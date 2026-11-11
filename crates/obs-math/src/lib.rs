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
pub mod math_extra;
pub mod matrix3;
pub mod matrix4;
pub mod plane;
pub mod quat;
pub mod vec2;
pub mod vec3;
pub mod vec4;

pub use axisang::*;
pub use math_extra::*;
pub use matrix3::*;
pub use matrix4::*;
pub use plane::*;
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

/// S-009-R01: `M_INFINITE` — a finite sentinel, not `+inf`.
pub const M_INFINITE: f32 = 3.4e38;

/// S-009-R01: `M_PI` as the `f32` literal the header falls back to (the
/// platform `f64` `M_PI` is `std::f64::consts::PI`).
// The exact decimal literals are pinned by S-009-R01 (C header values).
#[allow(clippy::excessive_precision)]
pub const M_PI_F32: f32 = 3.14159274;

/// S-009-R01: the `RAD` macro — product with the `f32` constant.
#[must_use]
#[allow(clippy::excessive_precision)]
pub const fn rad(v: f32) -> f32 {
    v * 0.0174532924
}

/// S-009-R01: the `DEG` macro — product with the `f32` constant.
#[must_use]
#[allow(clippy::excessive_precision)]
pub const fn deg(v: f32) -> f32 {
    v * 57.2957802
}

/// S-008 §2: `close_float(a, b, p) = |a - b| <= p`.
#[must_use]
pub fn close_float(a: f32, b: f32, p: f32) -> bool {
    (a - b).abs() <= p
}
