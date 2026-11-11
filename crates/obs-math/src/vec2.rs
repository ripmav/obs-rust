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

/// `vec2_dist(a, b)` — 2D Euclidean distance (R11: `len(a − b)`, no zero
/// guard).
#[must_use]
pub fn vec2_dist(a: &Vec2, b: &Vec2) -> f32 {
    let dx = a.x - b.x;
    let dy = a.y - b.y;
    (dx * dx + dy * dy).sqrt()
}

// ── S-009-R10: component-wise, IEEE semantics ───────────────────────────

/// S-009-R10.
#[must_use]
pub fn vec2_add(a: &Vec2, b: &Vec2) -> Vec2 {
    Vec2 {
        x: a.x + b.x,
        y: a.y + b.y,
    }
}

/// S-009-R10.
#[must_use]
pub fn vec2_sub(a: &Vec2, b: &Vec2) -> Vec2 {
    Vec2 {
        x: a.x - b.x,
        y: a.y - b.y,
    }
}

/// S-009-R10.
#[must_use]
pub fn vec2_mul(a: &Vec2, b: &Vec2) -> Vec2 {
    Vec2 {
        x: a.x * b.x,
        y: a.y * b.y,
    }
}

/// S-009-R10: division by zero gives inf/NaN, no checks.
#[must_use]
pub fn vec2_div(a: &Vec2, b: &Vec2) -> Vec2 {
    Vec2 {
        x: a.x / b.x,
        y: a.y / b.y,
    }
}

/// S-009-R10.
#[must_use]
pub fn vec2_addf(a: &Vec2, f: f32) -> Vec2 {
    Vec2 {
        x: a.x + f,
        y: a.y + f,
    }
}

/// S-009-R10.
#[must_use]
pub fn vec2_subf(a: &Vec2, f: f32) -> Vec2 {
    Vec2 {
        x: a.x - f,
        y: a.y - f,
    }
}

/// S-009-R10.
#[must_use]
pub fn vec2_mulf(a: &Vec2, f: f32) -> Vec2 {
    Vec2 {
        x: a.x * f,
        y: a.y * f,
    }
}

/// S-009-R10.
#[must_use]
pub fn vec2_divf(a: &Vec2, f: f32) -> Vec2 {
    Vec2 {
        x: a.x / f,
        y: a.y / f,
    }
}

/// S-009-R10.
#[must_use]
pub fn vec2_neg(a: &Vec2) -> Vec2 {
    Vec2 { x: -a.x, y: -a.y }
}

// ── S-009-R11 ────────────────────────────────────────────────────────────

/// S-009-R11: `x1·x2 + y1·y2`.
#[must_use]
pub fn vec2_dot(a: &Vec2, b: &Vec2) -> f32 {
    a.x * b.x + a.y * b.y
}

/// S-009-R11: `sqrt(x·x + y·y)` — no zero guard, NaN propagates.
#[must_use]
pub fn vec2_len(v: &Vec2) -> f32 {
    (v.x * v.x + v.y * v.y).sqrt()
}

// ── S-009-R12 ────────────────────────────────────────────────────────────

/// S-009-R12: C conditional — a comparison involving NaN is false, so the
/// **second operand is returned** (R12 worked example).
#[must_use]
pub fn vec2_min(a: &Vec2, b: &Vec2) -> Vec2 {
    Vec2 {
        x: if a.x < b.x { a.x } else { b.x },
        y: if a.y < b.y { a.y } else { b.y },
    }
}

/// S-009-R12: C conditional (second operand on NaN/false).
#[must_use]
pub fn vec2_max(a: &Vec2, b: &Vec2) -> Vec2 {
    Vec2 {
        x: if a.x > b.x { a.x } else { b.x },
        y: if a.y > b.y { a.y } else { b.y },
    }
}

/// S-009-R12: scalar as the second operand (NaN second → returned).
#[must_use]
pub fn vec2_minf(a: &Vec2, f: f32) -> Vec2 {
    Vec2 {
        x: if a.x < f { a.x } else { f },
        y: if a.y < f { a.y } else { f },
    }
}

/// S-009-R12: scalar as the second operand (NaN second → returned).
#[must_use]
pub fn vec2_maxf(a: &Vec2, f: f32) -> Vec2 {
    Vec2 {
        x: if a.x > f { a.x } else { f },
        y: if a.y > f { a.y } else { f },
    }
}

/// S-009-R12: per-component `fabsf`.
#[must_use]
pub fn vec2_abs(a: &Vec2) -> Vec2 {
    Vec2 {
        x: a.x.abs(),
        y: a.y.abs(),
    }
}

/// S-009-R12: per-component `floorf`.
#[must_use]
pub fn vec2_floor(a: &Vec2) -> Vec2 {
    Vec2 {
        x: a.x.floor(),
        y: a.y.floor(),
    }
}

/// S-009-R12: per-component `ceilf`.
#[must_use]
pub fn vec2_ceil(a: &Vec2) -> Vec2 {
    Vec2 {
        x: a.x.ceil(),
        y: a.y.ceil(),
    }
}

// ── S-009-R14/R15 ────────────────────────────────────────────────────────

/// S-009-R14: `close_float` on both components — **absolute value**,
/// inclusive. Returns `bool` (the C `int` 0/1).
#[must_use]
pub fn vec2_close(a: &Vec2, b: &Vec2, eps: f32) -> bool {
    crate::close_float(a.x, b.x, eps) && crate::close_float(a.y, b.y, eps)
}

