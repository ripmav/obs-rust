//! `vec3` (S-008 R01–R03, R05–R07, R38; `libobs/graphics/vec3.{h,c}`).
//!
//! Layout: four `f32` lanes `[x, y, z, w]`, 16 bytes, 16-byte aligned
//! (on x86 the C type is a union with `__m128`). **Q-1**: `w` is a live
//! lane — most operations write `w = 0`, but `dot`, `mul` and `mulf`
//! include it.

/// S-008 §3: the C `struct vec3`.
#[repr(C, align(16))]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Vec3 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub w: f32,
}

/// `vec3_zero()`.
#[must_use]
pub const fn vec3_zero() -> Vec3 {
    Vec3 {
        x: 0.0,
        y: 0.0,
        z: 0.0,
        w: 0.0,
    }
}

/// S-008-R01: `vec3_set(d, x, y, z)` stores `w = 0`.
#[must_use]
pub const fn vec3_set(x: f32, y: f32, z: f32) -> Vec3 {
    Vec3 { x, y, z, w: 0.0 }
}

/// `vec3_copy(d, v)`.
pub fn vec3_copy(d: &mut Vec3, v: &Vec3) {
    *d = *v;
}

/// S-008-R02: lane-wise on `x,y,z`, **`w = 0`**.
#[must_use]
pub fn vec3_add(a: &Vec3, b: &Vec3) -> Vec3 {
    Vec3 {
        x: a.x + b.x,
        y: a.y + b.y,
        z: a.z + b.z,
        w: 0.0,
    }
}

/// S-008-R02: lane-wise on `x,y,z`, **`w = 0`**.
#[must_use]
pub fn vec3_sub(a: &Vec3, b: &Vec3) -> Vec3 {
    Vec3 {
        x: a.x - b.x,
        y: a.y - b.y,
        z: a.z - b.z,
        w: 0.0,
    }
}

/// S-008-R02: lane-wise on `x,y,z`, **`w = 0`**.
#[must_use]
pub fn vec3_div(a: &Vec3, b: &Vec3) -> Vec3 {
    Vec3 {
        x: a.x / b.x,
        y: a.y / b.y,
        z: a.z / b.z,
        w: 0.0,
    }
}

/// S-008-R02: `x,y,z` plus `f`, **`w = 0`**.
#[must_use]
pub fn vec3_addf(a: &Vec3, f: f32) -> Vec3 {
    Vec3 {
        x: a.x + f,
        y: a.y + f,
        z: a.z + f,
        w: 0.0,
    }
}

/// S-008-R02: `x,y,z` minus `f`, **`w = 0`**.
#[must_use]
pub fn vec3_subf(a: &Vec3, f: f32) -> Vec3 {
    Vec3 {
        x: a.x - f,
        y: a.y - f,
        z: a.z - f,
        w: 0.0,
    }
}

/// S-008-R02: `x,y,z` divided by `f`, **`w = 0`**.
#[must_use]
pub fn vec3_divf(a: &Vec3, f: f32) -> Vec3 {
    Vec3 {
        x: a.x / f,
        y: a.y / f,
        z: a.z / f,
        w: 0.0,
    }
}

/// S-008-R02: component-wise min on `x,y,z`, **`w = 0`**.
#[must_use]
pub fn vec3_min(a: &Vec3, b: &Vec3) -> Vec3 {
    Vec3 {
        x: a.x.min(b.x),
        y: a.y.min(b.y),
        z: a.z.min(b.z),
        w: 0.0,
    }
}

/// S-008-R02: component-wise max on `x,y,z`, **`w = 0`**.
#[must_use]
pub fn vec3_max(a: &Vec3, b: &Vec3) -> Vec3 {
    Vec3 {
        x: a.x.max(b.x),
        y: a.y.max(b.y),
        z: a.z.max(b.z),
        w: 0.0,
    }
}

/// S-008-R02: min with a scalar, **`w = 0`**.
#[must_use]
pub fn vec3_minf(a: &Vec3, f: f32) -> Vec3 {
    Vec3 {
        x: a.x.min(f),
        y: a.y.min(f),
        z: a.z.min(f),
        w: 0.0,
    }
}

/// S-008-R02: max with a scalar, **`w = 0`**.
#[must_use]
pub fn vec3_maxf(a: &Vec3, f: f32) -> Vec3 {
    Vec3 {
        x: a.x.max(f),
        y: a.y.max(f),
        z: a.z.max(f),
        w: 0.0,
    }
}

/// S-008-R02/Q-1: `vec3_mul` computes **all four lanes** — the result `w`
/// is `a.w * b.w`.
#[must_use]
pub fn vec3_mul(a: &Vec3, b: &Vec3) -> Vec3 {
    Vec3 {
        x: a.x * b.x,
        y: a.y * b.y,
        z: a.z * b.z,
        w: a.w * b.w,
    }
}

/// S-008-R02/Q-1: `vec3_mulf` computes **all four lanes** — `w = v.w * f`.
#[must_use]
pub fn vec3_mulf(v: &Vec3, f: f32) -> Vec3 {
    Vec3 {
        x: v.x * f,
        y: v.y * f,
        z: v.z * f,
        w: v.w * f,
    }
}

