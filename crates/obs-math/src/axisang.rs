//! `axisang` basics (S-008-R44; `libobs/graphics/axisang.h`).
//!
//! Layout: four `f32` lanes `[x, y, z, w]`, 16 bytes, 16-byte aligned,
//! with `w` the **angle in radians**. `axisang_from_quat` (R15) landed
//! with WP-43 (P1-17.4), which swaps `axisang.c` into the Rust staticlib.

use crate::{EPSILON, close_float};

/// S-008 §3: the C `struct axisang`.
#[repr(C, align(16))]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct AxisAng {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    /// Angle in radians.
    pub w: f32,
}

/// S-008-R44: `axisang_zero()`.
#[must_use]
pub const fn axisang_zero() -> AxisAng {
    AxisAng {
        x: 0.0,
        y: 0.0,
        z: 0.0,
        w: 0.0,
    }
}

/// S-008-R44: `axisang_set(x, y, z, angle)`.
#[must_use]
pub const fn axisang_set(x: f32, y: f32, z: f32, angle: f32) -> AxisAng {
    AxisAng { x, y, z, w: angle }
}

/// S-008-R44: `axisang_copy(d, v)`.
pub fn axisang_copy(d: &mut AxisAng, v: &AxisAng) {
    *d = *v;
}

/// S-008-R15: `axisang_from_quat(d, q)` — `len = x²+y²+z²`; if `len`
/// is not close to zero: `d.xyz = q.xyz / sqrt(len)`,
/// `d.w = 2·acos(q.w)`; else `(0,0,0,0)`.
pub fn axisang_from_quat(d: &mut AxisAng, q: &crate::quat::Quat) {
    let len = q.x * q.x + q.y * q.y + q.z * q.z;
    if !close_float(len, 0.0, EPSILON) {
        let leni = 1.0 / len.sqrt();
        d.x = q.x * leni;
        d.y = q.y * leni;
        d.z = q.z * leni;
        d.w = q.w.acos() * 2.0;
    } else {
        *d = axisang_zero();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn within(a: f32, b: f32) -> bool {
        let d = (a - b).abs();
        d <= 1e-6 || d <= 1e-6 * a.abs().max(b.abs())
    }

    #[test]
    fn layout() {
        use std::mem::{align_of, offset_of, size_of};
        assert_eq!(size_of::<AxisAng>(), 16);
        assert_eq!(align_of::<AxisAng>(), 16);
        assert_eq!(offset_of!(AxisAng, w), 12);
    }

    #[test]
    fn set_zero_copy() {
        let a = axisang_set(0.0, 0.0, 1.0, std::f32::consts::FRAC_PI_2);
        assert_eq!(a.w, std::f32::consts::FRAC_PI_2);
        assert_eq!(
            axisang_zero(),
            AxisAng {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                w: 0.0
            }
        );
        let mut c = axisang_zero();
        axisang_copy(&mut c, &a);
        assert_eq!(c, a);
    }

    #[test]
    fn from_quat_roundtrip() {
        // S-008-R15 (WP-43 regression): axisang_from_quat inverts
        // quat_from_axisang for a normalised axis.
        let aa = axisang_set(0.0, 0.0, 1.0, std::f32::consts::FRAC_PI_2);
        let mut q = crate::quat::quat_identity();
        crate::quat::quat_from_axisang(&mut q, &aa);
        let mut back = axisang_zero();
        axisang_from_quat(&mut back, &q);
        assert!(within(back.x, aa.x));
        assert!(within(back.y, aa.y));
        assert!(within(back.z, aa.z));
        assert!(within(back.w, aa.w));
        // zero vector part -> (0,0,0,0)
        let mut zero = axisang_zero();
        axisang_from_quat(&mut zero, &crate::quat::quat_identity());
        assert_eq!(zero, axisang_zero());
    }
}
