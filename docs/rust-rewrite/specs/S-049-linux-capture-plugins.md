# S-049 Linux capture plugins (linux-v4l2, linux-pulseaudio, xshm part of linux-capture; ALSA, JACK, xcomposite catalogued)

| | |
|---|---|
| Status | Draft (Tier C behavioural catalog; Rust sufficiency check NOT performed, see 11) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1: `plugins/linux-v4l2/{v4l2-helpers,v4l2-controls,v4l2-input}.c`, `plugins/linux-pulseaudio/pulse-input.c`, `plugins/linux-capture/xshm-input.c`, `plugins/linux-alsa/alsa-input.c`, `plugins/linux-jack/{jack-input,jack-wrapper}.c`, `plugins/linux-capture/xcomposite-input.c` |
| Target crate | `crates/linux-capture-plugins` (modules `v4l2`, `pulse`, `xshm`, `alsa`, `jack`, `xcomposite`) |
| Depends on | S-014 (obs_data), S-015 (obs_properties), S-004 (signals/procs, xcomposite only) |
| Conformance | `test/conformance/scripts/linuxcapture/` (26 scripts), runner section "linuxcapture runner" in `test/conformance/README.md` |

## 1. Scope

Behaviour of the Linux device-capture source plugins: what each registers, which settings it reads and what defaults it
installs, which properties it builds (names, types, ranges, ordering, enable/visible state, list contents and the way
modified-callbacks cascade), how settings translate into device/server calls, the format/enum mappings, the geometry and
crop arithmetic, restart rules and failure paths.

Device and OS interaction is NOT byte-exact. It is specified as numbered rules and, where the logic is deterministic,
checked with vectors produced against **fake system calls**: a fake libv4l2 + kernel V4L2 model, a fake PulseAudio wrapper
and stream, a fake xcb/X11 helper layer, a fake OBS core (module text returns the key, one source object, recording
`output_audio`) and the real `obs-data.c` / `obs-properties.c`.

Covered by vectors: `linux-v4l2` (helpers, controls, input source minus the live capture loop), `linux-pulseaudio`
(`pulse-input.c`), `xshm-input.c`.
Catalogued by rules only (no vectors, reason in 10): `linux-alsa`, `linux-jack`, `xcomposite-input.c`.
Not covered at all: `v4l2-output.c` (virtual camera), `v4l2-udev.c` (hot-plug), `v4l2-decoder.c` (FFmpeg MJPEG/H.264
decode, faked), `pulse-wrapper.c` (PulseAudio mainloop/context, faked), `xhelpers.c`, `xcursor-xcb.c` (cursor image
fetch/draw; only its offset call is observable here), `pipewire`.

## 2. Terminology and conventions

MUST/SHOULD/MAY as RFC 2119. Settings keys are quoted. `obs_module_text(K)` returns K in the vectors, so descriptions and
list labels appear as translation keys (`"Device"`, `"LeaveUnchanged"`); a real build shows the translated string. Log
lines are `log LEVEL text`; the text after `, errno: ` is masked as `<E>` (platform strerror). Where the spec says "list item"
it means an `obs_property_list_*` entry (name, value, disabled flag). Tuples (resolution `W,H`, framerate `NUM,DEN`) are one
`int64` setting (see 4.1).

V4L2 constants used (Linux uAPI): `V4L2_CAP_VIDEO_CAPTURE=0x1`, `V4L2_CAP_DEVICE_CAPS=0x80000000`, `V4L2_IN_CAP_DV_TIMINGS=0x2`,
`V4L2_IN_CAP_STD=0x4`, `V4L2_FMT_FLAG_EMULATED=0x2`. `fourcc(a,b,c,d) = a | b<<8 | c<<16 | d<<24` (YUYV = `0x56595559`).

## 3. Interface

Registered sources (all `OBS_SOURCE_TYPE_INPUT`):

| id | output flags | name key | icon |
|---|---|---|---|
| `v4l2_input` | `ASYNC_VIDEO`, `DO_NOT_DUPLICATE` | `V4L2Input` | camera |
| `pulse_input_capture` | `AUDIO`, `DO_NOT_DUPLICATE` | `PulseInput` | audio input |
| `pulse_output_capture` | `AUDIO`, `DO_NOT_DUPLICATE`, `DO_NOT_SELF_MONITOR` | `PulseOutput` | audio output |
| `xshm_input` (v1) | `VIDEO`, `CUSTOM_DRAW`, `DO_NOT_DUPLICATE`, `SRGB`, `CAP_OBSOLETE` | `X11SharedMemoryDisplayInput` | desktop |
| `xshm_input_v2` | same minus `CAP_OBSOLETE` | same | desktop |
| `alsa_input_capture` | `AUDIO` | `AlsaInput` | audio input |
| `jack_output_capture` | `AUDIO` | `JACKInput` | audio output |
| `xcomposite_input` | `VIDEO`, `CUSTOM_DRAW`, `DO_NOT_DUPLICATE` | `XCompositeInput` (key to verify) | window |

Each source implements `get_name, create, update, destroy, get_defaults, get_properties`; xshm also `video_tick,
video_render, get_width, get_height`. The module loader registers `v4l2_input` always, the virtual camera output only if
the `v4l2loopback` kernel module is present.

## 4. Data formats

### 4.1 Packed tuples (v4l2)

`pack(a: i32, b: i32) = ((a as u64) << 32) | (b as u32 as u64)` stored as `i64`. (`a` is sign-extended to 64 bit before the
shift, so negative `a` gives a negative result.)
`unpack(p: i64) -> (a, b)`: if the upper 32 bits of `p` are all zero (the legacy 32-bit encoding) then
`a = (p >> 16) as i32, b = (p & 0xffff) as i32`; otherwise `a = (p >> 32) as i32, b = (p & 0xffffffff) as i32`.

