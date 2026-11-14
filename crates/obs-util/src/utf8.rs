//! Extended UTF-8 / wide-character conversion (P1-08.1, spec S-023 §4.1, §5.1–5.3).
//!
//! "wchar" is the reference's `wchar_t`: 32 bits. The reference accepts the
//! "extended" UTF-8 form (S-023 Q-1): overlong forms, 5- and 6-byte forms,
//! and values up to `0x7FFFFFFF`; it rejects only the lead bytes
//! `C0 C1 F5 FF` (without `UTF8_IGNORE_ERROR`) and surrogates. This module
//! MUST match that behaviour — it is deliberately not strict UTF-8.
//!
//! Negative wchar values are unreachable through the `u32` API; the C ABI
//! shim (`i32` `wchar_t`) must check the sign before delegating
//! (S-023-R15c). The locale `os_mbs_*` conversions stay in the C shim
//! (S-023-R22, current locale).

/// S-023 §3.1: decode/encode flags.
pub const UTF8_IGNORE_ERROR: u32 = 0x01;
pub const UTF8_SKIP_BOM: u32 = 0x02;

const SURROGATE_LO: u32 = 0xD800;
const SURROGATE_HI: u32 = 0xDFFF;
const BOM: u32 = 0xFEFF;

fn is_surrogate(v: u32) -> bool {
    (SURROGATE_LO..=SURROGATE_HI).contains(&v)
}

/// S-023 §4.1: (sequence length n, lead payload bits of the lead byte).
/// `None` = not a lead byte (`80..BF`, `FE`, `FF`).
fn lead_class(b: u8) -> Option<(usize, u32)> {
    match b {
        0x00..=0x7F => Some((1, u32::from(b))),
        0xC0..=0xDF => Some((2, u32::from(b & 0x1F))),
        0xE0..=0xEF => Some((3, u32::from(b & 0x0F))),
        0xF0..=0xF4 => Some((4, u32::from(b & 0x07))),
        0xF6 | 0xF7 => Some((4, u32::from(b & 0x07))),
        0xF5 | 0xF8..=0xFB => Some((5, u32::from(b & 0x03))),
        0xFC..=0xFD => Some((6, u32::from(b & 0x01))),
        _ => None,
    }
}

/// S-023 §4.1: length of the shortest extended form of `v` (values up to
/// `0x7FFFFFFF`; `> U+10FFFF` is still encoded, Q-6).
fn form_len(v: u32) -> usize {
    if v <= 0x7F {
        1
    } else if v <= 0x7FF {
        2
    } else if v <= 0xFFFF {
        3
    } else if v <= 0x1FFFFF {
        4
    } else if v <= 0x3FFFFFF {
        5
    } else {
        6
    }
}

/// Encode one value into `out` (capacity >= `form_len(v)`); returns n.
fn encode_value(v: u32, out: &mut [u8]) -> usize {
    match form_len(v) {
        1 => out[0] = v as u8,
        2 => {
            out[0] = 0xC0 | (v >> 6) as u8;
            out[1] = 0x80 | (v & 0x3F) as u8;
        }
        3 => {
            out[0] = 0xE0 | (v >> 12) as u8;
            out[1] = 0x80 | ((v >> 6) & 0x3F) as u8;
            out[2] = 0x80 | (v & 0x3F) as u8;
        }
        4 => {
            out[0] = 0xF0 | (v >> 18) as u8;
            out[1] = 0x80 | ((v >> 12) & 0x3F) as u8;
            out[2] = 0x80 | ((v >> 6) & 0x3F) as u8;
            out[3] = 0x80 | (v & 0x3F) as u8;
        }
        5 => {
            out[0] = 0xF8 | (v >> 24) as u8;
            out[1] = 0x80 | ((v >> 18) & 0x3F) as u8;
            out[2] = 0x80 | ((v >> 12) & 0x3F) as u8;
            out[3] = 0x80 | ((v >> 6) & 0x3F) as u8;
            out[4] = 0x80 | (v & 0x3F) as u8;
        }
        _ => {
            out[0] = 0xFC | (v >> 30) as u8;
            out[1] = 0x80 | ((v >> 24) & 0x3F) as u8;
            out[2] = 0x80 | ((v >> 18) & 0x3F) as u8;
            out[3] = 0x80 | ((v >> 12) & 0x3F) as u8;
            out[4] = 0x80 | ((v >> 6) & 0x3F) as u8;
            out[5] = 0x80 | (v & 0x3F) as u8;
        }
    }
    form_len(v)
}

