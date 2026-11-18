//! S-011 section 5.3 (R40-R54): the C-style preprocessor on top of the
//! cf-lexer token stream.
//!
//! Port of `libobs/util/cf-lexer.c` (`cf_preprocess` and friends). Behaviour
//! is pinned by the committed conformance vectors `test/conformance/vectors/
//! lexer/2*.expected`, `50-quirks.expected` (pp parts) and
//! `90-defects.expected`. Quirks reproduced: Q-6 (first definition wins),
//! Q-7 (open-file error embeds the rest of the buffer), Q-8/Q-9 (macro
//! parameter pitfalls), Q-13 (spurious `#elif` error after a true block),
//! Q-14 (`sscanf("%i")` reads the rest of the buffer). Divergences from the
//! reference, per section 8: D-3 (empty def token list expands to nothing),
//! D-4 (empty include = no tokens; cyclic includes bounded with an error),
//! D-5 (duplicate `add_def` warns, never crashes), D-6 (argument collection
//! stops at NONE), D-7 (self/mutually recursive macros terminate via an
//! expanding-name stack instead of overflowing).

use super::cf_lexer::{CfLexer, CfToken, CfTokenType, TokenSource};
use super::lexer::{ErrorData, LEX_ERROR, LEX_WARNING};

/// `struct cf_def`: a `#define` or an API-level definition (R53).
///
/// Body layout: directive defines are `[leading blank, body..., NONE,
/// trailing blank]` (the blank after the NONE is dead, like in the
/// reference); API defines carry exactly the tokens the caller gave (no
/// leading blank, terminated by a NONE token, or empty per D-3).
#[derive(Clone, Debug, Default)]
pub struct CfDef {
    /// The defined name (a view into its owning lexer).
    pub name: CfToken,
    /// Parameter names of a function-like macro.
    pub params: Vec<CfToken>,
    /// Body tokens; the expansion loop stops at the first NONE token.
    pub tokens: Vec<CfToken>,
    /// Function-like (parameterised) macro.
    pub r#macro: bool,
}

impl CfDef {
    /// Empty def (C `cf_def_init`).
    pub fn new() -> Self {
        Self::default()
    }

    /// `cf_def_addtoken`.
    pub fn add_token(&mut self, token: CfToken) {
        self.tokens.push(token);
    }

    /// `cf_def_addparam`.
    pub fn add_param(&mut self, token: CfToken) {
        self.params.push(token);
    }
}

/// `struct cf_preprocessor` (R40).
#[derive(Default)]
pub struct CfPreprocessor {
    /// Active definitions (first match wins, Q-6).
    pub defines: Vec<CfDef>,
    /// System include directories (accepted, unused: `<...>` includes are
    /// ignored per R48).
    pub sys_include_dirs: Vec<String>,
    /// Included files, in first-include order; indexed by `TokenSource::Dep`.
    pub dependencies: Vec<CfLexer>,
    /// Extra token sources owned by the caller (API definitions); indexed by
    /// `TokenSource::Side`.
    pub sources: Vec<CfLexer>,
    /// The preprocessed token stream (ends with the main NONE token).
    pub tokens: Vec<CfToken>,
    ignore_state: bool,
    /// Include paths currently being processed (D-4 bound); private.
    pending_includes: Vec<String>,
}

impl CfPreprocessor {
    /// `cf_preprocessor_init`.
    pub fn new() -> Self {
        Self::default()
    }

    /// `cf_preprocessor_add_sys_include_dir` (R48).
    pub fn add_sys_include_dir(&mut self, dir: &str) {
        self.sys_include_dirs.push(dir.to_owned());
    }

    /// Look up a definition by name (R42, Q-6: first match wins).
    pub fn get_def(&self, main: &CfLexer, name: &[u8]) -> Option<&CfDef> {
        self.defines
            .iter()
            .find(|d| self.resolve(main, &d.name) == name)
    }

    /// `cf_preprocess_get_def_value` (R47): the first non-blank body token as
    /// an integer, following NAME defines (D-7: a visited-name stack makes
    /// self-referencing defines terminate with `false`).
    pub fn get_def_value(&self, main: &CfLexer, name: &[u8], value: &mut i64) -> bool {
        let mut visited = Vec::new();
        self.def_value(main, name, value, &mut visited)
    }

    fn def_value(
        &self,
        main: &CfLexer,
        name: &[u8],
        value: &mut i64,
        visited: &mut Vec<Vec<u8>>,
    ) -> bool {
        let Some(def) = self.get_def(main, name) else {
            return false;
        };
        if def.r#macro {
            return false;
        }
        if visited.iter().any(|n| n.as_slice() == name) {
            return false;
        }
        visited.push(name.to_vec());
        let result = def.tokens.iter().find_map(|t| match t.r#type {
            CfTokenType::Spacetab => None,
            CfTokenType::Num => {
                *value = parse_c_int(self.resolve_tail(main, t));
                Some(true)
            }
            CfTokenType::Name => Some(self.def_value(main, self.resolve(main, t), value, visited)),
            _ => Some(false),
        });
        visited.pop();
        result.unwrap_or(false)
    }

    /// `cf_preprocess` (R40): pre-process `main`'s tokens, appending the
    /// result to `tokens` (including the stream's NONE terminator, which is
    /// appended again on a second call, like the reference). Returns false
    /// for an empty token list, otherwise `!main.unexpected_eof`.
    ///
    /// `tokens` keep views into `main`, the dependency lexers and `sources`;
    /// all of those must outlive the use of `tokens`.
    pub fn preprocess(&mut self, main: &CfLexer, ed: &mut ErrorData) -> bool {
        if main.tokens.is_empty() {
            return false;
        }
        self.pending_includes.clear();
        let mut out = Vec::new();
        let mut cur = Cur::new(&main.tokens, &main.reformatted);
        block(self, main, ed, &mut out, None, false, &mut cur);
        self.tokens.extend(out);
        self.tokens.push(*main.tokens.last().unwrap());
        !main.unexpected_eof
    }

    /// `cf_preprocessor_add_def` (R53, D-5): add or replace a definition by
    /// name. A duplicate replaces the old entry and, when an error stream is
    /// supplied, records the two reference warnings (positions unspecified;
    /// the reference crashes here per D-5, the Rust API must not).
    pub fn add_def(&mut self, ed: Option<&mut ErrorData>, def: &CfDef) {
        // The name bytes of an API definition live in one of `sources`;
        // `Main`/`Dep` names have no resolvable owner here, like the
        // reference's NULL lex pointers (D-5).
        let main = None;
        let new_name = self.resolve_opt(main, &def.name).to_vec();
        if let Some(existing) = self
            .defines
            .iter()
            .position(|d| self.resolve_opt(main, &d.name) == new_name.as_slice())
        {
            if let Some(ed) = ed {
                let name = String::from_utf8_lossy(&new_name).into_owned();
                ed.add(
                    None,
                    0,
                    0,
                    &substitute("Token $1 already defined", Some(&name), None, None),
                    LEX_WARNING,
                );
                ed.add(
                    None,
                    0,
                    0,
                    &substitute("Previous definition of $1 is here", Some(&name), None, None),
                    LEX_WARNING,
                );
            }
            self.defines[existing] = def.clone();
        } else {
            self.defines.push(def.clone());
        }
    }

    /// `cf_preprocessor_remove_def` (R54): remove the first definition with
    /// the given name; missing names are a no-op.
    pub fn remove_def(&mut self, main: &CfLexer, name: &str) {
        let name = name.as_bytes();
        if let Some(pos) = self
            .defines
            .iter()
            .position(|d| self.resolve(main, &d.name) == name)
        {
            self.defines.remove(pos);
        }
    }

    // -- token view resolution (R49/R54: positions from the owning lexer) --

    /// The lexer owning a token's views (`None` for `Synth` sources).
    pub fn token_lexer<'x>(&'x self, main: &'x CfLexer, t: &CfToken) -> Option<&'x CfLexer> {
        self.lexer_of(main, t.src)
    }

    /// The lexer owning a token's source span (`None` for `Synth` spans).
    pub fn token_span_lexer<'x>(&'x self, main: &'x CfLexer, t: &CfToken) -> Option<&'x CfLexer> {
        self.lexer_of(main, t.span_src)
    }

    /// The token text (`Synth` tokens carry the one-byte space filler).
    pub fn token_str<'x>(&'x self, main: &'x CfLexer, t: &CfToken) -> &'x [u8] {
        self.resolve(main, t)
    }

    /// The source span bytes (`None` for `Synth` spans).
    pub fn token_span<'x>(&'x self, main: &'x CfLexer, t: &CfToken) -> Option<&'x [u8]> {
        let l = self.token_span_lexer(main, t)?;
        let text = l.text();
        let start = t.span_off.min(text.len());
        let end = t.span_off.saturating_add(t.span_len).min(text.len());
        Some(&text[start..end])
    }

    /// 1-based row/column of the span start in the owning lexer.
    pub fn token_rowcol(&self, main: &CfLexer, t: &CfToken) -> Option<(u32, u32)> {
        self.token_span_lexer(main, t).map(|l| l.rowcol(t.span_off))
    }

    /// The owning lexer's file name.
    pub fn token_file<'x>(&'x self, main: &'x CfLexer, t: &CfToken) -> Option<&'x str> {
        self.token_span_lexer(main, t)
            .and_then(|l| l.file.as_deref())
    }

    fn lexer_of<'x>(&'x self, main: &'x CfLexer, src: TokenSource) -> Option<&'x CfLexer> {
        match src {
            TokenSource::Main => Some(main),
            TokenSource::Dep(i) => self.dependencies.get(i),
            TokenSource::Side(i) => self.sources.get(i),
            TokenSource::Synth => None,
        }
    }

    /// The token text resolved through its owning buffer.
    fn resolve<'x>(&'x self, main: &'x CfLexer, t: &CfToken) -> &'x [u8] {
        self.resolve_opt(Some(main), t)
    }

    fn resolve_opt<'x>(&'x self, main: Option<&'x CfLexer>, t: &CfToken) -> &'x [u8] {
        match t.src {
            TokenSource::Main => main
                .and_then(|m| m.reformatted.get(t.str_off..t.str_off + t.str_len))
                .unwrap_or(&[]),
            TokenSource::Dep(i) => self
                .dependencies
                .get(i)
                .and_then(|d| d.reformatted.get(t.str_off..t.str_off + t.str_len))
                .unwrap_or(&[]),
            TokenSource::Side(i) => self
                .sources
                .get(i)
                .and_then(|d| d.reformatted.get(t.str_off..t.str_off + t.str_len))
                .unwrap_or(&[]),
            // Synth tokens are always the one-byte space filler (R52/R53).
            TokenSource::Synth => b" ",
        }
    }

    /// The rest of the owning buffer from the token start (R47/Q-14: the
    /// reference runs `sscanf("%i")` over the token's str pointer, i.e. to
    /// the end of the buffer, not just the token text).
    fn resolve_tail<'x>(&'x self, main: &'x CfLexer, t: &CfToken) -> &'x [u8] {
        let buf: &'x [u8] = match t.src {
            TokenSource::Main => &main.reformatted,
            TokenSource::Dep(i) => match self.dependencies.get(i) {
                Some(d) => &d.reformatted,
                None => return &[],
            },
            TokenSource::Side(i) => match self.sources.get(i) {
                Some(d) => &d.reformatted,
                None => return &[],
            },
            TokenSource::Synth => return &[],
        };
        buf.get(t.str_off..).unwrap_or(&[])
    }
}

