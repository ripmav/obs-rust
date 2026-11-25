//! Bit reader (spec S-010 §5.6, `libobs/util/bitstream.{c,h}`).
//!
//! MSB-first reader over a byte slice. The free-function API mirrors the C
//! `bitstream_reader_*` surface; the caller keeps the state (the `pos`/`subPos`
//! pair) so the C-ABI bridge can operate on the C-owned
//! `struct bitstream_reader` (S-010 §3.3) without a copy.
//!
//! The quirks are the spec: `pos` is an 8-bit byte index (Q-3 — for `len > 256`
//! it wraps to byte 0 after byte 255 and the end of the buffer is never
//! reached), `read_bits` uses an 8-bit accumulator (Q-4 — for `n > 8` all `n`
//! bits are consumed but only the last 8 are retained), and bits past the end
//! read as 0 without advancing.

/// S-010-R50: zero the state for a fresh reader (`buf`/`len` are set by the
/// caller from the borrowed slice).
pub fn init(pos: &mut u8, sub_pos: &mut u8) {
    *pos = 0;
    *sub_pos = 0x80;
}

/// S-010-R51: one bit, MSB first. Past the end (`pos >= len`) returns 0 and
/// changes nothing. `buf` must be valid for `len` bytes; `len` may be smaller
/// than the slice (R54).
///
/// # Safety
/// `buf` is valid for reads of `len` bytes for the duration of the call (the
/// C-ABI contract of `bitstream_reader_init`).
pub unsafe fn read_bit(buf: *const u8, len: usize, pos: &mut u8, sub_pos: &mut u8) -> u8 {
    // SAFETY: `pos` is a byte index and `pos < len` is checked below; the read stays within `buf[0..len)`.
    if (*pos as usize) >= len {
        return 0;
    }
    // SAFETY: `pos` was checked `< len` above; the byte read stays within `buf[0..len)`.
    let byte = unsafe { *buf.add(*pos as usize) };
    let bit = if byte & *sub_pos != 0 { 1 } else { 0 };
    *sub_pos >>= 1;
    if *sub_pos == 0 {
        *sub_pos = 0x80;
        // Q-3: `pos` is an unsigned 8-bit field — `pos++` in C wraps.
        *pos = pos.wrapping_add(1);
    }
    bit
}

/// S-010-R52: `n` bits into an 8-bit accumulator, MSB first. `n <= 0` reads
/// nothing and returns 0; `n > 8` consumes all `n` bits and returns the last
/// 8 (Q-4).
///
/// # Safety
/// As [`read_bit`].
pub unsafe fn read_bits(
    buf: *const u8,
    len: usize,
    pos: &mut u8,
    sub_pos: &mut u8,
    bits: i32,
) -> u8 {
    let mut res: u8 = 0;
    for _ in 1..=bits {
        res <<= 1;
        // SAFETY: forwarded contract; the shift is a constant 1 (8-bit arithmetic, the C `uint8_t` semantics).
        res |= unsafe { read_bit(buf, len, pos, sub_pos) };
    }
    res
}

/// S-010-R53: `read_bits(8)`.
///
/// # Safety
/// As [`read_bit`].
#[must_use]
pub unsafe fn r8(buf: *const u8, len: usize, pos: &mut u8, sub_pos: &mut u8) -> u8 {
    // SAFETY: forwarded contract — the caller holds a live reader state.
    unsafe { read_bits(buf, len, pos, sub_pos, 8) }
}

