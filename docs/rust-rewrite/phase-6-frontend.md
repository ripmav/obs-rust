# Phase 6: Frontend

Goal: replace the Qt6/C++ application in `frontend/` (≈89k lines) with a Rust application. It runs in parallel with Phase 5 once the Phase 4 ABI exists. The toolkit is fixed by ADR 0004 (default assumption: `cxx-qt` with existing `.ui` forms migrated step by step; replace this in the tasks if another toolkit is chosen).

Source map: `frontend/OBSApp*.cpp`, `obs-main.cpp`, `OBSStudioAPI.*`, `api/`, `components/`, `dialogs/`, `docks/`, `forms/*.ui`, `importer(s)/`, `models/`, `oauth/`, `plugin-manager/`, `settings/`, `updater/`, `utility/`, `widgets/`, `wizards/`, `data/` (themes, locale), `plugins/frontend-tools`, `plugins/obs-websocket` (UI parts), `plugins/aja-output-ui`, `plugins/decklink-output-ui`.

## A. Application shell

- [ ] **P6-01 Launch and CLI** (M): `obs-main.cpp`: arguments (`--portable`, `--multi`, `--profile`, `--collection`, `--scene`, `--startstreaming`, `--startrecording`, `--startreplaybuffer`, `--startvirtualcam`, `--minimize-to-tray`, `--verbose`, `--unfiltered_log`, `--disable-updater`, `--only-bundled-plugins`, `--safe-mode`, ...), single-instance lock, crash handler, log file rotation, Windows/macOS/Linux startup quirks. Use `clap`; parity table test.
- [ ] **P6-02 Application object and config** (M): `OBSApp.cpp/hpp`: global/user config (`global.ini`), profile paths, portable mode, locale selection, theme load, first-run detection.
- [ ] **P6-03 Themes** (L): `OBSApp_Themes.cpp`, `frontend/data/themes/*`: QSS/obs_theme variables, `@define` palette, high-contrast, per-OS defaults, user themes. Same files must still load. Visual snapshot test for each shipped theme.
- [ ] **P6-04 Localisation** (S): load `frontend/data/locale/*.ini` (77 languages), `Str()` equivalents, plural/format rules. Test: every key in `en-US.ini` is used or listed as unused.
- [ ] **P6-05 Logging and crash upload** (M): `OBSLogViewer`, `LogUploadDialog`, crash report format (keep `obs-studio/logs`, `crashes`), analyzer link.
- [ ] **P6-06 Main window skeleton** (L): `OBSBasic*.cpp` (window, menu bar, status bar, docks layout, save/restore geometry, studio mode toggle, always-on-top, multiview). Menus and shortcuts table-tested against a list of actions.

## B. Core UI components

- [ ] **P6-07 Preview widget** (XL; split: display embedding, selection/transform handles, snapping, drag-and-drop of files/URLs, scaling modes, safe-area overlays, spacing helpers, mouse interaction forward to sources, studio-mode dual preview): `OBSBasicPreview.cpp`, `Multiview.cpp`.
- [ ] **P6-08 Scene list** (M): `SceneTree`, drag and reorder, scene collections, transitions per scene, duplicate/rename/remove.
- [ ] **P6-09 Source tree** (L): `SourceTree*`, `VisibilityItem*`, groups, lock, visibility, drag-and-drop reorder, multi-select, context menu, rename, accessibility.
- [ ] **P6-10 Audio mixer** (L): `VolumeControl`, `VolumeMeter`, `VolumeSlider`, `BalanceSlider`, `OBSAdvAudioCtrl`, `OBSBasicAdvAudio`, horizontal/vertical modes, peak hold, clip indicators, `VolumeAccessibleInterface`. Pixel-compare meter rendering against reference.
- [ ] **P6-11 Transitions dock** (M): selecting, duration, properties, quick transitions, T-bar, studio mode transition.
- [ ] **P6-12 Controls dock** (M): start/stop streaming, recording, replay buffer, virtual camera, studio mode, settings, exit; state labels and statuses (reconnecting, paused).
- [ ] **P6-13 Stats dock** (S): `OBSBasicStats`: CPU, memory, FPS, frame drops, bitrate, disk space.
- [ ] **P6-14 Source toolbars** (M): `components/*Toolbar.cpp` (colour, text, image, browser, device, display, window, game, audio, application audio, media controls).
- [ ] **P6-15 Dock framework** (M): `OBSDock`, `BrowserDock`, custom browser docks (`OBSExtraBrowsers`), dock layout save/restore, locked docks, third-party API `obs_frontend_add_dock_by_id`.
- [ ] **P6-16 Properties view** (XL; split: view model from `obs_properties_t`, each property widget kind (bool, int slider/spin, float, text multiline, path file/dir, list, editable list, button, colour/alpha, font, frame rate, group), live update with debounce, visibility rules, reset-to-defaults): `shared/properties-view/` (`OBSPropertiesView`, properties-view.cpp ≈2.4K lines), `dialogs/`: `OBSBasicProperties`, `OBSBasicFilters` (filter list + preview), `OBSBasicSourceSelect`, `OBSBasicTransform`, `OBSBasicInteraction`.
- [ ] **P6-17 Media controls** (M): play/pause/seek, time labels, loop, restart, previous/next.
- [ ] **P6-18 Accessibility pass** (M): screen reader roles, keyboard navigation for every custom widget, contrast checks.

