//! S-055 U3: source profiler (spec-side port of `libobs 33.0.1
//! util/source-profiler.c`).
//!
//! Spec: `S-055-libobs-util-remainder.md` §4 (R11–R22). Per-source frame
//! tick / render (CPU + GPU) / async-timestamp history in circular buffers
//! of `sample_capacity = ceil(fps_num/fps_den) * 5` samples, plus the
//! `profiler_result_t` computation. Sources are addressed by pointer value
//! (`obs_source_t *` is opaque to this crate).
//!
//! The C services the reference reaches (`os_gettime_ns`, the `gs_timer*`
//! / `gs_timer_range*` graphics timers, `gs_enter/leave_context`,
//! `obs_queue_task`, `obs_source_get_output_flags`,
//! `obs_source_get_last_async_ts`, `blog`) are injected as a
//! [`SourceProfilerBackend`] (the c-swap bridge installs the real services;
//! tests install a fake — the profiler-core clock-sink precedent).
//!
//! Deviations from the C reference (documented per house policy):
//! - the C `pthread_rwlock` on the entry history (and the unguarded sample
//!   hash "owned by the video thread") is one `Mutex` here: the same
//!   operations, coarser — and `task_delete_source` deleting an entry under
//!   the C *read* lock is a race the port does not inherit;
//! - the `profile_start`/`profile_end` around `frame_collect` (self-
//!   profiling, not observable through the vectors) is not ported;
//! - `reset_video` with `fps_den == 0` (C: the double division is inf and
//!   the `uint64_t` cast is UB): the port saturates to `sample_capacity = 0`.
//!
//! Pitfalls pinned: Q-7 (the history lags one frame behind and a zero tick
//! means "no data"), Q-2 (`fill_result` returns false with a zeroed result
//! for unknown sources, untouched when disabled).
//!
//! Tests are serialized on `TEST_LOCK` (W9 fence pattern) because the
//! profiler state is process-global.

use std::collections::HashMap;
use std::os::raw::c_void;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::{LazyLock, Mutex};

use crate::uint128::util_mul_div64;

/// C `NUM_TEXTURES`: frame slots per source (R18).
pub const FRAME_BUFFER_SIZE: usize = 2;
/// C `OBS_SOURCE_ASYNC_VIDEO` (obs-source.h): `OBS_SOURCE_ASYNC | OBS_SOURCE_VIDEO`.
const OBS_SOURCE_ASYNC_VIDEO: u32 = (1 << 0) | (1 << 2);
/// The "GPU Timers were disjoint" warning (R18; compared by text, S-055 Q-6).
const DISJOINT_WARNING: &str = "GPU Timers were disjoint, discarding samples.";

/// The C services of the source profiler (S-055 §4). Installed once by the
/// c-swap bridge (the real `gs_*` / `os_*` / `obs_*` functions) or per test
/// (a fake).
#[derive(Clone, Copy)]
pub struct SourceProfilerBackend {
    /// `os_gettime_ns`.
    pub now_ns: fn() -> u64,
    /// `gs_enter_context(obs->video.graphics)` (R17: enter/leave must
    /// balance; the graphics pointer is resolved by the installer).
    pub enter_context: fn(),
    /// `gs_leave_context`.
    pub leave_context: fn(),
    /// `gs_timer_create`.
    pub timer_create: fn() -> *mut c_void,
    /// `gs_timer_destroy`.
    pub timer_destroy: fn(*mut c_void),
    /// `gs_timer_begin`.
    pub timer_begin: fn(*mut c_void),
    /// `gs_timer_end`.
    pub timer_end: fn(*mut c_void),
    /// `gs_timer_get_data`: the ticks, or `None` when the data is not
    /// available (the C `bool` return).
    pub timer_get_data: fn(*mut c_void) -> Option<u64>,
    /// `gs_timer_range_create`.
    pub range_create: fn() -> *mut c_void,
    /// `gs_timer_range_destroy`.
    pub range_destroy: fn(*mut c_void),
    /// `gs_timer_range_begin`.
    pub range_begin: fn(*mut c_void),
    /// `gs_timer_range_end`.
    pub range_end: fn(*mut c_void),
    /// `gs_timer_range_get_data`: `(disjoint, frequency)`, or `None` when
    /// the range is not ready (the C reference ignores the `bool` return —
    /// its out values stay as they were).
    pub range_get_data: fn(*mut c_void) -> Option<(bool, u64)>,
    /// `obs_source_get_output_flags` (the async-video check of R18).
    pub output_flags: fn(source: usize) -> u32,
    /// `obs_source_get_last_async_ts` (R18, async sources only).
    pub last_async_ts: fn(source: usize) -> u64,
    /// `obs_queue_task(OBS_TASK_GRAPHICS, task, param, false)` (R22): the
    /// removal task runs later on the graphics queue, never synchronously.
    pub queue_task: fn(task: *const c_void, param: *mut c_void),
    /// `blog(LOG_WARNING, msg)` (R18 disjoint warning).
    pub log_warning: fn(&str),
}

static BACKEND: Mutex<Option<SourceProfilerBackend>> = Mutex::new(None);

/// Installs (replaces) the C-service backend. The c-swap bridge calls this
/// once; tests install a fake and [`test_reset`] clears it.
pub fn set_backend(backend: SourceProfilerBackend) {
    *BACKEND.lock().unwrap() = Some(backend);
}

fn backend() -> SourceProfilerBackend {
    *BACKEND
        .lock()
        .unwrap()
        .as_ref()
        .expect("source profiler backend not installed")
}

// ---------------------------------------------------------------------------
// C structures

/// C `struct ucirclebuf`: fixed-size circular buffer of the most recent N
/// values (older items overwritten). `num` is the number of stored values
/// (`min(capacity, pushes)`).
struct CircleBuf {
    idx: usize,
    capacity: usize,
    num: usize,
    array: Vec<u64>,
}

impl CircleBuf {
    /// `ucirclebuf_init`: `capacity == 0` leaves the buffer empty with no
    /// array (the C `memset` without `bmalloc`).
    fn init(capacity: usize) -> Self {
        if capacity == 0 {
            return Self {
                idx: 0,
                capacity: 0,
                num: 0,
                array: Vec::new(),
            };
        }
        Self {
            idx: 0,
            capacity,
            num: 0,
            array: vec![0; capacity],
        }
    }

