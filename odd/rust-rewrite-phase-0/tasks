# Rust rewrite — Phase 0 execution (P0-01…P0-21, unblocked subset)

Status: in progress. Working branch `docs/rewrite-coverage-2026-11` (L2). Scope expanded 2026-11-10 (L9) from the six ADR tasks to **all unblocked Phase 0 subtasks**: the `docs:` ADR stack (P0-01…P0-06) plus workspace skeleton, golden framework, deny policy, issue templates, labels/tracking issues, and test matrices. Reference C checkout (libobs 33.0.1) is NOT linked on this machine (L3); verified facts from it are quoted from `docs/rust-rewrite/subtasks/phase-0-subtasks.md` ("Facts verified in this checkout").

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

S13 (user instruction 2026-11-10, verbatim): "review-readability has the same model configured as all others, the `solheim/qwen3.8-37b` so do your thing and review, fix and work at the all other open tasks"
- Note: the local catalog only lists `solheim/qwen3.8-27b` (same as all sibling review agents); `solheim/qwen3.8-37b` is not resolvable in `models.json`, so `review-readability.md` was configured with `solheim/qwen3.8-27b` to match all other reviewers (L9).

S14 (new-task "Done when" quotes, `subtasks/phase-0-subtasks.md`):
- P0-07.1: "Done when: `cargo build --workspace` passes." / P0-07.2: "Done when: `cargo test` and `cargo clippy` run on all crates." / P0-07.3: "Done when: rustfmt, clippy, and toolchain configs committed." (P0-07.4 "CI runs on 3 OSes" — pending P0-09/CI)
- P0-10.1: "Done when: `cargo deny check licenses` passes." / P0-10.2: "Done when: rules documented."
- P0-11.1: "Done when: `.gitattributes` has `*.golden -text` (and `*.text -text`)."
- P0-13.1: "Done when: `assert_golden` reads/writes `*.golden`." / P0-13.2: "Done when: CI refuses to run with the variable set." / P0-13.3: "Done when: mismatch shows first differing line and byte offset." / P0-13.4: "Done when: matrix at `docs/tests/obs-golden.md`; used by one demo test."
- P0-19.1: "Done when: labels exist on the repo." / P0-19.2: "Done when: template creates a valid issue." / P0-19.3: "Done when: 8 issues exist, each linking its phase file and exit criteria."
- P0-20.1: "Done when: README explains the one-matrix-per-test-file rule." / P0-20.2: "Done when: matrix lists the cases." / P0-20.3: "Done when: matrix lists them." (P0-20.4 CI check — pending P0-09)

## Tasks

