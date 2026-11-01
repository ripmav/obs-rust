# S-045 RTMP stream output and network-interface helpers

| | |
|---|---|
| Status | Ready for review (Rust sufficiency check NOT performed, by coordinator instruction) |
| Reference | libobs 33.0.1 tree, `plugins/obs-outputs/rtmp-stream.c`, `rtmp-stream.h`, `net-if.c`, `net-if.h` |
| Target crate | `crates/obs-rtmp-output` (modules `settings`, `queue`, `dbr`, `connect`, `sender`, `netif`) |
| Depends on | S-021 (FLV/AMF0 muxing: the `flv_*` calls are inputs here), S-024 (happy-eyeballs address ordering: not repeated), S-031 (RTMP protocol engine: the `RTMP_*` calls are inputs here), S-014/S-015 (settings, properties), S-041 (encoder/output delay) |
| Conformance | `test/conformance/scripts/rtmpstream/` (30 scripts), runner `gen/rtmpstream_runner.c` |

## 1. Scope

The `rtmp_output` output type: it receives encoded packets from libobs, buffers them in a queue, drops frames when the queue
holds too much time, optionally lowers the encoder bitrate instead (dynamic bitrate, DBR), connects with the RTMP engine
(S-031), sends FLV tags (S-021) from a send thread, maps connection errors to user messages, and reports statistics. Also
`net-if.c`: the interface list for the "bind to IP" combo and textual address conversion.

Out of scope: RTMP wire protocol (S-031), FLV/AMF0 bytes (S-021), happy-eyeballs resolution order (S-024; the output only
passes a bind address / family hint to the engine), the Windows-only socket thread (`rtmp-windows.c`) and its
`new_socket_loop` behaviour (only the part of `init_send` that runs before the platform check is specified, R41), TLS.

## 2. Terminology

`track` = video/audio encoder index. Times: `dts_usec`/`sys_dts_usec` microseconds, `*_ns` nanoseconds, bitrates kbit/s.
"Output" = the libobs output object that owns the stream. All thresholds below are in the units shown. Log text is normative
only where stated; levels are normative.

## 3. Interface

Output registration record (R01): id `rtmp_output`; flags `AV | ENCODED | SERVICE | MULTI_TRACK_AV` (value 0x5f in the
conformance build); protocols `"RTMP;RTMPS"` (`"RTMP"` only when built without crypto); encoded video codecs `"h264;hevc;av1"`
(`"h264;av1"` without HEVC support); encoded audio codecs `"aac"`. Callbacks: create, destroy, start, stop(ts_ns),
encoded_packet, get_defaults, get_properties, get_total_bytes, get_congestion, get_connect_time_ms, get_dropped_frames.

Settings keys (strings are normative): `dyn_bitrate` (bool), `interpolation_table_data` (string, JSON), `drop_threshold_ms` (int),
`pframe_drop_threshold_ms` (int), `max_shutdown_time_sec` (int), `bind_ip` (string), `ip_family` (string),
`new_socket_loop_enabled` (bool, Windows), `low_latency_mode_enabled` (bool, Windows). Constants: `MAX_OUTPUT_VIDEO_ENCODERS = 10`,
`MAX_OUTPUT_AUDIO_ENCODERS = 6`, DBR increase timer 4 s, DBR trigger 200 ms of queued video, estimate window 1000..2000 ms.

Output result codes (signalled via stop): 0 success, -1 bad path, -2 connect failed, -3 invalid stream, -4 error, -5 disconnected,
-8 encode error, -9 HDR disabled.

Stream state (fields that matter): packet queue (FIFO of packets), `min_priority`, `congestion` (f32), `last_dts_usec`,
`dropped_frames` (i32), `total_bytes_sent` (u64), thresholds `drop_threshold_usec`, `pframe_drop_threshold_usec` (i64), flags
`connecting/active/disconnected/encode_error` (atomic), `sent_headers`, `got_first_packet`, `start_dts_offset` (ms),
`stop_ts` (us), `shutdown_timeout_ts` (ns), `max_shutdown_time_sec`, `addrlen_hint`, DBR: `dbr_enabled`, `dbr_cur/orig/prev/est/inc_bitrate`
(`long`), `audio_bitrate`, `dbr_data_size`, `dbr_inc_timeout` (ns), window `dbr_frames` (begin ns, end ns, size), table.

