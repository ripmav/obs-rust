# E26 — Packet queues and delay

**Milestone:** M4 — Phase 2: media pipeline (section E)
**Subtasks:** [phase-2-subtasks.md](../rust-rewrite/subtasks/phase-2-subtasks.md) § E

**Why:** packet queues and delay handling bridge the parsers and the media IO.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E26S1** P2-22 Encoder packet queue | Port Encoder packet queue to Rust (`obs-encoder.c`, `obs-encoder.h`) | spec S-041; vectors: nal (layout command — offsets of struct encoder_packet),…; ABI layout test | open |
| **E26S2** P2-23 Output delay buffer | Port Output delay buffer to Rust (`obs-output-delay.c`) | spec S-041; vectors: encoutput dl_start, dl_stop_drop, dl_force_stop; delay of N seconds measured | open |
| **E26S3** P2-24 Interleaving | Port Interleaving to Rust (`obs-output.c`) | spec S-041; vectors: encoutput il_start_basic, il_offsets, il_steady_state,…; trace equal | open |
