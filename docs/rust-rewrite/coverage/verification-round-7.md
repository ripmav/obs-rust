# Coverage verification — round 7 (2026-11-04)

Question: **how much of the original code (obs-studio @ 33.0.1@65c320b46) is covered by
(sub)tasks, specs and tests?** Acceptance: ≥ 75 % covered; optimum 80–90 %; helper
functions and one-line getters/setters are out of scope by user decision (excluded as
*trivial*: body < 5 lines).

Universe (regenerated with `tools/function-coverage.py`, ctags 6.2.1 + ObjC/Swift
extractors): 16 414 functions, 13 309 non-trivial, of which **12 197 first-party
non-trivial** (vendored third-party excluded: deps/*, SDK headers).

## Headline (non-trivial first-party functions)

| metric | meaning | value |
|---|---|---|
| **Covered by (sub)tasks, specs or tests** | a task owns the file (T2), the plan names the file (F1), or any artefact names the function (any1) / cites the file (any2) | **100.0 %** |
| Task coverage (file → task, T2) | explicit row in `coverage-matrix.md` | 100.0 % (0 glob rows — all 1 929 rows are exact file paths) |
| File explicitly named in the plan (F1) | path or basename in phase/subtask/guide docs | 94.3 % |
| Function named by a task, spec or test (any1) | identifier-level naming | 27.9 % |
| Behaviour pinned by a spec or a test (S2 ∨ X2) | the file is cited in `specs/S-*.md` or `test/conformance/` | 47.1 % |
| Full triple (task + spec + test, all2) | all three artefact kinds reach the file | 30.1 % |

**Verdict: PASS.** Coverage by (sub)tasks, specs or tests is 100.0 % — above the 75 %
floor and above the 80–90 % optimum band. The band is a sanity range, not a ceiling:
coverage sits above it because the coverage matrix was expanded to one explicit row per
file (verified: zero glob rows), so *every* non-trivial first-party file has a task that
owns it. The nuance: task coverage means *the port is scheduled and owned*, not that the
behaviour is defined — the behaviour-pinning number is the 47.1 % S2 ∨ X2 row, and the
remaining 53 % is fully assigned to spec actions by the round-6 backlog
(`specs/BACKLOG.md`, 41 actions / 748 files); S2 rises as those specs are written.

## Independent verification (two read-only subagents)

Method: stratified samples drawn from `function-inventory.csv` (round-5 snapshot, copied
to `/tmp/opencode/fi-round5.csv`), each claim re-checked by the verifier's own greps
over the artefact corpora — plan docs (`phase-*.md`, `subtasks/*.md`, `guides/*.md`,
`rust-rewrite-plan.md`, `coverage-matrix.md`), specs (`specs/S-*.md` + README), conformance
corpus (`test/conformance/`, `vectors/` excluded). Substring traps checked (a basename hit
inside a longer identifier or a different same-named file does not count).

| dimension | samples | confirmed | weak / borderline | false positives | false negatives |
|---|---|---|---|---|---|
| F1 = 1 (file named in plan) | 25 | 25 exact full-path | 0 | 0 | — |
| F1 = 0 (file not named) | 15 | 0 truly absent | 15 owned via matrix row only (T2-only) | — | 0 |
| Glob share of T2 | full matrix | 1 929 explicit rows | 0 glob / directory rows | — | — |
| S2 = 1 (file cited in spec) | 20 | 20 verified (9 basename-only, all unambiguous by context) | 0 | 0 | — |
| X2 = 1 (file in conformance corpus) | 15 | 14 solid (`#include`/link lists) | 1 borderline (see F3) | 0 | — |
| S2 = 0 ∧ X2 = 0 (cited nowhere) | 15 | 14 confirmed absent | — | — | 1 (brace form, fixed) |
| specs/README.md consistency | full | 56 table rows ↔ 56 `S-*.md` files | 0 orphans either way | — | — |

## Findings and dispositions

1. **F1 (task naming) is reliable: 0/25 sampled false positives**, every hit an exact
   full-path citation. The F1 = 0 pool contains no truly unowned file in the sample:
   all 15 are owned by an explicit matrix row (T2-only coverage — the matrix is
   deliberately not part of the F1 prose corpus).
2. **T2 has saturated (16 413/16 414 rows = 1)** because the matrix is per-file. It is
   structural (0 glob rows), not an artefact. The discriminating task metric is F1
   (94.3 %). The single T2 = 0 row — `libobs/obs-core-modules.c.in` — is named in
   `phase-4-core-object-model.md` (F1 = 1); `tools/task-coverage.py` still exits 0.
3. **1 borderline X2 citation**: `plugins/mac-capture/mac-audio.c` is named in a script
   header, but the maccapture runner is a pure-model runner (the ObjC file is never
   compiled) — the component is exercised via transcription. Kept as covered (the model
   is derived from that file), noted as the weakest class of test citation.
4. **1 S2 false negative, fixed**: specs cite files in brace form
   (`plugins/linux-jack/{jack-input,jack-wrapper}.c` in S-049, ~8 such patterns found),
   which the literal matcher missed. `tools/function-coverage.py` now expands brace
   forms in `corpus_files()` (regenerated: S2 ∨ X2 46.0 % → 47.1 %, triple
   29.2 % → 30.1 %; the table above shows the post-fix numbers).
5. **Basename-collision risk** (e.g. `ComPtr.hpp` exists in both
   `deps/libdshowcapture/src/source/` and `libobs/util/windows/`): a basename-only
   mention of one silently credits the other. Exact-path citations dominate in practice
   (25/25 samples resolved on exact paths); accepted residual risk, noted here.
6. **Record correction**: the round-5 commit message quotes S2 ∨ X2 = 48.2 %; the
   committed round-5 report already says 46.0 %. The report (tool-generated) is
   authoritative; the message quoted an intermediate pre-fix number.

## Residual risks

- The measurement limits documented in `function-coverage.md` (macro-generated
  functions invisible to ctags, `#if`-branch body measurement, identifier collisions)
  apply to all flags.
- Subagent sampling is a lower bound on reliability: the F1 sample is file-order-biased
  toward well-covered files, and S2 stratification was partially padded (few areas have
  any S2 = 1 file yet). 0/75 false positives across 85+ checks is an estimate, not a
  census.
