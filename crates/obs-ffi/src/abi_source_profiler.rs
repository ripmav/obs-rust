//! P1-12.3 (S-055 U3): the C ABI of the source profiler
//! (`libobs/util/source-profiler.c`, the CMake/cbindgen/recipe swap).
//!
//! The core is [`obs_util_small::source_profiler`] (a clean-room port);
//! this module provides the 15 C-named exports the conformance runner and
//! the production libobs call. The C services the core reaches (the
//! `gs_timer*` / `gs_timer_range*` graphics timers, `os_gettime_ns`,
//! `obs_queue_task`, `obs_source_get_output_flags`,
//! `obs_source_get_last_async_ts`, `blog` through `obs_ffi_log`) are
//! installed once as the core's backend: the graphics context through the
//! C shim (`c/srcprof-bridge.c`, which keeps the `obs->video.graphics`
//! layout in C).
//!
//! `source_profiler_reset_video` takes the C `struct obs_video_info *`
//! (obs-internal.h); only the leading `fps_num` / `fps_den` fields are
//! read, through the prefix mirror below (the 33.0.1 layout: the pointer,
//! then the two u32s — the offsets hold on every target width).

use std::mem::size_of;
use std::os::raw::{c_char, c_int, c_void};
use std::sync::Once;

use obs_util_small::source_profiler as core;

/// C `profiler_result_t` (source-profiler.h) — field order pinned by the
/// swap-mode runner's printer and the cbindgen header diff.
#[repr(C)]
#[derive(Debug)]
pub struct profiler_result {
    pub tick_avg: u64,
    pub tick_max: u64,
    pub render_avg: u64,
    pub render_max: u64,
    pub render_gpu_avg: u64,
    pub render_gpu_max: u64,
    pub render_sum: u64,
    pub render_gpu_sum: u64,
    pub async_input: f64,
    pub async_rendered: f64,
    pub async_input_best: u64,
    pub async_input_worst: u64,
    pub async_rendered_best: u64,
    pub async_rendered_worst: u64,
}

// The mirror and the core struct are the same C-ABI layout (same field
// order and types); the transmute below relies on it.
const _: () = assert!(size_of::<profiler_result>() == size_of::<core::SourceProfilerResult>());

/// The leading fields of the C `struct obs_video_info` (obs.h) the C
/// reference reads (`ovi->fps_num / ovi->fps_den`); the rest is untouched.
#[repr(C)]
struct obs_video_info_prefix {
    graphics_module: *const c_char,
    fps_num: u32,
    fps_den: u32,
}

// C services of the profiler (the swap build links the conformance
// runner's fakes / the production libobs; the unit tests link the
// stand-ins in the test module below — the bmalloc/bfree and
// os_gettime_ns stand-ins already exist in the cf-lexer /
// happy-eyeballs test modules).
unsafe extern "C" {
    fn os_gettime_ns() -> u64;
    fn gs_timer_create() -> *mut c_void;
    fn gs_timer_destroy(timer: *mut c_void);
    fn gs_timer_begin(timer: *mut c_void);
    fn gs_timer_end(timer: *mut c_void);
    fn gs_timer_get_data(timer: *mut c_void, ticks: *mut u64) -> bool;
    fn gs_timer_range_create() -> *mut c_void;
    fn gs_timer_range_destroy(range: *mut c_void);
    fn gs_timer_range_begin(range: *mut c_void);
    fn gs_timer_range_end(range: *mut c_void);
    fn gs_timer_range_get_data(range: *mut c_void, disjoint: *mut bool, freq: *mut u64) -> bool;
    fn obs_queue_task(
        task_type: c_int,
        task: Option<unsafe extern "C" fn(*mut c_void)>,
        param: *mut c_void,
        wait: bool,
    );
    fn obs_source_get_output_flags(source: *const c_void) -> u32;
    fn obs_source_get_last_async_ts(source: *const c_void) -> u64;
    // c/srcprof-bridge.c (the graphics context, the `obs->video.graphics`
    // layout stays in C).
    fn obs_ffi_srcprof_enter_context();
    fn obs_ffi_srcprof_leave_context();
    // c/base-logging.c (the non-variadic `blog` entry point).
    fn obs_ffi_log(level: c_int, msg: *const c_char);
}

/// C `OBS_TASK_GRAPHICS` (obs.h: the first `enum obs_task_type` value).
const OBS_TASK_GRAPHICS: c_int = 0;

// The backend adapters: the core's `fn`-pointer fields against the C
// services.

