# S-036 Media playback (`shared/media-playback`)

| | |
|---|---|
| Status | Draft (sufficiency check NOT run, see 11) |
| Reference | libobs 33.0.1 tree, `shared/media-playback/media-playback/` (`media.c`, `decode.c`, `cache.c`, `media-playback.c`, `closest-format.h`) |
| Target crate | `crates/obs-media-playback` |
| Depends on | S-001 (allocator/log levels), S-005 (deque is an implementation detail only), S-013 (`video_format_get_parameters_for_format` colour tables) |
| Conformance | `test/conformance/scripts/mediaplayback/` (33 scripts), runner `test/conformance/gen/mediaplayback_runner.c` |

## 1. Scope

The playback engine behind the "Media Source" and the stinger transition: it opens a media file or
network stream with FFmpeg, decodes audio and video on its own thread, converts decoded frames to
OBS frame descriptions and **paces** their delivery to two callbacks against a wall clock. Two
implementations sit behind one handle:

* **streaming** (`mp_media`): demux/decode on demand while playing, one thread per player;
* **cached** (`mp_cache`): chosen when the source is a local file *and* `full_decode` is requested:
  the whole file is decoded to memory first, playback then walks the arrays.

In scope: the state machines (play / stop / pause / seek / loop / reset / preload), timestamp
derivation, pacing arithmetic, the pixel/audio/colour mapping tables, option building for the
FFmpeg open call, decoder selection, and every observable corner of the above.
Out of scope: FFmpeg itself (container parsing, codec output, `swscale` pixel values), hardware
decoding (`hardware_decoding=true` path: device probing in priority order CUDA, D3D11VA, DXVA2,
VAAPI, VDPAU, QSV, VideoToolbox, and the hw-to-sw frame transfer: **not specified, not tested**),
the OBS source that wraps this (`obs-ffmpeg-source`), and the frame consumers.

### 1.1 What could and could not be linked

FFmpeg development headers and libraries were available (FFmpeg 8.x: libavformat/libavcodec 63,
libswscale 10, libavutil 61, libavdevice 63 from the system). **Nothing was unlinkable.** The real C sources of
media.c, cache.c, decode.c, media-playback.c are compiled unchanged. The conformance runner replaces
only the *container/codec entry points* by a scripted fake (`gen/mediaplayback_stubs.c`) so that every
packet, frame, error and seek position is dictated by the script (the model is in 2.2). Real libswscale
runs for pixel-format conversion, real libavutil helpers (rescale, dict, frame) run; swscale pixel
**values** are not compared (the vectors print pixel bytes and line sizes only for frames that were not
converted). The runner also supplies a **fake clock** (`os_gettime_ns`, `os_sleep_ms` advances it).

## 2. Terminology and conventions

RFC 2119 words. All times are signed 64-bit integers; "ns" nanoseconds; the container duration and
start time are microseconds (FFmpeg `AV_TIME_BASE`). `rescale(x, a/b, c/d)` = `x*(a/b)/(c/d)` computed
exactly (use 128-bit intermediates) and **rounded to nearest, ties away from zero** (FFmpeg
`av_rescale_q`). `NOPTS` = `i64::MIN`. "Wrap" = two's-complement i64 arithmetic; the unsigned
`timestamp` fields are the same bits.

### 2.1 Clock model

`now()` is the monotonic ns clock (fake in the conformance runner). `base_sys_ts` is a process-wide
value captured **once**: the first time a player is created while it is still 0, it is set to `now()`
(each of the two implementations keeps its own copy; a clock reading of exactly 0 leaves it 0 and
it is captured again at the next creation).

### 2.2 The abstract source (what FFmpeg must provide)

A conforming implementation needs these abstractions (any back end that behaves like FFmpeg):

* **Container**: `open(path, input_format?, options, flags)`, stream list, `duration`
  (us or NOPTS), `start_time` (us or NOPTS), `read_packet()` returning a packet (stream index,
  pts, size, data, key flag) or `EOF`, `EXIT` (interrupt) or another error, `seek(stream 0,
  target, flags BACKWARD | FRAME)`, per stream: time base, codec id, `nb_frames`, `avg_frame_rate`,
  metadata (`alpha_mode`), coded side data (mastering display luminance, content light level),
  `best_stream(type)` = first stream of that media type.
* **Decoder** (send/receive model): `send(packet | flush-marker)`, `receive() -> frame | EAGAIN | EOF`,
  `flush()`. A frame carries: best-effort timestamp (stream time base, or NOPTS), duration (stream
  time base, 0 = unknown), key flag, video: width, height, pixel format, colour space, range, transfer,
  primaries, planes with strides (stride of plane 0 may be negative = bottom-up image); audio:
  sample format, sample rate, channel count, sample count, data planes. The decoder's own
  `time_base` (decoder context) is read once, for the duration estimate in R14.
* Test fake: a scripted media (see conformance README) yields packets in order; a packet turns into
  exactly one frame; a decoder with `latency=L` returns a frame only when more than L frames are
  queued, or when draining; `seek(BACKWARD)` restarts at the last key packet of the stream whose pts
  <= target; `seek(FRAME)` at the target-th packet of the stream.

## 3. Interface

