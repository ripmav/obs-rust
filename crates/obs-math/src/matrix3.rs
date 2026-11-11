//! `matrix3` (S-009 §5.4; `libobs/graphics/matrix3.{h,c}`).
//!
//! The C `struct matrix3` is a **3×4** matrix: four rows `x, y, z, t`,
//! each a `vec3` with a live `w` lane — 64 bytes, 16-byte aligned
//! (S-009 §3, R30).
//!
//! **Q-12 (the C product convention)**: `matrix3_mul` dots rows with
//! rows and *subtracts* the translation (`x' = (x − t) · rows`), and
//! `matrix3_inv` is the ordinary 4×4 inverse of `[x;y;z;t]` — which is
//! **not** the inverse under that product. Both are reproduced.
//!
//! WP-5 (S-008-R41) had implemented this type as 3×`Vec4` (48 bytes)
//! with a "translation in the `w` lanes" reading; S-009 pins the real
//! C layout (four rows incl. `t`, which the C functions such as
//! `matrix3_translate` require). The rebuild lands here (in-stack fix;
//! no external consumers yet).

use crate::axisang::AxisAng;
use crate::matrix4::Matrix4;
use crate::plane::Plane;
use crate::quat::{Quat, quat_dot, quat_from_axisang};
use crate::vec3::{self, Vec3};

/// S-009 §3: the C `struct matrix3` — four rows, each a `vec3`.
#[repr(C, align(16))]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Matrix3 {
    pub x: Vec3,
    pub y: Vec3,
    pub z: Vec3,
    pub t: Vec3,
}

// ── S-009-R30: identity / copy ──────────────────────────────────────────

/// S-009-R30: rows `x,y,z` are `(1,0,0,0) (0,1,0,0) (0,0,1,0)`,
/// `t = (0,0,0,0)`.
#[must_use]
pub fn matrix3_identity() -> Matrix3 {
    Matrix3 {
        x: vec3::vec3_set(1.0, 0.0, 0.0),
        y: vec3::vec3_set(0.0, 1.0, 0.0),
        z: vec3::vec3_set(0.0, 0.0, 1.0),
        t: vec3::vec3_zero(),
    }
}

/// S-009-R30: copies all 16 floats.
pub fn matrix3_copy(d: &mut Matrix3, m: &Matrix3) {
    *d = *m;
}

// ── S-009-R31: construction ─────────────────────────────────────────────

/// S-009-R31: identical formula to `matrix4_from_quat` (S-008 R34,
/// including `n = dot4(q,q)`, `s = n > 0 ? 2/n : 0`; a zero quaternion
/// gives the identity rotation) but the rows are `vec3` (`w = 0`) and
/// `t = (0,0,0,0)`.
pub fn matrix3_from_quat(d: &mut Matrix3, q: &Quat) {
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
    d.x = Vec3 {
        x: 1.0 - (yy + zz),
        y: xy + wz,
        z: xz - wy,
        w: 0.0,
    };
    d.y = Vec3 {
        x: xy - wz,
        y: 1.0 - (xx + zz),
        z: yz + wx,
        w: 0.0,
    };
    d.z = Vec3 {
        x: xz + wy,
        y: yz - wx,
        z: 1.0 - (xx + yy),
        w: 0.0,
    };
    d.t = vec3::vec3_zero();
}

/// S-009-R31: `matrix3_from_axisang(d, aa) = matrix3_from_quat(d,
/// quat_from_axisang(aa))` — the axis is **not** normalised (script 28).
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

// ── S-009-R32: matrix4 conversions ──────────────────────────────────────

/// S-009-R32: copies the four rows and sets the `w` lane of **every**
/// row to 0.
pub fn matrix3_from_matrix4(d: &mut Matrix3, m: &Matrix4) {
    d.x = Vec3 {
        x: m.x.x,
        y: m.x.y,
        z: m.x.z,
        w: 0.0,
    };
    d.y = Vec3 {
        x: m.y.x,
        y: m.y.y,
        z: m.y.z,
        w: 0.0,
    };
    d.z = Vec3 {
        x: m.z.x,
        y: m.z.y,
        z: m.z.z,
        w: 0.0,
    };
    d.t = Vec3 {
        x: m.t.x,
        y: m.t.y,
        z: m.t.z,
        w: 0.0,
    };
}

