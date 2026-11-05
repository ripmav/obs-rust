# S-030 Image, colour and slideshow sources (plugin `image-source`)

| | |
|---|---|
| Status | Draft (vectors complete; Rust sufficiency check not performed, see section 10) |
| Spec version | initial |
| Reference | C implementation, libobs 33.0.1 (plugin image-source: image, colour, slideshow v1, slideshow mk2) |
| Target crate | `crates/image-source` |
| Depends on | S-014 (obs-data), S-015 (obs-properties), S-004 (signals/procs), S-008 (vec4, sRGB helpers), S-020 (graphics API surface used for drawing) |
| Conformance | `test/conformance/scripts/imagesource/` (46 scripts), runner `gen/imagesource_runner.c` |

## 1. Scope

Six registered source descriptors: `image_source`, `color_source` (v0 = "v1", v2, v3), `slideshow` (v1) and
`slideshow_v2` (mk2). Specified: settings and defaults, the state machines driven by the host callbacks
(create/update/destroy/video_tick/video_render/show/hide/activate/deactivate/media_*), recorded graphics calls,
transitions requested, signals, procedures, hotkeys, property schemas, missing-file reporting. Out of scope: the
audio path of the slideshows (copies the transition's audio mix), real image decoding, real transitions, UI.
The animated-image timing algorithm of libobs `gs_image_file_ex_tick` is specified here (section 6.4) because the
image source behaviour depends on it; the decoder itself is replaced by a scripted fake.

## 2. Conventions

MUST/SHOULD as RFC 2119. Floats printed `%.9g`; comparison is exact text (all values are f32 computations; use
f32 arithmetic where the rules say f32). `u32` arithmetic wraps. Rule ids `S-030-Rnn`.

## 3. The fake host (inputs, MUST be reproduced by the runner of an implementation)

H1 *Registry.* `obs_module_load` registers, in order: image_source, color_source v0, v2, v3, slideshow v0, v2.
Lookup id = id, or `id_vN` when version N != 0 (`color_source_v2`, `slideshow_v2`). Transition ids ending in
`_transition` (`cut_`, `fade_`, `swipe_`, `slide_`) create *fake transitions* (H6).
H2 *Sources.* Serial numbers start at 1, in creation order across all sources (private ones included). Label =
`ID#SERIAL`, for `image_source` `ID#SERIAL[FILE]` with FILE the current "file" setting. NULL label = `null`.
`obs_source_get_name` returns "" for unnamed sources. Creation: settings object is the given object (shared) or
a new empty one; then `get_defaults(settings)`; prints `ev create LABEL public|private`; then `create(settings,
source)`. Reference counted; at zero prints `ev destroy LABEL` then calls `destroy`. Weak references observe
liveness only. `obs_source_update(src, s)`: if s, apply user values of s onto the source settings (S-014
apply); for sources with the VIDEO flag only increment a defer counter, otherwise call `update` at once.
H3 *Tick of one source* (called for every live source in creation order, only sources existing at the start of the
frame; fake transitions are not ticked): (1) if CONTROLLABLE_MEDIA: pop queued media actions FIFO, call the
matching `media_*` callback and print `ev signal media_play|media_pause|media_restart|media_stopped|media_next|
media_previous LABEL`; (2) if defer counter > 0: call `update(data, settings)`, reset counter unless it changed;
(3) `show`/`hide` callback (+ `ev show|hide LABEL`) when (show_refs != 0) differs from the last callback state;
(4) same for `activate`/`deactivate` with activate_refs; (5) call `video_tick(data, seconds)`.
`seconds` = f32 given by the script; clock += llround(f64(f32) * 1e9). `obs_get_video_frame_time` = clock.
H4 *References.* `obs_source_showing` = show_refs != 0. Adding an active child (parent P, child C) adds P's
show_refs / activate_refs to C and, recursively, to C's active children (slideshow: its transition via
`enum_active_sources`; fake transition: its active sources); removing subtracts. `refs NAME S A` sets the
counts by adding the difference recursively.
H5 *Media actions* are only queued by `obs_source_media_*` (ignored when the descriptor lacks the callback or
CONTROLLABLE_MEDIA); `media_started`/`media_ended` print `ev signal media_started|media_ended LABEL`.
H6 *Fake transition* state: sources[0..1], active[0..1], transitioning, start (clock ns), duration (ns).
 - `get_time`: clock <= start -> 0; duration 0 or clock-start >= duration -> 1; else f32(elapsed / duration) in
   long double. A fresh transition has start 0, duration 0 (time 1 once clock > 0).
 - `set(t, s)`: `ev tr_set T S`; internal: ref s; release/deactivate old sources; sources[0]=s active, sources[1]
   none, transitioning=false; add s as active child.
 - `start(t, mode, ms, dest)`: if transitioning, internal set(sources[1]). same_src = dest==sources[0],
   same_dest = dest==sources[1]. If same_src and not transitioning: print `ev tr_start T mode=M ms=N dest=D ->
   ignored`, return false. Else print `-> started`; start=clock, duration=ms*1e6; if dest != sources[1]: ref new,
   if active[1] is false and new non-null: add as active child, active[1]=true, transitioning=true; replace/release
   old sources[1]. Finally if dest is null and same_dest and not same_src: transitioning=true.
 - `set_size` -> `ev tr_size T WxH`; `set_alignment` -> `ev tr_align T N`; `set_scale_type` -> `ev tr_scale T N`.
 - `get_active_source`: ref of sources[1] if transitioning else sources[0].
 - video render of a transition: t = get_time; if t >= 1 and transitioning: remove sources[0] as active child,
   sources[0]=sources[1], sources[1]=none, active=(true,false), transitioning=false, `ev tr_stop T`. Then
   `ev render T t=%g transitioning=B`, render sources[0] and (if transitioning) sources[1]. Rendering is the only
   place where a transition completes. Non-transition render: `ev render LABEL`, then `video_render(data, effect)`.
