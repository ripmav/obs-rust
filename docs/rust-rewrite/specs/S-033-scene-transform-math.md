# S-033 `scenemath` (scene-item transform, bounds, crop, order, group and visibility logic)

| | |
|---|---|
| Status | Draft (vectors generated and stable; Rust sufficiency check NOT performed, see section 11) |
| Reference | libobs 33.0.1 `obs-scene.c` / `obs-scene.h` (the geometry, order, group and visibility parts) |
| Target crate | `crates/obs-scene-math` (pure logic; no graphics, no threads of its own) |
| Depends on | S-004 (signals: names and order of emitted scene signals), S-008 (`matrix4`, `vec3`, axis-angle rotation; numeric tolerance), S-009 (`vec2`) |
| Conformance | `test/conformance/scripts/scenemath/` (40 scripts, runner `gen/scenemath_runner.c`, `gen/scenemath.sources`; script language in `test/conformance/README.md`, section "scenemath runner") |

## 1. Scope

In scope: everything that decides **where a scene item is and what order it is in** without drawing it:

* the stored transform of a scene item (position, rotation, scale, alignment, bounds box, bounds alignment,
  crop, crop-to-bounds), the two coordinate systems (relative to canvas height, or absolute pixels), conversions,
  and the "nudge to half pixels" rule;
* the computation of the item's **draw matrix** and **box matrix** (`update_item_transform`), including bounds types,
  bounds cropping, flips, rotation and nested-scene crop adjustment;
* when recomputation happens (setters, deferral, render-time update pass, canvas/scene size changes);
* scene item z-order (`set_order`, `set_order_position`, `reorder_items`, `reorder_items2`);
* **groups**: creating a group from items, adding/removing/ungrouping, converting transforms between group and
  parent space, group size/position derivation (`resize_group`), group-aware reorder;
* item bookkeeping that is pure state: ids, selected, locked, visible / user-visible with the audio-action queue,
  per-item show/hide transition configuration and the decision when a transition is started, removal and pruning of
  items whose source was removed, and the **signals** these operations emit (name, scene, item, order).

Out of scope: rendering (`render_item`, texture render, blend/scale-filter state; setters
`set_scale_filter`, `set_blending_*` only store enum values and flag a recompute), scene (de)serialisation
(`scene_load/save`, transform states save/load: S-014 territory), scene duplication, hotkey registration, audio
mixing of scenes (`scene_audio_render`, S-032), enumeration helpers (`find_source`, `enum_items`), the real signal
handler (S-004), and **hit testing / selection handles**, which live in the frontend (section 6.10 gives an
informative recipe built on the matrices defined here). Graphics, canvases, sources and transitions are *inputs*
(section 3.3); the component is specified against that small environment.

Callers: the frontend preview (reads matrices, sets pos/scale/rot/bounds/crop, reorders, groups), plugins and
scripts through the `obs_sceneitem_*` API, the render thread (update pass), the audio thread (visibility actions).

## 2. Terminology and conventions

RFC 2119 words apply. Vectors are `vec2 (x, y)` of `f32`. All geometry is `f32`; intermediate expressions are
evaluated in `f32` in the order written (no extended precision, no fused multiply-add reassociation is relied on).
Matrices follow S-008: `matrix4` has four row vectors `x, y, z, t`; **points are row vectors** (`p' = p * M`), so the
translation lives in row `t`. `matrix4_*` helper semantics (`scale`, `translate3f`, `rotate_aa4f`, `mul`, `inv`) are
those of S-008.

**Tolerance** (vectors): floats are compared with `|a-b| <= 1e-3 + 1e-5*|b|` (rotation produces `~1e-7` noise
where exact zeros are mathematically expected, e.g. `rot=360`; box translations carry up to `1e-3` noise at pixel
scale). `-0 == 0`. `nan`/`inf` compare as classes. Integers, booleans, ids, counters, orders, signal sequences are
exact. Log lines: level only.

`RAD(d) = d * (pi / 180)`: the reference macro computes it in double and rounds to `f32` at the call; within the
tolerance the difference is not observable.

Alignment flags (`u32`): `CENTER = 0`, `LEFT = 1`, `RIGHT = 2`, `TOP = 4`, `BOTTOM = 8`.
Bounds types (`u32`): `NONE 0`, `STRETCH 1`, `SCALE_INNER 2`, `SCALE_OUTER 3`, `SCALE_TO_WIDTH 4`,
`SCALE_TO_HEIGHT 5`, `MAX_ONLY 6`. Order movements: `UP 0`, `DOWN 1`, `TOP 2`, `BOTTOM 3`.

`trunc_i(x)` = conversion float to signed integer truncating toward zero. `trunc_u(x)` = the reference converts
float to `u32` with a C cast; for negative inputs the C result on the reference platform is the two's complement
of the signed truncation, and all consumers immediately reinterpret it as `int`. A conforming implementation MUST
therefore use **`trunc_i`** wherever this document says "`(int)` of a float-to-`u32` cast" (see Q-3). `round(x)` is
`roundf` (half away from zero).

An **item** belongs to exactly one **scene** (`parent`), which is either a *scene* or a *group scene*. A *group item*
is an item in a scene whose source is a *group scene*; its children are the items of that group scene. Items are in
a doubly linked list per scene: **first = bottom of the stack, last = top** (drawn last).

## 3. Interface

### 3.1 Item state (all of it observable through the API or the matrices)

