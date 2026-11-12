# process + module loading (S-055 U2, S-024 M3, S-023 R107/R82) — test matrix

WP-15 (P1-11), crate `crates/obs-util`, modules `process_args`
(`src/process_args.rs`), `dl` (`src/dl.rs`), `pipe` (`src/pipe.rs`),
`os_proc` (`src/os_proc.rs`). Specs: `S-055` §3 U2 (pipes), `S-024` §4.3
M3 (process args), `S-023` R107 (dynamic libs) + R82 (executable
path).

| Area | Pinned by test | Conformance vector |
| --- | --- | --- |
| **args (S-024 M3, R10)** | | |
| create + add_arg + argc/argv (NULL-terminated view, argc without it) | `args_basic`, `args_multiple` | `helpers/args-basic`, `helpers/args-multiple` |
| add_argf: `%d` `%i` `%s` `%x` `%X`, decimal width, zero pad, `%%`, literal-only | `args_argf` | `helpers/args-argf` |
| R16 defect fix: a format expanding to the empty string stores `""` (C stored NULL) | `args_argf` (empty entry) | `helpers/args-defect-argf-empty` |
| **dl (S-023 R107)** | | |
| real library loads, symbol found / missing symbol absent | `dl_real_library` | `platform/35-dl-and-misc` |
| missing library → `None` (with and without a `.so` suffix) | `dl_missing_library` | `platform/35-dl-and-misc` |
| the `.so` append rule (substring check), pinned through the failed `dlerror` text, which names the used file | `dl_appends_so_rule` | `platform/35-dl-and-misc` |
| **pipe (S-055 U2, R03-R09)** | | |
| read mode: stdout captured, stderr separate, EOF read returns 0 | `pipe_read` | `utilrest/pipe_read` |
| short reads: `fread` semantics (4/4/2 of 10 bytes) | `pipe_read` | `utilrest/pipe_read` |
| exit statuses as `(int)(signed char)status`: 0, 7, 200→-56, 255→-1, 128→-128 | `pipe_read` | `utilrest/pipe_read` |
| missing command: handle returned, destroy reports 127 (R06) | `pipe_read` | `utilrest/pipe_read` |
| `rw` is read mode; writing a read-mode handle returns 0 (R04, R08) | `pipe_read` | `utilrest/pipe_read` |
| a 200000-byte output is read completely | `pipe_read` | `utilrest/pipe_read` |
| D-1: a signalled child → the (positive) pid, not an exit status | `pipe_read` | `utilrest/pipe_read` |
| write mode: payload delivered to the child's stdin, exit 3 | `pipe_write` | `utilrest/pipe_write` |
| destroy without a write: the child sees 0 bytes | `pipe_write` | `utilrest/pipe_write` |
| 40-byte and 100000-byte writes delivered completely | `pipe_write` | `utilrest/pipe_write` |
| empty write → 0; `x` is write mode; child stderr readable | `pipe_write` | `utilrest/pipe_write` |
| create2: `argv[0]` direct, no shell, no PATH lookup (bare `echo` fails, `/nonexistent/binary` fails) | `pipe_create2` | `utilrest/pipe_null_and_args` |
| create2: metacharacters literal, arguments passed literally | `pipe_create2` | `utilrest/pipe_null_and_args` |
| create2: write mode with a custom shell executable | `pipe_create2` | `utilrest/pipe_null_and_args` |
| R06: missing executable → `None`, no leaked descriptor (pre-`access` check) | `pipe_create2` | `utilrest/pipe_null_and_args` |
| **P1-11.3 edge cases** | | |
| an executable path containing a space works | `pipe_edge_cases` | — |
| a write after the child exited (EPIPE) returns a count, no hang | `pipe_edge_cases` | — |
| reading a closed pipe (child exited): EOF, 0 | `pipe_edge_cases` | — |
| **os_proc (S-023-R82)** | | |
| `os_get_executable_path_ptr(None)` = the exe directory; `Some(name)` = dir + `"/"` + name; empty name ≡ None | `executable_path` | — |

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
- The 3-OS roundtrip of the P1-11 "Done when" (Linux only here).
- `os_get_executable_path_ptr` on the other platforms (S-023 R82 is
  machine dependent; the Linux `/proc/self/exe` path is pinned).
