# S-047 obs-ffmpeg media source and encoder wrappers (ffsources)

| | |
|---|---|
| Status | Draft (Rust sufficiency check NOT performed, see section 11) |
| Spec version | initial |
| Reference | C implementation, libobs 33.0.1 (plugin obs-ffmpeg: media source, audio encoder wrappers, video encoder wrapper, format helpers) |
| Target crate | `crates/obs-ffmpeg-sources` (plugin-side; talks to libobs, libavcodec, media-playback through traits) |
| Depends on | S-014 (obs_data), S-015 (obs_properties), S-004 (proc handler), S-034 (hotkeys), S-036 (media-playback, the player this source drives), S-025 (encoder option parsers and the create-time codec configuration of the ffmpeg audio/video encoders: **not repeated here**), S-040 (obs_source host contract) |
| Conformance | `test/conformance/scripts/ffsources/` (20 scripts), vectors in `test/conformance/vectors/ffsources/` |

## 1. Scope

In scope (all in plugin obs-ffmpeg):

- **Media source** `ffmpeg_source`: settings, defaults, properties and their visibility rule, the update/restart
  decision logic, how the source configures and drives the media player, play/pause/stop/restart/seek,
  hotkeys, procs, the network reconnect state machine, missing-files reporting.
- **Audio encoder wrappers** (`ffmpeg_aac`, `ffmpeg_opus`, `ffmpeg_pcm_s16le`, `ffmpeg_pcm_s24le`,
  `ffmpeg_pcm_f32le`, `ffmpeg_alac`, `ffmpeg_flac`): registration tables, buffer sizing, the encode step,
  timestamps, and the info callbacks.
- **Video encoder wrapper library** (`ffmpeg_video_encoder_*`) used by other encoders: init, init_codec,
  frame copy, encode step, first-packet hook, timeout, free.
- **Format helpers** (`obs-ffmpeg-formats.h`): sample-format mapping, timestamp rescale.

Out of scope (covered elsewhere, do not duplicate): the option-string parsers and the create-time codec
configuration (`bitrate`, channel layouts, sample format negotiation, sample-rate choice, `open` failure text)
of these encoders and `ffmpeg_video_encoder_update` (S-025 section 9, rules R60-R64); decoding/playback itself
(S-036); the libobs source machinery (S-040). The compile-time macro `LIBAVCODEC_VERSION_CHECK` in
`obs-ffmpeg-compat.h` has no observable behaviour and needs no Rust equivalent.

## 2. Terminology and conventions

RFC 2119 words. "Host" is libobs; "player" is the media-playback library (S-036); "codec" is libavcodec.
`T(key)` is the localized string of `key` (the conformance fake returns `key` itself, except `Encoder.Error` =
`Failed to open %1: %2` and `Encoder.Timeout` = `Encoder %1 is taking too long to encode (timeout: %2 seconds)`).
`bool(k)`, `int(k)`, `str(k)` read settings with the S-014 getters (missing key and no default: false / 0 / `""`;
`str` never returns NULL). Log text is informative; **log level** is normative.
Numeric enum values used by vectors: media state NONE 0, PLAYING 1, OPENING 2, BUFFERING 3, PAUSED 4,
STOPPED 5, ENDED 6, ERROR 7; video range DEFAULT 0, PARTIAL 1, FULL 2; encoder type AUDIO 0, VIDEO 1;
speaker layout UNKNOWN 0, MONO 1, STEREO 2, 2.1 3, 4.0 4, 4.1 5, 5.1 6, 7.1 8; OBS audio format UNKNOWN 0, U8BIT 1, 16BIT 2,
32BIT 3, FLOAT 4, U8BIT_PLANAR 5, 16BIT_PLANAR 6, 32BIT_PLANAR 7, FLOAT_PLANAR 8.

## 3. Interface

### 3.1 Registration records (MUST be exported exactly)

Source `ffmpeg_source`: `id = "ffmpeg_source"`, type INPUT, `output_flags = ASYNC_VIDEO | AUDIO | DO_NOT_DUPLICATE |
CONTROLLABLE_MEDIA` (0x2087), icon MEDIA, display name `T("FFmpegSource")`; callbacks: create, destroy,
get_defaults, get_properties, activate, deactivate, video_tick, missing_files, update, media_play_pause,
media_restart, media_stop, media_get_duration, media_get_time, media_set_time, media_get_state. (S-047-R01)

| kind | id | codec | name key | priming | audio_info | extra_data / frame_size |
|---|---|---|---|---|---|---|
| aac | `ffmpeg_aac` | `aac` | `FFmpegAAC` | yes | plain | yes |
| opus | `ffmpeg_opus` | `opus` | `FFmpegOpus` | yes | plain | yes |
| pcm | `ffmpeg_pcm_s16le` | `pcm_s16le` | `FFmpegPCM16Bit` | no | plain | yes |
| pcm24 | `ffmpeg_pcm_s24le` | `pcm_s24le` | `FFmpegPCM24Bit` | no | plain | yes |
| pcm32 | `ffmpeg_pcm_f32le` | `pcm_f32le` | `FFmpegPCM32BitFloat` | no | **float** (allow_clipping) | yes |
| alac | `ffmpeg_alac` | `alac` | `FFmpegALAC` | no | plain | yes |
| flac | `ffmpeg_flac` | `flac` | `FFmpegFLAC` | no | plain | yes |

All seven: type AUDIO, shared create/destroy/encode/frame-size/defaults/properties/extra-data functions. (S-047-R30)

### 3.2 Media-player interface the source needs (S-036)

