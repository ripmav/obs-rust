# S-028 Video filters (obs-filters: GPU filters)

| | |
|---|---|
| Status | Ready for review |
| Spec version | initial |
| Reference | OBS Studio 33.0.1, `plugins/obs-filters`: `color-correction-filter`, `color-grade-filter`, `chroma-key-filter`, `color-key-filter`, `luma-key-filter`, `sharpness-filter`, `scroll-filter`, `crop-filter`, `scale-filter`, `mask-filter`, `gpu-delay`, `hdr-tonemap-filter`, `sdr-on-hdr-filter` and their `data/*.effect` files |
| Target crate | `crates/obs-video-filters` |
| Depends on | S-014 (obs_data), S-015 (obs_properties), S-008 (matrix4/vec), S-020 (effect language: the `.effect` files are compiled by it), S-005 (deque) |
| Conformance | `test/conformance/scripts/videofilters/` (27 scripts; runner described in `test/conformance/README.md`, section "videofilters runner") |

## 1. Scope

The CPU side of the video filters: registration data, settings keys and defaults, property lists and the
property "modified" callbacks, the `update()` derivations that are pushed into effect parameters, and the
per-frame logic that does not touch pixels (sizes, when the effect is skipped, technique / multiplier choice,
parameter-set sequence, `texrender` order of gpu-delay). Pixel maths of the effect files is described in §6 in
pseudocode, with a *non-authoritative* author model (`model-*` commands) to check it.

Out of scope: the audio filters, `async-delay`, the effect compiler (S-020), the real GPU, image decoding
(`gs_image_file`), the frontend. The graphics API and the OBS core are **inputs** (fakes in the conformance
runner); every fake is listed in §3.2.

## 2. Conventions

RFC 2119 words. Floats: `f32` unless said; the reference evaluates `sin`/`cos`/`pow` in `f64` where stated.
Vector output uses `%.9g`. **Comparison tolerance for float values in vectors: relative 1e-6 (absolute 1e-9 near
0)**; integers, names, technique strings and sequences must match exactly. `inf` prints as `inf`. Model lines
(`model ...`) use tolerance 1e-4. Locale: `obs_module_text(K)` = `K`; `obs_module_file(F)` = `@data/F`.
`rgba(u32)`: byte 0 (low) = R, 1 = G, 2 = B, 3 = A, each `/255`; `rgba_srgb` additionally converts R,G,B with the
sRGB EOTF `x<=0.04045 ? x/12.92 : ((x+0.055)/1.055)^2.4` (f32 `powf`); alpha stays linear.
`mul(a,b)` of `matrix4` follows S-008 (`dst = a*b`, row-vector convention, rows x,y,z,t; 16 floats are printed in
row order x.x x.y x.z x.w y.x ... t.w).
Colour spaces `SRGB`, `SRGB_16F`, `709_EXTENDED` ("EXT"), `709_SCRGB` ("SCRGB"). `format(space)` = `RGBA` for SRGB,
`RGBA16F` for the other three. SDR white level `W` = `obs_get_video_sdr_white_level()` (nits).

## 3. Interface

### 3.1 What a filter exposes

Each filter is an `obs_source_info` of type FILTER with: `id`, `version` (absent = 0, printed `v0`), `output_flags`,
and optional callbacks `create(settings, source) -> data|NULL`, `destroy`, `update(data, settings)`,
`get_defaults(settings)`, `get_properties(data)`, `video_tick(data, seconds)`, `video_render(data, effect)`,
`get_width/get_height(data)`, `show(data)`, `video_get_color_space(data, count, preferred[])`, `missing_files(data)`.
Table (verified by vector `01`; `VIDEO` = 0x1, `SRGB` = 0x8000, `OBSOLETE` = 0x400):

| id | ver | flags | tick | size | show | colorspace | missing_files | effect file(s) |
|---|---|---|---|---|---|---|---|---|
| color_filter | 0 | VIDEO\|OBSOLETE | | | | | | color_correction_filter |
| color_filter | 2 | VIDEO\|SRGB | | | | x | | color_correction_filter |
| clut_filter | 0 | VIDEO\|SRGB | | | | x | x | color_grade_filter |
| chroma_key_filter | 0 / 2 | OBS / SRGB | | | | v2 | | chroma_key_filter / _v2 |
| color_key_filter | 0 / 2 | OBS / SRGB | | | | v2 | | color_key_filter / _v2 |
| luma_key_filter | 0 / 2 | OBS / SRGB | | | | v2 | | luma_key_filter / _v2 |
| sharpness_filter | 0 / 2 | OBS / SRGB | | | | v2 | | sharpness |
| scroll_filter | 0 | VIDEO\|SRGB | x | x | x | x | | crop_filter |
| crop_filter | 0 | VIDEO\|SRGB | x | x | | x | | crop_filter |
| scale_filter | 0 | VIDEO\|SRGB | x | x | | x | | base effects of the core |
| mask_filter | 0 / 2 | OBS / SRGB | x | | | v2 | x | mask_color/mask_alpha/blend_{mul,add,sub}_filter (setting `type`) |
| gpu_delay | 0 | VIDEO | x | | | x | | core `default` |
| hdr_tonemap_filter | 0 | VIDEO\|SRGB | | | | x | | hdr_tonemap_filter |
| sdr_on_hdr_filter | 0 | VIDEO\|SRGB | | | | x | | sdr_on_hdr_filter |

(`OBS` = VIDEO\|OBSOLETE.) Note: scroll_filter loads `crop_filter.effect`.

### 3.2 Inputs (fakes of the OBS core and the graphics API)

A conforming implementation takes these from its own host; the runner models them as follows (all are script knobs).

* **Settings object.** For a video filter, `obs_source_update(src, settings)` merges `settings` into the source's own
  settings and only **increments a deferred counter**; the filter's `update()` runs at the start of the next tick
  (`tick`: if counter > 0 and data exists: `update(data, own_settings)`, counter = 0; then `video_tick`).
  `create` receives the source's own settings, which already contain the filter's defaults merged with the creation settings.
  Filters that call `update` themselves inside `create` (and therefore are up to date before the first tick):
  color_filter v0/v2, chroma/color/luma key, sharpness, scale. Filters that call `obs_source_update` in `create`
  (first update happens at the first tick): clut, mask, scroll, crop, gpu_delay, hdr_tonemap, sdr_on_hdr.
* **Sources.** `filter.target` and `filter.parent` may be NULL, equal or different. A source has `base_size` and
  `size` (distinct accessors), a colour space (`obs_source_get_color_space(target, ...)` returns it, or SRGB for NULL),
  and output flags (`custom_draw`, `async`).