/// Cursor over the token slice being consumed (C `struct cf_token **`): the
/// slice plus the reformatted buffer the token texts live in.
struct Cur<'a> {
    toks: &'a [CfToken],
    buf: &'a [u8],
    i: usize,
}

impl<'a> Cur<'a> {
    fn new(toks: &'a [CfToken], buf: &'a [u8]) -> Self {
        Self { toks, buf, i: 0 }
    }

    /// Current token type (NONE on an exhausted slice: D-4/D-6 safe).
    fn ty(&self) -> CfTokenType {
        self.toks
            .get(self.i)
            .map(|t| t.r#type)
            .unwrap_or(CfTokenType::None)
    }

    fn tok(&self) -> Option<&'a CfToken> {
        self.toks.get(self.i)
    }

    /// First byte of the current token text. Only valid for cursors over the
    /// main or a dependency slice (def/param token lists carry views into
    /// other buffers, resolved through the token's own source instead).
    fn first_byte(&self) -> u8 {
        let Some(t) = self.tok() else {
            return 0;
        };
        self.buf.get(t.str_off).copied().unwrap_or(0)
    }

    /// `next_token` (pp mode skips SPACETAB runs).
    fn next(&mut self, pp_mode: bool) -> bool {
        if self.ty() != CfTokenType::None {
            self.i += 1;
        }
        if pp_mode {
            while self.ty() == CfTokenType::Spacetab {
                self.i += 1;
            }
        }
        self.ty() != CfTokenType::None
    }

    /// `go_to_newline`.
    fn go_to_newline(&mut self) -> bool {
        while self.ty() != CfTokenType::Newline && self.ty() != CfTokenType::None {
            self.i += 1;
        }
        self.ty() != CfTokenType::None
    }
}

/// `struct macro_param` / `macro_params` (R52).
#[derive(Debug)]
struct MacroParam {
    /// The parameter name (a copy of the def's parameter token).
    name: CfToken,
    /// The bound argument tokens, terminated by a NONE token (D-3 safe).
    tokens: Vec<CfToken>,
}

#[derive(Debug, Default)]
struct MacroParams {
    params: Vec<MacroParam>,
}

fn param_is_whitespace(param: &MacroParam) -> bool {
    !param.tokens.iter().any(|t| {
        matches!(
            t.r#type,
            t if t != CfTokenType::Spacetab && t != CfTokenType::Newline && t != CfTokenType::None
        )
    })
}

fn get_macro_param<'a>(
    pp: &CfPreprocessor,
    main: &CfLexer,
    params: &'a MacroParams,
    name: &[u8],
) -> Option<&'a MacroParam> {
    params
        .params
        .iter()
        .find(|p| pp.resolve(main, &p.name) == name)
}

/// R52 synthesized blank: text is the one-byte filler, span (and its owner)
/// comes from `base` when there is one (dumps `src=<base span>` + `u=`).
fn append_space(base: Option<&CfToken>) -> CfToken {
    match base {
        Some(b) => CfToken {
            str_off: 0,
            str_len: 1,
            span_off: b.span_off,
            span_len: b.span_len,
            r#type: CfTokenType::Spacetab,
            src: TokenSource::Synth,
            span_src: b.span_src,
        },
        None => CfToken {
            str_off: 0,
            str_len: 1,
            span_off: 0,
            span_len: 0,
            r#type: CfTokenType::Spacetab,
            src: TokenSource::Synth,
            span_src: TokenSource::Synth,
        },
    }
}

fn end_token() -> CfToken {
    CfToken {
        str_off: 0,
        str_len: 0,
        span_off: 0,
        span_len: 0,
        r#type: CfTokenType::None,
        src: TokenSource::Synth,
        span_src: TokenSource::Synth,
    }
}

/// R54: sequential global `$n` replacements; `None` args leave `$n` intact.
pub(crate) fn substitute(
    msg: &str,
    v1: Option<&str>,
    v2: Option<&str>,
    v3: Option<&str>,
) -> String {
    let mut s = msg.to_owned();
    if let Some(v) = v1 {
        s = s.replace("$1", v);
    }
    if let Some(v) = v2 {
        s = s.replace("$2", v);
    }
    if let Some(v) = v3 {
        s = s.replace("$3", v);
    }
    s
}

/// `cf_addew` (R54): position and file come from the lexer owning the
/// token's source span.
#[allow(clippy::too_many_arguments)] // mirrors the C signature (cf_addew)
fn addew(
    pp: &CfPreprocessor,
    main: &CfLexer,
    ed: &mut ErrorData,
    tok: &CfToken,
    message: &str,
    level: i32,
    v1: Option<&str>,
    v2: Option<&str>,
    v3: Option<&str>,
) {
    let file = pp.token_file(main, tok);
    let (row, col) = pp.token_rowcol(main, tok).unwrap_or((0, 0));
    ed.add(file, row, col, &substitute(message, v1, v2, v3), level);
}

/// Push a token into the output, re-basing `Main` sources to the current
/// dependency (include) index so token views resolve to the right lexer.
fn push_out(out: &mut Vec<CfToken>, rebase: Option<usize>, t: &CfToken) {
    let mut t = *t;
    if let Some(r) = rebase {
        if t.src == TokenSource::Main {
            t.src = TokenSource::Dep(r);
        }
        if t.span_src == TokenSource::Main {
            t.span_src = TokenSource::Dep(r);
        }
    }
    out.push(t);
}

/// `cf_preprocess_tokens` (R40/R41): the block loop.
fn block(
    pp: &mut CfPreprocessor,
    main: &CfLexer,
    ed: &mut ErrorData,
    out: &mut Vec<CfToken>,
    rebase: Option<usize>,
    if_block: bool,
    cur: &mut Cur,
) {
    let mut newline = true;
    let mut preprocessor_line = if_block;

    while cur.ty() != CfTokenType::None {
        if cur.ty() != CfTokenType::Spacetab && cur.ty() != CfTokenType::Newline {
            if preprocessor_line {
                if let Some(tok) = cur.tok() {
                    addew(
                        pp,
                        main,
                        ed,
                        tok,
                        "Unexpected token after preprocessor, expected newline",
                        LEX_ERROR,
                        None,
                        None,
                        None,
                    );
                }
                if !cur.go_to_newline() {
                    break;
                }
            }
            if cur.ty() != CfTokenType::Spacetab && cur.ty() != CfTokenType::Newline {
                if newline && cur.first_byte() == b'#' {
                    cur.next(true);
                    preprocessor_line = true;
                    if !directive(pp, main, ed, out, rebase, if_block, cur) {
                        break;
                    }
                    continue;
                }
                newline = false;
            }
        }
        if cur.ty() == CfTokenType::Newline {
            newline = true;
            preprocessor_line = false;
        } else if cur.ty() == CfTokenType::None {
            break;
        }
        let Some(tok) = cur.tok() else {
            break;
        };
        let base = *tok;
        add_token(
            pp,
            main,
            ed,
            out,
            rebase,
            cur,
            Some(&base),
            &MacroParams::default(),
            &mut Vec::new(),
        );
    }
}

