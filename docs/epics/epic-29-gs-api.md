# E29 — `gs_*` API

**Milestone:** M5 — Phase 3: graphics (section B)
**Subtasks:** [phase-3-subtasks.md](../rust-rewrite/subtasks/phase-3-subtasks.md) § B

**Why:** the Rust trait surface every backend and built-in effect is built against.

| Story                                       | I want                                                               | Acceptance                                                                                             | Status |
| ------------------------------------------- | -------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------ | ------ |
| **E29S1** P3-07 Trait design                | Port Trait design to Rust (`graphics.h`, `device-exports.h`)         | spec S-043; table in docs                                                                              | open   |
| **E29S2** P3-08 C shim                      | Port C shim to Rust (`graphics.c`)                                   | spec S-043; vectors: graphics (31 scripts; 02-context, 03-threads); nested enter test                  | open   |
| **E29S3** P3-09 Matrix stack and transforms | Port Matrix stack and transforms to Rust (`graphics.c`, `matrix4.c`) | spec S-043, S-008; vectors: graphics 04-matrix-stack; math (S-008, 8 scripts); golden                  | open   |
| **E29S4** P3-10 Immediate-mode vertex API   | Port Immediate-mode vertex API to Rust (`graphics.c`)                | spec S-043; vectors: graphics (05-immediate-mode, 06-immediate-limits,…; vertex data equal             | open   |
| **E29S5** P3-11 Texture utilities           | Port Texture utilities to Rust (`texture-render.c`)                  | spec S-043; vectors: graphics 16-texrender; begin/end with zstencil                                    | open   |
| **E29S6** P3-12 Image loading               | Port Image loading to Rust (`image-file.c`, `image-file.h`)          | spec S-053; vectors: gfximages (30-still-png, 31-still-formats-errors,…; pixel hash equal for 30 files | open   |
