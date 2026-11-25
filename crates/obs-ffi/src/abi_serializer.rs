//! P1-14.2 (WP-32): the C ABI for the serializer family
//! (`libobs/util/serializer.h`, `array-serializer.{c,h}`,
//! `file-serializer.{c,h}`, `buffered-file-serializer.{c,h}`, spec S-010).
//!
//! The `struct serializer` vtable is installed with these module-local
//! trampolines (C function pointers, not exported symbols). The file and
//! buffered backends wrap the Rust [`obs_util::serializer`] types boxed on
//! the Rust heap; `s->data` is the box pointer.
//!
//! The array backend is the exception: `struct array_output_data` holds a
//! `DARRAY(uint8_t)` whose storage C may `da_free` (WP-23 allocator
//! invariant), so it is manipulated in place in C memory through the C
//! allocator — the same pattern as the cf-lexer bridges' darray growth
//! ([`crate::abi_common`]).
//!
//! R44: `buffered_file_serializer_free` logs the one `LOG_DEBUG` "Final
//! buffer capacity: N KiB" line (N = the virtual reference-deque capacity
//! the synchronous port tracks). R45: a failed write logs the reference's
//! `LOG_ERROR` line once (the message detail is platform text — spec §11).

use crate::abi_common::{bfree, bmalloc, c_bytes};
use obs_util::serializer::{
    BufferedFileOutput, FileInput, FileOutput, SERIALIZE_SEEK_CURRENT, SERIALIZE_SEEK_END,
    SERIALIZE_SEEK_START,
};
use std::os::raw::{c_char, c_void};
use std::ptr;

/// `struct serializer` (S-010 §3.1): `data` @0, then the four callbacks at
/// 8/16/24/32. A NULL callback means "not supported".
#[repr(C)]
pub struct serializer {
    pub data: *mut c_void,
    pub read: Option<unsafe extern "C" fn(*mut c_void, *mut c_void, usize) -> usize>,
    pub write: Option<unsafe extern "C" fn(*mut c_void, *const c_void, usize) -> usize>,
    pub seek: Option<unsafe extern "C" fn(*mut c_void, i64, i32) -> i64>,
    pub get_pos: Option<unsafe extern "C" fn(*mut c_void) -> i64>,
}

/// `struct array_output_data` (S-010 §3.3): the `DARRAY(uint8_t)` field
/// view (`array` @0, `num` @8, `capacity` @16) + `cur_pos` @24.
#[repr(C)]
pub struct array_output_data {
    pub array: *mut u8,
    pub num: usize,
    pub capacity: usize,
    pub cur_pos: usize,
}

/* ========================================================================== */
/* Array serializer (state in C memory, C allocator)                           */

/// S-005-R-A02 over the C DARRAY: grow to at least `new_size` elements
/// (1-byte elements) through the C allocator, copying the old `capacity`
/// bytes (element_size * capacity, the C form).
fn array_ensure(out: &mut array_output_data, new_size: usize) {
    if new_size <= out.capacity {
        return;
    }
    let new_cap = if out.capacity == 0 {
        new_size
    } else {
        out.capacity * 2
    };
    let new_cap = new_cap.max(new_size);
    // SAFETY: C-ABI — S-001 allocation; the fresh block is valid for new_cap bytes.
    let ptr = unsafe { bmalloc(new_cap) };
    if !out.array.is_null() && out.capacity > 0 {
        // SAFETY: old block valid for capacity bytes; fresh block for new_cap ≥ capacity.
        unsafe {
            std::ptr::copy_nonoverlapping(out.array, ptr, out.capacity);
            bfree(out.array);
        };
    }
    out.array = ptr;
    out.capacity = new_cap;
}

