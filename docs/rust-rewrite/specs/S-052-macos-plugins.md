# S-052 macOS plugins (`mac-capture`, `mac-avcapture`, `mac-videotoolbox`, `mac-virtualcam`) - behavioural catalog

| | |
|---|---|
| Status | Ready for review (Tier C catalog; Objective-C / Apple frameworks, **nothing here is reference-produced**) |
| Reference | OBS Studio 33.0.1: `plugins/mac-capture` (SCK video/audio, legacy display/window capture, CoreAudio), `plugins/mac-avcapture`, `plugins/mac-videotoolbox/encoder.c`, `plugins/mac-virtualcam/src/common/MachProtocol.h` |
| Target crate | `crates/obs-mac-capture`, `crates/obs-mac-avcapture`, `crates/obs-mac-videotoolbox` (platform glue is out of scope; this spec fixes the decision logic) |
| Depends on | obs-data / obs-properties specs (settings defaults, `has_user_value`, list items, visibility), S-025 (encoder option parsers; not duplicated), audio-io (`get_audio_channels`) |
| Conformance | `test/conformance/scripts/maccapture/` (11 scripts, runner `gen/maccapture_runner.c`, README section "maccapture runner (spec S-052)") |

## Verification classes

- **[MODEL]** The rule was read from the Objective-C/C source and re-implemented as a small C function in the runner. The vector is **source-derived, not reference-produced**: macOS frameworks cannot run on this Linux host, so no vector in this suite was produced by the reference. Implementers must follow the rule text; the vectors pin only the author's reading.
- **[REAL]** Only one thing is real C: `get_audio_channels()` and the speaker/format enums from `libobs/media-io/audio-io.h` / `video-io.h` (used unchanged by the plugins).
- **[SRC]** Source-derived, no vector (needs a Mac, GPU, permissions dialogs, AVFoundation/CoreAudio/VideoToolbox objects). Must be tested on macOS.
- Apple constant values are written symbolically in the vectors (for example `420YpCbCr8BiPlanarVideoRange`); numeric FourCC/flag values quoted here come from the author's knowledge of the SDK headers, not from the repository, and must be checked against the SDK: `kAudioFormatFlagIsFloat=0x1`, `kAudioFormatFlagIsSignedInteger=0x4`, `kAudioFormatFlagIsNonInterleaved=0x20` (used by vector inputs).
- macOS version is written `major*100+minor` in vectors (12.5 -> 1205).

## 1. Scope

Covered, in priority order: (1) `screen_capture` (ScreenCaptureKit video; settings, defaults, property visibility, update/restart decision, stream configuration, frame-size tracking, window/application/display list building, display-id migration); (2) `sck_audio_capture`; (3) legacy `display_capture` / `window_capture` (settings, crop geometry, window matching); (4) CoreAudio capture (`coreaudio_input_capture`, `coreaudio_output_capture`); (5) `macos-avcapture` / `macos-avcapture-fast` decision logic and text tables; (6) `mac-videotoolbox` encoder property/profile/rate-control/colour logic (S-025 option parsers excluded); (7) `mac-virtualcam` protocol constants.

**Not covered** (needs a Mac or is pure framework glue): all Metal/OpenGL/IOSurface texture handling and the `DrawD65P3` effect; the SCK/CGDisplayStream/CGWindowList delivery threads and locking; `AVCaptureSession` construction, device discovery, `AVCaptureDeviceFormat` listing (`AVCaptureDeviceFormat+OBSListable.m`), the sample-buffer to `obs_source_frame` conversion in `OBSAVCapture.m` (beyond the tables in section 8), effects warnings (`effectsWarningForDevice`), the legacy `resolution` JSON setting (only described in R8-14); `VTCompressionSession` creation, `vt_encode` / annex-B conversion / SPS/PPS extraction (lines 814-1180 of `encoder.c`), ProRes encoder-id discovery; **`mac-virtualcam` beyond the three message ids** (CMIO DAL plug-in, Mach port server, `CMSampleBuffer` plumbing; not portable); locale strings.

## 2. Terminology

- Setting names are exact (saved scenes depend on them). A boolean/int that is absent with no default reads `false`/`0`; an absent string reads `""`.
- *Capture type* of `screen_capture`: `0` display, `1` window, `2` application. For `sck_audio_capture`: `0` desktop audio, `1` application audio.
- `hide_cursor` inside the source is `!show_cursor` (the setting is positive, the field negative).
- "restart" means: destroy the SCStream, adopt new values, create and start a new stream.

## 3. Source ids and registration (R1)

- **S-052-R01 [SRC]** Module `mac-capture` registers, in this order: if macOS >= 12.5: `screen_capture`; if additionally macOS >= 13: legacy `display_capture`, `window_capture` and `coreaudio_output_capture` get `OBS_SOURCE_DEPRECATED` OR-ed into their output flags and `sck_audio_capture` is registered; then (always) `display_capture`, `window_capture`, `coreaudio_input_capture`, `coreaudio_output_capture`. Ids must be preserved.
- **S-052-R02 [SRC]** Output flags: `screen_capture` = VIDEO|CUSTOM_DRAW|DO_NOT_DUPLICATE|SRGB|AUDIO; `window_capture` = ASYNC_VIDEO; coreaudio input = AUDIO|DO_NOT_DUPLICATE; coreaudio output = AUDIO|DO_NOT_DUPLICATE|DO_NOT_SELF_MONITOR. `mac-avcapture` registers `macos-avcapture` and `macos-avcapture-fast`.
- **S-052-R03 [SRC]** Display name of `screen_capture` is `SCK.Name` on macOS >= 13 and `SCK.Name.Beta` below.

## 4. `screen_capture` (ScreenCaptureKit video)

### 4.1 Settings

| key | type | default | meaning |
|---|---|---|---|
| `type` | int | 0 | capture type |
| `display_uuid` | string | NULL (no default) | UUID of display (types 0, 2) |
| `display` | int | - | legacy display id, migrated away (R4-9) |
| `window` | int | 0 (`kCGNullWindowID`) | CGWindowID (type 1) |
| `application` | string | NULL | bundle identifier (type 2) |
| `show_cursor` | bool | true | |
| `hide_obs` | bool | false | exclude OBS's own app (type 0 only) |
| `show_empty_names` | bool | false | list windows without title/app (type 1) |
| `show_hidden_windows` | bool | false | include off-screen windows |

