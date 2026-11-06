# Plan: Rewriting OBS Studio in Rust

Status: strategy confirmed — all 39 questions in [`docs/rust-rewrite/DECISIONS-NEEDED.md`](rust-rewrite/DECISIONS-NEEDED.md) answered 2026-11-06. Nothing implemented yet; tick the checkboxes as phases complete.

## 1. What exists today (measured)

| Area | Path | ~Lines | Language |
|---|---|---|---|
| Core engine (`libobs`) | `libobs/` | 94k | C (+ObjC for macOS) |
| Graphics backends | `libobs-opengl`, `libobs-d3d11`, `libobs-metal`, `libobs-winrt` | 17k | C / C++ / Swift |
| Frontend (Qt6 GUI) | `frontend/` | 109k | C++ (+ `.ui`) |
| Plugins (41 modules) | `plugins/` | 172k | C / C++ / ObjC |
| Shared helpers | `shared/` | 22k | C / C++ |
| Vendored deps | `deps/` (blake2, glad, json11, libcaption, libdshowcapture, ...) | 70k | C / C++ |

Total about 494k tracked source lines (`tools/task-coverage.py` against libobs 33.0.1), 15,819 commits, 3 OSes, 41 plugin modules.

Key architectural facts that shape the plan:

- **`libobs` is the product.** It owns sources, scenes, outputs, encoders, services, filters, the video/audio mixing threads, hotkeys, `obs_data` settings, and the signal/proc/calldata callback system. It is ~680 exported `EXPORT` symbols in `obs.h` alone.
- **The plugin ABI is C.** Plugins are shared libraries exporting `obs_module_load` etc. via `OBS_DECLARE_MODULE` (41 in-tree), registering `struct obs_source_info` and similar vtables. Hundreds of third-party plugins (and the scripting, browser and websocket integrations) depend on this ABI.
- **Graphics is an abstraction layer** (`libobs/graphics`, `gs_*` API) with a custom effect/shader language (`effect-parser.c`, `shader-parser.c`) transpiled per backend.
- **The frontend is Qt6 C++** (~109k lines incl. `.ui` forms, docks, settings, wizards, importers, updater, plugin manager) and also embeds Lua/Python scripting (`plugins/frontend-tools`).
- **Heavy native dependencies**: FFmpeg, x264, libcurl, Qt6, CEF (browser), PipeWire, Pulse/ALSA/JACK, V4L2, WASAPI, DirectShow, CoreAudio, AVFoundation, NVENC/AMF/QSV/VideoToolbox, Python/Lua.

## 2. Honest assessment

A big-bang rewrite is not advisable. At ~490k lines with a de-facto public C plugin ABI, it would take a multi-year effort by a team, freeze feature work, and break the plugin ecosystem, which is OBS's biggest asset. Rust also does not remove the need for FFmpeg, CEF, Qt, or platform capture APIs, so a large share of the code stays FFI glue regardless.

**Recommended strategy: a strangler-fig migration.** Rust and C coexist in one build for years. Each phase ships a working OBS. The C ABI (`obs.h`, `obs-module.h`, `obs-source.h`, ...) stays as a stable compatibility layer generated from Rust (`#[no_mangle] extern "C"`, `cbindgen`) until a native Rust plugin API is proven. "Everything in Rust" is the end state, reached incrementally.

## 3. Target architecture

Cargo workspace at the repo root, next to the CMake build (CMake invokes Cargo via Corrosion during the transition).

```
crates/
  obs-util          # bmem/dstr/darray/circlebuf/threading/platform helpers -> std + small crates
  obs-math          # vec2/3/4, matrix3/4, quat, plane, bounds (glam-based, repr(C) compatible)
  obs-data          # obs_data / obs_properties / JSON (serde_json), ABI-compatible
  obs-signal        # signal / proc / calldata, typed Rust API + C shim
  obs-graphics      # gs_* trait + wgpu backend (see section 4), effect language
  obs-media-io      # video-io, audio-io, format conversion, resampler
  obs-core          # sources, scenes, scene items, filters, transitions, outputs, encoders, services, hotkeys, mixers
  obs-ffi           # C ABI surface: generated obs.h, vtable adapters for C plugins
  obs-plugin-api    # native Rust plugin SDK (traits + proc macros), cdylib per D-06
  obs-modules/*     # one crate per current plugin
  obs-app           # frontend
  obs-cli           # headless binary used for tests and CI
```