/// `cf_preprocessor`: the directive dispatcher. Returns false to end the
/// current conditional block (an `#else`/`#elif`/`#endif` inside a block).
fn directive(
    pp: &mut CfPreprocessor,
    main: &CfLexer,
    ed: &mut ErrorData,
    out: &mut Vec<CfToken>,
    rebase: Option<usize>,
    if_block: bool,
    cur: &mut Cur,
) -> bool {
    let Some(tok) = cur.tok() else {
        return true;
    };
    let name = pp.resolve(main, tok);
    match name {
        b"include" => {
            pp_include(pp, main, ed, out, cur);
            true
        }
        b"define" => {
            pp_define(pp, main, ed, cur);
            true
        }
        b"undef" => {
            pp_undef(pp, main, ed, cur);
            true
        }
        b"ifdef" => {
            pp_ifdef(pp, main, ed, out, rebase, false, cur);
            true
        }
        b"ifndef" => {
            pp_ifdef(pp, main, ed, out, rebase, true, cur);
            true
        }
        b"if" => {
            pp_if(pp, main, ed, out, rebase, cur);
            true
        }
        b"else" | b"elif" | b"endif" => {
            if !if_block {
                addew(
                    pp,
                    main,
                    ed,
                    tok,
                    "#$1 outside of #if/#ifdef/#ifndef block",
                    LEX_ERROR,
                    Some(std::str::from_utf8(name).unwrap_or("")),
                    None,
                    None,
                );
                cur.i += 1;
                return true;
            }
            false
        }
        _ => {
            // R43: unknown directive: skip its name; the rest of the line is
            // handled by the block loop (its expected-newline error).
            if cur.ty() != CfTokenType::Newline && cur.ty() != CfTokenType::None {
                cur.i += 1;
            }
            true
        }
    }
}

/// `cf_preprocess_include` (R48/R49).
fn pp_include(
    pp: &mut CfPreprocessor,
    main: &CfLexer,
    ed: &mut ErrorData,
    out: &mut Vec<CfToken>,
    cur: &mut Cur,
) {
    if pp.ignore_state {
        cur.go_to_newline();
        return;
    }
    cur.next(true);
    let Some(tok) = cur.tok() else {
        return;
    };
    if tok.r#type != CfTokenType::String {
        addew(
            pp,
            main,
            ed,
            tok,
            "Expected $1",
            LEX_ERROR,
            Some("string"),
            None,
            None,
        );
        cur.go_to_newline();
        return;
    }
    let s = &cur.buf[tok.str_off..tok.str_off + tok.str_len];
    let sys = s.len() >= 2 && s.first() == Some(&b'<') && s.last() == Some(&b'>');
    let loc = s.len() >= 2 && s.first() == Some(&b'"') && s.last() == Some(&b'"');
    if sys {
        // R48: system includes are accepted and ignored.
    } else if loc {
        include_file(pp, main, ed, out, cur, s);
    } else {
        addew(
            pp,
            main,
            ed,
            tok,
            "Invalid or incomplete string",
            LEX_ERROR,
            None,
            None,
            None,
        );
        cur.go_to_newline();
        return;
    }
    cur.i += 1;
}

/// `cf_include_file` (R49, D-4).
fn include_file(
    pp: &mut CfPreprocessor,
    main: &CfLexer,
    ed: &mut ErrorData,
    out: &mut Vec<CfToken>,
    cur: &mut Cur,
    s: &[u8],
) {
    let tok = cur.tok().unwrap();
    let mut path = s[1..s.len() - 1].to_vec();
    // insert_path: the MAIN file's directory prefix (R49: nested includes
    // also resolve against the main file, not the including file).
    if let Some(f) = &main.file
        && let Some(slash) = f.rfind('/')
    {
        let mut prefix = f.as_bytes()[..=slash].to_vec();
        prefix.append(&mut path);
        path = prefix;
    }
    let path = String::from_utf8_lossy(&path).into_owned();

    // D-4 bound: a path already being processed (first run or re-run) must
    // not recurse; report it like an unreadable file.
    if pp.pending_includes.contains(&path) {
        include_error(pp, main, ed, tok, cur.buf);
        return;
    }

    let existing = pp
        .dependencies
        .iter()
        .position(|d| d.file.as_deref() == Some(path.as_str()));
    if let Some(idx) = existing {
        let mut toks = pp.dependencies[idx].tokens.clone();
        let buf = pp.dependencies[idx].reformatted.clone();
        pp.pending_includes.push(path);
        include_run(pp, main, ed, out, idx, &mut toks, &buf);
        pp.pending_includes.pop();
        return;
    }

    let data = match std::fs::read(&path) {
        Ok(d) => d,
        Err(_) => {
            include_error(pp, main, ed, tok, cur.buf);
            return;
        }
    };
    // R49: remove a leading UTF-8 BOM.
    let data = data
        .strip_prefix(&[0xEF, 0xBB, 0xBF][..])
        .unwrap_or(&data)
        .to_vec();
    let mut dep = CfLexer::new();
    dep.lex(Some(&data), Some(&path));
    let idx = pp.dependencies.len();
    let mut toks = dep.tokens.clone();
    let buf = dep.reformatted.clone();
    pp.dependencies.push(dep);
    pp.pending_includes.push(path);
    include_run(pp, main, ed, out, idx, &mut toks, &buf);
    pp.pending_includes.pop();
}

fn include_run(
    pp: &mut CfPreprocessor,
    main: &CfLexer,
    ed: &mut ErrorData,
    out: &mut Vec<CfToken>,
    dep_idx: usize,
    toks: &mut [CfToken],
    buf: &[u8],
) {
    // Tag the tokens with their owning dependency so name lookups and
    // captures resolve through the right lexer (R49).
    for t in toks.iter_mut() {
        if t.src == TokenSource::Main {
            t.src = TokenSource::Dep(dep_idx);
        }
        if t.span_src == TokenSource::Main {
            t.span_src = TokenSource::Dep(dep_idx);
        }
    }
    let mut cur = Cur::new(toks, buf);
    block(pp, main, ed, out, Some(dep_idx), false, &mut cur);
}

fn include_error(
    pp: &CfPreprocessor,
    main: &CfLexer,
    ed: &mut ErrorData,
    tok: &CfToken,
    buf: &[u8],
) {
    // Q-7: the message embeds the rest of the buffer at the string token.
    let rest = std::str::from_utf8(&buf[tok.str_off..]).unwrap_or("");
    addew(
        pp,
        main,
        ed,
        tok,
        "Could not open file '$1'",
        LEX_ERROR,
        Some(rest),
        None,
        None,
    );
}

/// `cf_preprocess_define` (R45/R46).
fn pp_define(pp: &mut CfPreprocessor, main: &CfLexer, ed: &mut ErrorData, cur: &mut Cur) {
    if pp.ignore_state {
        cur.go_to_newline();
        return;
    }
    cur.next(true);
    let Some(name_tok) = cur.tok() else {
        return;
    };
    if name_tok.r#type != CfTokenType::Name {
        addew(
            pp,
            main,
            ed,
            name_tok,
            "Expected $1",
            LEX_ERROR,
            Some("identifier"),
            None,
            None,
        );
        cur.go_to_newline();
        return;
    }
    let mut def = CfDef::new();
    // R53: directive bodies start with one blank (no base).
    def.tokens.push(append_space(None));
    def.name = *name_tok;
    if !cur.next(true) {
        complete_def(pp, def);
        return;
    }
    if cur.first_byte() == b'(' && !macro_params(pp, main, ed, cur, &mut def) {
        return;
    }
    while cur.ty() != CfTokenType::Newline && cur.ty() != CfTokenType::None {
        let t = cur.tok().unwrap();
        def.add_token(*t);
        cur.i += 1;
    }
    complete_def(pp, def);
}

fn complete_def(pp: &mut CfPreprocessor, def: CfDef) {
    let mut def = def;
    def.tokens.push(end_token());
    def.tokens.push(append_space(None));
    pp.defines.push(def);
}

/// `cf_preprocess_undef` (R54).
fn pp_undef(pp: &mut CfPreprocessor, main: &CfLexer, ed: &mut ErrorData, cur: &mut Cur) {
    if pp.ignore_state {
        cur.go_to_newline();
        return;
    }
    cur.next(true);
    let Some(tok) = cur.tok() else {
        return;
    };
    if tok.r#type != CfTokenType::Name {
        addew(
            pp,
            main,
            ed,
            tok,
            "Expected $1",
            LEX_ERROR,
            Some("identifier"),
            None,
            None,
        );
        cur.go_to_newline();
        return;
    }
    let name = pp.resolve(main, tok);
    if let Some(pos) = pp
        .defines
        .iter()
        .position(|d| pp.resolve(main, &d.name) == name)
    {
        pp.defines.remove(pos);
    }
    cur.i += 1;
}

