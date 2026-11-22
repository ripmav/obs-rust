//! P1-08.5 recipe step 2: the C ABI for the base lexer
//! (`libobs/util/lexer.h`).
//!
//! The ten `EXPORT` functions with the exact C signatures; the `static
//! inline` helpers (`strref_set`, `lexer_start`, `error_data_free`, ...)
//! stay C-side — the conformance runners and the C cf-lexer/cf-parser call
//! them directly.
//!
//! `struct lexer` is a C-owned struct: the inlines duplicate the text with
//! the C allocator (`lexer_start` = `bstrdup`), so the tokenizing logic
//! runs over the C buffer and the bridge only reads/updates the fields in
//! place. Allocation contract (enforced in [`crate::abi_common`]):
//! everything C later `bfree`s comes from the C allocator —
//! `error_data_add` grows the darray with `bmalloc` and `bstrdup`s the
//! message, `error_data_buildstring` returns a `bstrdup`'d string;
//! `strref_cmp*` allocate nothing and the token `text` spans point into
//! the C-owned buffer (the NUL invariant of `struct lexer` holds
//! unchanged).

use std::ffi::CStr;
use std::os::raw::{c_char, c_int};

use obs_util::lexer::{BaseTokenType, StrRef};

use crate::abi_common::{c_bytes, error_data_add_c, error_data_buildstring_c};

/* ------------------------------------------------------------------------- */
/* C struct mirrors (exact field layout, C owns the memory) */

/// C `struct strref`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct strref {
    pub array: *const c_char,
    pub len: usize,
}

/// C `enum base_token_type` values (an `int` in C).
pub const BASETOKEN_NONE: c_int = 0;
/// `BASETOKEN_ALPHA`.
pub const BASETOKEN_ALPHA: c_int = 1;
/// `BASETOKEN_DIGIT`.
pub const BASETOKEN_DIGIT: c_int = 2;
/// `BASETOKEN_WHITESPACE`.
pub const BASETOKEN_WHITESPACE: c_int = 3;
/// `BASETOKEN_OTHER`.
pub const BASETOKEN_OTHER: c_int = 4;

/// C `struct base_token`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct base_token {
    pub text: strref,
    /// `enum base_token_type` (an `int` in C): a `BASETOKEN_*` value.
    pub r#type: c_int,
    pub passed_whitespace: bool,
}

/// C `struct error_item`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct error_item {
    pub error: *mut c_char,
    pub file: *const c_char,
    pub row: u32,
    pub column: u32,
    pub level: c_int,
}

/// C `DARRAY(struct error_item)` — the anonymous-struct view of the
/// `struct darray` union: `{ T *array; size_t num; size_t capacity; }`
/// (darray.h).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct darray_error_item {
    pub array: *mut error_item,
    pub num: usize,
    pub capacity: usize,
}

/// C `struct error_data`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct error_data {
    pub errors: darray_error_item,
}

/// C `struct lexer`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct lexer {
    pub text: *mut c_char,
    pub offset: *const c_char,
}

/// C `enum ignore_whitespace` values.
pub const PARSE_WHITESPACE: c_int = 0;
pub const IGNORE_WHITESPACE: c_int = 1;

/* ------------------------------------------------------------------------- */
/* strref comparisons (no allocation) */

/// The Rust view of a C `strref`. A NULL pointer or a NULL `array` is an
/// empty span — the C `strref_is_empty` checks the pointer/array before
/// the length and never dereferences a NULL array.
fn sr_of(p: *const strref) -> StrRef<'static> {
    if p.is_null() {
        return StrRef::clear();
    }
    // SAFETY: C-ABI — `p` is non-NULL (guarded above); the strref is a valid CPOD.
    let s = unsafe { &*p };
    if s.array.is_null() {
        StrRef::clear()
    } else {
        // The span is a prefix of the NUL-terminated C buffer it came
        // from; the compare loops only read within `len` (out-of-range
        // reads compare against NUL exactly like the C reference).
        // SAFETY: C-ABI — `s.array` is a valid buffer (checked above) and s.len bytes are valid.
        let bytes = unsafe { std::slice::from_raw_parts(s.array.cast::<u8>(), s.len) };
        StrRef::set(bytes, s.len)
    }
}

