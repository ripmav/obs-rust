//! INI-style configuration store (S-007, P1-09).
//!
//! The store behind `global.ini`, `user.ini`, every profile's `basic.ini`
//! and many plugin settings. Files are user data and must round-trip
//! bit-exactly across versions, so the reader, the writer, and the typed
//! conversions are all normative (spec `docs/rust-rewrite/specs/
//! S-007-config-file.md`).
//!
//! Data model (S-007 §3): two ordered maps — **user** values and
//! **defaults** — each an ordered list of sections, each an ordered list
//! of items (name -> string value). Insertion order is kept, `get` on a
//! duplicated key returns the most recently inserted entry (Q-1), and
//! names compare byte-wise, case-sensitively. Everything is stored as a
//! string; typed getters convert on read.
//!
//! The C reference takes a recursive mutex per operation (§6); the safe
//! API gets the same exclusivity from `&self`/`&mut self`, and a
//! `get_string` result is valid exactly as long as the borrow lives
//! (stricter than the C pointer).
//!
//! `set_double` writes the D-2-fixed `os_dtostr` text (`os_str` module);
//! the corrupted reference texts are documented there (open question 2,
//! fixed branch per P1-09.3).
//!
//! Platform-neutral: on Windows the save output is preceded by the
//! `EF BB BF` BOM (spec §4.2); the platform layer applies it.

use std::fs;
use std::path::{Path, PathBuf};

use crate::os_str::{os_dtostr, os_strtod};

pub const CONFIG_SUCCESS: i32 = 0;
pub const CONFIG_FILENOTFOUND: i32 = -1;
pub const CONFIG_ERROR: i32 = -2;

/// S-007-R02 open modes (`enum config_open_type`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
#[repr(i32)]
pub enum ConfigOpenType {
    /// The file must exist; missing -> `CONFIG_FILENOTFOUND`.
    Existing = 0,
    /// Create the file if it is missing (empty).
    Always = 1,
}

struct Section {
    name: String,
    items: Vec<Item>,
}

struct Item {
    name: String,
    value: String,
}

/// S-007 §2 `config_t`: user values and defaults, both in insertion
/// order (duplicates kept, Q-1).
#[derive(Default)]
pub struct Config {
    file: Option<PathBuf>,
    user: Vec<Section>,
    defaults: Vec<Section>,
}

// ---------------------------------------------------------------------------
// parsing (S-007 §4.1)
// ---------------------------------------------------------------------------

/// In the value only: `\\` -> `\`, `\r` -> CR, `\n` -> LF; any other
/// backslash sequence is kept literally including the backslash, and a
/// trailing lone backslash is kept.
fn unescape(value: &str) -> String {
    let mut out = String::with_capacity(value.len());
    let mut chars = value.chars();
    while let Some(c) = chars.next() {
        if c == '\\' {
            match chars.next() {
                Some('\\') => out.push('\\'),
                Some('r') => out.push('\r'),
                Some('n') => out.push('\n'),
                Some(other) => {
                    out.push('\\');
                    out.push(other);
                }
                None => out.push('\\'), // trailing lone backslash
            }
        } else {
            out.push(c);
        }
    }
    out
}

/// Split a config text into lines: `\n`, `\r\n` and `\r` all end a line
/// (S-007 §4.1.5).
fn split_lines(text: &str) -> Vec<&str> {
    text.split(['\n', '\r']).collect()
}

/// S-007 §4.1: parse `text` into `sections`. A leading UTF-8 BOM
/// (`EF BB BF` at offset 0) is removed first; nothing else is touched.
/// Invalid UTF-8 is passed through as U+FFFD (the spec leaves this
/// uncharacterised; the C reference keeps raw bytes, which a Rust
/// `String` cannot represent). NUL bytes are kept.
fn parse_into(text: &str, sections: &mut Vec<Section>) {
    let text = text.strip_prefix('\u{feff}').unwrap_or(text);
    let lines = split_lines(text);
    let mut i = 0usize;
    loop {
        // top level: skip whitespace-only lines (space, tab, newlines)
        while i < lines.len() && lines[i].trim_matches([' ', '\t']).is_empty() {
            i += 1;
        }
        if i >= lines.len() {
            break;
        }
        let line = lines[i];
        i += 1;
        let trimmed = line.trim_start_matches([' ', '\t']);
        if !trimmed.starts_with('[') {
            continue; // rest of the line is ignored
        }
        // section header: after `[` up to `]` or end of line (Q-5: a
        // header missing `]` still creates the section from the text)
        let after = &trimmed[1..];
        let name = match after.find(']') {
            Some(p) => after[..p].to_string(),
            None => after.to_string(),
        };
        if name.is_empty() {
            break; // `[]` stops parsing entirely (R-D3)
        }
        let mut section = Section {
            name,
            items: vec![],
        };
        // body: until the next line whose first non-white char is `[`
        while i < lines.len() {
            let t = lines[i].trim_start_matches([' ', '\t']);
            if t.starts_with('[') {
                break;
            }
            i += 1;
            if t.is_empty() || t.starts_with('#') {
                continue;
            }
            let eq = match t.find('=') {
                Some(p) if p > 0 => p, // name would start with `=` -> ignore
                _ => continue,         // no `=` before end -> ignore
            };
            // name: up to the first `=` (trailing whitespace kept, Q-4);
            // value: after the `=` to end of line (whitespace kept, Q-4)
            let name = t[..eq].to_string();
            let value = unescape(&t[eq + 1..]);
            section.items.push(Item { name, value });
        }
        sections.push(section);
    }
}