    fn push(&mut self, val: u64) {
        if self.num == self.capacity {
            // C: `idx %= capacity` — a divide-by-zero SIGFPE when the
            // capacity is 0 (`sample_capacity` 0 = fps 0); the port panics
            // on the same C-UB edge.
            self.idx %= self.capacity;
            self.array[self.idx] = val;
            self.idx += 1;
        } else {
            self.array[self.idx] = val;
            self.idx += 1;
            self.num += 1;
        }
    }
}

/// C `struct frame_sample`: one frame slot of a source.
struct FrameSample {
    tick: u64,
    render_cpu: Vec<u64>,
    render_timers: Vec<*mut c_void>,
}

/// C `struct source_samples`: the per-source frame ring (R15–R16).
struct SourceSamples {
    frame_idx: u8,
    frames: [FrameSample; FRAME_BUFFER_SIZE],
}

impl SourceSamples {
    /// `source_samples_create`: `frame_idx = 0`, both slots fresh with the
    /// `render_times_reservation = 2` pre-reserved lists.
    fn create() -> Self {
        let slot = FrameSample {
            tick: 0,
            render_cpu: Vec::with_capacity(2),
            render_timers: Vec::with_capacity(2),
        };
        Self {
            frame_idx: 0,
            frames: [
                slot,
                FrameSample {
                    tick: 0,
                    render_cpu: Vec::with_capacity(2),
                    render_timers: Vec::with_capacity(2),
                },
            ],
        }
    }
}

/// C `struct profiler_entry`: the per-source history (R18).
struct ProfilerEntry {
    tick: CircleBuf,
    render_cpu: CircleBuf,
    render_gpu: CircleBuf,
    render_cpu_sum: CircleBuf,
    render_gpu_sum: CircleBuf,
    async_frame_ts: CircleBuf,
    async_rendered_ts: CircleBuf,
}

impl ProfilerEntry {
    /// `entry_create`: seven circular buffers of `sample_capacity` each.
    fn create(sample_capacity: usize) -> Self {
        Self {
            tick: CircleBuf::init(sample_capacity),
            render_cpu: CircleBuf::init(sample_capacity),
            render_gpu: CircleBuf::init(sample_capacity),
            render_cpu_sum: CircleBuf::init(sample_capacity),
            render_gpu_sum: CircleBuf::init(sample_capacity),
            async_frame_ts: CircleBuf::init(sample_capacity),
            async_rendered_ts: CircleBuf::init(sample_capacity),
        }
    }
}

// ---------------------------------------------------------------------------
// State (single instance, per C)

struct Inner {
    enabled: bool,
    gpu_enabled: bool,
    /// R11: requested state, applied by `frame_begin` / `frame_collect`.
    enable_next: AtomicBool,
    gpu_enable_next: AtomicBool,
    /// R13: `ceil(fps_num/fps_den) * 5` (R18 buffer size at entry creation).
    sample_capacity: u64,
    timer_idx: u8,
    timer_ranges: [*mut c_void; FRAME_BUFFER_SIZE],
    samples: HashMap<usize, SourceSamples>,
    entries: HashMap<usize, ProfilerEntry>,
}

// SAFETY: `timer_ranges` holds opaque C timer-range tokens that are only
// ever passed to the injected C services (never dereferenced by this
// crate); the C reference keeps the same tokens in a process-global
// struct, so sharing `Inner` across threads is the C model itself.
unsafe impl Send for Inner {}

static STATE: LazyLock<Mutex<Inner>> = LazyLock::new(|| {
    Mutex::new(Inner {
        enabled: false,
        gpu_enabled: false,
        enable_next: AtomicBool::new(false),
        gpu_enable_next: AtomicBool::new(false),
        sample_capacity: 0,
        timer_idx: 0,
        timer_ranges: [core::ptr::null_mut(); FRAME_BUFFER_SIZE],
        samples: HashMap::new(),
        entries: HashMap::new(),
    })
});

/// `frame_sample_destroy`: the slot's leftover GPU timers are destroyed
/// under the graphics context when present.
fn destroy_sample(smp: &mut SourceSamples, b: &SourceProfilerBackend) {
    for frame in smp.frames.iter_mut() {
        if !frame.render_timers.is_empty() {
            (b.enter_context)();
            for t in frame.render_timers.iter() {
                (b.timer_destroy)(*t);
            }
            (b.leave_context)();
            frame.render_timers.clear();
        }
    }
}

/// `reset_gpu_timers`.
fn reset_gpu_timers(state: &mut Inner, b: &SourceProfilerBackend) {
    (b.enter_context)();
    for r in state.timer_ranges.iter_mut() {
        if !r.is_null() {
            (b.range_destroy)(*r);
            *r = core::ptr::null_mut();
        }
    }
    (b.leave_context)();
}

/// `profiler_shutdown`: all samples + entries + GPU timer ranges (R13).
fn shutdown(state: &mut Inner, b: &SourceProfilerBackend) {
    for smp in state.samples.values_mut() {
        destroy_sample(smp, b);
    }
    state.samples.clear();
    state.entries.clear();
    reset_gpu_timers(state, b);
}

/// `task_delete_source` (R22): the graphics-queue removal of one source.
///
/// # Safety
/// `param` is the source pointer value queued by [`remove_source`]; the
/// backend guarantees it is a pointer value only (no dereference).
unsafe extern "C" fn task_delete(param: *mut c_void) {
    let key = param as usize;
    let b = backend();
    let mut state = STATE.lock().unwrap();
    if let Some(smp) = state.samples.remove(&key) {
        let mut one = smp;
        destroy_sample(&mut one, &b);
    }
    // C deletes the entry under the rwlock *read* lock (a race); the port
    // takes the same single lock as everything else (documented deviation).
    state.entries.remove(&key);
}

// ---------------------------------------------------------------------------
// C API (process-global, per C)

/// R11: `source_profiler_enable(b)` — sets `enable_next` only.
pub fn enable(enable: bool) {
    STATE
        .lock()
        .unwrap()
        .enable_next
        .store(enable, Ordering::SeqCst);
}

/// R11: `source_profiler_gpu_enable(b)` — `gpu_enable_next = b AND enable_next`
/// (a GPU request before the profiler is enabled is ignored).
pub fn gpu_enable(enable: bool) {
    let state = STATE.lock().unwrap();
    state.gpu_enable_next.store(
        enable && state.enable_next.load(Ordering::SeqCst),
        Ordering::SeqCst,
    );
}

