# S-083 Blackmagic DeckLink FFI contract (SDK kept, D-04/D-10)

| | |
|---|---|
| Status | Draft (round 9 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1: `plugins/decklink/*` (capture/output), `plugins/decklink-output-ui/*` (Qt UI), `plugins/decklink-captions/*` (CEA-708 forwarder) |
| Target crate | `crates/decklink-ffi` (contract + device control surface). Per **D-04/D-10** the Blackmagic DeckLink SDK/com drivers stay native: the Rust host MUST link the same `IDeckLink*` COM interfaces and replicate this contract. |
| Depends on | S-014 (obs_data/obs_property settings), S-015 (obs sources/outputs), S-048 (profile save callbacks, §5.9 frontend API) |
| Conformance | planned, none yet |

## 1. Scope

In scope: device discovery/notification lifecycle, per-device init (modes, connections,
capabilities, identity hash), the capture source control surface (frame/audio/caption
acquisition, format detection, HDR metadata), the output control surface (scheduled
playback, preroll, keying, audio), CEA-708 ancillary caption parsing, the caption
forwarder plugin, and the Qt output-UI behaviour (what it persists, what it triggers).

Out of scope: Blackmagic SDK internals (black box), the OBS properties-widget internals
(frontend, not this plugin).

### Reference files (23, full paths relative to `obs-studio/`)

- `plugins/decklink/plugin-main.cpp` — module load, SDK version logging, source/output registration
- `plugins/decklink/decklink-devices.cpp` — global `deviceEnum`, device list filler
- `plugins/decklink/decklink-device-discovery.cpp` — `IDeckLinkDiscovery` + notification callback impl
- `plugins/decklink/decklink-device.cpp` — refcounted device wrapper, capability + hash init
- `plugins/decklink/decklink-device-mode.cpp` — display-mode wrapper, frame-rate comparison
- `plugins/decklink/decklink-device-instance.cpp` — capture/output stream engine, SDK callbacks
- `plugins/decklink/decklink-source.cpp` — `decklink-input` source info, settings, property callbacks
- `plugins/decklink/DecklinkInput.cpp` — input activate/deactivate, device-change auto re-activate
- `plugins/decklink/decklink-output.cpp` — `decklink_output` output info, start/stop, audio prep
- `plugins/decklink/DecklinkOutput.cpp` — output activate/deactivate wrapper
- `plugins/decklink/OBSVideoFrame.cpp` / `plugins/decklink/OBSVideoFrame.h` — `IDeckLinkVideoFrame` impl for format conversion
- `plugins/decklink/audio-repack.c` — 8ch 16-bit → speaker-layout repack
- `plugins/decklink/util.cpp` — BMD connection → display-name maps
- `plugins/decklink/win/platform.cpp` — COM instance creation (CoCreateInstance), BSTR conversion
- `plugins/decklink/linux/platform.cpp` — `DeckLinkStringToStdString` (free-based)
- `plugins/decklink/mac/platform.cpp` — `DeckLinkStringToStdString` (CFRelease-based)
- `plugins/decklink-output-ui/decklink-ui-main.cpp` — program/preview output start/stop, settings load
- `plugins/decklink-output-ui/DecklinkOutputUI.cpp` — settings dialog, SaveSettings, state
- `plugins/decklink-captions/decklink-captions.cpp` — Tools-menu caption forwarder
- `plugins/decklink-captions/decklink-captions.h` — `DecklinkCaptionsUI` declaration
- `plugins/decklink/decklink-device-discovery.hpp` — `DeviceChangeInfo` callback table
- `plugins/decklink/decklink-device-instance.hpp` — instance state, `RenderDelegate`
- `plugins/decklink/decklink-device-discovery.hpp` is the single source of the callback API; `DecklinkInput.hpp`/`DecklinkOutput.hpp` define the `DecklinkBase` settings carried per instance

## 2. Conventions

RFC 2119 words. "Device hash" = the persistent string ID used in settings
(`<PersistentID or TopologicalID>_<ModelName>`). "Mode id" = a per-process 1-based index
into the device's display-mode list (0 = `Auto` input mode when format detection is
supported). All `HRESULT` failures are logged and abort the start path (no retry loop).
SDK string conversions are platform-split: Linux `free`, macOS `CFRelease`, Windows BSTR
delete (`util.cpp`/`platform.cpp` files).

