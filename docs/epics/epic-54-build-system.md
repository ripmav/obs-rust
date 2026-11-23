# E54 — Build system

**Milestone:** M9 — Phase 7: retire C/C++ (section B)
**Subtasks:** [phase-7-subtasks.md](../rust-rewrite/subtasks/phase-7-subtasks.md) § B

**Why:** CMake retirement: Cargo + cargo-xtask own the build, thin packaging layer.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E54S1** P7-06 Cargo-first build | Port Cargo-first build to Rust | replaces documented CMake commands | open |
| **E54S2** P7-07 Dependency management | Port Dependency management to Rust | every pinned dep has a version and SHA-256 and fetch verifies | open |
| **E54S3** P7-08 Cross-compilation and caches | Port Cross-compilation and caches to Rust | both targets build in CI | open |
| **E54S4** P7-09 Reproducible and signed builds | Port Reproducible and signed builds to Rust | two clean builds are hash-equal on 3 OSes | open |
