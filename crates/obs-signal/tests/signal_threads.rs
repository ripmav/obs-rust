//! P1-22.3: real-code thread tests (std threads) for the S-004
//! handler. The loom model (`signal_loom.rs`) proves no deadlock over
//! the modeled lock order; these run the **production** handler under
//! real threads for the Done-when case (connect from another thread
//! during an emit), the Q-2 remove_current-during-nested-emit case,
//! and the WP-33 ADR contract (a typed guard outliving the handler's
//! destruction on another thread).
//!
//! Like `signal_proc.rs` this is a dedicated binary: the callbacks
//! are `extern "C"` fns with process-global counters, with distinct
//! data tokens per test so the (cb, data) dedupe (R21) never
//! conflates tests.

use std::ffi::c_void;
use std::ptr::with_exposed_provenance;
use std::sync::atomic::{AtomicUsize, Ordering::SeqCst};
use std::sync::mpsc;

use obs_signal::calldata::{Calldata, calldata_get_int, calldata_set_int};
use obs_signal::signal::{
    SignalHandler, signal_handler_add, signal_handler_cb_count, signal_handler_connect,
    signal_handler_create, signal_handler_destroy, signal_handler_disconnect,
    signal_handler_is_dead, signal_handler_remove_current, signal_handler_signal,
};
use obs_signal::typed::{Signal, SignalArgs};

/// Distinct, stable, non-null `data` identity tokens (never
/// dereferenced) — same helper as `signal_proc.rs`.
fn data_token(n: usize) -> *mut c_void {
    with_exposed_provenance::<c_void>(n).cast_mut()
}

/// `data` carries `handler.as_ptr()` for the emit.
fn handler_data(h: &SignalHandler) -> *mut c_void {
    h as *const SignalHandler as *mut c_void
}

// ── T1: connect from another thread during an emit (Done-when) ───────

static T1_RAN: AtomicUsize = AtomicUsize::new(0);
static T1_RAN2: AtomicUsize = AtomicUsize::new(0);

unsafe extern "C" fn t1_cb(_d: *mut c_void, _p: *mut Calldata) {
    T1_RAN.fetch_add(1, SeqCst);
}
unsafe extern "C" fn t1_cb2(_d: *mut c_void, _p: *mut Calldata) {
    T1_RAN2.fetch_add(1, SeqCst);
}

/// The P1-22.3 Done-when case on the production handler: one thread
/// emits while another connects (and disconnects) a second callback
/// on the same signal. Deadlock = the join would hang; the exact
/// counts pin that the always-connected callback runs once per emit
/// and the transient one is cleaned up (Q-1: it is reached in the
/// emits it lands in, then erased).
#[test]
fn connect_during_emit() {
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void s()"));
    signal_handler_connect(&h, "s", t1_cb, data_token(0x1000));

    // `thread::scope` joins at the end: after it returns, the
    // connect/disconnect loop is fully done.
    std::thread::scope(|s| {
        s.spawn(|| {
            // `h` is borrowed for the scope only (the handler is
            // `Sync` per ADR 0002 Amendment 1).
            let h: &SignalHandler = &h;
            for _ in 0..200 {
                signal_handler_connect(h, "s", t1_cb2, data_token(0x2000));
                signal_handler_disconnect(h, "s", t1_cb2, data_token(0x2000));
            }
        });
        let mut cd = Calldata::init();
        for _ in 0..200 {
            signal_handler_signal(&h, "s", &mut cd);
        }
    });

    assert_eq!(
        T1_RAN.load(SeqCst),
        200,
        "the always-connected cb ran once per emit"
    );
    // Non-deterministic (Q-1: the transient cb runs only in the emits
    // it is connected during) — read for the log, not asserted.
    let _ = T1_RAN2.load(SeqCst);
    assert_eq!(
        signal_handler_cb_count(&h, "s"),
        Some(1),
        "the transient cb was cleaned up"
    );
    assert!(!signal_handler_destroy(&h), "refs 1 → 0: destroyed");
}

// ── T2: remove_current during a nested emit (S-004 Q-2) ───────────────

static T2_F_RAN: AtomicUsize = AtomicUsize::new(0);
static T2_G_RAN: AtomicUsize = AtomicUsize::new(0);
static T2_U_RAN: AtomicUsize = AtomicUsize::new(0);

/// `g`'s callback: removes **itself** (the current callback of the
/// emitting thread).
unsafe extern "C" fn t2_cb_g(_d: *mut c_void, _p: *mut Calldata) {
    T2_G_RAN.fetch_add(1, SeqCst);
    signal_handler_remove_current();
}

/// `f`'s callback: emits `g` (nested), then calls
/// `remove_current` for itself. Q-2: the nested emit's return
/// **cleared** the thread's current callback (it is not restored), so
/// this must be a no-op — `f`'s entry survives.
unsafe extern "C" fn t2_cb_f(data: *mut c_void, _p: *mut Calldata) {
    T2_F_RAN.fetch_add(1, SeqCst);
    // SAFETY: test — `data` carries `handler.as_ptr()` (registered above).
    let h: &SignalHandler = unsafe { &*(data as *const SignalHandler) };
    let mut cd = Calldata::init();
    signal_handler_signal(h, "g", &mut cd);
    signal_handler_remove_current();
}

unsafe extern "C" fn t2_cb_u(_d: *mut c_void, _p: *mut Calldata) {
    T2_U_RAN.fetch_add(1, SeqCst);
}

