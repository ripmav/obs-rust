# dl (S-023 R107) — test matrix

- Test file: `crates/obs-util/src/dl.rs` (unit tests, module `dl`)
- Spec: `S-023` R107 (`os_dlopen` / `os_dlsym` / `os_dlclose`), oracle:
  `test/conformance/{scripts,vectors}/platform/35-dl-and-misc` (the
  runner-side parts; the vector pins the failure-log prefix up to the
  first `"):"` because the `dlerror()` text is platform-dependent).
- P1-11.1 (WP-29, review 2026-11-21): the failure log line
  `os_dlopen(<path>-><used name>): <dlerror>` was missing from the Rust
  port — it now goes through the injectable process-global error sink
  (`set_dlopen_error_sink`, default `eprintln!`), and the append rule
  (".so" appended only when the name has no ".so" substring) is pinned
  by the captured log prefix.

| Case                              | Input                                                          | Expected                                                                                                                                                                                                       | Platforms                                                           | Status |
| --------------------------------- | -------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------- | ------ |
| R107 real library                 | `libm.so.6`                                                    | loads; `os_dlsym("cos")` Some, unknown symbol None; close                                                                                                                                                      | Linux (glibc name; macOS `.dylib` would hit the append rule — skip) | pass   |
| R107 missing library              | `nosuchlibrary_xyz`, `nosuchlibrary_xyz.so`                    | both `None`, no panic                                                                                                                                                                                          | Linux, Windows, macOS (skip on Miri)                                | pass   |
| R107 failure log prefix (P1-11.1) | 3 failed opens: without ".so", with ".so", with ".so" mid-name | exactly 3 sink lines; each starts with the vector-pinned `os_dlopen(<path>-><used name>):` (append vs as-is), a dlerror fragment follows, C trailing newline; sink restored to the default (`None`) afterwards | Linux, Windows, macOS (skip on Miri)                                | pass   |

Verification (WP-29): `cargo test -p obs-util -- dl::` 3/3; the dl tests
serialise on `DL_TEST_LOCK` (the sink and `dlerror` state are
process/thread-global, house pattern W9).
