//! S-010 §5.7 (R60–R67): locale string lookup tables (text-lookup).
//!
//! Clean-room port of `libobs/util/text-lookup.c`: a case-sensitive byte
//! table of localized strings loaded from `Key="Value"` files. Tokenization
//! reuses the WP-20 base lexer ([`lexer::Lexer`]: `get_basetoken` with
//! PARSE_WHITESPACE, `set_offset` for the C push-backs); the file loop, the
//! Q-5/Q-6/Q-7 token quirks and the D-1 lone-quote handling follow S-010
//! §6.2/§6.3. The committed serial vectors are the oracle (house pattern).
//!
//! Keys and values are owned byte vectors (S-010 §9): either may hold
//! invalid UTF-8, so [`TextLookup::getstr`] returns `Option<&[u8]>` (never
//! `&str`). There is no fallback or miss default: a miss is `None` and a
//! locale fallback chain is caller-side (S-010 §1). `text_lookup_destroy`
//! has no Rust equivalent: `Drop` frees the table, and the NULL no-op is
//! meaningless for an owned value.
//!
//! D-1: the reference underflows the token length on a lone `"` followed by
//! end of text (OOB write; the vector marks it `ub`). Rust models that
//! token as empty: at end of text it stops parsing, keeping earlier
//! entries (S-010 §8: "treat like `B=\"\"`"). A lone `"` followed by LF
//! keeps the reference semantics (the first char checked is the LF).

use std::collections::HashMap;

use crate::lexer::{BaseToken, BaseTokenType, Lexer};

/// Case-sensitive locale string table (S-010-R60..R63).
pub struct TextLookup {
    items: HashMap<Vec<u8>, Vec<u8>>,
}

impl TextLookup {
    /// `text_lookup_create` (R60): a new empty table, then `add(path)`;
    /// `None` when the add fails (unreadable, empty or BOM-only file). A
    /// file with entries or only blank/comment lines succeeds.
    pub fn create(path: &[u8]) -> Option<Self> {
        let mut lookup = Self {
            items: HashMap::new(),
        };
        lookup.add(path).then_some(lookup)
    }

    /// `text_lookup_add` (R61): read `path`, preprocess (R64) and merge the
    /// entries into the table (duplicate keys are replaced, last wins within
    /// and across files). `false` when the file cannot be opened, is empty,
    /// or contains only a BOM (the table is then unchanged).
    pub fn add(&mut self, path: &[u8]) -> bool {
        let Some(data) = read_file_text(path) else {
            return false;
        };
        add_file_data(&mut self.items, &prepare(&data));
        true
    }

    /// `text_lookup_getstr` (R62): exact, case-sensitive byte lookup. The
    /// value is valid until the next `add` that replaces this key, or drop.
    /// A miss is `None` (no fallback; the caller chains the lookups).
    ///
    /// The returned slice excludes the stored NUL terminator (the C dstr
    /// invariant, kept so the FFI bridge can hand out a `%s`-safe pointer).
    pub fn getstr(&self, key: &[u8]) -> Option<&[u8]> {
        self.items.get(key).map(|v| &v[..v.len() - 1])
    }
}

/// `os_fopen` + `os_fread_utf8` (R61, R64): whole-file read, `None` on open
/// failure or an empty file; a leading UTF-8 BOM is stripped (a BOM-only
/// file is then empty and fails too).
fn read_file_text(path: &[u8]) -> Option<Vec<u8>> {
    // C paths are byte strings; keep them verbatim on unix.
    // shortcut: narrow Windows C paths are locale-modelled; lossy UTF-8
    // is the pragmatic mapping (upgrade if a Windows vector uses non-UTF-8).
    #[cfg(unix)]
    let p = {
        let os = <std::ffi::OsStr as std::os::unix::ffi::OsStrExt>::from_bytes(path);
        std::path::Path::new(os)
    };
    #[cfg(not(unix))]
    let p = std::path::Path::new(&std::ffi::OsString::from(
        String::from_utf8_lossy(path).into_owned(),
    ));
    let data = std::fs::read(p).ok()?;
    let text = if data.starts_with(b"\xEF\xBB\xBF") {
        &data[3..]
    } else {
        &data
    };
    (!text.is_empty()).then(|| text.to_vec())
}

