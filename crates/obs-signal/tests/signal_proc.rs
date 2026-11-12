#![cfg_attr(miri, allow(dead_code, unused_imports))]
//! S-004 signal/proc vectors — dedicated sequential binary (like
//! `obs-util/tests/leak_counters.rs`): the callbacks are `extern "C"`
//! fns with process-global counters, so the vectors run one by one in a
//! single test function inside their own test process.

use std::ffi::{CString, c_void};
use std::ptr::with_exposed_provenance;
use std::sync::atomic::{AtomicBool, AtomicUsize, Ordering::SeqCst};

use obs_signal::calldata::{Calldata, calldata_get_int, calldata_set_int};
use obs_signal::proc::{
    proc_handler_add, proc_handler_call, proc_handler_create, proc_handler_destroy,
};
use obs_signal::signal::{
    signal_handler_add, signal_handler_add_array, signal_handler_cb_count, signal_handler_connect,
    signal_handler_connect_global, signal_handler_connect_ref, signal_handler_create,
    signal_handler_destroy, signal_handler_disconnect, signal_handler_disconnect_global,
    signal_handler_is_dead, signal_handler_refs, signal_handler_remove_current,
    signal_handler_signal,
};

static COUNTER: AtomicUsize = AtomicUsize::new(0);
static GLOBAL_COUNTER: AtomicUsize = AtomicUsize::new(0);
static SAW: AtomicUsize = AtomicUsize::new(0);

// ── signal callbacks ────────────────────────────────────────────────────

unsafe extern "C" fn count_cb(_data: *mut c_void, _params: *mut Calldata) {
    COUNTER.fetch_add(1, SeqCst);
}
unsafe extern "C" fn global_count(_data: *mut c_void, _sig: *const u8, _p: *mut Calldata) {
    GLOBAL_COUNTER.fetch_add(1, SeqCst);
}
unsafe extern "C" fn global_count2(_data: *mut c_void, _sig: *const u8, _p: *mut Calldata) {
    GLOBAL_COUNTER.fetch_add(10, SeqCst);
}
unsafe extern "C" fn disc_keep(data: *mut c_void, _p: *mut Calldata) {
    unsafe {
        let h = &*(data as *const obs_signal::signal::SignalHandler);
        signal_handler_disconnect(h, "g", count_cb, data_token(1));
    }
}

/// `data` carries `handler.as_ptr()` for the emit.
fn handler_data(h: &obs_signal::signal::SignalHandler) -> *mut c_void {
    h as *const obs_signal::signal::SignalHandler as *mut c_void
}

/// Distinct, stable, non-null `data` identity tokens (never dereferenced).
/// `with_exposed_provenance` is the legitimate strict-provenance-adjacent
/// API for fake pointers; Miri notes (not errors) that such pointers
/// cannot be checked for dangling — acceptable, these tokens are only
/// ever compared, never dereferenced.
fn data_token(n: usize) -> *mut c_void {
    with_exposed_provenance::<c_void>(n).cast_mut()
}

