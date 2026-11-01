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

Order: A (pure logic) -> B (network/mux) -> C (platform capture) -> D (hardware encoders) -> E (vendor SDK) -> F (browser). Within a group tasks are independent and can run in parallel.

## A. Pure logic (start here; lowest risk)

- [ ] **P5-01 `obs-filters`** (L): colour correction, chroma/colour key, LUT, sharpen, scroll, mask, crop/pad, render delay, scale/aspect, noise gate/suppress (RNNoise via `nnnoiseless`, speex), compressor, limiter, expander, gain, invert polarity, upmix, async delay, EQ. Source: `plugins/obs-filters/*.c`, `data/*.effect`. Done when: audio filters match golden PCM within 1e-4; video filters match PNG goldens.
- [ ] **P5-02 `obs-transitions`** (M): cut, fade, swipe, slide, stinger, fade-to-colour, luma wipe. Golden: 30 frames per transition.
- [ ] **P5-03 `image-source`** (M): image source, slideshow, colour source, `color-source.c`, `obs-slideshow.c`. Includes animated GIF/APNG/WebP.
- [ ] **P5-04 `obs-text`** (M): GDI+ text source (Windows only), port to DirectWrite (`windows` crate). Golden screenshots of fonts/outlines.
- [ ] **P5-05 `text-freetype2`** (M): FreeType text, read-from-file, custom effects. Use `freetype-rs` or `cosmic-text`/`swash`; check glyph metrics vs golden.
- [ ] **P5-06 `rtmp-services`** (M): `services.json` loading, per-service settings, `rtmp-common`, `rtmp-custom`, JSON schema validation, service list update from the web (`json-file`, package update), enhanced broadcasting/multitrack config.
- [ ] **P5-07 `obs-x264`** (M): x264 encoder via `x264` crate bindings; settings parse (`obs-x264-options.c`), CRF/CBR/VBR/ABR, custom `x264opts` strings. Done when: bitstream is byte-identical for fixed input and settings.
- [ ] **P5-08 `obs-libfdk`** (S): AAC via libfdk (kept as FFI). Optional build.
- [ ] **P5-09 `coreaudio-encoder`** (M): macOS AAC encoder via `AudioToolbox` (`objc2`/`coreaudio-sys`). Test on macOS CI.
- [ ] **P5-10 `nv-filters`** (L): NVIDIA Broadcast/AR SDK filters (noise suppress, room echo removal, face tracking/greenscreen) - FFI around the vendor SDK; Windows only. Task includes loading the SDK dynamically.

## B. Network and muxing

