# Phase 0 subtasks

Parent file: [../phase-0-foundation.md](../phase-0-foundation.md). Subtask IDs are `<task>.<n>`. Each subtask is at most one day of work and has **Files** (what is touched), **Steps** (how), and **Done when** (verifiable result).

Facts verified in this checkout that shape Phase 0:
- `test/` already contains a cmocka suite (`test/cmocka/test_bitstream.c`, `test_darray.c`, `test_os_path.c`, `test_serializer.c`) and a test plugin (`test/test-input/`: `test-input.c`, `test-filter.c`, `test-random.c`, `test-sinewave.c`, `sync-*.c`). Phase 0 extends these instead of starting from zero.
- `plugins/obs-websocket`, `plugins/obs-browser` and `deps/libdshowcapture/src` are git submodules (see `.gitmodules`) and are empty unless initialised: run `git submodule update --init` before any task that reads them.

## A. Decisions (ADRs)

### P0-01 ADR: migration strategy
- [ ] **P0-01.1 ADR template**
  - Files: `docs/adr/0000-template.md`, `docs/adr/README.md`
  - Steps: create sections Status / Context / Decision / Alternatives / Consequences / Links; add numbering rule (4 digits) and "supersedes" field.
  - Done when: template renders on GitHub and README lists how to add an ADR.
- [ ] **P0-01.2 Context section**
  - Files: `docs/adr/0001-strangler-migration.md`
  - Steps: copy measured sizes (libobs 94k, frontend 89k, plugins 170k, deps 70k lines), mention 3 OSes and ~40 plugins, state the C-plugin-ABI constraint with link to `libobs/obs-module.h`.
  - Done when: every number has a reproducible command in a footnote (`find ... | xargs cat | wc -l`).
- [ ] **P0-01.3 Alternatives**
  - Steps: write big-bang rewrite, new repository, status quo, "Rust only for new plugins"; for each give cost, risk, why rejected.
  - Done when: each alternative has at least two concrete reasons.
- [ ] **P0-01.4 Exit criteria per phase**
  - Steps: copy the exit-criteria lines from each `phase-N-*.md` into a table; define "ships" (nightly passes smoke test on 3 OSes).
  - Done when: table has 8 rows and a measurable check each.
- [ ] **P0-01.5 Review and accept**
  - Steps: open PR with prefix `docs:`, request two maintainer reviews, address comments, set status Accepted.
  - Done when: PR merged; ADR index lists it.

### P0-02 ADR: plugin ABI policy
- [ ] **P0-02.1 Frozen header list**
  - Files: `libobs/obs-module.h`, `obs-source.h`, `obs-output.h`, `obs-encoder.h`, `obs-service.h`, `obs-data.h`, `obs-properties.h`, `obs-hotkey.h`, `obs-hotkeys.h`, `obs-interaction.h`, `obs-audio-controls.h`, `obs-nal.h`, `obs-avc.h`, `obs-hevc.h`, `obs-av1.h`, `libobs/graphics/*.h`, `libobs/media-io/*.h`, `libobs/util/*.h`, `libobs/callback/*.h`.
  - Steps: script lists every header installed by `libobs/CMakeLists.txt`; mark each public/internal.
  - Done when: list in ADR matches the install set exactly (script output attached).
- [ ] **P0-02.2 Versioning rules**
  - Files: `libobs/obs-config.h`
  - Steps: document `LIBOBS_API_MAJOR_VER/MINOR/PATCH` semantics, what is breaking (struct layout, removed symbol, changed callback signature).
  - Done when: three worked examples (add field at end, remove symbol, change enum value) are classified.
- [ ] **P0-02.3 Coexistence with Rust API**
  - Steps: define how a module declares which API it uses, loader order, naming (`obs-plugin-api`), and deprecation timeline placeholder.
  - Done when: sequence diagram of module load for both kinds is in the ADR.
