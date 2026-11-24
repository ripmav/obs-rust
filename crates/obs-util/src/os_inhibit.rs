//! S-055 U4/U5 (P1-07.3): D-Bus and xdg-desktop-portal sleep inhibition
//! (nix reference: `platform-nix-dbus.c` / `platform-nix-portal.c` —
//! the Flatpak inhibition path).
//!
//! The GLib/Gio API is resolved **at runtime** from
//! `libgio-2.0.so.0` (`dlopen` + `dlsym`) instead of a build-time link
//! dependency: the swap and `OBS_ENABLE_RUST` builds already link Gio,
//! and a host without it gets `None` from the create functions — the
//! no-GIO C behaviour for these entry points (the files simply are not
//! compiled). All GLib objects are passed around opaquely; the only
//! struct touched is `GError` (to read `->message`).
//!
//! Log lines go through the platform log sink (see `os_fs`): the
//! `obs-ffi` swap bridge routes them through the C `blog`.

use std::ffi::{CStr, CString};
use std::os::raw::{c_char, c_void};
use std::sync::OnceLock;

use crate::os_fs::{LOG_DEBUG, LOG_ERROR, LOG_WARNING, log_platform};

// GLib constants (probed against glib 2.84; the conformance matrix pins
// the host's glib — see the CMake patch's host notes).
const G_BUS_TYPE_SESSION: u32 = 2;
const G_DBUS_CALL_FLAGS_NONE: u32 = 0;
const G_DBUS_CALL_FLAGS_NO_AUTO_START: u32 = 1;
const G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE: u32 = 1;
/// `G_IO_ERROR_CANCELLED` (glib 2.84's `gioenums.h`; probed).
const G_IO_ERROR_CANCELLED: i32 = 19;

const PORTAL_NAME: &str = "org.freedesktop.portal.Desktop";
const PORTAL_PATH: &str = "/org/freedesktop/portal/desktop";
const INHIBIT_PORTAL_IFACE: &str = "org.freedesktop.portal.Inhibit";

/// U4 probe table (fixed order, index = service type; R23).
struct Service {
    name: &'static str,
    path: &'static str,
    interface: &'static str,
    uninhibit: &'static str,
}

const SERVICES: [Service; 4] = [
    Service {
        name: "org.freedesktop.ScreenSaver",
        path: "/ScreenSaver",
        interface: "org.freedesktop.ScreenSaver",
        uninhibit: "UnInhibit",
    },
    Service {
        name: "org.freedesktop.PowerManagement.Inhibit",
        path: "/org/freedesktop/PowerManagement",
        interface: "org.freedesktop.PowerManagement.Inhibit",
        uninhibit: "UnInhibit",
    },
    Service {
        name: "org.mate.SessionManager",
        path: "/org/mate/SessionManager",
        interface: "org.mate.SessionManager",
        uninhibit: "Uninhibit",
    },
    Service {
        name: "org.gnome.SessionManager",
        path: "/org/gnome/SessionManager",
        interface: "org.gnome.SessionManager",
        uninhibit: "Uninhibit",
    },
];

/// C `GError` (only `message` is read; `g_error_free` frees it).
#[repr(C)]
struct GError {
    domain: u32,
    code: i32,
    message: *mut c_char,
}

fn err_message(err: *mut GError) -> String {
    // SAFETY: `err` is a live `GError` (checked by the caller); `message` is a NUL-terminated string owned by the error.
    let msg = unsafe { (*err).message };
    if msg.is_null() {
        return String::new();
    }
    // SAFETY: see above.
    let s = unsafe { CStr::from_ptr(msg) };
    s.to_string_lossy().into_owned()
}

// ------------------------------------------------------------------ loader

