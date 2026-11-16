//! S-001 §4.2 (R21-R25) scenario tests for the C logging surface.
//!
//! Each scenario runs in a subprocess: the test binary re-executes itself
//! with an `__OBS_FFI_LOG` directive (the `bmem_fatal` house pattern), the
//! worker runs the scenario through the C variadic `blog`/`bcrash` and
//! exits; the parent asserts byte-exact stdout/stderr/exit code. The C
//! handler callbacks live in `c/logging-test-helpers.c` (a `va_list`
//! handler cannot be defined in Rust — primer §5.5) and are linked from
//! the crate's static library.

use std::ffi::c_char;
use std::os::raw::c_void;
use std::process::{Command, Stdio};
use std::sync::Once;

use obs_ffi::{
    LOG_DEBUG, LOG_ERROR, LOG_INFO, LOG_WARNING, VaList, base_set_crash_handler,
    base_set_log_handler,
};

mod ffi_c {
    use super::VaList;
    use std::ffi::c_char;
    use std::os::raw::{c_int, c_void};

    #[allow(dead_code)]
    unsafe extern "C" {
        pub fn blog(level: c_int, fmt: *const c_char, ...);
        pub fn bcrash(fmt: *const c_char, ...);
        pub fn log_test_cap(lvl: c_int, fmt: *const c_char, args: VaList, p: *mut c_void);
        pub fn log_test_crash_a(fmt: *const c_char, args: VaList, p: *mut c_void);
        pub fn log_test_crash_b(fmt: *const c_char, args: VaList, p: *mut c_void);
    }
}

const MARKER: &str = "OBS_FFI_SCENARIO_OUTPUT_START\n";

static WORKER: Once = Once::new();

/// Worker process: exactly one thread runs the scenario (the child re-execs
/// the whole test binary), then the process exits via the scenario. The
/// marker line (flushed before any C output) lets the parent cut off
/// libtest's own progress lines from the child's stdout.
fn worker_or_noop() {
    WORKER.call_once(|| match std::env::var("__OBS_FFI_LOG").ok().as_deref() {
        // R23: per-level prefixes/streams, Q-3 drop, truncation at 8192
        Some("default") => {
            scenario_marker();
            unsafe {
                ffi_c::blog(LOG_ERROR, c"e1".as_ptr());
                ffi_c::blog(LOG_WARNING, c"w1".as_ptr());
                ffi_c::blog(LOG_INFO, c"i1 %d".as_ptr(), 7i32);
                ffi_c::blog(LOG_DEBUG, c"d1".as_ptr());
                ffi_c::blog(999, c"dropped".as_ptr());
                ffi_c::blog(LOG_INFO, c"%*s".as_ptr(), 3i32, c"padded".as_ptr());
                let mut big = vec![b'x'; 8999];
                big.push(0);
                ffi_c::blog(LOG_INFO, c"%s".as_ptr(), big.as_ptr().cast::<c_char>());
            }
            std::process::exit(0);
        }
        // R21: the handler receives the unformatted format + param (R22)
        Some("fmt") => {
            scenario_marker();
            unsafe {
                base_set_log_handler(Some(ffi_c::log_test_cap), 0x1234usize as *mut c_void);
                ffi_c::blog(400, c"a=%d b=%s".as_ptr(), 7i32, c"x".as_ptr());
            }
            std::process::exit(0);
        }
        // R24: default crash handler — formatted, no trailing newline, exit 0
        Some("crash-default") => {
            scenario_marker();
            unsafe {
                ffi_c::bcrash(c"boom %d".as_ptr(), 42i32);
            }
            std::process::exit(3); // unreachable (the C path exits first)
        }
        // R24: crash inside the crash handler -> exit 2
        Some("crash-nested") => {
            scenario_marker();
            unsafe {
                base_set_crash_handler(ffi_c::log_test_crash_a, std::ptr::null_mut());
                ffi_c::bcrash(c"outer".as_ptr());
            }
            std::process::exit(3); // unreachable
        }
        // R24: a returning crash handler -> exit 0
        Some("crash-return") => {
            scenario_marker();
            unsafe {
                base_set_crash_handler(ffi_c::log_test_crash_b, std::ptr::null_mut());
                ffi_c::bcrash(c"ret %s".as_ptr(), c"v".as_ptr());
            }
            std::process::exit(3); // unreachable
        }
        // R25: the second install is ignored (warning) and the first stays
        Some("once") => {
            scenario_marker();
            unsafe {
                base_set_crash_handler(ffi_c::log_test_crash_b, std::ptr::null_mut());
                base_set_crash_handler(ffi_c::log_test_crash_a, std::ptr::null_mut()); // ignored
                ffi_c::bcrash(c"who".as_ptr());
            }
            std::process::exit(3); // unreachable
        }
        _ => {}
    });
}