// ---------------------------------------------------------------------------
// open / create / close
// ---------------------------------------------------------------------------

/// Read a whole file, dropping a leading UTF-8 BOM (`os_fread_utf8`
/// semantics, S-023 R55; the native API returns the text, the C ABI
/// shim returns 0 per S-023 D-3). Invalid UTF-8 becomes U+FFFD.
fn read_config_file(path: &Path) -> Result<String, std::io::Error> {
    let bytes = fs::read(path)?;
    let bytes = bytes
        .strip_prefix(&[0xEF, 0xBB, 0xBF][..])
        .unwrap_or(&bytes);
    Ok(String::from_utf8_lossy(bytes).into_owned())
}

/// S-007-R01 `config_create(file)`: creates/truncates `file` (failure
/// -> `None`) and returns an empty config bound to it.
pub fn config_create(file: &Path) -> Option<Box<Config>> {
    match fs::write(file, []) {
        Ok(()) => Some(Box::new(Config {
            file: Some(file.to_path_buf()),
            ..Default::default()
        })),
        Err(_) => None,
    }
}

/// S-007-R02 `config_open(&c, file, open_type)`:
/// - file missing: `EXISTING` -> `CONFIG_FILENOTFOUND` (config stays
///   unset); `ALWAYS` -> the file is created empty and an empty config
///   is returned.
/// - file exists: it is parsed into the **user** map (an empty or
///   BOM-only file opens with zero sections).
pub fn config_open(out: &mut Option<Box<Config>>, file: &Path, open_type: ConfigOpenType) -> i32 {
    if out.is_some() {
        return CONFIG_ERROR;
    }
    match read_config_file(file) {
        Ok(text) => {
            let mut config = Config::default();
            parse_into(&text, &mut config.user);
            config.file = Some(file.to_path_buf());
            *out = Some(Box::new(config));
            CONFIG_SUCCESS
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
            if open_type == ConfigOpenType::Always {
                match fs::write(file, []) {
                    Ok(()) => {
                        *out = Some(Box::new(Config {
                            file: Some(file.to_path_buf()),
                            ..Default::default()
                        }));
                        CONFIG_SUCCESS
                    }
                    Err(_) => CONFIG_ERROR,
                }
            } else {
                CONFIG_FILENOTFOUND
            }
        }
        Err(_) => CONFIG_ERROR,
    }
}

/// S-007 §4.1 `config_open_string(&c, str)`: parses `str` exactly like
/// a file (a leading BOM, if any, is dropped); the config has **no
/// file**, so `config_save` fails with `CONFIG_ERROR`. Never fails.
pub fn config_open_string(out: &mut Option<Box<Config>>, text: &str) -> i32 {
    if out.is_some() {
        return CONFIG_ERROR;
    }
    let mut config = Config::default();
    parse_into(text, &mut config.user);
    *out = Some(Box::new(config));
    CONFIG_SUCCESS
}

/// S-007-R03 `config_open_defaults(c, file)`: parses `file` into the
/// **defaults** map, adding to what is there (repeated calls accumulate;
/// the later definition of a key wins). Missing file ->
/// `CONFIG_FILENOTFOUND`. It does not create user values.
pub fn config_open_defaults(config: &mut Config, file: &Path) -> i32 {
    match read_config_file(file) {
        Ok(text) => {
            parse_into(&text, &mut config.defaults);
            CONFIG_SUCCESS
        }
        Err(e) if e.kind() == std::io::ErrorKind::NotFound => CONFIG_FILENOTFOUND,
        Err(_) => CONFIG_ERROR,
    }
}

/// S-007-R16 `config_close`: frees everything (the `Box` is dropped).
pub fn config_close(out: &mut Option<Box<Config>>) {
    *out = None;
}

// ---------------------------------------------------------------------------
// section enumeration
// ---------------------------------------------------------------------------

/// S-007-R06: number of **user** sections (defaults are not counted).
/// Sections are never removed.
#[must_use]
pub fn config_num_sections(config: &Config) -> usize {
    config.user.len()
}

/// S-007-R06: the idx-th user section name in insertion order, or
/// `None` out of range.
#[must_use]
pub fn config_get_section(config: &Config, idx: usize) -> Option<&str> {
    config.user.get(idx).map(|s| s.name.as_str())
}

// ---------------------------------------------------------------------------
// entry lookups (S-007 Q-1: the *last inserted entry* for a (section,
// name) key — duplicate section records are scanned from the end, and
// within a section the items are scanned from the end)
// ---------------------------------------------------------------------------

/// Locate the last inserted (section, item) entry for `section`/`name`,
/// or `None`.
fn find_entry(sections: &[Section], section: &str, name: &str) -> Option<(usize, usize)> {
    for (si, sec) in sections.iter().enumerate().rev() {
        if sec.name != section {
            continue;
        }
        if let Some(ii) = sec.items.iter().rposition(|it| it.name == name) {
            return Some((si, ii));
        }
    }
    None
}

