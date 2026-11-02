# Phase 5 subtasks

Parent file: [../phase-5-plugins.md](../phase-5-plugins.md). Each subtask has **Files**, **Steps**, **Done when**.

## Bookends (apply to every plugin task unless stated)

Every plugin task below starts with **S1/S2** and ends with **E1/E2**; they are not repeated in each task. Subtask IDs keep the numbering `.1 .2 …` with S1=`.1`, S2=`.2`, and E1/E2 as the last two.

- **S1 Inventory.** Files: `plugins/<name>/*.c|cpp`. Steps: grep `obs_register_(source|output|encoder|service)`, list ids and flags, list every `obs_data_get_*`/`set_default_*` key and each hotkey. Done when: table `docs/tests/plugin-<name>-inventory.md` is committed.
- **S2 Settings compatibility test.** Steps: pick sources/filters/outputs of this plugin from the P0-18 corpus, load settings with the Rust plugin, assert the same effective values (defaults included). Done when: test passes for every corpus file that uses the plugin; gaps listed.
- **E1 cdylib build.** Steps: crate `crate-type = ["cdylib"]` named exactly like the old module (`<name>.so|.dll|.dylib`), exports `obs_module_*`; install rules keep the per-OS destinations (Windows: `core/<name>.dll` + `core/<name>/data`; Linux: `lib/obs-modules/core/<name>.so` + `share/obs/obs-modules/core/<name>`). Done when: `ldd`/`dumpbin`/`otool` show the expected exports and loader picks it up.
- **E2 Cutover.** Steps: drop the plugin's `add_subdirectory` from `plugins/CMakeLists.txt`, delete the ported C sources in the same PR, update README, run the manual smoke test on each supported OS and record in `docs/tests/plugin-<name>.md`. Done when: CI green and smoke result recorded.

Shared CMake targets: `obs-ffmpeg` spans P5-11…P5-16 + P5-41 and `obs-outputs` spans P5-17 + P5-18. Those tasks port their own files, but E2 cutover happens once, in the last-finishing task (the C target is deleted only then).

Verified in this checkout: **`plugins/obs-websocket` and `plugins/obs-browser` are git submodules and are empty** (`.gitmodules`), as is `deps/libdshowcapture/src`. Run `git submodule update --init` first; the file lists for those two plugins below come from knowledge of the upstream repos and must be re-checked once present. There is no separate AMF plugin directory: AMF lives in `plugins/obs-ffmpeg` (`texture-amf.cpp`, `texture-amf-opts.hpp`, `obs-amf-test/`). Media playback code is shared: `shared/media-playback/media-playback/{media,decode,cache,media-playback}.c`.

## A. Pure logic

### P5-01 `obs-filters`
- [ ] **P5-01.1/.2** S1/S2
- [ ] **P5-01.3 Colour filters** — Files: `color-correction-filter.c`, `color-grade-filter.c` (LUT), `chroma-key-filter.c`, `color-key-filter.c`, `luma-key-filter.c`, `sharpness-filter.c`, `mask-filter.c`, `crop-filter.c`, `scale-filter.c`, `scroll-filter.c`, `gpu-delay.c`. Steps: port parameter setting for the effects in `data/*.effect`. Done when: each filter renders a PNG golden for 3 settings.
- [ ] **P5-01.4 HDR filters** — Files: `hdr-tonemap-filter.c`, `sdr-on-hdr-filter.c`. Steps: port constants (needs P2-05). Done when: golden on PQ test frame.
- [ ] **P5-01.5 Dynamics** — Files: `compressor-filter.c`, `limiter-filter.c`, `expander-filter.c`, `noise-gate-filter.c`, `gain-filter.c`. Steps: port envelope math exactly (attack/release in ms, sidechain). Done when: PCM golden within 1e-4.
- [ ] **P5-01.6 Utility audio** — Files: `invert-audio-polarity.c`, `async-delay-filter.c`, `eq-filter.c`. Steps: port EQ biquads. Done when: golden.
- [ ] **P5-01.7 Noise suppression** — Files: `noise-suppress-filter.c`, `rnnoise/` vendored. Steps: choose `nnnoiseless` crate vs keep vendored RNNoise FFI; Speex option via `speexdsp`. Done when: output within tolerance and CPU cost ≤ C.
- [ ] **P5-01.8/.9** E1/E2

### P5-02 `obs-transitions`
- [ ] **P5-02.1/.2** S1/S2
- [ ] **P5-02.3 Simple** — Files: `transition-cut.c`, `transition-fade.c`, `transition-fade-to-color.c`, `easings.h`. Done when: golden at t=0, .25, .5, .75, 1.
- [ ] **P5-02.4 Swipe/slide** — `transition-swipe.c`, `transition-slide.c` (direction, easing). Done when: golden per direction.
- [ ] **P5-02.5 Luma wipe** — `transition-luma-wipe.c` + `data/luma_wipes/*`. Done when: golden per wipe image.
- [ ] **P5-02.6 Stinger** — `transition-stinger.c`: media source, transition point (ms or frame), matte modes, audio fade styles. Done when: golden.
- [ ] **P5-02.7 Goldens** — 30 frames per transition.
- [ ] **P5-02.8/.9** E1/E2

