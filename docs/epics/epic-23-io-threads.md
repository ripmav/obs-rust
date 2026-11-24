# E23 — IO threads

**Milestone:** M4 — Phase 2: media pipeline (section B)
**Subtasks:** [phase-2-subtasks.md](../rust-rewrite/subtasks/phase-2-subtasks.md) § B

**Why:** the video/audio IO threads are the real-time heart of Phase 2 (T6).

| Story                            | I want                                                    | Acceptance                                                                                                   | Status |
| -------------------------------- | --------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------ | ------ |
| **E23S1** P2-08 `audio-io` core  | Port `audio-io` core to Rust (`audio-io.c`, `audio-io.h`) | spec S-044; vectors: avio (26 scripts, lock-step fake clock); simulated 60 s run produces exact sample count | open   |
| **E23S2** P2-09 `video-io` core  | Port `video-io` core to Rust (`video-io.c`, `video-io.h`) | spec S-044; vectors: avio (26 scripts); ring semantic test                                                   | open   |
| **E23S3** P2-10 Timing and clock | Port Timing and clock to Rust (`obs-video.c`)             | spec S-044, S-058; vectors: obsvideo (23 scripts); all timing code takes &dyn Clock                          | open   |
| **E23S4** P2-11 Lock-free queues | Port Lock-free queues to Rust (`deque.h`)                 | ADR note                                                                                                     | open   |
| **E23S5** P2-12 Real-time audit  | Port Real-time audit to Rust                              | spec S-044; demonstrates failing on a deliberate alloc                                                       | open   |