- [ ] **P5-11 `obs-ffmpeg` encoders (software)** (L): FFmpeg audio encoders, `obs-ffmpeg-video-encoders`, `obs-ffmpeg-av1`, `obs-ffmpeg-openh264`. Use `ffmpeg-next`; verify packet bytes vs C for the same settings.
- [ ] **P5-12 `obs-ffmpeg` hardware paths (VAAPI)** (L): `obs-ffmpeg-vaapi.c` (Linux). Test on Intel and AMD.
- [ ] **P5-13 `obs-ffmpeg` NVENC fallback** (S): `obs-ffmpeg-nvenc.c` (legacy FFmpeg NVENC path).
- [ ] **P5-14 `obs-ffmpeg-mux` helper process** (L): `plugins/obs-ffmpeg/ffmpeg-mux/` out-of-process muxer: stdin protocol, MKV/MP4/fMP4/MOV/HLS, chapters, replay buffer, split file. Keep protocol; add fuzz tests for the pipe reader.
- [ ] **P5-15 `obs-ffmpeg` outputs** (L): `obs-ffmpeg-output.c`, `obs-ffmpeg-mpegts.c` (SRT, RIST via `obs-ffmpeg-srt.h`, `obs-ffmpeg-rist.h`), `obs-ffmpeg-hls-mux.c`, `obs-ffmpeg-url.h`. Loopback tests with `ffplay` or `srt-live-transmit`.
- [ ] **P5-16 `obs-ffmpeg` media source** (L): `obs-ffmpeg-source.c`, `media-playback/` decoder (A/V sync, looping, hw decode, seek, buffering, `mp-media`, `mp-decode`, `mp-cache`). Done when: A/V drift < 1 frame over a 2 h file.
- [ ] **P5-17 `obs-outputs` RTMP/FLV** (XL; split: FLV muxer, RTMP client (`librtmp` replacement, e.g. `rml_rtmp` or in-tree), enhanced-RTMP HEVC/AV1, multitrack, congestion control `dbr`, socket tuning): `rtmp-stream.c`, `flv-mux.c`, `flv-output.c`, `librtmp/`, `net-if.c`. Tested against nginx-rtmp and a mock server; TLS via `rustls`.
- [ ] **P5-18 `obs-outputs` FTL, null, mp4 output** (M): `ftl-stream.c`, `null-output.c`, `mp4-output.c`, `mp4-mux.c`. Golden MP4 box dumps (`mp4dump`).
- [ ] **P5-19 `obs-webrtc`** (L): WHIP output (`whip-output.cpp`, `whip-utils.h`), libdatachannel -> `webrtc-rs` or keep libdatachannel FFI; test against a WHIP server (e.g. mediamtx).
- [ ] **P5-20 `obs-websocket`** (XL; split: server/transport (`tokio-tungstenite`), auth (SHA256 challenge), request handlers by category (general, config, sources, scenes, inputs, transitions, filters, scene items, outputs, stream, record, media inputs, UI, ...), event dispatch, vendor API, CLI args): preserve protocol v5.x exactly; run the official obs-websocket protocol test-suite and `obs-websocket-js` client.
- [ ] **P5-21 `obs-vst`** (L): VST2 host (Windows/macOS/Linux): dynamic loading, editor window embedding. Hard to test; tie to a free VST plugin; consider keeping C++.
- [ ] **P5-22 `vlc-video`** (M): VLC source via `libvlc` FFI; playlist handling; optional.

## C. Platform capture and audio

Linux:
- [ ] **P5-23 `linux-pipewire`** (XL): screen capture portal (`xdg-desktop-portal` via `ashpd`), camera portal, audio capture; DMA-BUF import (depends on P3-18). Test on GNOME, KDE, wlroots.
- [ ] **P5-24 `linux-capture`** (L): XSHM, XComposite window capture, X11 cursor; `x11rb` crate.
- [ ] **P5-25 `linux-v4l2`** (M): V4L2 devices via `v4l` crate, controls, format enumeration, hotplug (udev).
- [ ] **P5-26 `linux-pulseaudio`** (M): `libpulse-binding`; input/output capture, monitoring, device hot-plug.
- [ ] **P5-27 `linux-alsa`** (S): `alsa` crate.
- [ ] **P5-28 `linux-jack`** (S): `jack` crate.
- [ ] **P5-29 `oss-audio`, `sndio`** (S each): BSD audio backends.

Windows:
- [ ] **P5-30 `win-wasapi`** (L): device and application (process loopback) audio capture, `windows` crate, device notification, exclusive/shared, latency tests.
- [ ] **P5-31 `win-capture`** (XL; split: monitor capture DXGI duplication, WGC capture (`libobs-winrt`), window capture BitBlt/WGC, game capture hook DLL + injector (`graphics-hook`, `get-graphics-offsets`, `inject-helper`), cursor capture): hook DLL stays C/C++ at first (injected into games; ABI-sensitive); host side in Rust. Test with D3D9/10/11/12/Vulkan/OpenGL samples.
- [ ] **P5-32 `win-dshow`** (L): DirectShow capture (`deps/libdshowcapture`), virtual camera driver (`virtualcam-module`, `shared-memory-queue`). Port host; keep libdshowcapture FFI.
- [ ] **P5-33 `libobs-winrt`** (M): `winrt-capture.cpp`, `winrt-dispatch.cpp` -> `windows` crate (Windows.Graphics.Capture).

