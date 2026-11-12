# S-070 Frontend: scene collections, profiles & importers

| | |
|---|---|
| Status | Draft (round 9 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1 frontend: `frontend/widgets/OBSBasic_SceneCollections.cpp`, `frontend/widgets/OBSBasic_Profiles.cpp`, `frontend/importer/*`, `frontend/importers/*` |
| Target crate | `crates/frontend` (non-GUI part: collection/profile file management + importer parsers; dialogs described at behaviour level) |
| Depends on | S-048 (collection JSON layout §4.4, naming R40/R47, enumeration/load/save R50–R56, profile basics R20–R26 — **cross-referenced, not duplicated**), S-007 (config files), S-014 (obs_data JSON) |
| Conformance | planned, none yet |

## 1. Scope

In scope: what S-048 deliberately left out — the dialog-driven lifecycle of scene
collections (new/duplicate/rename/delete/export/import flows, including dedup, rename and
uuid remap) and profiles (create/duplicate/rename/delete/export/import, activation and
restart requirements), plus the third-party importer framework (detection, conversion,
auto-search). Out of scope: the collection JSON layout itself (S-048 §4.4), the profile
directory layout and file list (S-048 §3.1, R20–R26), the first-run/migration pure logic
(S-048 §5.6).

### Reference files (11, full paths relative to `obs-studio/`)

- `frontend/widgets/OBSBasic_SceneCollections.cpp` — collection create/duplicate/rename/delete/export/remigrate, activation, save
- `frontend/widgets/OBSBasic_Profiles.cpp` — profile create/duplicate/rename/delete/export/import, activation
- `frontend/importer/OBSImporter.cpp` — import dialog, auto-search, `GetUnusedName`, `importCollections`
- `frontend/importer/ImporterModel.cpp` — table model (selected/name/path columns)
- `frontend/importer/ImporterEntryPathItemDelegate.cpp` — path cell with browse/clear buttons
- `frontend/importers/importers.cpp` — importer registry, `ImportSC`, `DetectProgram`, `ImportersFindFiles`
- `frontend/importers/importers.hpp` — `Importer` interface, result codes, shared helpers
- `frontend/importers/studio.cpp` — OBS Studio collection import (id remap + path translation)
- `frontend/importers/classic.cpp` — OBS classic `.xconfig` importer
- `frontend/importers/sl.cpp` — StreamLabs `.bpres/.json` importer
- `frontend/importers/xsplit.cpp` — XSplit V2 `.bpres` importer

## 2. Conventions

RFC 2119 words. "Display name" = user-facing name (map key); "file name" = safe-name'd
`<name>.json` on disk (S-048 R47). "Directory name" for profiles is the safe-name'd
directory (S-048 R21). Error propagation style: setup functions throw
`std::invalid_argument` (validation) or `std::logic_error` (I/O); UI handlers catch both
and log `LOG_ERROR` with the message. All file names go through S-048 R40/R41
(`GetFileSafeName`, `GetClosestUnusedFileName`).

## 3. Scene collections (dialog-level behaviour)

### 3.1 Creation & mutation flows

| Flow | Behaviour |
|---|---|
| **New** | Name prompt (existing display name ⇒ prompt rejected). Empty name ⇒ `logic_error`. `CreateSceneCollection`: throws `invalid_argument "Scene collection already exists: <n>"` on name collision; file name = S-048 R47 under `<SceneCollections>/obs-studio/basic/scenes/`. Then: emit `SCENE_COLLECTION_CHANGING`, `cleanBackupCollision`, activate, log `"Created scene collection '<n>' (clean, <file>)"`. |
| **Duplicate** | `SaveProjectNow()` first; **copy the whole current `.json` file** (overwrite) to the new file path; rewrite top-level `name`; **regenerate every `sources[*].uuid`** via `os_generate_uuid()` (S-048 R57); save with `obs_data_save_json_safe` (tmp, no bak); activate; log `"…(duplicate, …)"`. |
| **Rename** | Copy current file to the new file path (overwrite), rewrite `name`, save, activate the new entry, **remove the old file**, emit `SCENE_COLLECTION_RENAMED`. |
| **Delete (current)** | No-op when fewer than 2 collections. Confirmation dialog unless `skipConfirmation` (used by the "cannot delete while switching" path). Emit `SCENE_COLLECTION_CHANGING`, activate the **first** remaining collection, delete the file, log the switch. |
| **Delete (non-current)** | `DeleteSceneCollection(name)`: erases the map entry, refreshes, emits `SCENE_COLLECTION_LIST_CHANGED` — **but the reference deletes the *current* collection's file, not `name`'s (see D-1)**. |
| **Export** | `SaveProjectNow()`, re-read the current file, **sort `sources` by name case-insensitively**, write pretty JSON with `tmp`/`bak` extensions to the user-chosen path. |
| **Remigrate** | Menu item text toggles Migrate/Remigrate with the coordinate mode; enabled only in absolute mode; re-runs the S-048 R54 conversion on the active collection. |
| **Import** | See §3.2. |

Activation/save of collections (S-048 R56 for save mechanics): switching sets user config
`Basic.SceneCollection` / `Basic.SceneCollectionFile`; `cleanBackupCollision` removes a
stale `.bak` that would shadow the new file.

### 3.2 Import flow (`OBSImporter.cpp`)

1. On open: one-time prompt controlled by user config `General.AutoSearchPrompt` — if
   unset, ask (`Importer.AutomaticCollectionPrompt`), store the answer in
   `General.AutomaticCollectionSearch`, set `AutoSearchPrompt=true`. When enabled,
   `ImportersFindFiles()` pre-populates the table.
2. Rows added via browse (file dialog filter `(*.json *.bpres *.xml *.xconfig)`), file/dir
   drop (directories expanded to their files), or auto-search. Table columns: Selected
   (check), Name, Path; Import enabled iff ≥1 row checked.
3. `importCollections`: for each checked row: `ImportSC(path, name, res)` (empty JSON ⇒
   skip); dedup via **`GetUnusedName`**: if the display name exists, append `" 2"`,
   `" 3"`, … until unused (display-name only; no safe-name collision handling beyond R47);
   `GetFileSafeName` + `GetClosestUnusedFileName` for the file; write with
   `os_quick_write_utf8_file` (**not** the atomic safe-save); log
   `"Import Scene Collection: <name> (<file>) - SUCCESS|FAILURE"`.
4. After the dialog: `RefreshSceneCollections(true)`.

**No uuid regeneration on import** — imported collections keep their source uuids
(contrast with duplicate, §3.1). See Open questions re S-048 R57.

### 3.3 Importer framework (`importers.cpp` / `importers.hpp`)

- Registry order: **Studio, Classic, StreamLabs, XSplit**.
- `ImportSC(path, name, res)`: missing file ⇒ `IMPORTER_FILE_NOT_FOUND`;
  `DetectProgram` = first importer whose `Check` passes, else "Null" ⇒
  `IMPORTER_FILE_NOT_RECOGNISED`; then that importer's `ImportScenes`.
- Result codes: `SUCCESS, FILE_NOT_FOUND, FILE_NOT_RECOGNISED, FILE_WONT_OPEN,
  ERROR_DURING_CONVERSION, UNKNOWN_ERROR, NOT_FOUND`.

| Importer (prog id) | `Check` (detection) | `ImportScenes` (conversion) |
|---|---|---|
| `OBSStudio` (`studio.cpp`) | valid JSON with `sources`, `name`, `current_scene` | pass-through JSON; `TranslateOSStudio` remaps source ids (macOS: `wasapi_*`/`pulse_*`/`jack_*`→`coreaudio_*`, `game_capture`→`syphon-input`, `text_gdiplus`→`text_ft2_source`; other OSes analogous) updating `versioned_id`; `TranslatePaths` rewrites string values to absolute paths **only when** `rootDir + path` exists, where `rootDir` = the file's parent directory |
| `OBSClassic` (`classic.cpp`) | file starts with `"scenes : {\r\n"` | line-based parse of the legacy `.xconfig` (`scenes : {` vs `global sources : {`); name defaults to the file stem; `FindFiles`: Windows config dir `OBS\sceneCollection\`, `*.xconfig` |
| `StreamLabs` (`sl.cpp`) | JSON with `nodeType == "RootNode"` | parse `scenes.items` → OBS JSON (`attempt_import`); then `TranslateOSStudio` + `TranslatePaths` (parent dir); `FindFiles` searches StreamLabs dirs |
| `XSplitBroadcaster` (`xsplit.cpp`) | sniffs first line of `.bpres` | XML parse: playlists, slideshows, items → OBS sources; audio devices mapped via the `{0.0.1.00000000}.<dev>` pattern; `Name` is always "XSplit Import" |

## 4. Profiles (dialog-level behaviour)

| Flow | Behaviour |
|---|---|
| **New** | Name prompt (collision rejected) + "start wizard" checkbox (state = user config `Basic.ConfigOnNewProfile`). `CreateProfile`: throws on name collision; directory = `GetFileSafeName` + `GetClosestUnusedFileName(dir, nullptr)` (no extension); `create_directory`; emit `PROFILE_LIST_CHANGED`. `SetupNewProfile`: persist `Basic.ConfigOnNewProfile = useWizard`, `ActivateProfile(reset=true)`, then run the auto-config wizard modally if requested. |
| **Duplicate** | `CreateProfile` + **recursive overwrite copy of the whole profile directory** (S-048 R25) + activate. |
| **Rename** | Recursive copy to the new directory, erase old map entry, activate, **remove the old directory** (`remove_all`), emit `PROFILE_RENAMED`. |
| **Delete** | No-op when fewer than 2 profiles; confirmation unless skipped; activate the **first** remaining profile **with reset=true**, remove the directory. |
| **Import** | Directory chooser. Destination directory name = the chosen dir's file name; if a profile with that **directory name** exists ⇒ warning `Profile.Exists`, abort. Create `<Profiles>/obs-studio/basic/profiles/<dirname>/`; copy exactly: `basic.ini` (**mandatory** — missing ⇒ `LOG_ERROR "Failed to import profile … necessary file … not found"`, abort), `service.json`, `streamEncoder.json`, `recordEncoder.json` (optional, skipped when absent); `RefreshProfiles(true)`. |
| **Export** | Chooser; creates `<dest>/<directoryName>/` and copies the same 4 files (missing skipped, overwrite). |
| **Activate** (`ActivateProfile`) | 1. Open the profile `basic.ini` (`CONFIG_OPEN_ALWAYS`, failure ⇒ `logic_error`); write `General.Name = <name>`; `SaveSafe("tmp")`. 2. If a profile was active: `Auth::Save()`; when `reset`: `auth.reset()`, destroy the panel cookie manager (and YouTube dock where built in). 3. Compute `GetRestartRequirements(config)` (keys whose value changed that need a restart). 4. Save old config, **swap** `activeConfiguration`. 5. Set user config `Basic.Profile` + `Basic.ProfileDir`, save. 6. `InitBasicConfigDefaults`; when `reset`: `UpdateProfileEncoders` (defaults + missing-encoder checks + x264 fallback) + `ResetProfileData` (video, streaming service, outputs, hotkeys, audio monitoring). 7. Save, refresh, update title bar, emit `PROFILE_CHANGED`. 8. If restart requirements non-empty: question dialog listing them (joined by newlines); Yes ⇒ close with restart. |

## 5. Behaviour (numbered rules)

| # | Rule |
|---|---|
| S-070-R01 | `OBSBasic_SceneCollections.cpp`: `CreateSceneCollection` fails with `invalid_argument` on an existing display name or on `GetFileSafeName`/`GetClosestUnusedFileName` failure; the file is created under the scene-collection dir with S-048 R47 naming (never overwriting an existing file). |
| S-070-R02 | Duplicate rewrites `name` and regenerates **all** source uuids; rename does **not** touch uuids; both save via `obs_data_save_json_safe` (tmp only) and both require a prior `SaveProjectNow()` of the current collection. |
| S-070-R03 | Delete-guard: the last remaining collection (and last profile) can never be deleted (action is a silent no-op). Deleting the active collection/profile activates the first entry of the internal map (map order = enumeration order, S-048 R50). |
| S-070-R04 | Export sorts `sources` case-insensitively by `name` before writing (S-048 R57 second clause); a collection without `sources` aborts the export with `LOG_WARNING "No sources in exported scene collection"`. |
| S-070-R05 | `OBSImporter.cpp`: `GetUnusedName` appends `" <n>"` (n = 2,3,…) to the display name only; import writes are non-atomic plain writes; import never touches uuids; unchecked rows are skipped; failed `ImportSC` results are silently skipped. |
| S-070-R06 | `importers.cpp`: importer order is Studio → Classic → SL → XSplit; detection is first-match; `ImportSCFromProg` requires an explicit program match. |
| S-070-R07 | `studio.cpp`/`sl.cpp`: `TranslatePaths` only absolutizes a path string when the resolved file exists under the collection's parent dir; relative/absolute path handling uses `os_get_abs_path`. `TranslateOSStudio` id remaps are per-OS tables (see §3.3). |
| S-070-R08 | `classic.cpp`: classic files are recognized **only** by the exact 12-byte prefix `"scenes : {\r\n"`; the parser reads the file line-by-line (`ReadLine`), mapping `scenes`/`globals` sections into the OBS collection JSON; name defaults to the file stem. |
| S-070-R09 | `sl.cpp`: only `nodeType == "RootNode"` documents import; parse failure ⇒ `ERROR_DURING_CONVERSION`; name defaults to `Name(path)` when the user left it empty. |
| S-070-R10 | `xsplit.cpp`: `Name()` is constant ("XSplit Import"); playlist and slideshow XML elements become sources; audio devices use the `{0.0.1.x}` id pattern. |
| S-070-R11 | `ImporterModel.cpp`: three columns (Selected check state, Name, Path); rows added via `setData` with `NewPath`/`AutoPath` roles; `checkInputPath` validates paths on edit; header text from the i18n table. |
| S-070-R12 | `ImporterEntryPathItemDelegate.cpp`: the Path column renders a text field with browse and clear buttons; editing updates the model row. |
| S-070-R13 | `OBSBasic_Profiles.cpp`: profile import copies exactly 4 files (`basic.ini` mandatory); profile export copies the same 4 into `<dest>/<directoryName>`; duplicate/rename copy the **entire** directory recursively. |
| S-070-R14 | `OBSBasic_Profiles.cpp`: activation order is fixed as in §4 (open+write Name → auth save → reset side effects → restart requirements → swap → user config → defaults → encoders/reset data → save → refresh → event → restart prompt). `Basic.Profile` holds the display name, `Basic.ProfileDir` the directory name. |
| S-070-R15 | Restart requirements: computed by diffing the old vs new profile config via `GetRestartRequirements`; any non-empty result triggers the restart question (list joined by newlines). |

## 6. Reference defects and quirks

- **D-1** (data-loss risk): `DeleteSceneCollection(name)` for a **non-current** collection
  calls `RemoveSceneCollection(GetCurrentSceneCollection())` — it deletes the *current*
  collection's file while removing only the requested map entry. Input: delete a second,
  non-active collection. Reference behaviour: current file deleted, requested entry
  removed from the menu, file of `name` left on disk. Required behaviour: open question —
  the rewrite should either fix it (delete `name`'s file) or replicate; implementers must
  not guess (see Open questions).
- **Q-1**: import uses a non-atomic write (`os_quick_write_utf8_file`) while every other
  collection write is `obs_data_save_json_pretty_safe` (tmp/bak).
- **Q-2**: `GetUnusedName` operates on display names; two collections with different
  display names but the same safe file name are still separated by R41 numbering.
- **Q-3**: new-profile directory uses `GetClosestUnusedFileName` **without** an extension
  (numbering appended bare: `Untitled2`), unlike collection files.

## 7. Open questions

1. **D-1 resolution**: replicate the current-file-deleting bug for conformance, or fix
   `DeleteSceneCollection` to remove the requested collection's file? (Affects data
   safety; needs a coordinator decision.)
2. S-048 R57 states import regenerates source uuids; the 33.0.1 import path
   (`OBSImporter::importCollections` + all four importers) contains no uuid regeneration.
   One of the two documents is wrong — needs reconciliation before conformance vectors
   for import.
3. `GetRestartRequirements` key list lives outside the S-070 file set (in
   `OBSBasic.cpp`); the exact key set is not pinned here — implementer should pull it
   from that function when writing vectors.
4. SL/XSplit `FindFiles` search directories are OS-specific and were only partially read;
   pin them during conformance.
