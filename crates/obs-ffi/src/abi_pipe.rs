//! P1-11.4 recipe step 2: the C ABI for the process pipes
//! (`libobs/util/pipe-posix.c`, S-055 U2).
//!
//! `os_process_pipe_t` is the Rust `ProcessPipe` (opaque in C); the
//! handle is a leaked `Box` the C side returns from
//! `os_process_pipe_create`/`create2` and hands back to
//! `os_process_pipe_destroy`.
//!
//! The `os_process_args_*` functions stay C (`libobs/util/pipe.c` is
//! **not** swapped): `os_process_args_add_argf` is variadic, which Rust
//! cannot define. `create2` reads the C `struct os_process_args`
//! (a single `DARRAY(char *)` field, the layout of
//! `pipe.c:23-25`) and copies it into the Rust `ProcessArgs`.

use std::os::raw::{c_char, c_int};

use obs_util::pipe::{
    ProcessPipe, os_process_pipe_create as cd_pipe_create,
    os_process_pipe_create2 as cd_pipe_create2, os_process_pipe_destroy as cd_pipe_destroy,
    os_process_pipe_read as cd_pipe_read, os_process_pipe_read_err as cd_pipe_read_err,
    os_process_pipe_write as cd_pipe_write,
};
use obs_util::process_args::ProcessArgs;

/// The C `struct os_process_args` (pipe.c:23-25): one darray of
/// NUL-terminated strings with a trailing NULL slot.
#[repr(C)]
pub struct CProcessArgs {
    array: *mut *mut c_char,
    num: usize,
    capacity: usize,
}

/// `EXPORT os_process_pipe_t *os_process_pipe_create(const char
/// *cmd_line, const char *type)`
///
/// # Safety
/// `cmd_line`/`type` must be NUL-terminated (NULL → NULL, the C
/// reference's guard).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_process_pipe_create(
    cmd_line: *const c_char,
    type_: *const c_char,
) -> *mut ProcessPipe {
    let Some(cmd) = crate::abi_common::c_bytes(cmd_line) else {
        return std::ptr::null_mut();
    };
    let Some(type_) = crate::abi_common::c_bytes(type_) else {
        return std::ptr::null_mut();
    };
    match cd_pipe_create(
        String::from_utf8_lossy(cmd).as_ref(),
        String::from_utf8_lossy(type_).as_ref(),
    ) {
        Some(pp) => {
            // SAFETY: Box is the Rust owner; the C side takes over at the pointer boundary.
            Box::leak(Box::new(pp))
        }
        None => std::ptr::null_mut(),
    }
}

/// `EXPORT os_process_pipe_t *os_process_pipe_create2(const
/// os_process_args_t *args, const char *type)`
///
/// # Safety
/// `args` must be NULL or a live C `struct os_process_args` (the
/// trailing-NULL darray); `type` NUL-terminated (NULL → NULL, C guard).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_process_pipe_create2(
    args: *const CProcessArgs,
    type_: *const c_char,
) -> *mut ProcessPipe {
    let Some(type_) = crate::abi_common::c_bytes(type_) else {
        return std::ptr::null_mut();
    };
    let Some(args) = (if args.is_null() { None } else { Some(args) }) else {
        return std::ptr::null_mut(); // C: get_argv(NULL) is UB; rejected here.
    };
    // SAFETY: caller contract — `args` is a live C args struct for the call; the darray holds `num` NUL-terminated strings (the last one NULL, excluded below).
    let argv = unsafe {
        let c = &*args;
        let count = c.num.saturating_sub(1); // the trailing NULL terminator
        let mut v: Vec<String> = Vec::with_capacity(count);
        for i in 0..count {
            let s = std::ffi::CStr::from_ptr(*c.array.add(i));
            v.push(String::from_utf8_lossy(s.to_bytes()).into_owned());
        }
        v
    };
    let exe = match argv.first() {
        Some(exe) => exe.clone(),
        None => return std::ptr::null_mut(), // C: argv[0] of an empty list is the NULL terminator → spawn failure; rejected here.
    };
    let mut pa = ProcessArgs::create(&exe);
    for a in argv.iter().skip(1) {
        pa.add_arg(a);
    }
    match cd_pipe_create2(&pa, String::from_utf8_lossy(type_).as_ref()) {
        Some(pp) => {
            // SAFETY: Box is the Rust owner; the C side takes over at the pointer boundary.
            Box::leak(Box::new(pp))
        }
        None => std::ptr::null_mut(),
    }
}

/// `EXPORT int os_process_pipe_destroy(os_process_pipe_t *pp)`
///
/// # Safety
/// `pp` must be NULL (→ 0, the C guard) or a live create-handle, unused
/// afterwards.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_process_pipe_destroy(pp: *mut ProcessPipe) -> c_int {
    if pp.is_null() {
        return 0;
    }
    // SAFETY: caller contract — `pp` is a live handle, uniquely owned.
    unsafe { cd_pipe_destroy(Some(*Box::from_raw(pp))) }
}

/// `EXPORT size_t os_process_pipe_read(os_process_pipe_t *pp, uint8_t
/// *data, size_t len)`
///
/// # Safety
/// `pp` must be NULL or a live handle; `data` writable for `len` bytes
/// (NULL with `len == 0` is fine).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_process_pipe_read(
    pp: *mut ProcessPipe,
    data: *mut u8,
    len: usize,
) -> usize {
    if data.is_null() && len > 0 {
        return 0;
    }
    let pp = if pp.is_null() { None } else { Some(pp) };
    // SAFETY: caller contract — `pp` (when non-null) is a live handle for the call; `data` is writable for `len`.
    unsafe { cd_pipe_read(pp.map(|p| &*p), std::slice::from_raw_parts_mut(data, len)) }
}

/// `EXPORT size_t os_process_pipe_read_err(os_process_pipe_t *pp,
/// uint8_t *data, size_t len)`
///
/// # Safety
/// As [`os_process_pipe_read`].
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_process_pipe_read_err(
    pp: *mut ProcessPipe,
    data: *mut u8,
    len: usize,
) -> usize {
    if data.is_null() && len > 0 {
        return 0;
    }
    let pp = if pp.is_null() { None } else { Some(pp) };
    // SAFETY: caller contract — as `os_process_pipe_read`.
    unsafe { cd_pipe_read_err(pp.map(|p| &*p), std::slice::from_raw_parts_mut(data, len)) }
}

/// `EXPORT size_t os_process_pipe_write(os_process_pipe_t *pp, const
/// uint8_t *data, size_t len)`
///
/// # Safety
/// `pp` must be NULL or a live handle; `data` readable for `len` bytes
/// (NULL with `len == 0` is fine).
#[unsafe(no_mangle)]
pub unsafe extern "C" fn os_process_pipe_write(
    pp: *mut ProcessPipe,
    data: *const u8,
    len: usize,
) -> usize {
    if data.is_null() && len > 0 {
        return 0;
    }
    let pp = if pp.is_null() { None } else { Some(pp) };
    // SAFETY: caller contract — `pp` (when non-null) is a live handle for the call; `data` is readable for `len`.
    unsafe { cd_pipe_write(pp.map(|p| &*p), std::slice::from_raw_parts(data, len)) }
}
