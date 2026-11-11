//! `dstr` dynamic string (S-006; `libobs/util/dstr.{c,h}`).
//!
//! Layout-compatible `Dstr` (`{array, len, capacity}`, 3×usize) with
//! behaviour — **including capacity values** — matching the C reference,
//! because plugins read the fields directly. The variadic formatted
//! functions (R60–R62: `dstr_printf`/`dstr_catf`/`dstr_vprintf`/
//! `dstr_vcatf`) cannot be written in stable Rust and belong to the C
//! shim (primer §5.5); `dstr_safe_printf` (R63) is portable and is
//! implemented here.
//!
//! `len` excludes the terminating NUL; `capacity` is in bytes. "Free
//! when empty" leaves `array = NULL, len = 0, capacity = 0` (R04).

use crate::bmem::{bfree_raw, bmalloc_raw, brealloc_raw};

/// S-006 §3: the C `struct dstr`.
#[repr(C)]
#[derive(Debug)]
pub struct Dstr {
    /// NUL-terminated byte buffer (S-001 allocator) or null.
    pub array: *mut u8,
    /// Length in bytes, excluding the terminating NUL.
    pub len: usize,
    /// Capacity in bytes.
    pub capacity: usize,
}

/// S-006 (per S-011 §4): a borrowed, **not** NUL-terminated string
/// segment: `struct strref { const char *array; size_t len; }`.
#[repr(C)]
#[derive(Clone, Copy, Debug)]
pub struct StrRef {
    pub array: *const u8,
    pub len: usize,
}

impl Dstr {
    /// S-006-R04: `dstr_init` — all zero.
    #[must_use]
    pub const fn init() -> Self {
        Self {
            array: std::ptr::null_mut(),
            len: 0,
            capacity: 0,
        }
    }
}

impl Default for Dstr {
    fn default() -> Self {
        Self::init()
    }
}

impl Drop for Dstr {
    fn drop(&mut self) {
        if !self.array.is_null() {
            unsafe { bfree_raw(self.array) };
            self.array = std::ptr::null_mut();
            self.len = 0;
            self.capacity = 0;
        }
    }
}

impl std::ops::Deref for Dstr {
    type Target = [u8];
    fn deref(&self) -> &[u8] {
        if self.array.is_null() {
            &[]
        } else {
            unsafe { std::slice::from_raw_parts(self.array, self.len) }
        }
    }
}

/// The string contents as `&str` when valid UTF-8, else `None`.
#[must_use]
pub fn dstr_as_str(d: &Dstr) -> Option<&str> {
    std::str::from_utf8(d).ok()
}

// ── Allocation (R01–R05, R64/R65) ─────────────────────────────────────────

/// S-006-R01: `dstr_ensure_capacity(d, n)` — nothing if `n <= cap`;
/// else `new = cap ? cap * 2 : n`, `new = max(new, n)`, realloc to `new`.
pub fn dstr_ensure_capacity(d: &mut Dstr, n: usize) {
    if n <= d.capacity {
        return;
    }
    let new = if d.capacity != 0 { d.capacity * 2 } else { n };
    let new = new.max(n);
    if d.array.is_null() {
        d.array = unsafe { bmalloc_raw(new) };
    } else {
        d.array = unsafe { brealloc_raw(d.array, new) };
    }
    d.capacity = new;
}

/// S-006-R02: `dstr_reserve(d, c)` — nothing if `c == 0 || c <= len`
/// (**compares with `len`, not `cap`**); otherwise realloc to exactly `c`
/// (may shrink).
pub fn dstr_reserve(d: &mut Dstr, c: usize) {
    if c == 0 || c <= d.len {
        return;
    }
    if d.array.is_null() {
        d.array = unsafe { bmalloc_raw(c) };
    } else {
        d.array = unsafe { brealloc_raw(d.array, c) };
    }
    d.capacity = c;
}

/// S-006-R03: `dstr_resize(d, n)` — `n == 0` frees; else
/// `ensure_capacity(n + 1)`, `array[n] = 0`, `len = n`. (The C bytes in
/// `[old_len, n)` are uninitialised; we zero them — observably identical,
/// and keeps Miri honest.)
pub fn dstr_resize(d: &mut Dstr, n: usize) {
    if n == 0 {
        dstr_free(d);
        return;
    }
    let old = d.len;
    dstr_ensure_capacity(d, n + 1);
    if n > old {
        unsafe {
            std::ptr::write_bytes(d.array.add(old), 0, n - old);
        }
    }
    unsafe { *d.array.add(n) = 0 };
    d.len = n;
}

/// S-006-R04: `dstr_free(d)` — free; all fields zero.
pub fn dstr_free(d: &mut Dstr) {
    if !d.array.is_null() {
        unsafe { bfree_raw(d.array) };
    }
    d.array = std::ptr::null_mut();
    d.len = 0;
    d.capacity = 0;
}

/// S-006-R64: `dstr_array_free(d)` — frees only the buffer, keeps the
/// fields (caller resets them; C inline does exactly this).
pub fn dstr_array_free(d: &mut Dstr) {
    if !d.array.is_null() {
        unsafe { bfree_raw(d.array) };
        d.array = std::ptr::null_mut();
    }
}

/// S-006-R05: `dstr_init_move_array(d, s)` — takes ownership of `s`
/// (may be null); `len = strlen(s)`, `cap = len + 1` (even for null: 1).
pub fn dstr_init_move_array(d: &mut Dstr, s: *mut u8) {
    d.array = s;
    if s.is_null() {
        d.len = 0;
        d.capacity = 1;
    } else {
        d.len = cstr_len(s);
        d.capacity = d.len + 1;
    }
}

/// S-006-R05: `dstr_move_array(d, s)` — like `init_move_array`, freeing
/// `d` first.
pub fn dstr_move_array(d: &mut Dstr, s: *mut u8) {
    dstr_free(d);
    dstr_init_move_array(d, s);
}

/// S-006-R05: `dstr_init_move(d, src)` — struct copy, source reset.
pub fn dstr_init_move(d: &mut Dstr, src: &mut Dstr) {
    dstr_init(d);
    dstr_move(d, src);
}

/// S-006-R05: `dstr_move(d, src)` — struct copy, source reset. The
/// reset is field-wise: `*src = Dstr::init()` would run the old value's
/// Drop (freeing the buffer `d` now owns) before zeroing the fields.
pub fn dstr_move(d: &mut Dstr, src: &mut Dstr) {
    if std::ptr::eq(d as *const Dstr, src as *const Dstr) {
        return;
    }
    dstr_free(d);
    d.array = src.array;
    d.len = src.len;
    d.capacity = src.capacity;
    src.array = std::ptr::null_mut();
    src.len = 0;
    src.capacity = 0;
}

/// S-006-R05: `dstr_init(d)` — all zero.
pub fn dstr_init(d: &mut Dstr) {
    dstr_free(d);
}

