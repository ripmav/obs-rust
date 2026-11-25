# inject portable core (P1-33.4, no spec) — test matrix

Test files: `crates/obs-inject/src/inject.rs`
Issue: WP-34 (P1-33.4)

Portable core of `shared/obs-inject-library` in `crates/obs-inject`. The
reference is Windows-only (`LoadLibraryW`/`CreateRemoteThread`/
`SetWindowsHookEx`) and **stays linked** until `plugins/win-capture`
(P5-31.6/P5-31.7) is ported — no CMake exclusion. What is ported and
unit-tested: the half-byte XOR de-obfuscation of the API names
(`obfuscate.c`), the symbol resolution, and the injection control flow
(error-code mapping + cleanup ordering) with the Windows calls taken as
injected callables (`ObfApis` / `SafeApis`), so every failure branch is
testable cross-platform. The real WinAPI bindings land with P5-31.6/7.
Documented deviation: the reference dereferences resolved pointers
without NULL checks (faults on a missing symbol); the port returns
`INJECT_ERROR_UNLIKELY_FAIL` cleanly.

| Area                                                                                                                                                        | Pinned by test                                                                    | Conformance vector    |
| ----------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------- | --------------------- |
| **de-obfuscation (`deobfuscate_str`)**                                                                                                                      |                                                                                   |                       |
| the 12 real production names from `game-capture.c` / `inject-helper.c` resolve to the correct API strings; a wrong key does not; encrypt/decrypt round-trip | `obfuscated_names_resolve`, `wrong_key_does_not_resolve`, `deobfuscate_roundtrip` | — (Windows-only flow) |
| **`inject_library_obf` control flow**                                                                                                                       |                                                                                   |                       |
| success path (remote thread exit code non-zero → resolved module returned)                                                                                  | `success_path`                                                                    | —                     |
| allocation failure → `INJECT_ERROR_UNLIKELY_FAIL`, no free                                                                                                  | `alloc_failure`                                                                   | —                     |
| `WriteProcessMemory` failure frees the allocated memory                                                                                                     | `write_failure_frees_memory`                                                      | —                     |
| thread creation failure frees the memory                                                                                                                    | `thread_creation_failure_frees_memory`                                            | —                     |
| `WaitForSingleObject` timeout closes the handle and frees                                                                                                   | `wait_timeout_closes_and_frees`                                                   | —                     |
| remote exit code 0 → `INJECT_ERROR_INJECT_FAILED` (the reference's exact mapping)                                                                           | `exit_code_zero_is_inject_failed`                                                 | —                     |
| resolved pointer missing → clean `INJECT_ERROR_UNLIKELY_FAIL` (the documented deviation from the reference's fault)                                         | `missing_symbol_fails_cleanly`                                                    | —                     |
| **`inject_library_safe_obf` (hook variant)**                                                                                                                |                                                                                   |                       |
| success posts `RETRY_COUNT` retries at `RETRY_INTERVAL_MS` (4000/500)                                                                                       | `safe_success_posts_retries`                                                      | —                     |
| `LoadLibraryW` failure in the hook worker                                                                                                                   | `safe_load_library_failure`                                                       | —                     |
| user32 module missing                                                                                                                                       | `safe_user32_missing`                                                             | —                     |
| proc module missing                                                                                                                                         | `safe_proc_missing`                                                               | —                     |
| hook symbol missing in either module                                                                                                                        | `safe_hook_symbol_missing`                                                        | —                     |
| `SetWindowsHookEx` failure → last-error return (the reference's `GetLastError` value)                                                                       | `safe_hook_setup_failure_returns_last_error`                                      | —                     |
