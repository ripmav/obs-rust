# S-032 `audiomix` (audio mixer callback, source audio ingestion/render, faders, volmeters)

| | |
|---|---|
| Status | Draft (vectors generated and stable; Rust sufficiency check NOT performed, see section 11) |
| Reference | libobs 33.0.1 `obs-audio.c` (`audio_callback` and helpers), `obs-audio-controls.c/.h`, and the audio parts of `obs-source.c` |
| Target crate | `crates/obs-audio-mix` |
| Depends on | S-004 (signals: "volume", "destroy"), S-005 (deque), S-008 (numeric tolerance), S-013 (`audio_frames_to_ns`, `mul_to_db`, `db_to_mul`, enums, channel counts) |
| Conformance | `test/conformance/scripts/audiomix/` (39 scripts, runner `gen/audiomix_runner.c`, stubs `gen/audiomix_stubs.c`, `gen/audiomix.sources`) |

## 1. Scope

In scope: (a) the mixing callback the audio thread invokes once per 1024-frame tick; (b) how a source's audio
reaches that callback: `obs_source_output_audio` ingestion into a per-channel FIFO, timestamp smoothing, sync
offset, balance, force-mono, filters, and `obs_source_audio_render` (what the mixer receives: per-mix, per-channel
1024-float buffers with volume, mute and mixer masks applied); (c) buffering decisions; (d) faders and volmeters.

Out of scope: the audio thread itself and the post-mix clamp (`audio-io.c`, see phase-2 guide), the real
resampler (only its hook contract), scene/group audio rendering (`obs-scene.c`), transitions, audio monitoring
output devices, hotkey registration, filter implementations.

Callers: the audio thread (`audio_callback`), plugins (`obs_source_output_audio`, `obs_source_get_audio_mix`),
the frontend (faders, volmeters, mute/volume setters).

## 2. Terminology and conventions

RFC 2119 words apply. `R` = output sample rate, `C` = output channel count (1..8, from the output speaker
layout), `F` = 1024 (`AUDIO_OUTPUT_FRAMES`). Timestamps are `u64` nanoseconds; all subtraction/addition on
timestamps is **wrapping** (the reference is unsigned C). `mul_div(a,m,d) = (a / d) * m + ((a % d) * m) / d`
using wrapping 64-bit arithmetic (this is the POSIX `util_mul_div64`; it is not exact for huge values and a
conforming implementation MUST use this formula). `frames_to_ns(f) = mul_div(f, 1e9, R)`,
`ns_to_frames(t) = mul_div(t, R, 1e9)` (as `usize`). Sample buffers are `f32`.

Float tolerance: relative or absolute **1e-5** for single values; **1e-4 relative** for the `sum=` aggregates of
up to 1024 samples; `-0` equals `0`; `inf/-inf/nan` must match as classes. Integers, flags, counters and
timestamps are exact. Log lines: only the level is compared.

Constants: `MAX_AUDIO_MIXES 6`, `MAX_AUDIO_CHANNELS 8`, `MAX_AV_PLANES 8`, `MAX_CHANNELS 64` (source channels of a
view), `MAX_TS_VAR 2_000_000_000`, `TS_SMOOTHING_THRESHOLD 70_000_000`, `MAX_BUF_SIZE = 1000*1024*4` bytes
(per channel FIFO), default `max_buffering_ticks = 45` (when the audio config gives no `max_buffering_ms`;
otherwise `(ms*R/1000)/1024`).

## 3. Interface

### 3.1 State (normative field semantics; layout is not ABI)

Core audio state `A`: `render_order: [Source]`, `root_nodes: [Source]`, `buffered_ts: u64`, `buffered_timestamps:
deque<(start,end)>`, `buffering_wait_ticks: u64`, `total_buffering_ticks: i32`, `max_buffering_ticks: i32`,
`fixed_buffer: bool`, `tasks: FIFO of closures`, `monitoring_duplicating_source: Option<Source>`.
Data state `D`: `first_audio_source` singly linked list (new audio sources are inserted at the HEAD, so iteration is
newest first). Video: list of mixes, each `{view, mix_audio}`; a view has 64 source channels.

Per-source audio state (fields used by this spec): `audio_ts u64` (0 = none), `audio_pending bool`,
`pending_stop bool`, `input_buf[8]` (FIFO of f32 per channel; `size` below means frames), `last_input_size`,
`output_buf[6][8][1024] f32` (allocated for sources with the AUDIO or COMPOSITE flag; zero initially),
`audio_mixers u32` (default 0xFF), `volume`/`muted`/`ptt_pressed`/`ptm_pressed` (audio-thread copies),
`user_volume` (default 1.0), `user_muted`, `user_ptt_pressed`, `user_ptm_pressed`, `enabled` (default true),
`push_to_mute_enabled/delay_ms/stop_time`, `push_to_talk_enabled/delay_ms/stop_time`, `balance` (default 0.5),
`sync_offset i64`, `last_sync_offset i64`, `flags` (bit1 = FORCE_MONO), `monitoring_type`
(0 none, 1 monitor-only, 2 monitor-and-output), `monitoring_enabled`, `timing_set`, `timing_adjust u64`,
`resample_offset u64`, `next_audio_ts_min`, `next_audio_sys_ts_min`, `sample_info {rate, format, speakers}`
(all zero initially), `audio_failed`, `audio_data` (last processed packet), `audio_actions: queue of {timestamp,
kind VOL|MUTE|PTT|PTM, value}`, `audio_is_duplicated`, `activate_refs`, `removed`, `async_unbuffered`,
`async_decoupled`, capture callbacks list, `filters` list. Source kinds: *input* (type INPUT, flag AUDIO),
*audio_render* source (has a custom `audio_render` callback), *composite* (flag COMPOSITE; enumerates children),
*submix* (flag SUBMIX, has `audio_mix` callback).

