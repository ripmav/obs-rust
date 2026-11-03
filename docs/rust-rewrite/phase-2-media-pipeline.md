# Phase 2: Media pipeline

Goal: port `libobs/media-io`, the audio mixing code, and the bitstream parsers. This is real-time code: no allocation or blocking locks on the hot path, and no regression in benchmarks.

Source map: `libobs/media-io/*`, `libobs/obs-audio.c`, `obs-audio-controls.c`, `audio-monitoring/`, `obs-av1.c`, `obs-avc.c`, `obs-hevc.c`, `obs-nal.c`, `obs-video.c` (timing part only), `obs-output-delay.c`, `obs-encoder.c` (packet queue part only), `obs-output.c` (interleave/caption part only).

## A. Formats and conversion

- [ ] **P2-01 Format enums and helpers** (S): `video-io.h` / `audio-io.h` enums (`video_format`, `audio_format`, `speaker_layout`, `video_colorspace`, `video_range_type`), helper functions (`get_audio_bytes_per_channel`, `is_audio_planar`, `get_audio_channels`). `#[repr(C)]`, exhaustive match tests. Done when: enum values and helper tables equal the C headers in generated tests.
- [ ] **P2-02 Colour matrices** (M): `video-matrices.c` colour matrix tables (601/709/2020 primaries × full/limited × 9 bit-depth infos, plus the `video_format_get_parameters[_for_format]` public API). Done when: all 54 tables and the parameters API golden equal C for every `(colorspace, range, format)`.
- [ ] **P2-03 Pixel format conversion `video-scaler` and `video-frame`** (L): `video-frame.c`, `video-scaler-ffmpeg.c`. Wrap swscale initially (FFI) behind a trait so a native path can replace it later. `video_frame_copy`, `video_frame_init`. Done when: plane-size table and 8 conversion hashes equal C.
- [ ] **P2-04 SIMD pixel converters** (M): `format-conversion.c` packed<->planar YUV (UYVX <-> I420/NV12/I444/422: `compress_uyvx_to_i420/nv12`, `convert_uyvx_to_i444`, `decompress_420/nv12/422`) in `std::simd` (C is SSE via `sse-intrin.h`, SIMDE elsewhere). Exported but caller-free in 33.0.1 — port for S-013 conformance and the ABI. Done when: S-013 `mediaconv` conformance (27 scripts) green, byte-exact vs C, benchmark >= C.
- [ ] **P2-05 HDR helpers** (M): PQ (SMPTE ST 2084) / HLG (BT.2100) transfer functions and tonemap constants as a pure-Rust reference — C libobs has no CPU-side PQ/HLG maths (the transfer functions live in the `format_conversion.effect` shader and FFmpeg; `obs-source.c` `update_async_texrender` only computes the shader params); reuse the sRGB curve from P1-18 (`graphics/srgb.h`). Used by the Phase 5 HDR filters (P5-01.4). Done when: 256-point PQ/HLG curve goldens.
- [ ] **P2-06 Audio resampler** (M): `audio-resampler-ffmpeg.c`. Wrapper around swresample behind a trait; test channel-layout up/down-mix tables. Done when: output within 1 LSB of C for 5 conversions; mix tables golden.
- [ ] **P2-07 Audio maths** (S): `audio-math.h` (`mul_to_db`/`db_to_mul`, S-013-R10 incl. 0/-inf/NaN edges). Done when: table + edge-value golden (R10: 0, ±inf, NaN).

## B. IO threads

- [ ] **P2-08 `audio-io` core** (L): `audio-io.c`: audio thread, connected inputs, mixes (6 tracks), `audio_output_connect`, `audio_output_disconnect`, buffering, timestamp handling, `audio_output_get_block_size`. No allocation on the thread; ring buffers preallocated. Done when: 60 s simulated run produces the exact sample count and matches the C trace.
- [ ] **P2-09 `video-io` core** (L): `video-io.c`: video thread with cached frame ring, frame skipping counters, `video_output_connect/disconnect`, `video_output_lock_frame/unlock_frame`, frame-rate divisor, GPU encoding flag. Done when: dropped-frame accounting matches C in a 10 min simulated run.
- [ ] **P2-10 Timing and clock** (M): `obs-video.c` frame timing loop (`os_sleepto_ns`), `obs->video.video_time`, `video_output_get_frame_time`, clock drift handling. Test with a fake clock. Done when: 1000-frame fake-clock run equals the C trace.
- [ ] **P2-11 Lock-free queues** (M): replace `deque + mutex` pairs (`libobs/util/deque.h`) on the hot path with SPSC rings (`rtrb` or hand-rolled). `loom` model checks. Done when: queue choice documented in an ADR, loom models green, benchmark unchanged or better.
- [ ] **P2-12 Real-time audit** (S): check there is no `malloc`, mutex or syscall in audio callbacks, via `assert_no_alloc` in tests. Done when: the harness catches a deliberate alloc and the audio/video ticks pass under it.

## C. Audio mixing and controls

