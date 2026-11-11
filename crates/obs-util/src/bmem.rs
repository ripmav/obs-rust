//! Process-wide allocator and fatal-error (crash) hooks (spec S-001).
//!
//! This module ports `libobs/util/bmem.*` (+ the crash half of `base.c`)
//! per `docs/rust-rewrite/specs/S-001-bmem-base.md`. The `blog`/log-level
//! half of `base.c` arrives with P1-13.
//!
//! Conventions (S-001 §2): `ALIGNMENT` = 32. `bmalloc`/`brealloc` never
//! return NULL: zero size and OOM are fatal (R02, R03, R07). The C-visible
//! `extern "C"` shim (P1-01 step 2) and `OBS_ENABLE_RUST` build switch
//! (step 5) live in `obs-ffi` / the C build and are tracked per WP — they
//! need the C checkout.

use std::alloc::{Layout, alloc, dealloc};
use std::cell::UnsafeCell;
use std::process::exit;
use std::sync::atomic::{AtomicBool, AtomicIsize, AtomicUsize, Ordering};

/// S-001-R10: alignment of every block on every platform.
pub const ALIGNMENT: usize = 32;

/// Header size reserved before every data pointer. Layout:
/// `[ 8 bytes: original size | 24 bytes: pad ] data...`
/// The block itself is 32-aligned, so `block + 32` is 32-aligned and
/// `bfree`/`brealloc` recover everything from the user pointer alone
/// (S-001 §4.3 / §7).
const HEADER: usize = ALIGNMENT;

/// Live-block counter (S-001-R04..R09, R26). Atomic per Q-1.
static NUM_ALLOCS: AtomicIsize = AtomicIsize::new(0);

/// S-001-R24/Q-2: "crash in progress" flag (atomic, MAY per spec).
static CRASHING: AtomicBool = AtomicBool::new(false);

/// S-001-R25: crash handler installed (at most once per process).
static CRASH_HANDLER_SET: AtomicBool = AtomicBool::new(false);

/// Installed crash handler, `fn(formatted_msg, param)`.
type CrashHandler = fn(&str, usize);

struct HandlerCell(UnsafeCell<CrashHandler>);

// SAFETY: written at most once, guarded by CRASH_HANDLER_SET (R25); reads
// only race with that single start-up-time write, and a crash terminates
// the process.
unsafe impl Sync for HandlerCell {}

static CRASH_HANDLER: HandlerCell = HandlerCell(UnsafeCell::new(default_crash_handler));
static CRASH_PARAM: AtomicUsize = AtomicUsize::new(0);

/// S-001-R10: `base_get_alignment()` returns 32 on every platform.
#[must_use]
pub fn base_get_alignment() -> usize {
    ALIGNMENT
}

/// S-001-R09: number of live blocks. Reads need not be synchronised with
/// concurrent updates.
#[must_use]
pub fn bnum_allocs() -> i64 {
    NUM_ALLOCS.load(Ordering::Relaxed) as i64
}

/// S-001-R24: default crash handler — formatted message to stderr
/// **without a trailing newline**, then `exit(0)`.
fn default_crash_handler(msg: &str, _param: usize) {
    use std::io::Write;
    let _ = std::io::stderr().write_all(msg.as_bytes());
    exit(0);
}

/// S-001-R24/R25: `bcrash` — never returns.
///
/// - Crash already in progress → `"Crashed in the crash handler"` to stderr,
///   `exit(2)`.
/// - Otherwise mark crashing, call the installed (or default) handler with
///   the already-formatted message; if the handler returns, `exit(0)`.
///
/// Callers pass the final formatted string (the C `va_list` form stays in
/// the `obs-ffi` shim, S-001 §7).
#[cold]
pub fn bcrash(msg: &str) -> ! {
    if CRASHING.swap(true, Ordering::SeqCst) {
        use std::io::Write;
        let _ = std::io::stderr().write_all(b"Crashed in the crash handler");
        exit(2);
    }
    // SAFETY: see HandlerCell Sync impl — single writer guarded by R25.
    let handler = unsafe { *CRASH_HANDLER.0.get() };
    let param = CRASH_PARAM.load(Ordering::Relaxed);
    handler(msg, param);
    exit(0);
}

