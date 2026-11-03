# Phase 4 subtasks

Parent file: [../phase-4-core-object-model.md](../phase-4-core-object-model.md). Each subtask has **Files**, **Steps**, **Done when**.

Verified facts: `libobs/obs-internal.h` has 46 `pthread_mutex_t` fields (P4-04 must account for each). Exported symbols (`EXPORT` count): `obs.h` 680, `obs-data.h` 156, `obs-properties.h` 109, `obs-hotkey.h` 55, `obs-module.h` 15, small counts in the others — about 1,000 in total, not only the 680 in `obs.h`. All function names used below were grepped in the headers. Most `obs_*` declarations (sources, scenes, outputs, encoders, canvases, displays) live in `obs.h`; `obs-source.h`, `obs-output.h`, `obs-encoder.h`, `obs-service.h` hold the `*_info` vtables and flags.

**Adapter pattern** (referred to below): for each plugin-facing kind (`obs_source_info`, `obs_encoder_info`, `obs_output_info`, `obs_service_info`) define a Rust `trait`, a `C<Kind>` adapter that stores the C struct (copied by its `size`/version, never assuming newer fields exist) and calls the C callbacks with the right thread/locking rules, and an ABI test case in the P0-15 plugin.

## A. Design

### P4-01 Object model design doc
- [ ] **P4-01.1 Handles**
  - Files: `libobs/obs.h` (2734; opaque typedefs + handle APIs), `libobs/obs-internal.h` (1543; per-kind struct layouts), `libobs/obs-source.h` (572), `libobs/obs-scene.h` (116), `libobs/obs-output.h` (96), `libobs/obs-encoder.h` (365), `libobs/obs-service.h` (115).
  - Steps: list every object kind with its create/get_ref/release/weak APIs from the headers; decide `obs_source_t*` etc. are `Box<Arc<Inner>>` pointers, one handle type per kind; define null/dangling rules (invalid handle = debug log + no-op + default return, S-040 §3.2); produce the table.
  - Spec: S-040 §3.2, S-042 §5.7 R74–R79.
  - Done when: table of every kind (source, scene, sceneitem, encoder, output, service, data, properties, canvas, display, view, module).
- [ ] **P4-01.2 Weak refs and destruction**
  - Files: `libobs/obs-source.c` (6070; `obs_source_create_internal`, `obs_source_destroy`), `libobs/obs-internal.h` (1543; `obs_weak_source` control block, `destruction_task_thread`).
  - Steps: extract the control-block scheme (strong counter in `obs_weak_*`, upgrade via `obs_ref_release`/get_ref) and the immediate/deferred destruction split (S-040 R08/R09, deferred on `destruction_task_thread`); sequence-diagram the last-ref-on-wrong-thread case.
  - Spec: S-040 §5.1 R07–R10.
  - Vectors: obssource (`01_lifecycle_refcount`, `02_weak_refs`, `04_destroy_order_and_filters`, `05_double_destroy`).
  - Done when: sequence diagrams for last-ref-on-wrong-thread.
- [ ] **P4-01.3 Thread affinity**
  - Files: `libobs/obs-source.h` (572; per-callback comment blocks), `libobs/obs-video.c` (1200; graphics thread), `libobs/obs-audio.c` (721; audio callback).
  - Steps: extract each callback's thread/lock contract from the header comments; tabulate enter/leave requirement (`gs_enter_context`) for `gs_*` callers, `video_render`, `obs_source_video_render`, `video_tick`, `filter_video`, `audio_render`, `filter_audio`, `update`, `create`.
  - Spec: S-040 §3.1, §7; S-041 §7.
  - Done when: table with enter/leave requirement.
- [ ] **P4-01.4 ADR 0007**
  - Files: none (design artifact; inputs are the P4-01.1–.3 tables).
  - Steps: write `docs/adr/0007-object-model.md` merging the handle, weak-ref and thread-affinity tables (ADR style of `docs/adr/` P0-01…0005); review with someone who knows the C code.
  - Done when: ADR committed with handle, weak-ref and thread-affinity tables.

### P4-02 Handle registry and refcount compat
- [ ] **P4-02.1 addref/release**
  - Files: `libobs/obs-source.c` (6070), `libobs/obs-encoder.c` (2286), `libobs/obs-output.c` (3340), `libobs/obs-service.c` (466).
  - Steps: port `obs_source_get_ref`/`obs_source_release` (no exported `obs_source_addref` in 33 — only an internal helper in `obs-internal.h`) and the equivalent for every kind; expose a debug refcount query; run scripted sequences.
  - Spec: S-040 R07, R10; S-035 §3.3 (service ref API).
  - Vectors: obssource (`01_lifecycle_refcount`), modservice (`03-service-refs`).
  - Done when: refcount observable through a debug function equals C after scripted sequences.
- [ ] **P4-02.2 Weak**
  - Files: `libobs/obs-source.c` (6070), `libobs/obs-encoder.c` (2286), `libobs/obs-output.c` (3340), `libobs/obs-service.c` (466).
  - Steps: port `obs_source_get_weak_source`, `obs_weak_source_get_source`, `obs_weak_source_addref/release`, `obs_weak_source_references_source`, `obs_weak_source_expired` (plus the per-kind equivalents); assert upgrade after destroy returns NULL.
  - Spec: S-040 R10; S-035 §3.3.
  - Vectors: obssource (`02_weak_refs`), modservice (`03-service-refs`).
  - Done when: upgrade after destroy returns NULL.
- [ ] **P4-02.3 Race tests**
  - Steps: model release-vs-upgrade (and release-vs-`get_ref`) races in `loom` over the Arc/Weak mapping; the model must be exhaustive.
  - Spec: S-040 §7.
  - Done when: model passes.

### P4-03 Global context
- [ ] **P4-03.1 `Core`**
  - Files: `libobs/obs.c` (3491), `libobs/obs-internal.h` (1543; `struct obs_core` at line 549).
  - Steps: define `Core` in `crates/obs-core` (S-042 target crate) mirroring the `obs_core` field groups (module lists, type arrays, signals/procs, locale, `video`/`audio`/`data`/`hotkeys`, task handlers, `core_modules_loaded`); own it, no `static mut`.
  - Spec: S-042 §3.1, §5.1 R02.
  - Done when: no `static mut`.
- [ ] **P4-03.2 Startup/shutdown**
  - Files: `libobs/obs.c` (3491; `obs_startup` line 1326, `obs_shutdown` line 1228).
  - Steps: port the startup sequence (data → hotkeys → main canvas → destruction queue; R02) and the full teardown order (R05–R07); double-startup logs + returns false (R01); a failed attempt tears down and leaves nothing (R04).
  - Spec: S-042 §5.1 R01–R08.
  - Vectors: obscore (`010_startup_shutdown`, `011_locale_version_cmdline`, `012_name_store`, `013_startup_failure`, `014_data_paths`).
  - Done when: double-startup returns false as in C.
- [ ] **P4-03.3 Reset video/audio**
  - Files: `libobs/obs.c` (3491; `obs_reset_video` line 1526), `libobs/obs.h` (2734; `struct obs_video_info` line 211, all 13 fields).
  - Steps: port `obs_reset_video` per the §6.1 pseudocode (validation order R20 → tear down R21 → modify ovi in place R22 → graphics R23–R24 → video/mixes R26–R29); port `obs_reset_audio(2)` (R35–R39); return the same `OBS_VIDEO_SUCCESS/NOT_SUPPORTED/INVALID_PARAM/CURRENTLY_ACTIVE/MODULE_NOT_FOUND/FAIL` codes.
  - Spec: S-042 §5.2 R20–R34, §5.3 R35–R39, §6.1.
  - Vectors: obscore (`020_reset_video_validation` … `031_reset_audio_edge`).
  - Pitfalls: S-042 Q-1 — width/height 2 or 3 pass the size check and align to 0, then fail with `FAIL` after teardown (core left without video). Q-5 — 32-bit unsigned wrap of `max_buffering_ms * samples_per_sec` (`031_reset_audio_edge`). Q-10 — the graphics device and effects persist across resets; later `graphics_module`/`adapter` are ignored. D-2 — `fps_num = 0` must return `OBS_VIDEO_INVALID_PARAM`, not SIGFPE; D-3 — `samples_per_sec = 0` must return false, no crash.
  - Done when: invalid combinations return the same codes.
- [ ] **P4-03.4 Leak check**
  - Files: `libobs/obs.c` (3491; C `obs_shutdown` prints nothing), `libobs/util/bmem.c` (164; the `bnum_allocs` counter, S-001).
  - Steps: take an allocator counter baseline at startup (the Phase-1 `crates/obs-util` bmem counter, S-001); at shutdown compare to the baseline and report the delta; add a test with a deliberate leak.
  - Done when: deliberate leak is detected by the check.

### P4-04 Lock inventory and ordering
- [ ] **P4-04.1 Table of 46 mutexes**
  - Files: `libobs/obs-internal.h` (1543; the 46 `pthread_mutex_t` fields).
  - Steps: enumerate every `pthread_mutex_t` field with its struct member name; grep acquire sites to record what each protects; record the nesting order as encoded in the code; commit the table to the ADR (P4-01.4).
  - Spec: S-042 §7, S-041 §7, S-040 §7 (per-subsystem lock lists).
  - Done when: complete.
- [ ] **P4-04.2 Order checker**
  - Steps: implement a debug-only thread-local acquisition stack (assert on every acquire, P4-04.1 order); write a test that acquires in inverted order.
  - Note: no C equivalent and no vectors — this is new tooling the guide requires before other Phase 4 code lands.
  - Done when: inverted acquisition panics in tests.

## B. Sources

