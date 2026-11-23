# Epics & Stories — obs-rust (Rust rewrite)

The OBS Studio Rust rewrite replaces `libobs` C modules one by one with Rust crates under `crates/`
while keeping the C ABI stable (strangler pattern, [ADR 0001](adr/0001-strangler-migration.md)).
This file tracks the **open, not-yet-started** milestones of the continuation plan
([PLAN-2026-11-21.md](rust-rewrite/PLAN-2026-11-21.md), waves 3–5, WP-30…WP-52).
Already delivered or in-flight work (WP-27…WP-29) is not tracked here. Authoritative per-subtask
state: [phase-1-subtasks.md](rust-rewrite/subtasks/phase-1-subtasks.md) and
[phase-0-subtasks.md](rust-rewrite/subtasks/phase-0-subtasks.md). Delivery unit is one WP per PR
(WP numbers below are provisional, assigned in landing order). Open decisions:
[decisions-open.md](decisions-open.md).

## Milestones

- **M1 — Phase 1: code & recipe** → plan wave 3 (WP-30…WP-38) — remaining leaf modules + the cbindgen toolchain
- **M2 — Phase 1: integration & review** → plan wave 4 (WP-39…WP-46) — C-ref dumps, ABI plugin, benchmarks, corpus, recipe batch, nightly, phase-1 review
- **M3 — Phase 0 close-out & future ADRs** → plan wave 5 (WP-47…WP-52) — parallel track, no Phase 1 code dependency

---

## Milestone 1 — Phase 1: code & recipe

