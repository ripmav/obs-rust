# Phase 4 subtasks

Parent file: [../phase-4-core-object-model.md](../phase-4-core-object-model.md). Each subtask has **Files**, **Steps**, **Done when**.

Verified facts: `libobs/obs-internal.h` has 46 `pthread_mutex_t` fields (P4-04 must account for each). Exported symbols (`EXPORT` count): `obs.h` 680, `obs-data.h` 156, `obs-properties.h` 109, `obs-hotkey.h` 55, `obs-module.h` 15, small counts in the others — about 1,000 in total, not only the 680 in `obs.h`. All function names used below were grepped in the headers. Most `obs_*` declarations (sources, scenes, outputs, encoders, canvases, displays) live in `obs.h`; `obs-source.h`, `obs-output.h`, `obs-encoder.h`, `obs-service.h` hold the `*_info` vtables and flags.

**Adapter pattern** (referred to below): for each plugin-facing kind (`obs_source_info`, `obs_encoder_info`, `obs_output_info`, `obs_service_info`) define a Rust `trait`, a `C<Kind>` adapter that stores the C struct (copied by its `size`/version, never assuming newer fields exist) and calls the C callbacks with the right thread/locking rules, and an ABI test case in the P0-15 plugin.

## A. Design

### P4-01 Object model design doc
- [ ] **P4-01.1 Handles** — decide that `obs_source_t*` etc. are `Box<Arc<Inner>>` pointers; define null/dangling rules; one handle type per object kind. Done when: table of every kind (source, scene, sceneitem, encoder, output, service, data, properties, canvas, display, view, module).
- [ ] **P4-01.2 Weak refs and destruction** — `obs_weak_source_*` etc.; destroy callback runs when the last strong ref drops; deferred destruction on the graphics thread. Done when: sequence diagrams for last-ref-on-wrong-thread.
- [ ] **P4-01.3 Thread affinity** — list every function requiring graphics context (`gs_*`, `video_render`, `obs_source_video_render`). Done when: table with enter/leave requirement.
- [ ] **P4-01.4 ADR 0007** — write and review.

### P4-02 Handle registry and refcount compat
- [ ] **P4-02.1 addref/release** — `obs_source_get_ref`, `obs_source_release` (there is no `obs_source_addref` in 33), and the equivalent for every kind. Done when: refcount observable through a debug function equals C after scripted sequences.
- [ ] **P4-02.2 Weak** — `obs_source_get_weak_source`, `obs_weak_source_get_source`, `addref/release_weak`, `references_source`. Done when: upgrade after destroy returns NULL.
- [ ] **P4-02.3 Race tests** — `loom`: release vs upgrade. Done when: model passes.

### P4-03 Global context
- [ ] **P4-03.1 `Core`** — struct replacing the `obs_core` globals in `obs-internal.h` (data, video, audio, hotkeys, modules, sources lists). Done when: no `static mut`.
- [ ] **P4-03.2 Startup/shutdown** — `obs_startup(locale, module_config_path, store)`, `obs_shutdown`, `obs_initialized`. Done when: double-startup returns false as in C.
- [ ] **P4-03.3 Reset video/audio** — `obs_reset_video(struct obs_video_info*)` (all 13 fields, error codes `OBS_VIDEO_SUCCESS/NOT_SUPPORTED/INVALID_PARAM/CURRENTLY_ACTIVE/MODULE_NOT_FOUND/FAIL`), `obs_reset_audio(2)`. Done when: invalid combinations return the same codes.
- [ ] **P4-03.4 Leak check** — C `obs_shutdown` prints nothing; add a Rust check comparing `bnum_allocs()` (or the Rust allocator counter) to the startup baseline. Done when: deliberate leak is detected by the check.

### P4-04 Lock inventory and ordering
- [ ] **P4-04.1 Table of 46 mutexes** — name, struct, protects, order. Done when: complete.
- [ ] **P4-04.2 Order checker** — debug-only thread-local stack assertion on acquire. Done when: inverted acquisition panics in tests.

