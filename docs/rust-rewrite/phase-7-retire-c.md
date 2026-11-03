# Phase 7: Retire C/C++ and finish

Goal: delete what is replaced, decide the fate of remaining native code, and ship a stable Rust-native release with an SDK.

## A. Removal

- [ ] **P7-01 Delete C libobs and backends** (M): `libobs/`, `libobs-opengl/`, `libobs-d3d11/`, `libobs-metal/`, `libobs-winrt/` once wgpu is the default for 2 releases and no regressions are open. Keep a git tag `last-c-libobs`. Done when: build and tests green on 3 OSes and `last-c-libobs` is listed in `docs/rust-rewrite/retired.md`.
- [ ] **P7-02 Delete C++ frontend** (M): `frontend/` (Qt6 C++), `.ui` forms moved or removed; keep a tag `last-cpp-frontend`. Done when: installers still contain locale and themes and full CI is green on 3 OSes.
- [ ] **P7-03 Delete C plugins** (M): remove every fully ported plugin directory, one plugin per commit with a `last-c-plugin-<name>` tag before it (P7-03.2); keep a list of intentional FFI leftovers (obs-browser CEF glue and its page-process helpers, DeckLink/AJA SDK glue, libdshowcapture, win-capture `graphics-hook`/`inject-helper`/`get-graphics-offsets`, mac-virtualcam extension host) in P7-03.3. Done when: only the documented FFI leftovers remain under `plugins/` and the tags are listed in `docs/rust-rewrite/retired.md`.
- [ ] **P7-04 Delete vendored deps** (S): `deps/` entries with no remaining users (`blake2`, `json11`, `glad`, `libcaption` if ported, `w32-pthreads`), tagged `last-c-deps`; keep `libdshowcapture` (D-19: FFI) with its nested `capture-device-support` submodule (elgatosf; sources and include dir referenced by `deps/libdshowcapture/CMakeLists.txt`). `libnsgif` is not in `deps/` — it is `libobs/graphics/libnsgif/` and goes with P7-01. Done when: no CMake/CI references remain and `last-c-deps` is listed in `docs/rust-rewrite/retired.md`.
- [ ] **P7-05 Delete `shared/`** (S): the 14 modules (`bpm`, `file-updater`, `happy-eyeballs`, `ipc-util`, `media-playback`, `obs-d3d8-api`, `obs-hook-config`, `obs-inject-library`, `obs-scripting`, `obs-shared-memory-queue`, `obs-tiny-nv12-scale`, `opts-parser`, `properties-view`, `qt`), tagged `last-c-shared`, after each has a Rust replacement or is mapped to a C leftover in P7-05.1; keep those still used by the C hook DLL/inject helper. Done when: only the P7-05.1-mapped leftovers remain and `last-c-shared` is listed in `docs/rust-rewrite/retired.md`.
- [ ] **P7-25 Delete the C test harness** (S) (added): `test/cmocka/` (`test_bitstream.c`, `test_darray.c`, `test_os_path.c`, `test_serializer.c` — superseded by the Rust crates' own tests and the `test/conformance/` suite), `test/test-input/` (C ABI test plugin extended by P0-15; its checks superseded by the Rust crates' own tests and the `test/conformance/` suite), `test/win/`, `test/osx/`. Runs in the same PR wave as P7-01…P7-05 because cmocka links the C libobs that P7-01 removes. Keep tag `last-c-test`. Done when: `test/` is gone, build green on 3 OSes, and `last-c-test` is listed in `docs/rust-rewrite/retired.md`.

## B. Build system

- [ ] **P7-06 Cargo-first build** (L): replace the top-level `CMakeLists.txt`, `CMakePresets.json`, `cmake/` with `cargo xtask` (`build`, `package`, `install`, `test`, `lint`, `fmt`, `deps`) plus `build.rs` shims (`cc`/`cmake` crates) for the remaining C++ leftovers from P7-03.3. Done when: CI builds, tests and packages via xtask only and `cmake/` is deleted.
- [ ] **P7-07 Dependency management** (M): pin all native deps (FFmpeg, x264, CEF, Qt if kept, SRT, RIST, libdatachannel) with a lock file and a reproducible build recipe; vendoring policy. Done when: an offline build from the pinned manifest succeeds on 3 OSes.
- [ ] **P7-08 Cross-compilation and CI caches** (M): Windows (x64, arm64), macOS (universal), Linux (x86_64, aarch64); `sccache`, `cargo-nextest`. Done when: all target matrices build in CI and end-to-end CI is under 30 minutes.
- [ ] **P7-09 Reproducible and signed builds** (M): `SOURCE_DATE_EPOCH`, SBOM (`cargo-cyclonedx`), provenance attestations, signatures. Done when: two clean builds are hash-equal and SBOM, attestations and signatures ship with the RC.