## 3. Interface — the DeckLink SDK surface the host depends on

### 3.1 Call order

**Module load** (`plugin-main.cpp`): `CreateDeckLinkIteratorInstance()`; null ⇒ log
`"A DeckLink iterator could not be created.  The DeckLink drivers may not be installed"`
and fail load. Else `QueryInterface(IID_IDeckLinkAPIInformation)` +
`GetString(BMDDeckLinkAPIVersion)` ⇒ log compiled + installed API versions. Create global
`deviceEnum = new DeckLinkDeviceDiscovery` (`IDeckLinkDiscovery` + `InstallDeviceNotifications`);
on failure fail load. Register source `decklink-input` and output `decklink_output`.
Unload: `delete deviceEnum` (`UninstallDeviceNotifications`, release all devices).

**Device init** (`decklink-device.cpp`, `Init`): `QueryInterface(IDeckLinkProfileAttributes)`;
add `Auto` input mode iff flag `BMDDeckLinkSupportsInputFormatDetection`; iterate
`IDeckLinkInput::GetDisplayModeIterator` (ids 1..N) for input modes and
`IDeckLinkOutput::GetDisplayModeIterator` for output modes; read ints
`BMDDeckLinkVideoInputConnections`/`VideoOutputConnections`/`AudioInputConnections`/
`AudioOutputConnections`; flags `BMDDeckLinkSupportsExternalKeying`,
`BMDDeckLinkSupportsInternalKeying`, `BMDDeckLinkSupportsHDRMetadata`; ints
`BMDDeckLinkSubDeviceIndex`, `BMDDeckLinkNumberOfSubDevices`,
`BMDDeckLinkMinimumPrerollFrames` (default 3 on failure); `GetModelName`/`GetDisplayName`
(failure ⇒ `Init` false, device dropped); `maxChannel` = 8 for "Intensity Shuttle
Thunderbolt", else `BMDDeckLinkMaximumAudioChannels`, else 2; hash =
`"<BMDDeckLinkPersistentID or BMDDeckLinkTopologicalID>_<name>"`.

**Capture start** (`decklink-device-instance.cpp`, `StartCapture`):
`IDeckLinkConfiguration::SetInt(bmdDeckLinkConfigVideoInputConnection)` and
`SetInt(bmdDeckLinkConfigAudioInputConnection)` (only when value > 0; failures logged,
non-fatal) → `EnableVideoInput(displayMode, pixelFormat, flags)` (failure fatal) →
`SetupVideoFormat` (derive active colorspace from mode flags when user chose default;
allocate `OBSVideoFrame` conversion frame) → `EnableAudioInput(48 kHz, 16-bit integer,
channel)` if channel format set (failure = warning only) → allocate `AudioRepacker` when
>2ch (or 7.1+swap) and `maxChannel >= 8` → `SetCallback(this)` → `StartStreams()`.
Auto mode: `displayMode = bmdModeNTSC`, pixel = 10-bit YUV iff `allow_10_bit` else 8-bit,
flag `bmdVideoInputEnableFormatDetection`; fixed mode: mode's `BMDDisplayMode`, chosen
pixel format, `bmdVideoInputFlagDefault`.

**Capture callbacks** (`VideoInputFrameArrived`): per frame — `HandleVideoFrame` then
`HandleAudioPacket`.
- Video: query `IDeckLinkVideoFrameAncillaryPackets`, first packet with DID `0x61` +
  SDID `0x01` ⇒ `HandleCaptionPacket` (ATSC A53 CDP bit parse → `obs_source_output_cea708`).
  Pixel format ≠ conversion format ⇒ `IDeckLinkVideoConversion::ConvertFrame` into
  `OBSVideoFrame`; else pass frame through. `GetBytes/GetRowBytes/GetWidth/GetHeight`;
  zero size ⇒ drop. Flag `bmdFrameContainsHDRMetadata` ⇒ query
  `IDeckLinkVideoFrameMetadataExtensions::GetInt(bmdDeckLinkFrameMetadataHDRElectroOpticalTransferFunc)`:
  2 ⇒ `VIDEO_TRC_PQ`, 3 ⇒ `VIDEO_TRC_HLG`, else default. Emit `obs_source_output_video2`.
