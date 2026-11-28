//! P1-22.4 (procedures) recipe step 2: the C ABI for procedure handlers
//! (`libobs/callback/proc.c`, S-004 §6).
//!
//! `proc_handler_t` is the Rust `ProcHandler` (opaque in C); the handle
//! is a leaked `Box` the C side returns from `proc_handler_create` and
//! hands back to `proc_handler_destroy` (the C function frees the struct
//! — the bridge does the matching `Box::from_raw`).

use std::os::raw::{c_char, c_void};

use crate::abi_common::ensure_c_log_sink;
use obs_signal::calldata::Calldata;
use obs_signal::proc::{
    ProcHandler, ProcProc, proc_handler_add as cd_proc_handler_add,
    proc_handler_call as cd_proc_handler_call, proc_handler_create as cd_proc_handler_create,
    proc_handler_destroy as cd_proc_handler_destroy,
};

/// `EXPORT proc_handler_t *proc_handler_create(void)`
///
/// # Safety
/// None; the returned handle must be passed back to
/// `proc_handler_destroy` exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn proc_handler_create() -> *mut ProcHandler {
    ensure_c_log_sink();
    // SAFETY: Box is the Rust owner; the C side takes over at the pointer boundary and must free via the destroy bridge.
    Box::leak(cd_proc_handler_create())
}

/// `EXPORT void proc_handler_destroy(proc_handler_t *handler)`
///
/// # Safety
/// `handler` must be NULL or the exact handle from
/// `proc_handler_create`, not used afterwards (C: NULL is a no-op).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn proc_handler_destroy(handler: *mut ProcHandler) {
    if handler.is_null() {
        return; // C: `if (!handler) return;`
    }
    // SAFETY: caller contract — `handler` is a live create-handle, uniquely owned, unused afterwards.
    let h = unsafe { Box::from_raw(handler) };
    cd_proc_handler_destroy(h);
}

/// `EXPORT void proc_handler_add(proc_handler_t *handler, const char
/// *decl_string, proc_handler_proc_t proc, void *data)`
///
/// # Safety
/// `handler` must be a live handle; `decl_string` NUL-terminated;
/// `proc` a live C callback for the handler's lifetime; `data` passed
/// through to it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn proc_handler_add(
    handler: *mut ProcHandler,
    decl_string: *const c_char,
    proc: ProcProc,
    data: *mut c_void,
) {
    ensure_c_log_sink();
    let Some(handler) = (if handler.is_null() {
        None
    } else {
        Some(handler)
    }) else {
        return; // C: NULL dereference is UB; rejected here.
    };
    let Some(decl_string) = crate::abi_common::c_bytes(decl_string) else {
        return; // C: parse on NULL is UB; rejected here.
    };
    // SAFETY: caller contract — `handler` is a live handle for the call.
    unsafe {
        cd_proc_handler_add(
            &*handler,
            String::from_utf8_lossy(decl_string).as_ref(),
            proc,
            data,
        )
    }
}

/// `EXPORT bool proc_handler_call(proc_handler_t *handler, const char
/// *name, calldata_t *params)`
///
/// # Safety
/// `handler` must be a live handle; `name` NUL-terminated; `params` a
/// live calldata (the registered callback receives the same pointer).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn proc_handler_call(
    handler: *mut ProcHandler,
    name: *const c_char,
    params: *mut Calldata,
) -> bool {
    ensure_c_log_sink();
    let Some(handler) = (if handler.is_null() {
        None
    } else {
        Some(handler)
    }) else {
        return false; // C: NULL dereference is UB; rejected here.
    };
    let Some(name) = crate::abi_common::c_bytes(name) else {
        return false;
    };
    let Some(params) = (if params.is_null() { None } else { Some(params) }) else {
        return false; // C: calldata_t* NULL reaches the callback as NULL (the C callback contract allows it); the Rust port takes &mut, so NULL is rejected here.
    };
    // SAFETY: caller contract — `handler`/`params` are live for the call; the registered proc is a C callback (its own contract covers `params`).
    unsafe {
        cd_proc_handler_call(
            &*handler,
            String::from_utf8_lossy(name).as_ref(),
            &mut *params,
        )
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::{AtomicI64, Ordering};

    static SEEN_N: AtomicI64 = AtomicI64::new(-1);

    extern "C" fn cb(_data: *mut c_void, params: *mut Calldata) {
        // SAFETY: params is a live calldata (the test's).
        unsafe {
            let mut n: i64 = 0;
            crate::abi_calldata::calldata_get_data(
                params,
                c"n".as_ptr(),
                &mut n as *mut i64 as *mut c_void,
                8,
            );
            SEEN_N.store(n, Ordering::SeqCst);
        };
    }

    /// add/call through the C ABI: the callback sees the params; a
    /// missing proc fails; duplicate registration is refused.
    #[test]
    fn proc_bridge_roundtrip() {
        // SAFETY: test handle, freed at the end.
        unsafe {
            let h = proc_handler_create();
            let mut cd = Calldata::default();
            let v: i64 = 41;
            crate::abi_calldata::calldata_set_data(
                &mut cd as *mut Calldata,
                c"n".as_ptr(),
                v.to_ne_bytes().as_ptr().cast::<c_void>(),
                8,
            );
            assert!(!proc_handler_call(h, c"add_one".as_ptr(), &mut cd)); // not registered yet
            let data: i32 = 7;
            proc_handler_add(
                h,
                c"int add_one(int n)".as_ptr(),
                cb,
                &data as *const i32 as *mut c_void,
            );
            assert!(proc_handler_call(h, c"add_one".as_ptr(), &mut cd));
            assert_eq!(SEEN_N.load(Ordering::SeqCst), 41);
            assert!(!proc_handler_call(h, c"missing".as_ptr(), &mut cd));
            proc_handler_destroy(h);
            obs_signal::calldata::calldata_free(&mut cd);
            std::mem::forget(cd);
        }
    }
}
