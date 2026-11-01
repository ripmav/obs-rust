# S-046 `ffoutputs`: the obs-ffmpeg output family (ffmpeg_output, ffmpeg_muxer, replay_buffer, HLS, MPEG-TS glue)

| | |
|---|---|
| Status | Ready for review (Rust sufficiency check skipped on request, see 11) |
| Reference | libobs 33.0.1, `plugins/obs-ffmpeg/obs-ffmpeg-output.c`/`.h` (`ffmpeg_output`), `obs-ffmpeg-mux.c`/`.h` (`ffmpeg_muxer`, `ffmpeg_mpegts_muxer`, `replay_buffer`), `obs-ffmpeg-hls-mux.c`, `obs-ffmpeg-formats.h` |
| Target crate | `crates/obs-ffmpeg-outputs` (sender side of S-037; links libav only for `ffmpeg_output`) |
| Depends on | S-037 (wire format and helper process, NOT repeated here), S-001 (process pipe), S-014 (obs_data), S-012 (NAL priority), S-023 (`os_generate_formatted_filename`), S-041 (output lifecycle, stop codes) |
| Conformance | `test/conformance/scripts/ffoutputs/` (39 scripts) |

## 1. Scope

Five output types are registered by the obs-ffmpeg module (ids in 3.1). Four of them do not touch libav: they hand
already encoded packets to the helper process `obs-ffmpeg-mux` over a pipe (wire format, command line layout and helper
behaviour: **S-037**). This spec defines everything the *sender* decides:

* which settings it reads and with which defaults (3.2),
* start/stop/failure state machine and the stop codes it signals (section 5.2-5.4),
* the exact command line it builds (5.3; the layout is S-037 section 4, the value derivation is here),
* split-file recording: when to split, timestamp rebasing, file name generation (5.6, 5.7),
* the replay buffer ring and its save (5.8),
* the HLS queue, drop policy and worker (5.9),
* `ffmpeg_output`, the only output that links libav: settings -> configuration, stream set-up, buffering of
  raw audio, packet queue and write thread (5.10-5.13).

Out of scope (other specs or not specified): the helper program (S-037), the encoders, libavformat/libavcodec internals,
the *native* SRT/RIST MPEG-TS output (`obs-ffmpeg-mpegts.c`, `obs-ffmpeg-srt.h`, `-rist.h`, `-url.h`, used instead of
`ffmpeg_mpegts_muxer` when the build option `ENABLE_NEW_MPEGTS_OUTPUT` is on, which is the default; see 11), the hardware
and software *encoders* of the module (`obs-ffmpeg-vaapi.c`, `-nvenc.c`, `-av1.c`, `-video-encoders.c`, `-audio-encoders.c`,
`vaapi-utils.c`; they are encoders, not output glue), the frontend's recording/replay buffer UI.

## 2. Terminology and conventions

RFC 2119 words apply. "Stream" = the per-output state object (`struct ffmpeg_muxer`) of a mux-based output, "output"
= the libobs output object that owns it. Times are microseconds ("usec", `dts_usec`, `sys_dts_usec`) unless stated;
`stop(ts)` takes **nanoseconds** and the stream stores `ts / 1000` (integer division, truncation). "Packet" = libobs
`encoder_packet`: `pts`, `dts` in the packet's own time base, `dts_usec`/`sys_dts_usec` in usec, `keyframe`,
`track_idx`, `type` (video/audio), `size`, `timebase_den`, `drop_priority`. `HDR peak` = `obs_get_video_hdr_nominal_peak_level()`.
`stop code` names are the libobs `OBS_OUTPUT_*` values (S-041): `BAD_PATH -1`, `CONNECT_FAILED -2`, `INVALID_STREAM -3`, `ERROR -4`,
`DISCONNECTED -5`, `UNSUPPORTED -6`, `NO_SPACE -7`, `ENCODE_ERROR -8`.

## 3. Interface

### 3.1 Registered outputs

| id | flags | extra | callbacks |
|---|---|---|---|
| `ffmpeg_output` | VIDEO, AUDIO, MULTI_TRACK, CAN_PAUSE (0x33) | raw video + raw audio (`raw_video`, `raw_audio2`) | create, destroy, start, stop, total bytes |
| `ffmpeg_muxer` | AV, ENCODED, MULTI_TRACK, CAN_PAUSE (0x37) | | create, destroy, start, stop, encoded_packet, total bytes, properties |
| `ffmpeg_mpegts_muxer` (only if `ENABLE_NEW_MPEGTS_OUTPUT` is OFF) | AV, ENCODED, MULTI_TRACK, SERVICE (0x1f) | protocols `SRT;RIST`, video `h264`, audio `aac;opus` | same as `ffmpeg_muxer` + `get_connect_time_ms` (always 0) |
| `ffmpeg_hls_muxer` | AV, ENCODED, MULTI_TRACK, SERVICE (0x1f) | protocols `HLS`, video `h264` (`h264;hevc` when HEVC is enabled in the build), audio `aac` | create, destroy, start, stop, encoded_packet, total bytes, dropped frames |
| `replay_buffer` | AV, ENCODED, MULTI_TRACK, CAN_PAUSE (0x37) | | create, destroy, start, stop, encoded_packet, total bytes, **defaults** |

Display names are the module texts `FFmpegOutput`, `FFmpegMuxer`, `FFmpegMpegtsMuxer`, `FFmpegHlsMuxer`, `ReplayBuffer`.
The module registers the outputs in the order `ffmpeg_output`, `ffmpeg_muxer`, `ffmpeg_mpegts_muxer` (if compiled), `ffmpeg_hls_muxer`, `replay_buffer`.
`ffmpeg_muxer` and `ffmpeg_mpegts_muxer` share all callbacks; the only difference at runtime is the SERVICE flag (network mode, R12).

Signals and procedures added at create time:

* `ffmpeg_muxer`/`ffmpeg_mpegts_muxer`: signal `void file_changed(string next_file)`; procedure `void split_file(out bool split_file_enabled)`.
* `replay_buffer`: procedures `void save()` and `void get_last_replay(out string path)`; signals `void saved()` and `void saving(ptr output)`.
* `ffmpeg_hls_muxer`, `ffmpeg_output`: none.

### 3.2 Settings

All keys are read from the output's settings object at the time named in the rule. `int` values are 64-bit; narrowing to `int` is a plain C cast.

