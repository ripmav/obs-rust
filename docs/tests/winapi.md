# winapi (WP-38 raw Win32 declarations) — test matrix

- Test file: `crates/obs-util/src/winapi.rs` (unit tests, module
  `winapi` — `#[cfg(windows)] pub(crate)`; the layout pins compile and
  run on the Windows CI job only)
- Purpose: WP-38 (P1-05.3 + finding 5) introduces the hand-declared
  Win32 surface (no `windows` crate — ADR 0005, the raw `extern "system"` house pattern). The mirrors of the kernel structs
  (`StartUpInfoW`, `ProcessInformation`, `SecurityAttributes`) must
  match the Win64 ABI byte-for-byte — a drifted layout would make the
  kernel read `dwFlags`/the child stdio handles from the wrong
  `StartUpInfoW` offsets in `os_process_pipe_create` (WP-38 CI round
  2: a 64-byte 9-field draft read past its own buffer and the runner
  aborted with 0xc0000005; the full 19-field form is 104 bytes).
- Reference: `wingdi.h`/`winbase.h`/`winnt.h` (Win64: `DWORD` = 4 B,
  `HANDLE`/pointers = 8 B).

| Case                                | Input                    | Expected                                                                                                                                                                                                                                                                         | Platforms | Status (CI) |
| ----------------------------------- | ------------------------ | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------- | ----------- |
| `StartUpInfoW` size + field offsets | `size_of` / `offset_of!` | 104 bytes (the full 19-field form incl. `lpDesktop` + the `dwX`/`dwY`/… console fields); `cb` @ 0, `lpReserved` @ 8, `lpDesktop` @ 16, `lpTitle` @ 24, `dwX` @ 32, `dwFlags` @ 60, `wShowWindow` @ 64, `lpReserved2` @ 72, `hStdInput` @ 80, `hStdOutput` @ 88, `hStdError` @ 96 | Windows   | pass (CI)   |
| `ProcessInformation` size           | `size_of`                | 24 bytes (`hProcess`, `hThread`, `dwProcessId`, `dwThreadId` — 8 + 8 + 4 + 4)                                                                                                                                                                                                    | Windows   | pass (CI)   |
| `SecurityAttributes` size           | `size_of`                | 24 bytes (`nLength` 4 + padding 4 + `lpSecurityDescriptor` 8 + `bInheritHandle` 4 + padding 4)                                                                                                                                                                                   | Windows   | pass (CI)   |

Verification: `cargo test -p obs-util --target x86_64-pc-windows-msvc`
(windows-latest CI job; the local aarch64 host has no Windows
toolchain — the offsets were cross-checked against the Win64 ABI and
the C reference's own `sizeof(STARTUPINFOW)` = 104, proven by the
WP-38 CI-round-2 bisection repro: the C 104-byte call succeeded where
the Rust 64-byte draft aborted). The extern
declarations themselves are exercised by the consumer tests
(threading / dl / pipe / os_proc — see their matrices).
