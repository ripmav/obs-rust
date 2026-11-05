# S-041 Encoder, output, interleaver and output delay

| | |
|---|---|
| Status | Draft (partial coverage, see 1.2 and 11) |
| Spec version | working tree, branch docs/rust-rewrite-plan |
| Reference | C implementation, libobs 33.0.1 (encoder object, output object, output-delay buffer) |
| Target crate | `crates/obs-encoder-output` |
| Depends on | S-004 (signals), S-002 (calldata), S-025 (encoder options, not repeated here), S-021 / S-031 (FLV / RTMP: consumers of the packets, not repeated here) |
| Conformance | `test/conformance/scripts/encoutput/` (runner `gen/encoutput_runner.c`) |

## 1. Scope

### 1.1 In scope

* The **encoder object**: creation, media binding, initialisation, start/stop of the raw-frame connection, the
  raw-frame to packet path (video sync, audio buffering/alignment), the packet fan-out to callbacks with `dts_usec`,
  first-keyframe gating, SEI prefixing, encode-error handling.
* The **output object** for *encoded* outputs: creation, attach/detach of encoders, `start`/`stop`/`force_stop`,
  `begin_data_capture`/`end_data_capture`, the ten signals, stop codes, last-error, reconnect with back-off, destroy.
* The **interleaver** (audio/video packet merge, start-up pruning, timestamp offsets, release rule, batch size).
* The **output delay** buffer (delayed start, delayed stop, drop on stop/force-stop, reconnect with the preserve flag).

Callers: output plugins (RTMP, FFmpeg mux, ...) call `begin_data_capture`, `initialize_encoders`, `signal_stop`,
`end_data_capture`; the frontend calls `start`/`stop`/`force_stop`/`set_*`; encoder plugins implement the encoder
callbacks. The consumer of the interleaved packets is the output plugin's `encoded_packet` callback.

### 1.2 Not covered (see also section 11)

Raw (unencoded) outputs; pause/unpause; closed captions (`obs_output_caption*`, the caption track mutex);
encoder groups and the multi-track keyframe-alignment bookkeeping; GPU (texture) encoding and GPU rescale; encoder ROI;
encoder reconfigure-on-pause; packet-callback registration (`obs_output_add_packet_callback`) and the packet-timing
metrics (`encoder_packet_time` content beyond "is carried along"); congestion / connect time getters; output
properties/defaults/settings; weak references; the `obs_output_set_preferred_size*`, mixer and conversion setters;
service activation. The module-load registration of encoder/output *types* is S-035. Platform differences are not tested.

The Rust sufficiency check (clean-room re-implementation against the vectors) was **skipped** on the caller's
instruction (cost control); no spec bug was therefore exposed by it.

## 2. Terminology and conventions

RFC 2119 keywords. "Packet" = `encoder_packet`. "dts_us" = `dts * 1_000_000 / timebase_den` (integer division, the
numerator is NOT used, see Q-1). Times of the fake clock are nanoseconds. All numbers 64-bit little-endian.
`track` = index of the encoder inside the output (video index 0..9, audio index 0..5).
Constants: `MAX_OUTPUT_VIDEO_ENCODERS = 10`, `MAX_OUTPUT_AUDIO_ENCODERS = 6`, `RECONNECT_RETRY_MAX_MSEC = 900000`,
`RECONNECT_RETRY_BASE_EXP = 1.5`.

## 3. Interface

### 3.1 Flags, codes, enums

| Name | Value |
|---|---|
| `OBS_OUTPUT_VIDEO` / `AUDIO` / `AV` | 1 / 2 / 3 |
| `OBS_OUTPUT_ENCODED` | 4 |
| `OBS_OUTPUT_SERVICE` | 8 |
| `OBS_OUTPUT_MULTI_TRACK` (= `MULTI_TRACK_AUDIO`) | 16 |
| `OBS_OUTPUT_CAN_PAUSE` | 32 |
| `OBS_OUTPUT_MULTI_TRACK_VIDEO` | 64 |
| `OBS_OUTPUT_DELAY_PRESERVE` | 1 |
| Stop codes `SUCCESS, BAD_PATH, CONNECT_FAILED, INVALID_STREAM, ERROR, DISCONNECTED, UNSUPPORTED, NO_SPACE, ENCODE_ERROR, HDR_DISABLED` | 0, -1, -2, -3, -4, -5, -6, -7, -8, -9 |
| Encoder caps `DEPRECATED, PASS_TEXTURE, DYN_BITRATE, INTERNAL, ROI, SCALING, MULTITRACK_DYN_BITRATE` | bit 0 .. bit 6 |
| Encoder type `AUDIO, VIDEO` | 0, 1 |

### 3.2 Encoder API (behaviour in section 5.1)

`obs_video_encoder_create(id, name, settings, hotkeys)`, `obs_audio_encoder_create(id, name, settings, mixer_idx, hotkeys)`,
`obs_encoder_set_video/set_audio`, `obs_encoder_initialize`, `obs_encoder_shutdown`, `obs_encoder_start(enc, cb, param)`,
`obs_encoder_stop(enc, cb, param)`, `obs_encoder_set_frame_rate_divisor/get_...`, `obs_encoder_set_scaled_size`,
`obs_encoder_get_width/height/sample_rate/frame_size/mixer_index/caps/codec/type/last_error`, `obs_encoder_active`,
`obs_encoder_get_encoded_frames`, `obs_encoder_packet_create_instance/ref/release`, plus the internal
`send_off_encoder_packet(enc, success, received, pkt)` and `do_encode`.

Encoder plugin callbacks used: `create(settings, enc) -> data|NULL`, `destroy(data)`, `encode(data, frame, packet, &received) -> bool`,
`get_frame_size(data)` (audio), `get_sei_data(data, &sei, &size) -> bool` (video, optional).
`packet` is pre-filled by the core with `timebase_num = enc.timebase_num * enc.frame_rate_divisor`,
`timebase_den = enc.timebase_den`, `encoder = enc`. NOTE: for audio encoders `frame_rate_divisor` is 0, so the preset
numerator is 0; audio plugins set their own timebase (the fake plugin sets 1/sample_rate). (read from source, not
separately demonstrated.)

### 3.3 Packet (fields the core reads or writes)

