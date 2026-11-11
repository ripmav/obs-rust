//! `matrix4` (S-008 R30–R36; `libobs/graphics/matrix4.{h,c}`).
//!
//! Layout: four `vec4` **rows** `x, y, z, t` (64 bytes, 16-byte
//! aligned, row-major in that order). Points are **row vectors**:
//! `v' = v · M`, so the translation lives in row `t` and transform
//! chains read left to right.

use crate::axisang::AxisAng;
use crate::quat::{Quat, quat_dot, quat_from_axisang};
use crate::vec3::Vec3;
use crate::vec4::Vec4;

/// S-008 §3: the C `struct matrix4`.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Matrix4 {
    pub x: Vec4,
    pub y: Vec4,
    pub z: Vec4,
    pub t: Vec4,
}

/// S-008-R30: `matrix4_identity()`.
#[must_use]
pub fn matrix4_identity() -> Matrix4 {
    Matrix4 {
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
        t: Vec4 {
            x: 0.0,
            y: 0.0,
            z: 0.0,
            w: 1.0,
        },
    }
}

/// S-008-R30: `matrix4_copy(d, m)` — copies the four rows.
pub fn matrix4_copy(d: &mut Matrix4, m: &Matrix4) {
    *d = *m;
}

/// S-008-R30: ordinary transpose, safe when `d == m`.
pub fn matrix4_transpose(d: &mut Matrix4, m: &Matrix4) {
    let x = m.x;
    let y = m.y;
    let z = m.z;
    let t = m.t;
    d.x = Vec4 {
        x: x.x,
        y: y.x,
        z: z.x,
        w: t.x,
    };
    d.y = Vec4 {
        x: x.y,
        y: y.y,
        z: z.y,
        w: t.y,
    };
    d.z = Vec4 {
        x: x.z,
        y: y.z,
        z: z.z,
        w: t.z,
    };
    d.t = Vec4 {
        x: x.w,
        y: y.w,
        z: z.w,
        w: t.w,
    };
}

/// S-008-R31: `matrix4_mul(d, m1, m2)` — `d[r].c = Σₖ m1[r].k · m2[k].c`
/// (rows of `m1` times columns of `m2`; result = apply `m1` first, then
/// `m2` to a row vector). Safe when `d` aliases an input.
pub fn matrix4_mul(d: &mut Matrix4, m1: &Matrix4, m2: &Matrix4) {
    let rows1 = [m1.x, m1.y, m1.z, m1.t];
    let rows2 = [m2.x, m2.y, m2.z, m2.t];
    let out = |r: usize| Vec4 {
        x: rows1[r].x * rows2[0].x
            + rows1[r].y * rows2[1].x
            + rows1[r].z * rows2[2].x
            + rows1[r].w * rows2[3].x,
        y: rows1[r].x * rows2[0].y
            + rows1[r].y * rows2[1].y
            + rows1[r].z * rows2[2].y
            + rows1[r].w * rows2[3].y,
        z: rows1[r].x * rows2[0].z
            + rows1[r].y * rows2[1].z
            + rows1[r].z * rows2[2].z
            + rows1[r].w * rows2[3].z,
        w: rows1[r].x * rows2[0].w
            + rows1[r].y * rows2[1].w
            + rows1[r].z * rows2[2].w
            + rows1[r].w * rows2[3].w,
    };
    let (ox, oy, oz, ot) = (out(0), out(1), out(2), out(3));
    d.x = ox;
    d.y = oy;
    d.z = oz;
    d.t = ot;
}