### P4-05 Source info vtable adapter
- [ ] **P4-05.1 Trait**
  - Files: `libobs/obs-source.h` (572; `struct obs_source_info`), `libobs/obs-source.c` (6070; how each callback is invoked).
  - Steps: define `trait SourceInfo` in `crates/obs-source` (S-040 target crate) mirroring every `obs_source_info` callback (`get_name`, `create`, `destroy`, `get_width/height`, `get_defaults(2)`, `get_properties(2)`, `update`, `activate/deactivate`, `show/hide`, `video_tick`, `video_render`, `filter_video`, `filter_audio`, `audio_render`, `enum_active_sources`, `save/load`, `mouse_*`, `key_click`, `focus`, `missing_files`, `get_color_space`, `media_*`); build the one-to-one C-field → method table.
  - Spec: S-040 §3.1.
  - Done when: one-to-one table.
- [ ] **P4-05.2 `CSource`**
  - Files: `libobs/obs-source.h` (572), `libobs/obs-module.c` (1274; `obs_register_source_s` at line 1035).
  - Steps: implement the `CSource` adapter storing the C struct copied by `size` bytes (never read past it); register through the R82 path (size > known → reject; required callbacks per flags); register an old-size and a new-size struct in the P0-15 plugin.
  - Spec: S-035 §5.8 R82–R83.
  - Vectors: modservice (`24-register-other-kinds`, `25-register-module-recording`).
  - Pitfalls: S-035 Q-9 — a rejected source that already passed the type/duplicate/size checks stays recorded in the loading module's `sources` list (id recorded before the remaining checks).
  - Done when: old and new-size structs both register.
- [ ] **P4-05.3 Flags**
  - Files: `libobs/obs-source.h` (572; `OBS_SOURCE_*` flag defines at lines 88–209).
  - Steps: define all 19 `output_flags` (plus the 4 `OBS_SOURCE_TYPE_*`): `OBS_SOURCE_VIDEO/AUDIO/ASYNC/ASYNC_VIDEO/CUSTOM_DRAW/INTERACTION/COMPOSITE/DO_NOT_DUPLICATE/DEPRECATED/DO_NOT_SELF_MONITOR/CAP_DISABLED/CAP_DONT_SHOW_PROPERTIES/MONITOR_BY_DEFAULT/CONTROLLABLE_MEDIA/CEA_708/SRGB/CAP_OBSOLETE/REQUIRES_CANVAS/SUBMIX`; write a behavioural test per flag (VIDEO defers `update` R16; ASYNC enables `output_video` R60; COMPOSITE requires `audio_render` and forbids AUDIO|ASYNC R82; INTERACTION gates P4-14; REQUIRES_CANVAS assigns the main canvas; DEPRECATED/CAP_DISABLED hide from enumerations).
  - Spec: S-035 R82 (registration checks), S-040 R02, R16.
  - Pitfalls: S-040 Q-8 — `filter_compatible` masks with `VIDEO|ASYNC|AUDIO`, so a plain VIDEO filter cannot be added to an AUDIO-only source and vice versa (script `21_filter_compat`).
  - Done when: each flag has a behavioural test.
- [ ] **P4-05.4 Threading**
  - Files: `libobs/obs-source.h` (572; the callback comment blocks).
  - Steps: document which callbacks run on graphics/audio/UI threads and with which locks held (from the P4-01.3 table); assert it in the P0-15 host via a thread-id trace.
  - Spec: S-040 §3.1, §7.
  - Done when: documented and asserted.