| ID | Task | S# | Route | Commit |
|---|---|---|---|---|
| T1 | P0-01: `docs/adr/0000-template.md`, `docs/adr/README.md`, `docs/adr/0001-strangler-migration.md` | S2,S3,S4,S5,S11 | inline | 892b402 |
| T2 | P0-05: `docs/adr/0005-code-policy.md` (+ P0-10.3 exception section), `CONTRIBUTING.md` dependency checklist | S7,S12 | inline | 8aae571 |
| T3 | P0-02: `docs/adr/0002-plugin-abi.md`, `CODEOWNERS`, `CONTRIBUTING.md` public-header rule | S6,S12 | inline | 396d70c |
| T4 | P0-06: `docs/adr/0006-concurrency.md` (target model + RT rules; P0-06.2 mutex table pending L3) | S8,S12 | inline | ce1bfb8 |
| T5 | P0-03: ADR 0003 graphics | S12 (D-03) | blocked: wgpu quad prototype + DMA-BUF/DXGI/IOSurface imports on 3 OS/GPUs (P0-03.1–.3, hardware) | — |
| T6 | P0-04: ADR 0004 GUI toolkit | S12 (D-01) | blocked: cxx-qt/Slint/egui dock prototypes (P0-04.2–.3, hardware) | — |
| T7 | P0-11.1: `.gitattributes` `*.text`/`*.golden` `-text` verification (patterns pre-existed; `git check-attr` = unset) | S14 | inline | 5a7ee84 |
| T8 | P0-20.1: `docs/tests/README.md` (one-matrix-per-test-file rule) + `docs/tests/_template.md` | S14 | inline | f5a2cc1 |
| T9 | P0-07.1–.3: root `Cargo.toml` workspace (9 crates), `rust-toolchain.toml`, `rustfmt.toml`, `clippy.toml`, `.cargo/config.toml`, skeleton crates with smoke tests | S14 | inline (workspace root = this repo root, L10) | 53cc6d5 |
| T10 | P0-19.2: `.github/ISSUE_TEMPLATE/rust-task.yml` (YAML-validated) | S14 | inline | 8758ca6 |
| T11 | P0-19.1: 12 labels on `ripmav/obs-rust`; P0-19.3: 8 phase tracking issues #2–#9 (phase file, subtasks, guide, verbatim exit criteria, `rust-rewrite`+`phase-N` labels) | S14 | remote (gh) | fc6250f (tick commit) |
| T12 | P0-13.1–.4: `crates/obs-golden` (10th member) — `assert_golden_str`/`assert_golden_bytes`, `UPDATE_GOLDEN=1` + in-crate CI refusal, line/byte mismatch reports, demo golden, 7 tests | S14,S12 (stdlib-first rule 4) | inline | 248c013 |
| T13 | P0-10.1–.2: `deny.toml` (D-07 allow-list in SPDX terms + GPL-2.0-or-later, openssl-sys ban, multiple-versions warn, unknown-registry deny); `cargo deny check` all ok | S14,S12 (D-07) | inline | 82011a1 |
| T14 | P0-20.2: `docs/tests/obs-golden.md` matrix (7 cases, pass); P0-20.3: `docs/tests/abi-conformance.md` matrix (lifecycle cases from P0-15 spec, pending rows) | S14 | inline | 3364bbd |

Out of scope / remaining (L14): P0-08 (Corrosion integration — needs CMake tree), P0-09 (CI pipeline + branch protection — remote/CI unit), P0-12 (BUILDING.md — clean-env build untestable here), P0-11.2/P0-14/P0-15/P0-16/P0-17 (need the C reference checkout linked, L3), P0-18 (real-world corpus — user consent on sources), P0-20.4 (CI check), P0-21 (phase review — maintainer), plus the open subtask tails P0-02.1, P0-05.2, P0-06.2, P0-07.4, P0-01.5, P0-02.4.

Tick policy (L4): subtask boxes ticked in `subtasks/phase-0-subtasks.md` only when the artifact verifiably exists; review/merge/remote-verification/CI steps stay open for the user's PR/CI. P0-13.2 ticked on the in-crate CI refusal (panic when `UPDATE_GOLDEN`+`CI` set, tested) — the CI job that sets `CI` lands with P0-09.

## Log