### P5-03 `image-source`
- [ ] **P5-03.1/.2** S1/S2
- [ ] **P5-03.3 Image** — `image-source.c`: unload when hidden, file change detection, linear alpha. Done when: tests.
- [ ] **P5-03.4 Colour** — `color-source.c`. Done when: golden.
- [ ] **P5-03.5 Slideshow** — `obs-slideshow.c`, `obs-slideshow-mk2.c`: transition, playback modes (`always play`, `pause when hidden`, `stop when hidden`), shuffle, loop. Done when: tick-by-tick trace equal.
- [ ] **P5-03.6 Animations** — animated GIF via libobs `libnsgif`; other formats (WebP, …) are FFmpeg-decoded stills. Done when: GIF frame delays equal.
- [ ] **P5-03.7/.8** E1/E2

### P5-04 `obs-text` (Windows GDI+)
- [ ] **P5-04.1/.2** S1/S2
- [ ] **P5-04.3 Decision** — DirectWrite vs GDI+ via `windows` crate; stay pixel-compatible if possible. Done when: note.
- [ ] **P5-04.4 Layout** — Files: `gdiplus/obs-text.cpp`: font face/size/style, colours with gradient, outline, extents, chatlog mode, vertical align. Done when: screenshot golden of 10 configs.
- [ ] **P5-04.5 File mode** — read from file with change detection, UTF-8/UTF-16 BOM. Done when: tests.
- [ ] **P5-04.6 Goldens** — fonts/outlines.
- [ ] **P5-04.7/.8** E1/E2 (Windows only)

### P5-05 `text-freetype2`
- [ ] **P5-05.1/.2** S1/S2
- [ ] **P5-05.3 Backend** — `freetype` bindings vs `cosmic-text`/`swash`. Done when: spike result.
- [ ] **P5-05.4 Fonts** — Files: `find-font.c`, `find-font-unix.c`, `find-font-windows.c`, `find-font-cocoa.m`, `find-font-iconv.c`. Done when: font by family+style found on each OS.
- [ ] **P5-05.5 Layout** — Files: `text-freetype2.c`, `text-functionality.c`, `obs-convenience.c`: colours, outline, drop shadow, word wrap, custom width, vertical text. Done when: golden.
- [ ] **P5-05.6 Metric goldens** — glyph advances/positions for 10 font configs equal C. Done when: goldens committed.
- [ ] **P5-05.7/.8** E1/E2

### P5-06 `rtmp-services`
- [ ] **P5-06.1/.2** S1/S2
- [ ] **P5-06.3 JSON loading** — Files: `rtmp-common.c`, `rtmp-services-main.c`: `services.json` + `package.json`, schema checks, `format_version` (`rtmp-format-ver.h`). Done when: loads current file.
- [ ] **P5-06.4 Objects** — `rtmp-custom.c`, common services with server lists and stream-key rules. Done when: URL/key tests.
- [ ] **P5-06.5 Updater** — fetch + verify + atomic replace (uses `shared/file-updater`). Done when: local server test.
- [ ] **P5-06.6 Multitrack config** — `service-specific/` dir (per-service helpers such as Twitch ingest). Done when: tests with captured responses.
- [ ] **P5-06.7/.8** E1/E2

### P5-07 `obs-x264`
- [ ] **P5-07.1/.2** S1/S2
- [ ] **P5-07.3 Bindings** — FFI crate for libx264 (`x264_encoder_open`, `x264_param_*`). Done when: links on 3 OSes.
- [ ] **P5-07.4 Options** — Files: `obs-x264.c`: rate control (CBR/ABR/VBR/CRF/lossless), keyint, bframes, preset, tune, profile, threads, `x264opts` string parser. Done when: golden param dump for 20 settings.
- [ ] **P5-07.5 Encode loop** — NAL → packet, SEI, `get_extra_data`, pts/dts offsets. Done when: packet metadata equal.
- [ ] **P5-07.6 Bit-exactness** — same input + settings → identical bitstream. Done when: hash equal.
- [ ] **P5-07.7/.8** E1/E2 (`obs-x264-test.c` helper retained)

### P5-08 `obs-libfdk`
- [ ] **P5-08.1/.2** S1/S2
- [ ] **P5-08.3 Encoder** — Files: `obs-libfdk.c`: AAC-LC/HE modes, bitrate, afterburner. Done when: output decodes with ffprobe.
- [ ] **P5-08.4 Optional build** (cargo feature `fdk`).
- [ ] **P5-08.5/.6** E1/E2

