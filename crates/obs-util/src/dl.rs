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
//! Logging: R107's failure log `os_dlopen(<path>-><used name>):
//! <dlerror text>` goes through an injectable error sink (the profiler
//! in obs-util-small set the pattern). obs-util sits BELOW the base
//! logging layer (S-001 §4.2 lands in `obs-ffi`'s C base file), so it
//! cannot call `blog` yet; the default sink is `eprintln!` (blog's
//! ERROR stream is stderr). The FFI/base layer wires the real sink
//! later. C-NULL arguments are unrepresentable in the safe API (NULL
//! path returns NULL without a log; NULL handle for `os_dlclose` is a
//! no-op — both documented).

use std::os::raw::c_void;
use std::sync::Mutex;

/// R107 error sink: `blog(LOG_ERROR, "os_dlopen(%s->%s): %s\n", path, used,
/// dlerror())`. Same injectable-sink pattern as the profiler in
/// obs-util-small; default is `eprintln!` (blog's ERROR stream).
type ErrorSink = dyn FnMut(&str) + Send;

static ERROR_SINK: Mutex<Option<Box<ErrorSink>>> = Mutex::new(None);

/// Route R107's failure line (and future platform-helper error lines)
/// through `sink` instead of the default `eprintln!`.
pub fn set_dlopen_error_sink(sink: impl FnMut(&str) + Send + 'static) {
    *ERROR_SINK.lock().unwrap() = Some(Box::new(sink));
}

fn log_dlopen_error(line: &str) {
    let mut guard = ERROR_SINK.lock().unwrap();
    if let Some(sink) = guard.as_mut() {
        sink(line);
    } else {
        eprintln!("{line}");
    }
}

fn dlerror_text() -> String {
    // SAFETY: per-thread; returns a valid NUL-terminated string or NULL.
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

/// An open dynamic library (S-023 R107).
#[derive(Debug)]
pub struct OsLibrary {
    handle: *mut c_void,
}

/// `os_dlopen(path)`: if `path` contains the substring `.so` anywhere
/// it is used as is, else `.so` is appended; `dlopen` with `RTLD_NOW`.
/// `None` on failure, logging `os_dlopen(<path>-><used name>): <dlerror`
/// (R107; the sink is injectable via `set_dlopen_error_sink`).
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
        // R107: exact C format `os_dlopen(%s->%s): %s\n`. The dlerror
        // text is platform-dependent (the conformance vector truncates
        // it at "):" and compares only the prefix).
        log_dlopen_error(&format!("os_dlopen({path}->{name}): {}\n", dlerror_text()));
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

    // the error sink and dlerror state are process/thread-global: every
    // test that calls os_dlopen serialises on this lock (house pattern,
    // W9).
    static DL_TEST_LOCK: Mutex<()> = Mutex::new(());

    // vector 35-dl-and-misc (the parts testable without the runner):
    // a real library, a missing symbol, a missing library (with and
    // without the .so suffix), and the .so append rule.
    #[test]
    #[cfg(all(target_os = "linux", not(miri)))]
    fn dl_real_library() {
        // glibc vector: the real-library case uses the glibc name libm.so.6.
        // Not runnable on macOS (libm is libm.dylib, which the R107 append
        // rule would mangle: a name without a ".so" substring gets ".so"
        // appended) — revisit with the C checkout (S2).
        let lib = os_dlopen("libm.so.6").expect("libm.so.6 loads");
        assert!(os_dlsym(&lib, "cos").is_some());
        assert!(os_dlsym(&lib, "no_such_symbol_in_libm").is_none());
        os_dlclose(&lib);
    }

    #[test]
    #[cfg_attr(miri, ignore)]
    fn dl_missing_library() {
        let _lock = DL_TEST_LOCK.lock().unwrap();
        assert!(os_dlopen("nosuchlibrary_xyz").is_none());
        assert!(os_dlopen("nosuchlibrary_xyz.so").is_none());
    }

    // vector 35-dl-and-misc, R107: the failure log is
    // `os_dlopen(<path>-><used name>): <dlerror>` — the conformance
    // runner truncates the non-portable dlerror text at "):", so the
    // vector pins exactly the prefix up to and including "):". Capture
    // the sink and assert that prefix byte-for-byte for the append case
    // and the as-is case (the two vector lines).
    #[test]
    #[cfg(not(miri))]
    fn dl_r107_error_line_prefix() {
        use std::sync::Arc;
        let _lock = DL_TEST_LOCK.lock().unwrap();
        let lines: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
        let captured = lines.clone();
        set_dlopen_error_sink(move |line| captured.lock().unwrap().push(line.to_string()));
        assert!(os_dlopen("nosuchlibrary_xyz").is_none());
        assert!(os_dlopen("nosuchlibrary_xyz.so").is_none());
        assert!(os_dlopen("nosuchlibrary_xyz.something.so").is_none());
        // restore the default sink so other tests in this binary are
        // unaffected (the sink is process-global).
        *ERROR_SINK.lock().unwrap() = None;
        let lines = lines.lock().unwrap();
        assert_eq!(lines.len(), 3, "one error line per failed open");
        // the dlerror text after "):" is platform-dependent, so assert the
        // vector-pinned prefix (up to and including "):") and that a dlerror
        // fragment follows it, with the C trailing newline.
        let colon = lines[0].find("):").unwrap();
        assert_eq!(
            &lines[0][..colon + 2],
            "os_dlopen(nosuchlibrary_xyz->nosuchlibrary_xyz.so):"
        );
        let colon1 = lines[1].find("):").unwrap();
        assert_eq!(
            &lines[1][..colon1 + 2],
            "os_dlopen(nosuchlibrary_xyz.so->nosuchlibrary_xyz.so):"
        );
        let colon2 = lines[2].find("):").unwrap();
        assert_eq!(
            &lines[2][..colon2 + 2],
            "os_dlopen(nosuchlibrary_xyz.something.so->nosuchlibrary_xyz.something.so):"
        );
        assert!(lines[0].starts_with(&format!("{} ", &lines[0][..colon + 2])));
        assert!(lines[0].ends_with('\n'));
    }
}
