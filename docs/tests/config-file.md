# config-file — test matrix

Spec: `docs/rust-rewrite/specs/S-007-config-file.md` (+ S-023 R40/R41/R61/R62 for the platform helpers).
C reference: `libobs/util/config-file.c` (789 lines).
Implementation: `crates/obs-util/src/config.rs` (module `config`), `crates/obs-util/src/os_str.rs`, `crates/obs-util/src/os_fs.rs`.

## Platform helpers (S-023 R40/R41, D-2)

| Vector / case | Test | Status |
|---|---|---|
| R40 `os_dtostr` worked values: `0.1` → `0.10000000000000001`, `2` → `2.0`, `-0.0` → `-0.0`, `1e21` → `1e21`, `1.5e-7` → `1.4999999999999999e-7`, `1e16` → `10000000000000000.0`, `1e17` → `1e17`, `0.0001` → `0.0001` | `r40_dtostr_worked_values` | implemented (glibc `%.17g` probe) |
| R40 Q-7: `inf`/`-inf`/`nan` → `inf.0`/`-inf.0`/`nan.0` | `r40_dtostr_worked_values` | implemented |
| R40 exponent shortening: `e+21` → `e21`, `e-05` → `e-5`, `-` kept | `r40_dtostr_worked_values` | implemented |
| D-2 (S-023): the C stale tail (`1e211`, `1.0000000000000001e-55`) is **not** reproduced (fixed branch per P1-09.3 / open question 2); the stale texts are pinned as documented divergences and read back corrupted | `r40_d2_reference_corruption_documented` | implemented |
| R41 `os_strtod` worked values: `0x10` → 16, `0x1p4` → 16, `0x.8` → 0.5, `0x10z`/`0x1p4x` trailing garbage ignored, `0X1F` → 31, `12abc` → 12, `abc`/`""`/`.`/`e5` → 0, `  -12.5 ` → -12.5, `inf`/`INF`/`-infinity`, `NaN`, `1e400` → inf, `1e-400` → 0, `1.2.3` → 1.2, `1e` → 1, `2,5` → 2 (comma is not a dot), `-0x1.8p1` → -3 | `r41_strtod_worked_values` | implemented (glibc `strtod` probe) |
| R41: overflow → ±inf, hex overflow `0x1p1024` → inf, no-conversion `0x` → 0 | `r41_strtod_worked_values` | implemented |
| R41: 63-character cut, the rest ignored | `r41_63_char_truncation` | implemented |

## Parsing (S-007 §4.1, R02, R03, R06, Q-1, Q-5, R-D3)

| Vector / case | Test | Status |
|---|---|---|
| §4.1: sections, `key=value`, comments, top-level junk ignored | `parse_basic_sections`, `parse_top_level_junk_ignored` | implemented |
| R02: empty and BOM-only file → zero sections | `parse_empty_and_bom_only` | implemented |
| Q-4: names keep trailing whitespace, values keep all surrounding whitespace, names may contain spaces | `parse_whitespace_and_names` | implemented |
| §4.1.2: the first `=` splits name/value (`a=b=c` → value `b=c`) | `parse_first_equals_wins` | implemented |
| Q-1: duplicate sections **and** duplicate items stay listed, lookups return the last inserted (scanned across duplicate section records) | `parse_q1_duplicates_last_wins`, `open_defaults_accumulates` | implemented |
| Q-5: a header missing `]` still creates the section | `parse_q5_bad_header_still_creates_section` | implemented |
| R-D3: an empty name (`[]`) stops parsing entirely | `parse_rd3_empty_name_stops_entirely` | implemented |
| §4.1.5: line endings `\n`, `\r\n`, `\r` | `parse_line_endings` | implemented |
| §4.1.3: value unescaping `\\`/`\r`/`\n`; other backslash sequences kept literally; trailing lone backslash kept | `parse_unescape` | implemented |
| §4.1.4: empty value stored as `""` | `parse_empty_value` | implemented |
| R02: `config_open` EXISTING missing → `CONFIG_FILENOTFOUND`; ALWAYS creates empty; existing file parsed; double open → `CONFIG_ERROR` | `create_and_open_file_modes`, `open_string_double_call_errors` | implemented |
| R03: `config_open_defaults` accumulates into the defaults map (later definition of a key wins, keys from earlier calls survive), does not create user values, missing file → `CONFIG_FILENOTFOUND` | `open_defaults_accumulates` | implemented |
| §4.1: `config_open_string` parses exactly like a file and binds **no** file | `open_string_has_no_file`, `parse_*` (all use it) | implemented |
| R06: `config_num_sections` counts user sections; `config_get_section` insertion order, out of range → None | `parse_basic_sections`, `remove_value_user_only` | implemented |
| Invalid UTF-8 → U+FFFD (spec uncharacterised; a Rust `String` cannot carry raw bytes — documented deviation) | (documented in module docs; no vector pins it) | documented |

## Typed API (S-007 R07–R15)

