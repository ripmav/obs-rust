# E39 — Pure logic

**Milestone:** M7 — Phase 5: plugins (section A)
**Subtasks:** [phase-5-subtasks.md](../rust-rewrite/subtasks/phase-5-subtasks.md) § A

**Why:** the lowest-risk plugins first: filters, transitions, text, image, rtmp-services, x264.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E39S1** P5-01 `obs-filters` | Port `obs-filters` to Rust (`obs-filters.c`) | spec S-028, S-027, S-054; vectors: videofilters/; docs/tests/plugin-obs-filters-inventory.md committed; S2 passes on every corpus file using a filter | open |
| **E39S2** P5-02 `obs-transitions` | Port `obs-transitions` to Rust (`obs-transitions.c`) | spec S-029; vectors: transitions/; docs/tests/plugin-obs-transitions-inventory.md committed; S2 passes | open |
| **E39S3** P5-03 `image-source` | Port `image-source` to Rust (`image-source.c`) | spec S-030; vectors: imagesource/; docs/tests/plugin-image-source-inventory.md committed; S2 passes | open |
| **E39S4** P5-04 `obs-text` (Windows GDI+) | Port `obs-text` (Windows GDI+) to Rust (`obs-text.c`) | docs/tests/plugin-obs-text-inventory.md committed; S2 passes | open |
| **E39S5** P5-05 `text-freetype2` | Port `text-freetype2` to Rust (`text-freetype2.c`) | spec S-038; vectors: textft2/; docs/tests/plugin-text-freetype2-inventory.md committed; S2 passes | open |
| **E39S6** P5-06 `rtmp-services` | Port `rtmp-services` to Rust (`rtmp-services-main.c`) | spec S-026; vectors: rtmpservices/; docs/tests/plugin-rtmp-services-inventory.md committed; S2 passes | open |
| **E39S7** P5-07 `obs-x264` | Port `obs-x264` to Rust (`obs-x264-plugin-main.c`) | spec S-025, S-012; vectors: encopts/; docs/tests/plugin-obs-x264-inventory.md committed; S2 passes | open |
| **E39S8** P5-08 `obs-libfdk` | Port `obs-libfdk` to Rust (`obs-libfdk.c`) | spec S-025; vectors: encopts/; docs/tests/plugin-obs-libfdk-inventory.md committed; S2 passes | open |
| **E39S9** P5-09 `coreaudio-encoder` | Port `coreaudio-encoder` to Rust (`encoder.c`, `windows-imports.h`) | docs/tests/plugin-coreaudio-encoder-inventory.md committed; S2 passes | open |
| **E39S10** P5-10 `nv-filters` | Port `nv-filters` to Rust (`nv-filters.c`) | docs/tests/plugin-nv-filters-inventory.md committed; S2 passes | open |