Worked examples (vector `v4l2-01`): `pack(1920,1080) = 8246337209400`; `pack(1,30) = 4294967326`; `pack(-1,5) = -4294967291`;
`pack(1,-1) = 8589934591`; `unpack(83886800) = (1280,720)` (legacy); `unpack(30) = (0,30)`; `unpack(-1) = (-1,-1)`.

### 4.2 Hardcoded tables (v4l2)

Frame sizes, in this order, label `"WxH"`, value `pack(W,H)`: 160x120 320x240 480x320 640x480 800x600 1024x768 1280x960 1440x1050
1440x1080 1600x1200 640x360 960x540 1280x720 1600x900 1920x1080 1920x1200 2560x1440 3840x2160 2560x1080 3440x1440 5120x2160
432x520 480x320 480x530 486x440 576x310 576x520 576x570 720x576 1024x576 (480x320 appears twice; both are listed).
Frame rates, value `pack(1,D)`, label `"%.2f"` of `D/1`: D = 60 50 30 25 20 15 10 5 (labels `60.00` ... `5.00`).

### 4.3 Pixel format map (v4l2 fourcc -> OBS video format)

YVYU->YVYU, YUYV->YUY2, UYVY->UYVY, NV12->NV12, YU12 (YUV420)->I420, YV12 (YVU420)->I420, XR24 (XBGR32)->BGRX,
AR24 (ABGR32)->BGRA, BGR3->BGR3; everything else (including RGB3, MJPG, H264) -> NONE. MJPG and H264 are additionally
*accepted* (decoded), see S-049-R30.

### 4.4 PulseAudio maps

Sample format -> OBS: `u8->U8BIT, s16le->16BIT, s32le->32BIT, float32le->FLOAT`, all others UNKNOWN.
Channels -> layout: 1 MONO, 2 STEREO, 3 2.1, 4 4.0, 5 4.1, 6 5.1, 8 7.1, other UNKNOWN (so 7 and 0 are unsupported).
Layout -> `pa_channel_map` (first N entries, vectors `pulse-01`):

| layout | n | positions |
|---|---|---|
| MONO | 1 | mono |
| STEREO | 2 | FL FR |
| 2.1 | 3 | FL FR LFE |
| 4.0 | 4 | FL FR FC RC |
| 4.1 | 5 | FL FR FC LFE RC |
| 5.1 | 6 | FL FR FC LFE RL RR |
| 7.1 | 8 | FL FR FC LFE RL RR SL SR |
| UNKNOWN/other | 0 | none |

## 5. Behaviour

### 5.1 linux-v4l2 ioctl helpers (`v4l2-helpers.c`; vectors v4l2-02/03/04)

`dev` is a file descriptor; the value 0 is treated as invalid by helpers that check it.

- **S-049-R01** `set_input(dev,&input)`: `dev==0` or null pointer -> -1. `*input == -1`: `G_INPUT` into `*input`; else `S_INPUT`. Returns the ioctl result.
- **S-049-R02** `get_input_caps(dev,input,&caps)`: null -> -1. `input == -1` first does `G_INPUT` (failure -> -1). Then `ENUMINPUT{index=input}`; failure -> -1 and `*caps` untouched; success sets `*caps` = input capabilities.
- **S-049-R03** `set_format(dev,&res,&pix,&bpl)`: any null / `dev==0` -> -1. Always `G_FMT` (capture type) first (failure -> -1). If `res != -1` set width/height from `unpack(res)`; if `pix != -1` set pixelformat. `S_FMT` is issued only if something was set (failure -> -1). On success *all three outputs are rewritten* from the format struct as it is after the last ioctl: `res = pack(w,h)`, `pix = pixelformat`, `bpl = bytesperline`. On failure no output is modified. (So a driver that adjusts the format in `S_FMT` is reflected; the zero/unset case is a pure read-back.)
- **S-049-R04** `set_framerate(dev,&fr)`: null -> -1. `G_PARM`; if `*fr != -1` set `timeperframe = unpack(*fr)` (num,den) and `S_PARM`; on success `*fr = pack(num,den)` from the struct after the last ioctl. Failure of either ioctl -> -1, `*fr` unchanged.
- **S-049-R05** `set_standard(dev,&std)`: null -> -1. `*std == -1`: `G_STD` into it, else `S_STD`. See D-3 for the 32-bit width.
- **S-049-R06** `set_dv_timing(dev,&t)`: null -> -1; `*t == -1` -> 0 with no ioctl; else `ENUM_DV_TIMINGS{index=*t}` (failure -> -1) then `S_DV_TIMINGS` (failure -> -1).
- **S-049-R07** `create_mmap`: `REQBUFS{count=4, MMAP}` (failure -> -1, log error, state untouched); granted count < 2 -> -1 (log error), state untouched. Otherwise `count = granted` (may exceed 4), allocate `count` zeroed slots, then for index 0..count-1: `QUERYBUF`, record `length`, `mmap(length, RW, SHARED, offset=m.offset)`. A `QUERYBUF` or `mmap` failure returns -1 with `count` already set and earlier buffers still mapped (cleaned by `destroy_mmap`, which skips slots whose address is 0 or MAP_FAILED).
- **S-049-R08** `destroy_mmap`: munmap every mapped slot, free the slot array, `count = 0` (only if count != 0; calling twice is harmless). Returns 0.
- **S-049-R09** `start_capture`: `QBUF` for index 0..count-1 in order, then `STREAMON`; any failure -> -1 (error log, no rollback). `stop_capture`: `STREAMOFF`, failure -> -1. `reset_capture`: debug log `attempting to reset capture`, stop (failure -> -1 without starting), then start.

### 5.2 linux-v4l2 settings, defaults, registration (vector v4l2-05)