/// S-006-R64: `dstr_init_copy(d, s)` — init, then `dstr_copy`.
pub fn dstr_init_copy(d: &mut Dstr, s: &[u8]) {
    dstr_init(d);
    dstr_copy(d, s);
}

/// S-006-R64: `dstr_init_copy_dstr(d, src)` — init, then `copy_dstr`.
pub fn dstr_init_copy_dstr(d: &mut Dstr, src: &Dstr) {
    dstr_init(d);
    dstr_copy_dstr(d, src);
}

/// S-006-R64: `dstr_init_copy_strref(d, ref)` — init, then
/// `dstr_copy_strref`.
pub fn dstr_init_copy_strref(d: &mut Dstr, ref_: &StrRef) {
    dstr_init(d);
    dstr_copy_strref(d, ref_);
}

/// S-006-R11/R64: `dstr_copy_dstr(d, src)` — free `d` first; if
/// `src.len != 0` allocate **exactly** `src.len + 1` and copy.
pub fn dstr_copy_dstr(d: &mut Dstr, src: &Dstr) {
    dstr_free(d);
    if src.len == 0 {
        return;
    }
    d.array = unsafe { bmalloc_raw(src.len + 1) };
    unsafe {
        std::ptr::copy_nonoverlapping(src.array, d.array, src.len + 1);
    }
    d.len = src.len;
    d.capacity = src.len + 1;
}

// ── Copy / concatenate / insert / remove (R10–R16) ───────────────────────

/// S-006-R10: `dstr_copy(d, s)` — empty `s` frees `d`; else
/// `ensure_capacity(strlen + 1)` (capacity is reused across copies),
/// copy including NUL, `len = strlen(s)`.
pub fn dstr_copy(d: &mut Dstr, s: &[u8]) {
    if s.is_empty() {
        dstr_free(d);
        return;
    }
    dstr_ensure_capacity(d, s.len() + 1);
    if d.array.is_null() {
        d.array = unsafe { bmalloc_raw(s.len() + 1) };
        d.capacity = s.len() + 1;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(s.as_ptr(), d.array, s.len());
        *d.array.add(s.len()) = 0;
    }
    d.len = s.len();
}

/// S-006-R10 (strref variant): same with a borrowed segment.
pub fn dstr_copy_strref(d: &mut Dstr, ref_: &StrRef) {
    let bytes = if ref_.array.is_null() || ref_.len == 0 {
        &[][..]
    } else {
        unsafe { std::slice::from_raw_parts(ref_.array, ref_.len) }
    };
    dstr_copy(d, bytes);
}

/// S-006-R12: `dstr_ncopy(d, s, n)` — free if allocated; `n == 0` stays
/// empty; else copy exactly `n` bytes, terminate, `cap = n + 1`.
/// (Q-1: the C reads `n + 1` bytes; we read exactly `n` — observably
/// identical for valid input.)
pub fn dstr_ncopy(d: &mut Dstr, s: &[u8], n: usize) {
    dstr_free(d);
    if n == 0 {
        return;
    }
    let n = n.min(s.len());
    d.array = unsafe { bmalloc_raw(n + 1) };
    unsafe {
        std::ptr::copy_nonoverlapping(s.as_ptr(), d.array, n);
        *d.array.add(n) = 0;
    }
    d.len = n;
    d.capacity = n + 1;
}

/// S-006-R12: `dstr_ncopy_dstr(d, src, n)` — `m = min(n, src.len)`.
pub fn dstr_ncopy_dstr(d: &mut Dstr, src: &Dstr, n: usize) {
    dstr_free(d);
    let m = n.min(src.len);
    if m == 0 {
        return;
    }
    d.array = unsafe { bmalloc_raw(m + 1) };
    unsafe {
        std::ptr::copy_nonoverlapping(src.array, d.array, m);
        *d.array.add(m) = 0;
    }
    d.len = m;
    d.capacity = m + 1;
}

/// S-006-R13: `dstr_cat(d, s)` — no-op if empty; else
/// `ncat(d, s, strlen(s))`.
pub fn dstr_cat(d: &mut Dstr, s: &[u8]) {
    dstr_ncat(d, s, s.len());
}

/// S-006-R13 (strref variant).
pub fn dstr_cat_strref(d: &mut Dstr, ref_: &StrRef) {
    if ref_.array.is_null() || ref_.len == 0 {
        return;
    }
    let bytes = unsafe { std::slice::from_raw_parts(ref_.array, ref_.len) };
    dstr_ncat(d, bytes, bytes.len());
}

/// S-006-R66: `dstr_cat_quoted(d, s)` — wrap `s` in double quotes.
pub fn dstr_cat_quoted(d: &mut Dstr, s: &[u8]) {
    dstr_cat_ch(d, b'"');
    dstr_cat(d, s);
    dstr_cat_ch(d, b'"');
}

/// S-006-R13: `dstr_ncat(d, s, n)` — no-op if `s` empty or `n == 0`;
/// else `ensure_capacity(len + n + 1)`, append exactly `n` bytes,
/// terminate.
pub fn dstr_ncat(d: &mut Dstr, s: &[u8], n: usize) {
    if s.is_empty() || n == 0 {
        return;
    }
    let n = n.min(s.len());
    dstr_ensure_capacity(d, d.len + n + 1);
    if d.array.is_null() {
        d.array = unsafe { bmalloc_raw(d.len + n + 1) };
        d.capacity = d.len + n + 1;
    }
    unsafe {
        std::ptr::copy_nonoverlapping(s.as_ptr(), d.array.add(d.len), n);
        *d.array.add(d.len + n) = 0;
    }
    d.len += n;
}

/// S-006-R13: `dstr_cat_dstr(d, src)` — no-op if `src.len == 0`; else
/// append `src.len` bytes.
pub fn dstr_cat_dstr(d: &mut Dstr, src: &Dstr) {
    if src.len == 0 || src.array.is_null() {
        return;
    }
    dstr_ncat(
        d,
        unsafe { std::slice::from_raw_parts(src.array, src.len) },
        src.len,
    );
}

/// S-006-R13: `dstr_ncat_dstr(d, src, n)` — append `min(n, src.len)`.
pub fn dstr_ncat_dstr(d: &mut Dstr, src: &Dstr, n: usize) {
    if src.array.is_null() || src.len == 0 || n == 0 {
        return;
    }
    dstr_ncat(
        d,
        unsafe { std::slice::from_raw_parts(src.array, src.len) },
        n.min(src.len),
    );
}

/// S-006-R14: `dstr_cat_ch(d, c)` — `ensure_capacity(len + 2)`, append
/// `c` (a `\0` is a real byte, `len` grows), terminate.
pub fn dstr_cat_ch(d: &mut Dstr, c: u8) {
    dstr_ensure_capacity(d, d.len + 2);
    if d.array.is_null() {
        d.array = unsafe { bmalloc_raw(d.len + 2) };
        d.capacity = d.len + 2;
    }
    unsafe {
        *d.array.add(d.len) = c;
        *d.array.add(d.len + 1) = 0;
    }
    d.len += 1;
}