type FBusGetSync = unsafe extern "C" fn(u32, *mut c_void, *mut *mut GError) -> *mut c_void;
type FCallSync = unsafe extern "C" fn(
    *mut c_void,
    *const c_char,
    *const c_char,
    *const c_char,
    *const c_char,
    *mut c_void,
    *const c_void,
    u32,
    i32,
    *mut c_void,
    *mut *mut GError,
) -> *mut c_void;
type FCall = unsafe extern "C" fn(
    *mut c_void,
    *const c_char,
    *const c_char,
    *const c_char,
    *const c_char,
    *mut c_void,
    *const c_void,
    u32,
    i32,
    *mut c_void,
    FAsyncReady,
    *mut c_void,
);
type FCallFinish = unsafe extern "C" fn(*mut c_void, *mut c_void, *mut *mut GError) -> *mut c_void;
type FAsyncReady = unsafe extern "C" fn(*mut c_void, *mut c_void, *mut c_void);
type FSignalSubscribe = unsafe extern "C" fn(
    *mut c_void,
    *const c_char,
    *const c_char,
    *const c_char,
    *const c_char,
    *const c_char,
    u32,
    FSignalCb,
    *mut c_void,
    *mut c_void,
) -> u32;
type FSignalCb = unsafe extern "C" fn(
    *mut c_void,
    *const c_char,
    *const c_char,
    *const c_char,
    *const c_char,
    *mut c_void,
    *mut c_void,
);
type FSignalUnsubscribe = unsafe extern "C" fn(*mut c_void, u32);
type FUniqueName = unsafe extern "C" fn(*mut c_void) -> *const c_char;
type FVariantNew = unsafe extern "C" fn(*const c_char, ...) -> *mut c_void;
type FVariantGet = unsafe extern "C" fn(*mut c_void, *const c_char, ...);
type FVariantNewString = unsafe extern "C" fn(*const c_char) -> *mut c_void;
type FVariantUnref = unsafe extern "C" fn(*mut c_void);
type FVariantBuilderInit = unsafe extern "C" fn(*mut c_void, *const c_void);
type FVariantBuilderAdd = unsafe extern "C" fn(*mut c_void, *const c_char, ...);
type FVariantTypeNew = unsafe extern "C" fn(*const c_char) -> *mut c_void;
type FVariantTypeFree = unsafe extern "C" fn(*mut c_void);
type FCancellableNew = unsafe extern "C" fn() -> *mut c_void;
type FCancellableCancel = unsafe extern "C" fn(*mut c_void);
type FObjectUnref = unsafe extern "C" fn(*mut c_void);
type FErrorFree = unsafe extern "C" fn(*mut GError);
type FErrorMatches = unsafe extern "C" fn(*mut GError, u32, i32) -> i32;
type FIoErrorQuark = unsafe extern "C" fn() -> u32;

struct Gio {
    /// The `dlopen` handle, kept for the process lifetime (never
    /// `dlclose`d — the C reference keeps its Gio symbols linked the
    /// same way).
    #[allow(dead_code)]
    handle: *mut c_void,
    bus_get_sync: FBusGetSync,
    call_sync: FCallSync,
    call: FCall,
    call_finish: FCallFinish,
    signal_subscribe: FSignalSubscribe,
    signal_unsubscribe: FSignalUnsubscribe,
    unique_name: FUniqueName,
    variant_new: FVariantNew,
    variant_get: FVariantGet,
    variant_new_string: FVariantNewString,
    variant_unref: FVariantUnref,
    variant_builder_init: FVariantBuilderInit,
    variant_builder_add: FVariantBuilderAdd,
    variant_type_new: FVariantTypeNew,
    variant_type_free: FVariantTypeFree,
    cancellable_new: FCancellableNew,
    cancellable_cancel: FCancellableCancel,
    object_unref: FObjectUnref,
    error_free: FErrorFree,
    error_matches: FErrorMatches,
    io_error_quark: FIoErrorQuark,
}

// SAFETY: `Gio` holds a `dlopen` handle and C function pointers — plain
// data valid from any thread (`dlopen`/`dlsym` are thread-safe in glibc).
unsafe impl Send for Gio {}
// SAFETY: same as `Send` above — the handle and resolved pointers are valid from any thread.
unsafe impl Sync for Gio {}

static GIO: OnceLock<Option<Gio>> = OnceLock::new();

/// Resolve one symbol from the Gio handle. All targets are plain C
/// functions, so the word-sized pointer copy is the established
/// `dlsym` pattern.
fn sym<T: Copy>(handle: *mut c_void, name: &str) -> Option<T> {
    // SAFETY: `handle` is a live `dlopen` handle; the name is NUL-terminated.
    let p = unsafe { libc::dlsym(handle, c_str(name).as_ptr()) };
    if p.is_null() {
        None
    } else {
        // SAFETY: `p` is a word-sized pointer; `T` is a function pointer of the same size on all supported ABIs.
        Some(unsafe { std::mem::transmute_copy::<*mut c_void, T>(&p) })
    }
}