Key design decisions (confirmed 2026-11-06 in `DECISIONS-NEEDED.md`; the Phase 0 ADR set encodes them):

1. **Concurrency**: dedicated real-time threads for video and audio mixing (as today), `crossbeam` channels / lock-free ring buffers on the hot path, `tokio` only for network outputs and services. No allocation or locks in the audio callback.
2. **Graphics**: `wgpu` as the single backend replaces OpenGL/D3D11/Metal, with a port of the effect language to WGSL (or compiled via `naga`). Risk: per-frame texture sharing with capture sources (DMA-BUF, DXGI shared handles, IOSurface) needs `wgpu` HAL escape hatches.
3. **Frontend toolkit**: the largest open decision. Options: `cxx-qt` (keeps Qt, `.ui` forms and the existing UX, lowest risk), `Slint`, `egui`, `iced`, or `Tauri`. Confirmed (D-01, 2026-11-06): `cxx-qt` first; P0-04 still prototypes the alternatives.
4. **Plugin model**: keep loading C-ABI `.so/.dll/.dylib` modules (via `libloading`) for compatibility; add a Rust-native plugin API as the preferred path for new in-tree modules.
5. **Encoders/muxing**: bind FFmpeg through `ffmpeg-next`/`ffmpeg-sys-next` (or `rsmpeg`); keep x264 via `x264` bindings. These are not rewritten.
6. **Scripting**: `mlua` for Lua, `pyo3` for Python, exposing the same script API.
7. **Unsafe policy**: `unsafe` only in FFI and platform capture boundaries, each with a `// SAFETY:` comment. `#![deny(unsafe_op_in_unsafe_fn)]`, Miri on pure crates, `cargo-deny`, `cargo-audit` in CI.

## 4. Phased roadmap

Order is dictated by dependency and risk: leaf libraries first, the core second, plugins third, GUI last.

### Phase 0 — Foundation (docs, tooling, ADRs)
- [ ] ADRs: strategy (strangler), graphics (wgpu), GUI toolkit, plugin ABI policy, MSRV, unsafe policy, licensing (GPL-2.0-or-later + allow-list, D-07) (`docs/adr/`, prefixed `docs:` PRs, first in the stack).
- [ ] Cargo workspace, Corrosion integration in CMake, CI matrix (Linux/macOS/Windows), `cargo fmt/clippy/deny/audit/test`.
- [ ] Golden-test harness: record the current C implementation's outputs (math, `obs_data` JSON, audio mixing, video compositing frames, effect parsing) as `*.golden` files; add `.gitattributes` with `*.text -text` and `*.golden -text`.
- [ ] ABI test: build a small C plugin against `obs.h` and run it against both implementations.
- [ ] Test matrices in `docs/tests/` (one per test file, one per issue).

### Phase 1 — Leaf libraries (`libobs/util`, `libobs/graphics/*math*`, `callback`, `obs-data`)
- [ ] `obs-util`, `obs-math`, `obs-signal`, `obs-data`, each exported through `obs-ffi` with the original C headers kept valid; C callers switch to the Rust static lib behind a CMake option.
- [ ] Drop `deps/blake2`, `deps/json11` in favour of crates. Candidates: `blake2`, `serde_json`.
- Exit: `libobs` unit tests and goldens pass with Rust-backed util/math/data/signal on all 3 OSes.

### Phase 2 — Media pipeline (`libobs/media-io`, `obs-audio*.c`, `obs-video*.c`, `obs-nal/avc/hevc/av1`)
- [ ] Video IO thread, format conversion (SIMD via `std::simd`/`pulp`), audio mixer, resampler, audio controls (meters, fader curves), NAL/OBU parsers.
- [ ] Bit-exact or tolerance-based comparison against Phase 0 goldens; benchmark with `criterion`, no regression beyond 2%.