/// S-023-R01..R12 (quirks Q-1..Q-5).
///
/// `insize`: `Some(n)` = exactly n bytes are examined (an embedded `00` is
/// an ordinary character, R02); `None` = up to the first `00` byte (not
/// converted). `out`: `None` = sizing mode (R12), `Some` = writing mode.
/// Returns the number of wchars produced (sizing: needed); `0` on error and
/// for empty input.
pub fn utf8_to_wchar(
    input: &[u8],
    insize: Option<usize>,
    flags: u32,
    mut out: Option<&mut [u32]>,
) -> usize {
    let mut lim = insize.unwrap_or(input.len()).min(input.len());
    if insize.is_none()
        && let Some(p) = input[..lim].iter().position(|&b| b == 0)
    {
        lim = p;
    }
    let ignore = flags & UTF8_IGNORE_ERROR != 0;
    let skip_bom = flags & UTF8_SKIP_BOM != 0;
    let mut written = 0usize;
    let mut i = 0usize;
    while i < lim {
        let b = input[i];
        // R03: forbidden lead bytes without the flag.
        if !ignore && (b == 0xC0 || b == 0xC1 || b == 0xF5 || b == 0xFF) {
            return 0;
        }
        let (n, high) = match lead_class(b) {
            Some(x) => x,
            // R04: no lead byte (`80..BF`, `FE`, `FF`): error, or skipped with the flag.
            None => {
                if ignore {
                    i += 1;
                    continue;
                }
                return 0;
            }
        };
        // R05: truncation — explicit-length mode only; in terminator mode the
        // missing byte is caught by R06 (the `00` / end of input is not a
        // continuation byte).
        if insize.is_some() && lim - i < n {
            if ignore {
                i += 1;
                continue;
            }
            return 0;
        }
        // R06: the n-1 following bytes must all be `10xxxxxx`.
        if !(1..n).all(|k| i + k < input.len() && input[i + k] & 0xC0 == 0x80) {
            if ignore {
                i += 1;
                continue;
            }
            return 0;
        }
        // R07: value — no overlong check, no upper bound.
        let mut value = high;
        for k in 1..n {
            value = (value << 6) | u32::from(input[i + k] & 0x3F);
        }
        match out {
            Some(ref mut buf) => {
                // R10: every valid sequence that reaches the value stage first
                // needs a free slot — even if it is about to be dropped (Q-5).
                if written == buf.len() {
                    return 0;
                }
                if is_surrogate(value) {
                    if !ignore {
                        return 0; // R07
                    }
                    i += n;
                    continue; // dropped: not counted, not written
                }
                if value == BOM && skip_bom {
                    i += n;
                    continue; // R08
                }
                buf[written] = value;
                written += 1;
                i += n;
            }
            // R12: sizing mode — the value-stage checks do not apply;
            // surrogates and U+FEFF are counted (Q-4).
            None => {
                written += 1;
                i += n;
            }
        }
    }
    written
}

/// S-023-R13..R17 (quirk Q-6).
///
/// `insize`: `Some(n)` = exactly n wchars (a `0` is an ordinary character,
/// R14); `None` = up to the first `0` element (not encoded). `out`: `None` =
/// sizing mode (same rules except capacity, R13), `Some` = writing mode.
/// Returns the number of bytes produced; `0` on error and for empty / fully
/// skipped input.
pub fn wchar_to_utf8(
    input: &[u32],
    insize: Option<usize>,
    flags: u32,
    mut out: Option<&mut [u8]>,
) -> usize {
    let mut lim = insize.unwrap_or(input.len()).min(input.len());
    if insize.is_none()
        && let Some(p) = input[..lim].iter().position(|&v| v == 0)
    {
        lim = p;
    }
    let ignore = flags & UTF8_IGNORE_ERROR != 0;
    let skip_bom = flags & UTF8_SKIP_BOM != 0;
    let mut written = 0usize;
    for &v in &input[..lim] {
        // R15: check order — (a) surrogate, (b) BOM, (c) negative
        // (unreachable via the u32 API; the C shim checks the sign first).
        if is_surrogate(v) {
            if !ignore {
                return 0;
            }
            continue;
        }
        if v == BOM && skip_bom {
            continue;
        }
        let n = form_len(v);
        match out {
            Some(ref mut buf) => {
                // R16: an element of n bytes needs all n free bytes; no
                // partial result.
                if written + n > buf.len() {
                    return 0;
                }
                encode_value(v, &mut buf[written..written + n]);
                written += n;
            }
            None => written += n,
        }
    }
    written
}