/// S-010-R53: big-endian 16-bit read — the high byte is read first, then the
/// low byte; both reads happen in that order. Past the end the missing bytes
/// read as 0 (a partial value keeps the already-read leading bytes).
///
/// # Safety
/// As [`read_bit`].
#[must_use]
pub unsafe fn r16(buf: *const u8, len: usize, pos: &mut u8, sub_pos: &mut u8) -> u16 {
    // SAFETY: forwarded contract; the two reads happen in the reference order.
    let hi = unsafe { read_bits(buf, len, pos, sub_pos, 8) };
    // SAFETY: as above (second read after the first).
    let lo = unsafe { read_bits(buf, len, pos, sub_pos, 8) };
    ((hi as u16) << 8) | lo as u16
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Drive the free functions through a local state pair (the bridge's
    /// shape: C owns the struct, the functions mutate the fields in place).
    struct Reader<'a> {
        buf: &'a [u8],
        pos: u8,
        sub: u8,
    }

    impl<'a> Reader<'a> {
        fn new(buf: &'a [u8], len: usize) -> Self {
            let mut r = Self {
                buf,
                pos: 0,
                sub: 0,
            };
            init(&mut r.pos, &mut r.sub);
            assert!(len <= buf.len());
            r
        }

        fn bit(&mut self, len: usize) -> u8 {
            // SAFETY: `buf` is a live slice (test); `pos`/`sub` are disjoint fields.
            unsafe { read_bit(self.buf.as_ptr(), len, &mut self.pos, &mut self.sub) }
        }

        fn bits(&mut self, len: usize, n: i32) -> u8 {
            // SAFETY: `buf` is a live slice (test); `pos`/`sub` are disjoint fields.
            unsafe { read_bits(self.buf.as_ptr(), len, &mut self.pos, &mut self.sub, n) }
        }

        fn r8(&mut self, len: usize) -> u8 {
            // SAFETY: `buf` is a live slice (test); `pos`/`sub` are disjoint fields.
            unsafe { r8(self.buf.as_ptr(), len, &mut self.pos, &mut self.sub) }
        }

        fn r16(&mut self, len: usize) -> u16 {
            // SAFETY: `buf` is a live slice (test); `pos`/`sub` are disjoint fields.
            unsafe { r16(self.buf.as_ptr(), len, &mut self.pos, &mut self.sub) }
        }
    }

    #[test]
    fn r50_init_state() {
        // R50: pos 0, subPos 0x80 (buf/len set by the caller).
        let mut pos = 7u8;
        let mut sub = 1u8;
        init(&mut pos, &mut sub);
        assert_eq!((pos, sub), (0, 0x80));
    }

    #[test]
    fn r51_msb_first_and_end() {
        // R51: MSB first; past the end reads 0 and changes nothing.
        let data = [0b1011_0000u8, 0b0000_1111];
        let mut r = Reader::new(&data, 2);
        assert_eq!(
            (
                r.bit(2),
                r.bit(2),
                r.bit(2),
                r.bit(2),
                r.bit(2),
                r.bit(2),
                r.bit(2),
                r.bit(2)
            ),
            (1, 0, 1, 1, 0, 0, 0, 0)
        );
        assert_eq!((r.pos, r.sub), (1, 0x80)); // byte 0 consumed
        // Second byte 0b0000_1111: the high four bits are 0, the low four are 1.
        assert_eq!((r.bit(2), r.bit(2)), (0, 0));
        assert_eq!((r.pos, r.sub), (1, 0x20));
        assert_eq!((r.bit(2), r.bit(2)), (0, 0));
        assert_eq!((r.pos, r.sub), (1, 0x08));
        for _ in 0..4 {
            assert_eq!(r.bit(2), 1);
        }
        assert_eq!((r.pos, r.sub), (2, 0x80)); // all 16 bits consumed
        let (pos, sub) = (r.pos, r.sub);
        assert_eq!(r.bit(2), 0); // pos == len: 0, unchanged
        assert_eq!((r.pos, r.sub), (pos, sub));
        assert_eq!(r.bit(2), 0);
    }

    #[test]
    fn r52_n_zero_negative_and_wide() {
        let data = [0xA5u8, 0xC3];
        let mut r = Reader::new(&data, 2);
        assert_eq!(r.bits(2, 0), 0); // n <= 0: nothing read
        assert_eq!(r.bits(2, -3), 0);
        assert_eq!((r.pos, r.sub), (0, 0x80));
        // Q-4: n > 8 consumes n bits, returns the last 8. The 12 bits are
        // 1010 0101 1100 (0xA5C3 without the final 0011); the last 8 of
        // those = 0101 1100 = 0x5C.
        assert_eq!(r.bits(2, 12), 0x5C);
        assert_eq!((r.pos, r.sub), (1, 0x08)); // 4 bits into byte 1
        assert_eq!(r.bits(2, 4), 0b0011); // the remaining 4 bits of 0xC3
    }

    #[test]
    fn r53_r8_r16_big_endian_and_partial() {
        // R53 (vector bitstream_basic shape): r16 is big endian.
        let data = [0x34u8, 0xFF, 0xE1, 0x23, 0x91, 0x45];
        let mut r = Reader::new(&data, 6);
        assert_eq!(r.r8(6), 0x34);
        assert_eq!(r.bits(6, 1), 1);
        assert_eq!(r.bits(6, 3), 7);
        assert_eq!(r.bits(6, 4), 0xF);
        assert_eq!(r.r8(6), 0xE1);
        assert_eq!(r.r16(6), 0x2391);
        assert_eq!(r.r8(6), 0x45); // byte 5
        // Past the end: r8 -> 0; r16 with one byte left keeps the leading byte.
        assert_eq!(r.r8(6), 0);
        let data2 = [0xABu8];
        let mut r2 = Reader::new(&data2, 1);
        assert_eq!(r2.r8(1), 0xAB);
        assert_eq!(r2.r16(1), 0); // both bytes past the end
        let mut r3 = Reader::new(&[0x12u8, 0x34], 2);
        assert_eq!(r3.r8(2), 0x12);
        assert_eq!(r3.r16(2), 0x3400); // first byte kept, second reads 0
    }

    #[test]
    fn q3_pos_wraps_at_byte_255() {
        // Q-3: `pos` is 8 bits. Vector bitstream_quirk_pos_wrap shape:
        // pattern byte i = (i * 31 + 7) mod 256.
        let data: Vec<u8> = (0..300u32).map(|i| ((i * 31 + 7) % 256) as u8).collect();
        let mut r = Reader::new(&data, 300);
        for _ in 0..255 {
            r.r8(300);
        }
        assert_eq!((r.pos, r.sub), (255, 0x80));
        assert_eq!(r.r8(300), data[255]); // byte 255 is the last reachable one
        assert_eq!((r.pos, r.sub), (0, 0x80)); // wrapped to byte 0
        assert_eq!(r.r8(300), data[0]); // re-reads the start
        assert_eq!((r.pos, r.sub), (1, 0x80));
        for _ in 0..44 {
            r.r8(300);
        }
        assert_eq!((r.pos, r.sub), (45, 0x80));
        assert_eq!(r.r8(300), data[45]); // vector: 45*31+7 mod 256 = 0x7a

        // len = 256 also wraps (never "ends"); len = 255 ends correctly.
        let mut r256 = Reader::new(&data, 256);
        for _ in 0..256 {
            r256.r8(256);
        }
        assert_eq!((r256.pos, r256.sub), (0, 0x80)); // wrapped, not at rest
        assert_eq!(r256.r8(256), data[0]); // re-reads the start (vector: 0x07)

        let mut r255 = Reader::new(&data, 255);
        for _ in 0..255 {
            r255.r8(255);
        }
        assert_eq!((r255.pos, r255.sub), (255, 0x80));
        assert_eq!(r255.r8(255), 0); // at rest: 0 forever
    }

    #[test]
    fn r54_len_zero_and_clamped() {
        // R54: len = 0 is valid (all reads 0, state unchanged); len smaller
        // than the slice is honoured (the reader never reads outside [0..len)).
        let data = [0xFFu8, 0x00];
        let mut r = Reader::new(&data, 0);
        assert_eq!(r.r8(0), 0);
        assert_eq!((r.pos, r.sub), (0, 0x80));
        let mut r2 = Reader::new(&data, 1);
        assert_eq!(r2.r8(1), 0xFF);
        assert_eq!(r2.r8(1), 0); // byte 1 is outside `len`
        assert_eq!((r2.pos, r2.sub), (1, 0x80));
    }
}
