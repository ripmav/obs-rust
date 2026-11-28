//! P1-22.4 recipe step 2: the C ABI for signal handlers
//! (`libobs/callback/signal.c`, S-004).
//!
//! `signal_handler_t` is the Rust `SignalHandler` (opaque in C); the
//! handle is a leaked `Box`. `signal_handler_destroy` decrements the C
//! refcount; when it reaches 0 the bridge frees the `Box` — the exact
//! point where the C reference `bfree`s the struct. (The deferred case
//! — refs reaching 0 inside an emit — is documented in
//! `signal_handler_destroy`.)

use std::os::raw::{c_char, c_void};

use crate::abi_common::ensure_c_log_sink;
use obs_signal::calldata::Calldata;
use obs_signal::signal::{
    GlobalSignalCallback, SignalCallback, SignalHandler,
    signal_handler_add as cd_signal_handler_add,
    signal_handler_connect as cd_signal_handler_connect,
    signal_handler_connect_global as cd_signal_handler_connect_global,
    signal_handler_connect_ref as cd_signal_handler_connect_ref,
    signal_handler_create as cd_signal_handler_create,
    signal_handler_destroy as cd_signal_handler_destroy,
    signal_handler_disconnect as cd_signal_handler_disconnect,
    signal_handler_disconnect_global as cd_signal_handler_disconnect_global,
    signal_handler_remove_current as cd_signal_handler_remove_current,
    signal_handler_signal as cd_signal_handler_signal,
};

fn as_str<'a>(p: *const c_char) -> Option<&'a std::ffi::CStr> {
    if p.is_null() {
        None
    } else {
        // SAFETY: non-null NUL-terminated C string (caller contract).
        Some(unsafe { std::ffi::CStr::from_ptr(p) })
    }
}

/// `EXPORT signal_handler_t *signal_handler_create(void)`
///
/// # Safety
/// None; the returned handle must be passed to
/// `signal_handler_destroy` exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn signal_handler_create() -> *mut SignalHandler {
    ensure_c_log_sink();
    // SAFETY: Box is the Rust owner; the C side takes over at the pointer boundary and must free via the destroy bridge.
    Box::leak(cd_signal_handler_create())
}

/// `EXPORT void signal_handler_destroy(signal_handler_t *handler)`
///
/// The C reference frees the struct when the refcount reaches 0; the
/// bridge does the matching `Box::from_raw`. The deferred case (refs
/// reaching 0 inside an emit via `connect_ref`/`disconnect` keep-ref
/// accounting) is **not** freed by the bridge: the Rust port clears the
/// lists and marks the handler dead at the emit epilogue, and the dead
/// state (a small amount of memory) is then leaked until process exit.
/// All operations on a dead handler are no-ops, so the only difference
/// from the C reference is the leak, not the behaviour.
///
/// # Safety
/// `handler` must be NULL or a live create-handle (C: NULL is a no-op);
/// after a destroy that frees, the handle must not be used.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn signal_handler_destroy(handler: *mut SignalHandler) {
    if handler.is_null() {
        return; // C: `if (handler && ...)`.
    }
    // SAFETY: caller contract — `handler` is a live handle for the call.
    let alive = unsafe { cd_signal_handler_destroy(&*handler) };
    if !alive {
        // refs reached 0: the C struct is freed here.
        // SAFETY: the handler state was just destroyed; the Box is the Rust owner.
        unsafe { drop(Box::from_raw(handler)) };
    }
}

/// `EXPORT bool signal_handler_add(signal_handler_t *handler, const
/// char *signal_decl)`
///
/// # Safety
/// `handler` must be a live handle; `signal_decl` NUL-terminated
/// (borrowed into the handler, as in C — it must outlive the handler).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn signal_handler_add(
    handler: *mut SignalHandler,
    signal_decl: *const c_char,
) -> bool {
    ensure_c_log_sink();
    let Some(handler) = (if handler.is_null() {
        None
    } else {
        Some(handler)
    }) else {
        return false; // C: NULL dereference is UB; rejected here.
    };
    let Some(decl) = as_str(signal_decl) else {
        return false;
    };
    // SAFETY: caller contract — `handler` is a live handle for the call.
    unsafe { cd_signal_handler_add(&*handler, String::from_utf8_lossy(decl.to_bytes()).as_ref()) }
}

/// `EXPORT void signal_handler_connect(signal_handler_t *handler, const
/// char *signal, signal_callback_t callback, void *data)`
///
/// # Safety
/// `handler` must be a live handle; `signal` NUL-terminated; `callback`
/// a live C function for the connection's lifetime; `data` passed
/// through to it.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn signal_handler_connect(
    handler: *mut SignalHandler,
    signal: *const c_char,
    callback: SignalCallback,
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
    let Some(sig) = as_str(signal) else {
        return;
    };
    // SAFETY: caller contract — `handler` is a live handle for the call.
    unsafe {
        cd_signal_handler_connect(
            &*handler,
            String::from_utf8_lossy(sig.to_bytes()).as_ref(),
            callback,
            data,
        )
    }
}

