# Phase 0 subtasks

Parent file: [../phase-0-foundation.md](../phase-0-foundation.md). Subtask IDs are `<task>.<n>`. Each subtask is at most one day of work and has **Files** (what is touched), **Steps** (how), and **Done when** (verifiable result).

Facts verified in this checkout that shape Phase 0:
- `test/` already contains a cmocka suite (`test/cmocka/test_bitstream.c`, `test_darray.c`, `test_os_path.c`, `test_serializer.c`) and a test plugin (`test/test-input/`: `test-input.c`, `test-filter.c`, `test-random.c`, `test-sinewave.c`, `sync-*.c`). Phase 0 extends these instead of starting from zero.
- `plugins/obs-websocket`, `plugins/obs-browser` and `deps/libdshowcapture/src` are git submodules (see `.gitmodules`) and are empty unless initialised: run `git submodule update --init` before any task that reads them.
- C line counts below are measured in the read-only checkout: `git -C ~/github/obs-studio ls-files <path> | xargs wc -l`.

## A. Decisions (ADRs)

### P0-01 ADR: migration strategy
- [x] **P0-01.1 ADR template**
  - Files: `docs/adr/0000-template.md` (new), `docs/adr/README.md` (new)
  - Steps: create sections Status / Context / Decision / Alternatives / Consequences / Links; add numbering rule (4 digits) and "supersedes" field.
  - Done when: template renders on GitHub and README lists how to add an ADR.
- [x] **P0-01.2 Context section**
  - Files: `docs/adr/0001-strangler-migration.md` (new)
  - Steps: copy measured sizes (libobs 94k, frontend 109k incl. `.ui`, plugins 172k, shared 22k, deps 70k lines — all measured over the same tracked set as `tools/task-coverage.py`: `.c .cpp .cc .h .hpp .m .mm .swift .ui .effect`), mention 3 OSes and 41 plugins, state the C-plugin-ABI constraint with link to `libobs/obs-module.h` (201).
  - Done when: every number has a reproducible command in a footnote, e.g. `git ls-files libobs/ | grep -E '\.(c|cpp|cc|h|hpp|m|mm|swift|ui|effect)$' | xargs cat | wc -l`.
- [x] **P0-01.3 Alternatives**
  - Files: `docs/adr/0001-strangler-migration.md`
  - Steps: write big-bang rewrite, new repository, status quo, "Rust only for new plugins"; for each give cost, risk, why rejected.
  - Done when: each alternative has at least two concrete reasons.
- [x] **P0-01.4 Exit criteria per phase**
  - Files: `docs/adr/0001-strangler-migration.md`; sources `docs/rust-rewrite/phase-0-foundation.md`, `phase-1-leaf-libraries.md`, `phase-2-media-pipeline.md`, `phase-3-graphics.md`, `phase-4-core-object-model.md`, `phase-5-plugins.md`, `phase-6-frontend.md`, `phase-7-retire-c.md` (8 files)
  - Steps: copy the exit-criteria lines from each `phase-N-*.md` into a table; define "ships" (nightly passes smoke test on 3 OSes).
  - Done when: table has 8 rows and a measurable check each.
- [ ] **P0-01.5 Review and accept**
  - Files: `docs/adr/0001-strangler-migration.md`, `docs/adr/README.md`
  - Steps: open PR with prefix `docs:`, request two maintainer reviews, address comments, set status Accepted.
  - Done when: PR merged; ADR index lists it.

