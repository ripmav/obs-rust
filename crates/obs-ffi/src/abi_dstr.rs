//! P1-02.4 recipe step 2: the C ABI for the dstr exports
//! (`libobs/util/dstr.c`, S-006).
//!
//! 37 of the 41 exports. The four variadic/`va_list` functions
//! (`dstr_printf`, `dstr_catf`, `dstr_vprintf`, `dstr_vcatf`) cannot be
//! defined in Rust — they stay C in `c/dstr-variadic-bridge.c` (the
//! `srcprof-bridge.c` precedent; P1-02.3 "implement in the C shim"),
//! operating on the shared `struct dstr` through the dstr.h inlines.
//! The `static inline` helpers of dstr.h stay in the C header (they
//! drive the shared layout, the darray/deque precedent).
//!
//! `struct dstr` is the Rust `Dstr` (repr-C mirror: `array/len/
//! capacity`); `struct strref` is the Rust `StrRef`.

use std::os::raw::{c_char, c_int};

use crate::abi_common::bstrdup_bytes;
use obs_util::bmem::{bfree_raw, bmalloc_raw};
use obs_util::dstr::{
    Dstr, StrRef, astrcmp_n as cd_astrcmp_n, astrcmpi as cd_astrcmpi, astrcmpi_n as cd_astrcmpi_n,
    astrstri as cd_astrstri, dstr_cat_dstr as cd_dstr_cat_dstr,
    dstr_cat_strref as cd_dstr_cat_strref, dstr_copy as cd_dstr_copy,
    dstr_copy_strref as cd_dstr_copy_strref, dstr_depad as cd_dstr_depad,
    dstr_from_mbs as cd_dstr_from_mbs, dstr_from_wcs as cd_dstr_from_wcs,
    dstr_init_copy_strref as cd_dstr_init_copy_strref, dstr_insert as cd_dstr_insert,
    dstr_insert_ch as cd_dstr_insert_ch, dstr_insert_dstr as cd_dstr_insert_dstr,
    dstr_left as cd_dstr_left, dstr_mid as cd_dstr_mid, dstr_ncat as cd_dstr_ncat,
    dstr_ncat_dstr as cd_dstr_ncat_dstr, dstr_ncopy as cd_dstr_ncopy,
    dstr_ncopy_dstr as cd_dstr_ncopy_dstr, dstr_remove as cd_dstr_remove,
    dstr_replace as cd_dstr_replace, dstr_right as cd_dstr_right,
    dstr_safe_printf as cd_dstr_safe_printf, dstr_to_lower as cd_dstr_to_lower,
    dstr_to_mbs as cd_dstr_to_mbs, dstr_to_upper as cd_dstr_to_upper,
    dstr_to_wcs as cd_dstr_to_wcs, strdepad as cd_strdepad, strlist_split as cd_strlist_split,
    wcsdepad as cd_wcsdepad, wstrcmp_n as cd_wstrcmp_n, wstrcmpi as cd_wstrcmpi,
    wstrcmpi_n as cd_wstrcmpi_n, wstrstri as cd_wstrstri,
};

/// Read a NUL-terminated C string as a byte slice (NULL → `None`).
fn cstr<'a>(p: *const c_char) -> Option<&'a [u8]> {
    crate::abi_common::c_bytes(p)
}

/// Read a NUL-terminated wide string (u32) slice (NULL → `None`).
///
/// # Safety
/// `p` must be NULL or a live `wchar_t` string readable to its NUL.
unsafe fn wstr<'a>(p: *const u32) -> Option<&'a [u32]> {
    if p.is_null() {
        return None;
    }
    // SAFETY: caller contract — live wide string; scan to NUL.
    let mut n = 0;
    loop {
        // SAFETY: scan within the live string.
        if unsafe { *p.add(n) } == 0 {
            break;
        }
        n += 1;
    }
    // SAFETY: the scan bound is within the live buffer.
    Some(unsafe { std::slice::from_raw_parts(p, n) })
}