### Phase 3 — Graphics
- [ ] Effect and shader language parser (`effect-parser.c`, `shader-parser.c`) in Rust, with `nom`/`winnow` and golden AST dumps.
- [ ] `gs_*` trait; wgpu backend; the legacy D3D11/Metal backends are ported to Rust (D-13, S-078/S-079) and retired with their C sources in Phase 7 (P7-01); the OpenGL backend stays C (D-13).
- [ ] Port built-in effects (`libobs/data/*.effect`) and verify with image-diff tests.
- [ ] Shared-texture interop for capture sources per platform.

### Phase 4 — Core object model (`libobs/obs-source*`, `obs-scene`, `obs-output`, `obs-encoder`, `obs-service`, `obs-hotkey`, `obs-module`, `obs-canvas`)
- [ ] Ownership model: `Arc`-based handles with weak refs replacing manual `obs_*_addref/release`; typed IDs for the C ABI.
- [ ] `obs-ffi`: complete `obs.h` surface, with vtable adapters so unmodified C plugins register sources/encoders/outputs.
- [ ] Module loader (`libloading`); the three platform loaders merge into one.
- [ ] Save/load of scene collections and profiles stays format-compatible (JSON); add round-trip tests with real user collections.
- Exit: unmodified upstream C plugins (obs-websocket, obs-browser, obs-ffmpeg) load and run on the Rust core. At this point every `libobs` function is served by Rust behind `OBS_ENABLE_RUST` (the C code itself is deleted in Phase 7, P7-01).

### Phase 5 — Plugins, in order of risk (each is its own PR stack)
1. Pure logic: `obs-filters`, `obs-transitions`, `obs-text`, `image-source`, `rtmp-services`, `obs-x264`.
2. Network/muxing: `obs-outputs` (RTMP ported per D-15; WHIP/WebRTC via `webrtc-rs` per D-16; FTL dropped per D-25 — not present in the 33.0.1 reference), `obs-ffmpeg` (mux; SRT/RIST via FFI over the same native libs per D-17; HLS), `obs-websocket` (`tokio-tungstenite`).
3. Platform capture/audio: Linux (`linux-pipewire`, `linux-v4l2`, `linux-pulseaudio`, `linux-alsa`, `linux-jack`, `linux-capture`), then Windows (`win-wasapi`, `win-capture`, `win-dshow`, `winrt`), then macOS (`mac-capture`, `mac-avcapture`, `mac-videotoolbox`, `mac-virtualcam`).
4. Hardware encoders: `obs-nvenc`, `obs-qsv11`, AMF, VideoToolbox. These are thin SDK wrappers, so low gain and high risk; port last.
5. Third-party SDK bound: `decklink`, `aja` (D-10) and `obs-vst` (D-11) port their glue to Rust over hand-written FFI to the vendor SDK (the vendored SDK trees stay); `obs-libfdk` and `vlc-video` stay thin FFI over their native libraries (libfdk, libvlc).
6. `obs-browser` (CEF): the CEF glue and page processes are ported to Rust (D-09); CEF itself stays a vendor library, accessed through its official C API.

### Phase 6 — Frontend (`frontend/`)
- [ ] With `cxx-qt` (D-01, confirmed 2026-11-06; P0-04 prototypes the alternatives): port docks, scene/source tree, mixer, settings, wizards, importers, updater, plugin manager incrementally; each QObject-based widget is replaced by a Rust-backed Qt widget module, migrating the `.ui` forms step by step. Frontend API (`OBSStudioAPI`) re-exposed through `obs-ffi`.
- [ ] Scripting (`frontend-tools`): `mlua` + `pyo3`.
- [ ] i18n (`data/locale/*.ini`) kept as is; loaded by a Rust loader.
- [ ] Packaging for Flatpak, Windows installer (MSIX per D-39, portable ZIP as fallback; P6-42.0 feasibility spike), macOS notarized app, plus the existing update channel.

