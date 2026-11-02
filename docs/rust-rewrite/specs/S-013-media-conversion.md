# S-013 `mediaconv` (pixel-format converters, frame layout, YUV matrices, enum helpers, audio maths)

| | |
|---|---|
| Status | Ready for review (the threaded `video_output` / `audio_output` code of `video-io.c` / `audio-io.c`, the resampler, the scaler and `media-remux` are **not** covered) |
| Reference | libobs 33.0.1 `media-io/format-conversion.*`, `video-frame.*`, `video-matrices.c`, `video-fourcc.c`, `audio-math.h`, `frame-rate.h`, `media-io-defs.h`, and the inline helpers / enums of `video-io.h` and `audio-io.h` |
| Target crate | `crates/obs-media` (module `convert`, `frame`, `matrices`, `enums`) |
| Depends on | S-001 (allocator alignment 32, crash behaviour of a zero-size allocation) |
| Conformance | `test/conformance/scripts/mediaconv/` (27 scripts, runner `gen/mediaconv_runner.c`, `gen/mediaconv.sources`) |

## 1. Scope

Everything in `media-io` that is a pure function of its arguments:

- the three CPU **packed 4:4:4 to planar** converters `compress_uyvx_to_i420`, `compress_uyvx_to_nv12`,
  `convert_uyvx_to_i444` and the three **to packed 32-bit** decoders `decompress_420`, `decompress_nv12`,
  `decompress_422` (exported; zero in-tree callers in 33.0.1, kept normative for out-of-tree C plugins);
- plane layout of every `video_format`: `video_frame_init`, `video_frame_copy`, the two non-public helpers
  `video_frame_get_linesizes` / `video_frame_get_plane_heights`, `video_frame_free/create/destroy`;
- the YUV to RGB matrices `video_format_get_parameters` and `video_format_get_parameters_for_format`;
- `video_format_from_fourcc`;
- the inline helpers and every enum of `video-io.h` / `audio-io.h` that plugins compile in (names, flags, channel
  counts, sizes), `audio-math.h` (dB), `audio_frames_to_ns` / `ns_to_audio_frames`, `frame-rate.h`.

Callers in 33.0.1: none in-tree (only stale `#include`s in `obs-video.c`, `obs-source.c`, `video-io.c`); out-of-tree C plugins may still link these — port for S-013/ABI, not for in-tree use.
Numeric enum values and struct layouts cross the plugin ABI and are normative.

## 2. Terminology and conventions

RFC 2119 words apply. Little-endian 64-bit target (the committed vectors). `H(n) = (n + 1) / 2` (integer
division). "Pixel" means one 4-byte UYVX or output word. Integers are unsigned 32-bit unless stated; products
that the reference computes in 32 bits (`width * 4`) wrap modulo 2^32 and a conforming implementation MUST NOT
widen them (vector `04`, width `0x40000000`).

Floating point: matrices and dB maths are 32-bit float. Conformance compares floats with **relative or absolute
tolerance 1e-5** for matrix values, **1e-6** for `mul_to_db` / `db_to_mul` (the results come from the platform libm);
`-0` equals `0`; `nan`, `inf`, `-inf` must match exactly. All byte-valued results are compared exactly.

## 3. Interface

### 3.1 Enumerations (numeric values are exact)

```
video_format   NONE 0, I420 1, NV12 2, YVYU 3, YUY2 4, UYVY 5, RGBA 6, BGRA 7, BGRX 8, Y800 9, I444 10,
               BGR3 11, I422 12, I40A 13, I42A 14, YUVA 15, AYUV 16, I010 17, P010 18, I210 19, I412 20,
               YA2L 21, P216 22, P416 23, V210 24, R10L 25
video_trc      DEFAULT 0, SRGB 1, PQ 2, HLG 3
video_colorspace  DEFAULT 0, 601 1, 709 2, SRGB 3, 2100_PQ 4, 2100_HLG 5
video_range_type  DEFAULT 0, PARTIAL 1, FULL 2
video_scale_type  DEFAULT 0, POINT 1, FAST_BILINEAR 2, BILINEAR 3, BICUBIC 4
audio_format   UNKNOWN 0, U8BIT 1, 16BIT 2, 32BIT 3, FLOAT 4, U8BIT_PLANAR 5, 16BIT_PLANAR 6, 32BIT_PLANAR 7, FLOAT_PLANAR 8
speaker_layout UNKNOWN 0, MONO 1, STEREO 2, 2POINT1 3, 4POINT0 4, 4POINT1 5, 5POINT1 6, 7POINT1 8   (7 is unused)
constants      MAX_AV_PLANES 8, MAX_AUDIO_MIXES 6, MAX_AUDIO_CHANNELS 8, MAX_DEVICE_INPUT_CHANNELS 64,
               AUDIO_OUTPUT_FRAMES 1024, TOTAL_AUDIO_SIZE = 6*8*1024*4 = 196608,
               VIDEO_OUTPUT_SUCCESS 0 / INVALIDPARAM -1 / FAIL -2, AUDIO_OUTPUT_SUCCESS 0 / INVALIDPARAM -1 / FAIL -2
```
All enums are 4 bytes (C `int` underlying type on the reference ABI).

