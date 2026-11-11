//! Procedure handlers: named synchronous procedures (S-004 §6;
//! `libobs/callback/proc.c`).
//!
//! Same `&self`/interior-mutability model as [`crate::signal`]: the proc
//! list is a plain field guarded by a RECURSIVE `ReentrantMutex<()>`
//! (S-004-R60), so a proc may call `proc_handler_call` on its own handler
//! from inside another proc.

use std::cell::UnsafeCell;
use std::ffi::c_void;

use parking_lot::ReentrantMutex;

use crate::calldata::Calldata;
use crate::decl::parse_decl;

/// `proc_handler_proc_t`: `void (*proc)(void *data, calldata_t *params)`.
pub type ProcProc = unsafe extern "C" fn(data: *mut c_void, params: *mut Calldata);

#[derive(Clone, Copy)]
struct ProcRec {
    proc: ProcProc,
    data: *mut c_void,
}

struct Proc {
    name: String,
    rec: ProcRec,
}

/// `proc_handler_t` (opaque in C).
pub struct ProcHandler {
    /// RECURSIVE mutex (S-004-R60); guards `procs`. The `UnsafeCell` is
    /// the documented escape hatch for mutating through `&self` under
    /// the lock (stacked-borrows-clean).
    lock: ReentrantMutex<()>,
    procs: UnsafeCell<Vec<Proc>>,
}

/// S-004-R60: an empty handler guarded by a recursive mutex.
#[must_use]
pub fn proc_handler_create() -> Box<ProcHandler> {
    Box::new(ProcHandler {
        lock: ReentrantMutex::new(()),
        procs: UnsafeCell::new(vec![]),
    })
}

/// S-004-R63: frees everything (no reference counting).
///
/// # Safety
///
/// Caller transfers ownership; the handler must not be used afterwards.
#[allow(clippy::boxed_local)]
pub fn proc_handler_destroy(_h: Box<ProcHandler>) {}

/// S-004-R61: register a procedure from its declaration (S-003).
/// The first registration of a name wins.
pub fn proc_handler_add(h: &ProcHandler, decl_string: &str, proc: ProcProc, data: *mut c_void) {
    let Ok(decl) = parse_decl(decl_string) else {
        eprintln!("Function declaration invalid: {decl_string}");
        return;
    };
    let name = decl.name;
    let _l = h.lock.lock();
    let procs = unsafe { &mut *h.procs.get() };
    if procs.iter().any(|p| p.name == name) {
        eprintln!("Procedure '{name}' already exists");
        return;
    }
    procs.push(Proc {
        name,
        rec: ProcRec { proc, data },
    });
}

/// S-004-R62: call a procedure by name. The record is copied under the
/// lock, the lock is released, and the proc runs **outside** the lock.
/// Returns `false` for a missing procedure (no log).
#[must_use]
pub fn proc_handler_call(h: &ProcHandler, name: &str, params: &mut Calldata) -> bool {
    let rec = {
        let _l = h.lock.lock();
        let procs = unsafe { &*h.procs.get() };
        procs.iter().find(|p| p.name == name).map(|p| p.rec)
    };
    let Some(rec) = rec else {
        return false;
    };
    // Callbacks are `extern "C"`: a panic aborts (no unwinding into C).
    unsafe { (rec.proc)(rec.data, params as *mut Calldata) }
    true
}