## C. Settings and wizards

- [ ] **P6-19 Settings dialog framework** (M): `settings/`: pages, apply/cancel/OK, change tracking, restart-needed flags, search.
- [ ] **P6-20 Settings pages** (L; one sub-PR each): General, Appearance, Stream, Output (simple/advanced; encoder property views), Audio, Video, Hotkeys, Accessibility, Advanced. Compatibility test: load old `basic.ini` / `service.json` and show identical values.
- [ ] **P6-21 Hotkey editor** (M): `OBSHotkeyWidget`, `OBSHotkeyEdit`, conflict detection, key capture per-OS.
- [ ] **P6-22 Auto-configuration wizard** (L): `wizards/AutoConfig*`: bandwidth test (uses output + server), encoder tests, results, apply. Mock-server test.
- [ ] **P6-23 First-run / what's new / about** (S): `OBSWhatsNew`, `OBSAbout`, `OBSPermissions` (macOS permissions), licence text.

## D. Profiles, collections, importers

- [ ] **P6-24 Scene collection manager** (L): `models/SceneCollection`, JSON load/save, backups, autosave, `.json.bak`, rename/duplicate/delete/import/export, migration of missing files.
- [ ] **P6-25 Profile manager** (M): create, duplicate, rename, remove, export/import, per-profile `basic.ini`, `streamEncoder.json`, `recordEncoder.json`.
- [ ] **P6-26 Importers** (L): `importers/classic.cpp`, `sl.cpp` (Streamlabs), `studio.cpp`, `xsplit.cpp` plus the import dialog `frontend/importer/` (`OBSImporter`, `ImporterModel`, `ImporterEntryPathItemDelegate`) and `forms/OBSImporter.ui` (P6-26.6). Golden: converted JSON vs reference for sample files.
- [ ] **P6-27 Missing files dialog** (S): `OBSMissingFiles`.
- [ ] **P6-28 Remux dialog** (S): `OBSRemux`, uses `ffmpeg` remux with progress.

## E. Services, auth, updater

- [ ] **P6-29 OAuth framework** (L): `oauth/Auth`, `OAuth`, local listener (`AuthListener`), token storage (OS keychain via `keyring` crate), refresh. Twitch (`TwitchAuth`), YouTube (`YoutubeAuth`), Restream (`RestreamAuth`).
- [ ] **P6-30 YouTube integration** (L): `OBSYoutubeActions`, `YouTubeAppDock`, `YouTubeChatDock`, broadcast creation, stream key retrieval, scheduling; mock-API tests.
- [ ] **P6-31 Streaming services UI** (M): stream page: service list, key hiding, server picking, multitrack video config; connects to `rtmp-services`.
- [ ] **P6-32 Updater** (L): `updater/` (Windows updater with manifest, delta, blake2b hash verify, VC redist Authenticode; macOS Sparkle; Linux package managers). Keep RSA per D-23 (ed25519 only if D-07 review demands it) — the `data/OBSPublicRSAKey.pem` RSA check is the What's-New feed's (P6-23.2); test with local manifest server.
- [ ] **P6-33 Plugin manager** (M): `plugin-manager/`: list installed modules, enable/disable, pending restart prompt, Flatpak and Linux paths (`LoaderPaths_*`).
- [ ] **P6-34 Virtual camera config** (S): `OBSBasicVCamConfig`.
- [ ] **P6-35 Idian playground / debug dialogs** (S): `OBSIdianPlayground`, decide to keep or drop.

