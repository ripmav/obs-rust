# abi_platform (S-055 U4/U5, P1-07.3) — test matrix

WP-30 (P1-07.3 obs-ffi C-ABI half), crate `crates/obs-ffi`, module
`abi_platform` (`src/abi_platform.rs`, `#[cfg(unix)]`,
`#[cfg(any(test, feature = "c-swap"))]`). The six C exports
(`dbus_sleep_info_create/destroy`, `dbus_inhibit_sleep`,
`portal_inhibit_info_create/destroy`, `portal_inhibit`) bridge to the
`obs_util::os_inhibit` API with `Box` handles (opaque in C); the first
export wires the platform log sink and the R80 fatal hook to the C
`blog`/`bcrash` shims exactly once. ADR 0005 Amendment 1: every
`unsafe` block carries a `SAFETY:` comment.

| Area                                                                                                                                                                                                                                                                                     | Pinned by test                     | Conformance vector                                                                           |
| ---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------- | -------------------------------------------------------------------------------------------- |
| **wiring: platform log sink -> C `blog`**                                                                                                                                                                                                                                                |                                    |                                                                                              |
| after `ensure_wired`, an `os_fs::log_platform` line reaches the C log handler with format `%s` (the handler round-trip asserts on the unformatted format — the C `va_list` is not consumable from Rust, house pattern from `r21_r22_handler_roundtrip`); the default handler is restored | `wiring_routes_logs_to_c_handler`  | the `log` lines of `utilrest/dbus_*` / `portal_*`                                            |
| **create/destroy with an unreachable bus (R23/R26)**                                                                                                                                                                                                                                     |                                    |                                                                                              |
| with a nonexistent `DBUS_SESSION_BUS_ADDRESS` both create exports give NULL and both destroy exports are no-ops on NULL (the runner exercises the NULL destroys)                                                                                                                         | `create_null_with_unreachable_bus` | `utilrest/dbus_nobus`                                                                        |
| **full C-ABI flow (U4/U5)**                                                                                                                                                                                                                                                              | (no unit test — C vectors)         |                                                                                              |
| the six exports drive the complete dbus/portal inhibit flows byte-equal to the C reference (swap mode links the Rust staticlib in place of `platform-nix-dbus.c` / `platform-nix-portal.c`)                                                                                              | conformance swap run               | `utilrest/dbus_inhibit_*`, `dbus_noservice`, `dbus_nobus`, `portal_inhibit`, `portal_errors` |