/// `EXPORT int strref_cmp(const struct strref *str1, const char *str2)`
///
/// # Safety
/// `str1` (or NULL) must point to a valid C `strref` whose `array` span
/// is readable for `len` bytes (a NULL `array` is treated as empty, like
/// the C `strref_is_empty`); `str2` (or NULL) must point to a
/// NUL-terminated string readable for the whole call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn strref_cmp(str1: *const strref, str2: *const c_char) -> c_int {
    obs_util::lexer::strref_cmp(&sr_of(str1), c_bytes(str2))
}

/// `EXPORT int strref_cmpi(const struct strref *str1, const char *str2)`
///
/// # Safety
/// As [`strref_cmp`], but the comparison is case-insensitive.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn strref_cmpi(str1: *const strref, str2: *const c_char) -> c_int {
    obs_util::lexer::strref_cmpi(&sr_of(str1), c_bytes(str2))
}

/// `EXPORT int strref_cmp_strref(const struct strref *str1,
/// const struct strref *str2)`
///
/// # Safety
/// `str1` and `str2` (or NULL) must each point to a valid C `strref`
/// whose `array` span is readable for `len` bytes (a NULL `array` is
/// treated as empty, like the C `strref_is_empty`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn strref_cmp_strref(str1: *const strref, str2: *const strref) -> c_int {
    obs_util::lexer::strref_cmp_strref(&sr_of(str1), &sr_of(str2))
}

/// `EXPORT int strref_cmpi_strref(const struct strref *str1,
/// const struct strref *str2)`
///
/// # Safety
/// As [`strref_cmp_strref`], but the comparison is case-insensitive.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn strref_cmpi_strref(str1: *const strref, str2: *const strref) -> c_int {
    obs_util::lexer::strref_cmpi_strref(&sr_of(str1), &sr_of(str2))
}

/* ------------------------------------------------------------------------- */
/* number validators */

/// `EXPORT bool valid_int_str(const char *str, size_t n)`
///
/// # Safety
/// `str` (or NULL) must point to a NUL-terminated string readable for
/// the whole call; `n` is the C size (0 = the whole string).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn valid_int_str(str: *const c_char, n: usize) -> bool {
    if str.is_null() {
        return false;
    }
    // SAFETY: a live NUL-terminated C string (checked above).
    let bytes = unsafe { CStr::from_ptr(str) }.to_bytes_with_nul();
    obs_util::lexer::valid_int_str(Some(bytes), n)
}

/// `EXPORT bool valid_float_str(const char *str, size_t n)`
///
/// # Safety
/// As [`valid_int_str`], but for the C float validator.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn valid_float_str(str: *const c_char, n: usize) -> bool {
    if str.is_null() {
        return false;
    }
    // SAFETY: a live NUL-terminated C string (checked above).
    let bytes = unsafe { CStr::from_ptr(str) }.to_bytes_with_nul();
    obs_util::lexer::valid_float_str(Some(bytes), n)
}

/* ------------------------------------------------------------------------- */
/* error_data (C-allocated growth, C-allocated buildstring) */

/// `EXPORT void error_data_add(struct error_data *ed, const char *file,
/// uint32_t row, uint32_t column, const char *msg, int level)`
///
/// # Safety
/// `ed` (or NULL) must point to a valid C `struct error_data` (a
/// `da_init`'d darray); `file` and `msg` (or NULL) must point to
/// NUL-terminated strings readable for the whole call — the message is
/// copied with the C allocator, `file` is only borrowed.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn error_data_add(
    ed: *mut error_data,
    file: *const c_char,
    row: u32,
    column: u32,
    msg: *const c_char,
    level: c_int,
) {
    // SAFETY: C-ABI — `ed` is live (checked above); the C string args are valid for the call.
    unsafe { error_data_add_c(ed, file, row, column, msg, level) }
}

