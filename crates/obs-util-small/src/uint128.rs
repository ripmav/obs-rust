//! S-024 M2: 128-bit helpers (`util_uint128.h` / `util_uint64.h`).

/// `util_uint128_t`: 16 bytes, `i32[k]` are the little-endian 32-bit
/// limbs (S-024-R10, vector `u128_layout`).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
#[repr(C)]
pub struct UtilUInt128 {
    i32: [u32; 4],
}

impl UtilUInt128 {
    /// From the two 64-bit halves (`value = high * 2^64 + low`).
    pub fn from_parts(low: u64, high: u64) -> Self {
        let mut limbs = [0u32; 4];
        limbs[0] = low as u32;
        limbs[1] = (low >> 32) as u32;
        limbs[2] = high as u32;
        limbs[3] = (high >> 32) as u32;
        Self { i32: limbs }
    }

    pub fn low(&self) -> u64 {
        self.i32[0] as u64 | ((self.i32[1] as u64) << 32)
    }

    pub fn high(&self) -> u64 {
        self.i32[2] as u64 | ((self.i32[3] as u64) << 32)
    }

    /// `i32[k]`, the LE 32-bit limbs.
    pub fn limbs(&self) -> [u32; 4] {
        self.i32
    }

    pub fn as_u128(&self) -> u128 {
        (self.high() as u128) << 64 | self.low() as u128
    }
}

/// `util_add128`: addition modulo 2^128 (the carry out of bit 127 is
/// dropped).
pub fn util_add128(a: UtilUInt128, b: UtilUInt128) -> UtilUInt128 {
    let r = a.as_u128().wrapping_add(b.as_u128());
    UtilUInt128::from_parts(r as u64, (r >> 64) as u64)
}

/// `util_lshift64_internal_32`: `a * 2^32` (exact, 96 bits).
pub fn util_lshift64_internal_32(a: u64) -> UtilUInt128 {
    let r = (a as u128) << 32;
    UtilUInt128::from_parts(r as u64, (r >> 64) as u64)
}

/// `util_lshift64_internal_64`: `a * 2^64`.
pub fn util_lshift64_internal_64(a: u64) -> UtilUInt128 {
    UtilUInt128::from_parts(0, a)
}

/// `util_mul64_64`: the exact 64x64 -> 128-bit product.
pub fn util_mul64_64(a: u64, b: u64) -> UtilUInt128 {
    let r = (a as u128) * (b as u128);
    UtilUInt128::from_parts(r as u64, (r >> 64) as u64)
}

/// `util_div128_32`: `floor(a / b)` as a 128-bit quotient. `b == 0`
/// violates the precondition (D-1: the reference raises SIGFPE); the
/// port panics and returns no value.
pub fn util_div128_32(a: UtilUInt128, b: u32) -> UtilUInt128 {
    let r = a.as_u128() / (b as u128);
    UtilUInt128::from_parts(r as u64, (r >> 64) as u64)
}

/// `util_mul_div64(n, m, d)` (R09/Q-2): the exact
/// `floor(n * m / d)` computed with a 128-bit intermediate, returned
/// when it fits in u64; a quotient that does not fit saturates to
/// `u64::MAX` (the documented choice — the reference's value there is
/// meaningless: MSVC raises a divide error, the Linux formula wraps).
/// `d == 0` violates the precondition (D-1): the port panics.
pub fn util_mul_div64(n: u64, m: u64, d: u64) -> u64 {
    let r = (n as u128) * (m as u128) / (d as u128);
    u64::try_from(r).unwrap_or(u64::MAX)
}

#[cfg(test)]
mod tests {
    use super::*;

    type CaseAdd = ((u64, u64), (u64, u64), (u64, u64));
    type CaseDiv = ((u64, u64), u32, (u64, u64));

    fn u128(low: u64, high: u64) -> UtilUInt128 {
        UtilUInt128::from_parts(low, high)
    }

