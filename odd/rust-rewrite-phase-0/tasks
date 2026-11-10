# Rust rewrite — Phase 0 ADR stack (P0-01…P0-06)

Status: in progress. Working branch `docs/rewrite-coverage-2026-11` (L2). Scope: the six Phase 0 ADR tasks — the `docs:` start of the PR stack. P0-07…P0-21 are a later unit (see Out of scope). Reference C checkout (libobs 33.0.1) is NOT linked on this machine (L3); verified facts from it are quoted from `docs/rust-rewrite/subtasks/phase-0-subtasks.md` ("Facts verified in this checkout").

## Specs

S1 (user request, verbatim): "start working on rewrite plan `docs/rust-rewrite-plan.md` other importent documents you can find in `docs/rust-rewrite/` and other sub-folders"

S2 (`docs/rust-rewrite-plan.md` §5, verbatim): "Work in stacked PRs, one per crate or plugin; start the stack with `docs:` ADR/spec PRs."

S3 (`docs/rust-rewrite/phase-0-foundation.md`, verbatim): "Start the PR stack with the `docs:` tasks (P0-01 to P0-06)."

S4 (P0-01 task, verbatim): "- [ ] **P0-01 ADR: migration strategy** (S)\n  - Do: record the strangler-fig decision, the "every phase ships" rule, and the exit criteria per phase.\n  - Deliverables: `docs/adr/0001-strangler-migration.md`.\n  - Done when: reviewed by two maintainers; alternatives (big-bang, new-repo) are listed with reasons for rejection."

S5 (P0-01 subtasks, `subtasks/phase-0-subtasks.md` §P0-01; full Files/Steps verbatim there, "Done when" quoted):
- P0-01.1: "Done when: template renders on GitHub and README lists how to add an ADR."
- P0-01.2: "Done when: every number has a reproducible command in a footnote, e.g. `git ls-files libobs/ | grep -E '\.(c|cpp|cc|h|hpp|m|mm|swift|ui|effect)$' | xargs cat | wc -l`."
- P0-01.3: "Done when: each alternative has at least two concrete reasons."
- P0-01.4: "Done when: table has 8 rows and a measurable check each."
- P0-01.5: "Done when: PR merged; ADR index lists it." (human: maintainer reviews)

S6 (P0-02 task, verbatim): "- [ ] **P0-02 ADR: plugin ABI policy** (S)\n  - Source: `libobs/obs-module.h`, `obs-source.h`, `obs-output.h`, `obs-encoder.h`, `obs-service.h`.\n  - Do: define the C ABI compatibility policy — **best-effort** per D-02 (not a frozen hard requirement: compatibility is actively maintained, breakage allowed only with a deprecation path per D-05), how versions are bumped (`LIBOBS_API_VER`), and how a native Rust plugin API coexists.\n  - Deliverables: `docs/adr/0002-plugin-abi.md`.\n  - Done when: lists every header in the compat-tracked ABI and the ADR/deprecation rules for changes."
- P0-02.1: "Done when: list in ADR matches the install set exactly (script output attached)." (script re-run pending L3; list = verified install-set snapshot in the subtask file)
- P0-02.2: "Done when: three worked examples (add field at end, remove symbol, change enum value) are classified."
- P0-02.3: "Done when: sequence diagram of module load for both kinds is in the ADR."
- P0-02.4: "Done when: a test PR touching `obs-source.h` requests the owners automatically." (verification pending remote; files created)

S7 (P0-05 task, verbatim): "- [ ] **P0-05 ADR: unsafe, MSRV, dependency policy** (S)\n  - Do: define `unsafe` rules (FFI and platform boundaries only, `// SAFETY:` required), MSRV (latest stable), licence allow-list (GPLv2+ compatible), and "stdlib first" for deps.\n  - Deliverables: `docs/adr/0005-code-policy.md`.\n  - Done when: ADR 0005 merged with all five rules and the dependency checklist (P0-05.3)."
- P0-05.1: "Done when: lint config committed in P0-07."
- P0-05.2: "Done when: policy text merged." (MSRV CI job lands in P0-09; no `.github/` in this checkout)
- P0-05.3: "Done when: checklist is in `CONTRIBUTING.md`."
- P0-05.4: "Done when: rule referenced from ADR."

S8 (P0-06 task, verbatim): "- [ ] **P0-06 ADR: concurrency model** (S)\n  - Source: `libobs/obs-video.c`, `obs-audio.c`, `media-io/video-io.c`, `audio-io.c`.\n  - Do: document the current thread layout (graphics thread, audio thread, encoder threads, output threads) and the target one (real-time threads, lock-free queues, tokio only for network).\n  - Done when: lists every lock in `obs-internal.h` and its replacement."
- P0-06.1: "Done when: diagram with at least 6 thread kinds is in the ADR." (primer §4 table in; source line refs pending L3)
- P0-06.2: "Done when: table has every mutex; unknown entries are marked TODO with an owner." (46 mutexes per `guides/phase-0-guide.md`; table pending L3 — section marked TODO with owner)
- P0-06.3: "Done when: hierarchy rule can be checked by a debug-only lock-order assert."
- P0-06.4: "Done when: referenced from Phase 2."

S9 (Phase 0 exit criteria, verbatim): "Exit criteria: ADRs 1-6 merged, CI green on 3 OSes with Rust jobs, golden files generated from C, ABI plugin test passing, benchmark baseline stored."

S10 (README rule 8, verbatim): "Commit footer: `Assisted-by: [Model] ([Effort]) via [Tool]` (omit the effort brackets when the model has no effort setting)."

