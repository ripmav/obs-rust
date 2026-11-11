//! Signal handlers: named events with typed parameters (S-004;
//! `libobs/callback/signal.c`).
//!
//! All APIs take `&self` (interior mutability) so that a callback can call
//! any of this API on the same handler mid-emit (S-004-R44) by stashing
//! `handler.as_ptr()` in its `data` and recovering a shared reference —
//! the C reference does the same with raw pointers.
//!
//! Lock shape (as in C): each list is a plain field guarded by its own
//! `ReentrantMutex<()>` (signal / global) or `Mutex<()>` (handler).
//! List access goes through helpers that derive a fresh borrow, read or
//! write, and release it before returning — **no borrow is held across a
//! callback invocation** (S-004 §9), so nested re-entrancy (R50) is
//! well-defined and Miri-clean.
//!
//! Safe deviations from the C reference (documented, all "safer than C"):
//! - A handler whose refs reach 0 **during an emit** is marked dead and
//!   its lists are cleared when the outermost emit finishes; later calls
//!   are no-ops (C would dereference freed memory).
//! - `signal_handler_remove_current` resolves the stored
//!   `(handler, signal index, cb index, generation)` instead of a raw
//!   pointer into darray memory (C is UB after a realloc/free; the
//!   generation check makes this well-defined while reproducing Q-2).
//! - `connect_ref` on a nonexistent signal logs (S-004-R20) and leaves
//!   `refs` unchanged (balanced; the C reference may leak a ref there).

use std::cell::Cell;
use std::cell::UnsafeCell;
use std::ffi::{CStr, c_char, c_void};
use std::ptr::NonNull;
use std::sync::Mutex;
use std::sync::atomic::{AtomicBool, AtomicU64, AtomicUsize, Ordering::SeqCst};

use parking_lot::ReentrantMutex;

use crate::calldata::Calldata;
use crate::decl::parse_decl;

/// `signal_callback_t`: `void (*cb)(void *data, calldata_t *params)`.
pub type SignalCallback = unsafe extern "C" fn(data: *mut c_void, params: *mut Calldata);
/// `global_signal_callback_t`.
pub type GlobalSignalCallback =
    unsafe extern "C" fn(data: *mut c_void, signal: *const u8, params: *mut Calldata);

// ── Thread-local "current callback" (S-004 §4) ──────────────────────────

#[derive(Clone, Copy)]
struct Current {
    handler: NonNull<SignalHandler>,
    sig_idx: usize,
    entry: usize,
    generation: u64,
}

#[derive(Clone, Copy)]
struct CurrentGlobal {
    handler: NonNull<SignalHandler>,
    entry: usize,
    generation: u64,
}

thread_local! {
    /// The Cb currently running on this thread (or none). Q-2: cleared
    /// after *any* callback returns, including nested ones.
    static CURRENT_CB: Cell<Option<Current>> = const { Cell::new(None) };
    static CURRENT_GLOBAL: Cell<Option<CurrentGlobal>> = const { Cell::new(None) };
}

// ── Records ─────────────────────────────────────────────────────────────

// The `Vec<Box<…>>` below is deliberate (clippy::vec_box): each entry's
// allocation must survive Vec reallocations so that (a) entries appended
// during an emit stay reachable by index (Q-1), (b) `*mut Signal` stays
// valid while the handler lock is released (S-004 §7), and (c) the
// thread-local current-cb (handler, index, generation) stays resolvable.
#[allow(clippy::vec_box)]
struct Cb {
    cb: SignalCallback,
    data: *mut c_void,
    remove: bool,
    keep_ref: bool,
    generation: u64,
}

struct GlobalCb {
    cb: GlobalSignalCallback,
    data: *mut c_void,
    signaling: u32,
    remove: bool,
    generation: u64,
}

