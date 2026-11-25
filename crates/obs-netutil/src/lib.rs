//! Small S-024 M9 helper modules of the OBS Rust rewrite (crate split per
//! S-024 §1 — a suggestion).
//!
//! - `happy_eyeballs`: S-024 M9 (`shared/happy-eyeballs`, P1-33.3) — the
//!   address-list builder (resolver hints + in-place reordering) and the
//!   happy-eyeballs constants. The connection race (sockets, timers,
//!   threads) is out of scope (S-024 §1) and stays behind the C file until
//!   it is specified.

pub mod happy_eyeballs;