### 3.2 Functions

```
audio_callback(start, end, mixers: u32, mixes: [6][C][1024] f32 (pre-zeroed, caller owned)) -> (ret: bool, out_ts: u64)
obs_source_output_audio(src, packet{planes[8], frames, speakers, format, rate, timestamp})
obs_source_audio_render(src, mixers, channels, rate, size_bytes = 4096)
obs_source_set_volume / get_volume / set_muted / muted / set_balance_value / get_balance_value /
  set_sync_offset / set_audio_mixers / get_audio_mixers / push-to-mute|talk setters (state and signals only)
obs_fader_create/destroy/set_db/get_db/set_deflection/get_deflection/set_mul/get_mul/attach_source/
  detach_source/add_callback/remove_callback, obs_fader_db_to_def(fader) -> fn(f32)->f32
obs_volmeter_create/destroy/attach_source/detach_source/set_peak_meter_type/get_nr_channels/
  add_callback/remove_callback, obs_mul_to_db, obs_db_to_mul
```

Fader types: `CUBIC 0`, `IEC 1`, `LOG 2`; peak meter types `SAMPLE_PEAK 0`, `TRUE_PEAK 1`. Volmeter callback payload:
`(magnitude[8], peak[8], input_peak[8])`, all in dB (`f32`), fader callback `(db: f32)`.

## 4. Data formats

No byte formats. Buffers: planar f32. Layout facts that are observable: the six mixes of one source
are 8 consecutive channel planes of 1024 floats each (a "zero N channels of mix m" operation clears channels
0..N-1), and `obs_source_get_audio_mix` exposes pointers to those planes.

Worked example (`scripts/audiomix/10_basic_mix.script`): 48 kHz stereo, one source, three 1024-frame blocks of a
0.5-amplitude 440 Hz sine pushed at `T0, T0+21333333, T0+42666666` (`T0 = 1e9`). `ns(1024)=21333333`, so the
ticks cover `[1e9, 1021333333)`, `[1021333333, 1042666666)`, ...; the source's `audio_ts` after the run is
`1064000000` (= end of tick 2) and its input FIFO is empty. Mix 0 channel 0 gets `0.5*sin(...)`, channel 1 gets
`0.75` of that (generator gain; the mixer itself does not scale channels).

## 5. Behaviour (numbered rules)

### 5.1 Ingestion: `obs_source_output_audio`

- **S-032-R01** Planes at index >= `planes(format, speakers)` of the incoming packet are treated as absent. The
  packet is processed by R02..R07 and then offered to `source_output_audio_data` (R08..R14).
- **S-032-R02** (resampler hook) If the packet's `(rate, format, speakers)` differs from the source's stored
  `sample_info`, the source stores the new `sample_info`, drops any resampler, sets `resample_offset = 0`, and: if
  the packet equals the output format `(R, float planar, output speakers)` then `audio_failed = false` and no
  resampler exists; otherwise a resampler is created (`audio_failed = creation failed`; creation failure logs
  ERROR). Because `sample_info` starts all-zero, the first packet always runs this step.
- **S-032-R03** If `audio_failed`, the packet is not converted (see D-2 for what happens next). Otherwise: with a
  resampler, the packet is converted to `(R, float planar, C channels)`, producing `frames_out` and a
  `ts_offset` that is stored into `resample_offset` on every call; without one the planes are copied.
  `audio_data = {frames, timestamp = packet.timestamp (not adjusted)}`. The resampler algorithm is out of
  scope; implementers expose it as a trait. The conformance harness uses a zero-order-hold stand-in.
- **S-032-R04** Balance: if `C != 1` and the source's `sample_info.speakers == STEREO` and
  `balance > 0.51 || balance < 0.49`, then per frame `ch0 *= sinf(f32((1-balance) * (pi/2)))` and
  `ch1 *= sinf(f32(balance * (pi/2)))`, the product computed in double (`f32 * f64`) and rounded to f32 before
  `sinf`. Other balance values (inside the dead band) and other layouts are untouched.
- **S-032-R05** Force-mono: if `C != 1` and `flags & FORCE_MONO`: `ch0 = (ch0 + ch1 + ... + ch(C-1)) * (1.0f /
  (float)C)` (f32, summed in channel order), then ch0 is copied to channels 1..C-1.
- **S-032-R06** Filters: iterate the filter list from the last to the first; skip disabled filters and filters
  without `filter_audio`; each filter maps `audio_data` to new data or returns NULL. If any returns NULL the
  packet is dropped here (nothing stored, no capture callbacks, no timing update).
- **S-032-R07** The result `(planes, frames, timestamp)` is called `data`; steps R08..R14 run under the source's audio mutex.

### 5.2 Timestamp handling (`source_output_audio_data`)

Let `os = os_gettime_ns()` and `in = data`.