- [ ] **P0-02.4 Contributor rule**
  - Files: `CONTRIBUTING.md`
  - Steps: add "public header changes need an ADR"; add a CODEOWNERS entry for the listed headers.
  - Done when: a test PR touching `obs-source.h` requests the owners automatically.

### P0-03 ADR: graphics backend
- [ ] **P0-03.1 Linux prototype**
  - Files: new throwaway repo `proto/wgpu-quad/`
  - Steps: textured quad, custom WGSL shader, Vulkan and GL backends of `wgpu`, measure frame time with timestamp queries.
  - Done when: 1080p60 and 4K60 numbers recorded for one Intel/AMD/NVIDIA GPU each.
- [ ] **P0-03.2 Windows and macOS prototypes**
  - Steps: same program on D3D12/Vulkan and Metal.
  - Done when: table of frame times per OS/GPU.
- [ ] **P0-03.3 Shared-texture import spike**
  - Files: reference `plugins/linux-pipewire/pipewire.c` (DMA-BUF), `plugins/win-capture/graphics-hook/`, `plugins/mac-syphon/`
  - Steps: import a DMA-BUF, a DXGI shared NT handle and an IOSurface into `wgpu` through the HAL.
  - Done when: each path shows either a working frame or a documented blocker with upstream issue link.
- [ ] **P0-03.4 Write ADR 0003**
  - Steps: summarise measurements, decide go/no-go, define fallback (retain `libobs-opengl`, `libobs-d3d11`, `libobs-metal`).
  - Done when: reviewed and accepted.

### P0-04 ADR: GUI toolkit
- [ ] **P0-04.1 Criteria**
  - Steps: list criteria with weights: theming (`frontend/data/themes`), accessibility (`frontend/components/Accessible*`), HiDPI, Wayland, Flatpak, binary size, contributor familiarity, embedding the preview (native window handle).
  - Done when: scoring sheet exists.
- [ ] **P0-04.2 `cxx-qt` prototype** — scene-list dock with drag-reorder, theme variables, preview-window embedding. Done when: runs on Linux and Windows and a risk list is written.
- [ ] **P0-04.3 Slint and egui prototypes** — same dock. Done when: preview embedding result recorded for each.
- [ ] **P0-04.4 Score and migration path**
  - Files: `frontend/forms/*.ui`
  - Steps: for the winner, describe how `.ui` forms and the `shared/qt` widgets (`icon-label`, `obs-spinbox`, `slider-ignorewheel`, `vertical-scroll-area`) are carried over.
  - Done when: migration path section exists.
- [ ] **P0-04.5 Write ADR 0004** and adjust Phase 6 wording if the toolkit is not `cxx-qt`. Done when: accepted and Phase 6 files updated in the same PR.

### P0-05 ADR: unsafe, MSRV, dependency policy
- [ ] **P0-05.1 Unsafe rules** — allowed only in `*-sys`, `obs-ffi`, platform crates; each block needs `// SAFETY:`; crate-level `#![deny(unsafe_op_in_unsafe_fn)]`. Done when: lint config committed in P0-07.
- [ ] **P0-05.2 MSRV** — track latest stable; bump within one release cycle; document the CI job that checks it. Done when: policy text merged.
- [ ] **P0-05.3 Dependency admission** — checklist: stdlib alternative considered, licence GPLv2+-compatible, maintained (release < 12 months), `cargo audit` clean, one-line justification in PR. Done when: checklist is in `CONTRIBUTING.md`.
- [ ] **P0-05.4 `-sys` and vendored code rule** — pin versions, forbid network in `build.rs`, document who updates FFmpeg/x264/CEF pins. Done when: rule referenced from ADR.

### P0-06 ADR: concurrency model
- [ ] **P0-06.1 Thread diagram**
  - Files: `libobs/obs-video.c`, `obs-audio.c`, `media-io/video-io.c`, `media-io/audio-io.c`, `obs-video-gpu-encode.c`, `obs-encoder.c`, `obs-output.c`
  - Steps: list each `pthread_create` / `os_event` loop and what it owns.
  - Done when: diagram with at least 6 thread kinds is in the ADR.
