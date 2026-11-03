# Phase 2 subtasks

Parent file: [../phase-2-media-pipeline.md](../phase-2-media-pipeline.md). Each subtask has **Files**, **Steps**, **Done when**.

Real-time (RT) rule for every subtask that touches audio or video threads: no allocation, no blocking lock, no syscall on the hot path; enforce with the harness from P2-12.

Verified file map: `libobs/media-io/` contains `audio-io.c/.h`, `audio-math.h`, `audio-resampler-ffmpeg.c`, `audio-resampler.h`, `format-conversion.c/.h`, `frame-rate.h`, `media-io-defs.h`, `media-remux.c/.h`, `video-fourcc.c`, `video-frame.c/.h`, `video-io.c/.h`, `video-matrices.c`, `video-scaler-ffmpeg.c`, `video-scaler.h`. `libobs/audio-monitoring/` has `null/`, `osx/`, `pulse/`, `win32/` (no ALSA backend). GPU encoding lives in `libobs/obs-video-gpu-encode.c`. `media-remux.c` was missing from the earlier plan and is added as P2-29.

**Dependency note (see `../guides/phase-2-guide.md`):** P2-10.2 (`obs_graphics_thread_loop`), P2-13 (`audio_callback`), P2-16 (the volume/pan/mono kernels live in `obs-source.c`) and P2-22…P2-24 (`obs-encoder.c` / `obs-output.c`) are entangled with Phase 4 objects. Do the pure algorithm in Phase 2 with fake inputs; the integration lands in Phase 4.

## A. Formats and conversion

### P2-01 Format enums and helpers
- [ ] **P2-01.1 Enums**
  - Files: `libobs/media-io/video-io.h`, `audio-io.h`
  - Steps: `#[repr(C)]` enums `video_format` (NONE, I420, NV12, YVYU, YUY2, UYVY, RGBA, BGRA, BGRX, Y800, I444, BGR3, I422, I40A, I42A, YUVA, AYUV, I010, P010, I210, I412, YA2L, P216, P416, V210, R10L), `audio_format`, `speaker_layout`, `video_colorspace`, `video_range_type`.
  - Done when: numeric values asserted equal to the C headers via a generated test.
- [ ] **P2-01.2 Helpers** — `get_audio_channels`, `get_audio_bytes_per_channel`, `is_audio_planar`, `format_is_yuv`, `video_format_get_parameters*`, `audio_frames_to_ns` / `ns_to_audio_frames` / `get_total_audio_size` (`audio-io.h`). Done when: table test.
- [ ] **P2-01.3 FourCC** — `video-fourcc.c`: `video_format_from_fourcc` only (C has no format->fourcc direction). Done when: every `MAKE_FOURCC` alias in the C switch maps to the same format; unknown -> `NONE`.

### P2-02 Colour matrices
- [ ] **P2-02.1 Tables** — `libobs/media-io/video-matrices.c`: `format_info` (601, 709, 2100) × `bpp_info`[9] × 2 ranges of 16-float matrices. Done when: all 54 tables equal C bit for bit (keep the C f32 operation order, S-013-R53).
- [ ] **P2-02.2 Parameters API** — `video_format_get_parameters` / `_for_format` (HLG remaps to the 2100_PQ matrix, S-013-R50; per-format bit depth, S-013-R52). Done when: golden covers every `(colorspace, range, format)` combination.
- [ ] **P2-02.3 Golden** from `obs-ref-dump`.

### P2-03 `video-scaler` and `video-frame`
- [ ] **P2-03.1 Trait** — `Scaler` with `scale(&mut self, dst, src)`; error type. Done when: documented.
- [ ] **P2-03.2 swscale wrapper**
  - Files: `video-scaler-ffmpeg.c`
  - Steps: create context from `video_scale_info`, map formats/colour spaces/range, handle default `SWS_BICUBIC` flags.
  - Done when: output hash for 8 conversions equals C.
- [ ] **P2-03.3 Frame helpers** — `video_frame_init/free/copy`, plane sizing for planar and 10-bit formats. Done when: size table equal for all formats.
- [ ] **P2-03.4 Hash tests**

### P2-04 SIMD pixel converters
- [ ] **P2-04.1 Packed->planar** — `format-conversion.c`: `compress_uyvx_to_i420`, `compress_uyvx_to_nv12`, `convert_uyvx_to_i444` (UYVX in, I420/NV12/I444 out). Done when: byte-exact vs C.
- [ ] **P2-04.2 Planar->packed** — `decompress_420`, `decompress_nv12`, `decompress_422` (planar in, 32-bit UYVX out). Done when: byte-exact vs C.
- [ ] **P2-04.3 SIMD port** — `std::simd` (C is SSE via `sse-intrin.h`, no scalar path); keep the 32-bit wrap of `width*4` (S-013). Done when: S-013 `mediaconv` conformance (27 scripts) green.
- [ ] **P2-04.4 Benchmark** — ≥ C speed on x86_64. Done when: bench report.

