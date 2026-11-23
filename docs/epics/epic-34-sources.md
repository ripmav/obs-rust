# E34 — Sources

**Milestone:** M6 — Phase 4: core object model (section B)
**Subtasks:** [phase-4-subtasks.md](../rust-rewrite/subtasks/phase-4-subtasks.md) § B

**Why:** `obs_source` is the most-touched struct in libobs — the first big integration surface.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E34S1** P4-05 Source info vtable adapter | Port Source info vtable adapter to Rust (`obs-source.h`, `obs-source.c`) | spec S-040, S-035; vectors: modservice (24-register-other-kinds,…; one-to-one table | open |
| **E34S2** P4-06 Source lifecycle | Port Source lifecycle to Rust (`obs-source.c`) | spec S-040, S-042; vectors: obssource (00_smoke, 01_lifecycle_refcount,…; callback trace equal | open |
| **E34S3** P4-07 Async video sources | Port Async video sources to Rust (`obs-source.c`) | spec S-040; vectors: obssource (30_async_basic, 34_async_cache_reuse,…; ordering and drop semantics equal | open |
| **E34S4** P4-08 Audio on sources | Port Audio on sources to Rust (`obs-source.c`, `obs-audio.c`) | spec S-032; vectors: audiomix (30_ingest_timestamps, 31_ingest_far_timestamp,…; tests with 44.1k→48k | open |
| **E34S5** P4-09 Filters | Port Filters to Rust (`obs-source.c`) | spec S-040, S-032; vectors: obssource (20_filter_add_remove, 21_filter_compat,…; order test | open |
| **E34S6** P4-10 Source render path | Port Source render path to Rust (`obs-source.c`, `obs-video.c`) | spec S-040; vectors: obssource (14_activation_tree, 15_active_child_api,…; golden frame | open |
| **E34S7** P4-11 Transitions | Port Transitions to Rust (`obs-source-transition.c`) | spec S-040; vectors: obssource (50_tr_set_clear, 51_tr_start_stop,…; timeline test | open |
| **E34S8** P4-12 Source signals and procs | Port Source signals and procs to Rust (`obs-source.c`, `obs.h`) | spec S-040, S-004; vectors: obssource (every script emits; e.g.…; list committed | open |
| **E34S9** P4-13 Source types registry | Port Source types registry to Rust (`obs-module.c`, `obs.c`) | spec S-035, S-042; vectors: modservice (24-register-other-kinds,…; enumeration order equal | open |
| **E34S10** P4-14 Interaction API | Port Interaction API to Rust (`obs-interaction.h`) | layout test | open |
| **E34S11** P4-15 Missing-files tracking | Port Missing-files tracking to Rust (`obs-missing-files.c`, `obs-missing-files.h`) | spec S-024; vectors: helpers (missing-files-basic, missing-files-append,…; add/remove/count tests | open |
