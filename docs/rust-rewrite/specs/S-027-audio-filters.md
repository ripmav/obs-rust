# S-027 Audio filters (obs-filters: gain, invert polarity, compressor, limiter, expander / upward compressor, noise gate, async delay, 3-band EQ, noise suppress plumbing)

| | |
|---|---|
| Status | Ready for review |
| Spec version | initial |
| Reference | OBS Studio 33.0.1: `plugins/obs-filters/{gain-filter,invert-audio-polarity,compressor-filter,limiter-filter,expander-filter,noise-gate-filter,async-delay-filter,eq-filter,noise-suppress-filter}.c` |
| Target crate | `crates/obs-filters-audio` (one module per filter) |
| Depends on | S-014 (obs_data: defaults vs user values, `clear`, `apply`), S-015 (obs_properties) |
| Conformance | `test/conformance/scripts/audiofilters/` (15 scripts, runner described in `test/conformance/README.md`, section "audiofilters runner") |

## 1. Scope

The *behaviour* of the audio filters of the `obs-filters` plugin: registration data, default settings, property lists,
what `create`/`update` derive from settings and from the host's audio configuration, and the exact sample processing of
`filter_audio` (envelope followers, gain computers, ballistics, EQ filter, hysteresis gate, segment buffering of the noise
suppressor), including the compressor's sidechain plumbing and the async-delay video queue. Everything is specified at the
level of 32-bit float arithmetic.

Out of scope: the SpeexDSP and RNNoise libraries and the audio resampler (they are *inputs*: §3.4 lists the contract the
conformance fakes implement), the other filters of the plugin (colour, crop, scroll, LUT, chroma key, gpu delay, ...),
translation files (`obs_module_text(KEY)` returns `KEY` in the conformance runner so the vectors show text keys), the
frontend UI. Of `async-delay-filter.c` only the *video* path exists in the shipped build: the audio path is compiled out
(`DELAY_AUDIO` is not defined, `filter_audio` is absent, output flags `VIDEO|ASYNC`); it is not specified (informative:
it would cache audio packets in a deque and release them once `end_ts - oldest_ts >= interval`).

Callers: libobs source code calls, for a filter source, in this order: `get_defaults(settings)` on the freshly created live
settings (user values from the saved scene are already present), `create(settings, source)`, later `update(data, settings)`
after the host applied new user values (`obs_data_apply`) to the same live settings object (or after `obs_data_clear` +
apply when settings are reset), `filter_audio` on the audio thread, `video_tick` (compressor), `filter_video` /
`filter_remove` (delay), `get_properties(data)` (data may be the filter instance or NULL, see D-3), `destroy`.

## 2. Terminology and conventions

- `f32` = IEEE binary32, all arithmetic in `f32` with round-to-nearest-even, **no fused multiply-add**, no fast-math. Where a
  step is written `f64(...)` it is computed in double and rounded to `f32` afterwards. `x as f32` of an integer rounds to nearest.
- `fmaxf(a,b)`, `fminf(a,b)`: C99 semantics: if exactly one operand is NaN the other is returned; both NaN gives NaN. (`fmax_num`
  in Rust is `f32::max` / `f32::min`.)
- `mul_to_db(m) = (m == 0) ? -inf : 20.0f * log10f(m)`; `db_to_mul(d) = isfinite(d) ? powf(10.0f, d / 20.0f) : 0.0f`
  (so `nan`, `+inf`, `-inf` give `0`). `mul_to_db` of a negative or NaN `m` is NaN.
- `rate` = `audio_output_get_sample_rate` (u32), `ch` = `audio_output_get_channels` (usize, 1..8), read **at `create`/`update`
  time** (rule R03).
- A *block* is `struct obs_audio_data { data[8]: *mut u8 (planes of f32, NULL = absent), frames: u32, timestamp: u64 }`;
  planar float, one plane per channel, planes at index `>= ch` are ignored by the filters unless stated.
- "Setting X" means the effective value (user value if present, otherwise default; S-014). Typed reads convert: `get_int`,
  `get_double`, `get_string` per S-014 (an int setting read as double and vice versa converts).
- Tolerance (vectors): see the conformance README: tokens compared with `|a-b| <= 1e-5*max(|a|,|b|) + 1e-7`, so `powf/expf/log10f/sinf`
  implementations that differ by a few ulps are fine. Non-finite values are `nan`, `inf`, `-inf`; `-0 == 0`.
- Rule ids `S-027-Rnn`; DEFECT `D-n` (required behaviour stated), QUIRK `Q-n`.

## 3. Interface

### 3.1 Registration data (`info`, script 01)

| key | `id` | version | flags | callbacks present beyond create/destroy |
|---|---|---|---|---|
| gain | `gain_filter` | 0 | `AUDIO` (0x2) | update, filter_audio, get_defaults, get_properties |
| invert | `invert_polarity_filter` | 0 | AUDIO | filter_audio only (no update, no defaults, no properties) |
| compressor | `compressor_filter` | 0 | AUDIO | update, filter_audio, video_tick, get_defaults, get_properties |
| limiter | `limiter_filter` | 0 | AUDIO | update, filter_audio, defaults, properties |
| expander | `expander_filter` | 0 | AUDIO | same as limiter |
| upward | `upward_compressor_filter` | 0 | AUDIO | same as limiter (shares code with expander) |
| gate | `noise_gate_filter` | 0 | AUDIO | same as limiter |
| delay | `async_delay_filter` | 0 | `VIDEO|ASYNC` | update, get_properties, filter_video, filter_remove (no defaults) |
| eq | `basic_eq_filter` | 0 | AUDIO | update, filter_audio, defaults, properties |
| ns (v1) | `noise_suppress_filter` | 0 | `AUDIO | CAP_OBSOLETE` | update, filter_audio, defaults(v1), properties |
| ns2 (v2) | `noise_suppress_filter` | 2 | AUDIO | same, defaults(v2) |

