//! P1-01.3 recipe step 2 (WP-43): the C ABI for `libobs/util/bmem.c` —
//! fully ported to [`obs_util::bmem`] (S-001).
//!
//! With `OBS_ENABLE_RUST=ON` the C file is excluded from the build
//! (`test/conformance/obs-enable-rust.patch`); these exports provide its
//! six symbols to the rest of libobs and to the conformance runners.
//! They are also the allocator every other bridge allocates through in a
//! swap build (the [`crate::abi_common`] externs resolve to these
//! symbols inside the staticlib).
//!
//! Crash routing: on first use the Rust fatal paths (zero-size
//! `bmalloc`/`brealloc`, OOM) are wired to the C shim's `bcrash`
//! (base-logging.c), so a swapped build crashes through the C-installed
//! crash handler exactly like the C reference (the [`crate::abi_platform`]
//! fatal-hook pattern). The fatal messages are fixed literals without
//! `%`, the same precondition the C callers have.

use std::os::raw::{c_char, c_int, c_long, c_void};
use std::sync::Once;

use obs_util::bmem;

/// Route the Rust bmem fatal paths through the C shim's `bcrash` once
/// (the C handler is installed by the runner before any fatal path can
/// run; the wiring is idempotent and happens on the first bmem call).
static WIRED: Once = Once::new();

fn ensure_wired() {
    WIRED.call_once(|| {
        bmem::set_bcrash_sink(|msg| {
            let msg = format!("{msg}\0");
            // SAFETY: `bcrash` is the C shim (base-logging.c, always compiled into this staticlib); the NUL-terminated format contains no `%` (the fatal messages are fixed literals).
            unsafe { bcrash(msg.as_ptr().cast()) };
        });
    });
}

unsafe extern "C" {
    /// The C shim's `bcrash(format, ...)` (base-logging.c).
    fn bcrash(format: *const c_char);
}

/// C `bmalloc(size)` (S-001-R01/R02): 32-byte-aligned block, never NULL;
/// zero size is fatal.
///
/// # Safety
/// C-ABI — the result is freed with `bfree`/`brealloc` (S-001-R05).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bmalloc(size: usize) -> *mut c_void {
    ensure_wired();
    // SAFETY: forwarded contract; the returned pointer is stored by the C caller.
    unsafe { bmem::bmalloc_raw(size) as *mut c_void }
}

/// C `brealloc(ptr, size)` (S-001-R06–R08): NULL acts like `bmalloc`;
/// zero size is fatal; the first `min(old, new)` bytes are preserved.
///
/// # Safety
/// C-ABI — `ptr` is NULL or a live `bmalloc`/`brealloc` block (S-001-R05).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn brealloc(ptr: *mut c_void, size: usize) -> *mut c_void {
    ensure_wired();
    // SAFETY: forwarded contract; the result (or NULL input) follows the C bmem rules (checked in the implementation).
    unsafe { bmem::brealloc_raw(ptr as *mut u8, size) as *mut c_void }
}

/// C `bfree(ptr)` (S-001-R05): NULL is a no-op.
///
/// # Safety
/// C-ABI — `ptr` is NULL or a live `bmalloc`/`brealloc`/`bmemdup` block.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bfree(ptr: *mut c_void) {
    // SAFETY: forwarded contract (the NULL no-op is checked in the implementation).
    unsafe { bmem::bfree_raw(ptr as *mut u8) }
}

/// C `bmemdup(ptr, size)` (S-001-R11): `bmalloc` + copy; zero size is
/// fatal via the `bmalloc` message.
///
/// # Safety
/// C-ABI — `ptr` is valid for `size` reads (or `size == 0`); the result
/// is freed with `bfree`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bmemdup(ptr: *const c_void, size: usize) -> *mut c_void {
    ensure_wired();
    // SAFETY: forwarded contract; the fresh block is valid for the copy (checked in the implementation).
    unsafe { bmem::bmemdup_raw(ptr as *const u8, size) as *mut c_void }
}

/// C `bnum_allocs()` (S-001-R09): number of live blocks.
#[unsafe(no_mangle)]
pub extern "C" fn bnum_allocs() -> c_long {
    bmem::bnum_allocs() as c_long
}

/// C `base_get_alignment()` (S-001-R10): 32 on every platform.
#[unsafe(no_mangle)]
pub extern "C" fn base_get_alignment() -> c_int {
    bmem::base_get_alignment() as c_int
}
