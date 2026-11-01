# S-037 `ffmuxproto`: the obs-ffmpeg-mux helper process protocol

| | |
|---|---|
| Status | Ready for review (Rust sufficiency check skipped, see 11) |
| Reference | libobs 33.0.1, `plugins/obs-ffmpeg/obs-ffmpeg-mux.c`/`.h` (sender, in the OBS process), `plugins/obs-ffmpeg/ffmpeg-mux/ffmpeg-mux.c`/`.h` (receiver, helper program `obs-ffmpeg-mux`) |
| Target crate | `crates/obs-ffmpeg-mux` (helper binary) + sender module in the ffmpeg output crate |
| Depends on | S-001 (platform pipe), S-024 (`os_process_args`) for the sender |
| Conformance | `test/conformance/scripts/ffmuxproto/` (22 scripts) |

## 1. Scope

The "FFmpeg output" family (local recording, replay buffer, split-file recording, MPEG-TS/SRT/UDP/TCP/RIST/HTTP
network output, HLS) does not link libav into OBS. The OBS process starts the helper program `obs-ffmpeg-mux`
with a command line, and feeds it **already encoded** packets over the helper's **stdin**. The helper owns the
container muxer (libavformat) and the output file or socket. This spec defines everything that crosses the process
boundary and the helper's observable behaviour:

* the command line (section 4),
* the byte stream on stdin: a 32-byte packet info structure, then payload bytes (section 5),
* the order of messages: header packets, data packets, change-file packets, EOF (section 6),
* how the helper turns the command line and header packets into streams and codec parameters (section 7),
* timestamp conversion (section 8), error reporting and exit status (section 9),
* what the sender (OBS side) puts on the wire (section 10).

Out of scope: libavformat itself (container internals), the encoders, the file-name generation and split decisions
of the sender (only their effect on the wire is specified), HLS segmenting policy, replay buffer policy.

Either side may be reimplemented in isolation: a Rust sender must work with the C helper and the other way round.
The helper is a separate executable, so the two sides share no memory and no ABI beyond the byte stream.

## 2. Terminology and conventions

RFC 2119 words apply. All multi-byte integers on stdin are **native byte order** of the machine (the two processes
always run on the same machine): the committed vectors are little-endian x86-64. `argv[0]` is the program name
and is skipped. "Message" means a stdout/stderr line of the helper; the exact libav-originated suffix of a message
(after `': ` in "Error opening ...") is informational, the prefix text is normative where stated.
`num`/`den` are the numerator/denominator of the video time base `{fps_den, fps_num}`.

## 3. Interface

### 3.1 Packet info structure (the only framing)

```c
enum ffm_packet_type { FFM_PACKET_VIDEO = 0, FFM_PACKET_AUDIO = 1, FFM_PACKET_CHANGE_FILE = 2 };  /* 4 bytes */

struct ffm_packet_info {
    int64_t  pts;       /* offset  0 */
    int64_t  dts;       /* offset  8 */
    uint32_t size;      /* offset 16  payload length in bytes that follows this structure */
    uint32_t index;     /* offset 20  audio track number (0-based); 0 for video */
    enum ffm_packet_type type;  /* offset 24, 4 bytes */
    bool     keyframe;  /* offset 28, 1 byte, value 0 or 1 */
};                      /* sizeof = 32, bytes 29..31 are padding */
```

Exit codes used by the helper: `FFM_SUCCESS = 0`, `FFM_ERROR = -1`, `FFM_UNSUPPORTED = -2`.
`script sizeof` prints `sizeof(info)=32 pts@0 dts@8 size@16 index@20 type@24 keyframe@28`.

### 3.2 Process model

Sender: spawn `obs-ffmpeg-mux` (located next to the OBS executable, `FFMPEG_MUX` name, with the platform's
executable suffix) with the argument vector of section 4, a **write-only pipe connected to its stdin**, and a
readable pipe on its stderr. The helper's stdout is not read by the sender. Receiver: reads stdin to EOF; on Windows
it switches stdin to binary mode and converts its wide-character argv to UTF-8. Arguments are UTF-8 on all
platforms.

## 4. Command line

Positional, all required unless marked optional. Notation: `<name>` one argv element. Numbers are decimal text.

