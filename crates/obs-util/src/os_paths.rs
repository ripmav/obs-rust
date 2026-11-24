//! S-023 §5.6 (R75, R79–R81): config/program-data/absolute paths and
//! free disk space (nix reference: `platform-nix.c`).
//!
//! The buffer forms return the `snprintf` result — the length of the
//! *complete* text, also when truncated; `dst` may be NULL with `size` 0
//! to measure. The `_ptr` forms are documented per rule; Q-11:
//! `os_get_config_path_ptr(NULL or "")` ends with `/` (the buffer form
//! does not).
//!
//! The `HOME`-unset case (R80) is fatal in the reference
//! (`bcrash("Could not get $HOME\n")`); the port routes it through an
//! injectable fatal hook (default [`crate::bmem::bcrash`]; the `obs-ffi`
//! swap bridge installs the C `bcrash` shim so the installed crash
//! handler serves it, exactly like the C reference).

use std::ffi::CStr;
use std::os::raw::c_char;
use std::sync::Mutex;

/// S-023-R79: the `realpath` buffer of the C reference
/// (`char newpath[PATH_MAX]`, 4096 on Linux and macOS).
const PATH_MAX: usize = 4096;

/// R80's `bcrash("Could not get $HOME\n")` route (default:
/// [`crate::bmem::bcrash`]). The hook must not return (the C `bcrash`
/// aborts); a returning hook is treated as failed and falls through to
/// `bcrash`.
type FatalHook = dyn Fn(&str) + Send + Sync;

static FATAL_HOOK: Mutex<Option<Box<FatalHook>>> = Mutex::new(None);

/// Route R80's `bcrash("Could not get $HOME\n")` through `hook`
/// (default: [`crate::bmem::bcrash`]).
pub fn set_platform_fatal_hook(hook: impl Fn(&str) + Send + Sync + 'static) {
    *FATAL_HOOK.lock().unwrap() = Some(Box::new(hook));
}

fn fatal(msg: &str) -> ! {
    let guard = FATAL_HOOK.lock().unwrap();
    if let Some(hook) = guard.as_ref() {
        hook(msg);
    }
    crate::bmem::bcrash(msg);
}

/// Raw `getenv` bytes (`None` when unset; an empty value counts as set,
/// R80). Non-UTF-8 values are kept byte-for-byte (the C reference uses
/// the raw environment).
#[cfg(unix)]
fn getenv_bytes(var: &str) -> Option<Vec<u8>> {
    // The raw environment bytes (no UTF-8 assumption), like C `getenv`.
    std::env::var_os(var).map(|v| std::os::unix::ffi::OsStrExt::as_bytes(v.as_os_str()).to_vec())
}

#[cfg(not(unix))]
fn getenv_bytes(var: &str) -> Option<Vec<u8>> {
    std::env::var_os(var).map(|v| v.to_string_lossy().into_owned().into_bytes())
}

/// `snprintf(dst, size, "%s", text)` over byte strings: writes up to
/// `min(size, dst capacity) - 1` bytes plus the NUL (nothing when
/// `size == 0` or `dst` is None) and returns the full text length.
fn write_snprintf(dst: Option<&mut [c_char]>, size: usize, text: &[u8]) -> i32 {
    if let Some(dst) = dst {
        let cap = size.min(dst.len());
        if cap > 0 {
            let n = (cap - 1).min(text.len());
            // SAFETY: `text` is valid for `n` bytes; `dst` is valid for `cap` bytes with `n < cap`, so the NUL write fits.
            unsafe {
                std::ptr::copy_nonoverlapping(text.as_ptr(), dst.as_mut_ptr().cast::<u8>(), n);
                *dst.as_mut_ptr().add(n).cast::<u8>() = 0;
            }
        }
    }
    text.len() as i32
}