### P5-09 `coreaudio-encoder`
- [ ] **P5-09.1/.2** S1/S2
- [ ] **P5-09.3 Converter** — Files: `encoder.cpp`, `windows-imports.h` (loads Apple libs on Windows too). Wrap `AudioConverter`. Done when: AAC out decodes.
- [ ] **P5-09.4 Tables** — bitrates/sample rates/modes (CBR, ABR, CVBR, VBR). Done when: option list equal.
- [ ] **P5-09.5 Compare** on macOS CI.
- [ ] **P5-09.6/.7** E1/E2

### P5-10 `nv-filters`
- [ ] **P5-10.1/.2** S1/S2
- [ ] **P5-10.3 SDK loading** — Files: `nvafx-load.h`, `nvvfx-load.h`, `nv_sdk_versions.h`: dynamic loading; versions. Done when: absence handled.
- [ ] **P5-10.4 Audio** — `nvidia-audiofx-filter.c` (denoise, dereverb). Done when: runs on RTX.
- [ ] **P5-10.5 Video** — `nvidia-videofx-filter.c` (greenscreen, background blur). Done when: runs on RTX.
- [ ] **P5-10.6 Hardware test** — Done when: runs on RTX.
- [ ] **P5-10.7/.8** E1/E2 (Windows only)

## B. Network and muxing

### P5-11 `obs-ffmpeg` software encoders
- [ ] **P5-11.1/.2** S1/S2
- [ ] **P5-11.3 Audio** — `obs-ffmpeg-audio-encoders.c` (AAC, Opus, PCM s16/s24/f32, ALAC, FLAC). Done when: ffprobe parameters equal.
- [ ] **P5-11.4 Video** — `obs-ffmpeg-video-encoders.c/.h`, `obs-ffmpeg-av1.c`, `obs-ffmpeg-openh264.c`; options via `obs-ffmpeg-formats.h`. Done when: packets equal.
- [ ] **P5-11.5 Compare** packet bytes with C.
- [ ] **P5-11.6/.7** E1/E2

### P5-12 VAAPI
- [ ] **P5-12.1/.2** S1/S2
- [ ] **P5-12.3 Probe** — `vaapi-utils.c/.h`, device selection (`/dev/dri/renderD*`), supported profiles/entrypoints. Done when: log matches C.
- [ ] **P5-12.4 Encode** — `obs-ffmpeg-vaapi.c` H.264/HEVC/AV1, CQP/CBR/VBR. Done when: playable stream.
- [ ] **P5-12.5 Test** Intel and AMD.
- [ ] **P5-12.6/.7** E1/E2

### P5-13 NVENC (FFmpeg legacy)
- [ ] **P5-13.1/.2** S1/S2
- [ ] **P5-13.3 Wrapper** — `obs-ffmpeg-nvenc.c`. Done when: encodes.
- [ ] **P5-13.4 Deprecate** after P5-39. Done when: removal ADR.
- [ ] **P5-13.5/.6** E1/E2

### P5-14 `ffmpeg-mux` helper
- [ ] **P5-14.1/.2** S1/S2
- [ ] **P5-14.3 Protocol** — `ffmpeg-mux/ffmpeg-mux.c/.h`: header struct, stdin packet framing, argument parsing. Done when: parser fuzzed.
- [ ] **P5-14.4 Containers** — MKV, MP4, fMP4, MOV, MPEG-TS, HLS. Done when: `ffprobe` of each equal.
- [ ] **P5-14.5 Features** — replay buffer, split files, chapters, `obs-ffmpeg-mux.c` host side. Done when: tests.
- [ ] **P5-14.6 Fuzz** pipe reader.
- [ ] **P5-14.7/.8** E1/E2

### P5-15 `obs-ffmpeg` outputs
- [ ] **P5-15.1/.2** S1/S2
- [ ] **P5-15.3 Generic** — `obs-ffmpeg-output.c/.h`. Done when: custom output works.
- [ ] **P5-15.4 MPEG-TS/SRT** — `obs-ffmpeg-mpegts.c`, `obs-ffmpeg-srt.h` (latency, passphrase, stream id). Done when: loopback with `srt-live-transmit`.
- [ ] **P5-15.5 RIST/HLS** — `obs-ffmpeg-rist.h`, `obs-ffmpeg-hls-mux.c`, `obs-ffmpeg-url.h`. Done when: HLS playlist valid.
- [ ] **P5-15.6 Loopback** tests.
- [ ] **P5-15.7/.8** E1/E2

### P5-16 `obs-ffmpeg` media source
- [ ] **P5-16.1/.2** S1/S2
- [ ] **P5-16.3 Playback core** — `shared/media-playback/media-playback/{media.c,decode.c}`: demux thread, decode thread, queues, frame timing. Done when: A/V sync tests.
- [ ] **P5-16.4 Cache/loop/seek** — `cache.c`, `media-playback.c`. Done when: seek accuracy test.
- [ ] **P5-16.5 HW decode** — selection list and fallback in `obs-ffmpeg-source.c`. Done when: log of chosen decoder equal.
- [ ] **P5-16.6 Drift** — 2 h file < 1 frame.
- [ ] **P5-16.7/.8** E1/E2 (also covers the `shared/media-playback` crate; only in-tree consumer is obs-ffmpeg)