/// `EXPORT void signal_handler_connect_ref(signal_handler_t *handler,
/// const char *signal, signal_callback_t callback, void *data)`
///
/// # Safety
/// As [`signal_handler_connect`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn signal_handler_connect_ref(
    handler: *mut SignalHandler,
    signal: *const c_char,
    callback: SignalCallback,
    data: *mut c_void,
) {
    ensure_c_log_sink();
    let Some(handler) = (if handler.is_null() {
        None
    } else {
        Some(handler)
    }) else {
        return;
    };
    let Some(sig) = as_str(signal) else {
        return;
    };
    // SAFETY: caller contract — `handler` is a live handle for the call.
    unsafe {
        cd_signal_handler_connect_ref(
            &*handler,
            String::from_utf8_lossy(sig.to_bytes()).as_ref(),
            callback,
            data,
        )
    }
}

/// `EXPORT void signal_handler_disconnect(signal_handler_t *handler,
/// const char *signal, signal_callback_t callback, void *data)`
///
/// # Safety
/// As [`signal_handler_connect`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn signal_handler_disconnect(
    handler: *mut SignalHandler,
    signal: *const c_char,
    callback: SignalCallback,
    data: *mut c_void,
) {
    ensure_c_log_sink();
    let Some(handler) = (if handler.is_null() {
        None
    } else {
        Some(handler)
    }) else {
        return;
    };
    let Some(sig) = as_str(signal) else {
        return;
    };
    // SAFETY: caller contract — `handler` is a live handle for the call.
    unsafe {
        cd_signal_handler_disconnect(
            &*handler,
            String::from_utf8_lossy(sig.to_bytes()).as_ref(),
            callback,
            data,
        )
    }
}

/// `EXPORT void signal_handler_connect_global(signal_handler_t *handler,
/// global_signal_callback_t callback, void *data)`
///
/// # Safety
/// `handler` must be a live handle; `callback` a live C function; `data`
/// passed through.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn signal_handler_connect_global(
    handler: *mut SignalHandler,
    callback: GlobalSignalCallback,
    data: *mut c_void,
) {
    ensure_c_log_sink();
    let Some(handler) = (if handler.is_null() {
        None
    } else {
        Some(handler)
    }) else {
        return;
    };
    // SAFETY: caller contract — `handler` is a live handle for the call.
    unsafe { cd_signal_handler_connect_global(&*handler, callback, data) }
}

/// `EXPORT void signal_handler_disconnect_global(signal_handler_t
/// *handler, global_signal_callback_t callback, void *data)`
///
/// # Safety
/// As [`signal_handler_connect_global`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn signal_handler_disconnect_global(
    handler: *mut SignalHandler,
    callback: GlobalSignalCallback,
    data: *mut c_void,
) {
    ensure_c_log_sink();
    let Some(handler) = (if handler.is_null() {
        None
    } else {
        Some(handler)
    }) else {
        return;
    };
    // SAFETY: caller contract — `handler` is a live handle for the call.
    unsafe { cd_signal_handler_disconnect_global(&*handler, callback, data) }
}

/// `EXPORT void signal_handler_remove_current(void)`
#[unsafe(no_mangle)]
pub extern "C" fn signal_handler_remove_current() {
    cd_signal_handler_remove_current();
}

/// `EXPORT void signal_handler_signal(signal_handler_t *handler, const
/// char *signal, calldata_t *params)`
///
/// # Safety
/// `handler` must be a live handle; `signal` NUL-terminated; `params` a
/// live calldata (connected callbacks receive the same pointer).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn signal_handler_signal(
    handler: *mut SignalHandler,
    signal: *const c_char,
    params: *mut Calldata,
) {
    ensure_c_log_sink();
    let Some(handler) = (if handler.is_null() {
        None
    } else {
        Some(handler)
    }) else {
        return; // C: NULL dereference is UB; rejected here.
    };
    let Some(sig) = as_str(signal) else {
        return;
    };
    let Some(params) = (if params.is_null() { None } else { Some(params) }) else {
        return; // C: NULL params reach the callbacks as NULL (their contract); the Rust port takes &mut, so NULL is rejected here.
    };
    // SAFETY: caller contract — `handler`/`params` are live for the call.
    unsafe {
        cd_signal_handler_signal(
            &*handler,
            String::from_utf8_lossy(sig.to_bytes()).as_ref(),
            &mut *params,
        )
    }
}