/// S-006-R15: `dstr_insert(d, idx, s)` — no-op if empty; `idx == len`
/// → `cat`; else shift the tail (with NUL) up by `n`, copy `s`.
pub fn dstr_insert(d: &mut Dstr, idx: usize, s: &[u8]) {
    if s.is_empty() {
        return;
    }
    if idx == d.len {
        dstr_cat(d, s);
        return;
    }
    let n = s.len();
    dstr_ensure_capacity(d, d.len + n + 1);
    if d.array.is_null() {
        d.array = unsafe { bmalloc_raw(d.len + n + 1) };
        d.capacity = d.len + n + 1;
        unsafe { std::ptr::write_bytes(d.array, 0, d.len + n + 1) };
    }
    unsafe {
        std::ptr::copy(d.array.add(idx), d.array.add(idx + n), d.len - idx + 1);
        std::ptr::copy_nonoverlapping(s.as_ptr(), d.array.add(idx), n);
    }
    d.len += n;
}

/// S-006-R15: `dstr_insert_dstr(d, idx, src)` — no-op if `src.len == 0`.
pub fn dstr_insert_dstr(d: &mut Dstr, idx: usize, src: &Dstr) {
    if src.len == 0 || src.array.is_null() {
        return;
    }
    dstr_insert(d, idx, unsafe {
        std::slice::from_raw_parts(src.array, src.len)
    });
}

/// S-006-R15: `dstr_insert_ch(d, idx, c)` — `idx == len` → `cat_ch`;
/// else shift the tail up one, store `c`.
pub fn dstr_insert_ch(d: &mut Dstr, idx: usize, c: u8) {
    if idx == d.len {
        dstr_cat_ch(d, c);
        return;
    }
    dstr_ensure_capacity(d, d.len + 2);
    if d.array.is_null() {
        d.array = unsafe { bmalloc_raw(d.len + 2) };
        d.capacity = d.len + 2;
        unsafe { std::ptr::write_bytes(d.array, 0, d.len + 2) };
    }
    unsafe {
        std::ptr::copy(d.array.add(idx), d.array.add(idx + 1), d.len - idx + 1);
        *d.array.add(idx) = c;
    }
    d.len += 1;
}

/// S-006-R16: `dstr_remove(d, idx, count)` — `count == 0` no-op;
/// `count == len` → **free** (not just empty); else close the gap,
/// capacity unchanged.
pub fn dstr_remove(d: &mut Dstr, idx: usize, count: usize) {
    if count == 0 {
        return;
    }
    if count >= d.len {
        dstr_free(d);
        return;
    }
    let tail = d.len - idx - count;
    unsafe {
        std::ptr::copy(d.array.add(idx + count), d.array.add(idx), tail + 1);
    }
    d.len -= count;
}

// ── Search / compare (R20, R30–R34) ──────────────────────────────────────

fn cstr_len(s: *const u8) -> usize {
    let mut n = 0;
    unsafe {
        while *s.add(n) != 0 {
            n += 1;
        }
    }
    n
}

fn ascii_toupper(b: u8) -> u8 {
    if b.is_ascii_lowercase() { b - 32 } else { b }
}

/// S-006-R31: `astrcmpi(a, b)` — byte-wise after `toupper` (C locale;
/// ASCII specified). NULL ≡ `""`. Only the sign is specified.
#[must_use]
pub fn astrcmpi(a: Option<&[u8]>, b: Option<&[u8]>) -> i32 {
    let a = a.unwrap_or(&[]);
    let b = b.unwrap_or(&[]);
    for (x, y) in a.iter().zip(b.iter()) {
        let x = ascii_toupper(*x);
        let y = ascii_toupper(*y);
        if x != y {
            return if x < y { -1 } else { 1 };
        }
    }
    if a.len() < b.len() {
        -1
    } else if a.len() > b.len() {
        1
    } else {
        0
    }
}

/// S-006-R31: `astrcmp_n(a, b, n)` — at most `n` bytes; `n == 0` → 0.
#[must_use]
pub fn astrcmp_n(a: Option<&[u8]>, b: Option<&[u8]>, n: usize) -> i32 {
    if n == 0 {
        return 0;
    }
    let a = a.unwrap_or(&[]);
    let b = b.unwrap_or(&[]);
    for i in 0..n.min(a.len().min(b.len())) {
        let x = a[i] as i8;
        let y = b[i] as i8;
        if x != y {
            return if x < y { -1 } else { 1 };
        }
    }
    // hit NUL or the n limit on equal prefixes
    let al = a.len().min(n);
    let bl = b.len().min(n);
    if al < bl {
        -1
    } else if al > bl {
        1
    } else {
        0
    }
}

/// S-006-R31: `astrcmpi_n(a, b, n)` — case-insensitive, at most `n`.
#[must_use]
pub fn astrcmpi_n(a: Option<&[u8]>, b: Option<&[u8]>, n: usize) -> i32 {
    if n == 0 {
        return 0;
    }
    let a = a.unwrap_or(&[]);
    let b = b.unwrap_or(&[]);
    for i in 0..n.min(a.len().min(b.len())) {
        let x = ascii_toupper(a[i]);
        let y = ascii_toupper(b[i]);
        if x != y {
            return if x < y { -1 } else { 1 };
        }
    }
    let al = a.len().min(n);
    let bl = b.len().min(n);
    if al < bl {
        -1
    } else if al > bl {
        1
    } else {
        0
    }
}

/// S-006-R33: `astrstri(str, find)` — first offset where
/// `astrcmpi_n(p, find, strlen(find)) == 0`; NULL-arguments → NULL;
/// empty needle matches at offset 0.
#[must_use]
pub fn astrstri(str: Option<&[u8]>, find: Option<&[u8]>) -> Option<usize> {
    let str = str?;
    let find = find?;
    if find.is_empty() {
        return Some(0);
    }
    if str.len() < find.len() {
        return None;
    }
    for i in 0..=str.len() - find.len() {
        if astrcmpi_n(Some(&str[i..]), Some(find), find.len()) == 0 {
            return Some(i);
        }
    }
    None
}

/// S-006-R20: `dstr_find(d, s)` — `strstr(d->array, s)`; **empty
/// needle → offset 0**. (Q-5: the C crashes on a NULL array; the Rust
/// port returns `None` — the required behaviour is *no crash*.)
#[must_use]
pub fn dstr_find(d: &Dstr, s: &[u8]) -> Option<usize> {
    if d.array.is_null() {
        return None;
    }
    if s.is_empty() {
        return Some(0);
    }
    let buf = &d[..];
    if buf.len() < s.len() {
        return None;
    }
    for i in 0..=buf.len() - s.len() {
        if &buf[i..i + s.len()] == s {
            return Some(i);
        }
    }
    None
}