```c
typedef void (*mp_video_cb)(void *opaque, struct obs_source_frame *frame);
typedef void (*mp_audio_cb)(void *opaque, struct obs_source_audio *audio);
typedef void (*mp_stop_cb)(void *opaque);

struct mp_media_info {
    void *opaque;
    mp_video_cb v_cb, v_preload_cb, v_seek_cb;     /* any may be NULL */
    mp_audio_cb a_cb;  mp_stop_cb stop_cb;
    const char *path, *format;   /* format: forced demuxer name, NULL or "" = probe */
    char *ffmpeg_options;        /* "k=v k2=v2" (space separated); BORROWED, must outlive the player */
    int buffering;               /* bytes; 0 = no buffering flag semantics, see R04 */
    int speed;                   /* percent */
    enum video_range_type force_range;
    bool is_linear_alpha, hardware_decoding, is_local_file;
    bool reconnecting;           /* NOT used by the constructor, see Q-8 */
    bool request_preload, full_decode;
};
media_playback_t *media_playback_create(const struct mp_media_info *);
void media_playback_destroy(media_playback_t *);
void media_playback_play(media_playback_t *, bool looping, bool reconnecting);
void media_playback_play_pause(media_playback_t *, bool pause);
void media_playback_stop(media_playback_t *);
void media_playback_set_looping(media_playback_t *, bool);
void media_playback_set_is_linear_alpha(media_playback_t *, bool);
void media_playback_preload_frame(media_playback_t *);
int64_t media_playback_get_current_time(media_playback_t *);   /* milliseconds */
void media_playback_seek(media_playback_t *, int64_t ms);
int64_t media_playback_get_frames(media_playback_t *);
int64_t media_playback_get_duration(media_playback_t *);       /* microseconds */
bool media_playback_has_video(media_playback_t *);
bool media_playback_has_audio(media_playback_t *);
```

Frames handed to callbacks are valid only during the callback and are `obs_source_frame` /
`obs_source_audio` structures (field list: S-013 / libobs `obs-source.h`). Fields the player fills in
a video frame: `data[4]`, `linesize[4]`, `width`, `height`, `format`, `timestamp`, `full_range`,
`color_matrix[16]`, `color_range_min[3]`, `color_range_max[3]`, `max_luminance`, `flip`, `flags`
(`OBS_SOURCE_FRAME_LINEAR_ALPHA` = 1), `trc`. Audio: `data[8]`, `frames`, `samples_per_sec`,
`speakers`, `format`, `timestamp`.

Enumerations printed by the runner (names): video formats `NONE I420 NV12 YVYU YUY2 UYVY RGBA BGRA
BGRX Y800 I444 BGR3 I422 I40A I42A YUVA AYUV I010 P010 I210 I412 YA2L`; audio `U8BIT 16BIT 32BIT
FLOAT U8BIT_PLANAR 16BIT_PLANAR 32BIT_PLANAR FLOAT_PLANAR UNKNOWN`; speakers `UNKNOWN MONO STEREO
2POINT1 4POINT0 4POINT1 5POINT1 7POINT1`; colour spaces `DEFAULT 601 709 SRGB 2100_PQ 2100_HLG`;
trc `DEFAULT SRGB PQ HLG`; range `DEFAULT PARTIAL FULL`.

## 4. Data formats: mapping tables

All are pure functions; vectors `01-closest-format`, `02-converters`.

**D1 `closest_format(p)`** (decoded pixel format -> format the player hands over; if different from
the decoded format the frame is converted with a point-sampling scaler preserving size):

| decoded | result |
|---|---|
| yuyv422; uyvy422; yvyu422; p010le; yuva420p; yuva422p; yuv444p12le; yuva444p12le; yuv422p10le | itself |
| yuv444p, yuv444p{9,10,14,16}{le,be}, yuv444p12be | yuv444p |
| yuv422p, yuvj422p, yuv422p{9,12,14,16}{le,be}, yuv422p10be | yuv422p |
| nv12, nv21 | nv12 |
| yuv420p, yuv410p, yuv411p, yuvj420p, uyyvyy411 | yuv420p |
| yuv420p{9,10,12,14,16}{le,be} (including yuv420p10le itself) | yuv420p10le |
| yuva444p, yuva444p{9,10,16}{le,be}, yuva444p12be | yuva444p |
| rgba, bgra, bgr0 | itself |
| anything else (rgb24, gray, argb, abgr, yuvj444p, gbrp, nv16, none, ...) | bgra |

The vector `01-closest-format.expected` lists one input per line and is authoritative together with this
table (notably `yuv444p12le` and `yuv422p10le` stay themselves while their siblings of other depth or
endianness collapse).

**D2 `convert_pixel_format(p)`** (after D1): yuv420p I420; yuyv422 YUY2; yuv422p I422; yuv422p10le I210;
yuv444p I444; yuv444p12le I412; uyvy422 UYVY; yvyu422 YVYU; nv12 NV12; rgba RGBA; bgra BGRA; yuva420p
I40A; yuv420p10le I010; yuva422p I42A; yuva444p YUVA; yuva444p12le YA2L; bgr0 BGRX; p010le P010;
none and everything else NONE.

**D3 sample format**: u8 U8BIT, s16 16BIT, s32 32BIT, flt FLOAT, u8p U8BIT_PLANAR, s16p 16BIT_PLANAR,
s32p 32BIT_PLANAR, fltp FLOAT_PLANAR; everything else (dbl, s64, dblp, s64p) UNKNOWN.

**D4 speakers by channel count**: 0 UNKNOWN, 1 MONO, 2 STEREO, 3 2POINT1, 4 4POINT0, 5 4POINT1,
6 5POINT1, 8 7POINT1, anything else (7, 9, ..., 255) UNKNOWN.

**D5 colour space** `(space, trc, primaries)`: BT709 -> SRGB if trc is IEC61966-2-1 else 709;
FCC, BT470BG, SMPTE170M, SMPTE240M -> 601; BT2020_NCL -> HLG-2100 if trc is ARIB-STD-B67 else PQ-2100;
any other space -> (primaries == BT2020 ? (trc == ARIB-STD-B67 ? 2100_HLG : 2100_PQ) : DEFAULT).

**D6 range**: JPEG (full) -> FULL, everything else -> DEFAULT. The effective range of a frame is the
forced range if `force_range != DEFAULT`, else D6.

**D7 transfer**: BT709, GAMMA22, GAMMA28, SMPTE170M, SMPTE240M, IEC61966-2-1 -> SRGB; SMPTE2084 -> PQ;
ARIB-STD-B67 -> HLG; anything else DEFAULT.

**D8 libswscale-side colour space** (only to configure the scaler, no observable output in the vectors):
BT709 ITU709, FCC, BT470BG ITU624, SMPTE170M, SMPTE240M, BT2020_NCL BT2020, default ITU709.

