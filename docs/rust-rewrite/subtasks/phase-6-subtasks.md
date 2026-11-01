# Phase 6 subtasks

Parent file: [../phase-6-frontend.md](../phase-6-frontend.md). Each subtask has **Files**, **Steps**, **Done when**.

Verified facts: the main window is split across `frontend/widgets/OBSBasic*.cpp` (≈35 files: `_Docks`, `_Hotkeys`, `_Preview`, `_Profiles`, `_SceneCollections`, `_Scenes`, `_SceneItems`, `_Streaming`, `_Recording`, `_ReplayBuffer`, `_StudioMode`, `_Transitions`, `_VirtualCam`, `_SysTray`, `_Updater`, `_YouTube`, …). `frontend/forms/` has 34 `.ui` files. `frontend/data/locale/` has 77 locale files. Themes are `.obt` (theme) and `.ovt` (variant) files in `frontend/data/themes/` (`Yami`, `Acri`, `Dark`, `Light`, `Rachni`, `System`). The frontend API (`frontend/api/obs-frontend-api.h`) has 99 exported functions and 44 `OBS_FRONTEND_EVENT_*` values. Scripting bindings are SWIG interfaces: `shared/obs-scripting/obslua/obslua.i`, `obspython/obspython.i`. `plugins/obs-websocket` and `plugins/obs-browser` (needed by several tasks) are empty submodules until initialised.

**UI bookend** (written "+UI"): (a) view-model unit test before the widget, (b) screenshot/accessibility-tree snapshot in light and dark themes, (c) full keyboard-only operation, (d) string keys taken from `frontend/data/locale/en-US.ini` (no new hard-coded text).

## A. Application shell

### P6-01 Launch and CLI
- [ ] **P6-01.1 Argument table** — Files: `frontend/obs-main.cpp`. Steps: list every `--flag` parsed (`--portable`/`-p`, `--multi`/`-m`, `--profile`, `--collection`, `--scene`, `--startstreaming`, `--startrecording`, `--startreplaybuffer`, `--startvirtualcam`, `--minimize-to-tray`, `--verbose`, `--unfiltered_log`, `--disable-updater`, `--disable-missing-files-check`, `--only-bundled-plugins`, `--safe-mode`, `--always-on-top`, `--studio-mode`, `--allow-opengl`, `--steam`, `--help`/`-h`, `--version`/`-V`; verified by grepping `arg_is(...)` in `obs-main.cpp`: these 22 are the complete set — re-grep when starting, since upstream may add flags). Done when: table lists all with semantics.
- [ ] **P6-01.2 Parser** — `clap`; unknown arguments forwarded to Qt/plugins as today. Done when: parity test runs each flag.
- [ ] **P6-01.3 Single instance** — lock file / named mutex, `--multi` bypass, prompt text from locale. Done when: second launch shows the dialog.
- [ ] **P6-01.4 Crash handler/log rotation** — `frontend/utility/CrashHandler*.{cpp,mm}`; keep log dir layout (`logs/`, `crashes/`), 30-file cap. Done when: forced crash writes report.
- [ ] **P6-01.5 OS quirks** — Windows (DPI awareness, priority class), macOS (App Nap), Linux (X11 vs Wayland platform choice, `QT_QPA_PLATFORM`). Done when: logged per OS.

### P6-02 Application object and config
- [ ] **P6-02.1 Config** — `OBSApp.cpp/.hpp`: `global.ini` + `user.ini` handling via `obs-util` config. Done when: reads/writes real files.
- [ ] **P6-02.2 Paths/portable** — portable mode (`portable_mode.txt`), config dirs per OS. Done when: moves with the folder.
- [ ] **P6-02.3 First run** — detect missing profile/collection, show wizard. Done when: clean-profile test.

