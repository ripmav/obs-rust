# E27 — Integration

**Milestone:** M4 — Phase 2: media pipeline (section F)
**Subtasks:** [phase-2-subtasks.md](../rust-rewrite/subtasks/phase-2-subtasks.md) § F

**Why:** the Phase 2 modules entangled with Phase 4 objects (fake inputs in Phase 2, real integration in Phase 4).

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E27S1** P2-25 Wire into libobs | Port Wire into libobs to Rust | ON/OFF builds | open |
| **E27S2** P2-26 Soak test | Port Soak test to Rust | CSV produced for the full run | open |
| **E27S3** P2-27 Latency | Port Latency to Rust (`sync-pair-aud.c`, `sync-pair-vid.c`) | flash+beep source built into the test host | open |
| **E27S4** P2-28 Phase 2 review | Port Phase 2 review to Rust | review notes committed | open |
| **E27S5** P2-29 `media-remux` (added) | Port `media-remux` (added) to Rust (`media-remux.c`, `media-remux.h`) | output equals C by ffprobe JSON | open |