### P5-17 `obs-outputs` RTMP/FLV
- [ ] **P5-17.1/.2** S1/S2
- [ ] **P5-17.3 FLV mux** — `flv-mux.c/.h`, `flv-output.c`, enhanced codecs in `rtmp-hevc.c/.h`, `rtmp-av1.c/.h`. Done when: tag bytes equal C.
- [ ] **P5-17.4 RTMP client** — vendored `librtmp/` replaced by Rust or kept as FFI; decision note; handshake, chunking, AMF0, `connect`/`publish`. Done when: publishes to nginx-rtmp.
- [ ] **P5-17.5 TLS/sockets** — `rtmp-stream.c`, `net-if.c/.h`, `rtmp-windows.c` (TCP_NODELAY, send buffer tuning, bind to interface, IPv4/IPv6). Done when: RTMPS works through `rustls`.
- [ ] **P5-17.6 Congestion/DBR** — drop priorities (PFRAMES/BFRAMES), dynamic bitrate. Done when: scripted congestion trace equal.
- [ ] **P5-17.7 Multitrack** — enhanced RTMP multitrack. Done when: two-track publish verified.
- [ ] **P5-17.8 Servers** — nginx-rtmp and a mock server in CI.
- [ ] **P5-17.9/.10** E1/E2

### P5-18 `obs-outputs` FTL, null, MP4
- [ ] **P5-18.1/.2** S1/S2
- [ ] **P5-18.3 FTL** — FTL code is a submodule/vendored in upstream builds; verify presence in this tree first (not in `ls`). Decision: drop if removed upstream. Done when: decision recorded.
- [ ] **P5-18.4 Null** — `null-output.c`. Done when: counts bytes.
- [ ] **P5-18.5 MP4** — `mp4-mux.c/.h`, `mp4-mux-internal.h`, `mp4-output.c`: fragmented/hybrid MP4, chapters, multi-track audio. Done when: `mp4dump` equal.
- [ ] **P5-18.6 Box goldens**
- [ ] **P5-18.7/.8** E1/E2

### P5-19 `obs-webrtc`
- [ ] **P5-19.1/.2** S1/S2
- [ ] **P5-19.3 Stack** — `obs-webrtc.cpp`, `whip-output.cpp/.h`, `whip-service.cpp/.h`, `whip-utils.h`: libdatachannel (C++) today; choose `webrtc-rs` vs FFI. Done when: spike.
- [ ] **P5-19.4 Signalling** — HTTP POST SDP offer, bearer token, ICE trickle/PATCH. Done when: mock server test.
- [ ] **P5-19.5 Media** — H.264/AV1/Opus tracks, RTCP feedback, packetization. Done when: receives on mediamtx.
- [ ] **P5-19.6 Interop** — mediamtx and one hosted WHIP service.
- [ ] **P5-19.7/.8** E1/E2

### P5-20 `obs-websocket` (submodule)
- [ ] **P5-20.1/.2** S1/S2 (after `git submodule update --init plugins/obs-websocket`)
- [ ] **P5-20.3 Transport** — HTTP upgrade, JSON and MsgPack subprotocols, `tokio-tungstenite`. Done when: official client connects.
- [ ] **P5-20.4 Auth** — Hello/Identify with SHA256 challenge. Done when: wrong password rejected with same code.
- [ ] **P5-20.5 Requests A** — General, Config, Sources, Scenes, Inputs. Done when: each request type has a test.
- [ ] **P5-20.6 Requests B** — Transitions, Filters, SceneItems, Outputs, Stream, Record. Done when: same.
- [ ] **P5-20.7 Requests C** — MediaInputs, Ui, Vendor, batch requests (`RequestBatch`, execution types). Done when: same.
- [ ] **P5-20.8 Events** — subscriptions bitmask, high-volume events. Done when: events delivered in order.
- [ ] **P5-20.9 Protocol suite** — run the upstream protocol tests and `obs-websocket-js`.
- [ ] **P5-20.10/.11** E1/E2 (also requires P6-36 frontend API)

### P5-21 `obs-vst`
- [ ] **P5-21.1/.2** S1/S2
- [ ] **P5-21.3 Scan/load** — `VSTPlugin.cpp`, `win/`, `mac/`, `linux/`, `headers/`, `vst_header/`. Done when: lists installed VST2 plugins.
- [ ] **P5-21.4 Audio** — process callback, channel mapping, latency. Done when: test with free plugin.
- [ ] **P5-21.5 Editor** — `EditorWidget.cpp` window embedding per OS (depends on P6 toolkit). Done when: editor opens.
- [ ] **P5-21.6 Test** — Done when: a free VST plugin processes audio.
- [ ] **P5-21.7/.8** E1/E2 (may remain C++; see P5-50)

