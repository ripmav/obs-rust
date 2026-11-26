//! S-023 R107: dynamic library loading (`os_dlopen` / `os_dlsym` /
//! `os_dlclose`).
//!
//! The FFI boundary of the platform helpers; the `extern "C"` surface
//! lives in `obs-ffi` later. The `unsafe` blocks are the platform FFI
//! calls themselves (the S-023 platform tier, ADR 0005 Amendment 1):
//! the libc `dlopen` family on unix, `LoadLibraryExW`/
//! `GetProcAddress`/`FreeLibrary` on Windows (WP-38 / finding 5, per
//! `platform-windows.c`'s `os_dlopen`).
//!
//! Platform behaviour:
//! - unix (R107): `.so` appended when the name has no `.so` substring,
//!   `dlopen(RTLD_NOW)`, failure log `os_dlopen(<path>-><used name>):
//!   <dlerror text>`.
//! - Windows (platform-windows.c): backslashes to forward slashes, `.dll`
//!   appended when the name has no `.dll` substring; a name containing a
//!   path separator is resolved with `GetFullPathNameW` and loaded with
//!   `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`
//!   (a bare name with `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS`); an
//!   `ERROR_PROC_NOT_FOUND` failure is silent ("libraries that aren't
//!   meant to be dynamically linked"); any other failure logs
//!   `LoadLibrary failed for '<path>': <FormatMessageA text> (<code>)`.
//!
//! Logging: the failure line goes through an injectable error sink (the
//! profiler in obs-util-small set the pattern). obs-util sits BELOW the
//! base logging layer (S-001 §4.2 lands in `obs-ffi`'s C base file), so
//! it cannot call `blog` yet; the default sink is `eprintln!` (blog's
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

#[cfg(unix)]
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

/// `os_dlopen(path)` (S-023 R107, unix): if `path` contains the
/// substring `.so` anywhere it is used as is, else `.so` is appended;
/// `dlopen` with `RTLD_NOW`. `None` on failure, logging `os_dlopen(<path>
/// -><used name>): <dlerror>` (R107; the sink is injectable via
/// `set_dlopen_error_sink`).
#[cfg(unix)]
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

/// `os_dlopen(path)` (S-023 R107, Windows — platform-windows.c): the
/// `.dll` append rule, the full-path search for names with a path
/// separator, and the `ERROR_PROC_NOT_FOUND` silence (see the module
/// docs). `None` on failure, logging through the same sink.
#[cfg(windows)]
pub fn os_dlopen(path: &str) -> Option<OsLibrary> {
    use crate::winapi;
    // C: dstr_init_copy + dstr_replace("\\", "/") + the ".dll" append
    // rule (substring check, as in the ".so" rule).
    let name = path.replace('\\', "/");
    let name = if name.contains(".dll") {
        name
    } else {
        format!("{name}.dll")
    };
    // C: os_utf8_to_wcs_ptr (NUL-terminated UTF-16)
    let mut wname: Vec<u16> = name.encode_utf16().collect();
    wname.push(0);

    let handle = if name.contains('/') {
        // C: GetFullPathNameW(wpath, MAX_PATH, fullpath, NULL) — a path
        // separator means the search gets the DLL-load-dir flag.
        let mut full = [0u16; winapi::MAX_PATH as usize];
        // SAFETY: `wname` is a live NUL-terminated UTF-16 string; `full`
        // holds MAX_PATH wchars; the reserved out pointer may be NULL.
        let n = unsafe {
            winapi::GetFullPathNameW(
                wname.as_ptr(),
                winapi::MAX_PATH,
                full.as_mut_ptr(),
                std::ptr::null_mut(),
            )
        };
        if n == 0 || n > winapi::MAX_PATH {
            // C: `h_library` stays NULL and falls through to the error log.
            // SAFETY: GetLastError is valid after a failed system call.
            return dlopen_failed(path, unsafe { winapi::GetLastError() });
        }
        // SAFETY: `full` holds a NUL-terminated full path written by
        // GetFullPathNameW; NULL file handle; the search flags are the
        // Win8+ pair the C reference uses.
        unsafe {
            winapi::LoadLibraryExW(
                full.as_ptr(),
                std::ptr::null_mut(),
                winapi::LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | winapi::LOAD_LIBRARY_SEARCH_DEFAULT_DIRS,
            )
        }
    } else {
        // SAFETY: `wname` is a live NUL-terminated UTF-16 string.
        unsafe {
            winapi::LoadLibraryExW(
                wname.as_ptr(),
                std::ptr::null_mut(),
                winapi::LOAD_LIBRARY_SEARCH_DEFAULT_DIRS,
            )
        }
    };

    if handle.is_null() {
        // SAFETY: GetLastError is valid after the failed LoadLibraryExW.
        dlopen_failed(path, unsafe { winapi::GetLastError() })
    } else {
        Some(OsLibrary { handle })
    }
}