/// S-023-R80 `os_get_config_path(dst, size, name)`: `$XDG_CONFIG_HOME`
/// when set (an empty value counts as set), else `$HOME/.config`; a
/// non-NULL, non-empty `name` appends `/<name>`. If neither variable is
/// set the process terminates (R80, see the module docs). Returns the
/// `snprintf` result (the length of the complete text; `dst` may be
/// None to measure).
pub fn os_get_config_path(dst: Option<&mut [c_char]>, size: usize, name: Option<&CStr>) -> i32 {
    let name = name.filter(|n| !n.to_bytes().is_empty());
    let full: Vec<u8> = if let Some(mut xdg) = getenv_bytes("XDG_CONFIG_HOME") {
        if let Some(name) = name {
            xdg.push(b'/');
            xdg.extend_from_slice(name.to_bytes());
        }
        xdg
    } else if let Some(mut home) = getenv_bytes("HOME") {
        home.extend_from_slice(b"/.config");
        if let Some(name) = name {
            home.push(b'/');
            home.extend_from_slice(name.to_bytes());
        }
        home
    } else {
        fatal("Could not get $HOME\n");
    };
    write_snprintf(dst, size, &full)
}

/// S-023-R80/Q-11 `os_get_config_path_ptr(name)`: the same path, but
/// the separator is ALWAYS appended before `name` — a NULL or empty
/// `name` leaves the trailing slash (`/xdg/`, `/home/u/.config/`).
/// The bytes are NUL-free and NUL-terminated by the caller/bridge.
pub fn os_get_config_path_ptr(name: Option<&CStr>) -> Vec<u8> {
    if let Some(mut xdg) = getenv_bytes("XDG_CONFIG_HOME") {
        xdg.push(b'/');
        if let Some(name) = name {
            xdg.extend_from_slice(name.to_bytes());
        }
        xdg
    } else if let Some(mut home) = getenv_bytes("HOME") {
        home.extend_from_slice(b"/.config/");
        if let Some(name) = name {
            home.extend_from_slice(name.to_bytes());
        }
        home
    } else {
        fatal("Could not get $HOME\n");
    }
}

/// S-023-R81 `os_get_program_data_path(dst, size, name)` =
/// `snprintf(dst, size, "/usr/local/share/%s", name or "")` (same
/// return semantics as R80).
pub fn os_get_program_data_path(
    dst: Option<&mut [c_char]>,
    size: usize,
    name: Option<&CStr>,
) -> i32 {
    let mut full = b"/usr/local/share/".to_vec();
    if let Some(name) = name {
        full.extend_from_slice(name.to_bytes());
    }
    write_snprintf(dst, size, &full)
}

/// S-023-R81 `os_get_program_data_path_ptr(name)`: the same text as an
/// allocated (here: owned) copy.
pub fn os_get_program_data_path_ptr(name: Option<&CStr>) -> Vec<u8> {
    let mut full = b"/usr/local/share/".to_vec();
    if let Some(name) = name {
        full.extend_from_slice(name.to_bytes());
    }
    full
}

/// S-023-R79 `os_get_abs_path(path, abspath, size)`: `realpath` of the
/// path (missing file, empty path or a NULL path -> 0); the resolved
/// path is copied with `snprintf(abspath, min(size, PATH_MAX), "%s")`
/// and the return value is the length of the *full* resolved path (it
/// may be >= `size`, the buffer is then truncated and NUL terminated;
/// with `size == 0` nothing is written). Symbolic links are resolved,
/// `.`/`..` removed, no trailing slash.
pub fn os_get_abs_path(path: Option<&CStr>, abspath: Option<&mut [c_char]>, size: usize) -> usize {
    // C: `if (!abspath) return 0;` / `realpath(NULL, ...)` fails gracefully (glibc) -> 0
    let Some(abspath) = abspath else {
        return 0;
    };
    let Some(path) = path else {
        return 0;
    };
    // SAFETY: `newpath` is PATH_MAX bytes — the C reference's own buffer size; `realpath` writes at most that plus the NUL.
    let mut newpath = [0u8; PATH_MAX];
    // SAFETY: `path` is a valid NUL-terminated C string; `newpath` is valid for PATH_MAX bytes.
    let resolved = unsafe { libc::realpath(path.as_ptr(), newpath.as_mut_ptr().cast()) };
    if resolved.is_null() {
        return 0;
    }
    // SAFETY: `realpath` returned a pointer into `newpath`, NUL-terminated.
    let full = unsafe { CStr::from_ptr(resolved.cast()) };
    let bytes = full.to_bytes();
    let min_size = size.min(PATH_MAX);
    if min_size > 0 {
        let n = (min_size - 1).min(bytes.len());
        // SAFETY: `bytes` is valid for `n`; the NUL write is within `min_size <= abspath` capacity.
        unsafe {
            std::ptr::copy_nonoverlapping(bytes.as_ptr(), abspath.as_mut_ptr().cast::<u8>(), n);
            *abspath.as_mut_ptr().add(n).cast::<u8>() = 0;
        }
    }
    bytes.len()
}