#[allow(clippy::vec_box)]
struct Signal {
    name: String,
    /// `Vec<Box<Cb>>`: each entry's allocation is stable across pushes
    /// (Q-1 append-during-emit). Guarded by `lock`.
    cbs: Vec<Box<Cb>>,
    /// RECURSIVE lock (S-004-R50); guards `cbs` and `signalling`.
    lock: ReentrantMutex<()>,
    /// Boolean, **not** a counter (Q-3).
    signalling: bool,
}

#[allow(clippy::vec_box)]
struct HandlerState {
    /// Insertion order; looked up by name. The `UnsafeCell` is the
    /// documented escape hatch for mutating through `&self` while the
    /// handler lock is held (stacked-borrows-clean).
    #[allow(clippy::vec_box)]
    signals: UnsafeCell<Vec<Box<Signal>>>,
}

#[allow(clippy::vec_box)]
struct GlobalState {
    /// Guarded by `SignalHandler::global_lock`.
    #[allow(clippy::vec_box)]
    globals: UnsafeCell<Vec<Box<GlobalCb>>>,
}

struct Inner {
    /// Non-recursive handler lock (S-004 §7: never held while callbacks
    /// run); guards `state`.
    handler_lock: Mutex<()>,
    state: HandlerState,
    /// RECURSIVE global lock (held during the global pass of an emit);
    /// guards `globals`.
    global_lock: ReentrantMutex<()>,
    globals: GlobalState,
    /// S-004 refs; starts at 1 (SeqCst per S-004 §11).
    refs: AtomicUsize,
    /// Generation counter for Cb/GlobalCb records.
    generation: AtomicU64,
    /// Emits in progress on this handler. Used to defer freeing of a
    /// dead handler (module docs).
    emitting: AtomicUsize,
    /// Refs reached 0 (destroyed).
    dead: AtomicBool,
}

/// `signal_handler_t` (opaque in C).
pub struct SignalHandler {
    inner: Inner,
}

// ── List helpers (fresh borrow per call; never held across a callback) ──

unsafe fn cb_count(sp: *const Signal) -> usize {
    let s = unsafe { &*sp };
    let _g = s.lock.lock();
    s.cbs.len()
}

unsafe fn cb_at(sp: *const Signal, i: usize) -> Option<(SignalCallback, *mut c_void, u64)> {
    let s = unsafe { &*sp };
    let _g = s.lock.lock();
    s.cbs
        .get(i)
        .filter(|c| !c.remove)
        .map(|c| (c.cb, c.data, c.generation))
}

unsafe fn cb_mark(sp: *mut Signal, i: usize, generation: u64) -> bool {
    let s = unsafe { &mut *sp };
    let _g = s.lock.lock();
    if i < s.cbs.len() && s.cbs[i].generation == generation {
        s.cbs[i].remove = true;
        true
    } else {
        false
    }
}

/// Erase entry `i` if marked `remove`; returns `true` if it was a
/// `keep_ref` entry.
unsafe fn cb_erase_marked(sp: *mut Signal, i: usize) -> bool {
    let s = unsafe { &mut *sp };
    let _g = s.lock.lock();
    if i < s.cbs.len() && s.cbs[i].remove {
        let keep = s.cbs[i].keep_ref;
        s.cbs.remove(i);
        keep
    } else {
        false
    }
}

unsafe fn cb_set_signalling(sp: *mut Signal, v: bool) {
    let s = unsafe { &mut *sp };
    let _g = s.lock.lock();
    s.signalling = v;
}

// ── S-004-R01/R02: lifecycle ────────────────────────────────────────────

/// S-004-R01: `signal_handler_create` — `refs == 1`, no signals/globals.
#[must_use]
pub fn signal_handler_create() -> Box<SignalHandler> {
    Box::new(SignalHandler {
        inner: Inner {
            handler_lock: Mutex::new(()),
            state: HandlerState {
                signals: UnsafeCell::new(vec![]),
            },
            global_lock: ReentrantMutex::new(()),
            globals: GlobalState {
                globals: UnsafeCell::new(vec![]),
            },
            refs: AtomicUsize::new(1),
            generation: AtomicU64::new(1),
            emitting: AtomicUsize::new(0),
            dead: AtomicBool::new(false),
        },
    })
}