## C. Compatibility layer and SDK

- [ ] **P7-10 Make C ABI an opt-in compat feature** (M): move `obs-ffi`'s C exports behind a `compat-c-abi` cargo feature (default on until deprecation) for loading legacy C plugins; document supported `LIBOBS_API_VER`s and the deprecation timeline per D-05 (announce with SDK 1.0, remove after 3 releases; ADR in P7-10.3). Done when: a legacy C plugin loads with the feature on, and the supported-header list plus timeline are published.
- [ ] **P7-11 Native plugin SDK 1.0** (L): freeze `obs-plugin-api`; semver policy; `cargo-semver-checks` in CI; API docs on docs.rs-like hosting; `cargo generate` template; migration guide from C plugins with side-by-side examples for each type. Done when: `obs-plugin-api` is frozen and semver-checked in CI, the template generates a working plugin, and the migration guide is published.
- [ ] **P7-12 Optional WASM plugin sandbox (research)** (L): spike on loading WASM components for untrusted plugins; write findings ADR; no commitment. Done when: the findings ADR (adopt/defer/drop) is committed.
- [ ] **P7-13 Scripting API stability** (S): Lua/Python bindings remain source-compatible; run the full script example corpus. Done when: the corpus runs green against the Rust bindings and any differences are documented.

## D. Documentation and community

- [ ] **P7-14 Rewrite docs** (M): `docs/sphinx` C API reference -> generated from rustdoc + cbindgen headers; README, CONTRIBUTING, CODESTYLE (Rust), build instructions per OS. Done when: the rustdoc + cbindgen reference replaces `docs/sphinx` (with a redirect) and a clean clone follows the per-OS build docs.
- [ ] **P7-15 Contributor onboarding** (S): "good first issue" list, architecture overview, crate map diagram. Done when: the architecture overview and good-first-issue list are published.
- [ ] **P7-16 Plugin author outreach** (S): announcement, migration support window, compatibility test runner for plugin authors. Done when: the announcement is out, the runner passes on three known third-party plugins, and the support window is published.
- [ ] **P7-17 Translator workflow** (S): confirm `.ini` locale flow unchanged (Crowdin). Done when: a real Crowdin locale update flows to `.ini` and ships with the format unchanged.

## E. Hardening and release

- [ ] **P7-18 Security audit** (L): third-party review of `unsafe` blocks, network parsing (RTMP, WHIP, websocket, updater, importers), OAuth token handling. Fix findings. `SECURITY.md` update. Done when: the audit report is filed, findings fixed or accepted, and `SECURITY.md` is current.
- [ ] **P7-19 Fuzzing in CI** (M): continuous fuzzing (OSS-Fuzz) for parsers: `obs-data` JSON, NAL/OBU, effect parser, FLV/RTMP, MP4 mux input, importers, config, websocket requests. Done when: every P7-19.1 target fuzzes in CI (OSS-Fuzz) with a documented triage process and committed corpus.
- [ ] **P7-20 Performance acceptance** (M): final benchmark report vs C baseline (CPU, RAM, startup, GPU time, dropped frames) for 3 reference setups per OS; document in `docs/rust-rewrite/final-benchmarks.md`. Done when: the report is published and the release passes the acceptance criteria.
- [ ] **P7-21 Unsafe budget report** (S): `cargo-geiger` summary; target: `unsafe` only in `*-sys`, `ffi`, platform crates; each block justified. Done when: the geiger summary is committed and every `unsafe` block outside the allowed crates is justified or removed.
- [ ] **P7-22 Release candidate programme** (M): RC1-RC3, with regression triage and an automated crash-rate dashboard. Done when: RC3 is cut and the go/no-go decision is recorded with the crash-rate data.
- [ ] **P7-23 1.0 release** (M): version number, release notes, migration guide, updated website download pages, Flatpak/winget/Homebrew packages. Done when: 1.0 artifacts are published on all channels with notes and migration guide live.
- [ ] **P7-24 Retrospective** (S): record what worked and what did not in `docs/rust-rewrite/retrospective.md`; close the `rust-rewrite` label. Done when: `retrospective.md` is committed and the label is closed.

Exit criteria: no C/C++ of our own remains except documented FFI shims; `cargo xtask package` produces all release artifacts; SDK 1.0 published; security review closed.
