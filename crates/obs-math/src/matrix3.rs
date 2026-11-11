//! `matrix3` (S-008-R41; `libobs/graphics/matrix3.{h,c}`).
//!
//! Layout: three `vec4` **rows** `x, y, z` (48 bytes, 16-byte aligned)
//! — the same element layout as `matrix4` without the `t` row. Points
//! are row vectors: `v' = v · M`.
//!
//! **Row `w` lane**: the third row is absent, so the affine
//! translation is carried in the `w` lane of each row with an implicit
//! fourth row `(0,0,0,1)`; `matrix3_mul` composes accordingly. This
//! reading is not pinned by S-008 (the spec only fixes the layout) and
//! must be confirmed against the C source in the conformance substep.

use crate::axisang::AxisAng;
use crate::quat::{Quat, quat_from_axisang};
use crate::vec3::Vec3;
use crate::vec4::Vec4;

/// S-008 §3/R41: the C `struct matrix3`.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Matrix3 {
    pub x: Vec4,
    pub y: Vec4,
    pub z: Vec4,
}

/// Element `b` of row `a` (0=x, 1=y, 2=z).
#[inline]
fn el(m: &Matrix3, a: usize, b: usize) -> f32 {
    match (a, b) {
        (0, 0) => m.x.x,
        (0, 1) => m.x.y,
        (0, 2) => m.x.z,
        (1, 0) => m.y.x,
        (1, 1) => m.y.y,
        (1, 2) => m.y.z,
        _ => match b {
            0 => m.z.x,
            1 => m.z.y,
            _ => m.z.z,
        },
    }
}

/// S-008-R41: `matrix3_identity()` — rows `(1,0,0,0)`, `(0,1,0,0)`,
/// `(0,0,1,0)`.
#[must_use]
pub fn matrix3_identity() -> Matrix3 {
    Matrix3 {
        x: Vec4 {
            x: 1.0,
            y: 0.0,
            z: 0.0,
            w: 0.0,
        },
        y: Vec4 {
            x: 0.0,
            y: 1.0,
            z: 0.0,
            w: 0.0,
        },
        z: Vec4 {
            x: 0.0,
            y: 0.0,
            z: 1.0,
            w: 0.0,
        },
    }
}

/// S-008-R41: `matrix3_copy(d, m)`.
pub fn matrix3_copy(d: &mut Matrix3, m: &Matrix3) {
    *d = *m;
}

/// S-008-R41: ordinary 3×3 transpose (the `w` lanes are not part of
/// the 3×3), safe when `d == m`.
pub fn matrix3_transpose(d: &mut Matrix3, m: &Matrix3) {
    let x = m.x;
    let y = m.y;
    let z = m.z;
    d.x = Vec4 {
        x: x.x,
        y: y.x,
        z: z.x,
        w: x.w,
    };
    d.y = Vec4 {
        x: x.y,
        y: y.y,
        z: z.y,
        w: y.w,
    };
    d.z = Vec4 {
        x: x.z,
        y: y.z,
        z: z.z,
        w: z.w,
    };
}

/// S-008-R41: `matrix3_mul(d, m1, m2)` — row-vector product with the
/// implicit fourth row `(0,0,0,1)` (see module docs for the `w`-lane
/// convention). Safe when `d` aliases an input.
pub fn matrix3_mul(d: &mut Matrix3, m1: &Matrix3, m2: &Matrix3) {
    let r1 = [m1.x, m1.y, m1.z];
    let r2 = [m2.x, m2.y, m2.z];
    let out = |r: usize| Vec4 {
        x: r1[r].x * r2[0].x + r1[r].y * r2[1].x + r1[r].z * r2[2].x,
        y: r1[r].x * r2[0].y + r1[r].y * r2[1].y + r1[r].z * r2[2].y,
        z: r1[r].x * r2[0].z + r1[r].y * r2[1].z + r1[r].z * r2[2].z,
        w: r1[r].x * r2[0].w + r1[r].y * r2[1].w + r1[r].z * r2[2].w + r1[r].w,
    };
    let (ox, oy, oz) = (out(0), out(1), out(2));
    d.x = ox;
    d.y = oy;
    d.z = oz;
}

