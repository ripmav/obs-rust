//! S-014 §6.2/§6.3 — the JSON engine (writer and reader).
//!
//! This is a hand-rolled port of jansson 2.15.1's behaviour as specified by
//! S-014 (RFC 8259 with jansson's specifics): duplicate keys are an error,
//! an integer that does not fit `i64` is an error, a real that overflows is
//! an error, underflow keeps the sign (`-1e-400` → `-0.0`), nesting is
//! limited to depth 2047, and `\u0000` is an error. `serde_json` was
//! considered and rejected (P1-24.1): it accepts duplicate keys, `1e400`,
//! unbounded depth and NUL escapes, none of which jansson does.
//!
//! Text is byte oriented: keys and string values may carry invalid UTF-8
//! (the model stores byte strings); the writer then omits the affected
//! members entirely (QUIRK Q5) without error.

/// A parsed / renderable JSON value (byte oriented, insertion ordered).
#[derive(Clone, Debug, PartialEq)]
pub enum Json {
    Null,
    Bool(bool),
    Int(i64),
    Real(f64),
    /// Raw bytes; may be invalid UTF-8.
    Str(Vec<u8>),
    Arr(Vec<Json>),
    /// Insertion order; keys are unique (enforced by the parser).
    Obj(Vec<(Vec<u8>, Json)>),
}

/// Parse `text` as **one** JSON text with jansson's rules (S-014 §6.3).
///
/// The top-level value must be an object or an array; any trailing
/// non-whitespace is an error. BOMs are not skipped. The error is a unit:
/// S-014 logs one informational `LOG_ERROR` line on failure, which the
/// caller emits (this crate has no logger yet).
#[allow(clippy::result_unit_err)] // unit error is the contract (see docs)
pub fn parse(text: &[u8]) -> Result<Json, ()> {
    let mut p = P { b: text, i: 0 };
    p.ws();
    let v = p.value(0)?;
    p.ws();
    if p.i != p.b.len() {
        return Err(());
    }
    match &v {
        Json::Obj(_) | Json::Arr(_) => Ok(v),
        _ => Err(()),
    }
}

/// Render `v` per S-014 §6.2 (compact or pretty), omitting members whose
/// key or string value is not valid UTF-8 or whose real is non-finite
/// (QUIRK Q5, no error).
pub fn render(v: &Json, pretty: bool) -> String {
    let mut out = String::new();
    write_value(&mut out, v, 0, pretty);
    out
}

// ---------------------------------------------------------------------------
// Writer (S-014 §6.2)
// ---------------------------------------------------------------------------

fn write_value(out: &mut String, v: &Json, depth: usize, pretty: bool) {
    match v {
        Json::Null => out.push_str("null"),
        Json::Bool(b) => out.push_str(if *b { "true" } else { "false" }),
        Json::Int(i) => out.push_str(&i.to_string()),
        Json::Real(f) => out.push_str(&real_fmt(*f)),
        Json::Str(b) => write_string(
            out,
            std::str::from_utf8(b).expect("Q5: invalid standalone string"),
        ),
        Json::Arr(elems) => {
            if elems.is_empty() {
                out.push_str("[]");
                return;
            }
            out.push('[');
            for (n, e) in elems.iter().enumerate() {
                if n > 0 {
                    out.push(',');
                }
                if pretty {
                    out.push('\n');
                    indent(out, depth + 1);
                }
                write_value(out, e, depth + 1, pretty);
            }
            if pretty {
                out.push('\n');
                indent(out, depth);
            }
            out.push(']');
        }
        Json::Obj(members) => {
            if members.is_empty() {
                out.push_str("{}");
                return;
            }
            out.push('{');
            let mut written = 0;
            for (k, e) in members {
                if is_omitted(k, e) {
                    continue;
                }
                if written > 0 {
                    out.push(',');
                }
                if pretty {
                    out.push('\n');
                    indent(out, depth + 1);
                }
                write_string(out, std::str::from_utf8(k).expect("Q5: omitted member"));
                out.push(':');
                if pretty {
                    out.push(' ');
                }
                write_value(out, e, depth + 1, pretty);
                written += 1;
            }
            if pretty {
                out.push('\n');
                indent(out, depth);
            }
            out.push('}');
        }
    }
}

