//! P1-08.5 recipe step 2 — shared plumbing for the C ABI bridges.
//!
//! All allocation that C will later `bfree` goes through the C allocator
//! (`libobs/util/bmem.c`, C-side in the swap build); everything else is
//! Rust-owned and freed by the bridge's free functions.
//!
//! Part A (P1-08.5) covers text-lookup ([`crate::abi_text_lookup`]) and
//! the base lexer ([`crate::abi_lexer`]); part B1 adds the cf-lexer
//! ([`crate::abi_cf_lexer`], which drives the darray growth through the
//! same externs); the cf-parser view plumbing lands with part B2.

use std::ffi::CStr;
use std::mem::size_of;
use std::os::raw::c_char;

use crate::abi_lexer::{error_data, error_item};

// C allocator: anything C frees must come from here. P1-01.3 (WP-43):
// the swap build's `bmalloc`/`brealloc`/`bfree` symbols ARE the Rust
// bridge exports (`abi_bmem`) inside the staticlib — these imports
// resolve to them (and to the same functions in `cargo test`). The
// import names differ from the exports (cbindgen declares every extern
// name it sees; a same-named import + export would duplicate in the
// generated header — the `bfree_raw` precedent).
unsafe extern "C" {
    #[link_name = "bmalloc"]
    pub(crate) fn bmalloc_c(size: usize) -> *mut u8;
    #[link_name = "bfree"]
    fn bfree_raw(ptr: *mut u8);
    /// Used by the part B (cf-lexer/cf-parser) bridges.
    #[allow(dead_code)]
    #[link_name = "brealloc"]
    pub(crate) fn brealloc_c(ptr: *mut u8, size: usize) -> *mut u8;
}

/// C `bfree` for any pointee type: C `char` is `i8` on x86 and `u8` on
/// arm, so callers pass `*mut c_char`/`*mut u8` without per-site casts.
///
/// # Safety
/// `ptr` is NULL or came from `bmalloc`/`brealloc`/`bstrdup` and is not
/// used afterwards.
pub(crate) unsafe fn bfree<T>(ptr: *mut T) {
    // SAFETY: forwarded contract; the cast only changes the pointee type.
    unsafe { bfree_raw(ptr.cast()) }
}

/// C `bstrdup` (a `static inline` in bmem.h, so not linkable): a
/// NUL-terminated copy through `bmalloc`; NULL stays NULL. The result is
/// freed with `bfree` exactly like the C inline's result.
///
/// The source MUST be a NUL-terminated C buffer (it is read with CStr).
/// For Rust-owned (not NUL-terminated) buffers use [`bstrdup_bytes`].
pub(crate) unsafe fn bstrdup(s: *const c_char) -> *mut c_char {
    if s.is_null() {
        return std::ptr::null_mut();
    }
    // SAFETY: a live NUL-terminated C string (checked above).
    let len = unsafe { CStr::from_ptr(s) }.to_bytes().len();
    // SAFETY: C-ABI — S-001 allocation; the result is stored into `p` immediately.
    let p = unsafe { bmalloc_c(len + 1) } as *mut c_char;
    // SAFETY: the fresh bmalloc block is valid for len+1 bytes (allocated above); the NUL write fits.
    unsafe {
        std::ptr::copy_nonoverlapping(s, p, len);
        *p.add(len) = 0;
    };
    p
}

/// NUL-terminated copy of a (possibly non-NUL-terminated) Rust byte buffer
/// through `bmalloc`. Use this — not [`bstrdup`] — for Rust-owned strings.
pub(crate) unsafe fn bstrdup_bytes(bytes: &[u8]) -> *mut c_char {
    // SAFETY: C-ABI — S-001 allocation; the result is stored into `p` immediately.
    let p = unsafe { bmalloc_c(bytes.len() + 1) } as *mut c_char;
    // SAFETY: the fresh bmalloc block is valid for bytes.len()+1 bytes (allocated above); the NUL write fits.
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), p.cast::<u8>(), bytes.len());
        *p.add(bytes.len()) = 0;
    };
    p
}

