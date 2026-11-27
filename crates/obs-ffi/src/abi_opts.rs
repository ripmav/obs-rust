//! P1-33.5 (WP-34): the C ABI for `shared/opts-parser` (S-024 M1, R01-R07).
//!
//! The exact C signatures of `opts-parser.h`; the parse core is
//! [`obs_util_small::opts_parser`]. Ownership mirrors the C reference
//! (`obs_free_options` releases everything): every word is a `bmem`
//! allocation (the `value` pointers point into the `input_words`
//! allocations, the `name`s are separate copies), and in a non-zero result
//! all three arrays are non-NULL even when empty (R07) — both the
//! `options` and `ignored_words` arrays are allocated for the full input
//! word count, exactly like the reference.

use std::mem::size_of;
use std::os::raw::c_char;
use std::ptr;

use obs_util_small::opts_parser;

use crate::abi_common::{bfree, bmalloc_c, bstrdup_bytes, c_bytes};

/// `struct obs_option` (opts-parser.h).
#[repr(C)]
pub struct obs_option {
    pub name: *mut c_char,
    pub value: *mut c_char,
}

/// `struct obs_options` (opts-parser.h).
#[repr(C)]
pub struct obs_options {
    pub count: usize,
    pub options: *mut obs_option,
    pub ignored_word_count: usize,
    pub ignored_words: *mut *mut c_char,
    pub input_words: *mut *mut c_char,
}

/// `struct obs_options obs_parse_options(const char *options_string)`.
///
/// NULL or all-space input returns the zero struct (R01).
///
/// # Safety
/// `options_string` must be a valid NUL-terminated C string (or NULL).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn obs_parse_options(options_string: *const c_char) -> obs_options {
    let Some(bytes) = c_bytes(options_string) else {
        return zero(); // R01: NULL
    };
    let p = opts_parser::parse_options(bytes);
    if p.is_zero() {
        return zero(); // R01: no non-space character
    }

    // input_words: bmalloc'ed NULL-terminated array of bmalloc'ed
    // NUL-terminated word copies.
    let n = p.words.len();
    // SAFETY: bmalloc is the C bmem allocator (the swap build links the C
    // implementation); a fresh nonzero-size block is returned.
    let input_words = unsafe { bmalloc_c((n + 1) * size_of::<*mut c_char>()) } as *mut *mut c_char;
    for (i, w) in p.words.iter().enumerate() {
        // SAFETY: the fresh bmalloc'd word copy is live and exclusively owned by this result.
        unsafe { *input_words.add(i) = bstrdup_bytes(w) };
    }
    // SAFETY: the array was allocated for n + 1 slots above.
    unsafe { *input_words.add(n) = ptr::null_mut() };

    // options: `name` is a bmem copy, `value` points into the allocated
    // word (the C value pointer points into input_words). Allocated for
    // the full input count so it stays non-NULL with count 0 (R07), as in
    // the reference.
    // SAFETY: bmalloc as above; n >= 1 in a non-zero result (R01/R02), so
    // the size is never zero.
    let options = unsafe { bmalloc_c(n * size_of::<obs_option>()) } as *mut obs_option;
    let mut oi = 0;
    for (wi, kind) in p.kind.iter().enumerate() {
        let Some(eq) = *kind else { continue };
        // SAFETY: word wi is allocated above; the copy of `eq` bytes fits.
        let name = unsafe { bstrdup_bytes(&p.words[wi][..eq]) };
        // SAFETY: word wi is NUL-terminated and eq + 1 < word.len() (R03), so the pointer stays inside the allocation.
        let value = unsafe { (*input_words.add(wi)).add(eq + 1) };
        // SAFETY: the options array holds n entries and oi < n (one per option word).
        unsafe {
            options.add(oi).write(obs_option { name, value });
        }
        oi += 1;
    }

    // ignored_words: pointers into input_words (`*ignored_word =
    // *input_word` in C), allocated for the full input count (R07).
    // SAFETY: bmalloc as above; n >= 1 (R01/R02).
    let ignored_words = unsafe { bmalloc_c(n * size_of::<*mut c_char>()) } as *mut *mut c_char;
    let mut ii = 0;
    for (wi, kind) in p.kind.iter().enumerate() {
        if kind.is_none() {
            // SAFETY: word wi is allocated above.
            unsafe { *ignored_words.add(ii) = *input_words.add(wi) };
            ii += 1;
        }
    }

    obs_options {
        count: oi,
        options,
        ignored_word_count: ii,
        ignored_words,
        input_words,
    }
}

/// The zero result (R01): all fields 0 / NULL.
const fn zero() -> obs_options {
    obs_options {
        count: 0,
        options: ptr::null_mut(),
        ignored_word_count: 0,
        ignored_words: ptr::null_mut(),
        input_words: ptr::null_mut(),
    }
}