/// R13: `source_profiler_reset_video` — the capacity from the fps and a full
/// shutdown (called with the video thread stopped).
pub fn reset_video(fps_num: u32, fps_den: u32) {
    let fps = (fps_num as f64 / fps_den as f64).ceil();
    // C: `(uint64_t)(fps * 5)`; `fps_den == 0` is C-UB (inf) — the port
    // saturates to 0 (documented deviation).
    let capacity = (fps * 5.0) as u64;
    let b = backend();
    let mut state = STATE.lock().unwrap();
    state.sample_capacity = capacity;
    shutdown(&mut state, &b);
}

/// R12: `source_profiler_frame_begin`.
pub fn frame_begin() {
    let mut state = STATE.lock().unwrap();
    if !state.enabled && state.enable_next.load(Ordering::SeqCst) {
        state.enabled = true;
    }
    if !state.gpu_enabled && state.enabled && state.gpu_enable_next.load(Ordering::SeqCst) {
        state.gpu_enabled = true;
    } else if state.gpu_enabled {
        // The index does not advance in the frame in which GPU becomes
        // enabled (R12).
        state.timer_idx = (state.timer_idx + 1) % FRAME_BUFFER_SIZE as u8;
    }
}

/// R17: `source_profiler_render_begin` — the global timer range of the
/// current index, only when GPU profiling is effective.
pub fn render_begin() {
    let mut state = STATE.lock().unwrap();
    if !state.gpu_enabled {
        return;
    }
    let b = backend();
    let idx = state.timer_idx as usize;
    (b.enter_context)();
    if state.timer_ranges[idx].is_null() {
        state.timer_ranges[idx] = (b.range_create)();
    }
    (b.range_begin)(state.timer_ranges[idx]);
    (b.leave_context)();
}

/// R17: `source_profiler_render_end`.
pub fn render_end() {
    let state = STATE.lock().unwrap();
    if !state.gpu_enabled {
        return;
    }
    let idx = state.timer_idx as usize;
    if state.timer_ranges[idx].is_null() {
        return;
    }
    let b = backend();
    (b.enter_context)();
    (b.range_end)(state.timer_ranges[idx]);
    (b.leave_context)();
}

/// R18: `source_profiler_frame_collect` — the per-frame history update, one
/// frame behind (Q-7: a zero tick means "no data").
pub fn frame_collect() {
    let b = backend();
    let mut state = STATE.lock().unwrap();
    if !state.enabled {
        return;
    }

    // The previous frame's range, queried before the history update (R18;
    // the C ignores the get_data return — on `None` the out values keep
    // their initial state). The context is held across the whole history
    // update and balanced after it (the C enter/leave order).
    let mut gpu_disjoint = false;
    let mut gpu_ready = false;
    let mut freq = 0u64;
    if state.gpu_enabled {
        let range_idx = (state.timer_idx + 1) % FRAME_BUFFER_SIZE as u8;
        let range = state.timer_ranges[range_idx as usize];
        if !range.is_null() {
            gpu_ready = true;
            (b.enter_context)();
            if let Some((disjoint, f)) = (b.range_get_data)(range) {
                gpu_disjoint = disjoint;
                freq = f;
            }
        }
        if gpu_disjoint {
            (b.log_warning)(DISJOINT_WARNING);
        }
    }

    {
        // The per-source update over the older slot. (C iterates its hash
        // in insertion order; the per-source updates are independent, so
        // the `HashMap` order is not observable — only counts are.)
        // Destructured field borrows: `samples` and `entries` are disjoint
        // places, so a sample and its entry stay live at the same time.
        let Inner {
            enabled: _,
            gpu_enabled: _,
            enable_next: _,
            gpu_enable_next: _,
            sample_capacity,
            timer_idx: _,
            timer_ranges: _,
            samples,
            entries,
        } = &mut *state;
        let capacity = *sample_capacity as usize;
        let keys: Vec<usize> = samples.keys().copied().collect();
        for key in keys {
            let Some(smp) = samples.get_mut(&key) else {
                continue;
            };
            let slot = (smp.frame_idx + 1) % FRAME_BUFFER_SIZE as u8;
            if smp.frames[slot as usize].tick == 0 {
                continue;
            }
            let frame = &mut smp.frames[slot as usize];

            let ent = entries
                .entry(key)
                .or_insert_with(|| ProfilerEntry::create(capacity));

            ent.tick.push(frame.tick);

            if !frame.render_cpu.is_empty() {
                let sum = frame.render_cpu.iter().sum();
                let first = frame.render_cpu[0];
                ent.render_cpu.push(first);
                ent.render_cpu_sum.push(sum);
                frame.render_cpu.clear();
            } else {
                ent.render_cpu.push(0);
                ent.render_cpu_sum.push(0);
            }

            // Leftover GPU timers are destroyed even when GPU profiling
            // was disabled in the meantime (R18).
            if !frame.render_timers.is_empty() {
                let mut sum = 0u64;
                let mut first = 0u64;
                for t in frame.render_timers.iter() {
                    let ticks = if gpu_ready && !gpu_disjoint {
                        (b.timer_get_data)(*t)
                    } else {
                        None
                    };
                    if let Some(ticks) = ticks {
                        sum += util_mul_div64(ticks, 1_000_000_000, freq);
                        if first == 0 {
                            first = sum;
                        }
                    }
                    (b.timer_destroy)(*t);
                }
                if first != 0 {
                    ent.render_gpu.push(first);
                    ent.render_gpu_sum.push(sum);
                }
                frame.render_timers.clear();
            } else {
                ent.render_gpu.push(0);
                ent.render_gpu_sum.push(0);
            }

            if (b.output_flags)(key) & OBS_SOURCE_ASYNC_VIDEO == OBS_SOURCE_ASYNC_VIDEO {
                ent.async_rendered_ts.push((b.last_async_ts)(key));
            }
        }
    }

    if state.gpu_enabled && gpu_ready {
        (b.leave_context)();
    }

    // Apply the requested state for the next frame (R18).
    if !state.enable_next.load(Ordering::SeqCst) {
        state.enabled = false;
        state.gpu_enabled = false;
        shutdown(&mut state, &b);
    } else if !state.gpu_enable_next.load(Ordering::SeqCst) {
        state.gpu_enabled = false;
        reset_gpu_timers(&mut state, &b);
    }
}

/// R19: `source_profiler_async_frame_received` — only when an entry exists.
pub fn async_frame_received(source: *const c_void) {
    let b = backend();
    let mut state = STATE.lock().unwrap();
    if !state.enabled {
        return;
    }
    let key = source as usize;
    if let Some(ent) = state.entries.get_mut(&key) {
        ent.async_frame_ts.push((b.now_ns)());
    }
}

