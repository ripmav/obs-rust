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

// C allocator (`libobs/util/bmem.c`): anything C frees must come from
// here. Declared once for the whole crate; the swap build links
// `libobs/util/bmem.c` next to the Rust staticlib.
unsafe extern "C" {
    pub(crate) fn bmalloc(size: usize) -> *mut u8;
    pub(crate) fn bfree(ptr: *mut u8);
    /// Used by the part B (cf-lexer/cf-parser) bridges.
    #[allow(dead_code)]
    pub(crate) fn brealloc(ptr: *mut u8, size: usize) -> *mut u8;
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
    let len = unsafe { CStr::from_ptr(s) }.to_bytes().len();
    let p = unsafe { bmalloc(len + 1) } as *mut c_char;
    unsafe {
        std::ptr::copy_nonoverlapping(s, p, len);
        *p.add(len) = 0;
    };
    p
}

/// NUL-terminated copy of a (possibly non-NUL-terminated) Rust byte buffer
/// through `bmalloc`. Use this — not [`bstrdup`] — for Rust-owned strings.
pub(crate) unsafe fn bstrdup_bytes(bytes: &[u8]) -> *mut c_char {
    let p = unsafe { bmalloc(bytes.len() + 1) } as *mut c_char;
    unsafe {
        std::ptr::copy_nonoverlapping(bytes.as_ptr(), p, bytes.len());
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
    unsafe {
        let ptr = bmalloc(esize * new_cap) as *mut error_item;
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
    let ed = unsafe { &mut *ed };
    da_ensure(ed);
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
    let ed = unsafe { &*ed };
    if ed.errors.num == 0 {
        return std::ptr::null_mut();
    }
    let items = ed.errors.array;
    let mut s = String::new();
    for i in 0..ed.errors.num {
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
    unsafe { bstrdup_bytes(s.as_bytes()) }
}