### P6-03 Themes
- [ ] **P6-03.1 Parser** — `.obt`/`.ovt` syntax: variables, `@define`, calc expressions, `OBSTheme` (`frontend/utility/OBSTheme.hpp`, `OBSThemeVariable.hpp`). Done when: all shipped themes parse.
- [ ] **P6-03.2 Apply** — QSS generation or toolkit-native styles; palette; per-OS defaults (`System`). Done when: preview colours equal.
- [ ] **P6-03.3 Variants/high contrast/user themes** — `.ovt` overrides, user theme dir. Done when: user theme loads.
- [ ] **P6-03.4 Snapshots** — screenshot per shipped theme.

### P6-04 Localisation
- [ ] **P6-04.1 Loader** — `frontend/data/locale.ini` (language list) and `locale/*.ini`. Done when: all 77 files load.
- [ ] **P6-04.2 Fallback/format** — `en-US` fallback, `$1` style arguments. Done when: tests.
- [ ] **P6-04.3 Coverage** — script: keys used in code vs keys in `en-US.ini`. Done when: report lists unused/missing.

### P6-05 Logging and crash upload
- [ ] **P6-05.1 Viewer** — `dialogs/OBSLogViewer`. +UI.
- [ ] **P6-05.2 Upload** — `dialogs/LogUploadDialog` (HTTP POST to analyzer service, URL copy). Done when: mock server test.
- [ ] **P6-05.3 Crash reports** — format as today.

### P6-06 Main window skeleton
- [ ] **P6-06.1 Window/menus** — `widgets/OBSBasic.cpp`, `OBSBasic_MainControls.cpp`, `forms/OBSBasic.ui`. Done when: all menu items present (list from `.ui`).
- [ ] **P6-06.2 Status bar** — `OBSBasicStatusBar`, `StatusBarWidget`: rec/stream time, CPU, FPS, kbps, dropped frames. Done when: values live.
- [ ] **P6-06.3 Docks/geometry** — `OBSBasic_Docks.cpp`: layout save/restore, `dockState`. Done when: restart restores.
- [ ] **P6-06.4 Studio/always-on-top** — `OBSBasic_StudioMode.cpp`. Done when: toggles.
- [ ] **P6-06.5 Action table test** — every action has a keyboard shortcut/handler.

## B. Core UI components

### P6-07 Preview widget
- [ ] **P6-07.1 Display embedding** — `widgets/OBSQTDisplay.cpp`, `OBSBasicPreview.cpp`: native window handle per platform (X11 window id, Wayland subsurface, HWND, NSView). Done when: renders on all.
- [ ] **P6-07.2 Selection/handles** — hit-testing of scene items, rotate/scale/crop handles with modifier keys (Ctrl, Alt, Shift). Done when: behaviour table.
- [ ] **P6-07.3 Snapping** — snap to edges/centre/other sources (grid size from config), spacing helpers. Done when: tests.
- [ ] **P6-07.4 Drop** — `OBSBasic_Dropfiles.cpp`: files/URLs create image/media/browser/text sources. Done when: drop of 5 file types.
- [ ] **P6-07.5 Scaling/overlays** — `OBSPreviewScalingComboBox`, safe areas, overflow modes. Done when: golden overlay.
- [ ] **P6-07.6 Interaction** — send mouse/key to the source (`OBSBasicInteraction`). Done when: browser source receives click.
- [ ] **P6-07.7 Studio dual preview** — program/preview. Done when: transition runs.
- [ ] **P6-07.8 Multiview/projectors** — `components/Multiview.cpp`, `widgets/OBSProjector.cpp`, `OBSBasic_Projectors.cpp`. Done when: windowed + fullscreen projector.

### P6-08 Scene list
- [ ] **P6-08.1 Model/view** — `components/SceneTree.cpp`, `widgets/OBSBasic_Scenes.cpp`, `SceneRenameDelegate`. +UI.
- [ ] **P6-08.2 Reorder** — drag and drop, grid/list modes. Done when: order persisted.
- [ ] **P6-08.3 Context menu** — rename, duplicate, remove, filters, projector, screenshot, transition override. Done when: all items work.