/// `cf_preprocess_ifdef` (R50).
fn pp_ifdef(
    pp: &mut CfPreprocessor,
    main: &CfLexer,
    ed: &mut ErrorData,
    out: &mut Vec<CfToken>,
    rebase: Option<usize>,
    ifnot: bool,
    cur: &mut Cur,
) {
    cur.next(true);
    let Some(tok) = cur.tok() else {
        return;
    };
    if tok.r#type != CfTokenType::Name {
        addew(
            pp,
            main,
            ed,
            tok,
            "Expected $1",
            LEX_ERROR,
            Some("identifier"),
            None,
            None,
        );
        cur.go_to_newline();
        return;
    }
    let name = pp.resolve(main, tok);
    let is_true = pp.get_def(main, name).is_none() == ifnot;
    if !subblock(pp, main, ed, out, rebase, !is_true, cur) {
        return;
    }
    let Some(t) = cur.tok() else {
        return;
    };
    let name = pp.resolve(main, t);
    if name == b"else" {
        if !subblock(pp, main, ed, out, rebase, is_true, cur) {
            return;
        }
    } else if name == b"elif" {
        if is_true {
            if !subblock(pp, main, ed, out, rebase, is_true, cur) {
                return;
            }
        } else {
            pp_if(pp, main, ed, out, rebase, cur);
            return;
        }
    }
    cur.i += 1;
}

/// `cf_preprocess_if` (R51).
fn pp_if(
    pp: &mut CfPreprocessor,
    main: &CfLexer,
    ed: &mut ErrorData,
    out: &mut Vec<CfToken>,
    rebase: Option<usize>,
    cur: &mut Cur,
) {
    let mut value_a: i64 = 0;
    let mut value_b: i64 = 0;
    cur.next(true);
    let Some(tok) = cur.tok() else {
        return;
    };
    let name = pp.resolve(main, tok);
    let ok_a = (tok.r#type == CfTokenType::Name
        && pp.def_value(main, name, &mut value_a, &mut Vec::new()))
        || tok.r#type == CfTokenType::Num;
    if !ok_a {
        addew(
            pp,
            main,
            ed,
            tok,
            "Expected $1",
            LEX_ERROR,
            Some("identifier or number"),
            None,
            None,
        );
        cur.go_to_newline();
        return;
    }
    if tok.r#type == CfTokenType::Num {
        value_a = parse_c_int(pp.resolve_tail(main, tok));
    }
    cur.next(true);
    let Some(tok) = cur.tok() else {
        return;
    };
    let is_true = if tok.r#type == CfTokenType::Newline {
        cur.i -= 1;
        value_a != 0
    } else {
        if tok.r#type != CfTokenType::Other {
            addew(
                pp,
                main,
                ed,
                tok,
                "Expected $1",
                LEX_ERROR,
                Some("comparator"),
                None,
                None,
            );
            cur.go_to_newline();
            return;
        }
        // The comparator is the run of consecutive OTHER tokens (e.g. `<=`).
        let comp_off = tok.str_off;
        let mut comp_len = tok.str_len;
        cur.i += 1;
        while cur.ty() == CfTokenType::Other {
            if let Some(t) = cur.tok() {
                comp_len += t.str_len;
            }
            cur.i += 1;
        }
        cur.i -= 1;
        cur.next(true);
        let Some(tok) = cur.tok() else {
            return;
        };
        let name = pp.resolve(main, tok);
        let ok_b = (tok.r#type == CfTokenType::Name
            && pp.def_value(main, name, &mut value_b, &mut Vec::new()))
            || tok.r#type == CfTokenType::Num;
        if !ok_b {
            addew(
                pp,
                main,
                ed,
                tok,
                "Expected $1",
                LEX_ERROR,
                Some("identifier or number"),
                None,
                None,
            );
            cur.go_to_newline();
            return;
        }
        if tok.r#type == CfTokenType::Num {
            value_b = parse_c_int(pp.resolve_tail(main, tok));
        }
        let comp = &cur.buf[comp_off..comp_off + comp_len];
        match comp {
            b"==" => value_a == value_b,
            b">=" => value_a >= value_b,
            b"<=" => value_a <= value_b,
            b"!=" => value_a != value_b,
            b"<" => value_a < value_b,
            b">" => value_a > value_b,
            _ => {
                addew(
                    pp,
                    main,
                    ed,
                    tok,
                    "Expected $1",
                    LEX_ERROR,
                    Some("comparator"),
                    None,
                    None,
                );
                cur.go_to_newline();
                return;
            }
        }
    };
    if !subblock(pp, main, ed, out, rebase, !is_true, cur) {
        return;
    }
    let Some(t) = cur.tok() else {
        return;
    };
    let name = pp.resolve(main, t);
    if name == b"else" {
        if !subblock(pp, main, ed, out, rebase, is_true, cur) {
            return;
        }
    } else if name == b"elif" {
        if is_true {
            if !subblock(pp, main, ed, out, rebase, is_true, cur) {
                return;
            }
        } else {
            pp_if(pp, main, ed, out, rebase, cur);
            return;
        }
    }
    cur.i += 1;
}

/// `cf_preprocess_subblock` (R44).
fn subblock(
    pp: &mut CfPreprocessor,
    main: &CfLexer,
    ed: &mut ErrorData,
    out: &mut Vec<CfToken>,
    rebase: Option<usize>,
    ignore: bool,
    cur: &mut Cur,
) -> bool {
    if !cur.next(true) {
        return false;
    }
    if !pp.ignore_state {
        pp.ignore_state = ignore;
        block(pp, main, ed, out, rebase, true, cur);
        pp.ignore_state = false;
    } else {
        block(pp, main, ed, out, rebase, true, cur);
    }
    if cur.ty() == CfTokenType::None {
        if let Some(tok) = cur.tok() {
            addew(
                pp,
                main,
                ed,
                tok,
                "Unexpected end of file before #endif",
                LEX_ERROR,
                None,
                None,
                None,
            );
        }
        return false;
    }
    true
}

/// `cf_preprocess_macro_params` (R46).
fn macro_params(
    pp: &mut CfPreprocessor,
    main: &CfLexer,
    ed: &mut ErrorData,
    cur: &mut Cur,
    def: &mut CfDef,
) -> bool {
    def.r#macro = true;
    loop {
        cur.next(true);
        let Some(tok) = cur.tok() else {
            return false;
        };
        if tok.r#type != CfTokenType::Name {
            addew(
                pp,
                main,
                ed,
                tok,
                "Expected $1",
                LEX_ERROR,
                Some("identifier"),
                None,
                None,
            );
            cur.go_to_newline();
            return false;
        }
        def.add_param(*tok);
        cur.next(true);
        let Some(tok) = cur.tok() else {
            return false;
        };
        if tok.r#type != CfTokenType::Other
            || !(cur.first_byte() == b',' || cur.first_byte() == b')')
        {
            addew(
                pp,
                main,
                ed,
                tok,
                "Expected $1",
                LEX_ERROR,
                Some("',' or ')'"),
                None,
                None,
            );
            cur.go_to_newline();
            return false;
        }
        if cur.first_byte() == b')' {
            break;
        }
    }
    // Ended properly: go to the first define token (or newline).
    cur.next(true);
    true
}

/// `cf_preprocess_save_macro_params` (R52).
#[allow(clippy::too_many_arguments)] // mirrors the C signature
fn save_macro_params(
    pp: &mut CfPreprocessor,
    main: &CfLexer,
    ed: &mut ErrorData,
    rebase: Option<usize>,
    cur: &mut Cur,
    def: &CfDef,
    base: &CfToken,
    cur_params: &MacroParams,
    dst: &mut MacroParams,
    expanding: &mut Vec<Vec<u8>>,
) {
    let mut count: usize = 0;
    cur.next(false);
    let Some(tok) = cur.tok() else {
        return;
    };
    // Def-body cursors carry buf=&[]: resolve the byte through the token's
    // own source (first_byte is only valid on main/dep cursors).
    let f = pp.resolve(main, tok).first().copied().unwrap_or(0);
    if tok.r#type != CfTokenType::Other || f != b'(' {
        addew(
            pp,
            main,
            ed,
            tok,
            "Expected $1",
            LEX_ERROR,
            Some("'('"),
            None,
            None,
        );
        return;
    }
    loop {
        let mut param = MacroParam {
            name: CfToken::default(),
            tokens: Vec::new(),
        };
        cur.i += 1;
        count += 1;
        save_macro_param(
            pp, main, ed, rebase, cur, base, cur_params, &mut param, expanding,
        );
        let Some(tok) = cur.tok() else {
            return;
        };
        let f = pp.resolve(main, tok).first().copied().unwrap_or(0);
        if tok.r#type != CfTokenType::Other || !(f == b',' || f == b')') {
            addew(
                pp,
                main,
                ed,
                tok,
                "Expected $1",
                LEX_ERROR,
                Some("',' or ')'"),
                None,
                None,
            );
            return;
        }
        if param_is_whitespace(&param) && count == 1 && def.params.is_empty() && f == b')' {
            // Zero-parameter macro call: the first (empty) entry is ignored.
            break;
        }
        if count <= def.params.len() {
            param.name = def.params[count - 1];
            dst.params.push(param);
        }
        if f == b')' {
            break;
        }
    }
    if count != def.params.len() {
        let Some(tok) = cur.tok() else {
            return;
        };
        addew(
            pp,
            main,
            ed,
            tok,
            "Mismatching number of macro parameters",
            LEX_ERROR,
            None,
            None,
            None,
        );
    }
}

