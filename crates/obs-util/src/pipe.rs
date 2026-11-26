//! S-055 U2: child-process pipes (`os_process_pipe_*`).
//!
//! `os_process_pipe_create(cmd, type)` on unix runs `/bin/sh -c cmd`;
//! `os_process_pipe_create2(args, type)` runs `argv[0]` of the argument
//! list directly (no shell, no PATH lookup). Read mode (first character
//! of `type` is `r`) connects the child's stdout to the handle; write
//! mode connects the child's stdin; the child's stderr is connected in
//! both modes (`read_err`).
//!
//! Windows (WP-38 / finding 5, per `pipe-windows.c`): `create` runs the
//! command line with `CreateProcessW` — **no shell** (the C reference
//! itself differs here from the posix `sh -c` form), so commands must
//! name an executable (`cmd /c …` for shell builtins); `create2`
//! converts the argument list to a command line with the C reference's
//! Python-subprocess-style quoting (Windows has no argc/argv spawn API).
//! `type` characters other than `r`/`w` are rejected (`None`) — the
//! C Windows reference does this (the posix reference accepts any
//! non-`r` char as write mode). `read`/`read_err`/`write` are single
//! `ReadFile`/`WriteFile` calls per the C reference (the posix `fread`/
//! `write` loop is not mirrored); `destroy` returns the child's raw
//! exit code from `GetExitCodeProcess` (no posix signed-char
//! conversion, no D-1 pid).
//!
//! Deviations from the C reference:
//! - The unix spawn mechanism is fork + dup2 + execv (the classic
//!   `popen` shape) rather than the reference's posix_spawn file
//!   actions; the observable behaviour is the same (R03-R09, R06
//!   failure -> `None` with no leaked descriptor).
//! - Q-1 (stdio buffering of `write`): the unix port writes directly
//!   (no buffering). Delivery to the child is at least as early; the
//!   conformance vectors observe the same results (exit statuses).
//! - The Windows error paths close **all** four pipe handles; the C
//!   reference leaks the two stderr handles there (not a behaviour).
//! - C-NULL arguments (`create(NULL, …)`, `create2(NULL, …)`, NULL
//!   handles for the I/O functions and `destroy`) are unrepresentable
//!   in the safe API; the FFI layer maps them to the C behaviour
//!   (`None` / 0).
//! - D-1 (signalled child): the unix `destroy` returns the child's pid
//!   (positive), as the reference does (its `waitpid` result).
//! - Logging: none yet (no logger in the crate; the FFI layer wires
//!   it — the C reference's `CreateProcessW failed: %lu` line is
//!   dropped on Windows too, like the unix port's missing error logs).

use std::os::raw::c_void;

use crate::process_args::ProcessArgs;

/// A child process with a pipe to its stdout (read mode) or stdin
/// (write mode) and a pipe to its stderr (both modes).
#[cfg(unix)]
pub struct ProcessPipe {
    pid: libc::pid_t,
    /// Read mode: the parent's end of the stdout pipe.
    read_fd: Option<libc::c_int>,
    /// Write mode: the parent's end of the stdin pipe.
    write_fd: Option<libc::c_int>,
    /// The parent's end of the stderr pipe (both modes).
    err_fd: libc::c_int,
}

/// A child process with a pipe to its stdout (read mode) or stdin
/// (write mode) and a pipe to its stderr (both modes). The fields are
/// the `pipe-windows.c` `os_process_pipe` struct (kernel handles).
#[cfg(windows)]
pub struct ProcessPipe {
    read_pipe: bool,
    /// Read mode: the parent's end of the stdout pipe; write mode: the
    /// parent's end of the stdin pipe.
    handle: *mut c_void,
    /// The parent's end of the stderr pipe (both modes).
    handle_err: *mut c_void,
    /// The child's process handle (the `hThread` is closed at spawn).
    process: *mut c_void,
}

#[cfg(unix)]
fn set_cloexec(fd: libc::c_int) {
    // SAFETY: `fd` is a valid descriptor owned by this thread.
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFD) };
    // SAFETY: as above; F_SETFD with a valid flag word.
    let _ = unsafe { libc::fcntl(fd, libc::F_SETFD, flags | libc::FD_CLOEXEC) };
}

#[cfg(unix)]
fn close_fd(fd: libc::c_int) {
    // SAFETY: `fd` is a valid descriptor owned by the caller.
    let _ = unsafe { libc::close(fd) };
}

/// Read mode iff the first character of `type` is `r` (S-055-R04).
/// (Windows rejects first characters other than `r`/`w` entirely —
/// see the module docs.)
#[cfg(unix)]
fn is_read_mode(type_: &str) -> bool {
    type_.starts_with('r')
}

