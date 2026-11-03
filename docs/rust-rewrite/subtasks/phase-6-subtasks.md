# Phase 6 subtasks

Parent file: [../phase-6-frontend.md](../phase-6-frontend.md). Each subtask has **Files**, **Steps**, **Done when** (+ `Spec`/`Vectors`/`Pitfalls` where applicable).

Verified facts: the main window is split across `frontend/widgets/OBSBasic*.cpp` (33 files, ≈20K lines: `_Docks`, `_Hotkeys`, `_Preview`, `_Profiles`, `_SceneCollections`, `_Scenes`, `_SceneItems`, `_Streaming`, `_Recording`, `_ReplayBuffer`, `_StudioMode`, `_Transitions`, `_VirtualCam`, `_SysTray`, `_Updater`, `_YouTube`, …). `frontend/forms/` has 36 `.ui` files (29 top-level, 7 in `source-toolbar/`; `AutoConfigFinishPage.ui` is an orphan form referenced only by the build, never instantiated). `frontend/data/locale/` has 77 locale files. Themes in `frontend/data/themes/`: two `.obt` base themes (`System.obt`, hidden from the theme list, and `Yami.obt`) plus `Yami_*.ovt` variants (Acri, Classic, Default, Grey, Light, Rachni); the `Acri/`, `Dark/`, `Light/`, `Rachni/`, `Yami/` subdirs hold theme images; high-contrast themes use the `.oha` extension. The frontend API (`frontend/api/obs-frontend-api.h`) has 99 exported functions and 44 `OBS_FRONTEND_EVENT_*` values. Scripting bindings are SWIG interfaces: `shared/obs-scripting/obslua/obslua.i`, `obspython/obspython.i`. `plugins/obs-websocket` and `plugins/obs-browser` (needed by several tasks) are initialised in this checkout; a fresh clone still needs `git submodule update --init`.

**UI bookend** (written "+UI"): (a) view-model unit test before the widget, (b) screenshot/accessibility-tree snapshot in light and dark themes, (c) full keyboard-only operation, (d) string keys taken from `frontend/data/locale/en-US.ini` (no new hard-coded text).

## A. Application shell

### P6-01 Launch and CLI
- [ ] **P6-01.1 Argument table**
  - Files: `frontend/obs-main.cpp` (1108; flag parsing at 958–1062, `arg_is` at 815).
  - Steps: 1. grep every `arg_is(...)` in `obs-main.cpp`; 2. table each flag (flag, short form, variable set); 3. read `run_program` (487+) and record the startup branch each flag controls.
  - Spec: S-048 §5.1 (R01–R04, R08).
  - Vectors: test/conformance/scripts/frontendfmt/ (`cli_flags`).
  - Pitfalls: the 22 flags are the complete set at 33.0.1 — re-grep when starting. `--collection/--profile/--scene` swallow the next argument verbatim even if it looks like a flag; a trailing value flag is silently ignored (S-048 R03, Q-3).
  - Done when: table lists all with semantics.
- [ ] **P6-01.2 Parser**
  - Files: `frontend/obs-main.cpp` (1108; parse loop 958–1062).
  - Steps: 1. implement with `clap` in `crates/frontend-core`; 2. exact, case-sensitive match, unknown args skipped silently (R01); 3. value flags consume the next argument (R03); 4. `--help`/`--version` exit at the point they are seen (R04); 5. add a parity test running each flag.
  - Spec: S-048 §5.1 (R01–R04).
  - Vectors: test/conformance/scripts/frontendfmt/ (`cli_flags`).
  - Done when: parity test runs each flag.
- [ ] **P6-01.3 Single instance**
  - Files: `frontend/obs-main.cpp` (1108; check at 550–600), `frontend/utility/platform-windows.cpp` (446; `CheckIfAlreadyRunning` at 303, `RunOnceMutex`), `frontend/utility/platform-x11.cpp` (327; at 71), `frontend/utility/platform.hpp` (55, declaration).
  - Steps: 1. port per-OS instance detection (Windows named mutex, X11 socket) behind a platform hook (cxx-qt per D-01); 2. implement the `--multi` bypass and its log line; 3. port the already-running prompt using locale keys `AlreadyRunning.Title`/`AlreadyRunning.Text`/`AlreadyRunning.LaunchAnyway`.
  - Pitfalls: a second instance clears the `unclean_shutdown` flag (obs-main.cpp:591) — keep that, or safe-mode prompts fire falsely.
  - Done when: second launch shows the dialog.
- [ ] **P6-01.4 Crash handler/log rotation**
  - Files: `frontend/utility/CrashHandler.cpp` (392), `CrashHandler.hpp` (97), `CrashHandler_Windows.cpp` (85), `CrashHandler_Linux.cpp` (40), `CrashHandler_FreeBSD.cpp` (40), `CrashHandler_MacOS.mm` (115); `frontend/obs-main.cpp` (325–360, `delete_oldest_file`).
  - Steps: 1. port the crash handlers per OS (minidump / signal paths); 2. keep the `logs/` + `crashes/` directory layout; 3. implement oldest-log deletion by filename timestamp; 4. forced-crash self-check.
  - Pitfalls: deletion is governed by `global.ini` `[General] MaxLogs`, default 10 (`config_set_default_uint`, OBSApp.cpp:326; read at obs-main.cpp:332); only files matching the log-name pattern count.
  - Done when: forced crash writes report.
- [ ] **P6-01.5 OS quirks**
  - Files: `frontend/obs-main.cpp` (1108; `QT_QPA_PLATFORMTHEME` unset at 521–524), `frontend/OBSApp.cpp` (2107; `OBSInit` 1274–1300, `obs_set_nix_platform` EGL/X11/Wayland), `frontend/utility/platform-windows.cpp` (446), `frontend/utility/platform-osx.mm` (361).
  - Steps: 1. list the per-OS quirks (DPI awareness, priority class, App Nap, X11-vs-Wayland choice); 2. port each into `crates/frontend-core` behind platform features; 3. log the per-OS choice at startup.
  - Done when: logged per OS.

### P6-02 Application object and config
- [ ] **P6-02.1 Config**
  - Files: `frontend/OBSApp.cpp` (2107; `InitGlobalConfigDefaults` 324–357, `InitUserConfigDefaults` 376–407, global/user open 568–645), `frontend/OBSApp.hpp` (299).
  - Steps: 1. load `global.ini` + `user.ini` via `crates/obs-config` (S-007); 2. run Pre31 split + `MigrateLegacySettings` before defaults; 3. apply the defaults tables (S-048 §4.1–4.2); 4. round-trip test on real files.
  - Spec: S-048 §4.1–4.2, §5.2 (R10–R18).
  - Vectors: test/conformance/scripts/frontendfmt/ (`config_defaults`, `migrate_user`).
  - Pitfalls: exact defaults — `General.MaxLogs` 10, `General.InfoIncrement` -1, `Video.Renderer` `Direct3D 11` on Windows / `OpenGL` elsewhere (OBSApp.cpp:326–352); a failed Pre31 copy leaves `Pre31Migrated=false` and retries on every start (S-048 Q-1).
  - Done when: reads/writes real files.
- [ ] **P6-02.2 Paths/portable**
  - Files: `frontend/obs-main.cpp` (1108; portable detection 1065–1079), `frontend/OBSApp.cpp` (2107; `InitGlobalLocationDefaults` 358–373).
  - Steps: 1. implement portable detection (marker files, BASE_PATH `..` on L/M, `../..` on W); 2. implement per-OS config dirs + the four `Locations.*` overrides; 3. test that the whole tree moves with the folder.
  - Spec: S-048 §3.1, §5.1 (R05–R06).
  - Vectors: test/conformance/scripts/frontendfmt/ (`config_defaults`).
  - Pitfalls: portable mode is implied by any of `portable_mode`, `obs_portable_mode`, `portable_mode.txt`, `obs_portable_mode.txt`; `disable_updater[.txt]` and `disable_missing_files_check[.txt]` live in the same dir (obs-main.cpp:1065–1079, S-048 R05).
  - Done when: moves with the folder.
- [ ] **P6-02.3 First run**
  - Files: `frontend/OBSApp.cpp` (2107), `frontend/widgets/OBSBasic.cpp` (2101; `General.FirstRun` at 1158–1161), `frontend/widgets/OBSBasic_SceneCollections.cpp` (1705; `CreateDefaultScene` 1159), `frontend/widgets/OBSBasic_SceneItems.cpp` (1433; `CreateFirstRunSources` 91).
  - Steps: 1. detect missing profile/collection on a clean config; 2. create the default profile + collection and the first-run sources (S-048 R52); 3. clean-profile test.
  - Spec: S-048 §5.6 (R52).
  - Done when: clean-profile test.

### P6-03 Themes
- [ ] **P6-03.1 Parser**
  - Files: `frontend/OBSApp_Themes.cpp` (1120; `ParseThemeMeta` 46, `ParseThemeVariables` 290, dir scan 472–485), `frontend/utility/OBSTheme.hpp` (47), `frontend/utility/OBSThemeVariable.hpp` (44).
  - Steps: 1. port the `.obt`/`.ovt` syntax (meta, `@define`, variables, calc expressions); 2. model `OBSTheme` (id/name/extends/isDark/isBaseTheme/isHighContrast/font-scale/density); 3. parse test over all shipped themes.
  - Pitfalls: `System.obt` is a base theme but hidden from the theme list (`isVisible`); `.ovt` variants `extends` a base; high-contrast layers use `.oha` (OBSApp_Themes.cpp).
  - Done when: all shipped themes parse.
- [ ] **P6-03.2 Apply**
  - Files: `frontend/OBSApp_Themes.cpp` (1120; QSS generation + palette), `frontend/data/themes/System.obt` (477), `frontend/data/themes/Yami.obt` (2887).
  - Steps: 1. generate QSS (or toolkit-native styles) from the theme variables (cxx-qt per D-01); 2. port the palette + dark-mode flag; 3. port per-OS defaults (`System`); 4. snapshot the preview colours against the C build.
  - Done when: preview colours equal.
- [ ] **P6-03.3 Variants/high contrast/user themes**
  - Files: `frontend/OBSApp_Themes.cpp` (1120; user-theme dir scan 480–485), `frontend/data/themes/Yami_Acri.ovt` (232), `Yami_Classic.ovt` (286), `Yami_Default.ovt` (24), `Yami_Grey.ovt` (36), `Yami_Light.ovt` (444), `Yami_Rachni.ovt` (226).
  - Steps: 1. port `.ovt` overrides + the `extends` chain; 2. port the user theme dir `<config>/obs-studio/themes`; 3. port high-contrast `.oha` layers; 4. load a user theme test.
  - Pitfalls: the selected theme is `user.ini` `[Appearance] Theme`; legacy `General.CurrentTheme3` is migrated over (OBSApp_Themes.cpp:1076–1081); `Appearance.FontScale` (default 10) and `Appearance.Density` (default 1) feed QSS (S-048 §4.2).
  - Done when: user theme loads.
- [ ] **P6-03.4 Snapshots**
  - Files: `frontend/data/themes/System.obt` (477), `Yami.obt` (2887), the six `Yami_*.ovt` variants, theme image dirs `Acri/ Dark/ Light/ Rachni/ Yami/`.
  - Steps: 1. screenshot main window + one dialog per shipped theme, light and dark; 2. commit the snapshots.
  - Done when: screenshots per shipped theme committed.

### P6-04 Localisation
- [ ] **P6-04.1 Loader**
  - Files: `frontend/data/locale.ini` (170), `frontend/data/locale/*.ini` (77 files; `en-US.ini` 1729), `frontend/OBSApp.cpp` (2107; `GetLocaleNames` 1727, `InitLocale` 764–845).
  - Steps: 1. load `locale.ini` (tag→`Name` sections) via `crates/obs-config`; 2. build the text lookup: `locale/en-US.ini` as base, the selected `locale/<lang>.ini` added on top; 3. load test over all 77 files.
  - Spec: S-010 §text-lookup (the lookup container).
  - Pitfalls: the language is `user.ini` `[General] Language` (exact key, OBSApp.cpp:770); with no user value the preferred-locale scan picks the first installed file (OBSApp.cpp:800–824).
  - Done when: all 77 files load.