### P0-02 ADR: plugin ABI policy
- [ ] **P0-02.1 Frozen header list**
  - Files: `libobs/` public headers — `obs-module.h` (201), `obs-source.h` (572), `obs-output.h` (96), `obs-encoder.h` (365), `obs-service.h` (115), `obs-data.h` (314), `obs-properties.h` (364), `obs-hotkey.h` (272), `obs-hotkeys.h` (653), `obs-interaction.h` (56), `obs-audio-controls.h` (250), `obs-nal.h` (37), `obs-avc.h` (55), `obs-hevc.h` (81), `obs-av1.h` (47); all 21 headers in `libobs/graphics/` — `axisang.h` (65), `bounds.h` (108), `device-exports.h` (177), `effect-parser.h` (290), `effect.h` (190), `graphics-internal.h` (335), `graphics.h` (1025), `half.h` (100), `image-file.h` (58), `input.h` (34), `math-defs.h` (45), `math-extra.h` (61), `matrix3.h` (98), `matrix4.h` (102), `plane.h` (85), `quat.h` (170), `shader-parser.h` (273), `srgb.h` (177), `vec2.h` (148), `vec3.h` (224), `vec4.h` (241), plus internal `libnsgif/lzw.h` (137), `libnsgif/nsgif.h` (527); all 10 headers in `libobs/media-io/` — `audio-io.h` (228), `audio-math.h` (43), `audio-resampler.h` (44), `format-conversion.h` (50), `frame-rate.h` (29), `media-io-defs.h` (20), `media-remux.h` (37), `video-frame.h` (64), `video-io.h` (338), `video-scaler.h` (43); all 30 top-level headers in `libobs/util/` — `array-serializer.h` (37), `base.h` (97), `bitstream.h` (28), `bmem.h` (94), `buffered-file-serializer.h` (32), `c99defs.h` (79), `cf-lexer.h` (199), `cf-parser.h` (281), `config-file.h` (103), `crc32.h` (29), `darray.h` (606), `deque.h` (319), `dstr.h` (320), `file-serializer.h` (34), `lexer.h` (273), `pipe.h` (51), `platform.h` (224), `profiler.h` (97), `serializer.h` (158), `source-profiler.h` (66), `sse-intrin.h` (41), `task.h` (22), `text-lookup.h` (45), `threading.h` (103), `threading-posix.h` (77), `threading-windows.h` (142), `utf8.h` (35), `uthash.h` (34), `util_uint64.h` (34), `util_uint128.h` (108); all 4 headers in `libobs/callback/` — `calldata.h` (195), `decl.h` (61), `proc.h` (52), `signal.h` (73)
  - Steps: script lists every header installed by `libobs/CMakeLists.txt` (396); mark each public/internal.
  - Done when: list in ADR matches the install set exactly (script output attached).
- [x] **P0-02.2 Versioning rules**
  - Files: `libobs/obs-config.h` (52)
  - Steps: document `LIBOBS_API_MAJOR_VER/MINOR/PATCH` semantics, what is breaking (struct layout, removed symbol, changed callback signature).
  - Done when: three worked examples (add field at end, remove symbol, change enum value) are classified.
- [x] **P0-02.3 Coexistence with Rust API**
  - Files: `docs/adr/0002-plugin-abi.md` (new); loader reference `libobs/obs-module.c` (1274: `obs_open_module`/`obs_init_module`)
  - Steps: define how a module declares which API it uses, loader order, naming (`obs-plugin-api`), and deprecation timeline placeholder.
  - Done when: sequence diagram of module load for both kinds is in the ADR.
- [ ] **P0-02.4 Contributor rule**
  - Files: `CONTRIBUTING.md`, `CODEOWNERS` (new); headers as in P0-02.1
  - Steps: add "public header changes need an ADR"; add a CODEOWNERS entry for the listed headers.
  - Done when: a test PR touching `obs-source.h` requests the owners automatically.

### P0-03 ADR: graphics backend
- [ ] **P0-03.1 Linux prototype**
  - Files: new throwaway repo `proto/wgpu-quad/`
  - Steps: textured quad, custom WGSL shader, Vulkan and GL backends of `wgpu`, measure frame time with timestamp queries.
  - Done when: 1080p60 and 4K60 numbers recorded for one Intel/AMD/NVIDIA GPU each.
- [ ] **P0-03.2 Windows and macOS prototypes**
  - Files: `proto/wgpu-quad/` (same program, D3D12/Vulkan on Windows, Metal on macOS)
  - Steps: same program on D3D12/Vulkan and Metal.
  - Done when: table of frame times per OS/GPU.