unsafe extern "C" fn disconnect_next_cb(data: *mut c_void, _p: *mut Calldata) {
    let h = unsafe { &*(data as *const obs_signal::signal::SignalHandler) };
    // disconnect the *other* connection: it hasn't run yet → skipped
    // (R45); the marked entry is erased at the end of this emit.
    signal_handler_disconnect(h, "f", count_cb, data_token(2));
}
unsafe extern "C" fn remove_current_cb(_data: *mut c_void, _p: *mut Calldata) {
    signal_handler_remove_current();
}
unsafe extern "C" fn connect_more_cb(data: *mut c_void, _p: *mut Calldata) {
    let h = unsafe { &*(data as *const obs_signal::signal::SignalHandler) };
    signal_handler_connect(h, "f", count_cb, data_token(7));
}
unsafe extern "C" fn nested_emit_cb(data: *mut c_void, _p: *mut Calldata) {
    unsafe {
        let h = &*(data as *const obs_signal::signal::SignalHandler);
        COUNTER.fetch_add(1, SeqCst); // outer callback running
        signal_handler_signal(h, "g", &mut Calldata::init());
        COUNTER.fetch_add(100, SeqCst); // outer resumed
    }
}
unsafe extern "C" fn global_remove_current(_d: *mut c_void, _s: *const u8, _p: *mut Calldata) {
    GLOBAL_COUNTER.fetch_add(1, SeqCst);
    signal_handler_remove_current();
}
unsafe extern "C" fn set_a_cb(_d: *mut c_void, params: *mut Calldata) {
    unsafe { calldata_set_int(&mut *params, b"a", 42) }
}
unsafe extern "C" fn see_a_cb(_d: *mut c_void, params: *mut Calldata) {
    unsafe {
        SAW.store(
            calldata_get_int(&*params, b"a").unwrap_or(-1) as usize,
            SeqCst,
        );
    }
}
unsafe extern "C" fn global_self_disconnect(data: *mut c_void, _s: *const u8, _p: *mut Calldata) {
    unsafe {
        let h = &*(data as *const obs_signal::signal::SignalHandler);
        GLOBAL_COUNTER.fetch_add(1, SeqCst);
        // itself is executing (signaling == 1) → not found → stays
        // connected (Q-5)
        signal_handler_disconnect_global(h, global_self_disconnect, data);
    }
}
unsafe extern "C" fn disconnect_and_reconnect(data: *mut c_void, _p: *mut Calldata) {
    let h = unsafe { &*(data as *const obs_signal::signal::SignalHandler) };
    let d = data_token(2);
    signal_handler_disconnect(h, "f", count_cb, d);
    signal_handler_connect(h, "f", count_cb, d); // no-op: marked entry exists
}
unsafe extern "C" fn nested_then_remove(data: *mut c_void, _p: *mut Calldata) {
    unsafe {
        let h = &*(data as *const obs_signal::signal::SignalHandler);
        COUNTER.fetch_add(1, SeqCst);
        signal_handler_signal(h, "g", &mut Calldata::init());
        // current_cb was cleared when the nested emit's callback returned
        // → this is a no-op (Q-2)
        signal_handler_remove_current();
        COUNTER.fetch_add(10, SeqCst);
    }
}
static NESTED_ONCE: AtomicBool = AtomicBool::new(false);
unsafe extern "C" fn outer_disc_inner(data: *mut c_void, _p: *mut Calldata) {
    unsafe {
        let h = &*(data as *const obs_signal::signal::SignalHandler);
        COUNTER.fetch_add(1, SeqCst);
        // first run only (guards against re-running in the nested emit):
        // mark the inner connection (the signal is emitting, so R30 marks
        // instead of erasing) and emit the same signal nested (R50)
        if NESTED_ONCE
            .compare_exchange(false, true, SeqCst, SeqCst)
            .is_ok()
        {
            signal_handler_disconnect(h, "f", count_cb, data_token(2));
            signal_handler_signal(h, "f", &mut Calldata::init());
        }
        COUNTER.fetch_add(10, SeqCst);
    }
}
unsafe extern "C" fn cross_emit(data: *mut c_void, _p: *mut Calldata) {
    let h = unsafe { &*(data as *const obs_signal::signal::SignalHandler) };
    COUNTER.fetch_add(1, SeqCst);
    signal_handler_signal(h, "other", &mut Calldata::init());
}

/// C-style null-terminated pointer array of declarations.
struct DeclArray {
    /// Keeps the `CString`s alive (drop glue); never read.
    #[allow(dead_code)]
    strings: Vec<CString>,
    ptrs: Vec<*const u8>,
}

impl DeclArray {
    fn new(decls: &[&str]) -> Self {
        let strings: Vec<CString> = decls.iter().map(|d| CString::new(*d).unwrap()).collect();
        // CStr::as_ptr() yields *const c_char (u8 on aarch64, i8 on x86_64);
        // declarations are byte data, normalize to *const u8.
        let mut ptrs: Vec<*const u8> = strings.iter().map(|c| c.as_ptr() as *const u8).collect();
        ptrs.push(std::ptr::null::<u8>());
        Self { strings, ptrs }
    }
}

// ── proc callbacks ──────────────────────────────────────────────────────

static CALLED: AtomicUsize = AtomicUsize::new(0);

unsafe extern "C" fn add_proc(data: *mut c_void, params: *mut Calldata) {
    unsafe {
        let n = *(data as *const i64);
        CALLED.fetch_add(1, SeqCst);
        let p = &mut *params;
        let a0 = calldata_get_int(p, b"a").unwrap_or(0);
        calldata_set_int(p, b"return", a0 + n);
    }
}
struct ReenterCtx {
    h: *const obs_signal::proc::ProcHandler,
    depth: i64,
}
unsafe extern "C" fn reentering_proc(data: *mut c_void, params: *mut Calldata) {
    unsafe {
        let ctx = &mut *(data as *mut ReenterCtx);
        let h = &*ctx.h;
        CALLED.fetch_add(1, SeqCst);
        // R60: the recursive lock lets a proc re-enter its own handler;
        // bounded by the depth budget so the test terminates
        if ctx.depth > 0 {
            ctx.depth -= 1;
            assert!(proc_handler_call(h, "reenter", &mut *params));
        }
    }
}