`create(info) -> handle|NULL`, `destroy`, `play(looping, reconnecting)`, `play_pause(pause)`, `stop`,
`set_looping`, `set_is_linear_alpha`, `preload_frame`, `get_current_time`, `seek(ms)`, `get_frames`,
`get_duration` (microseconds), `has_video`. The player's getters tolerate a NULL handle (return 0 / no-op); the
source relies on this (`src-07`, `src-08`). `info` carries opaque, five callbacks (video, video-preload,
video-seek, audio, stop) and: `path, format, ffmpeg_options, buffering (bytes), speed, force_range,
is_linear_alpha, hardware_decoding, is_local_file, reconnecting, request_preload, full_decode`.

### 3.3 Host interface the source needs

`source_active`, `source_showing`, `output_video(frame|NULL)`, `preload_video`, `show_preloaded_video`,
`set_video_frame`, `output_audio`, `media_started`, `media_ended`, `media_restart`, `media_play_pause(pause)`,
`media_stop` (the last three ask the host, which calls the source's callbacks back), `get_settings`,
`update(settings)`, hotkey register/unregister, proc-handler add, missing-files create/add, `file_exists`.

## 4. Data formats

### 4.1 Source settings keys

| key | type | default (get_defaults) | used |
|---|---|---|---|
| `is_local_file` | bool | true | mode |
| `local_file` | string | none | path when local |
| `input`, `input_format` | string | none | URL / demuxer name when network |
| `looping` | bool | false | local only |
| `clear_on_media_end` | bool | true | |
| `restart_on_activate` | bool | true | |
| `linear_alpha` | bool | false | |
| `reconnect_delay_sec` | int | 10 | network |
| `buffering_mb` | int | 2 | |
| `speed_percent` | int | 100 | |
| `log_changes` | bool | true | |
| `hw_decode`, `close_when_inactive`, `seekable`, `full_decode`, `is_stinger`, `is_track_matte` | bool | none (false) | `full_decode`, `is_stinger`, `is_track_matte` have no property |
| `color_range` | int | none (0) | passed to the player unchanged |
| `ffmpeg_options` | string | none | |

(S-047-R02) get_defaults sets exactly the nine keys with a default above and no others.

### 4.2 Properties (S-047-R03)

Flags = DEFER_UPDATE (1). In this order:

| name | type | description key | extra |
|---|---|---|---|
| `is_local_file` | bool | `LocalFile` | modified callback (R04) |
| `local_file` | path (file) | `LocalFile` | filter and default_path below |
| `looping` | bool | `Looping` | |
| `restart_on_activate` | bool | `RestartWhenActivated` | |
| `buffering_mb` | int slider 0..16 step 1 | `BufferingMB` | suffix `" MB"` |
| `input` | text (default type) | `Input` | |
| `input_format` | text | `InputFormat` | |
| `reconnect_delay_sec` | int slider 1..60 step 1 | `ReconnectDelayTime` | suffix `" S"` |
| `hw_decode` | bool | `HardwareDecode` | |
| `clear_on_media_end` | bool | `ClearOnMediaEnd` | |
| `close_when_inactive` | bool | `CloseFileWhenInactive` | long description `CloseFileWhenInactive.ToolTip` |
| `speed_percent` | int slider 1..200 step 1 | `SpeedPercentage` | suffix `"%"` |
| `color_range` | list, combo type LIST, format INT | `ColorRange` | items `ColorRange.Auto`=0, `ColorRange.Partial`=1, `ColorRange.Full`=2 |
| `linear_alpha` | bool | `LinearAlpha` | |
| `seekable` | bool | `Seekable` | |
| `ffmpeg_options` | text | `FFmpegOpts` | long description `FFmpegOpts.ToolTip.Source` |

`local_file` filter string (one string, items separated by `;;`):
`T("MediaFileFilter.AllMediaFiles") + " (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.mp3 *.ogg *.aac *.wav *.gif *.webm);;" +
T("MediaFileFilter.VideoFiles") + " (*.mp4 *.m4v *.ts *.mov *.mxf *.flv *.mkv *.avi *.gif *.webm);;" +
T("MediaFileFilter.AudioFiles") + " (*.mp3 *.aac *.ogg *.wav);;" + T("MediaFileFilter.AllFiles") + " (*.*)"`.

## 5. Behaviour (numbered rules)

### 5.1 Media source: registration, settings UI

- **S-047-R01** Registration record as section 3.1.
- **S-047-R02** Defaults as 4.1. **R03** Property list as 4.2; all properties are initially visible and enabled
  (the visibility rule is NOT applied by get_properties; the front-end triggers the modified callback).
- **S-047-R04** Modified callback of `is_local_file` (returns true): with `e = bool(is_local_file)` of the settings
  passed: visible := e for `local_file`, `looping`, `speed_percent`; visible := !e for `input`, `input_format`,
  `buffering_mb`, `seekable`, `reconnect_delay_sec`. All other properties are untouched.
- **S-047-R05** `local_file` default_path: when the source exists and its current `input` is non-empty: copy it,
  replace every `\` with `/`, cut after the last `/` (inclusive; no `/`: empty string). Otherwise (no source
  or empty input) the default_path is NULL (absent). With an input that has no slash the result is the empty
  string, not NULL (`src-01`).

### 5.2 Media source: lifecycle

- **S-047-R06** create(settings, source): initial state all zero (state NONE, `reconnect_delay_sec` 0).
  1. Create the stop event (manual-reset); failure: log ERROR, release, return failure.
  2. Create the reconnect mutex; failure: destroy the event, log ERROR, return failure. Neither hotkey nor proc is registered on failure.
  3. Register hotkey `MediaSource.Restart` ("RestartMedia"); register the pair `MediaSource.Play` ("Play") /
     `MediaSource.Pause` ("Pause"); register hotkey `MediaSource.Stop` ("Stop"). In this order.
  4. Add procs `void restart()`, `void preload_first_frame()`, `void get_duration(out int duration)`,
     `void get_nb_frames(out int num_frames)`, in this order.
  5. Run update(settings) (R07..R12); return the source.
- **S-047-R26** destroy: stop the reconnect thread (R19); unregister **only** the restart hotkey (the pair and the stop
  hotkey are released by the host with the source); destroy the media object if any; release mutex, event and strings.

### 5.3 Media source: update

- **S-047-R07** Reading settings (`active := source_active`):

```
is_local := bool(is_local_file); is_stinger := bool(is_stinger); is_matte := bool(is_track_matte)
restart  := (is_local != s.is_local) OR (is_stinger != s.is_stinger)            // s.* are the OLD values
if is_local:  input := str(local_file); format := NULL; looping := bool(looping)
              if s.input != NULL AND NOT restart: restart := restart OR (s.input != input)        // byte compare
else:         restart := true; input := str(input); format := str(input_format)
              s.reconnect_delay_sec := int(reconnect_delay_sec); if it is 0: := 10 ; looping := false
stop_reconnect_thread()                                                            // R19, uses the OLD s.is_local
hw := bool(hw_decode); range := int(color_range) (stored as the enum without validation)
speed := int(speed_percent) as 32-bit; if speed < 1 or speed > 200: speed := 100                      // R09
opts := str(ffmpeg_options)
```
  Note: for a local file `reconnect_delay_sec` is left unchanged (0 after create; `src-03` shows 10, 25, 10 for a stream).
- **S-047-R08** Restart decision, continued: `restart := true` if any of `s.is_hw != hw`, `s.range != range`,
  `s.speed != speed`, or (`s.ffmpeg_options != NULL` and differs from `opts`). If `active AND s.state == ENDED AND looping
  AND NOT s.is_looping` then `restart := true` (a user-stopped source, state STOPPED, is not restarted by enabling looping,
  `src-10`). Changes of `buffering_mb`, `full_decode`, `clear_on_media_end`, `log_changes` and `is_track_matte` alone do NOT
  restart (Q-1). First update after create always restarts (old `speed` is 0 and old `is_local` false).
- **S-047-R09** `speed` outside 1..200 becomes 100 (before comparison, so 250 -> 100 compares equal to 100).
- **S-047-R10** Commit new values to the object: `is_looping, close_when_inactive := bool(close_when_inactive), input, input_format,
  hw, full_decode := bool(full_decode), clear_on_end := bool(clear_on_media_end), range, linear_alpha := bool(linear_alpha),
  buffering_mb := int(buffering_mb) (32-bit), speed, is_local, seekable := bool(seekable), ffmpeg_options, is_stinger,
  is_track_matte, log_changes := bool(log_changes)`.
  `restart_on_activate := false` if `input` starts with `rist` (ASCII case-insensitive, 4-byte prefix: `RIST://`, `ristretto://x` and `rist`
  all match, `ris` does not), else `bool(restart_on_activate)` (`src-03`).
