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
- Round 10 second-author check (2026-11-05): R51 corrected — the vendored `kiss_fft.c` uses the Opus naming (`opus_fft_c` / `opus_ifft_c`); there is no `kiss_fft_stride` symbol in the 33.0.1 copy. `WINDOW_SIZE = 2*FRAME_SIZE = 960` re-verified.
## 11. Round 9: vendored RNNoise internals (added)

§3.1 pins the public API and the observed behaviour; the eight vendored sources below were not cited. This section
pins the **algorithmic structure** of the vendored implementation — constants, state layout and pipeline order — so a
port (or an equivalent crate) knows what it has to approximate within the §5 tolerances. No code is transcribed;
anything not stated here is library-internal and not observable beyond §5.

### Reference files (all cited)

- `plugins/obs-filters/rnnoise/src/denoise.c`, `plugins/obs-filters/rnnoise/src/kiss_fft.c`, `plugins/obs-filters/rnnoise/src/pitch.c`, `plugins/obs-filters/rnnoise/src/celt_lpc.c`, `plugins/obs-filters/rnnoise/src/rnn.c`, `plugins/obs-filters/rnnoise/src/pitch.h`, `plugins/obs-filters/rnnoise/src/arch.h`, `plugins/obs-filters/rnnoise/src/rnn_reader.c`

### 10.1 Signal path (`denoise.c`)

- **S-054-R45** Constants: `FRAME_SIZE = 480` (120<<2), `WINDOW_SIZE = 960`, `FREQ_SIZE = 481`; pitch: `PITCH_MIN_PERIOD = 60`, `PITCH_MAX_PERIOD = 768`, `PITCH_FRAME_SIZE = 960`, `PITCH_BUF_SIZE = 1728`; features: `NB_BANDS = 22`, `CEPS_MEM = 8`, `NB_DELTA_CEPS = 6`, `NB_FEATURES = 42` (22 + 3·6 + 2). The 22 log-spaced band edges on the 481-bin 48 kHz spectrum (5 ms bands) are the fixed table `eband5ms = {0,1,2,3,4,5,6,7,8,10,12,14,16,20,24,28,34,40,48,60,78,100}`. The analysis pre-filter is a 2nd-order biquad highpass with fixed coefficients `a = {-1.99599f, 0.99600f}`, `b = {-2, 1}`.
- **S-054-R46** State: `struct DenoiseState {analysis_mem[480], cepstral_mem[8][22], memid, synthesis_mem[480], pitch_buf[1728], pitch_enh_buf[1728], last_gain, last_period, mem_hp_x[2], lastg[22], RNNState rnn}` — one per channel (R10), nothing shared. `rnnoise_init` zero-fills the state; `model == NULL` selects the built-in model, otherwise the caller's model; the three GRU state arrays (`vad/noise/denoise`) are `calloc`'d to the model's GRU sizes. `rnnoise_get_size() = sizeof(DenoiseState)`. One process-wide `CommonState {init, kiss_fft_state *kfft, half_window[480], dct_table[22·22]}` is built lazily on first use and shared by all states.
- **S-054-R47** Per-frame pipeline (`rnnoise_process_frame`): (1) biquad highpass (state `mem_hp_x`) over the 480 input samples; (2) analysis: 960-point FFT with 50 % overlap, band energies `Ex`, band correlation against the pitch frame `Ep/Exp`, DCT'd log-band energies (R48) into `features` (the 22 DCT terms, 18 delta-cepstral terms from the `CEPS_MEM`-frame `cepstral_mem` ring, and 2 pitch terms: the estimated period and gain), and the `silence` flag; the pitch ring buffer `pitch_buf` is advanced by `PITCH_FRAME_SIZE` each call, and `pitch_search` (R49) yields `(period, gain)` which are stored (`last_period`, `last_gain`) and fed to the correlation; (3) **only when not silent**: run the RNN (R52) → per-band gains `g[22]` and `vad_prob`; apply the pitch filter (per-bin renormalisation of `X` against the recomputed band energies); smooth per band `g[i] = max(g[i], 0.6 · lastg[i])` (state `lastg` persists across frames); interpolate the 22 band gains to `gf[481]` (`interp_band_gain`) and multiply the spectrum; when silent the spectrum is untouched and `vad_prob = 0` (the state is still updated); (4) synthesis: 960-point inverse FFT, overlap-add with the precomputed `half_window`, 480 output samples. Return `vad_prob`.
- **S-054-R48** DCT: the 22×22 table holds `table[i·22+j] = cos((i+0.5)·j·π/22)` with column 0 multiplied by `sqrt(0.5)`; the forward transform multiplies the sum by `sqrt(2/22)`. (The inverse variant exists in the file but is not used on the inference path.)