### P2-05 HDR helpers
- [ ] **P2-05.1 Transfer functions** — PQ (SMPTE ST 2084) and HLG (ARIB/BT.2100) encode/decode as a pure-Rust reference (the C libobs has no PQ/HLG maths — obs-filters shaders and FFmpeg do it; `obs_get_video_hdr_nominal_peak_level` / `obs_get_video_sdr_white_level` are thin getters in `obs.c`, Phase 4). Done when: 256-point golden.
- [ ] **P2-05.2 Tonemap constants** — used by `sdr-on-hdr` and `hdr-tonemap` filters later. Done when: constants documented.
- [ ] **P2-05.3 Golden**

### P2-06 Audio resampler
- [ ] **P2-06.1 Trait** — `Resampler`.
- [ ] **P2-06.2 swresample wrapper**
  - Files: `audio-resampler-ffmpeg.c`
  - Steps: channel layout mapping for speaker layouts, format mapping, `audio_resampler_resample` returning ts offset.
  - Done when: output within 1 LSB of C for 5 conversions.
- [ ] **P2-06.3 Mix tables** — mono↔stereo↔5.1↔7.1. Done when: golden.

### P2-07 Audio maths
- [ ] **P2-07.1 dB conversion** — `audio-math.h`: `mul_to_db`, `db_to_mul` (S-013-R10: 0 -> -inf, negative mul -> NaN, +inf -> +inf, non-finite db -> 0, large db -> +inf, all f32). Done when: table + edge-value golden.
- [ ] **P2-07.2 Conformance** — S-013 `mediaconv` scripts covering the dB maths. Done when: green.
- [ ] **P2-07.3 Tests** — standalone unit tests for `mul_to_db`/`db_to_mul` (static inline: must be compiled into the test binary). Done when: equal to the C golden including the R10 edge values (0, ±inf, NaN).

## B. IO threads

### P2-08 `audio-io` core
- [ ] **P2-08.1 Thread loop**
  - Files: `libobs/media-io/audio-io.c`
  - Steps: audio thread ticking per `AUDIO_OUTPUT_FRAMES` (1024), timestamp advance from sample rate.
  - Done when: simulated 60 s run produces exact sample count.
- [ ] **P2-08.2 Inputs and mixes** — per-mix `audio_output_connect(mix_idx, conversion, callback)`, 6 mix buffers. Done when: connect/disconnect while running does not drop data.
- [ ] **P2-08.3 Buffering** — `audio_input` buffers, `audio_output_get_block_size`. Done when: matches C trace.
- [ ] **P2-08.4 Live modification** — add/remove inputs from other threads without locks on the tick path. Done when: loom test.
- [ ] **P2-08.5 RT audit** — preallocate all buffers.

### P2-09 `video-io` core
- [ ] **P2-09.1 Thread and cache** — `video-io.c`: `video_output_open`, frame cache ring (`MAX_CACHE_SIZE`), `video_output_inc_texture_frames`. Done when: ring semantic test.
- [ ] **P2-09.2 Connect/disconnect** — inputs with `video_scale_info` conversion, scaler cache per input. Done when: two inputs with different formats.
- [ ] **P2-09.3 Lock/unlock frame** — `video_output_lock_frame/unlock_frame` with timestamp. Done when: producer/consumer test.
- [ ] **P2-09.4 Counters** — `video_output_get_skipped_frames`, `get_total_frames`. Done when: 10 min simulated run equals C.
- [ ] **P2-09.5 Frame rate divisor** — `video_output_create_with_frame_rate_divisor` / `video_output_free_frame_rate_divisor`, `frame-rate.h`. Done when: tests for 24/30/60 and NTSC.

### P2-10 Timing and clock
- [ ] **P2-10.1 Clock trait** — real and fake. Done when: all timing code takes `&dyn Clock`.
- [ ] **P2-10.2 Frame loop** — `libobs/obs-video.c` `obs_graphics_thread_loop`: next frame time, `video_sleep`, `video_frame_interval_ns`, lagged frame counters. Done when: fake-clock run for 1000 frames equals C trace.
- [ ] **P2-10.3 Drift** — `obs_get_video_frame_time`, audio↔video offset (`audio_ts`). Done when: drift test.