/// S-010-R11: overwrite in place when `cur_pos < num` (growing the length
/// to `cur_pos + size` when needed), else append; `cur_pos += size`; always
/// returns `size`.
unsafe extern "C" fn array_write(data: *mut c_void, buf: *const c_void, size: usize) -> usize {
    // SAFETY: vtable contract — `data` is a live `struct array_output_data`.
    let out = unsafe { &mut *(data as *mut array_output_data) };
    if out.cur_pos < out.num {
        let new_size = out.cur_pos + size;
        if new_size > out.num {
            array_ensure(out, new_size);
            out.num = new_size;
        }
        if size > 0 {
            // SAFETY: `array` holds new_size ≥ cur_pos + size bytes (ensured above); `buf` valid for size.
            unsafe {
                std::ptr::copy_nonoverlapping(buf.cast::<u8>(), out.array.add(out.cur_pos), size)
            };
        }
        out.cur_pos += size;
    } else {
        // da_push_back_array (R-A07): resize to num + size, copy at the old
        // end. (Zero-filling the new tail like darray_resize is invisible —
        // the copy overwrites it.)
        let old = out.num;
        array_ensure(out, old + size);
        out.num = old + size;
        if size > 0 {
            // SAFETY: `array` holds old + size bytes (ensured above); `buf` valid for size.
            unsafe { std::ptr::copy_nonoverlapping(buf.cast::<u8>(), out.array.add(old), size) };
        }
        out.cur_pos = out.num;
    }
    size
}

/// S-010-R12 (Q-8): START = offset, CURRENT = cur_pos + offset, END =
/// num − offset (backwards from the end), unsigned 64-bit; a target
/// `> num` (any negative target via wrap) returns -1 and changes nothing.
unsafe extern "C" fn array_seek(data: *mut c_void, offset: i64, kind: i32) -> i64 {
    // SAFETY: vtable contract — `data` is a live `struct array_output_data`.
    let out = unsafe { &mut *(data as *mut array_output_data) };
    let new_pos: usize = match kind {
        SERIALIZE_SEEK_START => offset as usize, // C: int64 -> size_t, negative wraps
        SERIALIZE_SEEK_CURRENT => out.cur_pos.wrapping_add(offset as usize),
        SERIALIZE_SEEK_END => out.num.wrapping_sub(offset as usize),
        _ => 0, // C: the switch has no default; new_pos stays 0
    };
    if new_pos > out.num {
        return -1;
    }
    out.cur_pos = new_pos;
    new_pos as i64
}

/// S-010-R13 (Q-1): the data length, NOT `cur_pos`.
unsafe extern "C" fn array_get_pos(data: *mut c_void) -> i64 {
    // SAFETY: vtable contract — `data` is a live `struct array_output_data`.
    let out = unsafe { &*(data as *const array_output_data) };
    out.num as i64
}

/// `EXPORT void array_output_serializer_init(struct serializer *s, struct
/// array_output_data *data)` (S-010-R10).
///
/// # Safety
/// `s` and `data` must be live for the call; afterwards `s`'s callbacks
/// reference `data` for its lifetime.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn array_output_serializer_init(
    s: *mut serializer,
    data: *mut array_output_data,
) {
    // SAFETY: caller contract — both are live `repr(C)`-compatible structs;
    // whole-struct stores (C: two memsets + field assignments).
    unsafe {
        std::ptr::write(
            data,
            array_output_data {
                array: ptr::null_mut(),
                num: 0,
                capacity: 0,
                cur_pos: 0,
            },
        );
        std::ptr::write(
            s,
            serializer {
                data: data as *mut c_void,
                read: None,
                write: Some(array_write),
                seek: Some(array_seek),
                get_pos: Some(array_get_pos),
            },
        );
    }
}

/// `EXPORT void array_output_serializer_free(struct array_output_data
/// *data)` (S-010-R14): `da_free` — `bfree` the array, zero the three
/// DARRAY fields (`cur_pos` is untouched, as in C).
///
/// # Safety
/// `data` must point to a `struct array_output_data` whose `array` came
/// from this bridge's writes (C allocator) or is NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn array_output_serializer_free(data: *mut array_output_data) {
    // SAFETY: C-ABI — `bfree` accepts NULL; `array` is C-allocated (or NULL).
    unsafe { bfree((*data).array) };
    // SAFETY: caller contract — `data` is live for the field stores.
    unsafe {
        (*data).array = ptr::null_mut();
        (*data).num = 0;
        (*data).capacity = 0;
    };
}

/// `EXPORT void array_output_serializer_reset(struct array_output_data
/// *data)` (S-010-R14): `da_clear` + `cur_pos = 0`, storage kept.
///
/// # Safety
/// `data` must be live for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn array_output_serializer_reset(data: *mut array_output_data) {
    // SAFETY: caller contract — live struct; the stores are plain fields.
    unsafe {
        (*data).num = 0;
        (*data).cur_pos = 0;
    };
}

/* ========================================================================== */
/* File input / file output (state is a Rust Box, s->data is the box pointer)  */