/// Q5: the member is dropped when the key is not valid UTF-8, the string
/// value is not valid UTF-8, or the real is NaN/±inf.
fn is_omitted(key: &[u8], v: &Json) -> bool {
    if !std::str::from_utf8(key).is_ok() {
        return true;
    }
    match v {
        Json::Str(b) => !std::str::from_utf8(b).is_ok(),
        Json::Real(f) => !f.is_finite(),
        _ => false,
    }
}

fn indent(out: &mut String, depth: usize) {
    for _ in 0..4 * depth {
        out.push(' ');
    }
}

/// §6.2 string escapes: `\" \\ \/ \b \f \n \r \t`, every other character
/// < 0x20 as `\u00XX` with **upper-case** hex; everything else (including `/`,
/// 0x7F, U+2028/U+2029, all non-ASCII) raw UTF-8. The text must be valid
/// UTF-8 — `render` omits members whose key or string value is not (Q5), so
/// only a standalone invalid string would reach the panic (caller error).
fn write_string(out: &mut String, s: &str) {
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\u{8}' => out.push_str("\\b"),
            '\u{C}' => out.push_str("\\f"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\t' => out.push_str("\\t"),
            c if (c as u32) < 0x20 => {
                out.push_str(&format!("\\u00{:02X}", c as u32));
            }
            c => out.push(c),
        }
    }
    out.push('"');
}

/// §6.2 real format. `digits` is the shortest round-trip digit string (Rust
/// `{:e}` produces exactly that), `x` its decimal exponent
/// (`value = d1.d2…dn × 10^x`).
fn real_fmt(f: f64) -> String {
    if f == 0.0 {
        return if f.is_sign_negative() {
            "-0.0".into()
        } else {
            "0.0".into()
        };
    }
    let neg = f.is_sign_negative();
    let e = format!("{:e}", f.abs()); // "d[.ddd]e±xx"
    let (mant, exp) = e.split_once('e').expect("format invariant");
    let digits: String = mant.chars().filter(|c| *c != '.').collect();
    let x: i32 = exp.parse().expect("format invariant");
    let body = if (-4..16).contains(&x) {
        if x >= 0 {
            let xi = x as usize;
            let int_part: String = digits
                .chars()
                .chain(std::iter::repeat('0'))
                .take(xi + 1)
                .collect();
            let frac = if digits.len() > xi + 1 {
                &digits[xi + 1..]
            } else {
                "0"
            };
            format!("{int_part}.{frac}")
        } else {
            format!("0.{}{}", "0".repeat((-x - 1) as usize), digits)
        }
    } else {
        let mantissa = if digits.len() > 1 {
            format!("{}.{}", &digits[..1], &digits[1..])
        } else {
            digits.clone()
        };
        format!("{mantissa}e{x}")
    };
    if neg { format!("-{body}") } else { body }
}

// ---------------------------------------------------------------------------
// Reader (S-014 §6.3)
// ---------------------------------------------------------------------------

const MAX_DEPTH: u32 = 2047;

struct P<'a> {
    b: &'a [u8],
    i: usize,
}

impl<'a> P<'a> {
    fn peek(&self) -> Option<u8> {
        self.b.get(self.i).copied()
    }
    fn next(&mut self) -> Option<u8> {
        let c = self.peek()?;
        self.i += 1;
        Some(c)
    }
    fn ws(&mut self) {
        while matches!(
            self.peek(),
            Some(b' ') | Some(b'\t') | Some(b'\n') | Some(b'\r')
        ) {
            self.i += 1;
        }
    }
    fn literal(&mut self, lit: &[u8]) -> Result<(), ()> {
        if self.b[self.i..].starts_with(lit) {
            // a token must be delimited: an identifier character after the
            // literal (e.g. `truex`) is an error
            if self
                .b
                .get(self.i + lit.len())
                .is_some_and(|c| c.is_ascii_alphanumeric() || *c == b'_')
            {
                return Err(());
            }
            self.i += lit.len();
            Ok(())
        } else {
            Err(())
        }
    }

    fn value(&mut self, depth: u32) -> Result<Json, ()> {
        match self.peek() {
            Some(b'{') => self.object(depth),
            Some(b'[') => self.array(depth),
            Some(b'"') => Ok(Json::Str(self.string()?)),
            Some(b't') => {
                self.literal(b"true")?;
                Ok(Json::Bool(true))
            }
            Some(b'f') => {
                self.literal(b"false")?;
                Ok(Json::Bool(false))
            }
            Some(b'n') => {
                self.literal(b"null")?;
                Ok(Json::Null)
            }
            Some(b'-') | Some(b'0'..=b'9') => self.number(),
            _ => Err(()),
        }
    }