- [ ] **P0-03.3 Shared-texture import spike**
  - Files: `plugins/linux-pipewire/pipewire.c` (1471, DMA-BUF); `plugins/win-capture/graphics-hook/` (19 files; C: `graphics-hook.c` 907, `vulkan-capture.c` 2103, `gl-capture.c` 881, `d3d8/9/10/11/12-capture.cpp`, `dxgi-capture.cpp`); `plugins/mac-syphon/` (`syphon.m` 749, `SyphonOBSClient.m` 18, `plugin-main.c` 16)
  - Steps: import a DMA-BUF, a DXGI shared NT handle and an IOSurface into `wgpu` through the HAL.
  - Done when: each path shows either a working frame or a documented blocker with upstream issue link.
- [ ] **P0-03.4 Write ADR 0003**
  - Files: `docs/adr/0003-graphics.md` (new); fallback modules `libobs-opengl/` (8880 lines), `libobs-d3d11/` (7268), `libobs-metal/` (8530)
  - Steps: summarise measurements, decide go/no-go, define fallback (retain `libobs-opengl`, `libobs-d3d11`, `libobs-metal`).
  - Done when: reviewed and accepted.

### P0-04 ADR: GUI toolkit
- [ ] **P0-04.1 Criteria**
  - Files: `frontend/data/themes/` (13 asset entries), `frontend/components/` Accessible* (6 files: `AccessibleAlignmentCell.{cpp,hpp}`, `AccessibleAlignmentSelector.{cpp,hpp}`, `VolumeAccessibleInterface.{cpp,hpp}`), `frontend/forms/` (29 `.ui` files)
  - Steps: list criteria with weights: theming (`frontend/data/themes`), accessibility (`frontend/components/Accessible*`), HiDPI, Wayland, Flatpak, binary size, contributor familiarity, embedding the preview (native window handle).
  - Done when: scoring sheet exists.
- [ ] **P0-04.2 `cxx-qt` prototype** — new throwaway `proto/gui-cxxqt/` (scene-list dock from `frontend/forms/*.ui` patterns) with drag-reorder, theme variables, preview-window embedding. Done when: runs on Linux and Windows and a risk list is written.
- [ ] **P0-04.3 Slint and egui prototypes** — new throwaway `proto/gui-slint/`, `proto/gui-egui/`: same dock. Done when: preview embedding result recorded for each.
- [ ] **P0-04.4 Score and migration path**
  - Files: `frontend/forms/` (29 `.ui` files); `shared/qt/icon-label/IconLabel.{hpp,cpp}` (46+31), `shared/qt/obs-spinbox/{SpinBox,DoubleSpinBox,AutoSelectLineEdit}.{hpp,cpp}` (45+244, 45+245, 38+28), `shared/qt/slider-ignorewheel/slider-ignorewheel.{hpp,cpp}` (33+49), `shared/qt/vertical-scroll-area/vertical-scroll-area.{hpp,cpp}` (18+11)
  - Steps: for the winner, describe how `.ui` forms and the `shared/qt` widgets (`icon-label`, `obs-spinbox`, `slider-ignorewheel`, `vertical-scroll-area`) are carried over.
  - Done when: migration path section exists.
- [ ] **P0-04.5 Write ADR 0004** — Files: `docs/adr/0004-gui-toolkit.md` (new), `docs/rust-rewrite/phase-6-frontend.md`. And adjust Phase 6 wording if the toolkit is not `cxx-qt`. Done when: accepted and Phase 6 files updated in the same PR.

### P0-05 ADR: unsafe, MSRV, dependency policy
- [x] **P0-05.1 Unsafe rules** — Files: `docs/adr/0005-code-policy.md` (new); lint config lands in `[workspace.lints]` (P0-07.1). Allowed only in `*-sys`, `obs-ffi`, platform crates; each block needs `// SAFETY:`; crate-level `#![deny(unsafe_op_in_unsafe_fn)]`. Done when: lint config committed in P0-07.
- [ ] **P0-05.2 MSRV** — Files: `docs/adr/0005-code-policy.md` (new), `.github/workflows/` (MSRV job). Track latest stable; bump within one release cycle; document the CI job that checks it. Done when: policy text merged.
- [x] **P0-05.3 Dependency admission** — Files: `CONTRIBUTING.md`. Checklist: stdlib alternative considered, licence GPLv2+-compatible, maintained (release < 12 months), `cargo audit` clean, one-line justification in PR. Done when: checklist is in `CONTRIBUTING.md`.
- [x] **P0-05.4 `-sys` and vendored code rule** — Files: `docs/adr/0005-code-policy.md` (new); pins live in `cmake/` dependency presets and `deps/`. Pin versions, forbid network in `build.rs`, document who updates FFmpeg/x264/CEF pins. Done when: rule referenced from ADR.