H7 *Hotkeys.* `obs_hotkey_register_source` on a public source: ids 1,2,.. in registration order, prints `ev
hotkey_register LABEL id=N name=NAME desc=DESC` (DESC = the localisation key; the fake has no translations, so
`obs_module_text(k)` returns k). Private sources: returns invalid id. `hotkey` runs the callback with `pressed`.
H8 *Task queue* (mk2): tasks are queued (`ev task_queued`) and run FIFO by the `tasks` command, immediately at
queue time with `core task_auto 1`, and all pending tasks run when the queue is destroyed (`ev task_queue_destroy
pending=N`), as the real queue joins its thread after the pending tasks.
H9 *rand.* `rand()` returns the scripted values in order and prints `ev rand -> V`; exhaustion is a script error.
H10 *File system.* `fs_file` files have an mtime (stat returns it; absent files fail with -1); `fs_dir` lists
entries in the given order (plugins do not sort); `name/` marks a sub-directory. Opening any other path as a
directory fails. `os_get_path_extension` returns the suffix starting at the last `.` of the last path component
or NULL (so `.png` and `a.b.PNG` have extensions; `noext` has none).
H11 *Image decoder.* `img` registers a still image (RGBA, 4 bytes/px). `img_anim PATH W H LOOPMAX FILESIZE
DELAYS` registers an animated GIF (delays in centiseconds, <= 64 frames). `gs_image_file_ex_init(file, alpha)`
prints `ev image_init FILE alpha=A`; clears the struct; for names ending `.gif` (case-insensitive, length > 4)
that are not registered: warning and return unloaded (no still attempt); registered with > 1 frames: animated,
`cx,cy`, format RGBA(3), `mem_usage = frames*8 + W*H*4*frames + W*H*4 + FILESIZE`, loaded; otherwise (or other
names) a registered image is a still: `mem_usage = W*H*4`, loaded; unknown: log warning, unloaded. `space` and
`alpha_mode` fields stay 0 (the reference never stores them). `init_texture` (loaded only) prints `ev
texture_create ID WxH dynamic=A` (ids from 1 in creation order); `update_texture` (animated only) prints `ev
texture_set_image ID frame=N`; `free` prints `ev texture_destroy ID` when a texture exists, then zeroes the struct.
H12 *Graphics.* Lines `gfx ...` in call order: `enable_framebuffer_srgb B`, `blend_push`, `blend_function 1 5`
(ONE, INVSRCALPHA), `set_texture_srgb PARAM tex=ID`, `draw_sprite tex=ID|-1 flip=F WxH`, `blend_pop`,
`set_vec4 PARAM x y z w`, `technique_begin NAME`, `begin_pass N`, `end_pass`, `technique_end`.
`framebuffer_srgb_enabled` / `get_linear_srgb` return the `core` inputs; `enable_framebuffer_srgb` updates the first.
H13 Colour space query of a transition returns sRGB (0).
H14 `base_size` provides `obs_get_video_info` base size (default 1920x1080).

