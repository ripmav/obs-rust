//! Typed signal API (P1-23): a `Signal<A>` handle generic over the
//! argument type, plus the `Connection` RAII guard, over the S-004
//! handler and the S-002 calldata byte stack.
//!
//! `Signal<'h, A>` is a zero-allocation handle: a borrow of the
//! [`SignalHandler`] plus the type `A: SignalArgs`. `A` is the only
//! thing that knows the signal's name, its S-003 declaration and the
//! parameter layout, so `write`/`read` produce exactly the bytes a C
//! plugin packs (S-002 byte layout) — the bridge is exact by
//! construction.
//!
//! `Connection` is a guard, not a reference: **drop disconnects**
//! (ADR 0002 Amendment 1). No manual disconnect is needed and no
//! C-side connection can leak — the guard's drop calls
//! [`signal_handler_disconnect_gen`] before freeing the closure. A guard
//! may outlive the handler's destruction (`refs` → 0): its disconnect
//! is a documented no-op on a dead handler, and the closure is still
//! freed. `Signal` and `Connection` are always `Send + Sync`: the
//! handler is internally synchronized (S-004 §7) and neither type
//! holds any `A` data — `A`'s thread-safety requirements fall on the
//! connected closure (bound `Send + Sync`) and on each `emit`'s `&A`
//! argument, never on the handle.
//!
//! # Drop ordering (why `Drop` is sound)
//!
//! After `signal_handler_disconnect_gen(h, sig, generation)` returns —
//! the guard disconnects its exact entry, by the generation from
//! [`signal_handler_connect_gen`] — no thread can invoke `(cb, data)`
//! again:
//!
//! - If `sig` is not emitting, the entry is erased under the signal's
//!   re-entrant lock; every callback invocation happens under that
//!   lock, so nothing is invoked after the erase (S-004-R30).
//! - If `sig` is emitting (re-entrant lock held by the emitting thread
//!   `T`), the entry is only marked `remove`. `T` reaches each entry
//!   at most once per emit pass, skips marked entries (S-004-R45), and
//!   nested re-emits of the same signal (R50) run on `T` under the
//!   same lock, so a marked entry is never re-invoked; the erase lands
//!   at the end of the outermost emit. A disconnect from another
//!   thread blocks on the signal lock until that emit finishes and
//!   then takes the first case.
//!
//! The closure box is therefore freed only after its last possible
//! invocation (and never from inside the closure's own call — see the
//! `Connection` docs).
//!
//! # Allocation behaviour (tenet T6)
//!
//! - `connect`: two small heap allocations (the closure `Box` plus the
//!   outer identity box of the guard — see `DataToken`), freed when
//!   the guard drops — never on the emit path.
//! - `emit`: one S-001-allocator round trip for the calldata
//!   (128 B minimum, S-002-R11; more if the payload is large), freed
//!   when the emit returns — the same profile a C caller gets from
//!   `calldata_create()`/`calldata_destroy()`.
//! - `emit_on`: reuses a caller-owned calldata — zero allocations after
//!   the first write (the real-time path).
//! - Per connected callback: `A::read` reconstructs the arguments; a
//!   `string` parameter allocates a `String` per callback (C plugins
//!   instead borrow a NUL-terminated pointer from the stack).
//!
//! # Panics
//!
//! A panic in a connected closure aborts the process: callbacks are
//! invoked through an `extern "C"` trampoline, and unwinding through
//! it would corrupt the emit state (the C reference cannot unwind at
//! all — S-001 "no unwinding into C").

use std::ffi::c_void;
use std::marker::PhantomData;

use crate::calldata::{Calldata, calldata_clear, calldata_get_string};
use crate::signal::{
    SignalHandler, signal_handler_connect_gen, signal_handler_disconnect_gen, signal_handler_signal,
};

/// The typed arguments of a signal (P1-23).
///
/// An impl is a fixed record: one field per parameter of
/// [`decl`](Self::decl), in declaration order. `write`/`read` use the
/// S-002 byte layout under the parameter names of `decl()`, so the
/// bytes on the calldata stack are identical to what a C plugin packs.
pub trait SignalArgs: Sized {
    /// The signal's name — the function name of [`decl`](Self::decl)
    /// (S-004-R12: case-sensitive, unique per handler).
    fn name() -> &'static str;