- [ ] **P6-04.2 Fallback/format**
  - Files: `frontend/OBSApp.cpp` (2107; `TranslateString` 1496–1506), `frontend/data/locale/en-US.ini` (1729).
  - Steps: 1. implement `Str()` as UI-translation hooks, then text lookup, else the key itself; 2. keep the Qt `%1` placeholder convention (expanded at call sites); 3. unit test the fallback.
  - Spec: S-010 §text-lookup.
  - Pitfalls: the lookup is a flat key→value map with no argument substitution (S-010); `%1`-style placeholders are expanded by callers via `QString::arg` — do not invent a `$1` convention.
  - Done when: tests.
- [ ] **P6-04.3 Coverage**
  - Files: `frontend/data/locale/en-US.ini` (1729); `Str(` / `QTStr(` call sites across `frontend/` (33 `OBSBasic*.cpp` files + the rest).
  - Steps: 1. script: extract every `Str("…")`/`QTStr("…")` key from the C sources; 2. diff against the `en-US.ini` keys; 3. report unused and missing keys.
  - Done when: report lists unused/missing.

### P6-05 Logging and crash upload
- [ ] **P6-05.1 Viewer** +UI
  - Files: `frontend/dialogs/OBSLogViewer.cpp` (130), `OBSLogViewer.hpp` (22), `frontend/forms/OBSLogViewer.ui` (142).
  - Steps: 1. port the view model (log lines, level filter, copy); 2. port the widget with the `.ui` fields (cxx-qt); 3. UI bookend (view-model test, a11y snapshot, keyboard-only).
  - Done when: UI bookend passes.
- [ ] **P6-05.2 Upload**
  - Files: `frontend/dialogs/LogUploadDialog.cpp` (150), `LogUploadDialog.hpp` (52), `frontend/forms/LogUploadDialog.ui` (306).
  - Steps: 1. port the log upload (HTTP POST of log + system info); 2. port the analyzer URL construction + copy button; 3. mock-server test.
  - Pitfalls: the analyzer link is `https://obsproject.com/tools/analyzer` with query parameters appended (LogUploadDialog.cpp:144–148) — keep the exact URL.
  - Done when: mock server test.
- [ ] **P6-05.3 Crash reports**
  - Files: `frontend/utility/CrashHandler.cpp` (392) + the five platform `CrashHandler_*` files (P6-01.4); `frontend/utility/system-info-posix.cpp` (676), `system-info-windows.cpp` (262), `system-info-macos.mm` (101), `system-info.hpp` (5).
  - Steps: 1. keep the crash report format (version, OS, system-info sections) identical; 2. unit test report assembly with fixed inputs.
  - Done when: report format diffs clean against a C sample.

### P6-06 Main window skeleton
- [ ] **P6-06.1 Window/menus** +UI
  - Files: `frontend/widgets/OBSBasic.cpp` (2101), `OBSBasic_MainControls.cpp` (711), `OBSBasic.hpp` (1705), `frontend/forms/OBSBasic.ui` (2294).
  - Steps: 1. port the window shell + menu bar from the `OBSBasic.ui` menu structure; 2. wire every `QAction` to its handler; 3. port the `MainControls` dock toggles; 4. UI bookend.
  - Pitfalls: the menu/action set is defined by `forms/OBSBasic.ui` + `OBSBasic_MainControls.cpp`; every action needs its locale key — the P6-48.1 parity checklist enumerates them.
  - Done when: all menu items present (list from the .ui).
- [ ] **P6-06.2 Status bar**
  - Files: `frontend/widgets/OBSBasicStatusBar.cpp` (620), `OBSBasicStatusBar.hpp` (107), `StatusBarWidget.cpp` (10), `StatusBarWidget.hpp` (19), `OBSBasic_StatusBar.cpp` (26), `frontend/forms/StatusBarWidget.ui` (451).
  - Steps: 1. port the status bar model (rec/stream time, CPU, FPS, kbps, dropped frames); 2. feed it from the obs stats signals (Phase 4); 3. UI bookend.
  - Done when: values live.
- [ ] **P6-06.3 Docks/geometry**
  - Files: `frontend/widgets/OBSBasic_Docks.cpp` (260), `frontend/widgets/OBSBasic.cpp` (2101; `DockState` restore 1118–1124, save 1751).
  - Steps: 1. port dock creation/registration; 2. implement base64 `DockState` save/restore in `user.ini` `[BasicWindow]`; 3. restart-restore test.
  - Pitfalls: the state is base64 of Qt's `QMainWindow::saveState()` bytes under key `BasicWindow.DockState` (OBSBasic.cpp:1118, 1751) — the byte format must stay Qt-compatible for mixed builds.
  - Done when: restart restores.
- [ ] **P6-06.4 Studio/always-on-top**
  - Files: `frontend/widgets/OBSBasic_StudioMode.cpp` (414), `frontend/widgets/OBSBasic.cpp` (2101; always-on-top toggle).
  - Steps: 1. port the studio-mode toggle + program/preview selection; 2. port the always-on-top flag (persisted); 3. emit `OBS_FRONTEND_EVENT_STUDIO_MODE_ENABLED/DISABLED` (ids 22/23, S-048).
  - Done when: toggles.
- [ ] **P6-06.5 Action table test**
  - Files: `frontend/widgets/OBSBasic*.cpp` (33 files, 19,974 lines — the action sources), `frontend/widgets/OBSBasic_Hotkeys.cpp` (344).
  - Steps: 1. generate the action table (action, shortcut, handler, locale key) from the C sources; 2. assert every action has a keyboard shortcut or handler; 3. commit the table (feeds P6-48.1).
  - Done when: table committed and the check passes.

## B. Core UI components

### P6-07 Preview widget
- [ ] **P6-07.1 Display embedding**
  - Files: `frontend/widgets/OBSQTDisplay.cpp` (214), `OBSQTDisplay.hpp` (49), `frontend/widgets/OBSBasicPreview.cpp` (2912; display creation), `frontend/widgets/OBSBasic_Preview.cpp` (683).
  - Steps: 1. create the native child window per platform (X11 window id, Wayland subsurface, HWND, NSView); 2. attach it through the Phase 4 display API; 3. render smoke test on all three OSes.
  - Done when: renders on all.
- [ ] **P6-07.2 Selection/handles** +UI
  - Files: `frontend/widgets/OBSBasic_SceneItems.cpp` (1433; hit-testing + handles), `frontend/widgets/OBSBasicPreview.cpp` (2912; mouse/key routing).
  - Steps: 1. write the behaviour table from the C (hit-test order, handle priority, Ctrl/Alt/Shift combos); 2. port hit-testing + rotate/scale/crop handles; 3. UI bookend (a11y snapshot).
  - Pitfalls: the modifier-key semantics (which key pairs enable rotate vs scale vs crop) live in `OBSBasic_SceneItems.cpp` — capture every combination in the table before porting.
  - Done when: behaviour table.
- [ ] **P6-07.3 Snapping** +UI
  - Files: `frontend/widgets/OBSBasic_SceneItems.cpp` (1433; snapping + spacing helpers).
  - Steps: 1. port snap to edges/centre/other sources + grid; 2. read the snap distance from `user.ini` `[BasicWindow] SnapDistance` (default 5.0); 3. unit test the snap geometry.
  - Pitfalls: `SnapDistance` is a double, default 5.0; a pre-33 value of exactly 10.0 is rewritten to 5.0 (S-048 R16, Q-4 exact float equality); toggles are `SnappingEnabled`/`ScreenSnapping`/`SourceSnapping`/`CenterSnapping` + `SpacingHelpersEnabled` (S-048 §4.2).
  - Done when: tests.
- [ ] **P6-07.4 Drop**
  - Files: `frontend/widgets/OBSBasic_Dropfiles.cpp` (342).
  - Steps: 1. port file/URL drop → image/media/browser/text source creation; 2. drop test with 5 file types.
  - Done when: drop of 5 file types.
- [ ] **P6-07.5 Scaling/overlays**
  - Files: `frontend/components/OBSPreviewScalingComboBox.cpp` (124), `OBSPreviewScalingComboBox.hpp` (64), `OBSPreviewScalingLabel.cpp` (31), `frontend/widgets/OBSBasicPreview.cpp` (2912; safe areas, overflow, `striped_line.effect` load at 56).
  - Steps: 1. port the scaling modes + safe-area overlays; 2. persist the `scaling_*` collection keys (S-048 §4.4); 3. golden overlay.
  - Done when: golden overlay.
- [ ] **P6-07.6 Interaction**
  - Files: `frontend/dialogs/OBSBasicInteraction.cpp` (402), `OBSBasicInteraction.hpp` (64), `frontend/forms/OBSBasicInteraction.ui` (48), `frontend/widgets/OBSBasic.cpp` (2101; `open_source_interaction` wiring).
  - Steps: 1. port the interaction window (mouse/key pass-through); 2. hook the frontend-API entry point (P6-36); 3. browser-source click test.
  - Done when: browser source receives click.
- [ ] **P6-07.7 Studio dual preview**
  - Files: `frontend/widgets/OBSBasic_StudioMode.cpp` (414), `frontend/widgets/OBSBasicPreview.cpp` (2912; dual displays).
  - Steps: 1. port the program/preview display pair + swap; 2. wire the trigger-transition button (events 22/23/24, S-048); 3. transition run test.
  - Done when: transition runs.
- [ ] **P6-07.8 Multiview/projectors**
  - Files: `frontend/components/Multiview.cpp` (779), `Multiview.hpp` (68), `frontend/widgets/OBSProjector.cpp` (499), `OBSProjector.hpp` (68), `frontend/widgets/OBSBasic_Projectors.cpp` (280).
  - Steps: 1. port the multiview grid (layout enum `BasicWindow.MultiviewLayout`, S-048 §4.2); 2. port the projector window (scene/source/studio/multiview types, S-048 R76); 3. windowed + fullscreen test.
  - Pitfalls: `MultiviewLayout` is an int enum 0–9; legacy string values are migrated (S-048 R14); projector type strings compare case-insensitively, unknown/NULL = Preview (S-048 R76).
  - Done when: windowed + fullscreen projector.
- [ ] **P6-07.9 Interaction behaviour spec (added)**
  - Files: `frontend/widgets/OBSBasic_SceneItems.cpp` (1433; hit-test + handles + snapping), `frontend/widgets/OBSBasicPreview.cpp` (2912; mouse/key routing, rotation).
  - Steps: 1. extract the behaviour table (hit-test order, handle priority, Ctrl/Alt/Shift rotate/scale/crop combinations, snap-distance rules) from the C before starting P6-07.2; 2. write `specs/S-067-preview-interaction.md` covering the pure-geometry parts (point-in-rotated-rect, handle hit boxes, snap candidate selection); 3. unit-test those geometry functions in the Rust crate (mouse input itself is not drivable by the conformance runner); 4. add the row to `specs/README.md`.
  - Pitfalls: P6-07.2 defers the table ("write the behaviour table from the C") — this subtask moves it up to a spec so the port has something to be checked against.
  - Done when: S-067 committed + geometry tests green.

### P6-08 Scene list
- [ ] **P6-08.1 Model/view** +UI
  - Files: `frontend/components/SceneTree.cpp` (237), `SceneTree.hpp` (42), `frontend/widgets/OBSBasic_Scenes.cpp` (1026).
  - Steps: 1. port the scene tree model (order, current scene); 2. port the list + grid views; 3. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-08.2 Reorder**
  - Files: `frontend/components/SceneTree.cpp` (237), `frontend/widgets/OBSBasic_Scenes.cpp` (1026).
  - Steps: 1. port drag-and-drop reorder + grid/list modes; 2. persist the order to the collection's `scene_order` (S-048 §4.4); 3. persistence test.
  - Done when: order persisted.
