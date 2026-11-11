# Test matrix: bmem (obs-util)

- Test files: `crates/obs-util/src/bmem.rs` (unit tests), `crates/obs-util/tests/bmem_fatal.rs` (subprocess)
- Task: P1-01 (spec S-001, allocation + crash parts; `blog` half lands with P1-13)
- Golden files: none (fatal-path expectations are inline MUST strings from S-001)

| Case | Input | Expected | Platforms | Status |
|---|---|---|---|---|
| alignment_is_32 | `base_get_alignment()` | 32 (S-001-R10) | Linux, Windows, macOS | pass |
| blocks_are_32_aligned | sizes 1…1 MiB | every block `% 32 == 0` (R01) | Linux, Windows, macOS | pass |
| counter_tracks_alloc_free | 3 alloc / 3 free | counter base+3 → base (R04/R05/R09) | Linux, Windows, macOS | pass |
| bfree_null_is_noop | `bfree(NULL)` | counter unchanged, no crash (R05) | Linux, Windows, macOS | pass |
| brealloc_null_is_alloc | `brealloc(NULL, 64)` | aligned block, net counter +1 (R06) | Linux, Windows, macOS | pass |
| brealloc_grows_preserving_prefix | 16 → 256 | first 16 bytes preserved, counter unchanged (R08) | Linux, Windows, macOS | pass |
| brealloc_shrinks_preserving_prefix | 256 → 32 | first 32 bytes preserved, counter unchanged (R08) | Linux, Windows, macOS | pass |
| bmemdup_copies_bytes | 5-byte slice | exact copy, counter +1 (R11) | Linux, Windows, macOS | pass |
| bzalloc_zero_fills_and_counts | 64 | all zeros, counter +1 (R12) | Linux, Windows, macOS | pass |
| bstrdup_nul_terminates | `b"hola"`, n=4 and n=2 | `n+1` bytes, `d[n] == 0` (R13) | Linux, Windows, macOS | pass |
| bmem_raw_roundtrip_keeps_counter | `into_raw`/`from_raw` | counter +1 stable, contents intact (R05/R09) | Linux, Windows, macOS | pass |
| bmalloc_zero_fatal_default_handler | subprocess `bmalloc(0)` | exit 0, stderr exactly `bmalloc: Allocating 0 bytes is broken behavior, please fix your code!` (no newline) (R02/R24) | Linux, Windows, macOS | pass |
| brealloc_zero_fatal | subprocess `brealloc(NULL, 0)` | exit 0, stderr exactly the `brealloc:` MUST text (R07) | Linux, Windows, macOS | pass |
| crash_handler_once_only_and_serves_crash | subprocess: set(42), set(99), `bmalloc(0)` | stdout `warning: Tried to set a crash handler when one already exists.`; stderr `HANDLER:42:<bmalloc text>\n`; 99 never used (R25) | Linux, Windows, macOS | pass |
| reentrant_crash_exits_2 | subprocess: handler calls `bcrash` | stderr contains `Crashed in the crash handler`, exit 2 (R24) | Linux, Windows, macOS | pass |
| crash_handler_install_is_once | in-process double install | param stays 7 (first wins) (R25) | Linux, Windows, macOS | pass |

Notes:
- `bcrash` never returns, so fatal paths run in a re-executed subprocess
  (`tests/bmem_fatal.rs`, `__BMEM_FATAL` directive; `Once` guarantees one
  worker thread per child).
- Counter tests serialise on a `Mutex` (process-wide counter, parallel
  tests would race each other's snapshots).
- OOM path (R03) is not scriptable with the current runner (S-001 §8) —
  the message text is pinned in code next to the crash call.
- Wide `bstrdup` variants and the `extern "C"` shim: C-side, tracked per
  WP (need the C checkout; see phase-1 doc S2).