    /// The S-003 declaration of the signal (parameter names and types);
    /// what the handler owner passes to
    /// [`signal_handler_add`](crate::signal::signal_handler_add).
    fn decl() -> &'static str;

    /// Serialize `self` into `cd` under the parameter names of
    /// [`decl`](Self::decl).
    fn write(&self, cd: &mut Calldata);

    /// Deserialize from `cd`. A missing (or mis-sized) parameter takes
    /// the C default — int `0`, float `0.0`, bool `false`, ptr `NULL`,
    /// string `NULL` (the `calldata_int`-family defaults of S-002).
    fn read(cd: &Calldata) -> Self;
}

/// Read a `string` parameter as a Rust `&str` with C `strlen` semantics
/// (up to the first NUL of the payload). `None` when the parameter is
/// absent, `NULL` (zero-length payload) or not valid UTF-8.
#[must_use]
pub fn calldata_str<'a>(cd: &'a Calldata, name: &[u8]) -> Option<&'a str> {
    let payload = calldata_get_string(cd, name)?;
    let end = payload
        .iter()
        .position(|&b| b == 0)
        .unwrap_or(payload.len());
    std::str::from_utf8(&payload[..end]).ok()
}

/// A named signal on a [`SignalHandler`] with typed arguments `A`
/// (P1-23).
///
/// The signal must already be declared on the handler (the owner calls
/// [`signal_handler_add`](crate::signal::signal_handler_add) with
/// [`A::decl`](SignalArgs::decl), e.g. at object creation): `connect`
/// on an undeclared signal logs a warning (S-004-R20) and its guard is
/// inert, `emit` is a no-op (S-004-R40).
///
/// `Signal` is `Copy` and always `Send + Sync`: the handler is
/// internally synchronized (S-004 §7; `SignalHandler: Sync` per ADR
/// 0002 Amendment 1) and the handle holds no `A` data (module docs).
#[derive(Clone, Copy)]
pub struct Signal<'h, A: SignalArgs> {
    handler: &'h SignalHandler,
    _marker: PhantomData<fn() -> A>,
}

impl<'h, A: SignalArgs> Signal<'h, A> {
    /// A handle to the signal named [`A::name`](SignalArgs::name) on
    /// `handler` (it must be declared there already — see the type
    /// docs).
    #[must_use]
    pub const fn new(handler: &'h SignalHandler) -> Self {
        Self {
            handler,
            _marker: PhantomData,
        }
    }

    /// The underlying handler.
    #[must_use]
    pub const fn handler(&self) -> &'h SignalHandler {
        self.handler
    }

    /// Connect `f` to the signal; returns the RAII guard (drop =
    /// disconnect, ADR 0002 Amendment 1).
    ///
    /// `f` runs on whichever thread emits the signal, with the
    /// arguments read from that emit's calldata (S-004-R42: one
    /// calldata per emit, shared and mutable across callbacks). The
    /// closure costs two small heap allocations (see the module
    /// docs), freed when the guard drops. `f` must be `Send + Sync`
    /// (the guard may move across threads; the emitting thread is not
    /// fixed) and a panic in `f` aborts the process (module docs).
    pub fn connect(&self, f: impl Fn(A) + Send + Sync + 'static) -> Connection<'h, A> {
        let inner: Box<dyn Fn(A) + Send + Sync> = Box::new(f);
        let outer: Box<Box<dyn Fn(A) + Send + Sync>> = Box::new(inner);
        // `Box::into_raw` of a fresh box is never null.
        let data = Box::into_raw(outer);
        let generation = signal_handler_connect_gen(
            self.handler,
            A::name(),
            trampoline::<A>,
            data.cast::<c_void>(),
        );
        Connection {
            handler: self.handler,
            data: DataToken(data),
            generation,
            _marker: PhantomData,
        }
    }

    /// Emit with `args`: a fresh calldata (an S-001-allocator round
    /// trip — see the module docs), written with `args`, handed to the
    /// callbacks, freed when the emit returns.
    pub fn emit(&self, args: &A) {
        let mut cd = Calldata::init();
        self.emit_on(&mut cd, args);
    }

    /// Emit using the caller-owned `cd` (cleared first, then written
    /// with `args`): zero allocations after the first write (tenet
    /// T6 — the real-time path).
    pub fn emit_on(&self, cd: &mut Calldata, args: &A) {
        calldata_clear(cd);
        args.write(cd);
        signal_handler_signal(self.handler, A::name(), cd);
    }
}