## 4. Settings and descriptors

| id | version | flags (hex) | defaults (insertion order) |
|---|---|---|---|
| image_source | 0 | 0x8001 (icon 1) | `unload` false, `linear_alpha` false |
| color_source | 0 | 0x409 | color 0xFFFFFFFF, width 400, height 400 |
| color_source_v2 | 2 | 0x409 | color 0xFFFFFFFF, width 1920, height 1080 |
| color_source_v3 | 3 | 0x8009 | color 0xFFD1D1D1 (stored 4291940817), width 1920, height 1080 |
| slideshow | 0 | 0x2449 | transition "fade", slide_time 8000, transition_speed 700, use_custom_size "SlideShow.CustomSize.Auto", playback_behavior "always_play", slide_mode "mode_auto", loop true |
| slideshow_v2 | 2 | 0x2049 | transition "fade", slide_time 2000, transition_speed 700, use_custom_size "1920x1080", playback_behavior "always_play", slide_mode "mode_auto", playback_mode "loop" |

Authoritative flag values and callback presence are in the `info` lines of the vectors (CORE). Slideshows
define media_play_pause/restart/stop/next/previous/get_state and NO get_time/get_duration/set_time (the `info
media_extra` line is empty): the playlist has no time or duration. Icons: image 1, colour 2, slideshow 3.

## 5. Rules

### 5.1 Colour source
- S-030-R01 `update`: color = int setting as u32 (low 32 bits, `(u32)` of the 64-bit value), width/height likewise;
  width/height 0 become 1 (no other clamp; -1 becomes 4294967295). `color` bytes in memory order R,G,B,A =
  little-endian low byte first; vec4 = byte/255 (f32). `color_srgb` = same with each RGB component converted
  sRGB->linear (S-008), alpha unchanged.
- S-030-R02 `render`: linear = host linear flag OR color.w < 1. Save framebuffer-sRGB flag, enable it to `linear`,
  draw with `color_srgb` if linear else `color`, restore the flag. Draw = set_vec4 `color`, technique `Solid`
  begin, pass 0, draw_sprite(no texture, flip 0, W x H), end pass, end technique (exact lines in
  `color_render`). get_width/get_height return the stored values.
- S-030-R03 properties (in order): `color` colour+alpha, `width` int 1..4096 step 1, `height` int 1..4096 step 1.

### 5.2 Image source
- S-030-R10 `update`: file = string (never null), persistent = !unload, linear_alpha, is_slide. If is_slide: stop.
  Else if persistent or showing: unload then (if file non-empty) decode + load texture; else unload.
- S-030-R11 decode: file_timestamp = mtime or -1; init the image with alpha mode PREMULTIPLY_SRGB(2) if
  linear_alpha else PREMULTIPLY(1)... see `ev image_init` lines (alpha=2 for linear_alpha false in vectors: the
  mode numbering follows libobs enum; CORE is the mapping shown in image_basic: linear_alpha=0 -> alpha=2,
  linear_alpha=1 -> alpha=1); set file_decoded.
- S-030-R12 load texture: if already loaded return; debug log; init texture; if image not loaded warn
  `failed to load texture`; update_time_elapsed = 0; texture_loaded = true (also on failure).
- S-030-R13 unload: clear both flags, free image. `create` calls update. `destroy` unloads.
- S-030-R14 show: if !persistent and !is_slide: load. hide: if !persistent and !is_slide: unload. activate:
  restart_gif = true.
- S-030-R15 tick (seconds): if texture not loaded: if file_decoded then load texture, else return (nothing else
  happens, elapsed not advanced). frame_time = clock. update_time_elapsed += seconds. If showing and elapsed >= 1.0
  (f32): mtime = stat; elapsed = 0; if mtime != file_timestamp: full reload (unload, decode, load). Then, if
  showing: if !active: (animated: last_time = frame_time); active = true; if restart_gif: restart. Else (hidden):
  if active: restart; active = false; return (last_time is NOT updated). If last_time != 0 and animated:
  tick the image with (frame_time - last_time) and if it reports a change update the texture. last_time = frame_time.
  Note update_time_elapsed keeps growing while hidden (poll fires at once on re-show) -- Q-2.
