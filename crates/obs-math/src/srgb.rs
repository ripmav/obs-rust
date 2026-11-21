//! `srgb` (S-009 §5.9; `libobs/graphics/srgb.h`).
//!
//! sRGB gamma conversion, u8/float conversions and premultiplication
//! helpers. Pure computations on caller memory.

/// S-009-R120: `u <= 0.04045 ? u/12.92 : powf((u+0.055)/1.055, 2.4)`.
/// Inputs below 0 take the linear branch; NaN fails the comparison and
/// goes through `powf`; inputs above 1 are not clamped. `powf` (f32),
/// not `f64` `pow`.
#[must_use]
pub fn gs_srgb_nonlinear_to_linear(u: f32) -> f32 {
    if u <= 0.04045 {
        u / 12.92
    } else {
        ((u + 0.055) / 1.055).powf(2.4)
    }
}

/// S-009-R120: `u <= 0.0031308 ? 12.92*u : 1.055*powf(u, 1.0/2.4) −
/// 0.055`. `1/2.4` is the f32 quotient. `linear_to_nonlinear(1.0)`
/// evaluates to `0.99999994` in f32.
#[must_use]
pub fn gs_srgb_linear_to_nonlinear(u: f32) -> f32 {
    if u <= 0.0031308 {
        12.92 * u
    } else {
        1.055 * u.powf(1.0 / 2.4) - 0.055
    }
}

/// S-009-R120: apply `gs_srgb_nonlinear_to_linear` to three lanes
/// (`f[3]` of a wider array is not touched).
pub fn gs_float3_srgb_nonlinear_to_linear(f: &mut [f32]) {
    assert!(f.len() >= 3);
    for x in f.iter_mut().take(3) {
        let v = gs_srgb_nonlinear_to_linear(*x);
        *x = v;
    }
}

/// S-009-R120: apply `gs_srgb_linear_to_nonlinear` to three lanes.
pub fn gs_float3_srgb_linear_to_nonlinear(f: &mut [f32]) {
    assert!(f.len() >= 3);
    for x in f.iter_mut().take(3) {
        let v = gs_srgb_linear_to_nonlinear(*x);
        *x = v;
    }
}

// ── S-009-R121/R122: u8 <-> float ──────────────────────────────────────

/// S-009-R121: `(f32)u / 255.0`.
#[must_use]
pub fn gs_u8_to_float(u: u8) -> f32 {
    u as f32 / 255.0
}

/// S-009-R121/Q-14: `(u8)(f*255.0 + 0.5)` — add 0.5 and truncate
/// toward zero, **without clamping** in the defined range (the open
/// interval `(-1, 256)`); outside it the reference is undefined, so
/// this implementation saturates.
#[must_use]
pub fn gs_float_to_u8(f: f32) -> u8 {
    (f * 255.0 + 0.5).clamp(0.0, 255.0) as u8
}

/// S-009-R122: four lanes.
pub fn gs_u8x4_to_float4(f: &mut [f32; 4], u: &[u8; 4]) {
    for i in 0..4 {
        f[i] = gs_u8_to_float(u[i]);
    }
}

/// S-009-R122: four lanes.
pub fn gs_float4_to_u8x4(u: &mut [u8; 4], f: &[f32; 4]) {
    for i in 0..4 {
        u[i] = gs_float_to_u8(f[i]);
    }
}

/// S-009-R122: **three** bytes; `u[3]` is not written.
pub fn gs_float3_to_u8x3(u: &mut [u8; 4], f: &[f32; 3]) {
    u[0] = gs_float_to_u8(f[0]);
    u[1] = gs_float_to_u8(f[1]);
    u[2] = gs_float_to_u8(f[2]);
}

// ── S-009-R123/R124: premultiply ───────────────────────────────────────

/// S-009-R123: `f[0..3] *= f[3]` (the original `f[3]`), alpha
/// unchanged.
pub fn gs_premultiply_float4(f: &mut [f32; 4]) {
    let a = f[3];
    f[0] *= a;
    f[1] *= a;
    f[2] *= a;
}

fn premultiply_u8x4(data: &mut [u8; 4]) {
    let mut f = [0.0f32; 4];
    gs_u8x4_to_float4(&mut f, data);
    gs_premultiply_float4(&mut f);
    gs_float3_to_u8x3(data, &[f[0], f[1], f[2]]);
    // data[3] is unchanged
}

/// S-009-R124: in place — convert, premultiply, write back the first
/// three bytes; the alpha byte is unchanged.
pub fn gs_premultiply_xyza(data: &mut [u8; 4]) {
    premultiply_u8x4(data)
}

