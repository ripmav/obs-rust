//! `bounds` (S-009 §5.6; `libobs/graphics/bounds.{h,c}`).
//!
//! An AABB: two `vec3`s (`min`, `max`) — 32 bytes, 16-byte aligned.
//! No function validates or normalises a box (a negative scale factor
//! gives `min > max`, Q-13).

use crate::matrix3::Matrix3;
use crate::matrix4::Matrix4;
use crate::plane::Plane;
use crate::vec3::{self, Vec3};
use crate::{EPSILON, M_INFINITE, TINY_EPSILON};

/// S-009 §3: the C `struct bounds`.
#[repr(C, align(16))]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Bounds {
    pub min: Vec3,
    pub max: Vec3,
}

/// S-009-R76: the three-way classification values (math-defs.h).
pub const BOUNDS_OUTSIDE: u32 = 1;
/// S-009-R76.
pub const BOUNDS_INSIDE: u32 = 2;
/// S-009-R76.
pub const BOUNDS_PARTIAL: u32 = 3;

// ── S-009-R70: construction / merging ──────────────────────────────────

/// S-009-R70: `bounds_zero` — both vec3s zeroed.
#[must_use]
pub fn bounds_zero() -> Bounds {
    Bounds {
        min: vec3::vec3_zero(),
        max: vec3::vec3_zero(),
    }
}

/// S-009-R70: `bounds_copy`.
pub fn bounds_copy(d: &mut Bounds, b: &Bounds) {
    *d = *b;
}

/// S-009-R70: `min = b.min + v`, `max = b.max + v` (`w = 0`). `d` may
/// alias `b`.
pub fn bounds_move(d: &mut Bounds, b: &Bounds, v: &Vec3) {
    let (nmin, nmax) = (vec3::vec3_add(&b.min, v), vec3::vec3_add(&b.max, v));
    d.min = nmin;
    d.max = nmax;
}

/// S-009-R70: `min = b.min * v`, `max = b.max * v` (all four lanes) —
/// **min/max are not re-sorted** (Q-13: a negative factor gives
/// `min > max`). `d` may alias `b`.
pub fn bounds_scale(d: &mut Bounds, b: &Bounds, v: &Vec3) {
    let (nmin, nmax) = (vec3::vec3_mul(&b.min, v), vec3::vec3_mul(&b.max, v));
    d.min = nmin;
    d.max = nmax;
}

/// S-009-R70: `min = vec3_min(a.min, b.min)`, `max = vec3_max(a.max,
/// b.max)`. `d` may alias either input.
pub fn bounds_merge(d: &mut Bounds, a: &Bounds, b: &Bounds) {
    let (nmin, nmax) = (
        vec3::vec3_min(&a.min, &b.min),
        vec3::vec3_max(&a.max, &b.max),
    );
    d.min = nmin;
    d.max = nmax;
}

/// S-009-R70: merge a single point. `d` may alias `b`.
pub fn bounds_merge_point(d: &mut Bounds, b: &Bounds, v: &Vec3) {
    let (nmin, nmax) = (vec3::vec3_min(&b.min, v), vec3::vec3_max(&b.max, v));
    d.min = nmin;
    d.max = nmax;
}

// ── S-009-R71/R72: points / center ─────────────────────────────────────

/// S-009-R71/Q-7: corner index; the guard is `i > 8` (not `i >= 8`), and
/// `i = 8` returns `(max.x, max.y, min.z)` — the same y/z selection as
/// `i = 6`. For `0 <= i <= 7`: bit 2 selects `max.x`, bit 1 `max.y`,
/// bit 0 `max.z`. **Only `x, y, z` are written** — `d.w` keeps its
/// previous value.
pub fn bounds_get_point(d: &mut Vec3, b: &Bounds, i: u32) {
    if i > 8 {
        return;
    }
    let i6 = if i == 8 { 6 } else { i % 4 };
    d.x = if i >= 4 { b.max.x } else { b.min.x };
    d.y = if i6 >= 2 { b.max.y } else { b.min.y };
    d.z = if i6 & 1 == 1 { b.max.z } else { b.min.z };
}

/// S-009-R72: `((b.max − b.min) * 0.5) + b.min` (`w = 0`).
pub fn bounds_get_center(d: &mut Vec3, b: &Bounds) {
    let s = vec3::vec3_sub(&b.max, &b.min);
    let m = vec3::vec3_mulf(&s, 0.5);
    *d = vec3::vec3_add(&m, &b.min);
}

