# Rewrite status review: `main` vs. the rewrite plan (2026-11-21)

Reviewed: `main` at `0b65196` (re-run after pulling the merged PRs #52 to #57 and the P1-05 threading work; first pass was at `3445e75`) against `docs/rust-rewrite-plan.md`, the phase files and
`subtasks/phase-*-subtasks.md`. Scope is Phase 0 and Phase 1; the rest of the plan has only specs and no code.

Legend: DONE = implemented on `main` and verified here. PARTIAL = part implemented. OPEN = not started.
BLOCKED = needs the C reference tree. IN PR = implemented on an unmerged branch.

## 1. Verification run (toolchain Rust 1.100.0, Linux)

| Check                                                                                              | Result                                                                                                            |
| -------------------------------------------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------- |
| `cargo fmt --check`                                                                                | pass                                                                                                              |
| `cargo clippy --workspace --all-targets -- -D warnings`                                            | pass                                                                                                              |
| `cargo test --workspace`                                                                           | pass (502 tests, 0 failed)                                                                                        |
| `cargo clippy -p obs-ffi --features c-swap -D warnings`, `cargo test -p obs-ffi --features c-swap` | pass (not covered by CI, see finding 16)                                                                          |
| `cargo deny check` / `cargo audit`                                                                 | not run locally (tools not installed); CI runs them                                                               |
| Miri                                                                                               | not run locally (only the stable toolchain was updated); CI runs it                                               |
| `test/conformance/run.sh check`                                                                    | fails: no `libobs/` link; the C checkout (`../obs-studio`, see `tools/link-obs-source.sh`) is not on this machine |
| Windows / macOS                                                                                    | not verifiable here; CI matrix exists                                                                             |

Note: the local default toolchain was 1.90, below the workspace `rust-version = "1.99"`. Updating stable to 1.100 fixed it.

## 2. Summary

| Phase            | Subtasks done / open | Real state on `main`                                                                                                                                                                                                                                                                                                                    |
| ---------------- | -------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0 Foundation     | 29 / 57              | ADRs 1, 2, 5 and 6 written, workspace, CI, deny, golden crate, issue template and test-matrix directory done. ADR 3 and 4 (prototypes), Corrosion, all C-side safety nets and the benchmark harness are open.                                                                                                                           |
| 1 Leaf libraries | 40 / 90              | Much more code exists than the checkboxes show: util, math, signal, data, properties and hotkey names are in Rust. Lexers, cf-lexer/parser, text-lookup, FFI bridges and threading (POSIX) landed since the first pass. Time/sleep, serializers, SIMD, typed signals, shared helpers and the cbindgen/CI integration are still missing. |
| 2 to 7           | 35 / 853             | Only spec-writing subtasks are ticked. No code.                                                                                                                                                                                                                                                                                         |

Headline problems (details in section 5):

1. Subtask checkboxes are still behind the code (about 50 Phase 1 subtasks are open although their code is on `main`; e.g. P1-05.2 semaphores/thread names).
1. ADR 0005 is violated and got worse: 790 `unsafe` uses workspace-wide (was 360), 38 `// SAFETY:` comments, no lint enforcing it. The new `obs-ffi` bridges (about 400 `unsafe`, 1 SAFETY comment) are inside the allowed crate but still break the comment rule.
1. The plan's status line still says "Nothing implemented yet".
1. Every verification step that compares against C is blocked or unrun, so "matches C" is unproven for all of it.

## 3. Phase 0: Foundation

### A. Decisions

| Task                            | Status            | Notes                                                                                                           |
| ------------------------------- | ----------------- | --------------------------------------------------------------------------------------------------------------- |
| P0-01 ADR migration strategy    | DONE, review open | `0001` exists; P0-01.5 (two-maintainer review) open.                                                            |
| P0-02 ADR plugin ABI            | DONE, partial     | `0002` exists; P0-02.1 frozen header list and P0-02.4 contributor rule open.                                    |
| P0-03 ADR graphics (wgpu spike) | OPEN              | No `0003`, no prototypes. Blocks Phase 3.                                                                       |
| P0-04 ADR GUI toolkit           | OPEN              | No `0004`, no prototypes. Blocks Phase 6.                                                                       |
| P0-05 ADR unsafe/MSRV/deps      | PARTIAL           | `0005` exists; P0-05.2 MSRV open (value is set to 1.99, rule documented). Rule 1 not honoured by code, see 5.2. |
| P0-06 ADR concurrency           | PARTIAL           | `0006` exists; P0-06.2 mutex table open.                                                                        |

### B. Workspace and CI

| Task                                | Status                | Notes                                                                                                                                                                                                 |
| ----------------------------------- | --------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| P0-07 Workspace skeleton            | DONE, P0-07.4 open    | 13 crates; three-OS build gate is in CI but not ticked.                                                                                                                                               |
| P0-08 Corrosion / `OBS_ENABLE_RUST` | OPEN, BLOCKED         | There is no CMake tree here. PR #56 (`p1/ffi-recipe`) touches this.                                                                                                                                   |
| P0-09 CI pipeline                   | PARTIAL, under-ticked | `.github/workflows/rust.yml` has fmt, clippy, nextest on 3 OSes, deny, audit, Miri, llvm-cov. P0-09.1 to .4 are still open in the file. Required-checks gate (P0-09.5) is a repo setting; unverified. |
| P0-10 cargo-deny                    | DONE                  |                                                                                                                                                                                                       |
| P0-11 `.gitattributes`              | PARTIAL               | File done; fixture audit and Windows verification open.                                                                                                                                               |
| P0-12 Build docs                    | OPEN                  | No `INSTALL` or `docs/rust-rewrite/BUILDING.md`.                                                                                                                                                      |

### C. Safety net

| Task                          | Status        | Notes                                                                                                    |
| ----------------------------- | ------------- | -------------------------------------------------------------------------------------------------------- |
| P0-13 Golden framework        | DONE          | `obs-golden`, one demo golden, matrix.                                                                   |
| P0-14 C reference dumper      | OPEN, BLOCKED | No `obs-ref-dump`, no `test/golden/*.golden` from C. Every "matches C" claim in Phase 1 depends on this. |
| P0-15 ABI conformance plugin  | OPEN          | `test/test-input` exists but is not extended or wired to CI.                                             |
| P0-16 Headless smoke test     | OPEN          |                                                                                                          |
| P0-17 Benchmark harness       | OPEN          | No criterion benches, no `benchmarks.md`, no 2% gate. Also blocks P1-16.4 and P1-19.3.                   |
| P0-18 Scene-collection corpus | OPEN          | No `test/fixtures/`. Blocks P1-09.4 and P1-24.3 (byte-identical round trips).                            |

### D. Tracking

| Task                      | Status  | Notes                                                                                                 |
| ------------------------- | ------- | ----------------------------------------------------------------------------------------------------- |
| P0-19 Labels and template | DONE    | Template present; labels are repo settings (not verified).                                            |
| P0-20 Test matrices       | PARTIAL | 23 files in `docs/tests/`. P0-20.4 CI check that every test file has a matrix is missing.             |
| P0-21 Phase 0 review      | OPEN    | Exit criteria not met: ADR 3 and 4, golden files from C, ABI test and benchmark baseline are missing. |

## 4. Phase 1: Leaf libraries

| Task                            | Status                   | Evidence / gap                                                                                                                                                                                                     |
| ------------------------------- | ------------------------ | ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| P1-01 bmem                      | DONE in code, boxes open | `obs-util/src/bmem.rs`, leak-counter and fatal tests. Alloc counter vs C not compared.                                                                                                                             |
| P1-02 dstr                      | DONE in code, boxes open | `dstr.rs` (1455 lines). The 1M-op cargo-fuzz test vs C is missing. Variadic printf is left to a C shim.                                                                                                            |
| P1-03 darray                    | DONE in code             | Layout tests.                                                                                                                                                                                                      |
| P1-04 deque                     | DONE in code             | Wraparound test vs C not compared.                                                                                                                                                                                 |
| P1-05 threading primitives      | PARTIAL                  | `threading.rs` (events, semaphores, thread names), `abi_threading.rs`, 64x1M stress test on `main`. Unix only: Windows parity (P1-05.3) open and the module is `cfg(unix)`. Boxes P1-05.2 still open.              |
| P1-06 time and sleep            | OPEN                     | No `os_gettime_ns`/`os_sleepto_ns`.                                                                                                                                                                                |
| P1-07 filesystem                | PARTIAL                  | `os_fs.rs` is 122 lines; the subtasks list open/rename/mkdir, directory iteration and config paths. Long-path and Windows coverage missing. Module is POSIX-only.                                                  |
| P1-08 text and lexers           | DONE in code             | `utf8.rs`, `lexer.rs`, `cf_lexer.rs`, `cf_preprocessor.rs`, `cf_parser.rs`, `text_lookup.rs` and the `abi_*` bridges are on `main` (PRs #53 to #56 merged). P1-08.1 box open. Golden token dumps vs C still unrun. |
| P1-09 config-file               | PARTIAL                  | `config.rs` done (parser, typed API, atomic save). Round trip of 10 real INI files (P1-09.4) blocked on the corpus.                                                                                                |
| P1-10 crc32 / blake2            | DONE in code             | `hash.rs` with `crc32fast` and `blake2`. Boxes open.                                                                                                                                                               |
| P1-11 dl and process            | DONE                     | `dl.rs`, `os_proc.rs`, `pipe.rs`, `process_args.rs`. POSIX only; Windows is missing.                                                                                                                               |
| P1-12 profiler                  | DONE in code             | `util-small/profiler.rs`. Subtasks P1-12.1 to .3 still open.                                                                                                                                                       |
| P1-13 logging                   | PARTIAL                  | Shim in `obs-ffi/c/base-logging.c` plus Rust side. Crash hook (P1-13.2) and `log` facade (P1-13.3) open.                                                                                                           |
| P1-14 misc util                 | PARTIAL                  | uint128 and task queue done (P1-14.3). Serializers and bitstream (P1-14.2), inventory (.1) and leftovers doc (.4) open.                                                                                            |
| P1-15 vec2/3/4                  | DONE in code             | 10k goldens vs C (P1-15.3) not done.                                                                                                                                                                               |
| P1-16 matrix3/4                 | DONE in code             | Benchmarks (P1-16.4) open.                                                                                                                                                                                         |
| P1-17 quat/axisang/plane/bounds | DONE in code             | `quat.rs`, `axisang.rs`, `plane.rs`, `bounds.rs` exist; P1-17.1, .2, .4 still open.                                                                                                                                |
| P1-18 math-extra/half/srgb      | DONE in code             |                                                                                                                                                                                                                    |
| P1-19 SIMD                      | OPEN                     | No `std::arch`/`std::simd` anywhere.                                                                                                                                                                               |
| P1-20 calldata                  | DONE in code             | Fuzz test missing.                                                                                                                                                                                                 |
| P1-21 decl and proc             | DONE in code             | 50 declaration goldens vs C missing.                                                                                                                                                                               |
| P1-22 signal                    | PARTIAL                  | Handler and emit done. Thread tests (P1-22.3) and `loom` missing (no `loom` dependency).                                                                                                                           |
| P1-23 typed `Signal<Args>`      | OPEN                     |                                                                                                                                                                                                                    |
| P1-24 JSON engine               | PARTIAL                  | `obs-data/src/json.rs`; formatting (24.2) and corpus round trip (24.3) open.                                                                                                                                       |
| P1-25 `obs_data` core           | PARTIAL                  | Core done; reference dump vs C (P1-25.5) open.                                                                                                                                                                     |
| P1-26 refcount under Miri/ASan  | OPEN                     | Miri runs in CI on `--lib`; no ASan job; no dedicated refcount corpus.                                                                                                                                             |
| P1-27 obs_properties            | PARTIAL                  | All kinds implemented; ABI plugin test (P1-27.5) needs P0-15.                                                                                                                                                      |
| P1-28 hotkey names              | PARTIAL                  | `obs-hotkey-names` done; combo JSON (28.2) and golden (28.3) open.                                                                                                                                                 |
| P1-33 shared leaf helpers       | OPEN                     | bpm, file-updater, happy-eyeballs, inject-library and opts-parser have no crate.                                                                                                                                   |
| P1-29 cbindgen header diff      | OPEN                     | No cbindgen config, no CI step.                                                                                                                                                                                    |
| P1-30 switch default            | OPEN                     | Depends on `OBS_ENABLE_RUST` (P0-08).                                                                                                                                                                              |
| P1-31 vendored deps scan        | OPEN                     |                                                                                                                                                                                                                    |
| P1-32 Phase 1 review            | OPEN                     |                                                                                                                                                                                                                    |

Not started crates: `obs-core`, `obs-graphics`, `obs-media-io` are 11-line stubs; `obs-cli` is a 14-line stub.

## 5. Findings

### Blockers

1. **[blocker] ADR 0005 Rule 1 is not met.** The ADR says a pure crate with any `unsafe` fails review; `obs-util`, `obs-signal` and `obs-properties` contain `unsafe` (`dstr.rs` 73, `signal.rs` 48, `bmem.rs` 43, `darray.rs` 35, `deque.rs` 29, `threading.rs` 28, `calldata.rs` 18, `pipe.rs` 18). The same ADR requires a `// SAFETY:` comment on each block, but `dstr`, `darray`, `deque`, `signal`, `calldata`, `decl`, `proc` and every `obs-ffi` `abi_*` module except `abi_common` have none (38 SAFETY comments for 790 `unsafe` uses workspace-wide; `abi_cf_lexer.rs` alone has 218 `unsafe`, 0 comments). Fix: either amend the ADR to allow a layout-compatible-C-types exception for these modules (my recommendation, since the plan requires C-layout `Dstr`/`DArray`/`Deque`), or move the raw-pointer layer to `obs-ffi`. Either way enable `clippy::undocumented_unsafe_blocks` in `[workspace.lints.clippy]` (there is no clippy lint section today) and add the missing comments.
1. **[blocker] Nothing is compared against C.** `P0-14` (reference dumper) and the conformance runner need the OBS checkout linked into the repo (`tools/link-obs-source.sh`). #52 only recorded the checkout in the ODD log (it pointed to `/home/ripmav/projects/obs-studio`, which does not exist on this machine), so `run.sh check` still fails. Phase 1 exit criteria ("goldens ... pass") cannot be met until the C reference is available and CI (or a documented local run) executes the conformance suite against the Rust swap build.

### Major

3. **[major] Checkbox drift.** About 50 Phase 1 subtasks and several Phase 0 ones (P0-09.1 to .4, P0-07.4, P1-05.2) are open though the code is on `main`; the phase files (`phase-0-foundation.md`, `phase-1-leaf-libraries.md`) have no ticked task at all. This breaks the rule "tick in the same PR". Fix: one `docs:` PR that ticks verified items. Use section 4 as the list.
1. **[major] Stale plan header.** `docs/rust-rewrite-plan.md:3` says "Nothing implemented yet". `crates/obs-util/src/lib.rs:1-4` still says "Phase 0 skeleton ... only proves the workspace builds" while the crate has ~10 modules. Update both.
1. **[major] Platform gaps.** `dl`, `pipe`, `os_proc` and now `threading` are POSIX-only (`lib.rs` comments); Windows variants are absent but P1-05, P1-07 and P1-11 require three-OS behaviour. CI is green on Windows only because the modules are cfg'd out.
1. **[major] No coverage of the "keep going" Phase 1 integration chain.** cbindgen config, header diff CI (P1-29), `OBS_ENABLE_RUST` (P0-08) and benchmarks (P0-17) all missing, so P1-30 to P1-32 cannot start.
1. **[resolved] Unmerged work.** PRs #52 to #57 are now merged and no PR is open. Stale `p1/*` remote branches can be deleted.
1. **[major] Phase 0 exit criteria unmet** (ADR 3 and 4, C goldens, ABI plugin test, benchmark baseline). Phase 1 started anyway; record that as an explicit decision or finish the Phase 0 items.

### Minor

9. **[minor] `unwrap()`/`expect()` in non-test library code**: `profiler.rs` 20, `task_queue.rs` 15, `json.rs` 8, `signal.rs` 6, `bmem.rs` 4, `config.rs` 3, `os_str.rs` 3, `dl.rs` 2. Check each is a justified invariant (lock poisoning is probably fine) and consider `clippy::unwrap_used` outside tests.
1. **[minor] Test run artefacts committed** in `test/conformance/` root: `a.txt`, `b.txt`, `bad.txt`, `bom.txt`, `e.txt`, `log.txt`, `new.txt`, `nul-byte.txt`, `u.txt`, `w.txt`, `mp4-out.bin`, `serial-*`. They look like outputs of the runner; confirm and gitignore or move into `vectors/`.
1. **[minor] `[workspace.dependencies]` is empty**; versions are repeated per crate (`libc`, `parking_lot`, `crc32fast`, `blake2`, `cc`). Centralise them.
1. **[minor] Miri scope.** CI runs Miri only on `--lib` for four crates, which skips the re-exec tests and `obs-properties`/`obs-util-small`/`obs-hotkey-names`. Add those crates.
1. **[minor] `CODEOWNERS`** uses a placeholder team `@obs-rust/abi` (the file says so); set real handles.
1. **[minor] `docs/rust-rewrite/README.md`** says "56 specs written, S-001...S-060" but `specs/` holds 81 files up to S-087; update counts.
1. **[minor] `cargo deny`, `cargo audit` and Miri** were not run in this review; they need to be confirmed green in CI on `main`.

### Added in the second pass

16. **[major] The `c-swap` configuration is untested in CI.** The `abi_*` bridges (about 400 `unsafe`) compile only with `cfg(test)` or `--features c-swap` (introduced while fixing the red PR #56). CI runs plain `cargo clippy/test --workspace --all-targets`, so non-test builds of the bridges are never linted, and nothing builds the staticlib with the feature or links it against C. I ran clippy and tests with the feature locally and both pass. Add a CI step for `--features c-swap`; `OBS_ENABLE_RUST=ON` must pass it (P0-08 not started).
01. **[minor] `threading.rs` has 10 `unwrap`/`expect` in non-test code and `cf_preprocessor.rs` 3** (see finding 9); confirm they are lock-poisoning invariants.
01. **[minor] P1-06 (time and sleep) is the next missing util piece** and nothing in `obs-util` implements `os_gettime_ns`/`os_sleepto_ns` yet (only the profiler references the names).

## 6. Recommended next steps (in order)

1. Link the OBS checkout (`tools/link-obs-source.sh`), run `test/conformance/run.sh check`, and wire it into CI. Unblocks P0-14, P0-15 and the "vs C" subtasks.
1. Decide the `unsafe` policy (finding 1): amend ADR 0005 or restructure; add the clippy lint; document existing blocks.
1. Docs PR: tick verified subtasks and tasks, fix the plan header and stale module docs (findings 3, 4, 14).
1. Add the `c-swap` CI step (finding 16) and delete merged `p1/*` remote branches.
1. Implement the missing Phase 1 code: P1-05.3 Windows threading, P1-06 time, P1-14.2 serializers/bitstream, P1-23 typed signals, P1-33 shared helpers, then P1-19 SIMD.
1. Add benchmarks (P0-17), cbindgen diff (P1-29) and the corpus (P0-18); then P1-30 to P1-32.
1. Finish Phase 0 leftovers: ADR 3 and 4 prototypes, build docs, Corrosion, ABI plugin, smoke test.

## 8. Checkbox audit (what was ticked vs. what is true)

Every `[x]` in the Phase 0 and Phase 1 subtask files was checked against the repo, CI and `gh`; the spec-writing ticks of Phases 3, 5 and 6 were checked for the existence of the cited spec files. The "Extend S-0xx" ticks (P1-34, P5-51) were spot-checked, not read line by line.

**Reopened (ticked but not true)**

| ID                                    | Why                                                                                                        |
| ------------------------------------- | ---------------------------------------------------------------------------------------------------------- |
| P5-30.9 (duplicate "WASAPI spec gap") | S-066 does not exist; the entry duplicated the open "Write S-066" subtask                                  |
| P1-34.4                               | cites S-065, which does not exist (S-064 to S-067 are unwritten)                                           |
| P1-05.4, P1-08.5                      | stress test and FFI bridges exist, but the recipe needs the CMake switch (P0-08) and cbindgen diff (P1-29) |
| P1-08.4                               | "loads `en-US.ini`" not verified (no `frontend/` in the tree)                                              |
| P1-11.1                               | `os_dlopen` does not log the R107 error line (documented in `dl.rs`)                                       |
| P1-18.2, P1-18.3                      | no exhaustive 65536-value f16 test, no 256-point sRGB golden                                               |
| P1-27.2, P1-27.3                      | Done-when needs the ABI plugin test (P0-15), absent                                                        |

**Newly ticked (done but not ticked):** P0-07.4, P0-09.1 to .4, P0-11.3, P1-01.1/.2, P1-02.1 to .3, P1-03.1, P1-04.1, P1-05.2, P1-08.1, P1-09.1, P1-10.1/.2, P1-12.1/.2, P1-13.1, P1-17.1/.2, P1-20.1/.2, P1-21.1/.2. Parent tasks ticked: P0-07, P0-10, P0-13, P0-19.

**Fixed while auditing:** P0-06.4's Done-when ("referenced from Phase 2") — P2-12 now links ADR 0006; `obs-util-small` did not inherit `[workspace.lints]`, `edition` or `rust-version` (now does; two clippy findings fixed).

**Ticked and confirmed:** ADR 1/2/5/6 content subtasks, deny.toml, `.gitattributes`, golden framework (including the CI refusal of `UPDATE_GOLDEN`), labels and the 8 phase issues (#2 to #9), issue template, `docs/tests` files, lexer/cf-lexer/text-lookup (tests read the committed conformance vectors), config, hash, process, task queue/uint128, math types and functions, signal handler, obs-data, obs-properties, hotkey names.

**Caveat:** "done" means implemented with passing tests on Linux. Equivalence with C is still unproven (finding 2), and Windows/macOS rely on CI.

## 7. Limits of this review

- Statuses come from the file tree, git history, the checkbox files and a local build/test run, not from reading every function. "DONE in code" means the module exists and tests pass; it does not mean behaviour matches C.
- The standard multi-agent PR review was not run: `main` has no diff against its upstream, so the whole tree was audited by direct inspection instead.