- Audio: `GetBytes` + `GetSampleFrameCount`; when the source is **not** buffered,
  re-stamp timestamp = `now − frames/rate`; repack when a repacker exists; emit
  `obs_source_output_audio` at 48 kHz.

**Output start** (`StartOutput`): `EnableVideoOutput(mode, bmdVideoOutputFlagDefault)` →
`EnableAudioOutput(48 kHz, 16-bit, 2ch, bmdAudioOutputStreamTimestamped)` → keyer:
mode 1 ⇒ `Enable(true)` (external) + `SetLevel(255)`, mode 2 ⇒ `Enable(false)` (internal),
0 ⇒ `Disable()` → preroll: `max(MinimumPrerollFrames, 3)` black frames via
`CreateVideoFrame` + `ScheduleVideoFrame(i·frameDuration)` (pixel `bmdFormat10BitRGBXLE`
+ `HDRVideoFrame` wrapper when source is HDR and conversion is PQ, else `bmdFormat8BitBGRA`)
→ `SetScheduledFrameCompletionCallback(renderDelegate)` → `StartScheduledPlayback(0, 100, 1.0)`.
The completion callback copies the next queued host blob into the reclaimed frame and
schedules it at `totalFramesScheduled·frameDuration`; audio uses
`WriteAudioSamplesSync`. Stop: clear completion callback, `DisableVideoOutput`,
`DisableAudioOutput`.

**Stop/teardown (capture)**: `StopStreams`, release instance. Device removed ⇒
`DeckLinkDeviceRemoved` ⇒ notify device-change callbacks ⇒ input `Deactivate`.

### 3.2 Interface instances created (names only)

`CreateDeckLinkIteratorInstance`, `CreateDeckLinkDiscoveryInstance`,
`CreateVideoConversionInstance` (Windows: `CoCreateInstance` of `CLSID_CDeckLinkIterator` /
`CLSID_CDeckLinkDiscovery` / `CLSID_CDeckLinkVideoConversion`),
`IDeckLinkAPIInformation`, `IDeckLinkDiscovery`, `IDeckLinkDeviceNotificationCallback`,
`IDeckLinkProfileAttributes`, `IDeckLinkInput`, `IDeckLinkOutput`, `IDeckLinkKeyer`,
`IDeckLinkConfiguration`, `IDeckLinkDisplayModeIterator`/`IDeckLinkDisplayMode`,
`IDeckLinkVideoInputFrame`, `IDeckLinkAudioInputPacket`, `IDeckLinkVideoFrame`,
`IDeckLinkVideoFrameAncillaryPackets`, `IDeckLinkAncillaryPacketIterator`,
`IDeckLinkAncillaryPacket`, `IDeckLinkVideoConversion`, `IDeckLinkVideoFrameMetadataExtensions`,
`IDeckLinkMutableVideoFrame`, `IDeckLinkScheduledFrameCompletionCallback`.

## 4. Behaviour (numbered rules)

### 4.1 Discovery & devices

| # | Rule |
|---|---|
| S-083-R01 | `plugin-main.cpp`: load fails without a DeckLink iterator (drivers missing). SDK compiled + installed versions are always logged on load. |
| S-083-R02 | `decklink-device-discovery.cpp`: `DeckLinkDeviceArrived` creates + inits a device and fires all registered `DeviceChangeInfo` callbacks with `added=true`; `DeckLinkDeviceRemoved` matches by `IsDevice`, fires callbacks with `added=false`, releases and erases. `FindByHash` adds a reference. |
| S-083-R03 | `decklink-device.cpp`: identity hash = `"<PersistentID|TopologicalID>_<model name>"` (persistent preferred); display name shown in UIs; `decklink-devices.cpp` fills property lists with display name → hash. |
| S-083-R04 | `decklink-device-mode.cpp`: mode ids are assigned in iterator order starting at 1 (input and output lists independently); `IsEqualFrameRate` compares by cross-multiplication (`scale·den == duration·num`). |
| S-083-R05 | `util.cpp`: connection enum → name maps are exactly: video SDI/HDMI/Optical SDI/Component/Composite/S-Video; audio Embedded/AES/EBU/Analog/Analog XLR/Analog RCA/Microphone/Headphones. |