### P5-22 `vlc-video`
- [ ] **P5-22.1/.2** S1/S2
- [ ] **P5-22.3 libvlc** — `vlc-video-source.c`, `vlc-video-plugin.c/.h`: dynamic load of libvlc, callbacks for video/audio. Done when: plays a file.
- [ ] **P5-22.4 Playlist** — shuffle/loop/behaviours. Done when: trace equal.
- [ ] **P5-22.5/.6** E1/E2

## C. Platform capture and audio

### P5-23 `linux-pipewire`
- [ ] **P5-23.1/.2** S1/S2
- [ ] **P5-23.3 Portal** — Files: `portal.c/.h`, `screencast-portal.c/.h`: `ashpd`/`zbus` session, `CreateSession`, `SelectSources`, `Start`, restore tokens. Done when: capture starts and re-starts without prompt.
- [ ] **P5-23.4 Streams** — `pipewire.c/.h`, `formats.c/.h`: format negotiation, modifiers, cursor metadata, damage. Done when: formats table equal.
- [ ] **P5-23.5 DMA-BUF** import (needs P3-18).
- [ ] **P5-23.6 Camera** — `camera-portal.c/.h`, `linux-pipewire.c`: device listing, controls. Done when: camera preview.
- [ ] **P5-23.7 Matrix** — GNOME, KDE, wlroots.
- [ ] **P5-23.8/.9** E1/E2

### P5-24 `linux-capture`
- [ ] **P5-24.1/.2** S1/S2
- [ ] **P5-24.3 XSHM** — `xshm-input.c`, `xhelpers.c/.h`. Done when: monitor capture.
- [ ] **P5-24.4 XComposite** — `xcomposite-input.c/.h`: window list, redirect, GLX/EGL pixmap. Done when: window capture.
- [ ] **P5-24.5 Cursor** — `xcursor-xcb.c/.h`. Done when: cursor visible.
- [ ] **P5-24.6/.7** E1/E2

### P5-25 `linux-v4l2`
- [ ] **P5-25.1/.2** S1/S2
- [ ] **P5-25.3 Enumeration** — `v4l2-helpers.c/.h`, formats, frame sizes/rates. Done when: lists equal `v4l2-ctl`.
- [ ] **P5-25.4 Capture** — `v4l2-input.c`, `v4l2-controls.c/.h`, `v4l2-decoder.c/.h` (MJPEG decode). Done when: preview.
- [ ] **P5-25.5 Hotplug** — `v4l2-udev.c/.h` via `udev` crate; `v4l2-output.c` (virtual cam output). Done when: plug/unplug test.
- [ ] **P5-25.6/.7** E1/E2

### P5-26 `linux-pulseaudio`
- [ ] **P5-26.1/.2** S1/S2
- [ ] **P5-26.3 Capture** — input/output (monitor) sources via `libpulse-binding`. Done when: levels visible.
- [ ] **P5-26.4 Devices** — hotplug, default devices. Done when: switching default works.
- [ ] **P5-26.5 Monitoring** — with P2-15 backend.
- [ ] **P5-26.6/.7** E1/E2

### P5-27 `linux-alsa`
- [ ] **P5-27.1/.2** S1/S2
- [ ] **P5-27.3 Input** — `alsa-input.c`, `linux-alsa.c`: `alsa` crate, device list, rate/format negotiation. Done when: capture works.
- [ ] **P5-27.4/.5** E1/E2

### P5-28 `linux-jack`
- [ ] **P5-28.1/.2** S1/S2
- [ ] **P5-28.3 Client** — `jack-input.c`, `jack-wrapper.c/.h`: ports, autoconnect, xruns. Done when: capture works.
- [ ] **P5-28.4/.5** E1/E2

### P5-29 `oss-audio`, `sndio`
- [ ] **P5-29.1/.2** S1/S2 (both plugins)
- [ ] **P5-29.3 OSS** — `oss-input.c`, `oss-audio.c`, `oss-platform.h.in`. Done when: FreeBSD run.
- [ ] **P5-29.4 sndio** — `sndio.c`, `sndio-input.c/.h`. Done when: OpenBSD run.
- [ ] **P5-29.5/.6** E1/E2 (added; E2 = BSD smoke run recorded in `docs/tests/`)

### P5-30 `win-wasapi`
- [ ] **P5-30.1/.2** S1/S2
- [ ] **P5-30.3 Devices** — `enum-wasapi.cpp/.hpp`, `wasapi-notify.cpp/.hpp` (IMMNotificationClient). Done when: hotplug.
- [ ] **P5-30.4 Capture** — `win-wasapi.cpp`: shared mode loop, buffer sizes, resample. Done when: levels and latency.
- [ ] **P5-30.5 Process loopback** — application audio capture (`ActivateAudioInterfaceAsync`), `plugin-main.cpp`. Done when: captures one app.
- [ ] **P5-30.6 Latency tests** — Done when: latency within C's measurements.
- [ ] **P5-30.7/.8** E1/E2