/// S-023-R79/Q-15 `os_get_abs_path_ptr(path)`: `os_get_abs_path` over a
/// 512-byte buffer; `None` when it returned 0; a result of 512 bytes or
/// more is silently truncated to 511 (Q-15).
pub fn os_get_abs_path_ptr(path: Option<&CStr>) -> Option<Vec<u8>> {
    let mut raw = [0u8; 512];
    // SAFETY: `raw` is 512 bytes of valid `c_char` storage (`c_char` is a single-byte integer type on all supported targets).
    let buf: &mut [c_char] =
        unsafe { std::slice::from_raw_parts_mut(raw.as_mut_ptr().cast::<c_char>(), 512) };
    let rc = os_get_abs_path(path, Some(buf), 512);
    if rc == 0 {
        None
    } else {
        let n = rc.min(511);
        Some(raw[..n].to_vec())
    }
}

/// S-023-R75 `os_get_free_disk_space(dir)`: `statvfs`; 0 on failure,
/// else `f_frsize * f_bavail`.
pub fn os_get_free_disk_space(dir: Option<&CStr>) -> u64 {
    let Some(dir) = dir else {
        return 0;
    };
    // SAFETY: `info` is plain C storage filled by `statvfs` on success.
    let mut info: libc::statvfs = unsafe { std::mem::zeroed() };
    // SAFETY: `dir` is a valid NUL-terminated C string for the call.
    if unsafe { libc::statvfs(dir.as_ptr(), &mut info) } != 0 {
        return 0;
    }
    info.f_frsize as u64 * info.f_bavail as u64
}

#[cfg(test)]
mod tests {
    use super::*;

    /// R81: program data path — buffer and ptr forms, empty name
    /// (vector 26-config-paths).
    #[test]
    fn program_data_path() {
        let mut buf = [0i8 as c_char; 100];
        let rc = os_get_program_data_path(Some(&mut buf), 100, Some(&c("obs")));
        assert_eq!(rc, 20);
        assert_eq!(cstr(&buf), "/usr/local/share/obs");
        // truncation: the length is the full text's
        let mut buf = [0i8 as c_char; 10];
        let rc = os_get_program_data_path(Some(&mut buf), 10, Some(&c("obs")));
        assert_eq!(rc, 20);
        assert_eq!(cstr(&buf), "/usr/loca");
        // NULL name -> empty
        assert_eq!(os_get_program_data_path(None, 0, None), 17);
        assert_eq!(cstr(&buf[..9]), "/usr/loca");
        // the ptr form
        assert_eq!(
            os_get_program_data_path_ptr(Some(&c("obs"))),
            b"/usr/local/share/obs".to_vec()
        );
        assert_eq!(
            os_get_program_data_path_ptr(None),
            b"/usr/local/share/".to_vec()
        );
        assert_eq!(
            os_get_program_data_path_ptr(Some(&c(""))),
            b"/usr/local/share/".to_vec()
        );
    }