/// `dstr_init_move_array` + `dstr_replace` (R64): the reference works on a
/// NUL-terminated buffer (bstrdup), so everything after the first internal
/// NUL never reaches the parser; every CR becomes a space.
fn prepare(text: &[u8]) -> Vec<u8> {
    let end = text.iter().position(|&b| b == 0).unwrap_or(text.len());
    text[..end]
        .iter()
        .map(|&b| if b == b'\r' { b' ' } else { b })
        .collect()
}

/// One token of `lookup_gettoken` (S-010 §6.2).
enum Tok<'a> {
    /// Token content: a maximal run, a single char, or a quoted body.
    Word(&'a [u8]),
    /// A lone `"` followed by LF or end of text. The reference underflows
    /// the token length here (D-1); the content is empty and the first char
    /// it checks is the byte after the quote (`None` at end of text).
    LoneQuote { next: Option<u8> },
}

fn first_char(t: &Tok<'_>) -> Option<u8> {
    match t {
        Tok::Word(w) => w.first().copied(),
        Tok::LoneQuote { next } => *next,
    }
}

/// `lookup_gettoken` (S-010 §6.2): one name/value token, `None` on failure
/// (end of text, or an empty quoted string — Q-7).
fn get_token(lex: &mut Lexer) -> Option<Tok<'_>> {
    let mut start: Option<usize> = None;
    let mut len = 0usize;
    loop {
        // (first byte, token length, is whitespace). The base token borrows
        // the lexer, so copy the data out before any `set_offset` push-back.
        let tok = {
            let mut t = BaseToken::new();
            if lex.get_basetoken(&mut t, false) {
                Some((
                    t.text.as_bytes()[0],
                    t.text.len(),
                    t.r#type == BaseTokenType::Whitespace,
                ))
            } else {
                None
            }
        };
        let Some((ch, tok_len, is_ws)) = tok else {
            // End of text: the word being built (if any) is complete.
            break start.map(|s| Tok::Word(&lex.text()[s..s + len]));
        };
        match start {
            None => {
                if ch == b'#' {
                    // Comment: skip to (not incl.) the next LF or end.
                    let off = lex.offset();
                    let end = lex.text()[off..]
                        .iter()
                        .position(|&b| b == b'\n')
                        .map_or(lex.text().len(), |p| off + p);
                    lex.set_offset(end);
                    continue;
                } else if is_ws {
                    break Some(Tok::Word(&lex.text()[lex.offset() - tok_len..lex.offset()]));
                } else if ch == b'"' {
                    break string_token(lex);
                } else if ch == b'=' {
                    break Some(Tok::Word(&lex.text()[lex.offset() - tok_len..lex.offset()]));
                } else {
                    start = Some(lex.offset() - tok_len);
                    len = tok_len;
                }
            }
            Some(s) => {
                // Extending a word: stop at whitespace or '=' (push the whole
                // token back) and at '#' (push back one byte); absorb the rest.
                if is_ws || ch == b'=' {
                    lex.set_offset(lex.offset() - tok_len);
                    break Some(Tok::Word(&lex.text()[s..s + len]));
                }
                if ch == b'#' {
                    lex.set_offset(lex.offset() - 1);
                    break Some(Tok::Word(&lex.text()[s..s + len]));
                }
                len += tok_len;
            }
        }
    }
}

/// `lookup_getstringtoken`: the opening quote is already consumed by the
/// base token; scan from right after it to NUL/LF (LF not consumed), `\`
/// escapes the next char, an unescaped `"` is consumed and ends the token.
fn string_token(lex: &mut Lexer) -> Option<Tok<'_>> {
    let off = lex.offset();
    let mut temp = off;
    let mut was_backslash = false;
    {
        let text = lex.text();
        while temp < text.len() && text[temp] != b'\n' {
            if was_backslash {
                was_backslash = false;
            } else if text[temp] == b'\\' {
                was_backslash = true;
            } else if text[temp] == b'"' {
                temp += 1;
                break;
            }
            temp += 1;
        }
    }
    lex.set_offset(temp);
    let text = lex.text();
    if temp == off {
        // D-1: the reference underflows the length here (OOB write). Rust:
        // empty token; the first char checked downstream is the byte after
        // the quote (LF, or none at end of text).
        return Some(Tok::LoneQuote {
            next: text.get(temp).copied(),
        });
    }
    let end = if text[temp - 1] == b'"' {
        temp - 1
    } else {
        temp
    };
    if end == off {
        // Q-7: `""` has content length 0, which the reference treats as a
        // token failure (ends parsing silently).
        return None;
    }
    Some(Tok::Word(&text[off..end]))
}

