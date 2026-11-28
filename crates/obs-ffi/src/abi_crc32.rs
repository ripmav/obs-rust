//! P1-10.4 recipe step 2: the C ABI for `calc_crc32`
//! (`libobs/util/crc32.{c,h}`) — the one exported function of the file.
//!
//! The Rust side is `obs_util::hash::calc_crc32` (S-055 U1, `crc32fast`);
//! the bridge is a pure forwarding — the exact C signature
//! (`uint32_t calc_crc32(uint32_t crc, const void *buf, size_t size)`),
//! no pointer crosses an allocator boundary.

use std::os::raw::c_void;

/// `EXPORT uint32_t calc_crc32(uint32_t crc, const void *buf, size_t size)`
///
/// # Safety
/// `buf` must be readable for `size` bytes (NULL is fine for `size == 0`);
/// no other constraint — the reference does not touch the buffer beyond
/// the read.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn calc_crc32(crc: u32, buf: *const c_void, size: usize) -> u32 {
    // SAFETY: caller contract — `buf` is readable for `size` bytes (the slice is read-only, the call does not outlive the call).
    // SAFETY: caller contract — `buf` is readable for `size` bytes; for `size == 0` a dangling pointer is used so the slice is well-formed even when `buf` is NULL (C behaviour).
    let bytes = unsafe {
        if size == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(buf.cast::<u8>(), size)
        }
    };
    obs_util::hash::calc_crc32(crc, bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Bridge forwarding: known-answer (the empty CRC continues from the
    /// seed) + a vector-pinned value (test/conformance/scripts/utilrest/
    /// crc32 script, `crc32` command — seed 0, "123456789" = 0xcbf43926).
    #[test]
    fn calc_crc32_bridge_known_answer() {
        let data = b"123456789";
        // SAFETY: the literal is valid for the call (size is exact).
        let got = unsafe { calc_crc32(0, data.as_ptr().cast::<c_void>(), data.len()) };
        assert_eq!(got, 0xcbf4_3926);
        // The seed continues the running CRC.
        // SAFETY: prefix of the live literal (size 4).
        let part = unsafe { calc_crc32(0, data.as_ptr().cast::<c_void>(), 4) };
        // SAFETY: suffix of the live literal (size 5).
        let cont = unsafe { calc_crc32(part, data[4..].as_ptr().cast::<c_void>(), 5) };
        assert_eq!(cont, 0xcbf4_3926);
    }

    /// Zero size is a no-op on the seed (the C reference may pass NULL).
    #[test]
    fn calc_crc32_bridge_zero_size() {
        // SAFETY: size is 0 — a dangling pointer is the valid zero-length form.
        let got = unsafe { calc_crc32(0x1234_5678, std::ptr::dangling(), 0) };
        assert_eq!(got, 0x1234_5678);
    }
}
