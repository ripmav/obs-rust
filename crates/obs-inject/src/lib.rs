//! `obs-inject` — P1-33.4 (WP-34).
//!
//! The portable core of `shared/obs-inject-library` (no spec: ported from
//! the C, per the subtask). The Windows API calls are injectable
//! callables, so the obfuscated **symbol resolution** and the injection
//! **failure paths** (the exact error-code mapping and cleanup ordering of
//! the reference) are unit-tested cross-platform. The real WinAPI
//! bindings (and the C source exclusion) land with `plugins/win-capture`
//! (P5-31.6/P5-31.7); until then the C file stays linked.

pub mod inject;

pub use inject::*;