/// Writable wide slice (NULL → `None`).
///
/// # Safety
/// `p` must be NULL or a live `wchar_t` buffer readable/writable to its NUL.
unsafe fn wstr_mut<'a>(p: *mut u32) -> Option<&'a mut [u32]> {
    if p.is_null() {
        return None;
    }
    // SAFETY: caller contract — live wide string; scan to NUL.
    let mut n = 0;
    loop {
        // SAFETY: scan within the live string.
        if unsafe { *p.add(n) } == 0 {
            break;
        }
        n += 1;
    }
    // SAFETY: the scan bound is within the live buffer.
    Some(unsafe { std::slice::from_raw_parts_mut(p, n) })
}

/// Read a NUL-terminated C string as a **writable** byte slice
/// (NULL → `None`).
///
/// # Safety
/// `p` must be NULL or a live C buffer writable to its NUL.
unsafe fn c_bytes_mut<'a>(p: *mut c_char) -> Option<&'a mut [u8]> {
    if p.is_null() {
        return None;
    }
    // SAFETY: caller contract — live string; scan to NUL.
    let mut n = 0;
    loop {
        // SAFETY: scan within the live string.
        if unsafe { *p.add(n) } == 0 {
            break;
        }
        n += 1;
    }
    // SAFETY: the scan bound is within the live buffer.
    Some(unsafe { std::slice::from_raw_parts_mut(p.cast::<u8>(), n) })
}

// ── The compare / search / depad families (S-006 R22/R31/R33) ──────────

/// `EXPORT int astrcmpi(const char *str1, const char *str2)`
///
/// # Safety
/// `str1`/`str2` must be NULL or live NUL-terminated strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn astrcmpi(str1: *const c_char, str2: *const c_char) -> c_int {
    // The C reads go through `cstr` (NULL -> empty, the C blank-string
    // rule); the comparison itself is safe.
    cd_astrcmpi(cstr(str1), cstr(str2)) as c_int
}

/// `EXPORT int wstrcmpi(const wchar_t *str1, const wchar_t *str2)`
///
/// # Safety
/// `str1`/`str2` must be NULL or live NUL-terminated wide strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wstrcmpi(str1: *const u32, str2: *const u32) -> c_int {
    // SAFETY: both reads are independent live wide strings.
    (unsafe { cd_wstrcmpi(wstr(str1), wstr(str2)) }) as c_int
}

/// `EXPORT int astrcmp_n(const char *str1, const char *str2, size_t n)`
///
/// # Safety
/// `str1`/`str2` must be NULL or live NUL-terminated strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn astrcmp_n(str1: *const c_char, str2: *const c_char, n: usize) -> c_int {
    // The C reads go through `cstr` (NULL -> empty, the C blank-string
    // rule); the comparison itself is safe.
    cd_astrcmp_n(cstr(str1), cstr(str2), n) as c_int
}

/// `EXPORT int wstrcmp_n(const wchar_t *str1, const wchar_t *str2, size_t n)`
///
/// # Safety
/// `str1`/`str2` must be NULL or live NUL-terminated wide strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wstrcmp_n(str1: *const u32, str2: *const u32, n: usize) -> c_int {
    // SAFETY: both reads are independent live wide strings.
    (unsafe { cd_wstrcmp_n(wstr(str1), wstr(str2), n) }) as c_int
}

/// `EXPORT int astrcmpi_n(const char *str1, const char *str2, size_t n)`
///
/// # Safety
/// `str1`/`str2` must be NULL or live NUL-terminated strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn astrcmpi_n(str1: *const c_char, str2: *const c_char, n: usize) -> c_int {
    // The C reads go through `cstr` (NULL -> empty, the C blank-string
    // rule); the comparison itself is safe.
    cd_astrcmpi_n(cstr(str1), cstr(str2), n) as c_int
}