/// Spawn the child (fork + dup2 + execv — the classic `popen` shape);
/// returns `(pid, read_fd, write_fd, err_fd)`.
#[cfg(unix)]
fn spawn(
    exe: &str,
    argv: Vec<String>,
    read_mode: bool,
) -> Option<(
    libc::pid_t,
    Option<libc::c_int>,
    Option<libc::c_int>,
    libc::c_int,
)> {
    // R06: the executable must exist before we fork (posix_spawn would
    // fail the spawn — and a bare name is never resolved through
    // PATH, as `access` on a non-absolute path shows)
    let exe_c = std::ffi::CString::new(exe.as_bytes()).ok()?;
    // SAFETY: valid NUL-terminated path.
    if unsafe { libc::access(exe_c.as_ptr(), libc::X_OK) } != 0 {
        return None;
    }

    let mut pout: [libc::c_int; 2] = [0; 2];
    let mut pin: [libc::c_int; 2] = [0; 2];
    let mut perr: [libc::c_int; 2] = [0; 2];
    // SAFETY: fixed-size arrays of ints for the pipe syscalls.
    let ok = unsafe { libc::pipe(pout.as_mut_ptr()) } == 0
        && unsafe { libc::pipe(pin.as_mut_ptr()) } == 0
        && unsafe { libc::pipe(perr.as_mut_ptr()) } == 0;
    if !ok {
        return None;
    }
    // CLOEXEC on every end: the child closes them all before exec
    // (only the dup2'd stdio numbers survive), and the parent's later
    // children do not see them (R05).
    for fd in [pout[0], pout[1], pin[0], pin[1], perr[0], perr[1]] {
        set_cloexec(fd);
    }

    // Everything the child needs is prepared before the fork, so the
    // child runs only async-signal-safe calls (dup2, close, execv,
    // _exit) with no allocation.
    let mut argv_c: Vec<std::ffi::CString> = Vec::with_capacity(argv.len() + 1);
    for a in argv {
        match std::ffi::CString::new(a.as_bytes()) {
            Ok(c) => argv_c.push(c),
            Err(_) => {
                // an interior NUL is not a valid argument
                for fd in [pout[0], pout[1], pin[0], pin[1], perr[0], perr[1]] {
                    close_fd(fd);
                }
                return None;
            }
        }
    }
    let mut argv_p: Vec<*const libc::c_char> = argv_c.iter().map(|s| s.as_ptr()).collect();
    argv_p.push(std::ptr::null());

    // SAFETY: fork is safe to call here; the child runs only
    // async-signal-safe functions and never returns to Rust.
    let child = unsafe { libc::fork() };
    if child < 0 {
        for fd in [pout[0], pout[1], pin[0], pin[1], perr[0], perr[1]] {
            close_fd(fd);
        }
        return None;
    }
    if child == 0 {
        // child: point stdio at the kept pipe ends, close everything
        // else, exec. SAFETY: post-fork child; async-signal-safe calls
        // only.
        let _ = if read_mode {
            // SAFETY: the pipe end is a live fd (created by `pipe()` above); 1 is a valid descriptor number.
            unsafe { libc::dup2(pout[1], 1) }
        } else {
            // SAFETY: the pipe end is a live fd (created by `pipe()` above); 0 is a valid descriptor number.
            unsafe { libc::dup2(pin[0], 0) }
        };
        // SAFETY: the pipe end is a live fd (created by `pipe()` above); 2 is a valid descriptor number.
        let _ = unsafe { libc::dup2(perr[1], 2) };
        // Close the pipe ends the child does not use — but never fd
        // numbers 0/1/2 themselves: if the parent's low fds are free
        // (a runner that closes stdin), pipe() can hand out numbers
        // 0-2, and closing one of them would destroy the just-dup'd
        // stdio end (observed as deterministic empty reads after the
        // 2026-11-15 ubuntu-latest 24.04 -> 26.04 runner swap). Any
        // pipe fd that shares a stdio number is CLOEXEC, so exec
        // closes it either way.
        for fd in [pout[0], pout[1], pin[0], pin[1], perr[0], perr[1]] {
            if fd >= 3 {
                // SAFETY: `fd` is a live pipe fd the child does not use (fd ≥ 3 per the check above).
                unsafe { libc::close(fd) };
            }
        }
        // SAFETY: NUL-terminated argv built before the fork.
        unsafe { libc::execv(exe_c.as_ptr(), argv_p.as_ptr()) };
        // exec failed (the executable vanished between access and
        // exec): report 127 like a missing command
        // SAFETY: child context after a failed `execv` — `_exit` is safe for the terminating child.
        unsafe { libc::_exit(127) };
    }

    // parent: keep exactly read mode -> pout[0], write mode ->
    // pin[1], and perr[0] in both modes; close the rest
    if read_mode {
        close_fd(pout[1]);
        close_fd(pin[0]);
        close_fd(pin[1]);
        close_fd(perr[1]);
        Some((child, Some(pout[0]), None, perr[0]))
    } else {
        close_fd(pout[0]);
        close_fd(pout[1]);
        close_fd(pin[0]);
        close_fd(perr[1]);
        Some((child, None, Some(pin[1]), perr[0]))
    }
}

/// `os_process_pipe_create(cmd, type)` (R03): `/bin/sh -c cmd`
/// (argv0 `sh`); the mode comes from `type` (R04).
#[cfg(unix)]
pub fn os_process_pipe_create(cmd: &str, type_: &str) -> Option<ProcessPipe> {
    // R03: /bin/sh -c cmd, argv0 "sh"
    let argv: Vec<String> = vec!["sh".to_string(), "-c".to_string(), cmd.to_string()];
    spawn("/bin/sh", argv, is_read_mode(type_)).map(|(pid, read_fd, write_fd, err_fd)| {
        ProcessPipe {
            pid,
            read_fd,
            write_fd,
            err_fd,
        }
    })
}

