//! `vec4` (S-008 R04, R05, R07, R08, R39; `libobs/graphics/vec4.h`).
//!
//! Layout: four `f32` lanes `[x, y, z, w]`, 16 bytes, 16-byte aligned.
//! No special-casing: every operation works on all four lanes (R04).

use crate::gs_float_to_u8;

/// S-008 §3: the C `struct vec4`.
#[repr(C, align(16))]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct Vec4 {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    pub w: f32,
}

/// `vec4_zero()`.
#[must_use]
pub const fn vec4_zero() -> Vec4 {
    Vec4 {
        x: 0.0,
        y: 0.0,
        z: 0.0,
        w: 0.0,
    }
}

/// S-008-R01: `vec4_set` stores all four lanes.
#[must_use]
pub const fn vec4_set(x: f32, y: f32, z: f32, w: f32) -> Vec4 {
    Vec4 { x, y, z, w }
}

/// `vec4_copy(d, v)`.
pub fn vec4_copy(d: &mut Vec4, v: &Vec4) {
    *d = *v;
}

/// S-008-R04: all four lanes, no special-casing.
#[must_use]
pub fn vec4_add(a: &Vec4, b: &Vec4) -> Vec4 {
    Vec4 {
        x: a.x + b.x,
        y: a.y + b.y,
        z: a.z + b.z,
        w: a.w + b.w,
    }
}

/// S-008-R04: all four lanes, no special-casing.
#[must_use]
pub fn vec4_sub(a: &Vec4, b: &Vec4) -> Vec4 {
    Vec4 {
        x: a.x - b.x,
        y: a.y - b.y,
        z: a.z - b.z,
        w: a.w - b.w,
    }
}

/// S-008-R04: all four lanes, no special-casing.
#[must_use]
pub fn vec4_div(a: &Vec4, b: &Vec4) -> Vec4 {
    Vec4 {
        x: a.x / b.x,
        y: a.y / b.y,
        z: a.z / b.z,
        w: a.w / b.w,
    }
}

/// S-008-R04: all four lanes, no special-casing.
#[must_use]
pub fn vec4_addf(a: &Vec4, f: f32) -> Vec4 {
    Vec4 {
        x: a.x + f,
        y: a.y + f,
        z: a.z + f,
        w: a.w + f,
    }
}

/// S-008-R04: all four lanes, no special-casing.
#[must_use]
pub fn vec4_subf(a: &Vec4, f: f32) -> Vec4 {
    Vec4 {
        x: a.x - f,
        y: a.y - f,
        z: a.z - f,
        w: a.w - f,
    }
}

/// S-008-R04: all four lanes, no special-casing.
#[must_use]
pub fn vec4_divf(a: &Vec4, f: f32) -> Vec4 {
    Vec4 {
        x: a.x / f,
        y: a.y / f,
        z: a.z / f,
        w: a.w / f,
    }
}

/// S-008-R04/R39: sums the four products.
#[must_use]
pub fn vec4_dot(a: &Vec4, b: &Vec4) -> f32 {
    a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w
}

/// S-008-R04: negates all four lanes.
#[must_use]
pub fn vec4_neg(v: &Vec4) -> Vec4 {
    Vec4 {
        x: -v.x,
        y: -v.y,
        z: -v.z,
        w: -v.w,
    }
}

/// S-008-R04: `len(v) = dot(v,v) > 0 ? sqrt(dot(v,v)) : 0`.
#[must_use]
pub fn vec4_len(v: &Vec4) -> f32 {
    let d = vec4_dot(v, v);
    if d > 0.0 { d.sqrt() } else { 0.0 }
}

/// S-008-R04: distance over all four lanes.
#[must_use]
pub fn vec4_dist(a: &Vec4, b: &Vec4) -> f32 {
    let d = vec4_sub(a, b);
    vec4_len(&d)
}

