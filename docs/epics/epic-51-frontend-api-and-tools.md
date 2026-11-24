# E51 — Frontend API and tools

**Milestone:** M8 — Phase 6: frontend (section F)
**Subtasks:** [phase-6-subtasks.md](../rust-rewrite/subtasks/phase-6-subtasks.md) § F

**Why:** OBSStudioAPI re-exposed through obs-ffi + scripting (mlua/pyo3) + frontend-tools.

| Story                                     | I want                                                                                  | Acceptance                                                                     | Status                                                     |
| ----------------------------------------- | --------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------ | ---------------------------------------------------------- |
| **E51S1** P6-36 Frontend API              | Port Frontend API to Rust (`obs-frontend-api.h`)                                        | spec S-048; table committed                                                    | open                                                       |
| **E51S2** P6-37 Scripting host            | Port Scripting host to Rust (`obs-scripting.c`, `obs-scripting-lua.c`)                  | spec S-039, S-060, S-063; vectors: scripting/, luascript/; example scripts run | partial — P6-37.1, P6-37.2, P6-37.3, P6-37.4, P6-37.5 open |
| **E51S3** P6-38 Auto scene switcher       | Port Auto scene switcher to Rust (`auto-scene-switcher-win.c`, `auto-scene-switcher.c`) | matching test passes                                                           | open                                                       |
| **E51S4** P6-39 Output timer and captions | Port Output timer and captions to Rust (`output-timer.hpp`)                             | UI bookend passes                                                              | open                                                       |
| **E51S5** P6-40 DeckLink/AJA output UIs   | Port DeckLink/AJA output UIs to Rust (`DecklinkOutputUI.c`, `DecklinkOutputUI.h`)       | UI bookend passes                                                              | open                                                       |
| **E51S6** P6-41 Tray and platform bits    | Port Tray and platform bits to Rust (`OBSBasic_SysTray.c`, `OBSApp.c`)                  | tray works per OS                                                              | open                                                       |
