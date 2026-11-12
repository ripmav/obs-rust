//! Small S-024 helper modules of the OBS Rust rewrite (crate split per
//! S-024 §1 — a suggestion; M3 and the S-023/S-055 platform helpers
//! landed in `obs-util` per the P1-11 subtasks).
//!
//! - `uint128`: S-024 M2 (`util_uint128.h` / `util_uint64.h`)
//! - `task_queue`: S-024 M6 (`util/task.c`)

pub mod task_queue;
pub mod uint128;