/// S-008-R31: `matrix4_mul_4x3_only(d, m1, m2)` — the first three rows
/// exactly as `matrix4_mul`, and `d.t` is **`m2.t` copied unchanged**
/// (assumes `m1`'s last column is `(0,0,0,1)` and ignores `m1.t`).
pub fn matrix4_mul_4x3_only(d: &mut Matrix4, m1: &Matrix4, m2: &Matrix4) {
    let rows1 = [m1.x, m1.y, m1.z, m1.t];
    let rows2 = [m2.x, m2.y, m2.z, m2.t];
    let row = |r: usize| Vec4 {
        x: rows1[r].x * rows2[0].x
            + rows1[r].y * rows2[1].x
            + rows1[r].z * rows2[2].x
            + rows1[r].w * rows2[3].x,
        y: rows1[r].x * rows2[0].y
            + rows1[r].y * rows2[1].y
            + rows1[r].z * rows2[2].y
            + rows1[r].w * rows2[3].y,
        z: rows1[r].x * rows2[0].z
            + rows1[r].y * rows2[1].z
            + rows1[r].z * rows2[2].z
            + rows1[r].w * rows2[3].z,
        w: rows1[r].x * rows2[0].w
            + rows1[r].y * rows2[1].w
            + rows1[r].z * rows2[2].w
            + rows1[r].w * rows2[3].w,
    };
    let (ox, oy, oz) = (row(0), row(1), row(2));
    d.x = ox;
    d.y = oy;
    d.z = oz;
    d.t = rows2[3];
}

/// 3×3 determinant helper (S-008-R32); row-major 9-element matrix.
fn det3(m: [f32; 9]) -> f32 {
    m[0] * (m[4] * m[8] - m[5] * m[7]) - m[1] * (m[3] * m[8] - m[5] * m[6])
        + m[2] * (m[3] * m[7] - m[4] * m[6])
}

/// Element `b` of row `a` (0=x, 1=y, 2=z, 3=t).
#[inline]
fn el(m: &Matrix4, a: usize, b: usize) -> f32 {
    match (a, b) {
        (0, 0) => m.x.x,
        (0, 1) => m.x.y,
        (0, 2) => m.x.z,
        (0, 3) => m.x.w,
        (1, 0) => m.y.x,
        (1, 1) => m.y.y,
        (1, 2) => m.y.z,
        (1, 3) => m.y.w,
        (2, 0) => m.z.x,
        (2, 1) => m.z.y,
        (2, 2) => m.z.z,
        (2, 3) => m.z.w,
        (3, 0) => m.t.x,
        (3, 1) => m.t.y,
        (3, 2) => m.t.z,
        _ => m.t.w,
    }
}

/// The 3×3 minor of `m` deleting row `i` and column `j`, as nine
/// elements in row-major order.
fn minor(m: &Matrix4, i: usize, j: usize) -> [f32; 9] {
    let mut out = [0.0f32; 9];
    let mut k = 0;
    for r in 0..4 {
        if r == i {
            continue;
        }
        for c in 0..4 {
            if c == j {
                continue;
            }
            out[k] = el(m, r, c);
            k += 1;
        }
    }
    out
}

/// S-008-R32: `matrix4_determinant` — expansion along the first row
/// with 3×3 cofactors: `Σⱼ (−1)ʲ · m[0][j] · det₃(minor(0,j))`.
#[must_use]
pub fn matrix4_determinant(m: &Matrix4) -> f32 {
    let mut det = 0.0f32;
    for j in 0..4 {
        let mi = minor(m, 0, j);
        det += if j.is_multiple_of(2) { 1.0 } else { -1.0 } * el(m, 0, j) * det3(mi);
    }
    det
}

/// S-008-R33: `matrix4_inv(d, m)` — `|det| < 0.0005` -> `false` with
/// `d` unmodified; else adjugate/determinant:
/// `d[j].component[i] = (−1)^(i+j) · det₃(minor(i,j)) / det`.
/// Allowed: `d == m`.
pub fn matrix4_inv(d: &mut Matrix4, m: &Matrix4) -> bool {
    let det = matrix4_determinant(m);
    if det.abs() < 0.0005 {
        return false;
    }
    // adjugate: d.row j = cofactor column j / det (unrolled)
    let f = |i: usize, j: usize| {
        let sgn = if (i + j).is_multiple_of(2) { 1.0 } else { -1.0 };
        sgn * det3(minor(m, i, j)) / det
    };
    d.x = Vec4 {
        x: f(0, 0),
        y: f(1, 0),
        z: f(2, 0),
        w: f(3, 0),
    };
    d.y = Vec4 {
        x: f(0, 1),
        y: f(1, 1),
        z: f(2, 1),
        w: f(3, 1),
    };
    d.z = Vec4 {
        x: f(0, 2),
        y: f(1, 2),
        z: f(2, 2),
        w: f(3, 2),
    };
    d.t = Vec4 {
        x: f(0, 3),
        y: f(1, 3),
        z: f(2, 3),
        w: f(3, 3),
    };
    true
}