/// S-009-R124: read `src`, write **all four bytes** of `dst` (alpha
/// copied). `dst` and `src` do not overlap (precondition).
pub fn gs_premultiply_xyza_restrict(dst: &mut [u8; 4], src: &[u8; 4]) {
    let mut f = [0.0f32; 4];
    gs_u8x4_to_float4(&mut f, src);
    gs_premultiply_float4(&mut f);
    gs_float4_to_u8x4(dst, &f);
}

/// S-009-R125: the in-place form over `n` consecutive 4-byte texels
/// (`n = 0` does nothing; bytes beyond `4*n` are untouched).
pub fn gs_premultiply_xyza_loop(data: &mut [u8], n: usize) {
    for i in 0..n {
        let mut texel = [0u8; 4];
        texel.copy_from_slice(&data[4 * i..4 * i + 4]);
        gs_premultiply_xyza(&mut texel);
        data[4 * i..4 * i + 4].copy_from_slice(&texel);
    }
}

/// S-009-R125: the restrict form over `n` texels.
pub fn gs_premultiply_xyza_restrict_loop(dst: &mut [u8], src: &[u8], n: usize) {
    for i in 0..n {
        let mut src_texel = [0u8; 4];
        src_texel.copy_from_slice(&src[4 * i..4 * i + 4]);
        let mut dst_texel = [0u8; 4];
        gs_premultiply_xyza_restrict(&mut dst_texel, &src_texel);
        dst[4 * i..4 * i + 4].copy_from_slice(&dst_texel);
    }
}

// ── S-009-R125: the _srgb forms ────────────────────────────────────────

fn premultiply_u8x4_srgb(data: &mut [u8; 4]) {
    let mut f = [0.0f32; 4];
    gs_u8x4_to_float4(&mut f, data);
    gs_float3_srgb_nonlinear_to_linear(&mut f[..3]);
    gs_premultiply_float4(&mut f);
    gs_float3_srgb_linear_to_nonlinear(&mut f[..3]);
    gs_float3_to_u8x3(data, &[f[0], f[1], f[2]]);
    // data[3] is unchanged
}

/// S-009-R125: sRGB in-place — nonlinear->linear on the colour
/// channels, premultiply, linear->nonlinear, write back (alpha
/// unchanged). Example: `ff804080 -> bc5d2d80` (±1 per byte).
pub fn gs_premultiply_xyza_srgb(data: &mut [u8; 4]) {
    premultiply_u8x4_srgb(data)
}

/// S-009-R125: sRGB restrict form — writes all four bytes of `dst`
/// (alpha copied).
pub fn gs_premultiply_xyza_restrict_srgb(dst: &mut [u8; 4], src: &[u8; 4]) {
    let mut f = [0.0f32; 4];
    gs_u8x4_to_float4(&mut f, src);
    gs_float3_srgb_nonlinear_to_linear(&mut f[..3]);
    gs_premultiply_float4(&mut f);
    gs_float3_srgb_linear_to_nonlinear(&mut f[..3]);
    gs_float4_to_u8x4(dst, &f);
}

/// S-009-R125: sRGB in-place loop.
pub fn gs_premultiply_xyza_srgb_loop(data: &mut [u8], n: usize) {
    for i in 0..n {
        let mut texel = [0u8; 4];
        texel.copy_from_slice(&data[4 * i..4 * i + 4]);
        gs_premultiply_xyza_srgb(&mut texel);
        data[4 * i..4 * i + 4].copy_from_slice(&texel);
    }
}