/// `find_entry`, but create the entry (section appended when absent,
/// item appended to the last section record with that name) when
/// missing and `create` is set (S-007-R07).
fn find_entry_or_create(sections: &mut Vec<Section>, section: &str, name: &str) -> (usize, usize) {
    if let Some(entry) = find_entry(sections, section, name) {
        return entry;
    }
    let si = match sections.iter().rposition(|s| s.name == section) {
        Some(i) => i,
        None => {
            sections.push(Section {
                name: section.to_string(),
                items: vec![],
            });
            sections.len() - 1
        }
    };
    sections[si].items.push(Item {
        name: name.to_string(),
        value: String::new(),
    });
    (si, sections[si].items.len() - 1)
}

// ---------------------------------------------------------------------------
// setters (S-007 R07/R09)
// ---------------------------------------------------------------------------

fn set_value(sections: &mut Vec<Section>, section: &str, name: &str, value: &str) {
    let (si, ii) = find_entry_or_create(sections, section, name);
    sections[si].items[ii].value = value.to_string();
}

fn set(
    config: &mut Config,
    user: bool,
    section: &str,
    name: &str,
    value: &str,
    also_user_when_absent: bool,
) {
    let was_user_value = has_in(&config.user, section, name);
    let maps = if user {
        &mut config.user
    } else {
        &mut config.defaults
    };
    set_value(maps, section, name, value);
    if !user && also_user_when_absent && !was_user_value {
        // R09: a default set without a user value also creates the
        // user value with the same text
        set_value(&mut config.user, section, name, value);
    }
}

fn has_in(sections: &[Section], section: &str, name: &str) -> bool {
    find_entry(sections, section, name).is_some()
}

fn get_text<'a>(sections: &'a [Section], section: &str, name: &str) -> Option<&'a str> {
    let (si, ii) = find_entry(sections, section, name)?;
    Some(sections[si].items[ii].value.as_str())
}

/// S-007-R07 `config_set_string(c, section, name, value)`: creates the
/// section (appended) and the item (appended) if absent, otherwise
/// replaces the value in place (position unchanged). `None` stores `""`.
pub fn config_set_string(config: &mut Config, section: &str, name: &str, value: Option<&str>) {
    set(config, true, section, name, value.unwrap_or(""), false);
}

/// S-007-R08 `config_set_int`: decimal `%lld`.
pub fn config_set_int(config: &mut Config, section: &str, name: &str, value: i64) {
    set(config, true, section, name, &value.to_string(), false);
}

/// S-007-R08 `config_set_uint`: decimal `%llu`.
pub fn config_set_uint(config: &mut Config, section: &str, name: &str, value: u64) {
    set(config, true, section, name, &value.to_string(), false);
}

/// S-007-R08 `config_set_bool`: `"true"`/`"false"`.
pub fn config_set_bool(config: &mut Config, section: &str, name: &str, value: bool) {
    set(
        config,
        true,
        section,
        name,
        if value { "true" } else { "false" },
        false,
    );
}

/// S-007-R08 `config_set_double`: the `os_dtostr` (D-2-fixed) text.
pub fn config_set_double(config: &mut Config, section: &str, name: &str, value: f64) {
    set(config, true, section, name, &os_dtostr(value), false);
}

/// S-007-R09: default variants. When there is **no** user value for
/// (section, name), a user value with the same text is created as well
/// (so `config_has_user_value` becomes true); an existing user value is
/// left unchanged.
pub fn config_set_default_string(
    config: &mut Config,
    section: &str,
    name: &str,
    value: Option<&str>,
) {
    set(config, false, section, name, value.unwrap_or(""), true);
}

/// S-007-R09 `config_set_default_int`: decimal `%lld`.
pub fn config_set_default_int(config: &mut Config, section: &str, name: &str, value: i64) {
    set(config, false, section, name, &value.to_string(), true);
}

/// S-007-R09 `config_set_default_uint`: decimal `%llu`.
pub fn config_set_default_uint(config: &mut Config, section: &str, name: &str, value: u64) {
    set(config, false, section, name, &value.to_string(), true);
}

/// S-007-R09 `config_set_default_bool`: `"true"`/`"false"`.
pub fn config_set_default_bool(config: &mut Config, section: &str, name: &str, value: bool) {
    set(
        config,
        false,
        section,
        name,
        if value { "true" } else { "false" },
        true,
    );
}

