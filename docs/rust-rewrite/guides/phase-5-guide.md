# Phase 5 guide: plugins

Read [00-developer-primer.md](00-developer-primer.md) and the Phase 4 guide first. Tasks: [../phase-5-plugins.md](../phase-5-plugins.md); subtasks: [../subtasks/phase-5-subtasks.md](../subtasks/phase-5-subtasks.md).

## What a plugin is (worked example)

`plugins/obs-filters/gain-filter.c` (≈100 lines) is the whole pattern:

```c
struct gain_data { obs_source_t *context; size_t channels; float multiple; };
static void *gain_create(obs_data_t *settings, obs_source_t *filter) {
    struct gain_data *gf = bzalloc(sizeof(*gf)); gf->context = filter; gain_update(gf, settings); return gf; }
static void gain_update(void *data, obs_data_t *s) {
    gf->multiple = db_to_mul((float)obs_data_get_double(s, "db"));
    gf->channels = audio_output_get_channels(obs_get_audio()); }
static struct obs_audio_data *gain_filter_audio(void *data, struct obs_audio_data *audio) { /* multiply samples */ }
struct obs_source_info gain_filter = { .id = "gain_filter", .type = OBS_SOURCE_TYPE_FILTER,
    .output_flags = OBS_SOURCE_AUDIO, .get_name = gain_name, .create = gain_create,
    .destroy = gain_destroy, .update = gain_update, .filter_audio = gain_filter_audio,
    .get_defaults = gain_defaults, .get_properties = gain_properties };
```
`obs-filters.c` has `OBS_DECLARE_MODULE()`, `OBS_MODULE_USE_DEFAULT_LOCALE("obs-filters", "en-US")` and `obs_module_load()` which calls `obs_register_source(&gain_filter)` for each filter. Strings come from `data/locale/en-US.ini` via `obs_module_text`. CMake: `add_library(obs-filters MODULE)`; the module installs to `core/` (Windows) or `lib/obs-modules/core/` (Linux), data to `core/obs-filters/data` (Windows) or `share/obs/obs-modules/core/obs-filters` (Linux).

The Rust version (**Proposal**, SDK in P5-46) should read like:
```rust
#[obs_module(name = "obs-filters", description = "…")]
mod filters {
    use obs_plugin_api::prelude::*;
    pub struct Gain { channels: usize, multiple: f32 }
    impl Filter for Gain {
        const ID: &'static str = "gain_filter";
        fn create(settings: &Settings, _ctx: &SourceCtx) -> Self { … }
        fn update(&mut self, s: &Settings) { self.multiple = db_to_mul(s.double("db")); … }
        fn filter_audio(&mut self, audio: &mut AudioData) { for ch in audio.planes_mut() { for x in ch { *x *= self.multiple } } }
    }
}
```
Until the SDK exists, port plugins with a thin hand-written `extern "C"` registration (same struct fill) so the port does not wait for P5-46.

## Size and difficulty (measured lines of C/C++/ObjC/Swift per plugin)

| Lines | Plugins |
|---|---|
| 15k+ | `obs-filters` (includes vendored RNNoise), `obs-outputs` (includes vendored librtmp), `obs-ffmpeg`, `decklink`, `win-capture`, `obs-websocket` (submodule, tag 5.7.6) |
| 4k–9k | `aja`, `mac-avcapture`, `obs-qsv11`, `obs-nvenc`, `win-dshow`, `linux-pipewire`, `obs-browser` (submodule, pinned 2.23.4-126-gd4db73e) |
| 2k–4k | `mac-capture`, `mac-virtualcam`, `frontend-tools`, `nv-filters`, `image-source`, `linux-v4l2`, `text-freetype2`, `linux-capture`, `rtmp-services`, `win-wasapi` |
| 1k–2k | `obs-transitions`, `obs-vst`, `coreaudio-encoder`, `vlc-video`, `mac-videotoolbox`, `obs-text`, `obs-webrtc`, `linux-pulseaudio` |
| < 1k | `obs-x264`, `mac-syphon`, `*-output-ui`, `oss-audio`, `linux-alsa`, `sndio`, `linux-jack`, `obs-libfdk`, `decklink-captions` |

