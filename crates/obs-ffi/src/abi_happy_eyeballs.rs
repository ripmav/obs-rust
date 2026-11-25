//! P1-33.3 (WP-34): the C ABI for `shared/happy-eyeballs` (S-024 M9).
//!
//! Only `build_addr_list` is bridged: it is file-local (static) in the C
//! reference, so it has no header declaration and is excluded from the
//! cbindgen header diff (the conformance runner declares it in swap mode).
//! The reordering core and the resolver-hint selection are
//! [`obs_netutil::happy_eyeballs`]; the connection race is out of scope
//! (S-024 §1) and stays in C.
//!
//! The context struct is also file-local in the reference; the mirror
//! below keeps its exact glibc layout (the swap-mode runner carries the
//! same C mirror, and the `he-*` vectors pin the round trip through the
//! `addresses` field).

use std::os::raw::{c_char, c_void};
use std::ptr;

use obs_netutil::happy_eyeballs;

// SOCK_STREAM / IPPROTO_TCP / AI_ADDRCONFIG (POSIX).
const SOCK_STREAM: i32 = 1;
const IPPROTO_TCP: i32 = 6;
const AI_ADDRCONFIG: i32 = 0x100;

#[repr(C, align(8))]
struct sockaddr_storage_mirror {
    ss_family: u16,
    _pad: [u8; 126],
}

#[repr(C)]
struct addrinfo_mirror {
    ai_flags: i32,
    ai_family: i32,
    ai_socktype: i32,
    ai_protocol: i32,
    ai_addrlen: u32,
    ai_addr: *mut c_void,
    ai_canonname: *mut c_char,
    ai_next: *mut addrinfo_mirror,
}

/// The file-local `struct happy_eyeballs_ctx` of happy-eyeballs.c (glibc
/// layout — only the fields the port reads/writes and their offsets
/// matter; the `he-*` vectors pin them through `addresses`). The mutex
/// mirrors are sized per glibc architecture: `__SIZEOF_PTHREAD_MUTEX_T`
/// is 40 on x86_64 and 48 on aarch64 (bits/pthreadtypes-arch.h) — the
/// conformance runner compiles the real C struct on its host, so a wrong
/// size shifts `addresses` and the `he-order*` vectors fail.
#[cfg(all(target_os = "linux", target_arch = "x86_64"))]
const GLIBC_MUTEX_MIRROR_LEN: usize = 40;
#[cfg(not(all(target_os = "linux", target_arch = "x86_64")))]
const GLIBC_MUTEX_MIRROR_LEN: usize = 48;

#[repr(C)]
pub struct happy_eyeballs_ctx {
    socket_fd: i32,
    winner_addr: sockaddr_storage_mirror,
    winner_addr_len: u32,
    error: i32,
    error_message: *const c_char,
    bind_addr_len: u32,
    bind_addr: sockaddr_storage_mirror,
    candidates: [u8; 24],
    winner_mutex: [u8; GLIBC_MUTEX_MIRROR_LEN],
    candidate_mutex: [u8; GLIBC_MUTEX_MIRROR_LEN],
    race_completed_event: *mut c_void,
    addresses: *mut addrinfo_mirror,
    name_resolution_time_ns: u64,
    connection_time_start: u64,
    connection_time_end: u64,
    is_starting: bool,
}

unsafe extern "C" {
    fn getaddrinfo(
        node: *const c_char,
        service: *const c_char,
        hints: *const addrinfo_mirror,
        res: *mut *mut addrinfo_mirror,
    ) -> i32;
    fn os_gettime_ns() -> u64;
    fn strerror(err: i32) -> *mut c_char;
    // c/sock-error.c (the C errno macro / WSAGetLastError per platform).
    fn obs_ffi_get_sock_error() -> i32;
}

/// `GetSockError()` of the reference (`WSAGetLastError` on Windows,
/// `errno` elsewhere) — the read lives in c/sock-error.c because the
/// errno accessor symbol differs per ABI/OS version; the C errno macro
/// resolves the right one everywhere.
unsafe fn get_sock_error() -> i32 {
    // SAFETY: the C shim returns the calling thread's last socket error;
    // getaddrinfo was just called.
    unsafe { obs_ffi_get_sock_error() }
}

