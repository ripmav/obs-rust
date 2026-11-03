# Phase 2 guide: media pipeline

Read [00-developer-primer.md](00-developer-primer.md) first. Tasks: [../phase-2-media-pipeline.md](../phase-2-media-pipeline.md); subtasks: [../subtasks/phase-2-subtasks.md](../subtasks/phase-2-subtasks.md).

## Dependency corrections (read before planning)

The task list groups some code by *file name*, but part of it is entangled with objects that are only ported in Phase 4. Split these tasks into a **pure part** (Phase 2) and an **integration part** (Phase 4):

| Task | Pure part (Phase 2) | Integration part (Phase 4) |
|---|---|---|
| P2-13 audio mixer | `mix_audio` sample loop, `ignore_audio` maths, buffering tick arithmetic (`add_audio_buffering`, `set_fixed_audio_buffering`); `clamp_audio_output` belongs to P2-08 (`audio-io.c`) | `audio_callback` in `obs-audio.c:548` walks `obs->video.mixes`, `view->channels`, `obs_source_enum_active_tree`, source mutexes: needs the object model |
| P2-16 volume/pan/mono kernels | `apply_audio_volume`/`multiply_output_audio`, `process_audio_balancing`, `downmix_to_mono_planar` as pure functions over fake buffers | the call sites in `obs_source_output_audio` (`obs-source.c`, P4-05) allocate and pass the real per-source buffers |
| P2-10 timing | `Clock` trait, `video_sleep` logic, interval/drift arithmetic | `obs_graphics_thread_loop` (`obs-video.c:1094`) calls `tick_sources`, `output_frames`, `render_displays` |
| P2-22/23/24 packet queues | queue/interleave/delay algorithms as pure data structures with traces | `obs-encoder.c` / `obs-output.c` / `obs-output-delay.c` are methods of `obs_encoder_t` / `obs_output_t` |

## How the C pipeline actually works (verified)

### Video
1. The **graphics thread** (Phase 3/4) renders a frame, then `output_frames()` → `video_output_lock_frame()` / `unlock_frame()` hand the converted frame to `video-io`.
2. `video_output_unlock_frame` posts `video->update_semaphore`. The **video thread** (`video_thread`, `video-io.c:190`) blocks on that semaphore, then loops `video_output_cur_frame()` until all cached frames are delivered.
3. `video_output_cur_frame` calls each registered `video_input`'s callback (`void (*)(void *param, struct video_data *frame)`) — these are the encoders or raw outputs. Per input there may be a **scaler** (`video_scaler_t`) if the requested format/size differs, with up to `MAX_CONVERT_BUFFERS` converted frames, and a **`frame_rate_divisor`** with its own counter (so inputs started at the same time align; do not replace the counter with `% divisor`, the comment in the source explains why).
4. The cache is a ring of `MAX_CACHE_SIZE` (16) `cached_frame_info {frame, skipped, count}`. If the graphics thread produces faster than the video thread consumes, frames are dropped and `skipped_frames` is incremented; `total_frames` counts delivered frames. Both are atomics (`os_atomic_inc_long`) read by the stats UI.

Rust sketch (**Proposal**):
```rust
pub struct VideoOutput { /* info, ring: [Slot; 16], inputs: Mutex<Vec<VideoInput>>, sem: Semaphore, … */ }
impl VideoOutput {
    pub fn lock_frame(&self, count: i32, ts: u64) -> Option<FrameSlot<'_>>; // C: video_output_lock_frame
    pub fn unlock_frame(&self);                                              // posts the semaphore
}
```
Keep `inputs` behind a mutex that the video thread holds only while iterating (C: `input_mutex`); connect/disconnect happen from UI threads.

