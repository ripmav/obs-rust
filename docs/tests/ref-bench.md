# Test matrix: ref-bench — P0-17 C baseline runner + regression gate

- Test files: `test/ref-bench/ref-bench.c` (+ `build.sh`),
  `test/ref-bench/compare.py`, `crates/obs-math/benches/p017.rs`
  (Criterion bench + `--gate` JSON mode — build-checked by `cargo bench`,
  the `--gate` path is exercised by the matrix rows below)
- Task: P0-17 (WP-41); C reference: pinned obs-studio 33.0.1
  (`libobs/graphics/*.c`, `libobs/media-io/format-conversion.c`,
  `libobs/obs-audio.c:90-124` verbatim)

| Case                 | Input                                                                                                                     | Expected                                                                                                                                                                         | Platforms                   | Status |
| -------------------- | ------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------- | ------ |
| list                 | `ref-bench --list`                                                                                                        | prints `matmul4`, `nv12_to_rgb`, `audio_mix`, exit 0                                                                                                                             | Linux (local, CI)           | pass   |
| JSON schema (C)      | `ref-bench --runs 10`                                                                                                     | stdout parses as JSON, `schema=1`, `side="c"`, all benches carry `iters`/`runs`/`ns_per_op` (number), `ns_per_op > 0`                                                            | Linux (local, CI)           | pass   |
| JSON schema (Rust)   | `cargo bench -p obs-math --bench p017 -- --gate`                                                                          | same shape, `side="rust"`, benches `matmul4`/`scene_render`; both files merge in `compare.py`                                                                                    | Linux (local, CI)           | pass   |
| nv12 self-check      | first pixel of `decompress_nv12` over the 1080p gradient                                                                  | byte 0 = Y, byte 1 = U, byte 2 = V, byte 3 = 0 (the pinned function's actual layout — NOT the inverse of `compress_uyvx_to_nv12`; ODD L91); wrong layout → exit 2                | Linux (local, CI)           | pass   |
| audio self-check     | `mix_audio` over 8 sources × 6 mixes × 2 ch × 1024 (deterministic ramp)                                                   | every output sample bit-equal to an identical-add-order recompute (f32, same order) — a corrupted mix loop → exit 2                                                              | Linux (local, CI)           | pass   |
| matmul4 workload     | 1M **dependent** `matrix4_mul`, bounded affine multiplier (rotation 1.0 rad about (0.3,0.5,0.7) + small translation)      | values stay bounded (no f32 overflow in the chain); 3-invocation stability ±0.0% (local, governor `performance`)                                                                 | Linux (local, CI)           | pass   |
| stability ±3%        | 3 consecutive runner invocations (C: `--runs 30`; Rust: `--gate`)                                                         | every bench within ±3% between invocations (P0-17.2 Done-when): measured C matmul4 22/22/22, nv12 2.16–2.21e6 (±1.1%), audio 26.3–26.6e3 (±0.7%); Rust 14.82–14.85 / 2.26–2.30e3 | Linux (local, CI)           | pass   |
| cross-arch build (C) | `build.sh` with `CC=x86_64-linux-gnu-gcc` + `CONFORMANCE_EXTRA_CFLAGS` (pinned simde), run under `qemu-x86_64`            | builds + runs, same JSON schema (the CI x86_64 job is the native confirmation)                                                                                                   | Linux (aarch64 host + qemu) | pass   |
| gate — ok            | `compare.py baseline.json --current <clean c.json> --current <clean rust.json>`                                           | table printed, `ok: no tracked bench regressed`, exit 0                                                                                                                          | Linux (local, CI)           | pass   |
| gate — regression    | current JSON with `matmul4/c` ×1.05 (deliberate +5% slowdown — the P0-17.5 Done-when demo)                                | `matmul4/c: +5.00% > +2% REGRESSION`, exit 1                                                                                                                                     | Linux (local, CI)           | pass   |
| gate — missing bench | current JSON without `audio_mix`                                                                                          | `audio_mix/c: missing from current output`, exit 1 (a deleted bench must not silently pass)                                                                                      | Linux (local, CI)           | pass   |
| gate — self-test     | `compare.py --self-test`                                                                                                  | `self-test: ok (5/5)` (ok / faster / regression / missing / schema-check synthetic cases), exit 0                                                                                | Linux (local)               | pass   |
| CI job (report mode) | `benchmarks` job in `rust.yml`: OBS checkout + simde + `build.sh` + run + `cargo bench … --gate` + `compare.py --no-fail` | both JSONs uploaded as artifact `p017-bench-json`; gate printed in report mode (WP-46 flips to blocking); job green                                                              | Linux (CI, x86_64)          | pass   |

Notes:

- `ns_per_op` semantics: per `matrix4_mul` (matmul4), per 1080p frame
  (nv12_to_rgb), per 8-track × 1024-frame mix pass (audio_mix), per 20-item
  scene frame (scene_render) — the `iters` field in the JSON states the
  work units per measured run.
- Local aarch64 numbers need the CPU governor set to `performance`
  (`ondemand` DVFS noise exceeds the ±3% bar); the committed baseline
  represents the CI x86_64 runner (see `docs/rust-rewrite/benchmarks.md`).
- No C `scene_render` and no Rust `nv12_to_rgb`/`audio_mix` in this WP —
  documented deviations (graphics runtime wall; Phase 2 `obs-media-io`
  ports), see `test/ref-bench/README.md` + `docs/rust-rewrite/benchmarks.md`.