/// `gs_timer_get_data` (the C `bool` + out value; on failure the out
/// value is untouched, so the adapter reports `None` and the core keeps
/// its initial state — the C caller's `disjoint = false, freq = 0`).
fn f_timer_get_data(timer: *mut c_void) -> Option<u64> {
    let mut ticks: u64 = 0;
    // SAFETY: the C graphics timer API; `ticks` is valid storage for the
    // out value, the timer is a live token owned by the core.
    let ok = unsafe { gs_timer_get_data(timer, &mut ticks) };
    ok.then_some(ticks)
}

/// `gs_timer_range_get_data` (the C reference ignores the `bool` return
/// and keeps its initialised out values on failure — the adapter
/// reports `None` for exactly that case).
fn f_range_get_data(range: *mut c_void) -> Option<(bool, u64)> {
    let mut disjoint: bool = false;
    let mut freq: u64 = 0;
    // SAFETY: the C graphics timer API; valid out storage, live range token.
    let ok = unsafe { gs_timer_range_get_data(range, &mut disjoint, &mut freq) };
    ok.then_some((disjoint, freq))
}

/// `obs_source_get_output_flags` — the source pointer value passes
/// through (the C function dereferences it; the core never does).
fn f_output_flags(source: usize) -> u32 {
    // SAFETY: the value is a C `obs_source_t *` owned by the caller; the
    // C function follows its documented contract.
    unsafe { obs_source_get_output_flags(source as *const c_void) }
}

/// `obs_source_get_last_async_ts` (same pass-through contract).
fn f_last_async_ts(source: usize) -> u64 {
    // SAFETY: as `f_output_flags`.
    unsafe { obs_source_get_last_async_ts(source as *const c_void) }
}

/// `obs_queue_task(OBS_TASK_GRAPHICS, task, param, false)` (R22).
fn f_queue_task(task: *const c_void, param: *mut c_void) {
    // SAFETY: `task` is the core's removal function (a C-ABI fn pointer);
    // the transmute is a type change only, the calling convention is the
    // same.
    let f = unsafe {
        std::mem::transmute::<*const c_void, Option<unsafe extern "C" fn(*mut c_void)>>(task)
    };
    // SAFETY: the C task-queue API with a valid task and the caller's param.
    unsafe { obs_queue_task(OBS_TASK_GRAPHICS, f, param, false) };
}

/// `blog(LOG_WARNING, msg)` — through the non-variadic `obs_ffi_log`
/// (the message is NUL-terminated for the C `vsnprintf("%s")`).
fn log_warning(msg: &str) {
    let mut buf = Vec::with_capacity(msg.len() + 1);
    buf.extend_from_slice(msg.as_bytes());
    buf.push(0);
    // SAFETY: `buf` is valid for its whole length and NUL-terminated; the
    // C sink reads it synchronously (the `blog(level, "%s", msg)` form).
    unsafe { obs_ffi_log(crate::LOG_WARNING, buf.as_ptr().cast()) };
}

/// The backend adapter fns: safe wrappers over the C services (one line
/// each; the SAFETY contract is the C function's documented one).
fn f_now_ns() -> u64 {
    // SAFETY: the C monotonic clock (threading-posix.c / the stub).
    unsafe { os_gettime_ns() }
}
fn f_enter_context() {
    // SAFETY: the C shim; no arguments.
    unsafe { obs_ffi_srcprof_enter_context() };
}
fn f_leave_context() {
    // SAFETY: the C shim; no arguments.
    unsafe { obs_ffi_srcprof_leave_context() };
}
fn f_timer_create() -> *mut c_void {
    // SAFETY: the C graphics timer API.
    unsafe { gs_timer_create() }
}
fn f_timer_destroy(timer: *mut c_void) {
    // SAFETY: the C graphics timer API; a live timer token.
    unsafe { gs_timer_destroy(timer) };
}
fn f_timer_begin(timer: *mut c_void) {
    // SAFETY: the C graphics timer API; a live timer token.
    unsafe { gs_timer_begin(timer) };
}
fn f_timer_end(timer: *mut c_void) {
    // SAFETY: the C graphics timer API; a live timer token.
    unsafe { gs_timer_end(timer) };
}
fn f_range_create() -> *mut c_void {
    // SAFETY: the C graphics timer API.
    unsafe { gs_timer_range_create() }
}
fn f_range_destroy(range: *mut c_void) {
    // SAFETY: the C graphics timer API; a live range token.
    unsafe { gs_timer_range_destroy(range) };
}
fn f_range_begin(range: *mut c_void) {
    // SAFETY: the C graphics timer API; a live range token.
    unsafe { gs_timer_range_begin(range) };
}
fn f_range_end(range: *mut c_void) {
    // SAFETY: the C graphics timer API; a live range token.
    unsafe { gs_timer_range_end(range) };
}