### P6-09 Source tree
- [ ] **P6-09.1 Model** — `components/SourceTreeModel`, `SourceTreeItem`, groups (expand/collapse). +UI.
- [ ] **P6-09.2 Row widgets** — `VisibilityItemWidget`, lock, label, rename-in-place. Done when: toggles.
- [ ] **P6-09.3 Reorder/multi-select** — group/ungroup, drag across groups. Done when: tests.
- [ ] **P6-09.4 Context menus** — transform, order, scale filtering, blending, show/hide transitions, deinterlacing, audio mixers. Done when: each item hits `obs-ffi`.
- [ ] **P6-09.5 Accessibility** — roles and names.

### P6-10 Audio mixer
- [ ] **P6-10.1 Meter** — `components/VolumeMeter.cpp`: peak/magnitude/input-peak bars, decay, clip indicator, warning/error thresholds, colour from theme. Done when: pixel compare with reference frames.
- [ ] **P6-10.2 Fader/balance** — `VolumeSlider`, `BalanceSlider`, `AbsoluteSlider`, dB/percent. Done when: curve matches P2-14.
- [ ] **P6-10.3 Mute/monitor** — `MuteCheckBox`, per-source menu. Done when: toggles.
- [ ] **P6-10.4 Layouts** — `widgets/AudioMixer.cpp`: vertical/horizontal/hidden-sources. Done when: persisted.
- [ ] **P6-10.5 Advanced audio** — `dialogs/OBSBasicAdvAudio.cpp`, `components/OBSAdvAudioCtrl.cpp`: sync offset, tracks, monitoring, balance. Done when: all fields.
- [ ] **P6-10.6 Reference compare**

### P6-11 Transitions dock
- [ ] **P6-11.1 Selector/duration** — `widgets/OBSBasic_Transitions.cpp`. +UI.
- [ ] **P6-11.2 Properties** — add/rename/remove transitions, properties dialog.
- [ ] **P6-11.3 Quick transitions/T-bar** — `utility/QuickTransition.cpp`, hotkeys. Done when: transitions fire.

### P6-12 Controls dock
- [ ] **P6-12.1 Stream/record** — `widgets/OBSBasicControls.cpp`, `OBSBasic_Streaming.cpp`, `OBSBasic_Recording.cpp`: button text/state, pause. +UI.
- [ ] **P6-12.2 Replay/vcam** — `OBSBasic_ReplayBuffer.cpp`, `OBSBasic_VirtualCam.cpp`.
- [ ] **P6-12.3 States** — reconnecting/paused/failed message dialogs from locale.

### P6-13 Stats dock
- [ ] **P6-13.1 Collection** — `widgets/OBSBasicStats.cpp`: CPU%, mem, disk space, FPS, render time, skipped/missed frames, output bitrate, dropped.
- [ ] **P6-13.2 Table +UI** — reset button, colour thresholds.

### P6-14 Source toolbars
- [ ] **P6-14.1 Simple** — `ColorSourceToolbar`, `TextSourceToolbar`, `ImageSourceToolbar`. +UI.
- [ ] **P6-14.2 Browser/media** — `BrowserToolbar`, `MediaControls`.
- [ ] **P6-14.3 Capture** — `DeviceCaptureToolbar`, `DisplayCaptureToolbar`, `WindowCaptureToolbar`, `GameCaptureToolbar`, `AudioCaptureToolbar`, `ApplicationAudioCaptureToolbar`, `ComboSelectToolbar`.

### P6-15 Dock framework
- [ ] **P6-15.1 Base/persist** — `docks/OBSDock.cpp`, dock id registry. Done when: state persists.
- [ ] **P6-15.2 Browser docks** — `docks/BrowserDock.cpp`, `dialogs/OBSExtraBrowsers.cpp`, `utility/ExtraBrowsers*`. Done when: add by URL.
- [ ] **P6-15.3 API** — `obs_frontend_add_dock_by_id`, `add_custom_qdock`, `remove_dock`. Done when: contract test.

