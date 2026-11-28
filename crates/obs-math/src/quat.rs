//! `quat` kernel (S-008; `libobs/graphics/quat.{h,c}`).
//!
//! The type and the matrix-composition helpers (R10, R14) landed with the
//! matrix ports; the full quaternion set (R13, R15–R21: mul,
//! from_matrix4, get_dir, set_look_dir, log/exp, interpolation, tangent,
//! interpolate_cubic) landed with WP-43 (P1-15.4), which swaps
//! `quat.c`/`axisang.c` into the Rust staticlib — the conformance
//! `math`/`math2` vectors then verify every exported function in swap
//! mode. The C header's `static inline` helpers (R10, plus the
//! `quat_set`/`quat_inv` defects D-1/D-2) stay C-side.

use crate::axisang::AxisAng;
use crate::vec3::{
    Vec3, vec3_add, vec3_copy, vec3_cross, vec3_dot, vec3_mulf, vec3_neg, vec3_norm,
};
use crate::{EPSILON, close_float};

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

// ── S-008 R13–R21: the quat.c exports (WP-43, P1-15.4) ─────────────────

/// The C `quat_vec3` helper: the vector part of a quaternion.
#[must_use]
fn quat_vec3(q: &Quat) -> Vec3 {
    Vec3 {
        x: q.x,
        y: q.y,
        z: q.z,
        w: 0.0,
    }
}

/// S-008-R13: `quat_mul(d, q1, q2)` — Hamilton product, `q1` as the
/// *left* factor: `d.xyz = q1.w·a2 + q2.w·a1 + a1 × a2`;
/// `d.w = q1.w·q2.w − a1·a2` (vector `06` `mul` vs `mul2`). `d` may
/// alias `q1`/`q2` (the C code computes through temporaries first).
pub fn quat_mul(d: &mut Quat, q1: &Quat, q2: &Quat) {
    let a1 = quat_vec3(q1);
    let a2 = quat_vec3(q2);
    let t1 = vec3_add(&vec3_mulf(&a2, q1.w), &vec3_mulf(&a1, q2.w));
    let r = vec3_add(&t1, &vec3_cross(&a1, &a2));
    *d = Quat {
        x: r.x,
        y: r.y,
        z: r.z,
        w: q1.w * q2.w - vec3_dot(&a1, &a2),
    };
}

/// S-008-R16: `quat_from_matrix4(d, m)` (the C `f4x4` cast reads rows
/// `x,y,z`, lanes `0..2`; the `t` row is never read). Vector `06`.
pub fn quat_from_matrix4(d: &mut Quat, m: &crate::matrix4::Matrix4) {
    let rows = [m.x, m.y, m.z];
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
        ds[j] = (el(i, j) + el(j, i)) * h;
        ds[k] = (el(i, k) + el(k, i)) * h;
        d.x = ds[0];
        d.y = ds[1];
        d.z = ds[2];
        d.w = (el(j, k) - el(k, j)) * h;
    }
}

/// S-008-R17: `quat_get_dir(d, q)` — `matrix3_from_quat(q)`'s `z` row
/// (the rotated +Z axis; vector `06` `dir`).
pub fn quat_get_dir(d: &mut Vec3, q: &Quat) {
    let mut m = crate::matrix3::matrix3_identity();
    crate::matrix3::matrix3_from_quat(&mut m, q);
    vec3_copy(d, &m.z);
}

/// S-008-R18: `quat_set_look_dir(d, dir)` — the inverted-direction
/// yaw/pitch decomposition; the validity predicates test **closeness to
/// zero** (Q-3), so a direction like `(1,1,1)` yields the identity
/// (vector `08`).
pub fn quat_set_look_dir(d: &mut Quat, dir: &Vec3) {
    let n = vec3_neg(&vec3_norm(dir));
    let xz_valid = close_float(n.x, 0.0, EPSILON) || close_float(n.z, 0.0, EPSILON);
    let yz_valid = close_float(n.y, 0.0, EPSILON);
    let mut xz_rot = quat_identity();
    let mut yz_rot = quat_identity();
    if xz_valid {
        quat_from_axisang(
            &mut xz_rot,
            &crate::axisang::axisang_set(0.0, 1.0, 0.0, n.x.atan2(n.z)),
        );
    }
    if yz_valid {
        quat_from_axisang(
            &mut yz_rot,
            &crate::axisang::axisang_set(-1.0, 0.0, 0.0, n.y.asin()),
        );
    }
    if !xz_valid {
        *d = yz_rot;
    } else if !yz_valid {
        *d = xz_rot;
    } else {
        quat_mul(d, &xz_rot, &yz_rot);
    }
}

