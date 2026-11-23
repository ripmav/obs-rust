# E4 — Typed signals (WP-33, P1-23)

**Milestone:** M1 — Phase 1: code & recipe

Typed signals (WP-33, P1-23)
**Why:** the safe Rust API over the C signal layer — the point of having `obs-signal` at all.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E4S1** `Signal<A>` + `Connection` design | design addendum (ADR) for the typed API | ADR addendum accepted; lock structure and S-004 Q-2 semantics documented | open |
| **E4S2** Calldata bridge | typed args to/from `calldata` (S-002 byte layout) | round-trip test passes (Rust → calldata → Rust, all type families) | open |
| **E4S3** Docs & tests | crate-level docs and usage tests for `crates/obs-signal/` | docs + tests merged; example compiles against the public API | open |