/// S-023-R18 (`os_utf8_to_wcs`): conversion with termination.
///
/// `len`: `Some(n) > 0` = exactly n bytes; `None` or `Some(0)` = up to the
/// first `00` byte. `dst`: `None` = sizing (the `utf8_to_wchar` size with
/// flags 0, so `0` on invalid input); `Some` = writing into the buffer with
/// a NUL terminator. The converter gets `dst_size` characters (not
/// `dst_size - 1`); D-1: `dst_size <= needed` is a failure (empty string),
/// never the reference's overflow.
pub fn os_utf8_to_wcs(input: &[u8], len: Option<usize>, dst: Option<&mut [u32]>) -> usize {
    let in_len = match len {
        None | Some(0) => None,
        Some(n) => Some(n),
    };
    match dst {
        None => utf8_to_wchar(input, in_len, 0, None),
        Some(dst) => {
            if dst.is_empty() {
                return 0; // R18: dst_size == 0
            }
            if dst.len() == 1 {
                dst[0] = 0;
                return 0; // R18: dst_size == 1
            }
            let needed = utf8_to_wchar(input, in_len, 0, None);
            if needed == 0 || needed >= dst.len() {
                dst[0] = 0; // invalid input (R18) / D-1 fix
                return 0;
            }
            let n = utf8_to_wchar(input, in_len, 0, Some(dst));
            dst[n] = 0;
            n
        }
    }
}

/// S-023-R19 (`os_wcs_to_utf8`): mirror of R18. The converter gets
/// `dst_size - 1` bytes (no D-1 here); a result needing `dst_size` or more
/// bytes yields the empty string and 0.
pub fn os_wcs_to_utf8(input: &[u32], len: Option<usize>, dst: Option<&mut [u8]>) -> usize {
    let in_len = match len {
        None | Some(0) => None,
        Some(n) => Some(n),
    };
    match dst {
        None => wchar_to_utf8(input, in_len, 0, None),
        Some(dst) => {
            if dst.is_empty() {
                return 0; // dst_size == 0
            }
            if dst.len() == 1 {
                dst[0] = 0;
                return 0; // dst_size == 1
            }
            let cap = dst.len() - 1;
            let n = wchar_to_utf8(input, in_len, 0, Some(&mut dst[..cap]));
            dst[n] = 0;
            n
        }
    }
}

/// S-023-R20 (`os_utf8_to_wcs_ptr`): owned NUL-terminated result. Invalid
/// input yields the empty string (a single NUL), never NULL. The buffer
/// holds exactly the converted units plus the terminating `0`.
pub fn os_utf8_to_wcs_ptr(input: &[u8], len: Option<usize>) -> Vec<u32> {
    let in_len = match len {
        None | Some(0) => None,
        Some(n) => Some(n),
    };
    let size = utf8_to_wchar(input, in_len, 0, None);
    let mut buf = vec![0u32; size + 1];
    let n = if size > 0 {
        utf8_to_wchar(input, in_len, 0, Some(&mut buf[..size]))
    } else {
        0
    };
    buf.truncate(n + 1);
    buf
}

/// S-023-R20 (`os_wcs_to_utf8_ptr`): owned NUL-terminated result. See
/// [`os_utf8_to_wcs_ptr`].
pub fn os_wcs_to_utf8_ptr(input: &[u32], len: Option<usize>) -> Vec<u8> {
    let in_len = match len {
        None | Some(0) => None,
        Some(n) => Some(n),
    };
    let size = wchar_to_utf8(input, in_len, 0, None);
    let mut buf = vec![0u8; size + 1];
    let n = if size > 0 {
        wchar_to_utf8(input, in_len, 0, Some(&mut buf[..size]))
    } else {
        0
    };
    buf.truncate(n + 1);
    buf
}

#[cfg(test)]
mod tests {
    use super::*;

    fn dec(input: &[u8], insize: Option<usize>, flags: u32, out: &mut [u32]) -> usize {
        utf8_to_wchar(input, insize, flags, Some(out))
    }
    fn dec_size(input: &[u8], insize: Option<usize>, flags: u32) -> usize {
        utf8_to_wchar(input, insize, flags, None)
    }
    fn enc(input: &[u32], insize: Option<usize>, flags: u32, out: &mut [u8]) -> usize {
        wchar_to_utf8(input, insize, flags, Some(out))
    }
    fn enc_size(input: &[u32], insize: Option<usize>, flags: u32) -> usize {
        wchar_to_utf8(input, insize, flags, None)
    }