### P6-16 Properties view
- [ ] **P6-16.1 View model** — from `obs_properties_t`; `shared/properties-view` is the reference. Done when: model reflects all 15 property kinds.
- [ ] **P6-16.2 Basic** — bool, int (slider/spin/scroller), float, text (single/multiline/password/info). +UI.
- [ ] **P6-16.3 Path/list** — path file/dir/save, list (combo/radio/editable), editable list. +UI.
- [ ] **P6-16.4 Rich** — colour/alpha, font, frame rate, button, group. +UI.
- [ ] **P6-16.5 Live update** — debounce, `modified` callbacks, visibility, reset to defaults, warnings. Done when: sample plugin updates live.
- [ ] **P6-16.6 Dialogs** — `OBSBasicProperties`, `OBSBasicFilters` (filter list, preview, drag order), `OBSBasicSourceSelect`, `OBSBasicTransform` (position, rotation, size, bounds, crop), `OBSBasicInteraction`. +UI each.

### P6-17 Media controls
- [ ] **P6-17.1 Play/pause/seek** — `components/MediaControls.cpp`, slider. +UI.
- [ ] **P6-17.2 Extras** — time labels, loop, restart, previous/next.

### P6-18 Accessibility pass
- [ ] **P6-18.1 Labels** — audit every widget role/name (`AccessibleAlignment*`, `VolumeAccessibleInterface` exist for custom widgets). Done when: report.
- [ ] **P6-18.2 Keyboard** — tab order and shortcuts. Done when: all dialogs keyboard-operable.
- [ ] **P6-18.3 Contrast** — WCAG AA checks for all themes. Done when: script report.

## C. Settings and wizards

### P6-19 Settings dialog framework
- [ ] **P6-19.1 Pages** — `settings/OBSBasicSettings.cpp`, `forms/OBSBasicSettings.ui`. Done when: navigation.
- [ ] **P6-19.2 Change tracking** — dirty flags, Apply/OK/Cancel, `SettingsEventFilter`. Done when: tests.
- [ ] **P6-19.3 Restart-needed/search** — restart prompt list. Done when: matches current set.

### P6-20 Settings pages
- [ ] **P6-20.1 General/Appearance** — language, theme (`OBSBasicSettings_Appearance.cpp`), updates channel, system tray, snapping, projectors, preview. +UI.
- [ ] **P6-20.2 Stream** — `OBSBasicSettings_Stream.cpp`: service, server, key, auth, bandwidth test, multitrack. +UI.
- [ ] **P6-20.3 Output simple** — `utility/SimpleOutput.cpp`: bitrate, encoder, recording path/format/quality. +UI.
- [ ] **P6-20.4 Output advanced** — `utility/AdvancedOutput.cpp`, encoders property views, tracks, replay buffer, file name formatting. +UI.
- [ ] **P6-20.5 Audio/Video** — sample rate, channels, devices, base/output resolution, FPS, downscale filter, colour format/space/range, SDR white level.
- [ ] **P6-20.6 Hotkeys/A11y/Advanced** — `OBSBasicSettings_A11y.cpp`, process priority, renderer, network (bind IP, dynamic bitrate), delay, reconnect, browser hardware accel.
- [ ] **P6-20.7 Compatibility** — load old `basic.ini`/`service.json`/`streamEncoder.json`; display identical values.

### P6-21 Hotkey editor
- [ ] **P6-21.1 Capture** — `settings/OBSHotkeyEdit`, `OBSHotkeyWidget`, `OBSHotkeyLabel`: per-OS key capture incl. mouse buttons. Done when: modifier combos.
- [ ] **P6-21.2 Conflicts** — highlight duplicates. Done when: test.
- [ ] **P6-21.3 Clear/reset/filter**.

### P6-22 Auto-configuration wizard
- [ ] **P6-22.1 Framework** — `wizards/AutoConfig*.cpp` pages. +UI.
- [ ] **P6-22.2 Bandwidth test** — `AutoConfigTestPage`: test stream to ingest, bitrate ramp. Done when: mock server.
- [ ] **P6-22.3 Encoder tests** — resolution/FPS/encoder trials. Done when: logic equal.
- [ ] **P6-22.4 Apply** results into profile.

