//! S-061 (R01–R05, R07): threading primitives — `os_event`, `os_sem`,
//! thread naming, and the pthread mutex-init helpers (POSIX only).
//!
//! Spec-driven port of P1-05.1 (events) + P1-05.2 (semaphores, thread
//! name) from `libobs/util/threading.h` + `threading-posix.c`, and
//! P1-05.3 (WP-38) from `threading-windows.c`: events/semaphores are
//! kernel `HANDLE`s from `CreateEvent`/`CreateSemaphore` driven by
//! `WaitForSingleObject` (no SRW locks). R06 (the Windows interlocked
//! helpers in `threading-windows.h`) stays open — the P1-05.3 subtask
//! scope is the event/semaphore/thread-name surface. The two
//! mutex-init styles (R07) live in `threading.h` and are pthread-only
//! (conformance vector 06; `#[cfg(unix)]` here).
//!
//! Mechanism notes (S-061 R03/R04 free the mechanism and pin only the
//! public semantics of R01/R02):
//! - unix `os_event` is `std` `Mutex` + `Condvar`. The C code returns
//!   the raw pthread errno on internal errors; in the Rust port those
//!   error paths are unreachable (the std primitives cannot fail), so
//!   the documented errno returns are reserved and every success-path
//!   return matches the C reference byte-for-byte (`0`, `EAGAIN`,
//!   `ETIMEDOUT`).
//! - unix `os_event_timedwait` waits on a monotonic deadline (the std
//!   condvar); the C reference builds a `CLOCK_REALTIME` deadline. The
//!   observable contract is identical (R01: `0` on wake, `ETIMEDOUT` on
//!   timeout, the full deadline recomputed after every wake, as in the
//!   C loop). Windows `os_event_timedwait` passes the milliseconds to
//!   `WaitForSingleObject` (the kernel owns the deadline, as in
//!   `threading-windows.c`).
//! - `os_sem` uses `sem_t` where unnamed POSIX semaphores exist; macOS
//!   lacks them (the C reference uses Mach semaphores there — the
//!   P1-05.2 subtask) and gets a counting semaphore on `std` primitives
//!   with the same `0`/`-1` contract (R04). Windows uses
//!   `CreateSemaphore` (max count `INT_MAX`), as in the C reference.
//! - Windows error returns mirror `threading-windows.c`: `EAGAIN` /
//!   `ETIMEDOUT` / `EINVAL` are the platform errno constants (`libc`),
//!   exactly as the C reference's constants resolve per platform.
//! - Windows `os_set_thread_name` (S-061 R05) is `SetThreadDescription`
//!   (UTF-16), looked up in `KernelBase.dll` exactly as the C reference
//!   does. The C MSVC-only `__try`/`__except` + `RaiseException(0x406D1388)`
//!   VS-debugger thread-name trick (the `NO_SEH_MINGW` dual branch) is
//!   omitted: S-061 R05 pins `SetThreadDescription` as the mechanism
//!   (unsupported → silently ignored), and Rust cannot emit SEH.
//!
//! The `unsafe` blocks are platform FFI calls — `libc` on unix,
//! `crate::winapi` externs on Windows (the platform tier, same class as
//! `dl`/`pipe`/`os_proc`; ADR 0005 Amendment 1).

#[cfg(unix)]
use std::sync::{Condvar, Mutex};

/// C `pthread_mutex_t` (POSIX only) — re-exported from `libc` so the FFI
/// bridge in obs-ffi can name the type in the exact C signature without
/// taking its own `libc` dependency.
#[cfg(unix)]
pub use libc::pthread_mutex_t;

/// The event/semaphore guards cannot be poisoned: the only code that runs
/// while the lock is held is an integer read/write (nothing panics), so
/// `expect` here documents the invariant rather than recovering a real
/// failure.
#[cfg(unix)]
const LOCK_NOT_POISONED: &str = "not poisoned: only integer read/writes run under the lock";

/// `enum os_event_type` (S-061 R01).
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
#[repr(u32)]
pub enum OsEventType {
    /// Resets when any waiter is woken — the default (C `OS_EVENT_TYPE_AUTO`).
    Auto = 0,
    /// Stays signalled until explicitly reset (C `OS_EVENT_TYPE_MANUAL`).
    Manual = 1,
}

/// C `struct os_event_data` (unix): the signalled flag, the condvar it is
/// protected by, and the manual-reset flag.
#[cfg(unix)]
pub struct OsEvent {
    signalled: Mutex<bool>,
    cond: Condvar,
    manual: bool,
}

/// C `struct os_event_data` (Windows): the `CreateEvent` kernel object —
/// auto/manual reset is a property of the handle (the C reference
/// `os_event_t` IS the `HANDLE`).
#[cfg(windows)]
pub struct OsEvent {
    handle: *mut std::os::raw::c_void,
}

#[cfg(unix)]
impl std::fmt::Debug for OsEvent {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        // `Condvar` is not `Debug`; the reset mode is the useful part.
        f.debug_struct("OsEvent")
            .field("manual", &self.manual)
            .finish_non_exhaustive()
    }
}

#[cfg(windows)]
impl std::fmt::Debug for OsEvent {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        f.debug_struct("OsEvent")
            .field("handle", &self.handle)
            .finish()
    }
}

// SAFETY: `OsEvent.handle` is a kernel event object: every Win32 call on
// it (`WaitForSingleObject`, `SetEvent`, `ResetEvent`, `CloseHandle`) is
// documented thread-safe, and the handle is consumed (closed) exactly
// once by `os_event_destroy`.
#[cfg(windows)]
unsafe impl Send for OsEvent {}
// SAFETY: as for `Send` above — shared use is through the thread-safe
// Win32 kernel API; closing happens once, in `os_event_destroy`.
#[cfg(windows)]
unsafe impl Sync for OsEvent {}

