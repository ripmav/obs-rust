//! The tiny C-like declaration language of signals/procs (S-003;
//! `libobs/callback/decl.{c,h}`): e.g. `"void source_create(ptr source)"`.
//!
//! Hand-written scanner over `&[u8]` (bytes ≥ 0x80 form single
//! "unknown" tokens — no UTF-8 assumption). Grammar:
//!
//! ```text
//! declaration := RET_TYPE NAME '(' [ param_list ] ')'   (anything after ')' ignored)
//! param_list  := param { ',' param } [ ',' ]
//! param       := { 'in' | 'out' } TYPE NAME
//! TYPE        := 'int' | 'float' | 'bool' | 'ptr' | 'string'
//! ```
//!
//! Keywords are case-sensitive. Quirks reproduced: Q-1 leading WS
//! fails, Q-2 trailing junk ignored, Q-3 silent stop, Q-4 trailing
//! comma, Q-5 empty string fails without logging, D-1 non-NAME
//! function name → `false` + log, **never crash**.

use obs_util::bmem::{bfree_raw, bstrdup};
use obs_util::darray::DArray;

/// `enum call_param_type` (C enum = 4 bytes → `repr(C, u32)`).
#[repr(u32)]
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CallParamType {
    Void = 0,
    Int = 1,
    Float = 2,
    Bool = 3,
    Ptr = 4,
    String_ = 5,
}

/// `CALL_PARAM_IN` (bit 0).
pub const CALL_PARAM_IN: u32 = 1 << 0;
/// `CALL_PARAM_OUT` (bit 1).
pub const CALL_PARAM_OUT: u32 = 1 << 1;

/// S-003 §3: `struct decl_param` (3W).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct DeclParam {
    /// NUL-terminated name (S-001 allocator), owned by [`DeclInfo`].
    pub name: *mut u8,
    pub ty: CallParamType,
    /// `CALL_PARAM_IN` / `CALL_PARAM_OUT` bits (R02).
    pub flags: u32,
}

/// S-003 §3: `struct decl_info` (5W). `decl_string` is the pointer the
/// caller passed in (R01: **not copied**; it must outlive the decl).
#[repr(C)]
pub struct DeclInfo {
    /// NUL-terminated function name (S-001 allocator).
    pub name: *mut u8,
    /// Borrowed declaration string.
    pub decl_string: *const u8,
    /// Parameters in declaration order (R01/R03).
    pub params: DArray<DeclParam>,
}

/// S-003 §3 inline: `decl_param_free(p)` — frees the name, zeroes the
/// struct.
pub fn decl_param_free(p: &mut DeclParam) {
    if !p.name.is_null() {
        unsafe { bfree_raw(p.name) };
    }
    p.name = std::ptr::null_mut();
    p.ty = CallParamType::Void;
    p.flags = 0;
}

/// S-003 §3 inline: `decl_info_free(d)` — frees names and params,
/// zeroes the struct.
pub fn decl_info_free(d: &mut DeclInfo) {
    for p in d.params.as_slice() {
        if !p.name.is_null() {
            unsafe { bfree_raw(p.name) };
        }
    }
    d.params.free();
    if !d.name.is_null() {
        unsafe { bfree_raw(d.name) };
    }
    d.name = std::ptr::null_mut();
    d.decl_string = std::ptr::null();
}

/// One parser diagnostic (R11): 1-based line and column.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Diagnostic {
    pub line: u32,
    pub col: u32,
    pub message: String,
}

/// Owned parameter of the safe Rust API.
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct DeclParamOwned {
    pub name: String,
    pub ty: CallParamType,
    pub flags: u32,
}

/// A successfully parsed declaration (safe Rust API).
#[derive(Clone, Debug, PartialEq, Eq)]
pub struct Decl {
    pub name: String,
    pub params: Vec<DeclParamOwned>,
}

// ── Scanner ──────────────────────────────────────────────────────────────

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Tok {
    /// NAME at `[start, end)`.
    Name(usize, usize),
    /// A number at `[start, end)` (token, not parsed as a value).
    Num(usize, usize),
    /// A quoted string at `[start, end)`.
    Str(usize, usize),
    /// Any other single byte (punctuation, bytes ≥ 0x80 — S-003 §2).
    Punct(u8, usize),
    Eof(usize),
}

fn tok_pos(tok: &Tok) -> usize {
    match tok {
        Tok::Name(s, _) | Tok::Num(s, _) | Tok::Str(s, _) | Tok::Punct(_, s) | Tok::Eof(s) => *s,
    }
}

