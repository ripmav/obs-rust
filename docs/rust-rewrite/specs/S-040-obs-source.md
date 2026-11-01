# S-040 Source core: lifecycle, activation, filter chain, async video, deinterlace, transitions (`obssource`)

| | |
|---|---|
| Status | Draft (vectors generated and stable; Rust sufficiency check NOT performed, see section 11) |
| Spec version | initial |
| Reference | C implementation, libobs 33.0.1: `obs-source.c`, `obs-source-deinterlace.c`, `obs-source-transition.c` (implementers do not read them) |
| Target crate | `crates/obs-source` |
| Depends on | S-002 (calldata), S-004 (signals and procs), S-005 (darray: `move_item`, `erase`, `insert`), S-014 (obs_data), S-013 (video formats), S-032 (audio side of sources: ingest, mixing, volume, mute, monitoring), S-033 (scene math), S-029 (consumes `obs_transition_*` helpers) |
| Conformance | `test/conformance/scripts/obssource/` (51 scripts; runner `gen/obssource_runner.c`, host fake `gen/obssource_stubs.c`, registration `gen/obssource.sources`) |

## 1. Scope

In scope (observable behaviour of the source object as seen by plug-ins, the frontend and the render loop):

* creation, defaults, reference counting, weak references, removal, deferred destruction, rename, enable, flags, update;
* show/active reference counting, the tick-time callbacks and the active-child tree;
* the filter chain: add, remove, ordering, targets, compatibility, copy/duplicate, backup/restore, size resolution;
* async video: ingest, frame cache, pending queue, timing/selection, unbuffered mode, preload/show, range and rotation,
  async filters; deinterlace configuration and the two-frame selection;
* the transition core (`obs_transition_*`): state machine, time computation, manual mode, child matrices, audio
  mixing, completion, save/load, swap; the media-control queue.

Out of scope (listed in section 11 with reasons): GPU rendering paths (`obs_source_video_render`, effects, texture
upload, colour-space conversion, `obs_source_process_filter_*`, `obs_source_draw`), audio ingestion/mixing/volume/
mute/monitoring/push-to-talk (S-032), hotkey registration, the object-context layer (name uniqueness, UUIDs, lookup by
name, canvases; this is `obs.c`/`obs-scene.c`), `obs_source_save/load` of settings, properties, interaction events,
scene and group sources (S-033), missing-files, profiler.

Callers: plug-ins (create callbacks, `obs_source_output_video`, `obs_source_filter_*`, `obs_transition_*` helpers),
the render loop (`obs_source_video_tick`), the frontend (everything else).

## 2. Terminology and conventions

RFC 2119 words apply. Timestamps and clocks are `u64` nanoseconds; **all timestamp subtractions/additions are wrapping**
(unsigned C). "Array order" of a filter list means the stored array: index 0 is the **newest** filter. "Fake host" =
the environment of the reference runner (section 3.4). Float tolerance: relative 1e-5 for printed `%.9g` values; all
other output is exact. Log lines: only the level is compared; texts quoted below are informative except where marked MUST.
Constants: `MAX_TS_VAR = 2_000_000_000` ns, `MAX_ASYNC_FRAMES = 30`, `MAX_UNUSED_FRAME_DURATION = 5`,
`TWOX_TOLERANCE = 1_000_000`, `TS_JUMP_THRESHOLD = 70_000_000`, frame smoothing window 2_000_000 ns,
`AUDIO_OUTPUT_FRAMES = 1024`, `MAX_AUDIO_MIXES = 6`, `MAX_AUDIO_CHANNELS = 8`.

Enumerations (numeric values are part of the contract):

| Enum | Values |
|---|---|
| source type | INPUT 0, FILTER 1, TRANSITION 2, SCENE 3 |
| output flags | VIDEO 1, AUDIO 2, ASYNC 4 (ASYNC_VIDEO = 5), CUSTOM_DRAW 8, COMPOSITE 64, DO_NOT_DUPLICATE 128, CONTROLLABLE_MEDIA 8192 |
| view type | INVALID 0, MAIN 1, AUX 2 |
| order movement | UP 0, DOWN 1, TOP 2, BOTTOM 3 |
| transition target | A 0, B 1 |
| transition mode | AUTO 0, MANUAL 1 |
| transition scale | MAX_ONLY 0, ASPECT 1, STRETCH 2 |
| align bits | CENTER 0, LEFT 1, RIGHT 2, TOP 4, BOTTOM 8 |
| deinterlace mode | DISABLE 0, DISCARD 1, RETRO 2, BLEND 3, BLEND_2X 4, LINEAR 5, LINEAR_2X 6, YADIF 7, YADIF_2X 8 |
| field order | TOP 0, BOTTOM 1 |

## 3. Interface

### 3.1 Plug-in kind (`obs_source_info`, only the fields this spec uses)

`id`, `type`, `output_flags`, `get_name`, `create(settings, source) -> data|NULL`, `destroy(data)`, `update(data, settings)`,
`get_defaults(settings)`, `get_width/get_height(data)`, `activate/deactivate/show/hide(data)`, `video_tick(data, seconds)`,
`enum_active_sources(data, cb, param)`, `enum_all_sources`, `filter_video(data, frame) -> frame|NULL`,
`filter_add/filter_remove(data, parent)`, `transition_start/transition_stop(data)`, `media_*`. Every callback is called
only if the source's `data` is non-NULL (except `create`, `get_defaults`), and only if present.

### 3.2 Functions (all take a source handle; invalid/NULL handle: debug log, no-op, return 0/false/NULL)