`data, size, pts, dts, timebase_num, timebase_den, type, keyframe, dts_usec, sys_dts_usec, priority, drop_priority,
track_idx, encoder`. `data` of packets handed to output callbacks is **reference counted**: a block with a `long`
counter in front; `create_instance(dst, src)` copies the struct, allocates `size` bytes, sets the counter to 1 and
copies the bytes; `ref(dst, src)` increments the counter and copies the struct; `release(pkt)` decrements, frees at 0,
and zeroes the struct (`data == NULL` packets are accepted).

### 3.4 Output API (behaviour in sections 5.2 to 5.6)

`obs_output_create(id, name, settings, hotkeys)`, `obs_output_destroy`, `obs_output_start/stop/force_stop`,
`obs_output_active`, `obs_output_reconnecting`, `obs_output_get_flags`, `obs_output_set_video_encoder2(out, enc, idx)`,
`obs_output_set_audio_encoder(out, enc, idx)`, `obs_output_set_delay(out, sec, flags)`, `obs_output_get_delay`,
`obs_output_get_active_delay`, `obs_output_set_reconnect_settings(out, count, sec)`, `obs_output_set_reconnect_callback`,
`obs_output_set_service`, `obs_output_get_last_error/set_last_error`, plugin-facing: `obs_output_can_begin_data_capture`,
`obs_output_initialize_encoders`, `obs_output_begin_data_capture`, `obs_output_end_data_capture`, `obs_output_signal_stop(out, code)`.

Output plugin callbacks: `create(settings, out) -> data|NULL`, `destroy`, `start(data) -> bool`, `stop(data, ts)`,
`encoded_packet(data, pkt|NULL)`. `ts` = the nanosecond clock value at the time of the stop request (0 for force_stop).

### 3.5 Signals (declared on every output)

`start(ptr output)`, `stop(ptr output, int code)`, `pause`, `unpause`, `starting`, `stopping`, `activate`, `deactivate`,
`reconnect`, `reconnect_success`. Extra calldata members set by the core although not declared: `stop`: `last_error`
(string, may be NULL), `code` (int), `output`; `reconnect`: `timeout_sec` (int, whole seconds = cur_msec / 1000, integer division), `output`.

### 3.6 Output state fields (observable through the runner `state` command)

`active`, `data_active`, `delay_active`, `delay_capturing`, `reconnecting`, `stopping` (the stopping event is NOT signalled),
`stop_code`, `total_frames`, `active_delay_ns`, `delay_restart_refs`, interleaved queue length, `received_video[track]`,
`received_audio`, `highest_audio_ts`, `highest_video_ts[track]` (initial `INT64_MIN`), `video_offsets`, `audio_offsets`,
`interleaver_max_batch_size`, `reconnect_retries`, `reconnect_retry_cur_msec`.

## 4. Worked example: interleaver start-up (script `il_start_basic`)

Video 30 fps (timebase 1/30), audio 48 kHz frames of 1024 (21333 us). Encoders' `start_ts` are 0 so `dts_usec` is the
packet dts in microseconds, offset by the encoder's first packet (R-E04).

| Step | Packet | Queue after (sorted by dts_usec) | Sent |
|---|---|---|---|
| 1 | A dts 0 | A@0 | |
| 2 | A dts 1024 | A@0 A@21333 | |
| 3 | V dts 0 (keyframe) | video and audio both received; audio last = 21333 >= 0 so start-up succeeds; V placed BEFORE A@0 (equal dts_usec, video first); offsets: video pts 0, audio dts 0 (not > 0, unchanged) | V@0 |
| 4 | A dts 2048 | A@0 A@21333 A@42666 (no video beyond audio yet) | |
| 5 | V dts 1 | V@33333 inserted | A@0 (one packet per arrival) |
| 6 | A dts 3072 | | A@21333 |
| 7 | A dts 4096 | | V@33333 |

## 5. Behaviour (numbered rules)

### 5.1 Encoder (`R-E`)

* **S-041-R01** Creating an encoder with an unknown id logs an error ("Encoder ID '<id>' not found", level error) but still
  returns an object with that id, no codec, and the requested type (inert: initialisation fails). A known id whose
  registered type differs from the requested type returns NULL, silently. Video encoders start with `frame_rate_divisor = 1`.
  (`enc_create`)
* **S-041-R02** `set_video(enc, video)` / `set_audio`: wrong encoder type -> warning, no change. While the encoder is active ->
  warning, no change. `set_video` additionally refuses once the encoder is initialised (warning). Binding sets the packet timebase:
  video `num = fps_den, den = fps_num` of the video output (so 30/1 -> 1/30, 60000/1001 -> 1001/60000); audio `1 / sample_rate`.
  Binding NULL clears media and timebase to 0/0. (`enc_create`, `enc_media_config`)
* **S-041-R03** `set_frame_rate_divisor(enc, d)`: false (with warning) for non-video, active, initialised encoder or `d == 0`;
  otherwise stores `d`. `set_scaled_size(w, h)`: ignored with warning when non-video, active or initialised; if `(w,h)` equals the video output size,
  scaling is *disabled* (scaled size 0x0, warning) else stored. `get_width/height` return the scaled size if set else the media size, 0 if no media, 0 +
  warning for audio encoders; `get_sample_rate`/`get_frame_size` are the mirror image for video encoders. `get_frame_size` is 0 before
  initialisation (read from the plugin at init). `get_mixer_index` is the creation argument. (`enc_media_config`, `enc_create`)
* **S-041-R04** `obs_encoder_initialize`: NULL -> false. Requires media (else error log, false). Already initialised or active -> true
  without side effects. Otherwise shuts down any old plugin state, calls the plugin `create`; NULL data -> false; audio encoders then read
  `samplerate`, planes, `framesize = plugin.get_frame_size(data)`. (`enc_create`, `out_start_failures`)
* **S-041-R05** `obs_encoder_start(enc, cb, param)`: no effect unless the encoder has plugin data (initialised) and media. The (cb, param) pair is
  appended once (duplicates ignored). When it is the FIRST registered callback: the paused flag is cleared, pause state reset, `cur_pts = 0`,
  and the raw-frame connection is made (video: raw video connection with the encoder's frame-rate divisor; audio: audio connection on
  `mixer_idx`), the encoder becomes active. `obs_encoder_stop(enc, cb, param)` removes that pair, clears the encoder's frame-timing queue,
  and when no callback is left removes the connection, **shuts the encoder down** (plugin `destroy`, `first_received = false`,
  `offset_usec = 0`, `start_ts = 0`, paired-encoder list freed, last error cleared), `initialized = false`, inactive. `cur_pts` is not reset
  by stop. (`enc_two_outputs`, `enc_flow_sync`, `out_destroy_active`)