```
obs-ffmpeg-mux <file> <has_video> <tracks>
   [if has_video == 1:]
      <vcodec> <vbitrate> <width> <height> <color_primaries> <color_trc> <colorspace> <color_range>
      <chroma_sample_location> <max_luminance> <fps_num> <fps_den> [<codec_tag>]
   [if tracks > 0:]
      <acodec>
      tracks x ( <name> <abitrate> <sample_rate> <frame_size> <initial_padding> <channels> )
   [<stream_key>] [<muxer_settings>]
```

| Field | Meaning |
|---|---|
| `file` | output path or URL (UTF-8) |
| `has_video` | `1` or `0` |
| `tracks` | number of audio tracks, `>= 0` |
| `vcodec` | libav codec **descriptor name** (e.g. `h264`, `hevc`, `av1`) |
| `vbitrate` | kbit/s, stored as `vbitrate * 1000` bit/s (64-bit multiplication) |
| `width`, `height` | pixels (also used as coded size) |
| `color_primaries`, `color_trc`, `colorspace`, `color_range`, `chroma_sample_location` | libav enum numbers, passed through unchanged (`AVCOL_*`, `AVCHROMA_LOC_*`) |
| `max_luminance` | cd/m2 integer, `0` = SDR |
| `fps_num`, `fps_den` | frame rate as a fraction; the video time base is `fps_den/fps_num` |
| `codec_tag` | optional; the codec tag as a signed integer (see 10.2); missing = 0 |
| `acodec` | libav codec descriptor name, **one codec for all audio tracks** |
| `name` | track title (stream metadata `title`) |
| `abitrate` | kbit/s |
| `sample_rate`, `frame_size`, `initial_padding`, `channels` | per track |
| `stream_key` | optional, `""` = none |
| `muxer_settings` | optional; `key=value` pairs separated by single spaces, may be `""` |

Rules:

* **S-037-R01** Arguments are consumed strictly in the order above; no flags, no quoting. Elements after
  `muxer_settings` are ignored (`10-atoi` / `09-args-optional`).
* **S-037-R02** Every numeric element is parsed like C `atoi`: optional leading white space, optional `+`/`-`,
  decimal digits, stop at the first other character; no digits gives 0; no error is ever reported for a malformed
  number (`10-atoi`). Senders MUST only send canonical integers; a receiver MAY be stricter only if it still
  accepts everything in `10-atoi` (values outside the `int` range are out of contract).
* **S-037-R03** Validation, in this order, after reading `file`, `has_video`, `tracks`: `has_video` not in {0,1} ->
  message `Invalid number of video tracks`; `tracks < 0` -> `Invalid number of audio tracks`;
  `has_video == 0 && tracks == 0` -> `Must have at least 1 audio track or 1 video track`. Each of these three is
  printed on **stdout** followed by an empty line (the reference uses `puts` of a text that already ends in a
  newline). Initialisation fails (R40) (`08-args-errors`).
* **S-037-R04** A missing element prints `Missing expected option: '<what>'` plus newline on stdout, where `<what>` is one of
  `file name`, `video track count`, `audio track count`, `video codec`, `video bitrate`, `video width`, `video height`,
  `video color primaries`, `video color trc`, `video colorspace`, `video color range`, `video chroma sample location`,
  `video max luminance`, `video fps num`, `video fps den`, `video codec tag`, `audio codec`, `audio track name`,
  `audio bitrate`, `audio sample rate`, `audio frame size`, `audio padding`, `audio channels`, `stream key`,
  `muxer settings`. It is fatal (initialisation fails) for every element except the last three groups: a missing
  `video codec tag` sets the tag to 0, a missing `stream key` is the empty key, a missing `muxer settings` is "no
  settings"; for those three the message is still printed and initialisation continues (`09-args-optional`, Q-3).
  Reading continues in order after a missing optional element, so a missing codec tag is followed by an attempt to read
  `acodec` (fatal if tracks > 0).
* **S-037-R05** `printable_file` = `file` with every occurrence of a non-empty `stream_key` replaced by the text
  `{stream_key}`. It is used in all messages that mention the file name; the real `file` is used for opening.
  The key is also kept for the helper's debug log filter (not observable). (`09-args-optional`)
* **S-037-R06** The argument vector is retained for the whole run: on a change-file packet (R37) the **same**
  arguments are used again with `file` replaced.

## 5. Stdin byte stream

The stream is a sequence of messages: `info` (32 bytes, section 3.1) followed by exactly `info.size` payload bytes.
There is no length prefix, magic number, checksum or version; message kind is determined by position and `type`.

