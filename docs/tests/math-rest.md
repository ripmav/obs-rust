# Test matrix: math-rest — plane, bounds, vec2/vec3 helpers, matrix3 rebuild, math-extra, half, sRGB (obs-math)

- Test files: `crates/obs-math/src/{plane,bounds,vec2,vec3,matrix3,math_extra,half,srgb}.rs` (unit tests per module)
- Task: WP-10 (P1-15.1/.2, P1-16.1–.3, P1-17.3, P1-18.1–.3 — spec S-009; Draft,
  second-author check pending). Supersedes WP-5's provisional `Matrix3`
  (3×`Vec4`, 48 bytes) with the C 3×4 layout (4×`Vec3`, 64 bytes).
- Golden files: none (C conformance vectors arrive with the C checkout;
  expectations here are from the spec's worked values + structural
  invariants, compared at the spec tolerance: relative **or** absolute
  1e-6, `-0` == 0). Bit-exact sections (half) compare the 16-bit pattern.

| Case | Input | Expected | Platforms | Status |
|---|---|---|---|---|
| math-defs (R01/R02) | constants | `M_INFINITE = 3.4e38` (finite sentinel, not `f32::INFINITY`); `M_PI_F32 = 3.14159274`; `rad(180) = π_f32` (worked); `deg(π_f32) = 180` (worked); `close_float` signed-inclusive, equal infinities true, `inf − inf` NaN false, large values at the inf-precision edge | Linux, Windows, macOS | pass |
| vec2 R10 component ops | `(3,4)` vs `(1,2)`, live `w`-free | add/sub/mul/div + scalar `±f/mulf/divf`, neg — all 2 lanes, C aliasing `d = a op b` via copy (Rust `&mut` forbids the in-place alias) | Linux, Windows, macOS | pass |
| vec2 R11 dot/len | `(3,4,0,w)` live `w` | `dot = x·x + y·y` only (2 lanes); `len((3,4)) = 5`; zero vector → `len = 0`, no guard (NaN propagates) | Linux, Windows, macOS | pass |
| vec2 R12 min/max | NaN operands | **second operand returned** when the comparison is false (C conditional semantics, not `f32::min`/`max`); ordinary values pinned | Linux, Windows, macOS | pass |
| vec2 abs/floor/ceil | negative, fractional | lane-wise `libm`-equivalent (Rust `abs`/`floor`/`ceil`) | Linux, Windows, macOS | pass |
| vec2 R14 close | `(0,0)` vs `(±eps)` | absolute, inclusive; NaN fails | Linux, Windows, macOS | pass |
| vec2 R15/Q-1 norm | `(3,4)`; zero; NaN-length | `(0.6,0.8)`; zero/NaN length → **destination not written** (unlike `vec3_norm` which writes zeros — Q-1, the WP-5 divergence fixed here) | Linux, Windows, macOS | pass |
| vec3 R20 plane_dist | `w`-lane live | `dot4(v, p.dir) − p.dist` (four lanes) | Linux, Windows, macOS | pass |
| vec3 R21 rotate | matrix3 row `w` lanes live | `d = v · M` over 4 lanes (Q-2: `v.w` participates); `d.w = 0` | Linux, Windows, macOS | pass |
| vec3 R22 transform3x4 | matrix3 3×4 | translate part subtracts `m.t` first (Q-12 order); `d.w = 0` | Linux, Windows, macOS | pass |
| vec3 R23 mirror / mirrorv | plane `y = 2`; normal ±(0,2,0) | reflection across plane / across normal line; normal need not be unit (mirrorv normalises); `d.w = 0` | Linux, Windows, macOS | pass |
| vec3 R24 rand | 1000 samples | each lane in `[-1, 1]`; `w = 0`; not all identical | Linux, Windows, macOS | pass |
| matrix3 layout (S-009 §3) | size/align/offsets | 64 bytes, align 16; rows `x, y, z, t` each a 4-lane `Vec3` at offsets 0/16/32/48 (the C 3×4) | Linux, Windows, macOS | pass |
| matrix3 R30 identity/copy | — | identity = diag(1) with `t = (0,0,0,0)`; copy exact | Linux, Windows, macOS | pass |
| matrix3 R31 from_quat/axisang | zero + 90°/Z | zero quat → identity; rotation rows at the 1e-6 tolerance (Q-5: `FRAC_1_SQRT_2` approximate) | Linux, Windows, macOS | pass |
| matrix3 R32 matrix4 conversions | both directions | `matrix4_from_matrix3`: rows `x,y,z` keep their `w` lanes, `t = (m.t, 1)`; `from_matrix4` takes the top 3×4 | Linux, Windows, macOS | pass |
| matrix3 R33 quat_from_matrix3 | rotation matrix | `quat_from_matrix3(m) == quat_from_matrix4(matrix4_from_matrix3(m))` within tolerance | Linux, Windows, macOS | pass |
| matrix3 R35 mul (Q-12) | non-commuting pair | rows dot rows; `d.t = dot4(a.t − b.t, b rows)`; result `w` lanes 0; alias-safe (`a·a`) | Linux, Windows, macOS | pass |
| matrix3 R36 translate | rotation matrix | **only `d.t` written** (`d.t = m.t − v`); `x,y,z` keep their previous content (C behaviour) | Linux, Windows, macOS | pass |
| matrix3 R37/R38 rotate/scale | 90°/Z, `S(2)` | composed with the existing matrix; `rotate_aa4f`/`scale3f` scalar forms | Linux, Windows, macOS | pass |
| matrix3 R39 transpose (Q-3) | asymmetric matrix | transposed 3×3 block, `w` lane of all three rows = `m.z.w`; `d.t` = negated dots of `m.t` | Linux, Windows, macOS | pass |
| matrix3 R40 inv | singular + rotation | `inverse · M ≈ I`; singular → `d` untouched but `d.t.w = 0` (D-1) | Linux, Windows, macOS | pass |
| matrix3 R41 mirror/mirrorv | plane | rows via `vec3_mirrorv`, `t` via `vec3_mirror` | Linux, Windows, macOS | pass |
| plane layout (R50) | size/align/offsets | 32 bytes, align 16; `dir @ 0`, `dist @ 16` | Linux, Windows, macOS | pass |
| plane R50 set/setf/copy | `dir` with live `w` | `set` copies all four lanes as-is; `setf` goes through `vec3_set` (`w = 0`) | Linux, Windows, macOS | pass |
| plane R51 from_tri | degenerate + normal tri | `dir = (b−a)×(c−a)` normalised (zero cross → zero dir, dist from `a`); worked values | Linux, Windows, macOS | pass |
| plane R52 transform (Q-4) | translation + rotation | normal transformed **as a point** (translation shifts it); dist re-projected | Linux, Windows, macOS | pass |
| plane R53 transform3x4 | 3×4 matrix | as R52 via matrix3; **dist never changes** (Q-4) | Linux, Windows, macOS | pass |
| plane R54 intersection_ray | parallel, hitting, behind | `dot4(dir) == 0` (within the C epsilon) → none; `t` in ray units; worked values | Linux, Windows, macOS | pass |
| plane R55 intersection_line | parallel + crossing | same test over the line direction; worked values | Linux, Windows, macOS | pass |
| plane R56 tri/line_inside (Q-5) | vertex far from plane | **true when any vertex is ≥ `prec` away** (misnomer — reproduces the C); worked values | Linux, Windows, macOS | pass |
| plane R57 close | signed direction test | `dist` difference within `prec`, **direction test is signed** (Q-6) | Linux, Windows, macOS | pass |
| plane R58 coplanar | two planes | both close tests (direction and distance) | Linux, Windows, macOS | pass |
| bounds layout (R70) | size/align | 32 bytes, align 16; `min @ 0`, `max @ 16` | Linux, Windows, macOS | pass |
| bounds R70 zero/copy/move/scale/merge | negative scale | move = copy; **negative scale gives `min > max`, never re-sorted** (Q-13); merge of disjoint boxes | Linux, Windows, macOS | pass |
| bounds R71 get_point (Q-7) | indices 0–7, 8 | `bit2/bit1/bit0 → max.x/max.y/max.z`; **index 8 accepted and acts like 6**; `w` never written (sentinel) | Linux, Windows, macOS | pass |
| bounds R72 get_center | `(0,2,4)-(1,3,5)` | `(0.5, 2.5, 4.5)` | Linux, Windows, macOS | pass |
| bounds R73 transform/transform3x4 | rotation 45°/Z | 8 point-transformed corners → AABB of the OBB; NaN never selected after the first corner; alias-safe | Linux, Windows, macOS | pass |
| bounds R74 intersection_ray | inside, outside, grazing | the spec slab pseudocode verbatim; strict `abs(f) > 0`; `*t` only on success; origin inside → `tmax`; zero direction inside → `M_INFINITE` (the finite sentinel) | Linux, Windows, macOS | pass |
| bounds R75 intersection_line | crossing segment | `len <= TINY_EPSILON` fails; **not clipped to [0,1]** (C behaviour) | Linux, Windows, macOS | pass |
| bounds R76 plane_test (D-2) | any box/plane | always `true` (the C `bool` folds 1/2/3); the Rust-native `bounds_plane_classify` offers OUTSIDE/PARTIAL/INSIDE | Linux, Windows, macOS | pass |
| bounds R77 under_plane | `dir` ± per axis | `vmin` per axis (`dir < 0` selects `max`); D-4: `w` pinned 0 | Linux, Windows, macOS | pass |
| bounds R78 inside/vec3_inside | corner point, `±eps` | inclusive with the `EPSILON` margin | Linux, Windows, macOS | pass |
| bounds R79 intersects | touching, gap, negative `eps` | per-axis gap ≤ `eps`; negative `eps` shrinks | Linux, Windows, macOS | pass |
| bounds R80 intersects_obb/obb3x4 | rotated OBB overlap | `matrix4_inv` / `matrix3_transpose` as the inverse; **singular `m` → `false`** (D-5) | Linux, Windows, macOS | pass |
| bounds R81 min_dist | box above/below plane | the spec formula `p.dist + plane_dist(center) − half` — reduces to `dot4(c, dir) − half` (pinned as specified; C conformance to confirm) | Linux, Windows, macOS | pass |
| math-extra R90/R91 polar↔cart | `(π/2, 0, 1)`, `(0, π/2, 1)` | worked values; round trip; **`d.w = 0`** (D-3: reference uninitialised); zero cart → `(0,0,0,0)` | Linux, Windows, macOS | pass |
| math-extra R92 norm↔polar | `(1,0,0)`, `(0,2,0)` | `atan2(n.x, n.z)`, `asin(n.y)` NaN for `|n.y| > 1`; `polar_to_norm` **does not write `d.w`** (sentinel) | Linux, Windows, macOS | pass |
| math-extra R93 calc_torquef | `t = 0`, clamped, `min_adjust`, negative `t` | spec pseudocode; `dist == 0` → second branch; `t = 0` → `val1`; negative `t` unclamped on that side | Linux, Windows, macOS | pass |
| math-extra R94 calc_torque | `v1 <= v2` componentwise; diagonal | **Q-8: signed `vec3_close` snaps to `v2` (all four lanes)**; `d` may alias `v2`; Q-9 (`d == v1` corruption) unreachable in Rust (caller aliasing violation) — documented deviation | Linux, Windows, macOS | pass |
| math-extra R95 percentage | `0/0`, `x/0` | `(mid−start)/(end−start)` in f32, **no zero check** (NaN/inf); `i` form subtracts in `i32` | Linux, Windows, macOS | pass |
| math-extra R96 rand_float | 1000 samples | `[0,1]` positive-only, `[-1,1]` signed; not all identical; deterministic seed (atomic counter ^ thread id — no wall clock, Miri isolation) | Linux, Windows, macOS | pass |
| half R110 layout | size | 2 bytes; `half_from_bits(u).u == u` | Linux, Windows, macOS | pass |
| half R111/R112 special values | spec vector list | `1 → 0x3C00`, `−1 → 0xBC00`, `0.5 → 0x3800`, `65504 → 0x7BFF`, `±0`, `±inf`, **every NaN → `0x7FFF`/`0xFFFF`** (payload not preserved), `2^-14 → 0x0400`, `2^-24 → 0x0001`, `0x1.ff8p-15` (bits `0x387FC000`) → `0x03FF` | Linux, Windows, macOS | pass |
| half R112/Q-10 overflow | representable values around 65504 | the boundary itself (`65504.0 = 0x477FE000`) stays `0x7BFF`; the **smallest f32 above it (`65504.00390625`, ulp `2^-8`) already → inf**; `65504.004_f32`, `65519.99_f32` (folds to `65519.984375`), `65520.0` → inf (IEEE would map `(65504, 65520)` to 65504 — reproduced); negative side `0xFC00` | Linux, Windows, macOS | pass |
| half R113 subnormal, no sticky | `2^-25` and variants | exact tie → `0` (even); `2^-25·(1+2^-12) → 1`; `·(1+2^-13) → 0`; `·(1+2^-23) → 0` (shift discards low bits before rounding); D-6 sub-f32 → signed zero | Linux, Windows, macOS | pass |
| srgb R120 gamma | `0`, `0.04045`, `0.5`, `1`, negative, NaN | both directions at the f32 `powf` tolerance; **`linear_to_nonlinear(1.0) = 0.99999994`** (pinned); below-0 inputs take the linear branch; NaN through `powf`; float3 forms leave `f[3]` untouched | Linux, Windows, macOS | pass |
| srgb R121 u8/float | spec examples | `0.5 → 128`, `0.002 → 1`, `0.0019 → 0`, `−0.004 → 0`, `1.001 → 255`; **no clamping in the defined range** (truncation toward zero), saturation outside (Q-14); truncation not rounding (`127.9`-class stays `127`) | Linux, Windows, macOS | pass |
| srgb R122 lane conversions | round trip | `u8x4 ↔ float4` exact at the worked values; `float3_to_u8x3` **leaves `u[3]` untouched** (sentinel) | Linux, Windows, macOS | pass |
| srgb R123 premultiply_float4 | `(1, 0.5, 0.25, 0.5)` | original alpha for all three multiplies; alpha unchanged | Linux, Windows, macOS | pass |
| srgb R124 xyza / restrict | spec examples | `ff804080 → 80402080`, `ffffff01 → 01010101`, `ff804000 → 00000000`; in-place keeps the alpha byte; restrict writes all four (alpha copied) | Linux, Windows, macOS | pass |
| srgb R125 sRGB forms + loops | `ff804080`, 2 texels | nonlinear→linear, premultiply, linear→nonlinear: `ff804080 → bc5d2d80` (**±1 per byte** allowed); `*_loop` forms: `n = 0` no-op, bytes beyond `4n` untouched | Linux, Windows, macOS | pass |