### P0-06 ADR: concurrency model
- [x] **P0-06.1 Thread diagram**
  - Files: `libobs/obs-video.c` (1200), `obs-audio.c` (721), `media-io/video-io.c` (679), `media-io/audio-io.c` (455), `obs-video-gpu-encode.c` (312), `obs-encoder.c` (2286), `obs-output.c` (3340)
  - Steps: list each `pthread_create` / `os_event` loop and what it owns.
  - Done when: diagram with at least 6 thread kinds is in the ADR.
- [ ] **P0-06.2 Mutex table**
  - Files: `libobs/obs-internal.h` (1543)
  - Steps: extract every `pthread_mutex_t` field with: protects, taken by, taken while holding.
  - Done when: table has every mutex; unknown entries are marked TODO with an owner.
- [x] **P0-06.3 Target model** — Files: `docs/adr/0006-concurrency.md` (new). Define lock hierarchy, RT threads, SPSC queues, tokio use. Done when: hierarchy rule can be checked by a debug-only lock-order assert.
- [x] **P0-06.4 RT rules** — Files: `docs/adr/0006-concurrency.md` (new). "No alloc, no lock, no syscall in audio callback/video tick"; define the test (P2-12). Done when: referenced from Phase 2.

## B. Workspace and CI

### P0-07 Cargo workspace skeleton
- [x] **P0-07.1 Root manifest** — Files: `Cargo.toml` (new). `[workspace]`, `[workspace.package]` (edition, licence GPL-2.0-or-later, rust-version), `[workspace.dependencies]`, `[workspace.lints]`. Done when: `cargo metadata` works.
- [x] **P0-07.2 Tool config** — Files: `rust-toolchain.toml`, `rustfmt.toml`, `clippy.toml`, `.cargo/config.toml` (all new). Done when: `cargo fmt --check` and `cargo clippy` run.
- [x] **P0-07.3 Empty crates** — Files: new `crates/obs-util`, `obs-math`, `obs-data`, `obs-signal`, `obs-media-io`, `obs-graphics`, `obs-core`, `obs-ffi`, `obs-cli`, each with a doc comment and a trivial test. Done when: `cargo test --workspace` passes.
- [ ] **P0-07.4 Three-OS build** — Files: `.github/workflows/` (new job `cargo build --workspace`). Done when: green on Linux, Windows, macOS.

### P0-08 Corrosion integration in CMake
- [ ] **P0-08.1 Fetch Corrosion**
  - Files: `CMakeLists.txt` (37), `cmake/common/`, `libobs/CMakeLists.txt` (396)
  - Steps: `FetchContent_Declare(Corrosion ... GIT_TAG <pinned>)`.
  - Done when: configure works offline from cache.
- [ ] **P0-08.2 Option** — Files: `CMakeLists.txt` (37), `CMakePresets.json`. `option(OBS_ENABLE_RUST ... OFF)` and a `CMakePresets.json` variant. Done when: both presets configure.
- [ ] **P0-08.3 Link** — Files: `libobs/CMakeLists.txt` (396). `corrosion_import_crate` for `obs-ffi` as a static lib; link to `libobs` when ON.
  - Done when: `nm libobs.so` shows a Rust test symbol when ON.
- [ ] **P0-08.4 CI** — Files: `.github/workflows/`. Run `ctest` for ON and OFF. Done when: both green.

### P0-09 CI pipeline for Rust
- [ ] **P0-09.1 Lint jobs** — Files: `.github/workflows/` (new jobs `fmt`, `clippy -D warnings`). Done when: failing a lint blocks the PR.
- [ ] **P0-09.2 Test jobs** — Files: `.github/workflows/`. `cargo nextest run --workspace` on 3 OSes. Done when: test report artifacts uploaded.
- [ ] **P0-09.3 Supply chain** — Files: `.github/workflows/`. `cargo deny check`, `cargo audit`, Miri on `obs-util`, `obs-math`, `obs-data`, `obs-signal`.
- [ ] **P0-09.4 Coverage and cache** — Files: `.github/workflows/`. `cargo-llvm-cov`, `Swatinem/rust-cache`. Done when: coverage % in PR comment.
- [ ] **P0-09.5 Required checks** — Files: GitHub branch-protection settings. Update branch protection. Done when: settings screenshot/export in PR.