Worked example (`01-layout`), little-endian. A video keyframe with `pts = 0x0102030405060708`, `dts = -2`, 3 payload
bytes `aa aa aa`:

```
08 07 06 05 04 03 02 01   pts
fe ff ff ff ff ff ff ff   dts (-2)
03 00 00 00               size
00 00 00 00               index
00 00 00 00               type (video)
01 00 00 00               keyframe=1 + 3 padding bytes (the sender writes zeros)
aa aa aa                  payload
```

A CHANGE_FILE message (type 2, payload = new file name bytes, **no terminator**, `size` = byte length, all other info
fields 0). Example for the name `next.mkv` after a 4-byte video header (`hdr video 0 0164001f`):

```
00..00 (16 bytes: pts, dts)  04 00 00 00 (size)  00 00 00 00 (index)  00 00 00 00 (type)  00 00 00 00 (keyframe+pad)
01 64 00 1f                      <- video header payload
00..00 (16 bytes)  08 00 00 00  00 00 00 00  02 00 00 00  00 00 00 00   <- info of CHANGE_FILE, size 8
6e 65 78 74 2e 6d 6b 76          <- "next.mkv"
```

An audio packet, track 1, `pts 1024`, `dts 1000`, key 0, payload `71 72`:
`00 04 00 00 00 00 00 00 | e8 03 00 00 00 00 00 00 | 02 00 00 00 | 01 00 00 00 | 01 00 00 00 | 00 00 00 00 | 71 72`
(the `01 00 00 00` at offset 24 is `type = audio`, the following word is key=0 + padding).

* **S-037-R10** The receiver reads in a loop until a read returns no data. A read of `n` bytes is repeated until all `n`
  bytes arrived or a read returns 0 (EOF/error); a short read is a failed read. (`14-packets-eof`)
* **S-037-R11** The receiver MUST NOT rely on the padding bytes and MUST ignore `keyframe`, `pts`, `dts` of header messages
  and of change-file messages (`11-headers`). The sender writes the whole structure with padding zeroed.
* **S-037-R12** `keyframe` is 0 or 1. Other byte values are undefined (the reference treats `bool` reads as UB).
* **S-037-R13** `size` may be 0. A zero-size data message is passed on to the muxer as an empty packet
  (`14-packets-eof`, last case: it is accepted and appears in the file). A zero-size header means "no extradata".
  Receivers SHOULD NOT impose a size limit smaller than what encoders produce; they MUST NOT trust `size` for
  allocation without checking it against the remaining input if reading incrementally.

## 6. Message order and lifecycle

```
stdin  := HEADERS ( DATA | CHANGE_FILE HEADERS )* EOF
HEADERS := [ video header if has_video ] [ audio header for track 0 .. tracks-1 ]     (has_video + tracks messages)
```

* **S-037-R20** Immediately after parsing the command line the receiver reads exactly `has_video + tracks` header
  messages. Header `k` is stored by **its own fields**, not by its position: `type == VIDEO` -> the video extradata;
  **any other type value** -> the extradata of audio track `index`. A later header for the same stream replaces the
  earlier one; a stream without header has empty extradata. Consequently the headers may come in any order
  (`11-headers`) but MUST be one per announced stream, and `index` MUST be `< tracks` for audio-typed headers (D-1).
  The sender sends video first (if present), then audio tracks 0..n-1.
* **S-037-R21** If any header cannot be read completely (EOF in the info or in the payload) initialisation fails
  (R40); nothing is created on disk (`12-headers-truncated`).
* **S-037-R22** After initialisation succeeded, the receiver reads messages until EOF. `type == CHANGE_FILE` -> R37.
  Any other `type` value -> a data message (R30).
* **S-037-R23 (shutdown)** EOF on stdin, whether between messages or inside a message, is the normal and only way to
  stop. The receiver finishes the current file (trailer, flush of the buffered output, close) and exits with status 0.
  There is no explicit "end" message (`14-packets-eof`: truncated info and truncated payload both give exit 0 and
  a finished file containing all earlier packets).
* **S-037-R24** A failure while muxing (R32) also ends the loop; the receiver then finishes the file and exits 0 (Q-1).
  The sender learns about failures only through a broken pipe or the stderr text.

## 7. Initialisation (`init`)