/// R15: `source_profiler_source_tick_start(void)` — `0` while disabled.
/// (No source parameter: the C signature is source-less.)
pub fn tick_start() -> u64 {
    let state = STATE.lock().unwrap();
    if !state.enabled {
        return 0;
    }
    (backend().now_ns)()
}

/// R15: `source_profiler_source_tick_end` — the slot advances here (the tick
/// happens first and only once at the start of each frame).
pub fn tick_end(source: *const c_void, start: u64) {
    let b = backend();
    let mut state = STATE.lock().unwrap();
    if !state.enabled {
        return;
    }
    let delta = (b.now_ns)() - start;
    let key = source as usize;
    state
        .samples
        .entry(key)
        .or_insert_with(SourceSamples::create);
    let smp = state.samples.get_mut(&key).unwrap();
    smp.frame_idx = (smp.frame_idx + 1) % FRAME_BUFFER_SIZE as u8;
    smp.frames[smp.frame_idx as usize].tick = delta;
}

/// R16: `source_profiler_source_render_begin(gs_timer_t **timer)` — while
/// disabled it returns 0 and does **not** touch `*timer` (R14); enabled, it
/// always writes `*timer` (the pass timer, NULL without GPU profiling).
pub fn render_pass_begin(timer: &mut *mut c_void) -> u64 {
    let b = backend();
    let state = STATE.lock().unwrap();
    if !state.enabled {
        return 0;
    }
    let t = if state.gpu_enabled {
        let t = (b.timer_create)();
        (b.timer_begin)(t);
        t
    } else {
        core::ptr::null_mut()
    };
    let now = (b.now_ns)();
    drop(state);
    *timer = t;
    now
}

/// R16: `source_profiler_source_render_end` — the source without samples
/// drops the CPU delta and destroys a non-NULL timer.
pub fn render_pass_end(source: *const c_void, start: u64, timer: *mut c_void) {
    let b = backend();
    let mut state = STATE.lock().unwrap();
    if !state.enabled {
        return;
    }
    if !timer.is_null() {
        (b.timer_end)(timer);
    }
    let delta = (b.now_ns)() - start;
    let key = source as usize;
    match state.samples.get_mut(&key) {
        Some(smp) => {
            let frame = &mut smp.frames[smp.frame_idx as usize];
            frame.render_cpu.push(delta);
            if !timer.is_null() {
                frame.render_timers.push(timer);
            }
        }
        None => {
            if !timer.is_null() {
                (b.timer_destroy)(timer);
            }
        }
    }
}

/// R22: `source_profiler_remove_source` — queued on the graphics queue
/// (non-waiting); nothing queued while disabled.
pub fn remove_source(source: *const c_void) {
    let state = STATE.lock().unwrap();
    if !state.enabled {
        return;
    }
    drop(state);
    (backend().queue_task)(task_delete as *const c_void, source as *mut c_void);
}

/// C `calculate_tick` (R20).
fn calculate_tick(ent: &ProfilerEntry, result: &mut SourceProfilerResult) {
    let mut sum = 0u64;
    for &delta in &ent.tick.array[..ent.tick.num] {
        if delta > result.tick_max {
            result.tick_max = delta;
        }
        sum += delta;
    }
    if ent.tick.num > 0 {
        result.tick_avg = sum / ent.tick.num as u64;
    }
}

/// C `calculate_render` (R20): the GPU part only while GPU is effective.
fn calculate_render(state: &Inner, ent: &ProfilerEntry, result: &mut SourceProfilerResult) {
    let mut sum = 0u64;
    let mut sum_sum = 0u64;
    for (&delta, &sum_v) in ent.render_cpu.array[..ent.render_cpu.num]
        .iter()
        .zip(&ent.render_cpu_sum.array[..ent.render_cpu.num])
    {
        if delta > result.render_max {
            result.render_max = delta;
        }
        sum += delta;
        sum_sum += sum_v;
    }
    if ent.render_cpu.num > 0 {
        result.render_avg = sum / ent.render_cpu.num as u64;
        result.render_sum = sum_sum / ent.render_cpu.num as u64;
    }
    if !state.gpu_enabled {
        return;
    }
    sum = 0;
    sum_sum = 0;
    for (&delta, &sum_v) in ent.render_gpu.array[..ent.render_gpu.num]
        .iter()
        .zip(&ent.render_gpu_sum.array[..ent.render_gpu.num])
    {
        if delta > result.render_gpu_max {
            result.render_gpu_max = delta;
        }
        sum += delta;
        sum_sum += sum_v;
    }
    if ent.render_gpu.num > 0 {
        result.render_gpu_avg = sum / ent.render_gpu.num as u64;
        result.render_gpu_sum = sum_sum / ent.render_gpu.num as u64;
    }
}

/// C `calculate_fps` (R21): best/worst/average frame time from a timestamp
/// ring (the wrap-around first delta is included — a wrapped buffer yields a
/// large first delta, see `srcprof_async`).
fn calculate_fps(frames: &CircleBuf, avg: &mut f64, best: &mut u64, worst: &mut u64) {
    let mut deltas = 0u64;
    let mut delta_sum = 0u64;
    let mut best_delta = 0u64;
    let mut worst_delta = 0u64;

    for i in 0..frames.num {
        let ts = frames.array[i];
        if ts == 0 {
            break;
        }
        let prev_idx = if i > 0 { i - 1 } else { frames.num - 1 };
        let prev_ts = frames.array[prev_idx];
        if prev_ts == 0 || prev_ts >= ts {
            continue;
        }
        let delta = ts - prev_ts;
        if delta < best_delta || best_delta == 0 {
            best_delta = delta;
        }
        if delta > worst_delta {
            worst_delta = delta;
        }
        delta_sum += delta;
        deltas += 1;
    }

    if deltas > 0 && delta_sum > 0 {
        *avg = 1.0E9 / (delta_sum as f64 / deltas as f64);
        *best = best_delta;
        *worst = worst_delta;
    }
}

/// R20 + Q-2: `source_profiler_fill_result` — false (output untouched) when
/// disabled, zeroed + false for unknown sources.
pub fn fill_result(source: *const c_void, out: &mut SourceProfilerResult) -> bool {
    let state = STATE.lock().unwrap();
    if !state.enabled {
        return false;
    }
    *out = SourceProfilerResult::zeroed();
    let key = source as usize;
    if let Some(ent) = state.entries.get(&key) {
        calculate_tick(ent, out);
        calculate_render(&state, ent, out);
        let async_source =
            (backend().output_flags)(key) & OBS_SOURCE_ASYNC_VIDEO == OBS_SOURCE_ASYNC_VIDEO;
        if async_source {
            calculate_fps(
                &ent.async_frame_ts,
                &mut out.async_input,
                &mut out.async_input_best,
                &mut out.async_input_worst,
            );
            calculate_fps(
                &ent.async_rendered_ts,
                &mut out.async_rendered,
                &mut out.async_rendered_best,
                &mut out.async_rendered_worst,
            );
        }
    }
    state.entries.contains_key(&key)
}