### P0-10 `cargo-deny` and licence policy
- [ ] **P0-10.1 `deny.toml` licences** — Files: `deny.toml` (new). Allow GPL-2.0-or-later compatible set (MIT, Apache-2.0, BSD-*, ISC, Zlib, MPL-2.0 per review). Done when: `cargo deny check licenses` passes.
- [ ] **P0-10.2 Bans** — Files: `deny.toml`. Ban `openssl-sys` if `rustls` chosen, duplicate major versions warn. Done when: rules documented.
- [x] **P0-10.3 Exceptions process** — Files: `docs/adr/0005-code-policy.md` (new section). Add section to ADR 0005. Done when: example exception written.

### P0-11 `.gitattributes`
- [x] **P0-11.1 Create file** — Files: `.gitattributes` (new). Add `*.text -text` and `*.golden -text`. Done when: `git check-attr` shows `-text` on a sample.
- [ ] **P0-11.2 Fixture audit**
  - Files: `test/cmocka/` (`test_bitstream.c` 36, `test_darray.c` 31, `test_os_path.c` 53, `test_serializer.c` 36), `frontend/data/locale/` (77 `.ini` files), `libobs/data/` (22 `.effect` files)
  - Steps: find fixtures compared byte-for-byte and add them.
  - Done when: list of added patterns in PR.
- [ ] **P0-11.3 Windows verification** — run golden tests on Windows CI. Done when: green without `autocrlf` tricks.

### P0-12 Dev container / build docs
- [ ] **P0-12.1 Linux doc** — Files: `docs/rust-rewrite/BUILDING.md` (new), `INSTALL`. Write `docs/rust-rewrite/BUILDING.md` listing packages (FFmpeg, x264, Qt6, PipeWire, Wayland dev packages). Done when: a clean container builds following only the doc.
- [ ] **P0-12.2 Windows/macOS doc** — Files: `docs/rust-rewrite/BUILDING.md` (new section). Visual Studio build tools, vcpkg or prebuilt deps, Xcode. Done when: tested on fresh VMs.
- [ ] **P0-12.3 Reproducible shell** — Files: `.devcontainer/` (new) or `flake.nix` (new), `INSTALL`. Update `INSTALL`. Done when: `cargo test` runs inside it.

## C. Safety net

### P0-13 Golden-test framework
- [ ] **P0-13.1 Crate `obs-golden`** — Files: new crate `crates/obs-golden/`. `assert_golden(name, &str|&[u8])` reading `tests/golden/<name>.golden`. Done when: unit-tested.
- [ ] **P0-13.2 Update mode** — `UPDATE_GOLDEN=1` rewrites files and prints a summary. Done when: CI refuses to run with the variable set.
- [ ] **P0-13.3 Diff output** — unified diff (e.g. `similar`) truncated to N lines. Done when: mismatch shows first differing line and byte offset.
- [ ] **P0-13.4 Demo and matrix** — Files: `docs/tests/obs-golden.md` (new). One demo test + matrix. Done when: matrix lists cases and expected results.

### P0-14 C reference dumper
- [ ] **P0-14.1 Target**
  - Files: `test/ref-dump/CMakeLists.txt` (new), `test/CMakeLists.txt` (15; add subdirectory next to `cmocka`), layout pattern `test/cmocka/CMakeLists.txt` (30)
  - Steps: new executable linking `libobs`; reuse the cmocka layout in `test/cmocka` as a pattern.
  - Done when: `obs-ref-dump --list` prints available dumps.
