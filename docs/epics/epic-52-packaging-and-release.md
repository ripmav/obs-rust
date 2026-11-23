# E52 — Packaging and release

**Milestone:** M8 — Phase 6: frontend (section G)
**Subtasks:** [phase-6-subtasks.md](../rust-rewrite/subtasks/phase-6-subtasks.md) § G

**Why:** Flatpak, MSIX (D-39), notarized macOS and the update channel.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E52S1** P6-42 Windows | Port Windows to Rust (`windows-msix-spike.m`) | docs/tests/windows-msix-spike.md records a GO/NO-GO decision | open |
| **E52S2** P6-43 macOS | Port macOS to Rust (`qt.c`) | bundle launches | open |
| **E52S3** P6-44 Linux | Port Linux to Rust | flatpak-builder runs the manifest | open |
| **E52S4** P6-45 Size and startup | Port Size and startup to Rust | numbers recorded vs the C build | open |
| **E52S5** P6-46 End-to-end UI tests | Port End-to-end UI tests to Rust | one journey passes per OS | open |
| **E52S6** P6-47 Beta programme | Port Beta programme to Rust | beta published | open |
| **E52S7** P6-48 Phase 6 review | Port Phase 6 review to Rust | checklist committed | open |
| **E52S8** P6-49 Frontend components sweep (added) | Port Frontend components sweep (added) to Rust (`ClickableLabel.hpp`, `DelButton.hpp`) | spec S-020; port order documented | open |
| **E52S9** P6-50 Frontend spec series (added) | Port Frontend spec series (added) to Rust (`OBSBasicSettings.c`) | S-068 committed; all 10 files cited | done |