    fn object(&mut self, depth: u32) -> Result<Json, ()> {
        if depth + 1 > MAX_DEPTH {
            return Err(());
        }
        self.next(); // '{'
        self.ws();
        if self.peek() == Some(b'}') {
            self.next();
            return Ok(Json::Obj(vec![]));
        }
        let mut members = Vec::new();
        loop {
            self.ws();
            if self.peek() != Some(b'"') {
                return Err(());
            }
            let key = self.string()?;
            if members.iter().any(|(k, _)| *k == key) {
                return Err(()); // duplicate keys are an error (byte-wise)
            }
            self.ws();
            if self.next() != Some(b':') {
                return Err(());
            }
            self.ws();
            let v = self.value(depth + 1)?;
            members.push((key, v));
            self.ws();
            match self.next() {
                Some(b',') => {}
                Some(b'}') => return Ok(Json::Obj(members)),
                _ => return Err(()),
            }
        }
    }

    fn array(&mut self, depth: u32) -> Result<Json, ()> {
        if depth + 1 > MAX_DEPTH {
            return Err(());
        }
        self.next(); // '['
        self.ws();
        if self.peek() == Some(b']') {
            self.next();
            return Ok(Json::Arr(vec![]));
        }
        let mut elems = Vec::new();
        loop {
            self.ws();
            elems.push(self.value(depth + 1)?);
            self.ws();
            match self.next() {
                Some(b',') => {}
                Some(b']') => return Ok(Json::Arr(elems)),
                _ => return Err(()),
            }
        }
    }

