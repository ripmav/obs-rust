//! obs-util: the `libobs/util` leaf ports (Phase 1) — bmem, dstr, darray,
//! deque, lexer/cf-lexer/cf-parser/cf-preprocessor, config, hash (blake2/
//! crc32), dl, os_fs, os_proc/process_args, os_str, os_time, pipe,
//! text_lookup, threading, utf8. One module per C source group, spec-driven
//! (S-001..S-011, S-023, S-024, S-055); the committed conformance vectors
//! are the oracle. `dl`, `pipe`, `os_proc`, `threading` are POSIX-only so
//! far (P1-05.3/P1-07/P1-11 Windows parity outstanding, review 2026-11-21).

pub mod bmem;
pub mod cf_lexer;
pub mod cf_parser;
pub mod cf_preprocessor;
pub mod config;
pub mod darray;
pub mod deque;
// POSIX-only modules: their libc surface (dlopen/readlink/pipe/fork/...) does
// not exist in the Windows build of the `libc` crate; the Windows variants
// arrive with the platform crate (S-023) when the C reference is in place.
#[cfg(unix)]
pub mod dl;
pub mod dstr;
pub mod hash;
pub mod lexer;
pub mod os_fs;
#[cfg(unix)]
pub mod os_proc;
pub mod os_str;
#[cfg(unix)]
pub mod os_time;
#[cfg(unix)]
pub mod pipe;
pub mod process_args;
pub mod text_lookup;
#[cfg(unix)]
#[cfg(unix)]
pub mod threading;
pub mod utf8;

#[cfg(test)]
mod tests {
    /// Skeleton smoke test (P0-07.3); replaced by real tests in the port.
    #[test]
    fn skeleton_smoke() {}
}
