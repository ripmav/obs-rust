//! P1-07.3 recipe step 2 (the WP-23 pattern): the C ABI for the Flatpak
//! sleep-inhibition files (`libobs/util/platform-nix-dbus.c`,
//! `libobs/util/platform-nix-portal.c`) — fully ported to
//! [`obs_util::os_inhibit`].
//!
//! With `OBS_ENABLE_RUST=ON` those two C files are excluded from the
//! build (`test/conformance/obs-enable-rust.patch`); these exports
//! provide their six symbols to the rest of libobs (the dispatcher in
//! the still-C `platform-nix.c` calls them).
//!
//! The handles are Rust `Box`es, opaque in C (`struct dbus_sleep_info;`
//! / `struct portal_inhibit_info;` are incomplete types — C only passes
//! the pointers around, and the bridge's destroy functions free them).
//! Nothing here crosses the C allocator boundary, so no `bmalloc`/
//! `bfree` is involved (the WP-23 allocator invariant is vacuously
//! satisfied, the [`crate::abi_threading`] precedent).
//!
//! Log routing: the Rust code logs through the `obs_util::os_fs`
//! platform sink and crashes through the `obs_util::os_paths` fatal
//! hook; on first use they are wired to this crate's `blog` (C handler)
//! and the C `bcrash`, so a swapped build logs and crashes like C.

use obs_util::os_inhibit::{DbusSleepInfo, PortalInhibitInfo};
use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::sync::Once;

/// Route the Rust platform log/fatal paths through the C side once
/// (the C handler is installed by the runner/`blog` users first — the
/// wiring only happens on the first platform call, which in every
/// conformance run comes after `base_set_log_handler`).
static WIRED: Once = Once::new();

fn ensure_wired() {
    WIRED.call_once(|| {
        obs_util::os_fs::set_platform_log_sink(|level, line| {
            // The C `blog` treats its message as a C string (primer §5.5)
            // — append the NUL the Rust line does not carry.
            let msg = format!("{line}\0");
            crate::blog(level, &msg);
        });
        obs_util::os_paths::set_platform_fatal_hook(|msg| {
            let msg = format!("{msg}\0");
            // SAFETY: `bcrash` is the C shim (base-logging.c, always compiled into this staticlib); the NUL-terminated format contains no `%` (the fatal lines are literals).
            unsafe { bcrash(msg.as_ptr().cast()) };
        });
    });
}

unsafe extern "C" {
    /// The C shim's `bcrash(format, ...)` (base-logging.c).
    fn bcrash(format: *const c_char);
}

/// Copy a possibly-NULL `reason` argument (C uses it only when
/// activating; keep this side total with a fallback of `""`).
///
/// # Safety
/// `reason` is either NULL or a valid NUL-terminated C string.
unsafe fn reason_cstr(reason: *const c_char) -> CString {
    if reason.is_null() {
        CString::new("").unwrap()
    } else {
        // SAFETY: non-NULL (guarded above); valid NUL-terminated C string, copied out.
        unsafe { CStr::from_ptr(reason).to_owned() }
    }
}

/// C `dbus_sleep_info_create()` (U4, R23).
///
/// # Safety
/// C-ABI — no arguments.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dbus_sleep_info_create() -> *mut c_void {
    ensure_wired();
    match obs_util::os_inhibit::dbus_sleep_info_create() {
        Some(info) => {
            // SAFETY: fresh Box — the raw handle is C-owned until the matching destroy bridge.
            Box::into_raw(Box::new(info)) as *mut c_void
        }
        None => std::ptr::null_mut(),
    }
}

/// C `dbus_sleep_info_destroy(info)` (U4, R25).
///
/// # Safety
/// `info` must be a handle from `dbus_sleep_info_create` (or NULL — a
/// no-op, C behaviour).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dbus_sleep_info_destroy(info: *mut c_void) {
    if !info.is_null() {
        // SAFETY: non-NULL (guarded above) and from the matching create bridge; ownership C -> Rust.
        let owned = unsafe { Box::from_raw(info as *mut DbusSleepInfo) };
        obs_util::os_inhibit::dbus_sleep_info_destroy(*owned);
    }
}

/// C `dbus_inhibit_sleep(info, reason, active)` (U4, R24/R25).
///
/// # Safety
/// `info` must be a live handle from `dbus_sleep_info_create` (the C
/// reference dereferences it unguarded; the conformance runner
/// pre-checks). `reason` must be a valid NUL-terminated string when
/// `active` (ignored otherwise, as in C).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dbus_inhibit_sleep(
    info: *mut c_void,
    reason: *const c_char,
    active: bool,
) {
    ensure_wired();
    // SAFETY: `reason_cstr` takes a `*const c_char` (NULL-safe, returns an owned `CString`).
    let reason = unsafe { reason_cstr(reason) };
    // SAFETY: the handle is non-NULL (the runner pre-checks) and from the matching create bridge; live for the call.
    let info = unsafe { &mut *(info as *mut DbusSleepInfo) };
    obs_util::os_inhibit::dbus_inhibit_sleep(info, &reason, active);
}