struct Scanner<'a> {
    src: &'a [u8],
    pos: usize,
}

impl<'a> Scanner<'a> {
    /// 1-based (line, column) of byte index `i`.
    fn line_col(&self, i: usize) -> (u32, u32) {
        let i = i.min(self.src.len());
        let mut line = 1u32;
        let mut start = 0usize;
        for (idx, &b) in self.src[..i].iter().enumerate() {
            if b == b'\n' {
                line += 1;
                start = idx + 1;
            }
        }
        (line, (i - start) as u32 + 1)
    }

    /// WS = spaces, tabs, newlines (incl. CR), `/* … */`, `// …` (S-003
    /// §2). Applied between tokens — never before the first one (Q-1).
    fn skip_ws(&mut self) {
        while self.pos < self.src.len() {
            let b = self.src[self.pos];
            match b {
                b' ' | b'\t' | b'\r' | b'\n' => self.pos += 1,
                b'/' if self.src.get(self.pos + 1) == Some(&b'*') => {
                    self.pos += 2;
                    while self.pos + 1 < self.src.len()
                        && !(self.src[self.pos] == b'*' && self.src[self.pos + 1] == b'/')
                    {
                        self.pos += 1;
                    }
                    if self.pos + 1 < self.src.len() {
                        self.pos += 2; // past "*/"
                    } else {
                        self.pos = self.src.len();
                    }
                }
                b'/' if self.src.get(self.pos + 1) == Some(&b'/') => {
                    self.pos += 2;
                    while self.pos < self.src.len() && self.src[self.pos] != b'\n' {
                        self.pos += 1;
                    }
                }
                _ => break,
            }
        }
    }

    fn next(&mut self, skip_ws: bool) -> (Tok, (u32, u32)) {
        if skip_ws {
            self.skip_ws();
        }
        let start = self.pos;
        let lc = self.line_col(start);
        if start >= self.src.len() {
            return (Tok::Eof(start), lc);
        }
        let b = self.src[start];
        if b.is_ascii_alphabetic() || b == b'_' {
            let mut p = start + 1;
            while p < self.src.len() && (self.src[p].is_ascii_alphanumeric() || self.src[p] == b'_')
            {
                p += 1;
            }
            self.pos = p;
            (Tok::Name(start, p), lc)
        } else if b.is_ascii_digit() {
            let mut p = start + 1;
            while p < self.src.len() && self.src[p].is_ascii_digit() {
                p += 1;
            }
            self.pos = p;
            (Tok::Num(start, p), lc)
        } else if b == b'"' {
            let mut p = start + 1;
            while p < self.src.len() && self.src[p] != b'"' {
                p += 1;
            }
            if p < self.src.len() {
                p += 1; // closing quote
            }
            self.pos = p;
            (Tok::Str(start, p), lc)
        } else {
            self.pos = start + 1;
            (Tok::Punct(b, start), lc)
        }
    }
}

// ── Keywords (case-sensitive, S-003 §4) ─────────────────────────────────

fn type_of(name: &str) -> Option<CallParamType> {
    match name {
        "int" => Some(CallParamType::Int),
        "float" => Some(CallParamType::Float),
        "bool" => Some(CallParamType::Bool),
        "ptr" => Some(CallParamType::Ptr),
        "string" => Some(CallParamType::String_),
        _ => None,
    }
}

/// R05 reserved names: the five types + void + return. `in`/`out` are
/// **not** reserved as names.
fn is_reserved(name: &str) -> bool {
    matches!(
        name,
        "int" | "float" | "bool" | "ptr" | "string" | "void" | "return"
    )
}

// ── Parser (byte core; no UTF-8 assumption) ─────────────────────────────

struct Parsed {
    name: Vec<u8>,
    params: Vec<(Vec<u8>, CallParamType, u32)>,
}

fn diag(sc: &Scanner<'_>, pos: usize, message: &str) -> Diagnostic {
    let (line, col) = sc.line_col(pos);
    Diagnostic {
        line,
        col,
        message: message.to_string(),
    }
}

