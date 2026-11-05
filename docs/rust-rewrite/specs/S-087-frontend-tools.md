# S-087 frontend-tools plugin (auto-scene-switcher, output-timer, captions, scripts UI)

| | |
|---|---|
| Status | Draft (round 8 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1 (`plugins/frontend-tools/`) |
| Target crate | `crates/frontend-tools` (plugin crate, links `crates/frontend` API) |
| Depends on | S-048 (frontend API + save/preload callbacks, event enum), S-014 (obs_data) |
| Conformance | planned, none yet |

## 1. Scope

In scope: the four tools of the `frontend-tools` plugin and their behavioural cores:
auto-scene-switcher (window-title → scene matching loop), output-timer (stream/record
timers), captions (MSS API speech-to-text, **Windows-only** — see R22), scripts UI
(list management, reload, script log window). Out of scope: the `obs-scripting` engine
itself (libobs-side, separate spec), CEF/browser rendering, localization content.

### Reference files (13, full paths relative to `obs-studio/`)

- `plugins/frontend-tools/frontend-tools.c` — module entry, init/free ordering
- `plugins/frontend-tools/auto-scene-switcher.cpp` — switcher state + dialog + match thread
- `plugins/frontend-tools/auto-scene-switcher-win.cpp` — Windows window enumeration
- `plugins/frontend-tools/auto-scene-switcher-nix.cpp` — X11/EWMH window enumeration
- `plugins/frontend-tools/auto-scene-switcher-osx.mm` — macOS "window" enumeration
- `plugins/frontend-tools/output-timer.cpp` — stream/record timers
- `plugins/frontend-tools/captions.cpp` — captions dialog + state (Win)
- `plugins/frontend-tools/captions-handler.cpp` — resampling base class
- `plugins/frontend-tools/captions-handler.hpp` — handler contract, `captions_handler_info`
- `plugins/frontend-tools/captions-mssapi.cpp` — MSS API session
- `plugins/frontend-tools/captions-mssapi-stream.cpp` — `CaptionStream` COM audio stream
- `plugins/frontend-tools/scripts.cpp` — scripts tool + log window
- `plugins/frontend-tools/tool-helpers.hpp` — weak-source helpers (see D-1)

## 2. Conventions

RFC 2119 words. Config keys are `obs_data` names under the tool's top-level key in
`basic.ini` (profile config) unless stated. Platform columns: W = Windows, L = Linux,
M = macOS. "The switcher", "the timer", "captions" refer to the plugin's process-global
state objects.

## 3. Behaviour

### 3.1 Module entry (`frontend-tools.c`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-087-R01 | `frontend-tools.c` | `obs_module_load` | Init order: `InitCaptions` (W only) → `InitSceneSwitcher` → `InitOutputTimer` → `InitScripts` (only when scripting is compiled in **and** not safe mode; else `LOG_WARNING "Scripting is disabled in Safe Mode"`). MUST always return `true`. | per-tool globals |
| S-087-R02 | `frontend-tools.c` | `obs_module_unload` | Reverse order; `FreeScripts` only if scripting was inited (i.e. not safe mode). | — |
| S-087-R03 | all tool files | `Init*` | Each tool adds exactly one `QAction` to the Tools menu via `obs_frontend_add_tools_menu_qaction(<translated title>)` and registers its save/event callbacks (S-048 §5.9). The switcher and captions dialogs are created lazily per trigger; the output-timer dialog is created at init (show/hide on trigger) and the scripts tool window on first trigger (log window at init). Trigger callbacks wrap the dialog in `obs_frontend_push/pop_ui_translation(obs_module_get_string)`. | Tools menu |

### 3.2 Auto-scene-switcher (`auto-scene-switcher*.cpp`)

Core state (`SwitcherData`): `switches: Vec<(scene: WeakSource, window: String, re: Regex)>`,
`nonMatchingScene: WeakSource`, `interval: int = 300` ms (`DEFAULT_INTERVAL`),
`switchIfNotMatching: bool`, worker thread + condvar + `stop` flag.

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-087-R04 | `auto-scene-switcher.cpp` | worker loop tick | `cv.wait_for(interval)`; on stop flag: clear it and exit. Read foreground title `title`; **skip all evaluation when `title == lastTitle`**. On change: prune; then (a) linear scan for exact match `s.window == title`; (b) else first `std::regex_match(title, s.re)` (full match), `regex_error` swallowed per entry; (c) else if `switchIfNotMatching && nonMatchingScene` → that scene. If matched scene ≠ current scene: `obs_frontend_set_current_scene`. `lastTitle = title`. | current scene |
| S-087-R05 | `auto-scene-switcher.cpp` | title change / dialog open / save | `Prune()`: drop entries whose scene weak ref no longer resolves; if `nonMatchingScene` dead → `switchIfNotMatching = false`, clear it. | `switches` |
| S-087-R06 | `auto-scene-switcher.cpp` | save/load | Key `"auto-scene-switcher"`: `interval` (int, default 300), `non_matching_scene` (string), `switch_if_not_matching` (bool), `active` (bool = thread joinable), `switches` (array of `{scene, window_title}`). Load: read defaults, rebuild list, `Start()` iff `active`. | switcher state |
| S-087-R07 | `auto-scene-switcher.cpp` | dialog "Add" | Empty window text → no-op. No entry for window → append `(scene, window)`. Entry exists → **reassign** its scene to the chosen one and re-sort list (no duplicates). Invalid regex (`regex_error`) → warning dialog `InvalidRegex.*`, no entry. List shows `"[<scene>]: <window>"`; item user-data = window string. | `switches` |
| S-087-R08 | `auto-scene-switcher-win.cpp` | window list / current (W) | List: walk `GetWindow(GetDesktopWindow(), GW_CHILD)` + `GetNextWindow(GW_HWNDNEXT)`; a window qualifies iff visible, owner PID ≠ current PID, not `WS_EX_TOOLWINDOW`, not `WS_CHILD`; title via `GetWindowTextW` → UTF-8. Current: `GetForegroundWindow()`; if owner PID == current PID → title `""` (never match own windows). | — |
| S-087-R09 | `auto-scene-switcher-nix.cpp` | window list / current (L) | Requires EWMH: `_NET_SUPPORTING_WM_CHECK` on root (and self-check) or both calls return empty/none. List: `_NET_CLIENT_LIST` per screen; title `XFetchName`, fallback `XGetWMName`. Current: `_NET_ACTIVE_WINDOW` on screen 0, same title fallback. Non-EWMH → empty list, current unchanged. | — |
| S-087-R10 | `auto-scene-switcher-osx.mm` | window list / current (M) | **Application names, not window titles**: list = `NSWorkspace.runningApplications[].localizedName` (non-empty); current = `frontmostApplication.localizedName`. | — |
| S-087-R11 | `auto-scene-switcher.cpp` | init (L, non-wayland only) | If Qt platform name contains `"wayland"` (and not W/M) → `InitSceneSwitcher` returns immediately: no state, no menu action, no callbacks. | — |
| S-087-R12 | `auto-scene-switcher.cpp` | dialog close / EXIT | `finished` (dialog done) → `obs_frontend_save()` (persists state). `OBS_FRONTEND_EVENT_EXIT` → `FreeSceneSwitcher` (cleanup display, stop thread, delete state). | — |
| S-087-R13 | `auto-scene-switcher.cpp` | Start/Stop toggle | Start: spawn thread iff not joinable. Stop: set `stop` under lock, `notify_one`, join. Dialog start button text `Start`/`Stop`, status `Inactive`/`Active`. | thread |

Note for implementers: 33.0.1 has **no** time-in-scene, min-time, priority or
manual-override scoring — the only inputs are the current foreground title and the
configured interval. Do not add those.

### 3.3 Output-timer (`output-timer.cpp`)

State: `streamingTimer`/`recordingTimer` (single-shot `QTimer`s), display timers at
1000 ms, flags `streamingAlreadyActive`/`recordingAlreadyActive`, `recordingTimeLeft` (ms, −1).

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-087-R14 | `output-timer.cpp` | duration computation | `total = (h*3600 + m*60 + s) * 1000` ms; if `total == 0` → `total = 1000`. | — |
| S-087-R15 | `output-timer.cpp` | stream button click | If streaming not active → `obs_frontend_streaming_start()` (log `LOG_INFO "Starting stream due to OutputTimer"`). Else if `streamingAlreadyActive` → `StreamTimerStart()` and clear flag. Else (active) → `obs_frontend_streaming_stop()`. Record button mirrors this. | output, timer |
| S-087-R16 | `output-timer.cpp` | `STREAMING_STARTED` / `RECORDING_STARTED` | Start the matching timer **iff** dialog visible **or** the `autoStart*Timer` checkbox is checked; otherwise set `*AlreadyActive = true` and return (deferred; R15 second branch fires it on the next click). | timer |
| S-087-R17 | `output-timer.cpp` | `STREAMING_STOPPING` / `RECORDING_STOPPING` | Stop timer + display timer, button text back to `Start`, display reset `00:00:00`, clear `*AlreadyActive`. | timer |
| S-087-R18 | `output-timer.cpp` | timer expiry | Single-shot timeout → `obs_frontend_streaming_stop()` / `obs_frontend_recording_stop()` with `LOG_INFO "...due to OutputTimer timeout"`. There is **no** action selector — expiry always stops the respective output. | output |
| S-087-R19 | `output-timer.cpp` | `RECORDING_PAUSED` / `RECORDING_UNPAUSED` | Pause (only when `pauseRecordTimer` checked and timer active): store `recordingTimeLeft = remainingTime()`, stop timer. Unpause (same check): if `recordingTimeLeft > 0` and timer not active → restart with that remaining time. | timer |
| S-087-R20 | `output-timer.cpp` | display tick (1 s) | Stream: `remainingTime()/1000` → `%02d:%02d:%02d`. Record: while `obs_frontend_recording_paused()` **and** `pauseRecordTimer` checked → use `recordingTimeLeft/1000`; else timer remaining. | labels |
| S-087-R21 | `output-timer.cpp` | save/load | Key `"output-timer"`: `streamTimerHours/Minutes/Seconds`, `recordTimerHours/Minutes/Seconds`, `autoStartStreamTimer`, `autoStartRecordTimer`, `pauseRecordTimer`. `OBS_FRONTEND_EVENT_EXIT` → `obs_frontend_save()`. Tools-menu action toggles dialog visibility. | config |

### 3.4 Captions (Windows-only contract)

**Platform exclusion (R22):** `captions.cpp`, `captions-mssapi*.cpp` compile only under
`_WIN32`; `InitCaptions` is called only on W. The tool is absent elsewhere. A conforming
implementation MUST NOT offer captions on non-Windows builds.

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-087-R22 | `captions.cpp` | init | Register handler `"mssapi"` → name `"Microsoft Speech-to-Text"` in a handler map (`captions_handler_info {name(), create(cb, lang)}`). State: `handler_id` (default `"mssapi"`), `source_name` + `source: WeakSource`, `lang_id = GetUserDefaultUILanguage()`, `handler: Option<unique_ptr>`. | state |
| S-087-R23 | `captions.cpp` | start (enable / source / language / provider change) | If no handler and `valid_lang(lang_id)` (MSS `SpFindBestToken(SPCAT_RECOGNIZERS, "language=%x")` succeeds): look up handler (missing → `LOG_WARNING`, return), `LCIDToLocaleName` → locale string (narrowed char-by-char), resolve source strong ref (invalid → `LOG_WARNING "Source invalid"`, return), create handler, then `obs_source_add_audio_capture_callback(source, audio_capture)`. Handler creation throws `std::string` → `LOG_WARNING "Failed to create handler: %s"` + `QMessageBox::warning(Captions.Error.GenericFail)`. Every change while active: stop → mutate → start. | handler, callback |
| S-087-R24 | `captions.cpp` / `captions-handler.cpp` | stop | Remove audio capture callback (if source alive), destroy handler. Dtor calls stop. | handler |
| S-087-R25 | `captions-handler.cpp` | audio capture callback | Resample OBS audio (src = `obs_get_audio_info`: planar float, OBS sample rate, OBS speaker layout) to **always mono** (`SPEAKERS_MONO`) at the handler's target format/rate, then `pcm_data(out, frames)`. Failure to (re)create resampler → `LOG_WARNING` + false (ctor throws `CAPTIONS_ERROR_GENERIC_FAIL`). | — |
| S-087-R26 | `captions-mssapi.cpp` | ctor | Target format: 16-bit PCM, 16000 Hz. Create stop event; `SpFindBestToken` (locale → `language=%x` LCID via `LocaleNameToLCID`); `CoCreateInstance(CLSID_SpInprocRecognizer)`; `SetRecognizer(token)`; `SetRecoState(SPRST_INACTIVE)`; `CreateRecoContext`; `SetInterest(SPEI_RECOGNITION \| SPEI_END_SR_STREAM)`; `SetNotifyWin32Event`; `GetNotifyEventHandle`; create `CaptionStream` at **current audio output sample rate**; `SetInput(stream)`; `CreateGrammar(1)`; `LoadDictation(nullptr, SPLO_STATIC)`; spawn worker thread. Any failure: `LOG_WARNING` + throw `CAPTIONS_ERROR_GENERIC_FAIL`. | MSS session |
| S-087-R27 | `captions-mssapi.cpp` | worker thread | `grammar->SetDictationState(SPRS_ACTIVE)`; `recognizer->SetRecoState(SPRST_ACTIVE)`; set `started`; loop `WaitForMultipleObjects({notify, stop})` — anything but `notify` exits. Drain `CSpEvent`s: `SPEI_RECOGNITION` → `result->GetText(-1, -1, true)` → UTF-8 into 512-byte buffer → `callback(text)` + `LOG_DEBUG` (failed `GetText` skipped); `SPEI_END_SR_STREAM` → exit. On exit `audio->Stop()`. `pcm_data` pushes to stream only after `started`. | recognition |
| S-087-R28 | `captions.cpp` | recognition callback | `obs_frontend_get_streaming_output()`; if present → `obs_output_output_caption_text1(output, text)`. No streaming output → text dropped silently. | stream caption |
| S-087-R29 | `captions-mssapi-stream.cpp` | COM usage | `CaptionStream` implements `IStream`/`ISpAudio`/`ISpStreamFormat`. Format: `WAVE_FORMAT_PCM`, 1 ch, 16000 Hz, 16-bit, block align 2. `SPAUDIOBUFFERINFO`: min-notify 50 ms, buffer 500 ms, bias 0. `PushAudio`: append `frames*2` bytes to internal deque; wake reader if it was waiting for ≥ that size. `Read(p, n)`: wait on cv while `n > buf.size`; copy min(n, size); return `S_FALSE` if less than n; advance seek pos. `Write` → `STG_E_INVALIDFUNCTION`. `Seek` only `SEEK_CUR` + 0 (returns current pos). `GetStatus` reports `CurSeekPos`. `SetState` stores state. | audio stream |
| S-087-R30 | `captions.cpp` | dialog population | Source combo: one entry per source with `OBS_SOURCE_AUDIO` output flag (empty entry first, preselected). Language combo: current system UI language first (label `Captions.CurrentSystemLanguage` with the single English name from `LOCALE_SENGLISHLANGUAGENAME`), then fixed table `0x0409, 0x0401, 0x0402, 0x0403, 0x0404, 0x0405, 0x0406, 0x0407, 0x0408, 0x040a…0x041a` (each must pass `valid_lang` + name lookup; system language not duplicated). No valid locales → disable source/enable/language controls. Current `lang_id` not in list → stop, set to first listed, start again if it was running. | state |
| S-087-R31 | `captions.cpp` | save/load | Key `"captions"`: `source` (string), `enabled` (bool = handler live), `lang_id` (int, default current UI language), `provider` (string, default `"mssapi"`). Load: stop, restore, start iff `enabled`. EXIT → free. | config |

### 3.5 Scripts UI (`scripts.cpp`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-087-R32 | `scripts.cpp` | preload (collection load) | **Preload** callback: replace script data with one `obs_script_create(path, settings)` per `{"path","settings"}` entry of the `"scripts-tool"` array. Save callback writes the same array from `obs_script_save(script)`. | scripts |
| S-087-R33 | `scripts.cpp` | add/remove | Add: file dialog filtered by `obs_scripting_supported_formats()`; initial dir `obs_module_file("scripts")`; already-open paths (by `obs_script_get_path`) skipped; new script: apply its saved settings to its properties, select it. Remove: destroy selected scripts, refresh list. | scripts |
| S-087-R34 | `scripts.cpp` | reload / defaults | Reload selected: `obs_script_reload` + re-apply `obs_script_get_settings` via `obs_properties_apply_settings`. Defaults (selected): `obs_data_clear(settings)` + `obs_script_update(script, nullptr)` (re-reads defaults). | scripts |
| S-087-R35 | `scripts.cpp` | script selected | Properties pane = `OBSPropertiesView(settings, script, obs_script_get_properties, obs_script_update)` with `SetDeferrable(false)` (settings apply immediately, S-076 R-defer). Description label from `obs_script_get_description`; http(s) links in it open only after a confirm dialog. | properties pane |
| S-087-R36 | `scripts.cpp` | scripting log callback | Messages rendered `"[<script file>] <msg>"` or `"[Unknown Script] <msg>"` (null script) into the log window (queued to UI thread). Window: read-only, no wrap; auto-scrolls only while the vertical scrollbar is at maximum; `log_level <= LOG_WARNING` → `show()` + `raise()`. Window geometry persisted in user.ini `[ScriptLogWindow] geometry` (base64 of `saveGeometry`). | log window |
| S-087-R37 | `scripts.cpp` | events | `OBS_FRONTEND_EVENT_SCENE_COLLECTION_CLEANUP`: hide+clear log window, reset script data (scripts re-created by the preload callback). EXIT: destroy windows + data. `FreeScripts` = `obs_scripting_unload()`. | state |
| S-087-R38 | `scripts.cpp` | Python tab | Shown only when Python scripting is compiled **and** platform is W or M. Python path in user.ini `[Python] "Path64bit"`/`"Path32bit"` (per architecture); browse writes it back. Version label from `obs_scripting_python_version` when loaded. | config |
| S-087-R39 | `scripts.cpp` | window state | Last selected script row persisted in user.ini `[scripts-tool] prevScriptRow`. | config |

## 4. Reference defects and quirks

- **D-1** (`tool-helpers.hpp`, `GetWeakSourceByName`): `obs_source_get_weak_source()` takes a
  weak ref which is **immediately released** (`obs_weak_source_release(weak)`), so the stored
  `OBSWeakSource` is *unowned* — kept alive only by the source's own base reference. When the
  scene is destroyed while the switcher holds it, the switcher later dereferences freed
  memory (`WeakSourceValid`, `GetWeakSourceName`, `Prune`). Required conforming behaviour:
  hold a genuinely referenced weak source (Rust: `Weak<Scene>`); pruning on drop is then safe
  and must not crash. (Contrast: `UpdateNonMatchingScene` keeps its ref via
  `OBSWeakSourceAutoRelease` — that one is owned.)
- **Q-1** (`output-timer.cpp`): button click while streaming *and not deferred* stops the
  stream — the button doubles as stop control. Timer expiry also only stops.
- **Q-2** (`auto-scene-switcher-osx.mm`): matching on macOS is per-application, not
  per-window (no window-title API used).
- **Q-3** (`auto-scene-switcher.cpp`): the thread re-evaluates only when the title *changed*;
  a stale matching title keeps the scene as-is even if the user never foregrounded a new window.

## 5. Concurrency (summary)

- Switcher: one worker thread, all state under one mutex + condvar; dialog mutates under the
  same mutex; weak-ref resolution may block briefly.
- Output-timer: UI-thread only (Qt timers); no extra threads.
- Captions: MSS worker thread + audio capture callback on the audio thread; stream push guarded
  by a mutex + condvar (R24); handler start/stop from UI thread.
- Scripts: log messages arrive from script threads and are queued to the UI thread.

## 6. Open questions

1. Captions COM surface (R26–R29) is large; confirm whether the Rust rewrite keeps the MSS API
   as the only provider behind the same `captions_handler` trait, or ships without captions
   initially (platform exclusion already pinned by R22 either way).
2. Whether `obs-scripting` (engine, Python/Lua) has its own spec in this plan — S-087 pins only
   the UI/bridge; if the engine spec does not exist yet, R32–R38 assume an equivalent
   `obs_script_*` API set (create/update/reload/save/get_settings/get_properties/log).
3. Output-timer: does the rewrite keep the "hidden dialog defers until next click" flow (R16)
   or change it to start on the triggering click? Behaviour as written is the 33.0.1 one.

Round 10 second-author check (2026-11-05): corrected R03 (dialog creation timing — switcher/captions lazy per trigger, output-timer dialog created at init, scripts tool window on first trigger; not all "lazily on trigger"). Re-derived R07/R08/R10/R13/R18/R21/R22/R35 — all correct; R11/R12/R14/R15/R16 also verified during the pass.
