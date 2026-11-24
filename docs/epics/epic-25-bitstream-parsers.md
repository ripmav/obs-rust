# E25 — Bitstream parsers

**Milestone:** M4 — Phase 2: media pipeline (section D)
**Subtasks:** [phase-2-subtasks.md](../rust-rewrite/subtasks/phase-2-subtasks.md) § D

**Why:** NAL/OBU parsers (AVC/HEVC/AV1) are pure parsing logic — the early memory-safety win the plan aims for first.

| Story                            | I want                                                   | Acceptance                                                                      | Status |
| -------------------------------- | -------------------------------------------------------- | ------------------------------------------------------------------------------- | ------ |
| **E25S1** P2-17 NAL / start-code | Port NAL / start-code to Rust (`obs-nal.c`, `obs-nal.h`) | spec S-012; vectors: nal (19 scripts; find command); vectors                    | open   |
| **E25S2** P2-18 AVC              | Port AVC to Rust (`obs-avc.c`, `obs-avc.h`)              | spec S-012; vectors: nal avc_keyframe; samples                                  | open   |
| **E25S3** P2-19 HEVC             | Port HEVC to Rust (`obs-hevc.c`, `obs-hevc.h`)           | spec S-012; vectors: nal hevc_extract; equal on a sample                        | open   |
| **E25S4** P2-20 AV1              | Port AV1 to Rust (`obs-av1.c`, `obs-av1.h`)              | spec S-012; vectors: nal av1_keyframe, av1_obu, av1_leb128; header types parsed | open   |
| **E25S5** P2-21 Caption support  | Port Caption support to Rust                             | Rust decode equals C on the vector set                                          | open   |