/// `EXPORT int wstrcmpi_n(const wchar_t *str1, const wchar_t *str2, size_t n)`
///
/// # Safety
/// `str1`/`str2` must be NULL or live NUL-terminated wide strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wstrcmpi_n(str1: *const u32, str2: *const u32, n: usize) -> c_int {
    // SAFETY: both reads are independent live wide strings.
    (unsafe { cd_wstrcmpi_n(wstr(str1), wstr(str2), n) }) as c_int
}

/// `EXPORT char *astrstri(const char *str, const char *find)`
///
/// # Safety
/// `str`/`find` must be NULL or live NUL-terminated strings; the
/// returned pointer points into `str` (or is NULL).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn astrstri(str: *const c_char, find: *const c_char) -> *mut c_char {
    // The C reads go through `cstr`; the offset arithmetic is safe.
    let off = cd_astrstri(cstr(str), cstr(find));
    match off {
        Some(o) => {
            // SAFETY: `o` is a byte offset within the live `str`; the C reference hands back `(char *)str` (the const discard is the API).
            unsafe { str.add(o) as *mut c_char }
        }
        None => std::ptr::null_mut(),
    }
}

/// `EXPORT wchar_t *wstrstri(const wchar_t *str, const wchar_t *find)`
///
/// # Safety
/// `str`/`find` must be NULL or live NUL-terminated wide strings; the
/// returned pointer points into `str` (or is NULL).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wstrstri(str: *const u32, find: *const u32) -> *mut u32 {
    // SAFETY: both reads are independent live wide strings.
    let off = unsafe { cd_wstrstri(wstr(str), wstr(find)) };
    match off {
        Some(o) => {
            // SAFETY: `o` is a code-unit offset within the live `str`; the C reference hands back `(wchar_t *)str` (const discard).
            unsafe { str.add(o) as *mut u32 }
        }
        None => std::ptr::null_mut(),
    }
}

/// `EXPORT char *strdepad(char *str)`
///
/// # Safety
/// `str` must be NULL or a live NUL-terminated buffer writable for its
/// length (the in-place depad).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn strdepad(str: *mut c_char) -> *mut c_char {
    // SAFETY: the depad is in place within the live string.
    let _ = unsafe { cd_strdepad(c_bytes_mut(str)) };
    str
}

/// `EXPORT wchar_t *wcsdepad(wchar_t *str)`
///
/// # Safety
/// `str` must be NULL or a live NUL-terminated wide buffer writable for
/// its length.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn wcsdepad(str: *mut u32) -> *mut u32 {
    // SAFETY: the depad is in place within the live wide string.
    let _ = unsafe { cd_wcsdepad(wstr_mut(str)) };
    str
}

// ── strlist (S-006 R40) ─────────────────────────────────────────────────

/// `EXPORT char **strlist_split(const char *str, char split_ch, bool
/// include_empty)`
///
/// # Safety
/// `str` must be NULL or a live NUL-terminated string. The returned
/// block (table + strings, one `bmalloc` allocation) must be freed with
/// `strlist_free` (NULL input → NULL, C behaviour).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn strlist_split(
    str: *const c_char,
    split_ch: c_char,
    include_empty: bool,
) -> *mut *mut c_char {
    // `str` is a live C string for the scan (safe through `cstr`).
    let list = cd_strlist_split(cstr(str), split_ch as i8 as u8, include_empty);
    match list {
        Some(l) => {
            // The C caller owns the block; drop the Rust Drop guard.
            let ptrs = l.ptrs;
            std::mem::forget(l);
            // The retype keeps size/alignment (the C `char **` table).
            ptrs.cast::<*mut c_char>()
        }
        None => std::ptr::null_mut(),
    }
}

/// `EXPORT void strlist_free(char **strlist)`
///
/// # Safety
/// `strlist` must be NULL or the result of `strlist_split` (the table
/// is the start of the one allocation, as in C).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn strlist_free(strlist: *mut *mut c_char) {
    if strlist.is_null() {
        return;
    }
    // SAFETY: the table pointer is the block start (both C and Rust layouts) — one `bfree` frees it all.
    unsafe { bfree_raw(strlist.cast::<u8>()) };
}