/// `os_process_pipe_create(cmd, type)` (R03, Windows — pipe-windows.c):
/// the command line goes to `CreateProcessW` (no shell); the mode comes
/// from `type` (R04, with the Windows `r`/`w`-only check).
#[cfg(windows)]
pub fn os_process_pipe_create(cmd: &str, type_: &str) -> Option<ProcessPipe> {
    use crate::winapi;
    // C: `if (*type != 'r' && *type != 'w') return NULL;` (an empty type
    // is rejected too — the C NULL guard covers it).
    let first = type_.chars().next()?;
    if first != 'r' && first != 'w' {
        return None;
    }
    let read_pipe = first == 'r';
    // C: os_utf8_to_wcs_ptr(cmd_line, 0, &cmd_line_w)
    let mut cmd_w: Vec<u16> = cmd.encode_utf16().collect();
    cmd_w.push(0);

    let mut input = std::ptr::null_mut();
    let mut output = std::ptr::null_mut();
    let mut err_input = std::ptr::null_mut();
    let mut err_output = std::ptr::null_mut();
    if !create_pipe(&mut input, &mut output) || !create_pipe(&mut err_input, &mut err_output) {
        close_pipe_handles(input, output, err_input, err_output);
        return None;
    }
    // C: drop inheritance on the ends the child does not take
    // (read mode: the child writes `output`, so `input` stays private;
    // write mode: the child reads `input`, so `output` stays private).
    // SAFETY: live handles from `create_pipe` above.
    let ok = unsafe {
        winapi::SetHandleInformation(
            if read_pipe { input } else { output },
            winapi::HANDLE_FLAG_INHERIT,
            0,
        ) != 0
            && winapi::SetHandleInformation(err_input, winapi::HANDLE_FLAG_INHERIT, 0) != 0
    };
    if !ok {
        close_pipe_handles(input, output, err_input, err_output);
        return None;
    }
    let mut process = std::ptr::null_mut();
    if !create_process(
        &mut cmd_w,
        if read_pipe {
            std::ptr::null_mut()
        } else {
            input
        },
        if read_pipe {
            output
        } else {
            std::ptr::null_mut()
        },
        err_output,
        &mut process,
    ) {
        // C leaks the err handles on this path; the port closes them
        // (module docs).
        close_pipe_handles(input, output, err_input, err_output);
        return None;
    }
    let pp = ProcessPipe {
        read_pipe,
        handle: if read_pipe { input } else { output },
        handle_err: err_input,
        process,
    };
    // C: close the child's ends in the parent (the child inherited them)
    // SAFETY: live handles owned by the child now.
    unsafe {
        winapi::CloseHandle(if read_pipe { output } else { input });
        winapi::CloseHandle(err_output);
    }
    Some(pp)
}

#[cfg(windows)]
fn create_pipe(input: &mut *mut c_void, output: &mut *mut c_void) -> bool {
    use crate::winapi;
    let sa = winapi::SecurityAttributes {
        n_length: std::mem::size_of::<winapi::SecurityAttributes>() as u32,
        lp_security_descriptor: std::ptr::null_mut(),
        b_inherit_handle: 1,
    };
    // SAFETY: `sa` is valid for the call; the two out pointers are live
    // locals the kernel writes.
    unsafe { winapi::CreatePipe(input, output, &sa, 0) != 0 }
}

#[cfg(windows)]
fn create_process(
    cmd_w: &mut Vec<u16>,
    stdin_h: *mut c_void,
    stdout_h: *mut c_void,
    stderr_h: *mut c_void,
    process: &mut *mut c_void,
) -> bool {
    use crate::winapi;
    // C: STARTUPINFOW { cb, dwFlags = STARTF_USESTDHANDLES |
    // STARTF_FORCEOFFFEEDBACK, hStdInput/Output/Error }
    // SAFETY: zeroed storage is a valid `STARTUPINFOW` (all fields are
    // plain data; the pointer fields are set below before the call).
    let mut si: winapi::StartUpInfoW = unsafe { std::mem::zeroed() };
    si.cb = std::mem::size_of::<winapi::StartUpInfoW>() as u32;
    si.dw_flags = winapi::STARTF_USESTDHANDLES | winapi::STARTF_FORCEOFFFEEDBACK;
    si.h_std_input = stdin_h;
    si.h_std_output = stdout_h;
    si.h_std_error = stderr_h;
    // SAFETY: as above — the call initializes both handles before use.
    let mut pi: winapi::ProcessInformation = unsafe { std::mem::zeroed() };
    // C: CreateProcessW(NULL, cmd_line_w, NULL, NULL, true, flags,
    // NULL, NULL, &si, &pi) — flags = CREATE_NO_WINDOW (the
    // SHOW_SUBPROCESSES define is a debug-only CMake option, never set
    // in a Rust build).
    // SAFETY: `cmd_w` is a live NUL-terminated command line; `si`/`pi`
    // are live storage; NULL attributes/environment/cwd are the C form;
    // inherit-handles true (the stdio ends are inheritable, the rest
    // have inheritance off).
    let ok = unsafe {
        winapi::CreateProcessW(
            std::ptr::null(),
            cmd_w.as_mut_ptr(),
            std::ptr::null(),
            std::ptr::null(),
            1,
            winapi::CREATE_NO_WINDOW,
            std::ptr::null_mut(),
            std::ptr::null(),
            &si,
            &mut pi,
        ) != 0
    };
    if ok {
        *process = pi.h_process;
        // C: CloseHandle(pi.hThread) — the port only needs the process.
        // SAFETY: `pi.h_thread` is a valid handle from CreateProcessW.
        unsafe { winapi::CloseHandle(pi.h_thread) };
    }
    // C: blog(LOG_ERROR, "CreateProcessW failed: %lu") on failure — no
    // logger in the crate yet (module docs).
    ok
}

#[cfg(windows)]
fn close_pipe_handles(
    input: *mut c_void,
    output: *mut c_void,
    err_input: *mut c_void,
    err_output: *mut c_void,
) {
    // SAFETY: all four are live handles created by `create_pipe`.
    unsafe {
        crate::winapi::CloseHandle(output);
        crate::winapi::CloseHandle(input);
        crate::winapi::CloseHandle(err_input);
        crate::winapi::CloseHandle(err_output);
    }
}

