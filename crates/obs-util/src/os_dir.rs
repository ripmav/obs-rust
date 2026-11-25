//! S-023 §5.6 (R76–R77): directory iteration (`os_opendir` /
//! `os_readdir` / `os_closedir`) and `os_glob` / `os_globfree` (POSIX
//! `glob(3)`, flags ignored — the reference passes 0).
//!
//! **D-5 (deliberate defect break):** the C reference keeps the
//! *caller's* `path` pointer in the handle and re-reads it in every
//! `os_readdir` until `os_closedir` — passing a temporary string that
//! is freed in between is a use-after-free. The port **copies** the
//! path into the handle (R76); the regression proof is the C-ABI test
//! `d5_opendir_copies_path` in `obs-ffi` (the source buffer is freed
//! before `os_readdir`), which the reference cannot pass.
//!
//! `OsDirent` / `OsGlobEnt` / `OsGlobInfo` are `#[repr(C)]` mirrors of
//! `struct os_dirent` / `os_globent` / `os_glob_info` (the conformance
//! runner reads the fields directly). The handles themselves are opaque
//! in C (`typedef struct os_dir os_dir_t;`), so their memory is
//! Rust-owned.

use std::ffi::{CStr, CString};
use std::os::raw::c_char;

use crate::os_fs::{LOG_DEBUG, log_platform};

/// C `struct os_dirent`: a 256-byte name plus the directory flag
/// (R76: `d_name` is the entry name, not a path; `directory` is true iff
/// `stat(path + "/" + name)` succeeds and is a directory, links
/// followed).
#[repr(C)]
#[derive(Debug)]
pub struct OsDirent {
    pub d_name: [c_char; 256],
    pub directory: bool,
}

/// `os_dir_t`: owns a copy of the caller's path (D-5), the OS directory
/// stream, and the `out` entry that [`os_readdir`] returns a pointer
/// to (valid until the next call, as in C).
#[derive(Debug)]
pub struct OsDir {
    /// D-5: a COPY of the caller's path bytes (the C reference keeps the
    /// pointer — a UAF on a temporary).
    path: Vec<u8>,
    dir: *mut libc::DIR,
    out: OsDirent,
}

impl OsDir {
    fn new(path: &CStr, dir: *mut libc::DIR) -> Self {
        Self {
            path: path.to_bytes().to_vec(),
            dir,
            out: OsDirent {
                d_name: [0; 256],
                directory: false,
            },
        }
    }
}

impl Drop for OsDir {
    fn drop(&mut self) {
        // SAFETY: `dir` is a live `DIR *` from `opendir` (or already closed); double close is impossible — one handle, one drop.
        unsafe { libc::closedir(self.dir) };
    }
}

/// S-023-R76 `os_opendir(path)`: `opendir`; `None` when the directory
/// cannot be opened (missing, not a directory). The path is copied
/// (D-5 — see the module docs).
pub fn os_opendir(path: &CStr) -> Option<OsDir> {
    // SAFETY: `path` is a valid NUL-terminated C string for the call; `opendir` returns a valid `DIR *` or NULL.
    let dir = unsafe { libc::opendir(path.as_ptr()) };
    if dir.is_null() {
        return None;
    }
    Some(OsDir::new(path, dir))
}

/// S-023-R76 `os_readdir(dir)`: the next entry in file-system order
/// (unspecified — callers must not rely on it), INCLUDING `.` and `..`,
/// or `None` after the last entry (and on every later call). Names of
/// 256 bytes or longer are skipped (`None` — cannot happen on Linux
/// file systems). The result points into the handle and is valid until
/// the next call.
pub fn os_readdir(dir: &mut OsDir) -> Option<&OsDirent> {
    // SAFETY: `dir.dir` is a live `DIR *` for the call; `readdir` returns a valid `dirent *` (valid until the next call) or NULL.
    let e = unsafe { libc::readdir(dir.dir) };
    if e.is_null() {
        return None;
    }
    let gd = e as *mut libc::dirent;
    // SAFETY: `e` is a live platform `dirent` (libc's per-target definition); `d_name` is NUL-terminated and copied below before return.
    let name = unsafe { CStr::from_ptr((*gd).d_name.as_ptr()) };
    let name_bytes = name.to_bytes();
    if name_bytes.len() >= 256 {
        return None;
    }
    let out = &mut dir.out;
    // SAFETY: `name_bytes` is valid for the copy; `d_name` is a 256-byte array, the copy fits (checked above), the NUL write is at index < 256.
    unsafe {
        std::ptr::copy_nonoverlapping(
            name_bytes.as_ptr(),
            out.d_name.as_mut_ptr().cast::<u8>(),
            name_bytes.len(),
        );
        *out.d_name.as_mut_ptr().add(name_bytes.len()).cast::<u8>() = 0;
    }
    // R76: `directory` = stat(path + "/" + name), links followed.
    let mut full = dir.path.clone();
    full.push(b'/');
    full.extend_from_slice(name_bytes);
    if let Ok(cfull) = CString::new(full) {
        out.directory = is_dir(&cfull);
    } else {
        // The C dstr copy cannot contain an interior NUL (a path byte
        // sequence); treat an unrepresentable path like a failed stat.
        out.directory = false;
    }
    Some(out)
}