/// Identity token of a connection: a pointer to the outer `Box`
/// allocated in [`Signal::connect`] (the `data` half of the C callback
/// identity, S-004-R21). The pointee is a `Box<Box<dyn Fn(A) + Send +
/// Sync>>`: the outer box keeps the token a **thin** pointer (what the
/// C `data` parameter carries), while the trampoline and the guard's
/// drop recover the inner box that owns the closure. The token is
/// `Send` by the raw-pointer auto-trait (the pointee is `Send + Sync`)
/// and `Sync` by the manual impl below (the pointee is only touched in
/// the trampoline and in `Connection::drop`, both governed by the
/// disconnect invariant documented in the module docs).
struct DataToken<A: SignalArgs>(*mut Box<dyn Fn(A) + Send + Sync>);

// SAFETY: moving a token copies only the pointer value; the pointee
// (a box whose closure is `Send + Sync` by the connect bound) is `Send`.
unsafe impl<A: SignalArgs> Send for DataToken<A> {}
// SAFETY: sharing a `&DataToken` exposes only the pointer value; the
// pointee is touched only in the trampoline and in `Connection::drop`,
// both governed by the disconnect invariant documented in the module
// docs.
unsafe impl<A: SignalArgs> Sync for DataToken<A> {}

/// RAII guard for a connection made by [`Signal::connect`] (P1-23).
///
/// **Drop disconnects** (ADR 0002 Amendment 1): the drop calls
/// [`signal_handler_disconnect_gen`] and then frees the closure box — the
/// ordering proof is in the module docs. Dropping a guard from inside a
/// callback for **another** connection (or from a different thread) is
/// well-defined (S-004-R30/R45: the entry is marked during the emit and
/// erased at its end, and the freed connection is not the running one).
/// **A guard must not be dropped from inside its own connected
/// callback** — the drop would free the closure object while it is being
/// called (undefined behavior; the trampoline borrows the closure for
/// the call, and Miri's stacked borrows pins it — unlike C, where the
/// callback code is separate from the `data` payload). To disconnect
/// oneself, use
/// [`signal_handler_remove_current`](crate::signal::signal_handler_remove_current)
/// (S-004-R32) or defer the drop until after the emit.
///
/// A guard may outlive the handler's destruction: the disconnect on a
/// dead handler is a no-op (S-004 module docs) and the closure is
/// still freed.
///
/// `Connection` is always `Send + Sync` (see the module docs).
pub struct Connection<'h, A: SignalArgs> {
    handler: &'h SignalHandler,
    data: DataToken<A>,
    generation: u64,
    _marker: PhantomData<fn() -> A>,
}

impl<'h, A: SignalArgs> Drop for Connection<'h, A> {
    fn drop(&mut self) {
        // Disconnect the exact entry this guard created first: when
        // it returns, no thread can invoke (trampoline, data) again
        // (module docs), so the box may be freed.
        signal_handler_disconnect_gen(self.handler, A::name(), self.generation);
        // SAFETY: `self.data` is the `Box::into_raw` of the outer box
        // allocated in `Signal::connect`; the preceding disconnect
        // guarantees no live reference to it remains (module docs).
        drop(unsafe { Box::from_raw(self.data.0) });
    }
}