/// S-008-R34: `matrix4_from_quat(d, q)` — formula in the spec, same
/// operation order. A zero quaternion yields the identity.
pub fn matrix4_from_quat(d: &mut Matrix4, q: &Quat) {
    let n = quat_dot(q, q);
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
    d.t = Vec4 {
        x: 0.0,
        y: 0.0,
        z: 0.0,
        w: 1.0,
    };
}

/// S-008-R34: `matrix4_from_axisang = from_quat(from_axisang(aa))`.
pub fn matrix4_from_axisang(d: &mut Matrix4, aa: &AxisAng) {
    let mut q = Quat {
        x: 0.0,
        y: 0.0,
        z: 0.0,
        w: 0.0,
    };
    quat_from_axisang(&mut q, aa);
    matrix4_from_quat(d, &q);
}

/// S-009-R32 (supersedes the S-008-R34 reading for the 3×4 layout): the
/// four rows are copied — rows `x, y, z` **with their `w` lanes**,
/// `t = (m.t.x, m.t.y, m.t.z, 1)`.
pub fn matrix4_from_matrix3(d: &mut Matrix4, m: &crate::matrix3::Matrix3) {
    d.x = Vec4 {
        x: m.x.x,
        y: m.x.y,
        z: m.x.z,
        w: m.x.w,
    };
    d.y = Vec4 {
        x: m.y.x,
        y: m.y.y,
        z: m.y.z,
        w: m.y.w,
    };
    d.z = Vec4 {
        x: m.z.x,
        y: m.z.y,
        z: m.z.z,
        w: m.z.w,
    };
    d.t = Vec4 {
        x: m.t.x,
        y: m.t.y,
        z: m.t.z,
        w: 1.0,
    };
}

/// S-008-R35: `matrix4_translate3v(dst, m, v)` — `dst = m ∘ op` with
/// `op` = identity and row `t = (v, 1)` (op is applied **after** `m`,
/// so the translation lands unscaled by `m`).
pub fn matrix4_translate3v(dst: &mut Matrix4, m: &Matrix4, v: &Vec3) {
    let mut op = matrix4_identity();
    op.t = Vec4 {
        x: v.x,
        y: v.y,
        z: v.z,
        w: 1.0,
    };
    matrix4_mul(dst, m, &op);
}

/// S-008-R35: `matrix4_translate4v(dst, m, v)` — row `t = v`.
pub fn matrix4_translate4v(dst: &mut Matrix4, m: &Matrix4, v: &Vec4) {
    let mut op = matrix4_identity();
    op.t = *v;
    matrix4_mul(dst, m, &op);
}

/// S-008-R35: `matrix4_rotate(dst, m, q)` — `op = from_quat(q)`.
pub fn matrix4_rotate(dst: &mut Matrix4, m: &Matrix4, q: &Quat) {
    let mut op = matrix4_identity();
    matrix4_from_quat(&mut op, q);
    matrix4_mul(dst, m, &op);
}

/// S-008-R35: `matrix4_rotate_aa(dst, m, aa)` — `op = from_axisang(aa)`.
pub fn matrix4_rotate_aa(dst: &mut Matrix4, m: &Matrix4, aa: &AxisAng) {
    let mut op = matrix4_identity();
    matrix4_from_axisang(&mut op, aa);
    matrix4_mul(dst, m, &op);
}

/// S-008-R35: `matrix4_scale(dst, m, v)` — `op = diag(v.x, v.y, v.z, 1)`.
pub fn matrix4_scale(dst: &mut Matrix4, m: &Matrix4, v: &Vec3) {
    let mut op = matrix4_identity();
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
    matrix4_mul(dst, m, &op);
}

