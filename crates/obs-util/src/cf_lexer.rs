//! S-011 §5.2 (R20–R30) + §5.4 (R60): the C-like token layer and literal
//! decoding.
//!
//! Clean-room port of the `cf-lexer.c` slice of P1-08.3: token production
//! (algorithm 6.1) — NAME/NUM/SPACETAB/NEWLINE/STRING/OTHER, comments to
//! one space (Q-3), strings with escaped delimiters (Q-4) and the +1 source
//! span (Q-5), line splices, include strings (R27), SPACETAB merging (R29),
//! the NONE terminator (R30), `unexpected_eof` (R20/R25) — plus
//! `cf_literal_to_str` with its Q-10 loop quirk. The preprocessor (R40–R54)
//! is `cf_preprocessor`; the parser helpers (R70–R80) are `cf_parser`.
//!
//! Tokens are views: `str` offsets into the owning lexer's `reformatted`
//! buffer, `span` offsets into its original text. They are valid until the
//! lexer is re-lexed or dropped.

use crate::lexer::{BaseToken, BaseTokenType, Lexer, is_newline, is_space_or_tab, newline_size};

/// `enum cf_token_type`.
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum CfTokenType {
    #[default]
    None,
    Name,
    Num,
    Spacetab,
    Newline,
    String,
    Other,
}

impl CfTokenType {
    /// The conformance runner's type names.
    pub fn as_str(self) -> &'static str {
        match self {
            Self::None => "NONE",
            Self::Name => "NAME",
            Self::Num => "NUM",
            Self::Spacetab => "SPACETAB",
            Self::Newline => "NEWLINE",
            Self::String => "STRING",
            Self::Other => "OTHER",
        }
    }
}

/// Which buffer a token's views point into. Tokens from different lexers
/// are distinguished by this id, never by pointer identity (spec section 9).
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq)]
pub enum TokenSource {
    /// The main (top-level) lexer the preprocessor was handed.
    #[default]
    Main,
    /// An include dependency of the preprocessor (by index).
    Dep(usize),
    /// An extra token source registered by the owner, e.g. the lexers
    /// backing API-level definitions (by index).
    Side(usize),
    /// A synthesized token with no owning buffer (dumps as `src=-`).
    Synth,
}

/// `struct cf_token`: text view + source span + type.
#[derive(Clone, Copy, Debug, Default)]
pub struct CfToken {
    /// Offset into the owning lexer's `reformatted` buffer (the token text).
    pub str_off: usize,
    pub str_len: usize,
    /// Offset into the owning lexer's original text (row/col source).
    pub span_off: usize,
    /// Span length; STRING tokens are one byte longer than the text (Q-5).
    pub span_len: usize,
    pub r#type: CfTokenType,
    /// Owner of the token text (ignored for `Synth` tokens, which carry a
    /// one-byte space filler).
    pub src: TokenSource,
    /// Owner of the source span; `Synth` means no span at all (dumps `src=-`).
    pub span_src: TokenSource,
}

impl CfToken {
    /// The token text (a view into the owning lexer's `reformatted` buffer).
    pub fn str_bytes<'a>(&self, reformatted: &'a [u8]) -> &'a [u8] {
        &reformatted[self.str_off..self.str_off + self.str_len]
    }

    /// First byte of the text (R29 merge test).
    pub fn first_byte(&self, reformatted: &[u8]) -> Option<u8> {
        self.str_bytes(reformatted).first().copied()
    }
}

/// `struct cf_lexer` (R20): owned original text (via the base lexer), the
/// reformatted buffer (comments → one space, splices removed), the token
/// array and the `unexpected_eof` flag.
#[derive(Clone, Debug, Default)]
pub struct CfLexer {
    /// The file name (C `lex->file`); NULL → None.
    pub file: Option<String>,
    base: Lexer,
    /// Comments replaced by one space, splices removed.
    pub reformatted: Vec<u8>,
    pub tokens: Vec<CfToken>,
    pub unexpected_eof: bool,
}

impl CfLexer {
    /// `cf_lexer_init`.
    pub fn new() -> Self {
        Self::default()
    }

    /// The original text (C `lex->base_lexer.text`).
    pub fn text(&self) -> &[u8] {
        self.base.text()
    }

    /// 1-based row/column of a position in the original text (rule 4.2).
    pub fn rowcol(&self, pos: usize) -> (u32, u32) {
        let (mut r, mut c) = (0u32, 0u32);
        self.base.get_stroffset(Some(pos), &mut r, &mut c);
        (r, c)
    }

    /// `cf_lexer_lex` (R20): free previous state; NULL/empty text returns
    /// false and produces no tokens. Returns `!unexpected_eof`.
    pub fn lex(&mut self, text: Option<&[u8]>, file: Option<&str>) -> bool {
        self.reset_state();
        let Some(t) = text else {
            return false;
        };
        if t.is_empty() {
            return false;
        }
        if let Some(f) = file {
            self.file = Some(f.to_owned());
        }
        self.base.start(Some(t));
        self.reformatted.clear();

        let mut write_pos = 0;
        let mut last_idx: Option<usize> = None;
        while let Some(tok) = self.next_token(&mut write_pos) {
            // R29: merge consecutive blank-ish tokens
            if let Some(i) = last_idx {
                let prev_first = self.tokens[i].first_byte(&self.reformatted);
                let tok_first = tok.first_byte(&self.reformatted);
                if prev_first.is_some_and(is_space_or_tab) && tok_first.is_some_and(is_space_or_tab)
                {
                    self.tokens[i].str_len += tok.str_len;
                    self.tokens[i].span_len += tok.span_len;
                    continue;
                }
            }
            self.tokens.push(tok);
            last_idx = Some(self.tokens.len() - 1);
        }

        // R30: the NONE terminator
        self.tokens.push(CfToken {
            str_off: write_pos,
            str_len: 0,
            span_off: self.base.offset(),
            span_len: 0,
            r#type: CfTokenType::None,
            ..Default::default()
        });

        !self.unexpected_eof
    }