### P5-31 `win-capture`
- [ ] **P5-31.1/.2** S1/S2
- [ ] **P5-31.3 DXGI duplication** — `duplicator-monitor-capture.c`, `monitor-capture.c`. Done when: monitor capture incl. multi-GPU.
- [ ] **P5-31.4 WGC** — via the `libobs-winrt` module (existing C module during the port; its Rust port is P5-33). Done when: monitor and window capture.
- [ ] **P5-31.5 Window/BitBlt** — `window-capture.c`, `dc-capture.c/.h`, `app-helpers.c/.h`, `compat-helpers.c/.h` (compat list). Done when: window selection by title/class/exe.
- [ ] **P5-31.6 Game capture host** — `game-capture.c`, `game-capture-file-init.c`, `load-graphics-offsets.c`, `nt-stuff.c/.h` plus `shared/ipc-util` (the named-pipe IPC the hook process uses; the only consumers in the tree are game capture and its hook build, so it ports here): hook state machine, shared memory/events, anti-cheat compat. Done when: hooks a D3D11 sample.
- [ ] **P5-31.7 Hook DLL and helpers** — `graphics-hook/` (`d3d8/9/10/11/12`, `dxgi`, `gl`, `vulkan` capture), `inject-helper/inject-helper.c`, `get-graphics-offsets/`. Stay C/C++ initially; wrap build only. Done when: builds from Cargo.
- [ ] **P5-31.8 Cursor** — `cursor-capture.c/.h`. Done when: cursor visible.
- [ ] **P5-31.9 API matrix** — D3D8/9/10/11/12, OpenGL, Vulkan samples.
- [ ] **P5-31.10/.11** E1/E2

### P5-32 `win-dshow`
- [ ] **P5-32.1/.2** S1/S2 (requires `deps/libdshowcapture/src` submodule)
- [ ] **P5-32.3 Bindings** — `libdshowcapture` FFI. Done when: device enumeration.
- [ ] **P5-32.4 Source** — `win-dshow.cpp`, `ffmpeg-decode.c/.h`, `encode-dstr.hpp`, `tiny-nv12-scale.c/.h`: formats, MJPEG/H.264 decode, audio. Done when: webcam preview.
- [ ] **P5-32.5 Hardware encoder devices** — `win-dshow-encoder.cpp`. Done when: lists devices.
- [ ] **P5-32.6 Virtual cam** — `virtualcam.c`, `virtualcam-module/` (`virtualcam-filter.cpp`, `virtualcam-module.cpp`, install/uninstall scripts), `shared-memory-queue.c/.h`. Done when: visible in a video-call app.
- [ ] **P5-32.7/.8** E1/E2

### P5-33 `libobs-winrt`
- [ ] **P5-33.1 S1/S2 + Capture** — helper module: S1 = the exported `winrt_capture_*` C API (no `obs_register_*`; win-capture loads it by module name), S2 vacuous (no settings). Then `libobs-winrt/winrt-capture.cpp`: `GraphicsCaptureItem`, frame pool, `Direct3D11CaptureFramePool::CreateFreeThreaded`. Done when: window capture via WGC.
- [ ] **P5-33.2 Dispatcher** — `winrt-dispatch.cpp`. Done when: tests.
- [ ] **P5-33.3 Integration** with P5-31.4. Done when: WGC window capture through the win-capture module.
- [ ] **P5-33.4/.5** E1/E2 (added; module name stays `libobs-winrt`)

### P5-34 `mac-capture`
- [ ] **P5-34.1/.2** S1/S2
- [ ] **P5-34.3 Display** — `mac-display-capture.m`, `mac-sck-video-capture.m`, `mac-sck-common.m/.h`. Done when: display capture incl. HDR.
- [ ] **P5-34.4 Window/app** — `mac-window-capture.m`, `window-utils.m/.h`. Done when: window and application capture.
- [ ] **P5-34.5 Audio** — `mac-audio.c`, `audio-device-enum.c/.h`, `mac-sck-audio-capture.m`. Done when: device and app audio.
- [ ] **P5-34.6 Permissions/cursor** — screen-recording permission flow. Done when: denied state handled.
- [ ] **P5-34.7/.8** E1/E2

### P5-35 `mac-avcapture`
- [ ] **P5-35.1/.2** S1/S2
- [ ] **P5-35.3 Devices** — `OBSAVCapture.m/.h`, `OBSAVCapturePresetInfo.m/.h`, `AVCaptureDeviceFormat+OBSListable.*`, `plugin-properties.m/.h`. Done when: formats list equal.
- [ ] **P5-35.4 Controls + legacy** — `legacy/av-capture.mm` ships as a second module (`mac-avcapture-legacy`); keep it building alongside the port. Done when: formats/controls work and the legacy module loads.
- [ ] **P5-35.5/.6** E1/E2