- **S-047-R11** Applying (in this order): if `restart` and a media object exists: destroy it. If a media object still exists: `set_looping(looping)` then
  `set_is_linear_alpha(linear_alpha)` (every update). If `(NOT close_when_inactive OR active) AND restart`: open (R13). Log the settings dump (R12).
  If `(NOT restart_on_activate OR active) AND restart`: start (R14).
- **S-047-R12** Settings dump: when `log_changes` (the NEW value) is true, one INFO log `[Media Source '<name>']: settings: ...` listing input, input_format
  (`(null)` for NULL), speed, looping, linear alpha, hw, clear_on_media_end, restart_on_activate, close_when_inactive, full_decode (yes/no) and ffmpeg_options.
- **S-047-R13** open: if `input` is non-empty, create the media object with info: `opaque := source object`, the five callbacks (R15, R16),
  `path := input`, `format := input_format` (NULL for a local file), `buffering := buffering_mb * 1048576` (32-bit), `speed`, `force_range := range`,
  `is_linear_alpha`, `hardware_decoding := hw`, `ffmpeg_options`, `is_local_file := is_local OR seekable`, `reconnecting := the current flag`,
  `request_preload := is_stinger`, `full_decode`. The result (possibly NULL when the player fails) replaces `media`. Empty input: nothing happens.
- **S-047-R14** start: if no media: open; if still none: return. `play(looping, reconnecting)`. If `is_local AND has_video AND (clear_on_end OR looping)`:
  `show_preloaded_video`, else `output_video(NULL)`. Set state PLAYING and call `media_started`.

### 5.4 Media source: player callbacks

- **S-047-R15** Frame callback -> `output_video(frame)`. Seek frame callback -> `set_video_frame`. Audio callback -> `output_audio`, then (not local and the
  `reconnecting` flag was set) clear the flag atomically and log INFO `Reconnected.`. Preload callback: ignored when `close_when_inactive`;
  otherwise `preload_video` only if `clear_on_end OR looping`; then the same Reconnected handling as audio (not local and flag was set).
- **S-047-R16** Stop callback (media ended / failed): if `clear_on_end AND NOT is_track_matte`: `output_video(NULL)`. If `(close_when_inactive OR NOT is_local) AND media`:
  set `destroy_media`. If `state != STOPPED`: state := ENDED and `media_ended`. (A user stop sets STOPPED first, so no ENDED notification; `src-05`.)

### 5.5 Media source: reconnect state machine

- **S-047-R17** tick (video_tick, `seconds` ignored): only if `destroy_media`: destroy the media object (if any), clear `destroy_media`; if NOT is_local:
  lock the mutex; if the `reconnecting` flag was clear: set it and log WARNING `Disconnected. Reconnecting...`; if a thread record is valid: signal the stop event, join, mark invalid,
  reset the event; create a thread running R18; on failure log WARNING `Could not create reconnect thread` and unlock (the flag stays set, no thread valid, Q-2); else mark valid; unlock.
