# S-044 Audio and video output threads (`avio`)

| | |
|---|---|
| Status | Ready for review (Rust sufficiency check **not performed**, see section 11) |
| Spec version | working tree on `docs/rust-rewrite-plan` (uncommitted) |
| Reference | C implementation, libobs 33.0.1: `media-io/video-io.c`, `media-io/audio-io.c` (+ the declarations in `video-io.h`, `audio-io.h` that plugins compile in) |
| Target crate | `crates/media-io` (module `video_output`, `audio_output`) |
| Depends on | S-013 (frame allocation `video_frame_init/free`, format/channel helpers, enum numbers), S-024 (small helpers: mul-div) |
| Related, not duplicated | S-032 (the audio *input* callback that fills the mixes), S-042 (core: owns one `audio_t`, one `video_t` per mix and calls `lock/unlock_frame`), S-041 |
| Conformance | `test/conformance/scripts/avio/` (25 scripts), runner `gen/avio_runner.c` |

## 1. Scope

`video_output` and `audio_output` are the two real-time pump objects underneath libobs. Each owns one worker thread.

* **Video output** is *pull-driven by its producer*: the core's render thread calls `video_output_lock_frame` to obtain a
  free slot of a small ring of frames, fills it, then `video_output_unlock_frame`. The worker thread wakes once per
  unlocked frame, calls every registered input (encoder, raw output, ...) with the frame - converting it first if the
  input asked for another format/size/range/colour space - and returns the slot to the ring. It counts total and
  skipped frames. A "frame-rate divisor" lets an input receive every n-th frame only; a lightweight *child* handle
  exposes the same output at a divided frame rate.
* **Audio output** is *clock-driven*: its thread wakes every 1024 samples, asks the core (callback) to mix the interval
  into up to 6 mixes x 8 planes of floats, clamps those mixes, and hands each mix to its registered consumers
  (encoders, raw audio outputs), optionally through a resampler.

In scope: parameter validation, object geometry, input registration/removal and its side effects, the ring, both
thread loops, tick timing, clamping, timestamp rules, counters, logging, concurrency contract. Out of scope: the
numerical work of the scaler (`video_scaler_*`, swscale) and the resampler (`audio_resampler_*`, swresample) - both are
*injected dependencies* here and are replaced by logging fakes in the vectors; the producers of the data (S-032,
S-042); frame allocation and format math (S-013).

## 2. Terminology and conventions

RFC 2119 words. `W` = 8. All times are nanoseconds (`u64`). "Root" = a video handle with no parent. "Slot" = one
element of the video ring. "Pass" = one delivery round of one frame to all inputs. "Tick" = one iteration of the audio
loop. Floats are IEEE binary32. Vector floats are printed `%.9g`, NaN as `nan`, infinities as `inf`/`-inf`, `-0` as `0`;
comparison is exact (every float in the vectors is the result of a copy or a clamp). Doubles are `%.17g` (exact).
Library log lines appear in vectors as `log LEVEL text`; the text of the three log messages in section 5 is normative.

## 3. Interface

### 3.1 Constants and enums used (numbers are those of S-013)

`MAX_AUDIO_MIXES = 6`, `MAX_AUDIO_CHANNELS = 8`, `AUDIO_OUTPUT_FRAMES = 1024`, `MAX_AV_PLANES = 8`.
Results: `VIDEO_OUTPUT_SUCCESS = 0`, `_INVALIDPARAM = -1`, `_FAIL = -2`; `AUDIO_OUTPUT_SUCCESS = 0`, `_INVALIDPARAM = -1`,
`_FAIL = -2`. Scaler results: `VIDEO_SCALER_SUCCESS = 0`, `_BAD_CONVERSION = -1`, `_FAILED = -2`. Scale type used:
`VIDEO_SCALE_FAST_BILINEAR = 2`. Range: `DEFAULT 0, PARTIAL 1, FULL 2`. Colour space: `DEFAULT 0, 601 1, 709 2, SRGB 3,
2100_PQ 4, 2100_HLG 5`. Internal constants: video `MAX_CONVERT_BUFFERS = 3`, `MAX_CACHE_SIZE = 16`.

### 3.2 Structs (W = 8; offsets in bytes)

