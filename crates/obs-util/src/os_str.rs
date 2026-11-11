//! Platform number helpers (S-023 R40/R41, used by S-007 config).
//!
//! Per S-007 §8 these belong in the platform crate once it lands
//! (P1-13/P1-14); until then they live here and `config` reuses them.
//!
//! `os_dtostr` implements the **required** (D-2-fixed) behaviour of
//! S-023-R40: the text and its length are consistent. The C reference
//! leaves stale tail bytes for exponent values (S-023 D-2, S-007 R08
//! open question 2 — the port takes the "fix it" branch per P1-09.3
//! "the port MUST make text and return value consistent"); the
//! corrupted reference texts are pinned in the tests as documentation.
//!
//! Rust has no `%g` format trait, so `g17` reproduces C `%.17g`
//! (glibc, C locale) digit for digit — verified against the C
//! implementation in this WP's probe.

/// C `%.17g` of `value` (glibc, C locale): 17 significant digits,
/// shortest of fixed/exponential per `%g`, trailing zeros stripped.
/// `inf`/`-inf`/`nan` as printed by `printf`.
fn g17(v: f64) -> String {
    if v.is_nan() {
        // glibc prints NaNs without a sign
        return "nan".into();
    }
    if v.is_infinite() {
        return if v > 0.0 { "inf".into() } else { "-inf".into() };
    }
    let s = format!("{v:.16e}"); // 17 significant digits, always e-form
    let (mant, exp) = s.split_once('e').unwrap();
    let exp: i32 = exp.parse().unwrap();
    let neg = mant.starts_with('-');
    let mant = mant.strip_prefix('-').unwrap_or(mant);
    let (int_part, frac_part) = mant.split_once('.').unwrap();
    let digits = format!("{int_part}{frac_part}"); // exactly 17
    let sign = if neg { "-" } else { "" };
    if !(-4..17).contains(&exp) {
        // e-form: C prints the exponent with a sign, at least 2 digits
        let exp_str = if exp >= 0 {
            format!("e+{exp:02}")
        } else {
            format!("e-{:-02}", -exp)
        };
        let frac = frac_part.trim_end_matches('0');
        let mantissa = if frac.is_empty() {
            int_part.to_string()
        } else {
            format!("{int_part}.{frac}")
        };
        format!("{sign}{mantissa}{exp_str}")
    } else {
        // f-form: 17 significant digits, trailing zeros stripped
        let mut out = String::new();
        if exp >= 0 {
            let (ip, fp) = digits.split_at((exp + 1) as usize);
            out.push_str(ip);
            let fp = fp.trim_end_matches('0');
            if !fp.is_empty() {
                out.push('.');
                out.push_str(fp);
            }
        } else {
            out.push_str("0.");
            for _ in 0..(-exp - 1) {
                out.push('0');
            }
            out.push_str(digits.trim_end_matches('0'));
        }
        format!("{sign}{out}")
    }
}

/// S-023-R40 (D-2 fixed): `os_dtostr(value)` — the stored text.
///
/// `%.17g`, then `.0` appended when the text has neither `.` nor `e`
/// (so `inf`/`nan` become `inf.0`/`nan.0`, Q-7), then the exponent
/// shortening: remove a `+` after `e` and leading zeros of the exponent
/// digits (`e+21` -> `e21`, `e-05` -> `e-5`), keeping `-`. The text is
/// correctly terminated after its final character (D-2 fixed).
#[must_use]
pub fn os_dtostr(value: f64) -> String {
    let mut text = g17(value);
    if !text.contains(['.', 'e']) {
        text.push_str(".0");
    }
    if let Some(pos) = text.find('e') {
        let rest = &text[pos + 1..];
        let (sign, digits) = if let Some(d) = rest.strip_prefix('+') {
            ("", d)
        } else if let Some(d) = rest.strip_prefix('-') {
            ("-", d)
        } else {
            ("", rest)
        };
        // a %.17g exponent never has fewer than two digits and never
        // consists only of zeros, so this is never empty (S-023 §6)
        let suffix = format!("{sign}{}", digits.trim_start_matches('0'));
        text.truncate(pos + 1);
        text.push_str(&suffix);
    }
    text
}