## 4. Data formats

DBR interpolation table JSON: array indexed by encoder: `[[e0 level0, e0 level1, ...], [e1 level0, ...]]`. Transposed into
`table[level].bitrates[encoder]` (10 `long` slots per level). Example (vector 05): `[[0,3000,4000],[0,1500,2000]]` gives
`[0] 0,0 / [1] 3000,1500 / [2] 4000,2000`.

Interface item: display name `"[ADAPTER] ADDR"` (or `ADDR` when the adapter name is empty/NULL) and value `ADDR`.

Fake FLV tag names used by the vectors (`mux`, `vstart`, ...) are test-only.

## 5. Behaviour (numbered rules)

### 5.1 Registration, defaults, properties
- **S-045-R01** The registration record is as in section 3 (vector 01).
- **S-045-R02** Defaults (written only when no user value): `drop_threshold_ms`=700, `pframe_drop_threshold_ms`=900,
  `max_shutdown_time_sec`=30, `bind_ip`="default"; Windows additionally `new_socket_loop_enabled`=false, `low_latency_mode_enabled`=false.
- **S-045-R03** Properties in order: int `drop_threshold_ms` (min 200, max 10000, step 100, suffix " ms"); string list `ip_family` with
  items ("IPv4+IPv6","IPv4+IPv6"), ("IPv4","IPv4"), ("IPv6","IPv6") (names are localized keys `IPFamily.Both/V4Only/V6Only`); string list
  `bind_ip` with first item (`Default`,"default") followed by one item per interface address (R40). Windows adds the two bool properties.
  `pframe_drop_threshold_ms` and `dyn_bitrate` are NOT exposed.

### 5.2 init_connect (runs on the connect thread before connecting)
- **S-045-R04** Without a service: return false, nothing else touched (no state reset; vector 02 end). Otherwise reset
  `disconnected`, `encode_error`, `total_bytes_sent`, `dropped_frames`, `min_priority`, `got_first_packet`.
- **S-045-R05** Path and key are copied from the service and **trimmed of surrounding whitespace**; username/password copied verbatim.
  NULL service strings become empty strings.
- **S-045-R06** `drop_b = drop_threshold_ms`, `drop_p = pframe_drop_threshold_ms`; if `drop_p < drop_b + 200` then `drop_p = drop_b + 200`.
  Stored thresholds are `1000 * drop_*` (usec). `max_shutdown_time_sec` is read as is. Unset ints read 0 (vector 02).
- **S-045-R07** `bind_ip` string is copied verbatim. `ip_family`: only if the string has exactly 4 bytes: "IPv6" -> hint 28
  (sizeof sockaddr_in6), "IPv4" -> hint 16, any other 4-byte text -> hint 0. Strings of other length leave the hint unchanged (Q-2).
- **S-045-R08** Audio: for each present audio encoder, `audio_codec[i]` = AAC if codec text is "aac" else none; `audio_bitrate` = sum of their
  `bitrate` settings. Video: for each present encoder, `video_codec[i]` from text ("h264"->H264, "av1"->AV1, "hevc"->HEVC if built with HEVC, else none), the
  encoder's `bitrate` summed into `dbr_orig_bitrate` (also `dbr_cur_bitrate`), `dbr_inc_bitrate = orig/10` (integer).
  Reset `dbr_data_size`, `dbr_est_bitrate`, `dbr_inc_timeout` to 0 and clear the DBR window (not `dbr_prev_bitrate`).
- **S-045-R09** DBR capability: `is_multitrack` = any video encoder with index >= 1 present. Each video encoder needs capability bit
  `DYN_BITRATE` (1<<2) when single-track, `MULTITRACK_DYN_BITRATE` (1<<6) when multitrack; otherwise DBR is incapable and an info log is
  written per offending encoder: if it lacks `DYN_BITRATE` the single-track text ("... does not support on-the-fly bitrate reconfiguration"),
  else (multitrack and lacks the multitrack bit) the multitrack text. `dbr_enabled = capable && dyn_bitrate`. A non-zero output delay forces it off (info log).
  Vector 03 pins every combination.