fn scenario_marker() {
    let mut so = std::io::stdout().lock();
    let _ = std::io::Write::write_all(&mut so, MARKER.as_bytes());
    let _ = std::io::Write::flush(&mut so);
}

fn run(directive: &str) -> (i32, String, String) {
    let out = Command::new(std::env::current_exe().unwrap())
        .env("__OBS_FFI_LOG", directive)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .output()
        .unwrap();
    let stdout = String::from_utf8_lossy(&out.stdout).into_owned();
    let idx = stdout
        .find(MARKER)
        .expect("scenario marker in child stdout")
        + MARKER.len();
    (
        out.status.code().unwrap(),
        stdout[idx..].to_string(),
        String::from_utf8_lossy(&out.stderr).into_owned(),
    )
}

fn guard_worker() -> bool {
    worker_or_noop();
    std::env::var("__OBS_FFI_LOG").is_ok()
}

// R23: default handler output — prefixes, streams, Q-3 drop, truncation
#[test]
fn r23_default_handler_output() {
    if guard_worker() {
        return;
    }
    let (code, out, err) = run("default");
    assert_eq!(code, 0);
    assert_eq!(err, "error: e1\n");
    assert_eq!(
        out,
        format!(
            "warning: w1\ninfo: i1 7\ndebug: d1\ninfo: padded\ninfo: {}\n",
            "x".repeat(8191)
        )
    );
}

// R21: the C handler receives the unformatted format + args
#[test]
fn r21_handler_gets_unformatted() {
    if guard_worker() {
        return;
    }
    let (code, out, err) = run("fmt");
    assert_eq!(code, 0);
    assert_eq!(err, "");
    assert_eq!(out, "lvl=400 fmt=a=%d b=%s out=a=7 b=x\n");
}

// R24: default crash handler — formatted message, no trailing newline,
// exit 0
#[test]
fn r24_default_crash() {
    if guard_worker() {
        return;
    }
    let (code, out, err) = run("crash-default");
    assert_eq!(code, 0);
    assert_eq!(out, "");
    assert_eq!(err, "boom 42");
}

// R24: a crash handler that itself calls bcrash -> "Crashed in the
// crash handler", exit 2
#[test]
fn r24_nested_crash() {
    if guard_worker() {
        return;
    }
    let (code, out, err) = run("crash-nested");
    assert_eq!(code, 2);
    assert_eq!(out, "");
    assert_eq!(err, "A outerCrashed in the crash handler");
}

// R24: a returning crash handler -> exit 0
#[test]
fn r24_crash_handler_returns() {
    if guard_worker() {
        return;
    }
    let (code, out, err) = run("crash-return");
    assert_eq!(code, 0);
    assert_eq!(out, "");
    assert_eq!(err, "B ret v");
}

// R25: the crash handler can be set at most once; the second call is
// ignored with the warning, the first handler stays active
#[test]
fn r25_crash_handler_once() {
    if guard_worker() {
        return;
    }
    let (code, out, err) = run("once");
    assert_eq!(code, 0);
    assert_eq!(
        out,
        "warning: Tried to set a crash handler when one already exists.\n"
    );
    assert_eq!(err, "B who");
}