### 3.2 Structs (LP64, sizes/offsets are normative)

| Struct | Fields (offset) | Size |
|---|---|---|
| `video_frame` | `uint8_t *data[8]` (0), `uint32_t linesize[8]` (64) | 96 |
| `video_data` | `data[8]` (0), `linesize[8]` (64), `uint64_t timestamp` (96) | 104 |
| `audio_data` | `uint8_t *data[8]` (0), `uint32_t frames` (64), `uint64_t timestamp` (72) | 80 |
| `audio_output_data` | `float *data[8]` | 64 |
| `video_scale_info` | `format` 0, `width` 4, `height` 8, `range` 12, `colorspace` 16 | 20 |
| `audio_convert_info` | `samples_per_sec` 0, `format` 4, `speakers` 8, `bool allow_clipping` 12 | 16 |
| `media_frames_per_second` | `numerator` 0, `denominator` 4 (both `u32`) | 8 |

### 3.3 Functions

```c
/* format-conversion.h -- all EXPORT */
void compress_uyvx_to_i420(const uint8_t *input, uint32_t in_linesize, uint32_t start_y, uint32_t end_y,
                           uint8_t *output[], const uint32_t out_linesize[]);
void compress_uyvx_to_nv12(same);   void convert_uyvx_to_i444(same);
void decompress_nv12(const uint8_t *const input[], const uint32_t in_linesize[], uint32_t start_y, uint32_t end_y,
                     uint8_t *output, uint32_t out_linesize);
void decompress_420(same);
void decompress_422(const uint8_t *input, uint32_t in_linesize, uint32_t start_y, uint32_t end_y,
                    uint8_t *output, uint32_t out_linesize, bool leading_lum);
/* video-frame.h */
void video_frame_init(struct video_frame *frame, enum video_format format, uint32_t width, uint32_t height);   /* EXPORT */
void video_frame_copy(struct video_frame *dst, const struct video_frame *src, enum video_format format, uint32_t height); /* EXPORT */
/* inline: video_frame_free, video_frame_create, video_frame_destroy */
/* not in a public header but exported symbols of video-frame.c: */
void video_frame_get_linesizes(uint32_t linesize[8], enum video_format format, uint32_t width);   /* needs zeroed array */
void video_frame_get_plane_heights(uint32_t heights[8], enum video_format format, uint32_t height);
/* video-matrices.c, video-fourcc.c */
bool video_format_get_parameters(enum video_colorspace cs, enum video_range_type range, float matrix[16], float min_range[3], float max_range[3]);
bool video_format_get_parameters_for_format(cs, range, enum video_format format, float matrix[16], float min_range[3], float max_range[3]);
enum video_format video_format_from_fourcc(uint32_t fourcc);
```
Inline helpers (plugins compile them; the Rust crate must offer identical behaviour): `format_is_yuv`,
`get_video_format_name`, `get_video_colorspace_name`, `resolve_video_range`, `get_video_range_name`,
`get_audio_channels`, `get_audio_bytes_per_channel`, `is_audio_planar`, `get_audio_planes`, `get_audio_size`,
`get_total_audio_size`, `audio_frames_to_ns`, `ns_to_audio_frames`, `mul_to_db`, `db_to_mul`,
`media_frames_per_second_to_frame_interval`, `..._to_fps`, `..._is_valid`.

All functions are thread-safe in the sense that they have no shared mutable state, except the lazy matrix tables
(section 7).

## 4. Data formats

### 4.1 Pixel layouts used by the converters

- **UYVX** (converter input): 4 bytes per pixel in memory order `U, Y, V, X`; `X` is never read.
- **I420**: plane 0 luma (`width` bytes/row), plane 1 U, plane 2 V (each `H(width)` bytes/row, `H(height)` rows).
- **NV12**: plane 0 luma, plane 1 interleaved `U V U V ...` (`width` bytes/row, `H(height)` rows).
- **I444**: three full planes.
- **Decoder outputs** (4 bytes per pixel, memory order): `decompress_420` gives `V, U, Y, 0`;
  `decompress_nv12` gives `Y, U, V, 0`; `decompress_422` is described in R40. These conventions differ from each other; callers' shaders were written against them (Q-1).