- S-030-R16 restart (animated only): cur_frame = cur_loop = cur_time = 0, update texture, restart_gif = false.
- S-030-R17 render: if !texture_loaded or no texture return. prev = framebuffer-sRGB; enable true; blend push;
  blend_function(ONE, INVSRCALPHA); set_texture_srgb on effect param "image"; draw_sprite(tex, 0, cx, cy); blend
  pop; restore flag (lines in `image_basic`).
- S-030-R18 width/height = image cx/cy (0 when unloaded). colour space = sRGB always (the reference never sets
  `space`; Q-3). memory usage = image.mem_usage.
- S-030-R19 missing files: if file != "" and the file does not exist: one entry (path); replacing sets "file"
  to the new path in the settings and calls update (deferred). Properties: `file` path (file open, filter as in
  `props_type`), `unload` bool, `linear_alpha` bool.

### 5.3 Slideshow common
- S-030-R20 create: registers hotkeys `SlideShow.PlayPause`, `.Restart`, `.Stop`, `.NextSlide`, `.PreviousSlide`
  (descriptions = same keys), procedures `void current_index(out int current_index)` and `void total_files(out int
  total_files)`, signal `void slide_changed(int index, string path)`, then `obs_source_update(source, NULL)`
  (deferred: until the first tick width/height = 0 and no transition exists).
- S-030-R21 behaviour string (case-insensitive): `pause_unpause`, `always_play`, anything else = stop_restart.
  Mode: `mode_manual` -> manual, else auto. Transition string (case-insensitive): cut, swipe, slide ->
  `cut_transition`, `swipe_transition`, `slide_transition`, everything else `fade_transition`.
- S-030-R22 slide time: d = slide_time (u32 cast); if transition is not cut: d = max(d,100) + transition_speed; else
  d = max(d,50). `slide_time = f32(d)/1000`. (`tr_speed` = transition_speed as u32.)
- S-030-R23 folders: an item whose path opens as a directory is expanded to every non-directory entry whose
  extension (case-insensitive) is one of .bmp .tga .png .jpeg .jpg .gif .webp (Linux), in listing order, path =
  `DIR/NAME`; other items are added as-is even if the file does not exist; empty paths skipped. (An entry named
  `.png` counts.) Q-4.
- S-030-R24 custom-size string (`use_custom_size`) -- see per-version rules R31/R41; stored size is used for
  get_width/get_height only when a transition exists (else 0) and for `set_size`; alignment centre (0) and scale
  type aspect (1) are always set (`ev tr_size/tr_align/tr_scale`).
- S-030-R25 do_transition(to_null): valid = files exist (v1: and cur_item in range). If valid and use_cut: `set`
  the transition to the current slide source. Else if valid and !to_null: `start(auto(0), tr_speed, slide)`. Else:
  `start(auto, tr_speed, NULL)`, state = ENDED(5), `media_ended`. Afterwards if valid and !to_null: set calldata
  `index`, `path` and emit signal `slide_changed`.
- S-030-R26 after a (re)configuration with files: do_transition(false); state = PAUSED(4) if manual else PLAYING(1);
  `media_started`. With no files none of this happens (state untouched in v1).
- S-030-R27 media_play_pause(pause): if stopped: clear stop and paused, do_transition(false); else paused = manual =
  pause. State = PAUSED(4) if pause else PLAYING(1). media_stop: elapsed = 0, [restart/select first], do_transition
  (to_null), stop = true, paused = false, state STOPPED(5)... exact per-version in 5.4/5.5; vectors `ss?_media`.
- S-030-R28 next/previous return at once if no files or transition time < 1.0; otherwise select the next/previous
  index (wrap), or random (R32/R42), then do_transition(false). Works in any state, including stopped (Q-5).
- S-030-R29 hotkeys: callbacks act only if the source is showing and `pressed`; PlayPause -> media_play_pause(!paused);
  Restart; Stop; NextSlide / PreviousSlide additionally require `manual`. They queue media actions (run next tick).
