# abi_bmem (S-001, P1-01.3) — test matrix

WP-43 (P1-01.3 obs-ffi C-ABI half), crate `crates/obs-ffi`, module
`abi_bmem` (`src/abi_bmem.rs`,
`#[cfg(any(test, feature = "c-swap"))]`). The six C exports of
`libobs/util/bmem.c` (`bmalloc`, `brealloc`, `bfree`, `bmemdup`,
`bnum_allocs`, `base_get_alignment`) bridge to the `obs_util::bmem`
API. From this WP on, the swap build's whole process (C + Rust) shares
the single Rust allocator: the swap set grows `util/bmem.c`
(`test/conformance/obs-enable-rust.patch`, the abi-host rust mode), so
every build that links the staticlib drops the C file and resolves the
allocator symbols to these exports (the `abi_common` imports resolve to
them via `#[link_name]`). The first bmem call wires the Rust fatal
paths (zero-size alloc, OOM) to the C shim's `bcrash` (base-logging.c,
the `abi_platform` fatal-hook pattern) so crash behaviour matches the
C reference; the fatal messages are fixed literals without `%`.
ADR 0005 Amendment 1: every `unsafe` block carries a `SAFETY:` comment.

| Area                                                                                                                                                                                                                            | Pinned by test                                                     | Conformance vector                           |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------ | -------------------------------------------- |
| **allocator behaviour = C reference**                                                                                                                                                                                           | (no unit test — C vectors)                                         |                                              |
| alignment (32), live-block counter, realloc grow/shrink prefix preservation, dup/strdup — swap mode links the staticlib in place of `bmem.c` and the vectors are byte-identical to the C-generated ones                         | conformance swap run                                               | `bmem/01-alignment-and-count`, `bmem/03-dup` |
| **fatal paths through the C crash handler**                                                                                                                                                                                     | (no unit test — C vectors)                                         |                                              |
| zero-size `bmalloc`/`brealloc` run in a child process with the C-installed crash handler; the Rust fatal paths route through the C shim's `bcrash`, so the output (`crash <msg>`, exit 77) is byte-identical to the C reference | conformance swap run                                               | `bmem/04-fatal-paths`                        |
| **log family untouched**                                                                                                                                                                                                        | (no unit test — C vectors)                                         |                                              |
| `blog`/crash-handler installation still served by the C shim (base-logging.c is not swapped)                                                                                                                                    | conformance swap run                                               | `bmem/05-logging`                            |
| **pure-crate behaviour (the bridge forwards to these)**                                                                                                                                                                         | `crates/obs-util` (245 lib + 6)                                    |                                              |
| `bmalloc`/`bfree`/`brealloc`/`bmemdup`/`bzalloc`/`bstrdup*` semantics, 32-byte alignment, the live-block counter, crash-handler once-only install                                                                               | `bmem` unit tests, `tests/bmem_fatal.rs`, `tests/leak_counters.rs` | —                                            |

## Verification (2026-11-24, aarch64)

- `run.sh check bmem`: 4 ok + 1 skip (`02-realloc`, known C reference
  bug, `known-c-bugs.txt`).
- `run.sh swap bmem`: 4 ok + 1 skip — byte-identical to the C vectors.
- cbindgen header diff: `util/bmem.h` 6/6 (the 92/92 total).
- The `cargo test` allocator stand-ins in `abi_cf_lexer` are removed —
  the tests now link the real bridge (the `abi_common` imports resolve
  to the `abi_bmem` exports under the same `cfg(test)` gate).
