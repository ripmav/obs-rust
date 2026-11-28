# P1-30 soak — `OBS_ENABLE_RUST` default flip

P1-30.2 (observe two weeks) is **in progress**: soak start **2026-11-28**,
flip date **2026-12-12** (start + 14 days). P1-30.2/.3 stay open until the
flip date.

## What the nightly does

`.github/workflows/nightly.yml` (daily 00:30 UTC, plus
`workflow_dispatch`) builds the C reference — pinned obs-studio 33.0.1 with
the D3 patch (`test/conformance/obs-enable-rust.patch`) applied — in both
modes of the OBS_ENABLE_RUST pair, over every conformance CI area
(`test/conformance/ci-areas.txt`):

| Mode                              | Command        | Build                                                                                   |
| --------------------------------- | -------------- | --------------------------------------------------------------------------------------- |
| `OBS_ENABLE_RUST=OFF` (ctest OFF) | `run.sh check` | the all-C reference                                                                     |
| `OBS_ENABLE_RUST=ON` (ctest ON)   | `run.sh swap`  | the C sources the D3 patch excludes are dropped, their symbols come from `libobs_ffi.a` |

**Build matrix** (the platforms the C build supports in this repo's CI):
`ubuntu-latest` (x86_64 Linux) only — the conformance C build has only ever
run there. aarch64 (Pi 5 dev host), macOS and Windows are not in CI for the
C reference build.

Each run leaves one comparable artifact (`soak-<run id>`: `summary.txt` +
`check.log` + `swap.log`): per-mode exit status, `ok`/`skip`/`fail` counts,
every `FAIL` line, and the **crash-signature lines** (runner aborts: bash
job messages + signal text).

## What is watched (P1-30.2)

- **New crash signatures** — diff today's `summary.txt` against the
  previous run's: a new crash line, a new `FAIL` line, or a rising
  `skip`/allowlist count (known-c-bugs.txt / known-racy.txt).
- **Perf** — the conformance builds run at `-O0` (correctness harness, not
  a perf signal); perf is watched by the P0-17 benchmark gate in CI (2%
  regression vs `test/ref-bench/baseline.json`, x86_64 runner, runs on
  every PR) and must stay green through the soak.
- **Patch drift** — the nightly's `apply D3 patch` step; red means the D3
  patch no longer applies cleanly to the pinned tree (WP-44 re-targeted
  the patch to the 33.0.1 tag; it previously targeted the fe1ed79 working
  tree, ODD L94).

## Stop / flip condition

- **Stop the soak, do not flip** if any nightly between the start and the
  flip date shows a new crash signature, a new conformance failure, a
  patch-apply failure, or a red P0-17 gate: fix it, record it in the ODD,
  and the soak window restarts from the fixed run (new flip date = fix
  date + 14 days).
- **Flip (P1-30.3)** on/after **2026-12-12**, when ≥14 days of nightlies
  show no new crash signatures and no new failures: flip the default to
  `OBS_ENABLE_RUST=ON` (`CMakeLists.txt`, `CMakePresets.json`) and update
  the docs.

## Run log

| #   | Date       | Trigger                    | Result                                                                                                                                                                              |
| --- | ---------- | -------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 1   | 2026-11-28 | manual `workflow_dispatch` | FAILED in 19s — patch path bug: `git -C obs-studio apply` resolved the patch relative to the obs-studio dir. Fixed in `nightly.yml` (absolute `$GITHUB_WORKSPACE` path); see run 2. |
| 2   | 2026-11-28 | manual `workflow_dispatch` | FAILED same 19s bug — dispatched from `main` before the fix landed (fix rode in with PR #90). See run 3.                                                                            |
| 3   | 2026-11-28 | manual `workflow_dispatch` | [run 46130532450](https://github.com/ripmav/obs-rust/actions/runs/46130532450) (in progress — first run with the patch-path fix)                                                    |