```
SceneItem {
  id: i64                       // never 0 for a normal item
  parent: Option<Scene>         // None after removal
  source                        // opaque; provides width()/height(), flags has_audio_or_composite, is_group, is_scene
  is_group, is_scene: bool      // source kind: group scene / scene
  absolute_coordinates: bool    // copied from the scene at creation
  pos: vec2                     // relative mode: unit-less (see 5.2); absolute mode: pixels
  rot: f32                      // degrees, stored as given (not normalised)
  scale: vec2                   // relative mode: relative to scale_ref; absolute: factor
  scale_ref: vec2               // canvas size at creation (pixels)
  align: u32                    // default TOP|LEFT = 5
  bounds_type: u32, bounds_align: u32, bounds: vec2 (relative: unit-less; absolute: pixels), crop_to_bounds: bool
  crop: {left,right,top,bottom: i32}        // >= 0 after set_crop
  bounds_crop: same shape                   // computed, reset on every recompute
  last_width, last_height: u32              // source size seen by the last recompute
  output_scale: vec2, box_scale: vec2       // computed
  draw_transform, box_transform: matrix4    // computed; identity until the first recompute
  update_transform: bool        // "needs recompute at next update pass"
  update_group_resize: bool     // groups: "children changed"
  defer_update, defer_group_resize: counters (>= 0)
  user_visible: bool, visible: bool, active_refs: i64
  selected, locked: bool
  show_transition, hide_transition: Option<Source>, show_duration_ms, hide_duration_ms: u32
  audio_actions: queue of { visible: bool, timestamp_ns: u64 }
  prev, next                    // list links
}
Scene { is_group, custom_size, cx, cy, absolute_coordinates, last_width, last_height, id_counter, first_item, canvas: Option<Canvas> }
```

Defaults of a freshly added item (`obs_scene_add`): `id = ++scene.id_counter` (counter starts at 0, so ids start
at 1; a given nonzero id is used as is and does not touch the counter), `align = TOP|LEFT`, `scale = (1,1)`,
`scale_ref = dims(item)` (5.1), `user_visible = true`, `locked = false`, `selected = false`, `active_refs = 1`,
matrices identity, everything else zero, `absolute_coordinates = scene.absolute_coordinates`; in relative mode
`pos = pos_from_absolute((0,0))` so that the item sits at the top-left corner of the canvas. If the source reports
audio **or is a scene/group (composite)** then `visible = false` and one action `{visible: true, timestamp: now}`
is queued, otherwise `visible = true`. New items are appended on top. `obs_scene_add` then signals `item_add`.
A new group scene has `custom_size = true, cx = cy = 0`; a new plain scene takes `absolute_coordinates` from the
core's private flag *AbsoluteCoordinates* at creation.

### 3.2 Public functions in scope

All take possibly-null item/scene pointers; "null" arguments make the function a no-op returning
`false`/`0`/nothing unless stated.

```
obs_scene_add(scene, source) -> item | null           obs_scene_insert_group(scene, name, items*, count) -> item | null
obs_sceneitem_set_pos/rot/scale/alignment/bounds_type/bounds_alignment/bounds_crop/bounds (item, v)
obs_sceneitem_get_pos/rot/scale/alignment/bounds_type/bounds_alignment/bounds_crop/bounds (item, out)
obs_sceneitem_set_crop / get_crop           obs_sceneitem_get_info2 / set_info2 (struct obs_transform_info)
obs_sceneitem_get_draw_transform / get_box_transform / get_box_scale
obs_sceneitem_defer_update_begin/end, defer_group_resize_begin/end, force_update_transform
obs_sceneitem_set_order(item, movement)     get_order_position(item) -> int      set_order_position(item, int)
obs_scene_reorder_items(scene, items*, n) -> bool      obs_scene_reorder_items2(scene, order_info*, n) -> bool
obs_sceneitem_group_add_item(group, item)   group_remove_item(group, item)       group_ungroup(group)
obs_sceneitem_select/selected, set_visible/visible, set_locked/locked, remove, get_id/set_id
obs_sceneitem_set_transition/get_transition, set/get_transition_duration, do_transition
render-thread entry: the scene update pass (video render prologue) and group resize
audio-thread entry: apply queued visibility actions
```

`struct obs_transform_info { pos: vec2, rot: f32, scale: vec2, alignment: u32, bounds_type, bounds_alignment: u32,
bounds: vec2, crop_to_bounds: bool }`. `struct obs_sceneitem_order_info { group: Option<item>, item: item }`.

### 3.3 Environment the component needs (inputs)

* `source.width()`, `source.height()` (`u32`; 0 when the source has no size yet; for a scene this is the scene's
  own size, rule R04).
* `canvas.base_width/base_height` for the canvas a scene belongs to, and for the main canvas.
* `source.has_audio_or_composite`, `source.removed`, `source.id`.
* `core.add_active_child(parent_source, child_source) -> bool` (false = would recurse infinitely) and
  `remove_active_child`.
* A signal sink (name, scene, item, optional bool) and a monotonic `now_ns()`.
* Transitions: `is_active(t)`, `force_stop`, `set_size`, `set_alignment`, `set_scale_type`, `set`, `start`.

## 4. Data formats

The observable numeric data are the two matrices and the scalars below. Worked example (vector `rotation`):
source 200x100, relative mode, canvas 1920x1080, `align = TOP|LEFT`, pos (300,200), rot 90:

```
draw = [ 0,  1, 0, 0]      // row x: image of the source x axis
       [-1,  0, 0, 0]      // row y
       [ 0,  0, 1, 0]
       [300,200, 0, 1]     // row t
box  = [ 0,200, 0, 0]  [-100, 0, 0, 0]  [0,0,1,0]  [300,200,0,1]
```

`draw` maps a point in **source pixel space** (`0..width`, `0..height`, y down) to canvas pixels:
`p_canvas = pos + R(rot) * (S * p - origin)`, with `R(a)(x,y) = (x cos a - y sin a, x sin a + y cos a)`, `S =
output_scale` and `origin` from alignment (6.2). `box` maps the **unit square** to the item's rectangle (the bounds
box when bounds are on, else the scaled, cropped source), i.e. `box = scale(box_scale) , translate(-base_origin) ,
rotate , translate(pos)`. For `align = CENTER`, 45 degrees, source 200x100 at (300,200): `draw.t =
(300 - (100c - 50c), 200 - (100c + 50c))` = `(264.6447, 93.9340)` with `c = 0.70710678` (vector `rotation`).

## 5. Behaviour (numbered rules)

### 5.1 Dimensions

* **S-033-R01** `scene_width/height(scene)`: if `custom_size` -> `cx`/`cy`; else if the scene's source has a canvas ->
  that canvas' base width/height (0 if the canvas no longer resolves); else the main canvas' video mix base size
  (0 if the main canvas has no mix).
* **S-033-R02** `dims(item)` (the canvas size used for all relative/absolute conversions of that item):
  if `item.parent` is None, **or** the parent is a group scene without canvas -> main canvas base size; else if the parent
  is a group scene (with canvas) -> that canvas' base size; else (plain scene) -> `scene_width/height(parent)`
  (which honours a custom scene size). `W, H = dims(item)`.