### P6-23 First-run / what's new / about
- [ ] **P6-23.1 About** — `dialogs/OBSAbout.cpp`: authors, licence (`frontend/data/license`). +UI.
- [ ] **P6-23.2 What's new** — `OBSWhatsNew` (web view). +UI.
- [ ] **P6-23.3 macOS permissions** — `OBSPermissions`. +UI.

## D. Profiles, collections, importers

### P6-24 Scene collection manager
- [ ] **P6-24.1 Load/save** — `widgets/OBSBasic_SceneCollections.cpp`, `models/SceneCollection.cpp`. Done when: corpus loads.
- [ ] **P6-24.2 Backup/autosave** — `.json.bak`, autosave timer. Done when: crash recovery test.
- [ ] **P6-24.3 CRUD/import/export** — rename, duplicate, remove, export, import with path fix-up.
- [ ] **P6-24.4 Missing files** — `utility/MissingFiles*`.

### P6-25 Profile manager
- [ ] **P6-25.1 CRUD** — `widgets/OBSBasic_Profiles.cpp`.
- [ ] **P6-25.2 Export/import**.
- [ ] **P6-25.3 Encoder JSON** — `streamEncoder.json`, `recordEncoder.json`, `service.json`.

### P6-26 Importers
- [ ] **P6-26.1 Classic** — `importers/classic.cpp`. Done when: golden JSON.
- [ ] **P6-26.2 Streamlabs** — `importers/sl.cpp`.
- [ ] **P6-26.3 Studio** — `importers/studio.cpp`.
- [ ] **P6-26.4 XSplit** — `importers/xsplit.cpp`.
- [ ] **P6-26.5 Goldens** — sample inputs → expected collection.
- [ ] **P6-26.6 Importer dialog UI (added)** — `frontend/importer/` (`OBSImporter.cpp/.hpp`, `ImporterModel.cpp/.hpp`, `ImporterEntryPathItemDelegate.cpp/.hpp`) + `frontend/forms/OBSImporter.ui` + view-model. +UI bookend. Done when: the dialog lists parsed entries from each of the four importers and the P6-26.5 goldens still pass.

### P6-27 Missing files dialog
- [ ] **P6-27.1 List/relink** — `dialogs/OBSMissingFiles.cpp`, `utility/MissingFilesModel`. +UI.
- [ ] **P6-27.2 Search dir** — recursive search.

### P6-28 Remux dialog
- [ ] **P6-28.1 Queue** — `dialogs/OBSRemux.cpp`, `utility/RemuxQueueModel`, `RemuxWorker`. +UI.
- [ ] **P6-28.2 Worker** — use P2-29. Done when: progress and errors.

## E. Services, auth, updater

### P6-29 OAuth framework
- [ ] **P6-29.1 Listener** — `oauth/AuthListener`, `OAuth`: local HTTP redirect. Done when: code received.
- [ ] **P6-29.2 Storage** — OS keychain via `keyring`. Done when: tokens survive restart.
- [ ] **P6-29.3 Twitch** — `TwitchAuth`: scopes, chat/stats/feed docks.
- [ ] **P6-29.4 YouTube** — `YoutubeAuth`.
- [ ] **P6-29.5 Restream** — `RestreamAuth`.
- [ ] **P6-29.6 Refresh tests** — expiry simulated.

### P6-30 YouTube integration
- [ ] **P6-30.1 Broadcast** — `dialogs/OBSYoutubeActions.cpp`: create/select/schedule. +UI.
- [ ] **P6-30.2 Key** — stream key fetch. Done when: mock.
- [ ] **P6-30.3 Docks** — `docks/YouTubeAppDock`, `YouTubeChatDock`, `widgets/OBSBasic_YouTube.cpp`.
- [ ] **P6-30.4 Mock API tests**.

