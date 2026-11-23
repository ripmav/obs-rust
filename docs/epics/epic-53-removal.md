# E53 — Removal

**Milestone:** M9 — Phase 7: retire C/C++ (section A)
**Subtasks:** [phase-7-subtasks.md](../rust-rewrite/subtasks/phase-7-subtasks.md) § A

**Why:** removing the C sources (P7-01) — the actual "replace everything" step.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E53S1** P7-01 Delete C libobs and backends | Port Delete C libobs and backends to Rust | spec S-042, S-043, S-058; vectors: obscore/, graphics/, glbackend/, obsvideo/; checklist signed off | open |
| **E53S2** P7-02 Delete C++ frontend | Port Delete C++ frontend to Rust (`parity-checklist.m`) | checklist signed off | open |
| **E53S3** P7-03 Delete C plugins | Port Delete C plugins to Rust | table committed with 41 rows; every kept dir names its decision ID | open |
| **E53S4** P7-04 Delete vendored deps | Port Delete vendored deps to Rust | scan table committed; every consumer is scheduled before this wave | open |
| **E53S5** P7-05 Delete `shared/` | Port Delete `shared/` to Rust | table committed; every module named with its consumers or keep reason | open |
| **E53S6** P7-25 Delete the C test harness (added) | Port Delete the C test harness (added) to Rust | spec S-005, S-010, S-023, S-040; vectors: darray/, serial/, platform/, obssource/; coverage table committed in docs/tests/ | open |