L1: "start working on rewrite plan `docs/rust-rewrite-plan.md` other importent documents you can find in `docs/rust-rewrite/` and other sub-folders"
L2: user checked out `docs/rewrite-coverage-2026-11` before starting; work commits land on that branch (one commit per task, S10 footer). The plan's `<prefix>/<issue>-<short-description>` branch rule applies when the user splits this into stacked PRs at push time.
L3: C reference checkout not linked on this machine (no `../obs-studio`; no `libobs/` tree found under `/home/ripmav` to depth 4). `tools/link-obs-source.sh` is the linker (default `../obs-studio`). Consequences: ADR numbers quoted from the plan §1 table + subtask file's verified facts with the subtask's reproducible footnote commands (S5 P0-01.2); P0-02.1 install-set script re-run, P0-06.1 source line refs, P0-06.2 mutex table (46 mutexes in `obs-internal.h`), P0-11.2 fixture audit, P0-14/15/16/17 pending the link.
L4: tick policy as in "Tick policy" above.
L5: branch `docs/rewrite-coverage-2026-11` already carried 38 commits (~33.5k lines vs origin/main) of coverage/decisions docs work before this ADR unit; slice accounting below for this unit: each task commit is one reviewable slice (T1 = 892b402, ~450 lines).
L6: P0-10.3 (exception process, belongs to P0-10) completed early inside T2 because its deliverable is a section of ADR 0005; ticked there.
L7: ADR 0002 open verification note: the P0-02.1 header snapshot does not list `obs.h`/`obs-config.h` and the public/internal split is provisional — both close with the install-set script re-run once the reference checkout is linked (L3). T2 commit = 8aae571.
L8: P0-06.1 ticked on the strength of the primer §4 verified thread table (7 thread kinds, in-repo); the per-file `pthread_create` enumeration against source awaits the checkout link and folds into the P0-06.2 mutex-table session. After T4: 4 of 6 ADR tasks committed; T5/T6 blocked on prototype hardware.
L9 (2026-11-10): user instruction S13 — review, fix, and work all other open tasks. Review side: `review-readability.md` had no `model:` line (first capture failed `reviewer-config-invalid`); added `model: solheim/qwen3.8-27b` to match all sibling reviewers (user said "the same model configured as all others"; `37b` is not in the local catalog). Two subsequent captures of lineage `review-5a8563cca9abb7d4` (target sha256:69a98dbd…, 11 files/933 lines, medium) failed at the host relay with `UND_ERR_SOCKET` after ~15 min thinking-only output, 0 text; provider guidance `automatic_retry: false`. Review state: `reviewing`, blocked on a reviewer model-run that completes — user decision: retry when the provider is healthy, pick a different reviewer model, or accept this candidate unreviewed.
L10 (2026-11-10): P0-07 location question resolved — the skeleton workspace lands in THIS repo root (`Cargo.toml` at root, `crates/*`), i.e. "repo root" per P0-07.1's Files spec; when the C checkout is linked, P0-08 wires the same workspace into the CMake build. T7 = 5a7ee84, T8 = f5a2cc1, T9 = 53cc6d5, T10 = 8758ca6.
L11 (2026-11-10): P0-19 remote work executed against `https://github.com/ripmav/obs-rust.git` (gh authed): 12 labels, 8 phase tracking issues (#2–#9) with corrected links after a first-body `.md` omission, tick commit T11 = fc6250f.
L12 (2026-11-10): P0-13 obs-golden (T12 = 248c013) and P0-10 deny.toml (T13 = 82011a1; `cargo-deny 0.20.2` installed for the check), P0-20.2/.3 matrices (T14 = 3364bbd). `deny.toml` needs SPDX license terms — `BSD-2`/`BSD-3` rejected by cargo-deny; replaced with `BSD-2-Clause`, `BSD-2-Clause-Patent`, `BSD-3-Clause`, `BSD-3-Clause-LBNL`, `0BSD`.
L13 (2026-11-10): workspace gates green after T14: `cargo build`/`cargo test` (19 suites ok incl. 7 obs-golden tests)/`cargo fmt --check`/`cargo clippy --all-targets -- -D warnings`; `cargo deny check` → advisories/bans/licenses/sources ok.
L14 (2026-11-10): remaining work, none further workable on this machine without (a) the C checkout linked via `tools/link-obs-source.sh` — P0-02.1, P0-05.2, P0-06.2, P0-07.4, P0-08, P0-11.2, P0-12, P0-14, P0-15, P0-16, P0-17, P0-20.4; (b) prototype hardware — P0-03 (ADR 0003), P0-04 (ADR 0004); (c) user/remote — P0-09 CI + branch protection, P0-18 corpus consent, P0-21 phase review, PR creation/merge (T5/T6 tails); (d) healthy reviewer model run or explicit waive — lineage `review-5a8563cca9abb7d4`.
L15: branch is 13 commits ahead of `origin/docs/rewrite-coverage-2026-11` (this unit), 51 commits ahead of `origin/main`; push stays the user's decision. Unit commits in order: 892b402 (T1), 8aae571 (T2), 396d70c (T3), ce1bfb8 (T4), 67ecb19 (ODD hash record), 5a7ee84 (T7), f5a2cc1 (T8), 53cc6d5 (T9), 8758ca6 (T10), fc6250f (T11 tick), 248c013 (T12), 82011a1 (T13), 3364bbd (T14).
L16 (2026-11-10): fresh review started for the completed unit (target sha256:e5ab8247…, 45 files/1,797 lines, medium, budget 200) as lineage `review-b1d801a806bea661` (lenses reliability+readability; consent resolved by host). Group forecast (2 model runs, pi_host_relay) relayed; user confirmed; the acknowledged run failed identically to the two previous ones: `UND_ERR_SOCKET` after 900s, ~54.7k thinking chars, 0 tokens, prepared_reviewers 0, mutation_performed false, automatic_retry false. Fresh bound STATUS re-offers both slots unchanged; lineage stays `reviewing`, budget untouched. User decision 2026-11-10: **pause, retry later** — resubmit only the exact reoffered one-slot binding after a fresh STATUS, and only when the user says so (provider/model healthy). Not a waiver: the candidate is not left unreviewed; it is pending.
L17 (2026-11-10): five capture attempts on lineage `review-b1d801a806bea661`, all user-triggered, all failed: (1)(4)(5) thinking-only streams (54.7k/70.9k/76.2k chars, 0 text, 0 tokens) closed by the solheim gateway at exactly ~900s (`UND_ERR_SOCKET`, `provider-unavailable`); (3) the one run that reached text (32k thinking tokens + 2,190 chars) was truncated by the model's 32,768 output cap → admission refused (`review-readability-1-82c497bbd731.json` preserved under `.git/gentle-ai/rejected-results/`; reviewer had produced a genuine finding). Fix sequence applied between attempts: `GENTLE_PI_REVIEW_RELAY_PI_TIMEOUT_MS=7200000` (relay bound confirmed 7,200,000 in failure JSON), pi catalog `~/.pi/agent/models.json` maxTokens 32768→65536 + provider `default_effort: minimal` (user), gentle-ai store `default_effort`/per-agent `effort: minimal` (user). Diagnosis: the gateway cuts any stream with no text tokens for ~15 min; the model still thinks 15+ min on this 153KB materialized prompt, so every attempt dies before text — except the lottery one, which would now be admitted given the 65536 headroom. Bounded stop reached (automatic_retry false, identical signature 3× after config fixes); user decision pending: gateway timeout raise / one-shot retries / smaller slices / explicit waiver. Lineage intact, budget 200 unused.
L19 (2026-11-10): user: “the smaller the better” — started minimal slice lineage `review-d5fdf2845a76fd65` over `fc6250f..HEAD` (8 commits, 12 files, 512 lines: obs-golden crate + tests + demo golden, deny.toml, two test matrices, .gitignore, ODD logs). One input error along the way: first acked capture was rejected (`capture-group-rejected`) for a one-char transcription drift in the repository-context token; fixed by fresh STATUS and byte-exact resubmission (no state change, slot still open). Attempt failed with the same signature: 904,239ms thinking-then-socket-close — this run emitted 98,401 thinking chars, then 208 text chars that arrived *after* the gateway's ~900s text-less window expired. Thinking volume across the three size points: 1,797 lines → 54–76k chars; 852 lines → 64.7k; 512 lines → 98.4k. **No correlation with prompt size (smaller actually thought more); the gateway cap wins at every slice size.** Slicing strategy is now refuted at three sizes (7/7 attempts total). Parked lineages, all intact, all budgets (200) unused: `review-b1d801a806bea661` (full unit), `review-8f095e3d5aae8392` (852-line slice), `review-d5fdf2845a76fd65` (512-line slice). Remaining options unchanged: (1) raise the solheim gateway text-less-stream timeout — only root-cause fix; (2) explicit waiver; (3) further one-shot lottery (same ~1/6 odds; any run reaching text early now passes with the 65536 cap).
