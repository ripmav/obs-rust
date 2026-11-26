//! Platform file helpers (S-023 R61/R62, §5.5–5.6), used by
//! `config_save_safe` (S-007 R05) and the P1-07 recipe.
//!
//! - `os_file_exists` / `os_safe_replace` (R61/R62): cross-platform.
//! - P1-07.1 (R50/R51, R70–R73): `os_fopen`, `os_wfopen`, `os_mkdir`,
//!   `os_mkdirs`, `os_rename`, `os_unlink`, `os_copyfile` — nix
//!   implementations (the C reference for the ported functions is
//!   `platform.c`/`platform-nix.c`); the Windows variants (UTF-8↔
//!   UTF-16) are trailing P1-07.1 work like the other platform modules.
//!
//! Logging: the R76 `is_dir` debug line and the U4/U5 D-Bus/portal
//! lines (os_inhibit) go through an injectable sink (the dl.rs error
//! sink pattern — obs-util sits below the S-001 base logging layer;
//! the `obs-ffi` swap bridge routes it through the C `blog`).

#[cfg(unix)]
use std::ffi::CStr;
use std::fs;
#[cfg(unix)]
use std::os::raw::c_void;
use std::path::Path;
use std::sync::Mutex;

/// S-001 log levels (the C values); the sink receives these.
pub const LOG_ERROR: i32 = 100;
/// S-001 log level (the C value).
pub const LOG_WARNING: i32 = 200;
/// S-001 log level (the C value).
pub const LOG_DEBUG: i32 = 400;

type LogSink = dyn FnMut(i32, &str) + Send;

static PLATFORM_LOG_SINK: Mutex<Option<Box<LogSink>>> = Mutex::new(None);

/// Route platform-helper log lines (R76 `is_dir`, U4/U5 D-Bus/portal)
/// through `sink` instead of the default `eprintln!`.
pub fn set_platform_log_sink(sink: impl FnMut(i32, &str) + Send + 'static) {
    *PLATFORM_LOG_SINK.lock().unwrap() = Some(Box::new(sink));
}

/// Log a platform-helper line (the sink, or `eprintln!` with the C
/// default-handler prefix).
pub fn log_platform(level: i32, line: &str) {
    let mut guard = PLATFORM_LOG_SINK.lock().unwrap();
    if let Some(sink) = guard.as_mut() {
        sink(level, line);
    } else {
        eprintln!("{level}: {line}");
    }
}

/// S-023-R62 `os_file_exists(path)`: true iff the path can be accessed
/// (`access(path, F_OK)` — symbolic links are followed, so a dangling
/// link does not exist; directories count).
#[must_use]
pub fn os_file_exists(path: &Path) -> bool {
    fs::metadata(path).is_ok()
}

/// S-023-R70 `os_mkdir` result codes (the C `MKDIR_*` defines).
pub const MKDIR_SUCCESS: i32 = 0;
/// S-023-R70: the name already exists (`EEXIST`).
pub const MKDIR_EXISTS: i32 = 1;
/// S-023-R70: any other failure.
pub const MKDIR_ERROR: i32 = -1;

/// S-023-R50 `os_fopen(path, mode)`: the C library `fopen` on the raw
/// path bytes. `NULL` in C is unrepresentable here (the bridge returns
/// NULL for a NULL path without calling).
#[cfg(unix)]
pub fn os_fopen(path: &CStr, mode: &CStr) -> *mut c_void {
    // SAFETY: `path`/`mode` are valid NUL-terminated C strings for the call; `fopen` returns a valid `FILE *` or NULL. The pointer cast keeps the value in the C `FILE *` shape for the caller.
    unsafe { libc::fopen(path.as_ptr(), mode.as_ptr()) as *mut c_void }
}

/// S-023-R51 `os_wfopen(wpath, mode)`: the wide path is converted to
/// UTF-8 exactly like the C reference (`os_wcs_to_utf8_ptr`) and opened
/// with `fopen`. A path that fails conversion (a lone surrogate) yields
/// the empty string, so `fopen("")` fails and the result is NULL.
/// `wide` is the code points WITHOUT the terminating 0 (the bridge
/// scans the C string).
#[cfg(unix)]
pub fn os_wfopen(wide: &[u32], mode: &CStr) -> *mut c_void {
    let bytes = crate::utf8::os_wcs_to_utf8_ptr(wide, None);
    // SAFETY: `bytes` is NUL-terminated (the converter's invariant) and alive for the call; `mode` is a valid C string. The pointer cast keeps the value in the C `FILE *` shape for the caller; `.cast()` targets `c_char` portably (i8 on x86_64, u8 on aarch64).
    unsafe { libc::fopen(bytes.as_ptr().cast(), mode.as_ptr()) as *mut c_void }
}