/// S-023-R76 `os_closedir(dir)`: `closedir` + free the handle.
/// `os_closedir(NULL)` in C is a no-op (the bridge guards).
pub fn os_closedir(dir: OsDir) {
    drop(dir);
}

/// R76: `stat` the full path; true iff it exists and is a directory
/// (links followed). A failed `stat` logs the C debug line (dangling
/// link: errno 2) and is false.
fn is_dir(path: &CStr) -> bool {
    // SAFETY: `st` is plain C storage; `stat` either fills it or leaves it.
    let mut st: libc::stat = unsafe { std::mem::zeroed() };
    // SAFETY: `path` is a valid NUL-terminated C string for the call.
    if unsafe { libc::stat(path.as_ptr(), &mut st) } == 0 {
        (st.st_mode & libc::S_IFMT) == libc::S_IFDIR
    } else {
        // C: `blog(LOG_DEBUG, "is_dir: stat for %s failed, errno: %d", path, errno)`.
        // SAFETY: `stat` was called on this thread immediately before.
        let err = std::io::Error::last_os_error();
        log_platform(
            LOG_DEBUG,
            &format!(
                "is_dir: stat for {} failed, errno: {}",
                path.to_string_lossy(),
                err.raw_os_error().unwrap_or(0)
            ),
        );
        false
    }
}

/// C `struct os_globent` (`path` points into the handle's `glob_t`
/// memory; both die with the handle, as in C).
#[repr(C)]
#[derive(Debug)]
pub struct OsGlobEnt {
    pub path: *mut c_char,
    pub directory: bool,
}

/// C `struct os_glob_info` (the `os_glob_t` base).
#[repr(C)]
#[derive(Debug)]
pub struct OsGlobInfo {
    pub gl_pathc: usize,
    pub gl_pathv: *mut OsGlobEnt,
}

/// `posix_glob_info`: the base plus the `glob_t` whose path strings the
/// entries point into (plus the Rust entry vector, invisible to C — the
/// C struct is `{base, gl}` and callers only touch `base`).
#[repr(C)]
pub struct OsGlob {
    pub base: OsGlobInfo,
    gl: libc::glob_t,
    ents: Vec<OsGlobEnt>,
}

/// S-023-R77 `os_glob(pattern, flags, &out)`: `flags` is ignored (the
/// reference passes 0 to `glob(3)`), so matches are sorted ascending
/// (bytewise in the C locale), no brace/tilde expansion, no match is an
/// error. `Ok` carries the handle (`*out`); `Err` is the `glob` error
/// code (3 = no match, 1 = out of memory, 2 = read error) with `*out`
/// left NULL by the caller/bridge.
pub fn os_glob(pattern: &CStr, _flags: i32) -> Result<Box<OsGlob>, i32> {
    // SAFETY: `gl` is plain C storage, zeroed before `glob()` fills the fields it uses on every path we observe (success and the error codes returned here).
    let mut gl: libc::glob_t = unsafe { std::mem::zeroed() };
    // SAFETY: `pattern` is a valid NUL-terminated C string; `gl` is valid `glob_t` storage; no error handler (`None` — the reference passes NULL).
    let ret = unsafe { libc::glob(pattern.as_ptr(), 0, None, &mut gl) };
    if ret != 0 {
        return Err(ret);
    }
    let mut ents = Vec::with_capacity(gl.gl_pathc as usize);
    for i in 0..gl.gl_pathc as usize {
        // SAFETY: `gl_pathv[0..gl_pathc]` is valid while `gl` is alive (it stays inside the returned handle); each path is NUL-terminated.
        let path = unsafe { *gl.gl_pathv.add(i) };
        // SAFETY: `path` is a valid NUL-terminated C string for the call.
        let cpath = unsafe { CStr::from_ptr(path) };
        ents.push(OsGlobEnt {
            path,
            directory: is_dir(cpath),
        });
    }
    let base = OsGlobInfo {
        gl_pathc: ents.len(),
        gl_pathv: ents.as_ptr() as *mut OsGlobEnt,
    };
    Ok(Box::new(OsGlob { base, gl, ents }))
}

