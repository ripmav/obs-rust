# S-071 Frontend: audio mixer dock & volume widgets

| | |
|---|---|
| Status | Draft (round 8 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1 frontend (`widgets/AudioMixer.cpp`, `components/Volume*`) |
| Target crate | `crates/frontend` (mixer UI part) |
| Depends on | S-032 (audio mixer model — `audiomix` callback, faders, volmeter), S-048 (frontend formats / dock layout) |
| Conformance | planned `test/conformance/scripts/audmixui/` — none yet |

## 1. Scope

The Qt presentation layer of the mixer: the AudioMixer dock (row list, pin/hide/lock
state, context menu) and the per-source `VolumeControl` composite (name, mute checkbox,
balance, volume slider, level meter). The signal path, fader math and volume-meter model
are S-032; this spec pins how the widgets present and persist that model.
Out of scope (described only): styling, the non-audio docks, the dock system itself (S-048).

### Reference files (all cited)

- `frontend/widgets/AudioMixer.cpp`
- `frontend/components/VolumeControl.cpp`, `frontend/components/VolumeControl.hpp`
- `frontend/components/VolumeMeter.cpp`
- `frontend/components/VolumeName.cpp`
- `frontend/components/AbsoluteSlider.cpp`
- `frontend/components/VolumeSlider.cpp`
- `frontend/components/BalanceSlider.hpp`
- `frontend/components/MenuCheckBox.cpp`
- `frontend/components/MuteCheckBox.hpp`
- `frontend/components/VolumeAccessibleInterface.cpp`

## 2. Conventions

RFC 2119. All state lives on the UI thread; obs callbacks arrive queued. "Private
settings" = the source's obs private settings (`obs_source_get_private_settings`), the
persistence medium for mixer UI state.

## 3. AudioMixer dock (`AudioMixer.cpp`)

- **S-071-R01** Rows: one `VolumeControl` per audio source, kept in `volumeList` keyed by
  source UUID. A source's eligibility comes from `obs_source_get_output_flags` (audio
  output bit). `createVolumeControl(source)` builds the row; rows are created/destroyed as
  sources appear/disappear.
- **S-071-R02** Active state: `isSourceAudioActive(source) = obs_source_active(source) &&
  obs_source_audio_active(source)` — used to style/grey rows, not to remove them.
- **S-071-R03** Per-source persisted keys (source private settings, booleans):
  `mixer_pinned` (read by `isPinnedInMixer`), `mixer_hidden` (read by `isHiddenInMixer`),
  `volume_locked` (read by `isVolumeLocked`). `VolumeControl` writes them back
  (`obs_data_set_bool(priv_settings, "mixer_pinned"/"mixer_hidden"/"volume_locked", …)`).
- **S-071-R04** Layout is deferred: source mutations schedule the single-shot `updateTimer`
  (QTimer, `setSingleShot(true)`); its timeout fires `updateVolumeLayouts`, coalescing
  bursts of source changes into one relayout.
- **S-071-R05** Pin/hide: rows carry a `VolumeControl::MixerStatus` flag set that includes
  `Pinned`; a pinned row keeps its place (pinned check at relayout) even when the source is
  inactive. Hidden rows are collapsed (`control->hide()`); the mixer scroll areas use
  `CustomContextMenuPolicy` → `mixerContextMenuRequested` builds the context menu
  (pin/hide entries plus a show/hide-hidden toggle whose tooltip is `QTStr`
  `"Basic.AudioMixer.HideHidden"`). `VolumeControl::unhideAll` connects to
  `unhideAllAudioControls`.
- **S-071-R06** A newly created control is initialised from the source state:
  `control->setPinnedInMixer(isPinnedInMixer(source))` (and the equivalent hidden/lock
  lookups) before it is shown.

## 4. VolumeControl composite (`VolumeControl.cpp` / `.hpp`)

- **S-071-R07** One row widget combining, left to right: `VolumeName`, `MuteCheckBox`,
  balance control, `VolumeSlider`, `VolumeMeter`. The volume slider drives the source
  volume (`obs_source_set_volume`); the mute checkbox drives source mute; the meter is fed
  by the S-032 volmeter (per-channel levels), not by the slider.
- **S-071-R08** The `MixerStatus` flag set (at least `Pinned`, plus hidden/locked as
  applicable) is queryable via `mixerStatus()` and drives the R04/R05 behaviour.
- **S-071-R09** Emitted signals used by the dock: `unhideAll` (and the per-row state-change
  signals feeding R03 writes).

## 5. Sub-widgets

- **S-071-R10** `VolumeMeter.cpp`: wraps the obs volmeter (S-032 model). `setLevels(magnitude,
  peak, inputPeak)` feeds per-channel levels; `setPeakMeterType(enum obs_peak_meter_type)`
  forwards to `obs_volmeter_set_peak_meter_type` and adjusts the display; `resetLevels`
  clears meter ballistics. Defaults: `peakDecayRate = 11.76` (≈ 20 dB / 1.7 s),
  `peakHoldDuration = 20.0` s. The true-peak type follows the EBU Permitted-Maximum
  convention noted in the source (Q2 for the exact displayed offset).
- **S-071-R11** `AbsoluteSlider.cpp` (base `SliderIgnoreScroll`): the slider is
  *absolute* — press or move sets the value to the cursor position, never relative
  stepping: `mousePressEvent` / `mouseMoveEvent` emit `sliderMoved(posToRangeValue(event))`;
  hover emits `absoluteSliderHovered(val)`. Wheel events are ignored (base class).
- **S-071-R12** `VolumeSlider.cpp`: the volume slider used in the row (absolute semantics of
  R11 with volume range mapping); `BalanceSlider.hpp`: the left/right balance slider of the
  row.
- **S-071-R13** `MuteCheckBox.hpp`: `QCheckBox` with `setTristate(true)` and style property
  `class = "indicator-mute"`; the partial (tristate) state exists for internal use, the
  user-facing toggle is binary (Q3 for the exact partial-state rule).
- **S-071-R14** `MenuCheckBox.cpp`: a checkbox with an attached dropdown menu — the row's
  mode selector (meter type etc.); checking the box enables the feature, the menu chooses
  the mode.
- **S-071-R15** `VolumeName.cpp`: the row's source-name label; kept in sync with the source
  name (updated on rename) and truncated to the row width.
- **S-071-R16** `VolumeAccessibleInterface.cpp`: the `QAccessible` implementation for the
  volume control — exposes name (source name) and value to assistive technology so the
  mixer is operable without the mouse (Q4 for the exact accessible value format).

## 6. Concurrency

UI thread only. obs source signals (volume, active, rename, destroy) reach the mixer via
queued connections; R04's single-shot timer is the only layout deferral. No background
threads are introduced by the widgets.

## 7. Quirks and open questions

- Q1: reset-to-default (0 dB) gesture — not re-derived this round (the reference's
  gesture set is larger than the rules pinned here).
- Q2: the exact true-peak display offset/scale per `obs_peak_meter_type` (the source
  comment cites the EBU Permitted Maximum; the numeric offset is not re-derived).
- Q3: the exact rule that sets/clears `MuteCheckBox`'s partial state.
- Q4: `VolumeAccessibleInterface`'s exact accessible value (dB text vs linear volume).
- Q5: `MenuCheckBox` menu contents per feature (mode list is UI data).
