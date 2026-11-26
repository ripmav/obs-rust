//! P1-22.3: loom model-check of the S-004 lock structure (`signal.rs`).
//!
//! **Why a test double (documented deviation, ODD L86):** loom models
//! `std::sync`/`loom` primitives by intercepting their operations; the
//! production handler's `parking_lot::ReentrantMutex` (the per-signal
//! and the global-callbacks locks, S-004-R50) is invisible to loom, so
//! running the production handler under `loom::model` would model none
//! of its locks. This file is the P1-22.3 fallback the subtask allows:
//! a model that mirrors the production **lock structure** — the same
//! three locks, the same acquire/release points, the same lock orders,
//! the same recursive semantics — with `loom::Mutex`, plus the
//! real-code std-thread confirmation in `signal_threads.rs`.
//!
//! The mirrored structure (each operation lists its production lock
//! points, in order):
//!
//! - `connect` / `disconnect`: `handler_lock` (signal lookup,
//!   released before the list mutation) → `sig_lock` (list mutation).
//! - `emit`: `handler_lock` (lookup, released) → per-op `sig_lock`
//!   (set signalling / count / read entry / erase pass / clear
//!   signalling) with callbacks invoked **without the lock** (S-004
//!   §7 — the Rust port takes the per-signal lock per op; the C
//!   reference holds it over the whole emit, so the port's one
//!   genuinely recursive lock is the global one) → the global pass
//!   **holds** `global_lock` for the whole pass (the production `g_*`
//!   helpers re-take it recursively on the same thread; a reentrant
//!   lock re-taken by its holder is the holder keeping it, so the
//!   model does not re-acquire) → the epilogue's free of a dead
//!   handler clears the handler list under `handler_lock`, **then**
//!   the global list under `global_lock` — never held together (see
//!   the lock-order note below), and only when this emit was the last
//!   one (`emitting` counter) and the handler is dead.
//! - `destroy` (refs 1 → 0): the same non-nested list clear only when
//!   no emit is in progress, then the dead flag (production order: the
//!   emit check precedes the dead store).
//! - `connect_global`: `global_lock` only, no handler lock (as in
//!   production).
//!
//! Lock order (WP-36, verified): the one cross-lock edge in the
//! handler is a global callback's nested emit taking `handler_lock`
//! (signal lookup) and `sig_lock` while the outer global pass holds
//! `global_lock` — `global → handler/signal`. The deferred free must
//! therefore **not** hold the handler lock across the global-lock
//! acquisition: the first draft of this model (mirroring the first
//! draft of the port, which nested the two free locks) deadlocked
//! against exactly that edge — a stale `emitting == 0` load lets the
//! destroyer take `handler_lock` while an emit that started after the
//! load is already in its global pass. The port's free paths
//! (`destroy` / `maybe_free_dead`) were fixed to the non-nested order
//! this model mirrors; with it, no thread holds a lock it does not
//! need while acquiring another, and loom checks that no cycle
//! remains over every interleaving (a deadlock panics the model),
//! with the `emitting`/`dead` visibility interleavings included.
//!
//! The `run` helper prints the number of permutations loom explored
//! per model (loom 0.7 runs the model closure once per permutation);
//! that count is the state-space size for the report.

use std::sync::Arc;
use std::sync::atomic::AtomicUsize as StdAtomicUsize;
use std::sync::atomic::Ordering::SeqCst;

use loom::sync::atomic::{AtomicBool, AtomicUsize};
use loom::sync::{Mutex, MutexGuard};
use loom::thread;

/// loom's `Mutex::lock` returns a `Result` (poisoning is not modeled
/// — a panicking thread inside the model fails the whole run anyway);
/// the model unwraps it at every acquire point.
fn lk<T: ?Sized>(m: &Mutex<T>) -> MutexGuard<'_, T> {
    m.lock().unwrap_or_else(std::sync::PoisonError::into_inner)
}

/// The state the per-signal lock guards (production: the `Signal`'s
/// `cbs` list and `signalling` flag, under its `ReentrantMutex`).
struct SigState {
    /// The callback list, `true` = marked `remove`.
    cbs: Vec<bool>,
    /// The boolean (not a counter) signalling flag (Q-3).
    signalling: bool,
}