- [ ] **P6-08.3 Context menu** +UI
  - Files: `frontend/widgets/OBSBasic_Scenes.cpp` (1026), `frontend/utility/SceneRenameDelegate.cpp` (57), `frontend/widgets/OBSBasic_Transitions.cpp` (1569; per-scene transition override).
  - Steps: 1. port the context menu (rename, duplicate, remove, filters, projector, screenshot, transition override); 2. port rename-in-place; 3. UI bookend.
  - Done when: all items work.

### P6-09 Source tree
- [ ] **P6-09.1 Model** +UI
  - Files: `frontend/components/SourceTreeModel.cpp` (450), `SourceTreeModel.hpp` (47), `SourceTreeItem.cpp` (598), `SourceTreeItem.hpp` (80), `SourceTree.cpp` (662), `SourceTree.hpp` (75).
  - Steps: 1. port the source tree model (sources + groups, expand/collapse); 2. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-09.2 Row widgets**
  - Files: `frontend/components/VisibilityItemWidget.cpp` (78), `VisibilityItemWidget.hpp` (35), `VisibilityItemDelegate.cpp` (70), `frontend/components/OBSSourceLabel.cpp` (60).
  - Steps: 1. port the visibility checkbox + lock + label; 2. port rename-in-place; 3. toggle tests.
  - Done when: toggles.
- [ ] **P6-09.3 Reorder/multi-select**
  - Files: `frontend/components/SourceTree.cpp` (662), `SourceTreeDelegate.cpp` (17), `frontend/widgets/OBSBasic_SceneItems.cpp` (1433; group/ungroup).
  - Steps: 1. port drag reorder across groups + multi-select; 2. port group/ungroup; 3. model unit tests.
  - Done when: tests.
- [ ] **P6-09.4 Context menus**
  - Files: `frontend/widgets/OBSBasic_SceneItems.cpp` (1433), `frontend/widgets/OBSBasic_ContextToolbar.cpp` (305).
  - Steps: 1. port the source context menus (transform, order, scale filtering, blending, show/hide transitions, deinterlacing, audio mixers); 2. route each entry to `obs-ffi` (Phase 1–4 crates); 3. table-test each entry.
  - Done when: each item hits `obs-ffi`.
- [ ] **P6-09.5 Accessibility** +UI
  - Files: `frontend/components/SourceTree.cpp` (662), `SourceTreeItem.cpp` (598), `SourceTreeModel.cpp` (450), `frontend/components/VolumeAccessibleInterface.cpp` (62).
  - Steps: 1. assign a11y roles and names for the tree rows; 2. commit the accessibility-tree snapshot.
  - Done when: accessibility-tree snapshot passes.

### P6-10 Audio mixer
- [ ] **P6-10.1 Meter**
  - Files: `frontend/components/VolumeMeter.cpp` (908), `VolumeMeter.hpp` (207).
  - Steps: 1. port the meter paint (peak/magnitude/input-peak bars, decay, clip indicator, warning/error thresholds); 2. use the S-032 metering logic from `crates/obs-audio-mix`; 3. pixel-compare against reference frames.
  - Spec: S-032.
  - Vectors: test/conformance/scripts/audiomix/.
  - Done when: pixel compare with reference frames.
- [ ] **P6-10.2 Fader/balance**
  - Files: `frontend/components/VolumeSlider.cpp` (80), `BalanceSlider.hpp` (21, header-only), `AbsoluteSlider.cpp` (167), `frontend/components/VolumeControl.cpp` (999), `VolumeControl.hpp` (171).
  - Steps: 1. port the fader widget on the S-032 dB curve; 2. port balance + absolute sliders (dB/percent); 3. curve test against P2-14.
  - Spec: S-032.
  - Vectors: test/conformance/scripts/audiomix/.
  - Done when: curve matches P2-14.
- [ ] **P6-10.3 Mute/monitor**
  - Files: `frontend/components/MuteCheckBox.hpp` (26, header-only), `frontend/components/VolumeControl.cpp` (999; per-source monitor menu).
  - Steps: 1. port the mute checkbox; 2. port the per-source monitor-type menu; 3. toggle tests.
  - Done when: toggles.
- [ ] **P6-10.4 Layouts**
  - Files: `frontend/widgets/AudioMixer.cpp` (1123).
  - Steps: 1. port vertical/horizontal/hidden-sources modes; 2. persist `MixerShowInactive`/`MixerKeepInactiveLast`/`MixerShowHidden`/`MixerKeepHiddenLast` (S-048 §4.2); 3. persistence test.
  - Done when: persisted.
- [ ] **P6-10.5 Advanced audio**
  - Files: `frontend/dialogs/OBSBasicAdvAudio.cpp` (195), `frontend/components/OBSAdvAudioCtrl.cpp` (582), `OBSAdvAudioCtrl.hpp` (90), `frontend/forms/OBSAdvAudio.ui` (349).
  - Steps: 1. port the advanced audio table (sync offset, tracks, monitoring, balance); 2. route edits to source settings via `obs-ffi`; 3. UI bookend.
  - Done when: all fields.
- [ ] **P6-10.6 Reference compare**
  - Files: `frontend/components/VolumeMeter.cpp` (908), `frontend/components/VolumeControl.cpp` (999).
  - Steps: 1. capture C mixer renders as reference frames; 2. diff the Rust renders with a threshold.
  - Done when: pixel diff within threshold.

### P6-11 Transitions dock
- [ ] **P6-11.1 Selector/duration** +UI
  - Files: `frontend/widgets/OBSBasic_Transitions.cpp` (1569).
  - Steps: 1. port the transition selector + duration field; 2. persist `transitions`/`transition_duration` (S-048 §4.4); 3. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-11.2 Properties** +UI
  - Files: `frontend/widgets/OBSBasic_Transitions.cpp` (1569; add/rename/remove), `frontend/dialogs/OBSBasicProperties.cpp` (508).
  - Steps: 1. port add/rename/remove user transitions; 2. wire the properties dialog (P6-16); 3. UI bookend.
  - Done when: add/rename/remove works.
- [ ] **P6-11.3 Quick transitions/T-bar**
  - Files: `frontend/utility/QuickTransition.cpp` (47), `QuickTransition.hpp` (48), `frontend/widgets/OBSBasic_Transitions.cpp` (1569; T-bar), `frontend/widgets/OBSBasic_Hotkeys.cpp` (344).
  - Steps: 1. port quick transitions + their hotkeys; 2. port the T-bar (`OBS_FRONTEND_EVENT_TBAR_VALUE_CHANGED`, id 33, S-048); 3. fire test.
  - Spec: S-029 (transition backend).
  - Vectors: test/conformance/scripts/transitions/.
  - Done when: transitions fire.

### P6-12 Controls dock
- [ ] **P6-12.1 Stream/record** +UI
  - Files: `frontend/widgets/OBSBasicControls.cpp` (285), `frontend/forms/OBSBasicControls.ui` (356), `frontend/widgets/OBSBasic_Streaming.cpp` (479), `OBSBasic_Recording.cpp` (431), `frontend/utility/BasicOutputHandler.cpp` (614).
  - Steps: 1. port the control buttons + text/state; 2. port pause + reconnect handling; 3. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-12.2 Replay/vcam** +UI
  - Files: `frontend/widgets/OBSBasic_ReplayBuffer.cpp` (238), `frontend/widgets/OBSBasic_VirtualCam.cpp` (185).
  - Steps: 1. port the replay buffer button + save; 2. port the virtual camera toggle; 3. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-12.3 States**
  - Files: `frontend/widgets/OBSBasic_Streaming.cpp` (479), `OBSBasic_Recording.cpp` (431), `frontend/utility/BasicOutputHandler.cpp` (614).
  - Steps: 1. port the reconnecting/paused/failed message dialogs; 2. use the exact locale keys from the C sources.
  - Done when: each state shows its locale text.

### P6-13 Stats dock
- [ ] **P6-13.1 Collection**
  - Files: `frontend/widgets/OBSBasicStats.cpp` (601).
  - Steps: 1. port the stats dialog (CPU%, mem, disk space, FPS, render time, skipped/missed frames, output bitrate, dropped); 2. feed it from the obs stats API (Phase 4); 3. compare values against a live C session.
  - Done when: values match a live C session.
- [ ] **P6-13.2 Table** +UI
  - Files: `frontend/widgets/OBSBasicStats.cpp` (601).
  - Steps: 1. port the table UI + reset button + colour thresholds; 2. UI bookend.
  - Done when: UI bookend passes.

### P6-14 Source toolbars
- [ ] **P6-14.1 Simple** +UI
  - Files: `frontend/components/ColorSourceToolbar.cpp` (83), `TextSourceToolbar.cpp` (152), `ImageSourceToolbar.cpp` (54), `SourceToolbar.cpp` (62); forms `frontend/forms/source-toolbar/color-source-toolbar.ui` (93), `text-source-toolbar.ui` (91), `image-source-toolbar.ui` (121).
  - Steps: 1. port the three toolbars + the base class; 2. UI bookend each.
  - Done when: UI bookends pass.
- [ ] **P6-14.2 Browser/media** +UI
  - Files: `frontend/components/BrowserToolbar.cpp` (23), `frontend/components/MediaControls.cpp` (539); forms `frontend/forms/source-toolbar/browser-source-toolbar.ui` (90), `media-controls.ui` (312).
  - Steps: 1. port the browser toolbar (obs-browser P5 port provides the content); 2. port the media controls (P6-17); 3. UI bookend each.
  - Done when: UI bookends pass.
- [ ] **P6-14.3 Capture** +UI
  - Files: `frontend/components/DeviceCaptureToolbar.cpp` (59), `DisplayCaptureToolbar.cpp` (41), `WindowCaptureToolbar.cpp` (42), `GameCaptureToolbar.cpp` (92), `AudioCaptureToolbar.cpp` (34), `ApplicationAudioCaptureToolbar.cpp` (26), `ComboSelectToolbar.cpp` (107); forms `device-select-toolbar.ui` (133), `game-capture-toolbar.ui` (155).
  - Steps: 1. port the capture toolbars (P5 capture plugin ports supply the option lists); 2. UI bookend each.
  - Done when: UI bookends pass.

### P6-15 Dock framework
- [ ] **P6-15.1 Base/persist**
  - Files: `frontend/docks/OBSDock.cpp` (46), `OBSDock.hpp` (18), `frontend/widgets/OBSBasic_Docks.cpp` (260).
  - Steps: 1. port the dock base class + dock id registry; 2. persist layout via the P6-06.3 `DockState`; 3. state persistence test.
  - Done when: state persists.
- [ ] **P6-15.2 Browser docks**
  - Files: `frontend/docks/BrowserDock.cpp` (28), `BrowserDock.hpp` (36), `frontend/dialogs/OBSExtraBrowsers.cpp` (36), `OBSExtraBrowsers.hpp` (23), `frontend/forms/OBSExtraBrowsers.ui` (101), `frontend/utility/ExtraBrowsersModel.cpp` (259), `ExtraBrowsersModel.hpp` (55), `ExtraBrowsersDelegate.cpp` (133).
  - Steps: 1. port the browser dock (obs-browser P5 port provides the content); 2. port the extra-browser model + add-by-URL; 3. UI bookend.
  - Done when: add by URL.
- [ ] **P6-15.3 API**
  - Files: `frontend/OBSStudioAPI.cpp` (772; dock functions), `frontend/api/obs-frontend-internal.hpp` (148).
  - Steps: 1. implement `obs_frontend_add_dock_by_id`/`add_custom_qdock`/`remove_dock`; 2. contract test with the test plugin.
  - Spec: S-048 §5.9 (R75).
  - Done when: contract test.

