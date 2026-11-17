//! S-011 §5.1 (R01–R12): the base lexer and its helpers.
//!
//! Clean-room port of the P1-08.2 slice of `libobs/util/lexer.c`/`lexer.h`:
//! the base token scanner, the `strref` comparison helpers, the number
//! validators, `error_data` and the row/column lookup. The CF lexer,
//! preprocessor and parser (R20–R80) arrive with P1-08.3/P1-08.4.
//!
//! Safe Rust (no raw pointers, per the crate policy): `StrRef<'a>` borrows
//! the buffer it references; its `len` is trusted exactly like in C and may
//! after [`StrRef::add`] extend beyond the visible buffer (callers must not
//! dereference past it; the helpers here never do).

/// `LEX_ERROR` (R12).
pub const LEX_ERROR: i32 = 0;
/// `LEX_WARNING` (R12).
pub const LEX_WARNING: i32 = 1;

/// `enum base_token_type` (R02).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Default)]
pub enum BaseTokenType {
    #[default]
    None,
    Alpha,
    Digit,
    Whitespace,
    Other,
}

/// `struct strref` (R06–R09): a span inside an existing byte buffer.
#[derive(Clone, Copy, Debug, Default)]
pub struct StrRef<'a> {
    array: Option<&'a [u8]>,
    len: usize,
}

impl<'a> StrRef<'a> {
    /// `strref_set`.
    pub fn set(array: &'a [u8], len: usize) -> Self {
        Self {
            array: Some(array),
            len,
        }
    }

    /// `strref_clear`.
    pub fn clear() -> Self {
        Self {
            array: None,
            len: 0,
        }
    }

    /// `strref_add` (R07): if `self` has no array copy `t`, else grow the
    /// length (no adjacency check).
    pub fn add(&mut self, t: &Self) {
        if self.array.is_none() {
            *self = *t;
        } else {
            self.len += t.len;
        }
    }

    /// `strref_is_empty` (R06): NULL array, zero length, or a NUL first byte.
    pub fn is_empty(&self) -> bool {
        self.array
            .is_none_or(|a| self.len == 0 || a.is_empty() || a[0] == 0)
    }

    /// The trusted span length (may exceed the visible buffer after `add`).
    pub fn len(&self) -> usize {
        self.len
    }

    /// The underlying buffer (the span's base, not clamped).
    pub fn array(&self) -> Option<&'a [u8]> {
        self.array
    }

    /// The referenced bytes, clamped to the buffer.
    pub fn as_bytes(&self) -> &'a [u8] {
        match self.array {
            Some(a) => &a[..self.len.min(a.len())],
            None => &[],
        }
    }
}

/// `is_whitespace` (R05).
pub fn is_whitespace(ch: u8) -> bool {
    matches!(ch, b' ' | b'\r' | b'\t' | b'\n')
}

/// `is_newline` (R05).
pub fn is_newline(ch: u8) -> bool {
    matches!(ch, b'\r' | b'\n')
}

/// `is_space_or_tab` (R05).
pub fn is_space_or_tab(ch: u8) -> bool {
    matches!(ch, b' ' | b'\t')
}

/// `is_newline_pair` (R05): true for `\r\n` and `\n\r` only.
pub fn is_newline_pair(ch1: u8, ch2: u8) -> bool {
    (ch1 == b'\r' && ch2 == b'\n') || (ch1 == b'\n' && ch2 == b'\r')
}

/// `newline_size` (R05): 2 for a `\r\n`/`\n\r` pair, 1 for a single newline
/// character, 0 otherwise.
pub fn newline_size(array: &[u8]) -> u32 {
    if array.len() >= 2 && is_newline_pair(array[0], array[1]) {
        2
    } else if array.first().is_some_and(|&c| is_newline(c)) {
        1
    } else {
        0
    }
}

/// ASCII-uppercase fold with C `toupper` semantics on the signed byte
/// (values outside `a..=z` pass through unchanged, incl. bytes >= 0x80).
fn fold_upper(c: i8) -> i8 {
    let u = c as u8;
    if u.is_ascii_lowercase() { c - 32 } else { c }
}

