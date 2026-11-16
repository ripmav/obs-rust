# obs-ffi logging shim (S-001 §4.2, R20–R25) — test matrix

WP-19 (P1-13.1), crate `crates/obs-ffi`: `c/base-logging.c` (clean-room
from the spec), `c/logging-test-helpers.c` (C `va_list` handler callbacks
for the scenario tests), `build.rs` (cc 1.11.2: both into one static
library), `src/lib.rs` (in-process unit tests), `tests/logging_scenarios.rs`
(child-process scenario tests, self re-exec — the `bmem_fatal` house
pattern, no separate binary to link). The variadic `blog`/`bcrash` and the
`va_list` handler callbacks stay in C (stable Rust cannot define or consume
variadics — primer §5.5); Rust logs through `blog(level, msg)` =
`obs_ffi_log` = `blog(level, "%s", msg)`. `base.c` stays C per the subtask;
the conformance vectors `test/conformance/scripts/bmem/` (`log`, `fatal`)
stay unticked (C checkout).

| Area | Pinned by test | Conformance vector |
| --- | --- | --- |
| **constants (R20)** | | |
| the Rust constants expose the spec values (100/200/300/400) | `r20_level_constants` | not scriptable (unit test required) |
| **handler dispatch (R21, R22)** | | |
| the C handler receives the **unformatted** format + args and formats them itself | `r21_handler_gets_unformatted` (child re-exec `fmt`) | `log` |
| install/get round-trip: param pointer round-trips; `None` reinstalls the default handler and the previous handler stops receiving | `r21_r22_handler_roundtrip` (in-process) | `log` |
| the Rust wrapper passes the message as the `"%s"` format | `blog_wrapper_formats` (in-process) | `log` |
| **default handler (R23, Q-3)** | | |
| per-level prefixes + streams (`error:` → stderr, the rest stdout), `debug: ` for LOG_DEBUG, unknown level silently dropped, `vsnprintf` truncation at 8192 bytes (9000-char message → 8191 chars), flush after write | `r23_default_handler_output` (child re-exec `default`) | `log` |
| **crash (R24)** | | |
| default crash handler: formatted message to stderr **without trailing newline**, exit 0 | `r24_default_crash` (child re-exec) | `fatal` |
| a crash handler that itself calls `bcrash`: `"Crashed in the crash handler"` to stderr, exit 2 | `r24_nested_crash` (child re-exec) | `fatal` |
| a returning crash handler: `bcrash` exits 0 | `r24_crash_handler_returns` (child re-exec) | `fatal` |
| **crash handler once-only (R25)** | | |
| the second install is ignored with the `LOG_WARNING` text (through the default handler) and the first handler stays active | `r25_crash_handler_once` (child re-exec `once`) | `fatal` |