    // --- decode, valid forms (R02, R07, R11, Q-1) ---

    #[test]
    fn d_ascii_and_terminator() {
        let mut out = [99u32; 8];
        assert_eq!(dec(b"hello", Some(5), 0, &mut out), 5);
        assert_eq!(&out[..5], &[104, 101, 108, 108, 111]);
        // R02: terminator mode stops at the first 00 (not counted).
        assert_eq!(dec(b"hi\0B", None, 0, &mut out), 2);
        assert_eq!(&out[..2], &[0x68, 0x69]);
        // R02: explicit length — embedded 00 is an ordinary character.
        assert_eq!(dec(b"\0A", Some(2), 0, &mut out), 2);
        assert_eq!(&out[..2], &[0, 0x41]);
        // R02: empty input.
        assert_eq!(dec(b"", None, 0, &mut out), 0);
    }

    #[test]
    fn d_worked_examples_from_spec() {
        let mut out = [0u32; 8];
        assert_eq!(dec(b"\xC3\xA9", Some(2), 0, &mut out), 1);
        assert_eq!(out[0], 0xE9);
        assert_eq!(dec(b"\xE2\x82\xAC", Some(3), 0, &mut out), 1);
        assert_eq!(out[0], 0x20AC);
        assert_eq!(dec(b"\xF0\x9F\x98\x80", Some(4), 0, &mut out), 1);
        assert_eq!(out[0], 0x1F600);
        assert_eq!(dec(b"\xF4\x8F\xBF\xBF", Some(4), 0, &mut out), 1);
        assert_eq!(out[0], 0x10FFFF);
    }

    #[test]
    fn d_q1_lenient_forms() {
        let mut out = [0u32; 8];
        // Values above U+10FFFF (Q-1).
        assert_eq!(dec(b"\xF4\x90\x80\x80", Some(4), 0, &mut out), 1);
        assert_eq!(out[0], 0x110000);
        assert_eq!(dec(b"\xFD\xBF\xBF\xBF\xBF\xBF", Some(6), 0, &mut out), 1);
        assert_eq!(out[0], 0x7FFFFFFF);
        // Overlong forms (Q-1).
        assert_eq!(dec(b"\xE0\x80\x80", Some(3), 0, &mut out), 1);
        assert_eq!(out[0], 0);
        assert_eq!(dec(b"\xF0\x80\x80\xAF", Some(4), 0, &mut out), 1);
        assert_eq!(out[0], 0x2F);
        // 5- and 6-byte forms.
        assert_eq!(dec(b"\xF8\x88\x80\x80\x80", Some(5), 0, &mut out), 1);
        assert_eq!(out[0], 0x200000);
    }

    #[test]
    fn d_roundtrip_all_forms() {
        let values: [u32; 13] = [
            0, 1, 0x7F, 0x80, 0x7FF, 0x800, 0xFFFF, 0x10000, 0x10FFFF, 0x110000, 0x3FFFFFF,
            0x40000000, 0x7FFFFFFF,
        ];
        let mut bytes = [0u8; 64];
        let n = enc(&values, Some(values.len()), 0, &mut bytes);
        assert_eq!(n, values.iter().map(|&v| form_len(v)).sum());
        let mut back = [0u32; 16];
        assert_eq!(dec(&bytes[..n], Some(n), 0, &mut back), values.len());
        assert_eq!(&back[..values.len()], &values[..]);
    }

    // --- decode, invalid (R03, R04, R05, R06, R07, R09) ---

    #[test]
    fn d_forbidden_leads() {
        let mut out = [0u32; 8];
        assert_eq!(dec(b"\xC0\x80", Some(2), 0, &mut out), 0); // R03
        assert_eq!(dec(b"\xC1\x80", Some(2), 0, &mut out), 0);
        assert_eq!(dec(b"\xF5\x80\x80\x80\x80", Some(5), 0, &mut out), 0);
        assert_eq!(dec(b"\xFF", Some(1), 0, &mut out), 0);
        assert_eq!(dec(b"A\xFF", None, 0, &mut out), 0); // FF after valid text
    }

    #[test]
    fn d_no_lead_bytes() {
        let mut out = [0u32; 8];
        assert_eq!(dec(b"A\x80B", Some(3), 0, &mut out), 0); // R04 stray continuation
        assert_eq!(dec(b"\xFE", Some(1), 0, &mut out), 0); // R04
    }