* **Video info:** base width/height, fps num/den; `W`; the current graphics colour space `gs_get_color_space()`;
  `obs_get_video_frame_time()` (ns).
* **`obs_source_process_filter_begin[_with_color_space](filter, format, [space,] allow)`** returns a scripted bool
  (default true) and is recorded. `process_filter_end/tech_end(filter, effect, w, h[, technique])` are recorded.
* **Effects.** `gs_effect_create_from_file` may fail (knob). Parameter existence and technique existence are checked against
  the uniform/technique lists of the real effect file (§6.0 lists them); a missing parameter yields NULL and the
  following `set_*` is a no-op in the reference. `obs_get_base_effect(type)`: effects `default`, `bicubic_scale`,
  `lanczos_scale`, `bilinear_lowres_scale`, `area`.
* **texrender (reference semantics, MUST be modelled by an implementation's graphics layer):** `create(format)` has no
  target; `begin(cx,cy,space)` fails if already `rendered` or `cx==0||cy==0`, (re)creates the target texture when the size
  differs from the last begin, returns false if there is no target; `end` sets `rendered`; `reset` clears `rendered`;
  `get_texture` returns the target (NULL before the first successful begin; it survives `reset`).
* **Images.** `gs_image_file_ex_init(path)` fills `loaded, cx, cy, format, is_animated_gif, texture_data`; the vectors use
  pixel `n` = bytes `(n, n>>8, n>>16, 255)` so that tile permutations are visible. `os_stat` mtime and `os_file_exists` are real files.
* **Print rules of the runner** (non-normative): graphics enter/leave are not printed; `samplerstate_destroy`,
  `effect_destroy`, `voltexture_destroy` of NULL print nothing (destroying NULL is a no-op).

Inline library code the filters compile in, and which an implementation must reproduce: `vec4_from_rgba[_srgb]` (§2),
`gs_srgb_nonlinear_to_linear` (§2), `half_from_float` (IEEE binary16, DirectXMath algorithm: round-to-nearest-even on
the mantissa, denormals for |x|<2^-14, saturate to inf above 65504 (`0x477FE000` in f32 bits), NaN -> 0x7FFF),
`gs_get_format_bpp`, `gs_get_format_from_space`, `util_mul_div64(n,m,d) = (n/d)*m + ((n%d)*m)/d` (u64; division by
zero traps), `matrix4_mul`, `vec3/vec2` helpers.

## 4. Data formats

### 4.1 `.cube` LUT file (clut_filter)

Line oriented text, lines read in chunks of at most 255 bytes. In the *header phase* each line is tested in this order:
(1) `sscanf("%f %f %f")` == 3 -> first data line, header phase ends (so a header keyword whose rest looks like three floats
is never confused: keywords start with letters); (2) `DOMAIN_MIN %f %f %f`; (3) `DOMAIN_MAX %f %f %f`;
(4) `LUT_1D_SIZE %u`; (5) `LUT_3D_SIZE %u`. All other lines (TITLE, comments) are ignored. Data lines are then read with the
same `%f %f %f` test (non-matching lines are skipped; tabs and exponent notation are accepted by `sscanf`).
Worked example (vector `20`): `LUT_3D_SIZE 2` followed by the eight corner lines `0 0 0`, `1 0 0`, `0 1 0`, `1 1 0`, `0 0 1`, `1 0 1`, `0 1 1`, `1 1 1`
yields the 64 bytes `000000000000003c 003c00000000003c 0000003c0000003c 003c003c0000003c 00000000003c003c 003c0000003c003c 0000003c003c003c 003c003c003c003c`
(RGBA16F little endian texels; red varies fastest).

Texel value: `half(srgb_nonlinear_to_linear(v))` for R,G,B; A = `0x3c00` (1.0). Entry order = file order; 3D: `x` fastest, then `y`, then `z`.

### 4.2 PNG LUT (clut_filter)

A decoded image `cx x cy` is a 64^3 volume iff `cx%64==0 && cy%64==0 && cx*cy == 262144` and `bpp%8==0`. Tile grid
`mw = cx/64`, `mh = cy/64`. Volume voxel `(x,y,z)` (x fastest) is the pixel `(tx + x, ty + y)` with the tile origin
`tx = (z % mw) * 64`, `ty = (z / mh) * 64` (**the reference divides by `mh`, not by `mw`**, see Q-4/D-2).

## 5. Behaviour (numbered rules)

### 5.1 Lifecycle and host contract

* **S-028-R01** `get_defaults` fills the defaults of §5.2 in exactly the listed order (iteration order of the object is observable, S-014 §6.1). (`01`)
* **S-028-R02** Video filters apply updates lazily (§3.2). A filter whose `update` is deferred has no loaded state before the first tick: it renders as "skip". (`16`, `18`, `10`, `12`)
* **S-028-R03** `create` returns NULL (and releases everything) when the filter's effect cannot be created, for: color_filter, chroma/color/luma key, sharpness, crop, scroll, hdr_tonemap, sdr_on_hdr. clut, mask, scale and gpu_delay always return a filter; clut/mask load their effect in `update` and render as "skip" without it. (`09`, `16`)
* **S-028-R04** `destroy` releases effect, samplers, textures, images; `gpu_delay.destroy` destroys every queued texrender in queue order. (`22`)
* **S-028-R05** "Skip" = `obs_source_skip_video_filter(filter)` and no other call. A render where `process_filter_begin*` returns false performs only the begin call (no parameter sets, no blend push, no end). (`04`, `16`, `15`)

### 5.2 Settings and defaults (key: type = default)

| filter | defaults |
|---|---|
| color_filter v0 | gamma f64 0.0, contrast 0.0, brightness 0.0, saturation 0.0, hue_shift 0.0, opacity **int** 100, color int 0x00FFFFFF |
| color_filter v2 | gamma, contrast, brightness, saturation, hue_shift 0.0, opacity **f64** 1.0, color_multiply int 0x00FFFFFF, color_add int 0 |
| clut_filter | clut_amount f64 1.0, passthrough_alpha bool false (image_path has no default) |
| chroma_key v0 | opacity int 100, contrast 0.0, brightness 0.0, gamma 0.0, key_color int 0x00FF00, key_color_type "green", similarity int 400, smoothness int 80, spill int 100 |
| chroma_key v2 | as v0 but opacity f64 1.0 |
| color_key v0 / v2 | as chroma (opacity int 100 / f64 1.0, contrast, brightness, gamma 0.0, key_color 0x00FF00, key_color_type "green"), similarity int **80**, smoothness int **50**; no spill |
| luma_key | luma_max 1.0, luma_min 0.0, luma_max_smooth 0.0, luma_min_smooth 0.0 |
| sharpness | sharpness f64 0.08 |
| scroll | `limit_size` bool false (never read: Q-6), cx int 100, cy int 100, loop bool true. speed_x/y, limit_cx/cy have **no** defaults (read as 0 / false) |
| crop | relative bool true (all others read as 0) |
| scale | sampling "bicubic", resolution "None", undistort bool false |
| mask v0 | type "mask_color_filter.effect", color int 0xFFFFFF, opacity int 100 |
| mask v2 | same, opacity f64 1.0 |
| gpu_delay | none (delay_ms reads 0) |
| hdr_tonemap | transform int 0, sdr_white_level_nits 300, hdr_input_maximum_nits 4000, hdr_output_maximum_nits 1000, sdr_input_maximum_nits 1000, sdr_output_maximum_nits 300 |
| sdr_on_hdr | gamma int 1, sdr_white_level_nits int 300, power f64 2.4 |

(`01` prints each table with `default` markers.)

### 5.3 Property lists

* **S-028-R10** Property lists, in order, with type and parameters, are exactly those printed by vector `02` (descriptions are locale keys). Differences v0 -> v2 that matter: color_filter contrast range +-2 -> +-4, brightness step 0.01 -> 0.0001, opacity int slider 0..100 -> float slider 0..1 step 0.0001, `color_alpha` -> two `color`; the info text `sdr_only_info` (type text, info) is the first property of color_filter v2, clut, chroma key v2, color key v2, luma key (both versions), sharpness (both versions share one list) and mask (both versions); the v0 chroma/color key lists do not have it. (`02`)
* **S-028-R11** A new property is visible and enabled. Modified callbacks only run on `obs_property_modified` / `apply_settings` (S-015-R19, reverse insertion order) -- so before any callback runs *every* conditional field is visible (scroll cx/cy, crop right/bottom/cx/cy, scale undistort, hdr / sdr_on_hdr fields). (`02`, `03`)
* **S-028-R12** Callbacks (all return true = "refresh"):
  * chroma/color key `key_color_type`: `key_color` visible iff the string equals `"custom"` (exact, case-sensitive; unknown strings hide it).
  * crop `relative`: relative -> descriptions of `left`/`top` = `Crop.Left`/`Crop.Top` (keys), else `"X"`/`"Y"` (literal); `right`,`bottom` visible iff relative; `cx`,`cy` visible iff not relative.
  * scroll `limit_cx`/`limit_cy`: `cx` / `cy` visible iff the bool is true.
  * scale `sampling`: `undistort` visible iff the string equals (ASCII case-insensitive) `lanczos` or `bicubic`, **or is unknown** (unknown strings are treated as bicubic).
  * hdr_tonemap `transform` (int): `sdr_white_level_nits` visible iff 0; `hdr_input/output_maximum_nits` iff 1; `sdr_input/output_maximum_nits` iff 2; out of range hides all five.
  * sdr_on_hdr `gamma`: `power` visible iff gamma == 2.
  (`03`)
* **S-028-R13** Property numeric limits as in `02`; color_grade `image_path` is a file path property with filter string `PNG/Cube (*.cube *.png)` and default path `@data/LUTs/` (the LUT dir with a trailing slash after backslash replacement); mask `image_path` filter is `BrowsePath.Images (*.bmp *.jpg *.jpeg *.tga *.gif *.png);;BrowsePath.AllFiles (*.*)`, no default path. scale `resolution` is an editable list of `None`, `Base.Canvas`, `16:9`, `16:10`, `4:3`, `1:1` followed by `WxH` for the 11 downscale divisors `1, 1.25, 1/0.75, 1.5, 1/0.6, 1.75, 2, 2.25, 2.5, 2.75, 3` of the video-info base size, each `(int)(base / divisor)` as `%dx%d`. (`02`)

### 5.4 color_filter (colour correction)

* **S-028-R20** `update` (v0 and v2, called in `create`) derives the matrices of §6.1 and stores `gamma`. Gamma: `g<0 ? -g+1 : 1/(g+1)` in f64, stored as f32. (`04`,`05`)
* **S-028-R21** Render v0: `begin(RGBA, ALLOW_DIRECT)`; if true: `set_float gamma`, `set_matrix4 color_matrix`, `blend_push`, `blend_function(ONE, INVSRCALPHA)`, `end(0,0)`, `blend_pop`. (`04`)
* **S-028-R22** Render v2: `source_space = target space`. EXT -> skip. Otherwise `begin_with_color_space(format(source_space), source_space, ALLOW_DIRECT)` and the same sequence as v0 (for SCRGB sources the same path with RGBA16F is taken). (`05`)
* **S-028-R23** v2 `video_get_color_space` returns the target's space (preferred list ignored). (`05`)

### 5.5 Keys and luma key

* **S-028-R30** chroma key `update`: `similarity/smoothness/spill = value/1000` as f32 (`(float)int / 1000.0f`). Key colour: `key_color_type` "green" -> 0x00FF00, "blue" -> 0xFF9900, "magenta" -> 0xFF00FF, anything else (including "custom") -> `key_color`; rgb = `rgba(key | 0xFF000000)`; `chroma = (dot(rgb_with_alpha1, cb_vec), dot(.., cr_vec))` with `cb_vec = (-0.100644, -0.338572, 0.439216, 0.501961)`, `cr_vec = (0.439216, -0.398942, -0.040274, 0.501961)` -- the 4-component dot, so the fourth term is `1.0 * 0.501961` (alpha is 1). (`06`)
* **S-028-R31** colour part, v2: `opacity = (f32)opacity`; `contrast = c<0 ? 1/(-c+1) : c+1` (f64, then f32); `brightness = (f32)b` unchanged; `gamma = g<0 ? -g+1 : 1/(g+1)`. v0: `opacity` is an int percent: `color = 0xFFFFFF | (((u32)opacity * 255 / 100) << 24)` as u32 arithmetic (QUIRK Q-1: values >100 or negative wrap the top byte), pushed as `vec4 color = rgba(color)`; `brightness *= 0.5` (v0 only); contrast and gamma as v2. (`06`)
* **S-028-R32** chroma render v2: base size of the **target** `(w,h)` (`obs_source_get_base_width/height`), `pixel_size = (1/(f32)w, 1/(f32)h)` (inf for 0). After `begin_with_color_space` (EXT -> skip; same colour-space rule as R22) the sets are, in order: `opacity, contrast, brightness, gamma` (floats), `chroma_key` (vec2), `pixel_size` (vec2), `similarity, smoothness, spill`; then blend push, `ONE/INVSRCALPHA`, `end(0,0)`, pop. v0: `begin(RGBA, ALLOW_DIRECT)`, sets `color` (vec4), `contrast, brightness, gamma, chroma_key, pixel_size, similarity, smoothness, spill`, `end(0,0)`, **no** blend change; base size is read before `begin`. (`06`)
* **S-028-R33** color key: presets "green" 0x00FF00, "blue" 0xFF0000, "red" 0x0000FF, "magenta" 0xFF00FF (low byte = red), else `key_color`; `key_color = rgba(preset | 0xFF000000)` (vec4); `similarity/smoothness = value/1000`. Colour part as R31. Render v0: sets `color, contrast, brightness, gamma, key_color, similarity, smoothness`, no blend change. v2: sets `opacity, contrast, brightness, gamma, key_color, similarity, smoothness` with blend push/pop. (`07`)
* **S-028-R34** luma key: the four settings are stored as f32; render (both versions, colour-space path of R22): sets `lumaMax, lumaMin, lumaMaxSmooth, lumaMinSmooth`; v2 (premultiplied) wraps `end` in blend push `ONE/INVSRCALPHA` and pop; v0 does not. v0 is **not** exempt from colour-space handling (uses `begin_with_color_space`, EXT -> skip). (`08`)
* **S-028-R35** For the v2 filters `video_get_color_space` returns the target's space (luma, chroma, color key, sharpness). (`08`,`09`)

### 5.6 Sharpness

* **S-028-R36** `update`: `sharpness = (f32)`. Render (both versions): EXT -> skip; else `begin_with_color_space(..., ALLOW_DIRECT)`; `texture_width/height = (f32)obs_source_get_width/height(target)` (**size, not base size**), sets `sharpness, texture_width, texture_height`, blend push `ONE/INVSRCALPHA`, `end(0,0)`, pop. (`09`)

### 5.7 Crop

* **S-028-R40** `update`: `absolute = !relative`; `left, top, right, bottom, abs_cx, abs_cy` = settings truncated from i64 to `i32` (wrap). (`10`)
* **S-028-R41** `tick`: `mul=add=(0,0)`; if no target return (width/height keep their last values, 0 initially). `(w,h)` = target base size. Relative: `width = w - left - right`, `height = h - top - bottom` (i32); absolute: `width=abs_cx`, `height=abs_cy`. `width<1 -> 1`, `height<1 -> 1`. If `w != 0`: `mul.x = (f32)width/(f32)w`, `add.x = (f32)left/(f32)w`; if `h != 0` likewise for y with `top`. (`10`)
* **S-028-R42** `get_width/get_height` = last computed `(u32)width/height`. (`10`)
* **S-028-R43** render: `source_space = target space`; `(technique, multiplier) = tech_multiplier(current_space, source_space)` (§6.2); `begin_with_color_space(format(source_space), source_space, NO_DIRECT)`; sets `mul_val`, `add_val`, `multiplier`; blend push `ONE/INVSRCALPHA`; `tech_end(width, height, technique)`; pop. No EXT skip. (`10`, `11`)
* **S-028-R44** `video_get_color_space(count, prefs)`: `s = target space`; `r = s`; for each `p` in prefs: `r = p; if p == s break`; return `r` (so the **last** preference when none matches; `s` when `count == 0`). Same rule for scroll, scale. (`11`)

### 5.8 Scroll

* **S-028-R45** `update`: `limit_cx/limit_cy` bools, `cx/cy` = `(u32)i64`, `speed = (f32)` both axes, `loop` bool; destroys and re-creates the sampler `{filter LINEAR, address_u = address_v = loop ? WRAP : BORDER, address_w CLAMP, anisotropy 0, border colour 0}`; if `speed.x == 0` then `offset.x = 0`, same for y. (`12`)
* **S-028-R46** `tick(s)`: `offset += size_i * speed * s` per axis (f32); if loop: `if offset > 1 { offset -= 1 }` (at most once, never below 0); else `if offset > 1 { offset = 1 }`. `size_i` is set only by `render` (initially 0, so the first ticks add nothing). `show()` zeroes the offset. (`12`)
* **S-028-R47** `get_width/height` = `limit ? cx : target base size` (0 for a missing target). render: `(bw,bh)` = target base; `cx = limit_cx ? cx : bw`, `cy` likewise; if `bw == 0 || bh == 0`: `size_i = 0`, skip. Else `size_i = (1/bw, 1/bh)`, `mul = ((f32)cx/(f32)bw, (f32)cy/(f32)bh)`; technique/multiplier as R43; `begin_with_color_space(..., NO_DIRECT)`; sets `add_val = offset`, `mul_val`, `multiplier`, then `set_next_sampler(image, sampler)`, blend push, `tech_end(cx, cy, technique)`, pop. (`12`)

### 5.9 Scale

* **S-028-R50** `update`: `valid = true`; resolution string `R`: if `R == "Base.Canvas"` (the translation of the key; identity in vectors): `aspect_only = false`, `base_canvas = true`, `cx_in,cy_in` = video-info base size. Else `sscanf(R, "%dx%d")` (leading whitespace allowed, trailing text ignored): 2 matches -> sizes; else `sscanf(R, "%d:%d")` 2 matches -> `aspect_only = true`; else `valid = false` and return (sampling, undistort are not updated). `sampling`: ASCII case-insensitive `point`, `bilinear`, `lanczos`, `area`, anything else -> bicubic. `can_undistort = undistort`. (`13`)
* **S-028-R51** `tick` (every frame): if `base_canvas` re-read `cx_in,cy_in`. `cx_out = cy_out = 0`; `target_valid = target != NULL`; if no target return; `(cx,cy)` = target base size as **i32**; if either 0: `target_valid = false`, return (outputs 0). Else `cx_out,cy_out = cx,cy`; if `!valid` return. `old = cx/cy`, `new = cx_in/cy_in` (f64). Aspect-only: if `|old-new| <= EPSILON (1e-4)`: `target_valid = false` return; else if `new > old`: `cx_out = (int)(cy*new)` else `cy_out = (int)(cx/new)`. Otherwise `cx_out,cy_out = cx_in,cy_in`. `dimension = (cx,cy)`, `dimension_i = (1/cx, 1/cy)` (f32), `undistort = upscale = false`. `lower = cx_out < cx/2 || cy_out < cy/2` (integer halves). If `lower && sampling != point` effect = `bilinear_lowres_scale`; else by sampling: point/bilinear -> `default`; bicubic -> `bicubic_scale`, `undistort = can_undistort`; lanczos -> `lanczos_scale`, `undistort = can_undistort`; area -> `area`, `upscale = (cx_out >= cx && cy_out >= cy)`. `undistort_factor = undistort ? new/old : 1.0` (f64; note `new` is `cx_in/cy_in` also for non-aspect mode). Parameters looked up on the chosen effect: `image`; `base_dimension`, `base_dimension_i` unless effect is `default`; `undistort_factor` only for bicubic/lanczos (not for the lowres/area effect even when `undistort` is true... since the effect isn't bicubic/lanczos); `multiplier`. Missing parameters (the lowres effect has no `base_dimension*`) stay NULL and are skipped. (`13`, `14`, `15`)
* **S-028-R52** `get_width/height = (u32)cx_out/cy_out` (negative inputs wrap). (`13`)
* **S-028-R53** render: `!valid || !target_valid` -> skip. Else `source_space = target space`, `(technique, multiplier)` by §6.3 (undistort / upscale variants), `begin_with_color_space(format, space, NO_DIRECT)`; then (each only if its parameter exists, in this order) `base_dimension` (vec2 dimension), `base_dimension_i`, `undistort_factor` (f32), `multiplier`, and for point sampling `set_next_sampler(image, point_sampler)` (sampler created in `create` with all-zero info: POINT, CLAMP); blend push, `tech_end(cx_out, cy_out, technique)`, pop. (`15`)

### 5.10 Mask

* **S-028-R55** `update` (v0: `opacity = (f32)(int * 0.01)` (the product is in f64); v2: `(f32)f64`): `image_file = copy(image_path)`; colour = `rgba(color)` (v0) / `rgba_srgb(color)` (v2) with `w = opacity`; reload image (R56); `lock_aspect = !stretch`; effect file = `type` setting (joined to the data dir as given), previous effect destroyed, new effect created (may be NULL). (`16`)
* **S-028-R56** image (re)load: unload (`gs_image_file_ex_free`); if path non-empty: `timestamp = mtime(path)` (-1 when stat fails), init image, `update_time_elapsed = 0`, create texture. `target = image.texture` (NULL if empty path or unloaded). (`16`, `17`)
* **S-028-R57** `tick(s)`: `elapsed += s`; if `elapsed >= 1.0`: `elapsed = 0`, `t = mtime(image_file)`; if `t != stored timestamp` reload (R56). Animated images: `cur = frame_time`; `last = last ? last : cur`; `image_tick(cur - last)` (ns), `update_texture`, `last = cur`. (`17`)
* **S-028-R58** render: skip if no target (filter target), no mask texture or no effect; EXT target space -> skip. Locked aspect: `src = target base size`, `mask = texture size`; `size_to_x = src.x/src.y < mask.x/mask.y`; `fix = size_to_x ? src.x/mask.x : src.y/mask.y`; `mask *= fix`; `mul = src/mask`; `add = -((src*0.5) - (mask*0.5)) / mask` (component-wise). Stretch: `mul=(1,1)`, `add=(0,0)`. `begin_with_color_space(ALLOW_DIRECT)`; sets `target` (texture, sRGB), `color` (vec4), `mul_val`, `add_val`; blend push `ONE/INVSRCALPHA`; `end(0,0)`; pop. (`16`)
* **S-028-R59** `missing_files`: returns a list; if `image_file` is non-empty and `os_file_exists` is false a missing-file entry for it is added; the resolve callback (called with the filter data) sets `image_path` in the source's settings and calls `obs_source_update(source, settings)`. clut identical with its `file`. (`16`)

### 5.11 Colour grade (clut_filter)

* **S-028-R60** `update`: free `file`, cube data, image, 3D texture; `path` empty -> NULL (no LUT: technique stays `Draw3D`, no texture, render skips). With a path: `domain_min = 0`, `domain_max = 1`; if the extension (`os_get_path_extension`, text from the last `.` unless a `/` or `\` comes first) equals `.cube` ASCII case-insensitively -> load cube (R61) else `image_init(path)` and `cube_width = 64`. Technique: 1D LUT -> `Draw1D`, texture `clut_1d`; else if any `domain_min > 0` or any `domain_max < 1` -> `DrawDomain3D`; else if `clut_amount < 1.0` -> `DrawAmount3D`; else if `!passthrough_alpha` -> `DrawAlpha3D`; else `Draw3D`. The effect is re-created at every update. (`18`, `20`)
* **S-028-R61** `.cube` loading (§4.1): the result is `NULL` when the file cannot be opened, the domain is invalid (`min >= max` on any axis -> log WARNING), no data line exists, both sizes are 0, or there are fewer data lines than entries. `LUT_1D_SIZE > 0` wins over 3D. For size `N`: 3D texels `N^3`, 1D `N`. 1D: `gs_texture_create(N, 1, RGBA16F)`; 3D: `gs_voltexture_create(N,N,N,RGBA16F)`. Scale and offset: `domain_scale = max - min`; `scale = (N-1)/domain_scale`; `offset = -min * scale`; then `scale /= N`; `offset = (offset + 0.5) / N` (vec3, f32). (`20`)
* **S-028-R62** PNG: if the image loaded: `target = make_png_volume(image)` (R63), `scale = 1 - 1/64` (all axes), `offset = 0.5/64`; domain stays unit. (`18`)
* **S-028-R63** `make_png_volume`: NULL if `cx%64 || cy%64 || cx*cy != 262144 || bpp%8`; otherwise the permutation of §4.2 into a 64^3 volume of the same format. (`18`)
* **S-028-R64** render: skip if no target, no LUT texture or no effect; EXT -> skip; else `begin_with_color_space(format, space, ALLOW_DIRECT)`; sets: `clut_1d|clut_3d` texture (sRGB), `clut_amount`, `clut_scale`, `clut_offset`, `domain_min`, `domain_max`; blend push `ONE/INVSRCALPHA`; `tech_end(0,0,technique)`; pop. (`18`,`20`)

### 5.12 gpu_delay

* **S-028-R70** `update`: `delay_ns = delay_ms * 1_000_000` (u64 from i64); full reset: `cx = cy = 0`, `interval = 0`, free all frames. (`22`)
* **S-028-R71** `tick`: `processed = false`; `check_size`: `target_valid = target != NULL`; if not, return "handled". `(w,h)` = target base; `target_valid = w != 0 && h != 0`; if not, handled. If `(w,h) != (cx,cy)`: store, `reset_textures` (interval = 0, free frames, `check_interval`), handled. If handled the tick ends; else `check_interval`. `check_interval`: `interval = util_mul_div64(fps_den, 1e9, fps_num)`; if it differs from the stored one: `update_interval(interval)`: if `!target_valid` free all frames (note: the interval is *not* stored); else store the interval, `n = delay_ns / interval`; if `n > frames` create `n - frames` more `texrender(RGBA, ZS_NONE)` (appended at the back; created in index order); if `n < frames` pop and destroy from the **front** until `n`. (`21`, `22`)
* **S-028-R72** The queue is a FIFO of `{texrender, space, ts}`; newly appended entries are zero-initialised (`space = SRGB`).
* **S-028-R73** render: skip if `!target_valid || !target || !parent || queue empty`. If `processed`: `draw_frame` only. Else: pop the front frame; `space = target space (prefs SRGB,SRGB_16F,EXT)`, `format(space)`; if the texrender's format differs: destroy it and create a new one; `texrender_reset`; blend push `ONE/ZERO`; `begin_with_color_space(cx, cy, space)`; if true: `clear(COLOR, (0,0,0,0), 0, 0)`, `ortho(0,cx,0,cy,-100,100)`, render the target (`default_render` iff `target == parent && !custom_draw && !async` else `video_render`), `texrender_end`, `frame.space = space` (otherwise the old `space` is kept); blend pop; push the frame to the **back**; `draw_frame` (of the new **front**); `processed = true`. (`21`, `22`)
* **S-028-R74** `draw_frame`: front frame, `(tech, mult)` = tech_multiplier(current, frame.space) (§6.2); `tex = get_texture(front)`; if non-NULL: `prev = framebuffer_srgb_enabled`, enable srgb, set `image` (texture, sRGB) and `multiplier` on the core `default` effect, loop `effect_loop(tech)` calling `draw_sprite(tex, 0, cx, cy)` per pass, restore `prev`. NULL texture (frame not rendered yet) draws nothing. Consequently the output lags by exactly `n` render ticks. (`21`)
* **S-028-R75** `video_get_color_space(count, prefs)`: if the queue is empty / invalid / no target / no parent: `count > 0 ? prefs[0] : SRGB`; else `frame = front`, `r = frame.space`, the scan of R44 over `prefs`. (`21`, `22`)

### 5.13 hdr_tonemap and sdr_on_hdr

* **S-028-R80** hdr `update`: `transform = (enum)int`; `sdr_white_level_nits_i = 1.0f / (f32)int` (inf for 0); the four limits `(f32)int`. (`23`)
* **S-028-R81** hdr render: `source_space = target space`; only EXT and SCRGB are processed (SRGB, SRGB_16F -> skip). `begin_with_color_space(format(space), space, NO_DIRECT)`; `m = (space == EXT) ? W : 80`; `m *= transform == 0 ? sdr_white_level_nits_i : 0.0001`; sets `multiplier = m`, `input_maximum_nits = (transform == 2) ? sdr_in : hdr_in`, `output_maximum_nits = (transform == 2) ? sdr_out : hdr_out`; blend push `ONE/INVSRCALPHA`; `tech_end(0,0, transform == 0 ? "Reinhard" : transform == 1 ? "MaxRGB" : "MaxRGBSDR")`; pop. Any other `transform` value uses the MaxRGBSDR technique with the HDR limits (Q-5). (`23`)
* **S-028-R82** hdr `video_get_color_space(count, prefs)`: `s = target space`, `r = s`; if `s` is EXT or SRGB and transform is 0 or 2: `r = SRGB`, then `r = SRGB_16F` if any preference is not SRGB (a zero count keeps SRGB). Return `r`. (`23`)
* **S-028-R83** sdr_on_hdr `update`: `gamma = (enum)int`, `multiplier = (f32)sdr_white_level_nits`, `power = (f32)f64`. Render: target space SRGB or SRGB_16F only (others skip); `begin_with_color_space(format, space, NO_DIRECT)`; sets `multiplier = nits / W`, `power`; blend push `ONE/INVSRCALPHA`; `tech_end(0,0, gamma == 1 ? "GammaSrgb" : "GammaPower")`; pop. `video_get_color_space` always returns EXT. (`24`)

## 6. Algorithms

### 6.0 Effect parameter / technique tables (names the filters use; checked by the runner)

```
color_correction_filter: gamma, color_matrix | Draw          chroma_key_filter(v0): color, contrast, brightness, gamma, chroma_key, pixel_size, similarity, smoothness, spill, yuv_mat
chroma_key_filter_v2: opacity, contrast, brightness, gamma, chroma_key, pixel_size, similarity, smoothness, spill, cb_v4, cr_v4
color_key_filter: color, contrast, brightness, gamma, key_color, similarity, smoothness | color_key_filter_v2: opacity instead of color
luma_key_filter(+_v2): lumaMax, lumaMin, lumaMaxSmooth, lumaMinSmooth | sharpness: sharpness, texture_width, texture_height
color_grade_filter: clut_1d, clut_3d, clut_amount, clut_scale, clut_offset, domain_min, domain_max | Draw1D Draw3D DrawAlpha3D DrawAmount3D DrawDomain3D
crop_filter: mul_val, add_val, multiplier | Draw DrawMultiply DrawTonemap DrawMultiplyTonemap
hdr_tonemap_filter: multiplier, input_maximum_nits, output_maximum_nits | Reinhard MaxRGB MaxRGBSDR
sdr_on_hdr_filter: multiplier, power | GammaSrgb GammaPower
mask_*/blend_*_filter: target, color, mul_val, add_val | Draw
```
(All `.effect` files also declare `image` and `ViewProj`.) Effect source text is data (S-020 compiles it).

### 6.1 Colour-correction matrices (v0 = "v1" in source names, v2)

```
gamma_out = g < 0 ? -g + 1 : 1/(g + 1)                       (f64 -> f32)
v0:  c = (f32)contrast + 1;   C = diag(c,c,c,1), C.t.xyz = (1-c)/2
v2:  c = (f32)contrast; c = c < 0 ? 1/(-c+1) : c+1;  C = diag(c,c,c,1)  (no translation)
B = identity with B.t.xyz = brightness
s = (f32)saturation + 1;  o_r = (1-s)*0.299, o_g = (1-s)*0.587, o_b = (1-s)*0.114      (constants f32)
S = [[o_r+s, o_r, o_r, 0],[o_g, o_g+s, o_g, 0],[o_b, o_b, o_b+s, 0],[0,0,0,1]]
half = 0.5f * (f32)(hue / (180.0f / M_PI))                  (the quotient is f64)
q = 0.57735f * (f32)sin(half);  w = (f32)cos(half)           (q is the same for x,y,z)
d = (0.5 - 2*q*q) * 2;  a = (q*q + q*w) * 2;  b = (q*q - q*w) * 2
H = [[d, b, a, 0],[a, d, b, 0],[b, a, d, 0],[0,0,0,opacity]]
     opacity: v0 = (f32)int * 0.01f ; v2 = (f32)f64
K = identity;   v0: K.x.x,y.y,z.z = rgba(color).rgb ; K.t.xyz = rgba.a * rgba.rgb
                v2: K.x.x,y.y,z.z = rgba_srgb(color_multiply).rgb ; K.t.xyz = rgba_srgb(color_add).rgb
v0:  F = ((B*C)*S)*H*K          v2:  F = ((C*B)*S)*H*K          (matrix4_mul(dst,a,b) = a*b)
pushed: gamma_out (float "gamma"), F (matrix4 "color_matrix")
```
With the default settings v0 and v2 give the identity matrix and gamma 1 (vector `04`/`05`, first render).

### 6.2 `tech_multiplier(current, source)` (crop, scroll, gpu_delay; the four-technique table)

```
default: ("Draw", 1)
source SRGB / SRGB_16F:  current == SCRGB -> ("DrawMultiply", W/80)
source EXT:    current SRGB|SRGB_16F -> "DrawTonemap" (1);  SCRGB -> ("DrawMultiply", W/80);  EXT -> default
source SCRGB:  current SRGB|SRGB_16F -> ("DrawMultiplyTonemap", 80/W);  EXT -> ("DrawMultiply", 80/W);  SCRGB -> default
```
(`W/80`, `80/W` evaluated in f32.) gpu_delay uses it with `frame.space` as the source and calls the core `default`
effect (gpu_delay's version treats unknown `current` values as "no change"). Vector `11` covers all 16 pairs.

### 6.3 Scale technique / multiplier

```
suffix(source, current) is the suffix of the 6.2 table:  "" | "Multiply" | "Tonemap" | "MultiplyTonemap"
technique = (undistort ? "DrawUndistort" : upscale ? "DrawUpscale" : "Draw") + suffix
   (undistort takes precedence over upscale; they can both be true only for no sampling mode)
multiplier: source in {SRGB, SRGB_16F, EXT}: current == SCRGB ? W/80 : 1;   source SCRGB: current != SCRGB ? 80/W : 1
```
The multiplier rule equals 6.2. The reference does not check that the technique exists in the chosen base effect; in every
vector the technique exists (`exists=1`), so an implementation needs no table of the core effects.

### 6.4 Shader pixel maths (author's pseudocode; data in the `.effect` files)

`tex` is the premultiplied-alpha sample. `unpm(t) = (t.rgb * (t.a > 0 ? 1/t.a : 0), t.a)`. `sat(x)=clamp(x,0,1)`.
`nl(u) = u <= 0.0031308 ? 12.92 u : 1.055 u^(1/2.4) - 0.055`. `CalcColor(c) = (c.rgb^gamma * contrast + brightness, c.a)`.

* **Chroma key v2.** `d(rgb) = distance(chroma_key, (dot(rgb, cb_v4.xyz) + cb_v4.w, dot(rgb, cr_v4.xyz) + cr_v4.w))`. Five taps: `p0=(px, py/2)`, `p1=(px/2,-py)` with `pixel_size=(px,py)`; `dist = (2*(d(nl(s(uv-p0))) + d(nl(s(uv+p0))) + d(nl(s(uv-p1))) + d(nl(s(uv+p1)))) + d(nl(unpm(center).rgb))) / 9` where taps `s` are the **premultiplied** samples. `base = dist - similarity`; `full = sat(base/smoothness)^1.5`; `spill_v = sat(base/spill)^1.5`; `a = alpha*opacity*full`; `desat = dot(rgb, (.2126,.7152,.0722))`; `rgb = lerp(desat, rgb, spill_v)`; `out = CalcColor`; `out.rgb *= out.a`. (Model: `chroma2`.)
* **Chroma key v0.** Same with `yuv_mat` product (row-vector convention is unverified: open question O-1), `rgba *= color` before alpha scaling, no premultiplication at the end. Not modelled.
* **Color key v2.** `d = distance(key_color.rgb, nl(unpm.rgb))`; `a = alpha*opacity * sat(max(d - similarity, 0)/smoothness)`; `CalcColor`; `rgb *= a`. v0: `rgba *= color`, `d = distance(key_color.rgb, rgb)` (no sRGB), alpha as above, no final premultiply. (Models `colorkey_v2`, `colorkey_v1`.)
* **Luma key.** `l = dot(rgb, coef)`, coef v2 `(.2126,.7152,.0722)`, v0 `(.2989,.5870,.1140)`; `mask = smoothstep(min, min+minS, l) * (1 - smoothstep(max-maxS, max, l))`; v2: `a *= mask; rgb *= a`; v0: output `(rgb, mask)`. `smoothstep` with a zero-width edge is undefined (GPU-dependent). (Models `luma_v2`, `luma_v1`.)
* **Mask (colour):** `rgba = unpm * color; a = rgba.a * (t.r+t.g+t.b)/3` with `t` the mask sample at `uv*mul+add`; `rgb *= a`. Alpha mask: `a *= t.a`. Blend mul / add / sub: `rgb = sat(rgb OP t.rgb)`, `rgb *= a`.
* **Tonemap.** `rec709->2020` / `2020->709` 3x3 matrices of `color.effect`; Reinhard: `x/(x+1)`, saturate, `^(1/2.4)`, sRGB EOTF; maxRGB: `m = max(r,g,b)`, PQ encode of `m`, EETF (knee `KS = 1.5*maxLum - 0.5`, Hermite spline above the knee), `scale = pq^-1(E2)/max(m, 6.10352e-5)`; HDR variant multiplies by `1/multiplier` at the end, SDR variant by `10000/out_nits`, then `^(1/2.4)` and the sRGB EOTF. (Models `reinhard`, `maxrgb`, `maxrgb_sdr`.)
* **sdr_on_hdr:** `GammaSrgb`: `rgb *= multiplier`; `GammaPower`: `rgb = nl(rgb)^power * multiplier`.
* **Crop/scroll vertex:** `uv' = uv * mul_val + add_val`; sampler `Border` (transparent) addressing.

## 7. Concurrency

`create/update/destroy` and `render` run on the graphics thread of the host or under `obs_enter_graphics`; `tick` runs on the
video thread. No filter is thread-safe by itself; the deferred update guarantees `update` and `tick` are serialised. No locks
in this component.

## 8. Reference defects and quirks

* **Q-1** (QUIRK) v0 keys: `color = 0xFFFFFF | (((u32)opacity*255/100) << 24)`; opacity `> 100` (e.g. 150: `382<<24` wraps to `0x7E`) or negative (u32 wrap) produce a wrapped alpha byte. SHOULD match. Script `06`.
* **Q-2** (QUIRK) the `Base.Canvas`/`None` resolution strings are compared against the *translated* text; in the vectors identity. A port MUST compare against the translated string it also puts into the combo, and treat any non-parsable string (including `None`) as invalid. `13`.
* **Q-3** (QUIRK) scale: unknown `sampling` strings mean bicubic (also for the `undistort` visibility); an invalid resolution leaves the previous `sampling`, `undistort` and `cx_in/cy_in` untouched. `13`, `03`.
* **Q-4** (QUIRK) PNG LUT tile row uses `z / mh` instead of `z / mw`. For square tile grids (512x512) this is correct; for in-bounds non-square grids (128x2048, 64x4096, 256x1024) the reference builds a wrong volume (it reuses tile rows). SHOULD match (vector `18` records source pixel indices). Required for out-of-bounds layouts: D-2.
* **Q-5** (QUIRK) hdr `transform` outside 0..2 selects technique `MaxRGBSDR` with the HDR limits and multiplier `* 0.0001`. `23`.
* **Q-6** (QUIRK) scroll default key `limit_size` is never read. `01`.
* **Q-7** (QUIRK) mask with an empty path never stores a timestamp (stays 0) while polling sees -1: the (empty) image is reloaded at *every* one-second poll. `17`.
* **Q-8** (QUIRK) crop with all-zero settings outputs the full frame (`10`).
* **Q-9** (QUIRK) the luma key v0 render applies the colour-space rules of v2 (EXT -> skip); and `get_color_space` is not provided for v0. `08`.
* **Q-10** (QUIRK) the lowres scale effect has no `base_dimension[_i]`: the parameters are not set (null-guarded). `13`.
* **Q-11** (QUIRK) gpu_delay: while `target_valid` is false the stored interval is not updated, so the next valid tick recomputes it. `22`.
* **D-1** (DEFECT) scale: aspect `A:B` with `A == 0` or `B == 0` (also `0:0`) converts inf/NaN to int (UB). Required: treat `A <= 0 || B <= 0` aspects as invalid resolution (pass-through, no crash). Script `25` (`ignore`).
* **D-2** (DEFECT) clut PNG whose tile grid satisfies `floor(63 / mh) >= mh` (4096x64: mh=1, 2048x128: mh=2, 1024x256: mh=4): the tile row `(z / mh) * 64 + 63` exceeds the image height and the reference reads out of bounds. Required: reject the image (no texture, the filter skips) or index with `z / mw`; MUST NOT read out of bounds. Script `19` (`try`).
* **D-3** (DEFECT) scale `%d` parse of an out-of-range number (`99999999999x5`) is UB (`sscanf`). Required: treat as invalid. Script `25`.
* **D-4** (DEFECT) crop: `width - left - right` in `int` overflows for extreme values (e.g. `left = INT_MIN`). Required: saturating arithmetic (result >= 1). Script `25`.
* **D-5** (DEFECT) gpu_delay with `fps_num == 0` divides by zero (SIGFPE). Required: no frames, filter skipped. `25` (`try`). (`fps_den == 0` is harmless.)
* **D-6** (DEFECT) clut `.cube` with a huge `LUT_*_SIZE` (e.g. 2048): `4*N^3*sizeof(half)` is computed in `u32` and wraps, so the buffer is too small and entries are written beyond it. Required: reject sizes whose volume does not fit (limit `N <= 256` is conforming), no crash. `25` (`ignore`).
* **D-7** (DEFECT) division by zero floats: `hdr_tonemap` with `sdr_white_level_nits == 0`, `sdr_on_hdr` with `W == 0`, chroma with base size 0 push `inf` (defined for f32). A port MUST push `inf` too. `23`, `24`, `06`.

## 9. Implementation notes (non-normative)

* Model each filter as a struct with `update`, `tick`, `render`, `size`, `colorspace` methods against a `trait Host` (source accessors, video info) and a `trait Gfx` (the calls listed in §3.2); the conformance runner implements both as recorders. Keep `f32`/`f64` exactly as written (`as f32` after f64 math).
* `sscanf("%d")` semantics: skip leading whitespace, optional sign, digits; stop at the first non-digit; do **not** use `str::parse` on the whole string.
* `.cube` parsing: emulate `%f`: decimal or exponent floats, `inf`/`nan` are accepted by `sscanf` too.
* gpu_delay: `VecDeque<Frame>`; the zero-initialised frame state (`space = SRGB`) matters for `video_get_color_space` before the first render.

## 10. Conformance coverage

| Rule(s) | Scripts |
|---|---|
| R01 | 01 |
| R02, R04, R05 | 04, 10, 12, 16, 18, 22 |
| R03 | 09, 16 |
| R10-R13 | 02, 03 |
| R20-R23 | 04, 05 |
| R30-R35 | 06, 07, 08, 26 (model) |
| R36 | 09 |
| R40-R44 | 10, 11 |
| R45-R47 | 12 |
| R50-R53 | 13, 14, 15 |
| R55-R59 | 16, 17 |
| R60-R64 | 18, 19, 20 |
| R70-R75 | 21, 22 |
| R80-R83 | 23, 24, 27 (model) |
| Q-1..Q-11 | 06, 13, 03, 18, 23, 01, 17, 10, 08, 15, 22 |
| D-1..D-7 | 25, 19, 23, 24, 06 |

Class: R-rules and Q-rules in scripts `01`-`24` are CORE/QUIRK as labelled; `19` and `25` are DEFECT scripts; `26`, `27` are
author-model scripts (tolerance 1e-4, not authoritative: they check the shader pseudocode of §6.4, not C behaviour).

## 11. Open questions

* **O-1** The matrix convention of `mul(yuv_mat, ...)`/`mul(color_matrix, px)` in the v0 chroma key and colour-correction shaders (row vs column vector after the effect compiler's translation) was not verified against a GPU; the CPU side (what is pushed) is fully specified. Implementers must not guess: use the S-020 semantics of `mul`.
* **O-2** `obs_source_get_color_space` of a *filter chain* target (as opposed to a plain source) is the host's job and is an input here.
* **O-3** Behaviour of `smoothstep` with equal edges, `pow` of negative numbers etc. in shaders is GPU dependent.
* **O-4** The exact multiplier/technique tables of §6.3 were transcribed from the code; the vector `15` covers only the combinations listed in that script. Unknown colour-space integers are not covered.
