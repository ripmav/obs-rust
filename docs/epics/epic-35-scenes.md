# E35 — Scenes

**Milestone:** M6 — Phase 4: core object model (section C)
**Subtasks:** [phase-4-subtasks.md](../rust-rewrite/subtasks/phase-4-subtasks.md) § C

**Why:** the scene tree (canvas, items, transitions) on top of the new sources.

| Story                                    | I want                                                         | Acceptance                                                                                | Status |
| ---------------------------------------- | -------------------------------------------------------------- | ----------------------------------------------------------------------------------------- | ------ |
| **E35S1** P4-16 Scene core               | Port Scene core to Rust (`obs-scene.c`, `obs-scene.h`)         | spec S-033; vectors: scenemath (canvas_sources, scene_item_size,…; scene usable as input  | open   |
| **E35S2** P4-17 Groups and nested scenes | Port Groups and nested scenes to Rust (`obs-scene.c`)          | spec S-033, S-040; vectors: scenemath (group_basic, group_move, group_membership,…; tests | open   |
| **E35S3** P4-18 Scene rendering          | Port Scene rendering to Rust (`obs-scene.c`)                   | spec S-033; vectors: scenemath (40 scripts); 50 cases equal to C                          | open   |
| **E35S4** P4-19 Scene save/load          | Port Scene save/load to Rust (`obs-scene.c`)                   | spec S-033; vectors: scenemath (prune, order); unknown keys preserved                     | open   |
| **E35S5** P4-20 Scene audio mixing       | Port Scene audio mixing to Rust (`obs-scene.c`, `obs-audio.c`) | spec S-032, S-033; vectors: audiomix (14_render_order, 16_submix); golden PCM             | open   |
