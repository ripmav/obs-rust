# S-008 `vec3`, `vec4`, `quat`, `matrix4`, `axisang` (core math types)

| | |
|---|---|
| Status | Ready for review (vec2, matrix3, plane, bounds, `math-extra`, `half`, sRGB → S-009, not yet written) |
| Reference | libobs 33.0.1 `graphics/vec3.*`, `vec4.*`, `quat.*`, `matrix4.*`, `axisang.*`, `srgb.h`, `math-defs.h` |
| Target crate | `crates/obs-math` |
| Depends on | – |
| Conformance | `test/conformance/scripts/math/` (8 scripts, runner `gen/math_runner.c`) |

## 1. Scope

These small types are used by every plugin that draws or transforms (scene items, filters,
transitions) and cross the ABI **by value inside structs and by pointer in ~150 functions**.
Most of the functions are `static inline` SSE code in the headers and stay in C; the exported
ones (those in `vec3.c`, `vec4.c`, `quat.c`, `matrix4.c`, `axisang.c`) are replaced. The Rust
types must be layout-compatible and behave identically, **including the unusual lane
semantics below**.

## 2. Conventions

- Vectors store **four `f32` lanes `[x, y, z, w]`** (16 bytes, 16-byte aligned); on x86 the C type is a union with `__m128`. `vec3` has a live fourth lane (`w`), normally kept 0.
- `quat` is lanes `[x, y, z, w]` with `w` the scalar part. `axisang` is `[x, y, z, w]` with `w` the **angle in radians**.
- `matrix4` is four `vec4` **rows**: `x`, `y`, `z`, `t` (16 floats, row-major in that order). Points are **row vectors**: `v' = v · M` (so the translation lives in row `t` and a transform chain reads left to right).
- Numeric comparison in conformance: expected values are printed with `%.9g`; compare with relative **or** absolute tolerance `1e-6` (`1e-5` after trigonometric functions). `-0` equals `0`.
- `EPSILON = 1e-4f`, `LARGE_EPSILON = 1e-2f`, `TINY_EPSILON = 1e-5f`; `close_float(a, b, p) = |a - b| <= p`.

## 3. Interface (layouts)

```c
struct vec3 { union { struct { float x, y, z, w; }; float ptr[4]; __m128 m; }; };   /* 16 bytes, align 16 */
struct vec4 { same };           struct quat { same };           struct axisang { same: x,y,z,w=angle };
struct matrix4 { struct vec4 x, y, z, t; };                     /* 64 bytes, align 16 */
```
Exported functions (replaced by the crate): `vec3_from_vec4`, `vec3_plane_dist`, `vec3_transform`, `vec3_rotate`, `vec3_transform3x4`, `vec3_mirror`, `vec3_mirrorv`, `vec3_rand`; `vec4_from_vec3`, `vec4_transform`; `quat_mul`, `quat_from_axisang`, `quat_from_matrix3`, `quat_from_matrix4`, `quat_get_dir`, `quat_set_look_dir`, `quat_log`, `quat_exp`, `quat_interpolate`, `quat_get_tangent`, `quat_interpolate_cubic`; `matrix4_from_matrix3`, `matrix4_from_quat`, `matrix4_from_axisang`, `matrix4_mul`, `matrix4_mul_4x3_only` (not in the public header), `matrix4_determinant`, `matrix4_translate3v/4v(_i)`, `matrix4_rotate(_aa)(_i)`, `matrix4_scale(_i)`, `matrix4_inv`, `matrix4_transpose`; `axisang_from_quat`. The remaining listed functions are inline (see section 5) and define the behaviour Rust-side wrappers must reproduce.

## 4. Lane rules for inline vector functions

