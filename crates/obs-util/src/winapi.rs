//! Raw WinAPI surface for the Windows side of the platform-tier modules
//! (`threading`, `pipe`, `dl`, `os_proc`) — WP-38 / P1-05.3 + finding 5.
//!
//! Direct `extern "system"` declarations against `kernel32`/`kernelbase`
//! (the house FFI tier, ADR 0005 Amendment 1 platform-tier exception):
//! no `windows`-crate dependency (ADR 0005 Rule 3 allow-list), no
//! intrinsics. Signatures mirror the C reference (`WIN32_LEAN_AND_MEAN
//! windows.h` in `threading-windows.c` / `pipe-windows.c` /
//! `platform-windows.c`); on x86_64/ARM64 the `system` ABI is the plain
//! C ABI, so the Win64 structs are repr(C) mirrors with the MSVC field
//! order.

use std::os::raw::c_void;

/// `MAX_PATH` (windows.h).
pub const MAX_PATH: u32 = 260;
/// `WAIT_OBJECT_0` (waitapi.h).
pub const WAIT_OBJECT_0: u32 = 0;
/// `WAIT_TIMEOUT` (waitapi.h).
pub const WAIT_TIMEOUT: u32 = 0x102;
/// `INFINITE` (minwinbase.h).
pub const INFINITE: u32 = u32::MAX;
/// `HANDLE_FLAG_INHERIT` (winbase.h).
pub const HANDLE_FLAG_INHERIT: u32 = 0x1;
/// `CREATE_NO_WINDOW` (winbase.h) — pipe-windows.c uses it unless the
/// debug-only `SHOW_SUBPROCESSES` define is set (it never is in a Rust build).
pub const CREATE_NO_WINDOW: u32 = 0x0800_0000;
/// `STARTF_USESTDHANDLES` (winbase.h).
pub const STARTF_USESTDHANDLES: u32 = 0x0100;
/// `STARTF_FORCEOFFFEEDBACK` (winbase.h).
pub const STARTF_FORCEOFFFEEDBACK: u32 = 0x0080;
/// `LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR` (winbase.h, Win8+).
pub const LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR: u32 = 0x0000_1000;
/// `LOAD_LIBRARY_SEARCH_DEFAULT_DIRS` (winbase.h, Win8+) — the same value
/// (default dirs = DLL dir + system + Windows + PATH).
pub const LOAD_LIBRARY_SEARCH_DEFAULT_DIRS: u32 = 0x0000_1000;
/// `ERROR_PROC_NOT_FOUND` (winerror.h) — "don't print an error for
/// libraries that aren't meant to be dynamically linked" (platform-windows.c).
pub const ERROR_PROC_NOT_FOUND: u32 = 1257;
/// `FORMAT_MESSAGE_FROM_SYSTEM` (winbase.h).
pub const FORMAT_MESSAGE_FROM_SYSTEM: u32 = 0x0001;
/// `FORMAT_MESSAGE_IGNORE_INSERTS` (winbase.h).
pub const FORMAT_MESSAGE_IGNORE_INSERTS: u32 = 0x0040;
/// `FORMAT_MESSAGE_ALLOCATE_BUFFER` (winbase.h).
pub const FORMAT_MESSAGE_ALLOCATE_BUFFER: u32 = 0x0100;
/// `MAKELANGID(LANG_ENGLISH, SUBLANG_ENGLISH_US)` (winnt.h).
pub const LANG_ENGLISH_US: u32 = 0x0409;

/// C `struct SECURITY_ATTRIBUTES` (winbase.h) — inheritable pipe ends.
#[repr(C)]
#[derive(Clone, Copy)]
pub struct SecurityAttributes {
    pub n_length: u32,
    pub lp_security_descriptor: *mut c_void,
    pub b_inherit_handle: i32,
}

/// C `struct STARTUPINFOW` (wingdi.h) — the full 19-field form: the
/// console fields (`dwX`/`dwY`/…) and `lpDesktop` ARE part of the struct
/// (zeroed here; `CreateProcessW` ignores them when `dwFlags` only sets
/// `STARTF_USESTDHANDLES`). 104 bytes on x86_64/ARM64; `CreateProcessW`
/// requires `cb = sizeof`. A drifted layout would make the kernel read
/// `dwFlags`/the stdio handles from the wrong offsets (WP-38 CI round 2:
/// the 64-byte "9-field" draft read garbage stdio handles and the
/// runner aborted with 0xc0000005).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct StartUpInfoW {
    pub cb: u32,
    pub lp_reserved: *mut c_void,
    pub lp_desktop: *mut u16,
    pub lp_title: *mut u16,
    pub dw_x: u32,
    pub dw_y: u32,
    pub dw_x_size: u32,
    pub dw_y_size: u32,
    pub dw_x_count_chars: u32,
    pub dw_y_count_chars: u32,
    pub dw_fill_attribute: u32,
    pub dw_flags: u32,
    pub w_show_window: u16,
    pub cb_reserved2: u16,
    pub lp_reserved2: *mut u16,
    pub h_std_input: *mut c_void,
    pub h_std_output: *mut c_void,
    pub h_std_error: *mut c_void,
}

/// C `struct PROCESS_INFORMATION` (winbase.h).
#[repr(C)]
#[derive(Clone, Copy)]
pub struct ProcessInformation {
    pub h_process: *mut c_void,
    pub h_thread: *mut c_void,
    pub dw_process_id: u32,
    pub dw_thread_id: u32,
}