/// S-009-R125: sRGB restrict loop.
pub fn gs_premultiply_xyza_restrict_srgb_loop(dst: &mut [u8], src: &[u8], n: usize) {
    for i in 0..n {
        let mut src_texel = [0u8; 4];
        src_texel.copy_from_slice(&src[4 * i..4 * i + 4]);
        let mut dst_texel = [0u8; 4];
        gs_premultiply_xyza_restrict_srgb(&mut dst_texel, &src_texel);
        dst[4 * i..4 * i + 4].copy_from_slice(&dst_texel);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn r120_gamma() {
        // S-009-R120
        assert_eq!(gs_srgb_nonlinear_to_linear(0.0), 0.0);
        assert!((gs_srgb_nonlinear_to_linear(0.04045) - 0.04045 / 12.92).abs() < 1e-7);
        let mid = gs_srgb_nonlinear_to_linear(0.5);
        assert!((mid - (0.555f32 / 1.055f32).powf(2.4)).abs() < 1e-6);
        assert!((mid - 0.21404115).abs() < 1e-6);
        assert!((gs_srgb_nonlinear_to_linear(1.0) - 1.0).abs() < 1e-7);
        // below 0: linear branch (negative output)
        assert!((gs_srgb_nonlinear_to_linear(-0.1) - (-0.1 / 12.92)).abs() < 1e-7);
        // NaN goes through powf
        assert!(gs_srgb_nonlinear_to_linear(f32::NAN).is_nan());
        // above 1 is not clamped
        assert!(gs_srgb_nonlinear_to_linear(2.0) > 1.0);

        // the pinned f32 value: linear_to_nonlinear(1.0) = 0.99999994
        let one = gs_srgb_linear_to_nonlinear(1.0);
        assert!((one - 0.99999994f32).abs() <= 1.5e-7, "got {one}");
        assert!(gs_srgb_linear_to_nonlinear(0.0) == 0.0);
        assert!((gs_srgb_linear_to_nonlinear(0.0031308) - 0.04045).abs() < 1e-6);
        assert!((gs_srgb_linear_to_nonlinear(0.5) - 0.7354).abs() < 1e-4);
        // below 0: linear branch
        assert!((gs_srgb_linear_to_nonlinear(-0.5) - 12.92 * -0.5).abs() < 1e-6);
        // float3 forms: three lanes, the fourth untouched
        let mut f = [0.5, 0.25, 0.125, 9.0];
        gs_float3_srgb_nonlinear_to_linear(&mut f);
        assert!((f[0] - 0.21404115).abs() < 1e-6);
        assert_eq!(f[3], 9.0, "f[3] not touched");
        gs_float3_srgb_linear_to_nonlinear(&mut f);
        assert!((f[0] - 0.5).abs() < 1e-4, "round trip");
    }

    #[test]
    fn r121_u8_float() {
        // S-009-R121 (script 26)
        assert!(gs_u8_to_float(255.0_f32 as u8).abs() - 1.0 < 1e-7);
        assert_eq!(gs_u8_to_float(0), 0.0);
        assert!((gs_u8_to_float(128) - 128.0 / 255.0).abs() < 1e-7);

        assert_eq!(gs_float_to_u8(0.5), 128);
        assert_eq!(gs_float_to_u8(0.002), 1);
        assert_eq!(gs_float_to_u8(0.0019), 0);
        assert_eq!(gs_float_to_u8(-0.004), 0);
        assert_eq!(gs_float_to_u8(1.001), 255);
        // Q-14: no clamping in the defined range; saturation outside
        assert_eq!(gs_float_to_u8(2.0), 255);
        assert_eq!(gs_float_to_u8(-2.0), 0);
        // truncation toward zero (not rounding): 127.9 stays 127
        assert_eq!(gs_float_to_u8((127.9 - 0.5) / 255.0), 127);
    }

    #[test]
    fn r120_r121_256_point_golden() {
        // vector 27-srgb-256-golden (generated from the C reference):
        // four sections of 256 lines over the points i/255 (i = 0..255).
        // Section 1: srgb_nonlinear_to_linear, section 2:
        // srgb_linear_to_nonlinear (floats, printed with %.9g), section 3:
        // u8_to_float(0..255), section 4: float_to_u8 at the points.
        // The C runner takes (float)strtod — the same f32 input is
        // (i as f64) / 255.0 as f32. Tolerance 1e-6 (review 2026-11-21).
        let path = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../test/conformance/vectors/math2/27-srgb-256-golden.expected"
        );
        let text = std::fs::read_to_string(path).expect("normative vector");
        let lines: Vec<&str> = text.lines().collect();
        assert_eq!(lines.len(), 1024);
        for i in 0..256 {
            let p = ((i as f64) / 255.0) as f32;
            let l1 = lines[i].splitn(3, ' ').collect::<Vec<_>>();
            assert_eq!((l1[0], l1[1]), ("c", "float"), "line {i}: {}", lines[i]);
            let want1: f32 = l1[2].parse().unwrap();
            assert!(
                (gs_srgb_nonlinear_to_linear(p) - want1).abs() <= 1e-6,
                "nonlinear->linear i={i}"
            );
            let l2 = lines[256 + i].splitn(3, ' ').collect::<Vec<_>>();
            let want2: f32 = l2[2].parse().unwrap();
            assert!(
                (gs_srgb_linear_to_nonlinear(p) - want2).abs() <= 1e-6,
                "linear->nonlinear i={i}"
            );
            // u8 -> float is exact: u / 255.0
            let l3 = lines[512 + i].splitn(3, ' ').collect::<Vec<_>>();
            let want3: f32 = l3[2].parse().unwrap();
            assert!(
                (gs_u8_to_float(i as u8) - want3).abs() <= 1e-7,
                "u8->float i={i}"
            );
            // float -> u8 at the point: integer, exact
            let l4 = lines[768 + i].splitn(3, ' ').collect::<Vec<_>>();
            let want4: u8 = l4[2].parse().unwrap();
            assert_eq!(gs_float_to_u8(p), want4, "float->u8 i={i}");
        }
    }

    #[test]
    fn r122_lane_conversions() {
        // S-009-R122
        let u = [255u8, 128, 64, 0];
        let mut f = [0.0f32; 4];
        gs_u8x4_to_float4(&mut f, &u);
        assert!((f[0] - 1.0).abs() < 1e-7);
        assert!((f[1] - 128.0 / 255.0).abs() < 1e-7);
        let mut back = [0u8; 4];
        gs_float4_to_u8x4(&mut back, &f);
        assert_eq!(back, [255, 128, 64, 0]);
        // float3: u[3] not written (sentinel)
        let mut u3 = [0u8, 0, 0, 0xEE];
        gs_float3_to_u8x3(&mut u3, &[1.0, 0.5, 0.0]);
        assert_eq!(u3, [255, 128, 0, 0xEE]);
    }

    #[test]
    fn r123_premultiply_float4() {
        // S-009-R123
        let mut f = [1.0, 0.5, 0.25, 0.5];
        gs_premultiply_float4(&mut f);
        assert!((f[0] - 0.5).abs() < 1e-7);
        assert!((f[1] - 0.25).abs() < 1e-7);
        assert!((f[2] - 0.125).abs() < 1e-7);
        assert!((f[3] - 0.5).abs() < 1e-7, "alpha unchanged");
    }

    #[test]
    fn r124_premultiply_xyza() {
        // S-009-R124: the spec examples
        let mut d = [0xff, 0x80, 0x40, 0x80];
        gs_premultiply_xyza(&mut d);
        assert_eq!(d, [0x80, 0x40, 0x20, 0x80]);

        let mut d2 = [0xff, 0xff, 0xff, 0x01];
        gs_premultiply_xyza(&mut d2);
        assert_eq!(d2, [0x01, 0x01, 0x01, 0x01]);

        let mut d3 = [0xff, 0x80, 0x40, 0x00];
        gs_premultiply_xyza(&mut d3);
        assert_eq!(d3, [0x00, 0x00, 0x00, 0x00]);

        // restrict: all four bytes written (alpha copied)
        let mut dst = [0u8; 4];
        gs_premultiply_xyza_restrict(&mut dst, &[0xff, 0x80, 0x40, 0x80]);
        assert_eq!(dst, [0x80, 0x40, 0x20, 0x80]);
    }

    #[test]
    fn r125_premultiply_srgb() {
        // S-009-R125: example ff804080 -> bc5d2d80 (±1 per byte)
        let mut d = [0xff, 0x80, 0x40, 0x80];
        gs_premultiply_xyza_srgb(&mut d);
        let expected = [0xbc, 0x5d, 0x2d, 0x80u8];
        for i in 0..4 {
            assert!(
                (d[i] as i32 - expected[i] as i32).abs() <= 1,
                "lane {i}: {d:02x?} vs {expected:02x?}"
            );
        }

        let mut dst = [0u8; 4];
        gs_premultiply_xyza_restrict_srgb(&mut dst, &[0xff, 0x80, 0x40, 0x80]);
        for i in 0..4 {
            assert!((dst[i] as i32 - expected[i] as i32).abs() <= 1, "lane {i}");
        }
    }

    #[test]
    fn r125_loops() {
        // S-009-R125: loops over n texels
        let mut data = [
            0xff, 0x80, 0x40, 0x80, // texel 0
            0xff, 0xff, 0xff, 0x01, // texel 1
            0xaa, 0xbb, 0xcc, 0xdd, // beyond 2n: untouched
        ];
        gs_premultiply_xyza_loop(&mut data, 2);
        assert_eq!(&data[0..4], &[0x80, 0x40, 0x20, 0x80]);
        assert_eq!(&data[4..8], &[0x01, 0x01, 0x01, 0x01]);
        assert_eq!(&data[8..12], &[0xaa, 0xbb, 0xcc, 0xdd]);

        // n = 0 does nothing
        let before = data;
        gs_premultiply_xyza_loop(&mut data, 0);
        assert_eq!(data, before);

        // restrict loop
        let mut dst = [0u8; 8];
        gs_premultiply_xyza_restrict_loop(&mut dst, &[0xff, 0x80, 0x40, 0x80, 0, 0, 0, 0], 1);
        assert_eq!(&dst[0..4], &[0x80, 0x40, 0x20, 0x80]);
        assert_eq!(&dst[4..8], &[0, 0, 0, 0]);
        // srgb loop
        let mut sd = [0xff, 0x80, 0x40, 0x80];
        gs_premultiply_xyza_srgb_loop(&mut sd, 1);
        assert_eq!(sd[3], 0x80, "alpha unchanged");
    }
}
