# S-054 obs-filters remainder (module table, vendored RNNoise / SpeexDSP contract, effect inventory)

| | |
|---|---|
| Status | Ready for review (Rust sufficiency check skipped by assignment) |
| Reference | OBS Studio 33.0.1: `plugins/obs-filters/obs-filters.c`, `noise-suppress-filter.c` (real-library behaviour), `rnnoise/src/*.c` (vendored), `data/*.effect` |
| Depends on | S-027 (audio filter plumbing; its rules S-027-R90..R98 define what the plugin does with the libraries), S-028 (video filters, shader maths), S-020 (effect language) |
| Conformance | `test/conformance/scripts/filtersrest/` (8 scripts), runner section "filtersrest runner" in `test/conformance/README.md` |

## 1. Scope

S-027 specified the noise suppress filter against *recording fakes* of SpeexDSP/RNNoise/resampler. This spec adds what was
missing for a complete `obs-filters`: (a) the **contract of the real libraries** as the plugin uses them, verified by linking
the real vendored RNNoise, the real `libspeexdsp` and the real libswresample-backed resampler; (b) the **module
registration table** (`obs-filters.c`, the last unlinked `.c` of the plugin); (c) the inventory of the `data/*.effect` files
(parameters, techniques) with pointers to where their text and semantics are authoritative.

Libraries are treated as **inputs**: a Rust port may use an equivalent crate (e.g. an `nnnoiseless`-style RNNoise port using
the same model weights, a SpeexDSP binding) provided it meets the tolerances in section 5. The 415 kB `rnn_data.c` weights
are data of that library; do not re-specify them.

## 2. Module table (`obs_module_load`)

- **S-054-R01** `obs_module_description()` is `"OBS core filters"`; `obs_module_load()` returns `true`.
- **S-054-R02** `obs_module_load` registers, in this exact order, one `obs_source_info` per line (identifier = C symbol; the
  registered `.id` string is given in S-027/S-028):
  `mask_filter, mask_filter_v2, crop_filter, gain_filter, eq_filter, hdr_tonemap_filter, sdr_on_hdr_filter, color_filter,
  color_filter_v2, scale_filter, scroll_filter, gpu_delay_filter, color_key_filter, color_key_filter_v2, color_grade_filter,
  sharpness_filter, sharpness_filter_v2, chroma_key_filter, chroma_key_filter_v2, async_delay_filter, [noise_suppress_filter,
  noise_suppress_filter_v2], invert_polarity_filter, noise_gate_filter, compressor_filter, limiter_filter, expander_filter,
  upward_compressor_filter, luma_key_filter, luma_key_filter_v2` -> 30 registrations (28 when `HAS_NOISEREDUCTION` is not
  defined, i.e. neither SpeexDSP nor RNNoise was built; the bracketed pair is then skipped).
- **S-054-R03** The two noise suppress registrations: v0: `id="noise_suppress_filter"`, `version=0`, type FILTER (1),
  `output_flags = AUDIO|CAP_OBSOLETE = 0x402`; v2: same id, `version=2`, flags `AUDIO = 0x2`. (Hidden-in-UI of the obsolete one
  is host business.)

## 3. Library contracts as used by the plugin

### 3.1 RNNoise (vendored, `DenoiseState`)

- **S-054-R10** Constants: frame = 480 samples, sample rate 48000 Hz, mono. `rnnoise_create(NULL)` selects the built-in model
  (weights from `rnn_data.c`) and returns a state; `rnnoise_destroy(state)` frees it. One state per channel; states share
  nothing (script `11`: two fresh states fed identically produce bit-identical output; a state fed more audio before
  differs from a fresh one: history is kept between calls).
- **S-054-R11** `rnnoise_process_frame(st, out, in) -> vad`: reads 480 floats, writes 480 floats, returns the voice-activity
  probability in `[0,1]`. `out == in` (in place) is allowed and gives the same result as separate buffers (script `11`).
  Sample **scale is int16 units**: the plugin multiplies float samples by 32768 before and divides by 32768 after
  (S-027-R96). The library does not clamp.
- **S-054-R12** Observed behaviour (informative, tolerance section 5), 200 Hz harmonic voice at rms ~3500 + noise:
  frame 0 output is strongly attenuated (rms ~81, i.e. start-up fade/warm-up), frame 1 ~80 %, from frame 2 on ~99 % of the input
  rms and `vad` -> 0.99. When the voice stops, output decays over ~3 frames (3346 -> 665 -> 43). Uniform white noise of rms
  1700 is attenuated to ~1200-1350 after warm-up (it does not drive the output to zero: the net is a speech/noise gain
  estimator, not a stationary-noise gate); a pure sine passes at gain ~1.0 with `vad` rising 0.5 -> 0.9.