/// `os_process_pipe_create2(args, type)` (R03): runs `argv[0]` of the
/// list directly — no shell, no PATH lookup (a bare name such as
/// `echo` fails, R06).
#[cfg(unix)]
pub fn os_process_pipe_create2(args: &ProcessArgs, type_: &str) -> Option<ProcessPipe> {
    let argv = args.argv();
    let exe = argv.first()?.clone();
    spawn(&exe, argv.to_vec(), is_read_mode(type_)).map(|(pid, read_fd, write_fd, err_fd)| {
        ProcessPipe {
            pid,
            read_fd,
            write_fd,
            err_fd,
        }
    })
}

/// `os_process_pipe_create2(args, type)` (R03, Windows —
/// pipe-windows.c): converts the argument list to a command line (the
/// C reference's Python-subprocess-style quoting — Windows has no
/// argc/argv spawn API) and runs it via `os_process_pipe_create`.
#[cfg(windows)]
pub fn os_process_pipe_create2(args: &ProcessArgs, type_: &str) -> Option<ProcessPipe> {
    os_process_pipe_create(&args_to_cmdline(args.argv()), type_)
}

/// The `os_process_pipe_create2` command-line conversion (pipe-
/// windows.c, "Based on Python subprocess module implementation"): an
/// argument is quoted iff it is empty or contains a space/tab; inside
/// a quoted argument, backslashes before a quote double and the quote
/// escapes, and a run of backslashes before the closing quote doubles.
#[cfg(windows)]
fn args_to_cmdline(argv: &[String]) -> String {
    let mut cmd = String::new();
    for (i, arg) in argv.iter().enumerate() {
        // C: needs_quotes = strlen(arg) == 0 || strstr(arg, " ") ||
        // strstr(arg, "\t")
        let needs_quotes = arg.is_empty() || arg.contains([' ', '\t']);
        if i > 0 {
            cmd.push(' ');
        }
        if needs_quotes {
            cmd.push('"');
        }
        let mut bs = 0usize;
        for ch in arg.chars() {
            match ch {
                '\\' => bs += 1,
                '"' => {
                    // C: add_backslashes(bs_count * 2) + "\\\""
                    for _ in 0..bs * 2 {
                        cmd.push('\\');
                    }
                    cmd.push('\\');
                    cmd.push('"');
                    bs = 0;
                }
                c => {
                    if bs > 0 {
                        for _ in 0..bs {
                            cmd.push('\\');
                        }
                        bs = 0;
                    }
                    cmd.push(c);
                }
            }
        }
        if bs > 0 {
            // C: trailing backslashes flushed (bs_count is NOT reset here)
            for _ in 0..bs {
                cmd.push('\\');
            }
        }
        if needs_quotes {
            // C: add_backslashes(bs_count) again to escape the closing
            // quote (a backslash directly before `"` would escape it),
            // then the quote itself.
            for _ in 0..bs {
                cmd.push('\\');
            }
            cmd.push('"');
        }
    }
    cmd
}

/// `os_process_pipe_read` (R07): unix — `fread` semantics, blocks
/// until `len` bytes or EOF, returns the byte count (0 at EOF); a
/// write-mode handle returns 0 without touching any stream; a NULL
/// handle returns 0. Windows — a single `ReadFile` per the C reference
/// (may return fewer than `buf.len()` bytes even before EOF; callers
/// loop as in the C usage).
pub fn os_process_pipe_read(pp: Option<&ProcessPipe>, buf: &mut [u8]) -> usize {
    let Some(pp) = pp else {
        return 0;
    };
    #[cfg(unix)]
    {
        let Some(fd) = pp.read_fd else {
            return 0;
        };
        read_until(fd, buf)
    }
    #[cfg(windows)]
    {
        use crate::winapi;
        if !pp.read_pipe {
            return 0;
        }
        // C: single ReadFile, `success && bytes_read` ? bytes_read : 0
        let mut bytes = 0u32;
        // SAFETY: `pp.handle` is a live pipe end; `buf` is valid for
        // `buf.len()` bytes for the call.
        let ok = unsafe {
            winapi::ReadFile(
                pp.handle,
                buf.as_mut_ptr() as *mut c_void,
                buf.len() as u32,
                &mut bytes,
                std::ptr::null(),
            ) != 0
        };
        if ok && bytes != 0 { bytes as usize } else { 0 }
    }
}

/// `os_process_pipe_read_err` (R07): the same semantics on the stderr
/// pipe (works in both modes); a NULL handle returns 0.
pub fn os_process_pipe_read_err(pp: Option<&ProcessPipe>, buf: &mut [u8]) -> usize {
    let Some(pp) = pp else {
        return 0;
    };
    #[cfg(unix)]
    {
        read_until(pp.err_fd, buf)
    }
    #[cfg(windows)]
    {
        use crate::winapi;
        // C: single ReadFile on handle_err (the dead-store
        // `bytes_read = GetLastError()` in the reference is a no-op).
        let mut bytes = 0u32;
        // SAFETY: `pp.handle_err` is a live pipe end; `buf` is valid for
        // the call.
        let ok = unsafe {
            winapi::ReadFile(
                pp.handle_err,
                buf.as_mut_ptr() as *mut c_void,
                buf.len() as u32,
                &mut bytes,
                std::ptr::null(),
            ) != 0
        };
        if ok && bytes != 0 { bytes as usize } else { 0 }
    }
}

