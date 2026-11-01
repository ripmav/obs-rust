# Phase 0: Foundation

Goal: decisions recorded, tooling in place, and a safety net (golden and ABI tests) that proves later phases did not change behaviour. No product code is replaced in this phase.

Start the PR stack with the `docs:` tasks (P0-01 to P0-06).

## A. Decisions (ADRs, `docs/adr/`)

- [ ] **P0-01 ADR: migration strategy** (S)
  - Do: record the strangler-fig decision, the "every phase ships" rule, and the exit criteria per phase.
  - Deliverables: `docs/adr/0001-strangler-migration.md`.
  - Done when: reviewed by two maintainers; alternatives (big-bang, new-repo) are listed with reasons for rejection.
- [ ] **P0-02 ADR: plugin ABI policy** (S)
  - Source: `libobs/obs-module.h`, `obs-source.h`, `obs-output.h`, `obs-encoder.h`, `obs-service.h`.
  - Do: define that the C ABI is frozen until Phase 7, how versions are bumped (`LIBOBS_API_VER`), and how a native Rust plugin API coexists.
  - Deliverables: `docs/adr/0002-plugin-abi.md`.
  - Done when: lists every header that is part of the frozen ABI.
- [ ] **P0-03 ADR: graphics backend** (M)
  - Do: prototype a textured quad plus a custom effect in `wgpu` on Linux, Windows and macOS; measure frame time; test DMA-BUF, DXGI shared handle and IOSurface import.
  - Deliverables: `docs/adr/0003-graphics.md` with measurements and a go/no-go.
  - Done when: decision covers fallback (keep GL/D3D11/Metal) if import is not possible.
- [ ] **P0-04 ADR: GUI toolkit** (M)
  - Do: build a throwaway dock with a scene list in `cxx-qt`, Slint and egui; compare effort, accessibility, theme support (`frontend/data/themes`), HiDPI, and Flatpak packaging.
  - Deliverables: `docs/adr/0004-gui-toolkit.md`.
  - Done when: one toolkit is chosen with a migration path for `.ui` forms.
- [ ] **P0-05 ADR: unsafe, MSRV, dependency policy** (S)
  - Do: define `unsafe` rules (FFI and platform boundaries only, `// SAFETY:` required), MSRV (latest stable), licence allow-list (GPLv2+ compatible), and "stdlib first" for deps.
  - Deliverables: `docs/adr/0005-code-policy.md`.
- [ ] **P0-06 ADR: concurrency model** (S)
  - Source: `libobs/obs-video.c`, `obs-audio.c`, `media-io/video-io.c`, `audio-io.c`.
  - Do: document the current thread layout (graphics thread, audio thread, encoder threads, output threads) and the target one (real-time threads, lock-free queues, tokio only for network).
  - Done when: lists every lock in `obs-internal.h` and its replacement.

## B. Workspace and CI

- [ ] **P0-07 Cargo workspace skeleton** (S)
  - Do: root `Cargo.toml` with `[workspace]`, `rust-toolchain.toml`, `rustfmt.toml`, `clippy.toml`, empty crates `obs-util`, `obs-math`, `obs-data`, `obs-signal`, `obs-media-io`, `obs-graphics`, `obs-core`, `obs-ffi`, `obs-cli`.
  - Done when: `cargo build --workspace` passes on all three OSes.
- [ ] **P0-08 Corrosion integration in CMake** (M)
  - Source: `CMakeLists.txt`, `cmake/`, `libobs/CMakeLists.txt`.
  - Do: add Corrosion, expose a CMake option `OBS_ENABLE_RUST` (default OFF) that links the Rust static libs into `libobs`.
  - Done when: both `ON` and `OFF` build and pass `ctest` on CI.
- [ ] **P0-09 CI pipeline for Rust** (M)
  - Source: `.github/workflows/`, `CMakePresets.json`.
  - Do: jobs for fmt, clippy `-D warnings`, `cargo deny`, `cargo audit`, `cargo test`, Miri on pure crates, coverage; matrix Linux, macOS, Windows.
  - Done when: required checks block merging.
- [ ] **P0-10 `cargo-deny` and licence policy** (S)
  - Deliverables: `deny.toml` with licence allow-list and banned crates.
- [ ] **P0-11 `.gitattributes`** (S)
  - Do: add `*.text -text` and `*.golden -text` so Windows tests do not break on line endings; review other text fixtures.
  - Done when: Windows CI passes golden tests unchanged.
- [ ] **P0-12 Dev container / build docs** (S)
  - Do: document `cargo`, `cmake`, FFmpeg and Qt prerequisites in `INSTALL` and `docs/rust-rewrite/BUILDING.md`.

## C. Safety net

- [ ] **P0-13 Golden-test framework** (M)
  - Do: a `obs-golden` crate: reads/writes `*.golden`, `UPDATE_GOLDEN=1` regenerates, shows readable diffs.
  - Done when: used by one demo test.
- [ ] **P0-14 C reference dumper** (L)
  - Source: `libobs/util`, `libobs/graphics/*math*`, `libobs/obs-data.c`, `libobs/callback`, `libobs/media-io`.
  - Do: a C test executable `obs-ref-dump` that runs fixed inputs through the C implementation and prints canonical output (math, `obs_data` JSON round trips, calldata, audio mixing, pixel conversion).
  - Deliverables: `test/golden/*.golden` produced from the C code.
  - Done when: re-running produces byte-identical files (deterministic).
- [ ] **P0-15 ABI conformance test plugin** (M)
  - Source: `test/` (existing test dir), `libobs/obs-module.h`.
  - Do: a small C plugin registering a source, filter, encoder, output and service; loaded by a test host, checks lifecycle callbacks (create, update, video_tick, video_render, destroy).
  - Done when: passes against the current `libobs` and is wired into CI.
- [ ] **P0-16 Headless smoke test** (M)
  - Do: script that starts OBS with `--headless`-equivalent (via test host), builds a scene with a colour source and a text source, records 5 s with `obs-ffmpeg-mux`, and checks the output with `ffprobe`.
  - Done when: runs in CI without a display (Xvfb or no-GUI host).
- [ ] **P0-17 Benchmark harness** (M)
  - Do: `criterion` benches plus a C baseline for: matrix multiply, pixel format conversion, audio mix of 8 tracks, scene render of 20 items.
  - Deliverables: baseline numbers stored in `docs/rust-rewrite/benchmarks.md`.
  - Done when: CI fails a PR on >2% regression of a tracked bench.
- [ ] **P0-18 Real-world scene-collection corpus** (M)
  - Do: collect (anonymised, with consent) 20+ scene-collection and profile JSON files that cover many source types.
  - Done when: stored under `test/fixtures/` and loadable by the current frontend.

## D. Tracking

- [ ] **P0-19 Issue templates and labels** (S): `rust-rewrite`, `phase-N`, `abi`, `perf`.
- [ ] **P0-20 Test matrices** (S): create `docs/tests/` and the first matrix files for the golden framework and the ABI test.
- [ ] **P0-21 Phase 0 review** (S): demo the safety net, confirm exit criteria, tick this file.

Exit criteria: ADRs 1-6 merged, CI green on 3 OSes with Rust jobs, golden files generated from C, ABI plugin test passing, benchmark baseline stored.