- S-030-R30 activate/deactivate: stop_restart: activate -> restart_on_activate = use_cut = true, state PLAYING.
  pause_unpause: activate -> pause_on_deactivate = false, PLAYING; deactivate -> pause_on_deactivate = true, PAUSED.
  always_play: nothing. Procs return `cur index` (v1: cur_item; mk2: current slide idx) and `total_files`.
- S-030-R30a tick (both): return if no transition or slide_time == 0. If restart_on_activate && use_cut: elapsed = 0,
  select first (random if randomize, see R32/R42), do_transition(false), clear restart_on_activate/use_cut/stop;
  return. Return if pause_on_deactivate || manual || stop || paused. If no files and the transition has an active
  source: do_transition(true). elapsed += seconds (f32); if elapsed > slide_time: elapsed -= slide_time; if not
  looping (mk2: and not random) and current is the last index: do_transition(hide ? true : false) and return;
  else queue media_next (processed on the next tick).
- S-030-R30b render: render the transition. Colour space of the slideshow = transition's (sRGB).
- S-030-R30c missing files: one entry per non-empty item path that does not exist (not per folder expansion);
  replacing rewrites that item's `value` (empty/NULL new path erases the item), then update. Properties list in
  `props_type`: playback_behavior (list string), slide_mode, transition, slide_time (50..3600000 step 50, suffix
  " ms"), transition_speed (0..3600000 step 50), then v1: loop, hide, randomize; mk2: playback_mode (once, loop,
  random), hide; then `use_custom_size` editable list string (v1 items Auto, 16:9, 16:10, 4:3, 1:1, WxH of base
  size; mk2 only WxH), `files` editable list (files; default path = directory part of the last file incl. final
  slash, '\\' normalised to '/').

### 5.4 Slideshow v1 (`slideshow`)
- S-030-R31 update: reads settings as in R21/R22, randomize/loop/hide bools. A new transition source is created only
  if the transition name differs from the previously stored one. Builds the new file list: a source for each path,
  reusing an existing source with the same path (from the old list, then from the list being built) else creating a
  private `image_source` with file=path, unload=false; accumulates `mem_usage += image mem_usage`; stops
  expanding when mem_usage >= 400*2^20 (checked after each added file in a folder and after each item); cx,cy = max
  width/height of the sources. Old list freed after. Custom size: string != "SlideShow.CustomSize.Auto": `%dx%d` ->
  explicit (u32 cast of the ints, Q-6); else `%d:%d` -> aspect-only: old = cx/cy, new = a/b (f64); if |old-new| >
  EPSILON (0.0001): new > old ? cx = u32(cy*new) : cy = u32(cx/new); otherwise auto. (D-1 for the casts.) Then
  cur_item = 0, elapsed = 0, transition size/alignment/scale; if randomize and files: cur_item = random(R32); add the
  new transition as active child of the slideshow; R26.
- S-030-R32 random (v1): if files > 1: repeat next = rand() % count until next != cur_item. (cur_item is 0 when
  starting, so the start index is never 0 -- Q-1.)
- S-030-R33 stop: elapsed = 0, cur_item = 0, do_transition(true), stop = true, paused = false, STOPPED. restart:
  elapsed = 0, cur_item = 0, stop = paused = false, do_transition(false), PLAYING. next: random or ++ with wrap;
  previous: random or -- with wrap to last.

### 5.5 Slideshow mk2 (`slideshow_v2`)
- S-030-R41 update: `new_data` starts zeroed (paused, stop, elapsed, state, use_cut ... reset). Behaviour/mode/
  transition as R21. Migration: if `playback_mode` has no user value: randomize==true (user) -> "random"; else if loop
  has a user value -> "loop"/"once" by its value; the result is written into the source settings. randomize =
  (mode=="random"), loop = (mode=="loop"); any other string = neither (plays once). A new transition source is created
  on EVERY update (the stored name is never recorded; Q-7). Files: R23 without a memory cap; no sources are created
  yet. Size: `sscanf "%u" "x" "%u"`: both parsed -> that size; else 0x0 (also for "Auto" or "16:9"; negative ints wrap, Q-6).
  After replacing the data the previous data (including all slide sources) is freed, THEN `restart_slides` runs, so no
  source is reused across updates. Then size/alignment/scale on the transition, add the new transition as active child,
  R26.
- S-030-R42 random (mk2): next = idx; if files > 1 loop while next == idx: draw r = rand() until r >= (2^64 mod
  count) (as size_t), next = r % count.