/// S-004-R02: decrements `refs`; frees everything only when it reaches 0.
/// Returns `true` if the handler is still alive (and fully usable).
#[must_use]
pub fn signal_handler_destroy(h: &SignalHandler) -> bool {
    if h.inner.refs.fetch_sub(1, SeqCst) == 1 {
        h.inner.destroy();
        false
    } else {
        true
    }
}

impl Inner {
    /// Free the state, but only when no emit is in progress; otherwise
    /// mark dead (the outermost emit's epilogue clears the lists).
    fn destroy(&self) {
        if self.emitting.load(SeqCst) == 0 {
            let _hl = self.handler_lock.lock().unwrap();
            unsafe { (*self.state.signals.get()).clear() };
            let _gl = self.global_lock.lock();
            unsafe { (*self.globals.globals.get()).clear() };
        }
        self.dead.store(true, SeqCst);
    }

    fn maybe_free_dead(&self) {
        if self.emitting.fetch_sub(1, SeqCst) == 1 && self.dead.load(SeqCst) {
            let _hl = self.handler_lock.lock().unwrap();
            unsafe { (*self.state.signals.get()).clear() };
            let _gl = self.global_lock.lock();
            unsafe { (*self.globals.globals.get()).clear() };
        }
    }
}

// ── S-004-R10–R12: declaring signals ───────────────────────────────────

/// S-004-R10/R11: add a signal from its declaration (S-003).
#[must_use]
pub fn signal_handler_add(h: &SignalHandler, signal_decl: &str) -> bool {
    if h.inner.dead.load(SeqCst) {
        return false;
    }
    let Ok(decl) = parse_decl(signal_decl) else {
        eprintln!("Signal declaration invalid: {signal_decl}");
        return false;
    };
    let name = decl.name.clone();
    {
        let _hl = h.inner.handler_lock.lock().unwrap();
        let signals = unsafe { &*h.inner.state.signals.get() };
        if signals.iter().any(|s| s.name == name) {
            eprintln!("Signal declaration '{name}' exists");
            return false;
        }
        unsafe {
            (*h.inner.state.signals.get()).push(Box::new(Signal {
                name,
                cbs: vec![],
                lock: ReentrantMutex::new(()),
                signalling: false,
            }))
        };
    }
    true
}

/// `signal_handler_add_array` (header inline): `NULL` array → `false`;
/// adds each declaration until the `NULL` terminator, returning `false`
/// if any `add` failed (continuing after a failure).
///
/// # Safety
///
/// `decls` must be a valid, NUL-terminated array of C strings (or null).
#[must_use]
pub unsafe fn signal_handler_add_array(h: &SignalHandler, decls: *const *const u8) -> bool {
    if decls.is_null() {
        return false;
    }
    let mut ok = true;
    let mut i = 0usize;
    loop {
        let p = unsafe { *decls.add(i) };
        if p.is_null() {
            break;
        }
        let bytes = unsafe { CStr::from_ptr(p as *const c_char) }.to_bytes();
        if !signal_handler_add(h, std::str::from_utf8(bytes).unwrap_or("")) {
            ok = false;
        }
        i += 1;
    }
    ok
}

// ── S-004-R20–R23: connecting ───────────────────────────────────────────

fn find_signal(h: &SignalHandler, sig: &str) -> Option<*mut Signal> {
    if h.inner.dead.load(SeqCst) {
        return None;
    }
    let _hl = h.inner.handler_lock.lock().unwrap();
    let signals = unsafe { &*h.inner.state.signals.get() };
    signals
        .iter()
        .position(|s| s.name == sig)
        .map(|i| Box::as_ptr(&signals[i]).cast_mut())
}

