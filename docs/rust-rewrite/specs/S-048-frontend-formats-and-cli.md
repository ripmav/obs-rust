# S-048 Frontend: CLI, configuration files, profile / scene-collection formats, frontend API, output file names

| | |
|---|---|
| Status | Draft (Tier C behavioural catalog; executable part covers only pure logic) |
| Spec version | initial |
| Reference | OBS Studio frontend 33.0.1 (C++/Qt): `obs-main.cpp`, `OBSApp.cpp`, `OBSStudioAPI.cpp`, `api/obs-frontend-api.{h,cpp}`, `widgets/OBSBasic*.cpp`, `models/SceneCollection.hpp` |
| Target crate | `crates/frontend-core` (non-GUI part of the frontend) |
| Depends on | S-007 (config file), S-014 (obs_data JSON), S-023 (`os_generate_formatted_filename`, UTF-8) |
| Conformance | `test/conformance/scripts/frontendfmt/` (7 scripts) |

## 1. Scope

In scope: command-line flags and their effect on startup state; the three INI files (`global.ini`,
`user.ini`, per-profile `basic.ini`): location, defaults, migrations; profile directory layout and
profile import/export; scene-collection JSON layout, naming, versioning and load rules; the
frontend API (`obs_frontend_*`: 99 functions, 44 events) as seen by plugins; recording and replay
file-name formatting and collision handling; the pure parts of the first-run/migration decisions.

Out of scope or only described (no executable vectors): all Qt widgets and dialogs, docks, theme
engine, the updater and crash reporter (network and OS specific; Section 9 lists what was not
read), auto-config wizard test logic, importers for other software, OAuth/YouTube/Twitch.

**Executable-vector honesty note.** The frontend does not link without Qt. The runner
`gen/frontendfmt_runner.c` is a C **transcription** of the pure functions (same branches and
constants) driving the **real** `libobs/util/config-file.c`. Vectors therefore pin the behaviour
this spec describes, but they are not produced by the C++ binary itself. Anything that needs Qt
(widget state, dialogs) is specified from source only.

## 2. Conventions

RFC 2119 words. `ver(a,b,c) = (a<<24)|(b<<16)|c` (uint32). `LastVersion` is stored in
`global.ini` `[General]` as an integer of this form. Booleans in INI are `true`/`false`. Platform
columns: L = Linux, W = Windows, M = macOS; vectors are the Linux column.

## 3. Interface

### 3.1 Files and directories (per user, `CONFIG` = platform config dir, or `<install>/config` in portable mode)

| Item | Path | Notes |
|---|---|---|
| global config | `CONFIG/obs-studio/global.ini` | "appConfig". Machine/app-level. |
| user config | `<Locations.Configuration>/obs-studio/user.ini` | "userConfig". UI + session state. |
| profile dir | `<Locations.Profiles>/obs-studio/basic/profiles/<dirname>/` | holds `basic.ini`, optionally `service.json`, `streamEncoder.json`, `recordEncoder.json` |
| scene collection | `<Locations.SceneCollections>/obs-studio/basic/scenes/<file>.json` | plus `<file>.json.bak`, `.json.invalid`, `.json.v1` |
| plugin manager | `<Locations.PluginManagerSettings>/obs-studio/plugin_manager` | |
| created at start | `obs-studio/{basic,logs,profiler_data,updates,plugin_config}` (+`crashes` on W) | creation failure is fatal ("Failed to create directory") |

### 3.2 Command-line flags (Section 5.1)

`--multi/-m`, `--portable/-p` (only where portable is compiled in: W always, others only when
built with `ENABLE_PORTABLE_CONFIG`), `--verbose`, `--safe-mode`, `--only-bundled-plugins`,
`--always-on-top`, `--unfiltered_log`, `--startstreaming`, `--startrecording`,
`--startreplaybuffer`, `--startvirtualcam`, `--collection NAME`, `--profile NAME`, `--scene NAME`,
`--minimize-to-tray`, `--studio-mode`, `--allow-opengl`, `--disable-updater`,
`--disable-missing-files-check`, `--steam`, `--help/-h`, `--version/-V`.

### 3.3 Frontend API (Section 5.9 has the catalog)

C ABI exported by the frontend library: 99 functions, enum `obs_frontend_event` (44 values, 0..43),
structs `obs_frontend_source_list { DARRAY(obs_source_t*) sources }` and
`obs_frontend_canvas_list { DARRAY(obs_canvas_t*) canvases }` with inline `*_free` helpers that
release every element then free the array.

## 4. Data formats

### 4.1 `global.ini` (appConfig)

Keys written by the program: `[General] LastVersion` (int, set at every shutdown/start),
`Pre31Migrated` (bool), `Pre33.0Migrated` (bool), `MaxLogs`, `InfoIncrement`, `ProcessPriority`,
`EnableAutoUpdates`, `BrowserHWAccel`, `[Video] Renderer`, `[Locations] Configuration|SceneCollections|Profiles|PluginManagerSettings`.
Defaults (set after migration, via `config_set_default_*`; vector `config_defaults`):

| Section.Key | Type | Default |
|---|---|---|
| General.MaxLogs | uint | 10 |
| General.InfoIncrement | int | -1 |
| General.ProcessPriority | string | `Normal` |
| General.EnableAutoUpdates | bool | true |
| Video.Renderer | string | W `Direct3D 11`, L/M `OpenGL` |
| General.BrowserHWAccel | bool | true on W/M/L |
| Audio.DisableAudioDucking | bool | W only: true |
| Video.DisableOSXVSync, Video.ResetOSXVSyncOnExit | bool | M only: true |
| Locations.{Configuration,SceneCollections,Profiles,PluginManagerSettings} | string | the platform config dir (`os_get_config_path(NULL)`) |

### 4.2 `user.ini` (userConfig) defaults (vector `config_defaults`, section "defaults user")