/// `strref_cmp` (R08): compare the strref span (NUL-padded) with a C string
/// byte by byte using signed-char semantics. `None` = NULL (the empty
/// string). Only the sign is normative.
pub fn strref_cmp(str1: &StrRef, str2: Option<&[u8]>) -> i32 {
    if str1.is_empty() {
        return if str2.is_none_or(|b| b.is_empty() || b[0] == 0) {
            0
        } else {
            -1
        };
    }
    cmp_pair(str1, str2.unwrap_or(&[]), false)
}

/// `strref_cmpi` (R08): case-insensitive variant.
pub fn strref_cmpi(str1: &StrRef, str2: Option<&[u8]>) -> i32 {
    if str1.is_empty() {
        return if str2.is_none_or(|b| b.is_empty() || b[0] == 0) {
            0
        } else {
            -1
        };
    }
    cmp_pair(str1, str2.unwrap_or(&[]), true)
}

fn cmp_pair(str1: &StrRef, str2: &[u8], ignore_case: bool) -> i32 {
    let a = str1.as_bytes();
    let mut i = 0;
    let mut j = 0;
    loop {
        let ch1: i8 = *a.get(i).unwrap_or(&0) as i8;
        let ch2: i8 = *str2.get(j).unwrap_or(&0) as i8;
        let (c1, c2) = if ignore_case {
            (fold_upper(ch1), fold_upper(ch2))
        } else {
            (ch1, ch2)
        };
        if c1 < c2 {
            return -1;
        }
        if c1 > c2 {
            return 1;
        }
        // while (i++ < str1->len && *str2++)
        let left = i < str1.len();
        i += 1;
        if !left {
            break;
        }
        let c = *str2.get(j).unwrap_or(&0);
        j += 1;
        if c == 0 {
            break;
        }
    }
    0
}

/// `strref_cmp_strref` (R09): span vs span. Q-1: a non-empty `str1` against
/// an empty `str2` returns -1.
pub fn strref_cmp_strref(str1: &StrRef, str2: &StrRef) -> i32 {
    if str1.is_empty() {
        return if str2.is_empty() { 0 } else { -1 };
    }
    if str2.is_empty() {
        return -1;
    }
    cmp_span(str1, str2, false)
}

/// `strref_cmpi_strref` (R09): case-insensitive variant.
pub fn strref_cmpi_strref(str1: &StrRef, str2: &StrRef) -> i32 {
    if str1.is_empty() {
        return if str2.is_empty() { 0 } else { -1 };
    }
    if str2.is_empty() {
        return -1;
    }
    cmp_span(str1, str2, true)
}

fn cmp_span(str1: &StrRef, str2: &StrRef, ignore_case: bool) -> i32 {
    let a = str1.as_bytes();
    let b = str2.as_bytes();
    let mut i = 0;
    loop {
        let ch1: i8 = *a.get(i).unwrap_or(&0) as i8;
        let ch2: i8 = *b.get(i).unwrap_or(&0) as i8;
        let (c1, c2) = if ignore_case {
            (fold_upper(ch1), fold_upper(ch2))
        } else {
            (ch1, ch2)
        };
        if c1 < c2 {
            return -1;
        }
        if c1 > c2 {
            return 1;
        }
        i += 1;
        // while (i <= str1->len && i <= str2->len)
        if !(i <= str1.len() && i <= str2.len()) {
            break;
        }
    }
    0
}

/// `valid_int_str` (R10): an optional leading `+`/`-`, then at most `n`
/// digits (`n == 0` = the whole NUL-terminated string; `n` counts
/// characters after the sign); at least one digit required.
pub fn valid_int_str(str: Option<&[u8]>, n: usize) -> bool {
    let s = match str {
        Some(s) if !s.is_empty() && s[0] != 0 => s,
        _ => return false,
    };
    let mut n = if n == 0 {
        s.iter().position(|&b| b == 0).unwrap_or(s.len())
    } else {
        n
    };
    let mut p = usize::from(s[0] == b'-' || s[0] == b'+');
    // C initializes found_num to false; the do-while always inspects the
    // first char and either returns or sets it, so the initial value is
    // never read.
    #[allow(unused_assignments)]
    let mut found_num = false;
    loop {
        let c = *s.get(p).unwrap_or(&0);
        if !c.is_ascii_digit() {
            return false;
        }
        found_num = true;
        p += 1;
        if *s.get(p).unwrap_or(&0) == 0 {
            break;
        }
        n -= 1;
        if n == 0 {
            break;
        }
    }
    found_num
}

