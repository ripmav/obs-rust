//! Fatal-path verification for `obs_util::bmem` (S-001 R02, R07, R24, R25).
//!
//! `bcrash` never returns, so each scenario runs in a subprocess: the test
//! binary re-executes itself with a `__BMEM_FATAL` directive; `WORKER`
//! guarantees exactly one thread performs the scenario (the process then
//! exits via the crash path). Parents assert exit code + exact streams.

use std::process::{Command, Stdio};
use std::sync::Once;

static WORKER: Once = Once::new();

fn worker_or_noop() {
    WORKER.call_once(|| match std::env::var("__BMEM_FATAL").ok().as_deref() {
        // R02 + R24 default handler: exact MUST text, no trailing newline, exit(0).
        Some("default") => {
            let _ = unsafe { obs_util::bmem::bmalloc_raw(0) };
            unreachable!("bmalloc_raw must not return (S-001-R02/R03)")
        }
        // R07: brealloc zero-size (incl. NULL ptr) fatal, exit(0) via default handler.
        Some("brealloc0") => {
            let _ = unsafe { obs_util::bmem::brealloc_raw(std::ptr::null_mut(), 0) };
            unreachable!("brealloc_raw must not return (S-001-R07)")
        }
        // R25 once-only + R24 handler-return exit(0): second install ignored,
        // first handler (param 42) serves the crash.
        Some("handler") => {
            obs_util::bmem::base_set_crash_handler(Some(handler_42), 42);
            obs_util::bmem::base_set_crash_handler(Some(handler_99), 99); // ignored (warning)
            let _ = unsafe { obs_util::bmem::bmalloc_raw(0) }; // -> handler_42 -> exit(0)
            unreachable!("bmalloc_raw must not return (S-001-R02/R03)")
        }
        // R24 re-entrancy: crash in the crash handler -> stderr note, exit(2).
        Some("reentrant") => {
            obs_util::bmem::base_set_crash_handler(Some(reentrant_handler), 0);
            let _ = unsafe { obs_util::bmem::bmalloc_raw(0) }; // -> reentrant_handler -> exit(2)
            unreachable!("bmalloc_raw must not return (S-001-R02/R03)")
        }
        _ => {}
    });
}

use std::io::Write;

fn handler_42(msg: &str, param: usize) {
    let _ = writeln!(std::io::stderr(), "HANDLER:{param}:{msg}");
    // Returning from the crash handler => exit(0) (R24).
}

fn handler_99(_msg: &str, _param: usize) {
    // Must never be used (R25 once-only).
}

fn reentrant_handler(msg: &str, _param: usize) {
    let _ = writeln!(std::io::stderr(), "HANDLER-START:{msg}");
    obs_util::bmem::bcrash("inner crash");
}

fn run(directive: &str) -> (i32, String, String) {
    let out = Command::new(std::env::current_exe().unwrap())
        .env("__BMEM_FATAL", directive)
        .stdout(Stdio::piped())
        .stderr(Stdio::piped())
        .output()
        .unwrap();
    (
        out.status.code().unwrap(),
        String::from_utf8(out.stdout).unwrap(),
        String::from_utf8(out.stderr).unwrap(),
    )
}

#[test]
fn bmalloc_zero_fatal_default_handler() {
    worker_or_noop();
    if std::env::var("__BMEM_FATAL").is_ok() {
        return; // worker process
    }
    // S-001-R02/R24: exit(0), stderr = MUST text without trailing newline.
    let (code, _out, err) = run("default");
    assert_eq!(code, 0);
    assert_eq!(
        err,
        "bmalloc: Allocating 0 bytes is broken behavior, please fix your code!"
    );
}

#[test]
fn brealloc_zero_fatal() {
    worker_or_noop();
    if std::env::var("__BMEM_FATAL").is_ok() {
        return;
    }
    // S-001-R07: brealloc message text.
    let (code, _out, err) = run("brealloc0");
    assert_eq!(code, 0);
    assert_eq!(
        err,
        "brealloc: Allocating 0 bytes is broken behavior, please fix your code!"
    );
}

#[test]
fn crash_handler_once_only_and_serves_crash() {
    worker_or_noop();
    if std::env::var("__BMEM_FATAL").is_ok() {
        return;
    }
    // S-001-R25 warning (stdout, default LOG_WARNING format) + first handler used.
    let (code, out, err) = run("handler");
    assert_eq!(code, 0);
    assert!(
        out.contains("warning: Tried to set a crash handler when one already exists."),
        "stdout: {out:?}"
    );
    assert_eq!(
        err,
        "HANDLER:42:bmalloc: Allocating 0 bytes is broken behavior, please fix your code!\n"
    );
    assert!(!err.contains("HANDLER:99"));
}

#[test]
fn reentrant_crash_exits_2() {
    worker_or_noop();
    if std::env::var("__BMEM_FATAL").is_ok() {
        return;
    }
    // S-001-R24: crash in the crash handler -> note on stderr, exit(2).
    let (code, _out, err) = run("reentrant");
    assert_eq!(code, 2);
    assert!(
        err.contains("Crashed in the crash handler"),
        "stderr: {err:?}"
    );
    assert!(err.contains("HANDLER-START:"), "stderr: {err:?}");
}