    #[test]
    fn d_truncation() {
        let mut out = [0u32; 8];
        // R05: explicit-length mode.
        assert_eq!(dec(b"\xC3", Some(1), 0, &mut out), 0);
        assert_eq!(dec(b"\xE2\x82", Some(2), 0, &mut out), 0);
        // R05/R06: terminator mode — caught by R06 (the 00 is not a
        // continuation byte).
        assert_eq!(dec(b"\xE2\x82\0", None, 0, &mut out), 0);
        // Terminator mode, sequence runs into the end of the slice.
        assert_eq!(dec(b"\xE2\x82", None, 0, &mut out), 0);
    }

    #[test]
    fn d_bad_continuations() {
        let mut out = [0u32; 8];
        assert_eq!(dec(b"\xC3\x28", Some(2), 0, &mut out), 0); // R06
        assert_eq!(dec(b"\xE2\x82\x22", Some(3), 0, &mut out), 0); // R06 (second byte)
    }

    #[test]
    fn d_surrogates_and_error_returns() {
        let mut out = [99u32; 8];
        assert_eq!(dec(b"\xED\xA0\x80", Some(3), 0, &mut out), 0); // R07 surrogate
        assert_eq!(dec(b"\xED\xAF\xBF", Some(3), 0, &mut out), 0); // R07 high surrogate
        assert_eq!(dec(b"hi\xED\xA0\x80", Some(5), 0, &mut out), 0); // R09: first error ends the call
    }

    // --- decode flags (R03, R04, R06, R07, R08, R09; Q-2, Q-3) ---

    #[test]
    fn d_ignore_error() {
        let mut out = [0u32; 8];
        // R04: stray bytes skipped.
        assert_eq!(dec(b"A\x80B", Some(3), UTF8_IGNORE_ERROR, &mut out), 2);
        assert_eq!(&out[..2], &[0x41, 0x42]);
        // Q-2: C0/C1/F5 become ordinary lead bytes.
        assert_eq!(
            dec(b"\x61\xC0\x80", Some(3), UTF8_IGNORE_ERROR, &mut out),
            2
        );
        assert_eq!(&out[..2], &[0x61, 0]);
        assert_eq!(dec(b"\xC1\x80", Some(2), UTF8_IGNORE_ERROR, &mut out), 1);
        assert_eq!(out[0], 0x40); // C1 & 0x1F = 1, followed by 0
        assert_eq!(
            dec(
                b"\xF5\x80\x80\x80\x80",
                Some(5),
                UTF8_IGNORE_ERROR,
                &mut out
            ),
            1
        );
        assert_eq!(out[0], 0x1000000); // F5 & 0x03 = 1, 24 zero bits
        // R03: FF falls under R04 (skipped) with the flag.
        assert_eq!(dec(b"A\xFFB", Some(3), UTF8_IGNORE_ERROR, &mut out), 2);
        assert_eq!(&out[..2], &[0x41, 0x42]);
        // Q-3: after a bad sequence only one byte is skipped; the
        // continuation byte is looked at again and dropped as stray.
        assert_eq!(
            dec(b"\xE2\x82\x22", Some(3), UTF8_IGNORE_ERROR, &mut out),
            1
        );
        assert_eq!(out[0], 0x22);
        // R07: surrogate dropped with the flag.
        assert_eq!(
            dec(b"\xED\xA0\x80A", Some(4), UTF8_IGNORE_ERROR, &mut out),
            1
        );
        assert_eq!(out[0], 0x41);
        // R09: nothing left -> 0.
        assert_eq!(dec(b"\x80\xC3", Some(2), UTF8_IGNORE_ERROR, &mut out), 0);
    }

    #[test]
    fn d_bom_handling() {
        let mut out = [0u32; 8];
        // R08: ordinary character without the flag.
        assert_eq!(dec(b"\xEF\xBB\xBFA", Some(4), 0, &mut out), 2);
        assert_eq!(&out[..2], &[0xFEFF, 0x41]);
        // R08: dropped with UTF8_SKIP_BOM.
        assert_eq!(dec(b"\xEF\xBB\xBFA", Some(4), UTF8_SKIP_BOM, &mut out), 1);
        assert_eq!(out[0], 0x41);
        assert_eq!(dec(b"\xEF\xBB\xBF", Some(3), UTF8_SKIP_BOM, &mut out), 0);
        // Both flags together.
        assert_eq!(
            dec(
                b"\xEF\xBB\xBF\xC0\x80A",
                Some(6),
                UTF8_IGNORE_ERROR | UTF8_SKIP_BOM,
                &mut out
            ),
            2
        );
        assert_eq!(&out[..2], &[0, 0x41]);
    }