/// S-001-R25: install the crash handler — succeeds **at most once** per
/// process. A second call logs the MUST warning (R23 default-handler
/// behaviour for `LOG_WARNING`; the `blog` facade arrives with P1-13) and
/// is ignored. `h == None` is rejected the same way (the C API has no
/// "restore default"; the default is what is stored until the first call).
pub fn base_set_crash_handler(h: Option<CrashHandler>, param: usize) {
    if CRASH_HANDLER_SET.swap(true, Ordering::SeqCst) {
        use std::io::Write;
        let _ = std::io::stdout()
            .write_all(b"warning: Tried to set a crash handler when one already exists.\n");
        return;
    }
    if let Some(h) = h {
        // SAFETY: see HandlerCell Sync impl.
        unsafe { *CRASH_HANDLER.0.get() = h };
        CRASH_PARAM.store(param, Ordering::Relaxed);
    }
}

/// Test hook: has a crash handler been installed? (S-001-R25 once-only.)
#[doc(hidden)]
#[must_use]
pub fn __crash_handler_installed_for_test() -> bool {
    CRASH_HANDLER_SET.load(Ordering::SeqCst)
}

// ── Allocation core (S-001 R01–R13) ─────────────────────────────────────────

/// S-001-R02: zero size is a fatal error (message is MUST — crash reports).
const ZERO_BMALLOC: &str = "bmalloc: Allocating 0 bytes is broken behavior, please fix your code!";
/// S-001-R07: zero size in brealloc is a fatal error.
const ZERO_BREALLOC: &str =
    "brealloc: Allocating 0 bytes is broken behavior, please fix your code!";

#[cold]
fn oom_crash(size: usize) -> ! {
    // S-001-R03: `os_oom()` hook MAY be a no-op (S-001 §4.3); message is MUST.
    bcrash(&format!(
        "Out of memory while trying to allocate {size} bytes"
    ))
}

/// S-001-R01: 32-byte-aligned pointer to at least `size` writable bytes,
/// contents unspecified. `bmalloc(0)` is fatal (R02); OOM is fatal (R03).
/// Increments the live-block counter (R04). Never returns NULL.
///
/// # Safety
/// The caller must only pass the result to `bfree_raw`/`brealloc_raw` and
/// keep the returned pointer uniquely owned until freed (R05).
#[must_use]
pub unsafe fn bmalloc_raw(size: usize) -> *mut u8 {
    if size == 0 {
        bcrash(ZERO_BMALLOC)
    }
    let layout = Layout::from_size_align(HEADER + size, ALIGNMENT).expect("valid layout");
    let block = unsafe { alloc(layout) };
    if block.is_null() {
        oom_crash(size)
    }
    unsafe {
        // Store the original size in the header; data starts at +HEADER.
        (block as *mut usize).write(size);
    }
    NUM_ALLOCS.fetch_add(1, Ordering::Relaxed);
    unsafe { block.add(HEADER) }
}

/// S-001-R05: `bfree(NULL)` is a no-op; otherwise releases the block and
/// decrements the counter. Only pointers from this allocator are valid.
///
/// # Safety
/// `ptr` must be NULL or a live pointer previously returned by
/// `bmalloc_raw`/`brealloc_raw`/`bmemdup_raw`/`bzalloc`/`bstrdup*`.
pub unsafe fn bfree_raw(ptr: *mut u8) {
    if ptr.is_null() {
        return;
    }
    let block = unsafe { ptr.sub(HEADER) };
    let size = unsafe { (block as *const usize).read() };
    let layout = Layout::from_size_align(HEADER + size, ALIGNMENT).expect("valid layout");
    unsafe { dealloc(block, layout) };
    NUM_ALLOCS.fetch_sub(1, Ordering::Relaxed);
}

