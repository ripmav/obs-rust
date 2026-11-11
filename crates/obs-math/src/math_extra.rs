//! `math-extra` (S-009 §5.7; `libobs/graphics/math-extra.{c,h}`).
//!
//! Small pure helpers. `rand_float` (R96): the spec does not pin the
//! reference's sequence or seeding (C library `rand()`); §7 explicitly
//! allows any thread-safe generator — a thread-local xorshift64 seeded
//! from a deterministic atomic counter ^ thread identity (no wall
//! clock, so Miri isolation stays green).

use std::cell::Cell;
use std::sync::atomic::{AtomicU64, Ordering};

use crate::vec2::{self, Vec2};
use crate::vec3::{self, Vec3};
use crate::{EPSILON, LARGE_EPSILON, close_float, vec3_close};

thread_local! {
    static RAND_STATE: Cell<u64> = const { Cell::new(0) };
}

static SEED_COUNTER: AtomicU64 = AtomicU64::new(0x9E37_79B9_7F4A_7C15);

/// Deterministic, syscall-free seed (Miri isolation forbids the wall
/// clock): golden-ratio counter ^ thread identity.
fn rand_seed() -> u64 {
    use std::hash::{Hash, Hasher};
    let mut h = std::collections::hash_map::DefaultHasher::new();
    std::thread::current().id().hash(&mut h);
    let c = SEED_COUNTER.fetch_add(1, Ordering::Relaxed);
    (c ^ h.finish()).max(1)
}

/// S-009-R96: `rand_float(positive_only)` — uniformly distributed `f32`
/// in `[0, 1]` when `positive_only`, else in `[-1, 1]`.
#[must_use]
pub fn rand_float(positive_only: bool) -> f32 {
    let x = RAND_STATE.with(|s| {
        let mut x = s.get();
        if x == 0 {
            x = rand_seed();
            s.set(x);
        }
        // xorshift64
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        s.set(x);
        x
    });
    // 53-bit mantissa → [0, 1)
    let u = (x >> 11) as f64 * (1.0 / 9_007_199_254_740_992.0);
    if positive_only {
        u as f32
    } else {
        (u * 2.0 - 1.0) as f32
    }
}

// ── S-009-R90–R95 ────────────────────────────────────────────────

/// S-009-R90: `v = (elevation, azimuth, radius)`; `h = v.z·cos(v.x)`,
/// `cart.x = h·sin(v.y)`, `cart.z = h·cos(v.y)`, `cart.y = v.z·sin(v.x)`;
/// everything is computed before the store (aliasing safe). `d.w` is
/// uninitialised in the reference (D-3); MUST be written as 0.
pub fn polar_to_cart(d: &mut Vec3, v: &Vec3) {
    let h = v.z * v.x.cos();
    *d = Vec3 {
        x: h * v.y.sin(),
        y: v.z * v.x.sin(),
        z: h * v.y.cos(),
        w: 0.0,
    };
}

/// S-009-R91: `r = vec3_len(v)` (four-lane dot, 0 if `dot4 <= 0`);
/// `close_float(r, 0, EPSILON)` → `(0,0,0,0)`; else
/// `d.x = asin(v.y/r)`, `d.y = atan2(v.x, v.z)`, `d.z = r`, `d.w = 0`
/// (D-3).
pub fn cart_to_polar(d: &mut Vec3, v: &Vec3) {
    let r = vec3::vec3_len(v);
    if close_float(r, 0.0, EPSILON) {
        *d = vec3::vec3_zero();
    } else {
        *d = Vec3 {
            x: (v.y / r).asin(),
            y: v.x.atan2(v.z),
            z: r,
            w: 0.0,
        };
    }
}

/// S-009-R92: `d.x = atan2(n.x, n.z)`, `d.y = asin(n.y)` (NaN when
/// `|n.y| > 1`).
pub fn norm_to_polar(d: &mut Vec2, n: &Vec3) {
    *d = vec2::vec2_set(n.x.atan2(n.z), n.y.asin());
}

