# S-081 libdshowcapture FFI contract (kept C++ library, D-19)

| | |
|---|---|
| Status | Draft (round 8 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1, `deps/libdshowcapture` (C++ DirectShow wrapper, no-atl branch) |
| Target crate | `crates/dshow-capture` (the library **stays C++** per D-19; the Rust side only binds via the host plugin `plugins/win-dshow`, S-051) |
| Depends on | S-051 (win-dshow host — format constants pinned there, its §11) |
| Conformance | planned; none yet (all behaviour is Windows COM runtime, not reproducible in Linux test vectors) |

## 1. Scope

In scope: the exported API of `libdshowcapture` (`src/dshowcapture.hpp`,
the `DSHOWCAPTURE_EXPORT` symbols) as consumed by the `win-dshow` host —
device/format enumeration, the `Device` open/start/stop lifecycle, the
video/audio capture callback contract and data ownership, the
`VideoEncoder` class, `OutputFilter` (virtualcam path), the log callback,
and the behavioural rules of the internal COM layer the host relies on
(format negotiation, pin/crossbar connection, media-type conversion,
vendor quirks).

Out of scope: the `win-dshow` host plugin itself (S-051), DirectShow SDK
semantics, anything the host never touches.

**Reference files** (all 20; directory `deps/libdshowcapture/src/source/`):

- public surface & device: `deps/libdshowcapture/src/source/dshowcapture.cpp`, `deps/libdshowcapture/src/source/device.cpp`, `deps/libdshowcapture/src/source/dshow-enum.cpp`, `deps/libdshowcapture/src/source/dshow-formats.cpp`, `deps/libdshowcapture/src/source/dshow-media-type.cpp`, `deps/libdshowcapture/src/source/dshow-media-type.hpp`
- capture graph filters: `deps/libdshowcapture/src/source/dshow-base.cpp`, `deps/libdshowcapture/src/source/output-filter.cpp`, `deps/libdshowcapture/src/source/output-filter.hpp`, `deps/libdshowcapture/src/source/capture-filter.cpp`
- encoded devices & encoder: `deps/libdshowcapture/src/source/dshow-encoded-device.cpp`, `deps/libdshowcapture/src/source/dshow-demux.cpp`, `deps/libdshowcapture/src/source/dshowencode.cpp`, `deps/libdshowcapture/src/source/encoder.cpp`, `deps/libdshowcapture/src/source/encoder.hpp`
- vendor workarounds: `deps/libdshowcapture/src/source/device-vendor.cpp`
- utilities: `deps/libdshowcapture/src/source/ComPtr.hpp`, `deps/libdshowcapture/src/source/CoTaskMemPtr.hpp`, `deps/libdshowcapture/src/source/log.cpp`, `deps/libdshowcapture/src/source/external/IVideoCaptureFilter.h`

The public header itself is `deps/libdshowcapture/src/dshowcapture.hpp`
(not in the 20-file list; cited as the API contract).

## 2. Conventions

RFC 2119 words. Times are DirectShow `REFERENCE_TIME` (100-ns units)
unless stated. `W` = 8 (64-bit Windows ABI; the library is compiled as
C++/Cxx11 with `__declspec(dllexport)`; the Rust host never re-implements
it). `HRESULT` errors are surfaced through the log callback (§5.6) as
English (`ConvertHRToEnglish`).

## 3. Interface (exported symbols)

### 3.1 Constants and enums (`dshowcapture.hpp`)

- Version: `DSHOWCAPTURE_VERSION = MAKE_DSHOWCAPTURE_VERSION(0,10,0)`,
  `MAKE(major,minor,patch) = (major<<24)|(minor<<16)|patch`.
  `DSHOW_MAX_PLANES = 8`.
- `VideoFormat` (values pinned in S-051 §11 — **do not re-derive**):
  Any 0, Unknown 1, ARGB 100, XRGB 101, RGB24 102, I420 200, NV12 201,
  YV12 202, Y800 203, P010 204, YVYU 300, YUY2 301, UYVY 302, HDYC 303,
  MJPEG 400, H264 401, HEVC 402.
- `AudioFormat`: Any 0, Unknown 1, Wave16bit 100, WaveFloat 101, AAC 200,
  AC3 201, MPGA 202 (MPEG 1).
- `AudioMode`: Capture, DirectSound, WaveOut. `Result`: Success, InUse,
  Error. `InitGraph`: False, True. `DialogType`: ConfigVideo, ConfigAudio,
  ConfigCrossbar, ConfigCrossbar2. `LogType`: Error, Warning, Info, Debug.

### 3.2 Data structures (field order as exported)

- `DeviceId { std::wstring name; std::wstring path; }` — `path` =
  registry `DevicePath` (instance path).
- `VideoDevice : DeviceId { bool audioAttached; bool separateAudioFilter;
  std::vector<VideoInfo> caps; }`;
  `AudioDevice : DeviceId { std::vector<AudioInfo> caps; }`.
- `VideoInfo { int minCX, minCY, maxCX, maxCY, granularityCX,
  granularityCY; long long minInterval, maxInterval; VideoFormat format;
  }` — `minCY` may be negative (flip); granularity = step from min.
- `AudioInfo { int minChannels, maxChannels, channelsGranularity,
  minSampleRate, maxSampleRate, sampleRateGranularity; AudioFormat format;
  }`.
- `Config : DeviceId { bool useDefaultConfig = true; }`.
- `VideoConfig : Config { VideoProc callback; ReactivateProc
  reactivateCallback; int cx, cy_abs; bool cy_flip; long long
  frameInterval; VideoFormat internalFormat; VideoFormat format; }` —
  `cy_flip` records a negative request; resolved height is positive.
- `AudioConfig : Config { AudioProc callback; bool useVideoDevice;
  bool useSeparateAudioFilter; int sampleRate, channels; AudioFormat
  format; AudioMode mode; int buffer; }` — `buffer` = requested capture
  buffer in ms (0 → default, R24).
- Callbacks (host functors, stored by value):
  `VideoProc(const VideoConfig&, unsigned char *data, size_t size,
  long long startTime, long long stopTime, long rotation)`;
  `AudioProc(const AudioConfig&, unsigned char *data, size_t size,
  long long startTime, long long stopTime)`; `ReactivateProc()`.
- `VideoEncoderConfig : DeviceId { int fpsNumerator, fpsDenominator,
  bitrate, keyframeInterval, cx, cy; }` — bitrate kbps,
  keyframeInterval frames.
- `EncoderPacket { unsigned char *data; size_t size; long long pts, dts;
  }`.

### 3.3 `Device` class (all methods)

`Device(InitGraph initialize = False)`, `~Device()`, `Valid()`,
`ResetGraph()`, `ShutdownGraph()`, `SetVideoConfig(VideoConfig*)` /
`SetAudioConfig(AudioConfig*)` (pointer args; `NULL` clears that half),
`ConnectFilters()`, `Start() -> Result`, `Stop()`,
`GetVideoConfig(VideoConfig&)` / `GetAudioConfig(AudioConfig&)`,
`GetVideoDeviceId(DeviceId&)` / `GetAudioDeviceId(DeviceId&)`,
`OpenDialog(void *hwnd, DialogType)`,
`static EnumVideoDevices(std::vector<VideoDevice>&)`,
`static EnumAudioDevices(std::vector<AudioDevice>&)`.

### 3.4 `VideoEncoder` class and global log

`VideoEncoder()`, `~VideoEncoder()`, `Valid()`, `Active()`,
`ResetGraph()`, `SetConfig(VideoEncoderConfig&)`,
`GetConfig(VideoEncoderConfig&)`,
`Encode(unsigned char *data[8], size_t linesize[8], long long
timestampStart, long long timestampEnd, EncoderPacket&, bool
&new_packet)`, `static EnumEncoders(std::vector<DeviceId>&)`.

`DSHOWCAPTURE_EXPORT void SetLogCallback(LogCallback, void *param)` with
`LogCallback = void (*)(LogType, const wchar_t *msg, void *param)`.

## 4. Data formats

- Video frame callback: raw pixels in `VideoFormat` layout; `size` =
  actual sample byte length (not necessarily `cx*cy*bits/8` for
  compressed).
- Encoded (MJPEG/H264/HEVC, format ≥ 400) and encoded audio (≥ 200): the
  callback receives **assembled packets** (byte-stream chunks), not
  frames (R22).
- `VideoInfo` intervals are per-frame, 100-ns units.

## 5. Behaviour (numbered rules)

### 5.1 Device lifecycle (`dshowcapture.cpp`, `device.cpp`)

- **S-081-R01** `Device` owns an `HDevice` context created at construction; `Valid()` = context exists. `InitGraph::True` creates the filter graph (`IGraphBuilder` + `ICaptureGraphBuilder2` + `IMediaControl`) at construction; `False` defers to the first op that needs it (`EnsureInitialized`). `ResetGraph()` re-creates the context (graph included if `initialized`); `ShutdownGraph()` tears down the graph (and active control) only; both log on misuse.
- **S-081-R02** `SetVideoConfig`/`SetAudioConfig`/`ConnectFilters`/`Start` require the graph to exist and the device **inactive**; a call while active (or without a graph) fails (`false`/`Result::Error`) with an error log naming the function.
- **S-081-R03** `SetVideoConfig(cfg)`: `NULL` removes the video side (filter + capture filter), returns `true`. With a config: requires non-empty `name` or `path` (else "No video device name or path specified"), looks the device up by name or path, and on success **writes the resolved config back into `*cfg`** (actual `cx`, `cy_abs`, `cy_flip`, `frameInterval`, `format`, `internalFormat`). The caller MUST use the returned config.
- **S-081-R04** `SetAudioConfig(cfg)`: `NULL` clears the audio side. Device source selection: `useVideoDevice` (requires a video device already set — else "no video device is present"; name/path ignored) → `useSeparateAudioFilter` (audio filter of the video device's parent, R36) → else named lookup in `AudioInputDeviceCategory`. `mode == Capture` installs a capture filter; `DirectSound`/`WaveOut` install the matching render filter (`CLSID_AudioRender`/`CLSID_DirectSoundRenderer`) — audio is then *played*, not captured. On capture success the resolved config is written back into `*cfg`.
- **S-081-R05** `ConnectFilters()`: for video, first sets the vendor tonemapper enabled iff `videoConfig.format != P010` (R38), then connects device capture pin → `CaptureFilter`; a failing direct pin connect falls back to `RenderFilter`. Audio connects only if video succeeded (or there is no video); either failure returns `false`.
- **S-081-R06** `Start()`: runs the graph. Failure with HRESULT `0x8007001F` (`ERROR_DEVICE_IN_USE`) → `Result::InUse`; any other failure → `Result::Error`; success sets active → `Result::Success`. A pending rocket-encoded device sleeps `ROCKET_WAIT_TIME_MS` before `Run()`. `Stop()` calls graph `Stop()` only if active.
- **S-081-R07** `GetVideoConfig/GetAudioConfig` return the *current* resolved config (updated by media-type changes during capture, R25); `GetVideoDeviceId/GetAudioDeviceId` return the device id iff the corresponding capture filter exists.
- **S-081-R08** `OpenDialog(hwnd, type)`: ConfigVideo → property pages of the video filter; ConfigAudio → audio filter; ConfigCrossbar → the `IAMCrossbar` on the video filter (capture-graph `FindInterface`); ConfigCrossbar2 → the **upstream** crossbar (`LOOK_UPSTREAM_ONLY`). No filter/interface → warning log, no dialog. Dialogs are the stock OLE property frame, blocking.

### 5.2 Device and capability enumeration (`dshow-enum.cpp`, `dshowcapture.cpp`)

- **S-081-R09** `EnumVideoDevices` clears the output vector, enumerates `VideoInputDeviceCategory`. Per device the name comes from the property bag `FriendlyName`; a missing name or failed bind silently skips. **Encoded-device exceptions** (case-insensitive `wcsstr` on the name): `C875`, `Prif Streambox`, `C385`/`C835` → single-cap AV_LGP (MJPEG); `IT9910` → HD_PVR_Rocket (H264); `Hauppauge HD PVR Capture` → HD_PVR1 (H264) — in all three: `audioAttached = true`, `separateAudioFilter = false`, granularity 1, min==max size/interval.
- **S-081-R10** Devices without a standard video capture pin are probed as exception devices: output pin named `656` → HD_PVR2 caps; pin `TS Out` → Roxio caps; neither → skipped. Otherwise caps come from `IAMStreamConfig` enumeration of the capture pin (R11).
- **S-081-R11** `VideoInfo` entries per media type (`Get_FORMAT_VideoInfo_Data`): min/max CX/CY, granularity, min/max interval from the capability array when present, else min==max and granularity 1; `cy` keeps its sign (negative = flipped); format per R16. `AudioInfo` likewise from `Get_FORMAT_WaveFormatEx_Data` (rate/channel ranges, granularity, format by `wFormatTag`/bits).
- **S-081-R12** `VideoDevice.audioAttached` = the video filter also has an audio capture pin; if not, `separateAudioFilter` is set iff `GetDeviceAudioFilter` (R36) finds the audio filter, and `audioAttached` is then reported true. `EnumAudioDevices` enumerates `AudioInputDeviceCategory`; devices without an audio capture pin are skipped.
- **S-081-R13** Enumeration runs under a process-wide recursive mutex (`enumMutex`); re-entrant enumeration from a device callback is possible. Decklink crash workaround (Q-1) on audio enumeration: audio devices whose name contains `Decklink` are skipped unless a Decklink **video** device was seen in the same or a previous enumeration (`decklinkVideoPresent` latch, checked via `CheckForDLCallback` during video enumeration).
- **S-081-R14** `VideoEncoder::EnumEncoders` enumerates `KSCATEGORY_ENCODER` filters (video), name/path per device, skipping entries that fail to bind.

### 5.3 Format negotiation (`dshow-enum.cpp`, `device.cpp`, `dshow-formats.cpp`)

- **S-081-R15** `useDefaultConfig == true` (video): the pin's default media type is used (`IAMStreamConfig::GetFormat`; `E_NOTIMPL` falls back to the pin's current type); `config.format` and `internalFormat` are then forced to `Any` so the closest-match search accepts any format. (Audio: default used, **except** 24-bit audio → forced `Wave16bit` with default rate/channels and `useDefaultConfig` off — Q-4.)
- **S-081-R16** fourcc → `VideoFormat` (unknown → `Unknown`): `RGB2`,`RGB4`→XRGB; `ARGB`→ARGB; `I420`,`IYUV`→I420; `YV12`→YV12; `NV12`→NV12; `Y800`→Y800; `P010`→P010; `YVYU`→YVYU; `YUY2`→YUY2; `UYVY`→UYVY; `HDYC`→HDYC; `H264`→H264; `HEVC`→HEVC (only with `ENABLE_HEVC`, else unknown); `MJPEG`→MJPEG (the only compressed format with an auto-created intermediate decoder filter on connect). Audio tags: `WAVE_FORMAT_RAW_AAC1`→AAC, `WAVE_FORMAT_DVM`→AC3, `WAVE_FORMAT_MPEG`→MPGA, 16-bit→Wave16bit, 32-bit→WaveFloat, else Unknown.
- **S-081-R17** Closest-video match (§6): candidates with a format different from a requested `internalFormat != Any` are dropped; score = `frameVal + yVal + xVal + formatVal`, each `*Val` = distance of the requested value from the candidate range (0 inside); `formatVal` = format class rating: planar YUV (I420..P010) 0, packed YUV (YVYU..HDYC) 5, MJPEG 10, everything else (incl. raw RGB, H264/HEVC) 15. Strictly-better score replaces the best (ties keep the first); a perfect match (0) stops the scan.
- **S-081-R18** When the chosen candidate matches exactly on one dimension, the media type is rewritten on that dimension: `biWidth` = requested cx clamped to the range's granularity; `biHeight` = requested cy_abs clamped, sign = `cy_flip`; if frame distance is 0 and `|AvgTimePerFrame − requested| > 1` (100-ns units), `AvgTimePerFrame` = requested (Q-3, 29.97 fps).
- **S-081-R19** After selection the format is applied via `IAMStreamConfig::SetFormat`; failure other than `E_NOTIMPL` aborts setup (error log). A `CaptureFilter` is installed with the resolved major/subtype as expected pin media type; for XRGB/ARGB/RGB24/YVYU/YUY2/UYVY the expected subtype is pinned to the matching `MEDIASUBTYPE_*` constant (to force the right intermediate converter), else to the chosen subtype.
- **S-081-R20** `ConvertVideoSettings` (on every received sample whose media type changed): `cx = biWidth`, `cy_abs = |biHeight|`, `cy_flip = biHeight<0`, `frameInterval = AvgTimePerFrame`, `internalFormat` = fourcc mapping; `format` updates to `internalFormat` only if `format == internalFormat` already (the host's requested format is never overridden). `ConvertAudioSettings`: `sampleRate`, `channels` always; `format` per R16.

### 5.4 Capture callbacks and data ownership (`device.cpp`, `capture-filter.cpp`)

- **S-081-R21** Samples arrive on the DirectShow worker thread at `CapturePin::Receive`/`ReceiveMultiple` (in order; `ReceiveCanBlock` = false); a flushing pin drops samples (`S_FALSE`); each sample is passed straight to `HDevice::Receive`.
- **S-081-R22** `HDevice::Receive` rules, in order: null sample or missing callback → drop; `reactivatePending` → drop; video on a rotatable device (name contains `StreamCam`) reads `IAMCameraControl` `CameraControl_Roll` for the rotation argument (else 0); if the HDR signal changed (R26) and a reactivate callback exists, it fires and **this sample is dropped**. Otherwise: media type converted (R20), size = `GetActualDataLength` (0 or pointer failure → drop); raw (video format < 400 / audio < 200): callback invoked **iff** the sample has timestamps, with `data` = sample buffer, `size`, `startTime`, `stopTime` (100-ns), `rotation` (video); encoded: bytes appended to a pending buffer — a sample **with** timestamps first flushes the previously accumulated bytes as one packet (with the previous times) before appending its own; samples without timestamps only accumulate. The callback may therefore receive data *not* aligned to the just-received sample.
- **S-081-R23** **Ownership**: `data` points into a DirectShow media sample and is valid **only for the duration of the callback**; the host MUST copy or process synchronously. No synchronisation beyond "one worker thread per device".
- **S-081-R24** Audio capture buffering: on connect, if the audio source is *not* the video device's built-in audio of a device whose name contains `Stream Engine`, `IAMBufferNegotiation::SuggestAllocatorProperties` is set with `cbBuffer = nAvgBytesPerSec * buffer / 1000` (buffer ms, 0 → **10 ms**), `cBuffers/cbAlign/cbPrefix = -1` (auto); failure is a warning only. Applies only to `FORMAT_WaveFormatEx` media types.
- **S-081-R25** Media-type change events (R20) also rewrite the *stored* config, so a subsequent `GetVideoConfig` reflects what the device is actually delivering.
- **S-081-R26** HDR/reactivate: on `SetVideoConfig` the vendor HDR flag is read (Elgato `EGAVDeviceProperties`, R38) and the vendor encoder type pre-set (HEVC iff HDR, `ENABLE_HEVC` builds). During capture each video sample re-checks the flag; on a change the vendor format is re-set, `reactivateCallback()` called, `reactivatePending` set (frames dropped until the host re-`Start`s), flag cached.
- **S-081-R27** The capture pin validates its connection media type (`IsValidMediaType`): mismatching subtype/majortype rejected; video types with 0 width/height rejected.

### 5.5 `OutputFilter` / `VideoEncoder` (`output-filter.cpp`, `encoder.cpp`, `dshowencode.cpp`)

- **S-081-R28** `OutputFilter` is an `IBaseFilter` with one output pin (`IAMStreamConfig` + `IKsPropertySet`) used to *feed* frames into a graph (virtualcam). Construction with (format, cx, cy, interval) defines the initial media type; `AddVideoFormat` appends to the advertised list (`GetStreamCaps`/`EnumMediaTypes`); `GetNumberOfCapabilities` reports list size.
- **S-081-R29** `SetVideoFormat` (resize): rebuilds the pin media type (bitrate = `size * 10000000 / interval * 8`). If anything changed and a pin is connected, buffers are reallocated (allocator re-fetched or created — `cBuffers = 4`, `cbAlign = 32`, `cbPrefix = 0`, `cbBuffer = cx*cy*bits/8` — re-committed, downstream pin re-notified); if not connected, just update state.
- **S-081-R30** `Send(data[8], linesize[8], tStart, tEnd)`: allocates a sample from the pin's allocator, locks it, copies the planes (respecting `linesize`), `UnlockSampleData(tStart, tEnd)` delivers it with those timestamps. `LockSampleData`/`UnlockSampleData` are the explicit pair used around encoder round-trips; both fail (false / no-op) when no pin is connected.
- **S-081-R31** `VideoEncoder` lifecycle: constructed uninitialised; `Valid()` = graph initialised (device + encoder + output + capture filters present); `ResetGraph()` deletes/recreates the context; `SetConfig` while `Active()` first resets the context. Config requires the device (name/path) and encoder (name/path) to resolve; AVerMedia encoders get their settings via `IKsPropertySet` (`SetAVMEncoderSetting` clamps values to the property's min/max): fps, bitrate, current+encoded resolution, GOP — any failure aborts.
- **S-081-R32** `Encode(...)`: requires `Active()`. Pushes the input frame through `OutputFilter::Send`, records `timestampStart` into a pts queue; then, under `packetMutex`, if the encoder produced at least one packet (accumulated from the capture pin into `packets`), pops the oldest pair: `packet.data/size` point **into the library's internal buffer** (`curPacket`), `pts = dts` = the corresponding input timestamp; `new_packet = true` only then. Frames and packets pair by order (FIFO), not content.
- **S-081-R33** `EncoderPacket.data` is owned by the encoder until the next `Encode` (or reset); the host MUST consume/copy synchronously.

### 5.6 COM layer, logging, ownership (`dshow-base.cpp`, `log.cpp`, `dshow-demux.cpp`, `device-vendor.cpp`, `ComPtr.hpp`, `CoTaskMemPtr.hpp`, `dshow-media-type.cpp`, `external/IVideoCaptureFilter.h`)

- **S-081-R34** `CreateFilterGraph` creates the three COM objects (`CoCreateInstance`, inproc), logs "Failed to create filter graph" (error) on any failure; `LogFilters` (debug) dumps filter names after connect.
- **S-081-R35** `GetDeviceFilter(iid, name, path, &filter)`: non-empty `path` → device moniker bound by instance path; else devices scanned for `FriendlyName == name` (exact). Any COM failure logs a warning, returns false.
- **S-081-R36** Pin/filter helpers: `GetFilterPin` matches by (majortype, category, direction), preferring connected pins; `GetPinByName`/`GetPinByMedium`/`GetFilterByMedium` resolve by name or registry pin medium (`KSPROPERTY_PIN_MEDIUM`); `DirectConnectFilters` connects the first compatible output→input pin pair. `GetDeviceAudioFilter(videoPath, &filter)` resolves the video device's **parent** device instance path (via `DevicePath`/CM registry walking) and finds that parent's audio-capture filter (built-in mics, `useSeparateAudioFilter`).
- **S-081-R37** `MapPinToPacketID(pin, id)` sets `KSPROPERTY_CONNECTION_CONNECT_INFO` (`CONNECT_INFO_STREAM_ID`) on a demux output pin so an `IMpeg2Demultiplexer` maps packets back to the source stream. Encoded devices (`dshow-encoded-device.cpp`): graph = crossbar → device → (encoder →) MPEG2 demuxer, with video/audio demux pins named `DEMUX_VIDEO_PIN`/`DEMUX_AUDIO_PIN`; packet IDs mapped (video first). The Hauppauge "rocket" encoder (IT9910) is enabled via a magic `IKsPropertySet` GUID (Q-2). `CreateDemuxVideoPin/AudioPin` (`dshow-demux.cpp`) build the pin media types: video H264-only, audio AAC/AC3/MPGA — anything else warns and creates no pin.
- **S-081-R38** Vendor workarounds (`device-vendor.cpp`): `IsVendorVideoHDR` queries Elgato `EGAVDeviceProperties` (device type GC4K60SPlus) for the HDR signal; `SetVendorVideoFormat` sets the Elgato encoder type HEVC/AVC; `SetVendorTonemapperUsage`: AVerMedia legacy KSPROPERTY (`KSPROPSETID_AVER_HDR_PROPERTY` id 2), AVerMedia GC553 (`IKsControl` topology property, node = first `KSNODETYPE_DEV_SPECIFIC` node, GUID_GC553 id 11, byte 15 ← 0x02), or Elgato KS property — selected by probing which interface/property the device answers; failures on not-applicable paths are silent.
- **S-081-R39** Logging: `SetLogCallback` stores callback+param (no lock). `Log` formats into a 4096-`wchar_t` stack buffer (`vswprintf_s`; **format overflow is silent**) and calls the callback; `*HR` variants append `: <hr value>` plus the English `HRESULT` description (`ConvertHRToEnglish`); with no callback set, messages are lost.
- **S-081-R40** Ownership helpers: `ComPtr<T>` = COM RAII (AddRef on construct/copy-from-raw, Release in destructor/clear; `ComQIPtr` = + `QueryInterface`); `CoTaskMemPtr<T>` frees via `CoTaskMemFree`. `MediaType` (value type wrapping `AM_MEDIA_TYPE`): copy/assign deep-copies `pbFormat` with `CoTaskMemAlloc` and AddRefs `pUnk` (`CopyMediaType`); release frees both (`FreeMediaType`); `AllocFormat<T>()` replaces `pbFormat` with a zeroed `CoTaskMemAlloc`'d `T`. `IVideoCaptureFilter.h` is a *header-only reference interface* (the AVerMedia contract: device-type/input/profile/colour-range/notification enums) used for vendor negotiation; the library does not implement it.

## 6. Algorithms

Closest video media type selection (R17–R18), verbatim logic:

```
best = none
for mt in EnumPinCaps(capture pin):        # IAMStreamConfig caps
    if mt not FORMAT_VideoInfo: continue
    info = parse caps ranges (min==max, gran=1 if no cap array)
    if config.internalFormat not in (Any, info.format): continue
    xVal  = dist(config.cx, [info.minCX, info.maxCX])
    yVal  = dist(config.cy_abs, [|info.minCY|, |info.maxCY|])
    fVal  = dist(config.frameInterval, [info.minInterval, info.maxInterval])
    rVal  = 0 if I420<=info.format<P010 else 5 if <MJPEG else 10 if ==MJPEG else 15
    if best==none or best.score > fVal+yVal+xVal+rVal:
        best = (mt, score)
        # rewrite mt on exactly-matching dims (granularity clamp, cy_flip sign,
        # AvgTimePerFrame if |Δ|>1)
        if score == 0: break
return best.found ? best.mt : fail
```

`dist(v,[a,b]) = a−v if v<a else b−v if v>b else 0`.

## 7. Concurrency

- Capture callbacks (R21–R23) run on the DirectShow worker thread for that graph — never the caller's thread. The host (`win-dshow`) copies data and marshals to the obs source thread (S-051's job).
- `HDevice`/`HVideoEncoder` are **not** internally locked except `HVideoEncoder::packetMutex` (encoder packet queue) and the global `enumMutex` (device enumeration). `SetLogCallback` and the log callback have no locking.
- One `Device` per graph; `Start`/`Stop` are not re-entrant; config setters must not run while active (R02).

## 8. Reference defects and quirks

- **Q-1** Decklink audio-enumeration crash workaround (R13): audio devices named `*Decklink*` are skipped unless a Decklink video device is present (`decklinkVideoPresent` latch). Preserve the observable behaviour (no audio entries, no crash), not the mechanism.
- **Q-2** Rocket (IT9910) encoder enabled through a magic `IKsPropertySet` call, GUID `{0x99100000,a330,11e1,a3,80,99,10,68,64,00,00}`, undocumented struct — only the effect (device starts emitting) is contractual.
- **Q-3** `AvgTimePerFrame` is overwritten only when the difference is > 1 (100-ns) to fix GV-USB2 29.97 fps; exact-nominal devices keep the device value.
- **Q-4** 24-bit audio defaults are forced down to `Wave16bit` (rate/channels kept) — no 24-bit path downstream.
- **Q-5** `Stream Engine` built-in audio must not get allocator buffer suggestions (device bug, R24).

## 9. Implementation notes (non-normative)

- D-19: the library **remains C++**, consumed only from the C++-hosted
  `win-dshow` plugin; no Rust re-implementation planned. A Rust host crate
  is the *host-side* FFI boundary of S-051, not this library.
- If any future Rust binding is added: expose `Device`/`VideoEncoder` as
  opaque handles; callbacks as function pointers with a trampoline; keep
  `VideoConfig`/`AudioConfig` as owned structs (copy in, copy the
  *resolved* value out per R03/R04); never retain callback buffers.
- Windows-only; all rules unverifiable by cross-platform vectors —
  conformance will be host-side integration tests on Windows.

## 10. Conformance coverage

| Rule(s) | Scripts |
|---|---|
| all S-081-Rxx | none yet (Windows COM runtime; no Linux-reproducible vectors) |

## 11. Open questions

1. `ROCKET_WAIT_TIME_MS` and the exact `RocketPropStruct` field semantics were not fully decoded (magic numbers, Q-2) — treat as opaque.
2. AVerMedia tonemapper v1 KSPROPERTY id `2` vs GC553 path selection order (which probe runs first when several succeed) not pinned; the observable effect (tonemapper on/off) is specified, the order is not.
3. `AudioMode::DirectSound`/`WaveOut` render paths were verified at filter selection only (`CLSID_AudioRender`, `CLSID_DirectSoundRenderer`); playback behaviour beyond "no capture callbacks fire" was not exercised.
4. `OutputPin::Connect`'s full allocator-handshake branches (external allocator negotiation) were not read line by line; R29 pins the reallocation path the host uses.
