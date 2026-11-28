# Test matrix: math golden ref-dump (P1-15.3)

- Test file: `crates/obs-math/tests/golden_refdump.rs` (WP-43)
- Task: P1-15.3 Goldens — vec3/vec4 compared against the C ref-dump at the
  S-008 §2 tolerance (1e-6 relative **or** absolute)
- Golden file: `test/golden/ref-dump/math.golden` (P0-14; pinned 33.0.1 C
  sources, 32 seeded splitmix64 cases per function, seed `0x0B5`, `%.9g`
  floats — round-trips f32 exactly)

| Case                             | Input                                                                                                                                                                                                                      | Expected                                                                                                                                    | Platforms          | Status |
| -------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------- | ------------------ | ------ |
| vec3_vec4_match_c_refdump_golden | every `math/vec3_*` / `math/vec4_*` group of the golden (all P1-15.2 exported vec3/vec4 functions: copy/neg/abs/…/transform/transform3x4/rotate/mirror/rand-free set) replayed through the Rust port, 32 seeded cases each | every float within 1e-6 relative or absolute of the C value; the int results of `vec3_close`/`vec4_close` exact; ~1952 comparisons in total | Linux (local + CI) | pass   |
| rand section exclusion           | the trailing `# section: rand` groups (`vec3_rand` & co. — libc `rand()` in C, own PRNG in the port)                                                                                                                       | excluded from the comparison (inputs differ by design; C-side coverage only, CI golden-diff strips the section)                             | n/a (documented)   | pass   |

Notes:

- The test always runs: the golden is committed to the repo.
- S-008 Q-6 (`vec3_rand` uses libc `rand()`): not comparable across
  implementations — the spec's tolerance applies to the deterministic
  functions only, which is exactly what this matrix pins.
- The matrix4/quat/axisang/plane/bounds/matrix3/half/srgb portions of the
  same golden are covered by the pre-existing golden tests in
  `crates/obs-math` (see `docs/tests/math-rest.md`).