// ── S-009-R33: quat from matrix3 ────────────────────────────────────────

/// S-009-R33: S-008 R16 applied to the `x,y,z` lanes of rows `x,y,z`
/// (the `w` lanes and `t` are not read).
pub fn quat_from_matrix3(d: &mut Quat, m: &Matrix3) {
    let rows = [&m.x, &m.y, &m.z];
    let el = |i: usize, j: usize| match j {
        0 => rows[i].x,
        1 => rows[i].y,
        _ => rows[i].z,
    };
    let tr = el(0, 0) + el(1, 1) + el(2, 2);
    if tr > 0.0 {
        let s = (tr + 1.0).sqrt();
        let h = 0.5 / s;
        d.w = s / 2.0;
        d.x = (el(1, 2) - el(2, 1)) * h;
        d.y = (el(2, 0) - el(0, 2)) * h;
        d.z = (el(0, 1) - el(1, 0)) * h;
    } else {
        let mut i = if el(0, 0) > el(1, 1) { 0 } else { 1 };
        if el(2, 2) > el(i, i) {
            i = 2;
        }
        let j = (i + 1) % 3;
        let k = (i + 2) % 3;
        let s = (el(i, i) - el(j, j) - el(k, k) + 1.0).sqrt();
        let h = 0.5 / s;
        let mut ds = [0.0f32; 3];
        ds[i] = s / 2.0;
        ds[j] += (el(i, j) + el(j, i)) * h;
        ds[k] += (el(i, k) + el(k, i)) * h;
        d.x = ds[0];
        d.y = ds[1];
        d.z = ds[2];
        d.w = (el(j, k) - el(k, j)) * h;
    }
}

// ── S-009-R35: the C product (Q-12) ─────────────────────────────────────

/// S-009-R35: `d.r = vec3_rotate(a.r, b)` for rows `x,y,z` — each row of
/// `a` is four-lane-dotted with the rows of `b` — and
/// `d.t = vec3_transform3x4(a.t, b)`, i.e. `dot4(a.t − b.t, b.c)` per
/// lane. All result `w` lanes are 0. `d` may alias `a` or `b` (computed
/// into temporaries first).
pub fn matrix3_mul(d: &mut Matrix3, a: &Matrix3, b: &Matrix3) {
    let dot_row = |r: &Vec3| Vec3 {
        x: vec3::vec3_dot(r, &b.x),
        y: vec3::vec3_dot(r, &b.y),
        z: vec3::vec3_dot(r, &b.z),
        w: 0.0,
    };
    let (dx, dy, dz) = (dot_row(&a.x), dot_row(&a.y), dot_row(&a.z));
    let tsub = vec3::vec3_sub(&a.t, &b.t);
    let dt = Vec3 {
        x: vec3::vec3_dot(&tsub, &b.x),
        y: vec3::vec3_dot(&tsub, &b.y),
        z: vec3::vec3_dot(&tsub, &b.z),
        w: 0.0,
    };
    d.x = dx;
    d.y = dy;
    d.z = dz;
    d.t = dt;
}

// ── S-009-R36–R38: translate / rotate / scale ───────────────────────────

/// S-009-R36: `d.t = m.t − v` (**subtracts**) and **nothing else is
/// written** — rows `x,y,z` of `d` keep their previous contents, even
/// when `d != m` (as in C).
pub fn matrix3_translate(d: &mut Matrix3, m: &Matrix3, v: &Vec3) {
    d.t = vec3::vec3_sub(&m.t, v);
}

/// S-009-R36: builds `v = (x,y,z,0)` and calls `matrix3_translate`.
pub fn matrix3_translate3f(d: &mut Matrix3, m: &Matrix3, x: f32, y: f32, z: f32) {
    matrix3_translate(d, m, &vec3::vec3_set(x, y, z));
}