Order: parse command line (R01-R06) -> read headers (R20/R21) -> select container (R25) -> create streams (R26..R29)
-> open output and write the container header (R31). Any failing step gives the exit status/result of R40.

* **S-037-R25 (container)** A target is a *network target* if the **first characters of `file`** equal one of
  `srt`, `udp`, `tcp`, `http`, `rist` (case sensitive, plain prefix test on the whole string, so also a relative
  file named `udp_dump.mkv`, Q-2). Network targets other than `http*` use the **MPEG-TS** muxer (`mpegts`, MIME
  `video/M2PT`) whatever the name says; `http*` and all non-network targets pick the muxer from the file name
  (extension). No muxer found -> message `Couldn't find an appropriate muxer for '<printable_file>'` on stderr, `FFM_ERROR`
  (`19-format-selection`, `08`). Network initialisation of libav is done for network targets.
* **S-037-R26 (streams)** The video stream (if `has_video`) is created first, then the audio streams in track order.
  Every stream gets `id` = its index in the output (0-based, creation order), which is also the index used when
  writing. A stream that cannot be created is **skipped without error** (the other streams keep working and later ids
  shift down): video codec descriptor not found -> `Couldn't find codec '<name>'` (stderr); audio codec descriptor not
  found -> `Couldn't find codec descriptor '<name>'`; audio codec has no encoder in the library -> `Couldn't find codec
  '<name>'`; stream allocation failure -> `Couldn't create stream for encoder '<name>'`. If **no** stream at all could be
  created, initialisation fails with `FFM_ERROR` (D-2 for the crash in the reference). (`20-codecs-settings`)
* **S-037-R27 (video parameters)** The video codec parameters are, exactly:
  codec id = id of the descriptor named `vcodec`, type video; `codec_tag = codec_tag` (0 if missing);
  `bit_rate = vbitrate * 1000`; `width = coded_width = width`; `height = coded_height = height`; the five colour fields
  from the command line verbatim; `time_base = {fps_den, fps_num}`; stream time base = that same value (the muxer may
  change it when writing the header, R31); `avg_frame_rate = {fps_num, fps_den}`; extradata = video header payload
  (empty -> none); the "global header" codec flag is set iff the container requires global headers.
  (`03-inspect-video`, `04-inspect-hdr`)
* **S-037-R28 (HDR side data)** If `max_luminance > 0`, two items are added to the stream's coded side data, in this order:
  (1) content light level `MaxCLL = MaxFALL = max_luminance`; (2) mastering display metadata with primaries
  R=(17/25, 8/25), G=(53/200, 69/100), B=(3/20, 3/50), white point (3127/10000, 329/1000), min luminance 0/1,
  max luminance `max_luminance`/1, both `has_primaries` and `has_luminance` set. `max_luminance <= 0`: none
  (`04-inspect-hdr`).
* **S-037-R29 (audio parameters)** Per track `i`: stream metadata `title` = `name`; stream time base `{1, sample_rate}`
  (codec time base likewise); `sample_rate`; `bit_rate = abitrate * 1000` **unless the codec descriptor has the
  "lossless" property**, in which case the bit rate is left at the library default; `frame_size = frame_size` **unless the
  encoder has the "variable frame size" capability** (then left unset = 0); `initial_padding = initial_padding`
  (priming samples to skip); extradata = audio header `i`; global-header flag as for video; channel layout =
  library default layout for `channels`, except `channels == 5` which is **4.1** (`2.1`, `4.0`, `4.1`, `5.1`, `6.1`, `7.1`
  for 3,4,5,6,7,8; mono/stereo for 1/2) (`05`, `06`, `07`). The per-track `abitrate` of lossless codecs is ignored.
* **S-037-R30 (data message)** For a data message with `type` VIDEO or any other value except CHANGE_FILE: payload is read
  (R10); if the payload read is short -> the loop ends (R23). Routing: `type == VIDEO`: the video stream if one exists;
  otherwise the audio stream of track `index` if `index < number of created audio streams` (note: the number of
  *created* streams, so a skipped audio track shifts the track numbering). **Type values other than 0 and 2 are treated
  as audio** (Q-4). A message that matches no stream is consumed and silently dropped (`13-packets`: audio index 2, and
  `20-codecs-settings` first case).