/// S-008-R04: same rule as vec3 — all lanes, or the zero vector.
#[must_use]
pub fn vec4_norm(v: &Vec4) -> Vec4 {
    let d = vec4_dot(v, v);
    if d > 0.0 {
        let f = 1.0 / d.sqrt();
        Vec4 {
            x: v.x * f,
            y: v.y * f,
            z: v.z * f,
            w: v.w * f,
        }
    } else {
        vec4_zero()
    }
}

/// S-008-R06/R39: **signed** differences on all four lanes (Q-2).
#[must_use]
pub fn vec4_close(a: &Vec4, b: &Vec4, eps: f32) -> bool {
    let t = vec4_sub(a, b);
    t.x < eps && t.y < eps && t.z < eps && t.w < eps
}

/// S-008-R39: absolute value on all four lanes.
#[must_use]
pub fn vec4_abs(v: &Vec4) -> Vec4 {
    Vec4 {
        x: v.x.abs(),
        y: v.y.abs(),
        z: v.z.abs(),
        w: v.w.abs(),
    }
}

/// S-008-R39: floor on all four lanes.
#[must_use]
pub fn vec4_floor(v: &Vec4) -> Vec4 {
    Vec4 {
        x: v.x.floor(),
        y: v.y.floor(),
        z: v.z.floor(),
        w: v.w.floor(),
    }
}

/// S-008-R39: ceil on all four lanes.
#[must_use]
pub fn vec4_ceil(v: &Vec4) -> Vec4 {
    Vec4 {
        x: v.x.ceil(),
        y: v.y.ceil(),
        z: v.z.ceil(),
        w: v.w.ceil(),
    }
}

/// S-008-R05: `vec4_from_vec3` copies `x,y,z` and sets **`w = 1`**.
#[must_use]
pub fn vec4_from_vec3(v: &crate::vec3::Vec3) -> Vec4 {
    Vec4 {
        x: v.x,
        y: v.y,
        z: v.z,
        w: 1.0,
    }
}

/// S-008-R07: `d = v · M` (rows `M.x, M.y, M.z, M.t`), all four lanes.
#[must_use]
pub fn vec4_transform(v: &Vec4, m: &crate::matrix4::Matrix4) -> Vec4 {
    Vec4 {
        x: v.x * m.x.x + v.y * m.y.x + v.z * m.z.x + v.w * m.t.x,
        y: v.x * m.x.y + v.y * m.y.y + v.z * m.z.y + v.w * m.t.y,
        z: v.x * m.x.z + v.y * m.y.z + v.z * m.z.z + v.w * m.t.z,
        w: v.x * m.x.w + v.y * m.y.w + v.z * m.z.w + v.w * m.t.w,
    }
}

/// S-008-R08: packs `R,G,B,A` in memory order `[R,G,B,A]` as a
/// native-endian `u32` (LE: `0xAABBGGRR`). No clamping (Q-4).
/// (`gs_float_to_u8` itself is S-009-R121, defined in `srgb.rs` — in
/// C it is the single `static inline` in `srgb.h`.)
#[must_use]
pub fn vec4_to_rgba(v: &Vec4) -> u32 {
    let r = gs_float_to_u8(v.x) as u32;
    let g = gs_float_to_u8(v.y) as u32;
    let b = gs_float_to_u8(v.z) as u32;
    let a = gs_float_to_u8(v.w) as u32;
    u32::from_ne_bytes([r as u8, g as u8, b as u8, a as u8])
}

/// S-008-R08: like `vec4_to_rgba` with R and B swapped.
#[must_use]
pub fn vec4_to_bgra(v: &Vec4) -> u32 {
    let r = gs_float_to_u8(v.x) as u32;
    let g = gs_float_to_u8(v.y) as u32;
    let b = gs_float_to_u8(v.z) as u32;
    let a = gs_float_to_u8(v.w) as u32;
    u32::from_ne_bytes([b as u8, g as u8, r as u8, a as u8])
}