// ── The dstr op families (S-006 R01–R25) ────────────────────────────────

/// `EXPORT void dstr_init_copy_strref(struct dstr *dst, const struct
/// strref *src)`
///
/// # Safety
/// `dst` must be a live dstr; `src` a live strref.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_init_copy_strref(dst: *mut Dstr, src: *const StrRef) {
    // SAFETY: caller contract — both live for the call.
    unsafe { cd_dstr_init_copy_strref(&mut *dst, &*src) }
}

/// `EXPORT void dstr_copy(struct dstr *dst, const char *array)`
///
/// # Safety
/// `dst` must be a live dstr; `array` NULL or a live NUL-terminated
/// string (C: NULL/empty frees `dst`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_copy(dst: *mut Dstr, array: *const c_char) {
    let bytes = cstr(array);
    // SAFETY: caller contract — `dst` live; `array` a live C string.
    unsafe { cd_dstr_copy(&mut *dst, bytes.unwrap_or(&[])) };
}

/// `EXPORT void dstr_copy_strref(struct dstr *dst, const struct
/// strref *src)`
///
/// # Safety
/// `dst` must be a live dstr; `src` a live strref.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_copy_strref(dst: *mut Dstr, src: *const StrRef) {
    // SAFETY: caller contract — both live for the call.
    unsafe { cd_dstr_copy_strref(&mut *dst, &*src) }
}

/// `EXPORT void dstr_ncopy(struct dstr *dst, const char *array, const
/// size_t len)`
///
/// # Safety
/// `dst` must be a live dstr; `array` NULL or readable for `len` bytes
/// (C: NULL/empty/len-0 frees `dst`).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_ncopy(dst: *mut Dstr, array: *const c_char, len: usize) {
    // SAFETY: caller contract — `dst` live; `array` readable for `len` when non-null.
    let bytes = unsafe {
        if array.is_null() || len == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(array.cast::<u8>(), len)
        }
    };
    // SAFETY: caller contract — `dst` live.
    unsafe { cd_dstr_ncopy(&mut *dst, bytes, len) };
}

/// `EXPORT void dstr_ncopy_dstr(struct dstr *dst, const struct dstr
/// *src, const size_t len)`
///
/// # Safety
/// `dst`/`src` must be live dstrs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_ncopy_dstr(dst: *mut Dstr, src: *const Dstr, len: usize) {
    // SAFETY: caller contract — both live for the call.
    unsafe { cd_dstr_ncopy_dstr(&mut *dst, &*src, len) }
}

/// `EXPORT void dstr_cat_dstr(struct dstr *dst, const struct dstr *str)`
///
/// # Safety
/// `dst`/`str` must be live dstrs (C: a NULL/empty `str` is a no-op).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_cat_dstr(dst: *mut Dstr, str: *const Dstr) {
    // SAFETY: caller contract — both live for the call; a NULL dstr is the zeroed struct (empty).
    unsafe { cd_dstr_cat_dstr(&mut *dst, &*str) }
}

/// `EXPORT void dstr_cat_strref(struct dstr *dst, const struct
/// strref *str)`
///
/// # Safety
/// `dst` must be a live dstr; `str` a live strref.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_cat_strref(dst: *mut Dstr, str: *const StrRef) {
    // SAFETY: caller contract — both live for the call.
    unsafe { cd_dstr_cat_strref(&mut *dst, &*str) }
}

/// `EXPORT void dstr_ncat(struct dstr *dst, const char *array, const
/// size_t len)`
///
/// # Safety
/// `dst` must be a live dstr; `array` NULL or readable for `len` bytes
/// (C: NULL/empty/len-0 is a no-op).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_ncat(dst: *mut Dstr, array: *const c_char, len: usize) {
    // SAFETY: caller contract — `dst` live; `array` readable for `len` when non-null.
    let bytes = unsafe {
        if array.is_null() || len == 0 {
            &[]
        } else {
            std::slice::from_raw_parts(array.cast::<u8>(), len)
        }
    };
    // SAFETY: caller contract — `dst` live.
    unsafe { cd_dstr_ncat(&mut *dst, bytes, len) };
}