### P6-31 Streaming services UI
- [ ] **P6-31.1 Pickers** — service/server dropdowns from `rtmp-services`.
- [ ] **P6-31.2 Key hiding** — show/hide button.
- [ ] **P6-31.3 Multitrack** — `utility/MultitrackVideoOutput.cpp`, `GoLiveAPI_*.cpp` (network request, censored JSON, post data), error dialogs. Done when: mock GoLive API.

### P6-32 Updater
- [ ] **P6-32.1 Manifest** — `frontend/updater/` (`manifest.hpp`, `http.cpp`, `hash.cpp`, `patch.cpp`): fetch, compare. Done when: local server.
- [ ] **P6-32.2 Download/verify** — delta patches, hashes, RSA/ed25519 signature (`data/OBSPublicRSAKey.pem`). Done when: tamper test fails.
- [ ] **P6-32.3 Windows swap** — `updater.cpp` as helper process, in-place replace, rollback. Done when: update test VM.
- [ ] **P6-32.4 macOS/Linux** — Sparkle (`OBSSparkle.mm`), package manager notice. Done when: documented.
- [ ] **P6-32.5 Tests** with local manifest.

### P6-33 Plugin manager
- [ ] **P6-33.1 List** — `plugin-manager/PluginManager.cpp`, `InstalledPluginRow`. +UI.
- [ ] **P6-33.2 Enable/disable** — disabled-modules file, restart prompt (see recent fix in git log for stale data).
- [ ] **P6-33.3 Paths** — `LoaderPaths_{Linux,BSD,Flatpak}.hpp.in`, `PluginModuleLoader_*.cpp`. Done when: Flatpak path correct.

### P6-34 Virtual camera config
- [ ] **P6-34.1 Dialog** — `dialogs/OBSBasicVCamConfig.cpp`: output type (program, preview, scene, source). +UI.
- [ ] **P6-34.2 Persist**.

### P6-35 Debug dialogs
- [ ] **P6-35.1 Decide** — `dialogs/OBSIdianPlayground.cpp`: keep/drop.
- [ ] **P6-35.2 Port** if kept.

## F. Frontend API and tools

### P6-36 Frontend API
- [ ] **P6-36.1 Inventory** — 99 functions and 44 events from `frontend/api/obs-frontend-api.h`; table.
- [ ] **P6-36.2 Scenes/transitions/profiles/collections** functions.
- [ ] **P6-36.3 Streaming/recording/replay/vcam** functions and events.
- [ ] **P6-36.4 Docks/menus/save callbacks/events** — `OBSStudioAPI.cpp` equivalent; callback registration order.
- [ ] **P6-36.5 Contract test** — against obs-websocket (after submodule init) and the test plugin.

### P6-37 Scripting host
- [ ] **P6-37.1 Lua** — `shared/obs-scripting/obs-scripting-lua*.c`: `mlua`, script load/unload/reload, `script_*` callbacks, timers. Done when: example scripts run.
- [ ] **P6-37.2 Python** — `obs-scripting-python*.c`: `pyo3`, Python path setting, GIL handling. Done when: example scripts run.
- [ ] **P6-37.3 Bindings** — replace SWIG (`obslua.i`, `obspython.i`) by generating from the same header list; every `obs_*` function exposed under the same names. Done when: symbol list equal.
- [ ] **P6-37.4 Manager UI** — `plugins/frontend-tools/scripts.cpp` + forms. +UI.
- [ ] **P6-37.5 Docs examples** — run all examples in `docs/sphinx` scripting pages as tests.

### P6-38 Auto scene switcher
- [ ] **P6-38.1 Windows** — `auto-scene-switcher-win.cpp`: foreground window title/exe.
- [ ] **P6-38.2 macOS** — `auto-scene-switcher-osx.mm`.
- [ ] **P6-38.3 Linux** — `auto-scene-switcher-nix.cpp` (X11 only; Wayland not available — document).
- [ ] **P6-38.4 Rules/UI** — regex matching, fallback scene, interval. +UI.