/// `cf_preprocess_save_macro_param` (R52, D-6: stops at the terminating
/// NONE instead of stepping past it).
#[allow(clippy::too_many_arguments)] // mirrors the C signature
fn save_macro_param(
    pp: &mut CfPreprocessor,
    main: &CfLexer,
    ed: &mut ErrorData,
    rebase: Option<usize>,
    cur: &mut Cur,
    base: &CfToken,
    cur_params: &MacroParams,
    param: &mut MacroParam,
    expanding: &mut Vec<Vec<u8>>,
) {
    param.tokens.push(append_space(Some(base)));
    let mut brace = 0;
    while cur.ty() != CfTokenType::None {
        let Some(tok) = cur.tok() else {
            break;
        };
        let f = pp.resolve(main, tok).first().copied().unwrap_or(0);
        if f == b'(' {
            brace += 1;
        } else if f == b')' {
            if brace > 0 {
                brace -= 1;
            } else {
                break;
            }
        } else if f == b',' && brace == 0 {
            break;
        }
        add_token(
            pp,
            main,
            ed,
            &mut param.tokens,
            rebase,
            cur,
            Some(base),
            cur_params,
            expanding,
        );
    }
    if cur.ty() == CfTokenType::None
        && let Some(tok) = cur.tok()
    {
        addew(
            pp,
            main,
            ed,
            tok,
            "Unexpected end of file",
            LEX_ERROR,
            None,
            None,
            None,
        );
    }
    param.tokens.push(append_space(Some(base)));
    param.tokens.push(end_token());
}

/// `cf_preprocess_addtoken` (R52, D-7: an expanding-name stack terminates
/// self- and mutually recursive macros by emitting the name verbatim).
#[allow(clippy::too_many_arguments)] // mirrors the C signature
fn add_token(
    pp: &mut CfPreprocessor,
    main: &CfLexer,
    ed: &mut ErrorData,
    out: &mut Vec<CfToken>,
    rebase: Option<usize>,
    cur: &mut Cur,
    base: Option<&CfToken>,
    params: &MacroParams,
    expanding: &mut Vec<Vec<u8>>,
) {
    if pp.ignore_state {
        cur.i += 1;
        return;
    }
    let Some(tok) = cur.tok() else {
        return;
    };
    let base = base.unwrap_or(tok);
    if tok.r#type == CfTokenType::Name {
        let name = pp.resolve(main, tok);
        if let Some(param) = get_macro_param(pp, main, params, name) {
            // Parameter expansion: the bound tokens, no further params.
            let mut pcur = Cur::new(&param.tokens, &[]);
            while pcur.ty() != CfTokenType::None {
                add_token(
                    pp,
                    main,
                    ed,
                    out,
                    rebase,
                    &mut pcur,
                    Some(base),
                    &MacroParams::default(),
                    expanding,
                );
            }
            cur.i += 1;
            return;
        }
        // Clone: the expansion mutates `pp` while the body tokens stay
        // borrowed otherwise.
        if let Some(def) = pp.get_def(main, name).cloned() {
            if expanding.iter().any(|n| n.as_slice() == name) {
                // D-7: a macro name inside its own expansion is emitted
                // verbatim; the recursion terminates.
                push_out(out, rebase, tok);
                cur.i += 1;
                return;
            }
            expanding.push(name.to_vec());
            unwrap_define(
                pp, main, ed, out, rebase, cur, base, &def, params, expanding,
            );
            expanding.pop();
            return;
        }
    }
    push_out(out, rebase, tok);
    cur.i += 1;
}

/// `cf_preprocess_unwrap_define` (R52, D-3: an empty def list expands to
/// nothing; the loop is index-based, so there is no null dereference).
#[allow(clippy::too_many_arguments)] // mirrors the C signature
fn unwrap_define(
    pp: &mut CfPreprocessor,
    main: &CfLexer,
    ed: &mut ErrorData,
    out: &mut Vec<CfToken>,
    rebase: Option<usize>,
    cur: &mut Cur,
    base: &CfToken,
    def: &CfDef,
    params: &MacroParams,
    expanding: &mut Vec<Vec<u8>>,
) {
    let mut new_params = MacroParams::default();
    if def.r#macro {
        save_macro_params(
            pp,
            main,
            ed,
            rebase,
            cur,
            def,
            base,
            params,
            &mut new_params,
            expanding,
        );
    }
    // Def body tokens resolve through their own sources; the cursor buffer
    // is unused for them (see `Cur::first_byte`).
    let mut dcur = Cur {
        toks: &def.tokens,
        buf: &[],
        i: 0,
    };
    while dcur.ty() != CfTokenType::None {
        add_token(
            pp,
            main,
            ed,
            out,
            rebase,
            &mut dcur,
            Some(base),
            &new_params,
            expanding,
        );
    }
    cur.i += 1;
}

// ---------------------------------------------------------------------------
// Q-14 / R47: C sscanf("%i") semantics over the rest of the buffer.
// ---------------------------------------------------------------------------

/// Parse an integer the way C `sscanf(s, "%i", &v)` does: optional sign,
/// then decimal, octal after a leading `0`, or hexadecimal after `0x`/`0X`,
/// stopping at the first non-digit (Q-14: reads the rest of the buffer, so
/// `08` -> 0, `1.5` -> 1, `1e5` -> 1, `0x10` -> 16).
fn parse_c_int(s: &[u8]) -> i64 {
    let mut i = 0;
    while i < s.len() && (s[i] as char).is_ascii_whitespace() {
        i += 1;
    }
    let mut neg = false;
    if i < s.len() && (s[i] == b'+' || s[i] == b'-') {
        neg = s[i] == b'-';
        i += 1;
    }
    let mut v: i64 = 0;
    if i + 1 < s.len() && s[i] == b'0' && (s[i + 1] == b'x' || s[i + 1] == b'X') {
        i += 2;
        while i < s.len() {
            let d = (s[i] as char).to_digit(16).unwrap_or(u32::MAX);
            if d > 15 {
                break;
            }
            v = v.wrapping_mul(16).wrapping_add(d as i64);
            i += 1;
        }
    } else if i < s.len() && s[i] == b'0' && i + 1 < s.len() {
        i += 1;
        while i < s.len() && (b'0'..=b'7').contains(&s[i]) {
            v = v.wrapping_mul(8).wrapping_add((s[i] - b'0') as i64);
            i += 1;
        }
    } else {
        while i < s.len() && s[i].is_ascii_digit() {
            v = v.wrapping_mul(10).wrapping_add((s[i] - b'0') as i64);
            i += 1;
        }
    }
    if neg { v.wrapping_neg() } else { v }
}

// ---------------------------------------------------------------------------
// Tests: conformance vectors as oracles (scripts `20`-`26`, `50`, `90`).
// The scripts' strings are unescaped per `test/conformance/gen/script.h`;
// the byte strings below are the actual input bytes.
// ---------------------------------------------------------------------------

#[cfg(test)]
mod tests {
    use super::*;
    use std::fmt::Write as _;

    /// One test case: a preprocessor plus the main lexer of the current
    /// `parse` (kept alive: the output tokens view into it).
    struct Case {
        pp: CfPreprocessor,
        main: CfLexer,
        tmp: std::path::PathBuf,
    }

    static COUNTER: std::sync::atomic::AtomicUsize = std::sync::atomic::AtomicUsize::new(0);
    fn unique() -> usize {
        COUNTER.fetch_add(1, std::sync::atomic::Ordering::Relaxed)
    }

    impl Case {
        fn new() -> Self {
            let raw =
                std::env::temp_dir().join(format!("cfpp-{}-{}", std::process::id(), unique()));
            // Forward-slash form: cf include resolution is slash-based (C:
            // strrchr(file, '/')) and the Windows file APIs accept '/'. The
            // directory is created lazily on the first writefile (Miri
            // isolation does not permit mkdir).
            let tmp = std::path::PathBuf::from(raw.to_string_lossy().replace('\\', "/"));
            Self {
                pp: CfPreprocessor::new(),
                main: CfLexer::new(),
                tmp,
            }
        }

        fn writefile(&self, name: &str, content: &[u8]) {
            std::fs::create_dir_all(&self.tmp).unwrap();
            std::fs::write(self.tmp.join(name), content).unwrap();
        }

        /// File name string for `parse`: forward-slash form on every
        /// platform (Path::join appends the native separator on Windows,
        /// but cf include resolution is slash-based: C strrchr(file, '/')).
        fn tmpfile(&self, name: &str) -> String {
            self.tmp.join(name).to_string_lossy().replace('\\', "/")
        }

