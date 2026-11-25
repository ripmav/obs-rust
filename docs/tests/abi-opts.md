# abi_opts (S-024 M1, P1-33.5) — test matrix

Test files: `crates/obs-ffi/src/abi_opts.rs`
Issue: WP-34 (P1-33.5)

C-ABI bridge `obs_parse_options` / `obs_free_options` in
`crates/obs-ffi` (S-024 M1). Wraps `obs_util_small::opts_parser` with the
C ownership layout: `name`s are separate `bmem` copies, `value` pointers
point *into* `input_words` (one `bmem` block, released by
`obs_free_options`), `#[repr(C)]` mirrors of the reference's
`struct option` / result layout.

| Area                                                                                                                | Pinned by test       | Conformance vector                           |
| ------------------------------------------------------------------------------------------------------------------- | -------------------- | -------------------------------------------- |
| **R01: zero result**                                                                                                |                      |                                              |
| empty input → result with count 0, NULL-free layout                                                                 | `r01_zero_result`    | `helpers/opts-empty`                         |
| **R02/R04: mixed options and ignored words**                                                                        |                      |                                              |
| `a=1 flag b=2 =x c= d=4` → 3 options, 3 ignored; `options`/`ignored_words`/`input_words` non-NULL; entries readable | `r02_r04_mixed`      | `helpers/opts-basic`, `helpers/opts-ignored` |
| **R07: all-ignored input**                                                                                          |                      |                                              |
| `only words` → count 0 but `options` non-NULL (2 ignored)                                                           | `r07_all_ignored`    | `helpers/opts-ignored`                       |
| **R03: value keeps `=`**                                                                                            |                      |                                              |
| `eq=a=b==c` → one option, name `eq`, value `a=b==c`                                                                 | `r03_value_keeps_eq` | `helpers/opts-basic`                         |
| **Q-1: no quoting**                                                                                                 |                      |                                              |
| verbatim value bytes across the C boundary                                                                          | `q1_no_quoting`      | `helpers/opts-noquoting`                     |
