//! Serializer backends (spec S-010 §5.3–5.5, `libobs/util/*-serializer.*`).
//!
//! The safe Rust implementations of the file input, file output (incl. the
//! "safe" temp-file variant) and the buffered file output serializer. The
//! in-memory array backend is implemented in the C-ABI bridge
//! (`obs_ffi::abi_serializer`) because its `DARRAY(u8)` storage is shared
//! with C code and must be allocated/freed through the C S-001 allocator
//! (the WP-23 invariant: anything C may `bfree` must come from the C
//! `bmalloc`).
//!
//! The buffered serializer is the **synchronous** implementation allowed by
//! S-010 §9: the observable contract is the file after `free` (all recorded
//! writes applied in call order) plus the logical position/seek rules. The
//! reference's background writer thread and its queue are an internal detail
//! (R46 informational); the virtual deque `size`/`capacity` are tracked with
//! the exact reference growth rules so the one `LOG_DEBUG` line per `free`
//! (R44) reports the same KiB value the reference would.
//!
//! Quirks are the spec: SEEK_END has three different conventions (Q-8 —
//! files add the offset to the file size, buffered subtracts it from the
//! current write position), buffered seek has no range check (Q-9 — a
//! negative START returns a negative value), and writes beyond the end zero
//! fill the hole.

use std::fs::File;
use std::io::{Read, Seek, SeekFrom, Write};

/// S-010 §3.1: `SERIALIZE_SEEK_START` (C `enum serialize_seek_type`).
pub const SERIALIZE_SEEK_START: i32 = 0;
/// S-010 §3.1: `SERIALIZE_SEEK_CURRENT`.
pub const SERIALIZE_SEEK_CURRENT: i32 = 1;
/// S-010 §3.1: `SERIALIZE_SEEK_END`.
pub const SERIALIZE_SEEK_END: i32 = 2;

/// C paths are byte strings; keep them verbatim on unix.
/// shortcut: narrow Windows C paths are locale-modelled; lossy UTF-8
/// is the pragmatic mapping (upgrade if a Windows vector uses non-UTF-8)
/// — the `text_lookup::read_file_text` pattern.
fn cpath(p: &[u8]) -> std::path::PathBuf {
    #[cfg(unix)]
    {
        let os = <std::ffi::OsStr as std::os::unix::ffi::OsStrExt>::from_bytes(p);
        std::path::Path::new(os).to_path_buf()
    }
    #[cfg(not(unix))]
    {
        std::path::PathBuf::from(String::from_utf8_lossy(p).into_owned())
    }
}

/// S-010-R22 (Q-8): seek with ordinary file semantics — END **adds** the
/// offset to the file size (so `-2` is two bytes before the end), positions
/// beyond the end are allowed, a negative resulting position fails with
/// `-1` and leaves the position unchanged.
fn file_seek(file: &mut File, offset: i64, kind: i32) -> i64 {
    match kind {
        SERIALIZE_SEEK_START => {
            if offset < 0 {
                // C: fseeko(f, offset, SEEK_SET) with a negative target fails.
                -1
            } else {
                match file.seek(SeekFrom::Start(offset as u64)) {
                    Ok(p) => p as i64,
                    Err(_) => -1,
                }
            }
        }
        SERIALIZE_SEEK_CURRENT => match file.seek(SeekFrom::Current(offset)) {
            Ok(p) => p as i64,
            Err(_) => -1,
        },
        SERIALIZE_SEEK_END => {
            let size = file.metadata().map(|m| m.len()).unwrap_or(0) as i128;
            let target = size + offset as i128;
            if target < 0 {
                -1
            } else {
                match file.seek(SeekFrom::Start(target as u64)) {
                    Ok(p) => p as i64,
                    Err(_) => -1,
                }
            }
        }
        _ => -1, // the C enum has exactly the three values above
    }
}

fn file_pos(file: &mut File) -> i64 {
    match file.stream_position() {
        Ok(p) => p as i64,
        Err(_) => -1,
    }
}

/// S-010-R20…R23: the file input serializer state. `init` opens `path` for
/// binary reading; failure returns `None` and leaves the caller's serializer
/// untouched (R20).
pub struct FileInput {
    file: File,
}

impl FileInput {
    /// R20: open for reading; `None` on failure (missing, unreadable).
    pub fn init(path: &[u8]) -> Option<Self> {
        match File::open(cpath(path)) {
            Ok(file) => Some(Self { file }),
            Err(_) => None,
        }
    }