Submodule lines are measured the same way (C/C++/ObjC/Swift source files); the submodules are initialised in this checkout (fresh clones still need `git submodule update --init`).

## Choosing the order (risk-based)

1. **Pure logic with golden tests** (`obs-transitions`, `obs-filters`, `image-source`, `rtmp-services`, `obs-x264`): fastest feedback, proves the SDK. Start with `obs-transitions` (1.9k lines, GPU effects + simple timing) as the pilot (P5-46.5).
2. **Network and muxing** (`obs-outputs`, `obs-ffmpeg`, `obs-webrtc`, `obs-websocket`): memory-safety win is highest here (parsers and network input). Differential tests against nginx-rtmp/mediamtx/protocol suites.
3. **Platform capture/audio**: one OS at a time, after the matching Phase 3 shared-texture task exists.
4. **Hardware encoders**: thin SDK wrappers around vendor DLLs; low safety gain, high test cost (needs hardware).
5. **Vendor SDK cards**: keep SDK glue in C++ behind a tiny FFI crate.
6. **Browser**: CEF stays C++.

## Cross-cutting rules for every plugin port

- **Same module file name** (`obs-filters.so` / `.dll` / `.dylib`) and same data directory so installers, the plugin manager (`frontend/plugin-manager`) and users' "disabled modules" lists keep working.
- **Same ids**: `id` strings of sources/filters/encoders/outputs/services are stored in users' scene collections and profiles. Changing one silently breaks user data. Add a test that lists all registered ids and compares to a committed list per plugin.
- **Same settings keys and defaults** (they are written to JSON by the frontend); verify with the P0-18 corpus (S2 bookend).
- **Same locale keys** — locale files are shared and translated externally.
- **Hotkeys**: ids registered via `obs_hotkey_register_source` must keep their names (`MuteHotkey`…).
- **Threading**: record in the port which thread each callback runs on (graphics: `video_render`, `video_tick`; audio: `filter_audio`, `audio_render`; arbitrary: `create/update/destroy`). Capture plugins usually own a capture thread and hand frames to libobs via `obs_source_output_video`.
- **Version handling**: `obs_source_info.version` and `unversioned_id` mean old saved ids may map to new versions (e.g. `*_v2`); preserve the mapping.

## Plugin-specific context

**`obs-filters`.** One file per filter; GPU filters use `.effect` files in `data/` (`color_correction_filter.effect`, etc.) via `gs_effect_create_from_file`; audio filters implement `filter_audio` on planar float. Noise suppression uses vendored RNNoise (`rnnoise/`) and optionally Speex and the NVIDIA AFX (the latter lives in `nv-filters`). Compressor/limiter/expander each implement the same attack/release envelope math; compare against golden PCM with 1e-4 tolerance because float summation order changes.

**`obs-outputs`.** `rtmp-stream.c` (the RTMP output with congestion control, dynamic bitrate and multitrack), `flv-mux.c`, `rtmp-hevc.c`, `rtmp-av1.c`, `mp4-mux.c` (hybrid/fragmented MP4), `net-if.c` (bind-to-interface), vendored `librtmp/`. No FTL sources are present in this tree (decision task P5-18.3).

**`obs-ffmpeg`.** The biggest hub of external codecs. `ffmpeg-mux/` is a **separate helper executable** that receives packets on stdin: preserve its protocol exactly, as updaters and old versions may meet new ones. VAAPI, AMF (`texture-amf.cpp`), openh264, SRT/RIST. The media source uses `shared/media-playback/media-playback/*` (`media.c`, `decode.c`, `cache.c`; obs-ffmpeg is its only in-tree consumer) — port it as its own crate and reuse.

**`win-capture`.** Three capture families: DXGI desktop duplication, Windows Graphics Capture (needs `libobs-winrt`), and *game capture*, which injects `graphics-hook.dll` (`plugins/win-capture/graphics-hook/`, covers D3D8/9/10/11/12, DXGI, OpenGL and Vulkan layers) into the game via `inject-helper`. The hook runs inside third-party processes with anti-cheat constraints: it stays C/C++ (smallest possible dependency surface); port only the host side.