General.ConfirmOnExit=true; General.HotkeyFocusType=`NeverDisableHotkeys`;
BasicWindow: PreviewEnabled=true, PreviewProgramMode=false, SceneDuplicationMode=true,
SwapScenesMode=true, SnappingEnabled=true, ScreenSnapping=true, SourceSnapping=true,
CenterSnapping=false, SnapDistance=5.0 (double), SpacingHelpersEnabled=true, RecordWhenStreaming=false,
KeepRecordingWhenStreamStops=false, SysTrayEnabled=true, SysTrayWhenStarted=false, SaveProjectors=false,
ShowTransitions=true, ShowListboxToolbars=true, ShowStatusBar=true, ShowSourceIcons=true,
ShowContextToolbars=true, StudioModeLabels=true, SideDocks=true, VerticalVolumeControl=true,
MultiviewMouseSwitch=true, MultiviewDrawNames=true, MultiviewDrawAreas=true,
MediaControlsCountdownTimer=true, MixerShowInactive=false, MixerKeepInactiveLast=false,
MixerShowHidden=false, MixerKeepHiddenLast=false; Appearance.FontScale=10 (int), Appearance.Density=1 (int).
Session keys with no default (absent until first written): `[Basic] Profile`, `ProfileDir`,
`SceneCollection`, `SceneCollectionFile`, `ConfigOnNewProfile`; `[General] FirstRun`, `Pre19Defaults`,
`Pre21Defaults`, `Pre23Defaults`, `Pre24.1Defaults`, `ResetDockLock23`, `DisableHotkeysInFocus` (legacy),
`[BasicWindow] MultiviewLayout`, `DocksLocked`, window geometry blobs.
`MultiviewLayout` is an int enum: 0 HORIZONTAL_TOP_8, 1 HORIZONTAL_BOTTOM_8, 2 VERTICAL_LEFT_8,
3 VERTICAL_RIGHT_8, 4 HORIZONTAL_TOP_24, 5 HORIZONTAL_TOP_18, 6 SCENES_ONLY_4, 7 _9, 8 _16, 9 _25.

### 4.3 `basic.ini` (profile) defaults (vector `config_defaults`, section "defaults basic 1920 1080")

Linux column; `$HOME` stands for `getenv("HOME")` (W: the Videos folder; M: `~/Movies`), `CONTAINER` = `hybrid_mp4`
on W/L, `hybrid_mov` on M. Full table (type, value):

- General: `Name` (string, written on activation = profile display name; Section 5.3).
- Output: Mode `Simple`; FilenameFormatting `%CCYY-%MM-%DD %hh-%mm-%ss`; DelayEnable false; DelaySec 20; DelayPreserve true;
  Reconnect true; RetryDelay 2; MaxRetries 25; BindIP `default`; IPFamily `IPv4+IPv6`; NewSocketLoopEnable false; LowLatencyEnable false.
- Stream1: IgnoreRecommended false; EnableMultitrackVideo false; MultitrackVideoMaximumAggregateBitrateAuto true; MultitrackVideoMaximumVideoTracksAuto true.
- SimpleOutput: FilePath `$HOME`; RecFormat2 CONTAINER; VBitrate 6000; ABitrate 160; UseAdvanced false; Preset `veryfast`; NVENCPreset2 `p5`;
  RecQuality `Stream`; RecRB false; RecRBTime 20; RecRBSize 512; RecRBPrefix `Replay`; StreamAudioEncoder `aac`; RecAudioEncoder `aac`; RecTracks 1.
  Set later (`InitBasicConfigDefaults2`, needs the encoder list): StreamEncoder and RecEncoder = `x264`, or the NVENC id when encoder
  `ffmpeg_nvenc` is available **and** `user.General.Pre23Defaults` is false.
- AdvOut: ApplyServiceSettings true; UseRescale false; TrackIndex 1; VodTrackIndex 2; Encoder `obs_x264`; RecType `Standard`; RecFilePath `$HOME`;
  RecFormat2 CONTAINER; RecUseRescale false; RecTracks 1; RecEncoder `none`; FLVTrack 1; StreamMultiTrackAudioMixes 1; FFOutputToFile true;
  FFFilePath `$HOME`; FFExtension `mp4`; FFVBitrate 6000; FFVGOPSize 250; FFUseRescale false; FFIgnoreCompat false; FFABitrate 160; FFAudioMixes 1;
  Track1Bitrate..Track6Bitrate 160; RecSplitFileTime 15; RecSplitFileSize 2048; RecRB false; RecRBTime 20; RecRBSize 512.
  `AudioEncoder` and `RecAudioEncoder` default to `ffmpeg_aac`, replaced by `CoreAudio_AAC` if available, else `libfdk_aac` if available.
- Video: BaseCX/BaseCY and OutputCX/OutputCY (Section 5.4); FPSType 0; FPSCommon `30`; FPSInt 30; FPSNum 30; FPSDen 1; ScaleType `bicubic`;
  ColorFormat `NV12`; ColorSpace `709`; ColorRange `Partial`; SdrWhiteLevel 300; HdrNominalPeakLevel 1000.
- Audio: MonitoringDeviceId `default`; MonitoringDeviceName = localized "Default"; SampleRate 48000; ChannelSetup `Stereo`;
  MeterDecayRate 23.53 (double); PeakMeterType 0.

### 4.4 Scene collection JSON (`<file>.json`, written pretty-printed with temp ext `tmp`, backup ext `bak`)

Top-level object (keys in the order the reference sets them; JSON object order is not significant):