### Epic E1 — Filesystem & platform paths (WP-30, P1-07)
**Why:** the next unported `libobs/util` area in roadmap order; unblocks CMake swap-set growth.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E1S1** Open/rename/mkdir (P1-07.1) | port `os_fopen`, `os_renamedir`, `os_mkdir`, `os_remove_path`, `os_rename_path`, `os_stat` semantics | [platform/](https://github.com/ripmav/obs-rust/tree/main/test/conformance/scripts/platform) conformance vectors pass against the Rust build; `apple/` files handled per P1-14.1 inventory | open |
| **E1S2** Dir iteration & glob (P1-07.2) | `os_opendir/readdir/closedir`, `os_glob`/`os_globfree` per S-023 §5.6 | sorted `ls`/`glob` results byte-equal C (vectors exist in `platform/`); regression test proves the port **copies the caller's path** (S-023 D-5 — the reference keeps the pointer, a UAF) | open |
| **E1S3** Config/data paths (P1-07.3) | `os_get_config_path`, `os_get_program_data_path`, `os_get_abs_path(_ptr)`, `os_get_free_disk_space` + Flatpak portal/dbus helpers | Flatpak path test passes; Q-11 rule tested (`os_get_config_path_ptr(NULL/"")` ends with `/`, the buffer form does not); `utilrest/` dbus vectors | open |
| **E1S4** CMake swap-set extension (P1-07.4, code half) | `OBS_ENABLE_RUST=ON` excludes the ported platform files | `ctest` green with the option ON, with the option provided as the in-repo patch file `test/conformance/obs-enable-rust.patch` (D3, decided 2026-11-23); cbindgen half is owned by E2S1 | open |

> D3 decided 2026-11-23 (Option 1): the `OBS_ENABLE_RUST` CMake change ships as a patch file inside
> this repo (`test/conformance/obs-enable-rust.patch`), applied by the CI/nightly jobs — the rewrite
> owns its own build integration. The patch is regenerated against the 33.0.1 pin at WP-30 start; an
> optional fork PR against `obsproject/obs-studio` stays possible later.

### Epic E2 — cbindgen FFI toolchain (WP-31, P1-29)
**Why:** every "Apply recipe" subtask's cbindgen diff is blocked on this; the single biggest unlock in M1.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E2S1** cbindgen config + diff in CI | per-crate cbindgen config and a diff script against the committed `libobs/**/*.h` headers, wired as a CI step | diff is empty on `main`; a deliberate header drift reds the CI step; unblocks the cbindgen half of P1-01.3, P1-02.4, P1-03.3, P1-04.3, P1-05.4, P1-08.5, P1-09.5, P1-10.4, P1-11.4, P1-14.5, P1-15.4, P1-16.5, P1-17.4, P1-18.4, P1-20.3, P1-21.4, P1-22.4, P1-25.6, P1-27.6, P1-28.4 | open |

### Epic E3 — Remaining `util` modules (WP-32, P1-14.1/.2/.4 + P1-03.2 + P1-04.2 + P1-02.4)
**Why:** closes the porting gaps the audit found inside "done" areas (darray/deque C helpers) and finishes the serializer family.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E3S1** Serializers & bitstream (P1-14.2) | port `bitstream`, `array-serializer`, `file-serializer`, `buffered-file-serializer` per S-010 | [test_bitstream.c](https://github.com/ripmav/obs-rust/blob/main/test/cmocka/test_bitstream.c) + [test_serializer.c](https://github.com/ripmav/obs-rust/blob/main/test/cmocka/test_serializer.c) pass unchanged against the Rust build; S-010 Q-3 (8-bit `pos` wrap) and Q-8 (three SEEK_END conventions) have dedicated tests | open |
| **E3S2** darray C helpers (P1-03.2) | the functions behind `da_push_back`/`da_insert`/`da_erase`/`da_move_item` | [test_darray.c](https://github.com/ripmav/obs-rust/blob/main/test/cmocka/test_darray.c) (31 cases) passes unchanged; S-005 Q-A2: `da_find` `memcmp`s the whole element incl. padding — mirrored byte-wise with a test | open |
| **E3S3** Deque layout sharing (P1-04.2) | C inline code driving a Rust-owned `struct deque` | a C test builds a Rust-owned deque, drives it with the C inline functions, and gets the same dump as the [deque/](https://github.com/ripmav/obs-rust/tree/main/test/conformance/scripts/deque) vectors | open |
| **E3S4** `util` inventory & curl note (P1-14.1, P1-14.4) | keep/port/drop decision for every remaining `libobs/util` file; curl stays FFI until Phase 6 | inventory table merged (every P1-14 file has a decision); curl FFI note recorded in [ADR 0005](adr/0005-code-policy.md) | open |
| **E3S5** dstr fuzz target (P1-02.4, code half) | `cargo-fuzz` target applying random op sequences to Rust and C builds | fuzz target runs clean; recipe half (CMake exclusion) is ticked by E13S1 | open |

### Epic E4 — Typed signals (WP-33, P1-23)
**Why:** the safe Rust API over the C signal layer — the point of having `obs-signal` at all.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E4S1** `Signal<A>` + `Connection` design | design addendum (ADR) for the typed API | ADR addendum accepted; lock structure and S-004 Q-2 semantics documented | open |
| **E4S2** Calldata bridge | typed args to/from `calldata` (S-002 byte layout) | round-trip test passes (Rust → calldata → Rust, all type families) | open |
| **E4S3** Docs & tests | crate-level docs and usage tests for `crates/obs-signal/` | docs + tests merged; example compiles against the public API | open |

### Epic E5 — Shared helpers (WP-34, P1-33.1–.5)
**Why:** the `shared/` helpers are leaf code with committed conformance scripts — cheap coverage wins.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E5S1** `obs-bpm` (P1-33.1) | port `shared/bpm` per S-024 M8 into `crates/obs-bpm` | `bpm` scripts in [helpers/](https://github.com/ripmav/obs-rust/tree/main/test/conformance/scripts/helpers) pass against C; S-024 R56/R57 pitfalls tested (HEVC w/o start code, unknown codec) | open |
| **E5S2** `obs-file-updater` (P1-33.2) | port `shared/file-updater` per S-024 M10 | `fu-*` scripts pass; fake-HTTP/in-memory-filesystem driven | open |
| **E5S3** `obs-netutil` happy-eyeballs (P1-33.3) | port `shared/happy-eyeballs` per S-024 M9 (ordering, resolver hints, constants) | `he-*` scripts pass; the socket race stays in C until a spec exists (documented) | open |
| **E5S4** `obs-inject-library` (P1-33.4) | port the thin `LoadLibrary`/`dlopen` helper, no spec | unit tests cover symbol resolution + failure paths; C file stays linked until win-capture (P5-31.6/.7) — exclusion only then | open |
| **E5S5** `opts-parser` (P1-33.5) | port `shared/opts-parser` per S-024 M1 into `crates/obs-util-small` | `opts-*` scripts pass | open |

### Epic E6 — SIMD (WP-35, P1-19.1–.3)
**Why:** `graphics` math performance parity before Phase 3.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E6S1** Intrinsic inventory (P1-19.1) | list of the intrinsics in use | list merged: `sse-intrin.h` + simde use in `libobs/graphics/*.h` covered | open |
| **E6S2** Implementation (P1-19.2) | `std::arch` SSE2/AVX2 with runtime check + NEON | scalar fallback results equal, on x86_64 and aarch64 | open |
| **E6S3** Benchmark (P1-19.3) | measured parity vs C | "not slower than C" on x86_64 **and** aarch64, using the E11S1 harness/baselines | open |

### Epic E7 — Signal threading & refcount hardening (WP-36 + WP-37)
**Why:** the audit's concurrency findings (P1-22.3, P1-26.1–.3) plus the two facade leftovers.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E7S1** Signal thread tests (P1-22.3) | `loom` coverage of connect-during-emit | no deadlock under `loom` for connect-from-other-thread during emit; lock structure kept (per-signal recursive, handler, global-callbacks); any deviation documented (S-004 Q-2) | open |
| **E7S2** Refcount semantics (P1-26.1) | `get_obj`/setter ref rules observable | refcount through the debug API equals C on the [obsdata/](https://github.com/ripmav/obs-rust/tree/main/test/conformance/scripts/obsdata) `refs`/`a_refs` vectors (S-014 §5.1) | open |
| **E7S3** Miri/ASan sweep (P1-26.2/.3) | the pure crates clean under the extended Miri scope + an ASan job | both clean; findings fixed in the same PR | open |
| **E7S4** `log` facade (P1-13.3) | Rust `log` records forwarded to `blog` | level-mapping table test passes | open |
| **E7S5** Source profiler (P1-12.3) | RAII guard for Rust users + port of `source-profiler.c` (S-055 U3) | goldens pass; S-055 Q-7 (history lags one frame, zero tick = no data) and Q-2 (`fill_result` false + zeroed struct) tested | open |

### Epic E8 — Windows parity (WP-38, P1-05.3 + review finding 5)
**Why:** the "3-OS" Done-when of the threading/pipe/dl tasks cannot tick without it.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E8S1** `threading-windows` port (P1-05.3) | events/semaphores as `HANDLE`s (`CreateEvent`/`CreateSemaphore`, `WaitForSingleObject`), no SRW locks | behaviour matches `threading-windows.c`; documented | open |
| **E8S2** Windows `dl`/`pipe`/`os_proc` variants (P1-11 3-OS) | the POSIX-only modules gain Windows implementations | Windows CI green for the three modules | open |
| **E8S3** Windows CI green (P1-05.3) | the whole threading area passes on the Windows runner | `test (windows-latest)` green incl. the new modules | open |

---

## Milestone 2 — Phase 1: integration & review

### Epic E9 — C-reference dump tool (WP-39, P0-14)
**Why:** goldens for the math/obsdata areas are blocked on the dump target.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E9S1** `obs-ref-dump` target | the dump binary with math + data commands | target builds; math and data dumps run against the C build | open |
| **E9S2** Golden generation in CI | `test/golden/` generation + regenerate-and-diff job | CI job reds on a deliberate golden change; unblocks P1-15.3 (10k seeded cases, 1e-6 relative) and P1-25.5 (corpus compare) | open |

### Epic E10 — ABI plugin harness (WP-40, P0-15 + P1-27.5)
**Why:** the reopened P1-27.2/.3 Done-when needs this harness to exist.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E10S1** Plugin ABI test harness | extend [test-input](https://github.com/ripmav/obs-rust/tree/main/test/test-input) (dummy encoder/output/service) + `test/abi-host` C driver + trace goldens + CI wiring | harness CI job green; P1-27.2, P1-27.3 and P1-27.5 become tickable | open |

### Epic E11 — Benchmarks (WP-41, P0-17)
**Why:** the "≥ C speed" Done-when clauses (P1-16.4, P1-19.3, P1-32.1) need baselines and a gate.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E11S1** Criterion + C baseline + gate | Criterion harness, C baseline runner, `benchmarks.md` with machine spec, 2% regression gate | gate wired in CI; first baseline recorded for the ported areas | open |

### Epic E12 — User corpus (WP-42, P0-18)
**Why:** byte-identical round trips on real-world config/script data (P1-09.4, P1-24.2/.3, P1-25.5).

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E12S1** Anonymiser + loader + index (code half) | the corpus tooling | anonymiser + load check + index pass on a synthetic fixture set | open |
| **E12S2** ≥20 corpus collections (D4, decided 2026-11-23) | the corpus populated with real-world-shaped data | ≥20 collections ingested, composed of licence-compliant public material (OBS issue-tracker configs, gists, …) + synthetic fixtures; load check green; unblocks the corpus round-trip subtasks | open |

> D4 decided 2026-11-23: the corpus is a **mix of licence-compliant public material and synthetic
> fixtures** (no private user collection). The code half (E12S1) runs on synthetic fixtures
> immediately.

### Epic E13 — Recipe batch, nightly & dependency scan (WP-43 + WP-44 + WP-45)
**Why:** the batch that ticks everything the toolchain finally makes verifiable — and the soak that flips the default.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E13S1** Recipe batch ticks (WP-43) | after E2/E9/E10/E11/E12: every area through `run.sh swap`, tick all now-verifiable subtasks | ticks only for what actually passed (the audit's lesson); covers the WP-31 unblock list + P1-09.4, P1-15.3/.4, P1-16.4, P1-20.3, P1-21.3, P1-24.2/.3, P1-25.5, P1-28.2/.3 and P1-06.3 (jitter p99 + recipe) | open |
| **E13S2** Nightly + soak + flip (WP-44, P1-30) | nightly builds with `OBS_ENABLE_RUST=ON`, two-week soak, then default flip + docs | nightly job green ON + OFF (`ctest`); soak period clean; default flipped in docs/CI (needs D3) | open |
| **E13S3** Dependency usage scan (WP-45, P1-31) | usage scan for `deps/blake2` / `deps/json11` + deferral record | scan merged; deferral record written (deletion stays P7-04) | open |

### Epic E14 — Phase 1 review (WP-46, P1-32)
**Why:** the phase gate: proof that Phase 1 is actually done.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E14S1** Phase 1 done-when | benchmarks vs baseline, goldens/ABI on 3 OSes, parent ticks in the phase-1 file | all P1 parent checkboxes ticked with passing evidence on all three OS CI runners | open |

---

## Milestone 3 — Phase 0 close-out & future ADRs

### Epic E15 — Graphics backend ADR 0003 (WP-47, P0-03)
**Why:** blocks Phase 3 (graphics).

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E15S1** wgpu prototypes (P0-03.1–.3) | Linux, Windows and macOS prototypes + shared-texture import spike | all three prototypes run; spike result recorded | open |
| **E15S2** Write ADR 0003 (P0-03.4) | the graphics-backend decision recorded | ADR accepted | open |

### Epic E16 — GUI toolkit ADR 0004 (WP-48, P0-04)
**Why:** blocks Phase 6 (frontend).

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E16S1** Evaluation criteria (P0-04.1) | the scored criteria list | criteria merged | open |
| **E16S2** cxx-qt prototype (P0-04.2) | throwaway `proto/gui-cxxqt/` dock (scene list) with drag-reorder, theme variables, preview embedding | runs on Linux and Windows; risk list written | open |
| **E16S3** Slint & egui prototypes (P0-04.3) | `proto/gui-slint/` + `proto/gui-egui/`, same dock | preview-embedding result recorded for each | open |
| **E16S4** Score & migration path (P0-04.4) | the comparison and migration plan | scoring merged | open |
| **E16S5** Write ADR 0004 (P0-04.5) | the toolkit decision + Phase 6 doc update | ADR accepted and Phase 6 files updated in the same PR | open |

### Epic E17 — Build documentation (WP-49, P0-12)
**Why:** a newcomer must be able to build from a clean machine using only the docs.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E17S1** Linux doc (P0-12.1) | `docs/rust-rewrite/BUILDING.md` with the full package list | a clean container builds following only the doc | open |
| **E17S2** Windows/macOS doc (P0-12.2) | the same for the other two OSes | tested on fresh VMs | open |
| **E17S3** Reproducible shell (P0-12.3) | `.devcontainer/` or `flake.nix` | `cargo test` runs inside it; `INSTALL` updated | open |

### Epic E18 — CI & repo hygiene (WP-50)
**Why:** small review findings that accumulate debt.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E18S1** Test-matrix CI check (P0-20.4) | CI fails when a test file lacks/loses its matrix | a matrix-less test file reds CI | open |
| **E18S2** Fixture audit (P0-11.2) | the fixture set is justified | audit merged; unused fixtures removed or justified | open |
| **E18S3** Artefacts, deps, CODEOWNERS (findings 10/11/13) | committed test-run artefacts in `test/conformance/` removed or moved; `[workspace.dependencies]` centralised; real `CODEOWNERS` handles | tree clean; deps centralised; CODEOWNERS resolves to real handles | open |

### Epic E19 — Headless smoke (WP-51, P0-16)
**Why:** the first end-to-end proof that the swapped build actually captures.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E19S1** Smoke test | null-graphics startup, non-black frame, 5 s MKV, `ffprobe` validation | all four checks pass in a run | open |
| **E19S2** Smoke in CI | the smoke as a CI job under `xvfb-run` (reusing the WP-27 checkout job) | job green on `ubuntu-latest` | open |

### Epic E20 — Phase 0 close-out (WP-52)
**Why:** the Phase 0 subtasks that the rewrite outgrew but the plan still owes.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E20S1** Admin items (P0-01.5, P0-02.1/.4, P0-05.2, P0-06.2, P0-09.5) | plan review/accept; frozen header list; contributor rule; MSRV job; mutex table; required checks | all five ticked; MSRV CI job green; required-checks export in the PR | open |
| **E20S2** Phase 0 review (P0-21) | the phase-0 review pass | P0-21 ticked; findings either fixed or booked as WPs | open |

---

## Cross-cutting

- One WP = one branch + one PR; subtask boxes tick in the same PR that finishes the work (plan §3).
- Every code PR carries the per-WP verification baseline (plan §6): fmt, clippy `-D warnings`,
  nextest, cargo deny/audit, [run.sh](https://github.com/ripmav/obs-rust/blob/main/test/conformance/run.sh)
  `check` per touched area, `--features c-swap` lint for `obs-ffi` changes, test matrix in
  [docs/tests/](tests/), ODD entry in
  [odd/tasks/rust-rewrite-phase-1.md](https://github.com/ripmav/obs-rust/blob/main/odd/tasks/rust-rewrite-phase-1.md).
- The C reference is pinned at obs-studio **33.0.1** (drift evidence in
  [PLAN-2026-11-21.md](rust-rewrite/PLAN-2026-11-21.md)); CI conformance runs per
  [.github/workflows/rust.yml](https://github.com/ripmav/obs-rust/blob/main/.github/workflows/rust.yml).
- `unsafe` in new code follows [ADR 0005](adr/0005-code-policy.md) Amendment 1: only the named
  exception sets, every block carries `// SAFETY:` (lint-enforced).
- CMake `OBS_ENABLE_RUST` integration: in-repo patch file, applied by CI/nightly (D3, decided
  2026-11-23 — see E1S4).

## Out of scope

- Phases 2–7 (spec-only, unchanged by the 2026-11-21 review).
- P1-34.4 + the S-064…S-067 spec backlog — parked as Phase 2 prep (D5).
- Deleting `deps/blake2` / `deps/json11` — stays P7-04; E13S3 only records the deferral.

## Open questions

See [decisions-open.md](decisions-open.md).
