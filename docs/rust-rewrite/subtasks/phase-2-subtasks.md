# Phase 2 subtasks

Parent file: [../phase-2-media-pipeline.md](../phase-2-media-pipeline.md). Each subtask has **Files**, **Steps**, **Done when**.

Real-time (RT) rule for every subtask that touches audio or video threads: no allocation, no blocking lock, no syscall on the hot path; enforce with the harness from P2-12.

Verified file map (libobs 33.0.1 checkout, line counts): `libobs/media-io/` has 19 files (3,642 lines): `audio-io.c/.h` (455/228), `audio-math.h` (43), `audio-resampler-ffmpeg.c` (218), `audio-resampler.h` (44), `format-conversion.c/.h` (288/50), `frame-rate.h` (29), `media-io-defs.h` (20), `media-remux.c/.h` (266/37), `video-fourcc.c` (51), `video-frame.c/.h` (263/64), `video-io.c/.h` (679/338), `video-matrices.c` (256), `video-scaler-ffmpeg.c` (270), `video-scaler.h` (43). `libobs/audio-monitoring/` has 14 files (2,433 lines): `null/` (35), `osx/` (567), `pulse/` (1,166), `win32/` (665) — no ALSA backend. GPU encoding lives in `libobs/obs-video-gpu-encode.c` (312). `media-remux.c` was missing from the earlier plan and is added as P2-29.

**Dependency note (see `../guides/phase-2-guide.md`):** P2-10.2 (`obs_graphics_thread_loop`), P2-13 (`audio_callback`), P2-16 (the volume/pan/mono kernels live in `obs-source.c`) and P2-22…P2-24 (`obs-encoder.c` / `obs-output.c`) are entangled with Phase 4 objects. Do the pure algorithm in Phase 2 with fake inputs; the integration lands in Phase 4.

## A. Formats and conversion

### P2-01 Format enums and helpers
- [ ] **P2-01.1 Enums**
  - Files: `libobs/media-io/video-io.h` (338), `libobs/media-io/audio-io.h` (228).
  - Steps: extract `video_format` (26 values, `NONE`=0…`R10L`=25), `audio_format`, `speaker_layout`, `video_colorspace`, `video_range_type` verbatim; write `#[repr(C)]`/`#[repr(i32)]` enums plus `TryFrom` in `crates/obs-media` module `enums` (S-013 target crate); acceptance is the `mediaconv` `enums` command (script `01-enums-layouts`: every value, `sizeof`, `offsetof`).
  - Spec: S-013 §3 (R01–R05), §4.
  - Vectors: test/conformance/scripts/mediaconv/ (27 scripts).
  - Done when: numeric values asserted equal to the C headers via a generated test (mediaconv script `01` green).
- [ ] **P2-01.2 Helpers**
  - Files: `libobs/media-io/audio-io.h` (228; static-inline helpers), `libobs/media-io/media-io-defs.h` (20).
  - Steps: port `get_audio_channels`, `get_audio_bytes_per_channel`, `get_audio_planes`, `is_audio_planar`, `get_audio_size`, `get_total_audio_size`, `audio_frames_to_ns`/`ns_to_audio_frames`, `util_mul_div64` to `crates/obs-media`; table-test every layout × format combination.
  - Spec: S-013 R07–R09.
  - Vectors: mediaconv `ainfo`, `frames_to_ns`, `ns_to_frames`.
  - Pitfalls: D-3 — `audio_frames_to_ns(0, n)` divides by zero in C; return an error and print `result crash` (vector 41).
  - Done when: table test.
- [ ] **P2-01.3 FourCC**
  - Files: `libobs/media-io/video-fourcc.c` (51).
  - Steps: port the `video_format_from_fourcc` switch (little-endian packed, first char = lowest byte; every `MAKE_FOURCC` alias; C has no format→fourcc direction); unknown → `NONE`.
  - Spec: S-013 R06.
  - Vectors: mediaconv `fourcc` (script 03).
  - Pitfalls: Q-7 — `VYUY` maps to YUY2 plus several Apple/ffmpeg aliases; the match is case-sensitive.
  - Done when: every `MAKE_FOURCC` alias in the C switch maps to the same format; unknown -> `NONE`.

### P2-02 Colour matrices
- [ ] **P2-02.1 Tables**
  - Files: `libobs/media-io/video-matrices.c` (256).
  - Steps: port the 16-float matrices for 601/709/2100 × bit depths 8..16 (`bpp_info`[9]) × full/limited = 54 tables into `crates/obs-media` module `matrices`; build them with the R53 `f32` operation order.
  - Spec: S-013 R53, D-4.
  - Vectors: mediaconv `vparams` (scripts 30, 33).
  - Pitfalls: R53 — `f64` evaluation then rounding changes bits; keep `f32`. D-4 — the C tables are lazily, unsynchronised-initialised; compute once with a thread-safe primitive.
  - Done when: all 54 tables equal C bit for bit (keep the C f32 operation order, S-013-R53).
- [ ] **P2-02.2 Parameters API**
  - Files: `libobs/media-io/video-matrices.c` (256).
  - Steps: port `video_format_get_parameters` / `_for_format`: colour-space remap (R50), range selection (R51), per-format bit depth (R52), `min_range`/`max_range` outputs (R54), NULL `min_range`/`max_range` (R55).
  - Spec: S-013 R50–R55.
  - Vectors: mediaconv `vparams`.
  - Pitfalls: R50 — `DEFAULT`/`SRGB` remap to 709 and `2100_HLG` remaps to the 2100_PQ matrix; a `cs` outside the six enumerators returns `false` and writes nothing. Q-5b — `RANGE_DEFAULT` selects the partial matrix for every format, also RGB.
  - Done when: golden covers every `(colorspace, range, format)` combination.
