# Phase 7: Retire C/C++ and finish

Goal: delete what is replaced, decide the fate of remaining native code, and ship a stable Rust-native release with an SDK.

## A. Removal

- [ ] **P7-01 Delete C libobs and backends** (M): `libobs/`, `libobs-opengl/`, `libobs-d3d11/`, `libobs-metal/`, `libobs-winrt/` once wgpu is the default for 2 releases and no regressions are open. Keep a git tag `last-c-libobs`.
- [ ] **P7-02 Delete C++ frontend** (M): `frontend/` (Qt6 C++), `.ui` forms moved or removed; keep a tag `last-cpp-frontend`.
- [ ] **P7-03 Delete C plugins** (M): remove every ported plugin directory; keep list of intentional FFI leftovers (obs-browser CEF glue, DeckLink/AJA SDK glue, libdshowcapture, graphics-hook DLL).
- [ ] **P7-04 Delete vendored deps** (S): `deps/` entries with no remaining users (`blake2`, `json11`, `glad`, `libcaption` if ported, `w32-pthreads`, `libnsgif`).
- [ ] **P7-05 Delete `shared/`** (S): `shared/obs-scripting`, `obs-frontend-api`-related shared helpers, `shared/bpm`, `shared/media-playback`, `shared/ipc-util`, `shared/happy-eyeballs`, `shared/properties-view`, `shared/qt` — after each is confirmed to have a Rust replacement.

## B. Build system

- [ ] **P7-06 Cargo-first build** (L): replace the top-level `CMakeLists.txt`, `CMakePresets.json`, `cmake/` with `cargo xtask` (`build`, `package`, `install`, `test`, `lint`) plus a minimal CMake wrapper only for remaining C++ crates (`cc`/`cmake` crates in `build.rs`).
- [ ] **P7-07 Dependency management** (M): pin all native deps (FFmpeg, x264, CEF, Qt if kept, SRT, RIST, libdatachannel, curl replacements) with a lock file and a reproducible build recipe; vendoring policy.
- [ ] **P7-08 Cross-compilation and CI caches** (M): Windows (x64, arm64), macOS (universal), Linux (x86_64, aarch64); `sccache`, `cargo-nextest`.
- [ ] **P7-09 Reproducible and signed builds** (M): `SOURCE_DATE_EPOCH`, SBOM (`cargo-cyclonedx`), provenance attestations, signatures.

## C. Compatibility layer and SDK

- [ ] **P7-10 Make C ABI an opt-in compat feature** (M): `obs-compat-c` crate (or cargo feature) for loading legacy C plugins; document supported header versions and deprecation timeline (announce, 2 releases, then remove? decide by ADR).
- [ ] **P7-11 Native plugin SDK 1.0** (L): freeze `obs-plugin-api`; semver policy; `cargo-semver-checks` in CI; API docs on docs.rs-like hosting; `cargo generate` template; migration guide from C plugins with side-by-side examples for each type.
- [ ] **P7-12 Optional WASM plugin sandbox (research)** (L): spike on loading WASM components for untrusted plugins; write findings ADR; no commitment.
- [ ] **P7-13 Scripting API stability** (S): Lua/Python bindings remain source-compatible; run the full script example corpus.

## D. Documentation and community

- [ ] **P7-14 Rewrite docs** (M): `docs/sphinx` C API reference -> generated from rustdoc + cbindgen headers; README, CONTRIBUTING, CODESTYLE (Rust), build instructions per OS.
- [ ] **P7-15 Contributor onboarding** (S): "good first issue" list, architecture overview, crate map diagram.
- [ ] **P7-16 Plugin author outreach** (S): announcement, migration support window, compatibility test runner for plugin authors.
- [ ] **P7-17 Translator workflow** (S): confirm `.ini` locale flow unchanged (Crowdin).

## E. Hardening and release

- [ ] **P7-18 Security audit** (L): third-party review of `unsafe` blocks, network parsing (RTMP, WHIP, websocket, updater, importers), OAuth token handling. Fix findings. `SECURITY.md` update.
- [ ] **P7-19 Fuzzing in CI** (M): continuous fuzzing (OSS-Fuzz) for parsers: `obs-data` JSON, NAL/OBU, effect parser, FLV/RTMP, MP4 mux input, importers, config, websocket requests.
- [ ] **P7-20 Performance acceptance** (M): final benchmark report vs C baseline (CPU, RAM, startup, GPU time, dropped frames) for 3 reference setups per OS; document in `docs/rust-rewrite/final-benchmarks.md`.
- [ ] **P7-21 Unsafe budget report** (S): `cargo-geiger` summary; target: `unsafe` only in `*-sys`, `ffi`, platform crates; each block justified.
- [ ] **P7-22 Release candidate programme** (M): RC1-RC3, with regression triage and an automated crash-rate dashboard.
- [ ] **P7-23 1.0 release** (M): version number, release notes, migration guide, updated website download pages, Flatpak/winget/Homebrew packages.
- [ ] **P7-24 Retrospective** (S): record what worked and what did not in `docs/rust-rewrite/retrospective.md`; close the `rust-rewrite` label.

Exit criteria: no C/C++ of our own remains except documented FFI shims; `cargo xtask package` produces all release artifacts; SDK 1.0 published; security review closed.