- S-030-R43 restart_slides: start = randomize ? rand() % count (plain modulo, may be 0) : 0. cur = source(start);
  next buffer: 5 times idx = get_new_file(idx, forward) -> source; prev buffer: 5 times backwards, pushed to the
  front (so prev[4] is the entry closest to cur). get_new_file: random -> R42, forward -> idx+1 wrap, backward ->
  idx-1 wrap to last. A source is reused if the same index is in the previous slides (cur, prev, next) or already
  in the list being built (one ref per use), else a new private image_source is created with is_slide=true and a
  decode task is queued (`ev task_queued`). The old buffers are released afterwards. With no files all are empty.
- S-030-R44 restart: restart_slides, elapsed = 0, stop = paused = false, do_transition(false), PLAYING. stop: restart_slides,
  elapsed = 0, stop = true, paused = false, do_transition(true), STOPPED.
- S-030-R45 next: take last = next[4]; idx = random(R42) or last+1 wrap; new source = reuse-or-create (against current
  slides only); push to next back; push cur to prev back; pop next front to cur; pop prev front and release; then
  do_transition(false). previous: first = prev[0]; idx = random or first-1 wrap to last; push to prev front; push cur to
  next front; pop prev back to cur; pop next back and release; do_transition.
- S-030-R46 image decode task: for the weak source, if alive, run the image source's `preload` (decode, sets
  file_decoded); slide image sources then load their texture in their own next tick (R15). is_slide sources skip
  loading in update and never react to show/hide.

## 6. Algorithms

6.1 f32 note: `elapsed += seconds` and `elapsed -= slide_time` are f32 operations; compare `elapsed > slide_time` in f32.
6.2 Tick order and the media queue (H3) make `next` take effect one tick after the elapsed threshold is passed.
6.3 Transition completion needs a render (H6): `get_active_source` keeps returning sources[1] until a render.
6.4 Animated image tick (image decoder contract, `gs_image_file_ex_tick(elapsed_ns)`): not animated/not loaded ->
no change. loops = loop_max, if >= 65535 then 0. if loops == 0 or cur_loop < loops: cur_time += elapsed; nf = cur_frame;
loop { t = delay(nf)*1e7 ns (0 -> 1e8); if cur_time <= t break; cur_time -= t; nf += 1; if nf == frames { if loops == 0 or
++cur_loop < loops { nf = 0 } else if cur_loop == loops { nf -= 1; break } } }; if nf != cur_frame: cur_frame = nf,
return true (caller updates the texture) else false. Note the comparison `<=`: exactly t elapsed stays on the frame.

## 7. Concurrency
v1 guards files/transition with a mutex taken by tick/update/render accessors; mk2 has none and relies on the host calling
update/tick/media actions on one thread; decode runs on a background queue and only sets atomics. A Rust port may use any
equivalent guarantee; observable results are as in the single-threaded fake.

## 8. Reference defects and quirks
- Q-1 v1 random start never picks index 0 with >= 2 files (`ss1_random`); mk2 may (`ss2_random`).
- Q-2 image `update_time_elapsed` and `last_time` stale while hidden (`image_poll`, `image_gif_loops`).
- Q-3 image colour space is always sRGB; `space`/`alpha_mode` fields never set (`image_basic`).
- Q-4 folder listings are not sorted: order is host-dependent; entries named `.png` count; the plain-file item that is
  also in a folder is deduplicated by v1 only (`ss1_dirs`, `ss2_dirs`).
- Q-5 next/previous work while stopped (`ss1_media`, `ss2_media`); v2 update resets paused/stop/elapsed/state (`ss2_quirks`);
  v1 does not (`ss1_quirks`).
- Q-6 `%dx%d` ints are cast to u32 (negative wraps: "-5x7" -> 4294967291x7) (`ss*_size`).
- Q-7 mk2 never stores the transition name, creates a new transition on every update (`ss2_transitions`).
- D-1 (`ss1_defect_aspect`) v1 aspect-only size with a zero or negative component converts inf / a negative double to
  u32 (undefined behaviour; UBSan float-cast-overflow reports it; no crash). Required: never crash, no trap; the converted
  value = truncate toward zero to i64 then wrap to u32, and 0 for non-finite values. Vector lines for this script follow that.