| struct | layout |
|---|---|
| `video_output_info` (48) | `name` 0 (ptr, **not copied**), `format` 8, `fps_num` 12, `fps_den` 16, `width` 20, `height` 24, `cache_size` 32 (size_t), `colorspace` 40, `range` 44 |
| `video_scale_info` (20) | `format` 0, `width` 4, `height` 8, `range` 12, `colorspace` 16 |
| `video_data` (104) | `data[8]` 0 (ptrs), `linesize[8]` 64 (u32), `timestamp` 96 (u64) |
| `video_frame` (96) | `data[8]` 0, `linesize[8]` 64 - exactly the first 96 bytes of `video_data` (the ring stores `video_data`-compatible slots) |
| `audio_output_info` (40) | `name` 0 (ptr, **not copied**), `samples_per_sec` 8, `format` 12, `speakers` 16, `input_callback` 24, `input_param` 32 |
| `audio_convert_info` (16) | `samples_per_sec` 0, `format` 4, `speakers` 8, `allow_clipping` 12 (bool) |
| `audio_data` (80) | `data[8]` 0 (ptrs), `frames` 64 (u32), `timestamp` 72 (u64) |
| `audio_output_data` (64) | `float *data[8]` |

Callback types:
`video_cb(void *param, video_data *frame)`;
`audio_output_callback_t(void *param, size_t mix_idx, audio_data *data)`;
`audio_input_callback_t(void *param, u64 start_ts, u64 end_ts, u64 *new_ts, u32 active_mixers, audio_output_data *mixes) -> bool`.
The opaque handles are `video_t` (`struct video_output`) and `audio_t` (`struct audio_output`).

### 3.3 Functions

Video: `int video_output_open(video_t **out, video_output_info *info)`; `void video_output_close(video_t*)`;
`bool video_output_connect(v, const video_scale_info *conv, cb, param)`;
`bool video_output_connect2(v, conv, u32 frame_rate_divisor, cb, param)`; `void video_output_disconnect(v, cb, param)`;
`bool video_output_disconnect2(v, cb, param)`; `bool video_output_active(const v)`;
`const video_output_info *video_output_get_info(const v)`; `bool video_output_lock_frame(v, video_frame *frame, int count, u64 ts)`;
`void video_output_unlock_frame(v)`; `u64 video_output_get_frame_time(const v)`; `void video_output_stop(v)`;
`bool video_output_stopped(v)`; `video_output_get_format|width|height|frame_rate(const v)` (`double` for the rate);
`u32 video_output_get_skipped_frames|total_frames(const v)`; `video_output_inc_texture_encoders|dec_texture_encoders|inc_texture_frames|
inc_texture_skipped_frames(v)`; `video_t *video_output_create_with_frame_rate_divisor(v, u32 divisor)`;
`void video_output_free_frame_rate_divisor(v)`.

Audio: `int audio_output_open(audio_t **out, audio_output_info *info)`; `void audio_output_close(audio_t*)`;
`bool audio_output_connect(a, size_t mix, const audio_convert_info *conv, audio_output_callback_t cb, void *param)`;
`void audio_output_disconnect(a, size_t mix, cb, param)`; `const audio_output_info *audio_output_get_info(const a)`;
`bool audio_output_active(const a)`; `size_t audio_output_get_block_size|planes|channels(const a)`;
`u32 audio_output_get_sample_rate(const a)`.

All functions are thread-safe with respect to the worker thread as described in section 7.

## 4. Data formats (state)

### 4.1 Video output state

```
info                (copy of the open() argument; cache_size clamped; name pointer kept)
frame_time          u64     floor(1e9 * fps_den / fps_num)
ring                N = info.cache_size slots, each { frame(video_data-shaped, planes allocated for info.format/width/height),
                    count: i32, skipped: i32 }
first, last         ring indices (start 0, 0)         avail   number of free slots (start N)
inputs              ordered list; each { conversion: video_scale_info, scaler?, buf[3]: video_frame, cur: 0..2,
                    divisor: u32, counter: u32, callback, param }
total, skipped      C `long` counters (atomic)        gpu_refs  C `long`      raw_active  bool      stop  bool
parent              none for roots
```

### 4.2 Audio output state

```
info, channels = get_audio_channels(speakers), planar = is_audio_planar(format)
planes     = planar ? channels : 1
block_size = (planar ? 1 : channels) * bytes_per_channel(format)
mix[6]:    inputs (ordered list of { conversion, resampler?, callback, param }),
           buffer[8][1024] f32 (the "clamped" buffer), buffer_unclamped[8][1024] f32
```

### 4.3 Worked example (vector `audio_mix`, 48 kHz FLOAT_PLANAR STEREO, clock 1 000 000 000)