Lifecycle: `create(id, name, settings, hotkey_data)`, `create_private`, `addref`, `release`, `get_ref`,
`get_weak_source`, `weak_get_source`, `weak_expired`, `remove`, `removed`, `set_name`, `get_name`, `get_id`, `get_type`,
`enabled/set_enabled`, `get_flags/set_flags/set_default_flags`, `is_hidden/set_hidden`, `update`, `reset_settings`,
`update_properties`, `get_settings`.
Activation: `activate(view)`, `deactivate(view)`, `inc_showing/inc_active/dec_showing/dec_active`, `showing`, `active`,
`add_active_child`, `remove_active_child`, `enum_active_sources/tree`, `enum_full_tree`, `video_tick(seconds)`.
Filters: `filter_add/remove/set_order/set_index/get_index`, `filter_count`, `enum_filters`, `get_filter_by_name`,
`filter_get_target`, `copy_filters`, `copy_single_filter`, `duplicate`, `backup_filters`, `restore_filters`,
`get_width/get_height`.
Async video: `output_video(frame)`, `output_video2(frame2)`, `preload_video(2)`, `show_preloaded_video`,
`set_video_frame(2)`, `get_frame`, `release_frame`, `set_async_unbuffered`, `set_async_rotation`.
Deinterlace: `set/get_deinterlace_mode`, `set/get_deinterlace_field_order`.
Transitions: `obs_transition_start/set/clear/get_time/get_source/get_active_source/is_active/set_manual_time/
set_manual_torque/enable_fixed/fixed/set_scale_type/get_scale_type/set_alignment/get_alignment/set_size/get_size/
video_render/video_render_direct/audio_render/swap_begin/swap_end/force_stop/save/load/enum_sources`.
Media: `media_play_pause/restart/stop/next/previous/set_time/get_duration/get_time/get_state/started/ended`.

### 3.3 Signals (source handler unless "global")

Source: `destroy remove update save load activate deactivate show hide mute enable rename volume update_properties
update_flags audio_sync filter_add filter_remove reorder_filters transition_start transition_video_stop transition_stop
media_play media_pause media_restart media_stopped media_next media_previous media_started media_ended` (plus
audio-related ones from S-032). Parameters used here: `source` (ptr); `filter` (ptr) on filter_add/remove; `enabled`
(bool) on enable; `flags` (int) on update_flags; `new_name`, `prev_name` (string) on rename.
Global (`obs->signals`): `source_create source_destroy source_remove source_update source_activate source_deactivate
source_show source_hide source_rename source_filter_add source_filter_remove source_transition_start
source_transition_video_stop source_transition_stop`. A global signal is sent only for **non-private** sources, **except**
`source_filter_add`/`source_filter_remove` which are sent unconditionally (filters are always private).
When both are sent the **global one comes first**.

### 3.4 Host services (the "fake host" of the reference runner; not specified further)