    fn reset_state(&mut self) {
        self.file = None;
        self.base.free();
        self.reformatted.clear();
        self.tokens.clear();
        self.unexpected_eof = false;
    }

    /// One token per algorithm 6.1.
    fn next_token(&mut self, write_pos: &mut usize) -> Option<CfToken> {
        let mut span_off: Option<usize> = None;
        let mut str_off = *write_pos;
        let mut str_len = 0usize;
        let mut wrote = false;
        let mut start_type = BaseTokenType::None;
        // first byte of the current token text, for the WHITESPACE/OTHER
        // break rules
        let mut start_first = 0u8;

        loop {
            let tb_off = self.base.offset();
            let mut tb = BaseToken::new();
            if !self.base.get_basetoken(&mut tb, false) {
                break;
            }
            // Copy the base token out so the borrow of `self.base` ends
            // before the `&mut self` process_* calls below.
            let tb_bytes: Vec<u8> = tb.text.as_bytes().to_vec();
            let mut tb_type = tb.r#type;
            let tb_end = tb_off + tb_bytes.len();
            let tb0 = *tb_bytes.first().unwrap_or(&0);
            if tb0 == b'_' {
                tb_type = BaseTokenType::Alpha;
            }

            // splice: `\` immediately followed by newline(s) — removed from
            // `str`, kept in the source span (R24)
            if tb0 == b'\\' && self.base.text().get(tb_end).is_some_and(|&n| is_newline(n)) {
                let text = self.base.text();
                self.base
                    .set_offset(tb_end + newline_size(&text[tb_end..]) as usize);
                continue;
            }

            if !wrote {
                span_off = Some(tb_off);
                str_off = *write_pos;
                start_first = tb0;
                if let Some(tok) = self.process_comment(tb_off, tb_end, write_pos) {
                    return Some(tok);
                }
                if let Some(tok) = self.process_string(tb_off, tb_end, write_pos) {
                    return Some(tok);
                }
                start_type = tb_type;
                wrote = true;
            } else if is_token_break(&mut start_type, start_first, tb_type, tb0) {
                self.base.set_offset(tb_off);
                break;
            }

            self.reformatted.extend_from_slice(&tb_bytes);
            *write_pos += tb_bytes.len();
            str_len += tb_bytes.len();
        }

        if !wrote {
            return None;
        }
        let end = self.base.offset();
        let start_off = span_off?;
        let first = *self.reformatted.get(str_off).unwrap_or(&0);
        Some(CfToken {
            str_off,
            str_len,
            span_off: start_off,
            span_len: end - start_off,
            r#type: get_token_type(first, start_type),
            ..Default::default()
        })
    }

    /// `cf_lexer_process_comment` (R25): only at token start.
    fn process_comment(
        &mut self,
        span_off: usize,
        o: usize,
        write_pos: &mut usize,
    ) -> Option<CfToken> {
        let text = self.base.text();
        if text.get(span_off) != Some(&b'/') {
            return None;
        }
        let mut o = o;
        pass_any_splices(&mut o, text);
        match text.get(o) {
            Some(b'/') | Some(b'*') => {}
            _ => return None,
        }

        // the comment becomes one space in `reformatted`
        let str_off = *write_pos;
        self.reformatted.push(b' ');
        *write_pos += 1;

        if text[o] == b'/' {
            // line comment (Q-3: the char after a splice is not examined)
            loop {
                o += 1;
                let ch = *text.get(o).unwrap_or(&0);
                if ch == 0 || is_newline(ch) {
                    break;
                }
                pass_any_splices(&mut o, text);
            }
        } else {
            // block comment
            self.unexpected_eof = true;
            let mut was_star = false;
            loop {
                o += 1;
                let ch = *text.get(o).unwrap_or(&0);
                if ch == 0 {
                    break;
                }
                pass_any_splices(&mut o, text);
                if was_star && *text.get(o).unwrap_or(&0) == b'/' {
                    o += 1;
                    self.unexpected_eof = false;
                    break;
                }
                was_star = *text.get(o).unwrap_or(&0) == b'*';
            }
        }

        self.base.set_offset(o);
        Some(CfToken {
            str_off,
            str_len: 1,
            span_off,
            span_len: o - span_off,
            r#type: CfTokenType::Spacetab,
            ..Default::default()
        })
    }

