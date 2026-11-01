# Phase 6 guide: frontend

Read [00-developer-primer.md](00-developer-primer.md) first. Tasks: [../phase-6-frontend.md](../phase-6-frontend.md); subtasks: [../subtasks/phase-6-subtasks.md](../subtasks/phase-6-subtasks.md). The toolkit decision (P0-04) is a prerequisite; this guide assumes `cxx-qt` (keep Qt) and notes where another toolkit changes things.

## What the frontend is

≈ 89k lines of C++/Qt6 Widgets in `frontend/`:

| Directory | Contents |
|---|---|
| `obs-main.cpp` (`main` at line 882, `run_program` at 487) | argument parsing (22 flags), log file, single-instance check, crash handler, creates `OBSApp program(argc, argv, nameStore)` and calls `program.OBSInit()` then `program.exec()` |
| `OBSApp.cpp/.hpp`, `OBSApp_Themes.cpp` | `class OBSApp : public QApplication`: config (`global.ini`), locale, theme, `StartupOBS()` (libobs init), creates `OBSBasic` |
| `widgets/OBSBasic*.cpp` and related files (≈21.7k lines in the `.cpp` files) | the main window, split by feature: `_Docks`, `_Hotkeys`, `_Preview`, `_Profiles`, `_SceneCollections`, `_Scenes`, `_SceneItems`, `_Streaming`, `_Recording`, `_ReplayBuffer`, `_StudioMode`, `_Transitions`, `_VirtualCam`, `_SysTray`, `_Updater`, `_YouTube`, `_OutputHandler`, … |
| `components/` | 43 `.cpp` files of reusable widgets: source tree, scene tree, volume meter/control/slider, toolbars, alignment selector, flow layout, multiview, media controls |
| `dialogs/` | 20 `.cpp` files: filters, properties, source select, transform, interaction, advanced audio, log viewer, remux, missing files, about, what's new, YouTube actions, extra browsers, permissions, VCam config, update |
| `docks/` | `OBSDock`, `BrowserDock`, `YouTubeAppDock`, `YouTubeChatDock` |
| `settings/` | `OBSBasicSettings*.cpp` (+ hotkey widgets) |
| `wizards/` | auto-configuration wizard (4 pages: start, video, stream, test) |
| `importers/` | Classic OBS, Streamlabs, OBS Studio, XSplit |
| `oauth/` | Twitch, YouTube, Restream, local listener |
| `plugin-manager/` | installed plugin list, loader paths per OS/Flatpak |
| `updater/` | Windows updater executable (manifest, hashing, patching) |
| `utility/` | output handlers (`SimpleOutput`, `AdvancedOutput`, `BasicOutputHandler`), multitrack video (`MultitrackVideoOutput`, `GoLiveAPI_*`), FFmpeg format/codec tables, platform helpers per OS, crash handlers, theme types, remux worker, thumbnails |
| `api/` | `obs-frontend-api` (99 exported functions, 44 `OBS_FRONTEND_EVENT_*`) implemented by `OBSStudioAPI.cpp` over the abstract `obs_frontend_callbacks` (`api/obs-frontend-internal.hpp`) |
| `forms/` | 34 Qt Designer `.ui` files |
| `data/` | `locale/` (77 `.ini` files), `themes/` (`.obt` themes, `.ovt` variants), images, `striped_line.effect`, `OBSPublicRSAKey.pem`, `license/` |

Startup (verified): `main` → `run_program` → `OBSApp::OBSInit()` (`OBSApp.cpp:1274`): sets the libobs Linux platform (`obs_set_nix_platform` EGL/X11 or Wayland, from `QApplication::platformName()`), calls `StartupOBS(locale, nameStore)` (libobs init), then `new OBSBasic()` and `mainWindow->OBSInit()` (modules load, profile and collection load, UI setup).

## Strategy: strangle the UI too

Do **not** rewrite the main window in one go. With Phase 4 done, the C++ frontend already runs on the Rust core through the unchanged C ABI. From there:

1. **Rust library first, UI shell second.** Port non-visual logic into crates that the C++ frontend calls through `cxx`/C ABI: scene-collection and profile managers (`widgets/OBSBasic_SceneCollections.cpp`, `_Profiles.cpp`, `models/SceneCollection`), importers, config handling, OAuth/token storage, updater logic, remux worker, GoLive API client, FFmpeg codec/format tables, theme parsing. These are testable without a display and remove C++ incrementally.
2. **Then widgets** via `cxx-qt`: reusable components first (`VolumeMeter`, `SourceTree`, `FlowLayout`, toolbars), each exposed as a QObject/QML or widget implemented in Rust and placed in the existing C++ window. Keep `.ui` forms until their last widget has moved.
3. **Last, the application shell** (`OBSApp`, `OBSBasic`) once everything it owns is Rust.