Tick 1 is due at `1e9 + frames_to_ns(48000, 1024) = 1021333333`; its `start_ts` is `1000000000`. With inputs on mix 0
and mix 3 the mask is `0x09`. The input callback writes `-3` to mix-3 plane 0 samples 0..1, `nan,inf,-inf` to plane 1
samples 0..2 and sets `*new_ts = end_ts`. The consumers see mix 3 plane 0 `[-1, -1, 0, 0]` and plane 1 `[0, 1, -1, 0]`
(clamped) and `ts = 1042666666` for tick 2.

## 5. Behaviour (numbered rules)

### 5.1 Video: open, geometry, getters

* **S-044-R01** `video_output_open` returns `-1` (and creates nothing, `*out` untouched) if any of `height`, `width`,
  `fps_den`, `fps_num` is 0. Nothing else is validated (format NONE and `cache_size` 0 are accepted).
* **S-044-R02** `frame_time = floor(1_000_000_000 * fps_den / fps_num)` (exact; `30000/1001 -> 33366666`, `60 -> 16666666`, `7/3 -> 428571428`).
* **S-044-R03** `cache_size` greater than 16 is clamped to 16 *in the stored info* (`get_info` shows 16). Each of the N
  slots owns a frame allocated for `(format, width, height)` as by `video_frame_init` (S-013). `avail = N`, `first = last = 0`.
* **S-044-R04** A new output has no inputs, `raw_active = false`, `gpu_refs = 0`, `stop = false`, `total = skipped = 0`; the worker thread exists and waits.
* **S-044-R05** Getters on a **root**: `get_info` returns the stored info; `get_format/width/height` its fields;
  `get_frame_rate = (double)fps_num / (double)fps_den`; `get_frame_time` the stored value. With a NULL handle:
  `get_info -> NULL`, `active -> false`, `stopped -> true`, `get_format -> NONE`, `width/height -> 0`, `frame_rate -> 0.0`,
  `frame_time -> 0`, `lock_frame -> false`, `disconnect/disconnect2 -> false`, `unlock_frame/stop/close/free_frame_rate_divisor` no-ops,
  `create_with_frame_rate_divisor -> NULL`.
* **S-044-R06** `video_output_get_skipped_frames/total_frames` return the counters converted to `u32` (truncation of the C `long`).

### 5.2 Video: the ring (`lock_frame` / `unlock_frame`)

* **S-044-R10** `lock_frame(v, &frame, count, ts)` on the root: if `avail == 0` -> `slot[last].count += count`,
  `slot[last].skipped += count`, return `false` (`frame` untouched). Otherwise: if `avail != N` then `last = (last+1) % N`;
  `slot[last].frame.timestamp = ts`, `.count = count`, `.skipped = 0`; copy the slot's plane pointers and linesizes
  into `*frame`; return `true`. (`count >= 1` is a precondition.)
* **S-044-R11** `unlock_frame`: `avail -= 1` and wake the worker once. It MUST only be called after a successful lock.
* **S-044-R12** Slot selection example (N = 3, worker idle after each unlock): frames go to slots 0,1,2,0,... and after each
  completed frame `first = last` again (see R22).

### 5.3 Video: the worker loop

* **S-044-R20** Worker: `loop { wait for a wake token; if stop -> exit; while !stop && !pass(): total += 1; total += 1 }`.
  Each wake therefore processes exactly one *frame* (all of its `count` passes), adding one to `total` per pass
  (including the final one). The final `total += 1` is executed even if the inner loop was left because of `stop`.
* **S-044-R21** `pass()` delivers `slot[first]` to the inputs: for each input *in registration order* (the list length and index are re-read after every callback, see Q-1):
  1. `skip = input.counter; input.counter += 1; if input.counter == input.divisor { input.counter = 0 }`; if `skip != 0` continue (no scaling, no callback). The counter advances on **every** pass, for every input, whether or not a callback happens.
  2. `frame` = shallow copy of the slot's `video_data` (planes, linesizes, timestamp).
  3. If the input has a scaler: `cur = (cur+1) % 3`; scale `frame` into `buf[cur]`; on success replace `frame`'s planes/linesizes with `buf[cur]`'s (timestamp unchanged); on failure log WARNING `video-io: Could not scale frame!` and **skip the callback for this input** (the buffer index has advanced anyway).
  4. `callback(param, &frame)`.