        fn reset(&mut self) {
            self.pp = CfPreprocessor::new();
            self.main = CfLexer::new();
        }

        /// Runner `parse TEXT [FILE]` (the parser entry from W21c reduces to
        /// lex + preprocess; the `parse` line prints the combined result).
        /// Note: like the runner, `parse` does NOT clear an existing
        /// preprocessor (only `reset` does), so API defs survive it.
        fn parse(&mut self, text: &[u8], file: Option<&str>, ed: &mut ErrorData) -> bool {
            self.main = CfLexer::new();
            let ok = self.main.lex(Some(text), file);
            if !ok {
                return false;
            }
            self.pp.preprocess(&self.main, ed)
        }

        /// Runner `tokens pp`.
        fn tokens_pp(&self) -> String {
            let n = self.pp.tokens.len();
            let mut s = format!("tokens pp {n}\n");
            for (i, t) in self.pp.tokens.iter().enumerate() {
                let _ = writeln!(s, "tok {i} {}", self.dump_tok(t));
            }
            s
        }

        fn dump_tok(&self, t: &CfToken) -> String {
            let mut s = format!(
                "type={} str={}",
                ctype(t.r#type),
                pseg(self.pp.token_str(&self.main, t))
            );
            match self.pp.token_span_lexer(&self.main, t) {
                Some(l) => {
                    let (r, c) = l.rowcol(t.span_off);
                    let _ = write!(s, " src={}+{} @{}:{}", t.span_off, t.span_len, r, c);
                    let strb = self.pp.token_str(&self.main, t);
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
                        let _ = write!(s, " f={}", pfile(l.file.as_deref(), &self.tmp));
                    }
                }
                None => s.push_str(" src=-"),
            }
            s
        }

