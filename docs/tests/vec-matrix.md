# Test matrix: vec2/vec3/vec4 + matrix3/matrix4 (obs-math)

- Test files: `crates/obs-math/src/{vec2,vec3,vec4,quat,axisang,matrix3,matrix4}.rs` (unit tests per module)
- Task: P1-15/P1-16 (spec S-008 R01–R08, R10/R14 (quat kernel), R30–R36, R37–R39, R41)
- Golden files: none (C conformance vectors arrive with the C checkout;
  expectations here are from the spec's worked values + structural
  invariants, compared at the spec tolerance: relative **or** absolute
  1e-6, `-0` == 0)

| Case | Input | Expected | Platforms | Status |
|---|---|---|---|---|
| layout (vec2) | size/offset | 8 bytes, align 4 | Linux, Windows, macOS | pass |
| set_zero_copy (vec2) | `(3,−4)` | round-trip | Linux, Windows, macOS | pass |
| dist_min_max (vec2) | `(3,4)`, `(0,0)`, `±f` | 5.0; component-wise | Linux, Windows, macOS | pass |
| norm (vec2) | `(3,4)`; zero | `(0.6,0.8)`; zero → zero (division-by-zero rule, R37) | Linux, Windows, macOS | pass |
| layout (vec3/vec4/quat/axisang) | size/offset | 16 bytes, align 16, C field order | Linux, Windows, macOS | pass |
| set_stores_w_zero | `vec3_set(1,2,3)` | `w = 0` (R01) | Linux, Windows, macOS | pass |
| w_lane_zeroed_by_most_ops | 14 ops on live-w input | `w = 0` for add/sub/div/±f/min(f)/max(f)/neg/abs/floor/ceil/cross (R02) | Linux, Windows, macOS | pass |
| w_lane_live_in_mul_mulf_dot | `w = 10, 10` | `mul.w = 100`, `mulf.w = 20`, `dot = 132`, `len((1,0,0,10)) = √101` (Q-1) | Linux, Windows, macOS | pass |
| dist_and_norm | live-w inputs | dist over xyz (sub zeroes w); norm scales all lanes (R03) | Linux, Windows, macOS | pass |
| close_is_signed | `(0,0,0)` vs `(1,1,1)` | true when `a−b` very negative; false reversed (Q-2) | Linux, Windows, macOS | pass |
| from_vec4_zeroes_w / from_vec3_sets_w_one | cross conversions | `w = 0` / `w = 1` (R05) | Linux, Windows, macOS | pass |
| transform_translation_and_no_divide | `t = (10,20,30,1)` row | `+t`; raw w dot, no perspective divide (R07) | Linux, Windows, macOS | pass |
| ops_touch_all_lanes (vec4) | 4-lane inputs | all-lane arithmetic, no special-casing (R04) | Linux, Windows, macOS | pass |
| close_is_signed_all_lanes (vec4) | w lane difference | w participates (R06) | Linux, Windows, macOS | pass |
| transform_is_row_times_matrix | `t = (10,20,30,1)` | `d = v · M` (R07) | Linux, Windows, macOS | pass |
| rgba_packing | `(1, 0.5, 0, 1)` | `0xff0080ff` (R08 worked value); bgra `0xffff8000`; round trips (Q-4 no clamping) | Linux, Windows, macOS | pass |
| srgb_linearization | R = 0, 255, 10, 128 | 0→0, 1→1, `u ≤ 0.04045` branch, `((u+0.055)/1.055)^2.4` branch (R08) | Linux, Windows, macOS | pass |
| identity_and_dot (quat) | `(1,2,3,4)` | identity `(0,0,0,1)`; dot 30 (R10) | Linux, Windows, macOS | pass |
| from_axisang_half_angle | 90° about +Z | `s = sin(π/4)`, `c = cos(π/4)`; non-normalised axis doubles the vector part (R14) | Linux, Windows, macOS | pass |
| axisang basics | set/zero/copy | `w` = radians (R44) | Linux, Windows, macOS | pass |
| layout (matrix3/matrix4) | size/align | 48/16 and 64/16, row order x,y,z(,t) | Linux, Windows, macOS | pass |
| identity_and_transpose | 1..16 matrix | transpose; `d == m` safe (R30) | Linux, Windows, macOS | pass |
| mul_and_aliasing | `T(1,2,3) · S(2)` | `t = (2,4,6,1)`; `S·S` alias-safe = `S(4)` (R31) | Linux, Windows, macOS | pass |
| mul_4x3_only_copies_t | garbage `m1.t` | `d.t = m2.t` unchanged; first 3 rows exact (R31) | Linux, Windows, macOS | pass |
| determinant | I, S(2), T, S·T | 1, 8, 1, 8 (R32) | Linux, Windows, macOS | pass |
| inverse | I, S(2), zero, S(0.0003), rot·trans round trip | 0.0005 threshold; `d` unmodified on failure (vector 04); `M·M⁻¹ ≈ I` (R33) | Linux, Windows, macOS | pass |
| from_quat_formula | 90° about +Z; zero quat | rows `(0,1,0,0)`, `(-1,0,0,0)`, `(0,0,1,0)`; zero quat → identity (R34) | Linux, Windows, macOS | pass |
| composition_helpers | T/S/rotate compositions | `dst = m ∘ op`; translate lands unscaled by m (R35) | Linux, Windows, macOS | pass |
| inverse_order_variants | v = (20,60,120) | vector 05: `ti = 20 60 120 1`; with `m = S(2)`: scaled (40,120,240,1); rotate_i/scale_i via `mul_4x3_only` (R36) | Linux, Windows, macOS | pass |
| inline_scalar_variants | `*_3f`/`rotate_aa4f` | scalar convenience forms (R41) | Linux, Windows, macOS | pass |
| matrix3 identity_transpose_copy / mul_and_aliasing / inverse / constructions / composition | same families | 3×3 corner; translation in row `w` lanes with implicit `(0,0,0,1)` row; `from_matrix4`/`from_matrix3` projection pair (R41) | Linux, Windows, macOS | pass |

Notes:
- **Q-5 pin**: `FRAC_1_SQRT_2` is an approximate unit quaternion, so
  `from_quat` rows carry ~1e-7 error; rotation-derived rows are compared
  at the spec tolerance (1e-6), never `assert_eq` — an exact compare is
  build-dependent (FMA contraction on real hardware differs from Miri;
  Miri caught this in this WP).
- **matrix3 `w`-lane convention**: the affine translation lives in the
  row `w` lanes with an implicit fourth row `(0,0,0,1)`; S-008 fixes the
  layout but not this reading — to be confirmed against the C source in
  the conformance substep.
- Exported functions **not** in this WP (their rules/inputs are not
  pinned by S-008 or belong to later WPs): `vec3_rotate`,
  `vec3_transform3x4`, `vec3_mirror(v)`, `vec3_plane_dist` (plane,
  P1-17), `vec3_rand` (S-009 R44 `rand_float`), `matrix3_mirror(v)`
  (needs the C formula). The full quat set (R11–R13, R15–R21), plane
  (R42), bounds (R43), math-extra (R44) → P1-17/P1-18.
- C-side remainder: conformance scripts `test/conformance/scripts/math/`
  (8) vs C, cbindgen header diff — blocked on the C checkout.