### P6-16 Properties view
- [ ] **P6-16.1 View model**
  - Files: `shared/properties-view/properties-view.cpp` (2420), `properties-view.hpp` (204), `properties-view.moc.hpp` (63).
  - Steps: 1. port the view model from `obs_properties_t` (S-015); 2. cover all 13 property kinds; 3. table-test the model against the `props` vectors.
  - Spec: S-015.
  - Vectors: test/conformance/scripts/props/.
  - Done when: model reflects all 13 property kinds (`OBS_PROPERTY_*` minus `OBS_PROPERTY_INVALID`).
- [ ] **P6-16.2 Basic** +UI
  - Files: `shared/properties-view/properties-view.cpp` (2420; bool/int/float/text widgets).
  - Steps: 1. port the basic widgets (bool, int slider/spin/scroller, float, text single/multiline/password/info); 2. UI bookend each.
  - Spec: S-015.
  - Done when: UI bookends pass.
- [ ] **P6-16.3 Path/list** +UI
  - Files: `shared/properties-view/properties-view.cpp` (2420; path/list/editable-list widgets).
  - Steps: 1. port path file/dir/save; 2. port list combo/radio/editable + editable list; 3. UI bookend each.
  - Spec: S-015.
  - Done when: UI bookends pass.
- [ ] **P6-16.4 Rich** +UI
  - Files: `shared/properties-view/properties-view.cpp` (2420; colour/alpha/font/frame-rate/button/group widgets).
  - Steps: 1. port colour/alpha, font, frame rate, button, group widgets; 2. UI bookend each.
  - Spec: S-015.
  - Done when: UI bookends pass.
- [ ] **P6-16.5 Live update**
  - Files: `shared/properties-view/properties-view.cpp` (2420; debounce, `modified` callbacks, visibility, reset).
  - Steps: 1. port the debounce + `modified` callbacks; 2. port visibility rules + reset-to-defaults + warnings; 3. live-update test with a sample plugin.
  - Spec: S-015 (callback semantics).
  - Vectors: test/conformance/scripts/props/.
  - Done when: sample plugin updates live.
- [ ] **P6-16.6 Dialogs** +UI
  - Files: `frontend/dialogs/OBSBasicProperties.cpp` (508) + `forms/OBSBasicProperties.ui` (179), `OBSBasicFilters.cpp` (1165) + `OBSBasicFilters.ui` (669), `OBSBasicSourceSelect.cpp` (949) + `OBSBasicSourceSelect.ui` (654), `OBSBasicTransform.cpp` (473) + `OBSBasicTransform.ui` (962), `OBSBasicInteraction.cpp` (402) + `OBSBasicInteraction.ui` (48).
  - Steps: 1. port the five dialogs (properties, filters list + preview + drag order, source select, transform, interaction) on the P6-16 widgets; 2. UI bookend each.
  - Done when: UI bookends pass.

### P6-17 Media controls
- [ ] **P6-17.1 Play/pause/seek** +UI
  - Files: `frontend/components/MediaControls.cpp` (539), `MediaControls.hpp` (79), `frontend/forms/source-toolbar/media-controls.ui` (312).
  - Steps: 1. port play/pause/seek + the slider; 2. drive the media-playback procs (S-036); 3. UI bookend.
  - Spec: S-036.
  - Vectors: test/conformance/scripts/mediaplayback/.
  - Done when: UI bookend passes.
- [ ] **P6-17.2 Extras**
  - Files: `frontend/components/MediaControls.cpp` (539).
  - Steps: 1. port time labels, loop, restart, previous/next; 2. match the C behaviour table.
  - Spec: S-036.
  - Done when: match C behaviour.

### P6-18 Accessibility pass
- [ ] **P6-18.1 Labels**
  - Files: `frontend/components/AccessibleAlignmentSelector.cpp` (126), `AccessibleAlignmentCell.cpp` (71), `VolumeAccessibleInterface.cpp` (62), `frontend/components/AlignmentSelector.cpp` (324).
  - Steps: 1. audit every widget role/name; 2. port the custom a11y interfaces for the custom widgets; 3. commit the report.
  - Done when: report.
- [ ] **P6-18.2 Keyboard**
  - Files: `frontend/widgets/OBSBasic*.cpp` (33 files; dialog wiring), `frontend/forms/*.ui` (29 top-level + 7 in `source-toolbar/`; focus order).
  - Steps: 1. port tab order and shortcuts from the `.ui` files; 2. keyboard-only walk of every dialog.
  - Done when: all dialogs keyboard-operable.
- [ ] **P6-18.3 Contrast**
  - Files: `frontend/data/themes/System.obt` (477), `Yami.obt` (2887), the six `Yami_*.ovt` variants.
  - Steps: 1. script WCAG AA contrast checks for all theme palettes; 2. commit the report.
  - Done when: script report.

## C. Settings and wizards

### P6-19 Settings dialog framework
- [ ] **P6-19.1 Pages**
  - Files: `frontend/settings/OBSBasicSettings.cpp` (6034), `OBSBasicSettings.hpp` (455), `frontend/forms/OBSBasicSettings.ui` (9406).
  - Steps: 1. port the settings dialog shell + page navigation from `OBSBasicSettings.ui`; 2. UI bookend.
  - Done when: navigation.
- [ ] **P6-19.2 Change tracking**
  - Files: `frontend/settings/OBSBasicSettings.cpp` (6034; dirty flags), `frontend/utility/SettingsEventFilter.hpp` (53).
  - Steps: 1. port the dirty flags + Apply/OK/Cancel; 2. port `SettingsEventFilter`; 3. unit tests.
  - Done when: tests.
- [ ] **P6-19.3 Restart-needed/search**
  - Files: `frontend/settings/OBSBasicSettings.cpp` (6034; restart list + search box).
  - Steps: 1. port the restart-needed prompt list; 2. port the page search; 3. test against the current set.
  - Done when: matches current set.
- [ ] **P6-19.4 Settings function inventory (added)**
  - Files: `frontend/settings/OBSBasicSettings.cpp` (6034; 186 functions, 0 named in the plan — see `docs/rust-rewrite/coverage/function-inventory.csv`).
  - Steps: 1. assign every non-trivial function of the file to exactly one step of P6-19.1–.3, P6-20.1–.7 or P6-21.1–.3 (per page: `Load*`/`Save*` pairs, change callbacks, dirty flags, restart list). 2. record the mapping here, one line per page group.
  - Done when: every non-trivial function of the file is in a named step.

### P6-20 Settings pages
- [ ] **P6-20.1 General/Appearance** +UI
  - Files: `frontend/settings/OBSBasicSettings.cpp` (6034; general page), `OBSBasicSettings_Appearance.cpp` (170).
  - Steps: 1. port language/theme/updates-channel/system-tray/snapping/projectors/preview settings; 2. persist to the S-048 §4.2 keys; 3. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-20.2 Stream** +UI
  - Files: `frontend/settings/OBSBasicSettings_Stream.cpp` (1868), `frontend/widgets/OBSBasic_Service.cpp` (128).
  - Steps: 1. port service/server/key/auth fields; 2. port the bandwidth test + multitrack page; 3. UI bookend.
  - Spec: S-026 (service list).
  - Vectors: test/conformance/scripts/rtmpservices/.
  - Done when: UI bookend passes.
- [ ] **P6-20.3 Output simple** +UI
  - Files: `frontend/utility/SimpleOutput.cpp` (969), `SimpleOutput.hpp` (63), `frontend/settings/OBSBasicSettings.cpp` (6034; simple output page).
  - Steps: 1. port the simple-output mapping (bitrate, encoder, recording path/format/quality); 2. write the `SimpleOutput.*`/`Output.*` keys (S-048 §4.3); 3. UI bookend.
  - Spec: S-048 §4.3.
  - Vectors: test/conformance/scripts/frontendfmt/ (`config_defaults`, `migrate_basic`).
  - Done when: UI bookend passes.
- [ ] **P6-20.4 Output advanced** +UI
  - Files: `frontend/utility/AdvancedOutput.cpp` (975), `AdvancedOutput.hpp` (44), `frontend/settings/OBSBasicSettings.cpp` (6034; advanced output page).
  - Steps: 1. port advanced output (tracks, replay buffer, file name formatting); 2. wire the encoder property views (P6-16); 3. UI bookend.
  - Spec: S-048 §4.3 (AdvOut defaults), §5.4 (R30).
  - Vectors: test/conformance/scripts/frontendfmt/ (`migrate_basic`).
  - Pitfalls: the R30 migrations run on every activation — e.g. `RecFormat`→`RecFormat2` with the map `ts`→`mpegts`, `m3u8`→`hls`, `fmp4`→`fragmented_mp4`, `fmov`→`fragmented_mov`; the rescale-filter backfill must NOT mark the profile changed (S-048 Q-2).
  - Done when: UI bookend passes.
- [ ] **P6-20.5 Audio/Video** +UI
  - Files: `frontend/settings/OBSBasicSettings.cpp` (6034; audio + video pages).
  - Steps: 1. port sample rate/channels/devices; 2. port base/output resolution, FPS, downscale filter, colour format/space/range, SDR white level; 3. apply the resolution defaults (S-048 R31); 4. UI bookend.
  - Spec: S-048 §4.3, §5.4 (R31).
  - Vectors: test/conformance/scripts/frontendfmt/ (`resolution_defaults`, `config_defaults`).
  - Done when: UI bookend passes.
- [ ] **P6-20.6 Hotkeys/A11y/Advanced** +UI
  - Files: `frontend/settings/OBSBasicSettings_A11y.cpp` (360), `frontend/settings/OBSBasicSettings.cpp` (6034; hotkeys + advanced pages).
  - Steps: 1. port the hotkeys page (P6-21); 2. port process priority, renderer, network (bind IP, dynamic bitrate), delay, reconnect, browser hardware accel; 3. persist to the S-048 keys; 4. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-20.7 Compatibility**
  - Files: `frontend/OBSApp.cpp` (2107; `MigrateLegacySettings`), `frontend/widgets/OBSBasic_Profiles.cpp` (1123).
  - Steps: 1. load old `basic.ini`/`service.json`/`streamEncoder.json`; 2. run the migrations; 3. assert identical effective values.
  - Spec: S-048 §5.2–5.4.
  - Vectors: test/conformance/scripts/frontendfmt/ (`migrate_user`, `migrate_basic`).
  - Done when: display identical values.

### P6-21 Hotkey editor
- [ ] **P6-21.1 Capture** +UI
  - Files: `frontend/settings/OBSHotkeyEdit.cpp` (223), `OBSHotkeyEdit.hpp` (96), `OBSHotkeyWidget.cpp` (246), `OBSHotkeyWidget.hpp` (99), `OBSHotkeyLabel.cpp` (73), `OBSHotkeyLabel.hpp` (35).
  - Steps: 1. port per-OS key capture incl. mouse buttons; 2. port the hotkey table editing; 3. UI bookend.
  - Spec: S-034.
  - Vectors: test/conformance/scripts/hotkeys/.
  - Done when: modifier combos.
- [ ] **P6-21.2 Conflicts**
  - Files: `frontend/settings/OBSBasicSettings.cpp` (6034; hotkeys page conflict detection).
  - Steps: 1. port the duplicate-binding highlight; 2. unit test.
  - Spec: S-034.
  - Vectors: test/conformance/scripts/hotkeys/.
  - Done when: test.
- [ ] **P6-21.3 Clear/reset/filter**
  - Files: `frontend/settings/OBSBasicSettings.cpp` (6034), `frontend/settings/OBSHotkeyEdit.cpp` (223).
  - Steps: 1. port clear/reset/filter controls; 2. test list updates.
  - Done when: list updates.

