# E42 — Hardware encoders

**Milestone:** M7 — Phase 5: plugins (section D)
**Subtasks:** [phase-5-subtasks.md](../rust-rewrite/subtasks/phase-5-subtasks.md) § D

**Why:** thin vendor-SDK wrappers (NVENC/QSV/AMF/VideoToolbox): ported last among plugins — high risk, low gain.

| Story                       | I want                                               | Acceptance                                                                                             | Status |
| --------------------------- | ---------------------------------------------------- | ------------------------------------------------------------------------------------------------------ | ------ |
| **E42S1** P5-39 `obs-nvenc` | Port `obs-nvenc` to Rust (`obs-nvenc.c`)             | spec S-050, S-025; vectors: hwencoders/; docs/tests/plugin-obs-nvenc-inventory.md committed; S2 passes | open   |
| **E42S2** P5-40 `obs-qsv11` | Port `obs-qsv11` to Rust (`obs-qsv11-plugin-main.c`) | spec S-050, S-025; vectors: encopts/; docs/tests/plugin-obs-qsv11-inventory.md committed; S2 passes    | open   |
| **E42S3** P5-41 AMF         | Port AMF to Rust (`texture-amf.c`)                   | spec S-050; vectors: none (AMF not vectored); inventory rows committed; S2 passes                      | open   |