/// R20: `source_profiler_get_result` — `None` when `fill_result` is false.
pub fn get_result(source: *const c_void) -> Option<SourceProfilerResult> {
    let mut result = SourceProfilerResult::zeroed();
    fill_result(source, &mut result).then_some(result)
}

/// `profiler_result_t` (source-profiler.h): the C-ABI layout the
/// conformance runner fills and prints (field order pinned there).
#[repr(C)]
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SourceProfilerResult {
    /// Tick times in ns.
    pub tick_avg: u64,
    pub tick_max: u64,
    /// Average and max render times for CPU and GPU in ns.
    pub render_avg: u64,
    pub render_max: u64,
    pub render_gpu_avg: u64,
    pub render_gpu_max: u64,
    /// Average of the sum of all render passes in a frame in ns.
    pub render_sum: u64,
    pub render_gpu_sum: u64,
    /// FPS of submitted async input.
    pub async_input: f64,
    /// Actually rendered async frames.
    pub async_rendered: f64,
    /// Best and worst frame times of input/output in ns.
    pub async_input_best: u64,
    pub async_input_worst: u64,
    pub async_rendered_best: u64,
    pub async_rendered_worst: u64,
}

impl SourceProfilerResult {
    /// The C `memset(result, 0, sizeof)` (the Q-2 zero-fill).
    pub fn zeroed() -> Self {
        Self {
            tick_avg: 0,
            tick_max: 0,
            render_avg: 0,
            render_max: 0,
            render_gpu_avg: 0,
            render_gpu_max: 0,
            render_sum: 0,
            render_gpu_sum: 0,
            async_input: 0.0,
            async_rendered: 0.0,
            async_input_best: 0,
            async_input_worst: 0,
            async_rendered_best: 0,
            async_rendered_worst: 0,
        }
    }
}

impl Default for SourceProfilerResult {
    fn default() -> Self {
        Self::zeroed()
    }
}

/// Test seam: clear the global state and the backend (per test, W9 fence).
pub fn test_reset() {
    *STATE.lock().unwrap() = Inner {
        enabled: false,
        gpu_enabled: false,
        enable_next: AtomicBool::new(false),
        gpu_enable_next: AtomicBool::new(false),
        sample_capacity: 0,
        timer_idx: 0,
        timer_ranges: [core::ptr::null_mut(); FRAME_BUFFER_SIZE],
        samples: HashMap::new(),
        entries: HashMap::new(),
    };
    *BACKEND.lock().unwrap() = None;
}

// ---------------------------------------------------------------------------
// RAII guards for Rust users (P1-12.3)

/// RAII tick guard: starts a tick for `source` on creation, ends it on
/// drop. `None` while the profiler is disabled (C: `tick_start` returns 0
/// and `tick_end` is inert, R14).
#[must_use = "the guard ends the tick on drop"]
pub struct SourceTickGuard {
    source: *const c_void,
    start: u64,
}

impl SourceTickGuard {
    pub fn new(source: *const c_void) -> Option<Self> {
        let start = tick_start();
        (start != 0).then(|| SourceTickGuard { source, start })
    }
}

impl Drop for SourceTickGuard {
    fn drop(&mut self) {
        tick_end(self.source, self.start);
    }
}

/// RAII render-pass guard: begins a render pass for `source` on creation
/// (creating a GPU timer while GPU profiling is effective, R16) and ends it
/// on drop. `None` while the profiler is disabled (R14: the out timer is
/// untouched by `render_pass_begin`).
#[must_use = "the guard ends the render pass on drop"]
pub struct SourceRenderGuard {
    source: *const c_void,
    start: u64,
    timer: *mut c_void,
}

impl SourceRenderGuard {
    /// `timer_out` receives the pass timer (NULL without GPU profiling),
    /// mirroring the C `gs_timer_t **` out parameter.
    pub fn new(source: *const c_void, timer_out: &mut *mut c_void) -> Option<Self> {
        let mut timer: *mut c_void = core::ptr::null_mut();
        let start = render_pass_begin(&mut timer);
        (start != 0).then(|| {
            *timer_out = timer;
            SourceRenderGuard {
                source,
                start,
                timer,
            }
        })
    }
}

impl Drop for SourceRenderGuard {
    fn drop(&mut self) {
        render_pass_end(self.source, self.start, self.timer);
    }
}

// ---------------------------------------------------------------------------
// Tests (fake backend, W9 fence on TEST_LOCK)

#[cfg(test)]
mod tests {
    use super::*;

    static TEST_LOCK: Mutex<()> = Mutex::new(());

    /// The fake C services (one process-global fake; every test holds
    /// TEST_LOCK for its whole body). Timer / range tokens are 1-based
    /// indices into `timers` / `ranges`.
    #[derive(Default)]
    struct Fake {
        now: u64,
        enter: i32,
        leave: i32,
        timer_create: i32,
        timer_destroy: i32,
        timer_begin: i32,
        timer_end: i32,
        /// the ticks the next created timer carries (`Some` = data
        /// available).
        timer_ticks_next: Option<u64>,
        /// per created timer: the ticks `get_data` returns (`None` = no data).
        timers: Vec<Option<u64>>,
        range_create: i32,
        range_destroy: i32,
        range_begin: i32,
        range_end: i32,
        ranges: Vec<()>,
        /// the `range_get_data` result (`None` = the C `bool` return false:
        /// the out values keep their initial state — see the C-UB note in
        /// the module docs).
        range_data: Option<(bool, u64)>,
        /// (source pointer value, output_flags).
        flags: Vec<(usize, u32)>,
        async_ts: u64,
        /// (task function address, param) — the queued removals.
        tasks: Vec<(usize, usize)>,
        warnings: Vec<String>,
    }

    static FAKE: LazyLock<Mutex<Fake>> = LazyLock::new(|| Mutex::new(Fake::default()));

    fn with<R, F: FnOnce(&mut Fake) -> R>(f: F) -> R {
        f(&mut FAKE.lock().unwrap())
    }

