# Epics & Stories — obs-rust (Rust rewrite)

The full plan to replace OBS Studio with Rust: a strangler-fig migration in which Rust and C
coexist in one build until [Phase 7](rust-rewrite/phase-7-retire-c.md) retires the C sources
([ADR 0001](adr/0001-strangler-migration.md), [plan](rust-rewrite-plan.md)). Rough effort
(plan §7): 3–4 years for a team of ~6; every phase ships a working OBS.

This index tracks all milestones. Each epic lives in its own file under [epics/](epics/) —
one file per epic, story tables with a `Status` column (values per the `po` skill:
`open`, `in progress`, `partial`, `done`). Authoritative per-subtask state:
[rust-rewrite/subtasks/](rust-rewrite/subtasks/). Open decisions:
[decisions-open.md](decisions-open.md). Principles that override convenience:
[tenets.md](tenets.md). How it fits together: [architecture.md](architecture.md).

## Milestones

- **M0 — Phase 0: foundation** → delivered (ADR set, workspace, CI, golden framework); open parts tracked below and in M3
- **M1 — Phase 1: code & recipe** → plan wave 3 (WP-30…WP-38) — remaining leaf modules + the cbindgen toolchain
- **M2 — Phase 1: integration & review** → plan wave 4 (WP-39…WP-46) — C-ref dumps, ABI plugin, benchmarks, corpus, recipe batch, nightly, phase-1 review; E21 = spec prep (D5/D6)
- **M3 — Phase 0 close-out & future ADRs** → plan wave 5 (WP-47…WP-52) — parallel track, no Phase 1 code dependency
- **M4 — Phase 2: media pipeline** → 4–6 months — formats, IO threads, audio, NAL parsers
- **M5 — Phase 3: graphics** → 6–9 months — effect language, `gs_*`, wgpu backend
- **M6 — Phase 4: core object model** → 9–12 months — sources/scenes/encoders/outputs, full `obs.h` ABI surface
- **M7 — Phase 5: plugins** → 12–18 months — the 41 plugin modules in risk order
- **M8 — Phase 6: frontend** → 12–18 months, parallel with M7 — cxx-qt port of `frontend/`
- **M9 — Phase 7: retire C/C++** → 3 months — C source removal, CMake retirement, SDK 1.0, first all-Rust release

## Epic index