### P6-22 Auto-configuration wizard
- [ ] **P6-22.1 Framework** +UI
  - Files: `frontend/wizards/AutoConfig.cpp` (388), `AutoConfig.hpp` (124), `AutoConfigStartPage.cpp` (48), `AutoConfigVideoPage.cpp` (131), `AutoConfigStreamPage.cpp` (712); forms `AutoConfigStartPage.ui` (116), `AutoConfigVideoPage.ui` (99), `AutoConfigStreamPage.ui` (562).
  - Steps: 1. port the wizard shell + start/video/stream pages; 2. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-22.2 Bandwidth test**
  - Files: `frontend/wizards/AutoConfigTestPage.cpp` (1243), `AutoConfigTestPage.hpp` (78), `frontend/forms/AutoConfigTestPage.ui` (141).
  - Steps: 1. port the test stream to ingest + bitrate ramp; 2. run against a mock server.
  - Done when: mock server.
- [ ] **P6-22.3 Encoder tests**
  - Files: `frontend/wizards/AutoConfigTestPage.cpp` (1243; resolution/FPS/encoder trials), `frontend/wizards/TestMode.hpp` (64).
  - Steps: 1. port the resolution/FPS/encoder trials; 2. assert the trial logic equals the C tables.
  - Done when: logic equal.
- [ ] **P6-22.4 Apply**
  - Files: `frontend/wizards/AutoConfig.cpp` (388; applying results).
  - Steps: 1. apply the results into the profile (S-048 §4.3 keys); 2. test they appear in profile settings.
  - Done when: results appear in profile settings.

### P6-23 First-run / what's new / about
- [ ] **P6-23.1 About** +UI
  - Files: `frontend/dialogs/OBSAbout.cpp` (172), `frontend/forms/OBSAbout.ui` (251), `frontend/data/license/gplv2.txt` (85).
  - Steps: 1. port the about dialog (authors, licence); 2. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-23.2 What's new** +UI
  - Files: `frontend/dialogs/OBSWhatsNew.cpp` (66), `OBSWhatsNew.hpp` (21), `frontend/utility/WhatsNewInfoThread.cpp` (297), `WhatsNewInfoThread.hpp` (18), `frontend/data/OBSPublicRSAKey.pem` (14).
  - Steps: 1. port the feed fetch + RSA signature check against `data/OBSPublicRSAKey.pem`; 2. port the blake2b If-None-Match cache hash; 3. UI bookend (web view).
  - Pitfalls: keep RSA per D-23 (same key, same verification path); the RSA check signs the feed only — the blake2b hash is the If-None-Match ETag, not a signature (WhatsNewInfoThread.cpp:88–111, 167, 229).
  - Done when: feed loads and a tampered feed is rejected.
- [ ] **P6-23.3 macOS permissions** +UI
  - Files: `frontend/dialogs/OBSPermissions.cpp` (98), `OBSPermissions.hpp` (46), `frontend/forms/OBSPermissions.ui` (496).
  - Steps: 1. port the permissions dialog (camera/mic/screen rows); 2. UI bookend.
  - Done when: UI bookend passes.

## D. Profiles, collections, importers

### P6-24 Scene collection manager
- [ ] **P6-24.1 Load/save**
  - Files: `frontend/widgets/OBSBasic_SceneCollections.cpp` (1705), `frontend/models/SceneCollection.cpp` (71), `SceneCollection.hpp` (64).
  - Steps: 1. port the collection JSON load/save (S-048 §4.4 layout); 2. load the P0-18 corpus.
  - Spec: S-048 §4.4, §5.6 (R51–R56).
  - Done when: corpus loads.
- [ ] **P6-24.2 Backup/autosave**
  - Files: `frontend/widgets/OBSBasic_SceneCollections.cpp` (1705; autosave timer, `.json.bak`).
  - Steps: 1. port the `.json.bak` backup + autosave timer; 2. crash-recovery test (corrupt main file, verify `.bak` fallback per S-048 R50).
  - Spec: S-048 §5.6 (R50, R56).
  - Done when: crash recovery test.
- [ ] **P6-24.3 CRUD/import/export** +UI
  - Files: `frontend/widgets/OBSBasic_SceneCollections.cpp` (1705).
  - Steps: 1. port rename/duplicate/remove/export/import with path fix-up; 2. duplicate/import must regenerate `sources[*].uuid` (S-048 R57); 3. UI bookend.
  - Spec: S-048 §5.6 (R57), §5.5 (R47).
  - Done when: UI bookend passes.
- [ ] **P6-24.4 Missing files**
  - Files: `frontend/widgets/OBSBasic_SceneCollections.cpp` (1705; missing-file scan on load), `frontend/utility/MissingFilesModel.cpp` (306).
  - Steps: 1. port the missing-file scan on collection load; 2. feed the P6-27 dialog model.
  - Done when: missing files listed on a moved corpus.

### P6-25 Profile manager
- [ ] **P6-25.1 CRUD** +UI
  - Files: `frontend/widgets/OBSBasic_Profiles.cpp` (1123).
  - Steps: 1. port create/duplicate/rename/remove (S-048 R20–R26); 2. UI bookend.
  - Spec: S-048 §5.3 (R20–R26).
  - Vectors: test/conformance/scripts/frontendfmt/ (`names`).
  - Pitfalls: the profile dir name is `GetFileSafeName` + `GetClosestUnusedFileName` (`-2` suffix, no separator — S-048 R41, Q-5 last-dot rule); duplicate copies **all** files of the profile dir recursively (R25).
  - Done when: UI bookend passes.
- [ ] **P6-25.2 Export/import**
  - Files: `frontend/widgets/OBSBasic_Profiles.cpp` (1123).
  - Steps: 1. port export (the four files, S-048 R26); 2. port import (directory name check, mandatory `basic.ini`); 3. round-trip test.
  - Spec: S-048 §5.3 (R26).
  - Pitfalls: import keeps the foreign directory name unsanitised and only checks directory names for collision — a display-name clash is possible (S-048 Q-7).
  - Done when: round-trip restores the profile.
- [ ] **P6-25.3 Encoder JSON**
  - Files: `frontend/widgets/OBSBasic_Profiles.cpp` (1123), `frontend/settings/OBSBasicSettings_Stream.cpp` (1868; `service.json`).
  - Steps: 1. read/write `streamEncoder.json`/`recordEncoder.json`/`service.json` (obs_data JSON, S-014); 2. parse-identity test.
  - Spec: S-048 §4.4 (companion files), S-014.
  - Done when: files parse identically.

### P6-26 Importers
- [ ] **P6-26.1 Classic**
  - Files: `frontend/importers/classic.cpp` (576), `frontend/importers/importers.cpp` (103), `importers.hpp` (168).
  - Steps: 1. port the classic-OBS importer; 2. golden JSON test.
  - Done when: golden JSON.
- [ ] **P6-26.2 Streamlabs**
  - Files: `frontend/importers/sl.cpp` (525).
  - Steps: 1. port the Streamlabs importer; 2. golden JSON test.
  - Done when: golden JSON.
- [ ] **P6-26.3 Studio**
  - Files: `frontend/importers/studio.cpp` (287).
  - Steps: 1. port the OBS-Studio importer; 2. golden JSON test.
  - Done when: golden JSON.
- [ ] **P6-26.4 XSplit**
  - Files: `frontend/importers/xsplit.cpp` (533).
  - Steps: 1. port the XSplit importer; 2. golden JSON test.
  - Done when: golden JSON.
- [ ] **P6-26.5 Goldens**
  - Files: `frontend/importers/classic.cpp` (576), `sl.cpp` (525), `studio.cpp` (287), `xsplit.cpp` (533).
  - Steps: 1. commit sample inputs + expected collections; 2. run all four goldens in CI.
  - Done when: goldens pass.
- [ ] **P6-26.6 Importer dialog UI (added)** +UI
  - Files: `frontend/importer/OBSImporter.cpp` (247), `OBSImporter.hpp` (46), `ImporterModel.cpp` (210), `ImporterModel.hpp` (61), `ImporterEntryPathItemDelegate.cpp` (161), `ImporterEntryPathItemDelegate.hpp` (44), `frontend/forms/OBSImporter.ui` (71).
  - Steps: 1. port the dialog view model (entries from the four importers); 2. port the widget (cxx-qt); 3. UI bookend.
  - Done when: the dialog lists parsed entries from each of the four importers and the P6-26.5 goldens still pass.
- [ ] **P6-26.7 Importer function inventory (added)**
  - Files: `frontend/importers/classic.cpp` (576; `translate_source` 177 lines, `create_string_obj`), `sl.cpp` (525; `attempt_import` 148 lines), `xsplit.cpp` (533; `parse_items` 172 lines), `studio.cpp` (287) — per `docs/rust-rewrite/coverage/function-inventory.csv`.
  - Steps: 1. name the four largest functions (`translate_source` 177, `parse_items` 172, `attempt_import` 148, `create_string_obj`) in the P6-26.1/.2/.4 steps; 2. cross-check every remaining non-trivial function of `frontend/importers/*.cpp` against P6-26.1–.5, one line per group here.
  - Done when: every non-trivial importer function is in a named step.

### P6-27 Missing files dialog
- [ ] **P6-27.1 List/relink** +UI
  - Files: `frontend/dialogs/OBSMissingFiles.cpp` (143), `OBSMissingFiles.hpp` (53), `frontend/forms/OBSMissingFiles.ui` (129), `frontend/utility/MissingFilesModel.cpp` (306), `MissingFilesModel.hpp` (68), `MissingFilesPathItemDelegate.cpp` (163).
  - Steps: 1. port the model + dialog (list, relink, delete); 2. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-27.2 Search dir**
  - Files: `frontend/utility/MissingFilesModel.cpp` (306; recursive search).
  - Steps: 1. port the recursive directory search; 2. test with a nested fixture.
  - Done when: search finds the file.

### P6-28 Remux dialog
- [ ] **P6-28.1 Queue** +UI
  - Files: `frontend/dialogs/OBSRemux.cpp` (316), `frontend/forms/OBSRemux.ui` (71), `frontend/utility/RemuxQueueModel.cpp` (387), `RemuxQueueModel.hpp` (78).
  - Steps: 1. port the queue model + dialog; 2. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-28.2 Worker**
  - Files: `frontend/utility/RemuxWorker.cpp` (63), `RemuxWorker.hpp` (44).
  - Steps: 1. call the P2-29 `media-remux` port (no spec; FFmpeg I/O); 2. surface progress + errors in the queue.
  - Done when: progress and errors.

## E. Services, auth, updater

### P6-29 OAuth framework
- [ ] **P6-29.1 Listener**
  - Files: `frontend/oauth/AuthListener.cpp` (107), `AuthListener.hpp` (24), `frontend/oauth/OAuth.cpp` (223), `OAuth.hpp` (53).
  - Steps: 1. port the local HTTP redirect listener; 2. port the OAuth base (authorize/refresh/token endpoints); 3. code-received test.
  - Done when: code received.
- [ ] **P6-29.2 Storage**
  - Files: `frontend/oauth/Auth.cpp` (87), `Auth.hpp` (60).
  - Steps: 1. port token storage to the OS keychain via the `keyring` crate; 2. restart test.
  - Done when: tokens survive restart.
- [ ] **P6-29.3 Twitch**
  - Files: `frontend/oauth/TwitchAuth.cpp` (511), `TwitchAuth.hpp` (38).
  - Steps: 1. port the Twitch scopes + token refresh; 2. port the chat/stats/feed data endpoints.
  - Done when: mock refresh passes.
- [ ] **P6-29.4 YouTube**
  - Files: `frontend/oauth/YoutubeAuth.cpp` (304), `YoutubeAuth.hpp` (39).
  - Steps: 1. port the YouTube OAuth (scopes, refresh); 2. test against the mock API (P6-30.4).
  - Done when: mock refresh passes.
- [ ] **P6-29.5 Restream**
  - Files: `frontend/oauth/RestreamAuth.cpp` (285), `RestreamAuth.hpp` (24).
  - Steps: 1. port the Restream token flow; 2. mock test.
  - Done when: mock refresh passes.