/// S-008-R36: `matrix4_translate3v_i(dst, v, m)` — rows `x,y,z` copied
/// from `m`; `dst.t = (v, 1) · m` (translation applied first, then `m`;
/// vector 05: `ti = 20 60 120 1`).
pub fn matrix4_translate3v_i(dst: &mut Matrix4, v: &Vec3, m: &Matrix4) {
    let v4 = Vec4 {
        x: v.x,
        y: v.y,
        z: v.z,
        w: 1.0,
    };
    dst.x = m.x;
    dst.y = m.y;
    dst.z = m.z;
    dst.t = Vec4 {
        x: v4.x * m.x.x + v4.y * m.y.x + v4.z * m.z.x + v4.w * m.t.x,
        y: v4.x * m.x.y + v4.y * m.y.y + v4.z * m.z.y + v4.w * m.t.y,
        z: v4.x * m.x.z + v4.y * m.y.z + v4.z * m.z.z + v4.w * m.t.z,
        w: v4.x * m.x.w + v4.y * m.y.w + v4.z * m.z.w + v4.w * m.t.w,
    };
}

/// S-008-R36: `matrix4_translate4v_i(dst, v, m)` — same with a 4-vector.
pub fn matrix4_translate4v_i(dst: &mut Matrix4, v: &Vec4, m: &Matrix4) {
    dst.x = m.x;
    dst.y = m.y;
    dst.z = m.z;
    dst.t = crate::vec4::vec4_transform(v, m);
}

/// S-008-R36: `matrix4_rotate_i(dst, q, m) = mul_4x3_only(from_quat(q), m)`.
pub fn matrix4_rotate_i(dst: &mut Matrix4, q: &Quat, m: &Matrix4) {
    let mut op = matrix4_identity();
    matrix4_from_quat(&mut op, q);
    matrix4_mul_4x3_only(dst, &op, m);
}

/// S-008-R36: `matrix4_rotate_aa_i(dst, aa, m)` — likewise for axis-angle.
pub fn matrix4_rotate_aa_i(dst: &mut Matrix4, aa: &AxisAng, m: &Matrix4) {
    let mut op = matrix4_identity();
    matrix4_from_axisang(&mut op, aa);
    matrix4_mul_4x3_only(dst, &op, m);
}

/// S-008-R36: `matrix4_scale_i(dst, v, m) = mul_4x3_only(diag(v,1), m)`.
pub fn matrix4_scale_i(dst: &mut Matrix4, v: &Vec3, m: &Matrix4) {
    let mut op = matrix4_identity();
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
    matrix4_mul_4x3_only(dst, &op, m);
}

/// S-008-R41 (`matrix4.h` inlines): `matrix4_translate3f`.
pub fn matrix4_translate3f(dst: &mut Matrix4, m: &Matrix4, x: f32, y: f32, z: f32) {
    matrix4_translate3v(dst, m, &Vec3 { x, y, z, w: 0.0 });
}

/// S-008-R41 (`matrix4.h` inlines): `matrix4_rotate_aa4f`.
pub fn matrix4_rotate_aa4f(dst: &mut Matrix4, m: &Matrix4, x: f32, y: f32, z: f32, angle: f32) {
    matrix4_rotate_aa(dst, m, &crate::axisang::axisang_set(x, y, z, angle));
}

/// S-008-R41 (`matrix4.h` inlines): `matrix4_scale3f`.
pub fn matrix4_scale3f(dst: &mut Matrix4, m: &Matrix4, x: f32, y: f32, z: f32) {
    matrix4_scale(dst, m, &Vec3 { x, y, z, w: 0.0 });
}

#[cfg(test)]
mod tests {
    use super::*;

    fn within(a: f32, b: f32) -> bool {
        let d = (a - b).abs();
        d <= 1e-6 || d <= 1e-6 * a.abs().max(b.abs())
    }

    fn scale(s: f32) -> Matrix4 {
        Matrix4 {
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
            t: Vec4 {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                w: 1.0,
            },
        }
    }