- **S-008-R01** `vec3_set(d, x, y, z)` stores `w = 0`. `vec4_set(d, x, y, z, w)` stores all four.
- **S-008-R02** `vec3_add`, `vec3_sub`, `vec3_div`, `vec3_addf`, `vec3_subf`, `vec3_divf`, `vec3_min(f)`, `vec3_max(f)` compute lane-wise on `x,y,z` and **set `w = 0`**. `vec3_mul` and `vec3_mulf` compute **all four lanes**, so the result `w` is `w1·w2` (resp. `w·f`) (Q-1). `vec3_neg`, `vec3_abs`, `vec3_floor`, `vec3_ceil` act on `x,y,z` and set `w = 0`. `vec3_cross` computes the usual cross product of `x,y,z`; its `w` is `0` (vector `01`, `02`).
- **S-008-R03** `vec3_dot(a, b)` sums **all four lanes**: `ax·bx + ay·by + az·bz + aw·bw` (Q-1; vector `02` `dw = 106` for w = 10, 10). `vec3_len(v) = dot(v,v) > 0 ? sqrt(dot(v,v)) : 0`. `vec3_dist(a,b) = len(a − b)` (using `vec3_sub`, so `w` of the difference is 0 and the distance is over x,y,z only). `vec3_norm(d, v)`: if `dot(v,v) > 0` multiply **all lanes** by `1/sqrt(dot)`; else the result is the zero vector (all four lanes 0).
- **S-008-R04** `vec4_*` equivalents operate on all four lanes with no special-casing: `vec4_dot` sums four products; `vec4_neg` negates all four; `vec4_norm` same rule as vec3.
- **S-008-R05** `vec3_from_vec4(d, v)`: copy lanes, **`w = 0`**. `vec4_from_vec3(d, v)`: copy `x,y,z`, **`w = 1`**.
- **S-008-R06** `*_close(a, b, eps)` (vec3, vec4, quat) compute `t = a − b` and return `t.x < eps && t.y < eps && t.z < eps [&& t.w < eps for vec4/quat]` — **signed differences, no absolute value** (Q-2). So `vec3_close(a, far)` is true when `a − far` is very negative.
- **S-008-R07** `vec4_transform(d, v, M)`: `d.x = Σᵢ vᵢ·M[i].x`, `d.y = Σᵢ vᵢ·M[i].y`, `d.z = Σᵢ vᵢ·M[i].z`, `d.w = Σᵢ vᵢ·M[i].w` where `M[0..3] = x,y,z,t` (i.e. `d = v · M`). `vec3_transform(d, v, M)`: `v4 = (x, y, z, 1)`; `d = (v4 · M).xyz`, `w = 0`; **no perspective divide**.
- **S-008-R08** `gs_float_to_u8(f) = (uint8)(f·255 + 0.5)` — **no clamping** (inputs outside `[0,1]` are undefined behaviour); `vec4_to_rgba` packs `R,G,B,A` into memory order bytes `[R,G,B,A]` as a native-endian `uint32` (LE: `0xAABBGGRR`); `vec4_to_bgra` swaps R and B. `(1, 0.5, 0, 1)` → `0xff0080ff` (vector `03`). `vec4_from_rgba`/`bgra`: `byte / 255.0`. `vec4_from_rgba_srgb`: as `from_rgba` then convert `x,y,z` with the sRGB-to-linear curve (`u ≤ 0.04045 ? u/12.92 : ((u+0.055)/1.055)^2.4`).

## 5. Quaternions

