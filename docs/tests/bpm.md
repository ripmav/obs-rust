# bpm pure framing (S-024 M8, P1-33.1) — test matrix

Test files: `crates/obs-bpm/src/bpm.rs`
Issue: WP-34 (P1-33.1)

Pure BPM framing logic of `shared/bpm` in `crates/obs-bpm` (S-024 §4.8,
M8): SEI emulation prevention, AVC SEI NAL framing, AV1 `metadata_obu`
framing (inlined from S-012), the TS/SM/ERM payload builders, ULEB128
and the RFC3339 rendering. Counters, timestamp validity and the
per-`(output, track)` state machine live in the bridge
(`obs_ffi::abi_bpm`). The wall-clock base is injected by the linked
`os_nstime_to_timespec` (the conformance runner links the identity
model `wall(ns) = ns`; production links libobs).

| Area                                                                                                                                                                                                                           | Pinned by test        | Conformance vector                                                                        |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | --------------------- | ----------------------------------------------------------------------------------------- |
| **RFC3339 rendering (R58/R59)**                                                                                                                                                                                                |                       |                                                                                           |
| civil-from-days rendering of the converted `(sec, nsec)` pair, second-boundary and leap-second-free edge values, cross-checked against `gmtime`                                                                                | `rfc3339_vectors`     | `helpers/bpm-timestamps`                                                                  |
| **emulation prevention (R54)**                                                                                                                                                                                                 |                       |                                                                                           |
| `03` inserted before every byte `<= 03` preceded by two `00`s; zero-run counter restarts after the inserted byte                                                                                                               | `escape_vector_bytes` | `helpers/bpm-avc-basic`, `helpers/bpm-hevc`                                               |
| **SM payload**                                                                                                                                                                                                                 |                       |                                                                                           |
| session-metrics SEI payload bytes (UUID + event tag + fields) match the C reference                                                                                                                                            | `sm_payload_vector`   | `helpers/bpm-counters`                                                                    |
| **TS payload**                                                                                                                                                                                                                 |                       |                                                                                           |
| timestamp SEI payload bytes per event (CTS/FER/FERC/PIR)                                                                                                                                                                       | `ts_payload_vector`   | `helpers/bpm-timestamps`                                                                  |
| **AV1 `metadata_obu` framing**                                                                                                                                                                                                 |                       |                                                                                           |
| OBU header (type 6) + size fields per the S-012 formula                                                                                                                                                                        | `av1_obu_vector`      | `helpers/bpm-av1`                                                                         |
| **per-codec packet injection**                                                                                                                                                                                                 |                       |                                                                                           |
| `frame_packet` for h264 (SEI NAL prefix), hevc (Annex-B start-code handling, R56: no start code → packet unchanged but counters advance), av1 (metadata OBU insertion), unknown codec (R57: bytes unchanged, counters advance) | `frame_packet_codecs` | `helpers/bpm-avc-basic`, `helpers/bpm-hevc`, `helpers/bpm-av1`, `helpers/bpm-other-codec` |
