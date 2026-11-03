# Phase 5: Plugins

Goal: one crate per in-tree plugin under `crates/obs-modules/<name>`, ported to the native Rust plugin API (`obs-plugin-api`) and also buildable as a C-ABI `cdylib` module so the old frontend and third-party hosts still load them.

Every plugin task has the same shape. **Common checklist (applies to each P5-xx task)**:

1. Inventory: list registered sources/filters/encoders/outputs/services (`obs_register_*` calls) and the settings keys they read/write.
2. Settings compatibility test: load old settings JSON (from the P0-18 corpus) and verify identical behaviour.
3. Locale: reuse `data/locale/*.ini`, same string IDs.
4. Port with a golden or behavioural test (frame hash, packet bytes, message trace).
5. Build as `cdylib` with the same module name, so the installer layout does not change.
6. Remove the C plugin from CMake in the same PR; update the README.
7. Manual smoke test on each supported OS and note it in `docs/tests/`.

Order: A (pure logic) -> B (network/mux) -> C (platform capture) -> D (hardware encoders) -> E (vendor SDK) -> F (browser). Within a group tasks are independent and can run in parallel. Exception: start P5-46.1–.5 (native SDK + obs-transitions pilot) early, right after the foundation, because the pilot proves the SDK the remaining ports use; P5-47…P5-50 stay the wrap-up.

## A. Pure logic (start here; lowest risk)

- [ ] **P5-01 `obs-filters`** (L): colour correction, chroma/colour key, LUT, sharpen, scroll, mask, crop, render delay, scale/aspect, noise gate/suppress (RNNoise via `nnnoiseless`, speex), compressor, limiter, expander, gain, invert polarity, async delay, EQ, HDR tonemap / SDR-on-HDR. Source: `plugins/obs-filters/*.c`, `data/*.effect`. Done when: audio filters match golden PCM within 1e-4; video filters match PNG goldens.
- [ ] **P5-02 `obs-transitions`** (M): cut, fade, swipe, slide, stinger, fade-to-colour, luma wipe. Golden: 30 frames per transition. Done when: goldens for all transitions match C.
- [ ] **P5-03 `image-source`** (M): image source, slideshow, colour source, `color-source.c`, `obs-slideshow.c`. Includes animated GIF (libobs `libnsgif`) and FFmpeg-decoded stills (WebP etc.). Done when: image/GIF/slideshow goldens match C.
- [ ] **P5-04 `obs-text`** (M): GDI+ text source (Windows only), port to DirectWrite (`windows` crate). Golden screenshots of fonts/outlines. Done when: font/outline screenshot goldens match GDI+.
- [ ] **P5-05 `text-freetype2`** (M): FreeType text, read-from-file, custom effects. Use `freetype-rs` or `cosmic-text`/`swash`; check glyph metrics vs golden. Done when: glyph metrics equal C on each OS.
- [ ] **P5-06 `rtmp-services`** (M): `services.json` loading, per-service settings, `rtmp-common`, `rtmp-custom`, JSON schema validation, service list update from the web (`json-file`, package update), enhanced broadcasting/multitrack config. Done when: current services.json loads with identical per-service settings.
- [ ] **P5-07 `obs-x264`** (M): x264 encoder via `x264` crate bindings; settings parse (`obs-x264.c`), CRF/CBR/VBR/ABR, custom `x264opts` strings. Done when: bitstream is byte-identical for fixed input and settings.
- [ ] **P5-08 `obs-libfdk`** (S): AAC via libfdk (kept as FFI), optional cargo feature. Done when: AAC output decodes.
- [ ] **P5-09 `coreaudio-encoder`** (M): AAC encoder via `AudioToolbox` (`objc2`/`coreaudio-sys` on macOS; Windows via `windows-imports.h`). Done when: AAC output decodes on macOS CI.
- [ ] **P5-10 `nv-filters`** (L): NVIDIA Broadcast/AR SDK filters (noise suppress, room echo removal, face tracking/greenscreen) - FFI around the vendor SDK; Windows only. Task includes loading the SDK dynamically. Done when: filters run on RTX and a missing SDK is handled.

## B. Network and muxing