* **S-044-R22** After the inputs: `slot.timestamp += frame_time`; `slot.count -= 1`; `complete = (slot.count == 0)`; `was_skipped = slot.skipped > 0`.
  If complete: `first = (first+1) % N; avail += 1; if avail == N { last = first }`. Else if `was_skipped`: `slot.skipped -= 1; skipped_total += 1` (atomic). `pass()` returns `complete`.
  Consequences: a frame with `count = 3` yields three callbacks per input with timestamps `ts, ts+ft, ts+2ft`; and `skipped_total` grows by 1 for each of the first `slot.skipped` non-final passes.
* **S-044-R23** Lag: while the worker is busy, a producer that keeps locking exhausts the ring (`avail == 0`); each failed lock folds its `count` into the newest slot as extra repeats and as `skipped` (R10). Vector `video_lag`: N = 2, frame 0 blocked in a callback, frame 1 queued, two further locks (count 1 each) fail -> slot 1 has `count = 3, skipped = 2`; after release the frame is delivered 3 times (`ts, ts+ft, ts+2ft`), `skipped_total = 2`, `total = 4`.
* **S-044-R24** With zero inputs the loop still runs and counts: every wake adds `count` to `total`.
* **S-044-R25** The `skipped`/`total` counters are only reset by R41/R42; the worker never resets them.

### 5.4 Video: inputs

* **S-044-R30** Input identity is `(callback, param)` (pointer equality). `connect` is `connect2` with divisor 1.
* **S-044-R31** `connect2` returns `false` (no state change) if `video` is NULL [D-1], `callback` is NULL, or `divisor == 0`.
  Operations act on the **root** of the handle.
* **S-044-R32** If the pair is already registered: nothing happens, result `false`.
* **S-044-R33** Conversion: `conv == NULL` -> `{format, width, height, range, colorspace}` of the output. Otherwise a copy of `*conv`, with `width == 0` -> output width and `height == 0` -> output height (**format, range and colour space are never defaulted**; format 0 is simply a different format).
* **S-044-R34** A scaler is needed iff any of: `conv.width != out.width`, `conv.height != out.height`, `conv.format != out.format`, `!match_range`, `!match_space`. `match_range(a,b) = ((a == FULL) == (b == FULL))` (DEFAULT and PARTIAL are the same). `match_space(a,b) = a == DEFAULT || b == DEFAULT || collapse(a) == collapse(b)` with `collapse(SRGB) = 709`, `collapse(2100_HLG) = 2100_PQ`, others identity. Vector `video_connect`.
* **S-044-R35** When a scaler is needed it is created with `(dst = conv, src = {out.format, out.width, out.height, out.range, out.colorspace}, FAST_BILINEAR)`; three conversion buffers are allocated for `(conv.format, conv.width, conv.height)` (S-013 `video_frame_init`). Result `BAD_CONVERSION` -> ERROR log `video_input_init: Bad scale conversion type`; any other non-success -> ERROR `video_input_init: Failed to create scaler`; either way connect returns `false` and nothing changes (R36 not executed).
* **S-044-R36** On success the input is appended (order preserved). If it is the **first** input: if `gpu_refs == 0` reset `total = skipped = 0`; set `raw_active = true`. Result `true`.
* **S-044-R37** `disconnect2` returns `false` for NULL video or NULL callback, or if the pair is unknown. Otherwise the input (scaler, buffers) is destroyed, later inputs shift down (order preserved) and it returns `true`. If the list became empty: `raw_active = false`, and if `gpu_refs == 0` run *log_skipped* (R43). `disconnect` is `disconnect2` with the result dropped.
* **S-044-R38** `video_output_active` = `raw_active` of the root (not affected by texture encoders, not cleared by `stop`).
* **S-044-R39** Callbacks are invoked on the worker thread while the input lock is held (S-044-R70).

### 5.5 Video: counters and texture encoders

* **S-044-R40** `inc_texture_frames` -> `total += 1`; `inc_texture_skipped_frames` -> `skipped += 1` (root).
* **S-044-R41** `inc_texture_encoders`: `gpu_refs += 1`; if the new value is 1 and `!raw_active` -> reset `total = skipped = 0`.
* **S-044-R42** `dec_texture_encoders`: `gpu_refs -= 1`; if the new value is 0 and `!raw_active` -> *log_skipped*.
* **S-044-R43** *log_skipped*: if `skipped != 0` log INFO `Video stopped, number of skipped frames due to encoding lag: %ld/%ld (%0.1f%%)` with `skipped`, `total` and `skipped/total*100` computed in `double` (total 0 prints `inf`; values may exceed 100). Nothing is logged for `skipped == 0`.
* **S-044-R44** The counters are not reset by `stop`, by disconnecting, or by `dec_texture_encoders`.