- [ ] **P0-06.2 Mutex table**
  - Files: `libobs/obs-internal.h`
  - Steps: extract every `pthread_mutex_t` field with: protects, taken by, taken while holding.
  - Done when: table has every mutex; unknown entries are marked TODO with an owner.
- [ ] **P0-06.3 Target model** — define lock hierarchy, RT threads, SPSC queues, tokio use. Done when: hierarchy rule can be checked by a debug-only lock-order assert.
- [ ] **P0-06.4 RT rules** — "no alloc, no lock, no syscall in audio callback/video tick"; define the test (P2-12). Done when: referenced from Phase 2.

## B. Workspace and CI

### P0-07 Cargo workspace skeleton
- [ ] **P0-07.1 Root manifest** — `Cargo.toml` `[workspace]`, `[workspace.package]` (edition, licence GPL-2.0-or-later, rust-version), `[workspace.dependencies]`, `[workspace.lints]`. Done when: `cargo metadata` works.
- [ ] **P0-07.2 Tool config** — `rust-toolchain.toml`, `rustfmt.toml`, `clippy.toml`, `.cargo/config.toml`. Done when: `cargo fmt --check` and `cargo clippy` run.
- [ ] **P0-07.3 Empty crates** — create `crates/obs-util`, `obs-math`, `obs-data`, `obs-signal`, `obs-media-io`, `obs-graphics`, `obs-core`, `obs-ffi`, `obs-cli`, each with a doc comment and a trivial test. Done when: `cargo test --workspace` passes.
- [ ] **P0-07.4 Three-OS build** — CI job `cargo build --workspace`. Done when: green on Linux, Windows, macOS.

### P0-08 Corrosion integration in CMake
- [ ] **P0-08.1 Fetch Corrosion**
  - Files: `cmake/common/`, `CMakeLists.txt`
  - Steps: `FetchContent_Declare(Corrosion ... GIT_TAG <pinned>)`.
  - Done when: configure works offline from cache.
- [ ] **P0-08.2 Option** — `option(OBS_ENABLE_RUST ... OFF)` and a `CMakePresets.json` variant. Done when: both presets configure.
- [ ] **P0-08.3 Link** — `corrosion_import_crate` for `obs-ffi` as a static lib; link to `libobs` when ON.
  - Done when: `nm libobs.so` shows a Rust test symbol when ON.
- [ ] **P0-08.4 CI** — run `ctest` for ON and OFF. Done when: both green.

### P0-09 CI pipeline for Rust
- [ ] **P0-09.1 Lint jobs** (`fmt`, `clippy -D warnings`) in `.github/workflows/`. Done when: failing a lint blocks the PR.
- [ ] **P0-09.2 Test jobs** — `cargo nextest run --workspace` on 3 OSes. Done when: test report artifacts uploaded.
- [ ] **P0-09.3 Supply chain** — `cargo deny check`, `cargo audit`, Miri on `obs-util`, `obs-math`, `obs-data`, `obs-signal`.
- [ ] **P0-09.4 Coverage and cache** — `cargo-llvm-cov`, `Swatinem/rust-cache`. Done when: coverage % in PR comment.
- [ ] **P0-09.5 Required checks** — update branch protection. Done when: settings screenshot/export in PR.

### P0-10 `cargo-deny` and licence policy
- [ ] **P0-10.1 `deny.toml` licences** — allow GPL-2.0-or-later compatible set (MIT, Apache-2.0, BSD-*, ISC, Zlib, MPL-2.0 per review). Done when: `cargo deny check licenses` passes.
- [ ] **P0-10.2 Bans** — ban `openssl-sys` if `rustls` chosen, duplicate major versions warn. Done when: rules documented.
- [ ] **P0-10.3 Exceptions process** — add section to ADR 0005. Done when: example exception written.