**`linux-pipewire`.** Portal-based capture: `portal.c`, `screencast-portal.c`, `camera-portal.c`, formats negotiation in `formats.c`; produces DMA-BUF textures (Phase 3 P3-18). Test on GNOME, KDE and a wlroots compositor.

**`mac-*`.** Objective-C/Swift today (`mac-capture` uses ScreenCaptureKit, `mac-avcapture` AVFoundation plus a second `mac-avcapture-legacy` module, `mac-virtualcam` a legacy CoreMediaIO DAL plug-in plus a system extension). Rust bindings via `objc2`; the virtual-camera *system extension* is a separate bundle with its own entitlements and may remain Swift (D-14: drop the DAL plug-in once the extension flow is the default).

**Hardware encoders.** `obs-nvenc` (`nvenc-d3d11.c`, `nvenc-cuda.c`, `nvenc-opengl.c`, `nvenc-properties.c`), `obs-qsv11` (+ `obs-qsv-test` helper), AMF inside `obs-ffmpeg`. Test helpers run in **separate processes** to probe drivers safely (a crashing driver must not take OBS down): keep that architecture.

**`obs-websocket`.** Submodule at tag 5.7.6 (16.9k LOC). Protocol surface: `src/websocketserver/` (Hello/Identify auth with SHA256+Base64 challenge, `obswebsocket.json`/`obswebsocket.msgpack` subprotocols), 147 request types in `src/requesthandler/` per category (+ batch, Canvases new in 5.7), 60 events in `src/eventhandler/`, vendor API in `lib/obs-websocket-api.h` (API v3). The protocol spec is *generated* from the C++ doc comments into `docs/generated/protocol.json` + `protocol.md` — treat that file (147 requests / 60 events / 7 enums, rpcVersion 1) as the conformance reference; there is no in-tree test suite at 5.7.6. Config + CLI args (`--websocket-port/-ipv4-only/-password/-debug`) in `src/Config.cpp`. It depends on the frontend API (`obs_frontend_*`, Phase 6) and on vendor requests used by third parties. External repo — its upstream owners may prefer to port in their own repo; coordinate before starting.

**`obs-browser`.** Submodule pinned at 2.23.4-126-gd4db73e (version file 2.27.5; 6.5k LOC). CEF (≥95) stays C++ and keeps its process model: a Windows `obs-browser-page` page process and four macOS `OBS Helper` bundles (GPU/Plugin/Renderer) with entitlements — the port must keep spawning them. `window.obsstudio` is registered via CefV8 in `browser-app.cpp`; source settings include `shutdown` (when not visible), `restart_when_active`, `is_local_file`, `reroute_audio`, `webpage_control_level`; cookie migration lives in the module main; the Qt dock is in `panel/`. The module also links the obs-websocket vendor API. External repo — coordinate with its upstream owners before starting.

## Pitfalls

- A "port" that changes behaviour silently is worse than a bug: always run the old and new plugin side by side on the same inputs (golden/differential).
- Don't conflate FFmpeg *bindings* with FFmpeg *functionality*. Use `ffmpeg-next`/`rsmpeg` or direct `-sys` bindings; match the FFmpeg major version the C build uses (see `cmake` for the pinned version in the dependency preset).
- Several plugins load vendor libraries at runtime (`nvEncodeAPI`, AFX/VFX SDKs, `libvlc`, CEF). Keep dynamic loading and graceful "not installed" behaviour; do not link statically.
- Some plugins are Windows-only (`obs-text`, `win-*`, `nv-filters`), macOS-only (`mac-*`), Windows+macOS (`coreaudio-encoder`, via `windows-imports.h` on Windows) or Linux-only; CI must build each on its OS.
- `frontend-tools` is mostly frontend code (Qt dialogs); its tasks live in Phase 6.

## Verification checklist per plugin

- [ ] id list, settings keys, locale keys unchanged (tests).
- [ ] Golden / differential test vs C plugin.
- [ ] Module file name and install layout unchanged.
- [ ] Manual smoke test on each supported OS recorded in `docs/tests/plugin-<name>.md`.
- [ ] C sources removed in the same PR.