    /// R79: abs path — resolution, truncation, missing/NULL (vector
    /// 25-abs-path-cwd).
    #[test]
    #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
    fn abs_path() {
        let d = std::env::temp_dir().join("obs_rust_os_paths_abs");
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(d.join("sub")).unwrap();
        std::fs::write(d.join("sub/file.txt"), "F").unwrap();
        // SAFETY: test — valid paths for `symlink`.
        unsafe {
            let t = c(d.join("sub").to_str().unwrap());
            let l = c(d.join("link").to_str().unwrap());
            libc::symlink(t.as_ptr(), l.as_ptr());
        }
        // the resolved absolute path (temp dirs may contain links, e.g.
        // /tmp -> /private/tmp on macOS: compare against `realpath` of
        // the same path, not the literal)
        let mut real = [0u8; PATH_MAX];
        // SAFETY: valid NUL-terminated path and PATH_MAX buffer.
        let resolved = unsafe {
            let p = c(d.join("sub/file.txt").to_str().unwrap());
            libc::realpath(p.as_ptr(), real.as_mut_ptr().cast())
        };
        assert!(!resolved.is_null());
        // SAFETY: `resolved` points into `real` (just written).
        let want = unsafe { CStr::from_ptr(resolved.cast()) };
        let mut buf = [0i8 as c_char; PATH_MAX];
        let rc = os_get_abs_path(
            Some(&c(d.join("sub/file.txt").to_str().unwrap())),
            Some(&mut buf),
            PATH_MAX,
        );
        assert_eq!(rc, want.to_bytes().len());
        assert_eq!(cstr(&buf), want.to_str().unwrap());
        // symlinks are resolved
        let rc2 = os_get_abs_path(
            Some(&c(d.join("link/file.txt").to_str().unwrap())),
            Some(&mut buf),
            PATH_MAX,
        );
        assert_eq!(rc2, want.to_bytes().len());
        assert_eq!(cstr(&buf), want.to_str().unwrap());
        // missing / empty / NULL path -> 0
        assert_eq!(
            os_get_abs_path(
                Some(&c(d.join("nosuch").to_str().unwrap())),
                Some(&mut buf),
                PATH_MAX
            ),
            0
        );
        assert_eq!(os_get_abs_path(Some(&c("")), Some(&mut buf), PATH_MAX), 0);
        assert_eq!(os_get_abs_path(None, Some(&mut buf), PATH_MAX), 0);
        // NULL buffer -> 0 (measuring with a buffer)
        assert_eq!(
            os_get_abs_path(Some(&c(d.join("sub").to_str().unwrap())), None, PATH_MAX),
            0
        );
        // size 0: nothing written, full length returned
        let rc = os_get_abs_path(Some(&c(d.join("sub").to_str().unwrap())), Some(&mut buf), 0);
        assert!(rc > 0);
        // truncation: rc is the full length, the buffer holds the prefix
        let mut small = [99i8 as c_char; 5];
        let rc = os_get_abs_path(
            Some(&c(d.join("sub").to_str().unwrap())),
            Some(&mut small),
            5,
        );
        assert!(rc > 5);
        assert_eq!(small[4], 0, "NUL-terminated");
        // the ptr form (of `sub`: compare against `realpath` of `sub`)
        // SAFETY: valid NUL-terminated path and PATH_MAX buffer.
        let resolved_sub = unsafe {
            let p = c(d.join("sub").to_str().unwrap());
            libc::realpath(p.as_ptr(), real.as_mut_ptr().cast())
        };
        assert!(!resolved_sub.is_null());
        // SAFETY: `resolved_sub` points into `real` (just written).
        let want_sub = unsafe { CStr::from_ptr(resolved_sub.cast()) };
        let p = os_get_abs_path_ptr(Some(&c(d.join("sub").to_str().unwrap()))).unwrap();
        assert_eq!(p, want_sub.to_bytes().to_vec());
        assert!(os_get_abs_path_ptr(Some(&c(d.join("nosuch").to_str().unwrap()))).is_none());
        assert!(os_get_abs_path_ptr(None).is_none());
        std::fs::remove_dir_all(&d).unwrap();
    }

    /// R75: free disk space — nonzero on the temp dir, 0 on a missing
    /// path (vector 33-system-contracts prints only ok/zero flags).
    #[test]
    #[cfg_attr(miri, ignore)] // statvfs is a foreign call (Miri isolation)
    fn free_disk_space() {
        assert!(os_get_free_disk_space(Some(&c("/"))) > 0);
        assert_eq!(os_get_free_disk_space(Some(&c("/nonexistent-wp30"))), 0);
        assert_eq!(os_get_free_disk_space(None), 0);
    }

