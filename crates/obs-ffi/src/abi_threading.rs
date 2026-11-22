//! P1-05.4 recipe step 2 (the WP-23 pattern): the C ABI for the threading
//! primitives (`libobs/util/threading.h`, `threading-posix.c`).
//!
//! The twelve `EXPORT` functions plus the two `static inline` pthread
//! mutex-init helpers (not linkable in C — the Rust definitions are the
//! linkable form, the `bstrdup` precedent of [`crate::abi_common`]).
//!
//! `os_event_t`/`os_sem_t` are opaque in the C header (`struct
//! os_event_data;` — no fields): the handles are the Rust `OsEvent`/
//! `OsSem` boxed on the heap. Rust owns the memory; C only passes the
//! pointers around, and the bridge's destroy functions free them. Nothing
//! here crosses the C allocator boundary, so no `bmalloc`/`bfree` is
//! involved (the WP-23 allocator invariant is vacuously satisfied).

use std::os::raw::{c_char, c_int, c_ulong, c_void};

use obs_util::threading::{OsEvent, OsEventType, OsSem, pthread_mutex_t};

/// `EXPORT int os_event_init(os_event_t **event, enum os_event_type type)`
///
/// # Safety
/// `event` must point to writable `os_event_t *` storage for the whole
/// call. On success it is written with a Rust-owned handle that must be
/// freed with `os_event_destroy`; on failure it is left unmodified (C
/// behaviour).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_event_init(event: *mut *mut c_void, r#type: c_int) -> c_int {
    // C: `data->manual = (type == OS_EVENT_TYPE_MANUAL)` — anything but 1
    // is AUTO.
    let kind = if r#type == 1 {
        OsEventType::Manual
    } else {
        OsEventType::Auto
    };
    match obs_util::threading::os_event_init(kind) {
        Ok(e) => {
            // SAFETY: fresh Box — the raw handle is C-owned until the matching destroy bridge (doc above).
            unsafe { *event = Box::into_raw(Box::new(e)) as *mut c_void };
            0
        }
        Err(code) => code,
    }
}

/// `EXPORT void os_event_destroy(os_event_t *event)`
///
/// # Safety
/// `event` must be a handle returned by `os_event_init` (or NULL — a
/// no-op, C behaviour).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_event_destroy(event: *mut c_void) {
    if !event.is_null() {
        // SAFETY: the handle is non-NULL (guarded above) and came from the matching init bridge; ownership C -> Rust.
        let owned = unsafe { Box::from_raw(event as *mut OsEvent) };
        obs_util::threading::os_event_destroy(Some(*owned));
    }
}

/// `EXPORT int os_event_wait(os_event_t *event)`
///
/// # Safety
/// `event` must be a live, non-NULL handle from `os_event_init` (the C
/// reference dereferences it unguarded; the conformance runner pre-checks).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_event_wait(event: *mut c_void) -> c_int {
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    obs_util::threading::os_event_wait(unsafe { &*(event as *const OsEvent) })
}

/// `EXPORT int os_event_timedwait(os_event_t *event, unsigned long
/// milliseconds)`
///
/// # Safety
/// As [`os_event_wait`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_event_timedwait(event: *mut c_void, milliseconds: c_ulong) -> c_int {
    // The C header takes `unsigned long` (64-bit on LP64); the Rust
    // primitive takes `u32` — saturate (the conformance range is far
    // below the limit).
    let ms = milliseconds.min(u32::MAX as c_ulong) as u32;
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    obs_util::threading::os_event_timedwait(unsafe { &*(event as *const OsEvent) }, ms)
}

/// `EXPORT int os_event_try(os_event_t *event)`
///
/// # Safety
/// As [`os_event_wait`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_event_try(event: *mut c_void) -> c_int {
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    obs_util::threading::os_event_try(unsafe { &*(event as *const OsEvent) })
}

/// `EXPORT int os_event_signal(os_event_t *event)`
///
/// # Safety
/// As [`os_event_wait`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_event_signal(event: *mut c_void) -> c_int {
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    obs_util::threading::os_event_signal(unsafe { &*(event as *const OsEvent) })
}

