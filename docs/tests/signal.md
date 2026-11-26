# Test matrix: signals & procs (obs-signal)

- Test files: `crates/obs-signal/tests/signal_proc.rs` (dedicated sequential
  binary, one process: the callbacks are `extern "C"` fns with
  process-global counters, so all vectors run in a single test function);
  `crates/obs-signal/tests/signal_loom.rs` (P1-22.3 loom model of the lock
  structure — test double, see the loom note below); `crates/obs-signal/ tests/signal_threads.rs` (P1-22.3 real std-thread tests on the production
  handler); `crates/obs-signal/src/signal.rs` / `proc.rs` (no lib unit
  tests: every vector needs an emit or a destroy, which is process-global
  state)
- Task: P1-22 (spec S-004, `libobs/callback/signal.{c,h}`)
- Golden files: none (the `test/conformance/scripts/signal/` C scripts
  arrive with the C checkout)

Miri note: vectors whose semantics depend on **function-pointer equality**
(cb/data dedupe, disconnect-by-cb — C compares the pointer field) are
`#[cfg(not(miri))]`: Miri assigns a different address to every coercion of
a function item (verified 2026-11-11: two coercions of the same item yield
`0x212b7` vs `0x212c2`), so C's pointer-identity dedupe is unobservable
under Miri. They run on real hardware and are pinned by the C conformance
scripts.

loom note (P1-22.3): `parking_lot::ReentrantMutex` (the per-signal and
the global-callbacks locks) is **invisible to loom** (loom models
`std::sync`/`loom` primitives by intercepting their operations), so the
production handler cannot be model-checked directly. `signal_loom.rs` is
the documented test double (WP-36, ODD L86): it mirrors the production
lock points 1:1 with `loom::Mutex` — same locks, same acquire/release
points per operation, same recursive semantics (a reentrant lock re-taken
by its holder = the holder keeping it, not re-acquired). A loom deadlock
is a hard test failure; the permutation counts (state space explored) are
printed per test. `signal_threads.rs` complements the model with the
production handler under real std threads.