fn det3(m: [f32; 9]) -> f32 {
    m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6])
        + m[2] * (m[3] * m[7] - m[4] * m[6])
}

/// S-008-R41: `matrix3_inv(d, m)` — adjugate/determinant of the 3×3
/// corner with the same `|det| < 0.0005` singularity threshold as
/// `matrix4_inv` (R33). The `w` lanes are copied through.
pub fn matrix3_inv(d: &mut Matrix3, m: &Matrix3) -> bool {
    let a = [
        [el(m, 0, 0), el(m, 0, 1), el(m, 0, 2)],
        [el(m, 1, 0), el(m, 1, 1), el(m, 1, 2)],
        [el(m, 2, 0), el(m, 2, 1), el(m, 2, 2)],
    ];
    let det = det3([
        a[0][0], a[0][1], a[0][2], a[1][0], a[1][1], a[1][2], a[2][0], a[2][1], a[2][2],
    ]);
    if det.abs() < 0.0005 {
        return false;
    }
    // adjugate: d.row j = cofactor column j / det (unrolled)
    let c = |i: usize, j: usize| {
        let (r1, r2) = match i {
            0 => (1, 2),
            1 => (0, 2),
            _ => (0, 1),
        };
        let (c1, c2) = match j {
            0 => (1, 2),
            1 => (0, 2),
            _ => (0, 1),
        };
        a[r1][c1] * a[r2][c2] - a[r1][c2] * a[r2][c1]
    };
    let sign = |i: usize, j: usize| if (i + j).is_multiple_of(2) { 1.0 } else { -1.0 };
    let cof = [
        [
            sign(0, 0) * c(0, 0),
            sign(0, 1) * c(0, 1),
            sign(0, 2) * c(0, 2),
        ],
        [
            sign(1, 0) * c(1, 0),
            sign(1, 1) * c(1, 1),
            sign(1, 2) * c(1, 2),
        ],
        [
            sign(2, 0) * c(2, 0),
            sign(2, 1) * c(2, 1),
            sign(2, 2) * c(2, 2),
        ],
    ];
    d.x = Vec4 {
        x: cof[0][0] / det,
        y: cof[1][0] / det,
        z: cof[2][0] / det,
        w: m.x.w,
    };
    d.y = Vec4 {
        x: cof[0][1] / det,
        y: cof[1][1] / det,
        z: cof[2][1] / det,
        w: m.y.w,
    };
    d.z = Vec4 {
        x: cof[0][2] / det,
        y: cof[1][2] / det,
        z: cof[2][2] / det,
        w: m.z.w,
    };
    true
}

/// S-008-R41: `matrix3_from_quat(d, q)` — the 3×3 corner of
/// `matrix4_from_quat` (R34). A zero quaternion yields the identity.
pub fn matrix3_from_quat(d: &mut Matrix3, q: &Quat) {
    let n = crate::quat::quat_dot(q, q);
    let s = if n > 0.0 { 2.0 / n } else { 0.0 };
    let xx = q.x * q.x * s;
    let yy = q.y * q.y * s;
    let zz = q.z * q.z * s;
    let xy = q.x * q.y * s;
    let xz = q.x * q.z * s;
    let yz = q.y * q.z * s;
    let wx = q.w * q.x * s;
    let wy = q.w * q.y * s;
    let wz = q.w * q.z * s;
    d.x = Vec4 {
        x: 1.0 - (yy + zz),
        y: xy + wz,
        z: xz - wy,
        w: 0.0,
    };
    d.y = Vec4 {
        x: xy - wz,
        y: 1.0 - (xx + zz),
        z: yz + wx,
        w: 0.0,
    };
    d.z = Vec4 {
        x: xz + wy,
        y: yz - wx,
        z: 1.0 - (xx + yy),
        w: 0.0,
    };
}

/// S-008-R41: `matrix3_from_axisang = from_quat(from_axisang(aa))`.
pub fn matrix3_from_axisang(d: &mut Matrix3, aa: &AxisAng) {
    let mut q = Quat {
        x: 0.0,
        y: 0.0,
        z: 0.0,
        w: 0.0,
    };
    quat_from_axisang(&mut q, aa);
    matrix3_from_quat(d, &q);
}