- **S-049-R10** Defaults (only where unset): `input=-1, pixelformat=-1, standard=-1, dv_timing=-1, resolution=-1, framerate=-1` (ints), `color_range=0` (`VIDEO_RANGE_DEFAULT`), `buffering=true`, `auto_reset=false`, `timeout_frames=5`. `device_id` has no default (reads as the empty string).
- **S-049-R11** `update` applies `buffering` as `async_unbuffered = !buffering` on every update (also when no restart happens). `create` sets the internal device handle to "closed" (-1) and calls `update`; without libudev support it logs info `Compiled without libudev, you can't reconnect devices`.

### 5.3 linux-v4l2 property construction (vectors v4l2-06..09, 15)

`get_properties` builds, in this order (all names are setting keys): `device_id` (list, string), `input` (list, int),
`pixelformat` (list, int), `standard` (list int, **invisible**), `dv_timing` (list int, **invisible**), `resolution` (list int),
`framerate` (list int), `color_range` (list int with the three items `ColorRange.Default=0`, `ColorRange.Partial=1`,
`ColorRange.Full=2`), `buffering` (bool), `auto_reset` (bool), `timeout_frames` (int 2..120 step 1, not a slider),
`controls` (group, normal, empty). Descriptions: `Device, Input, VideoFormat, VideoStandard, DVTiming, Resolution, FrameRate,
ColorRange, UseBuffering, AutoresetOnTimeout, FramesUntilTimeout, CameraCtrls`. Then the device list is filled (R12-R14) from the
source's current settings. Modified-callbacks: `device_id`->R15, `input`->R16, `pixelformat`->R17, `resolution`->R18.

- **S-049-R12** Device list: enumerate the entries of `/sys/class/video4linux` in directory order; skip directories; if the
  directory cannot be opened return immediately WITHOUT clearing the list. Otherwise clear the list first. For each entry open `/dev/<name>`
  (non-blocking read/write): open failure -> info log `Unable to open <path>`, skip; `QUERYCAP` failure -> info `Failed to query capabilities for <path>`, skip; effective caps = `device_caps` if the `DEVICE_CAPS` bit is set in `capabilities` else `capabilities`; no `VIDEO_CAPTURE` in them -> info `<path> seems to not support video capture`, skip; else add list item name `"<card> (<bus_info>)"` (formatted into a 68-byte buffer, truncated), value `<path>`; info `Found device '<card>' at <path>`. Each opened device is closed again.
- **S-049-R13** After the walk, if the current `device_id` setting is non-empty and none of the listed values equals it, append an item name=value=current string, **disabled**. (Empty current value: nothing appended.)
- **S-049-R14** `device_selected` (callback on `device_id`): open `device_id`. On failure: all properties except the device list itself are disabled and the callback returns **false**. On success: all properties except the device list are enabled, list `input` (R16a), rebuild the `controls` group (remove the old group, create a new one filled by R32, add it as last property), close, trigger `input` modified (cascade), return true.
- **S-049-R16** `input` list: `ENUMINPUT` from index 0 until failure; item name = input name, value = index; info log `Found input '<name>' (Index <i>)`. `input_selected` (callback on `input`): open device (failure -> return false, nothing changed), rebuild `pixelformat` list (R17a), close, trigger `pixelformat` modified, return true.
- **S-049-R17** `pixelformat` list: `ENUM_FMT` index 0.. until failure; name = description (+ `" (Emulated)"` if the EMULATED flag is set); the item is added (value = fourcc) iff the format maps to a non-NONE OBS format (4.3) or is MJPG or H264, with info log `Pixelformat: <name> (available)`; otherwise only info `Pixelformat: <name> (unavailable)`. `format_selected` (callback on `pixelformat`): open (failure -> false); `caps = get_input_caps(dev, input setting)` (failure -> **return false without closing the device**, defect D-4) masked with `STD|DV_TIMINGS`. Visibility: resolution and framerate visible iff `caps == 0`; standard visible iff STD; dv_timing visible iff DV_TIMINGS. If `caps == 0` rebuild `resolution` (R19) for the current `pixelformat` setting; if STD rebuild `standard` (R20); if DV rebuild `dv_timing` (R21); close; trigger `resolution` / `standard` / `dv_timing` modified for the rebuilt lists; return true. When both STD and DV bits are set both lists are rebuilt and shown, resolution/framerate hidden.
- **S-049-R18** `resolution_selected` (callback on `resolution`): open (failure -> false), `unpack(resolution setting)`, rebuild `framerate` (R22) for `(pixelformat, w, h)`, close, trigger `framerate` modified, return true.
- **S-049-R19** `resolution` list: clear; first item `LeaveUnchanged` = -1. Issue one `ENUM_FRAMESIZES` (index 0) *to learn the type*; if the type is DISCRETE: enumerate from index 0 (so index 0 is queried twice), item `"<w>x<h>"` = `pack(w,h)`; for any other type (stepwise, continuous, or the first call failed) log info `Stepwise and Continuous framesizes are currently hardcoded` and add the 4.2 table. (Defect D-2 when the first call fails.)
- **S-049-R20** `standard` list: clear; `LeaveUnchanged` = -1; then `ENUMSTD` index 0.. until failure, item name = standard name, value = standard id (as int).
- **S-049-R21** `dv_timing` list: clear; `LeaveUnchanged` = -1; then for index 0..: `ENUM_DV_TIMINGS` until failure; label `"<w>x<h><i|p> <rate>"` with `h' = height + vfrontporch + vsync + vbackporch + il_vfrontporch + il_vsync + il_vbackporch`, `w' = width + hfrontporch + hsync + hbackporch`, `k = interlaced ? 2 : 1`, `rate = pixelclock / (w' * (h' / k))` (double arithmetic), printed `%.2f`, suffix `i` if interlaced else `p`; value = index. E.g. 1920x1080 with 148.5 MHz, w'=2200 h'=1125 -> `1920x1080p 60.00`. The interlaced example in `v4l2-09` yields `1920x1080i 60.00`.
- **S-049-R22** `framerate` list: clear; `LeaveUnchanged` = -1; one `ENUM_FRAMEINTERVALS` (index 0) for the type; DISCRETE: enumerate from 0, label `"%.2f"` of `(float)den/num`, value = **`(int)pack(num,den)` (see D-1)**; otherwise info `Stepwise and Continuous framerates are currently hardcoded` and the 4.2 rate table (label `"%.2f"` of `(float)den/num`).
- **S-049-R23** Calling `obs_property_modified` on a property runs its callback; callbacks above call it on the next list, so modifying `device_id` ends by running the whole chain with the *current settings values* at each stage (vector `v4l2-07` demonstrates that `pixelformat`/`resolution` settings are read mid-cascade).