Worked example (vector `10`, 4x2, input row 0 pixels `(U,Y,V,X) = (10,20,30,AA) (11,21,31,AA) (12,22,32,AA) (13,23,33,AA)`
and row 1 `(50,60,70,BB) .. (53,63,73,BB)`): I420 gives Y rows `20212223` / `60616263`, U `3032`, V `5052`
(`U0 = (0x10+0x11+0x50+0x51)>>2 = 48 = 0x30`); NV12 chroma row `30503252`; I444 U rows `10111213` / `50515253`.

### 4.2 Frame allocation

One allocation of the sum of the plane sizes, each plane `linesize*height` bytes **rounded up to a multiple of 32**
(32 is the allocator alignment, S-001), planes stored back to back in order; `data[0]` is 32-byte aligned.
Example (vector `06`): I420 6x5: linesizes `6 3 3`, plane sizes `30->32, 9->32, 9->32`, offsets `0 32 64`.

## 5. Behaviour

### 5.1 Enums and names

- **S-013-R01** Enumerator values and constants are exactly those of section 3.1; struct sizes/offsets are those of 3.2 (vector `01`).
- **S-013-R02** `format_is_yuv` is true for I420, NV12, I422, I210, YVYU, YUY2, UYVY, I444, I412, I40A, I42A, YUVA, YA2L, AYUV, I010, P010, P216, P416, V210 and false for NONE, RGBA, BGRA, BGRX, Y800, BGR3, R10L and every value that is not an enumerator (vector `02`). Note Y800 is **not** YUV.
- **S-013-R03** `get_video_format_name`: the enumerator name (`"I420"`, `"NV12"`, `"I422"`, `"I210"`, `"YVYU"`, `"YUY2"`, `"UYVY"`, `"RGBA"`, `"BGRA"`, `"BGRX"`, `"I444"`, `"I412"`, `"Y800"`, `"BGR3"`, `"I40A"`, `"I42A"`, `"YUVA"`, `"YA2L"`, `"AYUV"`, `"I010"`, `"P010"`, `"P216"`, `"P416"`) except `V210 -> "v210"` and `R10L -> "R10l"`; NONE and every unknown value -> `"None"`.
- **S-013-R04** `get_video_colorspace_name`: DEFAULT and 709 -> `"Rec. 709"`, SRGB -> `"sRGB"`, 601 -> `"Rec. 601"`, 2100_PQ -> `"Rec. 2100 (PQ)"`, 2100_HLG -> `"Rec. 2100 (HLG)"`, any other value -> `"Unknown"`.
- **S-013-R05** `resolve_video_range(format, range)`: if `range == DEFAULT` the result is PARTIAL when `format_is_yuv(format)` else FULL; any other value (including out-of-range numbers) is returned unchanged. `get_video_range_name` = `"Full"` if the resolved range is FULL, otherwise `"Partial"`.
- **S-013-R06** `video_format_from_fourcc(f)`: `f` is the little-endian packing of four characters (first character in the lowest byte). Result UYVY for `UYVY HDYC UYNV UYNY uyv1 2vuy 2Vuy`; YUY2 for `YUY2 Y422 V422 VYUY YUNV yuv2 yuvs`; YVYU for `YVYU`; Y800 for `Y800`; NONE for everything else (case sensitive; vector `03`).

### 5.2 Audio and rate helpers

- **S-013-R07** `get_audio_channels`: MONO 1, STEREO 2, 2POINT1 3, 4POINT0 4, 4POINT1 5, 5POINT1 6, 7POINT1 8; UNKNOWN and every other value (7, 9, 99) 0. `get_audio_bytes_per_channel`: U8BIT(_PLANAR) 1, 16BIT(_PLANAR) 2, 32BIT, FLOAT and their planar versions 4, UNKNOWN and others 0. `is_audio_planar` is true exactly for the four `_PLANAR` formats.
- **S-013-R08** `get_audio_planes(f, s) = planar(f) ? channels(s) : 1`. `get_audio_size(f, s, frames) = (planar(f) ? 1 : channels(s)) * bytes(f) * frames` (bytes of **one plane**), computed in `size_t`; `get_total_audio_size = channels(s) * bytes(f) * frames`. Consequences kept (Q-6): planar with an unknown layout has `planes = 0` but `get_audio_size = bytes*frames != 0`; unknown format gives sizes 0 (vector `40`).
- **S-013-R09** `util_mul_div64(num, mul, div) = (num / div) * mul + ((num % div) * mul) / div` in unsigned 64-bit **wrapping** arithmetic. `audio_frames_to_ns(rate, frames) = mul_div(frames, 1_000_000_000, rate)`; `ns_to_audio_frames(rate, ns) = mul_div(ns, rate, 1_000_000_000)`. Results truncate; no clamping or overflow detection (vector `41`, e.g. rate 1, frames `u64::MAX` wraps modulo 2^64, as printed in the vector). Rate 0 divides by zero in `audio_frames_to_ns` (D-3).
- **S-013-R10** `mul_to_db(m) = (m == 0.0) ? -inf : 20 * log10f(m)` (negative `m` gives NaN, +inf gives +inf). `db_to_mul(db) = isfinite(db) ? powf(10, db / 20) : 0` (so +inf, -inf, NaN all give 0; large finite `db` overflow to +inf). All in `f32`.
- **S-013-R11** `media_frames_per_second_to_frame_interval = (f64)den / (f64)num`, `..._to_fps = (f64)num / (f64)den` (IEEE: 0 denominators give inf/NaN, no trap), `..._is_valid = num != 0 && den != 0`.

