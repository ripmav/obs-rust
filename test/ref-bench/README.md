# ref-bench — P0-17 C baseline runner (WP-41)

Standalone C runner that measures the pinned **33.0.1 libobs** reference
workloads and prints the **canonical bench JSON** (schema 1) on stdout.
It is the C half of the P0-17 benchmark harness; the Rust half is
`crates/obs-math/benches/p017.rs` (`--gate` mode emits the same JSON).

Design precedent (WP-39/40): **standalone build, no in-tree CMake** — a
CMake target would need a full OBS configure (GL/glib/ffmpeg) that CI does
not verify. `build.sh` is the single build path, locally and in CI.

## Build / run

```sh
LIBOBS=/path/to/obs-studio/libobs \
OUT=test/ref-bench/ref-bench \
bash test/ref-bench/build.sh          # env: LIBOBS, OUT, CC, CONFORMANCE_EXTRA_CFLAGS
./test/ref-bench/ref-bench            # all benches, 30 runs each
./test/ref-bench/ref-bench --list
./test/ref-bench/ref-bench --runs 60 --bench matmul4
```

Env (same contract as `test/ref-dump/build.sh` and `test/abi-host/build.sh`):
`LIBOBS` (default: local `obs-studio-3301`), `OUT`, `CC` (cross builds),
`CONFORMANCE_EXTRA_CFLAGS` (CI passes the pinned simde include; the aarch64
local build uses the system simde). Builds with `-O3` (this is a
performance baseline — unlike ref-dump's `-O0` correctness dumps).

## Workloads

| bench         | workload (fixed, deterministic, no libm)                                                                                                                                                                                                                             | C source under test                     |
| ------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------- |
| `matmul4`     | 1,000,000 **dependent** `matrix4_mul` — bounded affine matrix (rotation 1.0 rad about (0.3,0.5,0.7) + small translation) so the chain never overflows f32                                                                                                            | `graphics/matrix4.c` (pinned)           |
| `nv12_to_rgb` | one 1920×1080 frame through `decompress_nv12` — NV12 → the packed 444 output (4 B/px, `out_linesize` = 4W), the CPU step of the NV12→RGB pipeline in 33.0.1 (the YUV→RGB colour matrix is a shader there)                                                            | `media-io/format-conversion.c` (pinned) |
| `audio_mix`   | **the real mix path**: `mix_audio` (obs-audio.c:90–124) copied verbatim with minimal stand-in types (the ref-dump precedent); 8 input sources ("8 tracks") × 1024 frames × stereo into all `MAX_AUDIO_MIXES` (6) mixes, mix buffers zeroed per pass as in production | `obs-audio.c` function, verbatim        |

No `scene_render` C bench: the C per-item render path
(`obs-scene.c` → `obs_source_video_render`) needs the full graphics
runtime (texrender/shaders), which this standalone build does not have
(documented deviation, P0-17.3). The Rust stand-in in
`crates/obs-math/benches/p017.rs` mirrors the per-item CPU math of
`obs-scene.c` (33.0.1): `set_item_transform` lines 657–661
(identity → `scale3f` → `translate3f(-origin)` → `rotate_aa4f(0,0,1,RAD(rot))`
→ `translate3f(pos)`) + the per-frame `gs_matrix_mul(&item->draw_transform)`
(line 964).

Each bench: 2 untimed warmup runs, then `--runs` (default 30) timed runs
with `CLOCK_MONOTONIC`; `ns_per_op` = **median** run time / work units
(`matmul4`: per multiply, others: per frame/pass). An untimed self-check
(validates the `decompress_nv12` pixel layout and the `mix_audio` result
against an identical-order recompute) runs before any measurement.

## Canonical bench JSON (schema 1)

Both runners (C `ref-bench`, Rust `p017 --gate`) emit the same shape —
this is the input of the regression gate:

```json
{
  "schema": 1,
  "side": "c",                  // "c" | "rust"
  "os": "linux",
  "arch": "x86_64",
  "cpu": "<model name / Model / Hardware line of /proc/cpuinfo, else \"unknown\">",
  "benches": {
    "matmul4": {"iters": 1000000, "runs": 30, "ns_per_op": 2.2},
    "nv12_to_rgb": {"iters": 1, "runs": 30, "ns_per_op": 1234.5},
    "audio_mix": {"iters": 1, "runs": 30, "ns_per_op": 98.7},
    "scene_render": {"iters": 1, "runs": 30, "ns_per_op": 12.3}
  }
}
```

- `iters` = work units of one measured run (so `ns_per_op` is comparable
  across benches: per multiply for `matmul4`, per 1080p frame for
  `nv12_to_rgb`, per 8-track mix pass for `audio_mix`, per 20-item frame
  for `scene_render`).
- `ns_per_op` is a number in every case (no units field needed).
- Rust numbers use `{:.9e}` (9 significant digits — Rust has no `%g`);
  C uses `%.9g`. Both are valid JSON and carry the same precision.

## Regression gate (P0-17.5)

`compare.py` compares current JSON against the committed baseline
(`baseline.json` — `{"schema": 1, "benches": {<name>: {"rust": x, "c": y}}}`)
and **fails when a tracked bench is more than 2% slower**; improvements
never fail, a tracked bench missing from the current output fails.

```sh
python3 test/ref-bench/compare.py --self-test                 # built-in synthetic checks
python3 test/ref-bench/compare.py baseline.json \
  --current ref-bench.json --current rust-gate.json            # fails > +2%
python3 test/ref-bench/compare.py baseline.json \
  --current ref-bench.json --no-fail                           # report mode
```

**CPU awareness** (ODD L91, found in the first CI push): GitHub's
`ubuntu-latest` runners are **heterogeneous** — two consecutive runs
landed on AMD EPYC 9V45 and EPYC 7763, 1.5–3× apart. A percentage
threshold across different CPU models is hardware noise, not a code
regression, so the gate only enforces the threshold when the current
JSON's `cpu` matches the baseline's `generated.cpu`; cross-model deltas
are printed as `info (different CPU)` and never fail. A **missing**
tracked bench still fails on any CPU.

CI wiring: the `benchmarks` job in `.github/workflows/rust.yml` builds +
runs both sides (x86_64 runner) and runs `compare.py` in **report mode**
for this first baseline commit — WP-46 (P1-32.1) flips it to blocking
by dropping `--no-fail` (and should then also pin the runner image /
re-baseline per CPU model).
Deliberate-slowdown demonstration (the P0-17.5 Done-when): see
`docs/tests/ref-bench.md` row T-06.

## Updating the baseline

```sh
# on the machine the baseline should represent (CI: the benchmarks job's
# runner; locally: with the CPU governor set to performance):
bash test/ref-bench/build.sh && ./test/ref-bench/ref-bench --runs 30 > /tmp/c.json
cargo bench -p obs-math --bench p017 -- --gate > /tmp/r.json
# merge /tmp/c.json + /tmp/r.json into baseline.json (keep schema 1),
# update docs/rust-rewrite/benchmarks.md, commit both.
```