/// Load `libgio-2.0.so.0` and resolve the API (the dependency closure
/// brings in glib/gobject, so `dlsym` on the Gio handle finds all of
/// them). `None` when the library is absent — the create functions then
/// report no inhibitor (the no-GIO C behaviour).
fn gio() -> Option<&'static Gio> {
    GIO.get_or_init(|| {
        // SAFETY: a valid NUL-terminated library name; `RTLD_NOW | RTLD_GLOBAL` resolves the dependency closure.
        let handle = unsafe {
            libc::dlopen(
                c"libgio-2.0.so.0".as_ptr(),
                libc::RTLD_NOW | libc::RTLD_GLOBAL,
            )
        };
        if handle.is_null() {
            return None;
        }
        Some(Gio {
            handle,
            bus_get_sync: sym(handle, "g_bus_get_sync")?,
            call_sync: sym(handle, "g_dbus_connection_call_sync")?,
            call: sym(handle, "g_dbus_connection_call")?,
            call_finish: sym(handle, "g_dbus_connection_call_finish")?,
            signal_subscribe: sym(handle, "g_dbus_connection_signal_subscribe")?,
            signal_unsubscribe: sym(handle, "g_dbus_connection_signal_unsubscribe")?,
            unique_name: sym(handle, "g_dbus_connection_get_unique_name")?,
            variant_new: sym(handle, "g_variant_new")?,
            variant_get: sym(handle, "g_variant_get")?,
            variant_new_string: sym(handle, "g_variant_new_string")?,
            variant_unref: sym(handle, "g_variant_unref")?,
            variant_builder_init: sym(handle, "g_variant_builder_init")?,
            variant_builder_add: sym(handle, "g_variant_builder_add")?,
            variant_type_new: sym(handle, "g_variant_type_new")?,
            variant_type_free: sym(handle, "g_variant_type_free")?,
            cancellable_new: sym(handle, "g_cancellable_new")?,
            cancellable_cancel: sym(handle, "g_cancellable_cancel")?,
            object_unref: sym(handle, "g_object_unref")?,
            error_free: sym(handle, "g_error_free")?,
            error_matches: sym(handle, "g_error_matches")?,
            io_error_quark: sym(handle, "g_io_error_quark")?,
        })
    })
    .as_ref()
}

fn c_str(s: &str) -> CString {
    CString::new(s.as_bytes()).unwrap()
}

/// Monotonic-ish 31-bit sequence for the portal request token (see the
/// comment in `do_inhibit`): glibc-style `rand()` output is masked in
/// the conformance output, so only the digit count contract matters.
fn request_id() -> i32 {
    use std::sync::atomic::{AtomicU64, Ordering::Relaxed};
    static SEED: AtomicU64 = AtomicU64::new(0x853c_49e6_748f_ea9b);
    let mut x = SEED.fetch_add(0x9e37_79b9_7f4a_7c15, Relaxed);
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    SEED.store(x, Relaxed);
    (x & 0x7fff_ffff) as i32
}

// ------------------------------------------------------------------ U4 dbus

/// U4 (R23): a live D-Bus session-bus connection to the first service
/// of the probe table that has an owner (debug-logged); `None` when the
/// bus cannot be created (error-logged) or no service answers.
pub struct DbusSleepInfo {
    service: &'static Service,
    conn: *mut c_void,
    cookie: u32,
    service_type: u32,
}

/// U4 (R23) `dbus_sleep_info_create()`: session bus, then
/// `GetNameOwner` probes in the fixed table order (no auto-start). The
/// first answer wins (debug log); none -> `None` (the connection is
/// released).
pub fn dbus_sleep_info_create() -> Option<DbusSleepInfo> {
    let g = gio()?;
    let mut err: *mut GError = std::ptr::null_mut();
    // SAFETY: loader-verified pointer; NULL cancellable; `err` is valid out storage.
    let conn = unsafe { (g.bus_get_sync)(G_BUS_TYPE_SESSION, std::ptr::null_mut(), &mut err) };
    if conn.is_null() {
        log_platform(
            LOG_ERROR,
            &format!("Could not create dbus connection: {}", err_message(err)),
        );
        // SAFETY: `err` is a live `GError` returned by `g_bus_get_sync`.
        unsafe { (g.error_free)(err) };
        return None;
    }
    for (i, service) in SERVICES.iter().enumerate() {
        let name = c_str(service.name);
        // SAFETY: variadic `g_variant_new("(s)")` with one `*const c_char`.
        let params = unsafe { (g.variant_new)(c"(s)".as_ptr(), name.as_ptr()) };
        // SAFETY: loader-verified pointer; valid C strings; NULL expected reply type / cancellable / out error.
        let reply = unsafe {
            (g.call_sync)(
                conn,
                c_str("org.freedesktop.DBus").as_ptr(),
                c_str("/org/freedesktop/DBus").as_ptr(),
                c_str("org.freedesktop.DBus").as_ptr(),
                c_str("GetNameOwner").as_ptr(),
                params,
                std::ptr::null(),
                G_DBUS_CALL_FLAGS_NO_AUTO_START,
                -1,
                std::ptr::null_mut(),
                std::ptr::null_mut(),
            )
        };
        let found = !reply.is_null();
        if !reply.is_null() {
            // SAFETY: `reply` is a live `GVariant` (non-NULL — the guard above); `g_variant_unref` asserts on NULL.
            unsafe { (g.variant_unref)(reply) };
        }
        if found {
            log_platform(LOG_DEBUG, &format!("Found dbus service: {}", service.name));
            return Some(DbusSleepInfo {
                service,
                conn,
                cookie: 0,
                service_type: i as u32,
            });
        }
    }
    // the failed-create destroy (R23)
    // SAFETY: `conn` is a live `GObject` (the connection) created above.
    unsafe { (g.object_unref)(conn) };
    None
}