// ── vectors (run sequentially in `all_vectors`) ─────────────────────────

fn vec01_basic() {
    COUNTER.store(0, SeqCst);
    let h = signal_handler_create();
    assert_eq!(signal_handler_refs(&h), 1, "R01");
    let mut cd = Calldata::init();
    assert!(signal_handler_add(&h, "int add(int a, int b)"));
    signal_handler_connect(&h, "add", count_cb, std::ptr::null_mut());
    signal_handler_signal(&h, "add", &mut cd);
    assert_eq!(COUNTER.load(SeqCst), 1, "one connection, one emit");
    // undeclared signal: nothing happens (R40), globals not called
    signal_handler_signal(&h, "nope", &mut cd);
    assert_eq!(COUNTER.load(SeqCst), 1);
    // destroy at refs==1 frees (R02)
    assert!(!signal_handler_destroy(&h));
    assert!(signal_handler_is_dead(&h));
}

#[cfg(not(miri))]
fn vec02_connect_dedupe() {
    COUNTER.store(0, SeqCst);
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    let d1 = data_token(1);
    let d2 = data_token(2);
    signal_handler_connect(&h, "f", count_cb, d1);
    signal_handler_connect(&h, "f", count_cb, d1); // deduped (R21)
    assert_eq!(signal_handler_cb_count(&h, "f"), Some(1));
    signal_handler_connect(&h, "f", count_cb, d2); // different data
    assert_eq!(signal_handler_cb_count(&h, "f"), Some(2));
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "f", &mut cd);
    assert_eq!(COUNTER.load(SeqCst), 2);
    // connect on a missing signal: no entry, warning (R20)
    signal_handler_connect(&h, "missing", count_cb, d1);
    assert_eq!(signal_handler_cb_count(&h, "missing"), None);
}

#[cfg(not(miri))]
fn vec03_disconnect() {
    COUNTER.store(0, SeqCst);
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    let d1 = data_token(1);
    let d2 = data_token(2);
    signal_handler_connect(&h, "f", count_cb, d1);
    signal_handler_connect(&h, "f", count_cb, d2);
    signal_handler_disconnect(&h, "f", count_cb, d1);
    assert_eq!(signal_handler_cb_count(&h, "f"), Some(1));
    signal_handler_disconnect(&h, "f", count_cb, d1); // no match
    signal_handler_disconnect(&h, "missing", count_cb, d1); // no log
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "f", &mut cd);
    assert_eq!(COUNTER.load(SeqCst), 1);
}

#[cfg(not(miri))]
fn vec04_disconnect_during_emit() {
    COUNTER.store(0, SeqCst);
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    signal_handler_connect(&h, "f", disconnect_next_cb, handler_data(&h));
    signal_handler_connect(&h, "f", count_cb, data_token(2));
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "f", &mut cd);
    // the marked entry was skipped and erased: only the first ran
    assert_eq!(COUNTER.load(SeqCst), 0);
    assert_eq!(
        signal_handler_cb_count(&h, "f"),
        Some(1),
        "marked entry erased, the first remains"
    );
    signal_handler_signal(&h, "f", &mut cd);
    assert_eq!(COUNTER.load(SeqCst), 0);
}

fn vec05_remove_current() {
    COUNTER.store(0, SeqCst);
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    let d = data_token(1);
    signal_handler_connect(&h, "f", remove_current_cb, d);
    signal_handler_connect(&h, "f", count_cb, d);
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "f", &mut cd);
    assert_eq!(
        COUNTER.load(SeqCst),
        1,
        "second cb still ran (already passed)"
    );
    assert_eq!(
        signal_handler_cb_count(&h, "f"),
        Some(1),
        "self removed, other kept"
    );
    // outside a callback: no-op
    signal_handler_remove_current();
}

fn vec06_connect_during_emit() {
    COUNTER.store(0, SeqCst);
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    signal_handler_connect(&h, "f", connect_more_cb, handler_data(&h));
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "f", &mut cd);
    // the callback appended during the emit ran in the same emit (Q-1)
    assert_eq!(COUNTER.load(SeqCst), 1);
}