/// `EXPORT void os_event_reset(os_event_t *event)`
///
/// # Safety
/// As [`os_event_wait`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_event_reset(event: *mut c_void) {
    // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
    obs_util::threading::os_event_reset(unsafe { &*(event as *const OsEvent) })
}

/// `EXPORT int os_sem_init(os_sem_t **sem, int value)`
///
/// # Safety
/// `sem` must point to writable `os_sem_t *` storage for the whole call.
/// On success it is written with a Rust-owned handle that must be freed
/// with `os_sem_destroy`; on failure it is left unmodified (C behaviour
/// — the runner then never destroys the slot).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_sem_init(sem: *mut *mut c_void, value: c_int) -> c_int {
    match obs_util::threading::os_sem_init(value) {
        Ok(s) => {
            // SAFETY: fresh Box — the raw handle is C-owned until the matching destroy bridge (doc above).
            unsafe { *sem = Box::into_raw(Box::new(s)) as *mut c_void };
            0
        }
        Err(code) => code,
    }
}

/// `EXPORT void os_sem_destroy(os_sem_t *sem)`
///
/// # Safety
/// `sem` must be a handle returned by `os_sem_init` (or NULL — a no-op,
/// C behaviour).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_sem_destroy(sem: *mut c_void) {
    if !sem.is_null() {
        // SAFETY: the handle is non-NULL (guarded above) and came from the matching init bridge; ownership C -> Rust.
        let owned = unsafe { Box::from_raw(sem as *mut OsSem) };
        obs_util::threading::os_sem_destroy(Some(*owned));
    }
}

/// `EXPORT int os_sem_post(os_sem_t *sem)`
///
/// # Safety
/// `sem` must be a live handle from `os_sem_init` (or NULL → `-1`, C
/// behaviour).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_sem_post(sem: *mut c_void) -> c_int {
    let sem = if sem.is_null() {
        None
    } else {
        // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
        Some(unsafe { &*(sem as *const OsSem) })
    };
    obs_util::threading::os_sem_post(sem)
}

/// `EXPORT int os_sem_wait(os_sem_t *sem)`
///
/// # Safety
/// As [`os_sem_post`]. The caller must satisfy the wait (a post from
/// another thread) or the call blocks.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_sem_wait(sem: *mut c_void) -> c_int {
    let sem = if sem.is_null() {
        None
    } else {
        // SAFETY: C-ABI — the handle is non-NULL (guarded above); the value is live for the call.
        Some(unsafe { &*(sem as *const OsSem) })
    };
    obs_util::threading::os_sem_wait(sem)
}

/// `EXPORT void os_set_thread_name(const char *name)`
///
/// # Safety
/// `name` must be NULL (a no-op — the C reference would crash on the
/// `strlen`; the port never does) or a NUL-terminated string readable for
/// the whole call. Best-effort: never fails the caller (R05).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_set_thread_name(name: *const c_char) {
    if let Some(bytes) = crate::abi_common::c_bytes(name) {
        // The C glibc path reads `strlen(name)` bytes (NUL cut) and caps
        // at 15 (R05/Q2); the Rust helper applies both to the bytes.
        obs_util::threading::os_set_thread_name(bytes);
    }
}

/// `static inline void pthread_mutex_init_value(pthread_mutex_t *mutex)`
///
/// `static inline` in the C header (not linkable); this definition is
/// the linkable form (the `bstrdup` precedent).
///
/// # Safety
/// As `obs_util::threading::pthread_mutex_init_value`: `mutex` must be
/// null or point to valid, exclusively owned, unused `pthread_mutex_t`
/// storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pthread_mutex_init_value(mutex: *mut pthread_mutex_t) {
    // SAFETY: caller contract: `mutex` is valid, exclusively owned, unused storage (NULL is the documented no-op case).
    unsafe { obs_util::threading::pthread_mutex_init_value(mutex) }
}