unsafe extern "C" fn file_input_read(data: *mut c_void, buf: *mut c_void, size: usize) -> usize {
    // SAFETY: vtable contract — `data` is a live `Box<FileInput>`.
    let fin = unsafe { &mut *(data as *mut FileInput) };
    if buf.is_null() {
        // C `fread(NULL, 1, n, f)` is UB; the only vector-reachable shape
        // (serialize with a NULL buffer, at end of file) reads 0. Mirror the
        // safe half: nothing is read, the position is untouched.
        return 0;
    }
    // SAFETY: `buf` is valid for `size` bytes (the s_read guard, R01).
    let out = unsafe { std::slice::from_raw_parts_mut(buf.cast::<u8>(), size) };
    fin.read(out)
}

unsafe extern "C" fn file_input_seek(data: *mut c_void, offset: i64, kind: i32) -> i64 {
    // SAFETY: vtable contract — `data` is a live `Box<FileInput>`.
    let fin = unsafe { &mut *(data as *mut FileInput) };
    fin.seek(offset, kind)
}

unsafe extern "C" fn file_input_get_pos(data: *mut c_void) -> i64 {
    // SAFETY: vtable contract — `data` is a live `Box<FileInput>`.
    let fin = unsafe { &mut *(data as *mut FileInput) };
    fin.pos()
}

/// `EXPORT bool file_input_serializer_init(struct serializer *s, const char
/// *path)` (S-010-R20): failure returns false and leaves only `s->data`
/// NULL (C sets it from the failed `os_fopen`).
///
/// # Safety
/// `s` must be live; `path` a NUL-terminated string for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn file_input_serializer_init(
    s: *mut serializer,
    path: *const c_char,
) -> bool {
    match c_bytes(path).and_then(FileInput::init) {
        Some(fin) => {
            // SAFETY: fresh Box — the raw pointer is stored into `s->data` (caller-owned until the matching free).
            let data = Box::into_raw(Box::new(fin)) as *mut c_void;
            // SAFETY: caller contract — `s` is live; whole-struct store.
            unsafe {
                std::ptr::write(
                    s,
                    serializer {
                        data,
                        read: Some(file_input_read),
                        write: None,
                        seek: Some(file_input_seek),
                        get_pos: Some(file_input_get_pos),
                    },
                );
            };
            true
        }
        None => {
            // SAFETY: caller contract — `s` is live; only `data` is touched (C form).
            unsafe { (*s).data = ptr::null_mut() };
            false
        }
    }
}

/// `EXPORT void file_input_serializer_free(struct serializer *s)`
/// (S-010-R23): close if open; the serializer keeps the stale pointer
/// (C does not clear `s->data`).
///
/// # Safety
/// `s` must be live; a non-NULL `s->data` must come from
/// [`file_input_serializer_init`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn file_input_serializer_free(s: *mut serializer) {
    // SAFETY: caller contract — `s` is live; the pointer is re-boxed and
    // dropped exactly once (the matching init).
    if !unsafe { (*s).data }.is_null() {
        // SAFETY: caller contract — `s` is live; the pointer is re-boxed and dropped exactly once.
        unsafe {
            drop(Box::from_raw((*s).data as *mut FileInput));
        };
    }
}

unsafe extern "C" fn file_output_write(
    data: *mut c_void,
    buf: *const c_void,
    size: usize,
) -> usize {
    // SAFETY: vtable contract — `data` is a live `Box<FileOutput>`.
    let out = unsafe { &mut *(data as *mut FileOutput) };
    if buf.is_null() || size == 0 {
        return 0;
    }
    // SAFETY: `buf` is valid for `size` bytes (the s_write guard, R02).
    let bytes = unsafe { std::slice::from_raw_parts(buf.cast::<u8>(), size) };
    out.write(bytes)
}

unsafe extern "C" fn file_output_seek(data: *mut c_void, offset: i64, kind: i32) -> i64 {
    // SAFETY: vtable contract — `data` is a live `Box<FileOutput>`.
    let out = unsafe { &mut *(data as *mut FileOutput) };
    out.seek(offset, kind)
}

unsafe extern "C" fn file_output_get_pos(data: *mut c_void) -> i64 {
    // SAFETY: vtable contract — `data` is a live `Box<FileOutput>`.
    let out = unsafe { &mut *(data as *mut FileOutput) };
    out.pos()
}