- [ ] **P0-14.2 Math dumps**
  - Files: `libobs/graphics/vec3.c` (87), `vec4.c` (41), `matrix3.c` (138), `matrix4.c` (333), `quat.c` (215), `axisang.c` (38), `plane.c` (151), `bounds.c` (314), `math-extra.c` (131), `vec2.h` (148, header-only part)
  - Steps: vec2/3/4, matrix3/4, quat, plane, bounds with seeded PRNG (fixed seed `0x0B5`); print floats `%.9g`.
  - Spec: S-008, S-009 (describe the dumped behaviour; the Phase 1 port is checked against it)
  - Vectors: `test/conformance/scripts/math/` (S-008), `math2/` (S-009)
  - Done when: output stable across 3 OSes (print floats with `%.9g`).
- [ ] **P0-14.3 Data dumps**
  - Files: `libobs/obs-data.c` (2181), `libobs/callback/calldata.c` (238)
  - Steps: `obs_data` build/serialise/parse for corpus files; `calldata` pack/unpack.
  - Spec: S-014, S-002
  - Vectors: `test/conformance/scripts/obsdata/`, `calldata/`
  - Done when: JSON output sorted deterministically.
- [ ] **P0-14.4 Media dumps**
  - Files: `libobs/media-io/format-conversion.c` (288), `libobs/obs-audio.c` (721)
  - Steps: audio mix of generated sine waves; pixel conversion of a generated gradient for NV12/I420/UYVY/YUY2. Note: `mix_audio` is `static inline` in `obs-audio.c` (line 90), so the dumper compiles that function in (include the .c or a verbatim copy) rather than linking it; `format-conversion.c` functions are exported.
  - Spec: S-013
  - Vectors: `test/conformance/scripts/mediaconv/`
  - Done when: hashes recorded.
- [ ] **P0-14.5 Generate and verify** — Files: `test/golden/` (new). Run twice, diff must be empty, commit under `test/golden/`. Done when: CI regenerates and diffs in a job.

### P0-15 ABI conformance test plugin
- [ ] **P0-15.1 Extend the existing plugin**
  - Files: `test/test-input/test-input.c` (23), `test-filter.c` (71)
  - Steps: add trace logging to each callback (`create`, `update`, `video_tick`, `video_render`, `get_properties`, `destroy`); do not break the existing tests.
  - Done when: existing test-input users unaffected.
- [ ] **P0-15.2 Encoder/output/service** — Files: `test/test-input/test-input.c` (extend `obs_module_load`), registration API in `libobs/obs-module.h` (201). Add a dummy encoder, output and service (`obs_register_encoder/output/service`). Done when: all register in `obs_module_load`.
- [ ] **P0-15.3 Test host** — Files: `test/abi-host/abi-host.c` (new executable), `test/abi-host/CMakeLists.txt` (new). C program loads the module with `obs_open_module`/`obs_init_module`, creates one object of each kind, drives 10 ticks, writes trace.
- [ ] **P0-15.4 Expected traces** — Files: `test/abi-host/traces/*.golden` (new). Commit trace goldens; compare in cmocka/ctest. Done when: stable across runs.
- [ ] **P0-15.5 CI wiring** — Files: `.github/workflows/`, `test/CMakeLists.txt` (15). `ctest -R abi`. Done when: runs for `OBS_ENABLE_RUST` ON and OFF.

### P0-16 Headless smoke test
- [ ] **P0-16.1 Host program** — Files: `test/headless/main.c` (new), `test/headless/CMakeLists.txt` (new). No UI; `obs_startup`, `obs_reset_video` 1280x720@30, `obs_reset_audio` 48k stereo. Done when: starts on a machine without display (null/software graphics).
- [ ] **P0-16.2 Scene** — Files: `test/headless/main.c` (extend). Colour source + text (freetype) source in a scene set as output 0. Done when: scene renders a non-black frame.
- [ ] **P0-16.3 Record** — Files: `test/headless/main.c` (extend); encoders from `plugins/obs-ffmpeg/ffmpeg-mux/ffmpeg-mux.c` (1201), `plugins/obs-x264/obs-x264.c` (865), `plugins/obs-libfdk/obs-libfdk.c` (327). `ffmpeg_muxer` + x264 + AAC for 5 s to MKV. Done when: file non-empty.
- [ ] **P0-16.4 Validate** — `ffprobe` JSON: duration 4.5–5.5 s, h264, 1280x720, one audio stream.
- [ ] **P0-16.5 CI job** — Files: `.github/workflows/`. Run under `xvfb-run` on Linux; document Windows/macOS equivalents.