/// `valid_float_str` (R11, Q-2): as `valid_int_str` plus `.` (once, before
/// any `e`, after a digit), lowercase `e` (once, after a digit, a digit
/// must follow) and `+`/`-` only after `e` **and** a digit — so `1e+5`/
/// `1e-5` are rejected while `1e5+3`/`1e5-3` are accepted.
pub fn valid_float_str(str: Option<&[u8]>, n: usize) -> bool {
    let s = match str {
        Some(s) if !s.is_empty() && s[0] != 0 => s,
        _ => return false,
    };
    let mut n = if n == 0 {
        s.iter().position(|&b| b == 0).unwrap_or(s.len())
    } else {
        n
    };
    let mut p = usize::from(s[0] == b'-' || s[0] == b'+');
    let mut found_num = false;
    let mut found_exp = false;
    let mut found_dec = false;
    loop {
        let c = *s.get(p).unwrap_or(&0);
        if c == b'.' {
            if found_dec || found_exp || !found_num {
                return false;
            }
            found_dec = true;
        } else if c == b'e' {
            if found_exp || !found_num {
                return false;
            }
            found_exp = true;
            found_num = false;
        } else if c == b'-' || c == b'+' {
            if !found_exp || !found_num {
                return false;
            }
        } else if !c.is_ascii_digit() {
            return false;
        } else {
            found_num = true;
        }
        p += 1;
        if *s.get(p).unwrap_or(&0) == 0 {
            break;
        }
        n -= 1;
        if n == 0 {
            break;
        }
    }
    found_num
}

/// `struct base_token` (R03). `passed_whitespace` is never set by the base
/// lexer (the CF lexer uses it).
#[derive(Clone, Copy, Debug, Default)]
pub struct BaseToken<'a> {
    pub text: StrRef<'a>,
    pub r#type: BaseTokenType,
    pub passed_whitespace: bool,
}

impl<'a> BaseToken<'a> {
    /// `base_token_clear`.
    pub fn new() -> Self {
        Self {
            text: StrRef::clear(),
            r#type: BaseTokenType::None,
            passed_whitespace: false,
        }
    }
}

/// `struct lexer` (R01): an owned copy (or moved) text buffer plus the scan
/// offset.
#[derive(Clone, Debug, Default)]
pub struct Lexer {
    text: Vec<u8>,
    offset: usize,
}

impl Lexer {
    /// `lexer_init`.
    pub fn new() -> Self {
        Self::default()
    }

    /// `lexer_start` (R01): duplicate the text; `None` gives an empty
    /// lexer. Starting again frees the old text.
    pub fn start(&mut self, text: Option<&[u8]>) {
        self.text = text.map(|t| t.to_vec()).unwrap_or_default();
        self.offset = 0;
    }

    /// `lexer_start_move` (R01): take ownership of the buffer.
    pub fn start_move(&mut self, text: Vec<u8>) {
        self.text = text;
        self.offset = 0;
    }

    /// `lexer_reset` (R01).
    pub fn reset(&mut self) {
        self.offset = 0;
    }

    /// `lexer_free` (R01).
    pub fn free(&mut self) {
        self.text.clear();
        self.offset = 0;
    }

    /// The owned text (C `lex->text`).
    pub fn text(&self) -> &[u8] {
        &self.text
    }

    /// The current scan offset (C `lex->offset`).
    pub fn offset(&self) -> usize {
        self.offset
    }

    /// Set the scan cursor (C `lex->offset` assignment, used by the
    /// cf-lexer layer for splice skips and break rewinds).
    pub fn set_offset(&mut self, off: usize) {
        self.offset = off;
    }