    /// §6.3 rule 4: `-? (0|[1-9][0-9]*) (\.[0-9]+)? ([eE][+-]?[0-9]+)?`.
    /// A number with `.`/`e`/`E` is a real. Leading `+`, `.5`, `1.`, `01`,
    /// `-`, `1e` are errors. Integer overflow is an **error**; real overflow
    /// (`1e400`) is an **error**; real underflow keeps the sign
    /// (`-1e-400` → `-0.0`). `-0` is the integer 0.
    fn number(&mut self) -> Result<Json, ()> {
        let start = self.i;
        if self.peek() == Some(b'-') {
            self.i += 1;
        }
        match self.next() {
            Some(b'0') => {
                if matches!(self.peek(), Some(b'0'..=b'9')) {
                    return Err(()); // leading zero
                }
            }
            Some(b'1'..=b'9') => {
                while matches!(self.peek(), Some(b'0'..=b'9')) {
                    self.i += 1;
                }
            }
            _ => return Err(()),
        }
        let mut is_real = false;
        if self.peek() == Some(b'.') {
            is_real = true;
            self.i += 1;
            if !matches!(self.peek(), Some(b'0'..=b'9')) {
                return Err(());
            }
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.i += 1;
            }
        }
        if matches!(self.peek(), Some(b'e') | Some(b'E')) {
            is_real = true;
            self.i += 1;
            if matches!(self.peek(), Some(b'+') | Some(b'-')) {
                self.i += 1;
            }
            if !matches!(self.peek(), Some(b'0'..=b'9')) {
                return Err(());
            }
            while matches!(self.peek(), Some(b'0'..=b'9')) {
                self.i += 1;
            }
        }
        let text = std::str::from_utf8(&self.b[start..self.i]).expect("digits only");
        if is_real {
            let v: f64 = text.parse().expect("scanned grammar");
            if v.is_infinite() {
                return Err(()); // overflow is an error
            }
            Ok(Json::Real(v))
        } else {
            let v: i64 = text.parse().map_err(|_| ())?; // integer overflow is an error
            Ok(Json::Int(v))
        }
    }

    /// §6.3 rule 6: control characters are errors; escapes
    /// `\" \\ \/ \b \f \n \r \t \uXXXX`; `\u0000` is an error; surrogates
    /// must form a valid high+low pair; raw bytes must be well-formed UTF-8.
    fn string(&mut self) -> Result<Vec<u8>, ()> {
        self.next(); // '"'
        let mut out = Vec::new();
        loop {
            let Some(c) = self.next() else {
                return Err(());
            };
            match c {
                b'"' => {
                    // raw bytes must be well-formed UTF-8 (same rules as
                    // §6.2; the escape-decoded parts are valid by
                    // construction)
                    if std::str::from_utf8(&out).is_err() {
                        return Err(());
                    }
                    return Ok(out);
                }
                b'\\' => match self.next() {
                    Some(b'"') => out.push(b'"'),
                    Some(b'\\') => out.push(b'\\'),
                    Some(b'/') => out.push(b'/'),
                    Some(b'b') => out.push(0x08),
                    Some(b'f') => out.push(0x0C),
                    Some(b'n') => out.push(0x0A),
                    Some(b'r') => out.push(0x0D),
                    Some(b't') => out.push(0x09),
                    Some(b'u') => {
                        let hi = self.hex4()?;
                        if hi == 0 {
                            return Err(()); // \u0000
                        }
                        if (0xD800..=0xDBFF).contains(&hi) {
                            if self.next() != Some(b'\\') || self.next() != Some(b'u') {
                                return Err(());
                            }
                            let lo = self.hex4()?;
                            if !(0xDC00..=0xDFFF).contains(&lo) {
                                return Err(());
                            }
                            let cp: u32 =
                                0x10000 + (((hi - 0xD800) as u32) << 10) + (lo - 0xDC00) as u32;
                            let mut buf = [0u8; 4];
                            out.extend_from_slice(
                                char::from_u32(cp)
                                    .expect("valid pair")
                                    .encode_utf8(&mut buf)
                                    .as_bytes(),
                            );
                        } else if (0xDC00..=0xDFFF).contains(&hi) {
                            return Err(()); // lone low surrogate
                        } else {
                            let mut buf = [0u8; 4];
                            out.extend_from_slice(
                                char::from_u32(hi as u32)
                                    .expect("valid scalar")
                                    .encode_utf8(&mut buf)
                                    .as_bytes(),
                            );
                        }
                    }
                    _ => return Err(()),
                },
                c if c < 0x20 => return Err(()),
                c => out.push(c),
            }
        }
    }

    fn hex4(&mut self) -> Result<u16, ()> {
        let mut v: u16 = 0;
        for _ in 0..4 {
            let Some(c) = self.next() else {
                return Err(());
            };
            let d = (c as char).to_digit(16).ok_or(())?;
            v = v
                .checked_mul(16)
                .and_then(|x| x.checked_add(d as u16))
                .ok_or(())?;
        }
        Ok(v)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ok(s: &str) -> Json {
        parse(s.as_bytes()).expect("parse")
    }
    fn err(s: &str) {
        assert!(parse(s.as_bytes()).is_err(), "expected error: {s:?}");
    }

    #[test]
    fn parse_objects_and_arrays() {
        assert_eq!(ok("{}"), Json::Obj(vec![]));
        assert_eq!(ok("[]"), Json::Arr(vec![]));
        assert_eq!(
            ok(" {\"a\": 1, \"b\":[2,3]} "),
            Json::Obj(vec![
                (b"a".to_vec(), Json::Int(1)),
                (b"b".to_vec(), Json::Arr(vec![Json::Int(2), Json::Int(3)])),
            ])
        );
        // whitespace forms
        assert_eq!(ok("\t\r\n {\n\"a\"\t:\ttrue\r\n} "), ok("{\"a\":true}"));
    }

    #[test]
    fn parse_numbers() {
        assert_eq!(
            ok("[-0]"),
            Json::Arr(vec![Json::Int(0)]),
            "-0 is the integer 0"
        );
        assert_eq!(
            ok("[123, -45, 0, 1.5, 2.0, 1e3, 1E-3, 1.25e+2]"),
            Json::Arr(vec![
                Json::Int(123),
                Json::Int(-45),
                Json::Int(0),
                Json::Real(1.5),
                Json::Real(2.0),
                Json::Real(1000.0),
                Json::Real(0.001),
                Json::Real(125.0),
            ])
        );
        assert_eq!(
            ok("[9223372036854775807, -9223372036854775808]"),
            Json::Arr(vec![Json::Int(i64::MAX), Json::Int(i64::MIN)])
        );
        // integer overflow is an error
        err("[9223372036854775808]");
        err("[-9223372036854775809]");
        // real overflow is an error; underflow keeps the sign
        err("[1e400]");
        err("[-1e400]");
        assert_eq!(ok("[1e-400]"), Json::Arr(vec![Json::Real(0.0)]));
        let neg = ok("[-1e-400]").clone();
        match neg {
            Json::Arr(v) => {
                assert_eq!(v[0], Json::Real(f64::from_bits(1 << 63)), "-1e-400 is -0.0")
            }
            _ => panic!(),
        }
        // correct rounding
        assert_eq!(ok("[1e300]"), Json::Arr(vec![Json::Real(1e300)]));
    }

    #[test]
    fn parse_number_errors() {
        for s in [
            "[+5]",
            "[.5]",
            "[1.]",
            "[01]",
            "[-]",
            "[1e]",
            "[1e+]",
            "[1.e5]",
            "[0x10]",
            "[NaN]",
            "[Infinity]",
            "[1.2.3]",
        ] {
            err(s);
        }
    }

    #[test]
    fn parse_literals_delimited() {
        assert_eq!(
            ok("[true, false, null]"),
            Json::Arr(vec![Json::Bool(true), Json::Bool(false), Json::Null])
        );
        for s in ["[truex]", "[True]", "[nul]", "[tru e]"] {
            err(s);
        }
    }

    #[test]
    fn parse_strings_and_escapes() {
        assert_eq!(
            ok("[\"a\\\"b\\\\c\\/d\\be\\ff\\ng\\rh\\ti\"]"),
            Json::Arr(vec![Json::Str(vec![
                b'a', b'"', b'b', b'\\', b'c', b'/', b'd', 0x08, b'e', 0x0C, b'f', 0x0A, b'g',
                0x0D, b'h', 0x09, b'i',
            ])])
        );
        // \uXXXX both hex cases; non-ASCII
        assert_eq!(
            ok("[\"\\u0041\\u00e9\\uFFFF\"]"),
            Json::Arr(vec![Json::Str(vec![b'A', 0xC3, 0xA9, 0xEF, 0xBF, 0xBF])])
        );
        // surrogate pair: D801 DC11 is U+10411
        assert_eq!(
            ok("[\"\\uD801\\uDC11\"]"),
            Json::Arr(vec![Json::Str(vec![0xF0, 0x90, 0x90, 0x91])])
        );
        // DBFF + DC00 is a valid pair (U+10FC00); the max pair is U+10FFFF
        assert_eq!(
            ok("[\"\\uDBFF\\uDC00\"]"),
            Json::Arr(vec![Json::Str(vec![0xF4, 0x8F, 0xB0, 0x80])])
        );
        assert_eq!(
            ok("[\"\\uDBFF\\uDFFF\"]"),
            Json::Arr(vec![Json::Str(vec![0xF4, 0x8F, 0xBF, 0xBF])])
        );
        // errors
        for s in [
            "[\"\\u0000\"]",        // NUL escape
            "[\"\\uD800\"]",        // lone high
            "[\"\\uDC00\"]",        // lone low
            "[\"\\uD800x\"]",       // broken pair
            "[\"\\uD800\\u0041\"]", // high + non-low
            "[\"a\nb\"]",           // raw control
            "[\"a\\qb\"]",          // bad escape
            "[\"\\u12G4\"]",        // bad hex
        ] {
            err(s);
        }
        // raw bytes must be well-formed UTF-8: dangling lead byte
        let bad: [u8; 5] = [b'[', b'"', 0xC3, b'"', b']'];
        assert!(parse(&bad).is_err());
        // overlong encoding (C0 AF is a 2-byte '/')
        let bad: [u8; 6] = [b'[', b'"', 0xC0, 0xAF, b'"', b']'];
        assert!(parse(&bad).is_err());
    }

    #[test]
    fn parse_depth_limit() {
        let deep = |n: u32| "[".repeat(n as usize) + "1" + &"]".repeat(n as usize);
        assert!(parse(deep(2047).as_bytes()).is_ok(), "2047 pairs ok");
        assert!(parse(deep(2048).as_bytes()).is_err(), "2048 pairs fail");
    }

    #[test]
    fn parse_duplicate_keys_error() {
        err("{\"a\":1,\"a\":2}");
        err("{\"a\":1, \"b\":{\"c\":1,\"c\":2}}");
        // separate objects may repeat keys
        assert!(
            parse(b"[{\"a\":1},{\"a\":1}]").is_ok(),
            "separate objects are fine"
        );
    }

    #[test]
    fn parse_top_level_rules() {
        err("5");
        err("\"x\"");
        err("true");
        err("null");
        err("");
        err("   ");
        assert!(parse(b"[]").is_ok());
        err("{} x"); // trailing garbage
        // BOM is an error (not skipped)
        assert!(parse(&[0xEF, 0xBB, 0xBF, b'{', b'}']).is_err());
        err("{'a':1}"); // single quotes
        err("{a:1}"); // unquoted key
    }

    #[test]
    fn vector_12_json_write_roundtrip() {
        // the 12-json-write vector, byte for byte (parse + render must
        // reproduce the authoritative text)
        let d = r#"{"a":1.0,"b":1.5,"c":100.0,"d":1000000000000000.0,"e":1e16,"f":1.2345678901234568e17,"g":0.1,"h":0.0001,"i":1e-5,"j":1e-7,"k":0.30000000000000004,"l":3.141592653589793,"m":-0.0,"n":0.0,"o":1.5e300,"p":5e-324,"q":1.7976931348623157e308,"r":1e21,"s":-2.5e-10,"t":9007199254740992.0,"u":0.00012345678,"v":0.00012,"w":1.2e-5,"x":1500000000000000.0,"y":9999999999999998.0}"#;
        let j = parse(d.as_bytes()).expect("vector d");
        assert_eq!(render(&j, false), d);
        let d2 = r#"{"a":1.0000000000000002,"b":0.6666666666666666,"c":2.5e-5,"d":1.2345678901234568e17,"e":4.35,"f":1e23,"g":8.5e-5,"h":2.2250738585072014e-308,"i":5e-324,"j":-1e-300,"k":0.0001,"l":1000000000000000.0,"m":4503599627370496.0,"n":299792458.0,"o":0.0001,"p":12345678.9}"#;
        let j = parse(d2.as_bytes()).expect("vector d2");
        assert_eq!(render(&j, false), d2);
        // the 13-json-pretty vector
        let p = r#"{
    "s": "text",
    "i": 1,
    "d": 2.0,
    "b": true,
    "obj": {
        "inner": {
            "x": 1
        },
        "nullobj": null
    },
    "emptyobj": {},
    "emptyarr": [],
    "items": [
        {
            "n": 1,
            "s": "x"
        },
        {}
    ]
}"#;
        let j = parse(p.as_bytes()).expect("vector pretty");
        assert_eq!(render(&j, true), p);
        // the same object compact
        let c = r#"{"s":"text","i":1,"d":2.0,"b":true,"obj":{"inner":{"x":1},"nullobj":null},"emptyobj":{},"emptyarr":[],"items":[{"n":1,"s":"x"},{}]}"#;
        assert_eq!(render(&j, false), c);
    }

    #[test]
    fn real_fmt_worked_values() {
        // the 12-json-write vector (authoritative; jansson 2.15)
        let f = |s: &str| real_fmt(s.parse().unwrap());
        assert_eq!(f("1"), "1.0");
        assert_eq!(f("1.5"), "1.5");
        assert_eq!(f("100"), "100.0");
        assert_eq!(f("1e15"), "1000000000000000.0");
        assert_eq!(f("1e16"), "1e16");
        assert_eq!(f("123456789012345678"), "1.2345678901234568e17");
        assert_eq!(f("0.1"), "0.1");
        assert_eq!(f("0.0001"), "0.0001");
        assert_eq!(f("0.00001"), "1e-5");
        assert_eq!(f("1e-7"), "1e-7");
        // the script's literal 0.30000000000000004 is the double *above* 0.3;
        // 0.3 itself is the shortest "0.3"
        assert_eq!(f("0.3"), "0.3");
        assert_eq!(f("0.30000000000000004"), "0.30000000000000004");
        assert_eq!(f("3.141592653589793"), "3.141592653589793");
        assert_eq!(f("-0.0"), "-0.0");
        assert_eq!(f("0"), "0.0");
        assert_eq!(f("1.5e300"), "1.5e300");
        assert_eq!(f("5e-324"), "5e-324");
        assert_eq!(f("1.7976931348623157e308"), "1.7976931348623157e308");
        assert_eq!(f("1e21"), "1e21");
        assert_eq!(f("-2.5e-10"), "-2.5e-10");
        assert_eq!(f("9007199254740993"), "9007199254740992.0");
        assert_eq!(f("12345.678e-8"), "0.00012345678");
        assert_eq!(f("0.00012"), "0.00012");
        assert_eq!(f("0.000012"), "1.2e-5");
        assert_eq!(f("1.5e15"), "1500000000000000.0");
        assert_eq!(f("9999999999999998"), "9999999999999998.0");
        // more-doubles block
        assert_eq!(f("1.0000000000000002"), "1.0000000000000002");
        assert_eq!(f("0.6666666666666666"), "0.6666666666666666");
        assert_eq!(f("2.5e-5"), "2.5e-5");
        assert_eq!(f("4.35"), "4.35");
        assert_eq!(f("1e23"), "1e23");
        assert_eq!(f("8.5e-5"), "8.5e-5");
        assert_eq!(f("2.2250738585072014e-308"), "2.2250738585072014e-308");
        assert_eq!(f("4.9406564584124654e-324"), "5e-324");
        assert_eq!(f("-1e-300"), "-1e-300");
        assert_eq!(f("0.1e-3"), "0.0001");
        assert_eq!(f("4503599627370496.5"), "4503599627370496.0");
        assert_eq!(f("299792458"), "299792458.0");
        assert_eq!(f("1e-4"), "0.0001");
        assert_eq!(f("12345678.9"), "12345678.9");
        // positional/scientific boundaries: -4 .. 15 positional, else scientific
        assert_eq!(f("1e15"), "1000000000000000.0");
        assert_eq!(f("-2.9"), "-2.9");
        assert_eq!(f("3000000000.75"), "3000000000.75");
        // always distinguishable from integers
        assert!(real_fmt(1.0).ends_with(".0"));
        assert!(real_fmt(2.0).ends_with(".0"));
    }

    #[test]
    fn render_compact_and_pretty() {
        let v = ok("{\"a\":1,\"b\":[{\"c\":2}]}").clone();
        assert_eq!(render(&v, false), "{\"a\":1,\"b\":[{\"c\":2}]}");
        assert_eq!(
            render(&v, true),
            "{\n    \"a\": 1,\n    \"b\": [\n        {\n            \"c\": 2\n        }\n    ]\n}"
        );
        assert_eq!(render(&Json::Obj(vec![]), true), "{}");
        assert_eq!(render(&Json::Arr(vec![]), true), "[]");
        // empty members on one line
        assert_eq!(
            render(&ok("{\"a\":{},\"b\":[]}").clone(), false),
            "{\"a\":{},\"b\":[]}"
        );
    }

    #[test]
    fn render_string_escapes() {
        let v = Json::Str(vec![
            b'a', b'"', b'b', b'\\', b'c', 0x08, b'd', 0x0C, b'e', 0x0A, b'g', 0x0D, b'h', 0x09,
            b'i', 0x01,
        ]);
        let out = render(&Json::Obj(vec![(b"k".to_vec(), v)]), false);
        assert_eq!(out, "{\"k\":\"a\\\"b\\\\c\\bd\\fe\\ng\\rh\\ti\\u0001\"}");
        // / and 0x7F and U+2028 stay raw
        let out = render(
            &Json::Obj(vec![(
                b"k".to_vec(),
                Json::Str([b'/', 0x7F, 0xE2, 0x80, 0xA8].to_vec()),
            )]),
            false,
        );
        assert_eq!(out, "{\"k\":\"/\u{7f}\u{2028}\"}");
    }

    #[test]
    fn render_omits_invalid_members() {
        // invalid UTF-8 key
        let v = Json::Obj(vec![
            (vec![0xFF, 0xFE], Json::Int(1)),
            (b"ok".to_vec(), Json::Int(2)),
        ]);
        assert_eq!(render(&v, false), "{\"ok\":2}");
        // invalid UTF-8 string value
        let v = Json::Obj(vec![
            (b"a".to_vec(), Json::Str(vec![0xFF])),
            (b"b".to_vec(), Json::Int(1)),
        ]);
        assert_eq!(render(&v, false), "{\"b\":1}");
        // non-finite real
        let v = Json::Obj(vec![
            (b"a".to_vec(), Json::Real(f64::NAN)),
            (b"b".to_vec(), Json::Real(f64::INFINITY)),
            (b"c".to_vec(), Json::Int(1)),
        ]);
        assert_eq!(render(&v, false), "{\"c\":1}");
    }
}
