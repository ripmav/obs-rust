# E49 — Profiles, collections, importers

**Milestone:** M8 — Phase 6: frontend (section D)
**Subtasks:** [phase-6-subtasks.md](../rust-rewrite/subtasks/phase-6-subtasks.md) § D

**Why:** profile/collection management and the file importers.

| Story                                    | I want                                                                             | Acceptance                                                          | Status |
| ---------------------------------------- | ---------------------------------------------------------------------------------- | ------------------------------------------------------------------- | ------ |
| **E49S1** P6-24 Scene collection manager | Port Scene collection manager to Rust (`SceneCollection.hpp`)                      | spec S-048; corpus loads                                            | open   |
| **E49S2** P6-25 Profile manager          | Port Profile manager to Rust (`OBSBasic_Profiles.c`)                               | spec S-048, S-014; vectors: frontendfmt/ (names); UI bookend passes | open   |
| **E49S3** P6-26 Importers                | Port Importers to Rust (`importers.hpp`)                                           | golden JSON                                                         | open   |
| **E49S4** P6-27 Missing files dialog     | Port Missing files dialog to Rust (`OBSMissingFiles.hpp`, `MissingFilesModel.hpp`) | UI bookend passes                                                   | open   |
| **E49S5** P6-28 Remux dialog             | Port Remux dialog to Rust (`RemuxQueueModel.hpp`)                                  | UI bookend passes                                                   | open   |