/// S-004-R20/R21: connect (dedupes by (cb, data); entries marked
/// `remove` still count — Q-4).
pub fn signal_handler_connect(h: &SignalHandler, sig: &str, cb: SignalCallback, data: *mut c_void) {
    let Some(sp) = find_signal(h, sig) else {
        eprintln!("signal_handler_connect: signal '{sig}' not found");
        return;
    };
    let generation = h.inner.generation.fetch_add(1, SeqCst);
    unsafe {
        let s = &mut *sp;
        let _sl = s.lock.lock();
        if s.cbs
            .iter()
            .any(|c| std::ptr::fn_addr_eq(c.cb, cb) && c.data == data)
        {
            return;
        }
        s.cbs.push(Box::new(Cb {
            cb,
            data,
            remove: false,
            keep_ref: false,
            generation,
        }));
    }
}

/// S-004-R22: connect_ref — increments `refs` and always appends
/// (`keep_ref = true`), even for an existing identity.
pub fn signal_handler_connect_ref(
    h: &SignalHandler,
    sig: &str,
    cb: SignalCallback,
    data: *mut c_void,
) {
    let Some(sp) = find_signal(h, sig) else {
        eprintln!("signal_handler_connect: signal '{sig}' not found");
        return;
    };
    h.inner.refs.fetch_add(1, SeqCst);
    let generation = h.inner.generation.fetch_add(1, SeqCst);
    unsafe {
        let s = &mut *sp;
        let _sl = s.lock.lock();
        s.cbs.push(Box::new(Cb {
            cb,
            data,
            remove: false,
            keep_ref: true,
            generation,
        }));
    }
}

/// S-004-R23/Q-6: connect_global — dedupes by (cb, data) among entries
/// that are not executing.
pub fn signal_handler_connect_global(
    h: &SignalHandler,
    cb: GlobalSignalCallback,
    data: *mut c_void,
) {
    if h.inner.dead.load(SeqCst) {
        return;
    }
    let generation = h.inner.generation.fetch_add(1, SeqCst);
    let _gl = h.inner.global_lock.lock();
    {
        let gs = unsafe { &mut *h.inner.globals.globals.get() };
        if gs
            .iter()
            .any(|g| std::ptr::fn_addr_eq(g.cb, cb) && g.data == data && g.signaling == 0)
        {
            return;
        }
        gs.push(Box::new(GlobalCb {
            cb,
            data,
            signaling: 0,
            remove: false,
            generation,
        }));
    }
}

// ── S-004-R30–R32: disconnecting ───────────────────────────────────────

/// S-004-R30: disconnect the first entry with identity (cb, data).
/// While the signal is emitting: only marks `remove`. Otherwise erases;
/// a `keep_ref` entry decrements refs and destroys the handler at 0.
pub fn signal_handler_disconnect(
    h: &SignalHandler,
    sig: &str,
    cb: SignalCallback,
    data: *mut c_void,
) {
    let Some(sp) = find_signal(h, sig) else {
        return; // no log (R30)
    };
    unsafe {
        let s = &mut *sp;
        let _sl = s.lock.lock();
        let Some(i) = s
            .cbs
            .iter()
            .position(|c| std::ptr::fn_addr_eq(c.cb, cb) && c.data == data)
        else {
            return;
        };
        if s.signalling {
            s.cbs[i].remove = true;
            return;
        }
        let keep_ref = s.cbs[i].keep_ref;
        s.cbs.remove(i);
        drop(_sl);
        if keep_ref && h.inner.refs.fetch_sub(1, SeqCst) == 1 {
            h.inner.destroy();
        }
    }
}

/// S-004-R31/Q-5: disconnect_global — the entry is found by (cb, data)
/// among entries that are **not executing**; an executing entry is never
/// matched, so a global callback cannot disconnect itself this way.
pub fn signal_handler_disconnect_global(
    h: &SignalHandler,
    cb: GlobalSignalCallback,
    data: *mut c_void,
) {
    if h.inner.dead.load(SeqCst) {
        return;
    }
    let _gl = h.inner.global_lock.lock();
    let gs = unsafe { &mut *h.inner.globals.globals.get() };
    let Some(i) = gs
        .iter()
        .position(|g| std::ptr::fn_addr_eq(g.cb, cb) && g.data == data && g.signaling == 0)
    else {
        return;
    };
    gs.remove(i);
}