### 5.3 Frame layout

- **S-013-R20** `video_frame_get_linesizes(ls, fmt, w)` fills (the caller supplies zeros, unlisted entries are not touched) bytes per row of plane 0,1,2,3:

| Format | Linesizes |
|---|---|
| NONE / unknown | (nothing) |
| BGR3 | `3w` |
| RGBA BGRA BGRX AYUV R10L | `4w` |
| Y800 | `w` |
| YVYU YUY2 UYVY | `2w` |
| I420, I422 | `w, H(w), H(w)` |
| NV12 | `w, w` |
| I444 | `w, w, w` |
| I40A, I42A | `w, H(w), H(w), w` |
| YUVA | `w, w, w, w` |
| I010, I210 | `2w, w, w` |
| P010, P216 | `2w, 2w` |
| I412 | `2w, 2w, 2w` |
| YA2L | `2w, 2w, 2w, 2w` |
| P416 | `2w, 4w` |
| V210 | `((a + 5) / 6) * 16` with `a` from R21 |

  All arithmetic is 32-bit modular (vectors `04`).
- **S-013-R21 (QUIRK Q-2)** V210: `a = (w + 47) & ~47` evaluated on `u32` (`~47 = 0xFFFFFFD0`: this clears bits 0-3 **and bit 5**; it is **not** "round up to a multiple of 48"). Then `linesize[0] = ((a + 5) / 6) * 16`. Examples: `w=1 -> 48`, `w=48 -> 224`, `w=49 -> 176` (a real V210 row of 49 pixels needs 256, so the result is too small), `w=1920 -> 5120`, `w=0 -> 0`. A conforming implementation MUST reproduce the expression exactly; "fixing" it changes plane offsets seen by C plugins.
- **S-013-R22** `video_frame_get_plane_heights(hs, fmt, h)` (heights of plane 0..3):

| Formats | Heights |
|---|---|
| NONE / unknown | (nothing) |
| Y800 YVYU YUY2 UYVY RGBA BGRA BGRX BGR3 AYUV V210 R10L | `h` |
| I420, I010 | `h, H(h), H(h)` |
| NV12, P010 | `h, H(h)` |
| I444, I422, I210, I412 | `h, h, h` |
| I40A | `h, H(h), H(h), h` |
| I42A, YUVA, YA2L | `h, h, h, h` |
| P216, P416 | `h, h` |

- **S-013-R23** `video_frame_init(frame, fmt, w, h)`: if `frame` is NULL do nothing. Otherwise zero the struct, compute linesizes (R20) and heights (R22); the plane list is the planes with non-zero linesize **and** non-zero height. `size = sum over listed planes of align32(linesize_i * height_i)` (64-bit); allocate `size` bytes with the libobs allocator; `data[0]` = block, `linesize[0] = linesizes[0]`; for `i >= 1` with a listed plane: `data[i] = data[0] + sum_{j<i} align32(size_j)` and `linesize[i] = linesizes[i]`. Other `data[]` stay NULL and other `linesize[]` 0. The block is **uninitialised**. Vector `06` prints linesizes and offsets for every format.
- **S-013-R24 (DEFECT D-1)** When `size == 0` (format NONE or unknown, `w == 0`, or `h == 0` for formats whose every plane then has a zero size) the reference calls the allocator with 0 bytes, which aborts the process (S-001). A conforming implementation MUST NOT abort: the Rust API returns an error (`Err`), the C-ABI wrapper leaves `*frame` zeroed (data NULL, linesize 0). The conformance runner prints `result crash` for these inputs (vector `06`, last five lines).
- **S-013-R25** `video_frame_free(f)`: if `f` non-NULL, release `data[0]` and zero the whole struct; NULL is a no-op. `video_frame_create(fmt, w, h)` = zeroed heap `video_frame` followed by `video_frame_init`; `video_frame_destroy` releases `data[0]` and the struct. (vector `08`)
- **S-013-R26** `video_frame_copy(dst, src, fmt, h)`: heights from R22 with `h`; for each plane `i` with height `hs[i] != 0`: if `src.linesize[i] == dst.linesize[i]` copy `src.linesize[i] * hs[i]` bytes (one block, 32-bit product); otherwise for each row `y < hs[i]` copy `min(src.linesize[i], dst.linesize[i])` bytes from `src.data[i] + y*src.linesize[i]` to `dst.data[i] + y*dst.linesize[i]`. Bytes of `dst` rows beyond that count are not touched. The planes copied depend only on `fmt` and `h`, not on what the frames were created for (vector `07`). `h == 0` copies nothing.