## B. Sources

### P4-05 Source info vtable adapter
- [ ] **P4-05.1 Trait** — methods mirroring `obs_source_info` callbacks (`get_name`, `create`, `destroy`, `get_width/height`, `get_defaults(2)`, `get_properties(2)`, `update`, `activate/deactivate`, `show/hide`, `video_tick`, `video_render`, `audio_render`, `filter_*`, `enum_active_sources`, `save/load`, `mouse_*`, `key_click`, `focus`, `missing_files`, `get_color_space`, …). Done when: one-to-one table.
- [ ] **P4-05.2 `CSource`** — copy the struct respecting `obs_register_source_s(info, size)`; ignore unknown fields. Done when: old and new-size structs both register.
- [ ] **P4-05.3 Flags** — all 19 `output_flags` (plus the 4 `OBS_SOURCE_TYPE_*`): `OBS_SOURCE_VIDEO/AUDIO/ASYNC/ASYNC_VIDEO/CUSTOM_DRAW/INTERACTION/COMPOSITE/DO_NOT_DUPLICATE/DEPRECATED/DO_NOT_SELF_MONITOR/CAP_DISABLED/CAP_DONT_SHOW_PROPERTIES/MONITOR_BY_DEFAULT/CONTROLLABLE_MEDIA/CEA_708/SRGB/CAP_OBSOLETE/REQUIRES_CANVAS/SUBMIX`. Done when: each flag has a behavioural test.
- [ ] **P4-05.4 Threading** — which callbacks run on graphics, audio, UI threads and with which locks held. Done when: documented and asserted.

### P4-06 Source lifecycle
- [ ] **P4-06.1 Create/destroy** — `obs_source_create`/`_private`, stub-info path for unknown ids (source still created "so the user's data doesn't become lost"), destroy order. Done when: callback trace equal.
- [ ] **P4-06.2 Update** — `obs_source_update`, `obs_source_update_properties`, `obs_source_properties`. Done when: settings applied once.
- [ ] **P4-06.3 Save/load** — `obs_save_source`, `obs_load_source`, `obs_load_sources` (JSON keys: `prev_ver`, `name`, `uuid`, `id`, `versioned_id`, `settings`, `mixers`, `sync`, `flags`, `volume`, `balance`, `enabled`, `muted`, `monitoring_type`, `monitoring_enabled`, `hotkeys`, `deinterlace_mode`, `deinterlace_field_order`, `canvas_uuid`, `filters`, `private_settings`). Done when: corpus round trip.
- [ ] **P4-06.4 Naming/duplicate** — unique names, UUIDs, `obs_source_duplicate`, `obs_source_set_name`. Done when: tests.

### P4-07 Async video sources
- [ ] **P4-07.1 Frame cache** — `obs_source_output_video(2)`, `async_frames`, `cache`. Done when: ordering and drop semantics equal.
- [ ] **P4-07.2 Timing** — `async_unbuffered`, `async_decoupled`, timestamp smoothing in `obs-source.c`. Done when: simulated timestamp series equals C.
- [ ] **P4-07.3 Deinterlace** — `libobs/obs-source-deinterlace.c` modes. Done when: frame hash per mode.
- [ ] **P4-07.4 Get/release** — `obs_source_get_frame`, `release_frame`, colour space conversions. Done when: tests.

### P4-08 Audio on sources
- [ ] **P4-08.1 Output audio** — `obs_source_output_audio`, resampling to the core format. Done when: tests with 44.1k→48k.
- [ ] **P4-08.2 Monitoring/balance/sync** — flags `OBS_MONITORING_TYPE_*`, `obs_source_set_sync_offset`. Done when: golden.
- [ ] **P4-08.3 Capture callbacks** — `obs_source_add_audio_capture_callback`. Done when: callbacks fire per tick with expected data.