/// The byte-level parse core. Returns diagnostics on failure; an empty
/// input fails **without** diagnostics (Q-5: no log).
fn parse_decl_bytes(src: &[u8]) -> Result<Parsed, Vec<Diagnostic>> {
    if src.is_empty() {
        return Err(vec![]); // Q-5
    }
    let mut sc = Scanner { src, pos: 0 };

    // Return type — the first token, no leading WS (Q-1).
    let (tok, _) = sc.next(false);
    let ret = match &tok {
        Tok::Name(s, e) => {
            let name = std::str::from_utf8(&src[*s..*e]).unwrap_or("");
            if name == "void" {
                None
            } else {
                match type_of(name) {
                    Some(t) => Some(t),
                    None => {
                        return Err(vec![diag(&sc, *s, "Expected 'return type'")]);
                    }
                }
            }
        }
        _ => return Err(vec![diag(&sc, tok_pos(&tok), "Expected 'return type'")]),
    };

    // Function name (D-1: not a NAME → error + false, never crash).
    let (tok, _) = sc.next(true);
    let name = match &tok {
        Tok::Name(s, e) => {
            let n = &src[*s..*e];
            if is_reserved(std::str::from_utf8(n).unwrap_or("")) {
                return Err(vec![diag(
                    &sc,
                    *s,
                    &format!("'{}' is a reserved name", String::from_utf8_lossy(n)),
                )]);
            }
            n.to_vec()
        }
        _ => return Err(vec![diag(&sc, tok_pos(&tok), "Expected function name")]),
    };

    // '('
    let (tok, _) = sc.next(true);
    if !matches!(tok, Tok::Punct(b'(', _)) {
        return Err(vec![diag(&sc, tok_pos(&tok), "Expected '('")]);
    }

    // Parameter list (S-003 §5.1): the first token decides the entry,
    // then the after-param loop owns the rest.
    let mut params: Vec<(Vec<u8>, CallParamType, u32)> = vec![];
    let (tok, _) = sc.next(true);
    match &tok {
        Tok::Punct(b')', _) => {} // empty list — done
        Tok::Name(s, e) => {
            if let Some(d) = parse_param(&mut sc, &mut params, src, (*s, *e)) {
                return Err(vec![d]);
            }
            // After a param (step 4):
            loop {
                let (tok, _) = sc.next(true);
                match &tok {
                    Tok::Punct(b')', _) => break, // Q-2: the rest is ignored
                    Tok::Punct(b',', _) => {
                        let (tok2, _) = sc.next(true);
                        match &tok2 {
                            Tok::Punct(b')', _) => break, // Q-4 trailing comma
                            Tok::Name(s, e) => {
                                if let Some(d) = parse_param(&mut sc, &mut params, src, (*s, *e)) {
                                    return Err(vec![d]);
                                }
                            }
                            Tok::Eof(_) => {
                                return Err(vec![diag(&sc, sc.pos, "Unexpected EOF")]);
                            }
                            _ => return Err(vec![diag(&sc, tok_pos(&tok2), "Expected ')'")]),
                        }
                    }
                    Tok::Eof(_) => return Err(vec![diag(&sc, sc.pos, "Unexpected EOF")]),
                    _ => break, // Q-3: silent stop — the list ends, accepted
                }
            }
        }
        Tok::Eof(_) => return Err(vec![diag(&sc, sc.pos, "Unexpected EOF")]),
        _ => return Err(vec![diag(&sc, tok_pos(&tok), "Expected ')'")]),
    }

    // R03: a non-void return type appends the "return" parameter.
    if let Some(t) = ret {
        params.push((b"return".to_vec(), t, CALL_PARAM_OUT));
    }
    Ok(Parsed { name, params })
}

/// Parse one parameter; `first` is its first NAME token.
fn parse_param(
    sc: &mut Scanner<'_>,
    params: &mut Vec<(Vec<u8>, CallParamType, u32)>,
    src: &[u8],
    first: (usize, usize),
) -> Option<Diagnostic> {
    let mut flags: u32 = 0;
    let mut seen_in = false;
    let mut seen_out = false;
    let mut cur = first;
    loop {
        let name = std::str::from_utf8(&src[cur.0..cur.1]).unwrap_or("");
        if name == "in" {
            if seen_in {
                return Some(diag(sc, cur.0, "`in` specifier already exists"));
            }
            seen_in = true;
            flags |= CALL_PARAM_IN;
        } else if name == "out" {
            if seen_out {
                return Some(diag(sc, cur.0, "`out` specifier already exists"));
            }
            seen_out = true;
            flags |= CALL_PARAM_OUT;
        } else if let Some(ty) = type_of(name) {
            // R08: the TYPE must be followed by a NAME.
            let (tok2, _) = sc.next(true);
            return match &tok2 {
                Tok::Name(s, e) => {
                    let n = &src[*s..*e];
                    let ns = std::str::from_utf8(n).unwrap_or("");
                    if is_reserved(ns) {
                        return Some(diag(sc, *s, &format!("'{}' is a reserved name", ns)));
                    }
                    if params.iter().any(|(p, _, _)| p == n) {
                        return Some(diag(sc, *s, &format!("duplicate parameter name '{}'", ns)));
                    }
                    let flags = if flags == 0 {
                        CALL_PARAM_IN // R02: no specifier → IN
                    } else {
                        flags
                    };
                    params.push((n.to_vec(), ty, flags));
                    None
                }
                _ => Some(diag(sc, tok_pos(&tok2), "Expected 'parameter name'")),
            };
        } else {
            return Some(diag(sc, cur.0, "Expected 'type'"));
        }
        let (tok, _) = sc.next(true);
        match &tok {
            Tok::Name(s, e) => cur = (*s, *e),
            _ => return Some(diag(sc, tok_pos(&tok), "Expected 'type'")),
        }
    }
}