- [ ] **P2-02.3 Golden**
  - Files: `test/golden/` (P0-14 `obs-ref-dump` output).
  - Steps: dump the 54 tables plus the parameters API through the C `obs-ref-dump` (P0-14), commit as `*.golden`, diff the Rust output.
  - Vectors: mediaconv `vparams`.
  - Done when: `obs-ref-dump` output matches (P0-14 goldens).

### P2-03 `video-scaler` and `video-frame`
- [ ] **P2-03.1 Trait**
  - Files: `libobs/media-io/video-scaler.h` (43) — the contract: `video_scale_info {format, width, height, range, colorspace}`, `video_scaler_t`, status codes (`SUCCESS`, `BAD_CONVERSION`, `FAILED`).
  - Steps: define a `Scaler` trait in `crates/media-io` (S-044 target crate) with `scale(&mut self, dst, src) -> ScalerStatus`; document the error type and the creation-argument shape (dst info, src info, flags).
  - Spec: S-044 §5.4 (R34/R35 — when a scaler is needed and how it is created).
  - Done when: documented.
- [ ] **P2-03.2 swscale wrapper**
  - Files: `libobs/media-io/video-scaler-ffmpeg.c` (270).
  - Steps: wrap `sws_getContext`/`sws_scale` behind FFI in `crates/obs-media`; create the context from `video_scale_info`; copy the C's format/colour-space/range mapping and flag set (default `SWS_BICUBIC`) exactly.
  - Spec: S-044 R34/R35 (creation args and error paths).
  - Vectors: avio (S-044) drives the `Scaler` trait with logging fakes; the swscale output itself is checked by P2-03.4 hashes.
  - Pitfalls: output bytes depend on the exact flags/colour setup; replicate the C `get_flags` mapping — do not "improve" it.
  - Done when: output hash for 8 conversions equals C.
- [ ] **P2-03.3 Frame helpers**
  - Files: `libobs/media-io/video-frame.c` (263), `libobs/media-io/video-frame.h` (64).
  - Steps: port `video_frame_init` (32-byte-aligned block from linesizes × plane heights), `video_frame_free`, `video_frame_copy` (R26), `video_frame_get_linesizes` (R20), `video_frame_get_plane_heights` (R22) to `crates/obs-media` module `frame`.
  - Spec: S-013 R20–R26.
  - Vectors: mediaconv `linesizes`, `heights`, `frame_init`, `frame_copy`, `frame_free`, `frame_fill`, `frame_dump`.
  - Pitfalls: R21 (Q-2) — V210 `a = (w+47)&~47` evaluated on `u32` (wraps for huge widths); linesize can be smaller than a real V210 row. D-1 — `size == 0` must not abort (C crashes); return an error, frame left zeroed.
  - Done when: size table equal for all formats.
- [ ] **P2-03.4 Hash tests**
  - Files: `test/golden/` (P0-14).
  - Steps: run 8 format conversions through the wrapper plus `video_frame_copy` of every planar and 10-bit format; hash-compare with the C build.
  - Vectors: mediaconv `frame_*` commands.
  - Done when: hashes match the C `obs-ref-dump` goldens.

### P2-04 SIMD pixel converters
- [ ] **P2-04.1 Packed->planar**
  - Files: `libobs/media-io/format-conversion.c` (288), `libobs/media-io/format-conversion.h` (50).
  - Steps: port `compress_uyvx_to_i420`, `compress_uyvx_to_nv12`, `convert_uyvx_to_i444` per R30–R36 (processed width, luma copy, 2×2 chroma average, row/chroma-row writes) to `crates/obs-media` module `convert`.
  - Spec: S-013 §5.4 R30–R36.
  - Vectors: mediaconv `compress` (scripts 11, 13, 14).
  - Pitfalls: Q-3 — `convert_uyvx_to_i444` ignores `out_linesize[1]`/`[2]` (uses `[0]`). R32 — chroma sums are truncating `>> 2`, never rounded.
  - Done when: byte-exact vs C.
- [ ] **P2-04.2 Planar->packed**
  - Files: `libobs/media-io/format-conversion.c` (288), `format-conversion.h` (50).
  - Steps: port `decompress_420` (R37), `decompress_nv12` (R38), `decompress_422` lead/trail (R39); write native-endian `u32` words, no alignment requirement.
  - Spec: S-013 §5.5 R37–R40.
  - Vectors: mediaconv `decompress`, `decompress422` (scripts 20, 21, 23, 24, 25).
  - Pitfalls: Q-1 — output byte order is `V U Y 0` (420) vs `Y U V 0` (nv12); reproduce exactly. Q-4 — `decompress_422` word count is `min(ils, ols)/2` and the same `in_linesize` is the byte stride.
  - Done when: byte-exact vs C.
- [ ] **P2-04.3 SIMD port**
  - Files: `libobs/media-io/format-conversion.c` (288; C is SSE via `sse-intrin.h`, no scalar path).
  - Steps: implement the 4-pixel-group loops with `std::simd`; keep the 32-bit wrap of `width*4` (`wrapping_*`, overflow checks off per S-013 §9).
  - Spec: S-013 D-2, §9.
  - Vectors: S-013 `mediaconv` conformance (27 scripts) green.
  - Pitfalls: D-2 — C faults on unaligned inputs and overruns for `ols0` not a multiple of 4; Rust MUST accept any alignment, MUST NOT write outside `[0, rows*ols)`, SHOULD process only `ols0` pixels when `ols0 % 4 != 0`.
  - Done when: S-013 `mediaconv` conformance (27 scripts) green.
- [ ] **P2-04.4 Benchmark**
  - Files: `crates/obs-media` benches (criterion, P0-17 harness).
  - Steps: bench all six converters against the C baseline stored in `docs/rust-rewrite/benchmarks.md`.
  - Done when: ≥ C speed on x86_64 (bench report).