/// `os_event_init` (S-061 R01/R02): creates an event of the given reset
/// type. C returns `0` on success and a platform error code on failure;
/// `Err` carries that code. On unix the std primitives cannot fail, so
/// this always succeeds (`Err` is reserved).
pub fn os_event_init(kind: OsEventType) -> Result<OsEvent, i32> {
    #[cfg(unix)]
    {
        Ok(OsEvent {
            signalled: Mutex::new(false),
            cond: Condvar::new(),
            manual: kind == OsEventType::Manual,
        })
    }
    #[cfg(windows)]
    {
        use crate::winapi;
        // SAFETY: null attributes/name are the unnamed-object form
        // (threading-windows.c); the kernel returns the handle or NULL.
        let handle = unsafe {
            winapi::CreateEventA(
                std::ptr::null(),
                i32::from(kind == OsEventType::Manual),
                0,
                std::ptr::null(),
            )
        };
        if handle.is_null() {
            Err(-1)
        } else {
            Ok(OsEvent { handle })
        }
    }
}

/// `os_event_destroy` (S-061 R02): releases the object (Rust `Drop`
/// releases the unix primitives); this mirrors the C lifecycle including
/// the NULL no-op (vector 01 `edest @null`), represented as `None`.
pub fn os_event_destroy(event: Option<OsEvent>) {
    #[cfg(unix)]
    {
        let _ = event;
    }
    #[cfg(windows)]
    if let Some(event) = event {
        // SAFETY: `event.handle` was created by `os_event_init` and is
        // closed exactly once (this function consumes the `OsEvent`).
        unsafe { crate::winapi::CloseHandle(event.handle) };
    }
}

/// `os_event_wait` (S-061 R01): blocks until signalled and returns `0`.
/// An `Auto` event is consumed by the waking waiter, a `Manual` event
/// stays set. The unix C code additionally returns the raw pthread errno
/// on internal errors — unreachable in the port (see the module docs),
/// so that path is `0`; the Windows C code returns `EINVAL` on a
/// non-`WAIT_OBJECT_0` result (e.g. a broken handle).
pub fn os_event_wait(event: &OsEvent) -> i32 {
    #[cfg(unix)]
    {
        let mut signalled = event.signalled.lock().expect(LOCK_NOT_POISONED);
        while !*signalled {
            signalled = event.cond.wait(signalled).expect(LOCK_NOT_POISONED);
        }
        if !event.manual {
            *signalled = false;
        }
        0
    }
    #[cfg(windows)]
    {
        use crate::winapi;
        // SAFETY: `event.handle` is a live kernel event object
        // (threading-windows.c: INFINITE wait, EINVAL on any other code).
        let code = unsafe { winapi::WaitForSingleObject(event.handle, winapi::INFINITE) };
        if code == winapi::WAIT_OBJECT_0 {
            0
        } else {
            libc::EINVAL
        }
    }
}

/// `os_event_timedwait` (S-061 R01): like [`os_event_wait`] but with a
/// timeout in milliseconds: `0` on wake, `ETIMEDOUT` on timeout — callers
/// compare against that constant, part of the public contract.
#[cfg(unix)]
pub fn os_event_timedwait(event: &OsEvent, milliseconds: u32) -> i32 {
    use std::time::Instant;
    // The C reference builds an absolute deadline and recomputes the
    // remaining time after every wake; `wait_timeout` is relative, so
    // the absolute deadline is kept with `Instant` (R01; R03 frees the
    // clock — monotonic is the std mechanism).
    let deadline = std::time::Duration::from_millis(u64::from(milliseconds));
    let start = Instant::now();
    let mut signalled = event.signalled.lock().expect(LOCK_NOT_POISONED);
    loop {
        let remaining = deadline.saturating_sub(start.elapsed());
        if remaining.is_zero() {
            break;
        }
        let (guard, outcome) = event
            .cond
            .wait_timeout(signalled, remaining)
            .expect(LOCK_NOT_POISONED);
        signalled = guard;
        if *signalled || outcome.timed_out() {
            break;
        }
        // Spurious wake before the deadline: the recomputed remaining
        // time keeps the absolute deadline intact.
    }
    // A signal landing exactly at the deadline still wakes (the C loop
    // re-checks the flag after the timed wait and returns 0 if set).
    if *signalled {
        if !event.manual {
            *signalled = false;
        }
        0
    } else {
        libc::ETIMEDOUT
    }
}

#[cfg(windows)]
pub fn os_event_timedwait(event: &OsEvent, milliseconds: u32) -> i32 {
    use crate::winapi;
    // SAFETY: `event.handle` is a live kernel event object; the kernel
    // owns the absolute deadline (threading-windows.c passes the
    // milliseconds through verbatim).
    let code = unsafe { winapi::WaitForSingleObject(event.handle, milliseconds) };
    match code {
        winapi::WAIT_TIMEOUT => libc::ETIMEDOUT,
        winapi::WAIT_OBJECT_0 => 0,
        _ => libc::EINVAL,
    }
}

/// `os_event_try` (S-061 R01): non-blocking — `0` if signalled
/// (consumed for `Auto`), `EAGAIN` if not. The audio thread loops on the
/// `EAGAIN` return (P1-05.1 pitfall).
pub fn os_event_try(event: &OsEvent) -> i32 {
    #[cfg(unix)]
    {
        let mut signalled = event.signalled.lock().expect(LOCK_NOT_POISONED);
        if *signalled {
            if !event.manual {
                *signalled = false;
            }
            0
        } else {
            libc::EAGAIN
        }
    }
    #[cfg(windows)]
    {
        use crate::winapi;
        // SAFETY: `event.handle` is a live kernel event object; the zero-
        // timeout wait never blocks (threading-windows.c).
        let code = unsafe { winapi::WaitForSingleObject(event.handle, 0) };
        match code {
            winapi::WAIT_TIMEOUT => libc::EAGAIN,
            winapi::WAIT_OBJECT_0 => 0,
            _ => libc::EINVAL,
        }
    }
}