/// S-023-R77 `os_globfree(glob)`: `globfree` + free the entries and the
/// handle. `os_globfree(NULL)` in C is a no-op (the bridge guards).
pub fn os_globfree(g: Box<OsGlob>) {
    let mut g = g;
    // SAFETY: `g.gl` was filled by `glob()` in `os_glob` (the only constructor of `OsGlob`).
    unsafe { libc::globfree(&mut g.gl) };
    // `g.ents` and the box are dropped here.
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::os_fs::{LOG_DEBUG, set_platform_log_sink};

    fn dir(label: &str) -> std::path::PathBuf {
        let d = std::env::temp_dir().join(format!("obs_rust_os_dir_{label}"));
        let _ = std::fs::remove_dir_all(&d);
        std::fs::create_dir_all(&d).unwrap();
        d
    }

    fn c(s: &str) -> std::ffi::CString {
        std::ffi::CString::new(s.as_bytes()).unwrap()
    }

    /// R76: `.` and `..` are entries, the directory flag follows links,
    /// a dangling link logs the debug line and is not a directory
    /// (vector 22-dir-iteration).
    #[test]
    #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
    fn readdir() {
        let _lock = crate::os_fs::PLATFORM_SINK_TEST_LOCK.lock().unwrap();
        let d = dir("readdir");
        std::fs::write(d.join("a.txt"), "A").unwrap();
        std::fs::create_dir(d.join("sub")).unwrap();
        let path = c(d.to_str().unwrap());
        let mut h = os_opendir(&path).expect("open");
        let mut names: Vec<String> = Vec::new();
        let mut flags: Vec<(String, bool)> = Vec::new();
        while let Some(e) = os_readdir(&mut h) {
            // SAFETY: `e.d_name` is NUL-terminated (written by `os_readdir`).
            let n = unsafe { CStr::from_ptr(e.d_name.as_ptr()) }
                .to_string_lossy()
                .into_owned();
            names.push(n.clone());
            flags.push((n, e.directory));
        }
        // end of directory: further calls give None (the runner's
        // `end_repeat` protocol)
        assert!(os_readdir(&mut h).is_none());
        assert!(os_readdir(&mut h).is_none());
        let found = |n: &str| flags.iter().find(|(name, _)| name == n);
        assert!(found(".").unwrap().1, ". is a directory");
        assert!(found("..").unwrap().1, ".. is a directory");
        assert!(!found("a.txt").unwrap().1);
        assert!(found("sub").unwrap().1);
        os_closedir(h);
        // NULL handle: the bridge guards, the API needs a live handle
        // (checked at the C-ABI boundary).
        // missing / not a directory
        assert!(os_opendir(&c("/nonexistent-wp30")).is_none());
        assert!(os_opendir(&c(d.join("a.txt").to_str().unwrap())).is_none());
        // a dangling link is not a directory and logs the R76 debug line
        // SAFETY: test — valid paths for `symlink`.
        unsafe {
            let t = c(d.join("nowhere").to_str().unwrap());
            let l = c(d.join("dead").to_str().unwrap());
            libc::symlink(t.as_ptr(), l.as_ptr());
        }
        let (tx, rx) = std::sync::mpsc::channel();
        set_platform_log_sink(move |level, line| {
            let _ = tx.send((level, line.to_string()));
        });
        let mut h = os_opendir(&path).expect("open again");
        while os_readdir(&mut h).is_some() {}
        os_closedir(h);
        let line = rx
            .recv_timeout(std::time::Duration::from_secs(5))
            .expect("the is_dir debug line");
        assert_eq!(line.0, LOG_DEBUG);
        assert_eq!(
            line.1,
            format!("is_dir: stat for {}/dead failed, errno: 2", d.display())
        );
        std::fs::remove_dir_all(&d).unwrap();
    }

    /// D-5 regression (Rust-side half): the handle owns its path, so it
    /// outlives the source string. (The C-ABI half — a freed source
    /// buffer — is `d5_opendir_copies_path` in obs-ffi.)
    #[test]
    #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
    fn d5_path_is_copied() {
        let d = dir("d5");
        std::fs::write(d.join("x"), "x").unwrap();
        let path = c(d.to_str().unwrap());
        // the borrow ends when `os_opendir` returns; the CString is
        // dropped before the first `os_readdir`
        let mut h = { os_opendir(&path).expect("open") };
        drop(path);
        let mut names = Vec::new();
        while let Some(e) = os_readdir(&mut h) {
            // SAFETY: `e.d_name` is NUL-terminated.
            names.push(
                unsafe { CStr::from_ptr(e.d_name.as_ptr()) }
                    .to_string_lossy()
                    .into_owned(),
            );
        }
        assert!(
            names.contains(&"x".to_string()),
            "enumeration works after the source is gone"
        );
        os_closedir(h);
        std::fs::remove_dir_all(&d).unwrap();
    }

    /// R77: glob semantics — sorted, no brace/tilde, `*` does not
    /// match a leading `.` or `/`, trailing `/` restricts to
    /// directories (vector 23-glob).
    #[test]
    #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
    fn glob() {
        let d = dir("glob");
        let g = d.join("g");
        std::fs::create_dir(&g).unwrap();
        for (name, is_dir) in [
            ("a.txt", false),
            ("b.txt", false),
            ("c.dat", false),
            (".hid", false),
            ("A1", false),
            ("b1", false),
            ("b2", false),
            ("b10", false),
            ("sub", true),
            ("subdir", true),
        ] {
            let p = g.join(name);
            if is_dir {
                std::fs::create_dir(&p).unwrap();
            } else {
                std::fs::write(&p, "x").unwrap();
            }
        }
        std::fs::write(g.join("sub/inner.txt"), "I").unwrap();
        let rel = |s: &str| c(d.join(s).to_str().unwrap());

        // `g/*`: sorted bytewise, dotfiles excluded
        let h = os_glob(&rel("g/*"), 0xff).expect("match");
        // SAFETY: `gl_pathv[0..gl_pathc]` is valid while the handle is alive.
        let names: Vec<String> = unsafe {
            (0..h.base.gl_pathc)
                .map(|i| {
                    CStr::from_ptr((*h.base.gl_pathv.add(i)).path)
                        .to_string_lossy()
                        .into_owned()
                })
                .collect()
        };
        os_globfree(h);
        let core: Vec<String> = vec!["A1", "b1", "b10", "b2", "b.txt", "c.dat", "sub", "subdir"]
            .into_iter()
            .map(|n| g.join(n).to_string_lossy().into_owned())
            .collect();
        for w in &core {
            assert!(names.contains(w), "missing {w} in {names:?}");
        }
        assert!(
            !names.iter().any(|n| n.ends_with(".hid")),
            "dotfile excluded by *"
        );
        // `g/*.txt`
        let h = os_glob(&rel("g/*.txt"), 0xff).expect("match");
        assert_eq!(h.base.gl_pathc, 2);
        os_globfree(h);
        // `g/sub*/` keeps the trailing slash and restricts to directories
        let h = os_glob(&rel("g/sub*/"), 0xff).expect("match");
        assert_eq!(h.base.gl_pathc, 2);
        // SAFETY: C view while alive.
        let p0 = unsafe { CStr::from_ptr((*h.base.gl_pathv).path) }
            .to_string_lossy()
            .into_owned();
        assert!(p0.ends_with('/'));
        os_globfree(h);
        // `g/.*` matches `.`, `..` and dotfiles
        let h = os_glob(&rel("g/.*"), 0xff).expect("match");
        assert_eq!(h.base.gl_pathc, 3);
        os_globfree(h);
        // no match: no handle, error is the glob(3) return passed
        // through: 3 (GLOB_NOMATCH) on Linux; macOS's glob(3) returns
        // -3 for these no-match patterns — the C reference shows the
        // same value, so the expectation is platform-bound
        let no_match = if cfg!(target_os = "macos") { -3 } else { 3 };
        assert_eq!(os_glob(&rel("g/nothing"), 0).err(), Some(no_match));
        // braces and tilde are literal (no GLOB_BRACE / GLOB_TILDE)
        assert_eq!(os_glob(&rel("g/{a,b}.txt"), 0).err(), Some(no_match));
        assert_eq!(os_glob(&c("~"), 0).err(), Some(no_match));
        // wildcards never match `/`
        assert_eq!(
            os_glob(&rel("g/*/inner.txt"), 0)
                .map(|h| h.base.gl_pathc)
                .unwrap_or(0),
            1
        );
        // an exact (wildcard-free) directory name matches with the flag
        let h = os_glob(&rel("g"), 0).expect("match");
        assert_eq!(h.base.gl_pathc, 1);
        // SAFETY: C view while alive.
        assert!(unsafe { (*h.base.gl_pathv).directory });
        os_globfree(h);
        // a pattern with a missing component fails
        assert_eq!(os_glob(&rel("nodir/*"), 0).err(), Some(no_match));
        // the empty pattern is an error (GLOB_NOMATCH)
        assert_eq!(os_glob(&c(""), 0).err(), Some(no_match));
        std::fs::remove_dir_all(&d).unwrap();
    }
}