/// `lookup_goto_nextline`: consume the rest of the line; `false` when the
/// end of text is reached before an LF token (the caller stops parsing).
fn goto_nextline(lex: &mut Lexer) -> bool {
    while let Some(v) = get_token(lex) {
        // D-1: a lone `"` at end of text reads NUL (not LF) in the
        // reference, then the next token fails -> false. Same outcome.
        if matches!(&v, Tok::LoneQuote { next: None }) {
            return false;
        }
        if first_char(&v) == Some(b'\n') {
            return true;
        }
    }
    false
}

/// `lookup_addfiledata` (S-010 §6.3): the name/value file loop.
fn add_file_data(items: &mut HashMap<Vec<u8>, Vec<u8>>, text: &[u8]) {
    let mut lex = Lexer::new();
    lex.start(Some(text));
    while let Some(name) = get_token(&mut lex) {
        // D-1: a lone `"` at end of text behaves like the empty token (stop).
        if matches!(&name, Tok::LoneQuote { next: None }) {
            break;
        }
        if first_char(&name) == Some(b'\n') {
            continue; // blank line (incl. the LF of the previous line)
        }
        let mut got_eq = false;
        // Own the key before further lexing: a C strref is a raw pointer,
        // a Rust token view would outlive the next lexer call.
        let key = match name {
            Tok::Word(w) => w.to_vec(),
            Tok::LoneQuote { .. } => unreachable!("lone-quote names are handled above"),
        };
        let value = loop {
            match get_token(&mut lex) {
                // Token failure (incl. a D-1 lone quote at end of text):
                // stop parsing, the name is dropped.
                None => return,
                Some(Tok::LoneQuote { next: None }) => return,
                Some(v) => match first_char(&v) {
                    // A value whose first char is LF: the name has no value
                    // and is dropped; the next line starts.
                    Some(b'\n') => break None,
                    // Q-6: the first '=' after the name is a separator.
                    Some(b'=') if !got_eq => {
                        got_eq = true;
                        continue;
                    }
                    _ => break Some(v),
                },
            }
        };
        // Value failure or an LF-first value: the name is dropped.
        let Some(Tok::Word(val)) = value else {
            continue;
        };
        let mut val = convert_string(val);
        // C dstr invariant: stored values are NUL-terminated (getstr strips it).
        val.push(0);
        items.insert(key, val);
        if !goto_nextline(&mut lex) {
            break;
        }
    }
}

/// `convert_string` (R65): sequential unescape passes over the value only —
/// `\n` LF, `\t` TAB, `\r` CR, `\"` quote; each pass re-scans the previous
/// result, so `\\n` becomes backslash + LF. Other backslash sequences
/// (including `\\`) stay unchanged. Keys are never unescaped (R66).
fn convert_string(value: &[u8]) -> Vec<u8> {
    let mut out = value.to_vec();
    out = replace_seq(out, b"\\n", b"\n");
    out = replace_seq(out, b"\\t", b"\t");
    out = replace_seq(out, b"\\r", b"\r");
    out = replace_seq(out, b"\\\"", b"\"");
    out
}

/// One left-to-right, non-overlapping replace pass (dstr_replace: the
/// replacement itself is not re-scanned).
fn replace_seq(s: Vec<u8>, find: &[u8], repl: &[u8]) -> Vec<u8> {
    let mut out = Vec::with_capacity(s.len());
    let mut i = 0usize;
    while let Some(at) = s[i..].windows(find.len()).position(|w| w == find) {
        out.extend_from_slice(&s[i..i + at]);
        out.extend_from_slice(repl);
        i += at + find.len();
    }
    out.extend_from_slice(&s[i..]);
    out
}

