# Rust rewrite: task breakdown

Detailed, single-task breakdown for the strategy in [`../rust-rewrite-plan.md`](../rust-rewrite-plan.md).

| Phase | File | Theme |
|---|---|---|
| 0 | [phase-0-foundation.md](phase-0-foundation.md) | ADRs, workspace, CI, golden and ABI test harness |
| 1 | [phase-1-leaf-libraries.md](phase-1-leaf-libraries.md) | util, math, signal/proc/calldata, obs-data |
| 2 | [phase-2-media-pipeline.md](phase-2-media-pipeline.md) | video/audio IO, conversion, mixer, NAL parsers |
| 3 | [phase-3-graphics.md](phase-3-graphics.md) | effect language, `gs_*` trait, wgpu backend |
| 4 | [phase-4-core-object-model.md](phase-4-core-object-model.md) | sources, scenes, outputs, encoders, C ABI |
| 5 | [phase-5-plugins.md](phase-5-plugins.md) | every plugin, one task each |
| 6 | [phase-6-frontend.md](phase-6-frontend.md) | GUI, scripting, packaging |
| 7 | [phase-7-retire-c.md](phase-7-retire-c.md) | remove C/C++ and CMake, SDK 1.0 |

## Task format

Every task is a checkbox with a stable ID (`P<phase>-<nn>`) and these fields:

- **Source**: the C/C++ files being replaced or wrapped.
- **Do**: the concrete work.
- **Deliverables**: what lands in the PR.
- **Done when**: acceptance criteria, which must be verifiable.
- **Depends on**: task IDs that must be finished first.
- **Size**: S (<=2 days), M (<=1 week), L (<=3 weeks), XL (split further before starting).

## Rules that apply to every task

1. One task is one PR (or one PR stack if it is L/XL). Branch: `<prefix>/<issue>-<short-description>`.
2. Work starts with the test: add or extend a golden/unit test that fails against the Rust code, then implement.
3. Keep the C ABI intact. A task that changes a public header is not allowed without an ADR.
4. Tick the checkbox in this file in the same PR that finishes the task.
5. Every test file gets a test matrix in `docs/tests/`; every issue gets `docs/tests/I-<n>-<name>.md`.
6. Update README/rustdoc/comments with each change.
7. Gate before commit: `cargo fmt --check`, `cargo clippy --all-targets -- -D warnings`, `cargo deny check`, `cargo audit`, `cargo test --workspace`, plus the C build (`cmake --build`) and `ctest` while C code remains.
8. Commit footer: `Assisted-by: [Model] ([Effort]) via Claude Code`.

## Subtasks

Every task is broken into day-sized subtasks (`P<phase>-<nn>.<m>`) in [`subtasks/`](subtasks/): `phase-0-subtasks.md` … `phase-7-subtasks.md`. Each subtask lists **Files**, **Steps** and **Done when**; file and API names were checked against this checkout (the empty submodules `obs-websocket`, `obs-browser` and `libdshowcapture` could not be checked). Tick a subtask in its file; tick the parent task in the phase file once all of its subtasks are done.

## Developer guides

Start with [`guides/00-developer-primer.md`](guides/00-developer-primer.md) (build the C OBS, vocabulary, threads, FFI rules, test strategy), then the guide for your phase: [`guides/phase-0-guide.md`](guides/phase-0-guide.md) … [`guides/phase-7-guide.md`](guides/phase-7-guide.md). Each guide explains how the existing C code works (with file and line references), the Rust design, ordering, pitfalls and a verification checklist. Where a guide corrects a task or subtask (for example the Phase 2/3 dependency notes), the correction is also written into the phase and subtask files.

## Clean-room specifications and conformance suite

For work that must be done **without reading the C implementation** (by any engineer or AI agent):

- Process, roles, forbidden paths, provenance: [`clean-room/PROCESS.md`](clean-room/PROCESS.md); AI-agent prompt block: [`clean-room/AGENT-BRIEF.md`](clean-room/AGENT-BRIEF.md).
- Specifications (56 written, `S-001` … `S-060`, remaining gaps listed): [`specs/README.md`](specs/README.md).
- Executable conformance suite (1,561 scripts / 1,554 vectors, 59 areas): `test/conformance/README.md`; verify with `test/conformance/run.sh check`.
- Coverage: every file of the C reference mapped to a task in [`coverage/coverage-matrix.md`](coverage/coverage-matrix.md) (regenerate with `python3 tools/task-coverage.py --write`); line-level spec/test coverage with `python3 tools/coverage.py`.
- Decisions that need user/maintainer input, each with a default so work is never blocked: [`DECISIONS-NEEDED.md`](DECISIONS-NEEDED.md).

Task files reference the spec that applies (for Phase 1: P1-01 → S-001, P1-03/P1-04 → S-005, P1-20 → S-002, P1-21 → S-003, P1-22/P1-23 → S-004).