### P6-39 Output timer and captions
- [ ] **P6-39.1 Timer** — `output-timer.cpp`: stream/record stop timers. +UI.
- [ ] **P6-39.2 Captions framework** — `captions.cpp`, `captions-handler.*`.
- [ ] **P6-39.3 MS speech** — `captions-mssapi*.cpp` on Windows via `windows` crate.

### P6-40 DeckLink/AJA output UIs
- [ ] **P6-40.1 DeckLink** — `plugins/decklink-output-ui`. +UI.
- [ ] **P6-40.2 AJA** — `plugins/aja-output-ui`. +UI.

### P6-41 Tray and platform bits
- [ ] **P6-41.1 Tray** — `widgets/OBSBasic_SysTray.cpp`.
- [ ] **P6-41.2 Progress/notifications** — taskbar, toasts.
- [ ] **P6-41.3 Dark mode/DPI** — `utility/platform-*.{cpp,mm}`, `display-helpers.hpp`.
- [ ] **P6-41.4 Menus/jump list** — macOS app menu, Windows jump list, `system-info-*.cpp` logging.

## G. Packaging and release

### P6-42 Windows
- [ ] **P6-42.1 Installer** — `build-aux/`, NSIS/Inno; VC runtime.
- [ ] **P6-42.2 Portable ZIP**.
- [ ] **P6-42.3 Signing** — Authenticode.

### P6-43 macOS
- [ ] **P6-43.1 Bundle** — universal binary, Info.plist, camera extension embed.
- [ ] **P6-43.2 Notarise** — entitlements.
- [ ] **P6-43.3 Update feed**.

### P6-44 Linux
- [ ] **P6-44.1 Flatpak** — manifest in `build-aux/`.
- [ ] **P6-44.2 deb/rpm**.
- [ ] **P6-44.3 Sessions** — Wayland and X11 test.

### P6-45 Size and startup
- [ ] **P6-45.1 Measure** binary size, cold/warm start.
- [ ] **P6-45.2 Optimise** to within 10% of C++.

### P6-46 End-to-end UI tests
- [ ] **P6-46.1 Tooling** — per OS (AT-SPI, UIA, macOS accessibility, or screenshot compare).
- [ ] **P6-46.2 Journeys** — 30: add source, edit properties, record, stream to local RTMP, switch scene, studio mode, change setting, import collection, update, plugin manager, scripting.
- [ ] **P6-46.3 Nightly run** in CI.

### P6-47 Beta programme
- [ ] **P6-47.1 Channel** — publish beta.
- [ ] **P6-47.2 Triage** — two releases.

### P6-48 Phase 6 review
- [ ] **P6-48.1 Parity checklist** — generate `parity-checklist.md` by walking menus (from the 34 `.ui` files) and settings.
- [ ] **P6-48.2 Verify and tick**.

### P6-49 Frontend components sweep (added)
- [ ] **P6-49.1 Inventory and order** — run `python3 tools/task-coverage.py`, take the P6-49 rows (67 files), sort by number of consumers. Done when: port order documented.
- [ ] **P6-49.2 `frontend/components/*`** — the 21 generic Qt widgets (FlowFrame/FlowLayout, FocusList, MenuButton, ClickableLabel, DelButton, EditWidget, InfoChip, UIValidation, UrlPushButton, SilentUpdate*…): view-model unit test + port, UI bookend each. Done when: each widget has its snapshot test and the C file is excluded.
- [ ] **P6-49.3 `shared/qt/*`** — the 45 shared Qt widgets (obs-spinbox, idian components, icon-label, plain-text-edit, qt-wrappers, slider-ignorewheel, vertical-scroll-area), per the D-01 toolkit decision. Done when: same as above.
- [ ] **P6-49.4 Final sweep** — `frontend/data/striped_line.effect` + regeneration of `docs/rust-rewrite/coverage/coverage-matrix.md` (`tools/task-coverage.py --write`). Done when: P6-49 owns zero rows.