### P4-09 Filters
- [ ] **P4-09.1 Chain** — `obs_source_filter_add/remove/set_order/set_index`, `obs_source_get_filter_by_name`, `obs_filter_get_parent`, `obs_filter_get_target`. Done when: order test.
- [ ] **P4-09.2 Processing** — `obs_source_process_filter_begin/end/tech_end`, `obs_source_skip_video_filter`, pass-through for filters without a `filter_video` callback (there is no `obs_source_default_filter_*` in 33). Done when: golden render.
- [ ] **P4-09.3 Audio filters** — `filter_audio` callback chain. Done when: golden.

### P4-10 Source render path
- [ ] **P4-10.1 Render** — `obs_source_video_render`, `obs_source_default_render`, effect selection. Done when: golden frame.
- [ ] **P4-10.2 Texrender** — caching when filters exist, `OBS_SOURCE_CUSTOM_DRAW`. Done when: texture reuse test.
- [ ] **P4-10.3 Composite** — `OBS_SOURCE_COMPOSITE`, `get_width/height`, `obs_source_get_base_*`. Done when: tests.

### P4-11 Transitions
- [ ] **P4-11.1 State** — `obs_transition_start`, `set`, `clear`, `get_time`, duration modes (`OBS_TRANSITION_MODE_AUTO/MANUAL`). Done when: timeline test.
- [ ] **P4-11.2 Mix callbacks** — `transition_video_render`, A/B sources, `obs_transition_video_render(2)`, `obs_transition_get_source`. Done when: frames at t=0,0.5,1 equal.
- [ ] **P4-11.3 Audio** — `obs_transition_audio_render`, mix callbacks (`mix_a/mix_b`). Done when: PCM golden.
- [ ] **P4-11.4 Helpers** — T-bar `obs_transition_set_manual_time`, duration-mode helpers. (Stingers are a frontend concept — a transition over a video source; libobs has no stinger code.) Done when: tests.

### P4-12 Source signals and procs
- [ ] **P4-12.1 List** — extract signals from `obs-source.c` (`signal_handler_add` strings) and docs. Done when: list committed.
- [ ] **P4-12.2 Signals** — implement all with exact parameter names. Done when: table test emits each.
- [ ] **P4-12.3 Procs** — expose `obs_source_get_proc_handler` per source; plugins add their own procs (e.g. obs-ffmpeg `restart`/`get_duration`) and must call them through the ABI. (`get_hotkey_bindings`/`get_hotkey_names` do not exist in 33.0.1.) Done when: table test with a plugin-registered proc.

### P4-13 Source types registry
- [ ] **P4-13.1 Registration** — `obs_register_source_s`, `obs_enum_source_types`, `obs_enum_input_types(2)`, `obs_enum_filter_types`, `obs_enum_transition_types`. Done when: enumeration order equal.
- [ ] **P4-13.2 Metadata** — `obs_source_get_display_name`, `obs_source_get_icon_type`, deprecated filtering. Done when: tests.

### P4-14 Interaction API
- [ ] **P4-14.1 Types** — `libobs/obs-interaction.h` structs. Done when: layout test.
- [ ] **P4-14.2 Routing** — `obs_source_send_mouse_click/move/wheel`, `send_key_click`, `send_focus`. Done when: events reach C source.

### P4-15 Missing-files tracking
- [ ] **P4-15.1 Tracker** — `libobs/obs-missing-files.c`: `obs_missing_files_create/add_file/count/get_file/destroy/append`. Done when: add/remove/count tests.
- [ ] **P4-15.2 Callback** — `obs_missing_file_create(path, callback, src_type, …)` and `obs_missing_file_issue_callback` (how the frontend learns of replaced files). Done when: callback fires with the new path.

## C. Scenes

