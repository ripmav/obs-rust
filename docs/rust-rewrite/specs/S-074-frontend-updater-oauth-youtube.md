# S-074 Frontend: updater, OAuth & YouTube integration

| | |
|---|---|
| Status | Draft (round 8 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1 (`frontend/updater/`, `frontend/oauth/`, `frontend/docks/`, `frontend/utility/`) |
| Target crate | `crates/frontend` (updater module; OAuth/YouTube as `auth`/`youtube` modules) |
| Depends on | S-077 (R36 `deobfuscate_str`, `ExecThreadedWithoutBlocking`, `OBSMessageBox`), S-035 (services), S-048 (app config, `UpdateBranch`) |
| Conformance | planned, none yet |

## 1. Scope

In scope: the Windows manifest/CDN **updater** (`updater.exe` core), the auto-update
check thread, the "what's new" pipeline, the generic OAuth framework + Twitch/YouTube/
Restream auth providers, the YouTube live-broadcast flow (API wrappers, actions dialog,
docks), GoLive (enhanced broadcasting) config download/censoring, and the shared
remote-text fetch worker. Out of scope: macOS Sparkle (S-075), CEF internals, the
GoLive API *server* (external).

### Reference files (33, full paths relative to `obs-studio/`)

- `frontend/updater/updater.cpp` — updater main loop, manifest apply, download state machine
- `frontend/updater/http.cpp` — WinHTTP file/buffer/post wrappers
- `frontend/updater/hash.cpp` — blake2b file hashing
- `frontend/updater/patch.cpp` — ZSTD-dict delta patch apply
- `frontend/updater/helpers.hpp` — `CustomHandle` RAII + `CryptProvider`
- `frontend/updater/init-hook-files.c` — post-update graphics-hook DLL copy + vulkan layer registry
- `frontend/utility/AutoUpdateThread.cpp` — in-app update check (branches + manifest)
- `frontend/utility/AutoUpdateThread.hpp` — `UpdateBranch`, `Manifest` view model
- `frontend/utility/update-helpers.cpp` — `strprintf`/`vstrprintf`
- `frontend/utility/WhatsNewInfoThread.cpp` — signed whatsnew.json fetch + `FetchAndVerifyFile`
- `frontend/utility/WhatsNewBrowserInitThread.cpp` — browser warm-up thread
- `frontend/utility/models/whatsnew.hpp` — `WhatsNew` JSON model + optional serializer
- `frontend/dialogs/OBSWhatsNew.cpp` — what's-new browser window
- `frontend/utility/RemoteTextThread.cpp` — threaded `GetRemoteFile` worker
- `frontend/utility/RemoteTextThread.hpp` — worker contract (url/headers/version)
- `frontend/utility/GoLiveAPI_Network.cpp` — `DownloadGoLiveConfig` + status-error dialog
- `frontend/utility/GoLiveAPI_PostData.cpp` — GoLive POST body construction
- `frontend/utility/GoLiveAPI_CensoredJson.cpp` — `authentication` → `CENSORED` JSON clone
- `frontend/oauth/Auth.cpp` — auth registry, `[Auth] Type` load/save
- `frontend/oauth/OAuth.cpp` — OAuth base: token store, code/refresh exchange, stream-key auth
- `frontend/oauth/AuthListener.cpp` — 127.0.0.1 callback server + state check
- `frontend/dialogs/OAuthLogin.cpp` — CEF login dialog
- `frontend/oauth/TwitchAuth.cpp` — Twitch provider: helix API, 4 docks, emote scripts
- `frontend/oauth/YoutubeAuth.cpp` — YouTube provider: auth-code flow, state
- `frontend/oauth/RestreamAuth.cpp` — Restream provider: stream key, 3 docks
- `frontend/utility/YoutubeApiWrappers.cpp` — YouTube Data API v3 wrappers
- `frontend/widgets/OBSBasic_YouTube.cpp` — broadcast flow glue on the main window
- `frontend/dialogs/OBSYoutubeActions.cpp` — YouTube broadcast actions dialog (init/ready)
- `frontend/docks/YouTubeAppDock.cpp` — YouTube Studio webapp dock
- `frontend/docks/YouTubeChatDock.cpp` — YouTube chat dock (cookie gate)
- `frontend/docks/YouTubeChatDock.hpp` — chat dock contract
- `frontend/docks/BrowserDock.cpp` — CEF dock: close/show handling
- `frontend/docks/BrowserDock.hpp` — dock contract
- (related, not counted: `frontend/updater/manifest.hpp` — `Manifest`/`Package`/`File`/`Patch*` structs, 160-bit `B2Hash`)

## 2. Conventions

RFC 2119 words. All client IDs/secrets in binaries are XOR-obfuscated with
`deobfuscate_str` (S-077 R36) and deobfuscated at use. "The updater" = the `updater.exe`
process (W). "The app" = OBS Studio itself. Config sections in `basic.ini`.

## 3. Behaviour

### 3.1 Updater core (`updater.cpp`, `http.cpp`, `hash.cpp`, `patch.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-074-R01 | `updater.cpp` | startup | CDN constants: `kCDNHostname = "cdn-fastly.obsproject.com"`, `kCDNUpdateBaseUrl = "https://cdn-fastly.obsproject.com/update_studio"`, `kPatchManifestURL = "https://obsproject.com/update_studio/getpatchmanifest"`, `kVSRedistURL = "https://aka.ms/vs/17/release/vc_redist.x64.exe"`. Manifest shape (related `manifest.hpp`): `Manifest{packages, version_major/minor/patch, beta, rc, commit, vc2019_redist_x64, notes}`; `Package{name, files, removed_files}`; `File{compressed_hash, hash, name, size}` with 160-bit blake2b hashes. | — |
| S-074-R02 | `updater.cpp` | `Update(cmdLine)` entry | First: open `OBSStudioUpdateMutex` (Synchronize); if present, `WaitForMultipleObjects({mutex, cancelRequested}, INFINITE)` — cancel wins; else `WaitForOBS()` must succeed (OBS process gone) or abort. Then `CryptAcquireContext(MS_ENH_RSA_AES_PROV, PROV_RSA_AES, CRYPT_VERIFYCONTEXT)` (signature verification). cmdline args: `Portable`/`--portable`, `--branch=<b>`, `--appdata=<dir>`, legacy `--portable--branch=` (pre-29.1-beta2 quirk). Portable → config path = `<obs_base>\config`; else `--appdata` or `FOLDERID_RoamingAppData`. Branch default `stable`. | updater state |
| S-074-R03 | `updater.cpp` / `http.cpp` | downloads | WinHTTP session: UA **`"OBS Studio Updater/3.0"`**, secure protocols TLS 1.2 + 1.3, `WINHTTP_PROTOCOL_FLAG_HTTP2`, `WINHTTP_DECOMPRESSION_FLAG_ALL`. `HTTPGetFile` streams to disk with progress callback + response code + optional extra headers; `HTTPGetBuffer` to memory; `HTTPPostData` form/JSON POST (response code out). Connect failure → status `"Update failed: Couldn't connect to <host>"`. | files |
| S-074-R04 | `updater.cpp` | per-file state machine | `INVALID → PENDING_DOWNLOAD → DOWNLOADING → DOWNLOADED`; `ALREADY_DOWNLOADED` when the existing local file's blake2b equals the manifest hash (skip); `INSTALL_FAILED → INSTALLED` after the apply step. `update_t.CleanPartialUpdate`: on `INSTALL_FAILED`/`INSTALLED` with a backup → restore `previousFile` over `outputPath`, delete backup. | per-file state |
| S-074-R05 | `updater.cpp` | file fetch | Target path must pass `IsSafeFilename` (no path traversal) — both manifest-derived and patch-derived names. Download URL = `kCDNUpdateBaseUrl/<branch>/<package>/<file>`; when `compressed_hash` is set the URL gains `.zst` and the body is ZSTD-decompressed while streaming; final file hash must equal `hash`. | file on disk |
| S-074-R06 | `updater.cpp` / `patch.cpp` | patch path | A file is *patchable* when the patch manifest (fetched from `kPatchManifestURL`, per branch) offers a `PatchResponse{name, hash, source, size}` for it; `source` **must start with the CDN base URL**, and `name` is `"<package>/<file>"`. On apply, the patch replaces the file's download URL/hash/size. `ApplyPatch`: header = `kDeltaMagic "BOUF//ZSTD//DICT"` (16 bytes, `memcmp`) + 8-byte little-endian signed new size (`offtin`, sign bit `buf[7]&0x80`); invalid magic → `-4`, size `<0 || >= 0x7ffffffff` → `-5`, alloc failure → `-1`; body is ZSTD-decompressed with a dictionary into the new file image, then installed. | file on disk |
| S-074-R07 | `updater.cpp` | redist | `vc2019_redist_x64` manifest flag → offer download of `kVSRedistURL` (separate from the package loop). | — |
| S-074-R08 | `hash.cpp` | hashing | 160-bit (20-byte) blake2b: `CalculateFileHash` streams the file in 1 MiB reads (thread-local buffer); `HashToString`/`StringToHash` = lowercase hex. | — |
| S-074-R09 | `init-hook-files.c` | post-update (W) | `UpdateHookFilesLegacy`/`UpdateHookFiles`: copy the updated `graphics-hook.dll` from the update dir to **both** hook locations per bitness (b64 = 64-bit view): legacy `\data\obs-plugins\win-capture\` and core `\core\win-capture\data\`. After a successful copy, `update_vulkan_registry(b64)`: build `<ProgramData>\obs-studio-hook\obs-vulkan.json`; **delete** a stale HKCU `VulkanImplicitLayers` value for that path; if no HKLM value exists yet, create HKLM key and set the value to `REG_DWORD 0` (the layer manifest path is the *value name*; the DWORD marks it OBS-owned). | files, registry |
| S-074-R10 | `helpers.hpp` | RAII | `CustomHandle<T, free>`: dtor/assign/release call the free function exactly once; `operator&`/`operator T`/`get`/`operator!`. `CryptProvider` = handle around `CryptReleaseContext`. | — |

### 3.2 Auto-update check (`AutoUpdateThread.*`, `update-helpers.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-074-R11 | `AutoUpdateThread.cpp` | `run()` | RAII `FinishedTrigger` emits `OBSBasic::updateCheckFinished` on exit. (1) Fetch + verify `branches.json` → `App()->SetBranchData`. (2) Branch selection from app config `[General] UpdateBranch`: branch *found but disabled* → silently fall back to default; branch *removed* → warn. Non-default enabled branch → manifest URL `https://obsproject.com/update_studio/manifest_<branch>.json` (default: `.../manifest.json`). (3) Fetch + verify manifest (same `obs-studio\updates\` cache dir, extra headers for cache-bust). (4) Fetch + verify `updater.exe` itself. | cache files, branch |
| S-074-R12 | `AutoUpdateThread.cpp` | version compare | `version_major == 0 && commit.empty()` → `throw "Invalid version number..."`. With `commit` (nightly/test): compare commit hash (first 8 chars shown), `updateVer = commit[0:8]`. Else: `new_ver = MAKE_SEMANTIC_VERSION(major, minor, patch) << 16`; `rc > 0 → new_ver |= rc << 8` else `beta > 0 → new_ver |= beta` (RC shifted so rc1 ≠ beta1). Non-default branch **or** pre-release: update available iff `new_ver != currentVersion` (allows rollback); default stable branch: iff `new_ver > currentVersion`. | `updatesAvailable` |
| S-074-R13 | `update-helpers.cpp` | formatting | `vstrprintf`/`strprintf` — `std::string` printf helpers used for updater error messages. | — |

### 3.3 "What's new" pipeline (`WhatsNewInfoThread.cpp`, `OBSWhatsNew.cpp`, `WhatsNewBrowserInitThread.cpp`, `models/whatsnew.hpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-074-R14 | `WhatsNewInfoThread.cpp` | `run()` | `FetchAndVerifyFile("whatsnew", "obs-studio/updates/whatsnew.json", WHATSNEW_URL, &text)` → `emit Result(text)`. All three platforms use the same URL `https://obsproject.com/update_studio/whatsnew.json`. | cache file |
| S-074-R15 | `WhatsNewInfoThread.cpp` | `FetchAndVerifyFile` | Reuse a cached file iff its stored blake2b-style hash matches; else download with `GetRemoteFile` carrying an `Obs-Updates-Signature`-style expectation: the response must include a hex signature which is verified with **RSA** against `OBSPublicRSAKey.pem` (data file, loaded once under a mutex; `VerifySignature(key, data, sig)`); signature missing/odd-length/too-long → `throw "Missing or invalid signature for <name>: ..."`, mismatch → `throw "Signature check failed for <name>"`. `GenerateGUID`/`InstallGUID` (app config) for device identity. | cache + hash file |
| S-074-R16 | `OBSWhatsNew.cpp` | dialog | CEF required: 700×600 window titled `"What's New"`, no context-help button, `WA_DeleteOnClose`; `InitBrowserPanelSafeBlock`, `cef->create_widget(nullptr, url)`; window title follows `QCefWidget::titleChanged` (queued); `reject` closes the browser. No CEF → no window. | window |
| S-074-R17 | `WhatsNewBrowserInitThread.cpp` / `models/whatsnew.hpp` | warm-up / model | The init thread exists only to initialize CEF off the UI thread before the window opens. `whatsnew.hpp`: `WhatsNew` JSON model (`version`, `url`, optional fields) with an `nlohmann` `std::optional` ADL serializer (null → `nullopt`). | — |

### 3.4 Remote text + GoLive (`RemoteTextThread.*`, `GoLiveAPI_*`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-074-R18 | `RemoteTextThread.cpp` | worker | Wraps `GetRemoteFile` (libobs curl helper) on a `QThread`: sets a version User-Agent header, optional content-type + extra headers, optional POST data; emits result (text/error) to the UI thread. Used for remote text configs and OAuth token POSTs. | — |
| S-074-R19 | `GoLiveAPI_Network.cpp` | `DownloadGoLiveConfig(parent, url, post, name)` | POST the body to the service's auto-config URL; parse JSON into `GoLiveApi::Config`. Then `HandleGoLiveApiErrors`: `status.result` — `Success` → silent; `Unknown` → warning dialog (`ConfigDownload.WarningMessageTitle`, rich text `WarningUnknownStatus.arg(result-json)` + `WarningRetry`, buttons Yes/No — **No cancels** via `MultitrackVideoError::cancel()`); `Warning` → if no encoder configurations `throw warning(status.html_en_us)` else the same Yes/No continue dialog; `Error` → `throw critical(status.html_en_us)`. Missing HTML → `StatusMissingHTML` fallback. Dialog runs on the UI thread via blocking connection. | go-live config |
| S-074-R20 | `GoLiveAPI_PostData.cpp` | POST body | Builds the GoLive request JSON: stream key (inside `authentication`), max aggregate bitrate, max video tracks, vod-track flag, canvas list — matching `GoLiveApi::Config` on the response side. | — |
| S-074-R21 | `GoLiveAPI_CensoredJson.cpp` | logging | `censoredJson(obs_data_t | nlohmann::json, pretty)`: clones the data (via JSON write/read for obs_data), recursively replaces every `"authentication"` string with `"CENSORED"`, returns (pretty) JSON. Used for *all* go-live config logging so keys never hit the log. | — |

### 3.5 Auth framework (`Auth.cpp`, `OAuth.cpp`, `AuthListener.cpp`, `OAuthLogin.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-074-R22 | `Auth.cpp` | registry / load / save | `RegisterAuth(Def{service, type, externalOAuth}, create)`. Lookup uses **substring `std::string::find`** (service names are prefixes; order of registration decides ties). `Load()`: `[Auth] Type` → `Create` → `LoadInternal()` (per-provider token check) → `LoadUI()` + `SetBroadcastFlowEnabled(auth->broadcastFlow())`; no type/failed → broadcast flow disabled. `Save()`: sets `[Auth] Type` to the active provider's service name; signing out **removes** the key (`config_remove_value` when `config_has_user_value`). | `main->auth`, config |
| S-074-R23 | `OAuth.cpp` | token store | Per-service section: `RefreshToken`, `Token`, `ExpireTime` (uint64), `ScopeVer` (int). `LoadInternal`: valid iff `implicit ? !token.empty() : !refresh_token.empty()`. `TokenExpired`: empty token → true; `time(nullptr) > expire_time - 5` → true (5 s early expiry margin). | config |
| S-074-R24 | `OAuth.cpp` | `GetTokenInternal` | Scope bump: `0 < currentScopeVer < scope_ver` → `RetryLogin()` (re-login); refused → `QMessageBox::warning(Auth.InvalidScope.Title/.Text.arg(service))`. `auth_code` empty **and** token not expired → return true (reuse stored token). POST (form-encoded, `application/x-www-form-urlencoded`) to the token URL: `action=redirect&client_id=…[&client_secret=…][&redirect_uri=…]` + `grant_type=authorization_code&code=…` or `grant_type=refresh_token&refresh_token=…`. Executed through `ExecThreadedWithoutBlocking` (busy dialog `Auth.Authing.Title/.Text`). Response JSON: `error == "invalid_grant"` (non-retry) → `RetryLogin()` (re-login; true if it succeeds); other errors → throw `ErrorInfo(error, error_description)`, catch: non-retry → `QMessageBox::warning(Auth.AuthFailure.Title/.Text.arg(service, message, error))`, then `LOG_WARNING "<fn>: <message>: <error>"` + false. On success store token/refresh/expire/scope ver. | token |
| S-074-R25 | `AuthListener.cpp` | callback server | `QTcpServer` bound to `QHostAddress::LocalHost` port **0** (ephemeral); `GetPort()` for the `redirect_uri = "http://127.0.0.1:<port>"`. `SetState(state)`. On connect: read the redirect URL; extract `state` with regex `(&|\?)state=(?<state>[^&]+)` (and `code` similarly); `state` mismatch → `LOG_WARNING "state mismatch …"`, no state at all → `LOG_DEBUG "no 'state' in …"`; match → deliver code. | code |
| S-074-R26 | `OAuthLogin.cpp` | login dialog | CEF login window: title `"Auth"`, min 400×400, default 700×700, context-help button removed, uses the shared `panel_cookies` manager (so provider cookies persist across logins); `titleChanged` → window title (queued); Cancel button → `reject` (closes browser). No CEF → `exec()` always returns `Rejected`. | dialog |

### 3.6 Twitch (`TwitchAuth.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-074-R27 | `TwitchAuth.cpp` | login | `OAuthLogin` on `OAUTH_BASE_URL "v1/twitch/redirect"` (`obsproject.com/v1/twitch/redirect`); on code: `GetToken(OAUTH_BASE_URL "v1/twitch/token", deobfuscated client_id, scope_ver)`. Provider def `{"Twitch", Auth::Type::OAuth_StreamKey}`. | token |
| S-074-R28 | `TwitchAuth.cpp` | `MakeApiRequest(path)` | GET `https://api.twitch.tv/helix/<path>` with headers `Client-ID: <deobfuscated>` + `Authorization: Bearer <token>`, JSON content type, 5 s timeout, via `ExecThreadedWithoutBlocking` (`Auth.LoadingChannel.*`). **403 → `QMessageBox::warning(TwitchAuth.TwoFactorFail.Title/.Text)`** + `LOG_WARNING` ("user probably does not have two-factor authentication enabled") + false. Other failure/empty → `throw ErrorInfo("Failed to get text from remote", error)`; JSON parse error → `throw ErrorInfo("Failed to parse json", error)`; `error` field → `throw ErrorInfo(error, message)`. | — |
| S-074-R29 | `TwitchAuth.cpp` | `GetChannelInfo` | Helix `channels` lookup for the logged-in user → `Name` (login) + `UUID` stored in `[Twitch] Name/UUID`. | config |
| S-074-R30 | `TwitchAuth.cpp` | `LoadUI` (docks) | Requires CEF. Panel UUID: generated client-side (`QUuid`, `{}`/`-` stripped) when unset — *not* unique, only for the browser's local store. Four `BrowserDock`s (object names fixed): `twitchChat` = `https://www.twitch.tv/popout/<login>/chat` (300×600, min 200×300), with `cef->add_force_popup_url` on the moderation URL `https://www.twitch.tv/<login>/dashboard/settings/moderation?no-reload=true`; `twitchInfo` = `https://dashboard.twitch.tv/popout/u/<login>/stream-manager/edit-stream-info` (300×650, min 200×300); `twitchStats` = `https://www.twitch.tv/popout/<login>/dashboard/live/stats` (200×250, min 200×150); `twitchFeed` = `https://dashboard.twitch.tv/popout/u/<login>/stream-manager/activity-feed?uuid=<uuid>` (300×650, min 200×300). Emote extensions injected as CEF scripts: FrankerFaceZ `script.min.js`, BetterTTV `betterttv.js` + dark-mode/localStorage flags, and a document referrer override. `info`/`stats`/`feed` float and are positioned; on first load only `info` shows, otherwise `DockState` (main-window saveState base64) is read from `[<service>]` and restored (feed hidden when app `LastVersion ≤ 23.0.2`). | docks, config |
| S-074-R31 | `OAuth.cpp` | `OAuthStreamKey::OnStreamConfig` | On every stream setup: if `key_` empty → no-op. Service settings: `bwtest` bool **and** service `Twitch` → key = `"<key>?bandwidthtest=true"`, else plain key; `obs_service_update`. Called from the output handlers (S-073 R15/R24). | service key |

### 3.7 YouTube (`YoutubeAuth.cpp`, `YoutubeApiWrappers.cpp`, `OBSBasic_YouTube.cpp`, `OBSYoutubeActions.cpp`, `YouTubeAppDock.cpp`, `YouTubeChatDock.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-074-R32 | `YoutubeAuth.cpp` | login | `Login(owner, service)`: find the `youtubeServices` entry for the service id (exact match). `YoutubeApiWrappers` per service. `AuthListener` on 127.0.0.1; `state` = **32** random chars from a fixed alphabet (`YOUTUBE_API_STATE_LENGTH`). Auth URL `https://accounts.google.com/o/oauth2/v2/auth?response_type=code&client_id=<deobf>&redirect_uri=http://127.0.0.1:<port>&state=<state>&scope=https://www.googleapis.com/auth/youtube`. Waiting dialog (no close button, Cancel only; macOS forces non-native so the link is clickable) with the URL as a link; default action opens the system browser; the code is picked up by the listener, then `GetToken(https://www.googleapis.com/oauth2/v4/token, client_id, secret, redirect_uri, scope_ver, code)`. | token |
| S-074-R33 | `YoutubeAuth.cpp` | store | Same four keys as R23 under the *YouTube service section* (`SaveInternal`/`LoadInternal` duplicated per provider). | config |
| S-074-R34 | `YoutubeApiWrappers.cpp` | API calls | Endpoints (all `googleapis.com`): `youtube/v3/liveStreams`, `liveBroadcasts`, `liveBroadcasts/transition`, `liveBroadcasts/bind`, `channels`, `videoCategories`, `videos`, `upload/youtube/v3/thumbnails/set`; token refresh at `oauth2.googleapis.com/token`. Requests carry `Authorization: Bearer <token>`; each wrapper posts/gets JSON via `GetRemoteFile` and maps responses into typed structs. | — |
| S-074-R35 | `OBSBasic_YouTube.cpp` | broadcast flow | `BroadcastButtonClicked` → `SetupBroadcast` (enabled only when `SetBroadcastFlowEnabled(true)`, i.e. an auth provider with `broadcastFlow()` is loaded): opens the actions dialog. `YouTubeActionDialogOk(broadcastId, streamId, streamName)`: builds a YouTube RTMP service (custom URL from the bind response) + enables the flow; `YoutubeStreamCheck(key)`: validates the stream key via the API; `ShowYouTubeAutoStartWarning`; `NewYouTubeAppDock`/`DeleteYouTubeAppDock` manage the webapp dock for the active channel. | service, dock |
| S-074-R36 | `OBSYoutubeActions.cpp` | dialog | Two-phase: **InitBroadcast** (OK): create the live stream + broadcast objects via the API, `bind` to get the stream URL/key, store `broadcastId`/`streamId`. **ReadyBroadcast** (save): `liveBroadcasts/transition` → `liveBroadcasting` (go live) vs `completed`; video category/privacy/thumbnail fields from the dialog. Errors surface as message boxes with the API `error`/`message`. | broadcast state |
| S-074-R37 | `YouTubeAppDock.cpp` | dock | Webapp URL `https://studio.youtube.com/live/channel/<channelId>/console?kc=OBS`; CEF cookie sync for Google; injected JS bridges (window.open interception on `studio.youtube.com` hostname, video URL `://studio.youtube.com/video/` handling); chat placeholder page `https://obsproject.com/placeholders/youtube-chat`; chat popout `https://www.youtube.com/live_chat?is_popout=1&dark_theme=1&v=%1`. | dock |
| S-074-R38 | `YouTubeChatDock.cpp` / `BrowserDock.cpp` | dock | `YouTubeChatDock`: on load, `panel_cookies->CheckForCookie("https://www.youtube.com", "SID", cb)` — the chat panel only works once a signed-in YouTube cookie is present. `BrowserDock`: `closeEvent` (if accepted) → `cefWidget->closeBrowser()` when CEF ≥ v2; `showEvent` → restore `title`. | dock |

### 3.8 Restream (`RestreamAuth.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-074-R39 | `RestreamAuth.cpp` | login / key | Auth via `OAUTH_BASE_URL "v1/restream/redirect"`, token via `.../v1/restream/token` (scope ver 1). Stream key: GET `https://api.restream.io/v2/user/streamKey` with headers `Client-ID: <deobfuscated>` + `Authorization: Bearer <token>`, JSON, 5 s, `ExecThreadedWithoutBlocking` (`Auth.LoadingChannel.*`); failure → `throw ErrorInfo("Failed to get stream key from remote", error)`. Provider def `{"Restream", Auth::Type::OAuth_StreamKey}`. | token, key |
| S-074-R40 | `RestreamAuth.cpp` | docks | Three `BrowserDock`s: `restreamChat` = `https://restream.io/chat-application`, `restreamInfo` = `https://restream.io/titles/embed`, `restreamChannel` = `https://restream.io/channel/embed`. Removed (with all others) on sign-out. | docks |

## 4. Reference defects and quirks

- **Q-1** (`Auth.cpp`): provider lookup is **substring** `find` against the service string —
  service names must stay prefix-disjoint, and registration order silently breaks ties.
- **Q-2** (`TwitchAuth.cpp`): the 403→"two-factor fail" heuristic is a UX guess, not an API
  guarantee; any 403 (including a revoked token) shows the 2FA warning.
- **Q-3** (`init-hook-files.c`): the HKLM vulkan-layer entry is written as `REG_DWORD 0` —
  the *name* of the value (the manifest path) is the registry content; do not "fix" the type.
- **Q-4** (`updater.cpp`): `OBSStudioUpdateMutex` is created by the *running* OBS; the updater
  merely opens it. A stale mutex from a crashed OBS blocks the updater until OBS restarts.
- **Q-5** (`AutoUpdateThread.cpp`): RC versions sort *above* the matching release
  (`rc << 8` in the low 16 bits) — an rc of 33.0.1 is "newer" than the stable 33.0.1 manifest.

## 5. Concurrency (summary)

- Updater: single UI thread + download progress on the WinHTTP callback thread; file state is
  per-`update_t` and not shared.
- Auto-update check runs on a `QThread`; user prompts (`OBSUpdate` dialog, repair confirm) hop
  to the UI thread via `BlockingQueuedConnection`.
- OAuth token exchange never blocks the UI thread (`ExecThreadedWithoutBlocking`); the
  `AuthListener` socket runs in the UI thread's event loop.
- `WhatsNewInfoThread`/`RemoteTextThread` are one-shot `QThread`s emitting queued signals.

## 6. Open questions

1. The updater is Windows-only in 33.0.1; confirm the Rust plan keeps a separate
   `updater` binary (W) vs. in-app auto-update only, and whether `patch.cpp`
   (ZSTD-dict deltas) is in scope at all.
2. Resolved (D-23, 2026-11-06): the key moves to ed25519 — a new key pair ships as a data
   file, replacing `OBSPublicRSAKey.pem`; the C build's RSA-PSS/PKCS1-v1_5
   (`VerifySignature`, libobs `crypto-helpers`) stays the documented reference only.
3. YouTube broadcast flow (R35–R38) depends on CEF; if the rewrite's browser layer differs
   (S-062/CEF replacement), the dock injection scripts need a host-API equivalent — flag when
   the browser spec lands.
4. Round 10 second-author check (2026-11-05): corrected R30 — `twitchInfo` was
   `https://www.twitch.tv/<login>/dashboard/settings/moderation?no-reload=true` (that URL is the
   `add_force_popup_url` attached to `twitchChat`; real `twitchInfo` is
   `https://dashboard.twitch.tv/popout/u/<login>/stream-manager/edit-stream-info`; `twitchStats`/
   `twitchFeed` URLs were unspecified; added floating/first-load/`DockState`-restore details) —
   and R24 — error path was `LOG_WARNING` + false only (now includes the `Auth.AuthFailure`
   warning dialog on non-retry, and `invalid_grant` is a `RetryLogin()` re-login without sign-out).
