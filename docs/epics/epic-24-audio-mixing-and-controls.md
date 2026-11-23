# E24 — Audio mixing and controls

**Milestone:** M4 — Phase 2: media pipeline (section C)
**Subtasks:** [phase-2-subtasks.md](../rust-rewrite/subtasks/phase-2-subtasks.md) § C

**Why:** the mixer, resampler and audio controls (meters/faders) replace the audio core.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E24S1** P2-13 Audio mixer | Port Audio mixer to Rust (`obs-audio.c`) | spec S-032; vectors: audiomix (39 scripts); add/consume tests | open |
| **E24S2** P2-14 Fader and meter | Port Fader and meter to Rust (`obs-audio-controls.c`) | spec S-032; vectors: audiomix (fader scripts, e.g. 41_fader_curve_iec); 1001-point golden | open |
| **E24S3** P2-15 Audio monitoring | Port Audio monitoring to Rust (`obs.h`, `obs-internal.h`) | spec S-055; vectors: utilrest (26 scripts); documented | open |
| **E24S4** P2-16 Audio volume/pan/mono kernels | Port Audio volume/pan/mono kernels to Rust (`obs-source.c`) | golden on stereo and 5.1 buffers | open |