/// S-009-R15/Q-1: `d = v / len` **only if `len > 0`** — otherwise `d` is
/// **not written** (contrast `vec3_norm`, which writes zeros). An infinite
/// length gives `v · 0` (NaN components for infinite inputs).
pub fn vec2_norm(d: &mut Vec2, v: &Vec2) {
    let len = (v.x * v.x + v.y * v.y).sqrt();
    if len > 0.0 {
        let f = 1.0 / len;
        d.x = v.x * f;
        d.y = v.y * f;
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
    fn min_max_nan_returns_second_operand() {
        // S-009-R12 worked example: min((nan,1),(5,nan)) = (5,nan)
        let a = Vec2 {
            x: f32::NAN,
            y: 1.0,
        };
        let b = Vec2 {
            x: 5.0,
            y: f32::NAN,
        };
        let m = vec2_min(&a, &b);
        assert_eq!(m.x, 5.0);
        assert!(m.y.is_nan());
        // second operand NaN -> NaN (Rust's `f32::min` would keep `a.x`)
        assert!(vec2_minf(&vec2_set(1.0, 2.0), f32::NAN).x.is_nan());
        assert!(vec2_maxf(&vec2_set(1.0, 2.0), f32::NAN).y.is_nan());
        // first operand NaN -> the other operand (both NaN-free here)
        assert_eq!(vec2_max(&a, &b).x, 5.0);
        // second operand NaN -> NaN (R12 conditional: 1 < NaN is false)
        assert!(vec2_min(&a, &b).y.is_nan());
        assert!(vec2_max(&a, &b).y.is_nan());
    }

    #[test]
    fn r10_r11_component_wise() {
        // S-009-R10/R11
        let a = vec2_set(3.0, 4.0);
        let b = vec2_set(1.0, -2.0);
        assert_eq!(vec2_add(&a, &b), vec2_set(4.0, 2.0));
        assert_eq!(vec2_sub(&a, &b), vec2_set(2.0, 6.0));
        assert_eq!(vec2_mul(&a, &b), vec2_set(3.0, -8.0));
        assert_eq!(vec2_div(&a, &b), vec2_set(3.0, -2.0));
        assert_eq!(vec2_addf(&a, 1.0), vec2_set(4.0, 5.0));
        assert_eq!(vec2_subf(&a, 1.0), vec2_set(2.0, 3.0));
        assert_eq!(vec2_mulf(&a, 2.0), vec2_set(6.0, 8.0));
        assert_eq!(vec2_divf(&a, 2.0), vec2_set(1.5, 2.0));
        assert_eq!(vec2_neg(&a), vec2_set(-3.0, -4.0));
        // IEEE: no checks
        assert!(vec2_div(&a, &vec2_set(0.0, 1.0)).x.is_infinite());
        assert_eq!(vec2_dot(&a, &b), 3.0 * 1.0 + 4.0 * -2.0);
        assert!(within(vec2_len(&a), 5.0));
        // no zero guard: len(zero) = 0, len(nan) = nan
        assert_eq!(vec2_len(&vec2_zero()), 0.0);
        assert!(vec2_len(&vec2_set(f32::NAN, 0.0)).is_nan());
        assert_eq!(vec2_abs(&vec2_set(-1.5, 2.5)), vec2_set(1.5, 2.5));
        assert_eq!(vec2_floor(&vec2_set(1.7, -1.2)), vec2_set(1.0, -2.0));
        assert_eq!(vec2_ceil(&vec2_set(1.2, -1.7)), vec2_set(2.0, -1.0));
    }

    #[test]
    fn r14_close_is_absolute() {
        // S-009-R14 (contrast the signed vec3_close)
        assert!(vec2_close(
            &vec2_set(1.0, 1.0),
            &vec2_set(1.0 + 0.5e-4, 1.0),
            1e-4
        ));
        assert!(!vec2_close(
            &vec2_set(1.0, 1.0),
            &vec2_set(1.0 + 2.0e-4, 1.0),
            1e-4
        ));
        // inclusive: distance exactly eps
        assert!(vec2_close(&vec2_set(0.0, 0.0), &vec2_set(1e-4, 0.0), 1e-4));
        // NaN fails the comparison (R02)
        assert!(!vec2_close(
            &vec2_set(f32::NAN, 0.0),
            &vec2_set(f32::NAN, 0.0),
            1e-4
        ));
    }

    #[test]
    fn r15_norm_q1() {
        // S-009-R15/Q-1: zero/NaN length -> d NOT written
        let mut d = vec2_set(9.0, 9.0);
        vec2_norm(&mut d, &vec2_zero());
        assert_eq!(d, vec2_set(9.0, 9.0), "Q-1: destination untouched");
        vec2_norm(&mut d, &vec2_set(f32::NAN, 0.0));
        assert_eq!(d, vec2_set(9.0, 9.0), "Q-1: NaN length, untouched");
        // normal case
        let mut u = vec2_zero();
        vec2_norm(&mut u, &vec2_set(3.0, 4.0));
        assert!(within(u.x, 0.6));
        assert!(within(u.y, 0.8));
        // infinite length -> v * 0 (NaN for infinite inputs)
        let mut inf_d = vec2_zero();
        vec2_norm(&mut inf_d, &vec2_set(f32::INFINITY, 0.0));
        assert!(inf_d.x.is_nan());
    }

    #[test]
    fn norm() {
        let mut d = vec2_zero();
        vec2_norm(&mut d, &vec2_set(3.0, 4.0));
        assert!(within(d.x, 0.6));
        assert!(within(d.y, 0.8));
        // S-009-R15/Q-1: zero length -> destination NOT written
        vec2_norm(&mut d, &vec2_zero());
        assert!(within(d.x, 0.6));
        assert!(within(d.y, 0.8));
    }
}