    // vector u128-add (the layout line is covered by the layout test)
    #[test]
    fn u128_add() {
        let cases: [CaseAdd; 10] = [
            ((0, 0), (0, 0), (0, 0)),
            ((1, 0), (2, 0), (3, 0)),
            ((0xffffffff, 0), (1, 0), (0x100000000, 0)),
            ((0xffffffffffffffff, 0), (1, 0), (0, 1)),
            ((0xffffffffffffffff, 0xffffffffffffffff), (1, 0), (0, 0)),
            (
                (0xffffffffffffffff, 0xffffffffffffffff),
                (0xffffffffffffffff, 0xffffffffffffffff),
                (0xfffffffffffffffe, 0xffffffffffffffff),
            ),
            ((0, 0xffffffffffffffff), (0, 1), (0, 0)),
            (
                (0x123456789abcdef0, 0x0fedcba987654321),
                (0x0fedcba987654321, 0x123456789abcdef0),
                (0x2222222222222211, 0x2222222222222211),
            ),
            (
                (0x00000000ffffffff, 0x00000000ffffffff),
                (0x0000000000000001, 0x0000000000000001),
                (0x0000000100000000, 0x0000000100000000),
            ),
            (
                (0x8000000000000000, 0x8000000000000000),
                (0x8000000000000000, 0x8000000000000000),
                (0, 1),
            ),
        ];
        for (a, b, want) in cases {
            let r = util_add128(u128(a.0, a.1), u128(b.0, b.1));
            assert_eq!((r.low(), r.high()), want, "{a:?} + {b:?}");
        }
    }

    // vector u128-mul64
    #[test]
    fn u128_mul64() {
        let cases: [(u64, u64, (u64, u64)); 13] = [
            (0, 0, (0, 0)),
            (1, 1, (1, 0)),
            (0, 0xffffffffffffffff, (0, 0)),
            (0xffffffffffffffff, 1, (0xffffffffffffffff, 0)),
            (0xffffffff, 0xffffffff, (0xfffffffe00000001, 0)),
            (0x100000000, 0x100000000, (0, 1)),
            (
                0xffffffffffffffff,
                0xffffffffffffffff,
                (0x0000000000000001, 0xfffffffffffffffe),
            ),
            (0x8000000000000000, 2, (0, 1)),
            (
                0x123456789abcdef0,
                0xfedcba9876543210,
                (0x236d88fe5618cf00, 0x121fa00ad77d7422),
            ),
            (1000000000, 1000000000, (0x0de0b6b3a7640000, 0)),
            (90000, 48000, (0x00000001017df800, 0)),
            (
                0xdeadbeefcafebabe,
                0x0123456789abcdef,
                (0x7eb689f4ea447d62, 0x00fd5bdeeeb2a01d),
            ),
            (
                0x00000001ffffffff,
                0xffffffff00000001,
                (0x00000002ffffffff, 0x00000001fffffffd),
            ),
        ];
        for (a, b, want) in cases {
            let r = util_mul64_64(a, b);
            assert_eq!((r.low(), r.high()), want, "{a:#x} * {b:#x}");
        }
    }

    // vector u128-div32
    #[test]
    fn u128_div32() {
        let cases: [CaseDiv; 12] = [
            ((0, 0), 1, (0, 0)),
            ((10, 0), 3, (3, 0)),
            (
                (0xffffffffffffffff, 0xffffffffffffffff),
                1,
                (0xffffffffffffffff, 0xffffffffffffffff),
            ),
            (
                (0xffffffffffffffff, 0xffffffffffffffff),
                0xffffffff,
                (0x0000000100000001, 0x0000000100000001),
            ),
            (
                (0xffffffffffffffff, 0xffffffffffffffff),
                2,
                (0xffffffffffffffff, 0x7fffffffffffffff),
            ),
            ((0, 1), 2, (0x8000000000000000, 0)),
            ((0, 1), 3, (0x5555555555555555, 0)),
            (
                (0x123456789abcdef0, 0x0fedcba987654321),
                1000,
                (0x8b89c7c43773638d, 0x000413ea68d6e2a0),
            ),
            ((100, 0), 200, (0, 0)),
            ((0xffffffff, 0), 0xffffffff, (1, 0)),
            ((0, 0x100000000), 0x80000000, (0, 2)),
            (
                (0x0000000100000000, 0x0000000200000000),
                0x00000004,
                (0x0000000040000000, 0x00000080000000),
            ),
        ];
        for ((low, high), d, want) in cases {
            let r = util_div128_32(u128(low, high), d);
            assert_eq!((r.low(), r.high()), want, "({low:#x},{high:#x}) / {d:#x}");
        }
    }

