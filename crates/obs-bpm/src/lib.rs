//! `obs-bpm` — S-024 M8 (WP-34, P1-33.1).
//!
//! The pure logic of `shared/bpm`: the three BPM SEI payloads (timestamp,
//! session metrics, encoded-rendition metrics), the codec framing (AVC SEI
//! NALs, HEVC SEI NALs, AV1 metadata OBUs) and the RFC3339 rendering of
//! metric timestamps. The C-ABI state machine (`bpm_inject` /
//! `bpm_destroy`) is bridged in `obs_ffi::abi_bpm`.

pub mod bpm;

pub use bpm::*;