S11 (plan §1 measured table, verbatim): "| Core engine (`libobs`) | `libobs/` | 94k | C (+ObjC for macOS) |\n| Graphics backends | `libobs-opengl`, `libobs-d3d11`, `libobs-metal`, `libobs-winrt` | 17k | C / C++ / Swift |\n| Frontend (Qt6 GUI) | `frontend/` | 109k | C++ (+ `.ui`) |\n| Plugins (41 modules) | `plugins/` | 172k | C / C++ / ObjC |\n| Shared helpers | `shared/` | 22k | C / C++ |\n| Vendored deps | `deps/` (blake2, glad, json11, libcaption, libdshowcapture, ...) | 70k | C / C++ |" + "Total about 494k tracked source lines (`tools/task-coverage.py` against libobs 33.0.1), 15,819 commits, 3 OSes, 41 plugin modules." + "It is ~680 exported `EXPORT` symbols in `obs.h` alone."

S12 (confirmed decisions, verbatim from `DECISIONS-NEEDED.md`):
- D-01: "**Confirmed (user, 2026-11-05):** `cxx-qt` — keep Qt6, migrate the `.ui` forms; P0-04 still prototypes the alternatives as due diligence, but the Phase 6 wording stands unless the prototype surfaces a blocker"
- D-02: "**Confirmed (user, 2026-11-05): Best-effort.** `obs-ffi` still serves the C ABI (`obs.h` & friends) from Phase 1 onward and compatibility is actively maintained, but it is not a frozen hard requirement: breakage is allowed only with a deprecation path and migration docs (the D-05 timeline governs removal). Phase files updated 2026-11-05"
- D-03: "**Confirmed (user, 2026-11-05):** Yes — wgpu is the one backend; legacy backends stay selectable (`--graphics wgpu\|legacy`) until parity (P3-22 adapters), deleted in P7-01 (D-13: D3D11/Metal ported to Rust per S-078/S-079; OpenGL stays C)"
- D-05: "**Confirmed (user, 2026-11-05):** announce with SDK 1.0, remove after 3 Rust releases (P7-10.3 names the release in the ADR)"
- D-06: "**Confirmed (user, 2026-11-05):** native `cdylib` + `#[obs_module]` proc-macro; WASM stays research (P7-12)"
- D-07: "**Confirmed (user, 2026-11-05):** GPL-2.0-or-later; `deny.toml` allow-list = MIT/Apache-2.0/BSD/ISC/Zlib/MPL-2.0 (P0-10.1)"
- D-24: "**Confirmed (user, 2026-11-06): Track latest stable.** Bump within one release cycle (P0-05.2 already encodes this: ADR 0005 + MSRV CI job). No task-file changes."

## Tasks

| ID | Task | S# | Route | Commit |
|---|---|---|---|---|
| T1 | P0-01: `docs/adr/0000-template.md`, `docs/adr/README.md`, `docs/adr/0001-strangler-migration.md` | S2,S3,S4,S5,S11 | inline | — |
| T2 | P0-05: `docs/adr/0005-code-policy.md` (+ P0-10.3 exception section), `CONTRIBUTING.md` dependency checklist | S7,S12 | inline | — |
| T3 | P0-02: `docs/adr/0002-plugin-abi.md`, `CODEOWNERS`, `CONTRIBUTING.md` public-header rule | S6,S12 | inline | — |
| T4 | P0-06: `docs/adr/0006-concurrency.md` (target model + RT rules; P0-06.1 source refs / P0-06.2 mutex table pending L3) | S8,S12 | inline | — |
| T5 | P0-03: ADR 0003 graphics | S12 (D-03) | blocked: wgpu quad prototype + DMA-BUF/DXGI/IOSurface imports on 3 OS/GPUs (P0-03.1–.3, hardware) | — |
| T6 | P0-04: ADR 0004 GUI toolkit | S12 (D-01) | blocked: cxx-qt/Slint/egui dock prototypes (P0-04.2–.3, hardware) | — |

Out of scope (later unit): P0-07…P0-21 (Cargo workspace, Corrosion, CI, golden/ABI/smoke/bench harnesses, corpus). Needs the C build environment this checkout lacks (no `CMakeLists.txt`, no `.github/`) plus P0-03/P0-04 prototype hardware. Open question to surface at P0-07 start: where the Cargo workspace lands — plan says "repo root, next to the CMake build"; this planning repo has no CMake build.

Tick policy (L4): subtask boxes ticked in `subtasks/phase-0-subtasks.md` only when the artifact verifiably exists; review/merge/remote-verification steps (P0-01.5, P0-02.4, P0-05.2 "merged") stay open for the user's PR; parent task boxes stay open until all subtasks close.

## Log

L1: "start working on rewrite plan `docs/rust-rewrite-plan.md` other importent documents you can find in `docs/rust-rewrite/` and other sub-folders"
L2: user checked out `docs/rewrite-coverage-2026-11` before starting; work commits land on that branch (one commit per task, S10 footer). The plan's `<prefix>/<issue>-<short-description>` branch rule applies when the user splits this into stacked PRs at push time.
L3: C reference checkout not linked on this machine (no `../obs-studio`; no `libobs/` tree found under `/home/ripmav` to depth 4). `tools/link-obs-source.sh` is the linker (default `../obs-studio`). Consequences: ADR numbers quoted from the plan §1 table + subtask file's verified facts with the subtask's reproducible footnote commands (S5 P0-01.2); P0-02.1 install-set script re-run, P0-06.1 source line refs, P0-06.2 mutex table (46 mutexes in `obs-internal.h`) pending `tools/link-obs-source.sh` run.
L4: tick policy as in "Tick policy" above.