/// C `printf("%g")` of `value`: shortest of fixed/exponential with
/// `prec` significant digits, trailing zeros stripped, exponent with a
/// sign and at least two digits (`1234567.891` -> `1.23457e+06`).
fn g_sig(v: f64, prec: usize) -> String {
    if v.is_nan() {
        return "nan".into();
    }
    if v.is_infinite() {
        return if v > 0.0 { "inf".into() } else { "-inf".into() };
    }
    let s = format!("{v:.prec$e}", prec = prec - 1);
    let (mant, exp) = s.split_once('e').unwrap();
    let exp: i32 = exp.parse().unwrap();
    let neg = mant.starts_with('-');
    let mant = mant.strip_prefix('-').unwrap_or(mant);
    let (int_part, frac_part) = mant.split_once('.').unwrap();
    let digits = format!("{int_part}{frac_part}");
    let sign = if neg { "-" } else { "" };
    if !(-4..prec as i32).contains(&exp) {
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

/// S-007-R09 `config_set_default_double`: `printf("%g")`, six
/// significant digits (Q-3) — unlike `config_set_double`.
pub fn config_set_default_double(config: &mut Config, section: &str, name: &str, value: f64) {
    set(config, false, section, name, &g_sig(value, 6), true);
}

// ---------------------------------------------------------------------------
// getters (S-007 R10-R13)
// ---------------------------------------------------------------------------

/// S-007-R10 `config_get_string`: the user value if present, else the
/// default, else `None`. The borrow keeps the item immutable for its
/// lifetime (the C pointer is "valid until the item changes" — Rust
/// makes that a compile-time guarantee).
#[must_use]
pub fn config_get_string<'a>(config: &'a Config, section: &str, name: &str) -> Option<&'a str> {
    get_text(&config.user, section, name).or_else(|| get_text(&config.defaults, section, name))
}

/// S-007-R10: defaults map only.
#[must_use]
pub fn config_get_default_string<'a>(
    config: &'a Config,
    section: &str,
    name: &str,
) -> Option<&'a str> {
    get_text(&config.defaults, section, name)
}

/// S-007-R11: C `strtoll` semantics — NULL/empty -> 0; a text starting
/// with the two characters `0x` (**lowercase x only**, Q-6) is parsed
/// in base 16 (digits after the prefix), otherwise base 10; leading
/// whitespace is skipped, a sign is accepted, and parsing stops at the
/// first invalid character (`"12abc"` -> 12, `"abc"` -> 0, `"0X1F"`
/// -> 0). Out-of-range results saturate like `strtoll` (ERANGE).
fn parse_int(text: Option<&str>) -> i64 {
    let Some(t) = text.map(str::trim_start).filter(|t| !t.is_empty()) else {
        return 0;
    };
    let (sign, rest) = match t.as_bytes()[0] {
        b'-' => (-1, &t[1..]),
        b'+' => (1, &t[1..]),
        _ => (1, t),
    };
    let (base, digits) =
        if rest.len() >= 2 && rest.as_bytes()[0] == b'0' && rest.as_bytes()[1] == b'x' {
            (16, &rest[2..])
        } else {
            (10, rest)
        };
    let consumed: String = digits.chars().take_while(|c| c.is_digit(base)).collect();
    if consumed.is_empty() {
        return 0;
    }
    let Ok(magnitude) = i128::from_str_radix(&consumed, base) else {
        return if sign < 0 { i64::MIN } else { i64::MAX }; // digit overflow
    };
    let v = magnitude * sign as i128;
    if v > i64::MAX as i128 {
        i64::MAX
    } else if v < i64::MIN as i128 {
        i64::MIN
    } else {
        v as i64
    }
}

/// S-007-R11 `config_get_int`.
#[must_use]
pub fn config_get_int(config: &Config, section: &str, name: &str) -> i64 {
    parse_int(
        get_text(&config.user, section, name).or_else(|| get_text(&config.defaults, section, name)),
    )
}

/// S-007-R11 `config_get_uint`: C `strtoull` semantics — unsigned
/// conversion of a negative text wraps (`"-5"` -> 2^64-5); positive
/// overflow saturates to `u64::MAX`, negative overflow to 0 (ERANGE).
fn parse_uint(text: Option<&str>) -> u64 {
    let Some(t) = text.map(str::trim_start).filter(|t| !t.is_empty()) else {
        return 0;
    };
    let (sign, rest) = match t.as_bytes()[0] {
        b'-' => (-1, &t[1..]),
        b'+' => (1, &t[1..]),
        _ => (1, t),
    };
    let (base, digits) =
        if rest.len() >= 2 && rest.as_bytes()[0] == b'0' && rest.as_bytes()[1] == b'x' {
            (16, &rest[2..])
        } else {
            (10, rest)
        };
    let consumed: String = digits.chars().take_while(|c| c.is_digit(base)).collect();
    if consumed.is_empty() {
        return 0;
    }
    let magnitude: i128 = match i128::from_str_radix(&consumed, base) {
        Ok(m) => m,
        Err(_) => return if sign < 0 { 0 } else { u64::MAX },
    };
    let v = magnitude * sign as i128;
    let max = u64::MAX as i128;
    if v > max {
        u64::MAX
    } else if v < 0 {
        if v < -max { 0 } else { (max + v + 1) as u64 }
    } else {
        v as u64
    }
}

/// S-007-R11 `config_get_uint`.
#[must_use]
pub fn config_get_uint(config: &Config, section: &str, name: &str) -> u64 {
    parse_uint(
        get_text(&config.user, section, name).or_else(|| get_text(&config.defaults, section, name)),
    )
}