* **S-037-R31 (open and header)** For containers that need a file: non-network -> the file is opened for writing
  (failure: `Couldn't open '<printable_file>', <strerror>` on stderr, `FFM_ERROR`) and written through an internal queue;
  network targets use the library's own protocol handlers (`Couldn't open '<printable_file>', <text>`). `muxer_settings` is
  parsed as `key=value` pairs split at **spaces** (key/value separator `=`); on a parse error stderr gets
  `Failed to parse muxer settings: <text>` + newline + the settings string + newline, and the settings are dropped (init
  continues, Q-13). When at least one pair parsed, stdout gets `Using muxer settings:` and then `\n\t<key>=<value>` for each pair,
  then `\n`. The pairs are passed to the muxer's header-writing call as options; unknown options are ignored.
  Header write failure -> stderr `Error opening '<printable_file>': <text>` (**no newline**), result `FFM_UNSUPPORTED`
  if the library error is "invalid argument" (-22 on POSIX), else `FFM_ERROR` (`10-atoi` last case: sample rate 0).

### 7.1 Stream-id and index summary

```
output stream ids:  [video?] audio0 audio1 ...   (only streams that were created, in this order)
```

## 8. Timestamps and keyframes

* **S-037-R32 (conversion)** For a routed data message, with `T = {num, den}` = the codec time base of the target stream
  (video: `{fps_den, fps_num}`; audio: `{1, sample_rate}`) and `S` = the **current** time base of the output stream (after the container
  header was written; it can differ from the one set in R27/R29):

  ```
  convert(v) = rescale_nearest( trunc_div(v, T.num), T, S )
  rescale_nearest(x, T, S) = x * T.num * S.den / (T.den * S.num), rounded to nearest, halves away from zero,
                             computed with at least 128-bit intermediates; x == INT64_MIN or INT64_MAX is returned unchanged
  pts_out = convert(info.pts); dts_out = convert(info.dts)
  ```
  `trunc_div` is C division (truncation toward zero). The sender emits video timestamps as **multiples of `fps_den`**
  (so `trunc_div` yields the frame count) and audio timestamps in samples. Values that are not multiples are truncated
  (`15-timestamps`: 1500 with num 1001 becomes frame 1). Example, 30000/1001, matroska (S = 1/1000): `pts = 15015` ->
  frame 15 -> 15 * 1001 / 30000 s = 500.5 ms -> **501** (`15-timestamps`); `pts = 1001` -> 33; `2002` -> 67. Audio 44100 Hz:
  `1024` -> 23, `2048` -> 46 (`44100` -> 1000). Negative values follow the same rules (`17-timestamps-edge`).
* **S-037-R33** The packet handed to the muxer has data = payload, size = `size`, `stream_index` = the stream id,
  `pts_out`, `dts_out`, and flags = KEY if `keyframe != 0`, else no flags. Duration is not set. The muxer interleaves
  (`av_interleaved_write_frame` semantics); the output order is the muxer's, not stdin's (`13-packets`).
* **S-037-R34 (write errors)** After the write call, if the library result is "invalid data" or "invalid argument" (-22) the
  packet is **dropped silently and processing continues** (`17-timestamps-edge`: pts < dts, `16`); any other negative result
  prints `av_interleaved_write_frame failed: <code>: <text>` + newline on stderr and ends the loop (R24).

## 9. Exit status, messages, shutdown

* **S-037-R40** Failed initialisation prints `Couldn't initialize muxer` (+ newline) on stderr and the process exits with the
  init result: `FFM_ERROR` (-1) -> exit status 255, `FFM_UNSUPPORTED` (-2) -> 254 (POSIX; the sender reads the status as a
  *signed* 8-bit value, so it sees -1/-2). The message text of the failing step comes first (R03, R04, R21, R25, R26, R31).
* **S-037-R41** After successful init the exit status is **always 0**, also after a mid-stream failure or a truncated stream
  (Q-1). Help for implementers: the sender must not treat exit 0 as "recording complete".
* **S-037-R42 (messages)** Messages go to stdout unless stated: R03, R04, the settings echo (R31). Everything else listed
  goes to stderr. The helper also installs a log callback that suppresses all libav logging (no output unless compiled with
  a debug option), so libav's own diagnostics never appear.