| Case                                           | Input                                                                                                                                                                               | Expected                                                                                                                                                                                                                                     | Platforms                    | Status           |
| ---------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------------------------- | ---------------- |
| layout                                         | `SignalHandler` is opaque `Box<Inner>`; C surface is fn-pointer only                                                                                                                | no C struct layout to pin (opaque in C as well); `Calldata` shared by value/pointer as in C                                                                                                                                                  | Linux, Windows, macOS        | pass             |
| vec01 basic                                    | add `f`, `g`; connect 2 cbs on `f` (a, b); emit `f`                                                                                                                                 | callbacks run in connection order (a then b); `cb_count(f) = 2`, `cb_count(g) = 0`                                                                                                                                                           | Linux, Windows, macOS        | pass             |
| vec02 dedupe (R21)                             | connect same (cb, data) twice; different data; connect on missing signal                                                                                                            | duplicate ignored → 1 entry; different data → 2; missing signal → warning, 0 entries                                                                                                                                                         | Linux, Windows, macOS        | pass (not(miri)) |
| vec03 disconnect                               | two entries; disconnect one                                                                                                                                                         | only the matching (cb, data) entry erased; count 1                                                                                                                                                                                           | Linux, Windows, macOS        | pass (not(miri)) |
| vec04 disconnect-during-emit                   | cb1 disconnects cb2                                                                                                                                                                 | cb2 skipped in this emit, erased at the end (R45); count 1, only cb1 ran                                                                                                                                                                     | Linux, Windows, macOS        | pass (not(miri)) |
| vec05 remove_current                           | cb1 = `signal_handler_remove_current`, cb2 = count                                                                                                                                  | cb1 marked, erased at end; cb2 still ran (already passed); self-call outside a callback is a no-op                                                                                                                                           | Linux, Windows, macOS        | pass             |
| vec06 connect-during-emit                      | cb connects cb2 mid-emit                                                                                                                                                            | Q-1: cb2 appended and **reached in the same emit**; count 2 after                                                                                                                                                                            | Linux, Windows, macOS        | pass             |
| vec07 nested emit                              | cb on `f` emits `g` (which emits `f` again)                                                                                                                                         | re-entrance allowed; per-signal `signalling` (Q-3) not recursed; counters exact                                                                                                                                                              | Linux, Windows, macOS        | pass             |
| vec08 global callbacks (R23–R26)               | connect_global ×2 (dedupe) + different cb + signal cb                                                                                                                               | globals run before signal cbs, in order, for **every** declared signal; disconnect_global outside emit erases                                                                                                                                | Linux, Windows, macOS        | pass (not(miri)) |
| vec09 global remove_current                    | global cb calls `remove_current`                                                                                                                                                    | marked, erased after its run; second emit: 0 globals; Q-2: cleared after *any* callback                                                                                                                                                      | Linux, Windows, macOS        | pass             |
| vec10 params shared (R42)                      | cb stores `params` pointer; nested emit with same `&mut Calldata`                                                                                                                   | identical pointer in both callbacks (no copy); in order                                                                                                                                                                                      | Linux, Windows, macOS        | pass             |
| vec11 refs (R22/R30/Q-7)                       | connect_ref → refs 2; destroy → 1 (survives); disconnect → 0 (destroyed)                                                                                                            | R02: fully usable at refs 1; R30: destroyed at 0; Q-7: emit-removed keep_refs subtracted **without** a destroy check                                                                                                                         | Linux, Windows, macOS        | pass (not(miri)) |
| vec13 declarations                             | add duplicate / bad decl / empty; `add_array` incl. NULL                                                                                                                            | R10/R11: rejects with `false`; array stops at NULL, continues after a failed decl                                                                                                                                                            | Linux, Windows, macOS        | pass             |
| vec14 global self-disconnect                   | global cb disconnects *itself*                                                                                                                                                      | Q-5: still connected for the current emit (erased at end) — 3 emits run 3 times; from outside: erased                                                                                                                                        | Linux, Windows, macOS        | pass (not(miri)) |
| vec15 reconnect-during-emit                    | cb disconnects + reconnects (cb, data) mid-emit                                                                                                                                     | Q-4: connection lost in this emit (marked entry blocked the reconnect), erased at end; next emit reconnects and Q-1 reaches it                                                                                                               | Linux, Windows, macOS        | pass (not(miri)) |
| vec16 remove_current after nested emit         | cb emits `g` (count), then `remove_current` (no-op: Q-2 already cleared)                                                                                                            | counters exact; no entry lost                                                                                                                                                                                                                | Linux, Windows, macOS        | pass             |
| vec17 nested emit + disconnect                 | inner emit's cb disconnects an outer-pending cb                                                                                                                                     | R45 skip + erase across the nesting; counters exact                                                                                                                                                                                          | Linux, Windows, macOS        | pass (not(miri)) |
| R44 cross-signal re-entrance                   | cb on `f` emits `g`, cb on `g` emits `f`                                                                                                                                            | no deadlock (handler lock released before any cb runs, S-004 §7); per-signal guards correct                                                                                                                                                  | Linux, Windows, macOS        | pass             |
| add_array continues after failure              | decl array with one bad entry among good ones                                                                                                                                       | good ones added, return `false` (failure continued, header inline)                                                                                                                                                                           | Linux, Windows, macOS        | pass             |
| R60 proc create/recursive                      | create empty; proc calls `proc_handler_call` on itself                                                                                                                              | recursive proc lock (R60); empty handler usable                                                                                                                                                                                              | Linux, Windows, macOS        | pass             |
| R61 proc add + duplicate                       | add `p`; add `p` again                                                                                                                                                              | duplicate rejected with `Procedure 'p' already exists`; 1 entry                                                                                                                                                                              | Linux, Windows, macOS        | pass             |
| R62 proc call                                  | call with calldata; proc stashes the calldata pointer                                                                                                                               | calldata copied out of the lock; proc runs **outside** the proc lock (R62); pointer usable after return (calldata outlives the call)                                                                                                         | Linux, Windows, macOS        | pass             |
| R63 proc destroy                               | destroy a handler with procs                                                                                                                                                        | everything freed (no refcounting on procs)                                                                                                                                                                                                   | Linux, Windows, macOS        | pass             |
| loom connect-during-emit (P1-22.3)             | model: t1 emit, t2 connect on the same signal (lock points as in production)                                                                                                        | **no deadlock over every interleaving** (145 permutations); the connected entry survives (Q-1: reached by the same emit); emit finished cleanly                                                                                              | all (no target-specific asm) | pass             |
| loom disconnect-during-emit                    | model: t1 emit, t2 disconnect (mark-while-emitting or erase)                                                                                                                        | no deadlock (145 permutations); the entry is erased — directly, or by the next emit's erase pass (a mark landing after this emit's pass stays marked until then)                                                                             | all (no target-specific asm) | pass             |
| loom destroy-while-emitting                    | model: t1 emit with a re-emitting global callback (the global → handler/signal edges), t2 connect_global + destroy                                                                  | no deadlock (78 007 permutations); the free clears the two lists in non-nested critical sections (the nested order deadlocked — the bug this model caught and the port fixed); handler dead afterwards                                       | all (no target-specific asm) | pass             |
| thread connect-during-emit (P1-22.3 Done-when) | production handler, real std threads: 200 emits while another thread does 200 connect+disconnect pairs of a second cb                                                               | no deadlock; the always-connected cb ran exactly once per emit (200); the transient cb cleaned up (count 1 after)                                                                                                                            | Linux, Windows, macOS        | pass             |
| thread Q-2 remove_current nested               | production handler, real std threads: f's cb emits g (g's cb = `remove_current`), then f's cb calls `remove_current`; a second thread emits an unrelated signal on the same handler | Q-2: the nested emit's return **cleared** (not restored) the thread's current cb — g's self-removal erased g's entry, f's own remove_current was a no-op (f survives, runs again on the second emit); the other thread's 50 emits unaffected | Linux, Windows, macOS        | pass             |
| thread destroy-while-guard-alive (WP-33 ADR)   | production handler + typed `Connection` guard on thread B; the test thread emits once (guard runs), destroys (refs 1 → 0); B: emit, connect, drop both guards                       | ADR 0002 A1: post-destroy emit/connect are no-ops, the disconnect on the dead handler is a no-op, the closure boxes are freed exactly once (the handler is leaked + kept reachable — the typed tests' Miri-leak pattern)                     | Linux, Windows, macOS        | pass             |

Notes / findings:

- **Stacked-borrows finding (Miri)**: the first draft held a live
  `&mut Vec<…>` into the handler/global lists across callback invocations.
  Miri correctly rejected this: a re-entrant `remove_current` creating its
  own `&mut` from the same `UnsafeCell` invalidates the outer tag, and the
  resumed outer loop is UB. The port now derives **only short-lived**
  references per helper call (`cb_*` / `g_*`), none across a callback —
  the same discipline that makes the C reference (raw pointers, no aliasing
  rules) sound in Rust.
- **`UnsafeCell` per list**: `HandlerState.signals` and
  `GlobalState.globals` are `UnsafeCell<Vec<Box<…>>>` — the documented
  escape hatch for interior mutation through `&self` under the (reentrant)
  locks. `addr_of!` + `cast_mut` + deref is **not** a substitute (it only
  grants SharedReadOnly under Stacked Borrows).
- **Entry stability (Q-1)**: `Vec<Box<…>>` keeps each record's allocation
  stable across pushes, so an index (or a raw `*mut Signal` /
  `*mut GlobalCb`) stays valid while the handler lock is released before
  any callback runs (S-004 §7) and entries appended mid-emit are reachable
  in the same emit.
- **Rust deviations from C (safe-by-design, documented in module docs)**:
  1. a destroyed handler is freed **after** the in-flight emit unwinds
     (C frees under the lock and can use freed memory in the tail);
  1. `remove_current` outside any callback is a no-op (C reads a stale
     thread-local);
  1. `connect_ref` on a missing signal does not leak a ref (C does).
- **Deferred to the typed API (P1-23)**: `Signal<A>` + `Connection` guard,
  typed args round trip (S-002 byte layout), ADR addendum.
- **Loom finding (P1-22.3, WP-36)**: the model's first draft (mirroring
  the port's deferred free as written — the two free locks **nested**,
  `handler_lock` held across the `global_lock` acquisition) deadlocked
  against a re-emitting global callback: the destroyer's
  `emitting == 0` check is a stale load, so a thread can pass it before
  an emit starts, then take the handler lock while that emit's global
  pass holds the global lock and the callback re-takes the handler lock
  for its nested signal lookup — a real cycle. The port's `destroy` /
  `maybe_free_dead` now clear the two lists in **two non-nested
  critical sections** (handler list under the handler lock, released,
  then global list under the global lock); the model re-checked green
  (78 007 permutations, no deadlock). The lock structure itself is
  unchanged (documented in the `signal.rs` "Threads" section).
- **Not in this WP** (C checkout blocked): conformance scripts
  `test/conformance/scripts/signal/`, cbindgen header diff for
  `signal.h`, `OBS_ENABLE_RUST` switch.