| output | key | type | default | use |
|---|---|---|---|---|
| mux (file) | `path` | string | none (empty) | file written by the helper |
| mux (file) | `muxer_settings` | string | empty | passed to the helper (R33) |
| mux (file) | `split_file` | bool | false | enable split recording (R50) |
| mux (file) | `max_time_sec`, `max_size_mb` | int | 0 (unlimited) | split thresholds |
| mux (file) | `allow_overwrite` | bool | false | split file names may overwrite |
| mux (file) | `directory`, `format`, `extension`, `allow_spaces` | string, string, string, bool | empty, empty, empty, false | split file names (5.7) |
| replay_buffer | `max_time_sec` | int | **15** (via defaults callback) | ring duration limit, seconds |
| replay_buffer | `max_size_mb` | int | **500** | ring size limit, MiB |
| replay_buffer | `format` | string | **`%CCYY-%MM-%DD %hh-%mm-%ss`** | file name pattern |
| replay_buffer | `extension` | string | **`mp4`** | |
| replay_buffer | `allow_spaces` | bool | **true** | |
| replay_buffer | `directory` | string | empty | no default |
| video encoder | `bitrate` (int, kbit/s), `codec_type` (int, packed fourcc) | | 0 | command line |
| audio encoder | `bitrate` | int | 0 | command line |
| video encoder (HLS) | `keyint_sec` | int | 0 | HLS thresholds |
| ffmpeg_output | `url`, `format_name`, `format_mime_type`, `muxer_settings`, `video_encoder`, `audio_encoder`, `video_settings`, `audio_settings` | string | empty | R120 |
| ffmpeg_output | `video_bitrate`, `audio_bitrate`, `video_encoder_id`, `audio_encoder_id`, `scale_width`, `scale_height` | int | 0 | R120 |
| ffmpeg_output | `gop_size` | int | **120** (written as a *default* into the live settings by start) | |
| ffmpeg_output | `audio_names` | array of objects with string `name` | absent | R125 |

The replay buffer's `get_defaults` is the only defaults callback in this family. Defaults are applied by the host before `start`; a
`replay_buffer` started on settings without them has `max_time = max_size = 0`, i.e. an unbounded ring (R82).

## 4. Data formats

The pipe byte stream, the 32-byte `ffm_packet_info` and the argument list are specified in S-037. The sender-side value
derivation of the arguments is R30-R36. Worked example (vector `02-cmdline-colour`, `03-cmdline-tracks`, `07-headers-and-packets`):
an `h264` encoder at 6000 kbit/s on a 1280x720 output, BT.709 limited range, NV12, 30/1 fps, one AAC track (48000 Hz, frame 1024,
priming 1024, 160 kbit/s) and no muxer settings gives the list (after `argv[0]`, the helper path):

```
path 1 1 h264 6000 1280 720 1 1 1 1 1 0 30 1 <codec_tag> aac <name> 160 48000 1024 1024 2 "" ""
```

(`has_video`=1, `tracks`=1, then vcodec, bitrate, w, h, primaries, trc, colorspace, range, chroma location, max luminance, fps num, fps den,
codec tag, audio codec of track 0, then per track name, bitrate, rate, frame size, priming, channels, finally stream key and muxer settings.)

## 5. Behaviour (numbered rules)

### 5.1 Registration

* **S-046-R01** The ids, flags and display names are those of 3.1. Create of `ffmpeg_muxer`/mpegts/`replay_buffer` always succeeds; create of
  `ffmpeg_hls_muxer` returns NULL if its mutex, event or semaphore cannot be created (not observable in practice). The stream starts zeroed.