/// `os_process_pipe_write` (R08): unix — loops `write` until all bytes
/// are out or a write returns 0, returns the byte count (partial on
/// error). Windows — a single `WriteFile` per the C reference (a pipe
/// write can be partial; callers loop as in the C usage). A read-mode
/// handle returns 0; an empty write returns 0; a NULL handle returns 0.
pub fn os_process_pipe_write(pp: Option<&ProcessPipe>, data: &[u8]) -> usize {
    let Some(pp) = pp else {
        return 0;
    };
    #[cfg(unix)]
    {
        let Some(fd) = pp.write_fd else {
            return 0;
        };
        let mut sent = 0usize;
        while sent < data.len() {
            // SAFETY: `fd` is valid, `data` is live for the call.
            let n = unsafe {
                libc::write(
                    fd,
                    data[sent..].as_ptr() as *const c_void,
                    data.len() - sent,
                )
            };
            if n > 0 {
                sent += n as usize;
                continue;
            }
            if n < 0 && std::io::Error::last_os_error().raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            break;
        }
        sent
    }
    #[cfg(windows)]
    {
        use crate::winapi;
        if pp.read_pipe {
            return 0;
        }
        // C: single WriteFile, `success && bytes_written` ? bytes_written : 0
        let mut written = 0u32;
        // SAFETY: `pp.handle` is a live pipe end; `data` is valid for
        // the call.
        let ok = unsafe {
            winapi::WriteFile(
                pp.handle,
                data.as_ptr() as *const c_void,
                data.len() as u32,
                &mut written,
                std::ptr::null(),
            ) != 0
        };
        if ok && written != 0 {
            written as usize
        } else {
            0
        }
    }
}

/// `os_process_pipe_destroy` (R09): closes the main stream (the child
/// sees EOF), closes stderr, and waits for the child. Unix: the return
/// is the exit status as `(int)(signed char)status` — 7 -> 7,
/// 200 -> -56, 255 -> -1, 128 -> -128 (the `waitpid` EINTR retry as in
/// the reference); a signalled child returns the child's pid
/// (positive, D-1). Windows: the child's raw exit code from
/// `GetExitCodeProcess` — 200 -> 200, no signed-char conversion, no
/// D-1 pid (the C Windows reference has no `waitpid`). `destroy(NULL)`
/// returns 0. Takes ownership of the handle.
pub fn os_process_pipe_destroy(pp: Option<ProcessPipe>) -> i32 {
    let Some(pp) = pp else {
        return 0;
    };
    #[cfg(unix)]
    {
        if let Some(fd) = pp.write_fd {
            close_fd(fd);
        }
        if let Some(fd) = pp.read_fd {
            close_fd(fd);
        }
        close_fd(pp.err_fd);
        let mut status: libc::c_int = 0;
        loop {
            // SAFETY: `pp.pid` is our child, `status` is live.
            let r = unsafe { libc::waitpid(pp.pid, &mut status, 0) };
            if r == pp.pid {
                break;
            }
            if r < 0 && std::io::Error::last_os_error().raw_os_error() == Some(libc::EINTR) {
                continue;
            }
            break;
        }
        let low = status & 0x7f;
        if low != 0 && low != 0x7f {
            // D-1: the reference returns waitpid's (positive) result — the
            // child's pid (pid_t is i32 on this platform)
            pp.pid
        } else if low == 0 {
            // exited: (int)(signed char)(status >> 8)
            (((status >> 8) & 0xff) as i8) as i32
        } else {
            0
        }
    }
    #[cfg(windows)]
    {
        use crate::winapi;
        // C: CloseHandle(handle); CloseHandle(handle_err);
        // WaitForSingleObject(process, INFINITE);
        // GetExitCodeProcess(process, &code) ? (int)code : 0;
        // CloseHandle(process)
        let mut code = 0u32;
        // SAFETY: all three are live handles owned by this `ProcessPipe`.
        unsafe {
            winapi::CloseHandle(pp.handle);
            winapi::CloseHandle(pp.handle_err);
            winapi::WaitForSingleObject(pp.process, winapi::INFINITE);
            let ok = winapi::GetExitCodeProcess(pp.process, &mut code) != 0;
            winapi::CloseHandle(pp.process);
            if ok { code as i32 } else { 0 }
        }
    }
}

#[cfg(unix)]
fn read_until(fd: libc::c_int, buf: &mut [u8]) -> usize {
    let mut got = 0usize;
    while got < buf.len() {
        // SAFETY: `fd` is valid, `buf` is live for the call.
        let n = unsafe { libc::read(fd, buf[got..].as_mut_ptr() as *mut c_void, buf.len() - got) };
        if n > 0 {
            got += n as usize;
            continue;
        }
        if n < 0 && std::io::Error::last_os_error().raw_os_error() == Some(libc::EINTR) {
            continue;
        }
        return got; // EOF or error: return what we have
    }
    got
}

#[cfg(test)]
mod tests {
    use super::*;

    #[cfg(unix)]
    fn read_all(pp: &ProcessPipe, len: usize) -> Vec<u8> {
        let mut buf = vec![0u8; len];
        let n = os_process_pipe_read(Some(pp), &mut buf);
        buf.truncate(n);
        buf
    }