/// U4 (R24/R25) `dbus_inhibit_sleep(info, reason, active)`: no-op when
/// the state already matches; Inhibit sends `("libobs", reason)`
/// (types 0/1) or `("libobs", u32 0, reason, u32 0xC)` (types 2/3) and
/// stores the reply cookie; UnInhibit sends the cookie to the service's
/// uninhibit method and clears it. A call error logs
/// `Failed to call <method>: <message>` and leaves the state unchanged.
pub fn dbus_inhibit_sleep(info: &mut DbusSleepInfo, reason: &CStr, active: bool) {
    let Some(g) = gio() else {
        return;
    };
    if (info.cookie != 0) == active {
        return;
    }
    let method = if active {
        "Inhibit"
    } else {
        info.service.uninhibit
    };
    // SAFETY: loader-verified pointers; valid C strings; variadic forms per the formats.
    let params = unsafe {
        if active {
            match info.service_type {
                // MATE/GNOME session managers: (s@usu) — program, the
                // xid as a nested variant, reason, flags
                2 | 3 => {
                    let xid = (g.variant_new)(c"u".as_ptr(), 0u32);
                    (g.variant_new)(
                        c"(s@usu)".as_ptr(),
                        c_str("libobs").as_ptr(),
                        xid,
                        reason.as_ptr(),
                        0xCu32,
                    )
                }
                // freedesktop screensaver / power management: (ss)
                _ => (g.variant_new)(c"(ss)".as_ptr(), c_str("libobs").as_ptr(), reason.as_ptr()),
            }
        } else {
            (g.variant_new)(c"(u)".as_ptr(), info.cookie)
        }
    };
    let mut err: *mut GError = std::ptr::null_mut();
    // SAFETY: loader-verified pointer; valid C strings; NULL expected reply type / cancellable.
    let reply = unsafe {
        (g.call_sync)(
            info.conn,
            c_str(info.service.name).as_ptr(),
            c_str(info.service.path).as_ptr(),
            c_str(info.service.interface).as_ptr(),
            c_str(method).as_ptr(),
            params,
            std::ptr::null(),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            std::ptr::null_mut(),
            &mut err,
        )
    };
    if !err.is_null() {
        log_platform(
            LOG_ERROR,
            &format!("Failed to call {method}: {}", err_message(err)),
        );
        if !reply.is_null() {
            // SAFETY: `reply` is a live `GVariant` (non-NULL — the guard above); `g_variant_unref` asserts on NULL.
            unsafe { (g.variant_unref)(reply) };
        }
        // SAFETY: `err` is a live `GError` returned by the call.
        unsafe { (g.error_free)(err) };
        return; // R25: the state stays unchanged
    }
    if active {
        let mut cookie: u32 = 0;
        // SAFETY: `reply` is a live `(u)` GVariant (non-NULL: the error path returned above); `&mut cookie` is the out argument for the `u` format.
        unsafe { (g.variant_get)(reply, c"(u)".as_ptr(), &mut cookie) };
        info.cookie = cookie;
    } else {
        info.cookie = 0;
    }
    // SAFETY: `reply` is a live `GVariant` returned by the call.
    unsafe { (g.variant_unref)(reply) };
}

/// U4 (R25) `dbus_sleep_info_destroy(info)`: drops the connection. It
/// does NOT uninhibit (Q-4). NULL is a no-op (the bridge guards).
pub fn dbus_sleep_info_destroy(info: DbusSleepInfo) {
    if let Some(g) = gio() {
        // SAFETY: `info.conn` is a live `GObject` created by `dbus_sleep_info_create`.
        unsafe { (g.object_unref)(info.conn) };
    }
}

// ------------------------------------------------------------------ U5 portal