### 5.4 Converters (common contract)

Valid calls (outside them the reference faults or corrupts memory, D-2):

- `start_y` and `end_y` are **even**; the loop is over luma row pairs `y = start_y, start_y+2, ... < end_y`. `start_y >= end_y` does nothing.
- For the three `compress`/`convert` functions: `input` is 16-byte aligned, `in_linesize` is a multiple of 16, the **luma output stride `out_linesize[0]` is a multiple of 4** and equals the number of pixels processed per row (R30), the input rows contain at least that many pixels, and every output plane is large enough for the rows addressed.
- The result never depends on CPU features: the reference uses SSE2 (or an exact emulation); a conforming implementation uses plain integer code (vectors are produced on x86-64 SSE2 and must match scalar code).

- **S-013-R30** (processed width, QUIRK Q-5) `width = min(in_linesize, out_linesize[0])` (the first is in **bytes**, the second is used as a pixel count). The inner loop advances `x = 0, 4, 8, ... < width`; one iteration handles pixels `x..x+3` of rows `y` and `y+1`. With `in_linesize = 4*W` (the usual case) `width = out_linesize[0]` unless the output stride is larger than `4*W`. Therefore: output padding in the luma plane is **computed from real input pixels** (the input must contain them) and cropping works by passing a smaller `out_linesize[0]` than `in_linesize/4` (vector `14`). Chroma padding is free (R32/R33).
- **S-013-R31** (luma) For each processed pixel at column `c` of row `r in {y, y+1}`: `out0[r * ols0 + c] = byte1 of the input pixel (r, c)` (a plain copy of Y).
- **S-013-R32** (chroma average) For each pair of columns `(c, c+1)` with `c` even, `k = c / 2`, define for the 2x2 block `{y,y+1} x {c,c+1}`: `Ub = (sum of byte0 of the four pixels) >> 2` and `Vb = (sum of byte2 of the four pixels) >> 2` (**truncating** division by 4, never rounded to nearest; sums reach 1020 so `255*4 -> 255`; vector `11`). Bytes `X` are ignored.
- **S-013-R33** `compress_uyvx_to_i420`: `out1[(y/2) * ols1 + k] = Ub`, `out2[(y/2) * ols2 + k] = Vb`.
- **S-013-R34** `compress_uyvx_to_nv12`: `out1[(y/2) * ols1 + 2k] = Ub`, `out1[(y/2) * ols1 + 2k + 1] = Vb` (so the chroma row of `width` bytes is written at `x` ... `x+3` for each group).
- **S-013-R35** (QUIRK Q-3) `convert_uyvx_to_i444`: no averaging: `out1[r * ols0 + c] = byte0` and `out2[r * ols0 + c] = byte2` for rows `r in {y, y+1}` and every processed column. `out_linesize[1]` and `out_linesize[2]` are **ignored** (the luma stride is used for U and V as well); callers must pass equal strides.
- **S-013-R36** Only the rows `[start_y, end_y)` (and chroma rows `[start_y/2, end_y/2)`) are written; nothing else in the output planes is modified (vector `13`). Slices on disjoint row ranges may run concurrently (that is their purpose).

### 5.5 Decoders (packed 32-bit output)

These have **no alignment requirement**; any byte offset works (vector `24`). Output words are written as native-endian `u32` (little-endian here).