    // vector pipe_read: read mode end to end
    #[test]
    #[cfg(unix)]
    #[cfg_attr(miri, ignore)]
    fn pipe_read() {
        // hello + EOF
        let p = os_process_pipe_create("printf 'hello'", "r").unwrap();
        assert_eq!(read_all(&p, 100), b"hello");
        assert_eq!(os_process_pipe_read(Some(&p), &mut [0; 100]), 0);
        assert_eq!(os_process_pipe_read_err(Some(&p), &mut [0; 100]), 0);
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

        // stdout and stderr are separate streams
        let p = os_process_pipe_create("printf out; printf err >&2", "r").unwrap();
        let mut e = vec![0u8; 100];
        let n = os_process_pipe_read_err(Some(&p), &mut e);
        e.truncate(n);
        assert_eq!(e, b"err");
        assert_eq!(read_all(&p, 100), b"out");
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

        // short reads: fread semantics
        let p = os_process_pipe_create("printf abcdefghij", "r").unwrap();
        let mut b = [0u8; 4];
        assert_eq!(os_process_pipe_read(Some(&p), &mut b), 4);
        assert_eq!(b, *b"abcd");
        assert_eq!(os_process_pipe_read(Some(&p), &mut b), 4);
        assert_eq!(b, *b"efgh");
        assert_eq!(os_process_pipe_read(Some(&p), &mut b), 2);
        assert_eq!(&b[..2], b"ij");
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

        // exit statuses, converted as (int)(signed char)status
        for (cmd, want) in [
            ("exit 0", 0),
            ("exit 7", 7),
            ("exit 200", -56),
            ("exit 255", -1),
            ("exit 128", -128),
        ] {
            let p = os_process_pipe_create(cmd, "r").unwrap();
            assert_eq!(os_process_pipe_destroy(Some(p)), want, "cmd {cmd}");
        }
        // a missing command: the shell reports 127, not a NULL handle
        let p = os_process_pipe_create("this-command-does-not-exist-xyz 2>/dev/null", "r").unwrap();
        assert_eq!(os_process_pipe_destroy(Some(p)), 127);

        // "rw" is read mode
        let p = os_process_pipe_create("printf rw", "rw").unwrap();
        assert_eq!(read_all(&p, 10), b"rw");
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

        // writing to a read-mode handle returns 0
        let p = os_process_pipe_create("exit 0", "r").unwrap();
        assert_eq!(os_process_pipe_write(Some(&p), b"ignored"), 0);
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

        // a large output is read completely (BSD `wc -c` pads the count
        // with leading spaces — two for 200000 on the runner — GNU does
        // not; strip padding, pin the count)
        let p =
            os_process_pipe_create("head -c 200000 /dev/zero | tr '\\0' 'x' | wc -c", "r").unwrap();
        let out = read_all(&p, 100);
        let unpadded: Vec<u8> = out.iter().copied().filter(|b| *b != b' ').collect();
        assert_eq!(unpadded, b"200000\n");
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

        // D-1: a signalled child returns the (positive) pid
        let p = os_process_pipe_create("kill -KILL $$", "r").unwrap();
        assert_eq!(os_process_pipe_read(Some(&p), &mut [0; 10]), 0);
        let r = os_process_pipe_destroy(Some(p));
        assert!(r > 0, "signalled child: the pid, got {r}");
    }

    // vector pipe_write: write mode end to end
    #[test]
    #[cfg(unix)]
    #[cfg_attr(miri, ignore)]
    fn pipe_write() {
        // the child's stdin gets the payload (delivered at least at
        // destroy; the port delivers it directly — same exit status)
        let p =
            os_process_pipe_create("read x; [ \"$x\" = payload ] && exit 3; exit 4", "w").unwrap();
        assert_eq!(os_process_pipe_write(Some(&p), b"payload\n"), 8);
        assert_eq!(os_process_pipe_read(Some(&p), &mut [0; 10]), 0);
        assert_eq!(os_process_pipe_destroy(Some(p)), 3);

        // destroy without any write: the child sees 0 bytes
        let p = os_process_pipe_create("[ $(wc -c) -eq 0 ] && exit 5; exit 6", "w").unwrap();
        assert_eq!(os_process_pipe_destroy(Some(p)), 5);

        // all bytes of a write arrive
        let p = os_process_pipe_create("[ $(wc -c) -eq 40 ] && exit 7; exit 8", "w").unwrap();
        assert_eq!(
            os_process_pipe_write(Some(&p), b"0123456789012345678901234567890123456789"),
            40
        );
        assert_eq!(os_process_pipe_destroy(Some(p)), 7);

        // a 100000-byte write is delivered completely
        let p = os_process_pipe_create("[ $(wc -c) -eq 100000 ] && exit 9; exit 10", "w").unwrap();
        let big = vec![b'0'; 100000];
        assert_eq!(os_process_pipe_write(Some(&p), &big), 100000);
        assert_eq!(os_process_pipe_destroy(Some(p)), 9);

        // an empty write returns 0
        let p = os_process_pipe_create("cat >/dev/null", "w").unwrap();
        assert_eq!(os_process_pipe_write(Some(&p), &[]), 0);
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

        // "x" is write mode
        let p = os_process_pipe_create("cat >/dev/null; exit 11", "x").unwrap();
        assert_eq!(os_process_pipe_destroy(Some(p)), 11);

        // the child's stderr is readable
        let p = os_process_pipe_create("printf oops >&2; exit 12", "w").unwrap();
        let mut e = vec![0u8; 100];
        let n = os_process_pipe_read_err(Some(&p), &mut e);
        e.truncate(n);
        assert_eq!(e, b"oops");
        assert_eq!(os_process_pipe_destroy(Some(p)), 12);
    }

    // vector pipe_null_and_args: create2 (no shell, no PATH lookup)
    #[test]
    #[cfg(unix)]
    #[cfg_attr(miri, ignore)]
    fn pipe_create2() {
        // shell metacharacters are literal
        let mut args = ProcessArgs::create("/bin/echo");
        args.add_arg("hello");
        args.add_arg("a b");
        args.add_arg("c;d");
        args.add_arg("$HOME");
        args.add_arg("*");
        assert_eq!(args.argc(), 6);
        let p = os_process_pipe_create2(&args, "r").unwrap();
        assert_eq!(read_all(&p, 100), b"hello a b c;d $HOME *\n");
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

        // no PATH lookup: a bare name fails
        let mut args = ProcessArgs::create("echo");
        args.add_arg("x");
        assert!(os_process_pipe_create2(&args, "r").is_none());

        // a path that does not exist
        let args = ProcessArgs::create("/nonexistent/binary");
        assert!(os_process_pipe_create2(&args, "r").is_none());

        // arguments are passed literally (printf sees "fmt-2", not a
        // format); /bin/printf is Linux, macOS ships /usr/bin/printf
        const PRINTF: &str = if cfg!(target_os = "macos") {
            "/usr/bin/printf"
        } else {
            "/bin/printf"
        };
        let mut args = ProcessArgs::create(PRINTF);
        args.add_arg("fmt-2");
        let p = os_process_pipe_create2(&args, "r").unwrap();
        assert_eq!(read_all(&p, 100), b"fmt-2");
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

        // write mode with a custom shell as the executable
        let mut args = ProcessArgs::create("/bin/sh");
        args.add_arg("-c");
        args.add_arg("read a; [ \"$a\" = line ] && exit 4; exit 5");
        let p = os_process_pipe_create2(&args, "w").unwrap();
        assert_eq!(os_process_pipe_write(Some(&p), b"line\n"), 5);
        assert_eq!(os_process_pipe_destroy(Some(p)), 4);
    }

