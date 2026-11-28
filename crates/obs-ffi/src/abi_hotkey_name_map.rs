//! P1-28.4 recipe step 2: the C ABI for the hotkey key↔name map
//! (`libobs/obs-hotkey-name-map.c`, S-024 M4).
//!
//! The file's three non-static functions. In the swapped build the map is
//! never built (its `init_name_map` is file-local to the excluded source),
//! so `obs_key_from_name` is the pure table lookup (the map's contents are
//! the same table) and `obs_hotkey_name_map_free` is a no-op — identical
//! to the C behaviour when the `obs` global is NULL or the map is
//! uninitialised (`if (!obs || !obs->hotkeys.name_map) return;`).

use std::os::raw::{c_char, c_int};

use std::ffi::CString;
use std::sync::OnceLock;

use crate::{
    LOG_ERROR,
    abi_common::{bfree, bstrdup_bytes},
};
use obs_hotkey_names::OBS_KEY_LAST_VALUE;

unsafe extern "C" {
    fn blog(level: c_int, format: *const c_char, ...) -> c_int;
}

/// NUL-terminated copies of every table name, built once (process
/// lifetime, like the C static `obs_key_names[]`).
fn c_names() -> &'static [CString] {
    static NAMES: OnceLock<Vec<CString>> = OnceLock::new();
    NAMES.get_or_init(|| {
        obs_hotkey_names::KEY_NAMES
            .iter()
            .map(|s| CString::new(s.as_bytes()).expect("key names are NUL-free"))
            .collect()
    })
}

/// `const char *obs_key_to_name(obs_key_t key)`
///
/// `key` is the C `obs_key_t` enum (int-sized). Out of range
/// (`key >= OBS_KEY_LAST_VALUE`, incl. negative values) logs the exact
/// C ERROR line and returns `""`; otherwise the static table name.
///
/// # Safety
/// None (all arguments are by value); the returned pointer is static
/// storage for the process lifetime.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn obs_key_to_name(key: c_int) -> *const c_char {
    // `obs_key_t` is an `enum` that GCC compiles with an unsigned
    // underlying type, so the C reference's `key >= OBS_KEY_LAST_VALUE`
    // guard is true for every negative key too (the error path logs the
    // signed value and returns "" — helpers/hotkeys-lookup, `hk_name -1`).
    if key < 0 || key >= OBS_KEY_LAST_VALUE as c_int {
        // The C reference: blog(LOG_ERROR, "obs-hotkey.c: queried unknown
        // key with code %d", (int)key).
        // SAFETY: a fresh bmem allocation; freed below.
        let msg = unsafe {
            bstrdup_bytes(format!("obs-hotkey.c: queried unknown key with code {key}").as_bytes())
        };
        // SAFETY: msg is a live NUL-terminated C string for the call.
        unsafe { blog(LOG_ERROR, c"%s".as_ptr(), msg) };
        // SAFETY: msg came from the C bmem allocator.
        unsafe { bfree(msg) };
        return c"".as_ptr();
    }
    // `key` is in range (guarded above) — the table entry is static for
    // the process lifetime.
    c_names()[key as usize].as_ptr()
}

/// `obs_key_t obs_key_from_name(const char *name)`
///
/// Exact, case-sensitive full-string lookup; no match (or NULL) is
/// `OBS_KEY_NONE` (0) — S-024-R20.
///
/// # Safety
/// `name` must be NULL or a NUL-terminated string readable for the whole
/// call.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn obs_key_from_name(name: *const c_char) -> c_int {
    let Some(bytes) = crate::abi_common::c_bytes(name) else {
        return 0;
    };
    obs_hotkey_names::obs_key_from_name(&String::from_utf8_lossy(bytes)) as c_int
}

/// `void obs_hotkey_name_map_free(void)`
///
/// No-op in the swapped build: the map is never built (see the module
/// docs) — the C function returns early on a NULL map the same way.
#[unsafe(no_mangle)]
pub extern "C" fn obs_hotkey_name_map_free() {}

#[cfg(test)]
mod tests {
    use super::*;

    /// Table parity through the C ABI: every id round-trips name→id, the
    /// out-of-range id logs and yields "", a bogus name yields 0.
    #[test]
    fn name_map_bridge_roundtrip() {
        // SAFETY: all results are static strings / by-value.
        unsafe {
            for key in [0i32, 1, 100, 300, (OBS_KEY_LAST_VALUE - 1) as i32] {
                let name = std::ffi::CStr::from_ptr(obs_key_to_name(key));
                let back = obs_key_from_name(name.as_ptr());
                assert_eq!(back, key, "round trip of {key}");
            }
            assert_eq!(*obs_key_to_name(OBS_KEY_LAST_VALUE as c_int), b'\0'); // "" (the error log is unobservable here)
            // Negative keys take the same error path (the C enum compares unsigned):
            assert_eq!(*obs_key_to_name(-1), b'\0');
            assert_eq!(*obs_key_to_name(-12345), b'\0');
            assert_eq!(obs_key_from_name(c"bogus".as_ptr()), 0);
            assert_eq!(obs_key_from_name(std::ptr::null()), 0);
            obs_hotkey_name_map_free(); // no-op, must not crash
        }
    }
}
