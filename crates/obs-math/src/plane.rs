//! `plane` (S-009 §3, R50–R58; `libobs/graphics/plane.{h,c}`).
//!
//! Layout: `dir` (a `vec3`, 16 bytes, `w` lane live) at offset 0 and
//! `dist` (`f32`) at offset 16 — 32 bytes total. A plane is the set of
//! points `p` with `dot4(p, dir) = dist` (R50).

use crate::vec3::{self, Vec3};
use crate::{EPSILON, close_float, vec3_close};

/// S-009 §3: the C `struct plane`.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Plane {
    pub dir: Vec3,
    pub dist: f32,
}

/// S-009-R50: `plane_set(d, dir, dist)` copies `dir` (four lanes) and
/// `dist`.
pub fn plane_set(d: &mut Plane, dir: &Vec3, dist: f32) {
    d.dir = *dir;
    d.dist = dist;
}

/// S-009-R50: `plane_setf(d, a, b, c, dist)` stores `dir = (a, b, c, 0)`.
pub fn plane_setf(d: &mut Plane, a: f32, b: f32, c: f32, dist: f32) {
    d.dir = vec3::vec3_set(a, b, c);
    d.dist = dist;
}

/// S-009-R50: `plane_copy(d, p)` copies both fields.
pub fn plane_copy(d: &mut Plane, p: &Plane) {
    *d = *p;
}

// ── S-009-R51–R58 ────────────────────────────────────────────────────

/// S-009-R51: `e1 = v2 − v1`, `e2 = v3 − v1`, `n = cross(e1, e2)`,
/// `d.dir = norm3(n)`, `d.dist = dot4(v1, d.dir)`. A degenerate
/// triangle gives `dir = (0,0,0,0)`, `dist = 0` (via `vec3_norm`).
pub fn plane_from_tri(d: &mut Plane, v1: &Vec3, v2: &Vec3, v3: &Vec3) {
    let e1 = vec3::vec3_sub(v2, v1);
    let e2 = vec3::vec3_sub(v3, v1);
    let n = vec3::vec3_cross(&e1, &e2);
    d.dir = vec3::vec3_norm(&n);
    d.dist = vec3::vec3_dot(v1, &d.dir);
}

/// S-009-R52/Q-4: `d.dir = norm3(vec3_transform(p.dir, m))` — the
/// **point** transform (`w = 1`), so the translation of `m` is added
/// to the normal; `o = vec3_transform((0,0,0), m)` (= the xyz of
/// `m.t`); `d.dist = p.dist − dot4(d.dir, o)`. `d` may alias `p`.
pub fn plane_transform(d: &mut Plane, p: &Plane, m: &crate::matrix4::Matrix4) {
    let dir = vec3::vec3_transform(&p.dir, m);
    d.dir = vec3::vec3_norm(&dir);
    let o = vec3::vec3_transform(&vec3::vec3_zero(), m);
    d.dist = p.dist - vec3::vec3_dot(&d.dir, &o);
}

/// S-009-R53/Q-4: `d.dir = norm3(vec3_transform3x4(p.dir, m))` (the
/// normal is treated as a point: `(dir − m.t)` dotted with the rows);
/// `o = vec3_transform3x4(m.t, m)` is the dot of `m.t − m.t = 0`, so
/// **`o` is always zero** and `d.dist = p.dist` unchanged. `d` may
/// alias `p`.
pub fn plane_transform3x4(d: &mut Plane, p: &Plane, m: &crate::matrix3::Matrix3) {
    let dir = vec3::vec3_transform3x4(&p.dir, m);
    d.dir = vec3::vec3_norm(&dir);
    d.dist = p.dist;
}

/// S-009-R54: `c = dot4(p.dir, dir)`; `abs(c) < EPSILON` (strict) →
/// `*t = 0`, false. Otherwise `*t = (p.dist − dot4(p.dir, orig)) / c`,
/// true. `t` may be negative (the line, not the half-line).
pub fn plane_intersection_ray(p: &Plane, orig: &Vec3, dir: &Vec3, t: &mut f32) -> bool {
    let c = vec3::vec3_dot(&p.dir, dir);
    if c.abs() < EPSILON {
        *t = 0.0;
        return false;
    }
    *t = (p.dist - vec3::vec3_dot(&p.dir, orig)) / c;
    true
}

