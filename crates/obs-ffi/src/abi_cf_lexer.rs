//! P1-08.5 recipe step 2 (part B1): the C ABI for the C-family lexer
//! (`libobs/util/cf-lexer.h`).
//!
//! The nine `EXPORT` functions with the exact C signatures; the `static
//! inline` helpers (`cf_token_clear`, `cf_def_addtoken`, `cf_lexer_get_tokens`,
//! ...) stay C-side — the conformance runners, `cf-parser.c` and the effect
//! parser call them directly.
//!
//! Allocation contract (enforced in [`crate::abi_common`]): everything the C
//! side later `bfree`s comes from the C allocator. `cf_lexer_lex` bstrdups
//! the text and the file name and bmallocs the reformatted buffer; the token
//! darrays, `cf_def` darrays and dependency lexers grow with `bmalloc`;
//! `cf_literal_to_str` returns a bstrdup'd string. Tokens are views:
//! `str` points into the owning lexer's `reformatted` buffer,
//! `unmerged_str` into its original text — the runner finds the owner by
//! address range (`owner_of`), so the views must point into those exact C
//! buffers.
//!
//! The tokenizing loop runs through the verified engine
//! ([`obs_util::cf_lexer::CfLexer`], pinned by vectors 10-15) and the result
//! is copied into the C-owned buffers; the preprocessor is a faithful
//! in-place port of the cf-lexer.c preprocessor section (its crash behaviour
//! on defect inputs is pinned by `90-defects`: an empty included file or an
//! empty def body dereferences a NULL token array, recursive macros
//! overflow the stack — the port keeps those dereferences and the
//! unbounded recursion).

use std::mem::size_of;
use std::os::raw::{c_char, c_int, c_void};
use std::ptr;

use obs_util::cf_lexer::{CfLexer, CfTokenType, cf_literal_to_str as engine_cf_literal_to_str};
use obs_util::lexer::{LEX_ERROR, LEX_WARNING};

use crate::abi_common::{bfree, bmalloc_c, bstrdup, c_bytes, error_data_add_c};
use crate::abi_lexer::{error_data, lexer, strref};

/* ------------------------------------------------------------------------- */
/* C struct mirrors (exact field layout, C owns the memory) */

/// C `enum cf_token_type` values (an `int` in C).
pub const CFTOKEN_NONE: c_int = 0;
/// `CFTOKEN_NAME`.
pub const CFTOKEN_NAME: c_int = 1;
/// `CFTOKEN_NUM`.
pub const CFTOKEN_NUM: c_int = 2;
/// `CFTOKEN_SPACETAB`.
pub const CFTOKEN_SPACETAB: c_int = 3;
/// `CFTOKEN_NEWLINE`.
pub const CFTOKEN_NEWLINE: c_int = 4;
/// `CFTOKEN_STRING`.
pub const CFTOKEN_STRING: c_int = 5;
/// `CFTOKEN_OTHER`.
pub const CFTOKEN_OTHER: c_int = 6;

/// C `struct cf_token`: the owning lexer plus views into its buffers.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct cf_token {
    pub lex: *const cf_lexer,
    pub str: strref,
    pub unmerged_str: strref,
    /// `enum cf_token_type` (an `int` in C): a `CFTOKEN_*` value.
    pub r#type: c_int,
}

impl cf_token {
    /// C `cf_token_clear` (a `memset` to zero).
    pub fn clear() -> Self {
        cf_token {
            lex: ptr::null(),
            str: strref {
                array: ptr::null(),
                len: 0,
            },
            unmerged_str: strref {
                array: ptr::null(),
                len: 0,
            },
            r#type: CFTOKEN_NONE,
        }
    }
}

/// C `DARRAY(type)` (darray.h): `{ T *array; size_t num; size_t capacity; }`.
/// Growth and freeing go through the C allocator exactly like the C macros
/// (`darray_ensure_capacity` copies the old *capacity*, not `num`).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct darray<T> {
    pub array: *mut T,
    pub num: usize,
    pub capacity: usize,
}

impl<T: Copy> darray<T> {
    /// C `da_init`.
    pub fn init() -> Self {
        darray {
            array: ptr::null_mut(),
            num: 0,
            capacity: 0,
        }
    }

    /// C `da_free` (a NULL array frees nothing).
    pub fn free(&mut self) {
        // SAFETY: C-ABI darray drop — `array` is a bmem buffer (capacity·esize) owned exclusively by this array (NULL is a no-op).
        unsafe { bfree(self.array as *mut u8) };
        *self = darray::init();
    }

    /// C `darray_ensure_capacity` for `new_size` elements.
    fn ensure(&mut self, new_size: usize) {
        if new_size <= self.capacity {
            return;
        }
        let esize = size_of::<T>();
        let new_cap = if self.capacity == 0 {
            new_size
        } else {
            (self.capacity * 2).max(new_size)
        };
        // SAFETY: C-ABI darray growth — the fresh bmalloc block is valid and the copy fits in old capacity·esize.
        unsafe {
            let p = bmalloc_c(esize * new_cap) as *mut T;
            if !self.array.is_null() && self.capacity > 0 {
                ptr::copy_nonoverlapping(self.array, p, self.capacity);
                bfree(self.array as *mut u8);
            }
            self.array = p;
            self.capacity = new_cap;
        }
    }

    /// C `da_push_back`.
    pub fn push(&mut self, item: &T) {
        self.num += 1;
        self.ensure(self.num);
        // SAFETY: C-ABI — num ≥ 1 (checked above); the slot fits esize bytes; `item` is a valid T.
        unsafe { self.array.add(self.num - 1).write(*item) };
    }

    /// C `da_push_back_new` (zeroed element).
    pub fn push_new(&mut self) -> *mut T {
        self.num += 1;
        self.ensure(self.num);
        // SAFETY: C-ABI — num ≥ 1 (checked above); the slot is within the buffer.
        let p = unsafe { self.array.add(self.num - 1) };
        // SAFETY: C-ABI — `p` is the slot just fetched; it fits esize bytes within the buffer.
        unsafe { ptr::write_bytes(p as *mut u8, 0, size_of::<T>()) };
        p
    }

    /// C `da_erase` (no-op for `idx >= num`).
    pub fn erase(&mut self, idx: usize) {
        if idx >= self.num {
            return;
        }
        self.num -= 1;
        if self.num == 0 {
            return;
        }
        // SAFETY: C-ABI — the overlapping tail shift stays within the buffer (idx < num checked above).
        unsafe { ptr::copy(self.array.add(idx + 1), self.array.add(idx), self.num - idx) };
    }

    /// Element `i` (caller-checked range).
    pub fn get(&self, i: usize) -> &T {
        // SAFETY: C-ABI — i < num (checked above); the element is a valid T.
        unsafe { &*self.array.add(i) }
    }
}

/// C `struct cf_lexer`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct cf_lexer {
    pub file: *mut c_char,
    pub base_lexer: lexer,
    pub reformatted: *mut c_char,
    pub write_offset: *mut c_char,
    pub tokens: darray<cf_token>,
    pub unexpected_eof: bool,
}

impl cf_lexer {
    /// Zeroed `cf_lexer` (C `cf_lexer_init`).
    pub fn init() -> Self {
        cf_lexer {
            file: ptr::null_mut(),
            base_lexer: lexer {
                text: ptr::null_mut(),
                offset: ptr::null(),
            },
            reformatted: ptr::null_mut(),
            write_offset: ptr::null_mut(),
            tokens: darray::init(),
            unexpected_eof: false,
        }
    }
}

/// C `struct cf_def`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct cf_def {
    pub name: cf_token,
    pub params: darray<cf_token>,
    pub tokens: darray<cf_token>,
    pub r#macro: bool,
}

/// C `struct cf_preprocessor`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct cf_preprocessor {
    pub lex: *mut cf_lexer,
    pub ed: *mut error_data,
    pub defines: darray<cf_def>,
    pub sys_include_dirs: darray<*mut c_char>,
    pub dependencies: darray<cf_lexer>,
    pub tokens: darray<cf_token>,
    pub ignore_state: bool,
}

impl cf_preprocessor {
    /// Zeroed `cf_preprocessor` (C `cf_preprocessor_init`).
    pub fn init() -> Self {
        cf_preprocessor {
            lex: ptr::null_mut(),
            ed: ptr::null_mut(),
            defines: darray::init(),
            sys_include_dirs: darray::init(),
            dependencies: darray::init(),
            tokens: darray::init(),
            ignore_state: false,
        }
    }
}

/// C `static char space_filler[2] = " ";` (cf-lexer.c): the text of
/// synthesized blank tokens.
static SPACE_FILLER: [c_char; 2] = [b' ' as c_char, 0];

/* ------------------------------------------------------------------------- */
/* C file API (`gen/stubs.c` in the conformance build, platform code in OBS) */

unsafe extern "C" {
    fn os_fopen(path: *const c_char, mode: *const c_char) -> *mut c_void;
    fn os_fread_utf8(file: *mut c_void, pstr: *mut *mut c_char) -> usize;
    fn fclose(file: *mut c_void) -> c_int;
}

/* ------------------------------------------------------------------------- */
/* small helpers */

/// The bytes of a C `strref` (a NULL `array` is an empty span).
fn sr_bytes(s: &strref) -> &[u8] {
    if s.array.is_null() {
        &[]
    } else {
        // SAFETY: C-ABI — `s.array` is a valid bmem buffer and s.len ≤ s.capacity.
        unsafe { std::slice::from_raw_parts(s.array.cast::<u8>(), s.len) }
    }
}