fn vec07_nested_emit() {
    COUNTER.store(0, SeqCst);
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    assert!(signal_handler_add(&h, "void g()"));
    signal_handler_connect(&h, "f", nested_emit_cb, handler_data(&h));
    signal_handler_connect(&h, "g", count_cb, std::ptr::null_mut());
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "f", &mut cd);
    // outer 1 + nested emit of g (count_cb: 1) + outer 100
    assert_eq!(COUNTER.load(SeqCst), 1 + 1 + 100);
}

#[cfg(not(miri))]
fn vec08_global_callbacks() {
    GLOBAL_COUNTER.store(0, SeqCst);
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    let d1 = data_token(1);
    signal_handler_connect_global(&h, global_count, d1);
    signal_handler_connect_global(&h, global_count, d1); // dedupe (R23)
    signal_handler_connect_global(&h, global_count2, d1); // different cb
    signal_handler_connect(&h, "f", count_cb, d1);
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "f", &mut cd);
    assert_eq!(GLOBAL_COUNTER.load(SeqCst), 1 + 10, "globals ran in order");
    // globals run for every declared signal
    assert!(signal_handler_add(&h, "void g()"));
    signal_handler_signal(&h, "g", &mut cd);
    assert_eq!(GLOBAL_COUNTER.load(SeqCst), 1 + 10 + 1 + 10);
    // disconnect_global outside an emit: found (signaling == 0)
    signal_handler_disconnect_global(&h, global_count, d1);
    signal_handler_signal(&h, "g", &mut cd);
    assert_eq!(GLOBAL_COUNTER.load(SeqCst), 1 + 10 + 1 + 10 + 10);
}

fn vec09_global_remove_current() {
    GLOBAL_COUNTER.store(0, SeqCst);
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    let d = data_token(1);
    signal_handler_connect_global(&h, global_remove_current, d);
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "f", &mut cd);
    assert_eq!(GLOBAL_COUNTER.load(SeqCst), 1);
    // it marked itself; after its run signaling is back to 0 → erased
    signal_handler_signal(&h, "f", &mut cd);
    assert_eq!(GLOBAL_COUNTER.load(SeqCst), 1, "erased after the emit");
}

fn vec10_params_shared() {
    SAW.store(0, SeqCst);
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    signal_handler_connect(&h, "f", set_a_cb, std::ptr::null_mut());
    signal_handler_connect(&h, "f", see_a_cb, std::ptr::null_mut());
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "f", &mut cd);
    assert_eq!(SAW.load(SeqCst), 42, "R42: same params pointer, in order");
}

#[cfg(not(miri))]
fn vec11_refs() {
    COUNTER.store(0, SeqCst);
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    let d = data_token(1);
    signal_handler_connect_ref(&h, "f", count_cb, d);
    assert_eq!(signal_handler_refs(&h), 2, "R22: refs incremented");
    // destroy at refs 2→1: survives, fully usable (R02)
    assert!(signal_handler_destroy(&h));
    assert!(!signal_handler_is_dead(&h));
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "f", &mut cd);
    assert_eq!(COUNTER.load(SeqCst), 1);
    // disconnect the keep_ref entry outside an emit: refs 1→0 → destroy
    // (R30)
    signal_handler_disconnect(&h, "f", count_cb, d);
    assert_eq!(signal_handler_refs(&h), 0);
    assert!(signal_handler_is_dead(&h), "R30: destroyed at 0");

    // Q-7: emit-removed keep_refs are subtracted without a destroy check
    // — refs may reach 0 via an emit and the handler is *not* freed there
    // (dead stays false; no vector — unit-level observation).
    let h2 = signal_handler_create();
    assert!(signal_handler_add(&h2, "void g()"));
    signal_handler_connect_ref(&h2, "g", count_cb, d); // refs 2
    assert!(signal_handler_destroy(&h2)); // refs 1 (alive)
    signal_handler_connect(&h2, "g", disc_keep, handler_data(&h2));
    signal_handler_signal(&h2, "g", &mut Calldata::init());
    assert_eq!(signal_handler_refs(&h2), 0, "Q-7: R41.6 subtracted");
    assert!(
        !signal_handler_is_dead(&h2),
        "Q-7: no destroy check in R41.6"
    );
}