### P4-16 Scene core
- [ ] **P4-16.1 Scene as source** — `obs_scene_create`, `_private`, `obs_scene_get_source`. Done when: scene usable as input.
- [ ] **P4-16.2 Items** — `obs_scene_add`, `obs_sceneitem_remove`, reorder (`set_order`, `set_order_position`). Done when: order tests.
- [ ] **P4-16.3 Transform** — pos/rot/scale/bounds/bounds_type/alignment/crop/`obs_sceneitem_set_info2`. Done when: round trip.
- [ ] **P4-16.4 Visual flags** — scale filter, blend method/type, locked, visible; the scene-level `AbsoluteCoords` private setting drives the pos/scale conversions. Done when: golden.
- [ ] **P4-16.5 Show/hide transitions** — `obs_sceneitem_set_transition(item, show, transition)`, `obs_sceneitem_set_transition_duration`, `obs_sceneitem_do_transition`, `obs_sceneitem_transition_load`. Done when: test.

### P4-17 Groups and nested scenes
- [ ] **P4-17.1 Groups** — `obs_scene_add_group(2)`, `obs_sceneitem_group_add_item`, `obs_sceneitem_group_ungroup(2)`, `obs_sceneitem_group_enum_items`. Done when: tests.
- [ ] **P4-17.2 Nested** — scene-in-scene rendering. Done when: golden.
- [ ] **P4-17.3 Cycles** — `obs_source_add_active_child` detects infinite source recursion (self/ancestor reference); add fails with a log. Done when: add of parent fails.

### P4-18 Scene rendering
- [ ] **P4-18.1 Matrix** — `update_item_transform`. Done when: 50 cases equal to C.
- [ ] **P4-18.2 Crop/bounds** — all `obs_bounds_type` modes. Done when: golden.
- [ ] **P4-18.3 Queries** — `obs_sceneitem_get_box_transform`, `get_box_scale`, `get_draw_transform`. Done when: matrix equality.
- [ ] **P4-18.4 Goldens** (50 transform cases).

### P4-19 Scene save/load
- [ ] **P4-19.1 Item JSON** — keys `name`, `source_uuid`, `visible`, `locked`, `rot`, `pos`, `scale`, `align`, `bounds`, `crop_*`, `blend_type`, `scale_filter`, `group_item_backup`, `show_transition`, …. Done when: unknown keys preserved.
- [ ] **P4-19.2 IDs/ordering** — `id`, `private_settings`. Done when: stable ids.
- [ ] **P4-19.3 Corpus round trip**.

### P4-20 Scene audio mixing
- [ ] **P4-20.1 Compose** — child audio mixing in scene source. Done when: golden PCM.
- [ ] **P4-20.2 Hidden items** — a non-visible item contributes nothing to scene audio, except while its hide transition is active (`scene_audio_render`). Done when: test.

## D. Encoders, outputs, services

### P4-21 Encoder vtable adapter
- [ ] **P4-21.1 Trait/adapter** (adapter pattern) for `obs_encoder_info`: `create`, `destroy`, `encode`, `encode_texture(2)`, `get_defaults`, `get_properties`, `update`, `get_extra_data`, `get_sei_data`, `get_audio_info/video_info`, `get_frame_size`, `get_name`. Done when: ABI test encoder works.
- [ ] **P4-21.2 Encode path** — `encode`/`encode_texture(2)` are called from the graphics thread (video mix) and audio thread (mixer); C has no dedicated encoder thread; packet callbacks to outputs under `callbacks_mutex`/`outputs_mutex`. Done when: call-site thread trace equal.
- [ ] **P4-21.3 Texture path** — `encode_texture` with `gs_texture`/handle (needs P3-21). Done when: mocked texture encoder.
- [ ] **P4-21.4 Scaling** — `obs_encoder_set_scaled_size`, `set_gpu_scale_type`, `obs_encoder_scaling_enabled`. Done when: size test.