### P0-11 `.gitattributes`
- [ ] **P0-11.1 Create file** — add `*.text -text` and `*.golden -text`. Done when: `git check-attr` shows `-text` on a sample.
- [ ] **P0-11.2 Fixture audit**
  - Files: `test/`, `frontend/data/locale/*.ini`, `libobs/data/*.effect`
  - Steps: find fixtures compared byte-for-byte and add them.
  - Done when: list of added patterns in PR.
- [ ] **P0-11.3 Windows verification** — run golden tests on Windows CI. Done when: green without `autocrlf` tricks.

### P0-12 Dev container / build docs
- [ ] **P0-12.1 Linux doc** — write `docs/rust-rewrite/BUILDING.md` listing packages (FFmpeg, x264, Qt6, PipeWire, Wayland dev packages). Done when: a clean container builds following only the doc.
- [ ] **P0-12.2 Windows/macOS doc** — Visual Studio build tools, vcpkg or prebuilt deps, Xcode. Done when: tested on fresh VMs.
- [ ] **P0-12.3 Reproducible shell** — `.devcontainer/` or `flake.nix`; update `INSTALL`. Done when: `cargo test` runs inside it.

## C. Safety net

### P0-13 Golden-test framework
- [ ] **P0-13.1 Crate `obs-golden`** — `assert_golden(name, &str|&[u8])` reading `tests/golden/<name>.golden`. Done when: unit-tested.
- [ ] **P0-13.2 Update mode** — `UPDATE_GOLDEN=1` rewrites files and prints a summary. Done when: CI refuses to run with the variable set.
- [ ] **P0-13.3 Diff output** — unified diff (e.g. `similar`) truncated to N lines. Done when: mismatch shows first differing line and byte offset.
- [ ] **P0-13.4 Demo and matrix** — one demo test + `docs/tests/obs-golden.md`. Done when: matrix lists cases and expected results.

### P0-14 C reference dumper
- [ ] **P0-14.1 Target**
  - Files: `test/ref-dump/CMakeLists.txt`, `test/CMakeLists.txt`
  - Steps: new executable linking `libobs`; reuse the cmocka layout in `test/cmocka` as a pattern.
  - Done when: `obs-ref-dump --list` prints available dumps.
- [ ] **P0-14.2 Math dumps** — vec2/3/4, matrix3/4, quat, plane, bounds with seeded PRNG (fixed seed `0x0B5`). Done when: output stable across 3 OSes (print floats with `%.9g`).
- [ ] **P0-14.3 Data dumps** — `obs_data` build/serialise/parse for corpus files; `calldata` pack/unpack. Done when: JSON output sorted deterministically.
- [ ] **P0-14.4 Media dumps** — audio mix of generated sine waves; pixel conversion of a generated gradient for NV12/I420/UYVY/YUY2. Done when: hashes recorded.
- [ ] **P0-14.5 Generate and verify** — run twice, diff must be empty, commit under `test/golden/`. Done when: CI regenerates and diffs in a job.

### P0-15 ABI conformance test plugin
- [ ] **P0-15.1 Extend the existing plugin**
  - Files: `test/test-input/test-input.c`, `test-filter.c`
  - Steps: add trace logging to each callback (`create`, `update`, `video_tick`, `video_render`, `get_properties`, `destroy`); do not break the existing tests.
  - Done when: existing test-input users unaffected.
- [ ] **P0-15.2 Encoder/output/service** — add a dummy encoder, output and service (`obs_register_encoder/output/service`). Done when: all register in `obs_module_load`.
- [ ] **P0-15.3 Test host** — C program loads the module with `obs_open_module`/`obs_init_module`, creates one object of each kind, drives 10 ticks, writes trace.
- [ ] **P0-15.4 Expected traces** — commit trace goldens; compare in cmocka/ctest. Done when: stable across runs.
- [ ] **P0-15.5 CI wiring** — `ctest -R abi`. Done when: runs for `OBS_ENABLE_RUST` ON and OFF.