/// S-008-R38 scale helper: same all-four-lane semantics as `vec3_mulf`.
#[must_use]
pub fn vec3_scale(v: &Vec3, f: f32) -> Vec3 {
    vec3_mulf(v, f)
}

/// S-008-R02: negates `x,y,z`, **`w = 0`**.
#[must_use]
pub fn vec3_neg(v: &Vec3) -> Vec3 {
    Vec3 {
        x: -v.x,
        y: -v.y,
        z: -v.z,
        w: 0.0,
    }
}

/// S-008-R02: absolute value on `x,y,z`, **`w = 0`**.
#[must_use]
pub fn vec3_abs(v: &Vec3) -> Vec3 {
    Vec3 {
        x: v.x.abs(),
        y: v.y.abs(),
        z: v.z.abs(),
        w: 0.0,
    }
}

/// S-008-R02: floor on `x,y,z`, **`w = 0`**.
#[must_use]
pub fn vec3_floor(v: &Vec3) -> Vec3 {
    Vec3 {
        x: v.x.floor(),
        y: v.y.floor(),
        z: v.z.floor(),
        w: 0.0,
    }
}

/// S-008-R02: ceil on `x,y,z`, **`w = 0`**.
#[must_use]
pub fn vec3_ceil(v: &Vec3) -> Vec3 {
    Vec3 {
        x: v.x.ceil(),
        y: v.y.ceil(),
        z: v.z.ceil(),
        w: 0.0,
    }
}

/// S-008-R02: usual cross product of `x,y,z`, **`w = 0`**.
#[must_use]
pub fn vec3_cross(a: &Vec3, b: &Vec3) -> Vec3 {
    Vec3 {
        x: a.y * b.z - a.z * b.y,
        y: a.z * b.x - a.x * b.z,
        z: a.x * b.y - a.y * b.x,
        w: 0.0,
    }
}

/// S-008-R03/Q-1: `vec3_dot` sums **all four lanes**.
#[must_use]
pub fn vec3_dot(a: &Vec3, b: &Vec3) -> f32 {
    a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w
}

/// S-008-R03: `vec3_len(v) = dot(v,v) > 0 ? sqrt(dot(v,v)) : 0` (the `w`
/// lane participates through the dot — Q-1).
#[must_use]
pub fn vec3_len(v: &Vec3) -> f32 {
    let d = vec3_dot(v, v);
    if d > 0.0 { d.sqrt() } else { 0.0 }
}

/// S-008-R03: `vec3_dist(a,b) = len(a − b)`; `vec3_sub` zeroes `w`, so the
/// distance is over `x,y,z` only.
#[must_use]
pub fn vec3_dist(a: &Vec3, b: &Vec3) -> f32 {
    let d = vec3_sub(a, b);
    vec3_len(&d)
}

/// S-008-R03: if `dot(v,v) > 0` multiply **all lanes** by `1/sqrt(dot)`;
/// else the zero vector.
#[must_use]
pub fn vec3_norm(v: &Vec3) -> Vec3 {
    let d = vec3_dot(v, v);
    if d > 0.0 {
        vec3_mulf(v, 1.0 / d.sqrt())
    } else {
        vec3_zero()
    }
}

/// S-008-R06/Q-2: **signed** differences, no absolute value —
/// `vec3_close(a, far)` is true when `a − far` is very negative.
#[must_use]
pub fn vec3_close(a: &Vec3, b: &Vec3, eps: f32) -> bool {
    let t = vec3_sub(a, b);
    t.x < eps && t.y < eps && t.z < eps
}

/// S-008-R05: `vec3_from_vec4(d, v)` copies the lanes, **`w = 0`**.
#[must_use]
pub fn vec3_from_vec4(v: &crate::vec4::Vec4) -> Vec3 {
    Vec3 {
        x: v.x,
        y: v.y,
        z: v.z,
        w: 0.0,
    }
}

