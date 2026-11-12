# Phase 4: Core object model and C ABI

Goal: port the remainder of `libobs` (sources, scenes, filters, transitions, encoders, outputs, services, hotkeys, modules, canvases, display) and ship a complete C ABI (`obs-ffi`) so unmodified C plugins keep working. At the end `libobs` C code is retired.

Source map: `libobs/obs.c`, `obs-internal.h`, `obs-source.c`, `obs-source-deinterlace.c`, `obs-source-transition.c`, `obs-scene.c`, `obs-encoder.c`, `obs-output.c`, `obs-service.c`, `obs-module.c`, `obs-hotkey.c`, `obs-canvas.c`, `obs-view.c`, `obs-display.c`, `obs-missing-files.c`, `obs-properties.c`, `obs-config.h`, `obs-core-modules.c.in` (34) + `obs-core-modules.h` (25) + the three `obs-core-modules-loader-{nix.c (62), win.c (66), mac.m (60)}`, `obs-cocoa.m` (835), `obs-nix.c` (458), `obs-nix-x11.c` (1280), `obs-nix-wayland.c` (1630), `obs-nix-platform.c` (45), `obs-windows.c` (1233), `obs-win-crash-handler.c` (534), `obs-interaction.h` (56).

## A. Design

- [ ] **P4-01 Object model design doc** (M): `Arc<Object>` handles, weak refs, typed IDs for the C ABI (`obs_source_t*` as opaque pointer = boxed `Arc`), destruction order, graphics-thread-only operations. ADR `0007-object-model.md`. Done when: ADR reviewed; handle, weak-ref and thread-affinity tables committed.
- [ ] **P4-02 Handle registry and refcount compat** (M): `obs_*_get_ref`/`release`, `obs_weak_*_addref/release`, `*_get_weak_*`, `obs_weak_*_references_*` with exact semantics (weak -> strong upgrade fails after destroy). Loom tests for races. Done when: refcount/weak semantics equal C after scripted sequences; loom green.
- [ ] **P4-03 Global context `obs_core`** (M): `obs.c` startup/shutdown: `obs_startup`, `obs_reset_video`, `obs_reset_audio`, `obs_shutdown`, locale, module paths, leak check via `bnum_allocs` (C prints nothing at shutdown; the check is added in Rust). Replace `obs-internal.h` globals with an owned `Core`. Done when: startup/reset/shutdown return the same codes and state as C (incl. double startup).
- [ ] **P4-04 Lock inventory and ordering** (S): enumerate all mutexes in `obs-internal.h`, define lock hierarchy, document in the ADR. Done when: all 46 mutexes tabulated; order checker panics on inverted acquisition.

## B. Sources