fn vec13_declarations() {
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    assert!(!signal_handler_add(&h, "void f()"), "R11: duplicate");
    assert!(!signal_handler_add(&h, "void f(int)"), "R10: bad decl");
    assert!(!signal_handler_add(&h, ""), "R10: empty");
    assert_eq!(signal_handler_cb_count(&h, "f"), Some(0));
    // add_array
    let arr = DeclArray::new(&["void a()", "void b()"]);
    assert!(unsafe { signal_handler_add_array(&h, arr.ptrs.as_ptr()) });
    assert!(signal_handler_add(&h, "void c()"));
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "a", &mut cd);
    signal_handler_signal(&h, "b", &mut cd);
    // NULL array → false
    assert!(!unsafe { signal_handler_add_array(&h, std::ptr::null()) });
}

#[cfg(not(miri))]
fn vec14_global_self_disconnect() {
    GLOBAL_COUNTER.store(0, SeqCst);
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    let d = handler_data(&h);
    signal_handler_connect_global(&h, global_self_disconnect, d);
    let mut cd = Calldata::init();
    for _ in 0..3 {
        signal_handler_signal(&h, "f", &mut cd);
    }
    assert_eq!(GLOBAL_COUNTER.load(SeqCst), 3, "Q-5: still connected");
    // from outside any emit: found and erased
    signal_handler_disconnect_global(&h, global_self_disconnect, d);
    signal_handler_signal(&h, "f", &mut cd);
    assert_eq!(GLOBAL_COUNTER.load(SeqCst), 3, "erased from outside");
}

#[cfg(not(miri))]
fn vec15_reconnect_during_emit() {
    COUNTER.store(0, SeqCst);
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    signal_handler_connect(&h, "f", disconnect_and_reconnect, handler_data(&h));
    signal_handler_connect(&h, "f", count_cb, data_token(2));
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "f", &mut cd);
    // Q-4: the connection is lost *in this emit* — the marked entry
    // blocked the reconnect and was erased at the end
    assert_eq!(COUNTER.load(SeqCst), 0);
    assert_eq!(
        signal_handler_cb_count(&h, "f"),
        Some(1),
        "only the first remains"
    );
    // the next emit reconnects (no marked entry left) — and Q-1 reaches
    // the freshly added connection in the same emit
    signal_handler_signal(&h, "f", &mut cd);
    assert_eq!(COUNTER.load(SeqCst), 1);
    assert_eq!(
        signal_handler_cb_count(&h, "f"),
        Some(2),
        "reconnected for good"
    );
}

fn vec16_remove_current_after_nested_emit() {
    COUNTER.store(0, SeqCst);
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    assert!(signal_handler_add(&h, "void g()"));
    signal_handler_connect(&h, "f", nested_then_remove, handler_data(&h));
    signal_handler_connect(&h, "g", count_cb, std::ptr::null_mut());
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "f", &mut cd);
    // outer 1 + nested g (1) + outer 10; the (no-op) remove_current did
    // not remove the connection → a second emit still runs it
    assert_eq!(COUNTER.load(SeqCst), 1 + 1 + 10);
    signal_handler_signal(&h, "f", &mut cd);
    assert_eq!(COUNTER.load(SeqCst), 1 + 1 + 10 + 1 + 1 + 10);
}

#[cfg(not(miri))]
fn vec17_nested_emit_with_disconnect() {
    NESTED_ONCE.store(false, SeqCst);
    COUNTER.store(0, SeqCst);
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    signal_handler_connect(&h, "f", outer_disc_inner, handler_data(&h));
    signal_handler_connect(&h, "f", count_cb, data_token(2));
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "f", &mut cd);
    // outer: 1 + 10; nested emit of the same signal (R50): the outer cb
    // runs again (1 + 10, guard blocks the second nesting), the marked
    // inner entry is skipped and erased in the nested cleanup while the
    // outer loop continues
    assert_eq!(COUNTER.load(SeqCst), 1 + 10 + 1 + 10);
    assert_eq!(signal_handler_cb_count(&h, "f"), Some(1), "inner erased");
}

fn r44_cross_signal_reentry() {
    COUNTER.store(0, SeqCst);
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    assert!(signal_handler_add(&h, "void other()"));
    signal_handler_connect(&h, "f", cross_emit, handler_data(&h));
    signal_handler_connect(&h, "other", count_cb, std::ptr::null_mut());
    let mut cd = Calldata::init();
    signal_handler_signal(&h, "f", &mut cd);
    assert_eq!(COUNTER.load(SeqCst), 1 + 1);
}