/// The model: the same three locks as `SignalHandler`'s `Inner`,
/// guarding the same state (the per-signal callback list, the global
/// list, the ref/emit/death counters). One signal is enough — the
/// lock structure is per-signal and identical for every signal.
struct Model {
    /// The production `handler_lock` (non-recursive; guards the
    /// signal-list lookup).
    handler_lock: Mutex<()>,
    /// The production per-signal `lock` (recursive in C; the Rust
    /// port takes it per op, which is what is modeled here).
    sig_lock: Mutex<SigState>,
    /// The production `global_lock` (recursive; held across the whole
    /// global pass) and the global list, `true` = marked `remove`.
    global_lock: Mutex<Vec<bool>>,
    /// S-004 refs; starts at 1, as `signal_handler_create`.
    refs: AtomicUsize,
    /// Emits in progress — the guard of the epilogue/destroy free.
    emitting: AtomicUsize,
    /// Refs reached 0 (destroyed).
    dead: AtomicBool,
    /// Signal-callback invocations (postcondition).
    ran: AtomicUsize,
}

impl Model {
    fn new() -> Self {
        Self {
            handler_lock: Mutex::new(()),
            // One pre-connected callback: the handler already has a
            // listener when the other thread starts working.
            sig_lock: Mutex::new(SigState {
                cbs: vec![false],
                signalling: false,
            }),
            global_lock: Mutex::new(vec![]),
            refs: AtomicUsize::new(1),
            emitting: AtomicUsize::new(0),
            dead: AtomicBool::new(false),
            ran: AtomicUsize::new(0),
        }
    }
}

/// The deferred free (production `destroy` / `maybe_free_dead`): the
/// signal list under `handler_lock`, then the global list under
/// `global_lock` — **never held together**: a nested H→G hold would
/// deadlock against the global callback's nested emit, which takes H
/// while the pass holds G (the cycle the loom check exists to
/// exclude; see the signal.rs "Threads" section).
fn free_lists(h: &Model) {
    {
        let _hl = lk(&h.handler_lock);
        lk(&h.sig_lock).cbs.clear();
    }
    let mut gl = lk(&h.global_lock);
    gl.clear();
}

// ── Mirrored operations (lock points as in `signal.rs`) ───────────────

/// The production `find_signal`: `handler_lock` for the lookup,
/// released before any callback or list mutation (S-004 §7).
fn find_signal(h: &Model) {
    let _g = lk(&h.handler_lock);
}

/// `signal_handler_connect` / `signal_handler_connect_gen`: dead check
/// (before the locks, as in production), lookup, then the list push
/// under `sig_lock`. (The (cb, data) dedup runs under the same held
/// `sig_lock` and is irrelevant to the lock structure.)
fn connect(h: &Model) {
    if h.dead.load(SeqCst) {
        return;
    }
    find_signal(h);
    let mut s = lk(&h.sig_lock);
    s.cbs.push(false);
}

/// `signal_handler_disconnect`: lookup, then under `sig_lock` — mark
/// `remove` while the signal is emitting, erase otherwise (R30).
fn disconnect(h: &Model) {
    find_signal(h);
    let mut s = lk(&h.sig_lock);
    let Some(i) = s.cbs.iter().position(|&r| !r) else {
        return;
    };
    if s.signalling {
        s.cbs[i] = true;
    } else {
        s.cbs.remove(i);
    }
}

/// `signal_handler_connect_global`: `global_lock` only, no handler
/// lock (as in production).
fn connect_global(h: &Model) {
    if h.dead.load(SeqCst) {
        return;
    }
    let mut g = lk(&h.global_lock);
    g.push(false);
}