    // --- output capacity and sizing mode (R01, R10, R12; Q-4, Q-5) ---

    #[test]
    fn w_capacity() {
        let mut out = [0u32; 8];
        assert_eq!(dec(b"A", Some(1), 0, &mut out[..0]), 0); // R01: outsize 0
        assert_eq!(dec(b"AB", Some(2), 0, &mut out[..2]), 2); // exact fit
        assert_eq!(dec(b"AB", Some(2), 0, &mut out[..1]), 0); // R10
    }

    #[test]
    fn q5_dropped_character_still_needs_slot() {
        let mut out = [0u32; 8];
        // Q-5: the dropped surrogate still needs a free slot when reached.
        assert_eq!(
            dec(
                b"\x61\xED\xA0\x80",
                Some(4),
                UTF8_IGNORE_ERROR,
                &mut out[..1]
            ),
            0
        );
        assert_eq!(
            dec(
                b"\x61\xED\xA0\x80",
                Some(4),
                UTF8_IGNORE_ERROR,
                &mut out[..2]
            ),
            1
        );
        assert_eq!(out[0], 0x61);
        // R08: a skipped BOM needs a slot too.
        assert_eq!(
            dec(b"\xEF\xBB\xBF", Some(3), UTF8_SKIP_BOM, &mut out[..0]),
            0
        );
    }

    #[test]
    fn s_sizing_mode() {
        // Q-4: surrogates and BOM are counted in sizing mode.
        assert_eq!(dec_size(b"\xED\xA0\x80", Some(3), 0), 1);
        assert_eq!(dec_size(b"\xEF\xBB\xBF", Some(3), 0), 1);
        // R12: sizing can succeed where writing returns 0 (strict surrogate).
        assert_eq!(dec_size(b"hi\xED\xA0\x80", Some(5), 0), 3);
        let mut out = [0u32; 8];
        assert_eq!(dec(b"hi\xED\xA0\x80", Some(5), 0, &mut out), 0);
        // R12: the sizing result can be larger than the writing result.
        assert_eq!(dec_size(b"hi\xED\xA0\x80", Some(5), UTF8_IGNORE_ERROR), 3);
        assert_eq!(
            dec(b"hi\xED\xA0\x80", Some(5), UTF8_IGNORE_ERROR, &mut out),
            2
        );
        // R02: sizing honours the terminator.
        assert_eq!(dec_size(b"hi\0B", None, 0), 2);
    }

    // --- encode (R13, R14, R15, R16, R17; Q-6) ---

    #[test]
    fn e_worked_examples_from_spec() {
        let mut out = [0u8; 16];
        assert_eq!(enc(&[0x41, 0x7F], Some(2), 0, &mut out), 2);
        assert_eq!(&out[..2], b"\x41\x7F");
        assert_eq!(enc(&[0xE9], Some(1), 0, &mut out), 2);
        assert_eq!(&out[..2], b"\xC3\xA9");
        assert_eq!(enc(&[0x20AC], Some(1), 0, &mut out), 3);
        assert_eq!(&out[..3], b"\xE2\x82\xAC");
        assert_eq!(enc(&[0x1F600], Some(1), 0, &mut out), 4);
        assert_eq!(&out[..4], b"\xF0\x9F\x98\x80");
        assert_eq!(enc(&[0x10FFFF], Some(1), 0, &mut out), 4);
        assert_eq!(&out[..4], b"\xF4\x8F\xBF\xBF");
    }

    #[test]
    fn e_q6_above_10ffff() {
        let mut out = [0u8; 16];
        // Q-6: values above U+10FFFF are encoded, not failed.
        assert_eq!(enc(&[0x110000], Some(1), 0, &mut out), 4);
        assert_eq!(&out[..4], b"\xF4\x90\x80\x80");
        assert_eq!(enc(&[0x200000], Some(1), 0, &mut out), 5);
        assert_eq!(&out[..5], b"\xF8\x88\x80\x80\x80");
        assert_eq!(enc(&[0x3FFFFFF], Some(1), 0, &mut out), 5);
        assert_eq!(&out[..5], b"\xFB\xBF\xBF\xBF\xBF"); // 5-byte boundary
        assert_eq!(enc(&[0x40000000], Some(1), 0, &mut out), 6);
        assert_eq!(&out[..6], b"\xFD\x80\x80\x80\x80\x80"); // 6-byte start
        assert_eq!(enc(&[0x7FFFFFFF], Some(1), 0, &mut out), 6);
        assert_eq!(&out[..6], b"\xFD\xBF\xBF\xBF\xBF\xBF");
    }

