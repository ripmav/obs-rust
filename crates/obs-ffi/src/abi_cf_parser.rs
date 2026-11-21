//! P1-08.5 recipe step 2 (part B2): the C ABI for the C-family parser
//! (`libobs/util/cf-parser.h`).
//!
//! The two `EXPORT` functions with the exact C signatures
//! (`cf_adderror`, `cf_pass_pair`); every other declaration in the header
//! is a `static inline` helper that stays C-side — the conformance
//! runners, `effect-parser.c`/`shader-parser.c` and `decl.c` call them
//! directly, and they drive parsing by poking the C-owned
//! [`cf_parser`]'s fields (the cursor is a `struct cf_token *` that
//! walks the preprocessor's token array with raw pointer arithmetic).
//!
//! [`cf_parser`] is a C-allocated view (a global in the runners, a member
//! of the C `struct effect_parser`/`decl`): the bridge never allocates
//! or frees the struct itself — the C `cf_parser_init`/`cf_parser_free`
//! inlines call the bridged `cf_lexer_*`/`cf_preprocessor_*` functions on
//! the embedded fields and the C `error_data_*` inlines on the error
//! list. The embedded mirrors are the same ones the cf-lexer bridge
//! writes in place (one representation, same allocation/free
//! conventions). The only memory C later `bfree`s here is the error
//! message, which [`error_data_add_c`] bstrdups with the C allocator.

use std::os::raw::{c_char, c_int};

use crate::abi_cf_lexer::{
    CFTOKEN_NONE, CFTOKEN_OTHER, cf_lexer, cf_preprocessor, cf_token, safe_printf,
};
use crate::abi_common::{bfree, bstrdup_bytes, c_bytes, error_data_add_c};
use crate::abi_lexer::{error_data, lexer_getstroffset};

/* ------------------------------------------------------------------------- */
/* C struct mirror (exact field layout, C owns the struct) */

/// C `struct cf_parser` (cf-parser.h): the C caller allocates this and
/// the C-side inlines read/write its fields directly, so the layout must
/// match exactly. The sub-structs are the repr(C) mirrors the cf-lexer
/// and base-lexer bridges already operate on in place; `cur_token`
/// points into `pp.tokens.array` (a C-allocated darray).
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct cf_parser {
    /// C `struct cf_lexer lex`.
    pub lex: cf_lexer,
    /// C `struct cf_preprocessor pp`.
    pub pp: cf_preprocessor,
    /// C `struct error_data error_list`.
    pub error_list: error_data,
    /// C `struct cf_token *cur_token` (NULL before the first parse).
    pub cur_token: *mut cf_token,
}

/* ------------------------------------------------------------------------- */
/* EXPORT: cf_adderror */

/// `EXPORT void cf_adderror(struct cf_parser *parser, const char *error,
/// int level, const char *val1, const char *val2, const char *val3)`
///
/// The error/warning sink for the C-side parser helpers. The row/col come
/// from the current token's unmerged text in its owning lexer (vector 32
/// pins the `@1:1`/`@2:3`/`@2:6` positions); with all values NULL the
/// message is added verbatim, otherwise `dstr_safe_printf` substitutes
/// `$1`/`$2`/`$3` for the non-NULL values only — a NULL value leaves its
/// `$n` in the message (vector 32's `only2 x$2z`). The `file` is
/// borrowed from the owning lexer (NULL prints as `(null)`, like the
/// glibc `%s` in the reference).
///
/// # Safety
/// `parser` must point to a C `struct cf_parser` initialized by the C
/// `cf_parser_init` inline whose `cur_token` points into a valid token
/// array — a NULL cursor dereferences here, exactly like the reference
/// (vector 90 pins the crash); `error` and the non-NULL `val*` must
/// point to NUL-terminated strings readable for the whole call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cf_adderror(
    parser: *mut cf_parser,
    error: *const c_char,
    level: c_int,
    val1: *const c_char,
    val2: *const c_char,
    val3: *const c_char,
) {
    let p = unsafe { &mut *parser };
    let cur = p.cur_token;
    let lex = unsafe { &*(*cur).lex };
    let mut row: u32 = 0;
    let mut col: u32 = 0;
    unsafe {
        lexer_getstroffset(
            &lex.base_lexer,
            (*cur).unmerged_str.array,
            &mut row,
            &mut col,
        )
    };
    if val1.is_null() && val2.is_null() && val3.is_null() {
        unsafe { error_data_add_c(&mut p.error_list, lex.file, row, col, error, level) };
        return;
    }
    // C formats into a dstr, passes its buffer to error_data_add (which
    // bstrdups it) and frees the dstr. The temp copy is bstrdup_bytes'd
    // (Rust-owned bytes, never C-strlen'd) and bfree'd after the add.
    let msg = safe_printf(c_bytes(error).unwrap_or(&[]), val1, val2, val3);
    let temp = unsafe { bstrdup_bytes(&msg) };
    unsafe {
        error_data_add_c(
            &mut p.error_list,
            lex.file,
            row,
            col,
            temp as *const c_char,
            level,
        )
    };
    unsafe { bfree(temp) };
}