- [ ] **P5-11 `obs-ffmpeg` encoders (software)** (L): FFmpeg audio encoders, `obs-ffmpeg-video-encoders`, `obs-ffmpeg-av1`, `obs-ffmpeg-openh264`. Use `ffmpeg-next`; verify packet bytes vs C for the same settings. Done when: packet bytes equal C.
- [ ] **P5-12 `obs-ffmpeg` hardware paths (VAAPI)** (L): `obs-ffmpeg-vaapi.c` (Linux). Done when: encodes play back on Intel and AMD.
- [ ] **P5-13 `obs-ffmpeg` NVENC fallback** (S): `obs-ffmpeg-nvenc.c` (legacy FFmpeg NVENC path). Done when: encodes, or the deprecation ADR (P5-13.4) is merged after P5-39.
- [ ] **P5-14 `obs-ffmpeg-mux` helper process** (L): `plugins/obs-ffmpeg/ffmpeg-mux/` out-of-process muxer: stdin protocol, MKV/MP4/fMP4/MOV/HLS, chapters, replay buffer, split file. Keep protocol; add fuzz tests for the pipe reader. Done when: muxed files ffprobe-equal to C and the pipe reader is fuzzed.
- [ ] **P5-15 `obs-ffmpeg` outputs** (L): `obs-ffmpeg-output.c`, `obs-ffmpeg-mpegts.c` (SRT, RIST via `obs-ffmpeg-srt.h`, `obs-ffmpeg-rist.h`), `obs-ffmpeg-hls-mux.c`, `obs-ffmpeg-url.h`. Loopback tests with `ffplay` or `srt-live-transmit`. Done when: loopback tests pass.
- [ ] **P5-16 `obs-ffmpeg` media source** (L): `obs-ffmpeg-source.c`, `media-playback/` decoder (A/V sync, looping, hw decode, seek, buffering; `shared/media-playback`: `media.c`, `decode.c`, `cache.c`). Done when: A/V drift < 1 frame over a 2 h file.
- [ ] **P5-17 `obs-outputs` RTMP/FLV** (XL; split: FLV muxer, RTMP client (`librtmp` replacement, e.g. `rml_rtmp` or in-tree), enhanced-RTMP HEVC/AV1, multitrack, congestion control `dbr`, socket tuning): `rtmp-stream.c`, `flv-mux.c`, `flv-output.c`, `librtmp/`, `net-if.c`. Done when: RTMP/RTMPS and enhanced-RTMP publish verified against nginx-rtmp and a mock server; TLS via `rustls`.
- [ ] **P5-18 `obs-outputs` FTL, null, mp4 output** (M): `ftl-stream.c`, `null-output.c`, `mp4-output.c`, `mp4-mux.c`. Golden MP4 box dumps (`mp4dump`). Done when: box dumps equal C and the FTL decision (P5-18.3) is recorded.
- [ ] **P5-19 `obs-webrtc`** (L): WHIP output (`whip-output.cpp`, `whip-utils.h`), libdatachannel -> `webrtc-rs` or keep libdatachannel FFI; test against a WHIP server (e.g. mediamtx). Done when: mediamtx and one hosted service receive the WHIP stream.
- [ ] **P5-20 `obs-websocket`** (XL; git submodule, tag 5.7.6 — initialised in this checkout, fresh clones still need `git submodule update --init`; 16.9k LOC): split: server/transport (`src/websocketserver/`: JSON+MsgPack subprotocols, `tokio-tungstenite`), auth (SHA256+Base64 challenge, `src/utils/Crypto.cpp`), request handlers by category (`src/requesthandler/`: general, config, sources, scenes, inputs, transitions, filters, scene items, outputs, stream, record, media inputs, ui, canvases, batch), event dispatch (`src/eventhandler/`: 60 events, subscription bitmask), vendor API (`lib/obs-websocket-api.h` + `src/WebSocketApi.cpp`), config + CLI args (`src/Config.cpp`). Preserve the 5.x protocol (rpcVersion 1) exactly; there is **no in-tree protocol test suite at 5.7.6**, so conformance runs against `docs/generated/protocol.json` (147 requests / 60 events / 7 enums) and the `obs-websocket-js` client. Done when: protocol.json-driven harness and `obs-websocket-js` pass.
- [ ] **P5-21 `obs-vst`** (L): VST2 host (Windows/macOS/Linux): dynamic loading, editor window embedding. Hard to test; tie to a free VST plugin; consider keeping C++. Done when: a free VST plugin processes audio and the editor opens.
- [ ] **P5-22 `vlc-video`** (M): VLC source via `libvlc` FFI; playlist handling; optional. Done when: a playlist plays.

