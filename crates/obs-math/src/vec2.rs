//! `vec2` (S-008-R37; `libobs/graphics/vec2.{h,c}`).
//!
//! Layout: two `f32` (`x`, `y`) — 8 bytes, alignment 4.

/// S-008 §3: the C `struct vec2`.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Vec2 {
    pub x: f32,
    pub y: f32,
}

/// `vec2_zero()`.
#[must_use]
pub const fn vec2_zero() -> Vec2 {
    Vec2 { x: 0.0, y: 0.0 }
}

/// `vec2_set(x, y)`.
#[must_use]
pub const fn vec2_set(x: f32, y: f32) -> Vec2 {
    Vec2 { x, y }
}

/// `vec2_copy(d, v)`.
pub fn vec2_copy(d: &mut Vec2, v: &Vec2) {
    *d = *v;
}

/// `vec2_dist(a, b)` — 2D Euclidean distance.
#[must_use]
pub fn vec2_dist(a: &Vec2, b: &Vec2) -> f32 {
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    (dx * dx + dy * dy).sqrt()
}

/// `vec2_min(a, b)` — component-wise.
#[must_use]
pub fn vec2_min(a: &Vec2, b: &Vec2) -> Vec2 {
    Vec2 {
        x: a.x.min(b.x),
        y: a.y.min(b.y),
    }
}

/// `vec2_max(a, b)` — component-wise.
#[must_use]
pub fn vec2_max(a: &Vec2, b: &Vec2) -> Vec2 {
    Vec2 {
        x: a.x.max(b.x),
        y: a.y.max(b.y),
    }
}

/// `vec2_minf(a, f)`.
#[must_use]
pub fn vec2_minf(a: &Vec2, f: f32) -> Vec2 {
    Vec2 {
        x: a.x.min(f),
        y: a.y.min(f),
    }
}

/// `vec2_maxf(a, f)`.
#[must_use]
pub fn vec2_maxf(a: &Vec2, f: f32) -> Vec2 {
    Vec2 {
        x: a.x.max(f),
        y: a.y.max(f),
    }
}

/// `vec2_norm(d, v)` (in `vec2.c`) — unit vector; division by zero yields
/// the zero vector (R37 convention).
pub fn vec2_norm(d: &mut Vec2, v: &Vec2) {
    let len = (v.x * v.x + v.y * v.y).sqrt();
    if len > 0.0 {
        d.x = v.x / len;
        d.y = v.y / len;
    } else {
        d.x = 0.0;
        d.y = 0.0;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// S-008 §2: relative **or** absolute tolerance 1e-6.
    fn within(a: f32, b: f32) -> bool {
        let d = (a - b).abs();
        d <= 1e-6 || d <= 1e-6 * a.abs().max(b.abs())
    }

    #[test]
    fn layout() {
        use std::mem::{align_of, size_of};
        assert_eq!(size_of::<Vec2>(), 8);
        assert_eq!(align_of::<Vec2>(), 4);
        assert_eq!(std::mem::offset_of!(Vec2, x), 0);
        assert_eq!(std::mem::offset_of!(Vec2, y), 4);
    }

    #[test]
    fn set_zero_copy() {
        let v = vec2_set(3.0, -4.0);
        assert_eq!(v, Vec2 { x: 3.0, y: -4.0 });
        assert_eq!(vec2_zero(), Vec2 { x: 0.0, y: 0.0 });
        let mut c = vec2_zero();
        vec2_copy(&mut c, &v);
        assert_eq!(c, v);
    }

    #[test]
    fn dist_min_max() {
        let a = vec2_set(3.0, 4.0);
        let b = vec2_set(0.0, 0.0);
        assert!(within(vec2_dist(&a, &b), 5.0));
        let c = vec2_set(-1.0, 9.0);
        assert_eq!(vec2_min(&a, &c), Vec2 { x: -1.0, y: 4.0 });
        assert_eq!(vec2_max(&a, &c), Vec2 { x: 3.0, y: 9.0 });
        assert_eq!(vec2_minf(&a, 3.5), Vec2 { x: 3.0, y: 3.5 });
        assert_eq!(vec2_maxf(&a, 3.5), Vec2 { x: 3.5, y: 4.0 });
    }

    #[test]
    fn norm() {
        let mut d = vec2_zero();
        vec2_norm(&mut d, &vec2_set(3.0, 4.0));
        assert!(within(d.x, 0.6));
        assert!(within(d.y, 0.8));
        // Division by zero -> zero vector (R37).
        vec2_norm(&mut d, &vec2_zero());
        assert_eq!(d, vec2_zero());
    }
}