// kernel32 (the C reference includes windows.h with WIN32_LEAN_AND_MEAN)
unsafe extern "system" {
    // threading-windows.c
    // `lp_name` is `LPCSTR` (ANSI) in the C headers
    pub fn CreateEventA(
        lp_event_attributes: *const SecurityAttributes,
        b_manual_reset: i32,
        b_initial_state: i32,
        lp_name: *const u8,
    ) -> *mut c_void;
    pub fn CreateSemaphoreA(
        lp_semaphore_attributes: *const SecurityAttributes,
        l_initial_count: i32,
        l_maximum_count: i32,
        lp_name: *const u8,
    ) -> *mut c_void;
    pub fn CloseHandle(hObject: *mut c_void) -> i32;
    pub fn WaitForSingleObject(h_handle: *mut c_void, dw_milliseconds: u32) -> u32;
    pub fn SetEvent(h_event: *mut c_void) -> i32;
    pub fn ResetEvent(h_event: *mut c_void) -> i32;
    pub fn ReleaseSemaphore(
        h_semaphore: *mut c_void,
        l_release_count: u32,
        lp_previous_count: *mut u32,
    ) -> i32;
    // the pseudo-handle of the current thread ((HANDLE)-2 in C)
    pub fn GetCurrentThread() -> *mut c_void;

    // platform-windows.c (dl) + threading-windows.c (SetThreadDescription
    // is looked up by name in the C reference; the lookup needs these)
    pub fn LoadLibraryA(lp_lib_file_name: *const u8) -> *mut c_void;
    pub fn LoadLibraryExW(
        lp_library_name: *const u16,
        h_file: *mut c_void,
        dw_flags: u32,
    ) -> *mut c_void;
    pub fn GetProcAddress(h_module: *mut c_void, lp_proc_name: *const u8) -> *mut c_void;
    pub fn FreeLibrary(h_lib_module: *mut c_void) -> i32;
    pub fn GetFullPathNameW(
        lp_filename: *const u16,
        n_buffer_length: u32,
        lp_buffer: *mut u16,
        lp_file: *mut *mut u16,
    ) -> u32;
    pub fn FormatMessageA(
        dw_flags: u32,
        lp_source: *const c_void,
        dw_message_id: u32,
        dw_language_id: u32,
        lp_buffer: *mut i8,
        n_size: u32,
        arguments: *mut c_void,
    ) -> u32;
    pub fn LocalFree(hmem: *mut c_void) -> *mut c_void;
    pub fn GetLastError() -> u32;

    // pipe-windows.c
    pub fn CreatePipe(
        h_read_pipe: *mut *mut c_void,
        h_write_pipe: *mut *mut c_void,
        lp_pipe_attributes: *const SecurityAttributes,
        n_size: u32,
    ) -> i32;
    pub fn SetHandleInformation(h_handle: *mut c_void, dw_mask: u32, dw_information: u32) -> i32;
    pub fn CreateProcessW(
        lp_application_name: *const u16,
        lp_command_line: *mut u16,
        lp_process_attributes: *const SecurityAttributes,
        lp_thread_attributes: *const SecurityAttributes,
        b_inherit_handles: i32,
        dw_creation_flags: u32,
        lp_environment: *mut c_void,
        lp_current_directory: *const u16,
        lp_startup_info: *const StartUpInfoW,
        lp_process_information: *mut ProcessInformation,
    ) -> i32;
    pub fn GetExitCodeProcess(h_process: *mut c_void, lp_exit_code: *mut u32) -> i32;
    pub fn ReadFile(
        h_file: *mut c_void,
        lp_buffer: *mut c_void,
        n_number_of_bytes_to_read: u32,
        lp_number_of_bytes_read: *mut u32,
        lp_overlapped: *const c_void,
    ) -> i32;
    pub fn WriteFile(
        h_file: *mut c_void,
        lp_buffer: *const c_void,
        n_number_of_bytes_to_write: u32,
        lp_number_of_bytes_written: *mut u32,
        lp_overlapped: *const c_void,
    ) -> i32;

    // platform-windows.c (os_proc)
    pub fn GetModuleFileNameW(module: *mut c_void, lp_filename: *mut u16, n_size: u32) -> u32;
    // kernelbase (Win10 1607+); test read-back of SetThreadDescription
    #[cfg(test)]
    pub fn GetThreadDescription(h_thread: *mut c_void, ppsz_name: *mut *mut u16) -> i32;
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::mem::offset_of;

    // The Win64 struct sizes are the contract: `StartUpInfoW.cb` is
    // `sizeof` on the C side, and a drifted layout would make the kernel
    // read `dwFlags`/the stdio handles from the wrong offsets.
    #[test]
    fn win64_struct_layout_pins() {
        assert_eq!(size_of::<StartUpInfoW>(), 104);
        assert_eq!(offset_of!(StartUpInfoW, cb), 0);
        assert_eq!(offset_of!(StartUpInfoW, lp_reserved), 8);
        assert_eq!(offset_of!(StartUpInfoW, lp_desktop), 16);
        assert_eq!(offset_of!(StartUpInfoW, lp_title), 24);
        assert_eq!(offset_of!(StartUpInfoW, dw_x), 32);
        assert_eq!(offset_of!(StartUpInfoW, dw_flags), 60);
        assert_eq!(offset_of!(StartUpInfoW, w_show_window), 64);
        assert_eq!(offset_of!(StartUpInfoW, lp_reserved2), 72);
        assert_eq!(offset_of!(StartUpInfoW, h_std_input), 80);
        assert_eq!(offset_of!(StartUpInfoW, h_std_output), 88);
        assert_eq!(offset_of!(StartUpInfoW, h_std_error), 96);
        assert_eq!(size_of::<ProcessInformation>(), 24);
        assert_eq!(size_of::<SecurityAttributes>(), 24);
    }
}
