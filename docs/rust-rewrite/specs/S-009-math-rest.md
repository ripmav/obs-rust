# S-009 `vec2`, `matrix3`, `plane`, `bounds`, `math-extra`, `half`, `srgb`, `math-defs`

| | |
|---|---|
| Status | Draft (needs second-author review) |
| Reference | libobs 33.0.1 `graphics/vec2.*`, `matrix3.*`, `plane.*`, `bounds.*`, `math-extra.*`, `half.h`, `srgb.h`, `math-defs.h`, plus the `vec3_*` functions of `vec3.c` that use these types, `quat_from_matrix3`, `matrix4_from_matrix3` |
| Target crate | `crates/obs-math` (same crate as S-008) |
| Depends on | S-008 (`vec3`, `quat`, `matrix4`, `axisang`, lane rules, tolerance rule) |
| Conformance | `test/conformance/scripts/math2/` (28 scripts, runner `gen/math2_runner.c`) |

## 1. Scope

The second half of the libobs maths layer: the 2D vector, the 3x4 matrix, plane and
axis-aligned bounding box (AABB) types, the small helpers in `math-extra`, the float to
half-float conversion used for GPU vertex/texture data, the sRGB helpers used by the CPU-side
texture code, and the constants of `math-defs.h`. Everything here crosses the plugin ABI
(structs by value, ~70 exported functions, plus the header-inline code that plugins compile in).
The Rust types must be layout compatible and reproduce the lane semantics of S-008 (a `vec3` has
a live fourth lane `w`; `dot`/`mul` include it).

Out of scope: `vec3`/`vec4`/`quat`/`matrix4`/`axisang` bodies (S-008), `rand()` itself.

## 2. Conventions

- Everything from S-008 section 2 applies: `[x,y,z,w]` lanes, **row vectors**, `matrix4` rows
  `x,y,z,t`. `vec3` operations follow S-008 R01-R03: `add/sub/div/min/max/neg/abs...` write `w = 0`;
  `mul/mulf` act on **all four lanes**; `dot` sums **all four lanes**.
  In this document `dot4(a,b) = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w` (summed in this order) and
  `norm3(v)` is S-008 `vec3_norm` (`v * (1/sqrt(dot4(v,v)))` on all lanes, zero vector when `dot4 <= 0`).
- `close_float(a, b, p)` is defined in R02.
- Numeric comparison in conformance: floats are printed with `%.9g`; compare with relative **or**
  absolute tolerance `1e-6`, or `1e-5` for results of `sin/cos/asin/atan2/powf`; `-0 == 0`;
  `nan` equals `nan`. Integers, bools, half bit patterns and byte strings compare **exactly**, except
  that the `*_srgb*` premultiply results (`powf`) may differ by **1 per byte**.
- Use `f32` arithmetic, in the order written below (the reference is compiled with SSE
  and no fused multiply-add). Do not widen to `f64`.
- `float_to_u8` etc. refer to section 5.7.

## 3. Interface

```c
struct vec2    { float x, y; };                         /* 8 bytes, align 4 */
struct matrix3 { struct vec3 x, y, z, t; };             /* 64 bytes, align 16: 4 rows of vec3 (16 bytes each, w lane live) */
struct plane   { struct vec3 dir; float dist; };        /* 32 bytes (dir at 0, dist at 16, padded) */
struct bounds  { struct vec3 min, max; };               /* 32 bytes */
struct half    { uint16_t u; };                         /* 2 bytes */
#define BOUNDS_OUTSIDE 1  /* 1 */   #define BOUNDS_INSIDE 2   #define BOUNDS_PARTIAL 3
#define EPSILON 1e-4f  #define LARGE_EPSILON 1e-2f  #define TINY_EPSILON 1e-5f  #define M_INFINITE 3.4e38f
#define RAD(v) ((v) * 0.0174532925199432957692369076848f)   #define DEG(v) ((v) * 57.295779513082320876798154814105f)
```

Exported (replaced by the crate): `vec2_abs vec2_floor vec2_ceil vec2_close vec2_norm`;
`vec3_plane_dist vec3_rotate vec3_transform3x4 vec3_mirror vec3_mirrorv vec3_rand`;
`matrix3_from_quat matrix3_from_axisang matrix3_from_matrix4 matrix3_mul matrix3_rotate matrix3_rotate_aa
matrix3_scale matrix3_transpose matrix3_inv matrix3_mirror matrix3_mirrorv`, `quat_from_matrix3`,
`matrix4_from_matrix3`;
`plane_from_tri plane_transform plane_transform3x4 plane_intersection_ray plane_intersection_line
plane_tri_inside plane_line_inside`;
`bounds_move bounds_scale bounds_merge bounds_merge_point bounds_get_point bounds_get_center
bounds_transform bounds_transform3x4 bounds_intersection_ray bounds_intersection_line bounds_plane_test
bounds_under_plane bounds_intersects bounds_intersects_obb bounds_intersects_obb3x4 bounds_min_dist`;
`polar_to_cart cart_to_polar norm_to_polar polar_to_norm calc_torquef calc_torque rand_float`.

