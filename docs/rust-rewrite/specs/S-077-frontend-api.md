# S-077 Frontend API (obs_frontend_api) & utility glue

| | |
|---|---|
| Status | Draft (round 8 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1 (`frontend/api/`, `frontend/utility/`, `frontend/models/`, `frontend/components/`) |
| Target crate | `crates/frontend` (the `obs_frontend` API crate + GUI glue modules) |
| Depends on | S-048 (function catalog, event enum, file formats — **catalog lives there**), S-076 (OBSPropertiesView used by scripts tool, S-087) |
| Conformance | planned, none yet |

## 1. Scope

S-048 §5.9 already pins the **catalog** of the `obs_frontend_*` C ABI (99 functions, 44
events) and the file formats this frontend persists. This spec pins what S-048 left out:
the shim's dispatch and error behaviour, callback/event/save-defer semantics, the event
enum ordering, and the utility glue modules (undo/redo stack, screenshot pipeline,
thumbnail manager, validation dialogs, widget helpers, obfuscation helper).

### Reference files (19, full paths relative to `obs-studio/`)

- `frontend/api/obs-frontend-api.cpp` — exported shim: callback dispatch, zero-value fallbacks
- `frontend/api/obs-frontend-api.h` — C ABI, `obs_frontend_event` enum (44 values), list structs
- `frontend/utility/undo_stack.cpp` — undo/redo stack behind `obs_frontend_add_undo_redo_action`
- `frontend/utility/ScreenshotObj.cpp` — staged screenshot capture pipeline
- `frontend/utility/ThumbnailManager.cpp` — thumbnail cache/queues/timer
- `frontend/utility/ThumbnailItem.cpp` — per-source thumbnail item
- `frontend/utility/ThumbnailView.cpp` — view binding to an item
- `frontend/models/Rect.hpp` — clamped `Rect` value type
- `frontend/models/SceneCollection.cpp` — scene-collection value object (name/path/coordinate mode)
- `frontend/utility/display-helpers.hpp` — scale/center math, safe-area constants
- `frontend/components/UIValidation.cpp` — no-sources / missing-stream-settings confirmations
- `frontend/utility/item-widget-helpers.cpp` — list item take/delete/clear (no `deleteLater`)
- `frontend/utility/item-widget-helpers.hpp` — declarations + `InsertQObjectByName`
- `frontend/utility/OBSProxyStyle.cpp` — invisible-cursor proxy style
- `frontend/utility/OBSTranslator.cpp` — `QTranslator` → frontend translation impl
- `frontend/utility/ResizeSignaler.hpp` — resize event → signal
- `frontend/utility/SettingsEventFilter.hpp` — shortcut-filter wrapper (Escape passthrough)
- `frontend/utility/SurfaceEventFilter.hpp` — surface-destroy → display teardown
- `frontend/utility/obf.c` — `deobfuscate_str` half-byte XOR (client IDs, S-074)

## 2. Conventions

RFC 2119 words. "The frontend" = the OBS main application object that implements the
callback table. `char**` list conventions as in S-048 §5.9. Event numbers are 0-based
within `enum obs_frontend_event`.

## 3. Behaviour

### 3.1 Frontend API shim (`obs-frontend-api.cpp/.h`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-077-R01 | `obs-frontend-api.cpp` | startup | Frontend registers exactly one callback table via `obs_frontend_set_callbacks_internal` (stored in a process-global `unique_ptr`). Until then the shim is uninitialized. | callback table |
| S-077-R02 | `obs-frontend-api.cpp` | any export, table unset | `callbacks_valid()` logs `LOG_ERROR "Tried to call %s with no callbacks!"` (with the calling function name) and the export MUST return its zero value: `nullptr` (pointers/objects), `false` (bools), `0` (ints). No exception, no abort. | — |
| S-077-R03 | `obs-frontend-api.cpp` | `char**` list exports (`get_scene_names`, `get_scene_collections`, `get_profiles`) | Single allocation: `(n+1) * sizeof(char*)` pointer slots followed by the NUL-terminated strings, list terminated by NULL. Empty result → return NULL with no allocation. Caller frees with exactly one `bfree` of the base pointer. | — |
| S-077-R04 | `obs-frontend-api.cpp` | `obs_frontend_get_scene_names` | Calls the table's `get_scenes` (sources carry refs), maps each to `obs_source_get_name`, frees the list, returns the string list (R03). | — |
| S-077-R05 | `obs-frontend-api.cpp` | `obs_frontend_get_global_config` | Deprecated: logs `LOG_WARNING "DEPRECATION: obs_frontend_get_global_config is deprecated. Use obs_frontend_get_app_config or obs_frontend_get_user_config explicitly instead."` and returns the **app** config. | — |
| S-077-R06 | `obs-frontend-api.h` | ABI | `enum obs_frontend_event` MUST keep 33.0.1 numbering (values 0..43): `STREAMING_STARTING=0 … STREAMING_STOPPED=3`, `RECORDING_STARTING=4 … RECORDING_STOPPED=7`, `SCENE_CHANGED=8`, `SCENE_LIST_CHANGED=9`, `TRANSITION_CHANGED=10`, `TRANSITION_STOPPED=11`, `TRANSITION_LIST_CHANGED=12`, `SCENE_COLLECTION_CHANGED=13`, `SCENE_COLLECTION_LIST_CHANGED=14`, `PROFILE_CHANGED=15`, `PROFILE_LIST_CHANGED=16`, `EXIT=17`, `REPLAY_BUFFER_STARTING=18 … STOPPED=21`, `STUDIO_MODE_ENABLED=22`, `STUDIO_MODE_DISABLED=23`, `PREVIEW_SCENE_CHANGED=24`, `SCENE_COLLECTION_CLEANUP=25`, `FINISHED_LOADING=26`, `RECORDING_PAUSED=27`, `RECORDING_UNPAUSED=28`, `TRANSITION_DURATION_CHANGED=29`, `REPLAY_BUFFER_SAVED=30`, `VIRTUALCAM_STARTED=31`, `VIRTUALCAM_STOPPED=32`, `TBAR_VALUE_CHANGED=33`, `SCENE_COLLECTION_CHANGING=34`, `PROFILE_CHANGING=35`, `SCRIPTING_SHUTDOWN=36`, `PROFILE_RENAMED=37`, `SCENE_COLLECTION_RENAMED=38`, `THEME_CHANGED=39`, `SCREENSHOT_TAKEN=40`, `CANVAS_ADDED=41`, `CANVAS_REMOVED=42`, `REPLAY_BUFFER_SAVING=43`. Note the out-of-chronological grouping (e.g. `REPLAY_BUFFER_SAVED=30` sits before the virtualcam pair) — plugin code may branch on these values. | — |
| S-077-R07 | `obs-frontend-api.cpp` | save path | `obs_frontend_save()` triggers the frontend save. `defer_save_begin()`/`defer_save_end()` bracket a period during which the frontend MAY defer the actual write (reference flushes on end/exit). Save callbacks are invoked with `(obs_data_t *save_data, bool saving, void *data)`: `saving=true` → write state into `save_data`; `saving=false` → read from it. Preload callbacks use the same signature and run during scene-collection load, **before** the full save callback set (S-087 R32 relies on this). | config files |
| S-077-R08 | `obs-frontend-api.cpp` | event callbacks | Registered via `add_event_callback`/`remove_event_callback`; the frontend dispatches every `obs_frontend_event` (R06) to all callbacks with `private_data`. Removal matches on (callback, private_data) pair. | — |
| S-077-R09 | `obs-frontend-api.cpp/.h` | UI translation | `push_ui_translation(cb)`/`pop_ui_translation()` form a stack scoped to one dialog: while pushed, UI string lookup goes through the plugin's `obs_module_get_string`. Qt's own translation is intentionally bypassed (header comment). A push MUST have a matching pop before the next push. | translation stack |
| S-077-R10 | `obs-frontend-api.cpp` | dialog/preview helpers | `open_source_properties/filters/interaction(source)`, `open_sceneitem_edit_transform(item)`, `take_screenshot()`, `take_source_screenshot(source)` (R12), `open_projector(type, monitor, geometry, name)`, `reset_video()` — all pure dispatch; the frontend decides the concrete dialog/window. Preview program mode: `preview_program_mode_active()`, `set_preview_program_mode(bool)`, `preview_program_trigger_transition()`, `set_preview_enabled(bool)`/`preview_enabled()`, get/set `current_preview_scene`, plus scene/transition get/set + `get/set_transition_duration`, `set/get_tbar_position`, `release_tbar` — pure dispatch. | frontend UI |
| S-077-R11 | `obs-frontend-api.cpp` | undo/redo | `obs_frontend_add_undo_redo_action(name, undo_cb, redo_cb, undo_data, redo_data, repeatable)` forwards to the frontend undo stack (R12–R14). `undo_redo_cb` receives the data pointer it was registered with. | undo stack |
| S-077-R12 | `obs-frontend-api.cpp` | misc | `is_safe_mode_enabled()` reports the `--safe-mode` flag (S-048 §5.1); plugins gate features on it (S-087 R01). `get_last_recording/screenshot/replay()` return the most recent output path or NULL. `get_current_record_output_path()` = active recording's path. `copy_sceneitem` / `can_paste_sceneitem(duplicate)` / `paste_sceneitem(scene, duplicate)` — scene-item clipboard; `get_locale_string(key)` → frontend locale table; `is_theme_dark()`. | — |

### 3.2 Undo/redo stack (`undo_stack.cpp`)

State: `undo_items` / `redo_items` (deques of `{name, undo_data, redo_data, undo_cb, redo_cb}`),
`last_is_repeatable`, `enabled` (set when the scene collection loads/unloads), main-window
undo/redo `QAction`s. `MAX_STACK_SIZE = 5000`.

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-077-R13 | `undo_stack.cpp` | `add_action` | If disabled → no-op. While `undo_items >= MAX_STACK_SIZE` drop the **oldest** (back). Repeatable coalescing: if this action is repeatable **and** the previous one was **and** names equal → replace the top undo item's `redo`/`redo_data` and return (no new entry). Otherwise push_front, `last_is_repeatable = repeatable`, **clear redo**, set undo action text `"Undo.Item.Undo".arg(name)` enabled, redo action text `"Undo.Redo"` disabled. Repeatable actions start a `repeat_reset_timer` (timer expiry resets `last_is_repeatable`, so a burst only coalesces while continuous). | undo/redo deques |
| S-077-R14 | `undo_stack.cpp` | `undo()` / `redo()` | Move the front item between deques and invoke its `undo(undo_data)` / `redo(redo_data)` callback; `last_is_repeatable = false`; update action texts to the new front (`"Undo.Item.Undo".arg(name)` / `"Undo.Item.Redo".arg(name)`) or to plain disabled `"Undo.Undo"`/`"Undo.Redo"` when that side empties. | deques |
| S-077-R15 | `undo_stack.cpp` | `clear()` / enable/disable | `clear()`: both deques emptied, `last_is_repeatable=false`, both actions plain+disabled. `enable()`: re-enable actions (redo only if non-empty) — used on scene-collection load; `disable()` disables both. | actions |

### 3.3 Screenshot pipeline (`ScreenshotObj.cpp`)

Staged, one stage per render tick (`obs_add_tick_callback`), then queued to the UI thread.

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-077-R16 | `ScreenshotObj.cpp` | Render stage | Size: source w/h if the weak source is alive, else main `obs_video_info` base; both zero → `LOG_WARNING "Cannot render source, invalid target size"`, remove tick, delete. Colour space: W: source space, but `GS_CS_709_EXTENDED` → `GS_CS_709_SCRGB` (JXR path); other platforms: forced `GS_CS_SRGB` (HDR tonemapped). Output size = `options.size` if valid, else source size. Render: clear black, ortho projection over source rect, viewport from `GetScaleAndCenterPos` (letterboxed), blend ONE/ZERO, draw source (or `obs_render_main_texture` if source dead) into `gs_texrender`. | `texrender` |
| S-077-R17 | `ScreenshotObj.cpp` | Download stage | `gs_stage_texture(stagesurf, texrender texture)`. | `stagesurf` |
| S-077-R18 | `ScreenshotObj.cpp` | Output stage | Map stagesurface. `GS_RGBA16F` (HDR) → copy rows into `half_bytes` (`width*8` bytes/row). Otherwise → `QImage(Format_RGBX8888)` row copy. Unmap. Then **queued** `handleSave` on UI thread, remove tick, stage=Finished. | `image`/`half_bytes` |
| S-077-R19 | `ScreenshotObj.cpp` | `handleSave` → `saveToFile` | `options.outputToFile == false` → emit `imageReady(image)` only. Else: base dir from config — `Output.Mode == "Advanced"` ? (`AdvOut.RecType == "Standard"` ? `AdvOut.RecFilePath` : `AdvOut.FFFilePath`) : `SimpleOutput.FilePath`. Extension: `png` for SDR, `jxr` for HDR (`half_bytes` non-empty; JXR encoder is W-only — non-W HDR screenshots fall back to the SDR path, R16). Name = `GetOutputFilename(dir, ext, SimpleOutput.FileNameWithoutSpace, Output.OverwriteIfExists, GetFormatString(Output.FilenameFormatting, "Screenshot"))`. Write on a detached `std::thread` (`muxFile`); on completion emit `imageSaved(path)` (queued). | file on disk |
| S-077-R20 | `ScreenshotObj.cpp` | JXR write (W) | WIC frame: lossless option, 72 dpi, `GUID_WICPixelFormat64bppRGBAHalf` for HDR; SDR → PNG writer on the `QImage`. | — |

### 3.4 Thumbnail system (`ThumbnailManager.cpp`, `ThumbnailItem.cpp`, `ThumbnailView.cpp`)

Constants: `kMaxThumbnails = 64` (LRU cache size), `kMinimumThumbnailUpdateInterval = 100` ms,
`kThumbnailUpdateInterval = 5000` ms.

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-077-R21 | `ThumbnailManager.cpp` | ctor | Timer started at 100 ms interval. Subscribes to the `source_destroy` signal → drops the item for that source (queues cleaned). Cache = LRU (`std::list` + map), `put` moves key to front and evicts the back when > 64. | cache |
| S-077-R22 | `ThumbnailManager.cpp` | update tick | `updateNextItem()`: if `priorityQueue` non-empty → pop front (immediate update for newly requested items); else pop `updateQueue` front and **re-push it to the back** (round-robin). A failed render re-curses up to `thumbnailList.size()` times in the same tick. Items still on their default pixmap count as "quick update". Next tick interval: 100 ms if the priority queue is non-empty or a quick update happened, else 5000 ms (timer restart also restarts the elapsed clock). | queues, timer |
| S-077-R23 | `ThumbnailManager.cpp` | `addToPriorityQueue(uuid, immediate)` | Skip if already at front of priority queue; remove from both queues; `immediate` → front + run `updateTick()` now if the 100 ms since last restart already elapsed; else back. | queues |
| S-077-R24 | `ThumbnailItem.cpp` / `ThumbnailView.cpp` | view creation | One `ThumbnailItem` per source uuid, owned by the manager; `createView(parent)` returns a `ThumbnailView` that copies the current pixmap and follows the item's `pixmapUpdated` signal; if the parent is null the view `deleteLater`s; invalid item → view disabled. Views track view-count (items are deleted only when no views remain — `decrementViewCount`/`incrementViewCount`); `requestUpdate()` emits `updateRequested(uuid)` iff enabled. | items, views |

### 3.5 Small value types & helpers

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-077-R25 | `Rect.hpp` | construction/setters | `Rect{width, height}` non-negative doubles: direct `double` sets clamp `max(v, 0.0)`; integral sets are clamped into `[0, f64::MAX]` (`safeConvertToDouble`). Getters are conversion-checked (static_assert). | — |
| S-077-R26 | `SceneCollection.cpp` | `setCoordinateMode` | Rejects `SceneCoordinateMode::Invalid` with `std::invalid_argument("Invalid coordinate mode provided")`. Getters: `getName`, `getFileName` (file name, UTF-8), `getFileStem`, `getFilePath(String)`, `get/setMigrationResolution` (Rect). (Storage is in `SceneCollection.hpp`; JSON layout itself is S-048 §5.x.) | collection |
| S-077-R27 | `display-helpers.hpp` | layout math | `GetScaleAndCenterPos(base, window, &x, &y, &scale)`: aspect-preserving fit — wider window: `scale = windowCY/baseCY`, `newCX = int(windowCY*baseAspect)`, `newCY = windowCY`; else `scale = windowCX/baseCX`, `newCY = int(windowCX/baseAspect)`; `x = windowCX/2 - newCX/2`, `y = windowCY/2 - newCY/2` (integer division). `GetCenterPosFromFixedScale`: `x = (windowCX - baseCX*scale)/2`, `y = (windowCY - baseCY*scale)/2`. `GetPixelSize(widget) = size * devicePixelRatioF`. Safe-area outline constants: action 3.5 %, graphics 5.0 %, 4:3 16.25 % (BT.1848-1 / EBU R 95), drawn as vertex buffers by `InitSafeAreas`. | — |
| S-077-R28 | `UIValidation.cpp` | `NoSourcesConfirmation` | Count sources with `OBS_SOURCE_VIDEO` flag: nonzero → return true (no dialog). Parent not visible → true. Else Yes/No question dialog (`NoSources.Text` + `.AddSource`); only Yes proceeds. | — |
| S-077-R29 | `UIValidation.cpp` | `StreamSettingsConfirmation` | `obs_service_can_try_to_connect(service)` → `ContinueStream`. Else inspect `OBS_SERVICE_CONNECT_INFO_SERVER_URL`/`_STREAM_KEY`: custom service (`rtmp_custom`) or URL present → `MissingUrl` text; non-custom + key missing → `MissingStreamKey`; non-custom + both missing → `MissingUrlAndApiKey`. Dialog buttons `StreamSettingsWarning` (accept, default) / `Cancel` (escape); macOS uses `AcceptRole`/`ResetRole`, other platforms `NoRole`/`NoRole`. Result: continue vs open stream settings vs cancel. | — |
| S-077-R30 | `item-widget-helpers.cpp` | list mutation | `TakeListItem(w, row)`: **synchronously** `delete itemWidget(item)` (never `deleteLater` — prevents Qt event queue holding source refs of unknown lifetime) then `takeItem(row)`. `DeleteListItem`: same widget delete + `delete item`. `ClearListItems`: clear selection, delete every item widget, `clear()`. `InsertQObjectByName` (hpp): insert into vector keeping locale-aware `objectName` order. | list widgets |
| S-077-R31 | `OBSProxyStyle.cpp` | text rendering | `OBSInvisibleCursorProxyStyle::pixelMetric`: `PM_TextCursorWidth → 0` (hides the I-beam/caret in specific widgets); all other metrics proxied. | — |
| S-077-R32 | `OBSTranslator.cpp` | `QTranslator::translate` | Strip **all spaces** from the source text, look it up via the app's custom translation (`App()->TranslateString`); miss → original source text returned (not the space-stripped form). This is why UI strings are looked up without spaces (see Q-2). | — |
| S-077-R33 | `ResizeSignaler.hpp` | QEvent::Resize on installed object | Emits `resized()`, then defers to default handling. Used to re-render preview-related widgets. | — |
| S-077-R34 | `SettingsEventFilter.hpp` | key events on settings dialogs | Escape → not consumed (dialog close works). **All other events** (including non-Escape key press/release, via deliberate fall-through to `default`) are delegated to the hotkey shortcut filter created by `CreateShortcutFilter()`. | — |
| S-077-R35 | `SurfaceEventFilter.hpp` | `QEvent::PlatformSurface` `SurfaceAboutToBeDestroyed` | Calls `display->DestroyDisplay()` on the owning `OBSQTDisplay` (tears down the GL context for that preview). Other events pass through. | display |
| S-077-R36 | `obf.c` | `deobfuscate_str(str, key)` | In-place XOR: byte `i` of the string is XORed with the low nibble of `key_bytes[i/2]` for even `i` and the high nibble for odd `i`, wrapping at 16 bytes (8-byte little-endian key × 2 nibbles). Stops at NUL. Used to hide client IDs in binaries (S-074 R14). | string in place |

## 4. Reference defects and quirks

- **Q-1** (`SettingsEventFilter.hpp`): the `KeyPress`/`KeyRelease` cases have **no `break`** —
  after the Escape check they fall through into `default` and the event *is* passed to the
  shortcut filter. Only Escape returns early. Preserve this, it is load-bearing.
- **Q-2** (`OBSTranslator.cpp`): space-stripping on lookup means two different source strings
  differing only by spaces alias to one translation.
- **Q-3** (`ScreenshotObj.cpp`): HDR screenshots exist only on Windows (JXR); elsewhere the
  image is tonemapped to SRGB and written as PNG.
- **Q-4** (`undo_stack.cpp`): overflow drops the **oldest** action (back of the deque); the
  repeatable timer makes coalescing time-bounded.

## 5. Concurrency (summary)

- The shim is called from any thread; the callback table is set once before any export is
  used (frontend startup). `obs_frontend_save` and event callbacks are frontend-thread.
- `ScreenshotObj` runs its render stages on the render thread and hops to the UI thread only
  for `handleSave`/`onFinished`; file IO is a detached thread.
- `ThumbnailManager` is UI-thread (Qt timer + `source_destroy` signal hop).
- Undo callbacks run on the UI thread; data pointers are owned by the caller.

## 6. Open questions

1. S-048 §5.9 lists 99 functions; this spec's R01–R12 cover the dispatch behaviour for the
   whole table. Confirm the Rust `obs_frontend` crate re-exports the same symbol names for
   ABI compatibility with existing plugins, or whether the rewrite targets a plugin-free
   boundary (affects whether the shim layer is kept verbatim).
2. `defer_save_begin/end`: the reference implementation's flush policy lives in `OBSBasic`
   (outside this spec's file list); the exact flush trigger points need to be confirmed
   against S-048's save-section before conformance scripts.
3. `ThumbnailManager` cache size (64) and intervals are UI-tuning constants — confirm they
   should stay identical in the rewrite.