- [ ] **P2-13 `obs-audio.c` mixer** (L): `audio_callback` render order (per-video-mix channel sources + global audio sources), per-source mix buffers, buffering/alignment (`calc_min_ts`, `add_audio_buffering`, `set_fixed_audio_buffering`), `mix_audio` (per-mix summation into the output buffers — volume/mute/mixer mask are applied source-side, P2-16), monitoring dedup (`should_silence_monitored_source`), `discard_if_stopped`/`ignore_audio`. Done when: 8 sources × 6 mixes × 10 s of PCM golden hash-compares to C.
- [ ] **P2-14 Fader and meter** (L): `obs-audio-controls.c`: dB<->deflection curves (cubic, IEC 60268-18, log), volmeter sample-peak/true-peak (true peak via Whittaker-Shannon oversampling; reproduce the `get_true_peak` coefficients exactly — C comments disagree on 4x vs 5x, the goldens decide), fader/volmeter callbacks. Done when: 1001-point curve goldens and volmeter dB on known sine inputs equal C.
- [ ] **P2-15 Audio monitoring** (L): `audio-monitoring/` (PulseAudio, WASAPI, CoreAudio, null): trait + per-OS backends. Done when: monitoring device switch tested manually on each OS.
- [ ] **P2-16 Audio volume/pan/mono kernels** (S): the pure per-source audio kernels that `obs-source.c` (P4-05) applies — `apply_audio_volume`/`multiply_output_audio` (per-mix gain), `process_audio_balancing` (pan), `downmix_to_mono_planar`, and the mixer-mask buffer clearing in `process_audio_source_tick`. Port here as pure functions over fake buffers; the `obs_source_output_audio` integration lands with P4-05. Done when: golden on stereo and 5.1 buffers.

## D. Bitstream parsers

- [ ] **P2-17 NAL / start-code utilities** (S): `obs-nal.c/h`: `obs_nal_find_startcode` (word-aligned Annex-B start-code scan for `00 00 01`/`00 00 00 01`; the wrapper normalises a 4-byte start code to the 3-byte position — it does not skip emulation-prevention bytes), `OBS_NAL_PRIORITY` enum. Done when: scanner vectors equal C incl. 3/4-byte start-code edges.
- [ ] **P2-18 AVC** (M): `obs-avc.c/h`: `obs_avc_keyframe`, `obs_avc_find_startcode`, SPS/PPS extraction (`obs_parse_avc_header`, `obs_extract_avc_headers` — the SPS path strips emulation-prevention bytes before bit-parsing), Annex-B -> AVCC with refcounts (`obs_parse_avc_packet`, `obs_parse_avc_packet_priority`). Fuzz with `cargo-fuzz`; goldens from real x264 output. Done when: headers and AVCC bytes equal C on x264 samples; fuzz green.
- [ ] **P2-19 HEVC** (M): `obs-hevc.c/h`: `obs_hevc_keyframe`, VPS/SPS/PPS extraction (`obs_extract_hevc_headers`), `obs_parse_hevc_packet[_priority]`. Fuzz + goldens (NVENC/QSV sample streams). Done when: header extraction and keyframe results equal C on NVENC/QSV samples; fuzz green.
- [ ] **P2-20 AV1** (M): `obs-av1.c/h`: OBU parser (`parse_obu_header`), `obs_av1_keyframe` (frame-type bits in OBU frame headers), `obs_extract_av1_headers` (sequence header + metadata OBUs), `metadata_obu[_itu_t35]`. Fuzz + goldens. Done when: OBU parsing and header extraction equal C on samples; fuzz green.
- [ ] **P2-21 Caption support** (M): keep `deps/libcaption` (CEA-608/708) as an FFI crate per D-18; `obs-output.c` caption injection (H.264/HEVC SEI, AV1 ITU-T T35 metadata OBU — the file itself is P4-23, port the injection path here with fake outputs). Done when: FFI decode equals C and the SEI/OBU injection bytes golden.

## E. Packet queues and delay

- [ ] **P2-22 Encoder packet queue** (M): `obs-encoder.c` packet infrastructure: `obs_encoder_packet_create_instance`, ref-counted packets (`obs_encoder_packet_ref/release`), timestamp rebasing through `pause_data` (`sys_dts_usec += ts_offset`). Interleaving itself is `obs-output.c` (P2-24). Done when: ABI layout test and pause/resume traces equal C.
- [ ] **P2-23 Output delay buffer** (M): `obs-output-delay.c`. Done when: delay of 20 s with preserve-on-restart (`OBS_OUTPUT_DELAY_PRESERVE`) works in smoke test.
- [ ] **P2-24 Interleaving logic** (M): `obs-output.c` `interleave_packets`, `send_interleaved`, `find_first_packet_type_idx`, EPT offsets (`apply_ept_offsets`), dropping on congestion (`discard_to_idx`, `prune_interleaved_packets`, `calculate_batch_size`). Done when: queue traces and the scripted congestion test equal C.

## F. Integration

- [ ] **P2-25 Wire into `libobs`** (M): CMake switch for the media crates; C code kept behind `OBS_ENABLE_RUST=OFF`. Done when: ON/OFF builds both pass in CI.
- [ ] **P2-26 Soak test** (M): 24 h headless recording, 1080p60 + 6 audio mixes (`MAX_AUDIO_MIXES`), CPU, RSS, and dropped frames compared to C. Done when: 24 h run unattended, RSS growth < 1%/h, dropped frames not worse than C.
- [ ] **P2-27 Latency check** (S): measure audio-to-video offset change vs C (must be 0 ms for default settings). Done when: A/V offset measured at 0 ms for defaults on both builds.
- [ ] **P2-28 Phase 2 review** (S): benchmarks, soak, tick this file. Done when: review complete and this file ticked.

- [ ] **P2-29 `media-remux`** (S)
  - Source: `libobs/media-io/media-remux.c/.h`.
  - Do: port the remux helper (open input, copy streams, progress callback); used by the frontend remux dialog (P6-28).
  - Done when: output container passes `ffprobe` comparison with the C implementation.
  - Depends on: P2-03.

Exit criteria: video-io, audio-io, mixer, converters and parsers served by Rust; goldens and fuzzers green; soak test shows no regression.