/// S-009-R37: `d = matrix3_mul(d, m, matrix3_from_quat(q))`.
pub fn matrix3_rotate(d: &mut Matrix3, m: &Matrix3, q: &Quat) {
    let mut op = matrix3_identity();
    matrix3_from_quat(&mut op, q);
    matrix3_mul(d, m, &op);
}

/// S-009-R37: `d = matrix3_mul(d, m, matrix3_from_axisang(aa))`.
pub fn matrix3_rotate_aa(d: &mut Matrix3, m: &Matrix3, aa: &AxisAng) {
    let mut op = matrix3_identity();
    matrix3_from_axisang(&mut op, aa);
    matrix3_mul(d, m, &op);
}

/// S-009-R37: builds `aa = (x,y,z,angle)` and calls `matrix3_rotate_aa`.
pub fn matrix3_rotate_aa4f(d: &mut Matrix3, m: &Matrix3, x: f32, y: f32, z: f32, angle: f32) {
    matrix3_rotate_aa(d, m, &AxisAng { x, y, z, w: angle });
}

/// S-009-R38: each of the four rows `r`: `d.r = vec3_mul(m.r, v)` (all
/// four lanes, so `d.r.w = m.r.w · v.w`). `d` may alias `m`.
pub fn matrix3_scale(d: &mut Matrix3, m: &Matrix3, v: &Vec3) {
    let (nx, ny, nz, nt) = (
        vec3::vec3_mul(&m.x, v),
        vec3::vec3_mul(&m.y, v),
        vec3::vec3_mul(&m.z, v),
        vec3::vec3_mul(&m.t, v),
    );
    d.x = nx;
    d.y = ny;
    d.z = nz;
    d.t = nt;
}

/// S-009-R38: builds `v = (x,y,z,0)` and calls `matrix3_scale`.
pub fn matrix3_scale3f(d: &mut Matrix3, m: &Matrix3, x: f32, y: f32, z: f32) {
    matrix3_scale(d, m, &vec3::vec3_set(x, y, z));
}

// ── S-009-R39/R40: transpose / inverse ──────────────────────────────────

/// S-009-R39/Q-3: results are functions of the *original* `m` (`d` may
/// alias `m`):
/// `d.t = −(dot4(m.t,m.x), dot4(m.t,m.y), dot4(m.t,m.z))`, `w = 0`;
/// `d.x = (m.x.x, m.y.x, m.z.x, m.z.w)` etc. — the transposed 3×3 block
/// with the `w` lane of **all three rows** set to `m.z.w`.
pub fn matrix3_transpose(d: &mut Matrix3, m: &Matrix3) {
    let mt = vec3::vec3_neg(&Vec3 {
        x: vec3::vec3_dot(&m.t, &m.x),
        y: vec3::vec3_dot(&m.t, &m.y),
        z: vec3::vec3_dot(&m.t, &m.z),
        w: 0.0,
    });
    let (dx, dy, dz) = (
        Vec3 {
            x: m.x.x,
            y: m.y.x,
            z: m.z.x,
            w: m.z.w,
        },
        Vec3 {
            x: m.x.y,
            y: m.y.y,
            z: m.z.y,
            w: m.z.w,
        },
        Vec3 {
            x: m.x.z,
            y: m.y.z,
            z: m.z.z,
            w: m.z.w,
        },
    );
    d.t = mt;
    d.x = dx;
    d.y = dy;
    d.z = dz;
}