/// `build_addr_list` of the reference (S-024 §4.9): resolver hints from
/// the bind address, `getaddrinfo` with the decimal port, and the in-place
/// happy-eyeballs reordering of the result list.
///
/// # Safety
/// `hostname` must be a valid NUL-terminated C string (or NULL) and
/// `context` must point to a live context whose layout matches the C
/// reference struct (the swap-mode runner keeps that layout).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn build_addr_list(
    hostname: *const c_char,
    port: i32,
    context: *mut happy_eyeballs_ctx,
) -> i32 {
    // SAFETY: context is live per the function contract.
    let family = happy_eyeballs::hint_family(unsafe { (*context).bind_addr_len as usize });
    // SAFETY: the mirror holds only integer and pointer fields; zero is a
    // valid bit pattern for all of them.
    let mut hints: addrinfo_mirror = unsafe { std::mem::zeroed() };
    hints.ai_family = family;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    hints.ai_flags = AI_ADDRCONFIG;

    let mut port_buf = [0u8; 16];
    let service = format_port(port, &mut port_buf);

    // SAFETY: the calling thread's clock, the same os_gettime_ns the C
    // reference uses (the conformance runner provides it; the production
    // build links libobs/util/threading-posix.c).
    let start = unsafe { os_gettime_ns() };
    let mut res: *mut addrinfo_mirror = ptr::null_mut();
    // SAFETY: hostname is a valid C string per the function contract;
    // `service` is NUL-terminated; `hints` is fully initialised.
    let err = unsafe { getaddrinfo(hostname, service, &hints, &mut res) };
    // SAFETY: same clock as `start`.
    let elapsed = unsafe { os_gettime_ns() } - start;

    if err != 0 {
        // The C sets error = GetSockError() and message = strerror(...)
        // and leaves the address list untouched.
        // SAFETY: getaddrinfo was just called; errno holds its error.
        let e = unsafe { get_sock_error() };
        // SAFETY: context is live per the function contract.
        unsafe {
            (*context).error = e;
            // SAFETY: strerror returns a valid (thread-local) string.
            (*context).error_message = strerror(e) as *const c_char;
            (*context).name_resolution_time_ns = elapsed;
        };
        return -1; // STATUS_FAILURE
    }

    // Collect the resolver result (families + node pointers).
    let mut families: Vec<i32> = Vec::new();
    let mut nodes: Vec<*mut addrinfo_mirror> = Vec::new();
    let mut node = res;
    while !node.is_null() {
        // SAFETY: node is a live resolver result node.
        unsafe {
            families.push((*node).ai_family);
            nodes.push(node);
            node = (*node).ai_next;
        }
    }

    // The reordering is a permutation of the nodes (the head never
    // moves, so `res` stays the list head).
    let order = happy_eyeballs::reorder(&families);
    for (i, &idx) in order.iter().enumerate() {
        let next_node = order.get(i + 1).map_or(ptr::null_mut(), |&j| nodes[j]);
        // SAFETY: nodes[idx] is a live resolver result node; the relink
        // keeps a valid single chain over the same nodes.
        unsafe {
            (*nodes[idx]).ai_next = next_node;
        }
    }

    // SAFETY: context is live per the function contract.
    unsafe {
        (*context).addresses = res;
        (*context).name_resolution_time_ns = elapsed;
    };
    0 // STATUS_SUCCESS
}

