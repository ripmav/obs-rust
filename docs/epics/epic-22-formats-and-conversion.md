# E22 — Formats and conversion

**Milestone:** M4 — Phase 2: media pipeline (section A)
**Subtasks:** [phase-2-subtasks.md](../rust-rewrite/subtasks/phase-2-subtasks.md) § A

**Why:** the media-pipeline conversion core: enums, matrices, scalers and frames must be bit-exact vs C before the IO threads can be built on them.

| Story                                            | I want                                                                            | Acceptance                                                                                                                                    | Status |
| ------------------------------------------------ | --------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------- | ------ |
| **E22S1** P2-01 Format enums and helpers         | Port Format enums and helpers to Rust (`video-io.h`, `audio-io.h`)                | spec S-013; vectors: mediaconv/ (27 scripts); numeric values asserted equal to the C headers via a generated test (mediaconv script 01 green) | open   |
| **E22S2** P2-02 Colour matrices                  | Port Colour matrices to Rust (`video-matrices.c`)                                 | spec S-013; vectors: mediaconv vparams (scripts 30, 33); all 54 tables equal C bit for bit (keep the C f32 operation order, S-013-R53)        | open   |
| **E22S3** P2-03 `video-scaler` and `video-frame` | Port `video-scaler` and `video-frame` to Rust (`video-scaler.h`)                  | spec S-044, S-013; vectors: avio (S-044) drives the Scaler trait with logging fakes;…; documented                                             | open   |
| **E22S4** P2-04 SIMD pixel converters            | Port SIMD pixel converters to Rust (`format-conversion.c`, `format-conversion.h`) | spec S-013; vectors: mediaconv compress (scripts 11, 13, 14); byte-exact vs C                                                                 | open   |
| **E22S5** P2-05 HDR helpers                      | Port HDR helpers to Rust (`obs.c`)                                                | spec S-053; 256-point golden                                                                                                                  | open   |
| **E22S6** P2-06 Audio resampler                  | Port Audio resampler to Rust (`audio-resampler.h`)                                | spec S-032; vectors: avio (S-044) drives the trait with fakes; the swresample…; documented                                                    | open   |
| **E22S7** P2-07 Audio maths                      | Port Audio maths to Rust (`audio-math.h`)                                         | spec S-013; vectors: mediaconv mul_to_db, db_to_mul; table + edge-value golden                                                                | open   |