/// U5 (R26): a live portal Inhibit client. The GLib async callbacks
/// (`inhibited_cb`, `response_received`) run on the connection's main
/// context — the same dispatch the C reference relies on (the caller
/// must pump the loop, as the conformance runner does).
pub struct PortalInhibitInfo {
    conn: *mut c_void,
    cancellable: *mut c_void,
    signal_id: u32,
    sender_name: String,
    request_path: Option<String>,
    active: bool,
}

impl PortalInhibitInfo {
    fn unsubscribe(&mut self) {
        if self.signal_id > 0 {
            if let Some(g) = gio() {
                // SAFETY: loader-verified pointer; `conn` is live; `signal_id` was returned by `signal_subscribe`.
                unsafe { (g.signal_unsubscribe)(self.conn, self.signal_id) };
            }
            self.signal_id = 0;
        }
    }

    fn remove_inhibit_data(&mut self) {
        self.request_path = None;
        self.active = false;
    }
}

/// GLib `GAsyncReadyCallback` for the async Inhibit call (R28).
extern "C" fn inhibited_cb(_source: *mut c_void, result: *mut c_void, data: *mut c_void) {
    let Some(g) = gio() else {
        return;
    };
    // SAFETY: `data` is a live `PortalInhibitInfo` (registered with the call, alive until destroy).
    let info = unsafe { &mut *(data as *mut PortalInhibitInfo) };
    let mut err: *mut GError = std::ptr::null_mut();
    // SAFETY: loader-verified pointer; `conn`/`result` are live.
    let reply = unsafe { (g.call_finish)(info.conn, result, &mut err) };
    if !err.is_null() {
        if !reply.is_null() {
            // SAFETY: `reply` is a live `GVariant` (non-NULL — the guard above); `g_variant_unref` asserts on NULL.
            unsafe { (g.variant_unref)(reply) };
        }
        // SAFETY: `cancelled` matches the C `g_error_matches` check; `err` is live for the call.
        let cancelled =
            unsafe { (g.error_matches)(err, (g.io_error_quark)(), G_IO_ERROR_CANCELLED) } != 0;
        if !cancelled {
            log_platform(
                LOG_ERROR,
                &format!("Failed to inhibit: {}", err_message(err)),
            );
        }
        info.unsubscribe();
        info.remove_inhibit_data();
        // SAFETY: `err` is a live `GError` returned by `call_finish`.
        unsafe { (g.error_free)(err) };
    } else {
        // SAFETY: `reply` is a live `GVariant` returned by `call_finish` (non-NULL: the error path handled NULL).
        unsafe { (g.variant_unref)(reply) };
    }
    // `g_clear_object(&info->cancellable)` — the call is finished either way,
    // so a later `do_uninhibit` uses the `Close` branch, not a cancel.
    if !info.cancellable.is_null() {
        // SAFETY: `cancellable` is a live `GObject` created by `do_inhibit`.
        unsafe { (g.object_unref)(info.cancellable) };
        info.cancellable = std::ptr::null_mut();
    }
}

/// GLib `GDBusSignalCallback` for the portal `Request.Response` signal
/// (R29).
extern "C" fn response_received(
    _bus: *mut c_void,
    _sender: *const c_char,
    _path: *const c_char,
    _iface: *const c_char,
    _signal: *const c_char,
    parameters: *mut c_void,
    data: *mut c_void,
) {
    let Some(g) = gio() else {
        return;
    };
    // SAFETY: `data` is a live `PortalInhibitInfo` (subscription user data, alive until destroy).
    let info = unsafe { &mut *(data as *mut PortalInhibitInfo) };
    let mut response: u32 = 0;
    let mut dict: *mut c_void = std::ptr::null_mut();
    // SAFETY: `parameters` is the live `(u@a{sv})` signal body; the out arguments are valid storage.
    unsafe { (g.variant_get)(parameters, c"(u@a{sv})".as_ptr(), &mut response, &mut dict) };
    // SAFETY: `dict` is a live `GVariant` (or NULL) moved out of the signal body.
    unsafe { (g.variant_unref)(dict) };
    if response != 0 {
        if response == 1 {
            log_platform(LOG_WARNING, "Inhibit denied by user");
        }
        info.remove_inhibit_data();
    }
    info.unsubscribe();
}