Header-inline (plugins compile them in; the crate must offer equal behaviour): `vec2_zero/set/copy/add/sub/mul/div/
addf/subf/mulf/divf/neg/dot/len/dist/min/max/minf/maxf`, `matrix3_copy/identity/translate/translate3f/
rotate_aa4f/scale3f`, `plane_copy/set/setf/close/coplanar`, `bounds_zero/copy/inside/vec3_inside/
intersects_ray/intersects_line`, `get_percentage(i)`, `close_float`, `half_from_float`, `half_from_bits`,
and every function of `srgb.h` (`gs_srgb_*`, `gs_u8_*`, `gs_float*_to_u8*`, `gs_premultiply_*`).
`axisang_zero/copy/set` (S-008 area) are included in R34. Unless stated, no function allocates, locks
or logs; all are pure apart from writing the destination and are safe from any thread. Pointers
are never NULL (precondition). Destination aliasing is stated per function.

## 4. Data formats

There is no serialized format. `half` bit patterns (IEEE binary16: 1 sign, 5 exponent (bias 15), 10 mantissa bits)
are worked in section 5.6; vectors `22-half`, `23-half-rounding`, `24-half-overflow-quirk`.

## 5. Behaviour

### 5.1 `math-defs.h`

- **S-009-R01** Constants (as `f32`): `EPSILON = 1e-4`, `LARGE_EPSILON = 1e-2`, `TINY_EPSILON = 1e-5`,
  `M_INFINITE = 3.4e38` (a finite sentinel, not +inf). `RAD(v) = v * 0.017453292f32` (the product with the
  `f32` constant `0.0174532924`), `DEG(v) = v * 57.2957802f32`. `RAD(180) = 3.14159274`, `DEG(1) = 57.2957802`
  (vector `01`). `M_PI` is only defined by the header when the platform lacks it (then as the `f32`
  literal 3.14159274); on Linux it is the C library's `double`. Rust should expose `f32` and `f64` constants.
- **S-009-R02** `close_float(a, b, p) = abs(a - b) <= p` (inclusive). It is false when the difference is NaN,
  including **equal infinities** (`inf - inf = NaN`): `close_float(inf, inf, 1) = false`, `close_float(1e20, -1e20, inf) = true`.
- **S-009-R03** (informative) `RAD`/`DEG` are macros and evaluate in `f32` when the argument is `f32`
  (or an integer, which is converted to `f32` first).

### 5.2 `vec2`

- **S-009-R10** `vec2_set(x,y)`, `zero`, `copy`; `add/sub/mul/div` and `addf/subf/mulf/divf`, `neg` are
  component-wise with IEEE semantics (division by zero gives inf/NaN, no checks).
- **S-009-R11** `vec2_dot = x1*x2 + y1*y2`; `vec2_len = sqrt(x*x + y*y)` (no zero guard, NaN propagates);
  `vec2_dist(a,b) = len(a - b)`.
- **S-009-R12** `vec2_min(d, v, m)`: `d.c = (v.c < m.c) ? v.c : m.c` per component; `vec2_max`: `(v.c > m.c) ? v.c : m.c`;
  `minf/maxf` the same with a scalar as the second operand. A comparison involving NaN is false, so the **second
  operand is returned** (`min((nan,1),(5,nan)) = (5,nan)`). `vec2_abs`, `vec2_floor`, `vec2_ceil` are
  per-component `fabsf/floorf/ceilf`.
- **S-009-R13** All binary vec2 functions allow `dst` to alias an input (each component is computed from the
  inputs before the store of that component; the two components are independent).
- **S-009-R14** `vec2_close(a, b, eps) = close_float(a.x, b.x, eps) && close_float(a.y, b.y, eps)`: **absolute value**,
  inclusive; returns `int` 0/1. (Contrast: `vec3_close`/`vec4_close` are signed, S-008 Q-2; script `03`.)
- **S-009-R15** `vec2_norm(d, v)`: `len = sqrt(x*x + y*y)`; **only if `len > 0`**: `d = v * (1/len)`.
  Otherwise (zero, NaN length, or `len` underflowing to 0) **`d` is not written** (Q-1). An infinite
  length gives `v * 0`, i.e. NaN components for infinite inputs.

### 5.3 `vec3` functions that use plane / matrix3 (reference file `vec3.c`)

- **S-009-R20** `vec3_plane_dist(v, p) = dot4(v, p.dir) - p.dist`.
- **S-009-R21** `vec3_rotate(d, v, m)`: with `t = copy of v`: `d.x = dot4(t, m.x)`, `d.y = dot4(t, m.y)`,
  `d.z = dot4(t, m.z)`, `d.w = 0`. Note `dot4`: `v.w` and the `w` lanes of `m`'s rows take part (Q-2; script `05`).
  `d` may alias `v`.