/// C `strtod` of a hex float body (after the `0x`/`0X` prefix):
/// `[hex] ['.' [hex]] [p/P [sign] digits]`, trailing garbage ignored,
/// no p-exponent means p0, no conversion -> 0.0. Rust `FromStr` has
/// no hex floats, so the conversion is done by hand (the spec-pinned
/// mantissas are short; >16-digit mantissas are truncated left-to-
/// right, where C would round — not reachable from config values).
fn parse_hex_float(body: &str) -> f64 {
    let bytes = body.as_bytes();
    let mut i = 0usize;
    let mut seen_digit = false;
    while i < bytes.len() && bytes[i].is_ascii_hexdigit() {
        seen_digit = true;
        i += 1;
    }
    let int_part = &body[..i];
    let (frac_part, j) = if i < bytes.len() && bytes[i] == b'.' {
        i += 1;
        let f0 = i;
        while i < bytes.len() && bytes[i].is_ascii_hexdigit() {
            seen_digit = true;
            i += 1;
        }
        (&body[f0..i], i)
    } else {
        ("", i)
    };
    if !seen_digit {
        return 0.0;
    }
    let p_exp: Option<i64> = if j < bytes.len() && matches!(bytes[j], b'p' | b'P') {
        let mut e = j + 1;
        let neg = if e < bytes.len() && matches!(bytes[e], b'+' | b'-') {
            let n = bytes[e] == b'-';
            e += 1;
            n
        } else {
            false
        };
        let d0 = e;
        while e < bytes.len() && bytes[e].is_ascii_hexdigit() {
            e += 1;
        }
        if e == d0 {
            None // a `p` without digits is not an exponent (C)
        } else {
            let v: i64 = body[d0..e].parse().unwrap_or(i64::MAX);
            Some(if neg { -v } else { v })
        }
    } else {
        None
    };
    hex_float_to_f64(int_part, frac_part, p_exp.unwrap_or(0))
}

/// `value = (int + frac/16^b) x 16^(dropped) x 2^p` from a hex
/// mantissa (see `parse_hex_float` for the scan). Powers of two go
/// through `pow2` (exact bit construction) because `f64::powi` is
/// computed inexact under Miri; the exponent is clamped into the
/// saturating range so overflow/underflow match C.
fn hex_float_to_f64(int_part: &str, frac_part: &str, p: i64) -> f64 {
    let total = int_part.len() as i64 + frac_part.len() as i64;
    let (int_digits, frac_digits, shift): (String, String, i64) = if total > 16 {
        let shift = total - 16;
        if int_part.len() >= 16 {
            (int_part[..16].to_string(), String::new(), shift)
        } else {
            let need = 16 - int_part.len();
            (int_part.to_string(), frac_part[..need].to_string(), shift)
        }
    } else {
        (int_part.to_string(), frac_part.to_string(), 0)
    };
    let int_val = u64::from_str_radix(&int_digits, 16).unwrap_or(0) as f64;
    let frac_val = if frac_digits.is_empty() {
        0.0
    } else {
        let v = u64::from_str_radix(&frac_digits, 16).unwrap_or(0) as f64;
        v * pow2(-(frac_digits.len() as i64 * 4))
    };
    (int_val + frac_val) * pow2(4 * shift + p)
}

/// Exact saturating `2^e`: a power of two is representable exactly, so
/// the bit pattern is built directly (Miri computes `f64::powi`
/// inexact; this keeps the hex-float conversion deterministic).
fn pow2(e: i64) -> f64 {
    if e > 1023 {
        f64::INFINITY
    } else if e < -1074 {
        0.0
    } else if e >= -1022 {
        f64::from_bits(((e + 1023) as u64) << 52)
    } else {
        f64::from_bits(1u64 << ((1074 + e) as u32))
    }
}