* **S-037-R43** The helper's stderr is unbuffered.
* **S-037-R44 (buffering of file output)** For non-network files the container bytes are queued and written by a writer
  thread in chunks of up to 1 MiB, honouring seek requests of the muxer by writing at the requested offset; the queue is capped
  at 256 MiB (the muxer thread blocks when full). On shutdown the queue is drained before the file is closed. A write error of
  the writer thread makes later muxer writes fail with a generic error (so R34 applies, "failed: -1"); an error that only occurs while draining at shutdown
  or in the final `fclose` is not reported (`22-output-write-error`, Q-11). Observable consequence for the sender: a file is only
  guaranteed complete after the helper exited.

### 9.1 Change-file (split recording)

* **S-037-R37** On a CHANGE_FILE message: read `size` bytes as the new file name (UTF-8, no terminator; any bytes including
  spaces are taken verbatim: `18-change-file`); finish the current output (trailer, drain, close; the trailer is written
  only when the previous init had succeeded); re-run the whole initialisation of section 7 with the retained argument vector
  where `file` is replaced by the new name - **including reading a fresh complete set of header messages from stdin** (R20),
  and re-parsing the arguments (so messages like `Missing expected option` repeat). The sender therefore sends
  `CHANGE_FILE, headers..., data...`. Streams, ids and conversion state are rebuilt from scratch.
  If the new initialisation fails: stderr `Couldn't initialize muxer` after the step's own message; the loop ends; exit status 0;
  the previous file stays complete (`18-change-file`). If the sender does not send headers, the next *data* message is consumed
  as a header (its payload becomes extradata; `18-change-file` 3rd case, Q-6).

## 10. Sender (OBS side) behaviour

(Not executable in the conformance suite, which cannot link the OBS core; derived from the source and the byte layout
vectors, which use the real `struct ffm_packet_info`.)

* **S-037-R50 (command line)** Built from the output's encoders: `file` = path (for network outputs the service connect URL);
  `has_video` = 1 iff a video encoder exists; `tracks` = number of consecutive audio encoders starting at track 0 (stops at the
  first missing one); `vcodec` = encoder's codec name; `vbitrate` = the encoder setting `bitrate` (int, kbit/s); `width`/`height`
  = output size; colour fields from the video output colour space:

  | OBS colour space | primaries | trc | colorspace |
  |---|---|---|---|
  | 601 | 6 (SMPTE170M) | 6 | 6 |
  | default, 709 | 1 (BT709) | 1 | 1 |
  | sRGB | 1 | 13 (IEC61966-2-1) | 1 |
  | 2100 PQ | 9 (BT2020) | 16 (SMPTE2084) | 9 (BT2020_NCL) |
  | 2100 HLG | 9 | 18 (ARIB_STD_B67) | 9 |
  | anything else | 2 (unspecified) | 2 | 2 |

  `color_range` = 2 (JPEG) for full range, else 1 (MPEG); `chroma_sample_location` by pixel format: 4:4:4 -> 2 (center), 4:2:2 -> 1 (left),
  4:2:0 -> 3 (top-left) when colorspace is BT2020_NCL else 1 (left), anything else 0; `max_luminance` = the nominal HDR peak for PQ,
  1000 for HLG, 0 otherwise; `fps_num`/`fps_den` from the video info. `codec_tag` = the encoder setting `codec_type` (a four-character
  code packed big-endian into an int, e.g. `"hvc1"`), **byte-swapped on little-endian machines** so that the helper's value is the
  libav (little-endian) fourcc (`09-args-optional`: sender int `875967080` is `0x34363268` = fourcc `h264`). `acodec` = codec of
  the **first** audio encoder; per track: `name` = encoder name, `abitrate` = encoder setting `bitrate`, `sample_rate`, `frame_size`,
  `initial_padding` = encoder priming samples, `channels` = channel count of the global audio output. `stream_key` = service key or `""`;
  `muxer_settings` = the stream's own settings, else the output setting `muxer_settings`, else `""` (always sent).
* **S-037-R51 (start)** For non-network outputs the sender first checks that the path can be created (create + delete) and fails with
  a "not writable" error otherwise; then starts the helper. If the helper cannot be started: last error "helper process failed".
  Headers are **not** sent at start but lazily before the first data packet (`sent_headers` flag): `send_headers` = video extradata
  (as header, type VIDEO, index 0) then each audio encoder's extradata (type AUDIO, index = track). If an encoder has no extradata
  the sender aborts the headers (and the packet) - there is no empty header substitute.