/// S-004-R32: mark the currently running callback (or global callback)
/// of the calling thread for removal. Handler-agnostic, as in C.
pub fn signal_handler_remove_current() {
    if let Some(c) = CURRENT_CB.take() {
        let h: &SignalHandler = unsafe { &*(c.handler.as_ptr() as *const SignalHandler) };
        let sp: *mut Signal = {
            let _hl = h.inner.handler_lock.lock().unwrap();
            let signals = unsafe { &*h.inner.state.signals.get() };
            match signals.get(c.sig_idx) {
                Some(sig) => Box::as_ptr(sig).cast_mut(),
                None => return,
            }
        };
        unsafe { cb_mark(sp, c.entry, c.generation) };
        return;
    }
    if let Some(c) = CURRENT_GLOBAL.take() {
        let h: &SignalHandler = unsafe { &*(c.handler.as_ptr() as *const SignalHandler) };
        let matches = {
            let _gl = h.inner.global_lock.lock();
            let gs = unsafe { &*h.inner.globals.globals.get() };
            c.entry < gs.len() && gs[c.entry].generation == c.generation
        };
        if matches {
            g_mark_remove(h, c.entry);
        }
    }
}

// ── Global-list helpers ─────────────────────────────────────────────────
// Same discipline as the `cb_*` helpers: each derives a short-lived
// reference from the `UnsafeCell` under the (reentrant) global lock and
// holds no reference across a callback, so re-entrant modification
// (connect/disconnect/remove_current inside a callback) is sound.

fn g_count(h: &SignalHandler) -> usize {
    let _gl = h.inner.global_lock.lock();
    let gs = unsafe { &*h.inner.globals.globals.get() };
    gs.len()
}

fn g_at(h: &SignalHandler, i: usize) -> Option<(GlobalSignalCallback, *mut c_void, u64)> {
    let _gl = h.inner.global_lock.lock();
    let gs = unsafe { &*h.inner.globals.globals.get() };
    let g = &gs[i];
    if g.remove {
        None
    } else {
        Some((g.cb, g.data, g.generation))
    }
}

fn g_set_signalling(h: &SignalHandler, i: usize, delta: i32) {
    let _gl = h.inner.global_lock.lock();
    let gs = unsafe { &mut *h.inner.globals.globals.get() };
    if i < gs.len() {
        gs[i].signaling = gs[i].signaling.wrapping_add(delta as u32);
    }
}

fn g_mark_remove(h: &SignalHandler, i: usize) {
    let _gl = h.inner.global_lock.lock();
    let gs = unsafe { &mut *h.inner.globals.globals.get() };
    if i < gs.len() {
        gs[i].remove = true;
    }
}

fn g_erase_marked(h: &SignalHandler, i: usize) -> bool {
    let _gl = h.inner.global_lock.lock();
    let gs = unsafe { &mut *h.inner.globals.globals.get() };
    if i < gs.len() && gs[i].remove && gs[i].signaling == 0 {
        let keep_ref = false; // globals have no reference counting (R62)
        gs.remove(i);
        keep_ref
    } else {
        false
    }
}

// ── S-004-R40–R45: emitting ─────────────────────────────────────────────