/// S-023-R70 `os_mkdir(path)`: `mkdir(path, 0755)` (before umask);
/// `MKDIR_SUCCESS` if created, `MKDIR_EXISTS` on `EEXIST` (a directory,
/// a file, a link, even a dangling one), `MKDIR_ERROR` otherwise
/// (missing parent, parent not a directory, empty path, …). Backslashes
/// are ordinary name characters.
#[cfg(unix)]
pub fn os_mkdir(path: &CStr) -> i32 {
    // SAFETY: `path` is a valid NUL-terminated C string for the call.
    if unsafe { libc::mkdir(path.as_ptr(), 0o755) } == 0 {
        return MKDIR_SUCCESS;
    }
    // SAFETY: `mkdir` was called on this thread immediately before.
    let err = std::io::Error::last_os_error();
    if err.raw_os_error() == Some(libc::EEXIST) {
        MKDIR_EXISTS
    } else {
        MKDIR_ERROR
    }
}

/// S-023-R71 `os_mkdirs(path)`: every `\\` is replaced by `/` in a copy,
/// then the C `recursive_mkdir` recursion: `os_mkdir(p)`; if the result
/// is not `MKDIR_ERROR` return it; else, without a `/` return
/// `MKDIR_ERROR`; else recurse on the parent up to the last `/` and, if
/// the recursion did not fail, retry `os_mkdir(p)`. The result describes
/// only the final component. `"."`, `".."` and `"./"` return
/// `MKDIR_EXISTS`; the empty string is `MKDIR_ERROR` (S-023 D-4: the
/// reference crashes, the port must not).
#[cfg(unix)]
pub fn os_mkdirs(dir: &CStr) -> i32 {
    let mut bytes: Vec<u8> = dir
        .to_bytes()
        .iter()
        .map(|&b| if b == b'\\' { b'/' } else { b })
        .collect();
    // The C recursion works on the NUL-terminated buffer, mutating the
    // last-slash position in place.
    bytes.push(0);
    recursive_mkdir(&mut bytes)
}

#[cfg(unix)]
fn recursive_mkdir(path: &mut [u8]) -> i32 {
    // SAFETY: `path` is NUL-terminated for the whole recursion (invariant of this function and its caller); `.cast()` targets `c_char` portably (i8 on x86_64, u8 on aarch64).
    let c = unsafe { CStr::from_ptr(path.as_ptr().cast()) };
    let ret = os_mkdir(c);
    if ret != MKDIR_ERROR {
        return ret;
    }
    // `strrchr(path, '/')` over the NUL-terminated prefix.
    match c.to_bytes().iter().rposition(|&b| b == b'/') {
        None => MKDIR_ERROR,
        Some(i) => {
            let saved = path[i];
            path[i] = 0;
            let ret = recursive_mkdir(path);
            path[i] = saved;
            if ret == MKDIR_ERROR {
                return MKDIR_ERROR;
            }
            os_mkdir(c)
        }
    }
}

/// S-023-R72 `os_rename(old, new)` = `rename(2)`: 0 or -1. Replaces an
/// existing file target; directory/file cross and non-empty-directory
/// targets fail (-1); renaming onto itself succeeds.
#[cfg(unix)]
pub fn os_rename(old: &CStr, new: &CStr) -> i32 {
    // SAFETY: both are valid NUL-terminated C strings for the call.
    if unsafe { libc::rename(old.as_ptr(), new.as_ptr()) } == 0 {
        0
    } else {
        -1
    }
}

/// S-023-R72 `os_unlink(path)` = `unlink(2)` (files and links, not
/// directories): 0 or -1.
#[cfg(unix)]
pub fn os_unlink(path: &CStr) -> i32 {
    // SAFETY: `path` is a valid NUL-terminated C string for the call.
    if unsafe { libc::unlink(path.as_ptr()) } == 0 {
        0
    } else {
        -1
    }
}