    fn f_now() -> u64 {
        with(|f| f.now)
    }
    fn f_enter() {
        with(|f| f.enter += 1);
    }
    fn f_leave() {
        with(|f| f.leave += 1);
    }
    fn f_timer_create() -> *mut c_void {
        with(|f| {
            f.timer_create += 1;
            f.timers.push(f.timer_ticks_next.take().or(Some(0)));
            f.timers.len() as *mut c_void
        })
    }
    fn f_timer_destroy(t: *mut c_void) {
        with(|f| {
            f.timer_destroy += 1;
            let Some(i) = (t as usize).checked_sub(1) else {
                return;
            };
            if i < f.timers.len() {
                f.timers[i] = None;
            }
        });
    }
    fn f_timer_begin(t: *mut c_void) {
        with(|f| {
            f.timer_begin += 1;
            let _ = t;
        });
    }
    fn f_timer_end(t: *mut c_void) {
        with(|f| {
            f.timer_end += 1;
            let _ = t;
        });
    }
    fn f_timer_get_data(t: *mut c_void) -> Option<u64> {
        with(|f| {
            let i = (t as usize) - 1;
            f.timers.get(i).copied().flatten()
        })
    }
    fn f_range_create() -> *mut c_void {
        with(|f| {
            f.range_create += 1;
            f.ranges.push(());
            f.ranges.len() as *mut c_void
        })
    }
    fn f_range_destroy(r: *mut c_void) {
        with(|f| {
            f.range_destroy += 1;
            let Some(i) = (r as usize).checked_sub(1) else {
                return;
            };
            if i < f.ranges.len() {
                f.ranges[i] = ();
            }
        });
    }
    fn f_range_begin(r: *mut c_void) {
        with(|f| {
            f.range_begin += 1;
            let _ = r;
        });
    }
    fn f_range_end(r: *mut c_void) {
        with(|f| {
            f.range_end += 1;
            let _ = r;
        });
    }
    fn f_range_get_data(_r: *mut c_void) -> Option<(bool, u64)> {
        with(|f| f.range_data)
    }
    fn f_output_flags(source: usize) -> u32 {
        with(|f| {
            f.flags
                .iter()
                .find(|(k, _)| *k == source)
                .map(|(_, v)| *v)
                .unwrap_or(0)
        })
    }
    fn f_last_async_ts(_source: usize) -> u64 {
        with(|f| f.async_ts)
    }
    fn f_queue_task(task: *const c_void, param: *mut c_void) {
        with(|f| f.tasks.push((task as usize, param as usize)));
    }
    fn f_log_warning(msg: &str) {
        with(|f| f.warnings.push(msg.to_string()));
    }

    /// Fresh fake + backend + state (per test; the test holds TEST_LOCK).
    fn install() {
        *FAKE.lock().unwrap() = Fake {
            range_data: Some((false, 1_000_000_000)),
            ..Default::default()
        };
        // reset first: `test_reset` also clears the backend slot.
        test_reset();
        set_backend(SourceProfilerBackend {
            now_ns: f_now,
            enter_context: f_enter,
            leave_context: f_leave,
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
            log_warning: f_log_warning,
        });
    }

    const S0: *const c_void = 0x1000 as *const c_void;
    const S1: *const c_void = 0x2000 as *const c_void;
    const ASYNC: u32 = OBS_SOURCE_ASYNC_VIDEO;

    /// One scripted frame on S0: `frame_begin`, a tick advancing the clock
    /// by `tick_ns`, an optional render pass of `render_ns`, `frame_collect`.
    fn frame(tick_ns: u64, render_ns: Option<u64>) {
        frame_begin();
        let start = tick_start();
        with(|f| f.now += tick_ns);
        tick_end(S0, start);
        if let Some(render_ns) = render_ns {
            let mut timer: *mut c_void = core::ptr::null_mut();
            let rstart = render_pass_begin(&mut timer);
            with(|f| f.now += render_ns);
            render_pass_end(S0, rstart, timer);
        }
        frame_collect();
    }

    fn fill(src: *const c_void) -> (bool, SourceProfilerResult) {
        let mut r = SourceProfilerResult::zeroed();
        let ok = fill_result(src, &mut r);
        (ok, r)
    }

    /// R14: while disabled every entry point is inert.
    #[test]
    fn disabled_inert() {
        let _lock = TEST_LOCK.lock().unwrap();
        install();
        assert_eq!(tick_start(), 0);
        let mut r = SourceProfilerResult::zeroed();
        r.tick_avg = u64::MAX; // the runner's 0xff seed
        assert!(!fill_result(S0, &mut r));
        assert_eq!(r.tick_avg, u64::MAX, "the output stays untouched (R14)");
        assert!(get_result(S0).is_none());
        async_frame_received(S0);
        remove_source(S0);
        with(|f| assert!(f.tasks.is_empty(), "R14: nothing queued while disabled"));
        let mut sentinel = std::ptr::dangling_mut::<c_void>();
        assert_eq!(render_pass_begin(&mut sentinel), 0);
        assert_eq!(
            sentinel as usize, 0x1,
            "R14: *timer untouched while disabled"
        );
        render_pass_end(S0, 0, core::ptr::null_mut());
        frame_collect();
        // the guards report "inert" (None) while disabled.
        assert!(SourceTickGuard::new(S0).is_none());
        let mut t: *mut c_void = 0x1 as _;
        assert!(SourceRenderGuard::new(S0, &mut t).is_none());
        assert_eq!(t as usize, 0x1, "R14: the out timer is untouched");
    }

    /// srcprof_basic: the first collect lags one frame (Q-7); the second
    /// reports frame 1.
    #[test]
    fn basic_lag_and_first_result() {
        let _lock = TEST_LOCK.lock().unwrap();
        install();
        enable(true);
        frame_begin();
        reset_video(60, 1);
        frame(100, Some(40));
        assert!(!fill(S0).0, "Q-7: no entry after one frame (the lag)");
        frame(300, Some(60));
        let (ok, r) = fill(S0);
        assert!(ok);
        assert_eq!(
            (
                r.tick_avg,
                r.tick_max,
                r.render_avg,
                r.render_max,
                r.render_sum
            ),
            (100, 100, 40, 40, 40)
        );
        assert_eq!(
            (r.render_gpu_avg, r.render_gpu_max, r.render_gpu_sum),
            (0, 0, 0)
        );
    }