### 5.4 linux-v4l2 controls (`v4l2-controls.c`; vector v4l2-15)

- **S-049-R32** `update_controls(dev, props, settings)`: `dev==0` or null props -> -1. Iterate `QUERYCTRL` with `id = NEXT_CTRL` then `id |= NEXT_CTRL` of the last result until failure (next control with a strictly larger id). A control with any of `DISABLED(0x1) | READ_ONLY(0x4) | VOLATILE(0x80)` is skipped. By type: INTEGER -> int slider (min, max, step) named and described by the control's name, default of the setting = control default; BOOLEAN -> bool, default; MENU / INTEGER_MENU -> list (int), items from `QUERYMENU` for `index = min; index <= max; index += step` that succeed (name = menu name, value = index); if no item remains the property is removed again and nothing else happens (no default); otherwise default int set and info log `setting default for <name> to <default>`; every other type (button, int64, ctrl class, string, ...) is ignored. Returns 0. The property's modified-callback carries the control id as user data.
- **S-049-R33** Control modified-callback: open `device_id` (failure -> false). Value = bool setting for BOOL properties, int setting for INT and LIST properties (other types: error log `unknown property type for <name>`, return false). Issue `S_CTRL{id,value}`. **Returns true iff S_CTRL failed** (Q-2); returns false on success; returns false when the device cannot be opened.

### 5.5 linux-v4l2 source lifecycle (vectors v4l2-10/11/13/14)

- **S-049-R24** `settings_changed(data, settings)` (restart decision). If either `device_id` setting or the stored device id is absent -> restart. Otherwise OR of: device string differs; `input`, `pixelformat`, `standard`, `dv_timing`, `color_range` differ from stored; resolution rule; framerate rule. Resolution rule (framerate identical with its own flag): if setting is -1 and flag `resolution_unchanged` is false -> set the flag and restart (the *first* -1 forces one restart); if -1 and flag true -> no restart; otherwise clear the flag and restart iff `stored != setting && setting != -1`. `buffering`, `auto_reset`, `timeout_frames` never restart.
- **S-049-R25** `update`: `needs_restart = settings_changed`; if so `terminate`; replace stored device id by a copy of the setting; store `input, pixelformat, standard, dv_timing, resolution, framerate, color_range` (ints), `auto_reset`, `timeout_frames`; apply buffering (R11); if restarting, `init`.
- **S-049-R26** `init`, in order, each failure logging an error and going to the common failure path (R29): info `Start capture from <dev>`; open (`Unable to open device`); `set_input` (`Unable to set input <n>`); info `Input: <n>`; `get_input_caps(-1)` (`Unable to get input capabilities`); if caps has STD: `set_standard(&standard)` (`Unable to set video standard`) and **force `resolution = framerate = -1`**; if caps has DV_TIMINGS: `set_dv_timing(&dv_timing)` (`Unable to set dv timing`) and force `resolution = framerate = -1`; `set_format(&resolution,&pixelformat,&linesize)` (`Unable to set format`); if the resulting pixel format is not mappable and not MJPG/H264 -> error `Selected video format not supported`; `width,height = unpack(resolution)`; info `Resolution: WxH`, `Pixelformat: <4 chars>`, `Linesize: N Bytes`; `set_framerate(&framerate)` (`Unable to set framerate`); if the framerate value is 0 -> error `Framerate is not set, falling back to 30` and `framerate = pack(1,30)`; info `Framerate: %.2f fps` of `(float)den/num`; `create_mmap` (`Failed to map buffers`); for MJPG/H264 `init_decoder` (`Failed to initialize decoder`); create the stop event and start the capture thread.
- **S-049-R29** Failure path: error log `Initialization failed, errno: <strerror>` then `terminate`. `terminate`: if a thread exists signal its event, join it, destroy the event; for MJPG/H264 destroy the decoder; `destroy_mmap`; close the device and set it to closed. `destroy` = terminate + free the stored id.
- **S-049-R30** MJPG and H264 are accepted formats (listed in the format list and allowed after `set_format`) and decoded to OBS frames; their frames use format NONE during `prep_obs_frame`.
- **S-049-R27** `prep_obs_frame(data)`: clear the frame and the plane-offset array; `format = map(pixfmt)`; `flags = LINEAR_ALPHA`; width/height from data; color matrix and range min/max from the OBS helper for `(default colorspace, color_range setting, format)`. Plane layout (L = linesize, H = height, integer arithmetic): NV12: `linesize[0]=linesize[1]=L`, `offset[1]=L*H`. YV12: `linesize = {L, L/2, L/2}`, `offset[1] = L*H*5/4`, `offset[2] = L*H`. YU12: `linesize={L,L/2,L/2}`, `offset[1]=L*H`, `offset[2]=L*H*5/4`. All others: `linesize[0]=L`, offsets 0. (Vector `v4l2-13`, including odd sizes where `L*H*5/4` truncates.)
- **S-049-R28** Capture thread start: `(num,den) = unpack(framerate)`, `ffps = (float)den / num`; `timeout_usec = (1000000 * timeout_frames) / ffps` (float division, truncated to unsigned 64); logs debug `<dev>: framerate: %.2f fps` and info `<dev>: select timeout set to <usec> (<n>x frame periods)`; `start_capture` (failure -> go to exit); debug `new capture started`, build the frame, debug `obs frame prepared`; loop until the stop event is set (see 5.6); on exit info `<dev>: Stopped capture after <frames> frames` and always `stop_capture`. (`timeout = 5 frames at 30 fps -> 166666`; 2 frames at 60 -> 33333; 120 at 5 -> 24000000; see vector `v4l2-14`.)

