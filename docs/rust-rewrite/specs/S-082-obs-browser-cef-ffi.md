# S-082 obs-browser / CEF FFI contract (CEF kept as library, glue in Rust — D-09)

| | |
|---|---|
| Status | Draft (round 9 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1: `plugins/obs-browser/*` (CEF host, browser source, Qt panels) |
| Target crate | `crates/browser-ffi` (contract + CEF host integration). Per **D-09** (confirmed 2026-11-05) CEF stays the native C++ dependency and the glue is Rust: the host drives CEF through its official C API (`include/capi/`, bindgen — no hand-written C++ bridge) and replicates this contract. |
| Depends on | S-014 (obs_data settings), S-015 (obs sources), S-048 §5.9 (frontend events + API the JS bridge calls) |
| Conformance | planned, none yet |

## 1. Scope

In scope: CEF process startup/configuration (command line, cache, locale, features), the
`browser_source` contract (settings, lifecycle, render paths, audio, input forwarding),
the JS⇄OBS bridge (exposed functions, per-control-level permissions, event dispatch), the
`http://absolute/` local-file scheme, the child-process entry point, and the Qt panel
behaviour (what a panel does, what it persists). Out of scope: CEF/Chromium internals,
the frontend dialogs that host panels.

### Reference files (21, full paths relative to `obs-studio/`)

- `plugins/obs-browser/obs-browser-plugin.cpp` — module load, CEF init, source registration, frontend-event → JS dispatch
- `plugins/obs-browser/obs-browser-source.cpp` — `BrowserSource` lifecycle, render, input, update logic
- `plugins/obs-browser/obs-browser-source.hpp` — `BrowserSource` state, `ControlLevel` enum
- `plugins/obs-browser/browser-app.cpp` — `BrowserApp` CEF app/browser/V8 handlers, command line, JS function bridge
- `plugins/obs-browser/browser-app.hpp` — `BrowserApp` + `MessageObject` (Qt task queue) declarations
- `plugins/obs-browser/browser-client.cpp` — per-source CEF client: render/audio/load/permission/control-level handlers
- `plugins/obs-browser/browser-client.hpp` — `BrowserClient` handler interfaces
- `plugins/obs-browser/browser-dummy-client.cpp` — default client that auto-closes unmanaged windows
- `plugins/obs-browser/browser-scheme.cpp` — `http://absolute/` resource handler (local files)
- `plugins/obs-browser/drm-format.cpp` — CEF color type ↔ DRM/GS format map (Linux shared textures)
- `plugins/obs-browser/panel/browser-panel.cpp` — `QCefWidget` panel (cookies, browser creation, resize, close)
- `plugins/obs-browser/panel/browser-panel-client.cpp` — `QCefBrowserClient` panel handlers (popups, dialogs, key)
- `plugins/obs-browser/panel/browser-panel.hpp` — panel declarations
- `plugins/obs-browser/panel/browser-panel-client.hpp` — `QCefBrowserClient` declaration
- `plugins/obs-browser/obs-browser-page/obs-browser-page-main.cpp` — CEF child-process entry
- `plugins/obs-browser/deps/base64/base64.cpp` — base64 encode/decode helper
- `plugins/obs-browser/deps/ip-string-posix.cpp` — IPv4/IPv6 string detection (POSIX `inet_pton`)
- `plugins/obs-browser/deps/ip-string-windows.cpp` — IPv4/IPv6 string detection (Winsock `inet_pton`)
- `plugins/obs-browser/deps/signal-restore.cpp` — signal backup/restore around `CefInitialize`
- `plugins/obs-browser/deps/wide-string.cpp` — UTF-8 → wide-string conversion
- `plugins/obs-browser/linux-keyboard-helpers.hpp` — X11 keysym → OBS keycode table

## 2. Conventions

RFC 2119 words. "OSR" = off-screen rendering (windowless). "Control level" = the
per-source `ControlLevel` enum: `None=0, ReadObs=1, ReadUser=2, Basic=3, Advanced=4,
All=5` (default `ReadObs`). JS→OBS calls are CEF process messages; OBS→JS events are V8
function calls on every frame of the browser.

The C++ symbol names in this spec are the reference implementation's; the Rust glue (D-09)
drives the equivalent C-API functions in `include/capi/`.

## 3. Interface — the CEF surface the host depends on

### 3.1 Startup (`obs-browser-plugin.cpp`, `browser-app.cpp`)

First `create` starts CEF once (atomic flag): a manager thread (or Qt message object when
`ENABLE_BROWSER_QT_LOOP`) runs `BrowserStart`:
1. Settings: external message pump + no multithreaded loop (Qt build); locales dir next
   to the plugin binary on non-Windows; `locale` = obs locale, `accept_language_list` =
   `"<locale>,en-US,en"` (or `"en-US,en"`); cache path = absolute profile/config path
   (`cache_path` ≤ Chrome 6533, `root_cache_path` > 6533); browser subprocess path = the
   plugin's own binary (non-macOS).
2. `CefExecuteProcess(args, app, nullptr)` on Windows **before** init (child-process
   entry, §3.5). Non-Windows: `BackupSignalHandlers()` / `CefInitialize` /
   `RestoreSignalHandlers()` (CEF overrides 13 POSIX signals). Failure ⇒
   `LOG_ERROR "[obs-browser]: CEF failed to initialize. Exit code: %d"` and the plugin
   degrades (sources never create browsers).
3. `CefRegisterSchemeHandlerFactory("http", "absolute", …)` for local files.

`OnRegisterCustomSchemes`: `http` is a standard, CORS-enabled scheme.
`OnContextInitialized`: installs the `BrowserDummyClient` as default client (any
unmanaged top-level window is auto-closed) and sets ~20 request-context preferences
(autofill/signin/translate/printing/etc. disabled; `extensions.block_external_extensions`
+ `extensions.disabled` enabled).
`OnBeforeCommandLineProcessing`: appends `disable-gpu-compositing` (when no shared-texture
support and no `--enable-gpu`), a fixed `disable-features` list (+`do-not-de-elevate` on
Windows, merged with any existing), `disable-blink-features += DocumentPictureInPictureAPI`,
`autoplay-policy=no-user-gesture-required`, `disable-extensions`,
`hide-crash-restore-bubble`, `use-mock-keychain` (macOS), `ozone-platform=wayland|x11`.
`OnBeforeChildProcessLaunch`: Windows adds `parent_pid=<pid>`.

### 3.2 `browser_source` contract

Source id `browser_source`, flags `OBS_SOURCE_VIDEO|AUDIO|CUSTOM_DRAW|INTERACTION|…`,
icon browser. Settings (defaults in §4 R04): `url`, `local_file`, `is_local_file`,
`width`, `height`, `fps`, `fps_custom`, `shutdown` (shutdown when invisible),
`restart_when_active`, `css`, `reroute_audio`, `webpage_control_level`.

**Update semantics** (`obs-browser-source.cpp`): local files are URI-encoded (Windows:
keep the drive `:` before the first `/`; always unescape `%5C`/`%2F`) and routed as
`http://absolute/<path>` so local pages can load other local files. An update is a
no-op except size ⇒ `OnAutoResize` + `WasResized` + `Invalidate(PET_VIEW)`; any other
change ⇒ `DestroyBrowser` + `DestroyTextures` + recreate (when showing or
`!shutdown_on_invisible`).

**Render paths**: (a) CPU — `OnPaint(PET_VIEW)` copies BGRA into a `GS_DYNAMIC` texture
(recreate on size change); (b) shared — `OnAcceleratedPaint`/`OnAcceleratedPaint2` open
the handle (`gs_texture_open_shared` / NT shared / IOSurface / dmabuf with the
`drm-format.cpp` map: `RGBA_8888↔ABGR8888`, `BGRA_8888↔ARGB8888`). Render draws with
sRGB, `ONE/INVSRCALPHA` blend, `Draw` or `DrawSrgbDecompress` (extra linear texture copy
when texcoords not centered), optional V-flip on old-CEF hwaccel, then signals external
begin-frame or pumps the CEF loop.

**Audio (reroute)**: `obs_source_set_audio_active(reroute_audio)`; when rerouting, the
page is muted in CEF (`SetAudioMuted(true)` on load) and `OnAudioStreamPacket` emits
`obs_source_audio` (float planar, layout from CEF map, rate = obs audio rate via
`GetAudioParameters` — 1024 frames per buffer).

### 3.3 JS ⇄ OBS bridge

**OBS → JS** (`BrowserApp::OnContextCreated`): global `obsstudio` object with
`pluginVersion` + 19 functions: `getControlLevel, getCurrentScene, getStatus,
startRecording, stopRecording, startStreaming, stopStreaming, pauseRecording,
unpauseRecording, startReplayBuffer, stopReplayBuffer, saveReplayBuffer,
startVirtualcam, stopVirtualcam, getScenes, setCurrentScene, getTransitions,
getCurrentTransition, setCurrentTransition`.

**JS → OBS**: V8 call ⇒ process message to the browser process ⇒
`BrowserClient::OnProcessMessageReceived` dispatches by control level (fall-through:
All ⊃ Advanced ⊃ Basic ⊃ ReadUser ⊃ ReadObs ⊃ None): All: start/stop recording &
streaming, pause/unpause recording, start/stop virtualcam; Advanced: start/stop replay
buffer, `setCurrentScene`/`setCurrentTransition` (with name-existence warnings); Basic:
`saveReplayBuffer`; ReadUser: `getScenes`/`getCurrentScene`/`getTransitions`/
`getCurrentTransition`; ReadObs: `getStatus` {recording, streaming, recordingPaused,
replaybuffer, virtualcam}; None: `getControlLevel`. Replies: `executeCallback` message
(callback id + JSON) to `PID_RENDERER`.

**Frontend events → JS** (`obs-browser-plugin.cpp`): streaming starting/started/
stopping/stopped, recording starting/started/paused/unpaused/stopping/stopped, replay
buffer starting/started/saved/stopping/stopped, virtualcam started/stopped, scene
changed {name,width,height}, scene list changed [names], transition changed {name},
transition list changed [names], exit. Visibility/active changes dispatch
`onVisibilityChange`/`onActiveChange`.

### 3.4 Local-file scheme (`browser-scheme.cpp`)

`http://absolute/<path>`: only honored when the main frame's origin is
`http://absolute/`; path URI-decoded twice (spaces, then special chars); extension
lowercased, `woff2`→`woff`; first path segment that parses as an IPv4 **or** IPv6
address (via `deps/ip-string-*`) rejects the request (path-traversal safeguard); file
served through `CefStreamReader::CreateForFile` with `CefGetMimeType` (fallback
`application/octet-stream`).

### 3.5 Child process (`obs-browser-page-main.cpp`)

Windows: exports `NvOptimusEnablement=1` + `AmdPowerXpressRequestHighPerformance=1`
(GPU-hint for hybrid laptops), sets `ProcessPowerThrottling
IGNORE_TIMER_RESOLUTION`, and with `parent_pid` spawns a shutdown-watch thread (parent
exit ⇒ `WM_QUIT`; if the main thread doesn't signal shutdown within 5 s ⇒
`TerminateProcess(-1)`). macOS: loads the CEF framework via `CefScopedLibraryLoader::
LoadInHelper`. Both: `CefExecuteProcess(args, new BrowserApp, nullptr)`.

## 4. Behaviour (numbered rules)

| # | Rule |
|---|---|
| S-082-R01 | `obs-browser-plugin.cpp`: CEF init runs once per process on a dedicated thread (or Qt loop); hwaccel is decided at load (Windows: adapter blacklist `Intel`/`Microsoft`/`Radeon HD 8850M`/`Radeon HD 7660` when ≥2 adapters or non-Intel default; Linux: all DRM formats must be supported; macOS/Win: shared-texture availability). Unload: quit the CEF message loop, join the thread. |
| S-082-R02 | `obs-browser-source.cpp`: `Tick` checks `cef_started_event` **non-blockingly** (`os_event_try`) and returns early until CEF has started; browser creation is queued as a CEF UI task (`CreateBrowser` → `QueueCEFTask` → `CreateBrowserSync`); windowless rendering always; in shared-texture builds with external begin-frame: `external_begin_frame_enabled` + `windowless_frame_rate = 0` when not custom-fps, else `windowless_frame_rate = fps` (shared-texture builds without external begin-frame use `fps_custom ? fps : obs fps`; non-shared builds `= fps`); default font sizes 16 (`default_font_size` + `default_fixed_font_size`). |
| S-082-R03 | `obs-browser-source.cpp`: mouse click/move/wheel, key click, focus are forwarded to `CefBrowserHost` (Linux keys go through `linux-keyboard-helpers.hpp` X11 keysym→keycode mapping); `show`/`hide` dispatch visibility JS; `activate` refreshes the page when `restart_when_active`. |
| S-082-R04 | `obs-browser-plugin.cpp` defaults: url `https://obsproject.com/browser-source`, 800×600, fps 30, `fps_custom` = (shared textures ? false : true), shutdown false, restart_when_active false, control level `ReadObs`, css = transparent-body default, reroute_audio false. `is_local_file` toggle swaps the visible `url`/`local_file` properties; a missing local file is re-located via the frontend missing-file callback and written back to settings. |
| S-082-R05 | `browser-client.cpp`: popups blocked (`OnBeforePopup` true); context menu cleared; tooltips forwarded to a Qt tooltip; render-process crash ⇒ `LOG_ERROR "[obs-browser: '<name>'] Webpage has crashed unexpectedly! Reason: '<reason>'"`; permission prompts: loopback-network (localhost) implicitly accepted, everything else denied; load start re-mutes when rerouting; load end (main frame) injects the user `css`; load error logged (ERR_ABORTED ignored on new CEF). |
| S-082-R06 | `browser-app.cpp`: JS function bridge exposes exactly the 19 names in §3.3 and nothing else; `getControlLevel` returns the int level. |
| S-082-R07 | `browser-dummy-client.cpp`: the app-level default client returns "handled" for chrome commands/menu/toolbar (all hidden) and **closes any browser created without a client** in `OnAfterCreated` — the host MUST keep an equivalent safety net so unmanaged windows cannot appear. |
| S-082-R08 | `browser-scheme.cpp`: rules of §3.4 are exact (origin check, double URI decode, woff2 alias, IP segment rejection, mime fallback). |
| S-082-R09 | `panel/browser-panel.cpp`: `QCefWidget` creates its own CEF browser (windowed, `allowAllPopups` option, injected `script`); cookie manager with a storage path and optional session persistence; resize resizes the CEF view; close destroys the browser. |
| S-082-R10 | `panel/browser-panel-client.cpp`: panel popups: `allowAllPopups` ⇒ the popup is allowed (no client attached; on Windows it is parented to the widget's HWND); else the URL is checked against the panel's popup whitelist (case-insensitive) — whitelisted URLs are allowed (same parenting), any other URL is opened in the user's default browser (`QDesktopServices::openUrl`) and blocked in CEF. JS dialogs (alert/confirm/prompt) are shown as Qt dialogs (`QMessageBox`/`QInputDialog`); focus handled via `OnSetFocus`; the context menu is customised via the CEF menu model on all platforms (adds reload-no-cache + zoom in/reset/out, removes print/view-source). |
| S-082-R11 | `deps/wide-string.cpp` / `deps/base64/base64.cpp` / `deps/signal-restore.cpp` / `deps/ip-string-*.cpp`: utilities as described in §1 — UTF-8→wide via `os_utf8_to_wcs`, base64 helper, signal backup/restore of 13 signals, `inet_pton`-based IP detection. |
| S-082-R12 | `drm-format.cpp`: only RGBA_8888 and BGRA_8888 are accepted; `obs_cef_all_drm_formats_supported` requires **both** in the GPU's dmabuf list; unknown CEF color type ⇒ error log + `DRM_FORMAT_INVALID`. |
| S-082-R13 | `obs-browser-source.hpp`: `ControlLevel` values 0..5 as in §2; `BrowserSource` keeps one primary + one extra texture and a `destroying` flag for tear-down ordering. |

## 5. Concurrency

- CEF UI thread owns all `CefBrowser*` calls: host code MUST marshal via
  `CefPostTask(TID_UI)` (the `QueueCEFTask`/`ExecuteOnBrowser` pattern); with the Qt
  loop, tasks go through the Qt event queue (≤10 ms per pump).
- Render handler callbacks run on the CEF UI thread; texture creation happens inside
  `obs_enter_graphics`.
- Audio callbacks run on the CEF audio thread and emit directly into the source.
- The V8 bridge (`Execute`) runs on the CEF UI thread; the reply is a process message.

## 6. Reference defects and quirks

- **Q-1**: hwaccel decision on Windows mixes `adapterCount >= 2 || !is_intel` as an outer
  condition before the blacklist scan — a non-Intel default with one adapter skips the
  blacklist entirely. Replicate exactly.
- **Q-2**: `browser-scheme.cpp` decodes the URI twice (spaces, then URL-special chars) —
  order matters and is intentional.
- **Q-3**: `fps_custom` default differs by build (shared-texture builds default to false,
  i.e. "follow obs fps").
- **Q-4**: the JS `obsstudio` object is installed in **every** frame's context
  (`OnContextCreated`), not just the main frame.

## 7. Open questions

1. Exact CEF/Chromium build the rewrite pins (the C code branches on
   `CHROME_VERSION_BUILD` in many places; this spec records no version numbers).
2. `ENABLE_BROWSER_QT_LOOP` vs dedicated-thread build for the Rust host — the contract
   above is identical; only the message pump differs.
3. Resolved (D-09, 2026-11-05): the Qt panels (`QCefWidget`) become Rust (Phase 6, cxx-qt
   per D-01) driving CEF via the C API; R09/R10 stand as behaviour the Rust panel must keep.

Round 10 second-author check (2026-11-05): corrected R02 (was "Tick blocks until
`cef_started_event`"; source uses non-blocking `os_event_try`; added the exact
`windowless_frame_rate` branches per build configuration) and R10 (was "attach the same
client to the popup; else block"; source: allowed popup gets no client, else-branch is a
URL whitelist with fall-through to the user's default browser, and the context-menu
customisation is cross-platform CEF menu-model editing, not a Windows-only native menu).
R03/R04/R05/R07/R11/R12 re-derived and confirmed.