### 4.2 Capture source (`decklink-input`)

| # | Rule |
|---|---|
| S-083-R06 | `decklink-source.cpp`: source id `decklink-input`, flags `ASYNC_VIDEO|AUDIO|DO_NOT_DUPLICATE|CEA_708`, icon camera, `obs_source_set_async_decoupled(true)`. Settings keys: `device_hash`, `device_name`, `video_connection`, `audio_connection`, `mode_id`, `mode_name`, `channel_format`, `pixel_format`, `color_space`, `color_range`, `buffering`, `deactivate_when_not_showing`, `swap`, `allow_10_bit`. Defaults: buffering false, pixel 8-bit YUV, colorspace/range default, channel stereo. |
| S-083-R07 | `channel_format` sanitisation on update: 7 ⇒ 5.1; out of range ⇒ stereo. |
| S-083-R08 | Device-changed property callback: missing hash in list ⇒ re-insert old name+hash at index 0 **disabled**; rebuild video/audio connection lists from the device's connection bitmasks (in fixed enum order), mode list from input modes; channel list only offers 2.1/4.0/4.1/5.1/7.1 when `maxChannel >= 8`. |
| S-083-R09 | Mode-changed callback: `pixel_format` list hidden for `Auto` mode; `allow_10_bit` visible only for `Auto`. |
| S-083-R10 | `DecklinkInput.cpp`: `Activate` is a no-op (false) when device and all parameters (mode, both connections, pixel, colorspace, range, channel format, swap) are unchanged; otherwise stop, create `DeckLinkDeviceInstance`, `StartCapture`; on success increments `activateRefs` and `SaveSettings()` writes `device_hash`/`device_name`/`mode_id`/`mode_name` back into the source settings. |
| S-083-R11 | Device-change events: added + hash matches + source waiting (`activateRefs`) ⇒ auto re-activate; removed + current device ⇒ `Deactivate`. Every change calls `obs_source_update_properties`. |
| S-083-R12 | `deactivate_when_not_showing`: `show` re-activates, `hide` deactivates, only when the flag is set. |
| S-083-R13 | `decklink-device-instance.cpp` audio: audio is always delivered at 48 kHz 16-bit; unbuffered sources get wall-clock re-stamped timestamps (see §3.1). |
| S-083-R14 | CEA-708: caption ancillary = first packet with DID 0x61/SDID 0x01; A53 CDP parsed bit-by-bit (timecode section skipped when `cdp_timecode_added`); `cc_count` 3-byte CC words copied into `obs_source_cea_708` (packets = cc_count). |
| S-083-R15 | HDR: input frames with `bmdFrameContainsHDRMetadata` map EOTF 2→PQ, 3→HLG into `obs_source_frame2.trc`; colorspace default resolves from mode flags (2020 > 709 > 601). |
| S-083-R16 | `OBSVideoFrame.cpp`/`.h`: the conversion frame implements `IDeckLinkVideoFrame` (SetFlags/GetBytes/etc.), 16-bit YUV layout, width×height×2+1 bytes; timecode setters are no-ops. |

### 4.3 Output (`decklink_output`)

| # | Rule |
|---|---|
| S-083-R17 | `decklink-output.cpp`: output id `decklink_output`, `OBS_OUTPUT_AV`. On create/update reads `device_hash`, `mode_id`, `keyer`, `force_sdr`; video conversion = BGRA, full range, `VIDEO_CS_2100_PQ` when device supports HDR metadata and not `force_sdr`, else `VIDEO_CS_709`. |
| S-083-R18 | Start fails on: no audio info, empty device hash, missing device, missing video info, or mode frame-rate ≠ obs fps (log `"Start failed: FPS mismatch!"`). On success: audio conversion 16-bit stereo 48 kHz ("only format the decklink supports"), `SetSize`, keyer mode, `Activate`, `obs_output_begin_data_capture(out, 0)`. |
| S-083-R19 | Raw video: first frame sets `start_timestamp`; every frame copied into the free blob (`UpdateVideoFrame`). Raw audio: frames entirely before `start_timestamp` dropped; partial overlap cut (`cutoff` samples removed, timestamps shifted), then `WriteAudio` (`WriteAudioSamplesSync`). |
| S-083-R20 | Output property callbacks: missing device ⇒ old mode re-inserted disabled; mode list filtered to modes matching current obs fps; keyer list = Disabled/External/Internal per device flags. |
| S-083-R21 | `DecklinkOutput.cpp`: activate/deactivate mirrors the input: no-op when unchanged, else `StopOutput` + `StartOutput` on the shared `DeckLinkDeviceInstance`. |
| S-083-R22 | `audio-repack.c` (decklink): same repack model as AJA — 8ch 16-bit card audio squashed/reordered per target layout; only active when `maxChannel >= 8` and layout > stereo (or 7.1+swap). |