## C. Platform capture and audio

Linux:
- [ ] **P5-23 `linux-pipewire`** (XL): screen capture portal (`xdg-desktop-portal` via `ashpd`), camera portal, audio capture; DMA-BUF import (depends on P3-18). Done when: screen/camera/audio capture works on GNOME, KDE and wlroots.
- [ ] **P5-24 `linux-capture`** (L): XSHM, XComposite window capture, X11 cursor; `x11rb` crate. Done when: monitor, window and cursor capture work.
- [ ] **P5-25 `linux-v4l2`** (M): V4L2 devices via `v4l` crate, controls, format enumeration, hotplug (udev). Done when: device lists match v4l2-ctl and hotplug works.
- [ ] **P5-26 `linux-pulseaudio`** (M): `libpulse-binding`; input/output capture, monitoring, device hot-plug. Done when: capture and device hot-plug work.
- [ ] **P5-27 `linux-alsa`** (S): `alsa` crate. Done when: capture works.
- [ ] **P5-28 `linux-jack`** (S): `jack` crate. Done when: capture works.
- [ ] **P5-29 `oss-audio`, `sndio`** (S each): BSD audio backends. Done when: FreeBSD/OpenBSD smoke runs recorded in `docs/tests/`.

Windows:
- [ ] **P5-30 `win-wasapi`** (L): device and application (process loopback) audio capture, `windows` crate, device notification, exclusive/shared. Done when: latency within C's measurements.
- [ ] **P5-31 `win-capture`** (XL; split: monitor capture DXGI duplication, WGC capture (`libobs-winrt`), window capture BitBlt/WGC, game capture hook DLL + injector (`graphics-hook`, `get-graphics-offsets`, `inject-helper`, plus `shared/ipc-util` for the hook IPC pipe), cursor capture): hook DLL stays C/C++ at first (injected into games; ABI-sensitive); host side in Rust. Done when: D3D8/9/10/11/12, OpenGL and Vulkan samples capture (matrix in `docs/tests/`).
- [ ] **P5-32 `win-dshow`** (L): DirectShow capture (`deps/libdshowcapture` submodule, initialised in this checkout — public API `src/dshowcapture.hpp`, keep the ATL-free C++ library as FFI, D-19), virtual camera driver (`virtualcam-module`, `shared-memory-queue`). Port host; keep libdshowcapture FFI. Done when: webcam preview, MJPEG/H.264 decode and the virtual camera work as in C.
- [ ] **P5-33 `libobs-winrt`** (M): `winrt-capture.cpp`, `winrt-dispatch.cpp` -> `windows` crate (Windows.Graphics.Capture). Done when: WGC window capture works through win-capture.

macOS:
- [ ] **P5-34 `mac-capture`** (XL): ScreenCaptureKit (display, window, application), audio capture (`objc2` + `screencapturekit` crate), permissions, cursor. Done when: display/window/app and audio capture work and permissions are handled.
- [ ] **P5-35 `mac-avcapture`** (L): AVFoundation cameras, formats, presets, controls; keep the legacy module (`legacy/` → `mac-avcapture-legacy`) building. Done when: formats/controls equal C and the legacy module loads.
- [ ] **P5-36 `mac-syphon`** (M): Syphon client; IOSurface (depends on P3-20). Done when: a Syphon server is visible.
- [ ] **P5-37 `mac-virtualcam`** (L): the plugin ships **two** camera backends: the legacy CoreMediaIO DAL plug-in (`src/dal-plugin/`, 17 ObjC files, plus shared `src/common/MachProtocol.h`) and the new system extension (`src/camera-extension/` Swift + `src/obs-plugin/` host). Port the host side (sink stream, extension install flow, code-signing, entitlements); keep the DAL plug-in building until the extension flow is the default, then remove it (D-14). Done when: extension install flow tested and the DAL removal recorded in `retired.md`.
- [ ] **P5-38 `mac-videotoolbox`** (L): H.264/HEVC/ProRes hardware encoder; pixel buffer sharing with Metal (depends on P3-21). Done when: streams play and a ProRes file is valid.

## D. Hardware encoders