### P0-17 Benchmark harness
- [ ] **P0-17.1 Harness** — Files: `crates/obs-math/benches/`, `crates/obs-media-io/benches/` (new criterion benches), `test/ref-bench/ref-bench.c` (new C baseline runner). Criterion crate `benches/`, C baseline runner printing the same metrics as JSON. Done when: one bench runs in both.
- [ ] **P0-17.2 Math/pixel benches** — C baseline over `libobs/graphics/*.c` (see P0-14.2) and `libobs/media-io/format-conversion.c` (288). 1M matrix multiplies; 1080p NV12→RGB. Done when: stable ±3% between runs.
- [ ] **P0-17.3 Audio/render benches** — mix 8 tracks × 1024 frames; render 20-item scene headless. Done when: stable.
- [ ] **P0-17.4 Baselines** — Files: `docs/rust-rewrite/benchmarks.md` (new). Record machine spec + numbers. Done when: baseline committed with the machine spec.
- [ ] **P0-17.5 Regression gate** — job compares PR vs stored baseline; fails > 2%. Done when: deliberate slowdown trips it.

### P0-18 Scene-collection corpus
- [ ] **P0-18.1 Anonymiser** — Files: `test/fixtures/anonymise.py` (new). Script strips absolute paths, URLs, stream keys, window titles. Done when: unit test with a seeded secret finds none.
- [ ] **P0-18.2 Collect** — ≥20 files from consenting users covering browser, game capture, media, VST, NDI-less sources. Done when: consent log kept outside the repo.
- [ ] **P0-18.3 Load check** — each file opens in the current frontend without errors. Done when: log attached.
- [ ] **P0-18.4 Index** — Files: `test/fixtures/README.md` (new). Listing which source/filter/transition ids each file uses. Done when: all ids used by in-tree plugins are covered or listed as gaps.

## D. Tracking

### P0-19 Issue templates and labels
- [ ] **P0-19.1 Labels** — Files: GitHub repo settings. `rust-rewrite`, `phase-0`…`phase-7`, `abi`, `perf`, `blocked-submodule`. Done when: visible in repo.
- [x] **P0-19.2 Template** — Files: `.github/ISSUE_TEMPLATE/rust-task.yml` (new) with fields from the task format. Done when: template creates a valid issue.
- [ ] **P0-19.3 Phase issues** — Files: GitHub issues linking the 8 `docs/rust-rewrite/phase-*.md` files. One tracking issue per phase. Done when: 8 issues exist.

### P0-20 Test matrices
- [x] **P0-20.1 Directory and template** — Files: `docs/tests/README.md`, `docs/tests/_template.md` (new: Case / Input / Expected / Platforms / Status). Done when: both files exist.
- [ ] **P0-20.2 Golden framework matrix** — Files: `docs/tests/obs-golden.md` (new). Matrix lists the demo-test cases. Done when: matrix lists the cases.
- [ ] **P0-20.3 ABI plugin matrix** — Files: `docs/tests/abi-conformance.md` (new). Matrix lists the lifecycle cases. Done when: matrix lists them.
- [ ] **P0-20.4 CI check** — Files: `.github/workflows/` (new check). Script fails if a `tests/*.rs` or `test_*.c` has no matching matrix. Done when: negative test proves it.

### P0-21 Phase 0 review
- [ ] **P0-21.1 Demo** — run golden, ABI, smoke, benchmark jobs live. Done when: recording or CI links attached.
- [ ] **P0-21.2 Verify exit criteria** — Files: `docs/rust-rewrite/phase-0-foundation.md` (tick list). ADRs 1–6 accepted, CI green on 3 OSes, goldens generated from C. Done when: checklist ticked in `phase-0-foundation.md`.
- [ ] **P0-21.3 Re-estimate** — Files: `docs/rust-rewrite/phase-1-leaf-libraries.md`, `phase-2-media-pipeline.md`. Update Phase 1–2 sizes using measured baselines. Done when: PR edits the phase files.