/// `EXPORT char *error_data_buildstring(struct error_data *ed)`
///
/// # Safety
/// `ed` (or NULL) must point to a valid C `struct error_data` whose
/// items were added through [`error_data_add`] (readable C strings).
/// The returned string (or NULL) is C-allocated and must be freed with
/// the C `bfree`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn error_data_buildstring(ed: *mut error_data) -> *mut c_char {
    // SAFETY: C-ABI — `ed` is live (checked above); the result is bmem-owned.
    unsafe { error_data_buildstring_c(ed) }
}

/* ------------------------------------------------------------------------- */
/* the base lexer (runs over the C-owned buffer) */

/// `BASETOKEN_*` value of the engine token type.
fn mat_type(t: BaseTokenType) -> c_int {
    use BaseTokenType as B;
    match t {
        B::None => BASETOKEN_NONE,
        B::Alpha => BASETOKEN_ALPHA,
        B::Digit => BASETOKEN_DIGIT,
        B::Whitespace => BASETOKEN_WHITESPACE,
        B::Other => BASETOKEN_OTHER,
    }
}

/// `EXPORT bool lexer_getbasetoken(struct lexer *lex, struct base_token *t,
/// enum ignore_whitespace iws)`
///
/// Scans one base token from `lex->offset` over the C-owned text buffer
/// and writes `t`'s `strref`/type on success (the `strref` points into
/// `lex->text`, exactly where the C `strref_set` would point). A NULL
/// `lex`/`t`/`offset` returns false without touching `t` (C behaviour).
///
/// # Safety
/// `lex` (or NULL) must point to a C `struct lexer` set up with the C
/// inlines: `text` NUL-terminated, `offset` (or NULL) within
/// `[text, text+NUL]`; `t` (or NULL) must point to a writable C
/// `base_token`; `iws` must be a `PARSE_WHITESPACE`/`IGNORE_WHITESPACE`
/// value.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lexer_getbasetoken(
    lex: *mut lexer,
    t: *mut base_token,
    iws: c_int,
) -> bool {
    if lex.is_null() || t.is_null() {
        return false;
    }
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live and exclusively owned for the call.
    let l = unsafe { &mut *lex };
    if l.text.is_null() || l.offset.is_null() {
        return false;
    }
    // The C buffer is NUL-terminated (`bstrdup` from `lexer_start`); the
    // engine runs over the bytes without the NUL.
    // SAFETY: a live NUL-terminated C string (checked above).
    let text = unsafe { CStr::from_ptr(l.text) }.to_bytes();
    // SAFETY: C-ABI — both pointers are within the same live text buffer.
    let rel = unsafe { l.offset.offset_from(l.text as *const c_char) } as usize;
    let mut off = rel;
    let mut start = None;
    let mut ty = BaseTokenType::None;
    let ok = obs_util::lexer::basetoken_next(
        text,
        &mut off,
        &mut start,
        &mut ty,
        iws == IGNORE_WHITESPACE,
    );
    // SAFETY: C-ABI — `off` is within the live text buffer (checked above).
    l.offset = unsafe { l.text.add(off) } as *const c_char;
    if ok {
        let s = start.expect("basetoken_next sets start on success");
        // C writes only `token->text` and `token->type`;
        // `passed_whitespace` is left as the caller set it.
        // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live and exclusively owned for the call.
        let tok = unsafe { &mut *t };
        tok.text = strref {
            // SAFETY: C-ABI — `s` is within the live text buffer (checked above).
            array: unsafe { l.text.add(s) } as *const c_char,
            len: off - s,
        };
        tok.r#type = mat_type(ty);
    }
    ok
}