- [ ] **P4-05 Source info vtable adapter** (L): `struct obs_source_info` (all callbacks, flags `OBS_SOURCE_*`, versions via `version` field, deprecated fields) -> `trait Source` plus `CSource` adapter that calls C function pointers with the correct thread and locking rules. Done when: C sources register with old- and new-size `obs_source_info`.
- [ ] **P4-06 Source lifecycle** (L): create/destroy/update/`obs_source_update_properties`, `obs_source_load`, `obs_source_save`, defaults, `obs_source_set_name`, uniqueness, private sources, `obs_source_duplicate`. Done when: callback traces equal C over the P0-18 corpus.
- [ ] **P4-07 Async video sources** (L): `obs_source_output_video(2)`, frame cache, `async_frames`, timing (`os_gettime_ns`), `async_unbuffered`, deinterlacing (`obs-source-deinterlace.c`), `obs_source_get_frame`/`release_frame`, colour handling. Done when: ordering/timestamps equal C; deinterlace frame hashes per mode.
- [ ] **P4-08 Audio on sources** (L): `obs_source_output_audio`, audio buffer lifecycle, monitoring, mixers, balance, sync offset, `obs_source_audio_*` callbacks, audio capture callbacks for plugins. Done when: 44.1k→48k resample, monitoring and capture-callback goldens pass.
- [ ] **P4-09 Filters** (M): filter chain, `obs_source_filter_add/remove/set_order/set_index`, `obs_source_process_filter_*`, filter targets, effect begin/end helpers. Done when: filter order test + video/audio golden renders pass.
- [ ] **P4-10 Source render path** (L): `obs_source_video_render`, `obs_source_default_render`, filter chain in `source_render`, texrender caching, `OBS_SOURCE_CUSTOM_DRAW`, `OBS_SOURCE_COMPOSITE`. Done when: golden frames equal C for default, custom-draw and composite sources.
- [ ] **P4-11 Transitions** (L): `obs-source-transition.c`: `obs_transition_*`, duration modes (AUTO/MANUAL), `obs_transition_start`, mix callbacks, audio/video crossfade, T-bar. (No stinger code in libobs — stingers are a frontend concept.) Done when: frames at t=0/0.5/1 and PCM goldens equal C.
- [ ] **P4-12 Source signals and procs** (M): all `signal_handler` signals (`activate`, `show`, `rename`, `item_add`, ...) and the per-source `proc_handler` (libobs exposes `obs_source_get_proc_handler`; procs like obs-ffmpeg's `restart` are added by plugins). Table test: every signal name and signature from `docs/sphinx`. Done when: table test emits every signal and a plugin-registered proc.
- [ ] **P4-13 Source types registry** (S): `obs_register_source`, `obs_enum_*`, `obs_source_get_display_name`, `OBS_SOURCE_DEPRECATED` handling, icon types. Done when: enumeration order and metadata equal C.
- [ ] **P4-14 Interaction API** (S): `obs-interaction.h`, mouse/key events routed to sources. Done when: layout test passes; events reach a C source.
- [ ] **P4-15 Missing-files tracking** (S): `obs-missing-files.c`. Done when: add/count/destroy + replacement-callback tests pass.

## C. Scenes

- [ ] **P4-16 Scene core** (L): `obs-scene.c`: scene items, add/remove/reorder, `obs_scene_find_*`, transforms (pos/rot/scale/bounds/crop/align), visibility, locked, absolute coordinates, scale filter, blend type/method, show/hide transitions. Done when: item add/reorder/transform round trips and goldens equal C.
- [ ] **P4-17 Groups and nested scenes** (M): group items, `obs_scene_add_group`, `obs_sceneitem_group_*`, cycle detection. Done when: group/nested goldens pass; recursive add fails.
- [ ] **P4-18 Scene rendering** (M): item transform matrix calculation, cropping, bounds types, `obs_sceneitem_get_box_transform`, selection helpers. Golden for 50 transform cases. Done when: all 50 transform cases equal C.
- [ ] **P4-19 Scene save/load** (M): item JSON (transform keys, ids, ordering) compatible with existing collections. Round-trip over the P0-18 corpus. Done when: corpus round-trips with unknown keys preserved.
- [ ] **P4-20 Scene audio mixing** (M): scene audio composition in `scene_audio_render`; hidden items excluded (except while their hide transition is active). Done when: scene audio goldens pass, including hidden-item exclusion.

## D. Encoders, outputs, services

- [ ] **P4-21 Encoder vtable adapter** (L): `obs_encoder_info`, `obs-encoder.c`: encode paths on the graphics/audio threads (no dedicated encoder thread in C), `encode`, `encode_texture(2)`, scaling, `obs_encoder_set_scaled_size`, GPU scale, paused state, `obs_encoder_get_extra_data`, `get_sei_data` callback. Done when: ABI test encoder works; call-site thread trace equals C.
- [ ] **P4-22 Encoder lifecycle and signals** (M): start/stop semantics, `obs_encoder_active`, shared encoders (`obs_encoder_packet_*`), roi regions (`obs_encoder_add_roi`). Done when: state diagram and packet fan-out refcount tests pass.
- [ ] **P4-23 Output vtable adapter** (L): `obs_output_info`, `obs-output.c`: raw/encoded video/audio, `obs_output_begin_data_capture`, `obs_output_signal_stop`, reconnect with exponential back-off, `output_reconnect`, delays, `obs_output_get_total_bytes` etc. Done when: ABI test output works; raw + encoded capture flows pass.
- [ ] **P4-24 Output flags and multi-track** (M): `OBS_OUTPUT_MULTI_TRACK(_AV)`, `OBS_OUTPUT_SERVICE`, per-track encoders, `obs_output_set_audio_encoder(idx)`. Done when: each flag + 10 video / 6 audio per-track encoders tested.
- [ ] **P4-25 Service API** (S): `obs_service_info`, key/URL handling, `obs_service_apply_encoder_settings`, per-type `get_connect_info` (`OBS_SERVICE_CONNECT_INFO_*`). Done when: ABI test service works; bitrate clamp test passes.
- [ ] **P4-26 Multitrack video / enhanced RTMP plumbing** (L): `obs-service.c` per-type connect info, per-track video encoders (`obs_output_set_video_encoder2`, up to 10 tracks), `obs_output_get_connect_time_ms`. Dynamic bitrate is not in libobs 33 (it lives in `plugins/obs-outputs/rtmp-stream.c`). Done when: connect-info enums + per-track set/get tests pass; DBR out-of-scope note committed.
- [ ] **P4-27 Output state machine tests** (M): table-driven tests: start, stop, reconnect, congestion drop, force stop, signal order (`starting` -> `start` -> `stopping` -> `stop`). Done when: state table lists every legal transition; signal-order tests pass.

## E. Hotkeys, modules, display

- [ ] **P4-28 Hotkey system** (L): `obs-hotkey.c`, `obs-hotkey.h` (API; `obs-hotkeys.h` holds key names): registration, bindings, combos, press/release routing, hotkey thread, per-platform key translation (`obs-nix-x11.c`, `obs-cocoa.m`, `obs-windows.c`, wayland). Done when: JSON binding round trip + platform key-code tables equal C.
- [ ] **P4-29 Module loader** (L): `obs-module.c`, loader files for win/mac/nix, `OBS_DECLARE_MODULE`, `obs_module_load/unload/post_load/set_locale/get_string/description/author/name/ver/set_pointer`, bin/data paths, `obs_module_get_config_path`, `libobs/obs-core-modules` for built-in static modules, load failure reporting (`obs_module_failure_info`), disabled modules list. Done when: C plugins load/unload in the right order; failing modules reported.
- [ ] **P4-30 Locale and text lookup** (S): `obs_module_text`, fallback chain, `.ini` locale files. Done when: missing key returns the key; per-module locale files resolve.
- [ ] **P4-31 Display and views** (M): `obs_display_*`, `obs-view.c`, preview rendering with draw callbacks (`obs_display_add_draw_callback`). Done when: draw callback invoked with size; view channel set/get works.
- [ ] **P4-32 Platform integration** (L): `obs-nix.c`, `obs-nix-x11.c` (xcb + XInput2 via `x11rb`), `obs-nix-wayland.c` (core Wayland + xkbcommon via `wayland-client`), `obs-windows.c`, `obs-cocoa.m` -> Rust (`objc2` on macOS, `windows` crate on Windows): hotkey platform vtables (init/free/is_pressed + key translation), `log_system_info` (CPU/memory/etc.), module/data path helpers (nix). Done when: hotkey fires per platform; `log_system_info` fields identical.
- [ ] **P4-33 Canvases** (M): `obs-canvas.c`: multiple canvases, per-canvas video info, `obs_canvas_*`. Done when: a second canvas renders and records.

## F. ABI surface and verification

- [ ] **P4-34 `obs-ffi` header completeness** (L): every `EXPORT` in `obs.h`, `obs-source.h`, `obs-scene.h`, `obs-output.h`, `obs-encoder.h`, `obs-service.h`, `obs-data.h`, `obs-properties.h`, `obs-hotkey.h`, `obs-module.h`, `obs-missing-files.h`, `obs-nix-platform.h`, `obs-interaction.h`, `obs-audio-controls.h`, `obs-nal.h`, `obs-av1.h`, `obs-avc.h`, `obs-hevc.h` implemented. Script lists symbols from C `nm` and Rust `nm`; CI fails on any missing symbol. Done when: C-vs-Rust symbol diff is zero.
- [ ] **P4-35 ABI struct layout checks** (M): `static_assert`/`offset_of` tests for every public struct (`obs_source_info`, `obs_source_frame`, `obs_audio_data`, `encoder_packet`, `encoder_frame`, `obs_video_info`, ...). Done when: asserts pass on 3 OS × 2 architectures.
- [ ] **P4-36 Third-party plugin matrix** (L): load and run 10 popular external plugins (e.g. StreamFX-like, move-transition-like, source-record-like, DistroAV-like), record results in `docs/tests/`. Done when: 10 plugins run the standard scenario; results matrix committed.
- [ ] **P4-37 In-tree plugin compatibility run** (M): obs-websocket, obs-browser, obs-ffmpeg, obs-filters load and operate on the Rust core with the existing C `frontend`. Done when: all four in-tree plugins load and operate unmodified.
- [ ] **P4-38 Thread-safety test** (M): ThreadSanitizer and `loom` across create/destroy/render races; stress test with 200 sources toggled randomly for 1 h. Done when: TSan + 1 h stress clean.
- [ ] **P4-39 Memory test** (S): leak check (valgrind / ASan / Miri for pure crates) on the smoke test. Done when: deliberate leak detected; valgrind/ASan/Miri clean.
- [ ] **P4-40 Retire `libobs` C** (M): flip default; keep C sources one release behind `OBS_ENABLE_RUST=OFF`, then delete. Done when: C libobs deleted after the fallback release.
- [ ] **P4-41 Headless `obs-cli`** (M): binary that loads modules, a scene collection and runs an output (record/stream) without GUI; used for CI and servers. Done when: obs-cli records a scene headless (replaces the P0-16 host).
- [ ] **P4-42 Phase 4 review** (S): compatibility matrix, performance, tick this file. Done when: compatibility matrix committed; all parents ticked.

Exit criteria: C `frontend` and all C plugins run on the Rust core; symbol, layout and behaviour tests pass; `obs-cli` records a scene.
