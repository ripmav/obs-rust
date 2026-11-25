//! obs-ffi: C ABI surface.
//!
//! P1-13.1 (S-001 §4.2, R20-R25): the logging shim. The variadic `blog`/
//! `bcrash` and the `va_list` handler callbacks stay in `c/base-logging.c`
//! (stable Rust cannot define or consume variadics — primer §5.5). Rust
//! code logs through [`blog`], which routes via `obs_ffi_log(level, msg)`
//! = `blog(level, "%s", msg)` into the installed C handler.
//!
//! P1-08.5 (recipe step 2): the `extern "C"` bridges for the ported leaf
//! modules (feature `c-swap`) live in `abi_*` modules ([`abi_text_lookup`] first, then
//! [`abi_lexer`], then [`abi_cf_lexer`]); P1-05.4 adds the threading
//! primitives ([`abi_threading`], unix-only — the Windows HANDLE port is
//! P1-05.3); P1-07.3 adds the Flatpak sleep-inhibition exports
//! ([`abi_platform`], unix-only).

// The bridges reference C symbols (`bmalloc`, `bfree`, `os_fopen`, ...) that
// only exist in the swap build (libobs/util/*.c linked next to this
// staticlib). They are compiled only for that build (`--features c-swap`,
// what `OBS_ENABLE_RUST=ON` passes) and for this crate's unit tests, which
// provide stand-ins. Without the gate, linkers that do not garbage-collect
// unreferenced sections (MSVC) fail with LNK2019 on the integration tests.
#[cfg(any(test, feature = "c-swap"))]
pub mod abi_cf_lexer;
#[cfg(any(test, feature = "c-swap"))]
pub mod abi_cf_parser;
#[cfg(any(test, feature = "c-swap"))]
pub mod abi_common;
#[cfg(any(test, feature = "c-swap"))]
pub mod abi_lexer;
// The Flatpak sleep-inhibition files are unix-only (os_inhibit is
// `#[cfg(unix)]` in obs-util); the bridge follows the same gate.
#[cfg(unix)]
#[cfg(any(test, feature = "c-swap"))]
pub mod abi_platform;
#[cfg(any(test, feature = "c-swap"))]
pub mod abi_text_lookup;
// The threading primitives are unix in obs-util (`#[cfg(unix)]` — the
// Windows HANDLE port is P1-05.3); the bridge follows the same gate.
#[cfg(unix)]
#[cfg(any(test, feature = "c-swap"))]
pub mod abi_threading;

use std::os::raw::{c_int, c_void};

/// S-001 §3 (R20): log level constants; lower numbers are more severe.
pub const LOG_ERROR: c_int = 100;
/// S-001 §3 (R20).
pub const LOG_WARNING: c_int = 200;
/// S-001 §3 (R20).
pub const LOG_INFO: c_int = 300;
/// S-001 §3 (R20).
pub const LOG_DEBUG: c_int = 400;

/// Opaque C `va_list`; handler callbacks receive it, Rust never builds one.
pub type VaList = *mut c_void;
/// C `log_handler_t` (R21): receives the **unformatted** format string.
pub type LogHandler = unsafe extern "C" fn(
    level: c_int,
    format: *const std::ffi::c_char,
    args: VaList,
    param: *mut c_void,
);
/// C crash handler type (R24).
pub type CrashHandler =
    unsafe extern "C" fn(format: *const std::ffi::c_char, args: VaList, param: *mut c_void);

mod ffi {
    use super::{CrashHandler, LogHandler};
    use std::ffi::c_char;
    use std::os::raw::{c_int, c_void};

    unsafe extern "C" {
        pub fn obs_ffi_log(level: c_int, msg: *const c_char);
        pub fn base_get_log_handler(handler: *mut Option<LogHandler>, param: *mut *mut c_void);
        pub fn base_set_log_handler(handler: Option<LogHandler>, param: *mut c_void);
        pub fn base_set_crash_handler(handler: CrashHandler, param: *mut c_void);
    }
}

/// `blog(level, "%s", msg)` (primer §5.5): log an already-formatted message
/// through the installed C handler. `msg` is treated as a C string — an
/// embedded NUL terminates it.
pub fn blog(level: c_int, msg: &str) {
    // SAFETY: the C log sink (installed through the `base_set_log_handler` bridge); `msg` is NUL-terminated for the call.
    unsafe {
        ffi::obs_ffi_log(level, msg.as_ptr().cast());
    }
}

/// Install a log handler (R22). `None` reinstalls the default handler
/// (and stores `param`).
///
/// # Safety
/// The C shim stores `param` and the handler is called with C data; the
/// handler must follow the C `log_handler_t` contract.
pub unsafe fn base_set_log_handler(handler: Option<LogHandler>, param: *mut c_void) {
    unsafe {
        // SAFETY: the C log-handler setter; `handler`/`param` follow the documented C contract.
        ffi::base_set_log_handler(handler, param);
    }
}

