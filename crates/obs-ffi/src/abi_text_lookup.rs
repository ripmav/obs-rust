//! P1-08.5 recipe step 2: the C ABI for text-lookup
//! (`libobs/util/text-lookup.h`).
//!
//! The four `EXPORT` functions with the exact C signatures; `lookup_t`
//! stays opaque (the C header only exposes the typedef). State is the
//! Rust [`TextLookup`] boxed on the heap.
//!
//! Lifetime contract (identical to the C dstr-backed value): the pointer
//! `getstr` writes to `*out` is valid until the next `add` on the same
//! lookup or `destroy`.

use std::ffi::CStr;
use std::os::raw::{c_char, c_void};

use obs_util::text_lookup::TextLookup;

/// `EXPORT lookup_t *text_lookup_create(const char *path)`
///
/// # Safety
/// `path` (or NULL) must point to a NUL-terminated string readable for
/// the whole call. The returned `lookup_t *` (or NULL) must be freed
/// with `text_lookup_destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn text_lookup_create(path: *const c_char) -> *mut c_void {
    if path.is_null() {
        return std::ptr::null_mut();
    }
    // SAFETY: a live NUL-terminated C string (checked above).
    let bytes = unsafe { CStr::from_ptr(path) }.to_bytes();
    match TextLookup::create(bytes) {
        Some(lookup) => Box::into_raw(Box::new(lookup)) as *mut c_void,
        None => std::ptr::null_mut(),
    }
}

/// `EXPORT bool text_lookup_add(lookup_t *lookup, const char *path)`
///
/// # Safety
/// `lookup` must be a pointer returned by `text_lookup_create` (or NULL);
/// `path` (or NULL) must point to a NUL-terminated string readable for
/// the whole call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn text_lookup_add(lookup: *mut c_void, path: *const c_char) -> bool {
    if lookup.is_null() || path.is_null() {
        return false;
    }
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live and exclusively owned for the call.
    let lk = unsafe { &mut *(lookup as *mut TextLookup) };
    // SAFETY: a live NUL-terminated C string (checked above).
    let bytes = unsafe { CStr::from_ptr(path) }.to_bytes();
    lk.add(bytes)
}

/// `EXPORT void text_lookup_destroy(lookup_t *lookup)`
///
/// # Safety
/// `lookup` must be a pointer returned by `text_lookup_create` (or NULL).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn text_lookup_destroy(lookup: *mut c_void) {
    if !lookup.is_null() {
        // SAFETY: the handle is non-NULL (guarded above) and came from the matching init bridge; ownership C -> Rust.
        unsafe { drop(Box::from_raw(lookup as *mut TextLookup)) };
    }
}

/// `EXPORT bool text_lookup_getstr(lookup_t *lookup, const char *lookup_val,
/// const char **out)`
///
/// # Safety
/// `lookup` must be a pointer returned by `text_lookup_create` (or NULL);
/// `lookup_val` and `out` (or NULL) must point to writable/readable
/// memory for the whole call. On success `*out` references the lookup's
/// stored value — valid until the next `add` on the same lookup or
/// `destroy`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn text_lookup_getstr(
    lookup: *mut c_void,
    lookup_val: *const c_char,
    out: *mut *const c_char,
) -> bool {
    if lookup.is_null() || lookup_val.is_null() || out.is_null() {
        return false;
    }
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    let lk = unsafe { &*(lookup as *const TextLookup) };
    // SAFETY: a live NUL-terminated C string (checked above).
    let key = unsafe { CStr::from_ptr(lookup_val) }.to_bytes();
    match lk.getstr(key) {
        Some(value) => {
            // SAFETY: C-ABI — `out` is valid out storage (caller contract); the pointer is valid for the call (documented above).
            unsafe { *out = value.as_ptr().cast() };
            true
        }
        None => false,
    }
}
