# Phase 4 guide: core object model and C ABI

Read [00-developer-primer.md](00-developer-primer.md) first. Tasks: [../phase-4-core-object-model.md](../phase-4-core-object-model.md); subtasks: [../subtasks/phase-4-subtasks.md](../subtasks/phase-4-subtasks.md).

## Scope reminder

This phase replaces the remainder of `libobs/*.c` (≈ 50 files). The surface that must stay compatible is about 1,000 exported functions (`obs.h` 680, `obs-data.h` 156, `obs-properties.h` 109, `obs-hotkey.h` 55, …) plus the `*_info` vtable structs plugins fill in.

## The global state

`libobs/obs-internal.h:549` — `struct obs_core` (one global `obs`):

```
first_module, first_disabled_module, module_paths, safe_modules, disabled_modules
source_types, input_types, filter_types, transition_types  (arrays of obs_source_info)
output_types, encoder_types, service_types                 (arrays of *_info)
signals (signal_handler_t*), procs (proc_handler_t*)
locale, module_config_path, name_store
video (obs_core_video), audio (obs_core_audio), data (obs_core_data), hotkeys (obs_core_hotkeys)
destruction_task_thread, ui_task_handler, core_modules_loaded
```
Start-up (`obs_startup` → `obs_init`, `obs.c:1326/1228`) calls `obs_init_data` (`obs.c:969`), `obs_init_hotkeys` (`obs.c:1138`) and initialises the mutexes; double start-up logs and returns false. `obs_reset_video(ovi)` (`obs.c:1526`) then runs `obs_init_graphics` (`obs.c:478`) — graphics context, default effects, the video mix, graphics thread — and `obs_reset_audio` runs `obs_init_audio` (`obs.c:906`). `obs_reset_video` returns `OBS_VIDEO_*` error codes (`MODULE_NOT_FOUND`, `NOT_SUPPORTED`, `FAIL`, …). **Replicate the error codes**: the frontend shows different messages for each.

`obs_core_video` owns a list of `obs_core_video_mix` (one per canvas: textures, stage surfaces, conversion textures), `mixes_mutex`, the `graphics` pointer, default effects, and the graphics thread. `obs_core_data` owns the intrusive linked lists of all sources/outputs/encoders/services and their mutexes (`audio_sources_mutex`, `sources_mutex`, …).

## Object lifetime, the way C does it (verified)

`obs_source_create_internal` (`obs-source.c`) shows the pattern for all kinds:

1. `bzalloc` the struct; **copy** the registered `obs_source_info` into it (`source->info = *info`). If the id is unknown the source is still created ("so the user's data doesn't become lost") with a stub info.
2. Filters are forced `private` (not enumerable).
3. Initialise the `obs_context_data` (settings `obs_data_t`, name, uuid, hotkey data, signal handler, proc handler).
4. Call `info->get_defaults(settings)` then `get_defaults2(type_data, settings)`, then `obs_source_init`.
5. Call `info->create(settings, source)`; if it returns NULL the error is logged but the source object remains.
6. Emit `source_create` (and a canvas-specific signal) unless private.

Reference counting: every object has a **control block** `obs_weak_*` that doubles as the strong-reference counter: `obs_source_release()` does `obs_ref_release(&control->ref)` and when the strong count hits zero calls `obs_source_destroy` and then releases the weak control. `obs_source_destroy` guards against double destruction with an atomic `destroying` flag ("Double destroy just occurred…" log). Weak→strong upgrade (`obs_weak_source_get_source`) fails once strong count is zero. Rust mapping (**Proposal**): `Arc<SourceInner>` with `Weak`, handles exposed as `Box<Arc<_>>` raw pointers, with destruction of the *plugin's* `data` pointer performed in `Drop` on the thread the C code expects (check `obs_source_destroy` for UI/graphics-thread requirements: `destruction_task_thread` exists for deferred destruction).

## Plugin-facing vtables

`struct obs_source_info` has 54 fields (`obs-source.h`): `id`, `type`, `output_flags`, `get_name`, `create`, `destroy`, `get_width/height`, `get_defaults`, `get_properties`, `update`, `activate/deactivate`, `show/hide`, `video_tick`, `video_render`, `filter_video`, `filter_audio`, `enum_active_sources`, `save/load`, `mouse_*`, `focus`, `key_click`, `filter_remove`, `type_data`, `free_type_data`, `audio_render`, `enum_all_sources`, `transition_start/stop`, `get_defaults2`, `get_properties2`, `audio_mix`, `icon_type`, `media_*` (9), `version`, `unversioned_id`, `missing_files`, `video_get_color_space`, `filter_add`, `get_dark_icon`, `get_light_icon`. Fields were **appended over time**; `obs_register_source_s(info, size)` (`obs-module.c:1035`) copies only `size` bytes, so old plugins have the later fields NULL. Your adapter must treat every callback as optional and never read past `size`. Same for encoder/output/service infos.