/// `EXPORT void dstr_ncat_dstr(struct dstr *dst, const struct dstr
/// *str, const size_t len)`
///
/// # Safety
/// `dst`/`str` must be live dstrs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_ncat_dstr(dst: *mut Dstr, str: *const Dstr, len: usize) {
    // SAFETY: caller contract — both live for the call.
    unsafe { cd_dstr_ncat_dstr(&mut *dst, &*str, len) }
}

/// `EXPORT void dstr_insert(struct dstr *dst, const size_t idx, const
/// char *array)`
///
/// # Safety
/// `dst` must be a live dstr; `array` NULL or a live NUL-terminated
/// string (C: NULL/empty is a no-op).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_insert(dst: *mut Dstr, idx: usize, array: *const c_char) {
    let bytes = cstr(array);
    // SAFETY: caller contract — `dst` live; `array` a live C string.
    unsafe { cd_dstr_insert(&mut *dst, idx, bytes.unwrap_or(&[])) };
}

/// `EXPORT void dstr_insert_dstr(struct dstr *dst, const size_t idx,
/// const struct dstr *str)`
///
/// # Safety
/// `dst`/`str` must be live dstrs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_insert_dstr(dst: *mut Dstr, idx: usize, str: *const Dstr) {
    // SAFETY: caller contract — both live for the call.
    unsafe { cd_dstr_insert_dstr(&mut *dst, idx, &*str) }
}

/// `EXPORT void dstr_insert_ch(struct dstr *dst, const size_t idx, const
/// char ch)`
///
/// # Safety
/// `dst` must be a live dstr.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_insert_ch(dst: *mut Dstr, idx: usize, ch: c_char) {
    // SAFETY: caller contract — `dst` live.
    unsafe { cd_dstr_insert_ch(&mut *dst, idx, ch as i8 as u8) }
}

/// `EXPORT void dstr_remove(struct dstr *dst, const size_t idx, const
/// size_t count)`
///
/// # Safety
/// `dst` must be a live dstr.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_remove(dst: *mut Dstr, idx: usize, count: usize) {
    // SAFETY: caller contract — `dst` live.
    unsafe { cd_dstr_remove(&mut *dst, idx, count) }
}

/// `EXPORT void dstr_replace(struct dstr *str, const char *find, const
/// char *replace)`
///
/// # Safety
/// `str` must be a live dstr; `find`/`replace` NULL or live
/// NUL-terminated strings.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_replace(str: *mut Dstr, find: *const c_char, replace: *const c_char) {
    let find = cstr(find);
    let replace = cstr(replace);
    // SAFETY: caller contract — all live for the call.
    unsafe { cd_dstr_replace(&mut *str, find.unwrap_or(&[]), replace.unwrap_or(&[])) };
}

/// `EXPORT void dstr_depad(struct dstr *dst)`
///
/// # Safety
/// `dst` must be a live dstr.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_depad(dst: *mut Dstr) {
    // SAFETY: caller contract — `dst` live.
    unsafe { cd_dstr_depad(&mut *dst) }
}

/// `EXPORT void dstr_left(struct dstr *dst, const struct dstr *str,
/// const size_t pos)`
///
/// # Safety
/// `dst`/`str` must be live dstrs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_left(dst: *mut Dstr, str: *const Dstr, pos: usize) {
    // SAFETY: caller contract — both live for the call.
    unsafe { cd_dstr_left(&mut *dst, &*str, pos) }
}

/// `EXPORT void dstr_mid(struct dstr *dst, const struct dstr *str, const
/// size_t start, const size_t count)`
///
/// # Safety
/// `dst`/`str` must be live dstrs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_mid(dst: *mut Dstr, str: *const Dstr, start: usize, count: usize) {
    // SAFETY: caller contract — both live for the call.
    unsafe { cd_dstr_mid(&mut *dst, &*str, start, count) }
}