### 5.6 Video: frame-rate-divisor children

* **S-044-R50** `create_with_frame_rate_divisor(v, d)` returns NULL if `v` is NULL, `d == 0` or `d == 1`. Otherwise it allocates a new handle that is a shallow copy of `*v` (including `info`, `frame_time`) with `parent = v` and `info.fps_den *= d`.
* **S-044-R51** Every function listed in R10-R44 except `get_info`, `get_frame_time` and the two handle-lifetime functions first maps the handle to its root (`while parent: parent`), so the child shares inputs, ring, counters, `stop`, active state with the root. `get_info(child)` returns the child's copy (`fps_den` multiplied); `get_frame_time(child)` returns the copied (unscaled) value; `get_frame_rate/format/width/height(child)` use the root.
* **S-044-R52** `free_frame_rate_divisor(h)`: frees `h` iff it has a parent; NULL or a root: no-op. `video_output_close` MUST be called on roots only.

### 5.7 Video: stop and close

* **S-044-R60** `video_output_stop` (root): if `stop` is false: set it, wake the worker, join it. Later calls do nothing. `stopped(NULL) = true`; otherwise the root's `stop`.
* **S-044-R61** After stop the worker never calls an input again; frames locked/unlocked afterwards stay in the ring unprocessed (`avail` shrinks; nothing consumes them). `video_output_active` is unchanged by stop.
* **S-044-R62** `close(NULL)` is a no-op; otherwise: stop (R60), destroy all inputs (scalers/buffers), free all slots' frames, release the synchronisation objects, free the handle.

### 5.8 Audio: open and geometry