A conforming implementation needs these from its surroundings; the runner defines their behaviour: the object context
(name storage, UUID, settings object created from `obs_data`, signal handler creation; **name uniqueness is the
context layer's job, not specified here**), the strong/weak reference counter, a task queue for deferred destruction
(the runner runs it only on the script command `drain`), the clock `os_gettime_ns` (scripted by `clock`), the video clock
`video_time` and `video_half_frame_interval_ns` (scripted by `vtime`, `half`), a graphics layer (all calls succeed and
draw nothing), a registry of public sources searched by name (`obs_get_source_by_name`), `obs_save_source`/`obs_load_source`
(runner: saves id and name; loads a private source of that id/name), the source-kind table.

## 4. Data formats

No byte formats. The two "formats" with worked examples are the runner's `state`/`astate`/`tr` dump lines (README) and
the transition save record (a settings object with the keys `transition_source_a` (string: name of the active child or
`""`), `transition_alignment`, `transition_mode`, `transition_scale_type`, `transition_cx`, `transition_cy` (ints)).
Example (script 59): `tr_save t source_a="A" align=9 mode=0 scale=2 cx=800 cy=600`.

## 5. Behaviour (numbered rules)

### 5.1 Creation, defaults, destruction

**S-040-R01** `create(id, name, settings, hotkey)`: if `id` is not registered, log ERROR `Source ID '<id>' not found`; the
source is nevertheless created, with `info.id = id` (copied), type INPUT, output flags 0, and `create` returns it
(script 03 `u`). The "not found" source accepts any later call (checks that look at `output_flags` are bypassed, see R42, R60).
**R02** A source of type FILTER is always created private, whatever the argument. Private sources do not send the global
create/destroy/remove/rename/update/activate/show signals nor are they found by name.
**R03** Initial state: `enabled=true`, `removed=false`, hidden=false, `show_refs=activate_refs=0`, `showing=active=false`,
`flags = default_flags = 0` (unless the plug-in sets them), `user_volume=1`, `balance=0.5`, `audio_mixers=0xFF`
(`get_audio_mixers` reports 0 for a registered kind without the AUDIO flag), deinterlace mode DISABLE, field order TOP,
`async_active=false`, `last_frame_ts=0`. One strong reference belongs to the caller.
**R04** Default values from `get_defaults` are installed as **defaults** in the settings (do not override explicit values) before
`create` runs; `reset_settings` clears explicit values only, defaults remain (script 62).
**R05** `create` callback is called exactly once, after defaults are set. If the kind has a `create` callback and it returns
NULL, log ERROR `Failed to create source '<name>'!`; the source is still returned and registered; `data` stays NULL and every
later callback is skipped (script 03 `b`).
**R06** After `create` the source is registered, then a DEBUG line `[private ]source '<n>' (<id>) created` is logged BEFORE
registration, then signal `source_create` (global, non-private sources only).
**R07** `release`: the last release (strong count 1 -> 0) runs the **immediate part** of destruction at once (R08); extra
`addref`/`release` pairs only change the count. Releasing a source that is already destroying logs ERROR (double destroy).
**R08** Immediate destruction, in this order: mark destroying (ingest calls now return, R61); free audio/caption callback
lists; if a transition: `clear` (R86); unlink from the audio-source list; if the source is itself a filter attached to a parent:
remove it from the parent **without** releasing (R43 signals fire); then remove every own filter, always taking the one at
array index 0 (so newest first), each with R43 signals/callbacks and a release; unregister name/UUID; queue the **deferred part**.
**R09** Deferred part (runs later, never inside a tick): signals `source_destroy` (global, non-private) then `destroy`
(source); `destroy(data)` if `data` non-NULL; then free everything; log DEBUG `source '<n>' destroyed`.
Deferred parts run in queue order (script 04: F2, F1, A).
**R10** `get_ref` returns a new strong reference, or NULL if the strong count already dropped to zero (it never resurrects).
Weak handles: `get_weak_source` adds a weak count; `weak_get_source` = get_ref; `weak_expired` is true after the last
strong release (scripts 02).
**R11** `remove`: sets `removed=true` and sends `source_remove` + `remove` once; a second call is a no-op. `removed` of an
invalid handle returns true.
**R12** `set_name(n)`: if `n` is NULL/empty or differs from the current name, rename through the context layer, then send
`source_rename` (global, non-private only) and `rename` with `new_name`, `prev_name`. Same non-empty name: nothing (script 06).
**R13** `set_enabled(b)` stores `b` and **always** sends `enable` (`enabled=b`) even if unchanged; there is no global signal.
**R14** `set_flags(f)`: only if `f` differs, store and send `update_flags(flags=f)`. `set_default_flags` stores silently.
**R15** `set_hidden/is_hidden` read/write a plain boolean (no validity check; see D-2).
**R16** `update(settings)`: if `settings` non-NULL it is merged into the source settings. If the kind has the VIDEO flag the
callback is **deferred**: `defer_update_count++`; otherwise, if `data` and `update` exist, call `update(data, settings)` then send
`source_update` (global, non-private) and `update` (source). `reset_settings` = clear explicit settings + `update`.
`update_properties` only sends `update_properties`. At a tick with `defer_update_count > 0` the callback runs once (all queued
updates collapse), the counter is reset (compare-and-swap: only if unchanged meanwhile) and the same two signals are sent, only if
`data` and `update` exist (script 62).
**R17** `get_settings` returns an extra reference to the settings object.

### 5.2 Activation counting and the tick

**R20** State per source: `show_refs`, `activate_refs` (signed), `showing`, `active` (booleans changed only by the tick).
`obs_source_showing` = `show_refs != 0`, `obs_source_active` = `activate_refs != 0`.
**R21** `activate(src, view)`: `show_refs += 1` for `src` and for every source in `src`'s **active tree** (R25); if `view == MAIN`
also `activate_refs += 1` for `src` and the tree. `inc_showing` = activate AUX, `inc_active` = activate MAIN.
**R22** `deactivate(src, view)`: if `show_refs > 0` then `show_refs -= 1` and the tree's `show_refs -= 1` (tree members are
decremented unconditionally, they can go negative if used inconsistently); if `view == MAIN` and `activate_refs > 0` then
`activate_refs -= 1` and the tree likewise. The two checks are independent: MAIN deactivation with `show_refs == 0` still decrements
`activate_refs` (script 11). `dec_showing`/`dec_active` = deactivate AUX/MAIN.
**R23** Callbacks run in the tick, never at the call. At `video_tick`, if `(show_refs != 0) != showing`: call `show` (or `hide`)
on the plug-in, send `source_show|source_hide` (global) and `show|hide`, then do the same (callback + signals, `show`/`hide`
only) for each filter from the **highest array index down to 0** (oldest filter first); then `showing := show_refs != 0`. The same
for `active` with `activate/deactivate` and `source_activate|source_deactivate`. Show/hide is processed before activate/deactivate.
Net-zero changes between two ticks produce nothing (script 12). A source does not get callbacks for the same transition twice.
**R24** Tick order inside `video_tick(seconds)`: (1) transition tick (R80) if a transition, (2) async selection (R70) if the
ASYNC flag, (3) queued media actions (R90) if CONTROLLABLE_MEDIA, (4) deferred update (R16), (5) show/hide, (6) activate/
deactivate, (7) the plug-in `video_tick(data, seconds)` (if `data` and callback), (8) per-frame render flags cleared.
**R25** The **active tree** of `s`: the transition's A and B children (slot order, existing ones only) followed by `s`'s
`enum_active_sources` callback output (if `data` non-NULL), recursively: for each child the enumeration of its own children comes
**before** the callback is invoked for the child (post-order; script 61 prints `(g>l) (t>g)`). `enum_active_sources` (non-tree)
reports only direct children. `enum_full_tree` uses `enum_all_sources` if the kind has it, else `enum_active_sources`. A source that
is neither a transition nor has an enumeration callback enumerates nothing.
**R26** `add_active_child(parent, child)`: NULL -> false. `parent == child`: WARNING, false. If the **full tree of child** contains
`parent` (as child or as parent of any pair), false. Otherwise replay the parent's counts on the child: for `i` in `0..parent.show_refs`
call `activate(child, MAIN if i < parent.activate_refs else AUX)`; return true. (Script 15.)
**R27** `remove_active_child(parent, child)`: for `i` in `0..parent.show_refs`, `deactivate(child, MAIN if i < activate_refs else AUX)`.

### 5.3 Filter chain