- **S-047-R18** Reconnect thread: `ret := wait(stop_event, reconnect_delay_sec * 1000 ms)`; if `ret == 0` (event signalled) OR a media object exists: exit.
  `active := source_active`. If `NOT close_when_inactive OR active`: open. If `NOT restart_on_activate OR active`: start. Exit.
- **S-047-R19** stop_reconnect_thread: no-op for a local source (by the CURRENT value of `is_local`, which update has not yet overwritten, R07). Otherwise under the mutex: if a thread
  record is valid: signal the event, join, mark invalid, clear `reconnecting`, reset the event.

### 5.6 Media source: controls

- **S-047-R20** activate: if `restart_on_activate`: `media_restart` (host). deactivate: if `restart_on_activate` and media: `stop`; if `clear_on_end` also `output_video(NULL)`.
- **S-047-R21** media_restart: if `source_showing`: start (R14). Then state := PLAYING (even when not showing or start failed). media_play_pause(pause): open if no media; none: return;
  `play_pause(pause)`; state := PAUSED, or PLAYING + `media_started` when resuming. media_stop: only with media: `stop`, `output_video(NULL)`, state := STOPPED.
- **S-047-R22** media_get_duration := player duration / 1000 (ms; 0 without media). media_get_time := player time (0 without media). media_set_time(ms): `seek` if media. media_get_state := state.
- **S-047-R23** Hotkeys (callbacks run when `pressed`; releases are ignored, return false for the pair): restart: if showing: `media_restart`. Play (returns bool): false if `state == PLAYING` or not showing, else `media_play_pause(false)` and true.
  Pause: false if `state != PLAYING` or not showing, else `media_play_pause(true)` and true. Stop: if showing: `media_stop`.
- **S-047-R24** Procs: `restart` behaves as the restart hotkey pressed. `preload_first_frame`: if `is_track_matte`: `output_video(NULL)`; then `preload_frame` (no-op without media).
  `get_duration`: `duration := player duration (us) * 1000` (0 without media). `get_nb_frames`: `num_frames := player frames` (0 without media).
- **S-047-R25** missing_files: a report object is always returned; if `is_local` and `input` non-empty and the file does not exist: add one file (path = input, kind SOURCE) whose replacement callback receives the **plugin data**
  (not the host source), writes `local_file := new_path` into the host's current settings and calls the host `update(source, settings)`, then releases the settings. Network sources and empty paths report nothing.
- **S-047-R27** Looping enabled while ENDED: covered by R08 (needs `active`).

### 5.7 Audio encoders (runtime half; create-time half is S-025 R62-R64)

- **S-047-R30** Registration as 3.1. **R31** `get_defaults`: `bitrate` = 128. `get_properties`: one int `bitrate` `T("Bitrate")` min 64 max 1024 step 32, number type scroller (0, not slider), no flags.
- **S-047-R32** Derived sizes after open (S-025 R64 formula) use the sample-format map (R59): `frame_size = ctx.frame_size or 1024`, `audio_size = bytes(format) * (planar ? 1 : channels)`, `planes = planar ? channels : 1`,
  `frame_size_bytes = frame_size * audio_size`, where `format` is the OBS audio format of the context sample format and `channels` the host speaker layout's channel count. The sample buffer holds `frame_size` samples for
  `ctx.ch_layout` channels in `ctx.sample_fmt`.
- **S-047-R33** On failure after the codec object was created (`open` error, buffer allocation error) the object is released and create returns failure (`aud-02` tail).
- **S-047-R34** Encode step `encode(frame, packet, received)`: copy `frame_size_bytes` bytes from `frame.data[i]` into plane buffer `i` for `i < planes`; then:

```
aframe.nb_samples := frame_size
aframe.pts := rescale_q(total_samples, {1, ctx.sample_rate}, ctx.time_base)        // plain av_rescale_q: nearest, ties away from zero
aframe.ch_layout := ctx.ch_layout
r := fill_audio_frame(aframe, channels, ctx.sample_fmt, buffer0, frame_size_bytes * channels, align 1)
if r < 0: WARNING, return false                                // total_samples NOT advanced
total_samples += frame_size
r := send_frame(aframe); if r == 0: r := receive_packet(&avpkt)
got := (r == 0); if r is "again" or "end of file": r := 0
if r < 0: WARNING, return false                                // *received not written
*received := got
if not got: return true
packet_buffer := avpkt.data[0..size]                           // size 0 allowed
packet.pts := rescale_ts(avpkt.pts), dts := rescale_ts(avpkt.dts)    // rescale_ts(v) = rescale_q_rnd(v, ctx.time_base, {1, ctx.sample_rate}, NEAR_INF | PASS_MINMAX); INT64_MIN passes unchanged
packet.data := packet_buffer, size := avpkt.size, type := AUDIO, keyframe := true, timebase_num := 1, timebase_den := ctx.sample_rate
unref avpkt; return true
```
  "again" is the codec's EAGAIN (`-EAGAIN` as the codec reports it), "end of file" is `AVERROR_EOF` (-541478725). A send result of again/EOF skips receive and yields "no packet, success".
- **S-047-R35** Time stamps: the input pts of frame n (0-based, before it) is `n * frame_size` rescaled from `1/sample_rate` to the codec time base (`1/1000`, 48 kHz, 1024: 0, 21, 43), the output pts/dts are rescaled back (50 -> 2400) (`aud-03`).
- **S-047-R36** See R34 for the error mapping (send error, receive error, fill error -> false; again/EOF -> success without packet).
- **S-047-R37** Packet fields are exactly those of R34; every other field of the packet struct is untouched.
- **S-047-R38** `get_audio_info(info)`: `format := map(ctx.sample_fmt)` (R59), `samples_per_sec := ctx.sample_rate`, `speakers := channels` if `channels != 7 AND channels <= 8` (channels is `ctx.ch_layout.nb_channels`;
  the OBS speaker number equals the channel count for 1..6 and 8, 4.1 = 5 channels = number 5, 2.1 = 3 channels = number 3; 0 channels gives 0 = UNKNOWN), else UNKNOWN. The float variant (pcm32 only) then sets `allow_clipping := true`; the plain variant does not touch it.
