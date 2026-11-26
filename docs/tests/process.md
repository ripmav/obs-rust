# process + module loading (S-055 U2, S-024 M3, S-023 R107/R82) — test matrix

WP-15 (P1-11), WP-38 (finding 5 — the Windows variants of `dl` /
`pipe` / `os_proc`: `LoadLibraryExW` per `platform-windows.c`,
`CreateProcessW` pipes per `pipe-windows.c`, `GetModuleFileNameW`),
crate `crates/obs-util`, modules `process_args`
(`src/process_args.rs`), `dl` (`src/dl.rs`), `pipe` (`src/pipe.rs`),
`os_proc` (`src/os_proc.rs`). Specs: `S-055` §3 U2 (pipes), `S-024` §4.3
M3 (process args), `S-023` R107 (dynamic libs) + R82 (executable
path). The unix pipe rows are `#[cfg(unix)]` tests; the Windows rows
(WP-38) run on the windows-latest CI job (no local Windows toolchain
on the aarch64 host) and drive `cmd` — the Windows shell.

| Area                                                                                                                                                                                                       | Pinned by test                            | Conformance vector                            |
| ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ----------------------------------------- | --------------------------------------------- |
| **args (S-024 M3, R10)**                                                                                                                                                                                   |                                           |                                               |
| create + add_arg + argc/argv (NULL-terminated view, argc without it)                                                                                                                                       | `args_basic`, `args_multiple`             | `helpers/args-basic`, `helpers/args-multiple` |
| add_argf: `%d` `%i` `%s` `%x` `%X`, decimal width, zero pad, `%%`, literal-only                                                                                                                            | `args_argf`                               | `helpers/args-argf`                           |
| R16 defect fix: a format expanding to the empty string stores `""` (C stored NULL)                                                                                                                         | `args_argf` (empty entry)                 | `helpers/args-defect-argf-empty`              |
| **dl (S-023 R107)**                                                                                                                                                                                        |                                           |                                               |
| real library loads, symbol found / missing symbol absent (Linux-only: glibc name `libm.so.6`; macOS libm is unreachable through the R107 append rule — revisit with the C checkout)                        | `dl_real_library`                         | `platform/35-dl-and-misc`                     |
| missing library → `None` (with and without a `.so` suffix)                                                                                                                                                 | `dl_missing_library`                      | `platform/35-dl-and-misc`                     |
| the `.so` append rule (substring check), pinned through the failed `dlerror` text, which names the used file (asserts name presence — the dlerror format is platform-dependent)                            | `dl_r107_error_line_prefix`               | `platform/35-dl-and-misc`                     |
| **pipe (S-055 U2, R03-R09)**                                                                                                                                                                               |                                           |                                               |
| read mode: stdout captured, stderr separate, EOF read returns 0                                                                                                                                            | `pipe_read` (unix)                        | `utilrest/pipe_read`                          |
| short reads: `fread` semantics (4/4/2 of 10 bytes)                                                                                                                                                         | `pipe_read`                               | `utilrest/pipe_read`                          |
| exit statuses as `(int)(signed char)status`: 0, 7, 200→-56, 255→-1, 128→-128                                                                                                                               | `pipe_read`                               | `utilrest/pipe_read`                          |
| missing command: handle returned, destroy reports 127 (R06)                                                                                                                                                | `pipe_read`                               | `utilrest/pipe_read`                          |
| `rw` is read mode; writing a read-mode handle returns 0 (R04, R08)                                                                                                                                         | `pipe_read`                               | `utilrest/pipe_read`                          |
| a 200000-byte output is read completely                                                                                                                                                                    | `pipe_read`                               | `utilrest/pipe_read`                          |
| D-1: a signalled child → the (positive) pid, not an exit status                                                                                                                                            | `pipe_read`                               | `utilrest/pipe_read`                          |
| write mode: payload delivered to the child's stdin, exit 3                                                                                                                                                 | `pipe_write` (unix)                       | `utilrest/pipe_write`                         |
| destroy without a write: the child sees 0 bytes                                                                                                                                                            | `pipe_write`                              | `utilrest/pipe_write`                         |
| 40-byte and 100000-byte writes delivered completely                                                                                                                                                        | `pipe_write`                              | `utilrest/pipe_write`                         |
| empty write → 0; `x` is write mode; child stderr readable                                                                                                                                                  | `pipe_write`                              | `utilrest/pipe_write`                         |
| create2: `argv[0]` direct, no shell, no PATH lookup (bare `echo` fails, `/nonexistent/binary` fails)                                                                                                       | `pipe_create2` (unix)                     | `utilrest/pipe_null_and_args`                 |
| create2: metacharacters literal, arguments passed literally                                                                                                                                                | `pipe_create2`                            | `utilrest/pipe_null_and_args`                 |
| create2: write mode with a custom shell executable                                                                                                                                                         | `pipe_create2`                            | `utilrest/pipe_null_and_args`                 |
| R06: missing executable → `None`, no leaked descriptor (pre-`access` check)                                                                                                                                | `pipe_create2`                            | `utilrest/pipe_null_and_args`                 |
| **pipe (Windows, WP-38 — `pipe-windows.c`)**                                                                                                                                                               |                                           |                                               |
| read mode: `cmd /c echo hello` stdout captured (CRLF), stderr separate; raw exit codes 7 → 7 and 200 → 200 (no posix signed-char conversion); writing a read-mode handle returns 0                         | `pipe_windows_read` (Windows)             | — (C-reference behaviour)                     |
| `type` accepts only `r`/`w` (the C reference — anything else `None`); `create2` command-line quoting round-trips (`cmd /c "echo spaced"` → `spaced`); a missing executable → `None`                        | `pipe_windows_type_and_create2` (Windows) | — (C-reference behaviour)                     |
| write mode: the payload reaches the child's stdin (`findstr /v ""` consumes it, exit 0); destroy without a write → 0 bytes seen (exit 1); the child's stderr is readable; raw exit code 12 → 12            | `pipe_windows_write` (Windows)            | — (C-reference behaviour)                     |
| `args_to_cmdline` quoting (the C loop as pure logic): plain, empty, space/tab-quoted, interior-quote escaping (preceding backslashes double), trailing-backslash doubling, unquoted backslash pass-through | `args_to_cmdline_quoting` (Windows)       | — (pure logic)                                |
| **P1-11.3 edge cases**                                                                                                                                                                                     |                                           |                                               |
| an executable path containing a space works                                                                                                                                                                | `pipe_edge_cases` (unix)                  | —                                             |
| a write after the child exited (EPIPE) returns a count, no hang                                                                                                                                            | `pipe_edge_cases`                         | —                                             |
| reading a closed pipe (child exited): EOF, 0                                                                                                                                                               | `pipe_edge_cases`                         | —                                             |
| **os_proc (S-023-R82)**                                                                                                                                                                                    |                                           |                                               |
| `os_get_executable_path_ptr(None)` = the exe directory; `Some(name)` = dir + `"/"` + name; empty name ≡ None                                                                                               | `executable_path` (linux)                 | —                                             |
| Windows variant (`GetModuleFileNameW`, `\` → `/`): the exe directory (forward slashes); `Some(name)` = dir + "/" + name; empty name ≡ None                                                                 | `executable_path_windows` (Windows)       | —                                             |

## Documented deviations (see the module docs)

- **Q-1 (write buffering)**: C buffers `write` through stdio and
  delivers the data at the latest on `destroy`; the port writes
  directly. The conformance vectors observe the same exit statuses.
- **C-NULL arguments**: `create(NULL, …)`, `create2(NULL, …)`, NULL
  handles, a NULL `executable`/`arg` (Q-3 stores NULL elements) and
  non-UTF-8 argument bytes are unrepresentable in the safe API; the
  FFI layer maps them to the C behaviour (`None` / `0` / `""`).
- **spawn mechanism**: fork + dup2 + execv instead of the reference's
  posix_spawn file actions (this glibc's `adddup2` argument order made
  file actions unusable; observable behaviour R03-R09 identical).
- **R107 error log**: `os_dlopen` failure does not emit the C
  `LOG_ERROR` yet (no logger in the crate; the FFI layer wires it) —
  the used file name is pinned through `dlerror` instead.
- **R82**: machine dependent — the tests pin the structure (parent
  directory of the exe, `dir + "/" + name`), not the absolute path.

## Not covered yet (C-checkout dependent / later phases)

- P1-11.4 apply recipe: the `extern "C"` shims in `obs-ffi`, cbindgen
  header diff, `OBS_ENABLE_RUST=ON` build (needs `../obs-studio`).
- The macOS `os_get_executable_path_ptr` variant (`_NSGetExecutablePath`
  in the C nix reference — S-023 R82 is machine dependent): the Rust
  function returns `None` on macOS/FreeBSD (documented in the module
  docs — a pre-existing gap, not WP-38 scope). The P1-11 "3-OS"
  Done-when (echo-process roundtrip) runs on all three CI OS jobs since
  WP-38 (unix path on Linux/macOS, `CreateProcessW` on Windows).