/// `os_event_signal` (S-061 R02): sets the event and wakes a waiting
/// thread. `Auto` events are consumed by the first waiter, `Manual`
/// events stay set. Returns `0` (on unix the C `pthread_cond_signal`
/// errno is unreachable; on Windows `SetEvent` failure → `EINVAL`, as in
/// the C reference).
///
/// Like the C reference (pthread `notify_one` / `SetEvent` on the kernel
/// object), at most one waiter is woken; concurrent waiters on a
/// `Manual` event re-check as they do in C.
pub fn os_event_signal(event: &OsEvent) -> i32 {
    #[cfg(unix)]
    {
        let mut signalled = event.signalled.lock().expect(LOCK_NOT_POISONED);
        *signalled = true;
        drop(signalled);
        event.cond.notify_one();
        0
    }
    #[cfg(windows)]
    {
        // SAFETY: `event.handle` is a live kernel event object;
        // `SetEvent` is thread-safe.
        let ok = unsafe { crate::winapi::SetEvent(event.handle) } != 0;
        if ok { 0 } else { libc::EINVAL }
    }
}

/// `os_event_reset` (S-061 R02): clears the event (the `Manual` flag on
/// unix; `ResetEvent` on Windows — the return value is ignored, as in
/// the C reference).
pub fn os_event_reset(event: &OsEvent) {
    #[cfg(unix)]
    {
        *event.signalled.lock().expect(LOCK_NOT_POISONED) = false;
    }
    #[cfg(windows)]
    {
        // SAFETY: `event.handle` is a live kernel event object.
        unsafe { crate::winapi::ResetEvent(event.handle) };
    }
}

/// C `struct os_sem_data` (unix): a counting semaphore (S-061 R04). On
/// macOS — which lacks unnamed POSIX semaphores — this is a counting
/// semaphore on `std` primitives (the C reference uses Mach semaphores
/// there; the public contract is the same).
#[cfg(all(unix, not(target_os = "macos")))]
pub struct OsSem {
    sem: libc::sem_t,
}

#[cfg(target_os = "macos")]
pub struct OsSem {
    count: Mutex<u32>,
    cond: Condvar,
}

/// C `struct os_sem_data` (Windows): the `CreateSemaphore` kernel
/// object (max count `INT_MAX`, as in `threading-windows.c`).
#[cfg(windows)]
pub struct OsSem {
    handle: *mut std::os::raw::c_void,
}

// SAFETY: `OsSem.handle` is a kernel semaphore object: every Win32 call
// on it (`WaitForSingleObject`, `ReleaseSemaphore`, `CloseHandle`) is
// documented thread-safe, and the handle is consumed (closed) exactly
// once by `os_sem_destroy`.
#[cfg(windows)]
unsafe impl Send for OsSem {}
// SAFETY: as for `Send` above — shared use is through the thread-safe
// Win32 kernel API; closing happens once, in `os_sem_destroy`.
#[cfg(windows)]
unsafe impl Sync for OsSem {}

impl std::fmt::Debug for OsSem {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        // The interior (`sem_t` / `Mutex`) is not `Debug`.
        f.debug_struct("OsSem").finish_non_exhaustive()
    }
}

/// `os_sem_init` (S-061 R04): a counting semaphore with the given
/// initial count. `Err(-1)` mirrors the C failure return (`sem_init`
/// fails with `EINVAL` on a negative count — vector 04 `sem c -1` →
/// `fail rc=-1`).
pub fn os_sem_init(value: i32) -> Result<OsSem, i32> {
    // The C reference's `sem_init` fails with EINVAL on a negative count
    // (vector 04 `sem c -1` → rc=-1); the early check keeps the same
    // contract without depending on the platform errno (the Windows
    // reference relies on `CreateSemaphore` rejecting it — same result).
    if value < 0 {
        return Err(-1);
    }
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        let mut sem = std::mem::MaybeUninit::<libc::sem_t>::uninit();
        // SAFETY: `sem` is valid storage for a `sem_t`; `sem_init`
        // initializes it fully on success.
        let rc = unsafe { libc::sem_init(sem.as_mut_ptr(), 0, value as u32) };
        if rc != 0 {
            return Err(-1);
        }
        Ok(OsSem {
            // SAFETY: `sem_init` succeeded (rc == 0 checked above), so the storage is initialized.
            sem: unsafe { sem.assume_init() },
        })
    }
    #[cfg(target_os = "macos")]
    {
        Ok(OsSem {
            count: Mutex::new(value as u32),
            cond: Condvar::new(),
        })
    }
    #[cfg(windows)]
    {
        use crate::winapi;
        // SAFETY: null attributes/name are the unnamed-object form
        // (threading-windows.c); the kernel returns the handle or NULL.
        let handle = unsafe {
            winapi::CreateSemaphoreA(std::ptr::null(), value, 0x7fffffff, std::ptr::null())
        };
        if handle.is_null() {
            Err(-1)
        } else {
            Ok(OsSem { handle })
        }
    }
}

/// `os_sem_destroy` (S-061 R04): releases the semaphore; `None` is the C
/// NULL no-op (vector 04 `sdest @null`).
#[cfg(all(unix, not(target_os = "macos")))]
pub fn os_sem_destroy(sem: Option<OsSem>) {
    if let Some(mut sem) = sem {
        // SAFETY: `sem.sem` was created by `os_sem_init` and is destroyed
        // exactly once (this function consumes it).
        unsafe { libc::sem_destroy(&mut sem.sem) };
    }
}

#[cfg(target_os = "macos")]
pub fn os_sem_destroy(_sem: Option<OsSem>) {}

