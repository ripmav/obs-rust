# S-059 `mpegtsout`: native MPEG-TS network output (SRT / RIST / UDP / TCP)

| | |
|---|---|
| Status | Ready for review (Rust sufficiency check skipped on request, see 11) |
| Reference | libobs 33.0.1, `plugins/obs-ffmpeg/obs-ffmpeg-mpegts.c`, `obs-ffmpeg-srt.h`, `obs-ffmpeg-rist.h`, `obs-ffmpeg-url.h`, `obs-ffmpeg-output.h` (struct layouts) |
| Depends on | S-046 (registration context; this spec replaces its "out of scope" note), S-037 (not used here: no helper process), S-014 (obs_data) |
| Conformance | `test/conformance/scripts/mpegtsout/` (32 scripts), runner `gen/mpegtsout_runner.c` |

## 1. Scope

Output id `ffmpeg_mpegts_muxer` (built when `ENABLE_NEW_MPEGTS_OUTPUT`, the default): takes **encoded** H.264 (the code maps every
other video codec name to AV1) and AAC/Opus packets from libobs and sends them as an MPEG-TS stream over SRT, RIST (own thin
clients written against libsrt / librist), or UDP/TCP/HTTP (libavformat protocols). The MPEG-TS multiplexing itself is done by
libavformat; this spec does not define TS bytes, only what the output feeds into the muxer and how it reaches the network.
Not covered: libsrt / librist / libavformat internals, the frontend UI, the older helper-process variant (S-046).

## 2. Interface

`struct obs_output_info`: id `ffmpeg_mpegts_muxer`, flags `AV|ENCODED|MULTI_TRACK|SERVICE` = 0x1f (no pause), protocols
`SRT;RIST`, video codecs `h264` (`h264;hevc` only if `ENABLE_HEVC`; not defined in the tested build), audio `aac;opus`,
name key `FFmpegMpegts`, callbacks create / destroy / start / stop / encoded_packet / get_total_bytes / get_properties.
Properties: one text property `path` (description key `FilePath`, `OBS_TEXT_DEFAULT`). Settings read: `muxer_settings`
(string, default `""`, written back as default by the output). Everything else comes from the **service** (connect info:
server URL, username, password, stream id, encrypt passphrase) and the encoders (settings `bitrate`, `keyint_sec`).

## 3. Rules

### 3.1 Start (runs on a short-lived "start/stop thread")

* **S-059-R01** `start()` returns false (no thread) if `can_begin_data_capture(0)` or `initialize_encoders(0)` is false. Otherwise
  it joins a previous start/stop thread and write thread, zeroes `audio_start_ts`, `video_start_ts`, `total_bytes`, clears
  `got_headers`, spawns the thread and returns true. Failures inside the thread are reported only via stop signals.
* **S-059-R02** Configuration (`set_config`): no service -> stop code `ERROR` (-4) and `stop(false, 0)`. Otherwise url,
  username, password, stream id, passphrase are taken verbatim (NULL stays NULL); format name `mpegts`, mime `video/M2PT`.
  `is_rist` = URL starts with `rist` (case sensitive), `is_srt` = starts with `srt`. `srt_pkt_size` = integer (strtol base 10,
  garbage -> 0, negatives kept) of query tag `payload_size`, else `pkt_size`, looked up after the first `?` (0 if none).
* **S-059-R03** Video: width/height = output size (scale = same). Codec id H264 iff the encoder codec string is `h264`, else AV1.
  Pixel format via `obs_to_ffmpeg_video_format`; NONE -> warning "Invalid pixel format used for mpegts output", code `ERROR`.
  Range full -> JPEG else MPEG. Colour: 601 -> SMPTE170M primaries/trc/matrix; 709 and DEFAULT -> BT709 x3; SRGB -> BT709
  primaries + IEC61966-2-1 trc + BT709; PQ -> BT2020/SMPTE2084/BT2020NCL; HLG -> BT2020/ARIB-STD-B67/BT2020NCL (these switch
  results override the "RGB" matrix assignment before it). Bitrate = encoder setting `bitrate`; GOP = `keyint_sec * fps_num /
  fps_den` (integer), 250 if `keyint_sec` is 0.