* **S-033-R03** The size of a *group* source is its group scene's `cx, cy` (set by `resize_group`, R40..R44); the size
  of a *scene* source is R01. A source reporting size 0 is allowed (R30).
* **S-033-R04** The two canvas sizes the reference reads (canvas object vs. main video mix) are equal in a real
  core; an implementation needs one. (`canvas_sources`, `harness_canvas_mix_only` is harness-only.)

### 5.2 Coordinate conversion (relative mode: `absolute_coordinates == false`)

With `W, H = dims(item)`:

```
pos_from_abs(v)  = ( (2*v.x - W) / H ,  2*v.y / H - 1 )        // y range [-1,1], x scaled by the same H
pos_to_abs(v)    = ( (v.x*H + W)/2   ,  (v.y*H + H)/2 )         then NUDGE (R06)
size_from_abs(v) = ( 2*v.x / H , 2*v.y / H )
size_to_abs(v)   = ( v.x*H/2 , v.y*H/2 )                        then NUDGE (R06)
canvas_scale(item)   = item.scale * (H / item.scale_ref.y)      unless item.is_group or item.is_scene (then item.scale unchanged)
relative_scale(v)    = v * (item.scale_ref.y / H)               unless item.is_group or item.is_scene (then v)
```

* **S-033-R05** In relative mode the setters store: `pos = pos_from_abs(arg)`, `bounds = size_from_abs(arg)`,
  `scale = relative_scale(arg)`; the getters return `pos_to_abs(pos)`, `size_to_abs(bounds)`, `canvas_scale`. In
  **absolute mode** (`absolute_coordinates == true`) setters and getters copy values verbatim (pixels; no `scale_ref`
  use, no nudge). `rot`, `align`, `bounds_type`, `bounds_align` and `crop_to_bounds` are stored as given in both modes;
  `get_rot` returns the stored value (not normalised).
* **S-033-R06** NUDGE: `nudge(x) = floor(x*2 + 0.5) / 2` per component. It is applied by `pos_to_abs` and `size_to_abs`
  **unless** `item.is_group` or the item's parent is a group scene. (Hence a top-level item reads back positions and
  bounds rounded to .5; items inside groups and groups themselves read back exact values.) The *stored* values are
  never nudged by the setters.
* **S-033-R07** `get_pos`/`get_scale`/`get_bounds` on a null item write nothing. `obs_sceneitem_get_info2` reads all
  fields as above and `crop_to_bounds`; `set_info2` applies, in relative mode, `pos_from_abs`, `size_from_abs(bounds)`
  and -- **only if both scale components are finite** -- `relative_scale`; in absolute mode it copies; then it copies
  rot/alignment/bounds_type/bounds_alignment/crop_to_bounds and schedules a recompute (R20). A non-finite scale leaves
  the stored scale unchanged and still applies every other field (`info2`).
* **S-033-R08** A scene's relative values do not move when the canvas changes size; they are re-expressed at read /
  recompute time: after the canvas becomes `W' x H'` an item's absolute position changes to `(x'*...)`, specifically
  `pos_to_abs` with the new `W, H` (so a pure width change moves x but not y; a height change scales both), and the
  scale follows `H'/scale_ref.y`. `scale_ref` is fixed at creation and never updated (`rel_canvas_resize`,
  `bounds_canvas_resize`, `canvas_sources`). In absolute mode a canvas change changes nothing (`abs_basic`).

### 5.3 Setters, crop and recompute scheduling

* **S-033-R20** `schedule(item)` (used by pos, rot, scale, alignment, bounds*, set_info2, defer_end): if
  `item.parent` is None **or** the parent is a group scene -> set `update_transform = true` (recomputed by the next
  update pass); else -> recompute **immediately** (`update_item_transform(item, update_tex=false)`).
  `update_item_transform` itself returns at once, doing nothing and *not* clearing any flag, while `defer_update > 0`.
* **S-033-R21** `set_crop(item, crop)`: null item or crop -> error log, no-op; a crop equal to the stored crop ->
  no-op (the flag is not touched); otherwise store, clamp every negative field to 0, set `update_transform = true`.
  It does **not** recompute immediately (`crop`). `get_crop` copies the stored crop (post-clamp).
* **S-033-R22** `defer_update_begin` increments, `defer_update_end` decrements `defer_update`; when it reaches 0,
  `schedule(item)`. `defer_group_resize_begin/end` do the same for `defer_group_resize`; reaching 0 sets
  `update_group_resize = true`. `force_update_transform`: if `update_transform` was set, clear it and recompute now
  (`update_tex = false`); otherwise nothing. Neither counter is range checked (`defer`).
* **S-033-R23** `set_scale_filter` and `set_blending_mode` set `update_transform = true`; the other blend setter does not.
  (Informative; no script exercises these setters.)
* **S-033-R24** *Update pass* for a non-group scene `S` (executed before the scene is drawn; group scenes are never
  passed directly): `changed = scene_size_changed(S)` (compares `scene_width/height(S)` with `S.last_width/height`,
  stores the new values, true on difference; `last_*` start at 0, so the first pass is `changed` whenever the size is
  nonzero). Then `update_pass(S, group_item=None, changed)`:

  ```
  update_pass(S, G, changed):
      rebuild = G != None and G.update_group_resize
      for item in S from first to last:
          if item.source.removed:
              next = item.next
              remove_internal(item)            // R51 ; the handle is released after the pass
              rebuild = true ; continue with next
          if item.is_group: update_pass(item.group_scene, item, changed)     // children BEFORE the group's own transform
          if item.update_transform or item.last_size != item.source.size() or changed:
              update_item_transform(item, update_tex = true) ; rebuild = true
      if rebuild and G != None: resize_group(G, changed)
  ```
  `update_tex = true` additionally (a) adjusts the crop of nested scenes (R32) and (b) clears `update_transform`
  after the recompute. The pass is not run for items that are not in `S`'s tree.
* **S-033-R25** Setting `pos/scale/...` on an item inside a group never recomputes immediately; the child's matrices
  change at the next update pass of the parent scene (`group_resize_defer`, `signals_transform`).