- **S-052-R04 [MODEL]** (`sck_settings.script`) Defaults are exactly as in the table. `sck_audio_capture` defaults: `application` NULL, `type` 0.

### 4.2 Property visibility

- **S-052-R05 [MODEL]** The always-present properties are, in order: `type` (int list: 0 display, 1 window, 2 application), `display_uuid` (string list), `application` (string list), `window` (int list), `show_empty_names`, `show_hidden_windows`, `show_cursor`, `hide_obs`, button `reactivate_capture` (enabled only while `capture_failed`). Visibility by the source's *current* capture type:

  | type | display_uuid | window | application | show_empty_names | show_hidden_windows | hide_obs |
  |---|---|---|---|---|---|---|
  | 0 | yes | no | no | no | no | yes |
  | 1 | no | yes | no | yes | yes | no |
  | 2 | yes | no | yes | no | yes | no |

  For any other `type` value nothing is changed (all properties stay visible, as created). `show_cursor` is always visible. The modified-callbacks (`type`, `show_hidden_windows`, `show_empty_names`) apply the same table, but only `if sc->capture_type != new type` (so toggling `show_hidden_windows` alone never re-applies visibility), then store `show_empty_names`, `show_hidden_windows`, `hide_obs` into the live source, rebuild the content list and the three lists.
- **S-052-R06 [MODEL]** Below macOS 13 two extra text properties exist: `audio_info` (warning) and `capture_type_info` (error). `capture_type_info` is visible when the source's type is 0 or 2, hidden for 1; with no source instance it is hidden. In the type-changed callback it is visible for new type 0 and 2, hidden for 1. [SRC]
- **S-052-R07 [MODEL]** Content query (`SCShareableContent`): `excludingDesktopWindows = YES` always; `onScreenWindowsOnly = NO` when the list is being built for display capture (type 0), otherwise `!show_hidden_windows`. (`sck_content_flags`.) The audio source asks with "display capture" meaning desktop-audio type (0).

### 4.3 Lists