    /// R80: config path with XDG set (even empty) and the HOME
    /// fallback; Q-11 trailing slash on the ptr form. The env vars are
    // restored in a `finally`-like scope (Rust: a drop guard).
    #[test]
    fn config_path() {
        struct EnvGuard {
            xdg: Option<std::ffi::OsString>,
            home: Option<std::ffi::OsString>,
        }
        impl Drop for EnvGuard {
            fn drop(&mut self) {
                // SAFETY: test-only: no other thread reads `XDG_CONFIG_HOME`/`HOME` (the only readers are this test's own functions, running on the same thread).
                unsafe {
                    match &self.xdg {
                        Some(v) => std::env::set_var("XDG_CONFIG_HOME", v),
                        None => std::env::remove_var("XDG_CONFIG_HOME"),
                    }
                    match &self.home {
                        Some(v) => std::env::set_var("HOME", v),
                        None => std::env::remove_var("HOME"),
                    }
                }
            }
        }
        let _guard = EnvGuard {
            xdg: std::env::var_os("XDG_CONFIG_HOME"),
            home: std::env::var_os("HOME"),
        };
        // SAFETY: test-only env setup (see the Drop impl above).
        unsafe {
            std::env::set_var("XDG_CONFIG_HOME", "/xdg");
            std::env::set_var("HOME", "/hm/u");
        }
        let mut buf = [0i8 as c_char; 100];
        assert_eq!(os_get_config_path(Some(&mut buf), 100, Some(&c("obs"))), 8);
        assert_eq!(cstr(&buf), "/xdg/obs");
        assert_eq!(
            os_get_config_path(Some(&mut buf), 100, Some(&c("obs/plugins"))),
            16
        );
        assert_eq!(cstr(&buf), "/xdg/obs/plugins");
        // NULL / empty name: no "/name" part (the buffer form, Q-11)
        assert_eq!(os_get_config_path(Some(&mut buf), 100, None), 4);
        assert_eq!(cstr(&buf), "/xdg");
        assert_eq!(os_get_config_path(Some(&mut buf), 100, Some(&c(""))), 4);
        // measuring
        assert_eq!(os_get_config_path(None, 0, Some(&c("obs"))), 8);
        // truncation
        assert_eq!(os_get_config_path(Some(&mut buf), 5, Some(&c("obs"))), 8);
        assert_eq!(cstr(&buf), "/xdg");
        assert_eq!(os_get_config_path(Some(&mut buf), 1, Some(&c("obs"))), 8);
        assert_eq!(cstr(&buf), "");
        // the ptr form ALWAYS appends "/" before name (Q-11)
        assert_eq!(
            os_get_config_path_ptr(Some(&c("obs"))),
            b"/xdg/obs".to_vec()
        );
        assert_eq!(os_get_config_path_ptr(None), b"/xdg/".to_vec());
        assert_eq!(os_get_config_path_ptr(Some(&c(""))), b"/xdg/".to_vec());
        // an empty XDG value counts as set
        // SAFETY: test-only env setup (see the Drop impl above).
        unsafe { std::env::set_var("XDG_CONFIG_HOME", "") };
        assert_eq!(os_get_config_path(Some(&mut buf), 100, Some(&c("obs"))), 4);
        assert_eq!(cstr(&buf), "/obs");
        assert_eq!(os_get_config_path_ptr(Some(&c("obs"))), b"/obs".to_vec());
        // XDG unset: $HOME/.config
        // SAFETY: test-only env setup (see the Drop impl above).
        unsafe { std::env::remove_var("XDG_CONFIG_HOME") };
        assert_eq!(os_get_config_path(Some(&mut buf), 100, Some(&c("obs"))), 17);
        assert_eq!(cstr(&buf), "/hm/u/.config/obs");
        assert_eq!(os_get_config_path(Some(&mut buf), 100, None), 13);
        assert_eq!(cstr(&buf), "/hm/u/.config");
        assert_eq!(
            os_get_config_path_ptr(Some(&c("obs"))),
            b"/hm/u/.config/obs".to_vec()
        );
        assert_eq!(os_get_config_path_ptr(None), b"/hm/u/.config/".to_vec());
        // (HOME unset -> fatal: pinned by conformance vector 26's
        // `try config_path` lines.)
    }

    fn c(s: &str) -> std::ffi::CString {
        std::ffi::CString::new(s).unwrap()
    }

    fn cstr(buf: &[c_char]) -> &str {
        // SAFETY: the buffer is NUL-terminated by the functions under test (or the padding 0 bytes end it early).
        let s = unsafe { CStr::from_ptr(buf.as_ptr()) };
        s.to_str().unwrap()
    }
}