Worked example (script `25-frame-description`): decoded `yuv420p10le`, space BT2020_NCL, trc SMPTE2084,
limited range: delivered format I010 (no conversion), colour space 2100_PQ, `trc=PQ`, `full_range=0`;
`color_matrix[0] = 1.16781`, `range_min[0] = 0.0625611`, `range_max[0] = 0.918866` (values from S-013's
parameter function; tolerance 1e-5).

## 5. Behaviour (numbered rules)

### 5.1 Construction (streaming)

* **S-036-R01** `media_playback_create`: the cached implementation is used iff `is_local_file &&
  full_decode`. If initialisation fails (cached: container cannot be opened/initialised synchronously,
  see R50; streaming: mutex/semaphore/thread creation) it returns NULL.
* **S-036-R02** Streaming constructor: copies the info callbacks/flags; `path` and `format` are copied,
  `ffmpeg_options` is *borrowed*. `speed`: if `!is_local_file` or `speed < 1` or `speed > 200` it
  becomes 100. A thread is started unless `full_decode` (the thread opens the container asynchronously;
  creation therefore succeeds even for a missing file, R06). `base_sys_ts` is captured (2.1).
* **S-036-R03** NULL tolerance of the dispatcher: `destroy`, `play`, `play_pause`, `stop`,
  `preload_frame`, `get_current_time`, `get_frames`, `get_duration`, `has_video`, `has_audio` accept
  NULL (no-op / 0 / false). `set_looping`, `set_is_linear_alpha`, `seek` do not check (D-4).

### 5.2 Opening (runs on the player thread, or in the cached constructor)

* **S-036-R04** Option dictionary, in this order: (1) if `buffering != 0` and the source is not local
  and the path does not begin with the four characters `rist`, add `buffer_size=<buffering>`; (2) every `key=value` of the user option string
  (pairs separated by a space, key/value by `=`); if parsing fails, log WARNING `Failed to parse
  FFmpeg options: ...` (only the level is contractual) and keep what was parsed so far; (3) if not
  local, `stimeout=30000000`. Format-context flags: if `buffering == 0` the "no buffer" flag is set
  (regardless of local). If not local an interrupt callback (R09) is installed. Local files get none of
  (1), (3) nor the interrupt callback. Vectors: `18-open-options`, `17-nonlocal`.
* **S-036-R05** A non-empty `format` is looked up as an input-format name; if it is unknown log INFO
  `MP: Unable to find input format for '<path>'` and open with probing. Empty or NULL = probing.
* **S-036-R06** Open failure: log WARNING `MP: Failed to open media: '<path>'` unless the player's
  `reconnecting` flag is set (it is set only by `play(.., reconnecting=true)`, never by the info
  structure, Q-8); initialisation fails. Stream-info failure: WARNING `MP: Failed to find stream info
  for '<path>'`, fails. On success `reconnecting` is cleared.
* **S-036-R07** Decoder set-up per media type (video first, then audio): pick the *first* stream of
  that type; none = that type absent (`has_video`/`has_audio` false, silently). If the stream codec is
  VP8 or VP9 and the stream metadata contains `alpha_mode` equal to the string `1`, first try the decoder
  named `libvpx` (VP8) / `libvpx-vp9` (VP9); if it is not found fall back to the default decoder for
  the codec id. No decoder: WARNING `MP: Failed to find <video|audio> codec`; decoder open failure:
  WARNING `MP: Failed to open <video|audio> decoder: <ffmpeg text>`; the type is then absent. If neither
  type is present: WARNING `MP: Could not initialize audio or video: '<path>'` and initialisation fails.
  After opening the codec context: if its thread count was 1 and the codec is not PNG, TIFF, JPEG2000,
  MPEG4, WebP it is set to 0 (auto); not observable in the vectors. Vector: `23-open-failures`,
  `24-vp8-alpha-hdr`.
* **S-036-R08** `max_luminance` (video only, u16): iterate the stream's coded side data in order;
  a mastering-display entry with luminance sets it to `trunc(max_luminance_as_double + 0.5)`
  (4000.5 -> 4001); a content-light-level entry sets it to MaxCLL; the **last** such entry wins.
  The value is exported in every video frame (R18).
* **S-036-R09** Interrupt callback (non-local only): returns true iff a poll at least 20 ms (clock
  difference > 20 000 000 ns) after the previous poll found `kill || stopping` set (under the mutex);
  between polls it returns false regardless. Vector `17-nonlocal`.

### 5.3 Demux and decode

* **S-036-R10** `next_packet`: read one packet. On error: `EOF`/`EXIT` are returned silently, any other
  error logs WARNING `MP: av_read_frame failed: <ffmpeg text> (<code>)`; the code is returned. On
  success the packet goes to the **audio** decoder's queue if audio is present and the packet's stream is
  the audio stream, else to the video decoder's queue if video is present and it is the video stream,
  else (also when the packet has size 0) it is discarded. Packet buffers are recycled through a pool.
  Vector `22-decode-errors`. (The reference leaks the packet object on every failed read, D-1.)
* **S-036-R11** `decode_next(D)` (algorithm 6.1): clears `D.frame_ready`; if the media is not at EOF and
  `D` has no queued packet it returns immediately (true). Otherwise it feeds the decoder until a frame
  is produced, a decode error occurs or the decoder is drained, then derives pts/duration (R12).
  It always returns true; a decode error (negative result) drops the current packet only, no log.
  When the media is at EOF and the queue is empty an empty packet is sent once (drain); once the decoder
  reports end of stream with no frame, `D.eof = true`. Vectors `20-decode-timestamps`, `22-decode-errors`.
* **S-036-R12** Timestamps of a decoded frame. `last_pts = D.frame_pts`. `D.frame_pts` = best-effort
  timestamp rescaled from the stream time base to ns, or `D.next_pts` when the frame has none. Duration:
  the frame's own duration rescaled from the stream time base to ns when non-zero; otherwise the estimate
  of R13. If `M.speed != 100`: both `D.frame_pts` and the duration are replaced by
  `rescale(x, 1/speed, 1/100)` (= `x*100/speed`). Then `D.last_duration = duration`,
  `D.next_pts = D.frame_pts + duration`.