### 10.2 Supporting code

- **S-054-R49** `pitch.c` (+ inlined `pitch.h`): `pitch_search` works on the downsampled ring (`pitch_downsample`: 5-tap lowpass `celt_fir5`); `celt_pitch_xcorr` (4-lane unrolled cross-correlation via the `pitch.h` inlines `xcorr_kernel` / `dual_inner_prod`) scores lags in `[PITCH_MIN_PERIOD, PITCH_MAX_PERIOD]`; `find_best_pitch` weights harmonics (and uses the fixed `second_check[16] = {0,0,3,2,3,2,5,2,3,2,3,2,5,2,3,2}` re-check table); around the candidate lag a 4th-order LPC (`_celt_lpc(lpc, ac, 4)` with the autocorrelation from `celt_lpc.c`, applied through `celt_fir`/`celt_iir`) refines the estimate; `compute_pitch_gain` normalises by the energy ratio and clamps.
- **S-054-R50** `celt_lpc.c`: the four LPC routines in the float mapping of R53: `_celt_autocorr` (autocorrelation, scaled by `1-ε`), `_celt_lpc` (Levinson–Durbin, order `p`), `celt_fir` / `celt_iir` (direct-form filters).
- **S-054-R51** `kiss_fft.c`: the FFT core (the vendored copy is the Opus naming: `opus_fft_c` / `opus_ifft_c` over `opus_fft_impl`, mixed-radix, kiss-FFT-derived) used for both the analysis and synthesis transforms (`WINDOW_SIZE = 960` complex); no other signal processing.
- **S-054-R52** `rnn.c`: the network is `input_dense` → GRU `vad_gru` → GRU `noise_gru` → GRU `denoise_gru` → `{denoise_output, vad_output}`. Dense layer: `out[i] = act(Σ_j w[j·stride+i] · in[j])`. GRU: three gates (reset, update, candidate) each with an input- and a recurrent-weight block laid out at offsets `0 / N / 2N`. Weights are stored as `u8`, applied with scale `WEIGHTS_SCALE = 1/256`; activations are `0 = tanh`, `1 = sigmoid`, `2 = relu`, where tanh/sigmoid are evaluated through the 256-entry `tansig_table` (weights pre-scaled into the table) rather than libm (Q-5). The three GRU state arrays persist between frames (the only memory of the network).
- **S-054-R53** `arch.h`: the fixed-point/float type layer. In the float build `celt_sig = opus_val16 = opus_val32 = opus_val64 = float`, `NORM_SCALING = 1`, `QCONST16/32` are identity, `CELT_SIG_SCALE = 32768.f`; helpers `MIN16/MAX16/ABS32`, `SAT16` (saturate to int16), `OPUS_FAST_INT64` on 64-bit platforms; `celt_assert` aborts only with `ENABLE_ASSERTIONS`; `GLOBAL_STACK_SIZE = 120000`.
- **S-054-R54** `rnn_reader.c`: runtime loader for a **user-supplied** model file (the path taken when the host calls the create API with a non-NULL model): allocates the model, then for each layer in order (`input_dense`, `vad_gru`, `noise_gru`, `denoise_gru`, `denoise_output`, `vad_output`) reads the neuron count and the `u8` weight blocks; any format error aborts loading (NULL model). The built-in model (415 kB `rnn_data.c`, weights = data, §1) is what `rnnoise_create(NULL)` uses and is not re-specified.

### 10.3 Quirks (added)

- Q-4: the exact `silence` decision inside the feature computation (energy threshold over the band terms) is not transcribed here; a different threshold changes which frames skip the RNN and is observable through the §5 tolerances. Implementers must not invent one — approximate the reference's decision or keep the vendored code.
- Q-5: GRU tanh/sigmoid are 256-entry table lookups with pre-scaled weights, not libm transcendental functions; bit-exact reproduction (R11 self-consistency, §5) requires the same table (`tansig_table.h` is data).

