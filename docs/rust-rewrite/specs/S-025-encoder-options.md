# S-025 Encoder option logic (obs-x264, obs-nvenc, obs-qsv11, obs-libfdk, obs-ffmpeg encoders)

| | |
|---|---|
| Status | Ready for review |
| Spec version | initial |
| Reference | OBS Studio 33.0.1: the settings/property/option code of the plugins `obs-x264`, `obs-nvenc` (`nvenc-opts-parser`, `nvenc-properties`), `obs-qsv11` (settings translation only), `obs-libfdk`, `obs-ffmpeg` (`video-encoders`, `audio-encoders`) and the shared `opts-parser` |
| Target crate | `crates/obs-encoder-opts` (one module per plugin) |
| Depends on | S-014 (obs_data), S-015 (obs_properties), S-006 (`astrcmpi`, string lists) |
| Conformance | `test/conformance/scripts/encopts/` (56 scripts, runner described in `test/conformance/README.md`, section "encopts runner") |

## 1. Scope

Everything an encoder plugin does **before** a frame is encoded and that does not need the
encoder library: the defaults table and the property list of each encoder, the property
"modified" callbacks (what the UI shows/hides when a setting changes), migrations of legacy
settings, parsing of the user-typed option strings, and the translation of settings into the
parameters handed to the encoder library. The encoder libraries themselves (x264, NVENC, oneVPL,
FFmpeg, fdk-aac) are **not** part of this spec; their calls are specified as a *call trace*
(name, arguments, order) which the conformance runner records with fakes (README contract).

Out of scope: encoding (`encode`, packet parsing, extra data, SEI), ROI maps, texture paths,
hardware probing (caps are an input), the per-codec ffmpeg wrappers (`obs-ffmpeg-nvenc`, `-av1`,
`-openh264`, `-vaapi`, AMF), the QSV library internals, translation files (`obs_module_text`
returns the key; the fakes use the identity so vectors show keys).

Callers: the OBS frontend/libobs call `get_defaults`, `get_properties`, `create`, `update`.

## 2. Terminology and conventions

RFC 2119 words. "Settings" is an `obs_data` object (S-014): getters return the effective value
(user layer, else default layer) or the zero value (`0`, `""`, `false`) when the key is absent or of
another type. `has_user_value(key)` is true only if the **user** layer holds the key.
Defaults are registered in the listed order; that order is observable (iteration of the object). **The iteration order of a settings object is the one of S-014 §6.1**, including its move-to-the-end quirk: e.g. after `set bitrate 2500` on a key that only had a default, `bitrate` moves behind the other keys (vectors `03`, `20`, `75`). An implementation of this spec on top of an `obs_data` that does not reproduce S-014 §6.1 fails these vectors.
"Properties" is an `obs_properties` container (S-015): properties keep insertion order; a new
property is visible and enabled; `apply_settings` runs the property callbacks in **reverse
insertion order** (S-015-R19); `obs_property_modified(p)` runs the callback of that property only;
a callback receives the container and the settings object. `astrcmpi` is ASCII case-insensitive
comparison. `LOG(level, …)` = a log record (the conformance check compares the level and the
position only, never the text). `CALL name(args)` = a call into a faked library; its trace line is
part of the output. Integer widths are stated explicitly (`i32`, `u16`, …); casts between them
truncate (wrap) like C.
Display text: every property description is a locale key (`obs_module_text("Key")`) printed as
the key; list item *names* are keys unless written in quotes.

Platform: vectors are Linux x86-64 (`long` = 64 bit, `enum` = 32 bit unsigned when no negative
value exists). Windows differences are noted in §8 (Q-10).

## 3. Interface (what an implementation exports)

For each encoder: `defaults(settings)`, `properties() -> Properties`, `create(settings, host) -> Option<Encoder>`,
`update(encoder, settings) -> bool` where applicable, `video_info(in) -> out`. Section numbers give
each one. The host (`obs_encoder_*`, `video_output_get_info`, audio info) is an input: output
format/colourspace/range, fps `num/den`, encoder width/height, preferred video format, audio channel
count/speaker layout/sample rate.

Enum numbers used below: video formats `none I420 NV12 I444 ... P010 P216 P416`; colourspaces
`DEFAULT 601 709 SRGB 2100_PQ 2100_HLG`; range `PARTIAL FULL`. List item values are strings
unless "(int)" is written.

## 4. Shared option-string parser

- **S-025-R01** `parse(text)`: a NULL or empty text, or text without words, yields no options and no
  ignored words. Otherwise the text is split on the **space character (0x20) only** into words;
  empty words (runs of spaces, leading/trailing spaces) are dropped. Tabs, newlines, quotes and
  backslashes are ordinary word characters.
- **S-025-R02** A word is an **option** iff it is non-empty, does not start with `=`, contains a `=`, and has at least one
  character after the **first** `=`. Then `name` = the bytes before the first `=` and `value` = everything after it
  (further `=` stay in the value). Every other word is **ignored** (kept, in input order, in the ignored list).
- **S-025-R03** Options keep input order; duplicates are kept; case is preserved; no trimming.
- **S-025-R04** Consumers that log ignored words: obs-x264 only (one `LOG warning` per word, before any option is applied).
  nvenc and ffmpeg encoders drop them silently.

## 5. obs-x264

### 5.1 Defaults (R10) and properties (R11)

- **S-025-R10** Defaults, in this order: `bitrate` int 6000; `use_bufsize` bool false; `buffer_size` int 6000;
  `keyint_sec` int 0; `crf` int 23; `rate_control` string `"CBR"`; `preset` `"veryfast"`; `profile` `""`; `tune` `""`;
  `x264opts` `""`; `repeat_headers` bool false. (No default for `bf`, `cbr`.)
- **S-025-R11** Property list, in order (all initially visible+enabled unless stated):