    /// `lexer_getbasetoken` (R03): produce the next base token. ALPHA/DIGIT
    /// tokens extend over the maximal same-class run; WHITESPACE and OTHER
    /// tokens are one character, except a `\r\n`/`\n\r` pair is a single
    /// two-character WHITESPACE token. With `ignore_whitespace`, whitespace
    /// before a token is skipped (the offset still advances past it).
    /// Returns false at the end of the text; the token is unmodified.
    pub fn get_basetoken<'a, 'b>(
        &'b mut self,
        token: &mut BaseToken<'a>,
        ignore_whitespace: bool,
    ) -> bool
    where
        'b: 'a,
    {
        let text = self.text.as_slice();
        let mut offset = self.offset;
        let mut token_start: Option<usize> = None;
        let mut type_ = BaseTokenType::None;

        while offset < text.len() {
            let ch = text[offset];
            offset += 1;
            let new_type = match ch {
                c if is_whitespace(c) => BaseTokenType::Whitespace,
                b'0'..=b'9' => BaseTokenType::Digit,
                b'a'..=b'z' | b'A'..=b'Z' => BaseTokenType::Alpha,
                _ => BaseTokenType::Other,
            };

            if type_ == BaseTokenType::None {
                if new_type == BaseTokenType::Whitespace && ignore_whitespace {
                    continue;
                }
                token_start = Some(offset - 1);
                type_ = new_type;
                if !matches!(type_, BaseTokenType::Digit | BaseTokenType::Alpha) {
                    if is_newline(ch) && is_newline_pair(ch, *text.get(offset).unwrap_or(&0)) {
                        offset += 1;
                    }
                    break;
                }
            } else if type_ != new_type {
                offset -= 1;
                break;
            }
        }

        self.offset = offset;

        if let Some(start) = token_start.filter(|&s| offset > s) {
            token.text = StrRef::set(&self.text[start..], offset - start);
            token.r#type = type_;
            return true;
        }
        false
    }

    /// `lexer_getstroffset` (R04): 1-based row/column of a position inside
    /// the text; a `\r\n`/`\n\r` pair is one line break. `None` (a NULL C
    /// pointer) leaves both outputs untouched.
    pub fn get_stroffset(&self, pos: Option<usize>, row: &mut u32, col: &mut u32) {
        let Some(pos) = pos else {
            return;
        };
        let text = &self.text;
        let mut i = 0;
        let mut cur_col: u32 = 1;
        let mut cur_row: u32 = 1;
        while i < pos {
            if is_newline(text[i]) {
                i += newline_size(&text[i..]) as usize - 1;
                cur_col = 1;
                cur_row += 1;
            } else {
                cur_col += 1;
            }
            i += 1;
        }
        *row = cur_row;
        *col = cur_col;
    }
}

/// `struct error_item` + `struct error_data` (R12).
#[derive(Clone, Debug)]
pub struct ErrorItem {
    /// A copy of the file name (R12 holds the C pointer; the Rust item
    /// copies it so error lists are self-contained); `None` = NULL (C
    /// prints implementation-defined text there — Rust prints nothing; do
    /// not rely on it).
    pub file: Option<String>,
    pub row: u32,
    pub column: u32,
    pub level: i32,
    /// A copy of the message (C `bstrdup`).
    pub msg: String,
}

#[derive(Clone, Debug, Default)]
pub struct ErrorData {
    items: Vec<ErrorItem>,
}

impl ErrorData {
    /// `error_data_init`.
    pub fn new() -> Self {
        Self::default()
    }

    /// `error_data_add` (R12). (Adding to a NULL `error_data` is a C no-op
    /// with no Rust equivalent.)
    pub fn add(&mut self, file: Option<&str>, row: u32, column: u32, msg: &str, level: i32) {
        self.items.push(ErrorItem {
            file: file.map(str::to_owned),
            row,
            column,
            level,
            msg: msg.to_owned(),
        });
    }

    /// `error_data_item`.
    pub fn item(&self, idx: usize) -> Option<&ErrorItem> {
        self.items.get(idx)
    }

    /// The item count (C `ed->errors.num`).
    pub fn len(&self) -> usize {
        self.items.len()
    }

    pub fn is_empty(&self) -> bool {
        self.items.is_empty()
    }

    /// `error_data_type_count`.
    pub fn type_count(&self, level: i32) -> usize {
        self.items.iter().filter(|it| it.level == level).count()
    }

    /// `error_data_has_errors`: any item with level `LEX_ERROR`.
    pub fn has_errors(&self) -> bool {
        self.items.iter().any(|it| it.level == LEX_ERROR)
    }

    /// `error_data_free`.
    pub fn free(&mut self) {
        self.items.clear();
    }