/// `static inline int pthread_mutex_init_recursive(pthread_mutex_t *mutex)`
///
/// `static inline` in the C header (not linkable); this definition is
/// the linkable form (the `bstrdup` precedent).
///
/// # Safety
/// As `obs_util::threading::pthread_mutex_init_recursive`: `mutex` must
/// be null or point to valid, exclusively owned, unused `pthread_mutex_t`
/// storage.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn pthread_mutex_init_recursive(mutex: *mut pthread_mutex_t) -> c_int {
    // SAFETY: caller contract: `mutex` is valid, exclusively owned, unused storage (NULL is the documented no-op case).
    unsafe { obs_util::threading::pthread_mutex_init_recursive(mutex) }
}

#[cfg(test)]
mod tests {
    use super::*;

    /// The event surface round-trips through the C ABI (box handles,
    /// AUTO consumption, NULL destroy). The exact errno values (11/110)
    /// are pinned by the conformance vectors; here the bridge's
    /// forwarding is asserted (obs-ffi has no `libc` dependency — the
    /// `pthread_mutex_t` type comes through the obs-util re-export).
    #[test]
    #[cfg_attr(miri, ignore)]
    fn event_bridge_roundtrip() {
        // SAFETY: test — the handle is created by the bridge just above (asserted non-NULL) and valid for the call.
        unsafe {
            let mut ev: *mut c_void = std::ptr::null_mut();
            assert_eq!(os_event_init(&mut ev, 0), 0);
            assert!(!ev.is_null());
            let eagain = os_event_try(ev);
            assert_ne!(eagain, 0, "un-signalled try must be EAGAIN");
            assert_eq!(os_event_signal(ev), 0);
            assert_eq!(os_event_try(ev), 0); // AUTO: consumed
            assert_eq!(os_event_try(ev), eagain);
            let etimedout = os_event_timedwait(ev, 20);
            assert_ne!(etimedout, 0, "timeout must be ETIMEDOUT");
            os_event_reset(ev);
            os_event_destroy(ev);
            os_event_destroy(std::ptr::null_mut()); // NULL no-op
        }
    }

    /// The semaphore surface round-trips through the C ABI, incl. the C
    /// NULL guards and the negative-count failure (vector 04 shape).
    #[test]
    #[cfg_attr(miri, ignore)]
    fn sem_bridge_roundtrip() {
        // SAFETY: test — the handle is created by the bridge just above (asserted non-NULL) and valid for the call.
        unsafe {
            let mut sem: *mut c_void = std::ptr::null_mut();
            assert_eq!(os_sem_init(&mut sem, 1), 0);
            assert!(!sem.is_null());
            assert_eq!(os_sem_wait(sem), 0);
            assert_eq!(os_sem_post(sem), 0);
            assert_eq!(os_sem_wait(sem), 0);
            // Negative initial count fails with -1 and leaves *sem
            // unmodified (still the previous value's slot).
            let mut bad: *mut c_void = std::ptr::null_mut();
            assert_eq!(os_sem_init(&mut bad, -1), -1);
            assert!(bad.is_null());
            os_sem_destroy(sem);
            os_sem_destroy(std::ptr::null_mut()); // NULL no-op
            assert_eq!(os_sem_wait(std::ptr::null_mut()), -1);
            assert_eq!(os_sem_post(std::ptr::null_mut()), -1);
        }
    }

    /// The mutex-init helpers take the C `pthread_mutex_t *` signature.
    /// `pthread_mutex_init_value` assigns the initializer, so zeroed
    /// storage is valid before the first call (obs-ffi has no `libc`
    /// dependency for the `PTHREAD_MUTEX_INITIALIZER` constant).
    #[test]
    #[cfg_attr(miri, ignore)]
    fn mutex_bridge_init() {
        // SAFETY: POD storage for a C type; initialized by the init call below (the return code is checked).
        let mut m = unsafe { std::mem::zeroed::<pthread_mutex_t>() };
        // SAFETY: caller contract: `mutex` is valid, exclusively owned, unused storage (NULL is the documented no-op case).
        unsafe {
            pthread_mutex_init_value(&mut m);
            pthread_mutex_init_value(std::ptr::null_mut()); // NULL no-op
            assert_eq!(pthread_mutex_init_recursive(&mut m), 0);
            assert_ne!(
                pthread_mutex_init_recursive(std::ptr::null_mut()),
                0,
                "NULL must be an error (C would crash)"
            );
        }
    }
}