    fn translate(x: f32, y: f32, z: f32) -> Matrix4 {
        let mut m = matrix4_identity();
        m.t = Vec4 { x, y, z, w: 1.0 };
        m
    }

    fn rows_close(a: &Matrix4, b: &Matrix4) -> bool {
        for r in 0..4 {
            for c in 0..4 {
                if !within(el(a, r, c), el(b, r, c)) {
                    return false;
                }
            }
        }
        true
    }

    #[test]
    fn layout() {
        use std::mem::{align_of, size_of};
        assert_eq!(size_of::<Matrix4>(), 64);
        assert_eq!(align_of::<Matrix4>(), 16);
    }

    #[test]
    fn identity_and_transpose() {
        // S-008-R30
        let m = matrix4_identity();
        assert_eq!(
            m.x,
            Vec4 {
                x: 1.0,
                y: 0.0,
                z: 0.0,
                w: 0.0
            }
        );
        assert_eq!(
            m.t,
            Vec4 {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                w: 1.0
            }
        );
        let mut d = Matrix4 {
            x: Vec4 {
                x: 1.0,
                y: 2.0,
                z: 3.0,
                w: 4.0,
            },
            y: Vec4 {
                x: 5.0,
                y: 6.0,
                z: 7.0,
                w: 8.0,
            },
            z: Vec4 {
                x: 9.0,
                y: 10.0,
                z: 11.0,
                w: 12.0,
            },
            t: Vec4 {
                x: 13.0,
                y: 14.0,
                z: 15.0,
                w: 16.0,
            },
        };
        let m = d;
        matrix4_transpose(&mut d, &m); // d == m must be safe (copy stands in for the alias)
        assert_eq!(
            d,
            Matrix4 {
                x: Vec4 {
                    x: 1.0,
                    y: 5.0,
                    z: 9.0,
                    w: 13.0
                },
                y: Vec4 {
                    x: 2.0,
                    y: 6.0,
                    z: 10.0,
                    w: 14.0
                },
                z: Vec4 {
                    x: 3.0,
                    y: 7.0,
                    z: 11.0,
                    w: 15.0
                },
                t: Vec4 {
                    x: 4.0,
                    y: 8.0,
                    z: 12.0,
                    w: 16.0
                }
            }
        );
    }

    #[test]
    fn mul_and_aliasing() {
        // S-008-R31: T · S with row vectors: translate first, then scale ->
        // the translation is scaled too.
        let t = translate(1.0, 2.0, 3.0);
        let s = scale(2.0);
        let mut d = matrix4_identity();
        matrix4_mul(&mut d, &t, &s);
        assert_eq!(
            d.t,
            Vec4 {
                x: 2.0,
                y: 4.0,
                z: 6.0,
                w: 1.0
            }
        );
        assert_eq!(
            d.x,
            Vec4 {
                x: 2.0,
                y: 0.0,
                z: 0.0,
                w: 0.0
            }
        );
        // Alias safety: d == m1 == m2 (S · S = S(4))
        let mut a = s;
        let (a1, a2) = (a, a);
        matrix4_mul(&mut a, &a1, &a2);
        assert_eq!(
            a.x,
            Vec4 {
                x: 4.0,
                y: 0.0,
                z: 0.0,
                w: 0.0
            }
        );
        assert_eq!(
            a.y,
            Vec4 {
                x: 0.0,
                y: 4.0,
                z: 0.0,
                w: 0.0
            }
        );
        assert_eq!(
            a.z,
            Vec4 {
                x: 0.0,
                y: 0.0,
                z: 4.0,
                w: 0.0
            }
        );
        assert_eq!(
            a.t,
            Vec4 {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                w: 1.0
            }
        );
    }

    #[test]
    fn mul_4x3_only_copies_t() {
        // S-008-R31: d.t is m2.t unchanged; m1.t is ignored.
        let mut m1 = matrix4_identity();
        m1.t = Vec4 {
            x: 99.0,
            y: 99.0,
            z: 99.0,
            w: 99.0,
        }; // garbage
        let m2 = translate(1.0, 2.0, 3.0);
        let mut d = matrix4_identity();
        matrix4_mul_4x3_only(&mut d, &m1, &m2);
        assert_eq!(
            d.t,
            Vec4 {
                x: 1.0,
                y: 2.0,
                z: 3.0,
                w: 1.0
            }
        );
        assert_eq!(
            d.x,
            Vec4 {
                x: 1.0,
                y: 0.0,
                z: 0.0,
                w: 0.0
            }
        );
    }