// ── S-009-R73: transforms ──────────────────────────────────────────────

fn aabb_of_corners(d: &mut Bounds, corners: &[Vec3; 8]) {
    let mut min = corners[0];
    let mut max = corners[0];
    for p in corners.iter().skip(1) {
        // `p.c < min.c` then `else p.c > max.c` — NaN is never selected
        if p.x < min.x {
            min.x = p.x;
        } else if p.x > max.x {
            max.x = p.x;
        }
        if p.y < min.y {
            min.y = p.y;
        } else if p.y > max.y {
            max.y = p.y;
        }
        if p.z < min.z {
            min.z = p.z;
        } else if p.z > max.z {
            max.z = p.z;
        }
    }
    d.min = min;
    d.max = max;
}

/// S-009-R73: the eight corners are transformed with the **point**
/// transform (`w = 1`, S-008 R07) and `d` becomes the componentwise min
/// / max (temporary — `d` may alias `b`). The result is the AABB of the
/// OBB, generally larger.
pub fn bounds_transform(d: &mut Bounds, b: &Bounds, m: &Matrix4) {
    let mut corners = [vec3::vec3_zero(); 8];
    for (i, c) in corners.iter_mut().enumerate() {
        bounds_get_point(c, b, i as u32);
        *c = vec3::vec3_transform(c, m);
    }
    aabb_of_corners(d, &corners);
}

/// S-009-R73: the `matrix3` form (corners via `vec3_transform3x4`, R22).
pub fn bounds_transform3x4(d: &mut Bounds, b: &Bounds, m: &Matrix3) {
    let mut corners = [vec3::vec3_zero(); 8];
    for (i, c) in corners.iter_mut().enumerate() {
        bounds_get_point(c, b, i as u32);
        *c = vec3::vec3_transform3x4(c, m);
    }
    aabb_of_corners(d, &corners);
}

// ── S-009-R74/R75: intersections ───────────────────────────────────────

/// S-009-R74: the slab test of the spec pseudocode; `dir` need not be
/// unit (only `x,y,z` are used). `*t` is written only on success; an
/// origin inside the box gives the exit parameter (`tmax`), and a zero
/// direction with the origin inside gives `*t = M_INFINITE` (script 16).
pub fn bounds_intersection_ray(b: &Bounds, orig: &Vec3, dir: &Vec3, t: &mut f32) -> bool {
    let mut tmax = M_INFINITE;
    let mut tmin = -M_INFINITE;
    let mut c = vec3::vec3_zero();
    bounds_get_center(&mut c, b);
    let h = vec3::vec3_sub(&b.max, &c);
    let off = vec3::vec3_sub(&c, orig);
    for (e, f, hi) in [
        (off.x, dir.x, h.x),
        (off.y, dir.y, h.y),
        (off.z, dir.z, h.z),
    ] {
        if f.abs() > 0.0 {
            let fi = 1.0 / f;
            let t1 = (e + hi) * fi;
            let t2 = (e - hi) * fi;
            if t1 > t2 {
                if t2 > tmin {
                    tmin = t2;
                }
                if t1 < tmax {
                    tmax = t1;
                }
            } else {
                if t1 > tmin {
                    tmin = t1;
                }
                if t2 < tmax {
                    tmax = t2;
                }
            }
            if tmin > tmax {
                return false;
            }
            if tmax < 0.0 {
                return false;
            }
        } else if (-e - hi) > 0.0 || (-e + hi) < 0.0 {
            return false;
        }
    }
    *t = if tmin > 0.0 { tmin } else { tmax };
    true
}

/// S-009-R75: `len <= TINY_EPSILON` → false; the segment is **not
/// clipped** to `[0, 1]` — `t` may exceed 1 (script 16).
pub fn bounds_intersection_line(b: &Bounds, p1: &Vec3, p2: &Vec3, t: &mut f32) -> bool {
    let dir0 = vec3::vec3_sub(p2, p1);
    let len = vec3::vec3_len(&dir0);
    if len <= TINY_EPSILON {
        return false;
    }
    let dir = vec3::vec3_mulf(&dir0, 1.0 / len);
    if !bounds_intersection_ray(b, p1, &dir, t) {
        return false;
    }
    *t /= len;
    true
}