- **S-013-R37** `decompress_420(in[3], ils[3], start_y, end_y, out, ols)`: `pairs = ils[0] / 2` (so `ils[0]` is used both as luma stride and, halved, as the number of chroma samples per row; an odd `ils[0]` ignores the last luma column; the output stride does **not** limit the width). For each chroma row `yc` from `start_y / 2` to `end_y / 2 - 1` (integer divisions: odd `start_y` rounds down, odd `end_y` drops the last incomplete pair; vector `21`), for `x < pairs`: `U = in[1][yc*ils[1] + x]`, `V = in[2][yc*ils[2] + x]`; for `j in {0,1}` and the two luma rows `r in {2yc, 2yc+1}`: pixel `(r, 2x + j)` has luma `in[0][r*ils[0] + 2x + j]` and is written at byte `r*ols + 4*(2x+j)` as the 32-bit value `(Y << 16) | (U << 8) | V`, i.e. memory bytes **`V, U, Y, 0`** (Q-1). Example (vector `20`): Y `01..08`, U `aa bb`, V `cc dd` -> row 0 `ccaa0100 ccaa0200 ddbb0300 ddbb0400`.
- **S-013-R38** `decompress_nv12`: `pairs = min(ils[0], ols) / 2`; for the same chroma rows, `x < pairs`: `uv = u16 little-endian at in[1][yc*ils[1] + 2x]` (U in the low byte, V in the high byte; `ils[2]` unused), pixels as in R37 with luma from `in[0]`, written as `Y | (uv << 8)`: memory bytes **`Y, U, V, 0`**. (`ols` is in bytes so `min` is `ils[0]` in every sane call.) Vector `23`.
- **S-013-R39** `decompress_422(in, ils, start_y, end_y, out, ols, leading_lum)`: `words = min(ils, ols) / 2` (**QUIRK Q-4**: the input *byte stride* `ils` is also used, halved, as the count of 32-bit input words per row. For a real YUY2/UYVY row of `W` pixels (`2W` bytes, `W/2` words) this reads twice the row, so the function is only meaningful for single-row calls or when callers pass `ils` = pixel count with rows that overlap; the behaviour is nevertheless fully defined by the formula and the vectors exercise single-row and overlapping-row calls). For `y in [start_y, end_y)`: input row starts at byte `y*ils`, output row at `y*ols`; for each input word `dw` (little-endian, bytes `b0 b1 b2 b3`) write two words: `out[0] = dw` and
  - `leading_lum`: `out[1] = bytes (b2, b1, b2, b3)` (= `(dw & 0xFFFFFF00) | b2`)
  - otherwise: `out[1] = bytes (b0, b3, b2, b3)` (= `(dw & 0xFFFF00FF) | (b3 << 8)`)
  Output words are consecutive (8 bytes written per input word). So YUY2 `Y0 U Y1 V` becomes `[Y0 U Y1 V] [Y1 U Y1 V]` and UYVY `U Y0 V Y1` becomes `[U Y0 V Y1] [U Y1 V Y1]`. Example (vector `25`): `aabbccdd 11223344`: lead `aabbccdd ccbbccdd 11223344 33223344`, trail `aabbccdd aaddccdd 11223344 11443344`. Nothing is clipped or sign-extended.
- **S-013-R40** Reading/writing beyond what a formula addresses never happens; the decoders write only bytes `[row start, row start + 4 * pixels)` of each processed output row.

### 5.6 YUV to RGB matrices

- **S-013-R50** (selection) `cs` is first remapped: DEFAULT and SRGB -> 709; 2100_HLG -> 2100_PQ. Then the matrix set is chosen from `{601: Kb 0.114 Kr 0.299, 709: Kb 0.0722 Kr 0.2126, 2100_PQ: Kb 0.0593 Kr 0.2627}` (the constants are `f32`). A `cs` that is none of the six enumerators (6, 99, -1) makes the function return `false` and write **nothing**; otherwise it returns `true`.
- **S-013-R51** (range) `full = (range == FULL)`; every other value (DEFAULT, PARTIAL, 7) selects the **partial** set. `DEFAULT` is *not* resolved per format (Q-5b, vector `30`).
- **S-013-R52** (bit depth) `video_format_get_parameters`: `bpc = 10` if the **original** `cs` is 2100_PQ or 2100_HLG, else 8. `..._for_format`: I010 P010 I210 V210 R10L -> 10; I412 YA2L -> 12; P216 P416 -> 16; every other value -> 8, whatever the colour space (so `PQ` with I420 uses the 8-bit table; vector `33`).
- **S-013-R53** (table construction, `f32` arithmetic in this order). For `bpc in 8..=16`: `s = 2^(bpc-8)`, `lo = 16 s`, `hiY = 235 s`, `hiC = 240 s`, `N = 256 s - 1` (all exact in f32), `mid = 0.5 * (lo + hiC)`. Partial set: `mins = (lo,lo,lo)`, `maxs = (hiY,hiC,hiC)`, `black = (lo, mid, mid)`. Full set: `mins = (0,0,0)`, `maxs = (N,N,N)`, `black = (0, mid, mid)`. `build(Kb, Kr)`:
  ```
  yv = maxs0 - mins0;   uv = (maxs1 - mins1) / 2;   vv = (maxs2 - mins2) / 2
  ys = N / yv;  us = N / uv;  vs = N / vv;     Kg = 1 - Kb - Kr
  rowR = ( ys,  0,                          vs * (1 - Kr) )
  rowG = ( ys,  us * (Kb - 1) * Kb / Kg,    vs * (Kr - 1) * Kr / Kg )          (products/divisions left to right)
  rowB = ( ys,  us * (1 - Kb),              0 )
  off  = ( -black0 / N, -black1 / N, -black2 / N )
  t_R = off . rowR ,  t_G = off . rowG ,  t_B = off . rowB          (dot products; the summation order of the reference is SIMD-dependent, results differ by a few ulp, which is inside the section 2 tolerance)
  matrix (row-major 16 floats) = [ rowR.0 rowR.1 rowR.2 t_R,  rowG.0 rowG.1 rowG.2 t_G,  rowB.0 rowB.1 rowB.2 t_B,  0 0 0 1 ]
  ```
  The matrix maps `(Y, U, V, 1)` as normalised samples (`value / N`) to `(R, G, B)`. Example 8-bit 709 partial: row R `1.16438353 0 1.79274106 -0.972945154` (vector `30`).
