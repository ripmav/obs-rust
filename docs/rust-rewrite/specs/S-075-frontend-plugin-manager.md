# S-075 Frontend: plugin manager, crash handler, auto-update & system info

| | |
|---|---|
| Status | Draft (round 9 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1 frontend: `frontend/plugin-manager/*`, `frontend/utility/` (`CrashHandler*`, `platform-*`, `system-info-*`, `win-dll-blocklist`, `MacUpdateThread`, `OBSSparkle`, `OBSUpdateDelegate`, `NativeEventFilter*`, `crypto-helpers-*`) |
| Target crate | `crates/frontend` (plugin manager state + `modules.json`, crash handler, update threads, system-info collectors, platform utils, DLL blocklist; Qt UI at behaviour level only) |
| Depends on | S-048 (app config, `Locations` incl. `userPluginManagerSettingsLocation`), S-007 (config files), S-074 (shared `FetchAndVerifyFile` + embedded update public key), libobs core (module-load API: `obs_load_plugins`, `obs_module_failure_info`, `obs_module_add_*`) |
| Conformance | planned, none yet |

## 1. Scope

In scope: the plugin lifecycle around startup (core module load, per-platform third-party
plugin discovery paths, the `modules.json` cache, plugin-manager window behaviour), the
crash sentinel + crash-log detection + upload, the macOS update-branch fetch and Sparkle
glue, the per-platform system-info collectors, the platform utility layer, the Windows
DLL blocklist, the session-end event filter, and the signature verification helpers
(ed25519 per D-23, confirmed 2026-11-06). Out of
scope: libobs-side module loading internals, the Sparkle framework itself, the GoLive API
protocol (only its post-data assembly), Qt/Idian widget implementation details. Per
D-12 (confirmed 2026-11-05) BSD is out of scope: the Rust port implements Linux/Windows/
macOS only (D-04) — the BSD rows/columns in this spec document the C reference and are not
ported.

### Reference files (26, full paths relative to `obs-studio/`)

- `frontend/plugin-manager/PluginManager.cpp` — modules.json cache, load sequence, state
- `frontend/plugin-manager/PluginModuleLoader.hpp` — `obs_load_plugins` wrapper + path env-var constants
- `frontend/plugin-manager/PluginModuleLoader_Windows.cpp` — Windows discovery paths
- `frontend/plugin-manager/PluginModuleLoader_Linux.cpp` — Linux discovery paths
- `frontend/plugin-manager/PluginModuleLoader_BSD.cpp` — FreeBSD/DragonFly discovery paths (not ported, D-12)
- `frontend/plugin-manager/PluginModuleLoader_Flatpak.cpp` — Flatpak discovery paths
- `frontend/plugin-manager/PluginManagerWindow.cpp` — manager dialog: sections, categorisation, toggles
- `frontend/plugin-manager/InstalledPluginRow.cpp` — one module row: chips, toggle, trash
- `frontend/utility/CrashHandler.cpp` — sentinel, crash-log state, upload
- `frontend/utility/CrashHandler_Windows.cpp` — Windows crash-log discovery
- `frontend/utility/CrashHandler_Linux.cpp` — Linux crash-log discovery (stub)
- `frontend/utility/CrashHandler_FreeBSD.cpp` — FreeBSD crash-log discovery (stub; not ported, D-12)
- `frontend/utility/MacUpdateThread.cpp` — update-branch fetch + validation
- `frontend/utility/OBSSparkle.mm` — Sparkle controller wrapper
- `frontend/utility/OBSUpdateDelegate.mm` — Sparkle delegate: channels, feed URL, menu state
- `frontend/utility/system-info-posix.cpp` — Linux/BSD capability collector
- `frontend/utility/system-info-windows.cpp` — Windows capability collector
- `frontend/utility/system-info-macos.mm` — macOS (Apple Silicon) capability collector
- `frontend/utility/platform-windows.cpp` — Windows platform utilities
- `frontend/utility/platform-osx.mm` — macOS platform utilities
- `frontend/utility/platform-x11.cpp` — X11/BSD platform utilities
- `frontend/utility/win-dll-blocklist.c` — NtMapViewOfSection blocklist hook
- `frontend/utility/NativeEventFilter.cpp` — native event filter (stub)
- `frontend/utility/NativeEventFilter_Windows.cpp` — WM_QUERYENDSESSION/WM_ENDSESSION handling
- `frontend/utility/crypto-helpers-mac.mm` — RSA verify (Security.framework)
- `frontend/utility/crypto-helpers-mbedtls.cpp` — RSA verify (mbedTLS)

## 2. Conventions

RFC 2119 words. "App config" = per-user obs-studio config (S-007/S-048). `State` ∈
{`Failure`, `PartialFailure`, `Success`}: a path scan with no failures is `Success`, with
failures `PartialFailure`; an absent/unscannable path contributes nothing. "Legacy
plugin" = pre-manifest module from the flat DLL/SO legacy path set.

## 3. Plugin manager core

### 3.1 Module cache (`modules.json`)

| | |
|---|---|
| Location | `<Locations.userPluginManagerSettingsLocation>/obs-studio/plugin_manager/modules.json` |
| Content | JSON array of objects: `display_name`, `module_name`, `id`, `version`, `enabled`, `sources[]`, `outputs[]`, `encoders[]`, `services[]` |
| `enabledAtLaunch` | loaded from the **same `"enabled"` JSON key** as `enabled` (no separate persisted field — see Q-3) |

### 3.2 Load sequence (startup)

| Step | Action |
|---|---|
| 1 `preLoad` | read disabled-module list into `disableModules_` (modules.json) |
| 2 | `obs_load_core_modules()` — on failure **throw** the string `"Failed to load core OBS modules. OBS cannot run without these modules. Please try reinstalling OBS."` |
| 3 | `loadPlugins(portableMode)` — third-party plugins (full mode only) |
| 4 | `loadLegacyPlugins(portableMode)` — legacy plugins (full mode only) |
| 5 | `obs_log_loaded_modules()` |
| 6 | `obs_post_load_modules()` |
| 7 `postLoad` | enumerate loaded modules, register their contributed types, save `modules.json`, then `linkUnloadedModules_`: for every known-but-disabled module register its `sources/outputs/encoders/services` via `obs_module_add_*` so settings referencing disabled plugins still resolve |

`loadPluginsByInfo` (PluginModuleLoader.hpp): builds
`obs_runtime_module_info{.binary, .data, .type, .name = nullptr}`, calls
`obs_load_plugins(&info, &failure)`, appends every non-empty `failure.failed_modules[i]`
to the shared `failedModules_` list, frees the failure info, returns the failure count.

## 4. Third-party plugin discovery paths

Per path: binary dir pattern + data dir pattern (`%module%` = plugin name, `.dll`/`.so`
appended by libobs). Env vars: `OBS_PLUGINS_PATH`, `OBS_LEGACY_PLUGINS_PATH`,
`OBS_LEGACY_PLUGINS_DATA_PATH`. "config" = user config dir.

| Platform | Modern (`loadPlugins`) | Legacy (`loadLegacyPlugins`) |
|---|---|---|
| **Windows** | `$OBS_PLUGINS_PATH/%module%` (+`data/`); then ProgramData `obs-studio/plugins/%module%` (+`data/`) in installed mode, `../../plugins/%module%` (+`data/`) in portable mode (relative to app dir) | env pair `OBS_LEGACY_PLUGINS_PATH` (flat) + `OBS_LEGACY_PLUGINS_DATA_PATH/%module%`/`data` (both set); `../../obs-plugins/64bit` (flat) + `../../data/obs-plugins/%module%` **always** (Q-2); ProgramData `obs-studio/plugins/%module%/bin/64bit` + `/data` in installed mode |
| **Linux** | `$OBS_PLUGINS_PATH/%module%` (+`data/`); system `/usr/<lib>/obs-modules/plugins` + `/usr/share/obs/obs-modules/plugins/%module%`; `$XDG_DATA_HOME` (fallback `$HOME/.local/share`) `obs-studio/plugins/%module%` (+`data/`), non-portable only | env pair (both set, data +`/%module%`); system `/usr/<lib>/obs-plugins` (flat) + `/usr/share/obs/obs-plugins/%module%`; `$XDG_CONFIG_HOME` (fallback `$HOME/.config`) `obs-studio/plugins/%module%/bin/64bit` + `/data`, non-portable only |
| **BSD** | `$OBS_PLUGINS_PATH/%module%`; system root (compile-time paths, if non-empty); config path `obs-studio/plugins/%module%`, non-portable only | env pair (both set); system root `<lib>/obs-plugins` (flat) + `<data>/obs/obs-plugins/%module%`; config `…/plugins/%module%/bin/64bit` + `/data`, non-portable only |
| **Flatpak** | `/app/plugins/<lib>/obs-modules/plugins` + `/app/plugins/<data>/obs/obs-modules/plugins/%module%`; `$XDG_DATA_HOME` `obs-studio/plugins/%module%` (set-only, Q-1) | `/app/plugins/<lib>/obs-plugins` + `/app/plugins/<data>/obs/obs-plugins/%module%`; `$XDG_CONFIG_HOME` `…/plugins/%module%/bin/64bit` + `/data` (set-only) |

Env-var-based paths that are unset contribute no failures (skipped = OK), except the
Flatpak legacy XDG-config path which leaves the result `Failure` when unset (Q-6).
`loadPlugins`/`loadLegacyPlugins` return `Success` only if every scanned path returned
zero failures, else `PartialFailure`. Portable mode skips all user-config/XDG dirs.

## 5. Plugin manager window

| | |
|---|---|
| Sidebar | **Discover** (disabled, tooltip "ComingSoon"), **Manage** (active page), **Updates** (disabled, "ComingSoon") — **33.0.1 has no in-app install/update mechanism**; installing a plugin = placing files in the §4 paths (see R09) |
| Safe mode | banner on the Manage page; every entry forced to category *Installed*; every row shows a muted "Disabled" chip; no toggles |
| Entry categories | **Installed** (order 0), **Error** (1), **Missing** (2); ties broken by display name, case-insensitive |

Categorisation per cached `modules.json` entry (name = `module_name`):

| `obs_get_module(name)` | `obs_get_disabled_module(name)` | in `failedModules_` | Category |
|---|---|---|---|
| loaded | — | (ignored) | Installed |
| not loaded | loaded (disabled) | yes | **Error** (entry removed from `failedModules_` to avoid a duplicate dummy) |
| not loaded | loaded (disabled) | no | Installed (muted name, "Disabled" chip) |
| not loaded | not loaded | — | **Missing** |

Remaining `failedModules_` entries (never loaded successfully, no cache entry) become
**dummy Error rows**: `enabled = enabledAtLaunch = false`, `hasLoadedBefore = false`.

Row content (`InstalledPluginRow.cpp`): name (+ "Disabled"-muted when
`!enabledAtLaunch`), version; chip priority **safe mode → Disabled; else Error →
warning chip; else Missing → danger chip; else** legacy modules get a "Legacy" chip plus
a link line to `https://obsproject.com/go/legacy-plugin-locations`, and
`!enabledAtLaunch` adds a "Disabled" chip. A toggle switch exists **only if
`hasLoadedBefore`** (initial state = `enabled`). Missing rows get a trash button.

Toggle behaviour: sets `enabled` in the in-memory `modules_` list and the row entry; the
"restart required" label is visible while `isEnabledPluginsChanged()`:
`false` in safe mode, else true when any *Installed*-category entry has
`enabledAtLaunch != enabled`. The trash button on a Missing row removes that entry from
`modules_` (i.e. from `modules.json` on save) — **no files are deleted**. On close, a
changed state triggers the restart prompt (window → app).

## 6. Crash handler

### 6.1 Sentinel

- Path: app config dir + `obs-studio/.sentinel/run_<uuid>` (uuid = app-launch UUID, braces
  stripped). Created at construction if absent; directory created if missing.
- Startup scan: any other `run_*` file (not this run's) ⇒ **unclean shutdown** flag.
- Clean exit removes the `run_*` sentinel file(s).
- **Disabled entirely in debug builds** (`isSentinelEnabled = !NDEBUG`); the handler is
  then inactive and never reports unclean shutdown.

### 6.2 Crash-log discovery (`findLastCrashLog` / `getCrashLogDirectory`)

| Platform | Crash log directory | "Last crash log" |
|---|---|---|
| Windows | app config + `obs-studio/crashes` | newest file whose name starts with `"Crash "` by last-write time |
| macOS | `~/Library/Logs/DiagnosticReports` | newest `OBS*.ips` by creation date (in `CrashHandler_MacOS.mm` — Q-5) |
| Linux / FreeBSD | **not implemented** (empty path) | none |

### 6.3 New-crash detection & upload

- `updateLocalCrashLogState`: read app-config section **`CrashHandler`** keys
  `last_crash_log_file`, `last_crash_log_time` (unix seconds), `last_crash_log_url`;
  compare against `findLastCrashLog()`. Different file ⇒ *Updated* (URL cleared);
  neither local nor remembered file ⇒ *NotAvailable*; same ⇒ *NotUpdated*.
- `hasNewCrashLog` = *Updated* **or** no remembered URL.
- Upload: read the file, build the body — **Windows** prepends
  `"OBS <version> crash file uploaded at <YYYY-MM-DD, hh:mm:ss>\n\n"`
  (`std::put_time "%Y-%m-%d, %X"`); **macOS** sends the raw content; other platforms get
  an **empty** body (Q-4). POST as `text/plain` via a detached `RemoteTextThread` to
  `https://obsproject.com/logs/upload`.
- Response JSON `{"url": "…"}` is parsed; on success persist
  `last_crash_log_file` / `last_crash_log_time` / `last_crash_log_url` and
  `config_save_safe(appConfig, "tmp", nullptr)`. No new log + known URL ⇒ just re-emit
  the stored URL (no second upload).

## 7. Auto-update (macOS)

| | |
|---|---|
| Branch list | fetch + ed25519-verify `https://obsproject.com/update_studio/branches.json` via the shared `FetchAndVerifyFile` helper (S-074) (D-23 — the C reference is RSA, R25) → store under data dir `obs-studio/updates/branches.json` → `OBSApp::SetBranchData` |
| Branch selection | app config **`General` / `UpdateBranch`**; unset ⇒ `stable` (silent); branch present but disabled ⇒ silent fallback to `stable`; branch absent from fetched list ⇒ config reset to `stable` **and** info dialog (`Updater.BranchNotFound.*`) |
| Result | `Result(branch, manualUpdate)` signal |
| Sparkle | `SPUStandardUpdaterController` with `OBSUpdateDelegate`: allowed channel = {branch}; feed URL = `https://obsproject.com/osx_update/updates_arm64_v2.xml` when `os_get_emulation_status()` is true, else the default appcast (delegate feed URL nil); KVO on `canCheckForUpdates` enables/disables the Check-for-Updates menu action; `checkForUpdates(manual)`: manual ⇒ `checkForUpdates:`, else `checkForUpdatesInBackground` |

## 8. System info

`system_info(GoLiveApi::Capabilities&)` fills: `cpu{name, physical_cores, logical_cores,
speed (MHz)}`, `memory{total, free}`, `gpu[]{model, vendor_id, device_id,
driver_version, dedicated_video_memory, shared_system_memory}`, `system{name, version,
release, build, revision, bits, arm, armEmulation}`, `gaming_features` (Windows only).
In 33.0.1 the only consumer is the GoLive post-data builder (`GoLiveAPI_PostData.cpp`,
out of file set: client `"obs-studio"` + version + h264/hevc/av1 codec list).

| Platform | GPU | CPU | System |
|---|---|---|---|
| Windows | DXGI `EnumAdapters1`, skipping Microsoft Basic Render (vendor `0x1414`, device `0x8c`); driver version from `CheckInterfaceSupport(IDXGIDevice)` formatted `aa.bb.cccc.dddddd` | registry `HKLM\HARDWARE\DESCRIPTION\System\CentralProcessor\0` (`ProcessorNameString`, `~MHz`) | `GetWindowsVersion()` + registry `SOFTWARE\Microsoft\Windows NT\CurrentVersion` (major/minor, `UBR`, `CurrentBuildNumber`; `DisplayVersion` if build > 19041 else `ReleaseId`); `bits`/`arm` from platform queries, `armEmulation` from `os_get_emulation_status()`; gaming features only on Win10+ (`(major<<8|minor) >= 0xA00`): Game Bar, Game DVR (allowed/enabled/bg), Game Mode, HAGS via 6 registry lookups (Game Mode has a backup value name; HAGS treats missing-as-false, disabled-value 1) |
| Linux/BSD | DRM card enumeration (sysfs), sorted by match strength; **first** card takes the OpenGL `gs_driver_version` + GL-reported VRAM; other cards: driver `"Unknown"`, per-card sysfs VRAM (amdgpu only) | name + frequency from proc/sysfs helpers (missing ⇒ `"Unknown"` / 0) | distro from `/etc/os-release` helpers (fallback `"Linux-based distribution"`/`"unknown"`); `uname`: `bits` = machine contains `"64"`, `arm` = contains `"aarch"`, `version` = `"sysname release version"` truncated to 128 chars; gaming features unused |
| macOS | **Apple Silicon only**: the SoC — model = CPU name, vendor `0x106b`, device `0` | `sysctlbyname("machdep.cpu.brand_string")` (missing ⇒ nullopt); cores from `os_get_*` | name `"macOS"`, bits 64, version from `NSProcessInfo`; release mapped from major: 11 Big Sur, 12 Monterey, 13 Ventura, 14 Sonoma, 15 Sequoia, else `"unknown"`; `arm = true`, `armEmulation = false`. Non-arm64 builds: `system_info` is a **no-op** |

## 9. Platform utilities

| Function | Windows | macOS | X11/BSD |
|---|---|---|---|
| `CheckIfAlreadyRunning` | named mutex `"OBSStudioCore"`, or `"OBSStudioPortable"` + absolute config path (portable); non-alphanumerics → `_`; returns a handle to release on exit | running apps with same bundle id > 1 | Linux: bind abstract unix dgram socket `@/com/obsproject <pid> <version>` (bind fail ⇒ running), then count `@/com/obsproject` entries in `/proc/net/unix` (≠1 ⇒ running). BSD: `O_EXLOCK` on `/tmp/obs-studio.lock.<euid>` + procstat scan for a thread named `"OBS runonce"` (which it then starts itself) |
| `GetDataFilePath` | `data/obs-studio/<data>` (cwd-relative) then `$OBS_DATA_PATH/obs-studio/<data>` | bundle `Contents/Resources/<data>` | `$OBS_DATA_PATH`, exe-relative `../<OBS_DATA_PATH>/obs-studio/`, compile-time `OBS_DATA_PATH`, install prefix — first existing wins |
| `GetDefaultVideoSavePath` | `CSIDL_MYVIDEO` | Movies dir (`NSMoviesDirectory`, create), fallback `$HOME` | `$HOME` |
| `GetPreferredLocales` | user-preferred locale list | `NSLocale.preferredLanguages` mapped to known locales (first full prefix match, else first 2-letter match; dedup, order kept) | `setlocale(LC_MESSAGES)` with `.`→`-`; exact known-locale match wins, else all 2-letter prefix matches |
| `HighContrastEnabled` | platform query (see reference) | platform query (see reference) | XDG desktop portal DBus: `org.freedesktop.portal.Settings` version < 2 ⇒ false; else `ReadOne "org.freedesktop.appearance" "contrast"` != 0 |
| Other | `SetProcessPriority`, `SetWin32DropStyle`, `DisableAudioDucking` (app audio session `SetDuckOthers`), `IsRunningOnWine` (probe `ntdll!wine_get_version`, warn), taskbar overlay via `ITaskbarList4` | `EnableOSXVSync`, `EnableOSXDockIcon` (NSStatusItem), `CheckPermissionWithPrompt` (audio/video: `AVCaptureDevice` request, blocking event; screen: `CGPreflight/RequestScreenCaptureAccess`; input: `CGPreflight/RequestListenEventAccess`), `OpenMacOSPrivacyPreferences` (`x-apple.systempreferences:…Privacy_<tab>`), `SetMacOSDarkMode`, `InstallNSThreadLocks`, `InstallNSApplicationSubclass` | `IsAlwaysOnTop`/`SetAlwaysOnTop` via `Qt::WindowStaysOnTopHint`; `SetDisplayAffinitySupported` = false; taskbar overlay no-ops |

## 10. Windows DLL blocklist

`install_dll_blocklist_hook` (called on Windows startup) hooks `NtMapViewOfSection`
(resolving `NtQuerySection`/`NtUnmapViewOfSection` for image inspection). For each map
request it lower-cases the full image path and matches it **by suffix** against
`blocked_modules[]` (~28 entries, e.g. overlay/hook/capture-filter DLLs of third-party
software known to crash or deadlock OBS); matches with a timestamp threshold compare the
image's PE timestamp (`TS_IGNORE`, `TS_EQUAL`, `TS_LESS_THAN`, `TS_GREATER_THAN`,
`TS_ALLOW_ONLY_THIS`). Blocked maps are denied and counted; `log_blocked_dlls` reports
them at exit.

## 11. Session event filter & crypto

- **NativeEventFilter (Windows)**: on `windows_generic_MSG` — `WM_QUERYENDSESSION`:
  `lParam & ENDSESSION_CRITICAL` ⇒ `saveAll()` and let the session end; else if the app
  would prompt for close ⇒ reject (`*result = FALSE`) and queue a normal `close()` (so
  prompts can run); otherwise unhandled. `WM_ENDSESSION` with `wParam == TRUE` ⇒
  `closeWindow()` immediately, no prompts. **Other platforms**: stub returns `false`.
- **`VerifySignature(pubKey, pubKeyLen, buf, len, sig, sigLen)`**: RSA PKCS#1 v1.5 over
  SHA-512 of `buf`. macOS: Security.framework `SecItemImport` +
  `SecKeyVerifySignature(kSecKeyAlgorithmRSASignatureMessagePKCS1v15SHA512)`.
  mbedTLS: `mbedtls_pk_parse_public_key` (PEM; passes `pubKeyLen + 1`, i.e. the key must
  be NUL-terminated), SHA-512 hash, `mbedtls_pk_verify` (64-byte hash). Used by the
  shared download verifier (S-074) for update/whats-new files.

## 12. Behaviour (numbered rules)

| Rule | Statement |
|---|---|
| S-075-R01 | `modules.json` lives at `<userPluginManagerSettingsLocation>/obs-studio/plugin_manager/modules.json` with fields `display_name`, `module_name`, `id`, `version`, `enabled`, `sources`, `outputs`, `encoders`, `services`; `enabledAtLaunch` is initialised from the same `"enabled"` key. |
| S-075-R02 | Startup order is fixed: preLoad (disable list) → `obs_load_core_modules()` → (full mode) `loadPlugins` + `loadLegacyPlugins` → `obs_log_loaded_modules()` → `obs_post_load_modules()` → postLoad (enumerate, add types, save cache, `linkUnloadedModules_` via `obs_module_add_*`). |
| S-075-R03 | A core-module load failure throws the exact string `"Failed to load core OBS modules. OBS cannot run without these modules. Please try reinstalling OBS."`. |
| S-075-R04 | Each path scan goes through `obs_load_plugins` with `obs_runtime_module_info{name = nullptr}`; non-empty `failed_modules` are collected globally; a scan returns `Success` iff zero failures, else `PartialFailure`; the phase result is `Success` only if all its path scans were `Success`. |
| S-075-R05 | Windows modern paths: `$OBS_PLUGINS_PATH/%module%`, then ProgramData `obs-studio/plugins/%module%` (installed) / `../../plugins/%module%` (portable). Windows legacy: env pair (flat binary + data/`%module%`/`data`), `../../obs-plugins/64bit` + `../../data/obs-plugins/%module%` (always), ProgramData `…/plugins/%module%/bin/64bit` (installed only). |
| S-075-R06 | Linux modern: `$OBS_PLUGINS_PATH/%module%`, `/usr/<lib>/obs-modules/plugins` + `/usr/share/obs/obs-modules/plugins/%module%`, `$XDG_DATA_HOME` (fallback `$HOME/.local/share`) `obs-studio/plugins/%module%` (non-portable). Legacy: env pair, `/usr/<lib>/obs-plugins` + `/usr/share/obs/obs-plugins/%module%`, `$XDG_CONFIG_HOME` (fallback `$HOME/.config`) `…/plugins/%module%/bin/64bit` (non-portable). |
| S-075-R07 | BSD: modern = `$OBS_PLUGINS_PATH/%module%`, system root paths (if non-empty), config dir `obs-studio/plugins/%module%` (non-portable); legacy = env pair (both required), system `<lib>/obs-plugins` + `<data>/obs/obs-plugins/%module%`, config `…/plugins/%module%/bin/64bit` (non-portable). |
| S-075-R08 | Flatpak: modern = `/app/plugins/<lib>/obs-modules/plugins` + `/app/plugins/<data>/obs/obs-modules/plugins/%module%`, `$XDG_DATA_HOME` path (set-only); legacy = `/app/plugins/<lib>/obs-plugins` + `/app/plugins/<data>/obs/obs-plugins/%module%`, `$XDG_CONFIG_HOME` `…/bin/64bit` (set-only). |
| S-075-R09 | 33.0.1 has **no plugin install or update flow**: Discover and Updates sidebar sections are disabled "ComingSoon" placeholders; the window only manages modules already discovered via the §4 paths (install = drop files there). |
| S-075-R10 | Entry categorisation exactly as §5's table: loaded ⇒ Installed; disabled+failed ⇒ Error (and removed from the failed list); disabled+not-failed ⇒ Installed (muted); cached-but-nowhere ⇒ Missing; failed-but-uncached ⇒ dummy Error entry (`hasLoadedBefore = false`). Sort: category order (Installed, Error, Missing), then case-insensitive display name. |
| S-075-R11 | Row display: chip priority safe-mode-Disabled > Error(warning) > Missing(danger) > {Legacy chip + `https://obsproject.com/go/legacy-plugin-locations` link; Disabled chip if `!enabledAtLaunch`}. Toggle exists only for `hasLoadedBefore` entries, initial state `enabled`. |
| S-075-R12 | Toggling updates the in-memory `modules_` cache; the restart label shows while any Installed entry has `enabledAtLaunch != enabled` (never in safe mode); a changed state on close triggers the restart prompt. |
| S-075-R13 | The Missing-row trash button removes the entry from `modules.json` data only; it never deletes plugin files. |
| S-075-R14 | Crash sentinel = app config `obs-studio/.sentinel/run_<launch-uuid-without-braces>`; created if absent; any foreign `run_*` at startup ⇒ unclean shutdown; clean exit removes `run_*`; whole mechanism disabled in non-`NDEBUG` builds. |
| S-075-R15 | Crash-log discovery: Windows = app config `obs-studio/crashes`, newest `"Crash "*` by last-write time; Linux/FreeBSD = not implemented (empty); macOS = newest `OBS*.ips` in `~/Library/Logs/DiagnosticReports` by creation date. |
| S-075-R16 | New-crash = last log differs from remembered, or no remembered URL. Upload POSTs `text/plain` to `https://obsproject.com/logs/upload` (Windows body prefixed `"OBS <version> crash file uploaded at <%Y-%m-%d, %X>\n\n"`, macOS raw, others empty); response JSON `{"url":…}`; persists app-config `CrashHandler` keys `last_crash_log_file`, `last_crash_log_time`, `last_crash_log_url` + safe save. A known URL without a new log re-emits the URL without re-uploading. |
| S-075-R17 | Update branches: verify+fetched `https://obsproject.com/update_studio/branches.json` → data dir `obs-studio/updates/branches.json`; selection from app config `General`/`UpdateBranch` with fallbacks: unset → `stable` (silent), disabled → `stable` (silent), removed → `stable` + info dialog (`OBSMessageBox::information`, `Updater.BranchNotFound.*`); result signal `(branch, manual)`. |
| S-075-R18 | Sparkle (macOS): channel = {branch}; feed = `https://obsproject.com/osx_update/updates_arm64_v2.xml` iff `os_get_emulation_status()`, else default appcast; menu action enabled via KVO `canCheckForUpdates`; manual vs background check. |
| S-075-R19 | `system_info` fills `GoLiveApi::Capabilities` (cpu/memory/gpu/system/gaming_features fields as listed in §8); the 33.0.1 consumer is the GoLive post-data builder (out of file set). |
| S-075-R20 | Windows collector: DXGI adapters minus Basic Render `0x1414:0x008c`, driver version from `CheckInterfaceSupport`; CPU from `CentralProcessor\0` registry; gaming features (Win10+) from 6 registry values (Game Mode backup name; HAGS missing⇒false, disabled=1); version = major.minor + UBR + build, `DisplayVersion` iff build > 19041 else `ReleaseId`. |
| S-075-R21 | POSIX collector: DRM cards sorted by match strength (first = GL driver version + GL VRAM, rest `"Unknown"` + amdgpu sysfs VRAM); CPU name/freq helpers; distro from os-release (fallback `"Linux-based distribution"`/`"unknown"`); `uname` bits = machine has `"64"`, arm = has `"aarch"`, version string ≤ 128 chars. |
| S-075-R22 | macOS collector is arm64-only: sysctl CPU brand, SoC GPU (vendor `0x106b`, device `0`), release names for major 11–15 (else `"unknown"`), bits 64, arm true; non-arm64 `system_info` is a no-op. |
| S-075-R23 | Platform utilities behave per §9's table (single-instance checks, data paths, video save paths, locale lists, high-contrast queries differ per platform). |
| S-075-R24 | Windows session handling: `WM_QUERYENDSESSION` critical ⇒ `saveAll()`; prompt-eligible ⇒ reject + queued `close()`; else pass through. `WM_ENDSESSION(TRUE)` ⇒ immediate `closeWindow()`. Non-Windows filter is a stub returning false. |
| S-075-R25 | `VerifySignature` = RSA PKCS#1 v1.5 / SHA-512 over the buffer in the C reference (macOS via Security.framework, else mbedTLS; PEM key parsed with `pubKeyLen + 1` — caller must NUL-terminate). Per D-23 (confirmed 2026-11-06) the Rust port verifies with ed25519 (new key pair) instead — the C algorithm is pinned here as reference behaviour only. |
| S-075-R26 | Windows DLL blocklist: `NtMapViewOfSection` hook; case-insensitive **suffix** match of the full image path against ~28 blocked entries; timestamp-gated entries compare the image PE timestamp (`TS_*` methods); blocked maps denied + counted, `log_blocked_dlls` at exit. |

## 13. Reference defects and quirks

- **D-1** (comment/code divergence): the Linux and BSD loader headers document a legacy
  fallback path (`<OBS binary location>/../../obs-plugins/64bit`), but the 33.0.1 loader
  code does **not** scan it. Required behaviour: open question — replicate the code
  (no fallback) or the comment (with fallback); implementers must not guess.
- **Q-1**: the Flatpak modern XDG data path is scanned only when `$XDG_DATA_HOME` is set
  (no `$HOME/.local/share` fallback, unlike Linux).
- **Q-2**: the Windows legacy portable pair (`../../obs-plugins/64bit`,
  `../../data/obs-plugins/%module%`) is scanned unconditionally, in installed mode too.
- **Q-3**: `modules.json` has a single `"enabled"` key feeding both `enabled` and
  `enabledAtLaunch`; they only diverge in memory for the running session.
- **Q-4**: `buildCrashLogUploadContent` yields an **empty** upload body on every
  platform other than Windows and macOS.
- **Q-5**: the macOS crash-log lookup lives in `frontend/utility/CrashHandler_MacOS.mm`,
  which is not in the S-075 backlog file list; it is cited here as a cross-reference.
- **Q-6**: Flatpak `loadLegacyPlugins` leaves its XDG-config result `Failure` when
  `$XDG_CONFIG_HOME` is unset (no `else → Success`, unlike its own `loadPlugins`), so a
  Flatpak legacy scan reports `PartialFailure` on every fresh install.

## 14. Open questions

1. **D-1 resolution**: replicate the 33.0.1 code (no binary-relative legacy fallback on
   Linux/BSD) or the documented comment? Conformance vectors for legacy loading depend
   on this; needs a coordinator decision.
2. Linux/FreeBSD crash-log discovery is unimplemented in the reference (empty). Keep it
   empty (dialog with nothing to open/upload), or implement discovery (core dumps /
   abrt)? Affects what the crash dialog can offer on those platforms.
3. `FetchAndVerifyFile` + the embedded update public key + `AutoUpdateThread` are outside
   this file set — confirm S-074 pins the key material and file naming; S-075 only pins
   the branch URL and fallback logic.
4. The arm64-appcast condition (`os_get_emulation_status()`) looks inverted for Rosetta
   users (x86_64 binary under Rosetta would get the arm64 feed); confirm intent during
   conformance before replicating.
5. Windows `GetDataFilePath` tries cwd-relative `data/obs-studio/` first — pin the
   expected process cwd during conformance; the Win/macOS `HighContrastEnabled`
   mechanisms were not read in full.

Round 10 second-author check (2026-11-05): corrected R17's removed-branch dialog (was
"warning dialog"; source `MacUpdateThread.cpp` uses `OBSMessageBox::information` — §7
already said info dialog). R01/R11/R12/R14/R16/R20/R25 re-derived and confirmed.