    /// R21: the number of bytes actually read (C `fread` of 1-byte items):
    /// short counts at end of file, 0 at/after the end.
    pub fn read(&mut self, buf: &mut [u8]) -> usize {
        let mut n = 0;
        while n < buf.len() {
            match self.file.read(&mut buf[n..]) {
                Ok(0) | Err(_) => break,
                Ok(k) => n += k,
            }
        }
        n
    }

    /// R22: see [`file_seek`].
    pub fn seek(&mut self, offset: i64, kind: i32) -> i64 {
        file_seek(&mut self.file, offset, kind)
    }

    /// R22: the absolute position.
    pub fn pos(&mut self) -> i64 {
        file_pos(&mut self.file)
    }
}

/// S-010-R30…R34: the file output serializer state. `target`/`temp` are the
/// C `file_name`/`temp_name` (both `None` on a plain serializer — C zeroes
/// them with `bzalloc`).
pub struct FileOutput {
    file: File,
    target: Option<Vec<u8>>,
    temp: Option<Vec<u8>>,
}

impl FileOutput {
    /// R30: open `path` for binary writing, creating or truncating it.
    pub fn init(path: &[u8]) -> Option<Self> {
        match File::create(cpath(path)) {
            Ok(file) => Some(Self {
                file,
                target: None,
                temp: None,
            }),
            Err(_) => None,
        }
    }

    /// R33: NULL or empty `ext` fails. The temporary name is
    /// `path` + (`.` unless `ext` starts with `.`) + `ext` (the dot is
    /// added even when `path` already ends with one — C appends it purely
    /// from `ext`); the target is not touched until `free`.
    pub fn init_safe(path: &[u8], ext: &[u8]) -> Option<Self> {
        if ext.is_empty() {
            return None;
        }
        let mut temp = path.to_vec();
        if !ext.starts_with(b".") {
            temp.push(b'.');
        }
        temp.extend_from_slice(ext);
        match File::create(cpath(&temp)) {
            Ok(file) => Some(Self {
                file,
                target: Some(path.to_vec()),
                temp: Some(temp),
            }),
            Err(_) => None,
        }
    }

    /// R31: the number of bytes written (normally `size`). Holes created by
    /// seeking beyond the end and writing read back as zero bytes (R31).
    pub fn write(&mut self, buf: &[u8]) -> usize {
        let mut n = 0;
        while n < buf.len() {
            match self.file.write(&buf[n..]) {
                Ok(0) | Err(_) => break,
                Ok(k) => n += k,
            }
        }
        n
    }

    /// R31 (follows R22).
    pub fn seek(&mut self, offset: i64, kind: i32) -> i64 {
        file_seek(&mut self.file, offset, kind)
    }

    /// R31: the logical position (R32: the content is only guaranteed
    /// visible after `free`; this port is unbuffered, so position == file
    /// size, and the vector-visible behaviour is the same).
    pub fn pos(&mut self) -> i64 {
        file_pos(&mut self.file)
    }

    /// R34: close the file. On a safe serializer: delete `target` (ignore
    /// errors) and rename the temp file onto it (ignore errors); afterwards
    /// the temp name no longer exists and `target` holds the temp file's
    /// content (an empty file if nothing was written).
    pub fn free(self) {
        drop(self.file);
        if let (Some(target), Some(temp)) = (self.target, self.temp) {
            let _ = std::fs::remove_file(cpath(&target));
            let _ = std::fs::rename(cpath(&temp), cpath(&target));
        }
    }
}
const DEFAULT_CHUNK_SIZE: usize = 1024 * 1024; // 1 MiB (R40)
/// The 16 bytes of the reference's per-piece deque header (two u64: offset,
/// length) — tracked here for the virtual capacity (R47, R44).
const IO_HEADER_SIZE: usize = 16;
/// The reference deque starts at 1 MiB (`deque_reserve(&out->io.data, 1048576)`).
const START_CAPACITY: usize = 1024 * 1024;

