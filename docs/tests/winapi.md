# winapi (WP-38 raw Win32 declarations) — test matrix

- Test file: `crates/obs-util/src/winapi.rs` (unit tests, module
  `winapi` — `#[cfg(windows)] pub(crate)`; the layout pins compile and
  run on the Windows CI job only)
- Purpose: WP-38 (P1-05.3 + finding 5) introduces the hand-declared
  Win32 surface (no `windows` crate — ADR 0005, the raw `extern "system"` house pattern). The mirrors of the kernel structs
  (`StartUpInfoW`, `ProcessInformation`, `SecurityAttributes`) must
  match the Win64 ABI byte-for-byte — a drifted layout would write the
  child stdio handles into the wrong `StartUpInfoW` fields in
  `os_process_pipe_create`.
- Reference: `winbase.h`/`winnt.h` (Win64: `DWORD` = 4 B, `HANDLE`/
  pointers = 8 B).

| Case                                | Input                    | Expected                                                                                                            | Platforms | Status (CI) |
| ----------------------------------- | ------------------------ | ------------------------------------------------------------------------------------------------------------------- | --------- | ----------- |
| `StartUpInfoW` size + field offsets | `size_of` / `offset_of!` | 64 bytes (the modern 9-field form); `cb` @ 0, `dwFlags` @ 24, `hStdInput` @ 40, `hStdOutput` @ 48, `hStdError` @ 56 | Windows   | pass (CI)   |
| `ProcessInformation` size           | `size_of`                | 24 bytes (`hProcess`, `hThread`, `dwProcessId`, `dwThreadId` — 8 + 8 + 4 + 4)                                       | Windows   | pass (CI)   |
| `SecurityAttributes` size           | `size_of`                | 24 bytes (`nLength` 4 + padding 4 + `lpSecurityDescriptor` 8 + `bInheritHandle` 4 + padding 4)                      | Windows   | pass (CI)   |

Verification: `cargo test -p obs-util --target x86_64-pc-windows-msvc`
(windows-latest CI job; the local aarch64 host has no Windows
toolchain — the offsets were cross-checked against the MSVC Win64
ABI before delivery; a first draft had the legacy pre-Win95 console
fields (`dwX`/`dwY`/… , 104 B) and was corrected). The extern
declarations themselves are exercised by the consumer tests
(threading / dl / pipe / os_proc — see their matrices).
