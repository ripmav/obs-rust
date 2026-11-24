# E7 — Signal threading & refcount hardening (WP-36 + WP-37)

**Milestone:** M1 — Phase 1: code & recipe

Signal threading & refcount hardening (WP-36 + WP-37)
**Why:** the audit's concurrency findings (P1-22.3, P1-26.1–.3) plus the two facade leftovers.

| Story                                  | I want                                                             | Acceptance                                                                                                                                                                      | Status |
| -------------------------------------- | ------------------------------------------------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------ |
| **E7S1** Signal thread tests (P1-22.3) | `loom` coverage of connect-during-emit                             | no deadlock under `loom` for connect-from-other-thread during emit; lock structure kept (per-signal recursive, handler, global-callbacks); any deviation documented (S-004 Q-2) | open   |
| **E7S2** Refcount semantics (P1-26.1)  | `get_obj`/setter ref rules observable                              | refcount through the debug API equals C on the [obsdata/](https://github.com/ripmav/obs-rust/tree/main/test/conformance/scripts/obsdata) `refs`/`a_refs` vectors (S-014 §5.1)   | open   |
| **E7S3** Miri/ASan sweep (P1-26.2/.3)  | the pure crates clean under the extended Miri scope + an ASan job  | both clean; findings fixed in the same PR                                                                                                                                       | open   |
| **E7S4** `log` facade (P1-13.3)        | Rust `log` records forwarded to `blog`                             | level-mapping table test passes                                                                                                                                                 | open   |
| **E7S5** Source profiler (P1-12.3)     | RAII guard for Rust users + port of `source-profiler.c` (S-055 U3) | goldens pass; S-055 Q-7 (history lags one frame, zero tick = no data) and Q-2 (`fill_result` false + zeroed struct) tested                                                      | open   |