    #[test]
    fn e_errors_and_flags() {
        let mut out = [0u8; 16];
        assert_eq!(enc(&[0xD800], Some(1), 0, &mut out), 0); // R15a surrogate
        assert_eq!(enc(&[0xDFFF], Some(1), 0, &mut out), 0); // R15a high surrogate
        assert_eq!(enc(&[0xD800], Some(1), UTF8_SKIP_BOM, &mut out), 0); // R15: (a) before (b)
        assert_eq!(
            enc(&[0xD800, 0x41], Some(2), UTF8_IGNORE_ERROR, &mut out),
            1
        );
        assert_eq!(out[0], 0x41);
        assert_eq!(enc(&[0xFEFF, 0x41], Some(2), UTF8_SKIP_BOM, &mut out), 1); // R15b
        assert_eq!(out[0], 0x41);
        assert_eq!(enc(&[0xFEFF], Some(1), UTF8_SKIP_BOM, &mut out), 0); // R17 fully skipped
    }

    #[test]
    fn e_length_modes() {
        let mut out = [0u8; 16];
        // R14: explicit length — a 0 is an ordinary character, encoded as 00.
        assert_eq!(enc(&[0x41, 0, 0x42], Some(3), 0, &mut out), 3);
        assert_eq!(&out[..3], b"\x41\x00\x42");
        // R14: terminator mode — up to the first 0 (not encoded).
        assert_eq!(enc(&[0x41, 0, 0x42], None, 0, &mut out), 1);
        assert_eq!(out[0], 0x41);
        // R17: empty input.
        assert_eq!(enc(&[], Some(0), 0, &mut out), 0);
    }

    #[test]
    fn e_capacity_and_sizing() {
        let mut out = [0u8; 16];
        assert_eq!(enc(&[0xFFFF], Some(1), 0, &mut out[..2]), 0); // R16: no partial result
        assert_eq!(enc(&[0xFFFF], Some(1), 0, &mut out[..3]), 3); // R16 exact fit (3 bytes)
        assert_eq!(enc(&[0x10000], Some(1), 0, &mut out[..3]), 0); // 17 bits -> 4 bytes
        assert_eq!(enc(&[0x10000], Some(1), 0, &mut out[..4]), 4);
        assert_eq!(enc(&[0x41], Some(1), 0, &mut out[..0]), 0); // R13: outsize 0
        assert_eq!(enc_size(&[0x41, 0x10000], Some(2), 0), 5); // R13 sizing (1 + 4)
        assert_eq!(enc_size(&[0xD800], Some(1), 0), 0); // R13: sizing applies the rules
    }

    // --- conversions with termination (R18, R19, R20; D-1) ---

    #[test]
    fn o2w_os_utf8_to_wcs() {
        let mut dst = [99u32; 8];
        // Basic writing + NUL terminator.
        assert_eq!(os_utf8_to_wcs(b"AB", None, Some(&mut dst)), 2);
        assert_eq!(&dst[..3], &[0x41, 0x42, 0]);
        // R18: len > 0 converts embedded 00.
        assert_eq!(os_utf8_to_wcs(b"\0A", Some(2), Some(&mut dst)), 2);
        assert_eq!(&dst[..3], &[0, 0x41, 0]);
        // R18: len == 0 (and None) = terminator semantics.
        assert_eq!(os_utf8_to_wcs(b"\0A", Some(0), Some(&mut dst)), 0);
        assert_eq!(dst[0], 0);
        assert_eq!(os_utf8_to_wcs(b"\0A", None, Some(&mut dst)), 0);
        assert_eq!(dst[0], 0);
        // R18: invalid input yields the empty string and 0.
        assert_eq!(os_utf8_to_wcs(b"\xED\xA0\x80", None, Some(&mut dst)), 0);
        assert_eq!(dst[0], 0);
        // R18: dst_size == 0 and == 1.
        assert_eq!(os_utf8_to_wcs(b"A", None, Some(&mut dst[..0])), 0);
        assert_eq!(os_utf8_to_wcs(b"A", None, Some(&mut dst[..1])), 0);
        assert_eq!(dst[0], 0);
        // R18/D-1: text of exactly dst_size characters overflows the
        // reference (D-1) — here a failure with the empty string.
        assert_eq!(os_utf8_to_wcs(b"AB", None, Some(&mut dst[..3])), 2);
        assert_eq!(&dst[..3], &[0x41, 0x42, 0]);
        assert_eq!(os_utf8_to_wcs(b"ABC", None, Some(&mut dst[..3])), 0);
        assert_eq!(dst[0], 0);
        assert_eq!(os_utf8_to_wcs(b"ABCD", None, Some(&mut dst[..3])), 0);
        assert_eq!(dst[0], 0);
        // R18: sizing form.
        assert_eq!(os_utf8_to_wcs(b"ABC", None, None), 3);
        assert_eq!(os_utf8_to_wcs(b"\xED\xA0\x80", None, None), 1); // Q-4: counted
    }