/// C `dstr_replace(dst, find, replace)`: one left-to-right pass replacing
/// every occurrence of `find` with `replace` (the `strstr` loop in dstr.c
/// resumes after each inserted replacement).
fn dstr_replace(s: &mut Vec<u8>, find: &[u8], replace: &[u8]) {
    let mut out = Vec::with_capacity(s.len());
    let mut i = 0;
    while i < s.len() {
        if s[i..].starts_with(find) {
            out.extend_from_slice(replace);
            i += find.len();
        } else {
            out.push(s[i]);
            i += 1;
        }
    }
    *s = out;
}

/// C `dstr_safe_printf(dst, format, v1, v2, v3, NULL)`: copy the format,
/// then replace every `$1`/`$2`/`$3` with the non-NULL values, in that
/// order (`dstr_replace` loop semantics). `pub(crate)`: the cf-parser
/// bridge's `cf_adderror` formats through the same C function.
pub(crate) fn safe_printf(
    format: &[u8],
    v1: *const c_char,
    v2: *const c_char,
    v3: *const c_char,
) -> Vec<u8> {
    let mut s = format.to_vec();
    if !v1.is_null()
        && let Some(b) = c_bytes(v1)
    {
        dstr_replace(&mut s, b"$1", b);
    }
    if !v2.is_null()
        && let Some(b) = c_bytes(v2)
    {
        dstr_replace(&mut s, b"$2", b);
    }
    if !v3.is_null()
        && let Some(b) = c_bytes(v3)
    {
        dstr_replace(&mut s, b"$3", b);
    }
    s
}

/// C `sscanf(s, "%i", &v)`: optional blanks, optional sign, then decimal,
/// octal after a leading `0` or hexadecimal after `0x`/`0X`. glibc: a
/// leading `0` counts as a digit of its own, so `"08"`, `"0x"` and
/// `"0xG10"` all succeed with value 0. `Some` only on a successful
/// conversion. The reference passes the token's `str` pointer, i.e. the
/// rest of the buffer (Q-14).
fn c_scan_int(s: &[u8]) -> Option<i64> {
    let mut i = 0;
    while i < s.len() && (s[i] as char).is_ascii_whitespace() {
        i += 1;
    }
    let mut neg = false;
    if i < s.len() && (s[i] == b'+' || s[i] == b'-') {
        neg = s[i] == b'-';
        i += 1;
    }
    if i >= s.len() {
        return None;
    }
    let mut v: i64 = 0;
    let mut base: u32 = 10;
    let mut zero_seen = false;
    if s[i] == b'0' {
        zero_seen = true;
        if i + 1 < s.len() {
            if s[i + 1] == b'x' || s[i + 1] == b'X' {
                i += 2;
                base = 16;
            } else {
                i += 1;
                base = 8;
            }
        }
    }
    let start = i;
    while i < s.len() {
        let Some(d) = (s[i] as char).to_digit(base) else {
            break;
        };
        v = v.wrapping_mul(base as i64).wrapping_add(d as i64);
        i += 1;
    }
    if !zero_seen && i == start {
        return None;
    }
    Some(if neg { v.wrapping_neg() } else { v })
}

/// `CfTokenType` -> C `CFTOKEN_*` value (same order as the C enum).
fn mat_type(t: CfTokenType) -> c_int {
    match t {
        CfTokenType::None => CFTOKEN_NONE,
        CfTokenType::Name => CFTOKEN_NAME,
        CfTokenType::Num => CFTOKEN_NUM,
        CfTokenType::Spacetab => CFTOKEN_SPACETAB,
        CfTokenType::Newline => CFTOKEN_NEWLINE,
        CfTokenType::String => CFTOKEN_STRING,
        CfTokenType::Other => CFTOKEN_OTHER,
    }
}

/* ------------------------------------------------------------------------- */
/* EXPORT: cf_literal_to_str */

/// `EXPORT char *cf_literal_to_str(const char *literal, size_t count)`
///
/// Decodes a quoted C literal (the Q-10 loop quirk, verified by vector 33
/// through the engine); a `count` of 0 means the whole string. The result
/// (or NULL) is C-allocated and freed with the C `bfree`.
///
/// # Safety
/// `literal` (or NULL) must point to a NUL-terminated string readable for
/// the whole call; `count` (when non-zero) is the C length of the quoted
/// literal.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cf_literal_to_str(literal: *const c_char, count: usize) -> *mut c_char {
    // C reads `literal[0]` unconditionally (NULL is a crash in the
    // reference); the runners never pass NULL.
    let Some(bytes) = c_bytes(literal) else {
        return ptr::null_mut();
    };
    let Some(out) = engine_cf_literal_to_str(bytes, count) else {
        return ptr::null_mut();
    };
    let mut buf = out;
    buf.push(0);
    // SAFETY: C-ABI — `buf` is a valid C string buffer (checked above); the result is bmem-owned.
    unsafe { bstrdup(buf.as_ptr().cast::<c_char>()) }
}

/* ------------------------------------------------------------------------- */
/* EXPORT: cf_lexer_init / cf_lexer_free / cf_lexer_lex */

/// `EXPORT void cf_lexer_init(struct cf_lexer *lex)`
///
/// # Safety
/// `lex` must point to writable memory of size `cf_lexer`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cf_lexer_init(lex: *mut cf_lexer) {
    // SAFETY: C-ABI — `lex` is valid out storage (caller contract).
    unsafe { *lex = cf_lexer::init() };
}

/// `EXPORT void cf_lexer_free(struct cf_lexer *lex)`
///
/// Frees the file name, the original text, the reformatted buffer and the
/// token array (all C-allocated), then zeroes the struct like C.
///
/// # Safety
/// `lex` must point to a `cf_lexer` set up by [`cf_lexer_init`] /
/// [`cf_lexer_lex`] (its allocations come from the C allocator).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cf_lexer_free(lex: *mut cf_lexer) {
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    unsafe { bfree((*lex).file) };
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    unsafe { bfree((*lex).reformatted) };
    // C `lexer_free`: bfree the text, re-init the base lexer.
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    unsafe { bfree((*lex).base_lexer.text) };
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live and exclusively owned for the call.
    let l = unsafe { &mut *lex };
    l.tokens.free();
    *l = cf_lexer::init();
}

/// `EXPORT bool cf_lexer_lex(struct cf_lexer *lex, const char *str,
/// const char *file)`
///
/// Frees any previous state and re-lexes; NULL/empty text returns false
/// with a zeroed lexer. The tokenizing algorithm is the verified engine
/// ([`CfLexer::lex`], pinned by vectors 10-15); the results are copied into
/// C-owned buffers: `text`/`file` are bstrdup'd, `reformatted` is bmalloc'd
/// and NUL-terminated, token `str` views point into `reformatted` and
/// `unmerged_str` views into `text` (where the C `strref_set` would point).
/// Returns `!unexpected_eof`.
///
/// # Safety
/// `lex` must point to writable memory of size `cf_lexer`; `str` (or NULL)
/// and `file` (or NULL) must point to NUL-terminated strings readable for
/// the whole call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cf_lexer_lex(
    lex: *mut cf_lexer,
    str: *const c_char,
    file: *const c_char,
) -> bool {
    // SAFETY: the struct is a live C object (checked above); the C free is the owner's call.
    unsafe { cf_lexer_free(lex) };
    let Some(text) = c_bytes(str) else {
        return false;
    };
    if text.is_empty() {
        return false;
    }
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live and exclusively owned for the call.
    let l = unsafe { &mut *lex };
    if !file.is_null() {
        // SAFETY: C-ABI — `file` is a valid C string (caller contract); the result is bmem-owned.
        l.file = unsafe { bstrdup(file) };
    }
    // C `lexer_start`: bstrdup the text, offset at the start.
    // SAFETY: C-ABI — `str` is a valid C string (caller contract); the result is bmem-owned.
    l.base_lexer.text = unsafe { bstrdup(str) };
    l.base_lexer.offset = l.base_lexer.text as *const c_char;

    let mut eng = CfLexer::new();
    eng.lex(Some(text), None);

    let rf = &eng.reformatted;
    // SAFETY: C-ABI — S-001 allocation; the result is stored into `l.reformatted` immediately.
    l.reformatted = unsafe { bmalloc_c(rf.len() + 1) as *mut c_char };
    // SAFETY: C-ABI — `l.reformatted` is a live C buffer of at least rf.len()+1 bytes; the write and NUL fit (checked above).
    unsafe {
        ptr::copy_nonoverlapping(rf.as_ptr(), l.reformatted.cast::<u8>(), rf.len());
        l.reformatted.add(rf.len()).write(0);
    };
    // SAFETY: C-ABI — the write cursor sits at the end of the freshly written rf.len() bytes.
    l.write_offset = unsafe { l.reformatted.add(rf.len()) };
    let text_ptr = l.base_lexer.text as *const c_char;
    for t in &eng.tokens {
        l.tokens.push(&cf_token {
            lex: l as *const cf_lexer,
            str: strref {
                // SAFETY: C-ABI — `t.str_off` is within the reformatted buffer (computed above).
                array: unsafe { l.reformatted.add(t.str_off) } as *const c_char,
                len: t.str_len,
            },
            unmerged_str: strref {
                // SAFETY: C-ABI — `t.span_off` is within the live text buffer (computed above).
                array: unsafe { text_ptr.add(t.span_off) },
                len: t.span_len,
            },
            r#type: mat_type(t.r#type),
        });
    }
    // C ends with the base offset at the NUL.
    // SAFETY: C-ABI — the NUL terminator at the end of the live text buffer.
    l.base_lexer.offset = unsafe { text_ptr.add(text.len()) };
    l.unexpected_eof = eng.unexpected_eof;
    !l.unexpected_eof
}