### P2-05 HDR helpers
- [ ] **P2-05.1 Transfer functions**
  - Files: none in C (libobs has no CPU-side PQ/HLG maths — `libobs/data/color.effect` + `format_conversion.effect` do it in shaders, S-053 R41; FFmpeg elsewhere). `obs_get_video_hdr_nominal_peak_level` / `obs_get_video_sdr_white_level` are thin getters in `libobs/obs.c` (3491) — Phase 4.
  - Steps: implement SMPTE ST 2084 PQ EOTF/OETF and BT.2100 HLG encode/decode as a pure-Rust reference in `crates/obs-media`; 256-point golden.
  - Spec: S-053 §5.4 R41 pins the shader-side PQ/HLG constants (ST2084 both ways, HLG OOTF/EOTF) the curves must agree with.
  - Done when: 256-point golden.
- [ ] **P2-05.2 Tonemap constants**
  - Files: `libobs/data/color.effect` (S-053 R41: `reinhard`, `rec709_to_rec2020`, `rec2020_to_rec709`).
  - Steps: document the constants used by the `sdr-on-hdr` / `hdr-tonemap` filters (Phase 5, P5-01.4) with the sRGB curve from P1-18 (`libobs/graphics/srgb.h`, 177).
  - Done when: constants documented.
- [ ] **P2-05.3 Golden**
  - Steps: commit the PQ/HLG 256-point curve goldens and the tonemap-constant table.
  - Done when: goldens committed and re-running is byte-identical.

### P2-06 Audio resampler
- [ ] **P2-06.1 Trait**
  - Files: `libobs/media-io/audio-resampler.h` (44) — the contract: `audio_resampler_t`, `audio_resampler_resample` returning a timestamp offset.
  - Steps: define a `Resampler` trait in `crates/obs-media` with `resample(&mut self, data) -> u64` (ts offset) and a sample-info accessor.
  - Spec: S-032 R02/R03 (resampler is a trait; the conformance harness uses a zero-order-hold stand-in).
  - Done when: documented.
- [ ] **P2-06.2 swresample wrapper**
  - Files: `libobs/media-io/audio-resampler-ffmpeg.c` (218).
  - Steps: wrap `swr_convert` behind FFI; copy the speaker-layout → swr channel mapping, the format mapping, and the ts-offset arithmetic exactly.
  - Vectors: avio (S-044) drives the trait with fakes; the swresample output itself is checked by the 1-LSB comparison.
  - Done when: output within 1 LSB of C for 5 conversions.
- [ ] **P2-06.3 Mix tables**
  - Files: `libobs/media-io/audio-resampler-ffmpeg.c` (218; layout mapping).
  - Steps: table the mono↔stereo↔5.1↔7.1 up/down-mix coefficients and golden them.
  - Done when: golden.

### P2-07 Audio maths
- [ ] **P2-07.1 dB conversion**
  - Files: `libobs/media-io/audio-math.h` (43).
  - Steps: port `mul_to_db`, `db_to_mul` as f32 per R10: 0 → −inf, negative mul → NaN, +inf → +inf, non-finite db → 0, large db → +inf.
  - Spec: S-013 R10.
  - Vectors: mediaconv `mul_to_db`, `db_to_mul`.
  - Done when: table + edge-value golden.
- [ ] **P2-07.2 Conformance**
  - Steps: run the mediaconv scripts covering the dB maths against the Rust runner.
  - Spec: S-013 `mediaconv`.
  - Vectors: test/conformance/scripts/mediaconv/.
  - Done when: green.
- [ ] **P2-07.3 Tests**
  - Steps: standalone unit tests for `mul_to_db`/`db_to_mul` (static inline in C: they must be compiled into the test binary — port them, do not link the C header).
  - Done when: equal to the C golden including the R10 edge values (0, ±inf, NaN).

## B. IO threads

### P2-08 `audio-io` core
- [ ] **P2-08.1 Thread loop**
  - Files: `libobs/media-io/audio-io.c` (455), `libobs/media-io/audio-io.h` (228).
  - Steps: port the `audio_thread` loop per R80 into `crates/media-io` module `audio_output`: `samples += 1024` (`AUDIO_OUTPUT_FRAMES`), `due = start + frames_to_ns(rate, samples)` (integer arithmetic), `Clock::sleep_until(due)` seam; time derived from the absolute sample counter, not "now + interval".
  - Spec: S-044 §5.10 R80–R86.
  - Vectors: avio (26 scripts, lock-step fake clock).
  - Pitfalls: R80 — `frames_to_ns` truncates (44100 Hz tick 1 = 23219954 ns); a late tick returns immediately and later ticks run back to back (no skipping).
  - Done when: simulated 60 s run produces exact sample count.
- [ ] **P2-08.2 Inputs and mixes**
  - Files: `libobs/media-io/audio-io.c` (455).
  - Steps: port `audio_output_connect(mix, conv, cb, param)` (R74–R77): 6 mix buffers (`MAX_AUDIO_MIXES`), identity `(mix, cb, param)`, conversion with UNKNOWN fields defaulted (R75), per-input resampler when (format, rate, speakers) differ (R76).
  - Spec: S-044 §5.9 R74–R77.
  - Done when: connect/disconnect while running does not drop data.
- [ ] **P2-08.3 Buffering**
  - Files: `libobs/media-io/audio-io.c` (455).
  - Steps: port the per-input buffer handling and `audio_output_get_block_size` (S-044 §5.8).
  - Spec: S-044 §5.8.
  - Done when: matches C trace.
- [ ] **P2-08.4 Live modification**
  - Files: `libobs/media-io/audio-io.c` (455).
  - Steps: add/remove inputs from other threads without locks on the tick path; delivery runs from the last input to the first with the index re-read after each callback (R83).
  - Spec: S-044 R83, Q-2.
  - Pitfalls: Q-2 — removing an input with a *lower* index than the running one shifts the list and the running input is called again (vector `audio_reentrancy`: inputs [1,2,3], 3 disconnects 1 → order 3,3,2).
  - Done when: loom test.