/// `EXPORT bool file_output_serializer_init(struct serializer *s, const char
/// *path)` (S-010-R30).
///
/// # Safety
/// `s` must be live; `path` a NUL-terminated string for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn file_output_serializer_init(
    s: *mut serializer,
    path: *const c_char,
) -> bool {
    match c_bytes(path).and_then(FileOutput::init) {
        Some(out) => {
            // SAFETY: fresh Box — stored into `s->data` (caller-owned until the matching free).
            let data = Box::into_raw(Box::new(out)) as *mut c_void;
            // SAFETY: caller contract — `s` is live; whole-struct store.
            unsafe {
                std::ptr::write(
                    s,
                    serializer {
                        data,
                        read: None,
                        write: Some(file_output_write),
                        seek: Some(file_output_seek),
                        get_pos: Some(file_output_get_pos),
                    },
                );
            };
            true
        }
        None => {
            // SAFETY: caller contract — `s` is live; only `data` is touched (C form).
            unsafe { (*s).data = ptr::null_mut() };
            false
        }
    }
}

/// `EXPORT bool file_output_serializer_init_safe(struct serializer *s,
/// const char *path, const char *temp_ext)` (S-010-R33): NULL or empty
/// `temp_ext` returns false and leaves `s` untouched.
///
/// # Safety
/// `s` must be live; `path` a NUL-terminated string for the call; `temp_ext`
/// (or NULL) a NUL-terminated string for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn file_output_serializer_init_safe(
    s: *mut serializer,
    path: *const c_char,
    temp_ext: *const c_char,
) -> bool {
    // C: `if (!temp_ext || !*temp_ext) return false;` — `s` untouched.
    let Some(ext) = c_bytes(temp_ext) else {
        return false;
    };
    let Some(path) = c_bytes(path) else {
        return false;
    };
    match FileOutput::init_safe(path, ext) {
        Some(out) => {
            // SAFETY: fresh Box — stored into `s->data` (caller-owned until the matching free).
            let data = Box::into_raw(Box::new(out)) as *mut c_void;
            // SAFETY: caller contract — `s` is live; whole-struct store.
            unsafe {
                std::ptr::write(
                    s,
                    serializer {
                        data,
                        read: None,
                        write: Some(file_output_write),
                        seek: Some(file_output_seek),
                        get_pos: Some(file_output_get_pos),
                    },
                );
            };
            true
        }
        None => {
            // SAFETY: caller contract — `s` is live; only `data` is touched (C form).
            unsafe { (*s).data = ptr::null_mut() };
            false
        }
    }
}

/// `EXPORT void file_output_serializer_free(struct serializer *s)`
/// (S-010-R34): close; a safe serializer deletes the target and renames the
/// temp onto it. The stale-pointer form of R23 applies.
///
/// # Safety
/// `s` must be live; a non-NULL `s->data` must come from the output inits.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn file_output_serializer_free(s: *mut serializer) {
    // SAFETY: caller contract — `s` is live; re-boxed and dropped once.
    if !unsafe { (*s).data }.is_null() {
        // SAFETY: `s->data` came from `file_output_serializer_init` (checked non-NULL above); re-boxed and freed once.
        let out = unsafe { Box::from_raw((*s).data as *mut FileOutput) };
        out.free();
    }
}

/* ========================================================================== */
/* Buffered file output (synchronous — S-010 §9)                               */

unsafe extern "C" fn buffered_write(data: *mut c_void, buf: *const c_void, size: usize) -> usize {
    // SAFETY: vtable contract — `data` is a live `Box<BufferedFileOutput>`.
    let out = unsafe { &mut *(data as *mut BufferedFileOutput) };
    if size == 0 || out.failed() {
        return 0;
    }
    if buf.is_null() {
        // C: UB (the writer would read NULL); the s_write guard (R02) keeps
        // this unreachable in practice — treat as nothing written.
        return 0;
    }
    // SAFETY: `buf` is valid for `size` bytes (the s_write guard, R02).
    let bytes = unsafe { std::slice::from_raw_parts(buf.cast::<u8>(), size) };
    let n = out.write(bytes);
    // R45: the reference logs its one LOG_ERROR line when the writer fails.
    // The message detail is platform text (spec §11 — not vector-visible).
    if out.failed() {
        let msg = format!(
            "Error writing to '{}': {}\n",
            String::from_utf8_lossy(out.filename()),
            out.last_error()
                .map(|e| e.to_string())
                .unwrap_or_else(|| "short write".into()),
        );
        crate::blog(crate::LOG_ERROR, &msg);
    }
    n
}