## F. Frontend API and tools

- [ ] **P6-36 Frontend API** (L): `api/obs-frontend-api.cpp/h`: ~100 functions + events (`OBS_FRONTEND_EVENT_*`) used by plugins; implement in Rust + C ABI; contract test with `obs-websocket`.
- [ ] **P6-37 Scripting host** (L): `frontend-tools/scripts.cpp`: Lua (`mlua`) + Python (`pyo3`) host; implement `obslua`/`obspython` bindings (SWIG-generated today: replace with generated Rust bindings from the same header list), script manager UI, reload, log. Run the existing docs examples as tests.
- [ ] **P6-38 Auto scene switcher** (M): `auto-scene-switcher*.cpp` for Win/Mac/Linux (active window title polling).
- [ ] **P6-39 Output timer and captions** (M): `output-timer.cpp`, `captions*.cpp` (MS speech API on Windows).
- [ ] **P6-40 Decklink/AJA output UIs** (M): `decklink-output-ui`, `aja-output-ui` panels.
- [ ] **P6-41 Tray, notifications, platform bits** (M): system tray, taskbar progress, screen reader, dark-mode detection, macOS menu, Windows jump list, DPI.

## G. Packaging and release

- [ ] **P6-42 Windows installer and portable ZIP** (M): install-tree layout in `additional_install_files/`, CPack portable ZIP (no NSIS/Inno in the C build), code signing, VC runtime handling. Installer scope per D-34 (parity: ZIP only, no new format in 1.0).
- [ ] **P6-43 macOS bundle** (M): notarisation, entitlements, sparkle feed, Info.plist, universal binary.
- [ ] **P6-44 Linux: Flatpak, deb, AppImage?, RPM** (M): `build-aux` Flatpak manifest + CPack (C build ships DEB/TGZ/pkg only); Wayland and X11 tests; FreeBSD pkg best-effort per D-12.
- [ ] **P6-45 Binary size and startup time** (S): compare against C++ build; target startup within 10%.
- [ ] **P6-46 End-to-end UI tests** (L): scripted UI tests (AT-SPI / `accessibility` APIs or screenshot compare) of the top 30 user journeys: add source, record, stream to local RTMP, switch scene, change setting, import collection.
- [ ] **P6-47 Beta programme** (M): publish nightly beta, collect crash and regression reports for 2 releases.
- [ ] **P6-48 Phase 6 review** (S): tick this file.
- [ ] **P6-49 Frontend components sweep** (L) (added): owns every remaining frontend file no dedicated task lists — `frontend/components/*` generic Qt widgets (21 files: FlowFrame/FlowLayout, FocusList, MenuButton, MenuCheckBox, ClickableLabel, DelButton, EditWidget, InfoChip, UIValidation, UrlPushButton, SilentUpdate*…), `shared/qt/*` (45 files: obs-spinbox, idian, icon-label, plain-text-edit, wrappers, slider-ignorewheel, vertical-scroll-area) and `frontend/data/striped_line.effect`. Port per the P0-04 toolkit decision (D-01), view-model test first per the UI bookend, ordered by consumer count. Done when: the 67 files are excluded from the C build and `tools/task-coverage.py --write` shows P6-49 with zero rows.

Exit criteria: Rust application reaches feature parity per `docs/rust-rewrite/parity-checklist.md` (generated from the C++ UI by walking menus and settings), passes the UI test suite, ships as beta on 3 OSes.