- [ ] **P6-29.6 Refresh tests**
  - Files: `frontend/oauth/Auth.cpp` (87), `OAuth.cpp` (223), `TwitchAuth.cpp` (511), `YoutubeAuth.cpp` (304), `RestreamAuth.cpp` (285).
  - Steps: 1. simulate token expiry; 2. assert silent refresh + keychain update.
  - Done when: expiry simulated.

### P6-30 YouTube integration
- [ ] **P6-30.1 Broadcast** +UI
  - Files: `frontend/dialogs/OBSYoutubeActions.cpp` (741), `OBSYoutubeActions.hpp` (76), `frontend/forms/OBSYoutubeActions.ui` (695).
  - Steps: 1. port the create/select/schedule broadcast dialog; 2. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-30.2 Key**
  - Files: `frontend/dialogs/OBSYoutubeActions.cpp` (741; stream key fetch), `frontend/widgets/OBSBasic_YouTube.cpp` (259).
  - Steps: 1. port the stream key fetch + save; 2. mock API test.
  - Done when: mock.
- [ ] **P6-30.3 Docks** +UI
  - Files: `frontend/docks/YouTubeAppDock.cpp` (454), `YouTubeAppDock.hpp` (54), `YouTubeChatDock.cpp` (35), `YouTubeChatDock.hpp` (23), `frontend/widgets/OBSBasic_YouTube.cpp` (259).
  - Steps: 1. port the app dock (web content via the P5 obs-browser port); 2. port the chat dock; 3. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-30.4 Mock API tests**
  - Files: `frontend/oauth/YoutubeAuth.cpp` (304), `frontend/dialogs/OBSYoutubeActions.cpp` (741).
  - Steps: 1. build the mock YouTube API server; 2. run the create/schedule/key journeys against it.
  - Done when: mock journeys pass.

### P6-31 Streaming services UI
- [ ] **P6-31.1 Pickers**
  - Files: `frontend/settings/OBSBasicSettings_Stream.cpp` (1868; service/server dropdowns).
  - Steps: 1. port the service/server dropdowns from `crates/plugin-rtmp-services` (S-026); 2. list test against the `rtmpservices` vectors.
  - Spec: S-026.
  - Vectors: test/conformance/scripts/rtmpservices/.
  - Done when: list matches C.
- [ ] **P6-31.2 Key hiding** +UI
  - Files: `frontend/settings/OBSBasicSettings_Stream.cpp` (1868; show/hide key button).
  - Steps: 1. port the stream key field with show/hide; 2. default hidden; 3. UI bookend.
  - Done when: key hidden by default, toggles.
- [ ] **P6-31.3 Multitrack**
  - Files: `frontend/utility/MultitrackVideoOutput.cpp` (924), `MultitrackVideoOutput.hpp` (70), `GoLiveAPI_Network.cpp` (136), `GoLiveAPI_PostData.cpp` (79), `GoLiveAPI_CensoredJson.cpp` (89), `MultitrackVideoError.cpp` (46).
  - Steps: 1. port the multitrack video output config; 2. port the GoLive API client (censored JSON, post data); 3. mock GoLive API test.
  - Done when: mock GoLive API.

### P6-32 Updater
- [ ] **P6-32.1 Manifest**
  - Files: `frontend/updater/manifest.hpp` (93), `http.cpp` (455), `hash.cpp` (81).
  - Steps: 1. port the manifest fetch + version compare; 2. local server test.
  - Done when: local server.
- [ ] **P6-32.2 Download/verify**
  - Files: `frontend/updater/hash.cpp` (81), `patch.cpp` (150), `updater.cpp` (2173; VC redist Authenticode via Windows CryptoAPI).
  - Steps: 1. port the delta-patch download + blake2b manifest hash verification; 2. port the VC redist Authenticode check (Windows); 3. tamper test.
  - Pitfalls: keep RSA per D-23, but the RSA check lives in the What's-New feed (P6-23.2), not the updater — the updater verifies by blake2b manifest hashes + Authenticode only; ed25519 only if D-07 review demands it.
  - Done when: tamper test fails.
- [ ] **P6-32.3 Windows swap**
  - Files: `frontend/updater/updater.cpp` (2173).
  - Steps: 1. port the updater as a helper process; 2. port in-place replace + rollback; 3. update test VM.
  - Done when: update test VM.
- [ ] **P6-32.4 macOS/Linux**
  - Files: `frontend/utility/OBSSparkle.mm` (39), `OBSSparkle.hpp` (24), `MacUpdateThread.cpp` (74), `AutoUpdateThread.cpp` (355), `frontend/widgets/OBSBasic_Updater.cpp` (246), `frontend/dialogs/OBSUpdate.cpp` (45), `frontend/forms/OBSUpdate.ui` (109).
  - Steps: 1. port the Sparkle update path (macOS); 2. port the package-manager notice (Linux); 3. document the per-OS behaviour.
  - Done when: documented.
- [ ] **P6-32.5 Tests**
  - Files: `frontend/updater/updater.cpp` (2173), `http.cpp` (455), `hash.cpp` (81), `patch.cpp` (150), `manifest.hpp` (93).
  - Steps: 1. serve a local manifest + patch; 2. run the update + rollback suite.
  - Done when: suite passes.

### P6-33 Plugin manager
- [ ] **P6-33.1 List** +UI
  - Files: `frontend/plugin-manager/PluginManager.cpp` (386), `PluginManager.hpp` (105), `PluginManagerWindow.cpp` (334), `InstalledPluginRow.cpp` (169), `frontend/forms/PluginManagerWindow.ui` (379).
  - Steps: 1. port the installed-plugin list; 2. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-33.2 Enable/disable**
  - Files: `frontend/plugin-manager/PluginManager.cpp` (386; `obs_add_disabled_module` at 309, `obs_get_disabled_module` at 170).
  - Steps: 1. call the Phase 4 `obs_add_disabled_module`/`obs_get_disabled_module` API; 2. port the pending-restart prompt; 3. session-scoped persistence test.
  - Pitfalls: the disabled-module list is session-scoped (libobs API) — it is cleared on restart; do not persist it to disk (see the recent stale-data fix in the git log).
  - Done when: disabled module persists for the session and clears on restart.
- [ ] **P6-33.3 Paths**
  - Files: `frontend/plugin-manager/LoaderPaths_Linux.hpp.in` (30), `LoaderPaths_Flatpak.hpp.in` (30), `LoaderPaths_BSD.hpp.in` (28), `PluginModuleLoader_Linux.cpp` (224), `PluginModuleLoader_Flatpak.cpp` (142), `PluginModuleLoader_BSD.cpp` (220), `PluginModuleLoader_MacOS.mm` (196), `PluginModuleLoader_Windows.cpp` (196).
  - Steps: 1. port the per-OS/Flatpak loader paths; 2. Flatpak path test; 3. document BSD as best-effort (D-12).
  - Done when: Flatpak path correct.

### P6-34 Virtual camera config
- [ ] **P6-34.1 Dialog** +UI
  - Files: `frontend/dialogs/OBSBasicVCamConfig.cpp` (129), `OBSBasicVCamConfig.hpp` (31), `frontend/forms/OBSBasicVCamConfig.ui` (110).
  - Steps: 1. port the output-type dialog (program, preview, scene, source); 2. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-34.2 Persist**
  - Files: `frontend/dialogs/OBSBasicVCamConfig.cpp` (129), `frontend/widgets/OBSBasic_VirtualCam.cpp` (185).
  - Steps: 1. persist the choice in the collection's `virtual-camera` object (`type2` + `scene`/`source`, S-048 §4.4); 2. restart test.
  - Done when: choice survives restart.

### P6-35 Debug dialogs
- [ ] **P6-35.1 Decide**
  - Files: `frontend/dialogs/OBSIdianPlayground.cpp` (141), `OBSIdianPlayground.hpp` (34), `frontend/forms/OBSIdianPlayground.ui` (55).
  - Steps: 1. record the keep/drop decision (it is a widget playground; default drop); 2. note it in the task file.
  - Done when: decision recorded.
- [ ] **P6-35.2 Port**
  - Files: `frontend/dialogs/OBSIdianPlayground.cpp` (141).
  - Steps: 1. port only if the decision is keep; 2. UI bookend.
  - Done when: UI bookend passes (if kept).

## F. Frontend API and tools

### P6-36 Frontend API
- [ ] **P6-36.1 Inventory**
  - Files: `frontend/api/obs-frontend-api.h` (271; 99 `OBS_FRONTEND_API` exports, 44 `OBS_FRONTEND_EVENT_*` values).
  - Steps: 1. table the 99 functions (name, signature, no-callback result); 2. table the 44 event ids (names are ABI); 3. commit both tables.
  - Spec: S-048 §5.9 (function catalog + event numbering).
  - Done when: table committed.
- [ ] **P6-36.2 Scenes/transitions/profiles/collections**
  - Files: `frontend/api/obs-frontend-api.cpp` (693), `frontend/OBSStudioAPI.cpp` (772), `OBSStudioAPI.hpp` (244).
  - Steps: 1. implement the scene/transition/profile/collection functions; 2. implement `convert_string_list` (R70) and the new-reference rules (R71–R73).
  - Spec: S-048 §5.9 (R70–R73).
  - Done when: contract tests pass.
- [ ] **P6-36.3 Streaming/recording/replay/vcam**
  - Files: `frontend/api/obs-frontend-api.cpp` (693), `frontend/OBSStudioAPI.cpp` (772).
  - Steps: 1. implement the start/stop/active functions + the atomic state flags (R60); 2. fire events on the UI thread (ids 0–7, 18–21, 27–28, 30–32).
  - Spec: S-048 §5.7–5.9 (R60–R61, event numbering).
  - Done when: contract tests pass.
- [ ] **P6-36.4 Docks/menus/save callbacks/events**
  - Files: `frontend/OBSStudioAPI.cpp` (772), `frontend/api/obs-frontend-internal.hpp` (148).
  - Steps: 1. implement the dock/menu/callback/event API (R74–R75, R77–R79); 2. keep the callback registration order (events in reverse registration order, R74); 3. implement the `OBSStudioAPI` equivalent.
  - Spec: S-048 §5.9 (R74–R79).
  - Pitfalls: events are dropped while saving is deferred, except `SCENE_COLLECTION_CLEANUP` and `EXIT` (S-048 Q-6); duplicate (callback, data) adds are ignored and removal is by exact pair (R74).
  - Done when: contract tests pass.
- [ ] **P6-36.5 Contract test**
  - Files: `plugins/obs-websocket` (submodule; 5.7.6), the test plugin.
  - Steps: 1. `git submodule update --init plugins/obs-websocket` if not initialised; 2. run obs-websocket against the Rust frontend API; 3. run the test-plugin callback/event tests.
  - Done when: obs-websocket + test plugin pass.

### P6-37 Scripting host
- [ ] **P6-37.1 Lua**
  - Files: `shared/obs-scripting/obs-scripting.c` (471), `obs-scripting-lua.c` (1378), `obs-scripting-lua-frontend.c` (335), `obs-scripting-lua-source.c` (725), `obs-scripting-lua.h` (238), `obs-scripting.h` (67).
  - Steps: 1. port the script host with `mlua` (load/unload/reload, `script_*` callbacks, timers); 2. run the example scripts. 3. enumerate the four callbacks generated by the `DEFINE_VOID_DATA_CALLBACK` macro in `obs-scripting-lua-source.c` (activate/deactivate/show/hide) — macro-generated, invisible to ctags and the inventory; they must be registered in the mlua port.
  - Spec: S-039 (core), S-060 (Lua backend).
  - Vectors: test/conformance/scripts/scripting/, test/conformance/scripts/luascript/.
  - Done when: example scripts run.