### 5.6 linux-v4l2 live capture loop (informative, not vectored)

Per iteration: `select(fd)` with the timeout. Error: `EINTR` -> continue else error log, exit. Timeout (0): error log `select timed out`, `LOG_STATUS` ioctl (failure logged), if `auto_reset` then `reset_capture` (info/ error log), continue. Ready: `DQBUF` (`EAGAIN` -> continue; other failure -> error, exit). Buffers with the ERROR flag skip decoding but are requeued. Timestamp = buffer timestamp (sec,usec) in ns minus the first frame's timestamp (the first frame is 0). MJPG/H264: decode into the frame (failure -> error, exit); others: plane pointers = buffer start + offsets. `obs_source_output_video`, then `QBUF` (failure -> error, exit), `frames++`.

### 5.7 linux-pulseaudio (vectors pulse-01..06)

- **S-049-R40** Defaults: `device_id = "default"`. Both sources use the same `update/destroy/get_defaults`; input and output differ in `create` (flag `input`), properties, and flags (4.4/3).
- **S-049-R41** `get_properties(input)`: list `device_id` (string, description `Device`). `pulse_init`; input variant: for each PulseAudio *source* with `monitor_of_sink == INVALID` add item (description, name); output variant: for each *sink* with `monitor_source != INVALID` add item (description, **monitor_source_name**); `pulse_unref`. If at least one item was added insert `Default` = `"default"` at index 0 (none -> no Default entry). Return value of the listing calls is ignored.
- **S-049-R42** `create`: object with `input` flag; `pulse_init`; `update(settings)`. `destroy`: stop recording if a stream exists; for output sources call `capture_device_changed(NULL)`; `pulse_unref`; free. (`pulse_init` / `pulse_unref` are balanced per object.)
- **S-049-R43** `update`: read `device_id`. Restart iff the stored device is absent or differs by `strcmp`. On restart: for **output** sources first call `capture_device_changed(new_device)` (with the *requested* string, e.g. `"default"`); replace the device; `is_default = (device == "default")`; stop recording if a stream exists; start recording. No restart: nothing happens. (Q-3/Q-4.)
- **S-049-R44** `start_recording`: (1) get server info (failure -> error `Unable to get server info !`, -1). Server callback: info log `Server name: '<name> <version>'`; if `is_default` replace the device with the server's default source (input) or `<default sink>.monitor` (output), debug logs `Default input device: '<n>'` / `Default output device: '<n>'`. (2) get source info for the device (failure -> error `Unable to get source info !`). Source callback: error (`eol<0`) -> format = INVALID; `eol>0` ignored; else info `Audio format: <fmt>, <rate> Hz, <ch> channels`; if the format is not in the 4.4 map use `float32le` (info `Sample format <f> not supported by OBS,using float32le instead for recording`, sic: no space); if the channel count has no layout use 2 (info `<n> channels not supported by OBS, using 2 instead for recording`); store format, rate, channels. (3) format INVALID -> error `An error occurred while getting the source info!`. (4) invalid sample spec -> error `Sample spec is not valid`. (5) `speakers = layout(channels)`, `bytes_per_frame = frame size`, channel map from 4.4; create a stream named with the OBS source's name (failure -> error `Unable to create stream`); set the read callback; buffer attributes `fragsize = bytes for 25000 us`, `maxlength = minreq = prebuf = tlength = 0xffffffff`; flags `ADJUST_LATENCY (0x2000)`, plus `DONT_MOVE (0x200)` if the device is not the literal default; connect-record to the device (failure -> stop recording, error `Unable to connect to stream`, -1). Success: info `Started recording from '<device>'` plus ` (default)` if `is_default`. Fail paths keep the object alive with no stream. Before the first successful source-info the stored format is 0 = U8 (bzalloc), visible in failure states.
- **S-049-R45** `stop_recording`: if a stream exists disconnect and unref it. Always (even without a stream when called from update/destroy-with-stream only) info `Stopped recording from '<device>'` and `Got <packets> packets with <frames> frames`, then reset `first_ts, packets, frames` to 0. Note the device name logged is the *current* one (Q-3). `destroy` calls it only if a stream exists.
- **S-049-R46** Read callback (`stream_read`): if there is no stream -> nothing (only `pulse_signal`). Peek: 0 bytes -> nothing; data pointer null with bytes > 0 (a hole) -> error log `Got audio hole of <n> bytes`, drop, no counters; else build the audio packet: `speakers`, `samples_per_sec`, `format = map(pa format)`, `frames = bytes / bytes_per_frame` (integer division), `timestamp = now_ns - frames * 1e9 / rate` (128-bit-safe `mul_div`). If `first_ts == 0`: `first_ts = timestamp + 500 ms`. Deliver (`output_audio`) iff `timestamp > first_ts` (strict). Always `packets++`, `frames += frames` (also for dropped startup packets), then drop the chunk.

### 5.8 xshm (vectors xshm-01..05)