/// S-006-R20: `dstr_find_i(d, s)` — case-insensitive (NULL-safe).
#[must_use]
pub fn dstr_find_i(d: &Dstr, s: &[u8]) -> Option<usize> {
    astrstri(Some(d), Some(s))
}

/// S-006-R32: `dstr_cmp(d, s)` — `strcmp(d->array ?: "", s ?: "")`.
#[must_use]
pub fn dstr_cmp(d: &Dstr, s: Option<&[u8]>) -> i32 {
    astrcmp_n(Some(d), s, usize::MAX)
}

/// S-006-R32: `dstr_cmpi(d, s)` — case-insensitive.
#[must_use]
pub fn dstr_cmpi(d: &Dstr, s: Option<&[u8]>) -> i32 {
    astrcmpi(Some(d), s)
}

/// S-006-R32: `dstr_ncmp(d, s, n)`.
#[must_use]
pub fn dstr_ncmp(d: &Dstr, s: Option<&[u8]>, n: usize) -> i32 {
    astrcmp_n(Some(d), s, n)
}

/// S-006-R32: `dstr_ncmpi(d, s, n)`.
#[must_use]
pub fn dstr_ncmpi(d: &Dstr, s: Option<&[u8]>, n: usize) -> i32 {
    astrcmpi_n(Some(d), s, n)
}

/// S-006-R34: `dstr_is_empty` — NULL array, `len == 0`, or `*array == 0`.
#[must_use]
pub fn dstr_is_empty(d: &Dstr) -> bool {
    d.array.is_null() || d.len == 0 || unsafe { *d.array == 0 }
}

/// S-006-R34: `dstr_end(d)` — 0 if empty, else `array[len - 1]`.
#[must_use]
pub fn dstr_end(d: &Dstr) -> u8 {
    if dstr_is_empty(d) {
        0
    } else {
        unsafe { *d.array.add(d.len - 1) }
    }
}

// ── Replace / depad / substrings / case (R21–R24) ────────────────────────

/// S-006-R21/Q-6: `dstr_replace(d, find, repl)` — no-op if `d` is empty
/// **or `find` is empty** (Q-6: the C never terminates); `repl` empty
/// deletes. Scans left to right, resumes **after the inserted text**
/// (replacement text is never rescanned; matches do not overlap).
/// Capacity grows only when the result is longer; an empty result keeps
/// the array (vector 05: `abcabc`/`abc` → `""`).
pub fn dstr_replace(d: &mut Dstr, find: &[u8], repl: &[u8]) {
    if dstr_is_empty(d) || find.is_empty() {
        return;
    }
    let mut pos = 0usize;
    loop {
        let buf = d.as_ref();
        let mut found: Option<usize> = None;
        if buf.len() >= pos && buf.len() - pos >= find.len() {
            for i in pos..=buf.len() - find.len() {
                if &buf[i..i + find.len()] == find {
                    found = Some(i);
                    break;
                }
            }
        }
        let Some(idx) = found else { break };
        do_replace_at(d, idx, find, repl);
        pos = idx + repl.len();
    }
}

fn do_replace_at(d: &mut Dstr, idx: usize, find: &[u8], repl: &[u8]) {
    // the new length is independent of idx: old - find + repl
    let new_len = d.len - find.len() + repl.len();
    if new_len + 1 > d.capacity {
        dstr_ensure_capacity(d, new_len + 1);
    }
    let tail = d.len - idx - find.len();
    unsafe {
        std::ptr::copy(
            d.array.add(idx + find.len()),
            d.array.add(idx + repl.len()),
            tail + 1,
        );
        std::ptr::copy_nonoverlapping(repl.as_ptr(), d.array.add(idx), repl.len());
    }
    d.len = new_len;
}

/// S-006-R22: `strdepad(s)` — removes leading and trailing
/// `{space, tab, LF, CR}` in place; null → null; returns the start.
#[must_use]
pub fn strdepad(s: Option<&mut [u8]>) -> Option<&mut [u8]> {
    let s = s?;
    let is_pad = |b: u8| b == b' ' || b == b'\t' || b == b'\n' || b == b'\r';
    let mut start = 0;
    while start < s.len() && is_pad(s[start]) {
        start += 1;
    }
    let mut end = s.len();
    while end > start && is_pad(s[end - 1]) {
        end -= 1;
    }
    Some(&mut s[start..end])
}

/// S-006-R22: `dstr_depad(d)` — depad in place; `len = strlen`; an empty
/// result frees `d`.
pub fn dstr_depad(d: &mut Dstr) {
    if d.array.is_null() {
        return;
    }
    let is_pad = |b: u8| b == b' ' || b == b'\t' || b == b'\n' || b == b'\r';
    let buf = unsafe { std::slice::from_raw_parts_mut(d.array, d.len) };
    let mut start = 0;
    while start < buf.len() && is_pad(buf[start]) {
        start += 1;
    }
    let mut end = buf.len();
    while end > start && is_pad(buf[end - 1]) {
        end -= 1;
    }
    if end == start {
        dstr_free(d);
    } else {
        if start > 0 {
            unsafe {
                std::ptr::copy(d.array.add(start), d.array, end - start);
            }
        }
        unsafe { *d.array.add(end - start) = 0 };
        d.len = end - start;
    }
}

/// S-006-R23: `dstr_left(d, src, pos)` — `resize(d, pos)`, then copy
/// the first `pos` bytes of `src`. **Non-aliasing** (Rust references):
/// the C self-aliasing call `dstr_left(d, d, pos)` is
/// [`dstr_truncate`] here.
pub fn dstr_left(d: &mut Dstr, src: &Dstr, pos: usize) {
    let pos = pos.min(src.len);
    dstr_resize(d, pos);
    if pos > 0 && !std::ptr::eq(d, src) && !src.array.is_null() {
        unsafe {
            std::ptr::copy_nonoverlapping(src.array, d.array, pos);
        }
    }
}

/// S-006-R23: the self-aliasing case of `dstr_left` — truncate `d` to
/// `pos` bytes (`resize`). C callers may pass `d == src` to
/// `dstr_left`; Rust's borrow model expresses that as this function.
pub fn dstr_truncate(d: &mut Dstr, pos: usize) {
    dstr_resize(d, pos);
}

/// S-006-R23: `dstr_mid(d, src, start, count)` — copy `src[start..]`
/// truncated to `count` bytes; `count == 0` → empty.
pub fn dstr_mid(d: &mut Dstr, src: &Dstr, start: usize, count: usize) {
    let start = start.min(src.len);
    let count = count.min(src.len - start);
    dstr_free(d);
    if count == 0 || src.array.is_null() {
        return;
    }
    d.array = unsafe { bmalloc_raw(count + 1) };
    unsafe {
        std::ptr::copy_nonoverlapping(src.array.add(start), d.array, count);
        *d.array.add(count) = 0;
    }
    d.len = count;
    d.capacity = count + 1;
}