- **S-052-R08 [MODEL]** (`sck_lists.script`) Every list starts with a blank first entry so that opening the dialog does not capture the first enumerated item: display list `(" ", NULL)`, window list `(" ", 0)`, application list `(" ", NULL)`.
  - Window list: keep windows whose owning application name and title are both non-empty, unless `show_empty_names`; sort by application name then title, **case-insensitive** (`NSCaseInsensitiveSearch`; for non-ASCII use a Unicode case-insensitive compare, Foundation's exact collation is not specified here); label `"[<app>] <title>"`, value = window id. Sort stability is not guaranteed by Foundation; items equal under the comparator may appear in either order. With `show_empty_names` and a nil app name, the comparator input is nil (undefined; label becomes `"[(null)] ..."` in Foundation; the vector uses the empty string).
  - Application list: keep applications with non-empty name, sort by name case-insensitively, label = name, value = bundle identifier.
  - Display list: for each shareable display that has a matching `NSScreen` (match by `NSScreenNumber` == displayID; displays without one are skipped), label `"%.200s: %.12sx%.12s @ %.12s,%.12s"` of `localizedName`, width (`uint32` of frame width), height, origin x (`int32`), origin y; value = the display UUID string. (`sck_display_name`.)
- **S-052-R09 [MODEL]** Display-id migration `get_display_migrate_settings`: let `L = has_user_value("display")`, `N = has_user_value("display_uuid")`.
  - `L && !N`: convert the legacy id to a UUID, write `display_uuid`, erase `display`; if the UUID cannot be created return display id 0 **without** erasing anything.
  - `L && N`: erase `display` (new wins).
  - Then if `display_uuid` is non-empty return the display id for that UUID (0 if unknown), else 0.
  This mutates the settings object it is given, also when called from `update`.

### 4.4 Create / update

- **S-052-R10 [MODEL]** (`sck_update.script`) `update(settings)`:
  1. `new_window = settings.window`; if `new_window > 0 && new_window != old_window` store it into the source **immediately**, even if the stream is then kept. A `window` setting of 0 never clears a stored window id.
  2. If the new `type` equals the current type, the stream is **kept** (nothing happens) iff, per type: 0: `display` unchanged and cursor setting unchanged and `hide_obs` unchanged; 1: window id unchanged (after step 1) and cursor setting unchanged; 2: `display` unchanged, `application` string equal and cursor setting unchanged. Otherwise restart with the new values (`type`, `display`, `application`, `hide_cursor = !show_cursor`, `hide_obs`, `show_empty_names`, `show_hidden_windows`).
  3. Quirk Q-1: the cursor setting is part of the keep test, so toggling the cursor **restarts** the stream instead of updating `showsCursor` live. In the reference the expression is written `sc->hide_cursor != show_cursor`, which equals "unchanged" only because the field is negated; do not port it literally.
  4. Quirk Q-2: `show_empty_names`/`show_hidden_windows` changes never restart a running stream; they only affect list building.
- **S-052-R11 [SRC]** `create`: reads `show_cursor`, `hide_obs`, `show_empty_names`, `show_hidden_windows`, `window`, `type`, `application`, resolves the display via R09, requires the base effect (`DEFAULT_RECT` on OpenGL, `DEFAULT` otherwise) else fails (returns NULL), builds the content list and starts the stream. A failing `init_screen_stream` fails creation.
- **S-052-R12 [SRC]** Target resolution: display types need a display with id == resolved id; if none: error log, `disp = NULL`, stream not started, creation/`update` still succeeds (returns true). Window type needs `window != 0` and a window with that id, else the same error path. Application type resolves the display the same way; an application whose bundle id does not match leaves the target nil and `initWithObjects:nil` yields an empty include list (captures nothing; no error).

### 4.5 Stream configuration

- **S-052-R13 [MODEL]** (`sck_config.script`) Configuration applied for every started stream: queue depth 8; `showsCursor = !hide_cursor`; colour space name Display P3; background colour clear; minimum frame interval = 0.9 x (`fps_den / fps_num`) seconds, taken from the active OBS video info (if unavailable the interval is left unset and a warning logged); pixel format `64RGBAHalf` on macOS >= 15, else `32BGRA`; on macOS >= 13: audio capture on, exclude current process audio on, channel count 2.
  - Display type: width/height = **pixel** size of the display's current display mode. Application type: same, plus `includeMenuBar = YES` on >= 14.2, filter = display including that application, excepting no windows.
  - Window type: width/height = window frame size in points truncated to integers; filter = desktop-independent window; `includeChildWindows = YES` on >= 14.2.
  - Display type with `hide_obs`: filter excludes the application whose bundle identifier equals the main bundle's (if not found, the exclusion array contains nil and is effectively empty); otherwise excludes no windows.
  - Before macOS 13 only window type creates a stream; for types 0 and 2 init "succeeds" without a stream (`disp = NULL`).
  - The `min_frame_interval` is the exact `CMTime` produced by `CMTimeMultiplyByFloat64(CMTimeMake(den, num), 0.9)`; vectors give only the real-valued seconds.
- **S-052-R14 [SRC]** Failure handling: a stream-output registration failure logs an error and returns failure (creation fails, update ignores); a start failure logs and releases the stream (`disp = NULL`) but the call returns false only when the start completion reports failure. A `didStopWithError` delegate call sets `capture_failed = true` and asks for a property refresh (enabling `reactivate_capture`); user-stop and "no capture source" get dedicated log texts, any other code logs `Stream stopped with error <code> ("<desc>")`, all at warning level. `reactivate_capture` returns false (and warns) when `capture_failed` is false; otherwise restarts the stream and disables the button.

### 4.6 Frame handling

- **S-052-R15 [MODEL]** (`sck_frames.script`) Delegate dispatch: a screen-type sample is a video update iff the source is **not** `audio_only`; an audio-type sample is an audio update iff macOS >= 13; everything else (including a screen sample for an audio-only source) is dropped; a delegate with a null source pointer ignores everything. Note the `else if` structure: a screen sample for an audio-only source falls through to the audio check, where it fails the `type == Audio` test.
- **S-052-R16 [MODEL]** Window type size tracking: unless the frame details errored, content size is `content_rect.size / points_to_pixels * scale_factor` (32-bit float arithmetic, result stored as double). If it differs from the stored `frame.size`, store it and push the new width/height (truncated to `size_t`) to the stream via `updateConfiguration`. Other types compare the pixel-buffer width/height as `int` against the truncated stored size. If the scale-factor or content-rect attachments are missing the corresponding defaults are `scale_factor = 1` and (rect) the window size is left zero; any conversion failure sets `frame_detail_errored`, which suppresses the size update for that frame. `get_width/height` report the truncated stored size (0 until the first frame).
- **S-052-R17 [SRC]** The tick swaps `current` into `prev` under the mutex only if the source is showing and a new surface exists, rebinds or creates the texture, and releases the old surface; render draws with sRGB framebuffer enabled through technique `DrawD65P3`.
- **S-052-R18 [MODEL]** `video_get_color_space(preferred[])`: return `SRGB_16F` if listed, else `709_EXTENDED` if listed, else `SRGB` if listed, else `SRGB_16F`.

## 5. `sck_audio_capture`

- **S-052-R19 [MODEL]** (`sck_audio.script`) Stream setup: display filter for the main display (`CGMainDisplayID`), `capturesAudio`, `excludesCurrentProcessAudio`, no cursor, queue depth 8; filter type 0 = whole display excluding no windows, type 1 = include the application with the configured bundle id. Channel count = number of channels of the OBS speaker layout, but **2 if it has more than 1**. If OBS audio is not configured: error log `No audio configured`, init returns false. A dummy screen output is registered to silence SCK errors; its frames are dropped (R15). `application` visible only for audio type 1 (initial properties and the callback; the callback also rebuilds the list for type 1 only).
- **S-052-R20 [MODEL]** Sample buffer to `obs_source_audio`: reject (error log, drop) if `mChannelsPerFrame < 1`. Otherwise `speakers = channels`, `samples_per_sec = (uint32)sample_rate`, `format = FLOAT_PLANAR`, `frames = bytes / bytes_per_frame / channels`, plane `i` starts at `(bytes / channels) * i` (integer division), `timestamp = (uint64)(pts_seconds * 1e9)` (double multiply, truncation).
- **S-052-D1 DEFECT** Plane pointers are written to `audio.data[i]` for `i < mChannelsPerFrame` without bounding by `MAX_AV_PLANES` (8). More than 8 channels overflows the array; `speakers` is also not a valid layout. Required behaviour: reject buffers with more than 8 channels (log error), as for R20's zero case. Division by zero for `bytes_per_frame == 0` is likewise to be rejected. The vector shows the reference's offsets with an OOB marker.

## 6. Legacy `display_capture` / `window_capture`

- **S-052-R21 [MODEL]** (`legacy_window_display.script`) `display_capture` defaults: `display_uuid` = UUID of the main screen, `show_cursor` true, `crop_mode` 0, plus the window defaults `window` 0 and `show_empty_names` false. `window_capture` defaults: `show_shadow` false plus window defaults. Window-capture image option is `kCGWindowImageDefault` (includes shadow) when `show_shadow`, else bounds-ignoring-framing (`kCGWindowImageBoundsIgnoreFraming`) [SRC for constants].
- **S-052-R22 [MODEL]** Crop modes: 0 none, 1 manual, 2 to window, 3 to window and manual. A stored `crop_mode` outside 0..3 loads as 0. Crop rectangle settings (doubles) are loaded only for modes that use them, with names `manual.origin.x`, `manual.origin.y`, `manual.size.width`, `manual.size.height` (mode 1) and `window.origin.x` etc. (mode 3); for modes 0 and 2 the rectangle is left as it was.
- **S-052-R23 [MODEL]** Visibility on `crop_mode` change: `manual.*` visible iff mode 1; `window.*` crop properties visible iff mode 3; the `window` list and `show_empty_names` visible iff mode 2 or 3.
- **S-052-R24 [MODEL]** Output size (`dc_size`): mode 0 = frame size (truncated); mode 1 = `|frame - (crop.origin + crop.size)|` per axis, truncated, where `crop.origin` is x/y and `crop.size` is width/height; mode 2 = window rect size; mode 3 = `|window - (crop.origin + crop.size)|`. Draw rectangle (`dc_rect`): mode 1/3 start at origin = crop origin and end = -crop size; mode 0/1 add the frame size to the end; mode 2/3 add `window.origin - frame.origin` to origin and `window.size + that offset` to the end. Computed in `double`, narrowed to `float`. Modes 2/3 draw nothing while the window is off-screen.
- **S-052-R25 [MODEL]** Window matching (`window-utils.m`): the window table is `CGWindowListCopyWindowInfo(OnScreenOnly)` sorted by (owner name, owner pid, window name, window number), `NSString compare:` (case-sensitive literal).
  - `init`: skip if both owner and window names are empty. First entry in table order for which `owner == saved owner_name` **and** (`(pid == owner_pid && number == window)` **or** `window_name == saved window_name`) wins; it writes `window` and `owner_pid` back into the settings. Name matches therefore beat id matches that sort later.
  - `find` (re-search): needs owner and name both equal; rate-limited to once per 500 ms unless forced; writes `window` and `owner_pid` back.
  - List (`window_changed`): label `"[<owner>] <name>"` (empty string when owner empty); if the saved window is absent a **disabled** item with the saved owner/name and id is inserted at its sorted position (before the first entry that the key compares below, comparing owner then name only), otherwise appended last; entries with an empty name are hidden unless `show_empty_names` or they are the currently selected id; when the selected window exists its owner/name are written back to settings (`owner_name`, `window_name`).

## 7. CoreAudio capture

- **S-052-R26 [MODEL]** (`coreaudio.script`) Defaults: `device_id = "default"`, `enable_downmix = true`.
- **S-052-R27 [MODEL]** `convert_ca_format(flags, bits)`: float flag -> `FLOAT` / `FLOAT_PLANAR`; else not signed-int and `bits == 8` -> `U8BIT(_PLANAR)`; else not signed-int -> UNKNOWN; signed int: 16 -> `16BIT(_PLANAR)`, 32 -> `32BIT(_PLANAR)`, anything else (24, 8, ...) -> UNKNOWN. "Planar" = non-interleaved flag. UNKNOWN fails init with a warning (`unknown format flags: %u, bits: %u`). A non-PCM format id also fails (`format is not PCM`).
- **S-052-R28 [MODEL]** `convert_ca_speaker_layout(channels)`: 1 MONO, 2 STEREO, 3 2.1, 4 4.0, 5 4.1, 6 5.1, 8 7.1, others UNKNOWN. (Defined in the reference but unused by the init path, which takes the layout from OBS's audio info.)
- **S-052-R29 [MODEL]** Device resolution: uid `default` (case-insensitive compare): input source -> follow the system default input device (and register a default-device listener); output source -> the **first device in the output list** replaces the uid (if the list is empty: `no_devices`, init fails). Other uids are resolved with `kAudioHardwarePropertyDeviceForUID`.
- **S-052-R30 [MODEL]** Device enumeration: a device is listed only if it has input streams. The *output* list (macOS has no real output capture) contains only devices whose name contains, case-insensitively, one of `soundflower`, `wavtap`, `soundsiphon`, `ishowu`, `blackhole`, `loopback`, `groundcontrol`, `vbcable` - note the function is named `device_is_input` and returns true for everything **not** matching; output listing uses its negation. Input list contains every device. The device combo's first entry (if there is any device) is `("Default", "default")`.
- **S-052-R31 [MODEL]** `sanitize_device_name`: take the last 64 bytes of the uid; copy `isalnum` chars; map `-`, space, `_`, `:` to `_`; drop everything else. Used for per-device settings keys `output-<sanitized>-<n>` (n = 1..8). (Note: the 64-byte window is applied before dropping characters.)
- **S-052-R32 [MODEL]** Channel map (only when `enable_downmix` is false): for output channel `i` in 0..7 the setting `output-<dev>-<i+1>` is used only if it `has_user_value`; value `v > 0` -> map entry `v - 1`, anything else (absent, 0, negative) -> `-1` (muted). At init the map is applied to the OBS channel count; entries `>= available_channels` become `-1`; the log line is `Channel map enabled: [a,b,...] (N channels available)`. With downmix enabled the unit outputs `available_channels` channels (clamped to 64, `MAX_DEVICE_INPUT_CHANNELS`) and libobs mixes; log `Downmix enabled: %d to %d channels.`.
- **S-052-R33 [MODEL]** Per-channel combo for output channel `o` (0-based): the first item is channel `o+1` if `o < available`, else `None` (-1); then every other input channel in order with value `c+1`; `None` last unless already first. Property label `"<CoreAudio.Channel> <o+1>"`. Visible channel properties = `get_audio_channels(speakers)` when downmix is off; all `output-*` properties are hidden when downmix is on. Channel names are the device's per-channel names, falling back to `"<CoreAudio.Channel.Device> <n>"` (n 1-based).
- **S-052-R34 [MODEL]** Reconnect: a device-alive / format / default-device notification stops and uninitialises the unit, sets retry delay **300 ms if the notification was the default-device change, otherwise 2000 ms**, logs `coreaudio: device '%s' disconnected or changed.  attempting to reconnect`, and retries `init` in a thread every delay until success or shutdown. Initial failure: delay 2000 ms; if `no_devices` log `no device found`, else start the reconnect thread (`failed to find device uid: %s, waiting for connection`).
- **S-052-R35 [MODEL]** Capture callback: planar buffers, one per available/mapped channel (`mChannelsPerFrame` buffers of 1 channel each); `speakers = min(buffers, 8)`; timestamp = host time converted to ns. (A >8 buffer count is clamped for `speakers` but only the first 8 data pointers are set.)
- **S-052-R36 [SRC]** `update`: for the output source, a changed `device_id` notifies `obs_source_audio_output_capture_device_changed` (also at create, and with NULL at destroy for output sources); every `update` fully shuts down and re-initialises the unit.

## 8. `macos-avcapture` / `macos-avcapture-fast`

- **S-052-R37 [MODEL]** (`avcapture.script`) Defaults: `macos-avcapture`: `device` "", `use_preset` true, `preset` = `AVCaptureSessionPresetHigh`, `enable_audio` true. `macos-avcapture-fast`: `device` "", `use_preset` **false**, `enable_audio` true (no `preset` default).
- **S-052-R38 [MODEL]** Preset labels: Low->"Low", Medium->"Medium", High->"High", then `WxH` of the preset for 320x240, 352x288, 640x480, 1280x720, 1920x1080, 3840x2160; **Q-3: the 960x540 preset is labelled "960x460"** (reference typo; keep, it is user visible only); unknown preset -> `Unknown (<preset>)`.
- **S-052-R39 [MODEL]** `update` decision: (a) device uuid differs from the current -> switch device (failure: refresh properties, return NO), then reconfigure; (b) same device, preset-based and `use_preset` and preset string equals the session preset -> **nothing**; (c) otherwise preset (`use_preset`) or manual configure; manual configure failure refreshes properties and returns NO. Afterwards: audio active = device has audio or muxed media; for the non-fast source `set_async_unbuffered(!buffering)`.
- **S-052-R40 [MODEL]** Aspect-ratio text for a format list: non-positive width/height -> ""; `g = gcd(w,h)`; if `g <= 50`: width > height -> `"%.2f:1"` of w/h; else `"1:%.2f"` of h/w (square gives `1:1.00`); if `g > 50`: `x = w/g, y = h/g`, `8:5` rewritten to `16:10`, printed `"%d:%d"`.
- **S-052-R41 [MODEL]** Frame-rate range text: sort ranges by max descending then min descending, iterate in reverse (ascending); each value rounded to 2 decimals; equal min/max -> single value, else `min-max`; printed `%.0f` if both whole numbers else `%.2f`; at most 10 entries -> joined with `", "` plus `" FPS"`; more than 10 -> `"<lowest min>-<highest max> FPS (<n> values)"` (raw, `%.0f`); zero ranges -> nil.
- **S-052-R42 [MODEL]** Fallback frame rate for a format (when the configured one is unsupported), output fps `F` (0 if no video info): among ranges choose the one with the greatest `maxFrameRate <= F`; its rate is `timescale/value` of `minFrameDuration`. If none, the range with the smallest `minFrameRate > F`, using `maxFrameDuration` (so the slowest rate of the lowest range above F). If no range qualifies the result is `0/0`.
- **S-052-R43 [MODEL]** Pixel-format tables. Subtype -> OBS format: `422YpCbCr8`->UYVY, `422YpCbCr8_yuvs`->YUY2, `32BGRA`->BGRA, `420YpCbCr8BiPlanar{Video,Full}Range`->NV12, `420YpCbCr10BiPlanar{Video,Full}Range`->P010, anything else NONE. Full range test is true for exactly `420YpCbCr8PlanarFullRange`, `420YpCbCr8BiPlanarFullRange`, `420YpCbCr10BiPlanarFullRange`, `422YpCbCr8FullRange`. OBS format + range -> subtype: UYVY, YUY2, BGRA ignore range; NV12/P010 choose Full/Video range variant; other -> 0.
- **S-052-R44 [MODEL]** Colour space/range text: `Default`, `CS 601`, `CS 709`, `sRGB`, `CS 2100 (PQ)`, `CS 2100 (HLG)`, else `Unknown`; range `Default`/`Partial`/`Full`. Valid (selectable) colour spaces are only `DEFAULT`, `601`, `709`. FourCC string<->u32 is big-endian byte order of the 4 characters (`"2vuy"` = 0x32767579); the string builder reads 4 chars and NUL-terminates.
- **S-052-R45 [SRC]** The legacy `resolution` setting is a JSON object `{"width":N,"height":N}` (`legacyDimensionsFromSettings`); missing/zero width or height or empty/invalid JSON -> (0,0); values are clamped to `[0, UINT32_MAX]` before casting to int32.

## 9. `mac-videotoolbox`

- **S-052-R46 [SRC]** At module load the encoder list is fetched asynchronously (`VTCopyVideoEncoderList`); `is_apple_silicon` is compile-time on arm64, else from the Rosetta/emulation check. Post-load, per list entry by codec type: H.264 -> codec `h264`, h26x properties; HEVC (only `ENABLE_HEVC` builds) -> `hevc`; ProRes **422** -> codec `prores` with ProRes properties and also recorded in the hardware/software ProRes lists; other ProRes subtypes (4444XQ, 4444, 422 Proxy, LT, HQ) are only recorded in those lists (not registered); everything else skipped. Encoder id string and display name come from the list; hardware flag from the entry (absent = false). Caps: `DYN_BITRATE | MULTITRACK_DYN_BITRATE`. (`vt_register`.)
- **S-052-R47 [MODEL]** (`videotoolbox.script`) Encoder display name from the VT display name: `Apple H.264 (HW)`->`VTH264EncHW`, `(SW)`->`VTH264EncSW`, `Apple HEVC (HW)`->`VTHEVCEncHW`, `(AVE)`->`VTHEVCEncT2`, `(SW)`->`VTHEVCEncSW` (HEVC only with `ENABLE_HEVC`); display names starting `AppleProResHW` -> `VTProResEncHW`; starting `Apple ProRes` -> `VTProResEncSW`; otherwise the VT name verbatim. (Locale keys; compare is exact `strcmp` / prefix.)
- **S-052-R48 [MODEL]** Defaults (`get_defaults2`): `rate_control` ABR, **CBR** when macOS >= 13 and hardware encoder and Apple Silicon; `bitrate` 6000; `quality` 60; `limit_bitrate` false; `max_bitrate` 6000; `max_bitrate_window` 1.5; `keyint_sec` 2; `profile` `high` for H.264 else `main`; `codec_type` = ProRes 422; `bframes` true; `spatial_aq_mode` = AQ_AUTO. Enum `aq_mode`: INVALID=0, AUTO=1, DISABLED=2, ENABLED=3.
- **S-052-R49 [MODEL]** Property sets. H.26x: `rate_control` list (string) with `CBR` only if macOS >= 13 && hardware && Apple Silicon, always `ABR`, `CRF` only if hardware && Apple Silicon; `bitrate` int 50..10 000 000 step 50 (suffix " Kbps"); `quality` slider 0..100; `limit_bitrate` bool; `max_bitrate` int 50..10 000 000 step 50; `max_bitrate_window` float 0.10..10.0 (suffix " s"); `keyint_sec` int 0..20 (suffix " s"); `profile` list: H.264 `baseline`, `main`, `high`; HEVC `main`, `main10`, plus `main42210` (label "main 4:2:2 10") on macOS >= 12.3; `bframes` bool; `spatial_aq_mode` int list (Auto, Disabled, Enabled) only on macOS >= 15. ProRes: `codec_type` list of the ProRes subtypes present in the hardware or software list matching the encoder's hardware flag.
- **S-052-R50 [MODEL]** Visibility on `rate_control` / `limit_bitrate` change: `CBR`: can-limit false, has-bitrate true; `CRF`: can-limit true, has-bitrate false; `ABR`: true/true; any other string leaves both true. `limit_bitrate` visible iff can-limit; `max_bitrate` and `max_bitrate_window` iff can-limit && `limit_bitrate`; `bitrate` iff has-bitrate; `quality` iff not has-bitrate.
- **S-052-R51 [MODEL]** Settings read (`update_params`): `codec_type` set from the encoder codec (`h264` -> H.264, `hevc` -> HEVC, and written back into settings); else read from settings (ProRes). `keyint_sec`, `bitrate`, `max_bitrate` cast to u32; `quality = int / 100` as float32; `spatial_aq`: mode AUTO -> true iff `rate_control == "CRF"`; otherwise true iff mode == ENABLED (so INVALID/absent -> false). (`vt_params`.)
- **S-052-R52 [MODEL]** Pixel format (`set_video_format`) by OBS format and range: I420 -> `420YpCbCr8Planar` / `...PlanarFullRange`; NV12 -> `420YpCbCr8BiPlanarVideoRange` / `FullRange`; P010 only for HEVC -> `420YpCbCr10BiPlanar(Video|Full)Range`, H.264+P010 -> unsupported; P216 -> `422YpCbCr16BiPlanarVideoRange`, full range -> FullRangeUnsupported; P416 -> `444YpCbCr16BiPlanarVideoRange`, full -> FullRangeUnsupported; others -> ColorFormatUnsupported. Errors set the encoder last-error text (`ColorFormatUnsupported`, `FullRangeUnsupported`) and a warning log; `create` fails. [Q-4: `vt_update` ignores the return value of `update_params`, so a failing re-read leaves partially updated fields.]
- **S-052-R53 [MODEL]** Profile mapping (`vt_profile`): H.264: baseline/main/high -> `H264_{Baseline,Main,High}_AutoLevel`, anything else (including "") -> Main. HEVC: `main` -> Main, except for OBS format P010 -> Main10 with warning `Forcing main10 for P010`; `main10` -> Main10; `main42210` -> Main42210 (macOS >= 12.3 only, else falls to Main); anything else -> Main. Other codec types (ProRes) -> `H264_Baseline_AutoLevel` (unused; the profile property is only set for H.26x sessions).
- **S-052-R54 [MODEL]** Colour tagging (`vt_cs`): matrix: 601 -> ITU-R 601-4, 2100 PQ/HLG -> ITU-R 2020, else 709-2. Primaries: 601 -> SMPTE-C, 2100 -> 2020, else 709-2. Transfer: sRGB -> sRGB, PQ -> SMPTE ST 2084 PQ, HLG -> ITU-R 2100 HLG, else 709-2. HDR metadata (PQ and HLG only): `MasteringDisplayColorVolume` 24 bytes big-endian: primaries (13250,34500), (7500,3000), (34000,16000), white point (15635,16450) as u16 pairs, `max_luminance = peak * 10000` (u32 BE), `min_luminance = 0`; `ContentLightLevel` 4 bytes: peak twice (u16 BE). `peak` is the OBS nominal HDR peak (u16-truncated) for PQ and fixed **1000** for HLG. (`vt_hdr`.)
- **S-052-R55 [MODEL]** Rate control (`vt_rc`): `CBR`: on macOS >= 13 and Apple Silicon -> ConstantBitRate, no data-rate limits; on macOS >= 13 without Apple Silicon -> warn, AverageBitRate; below 13 -> warn, AverageBitRate (can-limit true for the fall-back cases). `ABR`: AverageBitRate, can-limit. `CRF`: Apple Silicon -> set `Quality` (float32 = quality%/100), no bitrate property; else warn and AverageBitRate; can-limit. Any other string (the comparison is case-sensitive, so `cbr` is invalid) -> error log `Selected rate control method is not supported: %s`, return `kVTParameterErr`. Bitrate properties are `bitrate_kbps * 1000` as int32. If `limit_bitrate` and can-limit: DataRateLimits = (`max_bitrate * 125 * window` bytes, `window` seconds); a `kVTPropertyNotSupportedErr` is downgraded to a warning and success.
- **S-052-D2 DEFECT (source-derived, verify on macOS)** In the DataRateLimits array the byte count is a `double` passed by address to `CFNumberCreate` with `kCFNumberIntType`, and `max_bitrate_window` (a `double`) with `kCFNumberFloatType`. On the little-endian hosts macOS runs on only the low 32 bits of each double are read, which for typical values are zero (vector shows `reference_int_slot=0 reference_float_slot=0`), so the limits are effectively 0/0. Required behaviour for the Rust port: implement the intended semantics (`int32 bytes`, `float32 seconds` computed from the settings); do not reproduce the broken values. Hardware encoders may ignore this property (the reference logs and continues on not-supported).
- **S-052-R56 [MODEL]** Session parameters for H.26x (`vt_params`): `MaxKeyFrameIntervalDuration = keyint` (failure only warns); `MaxKeyFrameInterval = keyint * (fps_num / fps_den)` using **integer** division of the frame rate (29.97 -> 29, so 58 for 2 s); `ExpectedFrameRate = fps_num / fps_den` (integer); `AllowFrameReordering = bframes`; `ProfileLevel` per R53; then rate control (R55); on macOS >= 15 `SpatialAdaptiveQPLevel = Default` if `spatial_aq` else `Disable` (failure warns); `RealTime = false` (failure warns); colour tags (R54); prepare. Any error from the first property set, rate control, colour or prepare aborts creation. ProRes sessions only get RealTime, colour and prepare.
- **S-052-R57 [MODEL]** `update` re-applies rate control to the live session **only if `bitrate` or `limit_bitrate` changed** (Q-5: changes of `rate_control`, `quality`, `max_bitrate`, `max_bitrate_window` alone are ignored until the encoder is recreated). Failure logs `Failed to set bitrate to session` and still returns true. (`vt_update`.)

## 10. `mac-virtualcam`

- **S-052-R58 [MODEL]** Message ids: `Connect = 1`, `Frame = 2`, `Stop = 3`; Mach service name `com.obsproject.obs-mac-virtualcam.server`. Everything else (frame payload layout, DAL plug-in, `sendPixelBuffer:timestamp:fpsNumerator:fpsDenominator:`) is [SRC] and not specified.

## 11. Reference quirks and defects

| id | where | summary | required behaviour |
|---|---|---|---|
| Q-1 | R10 | cursor toggle restarts the SCK stream | SHOULD match (or update live, no saved-data impact) |
| Q-2 | R10 | list-filter settings never restart | SHOULD match |
| Q-3 | R38 | "960x460" label for the 960x540 preset | MAY fix (display only) |
| Q-4 | R52 | `vt_update` ignores `update_params` failure | SHOULD report error |
| Q-5 | R57 | only bitrate/limit changes re-applied live | SHOULD match |
| Q-6 | R10 | `window` setting 0 never clears the stored window | SHOULD match |
| D-1 | R20 | >8 audio channels overflows plane array | reject buffer |
| D-2 | R55 | DataRateLimits uses wrong CFNumber types | implement intended values |
| D-3 | R12 | application type with unmatched bundle id builds an include list containing nil | treat as "no application": capture nothing, log a warning |

## 12. Implementation notes

- Keep the decision logic (R05, R08-R10, R13, R16, R18, R20, R26-R33, R37-R44, R47-R57) in a platform-independent crate and the framework calls behind traits; the vectors then run on any host.
- The SCK keep-vs-restart rule (R10) and the video-toolbox settings (R48-R57) are what saved scenes and users observe; everything in 4.5/4.6 and 9 that touches `SCStreamConfiguration` / `VTSession` property keys is [SRC] and must be validated on a Mac with each supported OS version (12.5, 13, 14.2, 15).
- Floating point in vectors is printed with `%.9g`; compare with an absolute tolerance of 1e-6.

## 13. Coverage table

| rules | script | class |
|---|---|---|
| R04 R05 R06 R07 | `sck_settings` | MODEL |
| R10 (+Q-1,2,6) | `sck_update` | MODEL |
| R13 R18 | `sck_config` | MODEL |
| R15 R16 | `sck_frames` | MODEL |
| R19 R20 D-1 | `sck_audio` | MODEL |
| R08 R09 | `sck_lists` | MODEL |
| R21-R25 | `legacy_window_display` | MODEL |
| R26-R35 | `coreaudio` | MODEL |
| R37-R44 | `avcapture` | MODEL |
| R46-R57 D-2 | `videotoolbox` | MODEL |
| R58 | `virtualcam` | MODEL |
| R01-R03 R11 R12 R14 R17 R36 R45 | none | SRC |

## 14. Open questions

1. Exact `CMTime` result of `CMTimeMultiplyByFloat64(CMTimeMake(den,num), 0.9)` (value/timescale rounding) is not derivable from the repository.
2. Foundation's case-insensitive sort for non-ASCII window/app names and nil handling in the window comparator.
3. Whether D-2 produces a no-op or a rejected property on shipping macOS versions (needs a Mac).
4. Numeric FourCC and flag constants should be checked against the SDK headers (the vectors use symbolic names).
5. `ENABLE_HEVC` is assumed defined for official macOS builds (HEVC rules R46/R47/R49/R53).
6. The Rust sufficiency check was **skipped** by instruction (cost control); nothing here has been implemented independently.

## 15. Round 8: file citations + capture internals, CoreAudio encoder, Syphon, DAL IPC (added)

### Reference files (all cited; §7–§10 already pin much of the behaviour below)

- `plugins/mac-capture/mac-audio.c`, `plugins/mac-capture/audio-device-enum.c`, `plugins/mac-capture/audio-device-enum.h`, `plugins/mac-capture/plugin-main.c`, `plugins/mac-capture/mac-sck-video-capture.m`, `plugins/mac-capture/mac-sck-audio-capture.m`, `plugins/mac-capture/mac-sck-common.h`, `plugins/mac-capture/mac-sck-common.m`, `plugins/mac-capture/mac-display-capture.m`, `plugins/mac-capture/mac-window-capture.m`, `plugins/mac-capture/window-utils.h`
- `plugins/mac-avcapture/legacy/av-capture.mm`, `plugins/mac-avcapture/legacy/left-right.hpp`, `plugins/mac-avcapture/legacy/scope-guard.hpp`, `plugins/mac-avcapture/plugin-main.m`, `plugins/mac-avcapture/plugin-main.h`, `plugins/mac-avcapture/plugin-properties.m`, `plugins/mac-avcapture/plugin-properties.h`, `plugins/mac-avcapture/OBSAVCapture.h`, `plugins/mac-avcapture/OBSAVCapturePresetInfo.m`
- `plugins/coreaudio-encoder/encoder.cpp`, `plugins/coreaudio-encoder/windows-imports.h`
- `plugins/mac-syphon/syphon.m`, `plugins/mac-syphon/SyphonOBSClient.m`, `plugins/mac-syphon/plugin-main.c`
- `plugins/mac-virtualcam/src/dal-plugin/OBSDALPlugIn.mm`, `plugins/mac-virtualcam/src/dal-plugin/OBSDALPlugInInterface.mm`, `plugins/mac-virtualcam/src/dal-plugin/OBSDALPlugInInterface.h`, `plugins/mac-virtualcam/src/dal-plugin/OBSDALPlugInMain.mm`, `plugins/mac-virtualcam/src/dal-plugin/OBSDALDevice.mm`, `plugins/mac-virtualcam/src/dal-plugin/OBSDALStream.mm`, `plugins/mac-virtualcam/src/dal-plugin/OBSDALMachClient.mm`, `plugins/mac-virtualcam/src/dal-plugin/OBSDALObjectStore.mm`, `plugins/mac-virtualcam/src/dal-plugin/CMSampleBufferUtils.mm`, `plugins/mac-virtualcam/src/obs-plugin/plugin-main.mm`, `plugins/mac-virtualcam/src/obs-plugin/OBSDALMachServer.mm`, `plugins/mac-virtualcam/src/camera-extension/OBSCameraProviderSource.swift`, `plugins/mac-virtualcam/src/camera-extension/OBSCameraDeviceSource.swift`, `plugins/mac-virtualcam/src/camera-extension/OBSCameraStreamSource.swift`, `plugins/mac-virtualcam/src/camera-extension/OBSCameraStreamSink.swift`

### 15.1 CoreAudio input path — remaining init rules (`mac-audio.c`, `audio-device-enum.c/.h`)

- **S-052-R59 [SRC]** §7 R26–R36 pin the device/format/reconnect/capture behaviour of `plugins/mac-capture/mac-audio.c`; the AudioUnit init path adds: unit created input-scope for the resolved device; the input stream description is read (`kAudioUnitProperty_StreamFormat`, input scope, bus 1), then the output-scope format is set to the input's sample rate and channel count; `sample_rate` = that rate; buffer size read via `kAudioDevicePropertyBufferFrameSize` (global scope).
- **S-052-R60 [SRC]** Capture entry point is the AudioUnit render callback (`input_callback(data, action_flags, timestamp, inputRange, outputBus, outputData)`): render the unit, then convert per R35 (planar buffers, host-time → ns).
- **S-052-R61 [SRC]** `audio-device-enum.c` / `audio-device-enum.h` implement the device enumeration backing R30 (names + uids, input vs output filtering); `mac-capture/plugin-main.c` registers the plugin module.

### 15.2 Legacy AVCapture path (`legacy/av-capture.mm` + properties)

- **S-052-R63 [SRC]** Device = `AVCaptureDevice deviceWithUniqueID:` from the `device` setting; `use_preset` true → session preset string applied, false → explicit `outputSettings` (format + fps). Cross-ref R39 (update decision) and R45 (legacy `resolution` JSON).
- **S-052-R64 [SRC]** Session graph: `AVCaptureSession` + device input + data output; data-output pixel format chosen from the R43 table; sample-buffer delegate pushes obs frames; session start/stop on create/update/destroy; device-interruption notifications stop and reconfigure.
- **S-052-R65 [SRC]** `plugin-properties.m` / `plugin-properties.h` / `OBSAVCapturePresetInfo.m` / `OBSAVCapture.h` are the properties model: device discovery session → combo; per-device format list with preset info (dimensions, frame-rate ranges and aspect-ratio text per R40/R41, fallback fps per R42); selections write `device` / `use_preset` / `preset` / `resolution`.
- **S-052-R66 [SRC]** `mac-avcapture/plugin-main.m` / `plugin-main.h` register the legacy source ids (S-052 §3) and wire the properties; `legacy/left-right.hpp` / `legacy/scope-guard.hpp` are RAII/left-right helper headers with no further behaviour.

### 15.3 CoreAudio encoder (`coreaudio-encoder/encoder.cpp`)

- **S-052-R67 [SRC]** Registered as an obs audio encoder. The format→string table (LinearPCM, AC3, 60958AC3, AppleIMA4, MPEG4AAC, MPEG4CELP/HVXC/TwinVQ, MACE3/6, ULaw, ALaw, QDesign/QDesign2, QUALCOMM, MPEGLayer1/2/3) is for logging/diagnostics — the accepted input set is resolved at runtime by CoreAudio support queries (Q-6).
- **S-052-R68 [SRC]** Pipeline: setup (input format + encoder settings → CoreAudio encoder), encode (input `AudioBufferList` → encoded packet queue → obs encoder frames), teardown; failures log and fail the encode call.
- **S-052-R69 [SRC]** `windows-imports.h` is the Win32 import shim letting the encoder compile on Windows (harness use); the encoder is macOS-only at runtime.

### 15.4 Syphon (`mac-syphon/syphon.m`)

- **S-052-R70 [SRC]** Each source instance owns a Syphon server announced to `SyphonServerDirectory` (description: UUID, version, app name, surfaces). `SyphonServerAnnounceNotification` updates the source (app name, active state) when a sending app's description appears/changes; the connection is matched on the server UUID.
- **S-052-R71 [SRC]** Frames arrive as `CVImageBuffer` → converted to an obs BGRA video frame at the buffer's size; server invalidation (sender quit) stops delivery.
- **S-052-R72 [SRC]** `SyphonOBSClient.m` is the client-side wrapper used by the source; `mac-syphon/plugin-main.c` registers the plugin.

### 15.5 SCK display/window capture + shared helpers (`mac-capture/*`)

- **S-052-R73 [SRC]** `mac-sck-common.h` / `mac-sck-common.m`: shared ScreenCaptureKit helpers (filter construction, display/window selection state) used by §4/§6.
- **S-052-R74 [SRC]** `mac-sck-video-capture.m`: the SCK video capture source implementing §4; `mac-sck-audio-capture.m`: the SCK audio source implementing §5.
- **S-052-R75 [SRC]** `mac-display-capture.m`: SCK display capture (monitor selection; cross-ref §4/§6); `mac-window-capture.m`: SCK window capture (window filter + selection).
- **S-052-R76 [SRC]** `window-utils.h`: window enumeration helpers (CGWindowList-based) used by the selection UI and filters.

### 15.6 VirtualCam DAL plug-in + Mach IPC (all `mac-virtualcam` files)

- **S-052-R77 [SRC]** Three components: camera extension (`OBSCameraProviderSource.swift`, `OBSCameraDeviceSource.swift`, `OBSCameraStreamSource.swift`, `OBSCameraStreamSink.swift`), DAL plug-in (`OBSDALPlugIn.mm`, `OBSDALPlugInInterface.mm/.h`, `OBSDALPlugInMain.mm`, `OBSDALDevice.mm`, `OBSDALStream.mm`, `OBSDALMachClient.mm`, `OBSDALObjectStore.mm`, `CMSampleBufferUtils.mm`), OBS side (`obs-plugin/plugin-main.mm`, `OBSDALMachServer.mm`).
- **S-052-R78 [SRC]** `OBSDALObjectStore.mm` maps device UUID → device/stream objects; `OBSDALDevice.mm` / `OBSDALStream.mm` implement the DAL stream, each stream owning a `CMIOStreamClock` (created on alloc, invalidated on dealloc); `CMSampleBufferUtils.mm` does the CMSampleBuffer/CVPixelBuffer → obs frame conversion.
- **S-052-R79 [SRC]** IPC contract: `OBSDALMachClient.mm` (plug-in) ↔ `OBSDALMachServer.mm` (OBS) over the Mach service pinned by R58 (ids 1/2/3, `com.obsproject.obs-mac-virtualcam.server`); the client connects at plug-in start (Connect carries the stream), OBS pushes frames/Stop back; server absence → the device reports unavailable (Q-7).
- **S-052-R80 [SRC]** `OBSCameraStreamSink.swift` pushes plug-in frames into the camera extension; `OBSCameraDeviceSource.swift` exposes device metadata/formats; `OBSCameraProviderSource.swift` implements the DAL provider entry; `OBSCameraStreamSource.swift` the stream source.
- **S-052-R81 [SRC]** `obs-plugin/plugin-main.mm` registers the virtualcam source (§3) and starts/stops the Mach server with the module.

### 15.7 Quirks (added)

- **Q-6** The exact set of input formats `coreaudio-encoder` accepts at runtime (CoreAudio support query result); the format table is diagnostic only.
- **Q-7** Exact byte layout of the DAL frame payload (pixel buffer transferred as a Mach right; layout not re-derived this round).