- **S-049-R50** Defaults: `screen = 0` for v1 and `-1` for v2; `show_cursor=true, advanced=false, cut_top=cut_left=cut_right=cut_bot=0`. `server` has no default.
- **S-049-R51** `update`: first stop the current capture (R54 teardown), then read `screen, show_cursor, advanced, server (copy), cut_top, cut_left, cut_right, cut_bot`, then start capture. `create` = zeroed object + `update`.
- **S-049-R52** Capture start: server name = `server` if `advanced` and non-empty, else the default display (null). `screen == -1`: destroy the texture if any, `adj_width = adj_height = 0`, return (no connection). Else connect (error or null -> error log `Unable to open X display !`); extension check: missing SHM -> error log `Missing SHM extension !` (fail); missing Xinerama / RandR -> info logs `Missing Xinerama extension !` / `Missing Randr extension !` (not failures; both logs happen in this order); `use_randr = randr active`, `use_xinerama = xinerama active`; `update_geometry` (< 0 -> error `failed to update geometry !`); attach shared memory with `adj_width x adj_height` (null -> error `failed to attach shm !`); create the cursor object and give it the offset `(adj_x_org, adj_y_org)`; create the texture `adj_width x adj_height` BGRA dynamic. Any failure runs the teardown below (so size and handles are reset, but `adj_*` keep their last values in the failed-attach case, see vector `xshm-04`).
- **S-049-R53** `update_geometry` (returns -1 failure, 0 size unchanged, 1 size changed): geometry source precedence RandR > Xinerama > plain X. RandR: `(x,y,w,h)` of monitor/CRTC `screen` and the root screen; Xinerama: screen info, X screen 0; plain: `x = y = 0`, `(w,h)` of X screen `screen`, X screen `screen`. Failure of the source -> -1. If `w == 0 || h == 0` -> error `Failed to get geometry`, -1. Then `adj = geometry`; **crop arithmetic**: `cut_top != 0`: `adj_y = (y > 0 ? y + cut_top : cut_top)`, `adj_h -= cut_top`; `cut_left != 0`: `adj_x = (x > 0 ? x + cut_left : cut_left)`, `adj_w -= cut_left`; `cut_right != 0`: `adj_w -= cut_right`; `cut_bot != 0`: `adj_h -= cut_bot`. No clamping (D-5). Info log `Geometry <w>x<h> @ <x>,<y>` (uncropped values). Return 0 if the new `adj_w/adj_h` equal the previous ones, else 1.
- **S-049-R54** Teardown (`capture_stop`): destroy texture, destroy cursor object, detach shared memory, disconnect, free server string; all pointers null. Safe on an idle object. `destroy` = teardown + free. `get_width/get_height` return `adj_width/adj_height` as `uint32` (negative values wrap, D-5).
- **S-049-R55** Properties: `screen` (list int, `Display`), `show_cursor` (bool `CaptureCursor`), `advanced` (bool `AdvancedSettings`), `cut_top` (int -4096..4096 step 1, suffix ` px`, `CropTop`), `cut_left` (-4096..4096, `CropLeft`), `cut_right` (0..4096, `CropRight`), `cut_bot` (0..4096, `CropBottom`), `server` (text, `XServer`). Callbacks: `advanced` -> toggle visibility of `server` to the setting value, then trigger `server` modified, return true; `server` -> R56. After building, `server` modified is triggered once with the source's current settings (so the list is filled at construction).
- **S-049-R56** Screen list (`server` callback): clear the list; if the stored `screen == -1` add disabled `SelectADisplay` = -1 first; connect (server name rule of R52); on error disable the whole list and return true. Else `count` = RandR monitor count, else Xinerama count, else number of X screens. For `i in 0..count`: name from RandR if available else decimal index; label `"<name> (<w>x<h> @ <x>,<y>)"`; add item (value `i`) only if `w > 0 && h > 0` (plain X reports origin `0,0`). If `old_screen + 1 > count` append disabled `"Display <old_screen> (not found)"` = old_screen. Disconnect; enable the list. Return true. Info logs for missing extensions are also emitted here.
- **S-049-R57** Per frame (informative): `video_tick` returns early when the texture is missing or the source is not showing; fetches the shared-memory image of `adj_*` rect at root, uploads with stride `adj_width * 4`, updates the cursor. `video_render` draws the opaque effect, then the cursor with the default effect when `show_cursor`.

### 5.9 linux-alsa (catalogued; no vectors)

- **S-049-R60** Defaults: `device_id="default"`, `custom_pcm="default"`, `rate=44100`. The effective PCM name is `device_id`, replaced by `custom_pcm` if `device_id == "__custom__"`.
- **S-049-R61** Properties: `device_id` (list string `Device`; first `Default`="default", then every ALSA PCM hint whose IOID is absent or `Input`, whose NAME contains `front:` and that has a DESC — label = DESC cut at the first newline, value = NAME; finally `Custom`="__custom__"), `custom_pcm` (text `PCM`, visible only when `device_id == "__custom__"`, toggled by the `device_id` callback), `rate` (list int `Rate`: `32000 Hz`, `44100 Hz`, `48000 Hz`). Hint enumeration failure returns the property set built so far (no `Custom` item).
- **S-049-R62** Open: PCM capture, non-blocking not requested, interleaved read/write access; first format that tests OK from the order `S16_LE, S32_LE, FLOAT_LE, U8` (none: error `device doesnt support any OBS formats`); `set_rate_near(rate)` (granted rate logged); channels from `get_channels` (error -> 2) then `set_channels_near`; period size from the hardware params; buffer of `period_size * sample_size` bytes. Channels -> layout 1..6,8 as in 4.4 (others unknown). Format -> OBS: U8->U8BIT, S16_LE->16BIT, S32_LE->32BIT, FLOAT_LE->FLOAT.
- **S-049-R63** Capture thread: `readi(period)`; timestamp = `now - frames * 1e9 / rate`; first packet sets `first_ts = ts + 500 ms`; packets with `ts <= first_ts` are dropped (same rule as R46).

### 5.10 linux-jack (catalogued; no vectors)