/// S-007-R12 `config_get_bool`: true iff the text equals `true`
/// case-insensitively (ASCII) or `get_uint(text)` is non-zero; missing
/// -> false.
#[must_use]
pub fn config_get_bool(config: &Config, section: &str, name: &str) -> bool {
    let Some(text) =
        get_text(&config.user, section, name).or_else(|| get_text(&config.defaults, section, name))
    else {
        return false;
    };
    text.eq_ignore_ascii_case("true") || parse_uint(Some(text)) != 0
}

/// S-007-R13 `config_get_double`: `os_strtod(text)` (63-character cut,
/// C-locale `strtod`); missing -> 0.0.
#[must_use]
pub fn config_get_double(config: &Config, section: &str, name: &str) -> f64 {
    os_strtod(
        get_text(&config.user, section, name)
            .or_else(|| get_text(&config.defaults, section, name))
            .unwrap_or(""),
    )
}

/// S-007-R13 defaults map only.
#[must_use]
pub fn config_get_default_int(config: &Config, section: &str, name: &str) -> i64 {
    parse_int(get_text(&config.defaults, section, name))
}

/// S-007-R13 defaults map only.
#[must_use]
pub fn config_get_default_uint(config: &Config, section: &str, name: &str) -> u64 {
    parse_uint(get_text(&config.defaults, section, name))
}

/// S-007-R13 defaults map only.
#[must_use]
pub fn config_get_default_bool(config: &Config, section: &str, name: &str) -> bool {
    let Some(text) = get_text(&config.defaults, section, name) else {
        return false;
    };
    text.eq_ignore_ascii_case("true") || parse_uint(Some(text)) != 0
}

/// S-007-R13 defaults map only.
#[must_use]
pub fn config_get_default_double(config: &Config, section: &str, name: &str) -> f64 {
    os_strtod(get_text(&config.defaults, section, name).unwrap_or(""))
}

// ---------------------------------------------------------------------------
// presence / removal (S-007 R14/R15)
// ---------------------------------------------------------------------------

/// S-007-R14 `config_remove_value`: removes the **user** item (not the
/// default); the section stays (possibly empty). True when removed,
/// false when the section or item is missing.
pub fn config_remove_value(config: &mut Config, section: &str, name: &str) -> bool {
    let Some((sec_idx, it_idx)) = find_entry(&config.user, section, name) else {
        return false;
    };
    config.user[sec_idx].items.remove(it_idx);
    true
}

/// S-007-R15: presence in the user map. Missing section/name never faults.
#[must_use]
pub fn config_has_user_value(config: &Config, section: &str, name: &str) -> bool {
    has_in(&config.user, section, name)
}

