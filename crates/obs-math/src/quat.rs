//! `quat` kernel (S-008 R10, R14; `libobs/graphics/quat.{h,c}`).
//!
//! This WP ships only what the matrix composition helpers need
//! (R34–R36): the type, `quat_identity`, `quat_dot` and
//! `quat_from_axisang`. The full quaternion set (R11–R13, R15–R21:
//! mul, from_matrix4, look-dir, log/exp, interpolation) arrives with
//! P1-17.

use crate::axisang::AxisAng;

/// S-008 §3: the C `struct quat` — lanes `[x, y, z, w]` with `w` the
/// scalar part.
#[repr(C, align(16))]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Quat {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub w: f32,
}

/// S-008-R10: `quat_identity() = (0,0,0,1)`.
#[must_use]
pub const fn quat_identity() -> Quat {
    Quat {
        x: 0.0,
        y: 0.0,
        z: 0.0,
        w: 1.0,
    }
}

/// S-008-R10: `quat_dot` sums the four products.
#[must_use]
pub fn quat_dot(a: &Quat, b: &Quat) -> f32 {
    a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w
}

/// S-008-R14: `quat_from_axisang(d, aa)`: `s = sin(aa.w/2)`;
/// `d = (aa.x·s, aa.y·s, aa.z·s, cos(aa.w/2))`. The axis is **not**
/// normalised.
pub fn quat_from_axisang(d: &mut Quat, aa: &AxisAng) {
    let (s, c) = (aa.w / 2.0).sin_cos();
    d.x = aa.x * s;
    d.y = aa.y * s;
    d.z = aa.z * s;
    d.w = c;
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
        use std::mem::{align_of, size_of};
        assert_eq!(size_of::<Quat>(), 16);
        assert_eq!(align_of::<Quat>(), 16);
    }

    #[test]
    fn identity_and_dot() {
        // S-008-R10
        assert_eq!(
            quat_identity(),
            Quat {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                w: 1.0
            }
        );
        let q = Quat {
            x: 1.0,
            y: 2.0,
            z: 3.0,
            w: 4.0,
        };
        assert!(within(quat_dot(&q, &q), 30.0));
    }

    #[test]
    fn from_axisang_half_angle() {
        // S-008-R14: 90° about +Z
        let mut q = quat_identity();
        quat_from_axisang(
            &mut q,
            &crate::axisang::axisang_set(0.0, 0.0, 1.0, std::f32::consts::FRAC_PI_2),
        );
        assert!(within(q.x, 0.0));
        assert!(within(q.y, 0.0));
        // exact expectation: s = sin(pi/4), c = cos(pi/4)
        let s = (std::f32::consts::FRAC_PI_4).sin();
        let c = (std::f32::consts::FRAC_PI_4).cos();
        assert!(within(q.z, s));
        assert!(within(q.w, c));
        // Axis is not normalised: a double-length axis doubles the vector part
        let mut q2 = quat_identity();
        quat_from_axisang(
            &mut q2,
            &crate::axisang::axisang_set(0.0, 0.0, 2.0, std::f32::consts::FRAC_PI_2),
        );
        assert!(within(q2.z, 2.0 * s));
    }
}