#[cfg(windows)]
pub fn os_sem_destroy(sem: Option<OsSem>) {
    if let Some(sem) = sem {
        // SAFETY: `sem.handle` was created by `os_sem_init` and is
        // closed exactly once (this function consumes the `OsSem`).
        unsafe { crate::winapi::CloseHandle(sem.handle) };
    }
}

/// `os_sem_post` (S-061 R04): increments the count and wakes a waiting
/// thread. `None` is the C NULL guard → `-1` (vector 04 `spost @null`).
/// Returns `0` on success, `-1` on failure.
pub fn os_sem_post(sem: Option<&OsSem>) -> i32 {
    let Some(sem) = sem else {
        return -1;
    };
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        // SAFETY: `sem.sem` is a live, initialized semaphore; POSIX `sem_post`
        // is thread-safe (it manipulates the futex word atomically), so the
        // shared-to-mutable pointer is sound.
        let rc = unsafe { libc::sem_post(std::ptr::addr_of!(sem.sem).cast_mut()) };
        if rc == 0 { 0 } else { -1 }
    }
    #[cfg(target_os = "macos")]
    {
        let mut count = sem.count.lock().expect(LOCK_NOT_POISONED);
        *count += 1;
        sem.cond.notify_one();
        0
    }
    #[cfg(windows)]
    {
        // SAFETY: `sem.handle` is a live kernel semaphore; `ReleaseSemaphore`
        // is thread-safe (threading-windows.c).
        let ok =
            unsafe { crate::winapi::ReleaseSemaphore(sem.handle, 1, std::ptr::null_mut()) } != 0;
        if ok { 0 } else { -1 }
    }
}

/// `os_sem_wait` (S-061 R04): blocks until the count is > 0 and
/// decrements it. `None` is the C NULL guard → `-1` (vector 04 `snull`).
/// Returns `0` on success, `-1` on failure.
pub fn os_sem_wait(sem: Option<&OsSem>) -> i32 {
    let Some(sem) = sem else {
        return -1;
    };
    #[cfg(all(unix, not(target_os = "macos")))]
    {
        // SAFETY: as in `os_sem_post`; the caller must satisfy the wait — the
        // runner's `wsemwake` pattern.
        let rc = unsafe { libc::sem_wait(std::ptr::addr_of!(sem.sem).cast_mut()) };
        if rc == 0 { 0 } else { -1 }
    }
    #[cfg(target_os = "macos")]
    {
        let mut count = sem.count.lock().expect(LOCK_NOT_POISONED);
        while *count == 0 {
            count = sem.cond.wait(count).expect(LOCK_NOT_POISONED);
        }
        *count -= 1;
        0
    }
    #[cfg(windows)]
    {
        use crate::winapi;
        // SAFETY: `sem.handle` is a live kernel semaphore; the caller must
        // satisfy the wait (threading-windows.c: INFINITE wait).
        let ret = unsafe { winapi::WaitForSingleObject(sem.handle, winapi::INFINITE) };
        if ret == winapi::WAIT_OBJECT_0 { 0 } else { -1 }
    }
}

/// `os_set_thread_name` (S-061 R05): best-effort — the platform return
/// code is discarded and the call never fails the caller.
///
/// `name` is the raw C byte string (the glibc path reads `strlen(name)`
/// bytes — a C string is bytes, not necessarily UTF-8).
///
/// glibc caps pthread names at 15 bytes; the C reference pre-truncates
/// with `bstrdup_n(name, 15)` (S-061 Q2) because `pthread_setname_np`
/// would reject the longer name. Windows has no such cap: `SetThreadDescription`
/// (looked up in `KernelBase.dll` as in the C reference) takes the full
/// NUL-cut name, UTF-8→UTF-16. The Apple/FreeBSD paths pass the name
/// through untruncated (not ported — silent no-ops); other unix targets
/// (including Android, which has no branch in the C reference) are
/// silent no-ops.
pub fn os_set_thread_name(name: &[u8]) {
    // R05: best-effort — the platform error is discarded.
    #[cfg(target_os = "linux")]
    {
        let bytes = thread_name_bytes(name);
        let mut c = Vec::with_capacity(bytes.len() + 1);
        c.extend_from_slice(bytes);
        c.push(0);
        // SAFETY: `c` is a valid NUL-terminated C string for the duration
        // of the call. The cast keeps this portable across the `c_char`
        // sign difference (x86_64: i8, aarch64: u8).
        unsafe {
            libc::pthread_setname_np(libc::pthread_self(), c.as_ptr().cast::<libc::c_char>())
        };
    }
    #[cfg(windows)]
    {
        use crate::winapi;
        use std::os::raw::c_void;
        // S-061 R05: `SetThreadDescription` with the UTF-16 conversion —
        // loaded from `KernelBase.dll` by name, exactly as the C
        // reference does (old SDKs lack the symbol). The C MSVC-only
        // `__try`/`__except` + `RaiseException(0x406D1388)` VS-debugger
        // trick (the `NO_SEH_MINGW` dual branch) is omitted: S-061 R05
        // pins `SetThreadDescription` as the mechanism, and Rust cannot
        // emit SEH.
        let end = name.iter().position(|&c| c == 0).unwrap_or(name.len());
        let wide: Vec<u16> = String::from_utf8_lossy(&name[..end])
            .encode_utf16()
            .chain(std::iter::once(0))
            .collect();
        // SAFETY: the literal is a valid NUL-terminated byte string for the call.
        let module = unsafe { winapi::LoadLibraryA(c"KernelBase.dll".as_ptr().cast::<u8>()) };
        if !module.is_null() {
            // SAFETY: the literal is a valid NUL-terminated byte string.
            let proc = unsafe {
                winapi::GetProcAddress(module, c"SetThreadDescription".as_ptr().cast::<u8>())
            };
            if !proc.is_null() {
                // SAFETY: `proc` is the address of kernelbase's
                // `SetThreadDescription` (Win10 1607+); the transmute is a
                // pointer-sized function-item cast (same size/layout as the
                // C `set_thread_description_t` typedef).
                let set_desc =
                    unsafe { std::mem::transmute::<*mut c_void, SetThreadDescriptionFn>(proc) };
                // SAFETY: `wide` is a live NUL-terminated UTF-16 string for the
                // call; `GetCurrentThread()` is a valid pseudo-handle.
                unsafe { set_desc(winapi::GetCurrentThread(), wide.as_ptr()) };
            }
            // SAFETY: `module` was loaded above and is not used afterwards.
            unsafe { winapi::FreeLibrary(module) };
        }
    }
    // shortcut: the macOS (`pthread_setname_np`) and FreeBSD pass-through
    // names are no-ops — no owning subtask (P1-05.2 was ticked with this
    // gap); wire them when a macOS-threaded build needs them (R05:
    // unsupported → silently ignored).
    #[cfg(not(any(target_os = "linux", windows)))]
    {
        let _ = name;
    }
}