- [ ] **P2-08.5 RT audit**
  - Steps: preallocate all buffers at connect; run the tick under the P2-12 harness.
  - Spec: S-044 §7 (concurrency).
  - Done when: passes the P2-12 `assert_no_alloc` harness.

### P2-09 `video-io` core
- [ ] **P2-09.1 Thread and cache**
  - Files: `libobs/media-io/video-io.c` (679), `libobs/media-io/video-io.h` (338).
  - Steps: port `video_output_open` (R01–R04: `frame_time = floor(1e9 * fps_den / fps_num)`, cache clamped to 16), the ring of `cached_frame_info {frame, skipped, count}`, and the `video_thread` worker loop (R20–R22) into `crates/media-io` module `video_output`.
  - Spec: S-044 §5.1–5.3 R01–R25.
  - Vectors: avio (26 scripts).
  - Pitfalls: R23 — while the worker is busy, failed `lock_frame` calls fold their `count` into the newest slot as extra repeats and as `skipped` (vector `video_lag`: slot gets `count=3, skipped=2`).
  - Done when: ring semantic test.
- [ ] **P2-09.2 Connect/disconnect**
  - Files: `libobs/media-io/video-io.c` (679).
  - Steps: port `video_output_connect2/disconnect2` (R30–R39): identity `(callback, param)`, duplicate → `false`, conversion `conv==NULL` → output geometry but format/range/colorspace never defaulted (R33), scaler needed per R34, created `FAST_BILINEAR` with three conversion buffers (R35).
  - Spec: S-044 §5.4 R30–R39.
  - Pitfalls: Q-10 — a `NONE` format conversion is not defaulted; the scaler layer rejects it ("Bad scale conversion type"). Q-1 — removal inside a callback skips an input for this pass (and its divisor counter is not advanced).
  - Done when: two inputs with different formats.
- [ ] **P2-09.3 Lock/unlock frame**
  - Files: `libobs/media-io/video-io.c` (679).
  - Steps: port `video_output_lock_frame`/`unlock_frame` with timestamp (R10–R12): `avail == 0` folds `count` into the last slot; unlock wakes the worker once.
  - Spec: S-044 §5.2 R10–R13.
  - Done when: producer/consumer test.
- [ ] **P2-09.4 Counters**
  - Files: `libobs/media-io/video-io.c` (679).
  - Steps: port `video_output_get_skipped_frames`/`get_total_frames` (u32 truncation, R06), `inc_texture_frames`/`inc_texture_skipped_frames` (R40), encoder-ref resets (R41–R42), the stop log (R43).
  - Spec: S-044 §5.5 R40–R44.
  - Pitfalls: Q-7 — the stop summary percentage is computed in `double` from `long` counters and may print `inf` or exceed 100 (`2/1 (200.0%)`, `1/0 (inf%)`); connecting the first input resets the counters even after a stop.
  - Done when: 10 min simulated run equals C.
- [ ] **P2-09.5 Frame rate divisor**
  - Files: `libobs/media-io/video-io.c` (679), `libobs/media-io/frame-rate.h` (29).
  - Steps: port `video_output_create_with_frame_rate_divisor` / `video_output_free_frame_rate_divisor` (R50–R52): NULL for `d == 0/1`, shallow child with `fps_den *= d`, every call maps to the root except `get_info`/`get_frame_time`.
  - Spec: S-044 §5.6 R50–R52.
  - Pitfalls: Q-4 — a child's `get_frame_rate` reports the root's rate, `get_frame_time` the unscaled copy, `get_info` the child's (`fps_den` multiplied; `frame_time` not).
  - Done when: tests for 24/30/60 and NTSC.

### P2-10 Timing and clock
- [ ] **P2-10.1 Clock trait**
  - Steps: define `Clock { now_ns(), sleep_until(ns) }` with real and fake implementations; all timing code takes `&dyn Clock` (S-044 §9 recommends exactly this seam).
  - Spec: S-044 §9 (implementation notes).
  - Done when: all timing code takes `&dyn Clock`.
- [ ] **P2-10.2 Frame loop**
  - Files: `libobs/obs-video.c` (1200) — `obs_graphics_thread_loop` (line 1094).
  - Steps: port the per-frame arithmetic: next frame time, `video_sleep` (R23: `count = clamped/interval`, `lagged_frames += count - 1`), one-frame-late `vframe_info` (R23), `stop_requested` (R24), fps/avg-frame-time accumulators (R22).
  - Spec: S-058 §5.1, §5.5 R20–R25, §5.6.
  - Vectors: obsvideo (23 scripts).
  - Pitfalls: Q-1 (S-058) — with **no mixes** `stop_requested()` is vacuously true: the loop returns false after one executed frame.
  - Done when: fake-clock run for 1000 frames equals C trace.
- [ ] **P2-10.3 Drift**
  - Files: `libobs/obs-video.c` (1200).
  - Steps: port `obs_get_video_frame_time` and the audio↔video offset (`audio_ts`) handling; drift test against the fake clock.
  - Spec: S-058 §5.5.
  - Done when: drift test.

### P2-11 Lock-free queues
- [ ] **P2-11.1 SPSC ring**
  - Files: `libobs/util/deque.h` (the replaced `deque + mutex` pattern).
  - Steps: choose `rtrb` or a custom SPSC ring; write the justification as an ADR note.
  - Note: no spec yet (threading primitives are gap S-061, decision D-27) — read the C.
  - Done when: ADR note.
- [ ] **P2-11.2 Replace**
  - Steps: swap the ring into the audio pending-buffer and video cache paths.
  - Done when: benchmark unchanged or better.
- [ ] **P2-11.3 loom** models.
  - Done when: loom models green.

### P2-12 Real-time audit
- [ ] **P2-12.1 Harness**
  - Steps: `assert_no_alloc` + lock-detect wrapper in tests.
  - Done when: demonstrates failing on a deliberate alloc.