- **S-049-R70** Defaults `channels=2`, `startjack=false`. Properties: `channels` (int 1..8 step 1, `Channels`), `startjack` (bool `StartJACKServer`).
- **S-049-R71** The JACK client name is `"OBS Studio: <source name>"` (changes of the source name restart the client). `update` restarts (close client, reopen) iff `startjack` changed, `channels` differs from the stored value (initially -1, so the first update always opens), or the device name changed. Open: `jack_client_open(name, startjack ? NullOption : NoStartServer)`, register `channels` input ports named `in_1 ... in_N` (audio type), set the process callback, activate. `create` returns NULL (and destroys) if no client could be opened.
- **S-049-R72** Process callback: speaker layout from channels (1..6,8 as 4.4), `samples_per_sec` = JACK sample rate, `frames = nframes`, planar float buffers = port buffers; timestamp = `now - period_usecs*1000` from the cycle times, or, if unavailable, `now - nframes*1e9/samples_per_sec` with a warning log (D-6).

### 5.11 xcomposite (catalogued; no vectors)

- **S-049-R80** Defaults: `capture_window=""`, `cut_top/left/right/bot = 0`, `show_cursor=true`, `include_border=false`, `exclude_alpha=false`. Crop ints are 0..4096. Properties list: `capture_window` (list), `cut_*` (int, px), `show_cursor`, `include_border`, `exclude_alpha`.
- **S-049-R81** Window identity string: `"<id>\r\n<name>\r\n<class>"` (decimal id). `convert(str)`: no `\r\n` -> id = `atol(str)`, name/class NULL; else id = first field, name = text between the marks, class = rest. Lookup: first by id among top-level windows; else by exact name AND class match; else keeps the decoded id (debug log). A string with exactly one `\r\n` makes the reference compute a pointer from NULL (D-7).
- **S-049-R82** Visible size: `width = max(0, win_width - (crop_left + crop_right + 2*border))`, `height` likewise with top/bottom, where `border = include_border ? border_width : 0`; if borders+crops exceed the window in either direction the pixmap is not created. The cursor offset is `(cursor.x_org + crop_left, cursor.y_org + crop_top)`; the cursor counts as outside when `x < 0 || y < 0 || x > width || y > height`. The texture draw uses the sub-region `(crop_left, crop_top, width, height)`. `exclude_alpha` picks the opaque effect.
- **S-049-R83** Signals `hooked(ptr source, string name, string class)` and `unhooked`; proc `get_hooked(out bool hooked, out string name, out string class)`.

## 6. Algorithms

Crop and geometry (R53), restart decision (R24) and the cascade (R23) are fully described by the rules; the vectors
(`xshm-02`, `xshm-03`, `v4l2-11`, `v4l2-07`) fix the numbers. Pseudocode for the dv-timing label (R21):

```
htotal = width + hfrontporch + hsync + hbackporch
vtotal = height + vfrontporch + vsync + vbackporch + il_vfrontporch + il_vsync + il_vbackporch
rate   = pixelclock / (htotal * (vtotal / (interlaced ? 2 : 1)))      # doubles
label  = "{width}x{height}{interlaced ? 'i' : 'p'} {rate:.2}"
```

## 7. Concurrency

v4l2: one capture thread per source, owned by the source; stopped through the stop event and joined in `terminate`
(`update` with restart and `destroy` are the only callers). The thread reads `data` fields set before it started; it
touches `buffers`, `dev`, `framerate`, `timeout_frames`, `auto_reset`. PulseAudio: all stream calls are done under the shared
mainloop lock (`pulse_lock/unlock`), the read callback runs on the mainloop thread and may be called after disconnect (hence
the stream check in R46). ALSA: a worker thread with an atomic shutdown flag. JACK: process callback on JACK's thread; client
open/close serialised by a mutex. xshm: `video_tick`/`render` on the graphics thread, property callbacks on the UI thread; the
object has no lock. Not covered by vectors (fakes run single threaded; the v4l2 thread is replaced by an on-demand call of its body, see the README section).

## 8. Reference defects and quirks

- **D-1 (v4l2 discrete frame rate list)** `v4l2_framerate_list` stores `int pack = v4l2_pack_tuple(num,den)`, narrowing the 64-bit value to 32 bits, so the list value is just `den` (e.g. 30 for 1/30). Selecting it stores 30, which the legacy decode turns into `(0,30)` -> `S_PARM 0/30`, "Framerate: inf fps" and a zero select timeout. Hardcoded (stepwise) lists are unaffected. Required behaviour: **store the full `pack(num,den)` value** (4294967326 for 1/30) in the list; accept legacy 32-bit values on read as R-4.1 says. The vector `v4l2-12` records the reference output (value 30, `S_PARM 0/30`); a conforming implementation will differ there by design; an implementation that deliberately deviates MUST exclude `v4l2-12` (and the discrete-framerate lines of `v4l2-07`/`v4l2-08`) from its comparison and state so.
- **D-2 (v4l2 uninitialised enum type)** `v4l2_resolution_list` / `v4l2_framerate_list` read `frmsize.type` / `frmival.type` after a first ioctl whose failure is ignored; on failure the struct is uninitialised stack. Required: a failing first enumeration behaves as "not discrete" -> hardcoded table (the fake zeroes the type, vector `v4l2-08`, second half).
- **D-3 (v4l2 standard width)** `set_standard` takes an `int*` but `G_STD`/`S_STD` use 64-bit `v4l2_std_id`: a real kernel writes 8 bytes into a 4-byte `int` (stack/field overflow into `dv_timing`; ASan flags it with a faithful fake) and reads garbage upper bits on `S_STD`. Required: use a 64-bit id. The fake and vectors model only the low 32 bits.
- **D-4 (v4l2 leak)** `format_selected` returns false on `get_input_caps` failure without closing the device (fd leak). Required: close it. Vector `v4l2-09` last case (the fake close trace shows no `close` line).
- **D-5 (xshm no clamping)** crop values larger than the screen give zero or negative `adj_*`; the shared-memory attach and texture receive those values and `get_width/height` return a wrapped `uint32` (`4294967216` for `-80`). Required: treat `adj_w <= 0 || adj_h <= 0` as a failed geometry (SHOULD) - the vectors record the reference behaviour; MUST match for 0 < size.
- **D-6 (jack)** `data->samples_per_sec` is never assigned (only read in the cycle-times fallback), so the fallback timestamp divides by zero. Required: use the JACK sample rate. Unverified at run time (not linked).
- **D-7 (xcomposite)** window string with exactly one `\r\n` makes `secondMark` NULL and `secondMark - secondStr` undefined. Required: treat as "id only" and ignore the rest. Unverified at run time.
- **D-8 (randr monitors reply)** `randr_screen_geo` returns 0 (success) with unset outputs when the monitors reply is NULL. Required: return -1 with zeroed outputs. Not testable with the fake (the fake layer replaces `xhelpers.c`).
- **Q-1 (v4l2 tuple collision)** `pack(0,b)` is the legacy encoding and unpacks via `>>16`: `pack(0,70000)` unpacks as `(1,4464)`. Vector `v4l2-01`.
- **Q-2 (v4l2 control callback)** returns true exactly when `S_CTRL` failed (R33). Vector `v4l2-15`. SHOULD match (the UI only uses the result to decide whether to refresh the property view).
- **Q-3 (pulse stop log)** after a device change the "Stopped recording from" line names the NEW device because the device string is replaced before stopping. Vector `pulse-05`.
- **Q-4 (pulse default restart)** `device_id == "default"` is resolved in place (the stored device becomes the real source name), so every later `update` with `"default"` compares unequal and restarts the stream. Vector `pulse-05`. SHOULD match; an implementation that fixes it MUST still restart when the server default changed.
- **Q-5 (xshm origin)** with a screen origin `<= 0` and a non-zero `cut_top`/`cut_left` the origin is ignored (`adj_y = cut_top`). SHOULD match. Vector `xshm-03`.
- **Q-6 (v4l2 first -1)** the first "-1" resolution/framerate update forces a restart (R24). Vector `v4l2-11`.