/// S-009-R75: the boolean wrapper.
pub fn bounds_intersects_ray(b: &Bounds, orig: &Vec3, dir: &Vec3) -> bool {
    let mut t = 0.0;
    bounds_intersection_ray(b, orig, dir, &mut t)
}

/// S-009-R75: the boolean wrapper.
pub fn bounds_intersects_line(b: &Bounds, p1: &Vec3, p2: &Vec3) -> bool {
    let mut t = 0.0;
    bounds_intersection_line(b, p1, p2, &mut t)
}

// ── S-009-R76–R79: plane / containment / intersection ──────────────────

/// S-009-R76/D-2: the C function is declared `bool`, and after the
/// `int → bool` conversion **every outcome is `true`**. A conforming
/// implementation MUST return `true` for every input.
pub fn bounds_plane_test(_b: &Bounds, _p: &Plane) -> bool {
    true
}

fn corner_pair(dir_c: f32, min_c: f32, max_c: f32) -> (f32, f32) {
    // (vmin, vmax) along the axis
    if dir_c >= 0.0 {
        (min_c, max_c)
    } else {
        (max_c, min_c)
    }
}

/// The Rust-native three-way classification that S-009-R76 offers:
/// per axis, `vmin` is the box corner nearest to the plane (against the
/// normal) and `vmax` the farthest; `OUTSIDE` if `plane_dist(vmin) > 0`,
/// else `PARTIAL` if `plane_dist(vmax) >= 0`, else `INSIDE`.
#[must_use]
pub fn bounds_plane_classify(b: &Bounds, p: &Plane) -> u32 {
    let (min_x, max_x) = corner_pair(p.dir.x, b.min.x, b.max.x);
    let (min_y, max_y) = corner_pair(p.dir.y, b.min.y, b.max.y);
    let (min_z, max_z) = corner_pair(p.dir.z, b.min.z, b.max.z);
    let vmin = vec3::vec3_set(min_x, min_y, min_z);
    let vmax = vec3::vec3_set(max_x, max_y, max_z);
    if vec3::vec3_plane_dist(&vmin, p) > 0.0 {
        BOUNDS_OUTSIDE
    } else if vec3::vec3_plane_dist(&vmax, p) >= 0.0 {
        BOUNDS_PARTIAL
    } else {
        BOUNDS_INSIDE
    }
}

/// S-009-R77: `vmin.c = (p.dir.c < 0) ? b.max.c : b.min.c`;
/// `dot4(vmin, p.dir) <= p.dist`, with `vmin.w = 0` (the reference
/// leaves it uninitialised, D-4).
pub fn bounds_under_plane(b: &Bounds, p: &Plane) -> bool {
    let vmin = Vec3 {
        x: if p.dir.x < 0.0 { b.max.x } else { b.min.x },
        y: if p.dir.y < 0.0 { b.max.y } else { b.min.y },
        z: if p.dir.z < 0.0 { b.max.z } else { b.min.z },
        w: 0.0,
    };
    vec3::vec3_dot(&vmin, &p.dir) <= p.dist
}

/// S-009-R78: all six containment comparisons, inclusive, no epsilon.
pub fn bounds_inside(b: &Bounds, test: &Bounds) -> bool {
    test.min.x >= b.min.x
        && test.min.y >= b.min.y
        && test.min.z >= b.min.z
        && test.max.x <= b.max.x
        && test.max.y <= b.max.y
        && test.max.z <= b.max.z
}

/// S-009-R78: each component within `EPSILON` of the box.
pub fn bounds_vec3_inside(b: &Bounds, v: &Vec3) -> bool {
    v.x >= b.min.x - EPSILON
        && v.x <= b.max.x + EPSILON
        && v.y >= b.min.y - EPSILON
        && v.y <= b.max.y + EPSILON
        && v.z >= b.min.z - EPSILON
        && v.z <= b.max.z + EPSILON
}

/// S-009-R79: per axis, `(b.min.c − test.max.c) <= eps` and
/// `(test.min.c − b.max.c) <= eps`. A negative `eps` shrinks the boxes.
pub fn bounds_intersects(b: &Bounds, test: &Bounds, eps: f32) -> bool {
    (b.min.x - test.max.x) <= eps
        && (test.min.x - b.max.x) <= eps
        && (b.min.y - test.max.y) <= eps
        && (test.min.y - b.max.y) <= eps
        && (b.min.z - test.max.z) <= eps
        && (test.min.z - b.max.z) <= eps
}