macOS:
- [ ] **P5-34 `mac-capture`** (XL): ScreenCaptureKit (display, window, application), audio capture (`objc2` + `screencapturekit` crate), permissions, cursor.
- [ ] **P5-35 `mac-avcapture`** (L): AVFoundation cameras, formats, presets, controls, Continuity Camera.
- [ ] **P5-36 `mac-syphon`** (M): Syphon client; IOSurface (depends on P3-20).
- [ ] **P5-37 `mac-virtualcam`** (L): CoreMediaIO system extension (DAL plug-in removed): sink stream, extension install flow, code-signing, entitlements.
- [ ] **P5-38 `mac-videotoolbox`** (L): H.264/HEVC/ProRes hardware encoder; pixel buffer sharing with Metal (depends on P3-21).

## D. Hardware encoders

- [ ] **P5-39 `obs-nvenc`** (XL): NVENC H.264/HEVC/AV1 encoder, CUDA interop, D3D11/12 texture input (`nvenc-d3d11.c`, `nvenc-opengl.c`, `nvenc-cuda.c`), 10-bit/HDR, B-frames, lookahead, presets p1-p7, `obs-nvenc-test` helper, dynamic loading of `nvEncodeAPI`. Uses `nvidia-video-codec-sdk` bindings.
- [ ] **P5-40 `obs-qsv11`** (L): Intel oneVPL/QSV H.264/HEVC/AV1 encoder, `obs-qsv-test` helper process.
- [ ] **P5-41 AMF encoder** (L): `obs-ffmpeg` AMF integration and `obs-amf-test`.

## E. Vendor SDK cards and tools

- [ ] **P5-42 `decklink`, `decklink-captions`, `decklink-output-ui`** (L): Blackmagic SDK (COM on Windows, C++ on Linux/macOS): keep the SDK glue as a thin C++ FFI crate; port the plugin logic (device discovery, input/output, format detection, captions) in Rust.
- [ ] **P5-43 `aja`, `aja-output-ui`** (L): AJA NTV2 SDK; same strategy as DeckLink.
- [ ] **P5-44 `frontend-tools`** (XL; handled in Phase 6): auto scene switcher, captions (MS SAPI), output timer, scripts. Not part of this phase except where noted in Phase 6.

## F. Browser

- [ ] **P5-45 `obs-browser`** (XL): CEF (Chromium Embedded Framework) is a C++ library; keep it. Port the surrounding plugin (browser source, browser panel/dock, JS bindings `obsstudio` object, QuickJS/V8 bridge, cookie manager) to Rust via a thin C++ `cef-bridge` crate. Done when: `https://obsproject.com`, a Twitch chat dock and a WebGL overlay render correctly on each OS; page-visibility and shutdown flags behave as before.

## G. Phase wrap-up

- [ ] **P5-46 Native plugin SDK `obs-plugin-api`** (L): `trait Source/Filter/Encoder/Output/Service`, `#[obs_module]` proc-macro, safe settings and properties builders, docs and a `cargo generate` template. All ported plugins use it.
- [ ] **P5-47 Plugin loader supports both** (M): load Rust-native modules (static/dynamic) and C-ABI modules in one list.
- [ ] **P5-48 Installer layout and module list** (S): confirm file names, `obs-plugins/` layout, `data/obs-plugins/<name>/` paths unchanged; update `cmake/` install rules and Flatpak manifest.
- [ ] **P5-49 Full-feature regression run** (L): run the P0-16 smoke test + a manual matrix of the 40 original plugins on 3 OSes; record in `docs/tests/`.
- [ ] **P5-50 Phase 5 review** (S): tick this file; list any plugin deliberately kept as C/C++ with reason.

Exit criteria: every plugin is either ported or explicitly kept as a thin FFI shim with an owner; the old C plugin directories are removed from the build.