### P4-06 Source lifecycle
- [ ] **P4-06.1 Create/destroy**
  - Files: `libobs/obs-source.c` (6070; `obs_source_create_internal`).
  - Steps: port `obs_source_create`/`_private` (info copied by value; unknown id still creates a stub-info source so the user's data is not lost R01; `get_defaults` before `create` R04–R05; signal order R06); port destruction: immediate part R08 + deferred part R09 queued to the Phase-1 task queue (S-024 M6).
  - Spec: S-040 §5.1 R01–R09.
  - Vectors: obssource (`00_smoke`, `01_lifecycle_refcount`, `03_create_variants`, `04_destroy_order_and_filters`, `05_double_destroy`).
  - Pitfalls: S-040 Q-1 — `create` returns a source even when the plugin's `create` fails: `data` stays NULL and every later callback is skipped. D-3 — if `get_ref(filter)` fails inside `filter_add` the reference must unlock the filter mutex and return (C leaks the lock).
  - Done when: callback trace equal.
- [ ] **P4-06.2 Update**
  - Files: `libobs/obs-source.c` (6070).
  - Steps: port `obs_source_update` (merge settings; VIDEO-flagged kinds defer to the tick, queued updates collapse to one call R16), `obs_source_update_properties`, `obs_source_properties`, `reset_settings`; settings applied exactly once per tick.
  - Spec: S-040 R16–R17.
  - Vectors: obssource (`62_update_settings`).
  - Done when: settings applied once.
- [ ] **P4-06.3 Save/load**
  - Files: `libobs/obs-source.c` (6070; `obs_source_save`/`obs_source_load`/`obs_load_sources`), `test/fixtures/` (the P0-18 corpus).
  - Steps: port save/load with the full JSON key set (`prev_ver`, `name`, `uuid`, `id`, `versioned_id`, `settings`, `mixers`, `sync`, `flags`, `volume`, `balance`, `enabled`, `muted`, `push-to-mute`/`-delay`, `push-to-talk`/`-delay`, `hotkeys`, `deinterlace_mode`, `deinterlace_field_order`, `monitoring_type`, `monitoring_enabled`, `canvas_uuid`, `private_settings`, `filters`, + `transition_*` keys for transitions); round-trip every P0-18 collection.
  - Note: settings save/load is not in the S-040 spec (§11 "not covered", no vectors) — the C functions are the reference.
  - Done when: corpus round trip.
- [ ] **P4-06.4 Naming/duplicate**
  - Files: `libobs/obs-source.c` (6070; `obs_source_duplicate`), `libobs/obs.c` (3491; name de-dup).
  - Steps: wire unique names/UUIDs through the context layer (S-042 §6.3 de-dup, R70–R71); port `obs_source_duplicate` (R48: filters duplicated oldest first, `name 2`, `name 3`, …) and `obs_source_set_name` (R12).
  - Spec: S-040 R12, R48; S-042 R70–R71, §6.3.
  - Vectors: obssource (`25_filter_copy_duplicate`), obscore (`073_unnamed_objects`, `076_uuid_reset`).
  - Pitfalls: S-042 Q-3 — the rename warning prints the old name (SHOULD match).
  - Done when: tests.

### P4-07 Async video sources
- [ ] **P4-07.1 Frame cache**
  - Files: `libobs/obs-source.c` (6070; `obs_source_output_video(2)`, `async_frames`, `cache`).
  - Steps: port the async frame cache, the `async_frames` queue and the frame selection rules (R60–R62).
  - Spec: S-040 §5.4 R60–R62.
  - Vectors: obssource (`30_async_basic`, `34_async_cache_reuse`, `35_async_queue_limit`, `37_async_formats`).
  - Done when: ordering and drop semantics equal.
- [ ] **P4-07.2 Timing**
  - Files: `libobs/obs-source.c` (6070).
  - Steps: port the `ready(sys)` selector (§6.1), `async_unbuffered`/`async_decoupled` handling and timestamp smoothing (R63–R68); compare a simulated timestamp series against C.
  - Spec: S-040 §6.1, R63–R68.
  - Vectors: obssource (`31_async_timing_select`, `32_async_unbuffered`, `33_async_timestamp_jump`, `40_preload_show`).
  - Pitfalls: S-040 Q-6 — unbuffered async sources keep the whole queue on the very first selection. Q-7 — `set_video_frame`/`preload_video` never set `async_active`; `get_width` stays 0 until a tick/show. Q-5 — `output_video2` with PARTIAL range on an RGB format keeps `full_range = false`, unlike `output_video`.
  - Done when: simulated timestamp series equals C.
- [ ] **P4-07.3 Deinterlace**
  - Files: `libobs/obs-source-deinterlace.c` (480).
  - Steps: port the deinterlace mode/field-order selectors (R100–R101, §6.2) and produce a frame hash per mode; compare to the vectors.
  - Spec: S-040 §5.5, §6.2.
  - Vectors: obssource (`41_deinterlace_config`, `42_deinterlace_frames`, `43_deinterlace_unbuffered`).
  - Note: S-040 §11 — the deinterlace selector is only approximately specified; vectors 42/43 are the reference.
  - Done when: frame hash per mode.
- [ ] **P4-07.4 Get/release**
  - Files: `libobs/obs-source.c` (6070; `obs_source_get_frame`, `release_frame`).
  - Steps: port frame get/release with the §6.3 frame-copy geometry (rows per format) and colour-space conversions (R69–R70).
  - Spec: S-040 R69–R70, §6.3.
  - Vectors: obssource (`38_async_rotation_size`, `39_async_video_filters`).
  - Done when: tests.

### P4-08 Audio on sources
- [ ] **P4-08.1 Output audio**
  - Files: `libobs/obs-source.c` (6070; `obs_source_output_audio`), `libobs/obs-audio.c` (721; the mixer consumes the result — P2-13).
  - Steps: port the source-side ingest: resampler hook R02, conversion R03, balance R04, force-mono R05, `filter_audio` chain R06; test 44.1k→48k through the audiomix runner.
  - Spec: S-032 §5.1 R01–R07.
  - Vectors: audiomix (`30_ingest_timestamps`, `31_ingest_far_timestamp`, `32_ingest_place`, `32b_ingest_place_truncate`, `34_resample_hook`).
  - Done when: tests with 44.1k→48k.
- [ ] **P4-08.2 Monitoring/balance/sync**
  - Files: `libobs/obs-source.c` (6070), `libobs/obs-audio.c` (721).
  - Steps: port the mute predicates and `OBS_MONITORING_TYPE_*` (R22 `source_muted`, §5.4), `obs_source_set_sync_offset` (applied at R12), push-to-mute/push-to-talk state.
  - Spec: S-032 §5.2 R08–R14, §5.4.
  - Vectors: audiomix (`33_sync_offset`, `35_balance_mono`, `37_monitoring`, `38_push_to_talk_mute`).
  - Pitfalls: S-032 Q-1 — the monitoring-silence `muted` is the audio-thread copy, applied only when that source is rendered: a source rendered before it sees the old state for one tick (`37_monitoring`).
  - Done when: golden.
- [ ] **P4-08.3 Capture callbacks**
  - Files: `libobs/obs-source.c` (6070; `obs_source_add_audio_capture_callback`).
  - Steps: port the capture-callback list and the per-ingest invocation (R14: unadjusted data, newest registration first, `muted` per R22, held under `audio_cb_mutex`).
  - Spec: S-032 R14, §7.
  - Vectors: audiomix (`50_volmeter_basic`, `54_volmeter_callbacks` — the volmeter is driven by capture callbacks), audiofilters (`sc_push` command drives them on a source).
  - Done when: callbacks fire per tick with expected data.

### P4-09 Filters
- [ ] **P4-09.1 Chain**
  - Files: `libobs/obs-source.c` (6070; the filter list ops).
  - Steps: port `obs_source_filter_add/remove/set_order/set_index/get_index/count/enum`, `obs_source_get_filter_by_name`, `obs_filter_get_parent`, `obs_filter_get_target`, `copy_filters`/`copy_single_filter` (R40–R46); recompute `target` after every structural change.
  - Spec: S-040 §5.3 R40–R46.
  - Vectors: obssource (`20_filter_add_remove`, `21_filter_compat`, `22_filter_order`, `22b_filter_order_mixed`, `23_filter_index`, `24_filter_by_name_enum`).
  - Pitfalls: S-040 Q-8 — compatibility mask `VIDEO|ASYNC|AUDIO` (R42). D-1 — `filter_set_index` with `index >= count` corrupts the reference; clamp to `count - 1`. D-4 — adding a filter that already belongs to another source must be refused with a WARNING (C silently leaves it in two arrays).
  - Done when: order test.
- [ ] **P4-09.2 Processing**
  - Files: `libobs/obs-source.c` (6070; `obs_source_process_filter_*`, `obs_source_skip_video_filter`).
  - Steps: port `obs_source_process_filter_begin/end/tech_end` and `obs_source_skip_video_filter`; pass filters without a `filter_video` callback through unchanged (there is no `obs_source_default_filter_*` in 33); golden-render via the P3-26 static-scene harness.
  - Note: S-040 §11 — `obs_source_process_filter_*` results are not vectored; golden against the C reference.
  - Done when: golden render.
- [ ] **P4-09.3 Audio filters**
  - Files: `libobs/obs-source.c` (6070), `libobs/obs-audio.c` (721).
  - Steps: wire the `filter_audio` callback chain into ingest (R06: iterate last→first, skip disabled and filterless, NULL drops the packet) and into the P2-13 mixer.
  - Spec: S-032 R06.
  - Vectors: audiomix (`36_filter_audio`), audiofilters (15 scripts; the `filter_audio` callback is the filter driver).
  - Done when: golden.

### P4-10 Source render path
- [ ] **P4-10.1 Render**
  - Files: `libobs/obs-source.c` (6070; `obs_source_video_render`, `obs_source_default_render`), `libobs/obs-video.c` (1200; the mix hookup, P3-26).
  - Steps: port the default-render effect selection and the source draw; wire into the P3-26 render loop (whose scene walk was deferred to Phase 4); golden-frame compare against C.
  - Note: S-040 §11 — GPU rendering and effect paths are not vectored; golden against C via the P3-26 harness.
  - Done when: golden frame.
- [ ] **P4-10.2 Texrender**
  - Files: `libobs/obs-source.c` (6070), `libobs/graphics/texture-render.c` (151 — P3-11.1).
  - Steps: port the texrender caching for filtered sources (render into the cached target when filters exist, reuse the texture) and the direct-draw path under `OBS_SOURCE_CUSTOM_DRAW`.
  - Done when: texture reuse test.
- [ ] **P4-10.3 Composite**
  - Files: `libobs/obs-source.c` (6070).
  - Steps: port `OBS_SOURCE_COMPOSITE` handling and the size rules: `obs_source_get_width/height`, `obs_source_get_base_width/height` (R25–R27 active-child sizes, R47 `base()`).
  - Spec: S-040 R25–R27, R47.
  - Vectors: obssource (`14_activation_tree`, `15_active_child_api`, `27_filter_size_resolution`, `61_tr_activation_tree`).
  - Done when: tests.

### P4-11 Transitions
- [ ] **P4-11.1 State**
  - Files: `libobs/obs-source-transition.c` (1072).
  - Steps: port `obs_transition_start/set/clear/get_time` and the duration modes `OBS_TRANSITION_MODE_AUTO/MANUAL` (R80–R87); timeline test.
  - Spec: S-040 §5.6 R80–R87.
  - Vectors: obssource (`50_tr_set_clear`, `51_tr_start_stop`, `52_tr_start_edge_cases`, `55_tr_manual_mode`).
  - Pitfalls: S-040 Q-3 — committing a running transition from `start`/`set` sends the stop signals but not the plugin's `transition_stop` callback. Q-4 — the "active, manual, same mode, same destination" early return is dead code: restarting towards the current destination is refused (returns false) after committing.
  - Done when: timeline test.
- [ ] **P4-11.2 Mix callbacks**
  - Files: `libobs/obs-source-transition.c` (1072).
  - Steps: port `transition_video_render`, the A/B slot handling and matrices (R88–R91: `set_size` else `actual_size`, MAX_ONLY scaling); frames at t=0, 0.5, 1 must equal C.
  - Spec: S-040 R88–R91.
  - Vectors: obssource (`53_tr_video_stop`, `54_tr_video_direct`, `58_tr_scale_align`, `59_tr_save_load_swap`).
  - Done when: frames at t=0,0.5,1 equal.
- [ ] **P4-11.3 Audio**
  - Files: `libobs/obs-source-transition.c` (1072; `obs_transition_audio_render`).
  - Steps: port `obs_transition_audio_render` and the `mix_a`/`mix_b` mix callbacks (R89–R90); PCM golden.
  - Spec: S-040 R89–R90.
  - Vectors: obssource (`56_tr_audio_mix`, `57_tr_audio_idle`).
  - Pitfalls: S-040 Q-9 — transition audio mixing reads the child's samples from index 0 while writing at an offset (SHOULD match, `56_tr_audio_mix`).
  - Done when: PCM golden.
- [ ] **P4-11.4 Helpers**
  - Files: `libobs/obs-source-transition.c` (1072).
  - Steps: port the T-bar `obs_transition_set_manual_time`/`set_manual_torque` and the duration-mode/scale/alignment helpers (R91–R94); (stingers are a frontend concept — a transition over a video source; libobs has no stinger code, the stinger plugin is S-029/P5).
  - Spec: S-040 R91–R94.
  - Vectors: obssource (`55_tr_manual_mode`, `58_tr_scale_align`, `59_tr_save_load_swap`).
  - Done when: tests.

### P4-12 Source signals and procs
- [ ] **P4-12.1 List**
  - Files: `libobs/obs-source.c` (6070; the `source_signals` array passed to `signal_handler_add_array`), `libobs/obs.h` (2734; signal name strings).
  - Steps: extract every signal name and its exact parameter names (source + global lists) and commit them as a table.
  - Spec: S-040 §3.3 (exact text, order significant).
  - Done when: list committed.
- [ ] **P4-12.2 Signals**
  - Files: `libobs/obs-source.c` (6070).
  - Steps: implement all signals on the Phase-1 `crates/obs-signal` handlers (S-004); global signal first, source signal second; global only for non-private sources except `source_filter_add/remove` (unconditional); table test emits each with its parameters.
  - Spec: S-040 §3.3; S-004.
  - Vectors: obssource (every script emits; e.g. `06_remove_rename_enable`, `20_filter_add_remove`, `26_filter_enable_and_tick`), signal (17 scripts with the `emit` command).
  - Done when: table test emits each.
- [ ] **P4-12.3 Procs**
  - Files: `libobs/obs-source.c` (6070; `obs_source_get_proc_handler`).
  - Steps: expose the per-source `proc_handler_t` (S-004 procs) through the ABI; in the P0-15 plugin register a proc (obs-ffmpeg-style `restart`/`get_duration`) and call it through `proc_handler_call` (`get_hotkey_bindings`/`get_hotkey_names` do not exist in 33.0.1).
  - Spec: S-004 (procs).
  - Vectors: signal (`proc_add`/`proc_call` commands).
  - Done when: table test with a plugin-registered proc.

### P4-13 Source types registry
- [ ] **P4-13.1 Registration**
  - Files: `libobs/obs-module.c` (1274; `obs_register_source_s` line 1035), `libobs/obs.c` (3491; the `obs_enum_*` entry points).
  - Steps: port `obs_register_source_s` (R82: type check, unversioned-id duplicate check, versioned id storage `"<id>_v<version>"`) and `obs_enum_source_types`, `obs_enum_input_types(2)`, `obs_enum_filter_types`, `obs_enum_transition_types` (R72–R73); compare enumeration order against C.
  - Spec: S-035 R82; S-042 R72–R73.
  - Vectors: modservice (`24-register-other-kinds`, `25-register-module-recording`), obscore (`070_registry_types`, `071_object_enumeration`).
  - Done when: enumeration order equal.
- [ ] **P4-13.2 Metadata**
  - Files: `libobs/obs-module.c` (1274).
  - Steps: port `obs_source_get_display_name`, `obs_source_get_icon_type` and the DEPRECATED/CAP_DISABLED filtering of enumerations (R72).
  - Spec: S-042 R72.
  - Vectors: obscore (`071_object_enumeration`, `072_lookup`).
  - Done when: tests.

### P4-14 Interaction API
- [ ] **P4-14.1 Types**
  - Files: `libobs/obs-interaction.h` (56).
  - Steps: port the event structs (`obs_source_mouse_info`, `obs_source_key_info`) as `#[repr(C)]`; add their layout assertions to P4-35.1.
  - Note: no spec (S-040 §11 leaves interaction events uncovered) — the header is the reference.
  - Done when: layout test.
- [ ] **P4-14.2 Routing**
  - Files: `libobs/obs-interaction.h` (56), `libobs/obs-source.c` (6070; `obs_source_send_*`).
  - Steps: port `obs_source_send_mouse_click/move/wheel`, `obs_source_send_key_click`, `obs_source_send_focus` routing to the INTERACTION-flagged source's `mouse_*`/`key_click`/`focus` callbacks.
  - Note: no spec (S-040 §11) — read the C.
  - Done when: events reach C source.

### P4-15 Missing-files tracking
- [ ] **P4-15.1 Tracker**
  - Files: `libobs/obs-missing-files.c` (142), `libobs/obs-missing-files.h` (53).
  - Steps: port `obs_missing_files_create/add_file/count/get_file/destroy/append` to `crates/obs-missing-files` (S-024 M5).
  - Spec: S-024 (missing-files module).
  - Vectors: helpers (`missing-files-basic`, `missing-files-append`, `missing-files-append-order`, `missing-files-release`).
  - Done when: add/remove/count tests.
- [ ] **P4-15.2 Callback**
  - Files: `libobs/obs-missing-files.c` (142), `libobs/obs-source.c` (6070; the `missing_files` info callback hookup).
  - Steps: port `obs_missing_file_create(path, callback, src_type, …)` and `obs_missing_file_issue_callback` (how the frontend learns of replaced files).
  - Spec: S-024 (missing-files module).
  - Vectors: helpers (`missing-files-callback`, `missing-files-quirk-fields`).
  - Done when: callback fires with the new path.

## C. Scenes

### P4-16 Scene core
- [ ] **P4-16.1 Scene as source**
  - Files: `libobs/obs-scene.c` (4144), `libobs/obs-scene.h` (116).
  - Steps: port `obs_scene_create`/`_private` (a scene is a source of type SCENE registered via R82) and `obs_scene_get_source`; scene size from its canvas (R01–R04).
  - Spec: S-033 §5.1 R01–R04.
  - Vectors: scenemath (`canvas_sources`, `scene_item_size`, `harness_canvas_mix_only`).
  - Done when: scene usable as input.
- [ ] **P4-16.2 Items**
  - Files: `libobs/obs-scene.c` (4144).
  - Steps: port `obs_scene_add`, `obs_sceneitem_remove` (R51) and the reorder ops `set_order`, `set_order_position`, `reorder(2)` (R60–R64).
  - Spec: S-033 R51, R60–R64.
  - Vectors: scenemath (`order`, `order_position`, `reorder`, `reorder2`, `select_lock_remove`).
  - Pitfalls: S-033 Q-2 — `set_order_position(n)` for `n < 0` equals `n == 1`. Q-4/D-4 — `reorder_items2` accepts an incomplete list and orphans the unlisted items (DEFECT: `quirk_reorder2_partial_list`).
  - Done when: order tests.
- [ ] **P4-16.3 Transform**
  - Files: `libobs/obs-scene.c` (4144).
  - Steps: port pos/rot/scale/bounds/bounds_type/alignment/crop setters and `obs_sceneitem_set_info2` (R05–R08, R20–R23; matrix per §6.1); round-trip test.
  - Spec: S-033 R05–R08, R20–R23.
  - Vectors: scenemath (`abs_basic`, `rel_basic`, `rotation`, `crop`, `info2`).
  - Done when: round trip.
- [ ] **P4-16.4 Visual flags**
  - Files: `libobs/obs-scene.c` (4144).
  - Steps: port scale filter, blend method/type, locked, visible (R23, R50–R52); the collection-level `AbsoluteCoordinates` private setting (core data in `obs-scene.c`) switches the pos/scale conversion between relative (R31) and absolute (R05).
  - Spec: S-033 R05, R23, R31, R50–R52.
  - Vectors: scenemath (`abs_basic`, `rel_basic`, `visibility`, `select_lock_remove`).
  - Done when: golden.
- [ ] **P4-16.5 Show/hide transitions**
  - Files: `libobs/obs-scene.c` (4144).
  - Steps: port `obs_sceneitem_set_transition(item, show, transition)`, `set_transition_duration`, `obs_sceneitem_do_transition` (R53: force-stop active first, start transition before the visibility flag changes) and `obs_sceneitem_transition_load`.
  - Spec: S-033 R53.
  - Vectors: scenemath (`transitions`).
  - Done when: test.

### P4-17 Groups and nested scenes
- [ ] **P4-17.1 Groups**
  - Files: `libobs/obs-scene.c` (4144).
  - Steps: port `obs_scene_add_group(2)`, `obs_sceneitem_group_add_item`, `obs_sceneitem_group_ungroup(2)`, `obs_sceneitem_group_enum_items` (R40–R48; group transform conversion §6.4, resize §6.5).
  - Spec: S-033 R40–R48.
  - Vectors: scenemath (`group_basic`, `group_move`, `group_membership`, `group_ungroup`, `group_resize_defer`, `group_precision`).
  - Pitfalls: S-033 Q-5 — after the last child leaves a group the group keeps a stale transform state (`quirk_stale_group_after_removal`). Q-7 — moving an item into a group by insert snaps its scale and position once (`quirk_group_insert_nudge`).
  - Done when: tests.
- [ ] **P4-17.2 Nested**
  - Files: `libobs/obs-scene.c` (4144).
  - Steps: port scene-in-scene rendering: the nested-scene crop adjustment (R32) and the nested transform chain; golden-render.
  - Spec: S-033 R32.
  - Note: GPU paths are not vectored (S-040 §11) — golden via the P3-26 harness.
  - Done when: golden.
- [ ] **P4-17.3 Cycles**
  - Files: `libobs/obs-source.c` (6070; `obs_source_add_active_child`).
  - Steps: port infinite-source-recursion detection (self/ancestor reference) in `obs_source_add_active_child`; the add fails with a log.
  - Spec: S-040 R25–R27.
  - Vectors: obssource (`12_activation_cycle_in_same_tick`, `14_activation_tree`, `61_tr_activation_tree`).
  - Done when: add of parent fails.

### P4-18 Scene rendering
- [ ] **P4-18.1 Matrix**
  - Files: `libobs/obs-scene.c` (4144; `update_item_transform`).
  - Steps: port the 12-step update pass (R30) and the matrix computation (R31–R36) into `crates/obs-scene-math` (S-033 target crate — pure, no graphics); 50 cases equal to C.
  - Spec: S-033 R30–R36, §6.1.
  - Vectors: scenemath (40 scripts).
  - Pitfalls: S-033 Q-3 — alignment shifts for flipped items use the signed truncated pixel size (negative) (`quirk_negative_scale_align`).
  - Done when: 50 cases equal to C.
- [ ] **P4-18.2 Crop/bounds**
  - Files: `libobs/obs-scene.c` (4144).
  - Steps: port all `obs_bounds_type` modes (NONE/CENTER/INSIDE/OUTSIDE) with the bounds step (§6.3) and `nested_crop` (§6.7); golden.
  - Spec: S-033 R33, §6.3, §6.7.
  - Vectors: scenemath (`bounds_types`, `bounds_crop`, `bounds_flip`, `crop`, `bounds_canvas_resize`).
  - Done when: golden.
- [ ] **P4-18.3 Queries**
  - Files: `libobs/obs-scene.c` (4144).
  - Steps: port `obs_sceneitem_get_box_transform`, `obs_sceneitem_get_box_scale` (R36) and `obs_sceneitem_get_draw_transform` (R35); matrix equality.
  - Spec: S-033 R35–R36.
  - Vectors: scenemath (the transform dumps in the scripts above).
  - Done when: matrix equality.
- [ ] **P4-18.4 Goldens**
  - Steps: commit the 50 transform cases (transform/box/draw matrices from the C runner) as goldens; run them against the Rust port.
  - Vectors: scenemath.
  - Done when: 50 golden cases committed and passing.

### P4-19 Scene save/load
- [ ] **P4-19.1 Item JSON**
  - Files: `libobs/obs-scene.c` (4144; the scene item save/load), `test/fixtures/` (the P0-18 corpus).
  - Steps: port item save/load with the key set (`name`, `source_uuid`, `visible`, `locked`, `rot`, `pos`, `scale`, `align`, `bounds`, `crop_*`, `blend_type`, `scale_filter`, `group_item_backup`, `show_transition`, …); unknown keys must be preserved.
  - Note: S-033 §11 — transform-field serialisation (`pos_rel`, `scale_ref`, …) is not specified there; the C functions are the reference.
  - Done when: unknown keys preserved.
- [ ] **P4-19.2 IDs/ordering**
  - Files: `libobs/obs-scene.c` (4144).
  - Steps: port stable scene-item `id` allocation (§6.9) and `private_settings` round-trip.
  - Spec: S-033 §6.9.
  - Vectors: scenemath (`prune`, `order`).
  - Done when: stable ids.
- [ ] **P4-19.3 Corpus round trip**
  - Steps: every P0-18 collection in `test/fixtures/` round-trips: load → save → reload with stable ids and byte-stable second save.
  - Done when: P0-18 corpus round-trips with stable ids.

### P4-20 Scene audio mixing
- [ ] **P4-20.1 Compose**
  - Files: `libobs/obs-scene.c` (4144; `scene_audio_render`), `libobs/obs-audio.c` (721; the scene source enters the mixer as a source).
  - Steps: port the scene-source audio composition (child mixing in `audio_render`, audio actions R54); golden PCM.
  - Spec: S-032 §5.6 (the scene is a mixer source), S-033 R54.
  - Vectors: audiomix (`14_render_order`, `16_submix`).
  - Done when: golden PCM.
- [ ] **P4-20.2 Hidden items**
  - Files: `libobs/obs-scene.c` (4144).
  - Steps: port the visibility→audio gating in `scene_audio_render`: a non-visible item contributes nothing, except while its hide transition is active (R50, R54).
  - Spec: S-033 R50, R54.
  - Vectors: scenemath (`visibility`, `visibility_recursion`, `visibility_scene_groups`) + audiomix golden for the audio side.
  - Done when: test.

## D. Encoders, outputs, services

### P4-21 Encoder vtable adapter
- [ ] **P4-21.1 Trait/adapter**
  - Files: `libobs/obs-encoder.h` (365; `obs_encoder_info`), `libobs/obs-encoder.c` (2286).
  - Steps: define `trait EncoderInfo` + the `CEncoder` adapter (adapter pattern) for `obs_encoder_info` callbacks (`create`, `destroy`, `encode`, `encode_texture(2)`, `get_defaults`, `get_properties`, `update`, `get_extra_data`, `get_sei_data`, `get_audio_info/video_info`, `get_frame_size`, `get_name`); register through R81; the P0-15 ABI test encoder works.
  - Spec: S-041 §5.1, S-035 R81.
  - Vectors: modservice (`24-register-other-kinds`), encoutput (`enc_create`).
  - Pitfalls: S-035 R81 — `PASS_TEXTURE`+`SCALING` caps together are rejected with a WARNING; `PASS_TEXTURE` requires one of `encode_texture(2)`, else `encode`; audio encoders additionally need `get_frame_size`.
  - Done when: ABI test encoder works.
- [ ] **P4-21.2 Encode path**
  - Files: `libobs/obs-encoder.c` (2286), `libobs/obs-output.c` (3340; `send_off_encoder_packet`).
  - Steps: call `encode`/`encode_texture(2)` from the graphics thread (raw video connection) and the audio thread (mixer connection) — C has no dedicated encoder thread; deliver packets to outputs under the callback/output locks (§7); compare the call-site thread trace against C.
  - Spec: S-041 R05–R12, §7.
  - Vectors: encoutput (`enc_flow_sync`, `enc_packet_path`, `enc_two_outputs`, `enc_hold_bframes`, `enc_errors`).
  - Builds on: P2-22 (packet queue), P2-24 (interleaver) in `crates/obs-encoder-output`.
  - Done when: call-site thread trace equal.
- [ ] **P4-21.3 Texture path**
  - Files: `libobs/obs-encoder.c` (2286; `encode_texture(2)`), `libobs/obs-video-gpu-encode.c` (312).
  - Steps: port `encode_texture` taking the `gs_texture` handle via the P3-21 native export; test with a mocked texture encoder.
  - Note: S-041 §11 — GPU texture encoding is not vectored; mock acceptance.
  - Done when: mocked texture encoder.
- [ ] **P4-21.4 Scaling**
  - Files: `libobs/obs-encoder.c` (2286).
  - Steps: port `obs_encoder_set_scaled_size` (ignored with warning when non-video/active/initialised; equal-to-output-size disables scaling), `set_gpu_scale_type`, `obs_encoder_scaling_enabled` (R03).
  - Spec: S-041 R03.
  - Vectors: encoutput (`enc_media_config`).
  - Done when: size test.

### P4-22 Encoder lifecycle and signals
- [ ] **P4-22.1 Active state**
  - Files: `libobs/obs-encoder.c` (2286).
  - Steps: port `obs_encoder_active`, `obs_encoder_paused`, the internal `obs_encoder_initialize` (called from `obs_output_start`; no public init/uninit; R04) and `obs_encoder_set_preferred_video_format` (R02); state-diagram test.
  - Spec: S-041 R02, R04.
  - Vectors: encoutput (`enc_create`, `enc_media_config`, `out_start_failures`).
  - Done when: state diagram test.
- [ ] **P4-22.2 Sharing**
  - Files: `libobs/obs-encoder.c` (2286).
  - Steps: port the packet fan-out: public `obs_encoder_packet_ref/release`, internal `obs_encoder_packet_create_instance` (long counter in front of the data; interleaving and delay copy the packet; §3.3).
  - Spec: S-041 §3.3.
  - Vectors: encoutput (`enc_packet_path`, `enc_two_outputs`).
  - Done when: refcount check.
- [ ] **P4-22.3 ROI**
  - Files: `libobs/obs-encoder.c` (2286; `obs_encoder_add_roi` … `obs_encoder_enum_roi`).
  - Steps: port `obs_encoder_add_roi`, `obs_encoder_has_roi`, `obs_encoder_clear_roi`, `obs_encoder_enum_roi`, `obs_encoder_get_roi_increment` (gated by the ROI encoder cap).
  - Note: S-041 §11 — ROI is not specified; read the C.
  - Done when: tests.

### P4-23 Output vtable adapter
- [ ] **P4-23.1 Trait/adapter**
  - Files: `libobs/obs-output.h` (96; `obs_output_info`), `libobs/obs-output.c` (3340).
  - Steps: define `trait OutputInfo` + the `COutput` adapter (adapter pattern) for `obs_output_info` callbacks (`create`, `destroy`, `update`, `get_defaults`, `get_name`, `start`, `stop`, `raw_video`, `raw_audio(2)`, `encoded_packet`, `get_properties`, `get_total_bytes`, `get_dropped_frames`, `get_congestion`, `get_connect_time_ms`, codec strings, `protocols` — there is no `get_stats` field in 33); register through R80; the P0-15 ABI test output works.
  - Spec: S-041 §5.2 R20–R21, S-035 R80.
  - Vectors: modservice (`24-register-other-kinds`), encoutput (`out_guards`, `start_basic`).
  - Pitfalls: S-035 R80 — required callbacks depend on the flags: SERVICE also needs `protocols`; ENCODED also `encoded_packet`; otherwise VIDEO → `raw_video`, AUDIO → `raw_audio2` if MULTI_TRACK else `raw_audio`.
  - Done when: ABI test output works.
- [ ] **P4-23.2 Data capture**
  - Files: `libobs/obs-output.c` (3340; `obs_output_begin_data_capture`).
  - Steps: port `obs_output_begin_data_capture`/`end_data_capture` with flags `OBS_OUTPUT_VIDEO/AUDIO/ENCODED/SERVICE` (R22 `can_begin_data_capture`, R23 `initialize_encoders`, R26 capture wiring: pairing, data hook, batch size, service activate, `activate` signal).
  - Spec: S-041 R22–R27.
  - Vectors: encoutput (`start_basic`, `out_start_stop`, `out_guards`).
  - Pitfalls: S-041 D-2 — `begin_data_capture` on an A+V output whose audio encoder was never initialised divides by zero in C (`frame_size == 0`); treat interval-0 as contributing nothing or return false, do not crash.
  - Done when: raw and encoded flows.
- [ ] **P4-23.3 Reconnect**
  - Files: `libobs/obs-output.c` (3340; `output_reconnect`).
  - Steps: port `obs_output_set_reconnect_settings`, the retry loop with exponential back-off (exponent `1.5 + rand*0.05`, R20 defaults, R60–R63) and `reconnect_retry_*`; fake-clock test.
  - Spec: S-041 §5.5 R60–R63.
  - Vectors: encoutput (`rc_backoff`, `rc_backoff_cap`, `rc_success`, `rc_veto_and_stop`).
  - Pitfalls: S-041 D-3 — two reconnect threads can exist in C (a failing plugin start inside the reconnect thread spawns the next thread and overwrites the stored handle); track each attempt separately and join/detach exactly once.
  - Done when: fake-clock test.
- [ ] **P4-23.4 Delay/stats**
  - Files: `libobs/obs-output.c` (3340), `libobs/obs-output-delay.c` (209 — the delay queue is P2-23).
  - Steps: port `obs_output_set_delay`/`get_delay`/`get_active_delay` (R70) and delayed start/stop/force-stop/reconnect (R71–R75) on top of the P2-23 queue; port the byte/frame counters.
  - Spec: S-041 §5.6 R70–R75.
  - Vectors: encoutput (`dl_start`, `dl_stop_drop`, `dl_force_stop`, `dl_reconnect_preserve`, `dl_reconnect_nopreserve`).
  - Pitfalls: S-041 Q-9 — a delayed stop never completes without further packets (no timer; `dl_stop_drop` only completes because a later packet arrives). Q-8 — `force_stop` of a delayed output emits the `stop` signal twice. Q-2 — after the interleaver starts, `dts_usec` is recomputed from `dts` alone and no longer contains `start_ts`.
  - Done when: tests.

### P4-24 Output flags and multi-track
- [ ] **P4-24.1 Flags**
  - Files: `libobs/obs-output.h` (96; flag defines at lines 20–34).
  - Steps: define `OBS_OUTPUT_MULTI_TRACK` (= `MULTI_TRACK_AUDIO`, 16), `OBS_OUTPUT_MULTI_TRACK_VIDEO` (64), `OBS_OUTPUT_MULTI_TRACK_AV` (composite 16|64), `OBS_OUTPUT_CAN_PAUSE` (32); one behavioural test per flag.
  - Spec: S-041 §3.1.
  - Vectors: encoutput (`il_multitrack_video`, `il_multitrack_audio`).
  - Done when: each tested.
- [ ] **P4-24.2 Per-track encoders**
  - Files: `libobs/obs-output.c` (3340), `libobs/obs-output.h` (96; `MAX_OUTPUT_AUDIO_ENCODERS`/`MAX_OUTPUT_VIDEO_ENCODERS` at lines 36–37).
  - Steps: port `obs_output_set_audio_encoder(output, enc, idx)` (index > 0 ignored without the matching multi-track flag; R21) and the pairing with the first video encoder (R27).
  - Spec: S-041 R21, R27.
  - Vectors: encoutput (`il_multitrack_audio`).
  - Done when: 6 tracks.
- [ ] **P4-24.3 Service**
  - Files: `libobs/obs-output.c` (3340; `obs_output_set_service`).
  - Steps: port `obs_output_set_service` so the service's URL/key reach the output (activate on capture, §5.5 of S-035).
  - Spec: S-035 R40–R43.
  - Vectors: modservice (`04-service-output`).
  - Done when: URL/key reach output.

### P4-25 Service API
- [ ] **P4-25.1 Adapter**
  - Files: `libobs/obs-service.h` (115; `obs_service_info`), `libobs/obs-service.c` (466).
  - Steps: define `trait ServiceInfo` + the `CService` adapter (adapter pattern) for `obs_service_info` callbacks (`create`, `destroy`, `activate`, `deactivate`, `update`, `get_defaults`, `get_properties`, `initialize`, `get_url`, `get_key`, `get_username`, `get_password`, `apply_encoder_settings`, `get_supported_video_codecs`, `get_protocol`, `get_connect_info`, `can_try_to_connect`); register through R01–R04; the P0-15 ABI test service works.
  - Spec: S-035 §3.2–3.3, §5.1–5.3.
  - Vectors: modservice (`01-service-register`, `02-service-create-update`, `06-service-null-and-defects`).
  - Pitfalls: S-035 Q-1 — `create` returning NULL does not fail creation: the object exists with `data == NULL`, `update`/`activate` are still called with NULL data, `destroy` is not called. Q-13 — `obs_service_update(s, NULL)` still invokes the plugin's `update`.
  - Done when: ABI test service.
- [ ] **P4-25.2 URL/key**
  - Files: `libobs/obs-service.c` (466).
  - Steps: port `obs_service_get_connect_info(type)` per `OBS_SERVICE_CONNECT_INFO_*` type (R26–R30 optional-callback getters).
  - Spec: S-035 R26–R30.
  - Vectors: modservice (`05-service-queries`).
  - Pitfalls: S-035 D-1 — `obs_service_get_supported_{video,audio}_codecs(NULL)` dereferences NULL in C; required: debug log + return NULL.
  - Done when: tests.
- [ ] **P4-25.3 Encoder settings**
  - Files: `libobs/obs-service.c` (466).
  - Steps: port `obs_service_apply_encoder_settings` (plugin clamps the requested bitrate against `obs_service_get_max_bitrate`); bitrate-clamp test.
  - Spec: S-035 §3.3.
  - Vectors: modservice (`05-service-queries`).
  - Done when: bitrate clamp test.

### P4-26 Multitrack video plumbing
- [ ] **P4-26.1 Connect info**
  - Files: `libobs/obs-service.h` (115; the `OBS_SERVICE_CONNECT_INFO_*` enum).
  - Steps: port every `OBS_SERVICE_CONNECT_INFO_*` value with its string mapping.
  - Spec: S-035 §3.3.
  - Vectors: modservice (`05-service-queries`).
  - Done when: all values.
- [ ] **P4-26.2 Per-track encoders**
  - Files: `libobs/obs-output.c` (3340), `libobs/obs-output.h` (96).
  - Steps: port `obs_output_set_video_encoder2`/`obs_output_get_video_encoder2` (10 video tracks) and `obs_output_set_audio_encoder`/`obs_output_get_audio_encoder` (6 audio tracks; the per-track state arrays in `obs-internal.h` lines 1261–1262).
  - Spec: S-041 R21, R27, §5.4 R42–R47.
  - Vectors: encoutput (`il_multitrack_video`, `il_multitrack_audio`, `il_steady_state`).
  - Done when: tests.
- [ ] **P4-26.3 DBR out of scope**
  - Files: `libobs/obs-encoder.h` (365; the `DYN_BITRATE` cap is declared but libobs has no API for it), `plugins/obs-outputs/rtmp-stream.c` (where dynamic bitrate actually lives).
  - Steps: verify libobs 33.0.1 has no dynamic-bitrate API (grep `dynamic_bitrate` under `libobs/` — no core entry points); note in the P4-42 review that the Rust port inherits the same split.
  - Done when: note committed.

### P4-27 Output state machine tests
- [ ] **P4-27.1 State table**
  - Files: `libobs/obs-output.c` (3340), `libobs/obs-internal.h` (1543; the output state fields at lines 1255–1290).
  - Steps: enumerate starting/active/stopping/reconnecting/paused/delay and list every legal transition from S-041 §5.2–5.6; commit the table.
  - Spec: S-041 §3.6, §5.2–5.6.
  - Done when: table lists every legal transition.
- [ ] **P4-27.2 Signal order**
  - Files: `libobs/obs-output.c` (3340).
  - Steps: port the signal emissions in their exact order (`activate`, `starting`, `start`, `stopping`, `stop`, `reconnect`, `reconnect_success`, `pause`, `unpause`; §3.5) and table-test the expected sequences.
  - Spec: S-041 §3.5.
  - Vectors: encoutput (`out_start_stop`, `rc_success`, `out_signal_stop`, `rc_veto_and_stop`).
  - Done when: expected sequences.
- [ ] **P4-27.3 Failure/force-stop**
  - Files: `libobs/obs-output.c` (3340).
  - Steps: port `obs_output_force_stop` and `obs_output_signal_stop` with the stop codes `OBS_OUTPUT_SUCCESS` = 0 … `HDR_DISABLED` = -9 (obs-defs.h, §3.1; R30–R37).
  - Spec: S-041 §3.1, R30–R37.
  - Vectors: encoutput (`out_signal_stop`, `out_start_failures`, `dl_force_stop`).
  - Pitfalls: S-041 Q-3 — `stop_code` is reset to 0 right after the `stop` signal, so `state` after `signal_stop(-7)` shows 0; the code is only visible in the signal. Q-4 — `force_stop` on an idle output emits `stopping`, calls the plugin `stop(0)`, and emits `stop(0)` although nothing was started; plain `stop` on an idle output does nothing.
  - Done when: tests.

## E. Hotkeys, modules, display

### P4-28 Hotkey system
- [ ] **P4-28.1 Registration**
  - Files: `libobs/obs-hotkey.c` (1400), `libobs/obs-hotkey.h` (272).
  - Steps: port `obs_hotkey_register_frontend/source/output/encoder/service` and the `unregister` variants (R02–R05; unregister R10–R13) into `crates/obs-hotkeys` (S-034 target crate); ids stable across registration order.
  - Spec: S-034 §5.2 R02–R05, §5.4 R10–R13.
  - Vectors: hotkeys (`01-register-basic`, `02-register-contexts`, `11-lifecycle`).
  - Pitfalls: S-034 Q-7 — after id exhaustion a real hotkey is registered with id `INVALID`; callers cannot tell it from a failure (SHOULD match; MAY use u64 ids). D-1 — unregistering a hotkey whose binding is pressed: exactly one `(pressed=false)` callback with the valid id, delivered after `hotkey_unregister` and before the binding disappears; the hotkey must stay alive until then (C has a use-after-free here).
  - Done when: ids stable.
- [ ] **P4-28.2 Bindings**
  - Files: `libobs/obs-hotkey.c` (1400), `libobs/obs-hotkeys.h` (655; the key names).
  - Steps: port `obs_hotkey_load_bindings` (raw combinations), `obs_hotkey_load` (JSON array, §4.2) and `obs_hotkeys_load_source/output/encoder/service` (context format, §4.1); combos with modifiers; JSON round trip.
  - Spec: S-034 §4.1–4.2, §5.5 R14–R18.
  - Vectors: hotkeys (`03-bindings-load-save`, `10-context-data`, `12-quirks-empty-bindings`, `14-replace-while-pressed`, `15-extra-modifier-bits`).
  - Pitfalls: S-034 Q-1 — `{}`/unknown key name without modifiers still creates a binding {mods 0, key NONE}, saved back as `{"key":"OBS_KEY_NONE"}` (saved profiles round-trip depend on it). Q-3 — context load appends instead of replacing (MUST match). Q-5 — `hotkey_load` with an empty/NULL array removes bindings without a signal; `load_bindings` signals when something was removed.
  - Done when: JSON round trip.
- [ ] **P4-28.3 Thread and routing**
  - Files: `libobs/obs-hotkey.c` (1400; `obs_hotkey_thread`).
  - Steps: port the hotkey thread polling step (R25–R27), press/release dispatch (R22–R24) and the push-to-talk helpers `obs_hotkey_pair_*` (R06, R29–R30); simulated key-event test.
  - Spec: S-034 §5.7–5.11 R22–R30.
  - Vectors: hotkeys (`04-poll-plain-keys`, `05-poll-modifiers`, `06-inject`, `07-pairs`, `08-routing`).
  - Pitfalls: S-034 Q-2 — `inject_event(pressed=false)` is a no-op for already-pressed bindings (MUST match). Q-4 — the blocked pair side delivers its release to the user callback (MUST match). Q-9 — in strict mode the first press of a fresh binding is delayed one poll by the latch.
  - Done when: simulated key events.
- [ ] **P4-28.4 Platform keys**
  - Files: `libobs/obs-nix-x11.c` (1280), `libobs/obs-nix-wayland.c` (1630), `libobs/obs-windows.c` (1233), `libobs/obs-cocoa.m` (835), `libobs/obs-hotkey-name-map.c` (134).
  - Steps: port the per-platform keysym/keycode → `obs_key` translators and the name map (`crates/obs-hotkey-names`, S-024 M4); key-code table equality against C.
  - Spec: S-034 §3.5 (abstract platform backend), S-024 (hotkey table).
  - Vectors: hotkeys (`09-translations`), helpers (hotkeys module).
  - Pitfalls: S-034 Q-6 — the translation fields left/right/up/down are ignored (MUST match; `09-translations`).
  - Done when: key-code tables equal.

### P4-29 Module loader
- [ ] **P4-29.1 Descriptor**
  - Files: `libobs/obs-module.c` (1274), `libobs/obs-module.h` (201).
  - Steps: port `obs_module_t` and resolve the exports with `libloading`: required `obs_module_load`, `obs_module_set_pointer`, `obs_module_ver` in that order (first missing → `MODULE_MISSING_EXPORTS` R61), optional list §3.5; version gate `v & 0xFFFF0000 > LIBOBS_API_VER → MODULE_INCOMPATIBLE_VER` (R62); build the module (R63).
  - Spec: S-035 §3.5, §5.7 R60–R63.
  - Vectors: modservice (`10-module-open`, `11-module-open-success`).
  - Pitfalls: S-035 R62 — only a newer major.minor is rejected: patch+5, major-1 and version 0 load fine. Q-5 — the module name drops the last 3 bytes of the file name even without an extension (`libpre-mod` → `libpre-`).
  - Done when: all resolved via `libloading`.
- [ ] **P4-29.2 Paths**
  - Files: `libobs/obs-module.c` (1274; `obs_add_module_path`, find-in-path), `libobs/obs-core-modules-loader-nix.c` (62), `libobs/obs-core-modules-loader-win.c` (66), `libobs/obs-core-modules-loader-mac.m` (60).
  - Steps: port `obs_add_module_path` (R90) and the search patterns (R91–R93, §6.1–6.3: glob in bytewise order, module name = last path component with everything from its last `.` removed, flat vs `%module%` layout); each OS finds modules.
  - Spec: S-035 §5.9 R90–R93, §6.1–6.3.
  - Vectors: modservice (`15-module-discovery`, `16-module-discovery-dirs`).
  - Pitfalls: S-035 Q-11 — names are cut at the last `.`: a directory `delta.v2` is skipped and a flat file `dotted.name.so` is named `dotted.name` (MUST match). Q-10 — the `lib`-prefix data fallback is applied even if neither directory exists (MUST match).
  - Done when: each OS finds modules.
- [ ] **P4-29.3 Lifecycle**
  - Files: `libobs/obs-module.c` (1274).
  - Steps: port `obs_open_module` (R63: `set_pointer(module)` then `set_locale`), `obs_init_module` (R65: sets the global `loadingModule` around `module->load()` — registrations during load are attributed to that module; do not break it), `obs_post_load_modules` (R66: enabled list head-to-tail = reverse of open order), and unload (R67: `mod->unload` from `free_module` during shutdown; C deliberately does not dlclose).
  - Spec: S-035 R63–R67.
  - Vectors: modservice (`12-module-init-free`, `18-module-load-plugins`, `19-module-load-policy`, `20-module-safe-mode`).
  - Pitfalls: S-035 Q-8 — when `obs_module_load` returns false after registering types, the registrations stay in the global tables and a retry reports "already exists". Q-12 — every retry creates another disabled-module entry for the same failing file (lists accumulate).
  - Done when: order test.
- [ ] **P4-29.4 Failures**
  - Files: `libobs/obs-module.c` (1274).
  - Steps: port `struct obs_module_failure_info` + `obs_module_failure_info_free` and `obs_load_plugins` (R101: failure string `;`-separated, count), the `MODULE_FAILED_TO_OPEN`/`MODULE_MISSING_EXPORTS` codes (R100), the disabled list, `obs_get_module_file_name` (R71) and `obs_module_get_locale_string/text` (R110–R113).
  - Spec: S-035 R100–R101, R71, R110–R113.
  - Vectors: modservice (`18-module-load-plugins`, `19-module-load-policy`, `20-module-safe-mode`, `23-module-files-locale`, `26-module-defects`).
  - Pitfalls: S-035 D-2 — `obs_add_module_path` leaves the path's `type` field uninitialised in C; required: `OBS_MODULE_TYPE_UNDEFINED` (0). D-3 — `obs_module_get_config_path(NULL, f)` must return NULL; `obs_module_get_locale_string(NULL, …)` must return false (C asserts in debug).
  - Note: the older `obs_load_all_modules2` is `OBS_DEPRECATED` but still exported — keep the symbol (P4-34).
  - Done when: failing module reported.
- [ ] **P4-29.5 Built-in modules**
  - Files: `libobs/obs-core-modules.c.in` (34), `libobs/obs-core-modules.h` (25), `libobs/obs-core-modules-loader-nix.c` (62), `libobs/obs-core-modules-loader-win.c` (66), `libobs/obs-core-modules-loader-mac.m` (60).
  - Steps: generate the Rust static table from the `obs-core-modules.c.in` name list; the core loader walks the list in order with the `%module%` templates (R94–R95) and sets `core_modules_loaded` (R102).
  - Spec: S-035 R94–R95, R102.
  - Vectors: modservice (`17-module-find-core`, `21-module-core-loader`, `22-module-core-loader-failure`).
  - Pitfalls: S-035 Q-7 — a missing core module file logs an error and ends the core loop but does not count as a failure, so `core_modules_loaded` becomes true and the remaining core modules are never loaded (SHOULD match).
  - Done when: generated Rust table.

### P4-30 Locale and text lookup
- [ ] **P4-30.1 `obs_module_text`**
  - Files: `libobs/obs-module.c` (1274; the locale-file lookup).
  - Steps: port `obs_module_text` (module locale `.ini` → `en-US` → the key itself; R110–R113) and `obs_module_get_config_path` (R114).
  - Spec: S-035 §4.2, §5.11 R110–R114.
  - Vectors: modservice (`23-module-files-locale`).
  - Pitfalls: S-035 Q-6 — `obs_module_get_config_path` with no configured directory returns the relative `"<mod>/<file>"`, not NULL as documented (callers rely on non-NULL).
  - Done when: tests.
- [ ] **P4-30.2 Fallback**
  - Files: `libobs/obs-module.c` (1274).
  - Steps: test the `en-US` default and the missing-key → key-identity fallback.
  - Spec: S-035 §4.2, R110–R113.
  - Vectors: modservice (`23-module-files-locale`).
  - Done when: missing key returns key.

### P4-31 Display and views
- [ ] **P4-31.1 Lifecycle**
  - Files: `libobs/obs-display.c` (311).
  - Steps: port `obs_display_create` (R58: inside a graphics context), `destroy` (R63), `resize` (R59: stores `next_cx/next_cy` only) and `obs_display_set_background_color` (R61: `0xAABBGGRR` packed) to `crates/obs-core`; tests with null graphics.
  - Spec: S-042 §5.6 R58–R63.
  - Vectors: obscore (`060_display`, `061_display_failure_colorspace`, `062_display_shutdown`).
  - Pitfalls: S-042 Q-7 — the colour-space refresh is dropped when a resize is applied in the same render (MUST match, `060_display`).
  - Done when: tests with null graphics.
- [ ] **P4-31.2 Draw callbacks**
  - Files: `libobs/obs-display.c` (311).
  - Steps: port `obs_display_add_draw_callback`/`remove` (R62: add appends, invocation order = add order; remove matches (callback, data)).
  - Spec: S-042 R62.
  - Vectors: obscore (`060_display`).
  - Done when: callback invoked with size.
- [ ] **P4-31.3 Views**
  - Files: `libobs/obs-view.c` (194).
  - Steps: port `obs_view_create`/`add`/`render` (R53–R57) and the 64-slot channel set/get (R50).
  - Spec: S-042 §5.5 R53–R57, R50.
  - Vectors: obscore (`050_views`, `051_view_mixes`).
  - Pitfalls: S-042 Q-11 — `obs_view_render` drops removed sources without deactivating them (MUST match, `050_views`).
  - Done when: channel set/get.

### P4-32 Platform integration
- [ ] **P4-32.1 X11**
  - Files: `libobs/obs-nix-x11.c` (1280), `libobs/obs-nix-x11.h` (22), `libobs/obs-nix.c` (458).
  - Steps: port the XInput2 hotkey platform vtable (raw events on the root window, keysym tables, pointer coordinates) via `x11rb` + `obs_nix_x11_log_info`.
  - Spec: S-058 Part B (the X11 hotkey backend is specified there), S-034 §3.5 (abstract backend contract).
  - Done when: hotkey fires.
- [ ] **P4-32.2 Wayland**
  - Files: `libobs/obs-nix-wayland.c` (1630; `wl_keyboard` + xkbcommon keymaps for hotkey translation, seat capabilities, `obs_nix_wayland_log_info`; no portal protocols in 33.0.1), `libobs/obs-nix-wayland.h` (24).
  - Steps: port the Wayland hotkey backend via `wayland-client` + xkbcommon.
  - Note: no spec — S-064 is a declared gap (DECISIONS-NEEDED D-30) and S-058 excludes Wayland; read the C.
  - Done when: connect and log compositor.
- [ ] **P4-32.3 Windows**
  - Files: `libobs/obs-windows.c` (1233), `libobs/obs-win-crash-handler.c` (534).
  - Steps: port version info, CPU names, the `obs_hotkeys_platform` vtable (via the `windows` crate) and the crash handler `obs-win-crash-handler.c`.
  - Note: no spec for the Windows platform backend (S-034 §11); read the C.
  - Done when: logs identical fields.
- [ ] **P4-32.4 macOS**
  - Files: `libobs/obs-cocoa.m` (835; Objective-C — hotkeys, system info, permissions).
  - Steps: port the macOS platform vtable and system info via `objc2`.
  - Note: no spec (S-052 covers the macOS plugins, not this glue); read the C.
  - Done when: logs identical.
- [ ] **P4-32.5 System info logging**
  - Files: `libobs/obs-nix.c` (458), `libobs/obs-nix-x11.c` (1280), `libobs/obs-nix-wayland.c` (1630), `libobs/obs-windows.c` (1233), `libobs/obs-cocoa.m` (835).
  - Steps: port each platform's `log_system_info` (CPU/memory/etc.); diff the Rust log against the C log.
  - Spec: S-058 Part B (the nix system-information logging).
  - Done when: log diff reviewed.

### P4-33 Canvases
- [ ] **P4-33.1 Canvas**
  - Files: `libobs/obs-canvas.c` (616).
  - Steps: port `obs_canvas_create(/_private)` (R41: MAIN flag always cleared), `obs_canvas_release/get_ref` (R42; destroy is internal, R43: `canvas_destroy` global signal then canvas `destroy` signal, mix removed), `obs_canvas_get_video_info`, `obs_canvas_reset_video` (R47–R48) and `obs_get_main_canvas` (R40: name `Main`, fixed UUID, R78).
  - Spec: S-042 §5.4 R40–R52.
  - Vectors: obscore (`040_canvas_main` … `049_canvas_video_active`).
  - Pitfalls: S-042 Q-6 — canvas video reset neither validates sizes nor aligns; on failure the old mix is already gone and the invalid info is stored (MUST match). D-4 — private canvases are skipped by the C mix walks (stale mix pointer, no `video_reset`, not restored); required: treat private canvases like public ones.
  - Done when: second canvas.
- [ ] **P4-33.2 Outputs**
  - Files: `libobs/obs-canvas.c` (616).
  - Steps: port the per-canvas `obs_canvas_get_video` plus channel/source membership (R50–R52).
  - Spec: S-042 R50–R52.
  - Vectors: obscore (`045_canvas_video`, `047_canvas_channels`, `048_canvas_sources`, `049_canvas_video_active`).
  - Done when: output records canvas.

## F. ABI surface and verification

### P4-34 `obs-ffi` header completeness
- [ ] **P4-34.1 C symbol list**
  - Files: the public headers — `libobs/obs.h` (2734), `obs-source.h` (572), `obs-scene.h` (116), `obs-output.h` (96), `obs-encoder.h` (365), `obs-service.h` (115), `obs-data.h` (314), `obs-properties.h` (364), `obs-hotkey.h` (272), `obs-module.h` (201), `obs-missing-files.h` (53), `obs-nix-platform.h` (53), `obs-interaction.h` (56), `obs-audio-controls.h` (250), `obs-nal.h` (37), `obs-av1.h` (47), `obs-avc.h` (55), `obs-hevc.h` (81).
  - Steps: write a script that preprocesses the headers and lists every `EXPORT` function name (~1,000); commit the list file.
  - Done when: file committed.
- [ ] **P4-34.2 Rust symbol list**
  - Steps: `nm -D` on the built `obs-ffi` dylib; script prints missing/extra against the P4-34.1 list.
  - Done when: script prints missing/extra.
- [ ] **P4-34.3 CI gate**
  - Steps: CI job runs the P4-34.2 diff and fails on any missing symbol (keep deprecated-but-exported symbols, e.g. `obs_load_all_modules2`).
  - Done when: CI fails on any missing symbol.
- [ ] **P4-34.4 Fill gaps**
  - Steps: implement the missing `#[no_mangle] extern "C"` exports (wrappers around the Phase 4 crates) until the diff is empty.
  - Done when: C-vs-Rust symbol diff is empty.

### P4-35 ABI struct layout checks
- [ ] **P4-35.1 Generate**
  - Steps: `size_of`/`offset_of` assertions for each public struct (`obs_source_info`, `obs_source_frame(2)`, `obs_audio_data`, `obs_source_audio`, `encoder_packet`, `encoder_frame`, `obs_video_info` (56 bytes, S-042 §3.1), `obs_audio_info(2)`, `obs_output_info`, `obs_encoder_info`, `obs_service_info` (216 bytes, S-035 §3.2), `gs_init_data`, …); generate from the C headers.
  - Spec: S-035 §3.2, S-042 §3.1 (the layout tables).
  - Done when: asserts compile.
- [ ] **P4-35.2 Platforms**
  - Steps: run the assertion matrix on Linux/Windows/macOS × x86_64/aarch64 in CI.
  - Done when: matrix green.

### P4-36 Third-party plugin matrix
- [ ] **P4-36.1 Select**
  - Steps: pick 10 widely used plugins covering sources, filters, transitions, outputs, encoders; commit the list with versions in `docs/tests/third-party-plugins.md`.
  - Done when: list committed in `docs/tests/third-party-plugins.md`.
- [ ] **P4-36.2 Obtain**
  - Steps: obtain builds for the three OSes; record versions and licences.
  - Done when: builds recorded with versions and licences.
- [ ] **P4-36.3 Run**
  - Steps: run one standard scenario per plugin on the Rust core (P0-16 headless host); log results in `docs/tests/third-party-plugins.md`.
  - Done when: results matrix committed.

### P4-37 In-tree plugin compatibility run
- [ ] **P4-37.1 Submodules**
  - Steps: `git submodule update --init plugins/obs-websocket plugins/obs-browser` (5.7.6 / 2.23.4-126); a fresh clone needs exactly this (sources already present in this checkout).
  - Done when: both submodules' sources are present in the build tree.
- [ ] **P4-37.2 Load**
  - Steps: load obs-websocket and obs-browser on the Rust core in the P0-16 headless host.
  - Done when: both initialise.
- [ ] **P4-37.3 Load**
  - Steps: load obs-ffmpeg and obs-filters; run the P0-16 smoke test (5 s MKV + ffprobe check, `test/headless/`).
  - Done when: smoke test passes on the Rust core.

### P4-38 Thread-safety test
- [ ] **P4-38.1 TSan builds**
  - Steps: build the Rust core with ThreadSanitizer (Linux); run the P0-16 smoke test.
  - Done when: smoke test clean.
- [ ] **P4-38.2 Stress**
  - Steps: 200 sources randomly created/destroyed/toggled for 1 h on the Rust core.
  - Done when: no crash or leak.
- [ ] **P4-38.3 Fix findings**
  - Steps: fix every TSan/stress finding, or file it with rationale in `docs/tests/`.
  - Done when: all findings fixed or filed with rationale.

### P4-39 Memory test
- [ ] **P4-39.1 Valgrind/ASan**
  - Steps: run the P0-16 smoke test under Valgrind and ASan; no leaks.
  - Done when: smoke test leak-free.
- [ ] **P4-39.2 Miri**
  - Steps: run Miri on the pure crates (at minimum `crates/obs-scene-math`, `crates/obs-hotkeys` and the Phase 1 pure crates).
  - Done when: Miri clean on the pure crates.

### P4-40 Retire libobs C
- [ ] **P4-40.1 Flip default**
  - Steps: flip the CMake `OBS_ENABLE_RUST` default so the default build links the Rust core.
  - Done when: the default build links the Rust core.
- [ ] **P4-40.2 One release**
  - Steps: ship one release keeping `OBS_ENABLE_RUST=OFF` as a working fallback.
  - Done when: one release ships with the fallback.
- [ ] **P4-40.3 Delete**
  - Steps: delete the C `libobs` sources in the next release.
  - Done when: C libobs sources removed from the tree.

### P4-41 Headless `obs-cli`
- [ ] **P4-41.1 CLI**
  - Steps: build the `obs-cli` binary (`clap`) with `--module-path`, `--collection`, `--profile`, `--record`, `--stream`, `--duration`.
  - Done when: `--help` lists all flags.
- [ ] **P4-41.2 Loading**
  - Steps: load modules (P4-29 loader) and a scene collection (P4-06.3/P4-19.3 save/load) headless with null/software graphics.
  - Done when: obs-cli loads modules and a collection.
- [ ] **P4-41.3 Commands**
  - Steps: implement the record/stream commands with exit codes (success/failure); replace the P0-16 host program.
  - Done when: replaces the P0-16 host.

### P4-42 Phase 4 review
- [ ] **P4-42.1 Matrix**
  - Steps: commit the compatibility matrix (P4-36/P4-37 results, symbol diff, layout matrix, conformance areas, stress/memory results) in `docs/tests/`.
  - Done when: matrix committed in `docs/tests/`.
- [ ] **P4-42.2 Tick**
  - Steps: tick every Phase 4 parent task in `phase-4-core-object-model.md` once all subtasks are done.
  - Done when: all Phase 4 tasks ticked.
