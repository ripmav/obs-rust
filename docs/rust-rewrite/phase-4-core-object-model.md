# Phase 4: Core object model and C ABI

Goal: port the remainder of `libobs` (sources, scenes, filters, transitions, encoders, outputs, services, hotkeys, modules, canvases, display) and ship a complete C ABI (`obs-ffi`) so unmodified C plugins keep working. At the end `libobs` C code is retired.

Source map: `libobs/obs.c`, `obs-internal.h`, `obs-source.c`, `obs-source-deinterlace.c`, `obs-source-transition.c`, `obs-scene.c`, `obs-encoder.c`, `obs-output.c`, `obs-service.c`, `obs-module.c`, `obs-hotkey.c`, `obs-canvas.c`, `obs-view.c`, `obs-display.c`, `obs-missing-files.c`, `obs-properties.c`, `obs-config.h`, `obs-core-modules*`, `obs-cocoa.m`, `obs-nix*.c`, `obs-windows.c`, `obs-interaction.h`.

## A. Design

- [ ] **P4-01 Object model design doc** (M): `Arc<Object>` handles, weak refs, typed IDs for the C ABI (`obs_source_t*` as opaque pointer = boxed `Arc`), destruction order, graphics-thread-only operations. ADR `0007-object-model.md`.
- [ ] **P4-02 Handle registry and refcount compat** (M): `obs_*_get_ref`/`release`, `obs_weak_*_addref/release`, `*_get_weak_*`, `obs_weak_*_references_*` with exact semantics (weak -> strong upgrade fails after destroy). Loom tests for races.
- [ ] **P4-03 Global context `obs_core`** (M): `obs.c` startup/shutdown: `obs_startup`, `obs_reset_video`, `obs_reset_audio`, `obs_shutdown`, locale, module paths, leak check via `bnum_allocs` (C prints nothing at shutdown; the check is added in Rust). Replace `obs-internal.h` globals with an owned `Core`.
- [ ] **P4-04 Lock inventory and ordering** (S): enumerate all mutexes in `obs-internal.h`, define lock hierarchy, document in the ADR.

## B. Sources