unsafe extern "C" fn buffered_seek(data: *mut c_void, offset: i64, kind: i32) -> i64 {
    // SAFETY: vtable contract — `data` is a live `Box<BufferedFileOutput>`.
    let out = unsafe { &mut *(data as *mut BufferedFileOutput) };
    out.seek(offset, kind)
}

unsafe extern "C" fn buffered_get_pos(data: *mut c_void) -> i64 {
    // SAFETY: vtable contract — `data` is a live `Box<BufferedFileOutput>`.
    let out = unsafe { &mut *(data as *mut BufferedFileOutput) };
    out.pos()
}

/// `EXPORT bool buffered_file_serializer_init(struct serializer *s, const
/// char *path, size_t max_bufsize, size_t chunk_size)` (S-010-R40).
///
/// # Safety
/// `s` must be live; `path` a NUL-terminated string for the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn buffered_file_serializer_init(
    s: *mut serializer,
    path: *const c_char,
    max_bufsize: usize,
    chunk_size: usize,
) -> bool {
    match c_bytes(path).and_then(|p| BufferedFileOutput::init(p, max_bufsize as u64, chunk_size)) {
        Some(out) => {
            // SAFETY: fresh Box — stored into `s->data` (caller-owned until the matching free).
            let data = Box::into_raw(Box::new(out)) as *mut c_void;
            // SAFETY: caller contract — `s` is live; whole-struct store.
            unsafe {
                std::ptr::write(
                    s,
                    serializer {
                        data,
                        read: None,
                        write: Some(buffered_write),
                        seek: Some(buffered_seek),
                        get_pos: Some(buffered_get_pos),
                    },
                );
            };
            true
        }
        None => {
            // SAFETY: caller contract — `s` is live; only `data` is touched (C form).
            unsafe { (*s).data = ptr::null_mut() };
            false
        }
    }
}

/// `EXPORT bool buffered_file_serializer_init_defaults(struct serializer
/// *s, const char *path)` (S-010-R40: `init(s, path, 0, 0)`).
///
/// # Safety
/// As [`buffered_file_serializer_init`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn buffered_file_serializer_init_defaults(
    s: *mut serializer,
    path: *const c_char,
) -> bool {
    // SAFETY: forwarded contract.
    unsafe { buffered_file_serializer_init(s, path, 0, 0) }
}

