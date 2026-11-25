# opts_parser (S-024 M1, P1-33.5) — test matrix

Test files: `crates/obs-util-small/src/opts_parser.rs`
Issue: WP-34 (P1-33.5)

Byte-oriented option-string parser in `crates/obs-util-small` (S-024 M1,
R01–R07, Q-1). Words are separated by the single space character (0x20)
only; a word is an option iff it does not start with `=`, contains `=`,
and has at least one character after the first `=`. No quoting, escaping
or tab handling (Q-1) — everything else is an ignored word.
`parse_options` returns `ParsedOptions` (ordered words + per-word
first-`=` offset).

| Area                                                                                                                  | Pinned by test                | Conformance vector       |
| --------------------------------------------------------------------------------------------------------------------- | ----------------------------- | ------------------------ |
| **R01: zero result**                                                                                                  |                               |                          |
| empty / space-only input → no words, `is_zero`                                                                        | `r01_zero_result`             | `helpers/opts-empty`     |
| **R02/R04: words, options and ignored words**                                                                         |                               |                          |
| space-separated words kept in order; option count + ignored count (`a=b c=d x` → 2 options, 1 ignored)                | `r02_split_and_order`         | `helpers/opts-basic`     |
| words starting with `=` or without `=` are ignored words                                                              | `r04_ignored_and_input_words` | `helpers/opts-ignored`   |
| **R03: option shape**                                                                                                 |                               |                          |
| name = bytes before the first `=`, value = everything after it (further `=` belong to the value: `a==b` → `a` / `=b`) | `r03_option_shape`            | `helpers/opts-basic`     |
| **R05: duplicates kept**                                                                                              |                               |                          |
| repeated keys are separate option entries (no dedup)                                                                  | `r05_duplicates_kept`         | `helpers/opts-basic`     |
| **Q-1: no quoting (R06 bytes)**                                                                                       |                               |                          |
| values with spaces/quotes/`=` bytes pass through verbatim; any byte is valid data                                     | `q1_no_quoting`               | `helpers/opts-noquoting` |
| **first-`=` offset (R03)**                                                                                            |                               |                          |
| `kind` records the byte offset of the first `=` per option word (the C `eq` result field)                             | `eq_offset_first_assign`      | `helpers/opts-empty`     |