| Key | Type | Meaning |
|---|---|---|
| `name` | string | collection display name (config `Basic.SceneCollection`) |
| `DesktopAudioDevice1`, `DesktopAudioDevice2`, `AuxAudioDevice1..4` | object | saved source (S-040 `obs_save_source`) bound to output channels 1..6 in this order; key omitted if the channel is empty |
| `sources` | array | all non-group non-scene sources except the six global audio sources, then all scenes |
| `groups` | array | groups (kept separate so pre-22 versions ignore them). On load `groups` is appended to `sources`; if `sources` is absent, `groups` is used alone |
| `scene_order` | array of `{ "name": string }` | UI order of scenes |
| `current_scene` | string | selected (preview) scene name |
| `current_program_scene` | string | program scene name (equals `current_scene` outside studio mode) |
| `canvases` | array | extra canvases (non-ephemeral only) |
| `current_transition` | string | name of the transition in output channel 0 |
| `transition_duration` | int ms | missing or 0 on load means 300 |
| `transitions` | array of `{name,id,settings}` | user transitions (+ hotkeys per entry) |
| `quick_transitions` | array of `{name,duration,hotkeys,id,fade_to_black}` | |
| `saved_projectors` | array | |
| `preview_locked`, `scaling_enabled` | bool | |
| `scaling_level` | int | |
| `scaling_off_x`, `scaling_off_y` | double | |
| `virtual-camera` | object | only when virtual camera enabled: `type2` int, plus `scene` (SceneOutput) or `source` (SourceOutput) |
| `modules` | object | plugin data from save callbacks (Section 5.9), preserved verbatim for plugins that are not loaded |
| `resolution` | `{x,y}` int | canvas size at save time (only if known) |
| `migration_resolution` | `{x,y}` int | present only when coordinate mode is Relative and the migration size is non-zero |
| `version` | int | 1 = absolute coordinates (legacy), 2 = relative coordinates (current; written 2 for Relative, 1 for Absolute) |

Companion files in a profile dir: `service.json`, `streamEncoder.json`, `recordEncoder.json` (obs_data JSON).

## 5. Behaviour (numbered rules)

### 5.1 Command line (vector `cli_flags`)

- **S-048-R01** Arguments are scanned left to right, index 1..argc-1, once, before anything else (before logging exists). Matching is exact and case sensitive
  (`--Multi`, `-M`, `--multi=1`, `--verbos` are ignored, no error is printed). Unknown arguments are skipped silently.
- **S-048-R02** Boolean flags set a state variable true; repeating is harmless. Mapping: `--multi/-m` multi (suppress the already-running warning);
  `--portable/-p` portable mode (see R05); `--verbose` verbose log; `--safe-mode` safe mode; `--only-bundled-plugins` disable third-party plugins;
  `--always-on-top`; `--unfiltered_log`; `--startstreaming`, `--startrecording`, `--startreplaybuffer`, `--startvirtualcam` start that output after load;
  `--minimize-to-tray`; `--studio-mode`; `--allow-opengl`; `--disable-updater`; `--disable-missing-files-check`; `--steam`.
- **S-048-R03** `--collection`, `--profile`, `--scene` take the **next** argument verbatim (even if it begins with `--`; that argument is consumed and not
  interpreted). If the flag is the last argument it is ignored. The last occurrence wins. Empty string = "not given".
- **S-048-R04** `--help/-h` prints the help text and exits the process with status 0 at the moment it is seen (flags before it have no effect; on Windows the
  text is shown in a message box, elsewhere on stdout followed by the `--version, -V` line). `--version/-V` prints `OBS Studio - <version string>` and exits 0 at once.
  A value flag swallowing `--help` as its argument prevents the exit (R03).
- **S-048-R05** Portable mode is on if `--portable`/`-p` was given (only on builds that allow it) **or** one of the files `portable_mode`, `obs_portable_mode`,
  `portable_mode.txt`, `obs_portable_mode.txt` exists in the directory `BASE_PATH` relative to the executable (`..` on L/M, `../..` on W). Likewise
  `--disable-updater` is implied by a file `disable_updater[.txt]` and `--disable-missing-files-check` by `disable_missing_files_check[.txt]` in the same directory.
  On builds without portable support `--portable/-p` is just an unknown argument.
- **S-048-R06** In portable mode the config root is `<BASE_PATH>/config` and the four `Locations.*` values are ignored (defaults used); otherwise each
  `Locations.*` value is used if the directory exists, else its default (R12).
- **S-048-R07** If more than one argument is present, the log gets one line `Command Line Arguments: <argv[1..] joined by single spaces>` (before `OBSInit`).
- **S-048-R08** Effects at run time: `--scene` overrides the program scene name on collection load, and also the preview scene unless studio mode is active (R55);
  `--profile` selects the profile by display name if it exists (else falls back to `Basic.Profile`, else creates one with that name); `--collection` likewise for scene
  collections (Section 5.3). `--safe-mode`, `--only-bundled-plugins` limit module loading (S-035). Restart after a crash-recovery safe-mode prompt re-launches with the
  original arguments (+ `--safe-mode` when chosen). Signals SIGINT/SIGTERM/SIGHUP/SIGQUIT/SIGABRT are routed to an orderly close on non-Windows, SIGPIPE is blocked.

### 5.2 Global / user config (vectors `migrate_user`, `config_defaults`)

- **S-048-R10** Startup order: open `global.ini` (create if absent; failure is fatal "Failed to open global.ini: <code>"); read `LastVersion`; run the Pre31 migration
  (R11); set global defaults (Section 4.1) and location defaults; decide the four locations (R06); open `user.ini` at `<Locations.Configuration>/obs-studio/user.ini`
  (create if absent; failure fatal); `MigrateLegacySettings(LastVersion)` (R13..R17); then user defaults (Section 4.2). Migrations see raw user values only (defaults are set later).
- **S-048-R11** Pre31 split: if `LastVersion != 0` and `LastVersion < ver(31,0,0)` and `General.Pre31Migrated` is false (absent counts as false): try to copy `global.ini`
  to `user.ini` unless `user.ini` already exists (then fail; the failure shows an error box and does not abort start-up); write `General.Pre31Migrated = <copy succeeded>`
  into global.ini and save. A false flag therefore **retries** on the next start (**Q-1**).
- **S-048-R12** Location defaults: all four equal `os_get_config_path(NULL)`; if that is not available start-up fails with "Unable to get global configuration path.".
- **S-048-R13** `MigrateLegacySettings`: for the four keys `Pre19Defaults` (< ver(19,0,0)), `Pre21Defaults` (< ver(21,0,0)), `Pre23Defaults` (< ver(23,0,0)),
  `Pre24.1Defaults` (< ver(24,1,0)) in `user.ini[General]`: if the key has no user value, set it to `LastVersion != 0 && LastVersion < threshold` (fresh install, `LastVersion` = 0, gives false)
  and mark user config dirty. Existing values are never overwritten.