* **S-036-R13** Duration estimate (frame duration 0): audio: `rescale(nb_samples, 1/sample_rate, 1/1e9)`.
  Video: if `last_pts != 0`: `D.frame_pts - last_pts` (note: *new unscaled* minus *previous scaled*, Q-1;
  and a previous pts of exactly 0 counts as "none", Q-2); else if `D.last_duration != 0` that value;
  else `rescale(tb.num, tb, 1/1e9)` with `tb` = the decoder context time base (`num*num/den` seconds in
  ns, Q-3).
* **S-036-R14** `flush(D)`: decoder flush, queued packets and the pending packet released,
  `eof=false`, `frame_pts=0`, `frame_ready=false`, `next_pts=0`. (`last_duration` and
  `got_first_keyframe` are kept.)
* **S-036-R15** `prepare_frames` (algorithm 6.2) returns false iff `next_packet` fails with an error
  other than EOF/EXIT, or `decode_next` returns false (never). While neither enabled decoder is "ready"
  (a decoder is ready when absent, at EOF, or holding a frame) it reads packets and decodes. EOF/EXIT sets
  `M.eof` unless the player is actively seeking (`seek_next_ts && pause`), in which case it just stops
  the loop. Each loop iteration clears the cached `obsframe.data[0]` (so a preload request cannot reuse
  a stale frame). After the loop, if video is present and has a frame and the scaler is not yet set up:
  `scale_format = D1(frame pixel format)`, and if it differs from the frame format a scaler for that
  conversion (size unchanged, point sampling) is created; failure logs WARNING `MP: Failed to initialize
  scaler` / `MP: Failed to create scale pic data` and prepare fails (returns false).

### 5.4 Delivery

* **S-036-R16** `can_play(D)`: `full_decode` mode: `D.frame_ready`; else `frame_ready && (frame_pts <=
  M.next_pts_ns || frame_pts - M.next_pts_ns > 2 000 000 000)`. Vector `30-can-play-frame` (exactly 2 s
  ahead is not playable; 2 s + 1 ns is).
* **S-036-R17** Audio delivery `next_audio`: if not `can_play` return. Otherwise `frame_ready = false`;
  if no audio callback return. Build: `samples_per_sec = sample_rate * speed / 100` (integer),
  `speakers` (D4 of the channel count), `format` (D3), `frames = nb_samples`, planes pointers,
  `timestamp` (R19). If `format == UNKNOWN` return without calling the callback (the frame is consumed).
  Vector `11`, `21`, `26`.
* **S-036-R18** Video delivery `next_video(preload)`:
  1. normal: if not `can_play` return; `frame_ready=false`; if no video callback return. preload: return
     unless `frame_ready`; `frame_ready` is not cleared.
  2. width or height zero: log ERROR `MP: media frame width or height are zero ('<path>': <w>x<h>)`, return.
  3. Pixels: with a scaler: convert the frame to `scale_format` into the scaler's buffer (size unchanged);
     else use the decoded planes. `flip = linesize[0] < 0 && linesize[1] == 0` (of whichever source).
     `linesize[i] = |linesize[i]|`; when flipping `data[0] -= linesize[0] * (height-1)`, i.e. data[0] points
     at the lowest address of the bottom-up image.
  4. `format = D2(scale_format)` (the *converted* format); space = D5, range = forced range or D6. If any of
     (format, space, range) differs from what the last delivered frame had (initially NONE/DEFAULT/DEFAULT, so
     the first frame always recomputes) then `full_range = (range == FULL)` and the colour matrix and range
     min/max are recomputed with the S-013 function for (space, range, format); on failure the frame format is
     set to NONE and nothing is delivered (unreachable with the tables above).
  5. If `format == NONE` return. Set `timestamp` (R19), `width`, `height`, `max_luminance` (R08), `flip`,
     `flags = linear_alpha ? 1 : 0`, `trc` (D7).
  6. Key-frame gate (non-local sources only): until the first key frame has been *delivered* every non-key
     frame is dropped here (after being consumed in step 1). The gate is never re-armed (not by loops,
     seeks or resets; Q-9).
  7. Delivery: preload mode: if `seek_next_ts` and a seek callback exists -> seek callback; else if
     `!request_preload` -> preload callback; otherwise nothing. Normal mode: video callback.
  Vectors `25`, `26`, `17`, `31`.
* **S-036-R19** Timestamp of a delivered frame: `full_decode`: `D.frame_pts`; otherwise
  `base_ts + D.frame_pts - start_ts + play_sys_ts - base_sys_ts` (wrapping i64, reinterpreted unsigned).

### 5.5 Pacing and the thread loop

* **S-036-R20** `base_pts(M) = max(0, v.next_pts if video, a.next_pts if audio)`. `next_min_pts(M) =
  min(v.frame_pts if video && frame_ready, a.frame_pts if audio && frame_ready)`, `i64::MAX` if none.
* **S-036-R21** `calc_next_ns` (after a delivery round): `m = next_min_pts`, `delta = m - next_pts_ns`.
  If `seek_next_ts` then `delta = 0` and `seek_next_ts = false`; else negative deltas and deltas greater
  than 3 000 000 000 become 0. `next_ns += delta` (unsigned, modular); `next_pts_ns = m`. Vector `27-pacing`.
* **S-036-R22** `sleep`: if `next_ns == 0` set it to `now()` and do not sleep, return false. Else if
  `next_ns > now`: `delta_ms = (next_ns - now + 500000) / 1000000` (u32); if `delta_ms > 0` sleep
  `min(delta_ms, 200)` ms and return `delta_ms > 200` (timeout: the iteration then does no delivery).
* **S-036-R23** `eof(M)`: `v_ended = !has_video || !v.frame_ready`, likewise audio; if both ended: lock;
  `looping = M.looping`; if not looping `active = false; stopping = true`; unlock; call `reset`; return true.