- [ ] **P5-39 `obs-nvenc`** (XL): NVENC H.264/HEVC/AV1 encoder, CUDA interop, D3D11/12 texture input (`nvenc-d3d11.c`, `nvenc-opengl.c`, `nvenc-cuda.c`), 10-bit/HDR, B-frames, lookahead, presets p1-p7, `obs-nvenc-test` helper, dynamic loading of `nvEncodeAPI`. Uses `nvidia-video-codec-sdk` bindings. Done when: option parity table committed and an HDR stream is valid.
- [ ] **P5-40 `obs-qsv11`** (L): Intel oneVPL/QSV H.264/HEVC/AV1 encoder, `obs-qsv-test` helper process. Done when: encodes from a GPU texture.
- [ ] **P5-41 AMF encoder** (L): `obs-ffmpeg` AMF integration and `obs-amf-test`. Done when: encodes.

## E. Vendor SDK cards and tools

- [ ] **P5-42 `decklink`, `decklink-captions`** (L): Blackmagic SDK (COM on Windows, C++ on Linux/macOS): keep the SDK glue as a thin C++ FFI crate; port the plugin logic (device discovery, input/output, format detection, captions) in Rust. Done when: capture + output verified on one card per OS. (`decklink-output-ui` is P6-40.)
- [ ] **P5-43 `aja`** (L): AJA NTV2 SDK; same strategy as DeckLink. Done when: signal on card. (`aja-output-ui` is P6-40.)
- [ ] **P5-44 `frontend-tools`** (XL; handled in Phase 6): auto scene switcher, captions (MS SAPI), output timer, scripts. Not part of this phase except where noted in Phase 6. Done when: the Phase 6 file owns every frontend-tools file.

## F. Browser

- [ ] **P5-45 `obs-browser`** (XL; git submodule, pinned 2.23.4-126-gd4db73e — initialised in this checkout, fresh clones still need `git submodule update --init`; 6.5k LOC): CEF (Chromium Embedded Framework, ≥95) is a C++ library; keep it, including the CEF process model (Windows `obs-browser-page` page process; macOS `OBS Helper` ×4 bundles with entitlements). Port the surrounding plugin — CEF glue (`browser-app.cpp` registers `window.obsstudio` + `ExecuteJSFunction`, `browser-client.cpp`, `browser-scheme.cpp` local-file scheme, `browser-dummy-client.cpp` fallback), browser source (`is_local_file`, `fps_custom`/`fps`, `shutdown` when not visible, `restart_when_active`, `css`, `reroute_audio`, `webpage_control_level`), panel/dock (`panel/`, Qt), cookie manager — to Rust via a thin C++ `cef-bridge` crate; the module also links the obs-websocket vendor API. Done when: `https://obsproject.com`, a Twitch chat dock and a WebGL overlay render correctly on each OS; page-visibility and shutdown flags behave as before.

## G. Phase wrap-up

- [ ] **P5-46 Native plugin SDK `obs-plugin-api`** (L): `trait Source/Filter/Encoder/Output/Service`, `#[obs_module]` proc-macro, safe settings and properties builders, docs and a `cargo generate` template. All ported plugins use it. Done when: the obs-transitions pilot (P5-46.5) builds on the SDK with goldens unchanged.
- [ ] **P5-47 Plugin loader supports both** (M): load Rust-native modules (static/dynamic) and C-ABI modules in one list. Done when: the mixed Rust+C registry test (P5-47.3) passes.
- [ ] **P5-48 Installer layout and module list** (S): confirm file names and per-OS module/data install destinations unchanged (Windows: `core/<name>.dll` + `core/<name>/data`; Linux: `lib/obs-modules/core/<name>.so` + `share/obs/obs-modules/core/<name>`; macOS: bundle layout per `cmake/macos/`); update `cmake/` install rules and Flatpak manifest. Done when: installed trees match per OS.
- [ ] **P5-49 Full-feature regression run** (L): run the P0-16 smoke test + a manual matrix of the 41 plugins on 3 OSes; record in `docs/tests/`. Done when: smoke + 41-plugin matrix recorded.
- [ ] **P5-50 Phase 5 review** (S): tick this file; list any plugin deliberately kept as C/C++ with reason. Done when: this file is ticked and the FFI-shim table is committed.

Exit criteria: every plugin is either ported or explicitly kept as a thin FFI shim with an owner; the old C plugin directories are removed from the build.
