# P0-17 Benchmarks (WP-41)

Benchmark harness + C baseline for the Rust rewrite (subtask P0-17,
`docs/rust-rewrite/subtasks/phase-0-subtasks.md`). Four tracked workloads
(P0-17.2/.3): matrix multiply, pixel format conversion, 8-track audio mix,
20-item scene render.

- **C baseline runner:** `test/ref-bench/ref-bench.c` (+ `build.sh`) —
  measures the pinned 33.0.1 libobs sources, prints canonical bench JSON.
- **Rust benches:** `crates/obs-math/benches/p017.rs` — criterion report
  (`cargo bench -p obs-math --bench p017`) + the same canonical JSON in
  `--gate` mode. The WP-35 SIMD benches (`benches/simd.rs`) are unchanged.
- **Committed baseline:** `test/ref-bench/baseline.json` (machine-readable)
  - the table below (human-readable). Keep both in sync when regenerating.
- **Regression gate (P0-17.5):** `test/ref-bench/compare.py` — fails when a
  tracked bench is >2% slower than the baseline. Wired into the `benchmarks`
  CI job; **blocking since WP-46 (P1-32.1)** (`--no-fail` dropped — a tracked
  bench >2% slower than the baseline on the same CPU model, or a missing
  bench, fails the job).
- **Contract + usage:** `test/ref-bench/README.md` (JSON schema 1, gate
  semantics, baseline regeneration).

## Bench catalogue

`ns/op` is per work unit: per `matrix4_mul` (matmul4), per 1080p frame
(nv12_to_rgb), per 8-track × 1024-frame mix pass (audio_mix), per 20-item
scene frame (scene_render).

| bench          | workload                                                               |    C ns/op (CI x86_64)     | Rust ns/op (CI x86_64) | ratio Rust/C |
| -------------- | ---------------------------------------------------------------------- | :------------------------: | :--------------------: | :----------: |
| `matmul4`      | 1M dependent `matrix4_mul` (bounded affine chain)                      |             8              |          9.87          |     1.23     |
| `nv12_to_rgb`  | 1080p NV12 → packed 444 (`decompress_nv12`)                            |          1.0984e5          |    — (Phase 2 port)    |      —       |
| `audio_mix`    | 8 tracks × 1024 frames → 6 mixes × 2 ch (verbatim `mix_audio`)         |          4.136e3           |    — (Phase 2 port)    |      —       |
| `scene_render` | 20-item scene, per-item transform math (Rust stand-in, see deviations) | — (needs graphics runtime) |        1.312e3         |      —       |

**Authoritative numbers: the committed `test/ref-bench/baseline.json`**
(the table is regenerated from it). The committed baseline is the
**CI x86_64 runner** (GitHub Actions `benchmarks` job, run 45958004554,
AMD EPYC 9V45); the host-aarch64 verification numbers are below.

Honest note: on the x86_64 runner Rust `matmul4` is 1.23× the C
baseline (the host aarch64 run had it at 0.67×) — the P0-17.1
"not slower than C" bar is per-machine and gets re-verified on each
runner as the port matures (P1-19.3). Re-verified on the current runner
(EPYC 7763, run 46130478940, WP-46): **1.15× — improved, not a new
regression**; the finding stays open (P1-16.4/P1-19.3,
`PHASE-1-REVIEW.md`).

### CI cross-check numbers (x86_64, current runner AMD EPYC 7763, WP-46)

