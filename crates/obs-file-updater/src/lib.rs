//! Small S-024 M10 helper modules of the OBS Rust rewrite (crate split per
//! S-024 §1 — a suggestion).
//!
//! - `file_updater`: the pure helpers of `shared/file-updater` (P1-33.2):
//!   `get_path` and the ETag header-line cleaning. The update algorithm
//!   itself (threads, HTTP, `obs_data`) is bridged in `obs-ffi`
//!   (`abi_file_updater`) on top of the C services still linked in
//!   Phase 1.

pub mod file_updater;