/// The current log handler and parameter (R22). `Some` unless a NULL
/// handler was installed from C (the default handler is a real function).
///
/// # Safety
/// Reads handler state owned by the C shim; the returned `param` is only
/// valid while the handler remains installed.
pub unsafe fn base_get_log_handler() -> (Option<LogHandler>, *mut c_void) {
    let mut h: Option<LogHandler> = None;
    let mut p: *mut c_void = std::ptr::null_mut();
    // SAFETY: `h` and `p` are valid storage for the C out params.
    unsafe {
        ffi::base_get_log_handler(&mut h, &mut p);
    }
    (h, p)
}

/// Install the crash handler — succeeds at most once per process (R25);
/// later calls are ignored (with a `LOG_WARNING` from the C shim).
///
/// # Safety
/// The C shim stores `param`; the handler must follow the C crash-handler
/// contract (R24).
pub unsafe fn base_set_crash_handler(handler: CrashHandler, param: *mut c_void) {
    unsafe {
        // SAFETY: the C crash-handler setter; `handler`/`param` follow the documented C contract.
        ffi::base_set_crash_handler(handler, param);
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;
    use std::sync::Mutex;

    // The log handler is process-global; serialize the in-process handler
    // tests on it (W9 fence pattern).
    static TEST_LOCK: Mutex<()> = Mutex::new(());

    static CAP: Mutex<Option<(c_int, String, usize)>> = Mutex::new(None);

    unsafe extern "C" fn capture(
        level: c_int,
        format: *const std::ffi::c_char,
        _args: VaList,
        param: *mut c_void,
    ) {
        // SAFETY: a live NUL-terminated C string (checked above).
        let s = unsafe { CStr::from_ptr(format) }
            .to_string_lossy()
            .into_owned();
        *CAP.lock().unwrap() = Some((level, s, param as usize));
    }

    // R20: the constants the Rust side exposes
    #[test]
    fn r20_level_constants() {
        assert_eq!(LOG_ERROR, 100);
        assert_eq!(LOG_WARNING, 200);
        assert_eq!(LOG_INFO, 300);
        assert_eq!(LOG_DEBUG, 400);
    }

    // R21/R22 in-process: the handler gets the unformatted format ("%s"
    // for the Rust path), the param round-trips, NULL reinstalls the default
    #[test]
    fn r21_r22_handler_roundtrip() {
        let _lock = TEST_LOCK.lock().unwrap();
        let mut sentinel = 0u8;
        // SAFETY: the C log-handler setter; `capture` is a valid C function pointer for the process lifetime (test).
        unsafe { base_set_log_handler(Some(capture), &mut sentinel as *mut _ as *mut c_void) };
        blog(LOG_INFO, "hello");
        let (lvl, fmt, p) = CAP.lock().unwrap().take().expect("handler ran");
        assert_eq!(lvl, LOG_INFO);
        assert_eq!(fmt, "%s");
        assert_eq!(p, &mut sentinel as *mut _ as usize);

        // SAFETY: `h` and `p` are valid storage for the C out params.
        let (h, p) = unsafe { base_get_log_handler() };
        assert!(h.is_some());
        assert_eq!(p as usize, &mut sentinel as *mut _ as usize);

        // NULL reinstalls the default (R22): the capture stops receiving
        // SAFETY: the C log-handler reset (the documented NULL no-op case).
        unsafe { base_set_log_handler(None, std::ptr::null_mut()) };
        // SAFETY: `h` and `p` are valid storage for the C out params.
        let (h, p) = unsafe { base_get_log_handler() };
        assert!(h.is_some());
        assert!(p.is_null());
        *CAP.lock().unwrap() = None;
        blog(LOG_INFO, "to the default handler");
        assert!(CAP.lock().unwrap().is_none());
    }

    // the Rust wrapper passes the message through as "%s" (embedded-NUL
    // caveat is C behaviour, not asserted)
    #[test]
    fn blog_wrapper_formats() {
        let _lock = TEST_LOCK.lock().unwrap();
        let mut sentinel = 0u8;
        // SAFETY: the C log-handler setter; `capture` is a valid C function pointer for the process lifetime (test).
        unsafe { base_set_log_handler(Some(capture), &mut sentinel as *mut _ as *mut c_void) };
        blog(LOG_ERROR, "plain message");
        let (lvl, fmt, _) = CAP.lock().unwrap().take().expect("handler ran");
        assert_eq!(lvl, LOG_ERROR);
        assert_eq!(fmt, "%s");
        // SAFETY: the C log-handler reset (the documented NULL no-op case).
        unsafe { base_set_log_handler(None, std::ptr::null_mut()) };
    }
}