/// S-009-R55: the four steps of the spec; `*t` is only written when the
/// function returns true (and in step 1 when `d2` is off the plane).
pub fn plane_intersection_line(p: &Plane, v1: &Vec3, v2: &Vec3, t: &mut f32) -> bool {
    let d1 = vec3::vec3_plane_dist(v1, p);
    let d2 = vec3::vec3_plane_dist(v2, p);
    if close_float(d1, 0.0, EPSILON) {
        if close_float(d2, 0.0, EPSILON) {
            return false;
        }
        *t = 0.0;
        return true;
    }
    if close_float(d2, 0.0, EPSILON) {
        *t = 1.0;
        return true;
    }
    if (d1 > 0.0) == (d2 > 0.0) {
        return false;
    }
    let a = d1.abs();
    let s = a + d2.abs();
    if s < EPSILON {
        return false;
    }
    *t = a / s;
    true
}

/// S-009-R56/Q-5: a vertex contributes a flag when `d >= prec` or
/// `d <= −prec` (nothing in between); the result is true when **any**
/// flag is set — with `prec = 0` every finite vertex sets one.
fn plane_vertex_flag(d: f32, prec: f32) -> bool {
    d >= prec || d <= -prec
}

/// S-009-R56 (Q-5): true when at least one vertex is at least `prec`
/// away from the plane on either side — NOT "inside".
pub fn plane_tri_inside(p: &Plane, v1: &Vec3, v2: &Vec3, v3: &Vec3, prec: f32) -> bool {
    plane_vertex_flag(vec3::vec3_plane_dist(v1, p), prec)
        || plane_vertex_flag(vec3::vec3_plane_dist(v2, p), prec)
        || plane_vertex_flag(vec3::vec3_plane_dist(v3, p), prec)
}

/// S-009-R56 (Q-5): the two-vertex form.
pub fn plane_line_inside(p: &Plane, v1: &Vec3, v2: &Vec3, prec: f32) -> bool {
    plane_vertex_flag(vec3::vec3_plane_dist(v1, p), prec)
        || plane_vertex_flag(vec3::vec3_plane_dist(v2, p), prec)
}

/// S-009-R57: the first test is the **signed** `vec3_close` (S-008 R06,
/// Q-6), then `close_float` on the distances.
pub fn plane_close(a: &Plane, b: &Plane, prec: f32) -> bool {
    vec3_close(&a.dir, &b.dir, prec) && close_float(a.dist, b.dist, prec)
}