### P4-22 Encoder lifecycle and signals
- [ ] **P4-22.1 Active state** — `obs_encoder_active`, `obs_encoder_paused`, internal `obs_encoder_initialize` (called from `obs_output_start`; no public init/uninit), `obs_encoder_set_preferred_video_format`. Done when: state diagram test.
- [ ] **P4-22.2 Sharing** — packet fan-out: public `obs_encoder_packet_ref/release`, internal `obs_encoder_packet_create_instance` (interleaving and delay copy the packet). Done when: refcount check.
- [ ] **P4-22.3 ROI** — `obs_encoder_add_roi`, `obs_encoder_has_roi`, `obs_encoder_clear_roi`, `obs_encoder_enum_roi`, `obs_encoder_get_roi_increment`. Done when: tests.

### P4-23 Output vtable adapter
- [ ] **P4-23.1 Trait/adapter** (adapter pattern) for `obs_output_info`: `start`, `stop`, `raw_video`, `raw_audio(2)`, `encoded_packet`, `get_properties`, `get_total_bytes`, `get_dropped_frames`, `get_congestion`, `get_connect_time_ms`, codec strings, `protocols` (there is no `get_stats` field). Done when: ABI test output works.
- [ ] **P4-23.2 Data capture** — `obs_output_begin_data_capture`, `end_data_capture`, flags `OBS_OUTPUT_VIDEO/AUDIO/ENCODED/SERVICE`. Done when: raw and encoded flows.
- [ ] **P4-23.3 Reconnect** — `obs_output_set_reconnect_settings`, exponential back-off, `reconnect_retry_*`. Done when: fake-clock test.
- [ ] **P4-23.4 Delay/stats** — `obs_output_set_delay`, counters. Done when: tests.

### P4-24 Output flags and multi-track
- [ ] **P4-24.1 Flags** — `OBS_OUTPUT_MULTI_TRACK`, `OBS_OUTPUT_MULTI_TRACK_VIDEO`, `OBS_OUTPUT_MULTI_TRACK_AV`, `OBS_OUTPUT_CAN_PAUSE`. Done when: each tested.
- [ ] **P4-24.2 Per-track encoders** — `obs_output_set_audio_encoder(output, enc, idx)`. Done when: 6 tracks.
- [ ] **P4-24.3 Service** — `obs_output_set_service`. Done when: URL/key reach output.

### P4-25 Service API
- [ ] **P4-25.1 Adapter** for `obs_service_info`: `get_url`, `get_key`, `get_username`, `get_password`, `apply_encoder_settings`, `get_supported_video_codecs`, `get_protocol`, `get_connect_info`, `can_try_to_connect`. Done when: ABI test service.
- [ ] **P4-25.2 URL/key** — `obs_service_get_connect_info(type)`. Done when: tests.
- [ ] **P4-25.3 Encoder settings** — `obs_service_apply_encoder_settings`. Done when: bitrate clamp test.

### P4-26 Multitrack video plumbing
- [ ] **P4-26.1 Connect info** — `OBS_SERVICE_CONNECT_INFO_*` enums. Done when: all values.
- [ ] **P4-26.2 Per-track encoders** — `obs_output_set_video_encoder2`/`obs_output_get_video_encoder2`, `obs_output_set_audio_encoder`/`obs_output_get_audio_encoder` (10 video / 6 audio tracks, `MAX_OUTPUT_*_ENCODERS`). Done when: tests.
- [ ] **P4-26.3 DBR out of scope** — verify libobs 33.0.1 has no dynamic-bitrate API (it lives in `plugins/obs-outputs/rtmp-stream.c`); note in the phase review that the Rust port inherits the same split. Done when: note committed.