- [ ] **P2-12.2 Run** audio tick and video tick under it.
  - Spec: S-044 §7.
  - Done when: both ticks pass.
- [ ] **P2-12.3 Fix/document** exceptions.
  - Done when: exceptions documented in the ADR.

## C. Audio mixing and controls

### P2-13 Audio mixer
- [ ] **P2-13.1 Source buffers**
  - Files: `libobs/obs-audio.c` (721).
  - Steps: consume per-source per-mix float buffers (`source->audio_output_buf[mix][ch]`, allocated by P4-05; supplied by a fake source here) into `audio_output_data` per R30–R33 (setup, render order, `obs_source_audio_render`).
  - Spec: S-032 §5.6 R30–R33.
  - Vectors: audiomix (39 scripts).
  - Done when: add/consume tests.
- [ ] **P2-13.2 Alignment**
  - Files: `libobs/obs-audio.c` (721).
  - Steps: port `calc_min_ts` (R34), `add_audio_buffering`/`set_fixed_audio_buffering` (R35), the `maxed` equality check (R36), `discard_if_stopped` (R43), `ignore_audio` (R39).
  - Spec: S-032 §5.6 R34–R36, R39, R43.
  - Done when: timestamp cases equal to C.
- [ ] **P2-13.3 Mute/monitoring**
  - Files: `libobs/obs-audio.c` (721).
  - Steps: port `mix_audio` (summation only, R40); mute (→ vol 0 via `get_source_volume`) and per-mix volume land source-side in `apply_audio_volume` (P2-16); port `should_silence_monitored_source` (R37).
  - Spec: S-032 R37, R40.
  - Pitfalls: Q-1 — the monitoring-silence `muted` is the audio-thread copy, applied only when that source is rendered: a source rendered before it sees the old state for one tick (vector `37_monitoring`). Q-2 — a root node listed twice is mixed twice (`19_double_root`: amplitude ×3 for three roots).
  - Done when: golden.
- [ ] **P2-13.4 Missing data**
  - Files: `libobs/obs-audio.c` (721).
  - Steps: port `source->audio_pending`, `audio_ts`, `audio_buffering` handling including the buffer-insufficient path (R38).
  - Spec: S-032 R38–R42.
  - Pitfalls: Q-8 — `audio_buffer_insufficient` marks a source pending as a side effect of the min-timestamp calculation (vector `20_buffering_dynamic`).
  - Done when: gap test.
- [ ] **P2-13.5 Golden**
  - Steps: 8 sources × 6 mixes × 10 s PCM, hash-compare against the C build.
  - Vectors: audiomix.
  - Done when: 8 sources × 6 mixes × 10 s PCM, hash compare.

### P2-14 Fader and meter
- [ ] **P2-14.1 Curves**
  - Files: `libobs/obs-audio-controls.c` (878).
  - Steps: port the cubic / IEC 60268-18 / log `def_to_db`/`db_to_def` curves (R45), `obs_fader_set_db` (R46), `attach_source` (R47), the "volume" handler (R48), `obs_fader_db_to_def` (R49) to `crates/obs-audio-mix`.
  - Spec: S-032 §5.7 R45–R49.
  - Vectors: audiomix (fader scripts, e.g. `41_fader_curve_iec`).
  - Pitfalls: Q-5 — IEC `db_to_def` is not the inverse of `def_to_db` below −60 dB. Q-6 — `obs_fader_set_db` arms `ignore_next_signal` even with no source, so the first external volume change after a later attach is swallowed.
  - Done when: 1001-point golden.
- [ ] **P2-14.2 Volmeter peak**
  - Files: `libobs/obs-audio-controls.c` (878).
  - Steps: port `obs_volmeter_create/attach_source/add_callback`, `obs_volmeter_set_peak_meter_type` (R50–R55), sample-peak and magnitude.
  - Spec: S-032 §5.8 R50–R55.
  - Pitfalls: Q-4 — channel magnitudes beyond the current plane count are never cleared (peaks are); each peak includes the 4-sample history; frames `% 4` trailing samples are not part of the block's own peak.
  - Done when: sine input yields expected dB.
- [ ] **P2-14.3 True peak**
  - Files: `libobs/obs-audio-controls.c` (878; `get_true_peak`).
  - Steps: port the Whittaker-Shannon oversampling coefficients exactly as in the C (S-032 §6.2).
  - Spec: S-032 §6.2.
  - Pitfalls: the C comments disagree on 4× vs 5× oversampling — reproduce the coefficients exactly; the goldens decide.
  - Done when: ITU test signal reading.
- [ ] **P2-14.4 Golden**
  - Steps: 1001-point curve goldens + volmeter goldens from the audiomix vectors.
  - Vectors: audiomix.
  - Done when: goldens committed and green.

### P2-15 Audio monitoring
- [ ] **P2-15.1 Trait**
  - Files: `libobs/obs.h` (2734; public `obs_audio_monitoring_available`/`obs_enum_audio_monitoring_devices`), `libobs/obs-internal.h` (1543; `audio_monitor_create/reset/destroy`), each backend's `devices_match`.
  - Steps: define a `MonitorBackend` trait from the C contract.
  - Done when: documented.
- [ ] **P2-15.2 PulseAudio**
  - Files: `libobs/audio-monitoring/pulse/`: `pulseaudio-enum-devices.c` (31), `pulseaudio-monitoring-available.c` (6), `pulseaudio-output.c` (528), `pulseaudio-wrapper.c` (389), `pulseaudio-wrapper.h` (212).
  - Spec: S-055 (PulseAudio audio monitor; target crate `obs-audio-monitor-pulse`).
  - Vectors: utilrest (26 scripts).
  - Done when: monitor plays a test tone.