- **S-032-R08** If `|in.ts - os| < MAX_TS_VAR`: `timing_adjust = 0; timing_set = true; direct = true`.
- **S-032-R09** If `!timing_set`: `timing_set = true; timing_adjust = os - in.ts`. Else if `next_audio_ts_min != 0`: with
  `diff = |next_audio_ts_min - in.ts|`: if `diff > MAX_TS_VAR && !direct` (timestamp jump): log DEBUG,
  `timing_set = true; timing_adjust = os - in.ts`, clear all input FIFOs, `last_input_size = 0`,
  `audio_ts = os; next_audio_sys_ts_min = os`; else if `diff < 70e6`: (if `async_unbuffered && async_decoupled`:
  `timing_adjust = os - in.ts`) and `in.ts = next_audio_ts_min` (smoothing); else log DEBUG (no change).
- **S-032-R10** `next_audio_ts_min = in.ts + frames_to_ns(in.frames)`; then `in.ts += timing_adjust`.
- **S-032-R11** `push_back = false`. If `next_audio_sys_ts_min == in.ts`: `push_back = true`. Else if
  `next_audio_sys_ts_min != 0`: `diff = |next_audio_sys_ts_min - in.ts|`; `diff < 70e6` -> `push_back = true`;
  `diff > MAX_TS_VAR` -> `timing_set = true; timing_adjust = os - data.ts` (the *unmodified* packet timestamp) and
  `in.ts = data.ts + timing_adjust`.
- **S-032-R12** `in.ts += sync_offset; in.ts -= resample_offset;` (wrapping). `next_audio_sys_ts_min =
  next_audio_ts_min + timing_adjust`. If `last_sync_offset != sync_offset`: if `last_sync_offset != 0` then
  `push_back = false`; `last_sync_offset = sync_offset`.
- **S-032-R13** Unless `monitoring_type == MONITOR_ONLY` (then nothing is stored): if `push_back && audio_ts != 0` ->
  *append* (R15), else *place* (R16).
- **S-032-R14** After releasing the buffer lock, every registered capture callback is invoked (newest registration first)
  with the **unadjusted** `data` and `muted = source_muted(os)` (R22), even for monitor-only sources.

### 5.3 Buffer writes

- **S-032-R15** *Append*: if `input_buf[0].size_bytes + frames*4 > MAX_BUF_SIZE` the packet is silently dropped. Else
  push `frames` samples on every one of the `C` channel FIFOs and set `last_input_size = 0`.
- **S-032-R16** *Place*: if `audio_ts == 0 || in.ts < audio_ts`: clear all FIFOs, `last_input_size = 0`, `audio_ts =
  in.ts`, `next_audio_sys_ts_min = in.ts`. Then `off = ns_to_frames(in.ts - audio_ts)`; if `(off+frames)*4 >
  MAX_BUF_SIZE` the packet is dropped silently (note: `last_input_size` unchanged). Otherwise for each channel write
  the samples at FIFO offset `off` (if past the end, the gap is zero filled), then **truncate** the FIFO to exactly
  `off + frames` samples (anything stored after the written region is discarded), and set `last_input_size = 0`.

### 5.4 Muting predicates and volume

- **S-032-R17** `obs_source_set_volume(v)`: creates an action `{ts = os_gettime_ns, VOL, v}`, emits signal "volume" with
  calldata `{source, volume: v}` (and "source_volume" on the core for public sources), reads `volume` back from the
  calldata (handlers may change it), queues the action (with the ORIGINAL `v`), and sets `user_volume` to the value
  read back (see Q-3). `get_volume` returns `user_volume` (0 for an invalid source).
- **S-032-R18** `set_muted(m)`: `user_muted = m`, signal "mute", queue action `{ts = now, MUTE, m}`. `set_balance_value`,
  `set_sync_offset`, `set_audio_mixers` emit "audio_balance" / "audio_sync" / "audio_mixers" with the value in the
  calldata, then store the value read back; `set_audio_mixers` returns early if equal or if the source lacks the
  AUDIO flag.
- **S-032-R19** Push-to-mute / push-to-talk hotkeys: set `user_ptm_pressed` / `user_ptt_pressed` and queue a PTM / PTT action with
  `ts = now`. Enabling (`obs_source_enable_push_to_*`) logs INFO when the value changes on an audio source; delay is
  stored in ms.