### 5.4 Item transform computation

`update_item_transform(item, update_tex)` -- see algorithm 6.1/6.2. Rules:

* **S-033-R30** Order of effects: (1) return if `defer_update > 0`; (2) `bounds_crop = 0`; (3) `(w,h) = source size`;
  (4) if `update_tex` and the item is a scene item whose source is a **scene** (`is_scene`), run R32; (5) `cx = calc_cx(w)`,
  `cy = calc_cy(h)`; `last_width/height = (w,h)` (the *uncropped* source size); (6) compute `scale` and `position`
  (R31); (7) bounds (R33) or plain size; (8) alignment origin; (9) `draw_transform`, `output_scale`; (10) `box_scale`,
  `box_transform` (6.2); (11) signal `item_transform` on the parent (a recompute of a parentless, i.e. removed, item is outside the
  preconditions: `force_update_transform` on one dereferences the null parent in the reference; no pass visits it, and setters only flag it); (12) if `update_tex`, clear
  `update_transform`.
  `calc_cx(w) = w - (crop.left + crop.right + bounds_crop.left + bounds_crop.right)` if that sum `<= w`, else **2**;
  `calc_cy` likewise with top/bottom (sums are `u32`; `bounds_crop` is 0 at this point, R33 fills it later). A crop
  exactly equal to the size gives 0, not 2 (`crop`).
* **S-033-R31** Relative mode: `scale = canvas_scale(item)`, `position = pos_to_abs(item.pos)` -- **with** nudge for
  top-level items (so the *drawn* position is rounded to .5 px; group children and groups are exact). Absolute mode:
  `scale = item.scale`, `position = item.pos`.
* **S-033-R32** Nested-scene crop adjustment (only `update_tex` and `is_scene`; a *group* item is not `is_scene`): if
  `last_width == 0` or `last_height == 0` do nothing; else with `fx = w / last_width`, `fy = h / last_height` (float)
  set each crop field to `trunc_i(field * f)` of its axis (left/right use `fx`, top/bottom `fy`). (`scene_item_size`)
* **S-033-R33** Without bounds (`bounds_type == NONE`): `cx = trunc_u(cx * scale.x)`, `cy = trunc_u(cy * scale.y)`
  (Q-3). With bounds: algorithm 6.3 modifies `scale`, `origin`, `cx`, `cy`, `bounds_crop`.
* **S-033-R34** Alignment: `add_alignment(origin, align, cx, cy)` (with `(int)cx`, `(int)cy`, see Q-3) adds to `origin.x`:
  `+cx` if `RIGHT`, else `+trunc(cx/2)` if not `LEFT` (so CENTER and anything without `LEFT`/`RIGHT`); same for y with
  `BOTTOM`/`TOP`. `RIGHT` wins over `LEFT` when both are set; `BOTTOM` over `TOP`. Integer division truncates toward
  zero (`align`, `quirk_negative_scale_align`).
* **S-033-R35** `draw_transform = I -> scale(scale.x, scale.y, 1) -> translate(-origin.x, -origin.y, 0) ->
  rotate_aa(0,0,1, RAD(rot)) -> translate(position.x, position.y, 0)` (each step is `matrix4_op(dst, dst, ...)`, i.e.
  the operation is appended on the right: `M = M * Op`). `output_scale = scale` (after bounds scaling, signed).
* **S-033-R36** `box_scale`: with bounds -> `size_to_abs(bounds)` (relative; nudged for top-level items) or
  `bounds` (absolute); without bounds -> `(scale.x * w', scale.y * h')` where `(w', h') = (cx_cropped, cy_cropped)` are the
  *crop-reduced* `calc_cx/cy` values from step 5 (and `scale` the signed output scale), as `f32` (not truncated).
  `base_origin = (0,0)`, `add_alignment(base_origin, align, trunc_i(box_scale.x), trunc_i(box_scale.y))`.
  `box_transform = I -> scale(box_scale) -> translate(-base_origin) -> rotate_aa(z, RAD(rot)) -> translate(position)`.
  So `box` ignores bounds alignment and bounds-crop; it is the rectangle the user manipulates.
* **S-033-R37** After step 11 (signal) nothing else changes. The signal `item_transform` carries `scene = parent`,
  `item = item`; it is emitted on every successful recompute, never while deferred.

### 5.5 Z-order

* **S-033-R60** `set_order(item, m)`: null item or null parent -> no-op, **no signal**. Otherwise detach the item and
  re-attach it: `DOWN` -> after `prev.prev` (or at the bottom if `prev` is None or has no prev); `UP` -> after `next`
  (if there is one, else after `prev`, i.e. unchanged); `TOP` -> after the last item of the scene; `BOTTOM` -> at the
  head. Moves at the ends are no-ops that still end in the signal. **Always** emits `reorder` (scene = parent) on success
  of the null/parent checks (`order`).
* **S-033-R61** `get_order_position(item)` = number of items below it in its scene (index from the bottom). The item MUST
  have a parent (the reference dereferences it: D-5). (`order`, `reorder2`)
* **S-033-R62** `set_order_position(item, n)`: null item/parent -> no-op. Detach; if the scene is now empty **or** `n == 0`
  -> attach at the head; else walk `n-1` steps from the first item (stopping at the last item) and attach after the item
  reached. So the final index is `n` clamped to the item count; `n < 0` behaves like `n == 1` (Q-2). Always emits
  `reorder` (`order_position`, `quirk_order_position_negative`).