static BACKEND_INIT: Once = Once::new();

/// Installs the real C backend once (the core is process-global, per C).
fn ensure_backend() {
    BACKEND_INIT.call_once(|| {
        core::set_backend(core::SourceProfilerBackend {
            now_ns: f_now_ns,
            enter_context: f_enter_context,
            leave_context: f_leave_context,
            timer_create: f_timer_create,
            timer_destroy: f_timer_destroy,
            timer_begin: f_timer_begin,
            timer_end: f_timer_end,
            timer_get_data: f_timer_get_data,
            range_create: f_range_create,
            range_destroy: f_range_destroy,
            range_begin: f_range_begin,
            range_end: f_range_end,
            range_get_data: f_range_get_data,
            output_flags: f_output_flags,
            last_async_ts: f_last_async_ts,
            queue_task: f_queue_task,
            log_warning,
        });
    });
}

/// R11: `source_profiler_enable`.
/// # Safety
/// the caller follows the C contract (process-global state).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn source_profiler_enable(enable: bool) {
    ensure_backend();
    core::enable(enable);
}

/// R11: `source_profiler_gpu_enable`.
/// # Safety
/// the caller follows the C contract (process-global state).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn source_profiler_gpu_enable(enable: bool) {
    ensure_backend();
    core::gpu_enable(enable);
}

/// R13: `source_profiler_reset_video` (obs-internal.h).
///
/// # Safety
/// `ovi` is a live `struct obs_video_info` (the C contract).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn source_profiler_reset_video(ovi: *mut c_void) {
    ensure_backend();
    // SAFETY: per the function contract, `ovi` points to a live C struct;
    // only the leading fields are read.
    let ovi = unsafe { &*(ovi as *const obs_video_info_prefix) };
    core::reset_video(ovi.fps_num, ovi.fps_den);
}

/// R12: `source_profiler_frame_begin`.
/// # Safety
/// the caller follows the C contract (process-global state).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn source_profiler_frame_begin() {
    ensure_backend();
    core::frame_begin();
}

/// R17: `source_profiler_render_begin`.
/// # Safety
/// the caller follows the C contract (process-global state).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn source_profiler_render_begin() {
    ensure_backend();
    core::render_begin();
}

/// R17: `source_profiler_render_end`.
/// # Safety
/// the caller follows the C contract (process-global state).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn source_profiler_render_end() {
    ensure_backend();
    core::render_end();
}

/// R18: `source_profiler_frame_collect`.
/// # Safety
/// the caller follows the C contract (process-global state).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn source_profiler_frame_collect() {
    ensure_backend();
    core::frame_collect();
}

/// R19: `source_profiler_async_frame_received`.
/// # Safety
/// `source` is a live C `obs_source_t` for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn source_profiler_async_frame_received(source: *const c_void) {
    ensure_backend();
    core::async_frame_received(source);
}

/// R15: `source_profiler_source_tick_start` (C: no source parameter).
/// # Safety
/// the caller follows the C contract (process-global state).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn source_profiler_source_tick_start() -> u64 {
    ensure_backend();
    core::tick_start()
}

/// R15: `source_profiler_source_tick_end`.
/// # Safety
/// `source` is a live C `obs_source_t` for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn source_profiler_source_tick_end(source: *const c_void, start: u64) {
    ensure_backend();
    core::tick_end(source, start);
}

/// R16: `source_profiler_source_render_begin` (C: no source parameter;
/// `*timer` is written only while enabled, R14).
///
/// # Safety
/// `timer` is valid for one `*mut c_void` write (the C contract).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn source_profiler_source_render_begin(timer: *mut *mut c_void) -> u64 {
    ensure_backend();
    // SAFETY: per the function contract; the core writes `*timer` only
    // while enabled (and always writes it then).
    unsafe { core::render_pass_begin(&mut *timer) }
}

/// R16: `source_profiler_source_render_end`.
/// # Safety
/// `source` is a live C `obs_source_t`; `timer` is the (live or NULL) timer from the matching begin.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn source_profiler_source_render_end(
    source: *const c_void,
    start: u64,
    timer: *mut c_void,
) {
    ensure_backend();
    core::render_pass_end(source, start, timer);
}