// ── S-009-R80/R81 ──────────────────────────────────────────────────────

/// S-009-R80/D-5: `mi = matrix4_inv(m)` — on failure the reference
/// behaviour is undefined (uninitialised `mi`); a conforming
/// implementation SHOULD return `false`.
pub fn bounds_intersects_obb(b: &Bounds, test: &Bounds, m: &Matrix4, eps: f32) -> bool {
    let mut mi = crate::matrix4::matrix4_identity();
    if !crate::matrix4::matrix4_inv(&mut mi, m) {
        return false;
    }
    let mut b_tr = *b;
    bounds_transform(&mut b_tr, b, m);
    let mut test_tr = *test;
    bounds_transform(&mut test_tr, test, &mi);
    bounds_intersects(b, &test_tr, eps) && bounds_intersects(&b_tr, test, eps)
}

/// S-009-R80: the `matrix3` variant — the **transpose** is the inverse
/// for a rigid matrix3 (R39).
pub fn bounds_intersects_obb3x4(b: &Bounds, test: &Bounds, m: &Matrix3, eps: f32) -> bool {
    let mut mi = *m;
    crate::matrix3::matrix3_transpose(&mut mi, m);
    let mut b_tr = *b;
    bounds_transform3x4(&mut b_tr, b, m);
    let mut test_tr = *test;
    bounds_transform3x4(&mut test_tr, test, &mi);
    bounds_intersects(b, &test_tr, eps) && bounds_intersects(&b_tr, test, eps)
}

/// S-009-R81: `half = dot4(b.max − b.min, abs(p.dir)) * 0.5` (both
/// `w = 0`); `p.dist + vec3_plane_dist(center, p) − half`, evaluated
/// left to right. Since `plane_dist = dot4(c, dir) − p.dist`, the
/// result reduces to `dot4(c, dir) − half` (independent of `p.dist`) —
/// pinned as specified; the C conformance script confirms.
#[must_use]
pub fn bounds_min_dist(b: &Bounds, p: &Plane) -> f32 {
    let half = vec3::vec3_dot(&vec3::vec3_sub(&b.max, &b.min), &vec3::vec3_abs(&p.dir)) * 0.5;
    let mut c = vec3::vec3_zero();
    bounds_get_center(&mut c, b);
    p.dist + vec3::vec3_plane_dist(&c, p) - half
}

#[cfg(test)]
mod tests {
    use super::*;

    fn box3(x0: f32, y0: f32, z0: f32, x1: f32, y1: f32, z1: f32) -> Bounds {
        Bounds {
            min: vec3::vec3_set(x0, y0, z0),
            max: vec3::vec3_set(x1, y1, z1),
        }
    }

    #[test]
    fn layout() {
        use std::mem::{align_of, offset_of, size_of};
        assert_eq!(size_of::<Bounds>(), 32);
        assert_eq!(align_of::<Bounds>(), 16);
        assert_eq!(offset_of!(Bounds, min), 0);
        assert_eq!(offset_of!(Bounds, max), 16);
    }

    #[test]
    fn r70_construct_merge() {
        // S-009-R70
        let z = bounds_zero();
        assert_eq!(z.min, vec3::vec3_zero());
        assert_eq!(z.max, vec3::vec3_zero());

        let mut b = box3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0);
        let mut c = bounds_zero();
        bounds_copy(&mut c, &b);
        assert_eq!(c, b);

        let b0 = b;
        bounds_move(&mut b, &b0, &vec3::vec3_set(10.0, 0.0, 0.0));
        assert_eq!(b.min, vec3::vec3_set(11.0, 2.0, 3.0));
        assert_eq!(b.max, vec3::vec3_set(14.0, 5.0, 6.0));

        // Q-13: negative factor — min/max NOT re-sorted
        let s = box3(1.0, 2.0, 3.0, 4.0, 5.0, 6.0);
        let mut s2 = s;
        bounds_scale(&mut s2, &s, &vec3::vec3_set(-2.0, -1.0, 1.0));
        assert_eq!(s2.min, vec3::vec3_set(-2.0, -2.0, 3.0));
        assert_eq!(s2.max, vec3::vec3_set(-8.0, -5.0, 6.0));
        assert!(s2.min.x > s2.max.x, "Q-13: not re-sorted");

