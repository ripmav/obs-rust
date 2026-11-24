# E31 — Built-in effects and render loop

**Milestone:** M5 — Phase 3: graphics (section D)
**Subtasks:** [phase-3-subtasks.md](../rust-rewrite/subtasks/phase-3-subtasks.md) § D

**Why:** built-in effects and the render loop must produce identical frames (image diff).

| Story                                       | I want                                                   | Acceptance                                                                                          | Status |
| ------------------------------------------- | -------------------------------------------------------- | --------------------------------------------------------------------------------------------------- | ------ |
| **E31S1** P3-24 Built-in effects            | Port Built-in effects to Rust                            | spec S-053; vectors: gfximages (40-effects-default-color, 41-effects-scaling,…; all compile on wgpu | open   |
| **E31S2** P3-25 Plugin effects              | Port Plugin effects to Rust                              | vectors: effect (S-020 corpus covers shipped effects); list generated in CI                         | open   |
| **E31S3** P3-26 Render loop                 | Port Render loop to Rust (`obs-video.c`)                 | spec S-058, S-053; vectors: obsvideo (23 scripts); frame hash equal                                 | open   |
| **E31S4** P3-27 Multi-canvas render support | Port Multi-canvas render support to Rust (`obs-video.c`) | spec S-058; two fake mixes render                                                                   | open   |