- **S-048-R14** If `user.ini[BasicWindow] MultiviewLayout` has a user value and, compared case-insensitively, equals `horizontaltop`/`horizontalbottom`/`verticalleft`/`verticalright`,
  it is rewritten to the int 0/1/2/3 and the config is marked dirty. Any other value (including already-numeric) is untouched.
- **S-048-R15** If `LastVersion != 0 && LastVersion < ver(24,0,0)`: the config is marked dirty unconditionally; if legacy `General.DisableHotkeysInFocus` is true then
  `General.HotkeyFocusType = "DisableHotkeysInFocus"`.
- **S-048-R16** If `LastVersion != 0 && LastVersion < ver(33,0,0)` and `global.ini[General] Pre33.0Migrated` has no user value: read `BasicWindow.SnapDistance` as double (0 if absent);
  if it equals exactly 10.0 set it to 5.0; set `Pre33.0Migrated = true`; mark **both** configs dirty. (A custom value, or an absent key, is kept; absent stays absent and takes the default 5.)
- **S-048-R17** Dirty configs are saved with `config_save_safe(..., temp_ext="tmp", backup_ext=none)`. Note the user config is dirty on every run with `LastVersion < ver(24,0,0)` and on every first run (R13 sets keys).
- **S-048-R18** `LastVersion` itself is updated by the shutdown path (not specified here; open question O-3).

### 5.3 Profiles (vector `names`, section `names.script`)

- **S-048-R20** A profile is a directory under `<Profiles>/obs-studio/basic/profiles/` containing `basic.ini`. Enumeration scans subdirectories, skips entries that are not
  directories or whose `basic.ini` cannot be opened (`CONFIG_OPEN_EXISTING`), and names the profile `basic.ini[General] Name` if present (any string), else the directory name.
  The cache is keyed by display name; on a duplicate display name the first one (directory iteration order) wins (`try_emplace`). The list shown to users and returned by `obs_frontend_get_profiles` is ordered by the map (byte-wise ascending name).
- **S-048-R21** Directory name for a new profile = `GetFileSafeName(name)` (R40) then `GetClosestUnusedFileName(dir, no extension)` (R41). Creating a profile whose
  display name already exists throws "Profile already exists". An empty-after-sanitising name becomes `characters_only`.
- **S-048-R22** Activation: open/create `<dir>/basic.ini`, write `General.Name = <display name>`, save (temp ext `tmp`). If a profile was active, its auth state is saved and its config saved.
  Restart-requirement check (R23). `user.ini[Basic] Profile` = display name and `ProfileDir` = directory name are written and saved. Basic migrations (R30) and defaults (Section 4.3) run.
  Events fired: `PROFILE_CHANGING` before, `PROFILE_CHANGED` after (and `PROFILE_LIST_CHANGED` on create/remove, `PROFILE_RENAMED` on rename).
- **S-048-R23** Restart requirement when switching: compare old and new `Audio.ChannelSetup` (strings, only if both non-NULL) and `Audio.SampleRate` (uint, only if both non-zero);
  each difference adds a localized line; if any, the user is asked to restart.
- **S-048-R24** Startup profile choice: `--profile` name if it matches a known profile; else `user.ini[Basic] Profile` if known; else a new profile named by `Basic.Profile` is created
  (empty name allowed to fall through to creation; see O-4). Failure to open `basic.ini` is fatal ("Failed to open basic.ini").
- **S-048-R25** Duplicate: new profile created as in R21, then **all files** of the current profile directory are copied recursively into it (skip existing), then activated without reset.
  Rename: same as duplicate into the new name, then the old profile is removed.
- **S-048-R26** Export copies the files `basic.ini`, `service.json`, `streamEncoder.json`, `recordEncoder.json` (those that exist; overwrite) into `<chosen dir>/<profile dirname>/`.
  Import takes a directory: its **directory name** must not match an existing profile's directory name (else "exists", abort); `basic.ini` is mandatory (missing aborts and leaves an empty new directory);
  the other three are optional; files are copied under the same directory name (no sanitising, no `-2` suffix); then the profile cache is refreshed.
  The display name afterwards comes from the imported `basic.ini[General] Name`, else the directory name.

### 5.4 Basic config migrations and derived defaults (vectors `migrate_basic`, `resolution_defaults`)

All migrations run on every activation, before defaults, and save the profile (`tmp`) if any set `changed`.

- **S-048-R30** (a) `AdvOut.FFAudioTrack` has a user value and `AdvOut.Pre22.1Settings` does not: `AdvOut.FFAudioMixes = 1 << (FFAudioTrack-1)`, `Pre22.1Settings = true`, changed.
  (b) `AdvOut.RecTrackIndex` present and `AdvOut.RecTracks` absent: `RecTracks = 1 << (RecTrackIndex-1)` (uint), `RecTrackIndex` removed, changed. Both present: nothing happens (index kept).
  (c) `user.General.Pre24.1Defaults` true and `Twitch.AddonChoice` absent: `Twitch.AddonChoice = 3`, changed.
  (d) `SimpleOutput.EnforceBitrate` present, `Stream1.IgnoreRecommended` absent and `Stream1.MovedOldEnforce` absent: `IgnoreRecommended = !EnforceBitrate`, `MovedOldEnforce = true`, changed.
  (e) `Output.RetryDelay` has a user value and `(int)uint value < 1`: set to 1, changed.
  (f) Container key: for each section `AdvOut`, `SimpleOutput` (in that order): if neither `RecFormat` nor `RecFormat2` has a user value, nothing. Else take the value of `RecFormat2` if it has one, else `RecFormat`;
  map `ts`->`mpegts`, `m3u8`->`hls`, `fmp4`->`fragmented_mp4`, `fmov`->`fragmented_mov` (anything else unchanged); if the mapped value differs **or** `RecFormat2` had no user value, write `RecFormat2` and mark changed. The old `RecFormat` is left in place.
  (g) `AdvOut.Rescale` true and `AdvOut.RescaleFilter` absent: `RescaleFilter = 3` (bilinear); same for `RecRescale`/`RecRescaleFilter`. These two do **not** set `changed` (**Q-2**: the value is saved only if something else saved).
