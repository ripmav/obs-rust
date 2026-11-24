# E57 — Hardening and release

**Milestone:** M9 — Phase 7: retire C/C++ (section E)
**Subtasks:** [phase-7-subtasks.md](../rust-rewrite/subtasks/phase-7-subtasks.md) § E

**Why:** soak, benchmarks, security review and the first all-Rust release.

| Story                                  | I want                                               | Acceptance                                      | Status |
| -------------------------------------- | ---------------------------------------------------- | ----------------------------------------------- | ------ |
| **E57S1** P7-18 Security audit         | Port Security audit to Rust                          | scope committed and auditor picked              | open   |
| **E57S2** P7-19 Fuzzing in CI          | Port Fuzzing in CI to Rust                           | every target builds and fuzzes                  | open   |
| **E57S3** P7-20 Performance acceptance | Port Performance acceptance to Rust (`benchmarks.m`) | 3 setups per OS committed                       | open   |
| **E57S4** P7-21 Unsafe budget report   | Port Unsafe budget report to Rust                    | summary committed                               | open   |
| **E57S5** P7-22 Release candidates     | Port Release candidates to Rust                      | RC1 published and the crash-rate dashboard live | open   |
| **E57S6** P7-23 1.0 release            | Port 1.0 release to Rust                             | notes and migration guide live                  | open   |
| **E57S7** P7-24 Retrospective          | Port Retrospective to Rust                           | feedback collected and committed                | open   |