/// S-006-R23: `dstr_right(d, src, pos)` — the last `src.len - pos`
/// bytes of `src`.
pub fn dstr_right(d: &mut Dstr, src: &Dstr, pos: usize) {
    let pos = pos.min(src.len);
    dstr_mid(d, src, pos, src.len - pos);
}

/// S-006-R24: `dstr_to_upper(d)` — ASCII case folding in place (C
/// locale; non-ASCII bytes untouched, Q-7). No-op if empty.
pub fn dstr_to_upper(d: &mut Dstr) {
    if dstr_is_empty(d) {
        return;
    }
    let buf = unsafe { std::slice::from_raw_parts_mut(d.array, d.len) };
    for b in buf.iter_mut() {
        *b = ascii_toupper(*b);
    }
}

/// S-006-R24: `dstr_to_lower(d)` — ASCII case folding in place.
pub fn dstr_to_lower(d: &mut Dstr) {
    if dstr_is_empty(d) {
        return;
    }
    let buf = unsafe { std::slice::from_raw_parts_mut(d.array, d.len) };
    for b in buf.iter_mut() {
        if *b >= b'A' && *b <= b'Z' {
            *b += 32;
        }
    }
}

// ── Wide conversions (R25, Q-2) ──────────────────────────────────────────

/// S-006-R25: `dstr_from_mbs(d, s)` — free `d`; on Linux mbs is UTF-8,
/// so this is a copy. **`capacity` is left 0** (Q-2).
pub fn dstr_from_mbs(d: &mut Dstr, s: &[u8]) {
    dstr_free(d);
    if s.is_empty() {
        return;
    }
    d.array = unsafe { bmalloc_raw(s.len() + 1) };
    unsafe {
        std::ptr::copy_nonoverlapping(s.as_ptr(), d.array, s.len());
        *d.array.add(s.len()) = 0;
    }
    d.len = s.len();
    d.capacity = 0; // Q-2
}

/// S-006-R25: `dstr_to_mbs(d)` — a new caller-owned (NUL-terminated)
/// string; `None` if the content is not valid UTF-8 (Linux mbs == UTF-8).
#[must_use]
pub fn dstr_to_mbs(d: &Dstr) -> Option<Vec<u8>> {
    let buf = d.as_ref();
    if std::str::from_utf8(buf).is_err() {
        return None;
    }
    let mut out = buf.to_vec();
    out.push(0);
    Some(out)
}

/// S-006-R25: `dstr_to_wcs(d)` — a new caller-owned wide string
/// (`wchar_t` is 32-bit on Linux); `None` on invalid UTF-8.
#[must_use]
pub fn dstr_to_wcs(d: &Dstr) -> Option<Vec<u32>> {
    let s = std::str::from_utf8(d.as_ref()).ok()?;
    Some(s.chars().map(|c| c as u32).collect())
}

/// S-006-R25: `dstr_from_wcs(d, w)` — free `d`; encode `w` as UTF-8.
/// Empty result stays empty. `capacity` left 0 (Q-2, symmetric with
/// `from_mbs`).
pub fn dstr_from_wcs(d: &mut Dstr, w: &[u32]) {
    dstr_free(d);
    let s: String = w.iter().filter_map(|&c| char::from_u32(c)).collect();
    if s.is_empty() {
        return;
    }
    let bytes = s.as_bytes();
    d.array = unsafe { bmalloc_raw(bytes.len() + 1) };
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), d.array, bytes.len());
        *d.array.add(bytes.len()) = 0;
    }
    d.len = bytes.len();
    d.capacity = 0; // Q-2
}

// ── Formatted (R63) ──────────────────────────────────────────────────────

/// S-006-R63: `dstr_safe_printf(d, fmt, v1..v4)` — `copy(d, fmt)`, then
/// replace `$1`…`$4` with the non-NULL values **sequentially** (Q-4:
/// substituted text is rescanned by later steps). Missing values leave
/// `$i` untouched.
pub fn dstr_safe_printf(
    d: &mut Dstr,
    fmt: &[u8],
    v1: Option<&[u8]>,
    v2: Option<&[u8]>,
    v3: Option<&[u8]>,
    v4: Option<&[u8]>,
) {
    dstr_copy(d, fmt);
    for (i, v) in [(b'1', v1), (b'2', v2), (b'3', v3), (b'4', v4)] {
        if let Some(v) = v {
            dstr_replace(d, &[b'$', i], v);
        }
    }
}

// ── strlist_split (R40) ──────────────────────────────────────────────────

/// S-006-R40: the result of `strlist_split` — one S-001 block holding a
/// table of `count + 1` pointers (last = NULL) followed by the
/// NUL-terminated segment strings.
pub struct StrList {
    /// Pointer table (`count + 1` entries, last NULL).
    pub ptrs: *mut *mut u8,
    /// The whole block (for `strlist_free`).
    pub block: *mut u8,
    pub count: usize,
}

/// S-006-R40: `strlist_split(s, ch, include_empty)` — `s == NULL` →
/// NULL. Segments kept if non-empty or `include_empty`.
#[must_use]
pub fn strlist_split(s: Option<&[u8]>, ch: u8, include_empty: bool) -> Option<StrList> {
    let s = s?;
    // first pass: count kept segments
    let mut count = 0usize;
    let mut seg_len = 0usize;
    for &b in s.iter() {
        if b == ch {
            if seg_len > 0 || include_empty {
                count += 1;
            }
            seg_len = 0;
        } else {
            seg_len += 1;
        }
    }
    if seg_len > 0 || include_empty {
        count += 1; // trailing segment (possibly empty)
    }
    let bytes: Vec<u8> = s.iter().map(|&b| if b == ch { 0 } else { b }).collect();
    // block: (count + 1) pointers, then the segment bytes + NULs.
    // Allocate an upper bound (every byte + a NUL); the table+strings
    // layout is what the C block contract fixes, not the exact size.
    let ptr_size = std::mem::size_of::<*mut u8>();
    let total = s.len() + count + 1;
    let block_size = (count + 1) * ptr_size + total;
    let block = unsafe { bmalloc_raw(block_size) };
    let ptrs = block as *mut *mut u8;
    let data = unsafe { block.add((count + 1) * ptr_size) };
    unsafe {
        std::ptr::write_bytes(data, 0, total);
    }
    let mut i = 0usize;
    let mut seg_start = 0usize;
    let mut data_pos = 0usize;
    let emit = |i: &mut usize, data_pos: &mut usize, seg_start: usize, len: usize| {
        unsafe {
            *ptrs.add(*i) = data.add(*data_pos);
            if len > 0 {
                std::ptr::copy_nonoverlapping(s.as_ptr().add(seg_start), data.add(*data_pos), len);
            }
            *data.add(*data_pos + len) = 0;
        }
        *i += 1;
        *data_pos += len + 1;
    };
    for (idx, &b) in bytes.iter().enumerate() {
        if b != 0 {
            continue;
        }
        let len = idx - seg_start;
        if len > 0 || include_empty {
            emit(&mut i, &mut data_pos, seg_start, len);
        }
        seg_start = idx + 1;
    }
    // trailing segment
    let len = s.len() - seg_start;
    if len > 0 || include_empty {
        emit(&mut i, &mut data_pos, seg_start, len);
    }
    unsafe {
        *ptrs.add(i) = std::ptr::null_mut();
    }
    Some(StrList {
        ptrs,
        block,
        count: i,
    })
}