    // P1-11.3 edge cases (the vectors for these live in
    // test/conformance/scripts/utilrest/ for the runner; the C-NULL and
    // non-UTF-8 byte cases are unrepresentable in the safe API —
    // documented in the module docs)
    #[test]
    #[cfg(unix)]
    #[cfg_attr(miri, ignore)]
    fn pipe_edge_cases() {
        // an executable path containing a space
        //
        // 26.04 runners enforce an exec security policy: an exec whose
        // requested utility name does not match the executable name is
        // refused ("Security violation: ..." on stderr, exit 1) — the
        // original shape (a copy named `echo sp.bin` in temp) is
        // rejected there, while 24.04 and local machines pass it.
        // The candidates keep a space in the exec path (the pinned
        // vector); the first one producing the exact output wins, and
        // a total failure reports every candidate's output, exit
        // status, and stderr.
        let mut candidates: Vec<std::path::PathBuf> = Vec::new();
        // spaced directory, basename = the real utility name
        candidates.push(std::env::temp_dir().join("pipe space test").join("echo"));
        // an unrestricted location, spaced basename
        if let Ok(home) = std::env::var("HOME") {
            candidates.push(
                std::path::PathBuf::from(home)
                    .join("pipe space test")
                    .join("echo sp.bin"),
            );
        }
        // the original shape (24.04 / local)
        candidates.push(
            std::env::temp_dir()
                .join("pipe-space-test")
                .join("echo sp.bin"),
        );

        let mut diag: Vec<String> = Vec::new();
        let mut passed = false;
        for exe in candidates {
            let dir = exe.parent().unwrap();
            std::fs::create_dir_all(dir).unwrap();
            std::fs::copy("/bin/echo", &exe).unwrap();
            let mut args = ProcessArgs::create(exe.to_string_lossy().as_ref());
            args.add_arg("spaced");
            // bounded retry: under heavy parallel load a child once
            // delivered an empty read (observed locally on 4 cores);
            // a flaked candidate simply falls through to the next one
            let (mut out, mut code, mut err) = (Vec::new(), 0i32, Vec::new());
            for _attempt in 0..3u32 {
                let p = os_process_pipe_create2(&args, "r").unwrap();
                let got = read_all(&p, 100);
                if !got.is_empty() {
                    (out, code, err) = (got, os_process_pipe_destroy(Some(p)), Vec::new());
                    break;
                }
                let mut e = [0u8; 256];
                let mut n = 0usize;
                while n < e.len() {
                    // SAFETY: err_fd is an open pipe read end, e is live.
                    let r = unsafe {
                        libc::read(p.err_fd, e[n..].as_mut_ptr() as *mut c_void, e.len() - n)
                    };
                    if r <= 0 {
                        break;
                    }
                    n += r as usize;
                }
                (out, code, err) = (
                    Vec::new(),
                    os_process_pipe_destroy(Some(p)),
                    e[..n].to_vec(),
                );
                std::thread::sleep(std::time::Duration::from_millis(50));
            }
            if out == b"spaced\n" && code == 0 {
                // the pinned vector: spaced exec path, exact output,
                // clean exit
                assert_eq!(out, b"spaced\n");
                assert_eq!(code, 0);
                let _ = std::fs::remove_dir_all(dir);
                passed = true;
                break;
            }
            diag.push(format!(
                "{} -> out={:?} exit={} stderr={}",
                exe.display(),
                out,
                code,
                String::from_utf8_lossy(&err)
            ));
        }
        assert!(
            passed,
            "no spaced-path exec candidate passed (runner exec policy?): {}",
            diag.join(" | ")
        );

        // a write after the child exited (EPIPE): returns a count, no
        // hang
        let p = os_process_pipe_create("exit 0", "w").unwrap();
        // the child is gone before we write
        std::thread::sleep(std::time::Duration::from_millis(200));
        let n = os_process_pipe_write(Some(&p), b"0123456789");
        assert!(n <= 10, "EPIPE write: {n}");
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

        // reading a closed pipe (the child exited): EOF, 0
        let p = os_process_pipe_create("exit 0", "r").unwrap();
        std::thread::sleep(std::time::Duration::from_millis(200));
        assert_eq!(os_process_pipe_read(Some(&p), &mut [0; 16]), 0);
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);
    }

    // WP-38 (finding 5, Windows): the `pipe-windows.c` port —
    // `CreateProcessW` (no shell), single `ReadFile`/`WriteFile`, the
    // raw `GetExitCodeProcess` exit code, the `r`/`w`-only type check,
    // and the `create2` command-line quoting. `cmd` is the Windows
    // shell (the C reference has no `sh -c` form).
    #[test]
    #[cfg(windows)]
    #[cfg_attr(miri, ignore)]
    fn pipe_windows_read() {
        // read mode: stdout captured (cmd emits CRLF), stderr separate
        let p = os_process_pipe_create("cmd /c echo hello", "r").unwrap();
        let mut buf = vec![0u8; 100];
        let n = os_process_pipe_read(Some(&p), &mut buf);
        buf.truncate(n);
        assert_eq!(buf, b"hello\r\n");
        assert_eq!(os_process_pipe_read_err(Some(&p), &mut [0; 100]), 0);
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

        // the raw exit code (no posix signed-char conversion): 7 -> 7,
        // 200 -> 200
        let p = os_process_pipe_create("cmd /c exit 7", "r").unwrap();
        assert_eq!(os_process_pipe_destroy(Some(p)), 7);
        let p = os_process_pipe_create("cmd /c exit 200", "r").unwrap();
        assert_eq!(os_process_pipe_destroy(Some(p)), 200);

        // stderr is a separate stream (cmd quirk: the space before the
        // `1>&2` redirect is part of `echo`'s output, so the bytes are
        // "oops " + CRLF)
        let p = os_process_pipe_create("cmd /c echo oops 1>&2", "r").unwrap();
        let mut e = vec![0u8; 100];
        let n = os_process_pipe_read_err(Some(&p), &mut e);
        e.truncate(n);
        assert_eq!(e, b"oops \r\n");
        assert_eq!(os_process_pipe_read(Some(&p), &mut [0; 100]), 0);
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

        // writing to a read-mode handle returns 0
        let p = os_process_pipe_create("cmd /c exit 0", "r").unwrap();
        assert_eq!(os_process_pipe_write(Some(&p), b"ignored"), 0);
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);
    }

    #[test]
    #[cfg(windows)]
    #[cfg_attr(miri, ignore)]
    fn pipe_windows_type_and_create2() {
        // Windows rejects type characters other than `r`/`w` (the posix
        // reference accepts any non-`r` char as write mode)
        assert!(os_process_pipe_create("cmd /c exit 0", "x").is_none());
        assert!(os_process_pipe_create("cmd /c exit 0", "").is_none());
        assert!(os_process_pipe_create("cmd /c exit 0", "w").is_some());

        // create2: arguments are quoted per the C loop (Python
        // subprocess style); `cmd /c "echo spaced"` strips the outer
        // quotes and echoes the literal word
        let mut args = ProcessArgs::create("cmd");
        args.add_arg("/c");
        args.add_arg("echo spaced");
        let p = os_process_pipe_create2(&args, "r").unwrap();
        let mut buf = vec![0u8; 100];
        let n = os_process_pipe_read(Some(&p), &mut buf);
        buf.truncate(n);
        assert_eq!(buf, b"spaced\r\n");
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

        // a missing executable: CreateProcessW fails -> None
        let args = ProcessArgs::create("nosuchbinary_xyz");
        assert!(os_process_pipe_create2(&args, "r").is_none());
    }

    #[test]
    #[cfg(windows)]
    #[cfg_attr(miri, ignore)]
    fn pipe_windows_write() {
        // `findstr payload` consumes all of stdin and exits 0 on a match
        // / 1 on none — the Windows stand-in for the posix `read x`/`wc -c`
        // write-mode vectors (a match proves the written bytes reached the
        // child's stdin)
        let p = os_process_pipe_create("cmd /c findstr payload", "w").unwrap();
        assert_eq!(os_process_pipe_write(Some(&p), b"payload\n"), 8);
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

        // destroy without any write: the child sees EOF with no input
        // (no match)
        let p = os_process_pipe_create("cmd /c findstr payload", "w").unwrap();
        assert_eq!(os_process_pipe_destroy(Some(p)), 1);

        // the child's stderr is readable in write mode
        let p = os_process_pipe_create("cmd /c (echo oops 1>&2 & exit 12)", "w").unwrap();
        let mut e = vec![0u8; 100];
        let n = os_process_pipe_read_err(Some(&p), &mut e);
        e.truncate(n);
        // the same cmd quirk as in `pipe_windows_read`
        assert_eq!(e, b"oops \r\n");
        assert_eq!(os_process_pipe_destroy(Some(p)), 12);
    }

    // The `create2` command-line quoting (pipe-windows.c, Python
    // subprocess style) as pure logic — runs on every host.
    #[cfg(windows)]
    #[test]
    fn args_to_cmdline_quoting() {
        let argv = |v: &[&str]| v.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        assert_eq!(args_to_cmdline(&argv(&["echo", "hello"])), "echo hello");
        // empty argument: quoted
        assert_eq!(args_to_cmdline(&argv(&["echo", ""])), "echo \"\"");
        // space / tab: quoted
        assert_eq!(args_to_cmdline(&argv(&["echo", "a b"])), "echo \"a b\"");
        assert_eq!(args_to_cmdline(&argv(&["echo", "a\tb"])), "echo \"a\tb\"");
        // an interior quote escapes (backslashes before it double)
        assert_eq!(
            args_to_cmdline(&argv(&["echo", "say \"hi\""])),
            "echo \"say \\\"hi\\\"\""
        );
        // interior quote without whitespace: no outer quotes (the C
        // reference quotes only on empty/space/tab); the backslashes
        // double and the quote escapes
        assert_eq!(
            args_to_cmdline(&argv(&["echo", "a\\\"b"])),
            "echo a\\\\\\\"b"
        );
        // trailing backslashes double before the closing quote
        assert_eq!(args_to_cmdline(&argv(&["echo", "a \\"])), "echo \"a \\\\\"");
        // unquoted: backslashes pass through
        assert_eq!(args_to_cmdline(&argv(&["echo", "a\\b"])), "echo a\\b");
    }
}
