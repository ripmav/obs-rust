# E19 — Headless smoke (WP-51, P0-16)

**Milestone:** M3 — Phase 0 close-out & future ADRs

Headless smoke (WP-51, P0-16)
**Why:** the first end-to-end proof that the swapped build actually captures.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E19S1** Smoke test | null-graphics startup, non-black frame, 5 s MKV, `ffprobe` validation | all four checks pass in a run | open |
| **E19S2** Smoke in CI | the smoke as a CI job under `xvfb-run` (reusing the WP-27 checkout job) | job green on `ubuntu-latest` | open |
