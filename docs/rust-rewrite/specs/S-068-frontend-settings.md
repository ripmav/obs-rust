# S-068 Frontend: settings dialog (pages, dirty/apply/save, hotkeys)

| | |
|---|---|
| Status | Draft (round 8 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1, `frontend/settings/*` |
| Target crate | `crates/frontend` (GUI layer; Qt in the reference) |
| Depends on | S-048 (config files, CLI/frontend API catalog), S-034 (hotkeys core), S-025 (encoder options UI), S-035 (services) |
| Conformance | planned; none yet (Qt widgets; no executable vectors) |

## 1. Scope

In scope: the settings dialog (`OBSBasicSettings`) — its nine pages, the
per-page dirty flags and load/apply/save/reject flows, the stream service
page, output encoder selection rules, video/audio page validation, the
hotkey editing widgets (`OBSHotkeyWidget`/`OBSHotkeyEdit`/`OBSHotkeyLabel`)
including duplicate detection and search, the accessibility colour page,
and the appearance (theme) page.

Out of scope: encoder property-view content (`OBSPropertiesView`,
libobs-driven — S-015/S-025), service property lists (S-035), config file
layout (S-048), theme engine internals.

**Reference files** (all 10, paths relative to the obs-studio root):
- `frontend/settings/OBSBasicSettings.cpp`
- `frontend/settings/OBSBasicSettings.hpp`
- `frontend/settings/OBSBasicSettings_Stream.cpp`
- `frontend/settings/OBSBasicSettings_Appearance.cpp`
- `frontend/settings/OBSBasicSettings_A11y.cpp`
- `frontend/settings/OBSHotkeyEdit.cpp`
- `frontend/settings/OBSHotkeyEdit.hpp`
- `frontend/settings/OBSHotkeyWidget.cpp`
- `frontend/settings/OBSHotkeyWidget.hpp`
- `frontend/settings/OBSHotkeyLabel.cpp` (class declared in
  `frontend/settings/OBSHotkeyLabel.hpp`, cited for context only)

## 2. Conventions

RFC 2119 words. "config" = the profile config (`basic.ini`, S-048 §4.3);
sections are `[General]`, `[Stream1]`, `[Output]`, `[AdvOut]`, `[Audio]`,
`[Video]`, `[Hotkeys]`, `[A11y]`, `[Appearance]`, `[Advanced]`.
`QTStr(...)` = translated UI string. All widgets run on the GUI thread.

## 3. Interface (dialog surface)

- `OBSBasicSettings : QDialog` — ctor takes `OBSBasic* parent`. Page
  order (`enum Pages`): GENERAL, APPEARANCE, STREAM, OUTPUT, AUDIO,
  VIDEO, HOTKEYS, ACCESSIBILITY, ADVANCED (`NUM_PAGES`).
- Per-page dirty flags: `generalChanged, stream1Changed, outputsChanged,
  audioChanged, videoChanged, hotkeysChanged, a11yChanged,
  appearanceChanged, advancedChanged`; `Changed()` = OR of all.
  `loading` suppresses change-detection while populating pages.
- State kept across reloads: `pageIndex`, `savedTheme`,
  `sampleRateIndex`, `channelIndex`, `llBufferingEnabled`,
  `hotkeysLoaded`, `forceAuthReload`, `forceUpdateCheck`, last-selected
  encoder indices/presets (`curPreset`, `curQSVPreset`, `curNVENCPreset`,
  `curAMDPreset`, `curAMDAV1Preset`, `curAdvStreamEncoder`,
  `curAdvRecordEncoder`), `lastSimpleRecQualityIdx`, `lastServiceIdx`,
  `lastIgnoreRecommended`, `lastChannelSetupIdx`, `outputCX/outputCY`,
  hotkey widget list (`vector<pair<bool, QPointer<OBSHotkeyWidget>>>`,
  bool = frontend registerer — only such rows are written to `[Hotkeys]`), `hotkeyConflictIcon`.
- Constants: `VOLUME_METER_DECAY_FAST 23.53`, `_MEDIUM 11.76`,
  `_SLOW 8.57` (a11y dB/s decay presets);
  `ENCODER_HIDE_FLAGS = OBS_ENCODER_CAP_DEPRECATED | OBS_ENCODER_CAP_INTERNAL`
  (hidden from the simple-output lists).
- A11y default colours (uint32, as stored): selectRed `0x0000FF`,
  selectGreen `0x00FF00`, selectBlue `0xFF7F00`, mixerGreen `0x267f26`,
  mixerYellow `0x267f7f`, mixerRed `0x26267f`, mixerGreenActive
  `0x4cff4c`, mixerYellowActive `0x4cffff`, mixerRedActive `0x4c4cff`.
- `DeserializeConfigText(value)`: parses the value as JSON, returns the
  `"text"` string (advanced page raw-config read-back).

## 4. Data formats

- Config writes go through per-widget helpers (`SaveCombo`,
  `SaveComboData`, `SaveCheckBox(invert)`, `SaveGroupBox`, `SaveEdit`,
  `SaveSpinBox`, `SaveText`, `SaveFormat`, `SaveEncoder`) — one key each;
  `SaveCheckBox(..., invert=true)` stores the logical negation.
- Resolution fields are text `"WxH"` parsed by `ConvertResText` (both
  integers required).
- Hotkey combinations are `obs_key_combination_t` (`modifiers` bitmask +
  `key`); text via `obs_key_combination_to_str` (S-034).

## 5. Behaviour (numbered rules)

### 5.1 Dialog lifecycle and dirty/apply/save (`OBSBasicSettings.cpp`, `OBSBasicSettings.hpp`)

- **S-068-R01** Construction builds all pages, saves the current theme (`savedTheme`), connects widgets (each via `HookWidget`, which initialises the widget's `"changed"` property to false), then `LoadSettings(false)`. While loading, `loading == true` suppresses change flags; `pageIndex` is restored after load.
- **S-068-R02** Any user edit on a page's widgets calls that page's `*Changed()` slot, which sets the page flag and `EnableApplyButton(true)` (Apply enabled iff `Changed()`); during programmatic population no flag is set.
- **S-068-R03** Button box: **Apply** → `QueryAllowedToClose()` must pass, else the click is ignored (stays open); on success `SaveSettings()`, update YouTube dock settings, `ClearChanged()`, dialog stays open. **OK** → same save path, then close. **Cancel/Reject** → restore theme iff `savedTheme != current`, `ClearChanged()`, close.
- **S-068-R04** Closing with unsaved changes (close button, WM close, `reject()`/Escape) goes through `AskIfCanCloseSettings()`: `!Changed()` → close immediately; else `QueryChanges()` Yes/No/Cancel — Cancel = stay open; Yes = `QueryAllowedToClose()` then `SaveSettings()`; No = restore theme if changed, `LoadSettings(true)` (changed pages only, R05) to discard edits. Afterwards `ClearChanged()`. `closeEvent`/`reject` are ignored when the flow refuses.
- **S-068-R05** `LoadSettings(changedOnly)` reloads in fixed order — General, Stream1, Output, Audio, Video, A11y, Appearance, Advanced — only the flagged pages when `changedOnly` (hotkeys reload via `LoadHotkeySettings` on demand, not here).
- **S-068-R06** `QueryAllowedToClose()` blocks saving when, for the current output mode: (simple) any of the four encoder combos empty, or the recording container combo empty, or zero recording tracks selected while quality ≠ "Stream" and container ≠ `flv`; (advanced) any encoder combo empty, or zero recording tracks with container ≠ `flv`, or zero streaming tracks. Each case shows its specific warning ("Codec/Container missing on exit", "No tracks selected on exit") and returns false.
- **S-068-R07** `SaveSettings()` saves only the flagged pages, in order General, Stream1, Output, Audio, Video, Hotkeys, A11y, Advanced, Appearance. Video or advanced changed → `main->ResetVideo()`. Then `config_save_safe(config, "tmp")` + `config_save_safe(userConfig, "tmp")` (S-048) + `main->SaveProject()`. A log line `Settings changed (<page list>)` is written when any flag was set.
- **S-068-R08** After saving, `restart = true` iff the language index changed, or the audio channel-setup/sample-rate index changed, or the browser-HW-accel checkbox changed; the caller (main window, S-069) prompts/restarts accordingly.

### 5.2 Stream page (`OBSBasicSettings_Stream.cpp`)

- **S-068-R09** Four service situations by type: `rtmp_custom`, `rtmp_common`, `whip_custom`, custom-server fallback (the three type strings identify the active service object). `IsCustomService()` = combo on the (custom) entry (`ListOpt::Custom`); `IsWHIP()` = combo on a WHIP entry (`ListOpt::WHIP`).
- **S-068-R10** `LoadStream1Settings()` reads the current service object's settings (`service`, `server`, `key`, `using_custom_server`, `use_auth`, `username`, `password`, `bearer_token`, `bwtest`) plus profile keys `IgnoreRecommended`, `WHIPSimulcastTotalLayers`, `EnableMultitrackVideo`, `MultitrackVideoMaximumAggregateBitrate[Auto]`, `MultitrackVideoMaximumVideoTracks[Auto]`. A non-list service name is inserted into the combo at index 1 and selected (saved custom entries persist); custom services fill the server field (custom+whip only).
- **S-068-R11** `ServiceChanged(resetFields)`: on a different service name (or forced reset) calls the frontend `reset_service_ui_fields` hook (S-048 §5.9), re-applies the multitrack checkbox from config, updates the multitrack UI. Visibility: auth fields only for custom services; server row inserted for custom or WHIP; OAuth account widgets only when the main window holds a matching connected auth (service-name substring match; YouTube matches any-YouTube). `OnAuthConnected` fills the connected-account label and shows disconnect/bandwidth-test/addon widgets as applicable.
- **S-068-R12** `FindProtocol()`: custom service → empty server text `RTMP`; `rtmps://` → `RTMPS` (only if the output protocol is registered); `srt://` → `SRT`; `rist://` → `RIST`; else the `protocol` field of the service property settings after `obs_property_modified`; fallback `RTMP`.
- **S-068-R13** VOD track / multitrack: `UpdateVodTrackSetting` enables the VOD track option only for compatible services/protocols (`AllowsMultiTrack` checks the protocol list); `SwapMultiTrack(protocol)` persists per-protocol multitrack state; the multitrack checkboxes (max aggregate bitrate auto, max video tracks auto, config override) gate the detailed controls via `UpdateMultitrackVideo`.
- **S-068-R14** `SaveStream1Settings()` rebuilds the service settings — custom: server + key (+ username/password iff `useAuth`); named: service name + key; WHIP bearer token kept. The service object is replaced (`obs_service_create` of the right type, S-035), `main->SetService` applied, then the profile `Stream1` keys are written (`IgnoreRecommended`, multitrack keys). `lastService`/`lastServiceIdx`/`lastCustomServer` update so a reload doesn't reset in-progress edits.
- **S-068-R15** OAuth: "Connect account" opens the auth flow (main-window `Auth` object); `OnOAuthStreamKeyConnected` fills the key field with the OAuth stream key (persisted into the service settings on save); "Disconnect" clears key/bearer token and hides account widgets. `SpawnTempService()` creates a throwaway service of the current type to validate values without touching the active service.
- **S-068-R16** `LoadServices(showAll)` populates the combo from `rtmp_common` service property entries (all, or only registered/available); the bandwidth-test checkbox is visible only for `rtmp_common`; the Twitch addon dropdown (None/BTTV/FFZ/Both) only for Twitch, persisted under `[Twitch] AddonChoice`.

### 5.3 Output page (`OBSBasicSettings.cpp` output section)

- **S-068-R17** Two modes, radio-switched: **Simple** / **Advanced** (`[Output] Mode`). Encoder combos hide `ENCODER_HIDE_FLAGS` encoders. Record and stream encoder/track sets are independent; audio track selection (up to `MAX_AUDIO_MIXES` VOD tracks + track 1) is checkbox-based; `SimpleOutGetSelectedAudioTracks`/`AdvOutGet*` count selected tracks (used by R06 and the no-tracks warnings).
- **S-068-R18** Bitrates: `PopulateSimpleBitrates` fills the simple audio-bitrate combo from the audio encoder's bitrate list (opus map vs AAC map, picked by the selected simple audio encoder); `PopulateAdvancedBitrates` fills advanced stream+record combos; `FindClosestAvailableAudioBitrate(box, bitrate)` picks the closest listed audio bitrate; `RestrictResetBitrates(boxes, maxbitrate)` drops list entries above the value closest to `maxbitrate` (only called with 320, audio tracks, when the channel setup is not surround); if the current selection was dropped the combo resets to the value closest to `maxbitrate/2` and is marked changed. Current values are preserved across reloads by the stored index/preset fields (§3).
- **S-068-R19** Advanced-FFmpeg: format list from `FFmpegFormat` entries (S-048 §4); video/audio encoder lists depend on the selected format (`ReloadCodecs`); "ignore compatibility" gates `obs_output_get_active_format` checks; container/codec mismatches show the FFmpeg warning labels. Delay estimates (simple & advanced) are recomputed from the configured delay.
- **S-068-R20** `AdvOutRecCheckWarnings`/`AdvOutRecCheckCodecs` warn (non-blocking labels) when the record encoder lacks capability for the chosen resolution/FPS, or when a hardware-encoder-specific preset (QSV/NVENC/AMD/AMF-AV1 preset combos) is inconsistent.
- **S-068-R21** Simple recording: quality combo (incl. "Stream" = use streaming settings) drives `FillSimpleRecordingValues` (res/FPS limits from the service recommendation, `ResFPSValid` against the service `resolution` list + `max_fps`); "lossless" qualities show `SimpleRecordingQualityLosslessWarning` for hardware encoders.

### 5.4 Video page

- **S-068-R22** Base and output resolution are free text (`WxH`, §4) in `[Video]` (`BaseCX/CY`, `OutputCX/CY`). Editing triggers `RecalcOutputResPixels`: parse; if unparseable or `ResTooLow` (cx or cy < 32) or `ResTooHigh` (> 16384) the value is rejected (no state change; the entered text is NOT reverted — the base-resolution path additionally sets the `videoMsg` warning to `INVALID_RES_STR` via `ValidResolutions`); otherwise `outputCX/outputCY` are stored, the aspect label updates, and the downscale filter list is refreshed (`LoadDownscaleFilters`).
- **S-068-R23** `ResetDownscales(cx, cy, ignoreAllSignals)` re-populates the downscale-filter combo with libobs's recommended filters for the base→output transition; `LoadDownscaleFilters` restores the saved choice if still present.
- **S-068-R24** Color range/space/format combos are populated from libobs (`obs_get_video_ranges/spaces/pixelformats`); `UpdateColorFormatSpaceWarning` warns when the chosen (range, space) pair is unsupported for the pixel format.
- **S-068-R25** FPS combos are stored as `FPSType`/`FPSNum`/`FPSDen`; `UpdateResFPSLimits` (toggled by "ignore service recommendations") either clamps resolution/FPS to the service's recommended maximums or frees them.

### 5.5 Audio page

- **S-068-R26** Global sample rate combo (44100 / 48000 / 96000; saved `[Audio] SampleRate`; default 44100, 48000 when the platform device list indicates 48k availability) and channel setup combo (Mono/Stereo/4.0/4.1/5.1/7.1 → `obs_audio_layout`), saved under `[Audio]`. A changed index here forces a restart (R08).
- **S-068-R27** `LoadAudioSources` builds one row per audio source (source id + mute + sync-offset spinbox + monitoring-device combo + volume), tracked in `audioSources` with per-source signals; new sources added live (`sourceCreated`), removals clean the row. Values persist per source id in `[Audio]` (`volume_<id>`, `mute_<id>`, `syncOffset_<id>`, `monitoringId_<id>`). `LowLatencyBufferingChanged` toggles the low-latency buffering checkbox remembered in `llBufferingEnabled`.
- **S-068-R28** `UpdateAudioWarnings` flags (non-blocking labels): sample-rate ≠ device rates, surround layout without a 5.1-capable monitoring device, etc.

### 5.6 Hotkeys page (`OBSHotkeyWidget.cpp/.hpp`, `OBSHotkeyEdit.cpp/.hpp`, `OBSHotkeyLabel.cpp`)

- **S-068-R29** The page is a form of `OBSHotkeyLabel` (name + `"fullName"` property = dotted path e.g. `OBS.Output.Start`) paired with an `OBSHotkeyWidget`. Pairs (a hotkey and its "partner") are marked (`pairPartner`); hovering one highlights the partner (`highlightPair` sets both labels' `class` to `text-bright`).
- **S-068-R30** `OBSHotkeyWidget` per binding row: an `OBSHotkeyEdit` plus four buttons — revert (enabled iff edit ≠ original), clear (enabled iff key non-empty), add (inserts an empty row after), remove (disabled on the last row). `SetKeyCombinations` with an empty list adds one empty edit. `AddEdit` wires `KeyChanged` → widget `KeyChanged` (marks page hotkey-changed) + button states; `SearchKey` propagates to the filter (R34). `Changed()` = any edit `changed`.
- **S-068-R31** `OBSHotkeyEdit`: non-editable line edit (input method disabled, invisible cursor). `keyPressEvent`: auto-repeat ignored; bare modifier presses (Shift/Ctrl/Alt/Meta alone) record `OBS_KEY_NONE` with the modifiers; on macOS CapsLock → virtual key 57; else `obs_key_from_virtual_key(nativeVirtualKey)` + `TranslateQtKeyboardEventModifiers`. `mousePressEvent`: middle → `OBS_KEY_MOUSE3`, extra buttons 1..24 → `OBS_KEY_MOUSE4`..`OBS_KEY_MOUSE27`, with current modifiers.
- **S-068-R32** `HandleNewKey`: a key equal to the current one or an empty combination is ignored; else `key` is set, `changed = true`, `KeyChanged` emitted, text re-rendered via `obs_key_combination_to_str`. `ResetKey()` restores `original` (clears `changed`); `ClearKey()` sets the empty combination (marks changed).
- **S-068-R33** Saving a row: `GetCombinations` collects all edits' keys; `Save(combinations)` calls `Apply()` (edit.original ← key, changed ← false, revert buttons disabled) then `obs_hotkey_load_bindings(id, combos)` **inside** `obs_hotkey_update_atomic` with `ignoreChangedBindings = true` around the load, so the resulting `hotkey_bindings_changed` signal (R35) doesn't re-enter the widget. `Apply()` alone just marks the UI clean.
- **S-068-R34** Search: the filter box (text + optional key-combo filter field, itself an `OBSHotkeyEdit` emitting `SearchKey`) drives `SearchHotkeys(text, combo)`: a row is visible iff (text empty OR the label's `fullName` contains the lower-cased text) AND (no combo filter OR the widget has an edit equal to the filter combo). The scroll area resets to top and the whole layout is disabled during the pass; "Reset" clears both filter inputs.
- **S-068-R35** External binding changes: each widget subscribes to `hotkey_bindings_changed` (obs signal) and queues `HandleChangedBindings(id)` on the GUI thread; if the id matches and `!ignoreChangedBindings` it re-reads the bindings via `obs_enum_hotkey_bindings`, removes all edits (no signal) and rebuilds. Keymap remaps: `layoutChanged` signal → `ReloadKeyLayout` (re-render all edits) via queued invoke.
- **S-068-R36** Duplicate detection: `ScanDuplicateHotkeys(layout)` builds `map<combination, vector<(label, edit)>>` over all non-empty edits; every edit whose combination already has an assignment from a *different* label (skipping the pair partner) gets `hasDuplicate = true` on both sides; `UpdateDuplicationState` shows/hides the conflict icon. Re-run after every `KeyChanged`.
- **S-068-R37** `LoadHotkeySettings(ignoreKey)` (and `ReloadHotkeySettings`) rebuild the whole form from `obs_enum_hotkeys` (S-034), one row per hotkey (frontend registerers get direct rows; encoder/output/service/source hotkeys are grouped under their object rows), `hotkeysLoaded` guarding re-entrancy; `SaveHotkeySettings` calls `widget->Save(combinations)` only on widgets with `Changed()` true and writes the `[Hotkeys]` config entry only for frontend-registerer rows; the page flag is cleared by `ClearChanged()` on the save paths (R03/R07).
- **S-068-R38** The page-level slot `HotkeysChanged()` sets `hotkeysChanged`, enables Apply, and triggers the duplicate scan.

### 5.7 Accessibility page (`OBSBasicSettings_A11y.cpp`)

- **S-068-R39** Nine colour buttons (selection red/green/blue, mixer green/yellow/red, mixer green/yellow/red *active*), each persisted in `[A11y]` (`SelectRed`…`MixerRedActive`); `LoadA11ySettings` reads them (falling back to the §3 defaults when absent), `UpdateA11yColors` re-applies them to styles, `SetDefaultColors`/`ResetDefaultColors` fill the §3 values. A colour-preset combo re-loads a preset palette (`on_colorPreset_currentIndexChanged`).
- **S-068-R40** The volume-meter decay options map to the three `VOLUME_METER_DECAY_*` constants (fast/medium/slow) under `[A11y]`.

### 5.8 Appearance page (`OBSBasicSettings_Appearance.cpp`)

- **S-068-R41** Theme + theme-variant combos populated from the available themes (`LoadThemeList`, which also applies the theme on activation — `on_theme_activated` → `App()->SetTheme`, `on_themeVariant_activated` the variant); the current theme is remembered as `savedTheme` at construction and **restored on reject/cancel** (R03/R04) but not on Apply.
- **S-068-R42** Font and density controls enable/disable according to the selected theme's support (`enableAppearanceFontControls`, `enableAppearanceDensityControls`); `updateAppearanceControls` re-evaluates on `OBSApp::StyleChanged`.

### 5.9 General / Advanced pages (`OBSBasicSettings.cpp`)

- **S-068-R43** General: language combo (restart required, R08), minimize-to-tray, "always on top", browser HW accel (restart required), safe-mode/verbose toggles where present; `SaveGeneralSettings` writes `[General]`. Advanced: max audio buffer ms, "disable auto-updates", raw config text edit (read via `DeserializeConfigText`), renderer selection (`LoadRendererList`, `[General] Renderer`).

## 6. Algorithms

Hotkey duplicate scan (R36), verbatim logic:

```
assignments = {}            # combo -> [(label, edit)]
for row in form rows:
    label = row.label       # OBSHotkeyLabel
    for edit in label.widget.edits:
        edit.hasDuplicate = false
        if edit.key empty: continue
        for (otherLabel, otherEdit) in assignments[edit.key]:
            if label.pairPartner == otherLabel: continue
            otherEdit.hasDuplicate = edit.hasDuplicate = true
        assignments[edit.key].append((label, edit))
for every edit: update dupe icon visibility
```

## 7. Concurrency

- Whole dialog is GUI-thread. The one cross-thread input is the obs
  signal `hotkey_bindings_changed` (obs thread), queued to the GUI thread
  via `QMetaObject::invokeMethod` (R35); config writes never race the obs
  hotkey core. `obs_hotkey_update_atomic` serialises the binding load
  against concurrent hotkey updates (S-034).
- Service creation (`SpawnTempService`, `SaveStream1Settings`) runs on the
  GUI thread; no service callbacks fire on other threads for these paths.

## 8. Reference defects and quirks

- **Q-1** `ClosestResFPS` is marked `// TODO: Remove, orphaned method` —
  declared/defined but never called; a conforming port may drop it
  (behaviour-neutral).
- **Q-2** On Cancel/No (R04) the theme is restored **before**
  `LoadSettings(true)`; if no page was actually dirty, `QueryChanges` is
  not shown at all, so a theme-only change made after opening is silently
  kept until the next OK.
- **Q-3** `ScanDuplicateHotkeys` treats the *pair partner* as exempt, so a
  hotkey pair may share a combination without the conflict icon —
  intended, keep it.

## 9. Implementation notes (non-normative)

- The dirty-flag-per-page + `QueryAllowedToClose` gating is the most
  stateful part; model pages as an enum of `DirtyFlags` and keep the
  button-role dispatch (R03) a pure function of (role, dirty, validation).
- Hotkey widgets: `OBSHotkeyEdit` maps 1:1 to a `BindingEdit { key,
  original }`; the atomic-load + `ignoreChangedBindings` handshake is the
  only place re-entrancy actually bites — keep the flag.
- Encoder combos are data-driven from libobs (S-025); this spec only pins
  the *selection* and *validation* behaviour around them.

## 10. Conformance coverage

| Rule(s) | Scripts |
|---|---|
| all S-068-Rxx | none yet (Qt UI; no executable vectors) |

## 11. Open questions

1. Exact audio sample-rate entry list and per-platform hiding (only 44100/48000/96000 observed; `96000` presence per device list) not fully pinned.
2. `AdvOutRecCheckCodecs` capability-matrix details (which encoder caps trigger which warning labels) not enumerated line by line.
3. The advanced-page raw-config "Apply text" path (how the edited blob is parsed and merged back) was only verified as `DeserializeConfigText` read-back; write/parse-error handling needs a dedicated pass.
4. Per-service VOD-track compatibility list (`AllowsMultiTrack` protocol list contents) not transcribed in full.
5. Round 10 second-author check (2026-11-05): corrected R09 (IsWHIP checks combo `ListOpt::WHIP`, not the service type string), R15 (OAuth key fills the key field; persisted into the service settings on save), R18 (bitrate combos come from audio-encoder bitrate lists; `RestrictResetBitrates` uses a fixed 320 cap, reset target = closest of half-max), R22 (invalid res text is not reverted; base-res path shows `videoMsg` `INVALID_RES_STR` warning), R37 (form built from `obs_enum_hotkeys`, not `obs_frontend_get_all_hotkeys`; save iterates only changed widgets), §3 (hotkey-list bool = frontend registerer, not pair partner).