/// C `set_thread_description_t` (threading-windows.c):
/// `HRESULT SetThreadDescription(HANDLE hThread, PCWSTR pszName)`.
#[cfg(windows)]
type SetThreadDescriptionFn =
    unsafe extern "system" fn(*mut std::os::raw::c_void, *const u16) -> i32;

/// The bytes the platform actually receives for `name`: cut at the first
/// NUL (the C `strlen` semantics of every platform path), and on glibc
/// truncated to the 15-byte pthread-name limit (S-061 R05 + Q2 — the C
/// reference pre-truncates with `bstrdup_n(name, 15)`).
#[cfg(target_os = "linux")]
fn thread_name_bytes(name: &[u8]) -> &[u8] {
    let end = name.iter().position(|&c| c == 0).unwrap_or(name.len());
    // glibc pthread names are capped at 15 chars (S-061 Q2); the C
    // reference pre-truncates with `bstrdup_n(name, 15)`.
    &name[..end.min(15)]
}

/// `pthread_mutex_init_value` (S-061 R07, POSIX only): assigns
/// `PTHREAD_MUTEX_INITIALIZER` to `*mutex` — one of the two mutex-init
/// styles the rest of libobs uses. C NULL check: a null pointer is a
/// no-op.
///
/// # Safety
///
/// `mutex` must be null or point to valid, exclusively owned storage for
/// a `pthread_mutex_t` that is not currently in use.
#[cfg(unix)]
pub unsafe fn pthread_mutex_init_value(mutex: *mut libc::pthread_mutex_t) {
    if mutex.is_null() {
        return;
    }
    // SAFETY: documented caller contract — valid, exclusively owned,
    // unused mutex storage.
    unsafe { *mutex = libc::PTHREAD_MUTEX_INITIALIZER };
}