- **S-047-R39** `get_extra_data`: always true; returns `ctx.extradata` and `extradata_size` (pointer may be NULL with size 0). `get_frame_size`: `frame_size`. `get_priming_samples` (aac and opus only): `ctx.initial_padding`.
- **S-047-R40** destroy releases the sample buffer, the context, the frame and the packet buffer; safe at any state after create started.

### 5.8 Video encoder wrapper

State record `E`: encoder handle, `enc_name`, codec, context, `start_ts`, `first_packet`, `vframe`, `buffer` (bytes), `height`, `initialized`, `parent`, callbacks `on_init_error(parent, code)` and `on_first_packet(parent, avpkt, &buffer)`.

- **S-047-R50** init(E, parent, encoder, lib, lib2, enc_name, on_init_error, on_first_packet): `codec := find(lib)`; if none and `lib2` given: `find(lib2)`. Store enc_name and callbacks, `first_packet := true`. Log an INFO separator line `---------------------------------`.
  If no codec: set the encoder last-error to `Couldn't find encoder: <lib>` (the PRIMARY name even when lib2 was tried), WARNING, return false. Allocate the context; failure: WARNING, false. Else true (`initialized` stays false). **D-1** applies to the not-found path.
- **S-047-R51** init_codec(E): `r := open(context, codec)`. On `r < 0`: if the encoder has NO last error yet: if `on_init_error` is set, call it with `(parent, r)`, otherwise set the last error to
  `T("Encoder.Error")` with `%1 := enc_name`, `%2 := <text of r>` followed by `<br><br>`. Return false (neither is done when a last error exists). `vframe` is not allocated in this case.
- **S-047-R52** Frame setup: allocate `vframe`; failure: WARNING, false. Copy from the context: `format := pix_fmt, width, height, color_range, color_primaries, color_trc, colorspace, chroma_location`. Allocate its buffers with alignment 32
  (the OBS default allocation alignment); failure: WARNING `Failed to allocate vframe`, return false (`initialized` stays false, `vframe` stays allocated until free). Success: `initialized := true`, return true.