- [ ] **P4-05 Source info vtable adapter** (L): `struct obs_source_info` (all callbacks, flags `OBS_SOURCE_*`, versions via `version` field, deprecated fields) -> `trait Source` plus `CSource` adapter that calls C function pointers with the correct thread and locking rules.
- [ ] **P4-06 Source lifecycle** (L): create/destroy/update/`obs_source_update_properties`, `obs_source_load`, `obs_source_save`, defaults, `obs_source_set_name`, uniqueness, private sources, `obs_source_duplicate`.
- [ ] **P4-07 Async video sources** (L): `obs_source_output_video(2)`, frame cache, `async_frames`, timing (`os_gettime_ns`), `async_unbuffered`, deinterlacing (`obs-source-deinterlace.c`), `obs_source_get_frame`/`release_frame`, colour handling.
- [ ] **P4-08 Audio on sources** (L): `obs_source_output_audio`, audio buffer lifecycle, monitoring, mixers, balance, sync offset, `obs_source_audio_*` callbacks, audio capture callbacks for plugins.
- [ ] **P4-09 Filters** (M): filter chain, `obs_source_filter_add/remove/set_order/set_index`, `obs_source_process_filter_*`, filter targets, effect begin/end helpers.
- [ ] **P4-10 Source render path** (L): `obs_source_video_render`, `obs_source_default_render`, filter chain in `source_render`, texrender caching, `OBS_SOURCE_CUSTOM_DRAW`, `OBS_SOURCE_COMPOSITE`.
- [ ] **P4-11 Transitions** (L): `obs-source-transition.c`: `obs_transition_*`, duration modes (AUTO/MANUAL), `obs_transition_start`, mix callbacks, audio/video crossfade, T-bar. (No stinger code in libobs — stingers are a frontend concept.)
- [ ] **P4-12 Source signals and procs** (M): all `signal_handler` signals (`activate`, `show`, `rename`, `item_add`, ...) and the per-source `proc_handler` (libobs exposes `obs_source_get_proc_handler`; procs like obs-ffmpeg's `restart` are added by plugins). Table test: every signal name and signature from `docs/sphinx`.
- [ ] **P4-13 Source types registry** (S): `obs_register_source`, `obs_enum_*`, `obs_source_get_display_name`, `OBS_SOURCE_DEPRECATED` handling, icon types.
- [ ] **P4-14 Interaction API** (S): `obs-interaction.h`, mouse/key events routed to sources.
- [ ] **P4-15 Missing-files tracking** (S): `obs-missing-files.c`.

## C. Scenes

- [ ] **P4-16 Scene core** (L): `obs-scene.c`: scene items, add/remove/reorder, `obs_scene_find_*`, transforms (pos/rot/scale/bounds/crop/align), visibility, locked, absolute coordinates, scale filter, blend type/method, show/hide transitions.
- [ ] **P4-17 Groups and nested scenes** (M): group items, `obs_scene_add_group`, `obs_sceneitem_group_*`, cycle detection.
- [ ] **P4-18 Scene rendering** (M): item transform matrix calculation, cropping, bounds types, `obs_sceneitem_get_box_transform`, selection helpers. Golden for 50 transform cases.
- [ ] **P4-19 Scene save/load** (M): item JSON (transform keys, ids, ordering) compatible with existing collections. Round-trip over the P0-18 corpus.
- [ ] **P4-20 Scene audio mixing** (M): scene audio composition in `scene_audio_render`; hidden items excluded (except while their hide transition is active).

## D. Encoders, outputs, services

- [ ] **P4-21 Encoder vtable adapter** (L): `obs_encoder_info`, `obs-encoder.c`: encode paths on the graphics/audio threads (no dedicated encoder thread in C), `encode`, `encode_texture(2)`, scaling, `obs_encoder_set_scaled_size`, GPU scale, paused state, `obs_encoder_get_extra_data`, `get_sei_data` callback.
- [ ] **P4-22 Encoder lifecycle and signals** (M): start/stop semantics, `obs_encoder_active`, shared encoders (`obs_encoder_packet_*`), roi regions (`obs_encoder_add_roi`).
- [ ] **P4-23 Output vtable adapter** (L): `obs_output_info`, `obs-output.c`: raw/encoded video/audio, `obs_output_begin_data_capture`, `obs_output_signal_stop`, reconnect with exponential back-off, `output_reconnect`, delays, `obs_output_get_total_bytes` etc.
- [ ] **P4-24 Output flags and multi-track** (M): `OBS_OUTPUT_MULTI_TRACK(_AV)`, `OBS_OUTPUT_SERVICE`, per-track encoders, `obs_output_set_audio_encoder(idx)`.
- [ ] **P4-25 Service API** (S): `obs_service_info`, key/URL handling, `obs_service_apply_encoder_settings`, per-type `get_connect_info` (`OBS_SERVICE_CONNECT_INFO_*`).
- [ ] **P4-26 Multitrack video / enhanced RTMP plumbing** (L): `obs-service.c` per-type connect info, per-track video encoders (`obs_output_set_video_encoder2`, up to 10 tracks), `obs_output_get_connect_time_ms`. Dynamic bitrate is not in libobs 33 (it lives in `plugins/obs-outputs/rtmp-stream.c`).
- [ ] **P4-27 Output state machine tests** (M): table-driven tests: start, stop, reconnect, congestion drop, force stop, signal order (`starting` -> `start` -> `stopping` -> `stop`).

## E. Hotkeys, modules, display

- [ ] **P4-28 Hotkey system** (L): `obs-hotkey.c`, `obs-hotkey.h` (API; `obs-hotkeys.h` holds key names): registration, bindings, combos, press/release routing, hotkey thread, per-platform key translation (`obs-nix-x11.c`, `obs-cocoa.m`, `obs-windows.c`, wayland).
- [ ] **P4-29 Module loader** (L): `obs-module.c`, loader files for win/mac/nix, `OBS_DECLARE_MODULE`, `obs_module_load/unload/post_load/set_locale/get_string/description/author/name/ver`, bin/data paths, `obs_module_get_config_path`, `libobs/obs-core-modules` for built-in static modules, load failure reporting (`obs_module_failure_info`), disabled modules list.
- [ ] **P4-30 Locale and text lookup** (S): `obs_module_text`, fallback chain, `.ini` locale files.
- [ ] **P4-31 Display and views** (M): `obs_display_*`, `obs-view.c`, preview rendering with draw callbacks (`obs_display_add_draw_callback`).
- [ ] **P4-32 Platform integration** (L): `obs-nix.c`, `obs-nix-x11.c` (xcb + XInput2 via `x11rb`), `obs-nix-wayland.c` (core Wayland + xkbcommon via `wayland-client`), `obs-windows.c`, `obs-cocoa.m` -> Rust (`objc2` on macOS, `windows` crate on Windows): hotkey platform vtables (init/free/is_pressed + key translation), `log_system_info` (CPU/memory/etc.), module/data path helpers (nix).
- [ ] **P4-33 Canvases** (M): `obs-canvas.c`: multiple canvases, per-canvas video info, `obs_canvas_*`.

## F. ABI surface and verification

- [ ] **P4-34 `obs-ffi` header completeness** (L): every `EXPORT` in `obs.h`, `obs-source.h`, `obs-scene.h`, `obs-output.h`, `obs-encoder.h`, `obs-service.h`, `obs-data.h`, `obs-properties.h`, `obs-hotkey.h`, `obs-module.h`, `obs-nix-platform.h`, `obs-interaction.h`, `obs-audio-controls.h`, `obs-nal.h`, `obs-av1.h`, `obs-avc.h`, `obs-hevc.h` implemented. Script lists symbols from C `nm` and Rust `nm`; CI fails on any missing symbol.
- [ ] **P4-35 ABI struct layout checks** (M): `static_assert`/`offset_of` tests for every public struct (`obs_source_info`, `obs_source_frame`, `obs_audio_data`, `encoder_packet`, `encoder_frame`, `obs_video_info`, ...).
- [ ] **P4-36 Third-party plugin matrix** (L): load and run 10 popular external plugins (e.g. StreamFX-like, move-transition-like, source-record-like, DistroAV-like), record results in `docs/tests/`.
- [ ] **P4-37 In-tree plugin compatibility run** (M): obs-websocket, obs-browser, obs-ffmpeg, obs-filters load and operate on the Rust core with the existing C `frontend`.
- [ ] **P4-38 Thread-safety test** (M): ThreadSanitizer and `loom` across create/destroy/render races; stress test with 200 sources toggled randomly for 1 h.
- [ ] **P4-39 Memory test** (S): leak check (valgrind / ASan / Miri for pure crates) on the smoke test.
- [ ] **P4-40 Retire `libobs` C** (M): flip default; keep C sources one release behind `OBS_ENABLE_RUST=OFF`, then delete.
- [ ] **P4-41 Headless `obs-cli`** (M): binary that loads modules, a scene collection and runs an output (record/stream) without GUI; used for CI and servers.
- [ ] **P4-42 Phase 4 review** (S): compatibility matrix, performance, tick this file.

Exit criteria: C `frontend` and all C plugins run on the Rust core; symbol, layout and behaviour tests pass; `obs-cli` records a scene.
