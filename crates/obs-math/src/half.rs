//! `half` (S-009 §5.8; `libobs/graphics/half.h`).
//!
//! `half` is an IEEE-754 binary16 **bit pattern** (1 sign, 5 exponent
//! bits bias 15, 10 mantissa bits) — no `half -> float` function exists
//! in this component.

/// S-009-R110: the C `struct half { uint16_t u; }` — 2 bytes.
#[repr(C)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Half {
    pub u: u16,
}

/// S-009-R110: `half_from_bits(u).u == u`.
#[must_use]
pub const fn half_from_bits(u: u16) -> Half {
    Half { u }
}

/// S-009-R111: the exact integer algorithm (all arithmetic on `u32`,
/// wrapping), round-to-nearest ties-to-even on the 13 dropped bits.
#[must_use]
pub fn half_from_float(f: f32) -> Half {
    let b = f.to_bits();
    let sign = (b & 0x8000_0000) >> 16;
    let mut a = b & 0x7FFF_FFFF;
    let r: u32 = if a > 0x477F_E000 {
        // Q-10: magnitude above 65504, or inf/NaN
        if (a & 0x7F80_0000) == 0x7F80_0000 && (a & 0x7F_FFFF) != 0 {
            0x7FFF
        } else {
            0x7C00
        }
    } else if a == 0 {
        0
    } else {
        if a < 0x3880_0000 {
            // below 2^-14: half subnormal range
            let e = a >> 23;
            if e <= 81 {
                // D-6: the reference is undefined here; required result
                // is signed zero
                return Half { u: sign as u16 };
            }
            a = (0x80_0000 | (a & 0x7F_FFFF)) >> (113 - e);
        } else {
            // rebias exponent (the reference adds 0xC8000000 mod 2^32)
            a -= 0x3800_0000;
        }
        // round-to-nearest, ties-to-even on the 13 dropped bits
        ((a + 0x0FFF + ((a >> 13) & 1)) >> 13) & 0x7FFF
    };
    Half {
        u: (sign | r) as u16,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use obs_util::hash::blake2b_512;

    /// Exact f16 -> f32 bit embedding, pure integer arithmetic — the
    /// same algorithm the C dumper (test/conformance/gen/half_dump.c)
    /// uses, so both sides feed bit-identical f32 values into
    /// half_from_float. NaN patterns map to the canonical quiet NaN
    /// 0x7fc00000 (the conversion only distinguishes NaN from inf).
    fn f16_to_f32_bits(u: u16) -> u32 {
        let sign = (u as u32 & 0x8000) << 16;
        let e = ((u >> 10) & 0x1F) as u32;
        let m = (u & 0x3FF) as u32;
        if (u & 0x7C00) == 0x7C00 {
            return 0x7F80_0000 | (if m != 0 { 0x0040_0000 } else { 0 }) | sign;
        }
        if e == 0 {
            if m == 0 {
                return sign;
            }
            // subnormal: value = m * 2^-24; normalise to a 10-bit
            // significand with the leading bit at position 9
            let mut f = m;
            while f & 0x200 == 0 {
                f <<= 1;
            }
            let bits_m = 32 - m.leading_zeros(); // m's bit length
            let mant = (f & 0x1FF) << 14;
            let biased = 127 + bits_m - 25;
            sign | (biased << 23) | mant
        } else {
            // normal: value = (1 + m*2^-10) * 2^(e-15)
            sign | ((e + 112) << 23) | (m << 13)
        }
    }

    /// P1-18.2 (review 2026-11-21): all 65536 f16 values round-trip
    /// identically to the C reference — for every bit pattern P, the
    /// exact f32 embedding of P fed through half_from_float must match
    /// the C golden (vectors/math2/half_exhaustive.expected: blake2b-512
    /// of the 65536-line dump + spot lines). The golden records the C
    /// round-trip breaks (small subnormals, NaN payloads) — parity, not
    /// identity.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn r111_exhaustive_65536_roundtrip() {
        let path = concat!(
            env!("CARGO_MANIFEST_DIR"),
            "/../../test/conformance/vectors/math2/half_exhaustive.expected"
        );
        let text = std::fs::read_to_string(path).expect("normative vector");
        let mut want_digest = None;
        let mut spots = Vec::new();
        for line in text.lines() {
            if let Some(h) = line.strip_prefix("blake2b512=") {
                want_digest = Some(hex64(h));
            } else if let Some(rest) = line.strip_prefix("spot ") {
                let (p, out) = rest.split_once(' ').unwrap();
                spots.push((
                    u16::from_str_radix(p.trim_start_matches("0x"), 16).unwrap(),
                    u16::from_str_radix(out.trim_start_matches("0x"), 16).unwrap(),
                ));
            }
        }
        let mut dump = String::with_capacity(65536 * 17);
        for p in 0u32..0x10000 {
            let in_bits = p as u16;
            let f = f32::from_bits(f16_to_f32_bits(in_bits));
            let out = half_from_float(f).u;
            dump.push_str(&format!("half 0x{in_bits:04x} 0x{out:04x}\n"));
        }
        assert_eq!(
            blake2b_512(dump.as_bytes()),
            want_digest.expect("digest in vector"),
            "65536-value f16 round-trip diverges from the C reference"
        );
        for (p, want) in &spots {
            let f = f32::from_bits(f16_to_f32_bits(*p));
            assert_eq!(half_from_float(f).u, *want, "spot {p:04x}");
        }
    }

    fn hex64(s: &str) -> [u8; 64] {
        let mut out = [0u8; 64];
        for i in 0..64 {
            out[i] = u8::from_str_radix(&s[i * 2..i * 2 + 2], 16).unwrap();
        }
        out
    }

    /// Exact power of two via bit pattern — `f32::powi` is NOT exact in
    /// Miri (it computes by general means: 2^-25 becomes 0x32FFFFFC),
    /// which would corrupt the tie-boundary vectors in R113.
    fn pow2f(n: i32) -> f32 {
        f32::from_bits(((n + 127) as u32) << 23)
    }

    fn f(bits: u32) -> f32 {
        f32::from_bits(bits)
    }

    #[test]
    fn r110_layout_roundtrip() {
        // S-009-R110
        assert_eq!(std::mem::size_of::<Half>(), 2);
        assert_eq!(half_from_bits(0x3C00).u, 0x3C00);
        assert_eq!(half_from_bits(0).u, 0);
        assert_eq!(half_from_bits(u16::MAX).u, u16::MAX);
    }

    #[test]
    fn r112_special_values() {
        // S-009-R112 (vectors 22, 24)
        let cases: [(f32, u16); 12] = [
            (1.0, 0x3C00),
            (-1.0, 0xBC00),
            (0.5, 0x3800),
            (65504.0, 0x7BFF),
            (0.0, 0x0000),
            (f(0x8000_0000), 0x8000), // -0.0
            (f32::INFINITY, 0x7C00),
            (f32::NEG_INFINITY, 0xFC00),
            (f32::NAN, 0x7FFF),
            (-f32::NAN, 0xFFFF),
            (pow2f(-14), 0x0400), // smallest normal
            (pow2f(-24), 0x0001),
        ];
        for (v, bits) in cases {
            assert_eq!(half_from_float(v).u, bits, "{v:?}");
        }
        assert_eq!(half_from_float(-pow2f(-24)).u, 0x8001);
        // largest subnormal 0x1.ff8p-15 = 1023 * 2^-24 exactly
        // (= 1.998046875 * 2^-15, which is exactly representable in f32)
        let ls: f32 = f32::from_bits(0x387F_C000);
        assert_eq!(half_from_float(ls).u, 0x03FF);
        // a quiet NaN with payload also collapses
        assert_eq!(half_from_float(f(0x7fc0_1234)).u, 0x7FFF);

        // Q-10: ANY magnitude strictly greater than 65504 -> inf.
        // (65519.99 as an f32 literal folds to 65519.984375.)
        assert_eq!(half_from_float(65504.004_f32).u, 0x7C00);
        assert_eq!(half_from_float(65519.99_f32).u, 0x7C00);
        assert_eq!(half_from_float(65520.0).u, 0x7C00);
        assert_eq!(half_from_float(-65520.0).u, 0xFC00);
        // the boundary itself stays finite (largest f32 below 65504)
        assert_eq!(half_from_float(f32::from_bits(0x477F_DFFC)).u, 0x7BFF);
    }

    #[test]
    fn r113_subnormal_no_sticky() {
        // S-009-R113 (script 23): the shift discards low bits before
        // rounding (no sticky bit). All inputs via exact bit patterns
        // (powi is not exact under Miri).
        let base = pow2f(-25);
        assert_eq!(half_from_float(base).u, 0, "exact tie -> even (0)");
        assert_eq!(half_from_float(base * (1.0 + pow2f(-12))).u, 1);
        assert_eq!(half_from_float(base * (1.0 + pow2f(-13))).u, 0);
        assert_eq!(half_from_float(base * (1.0 + pow2f(-23))).u, 0);
        // below 2^-25 -> zero
        assert_eq!(half_from_float(pow2f(-26)).u, 0);
        // D-6: magnitudes far below -> signed zero (smallest f32
        // subnormals, 2^-149; pow2f only covers exponents in [-126, 127])
        assert_eq!(half_from_float(f32::from_bits(1)).u, 0x0000);
        assert_eq!(half_from_float(f32::from_bits(0x8000_0001)).u, 0x8000);
    }

    #[test]
    fn r111_rounding_and_carry() {
        // round-to-nearest, ties-to-even; carry out of the mantissa
        // increments the exponent (0x03FF -> 0x0400)
        // 0x3FFF is the midpoint between 0x3FFE and 0x3FFF... test the
        // documented boundary via exact f32 values:
        // 1.5 -> 0x3E00 (exactly representable)
        assert_eq!(half_from_float(1.5).u, 0x3E00);
        // value a hair above 1.5f32 rounds up
        let above = 1.5f32 + f32::EPSILON;
        assert_eq!(half_from_float(above).u, 0x3E00);
        // 65504 is exact; the smallest f32 STRICTLY above it
        // (65504.00390625, ulp there is 2^-8) already overflows to inf
        // (Q-10: IEEE round-to-nearest would map (65504, 65520) to 65504)
        assert_eq!(
            half_from_float(f32::from_bits(0x477F_E001)).u,
            0x7C00,
            "Q-10"
        );
        // 65504.004 as an f32 literal folds to 65504.00390625
        assert_eq!(half_from_float(65504.004_f32).u, 0x7C00);
        // negative rounding
        assert_eq!(half_from_float(-1.5).u, 0xBE00);
    }
}