* **S-059-R04** Audio: tracks counted by probing `get_audio_encoder(i)` until NULL; codec/frame size from track 0 (id AAC for
  `aac`, OPUS for `opus`, otherwise unset); per-track bitrate setting `bitrate`. **D-3** with zero audio encoders track 0 is
  dereferenced (crash); at least one audio encoder is a precondition.
* **S-059-R05** `muxer_settings` setting is passed as muxer options; `protocol_settings` is always `""`.
* **S-059-R06** Open (`open_output_file`) order: create video stream (needs video info, codec descriptor), then one audio stream
  per track (needs audio info, descriptor), failure -> "mpegts avstream failed to be created", code `ERROR`. The AVFormatContext
  is `mpegts` with metadata service_provider `obs-studio`, service_name `mpegts output`; flag AUTO_BSF is set.
* **S-059-R07** URL dispatch: `rist` -> RIST client, `srt` -> SRT client (R20-R47), `udp`/`tcp`/`http` prefix -> `avio_open2`
  (QUIRK Q-5: failure code is the raw negative AVERROR, e.g. -111, passed to the stop signal), anything else -> `BAD_PATH`
  with info log "Invalid protocol". After SRT/RIST open a custom AVIO context is created: buffer = `max_packet_size` of the URL
  context (SRT: the socket's payload size read back; RIST: 1316), write-only, opaque = URL context. Failure -> `ERROR`.
* **S-059-R08** On success: write thread is created, `total_bytes := 0`, `begin_data_capture` is called, then `running := true`
  and `stopping := false`. The AVIO write callbacks are `libsrt_write` / `librist_write`.
* **S-059-R09** Any failure in the thread: stop signal with the code, then `stop(signal=false, 0)`. **D-4** resources opened
  before the failure (format context, streams) are not released because `stop` only frees when the output is active; a later
  start overwrites them.

### 3.2 Packets and write thread

* **S-059-R10** The first `encoded_packet` call (any packet, also NULL) obtains headers: video extradata and every audio track's
  extradata from the encoders (copied as codec extradata). Missing -> warning "Failed to retrieve headers", code
  `INVALID_STREAM`. Then `avformat_write_header` with the `muxer_settings` options (`key=value` pairs separated by spaces; parse
  error or header error -> "Failed to write headers", `INVALID_STREAM`; unknown keys are logged at info). Both failures
  signal and start an internal stop (no second signal). QUIRK Q-6: `got_headers` stays true after a failed header write.
  A packet delivered before any start is outside the preconditions (NULL dereference, not specified).
* **S-059-R11** If not active (`running && write thread active`) the packet is dropped. NULL packet (encoder failure) -> signal
  `ENCODE_ERROR`, internal stop. If `stopping` and `packet.sys_dts_usec >= stop_ts` (QUIRK Q-3: microseconds compared with a
  nanosecond value) -> internal stop.
* **S-059-R12** Conversion (`mpegts_write_packet`): dropped if `stopping`, no video stream / context, or no audio stream list.
  Payload copied; `stream_index` = stream id (video 0, audio track i -> 1+i); `pts`/`dts` = `rescale(v / tb.num, tb, stream_tb,
  NEAR_INF|PASS_MINMAX)` where `tb` = codec time base (video `fps_den/fps_num`, audio `1/sample_rate`); keyframes flagged.
  **D-2** the integer division by `tb.num` breaks fractional frame rates (29.97 fps: `num`=1001, small frame indexes become 0).
  Required behaviour: rescale the frame count without the division. Queue push + semaphore post.
* **S-059-R13** Write thread loop: wait semaphore; exit if the stop event is signalled; pop one packet; if `stopping` and the
  packet's system time (`pause_offset + dts in ns`, start timestamps are always 0) `>= stop_ts` drop it; else `total_bytes +=
  size` and write interleaved. Error `INVALIDDATA` or `-EINVAL` -> ignored (packet lost). Other error -> exit loop after
  signalling: SRT: socket state BROKEN or NONEXIST -> `DISCONNECTED` else `ERROR`; RIST -> `DISCONNECTED`; others -> `ERROR`.
  The thread sets `stopping := true` when it exits (so the next encoded packet triggers an internal stop; `running` stays true).
* **S-059-R14** `get_total_bytes` = sum of payload sizes handed to the muxer (not bytes on the wire).

### 3.3 Stop and destroy

* **S-059-R15** `stop(ts)` (and destroy) queue a stop on the start/stop thread: `stop(signal, ts)`: if active: when `ts > 0` set
  `stop_ts := ts`, `stopping := true` (QUIRK Q-4: no draining happens, full stop follows immediately: write thread stopped, queued
  packets freed unsent). Full stop: write thread joined, trailer written if initialised, SRT/RIST closed (`libsrt_close` /
  `librist_close`, then AVIO freed), format context freed, `ff_data` zeroed. Then, if `running` and signal: stop signal
  `SUCCESS` and `running := false`. Internal stops (signal=false) leave `running` true.
* **S-059-R16** `destroy` = `stop(0)` + wait for the thread + free. Joins of the start/stop thread happen only while its active flag
  is set; the thread clears the flag itself (Q-7: the handle is usually not joined).

### 3.4 SRT client (`libsrt_open` ...)

* **S-059-R20** `libsrt_open`: `srt_startup` (failure -> `ERROR`, nothing else); defaults (list in 3.5); `last_error := SUCCESS`;
  query parsed after the first `?` (tags in 3.5); then `libsrt_setup`; on setup failure the stored OBS code is returned; after setup
  the socket state must be CONNECTED else error log, `libsrt_close`, stored code or `CONNECT_FAILED`. Error exits call `srt_cleanup`.
* **S-059-R21** Setup: URL split; scheme must be `srt` (else `BAD_PATH`), port 1..65535 (else `BAD_PATH`, "Port missing in uri");
  `timeout` and `listen_timeout` tags (strtoll); `timeout >= 0` sets `h->rw_timeout` and the connect timeout; resolve host
  (empty host -> NULL; passive flag for listeners); failure -> `BAD_PATH`. Candidates are tried in order.
* **S-059-R22** Per candidate: create socket, set PRE options (order in 3.6), UDP buffers if configured (>0), non-blocking
  (`SNDSYN=0`, `RCVSYN=0`). Caller: epoll(write), connect, wait for writable (bounded by `timeout`), state check. Listener:
  epoll(read), `REUSEADDR=1` (failure only warns), bind, listen(1), wait (bounded by `listen_timeout`), accept, close the listening
  socket, continue with the accepted one (read back STREAMID, info log). Rendezvous: needs `localip` and `localport` (1..65535)
  else `BAD_PATH`; `SRTO_RENDEZVOUS=1`; bind `localip` with the REMOTE port (Q-1: `localport` is only validated).
* **S-059-R23** Failure handling: set-option failure, connect failure, wait failure -> next candidate (retry) if any, else return;
  interrupt -> `CONNECT_FAILED`; failure in epoll create, accept, or readback of payload size does not retry.
  Stored codes: most connect-phase failures end as `ERROR`; `libsrt_neterrno`'s mapping (ECONNREJ + bad secret -> `BAD_PATH`,
  other rejects/ECONNSOCK -> `CONNECT_FAILED`, ESECFAIL -> `INVALID_STREAM`, ECONNLOST/ENOCONN/ECONNFAIL -> `DISCONNECTED`,
  ENOSERVER/ETIMEOUT -> -10, else `ERROR`) is overwritten by `ERROR` in setup (Q-2) and only survives for a failing rendezvous bind
  (`CONNECT_FAILED`). Errno of the failed call becomes the negative errno return of `libsrt_neterrno`, else `AVERROR_UNKNOWN`.
* **S-059-R24** POST options after connect: `INPUTBW` (int64) and `OHEADBW`; applied always (defaults 0 and 25); failure ->
  `ERROR`. Then `SRTO_PAYLOADSIZE` is read back; if > 0 it becomes `max_packet_size`. A write-side epoll is created; its failure is `ERROR`.
* **S-059-R25** `libsrt_write(buf, n)`: wait writable (epoll, 100 ms polls, bounded by `rw_timeout` when > 0 -> `-ETIMEDOUT`;
  wait errors returned as is; EAGAIN mapping for async codes), `srt_send`; error -> `libsrt_neterrno`; success: every 60 s of
  wall clock the RTT / bandwidth are logged at debug. Returns `n`.
* **S-059-R26** `libsrt_close`: frees streamid and passphrase, logs the session summary (elapsed s, mean speed, total MB, retransmit %
  and drop %), releases the epoll, `srt_close`; on failure logs and returns -1 (no cleanup, no final log); else `srt_cleanup`, 0.
  Q-8: both percentages use integer division before `* 100.0` and are 0.0 unless retransmitted/dropped >= sent.
* **D-1** On connect failure the caller `connect_mpegts_url` closes again (`libsrt_close` on the failed context): second
  epoll release, `srt_close(0)`, unbalanced `srt_cleanup`. Required behaviour: release once.

### 3.5 SRT defaults and query tags

Defaults: rw_timeout, listen_timeout, send/recv buffer, maxbw, pbkeylen, mss, ffs, ipttl, iptos, latency, rcvlatency,
peerlatency, tlpktdrop, nakreport, connect_timeout, sndbuf, rcvbuf, lossmaxttl, minversion, messageapi, linger, tsbpd = -1
(unset); payload_size 1316; inputbw 0; oheadbw 25; mode caller; transtype live; smoother NULL. **Q-9** `enforced_encryption`,
`kmrefreshrate`, `kmpreannounce` and `snddropdelay` have no default (zero) so they are always applied as 0.
Tags (integer = strtol/strtoll base 10 unless noted): `maxbw pbkeylen passphrase enforced_encryption kmrefreshrate kmpreannounce
snddropdelay mss ffs ipttl iptos inputbw oheadbw latency tsbpddelay(alias of latency, wins) rcvlatency peerlatency tlpktdrop nakreport
connect_timeout payload_size|pkt_size mode(caller|listener|rendezvous, else ERROR) sndbuf rcvbuf lossmaxttl minversion(base 0)
streamid(replaces the service value) smoother messageapi transtype(live|file, else ERROR) linger localip localport`. Service
`stream_id` / `encrypt_passphrase` are copied into the context first when non-empty (empty -> unset).

### 3.6 PRE option order and conversions

`RENDEZVOUS`(rendezvous only), `TRANSTYPE`(if not invalid), `MAXBW`, `PBKEYLEN`, `PASSPHRASE`, `ENFORCEDENCRYPTION`,
`KMREFRESHRATE`, `KMPREANNOUNCE`, `SNDDROPDELAY`, `MSS`, `FC`(ffs), `IPTTL`, `IPTOS`, `LATENCY`, `RCVLATENCY`, `PEERLATENCY`, `TLPKTDROP`,
`NAKREPORT`, `CONNTIMEO`, `SNDBUF`, `RCVBUF`, `LOSSMAXTTL`, `MINVERSION`, `STREAMID`, `CONGESTION`(smoother), `MESSAGEAPI`,
`PAYLOADSIZE`, `SENDER`(always 1), `TSBPDMODE`, then `LINGER` (`onoff = linger > 0`). Each is applied only if its value passes `>= 0`
(`SNDDROPDELAY`: `>= -1`; `PASSPHRASE`/`STREAMID`/`CONGESTION`: non-NULL). Latency family: microseconds / 1000 (integer);
`snddropdelay` / 1000 only when > 0; connect_timeout passed unchanged. Version note: SRT >= 1.4.1 constants are used
(`ENFORCEDENCRYPTION`, `CONGESTION`); older headers use `STRICTENC` / `SMOOTHER`.

### 3.7 RIST client

* **S-059-R40** `librist_open`: buffer_size 3000 ms, profile MAIN, packet_size 1316, log level INFO, encryption 0, stats interval
  60000 ms; logging set up (failure -> `ERROR`); `max_packet_size := 1316` (the URL's packet size is ignored); sender created (flow 0)
  (failure -> `ERROR`); peer config defaults; address parsed by librist (`rist_parse_address2`; failure -> close, `BAD_PATH`).
* **S-059-R41** Encryption check: if (`encryption` 128/256 and no service secret) or (URL `key_size` 128/256 and empty URL secret) ->
  error log, close, `INVALID_STREAM` (key size 192 is not checked, Q-10). Service secret fills an empty URL secret; with context
  encryption 128/256 (never set) key_size would follow. `recovery_length_min/max := buffer_size` (3000) overrides the URL's
  `buffer`. Service username/password fill empty URL SRP credentials.
* **S-059-R42** Peer create, start (failures -> close, `ERROR`); stats callback registration failure only logs. Returns 0.
* **S-059-R43** `librist_write`: `rist_sender_data_write`; negative -> warning, `RIST_ERR_MALLOC` -> `-ENOMEM`, else `AVERROR_EXTERNAL`.
* **S-059-R44** `librist_close`: frees secret/username/password, destroys the context; failure -> error log, -1.
* **S-059-R45** Stats callback: always logs the JSON at INFO via librist logging; for sender-peer stats a debug summary
  (bandwidth Mbps, sent, received, retransmitted, quality, rtt); always frees the block. Precondition: the libav log callback
  (set by create) is installed, otherwise librist's logging dereferences the URL context as an AVClass.

### 3.8 Properties

* **S-059-R50** `get_properties` returns a property list with the single text property `path` (R2).

## 4. Defects and quirks

| ID | Kind | Summary |
|---|---|---|
| D-1 | DEFECT | double close/cleanup after failed SRT connect (3.4) |
| D-2 | DEFECT | fractional frame rates: pts divided by time-base numerator (R12) |
| D-3 | DEFECT | zero audio encoders dereferences NULL (R04) |
| D-4 | DEFECT | resource leak after failed start (R09) |
| Q-1 | QUIRK | rendezvous bind uses the remote port |
| Q-2 | QUIRK | SRT error mapping mostly dead (ERROR overwrites) |
| Q-3 | QUIRK | stop_ts unit mismatch (us vs ns) |
| Q-4 | QUIRK | stop(ts>0) never drains |
| Q-5 | QUIRK | raw AVERROR as stop code for udp/tcp/http open failure |
| Q-6 | QUIRK | got_headers true after failed header write |
| Q-7 | QUIRK | start/stop thread handle normally not joined |
| Q-8 | QUIRK | integer-division percentages in the SRT session summary |
| Q-9 | QUIRK | four SRT options always applied as 0 |
| Q-10 | QUIRK | RIST key size 192 unchecked; `hevc` mapped to AV1 id (R03) |

## 5. Environment notes (conformance)

libsrt is replaced by a scripted fake implementing the used API (headers 1.5.7 exist; no network is touched). librist's address
parser and logging are the real library (0.2.x, API 4.13: very lenient parsing, defaults dependent on the library version); its
sender is a shim. UDP and TCP use real loopback sockets opened by the runner. libavformat's MPEG-TS muxer is real: TS output is
verified structurally (188-byte packets, sync, continuity, PAT -> PMT 0x1000, PCR pid 0x100, stream types 0x1b / 0x0f (AAC) /
0x06 (Opus), PES stream ids 0xe0 / 0xc0 / 0xbd, PES timestamp spacing in 90 kHz) and not byte for byte. Log lines containing library
versions or libc resolver texts are filtered.

## 6. Coverage of rules by scripts

| Rules | Scripts |
|---|---|
| R01, R02 | 01, 02, 35 |
| R03 | 03, 04, 43 |
| R04, R05 | 05, 35, 43 |
| R06, R07, R08, R09 | 30-37 |
| R10, R11 | 36, 39 |
| R12 | 30, 31, 33, 34, 42 |
| R13 | 38, 41 |
| R14, R15, R16 | 30, 40, 41 |
| R20-R26, 3.5, 3.6 | 10-18, 36, 37 |
| R40-R45 | 20-22, 31, 36, 37 |
| R50 | 00 |

## 7. Not covered / open questions

Bytes of the TS stream (libavformat version dependent), SRT behaviour against a real peer, HEVC build option, Windows (WSAStartup
paths), `get_audio_headers` failures for secondary tracks beyond the first (covered by script 35 style for track 0 only), the
`ENABLE_NEW_MPEGTS_OUTPUT=OFF` variant (S-046).