All have `type = FILTER`. `get_name` returns the text key (`Gain`, `InvertPolarity`, `Compressor`, `Limiter`, `Expander`,
`Upward.Compressor`, `NoiseGate`, `AsyncDelayFilter`, `3BandEq`, `NoiseSuppress`). Numeric flag values: `OBS_SOURCE_VIDEO = 0x1`, `AUDIO = 0x2`, `ASYNC = 0x4`, `CAP_OBSOLETE = 0x400`; so the registered `output_flags` are gain..eq `0x2`, delay `0x5`, ns v1 `0x402`, ns v2 `0x2`.

### 3.2 Settings and defaults (scripts 03, 07)

| filter | key (type) default | notes |
|---|---|---|
| gain | `db` (double) 0.0 | |
| limiter | `threshold` (double) -6.0; `release_time` (int, ms) 60 | |
| compressor | `ratio` (double) 10.0; `threshold` (double) -18.0; `attack_time` (int ms) 6; `release_time` (int ms) 60; `output_gain` (double dB) 0.0; `sidechain_source` (string) `"none"` | |
| expander | `presets` (string) `"expander"`; `ratio` 2.0; `threshold` -40.0; `attack_time` 10; `release_time` 50; `output_gain` 0.0; `detector` (string) `"RMS"` | gate preset: `presets` `"gate"`, `ratio` 10.0, `release_time` 125, rest equal (R42) |
| upward | `ratio` 0.5; `threshold` -20.0; `attack_time` 10; `release_time` 50; `output_gain` 0.0; `detector` `"RMS"`; `knee_width` (int dB) 10 | no `presets` default |
| gate | `open_threshold` (double dB) -26.0; `close_threshold` -32.0; `attack_time` (int ms) 25; `hold_time` 200; `release_time` 150 | |
| eq | `low`, `mid`, `high` (double dB) 0.0 | |
| ns | `suppress_level` (int dB) -30; `method` (string): v1 `"speex"`, v2 `"rnnoise"` | with both libraries compiled in (reference Linux build); builds with only RNNoise default v1 to `"rnnoise"`, without RNNoise v2 defaults to `"speex"` |
| delay | `delay_ms` (int) — **no default** (reads 0 when absent) | |