/// S-008-R41: `matrix3_from_matrix4(d, m)` — the 3×3 corner of a
/// `matrix4` (rows `x,y,z`, `w` lanes dropped).
pub fn matrix3_from_matrix4(d: &mut Matrix3, m: &crate::matrix4::Matrix4) {
    d.x = m.x;
    d.y = m.y;
    d.z = m.z;
}

/// S-008-R41: `matrix3_rotate(d, m, q)` — `d = m ∘ from_quat(q)`.
pub fn matrix3_rotate(d: &mut Matrix3, m: &Matrix3, q: &Quat) {
    let mut op = matrix3_identity();
    matrix3_from_quat(&mut op, q);
    matrix3_mul(d, m, &op);
}

/// S-008-R41: `matrix3_rotate_aa(d, m, aa)` — `d = m ∘ from_axisang(aa)`.
pub fn matrix3_rotate_aa(d: &mut Matrix3, m: &Matrix3, aa: &AxisAng) {
    let mut op = matrix3_identity();
    matrix3_from_axisang(&mut op, aa);
    matrix3_mul(d, m, &op);
}

/// S-008-R41: `matrix3_scale(d, m, v)` — `d = m ∘ diag(v.x, v.y, v.z)`.
pub fn matrix3_scale(d: &mut Matrix3, m: &Matrix3, v: &Vec3) {
    let mut op = matrix3_identity();
    op.x = Vec4 {
        x: v.x,
        y: 0.0,
        z: 0.0,
        w: 0.0,
    };
    op.y = Vec4 {
        x: 0.0,
        y: v.y,
        z: 0.0,
        w: 0.0,
    };
    op.z = Vec4 {
        x: 0.0,
        y: 0.0,
        z: v.z,
        w: 0.0,
    };
    matrix3_mul(d, m, &op);
}

/// S-008-R41 (inline): `matrix3_translate3f(d, m, x, y, z)` —
/// `d = m ∘ T(x,y,z)` with the translation in the row `w` lanes (see
/// module docs for the convention).
pub fn matrix3_translate3f(d: &mut Matrix3, m: &Matrix3, x: f32, y: f32, z: f32) {
    let mut op = matrix3_identity();
    op.x.w = x;
    op.y.w = y;
    op.z.w = z;
    matrix3_mul(d, m, &op);
}

/// S-008-R41 (inline): `matrix3_rotate_aa4f(d, m, x, y, z, angle)`.
pub fn matrix3_rotate_aa4f(d: &mut Matrix3, m: &Matrix3, x: f32, y: f32, z: f32, angle: f32) {
    matrix3_rotate_aa(d, m, &AxisAng { x, y, z, w: angle });
}