/// S-023-R73 `os_copyfile(in, out)`: the exact C call sequence — if
/// `out` exists (links followed) return -1 untouched; open `in` binary
/// (failure -> -1, `out` not created); open `out` `ab+` (failure -> -1,
/// `in` closed); copy 4096-byte blocks (`fread`; write what was read;
/// loop while the block was full); 0 iff `feof(in)` else -1. A directory
/// as `in` returns -1 after creating an empty `out` (Q-10 — `fopen(dir,
/// "rb")` succeeds, the first `fread` fails). The new file is
/// `0666 & ~umask` (not copied from the source).
#[cfg(unix)]
pub fn os_copyfile(in_path: &CStr, out_path: &CStr) -> i32 {
    // The reference refuses when the DESTINATION already exists (Q-10).
    let out_bytes = out_path.to_bytes().to_vec();
    let out_os = <std::ffi::OsStr as std::os::unix::ffi::OsStrExt>::from_bytes(&out_bytes);
    if os_file_exists(std::path::Path::new(out_os)) {
        return -1;
    }
    // SAFETY: valid NUL-terminated paths and mode strings for the call; `fopen` returns a valid `FILE *` or NULL.
    let (file_in, file_out) = unsafe {
        let file_in = libc::fopen(in_path.as_ptr(), c"rb".as_ptr());
        if file_in.is_null() {
            return -1;
        }
        let file_out = libc::fopen(out_path.as_ptr(), c"ab+".as_ptr());
        (file_in, file_out)
    };
    if file_out.is_null() {
        // The C `error:` label: close what is open, return -1.
        // SAFETY: `file_in` is a live `FILE *` (created above).
        unsafe { libc::fclose(file_in) };
        return -1;
    }
    let mut data = [0u8; 4096];
    let mut size: usize;
    loop {
        // SAFETY: `file_in` is a live `FILE *`; `data` is a 4096-byte buffer.
        size = unsafe { libc::fread(data.as_mut_ptr().cast(), 1, data.len(), file_in) };
        if size != 0 {
            // SAFETY: `data` is valid for `size` bytes (just read).
            size = unsafe { libc::fwrite(data.as_ptr().cast(), 1, size, file_out) };
        }
        if size != data.len() {
            break;
        }
    }
    // SAFETY: `file_in` is a live `FILE *` for the call.
    let ret = if unsafe { libc::feof(file_in) } != 0 {
        0
    } else {
        -1
    };
    // SAFETY: both are live `FILE *` (created above, closed once).
    unsafe {
        libc::fclose(file_out);
        libc::fclose(file_in);
    }
    ret
}

/// S-023-R61 `os_safe_replace(target, from, backup)` (POSIX): if
/// `backup` is given and `target` exists (`os_file_exists`), rename
/// target -> backup (failure -> -1); then rename from -> target
/// (0 or -1). **No rollback**: when the second rename fails, the target
/// has already been moved to the backup (Q-9). An empty backup path is
/// used as a path (the rename fails -> -1 if the target exists).
/// `rename` replaces an existing file target atomically.
pub fn os_safe_replace(target: &Path, from: &Path, backup: Option<&Path>) -> i32 {
    if let Some(backup) = backup
        && os_file_exists(target)
        && fs::rename(target, backup).is_err()
    {
        return -1;
    }
    match fs::rename(from, target) {
        Ok(()) => 0,
        Err(_) => -1,
    }
}

/// The platform log sink is process-global: every test that installs
/// or asserts on it serialises on this lock (house pattern W9, like
/// `DL_TEST_LOCK` in dl.rs).
#[cfg(all(test, unix))]
pub(crate) static PLATFORM_SINK_TEST_LOCK: Mutex<()> = Mutex::new(());

#[cfg(test)]
mod tests {
    use super::*;