- **S-047-R53** `ffmpeg_video_encoder_update` is S-025 R60. For this spec: it sets `E.height := context.height` (the encoder's output height), which R54 uses.
- **S-047-R54** Frame copy `copy(vframe, frame, height, pix_fmt)`: `(hs, vs)` := chroma shifts of `pix_fmt`. For plane `p` in 0..7 with `frame.data[p] != NULL`: `bytes := min(frame.linesize[p], vframe.linesize[p])`;
  `rows := height >> (p == 0 ? 0 : vs)` (arithmetic shift, so odd heights floor: Q-4; applied to every plane >= 1 including the alpha plane of YUVA formats: Q-4); copy `bytes` bytes from row start `y * frame.linesize[p]` to row start `y * vframe.linesize[p]` for `y < rows`.
  Nothing else is written (padding and rows beyond `rows` keep their previous content). `hs` is unused. The behaviour for a pix_fmt without chroma information is unspecified (**D-2**: required: shifts 0).
- **S-047-R55** encode(E, frame, packet, received): `cur := now_ns`; `pause := encoder pause offset`; if `start_ts == 0`: `start_ts := cur`. Copy (R54), `vframe.pts := frame.pts`, `r := send_frame(vframe)`, if `r == 0`: `r := receive_packet`.
  `got := (r == 0)`; again/EOF -> `r := 0`; `r < 0`: WARNING `... Error encoding`, return false (`received` not written). If `got AND size > 0` -> packet path (R56, R57), else `*received := false`. The AVPacket is always unreferenced. Return `NOT timeout`.
- **S-047-R56** Packet path: if `on_first_packet` is set and `first_packet`: call `on_first_packet(parent, avpkt, &buffer)` (the buffer is NOT cleared first; the callback decides its content) and clear `first_packet`; otherwise `buffer := avpkt.data[0..size]` (replace).
  Then `packet.pts := avpkt.pts, dts := avpkt.dts, data := buffer, size := buffer.length, type := VIDEO, keyframe := (avpkt.flags & KEY) != 0`, `*received := true`. A packet with size 0 takes the "no packet" path and does not consume `first_packet`.
- **S-047-R57** Timeout: `recv_ts := mul_div_u64(avpkt.pts as u64, 1_000_000_000, context.time_base.den) + start_ts` (128-bit intermediate); `time_base.den` = `fps_num` (S-025 R60). If `(cur - recv_ts - pause) > 5_000_000_000` (signed 64-bit): set the last error to `T("Encoder.Timeout")` with
  `%1 := enc_name`, `%2 := "5"`, log ERROR, `timeout := true`. The packet fields were already written and `*received` is true; the function returns false. Exactly 5 s is not a timeout (`vid-04`).
- **S-047-R58** free(E): if `initialized`: `send_frame(NULL)`; then repeat `receive_packet` until it fails, unreferencing each received packet. Then free the context (also when NULL), unref and free `vframe` (also when NULL), free `buffer`. Safe after any failed init stage.
- **S-047-R59** Format helpers: sample format -> OBS audio format: U8 -> U8BIT, S16 -> 16BIT, S32 -> 32BIT, FLT -> FLOAT, U8P -> U8BIT_PLANAR, S16P -> 16BIT_PLANAR, S32P -> 32BIT_PLANAR, FLTP -> FLOAT_PLANAR, anything else (incl. DBL, DBLP, S64) -> 16BIT (Q-5).
  `rescale_ts(v, context, new_base) = rescale_q_rnd(v, context.time_base, new_base, NEAR_INF | PASS_MINMAX)`. Pixel format map and chroma location: S-025 R60/R61.

## 6. Algorithms

Everything with precise observable results is given in pseudocode in sections 5.3 (update), 5.5 (reconnect) and 5.7 (encode step). The reconnect thread and the host are modelled as follows in the vectors (the
conformance fake is single-threaded): a created thread is "pending"; joining a pending thread first runs its body (the stop event is signalled at that point, so the wait returns at once and the body exits); `thread_run` runs a pending body with
an unsignalled event so the wait times out. A body that finished stays "valid" until the next join, exactly as in the reference.

## 7. Concurrency

- Callbacks of the player (frame, audio, stop) arrive on the player's thread; `video_tick`, update, activate etc. on the host's threads; the reconnect body on its own thread.
- `reconnecting` is an atomic flag (test-and-set/clear); `destroy_media` is written by the player thread and read by tick without synchronisation (benign race); the reconnect thread record and event are protected by `reconnect_mutex` only in `tick` and `stop_reconnect_thread`.
  `media` is read/written by the reconnect body and by the host without a lock (Q-2/Q-6): a Rust port SHOULD guard `media` with a mutex and keep the observable sequences of section 5.
- `destroy` and `update` join the reconnect thread (R19) before touching state, so the body never outlives the object.
- The encoders are driven by one host encoder thread per instance; no internal locking.

## 8. Reference defects and quirks

- **Q-1** (QUIRK) Changes to `buffering_mb`, `full_decode`, `clear_on_media_end` and `log_changes` take effect only at the next restart (`src-02` last block). SHOULD match.
- **Q-2** (QUIRK) If the reconnect thread cannot be created, `reconnecting` stays set and nothing retries until the user restarts the source (`src-06` end). SHOULD match.
- **Q-3** (QUIRK) `fill_audio_frame` gets `buf_size = frame_size_bytes * channels` although for packed formats `frame_size_bytes` already contains the channel count (over-count by a factor `channels`; harmless to the codec). For planar formats the value is the exact total. Vector `aud-03` prints it (`buf_size=8192` planar 2 ch, `buf_size=16384` packed 2 ch). Irrelevant to a Rust port unless a codec abstraction checks it.
- **Q-4** (QUIRK) Frame copy: chroma-shifted plane height is `height >> vs` (floor) for ALL planes >= 1 including a full-size alpha plane of YUVA formats, so an alpha plane is only half copied and an odd height loses the last chroma row (`vid-03`). MUST match for bit-exact frames; a Rust implementation MAY fix it only knowingly (output changes).
- **Q-5** (QUIRK) Unknown sample formats map to 16-bit; the buffers are sized with that mapping while the codec uses the real format (`aud-02`, `dbl`).
- **Q-6** (QUIRK) Destroy unregisters only the restart hotkey; the Play/Pause pair and Stop are cleaned up by the host. (`src-08` end).
- **Q-7** (QUIRK) `rist` is a 4-byte case-insensitive prefix test (`ristretto://` matches). MUST match.
- **Q-8** (QUIRK) Visibility is not applied by get_properties (all visible at first). 
- **Q-9** (QUIRK) Video encode: on timeout the call returns false even though the packet was delivered and `received` is true.
- **Q-10** (QUIRK) Audio encode: on send/receive/fill errors `received` is not written (callers pass false).
- **D-1** (DEFECT) `ffmpeg_video_encoder_init`, codec-not-found path: the error-text string object is used without initialisation, so the reference crashes or works depending on the stack (`vid-01`: the stack filled with 0xFF crashes, with zeros works).
  Required: set the last error `Couldn't find encoder: <lib>`, log WARNING, return false, never crash. Scripts: `vid-01-init`, `vid-05-free` (vector records both outcomes; a conforming implementation matches the `zero` case and is not required to crash for `ones`).
- **D-2** (DEFECT, not testable) copy with a pixel format that has no descriptor reads uninitialised chroma shifts. Required: treat as 0.

## 9. Implementation notes (non-normative)

- Model the player, host, codec and clock as traits; make the reconnect body a function `reconnect_body(&self)` and provide a thread-less test double like the conformance fake.
- `Settings` -> an immutable struct per update; keep the `Source` fields of R10 in a struct guarded by one mutex except the atomic `reconnecting`.
- `rescale_q`: use i128 math: `round_half_away(a * bq.num * aq.den ... )`; `PASS_MINMAX` keeps `i64::MIN` and `i64::MAX` untouched. Do not call libav in the Rust crate unless you link it for the encoders themselves.
- The `enc_name` borrowed by the wrapper must outlive it (the conformance runner had to copy script tokens for this).

## 10. Conformance coverage

| Rule | Scripts |
|---|---|
| R01 | src-00-registration |
| R02-R05 | src-01-defaults-props |
| R06 | src-10-create-failures, src-01..09 (creation prologue) |
| R07-R13 | src-02-update-local, src-03-update-network |
| R14 | src-04-activate-restart, src-07-playback-control |
| R15, R16 | src-05-media-callbacks, src-06-reconnect |
| R17-R19 | src-06-reconnect |
| R20, R21 | src-04-activate-restart, src-07-playback-control |
| R22 | src-07-playback-control |
| R23, R24 | src-08-hotkeys-procs |
| R25 | src-09-missing-files |
| R26 | src-08-hotkeys-procs, src-06-reconnect |
| R27 | src-10-create-failures |
| R30, R31 | aud-01-registration |
| R32, R33, R59 | aud-02-create-and-sample-format |
| R34-R37 | aud-03-encode |
| R38, R39 | aud-04-info-callbacks |
| R40 | aud-02, aud-03 (destroy) |
| R50 | vid-01-init |
| R51, R52, R53 | vid-02-init-codec |
| R54 | vid-03-encode |
| R55-R57 | vid-04-packets-errors |
| R58 | vid-05-free |
| Q-1 | src-02 | Q-2 | src-06 | Q-3 | aud-03 | Q-4 | vid-03 | Q-5 | aud-02 | Q-6 | src-08 | Q-7 | src-03 | Q-9 | vid-04 | Q-10 | aud-03 | D-1 | vid-01, vid-05 |
| D-2 | not testable |

Tolerance: none (no floating point output). Vector lines `log LEVEL TEXT`: compare the level; the texts of channel layouts come from libavutil (`stereo`, `5.1(side)`) and may differ between FFmpeg versions.

## 11. Open questions

- **Rust sufficiency check not performed** (cost control, instructed by the caller): no Rust implementation was written against this spec; spec bugs it would find are still undiscovered.
- Exact behaviour of the real media player when `media_playback_create` fails on a network source mid-reconnect is S-036's.
- The host-side semantics of `obs_source_media_restart` (it calls back `media_restart` on the graphics/UI thread) are S-040; this spec only records the request.
- `av_err2str` texts for real codec error codes are library-specific; only the template (`Failed to open <name>: <text><br><br>`) is normative.
- Thread timing of the real reconnect body (`os_event_timedwait` precision) is not specified.
- Not linked/faked: libavcodec (every call), libavformat, the real player, libobs source/hotkey/proc/missing-files modules, real threads and events.

## 12. Round 9: file ownership of the ffmpeg source/encoders + OpenH264 + amf-test (added)

§5.1–§5.8 pin the observable behaviour of the media source, the seven audio encoders and the video
encoder wrapper, but the `.c` files themselves were not cited. This section maps the five remaining
files to the rules that own them and pins the two components no existing rule covers: the OpenH264
H.264 encoder (`obs-ffmpeg-openh264.c`) and the standalone AMF diagnostic program
(`obs-amf-test/obs-amf-test.cpp`).

### Reference files (all cited)

- `plugins/obs-ffmpeg/obs-ffmpeg-source.c`, `plugins/obs-ffmpeg/obs-ffmpeg-audio-encoders.c`, `plugins/obs-ffmpeg/obs-ffmpeg-video-encoders.c`, `plugins/obs-ffmpeg/obs-ffmpeg-openh264.c`, `plugins/obs-ffmpeg/obs-amf-test/obs-amf-test.cpp`

### 12.1 File-to-rule map: media source, audio encoders, video encoders

- **S-047-R60 [SRC]** `obs-ffmpeg-source.c` (38 fns): full `struct ffmpeg_source` field list, in order:
  `media_playback_t *media; bool destroy_media; enum video_range_type range; bool is_linear_alpha;
  obs_source_t *source; obs_hotkey_id hotkey; char *input; char *input_format; char *ffmpeg_options;
  int buffering_mb; int speed_percent; bool is_looping; bool is_local_file; bool is_hw_decoding; bool
  full_decode; bool is_clear_on_media_end; bool restart_on_activate; bool close_when_inactive; bool
  seekable; bool is_stinger; bool is_track_matte; bool log_changes; pthread_t reconnect_thread;
  pthread_mutex_t reconnect_mutex; bool reconnect_thread_valid; os_event_t *reconnect_stop_event;
  volatile bool reconnecting; int reconnect_delay_sec; enum obs_media_state state; obs_hotkey_pair_id
  play_pause_hotkey; obs_hotkey_id stop_hotkey`. `hotkey` is the restart hotkey, `play_pause_hotkey`
  the Play/Pause pair, `stop_hotkey` the stop hotkey (R06 step 3). Behaviour: R01–R27 (UI R01–R05,
  lifecycle R06/R26, update R07–R14, player callbacks R15–R16, reconnect R17–R19, controls R20–R25).
- **S-047-R61 [SRC]** `obs-ffmpeg-audio-encoders.c` (19 fns): the per-encoder `enc_create` arguments
  (registration itself: 3.1 table; create procedure: S-025 R62–R64): `ffmpeg_aac` = ("aac", no
  fallback, AV_SAMPLE_FMT_NONE), `ffmpeg_opus` = ("libopus", fallback "opus", AV_SAMPLE_FMT_FLT),
  `ffmpeg_pcm_s16le` = ("pcm_s16le", NONE), `ffmpeg_pcm_s24le` = ("pcm_s24le", NONE),
  `ffmpeg_pcm_f32le` = ("pcm_f32le", NONE), `ffmpeg_alac` = ("alac", AV_SAMPLE_FMT_S32P),
  `ffmpeg_flac` = ("flac", AV_SAMPLE_FMT_S16). `enc_defaults`/`enc_properties` are R31; runtime
  (encode, packet fields, audio info, extra data, destroy) R32–R40.
- **S-047-R62 [SRC]** `obs-ffmpeg-video-encoders.c` (6 fns): only the shared wrapper —
  `ffmpeg_video_encoder_init` (R50), `ffmpeg_video_encoder_init_codec` (R51–R53),
  `ffmpeg_video_encoder_update` (R53/S-025 R60), `ffmpeg_video_encoder_free` (R58), static `copy_data`
  (R54) and `ffmpeg_video_encode` (R55–R57). No encoder is registered in this file (NVENC info:
  S-046; OpenH264: 12.2).

### 12.2 OpenH264 encoder (`obs-ffmpeg-openh264.c`, new component)

- **S-047-R63** Info record: `id = "ffmpeg_openh264"`, type VIDEO, `codec = "h264"`, get_name "OpenH264";
  callbacks create/destroy/encode/defaults/properties/extra-data/video-info as the file's functions; no
  audio_info. `struct openh264_encoder { struct ffmpeg_video_encoder ffve; enum openh264_encoder_type
  type; DARRAY(uint8_t) header }` (the enum has the single value `H264_ENCODER_TYPE_OH264`, set when
  `enc_lib == "libopenh264"`).
- **S-047-R64** create: gate 1 — `voi->format` must be in the whitelist
  {I420, NV12, YVYU, YUY2, UYVY, RGBA, BGRA, BGRX, BGR3, Y800, I444, I422, I40A, I42A, YUVA, AYUV},
  else last-error `T("H264.UnsupportedVideoFormat")`, one ERROR log, return failure. Gate 2 —
  `voi->colorspace` must be DEFAULT or 709, else `T("H264.UnsupportedColorSpace")`, one ERROR log,
  failure. Then allocate (zeroed), set the type, `ffmpeg_video_encoder_init` (R50) with
  `enc_lib = "libopenh264"`, `enc_name = "OpenH264"`, `on_first_packet` callback = this file's; then
  update (R65). Any init/update failure: destroy (R68) and return failure.
- **S-047-R65** update: read `profile` (string), `bitrate`, `keyint_sec` (ints). Build the scale info
  from the video output (format, colorspace, range), set `context->thread_count = 0`, then overwrite
  `info.format = I420` (OpenH264 only supports I420 output), then on `context->priv_data`:
  `rc_mode = "quality"`, `profile = profile`, `allow_skip_frames = 1` (required by quality mode).
  `ffmpeg_video_encoder_update(bitrate, keyint_sec, voi, &info, ffmpeg_opts)` (R53), then one INFO log
  "settings:" with encoder, rc_mode, bitrate, keyint_sec, profile, width, height, ffmpeg opts, then
  `flags |= AV_CODEC_FLAG_GLOBAL_HEADER`, then `init_codec` (R51) — its result is the update result.
- **S-047-R66** `on_first_packet(parent, avpkt, da)` (invoked via the R56 first-packet path): copy
  `context->extradata` (`extradata_size` bytes) into `header`, then append the first packet's bytes —
  `header` = extradata ++ first packet. `get_extra_data` always true, returns `header.array`/`num`.
  `get_video_info`: output format I420 (as in R65). destroy: `ffmpeg_video_encoder_free` (R58), free
  `header`, free the object. encode: `ffmpeg_video_encode` (R55–R57).
- **S-047-R67** Defaults: `bitrate = 6000`, `profile = "main"`. Properties: list `profile`
  {constrained_baseline, main, high}; int `bitrate` 50..300000 step 50, suffix " Kbps"; int `keyint_sec`
  0..20 step 1, suffix " s"; text `ffmpeg_opts`.

### 12.3 AMF diagnostic program (`obs-amf-test/obs-amf-test.cpp`, new component)

- **S-047-R68 [SRC]** Standalone Windows console program (own executable, **not** part of the
  obs-ffmpeg plugin): `main` duplicates its main-thread handle, spawns `TimeoutThread` —
  `WaitForSingleObject(hMainThread, 2500)`; on `WAIT_TIMEOUT`:
  `TerminateProcess(GetCurrentProcess(), STATUS_TIMEOUT)` — then `LoadLibraryW(AMF_DLL_NAME)` (throw
  "Failed to load AMF lib" on failure), `GetProcAddress(amf_module, AMF_INIT_FUNCTION_NAME)` (throw
  "Failed to get init func"), `AMFInit(AMF_FULL_VERSION, &amf_factory)` (throw "AMFInit failed").
  `argv[1..argc-1]` are parsed as hex u64 into `luid_order` (the expected adapter LUID order).
  `CreateDXGIFactory1` (throw on failure); loop `get_adapter_caps(factory, idx++)` (R69) until
  `EnumAdapters` fails. Then for each stored adapter, in key order, print exactly:
  `[%u]\nis_amd=%s\nsupports_avc=%s\nsupports_hevc=%s\nsupports_av1=%s\n` (true/false). The whole
  main body is one `try/catch(const char *text)`: on throw print `[error]\nstring=%s\n` and **return 0**
  (Q-11). On MSVC the file exports `AmdPowerXpressRequestHighPerformance = 1` (dllexport) so the AMD
  driver picks the high-performance GPU.
- **S-047-R69 [SRC]** `get_adapter_caps(factory, adapter_idx)`: `EnumAdapters` (failure: return false —
  end of loop); take `DXGI_ADAPTER_DESC`; `luid_idx := index of desc.AdapterLuid in luid_order, else
  adapter_idx`; `adapter_info[luid_idx]` is the caps slot (default all-false). If `desc.VendorId !=
  0x1002` (AMD): leave the slot all-false, return true. Else set `is_amd`, create a D3D11 device on the
  adapter (`D3D_DRIVER_TYPE_UNKNOWN`, no debug, flags 0, `D3D11_SDK_VERSION`; failure: return true,
  slot left as-is), `amf_factory->CreateContext` (failure: return true), `amf_context->InitDX11(device)`
  (failure: return true), then `supports_avc/hevc/av1 := CreateComponent(context,
  AMFVideoEncoderVCE_AVC / AMFVideoEncoder_HEVC / AMFVideoEncoder_AV1) == AMF_OK`.

### 12.4 Quirks (added)

- Q-11: the amf-test program exits 0 on every handled failure — the catch block prints the message and
  returns 0; only the 2500 ms watchdog can kill it (with `STATUS_TIMEOUT`). Consumers must parse stdout,
  not the exit code.
- Q-12: non-AMD adapters still occupy an `adapter_info` slot and are printed (all `false`); an adapter
  whose D3D11/AMF context creation fails is likewise printed with only `is_amd=true`. The LUID
  remapping only reorders output for adapters whose LUID appears in `argv`; unknown LUIDs keep their
  enumeration index.