### P5-36 `mac-syphon`
- [ ] **P5-36.1/.2** S1/S2
- [ ] **P5-36.3 Client** — `syphon.m`, `SyphonOBSClient.m/.h`, `plugin-main.c`; IOSurface import (P3-20). Done when: Syphon server visible.
- [ ] **P5-36.4/.5** E1/E2

### P5-37 `mac-virtualcam`
- [ ] **P5-37.1/.2** S1/S2
- [ ] **P5-37.3 Legacy DAL plug-in** — `src/dal-plugin/` (17 ObjC files) + `src/common/MachProtocol.h`: keep it building and tested against the C reference during the port; it is the fallback camera until the extension flow is the default. Per D-14, remove it in this subtask once the extension is the default, and record the removal in `retired.md`. Done when: DAL still green, then deleted with no regressions.
- [ ] **P5-37.4 System extension + host** — `src/camera-extension/` (Swift: `OBSCameraDeviceSource`, `OBSCameraProviderSource`, stream sink/source) and `src/obs-plugin/` (host: `OBSDALMachServer`, `plugin-main.mm`): Rust cannot be the extension host directly — define the Mach-bridge. Sink stream, `OSSystemExtensionRequest`. Done when: extension builds and the install flow is tested.
- [ ] **P5-37.5 Signing** — entitlements, provisioning. Done when: notarised build.
- [ ] **P5-37.6/.7** E1/E2

### P5-38 `mac-videotoolbox`
- [ ] **P5-38.1/.2** S1/S2
- [ ] **P5-38.3 Session** — `encoder.c`: `VTCompressionSession`, H.264/HEVC properties, profiles, B-frames. Done when: stream plays.
- [ ] **P5-38.4 ProRes/buffers** — ProRes 422/4444, pixel buffers from Metal (P3-21). Done when: ProRes file valid.
- [ ] **P5-38.5/.6** E1/E2

## D. Hardware encoders

### P5-39 `obs-nvenc`
- [ ] **P5-39.1/.2** S1/S2
- [ ] **P5-39.3 Loader** — `nvenc-helpers.c/.h`, `nvenc-internal.h`: dynamic `nvEncodeAPI`, capability queries, driver version checks. Done when: capability log equal.
- [ ] **P5-39.4 D3D** — `nvenc-d3d11.c`, `nvenc-compat.c`: texture registration, shared textures. Done when: encodes from GPU texture.
- [ ] **P5-39.5 CUDA/GL** — `nvenc-cuda.c`, `cuda-helpers.c/.h`, `nvenc-opengl.c`. Done when: Linux encode.
- [ ] **P5-39.6 H.264/HEVC** — `nvenc.c`, `nvenc-properties.c`, `nvenc-opts-parser.c`: presets p1–p7, tune, multipass, lookahead, psycho-AQ, B-ref, bitrate modes. Done when: option parity table.
- [ ] **P5-39.7 AV1/10-bit** — AV1, P010, HDR metadata. Done when: HDR stream valid.
- [ ] **P5-39.8 Test helper** — `obs-nvenc-test/` process that probes GPUs. Done when: ported.
- [ ] **P5-39.9/.10** E1/E2

### P5-40 `obs-qsv11`
- [ ] **P5-40.1/.2** S1/S2
- [ ] **P5-40.3 Bindings** — oneVPL FFI. Done when: init session.
- [ ] **P5-40.4 Encode** — `QSV_Encoder.cpp/.h`, `QSV_Encoder_Internal.cpp/.h`, `obs-qsv11.c`, `common_directx11.*`, `common_utils*.cpp`: H.264/HEVC/AV1, texture input. Done when: encode.
- [ ] **P5-40.5 Test helper** — `obs-qsv-test/`. Done when: ported.
- [ ] **P5-40.6/.7** E1/E2

### P5-41 AMF
- [ ] **P5-41.1/.2** S1/S2 (inside `plugins/obs-ffmpeg`)
- [ ] **P5-41.3 Encoder** — `texture-amf.cpp`, `texture-amf-opts.hpp`: AMF SDK via dynamic loading; H.264/HEVC/AV1 options. Done when: encode.
- [ ] **P5-41.4 Probe** — `obs-amf-test/`. Done when: ported.
- [ ] **P5-41.5/.6** E1/E2

## E. Vendor SDK cards and tools

### P5-42 DeckLink
- [ ] **P5-42.1 S1/S2 + FFI crate** — S1/S2 first (registers `decklink-input` source + `decklink_output` output; the `decklink-captions` module registers its own captions input). Then wrap Blackmagic SDK (COM on Windows via IDL, C++ interfaces on Linux/mac). Files: `plugins/decklink/{win|mac|linux}/`, `platform.hpp`. Done when: table committed, library loads without hardware.
- [ ] **P5-42.2 Discovery** — `decklink-device-discovery.*`, `decklink-devices.*`, `decklink-device-mode.*`. Done when: mode list equal.
- [ ] **P5-42.3 Input** — `DecklinkInput.*`, `decklink-source.cpp`, `audio-repack.*`. Done when: capture.
- [ ] **P5-42.4 Output/captions** — `DecklinkOutput.*`, `decklink-output.cpp`, `decklink-captions` plugin (`decklink-output-ui` is P6-40). Done when: output.
- [ ] **P5-42.5 Hardware test** — Done when: capture + output verified on one card per OS.
- [ ] **P5-42.6/.7** E1/E2 (added; D-10: SDK glue C++ stays, cutover removes only the ported logic)

