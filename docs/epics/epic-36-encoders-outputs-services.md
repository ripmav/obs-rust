# E36 — Encoders, outputs, services

**Milestone:** M6 — Phase 4: core object model (section D)
**Subtasks:** [phase-4-subtasks.md](../rust-rewrite/subtasks/phase-4-subtasks.md) § D

**Why:** the encoder/output/service objects and their C ABI.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E36S1** P4-21 Encoder vtable adapter | Port Encoder vtable adapter to Rust (`obs-encoder.h`, `obs-encoder.c`) | spec S-041, S-035; vectors: modservice (24-register-other-kinds), encoutput (enc_create); ABI test encoder works | open |
| **E36S2** P4-22 Encoder lifecycle and signals | Port Encoder lifecycle and signals to Rust (`obs-encoder.c`) | spec S-041; vectors: encoutput (enc_create, enc_media_config, out_start_failures); state diagram test | open |
| **E36S3** P4-23 Output vtable adapter | Port Output vtable adapter to Rust (`obs-output.h`, `obs-output.c`) | spec S-041, S-035; vectors: modservice (24-register-other-kinds), encoutput…; ABI test output works | open |
| **E36S4** P4-24 Output flags and multi-track | Port Output flags and multi-track to Rust (`obs-output.h`) | spec S-041, S-035; vectors: encoutput (il_multitrack_video, il_multitrack_audio); each tested | open |
| **E36S5** P4-25 Service API | Port Service API to Rust (`obs-service.h`, `obs-service.c`) | spec S-035; vectors: modservice (01-service-register, 02-service-create-update,…; ABI test service | open |
| **E36S6** P4-26 Multitrack video plumbing | Port Multitrack video plumbing to Rust (`obs-service.h`) | spec S-035, S-041; vectors: modservice (05-service-queries); all values | open |
| **E36S7** P4-27 Output state machine tests | Port Output state machine tests to Rust (`obs-output.c`, `obs-internal.h`) | spec S-041; vectors: encoutput (out_start_stop, rc_success, out_signal_stop,…; table lists every legal transition | open |