- **S-013-R54** (ranges) `min_range` / `max_range` outputs: full -> `(0,0,0)` / `(1,1,1)`. Partial -> `min = lo / N` for the three components, `max = (hiY / N, hiC / N, hiC / N)` with `N` of the chosen bit depth (8-bit: `0.0627451017`, `0.921568632 0.941176474 0.941176474`; 10-bit: `64/1023 ...`).
- **S-013-R55** Either `min_range` or `max_range` may be NULL (not written); `matrix` is required.

## 6. Algorithms (pseudocode)

```
compress(kind, in, ils, y0, y1, out[], ols[]):
    width = min(ils, ols[0])
    for y in y0, y0+2, .. while y < y1:
        for x in 0, 4, .. while x < width:
            for r in (y, y+1): for j in 0..3: c = x + j
                p = in[r*ils + 4c ..+4]; out0[r*ols0 + c] = p[1]
                if kind == i444: out1[r*ols0 + c] = p[0]; out2[r*ols0 + c] = p[2]
            if kind != i444: for k in (0,1): c = x + 2k
                u = (p(y,c)[0] + p(y,c+1)[0] + p(y+1,c)[0] + p(y+1,c+1)[0]) >> 2   (same for v with byte 2)
                i420: out1[(y/2)*ols1 + x/2 + k] = u; out2[(y/2)*ols2 + x/2 + k] = v
                nv12: out1[(y/2)*ols1 + x + 2k] = u; out1[(y/2)*ols1 + x + 2k + 1] = v

decompress_420(...):  for yc in y0/2 .. y1/2 (exclusive): for x in 0 .. ils0/2: for dy in 0..1: for dx in 0..1:
    put32(out + (2yc+dy)*ols + 4*(2x+dx), (in0[(2yc+dy)*ils0 + 2x+dx] << 16) | (u[yc*ils1 + x] << 8) | v[yc*ils2 + x])
```

## 7. Concurrency

All functions are reentrant on disjoint buffers. The reference builds the matrix tables lazily on the first call
through unsynchronised statics (D-4): two first calls from different threads race on identical data, in practice
harmless. A Rust implementation SHOULD compute the tables once with `OnceLock` (or at compile time); results are
identical.

## 8. Reference defects and quirks

