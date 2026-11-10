# Test matrices

One matrix per test file and one per issue, per the project rule: "Every test
file gets a test matrix in `docs/tests/`; every issue gets
`docs/tests/I-<n>-<name>.md`."

- File matrices: `docs/tests/<test-name>.md` — one per test file (Rust:
  `crates/*/tests/*.rs`, `crates/*/src/**` unit tests; C: `test_*.c`,
  conformance areas).
- Issue matrices: `docs/tests/I-<n>-<name>.md` — one per GitHub issue
  (P0-19.3 creates the per-phase tracking issues).

## Template

Copy [`_template.md`](\_template.md). Columns: **Case / Input / Expected /
Platforms / Status**.

## Rules

- Every test case the test file exercises appears as a row; a row's `Status`
  is `pass`, `fail`, `xpass`, or `skip(<reason>)`.
- `Platforms` lists Linux / Windows / macOS (or the specific target, e.g.
  `linux-x86_64`) — a case that cannot run on a platform is `skip` there with
  the reason, not silently absent.
- Golden-backed cases name the `.golden` file they compare against.
- Update the matrix in the same PR that changes the test file. The CI check
  (P0-20.4) fails if a `tests/*.rs` or `test_*.c` has no matching matrix.
