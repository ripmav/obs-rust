//! P1-14.2 (WP-32): the C ABI for the bit reader
//! (`libobs/util/bitstream.{c,h}`, spec S-010 §5.6).
//!
//! The C `struct bitstream_reader` is C-owned (the conformance runner and
//! libobs C code declare and fill it); these exports mutate its fields in
//! place through the [`obs_util::bitstream`] free functions. `read_bit` is
//! linkable in the reference but not declared in the header (the runner
//! declares it itself), so the bridge provides it as well.
//!
//! The quirks are the spec: `pos` is 8 bits (Q-3), `read_bits` keeps only
//! the last 8 of the consumed bits (Q-4).

use obs_util::bitstream;
use std::os::raw::c_int;

/// `struct bitstream_reader` (S-010 §3.3: `pos` @0, `subPos` @1, `buf` @8,
/// `len` @16).
#[repr(C)]
pub struct bitstream_reader {
    pub pos: u8,
    pub sub_pos: u8,
    pub buf: *mut u8,
    pub len: usize,
}

/// `EXPORT void bitstream_reader_init(struct bitstream_reader *r, uint8_t
/// *data, size_t len)` (S-010-R50).
///
/// # Safety
/// `r` must be live for the call; after the call `r->buf`/`r->len`
/// reference `data` for `len` bytes (the C contract the caller must keep).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bitstream_reader_init(
    r: *mut bitstream_reader,
    data: *mut u8,
    len: usize,
) {
    // SAFETY: `r` is a live `struct bitstream_reader` (caller contract); the
    // write is a whole-struct store (C: memset + field assignments).
    unsafe {
        std::ptr::write(
            r,
            bitstream_reader {
                pos: 0,
                sub_pos: 0x80,
                buf: data,
                len,
            },
        );
    }
}

/// `EXPORT uint8_t bitstream_reader_read_bits(struct bitstream_reader *r,
/// int bits)` (S-010-R52).
///
/// # Safety
/// `r` must be initialised by `bitstream_reader_init` with `buf` valid for
/// `len` bytes for the whole call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bitstream_reader_read_bits(r: *mut bitstream_reader, bits: c_int) -> u8 {
    // SAFETY: `r` is live and disjoint from the caller's other state; `buf`
    // validity is the caller contract (checked inside against `len`).
    let r = unsafe { &mut *r };
    // SAFETY: read_bits — forwarded contract; the reader state is live and `buf` valid for `len`.
    unsafe { bitstream::read_bits(r.buf, r.len, &mut r.pos, &mut r.sub_pos, bits) }
}

/// `uint8_t bitstream_reader_read_bit(struct bitstream_reader *r)` —
/// linkable in the reference, not declared in the header (S-010-R51).
///
/// # Safety
/// As [`bitstream_reader_read_bits`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bitstream_reader_read_bit(r: *mut bitstream_reader) -> u8 {
    // SAFETY: as `bitstream_reader_read_bits`.
    let r = unsafe { &mut *r };
    // SAFETY: read_bit — forwarded contract; the reader state is live and `buf` valid for `len`.
    unsafe { bitstream::read_bit(r.buf, r.len, &mut r.pos, &mut r.sub_pos) }
}

/// `EXPORT uint8_t bitstream_reader_r8(struct bitstream_reader *r)`
/// (S-010-R53).
///
/// # Safety
/// As [`bitstream_reader_read_bits`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bitstream_reader_r8(r: *mut bitstream_reader) -> u8 {
    // SAFETY: as `bitstream_reader_read_bits`.
    let r = unsafe { &mut *r };
    // SAFETY: r8 — forwarded contract; the reader state is live and `buf` valid for `len`.
    unsafe { bitstream::r8(r.buf, r.len, &mut r.pos, &mut r.sub_pos) }
}

/// `EXPORT uint16_t bitstream_reader_r16(struct bitstream_reader *r)`
/// (S-010-R53, big endian).
///
/// # Safety
/// As [`bitstream_reader_read_bits`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bitstream_reader_r16(r: *mut bitstream_reader) -> u16 {
    // SAFETY: as `bitstream_reader_read_bits`.
    let r = unsafe { &mut *r };
    // SAFETY: r16 — forwarded contract; the reader state is live and `buf` valid for `len`.
    unsafe { bitstream::r16(r.buf, r.len, &mut r.pos, &mut r.sub_pos) }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ptr;

    /// Drive the bridges through a C-shaped struct on the Rust stack (the
    /// cmocka `test_bitstream` shape).
    #[test]
    fn cmocka_bitstream_shape() {
        let mut r = bitstream_reader {
            pos: 0,
            sub_pos: 0,
            buf: ptr::null_mut(),
            len: 0,
        };
        let mut data = [0x34u8, 0xff, 0xe1, 0x23, 0x91, 0x45];
        // SAFETY: `data` outlives the calls; `len` 5 < 6 mirrors the C test
        // ("stop reading at that len").
        unsafe { bitstream_reader_init(&mut r, data.as_mut_ptr(), 5) };
        assert_eq!((r.pos, r.sub_pos, r.len), (0, 0x80, 5));
        // SAFETY: initialised above; `data` is valid for 5 bytes.
        assert_eq!(unsafe { bitstream_reader_read_bits(&mut r, 8) }, 0x34);
        // SAFETY: as above.
        assert_eq!(unsafe { bitstream_reader_read_bits(&mut r, 1) }, 1);
        // SAFETY: as above.
        assert_eq!(unsafe { bitstream_reader_read_bits(&mut r, 3) }, 7);
        // SAFETY: as above.
        assert_eq!(unsafe { bitstream_reader_read_bits(&mut r, 4) }, 0xF);
        // SAFETY: as above.
        assert_eq!(unsafe { bitstream_reader_r8(&mut r) }, 0xE1);
        // SAFETY: as above.
        assert_eq!(unsafe { bitstream_reader_r16(&mut r) }, 0x2391);
        // len 5: byte 5 is outside, the next read is 0.
        // SAFETY: as above.
        assert_eq!(unsafe { bitstream_reader_r8(&mut r) }, 0);
        // SAFETY: as above.
        assert_eq!(unsafe { bitstream_reader_read_bit(&mut r) }, 0);
    }

    /// Q-3 through the C ABI: 300-byte pattern, the state wraps at byte 255
    /// (vector `bitstream_quirk_pos_wrap` shape).
    #[test]
    fn q3_wrap_through_c_abi() {
        let mut data: Vec<u8> = (0..300u32).map(|i| ((i * 31 + 7) % 256) as u8).collect();
        let mut r = bitstream_reader {
            pos: 0,
            sub_pos: 0,
            buf: ptr::null_mut(),
            len: 0,
        };
        // SAFETY: `data` outlives the calls.
        unsafe { bitstream_reader_init(&mut r, data.as_mut_ptr(), 300) };
        for _ in 0..256 {
            // SAFETY: initialised above.
            unsafe { bitstream_reader_r8(&mut r) };
        }
        assert_eq!((r.pos, r.sub_pos), (0, 0x80)); // wrapped
        // SAFETY: as above.
        assert_eq!(unsafe { bitstream_reader_r8(&mut r) }, data[0]);
    }
}