* **S-036-R24** `reset(M)` (algorithm 6.3): rebases and rewinds: `next_ts = base_pts`; `offset = next_ts -
  next_pts_ns`; `start_time = container start_time (NOPTS -> 0)`; `eof=false`; `base_ts += next_ts`;
  `seek_next_ts=false`; `seek_to(start_time)`; read-and-clear `stopping` and read `active` under the mutex;
  `prepare_frames` (fail -> reset returns false); if `active`: `play_sys_ts = now()` if it is 0,
  `start_ts = next_pts_ns = next_min_pts`, and if `next_ns != 0` then `next_ns += offset`;
  else (inactive): `start_ts = next_pts_ns = next_min_pts`, `play_sys_ts = now()`, `next_ns = 0`.
  Then `pause = false`; if `!active && is_local_file && preload callback`: `next_video(preload=true)`;
  if `stopping && stop callback`: stop callback. Returns true.
* **S-036-R25** Thread main loop (algorithm 6.4): after the thread starts it opens the container (R04-R08),
  performs one `reset` (the player starts inactive, so a local file with a preload callback gets the
  preload frame and `play_sys_ts` is set), then loops. Failure of open or of the first reset ends the
  thread, which then calls the stop callback. Failure of `prepare_frames` inside the loop ends the thread
  likewise. Vectors `10`, `12`, `13`, `22`.
* **S-036-R26** Order of work in one loop iteration: (a) snapshot `active` and `pause`; if inactive or paused
  wait for the semaphore, and after waking, if the *snapshot* said paused run `reset_ts` (Q-4); else run
  `sleep`; (b) under the mutex copy-and-clear `reset, kill, preload_frame, seek, reset_ts`, copy `pause` and
  `seek_pos`; (c) `kill` -> leave the loop (no stop callback); (d) `reset` -> `reset(M)`, next iteration;
  (e) `seek` -> `seek_next_ts = true; seek_to(seek_pos)`, next iteration; (f) `reset_ts` -> `reset_ts()`, next
  iteration; (g) if paused -> next iteration; (h) if `preload_frame && cached video frame pointer valid &&
  !snapshot_active` -> preload callback with the last prepared frame; (i) if the snapshot was active and no
  timeout: `next_video(false)` if video, `next_audio` if audio, `prepare_frames`, `eof` check (reset+continue
  if ended), else `calc_next_ns`. The snapshot is stale after a wake-up from the wait (Q-5): the first
  iteration after a play request delivers nothing.
* **S-036-R27** `reset_ts()` (clock rebase on pause/resume): `base_ts += base_pts`; `play_sys_ts = now()`;
  `start_ts = next_pts_ns = next_min_pts`; `next_ns = 0`. Vector `14-pause`.

### 5.6 Control calls (streaming)

All take the mutex, post the semaphore (wake the thread) after unlocking, and return immediately.

* **S-036-R28** `play(loop, reconnecting)`: if already active set `reset`; then `looping = loop; active = true;
  reconnecting = reconnecting`.
* **S-036-R29** `stop`: if active: `reset = true; active = false; stopping = true`. (Not active: only the wake-up.)
  The stop callback is invoked by the thread after the reset triggered by a stop request, by natural end of
  a non-looping stream (R23), or when the thread dies; never for `kill`. Vector `13`.
* **S-036-R30** `play_pause(p)`: only if active: `pause = p; reset_ts = !p`. Vector `14`.
* **S-036-R31** `seek(ms)`: only if active: `seek = true; seek_pos = ms * 1000` (microseconds).
* **S-036-R32** `seek_to(pos_us)` (thread): target stream is **stream index 0** (Q-6). If the container
  duration is NOPTS: flag FRAME and target = `pos` unscaled (Q-7); else flag BACKWARD and target =
  `rescale(pos, 1/1e6, stream0.time_base)`. If local: container seek (failure logs WARNING `MP: Failed to
  seek: ...`, continues). If video present and local: flush video decoder; then, if `seek_next_ts && pause &&
  preload callback && prepare_frames`: `next_video(preload=true)` (delivers to the **seek** callback if
  present, else the preload callback unless `request_preload`). If audio present and local: flush audio.
  Non-local: nothing happens (no seek, no flush). Vectors `15`, `16`, `19`.
* **S-036-R33** `preload_frame()`: if `request_preload && thread started && preload callback`: set
  `preload_frame`. Vector `31`.
* **S-036-R34** `set_looping(b)`: plain store (no lock). `set_is_linear_alpha(b)`: plain store.
* **S-036-R35** Queries. `get_current_time = base_pts * speed / 100 000 000` (integer, i64; ms at speed
  100). `get_duration`: container duration (us) or 0 without container. `get_frames`: 0 without container;
  no video stream: WARNING `MP: Getting number of frames failed: No video stream in media file!` and 0;
  `nb_frames > 0`: that; else DEBUG `MP: nb_frames not set, estimating using frame rate and duration` and
  `ceil(((duration_us as f64 / 1e6) * fps_num as f64) / fps_den as f64)` evaluated left to right in IEEE double arithmetic, cast to i64 (Q-10, D-6).
  `has_video/has_audio` = decoder presence. Vector `29`.
* **S-036-R36** `free`: `stop`, kill + join the thread (if any), release decoders/pools/scaler/container,
  zero the object.

### 5.7 Cached implementation

* **S-036-R50** Constructor: runs the streaming constructor in `full_decode` mode (no thread), opens the
  container **synchronously** (R04-R08; failure -> NULL), keeps `speed = info.speed` **unclamped** (D-3),
  `media_duration = container duration`, `has_video/has_audio` from the open, `base_sys_ts` captured (own
  copy), mutex/sem, then starts the cache thread. The thread first decodes everything (R51), then enters
  the loop (R56). A failed decode ends the thread with the stop callback.