* **S-037-R52 (data)** `write_packet`: one write of the 32-byte info, one write of the payload; `size` = payload length, `index` = track index
  (0 for video), `type` VIDEO/AUDIO, `keyframe`. In split-file mode `pts`/`dts` have the per-stream offsets subtracted (video offset, per-track
  audio offsets) so each file starts near 0. Any short write -> failure handling (R54). Counters: total bytes += payload size.
* **S-037-R53 (split)** To change files the sender writes CHANGE_FILE (R37 layout), then `send_headers`, resets its size/time counters
  and offsets, and signals "file changed" to listeners with the new path before sending the headers.
* **S-037-R54 (failure)** On a failed write: read up to 1023 bytes of helper stderr (non-empty -> logged and set as the output's last error),
  close the pipe (EOF) and wait for the helper, map its (signed) exit status: `-2` -> "unsupported" stop code, otherwise
  network outputs -> "disconnected", files -> "encode error".
* **S-037-R55 (stop)** A normal stop closes the pipe (stdin EOF = R23) after the stop timestamp is reached (the first packet whose system dts is
  at or after the stop time ends the recording), then waits for the helper process.

## 11. Concurrency

The receiver is single-threaded for the protocol; the only extra thread is the file writer of R44 (queue + mutex + two events).
The sender may write from the encoder callback thread; HLS and replay buffer modes use their own threads but write to the pipe
from one thread at a time. Writes to the pipe are blocking; the receiver reads sequentially, so a stalled helper back-pressures
the sender.

## 12. Reference defects and quirks

| ID | Kind | Reference behaviour | Required behaviour | Script |
|---|---|---|---|---|
| Q-1 | QUIRK | After a successful init the exit status is 0 even if muxing failed or stdin was cut. | Keep (sender relies on stderr and pipe errors). | 14, 17, 18 |
| Q-2 | QUIRK | Network detection is a prefix test on `file`; relative files named `udp...`, `tcp...`, `srt...`, `rist...`, `http...` are network targets (MPEG-TS for non-http). | Keep. | 19 |
| Q-3 | QUIRK | Missing stream key / muxer settings / codec tag only print a message. | Keep (the sender always sends all of them, but older callers may not). | 09 |
| Q-4 | QUIRK | Headers and data are routed by `type`: anything not VIDEO (including unknown values) counts as audio. | Keep. | 13 |
| Q-5 | QUIRK | Streams that cannot be created are silently skipped; video packets for a missing video stream are dropped. | Keep. | 20 |
| Q-6 | QUIRK | After CHANGE_FILE exactly `has_video + tracks` messages are read as headers regardless of their `type`. | Keep. | 18 |
| Q-7 | QUIRK | Some stderr messages (`Error opening ...`) have no trailing newline and run into the next message. | Keep text; MAY add newline only if the sender does not parse it (it only logs it). | 08, 10 |
| Q-8 | QUIRK | `pts`/`dts` are divided by the time base numerator before rescaling; non multiples lose their remainder. | Keep exactly (R32). | 15, 17 |
| Q-9 | QUIRK | Invalid-data / invalid-argument write failures are ignored per packet. | Keep. | 17 |
| Q-10 | QUIRK | The three validation messages end with an empty line (puts + newline). | Keep. | 08 |
| Q-11 | QUIRK | Write errors surfacing only while draining/closing the file at shutdown are not reported; exit status 0. | Keep. | 22 |
| Q-12 | QUIRK | The stream key replacement in `printable_file` replaces every substring occurrence (also in the directory part). | Keep. | 09 |
| Q-13 | QUIRK | Unparsable muxer settings are reported and ignored, init continues. | Keep. | 20 |
| D-1 | DEFECT | A header message with `type != VIDEO` and `index >= tracks` (or an audio-typed header when `tracks == 0`, or a video-typed header when `has_video == 0` is harmless) writes outside the audio header table: NULL dereference (`signal 11`) when `tracks == 0`, heap corruption otherwise. | Fail initialisation with `FFM_ERROR` and print a message. Also reject `type == CHANGE_FILE` headers. | 21 |
| D-2 | DEFECT | If no stream could be created (non-network target), cleanup dereferences the not-yet-opened output I/O context: `signal 11` instead of exit status 255. | Exit status 255 (`FFM_ERROR`) after `Couldn't initialize muxer`. | 21 |
| D-3 | DEFECT | `fps_den == 0` makes the timestamp conversion divide by zero (SIGFPE) and `fps_num == 0` an invalid time base (not executed: the muxer may reject earlier). | Reject `fps_num <= 0` or `fps_den <= 0` at init with `FFM_ERROR`. | (untested) |
| D-4 | DEFECT | `malloc`/`realloc` results for `size`-sized buffers are not checked; a corrupted or hostile `size` (up to 4 GiB) aborts or crashes. | Fail the read (treat as EOF) when allocation fails. | (untested) |

## 13. Implementation notes (non-normative)

* A Rust helper needs bindings to libavformat (e.g. `ffmpeg-next` or hand-written FFI); the protocol layer (R10, R20, R22-R24, R30, R32,
  R37) is pure and testable without libav: model it as `Reader` -> `enum Msg { Header, Packet, ChangeFile }` and keep a trait for the muxer.
* Use `i128` for `rescale_nearest`; round-half-away: `(n + sign*d/2)/d` with the sign of `n` (denominators are positive).
* Reading the info: a 32-byte array and `u64/i64/u32::from_ne_bytes`; do not transmute (padding, enum validity: map the 4-byte `type`
  with a fallback for unknown values -> audio-like, R30).
* The writer-thread queue (R44) is a performance detail; any implementation that keeps the final file identical and drains on exit is
  acceptable. Seeks come from the muxer (mp4/mkv index back-patching).
* Run the helper with stdin in binary mode; on Windows convert arguments from UTF-16 to UTF-8.
* The sender can be `std::process::Command` with `Stdio::piped()` for stdin and stderr; read stderr only after a failure (R54) to avoid
  blocking on a full pipe, or drain it on a thread.

## 14. Conformance coverage

All vectors drive the **real receiver** (`ffmpeg-mux.c` compiled into the runner, run in a child process) with scripted stdin bytes and
arguments, linked against libavformat 63.1.102 (dev headers present on the authoring machine; no part had to be left unlinked on the
receiver side). Parts that need the OBS core (section 10, sender) cannot be linked: they are covered by the byte-layout vectors of
`01-layout` (which print a message built with the real `struct ffm_packet_info`) and by reading the source. Comparison: exit status and
our own message lines exactly; libav-derived text (error strings after `': `, probe output such as stream time bases `1/51200`
for nut, `1/15360` for mp4, `1/1000` for matroska, mkv start shifts) depends on the FFmpeg version and muxer; an implementation that
uses libavformat compares equal, one that does not should compare packet payloads, order, and relative timestamps in the stream time base.

| Rule | Scripts |
|---|---|
| R01, R02 | 02, 10 |
| R03 | 08 |
| R04 | 08, 09 |
| R05, R06 | 09, 18 |
| R10 | 01, 12, 14 |
| R11, R12, R13 | 01, 11, 14 |
| R20 | 11, 21 |
| R21 | 12 |
| R22, R23 | 14, 18 |
| R24 | 17, 22 |
| R25 | 19 |
| R26 | 13, 20 |
| R27 | 03, 09 |
| R28 | 04 |
| R29 | 05, 06, 07 |
| R30 | 13, 20 |
| R31 | 10, 20 |
| R32 | 15, 16, 17 |
| R33 | 02, 13, 16 |
| R34 | 17 |
| R37 | 18 |
| R40 | 08, 12, 18, 19, 21 |
| R41 | 14, 17, 22 |
| R42 | 08, 09, 20 |
| R43 | (not observable) |
| R44 | 22 (partial) |
| R50-R55 | source analysis only (sender not linkable) |
| D-1, D-2 | 21 |

## 15. Open questions

* Whether a conforming Rust helper must keep the exact `Using muxer settings:` echo (stdout is never read by the sender in the reference; it is
  only visible when the helper is started by hand).
* The behaviour for `index >= number of created audio streams` mixing with *skipped* streams (R30) is consequence of the reference's indexing by
  created streams; it is untested because it needs an encoder-less codec between valid tracks with a shared codec name (all tracks share one codec,
  so either all audio streams exist or none does): in practice the case cannot arise.
* Windows exit-status mapping of -1/-2 (`DWORD` 0xFFFFFFFF/0xFFFFFFFE) and its interpretation by the sender were not verified.
* D-3 and D-4 were not executed.
* The Rust sufficiency check was skipped on instruction (cost control); the spec was written from the C and validated only against the
  real receiver.
