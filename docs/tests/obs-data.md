# obs-data — test matrix

Spec: `docs/rust-rewrite/specs/S-014-obs-data.md` (+ S-023 R57/R58/R60/R61 for the file helpers).
C reference: `libobs/obs-data.c` (2181 lines) with jansson 2.15.
Implementation: `crates/obs-data` (modules `json`, `model`, `api`, `io`).

## JSON engine (S-014 §6.2/§6.3, P1-24.1/.2)

| Vector / case | Test | Status |
|---|---|---|
| P1-24.1 decision: `serde_json` rejected (duplicate keys, `1e400`, unbounded depth and `\u0000` accepted — none of which jansson does); hand-rolled jansson 2.15 port; the differences note is the `json` module docs | `json.rs` docs | implemented |
| §6.3: top level must be object or array (`5`, `"x"`, `true`, `null`, empty → error); trailing non-whitespace → error | `json::tests::parse_*`, `reader_errors` | implemented |
| §6.3: duplicate keys at any depth → error (byte-wise) | `json::tests::parse_duplicate_keys_error` | implemented |
| §6.3 numbers: leading `+`, `.5`, `1.`, `01`, hex, `NaN`, `Infinity` → error; integer overflow (`9223372036854775808`, `-9223372036854775809`) → **error** (not converted to real); real overflow `1e400` → error; underflow `1e-400` → `0.0` sign kept (`-1e-400` → `-0.0`) | `json::tests::parse_number_errors`, `reader_errors` | implemented |
| §6.3: `-0` is the integer 0, `-0.0` the real −0 | `json::tests::parse_number_errors` | implemented |
| §6.3: literals exactly lower-case, delimited (`nul`, `True`, `truex` → error) | `json::tests::parse_*` | implemented |
| §6.3 strings: raw control `< 0x20` → error; `\" \\ \/ \b \f \n \r \t \uXXXX`; any other escape → error; `\u0000` → error (also in keys); lone/misordered surrogate → error; raw bytes must be well-formed UTF-8 (no overlongs, surrogates, `> U+10FFFF`) | `json::tests::parse_strings_and_escapes` | implemented |
| §6.3: depth limit 2047 (a 2048-deep chain fails, 2047 succeeds) | `json::tests::parse_depth_limit` | implemented |
| §6.2 writer: compact layout `{"a":1,"b":[{"c":2}]}`; pretty `4 × depth` indent, `{}, []` empty, no trailing newline; same tokens in both | `json::tests::render_*`, `r80_pretty_layout` | implemented |
| §6.2 reals: shortest round trip, `-4 ≤ x < 16` positional else scientific (`1e16`, `1.5e300`, `1e-5`, `5e-324`, `1.7976931348623157e308`), no `+`, no leading exponent zeros, `0` → `0.0` | `json::tests::render_*` (pinned against `test/conformance/vectors/obsdata/12-json-write.expected`, `13-json-pretty.expected`) | implemented |
| §6.2 strings: `\" \\ \b \f \n \r \t`, other `< 0x20` → `\u00XX` upper-case hex; `/`, `0x7F`, U+2028/2029, non-ASCII raw | `json::tests::render_string_escapes` | implemented |
| Q5: non-finite reals and invalid UTF-8 keys/values omit the member entirely, no error | `q5_non_finite_real_omitted`, `json::tests` | implemented |

## Model / reference counting (S-014 §4, R01–R05, D1)

| Vector / case | Test | Status |
|---|---|---|
| R01: create = 1, clone = addref, drop at 0 destroys (releasing every object/array value of its layers) | `r01_create_addref_release` | implemented |
| R02: getters return a **new** reference (the stored reference is separate) | `r02_getters_return_new_reference`, `r02_get_obj_returns_new_reference` | implemented |
| R03: setters take a reference; replacing releases the old value's reference | `r03_setters_take_a_reference_replace_releases` | implemented |
| R04: erase/clear/unset release references | `r04_erase_clear_unset_release` | implemented |
| D1 (DEFECT fix): `set_default_obj` on an existing item without a user layer **does** take the reference (the C undercounts) | `d1_default_object_refcount_fixed` | implemented |
| R26: `set_obj(…, None)` stores a **present** NULL object: `has_user_value` true, `get_obj` None, no fallback to a default; arrays likewise | `r26_present_null_object` | implemented |