* **S-036-R51** Decode pass (`full_decode` set, callbacks replaced by "store" callbacks): `reset(M)` once
  (inactive: preload callback is absent), then repeat { `next_video(false)`; `next_audio`;
  `prepare_frames` (failure aborts) } until `eof(M)` (which with `looping=false, active=false` sets
  `stopping` and runs `reset(M)` once more; irrelevant). Stored: a deep copy of every video frame (format,
  size, planes, colour description) and every audio segment (all planes concatenated), each with its raw
  `timestamp` = pts ns. `final_v_duration` / `final_a_duration` = `last_duration` of the respective decoder
  at the time the *last* stored frame was produced. `start_time = container start_time (NOPTS->0)`. Then
  the streaming object is freed. Vector `40-cache-decode`.
* **S-036-R52** Cache indices: `cur_*`, `next_*` (element indices), `next_v_ts`, `next_a_ts` (ns timestamp
  of the NEXT element to deliver). `v_eof = cur_v_idx == nvideo`, `a_eof` likewise. `next_min_pts =
  min(next_v_ts if has_video && !v_eof, next_a_ts if has_audio && !a_eof)`, `i64::MAX` if none;
  `base_pts = max(0, next_v_ts if video, next_a_ts if audio)`.
* **S-036-R53** `reset_ts`, `sleep`, `calc_next_ns` as R27, R22, R21 on the cache fields.
* **S-036-R54** `cache seek_to(pos)` (also called with `start_time` by reset; "closest" logic): if
  `pos > media_duration`: log WARNING `MP: Invalid seek position` and return. Video: `new_v_idx` = index of
  the **first** stored frame with `timestamp >= pos`, or the last index if none (0 if no frames; with
  frames but index 0 candidates the loop leaves it at the last visited); `next_v_ts` = timestamp of frame
  `new_v_idx + 1`, or `timestamp(new_v_idx) + final_v_duration` if it is the last. Audio likewise. Then
  `cur_v = next_v = new_v_idx`, `cur_a = next_a = new_a_idx`. **Unit defect D-2**: `pos` arrives in
  microseconds (`seek(ms)*1000`, container start time) but is compared with ns timestamps unconverted;
  the comparison against `media_duration` (us) is the only consistent one. Reproduce exactly. Vector `42`.
* **S-036-R55** Cache video step `next_video(preload)`: if `next_v_idx == n`: (end) `cur_v_idx =
  next_v_idx` iff `can_play_video`, return. Else take frame `next_v_idx`; the delivered copy's timestamp =
  `base_ts + ts - start_ts + play_sys_ts - cache_base_sys_ts`. Normal: if not `can_play_video` return;
  call the video callback; `if cur_v_idx < next_v_idx: cur_v_idx++`; `next_v_idx++`; `next_v_ts += (next
  element ts - delivered ts)` or `final_v_duration` when it was the last. Preload: seek callback if
  `seek_next_ts` and present, else (if `!request_preload`) the preload callback, **with the timestamp
  rebased** by the formula above. `can_play_video = !v_eof && (next_v_ts <= next_pts_ns || next_v_ts -
  next_pts_ns > 2e9)`. Audio step is the same without the preload branch; the audio callback receives the
  segment copy with a rebased timestamp.