/// S-008-R41 (inline): `matrix3_scale3f(d, m, x, y, z)`.
pub fn matrix3_scale3f(d: &mut Matrix3, m: &Matrix3, x: f32, y: f32, z: f32) {
    matrix3_scale(d, m, &Vec3 { x, y, z, w: 0.0 });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn within(a: f32, b: f32) -> bool {
        let d = (a - b).abs();
        d <= 1e-6 || d <= 1e-6 * a.abs().max(b.abs())
    }

    fn scale(s: f32) -> Matrix3 {
        Matrix3 {
            x: Vec4 {
                x: s,
                y: 0.0,
                z: 0.0,
                w: 0.0,
            },
            y: Vec4 {
                x: 0.0,
                y: s,
                z: 0.0,
                w: 0.0,
            },
            z: Vec4 {
                x: 0.0,
                y: 0.0,
                z: s,
                w: 0.0,
            },
        }
    }

    fn translate(x: f32, y: f32, z: f32) -> Matrix3 {
        let mut m = matrix3_identity();
        m.x.w = x;
        m.y.w = y;
        m.z.w = z;
        m
    }

    fn close(a: &Matrix3, b: &Matrix3) -> bool {
        for r in 0..3 {
            for c in 0..4 {
                let av = if c == 3 {
                    match r {
                        0 => a.x.w,
                        1 => a.y.w,
                        _ => a.z.w,
                    }
                } else {
                    el(a, r, c)
                };
                let bv = if c == 3 {
                    match r {
                        0 => b.x.w,
                        1 => b.y.w,
                        _ => b.z.w,
                    }
                } else {
                    el(b, r, c)
                };
                if !within(av, bv) {
                    return false;
                }
            }
        }
        true
    }

    #[test]
    fn layout() {
        use std::mem::{align_of, size_of};
        assert_eq!(size_of::<Matrix3>(), 48);
        assert_eq!(align_of::<Matrix3>(), 16);
    }

    #[test]
    fn identity_transpose_copy() {
        // S-008-R41
        assert_eq!(
            matrix3_identity(),
            Matrix3 {
                x: Vec4 {
                    x: 1.0,
                    y: 0.0,
                    z: 0.0,
                    w: 0.0
                },
                y: Vec4 {
                    x: 0.0,
                    y: 1.0,
                    z: 0.0,
                    w: 0.0
                },
                z: Vec4 {
                    x: 0.0,
                    y: 0.0,
                    z: 1.0,
                    w: 0.0
                }
            }
        );
        let mut d = Matrix3 {
            x: Vec4 {
                x: 1.0,
                y: 2.0,
                z: 3.0,
                w: 0.0,
            },
            y: Vec4 {
                x: 4.0,
                y: 5.0,
                z: 6.0,
                w: 0.0,
            },
            z: Vec4 {
                x: 7.0,
                y: 8.0,
                z: 9.0,
                w: 0.0,
            },
        };
        let m = d;
        matrix3_transpose(&mut d, &m); // alias safe (copy stands in for the alias)
        assert_eq!(
            d.x,
            Vec4 {
                x: 1.0,
                y: 4.0,
                z: 7.0,
                w: 0.0
            }
        );
        assert_eq!(
            d.y,
            Vec4 {
                x: 2.0,
                y: 5.0,
                z: 8.0,
                w: 0.0
            }
        );
        assert_eq!(
            d.z,
            Vec4 {
                x: 3.0,
                y: 6.0,
                z: 9.0,
                w: 0.0
            }
        );
        let mut c = matrix3_identity();
        matrix3_copy(&mut c, &scale(2.0));
        assert_eq!(c, scale(2.0));
    }

    #[test]
    fn mul_and_aliasing() {
        // S-008-R41
        let mut d = matrix3_identity();
        matrix3_mul(&mut d, &scale(2.0), &scale(3.0));
        assert_eq!(d, scale(6.0));
        let mut a = scale(2.0);
        let (a1, a2) = (a, a);
        matrix3_mul(&mut a, &a1, &a2);
        assert_eq!(a, scale(4.0));
        // translation composition: T(a) · T(b) = T(a + b)
        let mut d = matrix3_identity();
        matrix3_mul(&mut d, &translate(1.0, 2.0, 3.0), &translate(4.0, 5.0, 6.0));
        assert!(close(&d, &translate(5.0, 7.0, 9.0)));
    }

    #[test]
    fn inverse() {
        // S-008-R41
        let mut d = matrix3_identity();
        assert!(matrix3_inv(&mut d, &scale(2.0)));
        assert!(close(&d, &scale(0.5)));
        let sentinel = scale(3.0);
        let mut d2 = sentinel;
        assert!(!matrix3_inv(
            &mut d2,
            &Matrix3 {
                x: crate::vec4::vec4_zero(),
                y: crate::vec4::vec4_zero(),
                z: crate::vec4::vec4_zero()
            }
        ));
        assert_eq!(d2, sentinel);
        // round trip of a rotated + scaled matrix
        let mut rm = matrix3_identity();
        matrix3_rotate_aa(
            &mut rm,
            &scale(2.0),
            &AxisAng {
                x: 0.0,
                y: 0.3,
                z: 0.0,
                w: 1.1,
            },
        );
        let mut inv = matrix3_identity();
        assert!(matrix3_inv(&mut inv, &rm));
        let mut rt = matrix3_identity();
        matrix3_mul(&mut rt, &rm, &inv);
        assert!(close(&rt, &matrix3_identity()));
    }

    #[test]
    fn constructions() {
        // S-008-R41
        // 90° about +Z
        let q = Quat {
            x: 0.0,
            y: 0.0,
            z: std::f32::consts::FRAC_1_SQRT_2,
            w: std::f32::consts::FRAC_1_SQRT_2,
        };
        let mut d = matrix3_identity();
        matrix3_from_quat(&mut d, &q);
        assert!(within(d.x.y, 1.0));
        assert!(within(d.y.x, -1.0));
        assert!(within(d.z.z, 1.0));
        // from_axisang agrees
        let mut d2 = matrix3_identity();
        matrix3_from_axisang(
            &mut d2,
            &AxisAng {
                x: 0.0,
                y: 0.0,
                z: 1.0,
                w: std::f32::consts::FRAC_PI_2,
            },
        );
        assert!(close(&d2, &d));
        // zero quat -> identity
        let mut d3 = matrix3_identity();
        matrix3_from_quat(
            &mut d3,
            &Quat {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                w: 0.0,
            },
        );
        assert!(close(&d3, &matrix3_identity()));
        // from_matrix4 takes the 3×3 corner (w lanes dropped)
        let mut m4 = crate::matrix4::matrix4_identity();
        m4.x = Vec4 {
            x: 2.0,
            y: 0.0,
            z: 0.0,
            w: 0.0,
        };
        m4.t = Vec4 {
            x: 99.0,
            y: 99.0,
            z: 99.0,
            w: 1.0,
        };
        let mut d4 = matrix3_identity();
        matrix3_from_matrix4(&mut d4, &m4);
        assert_eq!(
            d4.x,
            Vec4 {
                x: 2.0,
                y: 0.0,
                z: 0.0,
                w: 0.0
            }
        );
        assert_eq!(d4.y, m4.y);
        // the 4×3 upgrade is the inverse projection
        let mut back = crate::matrix4::matrix4_identity();
        crate::matrix4::matrix4_from_matrix3(&mut back, &d4);
        assert_eq!(back.x, m4.x);
        assert_eq!(
            back.t,
            Vec4 {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                w: 1.0
            }
        );
    }

    #[test]
    fn composition() {
        // S-008-R41 (rotate/scale/translate helpers)
        let mut d = matrix3_identity();
        matrix3_scale(
            &mut d,
            &scale(2.0),
            &Vec3 {
                x: 3.0,
                y: 1.0,
                z: 1.0,
                w: 0.0,
            },
        );
        assert_eq!(
            d.x,
            Vec4 {
                x: 6.0,
                y: 0.0,
                z: 0.0,
                w: 0.0
            }
        );
        assert_eq!(
            d.y,
            Vec4 {
                x: 0.0,
                y: 2.0,
                z: 0.0,
                w: 0.0
            }
        );
        // rotation of the identity is the rotation
        let q = Quat {
            x: 0.0,
            y: 0.0,
            z: 0.0,
            w: 1.0,
        };
        let mut d = matrix3_identity();
        matrix3_rotate(&mut d, &scale(5.0), &q);
        assert!(close(&d, &scale(5.0)));
        // translate3f carries the translation in the w lanes
        let mut d = matrix3_identity();
        matrix3_translate3f(&mut d, &matrix3_identity(), 1.0, 2.0, 3.0);
        assert_eq!(
            d.x,
            Vec4 {
                x: 1.0,
                y: 0.0,
                z: 0.0,
                w: 1.0
            }
        );
        assert_eq!(
            d.y,
            Vec4 {
                x: 0.0,
                y: 1.0,
                z: 0.0,
                w: 2.0
            }
        );
        assert_eq!(
            d.z,
            Vec4 {
                x: 0.0,
                y: 0.0,
                z: 1.0,
                w: 3.0
            }
        );
        // scale3f
        let mut d = matrix3_identity();
        matrix3_scale3f(&mut d, &matrix3_identity(), 2.0, 3.0, 4.0);
        assert_eq!(
            d.z,
            Vec4 {
                x: 0.0,
                y: 0.0,
                z: 4.0,
                w: 0.0
            }
        );
        // rotate_aa4f
        let mut d = matrix3_identity();
        matrix3_rotate_aa4f(
            &mut d,
            &matrix3_identity(),
            0.0,
            0.0,
            1.0,
            std::f32::consts::FRAC_PI_2,
        );
        assert!(within(d.x.y, 1.0));
        assert!(within(d.y.x, -1.0));
    }
}