/// Q-2 under real threads: the f→g nested-emit scenario runs on the
/// test thread while a second thread hammers an unrelated signal on
/// the same handler (handler-lock contention). The thread-local
/// current-cb record is per thread, and the nested emit's return
/// clears (not restores) it — so `g`'s self-removal erases `g`'s
/// entry and `f`'s own `remove_current` is a no-op.
#[test]
fn remove_current_q2_nested_emit() {
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    assert!(signal_handler_add(&h, "void g()"));
    assert!(signal_handler_add(&h, "void u()"));
    signal_handler_connect(&h, "f", t2_cb_f, handler_data(&h));
    signal_handler_connect(&h, "g", t2_cb_g, data_token(0x3000));
    signal_handler_connect(&h, "u", t2_cb_u, data_token(0x4000));

    std::thread::scope(|s| {
        s.spawn(|| {
            // `h` is borrowed for the scope only (the handler is
            // `Sync` per ADR 0002 Amendment 1).
            let h: &SignalHandler = &h;
            let mut cd = Calldata::init();
            for _ in 0..50 {
                signal_handler_signal(h, "u", &mut cd);
            }
        });
        let mut cd = Calldata::init();
        signal_handler_signal(&h, "f", &mut cd);
    });

    assert_eq!(T2_F_RAN.load(SeqCst), 1, "cb_f ran once");
    assert_eq!(T2_G_RAN.load(SeqCst), 1, "cb_g ran once and removed itself");
    assert_eq!(
        T2_U_RAN.load(SeqCst),
        50,
        "the other thread's emits were unaffected"
    );
    assert_eq!(signal_handler_cb_count(&h, "g"), Some(0), "cb_g was erased");
    assert_eq!(
        signal_handler_cb_count(&h, "f"),
        Some(1),
        "Q-2: cb_f's remove_current was a no-op (cleared, not restored)"
    );

    // Second emit: cb_f runs again; cb_g stays gone.
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "f", &mut cd);
    assert_eq!(T2_F_RAN.load(SeqCst), 2);
    assert_eq!(T2_G_RAN.load(SeqCst), 1);
    assert!(!signal_handler_destroy(&h), "refs 1 → 0: destroyed");
}

// ── T3: destroy the handler while a typed guard is alive (WP-33 ADR) ──

/// The P1-23 typed arguments of the T3 signal (one int, S-002 layout).
struct I32Args(i64);

impl SignalArgs for I32Args {
    fn name() -> &'static str {
        "i32s"
    }
    fn decl() -> &'static str {
        "void i32s(int v)"
    }
    fn write(&self, cd: &mut Calldata) {
        calldata_set_int(cd, b"v", self.0);
    }
    fn read(cd: &Calldata) -> Self {
        I32Args(calldata_get_int(cd, b"v").unwrap_or(0))
    }
}

/// Reachable at exit so the leaked handler is not a Miri leak (the
/// same pattern as the `typed` tests' `DROP_HANDLER`).
static T3_HANDLER: std::sync::Mutex<Option<&'static SignalHandler>> = std::sync::Mutex::new(None);
static T3_RAN: AtomicUsize = AtomicUsize::new(0);

/// The WP-33 ADR contract (ADR 0002 Amendment 1) across threads: a
/// `Connection` guard lives on thread B while the handler is
/// destroyed on the main thread. The guard's emit before the destroy
/// runs; everything after (emit, connect, drop) is a documented
/// no-op on the dead handler, and the closure box is still freed
/// exactly once by the drop.
#[test]
fn destroy_handler_while_guard_alive() {
    let h: &'static SignalHandler = Box::leak(signal_handler_create());
    *T3_HANDLER.lock().unwrap() = Some(h);
    assert!(signal_handler_add(h, I32Args::decl()));
    let sig = Signal::<I32Args>::new(h);

    let (connected_tx, connected_rx) = mpsc::channel();
    let (destroyed_tx, destroyed_rx) = mpsc::channel();
    let (done_tx, done_rx) = mpsc::channel();
    let th = std::thread::spawn(move || {
        let sig = Signal::<I32Args>::new(h);
        let conn = sig.connect(|_| {
            T3_RAN.fetch_add(1, SeqCst);
        });
        connected_tx.send(()).unwrap();
        destroyed_rx.recv().unwrap();
        // The handler is dead here and the guard is still alive:
        sig.emit(&I32Args(1)); // no-op (dead handler)
        let conn2 = sig.connect(|_| {
            T3_RAN.fetch_add(1, SeqCst);
        }); // no-op (dead handler)
        drop(conn2); // inert generation: disconnect no-op, box freed once
        drop(conn); // disconnect on the dead handler: no-op, box freed once
        done_tx.send(()).unwrap();
    });
    connected_rx.recv().unwrap();

    sig.emit(&I32Args(7)); // guard alive, handler alive: the guard runs
    assert_eq!(T3_RAN.load(SeqCst), 1);
    assert!(!signal_handler_destroy(h), "refs 1 → 0: destroyed");
    assert!(signal_handler_is_dead(h));
    destroyed_tx.send(()).unwrap();
    done_rx.recv().unwrap();
    th.join().unwrap();

    assert_eq!(
        T3_RAN.load(SeqCst),
        1,
        "post-destroy emits/connections are no-ops"
    );
}
