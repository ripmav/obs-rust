//! P1-14.5 recipe step 2: the C ABI for the task queue
//! (`libobs/util/task.c`, S-024 M6).
//!
//! `os_task_queue_t` is the Rust `TaskQueue` (opaque in C); the handle
//! is a leaked `Box` the C side returns from `os_task_queue_create` and
//! hands back to `os_task_queue_destroy` (the C function frees the
//! struct — the bridge does the matching `Box::from_raw`).

use std::os::raw::c_void;

use obs_util_small::task_queue::{
    TaskQueue, task_queue_create as cd_task_queue_create,
    task_queue_destroy as cd_task_queue_destroy, task_queue_inside as cd_task_queue_inside,
    task_queue_queue_task as cd_task_queue_queue_task, task_queue_wait as cd_task_queue_wait,
};

/// `os_task_t`: `void (*task)(void *param)`.
pub type OsTask = unsafe extern "C" fn(param: *mut c_void);

/// The C `void *param` of a queued task: the task contract makes it
/// safe to cross the worker-thread boundary.
struct SendTaskParam(*mut c_void);
// SAFETY: the C task contract — the worker thread is the task's only
// consumer and the `os_task_queue_queue_task` caller hands ownership of
// the pointer to it (the C reference does exactly this).
unsafe impl Send for SendTaskParam {}
impl SendTaskParam {
    /// The C pointer (consumes the wrapper so the queued closure
    /// captures the whole wrapper — a field capture would be `!Send`).
    fn get(self) -> *mut c_void {
        self.0
    }
}

/// `EXPORT os_task_queue_t *os_task_queue_create(void)`
///
/// # Safety
/// None; the returned handle must be passed to `os_task_queue_destroy`
/// exactly once.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_task_queue_create() -> *mut TaskQueue {
    // SAFETY: Box is the Rust owner; the C side takes over at the pointer boundary.
    Box::leak(Box::new(cd_task_queue_create()))
}

/// `EXPORT bool os_task_queue_queue_task(os_task_queue_t *tt,
/// os_task_t task, void *param)`
///
/// # Safety
/// `tt` must be NULL (→ `false`, the C guard) or a live handle; `task`
/// a live C function for the queue's lifetime (a NULL task runs as an
/// empty closure — the C reference would call NULL, UB; the runner
/// never does).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_task_queue_queue_task(
    tt: *mut TaskQueue,
    task: OsTask,
    param: *mut c_void,
) -> bool {
    let Some(tt) = (if tt.is_null() { None } else { Some(tt) }) else {
        return false; // C: `if (!tq) return false;`
    };
    let param = SendTaskParam(param);
    // SAFETY: caller contract — `tt` is a live handle for the call; `task` is a C callback whose own contract covers `param` (SendTaskParam: the C task contract makes the pointer safe to hand to the worker thread).
    unsafe { cd_task_queue_queue_task(&*tt, Box::new(move || task(param.get()))) }
}

/// `EXPORT void os_task_queue_destroy(os_task_queue_t *tt)`
///
/// # Safety
/// `tt` must be NULL (no-op, the C guard) or a live create-handle,
/// unused afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_task_queue_destroy(tt: *mut TaskQueue) {
    if tt.is_null() {
        return; // C: `if (!tq) return;`
    }
    // SAFETY: caller contract — `tt` is a live handle, uniquely owned; the box is dropped after the (blocking) destroy.
    let t = unsafe { Box::from_raw(tt) };
    cd_task_queue_destroy(&t);
}

/// `EXPORT bool os_task_queue_wait(os_task_queue_t *tt)`
///
/// # Safety
/// `tt` must be NULL (→ `false`, the C guard) or a live handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_task_queue_wait(tt: *mut TaskQueue) -> bool {
    let Some(tt) = (if tt.is_null() { None } else { Some(tt) }) else {
        return false; // C: `if (!tq) return false;`
    };
    // SAFETY: caller contract — `tt` is a live handle for the call.
    unsafe { cd_task_queue_wait(&*tt) }
}

/// `EXPORT bool os_task_queue_inside(os_task_queue_t *tt)`
///
/// # Safety
/// `tt` must be a live handle (NULL is UB in the C reference — it
/// dereferences unconditionally; rejected here with `false`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_task_queue_inside(tt: *mut TaskQueue) -> bool {
    if tt.is_null() {
        return false;
    }
    // SAFETY: caller contract — `tt` is a live handle for the call.
    unsafe { cd_task_queue_inside(&*tt) }
}

/// Test-only (excluded from the cbindgen header, the
/// `obs_ffi_deque_new` precedent): the swap-mode form of the helpers
/// runner's `tq->tasks.size` poll — total pending entries, the destroy
/// stop marker included (the C field is private to the port).
///
/// # Safety
/// `q` must be a live Rust task-queue handle (the pointer the runner
/// got from `os_task_queue_create`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_ffi_task_queue_size(q: *mut c_void) -> usize {
    // SAFETY: caller contract — `q` is a live handle.
    unsafe { &*(q as *const TaskQueue) }.pending_entries()
}

/// Test-only (excluded from the cbindgen header, the
/// `obs_ffi_deque_new` precedent): the swap-mode form of the helpers
/// runner's `tq->waiting` poll — a wait marker is queued (the C flag is
/// private to the port).
///
/// # Safety
/// `q` must be a live Rust task-queue handle.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_ffi_task_queue_waiting(q: *mut c_void) -> bool {
    // SAFETY: caller contract — `q` is a live handle.
    unsafe { &*(q as *const TaskQueue) }.wait_pending()
}