- [ ] **P2-15.3 WASAPI**
  - Files: `libobs/audio-monitoring/win32/`: `wasapi-enum-devices.c` (174), `wasapi-monitoring-available.c` (6), `wasapi-output.c` (463), `wasapi-output.h` (22).
  - Note: no spec — gap S-065 (decision D-31); port by reading the C (thin platform glue).
  - Done when: same on Windows.
- [ ] **P2-15.4 CoreAudio + null**
  - Files: `libobs/audio-monitoring/osx/`: `coreaudio-enum-devices.c` (226), `coreaudio-monitoring-available.c` (6), `coreaudio-output.c` (322), `mac-helpers.h` (13); `libobs/audio-monitoring/null/null-audio-monitoring.c` (35).
  - Note: no spec — gap S-065 (decision D-31); port by reading the C.
  - Done when: compile and play on macOS.
- [ ] **P2-15.5 Device switch** — manual test per OS.
  - Done when: device switch verified manually on each OS.
- [ ] **P2-15.6 Write S-065 (added)** — audio-monitoring win32/osx/null spec (D-31)
  - Files: `libobs/audio-monitoring/win32/wasapi-output.c` (463), `libobs/audio-monitoring/osx/coreaudio-output.c` (322), `libobs/audio-monitoring/null/null-audio-monitoring.c` (35).
  - Steps: 1. read the three C backends (thin platform glue) and enumerate the contract: device matching, stream creation, format handling, error paths. 2. write `specs/S-065-audio-monitoring-platforms.md` mirroring S-055 (PulseAudio). 3. add the row to `specs/README.md`.
  - Done when: S-065 committed; P2-15.3/.4 cite it.

### P2-16 Audio volume/pan/mono kernels
- [ ] **P2-16.1 Kernels**
  - Files: `libobs/obs-source.c` (6070) — `apply_audio_volume` (line 5335), `multiply_output_audio` (5252), `process_audio_balancing` (4000), `downmix_to_mono_planar` (3980). P4-05 owns the file; fake buffers here.
  - Steps: port the four as pure functions over fake buffers: per-mix gain, sine-law pan matrix, mono downmix (and the mixer-mask buffer clearing in `process_audio_source_tick`).
  - Note: no spec covers these obs-source kernels (S-032 covers the mixer/fader side) — port by reading the C.
  - Done when: golden on stereo and 5.1 buffers.
- [ ] **P2-16.2 Benchmark**
  - Steps: criterion bench vs the C baseline (P0-17).
  - Done when: bench report committed.

## D. Bitstream parsers

### P2-17 NAL / start-code
- [ ] **P2-17.1 Scanner**
  - Files: `libobs/obs-nal.c` (66), `libobs/obs-nal.h` (37).
  - Steps: port `obs_nal_find_startcode` per R01–R05 (word-aligned Annex-B scan for `00 00 01`/`00 00 00 01`; the wrapper normalises a 4-byte start code to the 3-byte position) and the `OBS_NAL_PRIORITY` enum to `crates/obs-video-bitstream` (S-012 target crate).
  - Spec: S-012 §5.1 R01–R05, §3.
  - Vectors: nal (19 scripts; `find` command).
  - Pitfalls: Q-1 (S-012) — a start code ending in the last 3 bytes is not found; those bytes stay in the previous NAL (scripts `02`, `04`, `05`).
  - Done when: vectors.
- [ ] **P2-17.2 Emulation-prevention edges**
  - Files: `libobs/obs-nal.c` (66).
  - Steps: the C scanner is naive: it does **not** skip `00 00 03 xx` sequences, so pin its behaviour with EPB bytes present in the vectors (EPB stripping belongs to the SPS path, P2-18, S-012 R34).
  - Spec: S-012 Q-1, R34.
  - Vectors: nal.
  - Done when: vectors.
- [ ] **P2-17.3 Fuzz** target.
  - Done when: fuzz target runs clean in CI.

### P2-18 AVC
- [ ] **P2-18.1 Keyframe/NAL types**
  - Files: `libobs/obs-avc.c` (321), `libobs/obs-avc.h` (55).
  - Steps: port `obs_avc_keyframe` (R10: first NAL with type 1 or 5) and `obs_avc_find_startcode` (R04).
  - Spec: S-012 §5.3 R10.
  - Vectors: nal `avc_keyframe`.
  - Done when: samples.
- [ ] **P2-18.2 SPS/PPS**
  - Files: `libobs/obs-avc.c` (321), `libobs/obs-avc.h` (55).
  - Steps: port `obs_parse_avc_header` (R30–R35: `n <= 6` → 0; start code tested only at offset 0; last SPS/PPS wins; big-endian 16-bit lengths; high-profile fields strip EPB bytes per R34; result `bmalloc`ed) and `obs_extract_avc_headers` (R40).
  - Spec: S-012 §5.3 R30–R35, R40.
  - Vectors: nal `avc_header`, `avc_extract`.
  - Pitfalls: Q-5 — input not starting with a start code is returned as an exact copy (also `00 00 00 00 01`, `aa 00 00 00 01`). D-6 — SPS/PPS > 65535 bytes must return 0 (C corrupts the record). D-4 — high-profile `ue()` on 31/32 zero bits is UB in C: no crash, same output length.
  - Done when: headers equal C on an x264 stream.
- [ ] **P2-18.3 Conversion**
  - Files: `libobs/obs-avc.c` (321).
  - Steps: port `obs_parse_avc_packet` (R22: Annex-B → AVCC with the refcount prefix) and `obs_parse_avc_packet_priority` (R20–R21); C has no reverse direction.
  - Spec: S-012 §5.3 R20–R24.
  - Pitfalls: Q-2 — NAL header bit 7 set yields priority 4..7. Q-4 — adjacent start codes emit a zero-length NAL; priority from the first byte of the next start code. Q-6 — the source `keyframe` flag is only ever raised, never cleared.
  - Done when: output bytes equal C on sample streams.