/// R22: `source_profiler_remove_source`.
/// # Safety
/// `source` is a live C `obs_source_t` for the duration of the call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn source_profiler_remove_source(source: *const c_void) {
    ensure_backend();
    core::remove_source(source);
}

/// R20 + Q-2: `source_profiler_fill_result`.
///
/// # Safety
/// `result` is a valid `profiler_result_t` (the C contract); the core
/// zero-fills it before computing (and leaves it untouched when disabled).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn source_profiler_fill_result(
    source: *const c_void,
    result: *mut profiler_result,
) -> bool {
    ensure_backend();
    // SAFETY: the C reference returns false for a NULL result; the core
    // never writes through it.
    if result.is_null() {
        return false;
    }
    // SAFETY: `profiler_result` and `core::SourceProfilerResult` are the
    // same C-ABI layout (the `const _` size assert above; same field
    // order and types), and `result` is live per the function contract.
    let result = unsafe {
        std::mem::transmute::<*mut profiler_result, *mut core::SourceProfilerResult>(result)
    };
    // SAFETY: `result` points to live storage for the whole call.
    unsafe { core::fill_result(source, &mut *result) }
}

/// R20: `source_profiler_get_result` (C: `bmalloc`'d, NULL when
/// `fill_result` is false, freed by the caller with `bfree`).
/// # Safety
/// the returned pointer (non-NULL) is owned by the caller and freed with `bfree` (the C allocator).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn source_profiler_get_result(source: *const c_void) -> *mut profiler_result {
    ensure_backend();
    // SAFETY: the C allocator (the swap build links bmem.c, production
    // libobs the same); the result is `bfree`'d by the caller.
    let ret =
        unsafe { crate::abi_common::bmalloc(size_of::<profiler_result>()) } as *mut profiler_result;
    if ret.is_null() {
        // bmalloc aborts on failure (S-001); the NULL check keeps the
        // branch for a custom allocator.
        return std::ptr::null_mut();
    }
    // SAFETY: `ret` is fresh storage for a `profiler_result`.
    let ok = unsafe { source_profiler_fill_result(source, ret) };
    if ok {
        ret
    } else {
        // SAFETY: the fresh bmalloc block, not used afterwards.
        unsafe { crate::abi_common::bfree(ret) };
        std::ptr::null_mut()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Test stand-ins for the C services (the swap build links the
    // conformance runner's fakes / libobs; `cargo test` links these —
    // the os_gettime_ns / bmalloc / bfree stand-ins already exist in the
    // happy-eyeballs / cf-lexer test modules and are reused, not
    // redefined).
    /// The `obs` core pointer (the C shim reads it; no Rust thread writes
    /// it, so a plain data slot is sound).
    #[allow(dead_code)] // the C shim reads the field; no Rust reader
    pub(crate) struct ObsSlot(*mut c_void);
    // SAFETY: test stand-in only; write-once from the C side never
    // happens in the test build, the shim only reads.
    unsafe impl Sync for ObsSlot {}
    #[unsafe(no_mangle)]
    pub static obs: ObsSlot = ObsSlot(std::ptr::null_mut());

    #[unsafe(no_mangle)]
    pub extern "C" fn gs_timer_create() -> *mut c_void {
        std::ptr::null_mut()
    }
    // The C shim (srcprof-bridge.c) calls the graphics context pair;
    // inert stand-ins (the conformance stubs / production graphics
    // backend provide the real ones).
    #[unsafe(no_mangle)]
    pub extern "C" fn gs_enter_context(_graphics: *mut c_void) {}
    #[unsafe(no_mangle)]
    pub extern "C" fn gs_leave_context() {}
    #[unsafe(no_mangle)]
    pub extern "C" fn gs_timer_destroy(_timer: *mut c_void) {}
    #[unsafe(no_mangle)]
    pub extern "C" fn gs_timer_begin(_timer: *mut c_void) {}
    #[unsafe(no_mangle)]
    pub extern "C" fn gs_timer_end(_timer: *mut c_void) {}
    #[unsafe(no_mangle)]
    pub extern "C" fn gs_timer_get_data(_timer: *mut c_void, _ticks: *mut u64) -> bool {
        false
    }
    #[unsafe(no_mangle)]
    pub extern "C" fn gs_timer_range_create() -> *mut c_void {
        std::ptr::null_mut()
    }
    #[unsafe(no_mangle)]
    pub extern "C" fn gs_timer_range_destroy(_range: *mut c_void) {}
    #[unsafe(no_mangle)]
    pub extern "C" fn gs_timer_range_begin(_range: *mut c_void) {}
    #[unsafe(no_mangle)]
    pub extern "C" fn gs_timer_range_end(_range: *mut c_void) {}
    #[unsafe(no_mangle)]
    pub extern "C" fn gs_timer_range_get_data(
        _range: *mut c_void,
        disjoint: *mut bool,
        freq: *mut u64,
    ) -> bool {
        // SAFETY: the test's own out storage (the bridge's local).
        unsafe {
            *disjoint = false;
            *freq = 1_000_000_000;
        }
        true
    }
    #[unsafe(no_mangle)]
    pub extern "C" fn obs_queue_task(
        _task_type: c_int,
        _task: Option<unsafe extern "C" fn(*mut c_void)>,
        _param: *mut c_void,
        _wait: bool,
    ) {
    }
    #[unsafe(no_mangle)]
    pub extern "C" fn obs_source_get_output_flags(_source: *const c_void) -> u32 {
        0
    }
    #[unsafe(no_mangle)]
    pub extern "C" fn obs_source_get_last_async_ts(_source: *const c_void) -> u64 {
        0
    }

    const SRC: *const c_void = 0x1000 as *const c_void;

    // The profiler state is process-global (per C); serialize the two
    // tests that drive it (W9 fence pattern).
    static TEST_LOCK: std::sync::Mutex<()> = std::sync::Mutex::new(());

    // The C-ABI plumbing against the (fake) C services: the disabled
    // state is inert (R14), the enabled state zero-fills unknown sources
    // (Q-2), and `get_result` mirrors `fill_result` through the C
    // allocator (the bmalloc/bfree stand-ins of the cf-lexer tests).
    // Behavioural semantics are covered by the core's tests and the
    // conformance srcprof_* vectors.
    macro_rules! c {
        ($e:expr) => {
            // SAFETY: every call is a C-ABI export with its documented
            // contract; the arguments are valid per the test's setup.
            unsafe { $e }
        };
    }

    #[test]
    fn disabled_inert_and_q2_zeroed() {
        let _lock = TEST_LOCK.lock().unwrap();
        // The backend installs the stand-ins once per process; the state
        // is whatever an earlier test in this binary left — start from a
        // known-disabled state (the core's test_reset is not reachable
        // here; the exports are the only handle, per the C surface).
        c!(source_profiler_enable(false));
        c!(source_profiler_frame_begin());
        c!(source_profiler_frame_collect()); // applies the disable + shutdown

        assert_eq!(c!(source_profiler_source_tick_start()), 0);
        let mut sentinel: *mut c_void = 0x1 as _;
        assert_eq!(c!(source_profiler_source_render_begin(&mut sentinel)), 0);
        assert_eq!(
            sentinel as usize, 0x1,
            "R14: *timer untouched while disabled"
        );
        let mut r = profiler_result {
            tick_avg: u64::MAX,
            // SAFETY: zeroed is a valid value for the C-ABI result struct.
            ..unsafe { std::mem::zeroed() }
        };
        assert!(!c!(source_profiler_fill_result(SRC, &mut r)));
        assert_eq!(r.tick_avg, u64::MAX, "R14: the output stays untouched");
        assert!(c!(source_profiler_get_result(SRC)).is_null());
        assert!(!c!(source_profiler_fill_result(SRC, std::ptr::null_mut())));
    }

    // enabled + unknown source: fill is false with a zeroed result,
    // `get_result` NULL — through the real (fake-backed) backend.
    #[test]
    fn enabled_unknown_source_zeroed() {
        let _lock = TEST_LOCK.lock().unwrap();
        c!(source_profiler_enable(true));
        c!(source_profiler_frame_begin());
        let mut ovi = obs_video_info_prefix {
            graphics_module: std::ptr::null(),
            fps_num: 60,
            fps_den: 1,
        };
        c!(source_profiler_reset_video(
            &mut ovi as *mut _ as *mut c_void
        ));
        c!(source_profiler_frame_begin());
        c!(source_profiler_frame_collect());
        let mut r = profiler_result {
            tick_avg: u64::MAX,
            // SAFETY: zeroed is a valid value for the C-ABI result struct.
            ..unsafe { std::mem::zeroed() }
        };
        assert!(!c!(source_profiler_fill_result(SRC, &mut r)));
        assert_eq!(r.tick_avg, 0, "Q-2: zeroed for an unknown source");
        assert!(c!(source_profiler_get_result(SRC)).is_null());
        c!(source_profiler_enable(false));
        c!(source_profiler_frame_begin());
        c!(source_profiler_frame_collect());
    }
}
