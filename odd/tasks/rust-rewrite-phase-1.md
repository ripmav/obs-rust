# Feature: Rust Rewrite — Phase 1 (Leaf Libraries)

Stack: branches off the Phase 0 head `docs/rewrite-coverage-2026-11` (PR #10); rebase onto `main` once #10 merges. Each WP = own branch + PR; small work packages = commits.

## Specs

- S1 (user, 2026-11-11, verbatim, German): “Arbeite autonom die Roadmap ab. Jeder Milestone wird ein Stack, wenn möglich mit main als base, und jedes WP wird ein PR im Stack. Jedes WP soll in möglichst kleine Arbeitspakete aufgeteilt werden welche dann, abgearbeitet, als commit im jeweiligen Branch/PR commited und gepushed werden.”
- S2 (state): no C checkout on this machine (`../obs-studio` absent). Per-WP C-side substeps — cbindgen header diff vs committed `libobs/*.h`, `OBS_ENABLE_RUST` build switch, fuzz/golden comparison *against C*, P1-29/30/31 — are recorded on each WP and tick ONLY after `tools/link-obs-source.sh` + CMake env. Spec-driven implementation and spec-vector tests proceed in this repo (strangler pattern: Rust crate + safe API + tests now; C swap later).
- S3 (phase pattern, verbatim from `docs/rust-rewrite/phase-1-leaf-libraries.md`): “(1) Rust crate with a safe API, (2) `extern "C"` shim with the exact C signature, (3) `cbindgen` check that the generated header is compatible with the existing one, (4) goldens from P0-14 pass, (5) C file kept but excluded from the build when `OBS_ENABLE_RUST=ON`.”
- S4 (WP list, verbatim short titles from the phase file): P1-01 bmem (S-001); P1-02 dstr (S-006); P1-03 darray (S-005); P1-04 deque (S-005); P1-05 threading; P1-06 time/sleep; P1-07 filesystem; P1-08 text/lexers (S-010/S-011); P1-09 config-file (S-007); P1-10 base64/crc32/blake2; P1-11 process/module; P1-12 profiler (S-024 M7); P1-13 logging; P1-14 misc util; P1-15 vec2/3/4 (S-008); P1-16 matrix3/4 (S-008); P1-17 quat/axisang/plane/bounds (S-008/S-009); P1-18 math-extra/half/srgb (S-009); P1-19 SIMD; P1-20 calldata (S-002); P1-21 decl/proc (S-003); P1-22 signal (S-004); P1-23 typed Rust signals; P1-24 JSON engine (S-014); P1-25 obs_data_t core (S-014); P1-26 autoselect/ownership (S-014); P1-27 obs_properties (S-015); P1-28 hotkey data layer (S-034); P1-33 shared/ helpers (S-024 M8/M10/M9/M1); P1-29 header diff check (C); P1-30 default switch (C); P1-31 vendored deps scan (C); P1-32 phase review.
- S5 (dependency policy): ADR 0005 — stdlib first; new deps need allow-list license + recorded exception. Currently planned additions: `crc32fast` (MIT, P1-10), `blake2` (MIT, P1-10), `half` (MIT/Apache-2.0, P1-18). Each gets a cargo-deny green check in its WP.

## WPs

- WP-1 = P1-01 bmem — spec S-001 — branch `p1/bmem` — C-blocked: alloc-counter golden vs C, cbindgen diff, OBS_ENABLE_RUST.
- WP-2 = P1-04 deque — spec S-005 — branch `p1/deque` — C-blocked: wraparound property vs C, layout check vs C header.
- WP-3 = P1-03 darray — spec S-005 — branch `p1/darray` — C-blocked: layout vs C header, OBS_ENABLE_RUST.
- WP-4 = P1-10 crc32/blake2 — known-answer tests — branch `p1/crc-hash` — C-blocked: none beyond usage scan (deps are frontend-only per phase file).
- WP-5 = P1-15/P1-16 vec + matrix4 — spec S-008 — branch `p1/vec-matrix` — C-blocked: 10k goldens vs C, size/align vs C headers, benchmark.
- WP-6+ = dstr (S-006), calldata (S-002), decl (S-003), signal (S-004), math-rest (S-009), config-file (S-007), obs-data (S-014) — branches `p1/<name>` as capacity allows.
- C-tail WPs = P1-29/30/31/32 — blocked on C checkout + two-week nightly soak.

## Log

L1 (2026-11-11): user request verbatim — see S1.
L2 (2026-11-11): routing — no C checkout, so WPs execute spec-driven in this repo; C substeps tracked per WP (S2). Stack base = Phase 0 head until PR #10 merges (S1 “wenn möglich mit main als base”). Review at bounded stop per Phase 0 doc L16–L19; PRs carry review-status section.