/// `pthread_mutex_init_recursive` (S-061 R07, POSIX only): initializes
/// `*mutex` with the recursive attribute; `0` on success, errno on
/// failure. A null pointer returns `EINVAL` — the C reference would
/// crash (NULL deref); the port never does (the D-1-class rule).
///
/// # Safety
///
/// `mutex` must be null or point to valid, exclusively owned storage for
/// a `pthread_mutex_t` that is not currently in use.
#[cfg(unix)]
pub unsafe fn pthread_mutex_init_recursive(mutex: *mut libc::pthread_mutex_t) -> i32 {
    if mutex.is_null() {
        return libc::EINVAL;
    }
    // SAFETY: `pthread_mutexattr_t` is plain old data; zeroed storage is valid until `pthread_mutexattr_init` (whose rc is checked).
    let mut attr: libc::pthread_mutexattr_t = unsafe { std::mem::zeroed() };
    // SAFETY: `attr` is exclusively owned and fully initialized by
    // `pthread_mutexattr_init` before any use; `mutex` (documented caller
    // contract) is valid, exclusively owned, unused storage for a
    // pthread_mutex_t. The attr cleanup mirrors the C reference exactly
    // (destroyed only when `pthread_mutexattr_init` succeeded).
    let rc = unsafe { libc::pthread_mutexattr_init(&mut attr) };
    if rc == 0 {
        let rc =
            // SAFETY: `attr` is initialized above (`pthread_mutexattr_init` succeeded).
            unsafe { libc::pthread_mutexattr_settype(&mut attr, libc::PTHREAD_MUTEX_RECURSIVE) };
        if rc == 0 {
            // SAFETY: `mutex` is valid, exclusively owned storage (documented caller contract); `attr` is initialized.
            let rc = unsafe { libc::pthread_mutex_init(mutex, &attr) };
            // SAFETY: `attr` is valid (`pthread_mutexattr_init` succeeded above).
            unsafe { libc::pthread_mutexattr_destroy(&mut attr) };
            return rc;
        }
        // SAFETY: `attr` is valid (`pthread_mutexattr_init` succeeded above).
        unsafe { libc::pthread_mutexattr_destroy(&mut attr) };
    }
    rc
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::Arc;

    // The conformance vectors print `11` (EAGAIN) and `110` (ETIMEDOUT)
    // — the glibc values of the public-contract constants (S-061 §2).
    const EAGAIN: i32 = libc::EAGAIN;
    const ETIMEDOUT: i32 = libc::ETIMEDOUT;

    #[test]
    #[cfg(target_os = "linux")]
    fn errno_constants_match_conformance_host() {
        assert_eq!(libc::EAGAIN, 11);
        assert_eq!(libc::ETIMEDOUT, 110);
    }

    /// The conformance runner's worker thread
    /// (`gen/threading_runner.c`): a blocking operation is dispatched to
    /// a worker and main blocks until the result is reported. Here: one
    /// thread per command, the same ordering guarantee.
    fn worker_rc(op: impl FnOnce() -> i32 + Send + 'static) -> i32 {
        let (tx, rx) = std::sync::mpsc::channel();
        let thread = std::thread::spawn(move || {
            let rc = op();
            let _ = tx.send(rc);
        });
        let rc = rx.recv().expect("worker sends its result");
        thread.join().expect("worker does not panic");
        rc
    }

    /// Test helper: `os_event_init` without the `Debug` bound
    /// (`Condvar` does not derive it).
    fn event(kind: OsEventType) -> OsEvent {
        match os_event_init(kind) {
            Ok(e) => e,
            Err(rc) => panic!("os_event_init failed: rc={rc}"),
        }
    }

    /// Test helper: `os_sem_init` without the `Debug` bound.
    fn sem(value: i32) -> OsSem {
        match os_sem_init(value) {
            Ok(s) => s,
            Err(rc) => panic!("os_sem_init({value}) failed: rc={rc}"),
        }
    }

    // vector 01-events-auto (S-061 R01/R02): AUTO lifecycle — try/signal/
    // wait consumption, destroy (incl. NULL).
    #[test]
    fn v01_events_auto() {
        // evt a auto → ok
        let a = Arc::new(event(OsEventType::Auto));
        // etry a 11: not signalled
        assert_eq!(os_event_try(&a), EAGAIN);
        // esig a 0
        assert_eq!(os_event_signal(&a), 0);
        // etry a 0: signalled, consumed (AUTO)
        assert_eq!(os_event_try(&a), 0);
        // etry a 11: consumed by the previous try
        assert_eq!(os_event_try(&a), EAGAIN);
        // esig a 0; wwait a 0: the wait consumes; etry a 11
        assert_eq!(os_event_signal(&a), 0);
        let a1 = Arc::clone(&a);
        assert_eq!(worker_rc(move || os_event_wait(&a1)), 0);
        assert_eq!(os_event_try(&a), EAGAIN);
        // esig a 0; wtimed a 20 0: pre-signalled timedwait
        assert_eq!(os_event_signal(&a), 0);
        let a2 = Arc::clone(&a);
        assert_eq!(worker_rc(move || os_event_timedwait(&a2, 20)), 0);
        // ereset a; etry a 11
        os_event_reset(&a);
        assert_eq!(os_event_try(&a), EAGAIN);
        // edest a; edest @null
        let owned = Arc::try_unwrap(a).expect("single owner");
        os_event_destroy(Some(owned));
        os_event_destroy(None);
    }

    // vector 02-events-manual (S-061 R02/R03): MANUAL stays set until
    // reset; multiple waiters.
    #[test]
    fn v02_events_manual() {
        // evt b manual → ok
        let b = Arc::new(event(OsEventType::Manual));
        // etry b 11
        assert_eq!(os_event_try(&b), EAGAIN);
        // esig b 0; etry b 0; etry b 0: MANUAL is not consumed by try
        assert_eq!(os_event_signal(&b), 0);
        assert_eq!(os_event_try(&b), 0);
        assert_eq!(os_event_try(&b), 0);
        // wwait b 0; wwait b 0: the wait does not consume a MANUAL event
        let b1 = Arc::clone(&b);
        assert_eq!(worker_rc(move || os_event_wait(&b1)), 0);
        let b2 = Arc::clone(&b);
        assert_eq!(worker_rc(move || os_event_wait(&b2)), 0);
        // ereset b; etry b 11
        os_event_reset(&b);
        assert_eq!(os_event_try(&b), EAGAIN);
        // wtimed b 30 110: timeout after the reset
        let b3 = Arc::clone(&b);
        assert_eq!(worker_rc(move || os_event_timedwait(&b3, 30)), ETIMEDOUT);
        // esig b 0; esig b 0 (double signal); wwait b 0
        assert_eq!(os_event_signal(&b), 0);
        assert_eq!(os_event_signal(&b), 0);
        let b4 = Arc::clone(&b);
        assert_eq!(worker_rc(move || os_event_wait(&b4)), 0);
        let owned = Arc::try_unwrap(b).expect("single owner");
        os_event_destroy(Some(owned));
    }

    // vector 03-event-timeout (S-061 R01/R03): timedwait timeout vs.
    // pre-signal vs. signal-during-wait.
    #[test]
    fn v03_event_timeout() {
        // evt c auto; wtimed c 40 110: pure timeout
        let c = Arc::new(event(OsEventType::Auto));
        let c1 = Arc::clone(&c);
        assert_eq!(worker_rc(move || os_event_timedwait(&c1, 40)), ETIMEDOUT);
        // esig c 0; wtimed c 40 0: pre-signal
        assert_eq!(os_event_signal(&c), 0);
        let c2 = Arc::clone(&c);
        assert_eq!(worker_rc(move || os_event_timedwait(&c2, 40)), 0);
        // esig c 0; wwait c 0
        assert_eq!(os_event_signal(&c), 0);
        let c3 = Arc::clone(&c);
        assert_eq!(worker_rc(move || os_event_wait(&c3)), 0);

        // evt d manual; wtimedwake d 200 0: main signals right after
        // dispatch — both orderings (signal before the wait starts, or
        // waking an in-flight wait) return 0, so the result is
        // deterministic (the runner's guarantee).
        let d = Arc::new(event(OsEventType::Manual));
        let d1 = Arc::clone(&d);
        let waiter = std::thread::spawn(move || os_event_timedwait(&d1, 200));
        os_event_signal(&d);
        assert_eq!(waiter.join().expect("waiter"), 0);
        // etry d 0: MANUAL stayed set through the timedwait
        assert_eq!(os_event_try(&d), 0);
        // ereset d; etry d 11
        os_event_reset(&d);
        assert_eq!(os_event_try(&d), EAGAIN);

        // evt e auto; esig e 0; wtry e 0; wtry e 11
        let e = Arc::new(event(OsEventType::Auto));
        assert_eq!(os_event_signal(&e), 0);
        let e1 = Arc::clone(&e);
        assert_eq!(worker_rc(move || os_event_try(&e1)), 0);
        let e2 = Arc::clone(&e);
        assert_eq!(worker_rc(move || os_event_try(&e2)), EAGAIN);

        let c_ = Arc::try_unwrap(c).expect("single owner");
        let d_ = Arc::try_unwrap(d).expect("single owner");
        let e_ = Arc::try_unwrap(e).expect("single owner");
        os_event_destroy(Some(c_));
        os_event_destroy(Some(d_));
        os_event_destroy(Some(e_));
    }

    // vector 04-semaphores (S-061 R04): counting, wake of a waiting
    // thread, NULL guards, negative initial count.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn v04_semaphores() {
        // sem a 2 ok; wsem a 0; wsem a 0; spost a 1 0; wsem a 0
        let a = Arc::new(sem(2));
        let a1 = Arc::clone(&a);
        assert_eq!(worker_rc(move || os_sem_wait(Some(&a1))), 0);
        let a2 = Arc::clone(&a);
        assert_eq!(worker_rc(move || os_sem_wait(Some(&a2))), 0);
        assert_eq!(os_sem_post(Some(&a)), 0);
        let a3 = Arc::clone(&a);
        assert_eq!(worker_rc(move || os_sem_wait(Some(&a3))), 0);
        let owned = Arc::try_unwrap(a).expect("single owner");
        os_sem_destroy(Some(owned));

        // sem b 0 ok; wsemwake b 1 0; wsemwake b 2 0: main posts right
        // after dispatch — post-before-wait and wake-of-in-flight-wait
        // both return 0.
        let b = Arc::new(sem(0));
        let b1 = Arc::clone(&b);
        let waiter = std::thread::spawn(move || os_sem_wait(Some(&b1)));
        os_sem_post(Some(&b));
        assert_eq!(waiter.join().expect("waiter"), 0);
        let b2 = Arc::clone(&b);
        let waiter = std::thread::spawn(move || os_sem_wait(Some(&b2)));
        os_sem_post(Some(&b));
        os_sem_post(Some(&b));
        assert_eq!(waiter.join().expect("waiter"), 0);
        let owned = Arc::try_unwrap(b).expect("single owner");
        os_sem_destroy(Some(owned));

        // sem c -1 → fail rc=-1 (the slot is left empty — never destroy)
        match os_sem_init(-1) {
            Err(rc) => assert_eq!(rc, -1),
            Ok(_) => panic!("os_sem_init(-1) must fail"),
        }

        // snull wait=-1 post=-1; sdest @null; spost @null 1 -1
        assert_eq!(os_sem_wait(None), -1);
        assert_eq!(os_sem_post(None), -1);
        os_sem_destroy(None);
        assert_eq!(os_sem_post(None), -1);
    }

    // vector 05-thread-name (S-061 R05): glibc read-back via
    // pthread_getname_np; the 15-char truncation; the empty name.
    #[test]
    #[cfg(target_os = "linux")]
    #[cfg_attr(miri, ignore)]
    fn v05_thread_name() {
        assert_eq!(worker_name(b"hello"), "hello");
        assert_eq!(worker_name(b"abcdefghijklmno"), "abcdefghijklmno");
        assert_eq!(worker_name(b"abcdefghijklmnop"), "abcdefghijklmno");
        assert_eq!(worker_name(b"a b c"), "a b c");
        assert_eq!(worker_name(b""), "");
    }

    /// Sets the name on a worker thread and reads it back with
    /// `pthread_getname_np` (mirroring the C runner's `wname`).
    #[cfg(target_os = "linux")]
    fn worker_name(name: &[u8]) -> String {
        let name = name.to_owned();
        let (tx, rx) = std::sync::mpsc::channel();
        let thread = std::thread::spawn(move || {
            os_set_thread_name(&name);
            let mut buf = [0 as libc::c_char; 64];
            // SAFETY: `buf` is a valid 64-byte buffer; glibc returns 0.
            let rc = unsafe {
                libc::pthread_getname_np(libc::pthread_self(), buf.as_mut_ptr(), buf.len())
            };
            let end = buf.iter().position(|&c| c == 0).unwrap_or(buf.len());
            // SAFETY: test — `buf` holds the bytes written above; `end + 1` bytes are valid.
            let got = unsafe { std::slice::from_raw_parts(buf.as_ptr().cast::<u8>(), end + 1) };
            assert_eq!(rc, 0, "pthread_getname_np");
            let _ = tx.send(String::from_utf8_lossy(&got[..end]).into_owned());
        });
        thread.join().expect("name worker");
        rx.recv().expect("name sent")
    }

    // The glibc 15-byte truncation and the NUL cut (S-061 R05 + Q2), as
    // pure logic (Miri-runnable).
    #[test]
    #[cfg(target_os = "linux")]
    fn thread_name_bytes_truncates() {
        assert_eq!(thread_name_bytes(b"hello"), b"hello");
        assert_eq!(thread_name_bytes(b"abcdefghijklmno"), b"abcdefghijklmno");
        assert_eq!(thread_name_bytes(b"abcdefghijklmnop"), b"abcdefghijklmno");
        assert_eq!(thread_name_bytes(b"a b c"), b"a b c");
        assert_eq!(thread_name_bytes(b""), b"");
        assert_eq!(thread_name_bytes(b"abc\0def"), b"abc");
    }

    // vector 06-mutex-init (S-061 R07): the two init styles; the
    // recursive one allows a re-entrant lock. (POSIX only — pthread.)
    #[test]
    #[cfg(unix)]
    #[cfg_attr(miri, ignore)]
    fn v06_mutex_init() {
        let mut m = libc::PTHREAD_MUTEX_INITIALIZER;
        // mval
        // SAFETY: `m` is exclusive stack storage, just initialized.
        unsafe { pthread_mutex_init_value(&mut m) };
        // mlock 0; munlock 0
        // SAFETY: `m` was just initialized here and is not locked.
        assert_eq!(unsafe { libc::pthread_mutex_lock(&mut m) }, 0);
        // SAFETY: test — `m` is a live recursive mutex (initialized above).
        assert_eq!(unsafe { libc::pthread_mutex_unlock(&mut m) }, 0);
        // mrec 0
        // SAFETY: `m` is exclusive and unlocked at this point.
        assert_eq!(unsafe { pthread_mutex_init_recursive(&mut m) }, 0);
        // mlock 0; mlock 0 (re-entrant); munlock 0; munlock 0
        // SAFETY: test — `m` is a live recursive mutex (initialized above).
        assert_eq!(unsafe { libc::pthread_mutex_lock(&mut m) }, 0);
        // SAFETY: test — `m` is a live recursive mutex; re-entrant lock is the point of the test.
        assert_eq!(unsafe { libc::pthread_mutex_lock(&mut m) }, 0);
        // SAFETY: test — `m` is a live recursive mutex (initialized above).
        assert_eq!(unsafe { libc::pthread_mutex_unlock(&mut m) }, 0);
        // SAFETY: test — `m` is a live recursive mutex (initialized above).
        assert_eq!(unsafe { libc::pthread_mutex_unlock(&mut m) }, 0);
        // NULL handling: `_value` is a no-op (C); `_recursive` returns an
        // error instead of the C reference's NULL deref (documented).
        // SAFETY: null is the documented no-op/error case.
        unsafe { pthread_mutex_init_value(std::ptr::null_mut()) };
        // SAFETY: null is the documented no-op/error case.
        assert_eq!(
            // SAFETY: test — NULL is the documented no-op case of the recursive init.
            unsafe { pthread_mutex_init_recursive(std::ptr::null_mut()) },
            libc::EINVAL
        );
    }

    // S-061 R01: AUTO consumption by timedwait (the coverage table: "AUTO
    // consumption by wait, timedwait and try").
    #[test]
    fn r01_timedwait_consumes_auto() {
        let e = event(OsEventType::Auto);
        os_event_signal(&e);
        assert_eq!(os_event_timedwait(&e, 50), 0);
        assert_eq!(os_event_try(&e), EAGAIN);
    }

    // S-061 R04: counting — each wait decrements, each post increments;
    // at count 0 the wait blocks until a post.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn r04_counting() {
        let s = Arc::new(sem(3));
        assert_eq!(os_sem_post(Some(&s)), 0); // 4
        for _ in 0..4 {
            assert_eq!(os_sem_wait(Some(&s)), 0);
        }
        // count is 0: the next wait must block until the post
        let s1 = Arc::clone(&s);
        let waiter = std::thread::spawn(move || os_sem_wait(Some(&s1)));
        std::thread::sleep(std::time::Duration::from_millis(50));
        assert!(!waiter.is_finished(), "wait must block at count 0");
        assert_eq!(os_sem_post(Some(&s)), 0);
        assert_eq!(waiter.join().expect("waiter"), 0);
        let owned = Arc::try_unwrap(s).expect("single owner");
        os_sem_destroy(Some(owned));
    }

    // vector 05-thread-name (S-061 R05), Windows: `SetThreadDescription`
    // read-back via `GetThreadDescription`. The glibc 15-byte cap does
    // not apply (the C reference does not truncate on Windows).
    #[test]
    #[cfg(windows)]
    #[cfg_attr(miri, ignore)]
    fn v05_thread_name_windows() {
        assert_eq!(worker_name_windows("hello"), Some("hello".to_string()));
        let long = "abcdefghijklmnopqrst"; // 20 chars: no 15-byte cap
        assert_eq!(worker_name_windows(long), Some(long.to_string()));
        // the empty name is accepted (R05: best-effort, never fails)
        os_set_thread_name(b"");
    }

    /// Sets the name on a worker thread and reads it back with
    /// `GetThreadDescription` (mirrors the glibc `worker_name`).
    #[cfg(windows)]
    fn worker_name_windows(name: &str) -> Option<String> {
        let name = name.to_string();
        let (tx, rx) = std::sync::mpsc::channel();
        let thread = std::thread::spawn(move || {
            os_set_thread_name(name.as_bytes());
            let mut desc: *mut u16 = std::ptr::null_mut();
            // SAFETY: `desc` is a live out pointer; `GetCurrentThread()` is
            // the pseudo-handle of the current thread.
            let hr = unsafe {
                crate::winapi::GetThreadDescription(crate::winapi::GetCurrentThread(), &mut desc)
            };
            let got = if hr == 0 || hr == 1 {
                // S_OK / S_FALSE
                if desc.is_null() {
                    None
                } else {
                    // SAFETY: `desc` is a valid NUL-terminated UTF-16 string in
                    // LocalAlloc memory (the `GetThreadDescription` contract);
                    // the scan above found the terminator at `end`.
                    let end = (0..).find(|i| unsafe { *desc.add(*i) } == 0).unwrap();
                    // SAFETY: as above — `desc..desc+end` is valid UTF-16
                    // for `from_utf16_lossy` (it also tolerates lone
                    // surrogates).
                    let s =
                        unsafe { String::from_utf16_lossy(std::slice::from_raw_parts(desc, end)) };
                    // SAFETY: `desc` was allocated by the system for LocalFree.
                    unsafe { crate::winapi::LocalFree(desc as *mut std::os::raw::c_void) };
                    Some(s)
                }
            } else {
                None
            };
            let _ = tx.send(got);
        });
        thread.join().expect("name worker");
        rx.recv().expect("name sent")
    }
}