| name | type | description key | details |
|---|---|---|---|
| `rate_control` | list, combo LIST, format STRING | `RateControl` | items (name = value): `CBR`, `ABR`, `VBR`, `CRF`; modified callback `rate_control_modified` |
| `bitrate` | int | `Bitrate` | 50 .. 10000000 step 50, suffix `" Kbps"` |
| `use_bufsize` | bool | `CustomBufsize` | modified callback `use_bufsize_modified` |
| `buffer_size` | int | `BufferSize` | 0 .. 10000000 step 1 |
| `crf` | int | `CRF` | 0 .. 51 step 1 |
| `keyint_sec` | int | `KeyframeIntervalSec` | 0 .. 20 step 1, suffix `" s"` |
| `preset` | list STRING | `CPUPreset` | `ultrafast superfast veryfast faster fast medium slow slower veryslow placebo` (name = value) |
| `profile` | list STRING | `Profile` | (`None`, `""`), (`baseline`), (`main`), (`high`) |
| `tune` | list STRING | `Tune` | (`None`, `""`), `film animation grain stillimage psnr ssim fastdecode zerolatency` |
| `x264opts` | text, type DEFAULT | `EncoderOptions` | |
| `repeat_headers` | bool | description `repeat_headers` | **invisible** |

Int properties have number type SCROLLER (0). `vfr` is not present (compile-time off).

### 5.2 Callbacks

- **S-025-R12** `rate_control_modified(S)`: `rc` = string `rate_control`; `bufsize` = bool `use_bufsize`;
  `abr = rc =ci "CBR" or rc =ci "ABR"`; `crf_rc = rc =ci "CRF"` (all `astrcmpi`). Set visibility:
  `crf` = `!abr`; `bitrate` = `!crf_rc`; `use_bufsize` = `!crf_rc`; `buffer_size` = `!crf_rc && bufsize`. Returns true.
  So any other string (e.g. `"VBR"`, `""`, `"bogus"`) shows `crf` **and** `bitrate`.
- **S-025-R13** `use_bufsize_modified(S)`: only `buffer_size` visible = `use_bufsize && !(rate_control =ci "CRF")`. Returns true.

### 5.3 Option handling

- **S-025-R14** `validate(value, kind, list)`: returns *valid* if value is NULL or empty, or equals (byte-exact, case-sensitive) a list entry;
  otherwise `LOG warning` ("Invalid KIND: value") and returns *invalid*. Lists: presets and tunes as in R11 (tune list without `None`);
  profiles `baseline main high high10 high422 high444`.
- **S-025-R15** `override_base_params(options, &preset, &profile, &tune)`: for each option in order: if `name =ci "preset"` and
  `validate(value, preset)` is valid, `preset = value`; likewise `"profile"` with the profile list and `"tune"` with the tune list.
  Invalid values leave the variable unchanged. Later valid values replace earlier ones.
- **S-025-R16** `set_param(option)`: skipped (not forwarded) iff `name` (byte-exact, **case-sensitive**) is one of
  `preset profile tune fps force-cfr width height opencl stats qpfile pass`. Otherwise, if `name` equals
  `opencl_is_experimental_and_potentially_unstable`, the name `opencl` is used instead. Then `CALL x264_param_parse(name, value)`;
  a non-zero result gives `LOG warning` and processing continues.
- **S-025-R17** `reset(preset, tune)`: `p' = validate(preset)` (invalid → `"veryfast"`; an **empty** preset stays empty — Q-2),
  `t' = validate(tune)` (invalid → NULL; empty stays `""`); `CALL x264_param_default_preset(p', t')`; success iff it returns 0.
- **S-025-R18** Profile: only when the encoder has no context and the profile string is non-empty:
  `CALL x264_param_apply_profile(profile)`; non-zero → `LOG warning`; this never changes the result of `update_settings`.
  The profile string comes from settings `profile` (**not validated**, Q-3) or from a valid `profile=` option.

### 5.4 Algorithm `update_settings(S, update)` (R19)

- **S-025-R19**

```
preset, profile, tune := string(S,"preset"), string(S,"profile"), string(S,"tune")    // copies, "" if absent
options  := parse(string(S,"x264opts"));  repeat := bool(S,"repeat_headers");  success := true
if !update: LOG info (separator)
if no encoder context:
    override_base_params(options)
    if preset != "": LOG info;  if profile != "": LOG info;  if tune != "": LOG info
    success := reset(preset, tune)
if repeat: params.repeat_headers = 1; params.annexb = 1; params.aud = 1
if success:
    update_params(S, options, update)
    if !update: log_custom_options(options)        // LOG info iff options.count > 0
    if no encoder context: apply profile (R18)
return success
```

### 5.5 `update_params(S, options, update)` (R20)

- **S-025-R20** All values are read first: `rc = string(S,"rate_control")`, `bitrate = i32(int(S,"bitrate"))` (truncating cast of the i64 setting),
  `buffer_size`, `keyint_sec`, `crf` likewise, `bf = i32(int(S,"bf"))`, `use_bufsize`, `cbr_override = bool(S,"cbr")`,
  `width/height` = the encoder's size. Then, in this order:

```
fmt := host preferred format; if fmt not in {I420, NV12, I444}: fmt := (output format in that set) ? output format : NV12     // video_info
if cbr_override: LOG warning; rc := "CBR"
if rc =ci "ABR": mode := ABR, crf := 0
elif rc =ci "VBR": mode := VBR
elif rc =ci "CRF": mode := CRF, bitrate := 0, buffer_size := 0
else: mode := CBR, crf := 0                       // unknown names are CBR
if keyint_sec != 0: params.keyint_max := i32( u32(keyint_sec) * fps_num / fps_den )         // unsigned arithmetic (Q-11)
if !use_bufsize: buffer_size := bitrate
params.vfr_input := false; vbv_max_bitrate := bitrate; vbv_buffer_size := buffer_size; bitrate := bitrate
params.width/height := encoder size; fps_num/den := output fps; timebase_num := fps_den; timebase_den := fps_num
params.log_level := 1 (warning)
if has_user_value(S,"bf"): params.bframe := bf
colour names (cs := output colourspace):  DEFAULT/709 -> bt709/bt709/bt709;  601 -> smpte170m x3;  SRGB -> bt709, iec61966-2-1, bt709;
     any other (2100_*) -> bt709 x3
params.vui.sar := 1x1;  fullrange := (output range == FULL)
params.vui.colorprim/transfer/colmatrix := index of the name in x264's table (R21)
if mode in {CBR, ABR}: rc_method := ABR; if mode == CBR: filler := true       // x264 build >= 139
else:                  rc_method := CRF; rf_constant := f32(crf)               // VBR and CRF
csp := NV12->NV12(4), I420->I420(2), I444->I444(12), other->NV12
for each ignored word: LOG warning
for each option in order: set_param(option)       // R16
if !update: LOG info (settings banner)
```