* **S-033-R63** `reorder_items(scene, list, n)`: `n == 0` or null scene -> false. Succeeds (returns true, emits `reorder` once
  with `item = list[0]`'s scene) only when the list is a **permutation of the scene's items** (every item of the scene
  appears in `list` and `n` equals the item count; items of the list not in the scene, duplicates leaving an item
  out, or a size mismatch -> false, nothing changes) **and** the order differs from the current one. The list's index 0 becomes
  the bottom (`reorder`). Group children are not part of a scene's list.
* **S-033-R64** `reorder_items2(scene, info[], n)`: null scene/array or `n == 0` -> false. The *current layout* is
  the flat list: for each top-level item, the item as `{group: None, item}` followed, if it is a group, by each child as
  `{group: that group item, item: child}`. If `info` equals the current layout **element-wise** (same length, same
  group and item) -> false. Otherwise it is applied (returns true, emits `reorder` once) **without further validation**
  (Q-4): (a) for every listed non-group item, its current group (if any) transform is removed (R45); (b) the scene's
  chain is rebuilt from the list: a group entry is followed by the entries naming it as `group`, which become that group's
  children in listed order (hotkeys of items entering a group are re-registered; each moved child gets
  `apply_group_transform` for its **new** group and the group is resized with `resize_group(group, false)`); a
  non-group entry with no group becomes a top-level item (hotkeys re-registered if it came from a group). Items not
  listed are dropped from the chain (`quirk_reorder2_partial_list`).

### 5.6 Groups

* **S-033-R40** `group_resize(group_item, scene_resize)` (`resize_group`): returns at once if `defer_group_resize > 0`;
  `resize_scene_base` (R41); if the group scene is empty return (the group's own matrices stay stale, Q-5); if
  `group.bounds_type == NONE and not scene_resize`: re-anchor the group's position (R42); clear
  `update_group_resize`; `update_item_transform(group, false)`.
* **S-033-R41** `resize_scene_base(S)` (scene of a group): if empty: `cx = cy = 0`, return false. Else the extent is
  `minv/maxv` = component-wise min/max over all four corners `(0,0),(1,0),(0,1),(1,1)` of every child transformed by
  its `box_transform` (point times matrix, z = 0). Then **every child is re-based**: `child.pos -= minv_rel` where
  `minv_rel = size_from_abs(minv)` computed with the *first* child's item (relative mode) or `minv` (absolute), and each
  child is recomputed immediately (`update_item_transform(child, false)`). Finally `cx = ceil(maxv.x - minv.x)`,
  `cy = ceil(maxv.y - minv.y)` as `u32`. Hence a group's children coordinates are relative to the top-left of their
  union, and the group's size is the union's size (`group_basic`).
* **S-033-R42** Group re-anchor: `anchor.x = minv.x` if `group.align & LEFT`, `maxv.x` if `RIGHT`, else the midpoint
  `(maxv.x-minv.x)*0.5 + minv.x`; `anchor.y` likewise with `TOP`/`BOTTOM`; here `minv/maxv` are the values computed
  *before* the re-basing (so they are in the previous group-local space); `anchor = point_transform(anchor,
  group.draw_transform)`; relative mode: `anchor = pos_from_abs(anchor)` (item = the group); `group.pos = anchor`. This
  keeps the children where they were on screen when a child moves (`group_resize_defer`).
* **S-033-R43** `insert_group(scene, name, items, count)`: null scene -> null. If any listed item has a different parent
  than `scene` or is itself a group -> null (nothing created). A new group scene+item is created (as `obs_scene_add`
  would, positioned right above `items[count-1]`, or on top when `items` is null/`count == 0`); if there are no items the
  empty group item is returned, **no `item_add` signal is emitted** (Q-6). Otherwise, with the lists locked: for `i =
  count-1 .. 0`: `remove_group_transform(group, items[i])` then detach it; relink the items in list order (index 0 =
  bottom of the group), set their parent to the group scene, `apply_group_transform(items[i], group)` for each; set
  `items[0].prev = None`; `resize_group(group, false)`; then signal `item_add` (scene, group item). `count > 0` with
  `items == NULL` is invalid (D-1). Because the new group's matrices are identity and the items still belong to the
  top-level scene when their transform is removed, positions and scales are nudged to .5 px once (Q-7).
* **S-033-R44** `group_add_item(group, item)`: no-op unless `group` is a group item, `item` non-null and
  `item.parent == group.parent`. Effect: `remove_group_transform(group, item)`, detach, attach **at the bottom of the group
  scene**, `apply_group_transform`, `resize_group(group, false)`, signal `refresh` on the parent scene (`group_membership`).
* **S-033-R45** `remove_group_transform(group, item)` (convert from group space to parent space): no-op if the item has
  no parent or `group` is None. Algorithm 6.4a. `apply_group_transform(item, group)`: algorithm 6.4b (inverse). Both
  finish with `update_item_transform(item, false)`.
* **S-033-R46** `group_remove_item(group, item)`: no-op unless both non-null and `group` is a group item. `remove_group_transform`,
  detach, attach **at the bottom of the group's parent scene** (not the former slot), `resize_group(group, false)`, signal
  `refresh` on the parent. The item is not validated to be a member of `group` (D-2).
* **S-033-R47** `group_ungroup(group)`: no-op unless a group item. Signals `item_remove` for the group item first. Then for each
  child in group order (bottom first): `remove_group_transform(group, child)`; a **new** item for the same source is added to the
  parent scene directly above the previous new one (first one directly above the group item) with a **fresh id**
  (`id_counter`); all of these fields are copied from the child: selected, pos, rot, scale, align, last_width/height,
  output_scale, scale_filter, blend settings, box/draw transforms, box_scale, bounds_type/align/size, crop_to_bounds, bounds_crop,
  crop, locked, durations and duplicates of the transitions, private settings, hidden state (`set_visibility(false)` when the
  child was not user-visible); `update_transform = true` is set on the new item (R20 semantics: recomputed in the next pass).
  `scale_ref` is **not** copied (the new item takes the current canvas size, Q-8) and `visible/active_refs` follow the add rules.
  Then `apply_group_transform(child, group)` restores the child (the group scene keeps its items). Finally the group item is
  detached and released. `ungroup2(group, signal)` additionally signals `refresh` on the parent when `signal` (`group_ungroup`).
* **S-033-R48** Moving, scaling, rotating or flipping the group item changes only the group's own matrices
  (children are stored group-locally and are composed with the group's `draw_transform` at draw time) (`group_move`).
  Items in a group read back un-nudged values (R06).

### 5.7 Visibility, lock, selection, ids, removal

* **S-033-R50** `set_visible(item, v)`: null item, `user_visible == v`, or null parent -> `false`, nothing happens. Otherwise:
  (1) `do_transition(item, v)` (R53); (2) if the item is a group, call `do_transition(child, v)` for every child whose
  `user_visible` is true; (3) `user_visible = v`; (4) if `v`: `active_refs += 1` and if it became 1 try
  `add_active_child(parent.source, item.source)`; on failure `active_refs -= 1` and **return false** (Q-1 / D-3: `user_visible`
  already changed, no signal sent); (5) signal `item_visible` (scene, item, visible=v); (6) if the source has audio or is
  composite (scene/group): queue `{visible: v, timestamp: now}` (the item's `visible` flag stays unchanged until the
  actions are applied, R54); else apply immediately: `set_visibility(item, v)` (clears the queue, adjusts
  `active_refs`/active-child registration, sets `visible = user_visible = v`). Returns `true`.
* **S-033-R51** `remove(item)`: no-op for null or already-removed items. Marks removed, `select(false)` (signals
  `item_deselect` if it was selected), `set_visibility(false)` (no signal), detaches (parent -> None), signals `item_remove`
  on the former parent, clears both transitions, then releases the scene's reference. Afterwards setters only flag and
  getters still read the stored values.
* **S-033-R52** `select(item, s)`: no-op when null, parentless, or unchanged; otherwise stores and signals
  `item_select` / `item_deselect`. `set_locked(item, l)`: null, unchanged or parentless -> false; else stores, signals
  `item_locked` (with `locked`) and returns true. `selected()/locked()/visible()`: null -> false; `visible()` returns
  **`user_visible`**. `get_id` of a null item is 0; `set_id` overwrites without updating the counter.
* **S-033-R53** `do_transition(item, visible)`: (1) if the show transition is active (video or audio transitioning) force-stop
  it; likewise the hide transition. (2) `t = visible ? show_transition : hide_transition`; none -> return. (3) `ms =
  duration(visible)`; 0 -> 300. (4) `set_size(t, source.w, source.h)`, `set_alignment(CENTER)`, `set_scale_type(ASPECT)`. (5)
  `add_active_child(scene source, t)`; connect the transition-stop callback that removes it again. (6) `visible == false`:
  `set(t, item.source)`, `start(t, AUTO, ms, dest=None)`; `visible == true`: `set(t, None)`, `start(t, AUTO, ms, dest=item.source)`.
  The transition is started **before** the visibility flag changes and before the signal (`transitions`).
* **S-033-R54** Audio-thread action application (`apply_scene_item_audio_actions`, called with `ts = 0` to flush): for each queued
  action in order: pop it, `visible = action.visible`, count `deref` for every hide; after the loop, for each deref decrement
  `active_refs` and when it reaches 0 call `remove_active_child(parent source, item source)`. (Sample-accurate buffer
  generation is S-032's business.) With a nonzero `ts` an action later than the processed window stays queued; not
  covered here.
* **S-033-R55** *Prune*: in the update pass (R24) an item whose source `removed` flag is set is removed (R51) and
  released after the pass, the enclosing group (if any) is rebuilt. `obs_sceneitem_remove` of a group child does **not**
  rebuild the group (Q-5, `quirk_stale_group_after_removal`).
* **S-033-R56** Scene signals emitted by this component, with parameters `scene` (the parent scene) and, where listed, `item`
  and a bool: `item_add` (scene add, insert_group with items), `item_remove` (remove, prune, ungroup), `item_select`,
  `item_deselect`, `item_visible` (+visible), `item_locked` (+locked), `item_transform`, `reorder`, `refresh`. The vectors
  fix their order relative to each other (see scripts `signals_transform`, `select_lock_remove`, `transitions`).

## 6. Algorithms

### 6.1 `update_item_transform`

```
function update(item, update_tex):
    if item.defer_update > 0: return
    item.bounds_crop = 0
    (w, h) = item.source.size()
    if update_tex and item.is_scene: nested_crop(item, w, h)             // R32
    cx = calc_cx(w) ; cy = calc_cy(h)                                    // R30
    item.last_width, item.last_height = w, h
    cw, ch = cx, cy                                                      // cropped size, kept for the box
    origin = (0,0)
    if not absolute: scale = canvas_scale(item); position = pos_to_abs(item.pos)
    else:            scale = item.scale;          position = item.pos
    if item.bounds_type != NONE: bounds_step(item, origin, scale, cx, cy)       // 6.3  (changes cx, cy to the bounds size)
    else: cx = (u32)(cx * scale.x) ; cy = (u32)(cy * scale.y)                  // trunc_i semantics (Q-3)
    add_alignment(origin, item.align, (int)cx, (int)cy)
    draw = identity ; draw = draw*scale(scale.x, scale.y, 1) ; draw = draw*translate(-origin) ;
    draw = draw*rotate_z(RAD(item.rot)) ; draw = draw*translate(position)
    item.output_scale = scale
    if bounds_type != NONE: box_scale = absolute ? item.bounds : size_to_abs(item.bounds)
    else                    box_scale = (scale.x * cw, scale.y * ch)           // float, not truncated
    base_origin = (0,0) ; add_alignment(base_origin, item.align, (int)box_scale.x, (int)box_scale.y)
    box = identity ; box = box*scale(box_scale) ; box = box*translate(-base_origin) ; box = box*rotate_z(RAD(rot)) ; box = box*translate(position)
    emit item_transform ; if update_tex: item.update_transform = false
```

(the draw matrix' scale does **not** include the source size; the box matrix' scale does.)

### 6.2 Alignment helper

```
add_alignment(v, align, cx:int, cy:int):
    if align & RIGHT: v.x += cx   elif not (align & LEFT): v.x += cx / 2       // C integer division, toward zero
    if align & BOTTOM: v.y += cy  elif not (align & TOP):  v.y += cy / 2
```

### 6.3 Bounds step

```
bounds_step(item, origin, scale, cx, cy):          // cx, cy = crop-reduced source size (u32)
    B = absolute ? item.bounds : size_to_abs(item.bounds)           // nudged for top-level items
    w = cx * |scale.x| ; h = cy * |scale.y|                          // f32
    item_aspect = w / h ; bounds_aspect = B.x / B.y
    t = item.bounds_type
    if t == MAX_ONLY and (w > B.x or h > B.y): t = SCALE_INNER
    if t in {SCALE_INNER, SCALE_OUTER}:
        use_width = bounds_aspect < item_aspect
        if item.bounds_type == SCALE_OUTER: use_width = not use_width       // tests the ORIGINAL type
        scale = scale * (use_width ? B.x / w : B.y / h)                       // keeps the sign
    elif t == SCALE_TO_WIDTH:  scale = scale * (B.x / w)
    elif t == SCALE_TO_HEIGHT: scale = scale * (B.y / h)
    elif t == STRETCH:         scale.x = copysign(B.x / cx, scale.x) ; scale.y = copysign(B.y / cy, scale.y)
    // MAX_ONLY without overflow, or t == NONE-like: scale unchanged
    w = cx * scale.x ; h = cy * scale.y                                      // signed
    dw = B.x - |w| ; dh = B.y - |h|
    cx = (u32) B.x ; cy = (u32) B.y                                          // truncation
    add_alignment(origin, item.bounds_align, (int)(-dw), (int)(-dh))         // trunc_i of the float
    if crop_to_bounds_active(item) and (dw < -0.1 or dh < -0.1):
        // crop_to_bounds_active: item.crop_to_bounds and bounds_type (original) in {SCALE_OUTER, SCALE_TO_WIDTH, SCALE_TO_HEIGHT}
        horiz = dw < -0.1                                                    // width wins when both overflow
        flipped = horiz ? (w < 0) : (h < 0)
        diff = horiz ? dw : dh ; sc = horiz ? scale.x : scale.y ; org = horiz ? origin.x : origin.y
        al = item.bounds_align & (horiz ? LEFT|RIGHT : TOP|BOTTOM)
        over = |diff / sc|                                                   // overdraw in source pixels
        tl = (al & (TOP|LEFT)) ? 0 : (al & (BOTTOM|RIGHT)) ? over : over/2
        br = over - tl
        if flipped:
            if al == CENTER: org = org * 2
            elif al & (TOP|LEFT): org = org - diff
            c_br = round(tl) ; c_tl = round(br)                              // swapped on a flipped axis
        else:
            org = 0 ; c_br = round(br) ; c_tl = round(tl)
        if horiz: bounds_crop.right = c_br ; bounds_crop.left = c_tl ; origin.x = org
        else:     bounds_crop.bottom = c_br ; bounds_crop.top = c_tl ; origin.y = org
    origin.x += (w < 0) ? w : 0 ; origin.y += (h < 0) ? h : 0               // keep a flipped item inside its box
```

Notes: the bounds crop is *recorded* (`bounds_crop`) but `cx`/`cy` were already derived from the pre-crop values;
the renderer applies `crop + bounds_crop` as texture sub-rectangle. Only one axis is ever cropped.

### 6.4 Group transform conversion

```
// helper: point_transform(p, M) = (p.x,p.y,0,1) * M, x/y of the result
a) remove_group_transform(group, item)          // group space -> parent space
    (s, p) = item absolute ? (item.scale, item.pos) : (size_to_abs(item.scale), pos_to_abs(item.pos))     // conversions use `item`'s CURRENT parent for dims and nudge (R06)
    p = point_transform(p, group.draw_transform)
    T = group.draw_transform with row t set to (0,0,0,1)
    M = diag(s.x, s.y, 1, 1) * T
    s.x = |M.x| * (s.x > 0 ? 1 : -1) ; s.y = |M.y| * (s.y > 0 ? 1 : -1)       // |row| = length of the 4-vector; scale 0 gives negative zero
    write back (absolute: copy; relative: size_from_abs / pos_from_abs with `item`'s current parent)
    item.rot += group.rot ; update(item, false)
b) apply_group_transform(item, group)           // parent space -> group space
    same, but with Tinv = inverse(group.draw_transform) in place of draw_transform (point and linear part),
    and item.rot -= group.rot ; update(item, false)
```
The new scale uses only the lengths of the first two rows of the (inverse) linear part, so shear introduced by a
rotated, non-uniformly scaled group is approximated away; flips of the item are kept by the sign test, flips of the
group are not transferred to scale (they are in the position only). `matrix4_inv` of a singular `draw_transform`
(group scale 0) gives S-008's failure result; behaviour is then unspecified (open question).

### 6.5 Group resize

R40..R42. Complexity is linear in the number of children; no locks are re-entered (7).

### 6.6 Order primitives

`detach(item)`: unlink from the doubly linked list, `parent = None`. `attach(scene, item, after)`: link directly above
`after`, or at the head when `after` is None. All order rules (R60-R62) are compositions of these; the lists never hold
duplicates.

### 6.7 `nested_crop`

R32. Uses `float` division then `trunc_i(field * factor)`; fields never become negative.

### 6.8 Dimension functions

R01/R02 as written.

### 6.9 Scene id allocation

`id = given_id != 0 ? given_id : ++scene.id_counter` (per scene, groups have their own counters; children moved
between scenes keep their id; ungroup creates new ids).

### 6.10 Selection geometry (informative, frontend)

The selection rectangle of an item is the image of the unit square under `box_transform`: corners `(0,0),(1,0),(0,1),(1,1)`
mapped by `p' = p * box`. A canvas point `q` hits the item when `inverse(box)` maps it into `[0,1] x [0,1]` (negative box
scales from flips produce the same set); for an item inside a group compose with the group's `draw_transform` first.
Top-most hit = last item in the list whose `user_visible` is true. This is not implemented in the reference library.

## 7. Concurrency

The reference serialises scene mutation with two recursive mutexes per scene (`video`, `audio`; `full_lock` takes both,
video first). The update pass takes the video lock (group scenes: nested video lock). Order, group and remove operations take
`full_lock(scene)` (and the group scene's). Visibility actions are protected by a per-item mutex, shared with the audio
thread. A port MUST keep: updates of one scene are mutually exclusive with reordering/grouping of the same scene; the audio
thread only touches `audio_actions`, `visible`, `active_refs` (under the item mutex). `update_transform` and the two defer
counters are atomic and may be written without a lock. Signals are emitted while the scene locks may be held (callbacks must
not call back into scene mutation).

## 8. Reference defects and quirks

* **Q-1 (QUIRK, `visibility_recursion`)** `set_visible(true)` that fails the active-child registration returns `false` but has
  already written `user_visible = true` and emitted no signal; a second `set_visible(true)` returns `false` ("unchanged") so the
  item stays `visible = false` while `user_visible = true`. Required: reproduce (SHOULD).
* **Q-2 (QUIRK, `quirk_order_position_negative`)** `set_order_position(n)` for `n < 0` equals `n == 1`.
* **Q-3 (QUIRK, `quirk_negative_scale_align`)** Alignment shifts for flipped items use the *signed* truncated pixel size (negative);
  MUST be reproduced: `align_size = trunc_i(float)`; do not saturate at 0 (a Rust `as u32` would).
* **Q-4 / D-4 (DEFECT, `quirk_reorder2_partial_list`)** `reorder_items2` accepts an incomplete list and orphans the unlisted items.
  Required: a faithful port reproduces it; a port MAY return `false` for a list that is not a permutation of the current
  layout provided its callers (frontend) always pass complete lists. The vector records the reference result.
* **Q-5 (QUIRK, `group_membership`, `quirk_stale_group_after_removal`)** (a) After the last child leaves a group the group's own
  matrices keep the old size until the next pass (`resize_group` returns early on an empty group; the next update pass does recompute
  them because the reported size changed). (b) Removing a group child with `obs_sceneitem_remove` does not resize the group.
* **Q-6 (QUIRK, `group_basic`)** An empty group created by `insert_group` emits no `item_add`.
* **Q-7 (QUIRK, `quirk_group_insert_nudge`)** Moving an item into a group by `insert_group` snaps its scale and position (once) to
  the nudge grid (scale to multiples of `1/540` at 1080p).
* **Q-8 (QUIRK, `group_ungroup`)** Ungrouped items get a new `scale_ref` (current canvas) and new ids; this changes the effective relative
  scale if the canvas size changed since the original item was created.
* **Q-9 (QUIRK, `zero_size`)** A zero-sized source together with a bounds type (other than `NONE`, `MAX_ONLY`) yields `NaN`/`inf`
  scale and an all-`NaN` `draw_transform` (the `box` stays finite); no error is reported. Required: non-finite class match.
* **D-1 (DEFECT, precondition)** `insert_group(scene, name, items != NULL, 0)` reads `items[-1]`. Required: treat as "no items".
* **D-2 (DEFECT, precondition)** `group_remove_item` does not check membership; with a non-member the item is moved to the parent's bottom after
  a bogus transform conversion. Required: callers must not do this; a port SHOULD ignore non-members.
* **D-3** see Q-1 (inconsistent state, no crash).
* **D-5 (DEFECT, precondition)** `get_order_position` and `force_update_transform` on a removed (parentless) item dereference a null pointer
  in the reference. Required: callers must not; a port SHOULD return 0 / do nothing.
* **Q-10 (QUIRK, `canvas_sources`)** Scene sizes come from two fields of the reference core (canvas object vs. main video mix);
  harness-only desynchronisation in `harness_canvas_mix_only` is NOT required.
* **Q-11 (QUIRK, `signals_transform`)** In an update pass that recomputes group children, each flagged child signals `item_transform`
  once from the pass and once more when `resize_scene_base` recomputes it; the group item signals once after. The multiplicity is part
  of the vectors (SHOULD match).

## 9. Implementation notes (non-normative)

* Model the scene as an arena (`Vec<Item>` + indices) with `first/prev/next` as indices; handles are the stable ids of
  the arena slots. Keep `parent: Option<SceneId>` so removed items stay readable.
* Use `f32` everywhere and `matrix4` from S-008; do not use `glam` unless its `Mat4` multiply order is mapped to the row-vector
  convention above. Compute `RAD` as `rot as f64 * (PI/180)` then cast to `f32`.
* Float to int: write `fn trunc_i(f: f32) -> i32 { f as i32 }` (saturating is fine for realistic values) and never cast a negative
  float to `u32` first; `NaN -> 0`.
* Make the environment a trait (`SceneEnv { source_size, canvas_size(scene), add_active_child, now_ns, signal, transitions }`) and the
  signal sink a `Vec<Signal>` so the conformance runner needs only a recording implementation.
* The update pass is the only place that clears `update_transform`; immediate recomputes from setters pass `update_tex = false`.
* The nudge applies at **read** time only (`get_*`, drawn position); stored values stay exact.

## 10. Conformance coverage

| Rule | Scripts |
|---|---|
| R01-R04 | `canvas_sources`, `scene_item_size`, `group_canvas`, `harness_canvas_mix_only` |
| R05-R08 | `rel_basic`, `abs_basic`, `abs_group`, `rel_canvas_resize`, `bounds_canvas_resize`, `info2` |
| R20-R25 | `defer`, `crop`, `group_resize_defer`, `signals_transform`, `rel_canvas_resize`, `scene_item_size` |
| R30-R37 | `rel_basic`, `align`, `rotation`, `crop`, `bounds_types`, `bounds_crop`, `bounds_flip`, `scene_item_size`, `quirk_negative_scale_align`, `zero_size` |
| R40-R48 | `group_basic`, `group_move`, `group_membership`, `group_ungroup`, `group_resize_defer`, `group_precision`, `group_canvas`, `abs_group`, `quirk_group_insert_nudge`, `quirk_stale_group_after_removal`, `reorder2` |
| R50-R56 | `visibility`, `visibility_scene_groups`, `visibility_recursion`, `transitions`, `select_lock_remove`, `prune`, `signals_transform` |
| R60-R64 | `order`, `order_position`, `quirk_order_position_negative`, `reorder`, `reorder2`, `quirk_reorder2_partial_list` |
| Q-1..Q-11, D-1..D-5 | scripts named above (D-1, D-2 are preconditions: not scripted) |

## 11. Open questions

* The Rust sufficiency check (independent re-implementation from this document) was **not performed**. Treat the spec as unverified
  against an implementer; expect small gaps in 6.4 (group conversion edge cases) and 6.3.
* `matrix4_inv` on a singular group transform (group scale 0): behaviour of `apply_group_transform` undefined here (not scripted).
* Behaviour when `H == 0` (canvas height 0): divisions by zero produce `inf/NaN`; not scripted.
* Audio-action application with a nonzero timestamp (window cut-off at 1024 frames) is described only in prose.
* Scene serialisation of the transform fields (`pos_rel`, `scale_ref`, ...) and transform-state save/load are not covered
  here; they belong with the scene (de)serialisation spec.
* Windows/macOS: no platform differences visible in the covered code.