/* ------------------------------------------------------------------------- */
/* EXPORT: cf_pass_pair */

/// `EXPORT bool cf_pass_pair(struct cf_parser *p, char in, char out)`
///
/// Advances `p->cur_token` past one balanced `in`/`out` pair, recursing
/// on nested `in`s (vector 30's `{ x; { y } z }` and the `( ... )`
/// cases), and returns false when the closing `out` is missing. A
/// cursor token that is not the `in` character — or a token of another
/// type — leaves the cursor alone and reports `type != NONE`, like the
/// reference.
///
/// # Safety
/// `p` must point to a C `struct cf_parser` whose `cur_token` points
/// into a valid, NONE-terminated token array (the cursor walks it with
/// pointer arithmetic; a NULL cursor dereferences, like the reference).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn cf_pass_pair(p: *mut cf_parser, in_c: c_char, out_c: c_char) -> bool {
    let p = unsafe { &mut *p };
    unsafe {
        let cur = p.cur_token;
        if (*cur).r#type != CFTOKEN_OTHER || *(*cur).str.array != in_c {
            return (*cur).r#type != CFTOKEN_NONE;
        }
        p.cur_token = cur.add(1);
    }
    while unsafe { (*p.cur_token).r#type } != CFTOKEN_NONE {
        if unsafe { *(*p.cur_token).str.array } == in_c {
            if !unsafe { cf_pass_pair(p, in_c, out_c) } {
                return false;
            }
            continue;
        }
        if unsafe { *(*p.cur_token).str.array } == out_c {
            unsafe { p.cur_token = p.cur_token.add(1) };
            return true;
        }
        unsafe { p.cur_token = p.cur_token.add(1) };
    }
    false
}

/* ------------------------------------------------------------------------- */

#[cfg(test)]
mod tests {
    use super::*;
    use crate::abi_cf_lexer::{CFTOKEN_NAME, darray};
    use crate::abi_lexer::{darray_error_item, strref};
    use std::ffi::CString;
    use std::mem::size_of;
    use std::ptr;

    // The C-allocator stand-ins (bmalloc/bfree) that `error_data_add_c`
    // links against come from the `abi_cf_lexer` test module (same test
    // binary); the swap-build conformance run links the real bmem.c.

    fn cstr(s: &str) -> CString {
        CString::new(s).unwrap()
    }

    fn other(s: &'static [u8]) -> cf_token {
        cf_token {
            lex: ptr::null(),
            str: strref {
                array: s.as_ptr() as *const c_char,
                len: s.len(),
            },
            unmerged_str: strref {
                array: s.as_ptr() as *const c_char,
                len: s.len(),
            },
            r#type: CFTOKEN_OTHER,
        }
    }

    /// C `struct cf_parser` over a hand-built, NONE-terminated token
    /// array; `at` is the initial cursor index. The tokens' `lex`
    /// pointers are left NULL — the caller points them at a live lexer
    /// (like a real parse, `&P.lex`) after the parser is placed.
    fn mk_parser(toks: Vec<cf_token>, at: usize) -> (Vec<cf_token>, cf_parser) {
        let mut p = cf_parser {
            lex: cf_lexer::init(),
            pp: cf_preprocessor::init(),
            error_list: error_data {
                errors: darray_error_item {
                    array: ptr::null_mut(),
                    num: 0,
                    capacity: 0,
                },
            },
            cur_token: ptr::null_mut(),
        };
        p.pp.tokens = darray {
            array: toks.as_ptr() as *mut cf_token,
            num: toks.len(),
            capacity: toks.len(),
        };
        p.cur_token = unsafe { (toks.as_ptr() as *mut cf_token).add(at) };
        (toks, p)
    }

    #[test]
    fn layout_matches_c() {
        // C `struct cf_parser` { struct cf_lexer; struct cf_preprocessor;
        // struct error_data; struct cf_token *; } — all fields pointer
        // aligned, no padding between them.
        assert_eq!(size_of::<cf_parser>(), 224);
        assert_eq!(std::mem::offset_of!(cf_parser, lex), 0);
        assert_eq!(std::mem::offset_of!(cf_parser, pp), size_of::<cf_lexer>());
        assert_eq!(
            std::mem::offset_of!(cf_parser, error_list),
            size_of::<cf_lexer>() + size_of::<cf_preprocessor>()
        );
        assert_eq!(
            std::mem::offset_of!(cf_parser, cur_token),
            size_of::<cf_parser>() - 8
        );
    }