/// S-007-R15: presence in the defaults map.
#[must_use]
pub fn config_has_default_value(config: &Config, section: &str, name: &str) -> bool {
    has_in(&config.defaults, section, name)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn open(text: &str) -> Config {
        let mut out: Option<Box<Config>> = None;
        assert_eq!(config_open_string(&mut out, text), CONFIG_SUCCESS);
        *out.take().unwrap()
    }

    #[test]
    fn parse_basic_sections() {
        let c = open(
            "[General]\n\
             # a comment\n\
             Name=Bob\n\
             Count=3\n\
             \n\
             [Other]\n\
             Key=Value with spaces\n",
        );
        assert_eq!(config_num_sections(&c), 2);
        assert_eq!(config_get_section(&c, 0), Some("General"));
        assert_eq!(config_get_section(&c, 1), Some("Other"));
        assert_eq!(config_get_section(&c, 2), None);
        assert_eq!(config_get_string(&c, "General", "Name"), Some("Bob"));
        assert_eq!(
            config_get_string(&c, "Other", "Key"),
            Some("Value with spaces")
        );
        assert_eq!(config_get_string(&c, "General", "Missing"), None);
        assert_eq!(config_get_string(&c, "Nope", "Name"), None);
    }

    #[test]
    fn parse_top_level_junk_ignored() {
        let c = open("junk line before any section\n[Section]\nA=1\nmore junk\n");
        assert_eq!(config_num_sections(&c), 1);
        assert_eq!(config_get_string(&c, "Section", "A"), Some("1"));
    }

    #[test]
    fn parse_empty_and_bom_only() {
        assert_eq!(config_num_sections(&open("")), 0);
        assert_eq!(config_num_sections(&open("\u{feff}")), 0);
        assert_eq!(config_num_sections(&open("\u{feff}\n")), 0);
    }

    #[test]
    fn parse_whitespace_and_names() {
        // Q-4: names keep trailing whitespace, values keep all
        // surrounding whitespace; names may contain spaces
        let c = open("[S]\n  my key  =   spaced value  \nnoequals\n=a\n  =b\n");
        assert_eq!(
            config_get_string(&c, "S", "my key  "),
            Some("   spaced value  ")
        );
        assert!(config_get_string(&c, "S", "noequals").is_none());
        // a line whose name would start with `=` is ignored
        assert!(config_get_string(&c, "S", "").is_none());
    }

    #[test]
    fn parse_first_equals_wins() {
        let c = open("[S]\na=b=c\n");
        assert_eq!(config_get_string(&c, "S", "a"), Some("b=c"));
    }

    #[test]
    fn parse_q1_duplicates_last_wins() {
        // both duplicate sections and duplicate items stay listed, and
        // lookups return the last inserted one
        let c = open("[A]\nK=1\n[A]\nK=2\n");
        assert_eq!(config_num_sections(&c), 2);
        assert_eq!(config_get_string(&c, "A", "K"), Some("2"));
        let c = open("[S]\nK=first\nK=second\n");
        assert_eq!(config_num_sections(&c), 1);
        assert_eq!(config_get_string(&c, "S", "K"), Some("second"));
    }

    #[test]
    fn parse_q5_bad_header_still_creates_section() {
        // a header missing `]` still creates the section (Q-5)
        let c = open("[A\nK=1\n[B]\nL=2\n");
        assert_eq!(config_num_sections(&c), 2);
        assert_eq!(config_get_section(&c, 0), Some("A"));
        assert_eq!(config_get_string(&c, "A", "K"), Some("1"));
        assert_eq!(config_get_string(&c, "B", "L"), Some("2"));
    }

    #[test]
    fn parse_rd3_empty_name_stops_entirely() {
        // `[]` stops parsing; everything after is ignored (R-D3)
        let c = open("[S]\nK=1\n[]\n[After]\nL=2\n");
        assert_eq!(config_num_sections(&c), 1);
        assert_eq!(config_get_section(&c, 0), Some("S"));
    }

    #[test]
    fn parse_line_endings() {
        let lf = open("[S]\nA=1\nB=2\n");
        let crlf = open("[S]\r\nA=1\r\nB=2\r\n");
        let cr = open("[S]\rA=1\rB=2\r");
        for c in [&lf, &crlf, &cr] {
            assert_eq!(config_get_string(c, "S", "A"), Some("1"));
            assert_eq!(config_get_string(c, "S", "B"), Some("2"));
        }
    }

    #[test]
    fn parse_unescape() {
        let c = open(
            "[S]\n\
             back=a\\\\b\n\
             cr=x\\ry\n\
             lf=p\\nq\n\
             other=\\xz\n\
             lone=a\\\\\n\
             plain=none\n",
        );
        assert_eq!(config_get_string(&c, "S", "back"), Some("a\\b"));
        assert_eq!(config_get_string(&c, "S", "cr"), Some("x\ry"));
        assert_eq!(config_get_string(&c, "S", "lf"), Some("p\nq"));
        // other backslash sequences are kept literally (incl. backslash)
        assert_eq!(config_get_string(&c, "S", "other"), Some("\\xz"));
        // a trailing lone backslash is kept
        assert_eq!(config_get_string(&c, "S", "lone"), Some("a\\"));
        assert_eq!(config_get_string(&c, "S", "plain"), Some("none"));
    }

    #[test]
    fn parse_empty_value() {
        let c = open("[S]\nK=\n");
        assert_eq!(config_get_string(&c, "S", "K"), Some(""));
    }

    #[test]
    fn open_string_has_no_file() {
        let mut out: Option<Box<Config>> = None;
        assert_eq!(config_open_string(&mut out, "[S]\nA=1\n"), CONFIG_SUCCESS);
        let c = out.take().unwrap();
        // no file bound: config_save must fail (R04/Q-2, tested with save)
        assert!(c.file.is_none());
    }

    #[test]
    fn open_string_double_call_errors() {
        let mut out: Option<Box<Config>> = None;
        assert_eq!(config_open_string(&mut out, "[S]\n"), CONFIG_SUCCESS);
        assert_eq!(config_open_string(&mut out, "[T]\n"), CONFIG_ERROR);
    }

    #[test]
    #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
    fn open_defaults_accumulates() {
        // R03: defaults map only; repeated calls accumulate, later
        // definition wins
        let dir = std::env::temp_dir().join("obs_rust_config_defaults_test");
        std::fs::create_dir_all(&dir).unwrap();
        let f1 = dir.join("d1.ini");
        let f2 = dir.join("d2.ini");
        std::fs::write(&f1, "[S]\nA=one\nB=two\n").unwrap();
        std::fs::write(&f2, "[S]\nB=three\nC=four\n").unwrap();

        let mut out: Option<Box<Config>> = None;
        assert_eq!(
            config_open_string(&mut out, "[S]\nA=user\n"),
            CONFIG_SUCCESS
        );
        let mut c = out.take().unwrap();
        assert_eq!(config_open_defaults(&mut c, &f1), CONFIG_SUCCESS);
        assert_eq!(config_open_defaults(&mut c, &f2), CONFIG_SUCCESS);
        // defaults are readable but the user value wins
        assert_eq!(config_get_default_string(&c, "S", "A"), Some("one"));
        assert_eq!(config_get_default_string(&c, "S", "B"), Some("three"));
        assert_eq!(config_get_default_string(&c, "S", "C"), Some("four"));
        assert_eq!(config_get_string(&c, "S", "A"), Some("user"));
        assert_eq!(config_get_string(&c, "S", "B"), Some("three"));
        // open_defaults did not create user values
        assert!(!config_has_user_value(&c, "S", "B"));
        assert!(!config_has_user_value(&c, "S", "C"));
        // missing file
        assert_eq!(
            config_open_defaults(&mut c, &dir.join("missing.ini")),
            CONFIG_FILENOTFOUND
        );
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    #[cfg_attr(miri, ignore)] // file system access (Miri isolation)
    fn create_and_open_file_modes() {
        let dir = std::env::temp_dir().join("obs_rust_config_open_test");
        std::fs::create_dir_all(&dir).unwrap();
        let f = dir.join("c.ini");

        // R01: create/truncate
        let c = config_create(&f).unwrap();
        assert_eq!(config_num_sections(&c), 0);
        let c = config_create(&dir.join("nosuchdir/x.ini"));
        assert!(c.is_none());

        // R02: EXISTING missing -> FILENOTFOUND, config untouched
        let mut out: Option<Box<Config>> = None;
        assert_eq!(
            config_open(&mut out, &dir.join("missing.ini"), ConfigOpenType::Existing),
            CONFIG_FILENOTFOUND
        );
        assert!(out.is_none());

        // R02: ALWAYS missing -> created empty
        assert_eq!(
            config_open(&mut out, &f, ConfigOpenType::Always),
            CONFIG_SUCCESS
        );
        let c = out.take().unwrap();
        assert_eq!(config_num_sections(&c), 0);
        assert_eq!(fs::read_to_string(&f).unwrap(), "");

        // R02: existing file is parsed; BOM-only file -> zero sections
        std::fs::write(&f, "\u{feff}[S]\nA=1\n").unwrap();
        let mut out: Option<Box<Config>> = None;
        assert_eq!(
            config_open(&mut out, &f, ConfigOpenType::Existing),
            CONFIG_SUCCESS
        );
        let c = out.take().unwrap();
        assert_eq!(config_get_string(&c, "S", "A"), Some("1"));
        std::fs::remove_dir_all(&dir).unwrap();
    }

    #[test]
    fn set_creates_and_replaces_in_place() {
        // R07: creates section (appended) + item (appended); replace
        // keeps position
        let mut c = open("[First]\nA=1\n");
        config_set_string(&mut c, "Second", "X", Some("x"));
        config_set_string(&mut c, "First", "A", Some("replaced"));
        assert_eq!(config_num_sections(&c), 2);
        assert_eq!(config_get_section(&c, 0), Some("First"));
        assert_eq!(config_get_section(&c, 1), Some("Second"));
        assert_eq!(config_get_string(&c, "First", "A"), Some("replaced"));
        // None stores ""
        config_set_string(&mut c, "First", "A", None);
        assert_eq!(config_get_string(&c, "First", "A"), Some(""));
    }

    #[test]
    fn set_typed_formats() {
        // R08: %lld / %llu / true-false / os_dtostr (D-2-fixed)
        let mut c = open("");
        config_set_int(&mut c, "S", "i", -5);
        config_set_uint(&mut c, "S", "u", u64::MAX);
        config_set_bool(&mut c, "S", "b1", true);
        config_set_bool(&mut c, "S", "b0", false);
        config_set_double(&mut c, "S", "d1", 2.0);
        config_set_double(&mut c, "S", "d2", 0.1);
        config_set_double(&mut c, "S", "d3", -0.0);
        config_set_double(&mut c, "S", "d4", 1e21);
        assert_eq!(config_get_string(&c, "S", "i"), Some("-5"));
        assert_eq!(
            config_get_string(&c, "S", "u"),
            Some(u64::MAX.to_string().as_str())
        );
        assert_eq!(config_get_string(&c, "S", "b1"), Some("true"));
        assert_eq!(config_get_string(&c, "S", "b0"), Some("false"));
        assert_eq!(config_get_string(&c, "S", "d1"), Some("2.0"));
        assert_eq!(
            config_get_string(&c, "S", "d2"),
            Some("0.10000000000000001")
        );
        assert_eq!(config_get_string(&c, "S", "d3"), Some("-0.0"));
        // D-2 fixed: the reference would store the stale "1e211"
        assert_eq!(config_get_string(&c, "S", "d4"), Some("1e21"));
    }

    #[test]
    fn set_default_also_creates_user_value() {
        // R09: default set without a user value also creates the user
        // value (has_user_value becomes true); an existing user value
        // is left unchanged
        let mut c = open("[S]\nU=kept\n");
        config_set_default_string(&mut c, "S", "U", Some("default"));
        config_set_default_string(&mut c, "S", "V", Some("dv"));
        assert_eq!(config_get_string(&c, "S", "U"), Some("kept"));
        assert!(config_has_default_value(&c, "S", "U"));
        assert_eq!(config_get_string(&c, "S", "V"), Some("dv"));
        assert!(config_has_user_value(&c, "S", "V"));
        assert!(config_has_default_value(&c, "S", "V"));
        // %g six significant digits (Q-3)
        config_set_default_double(&mut c, "S", "D", 1234567.891);
        assert_eq!(config_get_default_string(&c, "S", "D"), Some("1.23457e+06"));
        // set_default_string(None) stores ""
        config_set_default_string(&mut c, "S", "W", None);
        assert_eq!(config_get_default_string(&c, "S", "W"), Some(""));
    }

    #[test]
    fn g_sig_worked_values() {
        // C printf("%g") semantics (precision 6), pinned
        assert_eq!(g_sig(1234567.891, 6), "1.23457e+06");
        assert_eq!(g_sig(0.1, 6), "0.1");
        assert_eq!(g_sig(2.0, 6), "2");
        assert_eq!(g_sig(100000.0, 6), "100000");
        assert_eq!(g_sig(1e21, 6), "1e+21");
        assert_eq!(g_sig(0.00123456789, 6), "0.00123457");
        assert_eq!(g_sig(999999.5, 6), "1e+06");
        assert_eq!(g_sig(f64::INFINITY, 6), "inf");
        assert_eq!(g_sig(f64::NAN, 6), "nan");
    }

    #[test]
    fn get_int_semantics() {
        // R11: empty/missing -> 0; 0x lowercase only; stop at invalid;
        // whitespace + sign; saturation
        let mut c = open("[S]");
        for (name, text, want) in [
            ("missing", "", 0i64),
            ("e1", "", 0),
            ("h1", "0x1F", 31),
            ("H1", "0X1F", 0), // uppercase X -> base 10 -> "0"
            ("s1", "12abc", 12),
            ("s2", "abc", 0),
            ("s3", "  -12", -12),
            ("s4", "+7", 7),
            ("s5", "0x", 0),
            ("s6", "-0x10", -16),
            ("big", "99999999999999999999", i64::MAX),
            ("negbig", "-99999999999999999999", i64::MIN),
        ] {
            if !text.is_empty() {
                config_set_string(&mut c, "S", name, Some(text));
            }
            assert_eq!(config_get_int(&c, "S", name), want, "{name}");
        }
    }

    #[test]
    fn get_uint_semantics() {
        // R11: negative wraps, overflow saturates
        let mut c = open("[S]");
        for (name, text, want) in [
            ("n1", "-5", u64::MAX - 4),
            ("n2", "-1", u64::MAX),
            ("n3", "5", 5u64),
            ("big", "99999999999999999999", u64::MAX),
            ("negbig", "-99999999999999999999", 0u64),
            ("hex", "0xFF", 255u64),
            ("missing", "", 0u64),
        ] {
            if !text.is_empty() {
                config_set_string(&mut c, "S", name, Some(text));
            }
            assert_eq!(config_get_uint(&c, "S", name), want, "{name}");
        }
    }

    #[test]
    fn get_bool_semantics() {
        // R12: "true" case-insensitively or nonzero uint
        let mut c = open("[S]");
        for (name, text, want) in [
            ("b1", "true", true),
            ("b2", "TRUE", true),
            ("b3", "True", true),
            ("b4", "1", true),
            ("b5", "2", true),
            ("b6", "0x1", true),
            ("b7", "yes", false),
            ("b8", "0", false),
            ("b9", "false", false),
            ("b10", "", false),
            ("b11", "", false), // missing -> false
        ] {
            if !text.is_empty() || name != "b11" {
                config_set_string(&mut c, "S", name, Some(text));
            }
            assert_eq!(config_get_bool(&c, "S", name), want, "{name}");
        }
    }

    #[test]
    fn get_double_semantics() {
        // R13: os_strtod; missing -> 0.0; unparsable -> 0.0
        let mut c = open("[S]");
        for (name, text, want) in [
            ("d1", "0x10", 16.0f64),
            ("d2", "abc", 0.0),
            ("d3", "", 0.0),
            ("d4", "1e400", f64::INFINITY),
        ] {
            if !text.is_empty() {
                config_set_string(&mut c, "S", name, Some(text));
            }
            assert_eq!(config_get_double(&c, "S", name), want, "{name}");
        }
        assert_eq!(config_get_double(&c, "S", "missing"), 0.0);
    }

    #[test]
    fn remove_value_user_only() {
        // R14: removes the user item, section stays; R15 presence
        let mut c = open("[S]\nA=1\nB=2\n");
        config_set_default_string(&mut c, "S", "A", Some("da"));
        assert!(config_has_user_value(&c, "S", "A"));
        assert!(config_has_default_value(&c, "S", "A"));
        assert!(config_remove_value(&mut c, "S", "A"));
        assert!(!config_has_user_value(&c, "S", "A"));
        // the default survives, so get falls back to it
        assert_eq!(config_get_string(&c, "S", "A"), Some("da"));
        // missing section or item -> false
        assert!(!config_remove_value(&mut c, "S", "A"));
        assert!(!config_remove_value(&mut c, "Nope", "A"));
        assert_eq!(config_num_sections(&c), 1);
        // R06: sections are never removed
        assert_eq!(config_get_section(&c, 0), Some("S"));
    }

    #[test]
    fn get_falls_back_to_defaults() {
        // R10: user value if present, else default, else NULL
        let mut c = open("[S]\nU=1\n");
        config_set_default_string(&mut c, "S", "U", Some("du"));
        config_set_default_string(&mut c, "S", "D", Some("dd"));
        assert_eq!(config_get_string(&c, "S", "U"), Some("1"));
        assert_eq!(config_get_string(&c, "S", "D"), Some("dd"));
        assert_eq!(config_get_string(&c, "S", "M"), None);
        assert_eq!(config_get_int(&c, "S", "M"), 0);
        assert!(!config_get_bool(&c, "S", "M"));
        assert_eq!(config_get_double(&c, "S", "M"), 0.0);
    }
}