/// S-001-R06–R08: `brealloc(NULL, size)` behaves like `bmalloc(size)`
/// (counter incremented *before* the zero-size check); `brealloc(p, 0)`
/// (incl. `p == NULL`) is fatal (R07); otherwise returns a 32-aligned
/// pointer whose first `min(old, new)` bytes equal the old contents and the
/// counter is unchanged (R08). OOM → R03 text.
///
/// # Safety
/// As `bmalloc_raw`/`bfree_raw`.
#[must_use]
pub unsafe fn brealloc_raw(ptr: *mut u8, size: usize) -> *mut u8 {
    if ptr.is_null() {
        // R06: increment before the R07 zero-size check.
        NUM_ALLOCS.fetch_add(1, Ordering::Relaxed);
        if size == 0 {
            bcrash(ZERO_BREALLOC)
        }
        let p = unsafe { bmalloc_raw(size) };
        // bmalloc_raw already incremented; R06's pre-increment is the same
        // single increment for the fresh block (net +1 overall).
        NUM_ALLOCS.fetch_sub(1, Ordering::Relaxed);
        return p;
    }
    if size == 0 {
        bcrash(ZERO_BREALLOC)
    }
    let block = unsafe { ptr.sub(HEADER) };
    let old_size = unsafe { (block as *const usize).read() };
    let layout = Layout::from_size_align(HEADER + size, ALIGNMENT).expect("valid layout");
    let new_block = unsafe { alloc(layout) };
    if new_block.is_null() {
        oom_crash(size)
    }
    unsafe {
        std::ptr::copy_nonoverlapping(ptr, new_block.add(HEADER), size.min(old_size));
        (new_block as *mut usize).write(size);
        dealloc(
            block,
            Layout::from_size_align(HEADER + old_size, ALIGNMENT).expect("valid layout"),
        );
    }
    // R08: counter unchanged.
    unsafe { new_block.add(HEADER) }
}

// ── Dup helpers + safe handle (S-001 R11–R13) ───────────────────────────────

/// S-001-R11: `bmemdup(p, size)` = `bmalloc(size)` then copy `size` bytes
/// (size 0 → fatal via the bmalloc text). Header-inline in C; exported here
/// for the Rust side and the future `obs-ffi` shim.
///
/// # Safety
/// `src` must be valid for `size` reads (or `size == 0`).
#[must_use]
pub unsafe fn bmemdup_raw(src: *const u8, size: usize) -> *mut u8 {
    let dst = unsafe { bmalloc_raw(size) };
    if size != 0 {
        unsafe { std::ptr::copy_nonoverlapping(src, dst, size) };
    }
    dst
}

/// A safe RAII handle to bmem-allocated memory — the Rust analogue of the
/// C++ `BPtr` (S-001-R29). Frees via `bfree` on drop.
#[derive(Debug)]
pub struct BMem {
    ptr: *mut u8,
    len: usize,
}

impl BMem {
    /// Consume and return the raw pointer (caller takes ownership and must
    /// free via `bfree_raw`).
    #[must_use]
    pub fn into_raw(self) -> *mut u8 {
        let p = self.ptr;
        std::mem::forget(self);
        p
    }

    /// Re-wrap a live bmem pointer.
    ///
    /// # Safety
    /// `ptr` must be a live pointer from this allocator (or the process
    /// will double-free at drop).
    #[must_use]
    pub const unsafe fn from_raw(ptr: *mut u8, len: usize) -> Self {
        Self { ptr, len }
    }

    #[must_use]
    pub const fn as_ptr(&self) -> *const u8 {
        self.ptr
    }

    #[must_use]
    pub const fn as_mut_ptr(&mut self) -> *mut u8 {
        self.ptr
    }
}

impl std::ops::Deref for BMem {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        unsafe { std::slice::from_raw_parts(self.ptr, self.len) }
    }
}

impl std::ops::DerefMut for BMem {
    fn deref_mut(&mut self) -> &mut [u8] {
        unsafe { std::slice::from_raw_parts_mut(self.ptr, self.len) }
    }
}

impl Drop for BMem {
    fn drop(&mut self) {
        unsafe { bfree_raw(self.ptr) };
    }
}

/// Safe `bmemdup` (R11) over a slice.
#[must_use]
pub fn bmemdup(src: &[u8]) -> BMem {
    unsafe { BMem::from_raw(bmemdup_raw(src.as_ptr(), src.len()), src.len()) }
}