* **S-041-R06** (video frame handling) For each raw video frame of an encoder with a callback: (a) if the encoder is paired with audio
  encoders (set by the output when it starts, R-O13) and has not yet encoded anything, the frame is dropped unless every paired encoder has
  `first_received` and its first raw audio timestamp is `<= frame.timestamp`; (b) pause check; (c) `start_ts` is set to the first
  accepted frame's timestamp; (d) the plugin is called with `frame.pts = cur_pts`; if the call succeeds `cur_pts += timebase_num * frame_rate_divisor`.
  A frame delivered before the audio has been seen is therefore *lost*, not queued. (`enc_flow_sync`)
* **S-041-R07** (audio frame handling) The first block marks `first_received` and records `first_raw_ts`; audio data is appended
  to a per-plane buffer. For an encoder paired with a video encoder: no audio is started until the video encoder has a `start_ts`, and a block that
  ends at or before it is discarded (buffered data cleared if `block.timestamp <= video start`); the first block that covers the video start is truncated
  at the front (offset = floor((video_start - block_ts) * rate / 1e9) frames * blocksize) and the audio encoder's `start_ts` is set to the video's
  `start_ts`; if earlier audio was buffered it is re-cut the same way. Unpaired audio encoders take `start_ts` = first block timestamp. Then, while the
  buffer holds at least `framesize_bytes`, one frame is encoded (`pts = cur_pts`, `cur_pts += framesize`); a failing encode stops the loop. (`enc_flow_sync`)
* **S-041-R08** (`send_off_encoder_packet(enc, success, received, pkt)`) `success == false`: error log "Error encoding with encoder '<name>'",
  then **full stop** (R-E12), nothing delivered. `received == false` with success: nothing delivered. Otherwise: the first delivered-or-withheld packet sets
  `offset_usec = dts_us(pkt)` and `first_received`; always
  `pkt.dts_usec = start_ts / 1000 + dts_us(pkt) - offset_usec` (integer arithmetic, `start_ts` in ns) and `pkt.sys_dts_usec = dts_usec + pause.ts_offset / 1000`.
  Note `offset_usec` is taken from the first packet *even if the first-keyframe gate then withholds it*. Then all callbacks are invoked in
  **reverse registration order**. For video packets `encoded_frames` is incremented after the callbacks. (`enc_packet_path`, `enc_hold_bframes`, `enc_two_outputs`)
* **S-041-R09** (frame timing records) `do_encode` with a frame creation timestamp pushes one `encoder_packet_time {pts = frame.pts, cts, fer, ferc}` per submitted
  video frame; `send_off_encoder_packet` removes the **latest** record whose `pts` equals the packet's pts and passes it to every callback (NULL if none; a debug log
  "Encoder packet timing for PTS n not found"). The content of the metrics is outside this spec.
* **S-041-R10** (first-keyframe gate and SEI) Each registered callback has a `sent_first_packet` flag. For video encoders, until it is set a packet is delivered only if
  `keyframe`; non-keyframes are withheld (for that callback only). If the plugin has `get_sei_data` returning true with non-empty data, the first delivered packet's payload is
  `SEI || original` (size grows by the SEI length; the callback gets a temporary copy), later packets are unmodified. Audio packets are never gated.
  Callbacks added later (second output) start with their own gate. (`enc_packet_path`, `enc_two_outputs`)
* **S-041-R11** `obs_encoder_get_encoded_frames` = number of video packets that went through `send_off_encoder_packet` with `received`. Hold-back: an encoder may return
  `received == false` for the first frames (look-ahead); `cur_pts` still advances per successful call. (`enc_hold_bframes`)
* **S-041-R12** (full stop) Called on encode failure: for every output using the encoder, in order: `force_stop(output)`, then under the output's interleaver
  lock the plugin's `encoded_packet(NULL)` is called; then the encoder's callback list is emptied and the connection is removed (the encoder is inactive but **not** shut down:
  `initialized` becomes false, plugin data stays until the next initialise/shutdown). (`enc_errors`)

### 5.2 Output creation and plugin entry points (`R-O`)

* **S-041-R20** `obs_output_create`: unknown id logs error "Output ID '<id>' not found" but returns an object (id copied, flags 0, `context.data == NULL`; also logs "Failed to create output '<name>'!");
  `start`/`stop` on such an object do nothing / return false. Defaults: `reconnect_retry_max = 20`, `reconnect_retry_sec = 2`, exponent =
  `1.5 + rand * 0.05` (rand in [0,1)). The stopping event is signalled (not "stopping"). (`out_guards`, `out_start_stop`)
* **S-041-R21** Encoder attachment (`set_video_encoder2`, `set_audio_encoder`): ignored with warning on non-encoded outputs, outputs without the matching video/audio flag,
  wrong encoder type, or while the output is active. Index `> 0` is ignored unless the output has the multi-track flag of that kind; with it, index must be `< 10` (video) / `< 6` (audio).
  Setting the same encoder again is a no-op. Replacing releases the old encoder and takes a reference on the new one; the encoder records the output in its output list;
  passing NULL clears the slot. (`out_guards`)
* **S-041-R22** `can_begin_data_capture` (core rule): output valid; true while delay is active; false if the output is active; else requires, in order: video flag => at least
  one video encoder (encoded) ; audio flag => at least one audio encoder; service flag => a service attached. A pending end-of-capture thread is joined first. (`out_start_failures`)
* **S-041-R23** `initialize_encoders`: false on non-encoded output (warning); if the output is active it returns `delay_active`; otherwise initialises every attached video encoder then every attached audio
  encoder (R-E04); the first failure stores that encoder's last-error as the output's error and returns false. A plugin MUST call this before `begin_data_capture` (D-2). (`out_start_failures`)
* **S-041-R24** `obs_output_start`: false if no plugin data; for service outputs false unless the service can try to connect and initialises. With `delay_sec > 0` -> delayed start (R-D01). Else
  calls `actual_start`; on true emits `starting` and returns true, otherwise false (no signal).
  `start` has no "already active" guard: calling it on an active output calls the plugin's `start` again (Q-6). (`out_start_stop`, `out_start_failures`)
