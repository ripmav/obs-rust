# Test matrix: ref-dump — P0-14 C reference dumper

- Test file / issue: `test/ref-dump/` (`ref-dump.c`, `ref-dump-math.c`,
  `ref-dump-data.c`, `ref-dump-media.c`) / P0-14 (WP-39)
- Task: P0-14 (C reference dumper + goldens + CI regenerate-and-diff)
- Golden files: `test/golden/ref-dump/{math,data,media}.golden` (committed;
  regenerated and diffed by the `reference dump` step of the `conformance`
  CI job — the trailing `# section: rand` block of `math` is libc-`rand`
  platform-dependent and awk-stripped from both sides before the diff)

| Case                    | Input                                                                                                                                                                                                                                           | Expected                                                                                                                                                                                                                              | Platforms          | Status |
| ----------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------ | ------ |
| list                    | `obs-ref-dump --list`                                                                                                                                                                                                                           | prints `math`, `data`, `media`, `all`, exit 0                                                                                                                                                                                         | Linux (local, CI)  | pass   |
| determinism             | two consecutive `obs-ref-dump all` runs                                                                                                                                                                                                         | `diff` of the two outputs is empty (PRNG seed `0x0B5` splitmix64, `%.9g` floats, no libm in the wave generation)                                                                                                                      | Linux (local, CI)  | pass   |
| math dump               | all 201 public C graphics-math functions (vec2/3/4, matrix3/4, quat, plane, bounds, axisang, math-extra) over the seeded PRNG + special-value sweeps                                                                                            | byte-identical to `test/golden/ref-dump/math.golden` above the `# section: rand` marker (IEEE-754 f32 arithmetic is arch-identical; x86_64 CI verifies cross-arch)                                                                    | Linux (local + CI) | pass   |
| data dump               | obs_data build/get/defaults/apply/erase/clear over all types + nested objects/arrays; sorted-JSON round trip through jansson 2.15.1; fixtures basic/scene/unicode/empty; 32-case double shortest-round-trip sweep; calldata per-type + raw data | byte-identical to `test/golden/ref-dump/data.golden`; pins: primitive JSON array elements dropped by obs_data, jansson prints integral doubles as `24.0`, `calldata_get_data` requires the exact stored size (S-014/S-002)            | Linux (local + CI) | pass   |
| media dump — pixels     | 64×36 UYVX gradient → all six `format-conversion.c` YUV functions + round trips (NV12↔UYVX, I420↔UYVX, YUY2/UYVY) + partial-range pass                                                                                                          | FNV-1a-64 hashes byte-identical to `test/golden/ref-dump/media.golden` (S-013)                                                                                                                                                        | Linux (local + CI) | pass   |
| media dump — mix_audio  | verbatim `mix_audio` (obs-audio.c:90) over ts offsets {0, 25 µs, 50 µs, 10 666 667 ns, 21 333 333 ns, 21 333 334 ns, outside window} × channels 1..3 over deterministic fixed-point triangle waves                                              | per-case FNV-1a-64 hashes of the 1024-sample mixes byte-identical to `test/golden/ref-dump/media.golden`; covers full mix, start_point 1/2/512/1023, the 1024-frame early return, and the out-of-window no-mix (S-013)                | Linux (local + CI) | pass   |
| latent defect avoidance | `mix_audio` start_point > AUDIO_OUTPUT_FRAMES                                                                                                                                                                                                   | NOT exercised — the C has an unguarded `size_t` underflow (`total_floats -= start_point` wraps → OOB read/write); unreachable in production (audio_ts within one frame of the window start); pinned in the ODD L89 and the S-013 spec | n/a (documented)   | pass   |
| CI regenerate + diff    | conformance job: build `obs-ref-dump` against the pinned 33.0.1 checkout + the job's jansson prefix, run all three dumps, strip `# section: rand`, diff against the committed goldens                                                           | red on any drift (P0-14.5 Done-when)                                                                                                                                                                                                  | Linux (CI, x86_64) | pass   |

Notes:

- The math `# section: rand` block (libc `rand()`-based helpers) is
  platform-dependent by nature (the C reference calls libc `rand()`); it is
  kept in the dumper output and in the committed golden for completeness,
  but excluded from the CI diff. Everything above the marker is
  byte-deterministic.
- The audio waves are fixed-point triangle waves (pure arithmetic, no libm):
  `sinf`/`cosf` are not ULP-stable across the 3-OS CI matrix, which would
  make the mix goldens non-reproducible. The mix arithmetic itself is the
  C code verbatim.
- Build: `bash test/ref-dump/build.sh` (env: `LIBOBS`, `JANSSON`, `OUT` —
  CI passes the job's pinned checkout and jansson prefix). The CMake-target
  half of P0-14.1 is a documented deviation (ODD L89): the standalone
  binary is the single build path.