    #[test]
    fn determinant() {
        // S-008-R32
        assert!(within(matrix4_determinant(&matrix4_identity()), 1.0));
        assert!(within(matrix4_determinant(&scale(2.0)), 8.0));
        assert!(within(matrix4_determinant(&translate(1.0, 2.0, 3.0)), 1.0));
        // S(2) · T(1,2,3): det = 8
        let mut d = matrix4_identity();
        matrix4_mul(&mut d, &scale(2.0), &translate(1.0, 2.0, 3.0));
        assert!(within(matrix4_determinant(&d), 8.0));
    }

    #[test]
    fn inverse() {
        // S-008-R33
        let mut d = matrix4_identity();
        assert!(matrix4_inv(&mut d, &matrix4_identity()));
        assert!(rows_close(&d, &matrix4_identity()));
        assert!(matrix4_inv(&mut d, &scale(2.0)));
        assert!(rows_close(&d, &scale(0.5)));
        // singular: d must be left unmodified
        let sentinel = translate(7.0, 8.0, 9.0);
        let mut d2 = sentinel;
        assert!(!matrix4_inv(
            &mut d2,
            &Matrix4 {
                x: crate::vec4::vec4_zero(),
                y: crate::vec4::vec4_zero(),
                z: crate::vec4::vec4_zero(),
                t: crate::vec4::vec4_zero()
            }
        ));
        assert_eq!(d2, sentinel);
        // below-threshold "invertible" matrix is reported singular (vector 04)
        assert!(!matrix4_inv(&mut d2, &scale(0.0003)));
        assert_eq!(d2, sentinel);
        // round trip M · M⁻¹ = I within tolerance
        let q = Quat {
            x: 0.0,
            y: 0.0,
            z: std::f32::consts::FRAC_1_SQRT_2,
            w: std::f32::consts::FRAC_1_SQRT_2,
        };
        let mut rm = matrix4_identity();
        matrix4_from_quat(&mut rm, &q);
        let rm0 = rm;
        matrix4_translate3v(
            &mut rm,
            &rm0,
            &Vec3 {
                x: 1.0,
                y: -2.0,
                z: 3.0,
                w: 0.0,
            },
        );
        let mut inv = matrix4_identity();
        assert!(matrix4_inv(&mut inv, &rm));
        let mut rt = matrix4_identity();
        matrix4_mul(&mut rt, &rm, &inv);
        assert!(rows_close(&rt, &matrix4_identity()));
    }

    #[test]
    fn from_quat_formula() {
        // S-008-R34: 90° about +Z, q = (0, 0, sin(π/4), cos(π/4))
        let s = (std::f32::consts::FRAC_PI_4).sin();
        let c = (std::f32::consts::FRAC_PI_4).cos();
        let q = Quat {
            x: 0.0,
            y: 0.0,
            z: s,
            w: c,
        };
        let mut d = matrix4_identity();
        matrix4_from_quat(&mut d, &q);
        // rows: x = (0, 1, 0, 0), y = (-1, 0, 0, 0), z = (0, 0, 1, 0), t = (0,0,0,1)
        assert!(within(d.x.x, 0.0));
        assert!(within(d.x.y, 1.0));
        assert!(within(d.x.z, 0.0));
        assert!(within(d.y.x, -1.0));
        assert!(within(d.y.y, 0.0));
        assert!(within(d.z.z, 1.0));
        assert!(within(d.t.w, 1.0));
        // zero quaternion -> identity
        let mut d2 = translate(1.0, 1.0, 1.0); // overwritten to prove the full write
        matrix4_from_quat(
            &mut d2,
            &Quat {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                w: 0.0,
            },
        );
        assert!(rows_close(&d2, &matrix4_identity()));
        // from_axisang agrees with from_quat(from_axisang)
        let mut d3 = matrix4_identity();
        matrix4_from_axisang(
            &mut d3,
            &crate::axisang::axisang_set(0.0, 0.0, 1.0, std::f32::consts::FRAC_PI_2),
        );
        assert!(rows_close(&d3, &d));
    }

