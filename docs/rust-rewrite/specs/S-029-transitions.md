# S-029 Transitions plugin (obs-transitions)

| | |
|---|---|
| Status | Ready for review |
| Reference | libobs 33.0.1 `plugins/obs-transitions` (all `transition-*.c`, `easings.h`, `obs-transitions.c`, `data/`) |
| Target crate | `crates/obs-transitions` |
| Depends on | S-014 (obs_data), S-015 (obs_properties), S-020 (effect language, the six `.effect` assets), S-002 (calldata) |
| Conformance | `test/conformance/scripts/transitions/` (16 scripts); language in `test/conformance/README.md` section "transitions runner" |

## 1. Scope

The seven built-in scene transitions: **cut**, **fade**, **fade to color**, **luma wipe**, **slide**, **swipe** and
**stinger** (video file overlay with optional track matte). Specified: registration (ids, flags, callbacks), settings
keys and defaults, properties (and their *modified* rules), the exact sequence of graphics calls a `video_render`
produces for a given progress `t`, the audio mix curves, the stinger timing/state machine, the luma image set and the
easing function.

Out of scope (S-0xx "source core"): the transition helpers of libobs (`obs_transition_video_render*`,
`obs_transition_audio_render`, fixed-duration clock, A/B texture rendering). They are **consumed** through the
contract in section 3.2, which the conformance runner fakes. Out of scope too: the ffmpeg media source, the PNG
decoder and the GPU. Shader *pixel* semantics are specified in section 6.9 because the `.effect` assets are inputs of
the plugin (their bytes are pinned by hash in vector `01-registration`).

## 2. Conventions

`t` is a float32 progress in nominal range 0..1 (callers pass whatever the clock gives; **nothing in the plugin
clamps `t`**, S-029-R40). All arithmetic is IEEE float32 unless a rule says `long double`/`u64`. Floats print with
`%.9g`; vectors compare numerically within relative 1e-6 (text comparison is exact for the committed outputs
because the reference was float32 too). The sign of zero is significant in the text (`-0`).
`obs_module_text(K)` returns the *locale key* K unchanged in the vectors; the real module resolves K through
`data/locale/*.ini` (English strings live there; keys listed below are the contract).
Enum numbers: `gs_color_space` 0 sRGB, 1 sRGB_16F, 2 709_EXTENDED, 3 709_SCRGB; `gs_color_format` GS_RGBA = 3, GS_RGBA16F = 9 (texrender format: sRGB -> 3, every other space -> 9);
`obs_media_state`: 0 NONE, 1 PLAYING, 2 OPENING, 3 BUFFERING, 4 PAUSED, 5 STOPPED, 6 ENDED, 7 ERROR.

## 3. Interface

### 3.1 Module

`obs_module_load()` registers, **in this order**, seven sources, always returns true:

| # | id | display name key | callbacks present |
|---|---|---|---|
| 1 | `cut_transition` | `CutTransition` | create destroy video_render audio_render video_get_color_space |
| 2 | `fade_transition` | `FadeTransition` | same as cut |
| 3 | `swipe_transition` | `SwipeTransition` | + update get_properties |
| 4 | `slide_transition` | `SlideTransition` | + update get_properties |
| 5 | `obs_stinger_transition` | `StingerTransition` | create destroy update get_defaults video_render **video_tick** audio_render **missing_files** get_properties **enum_active_sources enum_all_sources transition_start transition_stop** video_get_color_space |
| 6 | `fade_to_color_transition` | `FadeToColorTransition` | create destroy update get_defaults get_properties video_render audio_render video_get_color_space |
| 7 | `wipe_transition` (luma wipe) | `LumaWipeTransition` | same as 6 |

Module description string: `OBS core transitions`. Default locale en-US.

