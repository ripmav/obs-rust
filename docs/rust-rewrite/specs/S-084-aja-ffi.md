# S-084 AJA FFI contract (Kona SDK kept, D-04/D-10)

| | |
|---|---|
| Status | Draft (round 9 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1: `plugins/aja/*` (capture/output plugin), `plugins/aja-output-ui/*` (Qt settings UI) |
| Target crate | `crates/aja-ffi` (contract + device control surface). Per **D-04/D-10** the AJA Kona (NTV2) card driver/SDK stays native: the Rust host MUST link the same `CNTV2Card` API and replicate this contract; it does not reimplement card firmware routing. |
| Depends on | S-014 (obs_data/obs_property settings), S-015 (obs sources/outputs registration) |
| Conformance | planned, none yet |

## 1. Scope

In scope: the observable contract of the AJA plugin pair — device discovery and per-card
initialization, channel/IO-selection ownership between sources and outputs, the capture
(source) control surface (frame acquisition, audio DMA, format auto-detect), the output
control surface (DMA write pacing, A/V sync), the widget signal-routing model
(route-string grammar, preset table, socket tables), VPID parsing, audio repacking, and the
`aja-output-ui` Qt behaviour (what the dialog persists, what it triggers).

Out of scope: the NTV2 SDK internals themselves (black box), the Kona daemon, and the
obs-browser-free pure obs side (S-014/S-015).

### Reference files (13, full paths relative to `obs-studio/`)

- `plugins/aja/main.cpp` — module load/unload, `aja_loaded` signal
- `plugins/aja/aja-card-manager.cpp` — `CardManager`/`CardEntry`: card open, init sequence, ownership
- `plugins/aja/aja-source.cpp` — `AJASource` capture thread, settings→props, activate/deactivate
- `plugins/aja/aja-output.cpp` — `AJAOutput` run thread, DMA queues, A/V sync
- `plugins/aja/aja-common.cpp` — IO-selection/format/pixel-format list builders, pixel→obs format map, card-feature helpers
- `plugins/aja/aja-props.cpp` — `SourceProps`/`OutputProps` defaults and derivations
- `plugins/aja/aja-routing.cpp` — route-string parser, source/output route + audio configuration
- `plugins/aja/aja-widget-io.cpp` — firmware widget input/output socket tables and lookups
- `plugins/aja/aja-presets.cpp` — `RoutingConfigurator` preset table, `FindFirstPreset`
- `plugins/aja/aja-vpid-data.cpp` — `VPIDData` (dual VPID parse, RGB detection)
- `plugins/aja/audio-repack.c` — 8ch 32-bit → speaker-layout repack/squash
- `plugins/aja-output-ui/aja-ui-main.cpp` — program/preview output start/stop, settings load/save, preview render
- `plugins/aja-output-ui/AJAOutputUI.cpp` — settings dialog, Load/SaveSettings, state callbacks

## 2. Conventions

RFC 2119 words. "Card" = one physical AJA board, owned by a `CNTV2Card` handle (NTV2 SDK,
`ajantv2` headers). "Channel" = NTV2 framestore/channel (`NTV2_CHANNEL1..8`). "Owner" = the
string ID of the plugin instance holding a channel (source name or `aja_output_id` setting).
Audio on the card is always 32-bit PCM, 8 physical channels per audio system; OBS-side
repacking happens in the plugin, not on the card.

## 3. Interface — the NTV2 surface the host depends on

### 3.1 Call order

**Module load** (`main.cpp`): `CNTV2DeviceScanner::GetNumDevices()`; if 0 → log
`LOG_WARNING "No AJA devices found, skipping loading AJA plugin"` and return false (module
not loaded, no sources registered). Else `CardManager::EnumerateCards()`, register
`aja_source` + `aja_output` source/output info. `obs_module_post_load` signals `"aja_loaded"`
on the obs signal handler with calldata `card_manager` (raw `CardManager*`). `obs_module_unload`
→ `CardManager::ClearCardEntries()`.

**Card open + init** (`aja-card-manager.cpp`, `CardEntry::Initialize`), in order:
1. `CNTV2Card(index)` (owns the card; display name = `"<index> - <GetModelName()> (<GetSerialNumberString()>)"`).
2. Enter `NTV2_STANDARD_TASKS` briefly (reset via daemon), sleep 100 ms if changed; then `SetEveryFrameServices(NTV2_OEM_TASKS)`.
3. `AcquireStreamForApplicationWithReference(NTV2_FOURCC('O','B','S',' '), pid)`; `SetSuspendHostAudio(true)`; `ClearRouting()`.
4. `SetMultiFormatMode(true)` if `NTV2DeviceCanDoMultiFormat`; `SetReference(NTV2_REFERENCE_FREERUN)`.
5. All audio systems: `SetAudioLoopBack(NTV2_AUDIO_LOOPBACK_OFF)`.
6. All video inputs: `SetInputFrame(chan, 0xff)`; if 3G conversion: `SetSDIInLevelBtoLevelAConversion(chan, false)`.
7. All video outputs: `SetSDIOut3GEnable(true)`/`SetSDIOut3GbEnable(false)` where 3G; `SetSDIOut6GEnable(false)`/`SetSDIOut12GEnable(false)` where 12G; Level-A→B/RGB conversions off.
8. All framestores: `AutoCirculateStop` if running; `SetVideoFormat(NTV2_FORMAT_1080p_5994_A, false, false)`; `SetFrameBufferFormat(chan, NTV2_FBF_8BIT_YCBCR)`; `DisableChannel`.

**Channel acquisition** (`AcquireChannel(chan, mode, owner)`): allowed iff `ChannelReady`
(no other owner holds bit `chan`). Marks owner bitmap, then capture-mode:
`EnableInputInterrupt` + `SubscribeInputVerticalEvent`; display-mode: `EnableOutputInterrupt` +
`SubscribeOutputVerticalEvent`. `ReleaseChannel` mirrors (disable + unsubscribe) and clears the bit.

**Source start** (`aja-source.cpp`): props change → `Deactivate` + reconfigure → route
(`Routing::ConfigureSourceRoute`, §3.3) → `StartSourceAudio` → capture thread (§4.2).

**Output start** (`aja-output.cpp`): re-`EnumerateCards`; for framestores > channel 0:
`SetVideoFormat` + `SetRegisterWriteMode(NTV2_REGWRITE_SYNCTOFRAME)` + `SetFrameBufferFormat`;
`ConfigureOutputRoute` (apply crosspoints) then `ConfigureOutputAudio` (§3.3); set obs video
conversion (scale to raster, `VIDEO_CS_709`, partial) and audio conversion (32-bit, layout,
rate); `obs_output_begin_data_capture(output, 0)`; start "AJA Video Output Thread" (high priority).

**Stop** (both): release channel/IO selection (interrupts off), `StopAudioInput`/`StopAudioOutput`,
clear queues and cached crosspoint connections, source: generate black test pattern.

### 3.2 Card API calls used (names only; host MUST call the same, in the same order)

`CNTV2DeviceScanner`: `GetNumDevices`, `GetDeviceInfoList`.
`CNTV2Card`: `GetDeviceID`, `GetModelName`, `GetIndexNumber`, `GetSerialNumberString`,
`GetEveryFrameServices`/`SetEveryFrameServices`, `AcquireStreamForApplicationWithReference`,
`ReleaseStreamForApplicationWithReference`, `SetSuspendHostAudio`, `ClearRouting`,
`SetMultiFormatMode`, `SetReference`, `SetAudioLoopBack`, `SetInputFrame`,
`SetSDIInLevelBtoLevelAConversion`, `SetSDIOut3GEnable`, `SetSDIOut3GbEnable`,
`SetSDIOut6GEnable`, `SetSDIOut12GEnable`, `SetSDIOutLevelAtoLevelBConversion`,
`SetSDIOutRGBLevelAConversion`, `AutoCirculateStop`, `SetVideoFormat`, `SetFrameBufferFormat`,
`EnableChannel`/`DisableChannel`, `EnableInputInterrupt`/`DisableInputInterrupt`,
`SubscribeInputVerticalEvent`/`UnsubscribeInputVerticalEvent`,
`EnableOutputInterrupt`/`DisableOutputInterrupt`,
`SubscribeOutputVerticalEvent`/`UnsubscribeOutputVerticalEvent`,
`WaitForInputFieldID`, `GetInputVideoFormat`, `DMAReadFrame`, `DMAReadAudio`,
`DMAWriteFrame`, `DMAWriteAudio`, `StartAudioInput`/`StopAudioInput`,
`StartAudioOutput`/`StopAudioOutput`, `SetAudioCaptureEnable`, `SetAudioRate`,
`SetAudioBufferSize`, `SetNumberAudioChannels`, `SetAudioSystemInputSource`,
`SetAudioMixerInputChannelSelect`/`SetAudioMixerInputAudioSystem`/`SetAudioMixerInputChannelsMute`,
`SetHDMIOutAudioRate`/`SetHDMIOutAudioFormat`/`SetHDMIOutAudioChannels`/
`SetHDMIOutAudioSource2Channel`/`SetHDMIOutAudioSource8Channel`, `SetAESOutputSource`,
`SetSDIOutputAudioSystem`/`SetSDIOutputDS2AudioSystem`, `SetAudioOutputMonitorSource`,
`SetAudioOutputDelay`, `GetAudioWrapAddress`, `GetAudioReadOffset`, `ReadAudioSource`,
`ReadAudioLastIn`/`ReadAudioLastOut`, `SetVANCMode`, `SetTsiFrameEnable`,
`SetSDITransmitEnable`, `SetQuadQuadSquaresEnable`, `Set4kSquaresEnable`, `SetHDMIV2Mode`,
`GetInterruptCount`, `GetOutputFieldID`, `SetRegisterWriteMode`,
`WaitForOutputVerticalInterrupt`, `ApplySignalRoute`, `Close`.
Device helpers: `NTV2DeviceGetNumFrameStores`, `NTV2DeviceGetNumberFrameBuffers`,
`NTV2DeviceCanDo*`, `NTV2DeviceGetNumVideoInputs/Outputs/HDMIVideoOutputs`, `NTV2FormatDesc`
(`GetRasterWidth/Height`, `GetBytesPerRow`), `GetNTV2FrameRateFromVideoFormat`,
`GetFramesPerSecond`, `NTV2ChannelToAudioSystem`, `NTV2InputSourceToChannel`,
`NTV2OutputDestinationToChannel`, `GetIndexForNTV2Channel`/`GetNTV2ChannelForIndex`.
VPID: `CNTV2VPID` `SetVPID`/`GetStandard`/`GetSampling`/`GetBitDepth`.

### 3.3 Routing model

Route strings: `name[chan][ds]->name[chan][ds]` joined by `;`. Parser
(`aja-routing.cpp`): lowercase, strip spaces; each token pair parsed with
`%[A-Za-z_0-9][%d][%d]` (each side ≤ 64 chars); names resolved against the widget socket
tables (`aja-widget-io.cpp`: nickname→`NTV2CrosspointID`+channel+datastream, e.g.
`sdi`→`NTV2_XptSDIIn*`, `fb`→`NTV2_XptFrameBuffer*`, `hdmi`, `csc`, `lut`, `mix`, `dli/dlo`,
`4kdc`, `analog`, `tsi`, `pat`, …); builds `NTV2XptConnections{inputXpt → outputXpt}`;
returns true if **any** token parsed (partial-parse is tolerated — QUIRK Q-1). Presets are a
compiled-in table (`aja-presets.cpp`, `build_preset_table`), not read from disk; selected by
`FindFirstPreset(ConnectionKind, deviceID, mode, videoFormat, …)` (first match in table
order). `ConfigureSourceRoute`/`ConfigureOutputRoute` pick the preset, parse it, apply it, and
drive the audio config. `ConfigureOutputAudio` order: `SetNumberAudioChannels` →
`SetAudioRate` → `SetAudioBufferSize(NTV2_AUDIO_BUFFER_BIG)` → `SetAudioOutputDelay(sys,0)` →
SDI audio-system maps (plus forced `NTV2_CHANNEL1` map for IO XT/IO 4K UFC/IO 4K+/Kona 1/3G/
4UFC/5/5 2x4K; `NTV2_CHANNEL5` for SDI monitor widget) → HDMI rate/format LPCM →
`SetAudioOutputMonitorSource(NTV2_AudioChannel1_2)` → 4× `SetAESOutputSource` (1-4, 5-8, 9-12,
13-16) → if HDMI outputs: mixer main→audio system, mute aux1/aux2, `SetHDMIOutAudioChannels(8)`,
source 1-2/1-8 → `SetAudioLoopBack(OFF)` → `StopAudioOutput`.

## 4. Behaviour (numbered rules)

### 4.1 Card manager & module

| # | Rule |
|---|---|
| S-084-R01 | `main.cpp`: zero devices at load ⇒ module load fails with the warning above; otherwise cards are enumerated and `aja_source` + `aja_output` are registered. `obs_module_unload` ⇒ `ClearCardEntries` (each entry: `ReleaseStreamForApplicationWithReference` for held streams with the recorded pid, then `CNTV2Card::Close`). |
| S-084-R02 | `aja-card-manager.cpp`: `CardManager` is a process singleton; `CardEntry` owns its `CNTV2Card`. Ownership state is per-card `owner → channel-bitmask` maps plus input/output IO-selection maps; `ChannelReady(chan, owner)` is true iff unheld or held by the same owner. `Kona 1 + SDI1` input selection is always ready (special case). |
| S-084-R03 | The `aja_loaded` signal carries the raw `CardManager*` so `aja-output-ui` can drive cards; a host re-implementation MUST expose an equivalent out-of-band handle. |
| S-084-R04 | `aja-props.cpp`: source defaults = device not-found, IO invalid, format `NTV2_FORMAT_UNKNOWN`, pixel `NTV2_FBF_INVALID`, 8ch/4-byte/48 kHz audio, auto-detect off, no VPID, no swap FC/LFE. Output defaults = 8ch audio. `AudioFormat()` is always 32-bit. Speaker layout: 1=mono, 2=stereo, 3=2.1, 4=4.0, 5=4.1, 6=5.1, else 7.1. `AudioRate()`: 48000→48K (default for anything else), 96000→96K, 192000→192K. |
| S-084-R05 | Channel/framestore special cases (`aja-props.cpp`): source framestore = `NTV2_CHANNEL3` iff KonaHDMI + HDMI2 + 4K format, else channel; output channel/framestore special cases: Kona 1 → CH2; IO4K/IO4K+ SDI5 → CH4; HDMI monitor output → CH3 for 4K (or CH4 on Kona 5 8K) else CH4; TTAP Pro → CH1. |
| S-084-R06 | `aja-common.cpp`: `AJAPixelFormatToOBSVideoFormat`: `NTV2_FBF_8BIT_YCBCR`→UYVY, 24-bit RGB/BGR→BGR3, ARGB/ABGR/RGBA→BGRA, 10-bit YCBCR→V210, everything else→`VIDEO_FORMAT_NONE`. `GetSortedVideoFormats` groups device-capable formats by standard (starting at `NTV2_STANDARD_1080` in emission order), string-sorts within group. `HandleSpecialCaseFormats`: Level-B→Level-A unless 2-wire SDI+HD; SDI 1-4/5-8 quad IO + HD ⇒ quad-sized; one-wire SDI + 4K non-square + no 12G routing ⇒ quarter-quad. |
| S-084-R07 | `aja-widget-io.cpp`: socket tables are the single source of truth for widget nickname ↔ crosspoint/channel/datastream; `Find(name, channel, datastream)` and `*ByXpt` lookups are exact-match scans (first hit wins). |
| S-084-R08 | `aja-presets.cpp`: preset lookup `FindFirstPreset` returns the first table entry matching kind/device/mode/format constraints; `AddPreset`/`PresetByName` are name-indexed. |
| S-084-R09 | `aja-vpid-data.cpp`: `VPIDData` parses both VPIDs via `CNTV2VPID`; `IsRGB()` true iff sampling A ∈ {GBR_444, GBRA_4444, GBRD_4444}; equality is on (vpidA, vpidB) only. |
| S-084-R10 | `audio-repack.c`: `audio_repack_init(mode, bits_per_sample)` picks squash/repack routine; 8ch card audio is squashed (32→16-bit) to avoid resampler issues; `shuffle_8ch` reorders channels per target layout; `swapFrontCenterLFE` swaps FC↔LFE positions. |

### 4.2 Source (capture)

| # | Rule |
|---|---|
| S-084-R11 | `aja-source.cpp` settings read: `ui_prop_device` (card ID string), `ui_prop_input` (IOSelection int), `ui_prop_vid_fmt`, `ui_prop_pix_fmt` (0 = auto-detect), `ui_prop_sdi_transport`, `ui_prop_sdi_transport_4k`, `ui_prop_channel_format` (audio channels, default 2), `ui_prop_channel_swap_fc_lfe`, `ui_prop_deactivate_when_not_showing`, `ui_prop_buffering` (default true; false ⇒ `obs_source_set_async_unbuffered`). |
| S-084-R12 | Capture thread: ping-pongs two card frames starting at `framestoreIndex*2`; each loop: `WaitForInputFieldID(NTV2_FIELD0, channel)`; detect `GetInputVideoFormat`; if UNKNOWN ⇒ emit a black test-pattern frame to OBS (`obs_source_output_video2`, no card write), sleep 250 ms, loop; if auto-detect and format changed ⇒ `obs_source_update(source, nullptr)` + stop (restart via update callback). |
| S-084-R13 | Audio per loop: `CheckTransferAudioDMA`/`TransferAudio` (`DMAReadAudio` between read offsets); on success emit one `obs_source_audio` (rate = props, 32-bit, layout from props, frames = bytes/(8×4)); channels 2..6 ⇒ repacked from 8ch, 0 ⇒ memset silence, else pass-through 8ch. On failure: reset audio offsets. |
| S-084-R14 | Video per loop: `DMAReadFrame` → `obs_source_output_video2` with raster size/linesize from `NTV2FormatDesc`, colorspace `VIDEO_CS_601` for SD else `VIDEO_CS_709`, partial range; Level-B formats reported as Level-A; then `SetInputFrame(framestore, currentCardFrame)`. |
| S-084-R15 | Card removed while capturing (`GetModelName() == "(Not Found)"`): sleep 250 ms, `obs_source_update(source, nullptr)`, stop thread. On clean stop: emit a black test-pattern frame to OBS (`GenerateTestPattern`, same path as R12's UNKNOWN branch). |
| S-084-R16 | `ui_prop_deactivate_when_not_showing` (default false): `aja_source_hide` stops the capture thread (`Deactivate()`) when the source is no longer showing; `aja_source_show` restarts it (`Activate(true)`) when showing again. The OBS `activate`/`deactivate` callbacks are no-ops, and `Activate`/`Deactivate` only start/stop the capture thread — channel + IO-selection acquire/release happens in the update path and on destroy, not in show/hide. |

### 4.3 Output

| # | Rule |
|---|---|
| S-084-R17 | `aja-output.cpp` constants: 8 card frames per output, 4-frame output delay, stats period 2 s, audio sync adjust 500 samples, video sync adjust 1 frame. Card frame window = `channelIndex*8 .. +7`, validated against `NTV2DeviceGetNumberFrameBuffers` (else warn, no frames reserved). |
| S-084-R18 | `Initialize`: store `GetAudioWrapAddress`; write black test pattern to every reserved card frame; `WaitForOutputVerticalInterrupt`; delay = 4 frames in ns; video tolerance = 1 frame − 100 ns; audio tolerance = 500 samples. |
| S-084-R19 | Run thread: takes queued video frames, `DMAWriteFrame` to the next card frame (skipped while it is the playing frame); `mVideoAdjust > 0` ⇒ skip this write, `< 0` ⇒ write frame twice. Audio: `DMAWriteAudio` into the card ring with wrap-around at the wrap address; drops/inserts samples to hold sync within tolerance. |
| S-084-R20 | `aja_output_start` fails (returns false) on: null instance, missing card entry after re-enumeration, null card, or `ConfigureOutputRoute` failure. On success the video/audio conversions are registered **before** `obs_output_begin_data_capture`. |
| S-084-R21 | `aja_output_stop`: stop thread, `StopAudioOutput`, clear video/audio queues, `ClearConnections` (revert crosspoints), release IO selection (log warning if release fails). |

### 4.4 Output UI (behaviour level)

| # | Rule |
|---|---|
| S-084-R22 | `aja-ui-main.cpp`: program output = `obs_output_create("aja_output", "aja_output", settings)`; preview output = same id with `"aja_preview_output"` and renders the current preview/program scene into a stagesurface/texrender fed to the output video queue. Toggle semantics: start creates the output; stop releases it; auto-start on scene change is supported. Settings persist to the module config dir (`obs_module_get_config_path`): `ajaOutputProps.json` (program) and `ajaPreviewOutputProps.json` (preview), saved with `obs_data_save_json_safe` (`.tmp`/`.bak`). |
| S-084-R23 | `AJAOutputUI.cpp`: dialog loads saved settings (if any) then applies defaults: device, output destination, video/pixel format, SDI transport, 4K transport; sets `aja_output_id` = program/preview output id so the `CardManager` can track channel usage; saving writes the properties view settings back to the same JSON files; `OutputStateChanged`/`PreviewOutputStateChanged` drive the button state. |
| S-084-R24 | `aja-ui-main.cpp` property population: misc device list (all card display names), multi-view audio source list, and device/IO/format lists built via the `aja-common.cpp` populate functions; the UI never talks to the card directly — all card access goes through the `aja_loaded` `CardManager` handle. |

## 5. Concurrency

- Card manager mutex guards all ownership bitmaps per card; interrupt handles are enabled
  only while a channel is acquired.
- Source: one capture thread per active source; video/audio buffers are card-side DMA
  buffers (no host lock on the frame, ping-pong index).
- Output: video queue (mutex) + audio queue (mutex) + run-thread lock; OBS data-capture
  callbacks enqueue, the run thread drains.
- Multiple instances may share one card; they must never hold the same channel or
  conflicting IO selection simultaneously (R02).

## 6. Reference defects and quirks

- **Q-1** (route parser): `ParseRouteString` returns success if *any* token parsed; a
  partially valid route is applied silently. Conforming implementations must replicate the
  partial-apply semantics (or fail loudly — flagged for the implementer; see Open questions).
- **Q-2**: `WidgetIDToChannel` carries an SDK-version workaround (`NTV2_WgtSDIMonOut1` →
  channel mapping fixed for SDK ≤ 16.2); the Rust host pins one SDK version and must match.
- **Q-3**: `OutputProps::operator==` intentionally excludes `outputDest`; output reconfiguration
  must not be triggered by destination-only changes.
- **Q-4**: `aja-presets.cpp` is 1811 lines of compiled-in routing presets; they are data, not
  logic — a conformance test should diff the generated crosspoint maps for a sample of cards/modes.

## 7. Open questions

1. Which NTV2 SDK version is the rewrite pinned to (the C build supports a range; `Q-2`
   branches on it). The spec deliberately records no SDK version numbers.
2. Should the host fail-fast on partial route-string parse (deviating from Q-1)?
3. `kDefaultAJAVideoFormat` (720p5994) and `kDefaultAJAPixelFormat` (8-bit YCBCR) are defined
   in `aja-common.hpp` but the source defaults to auto-detect; confirm whether any path still
   falls back to these constants.

Round 10 second-author check (2026-11-05): corrected R12/R15 (black test pattern is emitted to
OBS via `obs_source_output_video2`, was "written to the framestore") and R16 (property id is
`ui_prop_deactivate_when_not_showing`, OBS activate/deactivate callbacks are no-ops — show/hide
only start/stop the capture thread, was "activate/deactivate acquire/release the channel + IO
selection"). R02/R04/R06/R12/R13/R17/R20 re-derived against 33.0.1 sources and confirmed.