| Vector / case | Test | Status |
|---|---|---|
| R07: set creates section/item appended; replace keeps position; `set_string(NULL)` → `""` | `set_creates_and_replaces_in_place` | implemented |
| R08: `set_int` `%lld`, `set_uint` `%llu`, `set_bool` `true`/`false`, `set_double` the `os_dtostr` (D-2-fixed) text (`2` → `2.0`, `0.1` → `0.10000000000000001`, `-0.0` → `-0.0`, `1e21` → `1e21`) | `set_typed_formats` | implemented |
| R09/Q-3: `set_default_*` store the default; without a user value they also create it (`has_user_value` → true); an existing user value is unchanged; `set_default_double` = `printf("%g")` six significant digits (`1234567.891` → `1.23457e+06`) | `set_default_also_creates_user_value`, `g_sig_worked_values` | implemented |
| R10: `get_string` user → default → None; the borrow keeps the item immutable | `get_falls_back_to_defaults` | implemented |
| R11: `get_int` — empty/missing → 0, lowercase-`0x`-only hex (Q-6: `0X1F` → 0), stop at the first invalid character (`12abc` → 12, `abc` → 0), whitespace + sign, saturation on overflow | `get_int_semantics` | implemented |
| R11: `get_uint` — negative wraps (`-5` → 2⁶⁴−5), positive overflow → `u64::MAX`, negative overflow → 0 | `get_uint_semantics` | implemented |
| R12: `get_bool` — `true` case-insensitively or nonzero uint; `yes`/`0`/`false`/`` missing → false | `get_bool_semantics` | implemented |
| R13: `get_double` — `os_strtod` (`0x10` → 16, `1e400` → inf), missing/unparsable → 0.0 | `get_double_semantics` | implemented |
| R14: `remove_value` removes the user entry (section stays), false when missing; the default survives | `remove_value_user_only` | implemented |
| R15: `has_user_value`/`has_default_value`, missing section/name never faults | `remove_value_user_only`, `set_default_also_creates_user_value` | implemented |

## Saving (S-007 §4.2, R04, R05, Q-2, S-023 R61, Q-9)

| Vector / case | Test | Status |
|---|---|---|
| §4.2: sections in order, blank line between, `[S]\n`, `name=value\n`, no trailing blank line | `save_format` | implemented |
| §4.2: value escaping — backslashes first, then CR → `\r`, LF → `\n`; a literal backslash+r round-trips differently from a real CR | `save_roundtrip_bit_exact` | implemented |
| Round trip: values with `\`, CR, LF, `\`+`r` save and re-parse bit-exactly | `save_roundtrip_bit_exact` | implemented |
| Q-1: both duplicate sections are written | `save_q1_duplicate_sections_both_written` | implemented |
| R04/Q-2: an empty config cannot be saved — file truncated to zero bytes, `CONFIG_ERROR`; no file → `CONFIG_ERROR`; missing parent directory → `CONFIG_FILENOTFOUND` | `save_q2_empty_config_truncates_and_fails` | implemented |
| R05: `save_safe` success — temp written, target → backup, temp → target, temp gone, backup holds the previous contents; a leading `.` in the extension is not doubled | `save_safe_success_with_backup` | implemented |
| R05: empty `backup_ext` → no backup; missing target → created | `save_safe_no_backup_and_new_target` | implemented |
| R05: empty `temp_ext` → `CONFIG_ERROR`, target untouched | `save_safe_empty_temp_ext_fails` | implemented |
| P1-09.3 power-loss simulation: a kill after the temp write (any moment before the final rename) leaves the old target intact — the swap happens only in the atomic rename | `save_safe_power_loss_window` | implemented |
| R61: `os_safe_replace` — backup rename only when the backup is given **and** the target exists; temp → target; 0/-1 | `safe_replace_basic`, `safe_replace_no_existing_target` | implemented |
| R61/Q-9: no rollback — a failing second rename leaves the target already moved to the backup | `safe_replace_q9_no_rollback` | implemented |
| R62: `os_file_exists` — files, directories count; dangling links do not (followed); missing → false | `file_exists` | implemented |
| §4.2: Windows BOM prefix (C-locale platform detail) — not applied on Linux; the platform layer adds it | (documented in module docs) | documented |

## Evidence

- `cargo test -p obs-util --lib`: 120/120 (obs-util lib total after this WP).
- Miri `cargo +nightly-aarch64-unknown-linux-gnu miri test -p obs-util --lib`: green (file-system tests `#[cfg_attr(miri, ignore)]` under Miri isolation).
- `g17`/`os_dtostr`/`os_strtod` verified against a glibc `printf("%.17g")`/`strtod` probe (same machine, same C library the conformance scripts will run against).
- Windows-only behaviour (BOM prefix, `ReplaceFile`-style swap) is out of scope for the Linux port and documented in the module docs.

## Not covered in this WP (C checkout, S2)

- `01-parse-basic` … `11-line-endings-and-escapes` conformance scripts (11) — C side; the Rust tests mirror the spec vectors the scripts encode.
- P1-09.1 "10 real files parse" and P1-09.4 byte-identical corpus round trip — need the C checkout / real config files.
- P1-09.5 apply recipe (CMake exclusion, `obs-ffi` C shim, cbindgen header diff).