Notes:
- **Miri `f32::powi` is not exact** (2⁻²⁵ computes as `0x32FFFFFC`): the
  half tie-boundary vectors use exact bit patterns
  (`f32::from_bits`) instead of `powi`-derived values.
- **C self-aliasing** (e.g. `vec2_add(d, d, v)`) has no Rust `&mut`
  spelling; the ports take disjoint references and the tests use copies
  where the C form would alias.
- **`gs_float_to_u8` dedupe**: the single definition lives in
  `srgb.rs` (S-009-R121, the C `srgb.h` `static inline`); `vec4.rs`
  (S-008-R08 consumers) uses it — this also cleared the ambiguous glob
  re-export.
- **matrix3 transform vs matrix4 transform**: `vec3_transform3x4` is
  row-major (`v · rows`), `vec4_transform` is column-major
  (`v · columns`) — same numerics, transposed rotation; the bounds
  tests pin both conventions.
- **C-side remainder** (S2): conformance scripts
  `test/conformance/scripts/math/` + `math2/`, cbindgen header diff,
  `OBS_ENABLE_RUST` switch, P1-15.3 10k goldens, P1-16.4 benchmark —
  blocked on the C checkout.
- **Not in this WP** (S-008 §5, own substep P1-17.1/.2): the full quat
  set (`quat_log/exp/interpolate`, …) and `axisang_from_quat`.
