# Test matrix: obs-golden

- Test file: `crates/obs-golden/src/lib.rs` (unit tests, `#[cfg(test)]`)
- Task: P0-13
- Golden files: `crates/obs-golden/tests/golden/demo.golden`

| Case | Input | Expected | Platforms | Status |
|---|---|---|---|---|
| matching_golden_passes | golden + value identical (str and bytes) | no panic | Linux, Windows, macOS | pass |
| str_mismatch_reports_first_line_and_offset | golden `a\nb\nX\n`, value `a\nb\nY\n` | panic message contains `line 3` and `byte offset`; diff shows `@ around line 3` | Linux, Windows, macOS | pass |
| bytes_mismatch_reports_first_byte | golden `[1,2,3,4,5,9,7]`, value `[1,2,3,4,5,8,7]` | panic message contains `first difference at byte 5`; 16-byte hex context per side | Linux, Windows, macOS | pass |
| update_mode_rewrites_golden | `UPDATE_GOLDEN=1`, `CI` unset, new str + bytes values | files written under GOLDEN_DIR with exact content; summary line printed | Linux, Windows, macOS | pass |
| update_forbidden_in_ci | `UPDATE_GOLDEN=1` and `CI` set | panic `forbidden in CI` (P0-13.2 refusal) | Linux, Windows, macOS | pass |
| missing_golden_says_how_to_create | no golden file, no `UPDATE_GOLDEN` | panic naming the path and `UPDATE_GOLDEN=1` | Linux, Windows, macOS | pass |
| demo_golden | committed `tests/golden/demo.golden` vs `obs-golden demo\n42\n` | no panic (P0-13.4 demo) | Linux, Windows, macOS | pass |

Notes:
- Tests mutate process-global env vars; they serialize on a `Mutex` and each
  uses its own temp dir, so they are order-independent. The env mutations use
  `SAFETY`-documented, mutex-serialized test helpers only (edition 2024 makes
  `env::set_var`/`remove_var` unsafe); product code is unsafe-free.
- CI refusal (P0-13.2) is enforced in-crate: any `cargo test` run with
  `UPDATE_GOLDEN=1` and `CI` set panics. The CI job that runs with `CI` set
  lands with P0-09.
- `GOLDEN_DIR` overrides the default `tests/golden` (cwd-relative; cargo test
  cwd is the crate root). `*.golden -text` from `.gitattributes` (P0-11.1)
  keeps these files byte-stable across OSes.
