# E28 — Effect language

**Milestone:** M5 — Phase 3: graphics (section A)
**Subtasks:** [phase-3-subtasks.md](../rust-rewrite/subtasks/phase-3-subtasks.md) § A

**Why:** the effect/shader language gates all rendering: parser + golden AST dumps before the `gs_*` port.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E28S1** P3-01 Effect-file preprocessing | Port Effect-file preprocessing to Rust (`effect-parser.c`, `cf-lexer.c`) | spec S-020, S-011; vectors: effect (41 scripts), lexer (25 scripts); #ifdef-per-backend effect files parse equal to C | open |
| **E28S2** P3-02 Shader parser | Port Shader parser to Rust (`shader-parser.c`, `shader-parser.h`) | spec S-020; vectors: effect; AST can print itself | open |
| **E28S3** P3-03 Effect parser | Port Effect parser to Rust (`effect-parser.c`, `effect-parser.h`) | spec S-020; all param types | open |
| **E28S4** P3-04 Effect runtime | Port Effect runtime to Rust (`effect.c`, `effect.h`) | spec S-020; vectors: effect 60-runtime-lookup, 61-runtime-values, 62-runtime-loop; type-mismatch behaviour equal | open |
| **E28S5** P3-05 Shader transpiler to WGSL | Port Shader transpiler to WGSL to Rust | spec S-020; written decision with a spike | open |
| **E28S6** P3-06 Legacy transpilers | Port Legacy transpilers to Rust (`gl-shaderparser.c`, `gl-shaderparser.h`) | spec S-057, S-020; vectors: glbackend (22 scripts); text equal on all effects (where the vectors pin it) and it compiles | open |