/// S-006-R40: the segment at `i` (borrowed from the block).
/// # Safety
/// `i` must be `< list.count` and `list` must be alive.
#[must_use]
pub unsafe fn strlist_get(list: &StrList, i: usize) -> &str {
    let p = unsafe { *list.ptrs.add(i) };
    let n = cstr_len(p as *const u8);
    std::str::from_utf8(unsafe { std::slice::from_raw_parts(p, n) }).unwrap_or("")
}

/// S-006-R40: `strlist_free` — one `bfree` of the whole block.
impl Drop for StrList {
    fn drop(&mut self) {
        if !self.block.is_null() {
            unsafe { bfree_raw(self.block) };
            self.block = std::ptr::null_mut();
            self.ptrs = std::ptr::null_mut();
            self.count = 0;
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn mk(s: &str) -> Dstr {
        let mut d = Dstr::init();
        dstr_copy(&mut d, s.as_bytes());
        d
    }
    fn str(d: &Dstr) -> String {
        String::from_utf8(d.to_vec()).unwrap()
    }

    #[test]
    fn layout_is_c_compatible() {
        assert_eq!(
            std::mem::size_of::<Dstr>(),
            3 * std::mem::size_of::<usize>()
        );
        assert_eq!(std::mem::offset_of!(Dstr, array), 0);
        assert_eq!(
            std::mem::offset_of!(Dstr, len),
            std::mem::size_of::<usize>()
        );
        assert_eq!(
            std::mem::offset_of!(Dstr, capacity),
            2 * std::mem::size_of::<usize>()
        );
        assert_eq!(
            std::mem::size_of::<StrRef>(),
            2 * std::mem::size_of::<usize>()
        );
    }

    #[test]
    fn r01_growth_vector_02() {
        let mut d = Dstr::init();
        dstr_cat(&mut d, b"a");
        assert_eq!(d.capacity, 2, "cat a on empty -> cap 2");
        dstr_cat(&mut d, b"b");
        assert_eq!(d.capacity, 4, "cat b -> cap 4");
        dstr_cat(&mut d, b"cdefgh");
        assert_eq!(d.capacity, 9, "needs 9 -> cap 9 (max rule)");
        assert_eq!(str(&d), "abcdefgh");
        assert_eq!(d.len, 8);
    }

    #[test]
    fn r01_ensure_noop_when_within() {
        let mut d = mk("abc"); // cap 4
        dstr_ensure_capacity(&mut d, 4);
        assert_eq!(d.capacity, 4);
    }

    #[test]
    fn r02_reserve_compares_len_not_cap() {
        let mut d = mk("abcd"); // len 4, cap 5
        dstr_reserve(&mut d, 4); // c <= len -> nothing
        assert_eq!(d.capacity, 5);
        dstr_reserve(&mut d, 8); // exact
        assert_eq!(d.capacity, 8);
        dstr_reserve(&mut d, 2); // c <= len -> nothing (even though 2 < cap)
        assert_eq!(d.capacity, 8);
    }

    #[test]
    fn r03_resize() {
        let mut d = Dstr::init();
        dstr_resize(&mut d, 5);
        assert_eq!(d.len, 5);
        assert_eq!(d.capacity, 6);
        assert_eq!(unsafe { *d.array.add(5) }, 0);
        dstr_resize(&mut d, 0);
        assert!(dstr_is_empty(&d));
        assert_eq!(d.capacity, 0);
    }

    #[test]
    fn r04_init_free() {
        let mut d = mk("hello");
        dstr_free(&mut d);
        assert!(d.array.is_null());
        assert_eq!(d.len, 0);
        assert_eq!(d.capacity, 0);
    }

    #[test]
    fn r05_move_array_caps() {
        let mut d = Dstr::init();
        dstr_init_move_array(&mut d, std::ptr::null_mut());
        assert!(d.array.is_null());
        assert_eq!(d.capacity, 1, "null -> cap 1 (R05)");
        // owned buffer "xy" (3 bytes) allocated through the S-001 allocator
        let p = unsafe { bmalloc_raw(3) };
        unsafe {
            *p = b'x';
            *p.add(1) = b'y';
            *p.add(2) = 0;
        }
        let mut d2 = Dstr::init();
        dstr_init_move_array(&mut d2, p);
        assert_eq!(str(&d2), "xy");
        assert_eq!(d2.capacity, 3);
        // move between dstrs resets the source
        let mut a = mk("abc");
        let mut b = Dstr::init();
        dstr_init_move(&mut b, &mut a);
        assert_eq!(str(&b), "abc");
        assert!(dstr_is_empty(&a));
    }

    #[test]
    fn r10_copy_reuses_capacity() {
        let mut d = Dstr::init();
        dstr_copy(&mut d, b"hello");
        assert_eq!(d.capacity, 6);
        dstr_copy(&mut d, b"hi");
        assert_eq!(d.capacity, 6, "capacity kept across copies");
        assert_eq!(str(&d), "hi");
        dstr_copy(&mut d, b"");
        assert!(dstr_is_empty(&d), "empty copy frees");
    }

    #[test]
    fn r11_copy_dstr_exact_alloc() {
        let a = mk("hello world");
        let mut b = mk("x");
        dstr_copy_dstr(&mut b, &a);
        assert_eq!(b.capacity, 12, "exactly len + 1");
        assert_eq!(str(&b), "hello world");
        let mut c = Dstr::init();
        dstr_copy_dstr(&mut c, &a);
        assert_eq!(str(&c), "hello world");
        let empty = Dstr::init();
        let mut e = Dstr::init();
        dstr_copy_dstr(&mut e, &empty);
        assert!(dstr_is_empty(&e));
    }

    #[test]
    fn r12_ncopy() {
        let mut d = Dstr::init();
        dstr_ncopy(&mut d, b"abcdef", 3);
        assert_eq!(str(&d), "abc");
        assert_eq!(d.capacity, 4);
        dstr_ncopy(&mut d, b"", 5);
        assert!(
            dstr_is_empty(&d),
            "n > len of s clamps to 0 -> stays empty? no: n>0, s empty"
        );
        let mut e = Dstr::init();
        dstr_ncopy(&mut e, b"", 0);
        assert!(dstr_is_empty(&e));
    }

    #[test]
    fn r13_cat_families() {
        let mut d = Dstr::init();
        dstr_cat(&mut d, b"");
        assert!(dstr_is_empty(&d));
        dstr_ncat(&mut d, b"ab\0cd", 2);
        assert_eq!(str(&d), "ab");
        let src = mk("xy");
        dstr_cat_dstr(&mut d, &src);
        assert_eq!(str(&d), "abxy");
        dstr_ncat_dstr(&mut d, &src, 1);
        assert_eq!(str(&d), "abxyx");
        dstr_ncat_dstr(&mut d, &src, 99);
        assert_eq!(str(&d), "abxyxxy");
    }

    #[test]
    fn r14_cat_ch_appends_nul() {
        let mut d = mk("ab");
        dstr_cat_ch(&mut d, b'\0');
        assert_eq!(d.len, 3);
        assert_eq!(d[2], 0);
        dstr_cat_ch(&mut d, b'c');
        assert_eq!(d.len, 4);
    }

    #[test]
    fn r15_insert() {
        let mut d = mk("abcd");
        dstr_insert(&mut d, 2, b"XY");
        assert_eq!(str(&d), "abXYcd");
        dstr_insert(&mut d, 4, b"");
        assert_eq!(str(&d), "abXYcd");
        dstr_insert_ch(&mut d, 0, b'Z');
        assert_eq!(str(&d), "ZabXYcd");
        let n = d.len;
        dstr_insert_ch(&mut d, n, b'!');
        assert_eq!(str(&d), "ZabXYcd!");
        let s = mk("Q");
        dstr_insert_dstr(&mut d, 1, &s);
        assert_eq!(str(&d), "ZQabXYcd!");
    }

    #[test]
    fn r16_remove() {
        let mut d = mk("abcdef");
        dstr_remove(&mut d, 2, 0);
        assert_eq!(str(&d), "abcdef");
        dstr_remove(&mut d, 2, 2);
        assert_eq!(str(&d), "abef");
        let cap_before = d.capacity;
        dstr_remove(&mut d, 0, 4);
        assert!(dstr_is_empty(&d), "count == len frees");
        assert_eq!(d.capacity, 0);
        let mut e = mk("abcdef");
        e.capacity = cap_before;
        dstr_remove(&mut e, 1, 3);
        assert_eq!(e.capacity, cap_before, "partial remove keeps capacity");
        assert_eq!(str(&e), "aef");
    }

    #[test]
    fn r20_find() {
        let d = mk("hello world");
        assert_eq!(dstr_find(&d, b"world"), Some(6));
        assert_eq!(dstr_find(&d, b"llo"), Some(2));
        assert_eq!(dstr_find(&d, b""), Some(0), "empty needle -> 0");
        assert_eq!(dstr_find(&d, b"xyz"), None);
        assert_eq!(dstr_find_i(&d, b"WORLD"), Some(6));
        let empty = Dstr::init();
        assert_eq!(dstr_find(&empty, b""), None, "Q-5: no crash, not found");
    }

    #[test]
    fn r30_31_compare() {
        assert_eq!(astrcmpi(Some(b"abc"), Some(b"abc")), 0);
        assert_eq!(astrcmpi(Some(b"abc"), Some(b"ABC")), 0);
        assert!(astrcmpi(Some(b"abc"), Some(b"abd")) < 0);
        assert!(astrcmpi(Some(b"abd"), Some(b"abc")) > 0);
        assert!(astrcmpi(Some(b"abc"), Some(b"abcd")) < 0);
        assert_eq!(astrcmpi(None, Some(b"")), 0, "NULL == empty");
        assert_eq!(astrcmp_n(Some(b"abc"), Some(b"abd"), 2), 0);
        assert_eq!(astrcmp_n(Some(b"abc"), Some(b"abd"), 3), -1);
        assert_eq!(astrcmp_n(Some(b"abc"), Some(b"abd"), 0), 0);
        assert_eq!(astrcmpi_n(Some(b"AbC"), Some(b"aBc"), 3), 0);
        assert_eq!(astrcmpi_n(Some(b"abc"), Some(b"abd"), 2), 0);
    }

    #[test]
    fn r33_astrstri() {
        assert_eq!(astrstri(Some(b"hello world"), Some(b"wo")), Some(6));
        assert_eq!(astrstri(Some(b"hello world"), Some(b"WORLD")), Some(6));
        assert_eq!(astrstri(Some(b"hello"), Some(b"")), Some(0));
        assert_eq!(astrstri(None, Some(b"x")), None);
        assert_eq!(astrstri(Some(b"hello"), None), None);
        assert_eq!(astrstri(Some(b"hello"), Some(b"xyz")), None);
    }

    #[test]
    fn r32_dstr_cmp() {
        let d = mk("abc");
        assert_eq!(dstr_cmp(&d, Some(b"abc")), 0);
        assert!(dstr_cmp(&d, Some(b"abd")) < 0);
        assert!(dstr_cmpi(&d, Some(b"ABC")) == 0);
        assert_eq!(dstr_ncmp(&d, Some(b"abd"), 2), 0);
        assert!(dstr_ncmpi(&d, Some(b"ABC"), 2) == 0);
        let empty = Dstr::init();
        assert_eq!(dstr_cmp(&empty, Some(b"")), 0);
    }

    #[test]
    fn r34_is_empty_end() {
        let mut d = Dstr::init();
        assert!(dstr_is_empty(&d));
        assert_eq!(dstr_end(&d), 0);
        dstr_copy(&mut d, b"abc");
        assert!(!dstr_is_empty(&d));
        assert_eq!(dstr_end(&d), b'c');
    }

    #[test]
    fn r21_replace_vector_05() {
        let mut d = mk("abcabc");
        dstr_replace(&mut d, b"abc", b"");
        assert_eq!(d.len, 0, "empty result");
        assert!(!d.array.is_null(), "array retained (vector 05)");
        let cap_before = d.capacity;
        assert_eq!(d.capacity, cap_before);
        // non-overlapping, resume after replacement
        let mut e = mk("aaa");
        dstr_replace(&mut e, b"aa", b"b");
        assert_eq!(str(&e), "ba");
        // longer replacement grows capacity
        let mut f = mk("a");
        dstr_replace(&mut f, b"a", b"1234567890");
        assert_eq!(str(&f), "1234567890");
        assert!(f.capacity >= 11);
        // empty find is a no-op (Q-6)
        let mut g = mk("abc");
        dstr_replace(&mut g, b"", b"x");
        assert_eq!(str(&g), "abc");
        // empty d is a no-op
        let mut h = Dstr::init();
        dstr_replace(&mut h, b"a", b"b");
        assert!(dstr_is_empty(&h));
    }

    #[test]
    fn r22_depad() {
        let mut d = mk("  \t hello \r\n ");
        dstr_depad(&mut d);
        assert_eq!(str(&d), "hello");
        let mut e = mk("   ");
        dstr_depad(&mut e);
        assert!(dstr_is_empty(&e), "all-pad frees");
        let mut f = Dstr::init();
        dstr_depad(&mut f);
        assert!(dstr_is_empty(&f));
        let mut buf = b" a b ".to_vec();
        let r = strdepad(Some(buf.as_mut_slice()));
        assert_eq!(r.unwrap(), b"a b");
        assert_eq!(strdepad(None), None);
    }

    #[test]
    fn r23_left_mid_right() {
        let src = mk("hello world");
        let mut d = Dstr::init();
        dstr_left(&mut d, &src, 5);
        assert_eq!(str(&d), "hello");
        dstr_left(&mut d, &src, 0);
        assert!(dstr_is_empty(&d));
        dstr_mid(&mut d, &src, 6, 5);
        assert_eq!(str(&d), "world");
        dstr_mid(&mut d, &src, 6, 0);
        assert!(dstr_is_empty(&d));
        dstr_mid(&mut d, &src, 6, 99);
        assert_eq!(str(&d), "world");
        dstr_right(&mut d, &src, 6);
        assert_eq!(str(&d), "world");
        dstr_right(&mut d, &src, 0);
        assert_eq!(str(&d), "hello world");
        // self-truncate: left(d, d, 2) — C-aliasing, expressed in Rust
        // as dstr_truncate (Rust references cannot alias &mut + &)
        let mut s = mk("hello");
        dstr_truncate(&mut s, 2);
        assert_eq!(str(&s), "he");
    }

    #[test]
    fn r24_case() {
        let mut d = mk("hEllO WoRld 123");
        dstr_to_upper(&mut d);
        assert_eq!(str(&d), "HELLO WORLD 123");
        dstr_to_lower(&mut d);
        assert_eq!(str(&d), "hello world 123");
        let mut empty = Dstr::init();
        dstr_to_upper(&mut empty);
        assert!(dstr_is_empty(&empty));
    }

    #[test]
    fn r25_wide_conversions_q2() {
        let mut d = Dstr::init();
        dstr_from_mbs(&mut d, "héllo".as_bytes());
        assert_eq!(str(&d), "héllo");
        assert_eq!(d.capacity, 0, "Q-2: capacity left 0");
        let mbs = dstr_to_mbs(&d).unwrap();
        assert_eq!(&mbs[..mbs.len() - 1], "héllo".as_bytes());
        assert_eq!(mbs[mbs.len() - 1], 0, "NUL-terminated");
        let wcs = dstr_to_wcs(&d).unwrap();
        assert_eq!(wcs, "héllo".chars().map(|c| c as u32).collect::<Vec<_>>());
        dstr_from_wcs(&mut d, &wcs);
        assert_eq!(str(&d), "héllo");
        assert_eq!(d.capacity, 0, "Q-2 symmetric");
        let mut bad = Dstr::init();
        dstr_copy(&mut bad, &[0xff, 0xfe]);
        assert!(dstr_to_wcs(&bad).is_none());
    }

    #[test]
    fn r63_safe_printf_q4() {
        let mut d = Dstr::init();
        dstr_safe_printf(
            &mut d,
            b"hello $1, $2",
            Some(b"world"),
            Some(b"again"),
            None,
            None,
        );
        assert_eq!(str(&d), "hello world, again");
        // Q-4: sequential — $1's text containing $2 is substituted again
        dstr_safe_printf(&mut d, b"x$1x", Some(b"$2"), Some(b"Z"), None, None);
        assert_eq!(str(&d), "xZx");
        // missing values leave $i untouched
        dstr_safe_printf(&mut d, b"a$1b$3c", Some(b"V"), None, None, None);
        assert_eq!(str(&d), "aVb$3c");
    }

    #[test]
    fn r40_strlist_split_vector_09() {
        let l = strlist_split(Some(b"a,,b"), b',', false).unwrap();
        assert_eq!(l.count, 2);
        assert_eq!(unsafe { strlist_get(&l, 0) }, "a");
        assert_eq!(unsafe { strlist_get(&l, 1) }, "b");

        let l = strlist_split(Some(b"a,,b"), b',', true).unwrap();
        assert_eq!(l.count, 3);
        assert_eq!(unsafe { strlist_get(&l, 0) }, "a");
        assert_eq!(unsafe { strlist_get(&l, 1) }, "");
        assert_eq!(unsafe { strlist_get(&l, 2) }, "b");

        let l = strlist_split(Some(b",a,"), b',', true).unwrap();
        assert_eq!(l.count, 3);
        assert_eq!(unsafe { strlist_get(&l, 0) }, "");
        assert_eq!(unsafe { strlist_get(&l, 1) }, "a");
        assert_eq!(unsafe { strlist_get(&l, 2) }, "");

        let l = strlist_split(Some(b""), b',', false).unwrap();
        assert_eq!(l.count, 0);
        let l = strlist_split(Some(b""), b',', true).unwrap();
        assert_eq!(l.count, 1);
        assert_eq!(unsafe { strlist_get(&l, 0) }, "");

        let l = strlist_split(Some(b","), b',', true).unwrap();
        assert_eq!(l.count, 2);
        assert_eq!(unsafe { strlist_get(&l, 0) }, "");
        assert_eq!(unsafe { strlist_get(&l, 1) }, "");

        assert!(strlist_split(None, b',', false).is_none());
    }

    #[test]
    fn r66_cat_quoted_and_strref() {
        let mut d = Dstr::init();
        dstr_cat_quoted(&mut d, b"hi");
        assert_eq!(str(&d), "\"hi\"");
        let bytes = b"ref";
        let r = StrRef {
            array: bytes.as_ptr(),
            len: bytes.len(),
        };
        let mut e = Dstr::init();
        dstr_copy_strref(&mut e, &r);
        assert_eq!(str(&e), "ref");
        dstr_cat_strref(&mut e, &r);
        assert_eq!(str(&e), "refref");
        let empty_ref = StrRef {
            array: bytes.as_ptr(),
            len: 0,
        };
        dstr_cat_strref(&mut e, &empty_ref);
        assert_eq!(str(&e), "refref");
    }

    #[test]
    fn dstr_as_str_helper() {
        let d = mk("ok");
        assert_eq!(dstr_as_str(&d), Some("ok"));
        let mut bad = Dstr::init();
        let buf: [u8; 2] = [0xff, 0];
        bad.array = buf.as_ptr() as *mut u8;
        bad.len = 1;
        bad.capacity = 0;
        assert!(dstr_as_str(&bad).is_none());
        // leak: this dstr owns a foreign pointer — reset before drop
        bad.array = std::ptr::null_mut();
    }
}