- **S-054-R13** Band-limited processing is not applied; there is no latency compensation inside the library beyond the first
  frame's fade-in. The plugin adds a 10 ms (`latency = 10_000_000 ns`) timestamp shift (S-027-R90/R99).

### 3.2 SpeexDSP preprocessor

- **S-054-R20** `speex_preprocess_state_init(frame_size, rate)` with `frame_size = rate/100` (480 @ 48 kHz, 441 @ 44.1 kHz),
  `speex_preprocess_run(st, i16*)` works in place on exactly one frame, and returns an int (ignored). Denoising is on by default.
- **S-054-R21** `SPEEX_PREPROCESS_SET_NOISE_SUPPRESS` stores the **negative magnitude**: setting `-30/-60/0` reads back
  `-30/-60/0`; setting a positive value `12` reads back `-12` (library behaviour; the plugin slider is `-60..0`, so
  positive values do not occur). Calls return 0.
- **S-054-R22** Observed behaviour (informative; 200 frames of white noise, input rms ~950): first output frame ~0 (rms 3);
  frame 50 (0.5 s) already attenuated to ~480 (both -30 and -60); frame 100: ~62 at `-30`, ~20 at `-60`; frame 199: ~55 at
  `-30`, ~17 at `-60`; with level `0` the output stays at the input level (~930). A port MUST show the same ordering (lower
  level -> stronger long-run suppression, `0` -> none) and the first-frame fade-in; exact values are library dependent
  (tolerance section 5). Short runs (frames 1-5) pass the noise nearly unchanged.

### 3.3 Resampler (non-48 kHz hosts)

- **S-054-R30** For `rate != 48000` the plugin builds a forward resampler `(48000 <- rate)` and a backward `(rate <- 48000)`
  (float planar, layout from channel count, S-027-R91). With the real resampler the segment lengths are 441 (44.1 kHz) and
  960 (96 kHz) in, and the filter returns the same number of frames as it received (script `32`: `frames=441`, `frames=960`;
  signal levels reach the unprocessed level after two segments, rms3 within 10 %). The resampler is not specified here (S-044).

## 4. Plugin behaviour with real libraries

Everything in S-027-R90..R99 applies unchanged; the real-library vectors confirm in addition:

- **S-054-R40** Packet handling (script `30`): input is queued per channel; each complete 10 ms segment is processed; the
  oldest `info` entry is released as soon as enough output is queued (`frames` equal to the input packet size,
  `timestamp = input_ts - 10_000_000`), else `filter_audio` returns NULL (first small packets).
- **S-054-R41** A timestamp jump of more than 1 s in either direction clears all queues before the new packet is queued
  (`reset_data`, script `30`), so the next packet is output with zero queued history (first-frame fade-in recurs).
- **S-054-R42** Channels 1..N-1 are processed by independent states: with input silence on a channel the output of that channel
  is exactly silence (script `31`, `33`: rms 0). The speex path produces rms 0 for the first segment of every channel (R22).
- **S-054-R43** A mono parent in a stereo host only sets `has_mono_src` (no change to the produced samples in v2 RNNoise;
  script `33`); 8 channels are supported (`MAX_PREPROC_CHANNELS = 8`).
- **S-054-R44** v1 with method `-` (defaults) uses speex; v2 default is rnnoise (S-027 section 3.2).

## 5. Tolerances (normative for ports using other library implementations)

| Quantity | Tolerance vs the vectors |
|---|---|
| RNNoise `vad` | +-0.02 absolute |
| RNNoise `rms_out`, `peak_out` (int16 units) | +-3 % relative or +-2 absolute, whichever is larger; frame 0 (fade-in) +-30 % |
| RNNoise bit-exactness between two states, in-place vs copy | exact (self-consistency, any implementation) |
| Speex `rms_out` | +-10 % (+-5 absolute) after convergence; first frame < 5 |
| Filter `rms*` columns (float, 3 decimals) | +-0.01 absolute, resampled paths +-10 % |
| Frame counts, timestamps, queue sizes, `null` returns | exact |

The vectors print rounded values (`%.3f` for vad and float rms, integers for int16-unit rms) so that libm differences between
machines do not appear; they were generated with glibc `sin`, `sqrt`, `-O0` GCC, no fast-math. The deterministic test signals
are `silence`, `sine F A`, `noise A SEED` (LCG `x = x*1664525 + 1013904223`, sample `((x>>8)/2^23) - 1`, scaled by A),
`speechlike F A NA SEED` (sum of harmonics `sin(2 pi k F t)/k`, k=1..8, times A/2, plus noise of amplitude NA); phase is the
running sample counter / rate.

## 6. Effect files (`plugins/obs-filters/data/*.effect`)