/// S-001-R12: `bzalloc(size)` = `bmalloc(size)` + zero-fill; 0 is fatal.
#[must_use]
pub fn bzalloc(size: usize) -> BMem {
    let p = unsafe { bmalloc_raw(size) };
    if size != 0 {
        unsafe { p.write_bytes(0, size) };
    }
    unsafe { BMem::from_raw(p, size) }
}

/// S-001-R13 (byte form): copies `n` bytes and NUL-terminates at `n`
/// (`bmemdup(str, n + 1)` then `d[n] = 0`). The C `NULL → NULL` case has
/// no safe analogue; the wide (`wchar_t`) variants live in the `obs-ffi`
/// shim. Panics when `n > src.len()` (the C contract's caller guarantee).
#[must_use]
pub fn bstrdup_n(src: &[u8], n: usize) -> BMem {
    assert!(
        n <= src.len(),
        "bstrdup_n: n must be <= src len (S-001-R13)"
    );
    let mut d = bzalloc(n + 1);
    d[..n].copy_from_slice(&src[..n]);
    d
}

/// S-001-R13: `bstrdup(str)` = `bstrdup_n(str, strlen(str))`.
#[must_use]
pub fn bstrdup(src: &[u8]) -> BMem {
    bstrdup_n(src, src.len())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Mutex;

    /// Serialise tests that read the process-wide counter (parallel tests
    /// would race each other's snapshots).
    static COUNTER_LOCK: Mutex<()> = Mutex::new(());

    static GOT: AtomicUsize = AtomicUsize::new(0);

    fn rec_handler(_msg: &str, param: usize) {
        GOT.store(param, Ordering::Relaxed);
    }

    /// RAII guard so every test leaves the counter exactly as found.
    struct Guard(*mut u8);
    impl Drop for Guard {
        fn drop(&mut self) {
            unsafe { bfree_raw(self.0) };
        }
    }
    fn alloc_guard(size: usize) -> Guard {
        Guard(unsafe { bmalloc_raw(size) })
    }

    #[test]
    fn alignment_is_32() {
        // S-001-R10
        assert_eq!(base_get_alignment(), 32);
        assert_eq!(ALIGNMENT, 32);
    }

    #[test]
    fn counter_starts_consistent() {
        // S-001-R09 (fresh process: no allocations yet in this crate)
        assert!(bnum_allocs() >= 0);
    }

    #[test]
    fn crash_handler_install_is_once() {
        // S-001-R25: first call stores, second is ignored (warning printed).
        assert!(!__crash_handler_installed_for_test());
        base_set_crash_handler(Some(rec_handler), 7);
        assert!(__crash_handler_installed_for_test());
        GOT.store(0, Ordering::Relaxed);
        base_set_crash_handler(Some(rec_handler), 8); // ignored
        // The stored handler is still the first one: a crash would record 7.
        // (Crash itself is verified in tests/bmem_fatal.rs, subprocess.)
        let _ = rec_handler;
        assert_eq!(CRASH_PARAM.load(Ordering::Relaxed), 7);
    }

    #[test]
    fn blocks_are_32_aligned() {
        let _lock = COUNTER_LOCK.lock().unwrap();
        // S-001-R01
        for size in [1usize, 7, 31, 32, 33, 1000, 4096, 1 << 20] {
            let p = alloc_guard(size);
            assert_eq!(p.0 as usize % ALIGNMENT, 0, "size {size}");
        }
    }

    #[test]
    fn counter_tracks_alloc_free() {
        let _lock = COUNTER_LOCK.lock().unwrap();
        // S-001-R04/R05/R09
        let base = bnum_allocs();
        {
            let a = alloc_guard(10);
            let b = alloc_guard(100);
            let c = alloc_guard(1000);
            assert_eq!(bnum_allocs(), base + 3);
            std::mem::drop(c);
            assert_eq!(bnum_allocs(), base + 2);
            let _ = (a, b);
        }
        assert_eq!(bnum_allocs(), base);
    }

    #[test]
    fn bfree_null_is_noop() {
        let _lock = COUNTER_LOCK.lock().unwrap();
        // S-001-R05
        let base = bnum_allocs();
        unsafe { bfree_raw(std::ptr::null_mut()) };
        assert_eq!(bnum_allocs(), base);
    }

    #[test]
    fn brealloc_null_is_alloc() {
        let _lock = COUNTER_LOCK.lock().unwrap();
        // S-001-R06: net counter effect +1, aligned result.
        let base = bnum_allocs();
        let p = unsafe { brealloc_raw(std::ptr::null_mut(), 64) };
        assert_eq!(p as usize % ALIGNMENT, 0);
        assert_eq!(bnum_allocs(), base + 1);
        unsafe { bfree_raw(p) };
        assert_eq!(bnum_allocs(), base);
    }

    #[test]
    fn brealloc_grows_preserving_prefix() {
        let _lock = COUNTER_LOCK.lock().unwrap();
        // S-001-R08: first min(old,new) bytes preserved, counter unchanged.
        let base = bnum_allocs();
        let old = unsafe { bmalloc_raw(16) };
        unsafe {
            for i in 0..16 {
                *old.add(i) = i.wrapping_mul(7) as u8;
            }
        }
        let new = unsafe { brealloc_raw(old, 256) };
        assert_eq!(new as usize % ALIGNMENT, 0);
        assert_eq!(bnum_allocs(), base + 1); // unchanged by realloc (one live block)
        unsafe {
            for i in 0..16usize {
                assert_eq!(*new.add(i), i.wrapping_mul(7) as u8);
            }
            bfree_raw(new);
        }
    }

    #[test]
    fn brealloc_shrinks_preserving_prefix() {
        let _lock = COUNTER_LOCK.lock().unwrap();
        // S-001-R08: shrink keeps the first `new` bytes.
        let base = bnum_allocs();
        let old = unsafe { bmalloc_raw(256) };
        unsafe {
            for i in 0..256usize {
                *old.add(i) = i as u8;
            }
        }
        let new = unsafe { brealloc_raw(old, 32) };
        assert_eq!(bnum_allocs(), base + 1); // unchanged by realloc (one live block)
        unsafe {
            for i in 0..32usize {
                assert_eq!(*new.add(i), i as u8);
            }
            bfree_raw(new);
        }
    }
    #[test]
    fn bmemdup_copies_bytes() {
        // S-001-R11
        let _lock = COUNTER_LOCK.lock().unwrap();
        let base = bnum_allocs();
        let src = [1u8, 2, 3, 4, 5];
        let d = bmemdup(&src);
        assert_eq!(d.len(), 5);
        assert_eq!(d[..], src);
        assert_eq!(bnum_allocs(), base + 1);
    }

    #[test]
    fn bzalloc_zero_fills_and_counts() {
        // S-001-R12
        let _lock = COUNTER_LOCK.lock().unwrap();
        let base = bnum_allocs();
        let z = bzalloc(64);
        assert!(z.iter().all(|&b| b == 0));
        assert_eq!(bnum_allocs(), base + 1);
    }

    #[test]
    fn bstrdup_nul_terminates() {
        // S-001-R13
        let _lock = COUNTER_LOCK.lock().unwrap();
        let base = bnum_allocs();
        let s = b"hola";
        let d = bstrdup(s);
        assert_eq!(d.len(), 5);
        assert_eq!(&d[..4], s);
        assert_eq!(d[4], 0);
        let n = bstrdup_n(s, 2);
        assert_eq!(n.len(), 3);
        assert_eq!(&n[..2], &s[..2]);
        assert_eq!(n[2], 0);
        assert_eq!(bnum_allocs(), base + 2);
    }

    #[test]
    fn bmem_raw_roundtrip_keeps_counter() {
        // S-001-R05/R09: into_raw/from_raw is a no-op for the counter.
        let _lock = COUNTER_LOCK.lock().unwrap();
        let base = bnum_allocs();
        let d = bstrdup(b"abc");
        let raw = d.into_raw();
        assert_eq!(bnum_allocs(), base + 1);
        let back = unsafe { BMem::from_raw(raw, 4) };
        assert_eq!(&back[..], b"abc\0");
        drop(back);
        assert_eq!(bnum_allocs(), base);
    }
}
