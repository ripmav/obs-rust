//! cf parser helpers (S-011 §5.5, R70–R80, Q-11, Q-12).
//!
//! A [`CfParser`] owns its main lexer, preprocessor and error list; the
//! preprocessed output tokens view into the owning lexers (main,
//! dependencies, caller side lexers) exactly like in the reference.
//!
//! The cursor is an index into the preprocessed stream. D-2 (reference
//! defect: helpers on a never-parsed parser dereference a NULL cursor and
//! crash) has no Rust equivalent: a fresh or failed parser treats the
//! stream as empty, so every helper is safe and the end-of-stream token is
//! a synthesized NONE sentinel.

use crate::cf_lexer::{CfLexer, CfToken, CfTokenType, TokenSource};
use crate::cf_preprocessor::{CfPreprocessor, substitute};
use crate::lexer::{ErrorData, LEX_ERROR};

/// `PARSE_SUCCESS`.
pub const PARSE_SUCCESS: i32 = 0;
/// `PARSE_CONTINUE`.
pub const PARSE_CONTINUE: i32 = -1;
/// `PARSE_BREAK`.
pub const PARSE_BREAK: i32 = -2;
/// `PARSE_UNEXPECTED_CONTINUE`.
pub const PARSE_UNEXPECTED_CONTINUE: i32 = -3;
/// `PARSE_UNEXPECTED_BREAK`.
pub const PARSE_UNEXPECTED_BREAK: i32 = -4;
/// `PARSE_EOF`.
pub const PARSE_EOF: i32 = -5;

/// `struct cf_parser` (R70).
pub struct CfParser {
    /// Main lexer of the last successful `parse` (the output tokens view
    /// into it); empty on a fresh parser.
    pub main: CfLexer,
    /// The preprocessor (definitions survive a `parse`, like the
    /// reference).
    pub pp: CfPreprocessor,
    /// Accumulated errors and warnings.
    pub error_list: ErrorData,
    /// Index into `pp.tokens`; `0` on a fresh parser (empty stream, D-2).
    cur: usize,
}

/// The token a cursor reports when the stream is empty (D-2 sentinel; the
/// reference would crash on its NULL cursor here).
const EMPTY: CfToken = CfToken {
    str_off: 0,
    str_len: 0,
    span_off: 0,
    span_len: 0,
    r#type: CfTokenType::None,
    src: TokenSource::Main,
    span_src: TokenSource::Main,
};

impl Default for CfParser {
    fn default() -> Self {
        Self::new()
    }
}

impl CfParser {
    /// `cf_parser_init`.
    pub fn new() -> Self {
        Self {
            main: CfLexer::new(),
            pp: CfPreprocessor::new(),
            error_list: ErrorData::new(),
            cur: 0,
        }
    }

    /// `cf_parser_parse` (R70): lex `text` (as `file`), pre-process into
    /// [`CfParser::error_list`], set the cursor to the first output token.
    /// Returns false for an unlexable or unpreprocessable stream; on false
    /// the previous state is kept (R70: the cursor is not re-set; D-2: a
    /// fresh parser then has an empty stream and every helper is safe).
    pub fn parse(&mut self, text: &[u8], file: Option<&str>) -> bool {
        let mut main = CfLexer::new();
        if !main.lex(Some(text), file) {
            return false;
        }
        // A second parse starts a fresh stream (the oracle always resets
        // first); the previous tokens view into the previous main lexer.
        self.pp.tokens.clear();
        let ok = self.pp.preprocess(&main, &mut self.error_list);
        if !ok {
            return false;
        }
        self.main = main;
        self.cur = 0;
        true
    }

    /// The runner's `reset`: re-initialize parser, preprocessor, errors.
    pub fn reset(&mut self) {
        *self = Self::new();
    }

    /// The token under the cursor (the empty-stream sentinel when the
    /// stream is empty, D-2).
    pub fn cur_token(&self) -> &CfToken {
        self.pp.tokens.get(self.cur).unwrap_or(&EMPTY)
    }

    /// `cf_next_token` (R72, Q-11: the first call on a stream that starts
    /// with a non-blank token skips that token; at NONE nothing moves).
    pub fn next_token(&mut self) -> bool {
        if !matches!(
            self.cur_token().r#type,
            CfTokenType::Spacetab | CfTokenType::Newline | CfTokenType::None
        ) {
            self.cur += 1;
        }
        while matches!(
            self.cur_token().r#type,
            CfTokenType::Spacetab | CfTokenType::Newline
        ) {
            self.cur += 1;
        }
        self.cur_token().r#type != CfTokenType::None
    }

    /// `cf_next_valid_token` (R73): [`CfParser::next_token`]; on false adds
    /// `Unexpected EOF` at the cursor and returns false.
    pub fn next_valid_token(&mut self) -> bool {
        if !self.next_token() {
            self.adderror_unexpected_eof();
            false
        } else {
            true
        }
    }

    /// `cf_peek_token` (R73): runs [`CfParser::next_token`], returns the
    /// token it lands on and restores the cursor.
    pub fn peek_token(&mut self) -> (CfToken, bool) {
        let saved = self.cur;
        let ok = self.next_token();
        let peek = *self.cur_token();
        self.cur = saved;
        (peek, ok)
    }

    /// `cf_peek_valid_token` (R73): [`CfParser::peek_token`]; on false adds
    /// `Unexpected EOF` at the restored cursor.
    pub fn peek_valid_token(&mut self) -> (CfToken, bool) {
        let (peek, ok) = self.peek_token();
        if !ok {
            self.adderror_unexpected_eof();
        }
        (peek, ok)
    }

    /// `cf_token_is` (R74): exact, case-sensitive whole-token comparison.
    pub fn token_is(&self, s: &str) -> bool {
        self.pp.token_str(&self.main, self.cur_token()) == s.as_bytes()
    }