/// The decimal port string the C formats with `dstr_printf("%d", port)`
/// (NUL-terminated into `buf`).
fn format_port(port: i32, buf: &mut [u8; 16]) -> *const c_char {
    let mut digits = [0u8; 16];
    let mut value = if port < 0 {
        (port as i64).wrapping_neg() as u64
    } else {
        port as u64
    };
    let mut n = 0;
    loop {
        digits[n] = b'0' + (value % 10) as u8;
        n += 1;
        value /= 10;
        if value == 0 {
            break;
        }
    }
    let mut i = 0;
    if port < 0 {
        buf[i] = b'-';
        i += 1;
    }
    let mut j = n;
    while j > 0 {
        j -= 1;
        buf[i] = digits[j];
        i += 1;
    }
    buf[i] = 0;
    buf.as_ptr() as *const c_char
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn port_decimal() {
        let mut buf = [0u8; 16];
        for port in [0i32, 1, 80, 1935, 65535] {
            let p = format_port(port, &mut buf);
            // SAFETY: p is NUL-terminated within buf.
            let s = unsafe { std::ffi::CStr::from_ptr(p) }.to_str().unwrap();
            assert_eq!(s, port.to_string());
        }
        let p = format_port(i32::MIN, &mut buf);
        assert_eq!(
            // SAFETY: p is NUL-terminated within buf.
            unsafe { std::ffi::CStr::from_ptr(p) }.to_str().unwrap(),
            "-2147483648"
        );
    }

    #[test]
    fn ctx_layout_matches_c_reference() {
        // The offsets of the fields the port touches, against the C
        // reference layout (glibc): the swap-mode runner carries the same
        // C struct, so a drift here is a conformance failure, but pin the
        // offsets directly as well. The mutex size (and therefore
        // `addresses` / the total size) differs per glibc architecture:
        // 40 bytes on x86_64, 48 on aarch64.
        #[cfg(all(target_os = "linux", target_arch = "x86_64"))]
        const ADDRESSES_OFFSET: usize = 400;
        #[cfg(not(all(target_os = "linux", target_arch = "x86_64")))]
        const ADDRESSES_OFFSET: usize = 416;
        assert_eq!(
            std::mem::offset_of!(happy_eyeballs_ctx, winner_addr_len),
            136
        );
        assert_eq!(std::mem::offset_of!(happy_eyeballs_ctx, error), 140);
        assert_eq!(std::mem::offset_of!(happy_eyeballs_ctx, bind_addr_len), 152);
        assert_eq!(std::mem::offset_of!(happy_eyeballs_ctx, bind_addr), 160);
        assert_eq!(
            std::mem::offset_of!(happy_eyeballs_ctx, addresses),
            ADDRESSES_OFFSET
        );
        assert_eq!(
            std::mem::offset_of!(happy_eyeballs_ctx, name_resolution_time_ns),
            ADDRESSES_OFFSET + 8
        );
        assert_eq!(
            std::mem::size_of::<happy_eyeballs_ctx>(),
            ADDRESSES_OFFSET + 40 // two u64 timestamps + bool + tail pad
        );
    }

    // Test stand-ins for the C externs (the swap build links the conformance
    // runner's fakes / libobs for these; `cargo test` links these instead,
    // like the bmalloc/bfree stand-ins of the cf-lexer tests). The
    // getaddrinfo/strerror stand-ins are off-Windows only: the MSVC backend
    // keeps them in the test binary where they would collide with the
    // ws2_32/msvcrt definitions (LNK2005); on Windows the bridge's externs
    // link the real system functions (never called by the Windows tests).
    #[cfg(not(windows))]
    #[unsafe(no_mangle)]
    pub extern "C" fn getaddrinfo(
        _node: *const c_char,
        _service: *const c_char,
        _hints: *const addrinfo_mirror,
        res: *mut *mut addrinfo_mirror,
    ) -> i32 {
        // SAFETY: res is the caller's out pointer (the bridge's local).
        unsafe { *res = ptr::null_mut() }
        -2
    }
    #[unsafe(no_mangle)]
    pub extern "C" fn os_gettime_ns() -> u64 {
        1000
    }
    #[cfg(not(windows))]
    #[unsafe(no_mangle)]
    pub extern "C" fn strerror(_err: i32) -> *mut c_char {
        c"test strerror".as_ptr() as *mut c_char
    }

    /// build_addr_list through the C ABI: a resolver failure sets error /
    /// error_message and returns -1, leaving `addresses` NULL. Unix-only:
    /// the resolver-failure errno is seeded through a guaranteed ENOENT
    /// (the Windows shim reads the Winsock last error, a different value).
    #[cfg(unix)]
    #[test]
    fn resolve_failure_abi() {
        // Seed a known non-zero errno for the calling thread: a lookup of
        // a path that cannot exist fails with ENOENT (2 on every unix ABI
        // and in the Win32 codes), and the C shim's obs_ffi_get_sock_error
        // reads the same errno the reference's GetSockError() would.
        assert!(std::fs::metadata("/definitely/missing/obs-ffi-errno-seed").is_err());
        // SAFETY: the mirror holds only integer and pointer fields; zero
        // mirrors the runner's memset of the C context.
        let mut ctx: happy_eyeballs_ctx = unsafe { std::mem::zeroed() };
        let host = std::ffi::CString::new("h").unwrap();
        // SAFETY: ctx has the C layout (zeroed mirrors the runner's
        // memset); host is a valid C string.
        let r = unsafe { build_addr_list(host.as_ptr(), 443, &mut ctx) };
        assert_eq!(r, -1);
        assert_eq!(ctx.error, 2); // ENOENT
        assert!(ctx.addresses.is_null());
        assert_eq!(ctx.name_resolution_time_ns, 0); // 1000 - 1000
        assert_eq!(
            // SAFETY: the error_message is the stand-in's static buffer.
            unsafe { std::ffi::CStr::from_ptr(ctx.error_message) }
                .to_str()
                .unwrap(),
            "test strerror"
        );
    }
}