/// The Windows failure exit of `os_dlopen` (platform-windows.c): silent
/// for `ERROR_PROC_NOT_FOUND`, otherwise the C-format log line through
/// the sink (`blog(LOG_INFO, "LoadLibrary failed for '%s': %s (%lu)")`).
#[cfg(windows)]
fn dlopen_failed(path: &str, error: u32) -> Option<OsLibrary> {
    use crate::winapi;
    // C: "don't print error for libraries that aren't meant to be
    // dynamically linked"
    if error == winapi::ERROR_PROC_NOT_FOUND {
        return None;
    }
    // C: FormatMessageA(FROM_SYSTEM | IGNORE_INSERTS | ALLOCATE_BUFFER,
    // NULL, error, MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US), ...)
    let mut message: *mut i8 = std::ptr::null_mut();
    // SAFETY: `message` is the out cell for the FormatMessageA-allocated
    // buffer (ALLOCATE_BUFFER): the call stores the allocated `LPSTR`
    // into the `message` cell — the C reference passes the cell's
    // address cast to `LPSTR`, which the `.cast` mirrors;
    // a NULL va list is valid for a message without inserts
    // (IGNORE_INSERTS).
    unsafe {
        winapi::FormatMessageA(
            winapi::FORMAT_MESSAGE_FROM_SYSTEM
                | winapi::FORMAT_MESSAGE_IGNORE_INSERTS
                | winapi::FORMAT_MESSAGE_ALLOCATE_BUFFER,
            std::ptr::null(),
            error,
            winapi::LANG_ENGLISH_US,
            std::ptr::addr_of_mut!(message).cast::<i8>(),
            0,
            std::ptr::null_mut(),
        );
    }
    let text = if message.is_null() {
        String::new()
    } else {
        // SAFETY: `message` is a valid NUL-terminated ANSI string in
        // LocalAlloc memory (the ALLOCATE_BUFFER contract).
        let s = unsafe { std::ffi::CStr::from_ptr(message) }
            .to_string_lossy()
            .into_owned();
        // SAFETY: `message` was allocated by FormatMessageA.
        unsafe { winapi::LocalFree(message as *mut c_void) };
        s
    };
    log_dlopen_error(&format!(
        "LoadLibrary failed for '{path}': {text} ({error})"
    ));
    None
}

/// `os_dlsym`: the address of `name` in `lib`, or `None` if absent.
#[cfg(unix)]
pub fn os_dlsym(lib: &OsLibrary, name: &str) -> Option<*mut c_void> {
    // SAFETY: NUL-terminated, valid for the duration of the call.
    let c = std::ffi::CString::new(name).expect("no interior NUL");
    // SAFETY: `lib.handle` was returned by `dlopen` and is alive; `c`
    // is a valid symbol name. `dlsym` returns an address or NULL.
    let addr = unsafe { libc::dlsym(lib.handle, c.as_ptr()) };
    if addr.is_null() { None } else { Some(addr) }
}