/* ------------------------------------------------------------------------- */
/* EXPORT: cf_preprocessor_init / cf_preprocessor_free */

/// `EXPORT void cf_preprocessor_init(struct cf_preprocessor *pp)`
///
/// # Safety
/// `pp` must point to writable memory of size `cf_preprocessor`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cf_preprocessor_init(pp: *mut cf_preprocessor) {
    // SAFETY: test — the structs/args are live C objects (checked above).
    unsafe { *pp = cf_preprocessor::init() };
}

/// `EXPORT void cf_preprocessor_free(struct cf_preprocessor *pp)`
///
/// Frees every def's token arrays, every sys-include-dir string and every
/// dependency lexer, then the four darrays (C order).
///
/// # Safety
/// `pp` must point to a `cf_preprocessor` set up by [`cf_preprocessor_init`]
/// (its allocations come from the C allocator).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cf_preprocessor_free(pp: *mut cf_preprocessor) {
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live and exclusively owned for the call.
    let p = unsafe { &mut *pp };
    for i in 0..p.defines.num {
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        cf_def_free(unsafe { p.defines.array.add(i) });
    }
    for i in 0..p.sys_include_dirs.num {
        // SAFETY: C-ABI — `i` is a live sys-include dir entry (bmem-owned C string); the preprocessor is the unique owner.
        unsafe { bfree(p.sys_include_dirs.get(i).cast::<u8>()) };
    }
    for i in 0..p.dependencies.num {
        // SAFETY: the struct is a live C object (checked above); the C free is the owner's call.
        unsafe { cf_lexer_free(p.dependencies.array.add(i)) };
    }
    p.defines.free();
    p.sys_include_dirs.free();
    p.dependencies.free();
    p.tokens.free();
    p.lex = ptr::null_mut();
    p.ed = ptr::null_mut();
    p.ignore_state = false;
}

/// C `cf_def_free` (a `static inline` in cf-lexer.h, so the bridge must
/// provide the logic for defs it frees itself).
fn cf_def_free(cfd: *mut cf_def) {
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live and exclusively owned for the call.
    let d = unsafe { &mut *cfd };
    d.name = cf_token::clear();
    d.params.free();
    d.tokens.free();
}

/* ------------------------------------------------------------------------- */
/* the preprocessor (a faithful in-place port of cf-lexer.c) */

