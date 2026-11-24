# E9 — C-reference dump tool (WP-39, P0-14)

**Milestone:** M2 — Phase 1: integration & review

C-reference dump tool (WP-39, P0-14)
**Why:** goldens for the math/obsdata areas are blocked on the dump target.

| Story                            | I want                                              | Acceptance                                                                                                                 | Status |
| -------------------------------- | --------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------- | ------ |
| **E9S1** `obs-ref-dump` target   | the dump binary with math + data commands           | target builds; math and data dumps run against the C build                                                                 | open   |
| **E9S2** Golden generation in CI | `test/golden/` generation + regenerate-and-diff job | CI job reds on a deliberate golden change; unblocks P1-15.3 (10k seeded cases, 1e-6 relative) and P1-25.5 (corpus compare) | open   |