- **S-048-R31** Resolution defaults. Let `(cx,cy)` = primary monitor size in device pixels. If `user.General.Pre19Defaults` is false and `cx*cy > 1920*1080` then `(cx,cy) = (1920,1080)`.
  Defaults `Video.BaseCX/BaseCY = (cx,cy)`. Output: `s = (cx,cy)`; for scale in `[1, 1.25, 1/0.75, 1.5, 1/0.6, 1.75, 2, 2.25, 2.5, 2.75, 3]` in order, while `s.x*s.y > 1280*720`: `s = (trunc(cx/scale), trunc(cy/scale))` (uint32 truncation of the double quotient, evaluated from the original cx,cy).
  `Video.OutputCX/OutputCY = s`. If `BaseCX` or `BaseCY` has no user value, **both** are written as user values (so later default changes do not affect existing profiles) and the profile saved; the same rule applies to `OutputCX/CY`. Vectors: 1366x768 -> out 1092x614; 2560x1440 (pre-19) -> base 2560x1440 out 1280x720.
- **S-048-R32** Scene-collection load: see R50..R56.

### 5.5 Names and paths (vectors `names`, `recording_names`)

- **S-048-R40** `GetFileSafeName(name)`: convert UTF-8 to wide chars (a failing conversion, i.e. empty or invalid input, returns false). For each wide char from the end: whitespace (`iswspace`) becomes `_`;
  else any char that is not `_` and not `iswalnum` is deleted. If the result is empty it becomes `characters_only`. Then back to UTF-8. ASCII behaviour is vector-covered; **non-ASCII alnum retention depends on the C locale** (O-1).