- **S-025-R21** Colour indices used by the vectors: prim: `bt709`=1, `smpte170m`=6; transfer: `bt709`=1, `smpte170m`=6, `iec61966-2-1`=13;
  matrix: `bt709`=1, `smpte170m`=6. (x264's tables return the position of the name; unknown name → 0.)
- **S-025-R22** `video_info(in)`: `out.format` = the host's preferred format if valid (I420/NV12/I444), else `in.format` if valid, else NV12.
- **S-025-R23** `create(S)`: if output format ∈ {I010, P010, P216, P416}: set_last_error(`HighPrecisionUnsupported`), `LOG warning`, return NULL;
  else if colourspace ∈ {2100_PQ, 2100_HLG}: set_last_error(`HdrUnsupported`), `LOG warning`, NULL. Otherwise: `update_settings(S, false)`;
  on success `CALL x264_encoder_open(params)`; NULL → `LOG warning`; else `CALL x264_encoder_headers`. On `update_settings` failure
  `LOG warning`. Without a context the result is NULL.
- **S-025-R24** `update(enc, S)`: `ok = update_settings(S, true)` (context exists, so no preset reset/overrides/profile; params keep their old values); if ok:
  `CALL x264_encoder_reconfig(params)`, non-zero → `LOG warning`, result = (rc == 0); else false.
- **S-025-R25** `destroy`: `CALL x264_encoder_close`.

## 6. obs-nvenc

### 6.1 Defaults (R30) and read-back (R31)

- **S-025-R30** `defaults(codec)` with the probed caps of that codec, in order: `bitrate` 10000; `max_bitrate` 10000; `target_quality` 20; `keyint_sec` 0;
  `cqp` 20; `device` -1; `bf` = 2 if `caps.bframes > 0` else 0; `rate_control` `"cbr"` (**lowercase**, Q-7); `preset` `"p5"`; `multipass` `"qres"`;
  `tune` `"hq"`; `profile` `"high"` for H.264, `"main"` for HEVC/AV1; `adaptive_quantization` true; `lookahead` = `caps.lookahead`;
  `repeat_headers` false; `force_cuda_tex` false; `disable_scenecut` false.
- **S-025-R31** `properties_read(S)` fills: ints (`bitrate max_bitrate keyint_sec cqp device bf bframe_ref_mode split_encode target_quality`),
  strings (`rate_control preset profile tune multipass`), bools (`adaptive_quantization lookahead disable_scenecut repeat_headers force_cuda_tex`)
  from the effective values (absent → 0 / "" / false; note `bframe_ref_mode` and `split_encode` have no default). Only if `has_user_value("opts")`
  does it read the string `opts` and parse it (R01); a default-only `opts` is ignored. It keeps a reference to S.

### 6.2 Property list (R32) and callback (R33)

- **S-025-R32** `properties(codec)` with caps `c` (and `devices = num_encoder_devices`). In order:

| # | name | cond | type / details |
|---|---|---|---|
| 1 | `rate_control` | | list STRING `RateControl`; items (name → value): `CBR`→`CBR`, `CQP`→`CQP`, `VBR`→`VBR`, `CQVBR`→`CQVBR`, and `Lossless`→`lossless` **iff** `c.lossless`; callback R33 |
| 2 | `bitrate` | | int `Bitrate` 50..4294967 (= u32::MAX/1000) step 50, suffix `" Kbps"` |
| 3 | `target_quality` | | int `TargetQuality` 1..(63 if AV1 else 51) step 1 |
| 4 | `max_bitrate` | | int `MaxBitrate` 0..4294967 step 50, suffix `" Kbps"` |
| 5 | `cqp` | | int `CQP` 1..(63 if AV1 else 51) step 1 |
| 6 | `keyint_sec` | | int `KeyframeIntervalSec` 0..10 step 1, suffix `" s"` |
| 7 | `preset` | | list STRING `Preset`; items `Preset.p1` … `Preset.p7` → `p1` … `p7` |
| 8 | `tune` | | list STRING `Tuning`; items `Tuning.uhq`→`uhq` **iff** `c.uhq`, then `Tuning.hq`→`hq`, `Tuning.ll`→`ll`, `Tuning.ull`→`ull` |
| 9 | `multipass` | | list STRING `Multipass`; `Multipass.disabled`, `.qres`, `.fullres` → `disabled qres fullres` |
| 10 | `profile` | | list STRING `Profile`, names = values. HEVC: `main10` (iff `c.ten_bit`), `main`. AV1: `main`. H.264: `high10` (iff `c.ten_bit`; SDK ≥ 13.0), `high`, `main`, `baseline` |
| 11 | `lookahead` | | bool `LookAhead`, long description `LookAhead.ToolTip` |
| 12 | `adaptive_quantization` | | bool `AdaptiveQuantization`, long `AdaptiveQuantization.ToolTip` |
| 13 | `device` | `devices > 1` | int `GPU` -1..`devices` step 1 |
| 14 | `bf` | `c.bframes > 0` | int `BFrames` 0..`c.bframes` step 1 |
| 15 | `bframe_ref_mode` | `c.bref_modes != 0` and codec != H.264 | list **INT** `BFrameRefMode`; `BframeRefMode.Disabled`→0; `BframeRefMode.Each`→1 iff `bref_modes & 1`; `BframeRefMode.Middle`→2 iff `bref_modes & 2` |
| 16 | `split_encode` | AV1 supported by the probe and `c.engines > 1` and no broken-split driver and codec ∈ {HEVC, AV1} | list INT `SplitEncode`: `SplitEncode.Auto`→0, `.Disabled`→15, `.Enabled`→2, `.ThreeWay`→3 iff `engines > 2`, `.FourWay`→4 iff `engines > 3` (values are SDK enum constants, §11) |
| 17 | `opts` | | text DEFAULT `Opts`, long `Opts.TT` |
| 18-20 | `repeat_headers`, `force_cuda_tex`, `disable_scenecut` | | bool, description = the name, **invisible** |

Number type SCROLLER (0) for ints; list combo type LIST (2).

- **S-025-R33** `rate_control_modified(S)` (case-**sensitive** `strcmp`, Q-7): `cqp = rc=="CQP"`, `vbr = rc=="VBR"`, `cqvbr = rc=="CQVBR"`,
  `lossless = rc=="lossless"`. Visibility: `bitrate` = `!cqp && !lossless && !cqvbr`; `max_bitrate` = `vbr || cqvbr`; `target_quality` = `cqvbr`;
  `cqp` = `cqp`; `preset`, `tune`, `adaptive_quantization` = `!lossless`. Returns true. (The default `"cbr"` and every unknown/other-case string
  fall into the "none of the above" layout.)

### 6.3 User options `apply_user_args` (R34-R38)

- **S-025-R34** Input: the parsed options of `opts` (R31) and the codec. For each option in order: if `name` is `lookaheadDepth` or `keyint` (exact):
  skip silently. Else try, in this order, the RC table, the config table, the codec table (R35–R37); the first table that knows the exact
  (case-sensitive) name applies it and the loop continues. If none knows it: `LOG warning` and the **final result becomes false**, but processing continues.
  Result true iff every option was known or skipped. Every applied option emits one `LOG debug` (for `level`, only when the parsed value is non-zero).
- **S-025-R35** Value parsing: **int options** use `strtol(value, base 10)` (64-bit `long`): leading blanks and sign accepted, parse stops at the first
  non-digit, no digits → 0, overflow saturates to `i64::MAX`/`i64::MIN`; the result is cast (truncated) to the field type. **Bit-field options**
  store `strtol` truncated to the field width (low bits). **QP options** (`constQP minQP maxQP initialRCQP`): if `sscanf(value, "%d:%d:%d")` returns 3 the values
  are P:B:I, otherwise `p=b=i=atoi(value)`; stored as `u32` (two's complement of negatives): `qpInterP=P`, `qpInterB=B`, `qpIntra=I`.
- **S-025-R36** Tables (field: type / width), applied to a zero-initialised `NV_ENC_CONFIG`:
  - RC params: `averageBitRate maxBitRate vbvBufferSize vbvInitialDelay`: u32; `targetQuality targetQualityLSB`: u8; `cbQPIndexOffset crQPIndexOffset`: i8;
    `lookaheadLevel`: 32-bit enum (SDK ≥ 12.2); bit-fields `enableMinQP enableMaxQP enableInitialRCQP enableAQ enableLookahead disableIadapt disableBadapt enableTemporalAQ`: 1 bit,
    `aqStrength`: 4 bits; QP triples as above.
  - Config: `gopLength`: u32; `frameIntervalP`: i32.
  - H.264: `level`(special, R37), `idrPeriod` u32, `useBFramesAsRef` 32-bit enum, `tfLevel` 32-bit enum (SDK ≥ 13.0), `enableFillerDataInsertion` 1 bit.
  - HEVC: `level`(special, ×3), `tier` u32, `idrPeriod` u32, `useBFramesAsRef`, `tfLevel` (≥ 12.2), `enableFillerDataInsertion` 1 bit.
  - AV1: `level` u32 (plain int), `tier numTileColumns numTileRows idrPeriod` u32, `useBFramesAsRef`, `tfLevel` (13.0), `enableBitstreamPadding` 1 bit.
  A name valid for another codec only (e.g. `tier` for H.264, `numTileColumns` for HEVC) is unknown.
- **S-025-R37** `level` for H.264 and HEVC: if the value contains a `.`: `sscanf(value, "%u.%u")` and, if exactly 2 fields were read, `v = hi*10 + lo` (u32 wrapping;
  `%u` accepts a leading sign and wraps negatives); a failed scan leaves `v = 0`. Without `.`: `v = strtol(value)` truncated to u32. If `v == 0` the level is **not** modified and no debug log is emitted;
  else for HEVC `v := v*3` (u32 wrapping), the level is set and one `LOG debug` is emitted.
- **S-025-R38** `get_user_arg_int(name)`: the first parsed option whose name equals `name` exactly → found, value `strtol` cast to `i32`; else not found. Used for `keyint` and `lookaheadDepth`.

## 7. obs-qsv11

### 7.1 Defaults (R40)

- **S-025-R40** `defaults(ver, codec)`, in order: `target_usage` `"TU4"`; `bitrate` 5000; `max_bitrate` 6000; `profile` `"high"` for AVC else `"main"`;
  `rate_control` `"CBR"`; `__ver` int `ver`; `cqp` 23; `qpi` 23; `qpp` 23; `qpb` 23; `icq_quality` 23; `keyint_sec` 0; `latency` `"normal"`;
  `bframes` 3; `repeat_headers` false. Variants: `h264v1`: ver 1 AVC; `h264v2`: ver 2 AVC; `av1`: ver 2; `hevc`: ver 2.

### 7.2 Properties (R41) and callback (R42)

- **S-025-R41** `properties(codec, ver)` in order: `rate_control` (list STRING `RateControl`; items name=value `CBR`, `VBR`, `CQP`, and `ICQ` only if the CPU platform
  is Haswell or newer (`platform >= 6`) or unknown (0); callback R42); `bitrate` int `Bitrate` 50..10000000 step 50 `" Kbps"`; `max_bitrate` int `MaxBitrate` 50..10000000 step 50 `" Kbps"`;
  if `ver >= 2`: `cqp` int `CQP` 1..(63 if AV1 else 51); else `qpi`, `qpp`, `qpb` ints (descriptions `QPI`, `QPP`, `QPB`) 1..51;
  `icq_quality` int `ICQQuality` 1..51; `target_usage` list STRING `TargetUsage`, items `TargetUsage.TU1`…`TU7` → `TU1`…`TU7`;
  `profile` list STRING `Profile` (names = values): AVC `high main baseline`; AV1 `main`; HEVC `main main10`; `keyint_sec` int `KeyframeIntervalSec` 0..20 `" s"`;
  `latency` list STRING `Latency`, long description `Latency.ToolTip`, items `ultra-low low normal`; `bframes` int `BFrames` 0..3.
- **S-025-R42** `rate_control_modified(S)`: `rc = string(rate_control)`. Visibility: `max_bitrate` = (`rc =ci VBR`); `bitrate` = !(`rc =ci CQP` or `ICQ`);
  `qpi qpb qpp cqp` = (`rc =ci CQP`) (each only if the property exists); `icq_quality` = (`rc =ci ICQ`). **Then the callback rewrites S**: `update_latency(S)`,
  `update_targetusage(S)`, `update_ratecontrol(S)` in that order. Returns true. A `bogus` rate control leaves `bitrate` visible and everything else hidden.

### 7.3 Migrations (R43)

- **S-025-R43** (all string comparisons `astrcmpi`)
  - `update_latency(S)`: `async = 4`, `la = 15`, `changed = false`. If key `async_depth` exists (any layer): `async = int(it)`, erase the key, `changed = true`.
    Same for `la_depth` (`la`). `rc = string(rate_control)`; `lookahead = false`; `LA_CBR` → set user `rate_control="CBR"`, lookahead; `LA_VBR` → `"VBR"`, lookahead; `LA_ICQ` → `"ICQ"`, lookahead.
    If `changed`: if lookahead: `latency = (la == 0 || la >= 15) ? "normal" : "low"`; else `latency = (async != 1) ? "normal" : "ultra-low"` (set as user string).
  - `update_targetusage(S)`: `veryslow|quality`→`TU1`; `slower`→`TU2`; `slow`→`TU3`; `medium|balanced`→`TU4`; `fast`→`TU5`; `faster`→`TU6`; `veryfast|speed`→`TU7`; anything else (including `TU1..7`) unchanged. Result is set as user string.
  - `update_ratecontrol(S)`: `VCM`→`CBR`; `AVBR`→`VBR`; others unchanged.
  `qsv_migrate` (the runner command) = `update_latency; update_targetusage; update_ratecontrol`.

### 7.4 `create` and `update_params` (R44-R46)

- **S-025-R44** `create(codec, S, tex)`: params zero-initialised. Format check on the output format: `I010`/`P010`: if codec is AVC: set_last_error(`10bitUnsupportedAvc`),
  `LOG error`, NULL; else `params.video_fmt_10bit = true`. `P216`/`P416`: set_last_error(`16bitUnsupported`), `LOG error`, NULL (any codec). Any other format: if colourspace
  ∈ {2100_PQ, 2100_HLG}: set_last_error(`8bitUnsupportedHdr`), `LOG error`, NULL. Then `update_params(S)`; `CALL qsv_encoder_open(codec, tex)`; NULL → `LOG warning`;
  `LOG info` (library version, always); result NULL iff no context.
- **S-025-R45** `update_params(S)`:

```
update_latency(S); update_targetusage(S)                               // R43: S is modified
read: target_usage, profile, rate_control, latency (strings); bitrate, max_bitrate, qpi, qpp, qpb, cqp, __ver, icq_quality, keyint_sec, bframes as i32;
      cbr_override = bool("cbr"); repeat_headers; bFrames := bframes; if has_user_value("bf"): bFrames := int("bf")
platform = host cpu platform; if platform in {IVB(3), SNB(2)}: bFrames := 0
usage: TU1->1, TU4->4, TU7->7, TU2->2, TU3->3, TU5->5, TU6->6, else unchanged (0)
codec AVC:  profile baseline->66, main->77, high->100, else 0 ("H.264" in logs)
codec HEVC: main -> 1 (if GPU texture P010 active: LOG warning, 2), main10 -> 2, else 0
codec AV1:  profile := 1 always
VideoFormat := 5; VideoFullRange := (range == FULL)
colour (prim, trc, matrix, chroma locs): 601 -> 6,6,6,0,0; DEFAULT/709 -> 1,1,1,0,0; SRGB -> 1,13,1,0,0; 2100_PQ -> 9,16,9,2,2; 2100_HLG -> 9,18,9,2,2
if cs in {PQ, HLG}: peak := PQ ? host.hdr_nominal_peak : 1000 (HLG);
      DisplayPrimariesX = {13250, 7500, 34000}; DisplayPrimariesY = {34500, 3000, 16000}; WhitePoint = (15635, 16450);
      MaxDisplayMasteringLuminance := peak*10000 (u32); MinDisplay... := 0; MaxContentLightLevel := peak; MaxPicAverageLightLevel := peak   (u16)
if cbr_override: LOG warning; rate_control := "CBR"
rc: CBR->1, VBR->2, CQP->3, ICQ->9, else 0 (unchanged)
LADEPTH := 0
latency ultra-low: async := 1;  low: async := 4, and LADEPTH := 30 iff rc in {CBR, VBR};  normal: async := 4, LADEPTH := 60 iff rc in {CBR, VBR};  other: async stays 0
if LADEPTH > 0: clamp to [10, 100]
if __ver == 1: QPI,QPP,QPB := qpi,qpp,qpb  else: q := cqp (x4 if AV1); QPI = QPP = QPB := q
TargetBitRate, MaxBitRate, Width, Height, FpsNum, FpsDen, nbFrames, KeyIntSec, ICQQuality := u16 truncation of bitrate, max_bitrate, encoder width/height, fps num/den, bFrames, keyint_sec, icq_quality
bRepeatHeaders := repeat_headers
logs: LOG info (settings banner: codec, rate control); LOG info target_bitrate iff rc not in {ICQ, CQP}; LOG info max_bitrate iff rc == VBR; LOG info ICQ quality iff rc == ICQ;
      LOG info LA depth iff LADEPTH != 0; LOG info qp line iff rc == CQP; LOG info (final settings block); LOG info "debug info"
```
  Note the checks on `rate_control` use the **migrated** (by `update_latency`) value; `update_ratecontrol` is **not** called here (so `VCM`/`AVBR` give rc 0).
- **S-025-R46** `update(enc, S)`: sets `TargetBitRate := u16(int(S,"bitrate"))` only, `CALL qsv_encoder_reconfig`; false (+ `LOG warning`) if it returns 0. Nothing else is re-read.
- **S-025-R47** `video_info(kind, in)`: `pref = host preferred format`. H.264/v1/v2: valid = {NV12}; AV1/HEVC: valid = {NV12, P010}. `out.format = pref if valid else (in.format if valid else NV12)`.
  Size cap: `out.width/height = encoder size`; platform := host cpu platform, but if the (single) adapter is a discrete GPU, platform := unknown(0); if `platform <= IVB(3)` and `platform != 0`:
  `width > 1920 → 1920`, `height > 1200 → 1200`.

## 8. obs-libfdk

- **S-025-R50** Defaults: `bitrate` 128, `afterburner` true. Properties: `bitrate` int `Bitrate` 32..1024 step 32; `afterburner` bool `Afterburner`.
- **S-025-R51** `create(S)`: `bitrate = i32(int("bitrate")) * 1000` (wrapping), `afterburner = bool ? 1 : 0`, `adts = bool("set_to_ADTS")`, `transmux = adts ? 2 : 0`. `bitrate == 0` → `LOG error`, NULL.
  `channels`/`rate` = host audio. Channel mode: 1→1, 2→2, 3→3, 4→4, 5→5, 6→6, 8→33; any other count → `LOG error`, then fail (`LOG warning` "creation failed", NULL; no library call).
  Library trace (each call may fail → `LOG error`, close the handle if open, `LOG warning`, NULL; negative bitrates pass): `aacEncOpen(0, channels)`; `SetParam`: AOT=2, SAMPLERATE=rate,
  CHANNELMODE=mode, CHANNELORDER=1, BITRATEMODE=0, BITRATE=bitrate, TRANSMUX=transmux, AFTERBURNER=afterburner; `aacEncEncode(init)`; `aacEncInfo`. On success `LOG info` ×2.
  Derived: `frame_size_bytes = frameLength*2*channels`; `packet_buffer_size = max(8192, channels*768)`. `destroy`: `aacEncClose`, `LOG info`.

## 9. obs-ffmpeg encoders

### 9.1 Video: `ffmpeg_video_encoder_update(bitrate, keyint_sec, voi, info, opts)` (R60)

- **S-025-R60**

```
rate := i32(bitrate * 1000)                                   // signed 32-bit multiply; overflow wraps (D-1)
ctx.bit_rate := rate;  ctx.rc_buffer_size := rate;  width/height := encoder size;  time_base := fps_den/fps_num;  framerate := fps_num/fps_den
pix_fmt := map(info.format)        I444->YUV444P  I412->YUV444P12LE  I420->YUV420P  NV12->NV12  YUY2->YUYV422  UYVY->UYVY422  YVYU->YVYU422  RGBA->RGBA  BGRA/BGRX->BGRA
                                   Y800->GRAY8  BGR3->BGR24  I422->YUV422P  I210->YUV422P10LE  I40A->YUVA420P  I42A->YUVA422P  YUVA->YUVA444P  YA2L->YUVA444P12LE
                                   I010->YUV420P10LE  P010->P010LE  P216->P216LE  P416->P416LE  (NONE, AYUV, others: NONE)
color_range := (info.range == FULL) ? JPEG(2) : MPEG(1)
colour (primaries, trc, colorspace): 601 -> (6, 6, 6);  DEFAULT/709 -> (1, 1, 1);  SRGB -> (1, 13, 1);  2100_PQ -> (9, 16, 9);  2100_HLG -> (9, 18, 9)      // FFmpeg enum numbers
chroma_sample_location := determine(pix_fmt, colorspace)      // R61
if keyint_sec != 0: gop_size := i32( u32(keyint_sec) * fps_num / fps_den )   else untouched
enc.height := ctx.height
for each option of parse(opts) in order: CALL av_opt_set(ctx.priv_data, name, value, 0)      // result ignored; ignored words dropped silently
```
- **S-025-R61** `determine(pix_fmt, colorspace)`: with the chroma shifts `(w, h)` of the pixel format: no descriptor → UNSPECIFIED(0); `h == 0`: `w == 0` → CENTER(2), `w == 1` → LEFT(1), else UNSPECIFIED; `h == 1 && w == 1` (4:2:0) → TOPLEFT(3) if `colorspace == BT2020_NCL`(9) else LEFT(1); otherwise UNSPECIFIED.

### 9.2 Audio encoders (R62)

- **S-025-R62** Defaults (all audio encoders): `bitrate` 128. Properties: `bitrate` int `Bitrate` 64..1024 step 32.
- **S-025-R63** `create(codec_name, alt, requested_format)` (aac: `aac`, none; opus: `libopus`/`opus`, FLT; pcm16 `pcm_s16le`, pcm24 `pcm_s24le`, pcm32 `pcm_f32le`, all none; alac: `alac`, S32P; flac: `flac`, S16):

```
bitrate := i32(int("bitrate"))
find codec by name (trace); if missing and alt: find alt (type := alt)
LOG info (separator)
if no codec: LOG warning, fail.   descriptor missing: LOG warning, fail.
if bitrate == 0 and not lossless(descriptor): LOG warning "Invalid bitrate", fail           // negative accepted
ctx.bit_rate := lossless ? 0 : bitrate*1000 (i32 multiply)
layout := default layout of N host channels                                   // N = audio channels; native masks (FL=1 FR=2 FC=4 LFE=8 BL=0x10 BR=0x20 FLC=0x40 FRC=0x80 BC=0x100 SL=0x200 SR=0x400):
                                                                               // 1: 0x4  2: 0x3  3: 0x7  4: 0x107  5: 0x607  6: 0x60f  7: 0x70f  8: 0x63f
if speakers == 4.1: layout := 4.1 (mask 0x10f, 5 channels);  if speakers == 2.1: layout := 3.0 (surround, mask 0x7, 3 channels);  if speakers == 7.1 and type =ci "alac": layout := 7.1 wide-back (mask 0xff, 8 channels)
ctx.sample_rate := host rate
formats := supported sample formats (maybe none), rates := supported sample rates (maybe none)
if formats: ctx.sample_fmt := requested if listed, else first listed;  else ctx.sample_fmt := FLTP
if rates: closest := 0; for r in rates in order: if |cur - r| < |cur - closest|: closest := r;   if closest != 0: ctx.sample_rate := closest
LOG info (bitrate/channels/layout banner)
strict_std_compliance := -2;  flags := GLOBAL_HEADER(0x400000 = 4194304)
open codec (trace); failure -> set_last_error("Failed to open AAC codec: " + strerror(rc)), LOG warning, fail   // text is compared; fake strerror(E) = "error E"
frame_size := ctx.frame_size, or 1024 if 0
after opening: allocate the sample buffer (trace av.samples_alloc with channels, frame_size, sample_fmt)
audio_size = bytes per frame/sample of the converted format x speakers (R64), planes
```
  A fail path also logs and returns NULL. (`requested = NONE` never matches a listed format.)
- **S-025-R64** `frame_size_bytes = frame_size * audio_size`; `audio_size` = `bytes_per_sample(format) * (planar ? 1 : channels)`, `planes = planar ? channels : 1` with the OBS audio format for the context's sample format (`U8 S16 S32 FLT` interleaved and `…P` planar; other → 16BIT interleaved).
  `channels` is the speaker layout's channel count: 1 mono, 2 stereo, 3 2.1, 4 quad, 5 4.1, 6 5.1, 7 6.1, 8 7.1 (speaker layout number = channel count except 4.1 = 5).

## 10. Concurrency

None of this code is thread-safe by itself: defaults/properties/update run on the UI/encoder-management thread; `create` runs once per encoder. QSV `create` and `destroy` take a process-wide mutex around
`qsv_encoder_open/close`. An implementation may use any lock; behaviour is single-threaded in the vectors.

## 11. Reference defects and quirks

- **Q-1 (x264 option-name case).** `PRESET=slow` / `Tune=film` override the base setting (matched `=ci`) but are not in the (case-sensitive) skip list, so they are also forwarded to `x264_param_parse`. Same for `FPS`, `Width`, `PASS`. Required: reproduce. Scripts `70`, `09`, `10`.
- **Q-2 (x264 empty preset).** An empty `preset` setting is passed as `""` to `x264_param_default_preset` (no `veryfast` fallback); only non-empty invalid presets fall back. Required: reproduce. `11`, `71`.
- **Q-3 (x264 profile validation).** The settings-level `profile` is not validated; a `profile=` option is validated against the six-name list. Required: reproduce. `71`, `10`.
- **Q-4 (opencl alias).** The skip list is checked against the original name: `opencl=…` is dropped, but the long alias passes the check and is forwarded renamed to `opencl`. Required: reproduce. `09`.
- **Q-5 (x264 rate-control string vs UI).** Unknown/empty `rate_control` is CBR in the parameters but not ABR/CBR in the callback (shows `crf`). VBR maps to x264's CRF method with VBV limits. Required: reproduce. `05`, `07`, `72`.
- **Q-6 (x264 negative keyint).** `keyint_sec` is converted to unsigned before the multiplication/division: a negative value yields a huge unsigned intermediate; the result is cast to `i32` (e.g. `-3*60/1` → -180, but `-3*30000/1001` → 4290587). Required: reproduce (Q-11 note). `08`.
- **Q-7 (nvenc rate-control case).** Default `"cbr"` is lowercase, callback compares case-sensitively to `CQP/VBR/CQVBR/lossless`. Required: reproduce. `23`, `73`.
- **Q-8 (nvenc level/`%u`).** See R37 (negative or huge parts wrap). Required: reproduce. `28`, `74`.
- **Q-9 (QSV callback mutates settings).** R42/R43: the UI callback (and `update_params`) rewrites S (erases `async_depth`/`la_depth`). Required: reproduce. `32`, `33`, `75`.
- **Q-10 (platform integers).** `strtol` is 64-bit on LP64 targets; on Windows `long` is 32-bit and saturates at 2^31-1/-2^31 (R35 would differ for values beyond 32 bits). The vectors are LP64; Windows builds of a port SHOULD keep LP64 semantics (document deviations).
- **Q-11 (unsigned keyint).** See Q-6 (also used for the ffmpeg `gop_size`).
- **Q-12 (audio sample-rate snapping).** The search starts from `closest = 0`, so a requested rate nearer to 0 than to every listed rate stays unchanged (not snapped). Ties keep the earlier list entry. Required: reproduce. `63`, `78`.
- **D-1 (ffmpeg bitrate overflow).** `bitrate*1000` overflows `i32` for `bitrate > 2147483` (UB in C). Required: no panic; wrapping multiplication reproduces what every real build does; vector lines for overflowing values are informational (`info_`). `52`.
- **D-2 (QSV item leak).** `update_latency` never releases the items it looks up. Required: nothing to reproduce (do not leak). `77`.
- **D-3 (QSV 16-bit truncation).** Bitrates, size, fps, keyint, ICQ quality and B-frame count are stored in `u16`; larger values wrap silently although the properties allow bitrates up to 10 000 000. Required: reproduce the wrap. `76`, `37`.

## 12. Implementation notes (non-normative)

- Model the fakes as a trait (`X264Lib`, `NvCaps`, `FfmpegOpts`, …) so that production code calls the real library through the same interface the conformance runner fakes.
- The parameter structs (`x264_param_t`, `qsv_param_t`, `NV_ENC_CONFIG`) can be plain Rust structs with the fields printed by the runner; keep `i32`/`u16`/`u32` field widths to get wrapping right (`as` casts).
- `obs_properties` callbacks that edit visibility need mutable access to the container while it is being iterated; model properties as a `Vec` with indices and run callbacks via `(&mut Properties, &mut Settings)`.
- The QSV migrations mutate the settings in a UI callback: keep them as plain functions on the settings type.
- Option-name matching in R15 (`astrcmpi`) vs R16 (`strcmp`) must not be unified.

## 13. Conformance coverage

| Rule | Scripts |
|---|---|
| R01–R04 | `01-parser-grammar`, `02-parser-quirks`, `09-x264-optstring-params` (R04), `51-ffmpeg-video-options` |
| R10, R11 | `03-x264-defaults`, `04-x264-properties` |
| R12, R13 | `05-x264-visibility`, `06-x264-visibility-single-callback`, `72-quirk-x264-visibility-vs-params` |
| R14, R15 | `10-x264-base-overrides`, `11-x264-preset-tune-validation`, `71-quirk-x264-empty-and-unvalidated` |
| R16 | `09-x264-optstring-params`, `70-quirk-x264-option-name-case` |
| R17, R18 | `11`, `12-x264-profile`, `71` |
| R19 | `07-x264-ratecontrol-params`, `09`, `10`, `14-x264-misc-settings` |
| R20, R21 | `07`, `08-x264-bufsize-keyint`, `13-x264-video-format-and-color`, `14` |
| R22 | `13` |
| R23 | `15-x264-create-rejections`, `16-x264-create-failures` |
| R24, R25 | `17-x264-update` |
| R30 | `20-nvenc-defaults` |
| R31 | `24-nvenc-read-settings` |
| R32 | `21-nvenc-properties`, `22-nvenc-properties-caps` |
| R33 | `23-nvenc-visibility`, `73-quirk-nvenc-rate-control-case` |
| R34–R36 | `25-nvenc-options`, `26-nvenc-qp-options`, `27-nvenc-codec-options` |
| R37 | `28-nvenc-level-parsing`, `74-quirk-nvenc-level-sscanf` |
| R38 | `29-nvenc-userarg` |
| R40 | `30-qsv-defaults` |
| R41 | `31-qsv-properties` |
| R42 | `32-qsv-visibility-and-migration`, `75-quirk-qsv-callback-mutates-settings` |
| R43 | `33-qsv-legacy-migration`, `75`, `77-defect-qsv-item-leak` |
| R44 | `38-qsv-rejections` |
| R45 | `34-qsv-params-ratecontrol`, `35-qsv-params-latency`, `36-qsv-params-codecs`, `37-qsv-params-misc`, `76-defect-qsv-16bit-truncation` |
| R46, R47 | `39-qsv-update-and-video-info` |
| R50 | `40-fdk-defaults-properties` |
| R51 | `41-fdk-create-channels`, `42-fdk-create-settings` |
| R60, R61 | `50-ffmpeg-video-update`, `51`, `52-ffmpeg-video-bitrate-overflow` |
| R62 | `60-ffmpeg-audio-defaults-properties` |
| R63, R64 | `61-ffmpeg-audio-create`, `62-ffmpeg-audio-layouts`, `63-ffmpeg-audio-formats-rates`, `78-quirk-audio-sample-rate-snap` |
| Q-1…Q-12, D-1…D-3 | scripts named in §11 |

## 14. Open questions

- The NVENC SDK header is not available in the build environment; `gen/encopts_inc/ffnvcodec/nvEncodeAPI.h` is a reconstruction. **Harness-assumed SDK 13.0 values, to be verified against the real SDK**:
  `NV_ENC_SPLIT_{AUTO,TWO_FORCED,THREE_FORCED,FOUR_FORCED,DISABLE}_MODE` = 0, 2, 3, 4, **15**; `NV_ENC_BFRAME_REF_MODE_*` = 0/1/2; widths of the bit-fields (checked against the `APPLY_BIT_OPT` arguments: 1 bit each, `aqStrength` 4) and integer types
  (checked against the `APPLY_INT_OPT` arguments); the existence of `tfLevel`/`lookaheadLevel`/`high10` assumes SDK ≥ 13.0 (older SDKs omit them, making those option names unknown).
  The enum-typed fields are printed as unsigned 32-bit values.
- `obs-ffmpeg-nvenc`, `-av1`, `-openh264`, `-vaapi`, AMF and the QSV library (`qsv_param_parse` etc.) option mappings are not covered.
- Hardware probing (`get_encoder_caps`, `num_encoder_devices`) and the locale tables are inputs: translated texts are unspecified.
- Windows/macOS: `strtol` width (Q-10); `enum` signedness in the NVENC debug logs.
