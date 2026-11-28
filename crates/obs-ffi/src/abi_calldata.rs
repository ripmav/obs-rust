//! P1-20.3 recipe step 2: the C ABI for the three exported calldata
//! functions (`libobs/callback/calldata.{c,h}`, S-002).
//!
//! `calldata_t` is the Rust `Calldata` (`#[repr(C)]`, the 4W layout of
//! S-002 §2 — `stack/size/capacity/fixed`); the bridge casts the pointer.
//! The header `static inline` typed accessors (`calldata_get/set_int/
//! float/bool/ptr`, `calldata_set_string`, the lifecycle inlines) stay
//! in the C header for non-swapped consumers — they operate on the
//! shared layout, the same arrangement as `darray.h`.
//!
//! `calldata_get_string` reproduces S-002-R04: it succeeds on **any**
//! found record (type is only the byte count, Q-1/Q-3) and writes a
//! pointer to the payload start — NULL when the stored size is 0. The
//! Rust `calldata_get_string` collapses found-but-empty into `None`, so
//! the bridge disambiguates with a zero-length `calldata_get_data`
//! (which succeeds exactly on a found record with `data_size == 0`).

use std::os::raw::{c_char, c_void};

use crate::abi_common::ensure_c_log_sink;
use obs_signal::calldata::{
    Calldata, calldata_get_data as cd_get, calldata_get_string as cd_get_string,
    calldata_set_data as cd_set,
};

/// `EXPORT bool calldata_get_data(const calldata_t *data, const char
/// *name, void *out, size_t size)`
///
/// # Safety
/// `data` must be NULL or a live calldata; `name` must be NULL or
/// NUL-terminated (NULL / empty name → `false`, C behaviour); `out`
/// writable for `size` bytes whenever the call succeeds.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn calldata_get_data(
    data: *const Calldata,
    name: *const c_char,
    out: *mut c_void,
    size: usize,
) -> bool {
    ensure_c_log_sink();
    let Some(data) = (if data.is_null() { None } else { Some(data) }) else {
        return false;
    };
    let Some(name) = crate::abi_common::c_bytes(name) else {
        return false;
    };
    // SAFETY: caller contract — `data` is a live calldata for the call; `out` is writable for `size` bytes on success (the copy happens inside).
    unsafe {
        let data = &*data;
        let out = std::slice::from_raw_parts_mut(out.cast::<u8>(), size);
        cd_get(data, name, out)
    }
}

/// `EXPORT void calldata_set_data(calldata_t *data, const char *name,
/// const void *in, size_t new_size)`
///
/// # Safety
/// `data` must be NULL or a live calldata (NULL / empty name → no-op,
/// C behaviour); `in` readable for `new_size` bytes (NULL is fine for
/// `new_size == 0`, C behaviour R15).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn calldata_set_data(
    data: *mut Calldata,
    name: *const c_char,
    in_: *const c_void,
    new_size: usize,
) {
    ensure_c_log_sink();
    let Some(data) = (if data.is_null() { None } else { Some(data) }) else {
        return;
    };
    let Some(name) = crate::abi_common::c_bytes(name) else {
        return;
    };
    // SAFETY: caller contract — `data` is a live calldata for the call; `in_` is readable for `new_size` bytes (NULL allowed for size 0).
    unsafe {
        let in_ = if new_size == 0 {
            &[][..]
        } else {
            std::slice::from_raw_parts(in_.cast::<u8>(), new_size)
        };
        cd_set(&mut *data, name, in_);
    }
}

/// `EXPORT bool calldata_get_string(const calldata_t *data, const char
/// *name, const char **str)`
///
/// # Safety
/// `data` must be NULL or a live calldata; `name` must be NULL or
/// NUL-terminated; on success `str` must point to writable
/// `const char *` storage. The stored pointer points into the calldata
/// stack and stays valid until the calldata is modified or freed
/// (S-002-R05).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn calldata_get_string(
    data: *const Calldata,
    name: *const c_char,
    str_out: *mut *const c_char,
) -> bool {
    ensure_c_log_sink();
    let Some(data) = (if data.is_null() { None } else { Some(data) }) else {
        return false;
    };
    let Some(name) = crate::abi_common::c_bytes(name) else {
        return false;
    };
    // SAFETY: caller contract — `data` is a live calldata for the call.
    unsafe {
        let data = &*data;
        if let Some(payload) = cd_get_string(data, name) {
            *str_out = payload.as_ptr().cast::<c_char>();
            return true;
        }
        // Not found, or found with an empty payload (S-002-R04 returns
        // true with NULL in the latter case) — the zero-length get_data
        // succeeds exactly on the latter.
        if cd_get(data, name, &mut []) {
            *str_out = std::ptr::null();
            return true;
        }
        false
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The three exports round-trip through the C ABI: set/get of an int
    /// payload, get_string on a string payload (payload start, NUL
    /// included per S-002 storage), the found-empty string case
    /// (true + NULL), and the not-found / NULL guards.
    #[test]
    fn calldata_bridge_roundtrip() {
        // SAFETY: test — Calldata::default() is a valid empty calldata (NULL stack, size 0), and every call below guards its own pointers.
        unsafe {
            let mut d = Calldata::default();
            let ptr = &mut d as *mut Calldata;

            let v: i64 = 0x1122_3344_5566_7788;
            calldata_set_data(
                ptr,
                c"i".as_ptr(),
                v.to_ne_bytes().as_ptr().cast::<c_void>(),
                8,
            );
            let mut out = [0u8; 8];
            assert!(calldata_get_data(
                ptr,
                c"i".as_ptr(),
                out.as_mut_ptr().cast::<c_void>(),
                8
            ));
            assert_eq!(i64::from_ne_bytes(out), v);
            // Q-1: a wrong-size read fails without a write.
            let mut two = [0u8; 2];
            assert!(!calldata_get_data(
                ptr,
                c"i".as_ptr(),
                two.as_mut_ptr().cast::<c_void>(),
                2
            ));
            assert_eq!(two, [0u8; 2]);

            let s = b"hello";
            let mut with_nul = s.to_vec();
            with_nul.push(0);
            calldata_set_data(
                ptr,
                c"s".as_ptr(),
                with_nul.as_ptr().cast::<c_void>(),
                with_nul.len(),
            );
            let mut sp: *const c_char = std::ptr::null();
            assert!(calldata_get_string(ptr, c"s".as_ptr(), &mut sp));
            assert_eq!(
                std::slice::from_raw_parts(sp.cast::<u8>(), 6),
                &with_nul[..]
            );

            calldata_set_data(ptr, c"e".as_ptr(), std::ptr::null(), 0);
            assert!(calldata_get_string(ptr, c"e".as_ptr(), &mut sp));
            assert!(sp.is_null(), "found-but-empty is true + NULL (R04)");

            assert!(!calldata_get_string(ptr, c"missing".as_ptr(), &mut sp));
            assert!(!calldata_get_data(
                std::ptr::null(),
                c"i".as_ptr(),
                out.as_mut_ptr().cast::<c_void>(),
                8
            ));
            assert!(!calldata_get_string(ptr, std::ptr::null(), &mut sp));
            calldata_set_data(
                std::ptr::null_mut(),
                c"i".as_ptr(),
                v.to_ne_bytes().as_ptr().cast::<c_void>(),
                8,
            ); // no-op

            // Free the stack the test calldata grew (R11 bmem allocation).
            obs_signal::calldata::calldata_free(&mut d);
            std::mem::forget(d);
        }
    }
}