* **S-046-R02** Create adds the signals/procedures of 3.1 (declaration strings exactly as there; the host's declaration parser is S-003/S-004).
* **S-046-R03** `ffmpeg_muxer` (and mpegts) properties: one text property named `path`, description text `FilePath`, type "default text". Other outputs have no properties callback.
* **S-046-R04** `get_total_bytes` of every mux-based output returns the stream's `total_bytes`; `ffmpeg_output` returns its own `total_bytes`. HLS also reports `dropped_frames`.

### 5.2 ffmpeg_muxer / mpegts start and stop

* **S-046-R10** `start`: (1) `path` := setting `path`. (2) If `path` ends in exactly `.m3u8` (compare from the **last '.'** of the path string, case sensitive; no '.' = no match) the
  video encoder's settings get `repeat_headers = true` and the encoder is updated. This happens **before** the checks below and even if start then fails.
  (3) `can_begin_data_capture` false -> return false. (4) `initialize_encoders` false -> return false (nothing else changed). No error text is set for 3/4.
* **S-046-R11** File mode (output without SERVICE flag): `max_time = max_time_sec * 1_000_000`, `max_size = max_size_mb * 1_048_576`, `split_file`, `allow_overwrite` from settings;
  `cur_size = 0`; `sent_headers = false`.
* **S-046-R12** Network mode (SERVICE flag): no service -> return false (no message). Otherwise `path` := the service's server URL and `split_file := false`.
  `max_time`, `max_size`, `allow_overwrite`, `cur_size`, `sent_headers` are **not** touched; settings `split_file` etc. are ignored.
* **S-046-R13** Timestamp offsets are cleared (R58). File mode only: the path must be creatable: open `path` for writing (create/truncate) and, on success, close and **delete**
  it; on failure set last error to the module text `UnableToWritePath` with every `%1` replaced by the path (Windows 10+ additionally appends two newlines and the text `WarnWindowsDefender`
  before the replacement) and return false. (**Q-1**: an existing file at `path` is truncated and deleted by this probe.)
* **S-046-R14** The helper is started with the command line of R30-R36 and the path. If the pipe cannot be created: last error = module text `HelperProcessFailed`, warning
  `Failed to create process pipe`, return false. (`stream.path` has already been set to the path by the command-line builder.)
* **S-046-R15** On success: `active = capturing = true`, `stopping = false`, `total_bytes = 0`, `begin_data_capture`, info log `Writing file '<path>'...`, return true. No header or packet is
  written yet (headers are lazy, R40).
* **S-046-R16** `stop(ts)`: only if `capturing` **or** `ts == 0`: `stop_ts := ts / 1000`, `stopping := true`, `capturing := false`. Otherwise ignored. (Used by `ffmpeg_muxer`, mpegts, HLS and `replay_buffer`.)
* **S-046-R17** `encoded_packet(p)` on an inactive stream is ignored. `p == NULL` (encoder failure) on an active stream: `deactivate(ENCODE_ERROR)` (R19) and return; **`capturing` stays true** (**Q-7**).
* **S-046-R18** While `stopping`, a packet with `sys_dts_usec >= stop_ts` (checked **after** the lazy headers of R40 were sent, **Q-6**) calls `deactivate(0)` and the packet is dropped.
* **S-046-R19** `deactivate(code)` (mux-based outputs): HLS only: stop the worker (R104). If active: close the pipe (its return value is the helper exit status, kept for the caller),
  `active = false`, `sent_headers = false`, info log `Output of file '<printable_path if set else path>' stopped`. Then if `code != 0`: `signal_stop(code)`; else if `stopping`: `end_data_capture`.
  HLS only: discard every queued packet (under the write lock). Finally `stopping = false`. Returns the helper exit status, or -1 if it was not active.
* **S-046-R20** `destroy` of `ffmpeg_muxer`/mpegts/`replay_buffer` does **not** call `deactivate`: it clears the replay ring, joins a running save thread, releases parked split packets, closes the
  pipe (if any) without reading stderr, and frees the stream. No stop signal, no `end_data_capture` (**Q-8**).

### 5.3 Command line (value derivation; layout: S-037 section 4)

* **S-046-R30** `argv[1]` = `path`. `has_video` = 1 if the output has a video encoder else 0. `tracks` = number of audio encoders found by asking for track 0,1,2,... until the first missing one.
* **S-046-R31** Video block (only if a video encoder exists): codec name of the encoder; `bitrate` = encoder setting `bitrate` truncated to `int`; output width and height of the **output** object; then the colour tags
  from the video output's colour space:

  | OBS colour space | primaries | trc | colorspace |
  |---|---|---|---|
  | `601` | 6 | 6 | 6 |
  | default, `709` | 1 | 1 | 1 |
  | sRGB | 1 | 13 | 1 |
  | 2100 PQ | 9 | 16 | 9 |
  | 2100 HLG | 9 | 18 | 9 |
  | any other value | 2 | 2 | 2 |

  range = 2 if the video output range is full else 1. Chroma location from the **video output's** pixel format mapped by 5.10 table T1 and the colorspace tag just computed (T2): 4:4:4 -> 2, 4:2:2 -> 1,
  4:2:0 -> 3 if colorspace tag is 9 else 1, otherwise 0. Max luminance: HDR peak (truncated to int) for PQ, 1000 for HLG, 0 otherwise. Then `fps_num`, `fps_den` of the video output (not of the encoder).
  Codec tag: the encoder setting `codec_type` as `int`, then on little-endian machines its four bytes reversed (arithmetic on 32-bit **unsigned** values; the reference performs signed shifts, **D-4**). Example: sender value 875967080 -> 1748121140.
* **S-046-R32** Audio block (only if `tracks > 0`): codec name of **track 0's** encoder, then for every track `i`: encoder *name*, setting `bitrate` (int), encoder sample rate, encoder frame size, encoder priming samples, channel count of the **global** audio output (same value for every track).
* **S-046-R33** Then `stream_key` (the stream's key, or "") and muxer settings: the stream's `muxer_settings` if non-empty, else the output setting `muxer_settings`, else "" - always one argument. The string is parsed as `key=value` pairs separated by spaces; on success each pair
  is listed in one info log `Using muxer settings:` followed by `\n\t<key>=<value>` per pair (nothing logged if none); on parse failure a warning `Failed to parse muxer settings: <libav error>\n<settings>` and the argument is still passed unchanged.
* **S-046-R34** The helper executable name is `obs-ffmpeg-mux` (`.exe` on Windows) located relative to the OBS executable; it is `argv[0]`.
* **S-046-R35** The path is copied into `stream.path` before the process is started (also when the start then fails).
* **S-046-R36** All numbers are decimal `%d`. Unset encoder settings read as 0.

### 5.4 Writing packets and headers

* **S-046-R40** The first packet of a session (`sent_headers == false`) is preceded by `send_headers` (R41); if that fails the packet is dropped and the next packet retries (`sent_headers` still false; headers already written are written again). After success
  `sent_headers = true` and, in split mode, `cur_time := packet.dts_usec`.
* **S-046-R41** `send_headers`: the video encoder's extra data as a VIDEO header packet (index 0, `pts = dts = 0`, `keyframe = 0`), then each audio encoder's extra data (AUDIO, index = track), tracks 0.. until the first missing one. If any encoder has no extra data,
  `send_headers` returns false at that point (video header alone may already be on the wire). A header is written through `write_packet` (R42), so in split mode its pts/dts have the current offsets subtracted (R59, **Q-2**) and its size counts (R43).
* **S-046-R42** `write_packet`: builds `ffm_packet_info {pts, dts, size, index = track_idx, type, keyframe}`; in split mode subtracts `video_pts_offset` (video) or `audio_dts_offsets[index]` (audio) from **both** pts and dts; writes the 32-byte info with one pipe write, then the
  payload with one pipe write (a zero-size payload is still "written"). A short write on either: warning (`os_process_pipe_write for info structure failed` / `... for packet data failed`), failure handling R44, return false.
* **S-046-R43** After a successful write `total_bytes += size` (payload only, headers included); in split mode `cur_size += size` too (headers included, **Q-5**).
* **S-046-R44** Failure handling (`signal_failure`): read up to 1023 bytes of the helper's stderr; if non-empty: warning `ffmpeg-mux: <text>` and set it as last error. `deactivate(0)` (R19). Stop code: helper exit status `-2` (`FFM_UNSUPPORTED`) -> `UNSUPPORTED`; otherwise `DISCONNECTED` for network
  outputs, `ENCODE_ERROR` for files. Signal it, then `capturing = false`. (For the replay buffer this runs on the save thread: R96.)
* **S-046-R45** In non-split mode timestamps are passed through unchanged.

### 5.5 Stop timestamp ordering

* **S-046-R46** Order inside `encoded_packet` (mux/split): inactive -> NULL -> split handling (R51-R57) -> lazy headers (R40) -> stop gate (R18) -> flush parked packets (R56) -> offset update + write.

### 5.6 Split-file recording

* **S-046-R50** Split mode is `split_file` setting true in file mode. The procedure `split_file` always outputs `split_file_enabled = stream.split_file` (as it is **at call time**: false before the first start); if enabled it also sets `manual_split = true` (atomic).
* **S-046-R51** `should_split(p)` (evaluated for every packet when no packet is parked): false unless `p` is VIDEO **and** keyframe; then true if `manual_split`; or `max_size > 0 && cur_size + p.size >= max_size`; or `max_time > 0 && p.dts_usec - cur_time >= max_time`.
  `cur_time` is only set after the first packet passed this test (**Q-4**, vector `16-split-first-packet-quirk`).
* **S-046-R52** Without audio encoders (track 0 missing) a split packet immediately runs `prepare_split_file(p)` and then continues down the normal path (R46) in the same call; `split_file_ready = true`.
* **S-046-R53** With audio the splitting video keyframe is **parked** (`mux_packets` gets a reference to it) and the call returns. While `mux_packets` is non-empty, every packet is compared with the first parked packet `F` by `pts * 1_000_000 / timebase_den`:
  `pts_usec < F` -> processed normally (written to the old file); `pts_usec >= F` and not AUDIO (video **or** anything else non-audio) -> parked, return; `pts_usec >= F` and AUDIO -> `prepare_split_file(F)` (its `dts_usec` becomes `cur_time`), `split_file_ready = true`, continue with this audio packet.
* **S-046-R54** `prepare_split_file(p)`: generate a new name (R70, overwrite per `allow_overwrite`) into `stream.path`; info log `Changing output file to '<path>'`; send a CHANGE_FILE message (S-037 R37 layout: info with type CHANGE_FILE and `size = strlen(name)`, then the name bytes without terminator; write failures as R42/R44, return false);
  signal `file_changed` with string parameter `next_file` = the new path; `send_headers` (false -> return false); then `cur_size = 0`, `cur_time = p.dts_usec`, offsets cleared (R58). If it returns false the current packet is dropped and parked packets stay parked.
* **S-046-R55** `sent_headers` is not reset by a split. If the very first packet triggers a split (manual request before any data, or R51's first-packet case) the order on the wire is: CHANGE_FILE, headers, then - because `sent_headers` is still false - **headers again**, then the packet (**Q-3**).
* **S-046-R56** When `split_file_ready`: after the lazy-header and stop steps, every parked packet is passed to the offset update (R58) and `write_packet` in parking order and released, the parking list is emptied, `split_file_ready = false` and `manual_split = false`.
* **S-046-R57** `manual_split` is cleared **only** by R56; a manual request therefore stays armed until a video keyframe splits.
* **S-046-R58** Offset update (`ts_offset_update`, applied to every packet written in split mode, parked ones before the current): VIDEO: the first video packet after a clear sets `video_pts_offset = pts` (its **pts**, in the packet's own time base) and `found_video`; AUDIO: the first packet of
  each track sets `audio_dts_offsets[track] = dts`. Clear (`ts_offset_clear`): `found_* = false`, offsets 0. The offset is taken **before** the packet is written, so the first packet of each stream in a file gets pts = dts-based 0 (video: pts 0 and dts `dts - pts`).
* **S-046-R59** Header packets use whatever offsets are current when they are sent: for a new file `prepare_split_file` sends headers **before** clearing the offsets, so their pts/dts equal `-old_offset` (helpers ignore header timestamps, S-037 R20-R22).
* **S-046-R60** Non-split mode never touches offsets, `cur_size`, `cur_time`.

### 5.7 File name generation (`generate_filename`, used by split and by the replay buffer save)

* **S-046-R70** Inputs: settings `directory`, `format`, `extension`, `allow_spaces`. `name := os_generate_formatted_filename(extension, allow_spaces, format)` (S-023: strftime-like tokens, `%FPS`, `%CRES`, ... - not repeated here). Result path: `directory` with every `\` replaced by `/`,
  a trailing `/` appended if the last character is not `/` (an empty directory therefore yields `/`), then `name`.
* **S-046-R71** The directory part (everything before the **last** `/` of the final path, so a `format` with slashes adds sub-directories) is created recursively (`mkdirs`) every time, before the existence check.
* **S-046-R72** Unless overwriting is allowed (split: `allow_overwrite`; replay buffer: **always allowed**): if the path exists, find the **last '.'** in the *whole path string* (not only the file name); none -> keep the path unchanged (**Q-9**: a colliding extension-less name is reused). Otherwise
  try `<stem> (N)<ext>` (`allow_spaces`) or `<stem>_N<ext>` (not) for N = 2, 3, ... and take the first that does not exist, where `stem`/`ext` split at that dot. (With a dot in a directory name and an extension-less file the result is e.g. `/v_2.dir/name`, **Q-9**.)

### 5.8 replay_buffer

* **S-046-R80** `start`: `can_begin_data_capture`/`initialize_encoders` as R10 (3, 4). `max_time = max_time_sec * 1_000_000`, `max_size = max_size_mb * 1_048_576` from the output settings, `active = capturing = true`, `total_bytes = 0`, `begin_data_capture`, return true. No pipe, no path check, no `.m3u8` handling.
* **S-046-R81** `stop(ts)` = R16. `encoded_packet` on an inactive stream is ignored. NULL packet: `deactivate_replay_buffer(ENCODE_ERROR)`: signal stop, then `active = sent_headers = stopping = false`, ring cleared (R89). While `stopping`, a packet with `sys_dts_usec >= stop_ts`: `deactivate_replay_buffer(0)` (`end_data_capture` first, because `stopping`) and the packet is dropped.
* **S-046-R82** Limits of 0 mean "no limit" in R84/R85.
* **S-046-R83** Per packet: take a reference to the packet; purge (R84); if the queue is empty `cur_time := packet.dts_usec`; `cur_size += packet.size`; append the packet; if VIDEO keyframe `keyframes += 1`; then the save check (R90).
* **S-046-R84** `replay_buffer_purge(p)`: if `max_size != 0`: return at once if the queue is empty or `keyframes <= 2`; else `while cur_size + p.size > max_size: purge()`. Then (time, also when `max_size == 0`): return if the queue is empty or `keyframes <= 2`; else `while p.dts_usec - cur_time > max_time: purge()`.
  Both loops re-test only their own condition: once purging has emptied the ring (`cur_time` is then 0) they never terminate (**D-1**). With both limits 0 (defaults not applied) and more than two queued keyframes the time loop condition is `dts_usec - cur_time > 0` and the call hangs (probe 1 of `28-replay-purge-hang`).
* **S-046-R85** `purge()` = `purge_front()`; if it returned true (the removed packet was a VIDEO keyframe) keep removing packets from the front while the new front is not a VIDEO keyframe (stop when empty). `purge_front`: remove the oldest packet (release it); if it was a VIDEO keyframe `keyframes -= 1`; if the queue is now empty `cur_size = cur_time = 0`,
  else `cur_time = new_front.dts_usec` and `cur_size -= removed.size`. Returns whether a video keyframe was removed. (Consequence: a non-keyframe at the front is removed alone and `purge` returns without skipping to the next group.)
* **S-046-R86** (removed; numbering kept)
* **S-046-R88** `save` procedure (`save_replay_proc`): if the stream is active: if the video encoder is paused -> info log `Could not save buffer because the encoder is paused`, return (no change); else `save_ts := os_gettime_ns() / 1000`. Inactive -> no effect.
* **S-046-R89** `get_last_replay`: outputs string `path` = `stream.path` only while `muxing` is false (the string may be NULL before the first save); while a save runs the output parameter is not set.
* **S-046-R90** Save trigger (end of R83): `save_ts != 0 && packet.sys_dts_usec >= save_ts` (the packet that triggers is already in the ring): if `muxing` -> return (`save_ts` stays armed); else join a previous save thread if any, `save_ts = 0`, `replay_buffer_save`.
* **S-046-R91** `replay_buffer_save`: for each queued packet in arrival order compute rebasing offsets lazily: first VIDEO packet: `video_pts_offset = pts`, `video_offset_usec = pts * 1_000_000 / timebase_den`; first packet of an audio track: `audio_offsets_usec[track] = dts_usec`, `audio_dts_offsets[track] = dts`.
  Insert a reference to a copy rebased by those offsets (video: `dts_usec -= video_offset_usec`, `pts`,`dts -= video_pts_offset`; audio: `dts_usec -= audio_offsets_usec[track]`, `pts`,`dts -= audio_dts_offsets[track]`) into `mux_packets` ordered by the rebased `dts_usec`: scan from the back and insert after the last element whose `dts_usec` is
  **strictly smaller** (so ties are placed before existing equal elements, **Q-10**). The queue itself is unchanged.
* **S-046-R92** After the list is built: signal `saving` with pointer parameter `output`; `generate_filename(overwrite = true)` into `stream.path` (R70-R72); `muxing = true`; start the save thread. If the thread cannot be created: warning `Failed to create muxer thread`, `muxing = false`.
* **S-046-R93** Save thread: start the helper with `stream.path` (R30-R36 with this path; failure: warning `Failed to create process pipe`, error path); `send_headers` (failure: warning `Could not write headers for file '<path>'`, error); every parked packet in order through `write_packet` (offsets in split mode do not apply; the replay buffer never sets `split_file`) and released
  (failure: warning `Could not write packet for file '<path>'`, error); success: info log `Wrote replay buffer to '<path>'`.
* **S-046-R94** Save thread exit: close the helper pipe; release all remaining parked packets **if there was an error**; empty the parking list; `muxing = false`; on success signal `saved` (no parameters).
* **S-046-R95** `muxing` serialises saves; ring maintenance continues while a save runs.
* **S-046-R96** A pipe write failure inside the save thread runs R44 on the save thread: it closes the pipe, **deactivates the stream** (`active = false`, queue not cleared, **Q-11**), signals `ENCODE_ERROR` (`DISCONNECTED` is impossible: replay buffer is never a network output) and the save thread then reports the error path of R93/R94.
  The ring stays allocated but later packets are ignored (inactive).
* **S-046-R97** `destroy` = R20. `replay_buffer_clear` (used by deactivate and destroy): release every queued packet, free the queue, `cur_size = cur_time = max_size = max_time = save_ts = keyframes = 0` (note: the limits are cleared too; the next `start` reads them again).

### 5.9 HLS (`ffmpeg_hls_muxer`)

* **S-046-R100** `start`: `can_begin_data_capture`/`initialize_encoders` as R10; no service -> false. `path` := service server URL with every `{stream_key}` replaced by the service stream key (a NULL key MUST be treated as empty); `stream.stream_key := key`.
* **S-046-R101** Muxer settings (stream-level, they win over the output setting, R33): the fixed string `method=PUT http_persistent=1 ignore_io_errors=1 ` followed by `http_user_agent=libobs/<version string>`; if the video encoder's setting `keyint_sec` (int) is non-zero append ` hls_time=<keyint_sec>` and set `stream.keyint_sec`. With 0 the previous `keyint_sec`
  (from an earlier session) is **kept** (**Q-12**). The settings string is rebuilt on every start.
* **S-046-R102** The helper is started with `path` (R30-R36). Failure: last error `HelperProcessFailed`, warning, return false. Then the worker thread is created; failure -> return false (the pipe stays open). Success: `active = capturing = true`, `is_hls = true`, `total_bytes = 0`, `dropped_frames = 0`, `min_priority = 0`, `begin_data_capture`;
  `printable_path := service URL` (**unreplaced**, i.e. with the `{stream_key}` placeholder) and info log `Writing to path '<printable_path>'...`. `last_dts_usec` is not reset (**Q-12**).
* **S-046-R103** `encoded_packet`: inactive -> ignore; NULL -> `deactivate(ENCODE_ERROR)` (**Q-7**); lazy headers written on the *calling* thread (R40, no split offsets); stop gate (R18); VIDEO packets of codec `h264` get `drop_priority := max(packet.priority, max over NAL units of (nal_header_byte >> 5))` (S-012;
  `hevc` uses the HEVC parser when the build has HEVC enabled; other codecs keep the incoming `drop_priority`). Take a reference; under the write lock, if still active: VIDEO -> R105, others (audio) -> append to the queue unconditionally. If a packet was appended post the semaphore; else release the reference.
* **S-046-R104** Worker: loop { wait on the semaphore; if the stop event is set -> return silently; pop the oldest queued packet under the lock (if any); write it via `write_packet` (R42); release }. A failed write (R44 already ran) leaves the loop: signal stop `ERROR`, `deactivate(0)`, return. Because R44 already signalled `ENCODE_ERROR`, **two** stop signals are produced
  (**Q-14**). `deactivate` (R19) for HLS first, if the worker is joinable: set the stop event, post the semaphore, join the worker. When run **on the worker itself** the join is a self-join (error `EDEADLK`, ignored); a conforming implementation MUST NOT join the current thread (**D-3**).
* **S-046-R105** `add_video_packet(p)`: `check_to_drop_frames(false)`, `check_to_drop_frames(true)`; if `p.drop_priority < min_priority`: `dropped_frames += 1`, return false (not queued); else `min_priority = 0`; `last_dts_usec = p.dts_usec`; append; return true.
* **S-046-R106** `check_to_drop_frames(pframes)`: priority limit `L = 3` if `pframes` else `2`; threshold `T = 2 * keyint_sec` seconds if `keyint_sec != 0` else 10; find the **first queued VIDEO packet that is not a keyframe** (by the `keyframe` flag; the reference calls this "first video packet"); none -> return;
  if `last_dts_usec - first.dts_usec > T * 1_000_000` -> `drop_frames(L)`.
* **S-046-R107** `drop_frames(L)`: rebuild the queue keeping every AUDIO packet and every packet with `drop_priority >= L`; release and count the others; `min_priority = max(min_priority, L)`; `dropped_frames += count`. The semaphore count is **not** reduced (**Q-13**: the worker later wakes with nothing to pop and continues).
* **S-046-R108** `get_dropped_frames` returns `dropped_frames`.
* **S-046-R109** `destroy`: `deactivate(0)`, destroy mutex/semaphore/event, free queues and strings. Packets queued for the worker when `deactivate` runs are discarded, not written.

### 5.10 ffmpeg_output: configuration (`try_connect`)

* **S-046-R120** `start`: if a connect is already running (`connecting`) -> false. Else `stopping = false`, `audio_start_ts = video_start_ts = total_bytes = 0`, start the connect thread; returns whether the thread was created and sets `connecting` to that result **after** the thread was created
  (**Q-20**: a connect that finishes before that assignment leaves `connecting == true`).
* **S-046-R121** The connect thread calls `try_connect`; if it returns false: stop signal `CONNECT_FAILED`. Then `connecting = false`.
* **S-046-R122** `try_connect` builds `ffmpeg_cfg` from the settings after writing the *default* `gop_size = 120` into the live settings: `url`, `muxer_settings`, `video_settings`, `audio_settings` verbatim (empty string stays an empty string); `format_name`, `format_mime_type`, `video_encoder`, `audio_encoder` -> **NULL if empty**;
  `video_bitrate`, `audio_bitrate` (kbit/s), `gop_size`, `video_encoder_id`, `audio_encoder_id`, `scale_width`, `scale_height` as `int`; `width`, `height` = output size; `format` = T1(pixel format of the output's video); `audio_tracks` = the output's mixer bit mask; `audio_mix_count` = popcount of the mask over mixes 0..5 (`MAX_AUDIO_MIXES` = 6).
* **S-046-R123** Colour: `color_range` = JPEG(2) for full range else MPEG(1). Initial `colorspace` = BT709(1) if the output's video format is YUV (`format_is_yuv`) else RGB(0); then the table of R31 (without the "any other value" row: **Q-21**, primaries/trc stay uninitialised for an unknown colour space) overwrites primaries, trc and colorspace.
* **S-046-R124** T1 (OBS video format -> libav pixel format): `I444`->yuv444p, `I412`->yuv444p12le, `I420`->yuv420p, `NV12`->nv12, `YUY2`->yuyv422, `UYVY`->uyvy422, `YVYU`->yvyu422, `RGBA`->rgba, `BGRA`->bgra, `BGRX`->bgra, `Y800`->gray, `BGR3`->bgr24, `I422`->yuv422p, `I210`->yuv422p10le, `I40A`->yuva420p,
  `I42A`->yuva422p, `YUVA`->yuva444p, `YA2L`->yuva444p12le, `I010`->yuv420p10le, `P010`->p010le, `P216`->p216le, `P416`->p416le; `NONE`, `AYUV` and unknown -> none. If the result is none: debug log `invalid pixel format used for FFmpeg output` and `try_connect` returns false (no init attempt, no last error).
  T2 (`determine_chroma_location(pix_fmt, colorspace)`): by the pixel format descriptor's chroma subsampling shifts: (h0,w0) -> center(2); (h0,w1) -> left(1); (h1,w1) -> top-left(3) if `colorspace == BT2020_NCL` else left(1); anything else, or no descriptor -> unspecified(0).
  T3 (libav sample format -> OBS audio format): u8->U8, s16->16bit, s32->32bit, flt->float, u8p/s16p/s32p/fltp -> the planar variants, **anything else -> 16bit**.
* **S-046-R125** `scale_width`/`scale_height` 0 -> output width/height. Audio stream names: if `audio_names` exists, for each mix bit `i` set in the mask (ascending) the next name slot gets `item[i]`'s string `name` where `i` is the **mix index, not the position** (**Q-15**); a missing item or missing key yields an empty string (not NULL). If the array does not exist all names are NULL.
* **S-046-R126** `ffmpeg_data_init(cfg)` failure (R140-R149): last error text of the ff data (if any) becomes the output's last error, `ffmpeg_data_free`, return false (-> `CONNECT_FAILED`).
* **S-046-R127** Success: `audio conversion = {format = ff_data.audio_format}` (other fields 0 = keep), **video conversion = none**; `active = true` (set **before** the next check); `can_begin_data_capture` false -> return false **leaving `active == true` and the libav context open** (**Q-16**; cleaned up by stop/destroy);
  create the write thread (failure: log `ffmpeg_output_start: failed to create write thread.`, full stop, false); `begin_data_capture`; `write_thread_active = true`; true.

### 5.11 ffmpeg_output: initialisation (`ffmpeg_data_init`)

* **S-046-R140** An empty or NULL `url` fails immediately with no error text and no log. Otherwise network init, then `is_rtmp = (url starts with "rtmp://", ASCII case-insensitive)`.
* **S-046-R141** Output format: libav `guess_format(name, url, mime)` with `name = "flv"`, `mime = NULL` if `is_rtmp`, else `format_name`/`format_mime_type`. None -> warning and last error `Couldn't find matching output format with parameters: name=%s, url=%s, mime=%s` (NULL pointers printed as `(NULL)`; when `is_rtmp` name is `flv` and mime `(NULL)`), then warning `ffmpeg_data_init failed`, false.
  Context creation failure -> `Couldn't create avformat context`. If `is_rtmp`: `audio_encoder_id := AAC`, `video_encoder_id := H264` (these two config fields are **never used for encoder lookup**, **Q-17**).
* **S-046-R142** Streams: the muxer's default video codec ≠ NONE -> video stream (R143); default audio codec ≠ NONE **and** `num_audio_streams > 0` (`audio_mix_count`) -> that many audio streams (R144), stopping at the first failure.
* **S-046-R143** Video stream: needs active video info (else `No active video`). Encoder = by name if `video_encoder` non-empty else the muxer's default video codec (error `Couldn't find encoder '<codec name>'`, `Couldn't create stream for encoder '...'`); stream id = stream count - 1. Codec context: `bit_rate = video_bitrate * 1000`, width/height = scale size, `time_base = {fps_den, fps_num}`,
  `framerate = {fps_num, fps_den}`, `gop_size`, colour range/primaries/trc/space from the config, threads 0 (auto); pixel format: the encoder's first supported-format list; if the list exists the best match for the input format (alpha-aware only for BGRA input), else the input format itself; chroma sample location = T2(chosen format, colorspace);
  stream time base = context time base, `avg_frame_rate = {fps_num, fps_den}`; `GLOBAL_HEADER` flag if the muxer wants it. Then open (R145). For PQ/HLG additionally attach content-light-level side data (`MaxCLL = MaxFALL = peak`, peak = HDR peak for PQ, 1000 for HLG) and mastering-display data:
  primaries R (17/25, 8/25), G (53/200, 69/100), B (3/20, 3/50), white point (3127/10000, 329/1000), min luminance 0, max luminance peak. A scaler (bicubic) is created iff the chosen pixel format differs from the input format or the scale size differs from the output size (failure: `Could not initialize swscale`).
* **S-046-R144** Audio stream `i`: needs active audio info (`No active audio`); encoder by name or muxer default audio codec; `bit_rate = audio_bitrate * 1000`, `time_base = {1, sample_rate}`, channels from the speaker layout (default layout for the channel count; 4.1 is the explicit 4.1 layout), `sample_fmt` = first supported or FLTP;
  records `audio_samplerate`, `audio_format = T3(sample_fmt)`, planes, bytes per frame (`audio_size`); `GLOBAL_HEADER` if wanted; stream metadata `title` = the configured name if non-empty. Open (R146).
* **S-046-R145** Opening video: for encoder `libx264` first set `preset=veryfast`; split `video_settings` at single spaces, each token containing `=` is set as an encoder option (search children); an option failure logs `Failed to set <name>=<value>` (and for libx264 a hint about `x264-params`) but does **not** fail. Encoder open failure: `Failed to open video codec: <libav error>`. The frame buffer
  (format/size/colour, chroma location T2 for the config colorspace) is allocated with `base_get_alignment()`; failures `Failed to allocate video frame` / `Failed to allocate vframe: <err>`. Stream parameters are copied from the context.
* **S-046-R146** Opening audio: `audio_settings` options as above (failures ignored, no hint); frame struct with the context's format/layout/rate; `strict_std_compliance = -2` (experimental codecs allowed); open (`Failed to open audio codec: <err>`); `frame_size := codec frame_size or 1024 if 0` (**one shared field, written by every stream, the last wins**); sample buffer for one frame
  (`Failed to create audio buffer: <err>`); parameters copied.
* **S-046-R147** Output open: parse `muxer_settings` (`key=value` separated by spaces; failure: last error `Failed to parse muxer settings: <err>\n<settings>`, false); log `Using muxer settings: ...` (as R33 but a single log line with the same format); unless the muxer has no file I/O open `url` for writing (`Couldn't open '<url>', <err>`); write the header with the
  options (`Error opening '<url>': <err>`); options the muxer did not consume are logged as `Invalid muxer settings: ...`. Success: stream dump at debug level, `initialized = true`.
* **S-046-R148** `ffmpeg_data_free`: write the trailer if initialised; free video (the frame is *not* freed for the two A64 multi codecs), audio buffers/contexts/frames, close the file unless no-file muxer, free the context, free last error, zero the structure.

### 5.12 ffmpeg_output: data flow

* **S-046-R160** `raw_video(frame)`: ignored if there is no video stream. `video_start_ts` and `start_timestamp` take the frame timestamp if they are still 0 (**Q-18**: a frame with timestamp 0 does not set them). Make the frame writable (failure: warning, return; the frame counter is **not** advanced). Convert via the scaler if one exists (full height of the *config* height)
  else copy plane by plane (`min(src linesize, dst linesize)` bytes per row, chroma plane heights shifted by the format's vertical subsampling). `frame.pts = total_frames` (in the codec time base). Send to the encoder, receive at most one packet. EAGAIN/EOF count as "no packet". Other errors: warning `receive_video: Error encoding video: <err>`, return (counter not advanced).
  A packet with `size > 0`: `pts`, `dts` rescaled from the codec time base to the **stream** time base with nearest rounding, ties away from zero, and min/max values passed through unchanged; `duration` rescaled with default rounding and truncated to `int`; appended to the packet list under the lock; post the semaphore. Otherwise nothing is queued. `total_frames += 1` for every frame that reached the encoder without error.
* **S-046-R161** `raw_audio2(mix, data)`: ignored if there are no audio streams; ignored if `audio_tracks` bit `mix` is clear; `track = number of set bits below mix` (`get_track_order`); ignored if a video stream exists and no video frame has been seen (`start_timestamp == 0`); `audio_start_ts` takes the frame timestamp if still 0. For every plane append `frames * audio_size` bytes to the track's per-plane FIFO;
  while plane 0 holds at least `frame_size * audio_size` bytes pop one codec frame from every plane and encode it.
* **S-046-R162** `encode_audio`: `nb_samples = frame_size`; `pts = rescale(total_samples[track], {1, sample_rate} -> codec time base)`; fill the frame from the sample buffer (failure: warning, return); `total_samples[track] += frame_size` (before sending, so a failed send still advances it); send and receive one packet; no packet or error -> nothing queued (errors logged `encode_audio: Error encoding audio: <err>`).
  A packet: pts/dts/duration rescaled as in R160 to the track's stream time base; `stream_index` = the track's stream index; queued and the semaphore posted.
* **S-046-R163** Packet system time (`get_packet_sys_dts`): `start_ts + pause_offset + dts * 1e9 * time_base` where, for a packet of the video stream, `start_ts = video_start_ts` and the time base is the video stream's, and for **every other packet** `start_ts = audio_start_ts` and the time base is that of audio stream 0; `pause_offset` from the output (S-041); rescale rounding to nearest.
* **S-046-R164** `process_packet` (worker): pop the oldest packet (none -> 0). If `stopping` and the packet's system time `>= stop_ts` the packet is discarded and 0 returned (no accounting). Otherwise `total_bytes += size` **before** writing, write interleaved; a negative result: warning `process_packet: Error writing packet: <libav error>` and last error set; return the result.
* **S-046-R165** Worker loop: wait on the semaphore; stop event set -> exit loop; `process_packet` != 0 -> detach the thread, `write_thread_active = false`, stop code `NO_SPACE` if the result was `-ENOSPC` else `ERROR`, signal, deactivate; leave the loop. At the end of the thread `active = false`.
* **S-046-R166** `stop(ts)`: if `active`: if `ts > 0` -> `stop_ts = ts`, `stopping = true`; then full stop (`end_data_capture`, deactivate) **immediately** - queued packets are discarded; `stopping` remains true until the next `start` (**Q-19**). Inactive -> ignored. `ffmpeg_deactivate`: stop and join the worker if running; free all queued packets;
  `ffmpeg_data_free`.
* **S-046-R167** `destroy`: join the connect thread if `connecting`; full stop (if active); destroy the primitives; free.
* **S-046-R168** `get_track_order(mask, mix)` = count of set bits of `mask` at positions `< mix`. `get_audio_mix_count(mask)` = count of set bits at positions 0..5.

## 6. Algorithms

```
// split decision for one packet p (mux with split_file)
on_packet(p):
  if !active: return
  if p == NULL: deactivate(ENCODE_ERROR); return
  if mux_packets non-empty:
      F = mux_packets[0]
      if usec(p.pts) >= usec(F.pts):
          if p.type != AUDIO: park(p); return
          if !prepare_split_file(F): return
          split_file_ready = true
      // else fall through: p belongs to the old file
  else if should_split(p):
      if track0 exists: park(p); return
      if !prepare_split_file(p): return
      split_file_ready = true
  if !sent_headers:
      if !send_headers(): return
      sent_headers = true; cur_time = p.dts_usec
  if stopping and p.sys_dts_usec >= stop_ts: deactivate(0); return
  if split_file_ready: for q in mux_packets: offset_update(q); write(q); mux_packets = []; ready = manual = false
  offset_update(p); write(p)

// replay save ordering (R91)
for p in ring (arrival order):
    note offsets from first video / first packet of each audio track
    r = rebase(p)
    i = len(out); while i > 0 and out[i-1].dts_usec >= r.dts_usec: i -= 1   // ties go before existing equals
    out.insert(i, r)
```

## 7. Concurrency

* Mux/replay: the packet callback runs on the output's encoder-delivery thread; `start`, `stop`, the procedures run on others. `active`, `capturing`, `stopping`, `muxing`, `manual_split`, `sent_headers` are atomic booleans; other fields are owned by the delivery thread (replay: also read by the save thread through `mux_packets`, which the delivery thread does not touch while `muxing`).
* The replay save thread is joined by the next save or by destroy; `muxing` is cleared by the save thread just before it signals `saved`.
* HLS: the delivery thread appends/drops under `write_mutex`; the worker pops under the same mutex; pipe writes (headers on the delivery thread, packets on the worker) are not serialised against each other. `deactivate` from the delivery thread sets the stop event, posts the semaphore and joins the worker.
* ffmpeg_output: `try_connect` runs on its own thread; video/audio callbacks and the worker share `packets` through `write_mutex`; the worker may run `deactivate` on itself (it detaches first, R165).

## 8. Reference defects and quirks

| ID | Kind | What the reference does | Required behaviour | Script |
|---|---|---|---|---|
| Q-1 | QUIRK | the writability probe of file start truncates and **deletes** an existing file at `path` | SHOULD match (callers may rely on the file being absent afterwards); never delete in network mode | `04-start-errors` (`unlink` lines) |
| Q-2 | QUIRK | in a split, header packets are sent with the old offsets: pts/dts of `-old_offset` | SHOULD match; helpers ignore header timestamps | `12-split-no-audio` |
| Q-3 | QUIRK | split triggered by the first packet: CHANGE_FILE first, headers sent twice | SHOULD match | `15-split-manual`, `16-split-first-packet-quirk` |
| Q-4 | QUIRK | `cur_time` is initialised after the first split test, a first keyframe with dts >= max_time splits immediately | SHOULD match | `16-split-first-packet-quirk` |
| Q-5 | QUIRK | header payloads count into `cur_size`/`total_bytes` (the new file's headers do not count towards `cur_size`: it is reset after them) | MUST match for `cur_size` | `13-split-size`, `07-headers-and-packets` |
| Q-6 | QUIRK | the packet that ends a recording still triggers the header phase first | SHOULD match | `10-stop-immediate-and-failure` |
| Q-7 | QUIRK | an encoder failure (NULL packet) leaves `capturing` true | SHOULD match | `10-stop-immediate-and-failure`, `32-hls-stop-restart` |
| Q-8 | QUIRK | destroy of an active mux output closes the pipe but signals nothing | SHOULD match | `10-stop-immediate-and-failure` |
| Q-9 | QUIRK | collision handling splits at the last `.` of the whole path; extension-less colliding names are reused | SHOULD match | `17-filenames` |
| Q-10 | QUIRK | replay save orders ties in reverse arrival order | MUST match | `24-replay-sort-ties`, `23-replay-save` |
| Q-11 | QUIRK | a write failure while saving deactivates the replay buffer (`active=false`, ring kept) | SHOULD match | `26-replay-save-failures` |
| Q-12 | QUIRK | HLS: `keyint_sec` and `last_dts_usec` survive a restart; "first video packet" for the drop test is the first non-keyframe video | MUST match | `32-hls-stop-restart`, `31-hls-drop-frames` |
| Q-13 | QUIRK | HLS drops leave surplus semaphore counts (worker wakes with an empty queue) | observable only as extra wake-ups | `31-hls-drop-frames` |
| Q-14 | QUIRK | HLS worker write failure signals ENCODE_ERROR (from R44) and then ERROR | SHOULD match | `33-hls-write-failure` |
| Q-15 | QUIRK | `audio_names[]` indexed by mix index, not by track position | MUST match | `40-out-config-mapping` |
| Q-16 | QUIRK | `ffmpeg_output`: refused capture leaves `active` true and the context open after CONNECT_FAILED | SHOULD match | `45-out-errors` |
| Q-17 | QUIRK | `video_encoder_id`/`audio_encoder_id` are never used for lookup | MUST match | `40-out-config-mapping` |
| Q-18 | QUIRK | start timestamps of 0 are retaken by the next frame | MUST match | `44-out-video-audio-flow` |
| Q-19 | QUIRK | `ffmpeg_output` stop with a timestamp stops immediately; the sys-time gate is effectively dead except for a race | SHOULD match | `46-out-stop-gating` |
| Q-20 | QUIRK | `connecting` assigned after thread creation (race) | the Rust version MUST NOT reproduce the race: set before spawning | `43-out-lifecycle` |
| Q-21 | QUIRK | unknown colour space leaves primaries/trc uninitialised | use "unspecified" (2) | not testable |
| D-1 | DEFECT | the purge loops spin forever once the ring has been emptied (a time jump larger than the limit with > 2 keyframes queued, one packet larger than the size limit, or limits left at 0 with > 2 keyframes) | MUST terminate: stop purging when empty, treat a limit of 0 as unlimited, never purge the newest keyframe group | `28-replay-purge-hang` (probe = forked child, 2 s alarm) |
| D-2 | DEFECT | when a save write fails after k packets, those k packets are released twice | release each reference exactly once | `27-replay-save-double-release` (`refs`) |
| D-3 | DEFECT | HLS worker joins itself when deactivating after a write failure | do not join the current thread | `33-hls-write-failure` |
| D-4 | DEFECT | signed left shift overflow in the codec tag byte swap | use unsigned 32-bit swap; result as `int` | UBSan note |
| D-5 | DEFECT | `try_connect` returns without releasing the settings reference when the pixel format is unsupported | release exactly once | `41-out-config-colour` |

## 9. Implementation notes (non-normative)

* Model the sender as a state machine owning a `Box<dyn HelperPipe>`; the pipe, clock, file system probe, name generator and thread spawning should be traits so that the fakes of the conformance runner can be reproduced.
* Keep the three buffers distinct: the ring (`VecDeque<Packet>`), the parking list (`Vec<Packet>` used by split and by the replay save) and the HLS queue. Share `Arc` payloads instead of reference counts and test R91 with the `refs` counter of the runner as a leak check.
* The command line can be produced as `Vec<String>` and compared against S-037 R50 tables.
* `ffmpeg_output` is the only place needing libav (`ffmpeg-next` or direct FFI). The rescale rounding is `AV_ROUND_NEAR_INF | AV_ROUND_PASS_MINMAX`.
* HLS: use a bounded channel + explicit drop pass; the "semaphore without items" quirk is not required.

## 10. Conformance coverage

Vectors are text transcripts produced by the C reference through `gen/ffoutputs_runner.c` (see the README section); the comparison is exact (no floating point is printed).

| Rules | Scripts |
|---|---|
| R01-R04, 3.1, 3.2 | `01-info-defaults`, `20-replay-start`, `43-out-lifecycle` |
| R10-R13 | `04-start-errors`, `05-network-start`, `06-m3u8-repeat-headers`, `03-cmdline-tracks` |
| R14, R15, R35 | `04-start-errors`, `07-headers-and-packets` |
| R16-R20 | `09-stop-semantics`, `10-stop-immediate-and-failure` |
| R30-R36 | `02-cmdline-colour`, `03-cmdline-tracks`, `30-hls-start` |
| R40-R46 | `07-headers-and-packets`, `08-header-failure`, `09-stop-semantics`, `10-stop-immediate-and-failure` |
| R42-R44 | `11-write-failure-mapping`, `33-hls-write-failure`, `26-replay-save-failures` |
| R50-R60 | `12-split-no-audio`, `13-split-size`, `14-split-with-audio`, `15-split-manual`, `16-split-first-packet-quirk` |
| R70-R72 | `17-filenames` (plus split scripts for the name use) |
| R80-R85 | `20-replay-start`, `21-replay-purge-time`, `22-replay-purge-size`, `28-replay-purge-hang` |
| R88-R97 | `23-replay-save`, `24-replay-sort-ties`, `25-replay-save-busy`, `26-replay-save-failures`, `27-replay-save-double-release` |
| R100-R109 | `30-hls-start`, `31-hls-drop-frames`, `32-hls-stop-restart`, `33-hls-write-failure` |
| R120-R127 | `40-out-config-mapping`, `41-out-config-colour`, `43-out-lifecycle`, `45-out-errors`, `42-out-format-tables` (T1-T3) |
| R140-R142, R147 (failure texts) | `47-out-data-init-failures` |
| R143-R146, R148 | `43-out-lifecycle`, `44-out-video-audio-flow`, `48-out-scaling` (stream parameters that do not depend on the encoder) |
| R160-R162, R168 | `44-out-video-audio-flow`, `48-out-scaling` |
| R163-R167 | `46-out-stop-gating`, `45-out-errors`, `43-out-lifecycle` |

Not covered by an executable script (derived from the source): the `rtmp://` special case of R140/R141, R143's HDR side data, bit-exact encoder options (R145/R146 option parsing and the x264 hint), R147's muxer option logging and file open errors, the Windows Defender text of R13, HEVC priority in R103,
`ffmpeg_mpegts_muxer` runtime behaviour (identical to `ffmpeg_muxer` in network mode; script `05-network-start` runs it), and the thread-creation failure branches.

## 11. Open questions

* The native MPEG-TS/SRT/RIST output (`obs-ffmpeg-mpegts.c` and the `-srt`, `-rist`, `-url` headers) replaces `ffmpeg_mpegts_muxer` in default builds. It is a separate large component (libav URL protocols for SRT/RIST, its own start/stop thread, connect-time reporting) and needs its own spec.
* Whether `ENABLE_HEVC` builds change the HLS codec list at registration is build configuration; the conformance build leaves it off (R103 HEVC branch unexecuted).
* The vectors depend on the system FFmpeg for the `ffmpeg_output` scripts: the muxer named `null`, `pcm_s16le` audio, and that the `null` muxer leaves stream time bases at `1/fps` / `1/sample_rate`. Video packet sizes and the video encoder name are intentionally never printed.
* The Rust sufficiency check required by the process brief was **skipped on request** (cost control); the spec has therefore not been validated by an independent implementation, and the rule texts R84 (purge loop guards) are the likeliest to need revision.
* Fakes (not the real components): `obs_data` (S-014) is a tiny typed store; signal/proc handlers (S-004) only print; `os_process_pipe_*` (S-001) is a byte decoder; `os_generate_formatted_filename` (S-023) is replaced by `format` with `%N` expanded and spaces mapped; file existence is virtual; the clock is a knob; `obs_encoder_packet_ref/release` is a counter.
  Windows/macOS differences (`.exe` name, Defender text) are source-derived only.