/// U5 (R26) `portal_inhibit_info_create()`: session bus; the sender
/// name is the unique bus name without the leading `:` with `.` -> `_`;
/// the portal is verified with `Properties.Get("Inhibit", "version")`
/// (debug-logged); failure -> `None` (the handle is destroyed).
pub fn portal_inhibit_info_create() -> Option<PortalInhibitInfo> {
    let g = gio()?;
    let mut err: *mut GError = std::ptr::null_mut();
    // SAFETY: loader-verified pointer; NULL cancellable; valid out storage.
    let conn = unsafe { (g.bus_get_sync)(G_BUS_TYPE_SESSION, std::ptr::null_mut(), &mut err) };
    if conn.is_null() {
        log_platform(
            LOG_ERROR,
            &format!("Could not create dbus connection: {}", err_message(err)),
        );
        // SAFETY: `err` is a live `GError` returned by `g_bus_get_sync`.
        unsafe { (g.error_free)(err) };
        return None;
    }
    // SAFETY: `conn` is a live connection.
    let unique = unsafe { (g.unique_name)(conn) };
    // SAFETY: the unique name is a live NUL-terminated string (`:1.N`).
    let sender_bytes = unsafe { CStr::from_ptr(unique) }.to_bytes();
    let mut sender_name: Vec<u8> = sender_bytes[1..].to_vec();
    for ch in sender_name.iter_mut() {
        if *ch == b'.' {
            *ch = b'_';
        }
    }
    let sender_name = String::from_utf8_lossy(&sender_name).into_owned();
    let iface = c_str(INHIBIT_PORTAL_IFACE);
    // SAFETY: variadic `g_variant_new("(ss)")` with two `*const c_char`.
    let params =
        unsafe { (g.variant_new)(c"(ss)".as_ptr(), iface.as_ptr(), c_str("version").as_ptr()) };
    // `G_VARIANT_TYPE("(v)")`
    // SAFETY: loader-verified pointer; the format string is a valid variant type string.
    let expected = unsafe { (g.variant_type_new)(c"(v)".as_ptr()) };
    // SAFETY: loader-verified pointer; valid C strings; live type pointer for the call's duration.
    let reply = unsafe {
        (g.call_sync)(
            conn,
            c_str(PORTAL_NAME).as_ptr(),
            c_str(PORTAL_PATH).as_ptr(),
            c_str("org.freedesktop.DBus.Properties").as_ptr(),
            c_str("Get").as_ptr(),
            params,
            expected,
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            std::ptr::null_mut(),
            std::ptr::null_mut(),
        )
    };
    // SAFETY: `expected` was created by `variant_type_new` above.
    unsafe { (g.variant_type_free)(expected) };
    if reply.is_null() {
        // the failed-create destroy (R26)
        // SAFETY: `conn` is a live `GObject` created above.
        unsafe { (g.object_unref)(conn) };
        return None;
    }
    // SAFETY: `reply` is a live `GVariant` returned by the call.
    unsafe { (g.variant_unref)(reply) };
    log_platform(LOG_DEBUG, "Found portal inhibitor");
    Some(PortalInhibitInfo {
        conn,
        cancellable: std::ptr::null_mut(),
        signal_id: 0,
        sender_name,
        request_path: None,
        active: false,
    })
}