`get_defaults` writes **defaults** (not user values) into the given settings object. Only `expander_defaults` reads the object (its
`presets` effective value decides which preset's defaults are written).

### 3.3 Properties (scripts 03, 10)

Order is insertion order. Identity locale: description = text key. `float slider` = `obs_properties_add_float_slider`,
`int slider` likewise; "suffix" as given (S-015 getters). A suffix call on a property of the wrong type is a silent no-op.

| filter | properties (name: type, min, max, step, suffix) |
|---|---|
| gain | `db`: float slider -30..30 step 0.1 `" dB"`, desc `Gain.GainDB` |
| limiter | `threshold` float slider -60..0 step 0.1 `" dB"` (`Limiter.Threshold`); `release_time` int **slider** 1..1000 step 1 `" ms"` (`Limiter.ReleaseTime`) |
| compressor | `ratio` float slider 1..32 step 0.5 `":1"`; `threshold` -60..0 step 0.1 `" dB"`; `attack_time` int slider 1..500 `" ms"`; `release_time` int slider 1..1000 `" ms"`; `output_gain` float slider -32..32 step 0.1 `" dB"`; `sidechain_source`: list (type LIST=2, format STRING) with item `("None" -> text key `None`, "none")` followed by one item (name, name) per enumerated source (host order) that has the `AUDIO` output flag and is not the filter's parent. Texts `Compressor.Ratio/Threshold/AttackTime/ReleaseTime/OutputGain/SidechainSource`. With data == NULL the parent is NULL. |
| expander | (only when not upward) `presets` list(string) items (`Expander.Presets.Expander`,`expander`), (`Expander.Presets.Gate`,`gate`) with a modified callback that returns true; `ratio` float slider 1..20 step 0.1 `":1"`; `threshold` -60..0 0.1 `" dB"`; `attack_time` int slider 1..100 `" ms"`; `release_time` 1..1000 `" ms"`; `output_gain` -32..32 0.1 `" dB"`; `detector` list(string): (`Expander.RMS`,`RMS`), (`Expander.Peak`,`peak`) |
| upward | as expander without `presets`/`detector`; `ratio` slider **0..1** step 0.1 `":1"`; plus `knee_width` int slider 0..20 step 1, **no suffix** (Q-1) |
| gate | `close_threshold` float slider -96..0 step 1 `" dB"`; `open_threshold` likewise (order: close first); `attack_time`, `hold_time`, `release_time`: int (plain, not slider) 0..10000 step 1 `" ms"` |
| delay | `delay_ms` int (plain) 0..20000 step 1 `" ms"` (`DelayMs`) |
| eq | order `high`, `mid`, `low`: float slider -20..20 step 0.1 `" dB"`, desc `3BandEq.high` etc. |
| ns | with both libraries: `method` list(string) items (`NoiseSuppress.Method.Speex`,`speex`), (`...RNNoise`,`rnnoise`) with modified callback `noise_suppress_method_modified`; then `suppress_level` int slider -60..0 step 1 `" dB"`. Callback: `suppress_level.visible = (settings.method == "speex")`, returns true. (Without Speex the slider is absent, without both libraries the method list is absent.) |
| invert | none (no `get_properties`) |

### 3.4 Host and library contract (inputs of the component, faked by the conformance runner)

The component calls the host only through these functions; the runner supplies them (documented in `gen/audiofilters_stubs.c`):

- `audio_output_get_sample_rate`, `audio_output_get_channels` (script `audio CH RATE`); `obs_get_audio_info` (only `samples_per_sec`).
- `obs_source_get_name(filter)`; `obs_filter_get_parent(filter)`; `obs_source_get_speaker_layout(parent)` (`UNKNOWN` for NULL parent).
- `os_gettime_ns()` (script clock).
- compressor: `obs_get_source_by_name(name)` (+1 strong ref or NULL), `obs_source_get_weak_source`, `obs_weak_source_get_source` (+1 strong or NULL
  when the source is gone), `obs_source_release`, `obs_weak_source_release`, `obs_source_add_audio_capture_callback(src, cb, param)`,
  `obs_source_remove_audio_capture_callback`, `obs_enum_sources`, `obs_source_get_output_flags`.
- delay: `obs_source_release_frame(parent, frame)`.
- noise suppress: SpeexDSP `speex_preprocess_state_init(frame_size, rate)`, `_destroy`, `_ctl(SET_NOISE_SUPPRESS = 18, &int)`,
  `_run(i16 *x)` (in place, `frame_size` samples, fake: `x[i] = x[i] / 2` C integer division); RNNoise `rnnoise_create(NULL)`,
  `_destroy`, `rnnoise_process_frame(state, out, in)` (480 floats, may alias, fake: `out[i] = in[i] * 0.5f`);
  audio resampler `create(dst_info, src_info)` / `destroy` / `resample(rs, out_ptrs, &out_frames, &ts_offset, in_ptrs, in_frames)`
  (float planar; fake: `out_frames = in*dst/src` unless forced, nearest-neighbour `out[k] = in[min(k*src/dst, in-1)]`, output planes
  owned by the resampler and valid until the next call).

Every call is traceable with `events on` (`host ...` lines); the call order in the vectors is normative for the lifecycle rules
(R37-R39, R82-R85, R98).

## 4. Data formats

No file formats. Block layout in §2. Worked example (script 02, gain 6 dB, 2 ch, 48 kHz): `multiple = db_to_mul(6.0) = 1.99526238`;
input `[0.5,0.5,0.5,0.5]` on plane 0 becomes `0.997631192` (4 times). Worked example (script 04, limiter defaults, 48 kHz): `attack_gain = 8.95773122e-10`,
`release_gain = 0.999652863`; first block of 16 frames of `1.0` on plane 0 and `0.5` on plane 1 leaves `envelope = 1` and produces
`0.501187205` / `0.250593603` (gain `db_to_mul(-6 - 0) = 0.501187205` since `mul_to_db(1) = 0`).

## 5. Behaviour (numbered rules)

### 5.1 Common

- **S-027-R01** Each filter instance owns its state; `create` initialises it to zero then calls the filter's update logic with the settings
  (`create` returns non-NULL for all filters; the compressor returns NULL only if mutex creation fails).
- **S-027-R02** `filter_audio` processes the block **in place** and returns the same pointer (`ret same`), except noise suppress (R90-R98),
  which returns its own buffer or NULL. A returned NULL means "drop this block" for the host.
- **S-027-R03** `rate` and `ch` are read from the host at `create` and at every `update` (gain, compressor, limiter, expander, upward,
  gate, noise suppress). **Exceptions:** eq reads them only in `create` (R70); delay reads `rate` only in `create` and does not use it
  (R80). A filter processes `ch` channels (not the number of planes supplied); `ch` is 1..8.
- **S-027-R04** `update` never resets signal state (envelopes, smoothed gain, filter memories, queues) **except** the noise gate (R62)
  and the delay (R81), whose update resets state as described.
- **S-027-R05** Time constants: `coef(rate, t) = f32( exp(f64(-1.0f / (rate as f32 * t))) )` for compressor and limiter (C `exp`, double
  precision, rounded to f32) and `expf(-1.0f / (rate as f32 * t))` for the expander: arguments are formed in f32: `rate as f32 * t` is an f32
  product, `-1.0f/that` an f32 quotient. `t_ms / 1000.0f` is an f32 division. `t = 0` gives `-1/0 = -inf`, `exp(-inf) = 0`.
- **S-027-R06** NaN/Inf: nothing is sanitised; NaN input propagates as the f32 formulas dictate (`fmaxf` ignores a NaN operand,
  `fminf(0, NaN) = 0`). The vectors pin the resulting values (scripts 04-06, 08, 09, 21).

### 5.2 Gain (script 02)

- **S-027-R10** `update`: `ch` from host; `multiple = db_to_mul((val as f32))` with `val = get_double("db")` (so a non-finite dB gives `0`).
- **S-027-R11** `filter_audio`: for `c in 0..ch`, if plane `c` non-NULL: `x[i] *= multiple` for `i in 0..frames` (exact f32 multiply; planes `>= ch` and NULL planes untouched).

### 5.3 Invert polarity (script 02)

- **S-027-R15** `filter_audio`: for `c in 0..8` in order: if plane `c` is NULL stop (later planes are **not** processed); else negate every
  sample (`x *= -1.0f`, so `0` becomes `-0`, NaN stays NaN). It ignores `ch`: all planes up to the first NULL are negated.
- **S-027-R16** `create` stores nothing (the instance pointer is the source itself), `destroy` does nothing; no settings.

### 5.4 Limiter (script 04)

State: `envelope: f32` (0), `slope = 1.0`, buffer `envelope_buf` (length ≥ block).
- **S-027-R20** `update`: `threshold = get_double("threshold") as f32`; `release_ms = get_int("release_time") as f32`;
  `attack_gain = coef(rate, 0.001f / 1000.0f)` (a fixed 1 µs time constant; the 1 ms constant is divided by 1000 **again**, Q-2);
  `release_gain = coef(rate, release_ms / 1000.0f)`; `output_gain = db_to_mul(0) = 1.0`; `slope = 1.0`; `ch`, `rate` stored.
  `envelope` unchanged. (Internal buffer sized `rate*10/1000` initially; unobservable.)
- **S-027-R21** `filter_audio`: if `frames == 0` return unchanged. Envelope pass: `buf[0..n] = 0`. For each `c in 0..ch` with non-NULL plane:
  `env = envelope` (**the same stored scalar for every channel**); for each sample: `x = fabsf(s)`; if `env < x` then
  `env = x + attack_gain*(env - x)` else `env = x + release_gain*(env - x)`; `buf[i] = fmaxf(buf[i], env)`. After all channels:
  `envelope = buf[n-1]` (0 if no channel had a plane).
- **S-027-R22** Gain pass: for `i in 0..n`: `env_db = mul_to_db(buf[i])`; `g = slope*(threshold - env_db)`; `gain = db_to_mul(fminf(0, g))`; for each
  `c in 0..ch` with non-NULL plane: `s[i] *= gain * output_gain` (the product `gain*output_gain` is formed first). Silence (`buf=0`, `env_db=-inf`)
  yields `g=+inf`, `fminf -> 0`, gain 1.

### 5.5 Compressor (scripts 05, 10)

Same envelope pass and gain pass as R21/R22 with these differences.
- **S-027-R30** `update`: `ratio = get_double("ratio") as f32`; `threshold`; `attack_gain = coef(rate, attack_ms/1000.0f)` with
  `attack_ms = get_int("attack_time") as f32`; `release_gain` likewise; `output_gain = db_to_mul(get_double("output_gain") as f32)`;
  `slope = 1.0f - (1.0f / ratio)` (`ratio = 0` gives `slope = -inf`; ratio < 1 gives negative slope, no clamping). `ch`, `rate`.
- **S-027-R31** Without an active sidechain (no weak reference held) the envelope pass reads the block itself (R21). With an active sidechain
  the envelope pass reads the *sidechain buffer* (R35) over `0..n` per channel in place of the block; `envelope` is shared state in both modes; gain is
  applied to the block.
- **S-027-R32** Sidechain selection in `update`: `valid = name != "" && name != "none"`. If not valid: release any held weak reference
  (host call order: `weak_get_source N`; if it resolves: `remove_audio_capture_callback N`, `source_release N`; then always `weak_release N`) and forget the name. If valid and the name differs from the stored one
  (or none stored): release any held weak reference as above, store the name, set `check_time = now - 3_000_000_000` (u64 wrapping). If valid and equal: nothing.
- **S-027-R33** `video_tick`: if a name is stored, no weak reference is held and `now - check_time > 3_000_000_000` (u64 wrapping subtraction, strict `>`):
  `check_time = now`; `src = get_source_by_name(name)`. If found: take a weak reference, store it (provided the stored name is still equal), add the capture
  callback `(param = this instance)` to `src`, then release the strong reference. If not found nothing is stored (the next attempt happens after > 3 s). The
  lookup therefore never happens in the same `tick` as the `update` that set the name unless the clock advanced.
- **S-027-R34** Capture callback `(source, audio_data, muted)`: `max_frames = max(max_frames, audio.frames)`; `expected = max_frames * 4` bytes; if `expected == 0` return.
  If the queue of channel 0 holds more than `2*expected` bytes, drop `expected` bytes from the front of **every** channel queue (`c < ch`). Then append `frames*4` bytes to every
  channel queue `c < ch`: zeros if `muted`, else the samples of plane `c` (plane `c` must be non-NULL, D-1).
- **S-027-R35** Sidechain fetch (when a weak ref is held), `buf_len` = internal buffer length in frames (initially `rate*10/1000`; it only grows: when a block
  has more frames than `buf_len`, `buf_len := n`): `max_frames = max(max_frames, n)`; if queue 0 holds fewer than `buf_len*4` bytes: the sidechain buffers
  (`buf_len` frames per channel) are filled with zeros; otherwise **`buf_len` frames** (not `n`) are popped from every channel queue into the buffers (Q-3: a small block consumes a whole
  buffer length; the envelope pass then uses the first `n` of them). Buffers of grown size hold zeros after growth only if the underflow rule applies; an
  implementation must not expose uninitialised data (after growth with enough queued data the pop fills them).
- **S-027-R36** `destroy`: if a weak reference is held and resolvable, remove the capture callback and release; release the weak reference; free queues.
- **S-027-R37** Properties as §3.3; `get_properties(NULL)` is legal (R.D-3 does not apply to the compressor).
- **S-027-R38** The reference counts must balance: every strong reference obtained by `get_source_by_name` / `weak_get_source` is released (`refs` returns `strong=0` in
  the vectors).
- **S-027-R39** With the weak reference present but the source dead (`weak_get_source` returns NULL), `update` to another name drops the weak reference without calling `remove_audio_capture_callback`.

### 5.6 Expander and upward compressor (scripts 06, 07)

State per channel `c < 8`: `runave[c]` (0), `envelope[c]` (0), `gain_db_buf[c]` (0). Flags: `is_upwcomp` (fixed per filter kind), `is_gate` (initially: `presets == "gate"` at create), `detector` (RMS = 0, PEAK = 2; initially RMS).
- **S-027-R40** `update`: first, if not upward: `p = get_string("presets")` (copy it; D-2). If `p == "expander"` and `is_gate`: `clear` all user values of the settings, set user `presets = "expander"`, call the expander default function (R42) on the settings, `is_gate = false`. Else if `p == "gate"` and `!is_gate`: likewise with `"gate"` and `is_gate = true`. Any other `p` (including empty) changes nothing. User values set by the same update call that changes the preset are therefore lost; a call that keeps the preset keeps all user values (script 07).
- **S-027-R41** `create`: `is_gate = (get_string("presets") == "gate")`, `is_upwcomp` from the kind, then the update logic (no clearing at create).
- **S-027-R42** Expander defaults: `presets` default is the *effective* `presets` if it equals `"gate"`, else `"expander"`; all other defaults per §3.2 depending on the same flag (ratio 2/10, release 50/125). They are written as defaults, replacing earlier defaults.
- **S-027-R43** Remaining derivations: `attack_ms`, `release_ms` = int settings as f32; `output_gain_db = get_double("output_gain") as f32`; `knee = is_upwcomp ? get_int("knee_width") as f32 : 0.0`; `ratio = get_double("ratio") as f32`;
  `threshold`; `attack_gain = expf(-1.0f/(rate as f32 * (attack_ms/1000.0f)))`, `release_gain` likewise; `output_gain = db_to_mul(output_gain_db)`; `slope = 1.0f - ratio`; `detector`: if `get_string("detector") == "RMS"` then RMS; if `== "peak"` then PEAK; any other string (incl. `"rms"`, empty) **leaves the previous mode** (Q-4). `ch`, `rate`.
- **S-027-R44** Detection (per block, `n >= 1`): `rmscoef = exp2f(-100.0f / rate as f32)`. All per-channel envelope buffers `E_c[0..n]` and a scratch `env_in[0..n]` start at 0. For each `c in 0..ch` with non-NULL plane (NULL planes are skipped here, but see D-1):
  RMS: `ra[0] = rmscoef*runave[c] + (1 - rmscoef)*powf(x[0],2)`; `env_in[0] = sqrtf(fmaxf(ra[0], 0))`; for `i>=1`: `ra[i] = rmscoef*ra[i-1] + (1-rmscoef)*powf(x[i],2)`, `env_in[i] = sqrtf(ra[i])` (no clamp).
  PEAK: `ra[i] = powf(x[i],2)`, `env_in[i] = fabsf(x[i])`. Then `runave[c] = ra[n-1]`; `E_c[i] = fmaxf(E_c[i], env_in[i])` (i.e. `fmaxf(0, env_in)`, a NaN `env_in` gives 0); `envelope[c] = E_c[n-1]`. (`powf(x,2.0f)` may be computed as `x*x`.) A NaN sample in RMS mode makes `runave[c]` NaN for good (script 06).
- **S-027-R45** Gain stage: for each `c in 0..ch` (all channels, each with its own state), `inv_a = 1.0f - attack_gain`, `inv_r = 1.0f - release_gain`, `g_prev_block = gain_db_buf[c]`; for `i in 0..n` compute `env_db = mul_to_db(E_c[i])`, `diff = threshold - env_db` and:
  - upward: `if env_db <= (threshold - 60.0f)/2 { diff = (env_db + 60.0f > 0) ? env_db + 60.0f : 0.0f }`; `prev = fmaxf(i>0 ? G[i-1] : g_prev_block, 0)`; `gain = 0`; then, in this order and each independently: `if env_db >= threshold + knee/2 { gain = 0 }`; `if threshold - knee/2 >= env_db { gain = slope*diff }`; `if env_db > threshold - knee/2 && threshold + knee/2 > env_db { gain = slope*powf(diff + knee/2, 2.0f) / (2.0f*knee) }` (left to right: `(slope*powf(..))/(2.0f*knee)`).
  - expander: `prev = i>0 ? G[i-1] : g_prev_block`; `gain = (diff > 0) ? fmaxf(slope*diff, -60.0f) : 0.0f`.
  - ballistics (both): `G[i] = (gain > prev) ? attack_gain*prev + inv_a*gain : release_gain*prev + inv_r*gain`.
  - output: expander `m = db_to_mul(fminf(0, G[i]))`, upward `m = db_to_mul(G[i])`; `s[i] *= m * output_gain`.
  After the block `gain_db_buf[c] = G[n-1]`. Because `G` buffers are zeroed per block, `G[i-1]` for `i = 0` is replaced by the stored value as written.
- **S-027-R46** `filter_audio`: `frames == 0` returns unchanged; else detection then gain stage on the same block.
- **S-027-R47** The upward compressor ignores `presets` (never clears; a stray `presets` user value is kept).

### 5.7 Noise gate (script 08)

- **S-027-R60** `update`: `open_db`, `close_db` = doubles as f32; `attack_ms`, `hold_ms`, `release_ms` = ints (`as i32`); `sr = rate as f32`; `sample_rate_i = 1.0f/sr`; `open_threshold = db_to_mul(open_db)`; `close_threshold = db_to_mul(close_db)`;
  `attack_rate = 1.0f/(ms_to_secf(attack_ms)*sr)`, `release_rate = 1.0f/(ms_to_secf(release_ms)*sr)` with `ms_to_secf(m) = (m as f32)/1000.0f` (0 ms gives `inf`); `decay_rate = (open_threshold - close_threshold) / ((1.0f/75.0f)*sr)`; `hold_time = ms_to_secf(hold_ms)`.
- **S-027-R61** Update also resets `is_open = false`, `attenuation = 0`, `level = 0`, `held_time = 0` (**always**, also for an update that changes nothing) and stores `ch`.
- **S-027-R62** `filter_audio`, for each frame `i` in order (state carried across blocks): `cur = fabsf(plane0[i])`; for `j in 0..ch`: `cur = fmaxf(cur, fabsf(plane_j[i]))` (note plane 0 is also read when `ch = 0`);
  `if cur > open_threshold && !is_open { is_open = true }`; `if level < close_threshold && is_open { held_time = 0; is_open = false }` (evaluated after the previous line, so a loud frame opens the gate and the same frame may close it again when `level` is still low);
  `level = fmaxf(level, cur) - decay_rate`;
  `if is_open { attenuation = fminf(1, attenuation + attack_rate) } else { held_time += sample_rate_i; if held_time > hold_time { attenuation = fmaxf(0, attenuation - release_rate) } }`;
  then `plane_c[i] *= attenuation` for all `c in 0..ch`. All planes `< ch` must be non-NULL (D-1).
- **S-027-R63** `close_threshold > open_threshold` is allowed (negative `decay_rate`, `level` then grows); no validation.

### 5.8 3-band EQ (script 09)

- **S-027-R70** `create`: `ch`; `freq = rate as f32`; `lf = 2.0f * sinf( f32( M_PI * 800.0 / freq as f64 ) )`, `hf = 2.0f * sinf( f32( M_PI * 5000.0 / freq as f64 ) )` (`M_PI`, `800.0f`, `5000.0f` promoted to double; the double quotient is rounded to f32 before `sinf`). These and `ch` are **never recomputed** by `update` (script 09, "rate-and-channels-fixed-at-create"). Then `update`.
- **S-027-R71** `update`: `low_gain = db_to_mul(get_double("low") as f32)`, `mid_gain`, `high_gain` likewise (state untouched).
- **S-027-R72** Per channel `c < ch` state: `lf_delay0..3`, `hf_delay0..3`, `sample_delay1..3`, all 0 at create. Per sample `x` (all f32, `EPS = 1.0f/4294967295.0f`):
  ```
  lf0 += lf*(x - lf0) + EPS;  lf1 += lf*(lf0 - lf1);  lf2 += lf*(lf1 - lf2);  lf3 += lf*(lf2 - lf3);  l = lf3
  hf0 += hf*(x - hf0) + EPS;  hf1 += hf*(hf0 - hf1);  hf2 += hf*(hf1 - hf2);  hf3 += hf*(hf2 - hf3)
  h = sd3 - hf3;  m = sd3 - (h + l)
  l *= low_gain;  m *= mid_gain;  h *= high_gain
  sd3 = sd2; sd2 = sd1; sd1 = x
  out = l + m + h
  ```
  (`a += b` means `a = a + b` where `b` is the f32 expression on the right including the `+ EPS` term as written: `a = a + (lf*(x-a) + EPS)`.) Silence therefore settles to a tiny positive bias (script 09), not zero.
- **S-027-R73** `filter_audio` processes planes `0..ch` for `frames` samples in order, plane by plane; every plane must be non-NULL (D-1). `frames == 0` is a no-op.

### 5.9 Async delay (script 11; video path only)

State: `video_frames` FIFO of frame handles, `last_video_ts: u64` (0), `interval: u64` ns (0), `video_delay_reached`, `reset_video`, `reset_audio`, `samplerate` (read at create, unused).
- **S-027-R80** `create`: `update`, then `samplerate = rate`.
- **S-027-R81** `update`: `new = get_int("delay_ms") as u64 * 1_000_000`; if `new < interval` release (via `obs_source_release_frame(parent, f)`, oldest first) all queued frames; `reset_audio = reset_video = true`; `interval = new`; `video_delay_reached = false`. (Also true on the very first call: `0 < 0` is false so nothing is released.)
- **S-027-R82** `filter_video(frame)`: if `reset_video` or `is_jump(frame.ts, last_video_ts)` (`ts < last || ts - last > 1_000_000_000`; the first frame is compared with `last = 0`): release all queued frames, `video_delay_reached = false`, `reset_video = false`. `last_video_ts = frame.ts`. Push the frame; `output =` oldest queued frame; `cur = frame.ts - output.ts` (u64 wrapping); if `!video_delay_reached && cur < interval` return NULL; else pop the oldest, `video_delay_reached = true`, return it (ownership passes to the caller; it is **not** released through the host).
- **S-027-R83** A frame returned by R82 may be the pushed frame itself (interval 0, or after a reset when `interval == 0`).
- **S-027-R84** `filter_remove(parent)`: release all queued frames to the host. `destroy` frees the queue **without** releasing frames (Q-5: the host must call `filter_remove` first).
- **S-027-R85** After the delay is reached a frame is returned for every frame pushed (queue length constant) until a reset; equal timestamps are not jumps.

### 5.10 Noise suppress (plumbing; scripts 12, 13)

State: queues per channel `input[c]`, `output[c]` (bytes of f32), `info` queue of `(frames: u32, timestamp: u64)`, `last_timestamp`, `ch`, `frames` (segment length), `suppress_level`, `use_rnnoise`, `has_mono_src`, `latency`.
- **S-027-R90** `update`: `rate`, `ch` from host; `frames = rate / 100` (integer division); `method = get_string("method")`; `suppress_level = get_int("suppress_level") as i32`; `latency = 1_000_000_000 / 100 = 10_000_000`; `use_rnnoise = (method == "rnnoise")`; `self.frames = frames`, `self.ch = ch` are stored on **every** update.
- **S-027-R91** One-time allocation: only if the relevant state does not exist yet (`!use_rnnoise && speex_states[0]` or `use_rnnoise && rnn_states[0]` exists ⇒ return). With both libraries available the first call allocates **everything** regardless of method: per channel a Speex state `init(frames, rate)` and an RNNoise state (`create(NULL)`), event order per channel `speex.init, rnnoise.create`; segment buffers sized for `frames`/`ch` of that moment; and, if `rate != 48000`, two resamplers: forward `(dst = 48000 Hz, src = rate)` then backward `(dst = rate, src = 48000 Hz)`, float planar, speakers = `layout(ch)` with 1→mono, 2→stereo, 3→2.1, 4→4.0, 5→4.1, 6→5.1, 8→7.1, other→unknown. A later switch of `method` or a second update therefore allocates nothing.
- **S-027-R92** `filter_audio` prologue: `layout = get_speaker_layout(parent)`; `has_mono_src = (layout == MONO && ch == 2)` (set but not used for processing: Q-6). If the needed library state is absent return the block unchanged (cannot happen after R91).
- **S-027-R93** Timestamp discontinuity: if `last_timestamp != 0` and `|last_timestamp - ts| > 1_000_000_000` (signed 64-bit difference): clear `input[*]`, `output[*]`, `info`. Then `last_timestamp = ts`.
- **S-027-R94** Push `(frames, ts)` to `info`; push `frames*4` bytes of plane `c` into `input[c]` for each `c < ch` (plane must be non-NULL, D-1; `frames == 0` pushes an info entry and no data). While `input[0]` holds `>= self.frames*4` bytes: pop `self.frames` samples from each input queue into the segment buffers, run the method (R95/R96), push the `self.frames` samples to each output queue.
- **S-027-R95** Speex segment: for each channel `ctl(SET_NOISE_SUPPRESS, suppress_level)` (all channels first), then convert each sample: `s = x; if s > 1 {1} else if s < -1 {-1}`; `q = (s * 32767.0f) as i16` (truncation toward zero; NaN input is outside the contract), run on each channel, then `x = (q as f32) / 32768.0f`.
- **S-027-R96** RNNoise segment (`R = 480`): *forward* — without resampler: `seg[j] = x[j] * 32768.0f` for `j < 480`; with resampler: `out = resample(x, frames)` (`out_frames` samples); `seg[j] = out[k]*32768.0f` for `k = out_frames - 480 + j >= 0`, else `0`. Process each channel's 480 samples in place. *Back* — without resampler: `x[j] = seg[j] / 32768.0f` for `j < 480`; with resampler: `out = resample_back(seg, 480)`; for `j < frames`: `k = out_frames - frames + j`; `x[j] = (k >= 0) ? out[k] / 32768.0f : 0`.
- **S-027-R97** Output: peek `info` front (zeros if empty): `out_size = info.frames*4` bytes; if `output[0]` holds fewer than `out_size` bytes return NULL (the packet is **kept** in the queues). Otherwise pop the info entry, pop `out_size` bytes from each `output[c]` into the result planes `0..ch`, and return `{frames = info.frames, timestamp = info.ts - latency (u64 wrapping)}` (a pointer different from the input). At most one packet is returned per call, so a backlog persists (script 12). A 0-frame packet returns a 0-frame packet when it reaches the info head.
- **S-027-R98** `destroy`: for each channel in order `speex.destroy` then `rnnoise.destroy`; afterwards, if resamplers exist, the forward resampler then the backward one are destroyed.
- **S-027-R99** Defaults and properties: §3.2 / §3.3 (library dependent).

## 6. Algorithms

Pseudocode is embedded in the rules (R21-R22, R35, R44-R45, R62, R72, R82, R94-R97). Evaluation order inside an f32 expression is the order
written; do not reassociate (e.g. `x + a*(e - x)`, `slope*powf(..)/(2.0f*knee)`).

## 7. Concurrency

`filter_audio` runs on the audio thread, `update`/`tick`/`get_properties` on the UI/graphics threads; the sidechain capture callback runs on the
sidechain source's audio thread. The compressor serialises `sidechain_data`/`max_frames` with one mutex and `weak_sidechain`/name/`check_time` with
another; the audio callback reads `weak_sidechain` under the second lock only to decide the mode (a stale decision is harmless). The rest of the
state is only touched by the audio thread and by `update` (the host does not call `update` concurrently with `filter_audio` of the same filter for audio
filters in practice; the reference does not lock). A Rust implementation must wrap shared state accordingly (R34/R35 are mutually exclusive).

## 8. Reference defects and quirks

- **Q-1** The upward compressor's `knee_width` is an int slider but a *float* suffix `" dB"` is requested; the call is a no-op, so the property has no suffix.
- **Q-2** Limiter attack time: `ATK_TIME = 0.001f` is divided by `MS_IN_S_F` although it already is in seconds: attack constant is 1 µs (`attack_gain ~ 9e-10`), i.e. attack is instantaneous. SHOULD match.
- **Q-3** Compressor sidechain pops `buf_len` frames per call independent of the block length (R35) and underflow substitutes silence for the whole buffer.
- **Q-4** Unknown `detector` strings keep the previous detector.
- **Q-5** Async delay `destroy` does not release queued frames.
- **Q-6** Noise suppress sets `has_mono_src` but never uses it.
- **Q-7** NaN in RMS detection poisons `runave` of that channel permanently; a NaN in the limiter/compressor envelope is absorbed by `fmaxf` (envelope stays finite, the NaN sample itself stays NaN).
- **Q-8** `ratio = 1` with a silent signal makes expander `slope*diff = 0*inf = NaN`; `fmaxf(NaN,-60) = -60` (gain floor) while a non-silent signal gives gain 0.
- **D-1** NULL planes. Gate (planes `0..ch`), EQ, expander (`process_expansion`, planes `0..ch`), noise suppress (input push), compressor sidechain capture (`audio_data->data[c]` for `c < ch`) dereference a NULL plane (crash, script 20); expander detection and compressor/limiter/gain *do* skip NULL planes. Required: never crash; treat a NULL plane as absent (skip it; for the capture callback and noise suppress treat it as silence). A host always supplies `ch` planes.
- **D-2** `expander_update` keeps the pointer returned by `get_string("presets")` across `obs_data_clear` and `set_string` and reads it again (heap use-after-free; observable only under a sanitizer, script 07 under ASan). Required: evaluate against a copy of the original preset string (R40).
- **D-3** `expander_properties(NULL)` (also upward) dereferences the NULL `data` (script 20 `props_null`). Required: `get_properties(NULL)` must work: treat as the non-upward expander when no instance is given (the expander kind is selected by the registration, so a Rust API should take the kind as a parameter, not instance data).
- **D-4** Channel/rate/frame-count growth after the first allocation (compressor: sidechain buffers, noise suppress: copy/segment buffers sized at first update, then `ch`/`frames` overwritten by later updates, script 20) leads to NULL deref or out-of-bounds. Required: never index out of bounds: extend/reallocate the per-channel state (Speex/RNNoise states, buffers) when `ch`, `rate` or `frames` grow after the first allocation (the vectors only require `try-end ok`); behaviour for unchanged configuration is as specified. Eq and the others reading `ch` at create are unaffected.

## 9. Implementation notes (non-normative)

- Model the host as a trait (`audio_config()`, `now_ns()`, sources registry, `speaker_layout(parent)`) and the DSP libraries as traits (`SpeexPreprocess`, `Denoise`, `Resampler`); the vectors only need the recording fakes of §3.4.
- Keep envelopes per call in scratch `Vec<f32>` grown on demand; none of the buffer lengths is observable except `buf_len` of the compressor (R35).
- Use `f32::exp_m1`-free direct calls: `f64::exp` for compressor/limiter, `f32::exp`/`exp2`/`powf`/`log10`/`sin` elsewhere; results may differ from glibc by an ulp (tolerance).
- Do not enable fused multiply-add (`-C target-feature=-fma` is the default for baseline x86-64 but check on aarch64, where LLVM contracts only with `mul_add`).
- Platform: the reference on Windows/macOS uses the same code; `exp`/`powf` ulp differences are within tolerance.

## 10. Conformance coverage

| Script | Rules |
|---|---|
| 01-info | §3.1 |
| 02-gain-invert | R03, R10, R11, R15, R16, R06 |
| 03-defaults-props | §3.2, §3.3, Q-1, R99 |
| 04-limiter | R03-R06, R20-R22, Q-2, Q-7 |
| 05-compressor | R30, R31, R21-R22, Q-7 |
| 06-expander | R40-R46, Q-4, Q-7, Q-8 |
| 07-gate-preset | R40-R42, R47, D-2 |
| 08-noisegate | R60-R63 |
| 09-eq | R70-R73 |
| 10-sidechain | R32-R39, Q-3, §3.3 compressor |
| 11-async-delay | R80-R85, Q-5 |
| 12-noise-suppress-speex | R90-R98, Q-6 |
| 13-noise-suppress-rnn | R91, R96 |
| 20-defects-crash | D-1, D-3, D-4 |
| 21-edge-cases | R03, R05, R06 |

## 11. Open questions

- The audio path of the async delay is compiled out; if a future build defines `DELAY_AUDIO` it needs its own spec.
- Whether to preserve D-2/D-3 exactly in a Rust port is moot (the required behaviour is the safe one).
- Noise suppress with Speex/RNNoise compiled out (flags `LIBSPEEXDSP_ENABLED`, `LIBRNNOISE_ENABLED`) changes defaults and properties (§3.2/§3.3) and turns `process_*` into no-ops; only the "both enabled" build is covered by vectors.
- NVIDIA AFX variants (`LIBNVAFX_ENABLED`) of the noise suppress source are not part of this spec.
## 12. Round 9: audio-side boundary of the RNNoise backend (added)

This round's obs-filters action brings **no new audio filter `.c` files**: the nine audio filters of
`plugins/obs-filters` (gain, invert polarity, compressor, limiter, expander, noise gate, async delay, EQ,
noise suppress) are the files cited in §1 and are fully pinned by R01–R99. The only audio-facing files of
the action are the vendored RNNoise sources consumed by the noise suppress filter (R90–R98); their contract
level is pinned in S-054 §11 (Round 9) and their tolerances in S-054 §5. What remains S-027-side is the
call boundary below.

### Reference files (all cited)

- `plugins/obs-filters/rnnoise/src/denoise.c`, `plugins/obs-filters/rnnoise/src/kiss_fft.c`, `plugins/obs-filters/rnnoise/src/pitch.c`, `plugins/obs-filters/rnnoise/src/celt_lpc.c`, `plugins/obs-filters/rnnoise/src/rnn.c`, `plugins/obs-filters/rnnoise/src/pitch.h`, `plugins/obs-filters/rnnoise/src/arch.h`, `plugins/obs-filters/rnnoise/src/rnn_reader.c`

- **S-027-R100 [SRC]** Call boundary: the noise suppress filter uses exactly three library entry points —
  `rnnoise_create(NULL)` (one state per channel, R91), `rnnoise_process_frame(state, seg, seg)` (in place,
  480 samples, int16-unit scaling, R96) and `rnnoise_destroy` (R98). The returned VAD probability is
  **discarded** in every call: it is not logged, stored, or fed back into the output. No other function of
  `denoise.c` is reachable from the plugin.
- **S-027-R101** What the library computes (480-sample framing, 22 log bands, RNN/GRU pipeline, pitch
  estimation, the first-frame fade-in) is S-054 §11 R45–R54; per-channel independence and the observed
  behaviour are S-054 R10–R13/R42. S-027's rules are independent of the library internals: a substitute
  denoiser is conforming iff it meets S-054 §5 (and keeps R90–R98's queueing, scaling and reset rules).