* **S-036-R56** Cache `reset(c)`: like R24 with: `next_ts = base_pts`; `offset = next_ts - next_pts_ns`;
  `eof=false; base_ts += next_ts; seek_next_ts=false; seek_to(start_time)` (R54, units Q-11); read-and-clear
  `stopping`, read `active`; if has_video: `cur_v = next_v = 0` and `next_v_ts = ts(frame[min(1,n-1)])`
  (**the second frame's timestamp**, Q-12); same for audio; then `start_ts = next_pts_ns = next_min_pts`
  and the `active` / inactive branches of R24 verbatim; `pause=false`; if `!active && preload callback`:
  `next_video(preload=true)`; if `stopping && stop callback`: call it. Returns true.
* **S-036-R57** Cache thread loop = R26 with the cache steps; deviations: (h) a preload request calls the
  preload callback with **video frame 0 and its raw, un-rebased timestamp** (Q-13), with no check of
  `active` and without the cached-pointer test; (i) after `next_video/audio`
  there is no `prepare_frames`; `eof` = both `v_eof` and `a_eof` (or absent): if not looping
  `active=false; stopping=true`, then reset. The cache **performs no initial reset** before the first
  `play` (Q-14).
* **S-036-R58** Cache controls (`play`, `play_pause`, `stop`, `preload_frame`, `seek`) are R28-R31, R33 with
  the cache fields (`seek` stores `ms*1000`; `play` has no reconnecting parameter; `preload_frame`
  requires `request_preload`, a live thread and a preload callback). `get_current_time = base_pts * speed /
  100 000 000` (cache `speed`, D-3); `get_frames = number of stored video frames`; `get_duration =
  media_duration`; `has_*` from the open. `set_looping` stores to the cache; `set_is_linear_alpha`
  stores into the embedded decode-time object, which is already finished, so it has no visible effect on
  cached playback (Q-15, not covered by a vector).
* **S-036-R59** Cache free: stop, kill/join, free stored frames/segments and the rest.

## 6. Algorithms (own pseudocode)

### 6.1 `decode_next(D)`

```
D.frame_ready = false
if !M.eof and D.queue.empty: return true
while !D.frame_ready:
    if !D.pending:
        if D.queue.empty:                       # only reachable when M.eof
            D.cur = EMPTY_PACKET                # drain marker, not "pending"
        else:
            D.cur = D.queue.pop_front(); D.pending = true
    (got, used) = step(D)                       # see below
    if !got and used == 0: D.eof = true; return true
    if used < 0:                                # decode error: drop this packet only
        if D.pending: release packet; D.pending = false
        return true
    D.frame_ready = got
    if D.pending:
        D.cur.advance(used)
        if D.cur.remaining() <= 0: release; D.pending = false
if D.frame_ready: derive pts and duration as R12/R13
return true

step(D):
    r = decoder.receive()
    if r is frame:            return (got=true, used=0)        # a queued frame comes out first
    if r is error other than EAGAIN/EOF: return (false, r)
    if r is EOF:              return (false, 0)
    s = decoder.send(D.cur)   # EAGAIN or EOF here is treated like success
    if s is error other than EAGAIN/EOF: return (false, s)
    r = decoder.receive()
    if r is frame:            return (true, D.cur.size)
    if r is error other than EAGAIN/EOF: return (false, r)
    return (false, D.cur.size)       # EAGAIN: packet consumed, no frame yet (latency)
                                     # (r == EOF: size too; used = size >0 so not "eof")
```
(Hardware frames: out of scope.) With the drain marker `D.cur.size == 0`, a returned frame has `used = 0`
and the loop calls `step` again; when the decoder is empty it returns EOF and `D.eof` becomes true.
A frame returned on the first `receive` leaves a pending packet un-advanced; it is sent by a later call.

### 6.2 `prepare_frames(M)`

```
seeking = M.seek_next_ts and M.pause
while !ready(M):
    if !M.eof:
        r = next_packet(M)
        if r in {EOF, EXIT}: if !seeking { M.eof = true } else { break }
        elif r < 0: return false
    M.obsframe.data[0] = null
    if has_video and !decode_next(video): return false
    if has_audio and !decode_next(audio): return false
ready(M) = (!has_audio or a.eof or a.frame_ready) and (!has_video or v.eof or v.frame_ready)
if has_video and v.frame_ready and no scaler: set up conversion (R15)
return true
```

### 6.3 `reset(M)` and one play iteration

See R24 and R26(i). Worked example (`10-video-basic`, clock 1e9 at creation, frames pts 0/40/80/120 ms,
duration 40 ms each, play without loop): preload ts 0; after play: deliveries ts 0, 40e6, 80e6, 120e6 with a
sleep of 40 ms between each (fake clock +40 ms), then the stream is exhausted: `eof` -> `active=false,
stopping=true`, reset: `base_ts = 160e6` (v.next_pts), preload callback with ts
`base_ts + 0 - 0 + play_sys_ts(1.12e9) - 1e9 = 280e6`, stop callback. `get_current_time` = 40 (the first
frame's end after the reset).

### 6.4 Thread loop skeleton

```
open container; if fail: stop_cb(); exit          # R25
if !reset(M): stop_cb(); exit
loop:
    (a)-(i) as R26
```

## 7. Concurrency

* One mutex protects the control flags (`active, pause, stopping, looping(read in eof), reset, kill, seek,
  seek_pos, reset_ts, preload_frame`). Control calls take it briefly and post the semaphore afterwards; the
  semaphore is counting, so wake-ups accumulate (spurious wake-ups run an iteration with no pending flags;
  while paused each such wake runs `reset_ts`, Q-4).
* `set_looping`, `set_is_linear_alpha` and `reconnecting` stores are not synchronised.
* Callbacks are invoked on the player thread (and, for `preload`/`seek` callbacks, during `reset` and
  `seek_to` on that thread). Callbacks MUST NOT block; they may call the control API (the vectors stop
  playback from inside the video callback: `51-threaded-loop-stop`).
* `destroy` joins the thread; the stop callback is not delivered for the destroy-time kill.
* Cache: the decode pass runs entirely on the thread before the first command is looked at.

## 8. Reference defects and quirks

QUIRKS (reproduce; vectors fix them):

* **Q-1** (R13) At `speed != 100` the pts-difference duration estimate subtracts the previous *scaled* pts
  from the new *unscaled* pts: frames without durations at speed 50 give durations 80, 160, 0, -200 ms.
  `21-speed` (second half).
* **Q-2** (R13) A previous pts of exactly 0 counts as "no previous frame": the first real estimate after
  pts 0 uses the previous duration, not `pts - 0`.
* **Q-3** (R13) The decoder-time-base fallback is `tb.num * tb` (not `tb`): with `1/25` it yields 40 ms, with
  `2/25` 160 ms. `20-decode-timestamps`.
* **Q-4** (R26) Every wake-up of a paused player runs `reset_ts`, which *adds* the end position to
  `base_ts` again: pausing and any later wake (a seek, a preload request, resume) skews the timestamps of
  all following frames by multiples of the played length. `14-pause`, `16-seek-paused`.
* **Q-5** (R26) The "is active" snapshot is taken before the semaphore wait and not refreshed, so the
  iteration right after a wake-up never delivers. `10-video-basic` (`mtickw 0 0`).
* **Q-6** (R32) Seeks always use stream index 0 and its time base even when stream 0 is not the video stream.
* **Q-7** (R32) Files without a duration use the FRAME seek flag with the microsecond value unscaled.
  `15-seek`.
* **Q-8** (R06) `mp_media_info.reconnecting` is ignored; only `play(..., reconnecting)` sets the flag, and
  then only after the player thread may already have opened the file. `23-open-failures`.
* **Q-9** (R18.6) The key-frame gate (non-local) is armed once per player lifetime.
* **Q-10** (R35) With unknown duration (NOPTS) `get_frames` evaluates the double formula on the raw NOPTS
  value, giving a large negative number (`-230584300921369` at 25 fps). `29-queries`.
* **Q-11** (R56) The cache reset seeks to `start_time` in microseconds although the arrays hold ns timestamps.
* **Q-12** (R56) After a cache reset `next_v_ts` is the timestamp of the *second* frame while the first is
  still to be delivered; since `start_ts` is derived from it, the first frame of every pass is delivered with
  a timestamp earlier by one frame duration (`-40 ms` in `43-cache-loop-stop-pause`).
* **Q-13** (R57) The cache preload callback gets frame 0 with its raw stored timestamp.
* **Q-14** (R57) A cache never reset before the first `play` has `play_sys_ts = start_ts = 0`: timestamps
  are `pts - base_sys_ts` (`-1000000000` for clock base 1e9, `41-cache-playback`).
* **Q-15** (R58) Linear-alpha toggling after construction has no effect on cached playback (derived from the
  source; not covered by a vector).

DEFECTS (reference is wrong; vectors record it; a conforming implementation MUST match the vectors until a
design decision changes this spec):

* **D-1** (R10) The packet object allocated by `next_packet` is leaked when the read fails (every EOF/error).
  Not observable. A Rust implementation MUST NOT leak (required behaviour = no behavioural difference).
* **D-2** (R54) Cache seek unit mismatch (us vs ns), so `seek(ms)` on a cached player lands on a frame that
  is about 1000x too early (seek to 80 ms selects the frame at 40 ms in `42-cache-seek-closest`, last
  section). Required: reproduce; open question OQ-2 asks whether to fix.
* **D-3** (R50/R58) The cache keeps `speed` unclamped: speed 0 makes `get_current_time` 0, speed 500 five
  times too large. `44-cache-speed-time`.
* **D-4** (R03) `set_looping`, `set_is_linear_alpha`, `seek` dereference a NULL handle. Required: no-op.
  Not tested (crash).
* **D-5** Cache of a file with a video stream that decoded zero frames: reset/seek index an empty array
  (null dereference). Required: no frame is delivered, no crash; indices stay 0. Not tested.
* **D-6** `get_frames` with `avg_frame_rate = 0/0` evaluates NaN -> integer cast (undefined). Required:
  return 0. Not tested (the vectors use `den = 1`).

## 9. Implementation notes (non-normative)

* Model the two FFmpeg abstractions (2.2) as traits (`Demuxer`, `Decoder`) so a scripted implementation
  can drive the conformance runner; real code wraps `ffmpeg-next`/`ffmpeg-sys`.
* Use `i64` with explicit `wrapping_*` for timestamp arithmetic; `next_ns` is a `u64`.
* One struct per player with a `Mutex<Flags>` plus a counting semaphore (e.g. `Mutex<usize>` + `Condvar`).
  Keep the loop structure of R26 literally; refactoring it changes Q-4/Q-5 behaviour.
* The two implementations share ~90% of the pacing code: implement a trait for "timeline source" if desired,
  but the differences in R56/R57 (second-frame `next_v_ts`, no initial reset) must remain.
* Rounding helper for `rescale`: `((x as i128 * num + den/2 * sign) / den)` with ties away from zero.
* Frames passed to callbacks borrow internal buffers; expose them as `&Frame` valid for the call.

## 10. Conformance coverage

Runner / script language: `test/conformance/README.md` section "mediaplayback runner (spec S-036)". Comparison:
integers and strings exact; `mat0/min0/max0` tolerance 1e-5; log lines: compare the **level** and, for texts
beginning `MP:`, the text up to the first FFmpeg-supplied text (`: ` followed by an FFmpeg error string, and
`av_read_frame failed` suffix).

| Rule(s) | Scripts |
|---|---|
| D1-D8 tables | 01-closest-format, 02-converters, 25-frame-description |
| R01, R02, R03 | 23-open-failures (init), 50/52/53-threaded-*, `t_null` in 52 |
| R04, R05, R06, R09 | 17-nonlocal, 18-open-options, 23-open-failures |
| R07, R08 | 23-open-failures, 24-vp8-alpha-hdr |
| R10 | 22-decode-errors, 20-decode-timestamps |
| R11, R12, R13, R14 | 20-decode-timestamps, 21-speed, 22-decode-errors, 15-seek |
| R15 | 10, 11, 19, 22, 25 |
| R16 | 30-can-play-frame, 27-pacing |
| R17 | 11-audio-video-interleave, 21-speed, 26-frame-edge-cases, 19 |
| R18, R19 | 25-frame-description, 26-frame-edge-cases, 17-nonlocal, 31-preload, 16-seek-paused |
| R20, R21, R22 | 27-pacing, 10-video-basic, 11 |
| R23, R24, R25, R26, R27 | 10, 12-loop, 13-stop-and-restart, 14-pause, 22 |
| R28, R29, R30 | 13, 14, 15, 51-threaded-loop-stop |
| R31, R32 | 15-seek, 16-seek-paused, 19 |
| R33 | 31-preload |
| R34 | 25 (linear flag), 51 |
| R35 | 29-queries, 21-speed (time), 52 |
| R36 | all (teardown), 50-53 |
| R50, R51 | 40-cache-decode, 53-threaded-cache |
| R52, R53, R55 | 41-cache-playback, 43-cache-loop-stop-pause |
| R54 | 42-cache-seek-closest |
| R56 | 41, 43 |
| R57 | 41, 43 |
| R58 | 43, 44-cache-speed-time, 53 |
| R59 | all cache scripts (cfree) |
| Q-1..Q-3 | 21-speed, 20-decode-timestamps |
| Q-4, Q-5 | 14-pause, 16-seek-paused, 10 |
| Q-6, Q-7 | 15-seek, 19 |
| Q-8 | 23-open-failures |
| Q-9 | 17-nonlocal |
| Q-10 | 29-queries |
| Q-12, Q-13, Q-14 | 41, 43 |
| D-2, D-3 | 42, 44 |
| D-1 | (leak; not observable) |

Totals: 33 scripts, 33 vectors, 46 rules (R01-R36, R50-R59; R37-R49 intentionally unused).

## 11. Open questions

* **Sufficiency check not run** (cost control): no Rust re-implementation was built from this spec, so
  gaps that only an implementer would notice have not been found. Treat the spec as Draft.
* **OQ-1** Hardware decoding (R07 and the hw frame transfer path) is unspecified and untested.
* **OQ-2** Should the cache seek unit mismatch (D-2) and the unclamped cache speed (D-3) be fixed in the Rust
  version? Until decided, reproduce.
* **OQ-3** Behaviour of the player when `stream 0` is not a video/audio stream (Q-6) is reproduced from the
  source reading only; the conformance fake does not model non-media streams.
* **OQ-4** swscale's conversion output is not specified (library dependent). Implementers may use any
  scaler that converts to the target format keeping the size; sample bytes are not compared.
* **OQ-5** Behaviour on Windows/macOS differences (thread naming, `os_sleep_ms` granularity) not examined.
