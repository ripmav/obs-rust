# Phase 7 subtasks

Parent file: [../phase-7-retire-c.md](../phase-7-retire-c.md). Each subtask has **Files**, **Steps**, **Done when**.

**Delete recipe** (used below): (1) `grep -r` for includes/link references and fix them; (2) tag the last commit that contains the code; (3) delete in one PR with the CMake/CI references; (4) update docs; (5) full CI on 3 OSes; (6) keep the tag listed in `docs/rust-rewrite/retired.md`.

Verified: the top-level directories are `libobs`, `libobs-opengl`, `libobs-d3d11`, `libobs-metal` (Swift), `libobs-winrt`, `frontend`, `plugins`, `deps`, `shared`, `test`, `cmake`, `build-aux`, `additional_install_files`, `docs` (sphinx). Ownership outside this phase: packaging `build-aux/`/`additional_install_files/` by P6-42…P6-44 (Windows install-tree layout P6-42.1), `docs/sphinx` by P7-14.1; `cmake/` and the root CMake files are removed by P7-06.3. `deps/` contains `blake2`, `glad`, `json11`, `libcaption`, `libdshowcapture`, `w32-pthreads` (`libnsgif` is not in `deps/`: it is `libobs/graphics/libnsgif/` and goes away with P7-01). `shared/` contains `bpm`, `file-updater`, `happy-eyeballs`, `ipc-util`, `media-playback`, `obs-d3d8-api`, `obs-hook-config`, `obs-inject-library`, `obs-scripting`, `obs-shared-memory-queue`, `obs-tiny-nv12-scale`, `opts-parser`, `properties-view`, `qt`.

## A. Removal

### P7-01 Delete C libobs and backends
- [ ] **P7-01.1 Preconditions** — `wgpu` default for ≥2 releases, no open P0/P1 graphics bugs, legacy fallback usage < agreed threshold from telemetry or issue count. Done when: checklist signed off.
- [ ] **P7-01.2 Tag** `last-c-libobs`.
- [ ] **P7-01.3 Delete** `libobs/`, `libobs-opengl/`, `libobs-d3d11/`, `libobs-metal/`, `libobs-winrt/`, update `CMakeLists.txt`/CI per delete recipe. Done when: build and tests green.