### P5-43 AJA
- [ ] **P5-43.1 S1/S2 + FFI** — S1/S2 first (aja source + output; ids per the S1 table). NTV2 SDK. Files: `aja-card-manager.*`, `aja-common.*`, `aja-routing.*`. Done when: table committed, NTV2 session inits on a card.
- [ ] **P5-43.2 Capture/output** — `aja-source.*`, `aja-output.*`, `audio-repack.*`. Done when: signal.
- [ ] **P5-43.3 Presets/props** — `aja-presets.*`, `aja-props.*`, `aja-vpid-data.*`, `aja-widget-io.*`. Done when: properties parity with C.
- [ ] **P5-43.4 Hardware test**; `aja-output-ui` in P6-40. Done when: signal on card.
- [ ] **P5-43.5/.6** E1/E2 (added; D-10: SDK glue C++ stays, cutover removes only the ported logic)

### P5-44 `frontend-tools`
(Bookends n/a — ported in Phase 6, see the phase file.)
- [ ] **P5-44.1 Cross-reference** — scripts → P6-37; auto-scene-switcher → P6-38; captions/output-timer → P6-39. Done when: the Phase 6 file owns every file.
- [ ] **P5-44.2 Check** nothing in `plugins/frontend-tools/*` is unassigned. Done when: the coverage matrix maps all files to P6-xx.

## F. Browser

### P5-45 `obs-browser` (submodule)
- [ ] **P5-45.1/.2** S1/S2 (after submodule init)
- [ ] **P5-45.3 Bridge** — C ABI `cef-bridge` around CEF (C++): browser create/destroy, navigate, execute JS, paint callback with shared texture. Done when: header reviewed.
- [ ] **P5-45.4 Source** — lifecycle, settings (URL, size, FPS, CSS, shutdown when not visible, refresh when active, local file). Done when: tests.
- [ ] **P5-45.5 JS bindings** — `window.obsstudio` object and events (`obsSourceVisibleChanged`, `obsSourceActiveChanged`, `obsStreamingStarting`, …). Done when: page receives events.
- [ ] **P5-45.6 Docks/cookies** — browser docks (Qt-dependent), cookie manager. Done when: Twitch chat dock loads.
- [ ] **P5-45.7 Render tests** — static, WebGL, chat dock on each OS.
- [ ] **P5-45.8/.9** E1/E2

## G. Wrap-up

### P5-46 Native plugin SDK
- [ ] **P5-46.1 Traits** — `Source`, `Filter`, `Encoder`, `Output`, `Service`, `Transition` with default methods for optional callbacks. Done when: docs.
- [ ] **P5-46.2 Macro** — `#[obs_module(name, author, description, version)]` generating `obs_module_*` exports. Done when: `cargo expand` shows exports.
- [ ] **P5-46.3 Builders** — settings and properties builders over `obs-data`. Done when: example plugin compiled.
- [ ] **P5-46.4 Template** — `cargo generate` template + `docs/rust-rewrite/plugin-sdk.md`. Done when: a generated crate builds.
- [ ] **P5-46.5 Pilot** — migrate `obs-transitions` onto the SDK. Done when: golden unchanged.

### P5-47 Plugin loader supports both
- [ ] **P5-47.1 Unified list** — Rust-native and C modules in one registry. Done when: registry lists both kinds.
- [ ] **P5-47.2 Order/failures** — failure reporting via `obs_module_failure_info`. Done when: a failing C module reports as before.
- [ ] **P5-47.3 Mixed test** — one Rust and one C module together. Done when: both load, order preserved.

### P5-48 Installer layout
- [ ] **P5-48.1 Compare** installed trees (old/new) with a script. Done when: diff empty or approved.
- [ ] **P5-48.2 CMake/Cargo install rules**. Done when: installed trees match (P5-48.1).
- [ ] **P5-48.3 Flatpak manifest** (`build-aux/`). Done when: manifest paths match and the Flatpak build passes.

### P5-49 Full-feature regression run
- [ ] **P5-49.1 Automated** smoke test on 3 OSes. Done when: CI jobs green.
- [ ] **P5-49.2 Manual matrix** of all 41 plugins. Done when: matrix recorded in `docs/tests/`.
- [ ] **P5-49.3 Record** and open issues. Done when: `docs/tests/phase-5-regression.md` committed.

### P5-50 Phase 5 review
- [ ] **P5-50.1 FFI-shim list** with owner and reason. Done when: table committed (input to P7-03.1).
- [ ] **P5-50.2 Tick** parents. Done when: every P5-xx checkbox is ticked in the phase file.