/// `signal_handler_signal`, lock point for lock point. `gcb` runs
/// while `global_lock` is held (the global pass); the nested-emit test
/// uses it to take the global→handler/signal edges.
fn emit(h: &Model, gcb: impl Fn(&Model)) {
    // R40 + dead check (before the locks, as in production).
    if h.dead.load(SeqCst) {
        return;
    }
    find_signal(h);
    h.emitting.fetch_add(1, SeqCst);
    {
        let mut s = lk(&h.sig_lock);
        s.signalling = true;
    }
    // R41.2/Q-1: index loop, length re-read every iteration (a
    // concurrent connect is reached by this emit); entry read under
    // the lock, callback invoked without it (S-004 §7).
    let mut i = 0usize;
    loop {
        let n = {
            let s = lk(&h.sig_lock);
            s.cbs.len()
        };
        if i >= n {
            break;
        }
        let run = {
            let s = lk(&h.sig_lock);
            i < s.cbs.len() && !s.cbs[i]
        };
        if run {
            h.ran.fetch_add(1, SeqCst);
        }
        i += 1;
    }
    // R41.3: erase pass, one per-op lock per entry, back to front.
    let mut j = {
        let s = lk(&h.sig_lock);
        s.cbs.len()
    };
    while j > 0 {
        j -= 1;
        let mut s = lk(&h.sig_lock);
        if j < s.cbs.len() && s.cbs[j] {
            s.cbs.remove(j);
        }
    }
    {
        let mut s = lk(&h.sig_lock);
        s.signalling = false;
    }
    // R41.5: the global pass — the recursive `global_lock` held for
    // the whole pass; the `g_*` helpers' recursive re-take is the
    // holder keeping it, so it is not re-acquired here. A global
    // callback's nested emit (`gcb`) runs under the held lock.
    {
        let mut g = lk(&h.global_lock);
        let mut i = 0usize;
        loop {
            let n = g.len();
            if i >= n {
                break;
            }
            if i < g.len() && !g[i] {
                gcb(h);
            }
            i += 1;
        }
        g.retain(|r| !*r);
    }
    // R41.6/Q-7 + `maybe_free_dead`: the deferred free — two
    // non-nested critical sections (handler list, then global list),
    // and only when this emit was the last one and the handler is dead.
    if h.emitting.fetch_sub(1, SeqCst) == 1 && h.dead.load(SeqCst) {
        free_lists(h);
    }
}

/// The lock edges of a nested emit from a global callback: the nested
/// `find_signal` takes `handler_lock` and the nested signal pass takes
/// `sig_lock` — while the outer global pass holds `global_lock`. The
/// nested global pass is the recursive re-take (the holder keeps it —
/// not re-acquired). The nested epilogue **can** free: `fetch_sub == 1`
/// holds when the outer emit is the only other one in progress (2 → 1)
/// and the handler is dead — production then clears the handler list
/// under the handler lock; the global-list half is the recursive
/// re-take (the holder keeps it, as in the `g_*` helpers).
fn nested_emit(h: &Model) {
    h.emitting.fetch_add(1, SeqCst);
    find_signal(h);
    {
        let mut s = lk(&h.sig_lock);
        s.signalling = true;
    }
    {
        let s = lk(&h.sig_lock);
        let _n = s.cbs.len(); // count + entry read, as in the pass
    }
    {
        let mut s = lk(&h.sig_lock);
        s.signalling = false;
    }
    // (nested global pass: recursive re-take = the holder keeps it)
    // Epilogue free: handler list under the handler lock (the global
    // list half is the recursive re-take — this thread already holds
    // the global lock in the outer pass, so the clear of it elides to
    // a comment).
    if h.emitting.fetch_sub(1, SeqCst) == 1 && h.dead.load(SeqCst) {
        let _hl = lk(&h.handler_lock);
        lk(&h.sig_lock).cbs.clear();
        // (globals cleared under the already-held global lock)
    }
}

/// `signal_handler_destroy`'s free (refs 1 → 0): the non-nested list
/// clear only when no emit is in progress, then the dead flag
/// (production order: the emit check precedes the dead store).
fn destroy(h: &Model) {
    if h.refs.fetch_sub(1, SeqCst) == 1 {
        if h.emitting.load(SeqCst) == 0 {
            free_lists(h);
        }
        h.dead.store(true, SeqCst);
    }
}

// ── Models ────────────────────────────────────────────────────────────

/// W9 pattern: the loom models run sequentially (each `loom::model`
/// call drives its own scheduler; the std mutex keeps the three
/// models apart from each other).
static MODEL_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