/// U5 (R27) inhibit half: `active = true`; a new request
/// (`"obs_inhibit_portal" + rand()`), a `Response` subscription (no
/// match rule) on the request path, then the async
/// `Inhibit("", u32 0xC, {handle_token, reason})` call with a
/// cancellable.
fn do_inhibit(info: &mut PortalInhibitInfo, reason: &CStr) {
    let Some(g) = gio() else {
        return;
    };
    info.active = true;
    // the request token (masked in the conformance output). The C
    // reference uses glibc's `rand()`; the token is masked in the
    // output, so a local generator keeps the contract without a libc
    // dependency (this libc version no longer exposes `rand` on unix).
    let id = request_id();
    let token = format!("obs_inhibit_portal{id}");
    let request_path = format!(
        "/org/freedesktop/portal/desktop/request/{}/{}",
        info.sender_name, token
    );
    let token_c = c_str(&token);
    let path_c = c_str(&request_path);
    // C sets `info->request_path` before the call (the response signal
    // may find the handle mid-call)
    info.request_path = Some(request_path.clone());
    // SAFETY: `info` is a live handle for the subscription's lifetime (user data).
    let user_data = info as *mut PortalInhibitInfo as *mut c_void;
    // SAFETY: loader-verified pointer; valid C strings; NULL match rule; the callback is the registered C function.
    info.signal_id = unsafe {
        (g.signal_subscribe)(
            info.conn,
            c_str(PORTAL_NAME).as_ptr(),
            c_str("org.freedesktop.portal.Request").as_ptr(),
            c_str("Response").as_ptr(),
            path_c.as_ptr(),
            std::ptr::null(),
            G_DBUS_SIGNAL_FLAGS_NO_MATCH_RULE,
            response_received,
            user_data,
            std::ptr::null_mut(),
        )
    };
    // The options vardict `{handle_token, reason}`.
    // SAFETY: loader-verified pointer; `a{sv}` is a valid variant type string.
    let dict_type = unsafe { (g.variant_type_new)(c"a{sv}".as_ptr()) };
    // `GVariantBuilder` is 128 bytes in GLib 2.80 — the buffer is sized to it.
    let mut builder = [0u8; 128];
    // SAFETY: `builder` is 128 bytes of valid storage for the `GVariantBuilder` (128 bytes in GLib 2.80); `dict_type` is a live type.
    unsafe { (g.variant_builder_init)(builder.as_mut_ptr().cast::<c_void>(), dict_type) };
    // SAFETY: variadic `builder_add("{sv}")` with a key string and a boxed string variant (the floating reference is absorbed).
    unsafe {
        (g.variant_builder_add)(
            builder.as_mut_ptr().cast::<c_void>(),
            c"{sv}".as_ptr(),
            c_str("handle_token").as_ptr(),
            (g.variant_new_string)(token_c.as_ptr()),
        );
        (g.variant_builder_add)(
            builder.as_mut_ptr().cast::<c_void>(),
            c"{sv}".as_ptr(),
            c_str("reason").as_ptr(),
            (g.variant_new_string)(reason.as_ptr()),
        );
    }
    // SAFETY: variadic `g_variant_new("(sua{sv})")` — empty string, u32, the builder address (the C form passes `&options`).
    let params = unsafe {
        (g.variant_new)(
            c"(sua{sv})".as_ptr(),
            c"".as_ptr(),
            0xCu32,
            builder.as_mut_ptr().cast::<c_void>(),
        )
    };
    // SAFETY: `dict_type` was created by `variant_type_new` above (the builder only needed it while building).
    unsafe { (g.variant_type_free)(dict_type) };
    // SAFETY: `cancellable_new` returns a live `GCancellable` (or NULL, treated as no cancellable — the C form does the same).
    info.cancellable = unsafe { (g.cancellable_new)() };
    // SAFETY: loader-verified pointer; valid C strings; NULL expected type; the callback + user data are live for the call.
    unsafe {
        (g.call)(
            info.conn,
            c_str(PORTAL_NAME).as_ptr(),
            c_str(PORTAL_PATH).as_ptr(),
            c_str(INHIBIT_PORTAL_IFACE).as_ptr(),
            c_str("Inhibit").as_ptr(),
            params,
            std::ptr::null(),
            G_DBUS_CALL_FLAGS_NONE,
            -1,
            info.cancellable,
            inhibited_cb,
            user_data,
        )
    };
}

/// U5 (R30) uninhibit half: a still-pending Inhibit call is cancelled
/// instead (nothing else is sent); otherwise the async
/// `Request.Close()` on the request path (error reply -> warning).
/// Then the request is forgotten and `active = false`.
fn do_uninhibit(info: &mut PortalInhibitInfo) {
    let Some(g) = gio() else {
        return;
    };
    if !info.cancellable.is_null() {
        // SAFETY: `cancellable` is a live `GCancellable` created by `do_inhibit`.
        unsafe {
            (g.cancellable_cancel)(info.cancellable);
            (g.object_unref)(info.cancellable);
        };
        info.cancellable = std::ptr::null_mut();
    } else {
        // SAFETY: `active` (checked by the caller) implies `do_inhibit` stored a request path.
        let request_path = c_str(info.request_path.as_ref().unwrap());
        // SAFETY: variadic `g_variant_new("()")` with no arguments.
        let params = unsafe { (g.variant_new)(c"()".as_ptr()) };
        // `G_VARIANT_TYPE_UNIT` = `"()"`
        // SAFETY: loader-verified pointer; the format string is a valid variant type string.
        let expected = unsafe { (g.variant_type_new)(c"()".as_ptr()) };
        // SAFETY: `info` is a live handle for the call's lifetime.
        let user_data = info as *mut PortalInhibitInfo as *mut c_void;
        // SAFETY: loader-verified pointer; valid C strings; live type pointer for the call.
        unsafe {
            (g.call)(
                info.conn,
                c_str(PORTAL_NAME).as_ptr(),
                request_path.as_ptr(),
                c_str("org.freedesktop.portal.Request").as_ptr(),
                c_str("Close").as_ptr(),
                params,
                expected,
                G_DBUS_CALL_FLAGS_NONE,
                -1,
                std::ptr::null_mut(),
                uninhibited_cb,
                user_data,
            )
        };
        // SAFETY: `expected` was created by `variant_type_new` above.
        unsafe { (g.variant_type_free)(expected) };
    }
    info.remove_inhibit_data();
}