- No crash was found by the fork-per-case probes (`ss1_probe`, `ss2_probe`). A clock moving backwards makes the gif tick
  loop run for ~2^64/delay iterations; the host clock MUST be monotonic (not tested).

## 9. Coverage

| Rules | Scripts |
|---|---|
| R01-R03 | color_basic, color_edges, color_render, props_type |
| R10-R19 | image_basic, image_unload, image_poll, image_gif, image_gif_loops, image_missing, props_type |
| R20-R22, R26 | ss1_basic, ss2_dirs, ss1_timing, ss?_transitions, ss?_size |
| R23 | ss1_dirs, ss2_dirs |
| R25, R27-R30b | ss?_media, ss?_notcomplete, ss?_hotkeys, ss?_behavior, ss?_loop_hide, ss?_probe, ss?_missing, ss?_props |
| R31-R33 | ss1_reuse, ss1_random, ss1_random_single, ss1_mem_cap, ss1_quirks, ss1_size, ss1_defect_aspect |
| R41-R46 | ss2_reuse, ss2_random, ss2_playback_mode, ss2_tasks, ss2_quirks |

## 10. Open questions and limitations
- The callback list per descriptor is in the vectors (`info` lines).
- The Rust sufficiency check was NOT performed (usage limit); the spec has not been validated by an independent
  implementation. Expect gaps in H3/H6 details and in the obs-data coercion rules (int<->double only).
- Log text is not specified except levels (warning for load failures).
- The audio path, real decoding and real transition rendering are not specified.

## 11. Round 9: file ownership of the four plugin sources (added)

§5 pins all four sources at behaviour level; the plugin's `.c` files themselves were not cited. This section
maps each file to the rules that own it and pins the struct layouts (the state those rules manipulate).

### Reference files (all cited)

- `plugins/image-source/image-source.c`, `plugins/image-source/color-source.c`, `plugins/image-source/obs-slideshow.c`, `plugins/image-source/obs-slideshow-mk2.c`

- **S-030-R47 [SRC]** `image-source.c` (24 fns): `struct image_source {source, char *file, bool persistent, is_slide, linear_alpha, time_t file_timestamp, float update_time_elapsed, uint64 last_time, bool active, restart_gif, volatile bool file_decoded, volatile bool texture_loaded, gs_image_file_ex_t image}`. Owned by R10–R19; note the two **volatile** flags: `file_decoded` is written by the decode task (R46) and read by the tick (R15), `texture_loaded` by the tick.
- **S-030-R48 [SRC]** `color-source.c` (11 fns): `struct color_source {vec4 color, vec4 color_srgb, uint32 width, height, src}`. All three descriptors live in this file: v0 and v2 flags `VIDEO|CUSTOM_DRAW|CAP_OBSOLETE`, v3 `VIDEO|CUSTOM_DRAW|SRGB`; owned by R01–R03.
- **S-030-R49 [SRC]** `obs-slideshow.c` (40 fns): `struct image_file_data {char *path, obs_source_t *source}`; `struct slideshow {source, bools randomize/loop/restart_on_activate/pause_on_deactivate/restart/manual/hide/use_cut/paused/stop, float slide_time, uint32 tr_speed, const char *tr_name, transition, calldata_t cd, float elapsed, size_t cur_item, uint32 cx, cy, uint64 mem_usage, pthread_mutex_t mutex, image_file_array_t files, enum behavior, five obs_hotkey_id, enum obs_media_state state}`. v1 guards `files` and the transition with the mutex (taken by tick, update and the size accessors; §7); owned by R20–R33.
- **S-030-R50 [SRC]** `obs-slideshow-mk2.c` (44 fns): `struct image_file_data {char *path}` (no source — the slide sources live in the buffers); `struct source_data {size_t slide_idx, const char *path, obs_source_t *source}`; `struct active_slides {deque prev, deque next, source_data cur}` with five entries per deque as R43 orders them (`prev[0]` farthest back, `next[4]` farthest ahead); `struct slideshow_data {slides, files, float slide_time, uint32 tr_speed, const char *tr_name, bools manual/randomize/loop/restart_on_activate/pause_on_deactivate/restart/hide/use_cut/paused/stop, calldata_t cd, float elapsed, enum behavior, state}`; `struct slideshow {source, data}`. mk2 holds no mutex of its own (§7); owned by R41–R46.
