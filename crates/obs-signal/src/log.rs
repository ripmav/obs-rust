//! C logging for the ported `callback/` sources (the swap build).
//!
//! The C reference logs through `blog(level, format, ...)`; the conformance
//! harness captures those lines (`log <level> <message>` on stdout), so the
//! swapped (Rust) paths must emit through the same sink. The port keeps a
//! process-global sink — set once by the FFI bridge in the swap build —
//! and falls back to stderr (the pre-swap behaviour, unobservable in the
//! vectors) when no sink is installed.

use std::ffi::c_char;
use std::sync::atomic::{AtomicUsize, Ordering};

/// S-001 §3: the C `blog` levels (lower numbers are more severe).
pub const LOG_ERROR: i32 = 100;
/// S-001 §3.
pub const LOG_WARNING: i32 = 200;

/// The C sink: `void fn(level, const char *msg)` — the FFI bridge forwards
/// to `blog(level, "%s", msg)`.
type CLogFn = unsafe extern "C" fn(level: i32, msg: *const c_char);

static LOG_FN: AtomicUsize = AtomicUsize::new(0);

/// Install (or remove) the C logging sink. Idempotent; the bridge calls it
/// once per process.
///
/// # Safety
/// `f` must be a valid `CLogFn` (the bridge's `blog` forwarder) or None.
pub unsafe fn set_log_sink(f: Option<CLogFn>) {
    LOG_FN.store(f.map(|f| f as usize).unwrap_or(0), Ordering::Release);
}

/// One `blog(level, "%s", msg)` of the C reference.
pub(crate) fn emit(level: i32, msg: &str) {
    let raw = LOG_FN.load(Ordering::Acquire);
    if raw == 0 {
        eprintln!("{msg}");
        return;
    }
    // SAFETY: `raw` was stored by `set_log_sink` from a `CLogFn`.
    let f = unsafe { std::mem::transmute::<usize, CLogFn>(raw) };
    // The sink takes a NUL-terminated string; the allocation lives for the
    // duration of the (synchronous) sink call. A stray NUL would truncate
    // the log line (the C format string would stop there too).
    let c = std::ffi::CString::new(msg).unwrap_or_else(|_| std::ffi::CString::new("").unwrap());
    // SAFETY: `f` is the bridge's forwarder (set via `set_log_sink`); `c` is live for the call.
    unsafe { (f as CLogFn)(level, c.as_ptr()) };
}