### P4-27 Output state machine tests
- [ ] **P4-27.1 State table** — starting/active/stopping/reconnecting/paused/delay. Done when: table lists every legal transition.
- [ ] **P4-27.2 Signal order** — `starting`, `start`, `stopping`, `stop`, `reconnect`, `reconnect_success`, `pause`, `unpause`. Done when: expected sequences.
- [ ] **P4-27.3 Failure/force-stop** — `obs_output_force_stop`, `obs_output_signal_stop` codes (`OBS_OUTPUT_SUCCESS` = 0, `obs-defs.h`). Done when: tests.

## E. Hotkeys, modules, display

### P4-28 Hotkey system
- [ ] **P4-28.1 Registration** — `obs_hotkey_register_frontend/source/output/encoder/service`, `unregister`. Done when: ids stable.
- [ ] **P4-28.2 Bindings** — `obs_hotkey_load_bindings` (raw combinations), `obs_hotkey_load` (JSON array), `obs_hotkeys_load_source/output/encoder/service`, combos with modifiers. Done when: JSON round trip.
- [ ] **P4-28.3 Thread and routing** — hotkey thread, press/release, push-to-talk helpers (`obs_hotkey_pair_*`). Done when: simulated key events.
- [ ] **P4-28.4 Platform keys** — `obs-nix-x11.c`, `obs-nix-wayland.c`, `obs-windows.c`, `obs-cocoa.m` translators. Done when: key-code tables equal.

### P4-29 Module loader
- [ ] **P4-29.1 Descriptor** — `obs_module_t`, exported functions `obs_module_load/unload/post_load/set_locale/free_locale/get_string/author/name/description/ver/set_pointer`. Done when: all resolved via `libloading`.
- [ ] **P4-29.2 Paths** — `obs_add_module_path`, search patterns from `obs-core-modules-loader-{nix.c,win.c,mac.m}`. Done when: each OS finds modules.
- [ ] **P4-29.3 Lifecycle** — `obs_open_module`, `obs_init_module`, `obs_post_load_modules`, unload (`mod->unload` called from `free_module` during shutdown; C deliberately does not dlclose). Done when: order test.
- [ ] **P4-29.4 Failures** — `struct obs_module_failure_info` + `obs_module_failure_info_free`, `obs_load_plugins(info, error)` (the older `obs_load_all_modules2` is `OBS_DEPRECATED` but still exported), `MODULE_FAILED_TO_OPEN` / `MODULE_MISSING_EXPORTS` codes, disabled list, `obs_get_module_file_name`, `obs_module_get_locale_string/text`. Done when: failing module reported.
- [ ] **P4-29.5 Built-in modules** — `obs-core-modules.c.in` static list. Done when: generated Rust table.

### P4-30 Locale and text lookup
- [ ] **P4-30.1 `obs_module_text`** and `obs_module_get_config_path`. Done when: tests.
- [ ] **P4-30.2 Fallback** — `en-US` default. Done when: missing key returns key.

### P4-31 Display and views
- [ ] **P4-31.1 Lifecycle** — `obs_display_create/destroy/resize`, `obs_display_set_background_color`. Done when: tests with null graphics.
- [ ] **P4-31.2 Draw callbacks** — `obs_display_add_draw_callback`, `remove`. Done when: callback invoked with size.
- [ ] **P4-31.3 Views** — `libobs/obs-view.c`, `obs_view_*`. Done when: channel set/get.

### P4-32 Platform integration
- [ ] **P4-32.1 X11** — `obs-nix-x11.c` via `x11rb`: hotkey platform vtable (XInput2 raw events on the root window, pointer coordinates) + `obs_nix_x11_log_info`. Done when: hotkey fires.
- [ ] **P4-32.2 Wayland** — `obs-nix-wayland.c` (41 KB: `wl_keyboard` + xkbcommon keymaps for hotkey translation, seat capabilities, `obs_nix_wayland_log_info`; no portal protocols in 33.0.1); `wayland-client`. Done when: connect and log compositor.
- [ ] **P4-32.3 Windows** — `obs-windows.c`: version info, CPU names, `obs_hotkeys_platform`, crash handler `obs-win-crash-handler.c`. Done when: logs identical fields.
- [ ] **P4-32.4 macOS** — `obs-cocoa.m` (34 KB): hotkeys, system info, permissions. Done when: logs identical.
- [ ] **P4-32.5 System info logging** — `log_system_info` content parity. Done when: log diff reviewed.