#[cfg(windows)]
pub fn os_dlsym(lib: &OsLibrary, name: &str) -> Option<*mut c_void> {
    // C: GetProcAddress(module, func) — NULL means absent.
    // SAFETY: NUL-terminated, valid for the duration of the call.
    let c = std::ffi::CString::new(name).expect("no interior NUL");
    // SAFETY: `lib.handle` is a live `HMODULE`; `c` a valid symbol name.
    let addr = unsafe { crate::winapi::GetProcAddress(lib.handle, c.as_ptr().cast::<u8>()) };
    if addr.is_null() { None } else { Some(addr) }
}

/// `os_dlclose`: closes the library. (C-NULL is a no-op — a `&OsLibrary`
/// can never be NULL.)
#[cfg(unix)]
pub fn os_dlclose(lib: &OsLibrary) {
    // SAFETY: `lib.handle` was returned by `dlopen` and is not closed
    // twice (the FFI layer owns the single close).
    unsafe { libc::dlclose(lib.handle) };
}

#[cfg(windows)]
pub fn os_dlclose(lib: &OsLibrary) {
    // C: FreeLibrary(module).
    // SAFETY: `lib.handle` was returned by `LoadLibraryExW` and is not
    // freed twice (the FFI layer owns the single close).
    unsafe { crate::winapi::FreeLibrary(lib.handle) };
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
    // and the as-is case (the two vector lines). (Unix only — the
    // Windows C reference logs a different line, pinned by
    // `dl_windows_error_line_shape`.)
    #[test]
    #[cfg(unix)]
    #[cfg_attr(miri, ignore)]
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

    // WP-38 (finding 5, Windows): a real library through the
    // `LoadLibraryExW`/`GetProcAddress` path — kernel32 is always loaded.
    #[test]
    #[cfg(windows)]
    fn dl_windows_real_library() {
        let lib = os_dlopen("kernel32").expect("kernel32.dll loads");
        assert!(os_dlsym(&lib, "GetProcAddress").is_some());
        assert!(os_dlsym(&lib, "no_such_symbol_in_kernel32").is_none());
        os_dlclose(&lib);
    }

    // WP-38 (finding 5, Windows): the C failure line shape
    // `LoadLibrary failed for '<path>': <message> (<code>)` through the
    // same sink as the unix R107 line (platform-windows.c format — no
    // trailing newline, unlike the unix `os_dlopen(…)` line).
    #[test]
    #[cfg(windows)]
    fn dl_windows_error_line_shape() {
        use std::sync::Arc;
        let _lock = DL_TEST_LOCK.lock().unwrap();
        let lines: Arc<Mutex<Vec<String>>> = Arc::new(Mutex::new(Vec::new()));
        let captured = lines.clone();
        set_dlopen_error_sink(move |line| captured.lock().unwrap().push(line.to_string()));
        assert!(os_dlopen("nosuchlibrary_xyz").is_none());
        // restore the default sink so other tests in this binary are
        // unaffected (the sink is process-global).
        *ERROR_SINK.lock().unwrap() = None;
        let lines = lines.lock().unwrap();
        assert_eq!(lines.len(), 1, "one error line per failed open");
        // prefix per the C format, a non-empty FormatMessageA text, and
        // the numeric error code in parentheses (the exact code/message
        // are platform-dependent and not pinned).
        let line = &lines[0];
        let colon = line.find(": ").expect("a message follows the path");
        assert_eq!(&line[..colon], "LoadLibrary failed for 'nosuchlibrary_xyz'");
        assert!(line.ends_with(')'));
        let code = line[line.rfind('(').unwrap() + 1..line.len() - 1].to_string();
        assert!(
            code.chars().all(|c| c.is_ascii_digit()) && !code.is_empty(),
            "a numeric error code: {line:?}"
        );
        assert!(
            line.len() > colon + 2 + code.len() + 2,
            "a message between: {line:?}"
        );
        assert!(!line.ends_with('\n'));
    }
}
