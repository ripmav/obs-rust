# E5 — Shared helpers (WP-34, P1-33.1–.5)

**Milestone:** M1 — Phase 1: code & recipe

Shared helpers (WP-34, P1-33.1–.5)
**Why:** the `shared/` helpers are leaf code with committed conformance scripts — cheap coverage wins.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E5S1** `obs-bpm` (P1-33.1) | port `shared/bpm` per S-024 M8 into `crates/obs-bpm` | `bpm` scripts in [helpers/](https://github.com/ripmav/obs-rust/tree/main/test/conformance/scripts/helpers) pass against C; S-024 R56/R57 pitfalls tested (HEVC w/o start code, unknown codec) | open |
| **E5S2** `obs-file-updater` (P1-33.2) | port `shared/file-updater` per S-024 M10 | `fu-*` scripts pass; fake-HTTP/in-memory-filesystem driven | open |
| **E5S3** `obs-netutil` happy-eyeballs (P1-33.3) | port `shared/happy-eyeballs` per S-024 M9 (ordering, resolver hints, constants) | `he-*` scripts pass; the socket race stays in C until a spec exists (documented) | open |
| **E5S4** `obs-inject-library` (P1-33.4) | port the thin `LoadLibrary`/`dlopen` helper, no spec | unit tests cover symbol resolution + failure paths; C file stays linked until win-capture (P5-31.6/.7) — exclusion only then | open |
| **E5S5** `opts-parser` (P1-33.5) | port `shared/opts-parser` per S-024 M1 into `crates/obs-util-small` | `opts-*` scripts pass | open |