/// GLib `GAsyncReadyCallback` for the async `Close` call (R30).
extern "C" fn uninhibited_cb(_source: *mut c_void, result: *mut c_void, data: *mut c_void) {
    let Some(g) = gio() else {
        return;
    };
    let mut err: *mut GError = std::ptr::null_mut();
    // SAFETY: `data` is a live `PortalInhibitInfo` (registered with the call, alive until destroy); `conn` is live for the call.
    let info = unsafe { &*(data as *const PortalInhibitInfo) };
    // SAFETY: loader-verified pointer; `info.conn`/`result` are live.
    let reply = unsafe { (g.call_finish)(info.conn, result, &mut err) };
    if !err.is_null() {
        log_platform(
            LOG_WARNING,
            &format!("Error uninhibiting: {}", err_message(err)),
        );
        // SAFETY: `err` is a live `GError` returned by `call_finish`.
        unsafe { (g.error_free)(err) };
    }
    if !reply.is_null() {
        // SAFETY: `reply` is a live `GVariant` (non-NULL — the guard above); `g_variant_unref` asserts on NULL.
        unsafe { (g.variant_unref)(reply) };
    }
}

/// U5 (R27/R30) `portal_inhibit(info, reason, active)`: no-op when the
/// state already matches; otherwise the inhibit/uninhibit halves above.
pub fn portal_inhibit(info: &mut PortalInhibitInfo, reason: &CStr, active: bool) {
    if info.active == active {
        return;
    }
    if active {
        do_inhibit(info, reason);
    } else {
        do_uninhibit(info);
    }
}

/// U5 (R31) `portal_inhibit_info_destroy(info)`: cancel a pending call,
/// unsubscribe, forget the path, release the connection. It does NOT
/// send `Close`. NULL is a no-op (the bridge guards).
pub fn portal_inhibit_info_destroy(mut info: PortalInhibitInfo) {
    let Some(g) = gio() else {
        return;
    };
    if !info.cancellable.is_null() {
        // SAFETY: live `GCancellable` created by `do_inhibit`.
        unsafe {
            (g.cancellable_cancel)(info.cancellable);
            (g.object_unref)(info.cancellable);
        };
    }
    info.unsubscribe();
    // `sender_name`/`request_path` are Rust-owned (dropped here); the C
    // form bfree'd them.
    // SAFETY: `conn` is a live `GObject` created by `portal_inhibit_info_create`.
    unsafe { (g.object_unref)(info.conn) };
}

#[cfg(test)]
mod tests {
    use super::*;

    /// Point the session bus at a path that does not exist: with Gio
    /// present, `g_bus_get_sync` fails; without Gio, `gio()` is None.
    /// Either way the create functions must give up cleanly (R23/R26
    /// failure paths — the `dbus_nobus` vector's contract).
    fn unreachable_bus() {
        // SAFETY: test-only: no other thread reads `DBUS_SESSION_BUS_ADDRESS` (only these tests' GLib calls do).
        unsafe { std::env::set_var("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent-wp30") };
    }

    #[test]
    #[cfg_attr(miri, ignore)] // dlopen / env (Miri isolation)
    fn dbus_create_no_bus() {
        let _lock = crate::os_fs::PLATFORM_SINK_TEST_LOCK.lock().unwrap();
        unreachable_bus();
        assert!(dbus_sleep_info_create().is_none());
    }

    #[test]
    #[cfg_attr(miri, ignore)] // dlopen / env (Miri isolation)
    fn portal_create_no_bus() {
        let _lock = crate::os_fs::PLATFORM_SINK_TEST_LOCK.lock().unwrap();
        unreachable_bus();
        assert!(portal_inhibit_info_create().is_none());
    }

    /// The error line keeps the C prefix (the conformance runner cuts
    /// the GLib-specific tail at the first `": "`).
    #[test]
    #[cfg_attr(miri, ignore)] // dlopen / env (Miri isolation)
    fn dbus_create_error_line() {
        let _lock = crate::os_fs::PLATFORM_SINK_TEST_LOCK.lock().unwrap();
        unreachable_bus();
        let (tx, rx) = std::sync::mpsc::channel();
        crate::os_fs::set_platform_log_sink(move |level, line| {
            let _ = tx.send((level, line.to_string()));
        });
        assert!(dbus_sleep_info_create().is_none());
        if gio().is_some() {
            let (level, line) = rx
                .recv_timeout(std::time::Duration::from_secs(5))
                .expect("the create error line");
            assert_eq!(level, LOG_ERROR);
            assert!(
                line.starts_with("Could not create dbus connection: "),
                "{line}"
            );
        }
    }
}