From the `benchmarks` job of run
[46130478940](https://github.com/ripmav/obs-rust/actions/runs/46130478940/job/140934278372)
(30-run medians, both sides on the same runner):

| bench          |          C ns/op           | Rust ns/op (CI x86_64) | ratio Rust/C |
| -------------- | :------------------------: | :--------------------: | :----------: |
| `matmul4`      |             13             |         14.95          |     1.15     |
| `nv12_to_rgb`  |          3.1605e5          |    — (Phase 2 port)    |      —       |
| `audio_mix`    |          1.1151e4          |    — (Phase 2 port)    |      —       |
| `scene_render` | — (needs graphics runtime) |        2.134e3         |      —       |

The 7763 is ~1.5–3× the baseline's 9V45 on the C workloads, so these C
numbers are not comparable to the committed baseline — they exist to give
the same-machine Rust-vs-C pair on the current runner (the matmul4 1.15×
row above). The `compare.py` rows of that run are all `info (different CPU)` (baseline CPU ≠ runner CPU) — the 2% threshold does not apply
cross-model (ODD L91).

### Local verification numbers (host aarch64, governor `performance`, 30-run medians)

| bench          | C ns/op | Rust ns/op | ratio Rust/C |
| -------------- | :-----: | :--------: | :----------: |
| `matmul4`      |  22.0   |   14.83    |     0.67     |
| `nv12_to_rgb`  | 2.122e6 |     —      |      —       |
| `audio_mix`    | 2.676e4 |     —      |      —       |
| `scene_render` |    —    |  2.259e3   |      —       |

Stability (3 consecutive runner invocations, same day; re-run for WP-46,
2026-11-28): matmul4 22/22/22 (±0.0%), nv12_to_rgb 2.09–2.19e6 (±1.1%),
audio_mix 26.76–27.28e3 (±1.0%), Rust matmul4 14.82–14.84 (±0.05%),
scene_render 2259/2259/2259 (±0.0%) — all within the ±3% P0-17.2
Done-when bar.

## Machine spec

### CI (source of the committed baseline)

GitHub Actions `ubuntu-latest`, **x86_64**, job `benchmarks` in
`.github/workflows/rust.yml`. Committed baseline: **AMD EPYC 9V45
96-Core** (run 45958004554; the exact CPU string is recorded in the job
log and in the JSON artifact `p017-bench-json`). C side compiled `-O3`
with the pinned simde 24fa5420 (GCC's libobs build path); Rust side in
the cargo bench profile (opt-level 3). Both are x86-64-baseline ISA (no
`-march=native` / no `target-feature=native`), so the two sides are
ISA-comparable.

**Runner heterogeneity:** `ubuntu-latest` rotates between CPU models —
the first two `benchmarks` jobs ran on **AMD EPYC 9V45** and **AMD EPYC
7763** respectively, 1.5–3× apart (the committed baseline is the 9V45
numbers). The gate therefore only enforces the 2% threshold on the
*same* CPU model; cross-model deltas are reported as informational
(ODD L91, `compare.py` CPU awareness). WP-46 flipped the gate to blocking
on that basis: as of 2026-11-28 the `benchmarks` job lands on **EPYC
7763**, so all committed-baseline rows are `info (different CPU)` and the
blocking mode enforces missing-bench detection plus same-CPU regressions
(which would activate if a runner ever lands on the baseline's 9V45 — note
the C `matmul4` baseline is 8 ns, where a 1-ns step is 12.5%). Pinning the
runner image / re-baselining per CPU model is the open follow-up, recorded
as a finding in `PHASE-1-REVIEW.md`.

### Host (local verification only)

- **CPU:** Raspberry Pi 5 Model B Rev 1.0 — 4× ARM Cortex-A76, up to 2.4 GHz
  (governor `ondemand` by default; measurements above taken with
  `performance` — DVFS noise otherwise exceeds the ±3% bar).
- **Arch / OS:** aarch64, Linux 6.18.50+rpt-rpi-2712, 8 GB RAM.
- C side: system simde (the aarch64 build path of `util/sse-intrin.h`);
  Rust side: NEON via the WP-35 `obs_math::simd` dispatch is *not* used by
  these workloads (they exercise the scalar ported kernels).

## Deviations (P0-17.2/.3 text → delivered)

1. **No C `scene_render` bench** — the C per-item render path needs the
   full graphics runtime (texrender/shaders/GL), which the standalone
   runner deliberately avoids (same scope cut as the WP-40 ABI harness,
   which also runs headless without a graphics module). The Rust bench is
   the **smallest faithful stand-in over already-ported code**: the exact
   per-item CPU math of `obs-scene.c` (33.0.1) — `set_item_transform`
   (lines 657–661) + per-frame `gs_matrix_mul(&item->draw_transform)`
   (line 964) — for 20 items, all in `obs-math`.
1. **`nv12_to_rgb` = the CPU step of the pipeline** — in 33.0.1 the
   YUV→RGB colour matrix is a shader; the CPU conversion of an NV12 frame
   is `decompress_nv12` (NV12 → packed 444, 4 B/px). The baseline measures
   that real function over a full 1080p frame.
1. **`audio_mix` uses the real mix path** — `mix_audio`
   (`libobs/obs-audio.c:90–124`, 33.0.1) copied **verbatim** with minimal
   stand-in types (the ref-dump precedent; the function is `static inline`
   in the pinned file). "8 tracks" = 8 input sources; the mixes are the
   real `MAX_AUDIO_MIXES` (6) × `MAX_AUDIO_CHANNELS` (8, stereo used) ×
   `AUDIO_OUTPUT_FRAMES` (1024).
1. **Rust benches for `nv12_to_rgb` / `audio_mix` are not in this WP** —
   `obs-media-io` is a Phase 0 skeleton; the ports (and their criterion
   benches in `crates/obs-media-io/benches/`) land in Phase 2, where this
   baseline is the "not slower than C" bar. The P0-17.1 file list
   anticipated that end state.
1. **No CMake/ctest** — standalone `build.sh` (WP-39/40 precedent; a CMake
   target needs a full OBS configure that CI does not verify).
1. **Gate in report mode for the first baseline commit** — the user rule
   for WP-41; **WP-46 flipped it to blocking** (drop `--no-fail` in the
   `benchmarks` job, P1-32.1): on the current runner (EPYC 7763) all rows
   are `info (different CPU)` (the baseline is 9V45), so the mode enforces
   missing-bench detection and same-CPU regressions (ODD L91).

## Known reference findings (for the Phase 2 spec, S-013)

- `decompress_nv12` (33.0.1) outputs packed pixels as **[Y, U, V, 0]**
  (byte-verified against the pinned code), while
  `compress_uyvx_to_nv12` reads Y from **byte 1** — the pair is **not an
  inverse** in the pinned tree. The Phase 2 port must mirror the actual
  per-function byte layout (pinned by the ref-bench self-check).