    #[test]
    fn pass_pair_nested_and_unbalanced() {
        // ( a ( b ) c ) NONE — balanced: true, cursor past the last )
        let toks = vec![
            other(b"("),
            other(b"a"),
            other(b"("),
            other(b"b"),
            other(b")"),
            other(b"c"),
            other(b")"),
            cf_token::clear(),
        ];
        let (_t, mut p) = mk_parser(toks, 0);
        assert!(unsafe { cf_pass_pair(&mut p, b'(' as c_char, b')' as c_char) });
        assert_eq!(unsafe { p.cur_token.offset_from(p.pp.tokens.array) }, 7);

        // ( a ( b ) NONE — unbalanced: false, cursor at the terminator
        let toks = vec![
            other(b"("),
            other(b"a"),
            other(b"("),
            other(b"b"),
            other(b")"),
            cf_token::clear(),
        ];
        let (_t, mut p) = mk_parser(toks, 0);
        assert!(!unsafe { cf_pass_pair(&mut p, b'(' as c_char, b')' as c_char) });
        assert_eq!(unsafe { p.cur_token.offset_from(p.pp.tokens.array) }, 5);

        // cursor not on the `in` char: true (type != NONE), cursor stays
        let toks = vec![other(b"a"), other(b"("), other(b")"), cf_token::clear()];
        let (_t, mut p) = mk_parser(toks, 0);
        assert!(unsafe { cf_pass_pair(&mut p, b'(' as c_char, b')' as c_char) });
        assert_eq!(unsafe { p.cur_token.offset_from(p.pp.tokens.array) }, 0);

        // a non-OTHER token on the cursor: same, cursor stays
        let toks = vec![
            cf_token {
                r#type: CFTOKEN_NAME,
                ..other(b"a")
            },
            other(b"("),
            other(b")"),
            cf_token::clear(),
        ];
        let (_t, mut p) = mk_parser(toks, 0);
        assert!(unsafe { cf_pass_pair(&mut p, b'(' as c_char, b')' as c_char) });
        assert_eq!(unsafe { p.cur_token.offset_from(p.pp.tokens.array) }, 0);

        // at the terminator: false, cursor stays
        let toks = vec![other(b"a"), cf_token::clear()];
        let (_t, mut p) = mk_parser(toks, 1);
        assert!(!unsafe { cf_pass_pair(&mut p, b'(' as c_char, b')' as c_char) });
        assert_eq!(unsafe { p.cur_token.offset_from(p.pp.tokens.array) }, 1);
    }

    #[test]
    fn adderror_positions_and_format() {
        // "ab\n  cd": the token at text+5 ("cd") is at row 2, col 3
        let text = cstr("ab\n  cd");
        let toks = vec![
            cf_token {
                lex: ptr::null(),
                str: strref {
                    array: unsafe { text.as_ptr().add(5) },
                    len: 2,
                },
                unmerged_str: strref {
                    array: unsafe { text.as_ptr().add(5) },
                    len: 2,
                },
                r#type: CFTOKEN_NAME,
            },
            cf_token::clear(),
        ];
        let (mut toks, mut p) = mk_parser(toks, 0);
        // the tokens belong to the parser's own lexer (like P.lex's)
        for t in toks.iter_mut() {
            t.lex = &p.lex as *const cf_lexer;
        }
        p.lex.base_lexer.text = text.as_ptr() as *mut c_char;
        p.lex.base_lexer.offset = p.lex.base_lexer.text as *const c_char;
        // p.lex.file stays NULL (vector 32: file=@null)

        let plain = cstr("plain $1");
        unsafe {
            cf_adderror(
                &mut p,
                plain.as_ptr(),
                0,
                ptr::null(),
                ptr::null(),
                ptr::null(),
            )
        };
        let fmt = cstr("v=$1 $2 $3");
        let a = cstr("A");
        let b = cstr("B");
        unsafe { cf_adderror(&mut p, fmt.as_ptr(), 1, a.as_ptr(), b.as_ptr(), ptr::null()) };

        assert_eq!(p.error_list.errors.num, 2);
        let e0 = unsafe { &*p.error_list.errors.array.add(0) };
        assert_eq!((e0.level, e0.row, e0.column), (0, 2, 3));
        assert!(e0.file.is_null());
        // all-NULL values: the message is added verbatim
        assert_eq!(c_bytes(e0.error), Some(b"plain $1".as_slice()));
        let e1 = unsafe { &*p.error_list.errors.array.add(1) };
        assert_eq!((e1.level, e1.row, e1.column), (1, 2, 3));
        // a NULL value leaves its $n in place (dstr_safe_printf)
        assert_eq!(c_bytes(e1.error), Some(b"v=A B $3".as_slice()));

        // free the C-allocated list like the C error_data_free inline
        for i in 0..p.error_list.errors.num {
            let e = unsafe { &*p.error_list.errors.array.add(i) };
            unsafe { bfree(e.error) };
        }
        unsafe { bfree(p.error_list.errors.array as *mut u8) };
    }
}