- **S-045-R10** Default table: two rows are appended to the table: row 0 all zeros, row 1 = per-encoder initial bitrates (slot i = encoder i's bitrate).
  If `dyn_bitrate` is enabled and `interpolation_table_data` has no user value, log info "Dynamic bitrate using default interpolation". If a user
  value exists the table is built from JSON (R11, which first clears the table): success -> info log; failure -> warning log and `dbr_enabled=false`. The JSON
  is loaded even when DBR is off. `Dynamic bitrate enabled.` info log when finally enabled. The default rows are appended on **every** call (Q-1).
- **S-045-R11** build table (vector 05). Parse JSON (duplicate keys rejected, trailing garbage rejected); on failure log a warning and return false. For each
  present video encoder i: `root[i]` missing/not an array -> warning, malformed; else its size must equal the first non-zero size seen
  (an empty first array sets the "overall size" to 0 and is not compared, Q-3); mismatch -> warning, malformed. If malformed: return false with
  the old table untouched. Otherwise clear the table, `overall` rows are created (zero-filled), and for every row and for **every array present in
  root[0..9]** (not only encoders with an encoder) set `bitrates[j] = integer value`; a non-integer or missing entry logs a warning and stays 0 but the
  build still succeeds (Q-4, D-1).
- **S-045-R12** `connecting` is not touched by init_connect.

### 5.3 Packet intake and frame dropping
- **S-045-R13** `encoded_packet(p)`: ignored when `disconnected` or not `active`. `p == NULL` = encoder failure: set `encode_error`, post the send semaphore, return.
  The first packet of any type sets `start_dts_offset = dts * 1000 / timebase_den` (i32 ms) and `got_first_packet`. Video packets whose track codec is NONE are logged
  (error) and discarded without being queued. Otherwise the packet is parsed by its codec (S-012/S-021) / referenced (audio) and queued under the packet lock;
  audio is always queued; video goes through R14. If queued, post the semaphore; else release the packet.
- **S-045-R14** `add_video_packet`: run `check_to_drop_frames(b-stage)` then `(p-stage)`. Then if `packet.drop_priority < min_priority`: `dropped_frames++`, reject
  (the packet is released). Else `min_priority = 0`, `last_dts_usec = packet.dts_usec`, queue it. The check runs BEFORE the new packet, so the triggering
  packet is itself subject to the new `min_priority` (vector 10).
- **S-045-R15** `check_to_drop_frames(pframes)`: priorities: b-stage = 2, p-stage = 3; threshold: b = `drop_threshold_usec`, p = `pframe_drop_threshold_usec`.
  1. b-stage with DBR enabled and `dbr_inc_timeout != 0` and `now >= dbr_inc_timeout`: set timeout 0, `dbr_inc_bitrate()`, `dbr_set_bitrate()`.
  2. If fewer than 5 packets queued: in the b-stage set `congestion = 0`; return.
  3. Find the first queued video packet that is not a keyframe; none -> return (congestion unchanged).
  4. `dur = last_dts_usec - first.dts_usec`; b-stage: `congestion = (f32)dur / (f32)threshold_b` (f32 division).
  5. DBR enabled: p-stage returns; b-stage: if `dur >= 200000` call `dbr_bitrate_lowered()` under the DBR lock and, if it returned true, `dbr_set_bitrate()`. Never drops. Return.
  6. Else if `dur > threshold` -> `drop_frames(priority)`.
- **S-045-R16** `drop_frames(prio)`: rebuild the queue keeping packets that are audio or have `drop_priority >= prio` (order kept); release the others and count them;
  `min_priority = max(min_priority, prio)`; `dropped_frames += count`. Debug log with the buffer duration (text not normative).
- **S-045-R17** Statistics (vector 13): total bytes = `total_bytes_sent`; dropped = `dropped_frames`; connect time = engine `connect_time_ms`; congestion = (new socket loop)
  `write_buf_len / write_buf_size` (f32) else `min_priority > 0 ? 1.0 : congestion`.

### 5.4 Dynamic bitrate
- **S-045-R18** `dbr_add_frame(f)` (vector 06): push f; `front` = oldest frame (may be f itself); `data_size += f.size`;
  `dur_ms = (f.send_end - front.send_beg) / 1_000_000` using **u64 wrapping** subtraction (Q-5). If `dur_ms >= 2000`: `data_size -= front.size` and pop exactly that one frame
  (the estimate below uses the span measured before the pop). `est = dur_ms >= 1000 ? (long)(data_size * 1000 / dur_ms) : 0` (size_t arithmetic), then `est *= 8; est /= 1000`.
  If `est != 0`: `est -= audio_bitrate`; if `est < 50` then 50.
- **S-045-R19** `dbr_bitrate_lowered()` (vector 07): `est = 0; if dbr_est_bitrate != 0 && dbr_est_bitrate < dbr_cur_bitrate`: clear window and `data_size`, `est = dbr_est_bitrate/100*100`, if `est < 50` then 50
  (`dbr_est_bitrate` itself is not changed). If `est != 0` new = est; else if `dbr_prev_bitrate != 0` new = prev and info log "going back to prev bitrate"; else return false.
  If new == cur return false. Otherwise `prev = 0; cur = new; inc_timeout = now_ns + 4_000_000_000`, info log "bitrate decreased to: N", return true.
- **S-045-R20** `dbr_inc_bitrate()`: `prev = cur; cur += inc`; if `cur >= orig`: `cur = orig`, info "bitrate increased to: N, done" (timer not re-armed); else `inc_timeout = now+4 s` and info "..., waiting".
- **S-045-R21** `dbr_set_bitrate()` (vector 08): no-op if the table is empty. `base` = initial last row; for each row in order, sum its 10 slots; the first row whose sum `> cur` ends the scan
  (so `base` is the last row with sum `<= cur`, but stays the LAST row if even row 0 exceeds `cur`, Q-6). `upper = min(base+1, last)`. `deltas[i] = upper[i]-base[i]`, `overall = max(sum deltas, 1)`,
  `remaining = cur - sum(base)`, `scale = min(remaining / (f64)overall, 1.0)`. For each present video encoder i: `br = base[i] + (long)(deltas[i] * scale)` (truncation toward zero);
  if the encoder's `bitrate` setting differs, set it and call `encoder_update` with the settings object.
- **S-045-R22** Window `dbr_frames`: every sent packet (when DBR is on) records (send begin ns, end ns, packet.size) after a successful write (R30) under the DBR lock.

### 5.5 Connect, start, stop
- **S-045-R23** `try_connect` (vectors 15-17), in order: empty path -> warning, -1. Info log "Connecting to RTMP URL ...". Free any TLS context, `RTMP_Init`.
  `RTMP_SetupURL(path)` fails -> -1. `RTMP_EnableWrite`. flashVer string = `FMLE/3.0 (compatible; FMSc/1.0)` (copied into `encoder_name`); username/password/flashVer set as AVal
  (empty -> NULL/0); `swfUrl = tcUrl`. Bind address per R24. `RTMP_AddStream(key)`. `outChunkSize=4096`, `sendChunkSizeInfo=true`, `useNagle=true`. `RTMP_Connect(NULL)` fails -> `set_output_error` (R28), -2.
  `RTMP_ConnectStream(0)` fails -> -3 (no error message set). Info log "Connection to PATH (IP) successful" with the peer address text (R39). Then `init_send` (R26).
- **S-045-R24** Bind: `bind_ip` empty or exactly "default" (case-sensitive) -> bind info cleared. Else `netif_str_to_addr`; on success info "Binding to IPv4|IPv6". On failure the structure still carries the
  guessed family and zero address (Q-7). If after this the bind length is 0 it is set to `addrlen_hint` (16/28 with family field 0).
- **S-045-R25** Result codes of `connect_thread` (vector 20): `init_connect` false -> signal -1 and return (leaving `connecting` set, Q-10). For any video track with codec not NONE/H264/HEVC (i.e. AV1)
  whose video output colour space is PQ(4) or HLG(5): signal -9 and return (also leaves `connecting` set). Otherwise `try_connect`; non-zero -> signal code and info log "Connection to PATH failed: N".
  If not stopping, detach the thread. Finally `connecting = false`.
- **S-045-R26** `init_send` (vector 17): reset the semaphore to 0, create the send thread (failure -> `RTMP_Close`, warning, -4). If `new_socket_loop` is set, R41 applies (before this step). Then `active = true`; write metadata (`flv_meta_data`);
  failure -> warning "Disconnected while attempting to send metadata", `set_output_error`, -5 (the stream stays `active`, the send thread keeps running). Success: `begin_data_capture`, 0. Metadata bytes are not counted in `total_bytes_sent`.
- **S-045-R27** `start`: false if `can_begin_data_capture` or `initialize_encoders` fail; else `connecting = true` then create the connect thread; its failure returns false with `connecting` still true (Q-9).
- **S-045-R28** `stop(ts_ns)` (vector 19): if stopping (stop event set) and `ts != 0`: return. If connecting: join the connect thread. `stop_ts = ts/1000`; if `ts != 0` `shutdown_timeout_ts = ts + max_shutdown_time_sec*1e9`.
  If active: set the stop event; if `stop_ts == 0` post the semaphore. Else signal stop(0) (success) (the stop event is NOT set).
- **S-045-R29** Error mapping `set_output_error` (vector 14): by the engine's `last_error_code`: ETIMEDOUT->`ConnectionTimedOut`, EACCES->`PermissionDenied`, ECONNABORTED->`ConnectionAborted`, ECONNRESET->`ConnectionReset`,
  HOST_NOT_FOUND->`HostNotFound`, NO_DATA->`NoData`, EADDRNOTAVAIL->`AddressNotAvailable`, EINVAL->`InvalidParameter`, EHOSTUNREACH->`NoRoute` (Windows uses the WSAE* equivalents); then, if unmapped, -0x2700 ->
  `SSLCertVerifyFailed`, -0x7680 -> the literal text "Failed to load root certificates for a secure TLS connection." plus on Linux " Check you have an up to date root certificate bundle in /etc/ssl/certs.". Messages
  are localized keys passed through `module_text`. Unmapped codes (incl. ECONNREFUSED) set nothing. NB: on Linux HOST_NOT_FOUND = 1 and NO_DATA = 4 so they shadow EPERM/EINTR.

### 5.6 Send thread
- **S-045-R30** Loop (vectors 21-24, 30): wait semaphore; if stopping and `stop_ts == 0` break (queued packets stay for `free_packets`); pop next packet (none -> continue; the loop only ends via the other exits);
  if stopping and (`now >= shutdown_timeout_ts` [info log "Stream shutdown timeout reached (N second(s))"] or `packet.sys_dts_usec >= stop_ts`): release it, break. If headers not sent: `send_headers` (sets `sent_headers`
  first); failure -> `disconnected = true`, break. DBR: record begin time/size. Routing: video where codec != H264 or track != 0 -> enhanced `frames` muxer with `dts_offset = start_dts_offset`; audio with track != 0 -> enhanced audio frames;
  everything else (H264 video track 0, audio track 0) -> legacy muxer with the same offset (headers: offset 0). A write result < 0 -> `disconnected = true`, break.
- **S-045-R31** Accounting: before each write, `handle_socket_read` (R33). `total_bytes_sent += muxed size` for legacy mux and enhanced video (incl. headers, footers and HDR metadata) but NOT for enhanced audio (Q-11). Packets are released/freed after writing.
- **S-045-R32** Headers (`send_headers`, vector 24/25): `sent_headers = true`; for each present audio encoder in index order: extra data missing -> fail; idx 0 legacy header (mux, header flag), idx>0 enhanced audio start. Then for each present video encoder in order: HDR metadata (R34),
  then header: no extra data -> fail; codec NONE -> error log, fail; H264 idx 0 legacy; H264 idx>0, HEVC, AV1 enhanced `start` (HEVC without build support fails).
- **S-045-R33** `handle_socket_read` (vector 26) unless `new_socket_loop`: if `FIONREAD > 0` read ONE RTMP packet; read failure -> error log "RTMP_ReadPacket error: N", return failure (the write is skipped). The packet is discarded.
- **S-045-R34** HDR metadata (`send_video_metadata`): encoder without video output -> fail. Only when the output colour space is PQ or HLG and the codec is not H264: write one `flv_packet_metadata` with bits per sample (I010,P010,I210 -> 10; I412,YA2L -> 12; else 8), primaries/transfer/matrix
  (PQ: BT2020(9), SMPTE2084(16), BT2020-NCL(9); HLG: 9, ARIB-B67(18), 9), min luminance 0, max luminance 1000 for HLG, `(int)nominal_peak` for PQ. Otherwise nothing.
- **S-045-R35** Exit: if `disconnected` info "Disconnected from PATH" (no footers); else if `encode_error` info "Encoder error, disconnecting" + footers; else info "User stopped the stream" + footers. Footers: for each present video encoder except (idx 0 and H264) one enhanced `end` packet.
  Then `set_output_error`, `RTMP_Close`, restore DBR (if enabled and `cur != orig`: `cur = orig`, `dbr_set_bitrate`). Signal: not stopping -> detach, signal -5; stopping and encode_error -> signal -8; else `end_data_capture`. Finally free queued packets
  (info "Freeing N remaining packets" when N > 0), reset the stop event, `active=false`, `sent_headers=false`.
- **S-045-R36** `can_shutdown_stream`: true if `now_ns >= shutdown_timeout_ts` (equality counts, logs the info line) or `sys_dts_usec >= stop_ts`.
- **S-045-R37** `destroy` (vector 29): if stopping and not connecting: join send thread. Else if connecting or active: if connecting join connect thread; `stop_ts = 0`, set stop event; if active post semaphore, `end_data_capture`, join send thread. Then free TLS, free queued packets, strings, events, tables.

### 5.7 Network helpers (net-if.c)
- **S-045-R38** `netif_str_to_addr(out, &len, text)` (vector 27): zero `out`, `len = 0`; NULL text -> false (len stays 0). A ':' anywhere selects IPv6 (family set, len = 28) else IPv4 (len = 16); parsing is inet_pton: success true, failure false with the length and family still
  set and a zero address. Port or brackets are not understood.
- **S-045-R39** `netif_addr_to_str(in, buf, len)`: false for NULL args, family not INET/INET6, or `len` smaller than 46 (IPv6) / 16 (IPv4); else zero-fill `len` bytes and write the canonical inet_ntop text.
- **S-045-R40** `netif_get_addrs` (vector 28): skip entries with no address or loopback (name exactly "lo" or "lo0"), non-IPv4/IPv6 families, and `getnameinfo` failures (warning). Entry name `"[adapter] addr"`, or `addr` when the adapter name is empty; value `addr` (no zone, no port). Order = OS order.
  `getifaddrs` failure -> warning "getifaddrs() failed" and empty list. `netif_log_saddrs` logs one info line `"\t\t" + name` per entry.
- **S-045-R41** New-socket-loop sizing (vector 18, platform note): when `new_socket_loop` is set `init_send` additionally sets the socket non-blocking (failure: warning, -4), logs "New socket loop enabled by user", computes `total = sum(video bitrates) + 6 * audio-encoder-0 bitrate`
  (Q-8; a video bitrate of 0 logs a warning, disables low latency and counts as 10000; audio default 160 if 0), buffer = `max(total*128, 131072)` bytes, and on non-Windows fails with warning "New socket loop not supported on this platform" (-4). `init_connect` forces the option off on non-Windows.

## 6. Algorithms

Section 5 gives them stepwise (R15, R18, R19, R21 are the numerically precise ones). Floating point: congestion `%.9g` of an f32; `dbr_set_bitrate` ratio is f64; compare exactly.

## 7. Concurrency

Threads: connect thread (init + connect), send thread (consumer), encoder thread (calls `encoded_packet`), UI thread (`stop`, `destroy`). `packets` queue is guarded by one mutex; DBR window/estimate by a second;
`connecting/active/disconnected/encode_error` are atomics; the stop event is a manual-reset event; the semaphore counts queued packets (+1 extra to wake for a forced stop). `congestion`, `dropped_frames`, `min_priority` are read without locks by the stats callbacks.
Rules R15..R16 run under the packet lock (called from `encoded_packet`); `dbr_set_bitrate` is called outside both locks and calls into the encoder.
Conformance executes everything on one thread with fake thread calls; real concurrency is not covered.

## 8. Reference defects and quirks

- **Q-1** Default DBR rows are appended on every `init_connect`: the table grows by 2 rows per start (script 04). Required: reproduce (SHOULD) or clear first; effect is only extra rows after the real ones, which change `dbr_set_bitrate` scanning. Behaviour listed here as reference.
- **Q-2** `addrlen_hint` is sticky when `ip_family` is not 4 bytes ("IPv4+IPv6" keeps the previous hint) (script 02).
- **Q-3** Size mismatch detection ignores arrays of size 0 at the start (script 05).
- **Q-4** Non-integer table entries only warn; the table is still accepted with zeros (script 05).
- **Q-5** Window span subtraction wraps if the clock goes backwards (script 06).
- **Q-6** If the target is below row 0's sum, `dbr_set_bitrate` selects the LAST row (script 08).
- **Q-7** An unparsable `bind_ip` silently becomes an all-zero address of the guessed family, no warning on Linux (script 16).
- **Q-8** New-socket-loop sizing counts audio encoder 0 six times (script 18).
- **Q-9** `start` leaves `connecting` set when thread creation fails (script 19). **Q-10** `connect_thread` early returns (no service / HDR+AV1) leave `connecting` set (script 20). Both make later `stop`/`destroy` join a thread that never ran; a Rust port must not hang.
- **Q-11** Enhanced audio writes are not counted in `total_bytes_sent` (script 30).
- **Q-12** The packet that triggers dropping is itself checked against the new `min_priority` and usually rejected (script 10).
- **Q-13** `try_connect` failure at `RTMP_ConnectStream` sets no user message (script 17).
- **D-1** DEFECT: when `root[i]` is an empty array while another array is longer, building the table dereferences a NULL JSON value while formatting the warning and crashes (SIGSEGV; script 05, `probe` lines). Required behaviour: log a warning for the missing entry, leave 0, succeed.

## 9. Implementation notes (non-normative)

Model the engine, encoder and output as traits and drive the send loop as a function over a `Transport` trait, as the conformance runner does; keep the time source injectable. Use `i64`/`u64` wrapping subtraction in the DBR window. The queue can be a `VecDeque`; `drop_frames` is a `retain`.
Keep `long` as `i64` (Linux/macOS 64-bit; Windows `long` is 32-bit: values above `i32::MAX` in tables are Linux-only behaviour).

## 10. Conformance coverage

| Rules | Scripts |
|---|---|
| R01-R03 | 01, 13 |
| R04-R12 | 02, 03, 04 |
| R11 | 05 |
| R13, R14 | 09, 10, 11 |
| R15, R16, R17 | 10, 11, 12, 13 |
| R18 | 06 |
| R19, R20 | 07 |
| R21 | 08 |
| R22 | 30 |
| R23, R24 | 15, 16, 17 |
| R25 | 20 |
| R26, R41 | 17, 18 |
| R27, R28 | 19 |
| R29 | 14 |
| R30, R31, R35, R36 | 21, 22, 23, 24, 30 |
| R32 | 22, 24 |
| R33 | 26 |
| R34 | 25 |
| R37 | 29 |
| R38, R39 | 27 |
| R40 | 28, 01 |

## 11. Open questions / not covered

- Not linked: real librtmp (the engine behaviour after `RTMP_Connect` is S-031), the FLV muxer (S-021), Windows code (`socket_thread_windows`, WSA error mapping, interface-type logging via `GetBestRoute`, `GetAdaptersAddresses` enumeration), real threads and sockets, the optional `TEST_FRAMEDROPS` debug code.
- Real thread interleavings (e.g. `stop` racing the send loop) are not tested.
- Windows `long` is 32 bits; Windows behaviour of bitrate arithmetic is unverified.
- Log texts: only the strings quoted above are intended to be stable; debug-level lines are not normative.
- Rust sufficiency check was skipped by instruction; spec completeness against an implementation is unverified.
