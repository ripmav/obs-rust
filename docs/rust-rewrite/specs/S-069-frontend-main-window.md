# S-069 Frontend: main window, preview area, output controls & main-window dialogs

| | |
|---|---|
| Status | Draft (round 8 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1, `frontend/widgets/OBSBasic*` (Qt main window) |
| Target crate | `crates/frontend` (GUI layer; Qt in the reference) |
| Depends on | S-048 (config files, CLI/app lifecycle, scene-collection format, frontend API) |
| Conformance | planned; none yet (Qt widgets; no executable vectors) |

## 1. Scope

In scope: the `OBSBasic` main window (shell, lifecycle, FPS accessors),
the preview/program display area and scaling state, scene/source lists,
per-scene-item actions, the transform dialog, transitions (quick +
T-bar), the control bar and stream/record/replay/vcam output state
machines, studio (preview/program) mode, projectors, multiview, file
drops, the clipboard, status bar + stats, the properties/filters/
interaction/adv-audio/source-select dialogs, the auto-config wizard, and
the hotkey/service/tray/updater/browser-dock glue.

Out of scope: canvas *interaction* (hit tests, drag, handles, snapping) —
**S-067 (P6-07.9)** owns it; `OBSBasicPreview` is covered here
**widget-surface only** (construction, state, signals, timers). `OBSApp`
behaviour — S-048 (`OBSApp.hpp` cited for the accessor surface only).
Tree/toolbar widgets — S-072. Settings dialog — S-068.

**Reference files** (all 58):

- main-window shell: `frontend/widgets/OBSBasic.cpp`, `frontend/widgets/OBSBasic.hpp`, `frontend/widgets/OBSBasic_MainControls.cpp`, `frontend/widgets/OBSBasicControls.cpp`, `frontend/widgets/OBSBasicStatusBar.cpp`, `frontend/widgets/OBSBasic_StatusBar.cpp`, `frontend/widgets/OBSBasic_SysTray.cpp`, `frontend/widgets/OBSBasic_Updater.cpp`, `frontend/widgets/OBSBasic_Service.cpp`, `frontend/widgets/OBSBasic_Icons.cpp`, `frontend/widgets/OBSBasic_Screenshots.cpp`, `frontend/widgets/OBSBasic_Hotkeys.cpp`, `frontend/widgets/OBSBasic_Docks.cpp`, `frontend/docks/OBSDock.cpp`, `frontend/widgets/OBSBasicStats.cpp`
- preview area: `frontend/widgets/OBSBasicPreview.cpp`, `frontend/widgets/OBSBasicPreview.hpp`, `frontend/widgets/OBSBasic_Preview.cpp`, `frontend/widgets/OBSQTDisplay.cpp`, `frontend/widgets/OBSQTDisplay.hpp`, `frontend/components/OBSPreviewScalingComboBox.cpp`, `frontend/components/OBSPreviewScalingComboBox.hpp`, `frontend/components/OBSPreviewScalingLabel.cpp`, `frontend/utility/PreviewProgramSizeObserver.cpp`, `frontend/widgets/OBSBasic_ContextToolbar.cpp`
- scenes/items/transform: `frontend/widgets/OBSBasic_Scenes.cpp`, `frontend/widgets/OBSBasic_SceneItems.cpp`, `frontend/widgets/OBSBasic_Transitions.cpp`, `frontend/dialogs/OBSBasicTransform.cpp`, `frontend/dialogs/OBSBasicTransform.hpp`, `frontend/widgets/OBSBasic_Canvases.cpp`, `frontend/utility/OBSCanvas.cpp`
- main-window dialogs: `frontend/dialogs/OBSBasicSourceSelect.cpp`, `frontend/dialogs/OBSBasicProperties.cpp`, `frontend/dialogs/OBSBasicFilters.cpp`, `frontend/dialogs/OBSBasicFilters.hpp`, `frontend/dialogs/OBSBasicInteraction.cpp`, `frontend/dialogs/OBSBasicAdvAudio.cpp`, `frontend/dialogs/OBSBasicVCamConfig.cpp`, `frontend/widgets/OBSBasic_Clipboard.cpp`
- output areas: `frontend/widgets/OBSBasic_Streaming.cpp`, `frontend/widgets/OBSBasic_Recording.cpp`, `frontend/widgets/OBSBasic_ReplayBuffer.cpp`, `frontend/widgets/OBSBasic_VirtualCam.cpp`, `frontend/widgets/OBSBasic_StudioMode.cpp`, `frontend/widgets/OBSBasic_Browser.cpp`
- projectors/multiview/drops: `frontend/widgets/OBSProjector.cpp`, `frontend/widgets/OBSBasic_Projectors.cpp`, `frontend/components/Multiview.cpp`, `frontend/components/Multiview.hpp`, `frontend/widgets/OBSBasic_Dropfiles.cpp`
- auto-config wizard: `frontend/wizards/AutoConfig.cpp`, `frontend/wizards/AutoConfigStartPage.cpp`, `frontend/wizards/AutoConfigStreamPage.cpp`, `frontend/wizards/AutoConfigTestPage.cpp`, `frontend/wizards/AutoConfigVideoPage.cpp`, `frontend/wizards/TestMode.hpp`
- misc: `frontend/OBSApp.hpp`

## 2. Conventions

RFC 2119 words. `main` = `OBSBasic::Get()`; `config` = profile `basic.ini`,
`userConfig` = `user.ini` (S-048); `QTStr(...)` = translated string. All
widgets are GUI-thread; obs-thread signals are queued
(`QMetaObject::invokeMethod`) before widget mutation. "Undo action" = an
entry on `main->undo_s` (callbacks + optional JSON payload).

## 3. Interface (main window surface)

- `OBSBasic : OBSMainWindow : QMainWindow`, single instance (`Get()`);
  owns the `OBSBasicControls` bar in dock `controlsDock`, the
  `OBSBasicPreview` canvas, scene/source lists, transition controls,
  status bar.
- Output state flags maintained by direct self-connections:
  `streamingStarting` (on `StreamingStarting`; cleared on Started/
  Stopped), `recordingStarted`/`recordingPaused`; plus
  `recordingStopping`/`streamingStopping`, `disableOutputsRef`
  (dialogs that block starting outputs), vcam active state, broadcast
  state (`broadcastActive/Ready`, `autoStart/StopBroadcast`).
- Signals consumed elsewhere: `mainWindowClosed`,
  `StreamingPreparing/Starting/Started(withDelay)/Stopping/
  Stopped(withDelay)`, `RecordingStarted(pausable)/Paused/Unpaused/
  Stopping/Stopped`, `ReplayBufferStarted/Stopping/Stopped`,
  `VirtualCamStarted/Stopped`, `TransitionStarted/Stopped/Added`,
  `PreviewProgramModeChanged(bool)`, `CanvasResized`/`OutputResized`,
  `PreviewZoomIn/Out/Reset`, `TransitionDurationChanged`.
- State: `previewProgramMode` (atomic), `currentScene` (atomic weak ref),
  `programScene`/`lastProgramScene` (weak, studio), `transitions` +
  `transitionNameToUuids` + `transitionUuids`, `quickTransitions`
  (`{source, duration, id, fadeToBlack, hotkey}`), `projectors`,
  `extraBrowserDocks`, `visDialogs`/`modalDialogs`/`visMsgBoxes`,
  `nudge_timer`, `diskFullTimer` (1000 ms), `previewScale`,
  `previewX/Y/CX/CY`, box/safe-area vertex buffers, `undo_s`, `service`.
- Constants: `PREVIEW_EDGE_SIZE 10` px (canvas box inset, R06);
  preview scaling R07; `T_BAR_PRECISION 1024` / `T_BAR_CLAMP 102` (R28);
  `MBYTES_LEFT_STOP_REC 50` MiB (R34).

## 4. Data formats

- `SourceCopyInfo` (clipboard, `OBSBasic.hpp`): scene-item transform,
  crop, blend method/mode, show/hide transition settings,
  `private_settings` JSON.
- `SavedProjectorInfo`: type (preview/source/multiview/scene/program),
  source/scene uuid, windowed vs fullscreen, monitor.
- Quick transitions persist as JSON array `{name, duration, id,
  fade_to_black, hotkeys}` (R27); transition sources persist in the
  scene collection (S-048).
- Drop-file extension tables (R44): text `txt, log`; image `bmp, gif,
  jpeg, jpg, [jxr (Win)], png, tga, webp`; html `htm, html`; media: ~90
  audio/video extensions (full list in reference).
- Wizard bandwidth test runs video at 128×128@60 with a random-noise
  render callback (`TestMode`, R55).

## 5. Behaviour (numbered rules)

### 5.1 Main window shell (`OBSBasic.cpp`, `OBSBasic.hpp`, `OBSApp.hpp`)

- **S-069-R01** Construction: `WA_NativeWindow`, `acceptDrops`, `CustomContextMenu`, `QEvent::User+Close`, API interface, auth for build-enabled services (Twitch/Restream/YouTube), R03 output flags, `controlsDock` + `OBSBasicControls` (buttons → `*ActionTriggered`).
- **S-069-R02** `OnActivate`/`OnDeactivate`: app sleep-inhibition refcount ± process priority; called on window focus changes and output start/stop (R30–R39).
- **S-069-R03** `closeEvent`: `QueryAllowedToClose` (plugins may veto); confirm if `shouldPromptForClose()`; then `SaveProject()` + `mainWindowClosed` (app shutdown is S-048). `saveAll()` = project + both configs.
- **S-069-R04** FPS accessors read `[Video]`: `GetConfigFPS` dispatches `FPSType` (1 → `FPSInt`, 2 → `FPSNum/FPSDen`, else common); `GetFPSCommon` maps: `10`→10/1, `20`→20/1, `24 NTSC`→24000/1001, `25 PAL`→25/1, `29.97`→30000/1001, `48`→48/1, `50 PAL`→50/1, `59.94`→60000/1001, `60`→60/1, unknown → 30/1; `GetFPSNanoseconds` exists but is never dispatched (Q-1).
- **S-069-R05** `OBSApp.hpp` is cited for the accessor surface only (`App()`, `QTStr`, user config paths, sleep inhibition, themes); behaviour = S-048.

### 5.2 Preview area (`OBSBasic_Preview.cpp`, `OBSQTDisplay.*`, scaling widgets, `OBSBasic_ContextToolbar.cpp`)

- **S-069-R06** `EnablePreviewDisplay` toggles the disabled overlay; `InitPrimitives` allocates box/safe-area/outline vertex buffers (canvas box inset `PREVIEW_EDGE_SIZE 10` px); `RenderMain` renders the active output (studio: preview = output 0, program = R40); `ResizePreview` recomputes `previewScale`; `Nudge` via `nudge_timer`.
- **S-069-R07** `OBSBasicPreview` scaling state: `scalingLevel` (1..32), `scalingAmount` (1..8.0), `fixedScaling` (1:1), `scrollingOffset`; setters clamp + emit `scalingChanged(float)`/`fixedScalingChanged(bool)`; one wheel step scales by `ZOOM_SENSITIVITY = 8^(1/32)` (handler = S-067). `OBSPreviewScalingComboBox` (percent presets + fixed 1:1 + canvas + output; signals `PreviewScaleChanged`/`PreviewFixedScalingChanged`, text refreshed on `CanvasResized`/`OutputResized`); `OBSPreviewScalingLabel` shows the current percent.
- **S-069-R08** `OBSQTDisplay`: native render surface per preview/projector widget — `SetDisplayBackgroundColor`, `CreateDisplay` (re)creates the platform context, `paintEvent` presents, `resizeEvent` resizes; `nativeEvent` (Win `WM_MOVE`/`WM_DISPLAYCHANGE`) → `OnMove`/`OnDisplayChange` re-query monitor geometry.
- **S-069-R09** Context toolbar: `UpdateContextBar`/`ClearContextBar` fill per-selected-item actions; `UpdateContextBarDeferred` batches selection churn; `SourceToolBarActionsSetEnabled` gates actions per capability.
- **S-069-R10** `OBSBasicPreview` **widget surface only** — canvas interaction (hit tests, drag, handles, snapping, draw family, cursor, scrollbars, key/wheel/mouse events) is S-067. Pinned here: ctor + `Init` (spacer labels `spacerLabel[4]` + `spacerPx[4]`); `HANDLE_RADIUS 4.0`, `HANDLE_SEL_RADIUS 6.0`, `HELPER_ROT_BREAKPOINT 45.0`, `SPACER_LABEL_MARGIN 6.0`; handle flags `ITEM_LEFT 1 / RIGHT 2 / TOP 4 / BOTTOM 8 / ROT 16`; SSE-aligned `operator new/delete` (bmalloc/bfree).

### 5.3 Scenes and source items (`OBSBasic_Scenes.cpp`, `OBSBasic_SceneItems.cpp`)

- **S-069-R11** Scene ops: `AddScene` (uniquified name), `RemoveScene` (confirm; transition to a remaining scene), `DuplicateSelectedScene`, `RemoveSelectedScene`; list order persists to the scene collection (S-048); grid/list display mode (grid widget S-072 R19).
- **S-069-R12** `CreateSourcePopupMenu`: reorder up/down/top/bottom, hide, filters, properties, interact, transform, project/window (R41), screenshot (R16), fit/center/stretch (R13), flip H/V, rotate 90°, align, copy/paste (R17), rename, remove — enabled per selection.
- **S-069-R13** Geometry actions (all `obs_sceneitem_*` + undo): `FitToScreen` (fit base canvas, keep aspect), `StretchToScreen` (fill, aspect lost), `CenterToScreen` (keep size), `HorizontalCenter`/`VerticalCenter`, `Rotate` (+90° CCW), `FlipHorizontal`/`FlipVertical`.
- **S-069-R14** Per-item menus → item settings with undo: scale filter (none/bilinear/bicubic/area), deinterlace (mode + field order), blend method (default/lessen), blend mode (normal/screen/multiply/add/subtract), background colour.
- **S-069-R15** Source list upkeep: `RefreshSources`, `ReorderSources`, `sourceUuidDropped` (tree-dragged source → scene), `on_toggleSourceIcons_toggled` (persist), `TogglePreview`, `CreateFirstRunSources` (first run: "Scene" + mic/aux + desktop), `ResetAudioDevice`.
- **S-069-R16** `QueryRemoveSource`: confirm (lists using scenes); removes scene items (undo); destroys the source only if unreferenced. Screenshots: `Screenshot` (current scene) / `ScreenshotSelectedSource` / `ScreenshotScene` → PNG via S-048 collision-safe names. Icons: `GetSourceIcon(id)` per-id cache + setters.

### 5.4 Clipboard and canvases (`OBSBasic_Clipboard.cpp`, `OBSBasic_Canvases.cpp`, `OBSCanvas.cpp`)

- **S-069-R17** `copySceneItem` fills `SourceCopyInfo`; `pasteSceneItem(scene, duplicate)` adds the item from the stored source (or a duplicated source) at the stored transform — both with undo; `on_actionCopyRef`/`PasteRef` copy/paste the source as an existing reference; `on_actionPasteDup` pastes a full duplicate.
- **S-069-R18** Filter clipboard: `actionCopyFilters` stores per-filter settings JSON; `actionPasteFilters`/`SourcePasteFilters` recreate in order; `CreateFilterPasteUndoRedoAction` (undo = remove by name, redo = re-add); `SceneCopyFilters`/`ScenePasteFilters` across selected items.
- **S-069-R19** Canvases: per-scene canvas bounds — `RemoveCanvas`/`ClearCanvases` (undo); `OBSCanvas::SaveCanvases` writes per-scene canvas data into the scene collection (S-048).

### 5.5 Transform dialog (`OBSBasicTransform.cpp/.hpp`)

- **S-069-R20** `setScene`/`setItem` subscribe the item signals (transform/removed/select/deselect/locked); `refreshControls` fills position/size/rotation/alignment/bounds/crop; `ignoreItemChange` suppresses write-back during self-updates; item removed → close.
- **S-069-R21** `onAlignChanged`: change alignment keeping the visible centre fixed — `pos −= (curRatio − newRatio)·size·scale` (exact math in §6).
- **S-069-R22** `onBoundsType`: index → `obs_bounds_type`; sub-widgets enabled accordingly (alignment off for `STRETCH`; crop-to-bounds only for `SCALE_OUTER`/`SCALE_TO_WIDTH`/`SCALE_TO_HEIGHT`). First change away from `NONE`: size ← source w/h, bounds ← (w−crop)·scale; change to `NONE`: size ← draw transform, pos re-derived top-left (§6); `onControlChanged`/`onCropChanged` write back with undo.

### 5.6 Transitions (`OBSBasic_Transitions.cpp`)

- **S-069-R23** `InitDefaultTransitions`: private source per *non*-configurable transition type; `fade_transition` + `cut_transition` are **required**, else `throw` (Q-2); defaults registered in the transition maps; `UpdateCurrentTransition(last)`.
- **S-069-R24** `AddTransition(id)` (NameDialog); remove (confirm; if in use, switch uses to a default first); `RenameTransition` (conflict check); persist in the scene collection (S-048).
- **S-069-R25** `SetTransition` drives `obs_get_output_source(0)`; duration = the source's `duration` setting (default 300 ms); per-scene overrides: `CreatePerSceneTransitionMenu`, `OverrideTransition`, `GetOverrideTransition(Duration)` — used when transitioning *to* that scene.
- **S-069-R26** `TransitionToScene(scene, force, …)`: no-op if already showing and not forced; else drive the main transition (effective or quick duration), emit `TransitionStarted`/`Stopped`, update `programScene`/`lastProgramScene`; `SetCurrentScene` = the *preview* scene.
- **S-069-R27** Quick transitions: defaults = cut 300 ms, fade 300 ms, fade-to-black (fade 300 ms, `fadeToBlack`); persist/restore as `{name, duration, id, fade_to_black, hotkeys}` (load matches by name via `transitionNameToUuids`, re-sequences ids); per-entry hotkeys; `RefreshQuickTransitions` rebuilds widgets; `QuickTransitionChange(Duration)`.
- **S-069-R28** T-bar: first grab resolves the transition (scene override → current → display-name "fade"; `cut_transition`/`obs_stinger_transition` invalid → override), manual torque (8.0, 0.05), start manual transition; drag → `set_manual_time(clamp(value/1024, 0.01, 0.99))` + `OBS_FRONTEND_EVENT_TBAR_VALUE_CHANGED`; release: `(max−value) ≤ 102` → manual 1.0 (finish), `value ≤ 102` → manual 0.0 + `TransitionFullyStopped` + `programScene = lastProgramScene` (cancel); bar reset without signals.

### 5.7 Main control bar (`OBSBasicControls.cpp`)

- **S-069-R29** Pure button state machine on the main-window signals: streaming Preparing (disable) / Starting / Started (`withDelay` → "Live (delay)") / Stopping / Stopped; recording Started (`pausable`) / Paused / Unpaused / Stopping / Stopped; replay Started / Stopping / Stopped; vcam Started / Stopped; `UpdateStudioModeState` swaps the stream button for the preview/program toggle; `EnableBroadcastFlow`/`EnableReplayBufferButtons`/`EnableVirtualCamButtons` gate optional buttons; clicks → `*ButtonClicked` → `*ActionTriggered` (R30–R37).

### 5.8 Streaming (`OBSBasic_Streaming.cpp`)

- **S-069-R30** `StartStreaming`: no-op if active or `disableOutputsRef`; broadcast-flow auth without active/ready broadcast → set-up-broadcast dialog; `StreamingPreparing`; `setupStreamingGuard = SetupStreaming(service, finish)` — success: `STREAMING_STARTING` event, `SaveProject`, `StreamingStarting(autoStartBroadcast)`, start output (fail → `DisplayStreamStartError`); `autoStartBroadcast` → `BroadcastStreamStarted(autoStopBroadcast)`; auto-start recording/replay buffer iff `RecordWhenStreaming`/`ReplayBufferWhileStreaming` (user config).
- **S-069-R31** `StopStreaming`: `SaveProject`, stop output (passing `streamingStopping`); broadcast reset (`!autoStart && !active` → force-reset flags, Q-5; `autoStop` → clear active/ready); `BroadcastStreamReady`; `OnDeactivate`; stop recording iff `RecordWhenStreaming && !KeepRecordingWhenStreamStops`; stop replay buffer iff `ReplayBufferWhileStreaming && !KeepReplayBufferStreamStops`; `ForceStopStreaming` = immediate.
- **S-069-R32** `StreamDelayStarting/Stopping(sec)` drive the status-bar countdown (R46); `StreamingStart`/`StreamStopping`/`StreamingStop(code, err)` handle reconnects (reconnect signal) + errors; `StreamActionTriggered` = button toggle.

### 5.9 Recording (`OBSBasic_Recording.cpp`)

- **S-069-R33** `StartRecording`: no-op if active or `disableOutputsRef`; invalid path → path-invalid message; `LowDiskSpace` (R34) unless FFmpeg-URL output → disk message; then `RECORDING_STARTING` event, `SaveProject`, start output. `RecordingStart` (signal): status bar, `RecordingStarted(pausable)`, tray text, start `diskFullTimer` (1000 ms), `OnActivate`, log.
- **S-069-R34** `LowDiskSpace`: free space at the output path < 50 MiB (`MBYTES_LEFT_STOP_REC 50`); `CheckDiskSpaceRemaining` (timer tick) → `StopRecording` + `StopReplayBuffer` + critical dialog.
- **S-069-R35** Pause/Unpause only when `isRecordingPausable` (container supports pause). `StopRecording` = `SaveProject` + stop; `RecordingStop(code, err)`: status bar, `RecordingStopped`, tray text, `OnDeactivate`, `RECORDING_STOPPED` event, and — when the format requires it — `AutoRemux(input, no_show)` (remux to the configured container, error dialog on failure); `on_actionShow_Recordings_triggered` opens the folder.

### 5.10 Replay buffer & virtualcam (`OBSBasic_ReplayBuffer.cpp`, `OBSBasic_VirtualCam.cpp`, `OBSBasicVCamConfig.cpp`)

- **S-069-R36** Replay buffer: toggle with `ShowReplayBufferPauseWarning` (starting while recording paused); `StartReplayBuffer` (max seconds from config) / `StopReplayBuffer`; `ReplayBufferSave` saves the newest N seconds (collision-safe name); start/stop/saved signals update the control bar + log.
- **S-069-R37** Virtualcam: `StartVirtualCam`/`StopVirtualCam`; `OnVirtualCamStart`/`OnVirtualCamStop` (output signals) update state + control bar; `RestartVirtualCam(config)` stop-and-start; `UpdateVirtualCamConfig` applies changes (restarting when active); `VirtualCamActionTriggered` toggles.
- **S-069-R38** `OBSBasicVCamConfig`: `OutputTypeChanged` rebuilds the target list — `ProgramView`/`PreviewOutput` disable it; `SceneOutput` = scenes in default order; `SourceOutput` = VIDEO-flag sources, alphabetical; preselect the configured entry; active vcam switching to/from `ProgramView` → `requireRestart` warning; `UpdateConfig` persists (unknown type → no save), active vcam → `UpdateVirtualCamConfig`.

### 5.11 Studio (preview/program) mode (`OBSBasic_StudioMode.cpp`)

- **S-069-R39** `SetPreviewProgramMode(enabled)`: no-op if unchanged; atomic flag + `PreviewProgramModeChanged`. Enable: ensure preview, `CreateProgramDisplay`/`CreateProgramOptions`, duplicate the current scene (`sceneDuplicationMode`: edit-properties → `OBS_SCENE_DUP_PRIVATE_COPY`, else `PRIVATE_REFS`; else share the source), point output 0 at the duplicate, `inc_showing`, weak refs `lastScene`/`programScene`, `RefreshQuickTransitions`, program label + widgets, `sizeObserver = PreviewProgramSizeObserver(preview, program)`, `STUDIO_MODE_ENABLED` event, log.
- **S-069-R40** Disable: strong-ref `programScene` (fallback current scene source), `SetCurrentScene` + `TransitionToScene(force)`, delete widgets/observer. `RenderProgram` renders the program output each frame; `ResizeProgram`; `UpdatePreviewProgramIndicators`; `ProgramViewContextMenuRequested`; `Enable/DisablePreviewProgram`; `OpenStudioProgramProjector/Window` (R41).

### 5.12 Projectors & multiview (`OBSProjector.cpp`, `OBSBasic_Projectors.cpp`, `Multiview.*`)

- **S-069-R41** `OBSProjector`: frameless top-level on a chosen monitor (`SetMonitor`); renders its source via `OBSRender` or a multiview grid via `OBSRenderMultiview`; `OpenFullScreen` (work area) vs `OpenWindowedProjector` (`ResizeToContent`); `AlwaysOnTopToggled`.
- **S-069-R42** Management: `LoadSavedProjectors` restores `SavedProjectorInfo` from the profile after load; `Open{Preview,Source,Multiview,Scene}Projector` (fullscreen) / `Open{Preview,Source,Scene}Window` (windowed); `OpenSavedProjector`; `DeleteProjector`/`ClearProjectors`; `updateMultiviewProjectorMenu` syncs the tray menu (R60).
- **S-069-R43** `Multiview`: grid of scene tiles — `Update(layout, drawLabel, drawSafeArea)` computes tile rects (label row + safe-area overlay); `Render` draws the tiles; `GetSourceByPosition` maps a point to the scene under the cursor (click switches to it).

### 5.13 File drops (`OBSBasic_Dropfiles.cpp`)

- **S-069-R44** `dragEnter`/`dragMove` accept files/URLs; `dropEvent` classifies each drop: Win `.url` file → INI key `InternetShortcut/URL`; local file by extension (R4 tables) → `Text`/`Image`/`Html`/`Media`; remote URL → `ConfirmDropUrl` → `AddDropURL` (media_input).
- **S-069-R45** `AddDropSource(data, type)` creates: `RawText` → inline text (Win `text_gdiplus`, else `text_ft2_source`); `Text` → file-backed text (Win `read_from_file`+`file`, else `from_file`+`text_file`); `Image` → `image_source`+`file`; `Media` → `ffmpeg_source`+`local_file`; `Html` → `browser_source`+`is_local_file`+`local_file`+base-resolution `width`/`height`; `Url` → `browser_source` (R44). Id remapped by `obs_get_latest_input_type_id` (unknown → ignored, no dialog); name = file name (or display name) uniquified by `GenerateSourceName` (§6); created, saved to JSON for redo, added to the current scene, "Add source" undo pushed.

### 5.14 Status bar & stats (`OBSBasicStatusBar.cpp`, `OBSBasicStats.cpp`, `OBSBasic_StatusBar.cpp`)

- **S-069-R46** Status bar: `Activate`/`Deactivate` toggle timers; `UpdateFPS` (warning < 95 %, danger < 80 % of target); `UpdateStreamTime`/`UpdateRecordTime` (HH:MM:SS); `UpdateDroppedFrames`; `UpdateBandwidth`; `UpdateCPUUsage`; `UpdateDelayMsg` (streaming-delay countdown); `Reconnect` + `OBSOutputReconnect` signal; `OBSBasic_StatusBar.cpp` = the `ShowStatusBarMessage` one-shot API.
- **S-069-R47** Stats: opened by `OBS_FRONTEND_EVENT_STATS`; 1 s timer while visible; `Update` = active FPS vs target (`text-danger` < 0.8×, `text-warning` < 0.95×), CPU (`os_cpu_usage_info_query`), free disk at the output path (MB/GB/TB), per-output labels (`OutputLabels::Update` for stream + record: bitrate, dropped/total, reconnects); recording time-remaining (`StartRecTimeLeft`/`RecordingTimeLeft`); `Reset` clears on close.

### 5.15 Main-window dialogs (`OBSBasicProperties.cpp`, `OBSBasicFilters.cpp/.hpp`, `OBSBasicInteraction.cpp`, `OBSBasicAdvAudio.cpp`)

- **S-069-R48** `OBSBasicProperties`: one per source id, hosts the libobs property view (S-015/S-025); `restoreDefaultsClicked` (confirm + defaults, undo); `previewTransitionClicked` (transitions: 1 s preview); `DrawPreview`/`DrawTransitionPreview` render callbacks; `CheckSettings`/`ConfirmQuit` confirm discard; OK = save (`obs_source_update` + undo), Cancel = close.
- **S-069-R49** `OBSBasicFilters`: filter list — `AddFilter`/`RemoveFilter` (undo), `ReorderFilters`; `UpdatePropertiesView(row, async)` shows the selected filter's properties (async vs effect lists); `AddNewFilter(id)` = §6 algorithm; `UpdateSplitter` divides the panes.
- **S-069-R50** `OBSBasicInteraction`: input routing for browser sources — `nativeEvent` (Win `WM_MOVE`/`WM_DISPLAYCHANGE`) re-lays-out displays; `GetSourceRelativeXY` maps mouse → source-relative via the draw transform; `HandleMouseClick/Move/Wheel/Focus/Key` → obs-interact (Shift/Alt; macOS Control↔Meta → `INTERACT_CONTROL/COMMAND_KEY`; mouse left/middle/right; keypad flag for keys); `DrawPreview` renders the source.
- **S-069-R51** `OBSBasicAdvAudio` (shell): `EnumSources` → one `OBSAdvAudioCtrl` row per audio source (row S-072 R32–R34); live via `OBSSourceAdded/Removed/Activated`; `on_usePercent_toggled` (dB↔%); `on_activeOnly_toggled`/`SetShowInactive`; `SetIconsVisible`.

### 5.16 Source-select dialog (`OBSBasicSourceSelect.cpp`)

- **S-069-R52** Type list: `rebuildSourceTypeList` from visible non-output source ids (module display names); select + Create → `createNew` → `setupNewSource` (NameDialog, create source, add scene item, undo); `getNewSourceName` uniquifies (" (2)", " (3)", …). Existing tab (`sourceButtonToggled`): `updateExistingSources` lists sources already in the scene; double-click/drop (`sourceDropped(uuid)`) / add → `addSelectedSources` → `setupSceneItem`/`setupExistingSource` (add item, or duplicate source) with undo; `sourcePaste` pastes from the clipboard (R17).

### 5.17 Auto-config wizard (`AutoConfig*`, `TestMode.hpp`)

- **S-069-R53** Structure: Start → Stream → Test → Video → results; `done(Accept)` → `SaveStreamSettings` (service + encoder) + `SaveSettings` (video settings into the profile), Cancel → no writes; `TestHardwareEncoding` probes HW encoders; `CanTestServer` gates the bandwidth test.
- **S-069-R54** Stream page: `LoadServices` (all/registered), OAuth account connect/disconnect, custom-server fields, `validatePage` requires server + key (or connected account), `on_preferHardware_clicked`; `ServiceChanged`/`UpdateServerList` keep the UI in sync. `TestMode`: ctor saves the video info, registers a random-noise render callback, disables all six output sources; `SetVideo(128, 128, 60, 1)`; dtor restores.
- **S-069-R55** Bandwidth test (`TestBandwidthThread`, background thread): x264 (CBR, `veryfast`, keyint 2, `startingBitrate`) + ffmpeg_aac (32 k) + `rtmp_common`/`rtmp_custom` service; stream key suffixed `?bandwidthtest` (Twitch/AmazonIVS) or `?test=true` (Restream.io); `bind_ip`/`ip_family` from user config `[Output]`; each candidate server (`GetServers`, or the single custom) probed ~10 s; highest sustainable bitrate within the service max is adopted.
- **S-069-R56** Encoder tests: `TestSoftwareEncoding` through `null_output` (x264 streaming → CBR `idealBitrate`, keyint 2, `main`, `veryfast`; recording → CRF 20, `high`, `veryfast`; aac 32 k) measuring FPS; `TestStreamEncoderThread`/`TestRecordingEncoderThread` run the selected HW encoder at target resolution/FPS.
- **S-069-R57** `FindIdealHardwareResolution` (§6 ladder: `maxDataRate` = 1920·1200·60+1000 px/s for ≥ 4 physical cores else 1280·720·30+1000; non-NVENC HW encoders bump min-bitrate × 114/100; ≤ 3 accepted). `FinalizeResults`: temp service + `obs_service_apply_encoder_settings` → recommended bitrate; ideal resolution clamped to the closest supported service resolution, ideal FPS to the service max; results form shows multitrack "Automatic" vs fixed. Video page (`validatePage`) validates the existing base resolution/FPS inputs only.

### 5.18 Main-window glue (`OBSBasic_Hotkeys.cpp`, `OBSBasic_Service.cpp`, `OBSBasic_SysTray.cpp`, `OBSBasic_Updater.cpp`, `OBSBasic_Browser.cpp`)

- **S-069-R58** `InitHotkeys`/`CreateHotkeys` register the main-window frontend hotkeys from `[Hotkeys]` (stats, screenshot, source screenshot, start/stop stream, force-stop stream, start/stop record, pause record, split file, add chapter, replay start/stop/save, vcam start/stop, …); `ClearHotkeys` on shutdown; hotkey *editing* = S-068.
- **S-069-R59** Service: `LoadService` (profile `service.json`, S-048) → `InitService` (create the `obs_service`); `SetService` (attach to the stream output + dependent UI); `SaveService` (persist).
- **S-069-R60** Sys tray: `SystemTrayInit` (icon + menu: show/hide, start/stop stream, start/stop record, projector submenu); `IconActivated` (double-click → show + activate main window); `SysTrayNotify` (balloons); `updateSysTrayProjectorMenu` (R42).
- **S-069-R61** Updater: `TimedCheckForUpdates` (periodic), `CheckForUpdates` (manual; dialog S-072 R52), `ReceivedIntroJson` (version metadata), `ShowWhatsNew` (changelog), `MacBranchesFetched` (stable/beta). Browser docks: load/save `extraBrowserDocks` (title/url/uuid) in the profile, `GenId`, manage dialog S-072 R39–R41, `InitBrowserPanelSafeBlock` defers dock creation.

## 6. Algorithms

Transform alignment shift (R21/R22):
`w = source.width − crop.left − crop.right` (or bounds.w if bounds set),
`h` likewise; `pos.x −= (curRatio.x − newRatio.x)·w·scale.x`,
`pos.y −= (curRatio.y − newRatio.y)·h·scale.y`, where the alignment
conversion yields {0, 0.5, 1} per axis. NONE→bounds: size ← source w/h,
bounds ← (w−crop)·scale; bounds→NONE: size ← draw-transform w/h, pos ←
top-left of the draw transform re-expressed through the old alignment.

`GenerateSourceName(base)`: first free of `base`, `base (2)`,
`base (3)`, … by source name (R45/R52).

Auto-config resolution ladder (R57): for cy in 2160, 1440, 1080, 720,
480, 360 (240 forced): `cx = baseCX·cy/baseCY`; accept iff
`cx·cy·fps ≤ maxDataRate` (or forced/recording) and `idealBitrate ≥
minBitrate` (HW: ×1.14 unless NVENC); stop after 3 accepted.

Quick-transition persistence (R27): JSON array `{name, duration, id,
fade_to_black, hotkeys}`; load matches `name` via
`transitionNameToUuids`, re-sequences ids (counter = max+1).

## 7. Concurrency

- GUI-thread for all widgets; obs-thread signals queued before mutation.
  Output state flags (R03) use **direct** connections (must update
  synchronously with the signal).
- Atomics: `previewProgramMode` (render-thread read, R39), `currentScene`;
  scene/program refs are weak refs resolved on the GUI thread.
- Background threads: wizard bandwidth/encoder tests (R55/R56) own
  throwaway obs objects and report via queued `Progress` /
  `UpdateMessage` / `Failure` slots; `TestMode` swaps the *global*
  output sources while active (restored in dtor).
- Timers (GUI thread): `diskFullTimer` 1000 ms (R34), stats 1000 ms
  (R47), `nudge_timer`, updater timer (R61).

## 8. Reference defects and quirks

- **Q-1** `GetFPSNanoseconds` is defined but never dispatched (FPSType 3
  commented out) — dead code; observable behaviour unchanged if dropped.
- **Q-2** `InitDefaultTransitions` throws a raw `std::string` (UI HTML)
  — the contract is "fade+cut must exist", not the exception type.
- **Q-3** The t-bar clamps the drag to [0.01, 0.99] but releases with a
  102/1024 dead zone — small residual drags silently cancel the
  transition and restore the program scene.
- **Q-4** Bandwidth tests append `?bandwidthtest` (Twitch/IVS) /
  `?test=true` (Restream.io) to the stream key — exact suffixes MUST be
  kept or tests count as real streams.
- **Q-5** `StopStreaming` implicitly force-resets the broadcast flags
  when `!autoStartBroadcast && !broadcastActive` (leftover
  `broadcastReady` from a previous cycle).
- **Q-6** Studio mode shares the scene source unless
  `sceneDuplicationMode` — without it, preview edits hit program live.
- **Q-7** Drop classification uses hard-coded extension tables; unknown
  extensions are silently ignored (no error dialog).

## 9. Implementation notes (non-normative)

- Keep the reference's per-feature `OBSBasic_*.cpp` split as Rust module
  boundaries (preview / transitions / outputs / studio / projectors /
  wizard).
- Model each output (stream/record/replay/vcam) as one `OutputState`
  (idle/starting/active/stopping); the control bar is a pure projection
  of those states.
- Decouple `PreviewViewState` (scaling, scroll, fixed) from S-067's
  pointer logic — same signals, different modules.
- Undo payloads are `obs_source`-state JSON (R17/R18/R45/R49) — keep the
  format so undo stays data-driven.

## 10. Conformance coverage

| Rule(s) | Scripts |
|---|---|
| all S-069-Rxx | none yet (Qt UI). Pure-logic candidates: FPS map (R04), t-bar clamp/dead-zone (R28), alignment shift (R21), `GenerateSourceName` (R45), resolution ladder (R57) |

## 11. Open questions

1. Interaction split with S-067: this spec pins the preview state and
   signals; S-067 must pin the interaction that reads/writes them — one
   reconciliation pass needed before implementation.
2. The `OBSBasicPreview` draw family (selection box, snap guides,
   spacing helpers) is rendering-only but deferred to S-067; confirm.
3. Projector re-layout on display change (R08/R41) is platform-specific;
   needs an ADR for the Rust target platform.
4. Updater endpoints and sys-tray behaviour (R60/R61) are OS-specific;
   only the state machine is pinned — conformance scope undefined.
5. `EstimateMinBitrate` (used by R57) was not transcribed line by line;
   only its use is pinned.