        let a = box3(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
        let e = box3(-1.0, 2.0, 0.0, 5.0, 3.0, 0.5);
        let mut m = a;
        bounds_merge(&mut m, &a, &e);
        assert_eq!(m.min, vec3::vec3_set(-1.0, 0.0, 0.0));
        assert_eq!(m.max, vec3::vec3_set(5.0, 3.0, 1.0));

        let mut mp = a;
        bounds_merge_point(&mut mp, &a, &vec3::vec3_set(9.0, -2.0, 0.5));
        assert_eq!(mp.min, vec3::vec3_set(0.0, -2.0, 0.0));
        assert_eq!(mp.max, vec3::vec3_set(9.0, 1.0, 1.0));
    }

    #[test]
    fn r71_get_point_q7() {
        // S-009-R71/Q-7
        let b = box3(0.0, 10.0, 20.0, 1.0, 11.0, 21.0);
        let expected = [
            (0.0, 10.0, 20.0), // 0
            (0.0, 10.0, 21.0), // 1: bit0 -> max.z
            (0.0, 11.0, 20.0), // 2: bit1 -> max.y
            (0.0, 11.0, 21.0), // 3
            (1.0, 10.0, 20.0), // 4: bit2 -> max.x
            (1.0, 10.0, 21.0), // 5
            (1.0, 11.0, 20.0), // 6
            (1.0, 11.0, 21.0), // 7
        ];
        for (i, (x, y, z)) in expected.iter().enumerate() {
            let mut p = vec3::vec3_set(-99.0, -99.0, -99.0);
            bounds_get_point(&mut p, &b, i as u32);
            assert_eq!((p.x, p.y, p.z), (*x, *y, *z), "corner {i}");
        }
        // i = 8: (max.x, max.y, min.z)
        let mut p8 = vec3::vec3_zero();
        bounds_get_point(&mut p8, &b, 8);
        assert_eq!((p8.x, p8.y, p8.z), (1.0, 11.0, 20.0));
        // i = 9: not written (sentinel)
        let mut p9 = vec3::vec3_set(7.0, 7.0, 7.0);
        p9.w = 42.0;
        bounds_get_point(&mut p9, &b, 9);
        assert_eq!(
            p9,
            vec3::Vec3 {
                x: 7.0,
                y: 7.0,
                z: 7.0,
                w: 42.0
            }
        );
        // d.w is never written (sentinel)
        let mut pw = vec3::vec3_set(0.0, 0.0, 0.0);
        pw.w = 42.0;
        bounds_get_point(&mut pw, &b, 0);
        assert_eq!(pw.w, 42.0, "w keeps its previous value");
    }

    #[test]
    fn r72_center() {
        let b = box3(0.0, 0.0, 0.0, 2.0, 4.0, 6.0);
        let mut c = vec3::vec3_zero();
        bounds_get_center(&mut c, &b);
        assert_eq!(c, vec3::vec3_set(1.0, 2.0, 3.0));
    }

    #[test]
    fn r73_transform() {
        // S-009-R73: identity keeps the box
        let b = box3(0.0, 0.0, 0.0, 2.0, 1.0, 1.0);
        let mut d = bounds_zero();
        bounds_transform(&mut d, &b, &crate::matrix4::matrix4_identity());
        assert_eq!(d, b);

        // 90° about z: (x,y,z) -> (−y, x, z); x extent [0,2] -> y [0,2];
        // y extent [0,1] -> x [−1,0]
        let mut m = crate::matrix4::matrix4_identity();
        let q = crate::quat::Quat {
            x: 0.0,
            y: 0.0,
            z: std::f32::consts::FRAC_PI_4.sin(),
            w: std::f32::consts::FRAC_PI_4.cos(),
        };
        crate::matrix4::matrix4_from_quat(&mut m, &q);
        bounds_transform(&mut d, &b, &m);
        assert!((d.min.x + 1.0).abs() < 1e-5);
        assert!((d.min.y - 0.0).abs() < 1e-5);
        assert!((d.max.x - 0.0).abs() < 1e-5);
        assert!((d.max.y - 2.0).abs() < 1e-5, "AABB larger (script 15)");
        assert_eq!(d.max.z, 1.0);
        assert_eq!(d.min.w, 0.0);

        // matrix3 form: same numeric rows, but vec3_transform3x4 applies
        // them as v·rows (row-major, R22) — the transpose of the matrix4
        // column-major result: x extent [0,2] -> y [−2,0]; y extent
        // [0,1] -> x [0,1]
        let m3 = crate::matrix3::Matrix3 {
            x: vec3::vec3_set(0.0, 1.0, 0.0),
            y: vec3::vec3_set(-1.0, 0.0, 0.0),
            z: vec3::vec3_set(0.0, 0.0, 1.0),
            t: vec3::vec3_zero(),
        };
        bounds_transform3x4(&mut d, &b, &m3);
        assert!((d.min.x - 0.0).abs() < 1e-5);
        assert!((d.min.y + 2.0).abs() < 1e-5);
        assert!((d.max.x - 1.0).abs() < 1e-5);
        assert!((d.max.y - 0.0).abs() < 1e-5);
        assert_eq!(d.max.z, 1.0);
    }

