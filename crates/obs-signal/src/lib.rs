//! obs-signal: the `libobs/callback` ports (Phase 1) — calldata (S-002),
//! the declaration language (S-003), and signal/proc handlers (S-004),
//! plus the typed Rust API (P1-23).
//!
//! - [`calldata`]: the typed-less parameter stack — byte-exact S-002
//!   layout (the C struct and the three exported functions).
//! - [`decl`]: the tiny C-like declaration language (S-003).
//! - [`signal`] / [`proc`]: the S-004 handler layer with C-faithful
//!   emit semantics (re-entrancy quirks Q-1…Q-7 included).
//! - [`typed`]: the `Signal<A>` + `Connection` guard that plugin code
//!   uses — typed arguments over the calldata byte layout, RAII
//!   disconnect on drop (ADR 0002 Amendment 1). Allocation behaviour
//!   of the emit path is documented there (tenet T6).

pub mod calldata;
pub mod decl;
pub mod proc;
pub mod signal;
pub mod typed;