### P2-11 Lock-free queues
- [ ] **P2-11.1 SPSC ring** — choose `rtrb` or custom; justify. Done when: ADR note.
- [ ] **P2-11.2 Replace** — audio pending-buffer and video cache paths. Done when: benchmark unchanged or better.
- [ ] **P2-11.3 loom** models.

### P2-12 Real-time audit
- [ ] **P2-12.1 Harness** — `assert_no_alloc` + lock-detect wrapper in tests. Done when: demonstrates failing on a deliberate alloc.
- [ ] **P2-12.2 Run** audio tick and video tick under it.
- [ ] **P2-12.3 Fix/document** exceptions.

## C. Audio mixing and controls

### P2-13 Audio mixer
- [ ] **P2-13.1 Source buffers** — `libobs/obs-audio.c`: per-source per-mix float buffers (`source->audio_output_buf[mix][ch]`, allocated by P4-05; supplied by a fake source here) consumed into `audio_output_data`. Done when: add/consume tests.
- [ ] **P2-13.2 Alignment** — `calc_min_ts`, `add_audio_buffering`, `set_fixed_audio_buffering`, `discard_if_stopped`, `ignore_audio`. Done when: timestamp cases equal to C.
- [ ] **P2-13.3 Mute/monitoring** — `mix_audio` is summation only; mute (→ vol 0 via `get_source_volume`) and per-mix volume land source-side in `apply_audio_volume` (P2-16); `should_silence_monitored_source` dedup. Done when: golden.
- [ ] **P2-13.4 Missing data** — `source->audio_pending`, `audio_ts`, `audio_buffering` handling. Done when: gap test.
- [ ] **P2-13.5 Golden** — 8 sources × 6 mixes × 10 s PCM, hash compare.

### P2-14 Fader and meter
- [ ] **P2-14.1 Curves** — `obs-audio-controls.c`: cubic, IEC 60268-18, log; `obs_db_to_mul`. Done when: 1001-point golden.
- [ ] **P2-14.2 Volmeter peak** — `obs_volmeter_create/attach_source/add_callback`, `obs_volmeter_set_peak_meter_type`. Done when: sine input yields expected dB.
- [ ] **P2-14.3 True peak** — `get_true_peak` Whittaker-Shannon oversampling (C comments disagree 4x vs 5x; reproduce the coefficients exactly). Done when: ITU test signal reading.
- [ ] **P2-14.4 Golden**

### P2-15 Audio monitoring
- [ ] **P2-15.1 Trait** — `MonitorBackend` from the C contract: `obs_audio_monitoring_available`/`obs_enum_audio_monitoring_devices` (public, `obs.h`), `audio_monitor_create/reset/destroy` (`obs-internal.h`), `devices_match` (each backend). Done when: documented.
- [ ] **P2-15.2 PulseAudio** — `libobs/audio-monitoring/pulse/`. Done when: monitor plays a test tone.
- [ ] **P2-15.3 WASAPI** — `libobs/audio-monitoring/win32/`. Done when: same on Windows.
- [ ] **P2-15.4 CoreAudio + null** — `osx/`, `null/`. Done when: compile and play on macOS.
- [ ] **P2-15.5 Device switch** — manual test per OS.

### P2-16 Audio volume/pan/mono kernels
- [ ] **P2-16.1 Kernels** — pure ports of `apply_audio_volume`/`multiply_output_audio` (per-mix gain), `process_audio_balancing` (pan matrix), `downmix_to_mono_planar` from `obs-source.c` (P4-05 owns the file; fake buffers here). Done when: golden on stereo and 5.1 buffers.
- [ ] **P2-16.2 Benchmark**

## D. Bitstream parsers

### P2-17 NAL / start-code
- [ ] **P2-17.1 Scanner** — `libobs/obs-nal.c`: `obs_nal_find_startcode`. Done when: vectors.
- [ ] **P2-17.2 Emulation-prevention edges** — the C scanner is naive: it does **not** skip `00 00 03 xx` sequences, so vectors must pin its behaviour with EPB bytes present (EPB stripping is the SPS path, P2-18). Done when: vectors.
- [ ] **P2-17.3 Fuzz** target.

### P2-18 AVC
- [ ] **P2-18.1 Keyframe/NAL types** — `obs_avc_keyframe`. Done when: samples.
- [ ] **P2-18.2 SPS/PPS** — `obs_parse_avc_header`, `obs_extract_avc_headers` (`libobs/obs-avc.h`). Done when: headers equal C on an x264 stream.
- [ ] **P2-18.3 Conversion** — Annex-B -> AVCC with refcounts (`obs_parse_avc_packet`, `obs_parse_avc_packet_priority`; C has no reverse direction). Done when: output bytes equal C on sample streams.
- [ ] **P2-18.4 Fuzz + goldens**