/// S-008-R07: `v4 = (x, y, z, 1)`; `d = (v4 · M).xyz`, `w = 0`; **no
/// perspective divide**.
#[must_use]
pub fn vec3_transform(v: &Vec3, m: &crate::matrix4::Matrix4) -> Vec3 {
    let v4 = crate::vec4::vec4_set(v.x, v.y, v.z, 1.0);
    let r = crate::vec4::vec4_transform(&v4, m);
    Vec3 {
        x: r.x,
        y: r.y,
        z: r.z,
        w: 0.0,
    }
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
        assert_eq!(size_of::<Vec3>(), 16);
        assert_eq!(align_of::<Vec3>(), 16);
        assert_eq!(offset_of!(Vec3, x), 0);
        assert_eq!(offset_of!(Vec3, y), 4);
        assert_eq!(offset_of!(Vec3, z), 8);
        assert_eq!(offset_of!(Vec3, w), 12);
    }

    #[test]
    fn set_stores_w_zero() {
        // S-008-R01
        assert_eq!(
            vec3_set(1.0, 2.0, 3.0),
            Vec3 {
                x: 1.0,
                y: 2.0,
                z: 3.0,
                w: 0.0
            }
        );
    }

    #[test]
    fn w_lane_zeroed_by_most_ops() {
        // S-008-R02: w = 0 for add/sub/div/neg/abs/floor/ceil/cross/min/max
        let a = Vec3 {
            x: 1.0,
            y: 2.0,
            z: 3.0,
            w: 7.0,
        };
        let b = Vec3 {
            x: 4.0,
            y: 5.0,
            z: 6.0,
            w: 9.0,
        };
        for v in [
            vec3_add(&a, &b),
            vec3_sub(&a, &b),
            vec3_div(&a, &b),
            vec3_addf(&a, 1.0),
            vec3_subf(&a, 1.0),
            vec3_divf(&a, 2.0),
            vec3_min(&a, &b),
            vec3_max(&a, &b),
            vec3_minf(&a, 5.0),
            vec3_maxf(&a, 5.0),
            vec3_neg(&a),
            vec3_abs(&a),
            vec3_floor(&a),
            vec3_ceil(&a),
            vec3_cross(&a, &b),
        ] {
            assert_eq!(v.w, 0.0, "op must zero w");
        }
    }

    #[test]
    fn w_lane_live_in_mul_mulf_dot() {
        // S-008-R02/R03/Q-1
        let a = Vec3 {
            x: 1.0,
            y: 2.0,
            z: 3.0,
            w: 10.0,
        };
        let b = Vec3 {
            x: 4.0,
            y: 5.0,
            z: 6.0,
            w: 10.0,
        };
        assert_eq!(vec3_mul(&a, &b).w, 100.0);
        assert_eq!(vec3_mulf(&a, 2.0).w, 20.0);
        assert_eq!(vec3_scale(&a, 2.0).w, 20.0);
        // dot includes w: 1·4 + 2·5 + 3·6 + 10·10 = 132
        assert!(within(vec3_dot(&a, &b), 132.0));
        // len includes w through the dot: (1,0,0,10) -> sqrt(101)
        assert!(within(
            vec3_len(&Vec3 {
                x: 1.0,
                y: 0.0,
                z: 0.0,
                w: 10.0
            }),
            101.0f32.sqrt()
        ));
    }

    #[test]
    fn dist_and_norm() {
        // S-008-R03
        // dist uses vec3_sub (w zeroed) -> xyz only, even with live w lanes
        assert!(within(
            vec3_dist(
                &Vec3 {
                    x: 3.0,
                    y: 4.0,
                    z: 0.0,
                    w: 5.0
                },
                &Vec3 {
                    x: 0.0,
                    y: 0.0,
                    z: 0.0,
                    w: 9.0
                }
            ),
            5.0
        ));
        let n = vec3_norm(&Vec3 {
            x: 3.0,
            y: 0.0,
            z: 0.0,
            w: 3.0,
        });
        // norm scales all lanes: dot = 18 -> (3,0,0,3)/sqrt(18)
        let f = 3.0 / 18.0f32.sqrt();
        assert!(within(n.x, f));
        assert!(within(n.w, f));
        assert_eq!(vec3_norm(&vec3_zero()), vec3_zero());
        assert_eq!(vec3_len(&vec3_zero()), 0.0);
    }

    #[test]
    fn close_is_signed() {
        // S-008-R06/Q-2: a − b very negative -> true (the documented hazard)
        assert!(vec3_close(&vec3_zero(), &vec3_set(1.0, 1.0, 1.0), 1e-4));
        assert!(!vec3_close(&vec3_set(1.0, 1.0, 1.0), &vec3_zero(), 1e-4));
        assert!(vec3_close(
            &vec3_set(0.0, 0.0, 0.0),
            &vec3_set(0.0, 0.0, 0.0),
            1e-4
        ));
    }

    #[test]
    fn from_vec4_zeroes_w() {
        // S-008-R05
        use crate::vec4::vec4_set;
        let v = vec3_from_vec4(&vec4_set(1.0, 2.0, 3.0, 9.0));
        assert_eq!(v, vec3_set(1.0, 2.0, 3.0));
    }

    #[test]
    fn transform_translation_and_no_divide() {
        // S-008-R07
        use crate::matrix4::{matrix4_identity, matrix4_translate3v};
        use crate::vec4::vec4_set;
        let mut m = matrix4_identity();
        let m0 = m;
        matrix4_translate3v(&mut m, &m0, &vec3_set(10.0, 20.0, 30.0));
        let p = vec3_transform(&vec3_set(1.0, 2.0, 3.0), &m);
        assert!(within(p.x, 11.0));
        assert!(within(p.y, 22.0));
        assert!(within(p.z, 33.0));
        assert_eq!(p.w, 0.0);
        // No perspective divide: w of the transformed 4-vector stays the
        // raw dot even when the t-row carries w != 1.
        let mut m2 = matrix4_identity();
        m2.t = vec4_set(1.0, 0.0, 0.0, 3.0);
        let r = crate::vec4::vec4_transform(&vec4_set(1.0, 2.0, 3.0, 1.0), &m2);
        assert!(within(r.w, 3.0));
    }
}