    /// `cf_lexer_process_string` (R26/R27): quote strings and, in an include
    /// context, `<...>` include strings.
    fn process_string(
        &mut self,
        span_off: usize,
        o: usize,
        write_pos: &mut usize,
    ) -> Option<CfToken> {
        let text = self.base.text();
        let c0 = *text.get(span_off).unwrap_or(&0);
        let (delim, escapes_allowed) = if c0 == b'<' && self.in_include() {
            (b'>', false)
        } else if c0 == b'"' || c0 == b'\'' {
            (c0, !self.in_include())
        } else {
            return None;
        };

        let str_off = *write_pos;
        self.reformatted.push(c0);
        *write_pos += 1;
        let mut o = o;
        let mut escaped = false;
        loop {
            pass_any_splices(&mut o, text);
            let ch = *text.get(o).unwrap_or(&0);
            if ch == 0 {
                break;
            }
            if ch == delim {
                if !escaped {
                    self.reformatted.push(ch);
                    *write_pos += 1;
                    o += 1;
                    break;
                }
            } else if is_newline(ch) {
                // stop before the newline, not consumed
                break;
            }
            self.reformatted.push(ch);
            *write_pos += 1;
            escaped = escapes_allowed && ch == b'\\';
            o += 1;
        }

        self.base.set_offset(o);
        Some(CfToken {
            str_off,
            str_len: *write_pos - str_off,
            span_off,
            // Q-5: one byte longer than the covered text
            span_len: 1 + (o - span_off),
            r#type: CfTokenType::String,
            ..Default::default()
        })
    }

    /// `cf_lexer_is_include` (R27): the tokens so far end (skipping blanks)
    /// with `include`/`import`, preceded (skipping blanks) by `#`, preceded
    /// by a NEWLINE or the start of the stream.
    fn in_include(&self) -> bool {
        let mut found_inc = false;
        let mut found_pp = false;
        let mut i = self.tokens.len();
        while i > 0 {
            i -= 1;
            let bytes = self.tokens[i].str_bytes(&self.reformatted);
            if bytes.is_empty() || is_space_or_tab(bytes[0]) {
                continue;
            }
            if !found_inc {
                if bytes != b"include" && bytes != b"import" {
                    break;
                }
                found_inc = true;
            } else if !found_pp {
                if bytes[0] != b'#' {
                    break;
                }
                found_pp = true;
            } else {
                return is_newline(bytes[0]);
            }
        }
        found_pp && found_inc
    }
}

/// `cf_pass_any_splices`.
fn pass_any_splices(o: &mut usize, text: &[u8]) {
    while text.get(*o) == Some(&b'\\') && text.get(*o + 1).is_some_and(|&n| is_newline(n)) {
        *o += 1 + newline_size(&text[*o + 1..]) as usize;
    }
}

/// `cf_is_token_break` (6.1); mutates `start_type` for the `.`+DIGIT case.
fn is_token_break(
    start_type: &mut BaseTokenType,
    start_first: u8,
    tb_type: BaseTokenType,
    tb0: u8,
) -> bool {
    match start_type {
        BaseTokenType::Alpha => matches!(tb_type, BaseTokenType::Other | BaseTokenType::Whitespace),
        BaseTokenType::Digit => {
            tb_type == BaseTokenType::Whitespace || (tb_type == BaseTokenType::Other && tb0 != b'.')
        }
        BaseTokenType::Whitespace => !(is_space_or_tab(start_first) && is_space_or_tab(tb0)),
        BaseTokenType::Other => {
            if start_first == b'.' && tb_type == BaseTokenType::Digit {
                *start_type = BaseTokenType::Digit;
                false
            } else {
                true
            }
        }
        BaseTokenType::None => true,
    }
}

/// `cf_get_token_type` (6.1).
fn get_token_type(first: u8, start_type: BaseTokenType) -> CfTokenType {
    match start_type {
        BaseTokenType::Alpha => CfTokenType::Name,
        BaseTokenType::Digit => CfTokenType::Num,
        BaseTokenType::Whitespace => {
            if is_newline(first) {
                CfTokenType::Newline
            } else {
                CfTokenType::Spacetab
            }
        }
        _ => CfTokenType::Other,
    }
}

/// C `strtoul`-style greedy parse (skips blanks, optional sign, `0x` prefix
/// in hex); no digits → 0. Low 64 bits on overflow.
fn strtoul_greedy(s: &[u8], hex: bool) -> u64 {
    let mut i = 0;
    while i < s.len() && matches!(s[i], b' ' | b'\t' | b'\n' | 0x0b | 0x0c | b'\r') {
        i += 1;
    }
    let mut neg = false;
    if i < s.len() && (s[i] == b'+' || s[i] == b'-') {
        neg = s[i] == b'-';
        i += 1;
    }
    if hex && i + 1 < s.len() && s[i] == b'0' && (s[i + 1] == b'x' || s[i + 1] == b'X') {
        i += 2;
    }
    let base = if hex { 16 } else { 8 } as u64;
    let mut v: u64 = 0;
    while i < s.len() {
        let c = s[i] as char;
        let Some(d) = c.to_digit(16) else {
            break;
        };
        if d as u64 >= base {
            break;
        }
        v = v.wrapping_mul(base).wrapping_add(d as u64);
        i += 1;
    }
    if neg { v.wrapping_neg() } else { v }
}

