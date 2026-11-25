# abi_bpm (S-024 M8, P1-33.1) — test matrix

Test files: `crates/obs-ffi/src/abi_bpm.rs`
Issue: WP-34 (P1-33.1)

C-ABI bridge for `shared/bpm` in `crates/obs-ffi` (S-024 M8):
`bpm_inject` / `bpm_destroy` exports over the `obs-bpm` builders, with
the per-`(output, track)` state machine (`Mutex<Vec<OutputLink>>`),
`update_metrics` (counters + timestamp validity) and `process_metrics`
(per-codec packet replacement; returns `false` for unknown codecs and
the R56 no-start-code HEVC case, mirroring the C `return avc || hevc || av1`). `#[repr(C)]` mirrors of `encoder_packet` (88 bytes) and
`encoder_packet_time` (40 bytes); the C encoder/output services are
excluded cbindgen imports (they stay linked C in Phase 1).

| Area                                                                                                                                                                                                                                                                                                           | Pinned by test       | Conformance vector                                                                                                                                                                                           |
| -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| **mirror layout vs the C reference**                                                                                                                                                                                                                                                                           |                      |                                                                                                                                                                                                              |
| `encoder_packet`: 88 bytes, `data` 0 … `encoder` 80; `encoder_packet_time`: 40 bytes, `pts` 0 … `pir` 32 (a drift breaks every `bpm-*` swap script)                                                                                                                                                            | `layout_pins`        | the `bpm-*` swap run                                                                                                                                                                                         |
| **full BPM flow (M8, R54–R59)**                                                                                                                                                                                                                                                                                |                      |                                                                                                                                                                                                              |
| per-codec injection (h264/hevc/av1), counter diff log lines (`%d` signed rendering of `u32` diffs), timestamp/SEI rendering, R56 (HEVC without Annex-B start code: packet unchanged + warn log, counters advance) and R57 (unknown codec: packet copied unchanged, counters advance, `bpm_inject` failure log) | conformance swap run | `helpers/bpm-avc-basic`, `helpers/bpm-hevc`, `helpers/bpm-av1`, `helpers/bpm-other-codec`, `helpers/bpm-counters`, `helpers/bpm-timestamps`, `helpers/bpm-tracks`, `helpers/bpm-novideo`, `helpers/bpm-skip` |
