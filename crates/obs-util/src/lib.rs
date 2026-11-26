//! obs-util: the `libobs/util` leaf ports (Phase 1) — bmem, dstr, darray,
//! deque, lexer/cf-lexer/cf-parser/cf-preprocessor, config, hash (blake2/
//! crc32), dl, os_fs, os_proc/process_args, os_str, os_time, pipe,
//! text_lookup, threading, utf8. One module per C source group, spec-driven
//! (S-001..S-011, S-023, S-024, S-055); the committed conformance vectors
//! are the oracle. `dl`, `pipe`, `os_proc`, `threading` are cross-platform
//! (P1-05.3 + finding 5 Windows parity delivered, WP-38); the remaining
//! nix-only platform helpers (`os_dir`/`os_inhibit`/`os_paths`/`os_time`)
//! stay unix until their P1-06/P1-07.2/.3 Windows subtasks.

pub mod bitstream;
pub mod bmem;
pub mod cf_lexer;
pub mod cf_parser;
pub mod cf_preprocessor;
pub mod config;
pub mod darray;
pub mod deque;
// Cross-platform dynamic libs (S-023 R107): `dlopen` family on unix,
// `LoadLibraryExW`/`GetProcAddress`/`FreeLibrary` on Windows.
pub mod dl;
pub mod dstr;
pub mod hash;
pub mod lexer;
pub mod os_fs;
// nix-only platform helpers (the Windows port of P1-07.2/.3 is a separate
// subtask; the C reference for this work package is `platform-nix.c`):
// `opendir`/`readdir`/`glob`, `dlopen`-based GLib D-Bus/portal inhibition,
// and the nix path helpers.
#[cfg(unix)]
pub mod os_dir;
#[cfg(unix)]
pub mod os_inhibit;
#[cfg(unix)]
pub mod os_paths;
// Cross-platform executable path (S-023 R82): `/proc/self/exe` on Linux,
// `GetModuleFileNameW` on Windows.
pub mod os_proc;
pub mod os_str;
#[cfg(unix)]
pub mod os_time;
// Cross-platform child-process pipes (S-055 U2): fork/exec + `pipe`/`waitpid`
// on unix, `CreateProcessW` + `CreatePipe`/`ReadFile`/`WriteFile` on Windows.
pub mod pipe;
pub mod process_args;
pub mod serializer;
pub mod text_lookup;
// Cross-platform threading primitives (S-061): `pthread`/`sem_*` on unix,
// `CreateEvent`/`CreateSemaphore`/`WaitForSingleObject` on Windows.
pub mod threading;
pub mod utf8;
// Windows WinAPI surface for the platform-tier modules above (WP-38).
#[cfg(windows)]
pub(crate) mod winapi;

#[cfg(test)]
mod tests {
    /// Skeleton smoke test (P0-07.3); replaced by real tests in the port.
    #[test]
    fn skeleton_smoke() {}
}