### Audio
1. `audio_thread` (`audio-io.c:205`) computes `audio_time = start_time + audio_frames_to_ns(rate, samples)` where `samples += AUDIO_OUTPUT_FRAMES` (1024) each loop and sleeps with `os_sleepto_ns_fast(audio_time)`. Time is derived from a sample counter, not from "now + interval", so it does not drift.
2. `input_and_output()` (`audio-io.c:160`): under `input_mutex` find which of the 6 mixes have inputs (`active_mixes` bitmask); zero the mix buffers; call `audio->input_cb(param, prev_time, audio_time, &new_ts, active_mixes, data)` — this is `audio_callback` from libobs — then **clamp to [-1, 1]** and call `do_audio_output` for each active mix, which feeds each connected `audio_input`, resampling (`audio_resampler_t`) to the consumer's format first.
3. `audio_callback` (`obs-audio.c:548`): builds the render order (sources reachable from each video mix's channels plus global audio sources), calls `obs_source_audio_render` for each, keeps a `buffered_timestamps` deque to decide extra buffering when a source's audio timestamp lags (`calc_min_ts`, `add_audio_buffering`, `set_fixed_audio_buffering`), mixes root nodes into the output with `mix_audio` (summation only; volume/mute/mixer mask were already applied source-side, P2-16), then discards consumed audio.
4. Constants: `MAX_AUDIO_MIXES` 6, `MAX_AUDIO_CHANNELS` 8, `AUDIO_OUTPUT_FRAMES` 1024 (21.33 ms at 48 kHz). `audio_output_info` carries `samples_per_sec`, `format` (float planar in practice), `speakers`, and the callback.

Real-time rules: the audio thread holds `input_mutex` only briefly; everything else must be allocation- and lock-free. Windows sets MMCSS class "Audio" — keep it.

## Format and conversion work

- `enum video_format` has 26 values (verified list in `video-io.h`): `NONE, I420, NV12, YVYU, YUY2, UYVY, RGBA, BGRA, BGRX, Y800, I444, BGR3, I422, I40A, I42A, YUVA, AYUV, I010, P010, I210, I412, YA2L, P216, P416, V210, R10L`. Anything that switches on this enum needs all of them — start every converter task by tabulating which formats it supports and what the C code does for the rest (usually returns "unsupported").
- `video-matrices.c` holds the colour matrices (BT.601/709/2020 × full/limited); `format-conversion.c` has SIMD packed<->planar YUV converters (UYVX <-> I420/NV12/I444/422), not YUV<->RGB. In 33.0.1 it is exported but has no callers (only stale includes in `obs-video.c`, `obs-source.c`, `video-io.c`) — port it anyway: S-013 specifies all six functions and the plugin ABI keeps them.
- `video-scaler-ffmpeg.c` and `audio-resampler-ffmpeg.c` are thin wrappers over `libswscale`/`libswresample`. Do **not** rewrite swscale/swresample; wrap them behind a trait so that a native implementation can be dropped in later. The risk is bit-exactness vs the C wrapper: replicate its flags and colour-space set-up exactly.
- `media-remux.c` (remux dialog backend) was added as P2-29.

## Bitstream parsers (P2-17…P2-21)

Public API in `libobs/obs-nal.h`, `obs-avc.h`, `obs-hevc.h`, `obs-av1.h` (all exported; used by RTMP/FLV/MP4 muxers and by the hardware-encoder plugins). Examples: `obs_nal_find_startcode`, `obs_avc_keyframe`, `obs_parse_avc_packet`, `obs_parse_avc_packet_priority`, `obs_parse_avc_header`, `obs_extract_avc_headers`, `obs_extract_av1_headers`. These work on caller-provided buffers and return heap buffers allocated with `bmalloc` — return allocations through the libobs allocator. They process untrusted-ish data (streams from hardware encoders and, via plugins, arbitrary sources): this is where Rust gives a safety dividend — write them first with fuzzing.

## Suggested order inside the phase

1. Enums/matrices/format helpers (P2-01, 02, 05, 07) – no threads.
2. Converters and scaler/resampler wrappers (P2-03, 04, 06).
3. Bitstream parsers with fuzzers (P2-17…21, caption FFI per D-18) – independent and high value.
4. Threads: audio-io, video-io with the fake clock (P2-08…12).
5. Pure parts of mixer/packet queues (P2-13…16, 22…24).
6. Remux (P2-29), integration (P2-25…28). Integration of the Phase-4-coupled parts waits for Phase 4.

## Pitfalls

- The video thread and graphics thread communicate only via the cache + semaphore. If you replace the semaphore with a channel, keep "drop oldest" semantics, not backpressure — blocking the graphics thread would drop real frames on screen.
- `skipped_frames` vs `total_frames` semantics are user-visible ("Missed frames", "Skipped frames due to encoding lag"); copy the increment points exactly.
- Floating-point: clamping `-1..1` happens once per mix after summation; summing in a different order changes low bits. Goldens should allow ≤ 1 ULP only where the order is demonstrably different.
- macOS and Windows differ in `audio-monitoring` backends; there is no ALSA monitoring backend (only `null`, `osx`, `pulse`, `win32`).

## Running this phase's checks

Conformance: from `test/conformance/` with `CONFORMANCE_BUILD_DIR` exported, `./run.sh check <area>` for
`mediaconv` (S-013, 27 scripts), `avio` (S-044, 26), `nal` (S-012, 19), `audiomix` (S-032, 39), `encoutput`
(S-041, 33) and `utilrest` (S-055, 26). A Rust runner implements each area's script language and diffs stdout
against `vectors/<area>/` (comparison rules: `test/conformance/README.md`). CMake side: build with
`-DOBS_ENABLE_RUST=ON/OFF` and run `ctest` (P0-08). Rust side: `cargo test`, `cargo clippy -- -D warnings`,
`cargo bench` against the P0-17 baselines in `docs/rust-rewrite/benchmarks.md`. `cargo xtask` arrives in Phase 7 (P7-06).

## Verification checklist

- [ ] Goldens for converters (all 26 formats where supported) and colour matrices.
- [ ] Fuzz corpora checked in and run in CI for NAL/AVC/HEVC/AV1.
- [ ] Fake-clock tests reproduce C frame/audio timing traces for 10 minutes of simulated time.
- [ ] 24 h soak (P2-26) shows no RSS growth and no more dropped frames than the C build.