/// `EXPORT void lexer_getstroffset(const struct lexer *lex, const char *str,
/// uint32_t *row, uint32_t *col)`
///
/// 1-based row/column of `str` within `lex->text` (a `\r\n`/`\n\r` pair is
/// one line break); a NULL `str` leaves the outputs untouched (C
/// behaviour), a `str` before `lex->text` reports 1:1.
///
/// # Safety
/// `lex` (or NULL) must point to a C `struct lexer` with a
/// NUL-terminated `text`; `str` (or NULL) must point within the text
/// buffer (or at its NUL); `row`/`col` (or NULL) must be writable.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn lexer_getstroffset(
    lex: *const lexer,
    str: *const c_char,
    row: *mut u32,
    col: *mut u32,
) {
    if lex.is_null() || str.is_null() || row.is_null() || col.is_null() {
        return;
    }
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    let l = unsafe { &*lex };
    if l.text.is_null() {
        return;
    }
    // SAFETY: a live NUL-terminated C string (checked above).
    let text = unsafe { CStr::from_ptr(l.text) }.to_bytes();
    // SAFETY: C-ABI — both pointers are within the same live text buffer.
    let pos = unsafe { str.offset_from(l.text as *const c_char) };
    // `str` may point at the NUL (one past the last byte); earlier than
    // `text` the C loop does not run (row 1, col 1).
    let pos = if pos < 0 {
        0
    } else {
        (pos as usize).min(text.len())
    };
    // SAFETY: C-ABI — valid out storage (caller contract).
    let row = unsafe { &mut *row };
    // SAFETY: C-ABI — valid out storage (caller contract).
    let col = unsafe { &mut *col };
    obs_util::lexer::stroffset(text, pos, row, col);
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;
    use std::mem::size_of;

    // The bridge runs over C-owned state; these tests build the C structs
    // in a heap buffer and drive the exact `extern "C"` entry points.
    // (The `error_data` bridges touch the C allocator and are covered by
    // the swap-build conformance run, not `cargo test`.)

    fn cstr(s: &str) -> Vec<u8> {
        CString::new(s).unwrap().into_bytes_with_nul()
    }

    #[test]
    fn layout_matches_c() {
        // strref { const char *array; size_t len; }
        assert_eq!(size_of::<strref>(), 16);
        assert_eq!(std::mem::offset_of!(strref, len), 8);
        // base_token { strref; enum(int); bool; }
        assert_eq!(size_of::<base_token>(), 24);
        assert_eq!(std::mem::offset_of!(base_token, r#type), 16);
        assert_eq!(std::mem::offset_of!(base_token, passed_whitespace), 20);
        // error_item { char *; const char *; uint32_t row, column; int; }
        assert_eq!(size_of::<error_item>(), 32);
        assert_eq!(std::mem::offset_of!(error_item, row), 16);
        assert_eq!(std::mem::offset_of!(error_item, column), 20);
        assert_eq!(std::mem::offset_of!(error_item, level), 24);
        // DARRAY(struct error_item) { T *array; size_t num; size_t capacity; }
        assert_eq!(size_of::<error_data>(), 24);
        assert_eq!(std::mem::offset_of!(error_data, errors.num), 8);
        assert_eq!(std::mem::offset_of!(error_data, errors.capacity), 16);
        // lexer { char *text; const char *offset; }
        assert_eq!(size_of::<lexer>(), 16);
    }

    #[test]
    fn getbasetoken_walks_the_c_buffer() {
        // The buffer must stay NUL-terminated in C form; the bridge
        // tokenizes it in place and leaves `offset` at the NUL at the end.
        let mut bytes = cstr("ab_1 \n\n\r end");
        let mut l = lexer {
            text: bytes.as_mut_ptr() as *mut c_char,
            offset: bytes.as_ptr() as *const c_char,
        };
        let mut t = base_token {
            text: strref {
                array: std::ptr::null(),
                len: 0,
            },
            r#type: BASETOKEN_NONE,
            passed_whitespace: false,
        };
        let mut got = Vec::new();
        loop {
            // SAFETY: test — the lexer and token storage are live C storage (initialized above); the text is NUL-terminated.
            let r = unsafe { lexer_getbasetoken(&mut l, &mut t, PARSE_WHITESPACE) };
            if !r {
                break;
            }
            // SAFETY: C-ABI — `t.text.array` is a valid buffer (checked above) and t.text.len bytes are valid.
            let s = unsafe { std::slice::from_raw_parts(t.text.array.cast::<u8>(), t.text.len) };
            got.push((t.r#type, s.to_vec()));
        }
        assert_eq!(
            got,
            vec![
                (BASETOKEN_ALPHA, b"ab".to_vec()),
                (BASETOKEN_OTHER, b"_".to_vec()),
                (BASETOKEN_DIGIT, b"1".to_vec()),
                (BASETOKEN_WHITESPACE, b" ".to_vec()),
                (BASETOKEN_WHITESPACE, b"\n".to_vec()),
                (BASETOKEN_WHITESPACE, b"\n\r".to_vec()),
                (BASETOKEN_WHITESPACE, b" ".to_vec()),
                (BASETOKEN_ALPHA, b"end".to_vec()),
            ]
        );
        // the offset walked to the NUL and the text pointer is untouched
        // SAFETY: C-ABI — both pointers are within the same live text buffer.
        let rel = unsafe { l.offset.offset_from(l.text as *const c_char) };
        assert_eq!(rel, bytes.len() as isize - 1);
        // ignore_whitespace: leading whitespace is skipped, no token
        l.offset = l.text as *const c_char;
        // SAFETY: test — the lexer and token storage are live C storage (initialized above); the text is NUL-terminated.
        let r = unsafe { lexer_getbasetoken(&mut l, &mut t, IGNORE_WHITESPACE) };
        assert!(r);
        assert_eq!((t.r#type, t.text.len), (BASETOKEN_ALPHA, 2));

        // end of text: false, and `t` left unmodified
        let mut t2 = base_token {
            text: strref {
                array: std::ptr::null(),
                len: 42,
            },
            r#type: BASETOKEN_NONE,
            passed_whitespace: false,
        };
        // SAFETY: C-ABI — the NUL at the end of the live text buffer.
        l.offset = unsafe { l.text.add(bytes.len() - 1) } as *const c_char; // the NUL
        let at_end = l.offset;
        // SAFETY: test — the lexer and token storage are live C storage (initialized above); the text is NUL-terminated.
        assert!(!unsafe { lexer_getbasetoken(&mut l, &mut t2, PARSE_WHITESPACE) });
        assert_eq!(l.offset, at_end);
        assert_eq!((t2.text.len, t2.r#type), (42, BASETOKEN_NONE));
        // NULL offset: false (C: `if (!offset) return false;`)
        l.offset = std::ptr::null();
        // SAFETY: test — the lexer and token storage are live C storage (initialized above); the text is NUL-terminated.
        assert!(!unsafe { lexer_getbasetoken(&mut l, &mut t2, PARSE_WHITESPACE) });
    }

    #[test]
    fn stroffset_over_the_c_buffer() {
        let mut bytes = cstr("ab\r\ncd\n\ref");
        let l = lexer {
            text: bytes.as_mut_ptr() as *mut c_char,
            offset: bytes.as_ptr() as *const c_char,
        };
        // SAFETY: test — i is within the live byte buffer (checked above).
        let at = |i: usize| unsafe { bytes.as_ptr().add(i) } as *const c_char;
        let cases: &[(usize, u32, u32)] = &[
            (0, 1, 1),
            (3, 2, 1),
            (4, 2, 1),
            (7, 3, 1),
            (8, 3, 1),
            (9, 3, 2),
            (10, 3, 3), // the NUL position
        ];
        for &(i, row, col) in cases {
            let (mut r, mut c) = (999u32, 999u32);
            // SAFETY: test — `l` is a live lexer; the pointer is within the live text.
            unsafe { lexer_getstroffset(&l, at(i), &mut r, &mut c) };
            assert_eq!((r, c), (row, col), "pos {i}");
        }
        // NULL str leaves the outputs untouched
        let (mut r, mut c) = (7u32, 8u32);
        // SAFETY: test — `l` is a live lexer; NULL is the documented offset-0 case.
        unsafe { lexer_getstroffset(&l, std::ptr::null(), &mut r, &mut c) };
        assert_eq!((r, c), (7, 8));
    }

    /// NUL-terminated C string kept alive for the enclosing test scope.
    #[allow(dead_code)] // field 1 is the keeper, intentionally unread
    struct CStrRef(*const c_char, std::mem::ManuallyDrop<CString>);

    impl CStrRef {
        fn new(s: &str) -> Self {
            let c = std::mem::ManuallyDrop::new(CString::new(s).unwrap());
            CStrRef(c.as_ptr(), c)
        }
        fn ptr(&self) -> *const c_char {
            self.0
        }
    }

    #[test]
    fn strref_cmps_and_validators() {
        let abc = CStrRef::new("abc");
        let abd = CStrRef::new("abd");
        let ab = CStrRef::new("ab");
        let x = CStrRef::new("x");
        let a = CStrRef::new("a");
        let empty = CStrRef::new("");
        let s = strref {
            array: abc.ptr(),
            len: 3,
        };
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert_eq!(unsafe { strref_cmp(&s, abc.ptr()) }, 0);
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert_eq!(unsafe { strref_cmp(&s, abd.ptr()) }, -1);
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert_eq!(unsafe { strref_cmp(&s, ab.ptr()) }, 1);
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert_eq!(unsafe { strref_cmp(&s, std::ptr::null()) }, 1);
        // NULL str1 / NULL-array strref are empty (C `strref_is_empty`)
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert_eq!(unsafe { strref_cmp(std::ptr::null(), std::ptr::null()) }, 0);
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert_eq!(unsafe { strref_cmp(std::ptr::null(), x.ptr()) }, -1);
        let null_arr = strref {
            array: std::ptr::null(),
            len: 5,
        };
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert_eq!(unsafe { strref_cmp(&null_arr, x.ptr()) }, -1);
        // signed-char semantics: 0xe9 sorts below ASCII (the C `char` on
        // the reference architecture)
        let hi_cstr = std::mem::ManuallyDrop::new(CString::new(vec![0xe9u8]).unwrap());
        let hi = CStrRef(hi_cstr.as_ptr(), hi_cstr);
        let hs = strref {
            array: hi.ptr(),
            len: 1,
        };
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert_eq!(unsafe { strref_cmp(&hs, a.ptr()) }, -1);

        let s2 = strref {
            // SAFETY: test — the offset is within the live fixture buffer.
            array: unsafe { abc.ptr().add(1) },
            len: 2,
        };
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert_eq!(unsafe { strref_cmp_strref(&s, &s2) }, -1);
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert_eq!(unsafe { strref_cmp_strref(&s2, &s) }, 1);
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert_eq!(unsafe { strref_cmp_strref(&s2, &s2) }, 0);

        let i123 = CStrRef::new("123");
        let i12a = CStrRef::new("-12a");
        let f15 = CStrRef::new("1.5");
        let fe5 = CStrRef::new("1e+5");
        let fe5b = CStrRef::new("e5");
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert!(unsafe { valid_int_str(i123.ptr(), 0) });
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert!(!unsafe { valid_int_str(i12a.ptr(), 3) });
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert!(!unsafe { valid_int_str(std::ptr::null(), 0) });
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert!(!unsafe { valid_int_str(empty.ptr(), 0) });
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert!(unsafe { valid_float_str(f15.ptr(), 0) });
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert!(!unsafe { valid_float_str(fe5.ptr(), 0) });
        // SAFETY: test — the strref/C string args are live for the call (test fixtures; NULL is the documented case).
        assert!(!unsafe { valid_float_str(fe5b.ptr(), 0) });
    }
}