// ── Public API ───────────────────────────────────────────────────────────

/// S-003: parse a declaration string (safe Rust API). `Ok(decl)` on
/// success; `Err(diagnostics)` on failure — `Err(vec![])` for the empty
/// string (Q-5: the C function fails without logging).
#[must_use = "the parse result (decl or diagnostics)"]
pub fn parse_decl(decl_string: &str) -> Result<Decl, Vec<Diagnostic>> {
    parse_decl_bytes(decl_string.as_bytes()).map(|p| Decl {
        name: String::from_utf8(p.name).unwrap_or_default(),
        params: p
            .params
            .into_iter()
            .map(|(n, ty, flags)| DeclParamOwned {
                name: String::from_utf8(n).unwrap_or_default(),
                ty,
                flags,
            })
            .collect(),
    })
}

/// S-003 §3: the exported C function. On success fills `decl` (R01/R02/
/// R03, names via `bstrdup`, `decl_string` borrowed); on failure
/// resets `decl` (R10) and logs one R11 warning unless the string was
/// empty (Q-5). D-1 inputs return `false` and log — never crash.
#[must_use = "the parse succeeded only if this is true"]
pub fn parse_decl_string(decl: &mut DeclInfo, decl_string: &[u8]) -> bool {
    *decl = DeclInfo {
        name: std::ptr::null_mut(),
        decl_string: std::ptr::null(),
        params: DArray::new(),
    };
    match parse_decl_bytes(decl_string) {
        Err(diags) => {
            // R11: one LOG_WARNING (level MUST, text SHOULD) — empty
            // input produces no diagnostics, hence no log (Q-5).
            if !diags.is_empty() {
                let s = String::from_utf8_lossy(decl_string);
                let lines: Vec<String> = diags
                    .iter()
                    .map(|d| format!("declaration ({}, {}): {}", d.line, d.col, d.message))
                    .collect();
                eprintln!("Errors/warnings for '{}':\n\n{}", s, lines.join("\n"));
            }
            false
        }
        Ok(parsed) => {
            decl.name = bstrdup(&parsed.name).into_raw();
            decl.decl_string = decl_string.as_ptr();
            for (n, ty, flags) in &parsed.params {
                decl.params.push_back(&DeclParam {
                    name: bstrdup(n).into_raw(),
                    ty: *ty,
                    flags: *flags,
                });
            }
            true
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::calldata::W;

    fn ok(s: &str) -> Decl {
        parse_decl(s).unwrap_or_else(|e| panic!("parse of {s:?} failed: {e:?}"))
    }
    fn fail(s: &str) -> Vec<Diagnostic> {
        match parse_decl(s) {
            Ok(_) => panic!("parse of {s:?} unexpectedly succeeded"),
            Err(e) => e,
        }
    }
    fn names(d: &Decl) -> Vec<&str> {
        d.params.iter().map(|p| p.name.as_str()).collect()
    }

    #[test]
    fn layout_is_c_compatible() {
        assert_eq!(std::mem::size_of::<CallParamType>(), 4, "C enum = int");
        // C layout: char* name @0 (8), enum @8 (4), uint32_t @12 (4)
        assert_eq!(std::mem::size_of::<DeclParam>(), 16);
        assert_eq!(std::mem::offset_of!(DeclParam, name), 0);
        assert_eq!(std::mem::offset_of!(DeclParam, ty), W);
        assert_eq!(std::mem::offset_of!(DeclParam, flags), W + 4);
        assert_eq!(std::mem::size_of::<DeclInfo>(), 5 * W);
        assert_eq!(std::mem::offset_of!(DeclInfo, name), 0);
        assert_eq!(std::mem::offset_of!(DeclInfo, decl_string), W);
        assert_eq!(std::mem::offset_of!(DeclInfo, params), 2 * W);
    }

    #[test]
    fn vec01_valid() {
        let d = ok("void source_create(ptr source)");
        assert_eq!(d.name, "source_create");
        assert_eq!(names(&d), vec!["source"]);
        assert_eq!(d.params[0].ty, CallParamType::Ptr);
        assert_eq!(d.params[0].flags, CALL_PARAM_IN);

        let d = ok("int add(int a, int b)");
        assert_eq!(names(&d), vec!["a", "b", "return"]);
        assert_eq!(d.params[2].ty, CallParamType::Int);
        assert_eq!(d.params[2].flags, CALL_PARAM_OUT);

        let d = ok("void f()");
        assert!(d.params.is_empty());

        let d = ok("string f(string s, ptr p)");
        assert_eq!(d.params[0].ty, CallParamType::String_);
        assert_eq!(d.params[1].ty, CallParamType::Ptr);
    }

    #[test]
    fn vec02_invalid() {
        assert!(
            fail("void f(void a)")
                .iter()
                .any(|d| d.message == "Expected 'type'"),
            "R04: void is not a param type"
        );
        assert!(
            fail("void f(int a, int a)")
                .iter()
                .any(|d| d.message.starts_with("duplicate")),
            "R06"
        );
        assert!(
            fail("void f(in in int a)")
                .iter()
                .any(|d| d.message.contains("specifier already")),
            "R07 in"
        );
        assert!(
            fail("void f(out out int a)")
                .iter()
                .any(|d| d.message.contains("specifier already")),
            "R07 out"
        );
        assert!(!fail("int int(int a)").is_empty(), "R05 function name");
        assert!(!fail("void int()").is_empty(), "R05 function name");
        assert!(
            fail("void f(int)")
                .iter()
                .any(|d| d.message == "Expected 'parameter name'"),
            "R08"
        );
        assert!(
            fail("void f(in)")
                .iter()
                .any(|d| d.message == "Expected 'type'"),
            "R08"
        );
        assert!(!fail("int f(int a").is_empty(), "EOF after param");
        assert!(!fail("1f()").is_empty(), "leading number");
        assert!(!fail("void f(void)").is_empty(), "R04 in param position");
    }

    #[test]
    fn vec03_case_and_keywords() {
        assert!(!fail("VOID f()").is_empty(), "case-sensitive return type");
        assert!(
            fail("void Int(a)")
                .iter()
                .any(|d| d.message == "Expected 'type'"),
            "bare name is not a type"
        );
        assert!(!fail("void f(IN a)").is_empty(), "IN is not a specifier");
        assert_eq!(names(&ok("void f(int In)")), vec!["In"]);
        assert_eq!(
            names(&ok("void f(int in)")),
            vec!["in"],
            "R05: in/out are names"
        );
        let d = ok("void in(int out)");
        assert_eq!(d.name, "in");
        assert_eq!(names(&d), vec!["out"]);
    }

    #[test]
    fn vec04_return_parameter() {
        let d = ok("int f()");
        assert_eq!(names(&d), vec!["return"], "R03: int f() has 1 param");
        assert_eq!(d.params[0].flags, CALL_PARAM_OUT);
        assert!(ok("void f()").params.is_empty());
        assert_eq!(ok("string f()").params[0].ty, CallParamType::String_);
        // R02: specifier combos
        assert_eq!(
            ok("void f(in out int x)").params[0].flags,
            CALL_PARAM_IN | CALL_PARAM_OUT
        );
        assert_eq!(
            ok("void f(out in int x)").params[0].flags,
            CALL_PARAM_IN | CALL_PARAM_OUT
        );
        assert_eq!(ok("void f(out int x)").params[0].flags, CALL_PARAM_OUT);
    }

    #[test]
    fn vec05_lenient_quirks() {
        assert!(!fail(" void f()").is_empty(), "Q-1 leading space");
        assert!(!fail("\tvoid f()").is_empty(), "Q-1 leading tab");
        assert!(!fail("/* c */ void f()").is_empty(), "Q-1 leading comment");
        assert!(
            !fail("// c\nvoid f()").is_empty(),
            "Q-1 leading line comment"
        );
        assert_eq!(ok("void f() extra").name, "f", "Q-2 trailing junk");
        assert_eq!(ok("void f(int a))").name, "f", "Q-2");
        assert_eq!(ok("void f(int a) )").name, "f", "Q-2");
        assert_eq!(
            names(&ok("void f(int a int b)")),
            vec!["a"],
            "Q-3 silent stop"
        );
        assert_eq!(names(&ok("void f(int a; int b)")), vec!["a"], "Q-3");
        assert_eq!(names(&ok("void f(int a, int b c)")), vec!["a", "b"], "Q-3");
        assert_eq!(
            names(&ok("void f(int a,)")),
            vec!["a"],
            "Q-4 trailing comma"
        );
        assert_eq!(names(&ok("void f(int a, int b, )")), vec!["a", "b"], "Q-4");
        // Q-5: empty string fails without diagnostics (no log)
        let e = parse_decl("");
        assert!(e.is_err());
        assert!(e.unwrap_err().is_empty());
    }

    // D-1: the reference crashes on these; we must fail + diagnose.
    #[test]
    fn vec06_reference_crashes_d1() {
        for s in [
            "void 1f()",
            "void 123()",
            "void +()",
            "void \"s\"()",
            "void \u{e9}()",
            "int 1f(int a)",
        ] {
            let e = parse_decl(s);
            assert!(e.is_err(), "{s:?} must fail");
            assert!(!e.unwrap_err().is_empty(), "{s:?} must be logged");
        }
    }

    #[test]
    fn vec07_many_and_long() {
        let mut decl = String::from("void f(");
        for i in 0..16 {
            if i > 0 {
                decl.push(',');
            }
            decl.push_str(&format!("int p{i}"));
        }
        decl.push(')');
        assert_eq!(ok(&decl).params.len(), 16, "R12: 16 params");
        let long: String = std::iter::repeat_n('a', 90).collect();
        let d = ok(&format!("void {long}()"));
        assert_eq!(d.name.len(), 90, "R12: 90-char name");
    }

    #[test]
    fn vec08_lexical() {
        assert_eq!(names(&ok("void /* x */ f(int // c\n a)")), vec!["a"]);
        assert_eq!(names(&ok("void f(int a /* x */ , int b)")), vec!["a", "b"]);
        assert!(
            parse_decl("void f(int \u{e9})").is_err(),
            "high byte is not a NAME"
        );
        // numbers and quoted strings are tokens, not names
        assert!(parse_decl("void 5f()").is_err());
        assert!(parse_decl("void f(int \"a\")").is_err());
    }

    #[test]
    fn vec09_parameter_list_edges() {
        assert!(ok("void f()").params.is_empty());
        assert!(!fail("void f(int a").is_empty(), "EOF after param");
        assert!(!fail("void f(").is_empty(), "EOF before param");
        assert!(!fail("void f(int a, ").is_empty(), "EOF after comma");
        // Q-3: a non-, non-) token after a param stops the list silently
        assert_eq!(names(&ok("void f(int a 5)")), vec!["a"]);
    }

    #[test]
    fn c_facing_roundtrip_and_free() {
        let mut info = DeclInfo {
            name: std::ptr::null_mut(),
            decl_string: std::ptr::null(),
            params: DArray::new(),
        };
        let s = b"int add(int a, int b)";
        assert!(parse_decl_string(&mut info, s));
        assert_eq!(info.params.num, 3);
        assert_eq!(
            unsafe { std::slice::from_raw_parts(info.params.as_slice()[0].name, 1) },
            b"a"
        );
        assert_eq!(
            info.decl_string as usize,
            s.as_ptr() as usize,
            "R01: borrowed, not copied"
        );
        let fname = unsafe { std::slice::from_raw_parts(info.name, 3) };
        assert_eq!(fname, b"add");
        decl_info_free(&mut info);
        assert!(info.name.is_null());
        assert_eq!(info.params.num, 0);
        assert!(info.decl_string.is_null());
        // failure path resets (R10)
        let mut bad = DeclInfo {
            name: std::ptr::null_mut(),
            decl_string: std::ptr::null(),
            params: DArray::new(),
        };
        assert!(!parse_decl_string(&mut bad, b"void 1f()"));
        assert!(bad.name.is_null());
        assert_eq!(bad.params.num, 0);
    }

    #[test]
    fn diagnostics_carry_positions() {
        let diags = fail("void f(\n  int a, int a)");
        let dup = diags
            .iter()
            .find(|d| d.message.starts_with("duplicate"))
            .unwrap();
        assert_eq!(dup.line, 2);
        assert_eq!(dup.col, 14);
    }
}