| Epic | Goal | Stories |
|---|---|---|
| [E58](epics/epic-58-phase-0-foundation.md) | Phase 0 foundation **(M0)** | 21 |
| [E1](epics/epic-01-filesystem-platform-paths-wp-30-p1-07.md) | Filesystem & platform paths (WP-30, P1-07) | 4 |
| [E2](epics/epic-02-cbindgen-ffi-toolchain-wp-31-p1-29.md) | cbindgen FFI toolchain (WP-31, P1-29) | 1 |
| [E3](epics/epic-03-remaining-util-modules-wp-32-p1-14-1-2.md) | Remaining `util` modules (WP-32, P1-14.1/.2/.4 + P1-03.2 + P1-04.2 + P1-02.4) | 5 |
| [E4](epics/epic-04-typed-signals-wp-33-p1-23.md) | Typed signals (WP-33, P1-23) | 3 |
| [E5](epics/epic-05-shared-helpers-wp-34-p1-33-1-5.md) | Shared helpers (WP-34, P1-33.1–.5) | 5 |
| [E6](epics/epic-06-simd-wp-35-p1-19-1-3.md) | SIMD (WP-35, P1-19.1–.3) | 3 |
| [E7](epics/epic-07-signal-threading-refcount-hardening-wp.md) | Signal threading & refcount hardening (WP-36 + WP-37) | 5 |
| [E8](epics/epic-08-windows-parity-wp-38-p1-05-3-review-fi.md) | Windows parity (WP-38, P1-05.3 + review finding 5) | 3 |
| [E9](epics/epic-09-c-reference-dump-tool-wp-39-p0-14.md) | C-reference dump tool (WP-39, P0-14) | 2 |
| [E10](epics/epic-10-abi-plugin-harness-wp-40-p0-15-p1-27-5.md) | ABI plugin harness (WP-40, P0-15 + P1-27.5) | 1 |
| [E11](epics/epic-11-benchmarks-wp-41-p0-17.md) | Benchmarks (WP-41, P0-17) | 1 |
| [E12](epics/epic-12-user-corpus-wp-42-p0-18.md) | User corpus (WP-42, P0-18) | 2 |
| [E13](epics/epic-13-recipe-batch-nightly-dependency-scan-w.md) | Recipe batch, nightly & dependency scan (WP-43 + WP-44 + WP-45) | 3 |
| [E14](epics/epic-14-phase-1-review-wp-46-p1-32.md) | Phase 1 review (WP-46, P1-32) | 1 |
| [E21](epics/epic-21-spec-prep.md) | Phase 2+ spec prep (D5, decided 2026-11-23) | 5 |
| [E15](epics/epic-15-graphics-backend-adr-0003-wp-47-p0-03.md) | Graphics backend ADR 0003 (WP-47, P0-03) | 2 |
| [E16](epics/epic-16-gui-toolkit-adr-0004-wp-48-p0-04.md) | GUI toolkit ADR 0004 (WP-48, P0-04) | 5 |
| [E17](epics/epic-17-build-documentation-wp-49-p0-12.md) | Build documentation (WP-49, P0-12) | 3 |
| [E18](epics/epic-18-ci-repo-hygiene-wp-50.md) | CI & repo hygiene (WP-50) | 3 |
| [E19](epics/epic-19-headless-smoke-wp-51-p0-16.md) | Headless smoke (WP-51, P0-16) | 2 |
| [E20](epics/epic-20-phase-0-close-out-wp-52.md) | Phase 0 close-out (WP-52) | 2 |
| [E22](epics/epic-22-formats-and-conversion.md) | Formats and conversion | 7 |
| [E23](epics/epic-23-io-threads.md) | IO threads | 5 |
| [E24](epics/epic-24-audio-mixing-and-controls.md) | Audio mixing and controls | 4 |
| [E25](epics/epic-25-bitstream-parsers.md) | Bitstream parsers | 5 |
| [E26](epics/epic-26-packet-queues-and-delay.md) | Packet queues and delay | 3 |
| [E27](epics/epic-27-integration.md) | Integration | 5 |
| [E28](epics/epic-28-effect-language.md) | Effect language | 6 |
| [E29](epics/epic-29-gs-api.md) | `gs_**` API | 6 |
| [E30](epics/epic-30-backends.md) | Backends | 11 |
| [E31](epics/epic-31-built-in-effects-and-render-loop.md) | Built-in effects and render loop | 4 |
| [E32](epics/epic-32-cleanup.md) | Cleanup | 5 |
| [E33](epics/epic-33-design.md) | Design | 4 |
| [E34](epics/epic-34-sources.md) | Sources | 11 |
| [E35](epics/epic-35-scenes.md) | Scenes | 5 |
| [E36](epics/epic-36-encoders-outputs-services.md) | Encoders, outputs, services | 7 |
| [E37](epics/epic-37-hotkeys-modules-display.md) | Hotkeys, modules, display | 6 |
| [E38](epics/epic-38-abi-surface-and-verification.md) | ABI surface and verification | 9 |
| [E39](epics/epic-39-pure-logic.md) | Pure logic | 10 |
| [E40](epics/epic-40-network-and-muxing.md) | Network and muxing | 12 |
| [E41](epics/epic-41-platform-capture-and-audio.md) | Platform capture and audio | 16 |
| [E42](epics/epic-42-hardware-encoders.md) | Hardware encoders | 3 |
| [E43](epics/epic-43-vendor-sdk-cards-and-tools.md) | Vendor SDK cards and tools | 3 |
| [E44](epics/epic-44-browser.md) | Browser | 1 |
| [E45](epics/epic-45-wrap-up.md) | Wrap-up | 6 |
| [E46](epics/epic-46-application-shell.md) | Application shell | 6 |
| [E47](epics/epic-47-core-ui-components.md) | Core UI components | 12 |
| [E48](epics/epic-48-settings-and-wizards.md) | Settings and wizards | 5 |
| [E49](epics/epic-49-profiles-collections-importers.md) | Profiles, collections, importers | 5 |
| [E50](epics/epic-50-services-auth-updater.md) | Services, auth, updater | 7 |
| [E51](epics/epic-51-frontend-api-and-tools.md) | Frontend API and tools | 6 |
| [E52](epics/epic-52-packaging-and-release.md) | Packaging and release | 9 |
| [E53](epics/epic-53-removal.md) | Removal | 6 |
| [E54](epics/epic-54-build-system.md) | Build system | 4 |
| [E55](epics/epic-55-compatibility-layer-and-sdk.md) | Compatibility layer and SDK | 4 |
| [E56](epics/epic-56-documentation-and-community.md) | Documentation and community | 4 |
| [E57](epics/epic-57-hardening-and-release.md) | Hardening and release | 7 |
## Cross-cutting

- One WP = one branch + one PR; subtask boxes tick in the same PR that finishes the work
  ([PLAN-2026-11-21.md](rust-rewrite/PLAN-2026-11-21.md) §3).
- Every code PR carries the per-WP verification baseline (plan §6): fmt, clippy `-D warnings`,
  nextest, cargo deny/audit, [run.sh](https://github.com/ripmav/obs-rust/blob/main/test/conformance/run.sh)
  `check` per touched area, `--features c-swap` lint for `obs-ffi` changes, test matrix in
  [docs/tests/](tests/), ODD entry in
  [odd/tasks/rust-rewrite-phase-1.md](https://github.com/ripmav/obs-rust/blob/main/odd/tasks/rust-rewrite-phase-1.md).
- The C reference is pinned at obs-studio **33.0.1** for parity evidence (T11); CI conformance
  runs per [rust.yml](https://github.com/ripmav/obs-rust/blob/main/.github/workflows/rust.yml).
- `unsafe` in new code follows [ADR 0005](adr/0005-code-policy.md) Amendment 1: only the named
  exception sets, every block carries `// SAFETY:` (lint-enforced, T5).
- CMake `OBS_ENABLE_RUST` integration: in-repo patch file, applied by CI/nightly (D3, decided
  2026-11-23 — see E1S4).
- Real-time rule: no allocation, no blocking lock, no syscall on the audio/video hot path (T6).

## Out of scope

- Vendor SDKs and native libraries (FFmpeg, CEF, NVENC/AMF/QSV/VideoToolbox, libfdk, libvlc, …):
  permanent FFI inputs — the glue is rewritten, the libraries are not (T9, plan §6).
- Deleting `deps/blake2` / `deps/json11` — stays P7-04 (E53); E13S3 only records the deferral.

## Open questions

See [decisions-open.md](decisions-open.md).