    #[test]
    fn r74_intersection_ray() {
        // S-009-R74
        let b = box3(0.0, 0.0, 0.0, 2.0, 2.0, 2.0);
        let mut t = -99.0;
        // hit from -x: enter at x = 0
        assert!(bounds_intersection_ray(
            &b,
            &vec3::vec3_set(-3.0, 1.0, 1.0),
            &vec3::vec3_set(1.0, 0.0, 0.0),
            &mut t
        ));
        assert_eq!(t, 3.0);
        // box behind the origin: miss
        assert!(!bounds_intersection_ray(
            &b,
            &vec3::vec3_set(3.0, 1.0, 1.0),
            &vec3::vec3_set(1.0, 0.0, 0.0),
            &mut t
        ));
        assert_eq!(t, 3.0, "*t only written on success");
        // origin inside: exit parameter
        assert!(bounds_intersection_ray(
            &b,
            &vec3::vec3_set(1.0, 1.0, 1.0),
            &vec3::vec3_set(1.0, 0.0, 0.0),
            &mut t
        ));
        assert_eq!(t, 1.0);
        // zero direction, origin inside: *t = M_INFINITE (script 16)
        assert!(bounds_intersection_ray(
            &b,
            &vec3::vec3_set(1.0, 1.0, 1.0),
            &vec3::vec3_zero(),
            &mut t
        ));
        assert_eq!(t, M_INFINITE);
        assert_eq!(t, 3.4e38);
        // zero direction, origin outside: miss
        assert!(!bounds_intersection_ray(
            &b,
            &vec3::vec3_set(5.0, 1.0, 1.0),
            &vec3::vec3_zero(),
            &mut t
        ));
        // non-unit direction: t is in dir-parameter units
        // (orig + t·dir hits x = 0: −6 + t·2 = 0 -> t = 3)
        assert!(bounds_intersection_ray(
            &b,
            &vec3::vec3_set(-6.0, 1.0, 1.0),
            &vec3::vec3_set(2.0, 0.0, 0.0),
            &mut t
        ));
        assert_eq!(t, 3.0, "t in dir units, not normalised");
    }

    #[test]
    fn r75_intersection_line() {
        // S-009-R75
        let b = box3(0.0, 0.0, 0.0, 2.0, 2.0, 2.0);
        let mut t = -99.0;
        // segment (-2,1,1) -> (4,1,1): hit at x = 0, t = 2/6
        assert!(bounds_intersection_line(
            &b,
            &vec3::vec3_set(-2.0, 1.0, 1.0),
            &vec3::vec3_set(4.0, 1.0, 1.0),
            &mut t
        ));
        assert!((t - 1.0 / 3.0).abs() < 1e-6);
        // not clipped to [0,1]: t may exceed 1 (script 16)
        assert!(bounds_intersection_line(
            &b,
            &vec3::vec3_set(-2.0, 1.0, 1.0),
            &vec3::vec3_set(-1.0, 1.0, 1.0),
            &mut t
        ));
        assert!((t - 2.0).abs() < 1e-6, "t = 2 (outside the segment)");
        // degenerate segment
        assert!(!bounds_intersection_line(
            &b,
            &vec3::vec3_set(1.0, 1.0, 1.0),
            &vec3::vec3_set(1.0, 1.0, 1.0),
            &mut t
        ));
        // boolean wrappers
        assert!(bounds_intersects_ray(
            &b,
            &vec3::vec3_set(-3.0, 1.0, 1.0),
            &vec3::vec3_set(1.0, 0.0, 0.0)
        ));
        assert!(!bounds_intersects_ray(
            &b,
            &vec3::vec3_set(3.0, 1.0, 1.0),
            &vec3::vec3_set(1.0, 0.0, 0.0)
        ));
        assert!(bounds_intersects_line(
            &b,
            &vec3::vec3_set(-3.0, 1.0, 1.0),
            &vec3::vec3_set(3.0, 1.0, 1.0)
        ));
        assert!(!bounds_intersects_line(
            &b,
            &vec3::vec3_set(3.0, 1.0, 1.0),
            &vec3::vec3_set(9.0, 1.0, 1.0)
        ));
    }