    #[test]
    fn composition_helpers() {
        // S-008-R35: dst = m ∘ op
        // translate: op.t = (v,1)
        let mut d = matrix4_identity();
        matrix4_translate3v(
            &mut d,
            &matrix4_identity(),
            &Vec3 {
                x: 20.0,
                y: 60.0,
                z: 120.0,
                w: 0.0,
            },
        );
        assert_eq!(
            d.t,
            Vec4 {
                x: 20.0,
                y: 60.0,
                z: 120.0,
                w: 1.0
            }
        );
        // with m = S(2): op is applied AFTER m, so the translation
        // lands unscaled — scaling is what the _i variant does
        let mut d = matrix4_identity();
        matrix4_translate3v(
            &mut d,
            &scale(2.0),
            &Vec3 {
                x: 20.0,
                y: 60.0,
                z: 120.0,
                w: 0.0,
            },
        );
        assert!(within(d.t.x, 20.0));
        assert!(within(d.t.y, 60.0));
        assert!(within(d.t.z, 120.0));
        assert!(within(d.t.w, 1.0));
        // scale: op = diag(v,1)
        let mut d = matrix4_identity();
        matrix4_scale(
            &mut d,
            &matrix4_identity(),
            &Vec3 {
                x: 2.0,
                y: 3.0,
                z: 4.0,
                w: 0.0,
            },
        );
        assert_eq!(
            d.x,
            Vec4 {
                x: 2.0,
                y: 0.0,
                z: 0.0,
                w: 0.0
            }
        );
        assert_eq!(
            d.y,
            Vec4 {
                x: 0.0,
                y: 3.0,
                z: 0.0,
                w: 0.0
            }
        );
        assert_eq!(
            d.z,
            Vec4 {
                x: 0.0,
                y: 0.0,
                z: 4.0,
                w: 0.0
            }
        );
        assert_eq!(
            d.t,
            Vec4 {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                w: 1.0
            }
        );
        // 4-vector translate
        let mut d = matrix4_identity();
        matrix4_translate4v(
            &mut d,
            &matrix4_identity(),
            &Vec4 {
                x: 1.0,
                y: 2.0,
                z: 3.0,
                w: 5.0,
            },
        );
        assert_eq!(
            d.t,
            Vec4 {
                x: 1.0,
                y: 2.0,
                z: 3.0,
                w: 5.0
            }
        );
        // rotate: op = from_quat
        let q = Quat {
            x: 0.0,
            y: 0.0,
            z: 0.0,
            w: 1.0,
        };
        let mut d = matrix4_identity();
        matrix4_rotate(&mut d, &scale(3.0), &q); // identity quat: no change
        assert!(rows_close(&d, &scale(3.0)));
    }

