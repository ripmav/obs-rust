# Test matrix: typed signals `Signal<A>` / `Connection` (obs-signal)

- Test file: `crates/obs-signal/src/typed.rs` (unit tests of the typed
  module, P1-23)
- Task: P1-23 (spec S-002 byte layout, S-003 declarations, S-004
  handler semantics; design per ADR 0002 Amendment 1)
- Golden files: none (the C-packed bytes are hand-computed from S-002 §4,
  W = 8 little-endian; the C conformance scripts stay in
  `test/conformance/scripts/signal/`)

| Case                              | Input                                                                   | Expected                                                                                                                                                        | Platforms             | Status |
| --------------------------------- | ----------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------- | ------ |
| bytes_match_c_packing             | `void t(int a, float f, bool b, ptr p, string s)` written via Rust      | exact 126-byte S-002 §4 stack (record sizes 26/26/19/26/21 + 8 terminator) — byte-identical to the C packing                                                    | Linux, Windows, macOS | pass   |
| round_trip                        | all five types written, then read back (`-7`, `2.25`, `true`, ptr, str) | Rust type → calldata → Rust type equal (P1-23.2 round trip)                                                                                                     | Linux, Windows, macOS | pass   |
| missing_params_take_c_defaults    | partial calldata (only `a` present); then 1-byte `a` read as 8-byte     | C defaults: float `0.0`, bool `false`, ptr `NULL`, string `NULL` (S-002 `calldata_int` family); mis-sized payload is absent (Q-1)                               | Linux, Windows, macOS | pass   |
| connection_raii_drop_disconnects  | connect → emit (runs) → drop guard → emit                               | cb_count 1 → 0 on drop; second emit does not run (T2: no leaked C-side connection; drop = disconnect)                                                           | Linux, Windows, macOS | pass   |
| distinct_closures_no_dedupe       | two equal closures connected, emit, drop each                           | both run (C identity is (cb, data), S-004-R21 — unique box pointers, no accidental dedupe); counts 2 → 1 → 0                                                    | Linux, Windows, macOS | pass   |
| emit_on_reuses_capacity           | caller-owned calldata, two `emit_on`                                    | no reallocation after the first write (T6 real-time path; S-002-R13 append, capacity unchanged)                                                                 | Linux, Windows, macOS | pass   |
| guard_outlives_handler_destroy    | connect → `signal_handler_destroy` (refs → 0) → drop guard              | dead-handler disconnect is a no-op (S-004 module docs); closure box still freed exactly once (Miri: no double free)                                             | Linux, Windows, macOS | pass   |
| drop_other_connection_during_emit | two connections; A's callback drops B's guard mid-emit                  | B marked (S-004-R30), skipped in this emit (R45), erased at its end; Box freed once; A stays connected; counts 1/0 → 2/0 (the drop-ordering proof, ADR 0002 A1) | Linux, Windows, macOS | pass   |
| send_sync_statically              | compile-time `Send`/`Sync` assertions for handle and guard              | always `Send + Sync` even for a non-`Sync` argument type (P1-23.1 "no Send/Sync surprises"; requires `SignalHandler: Sync`, ADR 0002 A1)                        | Linux, Windows, macOS | pass   |

Notes / findings:

- **Toolchain note (Rust 1.99):** raw pointers and `NonNull` carry
  negated auto-trait impls (`!Send`/`!Sync` unconditionally), so
  `DataToken` needs manual `unsafe impl Send + Sync` (SAFETY comments at
  the impls) and `SignalHandler` is `Sync` (explicit) but **not** `Send`
  — the `Box<SignalHandler>` stays on its creating thread; shared
  references and the typed handles cross threads. Recorded in ADR 0002
  Amendment 1.
- **Self-disconnect test mechanics:** a `'static` closure cannot capture
  a guard tied to a shorter handler borrow, so the guard and its handler
  live in dedicated statics (`Box::leak` + a static reference keeps the
  handler reachable — Miri leak check: not a leak).
- **Miri:** no foreign calls in the module (pure Rust + `parking_lot` +
  std alloc), so no `#[cfg_attr(miri, ignore)]`. The CI Miri job caught
  the original `drop_inside_its_own_callback` test: dropping a guard from
  inside its **own** callback frees the running closure object
  (stacked-borrows UB — the trampoline borrows the closure for the call;
  in C the callback code is separate from the `data` payload). The API
  contract now forbids that (use `signal_handler_remove_current()` or
  defer the drop — `Connection` docs, ADR 0002 A1) and the test pins the
  sound variant above.
- **Not in this WP:** C-ABI exports for the typed API (no C surface —
  the typed API is Rust-only by design, ADR 0002 Amendment 1), a
  `Signal<()>` blanket impl for parameterless signals (deliberately
  skipped — a one-liner per plugin if ever needed), and a `Proc`
  counterpart (P1-21's recipe half, later WP).
