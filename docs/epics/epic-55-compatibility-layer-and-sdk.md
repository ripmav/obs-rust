# E55 — Compatibility layer and SDK

**Milestone:** M9 — Phase 7: retire C/C++ (section C)
**Subtasks:** [phase-7-subtasks.md](../rust-rewrite/subtasks/phase-7-subtasks.md) § C

**Why:** the C ABI shim becomes an opt-in compat feature; native plugin SDK 1.0.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E55S1** P7-10 C ABI as opt-in compat feature | Port C ABI as opt-in compat feature to Rust | a legacy C plugin loads with the feature on; build passes with it off | open |
| **E55S2** P7-11 Native plugin SDK 1.0 | Port Native plugin SDK 1.0 to Rust | obs-plugin-api is frozen | open |
| **E55S3** P7-12 WASM sandbox research | Port WASM sandbox research to Rust | one WASM source plugin renders a frame | open |
| **E55S4** P7-13 Scripting API stability | Port Scripting API stability to Rust | the corpus runs green against the Rust bindings | open |
