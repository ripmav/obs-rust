# E43 — Vendor SDK cards and tools

**Milestone:** M7 — Phase 5: plugins (section E)
**Subtasks:** [phase-5-subtasks.md](../rust-rewrite/subtasks/phase-5-subtasks.md) § E

**Why:** DeckLink/AJA/VST/libfdk/vlc: Rust glue over hand-written FFI; the vendor SDKs stay (T9).

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E43S1** P5-42 DeckLink | Port DeckLink to Rust (`platform.hpp`) | table committed, library loads without hardware | open |
| **E43S2** P5-43 AJA | Port AJA to Rust (`aja-card-manager.hpp`, `aja-common.hpp`) | table committed, NTV2 session inits on a card | open |
| **E43S3** P5-44 `frontend-tools` | Port `frontend-tools` to Rust (`scripts.hpp`, `output-timer.hpp`) | the Phase 6 file owns every file | open |