### Phase 7 — Retire C/C++
- [ ] Remove the C ABI shim's default-on status (keep it as an opt-in compat feature for third-party plugins).
- [ ] Remove CMake; Cargo plus `cargo-xtask` handle the build, with a thin packaging layer.
- [ ] Publish the native plugin SDK 1.0.

## 5. Cross-cutting rules (per project guidelines)

- Work in stacked PRs, one per crate or plugin; start the stack with `docs:` ADR/spec PRs.
- Branches follow `<prefix>/<issue>-<short-description>`; commits carry the `Assisted-by:` footer.
- Update `README`, crate docs, and rustdoc with every change; mark plan tasks done in this file.
- Pre-commit gate for Rust crates: `cargo fmt --check`, `cargo clippy --all-targets -- -D warnings`, `cargo deny check`, `cargo audit`, `cargo test --workspace`. For any remaining Go tooling, the Go checklist applies.
- Every phase has a "no regression" gate: goldens, ABI test, benchmarks, and a manual smoke test (record 60 s of scene with browser source, mic, and game capture).

## 6. Risks

| Risk | Mitigation |
|---|---|
| Plugin ecosystem break | Keep C ABI compat through Phase 7; ABI conformance tests from Phase 0 |
| Real-time glitches (frame drops, audio xruns) | Lock-free hot path, soak tests, benchmarks in CI |
| wgpu cannot import platform shared textures cleanly | the OpenGL backend stays C as the non-wgpu fallback (D-13); D3D11/Metal are ported (S-078/S-079), not retained; wgpu HAL escape hatches for shared textures |
| GUI rewrite dwarfs everything else | toolkit decided by the Phase 0 prototype (D-01 confirmed: `cxx-qt`) |
| Scope creep and contributor burnout | Each phase ships, and each is independently valuable (memory safety in parsing and muxing first) |
| Unportable vendor SDKs (NVENC, AMF, DeckLink, AJA, CEF, VST) | the SDK binaries stay permanent FFI inputs; the glue around them is Rust (D-09/D-10/D-11); the SDKs themselves are not counted as "rewrite" |
| Licensing (GPLv2+) of new deps | `cargo-deny` license policy in Phase 0 (D-07 confirmed: GPL-2.0-or-later + allow-list) |

## 7. Rough effort (one team of ~6 experienced engineers)

| Phase | Estimate |
|---|---|
| 0 Foundation | 2-3 months |
| 1 Leaf libs | 3 months |
| 2 Media pipeline | 4-6 months |
| 3 Graphics | 6-9 months |
| 4 Core model + ABI | 9-12 months |
| 5 Plugins | 12-18 months (parallelisable) |
| 6 Frontend | 12-18 months (parallel with 5) |
| 7 Retire C/C++ | 3 months |

Realistic total: **3-4 years**. A useful interim milestone, a Rust headless core (`obs-cli`) running unmodified C plugins, lands after about 18-24 months.

## 8. Decisions for the maintainers

All 39 questions were confirmed or resolved with the user on 2026-11-06 — no open questions remain. The answers are recorded in
[`docs/rust-rewrite/DECISIONS-NEEDED.md`](rust-rewrite/DECISIONS-NEEDED.md) (strategy D-01…D-07,
scope and platform code D-08…D-14, dependency choices D-15…D-26, spec backlog D-27…D-33, plus D-34…D-39). The original four:

1. Is plugin ABI compatibility a hard requirement for the first Rust release? (D-02/D-05, confirmed: best-effort C ABI; deprecation after three releases.)
2. Which GUI toolkit? (D-01, confirmed: `cxx-qt`.)
3. Is dropping the native GL/D3D11/Metal backends in favour of `wgpu` acceptable? (D-03/D-13, confirmed: wgpu is the single backend; D3D11/Metal are ported to Rust and retired in P7-01; OpenGL stays C.)
4. Which platforms/vendor SDKs are in scope for the first all-Rust release? (D-04, confirmed: Linux/Windows/macOS, x86_64 + arm64; vendor SDKs via FFI.)

Detailed per-phase task lists live in [`docs/rust-rewrite/`](rust-rewrite/README.md).
