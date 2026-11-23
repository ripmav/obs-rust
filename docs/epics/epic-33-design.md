# E33 — Design

**Milestone:** M6 — Phase 4: core object model (section A)
**Subtasks:** [phase-4-subtasks.md](../rust-rewrite/subtasks/phase-4-subtasks.md) § A

**Why:** the ownership/handle design (Arc-based handles, typed IDs for the C ABI) everything in Phase 4 is built on.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E33S1** P4-01 Object model design doc | Port Object model design doc to Rust (`obs.h`, `obs-internal.h`) | spec S-040, S-042, S-041; vectors: obssource (01_lifecycle_refcount, 02_weak_refs,…; table of every kind (source, scene, sceneitem, encoder, output, service, data, properties, canvas, display, view, module) | open |
| **E33S2** P4-02 Handle registry and refcount compat | Port Handle registry and refcount compat to Rust (`obs-source.c`, `obs-encoder.c`) | spec S-040, S-035; vectors: obssource (01_lifecycle_refcount), modservice…; refcount observable through a debug function equals C after scripted sequences | open |
| **E33S3** P4-03 Global context | Port Global context to Rust (`obs.c`, `obs-internal.h`) | spec S-042; vectors: obscore (010_startup_shutdown, 011_locale_version_cmdline,…; no static mut | open |
| **E33S4** P4-04 Lock inventory and ordering | Port Lock inventory and ordering to Rust (`obs-internal.h`) | spec S-042, S-041, S-040; complete | open |