- **S-009-R22** `vec3_transform3x4(d, v, m)`: `t = v - m.t` (`vec3_sub`: `w = 0`), then the same three dot products
  as R21 (so `v.w` is discarded but the matrix rows' `w` lanes times `0` contribute nothing), `d.w = 0`.
  This is the matrix3 convention "translate by `-t`, then dot with the rows" (see Q-12).
- **S-009-R23** `vec3_mirror(d, v, p)`: `k = vec3_plane_dist(v, p) * 2`; `d = v - p.dir * k` where the product is the
  four-lane `vec3_mulf` and the subtraction is `vec3_sub` (`w = 0`). Result has `w = 0`.
- **S-009-R24** `vec3_mirrorv(d, v, n)`: `k = dot4(v, n) * 2`; `d = v - n * k` (four-lane mulf, `vec3_sub`). The normal is used
  as given (not normalised). `w = 0`.
- **S-009-R25** `vec3_rand(d, positive_only)`: `d.x, d.y, d.z` are three successive calls of `rand_float(positive_only)`
  (in this order), `d.w = 0`. Only the range contract of R96 is specified.

### 5.4 `matrix3` (3x4: rows `x, y, z, t`, each a `vec3` with a live `w` lane)

- **S-009-R30** `matrix3_identity`: rows `x,y,z` are `(1,0,0,0) (0,1,0,0) (0,0,1,0)`, `t = (0,0,0,0)`.
  `matrix3_copy` copies all 16 floats.
- **S-009-R31** `matrix3_from_quat(d, q)`: identical formula to `matrix4_from_quat` (S-008 R34, including
  `n = dot4(q,q)`, `s = n > 0 ? 2/n : 0`, zero quaternion gives the identity rotation) but the rows are `vec3`
  (`w = 0`) and `t = (0,0,0,0)`. `matrix3_from_axisang(d, aa)` = `matrix3_from_quat(d, quat_from_axisang(aa))`
  (the axis is **not** normalised, script `28`).
- **S-009-R32** `matrix3_from_matrix4(d, m)`: copies the four rows and sets the `w` lane of **every** row to 0.
  `matrix4_from_matrix3(d, m)`: copies the four rows with **all four lanes** (so non-zero `w` lanes of rows `x,y,z`
  are preserved) and then sets `d.t.w = 1`.
- **S-009-R33** `quat_from_matrix3(q, m)` = S-008 R16 applied to the `x,y,z` lanes of rows `x,y,z` (the `w` lanes and `t` are not read).
- **S-009-R34** `axisang_zero` = `(0,0,0,0)`, `axisang_copy` copies four lanes, `axisang_set(x,y,z,angle)`; the conversions
  `axisang_from_quat` etc. are S-008 R14/R15. (Script `28` also shows `axisang_from_quat` for tiny vector parts.)
- **S-009-R35** `matrix3_mul(d, a, b)`: `d.x = vec3_rotate(a.x, b)`, `d.y = vec3_rotate(a.y, b)`, `d.z = vec3_rotate(a.z, b)`,
  `d.t = vec3_transform3x4(a.t, b)`. Spelled out for the `x,y,z` rows: `d.r.x = dot4(a.r, b.x)`, `d.r.y = dot4(a.r, b.y)`,
  `d.r.z = dot4(a.r, b.z)`, i.e. **each row of `a` is dotted with the rows of `b`** (the xyz block is `a * transpose(b)`
  in the usual row-vector sense, Q-12), and `d.t.c = dot4(a.t - b.t, b.c)` for `c in x,y,z`. All `w` lanes of the result are 0.
  `d` may alias `a` or `b`.
- **S-009-R36** `matrix3_translate(d, m, v)`: `d.t = m.t - v` (`vec3_sub`, **subtracts**; `w = 0`) and **nothing else is written** (rows `x,y,z`
  of `d` keep their previous contents, even when `d != m`). `matrix3_translate3f(d, m, x, y, z)` builds `v = (x,y,z)` and calls it.
- **S-009-R37** `matrix3_rotate(d, m, q) = matrix3_mul(d, m, matrix3_from_quat(q))`; `matrix3_rotate_aa(d, m, aa) = matrix3_mul(d, m, matrix3_from_axisang(aa))`;
  `matrix3_rotate_aa4f(d, m, x, y, z, angle)` builds `aa = (x,y,z,angle)`.
- **S-009-R38** `matrix3_scale(d, m, v)`: each of the four rows `r in x,y,z,t`: `d.r = vec3_mul(m.r, v)` (all four lanes, so
  `d.r.w = m.r.w * v.w`). `matrix3_scale3f` builds `v = (x,y,z)` with `w = 0`.
- **S-009-R39** `matrix3_transpose(d, m)` (results are functions of the *original* `m`; `d` may alias `m`):
  `d.t = -(dot4(m.t, m.x), dot4(m.t, m.y), dot4(m.t, m.z))` with `w = 0`;
  `d.x = (m.x.x, m.y.x, m.z.x, m.z.w)`, `d.y = (m.x.y, m.y.y, m.z.y, m.z.w)`, `d.z = (m.x.z, m.y.z, m.z.z, m.z.w)`
  — the transposed 3x3 block, **with the `w` lane of all three rows set to `m.z.w`** (Q-3). With zero `w` lanes this is
  the classic "inverse of a rigid transform" (rotation transposed, translation `-t·R`).
- **S-009-R40** `matrix3_inv(d, m)`: `M = matrix4_from_matrix3(m)` (R32: rows copied with their `w` lanes, `t.w = 1`);
  call `matrix4_inv` (S-008 R33: fails when `|det| < 0.0005`) writing into `d` reinterpreted as a `matrix4`;
  then **unconditionally** `d.t.w = 0`. If the inverse fails `d` is unchanged **except `d.t.w = 0`** (D-1).
  The result is the ordinary matrix inverse of the 4x4 matrix `[x;y;z;t]` (row vectors), which is **not** the inverse
  under the `matrix3_mul` convention of R35 (Q-12). `d` may alias `m`. For an affine input the `w` lanes of the inverse's
  rows are `0` (possibly `-0`).
- **S-009-R41** `matrix3_mirror(d, m, p)`: rows `x,y,z`: `d.r = vec3_mirrorv(m.r, p.dir)`; `d.t = vec3_mirror(m.t, p)`.
  `matrix3_mirrorv(d, m, n)`: all four rows with `vec3_mirrorv(m.r, n)`. `d` may alias `m`.

### 5.5 `plane`

- **S-009-R50** `plane_set(d, dir, dist)` copies `dir` (four lanes) and `dist`; `plane_setf(d, a, b, c, dist)` stores `dir = (a,b,c,0)`;
  `plane_copy` copies both. A plane is the set of points `p` with `dot4(p, dir) = dist`.
- **S-009-R51** `plane_from_tri(d, v1, v2, v3)`: `e1 = v2 - v1`; `e2 = v3 - v1`; `n = cross(e1, e2)` (S-008 R02);
  `d.dir = norm3(n)`; `d.dist = dot4(v1, d.dir)`. A degenerate triangle gives `dir = (0,0,0,0)`, `dist = 0`.
  Winding: counter-clockwise seen from the normal's side with the right-hand rule on `e1 x e2`.
- **S-009-R52** `plane_transform(d, p, m)` (`m` is a `matrix4`): `d.dir = norm3(vec3_transform(p.dir, m))` where `vec3_transform`
  is the **point** transform with `w = 1` (S-008 R07) — so the translation of `m` is *added to the normal* (Q-4);
  `o = vec3_transform((0,0,0), m)` (= the xyz of `m.t`); `d.dist = p.dist - dot4(d.dir, o)`. `d` may alias `p`.
- **S-009-R53** `plane_transform3x4(d, p, m)` (`m` a `matrix3`): `d.dir = norm3(vec3_transform3x4(p.dir, m))` (R22, i.e. `(dir - m.t)` dotted with the rows;
  again the normal is treated as a point, Q-4); `o = vec3_transform3x4(m.t, m)` — which is the dot of `m.t - m.t = 0`, so **`o` is always the zero vector** and
  `d.dist = p.dist` unchanged; `d` may alias `p`.
- **S-009-R54** `plane_intersection_ray(p, orig, dir, *t)`: `c = dot4(p.dir, dir)`. If `abs(c) < EPSILON` (strict): `*t = 0`, return false.
  Otherwise `*t = (p.dist - dot4(p.dir, orig)) / c`, return true. (`t` may be negative: the line, not the half-line.)
- **S-009-R55** `plane_intersection_line(p, v1, v2, *t)`: `d1 = vec3_plane_dist(v1, p)`, `d2 = vec3_plane_dist(v2, p)`.
  1. if `close_float(d1, 0, EPSILON)`: if `close_float(d2, 0, EPSILON)` return false (`*t` untouched); else `*t = 0`, return true.
  2. else if `close_float(d2, 0, EPSILON)`: `*t = 1`, return true.
  3. else if `(d1 > 0) == (d2 > 0)` return false (`*t` untouched).
  4. else `a = abs(d1)`, `s = a + abs(d2)`; if `s < EPSILON` return false (unreachable after step 1-2 for finite input); `*t = a / s`, return true.
- **S-009-R56** `plane_tri_inside(p, v1, v2, v3, prec)` / `plane_line_inside(p, v1, v2, prec)`: for each vertex `d = vec3_plane_dist(v, p)`; a vertex
  contributes flag `2` if `d >= prec`, else flag `1` if `d <= -prec`, else nothing. The functions return **true when any flag
  is set** (Q-5): i.e. when at least one vertex is at least `prec` away from the plane on either side — NOT "inside". With `prec = 0`
  every finite vertex sets a flag, so the result is true.
- **S-009-R57** `plane_close(a, b, prec) = vec3_close(a.dir, b.dir, prec) && close_float(a.dist, b.dist, prec)`. The first test is the **signed** `vec3_close` (S-008 R06, Q-6).
- **S-009-R58** `plane_coplanar(a, b, prec)`: `c = dot4(a.dir, b.dir)`; if `close_float(c, 1, prec)` return `close_float(a.dist, b.dist, prec)`;
  else if `close_float(c, -1, prec)` return `close_float(-a.dist, b.dist, prec)`; else false. Normals are assumed unit.

### 5.6 `bounds` (AABB: `min`, `max`, both `vec3`)

- **S-009-R70** `bounds_zero` zeroes both vec3; `bounds_copy` copies. `bounds_move(d, b, v)`: `min = vec3_add(b.min, v)`, `max = vec3_add(b.max, v)` (`w = 0`).
  `bounds_scale(d, b, v)`: `min = vec3_mul(b.min, v)`, `max = vec3_mul(b.max, v)` (four lanes) — **min/max are not re-sorted**, a negative factor gives `min > max` (Q-13).
  `bounds_merge(d, a, b)`: `min = vec3_min(a.min, b.min)`, `max = vec3_max(a.max, b.max)`; `bounds_merge_point(d, b, v)`: `min = vec3_min(b.min, v)`, `max = vec3_max(b.max, v)`
  (`w = 0`; `vec3_min(a,b)` is `a < b ? a : b` per lane). A box is not validated or normalised by any function. `d` may alias inputs.
- **S-009-R71** `bounds_get_point(d, b, i)`: if `i > 8` return without writing. Otherwise `d.x = (i >= 4) ? max.x : min.x`; `i' = i mod 4` for `i < 8`, **and for `i = 8`
  the same as `i = 6`** (Q-7: the guard is `i > 8`, not `i >= 8`); `d.y = (i' >= 2) ? max.y : min.y`; `d.z = (i'' == 1) ? max.z : min.z` where `i''` is `i'` with the y bit removed.
  Equivalently for `0 <= i <= 7`: bit 2 selects `max.x`, bit 1 selects `max.y`, bit 0 selects `max.z`; index 8 returns `(max.x, max.y, min.z)`.
  **Only `x, y, z` are written**; `d.w` keeps its previous value.
- **S-009-R72** `bounds_get_center(d, b) = ((b.max - b.min) * 0.5) + b.min` (`vec3_sub`, `vec3_mulf`, `vec3_add`; `w = 0`).
- **S-009-R73** `bounds_transform(d, b, m)` (`m` matrix4) and `bounds_transform3x4(d, b, m)` (`m` matrix3): the eight corners `bounds_get_point(b, 0..7)` are transformed
  with `vec3_transform` (point transform, `w=1`, S-008 R07) resp. `vec3_transform3x4` (R22), and `d` becomes the componentwise min and max over the eight results (computed into a temporary,
  so `d` may alias `b`). The comparisons are `p.c < min.c` then `else p.c > max.c`; NaN coordinates are never selected after the first corner. Result `w` lanes are 0. The result is the AABB of the OBB,
  generally larger (script `15`).
- **S-009-R74** `bounds_intersection_ray(b, orig, dir, *t)` (`dir` need not be unit; uses `x,y,z` only):
  ```
  tmax = M_INFINITE; tmin = -M_INFINITE
  c = bounds_get_center(b); h = b.max - c; off = c - orig          (vec3 ops)
  for i in x,y,z:
      e = off[i]; f = dir[i]
      if abs(f) > 0:
          fi = 1/f; t1 = (e + h[i]) * fi; t2 = (e - h[i]) * fi
          if t1 > t2:  { if t2 > tmin: tmin = t2;  if t1 < tmax: tmax = t1 }
          else:        { if t1 > tmin: tmin = t1;  if t2 < tmax: tmax = t2 }
          if tmin > tmax: return false
          if tmax < 0:    return false
      else if (-e - h[i]) > 0 or (-e + h[i]) < 0: return false
  *t = (tmin > 0) ? tmin : tmax;  return true
  ```
  `*t` is written only on success. If the origin is inside the box `*t` is the exit parameter (`tmax`); a zero direction with the origin inside yields `*t = 3.4e38` (`M_INFINITE`, script `16`).
  A box *behind* the origin is a miss; a box containing the origin hits.
- **S-009-R75** `bounds_intersection_line(b, p1, p2, *t)`: `dir = p2 - p1` (`vec3_sub`), `len = vec3_len(dir)`; if `len <= TINY_EPSILON` return false; `dir = dir * (1/len)` (four-lane mulf);
  if not `bounds_intersection_ray(b, p1, dir, t)` return false; `*t = *t / len`; return true. The segment is **not clipped** to `[0,1]`: `t` may exceed 1 (script `16`).
  `bounds_intersects_ray(b, orig, dir)` and `bounds_intersects_line(b, p1, p2)` return only the boolean of the two functions above.
- **S-009-R76** `bounds_plane_test(b, p)` is declared `bool` while the algorithm produces `BOUNDS_OUTSIDE (1)`, `BOUNDS_INSIDE (2)` or `BOUNDS_PARTIAL (3)`; after the conversion to `bool`
  **every outcome is `true`** (D-2). A conforming implementation MUST return `true` for every input (callers compiled against the reference cannot distinguish the cases).
  Rust-native code may offer a separate function returning the three-way classification: select, per axis, the box corner nearest to
  (`vmin`: `min.c` where `dir.c >= 0`, else `max.c`) and farthest along (`vmax`: the other corner) the plane normal; `OUTSIDE` if `plane_dist(vmin) > 0`, else `PARTIAL` if `plane_dist(vmax) >= 0`, else `INSIDE`.
- **S-009-R77** `bounds_under_plane(b, p)`: `vmin.c = (p.dir.c < 0) ? b.max.c : b.min.c` for `c in x,y,z`; return `dot4(vmin, p.dir) <= p.dist`, with `vmin.w = 0` (the reference leaves it uninitialised, D-4).
- **S-009-R78** `bounds_inside(b, test)`: all six `test.min.c >= b.min.c` and `test.max.c <= b.max.c` (inclusive, no epsilon). `bounds_vec3_inside(b, v)`: `v.c >= b.min.c - EPSILON && v.c <= b.max.c + EPSILON` for all `c`.
- **S-009-R79** `bounds_intersects(b, test, eps)`: true iff for each axis `c`: `(b.min.c - test.max.c) <= eps` and `(test.min.c - b.max.c) <= eps`. (A negative `eps` shrinks the boxes.)
- **S-009-R80** `bounds_intersects_obb(b, test, m, eps)` (`m` matrix4): `mi = matrix4_inv(m)` (the success result is ignored, D-5); `b_tr = bounds_transform(b, m)`; `test_tr = bounds_transform(test, mi)`;
  return `intersects(b, test_tr, eps) && intersects(b_tr, test, eps)`. `bounds_intersects_obb3x4(b, test, m, eps)` (`m` matrix3): `mi = matrix3_transpose(m)` (the *transpose* is the inverse for a rigid matrix3, R39);
  `b_tr = bounds_transform3x4(b, m)`; `test_tr = bounds_transform3x4(test, mi)`; same combination.
  Behaviour for a singular `m` in the matrix4 variant is undefined in the reference (uninitialised `mi`); a conforming implementation SHOULD return `false`.
- **S-009-R81** `bounds_min_dist(b, p)`: `half = dot4(b.max - b.min, abs(p.dir)) * 0.5` (`vec3_sub`, `vec3_abs`: both `w = 0`); `c = bounds_get_center(b)`; return `p.dist + vec3_plane_dist(c, p) - half`
  (evaluated left to right).

### 5.7 `math-extra`

- **S-009-R90** `polar_to_cart(d, v)` with `v = (elevation, azimuth, radius)`: `h = v.z * cos(v.x)`; `cart.x = h * sin(v.y)`, `cart.z = h * cos(v.y)`, `cart.y = v.z * sin(v.x)`; the whole
  result is computed before `d` is written (aliasing is safe). `d.w` is **unspecified** in the reference (uninitialised, D-3); a conforming implementation MUST write `0`. Conformance prints the `w` lane as `?`.
- **S-009-R91** `cart_to_polar(d, v)`: `r = vec3_len(v)` (four-lane `dot4` square root, 0 if `dot4 <= 0`); if `close_float(r, 0, EPSILON)`: `d = (0,0,0,0)` (all lanes);
  else `d.x = asin(v.y / r)`, `d.y = atan2(v.x, v.z)`, `d.z = r`, `d.w` unspecified (D-3, MUST be 0). All computed before the store.
- **S-009-R92** `norm_to_polar(d: vec2, n)`: `d.x = atan2(n.x, n.z)`, `d.y = asin(n.y)` (NaN when `|n.y| > 1`). `polar_to_norm(d: vec3, p: vec2)`: `s = sin(p.x)`; `d.x = s * cos(p.y)`,
  `d.y = s * sin(p.y)`, `d.z = cos(p.x)`; **`d.w` is not written**.
- **S-009-R93** `calc_torquef(val1, val2, torque, min_adjust, t)`:
  ```
  if close_float(val1, val2, EPSILON): return val2
  dist = (val2 - val1) * torque
  if dist > 0:  if dist < min_adjust: dist = min_adjust;  out = val1 + dist * t;  if out > val2: out = val2
  else:         if dist > -min_adjust: dist = -min_adjust; out = val1 + dist * t;  if out < val2: out = val2
  return out
  ```
  (`dist == 0` takes the second branch.) `t = 0` returns `val1`; a negative `t` moves away without a clamp on that side.
- **S-009-R94** `calc_torque(d, v1, v2, torque, min_adjust, t)`:
  ```
  if vec3_close(v1, v2, EPSILON): d = copy of v2 (all four lanes); return        -- SIGNED test (Q-8)
  line = v2 - v1;  od = vec3_len(line);  dir = line * (1/od)
  td = od * torque;  if td < min_adjust: td = min_adjust
  adj = td * t
  if adj <= od - LARGE_EPSILON:  d = dir * adj;  d = d + v1          -- (vec3_mulf into d, then vec3_add(d, d, v1))
  else:                          d = copy of v2 (all four lanes)
  ```
  Because `vec3_close` is `v1 - v2 < EPSILON` per component *without* absolute value (S-008 Q-2), the first branch is taken whenever `v1` is **below or equal to `v2` in all of x,y,z** (Q-8): the
  function only moves when at least one component of `v1` exceeds `v2` by `EPSILON` or more. `d` may alias `v2`. If `d` aliases `v1` the first store overwrites `v1`, so the
  result is `dir*adj + dir*adj` (Q-9; reproduce, SHOULD).
- **S-009-R95** `get_percentage(start, end, mid) = (mid - start) / (end - start)` in `f32` with no zero check (`0/0 = NaN`, `x/0 = inf`).
  `get_percentagei(start, end, mid) = (float)(mid - start) / (float)(end - start)` with the subtractions in `int`.
- **S-009-R96** `rand_float(positive_only)`: returns a uniformly distributed `f32` in `[0, 1]` when `positive_only` is non-zero, else in `[-1, 1]` (both ends inclusive). The generator is the C library's `rand()`
  (reference: `rand() / RAND_MAX` evaluated in `f64`, doubled and shifted for the signed case); the sequence and the seeding are **not specified**. Conformance (`21-rand`) checks only range, `w = 0` (vec3), and
  that the values are not all identical.

### 5.8 `half.h`

`half` is an IEEE-754 binary16 bit pattern (no `half -> float` function exists in this component).

- **S-009-R110** `half_from_bits(u).u == u`; `struct half` is 2 bytes.
- **S-009-R111** `half_from_float(f)` algorithm (all arithmetic on `u32`, wrapping):
  ```
  b = bits(f);  sign = (b & 0x80000000) >> 16;  a = b & 0x7FFFFFFF
  if a > 0x477FE000:                                  -- magnitude above 65504 (bits of 65504.0 are 0x477FE000), or inf/NaN
      r = ((a & 0x7F800000) == 0x7F800000 and (a & 0x7FFFFF) != 0) ? 0x7FFF : 0x7C00
  elif a == 0: r = 0
  else:
      if a < 0x38800000:                              -- below 2^-14: half subnormal range
          e = a >> 23                                  -- biased float exponent
          if e <= 81: r = 0                             -- see D-6 (reference undefined); required result: signed zero
          else: a = (0x800000 | (a & 0x7FFFFF)) >> (113 - e)
      else: a = a - 0x38000000                         -- rebias exponent (reference adds 0xC8000000 mod 2^32)
      if not already assigned r: r = ((a + 0x0FFF + ((a >> 13) & 1)) >> 13) & 0x7FFF
  return sign | r
  ```
  The last step is round-to-nearest, ties-to-even on the 13 dropped bits; a carry out of the mantissa increments the exponent (including subnormal to smallest normal and `65504`-adjacent values).
- **S-009-R112** Special values and examples (vectors `22`, `24`): `1.0 -> 0x3C00`, `-1 -> 0xBC00`, `0.5 -> 0x3800`, `65504 -> 0x7BFF`, `+0 -> 0x0000`, `-0 -> 0x8000`, `+inf -> 0x7C00`, `-inf -> 0xFC00`,
  **every NaN -> `0x7FFF` (positive) or `0xFFFF` (sign bit set)**: payload and quiet/signalling bit are not preserved. Smallest normal `2^-14 -> 0x0400`; `2^-24 -> 0x0001`; `-2^-24 -> 0x8001`; largest subnormal `0x1.ff8p-15 -> 0x03FF`.
  **Overflow threshold (Q-10):** any magnitude strictly greater than 65504 (including 65504.004, 65519.99, 65520) converts to infinity; IEEE round-to-nearest would map `(65504, 65520)` to 65504. Reproduce.
- **S-009-R113** Subnormal results (`|f| < 2^-14`): the shift in R111 **discards** the low bits of the float mantissa before rounding (no sticky bit; Q-11). A value a hair above a rounding tie therefore rounds like the tie when the extra
  bits are shifted out: e.g. `2^-25` (exact tie between 0 and 1 unit) gives `0` (even), `2^-25 * (1 + 2^-12)` gives `1`, but `2^-25 * (1 + 2^-13)` and `2^-25 * (1 + 2^-23)` give `0` (script `23`). Reproduce by using exactly the R111 integer algorithm.
  Magnitudes below `2^-25` give zero; between `2^-25` and `2^-24` give `0` or `1` per the above.

### 5.9 `srgb.h`

- **S-009-R120** `gs_srgb_nonlinear_to_linear(u) = (u <= 0.04045) ? u / 12.92 : powf((u + 0.055) / 1.055, 2.4)`; `gs_srgb_linear_to_nonlinear(u) = (u <= 0.0031308) ? 12.92 * u : (1.055 * powf(u, 1/2.4)) - 0.055`.
  Constants are `f32`, `1/2.4` is the `f32` quotient `1.0f / 2.4f`. Inputs below 0 take the linear branch (negative output); NaN fails the comparison and goes through `powf` (NaN); inputs above 1 are not clamped.
  `linear_to_nonlinear(1.0)` evaluates to `0.99999994` in `f32`. Use `powf` (not `f64` `pow`) to stay within the tolerance. `gs_float3_srgb_nonlinear_to_linear(f)` / `..._linear_to_nonlinear` apply the matching function to `f[0..2]`; `f[3]` is not touched.
- **S-009-R121** `gs_u8_to_float(u) = (f32)u / 255.0`. `gs_float_to_u8(f) = (u8)(f * 255.0 + 0.5)`, i.e. add 0.5 and **truncate toward zero**, **without clamping** (Q-14): defined only while `f*255+0.5` lies in the open interval `(-1, 256)`;
  outside that the reference has undefined behaviour (no vector). Rust MAY saturate there. Examples: `0.5 -> 128`, `0.002 -> 1`, `0.0019 -> 0`, `-0.004 -> 0`, `1.001 -> 255` (script `26`).
- **S-009-R122** `gs_u8x4_to_float4(f, u)`: `f[i] = u8_to_float(u[i])` for four bytes. `gs_float4_to_u8x4(u, f)`: four bytes. `gs_float3_to_u8x3(u, f)`: **three** bytes; `u[3]` is not written.
- **S-009-R123** `gs_premultiply_float4(f)`: `f[0] *= f[3]; f[1] *= f[3]; f[2] *= f[3]` (alpha unchanged, premultiply uses the *original* `f[3]`).
- **S-009-R124** `gs_premultiply_xyza(data)` (4 bytes, in place): `f = u8x4_to_float4`; premultiply; write `float3_to_u8x3` into the first three bytes; **the alpha byte is unchanged**.
  `gs_premultiply_xyza_restrict(dst, src)`: same computation reading `src`, writing **all four bytes** of `dst` (alpha copied). `dst` and `src` do not overlap (precondition).
  Example: `ff804080 -> 80402080`, `ffffff01 -> 01010101`, `ff804000 -> 00000000`.
- **S-009-R125** The `_srgb` forms insert `float3_srgb_nonlinear_to_linear` before the premultiply and `float3_srgb_linear_to_nonlinear` after it (on the colour channels only), then convert as in R124 (example: `ff804080 -> bc5d2d80`; ±1 per byte allowed).
  `*_loop(data, n)` applies the in-place form to `n` consecutive 4-byte texels; `*_loop_restrict(dst, src, n)` applies the restrict form to `n` texels; `n = 0` does nothing; bytes beyond `4*n` are untouched.

## 6. Algorithms

Algorithms are inline in section 5 (R74 ray/box slab test, R94 torque, R111 half conversion).

## 7. Concurrency

All functions are pure computations on caller memory with no static state (except `rand_float`, which uses the C library's `rand()` state: no thread-safety guarantee is made; the Rust version may use any thread-safe generator).
No callbacks, locks, or re-entrancy concerns.

## 8. Reference defects and quirks

| ID | Where | Reference behaviour | Required behaviour | Script |
|---|---|---|---|---|
| Q-1 | `vec2_norm` | zero/NaN-length vector: destination not written (unlike `vec3_norm`, which writes zeros) | reproduce | `03` |
| Q-2 | `vec3_rotate`, `matrix3_*` | four-lane dot: `v.w` and rows' `w` lanes affect `x,y,z` | reproduce | `05`, `09` |
| Q-3 | `matrix3_transpose` | `w` lane of all three rows = source `z.w` | reproduce | `09` |
| D-1 | `matrix3_inv` | on matrix4_inv failure `d` untouched but `d.t.w = 0` | reproduce | `08` |
| Q-4 | `plane_transform`, `plane_transform3x4` | normal transformed as a point (translation shifts it); the 3x4 variant never changes `dist` | reproduce | `11` |
| Q-5 | `plane_tri_inside`, `plane_line_inside` | true when any vertex is >= `prec` away from the plane (misnomer) | reproduce | `12` |
| Q-6 | `plane_close` | direction test is signed | reproduce | `12` |
| Q-7 | `bounds_get_point` | index 8 accepted (acts like 6); `w` not written | reproduce | `14` |
| D-2 | `bounds_plane_test` | `bool` return folds 1/2/3 to `true` | always `true` | `17` |
| D-3 | `polar_to_cart`, `cart_to_polar` | `w` lane uninitialised | write `0`; vectors mask `w` | `19` |
| Q-8 | `calc_torque` | uses signed `vec3_close` so v1 <= v2 componentwise snaps to v2 | reproduce | `20` |
| Q-9 | `calc_torque` | `dst == v1` corrupts the result | reproduce (SHOULD) | `20` |
| Q-10 | `half_from_float` | overflow to inf for any magnitude > 65504 | reproduce | `24` |
| Q-11 | `half_from_float` | subnormal path truncates before rounding | reproduce algorithm R111 | `23` |
| D-4 | `bounds_under_plane`, `bounds_plane_test` | `vmin.w`/`vmax.w` uninitialised, participates in `dot4` | treat as 0 | – |
| D-5 | `bounds_intersects_obb` | singular `m`: `matrix4_inv` fails, uninitialised `mi` is used | SHOULD return `false` | – |
| D-6 | `half_from_float` | shift count `113 - e` is >= 32 for `e <= 81` (|f| < 2^-45 and float subnormals): undefined; on x86-64 `-O0` gives garbage such as `0x0492` for some exponents (e.g. `e = 17, 49, 81`) | return signed zero | – (no vector, UB) |
| Q-12 | `matrix3_mul`, `vec3_transform3x4`, `matrix3_transpose` | the `matrix3` product dots rows with rows and subtracts translation (`x' = (x - t) · rows`); `matrix3_inv` is the ordinary 4x4 inverse and does not invert this product | reproduce | `07`, `08` |
| Q-13 | `bounds_scale` | negative scale leaves `min > max` | reproduce | `13` |
| Q-14 | `gs_float_to_u8` | not clamped | defined range only | `26` |

Also notable (CORE behaviour, flagged because they surprise): `vec2_close` is absolute while `vec3_close` is signed (R14); `matrix3_translate` subtracts and writes only `t` (R36);
`close_float(inf, inf)` is false (R02); `matrix4_from_matrix3` keeps the rows' `w` lanes (R32).

## 9. Implementation notes (non-normative)

- `#[repr(C)] struct Vec2 { x: f32, y: f32 }`; `#[repr(C, align(16))] struct Matrix3 { x: Vec3, y: Vec3, z: Vec3, t: Vec3 }` (64 bytes);
  `#[repr(C)] struct Plane { dir: Vec3, dist: f32 }` with `size_of == 32`; `#[repr(C)] struct Bounds { min: Vec3, max: Vec3 }`; `#[repr(transparent)] struct Half(u16)`.
- Implement the lane rules once (a `dot4` helper and a "w = 0" constructor) and reuse them; most surprises in this spec (Q-2, Q-12) are consequences of a four-lane dot.
- `matrix3_inv` casts a `matrix3` to a `matrix4`: both are 16 consecutive floats.
- Compute `half_from_float` with `u32` wrapping arithmetic exactly as R111; do not use `f32 -> f16` hardware or `half::f16` crates, whose overflow and subnormal rounding differ (Q-10, Q-11).
- `float_to_u8`: use `(f * 255.0 + 0.5) as u8` — Rust saturates where C is undefined, which is within the permission of R121.

## 10. Conformance coverage

| Rule | Scripts |
|---|---|
| R01-R03 | `01-defs` |
| R10-R13 | `02-vec2` |
| R14, R15 | `03-vec2-close-and-quirks` |
| R20-R24 | `04-vec3-plane-matrix3`, `05-vec3-w-lane-quirks` |
| R25, R96 | `21-rand` |
| R30-R33 | `06-matrix3-build` |
| R34 | `28-axisang-helpers` |
| R35-R39, R41 | `07-matrix3-ops`, `09-matrix3-transpose-w-lane` |
| R40 | `08-matrix3-inverse` |
| R50, R51, R54, R55 | `10-plane-basic` |
| R52, R53 | `11-plane-transform` |
| R56-R58 | `12-plane-tests` |
| R70, R72, R78 | `13-bounds-basic` |
| R71 | `14-bounds-points` |
| R73 | `15-bounds-transform` |
| R74, R75 | `16-bounds-ray-line` |
| R76, R77, R81 | `17-bounds-plane` |
| R79, R80 | `18-bounds-intersects` |
| R90-R92 | `19-math-extra-polar` |
| R93-R95 | `20-math-extra-torque` |
| R110-R112 | `22-half`, `24-half-overflow-quirk` |
| R111, R113 | `23-half-rounding` |
| R120 | `25-srgb-curves` |
| R121, R122 | `26-srgb-u8` |
| R123-R125 | `27-premultiply` |

## 11. Open questions

- `bounds_intersects_obb` with a singular matrix (D-5) and `half_from_float` below 2^-45 (D-6) are undefined in the reference; the required behaviour here is a recommendation, with no vector.
- `rand_float` quality/seeding: left to the implementation (the reference depends on whoever seeded `rand()`).
- Results of `powf/sinf/cosf/asinf/atan2f` are compared with tolerance; bit-exact agreement with the reference libm is not required.
