# corpus_load_check (P0-18.3, WP-42) — test matrix

- Test file / issue: `crates/obs-data/tests/corpus_load_check.rs`
- Task: P0-18.3
- Golden files: none

The scene-collection corpus (`test/fixtures/scenes/`, 24 files: 6 public +
18 synthetic per D4) must parse without errors through the `obs-data`
crate. The test globs the directory, asserts ≥ 20 files, and parses each
with `obs_data::io::data_create_from_json_file` (the Rust port of the same
`obs_data_create_from_json_file` the 33.0.1 frontend uses to load
collections); per file it also probes the `name` string and the `sources`
array. The committed evidence log is
[`test/fixtures/LOAD-CHECK.md`](../../test/fixtures/LOAD-CHECK.md).

| Case                 | Input                                          | Expected                                                                                                                                             | Platforms             | Status |
| -------------------- | ---------------------------------------------- | ---------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------- | ------ |
| `corpus_files_parse` | every `test/fixtures/scenes/*.json` (24 files) | all parse (`Some(ObsData)`), the ≥ 20-file assertion holds, and the per-file probe lines print `sources=present` for all 24 (24/24 in LOAD-CHECK.md) | Linux, Windows, macOS | pass   |

Notes: the corpus spans three file generations (33.0.1 version-2 relative,
29–32-era version-1 absolute, and pre-29 legacy source/item shapes) — a
parse failure in any generation is a load-path regression. A parse failure
panics with the file name (P0-18.3 "opens/parses without errors").
