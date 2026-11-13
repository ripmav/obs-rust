//! Core math types and functions (spec S-008).
//!
//! Phase 1 WP-5 (P1-15/P1-16): `vec2`, `vec3`, `vec4`, `matrix3`, `matrix4`,
//! plus the minimal `quat`/`axisang` kernel that `matrix4_from_quat`,
//! `matrix4_rotate` and friends need (R10 identity/dot, R14 from_axisang,
//! R44 axisang basics). The full quaternion, plane and bounds sets arrive
//! with P1-17 (S-008 R11–R21, R42–R43) and P1-18 (S-009).
//!
//! Layout and behaviour reproduce the C reference exactly, including the
//! documented defects (Q-1 vec3 `w` lane, Q-2 signed `*_close`); see
//! `docs/rust-rewrite/specs/S-008-vec-quat-matrix4.md`.

pub mod axisang;
pub mod matrix3;
pub mod matrix4;
pub mod quat;
pub mod vec2;
pub mod vec3;
pub mod vec4;

pub use axisang::*;
pub use matrix3::*;
pub use matrix4::*;
pub use quat::*;
pub use vec2::*;
pub use vec3::*;
pub use vec4::*;

/// S-008 §2: `EPSILON = 1e-4`.
pub const EPSILON: f32 = 1e-4;
/// S-008 §2: `LARGE_EPSILON = 1e-2`.
pub const LARGE_EPSILON: f32 = 1e-2;
/// S-008 §2: `TINY_EPSILON = 1e-5`.
pub const TINY_EPSILON: f32 = 1e-5;

/// S-008 §2: `close_float(a, b, p) = |a - b| <= p`.
#[must_use]
pub fn close_float(a: f32, b: f32, p: f32) -> bool {
    (a - b).abs() <= p
}