/// C `portal_inhibit_info_create()` (U5, R26).
///
/// # Safety
/// C-ABI — no arguments.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn portal_inhibit_info_create() -> *mut c_void {
    ensure_wired();
    match obs_util::os_inhibit::portal_inhibit_info_create() {
        Some(info) => {
            // SAFETY: fresh Box — the raw handle is C-owned until the matching destroy bridge.
            Box::into_raw(Box::new(info)) as *mut c_void
        }
        None => std::ptr::null_mut(),
    }
}

/// C `portal_inhibit_info_destroy(info)` (U5, R31).
///
/// # Safety
/// `info` must be a handle from `portal_inhibit_info_create` (or NULL —
/// a no-op, C behaviour).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn portal_inhibit_info_destroy(info: *mut c_void) {
    if !info.is_null() {
        // SAFETY: non-NULL (guarded above) and from the matching create bridge; ownership C -> Rust.
        let owned = unsafe { Box::from_raw(info as *mut PortalInhibitInfo) };
        obs_util::os_inhibit::portal_inhibit_info_destroy(*owned);
    }
}

/// C `portal_inhibit(info, reason, active)` (U5, R27-R30).
///
/// # Safety
/// `info` must be a live handle from `portal_inhibit_info_create` (the
/// C reference dereferences it unguarded; the conformance runner
/// pre-checks). `reason` must be a valid NUL-terminated string when
/// `active` (ignored otherwise, as in C).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn portal_inhibit(info: *mut c_void, reason: *const c_char, active: bool) {
    ensure_wired();
    // SAFETY: `reason_cstr` takes a `*const c_char` (NULL-safe, returns an owned `CString`).
    let reason = unsafe { reason_cstr(reason) };
    // SAFETY: the handle is non-NULL (the runner pre-checks) and from the matching create bridge; live for the call.
    let info = unsafe { &mut *(info as *mut PortalInhibitInfo) };
    obs_util::os_inhibit::portal_inhibit(info, &reason, active);
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The platform log sink must reach the C `blog` through the swap
    /// wiring — the conformance `log` lines of the dbus vectors depend
    /// on it. (The Rust `LogHandler` type takes `VaList` as a pointer —
    /// the va_list ABI of the C side is not consumable from Rust, so
    /// like the `r21_r22_handler_roundtrip` test this asserts on the
    /// unformatted format, not on the formatted line.)
    #[test]
    fn wiring_routes_logs_to_c_handler() {
        static LINES: std::sync::Mutex<Vec<(i32, String)>> = std::sync::Mutex::new(Vec::new());

        /// Captures the unformatted format (house pattern, lib.rs
        /// `r21_r22_handler_roundtrip`).
        unsafe extern "C" fn handler(
            level: i32,
            format: *const c_char,
            _args: crate::VaList,
            _p: *mut c_void,
        ) {
            // SAFETY: `format` is a live NUL-terminated C string for the call.
            let s = unsafe { CStr::from_ptr(format) }
                .to_string_lossy()
                .into_owned();
            LINES.lock().unwrap().push((level, s));
        }

        // SAFETY: the C shim stores the handler; `handler` follows the `log_handler_t` contract.
        unsafe { crate::base_set_log_handler(Some(handler), std::ptr::null_mut()) };
        ensure_wired();
        obs_util::os_fs::log_platform(obs_util::os_fs::LOG_ERROR, "wp30 wiring line");
        {
            let lines = LINES.lock().unwrap();
            assert!(
                lines
                    .iter()
                    .any(|(level, fmt)| *level == obs_util::os_fs::LOG_ERROR && fmt == "%s"),
                "the platform line must reach the C blog (format `%s`): {lines:?}"
            );
        }
        // restore the default handler (process-global)
        // SAFETY: the C shim setter; NULL reinstalls the default.
        unsafe { crate::base_set_log_handler(None, std::ptr::null_mut()) };
    }

    /// With an unreachable session bus both create bridges give up
    /// (NULL) and destroy on NULL is a no-op — the `dbus_nobus`
    /// vector's contract at the C-ABI boundary.
    #[test]
    fn create_null_with_unreachable_bus() {
        // SAFETY: test-only: no other test in this binary reads `DBUS_SESSION_BUS_ADDRESS`.
        unsafe { std::env::set_var("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-wp30") };
        // SAFETY: C-ABI create bridge; with an unreachable bus it returns NULL (no handle leaks).
        assert!(unsafe { dbus_sleep_info_create() }.is_null());
        // SAFETY: C-ABI create bridge; with an unreachable bus it returns NULL (no handle leaks).
        assert!(unsafe { portal_inhibit_info_create() }.is_null());
        // NULL destroys are no-ops (the runner exercises both)
        // SAFETY: the destroy bridges take opaque handles; NULL is the documented no-op form.
        unsafe {
            dbus_sleep_info_destroy(std::ptr::null_mut());
            portal_inhibit_info_destroy(std::ptr::null_mut());
        }
    }
}