/// C `go_to_newline`.
fn go_to_newline(p_cur: &mut *mut cf_token) -> bool {
    let mut cur = *p_cur;
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    while unsafe { (*cur).r#type } != CFTOKEN_NONE && unsafe { (*cur).r#type } != CFTOKEN_NEWLINE {
        // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
        unsafe { cur = cur.add(1) };
    }
    *p_cur = cur;
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    unsafe { (*cur).r#type != CFTOKEN_NONE }
}

/// C `next_token` (the `preprocessor` flag skips SPACETAB runs; the C
/// condition `type == SPACETAB && (preprocessor || type == NEWLINE)` is
/// kept verbatim).
fn next_token(p_cur: &mut *mut cf_token, preprocessor: bool) -> bool {
    let mut cur = *p_cur;
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if unsafe { (*cur).r#type } != CFTOKEN_NONE {
        // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
        unsafe { cur = cur.add(1) };
    }
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    while unsafe { (*cur).r#type } == CFTOKEN_SPACETAB
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        && (preprocessor || unsafe { (*cur).r#type } == CFTOKEN_NEWLINE)
    {
        // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
        unsafe { cur = cur.add(1) };
    }
    *p_cur = cur;
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    unsafe { (*cur).r#type != CFTOKEN_NONE }
}

/// C `cf_addew`: row/col come from the MAIN lexer's text (the reference
/// walks `pp->lex->base_lexer.text` even for dependency tokens), the file
/// from the token's owning lexer; the message is `dstr_safe_printf`'d.
fn cf_addew(
    pp: *mut cf_preprocessor,
    token: *const cf_token,
    message: &[u8],
    level: c_int,
    v1: *const c_char,
    v2: *const c_char,
    v3: *const c_char,
) {
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    let pp = unsafe { &*pp };
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    let token = unsafe { &*token };
    let mut row = 0u32;
    let mut col = 0u32;
    // C `cf_gettokenoffset`: `&pp->lex->base_lexer` — a NULL `pp->lex`
    // dereferences here, like the reference (90-defects pins the crash).
    // SAFETY: C-ABI — `pp` and `token` are live C structs (checked above); the C lexer helper reads a live C string.
    unsafe {
        crate::abi_lexer::lexer_getstroffset(
            &(*pp.lex).base_lexer,
            token.unmerged_str.array,
            &mut row,
            &mut col,
        )
    };
    // SAFETY: C-ABI — `token.lex` is a live base lexer (checked above); the field is a CPOD pointer.
    let file = unsafe { (*token.lex).file };
    let msg = safe_printf(message, v1, v2, v3);
    let mut msgc = msg;
    msgc.push(0);
    // SAFETY: C-ABI — `pp.ed` is a live error data object (checked above); the C string args are valid for the call.
    unsafe { error_data_add_c(pp.ed, file, row, col, msgc.as_ptr().cast::<c_char>(), level) };
}

/// C `append_space` (a synthesized blank; `base`'s span or the filler).
fn append_space(pp: *mut cf_preprocessor, tokens: &mut darray<cf_token>, base: *const cf_token) {
    let mut token = cf_token::clear();
    token.str = strref {
        array: SPACE_FILLER.as_ptr(),
        len: 1,
    };
    token.r#type = CFTOKEN_SPACETAB;
    if !base.is_null() {
        // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
        let b = unsafe { &*base };
        token.lex = b.lex;
        token.unmerged_str = b.unmerged_str;
    } else {
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        token.lex = unsafe { (*pp).lex };
        token.unmerged_str = token.str;
    }
    tokens.push(&token);
}

/// C `append_end_token`.
fn append_end_token(tokens: &mut darray<cf_token>) {
    tokens.push(&cf_token::clear());
}

/// C `get_macro_param`.
fn get_macro_param(params: *const macro_params, name: &strref) -> Option<*const macro_param> {
    if params.is_null() {
        return None;
    }
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    let p = unsafe { &*params };
    for i in 0..p.params.num {
        // SAFETY: C-ABI — i < p.params.num (checked above); the element is a live CPOD.
        let mp = unsafe { &*p.params.array.add(i) };
        if sr_bytes(&mp.name.str) == sr_bytes(name) {
            return Some(mp as *const macro_param);
        }
    }
    None
}

/// C `param_is_whitespace`.
fn param_is_whitespace(p: &macro_param) -> bool {
    for i in 0..p.tokens.num {
        let t = p.tokens.get(i);
        if t.r#type != CFTOKEN_NONE && t.r#type != CFTOKEN_SPACETAB && t.r#type != CFTOKEN_NEWLINE {
            return false;
        }
    }
    true
}

/// C `struct macro_param` / `macro_params` (call-local; freed with the C
/// allocator before return). C names are kept for traceability.
#[allow(non_camel_case_types)]
#[derive(Clone, Copy)]
struct macro_param {
    name: cf_token,
    tokens: darray<cf_token>,
}

#[allow(non_camel_case_types)]
struct macro_params {
    params: darray<macro_param>,
}

fn macro_param_free(p: &mut macro_param) {
    p.name = cf_token::clear();
    p.tokens.free();
}

fn macro_params_free(mp: &mut macro_params) {
    for i in 0..mp.params.num {
        // SAFETY: C-ABI — i < mp.params.num (checked above); the element is a live macro param (unique owner: the preprocessor).
        macro_param_free(unsafe { &mut *mp.params.array.add(i) });
    }
    mp.params.free();
}

/// C `cf_preprocess_get_def_idx` (`INVALID_INDEX` = `usize::MAX`).
fn get_def_idx(pp: *mut cf_preprocessor, name: &strref) -> usize {
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    for i in 0..unsafe { (*pp).defines.num } {
        // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
        let d = unsafe { &*(*pp).defines.array.add(i) };
        if sr_bytes(&d.name.str) == sr_bytes(name) {
            return i;
        }
    }
    usize::MAX
}

/// C `cf_preprocess_get_def`.
fn get_def(pp: *mut cf_preprocessor, name: &strref) -> *const cf_def {
    let i = get_def_idx(pp, name);
    if i == usize::MAX {
        ptr::null()
    } else {
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        unsafe { (*pp).defines.array.add(i) }
    }
}

/// C `cf_preprocess_get_def_value` (recursive; the reference has no
/// visited-set, so a self-referencing define overflows the stack).
fn def_value(pp: *mut cf_preprocessor, name: &strref, value: &mut i64) -> bool {
    let i = get_def_idx(pp, name);
    if i == usize::MAX {
        return false;
    }
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    let d = unsafe { &*(*pp).defines.array.add(i) };
    if d.r#macro {
        return false;
    }
    for j in 0..d.tokens.num {
        let t = d.tokens.get(j);
        if t.r#type == CFTOKEN_SPACETAB {
            continue;
        }
        if t.r#type == CFTOKEN_NUM
            && let Some(v) = c_scan_int(c_bytes(t.str.array).unwrap_or(&[]))
        {
            *value = v;
            return true;
        }
        if t.r#type == CFTOKEN_NAME && def_value(pp, &t.str, value) {
            return true;
        }
        return false;
    }
    false
}

/// C `insert_path`: prefix the directory of the MAIN file.
fn insert_path(pp: *mut cf_preprocessor, path: &mut Vec<u8>) {
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    let main_lex = unsafe { (*pp).lex };
    if main_lex.is_null() {
        return;
    }
    // SAFETY: C-ABI — `main_lex` is live (checked above); the file field is a CPOD C string pointer.
    let Some(file) = c_bytes(unsafe { (*main_lex).file }) else {
        return;
    };
    let Some(slash) = file.iter().rposition(|&b| b == b'/') else {
        return;
    };
    let mut prefix = file[..=slash].to_vec();
    prefix.append(path);
    *path = prefix;
}

/// C `cf_include_file` (Q-7: the open-file error embeds the rest of the
/// buffer at the string token; an already-included file re-runs the
/// preprocessor on its tokens).
fn cf_include_file(pp: *mut cf_preprocessor, file_token: *const cf_token) {
    // SAFETY: C-ABI — `file_token` is non-NULL (guarded above); the token is live for the call.
    let ft = unsafe { &*file_token };
    // `dstr_copy_strref` + `dstr_mid(&str_file, &str_file, 1, len - 2)`:
    // the path is the string token without its surrounding quotes
    // (`len - 2` bytes from index 1).
    let s = sr_bytes(&ft.str);
    let mut path = s[1..s.len() - 1].to_vec();
    insert_path(pp, &mut path);
    let mut path_nul = path.clone();
    path_nul.push(0);

    // If the dependency was already lexed, run the preprocessor on it.
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    for i in 0..unsafe { (*pp).dependencies.num } {
        // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
        let dep = unsafe { &*(*pp).dependencies.array.add(i) };
        if !dep.file.is_null() && c_bytes(dep.file) == Some(&path[..]) {
            let mut tokens = dep.tokens.array;
            cf_preprocess_tokens(pp, false, &mut tokens);
            return;
        }
    }

    // SAFETY: C-ABI — both args are valid NUL-terminated C strings for the call.
    let file = unsafe { os_fopen(path_nul.as_ptr().cast(), c"rb".as_ptr()) };
    if file.is_null() {
        cf_addew(
            pp,
            file_token,
            b"Could not open file '$1'",
            LEX_ERROR,
            ft.str.array,
            ptr::null(),
            ptr::null(),
        );
        return;
    }
    let mut file_data: *mut c_char = ptr::null_mut();
    // SAFETY: C-ABI — `file` is a live FILE* (checked above); `file_data` is valid out storage.
    unsafe { os_fread_utf8(file, &mut file_data) };
    // SAFETY: C-ABI — `file` is the live FILE* opened above.
    unsafe { fclose(file) };

    let mut new_lex = cf_lexer::init();
    // SAFETY: the lexer and out params are live C storage (checked above); `text` is NUL-terminated for the call.
    unsafe { cf_lexer_lex(&mut new_lex, file_data, path_nul.as_ptr().cast::<c_char>()) };
    let mut tokens = new_lex.tokens.array;
    cf_preprocess_tokens(pp, false, &mut tokens);
    // SAFETY: C-ABI — `file_data` is a bmem buffer owned by this scope (checked above).
    unsafe { bfree(file_data) };
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    unsafe { (*pp).dependencies.push(&new_lex) };
}

/// C `cf_preprocess_include` (system includes are accepted and ignored,
/// like the reference's TODO).
fn pp_include(pp: *mut cf_preprocessor, p_cur: &mut *mut cf_token) {
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if unsafe { (*pp).ignore_state } {
        let mut cur = *p_cur;
        go_to_newline(&mut cur);
        *p_cur = cur;
        return;
    }
    let mut cur = *p_cur;
    next_token(&mut cur, true);
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if unsafe { (*cur).r#type } != CFTOKEN_STRING {
        cf_addew(
            pp,
            cur,
            b"Expected $1",
            LEX_ERROR,
            c"string".as_ptr(),
            ptr::null(),
            ptr::null(),
        );
        go_to_newline(&mut cur);
        *p_cur = cur;
        return;
    }
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    let cur_str = unsafe { (*cur).str };
    let s = sr_bytes(&cur_str);
    let sys = s.len() >= 2 && s[0] == b'<' && s[s.len() - 1] == b'>';
    let loc = s.len() >= 2 && s[0] == b'"' && s[s.len() - 1] == b'"';
    if sys {
        // TODO (same as the C reference): system includes.
    } else if loc {
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        if !unsafe { (*pp).ignore_state } {
            cf_include_file(pp, cur);
        }
    } else {
        cf_addew(
            pp,
            cur,
            b"Invalid or incomplete string",
            LEX_ERROR,
            ptr::null(),
            ptr::null(),
            ptr::null(),
        );
        go_to_newline(&mut cur);
        *p_cur = cur;
        return;
    }
    // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
    unsafe { cur = cur.add(1) };
    *p_cur = cur;
}

/// C `cf_preprocess_macro_params`.
fn pp_macro_params(pp: *mut cf_preprocessor, def: &mut cf_def, p_cur: &mut *mut cf_token) -> bool {
    let mut cur = *p_cur;
    def.r#macro = true;
    loop {
        next_token(&mut cur, true);
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        if unsafe { (*cur).r#type } != CFTOKEN_NAME {
            cf_addew(
                pp,
                cur,
                b"Expected $1",
                LEX_ERROR,
                c"identifier".as_ptr(),
                ptr::null(),
                ptr::null(),
            );
            go_to_newline(&mut cur);
            *p_cur = cur;
            return false;
        }
        // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
        def.params.push(unsafe { &*cur });
        next_token(&mut cur, true);
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        if unsafe { (*cur).r#type } != CFTOKEN_OTHER
            // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
            || (unsafe { *(*cur).str.array } != b',' as c_char
                // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
                && unsafe { *(*cur).str.array } != b')' as c_char)
        {
            cf_addew(
                pp,
                cur,
                b"Expected $1",
                LEX_ERROR,
                c"',' or ')'".as_ptr(),
                ptr::null(),
                ptr::null(),
            );
            go_to_newline(&mut cur);
            *p_cur = cur;
            return false;
        }
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        if unsafe { *(*cur).str.array } == b')' as c_char {
            break;
        }
    }
    // Ended properly: go to the first define token (or newline).
    next_token(&mut cur, true);
    *p_cur = cur;
    true
}

/// C `cf_preprocess_define`.
fn pp_define(pp: *mut cf_preprocessor, p_cur: &mut *mut cf_token) {
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if unsafe { (*pp).ignore_state } {
        let mut cur = *p_cur;
        go_to_newline(&mut cur);
        *p_cur = cur;
        return;
    }
    let mut cur = *p_cur;
    let mut def = cf_def {
        name: cf_token::clear(),
        params: darray::init(),
        tokens: darray::init(),
        r#macro: false,
    };
    next_token(&mut cur, true);
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if unsafe { (*cur).r#type } != CFTOKEN_NAME {
        cf_addew(
            pp,
            cur,
            b"Expected $1",
            LEX_ERROR,
            c"identifier".as_ptr(),
            ptr::null(),
            ptr::null(),
        );
        go_to_newline(&mut cur);
        *p_cur = cur;
        return;
    }
    append_space(pp, &mut def.tokens, ptr::null());
    // SAFETY: C-ABI — `cur` is a live token pointer (checked above); CPOD copy.
    def.name = unsafe { *cur };
    if !next_token(&mut cur, true) {
        complete_def(pp, &mut def);
        *p_cur = cur;
        return;
    }
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if unsafe { *(*cur).str.array } == b'(' as c_char && !pp_macro_params(pp, &mut def, &mut cur) {
        cf_def_free(&mut def as *mut cf_def);
        *p_cur = cur;
        return;
    }
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    while unsafe { (*cur).r#type } != CFTOKEN_NEWLINE && unsafe { (*cur).r#type } != CFTOKEN_NONE {
        // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
        def.tokens.push(unsafe { &*cur });
        // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
        unsafe { cur = cur.add(1) };
    }
    complete_def(pp, &mut def);
    *p_cur = cur;
}

fn complete_def(pp: *mut cf_preprocessor, def: &mut cf_def) {
    append_end_token(&mut def.tokens);
    append_space(pp, &mut def.tokens, ptr::null());
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    unsafe { (*pp).defines.push(def) };
}

/// C `cf_preprocess_remove_def_strref`.
fn remove_def_strref(pp: *mut cf_preprocessor, name: &strref) {
    let idx = get_def_idx(pp, name);
    if idx != usize::MAX {
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        cf_def_free(unsafe { (*pp).defines.array.add(idx) });
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        unsafe { (*pp).defines.erase(idx) };
    }
}

/// C `cf_preprocess_undef`.
fn pp_undef(pp: *mut cf_preprocessor, p_cur: &mut *mut cf_token) {
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if unsafe { (*pp).ignore_state } {
        let mut cur = *p_cur;
        go_to_newline(&mut cur);
        *p_cur = cur;
        return;
    }
    let mut cur = *p_cur;
    next_token(&mut cur, true);
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if unsafe { (*cur).r#type } != CFTOKEN_NAME {
        cf_addew(
            pp,
            cur,
            b"Expected $1",
            LEX_ERROR,
            c"identifier".as_ptr(),
            ptr::null(),
            ptr::null(),
        );
        go_to_newline(&mut cur);
        *p_cur = cur;
        return;
    }
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    remove_def_strref(pp, &unsafe { (*cur).str });
    // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
    unsafe { cur = cur.add(1) };
    *p_cur = cur;
}

/// C `cf_preprocess_subblock`.
fn pp_subblock(pp: *mut cf_preprocessor, ignore: bool, p_cur: &mut *mut cf_token) -> bool {
    let mut cur = *p_cur;
    if !next_token(&mut cur, true) {
        *p_cur = cur;
        return false;
    }
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if !unsafe { (*pp).ignore_state } {
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        unsafe { (*pp).ignore_state = ignore };
        cf_preprocess_tokens(pp, true, &mut cur);
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        unsafe { (*pp).ignore_state = false };
    } else {
        cf_preprocess_tokens(pp, true, &mut cur);
    }
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    let eof = unsafe { (*cur).r#type } == CFTOKEN_NONE;
    if eof {
        cf_addew(
            pp,
            cur,
            b"Unexpected end of file before #endif",
            LEX_ERROR,
            ptr::null(),
            ptr::null(),
            ptr::null(),
        );
    }
    *p_cur = cur;
    !eof
}

/// C `cf_preprocess_ifdef` / `cf_preprocess_if` shared tail: `#else` /
/// `#elif` after a taken block.
fn pp_else_elif(pp: *mut cf_preprocessor, is_true: bool, cur: &mut *mut cf_token) -> bool {
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    let cur_str = unsafe { (*(*cur)).str };
    let name = sr_bytes(&cur_str);
    if name == b"else" {
        // C `cf_preprocess_ifdef`/`cf_preprocess_if`: `cur_token++` after a
        // taken #else block (skipped on EOF, like the C `goto exit`).
        let taken = pp_subblock(pp, is_true, cur);
        if taken {
            // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
            unsafe { *cur = (*cur).add(1) };
        }
        taken
    } else if name == b"elif" {
        if is_true {
            let taken = pp_subblock(pp, is_true, cur);
            if taken {
                // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
                unsafe { *cur = (*cur).add(1) };
            }
            taken
        } else {
            // A false #elif re-enters #if at the elif token; `pp_if` owns
            // the cursor from there (C's `goto exit` after `cf_preprocess_if`).
            pp_if(pp, cur);
            false
        }
    } else {
        // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
        unsafe { *cur = (*cur).add(1) };
        true
    }
}

/// C `cf_preprocess_ifdef`.
fn pp_ifdef(pp: *mut cf_preprocessor, ifnot: bool, p_cur: &mut *mut cf_token) {
    let mut cur = *p_cur;
    next_token(&mut cur, true);
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if unsafe { (*cur).r#type } != CFTOKEN_NAME {
        cf_addew(
            pp,
            cur,
            b"Expected $1",
            LEX_ERROR,
            c"identifier".as_ptr(),
            ptr::null(),
            ptr::null(),
        );
        go_to_newline(&mut cur);
        *p_cur = cur;
        return;
    }
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    let is_true = (get_def_idx(pp, &unsafe { (*cur).str }) == usize::MAX) == ifnot;
    if pp_subblock(pp, !is_true, &mut cur) {
        pp_else_elif(pp, is_true, &mut cur);
    }
    *p_cur = cur;
}

/// C `cf_preprocess_if`.
fn pp_if(pp: *mut cf_preprocessor, p_cur: &mut *mut cf_token) {
    let mut cur = *p_cur;
    let mut value_a: i64 = 0;
    let mut value_b: i64 = 0;

    next_token(&mut cur, true);
    let mut accepted = false;
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if unsafe { (*cur).r#type } == CFTOKEN_NAME
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        && def_value(pp, &unsafe { (*cur).str }, &mut value_a)
    {
        accepted = true;
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    } else if unsafe { (*cur).r#type } == CFTOKEN_NUM
        // SAFETY: test — `cur` is a live token (checked above); its str is a live C string.
        && let Some(v) = c_scan_int(c_bytes(unsafe { (*cur).str.array }).unwrap_or(&[]))
    {
        value_a = v;
        accepted = true;
    }
    if !accepted {
        cf_addew(
            pp,
            cur,
            b"Expected $1",
            LEX_ERROR,
            c"identifier or number".as_ptr(),
            ptr::null(),
            ptr::null(),
        );
        go_to_newline(&mut cur);
        *p_cur = cur;
        return;
    }
    next_token(&mut cur, true);
    let is_true;
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if unsafe { (*cur).r#type } == CFTOKEN_NEWLINE {
        is_true = value_a != 0;
        // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
        unsafe { cur = cur.sub(1) };
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    } else if unsafe { (*cur).r#type } != CFTOKEN_OTHER {
        cf_addew(
            pp,
            cur,
            b"Expected $1",
            LEX_ERROR,
            c"comparator".as_ptr(),
            ptr::null(),
            ptr::null(),
        );
        go_to_newline(&mut cur);
        *p_cur = cur;
        return;
    } else {
        // The comparator is the run of consecutive OTHER tokens (e.g. `<=`);
        // the strref spans are contiguous in the reformatted buffer.
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        let mut comparator = unsafe { (*cur).str };
        // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
        unsafe { cur = cur.add(1) };
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        while unsafe { (*cur).r#type } == CFTOKEN_OTHER {
            // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
            comparator.len += unsafe { (*cur).str.len };
            // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
            unsafe { cur = cur.add(1) };
        }
        // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
        unsafe { cur = cur.sub(1) };
        next_token(&mut cur, true);
        let mut accepted = false;
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        if unsafe { (*cur).r#type } == CFTOKEN_NAME
            // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
            && def_value(pp, &unsafe { (*cur).str }, &mut value_b)
        {
            accepted = true;
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        } else if unsafe { (*cur).r#type } == CFTOKEN_NUM
            // SAFETY: test — `cur` is a live token (checked above); its str is a live C string.
            && let Some(v) = c_scan_int(c_bytes(unsafe { (*cur).str.array }).unwrap_or(&[]))
        {
            value_b = v;
            accepted = true;
        }
        if !accepted {
            cf_addew(
                pp,
                cur,
                b"Expected $1",
                LEX_ERROR,
                c"identifier or number".as_ptr(),
                ptr::null(),
                ptr::null(),
            );
            go_to_newline(&mut cur);
            *p_cur = cur;
            return;
        }
        let comp = sr_bytes(&comparator);
        is_true = match comp {
            b"==" => value_a == value_b,
            b">=" => value_a >= value_b,
            b"<=" => value_a <= value_b,
            b"!=" => value_a != value_b,
            b"<" => value_a < value_b,
            b">" => value_a > value_b,
            _ => {
                cf_addew(
                    pp,
                    cur,
                    b"Expected $1",
                    LEX_ERROR,
                    c"comparator".as_ptr(),
                    ptr::null(),
                    ptr::null(),
                );
                go_to_newline(&mut cur);
                *p_cur = cur;
                return;
            }
        };
    }

    if pp_subblock(pp, !is_true, &mut cur) {
        pp_else_elif(pp, is_true, &mut cur);
    }
    *p_cur = cur;
}

/// C `cf_preprocessor`: the directive dispatcher. Returns false to end the
/// current conditional block (an `#else`/`#elif`/`#endif` inside a block).
fn pp_dispatch(pp: *mut cf_preprocessor, if_block: bool, p_cur: &mut *mut cf_token) -> bool {
    let cur = *p_cur;
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    let cur_str = unsafe { (*cur).str };
    let name = sr_bytes(&cur_str);
    if name == b"include" {
        pp_include(pp, p_cur);
    } else if name == b"define" {
        pp_define(pp, p_cur);
    } else if name == b"undef" {
        pp_undef(pp, p_cur);
    } else if name == b"ifdef" {
        pp_ifdef(pp, false, p_cur);
    } else if name == b"ifndef" {
        pp_ifdef(pp, true, p_cur);
    } else if name == b"if" {
        pp_if(pp, p_cur);
    } else if name == b"else" || name == b"elif" || name == b"endif" {
        if !if_block {
            let mut name_c = name.to_vec();
            name_c.push(0);
            cf_addew(
                pp,
                cur,
                b"#$1 outside of #if/#ifdef/#ifndef block",
                LEX_ERROR,
                name_c.as_ptr().cast::<c_char>(),
                ptr::null(),
                ptr::null(),
            );
            // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
            unsafe { *p_cur = cur.add(1) };
            return true;
        }
        return false;
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    } else if unsafe { (*cur).r#type } != CFTOKEN_NEWLINE
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        && unsafe { (*cur).r#type } != CFTOKEN_NONE
    {
        // Language-specific preprocessor stuff (TODO, same as the
        // reference): skip the directive name.
        // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
        unsafe { *p_cur = cur.add(1) };
    }
    true
}

/// C `cf_preprocess_save_macro_param` (stops at the terminating NONE; an
/// unterminated parameter is the "Unexpected end of file" error).
fn save_macro_param(
    pp: *mut cf_preprocessor,
    p_cur: &mut *mut cf_token,
    param: &mut macro_param,
    base: *const cf_token,
    cur_params: *const macro_params,
) {
    let mut cur = *p_cur;
    append_space(pp, &mut param.tokens, base);
    let mut brace = 0;
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    while unsafe { (*cur).r#type } != CFTOKEN_NONE {
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        let first = unsafe { *(*cur).str.array };
        if first == b'(' as c_char {
            brace += 1;
        } else if first == b')' as c_char {
            if brace != 0 {
                brace -= 1;
            } else {
                break;
            }
        } else if first == b',' as c_char && brace == 0 {
            break;
        }
        pp_addtoken(pp, &mut param.tokens, &mut cur, base, cur_params);
    }
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if unsafe { (*cur).r#type } == CFTOKEN_NONE {
        cf_addew(
            pp,
            cur,
            b"Unexpected end of file",
            LEX_ERROR,
            ptr::null(),
            ptr::null(),
            ptr::null(),
        );
    }
    append_space(pp, &mut param.tokens, base);
    append_end_token(&mut param.tokens);
    *p_cur = cur;
}

/// C `cf_preprocess_save_macro_params` (the zero-parameter call quirk: an
/// empty first argument before `)` is ignored).
fn save_macro_params(
    pp: *mut cf_preprocessor,
    p_cur: &mut *mut cf_token,
    def: &cf_def,
    base: *const cf_token,
    cur_params: *const macro_params,
    dst: &mut macro_params,
) {
    let mut cur = *p_cur;
    let mut count: usize = 0;
    next_token(&mut cur, false);
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if unsafe { (*cur).r#type } != CFTOKEN_OTHER || unsafe { *(*cur).str.array } != b'(' as c_char {
        cf_addew(
            pp,
            cur,
            b"Expected $1",
            LEX_ERROR,
            c"'('".as_ptr(),
            ptr::null(),
            ptr::null(),
        );
        *p_cur = cur;
        return;
    }
    loop {
        let mut param = macro_param {
            name: cf_token::clear(),
            tokens: darray::init(),
        };
        // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
        unsafe { cur = cur.add(1) };
        count += 1;
        save_macro_param(pp, &mut cur, &mut param, base, cur_params);
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        if unsafe { (*cur).r#type } != CFTOKEN_OTHER
            // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
            || (unsafe { *(*cur).str.array } != b',' as c_char
                // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
                && unsafe { *(*cur).str.array } != b')' as c_char)
        {
            macro_param_free(&mut param);
            cf_addew(
                pp,
                cur,
                b"Expected $1",
                LEX_ERROR,
                c"',' or ')'".as_ptr(),
                ptr::null(),
                ptr::null(),
            );
            *p_cur = cur;
            return;
        }
        if param_is_whitespace(&param) {
            // If 0-param macro, ignore first entry.
            // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
            if count == 1 && def.params.num == 0 && unsafe { *(*cur).str.array } == b')' as c_char {
                macro_param_free(&mut param);
                break;
            }
        }
        if count <= def.params.num {
            param.name = *def.params.get(count - 1);
            dst.params.push(&param);
        } else {
            macro_param_free(&mut param);
        }
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        if unsafe { *(*cur).str.array } == b')' as c_char {
            break;
        }
    }
    if count != def.params.num {
        cf_addew(
            pp,
            cur,
            b"Mismatching number of macro parameters",
            LEX_ERROR,
            ptr::null(),
            ptr::null(),
            ptr::null(),
        );
    }
    *p_cur = cur;
}

/// C `cf_preprocess_unwrap_param`.
fn unwrap_param(
    pp: *mut cf_preprocessor,
    dst: &mut darray<cf_token>,
    p_cur: &mut *mut cf_token,
    base: *const cf_token,
    param: *const macro_param,
) {
    let mut cur = *p_cur;
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    let p = unsafe { &*param };
    let mut param_cur = p.tokens.array;
    // SAFETY: C-ABI — `param_cur` is a live token cursor (checked above); CPOD field read.
    while unsafe { (*param_cur).r#type } != CFTOKEN_NONE {
        pp_addtoken(pp, dst, &mut param_cur, base, ptr::null());
    }
    // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
    unsafe { cur = cur.add(1) };
    *p_cur = cur;
}

/// C `cf_preprocess_unwrap_define` (an empty def body dereferences a NULL
/// body pointer, like the reference — 90-defects pins the crash).
fn unwrap_define(
    pp: *mut cf_preprocessor,
    dst: &mut darray<cf_token>,
    p_cur: &mut *mut cf_token,
    base: *const cf_token,
    def: &cf_def,
    cur_params: *const macro_params,
) {
    let mut cur = *p_cur;
    let mut new_params = macro_params {
        params: darray::init(),
    };
    if def.r#macro {
        save_macro_params(pp, &mut cur, def, base, cur_params, &mut new_params);
    }
    let mut body = def.tokens.array;
    // SAFETY: C-ABI — `body` is a live token cursor (checked above); CPOD field read.
    while unsafe { (*body).r#type } != CFTOKEN_NONE {
        pp_addtoken(pp, dst, &mut body, base, &new_params);
    }
    macro_params_free(&mut new_params);
    // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
    unsafe { cur = cur.add(1) };
    *p_cur = cur;
}

/// C `cf_preprocess_addtoken` (recursive; self-referencing macros overflow
/// the stack, like the reference — 90-defects pins the crash).
fn pp_addtoken(
    pp: *mut cf_preprocessor,
    dst: &mut darray<cf_token>,
    p_cur: &mut *mut cf_token,
    base: *const cf_token,
    params: *const macro_params,
) {
    let mut cur = *p_cur;
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if unsafe { (*pp).ignore_state } {
        // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
        unsafe { cur = cur.add(1) };
        *p_cur = cur;
        return;
    }
    let base = if base.is_null() { cur } else { base };
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    if unsafe { (*cur).r#type } == CFTOKEN_NAME {
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        if let Some(param) = get_macro_param(params, &unsafe { (*cur).str }) {
            unwrap_param(pp, dst, &mut cur, base, param);
            *p_cur = cur;
            return;
        }
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        let def = get_def(pp, &unsafe { (*cur).str });
        if !def.is_null() {
            // Copy the def to the stack: expansion only reads the body
            // (the darray's heap) and pushes into `dst`; the `defines`
            // darray is never resized during expansion (only the
            // #define/#undef directives resize it, and those run in the
            // block loop, not here).
            // SAFETY: C-ABI — `def` is a live cf_def (checked above); CPOD copy.
            let def = unsafe { *def };
            unwrap_define(pp, dst, &mut cur, base, &def, params);
            *p_cur = cur;
            return;
        }
    }
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    dst.push(unsafe { &*cur });
    // SAFETY: pointer arithmetic on the live C token cursor (checked above); the step stays inside the token array.
    unsafe { cur = cur.add(1) };
    *p_cur = cur;
}

/// C `cf_preprocess_tokens`: the block loop.
fn cf_preprocess_tokens(pp: *mut cf_preprocessor, if_block: bool, p_cur: &mut *mut cf_token) {
    let mut cur = *p_cur;
    let mut newline = true;
    let mut preprocessor_line = if_block;
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    while unsafe { (*cur).r#type } != CFTOKEN_NONE {
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        if unsafe { (*cur).r#type } != CFTOKEN_SPACETAB
            // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
            && unsafe { (*cur).r#type } != CFTOKEN_NEWLINE
        {
            if preprocessor_line {
                cf_addew(
                    pp,
                    cur,
                    b"Unexpected token after preprocessor, expected newline",
                    LEX_ERROR,
                    ptr::null(),
                    ptr::null(),
                    ptr::null(),
                );
                if !go_to_newline(&mut cur) {
                    break;
                }
            }
            // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
            if newline && unsafe { *(*cur).str.array } == b'#' as c_char {
                next_token(&mut cur, true);
                preprocessor_line = true;
                if !pp_dispatch(pp, if_block, &mut cur) {
                    break;
                }
                continue;
            }
            newline = false;
        }
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        if unsafe { (*cur).r#type } == CFTOKEN_NEWLINE {
            newline = true;
            preprocessor_line = false;
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        } else if unsafe { (*cur).r#type } == CFTOKEN_NONE {
            break;
        }
        pp_addtoken(
            pp,
            // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
            unsafe { &mut (*pp).tokens },
            &mut cur,
            ptr::null(),
            ptr::null(),
        );
    }
    *p_cur = cur;
}

/* ------------------------------------------------------------------------- */
/* EXPORT: cf_preprocess / add_def / remove_def */

/// `EXPORT bool cf_preprocess(struct cf_preprocessor *pp, struct cf_lexer
/// *lex, struct error_data *ed)`
///
/// Runs the block loop over `lex`'s tokens (which must end in the NONE
/// terminator `cf_lexer_lex` appends) and appends the result — including
/// that terminator — to `pp->tokens`; a second call appends again, like
/// the reference. Returns false for an empty token list, otherwise
/// `!lex->unexpected_eof`.
///
/// # Safety
/// `pp` must point to a `cf_preprocessor` from [`cf_preprocessor_init`];
/// `lex` (or NULL) must point to a lexed `cf_lexer` (NULL is a no-op
/// false, like the reference's NULL token list); `ed` (or NULL) must point
/// to a valid `error_data` (a NULL one is a no-op).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cf_preprocess(
    pp: *mut cf_preprocessor,
    lex: *mut cf_lexer,
    ed: *mut error_data,
) -> bool {
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    let tokens = unsafe { (*lex).tokens.array };
    if tokens.is_null() {
        return false;
    }
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    unsafe {
        (*pp).ed = ed;
        (*pp).lex = lex;
    };
    let mut cur = tokens;
    cf_preprocess_tokens(pp, false, &mut cur);
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    unsafe { (*pp).tokens.push(&*cur) };
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    !unsafe { (*lex).unexpected_eof }
}

/// `EXPORT void cf_preprocessor_add_def(struct cf_preprocessor *pp,
/// struct cf_def *def)`
///
/// Adds the def or replaces an existing one with the same name (the two
/// reference-position warnings, then `cf_def_free` + copy). The def's token
/// views point into the caller's lexers, which must outlive the
/// preprocessor (the reference keeps the pointers verbatim).
///
/// # Safety
/// `pp` must point to a `cf_preprocessor` from [`cf_preprocessor_init`];
/// `def` must point to a valid `cf_def` whose token views are readable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cf_preprocessor_add_def(pp: *mut cf_preprocessor, def: *mut cf_def) {
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    let d = unsafe { &*def };
    let existing = get_def(pp, &d.name.str);
    if !existing.is_null() {
        let mut name_c = sr_bytes(&d.name.str).to_vec();
        name_c.push(0);
        let name_p = name_c.as_ptr() as *const c_char;
        cf_addew(
            pp,
            &d.name as *const cf_token,
            b"Token $1 already defined",
            LEX_WARNING,
            name_p,
            ptr::null(),
            ptr::null(),
        );
        cf_addew(
            pp,
            // SAFETY: C-ABI — `existing` is a live cf_def (checked above); the name field is a CPOD.
            unsafe { &(*existing).name } as *const cf_token,
            b"Previous definition of $1 is here",
            LEX_WARNING,
            name_p,
            ptr::null(),
            ptr::null(),
        );
        cf_def_free(existing as *mut cf_def);
        // SAFETY: C-ABI — `def` and `existing` are live, disjoint cf_def objects (checked above); CPOD.
        unsafe { ptr::copy_nonoverlapping(def, existing as *mut cf_def, 1) };
    } else {
        // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
        unsafe { (*pp).defines.push(d) };
    }
}

/// `EXPORT void cf_preprocessor_remove_def(struct cf_preprocessor *pp,
/// const char *def_name)`
///
/// Removes the first def with the given name; a missing name is a no-op.
///
/// # Safety
/// `pp` must point to a `cf_preprocessor` from [`cf_preprocessor_init`];
/// `def_name` (or NULL) must point to a NUL-terminated string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cf_preprocessor_remove_def(
    pp: *mut cf_preprocessor,
    def_name: *const c_char,
) {
    let Some(name) = c_bytes(def_name) else {
        return;
    };
    let r = strref {
        array: def_name,
        len: name.len(),
    };
    remove_def_strref(pp, &r);
}

/* ------------------------------------------------------------------------- */

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;

    // Test stand-ins for the platform file API: `cargo test` links these
    // instead of the platform stubs (the swap build conformance run links
    // those for real). The allocator stand-ins are gone — the real bmem
    // bridge (`abi_bmem`, P1-01.3) provides `bmalloc`/`bfree` under the
    // same `cfg(test)` gate.

    #[unsafe(no_mangle)]
    pub extern "C" fn os_fopen(_path: *const c_char, _mode: *const c_char) -> *mut c_void {
        ptr::null_mut()
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn os_fread_utf8(_file: *mut c_void, pstr: *mut *mut c_char) -> usize {
        // SAFETY: `pstr` is valid out storage (the caller's C pointer slot, documented above).
        unsafe { pstr.write(ptr::null_mut()) };
        0
    }

    #[unsafe(no_mangle)]
    pub extern "C" fn fclose(_file: *mut c_void) -> c_int {
        0
    }

    fn cstr(s: &str) -> CString {
        CString::new(s).unwrap()
    }

    fn cbuf(p: *const c_char) -> Vec<u8> {
        c_bytes(p).unwrap_or_default().to_vec()
    }

    fn tok_bytes(r: &strref) -> Vec<u8> {
        sr_bytes(r).to_vec()
    }

    #[test]
    fn layout_matches_c() {
        // cf_token { const cf_lexer *; strref; strref; enum(int); }
        assert_eq!(size_of::<cf_token>(), 48);
        assert_eq!(std::mem::offset_of!(cf_token, str), 8);
        assert_eq!(std::mem::offset_of!(cf_token, unmerged_str), 24);
        assert_eq!(std::mem::offset_of!(cf_token, r#type), 40);
        // cf_lexer { char *; struct lexer; char *; char *; DARRAY; bool; }
        assert_eq!(size_of::<cf_lexer>(), 72);
        assert_eq!(std::mem::offset_of!(cf_lexer, base_lexer), 8);
        assert_eq!(std::mem::offset_of!(cf_lexer, reformatted), 24);
        assert_eq!(std::mem::offset_of!(cf_lexer, write_offset), 32);
        assert_eq!(std::mem::offset_of!(cf_lexer, tokens.num), 48);
        assert_eq!(std::mem::offset_of!(cf_lexer, tokens.capacity), 56);
        assert_eq!(std::mem::offset_of!(cf_lexer, unexpected_eof), 64);
        // cf_def { cf_token; DARRAY; DARRAY; bool; }
        assert_eq!(size_of::<cf_def>(), 104);
        assert_eq!(std::mem::offset_of!(cf_def, params), 48);
        assert_eq!(std::mem::offset_of!(cf_def, tokens), 72);
        assert_eq!(std::mem::offset_of!(cf_def, r#macro), 96);
        // cf_preprocessor { cf_lexer *; error_data *; 4x DARRAY; bool; }
        assert_eq!(size_of::<cf_preprocessor>(), 120);
        assert_eq!(std::mem::offset_of!(cf_preprocessor, defines), 16);
        assert_eq!(std::mem::offset_of!(cf_preprocessor, sys_include_dirs), 40);
        assert_eq!(std::mem::offset_of!(cf_preprocessor, dependencies), 64);
        assert_eq!(std::mem::offset_of!(cf_preprocessor, tokens), 88);
        assert_eq!(std::mem::offset_of!(cf_preprocessor, ignore_state), 112);
    }

    #[test]
    fn c_scan_int_semantics() {
        // Q-14: sscanf("%i") over the rest of the buffer
        assert_eq!(c_scan_int(b"1.5"), Some(1));
        assert_eq!(c_scan_int(b"08"), Some(0));
        assert_eq!(c_scan_int(b"0x10"), Some(16));
        assert_eq!(c_scan_int(b"017"), Some(15));
        assert_eq!(c_scan_int(b"-12"), Some(-12));
        assert_eq!(c_scan_int(b"  +7"), Some(7));
        assert_eq!(c_scan_int(b"0"), Some(0));
        assert_eq!(c_scan_int(b".5"), None);
        assert_eq!(c_scan_int(b"abc"), None);
        assert_eq!(c_scan_int(b"0x"), Some(0));
        assert_eq!(c_scan_int(b"09"), Some(0));
        assert_eq!(c_scan_int(b"0xG10"), Some(0));
        assert_eq!(c_scan_int(b"+"), None);
        assert_eq!(c_scan_int(b"  "), None);
    }

    #[test]
    fn cf_lexer_lex_produces_c_tokens() {
        let mut l = cf_lexer::init();
        let text = cstr("a /* b */ c\nd\ne");
        let file = cstr("f.cf");
        // SAFETY: the lexer and out params are live C storage (checked above); `text` is NUL-terminated for the call.
        let r = unsafe { cf_lexer_lex(&mut l, text.as_ptr(), file.as_ptr()) };
        assert!(r);
        assert!(!l.unexpected_eof);
        assert_eq!(l.tokens.num, 8);
        // NAME a / SPACETAB "   " (pre-space + comment space + post-space
        // merged) / NAME c /
        // NEWLINE / NAME d / NEWLINE / NAME e / NONE
        let types: Vec<c_int> = (0..l.tokens.num).map(|i| l.tokens.get(i).r#type).collect();
        assert_eq!(
            types,
            [
                CFTOKEN_NAME,
                CFTOKEN_SPACETAB,
                CFTOKEN_NAME,
                CFTOKEN_NEWLINE,
                CFTOKEN_NAME,
                CFTOKEN_NEWLINE,
                CFTOKEN_NAME,
                CFTOKEN_NONE
            ]
        );
        assert_eq!(tok_bytes(&l.tokens.get(0).str), b"a");
        assert_eq!(tok_bytes(&l.tokens.get(1).str), b"   ");
        assert_eq!(tok_bytes(&l.tokens.get(1).unmerged_str), b" /* b */ ");
        // the final NONE terminator sits at the NUL positions
        assert_eq!(tok_bytes(&l.tokens.get(7).str), b"");
        // the reformatted buffer is NUL-terminated in C form
        assert_eq!(cbuf(l.reformatted), b"a   c\nd\ne");
        assert_eq!(cbuf(l.file), b"f.cf");
        // token->lex points back at the C struct (C `token.lex = lex`)
        for i in 0..l.tokens.num {
            assert_eq!(
                l.tokens.get(i).lex as *mut cf_lexer,
                &l as *const cf_lexer as *mut cf_lexer
            );
        }
        // the base offset walked to the NUL
        // SAFETY: C-ABI — both pointers are within the same live text buffer.
        let rel = unsafe { l.base_lexer.offset.offset_from(l.base_lexer.text) };
        assert_eq!(rel, text.as_bytes().len() as isize);
        // SAFETY: the struct is a live C object (checked above); the C free is the owner's call.
        unsafe { cf_lexer_free(&mut l) };
        assert!(l.file.is_null());
        assert!(l.reformatted.is_null());
        assert!(l.base_lexer.text.is_null());
        assert!(l.tokens.array.is_null());
    }

    #[test]
    fn cf_lexer_lex_unterminated_comment_and_splice() {
        let mut l = cf_lexer::init();
        let text = cstr("a /* x");
        // SAFETY: the lexer and out params are live C storage (checked above); `text` is NUL-terminated for the call.
        let r = unsafe { cf_lexer_lex(&mut l, text.as_ptr(), ptr::null()) };
        assert!(!r);
        assert!(l.unexpected_eof);
        assert_eq!(l.tokens.num, 3);
        assert_eq!(l.tokens.get(0).r#type, CFTOKEN_NAME);
        assert_eq!(tok_bytes(&l.tokens.get(1).str), b"  ");
        assert_eq!(tok_bytes(&l.tokens.get(1).unmerged_str), b" /* x");
        assert!(l.file.is_null());
        // SAFETY: the struct is a live C object (checked above); the C free is the owner's call.
        unsafe { cf_lexer_free(&mut l) };

        // a splice merges the lines: str "abcd", span keeps the backslash
        let text = cstr("ab\\\ncd");
        // SAFETY: the lexer and out params are live C storage (checked above); `text` is NUL-terminated for the call.
        assert!(unsafe { cf_lexer_lex(&mut l, text.as_ptr(), ptr::null()) });
        assert_eq!(l.tokens.num, 2);
        assert_eq!(tok_bytes(&l.tokens.get(0).str), b"abcd");
        assert_eq!(tok_bytes(&l.tokens.get(0).unmerged_str), b"ab\\\ncd");
        // SAFETY: the struct is a live C object (checked above); the C free is the owner's call.
        unsafe { cf_lexer_free(&mut l) };

        // empty text: false, zeroed lexer
        let text = cstr("");
        // SAFETY: the lexer and out params are live C storage (checked above); `text` is NUL-terminated for the call.
        assert!(!unsafe { cf_lexer_lex(&mut l, text.as_ptr(), ptr::null()) });
        assert_eq!(l.tokens.num, 0);
        assert!(l.reformatted.is_null());
    }

    #[test]
    fn cf_preprocess_expands_directive_defs() {
        // vector 20 case 1: #define A 1 / x A y / A
        let mut l = cf_lexer::init();
        let mut pp = cf_preprocessor::init();
        let mut ed = error_data {
            errors: crate::abi_lexer::darray_error_item {
                array: ptr::null_mut(),
                num: 0,
                capacity: 0,
            },
        };
        let text = cstr("#define A 1\nx A y\nA\n");
        // SAFETY: the lexer and out params are live C storage (checked above); `text` is NUL-terminated for the call.
        assert!(unsafe { cf_lexer_lex(&mut l, text.as_ptr(), ptr::null()) });
        // SAFETY: test — the structs/args are live C objects (checked above).
        assert!(unsafe { cf_preprocess(&mut pp, &mut l, &mut ed) });
        assert_eq!(pp.tokens.num, 12);
        let types: Vec<c_int> = (0..pp.tokens.num)
            .map(|i| pp.tokens.get(i).r#type)
            .collect();
        assert_eq!(
            types,
            [
                CFTOKEN_NEWLINE,
                CFTOKEN_NAME,
                CFTOKEN_SPACETAB,
                CFTOKEN_SPACETAB,
                CFTOKEN_NUM,
                CFTOKEN_SPACETAB,
                CFTOKEN_NAME,
                CFTOKEN_NEWLINE,
                CFTOKEN_SPACETAB,
                CFTOKEN_NUM,
                CFTOKEN_NEWLINE,
                CFTOKEN_NONE
            ]
        );
        assert_eq!(tok_bytes(&pp.tokens.get(1).str), b"x");
        // the leading body blank is synthesized (space filler)
        assert_eq!(tok_bytes(&pp.tokens.get(3).str), b" ");
        // the expanded NUM comes from the #define line
        assert_eq!(tok_bytes(&pp.tokens.get(4).str), b"1");
        assert_eq!(tok_bytes(&pp.tokens.get(4).unmerged_str), b"1");
        assert_eq!(tok_bytes(&pp.tokens.get(6).str), b"y");
        assert_eq!(ed.errors.num, 0);
        // SAFETY: the struct is a live C object (checked above); the C free is the owner's call.
        unsafe { cf_preprocessor_free(&mut pp) };
        // SAFETY: the struct is a live C object (checked above); the C free is the owner's call.
        unsafe { cf_lexer_free(&mut l) };
        assert!(pp.defines.array.is_null());
        assert!(pp.tokens.array.is_null());
    }

    #[test]
    fn cf_preprocessor_api_defs_and_undef() {
        // vector 26 flow: API defs from side lexers (name/body), #ifdef
        // sees them, undef removes the first match.
        let mut l = cf_lexer::init();
        let mut pp = cf_preprocessor::init();
        let mut ed = error_data {
            errors: crate::abi_lexer::darray_error_item {
                array: ptr::null_mut(),
                num: 0,
                capacity: 0,
            },
        };
        let text = cstr("FOO BAR FOO\n#ifdef FOO\ny\n#endif\n");
        // SAFETY: the lexer and out params are live C storage (checked above); `text` is NUL-terminated for the call.
        assert!(unsafe { cf_lexer_lex(&mut l, text.as_ptr(), ptr::null()) });

        let name_c = cstr("FOO");
        let file_name = cstr("def-name");
        let body_c = cstr("7");
        let file_body = cstr("def-body");
        let mut ln = cf_lexer::init();
        // SAFETY: the lexer and out params are live C storage (checked above); `text` is NUL-terminated for the call.
        assert!(unsafe { cf_lexer_lex(&mut ln, name_c.as_ptr(), file_name.as_ptr()) });
        let mut lb = cf_lexer::init();
        // SAFETY: the lexer and out params are live C storage (checked above); `text` is NUL-terminated for the call.
        assert!(unsafe { cf_lexer_lex(&mut lb, body_c.as_ptr(), file_body.as_ptr()) });

        let mut def = cf_def {
            name: *ln.tokens.get(0),
            params: darray::init(),
            tokens: darray::init(),
            r#macro: false,
        };
        for i in 0..lb.tokens.num {
            def.tokens.push(lb.tokens.get(i));
        }
        // SAFETY: test — the structs/args are live C objects (checked above).
        unsafe { cf_preprocessor_add_def(&mut pp, &mut def) };
        assert_eq!(pp.defines.num, 1);

        // SAFETY: test — the structs/args are live C objects (checked above).
        assert!(unsafe { cf_preprocess(&mut pp, &mut l, &mut ed) });
        // NUM 7 / SPACETAB / BAR / SPACETAB / NUM 7 / NEWLINE / NEWLINE
        // (the #ifdef line's, kept like the C reference) / y / NEWLINE /
        // NEWLINE (the #endif line's) / NONE
        assert_eq!(pp.tokens.num, 11);
        assert_eq!(tok_bytes(&pp.tokens.get(0).str), b"7");
        assert_eq!(tok_bytes(&pp.tokens.get(2).str), b"BAR");
        assert_eq!(tok_bytes(&pp.tokens.get(4).str), b"7");
        assert_eq!(tok_bytes(&pp.tokens.get(7).str), b"y");
        assert_eq!(ed.errors.num, 0);

        let f = cstr("FOO");
        // SAFETY: test — the structs/args are live C objects (checked above).
        unsafe { cf_preprocessor_remove_def(&mut pp, f.as_ptr()) };
        assert_eq!(pp.defines.num, 0);
        let n = cstr("Nothing");
        // SAFETY: test — the structs/args are live C objects (checked above).
        unsafe { cf_preprocessor_remove_def(&mut pp, n.as_ptr()) };
        assert_eq!(pp.defines.num, 0);

        // SAFETY: the struct is a live C object (checked above); the C free is the owner's call.
        unsafe { cf_lexer_free(&mut ln) };
        // SAFETY: the struct is a live C object (checked above); the C free is the owner's call.
        unsafe { cf_lexer_free(&mut lb) };
        // SAFETY: the struct is a live C object (checked above); the C free is the owner's call.
        unsafe { cf_preprocessor_free(&mut pp) };
        // SAFETY: the struct is a live C object (checked above); the C free is the owner's call.
        unsafe { cf_lexer_free(&mut l) };
    }

    #[test]
    fn cf_literal_to_str_bridged() {
        // vector 33 cases through the C entry point (result is C-allocated)
        let s = cstr("\"abc\"");
        // SAFETY: the literal bytes are valid for the call; the result is a C-owned pointer (doc above).
        let r = unsafe { cf_literal_to_str(s.as_ptr(), 0) };
        assert!(!r.is_null());
        assert_eq!(cbuf(r), b"abc");
        // SAFETY: `r` came from the cf_literal_to_str above; freed once here.
        unsafe { bfree(r) };

        // the Q-10 quirk: the closing quote leaks after \x
        let s = cstr("'\\x41'");
        // SAFETY: the literal bytes are valid for the call; the result is a C-owned pointer (doc above).
        let r = unsafe { cf_literal_to_str(s.as_ptr(), 0) };
        assert_eq!(cbuf(r), b"A'");
        // SAFETY: `r` came from the cf_literal_to_str above; freed once here.
        unsafe { bfree(r) };

        // explicit count, and rejection cases
        let s = cstr("\"abc\"");
        // SAFETY: the literal bytes are valid for the call; the result is a C-owned pointer (doc above).
        let r = unsafe { cf_literal_to_str(s.as_ptr(), 5) };
        assert_eq!(cbuf(r), b"abc");
        // SAFETY: `r` came from the cf_literal_to_str above; freed once here.
        unsafe { bfree(r) };
        let bad = cstr("\"abc'");
        // SAFETY: the literal bytes are valid for the call; the result is a C-owned pointer (doc above).
        assert!(unsafe { cf_literal_to_str(bad.as_ptr(), 0) }.is_null());
        let short = cstr("\"a\"");
        // SAFETY: the literal bytes are valid for the call; the result is a C-owned pointer (doc above).
        assert!(unsafe { cf_literal_to_str(short.as_ptr(), 2) }.is_null());
    }
}