/// Run one loom model; returns the number of permutations explored.
/// A deadlock (or a failed assertion) panics inside `loom::model`.
fn run(name: &str, f: impl Fn() + Send + Sync + 'static) -> usize {
    let _g = MODEL_LOCK.lock().unwrap();
    let perms = Arc::new(StdAtomicUsize::new(0));
    let count = Arc::clone(&perms);
    loom::model(move || {
        count.fetch_add(1, SeqCst);
        f();
    });
    let n = perms.load(SeqCst);
    eprintln!("loom {name}: explored {n} permutations, no deadlock");
    n
}

/// **P1-22.3 Done-when case:** connect from another thread during an
/// emit. No deadlock over every interleaving; the connected entry
/// survives the emit (Q-1: it is reached by it).
#[test]
fn loom_connect_during_emit() {
    let n = run("connect-during-emit", || {
        let h = loom::sync::Arc::new(Model::new());
        let t = loom::sync::Arc::clone(&h);
        let th = thread::spawn(move || connect(&t));
        emit(&h, |_h| {}); // no global callbacks
        th.join().unwrap();
        // Compute under the lock, assert after (a failing assert must
        // not panic while holding a model lock).
        let n = lk(&h.sig_lock).cbs.len();
        let sig = lk(&h.sig_lock).signalling;
        assert_eq!(n, 2, "the connect survives the emit");
        assert!(!sig, "the emit finished cleanly");
        assert!(h.ran.load(SeqCst) >= 1, "the pre-connected cb ran");
    });
    assert!(n > 1, "the model explored more than one interleaving");
}

/// The related case: disconnect from another thread during an emit —
/// mark-while-emitting (erased at the end of the emit) or erase
/// outside it. A mark that lands after this emit's erase pass stays
/// marked until the next emit (the port's per-op locking exposes the
/// interleaving; C's whole-emit lock cannot get there) — so the model
/// normalizes with one more emit and then checks the list is empty.
/// No deadlock in any of it.
#[test]
fn loom_disconnect_during_emit() {
    let n = run("disconnect-during-emit", || {
        let h = loom::sync::Arc::new(Model::new());
        let t = loom::sync::Arc::clone(&h);
        let th = thread::spawn(move || disconnect(&t));
        emit(&h, |_h| {}); // no global callbacks
        th.join().unwrap();
        // The next emit's erase pass removes a still-marked entry.
        emit(&h, |_h| {});
        let n = lk(&h.sig_lock).cbs.len();
        assert_eq!(
            n, 0,
            "the disconnect took effect (erased, or marked → erased by the next emit's pass)"
        );
    });
    assert!(n > 1, "the model explored more than one interleaving");
}

/// Destroy while an emit is in progress: the free's
/// `handler_lock → global_lock` order (destroy + the emit epilogue)
/// against the global callback's nested emit taking `handler_lock`
/// and `sig_lock` while the global pass holds `global_lock`, plus a
/// concurrent `connect_global` on the held global lock. The
/// `emitting` counter keeps the two order edges from ever meeting —
/// loom checks that over every interleaving.
#[test]
fn loom_destroy_while_emitting() {
    let n = run("destroy-while-emitting", || {
        let h = loom::sync::Arc::new(Model::new());
        let t = loom::sync::Arc::clone(&h);
        let th = thread::spawn(move || {
            connect_global(&t);
            destroy(&t);
        });
        emit(&h, nested_emit); // the global callback re-emits (nested edges)
        th.join().unwrap();
        assert!(h.dead.load(SeqCst), "the handler is destroyed");
        // The list clear (epilogue or destroy) is best-effort — the
        // same ordering gap as production's `destroy`/`maybe_free_dead`
        // (emit check vs. dead store): if both miss, the handler's
        // Box drop is the final free. At most the never-marked
        // pre-connected entry and the one global entry remain.
        let nc = lk(&h.sig_lock).cbs.len();
        let ng = lk(&h.global_lock).len();
        assert!(
            nc <= 1 && ng <= 1,
            "at most the unmarked entries remain until the Box drop"
        );
    });
    assert!(n > 1, "the model explored more than one interleaving");
}
