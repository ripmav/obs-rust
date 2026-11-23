# E13 — Recipe batch, nightly & dependency scan (WP-43 + WP-44 + WP-45)

**Milestone:** M2 — Phase 1: integration & review

Recipe batch, nightly & dependency scan (WP-43 + WP-44 + WP-45)
**Why:** the batch that ticks everything the toolchain finally makes verifiable — and the soak that flips the default.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E13S1** Recipe batch ticks (WP-43) | after E2/E9/E10/E11/E12: every area through `run.sh swap`, tick all now-verifiable subtasks | ticks only for what actually passed (the audit's lesson); covers the WP-31 unblock list + P1-09.4, P1-15.3/.4, P1-16.4, P1-20.3, P1-21.3, P1-24.2/.3, P1-25.5, P1-28.2/.3 and P1-06.3 (jitter p99 + recipe) | open |
| **E13S2** Nightly + soak + flip (WP-44, P1-30) | nightly builds with `OBS_ENABLE_RUST=ON`, two-week soak, then default flip + docs | nightly job green ON + OFF (`ctest`); soak period clean; default flipped in docs/CI (needs D3) | open |
| **E13S3** Dependency usage scan (WP-45, P1-31) | usage scan for `deps/blake2` / `deps/json11` + deferral record | scan merged; deferral record written (deletion stays P7-04) | open |