fn add_array_continues_after_failure() {
    let h = signal_handler_create();
    assert!(signal_handler_add(&h, "void f()"));
    let arr = DeclArray::new(&["void g()", "void f()", "void h()"]);
    assert!(
        !unsafe { signal_handler_add_array(&h, arr.ptrs.as_ptr()) },
        "f: duplicate"
    );
    // g and h were added by the array (it continued after f failed)
    assert!(!signal_handler_add(&h, "void g()"), "g already added");
    assert!(
        !signal_handler_add(&h, "void h()"),
        "h already added (continued)"
    );
}

fn r60_create_empty() {
    let h = proc_handler_create();
    assert!(!proc_handler_call(&h, "x", &mut Calldata::init()));
}

fn r61_add_and_duplicate() {
    let h = proc_handler_create();
    let n = 42i64;
    proc_handler_add(
        &h,
        "int add(int a)",
        add_proc,
        &n as *const i64 as *mut c_void,
    );
    proc_handler_add(
        &h,
        "int add(int a)",
        add_proc,
        &n as *const i64 as *mut c_void,
    );
    // bad declaration: logged, not added
    proc_handler_add(&h, "int add(int)", add_proc, std::ptr::null_mut());
    assert!(proc_handler_call(&h, "add", &mut Calldata::init()));
    assert!(!proc_handler_call(&h, "missing", &mut Calldata::init()));
}

fn r62_call_copies_and_runs_outside_lock() {
    let h = proc_handler_create();
    let n = 40i64;
    proc_handler_add(
        &h,
        "int add(int a)",
        add_proc,
        &n as *const i64 as *mut c_void,
    );
    let mut cd = Calldata::init();
    calldata_set_int(&mut cd, b"a", 2);
    assert!(proc_handler_call(&h, "add", &mut cd));
    assert_eq!(
        calldata_get_int(&cd, b"return"),
        Some(42),
        "result through params"
    );
}

fn r60_recursive_call() {
    CALLED.store(0, SeqCst);
    let h = proc_handler_create();
    let mut ctx = ReenterCtx {
        h: &*h as *const obs_signal::proc::ProcHandler,
        depth: 1,
    };
    proc_handler_add(
        &h,
        "void reenter()",
        reentering_proc,
        &mut ctx as *mut ReenterCtx as *mut c_void,
    );
    let mut cd = Calldata::init();
    assert!(proc_handler_call(&h, "reenter", &mut cd));
    assert_eq!(CALLED.load(SeqCst), 2, "outer + re-entrant");
}

fn r63_destroy() {
    let h = proc_handler_create();
    proc_handler_add(&h, "void x()", reentering_proc, std::ptr::null_mut());
    proc_handler_destroy(h);
}

/// All S-004 vectors, sequentially, in this binary's own process.
/// Note: vectors whose semantics depend on function-pointer equality
/// (cb/data dedupe, disconnect-by-cb) are `#[cfg(not(miri))]`: Miri
/// assigns a different address to every coercion of a function item, so
/// C's pointer-identity dedupe is unobservable there (verified 2026-11-11:
/// two coercions of the same item yield different addresses under Miri).
/// They run on real hardware and are covered by the C conformance scripts.
#[test]
fn all_vectors() {
    vec01_basic();
    #[cfg(not(miri))]
    vec02_connect_dedupe();
    #[cfg(not(miri))]
    vec03_disconnect();
    #[cfg(not(miri))]
    vec04_disconnect_during_emit();
    vec05_remove_current();
    vec06_connect_during_emit();
    vec07_nested_emit();
    #[cfg(not(miri))]
    vec08_global_callbacks();
    vec09_global_remove_current();
    vec10_params_shared();
    #[cfg(not(miri))]
    vec11_refs();
    vec13_declarations();
    #[cfg(not(miri))]
    vec14_global_self_disconnect();
    #[cfg(not(miri))]
    vec15_reconnect_during_emit();
    vec16_remove_current_after_nested_emit();
    #[cfg(not(miri))]
    vec17_nested_emit_with_disconnect();
    r44_cross_signal_reentry();
    add_array_continues_after_failure();
    r60_create_empty();
    r61_add_and_duplicate();
    r62_call_copies_and_runs_outside_lock();
    r60_recursive_call();
    r63_destroy();
}