/// `EXPORT void buffered_file_serializer_free(struct serializer *s)`
/// (S-010-R44): everything is already applied (synchronous port), the file
/// is closed, and the one `LOG_DEBUG` "Final buffer capacity: N KiB" line
/// is logged for an initialised serializer.
///
/// # Safety
/// `s` must be live; a non-NULL `s->data` must come from the buffered inits.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn buffered_file_serializer_free(s: *mut serializer) {
    // SAFETY: caller contract — `s` is live; re-boxed and freed once.
    if !unsafe { (*s).data }.is_null() {
        // SAFETY: `s->data` came from a buffered init (checked non-NULL above); re-boxed and freed once.
        let out = unsafe { Box::from_raw((*s).data as *mut BufferedFileOutput) };
        let kib = out.free();
        crate::blog(
            crate::LOG_DEBUG,
            &format!("Final buffer capacity: {kib} KiB"),
        );
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The buffered free closes the file and frees the state (R44); the
    /// one "Final buffer capacity" debug line is asserted byte-for-byte by
    /// the conformance vectors (they capture the C log handler).
    #[test]
    fn buffered_init_free_roundtrip() {
        let dir = std::env::temp_dir().join("obs-serial-bridge-test");
        std::fs::create_dir_all(&dir).unwrap();
        let p = dir.join("buf");
        let mut s = serializer {
            data: ptr::null_mut(),
            read: None,
            write: None,
            seek: None,
            get_pos: None,
        };
        let path = std::ffi::CString::new(p.to_str().unwrap()).unwrap();
        // SAFETY: `path` is a live C string for the call; `s` is live.
        let ok = unsafe { buffered_file_serializer_init_defaults(&mut s, path.as_ptr()) };
        assert!(ok);
        // Write through the vtable, then free (everything is synchronous).
        // SAFETY: `s` is initialised above.
        unsafe { s.write.unwrap()(s.data, b"abcd".as_ptr() as *const c_void, 4) };
        // SAFETY: as above.
        assert_eq!(unsafe { s.get_pos.unwrap()(s.data) }, 4);
        // SAFETY: `s` is initialised above.
        unsafe { buffered_file_serializer_free(&mut s) };
        assert_eq!(std::fs::read(&p).unwrap(), b"abcd");
        let _ = std::fs::remove_file(&p);
        let _ = std::fs::remove_dir_all(&dir);
    }

    /// The array backend through the C vtable (the cmocka
    /// `test_serializer` shape, extended to R11/R12/R13/R14).
    #[test]
    fn array_backend_c_shape() {
        let mut s = serializer {
            data: ptr::null_mut(),
            read: None,
            write: None,
            seek: None,
            get_pos: None,
        };
        let mut out = array_output_data {
            array: ptr::null_mut(),
            num: 0,
            capacity: 0,
            cur_pos: 0,
        };
        // SAFETY: both structs are live and stay live for the test.
        unsafe { array_output_serializer_init(&mut s, &mut out) };
        assert!(s.read.is_none());
        assert!(s.write.is_some() && s.seek.is_some() && s.get_pos.is_some());
        assert!(out.array.is_null());

        // write three bytes through the vtable (s_w8 shape: 1-byte writes)
        for b in [0x01u8, 0xff, 0xe1] {
            // SAFETY: `&mut out` is the live vtable state; `b` valid for 1 byte.
            let n = unsafe {
                s.write.unwrap()(
                    &mut out as *mut _ as *mut c_void,
                    &b as *const _ as *const c_void,
                    1,
                )
            };
            assert_eq!(n, 1);
        }
        assert_eq!(out.num, 3);
        // SAFETY: `out.array` holds 3 bytes (the writes above).
        let got = unsafe { std::slice::from_raw_parts(out.array, 3) };
        assert_eq!(got, &[0x01, 0xff, 0xe1]);
        // Q-1 (R13): get_pos is the length, not cur_pos.
        // SAFETY: vtable state is live.
        let pos = unsafe { s.get_pos.unwrap()(&mut out as *mut _ as *mut c_void) };
        assert_eq!(pos, 3);

        // R11: seek back and overwrite in place (no growth).
        // SAFETY: vtable state is live.
        let pos =
            unsafe { s.seek.unwrap()(&mut out as *mut _ as *mut c_void, 1, SERIALIZE_SEEK_START) };
        assert_eq!(pos, 1);
        // SAFETY: as above.
        unsafe {
            s.write.unwrap()(
                &mut out as *mut _ as *mut c_void,
                &0xaa as *const _ as *const c_void,
                1,
            )
        };
        assert_eq!(out.num, 3); // length unchanged
        // SAFETY: as above.
        let got = unsafe { std::slice::from_raw_parts(out.array, 3) };
        assert_eq!(got, &[0x01, 0xaa, 0xe1]);

        // R12 (Q-8): END subtracts from the length; > num fails unchanged.
        // SAFETY: vtable state is live.
        let pos =
            unsafe { s.seek.unwrap()(&mut out as *mut _ as *mut c_void, 1, SERIALIZE_SEEK_END) };
        assert_eq!(pos, 2);
        assert_eq!(out.cur_pos, 2);
        // SAFETY: as above.
        let pos =
            unsafe { s.seek.unwrap()(&mut out as *mut _ as *mut c_void, -1, SERIALIZE_SEEK_START) };
        assert_eq!(pos, -1);
        assert_eq!(out.cur_pos, 2); // unchanged
        // SAFETY: as above.
        let pos =
            unsafe { s.seek.unwrap()(&mut out as *mut _ as *mut c_void, 4, SERIALIZE_SEEK_START) };
        assert_eq!(pos, -1);

        // R14: reset keeps storage; free releases it (num 0 afterwards).
        // SAFETY: vtable state is live.
        unsafe { array_output_serializer_reset(&mut out) };
        assert_eq!((out.num, out.cur_pos), (0, 0));
        assert!(!out.array.is_null());
        // SAFETY: `out.array` is C-allocated by this bridge (or NULL).
        unsafe { array_output_serializer_free(&mut out) };
        assert_eq!((out.array.is_null(), out.num, out.capacity), (true, 0, 0));
    }
}