- **S-048-R41** `GetClosestUnusedFileName(path, ext)`: `p = path (+ "." + ext if ext)`. If it does not exist, done. Else for `n = 2, 3, ...`: `p = path + decimal(n) (+ "." + ext)`, return the first non-existing. No separator before the number; with a `NULL` extension the digits are appended to the bare path. Always returns true.
- **S-048-R42** `GetFormatString(format, prefix, suffix)`: start with `format`. If `prefix` non-empty: append a space to it unless it already ends with one; insert it at position 0 or, if `format` contains `/` or `\`, just after the **last** of either separator. If `suffix` non-empty: append `suffix`, preceded by a single space unless the suffix already begins with one. Finally replace `\`->`/` and each of `* ? " | : > <` with `_`. (Applied to the user template `Output.FilenameFormatting` before `%`-expansion; replay buffer passes prefix `RecRBPrefix`, default `Replay`.)
- **S-048-R43** `GetFormatExt(container)`: `fragmented_mp4|hybrid_mp4`->`mp4`; `fragmented_mov|hybrid_mov`->`mov`; `hls`->`m3u8`; `mpegts`->`ts`; everything else (including `mkv`, `flv`, `fmp4`, wrong case) unchanged.
- **S-048-R44** `GetOutputFilename(dir, container, noSpace, overwrite, format)`: if `dir` is empty or cannot be opened as a directory, show a warning (message box if the window is visible else a tray notification "Output.BadPath") and return the empty string.
  Else `path = dir`, plus `/` unless the last char is `/` or `\`; then `GenerateSpecifiedFilename(ext, noSpace, format)` = `os_generate_formatted_filename(ext, space=!noSpace, format)` (S-023: `%`-expansion, the extension is appended, `noSpace` makes spaces `_`); then every `\` in the whole path becomes `/` and the parent directory of the result (everything before the last `/`) is created with `os_mkdirs` (errors ignored, so a format containing `/` makes sub-folders).
  If `overwrite` is false and the file exists, `FindBestFilename` (R45).
- **S-048-R45** `FindBestFilename(path, noSpace)`: if the file does not exist or `path` has no `.` (searching from the end, so a dot in a directory counts), return unchanged. Else insert before the **last** `.`: `_N` when `noSpace`, ` (N)` otherwise, for `N = 2, 3, ...` until a free name.
- **S-048-R46** The legacy `GenerateTimeDateFilename` produces `YYYY-MM-DD<sep>HH-MM-SS.<ext>` (`%d-%02d-%02d%c%02d-%02d-%02d.%s`, sep is `_` if noSpace else space) from local time; used for screenshots.
- **S-048-R47** Scene-collection file name: `GetFileSafeName(name)` then `GetClosestUnusedFileName(<dir>/<safe>, "json")` (so `Untitled.json`, `Untitled2.json`, ...). Enumeration (R50) accepts only regular files with extension exactly `.json`.

### 5.6 Scene collections (no executable vectors for JSON content; name logic above)

- **S-048-R50** Enumeration: every non-directory entry ending `.json`; read with `obs_data_create_from_json_file_safe(path, "bak")` (falls back to `<path>.bak` when the main file is unreadable); display name = top-level `name` if non-empty, else the file stem. Map keyed by display name (first wins).
- **S-048-R51** Load order of operations: clear the old scene data (event `SCENE_COLLECTION_CLEANUP`), reinitialise default transitions, give plugins `preload` callbacks with `modules`, then load: six global audio devices, canvases, sources (with groups appended), scene order, transitions, quick transitions, projectors, virtual camera, preview state, then select scenes.
- **S-048-R52** Missing or unreadable file: the file (if it exists) is renamed `<stem>.json.invalid` (rename errors logged), the log says "No scene file found, creating default scene" and a default scene is created (with the first-run flow when `user.General.FirstRun` is false/absent).
- **S-048-R53** Defaults on load: `transition_duration` 0/absent -> 300; `current_transition` absent -> the built-in Fade; collection name: top-level `name` default is the configured `Basic.SceneCollection`, empty also falls back to it.
- **S-048-R54** Version/coordinate migration: `version` (int, absent = 0). If `version < 2`: if a `resolution {x,y}` exists and is non-zero, the collection is converted to relative coordinates: the canvas is temporarily reset to that resolution; if that fails the collection stays absolute. When the final mode is absolute, a one-time backup `<stem>.json.v1` of the original data is written (not overwritten if it exists) before conversion. If `version < 2` and no resolution is saved, the collection is treated as absolute. If `version >= 2` the optional `migration_resolution {x,y}` is read.
  The saved `version` is `2` for relative, `1` for absolute coordinate mode.
- **S-048-R55** `--scene NAME` replaces `current_program_scene`, and `current_scene` too unless studio mode is active. Names that don't exist fall back to the first scene.
- **S-048-R56** Saving: only when saving is not deferred (counter `disableSaving == 0`); written with `obs_data_save_json_pretty_safe(data, path, "tmp", "bak")`; failure logs "Could not save scene data to <path>". `DeferSaveBegin/End` increments/decrements the counter; reaching 0 triggers a save. User config keys `Basic.SceneCollection` (display name) and `Basic.SceneCollectionFile` (file name) are set on activation.
- **S-048-R57** Import / rename / create / delete / export of collections operate on the JSON file: rename rewrites top-level `name` and renames by R47; duplicate/import regenerate each source's `uuid` (`sources[*].uuid`) so that duplicates don't collide; sources inside the file are sorted by name (case-insensitive) when exporting via the dialog.

### 5.7 Streaming/recording state used by the API (no vectors)

- **S-048-R60** The active flags (`streaming_active`, `recording_active`, `recording_paused`, `replaybuf_active`, `virtualcam_active`) are atomic booleans updated by the UI thread when the corresponding `*_STARTED/STOPPED/PAUSED/UNPAUSED` events fire; `obs_frontend_*_active()` read them without blocking.
- **S-048-R61** `recording_split_file()` returns false unless recording is active and not paused; otherwise it calls the output's `split_file` proc and returns its `split_file_enabled` result. `recording_add_chapter(name)` returns false unless recording is active and not paused; else the result of the output's `add_chapter` proc (`chapter_name` set).

### 5.8 Output name / container notes

- **S-048-R65** Replay buffer file names use prefix `SimpleOutput.RecRBPrefix` (default `Replay`) through R42; recording uses `Output.FilenameFormatting`; unsafe characters are replaced as in R42 after the prefix/suffix join; `FilePath`/`RecFilePath`/`FFFilePath` select the directory by output mode.

### 5.9 Frontend API catalog

**Dispatch.** Every exported function first checks that the callbacks table is installed; if not it logs `Tried to call <name> with no callbacks!` (error) and returns the neutral value (`NULL`/`false`/`0`/nothing; see column "no-cb"). All wrappers are otherwise thin; "UI" = executed on the Qt thread via a blocking or queued invoke.

- **S-048-R70** `convert_string_list` (used by `get_scene_names`, `get_scene_collections`, `get_profiles`): returns one `bmalloc` block: `(n+1)` pointers followed by the NUL-terminated strings; the pointer array ends with `NULL`; an empty list is a block holding only the terminating `NULL` (never `NULL`). Caller frees with `bfree` (single free).
- **S-048-R71** `get_scenes`/`get_transitions`/`get_canvases` push a **new reference** per element into the caller-supplied list (so `*_free` must be used). `get_scene_names` returns names in the frontend scene order. `get_profiles` returns display names (map order); `get_scene_collections` display names.
- **S-048-R72** `get_current_scene`: in studio mode the **program** scene, else the selected scene, with a new reference (NULL possible). `set_current_scene(s)`: studio mode -> transition to `s` (as if by the transition button, blocking); else select it (blocking). `get_current_preview_scene`: studio mode only (else NULL). `set_current_preview_scene`: studio mode only; ignored otherwise.
- **S-048-R73** `set_current_scene_collection(name)` / `set_current_profile(name)`: find the menu entry whose text equals `name` exactly and trigger it; no match = silent no-op. `add_scene_collection(name)`: blocking; returns whether creation succeeded (false if a collection with that name exists). `create_profile/duplicate_profile/delete_profile(name)`: queued, no result; failures are logged by the UI. `get_current_scene_collection`/`get_current_profile`/`get_current_profile_path`: `bstrdup` strings (NULL + error log if the lookup throws); path is the profile directory (UTF-8).
- **S-048-R74** `add_event_callback/remove_event_callback`, `add_save_callback/remove_save_callback`, `add_preload_callback/remove_preload_callback`: a (callback, private_data) pair is added only if not already present (duplicate add ignored) and removed by exact pair (absent = no-op). Event dispatch: **reverse registration order**, over a copy of each entry; all events except `SCENE_COLLECTION_CLEANUP` and `EXIT` are **suppressed while saving is deferred or a collection is loading** (`disableSaving != 0`). `on_save`/`on_preload` call callbacks in reverse registration order with `saving = true` / `false` and the collection's `modules` object (shared across the callbacks).
- **S-048-R75** `add_dock_by_id(id,title,widget)`: if a dock with that object name exists log `Dock id '<id>' already used!  Duplicate library?` and return false; else create a floating, initially hidden dock and return true. `add_custom_qdock(id, dock)` likewise (sets the object name, returns false if used). `remove_dock(id)` removes by id. `add_tools_menu_qaction(name)` enables the Tools menu and returns the new `QAction*`; `add_tools_menu_item(name, cb, data)` adds an action that calls `cb(data)`.
- **S-048-R76** `open_projector(type, monitor, geometry, name)`: type compared case-insensitively to `Source`, `Scene`, `StudioProgram`, `Multiview`; anything else (including NULL) = Preview. NULL geometry/name = empty. Blocking.
- **S-048-R77** `get_global_config` is deprecated: logs a warning ("DEPRECATION: ...") and returns the **app** config (`global.ini`). `get_app_config` -> `global.ini`, `get_user_config` -> `user.ini`, `get_profile_config` -> active `basic.ini` (pointers owned by the frontend; never freed by the caller).
- **S-048-R78** `get_streaming_output`: the multitrack-video output if one exists and is running, else the normal stream output; `get_recording_output`, `get_replay_buffer_output`, `get_virtualcam_output`: current output or NULL; all return a new reference.
- **S-048-R79** Remaining behaviours: `get_locale_string(s)` -> localized string or `s` itself (pointer owned by the lookup); `get_last_recording/screenshot/replay` -> `bstrdup` of the last path (empty string if none); `get_current_record_output_path` -> `bstrdup` of the directory configured for the active mode; `is_theme_dark`; `reset_video` re-applies video settings; `save` is a no-op while saving is disabled (`disableSaving != 0`), otherwise it marks the project changed and schedules a queued save on the UI thread; `take_screenshot()` = program output, `take_source_screenshot(src)`; `push/pop_ui_translation` stack a translator `bool cb(const char*, const char**)`; `set_streaming_service`/`get_streaming_service`/`save_streaming_service`; `copy/can_paste/paste_sceneitem`; `add_undo_redo_action`; `add_canvas(name, ovi, flags)`/`remove_canvas`; `is_safe_mode_enabled`.

**Event numbering** (`enum obs_frontend_event`, values are ABI): 0 STREAMING_STARTING, 1 STREAMING_STARTED, 2 STREAMING_STOPPING, 3 STREAMING_STOPPED, 4 RECORDING_STARTING, 5 RECORDING_STARTED, 6 RECORDING_STOPPING, 7 RECORDING_STOPPED, 8 SCENE_CHANGED, 9 SCENE_LIST_CHANGED, 10 TRANSITION_CHANGED, 11 TRANSITION_STOPPED, 12 TRANSITION_LIST_CHANGED, 13 SCENE_COLLECTION_CHANGED, 14 SCENE_COLLECTION_LIST_CHANGED, 15 PROFILE_CHANGED, 16 PROFILE_LIST_CHANGED, 17 EXIT, 18 REPLAY_BUFFER_STARTING, 19 _STARTED, 20 _STOPPING, 21 _STOPPED, 22 STUDIO_MODE_ENABLED, 23 STUDIO_MODE_DISABLED, 24 PREVIEW_SCENE_CHANGED, 25 SCENE_COLLECTION_CLEANUP, 26 FINISHED_LOADING, 27 RECORDING_PAUSED, 28 RECORDING_UNPAUSED, 29 TRANSITION_DURATION_CHANGED, 30 REPLAY_BUFFER_SAVED, 31 VIRTUALCAM_STARTED, 32 VIRTUALCAM_STOPPED, 33 TBAR_VALUE_CHANGED, 34 SCENE_COLLECTION_CHANGING, 35 PROFILE_CHANGING, 36 SCRIPTING_SHUTDOWN, 37 PROFILE_RENAMED, 38 SCENE_COLLECTION_RENAMED, 39 THEME_CHANGED, 40 SCREENSHOT_TAKEN, 41 CANVAS_ADDED, 42 CANVAS_REMOVED, 43 REPLAY_BUFFER_SAVING.

Emission points (what triggers each event): STREAMING_*, RECORDING_*, REPLAY_BUFFER_* and VIRTUALCAM_* mirror the corresponding output signal/command (starting/stopping when the request is accepted, started/stopped on the output's signal); REPLAY_BUFFER_SAVING when a save is requested and REPLAY_BUFFER_SAVED when the file is written; RECORDING_PAUSED/UNPAUSED on pause toggles. SCENE_CHANGED when the program scene (studio mode) or selected scene (normal) changes; PREVIEW_SCENE_CHANGED when the preview selection changes in studio mode (also emitted on collection load); SCENE_LIST_CHANGED on add/remove/rename/reorder of scenes; TRANSITION_CHANGED / _LIST_CHANGED / _DURATION_CHANGED / _STOPPED (transition finished) / TBAR_VALUE_CHANGED (t-bar moved); SCENE_COLLECTION_CHANGING before and _CHANGED after a switch, _LIST_CHANGED on create/delete/rename/import, _RENAMED on rename, _CLEANUP while the old scene data is torn down (delivered even while saving is disabled); PROFILE_* analogous (Section 5.3); STUDIO_MODE_ENABLED/DISABLED; FINISHED_LOADING once after start-up load completes; SCRIPTING_SHUTDOWN when scripting is stopped before exit; EXIT at shutdown (delivered even while saving is disabled); THEME_CHANGED after a theme switch; SCREENSHOT_TAKEN after a screenshot file is written; CANVAS_ADDED/REMOVED on canvas add/remove.

**Function catalog** (99). Format: name -> effect; "BL" = blocking UI invoke, "Q" = queued UI invoke, "atomic" = reads atomic flag. No-callbacks result: `NULL` for pointer returns, `false` for bool, `0` for int, no-op for void.

Windows/handles: `get_main_window` (QMainWindow*), `get_main_window_handle` (native handle), `get_system_tray` (QSystemTrayIcon*).
Scenes: `get_scene_names`, `get_scenes(list)`, `get_current_scene`, `set_current_scene` (R70-R72).
Transitions: `get_transitions(list)`, `get_current_transition` (new ref), `set_current_transition(t)`, `get_transition_duration` (ms, int), `set_transition_duration(ms)`, `release_tbar` (end a manual t-bar drag, snapping to the end), `set_tbar_position(0..1023)`, `get_tbar_position`.
Collections: `get_scene_collections`, `get_current_scene_collection`, `set_current_scene_collection`, `add_scene_collection` (R73).
Profiles: `get_profiles`, `get_current_profile`, `get_current_profile_path`, `set_current_profile`, `create_profile`, `duplicate_profile`, `delete_profile`.
UI extension: `add_tools_menu_qaction`, `add_tools_menu_item`, `add_dock_by_id`, `remove_dock`, `add_custom_qdock`.
Callbacks: `add/remove_event_callback`, `add/remove_save_callback`, `add/remove_preload_callback` (R74); `push_ui_translation`, `pop_ui_translation`.
Outputs: `streaming_start/stop/active`, `recording_start/stop/active`, `recording_pause(bool)`, `recording_paused`, `recording_split_file`, `recording_add_chapter`, `replay_buffer_start/save/stop/active`, `start_virtualcam/stop_virtualcam/virtualcam_active`, `get_streaming_output`, `get_recording_output`, `get_replay_buffer_output`, `get_virtualcam_output` (start/stop are queued and have no result; `*_active` are atomic reads).
Config/state: `get_profile_config`, `get_app_config`, `get_user_config`, `get_global_config` (deprecated), `save`, `defer_save_begin`, `defer_save_end` (queued, counter), `open_projector`.
Service: `set_streaming_service`, `get_streaming_service`, `save_streaming_service`.
Studio mode: `preview_program_mode_active`, `set_preview_program_mode(bool)`, `preview_program_trigger_transition`, `set_preview_enabled(bool)`, `preview_enabled`, `get_current_preview_scene`, `set_current_preview_scene`.
Screenshots: `take_screenshot`, `take_source_screenshot`, `get_last_screenshot`.
Misc: `reset_video`, `open_source_properties`, `open_source_filters`, `open_source_interaction`, `open_sceneitem_edit_transform`, `get_current_record_output_path`, `get_locale_string`, `is_theme_dark`, `get_last_recording`, `get_last_replay`, `add_undo_redo_action(name, undo, redo, undo_data, redo_data, repeatable)`, `get_canvases`, `add_canvas`, `remove_canvas`, `copy_sceneitem`, `can_paste_sceneitem(duplicate)`, `paste_sceneitem(scene, duplicate)`, `is_safe_mode_enabled`.
Plus the internal install function `obs_frontend_set_callbacks_internal` (replaces the table; an owning `unique_ptr`, so installing deletes the previous one) and the inline helpers `obs_frontend_source_list_free`, `obs_frontend_canvas_list_free`.

## 6. Algorithms

Resolution default (R31) and safe-name (R40) are given above in prose; pseudocode:

```
default_output(cx, cy):
    s = (cx, cy); i = 0
    while s.x*s.y > 921600 and SCALES[i] > 0:
        k = SCALES[i]; i += 1
        s = (floor(cx / k), floor(cy / k))      # doubles, truncated to uint32
    return s