- **S-008-R10** `quat_identity` = `(0,0,0,1)`. `quat_add/sub/addf/subf/mulf/divf` are lane-wise on all four lanes. `quat_neg` negates all four. `quat_dot` sums four products; `quat_len`, `quat_dist`, `quat_norm` as for vec4. `quat_norm` of a zero quaternion is `(0,0,0,0)`.
- **S-008-R11 (DEFECT D-1)** `quat_set(d, x, y, z, w)` stores its arguments **reversed**: the result has `x = w_arg, y = z_arg, z = y_arg, w = x_arg` (vector `07`: `quat_set(1,2,3,4)` → `(4,3,2,1)`). A conforming Rust implementation of the exported ABI symbol **MUST** reproduce this because callers compiled against the reference header receive the same behaviour. (No in-tree use is known; new Rust code must not call it.)
- **S-008-R12 (DEFECT D-2)** `quat_inv(d, q)` writes only `x,y,z` (negated) and **leaves `d.w` unchanged** (vector `07`). Callers pass `d` with a valid `w` or copy first. Rust wrappers MUST not write `w`.
- **S-008-R13** `quat_mul(d, q1, q2)`: with `a1 = (q1.x,q1.y,q1.z)`, `a2 = (q2.x,q2.y,q2.z)`: `d.xyz = q1.w·a2 + q2.w·a1 + a1 × a2`; `d.w = q1.w·q2.w − a1·a2`. (Hamilton product, `q1` applied as the *left* factor; vector `06` `mul` vs `mul2`.)
- **S-008-R14** `quat_from_axisang(d, aa)`: `s = sin(aa.w/2)`; `d = (aa.x·s, aa.y·s, aa.z·s, cos(aa.w/2))`. The axis is **not** normalised.
- **S-008-R15** `axisang_from_quat(d, q)`: `len = x²+y²+z²`; if `|len| ≤ EPSILON` → `(0,0,0,0)`; else `d.xyz = q.xyz / sqrt(len)`, `d.w = 2·acos(q.w)`.
- **S-008-R16** `quat_from_matrix4(d, m)` (and `from_matrix3`, treating the first three rows): `tr = m.x.x + m.y.y + m.z.z`. If `tr > 0`: `s = sqrt(tr + 1)`; `d.w = s/2`; `h = 0.5/s`; `d.x = (m.y.z − m.z.y)·h`, `d.y = (m.z.x − m.x.z)·h`, `d.z = (m.x.y − m.y.x)·h`. Else: `i = (m.x.x > m.y.y) ? 0 : 1; if (m.z.z > M[i][i]) i = 2; j = (i+1)%3; k = (i+2)%3;` `s = sqrt(M[i][i] − M[j][j] − M[k][k] + 1)`; `d[i] = s/2`; `h = 0.5/s`; `d[j] = (M[i][j] + M[j][i])·h`; `d[k] = (M[i][k] + M[k][i])·h`; `d.w = (M[j][k] − M[k][j])·h` where `M[a][b]` is element `b` of row `a`, rows indexed 0=x,1=y,2=z.
- **S-008-R17** `quat_get_dir(d, q)`: build `matrix3_from_quat(q)` and return its `z` row (the rotated +Z axis; vector `06` `dir`).
- **S-008-R18** `quat_set_look_dir(d, dir)`: `n = −normalize(dir)`; `xz_valid = |n.x| ≤ EPSILON || |n.z| ≤ EPSILON`; `yz_valid = |n.y| ≤ EPSILON` (**these predicates test "close to zero", not "non-zero"**, Q-3); `xz = identity, yz = identity`; if `xz_valid`: `xz = from_axisang((0,1,0), atan2f(n.x, n.z))`; if `yz_valid`: `yz = from_axisang((−1,0,0), asinf(n.y))`; result: if `!xz_valid` the result is `yz` (identity unless `yz_valid`); else if `!yz_valid` the result is `xz`; else `quat_mul(xz, yz)`. A direction like `(1,1,1)` (no component close to zero) therefore yields the identity. (Vector `08`: four inputs.)
- **S-008-R19** `quat_log(d, q)`: `angle = acos(q.w)`, `sine = sin(angle)`; `d = q` with `w = 0`; if `|q.w| < 1 && |sine| ≥ EPSILON`: multiply all lanes by `angle/sine` (incl. the zeroed `w`). `quat_exp(d, q)`: `len = |q.xyz|`; `f = len > EPSILON ? sin(len)/len : 1`; `d = q · f` (all lanes); then `d.w = cos(len)`.
- **S-008-R20** `quat_interpolate(d, q1, q2, t)`: `dot = quat_dot(q1,q2)`, `a = acos(dot)` (**no clamping of `dot`**; `|dot| > 1` gives NaN). If `a ≥ EPSILON`: `d = q1·sin(a(1−t))/sin(a) + q2·sin(a t)/sin(a)`; else `d = q1 + (q2 − q1)·t`. No shortest-arc correction.
- **S-008-R21** `quat_get_tangent(d, prev, q, next)` = `((q − prev) + next − q) · 0.5`. `quat_interpolate_cubic(d, q1, q2, m1, m2, t)` = `interpolate(interpolate(q1,q2,t), interpolate(m1,m2,t), 2(1−t)t)`.