    fn dir(label: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("obs_rust_os_fs_{label}"));
        let _ = fs::remove_dir_all(&d);
        fs::create_dir_all(&d).unwrap();
        d
    }

    #[cfg(unix)]
    fn c(s: &str) -> std::ffi::CString {
        std::ffi::CString::new(s).unwrap()
    }

    #[test]
    #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
    fn file_exists() {
        let d = dir("exists");
        assert!(!os_file_exists(&d.join("nope")));
        fs::write(d.join("f"), "x").unwrap();
        assert!(os_file_exists(&d.join("f")));
        assert!(os_file_exists(&d), "directories count");
        // a dangling link does not exist (the link is followed)
        #[cfg(unix)]
        {
            std::os::unix::fs::symlink(d.join("nope"), d.join("link")).unwrap();
            assert!(!os_file_exists(&d.join("link")));
        }
        fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
    fn safe_replace_basic() {
        let d = dir("basic");
        let target = d.join("t.ini");
        let from = d.join("t.ini.tmp");
        let backup = d.join("t.ini.bak");
        fs::write(&target, "old").unwrap();
        fs::write(&from, "new").unwrap();
        // target exists -> it is moved to the backup, temp becomes target
        assert_eq!(os_safe_replace(&target, &from, Some(&backup)), 0);
        assert_eq!(fs::read_to_string(&target).unwrap(), "new");
        assert_eq!(fs::read_to_string(&backup).unwrap(), "old");
        assert!(!os_file_exists(&from), "the temp file no longer exists");
        fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
    fn safe_replace_no_existing_target() {
        let d = dir("new");
        let target = d.join("t.ini");
        let from = d.join("t.ini.tmp");
        let backup = d.join("t.ini.bak");
        fs::write(&from, "new").unwrap();
        // no target -> no backup rename at all
        assert_eq!(os_safe_replace(&target, &from, Some(&backup)), 0);
        assert_eq!(fs::read_to_string(&target).unwrap(), "new");
        assert!(!os_file_exists(&backup));
        // without a backup argument the same holds
        let from2 = d.join("t2.tmp");
        let target2 = d.join("t2");
        fs::write(&from2, "x").unwrap();
        assert_eq!(os_safe_replace(&target2, &from2, None), 0);
        assert_eq!(fs::read_to_string(&target2).unwrap(), "x");
        fs::remove_dir_all(&d).unwrap();
    }

    #[test]
    #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
    fn safe_replace_q9_no_rollback() {
        // Q-9: when the second rename fails, the target has already
        // been moved to the backup (no rollback). Make the source
        // missing so the second rename must fail.
        let d = dir("q9");
        let target = d.join("t");
        let from = d.join("f");
        let backup = d.join("b");
        fs::write(&target, "old").unwrap();
        assert_eq!(os_safe_replace(&target, &from, Some(&backup)), -1);
        assert_eq!(
            fs::read_to_string(&backup).unwrap(),
            "old",
            "target was already moved to backup"
        );
        assert!(!os_file_exists(&target));
        assert!(!os_file_exists(&from));
        fs::remove_dir_all(&d).unwrap();
    }

    #[cfg(unix)]
    mod wp30 {
        use super::*;

        /// P1-07.1: `os_fopen` (R50) — raw-byte `fopen`; missing file
        /// with `r` -> NULL; `wb` creates; a directory cannot be opened
        /// for writing (vectors 11-fopen-modes).
        #[test]
        #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
        fn fopen_modes() {
            let d = dir("fopen");
            let missing = d.join("missing.txt");
            assert!(
                os_fopen(&c(missing.to_str().unwrap()), &c("rb")).is_null(),
                "missing file with rb -> NULL"
            );
            let target = d.join("one.txt");
            let f = os_fopen(&c(target.to_str().unwrap()), &c("wb"));
            assert!(!f.is_null(), "wb creates");
            // SAFETY: `f` is a live `FILE *` (created above).
            unsafe { libc::fclose(f as *mut _) };
            assert!(target.exists());
            // a directory cannot be opened for writing
            assert!(os_fopen(&c(d.to_str().unwrap()), &c("wb")).is_null());
            // a missing parent component fails
            let f = os_fopen(&c(d.join("nodir/x.txt").to_str().unwrap()), &c("wb"));
            assert!(f.is_null());
            fs::remove_dir_all(&d).unwrap();
        }

        /// P1-07.1: `os_wfopen` (R51) — wide->UTF-8 conversion first;
        /// a lone surrogate converts to the empty string -> NULL
        /// (vector 11: `wfopen_cp 77,d800,2e,74,78,74` / `77,d800`).
        #[test]
        #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
        fn wfopen_converts() {
            let d = dir("wfopen");
            // the full path, as UTF-16 code units (no terminating 0)
            let wide: Vec<u32> = d
                .join("w\u{e9}.txt")
                .to_str()
                .unwrap()
                .encode_utf16()
                .map(u32::from)
                .collect();
            let f = super::os_wfopen(&wide, &c("wb"));
            assert!(!f.is_null());
            // SAFETY: `f` is a live `FILE *` (created above).
            unsafe { libc::fclose(f as *mut _) };
            // "w\u{e9}.txt" was created
            assert!(d.join("w\u{e9}.txt").exists());
            // surrogate -> empty string -> fopen("") fails
            let f = super::os_wfopen(&[0x77, 0xd800, 0x2e, 0x74, 0x78, 0x74], &c("rb"));
            assert!(f.is_null());
            let f = super::os_wfopen(&[0x77, 0xd800], &c("wb"));
            assert!(f.is_null());
            fs::remove_dir_all(&d).unwrap();
        }

        /// P1-07.1: `os_mkdir` (R70) — 0/1/-1 mapping incl. dangling
        /// links (vector 18-mkdir).
        #[test]
        #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
        fn mkdir_results() {
            let d = dir("mkdir");
            assert_eq!(os_mkdir(&c(d.join("a").to_str().unwrap())), MKDIR_SUCCESS);
            assert_eq!(os_mkdir(&c(d.join("a").to_str().unwrap())), MKDIR_EXISTS);
            std::fs::write(d.join("file.txt"), "x").unwrap();
            assert_eq!(
                os_mkdir(&c(d.join("file.txt").to_str().unwrap())),
                MKDIR_EXISTS
            );
            assert_eq!(
                os_mkdir(&c(d.join("missing/child").to_str().unwrap())),
                MKDIR_ERROR
            );
            assert_eq!(
                os_mkdir(&c(d.join("file.txt/child").to_str().unwrap())),
                MKDIR_ERROR
            );
            // trailing slash is accepted; a backslash is an ordinary character
            assert_eq!(
                os_mkdir(&c(d.join("a/b/").to_str().unwrap())),
                MKDIR_SUCCESS
            );
            assert_eq!(
                os_mkdir(&c(d.join("back\\slash").to_str().unwrap())),
                MKDIR_SUCCESS
            );
            assert!(d.join("back\\slash").is_dir());
            // the empty path fails (not EEXIST)
            assert_eq!(os_mkdir(&c("")), MKDIR_ERROR);
            // `..` and `.` forms
            assert_eq!(
                os_mkdir(&c(d.join("a/../c").to_str().unwrap())),
                MKDIR_SUCCESS
            );
            assert_eq!(os_mkdir(&c(d.join("./d").to_str().unwrap())), MKDIR_SUCCESS);
            assert_eq!(os_mkdir(&c(d.join("./d").to_str().unwrap())), MKDIR_EXISTS);
            // links: to an existing dir and dangling both are EEXIST
            #[cfg(unix)]
            {
                // SAFETY: test — valid paths for `symlink`.
                unsafe {
                    let t = c(d.join("a").to_str().unwrap());
                    let l = c(d.join("lnk").to_str().unwrap());
                    libc::symlink(t.as_ptr(), l.as_ptr());
                }
                assert_eq!(os_mkdir(&c(d.join("lnk").to_str().unwrap())), MKDIR_EXISTS);
            }
            fs::remove_dir_all(&d).unwrap();
        }

        /// P1-07.1: `os_mkdirs` (R71) — backslash conversion, recursion,
        /// final-component-only result, D-4 empty path (vector 19-mkdirs).
        #[test]
        #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
        fn mkdirs() {
            let d = dir("mkdirs");
            let p = |s: &str| c(d.join(s).to_str().unwrap());
            assert_eq!(os_mkdirs(&p("x")), MKDIR_SUCCESS);
            assert_eq!(os_mkdirs(&p("x")), MKDIR_EXISTS);
            assert_eq!(os_mkdirs(&p("p/q/r/s")), MKDIR_SUCCESS);
            assert!(d.join("p/q/r/s").is_dir());
            assert_eq!(os_mkdirs(&p("p/q/r/s")), MKDIR_EXISTS);
            assert_eq!(os_mkdirs(&p("u/v/")), MKDIR_EXISTS);
            assert_eq!(os_mkdirs(&p("w//x///y")), MKDIR_SUCCESS);
            assert_eq!(os_mkdirs(&p("./z/./k")), MKDIR_SUCCESS);
            // backslashes are separators here (unlike os_mkdir)
            assert_eq!(os_mkdirs(&p(r"b1\b2\b3")), MKDIR_SUCCESS);
            assert!(d.join("b1/b2/b3").is_dir());
            assert_eq!(os_mkdirs(&p(r"m1/m2\m3")), MKDIR_SUCCESS);
            // a regular file in the way: itself -> exists, under it -> -1
            std::fs::write(d.join("f.txt"), "x").unwrap();
            assert_eq!(os_mkdirs(&p("f.txt")), MKDIR_EXISTS);
            assert_eq!(os_mkdirs(&p("f.txt/sub")), MKDIR_ERROR);
            assert_eq!(os_mkdirs(&p("f.txt/sub/deeper")), MKDIR_ERROR);
            assert!(!d.join("f.txt/sub").exists());
            // `.`/`..`/`./` -> 1
            assert_eq!(os_mkdirs(&c(".")), MKDIR_EXISTS);
            assert_eq!(os_mkdirs(&c("..")), MKDIR_EXISTS);
            assert_eq!(os_mkdirs(&c("./")), MKDIR_EXISTS);
            // D-4: the empty path must not crash (vector 37)
            assert_eq!(os_mkdirs(&c("")), MKDIR_ERROR);
            fs::remove_dir_all(&d).unwrap();
        }

        /// P1-07.1: `os_rename` (R72) — POSIX rename semantics
        /// (vector 20-rename-unlink-rmdir).
        #[test]
        #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
        fn rename() {
            let d = dir("rename");
            let p = |s: &str| c(d.join(s).to_str().unwrap());
            std::fs::write(d.join("a.txt"), "A").unwrap();
            assert_eq!(os_rename(&p("a.txt"), &p("c.txt")), 0);
            assert_eq!(std::fs::read_to_string(d.join("c.txt")).unwrap(), "A");
            assert!(!d.join("a.txt").exists());
            std::fs::write(d.join("b.txt"), "B").unwrap();
            // replaces an existing file target
            assert_eq!(os_rename(&p("c.txt"), &p("b.txt")), 0);
            assert_eq!(std::fs::read_to_string(d.join("b.txt")).unwrap(), "A");
            assert_eq!(os_rename(&p("missing.txt"), &p("x.txt")), -1);
            assert_eq!(os_rename(&p("b.txt"), &p("nodir/b.txt")), -1);
            // directories: onto empty dir replaces, onto non-empty fails,
            // file onto dir fails
            std::fs::create_dir(d.join("d1")).unwrap();
            std::fs::create_dir(d.join("d2")).unwrap();
            std::fs::write(d.join("d1/in"), "X").unwrap();
            assert_eq!(os_rename(&p("d1"), &p("d3")), 0);
            assert_eq!(os_rename(&p("d3"), &p("b.txt")), -1, "dir onto file");
            assert_eq!(os_rename(&p("b.txt"), &p("d2")), -1, "file onto dir");
            std::fs::write(d.join("d2/x"), "x").unwrap();
            assert_eq!(os_rename(&p("d3"), &p("d2")), -1, "dir onto non-empty dir");
            // onto an empty directory replaces it
            std::fs::create_dir(d.join("e1")).unwrap();
            std::fs::remove_file(d.join("d2/x")).unwrap();
            std::fs::remove_dir(d.join("d2")).unwrap();
            assert_eq!(os_rename(&p("e1"), &p("d2")), 0);
            // renaming onto itself succeeds
            assert_eq!(os_rename(&p("b.txt"), &p("b.txt")), 0);
            fs::remove_dir_all(&d).unwrap();
        }

        /// P1-07.1: `os_unlink` (R72) — files and links, not directories
        /// (vector 20).
        #[test]
        #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
        fn unlink() {
            let d = dir("unlink");
            let p = |s: &str| c(d.join(s).to_str().unwrap());
            std::fs::write(d.join("s.txt"), "S").unwrap();
            assert_eq!(os_unlink(&p("s.txt")), 0);
            assert_eq!(os_unlink(&p("s.txt")), -1);
            std::fs::create_dir(d.join("ud")).unwrap();
            assert_eq!(os_unlink(&p("ud")), -1, "unlink of a directory fails");
            // SAFETY: test — valid paths for `symlink`.
            unsafe {
                let t = c(d.join("ud").to_str().unwrap());
                let l = c(d.join("lnk").to_str().unwrap());
                libc::symlink(t.as_ptr(), l.as_ptr());
                let m = c(d.join("nowhere").to_str().unwrap());
                let l2 = c(d.join("dangle").to_str().unwrap());
                libc::symlink(m.as_ptr(), l2.as_ptr());
            }
            assert_eq!(os_unlink(&p("lnk")), 0, "links are unlinked");
            assert!(d.join("ud").is_dir(), "the target survives");
            assert_eq!(os_unlink(&p("dangle")), 0, "dangling links are unlinked");
            fs::remove_dir_all(&d).unwrap();
        }

        /// P1-07.1: `os_copyfile` (R73, Q-10) — 4096-byte chunks, refuse
        /// to overwrite, directory source leaves an empty destination
        /// (vector 21-copyfile).
        #[test]
        #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
        fn copyfile() {
            let d = dir("copyfile");
            let p = |s: &str| c(d.join(s).to_str().unwrap());
            for n in [0usize, 1, 4095, 4096, 4097, 8192, 10000] {
                let src = d.join(format!("s{n}"));
                std::fs::write(
                    &src,
                    (0..n)
                        .map(|i| ((i * 7 + 3) & 0xff) as u8)
                        .collect::<Vec<u8>>(),
                )
                .unwrap();
                assert_eq!(
                    os_copyfile(&p(format!("s{n}").as_str()), &p(format!("d{n}").as_str())),
                    0
                );
                assert_eq!(
                    std::fs::read(d.join(format!("d{n}"))).unwrap(),
                    std::fs::read(&src).unwrap()
                );
            }
            // destination exists (even empty): -1, untouched
            std::fs::write(d.join("keep.txt"), "precious").unwrap();
            assert_eq!(os_copyfile(&p("s1"), &p("keep.txt")), -1);
            assert_eq!(
                std::fs::read_to_string(d.join("keep.txt")).unwrap(),
                "precious"
            );
            assert_eq!(os_copyfile(&p("s1"), &p("d0")), -1);
            // source missing: -1, no destination created
            assert_eq!(os_copyfile(&p("nosuch.bin"), &p("out.bin")), -1);
            assert!(!d.join("out.bin").exists());
            // destination directory missing
            assert_eq!(os_copyfile(&p("s1"), &p("nodir/out")), -1);
            // destination is an existing directory -> out exists -> -1
            std::fs::create_dir(d.join("dd")).unwrap();
            assert_eq!(os_copyfile(&p("s1"), &p("dd")), -1);
            // Q-10: a directory source returns -1 and leaves an EMPTY
            // destination (fopen(dir, "rb") succeeds, fread fails)
            std::fs::create_dir(d.join("srcdir")).unwrap();
            assert_eq!(os_copyfile(&p("srcdir"), &p("fromdir")), -1);
            assert_eq!(std::fs::metadata(d.join("fromdir")).unwrap().len(), 0);
            // a symbolic link as source is followed
            // SAFETY: test — valid paths for `symlink`.
            unsafe {
                let t = c(d.join("s1").to_str().unwrap());
                let l = c(d.join("lnk").to_str().unwrap());
                libc::symlink(t.as_ptr(), l.as_ptr());
            }
            assert_eq!(os_copyfile(&p("lnk"), &p("viaLink")), 0);
            assert_eq!(
                std::fs::read(d.join("viaLink")).unwrap(),
                std::fs::read(d.join("s1")).unwrap()
            );
            fs::remove_dir_all(&d).unwrap();
        }

        /// P1-07.1: the log sink routes platform log lines (shared by
        /// os_dir R76 and os_inhibit U4/U5).
        #[test]
        fn log_sink() {
            use std::sync::mpsc;
            let _lock = super::PLATFORM_SINK_TEST_LOCK.lock().unwrap();
            let (tx, rx) = mpsc::channel();
            set_platform_log_sink(move |level, line| {
                let _ = tx.send((level, line.to_string()));
            });
            log_platform(LOG_DEBUG, "is_dir: stat for x failed, errno: 2");
            let (level, line) = rx.recv().unwrap();
            assert_eq!(
                (level, line.as_str()),
                (LOG_DEBUG, "is_dir: stat for x failed, errno: 2")
            );
            // the sink must not be poisoned by concurrent readers: a
            // second call still works (dl.rs pattern)
            log_platform(LOG_ERROR, "other");
            assert_eq!(rx.recv().unwrap().0, LOG_ERROR);
        }
    }
}
