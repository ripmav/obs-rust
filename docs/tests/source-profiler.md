# source-profiler (S-055 U3, P1-12.3, WP-37) — test matrix

Spec: `docs/rust-rewrite/specs/S-055.md` U3. C reference: `libobs/util/source-profiler.c`
(630) / `source-profiler.h` (66) + the `source_profiler_*` externs of `obs-internal.h`.
Implementation: `crates/obs-util-small/src/source_profiler.rs` (core + RAII guards),
`crates/obs-ffi/src/abi_source_profiler.rs` (the 15 C exports, `c-swap`-gated),
`crates/obs-ffi/c/srcprof-bridge.c` (the `obs->video.graphics` enter/leave shim).

## Core (`source_profiler.rs` unit tests, fake backend)

| Area                                                                                                | Pinned by test               | Conformance vector                  |
| --------------------------------------------------------------------------------------------------- | ---------------------------- | ----------------------------------- |
| disabled: everything inert, `*timer` untouched, `fill_result` untouched, `get_result` NULL          | `disabled_inert`             | `utilrest/srcprof_basic`            |
| Q-7: history lags one frame; first collect has no data; zero tick = "no data"                       | `basic_lag_and_first_result` | `utilrest/srcprof_basic`            |
| the 1024-entry per-source ring wraps; `reset_video` clears history                                  | `ring_wrap_and_reset`        | `utilrest/srcprof_ring`             |
| GPU mode: timer/range create-destroy counts, context enter/leave balanced, per-frame range          | `gpu_timers`                 | `utilrest/srcprof_gpu`              |
| `frame_begin` advances `timer_idx` only when GPU is already enabled (the enable frame does not)     | `gpu_timers`                 | `utilrest/srcprof_gpu`              |
| async sources: fps from `last_async_ts` deltas, the min/max in/out samples                          | `async_fps`                  | `utilrest/srcprof_async`            |
| fps computation wrap (timestamp reset)                                                              | `calculate_fps_wrap`         | `utilrest/srcprof_async`            |
| `remove_source` queues the removal task on the graphics queue (deferred, R22); no-op while disabled | `remove_deferred`            | `utilrest/srcprof_remove`           |
| Q-2: enabled + unknown source → `fill_result` false with a zeroed struct                            | `q2_unknown_source_zeroed`   | `utilrest/srcprof_basic`            |
| RAII guards (`SourceTickGuard`/`SourceRenderGuard`): drop ends the tick / render pass               | `guards`                     | not scriptable (unit test required) |

## Bridge (`abi_source_profiler.rs` unit tests, real C ABI against test stand-ins)

| Area                                                                                   | Pinned by test                  | Conformance vector       |
| -------------------------------------------------------------------------------------- | ------------------------------- | ------------------------ |
| disabled: tick 0, `*timer` sentinel untouched (R14), fill untouched, `get_result` NULL | `disabled_inert_and_q2_zeroed`  | `utilrest/srcprof_basic` |
| enabled + unknown source: fill false + zeroed, `get_result` NULL (Q-2)                 | `enabled_unknown_source_zeroed` | `utilrest/srcprof_basic` |

## Conformance (vectors `test/conformance/scripts/utilrest/`, both modes)

| Vector           | Checks                                                                 | Mode         |
| ---------------- | ---------------------------------------------------------------------- | ------------ |
| `srcprof_basic`  | disabled inert, enable/reset_video, tick+fill, Q-2 zeroed, NULL source | check + swap |
| `srcprof_gpu`    | GPU timers/ranges + context balance, `*timer` set only while enabled   | check + swap |
| `srcprof_async`  | async fps + min/max samples through `async_frame_received`             | check + swap |
| `srcprof_ring`   | ring wrap + reset                                                      | check + swap |
| `srcprof_remove` | deferred removal task, post-remove fill                                | check + swap |

Recipe: `source-profiler.c` in the swap set (`run.sh apply_swap`) + the CMake
genex (`$<$<NOT:$<BOOL:${OBS_ENABLE_RUST}>>:util/source-profiler.c>`); both
`OBS_ENABLE_RUST` patches carry it identically (ODD L82). The cbindgen header
diff covers the 4 EXPORT declarations (`source-profiler.h`) + the 11 internal
externs (`obs-internal.h`) — see `docs/tests/header-diff.md`.

## P1-26.2 (WP-37) — sanitizer coverage

| Check        | Command / job                                                                                                                                                               | Result (2026-11-26, aarch64 local) |
| ------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------------- |
| Miri         | CI supply-chain job: `cargo +nightly miri test -p obs-util … -p obs-util-small -p obs-data … --lib` (all pure crates)                                                       | clean (incl. the profiler core)    |
| ASan (local) | `RUSTFLAGS="-Zsanitizer=address" cargo +nightly test -p obs-data -p obs-util-small --lib` (ASan is a documented local command, not a CI job — Miri is the CI pass; ODD L87) | clean (runtime verified linked)    |

P1-26.3 findings: none (no refcount leaks, double frees or UB).