```

## 7. Concurrency

API functions may be called from any thread; mutating calls marshal to the Qt thread (BL = caller waits, Q = fire and forget). Active-state getters are atomic reads. Event callbacks run on the Qt thread (and during start-up on the main thread) and must not block. Callback tables are only modified on the Qt thread.

## 8. Reference defects and quirks

- **Q-1** (R11) `Pre31Migrated=false` after a failed copy is not a terminal state: the next start retries, and fails again forever if `user.ini` exists. Required: keep as is.
- **Q-2** (R30g) Rescale-filter backfill does not mark the profile changed. Required: do not force a save for that alone.
- **Q-3** (R03) A value flag consumes the next argument even when it looks like a flag; a trailing value flag is silently ignored. Required.
- **Q-4** (R16) The 10.0 -> 5.0 snap distance rewrite is exact floating point equality. Required.
- **Q-5** (R45) `FindBestFilename` uses the last `.` in the whole path, so `/rec.dir/noext` gets ` (2)` inserted inside the directory part. Required to match.
- **Q-6** (R74) Events are dropped while saving is deferred; plugins can miss events during collection load. Required.
- **Q-7** (R26) Profile import keeps the foreign directory name unsanitised and does not suffix on collision with a profile of a different *display* name but the same directory name only (the check is on directory names only, so a display-name clash is possible; the cache then keeps one of them). Required.
- **D-1** (R20) With two profiles that share a display name the visible one depends on directory iteration order (filesystem dependent, nondeterministic across systems). Required: only that exactly one wins; a re-implementation SHOULD iterate in ascending byte order of directory name for determinism.
- **D-2** (R40) For an empty name `os_utf8_to_wcs` returns 0, so `GetFileSafeName("")` returns false (not `characters_only`) and profile creation throws "Failed to create safe directory". Required: empty name -> failure.

## 9. Implementation notes (non-normative)

Treat INI via S-007 and JSON via S-014. Keep the filename functions free of I/O by injecting an `exists(path)` predicate (the runner does). Model `LastVersion` as `u32`. A Rust frontend that is not Qt-based still needs R10-R17, R30-R31, R40-R47, R70-R79 for compatibility with existing user data and plugins.

## 10. Conformance coverage

| Rules | Scripts |
|---|---|
| R01-R04 | cli_flags |
| R05-R08, R12, R18, R20-R26 (non-name parts), R50-R57, R60-R79 | specified from source only (needs Qt or a running frontend) |
| R10, R11 | migrate_user (`pre31` decision), config_defaults |
| R13-R17 | migrate_user |
| R30 | migrate_basic |
| R31 | resolution_defaults, config_defaults (`defaults basic`) |
| R40, R41, R47 | names |
| R42-R45, R65 | recording_names |
| 4.1-4.3 tables | config_defaults |

## 11. Open questions

- **O-1** `GetFileSafeName` uses `iswalnum`/`iswspace`, so non-ASCII letters survive only if the process locale classifies them as alnum; the Qt application sets the locale from the environment. Vectors are ASCII only.
- **O-2** The exact text of the localized messages and the `%`-expansion are outside this spec (S-023 and translations).
- **O-3** Where `LastVersion` is written at shutdown, and the unclean-shutdown / safe-mode prompt logic (`checkForUncleanShutdown`, the sentinel file) were not read.
- **O-4** Whether an empty `Basic.Profile` on a fresh install creates a profile named from the translated "Untitled" string (this is localized) was not traced.
- **O-5** Not covered at all: auto-config wizard decision logic (`wizards/`), updater (`updater/`, `utility/update-helpers.cpp`, `WhatsNewInfoThread`), crash report upload, importers (`importer/`, `importers/`), `SimpleOutput`/`AdvancedOutput` settings-to-encoder mapping, hotkey persistence, scene-collection import/export details beyond R57, YouTube/Twitch/Restream auth storage, OBS_FRONTEND event ordering at start-up. These need further Tier C passes.
- **O-6** The Rust sufficiency check was **not performed** (cost control, as instructed); the vectors have not been validated against an independent implementation.