* **S-041-R25** `actual_start`: waits for the stopping event, sets `stop_code = 0`, frees the last-error string, calls plugin `start(data)`, then decrements `delay_restart_refs` if non-zero (regardless of
  the plugin's result), and clears each caption track's pending state. Returns the plugin result. Because the stopping event is waited for, an unfinished stop blocks the start.
* **S-041-R26** `obs_output_begin_data_capture` (plugin-facing): invalid -> false. If delay is active -> R-D03. If already active -> false. Otherwise: `total_frames = 0`; `can_begin_data_capture` must hold (else false);
  for A+V outputs the encoders are **paired** (R-O13); `data_active = true`; the data hook (R-I01); the batch size computed (R-I09); service activated; signal `activate`; `active = true`; then exactly one of
  `reconnect_success` (when reconnecting; clears `reconnecting`), `starting` (when delay is active), or `start`. So with a synchronous plugin `start()` the order is `activate, start, ... , starting` (the core's `starting` from `obs_output_start` comes after). (`start_basic`, `rc_success`, `dl_start`)
* **S-041-R27** Pairing (`pair_encoders`): only for outputs having both flags. Takes the first video encoder; if it is already active nothing happens; else every audio encoder that is not active and has no
  pairs yet is paired with it (both sides keep a weak reference to the other). Pairing makes the video wait for audio (R-E06) and aligns audio to the video start (R-E07).

### 5.3 Stop, signals, error codes

* **S-041-R30** `obs_output_stop`: no effect without plugin data, or when neither active nor reconnecting. If reconnecting -> `force_stop`. If encoded and a delay is active (`active_delay_ns != 0`) -> delayed
  stop (R-D05). Otherwise, if not already stopping: signal `stopping`, then `actual_stop(force = false, ts = clock)`. (`out_start_stop`)
* **S-041-R31** `obs_output_force_stop`: if not already stopping: `stop_code = 0` and signal `stopping`. Then `actual_stop(force = true, ts = 0)`. It works on idle outputs too (signals `stopping`, calls the plugin `stop`, see Q-4). (`out_start_stop`, `dl_force_stop`)
* **S-041-R32** `actual_stop(force, ts)`: if stopping and not forced -> return. Unpauses; resets the stopping event (=> "stopping"); if reconnecting: signals the reconnect-stop event and joins the reconnect thread
  (if it is running). Forced with delay active: `call_stop = delay_capturing`; delay flags cleared, `stop_code = 0`, `end_data_capture`, stopping event set. Non-forced or non-delay: call plugin `stop`. If the plugin
  `stop` is not called (data NULL or `call_stop` false) and the output *was reconnecting without delay*: `stop_code = 0`, signal `stop`, stopping event set. Finally pending captions are freed and the keyframe-group
  tracking is cleared. (`rc_veto_and_stop`, `dl_force_stop`)
* **S-041-R33** `obs_output_end_data_capture(out)` (plugin-facing, emits `stop`): invalid -> return. If not active or data not active: emit `stop` (payload: `last_error`, `code = stop_code`), set `stop_code = 0`, set the stopping
  event, return. If delay is active: clear `delay_capturing`; if `delay_restart_refs != 0` set the stopping event and return (the buffer is kept for the restart), else clear `delay_active`. Then: clear `data_active`; log the frame
  statistics (info lines "Output '<n>': stopping", "Total frames output: N [ (M attempted)]", "Total drawn frames: N [...]"; the lagged/dropped lines only when non-zero); join any previous end thread; start the end thread; then emit `stop` and set
  `stop_code = 0`. The `stop` signal is therefore emitted before the thread's work completes; the thread (R-O14) emits `deactivate`.
* **S-041-R34** End-of-capture thread: stops the encoders' callbacks (video then audio) with the callback that was registered (the delay callback if `active_delay_ns != 0`, else the interleaver for A+V outputs, else the plain
  callback), deactivates the service, cleans up the delay queue if delay was active (R-D07), emits `deactivate`, clears `active`, sets the stopping event, clears the "thread active" flag. Stopping the last callback of an
  encoder shuts it down (R-E05). The relative order of `stop` (main thread) and `deactivate` (end thread) is **unspecified**; the vectors fix it by running the thread after the command.
* **S-041-R35** `obs_output_signal_stop(out, code)` (plugin-facing): `stop_code = code`; if a reconnect is wanted (R-C01): bump `delay_restart_refs` if delay is active, `end_data_capture_internal(signal = false)` (no `stop`
  signal), then the reconnect scheduler (R-C02). Else: clear `delay_active` and `reconnecting` if set and `end_data_capture` (emits `stop` with the code). After the `stop` signal, `stop_code` is **reset to 0** (Q-3).
  On an output that never started, only the `stop` signal is emitted. (`out_signal_stop`, `rc_*`)
* **S-041-R36** `get_last_error`: the output's own message if set; otherwise the first attached video encoder's message, then the first audio encoder's; NULL if none. `set_last_error(NULL)` clears. The message is cleared by
  `actual_start` and survives `stop`. The `stop` signal carries it. (`out_signal_stop`, `out_start_failures`)
* **S-041-R37** `obs_output_destroy`: if valid and active -> forced `actual_stop`; waits for the stopping event and the end thread; calls plugin `destroy`; frees queued packets; for each attached encoder removes the output and releases
  the encoder (an encoder with no other user shuts down as in R-E05). (`out_destroy_active`)

### 5.4 Interleaver (`R-I`, applies to encoded outputs with BOTH video and audio flags)

Outputs with only one of the flags do not interleave: each packet is passed to `encoded_packet` directly (`track_idx` set), and for video `total_frames` is incremented after the call. (`enc_packet_path`)

* **S-041-R40** (data hook, at `begin_data_capture`) Resets packet state under the interleaver lock: `received_audio = false`, `received_video[*] = false`, offsets 0, `highest_audio_ts = 0`, `highest_video_ts[*] = INT64_MIN`, frame-timing
  queues emptied, **the interleaved queue freed** (so queued packets remain visible after `stop` until the next begin or destroy). Then registers the callback with all audio then all video encoders. With a delay the callback registered is `process_delay` and the
  real callback is stored for it (R-D02). (`il_steady_state`, `dl_start`)
* **S-041-R41** `interleave_packets(packet)`: ignored if the output is not active. Sets `packet.track_idx` from the position of `packet.encoder` in the output. Under the lock: a **video** packet that is not a keyframe while
  `received_video[track]` is false is dropped (with a delay it is released) and, before returning, queued packets with `dts_usec < packet.dts_usec` are discarded (audio and video alike, from the front). (`il_gating_keyframe`)
* **S-041-R42** Otherwise: `was_started = received_audio && all received_video`. The packet is copied (`create_instance`) unless a delay is active (then ownership is taken). A frame-timing record is appended to the per-track list if supplied.
  If `was_started`: the packet's `dts`, `pts` (and timing pts) are reduced by the track offset and `dts_usec` is recomputed as `dts_us(packet)` **without the encoder `start_ts`** (Q-2). Else the "received" flag of its kind/track is set. The packet is inserted (R-I03).
* **S-041-R43** Insert order (`dts_usec` ascending): the new packet goes before the first queued packet with a strictly greater `dts_usec`; additionally a **video** packet goes before ANY queued packet with equal `dts_usec` unless both are video and the new
  packet's track index is greater (then it goes after that one). Audio with equal `dts_usec` keeps arrival order (after the existing). (`il_start_basic`, `il_multitrack_video`, `il_multitrack_audio`)
* **S-041-R44** Start-up (when after the insert `received_audio && all received_video`, and `!was_started`): `prune` (R-I05); if it succeeds, `initialize` (R-I06); if that succeeds: re-sort, apply timing offsets, send one packet. (`il_start_basic`)
* **S-041-R45** Prune (`prune_premature_packets`): no video packet queued -> failure (stay in start-up). Let `V` = first track-0 video packet, `duration = 1e6 * V.timebase_num / V.timebase_den` (integer, µs). For every attached audio encoder in index order: no queued packet of that track
  -> `received_audio = false`, failure. `diff = first_audio_dts_usec - V.dts_usec` of THIS track; `max_idx` = largest queue index of any first-audio packet. With more than one audio encoder and `duration < max audio-frame duration` (`timebase_num * 1e6 / timebase_den * framesize`), `duration` is replaced by that. Result: **`diff > duration` (diff of the LAST examined audio track, not the maximum, Q-5)**: discard
  packets `[0, max_idx]` **inclusive of the audio packet itself** (Q-4b), otherwise nothing is pruned and the start index is picked by R-I07. After pruning, `initialize` runs and finds no video, clearing `received_video[0]` (and the
  start-up restarts, waiting for a keyframe). (`il_prune_far_audio`, `il_prune_two_audio_tracks`)
* **S-041-R46** Start index (when no prune): the queue index of the audio packet closest in `|dts_usec|` to the first video packet; `idx = min(video_index, that)`; then if the first audio packet at or after `idx` has `pts <= 0`, `idx` is lowered to the earliest first-audio-packet index of any track (priming packets are kept). Packets before `idx` are discarded. (`il_audio_priming`)
* **S-041-R47** Initialize: fails (clearing `received_video[t]` / `received_audio` of the missing stream) if some attached track has no queued packet; fails with `received_audio = false` if for any audio track the **last** queued audio packet has `dts_usec <` the first video packet's. Discards to the start index (R-I06), then
  offsets: `video_offsets[t] = first queued video packet pts of track t` (PTS, not DTS, so B-frame DTS become negative); `audio_offsets[t] = first audio packet dts` **only if that dts > 0**. `highest_audio_ts -= first audio dts_usec`. All queued packets are shifted by their offsets and get
  `dts_usec = dts_us` (no `start_ts`). Frame-timing records are shifted by the video offset. (`il_offsets`, `il_audio_priming`)
* **S-041-R48** Steady state (`was_started`): the packet is inserted after offsetting; `set_higher_ts` raises `highest_audio_ts` / `highest_video_ts[track]` to `dts_usec` if larger. Count the leading queue entries that are **streamable**: an entry is streamable if, for audio, every attached video track's
  highest ts `>` its `dts_usec`; for video, additionally every *other* video track's highest ts `>` and `highest_audio_ts >` its `dts_usec`; counting stops at the first non-streamable entry. If the count is non-zero, one packet is sent; then if `count - 1 > interleaver_max_batch_size` a second packet is sent. At most two per arrival. (`il_steady_state`, `il_batch_overflow`)
* **S-041-R49** Sending a packet (`send_interleaved`): pop the front; for video `total_frames++`; caption insertion (not covered); match and remove the frame-timing record with equal pts of that track (debug logs when missing); invoke registered packet callbacks (not covered); call `encoded_packet(data, &pkt)`; release.
* **S-041-R50** Batch size (computed at every `begin_data_capture` of an encoded output, debug log "Maximum interleaver batch size for '<name>' calculated to be N packets"): intervals are, per video encoder `1e9 * fps_den * divisor / fps_num` ns, per audio encoder `1e9 * frame_size / sample_rate` ns;
  `L` = largest; the batch size is **added** (`+=`) over all encoders of `floor(2 * L / interval_i)` (integer, no reset, Q-7). For 30 fps + 1024 @ 48 kHz: `2 + 3 = 5`. (`start_basic`, `il_batch_accumulates`)

### 5.5 Reconnect (`R-C`)

* **S-041-R60** A stop code triggers a reconnect when `retry_max != 0` (and the reconnect callback, if any, returns true for that code) and the code is `DISCONNECTED`, or the output is already `reconnecting` and the code is not `SUCCESS`. With the callback absent it counts as allowed. The callback is only consulted
  when `retry_max != 0`. (`rc_veto_and_stop`, `out_signal_stop`)
* **S-041-R61** Scheduler (`output_reconnect`): if `reconnecting` and the stop event is already set -> clear `reconnecting`, return. If not reconnecting: `cur_msec = retry_sec * 1000`, `retries = 0`. If `retries >= retry_max`: `stop_code = DISCONNECTED`, clear `reconnecting` (and `delay_active`), `end_data_capture` (emits `stop` with
  `-5`), return. If not yet reconnecting: set reconnecting, reset the stop event. If `retries != 0`: `cur_msec = (uint32)(cur_msec * exp)` capped at 900000. `retries++`, `stop_code = DISCONNECTED`, start the reconnect thread, log info "Output '<n>': Reconnecting in %.02f seconds..", emit `reconnect` with `timeout_sec = cur_msec / 1000`.
  With exp 1.5, retry_sec 2: waits 2000, 3000, 4500, 6750, 10125, 15187 ms; retry_sec 400: 400000, 600000, 900000, 900000. (`rc_backoff`, `rc_backoff_cap`)
* **S-041-R62** Reconnect thread: waits `cur_msec` ms on the reconnect-stop event; on timeout calls `actual_start`. Afterwards, if the stop event is still unset the thread detaches itself, else `reconnecting` is cleared. A successful restart (plugin calls `begin_data_capture`) emits `reconnect_success` and clears `reconnecting`. A plugin start failure that reports `signal_stop(code != SUCCESS)` while
  reconnecting schedules the next attempt (R-C02). The retry counter counts attempts; with `retry_max = N` the (N+1)th failure ends with `stop(-5)`. (`rc_success`, `rc_backoff`)
* **S-041-R63** `stop`/`force_stop` while reconnecting cancel the reconnect: the plugin `stop` is called (via force path) and `stop(code 0)` is emitted by the plugin's `end_data_capture`. `reconnecting` is false afterwards. (`rc_veto_and_stop`)

### 5.6 Output delay (`R-D`; only encoded outputs)

* **S-041-R70** `set_delay(out, sec, flags)`: warning and no change on non-encoded outputs; stores both. `get_delay` = `sec`; `get_active_delay` = `active_delay_ns / 1e9` (integer seconds, truncated), 0 when no delay is active. (`out_guards`, `dl_*`)
* **S-041-R71** `obs_output_start` with `delay_sec > 0` calls `delay_start`: pushes a START message stamped with the clock; if delay is not yet active: requires `can_begin_data_capture` and `initialize_encoders` (else false); increments `delay_restart_refs`; if delay was already active emits `starting` and returns true; otherwise
  calls `begin_data_capture` (which sets `active_delay_ns = delay_sec * 1e9`, `delay_active`, `delay_cur_flags`, registers the delay callback, emits `activate` and `starting`) and on failure cleans up the delay queue and returns false. The plugin `start` is NOT called yet. (`dl_start`)
* **S-041-R72** Delay queue (`process_delay`): each arriving packet (any encoder) is copied (`create_instance`, with its timing record) and appended with `ts = clock`. Then, repeatedly, the front entry is released while `now - ts > active_delay_ns` (**strictly greater**; unsigned subtraction). A released PACKET goes to the stored real callback if
  delay is active **and capturing**, else it is released unused. A released START calls `actual_start` (the plugin then calls `begin_data_capture` which, with delay active, runs `begin_delayed_capture`: if already capturing -> false; else resets packet state, sets `delay_capturing`, emits `reconnect_success` (when reconnecting) or `start`). A released STOP calls
  `actual_stop(false, ts_of_the_stop_request)`. The queue is drained **only when a packet arrives** (no timer): a delayed stop with no further packets never completes. With `PRESERVE` in `delay_cur_flags` and `reconnecting`, the front entry is not released and `active_delay_ns = now - front.ts` (the delay grows with the outage). (`dl_start`, `dl_reconnect_preserve`, `dl_reconnect_nopreserve`)
* **S-041-R73** Delayed stop (`obs_output_stop` with a delay): pushes a STOP message (stamp = clock) and emits `stopping`; the plugin's `stop(ts)` gets that stamp when the message is released. Messages and packets queued behind STOP are discarded when the stop finishes (end thread -> `cleanup_delay` frees every queued entry, sets `active_delay_ns = 0` and `delay_restart_refs = 0`). (`dl_stop_drop`)
* **S-041-R74** `force_stop` on a delayed output: `call_stop = delay_capturing` (plugin `stop` is skipped if the delayed START has not been released yet), `delay_active`/`delay_capturing` cleared, `stop_code = 0`, `end_data_capture`, stopping event set. Packets in the delay queue are dropped (`dl_force_stop`: queue 2 -> 0). See Q-8 for the duplicate `stop` signal.
* **S-041-R75** Delay and reconnect: `signal_stop` that will reconnect increments `delay_restart_refs`; `end_data_capture` then keeps `delay_active` and the queue (sets only `delay_capturing = false`). Packets arriving during the outage are consumed from the delay queue and dropped (not delivered). The restart's START message path or `actual_start` decrements the counter.
  When retries are exhausted `delay_active` is cleared before `end_data_capture`. (`dl_reconnect_preserve`, `dl_reconnect_nopreserve`)

## 6. Algorithms (pseudocode)

```
dts_us(p)      = p.dts * 1_000_000 / p.timebase_den                    # numerator ignored

send_off(enc, ok, received, p):
  if !ok: log error; full_stop(enc); return
  if !received: return
  if !enc.first_received: enc.offset_usec = dts_us(p); enc.first_received = true
  p.dts_usec = enc.start_ts/1000 + dts_us(p) - enc.offset_usec
  p.sys_dts_usec = p.dts_usec + enc.pause.ts_offset/1000
  ept = take latest timing record with pts == p.pts (video)
  for cb in reverse(enc.callbacks): send(cb, p, ept)
  if video: enc.encoded_frames++

send(cb, p, ept):
  if video and !cb.sent_first:
      if !p.keyframe: return
      deliver (sei ++ p) or p; cb.sent_first = true
  else deliver p

interleave(p):                                   # A+V encoded outputs
  if !active: return
  p.track = index of p.encoder
  lock
  if video(p) and !recv_v[p.track] and !p.keyframe:
      discard queue entries with dts_usec < p.dts_usec ; unlock ; (release p if delayed) ; return
  started = recv_a and all(recv_v[t] for attached t)
  q = delay ? p : copy(p)
  if started: q.dts -= off(q); q.pts -= off(q); q.dts_usec = dts_us(q)
  else: mark recv flag of p
  insert(q)
  if recv_a and all recv_v:
      if !started:
          if prune() and initialize(): resort(); shift timing; send_one()
      else:
          highest[q.kind/track] = max(.., q.dts_usec)
          n = count_streamable()
          if n > 0: send_one(); if n-1 > batch: send_one()
  unlock

insert(q): idx = 0
  while idx < len:
      c = queue[idx]
      if q.dts_usec == c.dts_usec and q.video and c.video and q.track > c.track: idx++; continue
      if q.dts_usec == c.dts_usec and q.video: break
      if q.dts_usec <  c.dts_usec: break
      idx++
  queue.insert(idx, q)

streamable(i, pkt): every attached video track t != (pkt video ? pkt.track : none) has highest_v[t] > pkt.dts_usec
                    and (pkt is audio or highest_a > pkt.dts_usec)

pop_delay(now):                                  # called after each push until false
  f = front ; if none: false
  if preserve and reconnecting: active_delay_ns = now - f.ts ; false
  if now - f.ts > active_delay_ns: pop, process(f) ; true else false
```

## 7. Concurrency

* The interleaver lock protects the queue, flags and offsets; `encoded_packet` is called with it held. `full_stop` takes the same lock to deliver NULL.
* The delay mutex protects only the delay deque; released entries are processed outside it. `process_delay_data` therefore runs plugin callbacks without any delay lock.
* Encoder callbacks run on the video/audio thread (here: the caller of the raw frame). The end-of-capture thread, reconnect thread and the main thread run concurrently in the reference; `actual_start` waits on the stopping event, `destroy` waits on it and on the end thread. Atomics: `active`, `data_active`, `delay_active`, `delay_capturing`, `reconnecting`, `delay_restart_refs`.
* The reconnect thread may spawn the next reconnect thread from inside `actual_start` (a failing plugin start calls `signal_stop`) while it is still running.

## 8. Reference defects and quirks

* **Q-1 (QUIRK, must match)** `dts_us` ignores `timebase_num`. A video encoder with divisor 2 therefore reports dts in ticks of the doubled numerator but `dts_usec` uses only the denominator: packets with `tb=5/30` and `tb=1/30` and equal `dts` have equal `dts_usec`. Script `enc_packet_path`.
* **Q-2 (QUIRK, must match)** After the interleaver has started, `dts_usec` is recomputed from `dts` alone (relative, starts near 0) and no longer contains the encoder `start_ts`; before start it does. Inter-stream sync after start therefore relies only on the start-up alignment. `il_prune_two_audio_tracks`, `il_offsets`.
* **Q-3 (QUIRK, must match)** `stop_code` is reset to 0 right after the `stop` signal, so `state` after `signal_stop(-7)` shows 0; the code is only visible in the signal. `out_signal_stop`.
* **Q-4 (QUIRK, SHOULD match)** `force_stop` on an *idle* output emits `stopping`, calls the plugin `stop(0)`, and (because the plugin ends capture) emits `stop(0)` although nothing was started; `stop` on an idle output does nothing. `out_start_stop`.
  **Q-4b** The start-up prune discards the offending audio packet too (`[0, max_idx]` inclusive). `il_prune_far_audio`.
* **Q-5 (QUIRK, SHOULD match)** With several audio tracks, the prune decision uses the *last* track's gap, not the largest. `il_prune_two_audio_tracks` (run 1 no prune although track 0 is 100 ms late, run 2 prunes).
* **Q-6 (QUIRK)** `obs_output_start` on an active output calls the plugin's `start` again. `out_start_stop`.
* **Q-7 (QUIRK, SHOULD match)** `interleaver_max_batch_size` is never reset: 5, 10, 15 over three starts of the same output. `il_batch_accumulates`. (A reimplementation MAY reset; vectors expect accumulation.)
* **Q-8 (QUIRK)** `force_stop` of a delayed output emits the `stop` signal twice (once from the core's `end_data_capture`, once from the plugin's). `dl_force_stop`.
* **Q-9 (QUIRK)** A delayed stop never completes without further packets (no timer). `dl_stop_drop` only completes because a later packet arrives.
* **Q-10 (QUIRK)** `get_delay` / `get_active_delay` log with the function name `obs_output_set_delay` when the output is invalid. Text only.
* **D-1 (DEFECT; precondition violation in the reference)** When a queued video packet is discarded by pruning/start-up and the per-track frame-timing list is empty (packet had no timing record, e.g. `received` packets injected without `do_encode`), the reference pops from an empty array (assertion/abort in debug builds, UB otherwise). Required: discard the packet and pop only if the list is non-empty. Not in vectors (the runner queues a record per injected packet).
* **D-2 (DEFECT/precondition)** `begin_data_capture` on an A+V output whose audio encoder was never initialised divides by zero in the batch-size computation (`frame_size == 0`). Plugins must call `initialize_encoders` first. Required: treat `frame_size == 0` or `sample_rate == 0` as "interval 0 contributes nothing" or return false; do not crash.
* **D-3 (DEFECT, latent)** Two reconnect threads can exist: a failing plugin start inside the reconnect thread calls `signal_stop`, which creates the next thread and overwrites the stored thread handle, and the "thread active" flag is shared. Required: track each attempt separately, join/detach exactly once. Demonstrated implicitly by `rc_backoff`.

## 9. Implementation notes (non-normative)

* Model the output as `struct Output` with an `Arc<Mutex<Interleaver>>` and atomics for the flags listed in 3.6; the interleaver queue is a sorted `Vec<Packet>` where insertion position follows 6 (linear search is fine, queues stay short). Packets: `Arc<[u8]>` payload gives the refcounting for free.
* Make the clock and the reconnect timer injectable (trait) - the vectors need a fake clock and an explicit "timer expires" event; a delay implementation without a timer must keep "drained only on packet arrival".
* Keep the signal payloads exactly as 3.5 (`timeout_sec` integer division).
* The end-of-capture thread and the reconnect thread can be tasks; preserve the guarantee that `stop` is emitted by the caller of `end_data_capture` and `deactivate` after the encoders have been disconnected.

## 10. Conformance coverage

| Rule | Scripts |
|---|---|
| R01, R02, R03, R04 | enc_create, enc_media_config |
| R05, R12 | enc_two_outputs, enc_flow_sync, enc_errors, out_destroy_active, start_basic |
| R06, R07 | enc_flow_sync |
| R08, R09, R10, R11 | enc_packet_path, enc_hold_bframes, enc_two_outputs, enc_flow_sync |
| R20, R21 | out_guards, out_start_stop |
| R22, R23 | out_start_failures, start_basic |
| R24, R25 | out_start_stop, out_start_failures, dl_start |
| R26, R27 | start_basic, rc_success, enc_flow_sync, dl_start |
| R30, R31, R32 | out_start_stop, dl_force_stop, rc_veto_and_stop |
| R33, R34, R35, R36, R37 | out_signal_stop, out_start_stop, out_destroy_active, rc_success |
| R40 | il_steady_state, dl_start |
| R41 | il_gating_keyframe |
| R42, R43 | il_start_basic, il_multitrack_video, il_multitrack_audio, il_steady_state |
| R44, R45, R46, R47 | il_start_basic, il_prune_far_audio, il_prune_two_audio_tracks, il_offsets, il_audio_priming |
| R48, R49 | il_steady_state, il_batch_overflow |
| R50 | start_basic, il_batch_accumulates |
| R60 | rc_veto_and_stop, out_signal_stop |
| R61, R62 | rc_backoff, rc_backoff_cap, rc_success |
| R63 | rc_veto_and_stop |
| R70 | out_guards, dl_start |
| R71, R72, R73, R74, R75 | dl_start, dl_stop_drop, dl_force_stop, dl_reconnect_preserve, dl_reconnect_nopreserve |
| Q-1 .. Q-10 | enc_packet_path, il_prune_two_audio_tracks, out_signal_stop, out_start_stop, il_prune_far_audio, il_batch_accumulates, dl_force_stop, dl_stop_drop |
| D-1, D-2, D-3 | not in vectors (precondition violations / races); D-3 visible in rc_backoff |

The rule labels R-E / R-O / R-I / R-C / R-D used in text map to the numeric ranges R01-R12, R20-R37, R40-R50, R60-R63, R70-R75.

## 11. Open questions / not specified

* Pause/unpause of encoded outputs and encoders (offsets, `pause.ts_offset`, timestamps of paused packets).
* Closed-caption insertion into video packets (SEI), the caption track data, `obs_output_caption*`.
* Encoder groups (shared start timestamp, reconfigure requests, keyframe-group alignment and the "Missing keyframe" warning), `obs_encoder_group_*`.
* GPU texture encoding, GPU rescale mixes, ROI; raw (unencoded) outputs and their audio/video callbacks and conversion.
* Packet callbacks and the packet-timing metrics (FER, FERC, CTS, PIR) beyond being carried along.
* Whether `R-C03`'s exponent randomisation `[1.5, 1.55)` is relied on by anything: the vectors pin it to 1.5 through a test hook.
* The unspecified order of `stop` (main thread) and `deactivate` (end thread); the vectors fix `stop` before `deactivate`.
* `D-1`: required behaviour is stated but cannot be demonstrated without aborting the reference.

## 12. Round 9: `obs-output.c` object model & `obs-output-delay.c` loop (added)

### Reference files (all cited)

- `libobs/obs-output.c`, `libobs/obs-output-delay.c`

- **S-041-R76** `obs-output.c` object model: output registration/lookup (`find_output`, `obs_output_get_display_name`, `obs_output_get_module`, `obs_get_output_flags`), state (`obs_output_load_state`/save pair, `get_defaults`), lifecycle (`obs_output_create`/`init_output_handlers`/`obs_output_destroy` with `free_packets`, `clear_raw_audio_buffers`, `destroy_caption_track`), start/stop (`obs_output_start`/`obs_output_actual_start`, `obs_output_stop`/`obs_output_actual_stop`, `obs_output_force_stop`), and the per-packet flag logging helpers `log_flag_encoded`/`log_flag_video`/`log_flag_audio`/`log_flag_service` + `log_frame_info` — the start/stop ordering and packet routing are pinned in the earlier sections; this rule pins the file's remaining surface (the 146 non-trivial functions of the output object).
- **S-041-R77** `obs-output-delay.c`: the output-delay loop — `obs_output_delay_start`/`obs_output_delay_stop` (start/stop the delay thread + active flag), `obs_output_set_delay`/`obs_output_get_active_delay`, `process_delay`/`process_delay_data` (the timer: pop queued packets whose delay has elapsed), `push_packet`/`pop_packet` (the delayed-packet queue), `obs_output_cleanup_delay`, `log_flag_encoded` — delay semantics per the earlier sections.

## 13. Round 9: obs-outputs plugin — null output, module glue, utils (added)

Section 12 pinned the libobs object model this plugin builds on; this section pins the
`plugins/obs-outputs` files that were not cited: the null output (a consumer of the encoded-output
object), the plugin module glue, and the small bit helpers.

### Reference files (all cited)

- `plugins/obs-outputs/null-output.c`, `plugins/obs-outputs/obs-outputs.c`, `plugins/obs-outputs/utils.h`

- **S-041-R78 [SRC]** `null-output.c` (7 fns): `struct null_output { obs_output_t *output; pthread_t
  stop_thread; bool stop_thread_active }`. Info: id `null_output`, flags `AV | ENCODED |
  MULTI_TRACK_AV`, `get_name` returns the **literal** string `Null Encoding Output` (not a module
  text key, Q-10), callbacks create/destroy/start/stop/encoded_packet. create: zeroed object,
  stores the output handle, settings ignored. start: `obs_output_can_begin_data_capture(output, 0)`
  failure → false; `obs_output_initialize_encoders(output, 0)` failure → false; if a stop thread is
  still active join it first; `obs_output_begin_data_capture(output, 0)`; true. stop(`ts` ignored):
  `stop_thread_active := (pthread_create(stop_thread) == 0)` — the end of the capture runs
  **asynchronously**. stop_thread: `obs_output_end_data_capture(output)`, then
  `stop_thread_active = false`. encoded_packet: no-op (packets are dropped). destroy: join the stop
  thread if active, free. (The join-in-start is what makes a stop→start race safe; the output
  object's own start/stop ordering is the earlier sections'.)
- **S-041-R79 [SRC]** `obs-outputs.c` (6 fns): module description "OBS core RTMP/FLV/null outputs",
  locale en-US. `obs_module_load`: on `_WIN32` `WSAStartup(MAKEWORD(2, 2), ...)` (result ignored);
  on `_WIN32` + `MBEDTLS_THREADING_ALT`: `mbedtls_threading_set_alt` with a CRITICAL_SECTION-backed
  implementation (init: bzalloc + InitializeCriticalSection; free: Delete + free + NULL; lock/unlock:
  Enter/Leave, always return 0); then register the five outputs in order: `rtmp_output_info`,
  `null_output_info`, `flv_output_info`, `mp4_output_info`, `mov_output_info`.
  `obs_module_unload`: on `_WIN32` — `mbedtls_threading_free_alt()` (if `MBEDTLS_THREADING_ALT`)
  then `WSACleanup()`.
- **S-041-R80 [SRC]** `utils.h` (3 fns): the bit helpers used by the plugin's RTMP codec helpers
  (rtmp-av1.c, rtmp-hevc.c): `clz32` / `ctz32` — GCC/clang: `__builtin_clz` / `__builtin_ctz`;
  MSVC: clz via `_BitScanReverse` → `31 - index` (BSR chosen deliberately over `_lzcnt_u32` for
  older-Intel compatibility), ctz via `_tzcnt_u32` (x86) or `_CountTrailingZeros` (ARM64); other
  compilers: portable `popcnt`, clz = `32 - popcnt(bitspread(x))`, ctz = `popcnt((x & -x) - 1)`.
  Plus the trivial `min_u32` / `min_u16` / `min_i32` / `max_u8` inlines.

### Quirks (added)

- Q-10: the null output's `get_name` returns a hardcoded English literal, while the other outputs of
  the same plugin go through `obs_module_text` — it never localises.