## §6.1 list order / growth (QUIRK Q1)

| Vector / case | Test | Status |
|---|---|---|
| Q1: user-only growth moves the item to the end | `q1_user_only_growth_moves` | implemented |
| Q1: adding a default to a user item aligns the user region to 32 (`A(5)=32`, need 37 > 5) → move; int over int (same size) → no move | `q1_growth_moves_item_to_end` | implemented |
| Q1: reverse order (default first, then user) moves | `q1_reverse_order_user_after_default` | implemented |
| R11: an ignored default changes nothing (no size change, no move, nothing stored) | `q1_r11_ignored_default_changes_nothing` | implemented |
| D4 (DEFECT fix): growth cannot dangle a handle — item storage is `Rc`-shared, all handles stay valid, the item is last | `d4_growth_does_not_dangle_handles` | implemented |

## Items / iteration (S-014 §5.8, R40–R43)

| Vector / case | Test | Status |
|---|---|---|
| R40: `first`/`byname`/`next`/`release`; empty object → None; `*it` cleared at the end; NULL handle no-op | `r40_first_byname_next_release` | implemented |
| R41: an append during a walk is visited | `r41_append_during_walk_visited` | implemented |
| R41: an erase during a walk continues with the successor | `r41_erase_during_walk_continues` | implemented |
| R41: a detached handle walks to the item that followed at detach time | `r41_detached_handle_walk_continues` | implemented |
| R42: a detached item is destroyed with its last handle (releasing its values' references) | `r42_detached_item_destroyed_with_last_handle` | implemented |
| R25: `gettype`/`numtype`/`get_name` (numtype of the **effective** value; shell → Invalid) | `r25_gettype_numtype_name` | implemented |
| R13: item setters = name-based semantics, no creation; R11 type check on defaults; a type change overwrites | `r13_item_set_semantics` | implemented |
| R30: unset leaves a shell (type and position kept, reads zero) | `r30_unset_leaves_shell` | implemented |
| R31: clear keeps default/autoselect layers; erase keeps handles valid | `r31_clear_keeps_defaults_and_autoselect`, `r41_*` | implemented |

## Arrays (S-014 §5.9, R50–R53)

| Vector / case | Test | Status |
|---|---|---|
| R50/R52: push_back/insert/item/erase with +1 reference per stored element; out-of-range ignored (D6, no corruption) | `r50_r52_array_lifecycle` | implemented |
| R52: `push_back_array` shares the elements | `r50_r52_array_lifecycle` | implemented |
| R53: enum calls the callback per element in order, no net reference change | `r53_array_enum_shares_elements` | implemented |

## Typed API (S-014 §5.2–§5.5, §5.10–§5.11)

| Vector / case | Test | Status |
|---|---|---|
| R20: missing name / type mismatch / absent effective value → the zero value of K (`""`, 0, 0.0, false, NULL obj/arr) | `r20_zero_values_and_type_mismatch` | implemented |
| R22/D5: double → int truncates toward zero (`-2.9` → `-2`, `3000000000.75` → `3000000000`); int → double round-to-nearest (`9007199254740993` → `…992.0`); out-of-range/NaN saturate | `r22_number_conversion` | implemented |
| uint: stored as the 64 bits of an int64 (the C union overlap), round-trip unchanged, negative int reads as the two's-complement bits | `r22_number_conversion` | implemented |
| R10: a set with a different type changes the item's type, keeps one item | `r10_type_change_replaces` | implemented |
| R11: default type check (ignored on mismatch, creates nothing; a fresh item gets the default) | `r11_default_type_check` | implemented |
| R12: autoselect layer (creates, overwrites the type without checking) | `r12_autoselect_layer` | implemented |
| R24/R30: `has_*` per layer (a present NULL counts); unset per layer independently, shell stays | `r30_unset_layers_independently`, `r26_present_null_object` | implemented |
| R60/R61: vec2/vec3/vec4/quat stored as a new object with double members in order; getters narrow to f32, untouched without an object value (other type, NULL object), missing members read 0, int members convert; the C vec3 has no `w` (untouched) | `r60_r61_vec_quat` | implemented |
| R65/R66: fps `{numerator,denominator}` doubles in order or `{option}` (fps ignored); reads: option member (any type → its `get_string`) wins with `fps` untouched; missing members → false untouched; clamp to `[0, 4294967295]` stored even when the result is false; result `num != 0 && den != 0` | `r65_r66_frames_per_second` | implemented |
| R13 item forms: the name-based semantics on the handle's item, never creating one; the handle follows a grown item | `item_typed_api` | implemented |
| R32 `apply`: deep copy of user layers only (nested objects/arrays rebuilt, NULL members dropped, nothing shared, source untouched, same-object no-op) | `r32_apply_copies_user_layers_deep` | implemented |
| R33 `get_defaults`: list order, string → default or `""`, number per the **effective** number type (shell → no member), objects/arrays recursive, int↔double defaults convert | `r33_get_defaults`, `apply_and_defaults_roundtrip` | implemented |

## JSON entry points / files (S-014 §6.4–§6.5, R70–R83)

| Vector / case | Test | Status |
|---|---|---|
| R80: `get_json` compact user-only (default-only items skipped); empty object → `{}` | `r80_get_json_user_only` | implemented |
| R80: pretty layout (4-space indent, `{} ` empty) | `r80_pretty_layout` | implemented |
| R82: with-defaults writes the effective value; shells as `""`, `0.0`, `false`, `null`, `[]`; a default object written recursively with defaults | `r82_defaults_and_shells` | implemented |
| R81: `get_last_json` = the buffer last produced (None before any call) | `r81_last_json` | implemented |
| Q2: a top-level array parses to a valid **empty** object | `q2_q3_reader_quirks` | implemented |
| Q3: array elements that are not objects are silently dropped | `q2_q3_reader_quirks` | implemented |
| §6.3 table: object/array/string/int/real/bool/null member conversion (`null` → present NULL object) | `q2_q3_reader_quirks`, `r83_roundtrip_keeps_typing` | implemented |
| R83: a re-read reproduces all user values, int/double typing preserved (`2.0` stays a double) | `r83_roundtrip_keeps_typing` | implemented |
| Reader errors: top-level scalars, trailing garbage, duplicate keys, i64 overflow, `1e400`, `01`, `.5`, `1.`, `+5`, `\u0000`, lone surrogate, BOM (not skipped); underflow keeps the sign | `reader_errors` | implemented |
| R70/R57: `create_from_json_file` — missing/empty/invalid → None; one leading BOM stripped | `r70_r72_files` | implemented |
| R72: `save_json` writes compact JSON without a BOM; empty data → `{}`; unwritable path fails cleanly | `r70_r72_files` | implemented |
| R72/R60: `save_json_safe`/`_pretty_safe` write the temp, keep the target as backup, a leading dot is not doubled, an invalid temp extension leaves the target untouched, the temp is gone on success | `r72_safe_save_and_r71_safe_load` | implemented |
| R71: the safe loader uses the backup only when the primary fails, **renames** the backup over the primary (consumed), a good primary leaves the backup | `r72_safe_save_and_r71_safe_load` | implemented |
| Q6: the safe loader consumes the backup even when the primary merely **does not exist** | `r72_safe_save_and_r71_safe_load` | implemented |
| R58: `os_quick_write_utf8_file` — BOM marker, zero-length write, binary | `r60_marker_and_len_zero` | implemented |
| R61: `os_safe_replace` (including Q-9 no rollback) | WP-11 `os_fs` tests (`crates/obs-util/src/os_fs.rs`) | implemented |

## Not yet covered (C-checkout dependent, P1-24.2/.3, P1-25.5/.6)

- P1-24.2/24.3: byte-identical corpus round trip and the `json*` command diff — needs the conformance runner's C reference (`libobs/obs-data.c` + system jansson 2.15) from the linked checkout.
- P1-25.5: the reference-dump comparison on the corpus (same runner).
- P1-25.6: apply recipe (CMake exclusion of `obs-data.c` when `OBS_ENABLE_RUST=ON`, `obs-ffi` C shim, cbindgen header diff).

Documented deviations (spec gaps): invalid UTF-8 read through the safe API renders lossily (a `String` cannot carry raw bytes; the FFI layer handles bytes); the FFI log lines of the C (LOG_ERROR on parse failure, LOG_WARNING on backup use) are not wired yet (no logger in this crate).