### P7-02 Delete C++ frontend
- [ ] **P7-02.1 Preconditions** — parity checklist (P6-48.1) 100%, beta feedback closed.
- [ ] **P7-02.2 Tag** `last-cpp-frontend`.
- [ ] **P7-02.3 Delete** `frontend/` including `.ui` forms (themes/locale moved to the Rust app's data dir first), per delete recipe. Done when: installers still contain locale and themes, CI green on 3 OSes, tag listed in `docs/rust-rewrite/retired.md`.

### P7-03 Delete C plugins
- [ ] **P7-03.1 List** — plugin → status table from P5-50.
- [ ] **P7-03.2 Delete** each fully ported plugin dir in its own commit, tagged `last-c-plugin-<name>` on the last commit containing it first (so `git revert` per plugin works); CMake/CI references in the same PR per delete recipe. Done when: only the P7-03.3 leftovers remain under `plugins/` and the tags are listed in `docs/rust-rewrite/retired.md`.
- [ ] **P7-03.3 FFI leftovers** — document `obs-browser` CEF glue (incl. the page-process helpers: `obs-browser-page` on Windows, the `OBS Helper*` bundles on macOS, `browser-helper` on Linux), DeckLink/AJA SDK glue, `libdshowcapture` (INTERFACE lib; header surface `src/dshowcapture.hpp`), win-capture `graphics-hook`, `inject-helper` and `get-graphics-offsets`, and the `mac-virtualcam` extension host, with owner, next review date and the decision IDs (D-08, D-09, D-10, D-14, D-19). Done when: list committed and every `plugins/` dir not in it is deleted.

### P7-04 Delete vendored deps
- [ ] **P7-04.1 Scan** — for each `deps/*` entry, `grep` consumers.
- [ ] **P7-04.2 Delete** unused ones (`blake2`, `json11`, `glad`, `w32-pthreads`, `libcaption` if ported), tagged `last-c-deps` first, per delete recipe; keep `libdshowcapture` (D-19: FFI, listed in P7-03.3) with its nested `capture-device-support` submodule (elgatosf; referenced by `deps/libdshowcapture/CMakeLists.txt`). Done when: no CMake/CI references remain, CI green on 3 OSes, tag listed in `docs/rust-rewrite/retired.md`.

### P7-05 Delete `shared/`
- [ ] **P7-05.1 Mapping table** — `bpm`, `file-updater`, `happy-eyeballs`, `ipc-util`, `media-playback`, `obs-d3d8-api`, `obs-hook-config`, `obs-inject-library`, `obs-scripting`, `obs-shared-memory-queue`, `obs-tiny-nv12-scale`, `opts-parser`, `properties-view`, `qt` → Rust crate or "still used by hook DLL".
- [ ] **P7-05.2 Delete** the migrated ones per delete recipe, tagged `last-c-shared` first; keep those used by the C game-capture hook DLL/inject helper (D-08). Done when: only the P7-05.1-mapped leftovers remain, CI green on 3 OSes, tag listed in `docs/rust-rewrite/retired.md`.

### P7-25 Delete the C test harness (added)
- [ ] **P7-25.1 Preconditions** — prove the four cmocka suites (`test_bitstream.c`, `test_darray.c`, `test_os_path.c`, `test_serializer.c`) and `test/test-input` are superseded: every exercised behaviour has a Rust test or a `test/conformance/` area. Done when: coverage table committed in `docs/tests/`.
- [ ] **P7-25.2 Tag** `last-c-test`.
- [ ] **P7-25.3 Delete** `test/` per the delete recipe (CMake/CI references in the same PR). Done when: build green on 3 OSes.

## B. Build system

### P7-06 Cargo-first build
- [ ] **P7-06.1 xtask** — commands `build`, `package`, `install`, `test`, `lint`, `fmt`, `deps` (fetch native libs). Done when: replaces documented CMake commands.
- [ ] **P7-06.2 C++ shims** — `build.rs` with `cc`/`cmake` for the leftovers from P7-03.3. Done when: builds on 3 OSes.
- [ ] **P7-06.3 Remove CMake** — `CMakeLists.txt`, `CMakePresets.json`, `cmake/`; keep `build-aux/` packaging. Only after P7-01…P7-05 and P7-25 are done and P7-06.2 builds the leftovers. Done when: CI uses xtask only.

### P7-07 Dependency management
- [ ] **P7-07.1 Pins** — FFmpeg, x264, CEF, SRT, RIST, libdatachannel/webrtc, Qt (if kept), with versions and SHA-256.
- [ ] **P7-07.2 Vendoring/mirrors** policy and offline build docs.

### P7-08 Cross-compilation and caches
- [ ] **P7-08.1 Windows x64/arm64**.
- [ ] **P7-08.2 macOS universal**.
- [ ] **P7-08.3 Linux x86_64/aarch64**.
- [ ] **P7-08.4 `sccache`/`nextest`** timings: CI < 30 min.

### P7-09 Reproducible and signed builds
- [ ] **P7-09.1 Determinism** — `SOURCE_DATE_EPOCH`, path remapping; two builds hash-equal.
- [ ] **P7-09.2 SBOM** — `cargo-cyclonedx` + native deps list.
- [ ] **P7-09.3 Provenance** — attestations and signature publishing.

## C. Compatibility layer and SDK

### P7-10 C ABI as opt-in compat feature
- [ ] **P7-10.1 Feature** — move C exports of `obs-ffi` behind `compat-c-abi` (default on until deprecation).
- [ ] **P7-10.2 Supported headers** — document API versions supported.
- [ ] **P7-10.3 Deprecation ADR** — record the D-05 timeline (announce with SDK 1.0, remove after 3 releases). Done when: ADR merged and timeline published with the SDK 1.0 notes.

### P7-11 Native plugin SDK 1.0
- [ ] **P7-11.1 Freeze review** — public API audit.
- [ ] **P7-11.2 Semver checks** — `cargo-semver-checks` in CI.
- [ ] **P7-11.3 Docs/template** published.
- [ ] **P7-11.4 Migration guide** — side-by-side C vs Rust for source, filter, encoder, output, service.

### P7-12 WASM sandbox research
- [ ] **P7-12.1 Spike** — load a WASM component source plugin (wasmtime).
- [ ] **P7-12.2 Overhead** — measure per-frame cost.
- [ ] **P7-12.3 ADR** — findings.

### P7-13 Scripting API stability
- [ ] **P7-13.1 Corpus** — run all documented script examples + community scripts.
- [ ] **P7-13.2 Differences** documented.

## D. Documentation and community

### P7-14 Rewrite docs
- [ ] **P7-14.1 API reference** — rustdoc + cbindgen headers; replaces `docs/sphinx`.
- [ ] **P7-14.2 Contributor docs** — README, CONTRIBUTING, CODESTYLE.md (Rust).
- [ ] **P7-14.3 Build docs** per OS.

### P7-15 Contributor onboarding
- [ ] **P7-15.1 Architecture overview** + crate map diagram.
- [ ] **P7-15.2 Good first issues** list.

### P7-16 Plugin author outreach
- [ ] **P7-16.1 Announcement**.
- [ ] **P7-16.2 Compat runner** — tool to test a third-party plugin on the Rust core.
- [ ] **P7-16.3 Support window**.

### P7-17 Translator workflow
- [ ] **P7-17.1 Verify** locale flow (Crowdin) unchanged.
- [ ] **P7-17.2 Docs update**.

## E. Hardening and release

### P7-18 Security audit
- [ ] **P7-18.1 Scope/auditor** selection.
- [ ] **P7-18.2 Threat model** — plugins, network inputs, updater, importers, OAuth, scripting.
- [ ] **P7-18.3 Fix findings**.
- [ ] **P7-18.4 `SECURITY.md`** update.

### P7-19 Fuzzing in CI
- [ ] **P7-19.1 Targets** — `obs-data` JSON, NAL/OBU parsers, effect parser, FLV/RTMP, MP4 input, importers, config, websocket requests, ffmpeg-mux pipe.
- [ ] **P7-19.2 OSS-Fuzz** integration.
- [ ] **P7-19.3 Triage** process and corpus.

### P7-20 Performance acceptance
- [ ] **P7-20.1 Setups** — 3 reference per OS.
- [ ] **P7-20.2 Runs** on `last-c-*` tags vs release candidate.
- [ ] **P7-20.3 Report** `final-benchmarks.md`.

### P7-21 Unsafe budget report
- [ ] **P7-21.1 `cargo-geiger`**.
- [ ] **P7-21.2 Justify/remove** outside allowed crates.

### P7-22 Release candidates
- [ ] **P7-22.1 RC1**.
- [ ] **P7-22.2 RC2** after triage.
- [ ] **P7-22.3 RC3** and go/no-go.

### P7-23 1.0 release
- [ ] **P7-23.1 Notes** and migration guide.
- [ ] **P7-23.2 Artifacts/packages** (Flatpak, winget, Homebrew, installers).
- [ ] **P7-23.3 Website** download pages.

### P7-24 Retrospective
- [ ] **P7-24.1 Feedback** from team and plugin authors.
- [ ] **P7-24.2 Write** `retrospective.md`.
- [ ] **P7-24.3 Close** `rust-rewrite` label.