/// S-023-R41: `os_strtod(text)` — at most 63 characters considered,
/// then C-locale `strtod` semantics: leading blanks skipped, optional
/// sign, decimal digits with optional `.` and exponent, hexadecimal
/// floats (`0x10` = 16, `0x1p4`, `0x.8`, both `0x`/`0X`),
/// `inf`/`infinity`/`nan` in any letter case, overflow -> ±inf,
/// underflow -> 0, trailing garbage ignored, no conversion
/// (`abc`, `""`, `.`, `e5`) -> 0.0. A `,` is not a decimal point.
#[must_use]
pub fn os_strtod(text: &str) -> f64 {
    let mut n = text.len().min(63);
    if n == 63 && text.len() > 63 {
        while !text.is_char_boundary(n) {
            n -= 1;
        }
    }
    let s = text.get(..n).unwrap_or("");
    let trimmed = s.trim_start();
    let sign = match trimmed.as_bytes().first() {
        Some(b'-') => -1.0,
        Some(b'+') => 1.0,
        _ => 1.0,
    };
    let rest = trimmed.strip_prefix(['-', '+']).unwrap_or(trimmed);
    let lower = rest.to_ascii_lowercase();
    if lower.starts_with("inf") {
        return sign * f64::INFINITY;
    }
    if lower.starts_with("nan") {
        return f64::NAN;
    }
    let bytes = rest.as_bytes();
    if bytes.len() >= 2 && bytes[0] == b'0' && matches!(bytes[1], b'x' | b'X') {
        return sign * parse_hex_float(&rest[2..]);
    }
    // decimal: [digits] ['.' [digits]] [e/E [sign] digits]
    let mut end = 0usize;
    let mut seen_digit = false;
    while end < bytes.len() && bytes[end].is_ascii_digit() {
        seen_digit = true;
        end += 1;
    }
    if end < bytes.len() && bytes[end] == b'.' {
        end += 1;
        while end < bytes.len() && bytes[end].is_ascii_digit() {
            seen_digit = true;
            end += 1;
        }
    }
    if end < bytes.len() && matches!(bytes[end], b'e' | b'E') {
        let mut e = end + 1;
        if e < bytes.len() && matches!(bytes[e], b'+' | b'-') {
            e += 1;
        }
        let mut exp_digits = false;
        while e < bytes.len() && bytes[e].is_ascii_digit() {
            exp_digits = true;
            e += 1;
        }
        if exp_digits {
            end = e;
        } // an exponent marker without digits is not consumed (C)
    }
    if !seen_digit || end == 0 {
        return 0.0;
    }
    sign * rest[..end].parse::<f64>().unwrap_or(0.0)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn r40_dtostr_worked_values() {
        // S-023 §4.3 worked examples (g17 output, pre-shortening where
        // noted), verified against glibc printf("%.17g")
        assert_eq!(g17(0.1), "0.10000000000000001");
        assert_eq!(g17(2.0), "2");
        assert_eq!(g17(-0.0), "-0");
        assert_eq!(g17(1e21), "1e+21");
        assert_eq!(g17(1.5e-7), "1.4999999999999999e-07");
        assert_eq!(g17(1.5e300), "1.5000000000000001e+300");
        assert_eq!(g17(0.00001), "1.0000000000000001e-05");
        assert_eq!(g17(1e16), "10000000000000000");
        assert_eq!(g17(1e17), "1e+17");
        assert_eq!(g17(0.0001), "0.0001");
        assert_eq!(g17(100.0), "100");
        assert_eq!(g17(1234567.891), "1234567.8910000001");
        assert_eq!(g17(7.5), "7.5");
        assert_eq!(g17(-2.0), "-2");
        assert_eq!(g17(f64::from_bits(1u64)), "4.9406564584124654e-324");
        assert_eq!(g17(f64::INFINITY), "inf");
        assert_eq!(g17(f64::NEG_INFINITY), "-inf");
        assert_eq!(g17(f64::NAN), "nan");

        // S-023 §4.3 / R40 final os_dtostr texts (D-2 fixed)
        assert_eq!(os_dtostr(0.1), "0.10000000000000001");
        assert_eq!(os_dtostr(2.0), "2.0");
        assert_eq!(os_dtostr(-0.0), "-0.0");
        assert_eq!(os_dtostr(1e21), "1e21");
        assert_eq!(os_dtostr(1.5e-7), "1.4999999999999999e-7");
        assert_eq!(os_dtostr(0.00001), "1.0000000000000001e-5");
        assert_eq!(os_dtostr(1.5e300), "1.5000000000000001e300");
        // `%.17g` of 1e16 is the fixed text `10000000000000000`
        // (no `.`, no `e`) -> the `.0` rule applies
        assert_eq!(os_dtostr(1e16), "10000000000000000.0");
        assert_eq!(os_dtostr(1e17), "1e17");
        assert_eq!(os_dtostr(0.0001), "0.0001");
        assert_eq!(os_dtostr(100.0), "100.0");
        assert_eq!(os_dtostr(-2.0), "-2.0");
        // Q-7: inf/nan get .0
        assert_eq!(os_dtostr(f64::INFINITY), "inf.0");
        assert_eq!(os_dtostr(f64::NEG_INFINITY), "-inf.0");
        assert_eq!(os_dtostr(f64::NAN), "nan.0");
    }

    #[test]
    fn r40_d2_reference_corruption_documented() {
        // The C reference (S-023 D-2, S-007 R08) leaves stale tail
        // bytes: the second column is what config files written by the
        // C build contain. The port writes the fixed text (P1-09.3),
        // which reads back to the value it was written from; the stale
        // C text reads back to a different (corrupted) value.
        for (v, stale) in [(1e21, "1e211"), (0.00001, "1.0000000000000001e-55")] {
            let fixed = os_dtostr(v);
            assert_ne!(fixed, stale, "port must not reproduce the D-2 stale tail");
            assert_eq!(os_strtod(&fixed), v, "fixed text round-trips");
            assert_ne!(os_strtod(stale), v, "stale C text reads back corrupted");
        }
    }

    #[test]
    fn r41_strtod_worked_values() {
        // verified against glibc strtod (C locale)
        let cases: [(&str, f64); 26] = [
            ("0x10", 16.0),
            ("0x1p4", 16.0),
            ("0x.8", 0.5),
            ("0x10z", 16.0),
            ("0x1p4x", 16.0),
            ("0x", 0.0),
            ("0X1F", 31.0),
            ("12abc", 12.0),
            ("abc", 0.0),
            ("", 0.0),
            (".", 0.0),
            ("e5", 0.0),
            ("  -12.5 ", -12.5),
            ("inf", f64::INFINITY),
            ("INF", f64::INFINITY),
            ("-infinity", f64::NEG_INFINITY),
            ("NaN", f64::NAN),
            ("1e400", f64::INFINITY),
            ("1e-400", 0.0),
            ("1.2.3", 1.2),
            ("1e", 1.0),
            ("1e+5", 100000.0),
            ("2,5", 2.0),
            (".8", 0.8),
            ("0x8.p2", 32.0),
            ("-0x1.8p1", -3.0),
        ];
        for (s, want) in cases {
            let got = os_strtod(s);
            if want.is_nan() {
                assert!(got.is_nan(), "{s:?} -> {got}");
            } else {
                assert_eq!(got, want, "{s:?}");
            }
        }
        assert_eq!(os_strtod("  "), 0.0);
        assert_eq!(os_strtod("1e999"), f64::INFINITY);
        assert_eq!(os_strtod("0x1p1024"), f64::INFINITY, "hex overflow");
        assert_eq!(os_strtod("0x.8p2x"), 2.0);
    }

    #[test]
    fn r41_63_char_truncation() {
        // a longer string is cut at 63 characters, the rest ignored
        let long = "1".repeat(71);
        let truncated: String = long.chars().take(63).collect();
        assert_eq!(os_strtod(&long), truncated.parse::<f64>().unwrap());
        // sign + digits + junk beyond the cut: 63 chars keep the 60 nines
        let long2 = format!("0.1{}", "9".repeat(70));
        let expected: String = format!("0.1{}", "9".repeat(60));
        assert_eq!(os_strtod(&long2), expected.parse::<f64>().unwrap());
    }
}
