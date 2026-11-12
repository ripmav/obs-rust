//! S-023 R107: dynamic library loading (`os_dlopen` / `os_dlsym` /
//! `os_dlclose`).
//!
//! The FFI boundary of the platform helpers (the C `dlopen` family);
//! the `extern "C"` surface lives in `obs-ffi` later. The `unsafe`
//! blocks below are the libc FFI calls themselves — the same class as
//! the bmem crash-handler cell (Rule 1 records the FFI boundary in the
//! `*-sys`/`obs-ffi`/platform crates; these platform helpers are the
//! platform tier of S-023).
//!
//! Logging: R107's `LOG_ERROR` on a failed `os_dlopen` is not emitted
//! yet (no logger in the crate — it arrives with the platform crate,
//! S-023); the `None` result is pinned, the FFI layer wires the log
//! line. C-NULL arguments are unrepresentable in the safe API (NULL
//! path returns NULL without a log; NULL handle for `os_dlclose` is a
//! no-op — both documented).

use std::os::raw::c_void;

/// An open dynamic library (S-023 R107).
#[derive(Debug)]
pub struct OsLibrary {
    handle: *mut c_void,
}

/// `os_dlopen(path)`: if `path` contains the substring `.so` anywhere
/// it is used as is, else `.so` is appended; `dlopen` with `RTLD_NOW`.
/// `None` on failure (the C ERROR log is not wired yet — see the module
/// docs).
pub fn os_dlopen(path: &str) -> Option<OsLibrary> {
    let name: String = if path.contains(".so") {
        path.to_string()
    } else {
        format!("{path}.so")
    };
    let c = std::ffi::CString::new(name.as_bytes()).expect("no interior NUL");
    // SAFETY: `c` is a valid NUL-terminated byte string for its use in
    // this call; `dlopen` returns a valid handle or NULL.
    let handle = unsafe { libc::dlopen(c.as_ptr(), libc::RTLD_NOW) };
    if handle.is_null() {
        None
    } else {
        Some(OsLibrary { handle })
    }
}

/// `os_dlsym`: the address of `name` in `lib`, or `None` if absent.
pub fn os_dlsym(lib: &OsLibrary, name: &str) -> Option<*mut c_void> {
    // SAFETY: NUL-terminated, valid for the duration of the call.
    let c = std::ffi::CString::new(name).expect("no interior NUL");
    // SAFETY: `lib.handle` was returned by `dlopen` and is alive; `c`
    // is a valid symbol name. `dlsym` returns an address or NULL.
    let addr = unsafe { libc::dlsym(lib.handle, c.as_ptr()) };
    if addr.is_null() { None } else { Some(addr) }
}

/// `os_dlclose`: closes the library. (C-NULL is a no-op — a `&OsLibrary`
/// can never be NULL.)
pub fn os_dlclose(lib: &OsLibrary) {
    // SAFETY: `lib.handle` was returned by `dlopen` and is not closed
    // twice (the FFI layer owns the single close).
    unsafe { libc::dlclose(lib.handle) };
}

#[cfg(test)]
mod tests {
    use super::*;

    // vector 35-dl-and-misc (the parts testable without the runner):
    // a real library, a missing symbol, a missing library (with and
    // without the .so suffix), and the .so append rule.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn dl_real_library() {
        let lib = os_dlopen("libm.so.6").expect("libm.so.6 loads");
        assert!(os_dlsym(&lib, "cos").is_some());
        assert!(os_dlsym(&lib, "no_such_symbol_in_libm").is_none());
        os_dlclose(&lib);
    }

    #[test]
    #[cfg_attr(miri, ignore)]
    fn dl_missing_library() {
        assert!(os_dlopen("nosuchlibrary_xyz").is_none());
        assert!(os_dlopen("nosuchlibrary_xyz.so").is_none());
    }

    /// The failed `dlerror` text names the *used* file — this pins the
    /// `.so` append rule (the reference's error log carries
    /// `<path>-><used name>`; the log itself is not wired yet).
    fn dlerror_text() -> String {
        // SAFETY: per-thread; returns a valid NUL-terminated string.
        let ptr = unsafe { libc::dlerror() };
        if ptr.is_null() {
            String::new()
        } else {
            // SAFETY: pointer from dlerror is valid for this call.
            unsafe { std::ffi::CStr::from_ptr(ptr) }
                .to_string_lossy()
                .into_owned()
        }
    }

    #[test]
    #[cfg_attr(miri, ignore)]
    fn dl_appends_so_rule() {
        // no ".so" substring: ".so" is appended — the attempted name in
        // the error is "nosuchlibrary_xyz.so"
        assert!(os_dlopen("nosuchlibrary_xyz").is_none());
        let err = dlerror_text();
        assert!(
            err.starts_with("nosuchlibrary_xyz.so"),
            "append rule applied: {err}"
        );
        // ".so" substring: used as is (never "…so.so")
        assert!(os_dlopen("nosuchlibrary_xyz.so").is_none());
        let err = dlerror_text();
        assert!(
            err.starts_with("nosuchlibrary_xyz.so") && !err.contains(".so.so"),
            "used as is: {err}"
        );
        // a name with ".so" in the middle is also used as is
        assert!(os_dlopen("nosuchlibrary_xyz.something.so").is_none());
        let err = dlerror_text();
        assert!(
            err.starts_with("nosuchlibrary_xyz.something.so"),
            "used as is: {err}"
        );
    }
}
