# abi_happy_eyeballs (S-024 M9, P1-33.3) — test matrix

Test files: `crates/obs-ffi/src/abi_happy_eyeballs.rs`
Issue: WP-34 (P1-33.3)

C-ABI bridge for the M9 half in `crates/obs-ffi`:
`build_addr_list` (resolver hints + `getaddrinfo` + reorder, filling the
file-local `struct happy_eyeballs_ctx` the reference keeps static in
`happy-eyeballs.c`) and the `addrinfo` list it allocates. The
`#[repr(C)]` mirrors pin the glibc layout the swap-mode runner carries
(`socklen_t` = 4, `pthread_mutex_t` = 48, `sockaddr_storage` align 8).

| Area                                                                                                                                                                                                                          | Pinned by test                   | Conformance vector                              |
| ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------- | ----------------------------------------------- |
| **ctx layout vs the C reference**                                                                                                                                                                                             |                                  |                                                 |
| offsets pinned: `winner_addr_len` 136, `error` 140, `bind_addr_len` 152, `bind_addr` 160, `addresses` 416, `name_resolution_time_ns` 424, total 456 (a drift here is a conformance failure — the runner carries the C struct) | `ctx_layout_matches_c_reference` | the `he-*` swap run                             |
| **`format_port` (port → decimal)**                                                                                                                                                                                            |                                  |                                                 |
| positive ports, 0, and `i32::MIN` → `-2147483648` in the `char[64]` buffer (NUL-terminated)                                                                                                                                   | `port_decimal`                   | `helpers/he-resolve-args`                       |
| **resolver failure ABI**                                                                                                                                                                                                      |                                  |                                                 |
| `getaddrinfo` failure → non-zero result, `error`/`error_message` set, `addresses` NULL (no leak of a partial list)                                                                                                            | `resolve_failure_abi`            | (runner `he_fail` command; no committed script) |