### 4.4 Captions plugin & output UI

| # | Rule |
|---|---|
| S-083-R23 | `decklink-captions.cpp`: separate module; registers only if the `decklink` module is loaded (checked in `obs_module_post_load`). Adds a Tools-menu "Decklink Captions" action; dialog lists all sources with `OBS_SOURCE_CEA_708` output flag. Selecting a source installs a caption callback that forwards CEA-708 to the streaming output **only while streaming is active** (`obs_frontend_streaming_active` + output active). The selected source name persists via a frontend save callback as `decklink_captions { "source": <name> }` and is restored on profile load (start). |
| S-083-R24 | `decklink-ui-main.cpp`: program output = `obs_output_create("decklink_output", "decklink_output")` started from saved settings; preview output same id with a rendered scene via `obs_output_set_media` (video queue + obs audio). Toggle: start creates, stop releases. |
| S-083-R25 | `DecklinkOutputUI.cpp`: dialog holds program + preview `OBSPropertiesView` for `decklink_output`; every change saves immediately; files in the module config dir: `decklinkOutputProps.json`, `decklinkPreviewOutputProps.json` (`obs_data_save_json_safe`, `.tmp`/`.bak`). Output button text tracks active state ("Start"/"Stop"). |

## 5. Concurrency

- Discovery: recursive mutex around the device vector; callbacks fire under lock.
- Input/output: per-instance mutex (`deviceMutex`) around activate/deactivate; SDK
  callbacks (video/audio/ancillary) run on the SDK thread and emit directly into the
  source — no queue on the capture side.
- Output: host video frames are copied into a ring of blobs; the scheduled-frame
  completion callback (SDK thread) recycles blobs; `frameQueueDecklinkToObs` /
  `frameQueueObsToDecklink` pair the two directions.
- Multiple instances may share a device; each `Activate` creates its own
  `DeckLinkDeviceInstance` (own input/output streams).

## 6. Reference defects and quirks

- **Q-1**: `StartCapture` logs the *video* connection value in the *audio* connection
  error message (`"Couldn't set input audio port to %d"` prints `bmdVideoConnection`) —
  cosmetic log bug; replicate the message text only if log-text conformance is required.
- **Q-2**: `DeckLinkDevice::Init` keeps going after a failed
  `BMDDeckLinkMinimumPrerollFrames` (default 3) — preroll is never 0.
- **Q-3**: hash falls back to display name when neither persistent nor topological ID is
  available (identity not stable across reconnects for very old devices).
- **Q-4**: `OBSVideoFrame` timecode setters are no-ops (always return `0`, not `S_OK`).
- **Q-5**: audio enable failure on capture is non-fatal (source continues video-only),
  while video enable failure is fatal.

## 7. Open questions

1. DeckLink is effectively Windows/macOS-only (the Linux platform file only provides the
   string-conversion stub; discovery yields no devices). Confirm the Rust build gates the
   crate on the same OS list.
2. `auto_start` (output property) is persisted by the UI but I did not verify the exact
   auto-start trigger path in `decklink-ui-main.cpp` — implementer should pin it during
   conformance (candidate: on profile load / preview scene change).
3. Whether the captions plugin's Tools-menu entry should also be re-implemented on the
   Rust frontend (it depends on Qt `QAction`) — behaviour is pinned in R23; the Qt glue
   itself is out of the FFI contract.