- [ ] **P2-18.4 Fuzz + goldens**
  - Steps: `cargo-fuzz` target; goldens from real x264 output.
  - Vectors: nal.
  - Done when: fuzz green, goldens committed.

### P2-19 HEVC
- [ ] **P2-19.1 Headers**
  - Files: `libobs/obs-hevc.c` (202), `libobs/obs-hevc.h` (81).
  - Steps: port `obs_extract_hevc_headers` (R54: NAL types 32/33/34 → `header`, 39/40 → `sei`, rest → `new`).
  - Spec: S-012 §5.4 R54.
  - Vectors: nal `hevc_extract`.
  - Done when: equal on a sample.
- [ ] **P2-19.2 Keyframe** detection (IRAP)
  - Steps: port `obs_hevc_keyframe` (R50: the first NAL with `t <= 23` decides; `t >= 16` = IRAP), per-NAL priority (R51), `obs_parse_hevc_priority` (R52), `obs_parse_hevc_packet` (R53).
  - Spec: S-012 §5.4 R50–R53.
  - Vectors: nal `hevc_keyframe`, `hevc_priority`, `hevc_packet`.
  - Done when: samples.
- [ ] **P2-19.3 Goldens + fuzz**
  - Steps: NVENC/QSV sample streams; `cargo-fuzz`.
  - Done when: fuzz green, goldens committed.

### P2-20 AV1
- [ ] **P2-20.1 OBU parser**
  - Files: `libobs/obs-av1.c` (208), `libobs/obs-av1.h` (47).
  - Steps: port `parse_obu_header` (R70: type/ext/size from the header byte, LEB128 size) and `obs_av1_keyframe` (R71: FRAME(6)/FRAME_HEADER(3) frame-type bits).
  - Spec: S-012 §5.5 R70–R71.
  - Vectors: nal `av1_keyframe`, `av1_obu`, `av1_leb128`.
  - Pitfalls: Q-7 — an OBU with the extension flag but no size field: `size = avail - 1` (the extension byte is also counted in the header). D-2 — FRAME/FRAME_HEADER with non-zero size but no payload byte: return `false` (C reads 1 byte past the end).
  - Done when: header types parsed.
- [ ] **P2-20.2 Headers**
  - Files: `libobs/obs-av1.c` (208).
  - Steps: port `obs_extract_av1_headers` (R72: clamped walk; sequence header + metadata OBUs), `metadata_obu` (R73: `2a || leb128(len+2) || type || payload || 80`), `metadata_obu_itu_t35`.
  - Spec: S-012 §5.5 R72–R73.
  - Vectors: nal `av1_extract`, `av1_metadata`, `av1_metadata_t35`.
  - Pitfalls: Q-3 — `new` also contains the header OBUs and TDs. D-1 — an OBU extent past the end must be clamped (C reads out of bounds). D-3 — LEB128 groups 5..8: produce the exact 64-bit value (C overflows `int`).
  - Done when: sample equal.
- [ ] **P2-20.3 Goldens + fuzz**
  - Steps: sample streams; `cargo-fuzz`.
  - Done when: fuzz green, goldens committed.

### P2-21 Caption support
- [ ] **P2-21.1 FFI wrapper**
  - Files: `deps/libcaption/` (≈7,465 lines across `caption/` + `src/`; kept as FFI per D-18 — no port).
  - Steps: build a Rust crate wrapping `caption_*`/`sei_*`; decode CEA-608/708 and compare against C.
  - Done when: crate wraps `caption_*`/`sei_*` and CEA-608/708 decode equals C.
- [ ] **P2-21.2 Encode path**
  - Files: `libobs/obs-output.c` (3340; the caption queue), `libobs/obs.h` (2734; `obs_output_output_caption_text1/2`, `obs_output_caption` at lines 2159–2162).
  - Steps: port the caption queue and the `obs_output_output_caption_text1/2` entry points with a fake output (the file itself is P4-23).
  - Note: S-041 §1.2 excludes captions from the spec — port by reading the C.
  - Done when: CEA-608 bytes equal.
- [ ] **P2-21.3 SEI injection**
  - Files: `libobs/obs-output.c` (3340; `add_caption`).
  - Steps: golden the injected SEI bytes for H.264/HEVC and the ITU-T T35 metadata OBU for AV1.
  - Done when: golden SEI bytes in H.264/HEVC, ITU-T T35 metadata OBU for AV1.

## E. Packet queues and delay

### P2-22 Encoder packet queue
- [ ] **P2-22.1 Packet type**
  - Files: `libobs/obs-encoder.c` (2286), `libobs/obs-encoder.h` (365).
  - Steps: port the `encoder_packet` struct (S-041 §3.3 field list, refcount in the 8 bytes before `data`), `obs_encoder_packet_create_instance`, `obs_encoder_packet_ref/release` to `crates/obs-encoder-output` (S-041 target crate).
  - Spec: S-041 §3.3, §5.1.
  - Vectors: nal (`layout` command — offsets of `struct encoder_packet`), encoutput (33 scripts).
  - Done when: ABI layout test.
- [ ] **P2-22.2 Timestamps**
  - Files: `libobs/obs-encoder.c` (2286).
  - Steps: port the `dts_usec` computation (`start_ts / 1000 + dts_us - offset_usec`) and `sys_dts_usec = dts_usec + pause.ts_offset / 1000` (S-041 §5.1 R08); port `pause_data` (`ts_start`/`ts_end`/`ts_offset`), `video_pause_check_internal`, `pause_reset` from the C.
  - Spec: S-041 §5.1 R08 (dts formula); §1.2/§11 — pause/unpause is explicitly **not** specified, port it by reading the C.
  - Note: `encoder->paused` is set by the output (P4-23) — fake input here.
  - Done when: pause/resume trace equal.
- [ ] **P2-22.3 Queue tests**
  - Steps: queue traces for the packet path (keyframe gate, hold-back).
  - Vectors: encoutput `enc_packet_path`, `enc_hold_bframes`, `enc_flow_sync`.
  - Done when: traces equal the vectors.