/// S-004-R40/R41: emit `sig` on `h` with shared `params`.
pub fn signal_handler_signal(h: &SignalHandler, sig: &str, params: &mut Calldata) {
    if h.inner.dead.load(SeqCst) {
        return;
    }
    let (sig_idx, sp) = {
        let _hl = h.inner.handler_lock.lock().unwrap();
        let signals = unsafe { &*h.inner.state.signals.get() };
        let sig_idx = match signals.iter().position(|s| s.name == sig) {
            Some(i) => i,
            None => return, // R40: undeclared — no globals either
        };
        // The Box<Signal> allocation is stable; the handler lock is
        // released before any callback runs (S-004 §7).
        (sig_idx, Box::as_ptr(&signals[sig_idx]).cast_mut())
    };
    h.inner.emitting.fetch_add(1, SeqCst);
    let handler = NonNull::from(h);

    unsafe { cb_set_signalling(sp, true) };
    let mut removed_keep_refs = 0usize;
    // R41.2 / Q-1: index loop, length re-read every iteration — entries
    // appended during the emit are reached by this emit.
    let mut i = 0usize;
    loop {
        let n = unsafe { cb_count(sp) };
        if i >= n {
            break;
        }
        if let Some((f, data, generation)) = unsafe { cb_at(sp, i) } {
            CURRENT_CB.set(Some(Current {
                handler,
                sig_idx,
                entry: i,
                generation,
            }));
            // Callbacks are `extern "C"`: a panic aborts (no unwinding
            // into the callback's stack frame), per the S-001
            // "no unwinding into C" guard.
            unsafe { (f)(data, params as *mut Calldata) };
            // Q-2: cleared after *any* callback returns.
            CURRENT_CB.set(None);
        }
        i += 1;
    }
    // R41.3: erase marked entries, back to front.
    let mut j = unsafe { cb_count(sp) };
    while j > 0 {
        j -= 1;
        if unsafe { cb_erase_marked(sp, j) } {
            removed_keep_refs += 1;
        }
    }
    unsafe { cb_set_signalling(sp, false) }; // boolean, Q-3

    // R41.5: global pass under the recursive global lock. Copy every
    // callable field out of the record before the invocation — no
    // reference into the Vec is held across a callback (module docs).
    // R41.5: global pass. No reference into the global list is held
    // across a callback (module docs): re-entrant connect/disconnect/
    // remove_current inside a callback re-derive their own references.
    // The (recursive) global lock is held for the whole pass, as in C.
    {
        let _gl = h.inner.global_lock.lock();
        let mut i = 0usize;
        loop {
            let n = g_count(h);
            if i >= n {
                break;
            }
            if let Some((f, data, generation)) = g_at(h, i) {
                g_set_signalling(h, i, 1);
                CURRENT_GLOBAL.set(Some(CurrentGlobal {
                    handler,
                    entry: i,
                    generation,
                }));
                // Pointer into the signal's name (stable allocation);
                // the borrow of the `name` field ends with this statement.
                let sig_name = unsafe { (*sp).name.as_ptr() };
                unsafe { (f)(data, sig_name, params as *mut Calldata) };
                CURRENT_GLOBAL.set(None);
                g_set_signalling(h, i, -1);
            }
            i += 1;
        }
        // Erase marked entries, back to front.
        let mut j = g_count(h);
        while j > 0 {
            j -= 1;
            g_erase_marked(h, j);
        }
    }

    // R41.6 / Q-7: subtract without a destroy check.
    if removed_keep_refs > 0 {
        h.inner.refs.fetch_sub(removed_keep_refs, SeqCst);
    }
    h.inner.maybe_free_dead();
}

// ── Introspection (mirrors the C darray access; used by tests and the
// future C shim) ──────────────────────────────────────────────────────

/// The number of callbacks connected to `sig`, or `None` if the signal
/// is unknown (or the handler is dead).
#[must_use]
pub fn signal_handler_cb_count(h: &SignalHandler, sig: &str) -> Option<usize> {
    let sp = find_signal(h, sig)?;
    Some(unsafe { cb_count(sp) })
}

/// The current reference count (starts at 1; S-004-R02/R22/R30).
#[must_use]
pub fn signal_handler_refs(h: &SignalHandler) -> usize {
    h.inner.refs.load(SeqCst)
}

/// Whether the handler was destroyed (refs reached 0; module docs:
/// calls on a dead handler are no-ops).
#[must_use]
pub fn signal_handler_is_dead(h: &SignalHandler) -> bool {
    h.inner.dead.load(SeqCst)
}