/// `EXPORT void dstr_right(struct dstr *dst, const struct dstr *str,
/// const size_t pos)`
///
/// # Safety
/// `dst`/`str` must be live dstrs.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_right(dst: *mut Dstr, str: *const Dstr, pos: usize) {
    // SAFETY: caller contract — both live for the call.
    unsafe { cd_dstr_right(&mut *dst, &*str, pos) }
}

/// `EXPORT void dstr_from_mbs(struct dstr *dst, const char *mbstr)`
///
/// # Safety
/// `dst` must be a live dstr; `mbstr` NULL or a live NUL-terminated
/// string.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_from_mbs(dst: *mut Dstr, mbstr: *const c_char) {
    let bytes = cstr(mbstr);
    // SAFETY: caller contract — `dst` live; `mbstr` a live C string.
    unsafe { cd_dstr_from_mbs(&mut *dst, bytes.unwrap_or(&[])) };
}

/// `EXPORT char *dstr_to_mbs(const struct dstr *str)`
///
/// # Safety
/// `str` must be a live dstr. Returns a `bstrdup`-owned copy (the C
/// caller bfree's it) or NULL (C: empty/unconvertible).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_to_mbs(str: *const Dstr) -> *mut c_char {
    // SAFETY: caller contract — `str` live.
    match unsafe { cd_dstr_to_mbs(&*str) } {
        // SAFETY: fresh bmem copy handed to the C caller (bfree contract).
        Some(bytes) => unsafe { bstrdup_bytes(&bytes) },
        None => std::ptr::null_mut(),
    }
}

/// `EXPORT void dstr_from_wcs(struct dstr *dst, const wchar_t *wstr)`
///
/// # Safety
/// `dst` must be a live dstr; the wide string must be NULL or live and
/// NUL-terminated.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_from_wcs(dst: *mut Dstr, w: *const u32) {
    // SAFETY: caller contract — `w` a live wide string.
    let w = unsafe { wstr(w) };
    // SAFETY: caller contract — `dst` live.
    unsafe { cd_dstr_from_wcs(&mut *dst, w.unwrap_or(&[])) };
}

/// `EXPORT wchar_t *dstr_to_wcs(const struct dstr *str)`
///
/// # Safety
/// `str` must be a live dstr. Returns a `bmalloc`-owned wide buffer
/// (NUL-terminated, the C caller bfree's it) or NULL.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_to_wcs(str: *const Dstr) -> *mut u32 {
    // SAFETY: caller contract — `str` live.
    let Some(w) = (unsafe { cd_dstr_to_wcs(&*str) }) else {
        return std::ptr::null_mut();
    };
    // SAFETY: fresh bmem allocation (NUL-terminated), handed to the C caller (bfree contract).
    unsafe {
        let buf = bmalloc_raw((w.len() + 1) * std::mem::size_of::<u32>());
        std::ptr::copy_nonoverlapping(w.as_ptr(), buf.cast::<u32>(), w.len());
        *buf.cast::<u32>().add(w.len()) = 0;
        buf.cast::<u32>()
    }
}

/// `EXPORT void dstr_to_upper(struct dstr *str)`
///
/// # Safety
/// `str` must be a live dstr.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_to_upper(str: *mut Dstr) {
    // SAFETY: caller contract — `str` live.
    unsafe { cd_dstr_to_upper(&mut *str) }
}

/// `EXPORT void dstr_to_lower(struct dstr *str)`
///
/// # Safety
/// `str` must be a live dstr.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_to_lower(str: *mut Dstr) {
    // SAFETY: caller contract — `str` live.
    unsafe { cd_dstr_to_lower(&mut *str) }
}