### P2-19 HEVC
- [ ] **P2-19.1 Headers** — VPS/SPS/PPS in `obs-hevc.c`. Done when: equal on a sample.
- [ ] **P2-19.2 Keyframe** detection (IRAP). Done when: samples.
- [ ] **P2-19.3 Goldens + fuzz**

### P2-20 AV1
- [ ] **P2-20.1 OBU parser** — `obs-av1.c` `parse_obu_header`, `obs_av1_keyframe` (frame-type bits). Done when: header types parsed.
- [ ] **P2-20.2 Headers** — `obs_extract_av1_headers` (sequence header + metadata OBUs), `metadata_obu` / `metadata_obu_itu_t35`. Done when: sample equal.
- [ ] **P2-20.3 Goldens + fuzz**

### P2-21 Caption support
- [ ] **P2-21.1 FFI wrapper** — per D-18 keep `deps/libcaption` as FFI (no port). Done when: crate wraps `caption_*`/`sei_*` and CEA-608/708 decode equals C.
- [ ] **P2-21.2 Encode path** — `obs-output.c` caption queue (`obs_output_output_caption_text1/2`, `obs_output_caption`). Done when: CEA-608 bytes equal.
- [ ] **P2-21.3 SEI injection** — `add_caption`: golden SEI bytes in H.264/HEVC, ITU-T T35 metadata OBU for AV1.

## E. Packet queues and delay

### P2-22 Encoder packet queue
- [ ] **P2-22.1 Packet type** — `encoder_packet` with refs (`obs_encoder_packet_ref/release`). Done when: ABI layout test.
- [ ] **P2-22.2 Timestamps** — pause offsets: `pause_data` (`ts_start`/`ts_end`/`ts_offset`), `video_pause_check_internal`, `pause_reset`, `sys_dts_usec` rebasing; `encoder->paused` is set by the output (P4-23) — fake input here. Done when: pause/resume trace equal.
- [ ] **P2-22.3 Queue tests**

### P2-23 Output delay buffer
- [ ] **P2-23.1 Queue** — `libobs/obs-output-delay.c`. Done when: delay of N seconds measured.
- [ ] **P2-23.2 Reconnect** — `OBS_OUTPUT_DELAY_PRESERVE`. Done when: test.
- [ ] **P2-23.3 Smoke** — 20 s delay recording.

### P2-24 Interleaving
- [ ] **P2-24.1 Interleave/send** — `interleave_packets`, `send_interleaved`, `find_first_packet_type_idx`, `apply_ept_offsets` (extended packet times). Done when: trace equal.
- [ ] **P2-24.2 Congestion** — drop paths `discard_to_idx`, `discard_unused_audio_packets`, `prune_premature_packets`/`prune_interleaved_packets`; backpressure `calculate_batch_size` -> `interleaver_max_batch_size`. Done when: scripted congestion test.
- [ ] **P2-24.3 Golden traces**

## F. Integration

### P2-25 Wire into libobs
- [ ] **P2-25.1 CMake switch** per crate. Done when: ON/OFF builds.
- [ ] **P2-25.2 Fallback** C path remains. Done when: CI runs both.
- [ ] **P2-25.3 CI matrix**

### P2-26 Soak test
- [ ] **P2-26.1 Script** — 24 h headless record (1080p60, 6 audio mixes, x264). Done when: runs unattended.
- [ ] **P2-26.2 Metrics** — CPU, RSS, dropped frames every minute to CSV.
- [ ] **P2-26.3 Compare** to C: RSS growth < 1%/h, dropped frames not worse.

### P2-27 Latency
- [ ] **P2-27.1 Signal** — flash+beep source (`test/test-input/sync-pair-*.c` exists for this).
- [ ] **P2-27.2 Measure** A/V offset in recordings of both builds.
- [ ] **P2-27.3 Gate** — delta = 0 ms at defaults.

### P2-28 Phase 2 review
- [ ] **P2-28.1 Review** benchmarks, fuzz corpus, soak.
- [ ] **P2-28.2 Tick** parent tasks.

### P2-29 `media-remux` (added)
- [ ] **P2-29.1 Port** — `libobs/media-io/media-remux.c`: open input, copy streams, progress callback. Done when: output equals C by `ffprobe` JSON.
- [ ] **P2-29.2 Frontend hook** — used by remux dialog (P6-28). Done when: API doc.