Calling conventions to preserve (read each callback's comments in the header):
- `video_render(data, effect)` runs on the graphics thread inside `gs_enter_context`.
- `video_tick(data, seconds)` runs on the graphics thread before rendering.
- `filter_audio` runs on the audio thread; `audio_render` also (the mixer thread).
- `update` can be called from the UI thread at any time; plugins protect their own state.
- `create` may be called from any thread (UI during load).

## Module loading (verified)

`obs_open_module(&module, path, data_path)` (`obs-module.c`): `os_dlopen`, `load_module_exports` (required: `obs_module_load`, `obs_module_set_pointer`, `obs_module_ver`; the rest optional), rejects modules built against a newer libobs: `(mod.ver() & 0xFFFF0000) > LIBOBS_API_VER` (patch version ignored) → `MODULE_INCOMPATIBLE_VER`; builds `obs_module_t` (bin/data paths, name from file), loads a metadata file if present (`obs_module_load_metadata`), calls `mod.set_pointer(module)` (what `OBS_DECLARE_MODULE` defines). `obs_init_module(module)` sets the global `loadingModule` and calls `module->load()` — **`obs_register_*` calls made during load are attributed to that module via `loadingModule`**; do not break this (uninstalling/disabling modules relies on the per-module lists `sources/outputs/encoders/services`). Failures are reported via `struct obs_module_failure_info` and `obs_load_plugins`.

Platform differences: `obs-core-modules-loader-{nix.c,win.c,mac.m}` resolve the per-module bin/data paths (macOS reads the app bundle's PlugIns; the mac loader is Objective-C) and walk the *static* built-in module list that `obs-core-modules.c.in` generates.

## Hotkeys, platform

`obs-hotkey.c` (34 KB): registrations return ids; bindings map to `obs_key_combination`; a dedicated hotkey thread (`obs_hotkey_thread`) processes platform events; `obs-nix-x11.c` (xcb + XInput2), `obs-nix-wayland.c` (41 KB: `wl_keyboard` + xkbcommon keymaps; Rust: `wayland-client`), `obs-windows.c`, `obs-cocoa.m` (34 KB) translate platform keys and provide system information for the log (`log_system_info` in each file). The frontend sets the platform before init (`obs_set_nix_platform`, `obs_set_nix_platform_display` in `OBSApp::OBSInit`); replicate those entry points.

## Frontend hooks libobs already has

`obs_task_handler_t ui_task_handler` (run a task on the UI thread; used by `obs_queue_task(OBS_TASK_UI, …)`); signals/procs on `obs->signals`; `obs_frontend_*` is a separate API owned by Phase 6 (P6-36) but **plugins call it through `obs-frontend-api`**, so Phase 4 tests with the C frontend must keep working.

## Suggested order

1. Design (P4-01…04) — write the ADR first; review the lock inventory with someone who knows the code.
2. Pure data objects needing no threads: `obs_data`-based save/load structs, hotkey data (done in Phase 1), module metadata, scene item transform maths (P4-18).
3. Core + module loader (P4-03, P4-29) so that a C plugin can register something.
4. Sources (P4-05…15) → scenes (P4-16…20) → encoders/outputs/services (P4-21…27).
5. Hotkeys/platform/display/canvas (P4-28…33).
6. ABI verification (P4-34…39), retirement (P4-40).

Because libobs's graphics thread, audio callback and packet queues all depend on these objects, the **integration parts of Phase 2/3 tasks land here** (see the dependency tables in the Phase 2 and Phase 3 guides).

## Pitfalls

- **Locking order.** 46 mutexes in `obs-internal.h`. The C code has historic lock-order conventions encoded in comments, not types. Build the lock table (P4-04) *before* writing code and add a debug-only order checker.
- **Callbacks while holding locks.** Several `signal` emissions and `enum_*` callbacks happen under locks; plugin callbacks may call back into libobs. Rust's `Mutex` is not recursive: either use `parking_lot::ReentrantMutex` where C uses recursive mutexes (`pthread_mutex_init_recursive`) or restructure after proving no re-entrancy.
- **Pointer identity.** Plugins compare `obs_source_t*` pointers and use them as map keys. Handles must be stable for the lifetime of the object.
- **NULL tolerance.** Many exported functions accept NULL and return a default (e.g. `obs_source_release(NULL)` is a no-op, `obs_source_get_name(NULL)` returns NULL). Copy it with tests.
- **Strings returned to C** live as long as the owning object or until the next call on that object; document per function and keep storage in the object.
- **Scenes need a canvas** (`requires_canvas`): a scene created without one is assigned `obs->data.main_canvas` with a warning. Mirror it.
- **Order of signals** (`source_create` after `create` callback, etc.) is relied on by the frontend and obs-websocket. Table-test the order.

## Verification checklist

- [ ] Symbol list diff (C vs Rust) = 0 missing (P4-34).
- [ ] Layout assertions for all public structs on 3 OS × 2 architectures (P4-35).
- [ ] ABI test plugin (P0-15) + older-struct-size test pass.
- [ ] obs-websocket, obs-browser (after submodule init), obs-ffmpeg, obs-filters run unmodified.
- [ ] 1 h random create/destroy/toggle stress test under TSan with no reports.