/// S-009-R40/D-1: `M = matrix4_from_matrix3(m)` (rows keep their `w`
/// lanes, `t.w = 1`); `matrix4_inv` (fails when `|det| < 0.0005`)
/// writing into `d` reinterpreted as a `matrix4` (both are 16
/// consecutive floats per row); then **unconditionally** `d.t.w = 0`.
/// On failure `d` is unchanged **except `d.t.w = 0`** (D-1). `d` may
/// alias `m`.
///
/// Returns the success of the underlying `matrix4_inv`.
pub fn matrix3_inv(d: &mut Matrix3, m: &Matrix3) -> bool {
    let mut m4 = crate::matrix4::matrix4_identity();
    crate::matrix4::matrix4_from_matrix3(&mut m4, m);
    let mut out = m4;
    let ok = crate::matrix4::matrix4_inv(&mut out, &m4);
    if ok {
        d.x = Vec3 {
            x: out.x.x,
            y: out.x.y,
            z: out.x.z,
            w: out.x.w,
        };
        d.y = Vec3 {
            x: out.y.x,
            y: out.y.y,
            z: out.y.z,
            w: out.y.w,
        };
        d.z = Vec3 {
            x: out.z.x,
            y: out.z.y,
            z: out.z.z,
            w: out.z.w,
        };
        d.t = Vec3 {
            x: out.t.x,
            y: out.t.y,
            z: out.t.z,
            w: out.t.w,
        };
    }
    d.t.w = 0.0;
    ok
}

// ── S-009-R41: mirror ───────────────────────────────────────────────────

/// S-009-R41: rows `x,y,z`: `d.r = vec3_mirrorv(m.r, p.dir)`;
/// `d.t = vec3_mirror(m.t, p)`. `d` may alias `m`.
pub fn matrix3_mirror(d: &mut Matrix3, m: &Matrix3, p: &Plane) {
    let (nx, ny, nz, nt) = (
        vec3::vec3_mirrorv(&m.x, &p.dir),
        vec3::vec3_mirrorv(&m.y, &p.dir),
        vec3::vec3_mirrorv(&m.z, &p.dir),
        vec3::vec3_mirror(&m.t, p),
    );
    d.x = nx;
    d.y = ny;
    d.z = nz;
    d.t = nt;
}

