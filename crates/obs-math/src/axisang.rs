//! `axisang` basics (S-008-R44; `libobs/graphics/axisang.h`).
//!
//! Layout: four `f32` lanes `[x, y, z, w]`, 16 bytes, 16-byte aligned,
//! with `w` the **angle in radians**. The full quaternion/axisang
//! conversion set (R15, R17) arrives with P1-17.

/// S-008 §3: the C `struct axisang`.
#[repr(C, align(16))]
#[derive(Clone, Copy, Debug, PartialEq)]
pub struct AxisAng {
    pub x: f32,
    pub y: f32,
    pub z: f32,
    /// Angle in radians.
    pub w: f32,
}

/// S-008-R44: `axisang_zero()`.
#[must_use]
pub const fn axisang_zero() -> AxisAng {
    AxisAng {
        x: 0.0,
        y: 0.0,
        z: 0.0,
        w: 0.0,
    }
}

/// S-008-R44: `axisang_set(x, y, z, angle)`.
#[must_use]
pub const fn axisang_set(x: f32, y: f32, z: f32, angle: f32) -> AxisAng {
    AxisAng { x, y, z, w: angle }
}

/// S-008-R44: `axisang_copy(d, v)`.
pub fn axisang_copy(d: &mut AxisAng, v: &AxisAng) {
    *d = *v;
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn layout() {
        use std::mem::{align_of, offset_of, size_of};
        assert_eq!(size_of::<AxisAng>(), 16);
        assert_eq!(align_of::<AxisAng>(), 16);
        assert_eq!(offset_of!(AxisAng, w), 12);
    }

    #[test]
    fn set_zero_copy() {
        let a = axisang_set(0.0, 0.0, 1.0, std::f32::consts::FRAC_PI_2);
        assert_eq!(a.w, std::f32::consts::FRAC_PI_2);
        assert_eq!(
            axisang_zero(),
            AxisAng {
                x: 0.0,
                y: 0.0,
                z: 0.0,
                w: 0.0
            }
        );
        let mut c = axisang_zero();
        axisang_copy(&mut c, &a);
        assert_eq!(c, a);
    }
}