All files are compiled by the S-020 effect compiler; their exact text, parameters, defaults, techniques and assembled shader
text are pinned (real C parser against a fake OpenGL backend) by `test/conformance/vectors/effect/73-corpus-obs-filters.expected`
(S-020). The semantics of the pixel shaders (key functions, tonemap maths, LUT sampling) are S-028 section 6.4. Inventory
(types from the real parser; every effect also declares `ViewProj` and `image`):

| File | Parameters (besides ViewProj/image) | Techniques |
|---|---|---|
| blend_add/mul/sub_filter | target:tex, color:vec4, mul_val:vec2, add_val:vec2 | Draw |
| mask_alpha_filter, mask_color_filter | target:tex, color:vec4, mul_val:vec2, add_val:vec2 | Draw |
| chroma_key_filter | yuv_mat:mat4, color:vec4, contrast, brightness, gamma:f, chroma_key:vec2, pixel_size:vec2, similarity, smoothness, spill:f | Draw |
| chroma_key_filter_v2 | cb_v4:vec4, cr_v4:vec4, opacity, contrast, brightness, gamma, chroma_key:vec2, pixel_size:vec2, similarity, smoothness, spill | Draw |
| color.effect | none (shared include with colour-space functions and matrices) | none |
| color_correction_filter | gamma:f, color_matrix:mat4 | Draw |
| color_grade_filter | clut_1d:tex, clut_3d:tex, clut_amount:f, clut_scale/offset/domain_min/domain_max:vec3 | Draw1D, Draw3D, DrawAlpha3D, DrawAmount3D, DrawDomain3D |
| color_key_filter | color:vec4, contrast, brightness, gamma:f, key_color:vec4, similarity, smoothness:f | Draw |
| color_key_filter_v2 | opacity, contrast, brightness, gamma:f, key_color:vec4, similarity, smoothness:f | Draw |
| crop_filter | mul_val:vec2, add_val:vec2, multiplier:f | Draw, DrawMultiply, DrawTonemap, DrawMultiplyTonemap |
| hdr_tonemap_filter | multiplier, input_maximum_nits, output_maximum_nits:f | Reinhard, MaxRGB, MaxRGBSDR |
| luma_key_filter, luma_key_filter_v2 | lumaMax, lumaMin, lumaMaxSmooth, lumaMinSmooth:f | Draw |
| sdr_on_hdr_filter | multiplier, power:f | GammaSrgb, GammaPower |
| sharpness | sharpness, texture_width, texture_height:f | Draw |

(Parameter order in the real dump begins `ViewProj, image`, then the list above in file order; see the vector for exact order and
defaults.) The technique names are what the C code selects by name (S-028 section 4); a port must keep names byte-identical.

## 7. Reference defects / quirks

- **Q-1** Positive `suppress_level` stored negated by SpeexDSP (R21); not reachable from the UI.
- **Q-2** RNNoise white-noise behaviour (R12): not a hard gate; do not "fix".
- **Q-3** A Speex segment's first output is ~0 and the RNNoise first output is faded: audible start-up dip after every
  timestamp reset (R41). SHOULD be reproduced (a port may use a different library, but tests check the dip).
- **D-1** (S-027 D-4) growth of `rate`/`channels` after the first allocation is out of bounds in the reference; unchanged here.

## 8. Implementation notes

Model the libraries as traits (`Denoiser { process(&mut self, out, in) -> f32 }`, `SpeexPreprocess`, `Resampler`) so the
conformance runner can bind the real crate; the runner in the Rust repo must reproduce the script language of the README
section "filtersrest runner". No concurrency beyond S-027 (audio thread only).

## 9. Coverage

| Script | Rules |
|---|---|
| 10-rnnoise-speech-noise | R10-R13 |
| 11-rnnoise-state | R10, R11 |
| 20-speex-direct | R20-R22, Q-1 |
| 30-filter-rnnoise-48k | R40, R41, R42 |
| 31-filter-speex-48k | R22, R42, R44 |
| 32-filter-rnnoise-resampled | R30 |
| 33-filter-mono-and-channels | R42, R43 |
| 40-module-registration | R01-R03 |
| (S-020 vector 73) | section 6 |

## 10. Open questions

- **O-1** Tolerances were established on one machine (glibc 2.4x, libspeexdsp 1.2.x); other libm builds should stay within them but
  were not tested. Windows/macOS use the same vendored RNNoise (system RNNoise may be used on Linux if found: its output may
  differ by model version; the build then links `Librnnoise` instead of `rnnoise/src`).
- **O-2** NVIDIA AFX noise suppression (`LIBNVAFX_ENABLED`, `nv-filters`) not specified (proprietary SDK).
- The Rust sufficiency check was skipped (cost control, per assignment).