### P2-23 Output delay buffer
- [ ] **P2-23.1 Queue**
  - Files: `libobs/obs-output-delay.c` (209).
  - Steps: port the `process_delay` queue per R72: copy each arriving packet with `ts = clock`, release the front while `now - ts > active_delay_ns` (strictly greater, unsigned), drain **only when a packet arrives** (no timer).
  - Spec: S-041 §5.6 R70–R75.
  - Vectors: encoutput `dl_start`, `dl_stop_drop`, `dl_force_stop`.
  - Pitfalls: Q-9 — a delayed stop never completes without further packets (no timer). Q-8 — `force_stop` of a delayed output emits the `stop` signal twice.
  - Done when: delay of N seconds measured.
- [ ] **P2-23.2 Reconnect** — `OBS_OUTPUT_DELAY_PRESERVE`
  - Steps: port the PRESERVE path (R72/R75): while reconnecting the front entry is not released and `active_delay_ns = now - front.ts` (the delay grows with the outage).
  - Spec: S-041 R72, R75.
  - Vectors: encoutput `dl_reconnect_preserve`, `dl_reconnect_nopreserve`.
  - Done when: test.
- [ ] **P2-23.3 Smoke** — 20 s delay recording.
  - Done when: 20 s delay recording smoke passes.

### P2-24 Interleaving
- [ ] **P2-24.1 Interleave/send**
  - Files: `libobs/obs-output.c` (3340; the interleave part).
  - Steps: port `interleave_packets` (R41–R43: keyframe gate, drop-before-insert, insert order), `send_interleaved` (R48–R49: at most two packets per arrival), `find_first_packet_type_idx` (start index R46), `apply_ept_offsets` (R47: video offset = first PTS, audio offset = first dts only if `dts > 0`).
  - Spec: S-041 §5.4 R40–R50.
  - Vectors: encoutput `il_start_basic`, `il_offsets`, `il_steady_state`, `il_multitrack_*`.
  - Pitfalls: R43 — at equal `dts_usec` a video packet goes before ANY queued packet unless both are video and the new track index is greater; audio keeps arrival order. Q-2 — after start, `dts_usec` is recomputed from `dts` alone (no `start_ts`).
  - Done when: trace equal.
- [ ] **P2-24.2 Congestion**
  - Files: `libobs/obs-output.c` (3340).
  - Steps: port the drop paths `discard_to_idx` (R41/R46), `discard_unused_audio_packets` (R47), `prune_premature_packets`/`prune_interleaved_packets` (R45) and backpressure `calculate_batch_size` → `interleaver_max_batch_size` (R50).
  - Spec: S-041 §5.4 R45, R47, R50.
  - Vectors: encoutput `il_prune_far_audio`, `il_prune_two_audio_tracks`, `il_batch_accumulates`, `il_batch_overflow`.
  - Pitfalls: Q-5 — with several audio tracks the prune decision uses the *last* track's gap, not the largest. Q-4b — the start-up prune discards `[0, max_idx]` inclusive of the offending audio packet. Q-7 — `interleaver_max_batch_size` is never reset: 5, 10, 15 over three starts of the same output.
  - Done when: scripted congestion test.
- [ ] **P2-24.3 Golden traces**
  - Steps: commit the interleaver queue traces as goldens.
  - Vectors: encoutput.
  - Done when: golden traces match.

## F. Integration

### P2-25 Wire into libobs
- [ ] **P2-25.1 CMake switch** per crate.
  - Files: `CMakeLists.txt`, `libobs/CMakeLists.txt` (the `OBS_ENABLE_RUST` option from P0-08).
  - Steps: add a per-crate CMake switch linking the Rust static libs into `libobs`.
  - Done when: ON/OFF builds.
- [ ] **P2-25.2 Fallback** C path remains. Done when: CI runs both.
- [ ] **P2-25.3 CI matrix**
  - Done when: CI matrix covers both switches on 3 OSes.

### P2-26 Soak test
- [ ] **P2-26.1 Script** — 24 h headless record (1080p60, 6 audio mixes, x264). Done when: runs unattended.
- [ ] **P2-26.2 Metrics** — CPU, RSS, dropped frames every minute to CSV.
  - Done when: CSV produced for the full run.
- [ ] **P2-26.3 Compare** to C: RSS growth < 1%/h, dropped frames not worse.
  - Done when: comparison report committed.

### P2-27 Latency
- [ ] **P2-27.1 Signal** — flash+beep source.
  - Files: `test/test-input/sync-pair-aud.c` (134), `test/test-input/sync-pair-vid.c` (133) exist for this.
  - Done when: flash+beep source built into the test host.
- [ ] **P2-27.2 Measure** A/V offset in recordings of both builds.
  - Done when: A/V offset measured on both builds.
- [ ] **P2-27.3 Gate** — delta = 0 ms at defaults.
  - Done when: delta = 0 ms at defaults, reported.

### P2-28 Phase 2 review
- [ ] **P2-28.1 Review** benchmarks, fuzz corpus, soak.
  - Done when: review notes committed.
- [ ] **P2-28.2 Tick** parent tasks.
  - Done when: parent tasks ticked in `../phase-2-media-pipeline.md`.

### P2-29 `media-remux` (added)
- [ ] **P2-29.1 Port**
  - Files: `libobs/media-io/media-remux.c` (266), `libobs/media-io/media-remux.h` (37).
  - Steps: port the remux helper (open input, copy streams, progress callback) on top of the P2-03 frame/scaler types.
  - Note: no spec (S-013 §1 scope is the pure media-io functions; remux is FFmpeg I/O) — port by reading the C.
  - Done when: output equals C by `ffprobe` JSON.
- [ ] **P2-29.2 Frontend hook** — used by remux dialog (P6-28).
  - Done when: API doc.