    #[test]
    fn inverse_order_variants() {
        // S-008-R36
        // vector 05: m = I, v = (20,60,120) -> t = (20,60,120,1)
        let mut d = matrix4_identity();
        matrix4_translate3v_i(
            &mut d,
            &Vec3 {
                x: 20.0,
                y: 60.0,
                z: 120.0,
                w: 0.0,
            },
            &matrix4_identity(),
        );
        assert_eq!(
            d.t,
            Vec4 {
                x: 20.0,
                y: 60.0,
                z: 120.0,
                w: 1.0
            }
        );
        assert_eq!(
            d.x,
            Vec4 {
                x: 1.0,
                y: 0.0,
                z: 0.0,
                w: 0.0
            }
        );
        // with m = S(2): t = (v,1)·m -> scaled (the _i distinction)
        let mut d = matrix4_identity();
        matrix4_translate3v_i(
            &mut d,
            &Vec3 {
                x: 20.0,
                y: 60.0,
                z: 120.0,
                w: 0.0,
            },
            &scale(2.0),
        );
        assert!(within(d.t.x, 40.0));
        assert!(within(d.t.y, 120.0));
        assert!(within(d.t.z, 240.0));
        assert!(within(d.t.w, 1.0));
        // translate4v_i
        let mut d = matrix4_identity();
        matrix4_translate4v_i(
            &mut d,
            &Vec4 {
                x: 1.0,
                y: 2.0,
                z: 3.0,
                w: 1.0,
            },
            &translate(10.0, 20.0, 30.0),
        );
        assert!(within(d.t.x, 11.0));
        assert!(within(d.t.y, 22.0));
        assert!(within(d.t.z, 33.0));
        // rotate_i = mul_4x3_only(from_quat, m): 90° about Z, m = T
        let q = Quat {
            x: 0.0,
            y: 0.0,
            z: std::f32::consts::FRAC_1_SQRT_2,
            w: std::f32::consts::FRAC_1_SQRT_2,
        };
        let mut d = matrix4_identity();
        matrix4_rotate_i(&mut d, &q, &translate(1.0, 2.0, 3.0));
        // from_quat rows (0,1,0),(-1,0,0),(0,0,1) pick the rows of m:
        // d.x = m.y, d.y = -m.x, d.z = m.z (identity rows of m)
        assert_eq!(
            d.x,
            Vec4 {
                x: 0.0,
                y: 1.0,
                z: 0.0,
                w: 0.0
            }
        );
        assert!(within(d.y.x, -1.0));
        assert!(within(d.y.y, 0.0));
        assert_eq!(
            d.z,
            Vec4 {
                x: 0.0,
                y: 0.0,
                z: 1.0,
                w: 0.0
            }
        );
        assert_eq!(
            d.t,
            Vec4 {
                x: 1.0,
                y: 2.0,
                z: 3.0,
                w: 1.0
            }
        );
        // scale_i = mul_4x3_only(diag(v,1), m)
        let mut d = matrix4_identity();
        matrix4_scale_i(
            &mut d,
            &Vec3 {
                x: 3.0,
                y: 4.0,
                z: 5.0,
                w: 0.0,
            },
            &scale(2.0),
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
                y: 8.0,
                z: 0.0,
                w: 0.0
            }
        );
        assert_eq!(
            d.z,
            Vec4 {
                x: 0.0,
                y: 0.0,
                z: 10.0,
                w: 0.0
            }
        );
        assert_eq!(
            d.t,
            Vec4 {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                w: 1.0
            }
        );
        // rotate_aa_i
        let mut d = matrix4_identity();
        matrix4_rotate_aa_i(
            &mut d,
            &crate::axisang::axisang_set(0.0, 0.0, 1.0, std::f32::consts::FRAC_PI_2),
            &translate(1.0, 2.0, 3.0),
        );
        assert_eq!(
            d.t,
            Vec4 {
                x: 1.0,
                y: 2.0,
                z: 3.0,
                w: 1.0
            }
        );
        assert!(within(d.y.x, -1.0));
        assert!(within(d.y.y, 0.0));
    }

    #[test]
    fn inline_scalar_variants() {
        // S-008-R41 (matrix4.h inlines)
        let mut d = matrix4_identity();
        matrix4_translate3f(&mut d, &matrix4_identity(), 1.0, 2.0, 3.0);
        assert_eq!(
            d.t,
            Vec4 {
                x: 1.0,
                y: 2.0,
                z: 3.0,
                w: 1.0
            }
        );
        let mut d = matrix4_identity();
        matrix4_scale3f(&mut d, &matrix4_identity(), 2.0, 3.0, 4.0);
        assert_eq!(
            d.z,
            Vec4 {
                x: 0.0,
                y: 0.0,
                z: 4.0,
                w: 0.0
            }
        );
        let mut d = matrix4_identity();
        matrix4_rotate_aa4f(
            &mut d,
            &matrix4_identity(),
            0.0,
            0.0,
            1.0,
            std::f32::consts::FRAC_PI_2,
        );
        assert!(within(d.x.y, 1.0));
        assert!(within(d.y.x, -1.0));
    }
}