State: `filters` array (index 0 newest), each filter has `parent` and `target` (the next array element, or the source for the last).
**R40** `filter_add(source, filter)`: NULL checks. If the filter is already in the array: WARNING `Tried to add a filter that was
already present on the source`, nothing else. If the source is a *registered* kind and `!compatible(source, filter)`: silently
refuse (no log, no signal). Otherwise take a strong reference on the filter (get_ref), `filter.parent = source`,
`filter.target = array[0]` or `source` if the array is empty, **insert at index 0**.
**R41** Then, in this order: global `source_filter_add`, source `filter_add` (both with `source`,`filter`), DEBUG log, then the filter's
plug-in `filter_add(data, parent)` callback (if data and callback).
**R42** `compatible(s, f)`: `caps(x) = x.output_flags & (VIDEO | ASYNC | AUDIO)` (VIDEO is part of the mask!). If `caps(f)` has AUDIO
and not VIDEO, clear its ASYNC bit. Compatible iff `caps(s) & caps(f) == caps(f)`. (Script 21 prints the matrix.)
**R43** `filter_remove(source, filter)`: if not in the array, nothing (returns). Else: if `idx > 0`, `array[idx-1].target := filter.target`;
erase `idx`; send global `source_filter_remove`, source `filter_remove`; DEBUG; plug-in `filter_remove(data, parent)`; then
`filter.parent = filter.target = NULL`; finally release the chain's reference (the filter may be destroyed here, deferred).
**R44** `set_order(source, filter, mv)`: not found -> nothing. UP: find the nearest **higher** index whose ASYNC flag equals the
filter's; none -> no-op. DOWN: nearest **lower** index with equal ASYNC flag. TOP: move to the last index (no-op if there). BOTTOM:
move to index 0 (no-op if there). A move is `move_item(from, to)` (remove then insert at `to`). After a successful move all `target`
pointers are recomputed (`array[i].target = array[i+1]`, last -> source) and `reorder_filters` is sent; a no-op sends nothing.
**R45** `set_index(source, filter, i)`: not found -> nothing; else `move_item(idx, i)`, recompute targets, send `reorder_filters`
(even if the position did not change). `i >= count` is a reference defect (D-1). `get_index` returns the array index or -1.
**R46** `filter_count`, `enum_filters` (visits array indices from highest to 0, i.e. oldest first), `get_filter_by_name` (first match
in array order, returns a **new** strong reference or NULL; name compare is exact, case-sensitive), `filter_get_target`.
**R47** `get_width/height(source)` for a non-filter: if it has filters use the width/height of `array[0]`, else of itself. `base(x)`:
transition -> `enabled ? actual_size : 0`; else if `x` has `data` and a size callback and (`x` is not a filter or is enabled) -> callback;
else if `x` is a filter -> `base(x.target)`; else if `async_active` -> async frame size (swapped when `rotation % 180 != 0`, C remainder,
so -90 swaps) else 0. For a filter, `get_width` = `base(filter)` (script 27).
**R48** `copy_filters(dst, src)` / `copy_single_filter(dst, f)` / `duplicate`: filters are duplicated **oldest first**; each copy is
named `name`, or `name 2`, `name 3`, ... (first free) among `dst`'s current filters, created private (or non-private only when
duplicating a non-private source), has the same id and a copy of the settings, keeps the enabled flag, and is added with the normal
R40 path (compatibility rules apply; `copy_single_filter` silently skips an incompatible one). `duplicate(src, name, private)`:
DO_NOT_DUPLICATE kinds return a new reference to the same source; scenes are out of scope; otherwise create a new source of the same
id with copies of the settings, `audio_mixers, sync_offset, user_volume, user_muted, volume, muted, flags`, private settings, and (for
non-filters) its filters (script 25).
**R49** `backup_filters` returns one record per filter in array order (the runner's record holds id and name). `restore_filters(src, rec)`:
detaches every current filter (`parent=target=NULL`) **without** filter_remove callbacks or signals and empties the array; for each
record in order: a detached filter with the same name is reused, else the filter is loaded (new object); the new array has the record
order, parents/targets are rewired; **no** filter_add callbacks or signals; finally the reference of every old filter is released
(script 71: a filter reused keeps its single chain reference; one not in the backup is released).

### 5.4 Async video

State: `async_frames` (queue, oldest at index 0), `async_cache` (entries `{frame, used, unused_count}`), `cur_async_frame`,
`prev_async_frame`, `last_frame_ts`, `last_sys_timestamp`, `async_active`, texture description `{w,h,format,full_range,trc}`,
`async_unbuffered`, `async_rotation`, cache description `{cache_w, cache_h, cache_format, cache_full_range, cache_trc}`, `preload_frame`.
**Convert class** of `(format, full_range, trc)`: I420 -> 420 (PQ variant if trc is PQ); NV12; I444; I412; I422; I210; YVYU/YUY2/UYVY
-> 422_PACK; Y800; NONE/RGBA/BGRA/BGRX -> `NONE` if `full_range` else `RGB_LIMITED`; BGR3; I40A, I42A, YUVA, YA2L, AYUV, I010, P010, V210,
R10L each their own; P216/P416 -> `NONE`. A frame needs "GPU conversion" iff its class is not `NONE`. Two formats of the same class
(e.g. BGRA and RGBA, both full range) are not a texture change.
**R60** `output_video(frame)`: ignored if the source is destroying. `frame == NULL`: `async_active=false`, `last_frame_ts=0`, free the
whole cache, empty the queue, `cur=prev=NULL`. Else make a copy `f'` with `full_range = true` if the format is **not** YUV (otherwise
as given), and run R61.
**R61** (cache_video) If the queue already holds 30 frames: free cache and queue, `cur=prev=NULL`, `last_frame_ts := 0`, the new frame
is **dropped** (script 35). If the frame's `(w,h,class)` differs from the cache description: free cache/queue/cur/prev and store the new
`(w,h)`; always store the frame's format/range/trc in the cache description. Pick the **first unused** cache entry (set used, unused_count 0),
else allocate a new entry (used). Then for every entry still unused: `unused_count += 1`, and when it reaches 5 free the entry
(`clean_cache`, applied to all unused entries after the pick, iterating from the last to the first). Copy the pixel planes, timestamp, flip,
flags, trc, full_range, colour data into the entry (the copy preserves the source's plane geometry: chroma planes use `(h+1)/2` rows for
420/NV12). Append the entry's frame to the queue; `async_active := true`.
**R62** `output_video2(frame2)`: same, but `full_range := resolve_range(format, range)` where DEFAULT -> limited for YUV and full for RGB,
PARTIAL -> false, FULL -> true. RGB is **not** forced to full (Q-5). `max_luminance := 0`.
**R63** `preload_video(frame)`: keep one private frame (re-allocated if `(w,h,format)` changed), copy into it, `last_frame_ts := frame.timestamp`.
Nothing enters the queue/cache and `async_active` is unchanged. `show_preloaded_video`: no preload frame -> nothing; else update the texture
description from it, `async_active := true`, reset the audio timing to `(last_frame_ts, now if not monitor-only else 0)` and clear the audio
buffer (S-032). `set_video_frame(frame)`: like preload plus immediate texture update, but **does not** set `async_active` (Q-7).
**R64** Size: `get_width/height` of a plain async source = `async_active ? texture size (swapped for rotation) : 0`; the texture description
is updated by the **tick** that selected a frame (or `show_preloaded`/`set_video_frame`), not by ingest (script 30).
**R65** Selection per tick, without deinterlacing (`async_tick`): `sys = video_time`. (a) if `cur != NULL`: mark its cache entry unused
(`prev_frame:=false`), `cur := NULL`. (b) `cur := closest(sys)` (R66). (c) `last_sys_timestamp := sys`. (d) if `cur != NULL`, run the async filters
(R69) on it (the result may be NULL); (e) if `cur != NULL` update the texture description from it. (Script 31 prints `cur=-` on ticks without a
new frame: nothing is shown, the previous picture stays.)
**R66** `closest(sys)`: if the queue is empty -> NULL. If `last_frame_ts == 0` **or** `ready(sys)`: pop queue[0]; if `last_frame_ts == 0` set it to
that frame's timestamp; return it. Else NULL. (First frame after a start is shown at once, whatever its timestamp.)
**R67** `ready(sys)` pseudocode (section 6.1). Summary: the playback clock `last_frame_ts` advances by `sys - last_sys_timestamp` per tick; a frame is
due when the clock is **strictly greater** than its timestamp; of several due frames the **latest** is chosen unless the next one is within 2 ms
of the clock (then the earlier is kept to avoid duplicates); a timestamp more than 2 s away (either direction) from the clock re-synchronises
the clock to the frame and the frame is returned immediately. Unbuffered sources drop all but the newest queued frame and return it, setting the
clock to its timestamp (Q-6: not on the first selection, which goes through R66's `last_frame_ts == 0` branch).
**R68** `get_frame(source)`: takes `cur` (sets `cur := NULL`) and increments its refcount; NULL if none. `release_frame(source, f)`: NULL frame ->
nothing; NULL source -> destroy frame; else decrement; at zero destroy, else mark the cache entry unused (script 30).
**R69** `filter_frame`: refcount++, pass through each **enabled** filter with a `filter_video` callback and `data`, from highest array index to 0
(oldest first); a NULL result stops the chain and the source has no current frame this tick; refcount--. (Script 39: the second filter is not
called after the first drops the frame; a disabled filter is skipped.)
**R70** `set_async_unbuffered/async_unbuffered`, `set_async_rotation` (accepts any `long`; ignored on NULL source) are plain setters.

### 5.5 Deinterlace

**R100** `set_deinterlace_mode(s, m)`: equal -> nothing; DISABLE -> m: `enable` (set the mode, drop `prev_async_frame` marking it unused,
allocate previous textures when a texture description exists); m -> DISABLE: `disable` (mode := DISABLE, free previous textures); other
transitions only change the mode (any integer is accepted and stored, including out-of-range values, script 41). `get_*` return the stored
values. `set_deinterlace_field_order(s, o)`: stores `top_first = (o == TOP)` (any other value selects BOTTOM).
**R101** With deinterlacing enabled the tick (R65) does not use R66; instead: mark `prev`/`cur` unused, set both NULL, then `get_closest_frames(sys)`
(section 6.2), `last_sys_timestamp := sys`, run R69 on `prev` and `cur`, update the texture description from `cur`.

### 5.6 Transitions

State: slots `sources[0..1]`, `active[0..1]` (does the transition currently hold an active-child link on that slot), `transitioning_video`,
`transitioning_audio`, `mode`, `start_time`, `duration`, `fixed_enabled`, `fixed_ms`, `manual_val`, `manual_target`, `torque`, `clamp`, `scale_type`,
`alignment`, `set_size (cx,cy)`, `actual_size`, `matrices[2]`. Initial: no children, `active[0]=true`, `alignment = LEFT|TOP`, `scale = MAX_ONLY`.
The transition holds one strong reference per child.
**R80** `tick(seconds)` (R24 step 1): recompute `actual_size = componentwise max of the children's get_width/height`; recompute both matrices
(R86); if MANUAL: `manual_val = target` if torque == 0 else `calc_torque(val, target, torque, clamp, seconds)`. `calc_torque(v,t,k,c,dt)`: if `|v-t| < 1e-4` return `t`;
`d = (t-v)*k`; if `d > 0`: `d = max(d, c)`, `out = min(v + d*dt, t)`; else `d = min(d, -c)`, `out = max(v + d*dt, t)`.
**R81** `get_time`: MANUAL -> `manual_val`. Else with `ts = video_time`: `ts <= start_time` -> 0; `end = duration`; `ts - start >= end` or `end == 0` -> 1; else `(ts-start)/end` computed
in long double and rounded to f32.
**R82** `start(t, mode, ms, dest)`: if the transition is active (video or audio) first do `set(t, sources[1])` (commit; R84). Then evaluate with the new state:
`same_as_source = dest == sources[0]`; `same_as_dest = dest == sources[1]`; `active` is now false. If `same_as_source && !active` return **false**
(script 52). Set `mode`, `manual_val = manual_target = 0`; call the plug-in `transition_start`; if fixed duration is enabled `ms := fixed_ms`; since not
active: `start_time := os_gettime_ns()`, `duration := ms * 1_000_000`. Put `dest` in slot B (R85); if `dest == NULL && same_as_dest && !same_as_source` set both
transitioning flags (unreachable after the commit: Q-4). Send `source_transition_start` (global, non-private) + `transition_start`; recompute size and matrices; return true.
**R83** The reference code also contains a test "active, manual mode, same mode, same destination -> return true"; it is **unreachable** because `active` was cleared by the commit in R82.
Conforming implementations MUST behave as if that test did not exist (Q-4, script 55 last part).
**R84** `set(t, src)`: take a reference on `src` (NULL allowed); if transitioning, send `source_transition_stop` and `transition_stop` (**signals only, no plug-in stop callback**, Q-3);
swap both slots out; slot A := `src`, `active[0] := true`, slot B empty, `active[1] := false`, both transitioning flags false, `manual_val = manual_target = 0`; release the old slots'
active links (`remove_active_child` where `active[i]`) and strong references; if `src` then `add_active_child(t, src)`.
**R85** Setting slot `idx` to child `d` (internal `set_source`, used by `start` for slot B with the callback `activate`): take a reference on `d`; `d == current` -> release and return.
If `active[idx]` already: add `d` as active child (if non-NULL; failure = `add_success=false`), and if that succeeded remove the old child's link. If `add_success`, call the slot callback
(`activate`: if `!active[idx]`, `add_active_child(t, d)`, failure (including `d == NULL`) = false, on success `active[idx] = true`; then both transitioning flags := true; returns true). The slot
becomes `d` if `add_success`, else **NULL** (and `d` is released). The previous child's strong reference is always released. On success, if `set_size` has a zero component, size and matrices are recomputed.
**R86** `clear(t)`: take both slots, set them NULL, `active[] = false`, both transitioning flags false; for each slot that was active remove the active-child link; release the strong references.
Slot A's `active` flag is false afterwards (script 50: `A=-(act=0)`). Also done by destroy (R08).
**R87** `stop` (internal, when both parts have finished): remove A's active link (if active), release A, `sources[0] = sources[1]`, `sources[1] = NULL`, `active[0] = true`, `active[1] = false`. Followed by
(outside the lock) the plug-in `transition_stop` callback and the `source_transition_stop` + `transition_stop` signals. `force_stop` only does the callback and signals.
**R88** Matrices: for each slot with a child whose size `(w,h)` is non-zero (else the old matrix is kept): `tr = set_size` if non-zero else `actual_size`; MAX_ONLY: if the child is larger than
the transition in either dimension behave as ASPECT, else scale 1; ASPECT: `use_width = tr_aspect < child_aspect`; `s = use_width ? tr_w/w : tr_h/h` for both axes; STRETCH: `(tr_w/w, tr_h/h)`.
Position = `add_alignment(zero, alignment, int(tr_w - w*s), int(tr_h - h*s))` where `add_alignment` adds `cx` if RIGHT, else `cx/2` (integer division) unless LEFT; likewise Y with BOTTOM/TOP.
Matrix = scale then translate. (Script 58.)
**R89** Video render (`video_render`, `render2`): `t = get_time`; if `t >= 1` and `transitioning_video`: `transitioning_video = false`, remember "video stopped"; if `!transitioning_audio` also perform `stop`
(R87). While transitioning, the callback receives the two child textures and `t`; sizes use `set_size` if non-zero else `actual_size`; with size 0 the callback is not called. If only the audio part is still transitioning, child B is rendered directly; otherwise child A. `video_stopped` sends
`source_transition_video_stop` + `transition_video_stop` (before the stop callback/signals). `render_direct(t, target)` is similar but also clears
`transitioning_audio` when the transition itself is not active (`activate_refs == 0`) and returns whether it is still/was transitioning; with transitioning it renders B only when `target == B`.
(Script 53, 54.)
**R90** Audio render `audio_render(t, &ts, mix, mixers, channels, rate, mix_a, mix_b)`: children that are `removed` are ignored. `min_ts` = smallest `audio_ts` among children that are not
`audio_pending` and have `audio_ts != 0`. If `min_ts != 0`: `t0 = get_time at min_ts` (calc with the child timestamp as clock; MANUAL mode uses `manual_val`); if `t0 >= 1` and
transitioning_audio -> `stop_audio` (audio flag false; if video also done, `stop`); re-read the slots and `min_ts`. Else if `!transitioning_video && transitioning_audio` -> `stop_audio`.
If `min_ts != 0` still: if transitioning_audio, each valid child (not pending, `audio_ts != 0`, not `audio_is_duplicated`) is added into the output: `pos = ns_to_frames(rate, child.audio_ts - min_ts)`;
if `pos > 1024` skip; for each mixer bit in `mixers` and channel < `channels`: `out[pos + i] += in[i] * mix(t_i)` for `i` in `0..1024-pos`, with `t_i = time at (child.audio_ts + i*1e9/rate)` (**`in` is read from
index 0**, not shifted: Q-9) using `mix_a` for A and `mix_b` for B. If not transitioning, copy the whole mix block of slot A (6 mixers x 8 channels x 1024 f32, contiguous) unchanged into the output.
Return `*ts = min_ts`, result `min_ts != 0`. If `stop` happened, run the plug-in stop callback and signals after releasing locks. (Scripts 56, 57.)
**R91** `enable_fixed(t, on, ms)` / `fixed` are plain setters. `set_manual_time(t, v)`: stores the target; if MANUAL and `v == 0` -> `set(t, sources[0])` (reset). `set_manual_torque` stores torque and clamp.
`set_size/get_size`, `set/get_scale_type`, `set/get_alignment`: plain accessors (the getters return 0/MAX_ONLY defaults for invalid input; `get_size` writes 0,0). `get_source(t, A|B)` returns a new reference to slot A or B (NULL if empty); `get_active_source` returns a new reference to slot B while transitioning (audio or video), else slot A.
**R92** `save`: record the active child's name (B when transitioning, else A, `""` if none), alignment, mode, scale type, set size. `load`: look up the child by name; failing: WARNING
`Failed to find source '<n>' for transition '<t>'`, child NULL; if found, `add_active_child(t, child)` must succeed else WARNING (recursion) and NULL; then set slot A (`active[0] = true`), alignment, mode,
scale, size verbatim and recompute size and matrices. Missing child leaves slot A empty (script 59).
**R93** `swap_begin(dst, src)`: if same object nothing. Else move `src`'s children (and their `active` flags) into `dst` (dst's old children lose their active links and references; the child's active link is re-added for
`dst`); `src` still lists them until `swap_end(dst, src)`, which clears `src` and exchanges the two transitions' texture renderers. The net effect is a **move** (script 59).

### 5.7 Media control

**R94** `media_play_pause(pause)`, `restart`, `stop`, `next`, `previous`, `set_time(ms)` do nothing unless the kind has CONTROLLABLE_MEDIA and the corresponding callback; otherwise they append an action to a
queue. At the next tick (R24 step 3) the actions are executed in FIFO order: call the callback, then send `media_pause`/`media_play`, `media_restart`, `media_stopped`, `media_next`, `media_previous`
(`set_time` sends nothing). `get_duration/get_time/get_state` call the callback directly (0 / 0 / NONE without it or without the flag). `media_started/media_ended` send their signal at once if the flag is set. (Script 70.)

## 6. Algorithms

### 6.1 `ready(sys)` (async, buffered)

```
fn ready(s, sys) -> bool:                     // queue non-empty, last_frame_ts != 0
  next = queue[0]; sys_off = sys - s.last_sys_timestamp; ft = next.ts; foff = 0
  if s.unbuffered:
     while queue.len > 1: pop_front; mark old next unused; next = queue[0]
     s.last_frame_ts = next.ts; return true
  if |ft - s.last_frame_ts| > 2e9 (direction-aware, wrapping compare as below):
     s.last_frame_ts = next.ts; return true
  foff = ft - s.last_frame_ts; s.last_frame_ts += sys_off
  frame = NULL
  while s.last_frame_ts > next.ts:                       // strictly greater
     if frame != NULL and s.last_frame_ts - next.ts < 2_000_000: break
     if frame != NULL: pop_front                          // drop the earlier candidate
     mark `frame` (maybe NULL) unused
     if queue.len == 1: return true                       // queue[0] is the answer
     frame = next; next = queue[1]
     if next.ts - ft > 2e9: s.last_frame_ts = next.ts - foff
     ft = next.ts; foff = ft - s.last_frame_ts
  return frame != NULL                                    // get_closest pops queue[0] (== frame)
```
Out-of-bounds test: `ts < clock ? clock - ts > MAX_TS_VAR : ts - clock > MAX_TS_VAR` (u64).

### 6.2 Deinterlaced selection

```
fn get_closest_frames(s, sys):
  if s.unbuffered and s.deint_offset != 0:
     frame_end = s.deint_frame_ts + s.deint_offset + 2*s.deint_half_duration - 1_000_000
     if sys < frame_end and frame_end - sys < 70_000_000: return        // keep the current pair
  if queue empty: return
  if first_frame(s) or ready_deint(s, sys):
     s.prev = NULL; s.cur = pop_front()
     if queue non-empty and s.cur.prev_frame:
        s.prev = s.cur; s.cur = pop_front(); s.half = (cur.ts - prev.ts)/2
     else: s.half = (s.cur.ts - s.deint_frame_ts)/2
     if s.last_frame_ts == 0: s.last_frame_ts = cur.ts
     s.deint_frame_ts = cur.ts
     off = video_time - cur.ts
     if s.deint_offset == 0 or |s.deint_offset - off| > half_frame_interval: s.deint_offset = off
fn first_frame(s): if s.last_frame_ts != 0 return false; if queue.len >= 2: queue[0].prev_frame = true; return true
```
`ready_deint` is the buffered selection of R67 adapted to field pairs (it additionally marks the frame preceding the chosen one with `prev_frame = true`, requires two queued frames to leave, and resets `deint_offset` to 0 on every
resynchronisation); the unbuffered branch keeps at most two frames, sets `queue[0].prev_frame` unless the current pair has not yet elapsed (`sys < ts0 + offset + (ts1-ts0)`, in which case `deint_frame_ts = ts0 - duration`),
resets `deint_offset` to 0 and returns true. Because this part is intricate the vectors 42 and 43 are normative for it; implementers SHOULD port it from the vectors as a state machine and report differences.

### 6.3 Frame copy geometry
Rows copied per plane: I420/I010/I40A: `h, (h+1)/2, (h+1)/2`; NV12/P010: `h, (h+1)/2`; I444/I422/I210/I412: three planes of `h`; all packed formats: one plane of `h`. Each row copies `min(linesize_src, linesize_dst)` bytes
(the destination uses its own allocation layout; only defined bytes are observable).

## 7. Concurrency

Locks per source: filter mutex (recursive; protects the filter array), async mutex (recursive; queue, cache, current frames), audio mutexes (S-032), transition mutex and texture mutex. `obs_source_filter_add` takes the filter
mutex around array edits; signals and plug-in callbacks run **outside** it. `video_tick` is called by one render thread; ingestion (`output_video`) may come from any thread concurrently with it (the async mutex serialises).
Destruction: the deferred part is guaranteed not to run inside a tick (the task waits for the context's tick). Deferred destruction runs on a separate thread. A conforming implementation MUST NOT hold the filter mutex across callbacks
or signals (D-3).

## 8. Reference defects and quirks

* **Q-1** `create` returns a source even when the plug-in's `create` fails (R05). SHOULD match. Script 03.
* **Q-2** `enable` always signals, `flags` only on change, rename only on change (R12-R14). Script 06, 26.
* **Q-3** Committing a running transition from `start`/`set` sends the stop *signals* but not the plug-in stop callback (R84). SHOULD match. Scripts 51, 52.
* **Q-4** The "active, manual, same mode, same destination" early return in `start` is dead code (R83); restarting towards the current destination is refused (returns false) after committing. SHOULD match. Scripts 52, 55.
* **Q-5** `output_video2` with PARTIAL range on an RGB format keeps `full_range = false` (R62), unlike `output_video`. SHOULD match. Script 36.
* **Q-6** Unbuffered async sources keep the whole queue on the very first selection (R67). SHOULD match. Script 32.
* **Q-7** `set_video_frame` / `preload_video` never set `async_active`; `get_width` stays 0 until a tick/show (R63, R64). Script 40.
* **Q-8** `filter_compatible` masks with `VIDEO|ASYNC|AUDIO`, so a plain VIDEO filter cannot be added to an AUDIO-only source and an ASYNC_VIDEO filter cannot be added to a plain VIDEO source (R42). Script 21.
* **Q-9** Transition audio mixing reads the child's samples from index 0 while writing at an offset (R90). SHOULD match. Script 56.
* **Q-10** (retired: an early misreading; `get_source` simply returns the requested slot, R91.)
* **Q-11** `swap_begin/end` moves rather than swaps (R93). Script 59.
* **Q-12** `restore_filters` sends no signals and calls no filter callbacks (R49). Script 71.
* **Q-13** `deactivate` decrements descendants unconditionally (R22).
* **D-1** `filter_set_index` with `index >= count` calls the array move with an out-of-range destination and corrupts memory in the reference (observed: heap corruption abort). **Required:** clamp `index` to `count - 1` (documented as a deviation-free choice; no vector).
* **D-2** `set_hidden`/`is_hidden` dereference a NULL source. **Required:** NULL is a no-op / returns false.
* **D-3** If `get_ref(filter)` fails inside `filter_add` (filter already being destroyed) the reference returns with the filter mutex still locked. **Required:** unlock, return.
* **D-4** `obs_source_filter_add` of a filter that already belongs to *another* source overwrites its `parent`/`target` and leaves it in two arrays. **Required:** SHOULD refuse (log WARNING); behaviour of the reference not scripted.

## 9. Implementation notes (non-normative)

* Model the source as an `Arc`-managed object with interior-mutable sub-structures (filters under a `ReentrantMutex<RefCell<..>>`, async state under its own lock). Strong/weak counts follow `Arc`/`Weak`, but destruction must be
  split into the immediate and the deferred part (R08/R09) and the deferred part must be queued to a worker.
* Keep the host services behind traits (context, clock, task queue, graphics sink) so the conformance runner can script them.
* Signal emission order is part of the contract; build helper `emit(source, global: Option<&str>, local: &str, args)` and use it everywhere (global first, only for non-private sources).
* `filters` as `Vec<Arc<Source>>` with index 0 newest; recompute `target` from the vector after every structural change rather than patching it incrementally (the observable result is identical).
* `ready()` and the deinterlace selector are order-sensitive state machines: port them literally, test with vectors 31-33, 42, 43.
* Float formatting in dumps: `%.9g`.

## 10. Conformance coverage

Script names are `NN_*` in `test/conformance/scripts/obssource/`.

| Rules | Scripts |
|---|---|
| R01, R02, R05, R06 | 03 |
| R03, R04, R07, R09, R10 | 01, 02, 05, 60 |
| R08 | 04, 05, 60 |
| R11-R15 | 06, 26 |
| R16, R17 | 62 |
| R20-R24 | 10, 11, 12, 13 |
| R25-R27 | 14, 15, 61 |
| R40-R43 | 20, 21, 05 |
| R44 | 22, 22b |
| R45, R46 | 23, 24 |
| R47 | 27, 30, 38 |
| R48 | 25 |
| R49 | 71 |
| R60-R62 | 30, 34, 35, 36, 37 |
| R63, R64 | 30, 38, 40 |
| R65-R68 | 30, 31, 32, 33 |
| R69, R70 | 39, 32, 38 |
| R100, R101 | 41, 42, 43 |
| R80 | 55, 58 |
| R81, R82, R83 | 51, 52, 55 |
| R84-R87 | 50, 51, 52, 53, 60 |
| R88 | 58 |
| R89 | 53, 54 |
| R90 | 53, 56, 57 |
| R91 | 50, 51, 55, 58 |
| R92, R93 | 59 |
| R94 | 70 |
| Q-1..Q-13 (Q-10 retired) | 03, 06, 51, 52, 55, 36, 32, 40, 21, 56, 59, 71, 11 |
| D-1..D-4 | none (not scriptable; behaviour required by this spec) |

## 11. Open questions and not covered

* **Not covered (no vectors):** GPU rendering and effect paths, `obs_source_process_filter_*`, texture/colour-space conversion results, HDR (`trc`) behaviour, `max_luminance`, the deinterlace *rendering*, `obs_source_draw*`;
  audio ingest/mixing/volume/mute/monitoring/push-to-talk/balance (S-032); hotkeys; interaction events; properties; `obs_source_save/load` of settings; scenes/groups and canvases (S-033); the name-uniqueness and
  UUID logic of the context layer; `obs_source_get_audio_mix` implementation; CEA-708 captions callbacks; caption/audio callbacks lists; `obs_source_audio_active`; async decoupled mode; profiler; `source_get_last_async_ts`.
* The Rust sufficiency check was **not performed** (cost control); consequently the prose of R67 and section 6.2 has not been validated by an independent implementation. The deinterlace selector is described only
  approximately; vectors 42/43 are the reference.
* `filter_add` with a filter that already has a parent (D-4) was not probed.
* Thread-safety behaviour (concurrent ingest vs tick) is not testable by scripts.