/// `cf_literal_to_str` (R60, Q-10, D-8): decode a quoted literal. The loop
/// runs exactly `count - 2` output iterations (or stops at NUL), and every
/// escape consumes its source characters in a single iteration — so an
/// escape leaves extra iterations that copy following source chars (the
/// closing quote leaks). Returns the decoded bytes (a decoded NUL ends the
/// string as seen by C callers).
pub fn cf_literal_to_str(literal: &[u8], count: usize) -> Option<Vec<u8>> {
    let n = if count == 0 {
        literal
            .iter()
            .position(|&b| b == 0)
            .unwrap_or(literal.len())
    } else {
        count
    };
    if n < 2 {
        return None;
    }
    let first = literal.first().copied().unwrap_or(0);
    let last = *literal.get(n - 1).unwrap_or(&0);
    if first != last || (first != b'"' && first != b'\'') {
        return None;
    }

    let ch = |p: usize| *literal.get(p).unwrap_or(&0);
    let mut out: Vec<u8> = Vec::new();
    let mut p = 1;
    let mut iters = n - 2;
    while iters > 0 && ch(p) != 0 {
        iters -= 1;
        if ch(p) == b'\\' {
            p += 1;
            match ch(p) {
                c @ b'\'' | c @ b'"' | c @ b'?' | c @ b'\\' => {
                    out.push(c);
                    p += 1;
                }
                b'0' => {
                    out.push(0);
                    p += 1;
                }
                b'a' => {
                    out.push(0x07);
                    p += 1;
                }
                b'b' => {
                    out.push(0x08);
                    p += 1;
                }
                b'f' => {
                    out.push(0x0c);
                    p += 1;
                }
                b'n' => {
                    out.push(0x0a);
                    p += 1;
                }
                b'r' => {
                    out.push(0x0d);
                    p += 1;
                }
                b't' => {
                    out.push(0x09);
                    p += 1;
                }
                b'v' => {
                    out.push(0x0b);
                    p += 1;
                }
                b'x' | b'X' => {
                    // the switch consumed the x; C reads two chars from here
                    out.push(strtoul_greedy(&literal[p + 1..], true) as u8);
                    p += 3;
                }
                _ => {
                    // C's switch already consumed this char; the octal check
                    // looks at the next char and skips three from there
                    if ch(p + 1).is_ascii_digit() {
                        out.push(strtoul_greedy(&literal[p + 1..], false) as u8);
                        p += 4;
                    } else {
                        p += 1;
                    }
                }
            }
        } else {
            out.push(ch(p));
            p += 1;
        }
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    //! Oracle: the committed C reference vectors
    //! `test/conformance/vectors/lexer/{10,11,12,13,14,15,33}` and the
    //! conformance runner's print format (`gen/lexer_runner.c`,
    //! `script.h::script_print_quoted`).

    use super::*;

    /// `script_print_quoted`: escapes `\\ \" \n \t \r`, other non-printable
    /// bytes as `\xHH`.
    fn quoted(s: &[u8]) -> String {
        let mut out = String::from("\"");
        for &b in s {
            match b {
                b'\\' => out.push_str("\\\\"),
                b'"' => out.push_str("\\\""),
                b'\n' => out.push_str("\\n"),
                b'\t' => out.push_str("\\t"),
                b'\r' => out.push_str("\\r"),
                b if !(0x20..0x7f).contains(&b) => out.push_str(&format!("\\x{b:02x}")),
                b => out.push(b as char),
            }
        }
        out.push('"');
        out
    }

    fn print_tok(lex: &CfLexer, t: &CfToken) -> String {
        let str = t.str_bytes(&lex.reformatted);
        let text = lex.text();
        let u_len = t.span_len.min(text.len().saturating_sub(t.span_off));
        let u = &text[t.span_off..t.span_off + u_len];
        let (r, c) = lex.rowcol(t.span_off);
        let show_u = t.span_len != t.str_len || (str.len() <= u.len() && &u[..str.len()] != str);
        let mut s = format!(
            "type={} str={} src={}+{} @{}:{}",
            t.r#type.as_str(),
            quoted(str),
            t.span_off,
            t.span_len,
            r,
            c
        );
        if show_u {
            s.push_str(&format!(" u={}", quoted(u)));
        }
        s
    }

    /// The runner's `lex TEXT` + `tokens lex` output.
    fn lex_case(lex: &mut CfLexer, text: Option<&[u8]>) -> String {
        let r = lex.lex(text, None);
        let mut s = format!(
            "lex {} tokens={} unexpected_eof={}\n",
            r,
            lex.tokens.len(),
            lex.unexpected_eof as u8
        );
        if text.is_some_and(|t| !t.is_empty()) {
            s.push_str(&format!("reformatted {}\n", quoted(&lex.reformatted)));
        }
        s.push_str(&format!("tokens lex {}\n", lex.tokens.len()));
        for (i, t) in lex.tokens.iter().enumerate() {
            s.push_str(&format!("tok {} {}\n", i, print_tok(lex, t)));
        }
        s
    }

    // 10-cf-lex-basic
    #[test]
    fn v10_cf_lex_basic() {
        let mut lex = CfLexer::new();
        let mut got = String::new();
        got.push_str(&lex_case(
            &mut lex,
            Some(b"a1b _x __ 1.5.3 .5 0x1F 1e+5 1_a a.5 1.. 1+2 ==  ->  x\ty \t z".as_slice()),
        ));
        got.push_str(&lex_case(
            &mut lex,
            Some(b"a\tb \t c\r\nd\n\re\rf\x0bg\x0ch\n\n".as_slice()),
        ));
        got.push_str(&lex_case(&mut lex, Some(b"\xc3\xa9 $ @ \\ `".as_slice())));
        let expected = r#"lex true tokens=39 unexpected_eof=0
reformatted "a1b _x __ 1.5.3 .5 0x1F 1e+5 1_a a.5 1.. 1+2 ==  ->  x\ty \t z"
tokens lex 39
tok 0 type=NAME str="a1b" src=0+3 @1:1
tok 1 type=SPACETAB str=" " src=3+1 @1:4
tok 2 type=NAME str="_x" src=4+2 @1:5
tok 3 type=SPACETAB str=" " src=6+1 @1:7
tok 4 type=NAME str="__" src=7+2 @1:8
tok 5 type=SPACETAB str=" " src=9+1 @1:10
tok 6 type=NUM str="1.5.3" src=10+5 @1:11
tok 7 type=SPACETAB str=" " src=15+1 @1:16
tok 8 type=NUM str=".5" src=16+2 @1:17
tok 9 type=SPACETAB str=" " src=18+1 @1:19
tok 10 type=NUM str="0x1F" src=19+4 @1:20
tok 11 type=SPACETAB str=" " src=23+1 @1:24
tok 12 type=NUM str="1e" src=24+2 @1:25
tok 13 type=OTHER str="+" src=26+1 @1:27
tok 14 type=NUM str="5" src=27+1 @1:28
tok 15 type=SPACETAB str=" " src=28+1 @1:29
tok 16 type=NUM str="1_a" src=29+3 @1:30
tok 17 type=SPACETAB str=" " src=32+1 @1:33
tok 18 type=NAME str="a" src=33+1 @1:34
tok 19 type=NUM str=".5" src=34+2 @1:35
tok 20 type=SPACETAB str=" " src=36+1 @1:37
tok 21 type=NUM str="1.." src=37+3 @1:38
tok 22 type=SPACETAB str=" " src=40+1 @1:41
tok 23 type=NUM str="1" src=41+1 @1:42
tok 24 type=OTHER str="+" src=42+1 @1:43
tok 25 type=NUM str="2" src=43+1 @1:44
tok 26 type=SPACETAB str=" " src=44+1 @1:45
tok 27 type=OTHER str="=" src=45+1 @1:46
tok 28 type=OTHER str="=" src=46+1 @1:47
tok 29 type=SPACETAB str="  " src=47+2 @1:48
tok 30 type=OTHER str="-" src=49+1 @1:50
tok 31 type=OTHER str=">" src=50+1 @1:51
tok 32 type=SPACETAB str="  " src=51+2 @1:52
tok 33 type=NAME str="x" src=53+1 @1:54
tok 34 type=SPACETAB str="\t" src=54+1 @1:55
tok 35 type=NAME str="y" src=55+1 @1:56
tok 36 type=SPACETAB str=" \t " src=56+3 @1:57
tok 37 type=NAME str="z" src=59+1 @1:60
tok 38 type=NONE str="" src=60+0 @1:61
lex true tokens=18 unexpected_eof=0
reformatted "a\tb \t c\r\nd\n\re\rf\x0bg\x0ch\n\n"
tokens lex 18
tok 0 type=NAME str="a" src=0+1 @1:1
tok 1 type=SPACETAB str="\t" src=1+1 @1:2
tok 2 type=NAME str="b" src=2+1 @1:3
tok 3 type=SPACETAB str=" \t " src=3+3 @1:4
tok 4 type=NAME str="c" src=6+1 @1:7
tok 5 type=NEWLINE str="\r\n" src=7+2 @1:8
tok 6 type=NAME str="d" src=9+1 @2:1
tok 7 type=NEWLINE str="\n\r" src=10+2 @2:2
tok 8 type=NAME str="e" src=12+1 @3:1
tok 9 type=NEWLINE str="\r" src=13+1 @3:2
tok 10 type=NAME str="f" src=14+1 @4:1
tok 11 type=OTHER str="\x0b" src=15+1 @4:2
tok 12 type=NAME str="g" src=16+1 @4:3
tok 13 type=OTHER str="\x0c" src=17+1 @4:4
tok 14 type=NAME str="h" src=18+1 @4:5
tok 15 type=NEWLINE str="\n" src=19+1 @4:6
tok 16 type=NEWLINE str="\n" src=20+1 @5:1
tok 17 type=NONE str="" src=21+0 @6:1
lex true tokens=11 unexpected_eof=0
reformatted "\xc3\xa9 $ @ \\ `"
tokens lex 11
tok 0 type=OTHER str="\xc3" src=0+1 @1:1
tok 1 type=OTHER str="\xa9" src=1+1 @1:2
tok 2 type=SPACETAB str=" " src=2+1 @1:3
tok 3 type=OTHER str="$" src=3+1 @1:4
tok 4 type=SPACETAB str=" " src=4+1 @1:5
tok 5 type=OTHER str="@" src=5+1 @1:6
tok 6 type=SPACETAB str=" " src=6+1 @1:7
tok 7 type=OTHER str="\\" src=7+1 @1:8
tok 8 type=SPACETAB str=" " src=8+1 @1:9
tok 9 type=OTHER str="`" src=9+1 @1:10
tok 10 type=NONE str="" src=10+0 @1:11
"#;
        assert_eq!(got, expected);
    }

    // 11-cf-lex-empty
    #[test]
    fn v11_cf_lex_empty() {
        let mut lex = CfLexer::new();
        let mut got = String::new();
        got.push_str(&lex_case(&mut lex, Some(b"".as_slice())));
        got.push_str(&lex_case(&mut lex, Some(b"   ".as_slice())));
        got.push_str(&lex_case(&mut lex, Some(b"\n".as_slice())));
        let expected = r#"lex false tokens=0 unexpected_eof=0
tokens lex 0
lex true tokens=2 unexpected_eof=0
reformatted "   "
tokens lex 2
tok 0 type=SPACETAB str="   " src=0+3 @1:1
tok 1 type=NONE str="" src=3+0 @1:4
lex true tokens=2 unexpected_eof=0
reformatted "\n"
tokens lex 2
tok 0 type=NEWLINE str="\n" src=0+1 @1:1
tok 1 type=NONE str="" src=1+0 @2:1
"#;
        assert_eq!(got, expected);
    }

    // 12-cf-lex-comments
    #[test]
    fn v12_cf_lex_comments() {
        let mut lex = CfLexer::new();
        let mut got = String::new();
        got.push_str(&lex_case(
            &mut lex,
            Some(b"x /* y */* z / * q //* r\nu".as_slice()),
        ));
        got.push_str(&lex_case(&mut lex, Some(b"a /* unterminated".as_slice())));
        got.push_str(&lex_case(
            &mut lex,
            Some(b"a/**/b /***/ c /*/ d */ e //".as_slice()),
        ));
        got.push_str(&lex_case(
            &mut lex,
            Some(b"1/*c*/2 ./*c*/ a//c\nb".as_slice()),
        ));
        let expected = r#"lex true tokens=15 unexpected_eof=0
reformatted "x  * z / * q  \nu"
tokens lex 15
tok 0 type=NAME str="x" src=0+1 @1:1
tok 1 type=SPACETAB str="  " src=1+8 @1:2 u=" /* y */"
tok 2 type=OTHER str="*" src=9+1 @1:10
tok 3 type=SPACETAB str=" " src=10+1 @1:11
tok 4 type=NAME str="z" src=11+1 @1:12
tok 5 type=SPACETAB str=" " src=12+1 @1:13
tok 6 type=OTHER str="/" src=13+1 @1:14
tok 7 type=SPACETAB str=" " src=14+1 @1:15
tok 8 type=OTHER str="*" src=15+1 @1:16
tok 9 type=SPACETAB str=" " src=16+1 @1:17
tok 10 type=NAME str="q" src=17+1 @1:18
tok 11 type=SPACETAB str="  " src=18+6 @1:19 u=" //* r"
tok 12 type=NEWLINE str="\n" src=24+1 @1:25
tok 13 type=NAME str="u" src=25+1 @2:1
tok 14 type=NONE str="" src=26+0 @2:2
lex false tokens=3 unexpected_eof=1
reformatted "a  "
tokens lex 3
tok 0 type=NAME str="a" src=0+1 @1:1
tok 1 type=SPACETAB str="  " src=1+16 @1:2 u=" /* unterminated"
tok 2 type=NONE str="" src=17+0 @1:18
lex true tokens=9 unexpected_eof=0
reformatted "a b   c   e  "
tokens lex 9
tok 0 type=NAME str="a" src=0+1 @1:1
tok 1 type=SPACETAB str=" " src=1+4 @1:2 u="/**/"
tok 2 type=NAME str="b" src=5+1 @1:6
tok 3 type=SPACETAB str="   " src=6+7 @1:7 u=" /***/ "
tok 4 type=NAME str="c" src=13+1 @1:14
tok 5 type=SPACETAB str="   " src=14+10 @1:15 u=" /*/ d */ "
tok 6 type=NAME str="e" src=24+1 @1:25
tok 7 type=SPACETAB str="  " src=25+3 @1:26 u=" //"
tok 8 type=NONE str="" src=28+0 @1:29
lex true tokens=11 unexpected_eof=0
reformatted "1 2 .  a \nb"
tokens lex 11
tok 0 type=NUM str="1" src=0+1 @1:1
tok 1 type=SPACETAB str=" " src=1+5 @1:2 u="/*c*/"
tok 2 type=NUM str="2" src=6+1 @1:7
tok 3 type=SPACETAB str=" " src=7+1 @1:8
tok 4 type=OTHER str="." src=8+1 @1:9
tok 5 type=SPACETAB str="  " src=9+6 @1:10 u="/*c*/ "
tok 6 type=NAME str="a" src=15+1 @1:16
tok 7 type=SPACETAB str=" " src=16+3 @1:17 u="//c"
tok 8 type=NEWLINE str="\n" src=19+1 @1:20
tok 9 type=NAME str="b" src=20+1 @2:1
tok 10 type=NONE str="" src=21+0 @2:2
"#;
        assert_eq!(got, expected);
    }

    // 13-cf-lex-strings
    #[test]
    fn v13_cf_lex_strings() {
        let mut lex = CfLexer::new();
        let mut got = String::new();
        got.push_str(&lex_case(&mut lex, Some(b"\"abc".as_slice())));
        got.push_str(&lex_case(
            &mut lex,
            Some(b"'x\\'y' \"p\\\"q\" \"a\nb\"".as_slice()),
        ));
        got.push_str(&lex_case(
            &mut lex,
            Some(b"#include <abc.h>\n#include \"a\\b\"\n# include \"x\" \n a <b> #include <c> \n#import <q>\n/* c */ #include <z>".as_slice()),
        ));
        let expected = r##"lex true tokens=2 unexpected_eof=0
reformatted "\"abc"
tokens lex 2
tok 0 type=STRING str="\"abc" src=0+5 @1:1 u="\"abc"
tok 1 type=NONE str="" src=4+0 @1:5
lex true tokens=9 unexpected_eof=0
reformatted "'x\\'y' \"p\\\"q\" \"a\nb\""
tokens lex 9
tok 0 type=STRING str="'x\\'y'" src=0+7 @1:1 u="'x\\'y' "
tok 1 type=SPACETAB str=" " src=6+1 @1:7
tok 2 type=STRING str="\"p\\\"q\"" src=7+7 @1:8 u="\"p\\\"q\" "
tok 3 type=SPACETAB str=" " src=13+1 @1:14
tok 4 type=STRING str="\"a" src=14+3 @1:15 u="\"a\n"
tok 5 type=NEWLINE str="\n" src=16+1 @1:17
tok 6 type=NAME str="b" src=17+1 @2:1
tok 7 type=STRING str="\"" src=18+2 @2:2 u="\""
tok 8 type=NONE str="" src=19+0 @2:3
lex true tokens=43 unexpected_eof=0
reformatted "#include <abc.h>\n#include \"a\\b\"\n# include \"x\" \n a <b> #include <c> \n#import <q>\n  #include <z>"
tokens lex 43
tok 0 type=OTHER str="#" src=0+1 @1:1
tok 1 type=NAME str="include" src=1+7 @1:2
tok 2 type=SPACETAB str=" " src=8+1 @1:9
tok 3 type=STRING str="<abc.h>" src=9+8 @1:10 u="<abc.h>\n"
tok 4 type=NEWLINE str="\n" src=16+1 @1:17
tok 5 type=OTHER str="#" src=17+1 @2:1
tok 6 type=NAME str="include" src=18+7 @2:2
tok 7 type=SPACETAB str=" " src=25+1 @2:9
tok 8 type=STRING str="\"a\\b\"" src=26+6 @2:10 u="\"a\\b\"\n"
tok 9 type=NEWLINE str="\n" src=31+1 @2:15
tok 10 type=OTHER str="#" src=32+1 @3:1
tok 11 type=SPACETAB str=" " src=33+1 @3:2
tok 12 type=NAME str="include" src=34+7 @3:3
tok 13 type=SPACETAB str=" " src=41+1 @3:10
tok 14 type=STRING str="\"x\"" src=42+4 @3:11 u="\"x\" "
tok 15 type=SPACETAB str=" " src=45+1 @3:14
tok 16 type=NEWLINE str="\n" src=46+1 @3:15
tok 17 type=SPACETAB str=" " src=47+1 @4:1
tok 18 type=NAME str="a" src=48+1 @4:2
tok 19 type=SPACETAB str=" " src=49+1 @4:3
tok 20 type=OTHER str="<" src=50+1 @4:4
tok 21 type=NAME str="b" src=51+1 @4:5
tok 22 type=OTHER str=">" src=52+1 @4:6
tok 23 type=SPACETAB str=" " src=53+1 @4:7
tok 24 type=OTHER str="#" src=54+1 @4:8
tok 25 type=NAME str="include" src=55+7 @4:9
tok 26 type=SPACETAB str=" " src=62+1 @4:16
tok 27 type=OTHER str="<" src=63+1 @4:17
tok 28 type=NAME str="c" src=64+1 @4:18
tok 29 type=OTHER str=">" src=65+1 @4:19
tok 30 type=SPACETAB str=" " src=66+1 @4:20
tok 31 type=NEWLINE str="\n" src=67+1 @4:21
tok 32 type=OTHER str="#" src=68+1 @5:1
tok 33 type=NAME str="import" src=69+6 @5:2
tok 34 type=SPACETAB str=" " src=75+1 @5:8
tok 35 type=STRING str="<q>" src=76+4 @5:9 u="<q>\n"
tok 36 type=NEWLINE str="\n" src=79+1 @5:12
tok 37 type=SPACETAB str="  " src=80+8 @6:1 u="/* c */ "
tok 38 type=OTHER str="#" src=88+1 @6:9
tok 39 type=NAME str="include" src=89+7 @6:10
tok 40 type=SPACETAB str=" " src=96+1 @6:17
tok 41 type=STRING str="<z>" src=97+4 @6:18 u="<z>"
tok 42 type=NONE str="" src=100+0 @6:21
"##;
        assert_eq!(got, expected);
    }

    // 14-cf-lex-splices
    #[test]
    fn v14_cf_lex_splices() {
        let mut lex = CfLexer::new();
        let mut got = String::new();
        got.push_str(&lex_case(
            &mut lex,
            Some(b"ab\\\ncd /\\\n* x *\\\n/ e".as_slice()),
        ));
        got.push_str(&lex_case(
            &mut lex,
            Some(b"/\\\n/ hi\\\n there\nnext".as_slice()),
        ));
        got.push_str(&lex_case(
            &mut lex,
            Some(b"\"a\\\nb\" x\\\r\ny z\\\n\r1".as_slice()),
        ));
        got.push_str(&lex_case(&mut lex, Some(b"a \\ b \\x".as_slice())));
        let expected = r#"lex true tokens=4 unexpected_eof=0
reformatted "abcd   e"
tokens lex 4
tok 0 type=NAME str="abcd" src=0+6 @1:1 u="ab\\\ncd"
tok 1 type=SPACETAB str="   " src=6+13 @2:3 u=" /\\\n* x *\\\n/ "
tok 2 type=NAME str="e" src=19+1 @4:3
tok 3 type=NONE str="" src=20+0 @4:4
lex true tokens=4 unexpected_eof=0
reformatted " \nnext"
tokens lex 4
tok 0 type=SPACETAB str=" " src=0+15 @1:1 u="/\\\n/ hi\\\n there"
tok 1 type=NEWLINE str="\n" src=15+1 @3:7
tok 2 type=NAME str="next" src=16+4 @4:1
tok 3 type=NONE str="" src=20+0 @4:5
lex true tokens=6 unexpected_eof=0
reformatted "\"ab\" xy z1"
tokens lex 6
tok 0 type=STRING str="\"ab\"" src=0+7 @1:1 u="\"a\\\nb\" "
tok 1 type=SPACETAB str=" " src=6+1 @2:3
tok 2 type=NAME str="xy" src=7+5 @2:4 u="x\\\r\ny"
tok 3 type=SPACETAB str=" " src=12+1 @3:2
tok 4 type=NAME str="z1" src=13+5 @3:3 u="z\\\n\r1"
tok 5 type=NONE str="" src=18+0 @4:2
lex true tokens=9 unexpected_eof=0
reformatted "a \\ b \\x"
tokens lex 9
tok 0 type=NAME str="a" src=0+1 @1:1
tok 1 type=SPACETAB str=" " src=1+1 @1:2
tok 2 type=OTHER str="\\" src=2+1 @1:3
tok 3 type=SPACETAB str=" " src=3+1 @1:4
tok 4 type=NAME str="b" src=4+1 @1:5
tok 5 type=SPACETAB str=" " src=5+1 @1:6
tok 6 type=OTHER str="\\" src=6+1 @1:7
tok 7 type=NAME str="x" src=7+1 @1:8
tok 8 type=NONE str="" src=8+0 @1:9
"#;
        assert_eq!(got, expected);
    }

    // 15-cf-lex-rows
    #[test]
    fn v15_cf_lex_rows() {
        let mut lex = CfLexer::new();
        let got = lex_case(&mut lex, Some(b"a\r\nb\n\rc\rd\ne".as_slice()));
        let expected = r#"lex true tokens=10 unexpected_eof=0
reformatted "a\r\nb\n\rc\rd\ne"
tokens lex 10
tok 0 type=NAME str="a" src=0+1 @1:1
tok 1 type=NEWLINE str="\r\n" src=1+2 @1:2
tok 2 type=NAME str="b" src=3+1 @2:1
tok 3 type=NEWLINE str="\n\r" src=4+2 @2:2
tok 4 type=NAME str="c" src=6+1 @3:1
tok 5 type=NEWLINE str="\r" src=7+1 @3:2
tok 6 type=NAME str="d" src=8+1 @4:1
tok 7 type=NEWLINE str="\n" src=9+1 @4:2
tok 8 type=NAME str="e" src=10+1 @5:1
tok 9 type=NONE str="" src=11+0 @5:2
"#;
        assert_eq!(got, expected);
    }

    // 33-literal
    #[test]
    fn v33_literal() {
        // (input bytes, explicit count, expected decoded bytes or None)
        // `C` = view as seen by a C caller: up to the first decoded NUL.
        type Case<'a> = (&'a [u8], usize, Option<Vec<u8>>);
        let cases: &[Case<'_>] = &[
            (b"\"abc\"", 0, Some(b"abc".to_vec())),
            (b"'abc'", 0, Some(b"abc".to_vec())),
            (b"\"abc\"", 5, Some(b"abc".to_vec())),
            (b"\"abc\"", 3, None),
            (b"\"abc'", 0, None),
            (b"abc", 0, None),
            (b"\"\"", 0, Some(Vec::new())),
            (b"\"", 0, None),
            (
                b"\"a\nb\tc\\\\d\"e'f?g\"",
                0,
                Some(vec![
                    b'a', 0x0a, b'b', 0x09, b'c', b'\\', b'd', b'"', b'e', b'\'', b'f', b'?', b'g',
                    b'"',
                ]),
            ),
            (
                b"\"\\a\\b\\f\\r\\v\\0z\"",
                0,
                Some(vec![0x07, 0x08, 0x0c, 0x0d, 0x0b]),
            ),
            (b"\"\\101z\"", 0, Some(vec![0x01, b'"'])),
            (b"\"\\q\"", 0, Some(vec![b'"'])),
            (b"\"\\\"", 0, Some(vec![b'"'])),
            (b"\"ab\\\"", 0, Some(vec![b'a', b'b', b'"'])),
            (b"\"a\"", 2, None),
            (b"'\\x41'", 0, Some(vec![b'A', b'\''])),
            (
                b"\"\\n\\n\\n\\n\"",
                0,
                Some(vec![0x0a, 0x0a, 0x0a, 0x0a, b'"']),
            ),
            (b"\"\\xzz\"", 0, Some(Vec::new())),
            (b"\"\\123\"", 0, Some(vec![0x13])),
            (b"\"\\1\"", 0, Some(vec![b'"'])),
            (b"\"\\q\"", 0, Some(vec![b'"'])),
        ];
        let mut out = String::new();
        for (input, count, want) in cases {
            let got = cf_literal_to_str(input, *count);
            let c_view = got.as_ref().map(|v| {
                let end = v.iter().position(|&b| b == 0).unwrap_or(v.len());
                v[..end].to_vec()
            });
            assert_eq!(
                c_view.as_ref(),
                want.as_ref(),
                "literal {input:?} n={count}"
            );
            match &c_view {
                Some(v) => out.push_str(&format!("literal {}\n", quoted(v))),
                None => out.push_str("literal @null\n"),
            }
        }
        let expected = r#"literal "abc"
literal "abc"
literal "abc"
literal @null
literal @null
literal @null
literal ""
literal @null
literal "a\nb\tc\\d\"e'f?g\""
literal "\x07\x08\x0c\r\x0b"
literal "\x01\""
literal "\""
literal "\""
literal "ab\""
literal @null
literal "A'"
literal "\n\n\n\n\""
literal ""
literal "\x13"
literal "\""
literal "\""
"#;
        assert_eq!(out, expected);
    }
}