    #[test]
    fn o2u_os_wcs_to_utf8() {
        let mut dst = [99u8; 8];
        // Basic writing + NUL terminator.
        assert_eq!(os_wcs_to_utf8(&[0x41, 0x10000], None, Some(&mut dst)), 5);
        assert_eq!(&dst[..6], b"A\xF0\x90\x80\x80\0");
        // R19: the converter gets dst_size - 1 bytes; needs dst_size or more
        // -> empty string and 0.
        assert_eq!(os_wcs_to_utf8(&[0x10000], None, Some(&mut dst[..3])), 0);
        assert_eq!(dst[0], 0);
        assert_eq!(os_wcs_to_utf8(&[0x10000], None, Some(&mut dst[..4])), 0);
        assert_eq!(dst[0], 0);
        assert_eq!(os_wcs_to_utf8(&[0x10000], None, Some(&mut dst[..5])), 4);
        assert_eq!(&dst[..5], b"\xF0\x90\x80\x80\0");
        // R19: invalid input.
        assert_eq!(os_wcs_to_utf8(&[0xD800], None, Some(&mut dst)), 0);
        assert_eq!(dst[0], 0);
        // R19: dst_size == 0 and == 1.
        assert_eq!(os_wcs_to_utf8(&[0x41], None, Some(&mut dst[..0])), 0);
        assert_eq!(os_wcs_to_utf8(&[0x41], None, Some(&mut dst[..1])), 0);
        assert_eq!(dst[0], 0);
        // R19: explicit length — a 0 element is an ordinary character.
        assert_eq!(os_wcs_to_utf8(&[0x41, 0, 0x42], Some(3), Some(&mut dst)), 3);
        assert_eq!(&dst[..4], b"A\0B\0");
        // R19: terminator mode.
        assert_eq!(os_wcs_to_utf8(&[0x41, 0, 0x42], None, Some(&mut dst)), 1);
        assert_eq!(&dst[..2], b"A\0");
        // R19: sizing form.
        assert_eq!(os_wcs_to_utf8(&[0x41, 0x10000], None, None), 5);
        assert_eq!(os_wcs_to_utf8(&[0xD800], None, None), 0);
    }

    #[test]
    fn p_ptr_forms() {
        // R20: exactly the converted units + the terminating 0.
        assert_eq!(
            os_utf8_to_wcs_ptr(b"hi\xC3\xA9", None),
            vec![0x68, 0x69, 0xE9, 0]
        );
        assert_eq!(
            os_wcs_to_utf8_ptr(&[0x41, 0x10FFFF], None),
            vec![0x41, 0xF4, 0x8F, 0xBF, 0xBF, 0]
        );
        // R20: invalid input -> the empty string, not NULL.
        assert_eq!(os_utf8_to_wcs_ptr(b"\xED\xA0\x80", None), vec![0]);
        assert_eq!(os_wcs_to_utf8_ptr(&[0xD800], None), vec![0]);
        // R20: empty input -> the empty string.
        assert_eq!(os_utf8_to_wcs_ptr(b"", None), vec![0]);
        assert_eq!(os_wcs_to_utf8_ptr(&[], None), vec![0]);
        // R20: len selects the input bytes only.
        assert_eq!(os_utf8_to_wcs_ptr(b"\0A", Some(2)), vec![0, 0x41, 0]);
        assert_eq!(os_utf8_to_wcs_ptr(b"\0A", Some(0)), vec![0]);
        // Roundtrip through the ptr forms.
        let src: Vec<u32> = vec![0, 0x41, 0x20AC, 0x10FFFF, 0x110000, 0x7FFFFFFF];
        let bytes = os_wcs_to_utf8_ptr(&src, Some(src.len()));
        let back = os_utf8_to_wcs_ptr(&bytes[..bytes.len() - 1], Some(bytes.len() - 1));
        assert_eq!(&back[..back.len() - 1], &src[..]); // minus the terminator
    }
}
