# E12 — User corpus (WP-42, P0-18)

**Milestone:** M2 — Phase 1: integration & review

User corpus (WP-42, P0-18)
**Why:** byte-identical round trips on real-world config/script data (P1-09.4, P1-24.2/.3, P1-25.5).

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E12S1** Anonymiser + loader + index (code half) | the corpus tooling | anonymiser + load check + index pass on a synthetic fixture set | open |
| **E12S2** ≥20 corpus collections (D4, decided 2026-11-23) | the corpus populated with real-world-shaped data | ≥20 collections ingested, composed of licence-compliant public material (OBS issue-tracker configs, gists, …) + synthetic fixtures; load check green; unblocks the corpus round-trip subtasks | open |

> D4 decided 2026-11-23: the corpus is a **mix of licence-compliant public material and synthetic
> fixtures** (no private user collection). The code half (E12S1) runs on synthetic fixtures
> immediately.