- [ ] **P6-37.2 Python**
  - Files: `shared/obs-scripting/obs-scripting-python.c` (1769), `obs-scripting-python-frontend.c` (450), `obs-scripting-python-import.c` (210), `obs-scripting-python-import.h` (279), `obs-scripting-python.h` (219).
  - Steps: 1. port the host with `pyo3` (GIL handling, Python path setting); 2. block on S-063 per D-29 for the backend semantics; 3. run the example scripts.
  - Spec: S-039 (core); S-063 pending (D-29).
  - Vectors: test/conformance/scripts/pyscript/ (25 vectors exist, no spec yet).
  - Done when: example scripts run.
- [ ] **P6-37.3 Bindings**
  - Files: `shared/obs-scripting/obslua/obslua.i` (111), `obspython/obspython.i` (139).
  - Steps: 1. list every `obs_*` symbol the SWIG `.i` files expose; 2. generate the Rust bindings from the same header list; 3. diff the symbol lists.
  - Done when: symbol list equal.
- [ ] **P6-37.4 Manager UI** +UI
  - Files: `plugins/frontend-tools/scripts.cpp` (694), `scripts.hpp` (64), `plugins/frontend-tools/forms/scripts.ui` (320).
  - Steps: 1. port the script manager UI (add/remove/run/reload/log); 2. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-37.5 Docs examples**
  - Files: `docs/sphinx` scripting pages (obs-studio checkout).
  - Steps: 1. extract every example from the scripting pages; 2. run them as tests against the host.
  - Done when: examples pass.
- [ ] **P6-37.6 Write S-063 (added)** — Python scripting backend spec (D-29)
  - Files: `shared/obs-scripting/obs-scripting-python.c` (1769), `obs-scripting-python-frontend.c` (450), the existing `test/conformance/scripts/pyscript/` (25 vectors).
  - Steps: 1. write `specs/S-063-pyscript-backend.md` from the 25 existing `pyscript` vectors + the C (GIL handling, import hook, timer semantics). 2. add the row to `specs/README.md`.
  - Pitfalls: S-039 explicitly excludes the backends and S-060 covers Lua only — pair with S-060, do not duplicate it.
  - Done when: S-063 committed; P6-37.2 cites it.

### P6-38 Auto scene switcher
- [ ] **P6-38.1 Windows**
  - Files: `plugins/frontend-tools/auto-scene-switcher-win.cpp` (77), `auto-scene-switcher.cpp` (525).
  - Steps: 1. port the foreground window title/exe polling (Windows); 2. matching unit test.
  - Done when: matching test passes.
- [ ] **P6-38.2 macOS**
  - Files: `plugins/frontend-tools/auto-scene-switcher-osx.mm` (45).
  - Steps: 1. port the accessibility foreground-app polling (macOS); 2. matching unit test.
  - Done when: matching test passes.
- [ ] **P6-38.3 Linux**
  - Files: `plugins/frontend-tools/auto-scene-switcher-nix.cpp` (199).
  - Steps: 1. port the X11 `_NET_ACTIVE_WINDOW` polling; 2. document that Wayland is not available.
  - Done when: matching test passes; Wayland documented.
- [ ] **P6-38.4 Rules/UI** +UI
  - Files: `plugins/frontend-tools/auto-scene-switcher.cpp` (525), `auto-scene-switcher.hpp` (46), `plugins/frontend-tools/forms/auto-scene-switcher.ui` (313).
  - Steps: 1. port the regex rules + fallback scene + interval; 2. port the rules UI; 3. UI bookend.
  - Done when: UI bookend passes.

### P6-39 Output timer and captions
- [ ] **P6-39.1 Timer** +UI
  - Files: `plugins/frontend-tools/output-timer.cpp` (332), `output-timer.hpp` (44), `plugins/frontend-tools/forms/output-timer.ui` (250).
  - Steps: 1. port the stream/record stop timers; 2. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-39.2 Captions framework**
  - Files: `plugins/frontend-tools/captions.cpp` (439), `captions.hpp` (21), `captions-handler.cpp` (43), `captions-handler.hpp` (57).
  - Steps: 1. port the captions source + handler; 2. caption-event unit test.
  - Done when: event test passes.
- [ ] **P6-39.3 MS speech**
  - Files: `plugins/frontend-tools/captions-mssapi.cpp` (186), `captions-mssapi.hpp` (47), `captions-mssapi-stream.cpp` (410), `captions-mssapi-stream.hpp` (87).
  - Steps: 1. port the MS Speech recognition via the `windows` crate; 2. Windows runtime test.
  - Done when: runs on Windows.

### P6-40 DeckLink/AJA output UIs
- [ ] **P6-40.1 DeckLink** +UI
  - Files: `plugins/decklink-output-ui/DecklinkOutputUI.cpp` (145), `DecklinkOutputUI.h` (34), `decklink-ui-main.cpp` (507), `plugins/decklink-output-ui/forms/output.ui` (113).
  - Steps: 1. port the DeckLink output UI (P5-42 SDK stays FFI per D-10); 2. UI bookend.
  - Done when: UI bookend passes.
- [ ] **P6-40.2 AJA** +UI
  - Files: `plugins/aja-output-ui/AJAOutputUI.cpp` (253), `AJAOutputUI.h` (42), `aja-ui-main.cpp` (448), `plugins/aja-output-ui/forms/output.ui` (123).
  - Steps: 1. port the AJA output UI (P5-43 SDK stays FFI per D-10); 2. UI bookend.
  - Done when: UI bookend passes.

### P6-41 Tray and platform bits
- [ ] **P6-41.1 Tray**
  - Files: `frontend/widgets/OBSBasic_SysTray.cpp` (160), `frontend/OBSApp.cpp` (2107; tray notifications).
  - Steps: 1. port the system tray icon + menu (minimize/restore/exit, projector menu); 2. port tray notifications.
  - Done when: tray works per OS.
- [ ] **P6-41.2 Progress/notifications**
  - Files: `frontend/utility/platform-windows.cpp` (446; taskbar progress), `frontend/utility/platform-osx.mm` (361; toasts).
  - Steps: 1. port the taskbar progress (Windows) + toasts; 2. per-OS smoke test.
  - Done when: works per OS.
- [ ] **P6-41.3 Dark mode/DPI**
  - Files: `frontend/utility/platform-osx.mm` (361), `platform-windows.cpp` (446), `platform-x11.cpp` (327), `display-helpers.hpp` (146).
  - Steps: 1. port dark-mode detection + DPI scaling helpers; 2. per-OS smoke test.
  - Done when: works per OS.
- [ ] **P6-41.4 Menus/jump list**
  - Files: `frontend/utility/platform-osx.mm` (361; macOS app menu), `platform-windows.cpp` (446; Windows jump list), `frontend/utility/system-info-posix.cpp` (676), `system-info-windows.cpp` (262), `system-info-macos.mm` (101).
  - Steps: 1. port the macOS app menu + Windows jump list; 2. port the startup system-info logging.
  - Done when: works per OS.

## G. Packaging and release

### P6-42 Windows
- [ ] **P6-42.1 Installer**
  - Files: `additional_install_files/` (data/, exec32–64{,d,r}/, libs32–64{,d,r}/, misc/, steam/), `cmake/windows/cpackconfig.cmake` (15), `frontend/updater/updater.rc` (146).
  - Steps: 1. mirror the install-tree layout from `additional_install_files/`; 2. build the portable ZIP via CPack (`CPACK_GENERATOR ZIP`); 3. handle the VC runtime.
  - Pitfalls: parity per D-34 — the C build ships a ZIP only (no NSIS/Inno); no new installer format in 1.0.
  - Done when: ZIP builds and launches.
- [ ] **P6-42.2 Portable ZIP**
  - Files: `additional_install_files/`, `frontend/obs-main.cpp` (1065–1079, portable detection).
  - Steps: 1. package the ZIP with portable mode enabled; 2. launch test from an arbitrary directory.
  - Done when: ZIP launches and runs.
- [ ] **P6-42.3 Signing**
  - Files: `frontend/updater/updater.rc` (146), `cmake/windows/` (signing hooks).
  - Steps: 1. Authenticode-sign the binaries + updater; 2. verify the signatures.
  - Done when: signatures verify.

### P6-43 macOS
- [ ] **P6-43.1 Bundle**
  - Files: `frontend/cmake/macos/` (`Info.plist.in`, `entitlements.plist`, `entitlements-extension.plist`, `exportOptions.plist.in`, `exportOptions-extension.plist.in`, `qt.conf`, `Assets.xcassets/`).
  - Steps: 1. build the universal binary + `.app` bundle; 2. embed the camera extension; 3. bundle launch test.
  - Done when: bundle launches.
- [ ] **P6-43.2 Notarise**
  - Files: `frontend/cmake/macos/entitlements.plist`, `entitlements-extension.plist`, `exportOptions.plist.in`.
  - Steps: 1. port the entitlements; 2. notarise the bundle.
  - Done when: notarisation passes.
- [ ] **P6-43.3 Update feed**
  - Files: `frontend/utility/OBSSparkle.mm` (39), `MacUpdateThread.cpp` (74).
  - Steps: 1. point Sparkle at the feed URL; 2. mock feed test.
  - Done when: feed tested.

### P6-44 Linux
- [ ] **P6-44.1 Flatpak**
  - Files: `build-aux/com.obsproject.Studio.json` (171).
  - Steps: 1. adapt the manifest for the Rust build; 2. run flatpak-builder.
  - Done when: flatpak-builder runs the manifest.
- [ ] **P6-44.2 deb/rpm**
  - Files: `CMakeLists.txt` (37; CPack), `cmake/windows/cpackconfig.cmake` (15, generator reference).
  - Steps: 1. port the DEB CPack generator (C build ships DEB + TGZ, no RPM); 2. add RPM only if the release decision wants it; 3. install + run the `.deb`.
  - Done when: .deb installs and runs.
- [ ] **P6-44.3 Sessions**
  - Files: `frontend/OBSApp.cpp` (1274–1300, `obs_set_nix_platform`), `frontend/obs-main.cpp` (521–524, `QT_QPA_PLATFORMTHEME`).
  - Steps: 1. Wayland session test; 2. X11 session test.
  - Done when: both pass.

### P6-45 Size and startup
- [ ] **P6-45.1 Measure**
  - Steps: 1. measure the Rust binary size; 2. measure cold/warm startup; 3. record the numbers vs the C build.
  - Done when: numbers recorded vs the C build.
- [ ] **P6-45.2 Optimise**
  - Steps: 1. profile the startup path; 2. trim until within 10% of the C++ build.
  - Done when: within 10%.

### P6-46 End-to-end UI tests
- [ ] **P6-46.1 Tooling**
  - Steps: 1. pick the per-OS driver (AT-SPI, UIA, macOS accessibility, or screenshot compare); 2. get one journey passing per OS.
  - Done when: one journey passes per OS.
- [ ] **P6-46.2 Journeys**
  - Steps: 1. script the 30 journeys (add source, edit properties, record, stream to local RTMP, switch scene, studio mode, change setting, import collection, update, plugin manager, scripting); 2. run all 30.
  - Done when: all 30 pass.
- [ ] **P6-46.3 Nightly run**
  - Steps: 1. wire the journey suite into the nightly CI.
  - Done when: nightly green.

### P6-47 Beta programme
- [ ] **P6-47.1 Channel**
  - Steps: 1. publish the beta to the beta channel.
  - Done when: beta published.
- [ ] **P6-47.2 Triage**
  - Steps: 1. triage crash + regression reports for two releases.
  - Done when: two releases triaged.

### P6-48 Phase 6 review
- [ ] **P6-48.1 Parity checklist**
  - Steps: 1. walk the menus (from the 36 `.ui` files) and settings pages; 2. generate `docs/rust-rewrite/parity-checklist.md`; 3. commit it.
  - Done when: checklist committed.
- [ ] **P6-48.2 Verify and tick**
  - Steps: 1. verify each checklist item against the running app; 2. tick it.
  - Done when: 100% ticked.