#[cfg(test)]
mod tests {
    //! Oracle: the committed C reference vectors
    //! `test/conformance/{scripts,vectors}/serial/textlookup_*`, executed in
    //! the serial runner's print format (`gen/serial_runner.c` + `script.h`).

    use super::*;
    use std::fmt::Write as _;

    /// One vector case: the runner's four lookup slots plus a lazy temp dir
    /// (house pattern from the cf test modules: lazy mkdir on first write,
    /// forward-slash paths; Miri isolation does not permit fs, so the tests
    /// are `#[cfg(not(miri))]`).
    struct Case {
        slots: [Option<TextLookup>; 4],
        tmp: std::path::PathBuf,
    }

    static COUNTER: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);

    fn unique() -> usize {
        COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
    }

    impl Case {
        fn new() -> Self {
            let raw =
                std::env::temp_dir().join(format!("tlookup-{}-{}", std::process::id(), unique()));
            let tmp = std::path::PathBuf::from(raw.to_string_lossy().replace('\\', "/"));
            Self {
                slots: [None, None, None, None],
                tmp,
            }
        }

        fn writefile(&self, name: &str, content: &[u8]) {
            std::fs::create_dir_all(&self.tmp).unwrap();
            std::fs::write(self.tmp.join(name), content).unwrap();
        }

        /// File name for `create`/`add`: forward-slash form after the join
        /// (Path::join appends the native separator on Windows; the Windows
        /// file APIs accept '/').
        fn tmpfile(&self, name: &str) -> String {
            self.tmp.join(name).to_string_lossy().replace('\\', "/")
        }
    }

    /// `script_print_quoted` (gen/script.h): bytes between double quotes,
    /// escaping `\\` `\"` `\n` `\t` `\r` and non-printable / >= 0x7f bytes.
    fn pseg(bytes: &[u8]) -> String {
        let mut s = String::from("\"");
        for &b in bytes {
            match b {
                b'\\' => s.push_str("\\\\"),
                b'"' => s.push_str("\\\""),
                b'\n' => s.push_str("\\n"),
                b'\t' => s.push_str("\\t"),
                b'\r' => s.push_str("\\r"),
                0x20..=0x7e => s.push(b as char),
                _ => {
                    let _ = write!(s, "\\x{b:02x}");
                }
            }
        }
        s.push('"');
        s
    }

    fn hexval(c: Option<u8>) -> Option<u8> {
        match c? {
            c @ b'0'..=b'9' => Some(c - b'0'),
            c @ b'a'..=b'f' => Some(c - b'a' + 10),
            c @ b'A'..=b'F' => Some(c - b'A' + 10),
            _ => None,
        }
    }

    /// `script_tokenize` (gen/script.h): space/tab separated tokens, `#`
    /// starts a comment, quoted tokens carry C escapes (`\xHH`, any other
    /// escape = the escaped char itself).
    fn tokenize(line: &str) -> Vec<Vec<u8>> {
        let mut toks = Vec::new();
        let b = line.as_bytes();
        let mut i = 0usize;
        while i < b.len() {
            while i < b.len() && matches!(b[i], b' ' | b'\t' | b'\r' | b'\n') {
                i += 1;
            }
            if i >= b.len() || b[i] == b'#' {
                break;
            }
            let mut tok = Vec::new();
            if b[i] == b'"' {
                i += 1;
                while i < b.len() && b[i] != b'"' {
                    if b[i] == b'\\' && i + 1 < b.len() {
                        i += 1;
                        match b[i] {
                            b'n' => tok.push(b'\n'),
                            b't' => tok.push(b'\t'),
                            b'r' => tok.push(b'\r'),
                            b'x' => {
                                match (hexval(b.get(i + 1).copied()), hexval(b.get(i + 2).copied()))
                                {
                                    (Some(h), Some(l)) => {
                                        tok.push(h * 16 + l);
                                        i += 2;
                                    }
                                    _ => panic!("script: bad \\x"),
                                }
                            }
                            c => tok.push(c),
                        }
                    } else {
                        tok.push(b[i]);
                    }
                    i += 1;
                }
                if i >= b.len() {
                    panic!("script: unterminated string");
                }
                i += 1;
            } else {
                while i < b.len() && !matches!(b[i], b' ' | b'\t' | b'\r' | b'\n') {
                    tok.push(b[i]);
                    i += 1;
                }
            }
            toks.push(tok);
        }
        toks
    }

    /// `script_parse_hex` (gen/script.h): "-" is empty.
    fn parse_hex(s: &[u8]) -> Vec<u8> {
        if s == b"-" {
            return Vec::new();
        }
        let mut out = Vec::with_capacity(s.len() / 2);
        let mut i = 0usize;
        while i + 1 < s.len() {
            let h = hexval(Some(s[i])).unwrap();
            let l = hexval(Some(s[i + 1])).unwrap();
            out.push(h * 16 + l);
            i += 2;
        }
        out
    }

    fn str_of(t: &[u8]) -> &str {
        std::str::from_utf8(t).unwrap()
    }

    fn slot_of(t: &[u8]) -> usize {
        str_of(t).parse::<usize>().unwrap() & 3
    }

    fn tl_line(out: &mut String, cmd: &str, slot: usize, name: &[u8], r: &str) {
        let _ = write!(out, "{cmd} {slot} ");
        out.push_str(str_of(name));
        let _ = writeln!(out, " -> {r}");
    }

    /// Runs a committed serial script against the Rust table and returns the
    /// runner's stdout (the lines the C runner prints for the same script).
    fn run(case: &mut Case, script: &str) -> String {
        let mut out = String::new();
        for line in script.lines() {
            let toks = tokenize(line);
            if toks.is_empty() {
                continue;
            }
            match str_of(&toks[0]) {
                "fput" => case.writefile(str_of(&toks[1]), &parse_hex(&toks[2])),
                "fputs" => {
                    // The C runner writes strlen(argv) bytes (a NUL truncates).
                    let text = toks[2]
                        .iter()
                        .position(|&b| b == 0)
                        .map_or(&toks[2][..], |p| &toks[2][..p]);
                    case.writefile(str_of(&toks[1]), text);
                }
                "funlink" => {
                    let _ = std::fs::remove_file(case.tmp.join(str_of(&toks[1])));
                }
                "tl_create" => {
                    let slot = slot_of(&toks[1]);
                    let lookup = TextLookup::create(case.tmpfile(str_of(&toks[2])).as_bytes());
                    let ok = lookup.is_some();
                    case.slots[slot] = lookup;
                    tl_line(
                        &mut out,
                        "tl_create",
                        slot,
                        &toks[2],
                        if ok { "ok" } else { "null" },
                    );
                }
                "tl_add" => {
                    let slot = slot_of(&toks[1]);
                    let file = case.tmpfile(str_of(&toks[2]));
                    let ok = match &mut case.slots[slot] {
                        Some(l) => l.add(file.as_bytes()),
                        // No table: the C reference would crash on a readable
                        // file (NULL deref); the vectors never add to a NULL
                        // slot, so report a safe miss instead.
                        None => false,
                    };
                    tl_line(
                        &mut out,
                        "tl_add",
                        slot,
                        &toks[2],
                        if ok { "true" } else { "false" },
                    );
                }
                "tl_get" => {
                    let slot = slot_of(&toks[1]);
                    let key = &toks[2];
                    let _ = write!(out, "tl_get {slot} {}", pseg(key));
                    match case.slots[slot].as_ref().and_then(|l| l.getstr(key)) {
                        Some(v) => {
                            let _ = writeln!(out, " -> {}", pseg(v));
                        }
                        None => out.push_str(" -> false\n"),
                    }
                }
                "tl_destroy" => {
                    case.slots[slot_of(&toks[1])] = None;
                }
                "tl_defect" => {
                    // A conforming runner loads the defect file itself (the
                    // reference loads the equivalent file and prints "ub").
                    let slot = slot_of(&toks[1]);
                    let lookup = TextLookup::create(case.tmpfile(str_of(&toks[2])).as_bytes());
                    let ok = lookup.is_some();
                    case.slots[slot] = lookup;
                    tl_line(
                        &mut out,
                        "tl_defect",
                        slot,
                        &toks[2],
                        if ok { "ok" } else { "null" },
                    );
                }
                other => panic!("unknown script command {other:?}"),
            }
        }
        out
    }

    fn check_vector(script: &str, expected: &str) {
        let mut c = Case::new();
        let out = run(&mut c, script);
        assert_eq!(out, expected, "serial output mismatch");
    }

    #[test]
    #[cfg(not(miri))]
    fn v_tl_basic() {
        check_vector(
            include_str!("../../../test/conformance/scripts/serial/textlookup_basic.script"),
            include_str!("../../../test/conformance/vectors/serial/textlookup_basic.expected"),
        );
    }

    #[test]
    #[cfg(not(miri))]
    fn v_tl_files() {
        check_vector(
            include_str!("../../../test/conformance/scripts/serial/textlookup_files.script"),
            include_str!("../../../test/conformance/vectors/serial/textlookup_files.expected"),
        );
    }

    #[test]
    #[cfg(not(miri))]
    fn v_tl_quirk_equals() {
        check_vector(
            include_str!("../../../test/conformance/scripts/serial/textlookup_quirk_equals.script"),
            include_str!(
                "../../../test/conformance/vectors/serial/textlookup_quirk_equals.expected"
            ),
        );
    }

    #[test]
    #[cfg(not(miri))]
    fn v_tl_quirk_whitespace() {
        check_vector(
            include_str!(
                "../../../test/conformance/scripts/serial/textlookup_quirk_whitespace.script"
            ),
            include_str!(
                "../../../test/conformance/vectors/serial/textlookup_quirk_whitespace.expected"
            ),
        );
    }

    #[test]
    #[cfg(not(miri))]
    fn v_tl_quirk_empty_string() {
        check_vector(
            include_str!(
                "../../../test/conformance/scripts/serial/textlookup_quirk_empty_string.script"
            ),
            include_str!(
                "../../../test/conformance/vectors/serial/textlookup_quirk_empty_string.expected"
            ),
        );
    }

    #[test]
    #[cfg(not(miri))]
    fn v_tl_defect_lone_quote() {
        // D-1: the C reference underflows the token length on the defect
        // file (`B="` + EOF) — the expected file marks those lines `-> ub`
        // (the runner printed it after loading the equivalent file instead).
        // The required Rust behaviour is "same as B=\"\"" (S-010 §8): no
        // panic, create succeeds, earlier entries survive, the lone-quote
        // entry is a miss. The conforming runner loads the defect file
        // itself and prints `-> ok`, so compare the non-`ub` expected lines
        // against the harness output minus the `tl_defect` lines (documented
        // deviation, see docs/tests/text-lookup.md).
        let mut c = Case::new();
        let out = run(
            &mut c,
            include_str!(
                "../../../test/conformance/scripts/serial/textlookup_defect_lone_quote.script"
            ),
        );
        // create succeeded for both defect files (no panic, no null)
        let defect: Vec<&str> = out
            .lines()
            .filter(|l| l.starts_with("tl_defect "))
            .collect();
        assert_eq!(
            defect,
            vec!["tl_defect 0 td-1 -> ok", "tl_defect 0 td-2 -> ok"]
        );
        let got: Vec<&str> = out
            .lines()
            .filter(|l| !l.starts_with("tl_defect "))
            .collect();
        let want: Vec<&str> = include_str!(
            "../../../test/conformance/vectors/serial/textlookup_defect_lone_quote.expected"
        )
        .lines()
        .filter(|l| !l.ends_with(" -> ub"))
        .collect();
        assert_eq!(got, want);
    }

    #[test]
    #[cfg(not(miri))]
    fn v_tl_escapes() {
        check_vector(
            include_str!("../../../test/conformance/scripts/serial/textlookup_escapes.script"),
            include_str!("../../../test/conformance/vectors/serial/textlookup_escapes.expected"),
        );
    }

    #[test]
    #[cfg(not(miri))]
    fn v_tl_tokens() {
        check_vector(
            include_str!("../../../test/conformance/scripts/serial/textlookup_tokens.script"),
            include_str!("../../../test/conformance/vectors/serial/textlookup_tokens.expected"),
        );
    }

    #[test]
    #[cfg(not(miri))]
    fn v_tl_unterminated() {
        check_vector(
            include_str!("../../../test/conformance/scripts/serial/textlookup_unterminated.script"),
            include_str!(
                "../../../test/conformance/vectors/serial/textlookup_unterminated.expected"
            ),
        );
    }
}