/// `void obs_free_options(struct obs_options options)`.
///
/// A zero struct (or one with NULL pointers) is a no-op, as in C
/// (`bfree(NULL)` / `strlist_free(NULL)`).
///
/// # Safety
/// `options` must be the value returned by [`obs_parse_options`] (or a
/// zero struct); every pointer inside must not have been freed before.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn obs_free_options(options: obs_options) {
    if !options.options.is_null() {
        for i in 0..options.count {
            // SAFETY: every name was bmem-allocated by obs_parse_options.
            unsafe { bfree((*options.options.add(i)).name) };
        }
        // SAFETY: allocated by obs_parse_options (the full input word count).
        unsafe { bfree(options.options as *mut u8) };
    }
    if !options.ignored_words.is_null() {
        // SAFETY: allocated by obs_parse_options; the entries point into
        // input_words and are not freed here (the C does the same).
        unsafe { bfree(options.ignored_words as *mut u8) };
    }
    if !options.input_words.is_null() {
        // SAFETY: a NULL-terminated array allocated by obs_parse_options;
        // walk it to count the words (the struct carries no word count).
        let n = unsafe {
            let mut n = 0;
            while !(*options.input_words.add(n)).is_null() {
                n += 1;
            }
            n
        };
        for i in 0..n {
            // SAFETY: every word copy was bmem-allocated by obs_parse_options.
            unsafe { bfree(*options.input_words.add(i)) };
        }
        // SAFETY: the array itself was bmem-allocated by obs_parse_options.
        unsafe { bfree(options.input_words as *mut u8) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn parse_c(s: &str) -> obs_options {
        let c = std::ffi::CString::new(s).unwrap();
        // SAFETY: `c` is a valid NUL-terminated string for the call.
        let o = unsafe { obs_parse_options(c.as_ptr()) };
        // keep `c` alive: the value pointers point into the result, not c.
        drop(c);
        o
    }

    fn read(p: *const c_char) -> String {
        // SAFETY: a live NUL-terminated C string produced by the bridge.
        unsafe { std::ffi::CStr::from_ptr(p) }
            .to_string_lossy()
            .into_owned()
    }

    /// R01: NULL and all-space input -> the zero struct.
    #[test]
    fn r01_zero_result() {
        // SAFETY: NULL is the documented zero-result input.
        let o = unsafe { obs_parse_options(ptr::null()) };
        assert!(is_zero(&o));
        // SAFETY: zero struct (all NULL) — the no-op free contract.
        unsafe { obs_free_options(o) };

        let o = parse_c("     ");
        assert!(is_zero(&o));
        // SAFETY: the result is exclusively ours.
        unsafe { obs_free_options(o) };
    }

    fn is_zero(o: &obs_options) -> bool {
        o.count == 0
            && o.options.is_null()
            && o.ignored_word_count == 0
            && o.ignored_words.is_null()
            && o.input_words.is_null()
    }

    /// R02/R03/R04/R07: a mixed option/ignored input through the C ABI —
    /// the shape of the `opts-basic`/`opts-ignored` conformance vectors.
    #[test]
    fn r02_r04_mixed() {
        let o = parse_c("a=1 flag b=2 =x c= d=4");
        assert_eq!((o.count, o.ignored_word_count), (3, 3));
        assert!(!o.options.is_null());
        assert!(!o.ignored_words.is_null());
        assert!(!o.input_words.is_null());
        // SAFETY: the result is non-NULL (checked above) and i < count, so
        // each entry and its name/value pointers are live bridge strings.
        let names: Vec<String> = (0..o.count)
            .map(|i| unsafe { read((*o.options.add(i)).name) })
            .collect();
        // SAFETY: as above for the value pointers (inside the word buffers).
        let values: Vec<String> = (0..o.count)
            .map(|i| unsafe { read((*o.options.add(i)).value) })
            .collect();
        assert_eq!(names, ["a", "b", "d"]);
        assert_eq!(values, ["1", "2", "4"]);
        // value points into the input_words allocation (the C ownership).
        // SAFETY: words 0..6 exist (checked below: word 6 is the terminator).
        let words: Vec<String> = (0..6)
            .map(|i| unsafe { read(*o.input_words.add(i)) })
            .collect();
        assert_eq!(words, ["a=1", "flag", "b=2", "=x", "c=", "d=4"]);
        // SAFETY: word 6 is the NULL terminator written by the bridge.
        assert!(unsafe { (*o.input_words.add(6)).is_null() });
        // SAFETY: the result is exclusively ours.
        unsafe { obs_free_options(o) };
    }

    /// R07: an all-ignored input has non-NULL options with count 0.
    #[test]
    fn r07_all_ignored() {
        let o = parse_c("only words");
        assert_eq!(o.count, 0);
        assert!(!o.options.is_null());
        assert_eq!(o.ignored_word_count, 2);
        // SAFETY: the result is exclusively ours.
        unsafe { obs_free_options(o) };
    }

    /// Q-1: no quoting — the vector `opts-noquoting` shape through the C ABI.
    #[test]
    fn q1_no_quoting() {
        let o = parse_c("title=\"hello world\"");
        assert_eq!(o.count, 1);
        // SAFETY: count == 1 (checked above), so options[0]'s name/value are
        // live, non-NULL bridge strings.
        let (name, value) = unsafe { (read((*o.options).name), read((*o.options.add(0)).value)) };
        assert_eq!(name, "title");
        assert_eq!(value, "\"hello");
        assert_eq!(o.ignored_word_count, 1);
        // SAFETY: the result is exclusively ours.
        unsafe { obs_free_options(o) };
    }

    /// R03: a second `=` belongs to the value.
    #[test]
    fn r03_value_keeps_eq() {
        let o = parse_c("eq=a=b==c");
        assert_eq!(o.count, 1);
        // SAFETY: count == 1 (checked above), so options[0]'s name/value are
        // live, non-NULL bridge strings.
        let (name, value) = unsafe { (read((*o.options).name), read((*o.options.add(0)).value)) };
        assert_eq!(name, "eq");
        assert_eq!(value, "a=b==c");
        // SAFETY: the result is exclusively ours.
        unsafe { obs_free_options(o) };
    }
}