/// `EXPORT void dstr_safe_printf(struct dstr *dst, const char
/// *format, const char *val1, const char *val2, const char *val3, const
/// char *val4)`
///
/// # Safety
/// `dst` must be a live dstr; `format`/`valN` NULL or live
/// NUL-terminated strings (C: a NULL `format` is a no-op, NULL values
/// skip the replacement).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dstr_safe_printf(
    dst: *mut Dstr,
    format: *const c_char,
    val1: *const c_char,
    val2: *const c_char,
    val3: *const c_char,
    val4: *const c_char,
) {
    let f = cstr(format);
    let v1 = cstr(val1);
    let v2 = cstr(val2);
    let v3 = cstr(val3);
    let v4 = cstr(val4);
    // SAFETY: caller contract — all live for the call.
    unsafe { cd_dstr_safe_printf(&mut *dst, f.unwrap_or(&[]), v1, v2, v3, v4) };
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CStr;

    fn dstr_bytes(d: &Dstr) -> Vec<u8> {
        // SAFETY: Dstr invariant — `array` holds `len` bytes + NUL.
        unsafe { std::slice::from_raw_parts(d.array, d.len + 1) }.to_vec()
    }

    /// dstr ops through the C ABI: copy/ncat/insert/replace/upper, the
    /// compare family, strlist round trip, the mbs/wcs conversions.
    #[test]
    fn dstr_bridge_roundtrip() {
        // SAFETY: stack dstrs, live for the whole test.
        unsafe {
            let mut d: Dstr = std::mem::zeroed();
            dstr_copy(&mut d, c"hello".as_ptr());
            dstr_ncat(&mut d, c" world".as_ptr(), 6);
            dstr_insert_ch(&mut d, 5, b'!' as c_char);
            assert_eq!(dstr_bytes(&d), b"hello! world\0");

            dstr_replace(&mut d, c"world".as_ptr(), c"Rust".as_ptr());
            assert_eq!(dstr_bytes(&d), b"hello! Rust\0");
            dstr_to_upper(&mut d);
            assert_eq!(dstr_bytes(&d), b"HELLO! RUST\0");

            assert_eq!(astrcmpi(c"aBc".as_ptr(), c"aBc".as_ptr()), 0);
            assert_ne!(astrcmpi(c"abc".as_ptr(), c"abd".as_ptr()), 0);
            assert_eq!(astrcmp_n(c"abc".as_ptr(), c"abd".as_ptr(), 1), 0);
            assert!(astrcmpi(c"abc".as_ptr(), std::ptr::null()) > 0); // "abc" > "" (the first char beats the NUL)
            assert!(astrcmpi(std::ptr::null(), c"abc".as_ptr()) < 0);

            let f = c"findme".as_ptr();
            let hit = astrstri(c"xxfindme".as_ptr(), f);
            assert_eq!(hit, c"xxfindme".as_ptr().add(2) as *mut c_char);
            assert!(astrstri(c"nope".as_ptr(), f).is_null());

            let list = strlist_split(c"a,b,,c".as_ptr(), b',' as c_char, true);
            assert!(!list.is_null());
            // table: "a", "b", "", "c", NULL
            assert_eq!(CStr::from_ptr(*list).to_bytes(), b"a");
            assert_eq!(CStr::from_ptr(*list.add(3)).to_bytes(), b"c");
            assert!((*list.add(4)).is_null());
            strlist_free(list);

            let w = dstr_to_wcs(&d);
            assert!(!w.is_null());
            let mut dw: Dstr = std::mem::zeroed();
            dstr_from_wcs(&mut dw, w);
            bfree_raw(w.cast::<u8>());
            assert_eq!(dstr_bytes(&dw), b"HELLO! RUST\0");
            obs_util::dstr::dstr_free(&mut dw);

            obs_util::dstr::dstr_free(&mut d);
        }
    }

    /// A zeroed dstr is empty (the C `struct dstr {0}` shape).
    #[test]
    fn dstr_bridge_zeroed() {
        // SAFETY: stack dstr.
        unsafe {
            let mut d: Dstr = std::mem::zeroed();
            dstr_ncat(&mut d, c"xy".as_ptr(), 2);
            assert_eq!(d.len, 2);
            dstr_remove(&mut d, 0, 1);
            assert_eq!(d.len, 1);
            obs_util::dstr::dstr_free(&mut d);
        }
    }
}