## 9. Implementation notes

- Keep the platform layer behind traits so the vectors' fakes can be reproduced: `V4l2Dev` (open/ioctl subset/mmap), `PulseApi` (server info, source/sink lists, stream), `XApi` (connect, extensions, screen geometry, shm, cursor), plus `ObsHost` (settings, properties, logging).
- The cascade (R23) depends on `obs_property_modified` invoking the callback synchronously and re-entrantly with the live settings object.
- Do not log with `errno` text in tests; the vectors mask it.
- Treat the vectors as the authority for log text only where this spec quotes it; other lines are informative.

## 10. What could not be linked or tested

- No real device, PulseAudio, JACK, ALSA, or X server was used; all system interaction is a fake whose behaviour was written from the kernel/PulseAudio/xcb documentation, not from captured traces. A real kernel may differ (e.g. `S_FMT` adjusting sizes; the fake leaves them as set; `ENUM_*` after a failed first call; `QUERYCTRL` ordering).
- The v4l2 capture-thread loop (select/DQBUF/QBUF, timestamps, MJPEG decode) is not executed; only its prologue (timeout computation, start/stop capture) is run through an on-demand call. The hot-plug (udev) code path is compiled out in the vectors (`HAVE_UDEV` undefined: the info line `Compiled without libudev` appears).
- `pulse-wrapper.c`, `xhelpers.c`, `xcursor-xcb.c`, `v4l2-decoder.c`, `v4l2-udev.c`, `v4l2-output.c` are replaced by fakes or excluded.
- `linux-alsa`, `linux-jack`, `xcomposite-input.c` are not linked (they need ALSA, JACK, GL/X11 composite); their rules in 5.9-5.11 come from reading the source and are unvectored. xcomposite's pure helpers (`convert_encoded_window_id`) are static and tied to X11 headers; they could be vectored later by `#include`-ing the file with a fake X layer.
- The video render path of xshm (effects, sRGB) and the PulseAudio mainloop threading are not specified beyond 5.8/7.

## 11. Rust sufficiency check

Not performed (cost control, as instructed). The statement "an implementer can pass the vectors from this spec alone" is therefore **unverified**; expect spec bugs in the log wording of 5.5 and in the property-ordering details of 5.3.

## 12. Coverage

| Rules | Scripts |
|---|---|
| R01 R02 (and 4.1-4.3 tables) | v4l2-01 |
| R01-R06 | v4l2-02, v4l2-03 |
| R07-R09 | v4l2-04 |
| R10 R11 | v4l2-05, v4l2-10 (defaults applied) |
| R12 R13 R14(part) | v4l2-06 |
| R14-R23 | v4l2-07, v4l2-08, v4l2-09 |
| R32 R33 | v4l2-15 |
| R24 R25 | v4l2-11 |
| R26 R29 R30 | v4l2-10 |
| R27 | v4l2-13 |
| R28 | v4l2-14 |
| D-1 | v4l2-12 |
| R40 R41 | pulse-02 |
| R44 (success, fallbacks) | pulse-03 |
| R44 (failures) R42 | pulse-04 |
| R43 R45 Q-3 Q-4 | pulse-05 |
| R46 | pulse-06 |
| 4.4 maps | pulse-01 |
| R50 | xshm-01 |
| R51 R52 R54 | xshm-04 |
| R53 R55(part) | xshm-02, xshm-03 |
| R55 R56 | xshm-05 |
| R57, 5.6, R60-R83 | not vectored (informative) |

## 13. Open questions

- Is the double `ENUM_FRAMESIZES` index-0 query (R19) required, or may a rewrite query once? Observable only through a call trace.
- For D-1, should the legacy 32-bit decode accept `30` as `1/30` (heuristic for old saved settings)? The reference cannot tell the two apart (Q-1).
- JACK/ALSA/xcomposite vectors: worth adding behind fakes in a later pass?