/// S-008-R19: `quat_log(d, q)` — `angle = acos(q.w)`, `sine =
/// sin(angle)`; `d = q` with `w = 0`; if `|q.w| < 1 && |sine| ≥
/// EPSILON` multiply all lanes by `angle/sine` (incl. the zeroed `w`).
pub fn quat_log(d: &mut Quat, q: &Quat) {
    let angle = q.w.acos();
    let mut sine = angle.sin();
    let w = q.w;
    *d = *q;
    d.w = 0.0;
    if w.abs() < 1.0 && sine.abs() >= EPSILON {
        sine = angle / sine;
        *d = Quat {
            x: d.x * sine,
            y: d.y * sine,
            z: d.z * sine,
            w: d.w * sine,
        };
    }
}

/// S-008-R19: `quat_exp(d, q)` — `len = |q.xyz|`; `f = len > EPSILON ?
/// sin(len)/len : 1`; `d = q · f` (all lanes); then `d.w = cos(len)`.
pub fn quat_exp(d: &mut Quat, q: &Quat) {
    let len = (q.x * q.x + q.y * q.y + q.z * q.z).sqrt();
    *d = *q;
    let f = if len > EPSILON { len.sin() / len } else { 1.0 };
    d.x *= f;
    d.y *= f;
    d.z *= f;
    d.w = len.cos();
}

/// S-008-R20: `quat_interpolate(d, q1, q2, t)` — slerp without
/// shortest-arc correction and **no clamping of the dot** (Q-4; `|dot| > 1`
/// gives NaN): `a = acos(dot)`; if `a ≥ EPSILON`: `d = q1·sin(a(1−t))/sin(a)
/// + q2·sin(at)/sin(a)`; else linear (vector `08`).
pub fn quat_interpolate(d: &mut Quat, q1: &Quat, q2: &Quat, t: f32) {
    let a = quat_dot(q1, q2).acos();
    if a >= EPSILON {
        let sine = a.sin();
        let sinet = (a * t).sin() / sine;
        let sineti = (a * (1.0 - t)).sin() / sine;
        let temp = Quat {
            x: q1.x * sineti,
            y: q1.y * sineti,
            z: q1.z * sineti,
            w: q1.w * sineti,
        };
        *d = Quat {
            x: temp.x + q2.x * sinet,
            y: temp.y + q2.y * sinet,
            z: temp.z + q2.z * sinet,
            w: temp.w + q2.w * sinet,
        };
    } else {
        // C `quat_sub`/`quat_add` are four-lane (R10) — `w` included;
        // `vec3_sub` zeroes `w`, so the lane is explicit here.
        let dx = q2.x - q1.x;
        let dy = q2.y - q1.y;
        let dz = q2.z - q1.z;
        let dw = q2.w - q1.w;
        *d = Quat {
            x: q1.x + dx * t,
            y: q1.y + dy * t,
            z: q1.z + dz * t,
            w: q1.w + dw * t,
        };
    }
}

/// S-008-R21: `quat_get_tangent(d, prev, q, next) = ((q − prev) + next
/// − q) · 0.5` (the C `vec3` ops keep the `w` lanes: `temp.w` goes
/// `q.w − prev.w + next.w − q.w = next.w − prev.w` scaled by `0.5`).
pub fn quat_get_tangent(d: &mut Quat, prev: &Quat, q: &Quat, next: &Quat) {
    let temp = Quat {
        x: q.x - prev.x + next.x - q.x,
        y: q.y - prev.y + next.y - q.y,
        z: q.z - prev.z + next.z - q.z,
        w: q.w - prev.w + next.w - q.w,
    };
    *d = Quat {
        x: temp.x * 0.5,
        y: temp.y * 0.5,
        z: temp.z * 0.5,
        w: temp.w * 0.5,
    };
}