## 6. Matrices

- **S-008-R30** `matrix4_identity`: rows `(1,0,0,0)`, `(0,1,0,0)`, `(0,0,1,0)`, `(0,0,0,1)`. `matrix4_transpose(d, m)` is the ordinary transpose and is safe when `d == m`. `matrix4_copy` copies four rows.
- **S-008-R31** `matrix4_mul(d, m1, m2)`: `d[r].c = Σₖ m1[r].k · m2[k].c` (rows of `m1` times columns of `m2`; result = apply `m1` first, then `m2` to a row vector). Safe when `d` aliases an input. `matrix4_mul_4x3_only(d, m1, m2)`: computes the first three rows of `m1·m2` exactly as above and **copies `m2.t` unchanged into `d.t`** (assumes `m1`'s last column is `(0,0,0,1)` and ignores `m1.t`).
- **S-008-R32** `matrix4_determinant`: expansion along the first row with 3×3 cofactors: `Σⱼ (−1)ʲ · m[0][j] · det₃(minor(0,j))`, `det₃` the usual 3×3 determinant.
- **S-008-R33** `matrix4_inv(d, m)`: `det = determinant(m)`; **if `|det| < 0.0005` return `false` and leave `d` unmodified**; else `d[j].component[i] = (−1)^(i+j) · det₃(minor(i,j)) / det` for all `i, j` (adjugate/determinant), return `true`. (An invertible matrix with a determinant below the threshold — e.g. a pure scale by 0.0003 — is reported singular, vector `04`.) `d == m` is allowed.
- **S-008-R34** `matrix4_from_quat(d, q)`: `n = quat_dot(q,q)`, `s = n > 0 ? 2/n : 0`; with `xx = q.x²s`, `yy = q.y²s`, `zz = q.z²s`, `xy = q.x q.y s`, `xz = q.x q.z s`, `yz = q.y q.z s`, `wx = q.w q.x s`, `wy = q.w q.y s`, `wz = q.w q.z s`: rows `x = (1−(yy+zz), xy+wz, xz−wy, 0)`, `y = (xy−wz, 1−(xx+zz), yz+wx, 0)`, `z = (xz+wy, yz−wx, 1−(xx+yy), 0)`, `t = (0,0,0,1)`. A zero quaternion therefore yields the identity. `matrix4_from_axisang` = `from_quat(from_axisang(aa))`. `matrix4_from_matrix3` copies the four rows and sets `t.w = 1`.
- **S-008-R35** Composition helpers (`dst = m ∘ op`, where `op` is the new matrix and the product is `matrix4_mul(dst, m, op)`): `matrix4_translate3v/4v(dst, m, v)`: `op` = identity with row `t = (v, 1)` or `v`; `matrix4_rotate(dst, m, q)`: `op = from_quat(q)`; `matrix4_rotate_aa`: `op = from_axisang`; `matrix4_scale(dst, m, v)`: `op = diag(v.x, v.y, v.z, 1)`. 
- **S-008-R36** The `_i` ("inverse order") variants apply the operation **before** `m`: `matrix4_translate3v_i(dst, v, m)`: rows `x,y,z` copied from `m`; `dst.t = (v, 1) · m`, i.e. `dst.t.c = Σₖ v4ₖ · m[k].c` with `v4 = (v.x, v.y, v.z, 1)` and `m[k]` the k-th row (translation applied first, then `m`; vector `05` `ti` = `20 60 120 1`); `matrix4_translate4v_i` same with a 4-vector. `matrix4_rotate_i(dst, q, m)` = `mul_4x3_only(from_quat(q), m)`; `matrix4_rotate_aa_i` likewise; `matrix4_scale_i(dst, v, m)` = `mul_4x3_only(diag(v,1), m)`. Vectors `05` list inputs and outputs.

## 7. Reference defects and quirks

- **Q-1 (vec3 `w` lane).** `dot`, `mul`, `mulf` include `w`; callers keep `w = 0`, but the lane is observable (vector `02`). Required: reproduce.
- **Q-2 (`*_close` is signed).** See R06. Required: reproduce (SHOULD; documented hazard).
- **Q-3 (look-dir predicates).** `quat_set_look_dir` tests closeness to zero (R18). Required: reproduce.
- **D-1 `quat_set` reversed**, **D-2 `quat_inv` leaves `w`** (R11, R12). Required: reproduce (ABI-visible).
- **Q-4 (no clamping)** in `quat_interpolate` and `gs_float_to_u8` (R20, R08). Required: reproduce; NaN results are acceptable and compared as "both NaN".
- **Q-5 (x86 SSE vs scalar).** The reference uses SSE; on ARM `simde` emulates the same lane semantics. Results may differ in the last ulp between builds: compare with the tolerance in section 2.
- **Q-6 (`vec3_rand` / `rand_float`)** depend on the C library `rand()`; not specified (outputs are in `[0,1]` or `[−1,1]` — see S-009).

## 8. Implementation notes (non-normative)

- `#[repr(C, align(16))] pub struct Vec3 { pub x: f32, pub y: f32, pub z: f32, pub w: f32 }` (same for Vec4/Quat/AxisAng); `#[repr(C)] pub struct Matrix4 { pub x: Vec4, pub y: Vec4, pub z: Vec4, pub t: Vec4 }`; assert `size_of::<Matrix4>() == 64`.
- Implement the exported functions with plain `f32` arithmetic in the **same operation order** as the formulas above (summation order changes the last bits); use `f32::sin_cos` etc., not `f64`.
- Do not "fix" D-1, D-2 or Q-2 in the exported C symbols; fix them in the Rust-native API only.

## 9. Conformance coverage

| Rule | Scripts |
|---|---|
| R01–R03 | `01-vec3`, `02-vec3-w-lane-and-close` |
| R04, R05 | `03-vec4` |
| R06 | `02`, `03` |
| R07, R08 | `03`, `04` (transforms), `03` (`rgba`) |
| R10, R13 | `06-quat` |
| R11, R12 | `07-quat-set-and-inv-defects` |
| R14–R17 | `06-quat` (from axis-angle, matrix round-trips, dir) |
| R18–R21 | `08-quat-interpolation` |
| R30–R33 | `04-matrix4-basic` |
| R34–R36 | `05-matrix4-compose`, `06-quat` |
| `quat_from_matrix4` non-trace branch | `06-quat` (`back3`, `back4`) |
| `vec3_rand`, `matrix3`, `plane`, `bounds`, `vec2` | S-009 |

## 10. Open questions

- Behaviour of `quat_from_matrix4` for a matrix that is not a pure rotation (scale/shear) follows the formula; no additional guarantees are made.
- `vec3_close` / `*_close` intentionally preserve the signed comparison; whether to fix it in a future ABI version is a 

## 11. Round 9: remaining math headers — vec2/vec3/vec4/quat inlines, matrix3, plane, bounds, axisang, math-extra (added)

### Reference files (all cited)

- `libobs/graphics/vec2.h`, `libobs/graphics/vec2.c`, `libobs/graphics/vec3.h`, `libobs/graphics/vec4.h`, `libobs/graphics/quat.h`
- `libobs/graphics/matrix3.c`, `libobs/graphics/matrix3.h`, `libobs/graphics/matrix4.h`
- `libobs/graphics/plane.c`, `libobs/graphics/plane.h`
- `libobs/graphics/bounds.c`, `libobs/graphics/bounds.h`
- `libobs/graphics/axisang.h`, `libobs/graphics/math-extra.c`

- **S-008-R37** `vec2.h`/`vec2.c`: the vec2 inline set (`vec2_zero`, `vec2_set`, `vec2_copy`, `vec2_dist`, `vec2_min`/`vec2_max` ± the `f` float variants) and `vec2_norm` in `vec2.c` — same conventions as the pinned vec3/vec4 rules (division by zero → zero vector).
- **S-008-R38** `vec3.h`: the full vec3 inline set (`vec3_add`/`sub`/`div` ± `f`, `dot`, `cross`, `neg`, `len`, `dist`, `norm`, `close`, `min`/`max`, scale/mul helpers) — element-wise semantics per the earlier sections.
- **S-008-R39** `vec4.h`: `vec4_dot`, `neg`, `len`, `dist`, `norm`, `close`, `abs`, `floor`, `ceil` plus the channel converters: `vec4_to_rgba`/`vec4_to_bgra` (0–1 floats → 0–255 bytes) and `vec4_from_rgba`/`vec4_from_bgra`/`vec4_from_rgba_srgb` (bytes → floats, srgb = linearized) — rounding/gamma per the earlier sections.
- **S-008-R40** `quat.h`: `quat_identity`, `dot`, `inv` (conjugate), `neg`, `len`, `dist`, `norm`, `close` — quaternion conventions per the earlier sections.
- **S-008-R41** `matrix3.c`/`matrix3.h`: 3×3 construction (`matrix3_from_quat`, `matrix3_from_axisang`, `matrix3_from_matrix4` — the 3×3 corner of a matrix4), `matrix3_mul`, rotations (`matrix3_rotate` quat / `matrix3_rotate_aa` axis-angle), `scale`, `transpose`, `inv`, `mirror`/`mirrorv`; inlines `matrix3_copy`, `matrix3_identity`, `translate3f`, `rotate_aa4f`, `scale3f` — element layout as pinned for `matrix4` in this spec. `matrix4.h`: the `matrix4_copy`/`matrix4_identity`/`matrix4_translate3f`/`matrix4_rotate_aa4f`/`matrix4_scale3f` inlines.
- **S-008-R42** `plane.c`/`plane.h`: a plane is point + normal: `plane_from_tri` (normal from the triangle edges, normalized), `plane_transform`/`plane_transform3x4` (rotate point and normal), `plane_intersection_ray`/`plane_intersection_line` (parameter t; parallel/no-hit → false), `plane_tri_inside`/`plane_line_inside` (which side of the plane); inlines `plane_copy`, `plane_set`/`plane_setf`, `plane_coplanar` (epsilon test).
- **S-008-R43** `bounds.c`/`bounds.h`: bounds manipulation — `bounds_move`, `bounds_scale`, `bounds_merge`/`bounds_merge_point`, `bounds_get_point`/`bounds_get_center`, `bounds_transform` (quat + scale + translate) /`bounds_transform3x4`; testing: `bounds_intersection_ray`/`_line` (t + point), `bounds_plane_test`/`bounds_under_plane`, `bounds_intersects` (AABB–AABB), `bounds_intersects_obb` (separating axes); inlines `bounds_zero`, `bounds_copy`, `bounds_inside`, `bounds_vec3_inside`, `bounds_intersects_ray`/`_line`.
- **S-008-R44** `axisang.h`: `axisang_zero`/`axisang_copy`/`axisang_set`. `math-extra.c`: `polar_to_cart`/`cart_to_polar` (r, θ ↔ x, y), `norm_to_polar`/`polar_to_norm` (unit vector ↔ angle), `calc_torque`/`calc_torquef` (F × r), `rand_float` (pseudo-random float in [0,1)).project decision.