        /// Runner `errors`.
        fn errors(&self, ed: &ErrorData) -> String {
            let mut s = format!(
                "errors {} (errors={} warnings={} has_errors={})",
                ed.len(),
                ed.type_count(LEX_ERROR),
                ed.type_count(LEX_WARNING),
                if ed.has_errors() { 1 } else { 0 }
            );
            s.push('\n');
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
                    pfile(item.file.as_deref(), &self.tmp),
                    item.row,
                    item.column,
                    pseg(item.msg.as_bytes())
                );
            }
            s
        }

        /// Runner `errstr`.
        fn errstr(&self, ed: &ErrorData) -> String {
            format!(
                "errstr {}\n",
                pseg(ed.buildstring().unwrap_or_default().as_bytes())
            )
        }

        /// Runner `def NAME [BODY]` (API definition, R53): name and body are
        /// lexed in their own side lexers (`def-name`/`def-body`); the body
        /// tokens (including the NONE terminator) form the def body verbatim.
        fn def(&mut self, name: &str, body: Option<&[u8]>) -> usize {
            let mut ln = CfLexer::new();
            ln.lex(Some(name.as_bytes()), Some("def-name"));
            let name_idx = self.pp.sources.len();
            rebase_tokens(&mut ln, TokenSource::Side(name_idx));
            self.pp.sources.push(ln);

            let mut lb = CfLexer::new();
            let body_idx = self.pp.sources.len();
            if let Some(b) = body {
                lb.lex(Some(b), Some("def-body"));
            }
            rebase_tokens(&mut lb, TokenSource::Side(body_idx));
            self.pp.sources.push(lb);

            let mut def = CfDef::new();
            if let Some(t) = self.pp.sources.get(name_idx).and_then(|l| l.tokens.first()) {
                def.name = *t;
            }
            if let Some(l) = self.pp.sources.get(body_idx) {
                def.tokens = l.tokens.clone();
            }
            self.pp.add_def(None, &def);
            self.pp.defines.len()
        }

        /// Runner `undef NAME`.
        fn undef(&mut self, name: &str) -> usize {
            self.pp.remove_def(&self.main, name);
            self.pp.defines.len()
        }
    }

    fn rebase_tokens(lex: &mut CfLexer, src: TokenSource) {
        for t in &mut lex.tokens {
            t.src = src;
            t.span_src = src;
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

    /// Runner `pfile`: `@null` for none, the temp dir printed as `@tmp`.
    fn pfile(f: Option<&str>, tmp: &std::path::Path) -> String {
        let Some(f) = f else {
            return "@null".into();
        };
        let tmp_str = tmp.to_string_lossy();
        if let Some(rest) = f.strip_prefix(&*tmp_str) {
            pseg(format!("@tmp{rest}").as_bytes())
        } else {
            pseg(f.as_bytes())
        }
    }

    fn check(case: &Case, out: &str, want: &str) {
        let _ = case;
        assert_eq!(out, want, "case output mismatch");
    }

    fn run_case(
        c: &mut Case,
        out: &mut String,
        ed: &mut ErrorData,
        text: impl AsRef<[u8]>,
        file: Option<&str>,
    ) {
        let ok = c.parse(text.as_ref(), file, ed);
        let _ = writeln!(out, "parse {}", if ok { "true" } else { "false" });
    }

    // -- vector 20: #define, #undef, expansion -------------------------------

    #[test]
    fn v20_pp_define() {
        let mut c = Case::new();
        let mut out = String::new();
        let mut ed = ErrorData::new();
        run_case(&mut c, &mut out, &mut ed, "#define A 1\nx A y\nA\n", None);
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));

        c.reset();
        ed = ErrorData::new();
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            "#define Z\nZ x\n#define W  2 3 \nW|\n#undef Z\nZ\n#undef Nope\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));

        c.reset();
        ed = ErrorData::new();
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            "# define  S  ( 4 )\nS\n#define B A A\n#define A 1\nB\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));

        check(
            &c,
            &out,
            r#"parse true
tokens pp 12
tok 0 type=NEWLINE str="\n" src=11+1 @1:12
tok 1 type=NAME str="x" src=12+1 @2:1
tok 2 type=SPACETAB str=" " src=13+1 @2:2
tok 3 type=SPACETAB str=" " src=-
tok 4 type=NUM str="1" src=10+1 @1:11
tok 5 type=SPACETAB str=" " src=15+1 @2:4
tok 6 type=NAME str="y" src=16+1 @2:5
tok 7 type=NEWLINE str="\n" src=17+1 @2:6
tok 8 type=SPACETAB str=" " src=-
tok 9 type=NUM str="1" src=10+1 @1:11
tok 10 type=NEWLINE str="\n" src=19+1 @3:2
tok 11 type=NONE str="" src=20+0 @4:1
errors 0 (errors=0 warnings=0 has_errors=0)
parse true
tokens pp 18
tok 0 type=NEWLINE str="\n" src=9+1 @1:10
tok 1 type=SPACETAB str=" " src=-
tok 2 type=SPACETAB str=" " src=11+1 @2:2
tok 3 type=NAME str="x" src=12+1 @2:3
tok 4 type=NEWLINE str="\n" src=13+1 @2:4
tok 5 type=NEWLINE str="\n" src=29+1 @3:16
tok 6 type=SPACETAB str=" " src=-
tok 7 type=NUM str="2" src=25+1 @3:12
tok 8 type=SPACETAB str=" " src=26+1 @3:13
tok 9 type=NUM str="3" src=27+1 @3:14
tok 10 type=SPACETAB str=" " src=28+1 @3:15
tok 11 type=OTHER str="|" src=31+1 @4:2
tok 12 type=NEWLINE str="\n" src=32+1 @4:3
tok 13 type=NEWLINE str="\n" src=41+1 @5:9
tok 14 type=NAME str="Z" src=42+1 @6:1
tok 15 type=NEWLINE str="\n" src=43+1 @6:2
tok 16 type=NEWLINE str="\n" src=55+1 @7:12
tok 17 type=NONE str="" src=56+0 @8:1
errors 0 (errors=0 warnings=0 has_errors=0)
parse true
tokens pp 13
tok 0 type=NEWLINE str="\n" src=18+1 @1:19
tok 1 type=NAME str="S" src=19+1 @2:1
tok 2 type=NEWLINE str="\n" src=20+1 @2:2
tok 3 type=NEWLINE str="\n" src=34+1 @3:14
tok 4 type=NEWLINE str="\n" src=46+1 @4:12
tok 5 type=SPACETAB str=" " src=-
tok 6 type=SPACETAB str=" " src=-
tok 7 type=NUM str="1" src=45+1 @4:11
tok 8 type=SPACETAB str=" " src=32+1 @3:12
tok 9 type=SPACETAB str=" " src=-
tok 10 type=NUM str="1" src=45+1 @4:11
tok 11 type=NEWLINE str="\n" src=48+1 @5:2
tok 12 type=NONE str="" src=49+0 @6:1
errors 1 (errors=1 warnings=0 has_errors=1)
err 0 ERROR file=@null row=1 col=16 msg="Expected identifier"
"#,
        );
        let _ = c.tmp;
    }

    // -- vector 21: function-like macros --------------------------------------

    #[test]
    fn v21_pp_macro() {
        let mut c = Case::new();
        let mut out = String::new();
        let mut ed = ErrorData::new();
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            "#define F(a,b) a+b\nF(1,2) F( x , y) F(g(1,2),3)\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));

        c.reset();
        ed = ErrorData::new();
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            "#define N() k\nN() N(1)\n#define M(a) [a]\nM() M(1,2)\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));

        c.reset();
        ed = ErrorData::new();
        run_case(&mut c, &mut out, &mut ed, "#define F(a) a\nF (1)\n", None);
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));

        check(
            &c,
            &out,
            r#"parse true
tokens pp 37
tok 0 type=NEWLINE str="\n" src=18+1 @1:19
tok 1 type=SPACETAB str=" " src=-
tok 2 type=SPACETAB str=" " src=19+1 @2:1 u="F"
tok 3 type=NUM str="1" src=21+1 @2:3
tok 4 type=SPACETAB str=" " src=19+1 @2:1 u="F"
tok 5 type=OTHER str="+" src=16+1 @1:17
tok 6 type=SPACETAB str=" " src=19+1 @2:1 u="F"
tok 7 type=NUM str="2" src=23+1 @2:5
tok 8 type=SPACETAB str=" " src=19+1 @2:1 u="F"
tok 9 type=SPACETAB str=" " src=25+1 @2:7
tok 10 type=SPACETAB str=" " src=-
tok 11 type=SPACETAB str=" " src=26+1 @2:8 u="F"
tok 12 type=SPACETAB str=" " src=28+1 @2:10
tok 13 type=NAME str="x" src=29+1 @2:11
tok 14 type=SPACETAB str=" " src=30+1 @2:12
tok 15 type=SPACETAB str=" " src=26+1 @2:8 u="F"
tok 16 type=OTHER str="+" src=16+1 @1:17
tok 17 type=SPACETAB str=" " src=26+1 @2:8 u="F"
tok 18 type=SPACETAB str=" " src=32+1 @2:14
tok 19 type=NAME str="y" src=33+1 @2:15
tok 20 type=SPACETAB str=" " src=26+1 @2:8 u="F"
tok 21 type=SPACETAB str=" " src=35+1 @2:17
tok 22 type=SPACETAB str=" " src=-
tok 23 type=SPACETAB str=" " src=36+1 @2:18 u="F"
tok 24 type=NAME str="g" src=38+1 @2:20
tok 25 type=OTHER str="(" src=39+1 @2:21
tok 26 type=NUM str="1" src=40+1 @2:22
tok 27 type=OTHER str="," src=41+1 @2:23
tok 28 type=NUM str="2" src=42+1 @2:24
tok 29 type=OTHER str=")" src=43+1 @2:25
tok 30 type=SPACETAB str=" " src=36+1 @2:18 u="F"
tok 31 type=OTHER str="+" src=16+1 @1:17
tok 32 type=SPACETAB str=" " src=36+1 @2:18 u="F"
tok 33 type=NUM str="3" src=45+1 @2:27
tok 34 type=SPACETAB str=" " src=36+1 @2:18 u="F"
tok 35 type=NEWLINE str="\n" src=47+1 @2:29
tok 36 type=NONE str="" src=48+0 @3:1
errors 0 (errors=0 warnings=0 has_errors=0)
parse true
tokens pp 25
tok 0 type=NEWLINE str="\n" src=13+1 @1:14
tok 1 type=NAME str="N" src=14+1 @2:1
tok 2 type=OTHER str="(" src=15+1 @2:2
tok 3 type=OTHER str=")" src=16+1 @2:3
tok 4 type=SPACETAB str=" " src=17+1 @2:4
tok 5 type=NAME str="N" src=18+1 @2:5
tok 6 type=OTHER str="(" src=19+1 @2:6
tok 7 type=NUM str="1" src=20+1 @2:7
tok 8 type=OTHER str=")" src=21+1 @2:8
tok 9 type=NEWLINE str="\n" src=22+1 @2:9
tok 10 type=NEWLINE str="\n" src=39+1 @3:17
tok 11 type=SPACETAB str=" " src=-
tok 12 type=OTHER str="[" src=36+1 @3:14
tok 13 type=SPACETAB str=" " src=40+1 @4:1 u="M"
tok 14 type=SPACETAB str=" " src=40+1 @4:1 u="M"
tok 15 type=OTHER str="]" src=38+1 @3:16
tok 16 type=SPACETAB str=" " src=43+1 @4:4
tok 17 type=SPACETAB str=" " src=-
tok 18 type=OTHER str="[" src=36+1 @3:14
tok 19 type=SPACETAB str=" " src=44+1 @4:5 u="M"
tok 20 type=NUM str="1" src=46+1 @4:7
tok 21 type=SPACETAB str=" " src=44+1 @4:5 u="M"
tok 22 type=OTHER str="]" src=38+1 @3:16
tok 23 type=NEWLINE str="\n" src=50+1 @4:11
tok 24 type=NONE str="" src=51+0 @5:1
errors 2 (errors=2 warnings=0 has_errors=1)
err 0 ERROR file=@null row=1 col=11 msg="Expected identifier"
err 1 ERROR file=@null row=4 col=10 msg="Mismatching number of macro parameters"
parse true
tokens pp 8
tok 0 type=NEWLINE str="\n" src=14+1 @1:15
tok 1 type=SPACETAB str=" " src=-
tok 2 type=NAME str="a" src=13+1 @1:14
tok 3 type=OTHER str="(" src=17+1 @2:3
tok 4 type=NUM str="1" src=18+1 @2:4
tok 5 type=OTHER str=")" src=19+1 @2:5
tok 6 type=NEWLINE str="\n" src=20+1 @2:6
tok 7 type=NONE str="" src=21+0 @3:1
errors 1 (errors=1 warnings=0 has_errors=1)
err 0 ERROR file=@null row=2 col=2 msg="Expected '('"
"#,
        );
        let _ = c.tmp;
    }

    #[test]
    fn v22_pp_ifdef() {
        let mut c = Case::new();
        let mut out = String::new();
        let mut ed = ErrorData::new();
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            b"#define A\n#ifdef A\nyes\n#else\nno\n#endif\nafter\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        c.reset();
        ed = ErrorData::new();
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            b"#ifdef A\nyes\n#else\nno\n#endif\n#ifndef A\nn2\n#endif\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        c.reset();
        ed = ErrorData::new();
        run_case(&mut c, &mut out, &mut ed, b"#ifdef A\nyes\n", None);
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        c.reset();
        ed = ErrorData::new();
        run_case(&mut c, &mut out, &mut ed, b"#else\n#endif\n#elif\nx", None);
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        c.reset();
        ed = ErrorData::new();
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            b"#ifdef A junk\nq\n#endif trailing\nz\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        c.reset();
        ed = ErrorData::new();
        run_case(&mut c, &mut out, &mut ed, b"#define A\n#ifdef A\n#ifdef B\nab\n#else\na-nob\n#endif\n#else\nnoa\n#endif\n#ifndef A\n#ifdef Q\n#endif\n#endif\nend\n", None);
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        c.reset();
        ed = ErrorData::new();
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            b"#ifdef\nx\n#endif\n#ifdef 5\n#endif\n#undef\n#define\n#define 1\n#pragma once\nw\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        check(
            &c,
            &out,
            include_str!("../../../test/conformance/vectors/lexer/22-pp-ifdef.expected"),
        );
        let _ = c.tmp;
    }
    #[test]
    fn v23_pp_if() {
        let mut c = Case::new();
        let mut out = String::new();
        let mut ed = ErrorData::new();
        run_case(&mut c, &mut out, &mut ed, b"#define V 3\n#if V == 3\na\n#else\nb\n#endif\n#if V >= 4\nc\n#elif V < 3\nd\n#elif V\ne\n#else\nf\n#endif\n#if 0\ng\n#else\nh\n#endif\n#if V\ni\n#endif\n#if 0x10 == 16\np\n#endif\n#if 1 != 1\nq\n#endif\n#if 2 <= 1\nr\n#endif\n#if 2 > 1\ns\n#endif\n", None);
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        c.reset();
        ed = ErrorData::new();
        run_case(&mut c, &mut out, &mut ed, b"#define V 3\n#if V = 3\nj\n#endif\n#if V ~ 3\nk\n#endif\n#if W\nl\n#endif\n#if\nm\n#endif\n#if 1 == W\nn\n#endif\n#if 1 ==\no\n#endif\n#if 1 == 1 junk\nq\n#endif\n", None);
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        c.reset();
        ed = ErrorData::new();
        run_case(&mut c, &mut out, &mut ed, b"#define A B\n#define B 2\n#define M(x) 2\n#if A == 2\nyes\n#endif\n#if M == 2\nno\n#endif\n", None);
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        check(
            &c,
            &out,
            include_str!("../../../test/conformance/vectors/lexer/23-pp-if.expected"),
        );
        let _ = c.tmp;
    }
    #[test]
    #[cfg(not(miri))]
    // Miri isolation: writes real include files (lazy mkdir + fs::read in the
    // include loader).
    fn v24_pp_include() {
        let mut c = Case::new();
        let mut out = String::new();
        let mut ed = ErrorData::new();
        c.writefile("inc.h", b"#define INC 5\nincval INC\n");
        let file = c.tmpfile("main.cf");
        run_case(&mut c, &mut out, &mut ed, b"a\n#include \"inc.h\"\nb INC\n#include \"inc.h\"\n#include \"nofile.h\"\n#include <sys.h>\n#include foo\n#include \"x\" y\n", Some(&file));
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        c.writefile("bom.h", b"\xef\xbb\xbfbomtext\n");
        c.reset();
        ed = ErrorData::new();
        let file = c.tmpfile("m2.cf");
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            b"#include \"bom.h\"\n",
            Some(&file),
        );
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        check(
            &c,
            &out,
            include_str!("../../../test/conformance/vectors/lexer/24-pp-include.expected"),
        );
        let _ = c.tmp;
    }
    #[test]
    fn v25_pp_errors() {
        let mut c = Case::new();
        let mut out = String::new();
        let mut ed = ErrorData::new();
        run_case(&mut c, &mut out, &mut ed, b"#define\n#define 1\n#undef 2\n#ifdef\n#if\n#bogus x y\nok\n#define M(\n#define M(a b)\n#define M(a,)\n", Some("demo.cf"));
        out.push_str(&c.errors(&ed));
        out.push_str(&c.errstr(&ed));
        check(
            &c,
            &out,
            include_str!("../../../test/conformance/vectors/lexer/25-pp-errors.expected"),
        );
        let _ = c.tmp;
    }
    #[test]
    fn v26_pp_api_defs() {
        let mut c = Case::new();
        let mut out = String::new();
        let mut ed = ErrorData::new();
        let n = c.def("FOO", Some(b"7"));
        out.push_str(&format!("def added defines={n}\n"));
        let n = c.def("BAR", Some(b"a b"));
        out.push_str(&format!("def added defines={n}\n"));
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            b"FOO BAR FOO\n#ifdef FOO\ny\n#endif\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        let n = c.undef("FOO");
        out.push_str(&format!("undef defines={n}\n"));
        let n = c.undef("Nothing");
        out.push_str(&format!("undef defines={n}\n"));
        out.push_str(&c.errors(&ed));
        check(
            &c,
            &out,
            include_str!("../../../test/conformance/vectors/lexer/26-pp-api-defs.expected"),
        );
        let _ = c.tmp;
    }

    // -- vector 27: parameterized macro calls inside def bodies ---------------

    #[test]
    fn v27_pp_macro_in_def() {
        let mut c = Case::new();
        let mut out = String::new();
        let mut ed = ErrorData::new();
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            "#define F(a) a\n#define G(x) F(x)\nG(1)\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));

        c.reset();
        ed = ErrorData::new();
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            "#define ADD(a, b) a + b\n#define P(x, y) ADD(x, y)\nP(1, 2)\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));

        c.reset();
        ed = ErrorData::new();
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            "#define ADD(a, b) a + b\n#define P(x, y) ADD(x)\nP(1, 2)\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));

        check(
            &c,
            &out,
            include_str!("../../../test/conformance/vectors/lexer/27-pp-macro-in-def.expected"),
        );
        let _ = c.tmp;
    }

    #[test]
    fn v50_pp_quirks() {
        // Q-13, Q-6, Q-8, Q-9, unknown directive (the remaining 50 cases
        // are lexer/parser parts covered by the cf_lexer and cf_parser tests).
        let mut c = Case::new();
        let mut out = String::new();
        let mut ed = ErrorData::new();
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            b"#define A\n#ifdef A\nyes\n#elif B\nb\n#endif\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        c.reset();
        ed = ErrorData::new();
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            b"#define D 1\n#define D 2\nD\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        c.reset();
        ed = ErrorData::new();
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            b"#define F(a) a\nF\nnext\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        c.reset();
        ed = ErrorData::new();
        run_case(
            &mut c,
            &mut out,
            &mut ed,
            b"#define S (a) a\nS(1)\n#define Z() 0\nZ()\n",
            None,
        );
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        c.reset();
        ed = ErrorData::new();
        run_case(&mut c, &mut out, &mut ed, b"#pragma once\nx\n", None);
        out.push_str(&c.tokens_pp());
        out.push_str(&c.errors(&ed));
        check(
            &c,
            &out,
            "parse true
tokens pp 6
tok 0 type=NEWLINE str=\"\\n\" src=9+1 @1:10
tok 1 type=NEWLINE str=\"\\n\" src=18+1 @2:9
tok 2 type=NAME str=\"yes\" src=19+3 @3:1
tok 3 type=NEWLINE str=\"\\n\" src=22+1 @3:4
tok 4 type=NEWLINE str=\"\\n\" src=39+1 @6:7
tok 5 type=NONE str=\"\" src=40+0 @7:1
errors 1 (errors=1 warnings=0 has_errors=1)
err 0 ERROR file=@null row=4 col=7 msg=\"Unexpected token after preprocessor, expected newline\"
parse true
tokens pp 6
tok 0 type=NEWLINE str=\"\\n\" src=11+1 @1:12
tok 1 type=NEWLINE str=\"\\n\" src=23+1 @2:12
tok 2 type=SPACETAB str=\" \" src=-
tok 3 type=NUM str=\"1\" src=10+1 @1:11
tok 4 type=NEWLINE str=\"\\n\" src=25+1 @3:2
tok 5 type=NONE str=\"\" src=26+0 @4:1
parse true
tokens pp 6
tok 0 type=NEWLINE str=\"\\n\" src=14+1 @1:15
tok 1 type=SPACETAB str=\" \" src=-
tok 2 type=NAME str=\"a\" src=13+1 @1:14
tok 3 type=NAME str=\"next\" src=17+4 @3:1
tok 4 type=NEWLINE str=\"\\n\" src=21+1 @3:5
tok 5 type=NONE str=\"\" src=22+0 @4:1
errors 1 (errors=1 warnings=0 has_errors=1)
err 0 ERROR file=@null row=2 col=2 msg=\"Expected '('\"
parse true
tokens pp 12
tok 0 type=NEWLINE str=\"\\n\" src=15+1 @1:16
tok 1 type=SPACETAB str=\" \" src=-
tok 2 type=SPACETAB str=\" \" src=16+1 @2:1 u=\"S\"
tok 3 type=NUM str=\"1\" src=18+1 @2:3
tok 4 type=SPACETAB str=\" \" src=16+1 @2:1 u=\"S\"
tok 5 type=NEWLINE str=\"\\n\" src=20+1 @2:5
tok 6 type=NEWLINE str=\"\\n\" src=34+1 @3:14
tok 7 type=NAME str=\"Z\" src=35+1 @4:1
tok 8 type=OTHER str=\"(\" src=36+1 @4:2
tok 9 type=OTHER str=\")\" src=37+1 @4:3
tok 10 type=NEWLINE str=\"\\n\" src=38+1 @4:4
tok 11 type=NONE str=\"\" src=39+0 @5:1
errors 1 (errors=1 warnings=0 has_errors=1)
err 0 ERROR file=@null row=3 col=11 msg=\"Expected identifier\"
parse true
tokens pp 5
tok 0 type=SPACETAB str=\" \" src=7+1 @1:8
tok 1 type=NEWLINE str=\"\\n\" src=12+1 @1:13
tok 2 type=NAME str=\"x\" src=13+1 @2:1
tok 3 type=NEWLINE str=\"\\n\" src=14+1 @2:2
tok 4 type=NONE str=\"\" src=15+0 @3:1
errors 1 (errors=1 warnings=0 has_errors=1)
err 0 ERROR file=@null row=1 col=9 msg=\"Unexpected token after preprocessor, expected newline\"
",
        );
        let _ = c.tmp;
    }
    #[test]
    #[cfg(not(miri))]
    // Miri isolation: the D-4 case writes an empty include file.
    fn v90_defects() {
        let mut c = Case::new();
        let mut ed = ErrorData::new();
        c.writefile("emptyfile.h", b"");
        // D-4: an empty included file produces no tokens and no crash.
        let file = c.tmpfile("main.cf");
        let ok = c.parse(b"#include \"emptyfile.h\"\nx\n", Some(&file), &mut ed);
        assert!(ok, "parse of an empty include must succeed");
        assert!(!c.pp.tokens.is_empty(), "stream must end with a token");
        // D-3: a definition with an empty body expands to nothing (no crash).
        let mut c2 = Case::new();
        let n = c2.def("Q", None);
        assert_eq!(n, 1);
        let ok = c2.parse(b"Q\n", None, &mut ed);
        assert!(ok);
        // D-7: self-referencing macros terminate (name emitted verbatim).
        let mut c3 = Case::new();
        let ok = c3.parse(b"#define A A\nA\n", None, &mut ed);
        assert!(ok, "self-referencing macro must terminate");
        let mut c4 = Case::new();
        let ok = c4.parse(b"#define A B\n#define B A\nA\n", None, &mut ed);
        assert!(ok, "mutually recursive macros must terminate");
        let _ = (c.tmp, c2.tmp, c3.tmp, c4.tmp);
    }
}