If P0-04 picks a non-Qt toolkit this strategy becomes "parallel app + feature parity checklist" and the intermediate hybrid steps are not available; the timeline then grows (re-estimate in the ADR).

## Contracts you must keep

- **Frontend API** (`obs-frontend-api.h`): third-party plugins and obs-websocket call it. The implementation object `obs_frontend_callbacks` is a C++ abstract class with ~99 virtual methods; the exported C functions delegate to it. Rust must export the same C functions; the 44 event ids (`OBS_FRONTEND_EVENT_*`) must fire in the same order and thread (UI thread) as today — table-test it.
- **File formats**: `global.ini`, `user.ini`, `basic.ini` per profile, `service.json`, `streamEncoder.json`, `recordEncoder.json`, scene-collection JSON, `.bak` files, log and crash folders. Downgrade compatibility is a goal: keep keys and structure.
- **Themes**: `.obt` / `.ovt` syntax (variables, `@define`, `calc`) from `OBSTheme.hpp` / `OBSThemeVariable.hpp`; third-party themes exist.
- **Locale**: same `.ini` files, same keys; `Str("Key")` call sites are what translators depend on.
- **Hotkeys UI** edits libobs hotkey bindings (Phase 4) and persists them in the profile.
- **Command line flags**: 22 of them (`--multi`, `--portable`, `--collection`, `--profile`, `--scene`, `--startstreaming`, …); launchers and scripts depend on them (`--steam` is one of the 22).

## Hard parts (budget extra time)

1. **Preview widget** (`OBSBasicPreview.cpp`, `OBSQTDisplay.cpp`): a native child window that libobs renders into via `obs_display_create` with platform handles (X11 window id/Wayland surface/HWND/NSView). Hit-testing, snapping and transform handles are hundreds of lines of subtle geometry (modifier keys, rotation, crop, bounds types). Port after writing a behaviour table from the C++.
2. **Properties view** (`shared/properties-view` + `widgets`): generates UI from `obs_properties_t` (15+ property kinds with callbacks that mutate other properties). Plugin authors rely on every detail.
3. **Source tree / scene tree**: drag-and-drop reordering with groups, multi-select, in-place rename, accessibility.
4. **Audio mixer meters**: custom painting with peak-hold, magnitude, clip indicators; pixel-compare against reference renders.
5. **Auto-configuration wizard**: runs real test streams with a bandwidth test; hard to test without a mock ingest.
6. **Updater**: Windows self-update replaces the running application; failure modes are severe. Port last; heavy tests; keep rollback.
7. **macOS/Windows/Linux platform code** in `utility/platform-*.{cpp,mm}`, crash handlers, system info, taskbar/tray.
8. **Scripting** (`shared/obs-scripting`): Lua/Python bindings generated by SWIG (`obslua.i`, `obspython.i`) from libobs headers — replace the generator by a Rust one that emits the same names; scripts in the wild call hundreds of functions.

## Pitfalls

- **Thread rules.** Many libobs signals fire on non-UI threads; the C++ code marshals to the UI thread with `QMetaObject::invokeMethod(..., Qt::QueuedConnection)` and `qRegisterMetaType<VoidFunc>`. In Rust keep the same pattern, never touch widgets off-thread.
- **Reference counting in the UI.** Widgets hold `OBSSource`/`OBSScene` smart wrappers (C++ RAII around `obs_source_t*`). Mirror with Rust `Drop` wrappers; leaking a source ref prevents plugin cleanup and shows up in the shutdown leak report.
- **Accessibility** classes exist for custom widgets (`components/Accessible*`, `VolumeAccessibleInterface`); regressions here are easy to miss — keep an accessibility-tree snapshot test.
- **HiDPI/Wayland** quirks (`Qt::AA_DontCreateNativeWidgetSiblings`, platform plugin checks in `OBSInit`) exist for good reasons; do not drop them.

## Suggested order

P6-04 (locale), P6-24/25/26 (collections, profiles, importers), P6-29/30/32 (OAuth, YouTube, updater logic) as libraries → P6-36 (frontend API) → P6-37 (scripting) → components and dialogs (P6-08…18) → settings (P6-19…23) → shell (P6-01…06) → packaging (P6-42…44) → parity checklist and beta (P6-45…48).

## Verification checklist

- [ ] Parity checklist generated from the 34 `.ui` forms and settings pages is 100% ticked.
- [ ] Frontend API contract test passes with obs-websocket (submodule initialised) and a test plugin.
- [ ] 30 UI journeys pass nightly on three OSes.
- [ ] Startup time and binary size within 10% of the C++ build.
