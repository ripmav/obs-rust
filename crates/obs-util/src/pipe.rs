//! S-055 U2: child-process pipes (`os_process_pipe_*`), POSIX.
//!
//! `os_process_pipe_create(cmd, type)` runs `/bin/sh -c cmd`;
//! `os_process_pipe_create2(args, type)` runs `argv[0]` of the argument
//! list directly (no shell, no PATH lookup). Read mode (first character
//! of `type` is `r`) connects the child's stdout to the handle; write
//! mode connects the child's stdin; the child's stderr is connected in
//! both modes (`read_err`).
//!
//! Deviations from the C reference:
//! - The spawn mechanism is fork + dup2 + execv (the classic `popen`
//!   shape) rather than the reference's posix_spawn file actions; the
//!   observable behaviour is the same (R03-R09, R06 failure -> `None`
//!   with no leaked descriptor).
//! - Q-1 (stdio buffering of `write`): the port writes directly (no
//!   buffering). Delivery to the child is at least as early; the
//!   conformance vectors observe the same results (exit statuses).
//! - C-NULL arguments (`create(NULL, …)`, `create2(NULL, …)`, NULL
//!   handles for the I/O functions and `destroy`) are unrepresentable
//!   in the safe API; the FFI layer maps them to the C behaviour
//!   (`None` / 0).
//! - D-1 (signalled child): `destroy` returns the child's pid
//!   (positive), as the reference does (its `waitpid` result).
//! - Logging: none yet (no logger in the crate; the FFI layer wires
//!   it).

use std::os::raw::c_void;

use crate::process_args::ProcessArgs;

/// A child process with a pipe to its stdout (read mode) or stdin
/// (write mode) and a pipe to its stderr (both modes).
pub struct ProcessPipe {
    pid: libc::pid_t,
    /// Read mode: the parent's end of the stdout pipe.
    read_fd: Option<libc::c_int>,
    /// Write mode: the parent's end of the stdin pipe.
    write_fd: Option<libc::c_int>,
    /// The parent's end of the stderr pipe (both modes).
    err_fd: libc::c_int,
}

fn set_cloexec(fd: libc::c_int) {
    // SAFETY: `fd` is a valid descriptor owned by this thread.
    let flags = unsafe { libc::fcntl(fd, libc::F_GETFD) };
    // SAFETY: as above; F_SETFD with a valid flag word.
    let _ = unsafe { libc::fcntl(fd, libc::F_SETFD, flags | libc::FD_CLOEXEC) };
}

fn close_fd(fd: libc::c_int) {
    // SAFETY: `fd` is a valid descriptor owned by the caller.
    let _ = unsafe { libc::close(fd) };
}

/// Read mode iff the first character of `type` is `r` (S-055-R04).
fn is_read_mode(type_: &str) -> bool {
    type_.starts_with('r')
}

/// Spawn the child (fork + dup2 + execv — the classic `popen` shape);
/// returns `(pid, read_fd, write_fd, err_fd)`.
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
            unsafe { libc::dup2(pout[1], 1) }
        } else {
            unsafe { libc::dup2(pin[0], 0) }
        };
        let _ = unsafe { libc::dup2(perr[1], 2) };
        for fd in [pout[0], pout[1], pin[0], pin[1], perr[0], perr[1]] {
            unsafe { libc::close(fd) };
        }
        // SAFETY: NUL-terminated argv built before the fork.
        unsafe { libc::execv(exe_c.as_ptr(), argv_p.as_ptr()) };
        // exec failed (the executable vanished between access and
        // exec): report 127 like a missing command
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

/// `os_process_pipe_create2(args, type)` (R03): runs `argv[0]` of the
/// list directly — no shell, no PATH lookup (a bare name such as
/// `echo` fails, R06).
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

/// `os_process_pipe_read` (R07): `fread` semantics — blocks until `len`
/// bytes or EOF, returns the byte count (0 at EOF). A write-mode handle
/// returns 0 without touching any stream; a NULL handle returns 0.
pub fn os_process_pipe_read(pp: Option<&ProcessPipe>, buf: &mut [u8]) -> usize {
    let Some(pp) = pp else {
        return 0;
    };
    let Some(fd) = pp.read_fd else {
        return 0;
    };
    read_until(fd, buf)
}

/// `os_process_pipe_read_err` (R07): same semantics on the stderr pipe
/// (works in both modes); a NULL handle returns 0.
pub fn os_process_pipe_read_err(pp: Option<&ProcessPipe>, buf: &mut [u8]) -> usize {
    let Some(pp) = pp else {
        return 0;
    };
    read_until(pp.err_fd, buf)
}

/// `os_process_pipe_write` (R08): loops `write` until all bytes are out
/// or a write returns 0; returns the byte count (partial on error). A
/// read-mode handle returns 0; an empty write returns 0; a NULL handle
/// returns 0.
pub fn os_process_pipe_write(pp: Option<&ProcessPipe>, data: &[u8]) -> usize {
    let Some(pp) = pp else {
        return 0;
    };
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

/// `os_process_pipe_destroy` (R09): closes the main stream (the child
/// sees EOF), closes stderr, waits for the child (retrying EINTR) and
/// returns the exit status as `(int)(signed char)status` — 7 -> 7,
/// 200 -> -56, 255 -> -1, 128 -> -128. A signalled child returns the
/// child's pid (D-1). `destroy(NULL)` returns 0. Takes ownership of the
/// handle.
pub fn os_process_pipe_destroy(pp: Option<ProcessPipe>) -> i32 {
    let Some(pp) = pp else {
        return 0;
    };
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

    fn read_all(pp: &ProcessPipe, len: usize) -> Vec<u8> {
        let mut buf = vec![0u8; len];
        let n = os_process_pipe_read(Some(pp), &mut buf);
        buf.truncate(n);
        buf
    }

    // vector pipe_read: read mode end to end
    #[test]
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

        // a large output is read completely
        let p =
            os_process_pipe_create("head -c 200000 /dev/zero | tr '\\0' 'x' | wc -c", "r").unwrap();
        let out = read_all(&p, 100);
        assert_eq!(out, b"200000\n");
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

        // D-1: a signalled child returns the (positive) pid
        let p = os_process_pipe_create("kill -KILL $$", "r").unwrap();
        assert_eq!(os_process_pipe_read(Some(&p), &mut [0; 10]), 0);
        let r = os_process_pipe_destroy(Some(p));
        assert!(r > 0, "signalled child: the pid, got {r}");
    }

    // vector pipe_write: write mode end to end
    #[test]
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
        // format)
        let mut args = ProcessArgs::create("/bin/printf");
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
    #[cfg_attr(miri, ignore)]
    fn pipe_edge_cases() {
        // an executable path containing a space
        let dir = std::env::temp_dir().join("pipe-space-test");
        std::fs::create_dir_all(&dir).unwrap();
        let exe = dir.join("echo sp.bin");
        std::fs::copy("/bin/echo", &exe).unwrap();
        let mut args = ProcessArgs::create(exe.to_string_lossy().as_ref());
        args.add_arg("spaced");
        let p = os_process_pipe_create2(&args, "r").unwrap();
        let out = read_all(&p, 100);
        assert_eq!(out, b"spaced\n");
        assert_eq!(os_process_pipe_destroy(Some(p)), 0);

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
}