/// S-009-R41: all four rows with `vec3_mirrorv(m.r, n)`. `d` may alias
/// `m`.
pub fn matrix3_mirrorv(d: &mut Matrix3, m: &Matrix3, n: &Vec3) {
    let (nx, ny, nz, nt) = (
        vec3::vec3_mirrorv(&m.x, n),
        vec3::vec3_mirrorv(&m.y, n),
        vec3::vec3_mirrorv(&m.z, n),
        vec3::vec3_mirrorv(&m.t, n),
    );
    d.x = nx;
    d.y = ny;
    d.z = nz;
    d.t = nt;
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
        // S-009 §3: 64 bytes, align 16, four vec3 rows
        assert_eq!(size_of::<Matrix3>(), 64);
        assert_eq!(align_of::<Matrix3>(), 16);
        assert_eq!(offset_of!(Matrix3, x), 0);
        assert_eq!(offset_of!(Matrix3, y), 16);
        assert_eq!(offset_of!(Matrix3, z), 32);
        assert_eq!(offset_of!(Matrix3, t), 48);
    }

    #[test]
    fn r30_identity_copy() {
        // S-009-R30
        let m = matrix3_identity();
        assert_eq!(m.x, vec3::vec3_set(1.0, 0.0, 0.0));
        assert_eq!(m.y, vec3::vec3_set(0.0, 1.0, 0.0));
        assert_eq!(m.z, vec3::vec3_set(0.0, 0.0, 1.0));
        assert_eq!(m.t, vec3::vec3_zero());
        let mut c = matrix3_identity();
        c.t = vec3::vec3_set(1.0, 2.0, 3.0);
        let mut d = matrix3_identity();
        matrix3_copy(&mut d, &c);
        assert_eq!(d, c);
    }

    #[test]
    fn r31_from_quat_zero_and_rotation() {
        // S-009-R31: zero quaternion -> identity rotation
        let mut m = Matrix3 {
            x: vec3::vec3_zero(),
            y: vec3::vec3_zero(),
            z: vec3::vec3_zero(),
            t: vec3::vec3_zero(),
        };
        matrix3_from_quat(
            &mut m,
            &Quat {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                w: 0.0,
            },
        );
        assert_eq!(&m, &matrix3_identity());

        // 90° about z: (1,0,0) -> (0,1,0)
        let t = std::f32::consts::FRAC_PI_4;
        let q = Quat {
            x: 0.0,
            y: 0.0,
            z: t.sin(),
            w: t.cos(),
        };
        matrix3_from_quat(&mut m, &q);
        assert!(within(m.x.x, 0.0));
        assert!(within(m.x.y, 1.0));
        assert!(within(m.y.x, -1.0));
        assert!(within(m.y.y, 0.0));
        assert_eq!(m.t, vec3::vec3_zero());
        assert_eq!(m.x.w, 0.0);

        // unnormalised quaternion: n = dot4(q,q) scaling (R31 formula)
        let q2 = Quat {
            x: q.x * 2.0,
            y: q.y * 2.0,
            z: q.z * 2.0,
            w: q.w * 2.0,
        };
        let mut m2 = matrix3_identity();
        matrix3_from_quat(&mut m2, &q2);
        assert!(within(m2.x.y, 1.0), "normalisation-invariant");

        // from_axisang (script 28: axis not normalised — the formula is
        // applied as-is, matching C: q = (0,0,√2·sin(π/4), cos(π/4)),
        // n = 2.5, s = 2/n = 0.8 -> x row (−0.6, 0.8, 0)
        let mut ma = matrix3_identity();
        matrix3_from_axisang(
            &mut ma,
            &AxisAng {
                x: 0.0,
                y: 0.0,
                z: 2.0, // non-unit axis — NOT normalised (R31 formula)
                w: std::f32::consts::FRAC_PI_2,
            },
        );
        assert!(within(ma.x.x, -0.6));
        assert!(within(ma.x.y, 0.8));
        assert!(within(ma.y.x, -0.8));
        assert!(within(ma.y.y, -0.6));
    }

    #[test]
    fn r32_matrix4_conversions() {
        // S-009-R32
        let m4 = Matrix4 {
            x: crate::vec4::vec4_set(1.0, 2.0, 3.0, 0.25),
            y: crate::vec4::vec4_set(4.0, 5.0, 6.0, 0.5),
            z: crate::vec4::vec4_set(7.0, 8.0, 9.0, 0.75),
            t: crate::vec4::vec4_set(10.0, 11.0, 12.0, 1.0),
        };
        let mut m3 = matrix3_identity();
        matrix3_from_matrix4(&mut m3, &m4);
        // w lanes of every row zeroed
        assert_eq!(
            m3,
            Matrix3 {
                x: vec3::vec3_set(1.0, 2.0, 3.0),
                y: vec3::vec3_set(4.0, 5.0, 6.0),
                z: vec3::vec3_set(7.0, 8.0, 9.0),
                t: vec3::vec3_set(10.0, 11.0, 12.0),
            }
        );
        // round trip keeps the w lanes of rows x,y,z and sets t.w = 1
        let mut back = m4;
        crate::matrix4::matrix4_from_matrix3(&mut back, &m3);
        assert_eq!(back.t.w, 1.0);
        let mut m3b = m3;
        m3b.x.w = 0.25;
        m3b.y.w = 0.5;
        m3b.z.w = 0.75;
        let mut back2 = m4;
        crate::matrix4::matrix4_from_matrix3(&mut back2, &m3b);
        assert_eq!(back2.x.w, 0.25);
        assert_eq!(back2.y.w, 0.5);
        assert_eq!(back2.z.w, 0.75);
        assert_eq!(back2.t.w, 1.0);
    }

    #[test]
    fn r33_quat_from_matrix3() {
        // S-009-R33: 90° about z back to a quaternion
        let q = Quat {
            x: 0.0,
            y: 0.0,
            z: 0.5f32.sin(),
            w: 0.5f32.cos(),
        };
        let mut m = matrix3_identity();
        matrix3_from_quat(&mut m, &q);
        let mut q2 = Quat {
            x: 0.0,
            y: 0.0,
            z: 0.0,
            w: 0.0,
        };
        quat_from_matrix3(&mut q2, &m);
        // same rotation (sign not pinned by R16's branches)
        let d = crate::quat::quat_dot(&q, &q2);
        assert!(within(d.abs(), 1.0));
        // the w lanes and t are not read
        let mut mw = m;
        mw.x.w = 9.0;
        mw.t = vec3::vec3_set(9.0, 9.0, 9.0);
        let mut q3 = Quat {
            x: 0.0,
            y: 0.0,
            z: 0.0,
            w: 0.0,
        };
        quat_from_matrix3(&mut q3, &mw);
        assert!(within(crate::quat::quat_dot(&q2, &q3), 1.0));
    }

    #[test]
    fn r35_mul_convention_q12() {
        // S-009-R35/Q-12: rows dotted with rows, translation subtracted
        // a = identity rotated 90° about z; b = translate (1,2,3)
        let qa = Quat {
            x: 0.0,
            y: 0.0,
            z: std::f32::consts::FRAC_PI_4.sin(),
            w: std::f32::consts::FRAC_PI_4.cos(),
        };
        let mut a = matrix3_identity();
        matrix3_from_quat(&mut a, &qa);
        let mut b = matrix3_identity();
        b.t = vec3::vec3_set(1.0, 2.0, 3.0);

        let mut d = matrix3_identity();
        matrix3_mul(&mut d, &a, &b);
        // d.r = a.r · b.rows (b's rotation is identity)
        assert!(within(d.x.x, 0.0));
        assert!(within(d.x.y, 1.0));
        assert!(within(d.y.x, -1.0));
        // d.t = (a.t − b.t) · b.rows = (0,0,0) − (1,2,3) = (−1,−2,−3)
        assert!(within(d.t.x, -1.0));
        assert!(within(d.t.y, -2.0));
        assert!(within(d.t.z, -3.0));
        assert_eq!(d.t.w, 0.0);
        assert_eq!(d.x.w, 0.0);
        // alias: d == a
        let a0 = a;
        matrix3_mul(&mut a, &a0, &b);
        assert!(within(a.t.x, -1.0));
        assert!(within(a.t.y, -2.0));
    }

    #[test]
    fn r36_translate_subtracts_only_t() {
        // S-009-R36: d.t = m.t − v; rows of d keep their PREVIOUS content
        let mut m = matrix3_identity();
        m.t = vec3::vec3_set(10.0, 20.0, 30.0);
        let mut d = matrix3_identity();
        d.x = vec3::vec3_set(9.0, 9.0, 9.0); // previous content
        matrix3_translate(&mut d, &m, &vec3::vec3_set(1.0, 2.0, 3.0));
        assert_eq!(d.t, vec3::vec3_set(9.0, 18.0, 27.0));
        assert_eq!(d.x, vec3::vec3_set(9.0, 9.0, 9.0), "x not written");
        // y/z keep their PREVIOUS content (identity rows), even d != m
        assert_eq!(d.y, vec3::vec3_set(0.0, 1.0, 0.0), "y not written");
        assert_eq!(d.z, vec3::vec3_set(0.0, 0.0, 1.0), "z not written");
        // translate3f
        let mut d2 = matrix3_identity();
        matrix3_translate3f(&mut d2, &m, 1.0, 2.0, 3.0);
        assert_eq!(d2.t, vec3::vec3_set(9.0, 18.0, 27.0));
    }

    #[test]
    fn r37_r38_rotate_scale() {
        // R37: rotate by 90° about z
        let mut m = matrix3_identity();
        m.t = vec3::vec3_set(5.0, 0.0, 0.0);
        let q = Quat {
            x: 0.0,
            y: 0.0,
            z: std::f32::consts::FRAC_PI_4.sin(),
            w: std::f32::consts::FRAC_PI_4.cos(),
        };
        let m0 = m;
        matrix3_rotate(&mut m, &m0, &q);
        // Q-12 product: d.r = (dot4(a.r, b.x), dot4(a.r, b.y), dot4(a.r, b.z))
        // with a = identity, b = 90° z-rotation (rows (0,1,0), (−1,0,0), (0,0,1))
        assert!(within(m.x.x, 0.0));
        assert!(within(m.x.y, -1.0));
        assert!(within(m.y.x, 1.0));
        // d.t = (a.t − b.t) rotated by b's rows: (5,0,0)·rows ≈ (0,−5,0)
        // (t.x is a derived zero: 5 × (1−(yy+zz)) ~ 1e-7 — absolute bound,
        // Miri's FMA contraction can push this beyond the relative 1e-6)
        assert!(m.t.x.abs() < 1e-5, "t.x derived zero, got {}", m.t.x);
        assert!(within(m.t.y, -5.0));
        // R38: scale (2,3,4) — all four lanes per row
        // (aliasing d == m is guaranteed by construction: all four rows
        // are computed into temporaries before any write — tested via
        // copy, as C-style self-aliasing has no Rust &mut spelling)
        let mut s = matrix3_identity();
        s.x.w = 2.0; // live w lane
        let s0 = s;
        matrix3_scale(
            &mut s,
            &s0,
            &Vec3 {
                x: 2.0,
                y: 3.0,
                z: 4.0,
                w: 10.0,
            },
        );
        assert_eq!(s.x.x, 2.0);
        assert_eq!(s.y.y, 3.0);
        assert_eq!(s.z.z, 4.0);
        assert_eq!(s.x.w, 20.0, "w = m.r.w · v.w (all four lanes)");
        // scale3f: v.w = 0 -> w lanes zero
        let mut s2 = s;
        matrix3_scale3f(&mut s2, &s, 1.0, 1.0, 1.0);
        assert_eq!(s2.x.w, 0.0);
        // R37 rotate_aa4f
        let mut ra = matrix3_identity();
        let ra0 = ra;
        matrix3_rotate_aa4f(&mut ra, &ra0, 0.0, 0.0, 2.0, std::f32::consts::FRAC_PI_2);
        // same formula as r31, transposed by the Q-12 product:
        // d.x = (op.x.x, op.y.x, op.z.x) = (−0.6, −0.8, 0)
        assert!(within(ra.x.y, -0.8), "non-unit axis not normalised (R31)");
    }

    #[test]
    fn r39_transpose_q3() {
        // S-009-R39/Q-3: w lanes of all three rows = m.z.w
        let mut m = Matrix3 {
            x: Vec3 {
                x: 1.0,
                y: 2.0,
                z: 3.0,
                w: 0.1,
            },
            y: Vec3 {
                x: 4.0,
                y: 5.0,
                z: 6.0,
                w: 0.2,
            },
            z: Vec3 {
                x: 7.0,
                y: 8.0,
                z: 9.0,
                w: 0.9,
            },
            t: Vec3 {
                x: 10.0,
                y: 11.0,
                z: 12.0,
                w: 0.3,
            },
        };
        let mut d = matrix3_identity();
        matrix3_transpose(&mut d, &m);
        assert_eq!(d.x.x, 1.0);
        assert_eq!(d.y.x, 2.0);
        assert_eq!(d.z.x, 3.0);
        assert_eq!(d.x.y, 4.0);
        assert_eq!(d.z.z, 9.0);
        assert_eq!(d.x.w, 0.9, "Q-3: w lane = m.z.w");
        assert_eq!(d.y.w, 0.9);
        assert_eq!(d.z.w, 0.9);
        // d.t = −(dot4(m.t,m.x), dot4(m.t,m.y), dot4(m.t,m.z))
        // dot4(t,x) = 10·1+11·2+12·3+0.3·0.1 = 68.03
        assert!(within(d.t.x, -68.03));
        // dot4(t,y) = 40+55+72+0.06 = 167.06
        assert!(within(d.t.y, -167.06));
        // dot4(t,z) = 70+88+108+0.27 = 266.27
        assert!(within(d.t.z, -266.27));
        assert_eq!(d.t.w, 0.0);
        // alias d == m (by construction; tested via copy)
        let m0 = m;
        matrix3_transpose(&mut m, &m0);
        assert_eq!(m.x.w, 0.9);
        // affine rigid check: zero w lanes -> classic transpose of the 3×3
        let mut r = matrix3_identity();
        r.x.y = 1.0;
        r.t = vec3::vec3_set(1.0, 2.0, 3.0);
        let mut ri = matrix3_identity();
        matrix3_transpose(&mut ri, &r);
        // d.x = (m.x.x, m.y.x, m.z.x, m.z.w) = (1,0,0,0);
        // d.y = (m.x.y, m.y.y, m.z.y, m.z.w) = (1,1,0,0)
        assert_eq!(ri.x, vec3::vec3_set(1.0, 0.0, 0.0));
        assert_eq!(ri.y, vec3::vec3_set(1.0, 1.0, 0.0));
        // d.t = −(dot4(t,x), dot4(t,y), dot4(t,z)) = −(3, 2, 3)
        assert!(within(ri.t.x, -3.0));
        assert!(within(ri.t.y, -2.0));
        assert!(within(ri.t.z, -3.0));
    }

    #[test]
    fn r40_inv() {
        // S-009-R40: ordinary 4x4 inverse of [x;y;z;t] (t.w = 1)
        let mut m = matrix3_identity();
        m.t = vec3::vec3_set(1.0, 2.0, 3.0);
        let mut d = matrix3_identity();
        assert!(matrix3_inv(&mut d, &m));
        // inverse of a pure translation is the negated translation
        assert!(within(d.t.x, -1.0));
        assert!(within(d.t.y, -2.0));
        assert!(within(d.t.z, -3.0));
        assert_eq!(d.t.w, 0.0, "unconditionally t.w = 0");
        assert_eq!(d.x.w, 0.0, "affine input -> w lanes 0");

        // D-1: singular input -> d unchanged except t.w = 0
        let mut sing = matrix3_identity();
        // scale x to 0 -> det 0
        sing.x = vec3::vec3_zero();
        sing.x.w = 7.0;
        let mut ds = Matrix3 {
            x: vec3::vec3_set(4.0, 4.0, 4.0),
            y: vec3::vec3_set(5.0, 5.0, 5.0),
            z: vec3::vec3_set(6.0, 6.0, 6.0),
            t: vec3::vec3_set(7.0, 7.0, 7.0),
        };
        assert!(!matrix3_inv(&mut ds, &sing));
        assert_eq!(ds.x, vec3::vec3_set(4.0, 4.0, 4.0), "D-1: rows untouched");
        assert_eq!(ds.y, vec3::vec3_set(5.0, 5.0, 5.0));
        assert_eq!(ds.z, vec3::vec3_set(6.0, 6.0, 6.0));
        assert_eq!(ds.t, vec3::vec3_set(7.0, 7.0, 7.0), "D-1: t xyz untouched");
        assert_eq!(ds.t.w, 0.0, "D-1: but t.w = 0");

        // alias d == m
        let mut al = m;
        let al0 = al;
        assert!(matrix3_inv(&mut al, &al0));
        assert!(within(al.t.x, -1.0));
    }

    #[test]
    fn r41_mirror() {
        // S-009-R41: mirror in the plane y = 0 (normal (0,1,0))
        let p = Plane {
            dir: vec3::vec3_set(0.0, 1.0, 0.0),
            dist: 0.0,
        };
        let mut m = matrix3_identity();
        m.t = vec3::vec3_set(1.0, 2.0, 3.0);
        let mut d = matrix3_identity();
        matrix3_mirror(&mut d, &m, &p);
        // rows: mirrorv each row in the y=0 plane: (x,y,z) -> (x,-y,z)
        assert_eq!(d.x, vec3::vec3_set(1.0, 0.0, 0.0));
        assert_eq!(d.y, vec3::vec3_set(0.0, -1.0, 0.0));
        assert_eq!(d.z, vec3::vec3_set(0.0, 0.0, 1.0));
        // t: (1,2,3) mirrored in y=0 -> (1,-2,3)
        assert_eq!(d.t, vec3::vec3_set(1.0, -2.0, 3.0));
        // mirrorv: all four rows against a bare normal
        let mut mv = m;
        matrix3_mirrorv(&mut mv, &m, &vec3::vec3_set(0.0, 1.0, 0.0));
        assert_eq!(mv.y, vec3::vec3_set(0.0, -1.0, 0.0));
        assert_eq!(mv.t, vec3::vec3_set(1.0, -2.0, 3.0));
    }
}