    /// `error_data_buildstring` (R12): per item,
    /// `"<file> (<row>, <col>): <msg>\n"`; `None` when there are no items.
    pub fn buildstring(&self) -> Option<String> {
        if self.items.is_empty() {
            return None;
        }
        let mut s = String::new();
        for it in &self.items {
            s.push_str(it.file.as_deref().unwrap_or(""));
            s.push_str(&format!(" ({}, {}): {}\n", it.row, it.column, it.msg));
        }
        Some(s)
    }
}

#[cfg(test)]
mod tests {
    //! Oracle: the committed C reference vectors
    //! `test/conformance/vectors/lexer/01..06` (generated from
    //! `libobs/util/lexer.c`).

    use super::*;

    fn tref<'a>(buf: &'a [u8], off: usize, len: usize) -> StrRef<'a> {
        StrRef::set(&buf[off..], len)
    }

    fn off_of(buf: &[u8], r: &StrRef) -> usize {
        r.array()
            .map(|a| a.as_ptr() as usize - buf.as_ptr() as usize)
            .unwrap_or(0)
    }

    fn drain(lex: &mut Lexer, ignore: bool) -> Vec<(BaseTokenType, Vec<u8>)> {
        let mut out = Vec::new();
        while let Some((ty, bytes)) = {
            let mut t = BaseToken::new();
            if lex.get_basetoken(&mut t, ignore) {
                Some((t.r#type, t.text.as_bytes().to_vec()))
            } else {
                None
            }
        } {
            out.push((ty, bytes));
        }
        out
    }

    // 01-base-tokens
    #[test]
    fn v01_base_tokens() {
        let mut lex = Lexer::new();
        lex.start(Some(b"ab_1 \t\r\n\n\r\r\n\n\n x\xc3\xa9".as_slice()));
        let got = drain(&mut lex, false);
        let expected: Vec<(BaseTokenType, Vec<u8>)> = vec![
            (BaseTokenType::Alpha, b"ab".to_vec()),
            (BaseTokenType::Other, b"_".to_vec()),
            (BaseTokenType::Digit, b"1".to_vec()),
            (BaseTokenType::Whitespace, b" ".to_vec()),
            (BaseTokenType::Whitespace, b"\t".to_vec()),
            (BaseTokenType::Whitespace, b"\r\n".to_vec()),
            (BaseTokenType::Whitespace, b"\n\r".to_vec()),
            (BaseTokenType::Whitespace, b"\r\n".to_vec()),
            (BaseTokenType::Whitespace, b"\n".to_vec()),
            (BaseTokenType::Whitespace, b"\n".to_vec()),
            (BaseTokenType::Whitespace, b" ".to_vec()),
            (BaseTokenType::Alpha, b"x".to_vec()),
            (BaseTokenType::Other, vec![0xc3]),
            (BaseTokenType::Other, vec![0xa9]),
        ];
        assert_eq!(got, expected);
        assert_eq!(lex.offset(), 18);

        // lreset; lall ignore
        lex.reset();
        let got = drain(&mut lex, true);
        assert_eq!(
            got,
            vec![
                (BaseTokenType::Alpha, b"ab".to_vec()),
                (BaseTokenType::Other, b"_".to_vec()),
                (BaseTokenType::Digit, b"1".to_vec()),
                (BaseTokenType::Alpha, b"x".to_vec()),
                (BaseTokenType::Other, vec![0xc3]),
                (BaseTokenType::Other, vec![0xa9]),
            ]
        );
        assert_eq!(lex.offset(), 18);

        // lstart "   "; lall ignore -> none, offset 3
        lex.start(Some(b"   "));
        assert!(drain(&mut lex, true).is_empty());
        assert_eq!(lex.offset(), 3);

        // lstart ""; lall -> none, offset 0
        lex.start(Some(b""));
        assert!(drain(&mut lex, false).is_empty());
        assert_eq!(lex.offset(), 0);

        // lstart @null; lall -> none
        lex.start(None);
        assert!(drain(&mut lex, false).is_empty());
        assert_eq!(lex.offset(), 0);

        // lstart "a\x01b"
        lex.start(Some(b"a\x01b".as_slice()));
        assert_eq!(
            drain(&mut lex, false),
            vec![
                (BaseTokenType::Alpha, b"a".to_vec()),
                (BaseTokenType::Other, vec![0x01]),
                (BaseTokenType::Alpha, b"b".to_vec()),
            ]
        );

        // lstart_move "x y"; lnext x3 + none; lreset; lnext ignore; lfree
        lex.start_move(b"x y".to_vec());
        assert_eq!(
            drain(&mut lex, false),
            vec![
                (BaseTokenType::Alpha, b"x".to_vec()),
                (BaseTokenType::Whitespace, b" ".to_vec()),
                (BaseTokenType::Alpha, b"y".to_vec()),
            ]
        );
        assert!(drain(&mut lex, false).is_empty());
        lex.reset();
        let mut t = BaseToken::new();
        assert!(lex.get_basetoken(&mut t, true));
        assert_eq!(
            (t.r#type, t.text.as_bytes()),
            (BaseTokenType::Alpha, b"x".as_slice())
        );
        lex.free();
        assert!(drain(&mut lex, false).is_empty());
        assert_eq!(lex.offset(), 0);
    }

    // 02-base-classify
    #[test]
    fn v02_base_classify() {
        // (ch, ws, nl, st)
        let cases: &[(u8, bool, bool, bool)] = &[
            (b' ', true, false, true),
            (b'\t', true, false, true),
            (b'\r', true, true, false),
            (b'\n', true, true, false),
            (0x0b, false, false, false),
            (0x0c, false, false, false),
            (b'_', false, false, false),
            (b'a', false, false, false),
            (b'0', false, false, false),
        ];
        for (ch, ws, nl, st) in cases {
            assert_eq!(is_whitespace(*ch), *ws, "ws {ch:#04x}");
            assert_eq!(is_newline(*ch), *nl, "nl {ch:#04x}");
            assert_eq!(is_space_or_tab(*ch), *st, "st {ch:#04x}");
        }
        assert!(is_newline_pair(b'\r', b'\n'));
        assert!(is_newline_pair(b'\n', b'\r'));
        assert!(!is_newline_pair(b'\n', b'\n'));
        assert!(!is_newline_pair(b'\r', b'\r'));
        assert_eq!(newline_size(b"\r\nx"), 2);
        assert_eq!(newline_size(b"\n\rx"), 2);
        assert_eq!(newline_size(b"\n\nx"), 1);
        assert_eq!(newline_size(b"\rx"), 1);
        assert_eq!(newline_size(b"x"), 0);
        assert_eq!(newline_size(b""), 0);
    }

    // 03-strref-cmp
    #[test]
    fn v03_strref_cmp() {
        type CmpCase<'a> = ((Option<&'a [u8]>, usize), Option<&'a [u8]>, i32);
        let cmp_cases: &[CmpCase] = &[
            ((Some(b"abc"), 3), Some(b"abc"), 0),
            ((Some(b"abc"), 3), Some(b"abd"), -1),
            ((Some(b"abc"), 3), Some(b"ab"), 1),
            ((Some(b"abc"), 2), Some(b"ab"), 0),
            ((Some(b"abc"), 2), Some(b"abc"), -1),
            ((Some(b"abc"), 0), Some(b""), 0),
            ((Some(b"abc"), 0), Some(b"x"), -1),
            ((Some(b"abc"), 0), None, 0),
            ((None, 0), None, 0),
            ((None, 0), Some(b"a"), -1),
            ((Some(b"abc"), 3), None, 1),
            ((Some(b"abc"), 3), Some(b""), 1),
            ((Some(b"Abc"), 3), Some(b"abc"), -1),
        ];
        for ((b1, len), b2, want) in cmp_cases {
            let a = match b1 {
                Some(bytes) => StrRef::set(bytes, *len),
                None => StrRef::clear(),
            };
            assert_eq!(strref_cmp(&a, *b2), *want, "cmp {:?}", b1);
        }
        let cmpi_cases: &[CmpCase] = &[
            ((Some(b"Abc"), 3), Some(b"aBC"), 0),
            ((Some(b"abc"), 3), Some(b"ABD"), -1),
            ((Some(b"a_"), 2), Some(b"A^"), 1),
        ];
        for ((b1, len), b2, want) in cmpi_cases {
            let a = match b1 {
                Some(bytes) => StrRef::set(bytes, *len),
                None => StrRef::clear(),
            };
            assert_eq!(strref_cmpi(&a, *b2), *want, "cmpi {:?}", b1);
        }
        // signed-char semantics for bytes >= 0x80
        let hi = StrRef::set(&[0xe9], 1);
        assert_eq!(strref_cmp(&hi, Some(b"a")), -1);
        let lo = StrRef::set(b"a", 1);
        assert_eq!(strref_cmp(&lo, Some(&[0xe9])), 1);
        assert_eq!(strref_cmpi(&hi, Some(b"a")), -1);

        fn sr<'a>(b: &'a [u8], len: usize) -> StrRef<'a> {
            StrRef::set(b, len)
        }
        let bufx = [0x78u8]; // "x"
        let empty = [0u8; 0];
        assert_eq!(strref_cmp_strref(&sr(b"abc", 3), &sr(b"abc", 3)), 0);
        assert_eq!(strref_cmp_strref(&sr(b"abc", 3), &sr(b"abd", 3)), -1);
        assert_eq!(strref_cmp_strref(&sr(b"abc", 2), &sr(b"abc", 3)), -1);
        assert_eq!(strref_cmp_strref(&sr(b"abc", 3), &sr(b"abc", 2)), 1);
        assert_eq!(strref_cmp_strref(&sr(b"abc", 3), &sr(&empty, 0)), -1);
        assert_eq!(strref_cmp_strref(&sr(&empty, 0), &sr(b"abc", 3)), -1);
        assert_eq!(strref_cmp_strref(&sr(&empty, 0), &sr(&empty, 0)), 0);
        assert_eq!(strref_cmp_strref(&StrRef::clear(), &sr(&bufx, 1)), -1);
        assert_eq!(strref_cmp_strref(&sr(b"b", 1), &sr(b"a", 1)), 1);
        assert_eq!(strref_cmp_strref(&sr(b"a", 1), &sr(b"b", 1)), -1);
        assert_eq!(strref_cmpi_strref(&sr(b"ABC", 3), &sr(b"abc", 3)), 0);
        assert_eq!(strref_cmpi_strref(&sr(b"abc", 3), &sr(b"abd", 3)), -1);
        assert_eq!(strref_cmpi_strref(&sr(b"abc", 3), &sr(&empty, 0)), -1);
        assert_eq!(strref_cmpi_strref(&sr(b"xbc", 1), &sr(b"XYZ", 1)), 0);

        // is_empty (R06)
        assert!(!sr(b"abc", 3).is_empty());
        assert!(sr(b"abc", 0).is_empty());
        assert!(sr(&empty, 0).is_empty());
        assert!(StrRef::clear().is_empty());
        // a NUL first byte is empty
        assert!(StrRef::set(b"\0x", 1).is_empty());

        // ref_add (R07)
        let world = b"hello world";
        let mut dst = tref(world, 0, 5);
        dst.add(&tref(world, 6, 5));
        assert_eq!((off_of(world, &dst), dst.len()), (0, 10));
        let mut dst = StrRef::clear();
        dst.add(&tref(world, 6, 5));
        assert_eq!((off_of(world, &dst), dst.len()), (6, 5));
        let mut dst = tref(world, 6, 5);
        dst.add(&tref(world, 0, 2));
        assert_eq!((off_of(world, &dst), dst.len()), (6, 7));
    }

    // 04-valid-numbers
    #[test]
    fn v04_valid_numbers() {
        let c = |s: &str| format!("{s}\0").into_bytes();
        let int_cases: &[(&str, usize, bool)] = &[
            ("-12a", 2, true),
            ("-12a", 3, false),
            ("123", 0, true),
            ("-123", 0, true),
            ("+1", 0, true),
            ("-", 0, false),
            ("", 0, false),
            ("12a", 0, false),
            ("12a", 2, true),
            ("12a", 3, false),
            ("1 ", 0, false),
            ("007", 0, true),
            ("1.5", 0, false),
            ("123456", 3, true),
        ];
        for (s, n, want) in int_cases {
            let bytes = c(s);
            assert_eq!(
                valid_int_str(Some(&bytes), *n),
                *want,
                "valid_int {s:?} n={n}"
            );
        }
        assert!(!valid_int_str(None, 0));

        let float_cases: &[(&str, usize, bool)] = &[
            ("-1.5abc", 3, true),
            ("-1.5abc", 4, false),
            ("1.5", 0, true),
            ("1", 0, true),
            ("1.", 0, true),
            (".5", 0, false),
            ("-.5", 0, false),
            ("-1.5", 0, true),
            ("1e5", 0, true),
            ("1e+5", 0, false), // Q-2
            ("1e-5", 0, false), // Q-2
            ("1e5+3", 0, true), // Q-2
            ("1e5-3", 0, true),
            ("1E5", 0, false),
            ("1e", 0, false),
            ("e5", 0, false),
            ("1.5.2", 0, false),
            ("1e5.2", 0, false),
            ("1e5e2", 0, false),
            ("1-2", 0, false),
            ("+", 0, false),
            ("", 0, false),
            ("1.5abc", 3, true),
            ("1.5abc", 0, false),
            ("1.5", 2, true),
            ("12.5", 1, true),
        ];
        for (s, n, want) in float_cases {
            let bytes = c(s);
            assert_eq!(
                valid_float_str(Some(&bytes), *n),
                *want,
                "valid_float {s:?} n={n}"
            );
        }
        assert!(!valid_float_str(None, 0));
    }

    // 05-error-data
    #[test]
    fn v05_error_data() {
        let mut ed = ErrorData::new();
        assert!(ed.is_empty());
        assert!(ed.buildstring().is_none());

        ed.add(Some("f.txt"), 1, 2, "bad thing", LEX_ERROR);
        ed.add(Some("g"), 10, 20, "warn", LEX_WARNING);
        ed.add(Some("f.txt"), 3, 4, "level seven", 7);
        assert_eq!(ed.len(), 3);
        assert_eq!(ed.type_count(LEX_ERROR), 1);
        assert_eq!(ed.type_count(LEX_WARNING), 1);
        assert_eq!(ed.type_count(7), 1);
        assert!(ed.has_errors());
        let it = ed.item(1).expect("item 1");
        assert_eq!(
            (it.file.as_deref(), it.row, it.column, it.msg.as_str()),
            (Some("g"), 10u32, 20u32, "warn")
        );

        assert_eq!(
            ed.buildstring().as_deref(),
            Some("f.txt (1, 2): bad thing\ng (10, 20): warn\nf.txt (3, 4): level seven\n")
        );

        ed.free();
        assert!(ed.is_empty());
        assert!(ed.buildstring().is_none());
        assert!(!ed.has_errors());
    }

    // 06-stroffset
    #[test]
    fn v06_stroffset() {
        let text = "ab\r\ncd\n\ref";
        let mut lex = Lexer::new();
        lex.start(Some(text.as_bytes()));
        for (pos, row, col) in [
            (Some(0), 1u32, 1u32),
            (Some(3), 2, 1),
            (Some(4), 2, 1),
            (Some(7), 3, 1),
            (Some(8), 3, 1),
            (Some(9), 3, 2),
        ] {
            let (mut r, mut c) = (0u32, 0u32);
            lex.get_stroffset(pos, &mut r, &mut c);
            assert_eq!((r, c), (row, col), "pos {pos:?}");
        }
        // a NULL pointer leaves the outputs untouched
        let (mut r, mut c) = (999u32, 999u32);
        lex.get_stroffset(None, &mut r, &mut c);
        assert_eq!((r, c), (999, 999));

        let mut lex = Lexer::new();
        lex.start(Some(b"ab"));
        lex.get_stroffset(Some(2), &mut r, &mut c);
        assert_eq!((r, c), (1, 3));

        let mut lex = Lexer::new();
        lex.start(Some(b"\n\n"));
        lex.get_stroffset(Some(2), &mut r, &mut c);
        assert_eq!((r, c), (3, 1));

        let mut lex = Lexer::new();
        lex.start(Some(b"\r\n"));
        lex.get_stroffset(Some(1), &mut r, &mut c);
        assert_eq!((r, c), (2, 1));

        let mut lex = Lexer::new();
        lex.start(Some(b"a\tb"));
        lex.get_stroffset(Some(2), &mut r, &mut c);
        assert_eq!((r, c), (1, 3));
    }
}