* **S-044-R70** `audio_output_open` returns `-1` if `format == UNKNOWN(0)`, `name == NULL`, `samples_per_sec == 0` or `speakers == UNKNOWN(0)`. `speakers` is otherwise *not* checked (see Q-8, D-5). On success it returns 0 and the thread is running.
* **S-044-R71** Derived values: `channels = get_audio_channels(speakers)`, `planes = planar ? channels : 1`, `block_size = (planar ? 1 : channels) * bytes_per_channel(format)`. Examples: FLOAT_PLANAR stereo -> 4/2/2 (block/planes/channels); FLOAT 5.1 -> 24/1/6; 16BIT_PLANAR 7.1 -> 2/8/8; U8BIT mono -> 1/1/1; 32BIT 2.1 -> 12/1/3.
* **S-044-R72** `get_info` returns the stored info (`name` is the caller's pointer, `input_callback` unchanged); NULL -> NULL. `audio_output_active(NULL) = false`. `get_sample_rate` returns `samples_per_sec`.
* **S-044-R73** `audio_output_active` is true iff at least one mix has at least one input.

### 5.9 Audio: input registration

* **S-044-R74** `connect(a, mix, conv, cb, param)` returns `false` for NULL `a` or `mix >= 6`. Identity is `(mix, cb, param)`; a duplicate returns `false` (no change). The same `(cb,param)` on another mix is a separate input.
* **S-044-R75** Conversion: `conv == NULL` -> `{format, speakers, rate}` of the output and `allow_clipping = false`; otherwise a copy. Then each of `format == UNKNOWN`, `speakers == UNKNOWN`, `samples_per_sec == 0` is replaced by the output's value. `allow_clipping` is kept as given.
* **S-044-R76** A resampler is created iff `conv.format != out.format || conv.rate != out.rate || conv.speakers != out.speakers`, with `(dst = conv, src = out)`; failure -> ERROR `audio_input_init: Failed to create resampler` and `connect` returns `false` (nothing registered). Success appends the input (order preserved) and returns `true`.
* **S-044-R77** `disconnect(a, mix, cb, param)`: NULL `a` or `mix >= 6` or unknown pair: no-op. Otherwise destroys the resampler and removes the input preserving the order of the rest.

### 5.10 Audio: the tick

* **S-044-R80** Loop: `rate = samples_per_sec; samples = 0; start = now(); prev = start; while !stop_requested { samples += 1024; due = start + frames_to_ns(rate, samples); sleep_until(due); tick(due, prev); prev = due }`, with `frames_to_ns(r, n) = (n / r) * 1e9 + ((n % r) * 1e9) / r` (integer division, so the sum truncates: 44100 Hz tick 1 = 23219954, 8000 Hz tick 1 = 128000000). The due time is derived from the absolute sample count (no accumulated rounding). `start_ts` of tick 1 is `start`; of tick k > 1 the due time of tick k-1. `start` is read **once**, when the thread starts. If a tick is late `sleep_until` returns at once and later ticks run back to back (no skipping).
* **S-044-R81** `tick(end, start_ts)`:
  1. `mask` = bit `i` for every mix with >= 1 input (read under the input lock).
  2. Zero **all 6 mixes' clamped buffers** (every plane, 1024 samples). Build `mixes[6]`: for every mix (active or not) `mixes[m].data[p] = &buffer[m][p]` for `p < planes`, the remaining entries NULL.
  3. `new_ts = 0; ok = input_cb(input_param, start_ts, end, &new_ts, mask, mixes)`. If `!ok` the tick ends here (no clamp, no outputs).
  4. Clamp (R82). 5. For `m = 0..5` deliver (R83) with timestamp `new_ts` and 1024 frames.
* **S-044-R82** Clamp, for every mix with >= 1 input (inactive mixes are untouched): `bytes = 1024 * block_size`; for every plane `p < planes`: first copy `bytes` bytes of the plane to `unclamped[p]`, then for the first `bytes/4` samples of the plane: `NaN -> 0`, `> 1 -> 1`, `< -1 -> -1`. The unclamped copy keeps NaN/inf. For FLOAT_PLANAR the region is the whole 1024-sample plane; for other formats see Q-3.
* **S-044-R83** Deliver mix `m`: under the input lock, for the inputs from **the last to the first** (index `n-1` down to 0, the index is re-read after each callback, see Q-2): `data.data[p] = &(allow_clipping ? unclamped : clamped)[p]` for `p < planes`, `data.frames = 1024`, `data.timestamp = new_ts`. If the input has a resampler: call it with those planes and `frames`; on failure skip this input's callback; on success `data.data[0..8] = resampler outputs (unused entries NULL)`, `data.frames = resampler frames`, `data.timestamp = new_ts - ts_offset` (u64 wrapping). Then `cb(param, m, &data)`.
* **S-044-R84** `new_ts` is whatever the input callback stored (initially 0, so a callback that does not set it makes consumers see timestamp 0). Delivery order across mixes: 0, 1, ..., 5.
* **S-044-R85** Every tick calls the input callback even when no mix is active (mask 0).
* **S-044-R86** Entries of `data.data[]` beyond `planes` are unspecified in the reference (uninitialised stack); a conforming implementation MUST NOT read them and SHOULD supply NULL.

### 5.11 Audio: close

* **S-044-R90** `audio_output_close(NULL)` is a no-op. Otherwise: signal stop, join the thread (the loop notices the stop only at the top of the loop, so a thread inside `sleep_until` completes that sleep **and one more tick**, Q-6), destroy every remaining input's resampler, free everything.

## 6. Algorithms (summary pseudocode)

```
video_lock(v, frame, count, ts):                       video_pass(v):
  r = root(v); lock(data)                                 slot = ring[first]                      # data lock released
  if avail == 0: ring[last].count += count;              lock(input)
                 ring[last].skipped += count;            for i in 0.. while i < inputs.len:      # re-evaluated
                 return false                              in = inputs[i]; skip = in.counter++; if in.counter==in.divisor: in.counter=0
  if avail != N: last = (last+1) % N                       if skip: continue
  ring[last].{ts,count,skipped} = ts,count,0               f = slot.frame; if in.scaler: in.cur=(in.cur+1)%3
  *frame = ring[last].planes; return true                     if scale(in, f): f.planes = in.buf[in.cur]  else warn, continue
                                                           in.callback(in.param, f)
                                                         unlock(input); lock(data)
                                                         slot.ts += frame_time; complete = --slot.count == 0
                                                         if complete: first=(first+1)%N; avail++; if avail==N: last=first
                                                         elif slot.skipped>0: slot.skipped--; skipped_total++
                                                         return complete
```

Audio tick timing: see R80; clamp: see R82.

## 7. Concurrency

* Video: two recursive mutexes. `data` guards the ring indices/slots (held only in `lock_frame`, `unlock_frame` and the two short sections of `pass()`; **never while a callback runs**, so the producer is never blocked by a slow consumer). `input` guards the input list and is **held while callbacks run**, so `connect/disconnect` from another thread blocks until the pass in progress finishes (the lock is taken per pass, so it blocks for at most one pass). Because it is recursive, a callback may call `connect/disconnect` itself (Q-1). `total`, `skipped`, `gpu_refs`, `raw_active` are atomics; `stop` is a plain flag written by `stop()` and read by the worker (benign race).
* Audio: one recursive mutex `input` guards all 6 input lists. It is held while computing the mask and while delivering one mix; it is **not** held while the input callback runs, so the input callback may connect/disconnect (S-042 does). Output callbacks run with it held and on the audio thread; they may disconnect (Q-2).
* The wake token of the video worker is a counting semaphore: *n* unlocks queue *n* wake-ups.
* Neither worker may be started or stopped from within its own callbacks.

## 8. Reference defects and quirks

* **Q-1 (QUIRK, video) removal inside a callback skips an input.** The pass iterates by ascending index and re-reads the list length. If a callback removes an input at or before the current index, the next input moves into the current index and is skipped for this pass (but its divisor counter is not advanced either). Vector `video_reentrancy`: inputs [1,2,3], input 1 disconnects itself -> 3 is called, 2 is not (and a frame later 2 is delivered normally). SHOULD match.
* **Q-2 (QUIRK, audio) removal inside an output callback.** Delivery runs from the last input to the first. Self-removal is safe (every remaining input is called once). Removing an input with a *lower* index than the running one shifts the list, and the running input is called again. Vector `audio_reentrancy`: inputs [1,2,3] on one mix, 3 disconnects 1 -> order 3,3,2. SHOULD match.
* **Q-3 (QUIRK, audio) non-float formats clamp a different region.** The clamp/copy size is `1024 * block_size` bytes read as floats: FLOAT_PLANAR 1024 samples/plane (correct), 16BIT_PLANAR 512 samples, U8BIT_PLANAR 256, FLOAT stereo (interleaved, one plane) 2048 floats. Samples outside the region reach consumers unclamped and un-copied. Real use is FLOAT_PLANAR only. SHOULD match (vector `audio_formats`).
* **Q-4 (QUIRK, video) child output metadata.** A child's `info.fps_den` is multiplied but `frame_time` is not and `get_frame_rate` reports the root's rate. Vector `video_child` (`fps=60/2`, `rate=60`, `frame_time=16666666`).
* **Q-5 (QUIRK, audio) `new_ts` defaults to 0** if the input callback returns true without setting it (vector `audio_newts`).
* **Q-6 (QUIRK, audio) one extra tick on close.** The stop flag is polled only after a tick, so close lets the thread finish its sleep and run one more tick (the input callback is invoked once more, outputs delivered). Not in the vectors (the runner suppresses the callback during close); SHOULD be harmless to reproduce.
* **Q-7 (QUIRK, video) stats text.** The summary percentage is computed in `double` from `long` counters and may print `inf` or exceed 100 (vector `video_log_edge`: `2/1 (200.0%)`, `1/0 (inf%)`). Connecting the first input resets the counters even after a stop.
* **Q-8 (QUIRK, audio) invalid layouts pass validation.** `speakers = 7` (any positive value that is not a layout) is accepted; `channels = 0`; for a planar format `planes = 0` and the mix pointer tables are all NULL; output `data[]` pointers are uninitialised (R86). Vector `audio_open` shows the geometry (`planes=0 channels=0`). The behaviour of a running loop with `planes = 0` is **unspecified** (the reference hands garbage pointers to consumers).
* **Q-9 (QUIRK) `info.name` is borrowed.** Both `open` functions keep the caller's `name` pointer (used for profiler names and returned by `get_info`); it must outlive the object.
* **Q-10 (QUIRK, video) conversion with `format = NONE`** is not defaulted: it differs from the output format, a scaler is requested, and the scaler layer rejects it (`Bad scale conversion type`). Specified through R33/R35; the rejection itself belongs to the scaler.
* **D-1 (DEFECT) `video_output_connect/connect2(NULL, ...)`** dereferences NULL (the root lookup precedes the NULL check). Required: return `false`. Vector `video_defects` (crash).
* **D-2 (DEFECT) `video_output_get_skipped_frames/total_frames(NULL)`** crash. Required: return 0.
* **D-3 (DEFECT) `video_output_inc/dec_texture_encoders(NULL)`, `inc_texture_frames(NULL)`, `inc_texture_skipped_frames(NULL)`** crash. Required: no-op.
* **D-4 (DEFECT) `audio_output_get_block_size/planes/channels/sample_rate(NULL)`** crash. Required: return 0. Vector `audio_defects`.
* **D-5 (DEFECT) `audio_output_open` with a non-layout `speakers` value** (Q-8) leads to uninitialised pointers handed to callbacks. Required: return `AUDIO_OUTPUT_INVALIDPARAM` for `get_audio_channels(speakers) == 0` (no caller in libobs depends on acceptance). Not in the vectors.

## 9. Implementation notes (non-normative)

* **Testability by construction.** Do not call a clock or sleep directly: give the audio loop a `Clock { now_ns(), sleep_until(ns) }` and make the video worker report "iteration finished". The conformance runner needs exactly these seams: for audio, `sleep_until` blocks until the driver releases a tick and the driver waits for the iteration to finish; for video, `unlock_frame` followed by "wait until the worker finished that wake". Alternatively expose `tick(due, prev)` and `pass()` as plain functions and let the test driver call them on one thread - all vectors are consistent with that (gating = blocking inside a callback needs a second thread).
* A Rust port can keep the two recursive locks with `parking_lot::ReentrantMutex<RefCell<_>>`, but then callbacks that disconnect while the list is borrowed must not hold a `RefCell` borrow across the callback (copy the function pointer and parameter, release the borrow, call, re-borrow and re-check the length) - this also reproduces Q-1/Q-2 naturally.
* `callback + param` identity is pointer identity: model `param` as `*mut c_void`/`usize`, the callback as `extern "C" fn` pointer compared by address.
* The scaler and the resampler are traits injected at construction; the vectors use logging fakes whose arguments are part of the expectations (creation arguments, destroy calls).
* Frame buffers: slots hold plane pointers that producers write into through the pointers returned by `lock_frame`; keep them stable for the object's lifetime.
* `total`/`skipped` are C `long` (64-bit on Linux); the getters truncate to `u32`.
* Windows (not tested): the audio thread registers with MMCSS ("Audio"); thread names are set via `os_set_thread_name`.

## 10. Conformance coverage

Runner language: README section "avio runner". Scripts (all under `scripts/avio/`, vectors under `vectors/avio/`):

| Rule | Scripts |
|---|---|
| R01-R05 | video_open |
| R06 | video_counters, video_child |
| R10-R12 | video_frames, video_cache0, video_lag |
| R20, R22 | video_frames, video_duplicates |
| R21 | video_divisor, video_scale, video_connect (indirect) |
| R23 | video_lag, video_cache0, video_disconnect |
| R24 | video_frames |
| R25, R40-R44 | video_counters, video_log_edge, video_disconnect |
| R30-R33 | video_connect |
| R34-R35 | video_connect, video_connect_failures, video_scale |
| R36-R38 | video_connect, video_disconnect, video_connect_failures |
| R39 | video_reentrancy |
| R50-R52 | video_child |
| R60-R62 | video_stop, video_child |
| R70-R73 | audio_open, audio_connect |
| R74-R77 | audio_connect, audio_resample |
| R80 | audio_timing |
| R81, R84, R85 | audio_mix, audio_newts |
| R82 | audio_mix, audio_clip, audio_formats |
| R83 | audio_clip, audio_resample, audio_reentrancy |
| R86 | spec only (uninitialised in the reference) |
| R90 | all audio scripts (`aclose`), audio_resample (resampler destroy) |
| Q-1 | video_reentrancy |
| Q-2 | audio_reentrancy |
| Q-3 | audio_formats |
| Q-4 | video_child |
| Q-5 | audio_newts |
| Q-7 | video_log_edge |
| Q-8 | audio_open (geometry only) |
| Q-6, Q-9, Q-10 | spec only |
| D-1..D-3 | video_defects |
| D-4 | audio_defects |
| D-5 | spec only |

## 11. Open questions

* **Rust sufficiency check not performed** (cost control, by instruction). Spec gaps that such a check would normally expose are therefore possible; the likeliest places are the runner's lock-step contract (section 9) and the exact set of calls the fake scaler/resampler must support.
* The numerical behaviour of the real scaler and resampler (swscale/swresample configuration, resampler `ts_offset` meaning and output frame counts) is outside this spec; the vectors use fakes. Whoever specifies `video_scaler_*` / `audio_resampler_*` must define their contract.
* Behaviour of a running audio loop with `planes = 0` (Q-8/D-5) is left unspecified on purpose.
* The extra tick on close (Q-6) and the real-time rate of the audio thread are not testable with the lock-step harness.
* Out-of-precondition use is not specified: `count <= 0` in `lock_frame` (the reference never completes such a frame and spins until stop), `unlock_frame` without a successful lock, closing a child handle, starting/stopping from callbacks.
* Wake-up granularity on Windows/macOS (`os_sleepto_ns_fast`) is not specified.