| ID | Kind | Description | Required behaviour | Script |
|---|---|---|---|---|
| Q-1 | QUIRK | The three decoders emit different byte orders (`V U Y 0`, `Y U V 0`, and R39's 4:2:2 layouts) and the encoders read `U Y V X`. | Reproduce exactly. | `20`, `23`, `25` |
| Q-2 | QUIRK | V210 width "alignment" is `(w+47)&~47` (not a multiple of 48); linesize can be smaller than a real V210 row. | Reproduce the expression (R21). | `04` |
| Q-3 | QUIRK | `convert_uyvx_to_i444` ignores `out_linesize[1]`, `[2]` (uses `[0]`). | Reproduce (R35). | `14` |
| Q-4 | QUIRK | `decompress_422` word count is `min(in_linesize, out_linesize)/2` and the same `in_linesize` is the byte stride. | Reproduce (R39). | `25` |
| Q-5 | QUIRK | `width = min(in_linesize [bytes], out_linesize[0] [pixels])`, rounded up to groups of 4; luma output padding is converted from real input pixels. (5b: `RANGE_DEFAULT` selects the partial matrix for every format, also RGB.) | Reproduce (R30, R51). | `14`, `30` |
| Q-6 | QUIRK | `get_audio_size` for a planar format with an unknown layout is non-zero while `get_audio_planes` is 0. | Reproduce (R08). | `40` |
| Q-7 | QUIRK | `video_format_from_fourcc` maps `VYUY` to YUY2 and several Apple/ffmpeg aliases; case sensitive. | Reproduce the list (R06). | `03` |
| D-1 | DEFECT | `video_frame_init` with a total size of 0 (NONE/unknown format, zero width or height) aborts in the allocator. | Do not abort; return an error / leave the frame zeroed (R24). | `06` |
| D-2 | DEFECT | The `compress`/`convert` functions use aligned 16-byte loads and process groups of 4 pixels: an input pointer not 16-byte aligned faults (SIGSEGV on x86; observed), and `out_linesize[0]` not a multiple of 4 (e.g. 6) overruns the output rows by the rounded-up group width (observed: guard bytes overwritten, rows overlap), an odd `start_y` shifts the pairing (rows `start_y, start_y+1` become one pair, chroma row `start_y/2`). All outside the contract and no vector asserts them. | A Rust implementation MUST accept any alignment and MUST NOT write outside `[0, rows*ols)`; for `ols0` not a multiple of 4 it SHOULD process only `ols0` pixels (the only behaviour that cannot corrupt memory), for odd `start_y`/`end_y` it MAY return an error. | (none; contract only) |
| D-3 | DEFECT | `audio_frames_to_ns(0, n)` divides by zero (SIGFPE on x86). | The Rust API returns `None`/`Err`; the runner prints `result crash`. | `41` |
| D-4 | DEFECT | Lazy, unsynchronised initialisation of the matrix tables. | Compute once with a thread-safe primitive. | (none) |

## 9. Implementation notes (non-normative)

- `#[repr(C)] struct VideoFrame { data: [*mut u8; 8], linesize: [u32; 8] }`; assert `size_of == 96`. Use `#[repr(i32)]` enums with
  `TryFrom<i32>` for safe code; the C-ABI wrappers must accept arbitrary integers (unknown values behave like NONE/UNKNOWN).
- Write converters over safe slices with explicit row/column indices (no raw pointer casts); decoders may use
  `u32::to_le_bytes` stores. Compile with overflow checks off for the few places that rely on 32-bit wrap
  (`width*4`, V210 expression) or use `wrapping_*` explicitly.
- Allocation: `std::alloc::alloc` with `Layout::from_size_align(size, 32)` for `video_frame_init`; the block is freed by `video_frame_free`/`bfree`
  only through the same allocator when exposed over the C ABI (S-001).
- Compute the matrix tables with the same `f32` operation order as R53; `f64` evaluation then rounding changes bits (tolerance still passes, but
  keep `f32` for repeatability).
- The packed-pixel conventions of Q-1 are shader contracts, not accidents to be cleaned up.

## 10. Conformance coverage

| Rule | Scripts |
|---|---|
| R01 | `01-enums-layouts` |
| R02, R03, R04, R05 | `02-format-info` |
| R06 | `03-fourcc` |
| R07, R08 | `40-audio-enums` |
| R09 | `41-audio-time` |
| R10 | `42-audio-db` |
| R11 | `43-frame-rate` |
| R20, R21 | `04-linesizes` |
| R22 | `05-plane-heights` |
| R23, R24 | `06-frame-init` |
| R25 | `08-frame-free` |
| R26 | `07-frame-copy` |
| R30 | `14-compress-strides`, `13-compress-slices` |
| R31, R32, R33, R34 | `10-compress-basic`, `11-compress-rounding`, `12-compress-16x2` |
| R35 | `10`, `14` |
| R36 | `13-compress-slices` |
| R37 | `20-decompress-420`, `21-decompress-420-ranges`, `22-decompress-420-strides`, `24-decompress-alignment` |
| R38 | `23-decompress-nv12`, `24-decompress-alignment` |
| R39 | `25-decompress-422`, `24-decompress-alignment` |
| R40 | `20`–`25` (`guard=intact` on every line) |
| R50, R51 | `30-matrices-8bit`, `33-matrices-edges` |
| R52 | `31-matrices-bit-depth`, `32-matrices-grid`, `33-matrices-edges` |
| R53, R54 | `30`, `31`, `32` |
| R55 | `33-matrices-edges` |

## 11. Open questions

- Odd `start_y` / `end_y`, `out_linesize[0]` not a multiple of 4 and unaligned compressor input are outside the contract (D-2); the
  reference results are not vectors. Whether the Rust side should reject them or process `ols0` pixels is a project decision.
- Whether `decompress_422`'s linesize handling (Q-4) is intended; no in-tree caller exists in libobs 33.0.1 (the symbols remain for plugins).