    // vector u128-shift
    #[test]
    fn u128_shift() {
        let s32: [(u64, (u64, u64)); 5] = [
            (0, (0, 0)),
            (1, (0x100000000, 0)),
            (0xffffffff, (0xffffffff00000000, 0)),
            (0x100000000, (0, 1)),
            (0xffffffffffffffff, (0xffffffff00000000, 0xffffffff)),
        ];
        for (a, want) in s32 {
            let r = util_lshift64_internal_32(a);
            assert_eq!((r.low(), r.high()), want, "32 << {a:#x}");
        }
        let s64: [(u64, (u64, u64)); 3] = [
            (0, (0, 0)),
            (1, (0, 1)),
            (0xffffffffffffffff, (0, 0xffffffffffffffff)),
        ];
        for (a, want) in s64 {
            let r = util_lshift64_internal_64(a);
            assert_eq!((r.low(), r.high()), want, "64 << {a:#x}");
        }
    }

    // vector u128_layout (R10)
    #[test]
    fn u128_layout() {
        assert!(std::mem::size_of::<UtilUInt128>() == 16);
        // the reference prints low=0000000200000001 high=0000000400000003
        // for i32 = [1, 2, 3, 4]: low = i32[1]<<32 | i32[0], high = i32[3]<<32 | i32[2]
        let v = UtilUInt128::from_parts(0x0000000200000001, 0x0000000400000003);
        assert_eq!(v.limbs(), [1, 2, 3, 4]);
    }

    // vector muldiv64: the port returns the exact value (the reference
    // column is pinned in the vector file)
    #[test]
    fn mul_div64() {
        let cases: [(u64, u64, u64, u64); 15] = [
            (0, 5, 7, 0),
            (1, 1, 1, 1),
            (10, 3, 4, 7),
            (1000000, 48000, 1000000, 48000),
            (90000, 1000, 90000, 1000),
            (12345678901, 3, 7, 5291005243),
            (0x7fffffff, 0x7fffffff, 0x7fffffff, 2147483647),
            (1000000000, 1000000000, 3, 333333333333333333),
            (0xffffffffffffffff, 1, 1, 0xffffffffffffffff),
            (0xffffffffffffffff, 1, 0xffffffffffffffff, 1),
            (0xffffffffffffffff, 1000, 1000, 0xffffffffffffffff),
            (1000000000000, 90000, 1000000000, 90000000),
            (5, 4, 2, 10),
            (0xffffffff, 0xffffffff, 0xffffffff, 4294967295),
            (3, 0x7fffffffffffffff, 0x7fffffffffffffff, 3),
        ];
        for (n, m, d, want) in cases {
            assert_eq!(util_mul_div64(n, m, d), want, "{n} * {m} / {d}");
        }
    }

    // vector muldiv64-quirk-overflow (Q-2): the exact value wins over
    // the reference's wrapping formula; an exact quotient that does not
    // fit in u64 saturates to u64::MAX (the documented choice)
    #[test]
    fn mul_div64_quirk_overflow() {
        assert_eq!(
            util_mul_div64(10000000000000, 10000000000000, 0x7fffffffffffffff),
            10842021
        );
        assert_eq!(util_mul_div64(5, 0xffffffffffffffff, 0xffffffffffffffff), 5);
        assert_eq!(
            util_mul_div64(0xffffffffffffffff, 3, 4),
            13835058055282163711
        );
        assert_eq!(
            util_mul_div64(0xffffffffffffffff, 0xffffffffffffffff, 1),
            u64::MAX
        );
    }

    // vector u128-defect-div0 (D-1): division by zero is a precondition
    // violation — the port panics and returns no value
    #[test]
    #[should_panic]
    fn u128_defect_div0() {
        let _ = util_div128_32(u128(1, 0), 0);
    }

    #[test]
    #[should_panic]
    fn mul_div64_by_zero() {
        let _ = util_mul_div64(1, 1, 0);
    }
}
