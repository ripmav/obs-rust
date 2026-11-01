# Phase 2: Media pipeline

Goal: port `libobs/media-io`, the audio mixing code, and the bitstream parsers. This is real-time code: no allocation or blocking locks on the hot path, and no regression in benchmarks.

Source map: `libobs/media-io/*`, `libobs/obs-audio.c`, `obs-audio-controls.c`, `audio-monitoring/`, `obs-av1.c`, `obs-avc.c`, `obs-hevc.c`, `obs-nal.c`, `obs-video.c` (timing part only), `obs-output-delay.c`, `obs-encoder.c` (packet queue part only).

## A. Formats and conversion

- [ ] **P2-01 Format enums and helpers** (S): `video-io.h` / `audio-io.h` enums (`video_format`, `audio_format`, `speaker_layout`, `video_colorspace`, `video_range_type`), helper functions (`get_audio_bytes_per_channel`, `is_audio_planar`, `get_audio_channels`). `#[repr(C)]`, exhaustive match tests.
- [ ] **P2-02 Colour matrices** (M): `video-io.c` colour matrix tables (BT.601, 709, 2020, PQ/HLG). Golden: every matrix times range combination.
- [ ] **P2-03 Pixel format conversion `video-scaler` and `video-frame`** (L): `video-frame.c`, `video-scaler-ffmpeg.c`. Wrap swscale initially (FFI) behind a trait so a native path can replace it later. `video_frame_copy`, `video_frame_init`.
- [ ] **P2-04 SIMD pixel converters** (L): `format-conversion.c` (NV12/I420/I444/UYVY/YUY2 <-> RGB) in `std::simd`. Tolerance +/-1 per channel vs C; benchmark >= C.
- [ ] **P2-05 HDR helpers** (M): `graphics/srgb.h`, PQ/HLG tonemap constants used by conversion; golden for 256-point curves.
- [ ] **P2-06 Audio resampler** (M): `audio-resampler-ffmpeg.c`. Wrapper around swresample behind a trait; test channel-layout up/down-mix tables.
- [ ] **P2-07 Audio format conversion** (S): planar/interleaved, int16/int32/float, in-place helpers.

## B. IO threads

- [ ] **P2-08 `audio-io` core** (L): `audio-io.c`: audio thread, connected inputs, mixes (6 tracks), `audio_output_connect`, `audio_output_disconnect`, buffering, timestamp handling, `audio_output_get_block_size`. No allocation on the thread; ring buffers preallocated.
- [ ] **P2-09 `video-io` core** (L): `video-io.c`: video thread with cached frame ring, frame skipping counters, `video_output_connect/disconnect`, `video_output_lock_frame/unlock_frame`, frame-rate divisor, GPU encoding flag. Done when: dropped-frame accounting matches C in a 10 min simulated run.
- [ ] **P2-10 Timing and clock** (M): `obs-video.c` frame timing loop (`os_sleepto_ns`), `obs->video.video_time`, `video_output_get_frame_time`, clock drift handling. Test with a fake clock.
- [ ] **P2-11 Lock-free queues** (M): replace `deque + mutex` pairs (`libobs/util/deque.h`) on the hot path with SPSC rings (`rtrb` or hand-rolled). `loom` model checks.
- [ ] **P2-12 Real-time audit** (S): check there is no `malloc`, mutex or syscall in audio callbacks, via `assert_no_alloc` in tests.

## C. Audio mixing and controls

- [ ] **P2-13 `obs-audio.c` mixer** (L): per-source buffers, timestamp alignment (`audio_buf_pos`), ducking of missed data, mixing `mix` / `obs_source_audio_pending`, mute, mono, monitoring flags, sync offset, `audio_buffering`. Golden: 8 sources, 6 mixes, 10 s of PCM.
- [ ] **P2-14 Fader and meter** (M): `obs-audio-controls.c`: dB<->deflection curves (cubic, IEC 60268-18, log), volmeter peak/magnitude/true-peak (4x oversample), callbacks. Golden: known sine inputs.
- [ ] **P2-15 Audio monitoring** (M): `audio-monitoring/` (PulseAudio, WASAPI, CoreAudio, null): trait + per-OS backends. Done when: monitoring device switch tested manually on each OS.
- [ ] **P2-16 Audio filters kernels** (S): kernels used by built-ins that live in libobs (not obs-filters): gain, volume apply (`obs_source_output_audio` volume path).

## D. Bitstream parsers

- [ ] **P2-17 NAL / start-code utilities** (S): `obs-nal.c/h`: Annex-B parsing, emulation prevention bytes.
- [ ] **P2-18 AVC** (M): `obs-avc.c/h`: keyframe detect, SPS/PPS extraction, `obs_avc_get_sps_pps`, `obs_extract_avc_headers`, annex-B <-> AVCC conversion. Fuzz with `cargo-fuzz`; goldens from real x264 output.
- [ ] **P2-19 HEVC** (M): `obs-hevc.c/h`: VPS/SPS/PPS, keyframe, headers. Fuzz + goldens (NVENC/QSV sample streams).
- [ ] **P2-20 AV1** (M): `obs-av1.c/h`: OBU parser, sequence header extraction, temporal delimiter. Fuzz + goldens.
- [ ] **P2-21 Caption support** (M): `deps/libcaption` (CEA-608/708) -> port or keep as FFI crate; `obs-output.c` caption injection into SEI. Decide, document, test with golden SEI bytes.

## E. Packet queues and delay

- [ ] **P2-22 Encoder packet queue** (M): `obs-encoder.c` interleaving, `obs_encoder_packet_create_instance`, ref-counted packets, timestamp rebasing, `pause` handling.
- [ ] **P2-23 Output delay buffer** (M): `obs-output-delay.c`. Done when: delay of 20 s with preserve-on-reconnect works in smoke test.
- [ ] **P2-24 Interleaving logic** (M): `obs-output.c` `interleave_packets`, `send_interleaved`, dropping on congestion (`drop_frames`, `dbr_*`). Golden queue traces.

## F. Integration

- [ ] **P2-25 Wire into `libobs`** (M): CMake switch for the media crates; C code kept behind `OBS_ENABLE_RUST=OFF`.
- [ ] **P2-26 Soak test** (M): 24 h headless recording, 1080p60 + 8 audio tracks, CPU, RSS, and dropped frames compared to C.
- [ ] **P2-27 Latency check** (S): measure audio-to-video offset change vs C (must be 0 ms for default settings).
- [ ] **P2-28 Phase 2 review** (S): benchmarks, soak, tick this file.

- [ ] **P2-29 `media-remux`** (S)
  - Source: `libobs/media-io/media-remux.c/.h`.
  - Do: port the remux helper (open input, copy streams, progress callback); used by the frontend remux dialog (P6-28).
  - Done when: output container passes `ffprobe` comparison with the C implementation.
  - Depends on: P2-03.

Exit criteria: video-io, audio-io, mixer, converters and parsers served by Rust; goldens and fuzzers green; soak test shows no regression.
