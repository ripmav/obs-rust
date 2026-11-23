# E18 — CI & repo hygiene (WP-50)

**Milestone:** M3 — Phase 0 close-out & future ADRs

CI & repo hygiene (WP-50)
**Why:** small review findings that accumulate debt.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E18S1** Test-matrix CI check (P0-20.4) | CI fails when a test file lacks/loses its matrix | a matrix-less test file reds CI | open |
| **E18S2** Fixture audit (P0-11.2) | the fixture set is justified | audit merged; unused fixtures removed or justified | open |
| **E18S3** Artefacts, deps, CODEOWNERS (findings 10/11/13) | committed test-run artefacts in `test/conformance/` removed or moved; `[workspace.dependencies]` centralised; real `CODEOWNERS` handles | tree clean; deps centralised; CODEOWNERS resolves to real handles | open |