/// The per-`A` C trampoline: reconstructs `A` from the emit's calldata
/// (S-002 byte layout via [`SignalArgs::read`]) and calls the stored
/// closure. `data` is the outer box allocated in [`Signal::connect`].
extern "C" fn trampoline<A>(data: *mut c_void, params: *mut Calldata)
where
    A: SignalArgs,
{
    // SAFETY: `params` is the emit's live calldata, valid for the whole
    // callback pass (S-004-R42).
    let cd = unsafe { &*params };
    // SAFETY: `data` is the outer box from `Signal::connect`; a
    // connection frees it only in its own `Drop`, which runs after the
    // disconnect that removes this entry (module docs).
    let f = unsafe { &*(data as *const Box<dyn Fn(A) + Send + Sync>) };
    f(A::read(cd));
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::calldata::{
        calldata_get_bool, calldata_get_float, calldata_get_int, calldata_get_ptr,
        calldata_set_bool, calldata_set_float, calldata_set_int, calldata_set_ptr,
        calldata_set_string,
    };
    use crate::signal::{
        signal_handler_add, signal_handler_cb_count, signal_handler_create, signal_handler_destroy,
    };
    use std::sync::{
        Arc,
        atomic::{AtomicUsize, Ordering::SeqCst},
    };

    /// The bridge's worked example: all five C parameter types (S-002
    /// §3), one field per parameter of the declaration.
    #[derive(Debug, Clone, PartialEq)]
    struct TArgs {
        a: i64,
        f: f64,
        b: bool,
        p: *mut c_void,
        s: Option<String>,
    }

    impl SignalArgs for TArgs {
        fn name() -> &'static str {
            "t"
        }
        fn decl() -> &'static str {
            "void t(int a, float f, bool b, ptr p, string s)"
        }
        fn write(&self, cd: &mut Calldata) {
            calldata_set_int(cd, b"a", self.a);
            calldata_set_float(cd, b"f", self.f);
            calldata_set_bool(cd, b"b", self.b);
            calldata_set_ptr(cd, b"p", self.p.cast::<u8>());
            calldata_set_string(cd, b"s", self.s.as_deref().map(str::as_bytes));
        }
        fn read(cd: &Calldata) -> Self {
            Self {
                a: calldata_get_int(cd, b"a").unwrap_or(0),
                f: calldata_get_float(cd, b"f").unwrap_or(0.0),
                b: calldata_get_bool(cd, b"b").unwrap_or(false),
                p: calldata_get_ptr(cd, b"p")
                    .map(|p| p.cast::<c_void>())
                    .unwrap_or(std::ptr::null_mut()),
                s: calldata_str(cd, b"s").map(str::to_owned),
            }
        }
    }

    fn targs() -> TArgs {
        TArgs {
            a: -7,
            f: 2.25,
            b: true,
            p: 0xcafe_usize as *mut c_void,
            s: Some("hello".to_owned()),
        }
    }

    // P1-23.2: Rust type -> calldata -> Rust type, byte-identical to
    // the C packing of the same values (S-002 §4, W = 8 little-endian).
    #[test]
    fn bytes_match_c_packing() {
        assert_eq!(crate::calldata::W, 8, "this vector is for W = 8 (S-002 §2)");
        let mut cd = Calldata::init();
        TArgs {
            a: 1,
            f: 1.5,
            b: true,
            p: 0xdeadbeef_usize as *mut c_void,
            s: Some("hi".to_owned()),
        }
        .write(&mut cd);
        // record layout: name_size, name\0, data_size, payload; a
        // zero size_t terminator ends the stack:
        //   "a" int 1        8+2+8+8 = 26
        //   "f" float 1.5    8+2+8+8 = 26
        //   "b" bool true    8+2+8+1 = 19
        //   "p" ptr 0xdeadbeef 8+2+8+8 = 26
        //   "s" "hi\0"       8+2+8+3 = 21
        //   terminator       8
        // total 126
        let mut exp: Vec<u8> = Vec::with_capacity(126);
        exp.extend_from_slice(&2usize.to_ne_bytes());
        exp.extend_from_slice(b"a\0");
        exp.extend_from_slice(&8usize.to_ne_bytes());
        exp.extend_from_slice(&1i64.to_ne_bytes());
        exp.extend_from_slice(&2usize.to_ne_bytes());
        exp.extend_from_slice(b"f\0");
        exp.extend_from_slice(&8usize.to_ne_bytes());
        exp.extend_from_slice(&1.5f64.to_bits().to_ne_bytes());
        exp.extend_from_slice(&2usize.to_ne_bytes());
        exp.extend_from_slice(b"b\0");
        exp.extend_from_slice(&1usize.to_ne_bytes());
        exp.push(1);
        exp.extend_from_slice(&2usize.to_ne_bytes());
        exp.extend_from_slice(b"p\0");
        exp.extend_from_slice(&8usize.to_ne_bytes());
        exp.extend_from_slice(&0xdeadbeef_usize.to_ne_bytes());
        exp.extend_from_slice(&2usize.to_ne_bytes());
        exp.extend_from_slice(b"s\0");
        exp.extend_from_slice(&3usize.to_ne_bytes());
        exp.extend_from_slice(b"hi\0");
        exp.extend_from_slice(&0usize.to_ne_bytes());
        // SAFETY: test — Calldata invariant (valid bmem buffer, size ≤ capacity).
        let got = unsafe { std::slice::from_raw_parts(cd.stack, cd.size) };
        assert_eq!(got, exp, "exact S-002 byte layout");
    }

    #[test]
    fn round_trip() {
        let mut cd = Calldata::init();
        let v = targs();
        v.write(&mut cd);
        assert_eq!(TArgs::read(&cd), v, "all five types round-trip");
    }

    #[test]
    fn missing_params_take_c_defaults() {
        // only "a" present; the rest must take the C defaults
        let mut cd = Calldata::init();
        calldata_set_int(&mut cd, b"a", 3);
        assert_eq!(
            TArgs::read(&cd),
            TArgs {
                a: 3,
                f: 0.0,
                b: false,
                p: std::ptr::null_mut(),
                s: None,
            }
        );
        // a mis-sized payload is absent to a typed read (S-002 Q-1:
        // type is only the byte count)
        let mut cd2 = Calldata::init();
        calldata_set_bool(&mut cd2, b"a", true); // 1-byte "a"
        assert_eq!(
            TArgs::read(&cd2).a,
            0,
            "8-byte read fails on 1-byte payload"
        );
    }

    #[test]
    fn connection_raii_drop_disconnects() {
        let h = signal_handler_create();
        assert!(signal_handler_add(&h, TArgs::decl()));
        let sig = Signal::<TArgs>::new(&h);
        let ran = Arc::new(AtomicUsize::new(0));
        {
            let r = ran.clone();
            let _conn = sig.connect(move |_| {
                r.fetch_add(1, SeqCst);
            });
            assert_eq!(signal_handler_cb_count(&h, "t"), Some(1));
            sig.emit(&targs());
            assert_eq!(ran.load(SeqCst), 1);
        }
        // guard dropped → disconnected (T2: no leaked connection)
        assert_eq!(signal_handler_cb_count(&h, "t"), Some(0));
        sig.emit(&targs());
        assert_eq!(ran.load(SeqCst), 1, "drop = disconnect");
    }

    #[test]
    fn distinct_closures_no_dedupe() {
        let h = signal_handler_create();
        assert!(signal_handler_add(&h, TArgs::decl()));
        let sig = Signal::<TArgs>::new(&h);
        let ran = Arc::new(AtomicUsize::new(0));
        // equal closures are distinct connections (unique box data
        // pointers — C identity is (cb, data), S-004-R21)
        let r1 = ran.clone();
        let c1 = sig.connect(move |_| {
            r1.fetch_add(1, SeqCst);
        });
        let r2 = ran.clone();
        let c2 = sig.connect(move |_| {
            r2.fetch_add(1, SeqCst);
        });
        sig.emit(&targs());
        assert_eq!(ran.load(SeqCst), 2);
        drop(c1);
        assert_eq!(signal_handler_cb_count(&h, "t"), Some(1));
        drop(c2);
        assert_eq!(signal_handler_cb_count(&h, "t"), Some(0));
    }

    #[test]
    fn emit_on_reuses_capacity() {
        let h = signal_handler_create();
        assert!(signal_handler_add(&h, TArgs::decl()));
        let sig = Signal::<TArgs>::new(&h);
        let mut cd = Calldata::init();
        sig.emit_on(&mut cd, &targs()); // first write: S-002-R11 allocation
        let cap = cd.capacity;
        sig.emit_on(&mut cd, &targs());
        assert_eq!(cd.capacity, cap, "T6: no reallocation on reuse");
    }

    #[test]
    fn guard_outlives_handler_destroy() {
        let h = signal_handler_create();
        assert!(signal_handler_add(&h, TArgs::decl()));
        let sig = Signal::<TArgs>::new(&h);
        let conn = sig.connect(|_| {});
        assert!(!signal_handler_destroy(&h), "refs 1 → 0: destroyed");
        // disconnect on a dead handler is a no-op; the closure box is
        // still freed exactly once (Miri: no double free)
        drop(conn);
    }

    // Disconnecting *another* connection from inside a callback
    // (S-004-R30/R45): the dropped guard is not the running closure, so
    // its box is freed once and soundly — marked during the emit,
    // skipped in this emit, erased at its end. (Dropping a guard from
    // inside its *own* callback is forbidden — Connection docs.) The
    // handler must be 'static for the 'static closure, so it and the
    // dropped guard live in dedicated statics (reachable at test end:
    // Miri leak check, not a leak).
    static DROP_SLOT: std::sync::Mutex<Option<Connection<'static, TArgs>>> =
        std::sync::Mutex::new(None);
    static DROP_HANDLER: std::sync::Mutex<Option<&'static SignalHandler>> =
        std::sync::Mutex::new(None);
    static DROP_A_RAN: AtomicUsize = AtomicUsize::new(0);
    static DROP_B_RAN: AtomicUsize = AtomicUsize::new(0);

    #[test]
    fn drop_other_connection_during_emit() {
        let h: &'static SignalHandler = Box::leak(signal_handler_create());
        assert!(signal_handler_add(h, TArgs::decl()));
        let sig = Signal::<TArgs>::new(h);
        // A runs first and drops B's guard mid-emit; B (connected
        // later) must be skipped in this emit, not run.
        let conn_a = sig.connect(|_| {
            DROP_A_RAN.fetch_add(1, SeqCst);
            // drop B's guard while the emit is running (B is not the
            // running callback — sound; its box is freed exactly once)
            drop(DROP_SLOT.lock().unwrap().take());
        });
        let conn_b = sig.connect(|_| {
            DROP_B_RAN.fetch_add(1, SeqCst);
        });
        *DROP_SLOT.lock().unwrap() = Some(conn_b);
        *DROP_HANDLER.lock().unwrap() = Some(h);
        sig.emit(&targs());
        assert_eq!(DROP_A_RAN.load(SeqCst), 1);
        assert_eq!(
            DROP_B_RAN.load(SeqCst),
            0,
            "R45: marked before its turn — skipped"
        );
        // B erased at the end of the emit; A still connected
        assert_eq!(signal_handler_cb_count(h, "t"), Some(1));
        sig.emit(&targs());
        assert_eq!(DROP_A_RAN.load(SeqCst), 2);
        assert_eq!(DROP_B_RAN.load(SeqCst), 0, "stays disconnected");
        drop(conn_a);
        assert_eq!(signal_handler_cb_count(h, "t"), Some(0));
    }

    #[test]
    fn send_sync_statically() {
        // "no Send/Sync surprises" (P1-23.1): pinned at compile time —
        // the handle and guard are always `Send + Sync`, even for this
        // `TArgs` that is not `Sync` (it holds a raw pointer).
        fn assert_send<T: Send>() {}
        fn assert_sync<T: Sync>() {}
        assert_send::<Signal<'static, TArgs>>();
        assert_sync::<Signal<'static, TArgs>>();
        assert_send::<Connection<'static, TArgs>>();
        assert_sync::<Connection<'static, TArgs>>();
    }
}
