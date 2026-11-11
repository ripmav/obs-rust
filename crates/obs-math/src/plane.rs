//! `plane` (S-009 §3, R50–R58; `libobs/graphics/plane.{h,c}`).
//!
//! Layout: `dir` (a `vec3`, 16 bytes, `w` lane live) at offset 0 and
//! `dist` (`f32`) at offset 16 — 32 bytes total. A plane is the set of
//! points `p` with `dot4(p, dir) = dist` (R50).

use crate::vec3::{self, Vec3};

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
}