    /// srcprof_ring: fps 1 -> capacity 5; the averages cover only the last 5
    /// collected frames; `reset_video` discards the history.
    #[test]
    fn ring_wrap_and_reset() {
        let _lock = TEST_LOCK.lock().unwrap();
        install();
        enable(true);
        frame_begin();
        reset_video(1, 1);
        for tick in [10u64, 20, 30, 40, 50, 60] {
            frame(tick, None);
        }
        let (_, r) = fill(S0);
        assert_eq!((r.tick_avg, r.tick_max), (30, 50), "five samples: 10..50");
        frame(70, None);
        frame(1000, None);
        let (_, r) = fill(S0);
        assert_eq!(
            (r.tick_avg, r.tick_max),
            (50, 70),
            "wrapped: 60,70,30,40,50"
        );
        reset_video(30000, 1001);
        let mut r = SourceProfilerResult::zeroed();
        r.tick_avg = u64::MAX;
        assert!(!fill_result(S0, &mut r));
        assert_eq!(r.tick_avg, 0, "Q-2: zeroed — the reset discarded the entry");
    }

    /// srcprof_gpu: the enable ordering (R11/R12), the range and pass
    /// timers, the tick->ns conversion, the disjoint warning, the no-data
    /// destruction, the disable cleanup, and the enter/leave balance.
    ///
    /// Each GPU frame mirrors the script's interleaving: `frame_begin`,
    /// `render_begin`, tick, pass, `render_end`, `frame_collect` (the
    /// collect processes the *previous* frame's slot, Q-7).
    #[test]
    fn gpu_timers() {
        let _lock = TEST_LOCK.lock().unwrap();
        install();
        // GPU requested before the profiler is enabled: ignored (R11 AND).
        gpu_enable(true);
        enable(true);
        frame_begin();
        reset_video(60, 1);
        render_begin(); // no-op while GPU is off (R17)
        frame(7, Some(5));
        with(|f| assert_eq!((f.timer_create, f.range_create), (0, 0), "GPU still off"));

        // the proper request: GPU becomes effective at the next frame_begin
        // (R12); the render range is created at the current index.
        gpu_enable(true);
        frame_begin(); // first GPU frame: the index does not advance
        render_begin();
        render_end();
        with(|f| assert_eq!((f.range_create, f.range_begin, f.range_end), (1, 1, 1)));

        // two pass frames: the second collect samples the first pass's
        // timer (ticks == ns at the 1e9 range frequency).
        for ticks in [2000u64, 3000] {
            frame_begin();
            render_begin();
            let start = tick_start();
            with(|f| f.now += 7);
            tick_end(S0, start);
            with(|f| f.timer_ticks_next = Some(ticks));
            let mut timer: *mut c_void = core::ptr::null_mut();
            let rstart = render_pass_begin(&mut timer);
            with(|f| f.now += 5);
            render_pass_end(S0, rstart, timer);
            render_end();
            frame_collect();
        }
        let (_, r) = fill(S0);
        // the gpu buffer holds the 0/0 push of the first (gpu-off) frame
        // and the 2000 ns of the first timer frame (multi-pass sums are
        // pinned byte-exact by the conformance srcprof_gpu vector).
        assert_eq!(
            (r.render_gpu_avg, r.render_gpu_max, r.render_gpu_sum),
            (1000, 2000, 1000)
        );

        // disjoint range: the warning; the previous frame's timer is
        // destroyed without a sample (R18).
        with(|f| {
            f.range_data = Some((true, 1_000_000_000));
            f.timer_ticks_next = None;
        });
        gpu_frame();
        with(|f| assert_eq!(f.warnings, vec![DISJOINT_WARNING.to_string()]));
        let (_, r) = fill(S0);
        assert_eq!(
            (r.render_gpu_avg, r.render_gpu_max),
            (1000, 2000),
            "the disjoint frame pushed nothing"
        );

        // a frame whose previous timer has no data: destroyed without a
        // sample (the range is ready, the timer's get_data fails).
        with(|f| {
            f.range_data = Some((false, 1_000_000_000));
            f.timer_ticks_next = None;
        });
        gpu_frame();
        let (_, r) = fill(S0);
        assert_eq!(
            (r.render_gpu_avg, r.render_gpu_max, r.render_gpu_sum),
            (1000, 2000, 1000)
        );

        // disabling GPU destroys the ranges at the end of the next collect
        // (R18); the GPU part of the result is then skipped (R20).
        gpu_enable(false);
        frame_begin();
        frame_collect();
        with(|f| assert_eq!(f.range_destroy, 2, "both timer ranges destroyed"));
        let (_, r) = fill(S0);
        assert_eq!(
            (r.render_gpu_avg, r.render_gpu_max),
            (0, 0),
            "R20: the GPU part is only reported while GPU is effective"
        );

        // disabling the profiler frees everything; results are gone.
        enable(false);
        frame_begin();
        frame_collect();
        assert!(!fill(S0).0);
        with(|f| {
            assert_eq!(f.enter, f.leave, "the context enter/leave calls balance");
            assert!(
                f.timers.iter().all(|t| t.is_none()),
                "every pass timer was destroyed"
            );
        });
    }

    /// One GPU frame with a pass (the ticks come from the fake's
    /// `timer_ticks_next`), the script's interleaving.
    fn gpu_frame() {
        frame_begin();
        render_begin();
        let start = tick_start();
        with(|f| f.now += 7);
        tick_end(S0, start);
        let mut timer: *mut c_void = core::ptr::null_mut();
        let rstart = render_pass_begin(&mut timer);
        with(|f| f.now += 5);
        render_pass_end(S0, rstart, timer);
        render_end();
        frame_collect();
    }

    /// srcprof_async (R19 + R21): arrivals before the first entry are lost;
    /// the fps rule over the timestamp rings (the rendered ring's
    /// wrap-around first delta is the 1016000000 quirk).
    #[test]
    fn async_fps() {
        let _lock = TEST_LOCK.lock().unwrap();
        install();
        with(|f| f.flags.push((S0 as usize, ASYNC)));
        enable(true);
        frame_begin();
        reset_video(60, 1);
        // an arrival with no entry yet: lost (R19).
        with(|f| f.now = 1_000_000_000);
        async_frame_received(S0);
        with(|f| f.async_ts = 1_000_000_000);
        frame(10, None); // builds the lag; the first collect pushes nothing
        frame(10, None); // the entry exists now; rendered push 1e9
        // four arrivals 16 / 17 / 33 ms apart (the entry exists).
        for ts in [
            2_000_000_000u64,
            2_016_000_000,
            2_033_000_000,
            2_066_000_000,
        ] {
            with(|f| f.now = ts);
            async_frame_received(S0);
        }
        // three more frames with the collect-time async ts (rendered pushes).
        for ts in [2_016_000_000u64, 2_033_000_000, 2_066_000_000] {
            with(|f| f.async_ts = ts);
            frame(10, None);
        }
        let (_, r) = fill(S0);
        assert_eq!(
            (r.async_input_best, r.async_input_worst),
            (16_000_000, 33_000_000)
        );
        assert!((r.async_input - 45.4545455).abs() < 1e-6, "1e9 / (66e6/3)");
        assert_eq!(r.async_rendered_best, 17_000_000);
        assert_eq!(
            r.async_rendered_worst, 1_016_000_000,
            "R21 wrap-around first delta"
        );
        assert!((r.async_rendered - 2.81425891).abs() < 1e-6);
        // a non-async source with no history reports nothing.
        let (_, r) = fill(S1);
        assert_eq!((r.async_input, r.async_rendered), (0.0, 0.0));
    }