### P6-49 Frontend components sweep (added)
- [ ] **P6-49.1 Inventory and order**
  - Steps: 1. run `python3 tools/task-coverage.py`; 2. take the P6-49 rows (67 files); 3. sort by number of consumers and document the port order.
  - Done when: port order documented.
- [ ] **P6-49.2 `frontend/components/*`**
  - Files: 21 files in `frontend/components/`: `ClickableLabel.hpp` (21), `DelButton.hpp` (13), `EditWidget.hpp` (15), `FlowFrame.cpp` (134)/`.hpp` (37), `FlowLayout.cpp` (170)/`.hpp` (40), `FocusList.cpp` (26)/`.hpp` (17), `InfoChip.cpp` (28)/`.hpp` (28), `MenuButton.cpp` (37)/`.hpp` (16), `MenuCheckBox.cpp` (112)/`.hpp` (50), `SilentUpdateCheckBox.hpp` (33), `SilentUpdateSpinBox.hpp` (33), `UIValidation.cpp` (117)/`.hpp` (28), `UrlPushButton.cpp` (27)/`.hpp` (22).
  - Steps: 1. per widget: view-model unit test, then the cxx-qt port (D-01); 2. UI bookend each (snapshot in light + dark); 3. exclude the C file from the build when ported.
  - Done when: each widget has its snapshot test and the C file is excluded.
- [ ] **P6-49.3 `shared/qt/*`**
  - Files: 45 code files across 7 subdirs of `shared/qt/`: `icon-label/` (2), `idian/` (29), `obs-spinbox/` (6), `plain-text-edit/` (2), `slider-ignorewheel/` (2), `vertical-scroll-area/` (2), `wrappers/` (2).
  - Steps: 1. port each per the D-01 toolkit decision; 2. view-model test first (UI bookend); 3. exclude the C file when ported.
  - Done when: same as above.
- [ ] **P6-49.4 Final sweep**
  - Files: `frontend/data/striped_line.effect` (38; loaded by `OBSBasicPreview.cpp:56` for the preview overlay).
  - Steps: 1. port (or vendor) the striped-line effect via the S-020 effect language; 2. regenerate `docs/rust-rewrite/coverage/coverage-matrix.md` with `tools/task-coverage.py --write`.
  - Spec: S-020 (effect language).
  - Done when: P6-49 owns zero rows.

### P6-50 Frontend spec series (added)
- Goal: every `frontend/` file cited by a spec — the frontend (~109k lines) was previously specified only as S-048 (formats/CLI) + S-067 (preview interaction); round 6 adds the behavioural spec series S-068…S-077 + S-087. Per-file lists in `docs/rust-rewrite/specs/BACKLOG.md`; every spec gets a row in `specs/README.md`. UI-heavy behaviour is specified as behavioural tables (the S-048 convention), not line-by-line.
- [ ] **P6-50.1 Write S-068** — frontend settings dialog (10 files / 256 fns)
  - Files: `frontend/settings/*` (OBSBasicSettings.cpp 158, _Stream 41, _A11y 18, _Appearance 7, OBSHotkey* 27, .hpp 27).
  - Steps: 1. write `specs/S-068-settings-dialog.md`: page structure, dirty-flag/Apply-OK-Cancel semantics, restart-needed rules, per-page load/save key tables (cross-ref S-048 §4), hotkey capture widget. 2. add the row.
  - Done when: S-068 committed; all 10 files cited.
- [ ] **P6-50.2 Write S-069** — frontend main window & controls (58 files / 821 fns)
  - Files: `OBSBasic.cpp/hpp` (35+11), `OBSApp*` (2), `OBSBasic_MainControls` (42), `OBSBasicControls` (16), `OBSBasicStatusBar` (28+1), `OBSBasic_Scenes` (35), `OBSBasic_Transitions` (53), `OBSBasic_Streaming` (11), `OBSBasic_Recording` (17), `OBSBasic_StudioMode` (8), `OBSBasic_SysTray` (5), `OBSBasic_Service` (5), `OBSBasic_Canvases` (4), `OBSBasic_ReplayBuffer` (10), `OBSBasic_VirtualCam` + `OBSBasicVCamConfig` (14), `OBSBasic_Icons` (2), `OBSBasic_ContextToolbar` (10), `OBSBasic_Browser` (12), `OBSBasic_YouTube` (20), `OBSQTDisplay` (11+1), `OBSBasicPreview*` + `OBSBasic_Preview` + scaling combo/label + `PreviewProgramSizeObserver` (127+6), `Multiview` (6+2), `OBSProjector` + `OBSBasic_Projectors` (21+14), `OBSBasic_Dropfiles` (7), `OBSBasicInteraction` (15), `OBSBasic_Clipboard` (14), `OBSBasic_Screenshots` (3), `OBSBasic_Hotkeys` (5), `OBSBasicStats` (16), `OBSBasic_Docks` (10), `OBSDock` (1), `AutoConfig*` + `TestMode.hpp` (63), `OBSCanvas` (5), `display-helpers` (4).
  - Pitfalls: the interaction behaviour of `OBSBasic_SceneItems.cpp` + `OBSBasicPreview.cpp` is S-067 (P6-07.9) — S-069 covers the remaining widget surface, not the hit-test/handle rules.
  - Done when: S-069 committed; all 58 files cited.
- [ ] **P6-50.3 Write S-070** — frontend scene collections, profiles & importers (11 files / 148 fns)
  - Files: `OBSBasic_SceneCollections` (42), `OBSBasic_Profiles` (30), `importers/*` (48), `importer/*` (22).
  - Steps: 1. write `specs/S-070-collections-profiles-importers.md`: collection save/load/duplicate/rename, profile format (cross-ref S-048), the four importer transforms (cross-ref P6-26.7 inventory), dialog view model. 2. add the row.
  - Done when: S-070 committed; all 11 files cited.
- [ ] **P6-50.4 Write S-071** — frontend audio mixer & volume widgets (11 files / 156 fns)
  - Files: `AudioMixer` (48), `VolumeControl` (39+1), `VolumeMeter` (34), `VolumeName` (13), `VolumeSlider` (2), `VolumeAccessibleInterface` (2), `BalanceSlider.hpp` (1), `AbsoluteSlider` (7), `MuteCheckBox.hpp` (1), `MenuCheckBox` (7).
  - Steps: write `specs/S-071-audio-mixer-ui.md` — the UI half of S-032 (fader curves, meter peak behaviour, per-source widget state).
  - Done when: S-071 committed; all 11 files cited.
- [ ] **P6-50.5 Write S-072** — frontend dialogs & source pickers/toolbars (47 files / 260 fns)
  - Files: `OBSBasicFilters` (46+1), `OBSBasicProperties` (17), `OBSBasicSourceSelect` (26), `SourceSelectButton` (8), `OBSBasicTransform` (20+1), `OBSBasicAdvAudio` + `OBSAdvAudioCtrl` (39), `OBSMissingFiles` + model/delegate (25), `NameDialog` (4), `OBSPermissions` (7), `OBSAbout` (4), `OBSLogViewer` (4), `OBSUpdate` (1), `OBSWhatsNew` (3), `OBSExtraBrowsers` + model/delegate (20), `LogUploadDialog` (6), `OBSIdianPlayground` (1), `OBSHotkey*` label (5+2), `SourceTree*` (39+1), `SceneTree` (8), `AlignmentSelector` + accessible (23), `SourceToolbar` + 8 toolbars (35+2), `VisibilityItem*` (12), `SceneRenameDelegate` (2), `ColorSourceToolbar` (4), `GameCaptureToolbar` (4), `TextSourceToolbar` (4), `BrowserToolbar` (2), `DeviceCaptureToolbar` (2), `DisplayCaptureToolbar` (1), `AudioCaptureToolbar` (1), `ApplicationAudioCaptureToolbar` (1), `WindowCaptureToolbar` (1), `ImageSourceToolbar` (2), `ComboSelectToolbar` (6), `MenuButton` (1), `UrlPushButton` (1), `ClickableLabel.hpp` (1), `FlowLayout` (10), `FlowFrame` (2), `FocusList` (1), `OBSHotkeyEdit/Widget.hpp` (12).
  - Done when: S-072 committed; all 47 files cited.
- [ ] **P6-50.6 Write S-073** — frontend output pipeline UI (26 files / 233 fns)
  - Files: `SimpleOutput` (33), `AdvancedOutput` (23), `BasicOutputHandler` (30+4), `OBSBasic_OutputHandler` (7), `audio-encoders` (14), `FFmpegCodec/Format/Shared` (11), `RemuxQueueModel` + `RemuxWorker` + `RemuxEntryPathItemDelegate` + `OBSRemux` (39), `MultitrackVideo*` (42), `WHIPSimulcastEncoders` (4), `QuickTransition` (3+1), `SilentUpdateCheckBox/SpinBox` (2), `MediaControls` (35).
  - Done when: S-073 committed; all 26 files cited.
- [ ] **P6-50.7 Write S-074** — frontend updater, oauth & YouTube (33 files / 226 fns)
  - Files: `updater/*` (64), `oauth/*` (57), `YouTube*` docks/wrappers (60), `GoLiveAPI_*` (9), `WhatsNew*` + `RemoteTextThread` (17), `BrowserDock` (3), `MacUpdateThread` (3), `update-helpers` (2), `AutoUpdateThread` (8), `crypto-helpers-*` (3), `OBSSparkle`/`OBSUpdateDelegate` (4), `OBSBasic_Updater` (5), `OBSUpdate.cpp` dialog (1).
  - Done when: S-074 committed; all 33 files cited.
- [ ] **P6-50.8 Write S-075** — frontend plugin manager, crash handler & system info (26 files / 134 fns)
  - Files: `plugin-manager/*` (35), `CrashHandler*` (20), `system-info-*` (23), `platform-{windows,osx,x11}` (54), `win-dll-blocklist` (4), `NativeEventFilter*` (2), `OBSProxyStyle` (2).
  - Done when: S-075 committed; all 26 files cited.
- [ ] **P6-50.9 Write S-076** — properties view & Qt widget kit (27 files / 184 fns)
  - Files: `shared/properties-view/*` (73), `shared/qt/idian/*` (44), `shared/qt/obs-spinbox/*` (24), `shared/qt/{slider-ignorewheel, icon-label, plain-text-edit, vertical-scroll-area}/*` (7), `shared/qt/wrappers/qt-wrappers.cpp` (22).
  - Steps: write `specs/S-076-properties-view-ui.md` — property-view rendering rules (widget per property type, visibility/enable callbacks, value round-trips), the idian row/state model, spinbox behaviour.
  - Done when: S-076 committed; all 27 files cited.
- [ ] **P6-50.10 Write S-077** — frontend API & utility glue (19 files / 136 fns)
  - Files: `frontend/api/obs-frontend-api.{cpp,h}` (60+2 — the public frontend API surface; S-048 lists the catalog but never the behaviour), `Thumbnail*` (23), `undo_stack` (11), `OBSTranslator` (1), `UIValidation` (3), `ResizeSignaler.hpp` (1), `SurfaceEventFilter` (1), `SettingsEventFilter` (1), `item-widget-helpers*` (3), `display-helpers.hpp` (4), `models/*` (8+1), `obf.c` (1).
  - Done when: S-077 committed; all 19 files cited.
- [ ] **P6-50.11 Write S-087** — frontend-tools plugin (13 files / 138 fns)
  - Files: `plugins/frontend-tools/*` (scripts.cpp 31, captions-mssapi-stream 28, auto-scene-switcher 22+13, output-timer 17, captions 15, captions-mssapi 4, captions-handler 3, tool-helpers 2, module 2).
  - Steps: write `specs/S-087-frontend-tools.md` — auto-scene-switcher scoring/decision rules, output-timer semantics, the Windows MSS captions contract, the scripts-manager behaviour (cross-ref P6-37.4).
  - Done when: S-087 committed; all 13 files cited.