/// S-010-R40…R46: the buffered file output serializer state (synchronous).
pub struct BufferedFileOutput {
    file: File,
    filename: Vec<u8>,
    /// R43: the logical write position (unsigned 64-bit, no range check —
    /// Q-9: a negative START wraps and reports a negative value).
    next_pos: u64,
    chunk_size: usize,
    /// Virtual reference-deque size/capacity (bytes, incl. the 16-byte
    /// per-piece header) for the R44 `Final buffer capacity` line.
    dq_size: usize,
    dq_capacity: usize,
    /// R45: a failed write stops the serializer (subsequent writes return 0,
    /// seek/get_pos return -1).
    failed: bool,
    /// The I/O error of the failed write (R45 log line); `None` for a
    /// short write without an `io::Error`.
    last_error: Option<std::io::Error>,
}

impl BufferedFileOutput {
    /// R40: open `path` "wb" (create/truncate); `max_bufsize` 0 → 256 MiB,
    /// `chunk_size` 0 → 1 MiB. The synchronous port never blocks on queue
    /// space (R41's back pressure is internal to the reference; R47's
    /// `max_bufsize` floor has no observable effect here).
    pub fn init(path: &[u8], _max_bufsize: u64, chunk_size: usize) -> Option<Self> {
        let file = match File::create(cpath(path)) {
            Ok(f) => f,
            Err(_) => return None,
        };
        Some(Self {
            file,
            filename: path.to_vec(),
            next_pos: 0,
            chunk_size: if chunk_size == 0 {
                DEFAULT_CHUNK_SIZE
            } else {
                chunk_size
            },
            dq_size: 0,
            dq_capacity: START_CAPACITY,
            failed: false,
            last_error: None,
        })
    }

    /// R41: record the write in pieces of at most `chunk_size`, each applied
    /// at the current `next_pos`, in call order. Returns `buf.len()` on
    /// success, 0 when empty or already failed, and the bytes applied
    /// before the failure when the write fails (the failed state is set —
    /// R45: the caller logs the reference's one `LOG_ERROR` line via
    /// [`last_error`]/[`filename`], and subsequent writes return 0).
    ///
    /// A failing position seek does not stop the serializer: the
    /// reference's writer ignores its `fseeko` result and writes at the
    /// file's current position (Q-9 territory — unspecified, not in
    /// vectors); only a failed/short file write stops it (R45).
    pub fn write(&mut self, buf: &[u8]) -> usize {
        if buf.is_empty() || self.failed {
            return 0;
        }
        let mut off = 0;
        while off < buf.len() {
            let piece = (buf.len() - off).min(self.chunk_size);
            // C: the writer's `os_fseeki64` result is ignored (Q-9).
            let _ = self.file.seek(SeekFrom::Start(self.next_pos));
            let mut n = 0;
            let mut err = None;
            while n < piece {
                match self.file.write(&buf[off + n..off + piece]) {
                    Ok(0) => break,
                    Ok(k) => n += k,
                    Err(e) => {
                        err = Some(e);
                        break;
                    }
                }
            }
            if n < piece {
                match err {
                    Some(e) => self.fail(e),
                    None => self.failed = true, // short write without an io::Error
                }
                return off + n; // the reference stops the writer
            }
            self.next_pos = self.next_pos.wrapping_add(piece as u64);
            // Virtual deque growth (R-D02): header + piece, doubling from 1 MiB.
            self.dq_size += IO_HEADER_SIZE + piece;
            if self.dq_size > self.dq_capacity {
                self.dq_capacity = (self.dq_capacity * 2).max(self.dq_size);
            }
            off += piece;
        }
        buf.len()
    }

    /// R45: the I/O error of the failed write, if any (a short write
    /// without an `io::Error` leaves this `None`).
    #[must_use]
    pub fn last_error(&self) -> Option<&std::io::Error> {
        self.last_error.as_ref()
    }

    fn fail(&mut self, err: std::io::Error) {
        self.failed = true;
        self.last_error = Some(err);
    }

    /// R42 (Q-8/Q-9): START sets, CURRENT adds, END **subtracts** the offset
    /// from the current write position. Unsigned 64-bit arithmetic, no range
    /// check; returns the new `next_pos` as `int64` (a negative target thus
    /// returns a negative value).
    pub fn seek(&mut self, offset: i64, kind: i32) -> i64 {
        if self.failed {
            return -1;
        }
        let new_pos = match kind {
            SERIALIZE_SEEK_START => offset as u64,
            SERIALIZE_SEEK_CURRENT => self.next_pos.wrapping_add(offset as u64),
            SERIALIZE_SEEK_END => self.next_pos.wrapping_sub(offset as u64),
            _ => return -1,
        };
        self.next_pos = new_pos;
        new_pos as i64
    }