/// Read a NUL-terminated C string as bytes (`None` for NULL; the NUL is
/// excluded, exactly like `CStr::to_bytes`).
pub(crate) fn c_bytes<'a>(p: *const c_char) -> Option<&'a [u8]> {
    if p.is_null() {
        None
    } else {
        // SAFETY: a live NUL-terminated C string (checked above).
        Some(unsafe { CStr::from_ptr(p) }.to_bytes())
    }
}

/// `darray_ensure_capacity` (darray.h) for one more `error_item`:
/// `bmalloc` the doubled capacity, copy the old elements, `bfree` the old
/// array.
fn da_ensure(ed: &mut error_data) {
    let new_size = ed.errors.num + 1;
    if new_size <= ed.errors.capacity {
        return;
    }
    let esize = size_of::<error_item>();
    let new_cap = if ed.errors.capacity == 0 {
        new_size
    } else {
        ed.errors.capacity * 2
    };
    let new_cap = new_cap.max(new_size);
    // SAFETY: C-ABI error_list growth — the fresh bmalloc block is valid and the copy fits in old capacity·esize.
    unsafe {
        let ptr = bmalloc_c(esize * new_cap) as *mut error_item;
        if !ed.errors.array.is_null() && ed.errors.capacity > 0 {
            // element count (not bytes): copies esize * old_capacity bytes,
            // exactly like the C `memcpy(ptr, old, element_size * capacity)`
            std::ptr::copy_nonoverlapping(ed.errors.array, ptr, ed.errors.capacity);
            bfree(ed.errors.array as *mut u8);
        }
        ed.errors.array = ptr;
        ed.errors.capacity = new_cap;
    }
}

/// `error_data_add` over a C `struct error_data` (`da_push_back`): borrow
/// `file`, `bstrdup` the message (NULL stays NULL), grow the darray with
/// the C allocator. A NULL `ed` is a no-op (C behaviour).
pub(crate) unsafe fn error_data_add_c(
    ed: *mut error_data,
    file: *const c_char,
    row: u32,
    column: u32,
    msg: *const c_char,
    level: i32,
) {
    if ed.is_null() {
        return;
    }
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live and exclusively owned for the call.
    let ed = unsafe { &mut *ed };
    da_ensure(ed);
    // SAFETY: C-ABI — raw CPOD field access on a live C struct (checked above); the fields are Copy, no drop.
    unsafe {
        ed.errors.array.add(ed.errors.num).write(error_item {
            error: if msg.is_null() {
                std::ptr::null_mut()
            } else {
                bstrdup(msg)
            },
            file,
            row,
            column,
            level,
        });
    }
    ed.errors.num += 1;
}

/// `error_data_buildstring` over a C `struct error_data`:
/// `"<file> (row, col): msg\n"` per item (a NULL file or message prints
/// as `(null)`, the glibc `%s` rendering the C reference produces); NULL
/// when there are no items. The result is C-allocated (`bstrdup`).
pub(crate) unsafe fn error_data_buildstring_c(ed: *const error_data) -> *mut c_char {
    if ed.is_null() {
        return std::ptr::null_mut();
    }
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    let ed = unsafe { &*ed };
    if ed.errors.num == 0 {
        return std::ptr::null_mut();
    }
    let items = ed.errors.array;
    let mut s = String::new();
    for i in 0..ed.errors.num {
        // SAFETY: C-ABI — i < items.num (checked above); the element is a live CPOD.
        let it = unsafe { &*items.add(i) };
        let file = c_bytes(it.file)
            .map(|f| String::from_utf8_lossy(f).into_owned())
            .unwrap_or_else(|| "(null)".to_owned());
        let msg = c_bytes(it.error)
            .map(|m| String::from_utf8_lossy(m).into_owned())
            .unwrap_or_else(|| "(null)".to_owned());
        s.push_str(&file);
        s.push_str(&format!(" ({}, {}): {}\n", it.row, it.column, msg));
    }
    // SAFETY: C-ABI — the bytes are valid for the call; the result is bmem-owned.
    unsafe { bstrdup_bytes(s.as_bytes()) }
}