    #[test]
    fn r76_plane_test_d2() {
        // S-009-R76/D-2: the bool is ALWAYS true
        let b = box3(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
        let p_front = Plane {
            dir: vec3::vec3_set(0.0, 0.0, 1.0),
            dist: 5.0,
        };
        let p_back = Plane {
            dir: vec3::vec3_set(0.0, 0.0, 1.0),
            dist: -5.0,
        };
        let p_cut = Plane {
            dir: vec3::vec3_set(0.0, 0.0, 1.0),
            dist: 0.5,
        };
        assert!(bounds_plane_test(&b, &p_front), "D-2: always true");
        assert!(bounds_plane_test(&b, &p_back), "D-2: always true");
        assert!(bounds_plane_test(&b, &p_cut), "D-2: always true");

        // the offered Rust classification
        assert_eq!(bounds_plane_classify(&b, &p_front), BOUNDS_INSIDE);
        assert_eq!(bounds_plane_classify(&b, &p_back), BOUNDS_OUTSIDE);
        assert_eq!(bounds_plane_classify(&b, &p_cut), BOUNDS_PARTIAL);
    }

    #[test]
    fn r77_under_plane() {
        // S-009-R77
        let p = Plane {
            dir: vec3::vec3_set(0.0, 0.0, 1.0),
            dist: 0.0,
        };
        assert!(bounds_under_plane(
            &box3(0.0, 0.0, -2.0, 1.0, 1.0, -1.0),
            &p
        ));
        assert!(!bounds_under_plane(&box3(0.0, 0.0, 1.0, 1.0, 1.0, 2.0), &p));
        // touching the plane: dot == dist -> true
        assert!(bounds_under_plane(&box3(0.0, 0.0, -1.0, 1.0, 1.0, 0.0), &p));
        // dir < 0 selects max: the "under" side of (0,0,−1) is z >= 0
        let p2 = Plane {
            dir: vec3::vec3_set(0.0, 0.0, -1.0),
            dist: 0.0,
        };
        assert!(bounds_under_plane(&box3(0.0, 0.0, 1.0, 1.0, 1.0, 2.0), &p2));
        assert!(!bounds_under_plane(
            &box3(0.0, 0.0, -2.0, 1.0, 1.0, -1.0),
            &p2
        ));
    }

    #[test]
    fn r78_inside() {
        // S-009-R78: inclusive, no epsilon
        let b = box3(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
        assert!(bounds_inside(&b, &box3(0.2, 0.2, 0.2, 0.8, 0.8, 0.8)));
        assert!(bounds_inside(&b, &b), "inclusive");
        assert!(!bounds_inside(
            &b,
            &box3(0.0, 0.0, 0.0, 1.0 + 1e-6, 1.0, 1.0)
        ));
        assert!(!bounds_inside(&b, &box3(-1e-6, 0.0, 0.0, 1.0, 1.0, 1.0)));

        // vec3_inside: EPSILON margin
        assert!(bounds_vec3_inside(
            &b,
            &vec3::vec3_set(0.0 - 0.5e-4, 0.5, 0.5)
        ));
        assert!(bounds_vec3_inside(
            &b,
            &vec3::vec3_set(1.0 + 0.5e-4, 0.5, 0.5)
        ));
        assert!(!bounds_vec3_inside(
            &b,
            &vec3::vec3_set(1.0 + 2e-4, 0.5, 0.5)
        ));
        assert!(!bounds_vec3_inside(&b, &vec3::vec3_set(0.5, 0.5, -2.0)));
    }

    #[test]
    fn r79_intersects() {
        // S-009-R79
        let a = box3(0.0, 0.0, 0.0, 1.0, 1.0, 1.0);
        let overlap = box3(0.5, 0.5, 0.5, 2.0, 2.0, 2.0);
        let apart = box3(2.0, 0.0, 0.0, 3.0, 1.0, 1.0);
        assert!(bounds_intersects(&a, &overlap, 0.0));
        assert!(!bounds_intersects(&a, &apart, 0.0));
        // touching (gap == 0 <= eps)
        assert!(bounds_intersects(
            &a,
            &box3(1.0, 0.0, 0.0, 2.0, 1.0, 1.0),
            0.0
        ));
        // a small positive eps bridges a gap
        assert!(bounds_intersects(
            &a,
            &box3(1.0 + 0.5e-4, 0.0, 0.0, 2.0, 1.0, 1.0),
            1e-4
        ));
        // a negative eps shrinks the boxes
        assert!(!bounds_intersects(
            &a,
            &box3(1.0, 0.0, 0.0, 2.0, 1.0, 1.0),
            -1e-6
        ));
    }

    #[test]
    fn r80_intersects_obb() {
        // S-009-R80
        let b = box3(0.0, 0.0, 0.0, 2.0, 1.0, 1.0);
        let mut m = crate::matrix4::matrix4_identity();
        let q = crate::quat::Quat {
            x: 0.0,
            y: 0.0,
            z: std::f32::consts::FRAC_PI_4.sin(),
            w: std::f32::consts::FRAC_PI_4.cos(),
        };
        crate::matrix4::matrix4_from_quat(&mut m, &q);
        // the test box is the rotated b: they intersect
        let mut test_tr = b;
        bounds_transform(&mut test_tr, &b, &m);
        assert!(bounds_intersects_obb(&b, &test_tr, &m, 0.0));
        // moved far away: no
        let mut far = test_tr;
        let far0 = far;
        bounds_move(&mut far, &far0, &vec3::vec3_set(100.0, 0.0, 0.0));
        assert!(!bounds_intersects_obb(&b, &far, &m, 0.0));
        // singular m: SHOULD return false (D-5)
        let mut sing = crate::matrix4::matrix4_identity();
        sing.x = crate::vec4::vec4_zero();
        assert!(!bounds_intersects_obb(&b, &test_tr, &sing, 0.0));

        // matrix3 variant: 90° about z (rigid)
        let m3 = crate::matrix3::Matrix3 {
            x: vec3::vec3_set(0.0, 1.0, 0.0),
            y: vec3::vec3_set(-1.0, 0.0, 0.0),
            z: vec3::vec3_set(0.0, 0.0, 1.0),
            t: vec3::vec3_zero(),
        };
        let mut test3 = b;
        bounds_transform3x4(&mut test3, &b, &m3);
        assert!(bounds_intersects_obb3x4(&b, &test3, &m3, 0.0));
    }

    #[test]
    fn r81_min_dist() {
        // S-009-R81: `p.dist + plane_dist(c, p) − half` — note that with
        // plane_dist = dot4(c, dir) − p.dist this REDUCES to
        // `dot4(c, dir) − half`, independent of p.dist. The formula is
        // pinned as specified; the C conformance script (script 26?)
        // confirms against the reference.
        let b = box3(10.0, 0.0, 0.0, 12.0, 2.0, 2.0); // center (11,1,1)
        let p = Plane {
            dir: vec3::vec3_set(1.0, 0.0, 0.0),
            dist: 0.0,
        };
        // half = dot4((2,2,2),(1,0,0))·0.5 = 1; 0 + (11 − 0) − 1 = 10
        assert!((bounds_min_dist(&b, &p) - 10.0).abs() < 1e-5);
        // p.dist cancels: same result at dist = 5
        let p5 = Plane {
            dir: vec3::vec3_set(1.0, 0.0, 0.0),
            dist: 5.0,
        };
        assert!((bounds_min_dist(&b, &p5) - 10.0).abs() < 1e-5);
        // non-unit normal: half uses abs(dir)
        let p2 = Plane {
            dir: vec3::vec3_set(2.0, 0.0, 0.0),
            dist: 0.0,
        };
        // half = dot4((2,2,2),(2,0,0))·0.5 = 2; 22 − 2 = 20
        assert!((bounds_min_dist(&b, &p2) - 20.0).abs() < 1e-5);
        // y and z components of abs(dir) count too
        let p3 = Plane {
            dir: vec3::vec3_set(0.0, 1.0, 1.0),
            dist: 0.0,
        };
        // half = (2+2)·0.5 = 2; dot4((11,1,1),(0,1,1)) = 2; 2 − 2 = 0
        assert!((bounds_min_dist(&b, &p3) - 0.0).abs() < 1e-5);
    }
}