/// S-009-R92: `s = sin(p.x)`; `d.x = s·cos(p.y)`, `d.y = s·sin(p.y)`,
/// `d.z = cos(p.x)`; **`d.w` is not written**.
pub fn polar_to_norm(d: &mut Vec3, p: &Vec2) {
    let s = p.x.sin();
    d.x = s * p.y.cos();
    d.y = s * p.y.sin();
    d.z = p.x.cos();
}

/// S-009-R93: the spec pseudocode; `dist == 0` takes the second
/// branch; `t = 0` returns `val1`; a negative `t` moves away without
/// the clamp on that side.
#[must_use]
pub fn calc_torquef(val1: f32, val2: f32, torque: f32, min_adjust: f32, t: f32) -> f32 {
    if close_float(val1, val2, EPSILON) {
        return val2;
    }
    let mut dist = (val2 - val1) * torque;
    let mut out: f32;
    if dist > 0.0 {
        if dist < min_adjust {
            dist = min_adjust;
        }
        out = val1 + dist * t;
        if out > val2 {
            out = val2;
        }
    } else {
        if dist > -min_adjust {
            dist = -min_adjust;
        }
        out = val1 + dist * t;
        if out < val2 {
            out = val2;
        }
    }
    out
}

/// S-009-R94: the spec pseudocode with the **signed** `vec3_close` test
/// (Q-8: snaps to `v2` whenever `v1 <= v2` componentwise); `d` may
/// alias `v2` (safe: `v2` is fully read before any store).
///
/// Q-9: in the C reference the incremental stores corrupt the result
/// when `d` aliases `v1`. In Rust, aliasing `d` with `v1` is a caller
/// aliasing violation (the two references overlap), so this port reads
/// `v1` completely before the store — the corruption is unreachable
/// from a well-behaved caller (documented deviation).
pub fn calc_torque(d: &mut Vec3, v1: &Vec3, v2: &Vec3, torque: f32, min_adjust: f32, t: f32) {
    if vec3_close(v1, v2, EPSILON) {
        *d = *v2;
        return;
    }
    let line = vec3::vec3_sub(v2, v1);
    let od = vec3::vec3_len(&line);
    let dir = vec3::vec3_mulf(&line, 1.0 / od);
    let mut td = od * torque;
    if td < min_adjust {
        td = min_adjust;
    }
    let adj = td * t;
    if adj <= od - LARGE_EPSILON {
        let step = vec3::vec3_mulf(&dir, adj);
        *d = vec3::vec3_add(&step, v1);
    } else {
        *d = *v2;
    }
}

/// S-009-R95: `(mid − start) / (end − start)` in f32, no zero check
/// (`0/0 = NaN`, `x/0 = inf`).
#[must_use]
pub fn get_percentage(start: f32, end: f32, mid: f32) -> f32 {
    (mid - start) / (end - start)
}