    /// R21 wrap-around quirk, directly on the fps rule: a wrapped ring (not
    /// in chronological order) produces a very large first delta; a zero
    /// timestamp stops the walk.
    #[test]
    fn calculate_fps_wrap() {
        let _lock = TEST_LOCK.lock().unwrap();
        let mut buf = CircleBuf::init(3);
        for v in [1000u64, 2000, 1_016_000] {
            buf.push(v);
        }
        let mut avg = 0.0;
        let mut best = u64::MAX;
        let mut worst = 0u64;
        calculate_fps(&buf, &mut avg, &mut best, &mut worst);
        // i=0: prev = ts[2] = 1_016_000 >= 1000 -> skip. i=1: 1000. i=2: 1_014_000.
        assert_eq!((best, worst), (1000, 1_014_000));
        assert!((avg - 1.0E9 / (1_015_000.0 / 2.0)).abs() < 1e-6);
        let mut buf = CircleBuf::init(4);
        for v in [5u64, 0, 10, 20] {
            buf.push(v);
        }
        let mut best = u64::MAX;
        calculate_fps(&buf, &mut avg, &mut best, &mut worst);
        assert_eq!(best, u64::MAX, "no valid delta: best untouched");
    }

    /// R22 + srcprof_remove: the removal runs on the (fake) graphics queue —
    /// the data stays visible until the task runs; an unknown source is
    /// harmless.
    #[test]
    fn remove_deferred() {
        let _lock = TEST_LOCK.lock().unwrap();
        install();
        enable(true);
        frame_begin();
        reset_video(60, 1);
        frame(10, None);
        frame(10, None);
        assert!(fill(S0).0);
        remove_source(S0);
        with(|f| assert_eq!(f.tasks.len(), 1));
        assert!(fill(S0).0, "the data stays visible until the task runs");
        // run the queued task (the runner's sp_run_tasks).
        with(|f| {
            for (_task, param) in f.tasks.drain(..) {
                // SAFETY: task_delete is the queued fn; param is the queued
                // source pointer value (no dereference).
                unsafe { (task_delete as unsafe extern "C" fn(*mut c_void))(param as *mut c_void) };
            }
        });
        let mut r = SourceProfilerResult::zeroed();
        r.tick_avg = u64::MAX;
        assert!(!fill_result(S0, &mut r));
        assert_eq!(
            r.tick_avg, 0,
            "Q-2: zeroed — the entry was removed by the task"
        );
        // removing an unknown source is harmless.
        remove_source(S1);
        with(|f| {
            let (_task, param) = *f.tasks.last().unwrap();
            // SAFETY: as above.
            unsafe { (task_delete as unsafe extern "C" fn(*mut c_void))(param as *mut c_void) };
        });
    }

    /// Q-2: enabled profiler, unknown source -> false with a zeroed result;
    /// a source with samples but no entry yet (the lag) is unknown too.
    #[test]
    fn q2_unknown_source_zeroed() {
        let _lock = TEST_LOCK.lock().unwrap();
        install();
        enable(true);
        frame_begin();
        reset_video(60, 1);
        let mut r = SourceProfilerResult::zeroed();
        r.tick_avg = u64::MAX;
        r.render_sum = u64::MAX;
        assert!(!fill_result(S1, &mut r));
        assert_eq!((r.tick_avg, r.render_sum), (0, 0), "zeroed");
        assert!(get_result(S1).is_none());
        frame(10, None);
        assert!(!fill(S0).0, "the lag: samples without an entry are unknown");
    }

    /// P1-12.3 RAII: the tick and render guards.
    #[test]
    fn guards() {
        let _lock = TEST_LOCK.lock().unwrap();
        install();
        assert!(SourceTickGuard::new(S0).is_none());
        let mut t: *mut c_void = 0x1 as _;
        assert!(SourceRenderGuard::new(S0, &mut t).is_none());
        assert_eq!(t as usize, 0x1);

        enable(true);
        frame_begin();
        reset_video(60, 1);
        with(|f| f.now = 500);
        {
            let _guard = SourceTickGuard::new(S0).expect("enabled: the guard starts a tick");
            with(|f| f.now = 900);
        } // tick_end: delta 400 -> the current slot
        frame_begin();
        {
            let _guard = SourceTickGuard::new(S0).unwrap();
            with(|f| f.now += 100);
        } // delta 100 -> the other slot
        frame_collect(); // processes slot 0 (the 400 ns tick)
        let (_, r) = fill(S0);
        assert_eq!((r.tick_avg, r.tick_max), (400, 400));

        // the render guard with GPU: creates the pass timer, ends it on
        // drop, and keeps it in the frame slot.
        gpu_enable(true);
        frame_begin();
        let mut t: *mut c_void = core::ptr::null_mut();
        let (tc, tb) = with(|f| (f.timer_create, f.timer_begin));
        {
            let _guard = SourceRenderGuard::new(S0, &mut t).unwrap();
            assert!(!t.is_null(), "GPU effective: the pass carries a timer");
        }
        with(|f| {
            assert_eq!(f.timer_create - tc, 1, "one timer created by the guard");
            assert_eq!(f.timer_begin - tb, 1);
            assert_eq!(f.timer_end, 1, "timer_end on drop");
            assert_eq!(
                f.timer_destroy, 0,
                "the known source keeps its timer in the frame slot"
            );
        });
        // an unknown source's pass timer is destroyed by the end (R16).
        let mut t: *mut c_void = core::ptr::null_mut();
        {
            let _guard = SourceRenderGuard::new(S1, &mut t).unwrap();
        }
        with(|f| {
            assert_eq!(
                f.timer_destroy, 1,
                "the unknown source's timer is destroyed"
            )
        });
    }
}