/// S-008-R39: `byte / 255.0` per channel, memory order `[R,G,B,A]`.
pub fn vec4_from_rgba(d: &mut Vec4, c: u32) {
    let [r, g, b, a] = c.to_ne_bytes();
    d.x = r as f32 / 255.0;
    d.y = g as f32 / 255.0;
    d.z = b as f32 / 255.0;
    d.w = a as f32 / 255.0;
}

/// S-008-R39: like `vec4_from_rgba` with R and B swapped.
pub fn vec4_from_bgra(d: &mut Vec4, c: u32) {
    let [b, g, r, a] = c.to_ne_bytes();
    d.x = r as f32 / 255.0;
    d.y = g as f32 / 255.0;
    d.z = b as f32 / 255.0;
    d.w = a as f32 / 255.0;
}

/// S-008-R08: sRGB-to-linear curve per channel:
/// `u <= 0.04045 ? u/12.92 : ((u+0.055)/1.055)^2.4`.
fn srgb_to_linear(u: f32) -> f32 {
    if u <= 0.04045 {
        u / 12.92
    } else {
        ((u + 0.055) / 1.055).powf(2.4)
    }
}

/// S-008-R08: `vec4_from_rgba` then linearize `x,y,z`.
pub fn vec4_from_rgba_srgb(d: &mut Vec4, c: u32) {
    vec4_from_rgba(d, c);
    d.x = srgb_to_linear(d.x);
    d.y = srgb_to_linear(d.y);
    d.z = srgb_to_linear(d.z);
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
        assert_eq!(size_of::<Vec4>(), 16);
        assert_eq!(align_of::<Vec4>(), 16);
        assert_eq!(offset_of!(Vec4, x), 0);
        assert_eq!(offset_of!(Vec4, y), 4);
        assert_eq!(offset_of!(Vec4, z), 8);
        assert_eq!(offset_of!(Vec4, w), 12);
    }

    #[test]
    fn ops_touch_all_lanes() {
        // S-008-R04: no special-casing
        let a = Vec4 {
            x: 1.0,
            y: 2.0,
            z: 3.0,
            w: 4.0,
        };
        let b = Vec4 {
            x: 5.0,
            y: 6.0,
            z: 7.0,
            w: 8.0,
        };
        assert_eq!(
            vec4_add(&a, &b),
            Vec4 {
                x: 6.0,
                y: 8.0,
                z: 10.0,
                w: 12.0
            }
        );
        assert_eq!(
            vec4_sub(&b, &a),
            Vec4 {
                x: 4.0,
                y: 4.0,
                z: 4.0,
                w: 4.0
            }
        );
        assert_eq!(
            vec4_div(&b, &a),
            Vec4 {
                x: 5.0,
                y: 3.0,
                z: 7.0 / 3.0,
                w: 2.0
            }
        );
        assert_eq!(
            vec4_addf(&a, 1.0),
            Vec4 {
                x: 2.0,
                y: 3.0,
                z: 4.0,
                w: 5.0
            }
        );
        assert_eq!(
            vec4_neg(&a),
            Vec4 {
                x: -1.0,
                y: -2.0,
                z: -3.0,
                w: -4.0
            }
        );
        assert!(within(vec4_dot(&a, &b), 70.0)); // 5+12+21+32
        // dist includes w
        assert!(within(
            vec4_dist(&a, &b),
            (16.0f32 + 16.0 + 16.0 + 16.0).sqrt()
        ));
        let n = vec4_norm(&a);
        let l = 30.0f32.sqrt();
        assert!(within(n.x, 1.0 / l));
        assert!(within(n.w, 4.0 / l));
        assert_eq!(vec4_norm(&vec4_zero()), vec4_zero());
        assert_eq!(
            vec4_abs(&Vec4 {
                x: -1.0,
                y: 0.0,
                z: 0.0,
                w: -2.0
            }),
            Vec4 {
                x: 1.0,
                y: 0.0,
                z: 0.0,
                w: 2.0
            }
        );
        assert_eq!(
            vec4_floor(&Vec4 {
                x: 1.5,
                y: -1.5,
                z: 0.0,
                w: 0.0
            }),
            Vec4 {
                x: 1.0,
                y: -2.0,
                z: 0.0,
                w: 0.0
            }
        );
        assert_eq!(
            vec4_ceil(&Vec4 {
                x: 1.5,
                y: -1.5,
                z: 0.0,
                w: 0.0
            }),
            Vec4 {
                x: 2.0,
                y: -1.0,
                z: 0.0,
                w: 0.0
            }
        );
    }

    #[test]
    fn close_is_signed_all_lanes() {
        // S-008-R06/Q-2
        assert!(vec4_close(
            &vec4_zero(),
            &vec4_set(1.0, 1.0, 1.0, 1.0),
            1e-4
        ));
        assert!(!vec4_close(
            &vec4_set(1.0, 1.0, 1.0, 1.0),
            &vec4_zero(),
            1e-4
        ));
        // w lane participates
        assert!(!vec4_close(
            &vec4_set(0.0, 0.0, 0.0, 1.0),
            &vec4_zero(),
            1e-4
        ));
    }

    #[test]
    fn from_vec3_sets_w_one() {
        // S-008-R05
        use crate::vec3::vec3_set;
        let v = vec4_from_vec3(&vec3_set(1.0, 2.0, 3.0));
        assert_eq!(
            v,
            Vec4 {
                x: 1.0,
                y: 2.0,
                z: 3.0,
                w: 1.0
            }
        );
    }

    #[test]
    fn transform_is_row_times_matrix() {
        // S-008-R07: d = v · M
        use crate::matrix4::matrix4_identity;
        let mut m = matrix4_identity();
        m.t = vec4_set(10.0, 20.0, 30.0, 1.0);
        let r = vec4_transform(&vec4_set(1.0, 2.0, 3.0, 1.0), &m);
        assert!(within(r.x, 11.0));
        assert!(within(r.y, 22.0));
        assert!(within(r.z, 33.0));
        assert!(within(r.w, 1.0));
    }

    #[test]
    fn rgba_packing() {
        // S-008-R08 worked value: (1, 0.5, 0, 1) -> 0xff0080ff
        let v = vec4_set(1.0, 0.5, 0.0, 1.0);
        assert_eq!(vec4_to_rgba(&v), 0xff00_80ff);
        // bgra stores [B,G,R,A] in memory order: (1, 0.5, 0, 1) -> 0xffff8000
        assert_eq!(vec4_to_bgra(&v), 0xffff_8000);
        // round trip
        let mut d = vec4_zero();
        vec4_from_rgba(&mut d, 0xff00_80ff);
        assert!(within(d.x, 1.0));
        assert!(within(d.y, 128.0 / 255.0));
        assert!(within(d.z, 0.0));
        assert!(within(d.w, 1.0));
        vec4_from_bgra(&mut d, 0xffff_8000);
        assert!(within(d.x, 1.0));
        assert!(within(d.z, 0.0));
    }

    #[test]
    fn srgb_linearization() {
        // S-008-R08: boundary and the upper branch
        let mut d = vec4_zero();
        // 0 -> 0, 1 -> 1 (R is the LSB byte in our packing)
        vec4_from_rgba_srgb(&mut d, 0x0000_0000);
        assert!(within(d.x, 0.0));
        vec4_from_rgba_srgb(&mut d, 0x0000_00ff);
        assert!(within(d.x, 1.0));
        // R = 10: 10/255 = 0.0392 <= 0.04045 -> low branch
        vec4_from_rgba_srgb(&mut d, 0x0000_000a);
        let expected_low = 10.0f32 / 255.0 / 12.92;
        assert!(within(d.x, expected_low));
        // upper branch at 0.5: ((0.5+0.055)/1.055)^2.4
        let u: f32 = 128.0 / 255.0;
        let expected = ((u + 0.055f32) / 1.055f32).powf(2.4f32);
        vec4_from_rgba_srgb(&mut d, 0x0000_0080);
        assert!(within(d.x, expected));
        // w is not linearized (A = 0 here)
        assert!(within(d.w, 0.0));
    }
}