    /// R43: the logical position (-1 after a failure).
    pub fn pos(&self) -> i64 {
        if self.failed {
            -1
        } else {
            self.next_pos as i64
        }
    }

    /// Whether a write failed (R45). The caller logs the error line once.
    #[must_use]
    pub fn failed(&self) -> bool {
        self.failed
    }

    /// The path (for the R45 error message).
    pub fn filename(&self) -> &[u8] {
        &self.filename
    }

    /// R44: close the file (everything is already applied — synchronous) and
    /// report the final virtual deque capacity in KiB for the one
    /// `LOG_DEBUG` line. The reference logs `capacity / 1024`.
    pub fn free(self) -> u64 {
        let kib = self.dq_capacity as u64 / 1024;
        drop(self.file);
        kib
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn tmp(name: &str) -> std::path::PathBuf {
        // Forward slashes on Windows (the cf test-harness pattern).
        let p = std::env::temp_dir().join("obs-serializer-test").join(name);
        let _ = std::fs::create_dir_all(p.parent().unwrap());
        p
    }

    /// The init APIs take C byte-string paths; the test paths are ASCII.
    fn pb(p: &std::path::Path) -> Vec<u8> {
        p.to_str().unwrap().as_bytes().to_vec()
    }

    /// The conformance `pattern(n, start)`: byte i = (start + i) * 31 + 7.
    fn pat(n: usize, start: u32) -> Vec<u8> {
        (0..n)
            .map(|i| (((start + i as u32) * 31 + 7) % 256) as u8)
            .collect()
    }

    fn hex(s: &str) -> Vec<u8> {
        (0..s.len() / 2)
            .map(|i| u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap())
            .collect()
    }

    #[test]
    fn r20_init_missing_fails() {
        // R20/R30: failure returns None and the serializer is unusable.
        let missing = tmp("r20-missing");
        assert!(FileInput::init(&pb(&missing)).is_none());
        assert!(FileOutput::init(&pb(&missing.join("nodir/x"))).is_none());
        assert!(BufferedFileOutput::init(&pb(&missing.join("nodir/y")), 0, 0).is_none());
    }

    #[test]
    fn r21_r22_file_input_vector() {
        // Vector file_input: 11-byte file 00..0a; short reads at EOF; END
        // adds the offset to the file size (Q-8); negative targets fail
        // without moving the position.
        let p = tmp("r21");
        std::fs::write(&p, hex("000102030405060708090a")).unwrap();
        let mut fin = FileInput::init(&pb(&p)).unwrap();
        let mut buf = [0u8; 16];
        assert_eq!(fin.pos(), 0);
        assert_eq!(fin.read(&mut buf[0..4]), 4);
        assert_eq!(&buf[0..4], &hex("00010203"));
        assert_eq!(fin.pos(), 4);
        assert_eq!(fin.seek(2, SERIALIZE_SEEK_CURRENT), 6);
        assert_eq!(fin.read(&mut buf[0..3]), 3);
        assert_eq!(&buf[0..3], &hex("060708"));
        assert_eq!(fin.pos(), 9);
        // END adds the offset to the file size (Q-8): 11 + (-2) = 9.
        assert_eq!(fin.seek(-2, SERIALIZE_SEEK_END), 9);
        assert_eq!(fin.read(&mut buf[0..10]), 2);
        assert_eq!(&buf[0..2], &hex("090a"));
        assert_eq!(fin.read(&mut buf), 0); // at the end
        assert_eq!(fin.pos(), 11);
        assert_eq!(fin.seek(0, SERIALIZE_SEEK_START), 0);
        assert_eq!(fin.read(&mut []), 0); // zero size
        assert_eq!(fin.read(&mut buf), 11); // the whole file
        assert_eq!(fin.read(&mut buf[0..1]), 0);
        assert_eq!(fin.seek(3, SERIALIZE_SEEK_START), 3);
        assert_eq!(fin.seek(-1, SERIALIZE_SEEK_CURRENT), 2);
        // END: 11 + 0, 11 + 5 (beyond the end: allowed).
        assert_eq!(fin.seek(0, SERIALIZE_SEEK_END), 11);
        assert_eq!(fin.seek(5, SERIALIZE_SEEK_END), 16);
        assert_eq!(fin.read(&mut buf[0..1]), 0); // past the end
        assert_eq!(fin.seek(20, SERIALIZE_SEEK_START), 20);
        assert_eq!(fin.read(&mut buf[0..1]), 0);
        // Negative resulting positions fail and leave the position unchanged.
        assert_eq!(fin.seek(-1, SERIALIZE_SEEK_START), -1);
        assert_eq!(fin.pos(), 20);
        assert_eq!(fin.seek(-100, SERIALIZE_SEEK_CURRENT), -1);
        assert_eq!(fin.pos(), 20);
        let _ = std::fs::remove_file(&p);

        // Empty file: reads 0, END 0 is the empty position.
        let e = tmp("r21-empty");
        std::fs::write(&e, []).unwrap();
        let mut fe = FileInput::init(&pb(&e)).unwrap();
        assert_eq!(fe.read(&mut buf), 0);
        assert_eq!(fe.pos(), 0);
        assert_eq!(fe.seek(0, SERIALIZE_SEEK_END), 0);
        let _ = std::fs::remove_file(&e);
    }

    #[test]
    fn q8_file_vs_buffered_seek_end() {
        // Q-8: files ADD the END offset to the file size, the buffered
        // writer SUBTRACTS it from the current write position.
        let p = tmp("q8-file");
        std::fs::write(&p, [0u8; 11]).unwrap();
        let mut fin = FileInput::init(&pb(&p)).unwrap();
        assert_eq!(fin.seek(-2, SERIALIZE_SEEK_END), 9); // 11 - 2
        assert_eq!(fin.seek(5, SERIALIZE_SEEK_END), 16); // beyond the end
        let _ = std::fs::remove_file(&p);

        let p2 = tmp("q8-buf");
        let mut b = BufferedFileOutput::init(&pb(&p2), 0, 0).unwrap();
        assert_eq!(b.write(&hex("0102030405060708")), 8); // next_pos 8
        assert_eq!(b.seek(1, SERIALIZE_SEEK_END), 7); // 8 - 1 (not the file size!)
        assert_eq!(b.seek(0, SERIALIZE_SEEK_END), 7);
        assert_eq!(b.seek(3, SERIALIZE_SEEK_CURRENT), 10);
        let _ = b.free();
        let _ = std::fs::remove_file(&p2);
    }

    #[test]
    fn r30_r31_file_output_vector() {
        // Vector file_output: truncate on open, seek then overwrite, holes
        // zero fill, get_pos is the logical position, content visible
        // after close.
        let p = tmp("r31");
        std::fs::write(&p, [0xffu8; 16]).unwrap();
        let mut out = FileOutput::init(&pb(&p)).unwrap();
        assert_eq!(out.pos(), 0);
        assert_eq!(out.write(&hex("01020304")), 4);
        assert_eq!(out.pos(), 4);
        assert_eq!(out.seek(2, SERIALIZE_SEEK_START), 2);
        assert_eq!(out.write(&hex("aa")), 1);
        assert_eq!(out.pos(), 3);
        // END adds the offset to the CURRENT file size (4 after the
        // overwrite): 4 + 0, 4 + 2, then back one.
        assert_eq!(out.seek(0, SERIALIZE_SEEK_END), 4);
        assert_eq!(out.seek(2, SERIALIZE_SEEK_END), 6);
        assert_eq!(out.seek(-1, SERIALIZE_SEEK_CURRENT), 5);
        assert_eq!(out.seek(-1, SERIALIZE_SEEK_START), -1);
        assert_eq!(out.pos(), 5);
        assert_eq!(out.seek(8, SERIALIZE_SEEK_START), 8);
        assert_eq!(out.write(&hex("bb")), 1);
        assert_eq!(out.pos(), 9);
        // Vector: serialize_r prefers the WRITE callback (Q-2) — it writes
        // the (zeroed) buffer, then cc lands after it.
        assert_eq!(out.write(&[0, 0]), 2);
        assert_eq!(out.pos(), 11);
        assert_eq!(out.write(&hex("cc")), 1);
        out.free();
        let data = std::fs::read(&p).unwrap();
        assert_eq!(data, hex("0102aa0400000000bb0000cc"));
        // Re-open truncates (R30).
        let out2 = FileOutput::init(&pb(&p)).unwrap();
        out2.free();
        assert!(std::fs::read(&p).unwrap().is_empty());
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn r33_r34_safe_temp_file() {
        // Vector file_output_safe: ext with/without leading '.', empty ext
        // fails, free replaces the target with the temp content.
        let dir = tmp("r34");
        std::fs::create_dir_all(&dir).unwrap();

        let a = dir.join("sf-a");
        std::fs::write(&a, b"abcd").unwrap();
        let mut out = FileOutput::init_safe(&pb(&a), b"tmp").unwrap();
        assert_eq!(std::fs::read(&a).unwrap(), b"abcd"); // untouched
        assert_eq!(std::fs::read(dir.join("sf-a.tmp")).unwrap(), b"");
        assert_eq!(out.write(&hex("99")), 1);
        assert_eq!(out.pos(), 1);
        out.free();
        assert_eq!(std::fs::read(&a).unwrap(), hex("99"));
        assert!(!dir.join("sf-a.tmp").exists()); // temp name gone

        // ext with a leading dot: no extra '.' is added.
        let b = dir.join("sf-b");
        let mut out = FileOutput::init_safe(&pb(&b), b".new").unwrap();
        assert_eq!(out.write(&hex("5566")), 2);
        out.free();
        assert_eq!(std::fs::read(&b).unwrap(), hex("5566"));
        assert!(!dir.join("sf-b.new").exists());

        // The dot is added purely from ext: a path that already ends in
        // '.' gets a second one (C appends from temp_ext alone).
        let c = dir.join("sf-dot.");
        let out = FileOutput::init_safe(&pb(&c), b"tmp").unwrap();
        assert!(dir.join("sf-dot..tmp").exists());
        out.free();
        assert!(!dir.join("sf-dot..tmp").exists());
        assert_eq!(std::fs::read(&c).unwrap(), b""); // empty temp content

        // empty ext fails; the target is untouched.
        let d = dir.join("sf-e");
        std::fs::write(&d, b"abcd").unwrap();
        assert!(FileOutput::init_safe(&pb(&d), b"").is_none());
        assert_eq!(std::fs::read(&d).unwrap(), b"abcd");
        // missing directory fails.
        assert!(FileOutput::init_safe(&pb(&dir.join("nodir/c")), b"tmp").is_none());
        // nothing written: the target becomes an empty file.
        let f = dir.join("sf-f");
        std::fs::write(&f, b"abcd").unwrap();
        FileOutput::init_safe(&pb(&f), b"x").unwrap().free();
        assert!(std::fs::read(&f).unwrap().is_empty());
        let _ = std::fs::remove_dir_all(&dir);
    }

    #[test]
    fn r40_r41_buffered_write_order_and_chunks() {
        // Vector buffered_seek bs-a + buffered_large bl-b: writes applied
        // in call order, holes zero fill, chunked writes, file after free.
        let p = tmp("r41");
        let mut b = BufferedFileOutput::init(&pb(&p), 0, 0).unwrap();
        // The exact vector buffered_seek bs-a sequence (hex bytes).
        assert_eq!(b.write(&hex("0102030405060708")), 8);
        assert_eq!(b.seek(2, SERIALIZE_SEEK_START), 2);
        assert_eq!(b.write(&hex("aa")), 1);
        assert_eq!(b.pos(), 3);
        assert_eq!(b.seek(1, SERIALIZE_SEEK_END), 2); // END subtracts from pos
        assert_eq!(b.seek(3, SERIALIZE_SEEK_CURRENT), 5);
        assert_eq!(b.seek(-4, SERIALIZE_SEEK_CURRENT), 1);
        assert_eq!(b.seek(0, SERIALIZE_SEEK_END), 1);
        assert_eq!(b.write(&hex("bb")), 1);
        assert_eq!(b.seek(12, SERIALIZE_SEEK_START), 12);
        assert_eq!(b.write(&hex("cc")), 1);
        assert_eq!(b.free(), 1024); // 1 MiB start capacity, no growth
        let data = std::fs::read(&p).unwrap();
        assert_eq!(
            data,
            hex("01bbaa040506070800000000cc"),
            "file after free must match the reference"
        );
        let _ = std::fs::remove_file(&p);

        // Chunked writes (bl-b shape: max_bufsize 1, chunk 1) —
        // chunk_size 1 forces one piece per byte; the file is identical.
        let p2 = tmp("r41-chunk");
        let mut b2 = BufferedFileOutput::init(&pb(&p2), 1, 1).unwrap();
        let bytes = pat(40, 0);
        assert_eq!(b2.write(&bytes), 40);
        assert_eq!(b2.seek(38, SERIALIZE_SEEK_START), 38);
        let bytes2 = pat(5, 9);
        assert_eq!(b2.write(&bytes2), 5);
        assert_eq!(b2.free(), 1024); // 45 pieces * 17 bytes = 765 < 1 MiB
        let data2 = std::fs::read(&p2).unwrap();
        let mut expect = bytes.clone();
        expect.resize(43, 0); // the write at 38 grows the file to 43
        for (i, &v) in bytes2.iter().enumerate() {
            expect[38 + i] = v;
        }
        assert_eq!(data2, expect);
        let _ = std::fs::remove_file(&p2);

        // A zero-size write returns 0; the 256 MiB / 1 MiB defaults do not
        // change behaviour (only the chunk size matters).
        let p3 = tmp("r41-default");
        let mut b3 = BufferedFileOutput::init(&pb(&p3), 0, 0).unwrap();
        assert_eq!(b3.write(b""), 0);
        assert_eq!(b3.pos(), 0);
        b3.free();
        let _ = std::fs::remove_file(&p3);
    }

    #[test]
    fn r42_buffered_seek_no_range_check() {
        // Vector buffered_seek bs-b: seeks far past the end; the file after
        // free is 101 bytes with a zero hole.
        let p = tmp("r42");
        let mut b = BufferedFileOutput::init(&pb(&p), 64, 4).unwrap();
        let bytes = pat(10, 0);
        assert_eq!(b.write(&bytes), 10);
        assert_eq!(b.seek(100, SERIALIZE_SEEK_START), 100);
        assert_eq!(b.write(&[0x77]), 1);
        assert_eq!(b.seek(3, SERIALIZE_SEEK_START), 3);
        let bytes2 = pat(3, 50);
        assert_eq!(b.write(&bytes2), 3);
        assert_eq!(b.seek(1000, SERIALIZE_SEEK_CURRENT), 1006);
        assert_eq!(b.seek(0, SERIALIZE_SEEK_START), 0);
        b.free();
        let data = std::fs::read(&p).unwrap();
        assert_eq!(data.len(), 101);
        assert_eq!(&data[0..3], b"\x07\x26\x45");
        assert_eq!(&data[3..6], &bytes2);
        assert_eq!(&data[6..10], &bytes[6..10]);
        assert_eq!(&data[10..100], vec![0u8; 90]);
        assert_eq!(data[100], 0x77);
        let _ = std::fs::remove_file(&p);
    }

    #[test]
    fn r43_get_pos_logical_and_q9_negative_start() {
        // Vector buffered_seek bs-c: a seek without a write leaves an empty
        // file; get_pos is the logical position. bs-d: a negative START
        // wraps u64 and reports -1 (Q-9), get_pos follows.
        let p = tmp("r43");
        let mut b = BufferedFileOutput::init(&pb(&p), 0, 0).unwrap();
        assert_eq!(b.pos(), 0);
        assert_eq!(b.seek(5, SERIALIZE_SEEK_START), 5);
        assert_eq!(b.pos(), 5);
        b.free();
        assert!(std::fs::read(&p).unwrap().is_empty());
        let _ = std::fs::remove_file(&p);

        let p2 = tmp("r43-neg");
        let mut b2 = BufferedFileOutput::init(&pb(&p2), 0, 0).unwrap();
        assert_eq!(b2.write(&hex("0102")), 2);
        assert_eq!(b2.seek(-1, SERIALIZE_SEEK_START), -1); // u64 wrap -> i64 -1
        assert_eq!(b2.pos(), -1);
        b2.free();
        let _ = std::fs::remove_file(&p2);
    }

    #[test]
    fn r44_virtual_capacity_growth() {
        // R44: the reported KiB is the (virtual) deque capacity. It starts
        // at 1 MiB and doubles when the recorded pieces (16-byte header
        // each) exceed it — 3 pieces of the default 1 MiB chunk need
        // 4 MiB -> 4096 KiB.
        let p = tmp("r44");
        let mut b = BufferedFileOutput::init(&pb(&p), 0, 0).unwrap();
        assert_eq!(b.write(&pat(3 * 1024 * 1024, 0)), 3 * 1024 * 1024);
        assert_eq!(b.free(), 4096);
        let data = std::fs::read(&p).unwrap();
        assert_eq!(data.len(), 3 * 1024 * 1024);
        let _ = std::fs::remove_file(&p);
    }
}