S-029-R01. Registration order and ids are exactly the table (note the luma wipe id is `wipe_transition`, the stinger id
is `obs_stinger_transition`). **R02.** All seven have `type = TRANSITION` (2), `version = 0` and declare
`output_flags = 0`; the core adds `COMPOSITE|VIDEO|CUSTOM_DRAW` (= 0x49) at registration, so the effective flags are
0x49 for all. No transition sets `get_width/get_height` (the core would ignore them). **R03.** The module is
`OBS_DECLARE_MODULE` with API version `LIBOBS_API_VER`. Because all transitions carry `OBS_SOURCE_VIDEO`, every
`obs_source_update` is **deferred** to the next video tick (the core increments a counter; the next tick calls
`update(data, settings)` once, *before* the transition's own `video_tick`). Exceptions: the luma wipe calls its own
update synchronously from `create`.

### 3.2 Core contract assumed by the plugin (not specified here, faked by the runner)

- `create(settings, source)`: `settings` already holds the user's values and the defaults from `get_defaults`.
  `obs_source_update(source, s)` called inside `create` only records a deferred update (`data` is not yet set).
- `obs_transition_video_render(src, cb)` / `render2(src, cb, placeholder)`: while a transition is active and the
  transition size is non-zero it renders sources A and B into textures (a missing source yields the placeholder; for
  `render` the placeholder is the non-NULL transparent texture, fade uses `render2` with placeholder NULL so A or B can
  be NULL) and calls `cb(data, a, b, t, cx, cy)` once, inside a blend-state push/pop. `cb == NULL` means "draw the
  active child directly". `t` is the (possibly fixed-duration) progress.
- `obs_transition_video_render_direct(src, A|B)` draws that child directly, returns false when no longer transitioning.
- `obs_transition_audio_render(src, &ts, audio, mixers, channels, rate, mixA, mixB)` evaluates `mixA(data,t)` /
  `mixB(data,t)` per sample and fills the enabled mixers with `A*mixA + B*mixB`.
- `obs_transition_get_time`, `obs_transition_enable_fixed(src, on, ms)` (fixed linear duration, `ms=0` for cut),
  `obs_transition_video_get_color_space`.

## 4. Data: assets

**Effects.** `data/<name>.effect`, six files: `fade_transition` (techniques `Fade`, `FadeLinear`, `FadeSingle`),
`fade_to_color_transition` (`FadeToColor`), `luma_wipe_transition` (`LumaWipe`), `slide_transition` (`Slide`),
`swipe_transition` (`Swipe`), `stinger_matte_transition` (`StingerMatte`, `StingerMatteLinear`). Sizes and FNV-1a/64
hashes are in vector `01-registration` (e.g. fade 1768 bytes `2c736b1652fa818b`). A parameter lookup by a name the file
does not declare yields NULL. Uniform names per file: fade `tex_a tex_b fade_val`; fade-to-color `tex swp color`;
luma `a_tex b_tex l_tex progress invert softness`; slide `tex_a tex_b tex_a_dir tex_b_dir`; swipe `tex_a tex_b swipe_val`;
stinger `a_tex b_tex matte_tex invert_matte` (plus the core base effect `default.effect`: `image`, `multiplier`,
techniques `Draw`, `DrawMultiply`, `DrawTonemap`, `DrawMultiplyTonemap`).

**Luma set.** `data/luma_wipes/` holds 34 greyscale (PNG color type 0, 8-bit) images plus `wipes.json`, an object
mapping *display key* -> file name, in this order (the order is the list order of the property):
`LumaWipe.Type.BarndoorBottomLeft` barndoor-botleft.png, `...BarndoorHorizontal` barndoor-h.png, `...BarndoorTopLeft`
barndoor-topleft.png, `...BarndoorVertical` barndoor-v.png, `...BlindsHorizontal` blinds-h.png, `...BoxBottomLeft`
box-botleft.png, `...BoxBottomRight` box-botright.png, `...BoxTopLeft` box-topleft.png, `...BoxTopRight`
box-topright.png, `...Burst` burst.png, `...CheckerboardSmall` checkerboard-small.png, `...Circles` circles.png,
`...Clock` clock.png, `...Cloud` cloud.png, `...Curtain` curtain.png, `...Fan` fan.png, `...Fractal` fractal.png,
`...Iris` iris.png, `...LinearHorizontal` linear-h.png, `...LinearTopLeft` linear-topleft.png, `...LinearTopRight`
linear-topright.png, `...LinearVertical` linear-v.png, `...ParallelZigzagHorizontal` parallel-zigzag-h.png,
`...ParallelZigzagVertical` parallel-zigzag-v.png, `...Sinus9` sinus9.png, `...Spiral` spiral.png, `...Square`
square.png, `...Squares` squares.png, `...Stripes` stripes.png, `...StripsHorizontal` strips-h.png,
`...StripsVertical` strips-v.png, `...Watercolor` watercolor.png, `...ZigzagHorizontal` zigzag-h.png,
`...ZigzagVertical` zigzag-v.png. Image sizes: 14 images 720x480, 4 images 720x560 (burst and three others),
15 images 720x576, one 800x600; exact per-file size, length and hash in vector `01-registration` (`assets`).
The size handling is entirely in UV space: the image is bound as the luma texture and sampled with normalised
coordinates, so *any* resolution is stretched over the output (the plugin never rescales). Images are loaded with
*straight* alpha mode (value 0) and the red channel is the luma.

## 5. Behaviour (numbered rules)

### 5.1 Easing

**R10.** `cubic_ease_in_out(t)`: for `t < 0.5`: `4*t*t*t`; else with `u = 2*t - 2`: `(t-1)*u*u + 1` (float32, evaluated
in that association: `((t-1)*u)*u + 1`). It is the only easing function of the plugin; it is **not clamped**: e.g.
`f(-0.5) = -0.5`, `f(1.5) = 1.5`. Table (vector 01): f(0.1)=0.004, f(0.25)=0.0625, f(0.4)=0.256, f(0.6)=0.744,
f(0.75)=0.9375, f(0.9)=0.996, f(0.5)=0.5, f(1)=1.

### 5.2 Cut (`cut_transition`)

**R11.** Create: allocate, `obs_transition_enable_fixed(source, true, 0)`; no settings. Never fails. **R12.** `video_render`
calls `obs_transition_video_render(source, NULL)` (no callback; core draws the active child). **R13.** Audio: mixA = `1-t`,
mixB = `t`. **R14.** `video_get_color_space` returns `obs_transition_video_get_color_space(source)` (ignores the
preferred list).

### 5.3 Fade (`fade_transition`)

**R15.** Create loads `fade_transition.effect` inside `enter/leave_graphics`; on failure logs ERROR
`Could not find fade_transition.effect` and returns NULL (no instance). Params `tex_a`, `tex_b`, `fade_val`.
**R16.** `video_render`: `prev = gs_set_linear_srgb(true)`; `obs_transition_video_render2(source, cb, NULL)`;
`gs_set_linear_srgb(prev)`. **R17.** Callback with `(a, b, t, cx, cy)`: if both `a` and `b` are NULL do nothing at all
(not even the srgb toggle). Else: `prev = framebuffer_srgb`; `enable_framebuffer_srgb(true)`;
- one input missing: technique `FadeSingle`; present input P: `set_texture_srgb(tex_a, P)`; if it is A then `t = 1 - t`;
- both present and `gs_get_color_space() == SRGB`: `set_texture(tex_a, a)`, `set_texture(tex_b, b)` (non-srgb setters), technique `Fade`;
- both present and space != SRGB: `set_texture_srgb` for both, technique `FadeLinear`;
then `set_float(fade_val, t)`, one pass `gs_effect_loop(technique)` -> `draw_sprite(NULL, 0, cx, cy)`, then
`enable_framebuffer_srgb(prev)`. **R18.** Audio mixA `1-t`, mixB `t`. **R19.** `video_get_color_space(count, preferred)`:
`space = transition_space`; for each preferred `p` in order: `space = p`; stop at the first with `p == transition_space`.
So with `count = 0` the answer is `transition_space`; if no entry matches the answer is the **last** entry.

### 5.4 Fade to color (`fade_to_color_transition`)

Settings: `color` int default `0xFF000000`; `switch_point` int default 50 (percent). **R20.** Create loads
`fade_to_color_transition.effect` (error `Could not find fade_to_color_transition.effect`, NULL), params `tex swp color`,
zero-initialised instance, then `obs_source_update(source, settings)` (deferred; fields are 0 until the first tick).
**R21.** `update`: `color = (u32)get_int(color) | 0xFF000000` (alpha forced opaque); the value is `0xAABBGGRR`: R = bits 0-7,
G = 8-15, B = 16-23, each `/255`, A = 1. Two vec4: linear-in-sRGB-space-agnostic `color` and `color_srgb` (each channel
converted with the sRGB decode `c<=0.04045 ? c/12.92 : ((c+0.055)/1.055)^2.4`, alpha unchanged). `switch_point =
(float)(u32)get_int(switch_point) / 100`: negative ints wrap through u32 (-10 -> 42949672); no clamp (500 -> 5).
**R22.** Properties: `color` (type color, desc key `Color`), `switch_point` (int slider 0..100 step 1, suffix `%`, desc
`SwitchPoint`); property flags 0. **R23.** Callback(t): `sa = smoothstep(0, sp, t)`, `sb = smoothstep(sp, 1, t)`;
`swp = t < sp ? sa : 1 - sb`; texture = `t < sp ? a : b`; `nonlinear = (gs_get_color_space() == SRGB)`;
`prev = fb_srgb`; `enable_framebuffer_srgb(!nonlinear)`; nonlinear: `set_texture(tex, T)`, `set_vec4(color, color)`;
else `set_texture_srgb(tex, T)`, `set_vec4(color, color_srgb)`; `set_float(swp)`; pass "FadeToColor" -> `draw_sprite`;
`enable_framebuffer_srgb(prev)`. `video_render` wraps this in `gs_set_linear_srgb(true)`/restore around
`obs_transition_video_render(source, cb)`. **R24.** `smoothstep(min,max,x) = c*c*(3-2c)` with
`c = clamp((x-min)/(max-min), 0, 1)` and `clamp(x,lo,hi)` implemented as `x<lo ? lo : x>hi ? hi : x`
(**NaN propagates**, Q-3). **R25.** Audio: `sp` = switch_point; `mixA(t) = lerp(1-t, 0, smoothstep(0,sp,t))`,
`mixB(t) = lerp(0, t, smoothstep(sp,1,t))` where `lerp(a,b,x) = (1-x)*a + x*b`. Values: sp=0.5: mixA(0.25)=0.375,
mixA(0.5)=0, mixB(0.75)=0.375, mixB(1)=1. **R26.** Color-space query as R19.

### 5.5 Luma wipe (`wipe_transition`)

Settings: `luma_image` string default `linear-h.png`; `luma_softness` double default 0.03; `luma_invert` bool default false.
**R27.** Create: load `luma_wipe_transition.effect` (ERROR `Could not open luma_wipe_transition.effect`, NULL), params
`a_tex b_tex l_tex progress invert softness`, zeroed instance, `luma_wipe_get_list` (reads `luma_wipes/wipes.json` through
the module-file lookup; a missing file just leaves the list empty/NULL), then the **synchronous** `update`.
**R28.** `update`: read image name, invert, softness `(float)get_double`; path = `"luma_wipes/" + name` resolved with the
module-file lookup (NULL if the file does not exist -> no image); in graphics: free the old image; **outside** graphics
`image_init(path, ALPHA_STRAIGHT)`; in graphics `image_init_texture`. A missing/undecodable file leaves
`loaded = false`, `texture = NULL` (decode failure logs WARNING `Failed to load file`) and rendering proceeds with a NULL
luma texture. No path sanitation (`../` accepted, Q-5); empty name resolves to the directory name (exists) and fails to
decode. **R29.** Properties: list `luma_image` (desc `LumaWipe.Image`, list type LIST, format STRING) with one entry per
`wipes.json` item in file order `(obs_module_text(key), file)`; **only when `data` is non-NULL** (NULL -> empty list);
float `luma_softness` (0..1 step 0.05, scroller, desc `LumaWipe.Softness`); bool `luma_invert` (`LumaWipe.Invert`).
**R30.** Callback `(a, b, t, cx, cy)` in order: `prev = fb_srgb`; `enable_framebuffer_srgb(true)`;
`set_texture_srgb(a_tex, a)`; `set_texture_srgb(b_tex, b)`; `set_texture(l_tex, luma_texture)` (non-srgb);
`set_float(progress, t)`; `set_bool(invert, invert)`; `set_float(softness, softness)`; pass "LumaWipe" ->
`draw_sprite(NULL,0,cx,cy)`; restore. `video_render` has **no** `set_linear_srgb` wrap. **R31.** Destroy frees the image in
graphics and releases the list. Audio mixA `1-t`, mixB `t`. Color space query = cut (R14).

### 5.6 Slide (`slide_transition`)

Setting `direction` string (no default; absent = "left"). **R32.** Create like R20 (effect `slide_transition.effect`, params
`tex_a tex_b tex_a_dir tex_b_dir`, error `Could not find slide_transition.effect`, deferred update). **R33.** `update`:
`"right" -> (-1,0)`, `"up" -> (0,1)`, `"down" -> (0,-1)`, anything else (including "left", "Left", "") `-> (1,0)`; `dir` is (0,0)
until the first update. **R34.** Properties: list `direction` (desc `Direction`, LIST/STRING): (`Direction.Left`,"left"),
(`Direction.Right`,"right"), (`Direction.Up`,"up"), (`Direction.Down`,"down"). **R35.** Callback: `e = cubic_ease_in_out(t)`;
`a_dir = dir*e`; `b_dir = dir*(1-e)`; `prev = fb_srgb`; enable srgb; `set_texture_srgb(tex_a, a)`,
`set_texture_srgb(tex_b, b)`, `set_vec2(tex_a_dir, a_dir)`, `set_vec2(tex_b_dir, b_dir)`, pass "Slide" -> draw; restore. No
linear-srgb toggle. Example left, t=0.25: a_dir=(0.0625,0), b_dir=(0.9375,0). **R36.** Audio: mixA `1-ease(t)`, mixB `ease(t)`.
Color space = R14.

### 5.7 Swipe (`swipe_transition`)

Settings `direction` (as slide), `swipe_in` bool (default false). **R37.** Create: `bmalloc` instance (**fields
uninitialised until the first update**; a render before the first tick is outside the contract), effect
`swipe_transition.effect` params `tex_a tex_b swipe_val`, error `Could not find swipe_transition.effect`, deferred update.
`update` maps direction exactly as R33 and stores `swipe_in`. **R38.** Properties: list `direction` as R34, then bool
`swipe_in` (desc `SwipeIn`). **R39.** Callback: `v = dir`; if swipe_in `v = -v` (component-wise negation, **-0 is
produced** for zero components); `e = ease(t)`; `v = v * (swipe_in ? 1-e : e)`; `prev = fb_srgb`; enable srgb;
`set_texture_srgb(tex_a, swipe_in ? b : a)`; `set_texture_srgb(tex_b, swipe_in ? a : b)`; `set_vec2(swipe_val, v)`;
pass "Swipe" -> draw; restore. Audio mix as slide (R36).

**R40.** None of R17/R23/R35/R39 clamp `t`: `render` with `t = 1.5` produces `swipe_val` magnitude 1.5 (vector 05).

### 5.8 Stinger (`obs_stinger_transition`)

Settings (keys): `path` string; `hw_decode` bool (**default true**); `preload` bool; `tp_type` int (0 time, 1 frame);
`transition_point` int; `track_matte_enabled` bool; `track_matte_layout` int (0 horizontal, 1 vertical, 2 separate file,
3 mask); `track_matte_path` string; `invert_matte` bool; `enable_monitoring` bool; `audio_fade_style` int (0
fade-out/fade-in, 1 cross fade; any other value acts as 0 for the mix functions and is stored as-is).

**R41. Create.** Load `stinger_matte_transition.effect` in graphics with an error string; on failure ERROR
`Could not open stinger_matte_transition.effect: <err>` and NULL. Params `a_tex b_tex matte_tex invert_matte`;
zeroed instance whose `mix_a/mix_b` are fade-out/fade-in; `obs_transition_enable_fixed(source, true, 0)`; deferred
`obs_source_update`.

**R42. Update** (runs at the first tick and after every settings change), in this order:
1. read `path`, `hw_decode`, `preload`; build media settings `{local_file=path, hw_decode, looping=false,
   full_decode=preload, is_stinger=true, is_track_matte=<*current* track_matte_enabled field, i.e. the value from before
   this update>}`;
2. if a media source exists and `transitioning`: `remove_active_child(media)`; release the old media source; create
   private `ffmpeg_source` named `"<transition name> (Stinger)"`; if created and `transitioning`: `add_active_child`;
3. `point = get_int(transition_point)`; `tp_is_frame = get_int(tp_type) == 1`; frame mode: `transition_point_frame = (u64)point`;
   time mode: `transition_point_ns = (u64)(point * 1000000)` (signed multiply; overflow is UB in the reference, D-1:
   required = wrap modulo 2^64);
4. `was = track_matte_enabled`; read `track_matte_enabled`, `matte_layout = (int)get_int(track_matte_layout)`;
   `matte_width_factor = layout==0 ? 2 : 1`, `matte_height_factor = layout==1 ? 2 : 1`; `invert_matte`;
   `do_texrender = enabled && layout < 2`;
5. if a matte source exists: (transitioning -> `remove_active_child`), release, clear. If `enabled && layout == 2`: create
   private `ffmpeg_source` with NULL name and `{local_file=track_matte_path, looping=false}`; (created && transitioning ->
   `add_active_child`); `set_muted(matte, true)`;
6. `monitoring = enable_monitoring`; `set_monitoring_enabled(media, monitoring)`;
7. `fade_style = get_int(audio_fade_style)`: 1 -> cross fade mixers, else fade-out/fade-in mixers;
8. if `enabled != was`: in graphics destroy both texrenders (matte_tex, stinger_tex); if now enabled create both
   (`texrender_create(GS_RGBA, ZS_NONE)`, matte first then stinger).
Event order within an update is exactly this (vector 06). The media source is **re-created on every update**, even if
only unrelated keys changed.

**R43. Start.** `transition_start`: if a media source exists: `state = media_get_state`; if `transitioning` **or** `state ==
PLAYING`: call proc `restart` on the media (and on the matte if present) and **return without changing anything else**
(`transitioning` is not set; Q-1). Else: `matte_rendered = false`; call procs `get_duration` then `get_nb_frames` on the
media into one calldata; `duration_ns = (u64)duration + 250000000`; `duration_frames = (u64)num_frames`;
`transition_point = (float)((long double)tp / (long double)duration)` with tp/duration = frame count/frames in frame mode,
ns/ns in time mode (long-double division then float32 rounding); clamp: `> 0.999 -> 0.999`, `< 0.001 -> 0.001`
(**NaN passes both tests unchanged**, Q-2); `a_mul = 1/tp`, `b_mul = 1/(1-tp)`; if `track_matte_enabled && matte_source`:
call `get_duration` on the matte, `duration_ns = max(duration_ns, matte_duration)`, `add_active_child(matte)`;
`obs_transition_enable_fixed(source, true, (u32)(duration_ns / 1000000))` (truncated to 32 bits, D-2);
`add_active_child(media)`. In every case that gets past the `if (media)` block (including no media at all)
`transitioning = true` is set at the end. **R44. Stop.** `remove_active_child(media)` if media; `remove_active_child(matte)` if
matte; call proc `preload_first_frame` on the media's proc handler (a NULL handler is a harmless no-op); `transitioning =
false`. Stop without start still performs these calls (the child counter goes negative in the fake; the core tolerates it).

**R45. Enum/missing.** `enum_active_sources` reports media then matte only while `transitioning`; `enum_all_sources` reports
whichever exist. **R46. missing_files**: list; if `path` non-empty and the file does not exist, add a source-type missing
file ("media_source" tag); then same for non-empty `track_matte_path` ("matte_source" tag); the callback writes the new path to
`path` or `track_matte_path` and calls `obs_source_update(source, settings)` (deferred).

**R47. Properties** (flags `OBS_PROPERTIES_DEFER_UPDATE` = 1), in order: path `path` (desc `VideoFile`, FILE, filter
`<FileFilter.VideoFiles> (*.mp4 *.ts *.mov *.wmv *.flv *.mkv *.avi *.gif *.webm);;<FileFilter.AllFiles> (*.*)` — the
two keys are looked up and substituted); list `tp_type` (desc `TransitionPointType`, INT: `TransitionPointTypeTime`=0,
`TransitionPointTypeFrame`=1; modified rule R48); bool `hw_decode` (`HardwareDecode`); bool `preload`
(`PreloadVideoToRam`, long description `PreloadVideoToRam.Description`); int `transition_point` (`TransitionPoint`, 0..120000
step 1, scroller); checkable group `track_matte_enabled` (desc `TrackMatteEnabled`, modified rule R50) containing: list
`track_matte_layout` (desc `TrackMatteLayout`, INT: `TrackMatteLayoutHorizontal`=0, `TrackMatteLayoutVertical`=1,
`TrackMatteLayoutMask`=3; separate-file entry is compiled out; modified rule R49), path `track_matte_path`
(`TrackMatteVideoFile`, same filter), bool `invert_matte` (`InvertTrackMatte`); then bool `enable_monitoring`
(`AudioMonitoring`); list `audio_fade_style` (`AudioFadeStyle`, INT: `AudioFadeStyle.FadeOutFadeIn`=0,
`AudioFadeStyle.CrossFade`=1). Properties do not depend on `data`.
**R48.** `tp_type` modified: `tp_type == 0`: `transition_point` description `TransitionPoint`, suffix `" ms"`; else description
`TransitionPointFrame`, suffix `""`; returns true. **R49.** `track_matte_layout` modified: `track_matte_path.visible = (layout == 2)`;
true (the lookup `obs_properties_get(top, ...)` descends into groups). **R50.** `track_matte_enabled` modified: `tp_type`
description becomes `AudioTransitionPointType` if enabled else `TransitionPointType`; true.

**R51. Video render.**
*Without track matte:* `t = obs_transition_get_time`; target = `t < transition_point ? A : B`;
`ok = obs_transition_video_render_direct(source, target)`; if `!ok` return.
*With track matte:* `ready = media active && media_cx != 0 && media_cy != 0`; if ready: `matte_rendered = true`,
`obs_transition_video_render(source, matte_cb)`; else `render_direct(matte_rendered ? B : A)` (result ignored). If `layout ==
3 (mask)` return.
*Then (all paths that did not return):* `source_cx/cy = obs_source_get_width/height(source)`; if media_cx or media_cy is 0 return.
- `do_texrender` true: `space = media color space`; `stinger_texrender(...)` (below); `prev=fb_srgb`; enable srgb;
  `(tech, mult) = tech_and_multiplier(gs_get_color_space(), space)`; `e = base default effect`; `set_texture_srgb(image,
  stinger_tex_texture)`; `set_float(multiplier, mult)`; pass `tech` -> `draw_sprite(NULL,0,source_cx,source_cy)`; restore.
- else: `prev = gs_set_linear_srgb(true)`; `matrix_push`; `matrix_scale3f(source_cx/media_cx, source_cy/media_cy, 1)`;
  `obs_source_video_render(media)`; `matrix_pop`; `gs_set_linear_srgb(prev)`.
**stinger_texrender:** `format = format_from_space(space)` (sRGB -> GS_RGBA(3), others -> 16F format); if the texrender's
format differs destroy and recreate it; if `texrender_begin(stinger_tex, source_cx, source_cy, space)` succeeds:
`ortho(0, media_cx/wf, 0, media_cy/hf, -100, 100)`; `blend_state_push`; `enable_blending(false)`; render media; `blend_state_pop`;
`texrender_end`.
**tech_and_multiplier(current, source)** with `w = sdr_white_level`:
source sRGB/sRGB_16F: current 709_SCRGB -> (`DrawMultiply`, w/80) else (`Draw`,1);
source 709_EXTENDED: current sRGB/sRGB_16F -> (`DrawTonemap`,1); 709_SCRGB -> (`DrawMultiply`, w/80); 709_EXTENDED -> (`Draw`,1);
source 709_SCRGB: current sRGB/sRGB_16F -> (`DrawMultiplyTonemap`, 80/w); 709_EXTENDED -> (`DrawMultiply`, 80/w); 709_SCRGB -> (`Draw`,1).
**matte_cb(a, b, t, cx, cy):** `matte_src = layout == 2 ? matte_source : media`; `mcx = width(matte_src)/wf`, `mcy =
height(matte_src)/hf` (floats); `w_off = layout==0 ? -mcx : 0`, `h_off = layout==1 ? -mcy : 0`; if `mcx>0 && mcy>0`:
`sx = cx/mcx`, `sy = cy/mcy`; `space = color_space(matte_src)`; if `matte_tex` format != format(space) recreate matte_tex;
if `texrender_begin(matte_tex, cx, cy, space)`: `matrix_scale3f(sx,sy,1)`, `matrix_translate3f(w_off,h_off,0)`,
`clear(COLOR, (0,0,0,0), 0, 0)`, `ortho(0,cx,0,cy,-100,100)`, render matte_src, `texrender_end`. Then (always) `prev=fb_srgb`;
enable srgb; if `gs_get_color_space() == SRGB`: `set_texture(a_tex,a)`, `set_texture(b_tex,b)`, tech `StingerMatte`; else
`set_texture_srgb` both, tech `StingerMatteLinear`; `set_texture(matte_tex, matte_tex_texture)`, `set_bool(invert_matte)`; pass ->
`draw_sprite(NULL,0,cx,cy)`; restore. (`t` unused.)
**R52. Tick.** If `track_matte_enabled`: `texrender_reset(stinger_tex)` then `texrender_reset(matte_tex)`.
**R53. Audio** `stinger_audio_render(ts_out, audio, mixers, channels, rate)`:
`ts = 0`; if media exists and `!audio_pending(media)`: `ts = audio_timestamp(media)`; if `ts == 0` **return false** (nothing
rendered). `success = obs_transition_audio_render(..., s->mix_a, s->mix_b)`; if `ts == 0` return `success`. Else:
`if (*ts_out == 0 || ts < *ts_out) *ts_out = ts`; get the media's audio mix; for every mixer bit set in `mixers` and each
channel `< channels`, add the media's 1024 samples to the output; **return true** (even if `success` was false, Q-4).
**R54. Mix curves.** fade-out/fade-in: `calc_fade(t, m) = min(t*m, 1)` (only the upper clamp);
`mixA(t) = 1 - calc_fade(t, a_mul)`, `mixB(t) = 1 - calc_fade(1-t, b_mul)`. With a_mul=4, b_mul=1.3333334: mixA(0.1)=0.6,
mixA(0.25)=0, mixB(0.25)=0.0, mixB(0.5)=0.3333; before start `a_mul = b_mul = 0` (mixA=1, mixB=1). Cross fade: `1-t`, `t`.
**R55.** `video_get_color_space` = `obs_transition_video_get_color_space(source)`. **R56.** Destroy: release media and matte;
in graphics destroy both texrenders and the effect; free.

## 6. Algorithms

### 6.1 Transition point (R43) worked example
Media duration 2.0 s, `transition_point = 500` ms: `duration_ns = 2,250,000,000`; `tp = 0.5e9 / 2.25e9 = 0.2222222`;
`a_mul = 4.5`, `b_mul = 1.2857143`; fixed duration 2250 ms. Frame mode 60 of 120 frames: `tp = 0.5`, `a_mul = b_mul = 2`.

### 6.9 Shader pixel semantics (informative; the assets define them)
fade: `lerp(A,B,fade_val)`; `Fade` additionally decodes sRGB->linear on rgb; `FadeSingle`: `A*fade_val`.
fade-to-color: `lerp(tex, color, swp)`. luma: `luma = l.x` (inverted when `invert`); `time = softness + progress*... = lerp(0, 1+softness,
progress)`; `luma <= time-softness -> B`; `luma >= time -> A`; else `lerp(A, B, (time-luma)/softness)`. slide: `uvA = uv + a_dir`,
`uvB = uv - b_dir`; if uvA outside [0,1] sample B at uvB else A at uvA. swipe: `uv' = uv + swipe_val`; outside [0,1] -> `tex_b` at `uv`, else
`tex_a` at `uv'`. stinger matte: `luma = 0.2126r + 0.7152g + 0.0722b` of the matte (inverted if set); `lerp(A,B,luma)`.

## 7. Concurrency

`create/update/destroy` run on the UI/tick thread; `video_render` and the callbacks on the graphics thread; `audio_render`
on the audio thread. The plugin has no locks: `mix_a/mix_b` read `a_mul/b_mul/switch_point` which `update`/`start` write on
another thread (torn float reads are tolerated by the reference). An implementation SHOULD publish these as atomics.
The stinger takes no part in the core's texture lock.

## 8. Reference defects and quirks

- **Q-1** (`08`, `09`): `transition_start` while `transitioning` or media `PLAYING` only restarts the media and does not
  set `transitioning`. Required: same.
- **Q-2** (`09-stinger-quirks`): frame mode with `num_frames == 0` -> `+inf` -> clamped to 0.999; `0/0` -> NaN, left as NaN
  (a_mul, b_mul NaN). SHOULD match (a tolerant implementation may clamp NaN to 0.001 but must document).
- **Q-3** (`15-mix-curves`): fade-to-color with switch point 0 or 100 evaluates `smoothstep` with `0/0`: NaN propagates
  (mixA(0)=NaN at sp=0; mixB(1)=NaN at sp=1). The vectors print `nan`.
- **Q-4** (`14`): stinger audio returns true even if the transition's audio render failed.
- **Q-5** (`04`): luma image name is not validated.
- **Q-6** (`06`): the media source is recreated on every update, and `is_track_matte` is passed with the *previous* enabled value.
- **Q-7** (`05`): swipe negation yields `-0` components.
- **D-1** (`16-defects`): `point * 1000000LL` is signed-overflow UB for `|transition_point| > 9.2e12`. Required: compute as
  `(u64)point.wrapping_mul(1_000_000)`; no crash.
- **D-2** (`16-defects`): `(u32)(duration_ns/1e6)` truncates modulo 2^32 (vector shows 705032954); required: same truncation, no panic.
- **D-3** (`05`): `swipe_create` leaves `dir/swipe_in` uninitialised until the first update; a conforming implementation
  zero-initialises (dir=(0,0), swipe_in=false) and never renders before the first update.
- No reference case crashes (`fork` cases report none): `stop`/`start` without media, missing `wipes.json`, `props` with NULL
  data, render with NULL luma texture are all defined.

## 9. Implementation notes (non-normative)

Model the graphics API as a trait; the callbacks are pure functions of `(settings, t, textures, space)` -> list of calls,
which is what the vectors check. Keep `float` (f32) arithmetic; `long double` division in R43 may be done with f64 followed
by f32 rounding except for pathological values (use `f64` and verify on the vectors). Color conversion (R21) is `u8 -> f32 /255`.

## 10. Conformance coverage

| Rules | Scripts |
|---|---|
| R01-R03 | 01-registration |
| R10 | 01-registration, 15-mix-curves |
| R11-R19 | 02-cut-fade, 15-mix-curves |
| R20-R26 | 03-fade-to-color, 15-mix-curves |
| R27-R31 | 04-luma-wipe, 01-registration (assets) |
| R32-R36, R37-R40 | 05-slide-swipe, 15-mix-curves |
| R41, R42 | 06-stinger-config, 16-defects |
| R43, R44, R45 | 08-stinger-timing, 09-stinger-quirks, 06-stinger-config |
| R46, R47-R50 | 07-stinger-properties |
| R51 | 10-stinger-render, 11-stinger-matte, 12-stinger-colorspace |
| R52 | 06-stinger-config, 11-stinger-matte |
| R53, R54 | 13-stinger-audio, 14-stinger-audio-mix |
| R55, R56 | 03-fade-to-color (cspace), 06-stinger-config |

## 11. Open questions

- Real PNG decode (16-bit, palette) is delegated to the image loader; only the shipped greyscale set is pinned.
- Behaviour of the real core helpers is only contract-level here (3.2); see the source-core spec.