### P0-16 Headless smoke test
- [ ] **P0-16.1 Host program** — no UI; `obs_startup`, `obs_reset_video` 1280x720@30, `obs_reset_audio` 48k stereo. Done when: starts on a machine without display (null/software graphics).
- [ ] **P0-16.2 Scene** — colour source + text (freetype) source in a scene set as output 0. Done when: scene renders a non-black frame.
- [ ] **P0-16.3 Record** — `ffmpeg_muxer` + x264 + AAC for 5 s to MKV. Done when: file non-empty.
- [ ] **P0-16.4 Validate** — `ffprobe` JSON: duration 4.5–5.5 s, h264, 1280x720, one audio stream.
- [ ] **P0-16.5 CI job** — run under `xvfb-run` on Linux; document Windows/macOS equivalents.

### P0-17 Benchmark harness
- [ ] **P0-17.1 Harness** — criterion crate `benches/`, C baseline runner printing the same metrics as JSON. Done when: one bench runs in both.
- [ ] **P0-17.2 Math/pixel benches** — 1M matrix multiplies; 1080p NV12→RGB. Done when: stable ±3% between runs.
- [ ] **P0-17.3 Audio/render benches** — mix 8 tracks × 1024 frames; render 20-item scene headless. Done when: stable.
- [ ] **P0-17.4 Baselines** — record machine spec + numbers in `docs/rust-rewrite/benchmarks.md`.
- [ ] **P0-17.5 Regression gate** — job compares PR vs stored baseline; fails > 2%. Done when: deliberate slowdown trips it.

### P0-18 Scene-collection corpus
- [ ] **P0-18.1 Anonymiser** — script strips absolute paths, URLs, stream keys, window titles. Done when: unit test with a seeded secret finds none.
- [ ] **P0-18.2 Collect** — ≥20 files from consenting users covering browser, game capture, media, VST, NDI-less sources. Done when: consent log kept outside the repo.
- [ ] **P0-18.3 Load check** — each file opens in the current frontend without errors. Done when: log attached.
- [ ] **P0-18.4 Index** — `test/fixtures/README.md` listing which source/filter/transition ids each file uses. Done when: all ids used by in-tree plugins are covered or listed as gaps.

## D. Tracking

### P0-19 Issue templates and labels
- [ ] **P0-19.1 Labels** — `rust-rewrite`, `phase-0`…`phase-7`, `abi`, `perf`, `blocked-submodule`. Done when: visible in repo.
- [ ] **P0-19.2 Template** — `.github/ISSUE_TEMPLATE/rust-task.yml` with fields from the task format. Done when: template creates a valid issue.
- [ ] **P0-19.3 Phase issues** — one tracking issue per phase linking the task files. Done when: 8 issues exist.

### P0-20 Test matrices
- [ ] **P0-20.1 Directory and template** — `docs/tests/README.md`, `_template.md` (Case / Input / Expected / Platforms / Status).
- [ ] **P0-20.2 Golden framework matrix** — `docs/tests/obs-golden.md`.
- [ ] **P0-20.3 ABI plugin matrix** — `docs/tests/abi-conformance.md`.
- [ ] **P0-20.4 CI check** — script fails if a `tests/*.rs` or `test_*.c` has no matching matrix. Done when: negative test proves it.

### P0-21 Phase 0 review
- [ ] **P0-21.1 Demo** — run golden, ABI, smoke, benchmark jobs live. Done when: recording or CI links attached.
- [ ] **P0-21.2 Verify exit criteria** — ADRs 1–6 accepted, CI green on 3 OSes, goldens generated from C. Done when: checklist ticked in `phase-0-foundation.md`.
- [ ] **P0-21.3 Re-estimate** — update Phase 1–2 sizes using measured baselines. Done when: PR edits the phase files.