- **S-032-R20** Applying an action (audio thread): VOL sets `volume`, MUTE sets `muted`, PTT sets `ptt_pressed`, PTM sets `ptm_pressed`.
- **S-032-R21** `get_source_volume(os_time)` (audio thread; `os_time` = the source's `audio_ts`, not the wall clock):
  if `ptm_enabled && ptm_pressed`: `ptm_stop = os_time + ptm_delay*1_000_000`; likewise for talk. `ptm_active = ptm_pressed ||
  os_time < ptm_stop`, `ptt_active = ptt_pressed || os_time < ptt_stop`. `muted = !enabled || muted_flag || (ptm_enabled
  && ptm_active) || (ptt_enabled && !ptt_active)`. Result: `0` if `muted` or `|volume| <= 1e-4`; `1` if `|volume-1| <= 1e-4`; else `volume`.
- **S-032-R22** `source_muted(os_time)` (callback flag) uses the *user* copies: `!enabled || user_muted || (ptm_enabled &&
  (user_ptm_pressed || os_time < ptm_stop)) || (ptt_enabled && !(user_ptt_pressed || os_time < ptt_stop))`, after first
  updating `ptm_stop/ptt_stop` from the user pressed flags exactly as R21 does with the audio-thread flags.

### 5.5 Render: what the mixer receives

`obs_source_audio_render(src, mixers, channels, R, size)`, `size = 4096`:

- **S-032-R23** No `output_buf` (non-audio, non-composite source): `audio_pending = true`, return.
- **S-032-R24** Source with `audio_render`: if it has no context data -> pending, return. Else *custom render*: for each mix `m`: if
  `src.audio_mixers & mixers & (1<<m)`, zero channels 0..channels-1 of `output_buf[m]`. Call the callback with the
  six-mix buffer set; `audio_ts = ok ? ts : 0; audio_pending = !ok`. If `!ok || audio_ts == 0 || mixers == 0` return. For each mix in
  `mixers` not in `src.audio_mixers`: zero channels 0..channels-1. Then apply volume (R28). Mixes not in `mixers` are not touched.
- **S-032-R25** Submix source (`audio_mix` callback): call it with a scratch `channels x 1024` buffer (zeroed first); on success feed
  it to the normal ingestion (R01..R14) as float-planar packet at `R`, speakers = `channels`, frames 1024, the reported timestamp. The
  render then continues with R26.
- **S-032-R26** If `audio_ts == 0`: `audio_pending = true`, return. Otherwise *tick*:
  - if `input_buf[0]` holds fewer than 1024 frames: `audio_pending = true`, return (output buffers untouched).
  - copy the first 1024 frames of each channel (non-destructive peek) into `output_buf[0][ch]`.
  - for `m` in `1..5`: for a submix, `m > 1` stops the loop and `mixers`/mask are replaced by `1` (so mix 1 is
    filled from the bit-0 test). If `(src.audio_mixers & bit) == 0 || (mixers & bit) == 0` zero channels 0..channels-1 of
    `output_buf[m]`, else copy mix 0's channels into it.
  - submix: `audio_pending = false`, return (no volume applied).
  - if `(src.audio_mixers & 1) == 0 || (mixers & 1) == 0` zero channels of `output_buf[0]`.
  - apply volume (R28); `audio_pending = false`.
- **S-032-R27** `obs_source_audio_pending` returns true for invalid or removed sources and for sources that are neither
  composite nor audio; otherwise `audio_pending`.
- **S-032-R28** *Apply volume* `(src, mixers, channels)`: if the action queue is non-empty and `actions[0].ts < audio_ts +
  frames_to_ns(1024)`, run R29 and return. Else `v = get_source_volume(audio_ts)`; if `v == 1.0` return; if `v == 0.0 || mixers == 0`
  zero the **entire** `output_buf` (all 6 mixes, 8 channels); else for each mix with the bit set in both `src.audio_mixers` and
  `mixers`, multiply channels 0..channels-1 of `output_buf[m]` by `v`.
- **S-032-R29** *Actions inside the tick*: `cur = get_source_volume(audio_ts)`, `frame = 0`. For the queue in order: `t = max(action.ts,
  audio_ts)`, `nf = ns_to_frames(t - audio_ts)`; if `nf >= 1024` stop (the action and all later ones stay queued); remove the
  action, apply it (R20); if `nf > frame`: `vol[frame..nf) = cur; frame = nf`; then `cur = get_source_volume(t)`. Finally `vol[frame..1024) =
  cur`. For every mix whose bit is set in `src.audio_mixers` (the `mixers` argument is NOT consulted), multiply channel c frame i of
  `output_buf[m]` by `vol[i]`.

### 5.6 The mixing callback

- **S-032-R30** (setup) `R`, `C` are read from the audio output. Clear `render_order` (releasing references) and `root_nodes`. Push
  `(start,end)` onto the BACK of `buffered_timestamps`, then read the FRONT entry into `ts` (an entry pushed to the front by
  buffering is therefore processed first). `min_ts = ts.start`.
- **S-032-R31** (render order) For each video mix in order that has a view, for source channel `i` in `0..63`: skip empty slots, sources
  with `activate_refs == 0`, and removed sources. If the mix has `mix_audio`, append the source to `root_nodes`. Run
  `enum_active_tree(source, add2)`, then `add1(source)`. `enum_active_tree` visits children post-order: for a child, first its own
  children (recursively) and then the child itself, with the callback receiving each; sources without `enum_active_sources` (and not
  transitions) contribute nothing. `add1(s)`: if `s` is not in `render_order` append it and set `s.audio_is_duplicated = false`.
  `add2(s)`: skip if removed; if not in `render_order` append (and clear the flag); if present and `s` is an individual audio
  source (type INPUT, flag AUDIO, not COMPOSITE) and not yet flagged, append `s` to `root_nodes` and set the flag.
- **S-032-R32** Then, over the audio-source list (newest first) hold the list lock: every non-removed source gets `add1`. (Audio sources
  that are in no view are rendered, but are never roots.)
- **S-032-R33** (render) For each source in `render_order`: `obs_source_audio_render(src, mixers, C, R, 4096)`. If
  `should_silence_monitored_source(src)` (R37) zero `output_buf[m]` channels 0..7 for every mix `m` set in both `dup.audio_mixers` and
  `src.audio_mixers`. Then if the buffering is maxed (R36) and `src.audio_ts != 0 && src.audio_ts < ts.start`: for an `audio_render` source log
  DEBUG and set `audio_pending = true`; otherwise run *ignore_audio* (R39); if it returns true render the source again.
- **S-032-R34** (min timestamp) `buffering_name = calc_min_ts`: `find_min_ts` visits the audio-source list and takes the minimum `audio_ts` of
  sources with `!audio_pending && audio_ts != 0 && audio_ts < min_ts`, remembering the source name; `mark_invalid_sources` runs
  *buffer insufficient* (R38) on every audio source; if any returned true `find_min_ts` is run again (it continues from the current `min_ts`).
- **S-032-R35** (buffering) If `fixed_buffer`: if not maxed, `set_fixed_audio_buffering`. Else if `min_ts < ts.start`: `add_audio_buffering`.
  Both algorithms are in section 6.
- **S-032-R36** `maxed := total_buffering_ticks == max_buffering_ticks` (equality, not >=).
- **S-032-R37** `should_silence_monitored_source(src)`: false if there is no `monitoring_duplicating_source` or it is not active
  (`activate_refs == 0`). `muted_dup = |dup.volume - 0| <= 1e-4` (uses the AUDIO-THREAD copy `volume`); `unmuted = !dup.muted && !muted_dup`
  (audio-thread copy `muted`). Result `unmuted && src.monitoring_enabled && src != dup`.
- **S-032-R38** *buffer insufficient* `(src, min_ts)`: false if `src` has `audio_render`, or is pending, or `audio_ts == 0`. If `audio_ts !=
  min_ts && audio_ts != min_ts-1`: `sp = ns_to_frames(audio_ts - min_ts)`; if `sp >= 1024` return false; `need = 1024 - sp`; else `need = 1024`. If the
  FIFO holds fewer than `need` frames: `audio_pending = true`, return true; else false.
- **S-032-R39** *ignore_audio* `(src, start_ts)` (under the source buffer lock; `n` = FIFO frames): if `audio_ts == 0 && n > 0`: clear the FIFOs,
  `last_input_size = 0`, return false. If `n > 0`: `drop = mul_div(start_ts - audio_ts - 1, R, 1e9) + 1`, clamped to `n`; pop `drop` frames from each
  channel; `last_input_size = 0`; `audio_ts += mul_div(drop, 1e9, R)`; log DEBUG; if `audio_ts == start_ts - 1` then `audio_ts = start_ts`; if `audio_ts >= start_ts` return true.
  Then (the source is still behind, or had no data): if `!audio_pending || n > 0` log WARNING; set `audio_pending = true; audio_ts = 0; timing_set = false`; return false.
- **S-032-R40** (mix) If `buffering_wait_ticks == 0`: for each root node in order: skip if `audio_pending`; if it has `output_buf` and `audio_ts != 0`: *mix_audio*:
  nothing if `audio_ts < ts.start || ts.end <= audio_ts`; `sp = 0` if `audio_ts == ts.start` else `ns_to_frames(audio_ts - ts.start)` (nothing if `sp == 1024`);
  for ALL six mixes and channels 0..C-1: `mix[m][c][sp + i] += output_buf[m][c][i]` for `i in 0..1024-sp`. The `mixers` argument is not consulted here (a
  duplicated root is added twice, Q-2).
- **S-032-R41** (discard) For each source of the audio-source list (newest first), under its buffer lock, run *discard_audio* (R42).
- **S-032-R42** *discard_audio* `(src, ts)`: if `src` has `audio_render`: `audio_ts = 0`, return. If `ts.end <= audio_ts` return. If `audio_ts < ts.start - 1`: if
  `audio_pending && FIFO bytes < 4096 && discard_if_stopped` return; return. Total `t = 1024`; if `audio_ts != ts.start && audio_ts != ts.start - 1`:
  `sp = ns_to_frames(audio_ts - ts.start)`; if `sp == 1024` return; `t = 1024 - sp`. If the FIFO holds fewer than `t` frames: if `discard_if_stopped` return; else
  `audio_ts = ts.end`; return. Otherwise pop `t` frames from every channel, `last_input_size = 0`, `pending_stop = false`, `audio_ts = ts.end`.
- **S-032-R43** `discard_if_stopped(src)`: `size` = FIFO frames of channel 0; if 0 return false. If `last_input_size == size`: if `!pending_stop`: `pending_stop = true`, return false;
  else clear all FIFOs, `pending_stop = false`, `audio_ts = 0`, `last_input_size = 0`, return true. Else `last_input_size = size`; return false.
- **S-032-R44** (finish) Release the render-order references; pop the FRONT entry of `buffered_timestamps`; `out_ts = ts.start`. If `buffering_wait_ticks > 0`:
  decrement it, return `false` (queued tasks do not run). Else run all queued audio tasks (FIFO order, lock released while a task runs) and return `true`.

### 5.7 Faders

- **S-032-R45** Curves, all `f32`. *Cubic*: `def_to_db(d) = 0 if d == 1; -inf if d <= 0; else mul_to_db(d*d*d)`; `db_to_def(db) = 1 if db == 0; 0 if db == -inf; else cbrtf(db_to_mul(db))`.
  *IEC*: `def_to_db`: `1 -> 0`; `<= 0 -> -inf`; `>= 0.75: (d-1)/0.25*9`; `>= 0.5: (d-0.75)/0.25*11 - 9`; `>= 0.3: (d-0.5)/0.2*10 - 20`; `>= 0.15: (d-0.3)/0.15*10 - 30`;
  `>= 0.075: (d-0.15)/0.075*10 - 40`; `>= 0.025: (d-0.075)/0.05*10 - 50`; `>= 0.001: (d-0.025)/0.025*90 - 60`; else `-inf`. `db_to_def`: `0 -> 1`; `-inf -> 0`;
  `>= -9: (db+9)/9*0.25 + 0.75`; `>= -20: (db+20)/11*0.25 + 0.5`; `>= -30: (db+30)/10*0.2 + 0.3`; `>= -40: (db+40)/10*0.15 + 0.15`; `>= -50: (db+50)/10*0.075 + 0.075`;
  `>= -60: (db+60)/10*0.05 + 0.025`; `>= -114: (db+150)/90*0.025`; else `0` (the pieces are NOT continuous at -60/-114, Q-5; values above 0 dB continue the first piece).
  *Log*: `def_to_db(d) = 0 if d >= 1; -inf if d <= 0; else -(102) * (102/6)^(-d) + 6` (`powf`); `db_to_def(db) = 1 if db >= 0; 0 if db <= -96; else
  (-log10f(-db + 6) - LOG_RANGE_VAL) / (LOG_OFFSET_VAL - LOG_RANGE_VAL)` with `LOG_OFFSET_VAL = -0.77815125038364363`, `LOG_RANGE_VAL = -2.00860017176191756`.
  Limits: cubic/IEC `max_db = 0, min_db = -inf`; log `max_db = 0, min_db = -96`. Unknown type -> `create` returns NULL.
- **S-032-R46** `set_db(db)`: NULL fader -> false. `cur = db`; if `cur > max_db`: `cur = max_db`, clamped; if `cur < min_db`: `cur = -inf`, clamped. Sets `ignore_next_signal = true` (even with no source),
  and if a source is attached calls `set_volume(source, db_to_mul(cur))`. Returns `!clamped`. `set_deflection(d) = set_db(def_to_db(d))`, `set_mul(m) = set_db(mul_to_db(m))`. Getters:
  `get_db = cur` , `get_deflection = db_to_def(cur)`, `get_mul = db_to_mul(cur)`; all getters return `0.0` for NULL, setters `false`. A fresh fader has `cur_db = 0`.
- **S-032-R47** `attach_source`: NULL arguments -> false; detaches any previous source; connects handlers on the source's "volume" and "destroy" signals; `cur_db = mul_to_db(get_volume(source))`.
  `detach` disconnects both and forgets the source (idempotent). The destroy signal detaches. `destroy` detaches and frees.
- **S-032-R48** "volume" handler: if `ignore_next_signal`: clear it, do nothing. Else `cur_db = mul_to_db(calldata.volume)` and invoke the fader callbacks **newest registration first** with the new dB.
  `set_db` itself does not invoke callbacks. `remove_callback` removes the first entry equal to `(fn, param)`.
- **S-032-R49** `obs_fader_db_to_def(fader)` returns the `db_to_def` curve of that fader type.

### 5.8 Volmeters

- **S-032-R50** `create(type)` stores the type (unused), `peak_meter_type = SAMPLE`, history/peaks/magnitudes zero. `attach_source` (NULL -> false): detach previous; connect "volume" (updates `cur_db =
  mul_to_db(calldata.volume)`) and "destroy" (detach); register the audio-capture callback on the source; `cur_db = mul_to_db(get_volume(source))`. `detach` disconnects and unregisters.
  `get_nr_channels = clamp(channels(source.sample_info.speakers) or 0 if unattached, 0, channels(output speakers) or 2 if there is no audio info)`.
- **S-032-R51** On a capture callback `(data, muted)` under the meter lock: `nr = clamp(count of non-null planes among 8, 0, 8)`; then *peak* (R52), *magnitude* (R53); `mul = (muted && !source.user_muted) ? 0 : db_to_mul(cur_db)`;
  for ch in 0..7: `magnitude_db[ch] = mul_to_db(magnitude[ch]*mul)`, `peak_db[ch] = mul_to_db(peak[ch]*mul)`, `input_peak_db[ch] = mul_to_db(peak[ch])`. Callbacks are then invoked (newest first) outside the meter lock.
  `cur_db` is the dB of the *source volume at last signal* (so a mute via `user_muted` leaves levels unscaled, and a push-to-mute/disable zeroes them).
- **S-032-R52** *Peak*: iterate the planes in index order, skipping null planes, assigning meter channel numbers 0,1,.. to the non-null ones until `nr` are done. Per channel: `prev` = the 4-sample history
  (initially 0). `n = frames`. *Sample peak* = max(|prev[0..4]|, |samples[i]| for i in the largest multiple of 4 not exceeding n, i.e. only `floor(n/4)*4` samples). *True peak* additionally evaluates, for
  each group of four new samples, interpolated points (algorithm in section 6) and includes their absolute values. Then the history is updated (R54) and `peak[ch]` stored. Channels `nr..7`: `peak = 0`.
  If a plane address is not 16-byte aligned, the reference prints a diagnostic to stdout, sets that channel's `peak = 1.0`, does NOT update its history, and still counts the channel (D-1).
- **S-032-R53** *Magnitude*: per non-null plane/channel: `sum = Σ s*s` (f32, in order), `magnitude = sqrtf(sum / n)` (so `frames == 0` gives NaN). Channels `>= nr` keep their previous magnitude (they are never reset; only `peak` is zeroed. In practice a
  channel count that shrinks reports stale magnitudes, Q-4).
- **S-032-R54** History update (per channel), with `n = frames`: `n == 0`: unchanged; `1..3`: shift the history left by `n` and append the new samples; `>= 4`: the last four samples.
- **S-032-R55** `set_peak_meter_type` stores the type (under the lock). Callback add/remove as R48. `obs_mul_to_db(m) = (m == 0) ? -inf : 20*log10f(m)`, `obs_db_to_mul(d) = finite(d) ? powf(10, d/20) : 0`.

## 6. Algorithms

### 6.1 Buffering (inputs: `ts`, `min_ts`, `R`)

```
ms(ticks) = ticks * 1024 * 1000 / R            (integer arithmetic)

add_audio_buffering(ts, min_ts):               # called only when min_ts < ts.start and !fixed
    if maxed: return
    if buffering_wait_ticks == 0: buffered_ts = ts.start
    frames = ns_to_frames(ts.start - min_ts); ticks = (frames + 1023) / 1024
    total += ticks
    if total >= max: ticks -= total - max; total = max; log WARNING "Max audio buffering reached!"
    log INFO (ms(ticks), ms(total), source name)
    new.start = buffered_ts - frames_to_ns(wait * 1024)
    while ticks-- > 0:
        wait += 1
        new.end = new.start
        new.start = buffered_ts - frames_to_ns(wait * 1024)
        push_front(buffered_timestamps, new)
    ts = new
set_fixed_audio_buffering(ts):                 # fixed mode; ticks = max - total
    same as above with ticks = max - total, total = max, one INFO log (ms(total))
```
`ts` after buffering replaces the working timestamp: the tick mixes that earlier window instead (but with `buffering_wait_ticks > 0` nothing is mixed
and the call returns `false`; mixes stay silent). Note `ticks` can be 0 (when `ticks - (total-max)` reaches 0) and then `new` is uninitialised in the
reference (it would copy stack garbage into `ts`); a conforming implementation SHOULD leave `ts` unchanged in that case (not covered by vectors).

### 6.2 True peak (per channel)

For each group of four new samples `x0..x3` with the work window `w = [p0,p1,p2,p3]` (initially the 4 history samples): `peak = max(peak, |x0..x3|)`;
then repeat 4 times: shift the window one sample (`w = [w1,w2,w3,x_k]`, `x_k` successively `x0..x3`), compute 4 interpolated values `out_j = Σ_i w_i * M_j[i]` with
with the coefficient vectors below. Normative form: `out = (w · m3, w · m1, w · p1, w · p3)` with
`m3 = [-0.103943, 0.233872, 0.935489, -0.155915]`, `m1 = [-0.189207, 0.504551, 0.756827, -0.216236]`,
`p1 = [-0.216236, 0.756827, 0.504551, -0.189207]`, `p3 = [-0.155915, 0.935489, 0.233872, -0.103943]`,
each dot product accumulated in the order `w0*c0 + w1*c1 + w2*c2 + w3*c3` (the SIMD reference sums element-wise products in a different
association order; the tolerance of section 2 covers it). The window starts as `prev` (history) and after the four shifts has consumed the group. The result is the maximum of all |values|.

## 7. Concurrency

The reference runs `audio_callback` on the audio thread and `obs_source_output_audio` on arbitrary capture threads. Locks: `audio_sources_mutex` (audio-source list), `mixes_mutex` + each view's
`channels_mutex` (render-order construction), per source `audio_buf_mutex` (FIFOs, `audio_ts`, `audio_pending`, timing fields, held by ingestion R11..R16, discard, ignore, mix and the peek in render), `audio_actions_mutex`
(action queue), `audio_mutex` (serialises R08..R14 per source), `audio_cb_mutex` (capture callbacks, held while calling them: a callback must not add/remove callbacks), `filter_mutex` (recursive). Fader and volmeter:
a state mutex plus a callback mutex held while callbacks run (callbacks must not re-enter the same fader/meter). Task callbacks run with the task mutex released. Lock order is not
documented; a Rust port should keep the same granularity (audio thread must never wait for a capture thread longer than a FIFO copy). The vectors are single-threaded; the mixing algorithm is a
function of the call sequence and the fake clock only. The volmeter's callback is invoked on the capture thread of the source.

## 8. Reference defects and quirks

- **Q-1** (CORE-adjacent) Monitoring-silence latency: `muted` of the audio-output-capture source is the audio-thread copy, applied only when that source is rendered, so a source rendered before it sees the old state for one tick
  (`37_monitoring`).
- **Q-2** A source that is a root node twice (same source in two mix_audio views, or twice in one view) is mixed twice (`19_double_root`: amplitude x3 for three roots).
- **Q-3** Signal handlers that rewrite `volume` change `user_volume` only; the queued audio action keeps the original (`45_fader_ignore_quirk`: `user_vol=0.4` while the audio thread later uses 0.8).
- **Q-4** Volmeter magnitudes of channels beyond the current plane count are never cleared; peaks are (`53_volmeter_channels`). Also the 4-sample history is included in each peak, so a silent block after a loud one still reports the old peak
  (`50_volmeter_basic`, last tick), and frames `% 4` trailing samples are not part of the block's own peak (`52_volmeter_framecount`).
- **Q-5** IEC `db_to_def` is not the inverse of `def_to_db` below -60 dB (`41_fader_curve_iec`).
- **Q-6** `obs_fader_set_db` arms `ignore_next_signal` even with no source, so the first external volume change after a later `attach` is swallowed (`45_fader_ignore_quirk`).
- **Q-7** `custom_audio_render` only zeroes mixes that are in `mixers`; stale data in other mixes is still added by `mix_audio` (harmless because the audio output only reads active mixes).
- **Q-8** `audio_buffer_insufficient` marks a source pending as a side effect of the min-timestamp calculation (`20_buffering_dynamic`).
- **D-1** Volmeter with a plane whose address is not 16-byte aligned: the reference `printf`s the pointer to stdout and reports peak 1.0 (also skips its history update). Required: report `peak = 1.0` for that channel, do not print to stdout,
  do not update the history; magnitude as normal (`56_volmeter_unaligned_quirk`; the runner discards the diagnostic). A Rust port without alignment constraints SHOULD NOT reproduce the misaligned case at all and MAY simply treat all planes as aligned; the vector does not
  compare the values.
- **D-2** After a resampler creation failure (`audio_failed`), `obs_source_output_audio` still continues past the conversion step and stores `source->audio_data`: an empty packet (`frames 0`, null planes: zero-length `memcpy` from NULL, UB found with UBSan) for a fresh source, or the
  PREVIOUS packet again for an established source (`39_failed_resampler_stale`, `34_resample_hook` source c). Required behaviour: **match the observable state of the reference**
  (the stale packet is re-ingested with the new timestamp; an empty packet runs the timing logic with `frames=0`); the zero-length copy from NULL MUST NOT crash.
- **D-3** `add_audio_buffering` with `ticks == 0` uses an uninitialised `new_ts` (not reachable through the vectors; see 6.1).

## 9. Implementation notes (non-normative)

Model `Source` audio state as a struct behind a `Mutex` for the buffer part and use `VecDeque<f32>` per channel (`place` = resize with zeros + overwrite + truncate). Keep `u64` arithmetic wrapping (`wrapping_sub`). Do not link a real clock: take a `Clock` trait. Keep `render_order` as `Vec<Arc<Source>>`
(the C reference holds references only for the duration of the call). The resampler is a trait returning `(frames, ts_offset)`. For volmeters the SSE code can be replaced by scalar code; stay within the tolerance. Fader curves must use `f32` (`cbrtf`, `powf`, `log10f`) - evaluate in `f32`, not `f64`.
The mixing callback allocates nothing per tick in the reference (`render_order` capacity is reused); keep it allocation-free.

## 10. Conformance coverage

| Rule | Scripts |
|---|---|
| R01-R03 | 34_resample_hook, 39_failed_resampler_stale, 10_basic_mix |
| R04, R05 | 35_balance_mono |
| R06 | 36_filter_audio |
| R07-R12 | 30_ingest_timestamps, 31_ingest_far_timestamp, 33_sync_offset |
| R13, R14 | 37_monitoring, 50_volmeter_basic |
| R15, R16 | 30_ingest_timestamps, 32_ingest_place, 32b_ingest_place_truncate |
| R17-R19 | 11_volume_mute_mixers, 17_volume_actions, 38_push_to_talk_mute, 45_fader_ignore_quirk |
| R20-R22 | 17_volume_actions, 38_push_to_talk_mute, 55_volmeter_muted_flag |
| R23-R28 | 11_volume_mute_mixers, 15_custom_render, 16_submix, 12_start_offset |
| R29 | 17_volume_actions |
| R30-R33 | 10_basic_mix, 13_two_sources, 14_render_order, 19_double_root |
| R34-R36, R38 | 20_buffering_dynamic, 21_buffering_fixed, 22_buffering_maxed |
| R37 | 37_monitoring |
| R39 | 18_ignore_audio_no_ts, 22_buffering_maxed |
| R40 | 10_basic_mix, 12_start_offset, 13_two_sources |
| R41-R43 | 23_discard_stopped, 10_basic_mix |
| R44 | 20_buffering_dynamic, 24_tasks |
| R45 | 40_fader_curve_cubic, 41_fader_curve_iec, 42_fader_curve_log |
| R46 | 43_fader_state |
| R47, R48 | 44_fader_source, 45_fader_ignore_quirk |
| R49 | 40-42 (`fader_conv ... db`) |
| R50 | 50_volmeter_basic, 53_volmeter_channels, 54_volmeter_callbacks |
| R51 | 50_volmeter_basic, 55_volmeter_muted_flag |
| R52, R54 | 51_volmeter_peak_types, 52_volmeter_framecount, 56_volmeter_unaligned_quirk |
| R53 | 50_volmeter_basic, 52_volmeter_framecount |
| R55 | 51_volmeter_peak_types, 54_volmeter_callbacks |

## 11. Open questions

1. The Rust sufficiency check (implementing from this spec and running the vectors) was **not performed** in this pass (usage limit); the spec has not been validated by an independent implementation.
2. `scene` / `group` audio rendering (`obs-scene.c`) decides what a composite source reports and mixes; here composite sources use a harness callback. A scene spec must define it.
3. Real resampler (`audio_resampler_resample`) timestamp offset semantics beyond "value is subtracted from the stored timestamp" (R12) are not specified.
4. Behaviour of `audio_callback` with `C` different from the channels the sources were ingested with after a mid-run `obs_reset_audio` is not covered.
5. Windows/macOS: `util_mul_div64` uses `_udiv128` on MSVC (exact result) so timestamps can differ by 1 ns in extreme ranges; the vectors stay inside the range where both agree.