### P4-33 Canvases
- [ ] **P4-33.1 Canvas** — `obs_canvas_create(/_private)`, `obs_canvas_release/get_ref` (destroy is internal), `obs_canvas_get_video_info`, `obs_canvas_reset_video`, `obs_get_main_canvas`. Done when: second canvas.
- [ ] **P4-33.2 Outputs** — per-canvas `obs_canvas_get_video`. Done when: output records canvas.

## F. ABI surface and verification

### P4-34 `obs-ffi` header completeness
- [ ] **P4-34.1 C symbol list** — script: preprocess headers, list `EXPORT` function names (~1,000). Done when: file committed.
- [ ] **P4-34.2 Rust symbol list** — `nm -D` on `obs-ffi`. Done when: script prints missing/extra.
- [ ] **P4-34.3 CI gate** on missing.
- [ ] **P4-34.4 Fill gaps** until zero.

### P4-35 ABI struct layout checks
- [ ] **P4-35.1 Generate** — `size_of`/`offset_of` assertions for each public struct (`obs_source_info`, `obs_source_frame(2)`, `obs_audio_data`, `obs_source_audio`, `encoder_packet`, `encoder_frame`, `obs_video_info`, `obs_audio_info(2)`, `obs_output_info`, `obs_encoder_info`, `obs_service_info`, `gs_init_data`, …). Done when: asserts compile.
- [ ] **P4-35.2 Platforms** — Linux/Windows/macOS × x86_64/aarch64. Done when: matrix green.

### P4-36 Third-party plugin matrix
- [ ] **P4-36.1 Select** 10 widely used plugins covering sources, filters, transitions, outputs, encoders.
- [ ] **P4-36.2 Obtain** builds for the three OSes (record versions and licences).
- [ ] **P4-36.3 Run** a standard scenario each; log results in `docs/tests/third-party-plugins.md`.

### P4-37 In-tree plugin compatibility run
- [ ] **P4-37.1 Submodules** — run `git submodule update --init plugins/obs-websocket plugins/obs-browser` (empty in a fresh checkout). Done when: sources present.
- [ ] **P4-37.2 Load** obs-websocket and obs-browser on the Rust core. Done when: both initialise.
- [ ] **P4-37.3 Load** obs-ffmpeg, obs-filters and run P0-16 smoke test.

### P4-38 Thread-safety test
- [ ] **P4-38.1 TSan builds** (Linux). Done when: smoke test clean.
- [ ] **P4-38.2 Stress** — 200 sources randomly created/destroyed/toggled for 1 h. Done when: no crash or leak.
- [ ] **P4-38.3 Fix findings**.

### P4-39 Memory test
- [ ] **P4-39.1 Valgrind/ASan** on smoke test.
- [ ] **P4-39.2 Miri** for pure crates.

### P4-40 Retire libobs C
- [ ] **P4-40.1 Flip default** to Rust.
- [ ] **P4-40.2 One release** with `OBS_ENABLE_RUST=OFF` fallback.
- [ ] **P4-40.3 Delete** C sources in the next release.

### P4-41 Headless `obs-cli`
- [ ] **P4-41.1 CLI** (`clap`): `--module-path`, `--collection`, `--profile`, `--record`, `--stream`, `--duration`.
- [ ] **P4-41.2 Loading** modules and collection.
- [ ] **P4-41.3 Commands** record/stream; exit codes. Done when: replaces the P0-16 host.

### P4-42 Phase 4 review
- [ ] **P4-42.1 Matrix** of compatibility results.
- [ ] **P4-42.2 Tick** parents.