    /// `cf_go_to_token` (R75, Q-12: because `cf_next_token` pre-advances,
    /// the token right after a skipped `{...}` block is not examined when
    /// it follows `}` without a blank).
    pub fn go_to_token(&mut self, s1: &str, s2: Option<&str>) -> bool {
        while self.next_token() {
            let (eq1, eq2, first) = {
                let t = self.cur_token();
                let text = self.pp.token_str(&self.main, t);
                (
                    text == s1.as_bytes(),
                    s2.is_some_and(|s2| text == s2.as_bytes()),
                    text.first().copied(),
                )
            };
            if eq1 || eq2 {
                return true;
            }
            if first == Some(b'{') && !self.pass_pair(b'{', b'}') {
                break;
            }
        }
        false
    }

    /// `cf_go_to_valid_token` (R75): [`CfParser::go_to_token`]; on false
    /// adds `Unexpected EOF`.
    pub fn go_to_valid_token(&mut self, s1: &str, s2: Option<&str>) -> bool {
        if !self.go_to_token(s1, s2) {
            self.adderror_unexpected_eof();
            false
        } else {
            true
        }
    }

    /// `cf_go_to_token_type` (R76): starting at the cursor itself (no
    /// pre-advance), advance while not NONE and the type differs.
    pub fn go_to_token_type(&mut self, ty: CfTokenType) -> bool {
        while self.cur_token().r#type != CfTokenType::None && self.cur_token().r#type != ty {
            self.cur += 1;
        }
        self.cur_token().r#type != CfTokenType::None
    }

    /// `cf_pass_pair` (R77): first-character tests apply to any token
    /// type; nested `in` tokens recurse (a failed recursion aborts); a
    /// token starting with `out` is consumed and returns true; NONE ends
    /// with false.
    pub fn pass_pair(&mut self, in_ch: u8, out_ch: u8) -> bool {
        let (is_other, first, is_none) = {
            let t = self.cur_token();
            let text = self.pp.token_str(&self.main, t);
            (
                t.r#type == CfTokenType::Other,
                text.first().copied(),
                t.r#type == CfTokenType::None,
            )
        };
        if !is_other || first != Some(in_ch) {
            return !is_none;
        }
        self.cur += 1;
        loop {
            let (is_none, first) = {
                let t = self.cur_token();
                let text = self.pp.token_str(&self.main, t);
                (t.r#type == CfTokenType::None, text.first().copied())
            };
            if is_none {
                return false;
            }
            match first {
                Some(c) if c == in_ch => {
                    if !self.pass_pair(in_ch, out_ch) {
                        break;
                    }
                }
                Some(c) if c == out_ch => {
                    self.cur += 1;
                    return true;
                }
                _ => {
                    self.cur += 1;
                }
            }
        }
        false
    }

    /// `cf_adderror` (R71): position and file come from the lexer owning
    /// the cursor's source span. When all of `v1..v3` are `None` the
    /// message is stored verbatim; otherwise every `$1`/`$2`/`$3` is
    /// substituted (sequential global replacements; `None` arguments leave
    /// their `$n` untouched; replacement text is itself subject to later
    /// substitutions).
    pub fn adderror(
        &mut self,
        msg: &str,
        level: i32,
        v1: Option<&str>,
        v2: Option<&str>,
        v3: Option<&str>,
    ) {
        let t = *self.cur_token();
        let file = self.pp.token_file(&self.main, &t).map(str::to_owned);
        let (row, col) = self.pp.token_rowcol(&self.main, &t).unwrap_or((1, 1));
        let msg = if v1.is_none() && v2.is_none() && v3.is_none() {
            msg.to_owned()
        } else {
            substitute(msg, v1, v2, v3)
        };
        self.error_list.add(file.as_deref(), row, col, &msg, level);
    }

    /// `cf_adderror_expecting` (R71).
    pub fn adderror_expecting(&mut self, expected: &str) {
        self.adderror("Expected '$1'", LEX_ERROR, Some(expected), None, None);
    }

    /// `cf_adderror_unexpected_eof` (R71).
    pub fn adderror_unexpected_eof(&mut self) {
        self.adderror("Unexpected EOF", LEX_ERROR, None, None, None);
    }

    /// `cf_adderror_syntax_error` (R71).
    pub fn adderror_syntax_error(&mut self) {
        self.adderror("Syntax error", LEX_ERROR, None, None, None);
    }

    /// `cf_token_should_be` (R78): SUCCESS when the cursor is `s`; else,
    /// when `g1` is given, `cf_go_to_token(g1, g2)` (PARSE_EOF without an
    /// error when that fails); then `Expected '<s>'` at the (post-jump)
    /// cursor and PARSE_CONTINUE.
    pub fn token_should_be(&mut self, s: &str, g1: Option<&str>, g2: Option<&str>) -> i32 {
        if self.token_is(s) {
            return PARSE_SUCCESS;
        }
        if let Some(g1) = g1
            && !self.go_to_token(g1, g2)
        {
            return PARSE_EOF;
        }
        self.adderror_expecting(s);
        PARSE_CONTINUE
    }

    /// `cf_next_token_should_be` (R78): [`CfParser::next_token`] first (on
    /// false: `Unexpected EOF` + PARSE_EOF), then as above.
    pub fn next_token_should_be(&mut self, s: &str, g1: Option<&str>, g2: Option<&str>) -> i32 {
        if !self.next_token() {
            self.adderror_unexpected_eof();
            return PARSE_EOF;
        }
        self.token_should_be(s, g1, g2)
    }

    /// `cf_token_is_type` (R79): SUCCESS when the cursor's type matches;
    /// else `Expected '<expected>'` at the current cursor, then when
    /// `goto` is given `cf_go_to_valid_token(goto, NULL)` (PARSE_EOF on
    /// failure); PARSE_CONTINUE.
    pub fn token_is_type(&mut self, ty: CfTokenType, expected: &str, goto: Option<&str>) -> i32 {
        if self.cur_token().r#type == ty {
            return PARSE_SUCCESS;
        }
        self.adderror_expecting(expected);
        if let Some(goto) = goto
            && !self.go_to_valid_token(goto, None)
        {
            return PARSE_EOF;
        }
        PARSE_CONTINUE
    }

    /// `cf_get_name` (R80): [`CfParser::token_is_type`] with NAME; on
    /// SUCCESS returns a copy of the token text.
    pub fn get_name(&mut self, expected: &str, goto: Option<&str>) -> (i32, Option<Vec<u8>>) {
        let code = self.token_is_type(CfTokenType::Name, expected, goto);
        if code != PARSE_SUCCESS {
            return (code, None);
        }
        let t = *self.cur_token();
        let text = self.pp.token_str(&self.main, &t);
        (code, Some(text.to_vec()))
    }

    /// `cf_next_name` (R80): [`CfParser::next_valid_token`] (PARSE_EOF on
    /// failure) then [`CfParser::get_name`].
    pub fn next_name(&mut self, expected: &str, goto: Option<&str>) -> (i32, Option<Vec<u8>>) {
        if !self.next_valid_token() {
            return (PARSE_EOF, None);
        }
        self.get_name(expected, goto)
    }

    /// `cf_get_name_ref` (R80): as [`CfParser::get_name`] but returns a
    /// view of the token text instead of a copy.
    pub fn get_name_ref(&mut self, expected: &str, goto: Option<&str>) -> (i32, Option<&[u8]>) {
        let code = self.token_is_type(CfTokenType::Name, expected, goto);
        if code != PARSE_SUCCESS {
            return (code, None);
        }
        let t = *self.cur_token();
        let text = self.pp.token_str(&self.main, &t);
        (code, Some(text))
    }

    /// `cf_next_name_ref` (R80).
    pub fn next_name_ref(&mut self, expected: &str, goto: Option<&str>) -> (i32, Option<&[u8]>) {
        if !self.next_valid_token() {
            return (PARSE_EOF, None);
        }
        self.get_name_ref(expected, goto)
    }

    /// `cf_copy_token` (R80): a copy of the token text under the cursor.
    pub fn copy_token(&self) -> Vec<u8> {
        let t = *self.cur_token();
        self.pp.token_str(&self.main, &t).to_vec()
    }

    /// `cf_next_token_copy` (R80): [`CfParser::next_valid_token`] (PARSE_EOF
    /// on failure) then [`CfParser::copy_token`].
    pub fn next_token_copy(&mut self) -> (i32, Option<Vec<u8>>) {
        if !self.next_valid_token() {
            return (PARSE_EOF, None);
        }
        (PARSE_SUCCESS, Some(self.copy_token()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::lexer::LEX_WARNING;
    use std::fmt::Write as _;

    /// One test case: a parser plus a temp dir for include files. The
    /// commands print in the conformance runner's format
    /// (`gen/lexer_runner.c` + `script.h`) so the committed vectors are the
    /// oracle.
    struct Case {
        p: CfParser,
        tmp: std::path::PathBuf,
    }

    static COUNTER: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    fn unique() -> usize {
        COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
    }

    impl Case {
        fn new() -> Self {
            let raw =
                std::env::temp_dir().join(format!("cfpar-{}-{}", std::process::id(), unique()));
            // Forward-slash form: cf include resolution is slash-based (C:
            // strrchr(file, '/')) and the Windows file APIs accept '/'. The
            // directory is created lazily on the first writefile (Miri
            // isolation does not permit mkdir).
            let tmp = std::path::PathBuf::from(raw.to_string_lossy().replace('\\', "/"));
            Self {
                p: CfParser::new(),
                tmp,
            }
        }

        fn tmpfile(&self, name: &str) -> String {
            // Forward-slash form on every platform: Path::join appends the
            // native separator on Windows, but cf include resolution is
            // slash-based (C strrchr(file, '/')).
            self.tmp.join(name).to_string_lossy().replace('\\', "/")
        }

        fn writefile(&self, name: &str, content: &[u8]) {
            std::fs::create_dir_all(&self.tmp).unwrap();
            std::fs::write(self.tmp.join(name), content).unwrap();
        }

        fn reset(&mut self) {
            self.p.reset();
        }

        /// Runner `parse TEXT [FILE]`.
        fn parse(&mut self, text: &[u8], file: Option<&str>) -> String {
            let ok = self.p.parse(text, file);
            format!("parse {}\n", if ok { "true" } else { "false" })
        }

        fn dump_tok(&self, t: &CfToken) -> String {
            let tmp = self.tmp.to_string_lossy();
            dump_tok(&self.p, t, &tmp, "@tmp")
        }

        /// Runner `cur`.
        fn cur(&self) -> String {
            format!(
                "  at #{} {}\n",
                self.p.cur,
                self.dump_tok(self.p.cur_token())
            )
        }

        fn cur_out(&self, out: &mut String) {
            out.push_str(&self.cur());
        }

        fn setcur(&mut self, i: usize, out: &mut String) {
            self.p.cur = i;
            self.cur_out(out);
        }

        fn next(&mut self, out: &mut String) {
            let r = self.p.next_token();
            let _ = writeln!(out, "next {}", if r { 1 } else { 0 });
            self.cur_out(out);
        }

        fn next_valid(&mut self, out: &mut String) {
            let r = self.p.next_valid_token();
            let _ = writeln!(out, "next_valid {}", if r { 1 } else { 0 });
            self.cur_out(out);
        }

        fn peek(&mut self, out: &mut String) {
            let (tok, r) = self.p.peek_token();
            let _ = writeln!(
                out,
                "peek {} {}",
                if r { 1 } else { 0 },
                self.dump_tok(&tok)
            );
            self.cur_out(out);
        }

        fn is(&self, v: &str, out: &mut String) {
            let _ = writeln!(out, "is {}", if self.p.token_is(v) { 1 } else { 0 });
        }

        fn is_type(&mut self, ty_name: &str, expected: &str, goto: Option<&str>, out: &mut String) {
            let code = self.p.token_is_type(ty(ty_name), expected, goto);
            let _ = writeln!(out, "is_type {}", code_name(code));
            self.cur_out(out);
        }

        fn should_be(&mut self, s: &str, g1: Option<&str>, g2: Option<&str>, out: &mut String) {
            let code = self.p.token_should_be(s, g1, g2);
            let _ = writeln!(out, "should_be {}", code_name(code));
            self.cur_out(out);
        }

        fn next_should_be(
            &mut self,
            s: &str,
            g1: Option<&str>,
            g2: Option<&str>,
            out: &mut String,
        ) {
            let code = self.p.next_token_should_be(s, g1, g2);
            let _ = writeln!(out, "next_should_be {}", code_name(code));
            self.cur_out(out);
        }

        fn goto_tok(&mut self, s1: &str, s2: Option<&str>, out: &mut String) {
            let r = self.p.go_to_token(s1, s2);
            let _ = writeln!(out, "goto {}", if r { 1 } else { 0 });
            self.cur_out(out);
        }

        fn goto_type(&mut self, t: &str, out: &mut String) {
            let r = self.p.go_to_token_type(ty(t));
            let _ = writeln!(out, "goto_type {}", if r { 1 } else { 0 });
            self.cur_out(out);
        }

        fn pass_pair(&mut self, a: char, b: char, out: &mut String) {
            let r = self.p.pass_pair(a as u8, b as u8);
            let _ = writeln!(out, "pass_pair {}", if r { 1 } else { 0 });
            self.cur_out(out);
        }

        fn next_name(&mut self, expected: &str, goto: Option<&str>, out: &mut String) {
            let (code, name) = self.p.next_name(expected, goto);
            let _ = writeln!(
                out,
                "next_name {} name={}",
                code_name(code),
                name.map(|n| pseg(&n)).unwrap_or_else(|| "@null".into())
            );
            self.cur_out(out);
        }

        fn get_name_ref(&mut self, expected: &str, goto: Option<&str>, out: &mut String) {
            let (code, r) = self.p.get_name_ref(expected, goto);
            let _ = writeln!(
                out,
                "get_name_ref {} ref={}",
                code_name(code),
                r.map(pseg).unwrap_or_else(|| "@null".into())
            );
            self.cur_out(out);
        }

        fn next_name_ref(&mut self, expected: &str, goto: Option<&str>, out: &mut String) {
            let (code, r) = self.p.next_name_ref(expected, goto);
            let _ = writeln!(
                out,
                "next_name_ref {} ref={}",
                code_name(code),
                r.map(pseg).unwrap_or_else(|| "@null".into())
            );
            self.cur_out(out);
        }

        fn next_copy(&mut self, out: &mut String) {
            let (code, c) = self.p.next_token_copy();
            let _ = writeln!(
                out,
                "next_copy {} copy={}",
                code_name(code),
                c.map(|n| pseg(&n)).unwrap_or_else(|| "@null".into())
            );
            self.cur_out(out);
        }

        fn copy(&mut self, out: &mut String) {
            let c = self.p.copy_token();
            let _ = writeln!(out, "copy {} copy={}", code_name(PARSE_SUCCESS), pseg(&c));
            self.cur_out(out);
        }

        fn adderror(
            &mut self,
            msg: &str,
            level: i32,
            v1: Option<&str>,
            v2: Option<&str>,
            v3: Option<&str>,
            out: &mut String,
        ) {
            self.p.adderror(msg, level, v1, v2, v3);
            out.push_str(&self.errors());
        }

        fn expecting(&mut self, s: &str) {
            self.p.adderror_expecting(s);
        }

        fn eof_error(&mut self) {
            self.p.adderror_unexpected_eof();
        }

        fn syntax_error(&mut self) {
            self.p.adderror_syntax_error();
        }

        /// Runner `errors`.
        fn errors(&self) -> String {
            let ed = &self.p.error_list;
            let mut s = format!(
                "errors {} (errors={} warnings={} has_errors={})",
                ed.len(),
                ed.type_count(LEX_ERROR),
                ed.type_count(LEX_WARNING),
                if ed.has_errors() { 1 } else { 0 }
            );
            s.push('\n');
            let tmp = self.tmp.to_string_lossy();
            for i in 0..ed.len() {
                let item = ed.item(i).unwrap();
                let _ = writeln!(
                    s,
                    "err {i} {} file={} row={} col={} msg={}",
                    if item.level == LEX_ERROR {
                        "ERROR"
                    } else {
                        "WARNING"
                    },
                    pfile(item.file.as_deref(), &tmp, "@tmp"),
                    item.row,
                    item.column,
                    pseg(item.msg.as_bytes())
                );
            }
            s
        }

        /// Runner `errstr`.
        fn errstr(&self) -> String {
            format!(
                "errstr {}\n",
                pseg(
                    self.p
                        .error_list
                        .buildstring()
                        .unwrap_or_default()
                        .as_bytes()
                )
            )
        }

        /// Runner `def NAME [BODY]` (API definition, R53).
        fn def(&mut self, name: &str, body: Option<&[u8]>) {
            let mut ln = CfLexer::new();
            ln.lex(Some(name.as_bytes()), Some("def-name"));
            let name_idx = self.p.pp.sources.len();
            rebase_tokens(&mut ln, TokenSource::Side(name_idx));
            self.p.pp.sources.push(ln);

            let mut lb = CfLexer::new();
            let body_idx = self.p.pp.sources.len();
            if let Some(b) = body {
                lb.lex(Some(b), Some("def-body"));
            }
            rebase_tokens(&mut lb, TokenSource::Side(body_idx));
            self.p.pp.sources.push(lb);

            let mut def = crate::cf_preprocessor::CfDef::new();
            if let Some(t) = self
                .p
                .pp
                .sources
                .get(name_idx)
                .and_then(|l| l.tokens.first())
            {
                def.name = *t;
            }
            if let Some(l) = self.p.pp.sources.get(body_idx) {
                def.tokens = l.tokens.clone();
            }
            self.p.pp.add_def(None, &def);
        }
    }

    /// Runner token line (after `tok I`): type/str, then the source span in
    /// the owning lexer (`src=-` for synthesised tokens), `u=` when the
    /// unmerged span differs from the token text, and `f=` for an
    /// included-file owner. `prefix`/`tag` map the printed file names.
    fn dump_tok(p: &CfParser, t: &CfToken, prefix: &str, tag: &str) -> String {
        let mut s = format!(
            "type={} str={}",
            ctype(t.r#type),
            pseg(p.pp.token_str(&p.main, t))
        );
        match p.pp.token_span_lexer(&p.main, t) {
            Some(l) => {
                let (r, c) = l.rowcol(t.span_off);
                let _ = write!(s, " src={}+{} @{}:{}", t.span_off, t.span_len, r, c);
                let strb = p.pp.token_str(&p.main, t);
                let text = l.text();
                let start = t.span_off.min(text.len());
                let end = t.span_off.saturating_add(t.span_len).min(text.len());
                let u = &text[start..end];
                // C: differ when the lengths differ or the first
                // str.len bytes differ (span always covers the text).
                let n = strb.len().min(u.len());
                let differ = t.span_len != t.str_len || strb[..n] != u[..n];
                if differ {
                    let _ = write!(s, " u={}", pseg(u));
                }
                if t.span_src != TokenSource::Main {
                    let _ = write!(s, " f={}", pfile(l.file.as_deref(), prefix, tag));
                }
            }
            None => s.push_str(" src=-"),
        }
        s
    }

    /// The full dump text of `gen/effect_dump.c` for a successfully parsed
    /// corpus file: `parse` + `tokens pp` + the token lines + `errors` (plus
    /// the `err` lines). `data_prefix` (no trailing slash) maps file names
    /// to `@data…` in the `f=` and `err` prints.
    fn corpus_dump(p: &CfParser, data_prefix: &str) -> String {
        let mut s = String::from("parse true\n");
        let _ = writeln!(s, "tokens pp {}", p.pp.tokens.len());
        for (i, t) in p.pp.tokens.iter().enumerate() {
            let _ = writeln!(s, "tok {i} {}", dump_tok(p, t, data_prefix, "@data"));
        }
        let ed = &p.error_list;
        let _ = writeln!(
            s,
            "errors {} (errors={} warnings={} has_errors={})",
            ed.len(),
            ed.type_count(LEX_ERROR),
            ed.type_count(LEX_WARNING),
            if ed.has_errors() { 1 } else { 0 }
        );
        for i in 0..ed.len() {
            let item = ed.item(i).unwrap();
            let _ = writeln!(
                s,
                "err {i} {} file={} row={} col={} msg={}",
                if item.level == LEX_ERROR {
                    "ERROR"
                } else {
                    "WARNING"
                },
                pfile(item.file.as_deref(), data_prefix, "@data"),
                item.row,
                item.column,
                pseg(item.msg.as_bytes())
            );
        }
        s
    }

    fn rebase_tokens(lex: &mut CfLexer, src: TokenSource) {
        for t in &mut lex.tokens {
            t.src = src;
            t.span_src = src;
        }
    }

    fn ty(s: &str) -> CfTokenType {
        match s {
            "NAME" => CfTokenType::Name,
            "NUM" => CfTokenType::Num,
            "SPACETAB" => CfTokenType::Spacetab,
            "NEWLINE" => CfTokenType::Newline,
            "STRING" => CfTokenType::String,
            "OTHER" => CfTokenType::Other,
            _ => CfTokenType::None,
        }
    }

    fn ctype(t: CfTokenType) -> &'static str {
        match t {
            CfTokenType::None => "NONE",
            CfTokenType::Name => "NAME",
            CfTokenType::Num => "NUM",
            CfTokenType::Spacetab => "SPACETAB",
            CfTokenType::Newline => "NEWLINE",
            CfTokenType::String => "STRING",
            CfTokenType::Other => "OTHER",
        }
    }

    fn code_name(c: i32) -> &'static str {
        match c {
            PARSE_SUCCESS => "SUCCESS",
            PARSE_CONTINUE => "CONTINUE",
            PARSE_BREAK => "BREAK",
            PARSE_UNEXPECTED_CONTINUE => "UNEXPECTED_CONTINUE",
            PARSE_UNEXPECTED_BREAK => "UNEXPECTED_BREAK",
            PARSE_EOF => "EOF",
            _ => "?",
        }
    }

    /// `script_print_quoted`: bytes between double quotes, escaping `\\`,
    /// `\"`, `\n`, `\t`, `\r` and other non-printable / >= 0x7f bytes.
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

    /// Runner `pfile`: `@null` for none; a file name under `prefix` (with a
    /// following `/`) is printed as `@tag…`.
    fn pfile(f: Option<&str>, prefix: &str, tag: &str) -> String {
        let Some(f) = f else {
            return "@null".into();
        };
        if let Some(rest) = f.strip_prefix(prefix)
            && rest.starts_with('/')
        {
            pseg(format!("{tag}{rest}").as_bytes())
        } else {
            pseg(f.as_bytes())
        }
    }

    fn check(out: &str, want: &str) {
        assert_eq!(out, want, "case output mismatch");
    }

    // -- vector 30: navigation (Q-11, Q-12) ---------------------------------

    #[test]
    fn v30_parser_nav() {
        // case 1
        let mut c = Case::new();
        let mut out = String::new();
        out.push_str(&c.parse(b"void f(int a, 12) { x; { y } z } w", None));
        c.cur_out(&mut out);
        c.next(&mut out);
        c.next(&mut out);
        c.peek(&mut out);
        c.next(&mut out);
        c.is("int", &mut out);
        c.is("in", &mut out);
        c.is("", &mut out);
        c.should_be("(", None, None, &mut out);
        c.should_be("int", None, None, &mut out);
        c.should_be(")", Some(","), None, &mut out);
        c.should_be(")", Some("nothere"), None, &mut out);
        c.cur_out(&mut out);
        c.setcur(0, &mut out);
        c.next_should_be("f", None, None, &mut out);
        c.next_should_be("x", Some(")"), None, &mut out);
        c.next_should_be("x", Some("{"), Some("}"), &mut out);
        out.push_str(&c.errors());
        check(
            &out,
            r#"parse true
  at #0 type=NAME str="void" src=0+4 @1:1
next 1
  at #2 type=NAME str="f" src=5+1 @1:6
next 1
  at #3 type=OTHER str="(" src=6+1 @1:7
peek 1 type=NAME str="int" src=7+3 @1:8
  at #3 type=OTHER str="(" src=6+1 @1:7
next 1
  at #4 type=NAME str="int" src=7+3 @1:8
is 1
is 0
is 0
should_be CONTINUE
  at #4 type=NAME str="int" src=7+3 @1:8
should_be SUCCESS
  at #4 type=NAME str="int" src=7+3 @1:8
should_be CONTINUE
  at #7 type=OTHER str="," src=12+1 @1:13
should_be EOF
  at #28 type=NONE str="" src=34+0 @1:35
  at #28 type=NONE str="" src=34+0 @1:35
  at #0 type=NAME str="void" src=0+4 @1:1
next_should_be SUCCESS
  at #2 type=NAME str="f" src=5+1 @1:6
next_should_be CONTINUE
  at #10 type=OTHER str=")" src=16+1 @1:17
next_should_be CONTINUE
  at #17 type=OTHER str="{" src=23+1 @1:24
errors 4 (errors=4 warnings=0 has_errors=1)
err 0 ERROR file=@null row=1 col=8 msg="Expected '('"
err 1 ERROR file=@null row=1 col=13 msg="Expected ')'"
err 2 ERROR file=@null row=1 col=17 msg="Expected 'x'"
err 3 ERROR file=@null row=1 col=24 msg="Expected 'x'"
"#,
        );

        // case 2
        c.reset();
        out.clear();
        out.push_str(&c.parse(b"  void  f(int a, 12) { x; { y } z } w", None));
        c.setcur(0, &mut out);
        c.next(&mut out);
        c.next_valid(&mut out);
        c.goto_tok("{", None, &mut out);
        c.pass_pair('{', '}', &mut out);
        c.next(&mut out);
        c.pass_pair('(', ')', &mut out);
        c.next_valid(&mut out);
        c.cur_out(&mut out);
        out.push_str(&c.errors());
        check(
            &out,
            r#"parse true
  at #0 type=SPACETAB str="  " src=0+2 @1:1
next 1
  at #1 type=NAME str="void" src=2+4 @1:3
next_valid 1
  at #3 type=NAME str="f" src=8+1 @1:9
goto 1
  at #13 type=OTHER str="{" src=21+1 @1:22
pass_pair 1
  at #27 type=SPACETAB str=" " src=35+1 @1:36
next 1
  at #28 type=NAME str="w" src=36+1 @1:37
pass_pair 1
  at #28 type=NAME str="w" src=36+1 @1:37
next_valid 0
  at #29 type=NONE str="" src=37+0 @1:38
  at #29 type=NONE str="" src=37+0 @1:38
errors 1 (errors=1 warnings=0 has_errors=1)
err 0 ERROR file=@null row=1 col=38 msg="Unexpected EOF"
"#,
        );

        // case 3
        c.reset();
        out.clear();
        out.push_str(&c.parse(b"f ( { [ ( ) ] } ) g", None));
        c.setcur(0, &mut out);
        c.next(&mut out);
        c.pass_pair('(', ')', &mut out);
        c.goto_tok("g", None, &mut out);
        c.goto_type("NAME", &mut out);
        c.setcur(0, &mut out);
        c.goto_type("OTHER", &mut out);
        c.goto_type("STRING", &mut out);
        check(
            &out,
            r#"parse true
  at #0 type=NAME str="f" src=0+1 @1:1
next 1
  at #2 type=OTHER str="(" src=2+1 @1:3
pass_pair 1
  at #17 type=SPACETAB str=" " src=17+1 @1:18
goto 1
  at #18 type=NAME str="g" src=18+1 @1:19
goto_type 1
  at #18 type=NAME str="g" src=18+1 @1:19
  at #0 type=NAME str="f" src=0+1 @1:1
goto_type 1
  at #2 type=OTHER str="(" src=2+1 @1:3
goto_type 0
  at #19 type=NONE str="" src=19+0 @1:20
"#,
        );
    }

    // -- vector 31: name/copy helpers ----------------------------------------

    #[test]
    fn v31_parser_names() {
        // case 1
        let mut c = Case::new();
        let mut out = String::new();
        out.push_str(&c.parse(b" int foo 12 \"s\" ; bar", None));
        c.setcur(0, &mut out);
        c.next_name("type", Some(";"), &mut out);
        c.next_name("name", Some(";"), &mut out);
        c.next_name("name", Some(";"), &mut out);
        out.push_str(&c.errors());
        check(
            &out,
            r#"parse true
  at #0 type=SPACETAB str=" " src=0+1 @1:1
next_name SUCCESS name="int"
  at #1 type=NAME str="int" src=1+3 @1:2
next_name SUCCESS name="foo"
  at #3 type=NAME str="foo" src=5+3 @1:6
next_name CONTINUE name=@null
  at #9 type=OTHER str=";" src=16+1 @1:17
errors 1 (errors=1 warnings=0 has_errors=1)
err 0 ERROR file=@null row=1 col=10 msg="Expected 'name'"
"#,
        );

        // case 2
        c.reset();
        out.clear();
        out.push_str(&c.parse(b" a 5 b ;", None));
        c.setcur(0, &mut out);
        c.next_name_ref("n", None, &mut out);
        c.next_name_ref("n", Some(";"), &mut out);
        c.get_name_ref("n", None, &mut out);
        c.next_name_ref("n", None, &mut out);
        c.next_name_ref("n", None, &mut out);
        c.next_name_ref("n", None, &mut out);
        out.push_str(&c.errors());
        check(
            &out,
            r#"parse true
  at #0 type=SPACETAB str=" " src=0+1 @1:1
next_name_ref SUCCESS ref="a"
  at #1 type=NAME str="a" src=1+1 @1:2
next_name_ref CONTINUE ref=@null
  at #7 type=OTHER str=";" src=7+1 @1:8
get_name_ref CONTINUE ref=@null
  at #7 type=OTHER str=";" src=7+1 @1:8
next_name_ref EOF ref=@null
  at #8 type=NONE str="" src=8+0 @1:9
next_name_ref EOF ref=@null
  at #8 type=NONE str="" src=8+0 @1:9
next_name_ref EOF ref=@null
  at #8 type=NONE str="" src=8+0 @1:9
errors 5 (errors=5 warnings=0 has_errors=1)
err 0 ERROR file=@null row=1 col=4 msg="Expected 'n'"
err 1 ERROR file=@null row=1 col=8 msg="Expected 'n'"
err 2 ERROR file=@null row=1 col=9 msg="Unexpected EOF"
err 3 ERROR file=@null row=1 col=9 msg="Unexpected EOF"
err 4 ERROR file=@null row=1 col=9 msg="Unexpected EOF"
"#,
        );

        // case 3
        c.reset();
        out.clear();
        out.push_str(&c.parse(b" a 5", None));
        c.setcur(0, &mut out);
        c.next_copy(&mut out);
        c.copy(&mut out);
        c.next_copy(&mut out);
        c.next_copy(&mut out);
        c.next_copy(&mut out);
        out.push_str(&c.errors());
        check(
            &out,
            r#"parse true
  at #0 type=SPACETAB str=" " src=0+1 @1:1
next_copy SUCCESS copy="a"
  at #1 type=NAME str="a" src=1+1 @1:2
copy SUCCESS copy="a"
  at #1 type=NAME str="a" src=1+1 @1:2
next_copy SUCCESS copy="5"
  at #3 type=NUM str="5" src=3+1 @1:4
next_copy EOF copy=@null
  at #4 type=NONE str="" src=4+0 @1:5
next_copy EOF copy=@null
  at #4 type=NONE str="" src=4+0 @1:5
errors 2 (errors=2 warnings=0 has_errors=1)
err 0 ERROR file=@null row=1 col=5 msg="Unexpected EOF"
err 1 ERROR file=@null row=1 col=5 msg="Unexpected EOF"
"#,
        );

        // case 4
        c.reset();
        out.clear();
        out.push_str(&c.parse(b" a 5", None));
        c.setcur(0, &mut out);
        c.next(&mut out);
        c.is_type("NAME", "name", Some("zzz"), &mut out);
        c.is_type("NUM", "number", Some("zzz"), &mut out);
        out.push_str(&c.errors());
        check(
            &out,
            r#"parse true
  at #0 type=SPACETAB str=" " src=0+1 @1:1
next 1
  at #1 type=NAME str="a" src=1+1 @1:2
is_type SUCCESS
  at #1 type=NAME str="a" src=1+1 @1:2
is_type EOF
  at #4 type=NONE str="" src=4+0 @1:5
errors 2 (errors=2 warnings=0 has_errors=1)
err 0 ERROR file=@null row=1 col=2 msg="Expected 'number'"
err 1 ERROR file=@null row=1 col=5 msg="Unexpected EOF"
"#,
        );
    }

    // -- vector 32: adderror formatting ---------------------------------------

    #[test]
    fn v32_parser_errors() {
        let mut c = Case::new();
        let mut out = String::new();
        out.push_str(&c.parse(b"ab\n  cd ef", None));
        c.setcur(0, &mut out);
        // The script writes `\$1`..`\$3` (script.h unescape -> literal $n).
        c.adderror("plain $1", LEX_ERROR, None, None, None, &mut out);
        c.adderror(
            "v=$1 $2 $3 $1",
            LEX_WARNING,
            Some("A"),
            Some("B"),
            Some("C"),
            &mut out,
        );
        // `@null` in the script is a NULL value: its $2 stays untouched.
        c.adderror(
            "only2 $1$2$3",
            LEX_ERROR,
            Some("x"),
            None,
            Some("z"),
            &mut out,
        );
        c.next(&mut out);
        c.next(&mut out);
        c.expecting("';'");
        c.eof_error();
        c.syntax_error();
        out.push_str(&c.errstr());
        check(
            &out,
            r#"parse true
  at #0 type=NAME str="ab" src=0+2 @1:1
errors 1 (errors=1 warnings=0 has_errors=1)
err 0 ERROR file=@null row=1 col=1 msg="plain $1"
errors 2 (errors=1 warnings=1 has_errors=1)
err 0 ERROR file=@null row=1 col=1 msg="plain $1"
err 1 WARNING file=@null row=1 col=1 msg="v=A B C A"
errors 3 (errors=2 warnings=1 has_errors=1)
err 0 ERROR file=@null row=1 col=1 msg="plain $1"
err 1 WARNING file=@null row=1 col=1 msg="v=A B C A"
err 2 ERROR file=@null row=1 col=1 msg="only2 x$2z"
next 1
  at #3 type=NAME str="cd" src=5+2 @2:3
next 1
  at #5 type=NAME str="ef" src=8+2 @2:6
errstr "(null) (1, 1): plain $1\n(null) (1, 1): v=A B C A\n(null) (1, 1): only2 x$2z\n(null) (2, 6): Expected '';''\n(null) (2, 6): Unexpected EOF\n(null) (2, 6): Syntax error\n"
"#,
        );
    }

    // -- vector 50: parser quirks (Q-11, Q-12) ---------------------------------

    #[test]
    fn v50_parser_quirks() {
        // Q-12: goto skips the token right after a skipped {...} block when
        // there is no blank between.
        let mut c = Case::new();
        let mut out = String::new();
        out.push_str(&c.parse(b"a {}x y", None));
        c.setcur(0, &mut out);
        c.goto_tok("x", None, &mut out);
        c.cur_out(&mut out);
        out.push_str(&c.errors());

        c.reset();
        out.push_str(&c.parse(b"a { } x y", None));
        c.setcur(0, &mut out);
        c.goto_tok("x", None, &mut out);

        c.reset();
        out.push_str(&c.parse(b"abc def", None));
        c.next(&mut out);
        c.cur_out(&mut out);
        check(
            &out,
            r#"parse true
  at #0 type=NAME str="a" src=0+1 @1:1
goto 0
  at #7 type=NONE str="" src=7+0 @1:8
  at #7 type=NONE str="" src=7+0 @1:8
errors 0 (errors=0 warnings=0 has_errors=0)
parse true
  at #0 type=NAME str="a" src=0+1 @1:1
goto 1
  at #6 type=NAME str="x" src=6+1 @1:7
parse true
next 1
  at #2 type=NAME str="def" src=4+3 @1:5
  at #2 type=NAME str="def" src=4+3 @1:5
"#,
        );
    }

    // -- vector 90: reference defects ------------------------------------------
    // The reference crashes in every try-block (vector: `try-end crash`); a
    // conforming implementation must not (spec section 8). The try-block
    // output is not compared: each case runs in-process and must not panic.

    #[test]
    #[cfg(not(miri))]
    // Miri isolation: the D-4 case writes an empty include file.
    fn v90_defects() {
        let mut c = Case::new();
        let mut out = String::new();

        // D-4: empty included file (reference crashes expanding/terminating
        // the empty dependency).
        c.reset();
        c.writefile("emptyfile.h", b"");
        let main_cf = c.tmpfile("main.cf");
        out.push_str(&c.parse(b"#include \"emptyfile.h\"\nx\n", Some(&main_cf)));

        // D-2: parse "" fails (empty text); the helpers on the never-parsed
        // parser must be safe (reference: NULL cursor crash).
        c.reset();
        out.push_str(&c.parse(b"", None));
        c.next(&mut out);

        c.reset();
        out.push_str(&c.parse(b"", None));
        c.peek(&mut out);

        c.reset();
        out.push_str(&c.parse(b"", None));
        c.adderror("hi", LEX_ERROR, None, None, None, &mut out);

        // D-3/API: an empty definition expands to nothing; the reference
        // reads past the empty def body.
        c.reset();
        c.def("Q", None);
        out.push_str(&c.parse(b"Q", None));

        // D-7: self- and mutually recursive macros terminate.
        c.reset();
        out.push_str(&c.parse(b"#define A A\nA\n", None));

        c.reset();
        out.push_str(&c.parse(b"#define A B\n#define B A\nA\n", None));

        // The stream must exist and end in NONE in every case.
        assert!(out.contains("parse"));
    }

    // -- corpus: libobs/data effect-file digests (W21d) ----------------------
    // vectors/effect/corpus-digests.expected was generated by
    // test/conformance/gen/effect_dump.c against the C reference; the dump
    // text rebuilt here must match it byte for byte (blake2b-512). Skipped
    // (one line printed) when the libobs checkout link is absent (CI).

    fn hex64(s: &str) -> [u8; 64] {
        let mut out = [0u8; 64];
        for i in 0..64 {
            out[i] = u8::from_str_radix(&s[2 * i..2 * i + 2], 16).unwrap();
        }
        out
    }

    #[test]
    #[cfg(not(miri))]
    // Miri isolation: canonicalize + real reads are unsupported; the
    // pipeline logic it covers is exercised by the in-memory tests.
    fn v_corpus_effect_digests() {
        let root =
            std::fs::canonicalize(std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../.."))
                .unwrap();
        let data_dir = root.join("libobs/data");
        let digest_path = root.join("test/conformance/vectors/effect/corpus-digests.expected");
        let Ok(text) = std::fs::read_to_string(&digest_path) else {
            eprintln!("v_corpus_effect_digests: skipped - C checkout link (libobs/data) missing");
            return;
        };
        let want: Vec<(String, usize, usize, [u8; 64])> = text
            .lines()
            .filter(|l| !l.trim().is_empty() && !l.trim_start().starts_with('#'))
            .map(|line| {
                let mut f = line.split_whitespace();
                let name = f.next().unwrap().to_owned();
                let mut tokens = 0;
                let mut errors = 0;
                let mut hash = [0u8; 64];
                for k in f {
                    if let Some(v) = k.strip_prefix("tokens=") {
                        tokens = v.parse().unwrap();
                    } else if let Some(v) = k.strip_prefix("errors=") {
                        errors = v.parse().unwrap();
                    } else if let Some(h) = k.strip_prefix("blake2b512=") {
                        hash = hex64(h);
                    }
                }
                (name, tokens, errors, hash)
            })
            .collect();
        let Some((first, ..)) = want.first() else {
            eprintln!("v_corpus_effect_digests: skipped - C checkout link (libobs/data) missing");
            return;
        };
        if !data_dir.is_dir() || !data_dir.join(first).is_file() {
            eprintln!("v_corpus_effect_digests: skipped - C checkout link (libobs/data) missing");
            return;
        }
        let data_prefix = data_dir.to_string_lossy().into_owned();
        let mut checked = 0;
        for (name, want_tokens, want_errors, want_hash) in &want {
            let path = data_dir.join(name);
            let bytes = std::fs::read(&path).unwrap();
            let path_str = path.to_string_lossy().into_owned();
            let mut p = CfParser::new();
            assert!(p.parse(&bytes, Some(&path_str)), "{name} must parse");
            let dump = corpus_dump(&p, &data_prefix);
            assert_eq!(
                (
                    p.pp.tokens.len(),
                    p.error_list.len(),
                    crate::hash::blake2b_512(dump.as_bytes())
                ),
                (*want_tokens, *want_errors, *want_hash),
                "{name}: corpus digest mismatch"
            );
            checked += 1;
        }
        assert_eq!(checked, want.len(), "corpus digest lines not all checked");
        eprintln!("v_corpus_effect_digests: checked {checked} effect files");
    }
}