/// S-009-R95: the `int` form — the subtractions happen in `i32`.
#[must_use]
pub fn get_percentagei(start: i32, end: i32, mid: i32) -> f32 {
    (mid - start) as f32 / (end - start) as f32
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rand_float_range() {
        // S-009-R96: range contract only (sequence unspecified)
        let mut seen = std::collections::HashSet::new();
        for _ in 0..1000 {
            let p = rand_float(true);
            assert!((0.0..=1.0).contains(&p), "positive: {p}");
            let s = rand_float(false);
            assert!((-1.0..=1.0).contains(&s), "signed: {s}");
            seen.insert(p.to_bits());
        }
        assert!(seen.len() > 1, "values are not all identical");
    }

    #[test]
    fn r90_r91_polar_cart() {
        // S-009-R90: v = (elevation, azimuth, radius)
        // elevation = pi/2, radius = 1 -> (0, 1, 0)
        let mut d = vec3::vec3_zero();
        polar_to_cart(
            &mut d,
            &vec3::vec3_set(std::f32::consts::FRAC_PI_2, 0.0, 1.0),
        );
        assert!((d.x).abs() < 1e-6);
        assert!((d.y - 1.0).abs() < 1e-6);
        assert!(d.z.abs() < 1e-6);
        assert_eq!(d.w, 0.0, "D-3: MUST write 0");
        // elevation = 0, azimuth = pi/2 -> (1, 0, 0)
        polar_to_cart(
            &mut d,
            &vec3::vec3_set(0.0, std::f32::consts::FRAC_PI_2, 1.0),
        );
        assert!((d.x - 1.0).abs() < 1e-6);
        assert!(d.y.abs() < 1e-6);
        assert!(d.z.abs() < 1e-6);

        // S-009-R91: round trip
        let mut p = vec3::vec3_zero();
        cart_to_polar(&mut p, &vec3::vec3_set(1.0, 0.0, 0.0));
        assert!(p.x.abs() < 1e-6);
        assert!((p.y - std::f32::consts::FRAC_PI_2).abs() < 1e-6);
        assert!((p.z - 1.0).abs() < 1e-6);
        assert_eq!(p.w, 0.0, "D-3: MUST write 0");
        // zero vector: close_float(r, 0, EPSILON) -> (0,0,0,0)
        cart_to_polar(&mut p, &vec3::vec3_zero());
        assert_eq!(p, vec3::vec3_zero());
    }

    #[test]
    fn r92_norm_polar() {
        // S-009-R92
        let mut d = vec2::vec2_zero();
        norm_to_polar(&mut d, &vec3::vec3_set(1.0, 0.0, 0.0));
        assert!((d.x - std::f32::consts::FRAC_PI_2).abs() < 1e-6);
        assert!(d.y.abs() < 1e-6);
        // asin(n.y) NaN when |n.y| > 1
        norm_to_polar(&mut d, &vec3::vec3_set(0.0, 2.0, 0.0));
        assert!(d.y.is_nan());
        // polar_to_norm: d.w NOT written (sentinel)
        let mut v = vec3::vec3_zero();
        v.w = 42.0;
        polar_to_norm(&mut v, &vec2::vec2_set(0.0, 0.0));
        assert!((v.x).abs() < 1e-6);
        assert!(v.y.abs() < 1e-6);
        assert!((v.z - 1.0).abs() < 1e-6);
        assert_eq!(v.w, 42.0, "d.w not written");
        polar_to_norm(&mut v, &vec2::vec2_set(std::f32::consts::FRAC_PI_2, 0.0));
        // s = sin(pi/2) = 1: (x, y, z) = (cos p.y, sin p.y, cos p.x)
        assert!((v.x - 1.0).abs() < 1e-6);
        assert!(v.y.abs() < 1e-6);
        assert!(v.z.abs() < 1e-6);
        assert_eq!(v.w, 42.0);
    }

    #[test]
    fn r93_calc_torquef() {
        // S-009-R93
        // close: returns val2
        assert_eq!(calc_torquef(1.0, 1.0 + 0.5e-4, 1.0, 1.0, 0.5), 1.0 + 0.5e-4);
        // t = 0 returns val1
        assert_eq!(calc_torquef(0.0, 10.0, 2.0, 1.0, 0.0), 0.0);
        // full travel reaches val2, clamped there
        assert!((calc_torquef(0.0, 10.0, 2.0, 1.0, 1.0) - 10.0).abs() < 1e-6);
        // half travel
        assert!(
            (calc_torquef(0.0, 10.0, 2.0, 1.0, 0.5) - 10.0).abs() < 1e-6,
            "dist = 20, out = 10"
        );
        assert!((calc_torquef(0.0, 10.0, 1.0, 1.0, 0.5) - 5.0).abs() < 1e-6);
        // min_adjust raises a small positive dist
        assert!((calc_torquef(0.0, 1.0, 0.1, 1.0, 1.0) - 1.0).abs() < 1e-6);
        // negative direction: clamped at val2 from below
        assert!((calc_torquef(10.0, 0.0, 2.0, 1.0, 1.0) - 0.0).abs() < 1e-6);
        assert!((calc_torquef(10.0, 0.0, 1.0, 1.0, 0.5) - 5.0).abs() < 1e-6);
        // negative t moves away WITHOUT the clamp on that side
        assert!((calc_torquef(0.0, 10.0, 2.0, 1.0, -0.5) + 10.0).abs() < 1e-6);
    }

    #[test]
    fn r94_calc_torque() {
        // S-009-R94
        let mut d = vec3::vec3_zero();
        // Q-8: v1 below v2 in all lanes -> snaps to v2 (all four lanes)
        calc_torque(
            &mut d,
            &vec3::vec3_set(0.0, 0.0, 0.0),
            &Vec3 {
                x: 10.0,
                y: 0.0,
                z: 0.0,
                w: 7.0,
            },
            1.0,
            1.0,
            1.0,
        );
        assert_eq!(
            d,
            Vec3 {
                x: 10.0,
                y: 0.0,
                z: 0.0,
                w: 7.0,
            }
        );
        // v1 exceeds v2 by >= EPSILON in one component -> moves
        calc_torque(
            &mut d,
            &vec3::vec3_set(12.0, 0.0, 0.0),
            &vec3::vec3_set(10.0, 0.0, 0.0),
            1.0,
            1.0,
            1.0,
        );
        // adj = od·torque·t = 2 >= od - LARGE_EPSILON -> d = v2
        assert_eq!(d, vec3::vec3_set(10.0, 0.0, 0.0));
        calc_torque(
            &mut d,
            &vec3::vec3_set(12.0, 0.0, 0.0),
            &vec3::vec3_set(10.0, 0.0, 0.0),
            1.0,
            1.0,
            0.5,
        );
        // adj = 1 < 2 - 1e-2 -> d = dir·adj + v1 = (-1,0,0) + (12,0,0)
        assert_eq!(d, vec3::vec3_set(11.0, 0.0, 0.0));
        // 2D case: movement along a diagonal (od = sqrt(32) = 4*sqrt(2))
        calc_torque(
            &mut d,
            &vec3::vec3_set(14.0, 4.0, 0.0),
            &vec3::vec3_set(10.0, 0.0, 0.0),
            1.0,
            1.0,
            1.0,
        );
        // od = 4*sqrt(2), dir = (-1,-1,0)/sqrt(2), adj = od >= od - eps -> d = v2
        assert_eq!(d, vec3::vec3_set(10.0, 0.0, 0.0));
        calc_torque(
            &mut d,
            &vec3::vec3_set(14.0, 4.0, 0.0),
            &vec3::vec3_set(10.0, 0.0, 0.0),
            1.0,
            1.0,
            0.5,
        );
        // adj = od/2 = 2*sqrt(2) -> d = dir*adj + v1 = (-2,-2,0) + (14,4,0)
        assert!((d.x - 12.0).abs() < 1e-6);
        assert!((d.y - 2.0).abs() < 1e-6);
        // min_adjust floors td
        calc_torque(
            &mut d,
            &vec3::vec3_set(14.0, 0.0, 0.0),
            &vec3::vec3_set(10.0, 0.0, 0.0),
            0.1,
            1.0,
            1.0,
        );
        // td = 0.4 -> 1.0; adj = 1.0 < 4 - 1e-2 -> d = (-1,0,0) + (14,0,0)
        assert!((d.x - 13.0).abs() < 1e-6);
    }

    #[test]
    fn r95_percentage() {
        // S-009-R95: no zero check
        assert_eq!(get_percentage(0.0, 10.0, 5.0), 0.5);
        assert_eq!(get_percentage(0.0, 4.0, 5.0), 1.25);
        assert!(get_percentage(5.0, 5.0, 5.0).is_nan(), "0/0 = NaN");
        assert!(get_percentage(0.0, 0.0, 1.0).is_infinite(), "x/0 = inf");
        assert_eq!(get_percentagei(0, 10, 5), 0.5);
        assert_eq!(get_percentagei(0, 10, 7), 0.7);
    }
}