/// S-008-R21: `quat_interpolate_cubic(d, q1, q2, m1, m2, t) =
/// interpolate(interpolate(q1,q2,t), interpolate(m1,m2,t), 2(1−t)t)`
/// (vector `08`).
pub fn quat_interpolate_cubic(d: &mut Quat, q1: &Quat, q2: &Quat, m1: &Quat, m2: &Quat, t: f32) {
    let mut temp1 = quat_identity();
    let mut temp2 = quat_identity();
    quat_interpolate(&mut temp1, q1, q2, t);
    quat_interpolate(&mut temp2, m1, m2, t);
    quat_interpolate(d, &temp1, &temp2, 2.0 * (1.0 - t) * t);
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

    fn within4(a: &Quat, b: &Quat) -> bool {
        within(a.x, b.x) && within(a.y, b.y) && within(a.z, b.z) && within(a.w, b.w)
    }

    #[test]
    fn mul_matches_c_reference() {
        // S-008-R13, vector `06`: the two fixed quaternions a=(0.1,0.2,0.3,0.9),
        // b=(0.5,-0.5,0.5,0.5). Expected `mul` (a·b) and `mul2` (b·a) computed
        // by the same formula the C reference uses (independent re-derivation).
        let a = Quat {
            x: 0.1,
            y: 0.2,
            z: 0.3,
            w: 0.9,
        };
        let b = Quat {
            x: 0.5,
            y: -0.5,
            z: 0.5,
            w: 0.5,
        };
        let mut r = quat_identity();
        quat_mul(&mut r, &a, &b);
        let a1 = quat_vec3(&a);
        let a2 = quat_vec3(&b);
        let want_xyz = vec3_add(
            &vec3_add(&vec3_mulf(&a2, a.w), &vec3_mulf(&a1, b.w)),
            &vec3_cross(&a1, &a2),
        );
        let want = Quat {
            x: want_xyz.x,
            y: want_xyz.y,
            z: want_xyz.z,
            w: a.w * b.w - vec3_dot(&a1, &a2),
        };
        // cross-check the scalar part against a hand value: a.w*b.w - a1.a2
        // = 0.9*0.5 - (0.1*0.5 + 0.2*-0.5 + 0.3*0.5) = 0.45 - (0.05-0.1+0.15)=0.45-0.10
        assert!(within(r.w, 0.35));
        assert!(within4(&r, &want));
        // non-commutativity: mul2 = b·a must differ in the vector part
        let mut r2 = quat_identity();
        quat_mul(&mut r2, &b, &a);
        assert!(within(r2.w, 0.35));
        assert!(!within(r.x, r2.x) || !within(r.y, r2.y) || !within(r.z, r2.z));
    }

    #[test]
    fn from_matrix4_roundtrip() {
        // S-008-R16, vector `06`: matrix4_from_quat -> quat_from_matrix4
        // recovers the quaternion (up to the overall sign, which the
        // reference also leaves sign-ambiguous); we compare components.
        let qy = Quat {
            x: 0.0,
            y: std::f32::consts::FRAC_1_SQRT_2,
            z: 0.0,
            w: std::f32::consts::FRAC_1_SQRT_2,
        };
        let mut m = crate::matrix4::matrix4_identity();
        crate::matrix4::matrix4_from_quat(&mut m, &qy);
        let mut back = quat_identity();
        quat_from_matrix4(&mut back, &m);
        // from_matrix4 returns the same orientation; allow the ± sign.
        assert!(
            within4(&back, &qy)
                || within4(
                    &back,
                    &Quat {
                        x: -qy.x,
                        y: -qy.y,
                        z: -qy.z,
                        w: -qy.w
                    }
                )
        );
        // the 180°-about-Z flip (vector `06` `back3`) has tr = -1 < 0 and
        // must take the diagonal branch; the recovered quat is (0,0,±1,0).
        let flip = crate::matrix4::Matrix4 {
            x: crate::vec4::vec4_set(-1.0, 0.0, 0.0, 0.0),
            y: crate::vec4::vec4_set(0.0, -1.0, 0.0, 0.0),
            z: crate::vec4::vec4_set(0.0, 0.0, 1.0, 0.0),
            t: crate::vec4::vec4_set(0.0, 0.0, 0.0, 1.0),
        };
        let mut back3 = quat_identity();
        quat_from_matrix4(&mut back3, &flip);
        assert!(within(back3.x, 0.0) && within(back3.y, 0.0));
        assert!(within(back3.z, 1.0) || within(back3.z, -1.0));
        assert!(within(back3.w, 0.0));
    }

    #[test]
    fn get_dir_is_rotated_z() {
        // S-008-R17, vector `06` `dir`: qy = 45° about +Y maps +Z to (sin45,0,cos45).
        let qy = Quat {
            x: 0.0,
            y: (std::f32::consts::FRAC_PI_4 / 2.0).sin(),
            z: 0.0,
            w: (std::f32::consts::FRAC_PI_4 / 2.0).cos(),
        };
        let mut d = crate::vec3::vec3_zero();
        quat_get_dir(&mut d, &qy);
        let s45 = std::f32::consts::FRAC_PI_4;
        assert!(within(d.x, s45.sin()));
        assert!(within(d.y, 0.0));
        assert!(within(d.z, s45.cos()));
        // identity -> +Z
        let mut d0 = crate::vec3::vec3_zero();
        quat_get_dir(&mut d0, &quat_identity());
        assert!(within(d0.x, 0.0) && within(d0.y, 0.0) && within(d0.z, 1.0));
    }

    #[test]
    fn set_look_dir_vectors() {
        // S-008-R18, vector `08`: the four look directions. Q-3: the
        // predicates test closeness to ZERO, so (1,1,1) -> identity.
        let d = crate::vec3::vec3_set(0.0, 0.0, -1.0);
        let mut r = quat_identity();
        quat_set_look_dir(&mut r, &d);
        let d2 = crate::vec3::vec3_set(1.0, 0.0, 0.0);
        let mut r2 = quat_identity();
        quat_set_look_dir(&mut r2, &d2);
        let d3 = crate::vec3::vec3_set(0.0, 1.0, 0.0);
        let mut r3 = quat_identity();
        quat_set_look_dir(&mut r3, &d3);
        let d4 = crate::vec3::vec3_set(1.0, 1.0, 1.0);
        let mut r4 = quat_identity();
        quat_set_look_dir(&mut r4, &d4);
        // (1,1,1): no component close to zero -> xz_valid && yz_valid both
        // false is NOT the case; both valid false => result = xz_rot (identity)
        // because !yz_valid. Verify it is the identity (all lanes).
        assert!(within4(&r4, &quat_identity()));
        // The three axis-aligned directions must be finite (no NaN from the
        // atan2/asin of a component that is close to zero).
        for (x, y, z, w) in [
            (r.x, r.y, r.z, r.w),
            (r2.x, r2.y, r2.z, r2.w),
            (r3.x, r3.y, r3.z, r3.w),
        ] {
            assert!(x.is_finite() && y.is_finite() && z.is_finite() && w.is_finite());
        }
    }

    #[test]
    fn log_exp_roundtrip() {
        // S-008-R19, vector `08`: ex = exp(log(b)) ~= b for |w| < 1.
        let b = Quat {
            x: 0.0,
            y: std::f32::consts::FRAC_1_SQRT_2,
            z: 0.0,
            w: std::f32::consts::FRAC_1_SQRT_2,
        };
        let mut lg = quat_identity();
        quat_log(&mut lg, &b);
        assert!(within(lg.w, 0.0));
        let mut ex = quat_identity();
        quat_exp(&mut ex, &lg);
        assert!(within4(&ex, &b));
    }

    #[test]
    fn interpolate_endpoints_and_identity() {
        // S-008-R20, vector `08`: t=0 -> q1, t=1 -> q2, q1==q2 -> q1.
        let a = quat_identity();
        let b = Quat {
            x: 0.0,
            y: std::f32::consts::FRAC_1_SQRT_2,
            z: 0.0,
            w: std::f32::consts::FRAC_1_SQRT_2,
        };
        let mut i0 = quat_identity();
        quat_interpolate(&mut i0, &a, &b, 0.0);
        assert!(within4(&i0, &a));
        let mut i1 = quat_identity();
        quat_interpolate(&mut i1, &a, &b, 1.0);
        assert!(within4(&i1, &b));
        // same -> linear branch (dot = 1, angle = 0 < EPSILON)
        let mut same = quat_identity();
        quat_interpolate(&mut same, &a, &a, 0.5);
        assert!(within4(&same, &a));
    }

    #[test]
    fn get_tangent_formula() {
        // S-008-R21, vector `08`: ((q - prev) + next - q) * 0.5, four-lane.
        let prev = Quat {
            x: 0.1,
            y: 0.2,
            z: 0.3,
            w: 0.9,
        };
        let q = Quat {
            x: 0.4,
            y: 0.5,
            z: 0.6,
            w: 0.3,
        };
        let next = Quat {
            x: 0.7,
            y: 0.1,
            z: 0.2,
            w: 0.5,
        };
        let mut t = quat_identity();
        quat_get_tangent(&mut t, &prev, &q, &next);
        let want = Quat {
            x: (q.x - prev.x + next.x - q.x) * 0.5,
            y: (q.y - prev.y + next.y - q.y) * 0.5,
            z: (q.z - prev.z + next.z - q.z) * 0.5,
            w: (q.w - prev.w + next.w - q.w) * 0.5,
        };
        assert!(within4(&t, &want));
    }

    #[test]
    fn interpolate_cubic_endpoints() {
        // S-008-R21, vector `08`: at t=0 the cubic returns q1 regardless of
        // the control tangents m1/m2 (inner interpolates collapse to q1/m1,
        // outer interpolate(q1, m1, 0) = q1).
        let q1 = Quat {
            x: 0.1,
            y: 0.2,
            z: 0.3,
            w: 0.9,
        };
        let q2 = Quat {
            x: 0.5,
            y: -0.5,
            z: 0.5,
            w: 0.5,
        };
        let m1 = quat_identity();
        let m2 = quat_identity();
        let mut r = quat_identity();
        quat_interpolate_cubic(&mut r, &q1, &q2, &m1, &m2, 0.0);
        assert!(within4(&r, &q1));
    }
}