/// S-009-R58: `c = dot4(a.dir, b.dir)`; parallel → compare distances,
/// anti-parallel → compare `−a.dist` to `b.dist`; else false. Normals
/// are assumed unit.
pub fn plane_coplanar(a: &Plane, b: &Plane, prec: f32) -> bool {
    let c = vec3::vec3_dot(&a.dir, &b.dir);
    if close_float(c, 1.0, prec) {
        return close_float(a.dist, b.dist, prec);
    }
    if close_float(c, -1.0, prec) {
        return close_float(-a.dist, b.dist, prec);
    }
    false
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layout() {
        use std::mem::{align_of, offset_of, size_of};
        assert_eq!(size_of::<Plane>(), 32);
        assert_eq!(align_of::<Plane>(), 16);
        assert_eq!(offset_of!(Plane, dir), 0);
        assert_eq!(offset_of!(Plane, dist), 16);
    }

    #[test]
    fn set_setf_copy() {
        // S-009-R50
        let mut p = Plane {
            dir: Vec3 {
                x: 9.0,
                y: 9.0,
                z: 9.0,
                w: 9.0,
            },
            dist: 9.0,
        };
        let dir = Vec3 {
            x: 0.0,
            y: 1.0,
            z: 0.0,
            w: 5.0, // w lane copied as-is by plane_set
        };
        plane_set(&mut p, &dir, 2.0);
        assert_eq!(p.dir, dir);
        assert_eq!(p.dist, 2.0);

        plane_setf(&mut p, 1.0, 2.0, 3.0, -4.0);
        assert_eq!(
            p,
            Plane {
                dir: vec3::vec3_set(1.0, 2.0, 3.0),
                dist: -4.0
            }
        );

        let mut q = Plane {
            dir: vec3::vec3_zero(),
            dist: 0.0,
        };
        plane_copy(&mut q, &p);
        assert_eq!(q, p);
    }

    #[test]
    fn r51_from_tri() {
        // S-009-R51: (0,0,0),(1,0,0),(0,1,0) -> plane z = 0
        let mut d = Plane {
            dir: vec3::vec3_set(9.0, 9.0, 9.0),
            dist: 9.0,
        };
        plane_from_tri(
            &mut d,
            &vec3::vec3_zero(),
            &vec3::vec3_set(1.0, 0.0, 0.0),
            &vec3::vec3_set(0.0, 1.0, 0.0),
        );
        assert_eq!(d.dir, vec3::vec3_set(0.0, 0.0, 1.0));
        assert_eq!(d.dist, 0.0);
        // offset triangle: z = 2, winding keeps the normal +z
        plane_from_tri(
            &mut d,
            &vec3::vec3_set(0.0, 0.0, 2.0),
            &vec3::vec3_set(1.0, 0.0, 2.0),
            &vec3::vec3_set(0.0, 1.0, 2.0),
        );
        assert_eq!(d.dir, vec3::vec3_set(0.0, 0.0, 1.0));
        assert_eq!(d.dist, 2.0);
        // degenerate: e1 = 0 -> n = 0 -> dir = (0,0,0,0), dist = 0
        plane_from_tri(
            &mut d,
            &vec3::vec3_set(1.0, 1.0, 1.0),
            &vec3::vec3_set(1.0, 1.0, 1.0),
            &vec3::vec3_set(2.0, 2.0, 2.0),
        );
        assert_eq!(d.dir, vec3::vec3_zero());
        assert_eq!(d.dist, 0.0);
        // reversed winding flips the normal
        plane_from_tri(
            &mut d,
            &vec3::vec3_zero(),
            &vec3::vec3_set(0.0, 1.0, 0.0),
            &vec3::vec3_set(1.0, 0.0, 0.0),
        );
        assert_eq!(d.dir, vec3::vec3_set(0.0, 0.0, -1.0));
        assert_eq!(d.dist, 0.0);
    }

    fn within(a: f32, b: f32) -> bool {
        let d = (a - b).abs();
        d <= 1e-6 || d <= 1e-6 * a.abs().max(b.abs())
    }

    #[test]
    fn r52_transform_matrix4() {
        // S-009-R52: 90° about z, plane x = 5 -> plane y = 5
        use crate::matrix4::{matrix4_from_quat, matrix4_identity};
        let mut m = matrix4_identity();
        let q = crate::quat::Quat {
            x: 0.0,
            y: 0.0,
            z: std::f32::consts::FRAC_PI_4.sin(),
            w: std::f32::consts::FRAC_PI_4.cos(),
        };
        matrix4_from_quat(&mut m, &q);
        let p = Plane {
            dir: vec3::vec3_set(1.0, 0.0, 0.0),
            dist: 5.0,
        };
        let mut d = Plane {
            dir: vec3::vec3_zero(),
            dist: 0.0,
        };
        plane_transform(&mut d, &p, &m);
        // x is a derived zero (1 − (yy+zz) ~ 1e-7, amplified by the
        // normalisation under Miri's FMA contraction) — absolute bound
        assert!(d.dir.x.abs() < 1e-5, "x derived zero, got {}", d.dir.x);
        assert!(within(d.dir.y, 1.0));
        assert_eq!(d.dir.z, 0.0);
        assert!(within(d.dist, 5.0));

        // Q-4: with a translation, the normal is transformed as a POINT
        use crate::matrix4::matrix4_translate3v;
        let mut t4 = matrix4_identity();
        let t40 = t4;
        matrix4_translate3v(&mut t4, &t40, &vec3::vec3_set(10.0, 20.0, 30.0));
        let p2 = Plane {
            dir: vec3::vec3_set(0.0, 0.0, 1.0),
            dist: 0.0,
        };
        plane_transform(&mut d, &p2, &t4);
        // dir' = norm3((0,0,1) + (10,20,30)) = norm3((10,20,31))
        let n = (10.0_f32 * 10.0 + 20.0 * 20.0 + 31.0 * 31.0).sqrt();
        assert!(within(d.dir.x, 10.0 / n));
        assert!(within(d.dir.y, 20.0 / n));
        assert!(within(d.dir.z, 31.0 / n));
        // dist' = 0 − dot(dir', (10,20,30)) = −(100+400+930)/n
        assert!(within(d.dist, -1430.0 / n));
    }

    #[test]
    fn r53_transform3x4_dist_unchanged() {
        // S-009-R53: o is always zero -> dist unchanged
        let m = crate::matrix3::Matrix3 {
            x: vec3::vec3_set(1.0, 0.0, 0.0),
            y: vec3::vec3_set(0.0, 1.0, 0.0),
            z: vec3::vec3_set(0.0, 0.0, 1.0),
            t: vec3::vec3_set(1.0, 2.0, 3.0),
        };
        let p = Plane {
            dir: vec3::vec3_set(0.0, 1.0, 0.0),
            dist: 5.0,
        };
        let mut d = Plane {
            dir: vec3::vec3_zero(),
            dist: 0.0,
        };
        plane_transform3x4(&mut d, &p, &m);
        // Q-4: dir' = norm3((0,1,0) − (1,2,3)) = norm3((−1,−1,−3))
        let n = (1.0_f32 + 1.0 + 9.0).sqrt();
        assert!(within(d.dir.x, -1.0 / n));
        assert!(within(d.dir.y, -1.0 / n));
        assert!(within(d.dir.z, -3.0 / n));
        assert!(within(d.dist, 5.0), "dist unchanged");
    }

    #[test]
    fn r54_intersection_ray() {
        // S-009-R54
        let p = Plane {
            dir: vec3::vec3_set(0.0, 0.0, 1.0),
            dist: 2.0,
        };
        let mut t = -99.0;
        assert!(plane_intersection_ray(
            &p,
            &vec3::vec3_zero(),
            &vec3::vec3_set(0.0, 0.0, 1.0),
            &mut t
        ));
        assert_eq!(t, 2.0);
        // negative t (line, not half-line)
        assert!(plane_intersection_ray(
            &p,
            &vec3::vec3_zero(),
            &vec3::vec3_set(0.0, 0.0, -1.0),
            &mut t
        ));
        assert_eq!(t, -2.0);
        // parallel: *t = 0, false
        assert!(!plane_intersection_ray(
            &p,
            &vec3::vec3_zero(),
            &vec3::vec3_set(1.0, 0.0, 0.0),
            &mut t
        ));
        assert_eq!(t, 0.0);
        // strict: |c| == EPSILON is NOT < EPSILON -> intersects
        let c = crate::EPSILON;
        let mut t2 = -99.0;
        let p2 = Plane {
            dir: Vec3 {
                x: c,
                y: 0.0,
                z: 0.0,
                w: 0.0,
            },
            dist: 0.0,
        };
        assert!(plane_intersection_ray(
            &p2,
            &vec3::vec3_zero(),
            &vec3::vec3_set(1.0, 0.0, 0.0),
            &mut t2
        ));
    }

    #[test]
    fn r55_intersection_line() {
        // S-009-R55
        let p = Plane {
            dir: vec3::vec3_set(0.0, 0.0, 1.0),
            dist: 0.0,
        };
        let mut t = -99.0;
        // crossing at the midpoint: (0,0,−1)-(0,0,1) -> t = 0.5
        assert!(plane_intersection_line(
            &p,
            &vec3::vec3_set(0.0, 0.0, -1.0),
            &vec3::vec3_set(0.0, 0.0, 1.0),
            &mut t
        ));
        assert_eq!(t, 0.5);
        // both on the same side -> false, *t untouched
        assert!(!plane_intersection_line(
            &p,
            &vec3::vec3_set(0.0, 0.0, 1.0),
            &vec3::vec3_set(0.0, 0.0, 2.0),
            &mut t
        ));
        assert_eq!(t, 0.5, "*t untouched");
        // v1 on the plane -> t = 0
        assert!(plane_intersection_line(
            &p,
            &vec3::vec3_set(1.0, 0.0, 0.0),
            &vec3::vec3_set(0.0, 0.0, 1.0),
            &mut t
        ));
        assert_eq!(t, 0.0);
        // v2 on the plane -> t = 1
        assert!(plane_intersection_line(
            &p,
            &vec3::vec3_set(0.0, 0.0, 1.0),
            &vec3::vec3_set(1.0, 0.0, 0.0),
            &mut t
        ));
        assert_eq!(t, 1.0);
        // both on the plane -> false, *t untouched
        assert!(!plane_intersection_line(
            &p,
            &vec3::vec3_set(1.0, 0.0, 0.0),
            &vec3::vec3_set(2.0, 0.0, 0.0),
            &mut t
        ));
        assert_eq!(t, 1.0, "*t untouched");
        // asymmetric crossing: (0,0,−1)-(0,0,3) -> t = 1/4
        assert!(plane_intersection_line(
            &p,
            &vec3::vec3_set(0.0, 0.0, -1.0),
            &vec3::vec3_set(0.0, 0.0, 3.0),
            &mut t
        ));
        assert_eq!(t, 0.25);
    }

    #[test]
    fn r56_inside_q5() {
        // S-009-R56/Q-5: any vertex >= prec away on either side
        let p = Plane {
            dir: vec3::vec3_set(0.0, 0.0, 1.0),
            dist: 0.0,
        };
        assert!(plane_tri_inside(
            &p,
            &vec3::vec3_set(0.0, 0.0, 1.0),
            &vec3::vec3_set(0.0, 0.0, 0.1),
            &vec3::vec3_set(0.0, 0.0, -0.1),
            0.5
        ));
        // all within prec -> false
        assert!(!plane_tri_inside(
            &p,
            &vec3::vec3_set(0.0, 0.0, 0.1),
            &vec3::vec3_set(0.0, 0.0, -0.1),
            &vec3::vec3_set(0.0, 0.0, 0.2),
            0.5
        ));
        // prec = 0: every finite vertex sets a flag (d >= 0 or d <= 0)
        assert!(plane_line_inside(
            &p,
            &vec3::vec3_set(0.0, 0.0, 0.0),
            &vec3::vec3_set(0.0, 0.0, 0.0),
            0.0
        ));
        // negative side counts too
        assert!(plane_tri_inside(
            &p,
            &vec3::vec3_set(0.0, 0.0, -1.0),
            &vec3::vec3_set(0.0, 0.0, 0.1),
            &vec3::vec3_set(0.0, 0.0, 0.1),
            0.5
        ));
    }

    #[test]
    fn r57_r58_close_coplanar() {
        // S-009-R57: signed vec3_close on the dirs
        let a = Plane {
            dir: vec3::vec3_set(1.0, 1.0, 1.0),
            dist: 2.0,
        };
        // b.dir is 1.0 LESS in x: a − b = (−1,0,0) < eps on all lanes
        let b = Plane {
            dir: vec3::vec3_set(2.0, 1.0, 1.0),
            dist: 2.0,
        };
        assert!(plane_close(&a, &b, 1e-4), "signed close: a below b passes");
        // reversed: a − b = (1,0,0) — NOT < eps
        assert!(!plane_close(&b, &a, 1e-4));
        // distance must be close too
        let c = Plane {
            dir: vec3::vec3_set(2.0, 1.0, 1.0),
            dist: 3.0,
        };
        assert!(!plane_close(&a, &c, 1e-4));

        // S-009-R58: parallel -> compare dist
        let p1 = Plane {
            dir: vec3::vec3_set(0.0, 1.0, 0.0),
            dist: 5.0,
        };
        let p2 = Plane {
            dir: vec3::vec3_set(0.0, 1.0, 0.0),
            dist: 5.0,
        };
        let p3 = Plane {
            dir: vec3::vec3_set(0.0, 1.0, 0.0),
            dist: 6.0,
        };
        assert!(plane_coplanar(&p1, &p2, 1e-4));
        assert!(!plane_coplanar(&p1, &p3, 1e-4));
        // anti-parallel: compare −a.dist to b.dist
        let p4 = Plane {
            dir: vec3::vec3_set(0.0, -1.0, 0.0),
            dist: -5.0,
        };
        assert!(plane_coplanar(&p1, &p4, 1e-4));
        let p5 = Plane {
            dir: vec3::vec3_set(0.0, -1.0, 0.0),
            dist: 5.0,
        };
        assert!(!plane_coplanar(&p1, &p5, 1e-4));
        // orthogonal: false
        let p6 = Plane {
            dir: vec3::vec3_set(1.0, 0.0, 0.0),
            dist: 5.0,
        };
        assert!(!plane_coplanar(&p1, &p6, 1e-4));
    }
}
