# E46 — Application shell

**Milestone:** M8 — Phase 6: frontend (section A)
**Subtasks:** [phase-6-subtasks.md](../rust-rewrite/subtasks/phase-6-subtasks.md) § A

**Why:** the frontend application shell on cxx-qt (D-01).

| Story                                         | I want                                                              | Acceptance                                                                                 | Status |
| --------------------------------------------- | ------------------------------------------------------------------- | ------------------------------------------------------------------------------------------ | ------ |
| **E46S1** P6-01 Launch and CLI                | Port Launch and CLI to Rust (`obs-main.c`)                          | spec S-048; vectors: frontendfmt/ (cli_flags); table lists all with semantics              | open   |
| **E46S2** P6-02 Application object and config | Port Application object and config to Rust (`OBSApp.c`, `OBSApp.h`) | spec S-048; vectors: frontendfmt/ (config_defaults, migrate_user); reads/writes real files | open   |
| **E46S3** P6-03 Themes                        | Port Themes to Rust (`OBSApp_Themes.c`, `OBSTheme.h`)               | all shipped themes parse                                                                   | open   |
| **E46S4** P6-04 Localisation                  | Port Localisation to Rust (`OBSApp.c`)                              | spec S-010; all 77 files load                                                              | open   |
| **E46S5** P6-05 Logging and crash upload      | Port Logging and crash upload to Rust (`OBSLogViewer.hpp`)          | UI bookend passes                                                                          | open   |
| **E46S6** P6-06 Main window skeleton          | Port Main window skeleton to Rust (`OBSBasic.hpp`)                  | all menu items present (list from the .ui)                                                 | open   |
