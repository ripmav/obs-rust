# obs-properties (S-015) — test matrix

Crate: `crates/obs-properties` (WP-13, P1-27). Spec: `docs/rust-rewrite/specs/S-015-obs-properties.md`.
Conformance vectors: `test/conformance/scripts/props/01…25` (runner: C-checkout dependent).

The property tree is an `Rc<RefCell<…>>` structure: containers and properties hold
strong `Rc`s, parent/owner links are `Weak` (no reference cycles). Handles are
`Rc`-cloning value wrappers, so they stay valid across removal of other
properties (D4 class). The `settings` object is the opaque `SettingsHandle`
(`From<&ObsData>` identity token; `obs-properties` never reads the data).

| Area | Pinned by test | Conformance vector |
| --- | --- | --- |
| **Container** | | |
| create/destroy order (param after outer, before properties) | `v02_param_and_destroy_order` | 02 |
| ordering, flags get/set, param get/set | `v01_container_basics`, `v25_misc_edges` | 01, 25 |
| first/next iteration | `v01_container_basics` | 01 |
| lookup: direct children only, groups not searched; case-sensitive; byte-wise | `v03_lookup_duplicates_remove`, `v25_misc_edges` | 03, 25 |
| duplicate names: first `add_*` wins (R09, log not wired yet) | `v03…`, `v10_list_creation_rules` (R09 priority over editable refusal) | 03, 10 |
| remove by name: direct children only; groups not recursed | `v03_lookup_duplicates_remove` | 03 |
| group add: rejects self/descendant/already-grouped (R10), cycle check, silent NULL on Q-2 | `v14_groups`, `v15_group_rejections` | 14, 15 |
| group removal detaches content from the tree | `v16_group_remove` | 16 |
| group content access + wrong kind | `v14_groups`, `v20_wrong_kind_and_null` | 14, 20 |
| quirks: Q-1 (duplicate check direct children), Q-6 (property survives orphan removal) | `v21_quirks` | 21 |
| orphan property handles stay valid | `orphan_property_handle_stays_valid` (D4) | — |
| **Simple kinds** | | |
| add_bool/int/float/text/path/button + duplicate rejection | `v04_simple_kinds`, `v20_wrong_kind_and_null` | 04, 20 |
| int/float limits, sliders, suffix | `v05_int_float` | 05 |
| text types, list/edit, defaults | `v06_text` | 06 |
| path filters (FILE/FILE_SAVE stored; DIRECTORY and out-of-range discarded, Q-1), editable list | `v07_path_editable_list` | 07 |
| button + button2, url type (R18; non-button → Some("")) | `v11_button`, `v12_button_url_type` | 11, 12 |
| description/long_description (empty == NULL, R15) | `v25_misc_edges` | 25 |
| visible/enabled flags (R17) | `v25_misc_edges`, `v22_group_content_adds` | 25, 22 |
| name/desc/long/type accessors, wrong-kind defaults | `v04_simple_kinds`, `v20_wrong_kind_and_null` | 04, 20 |
| string ownership: getter copies | `v19_string_ownership` | 19 |
| **List (R27–R33)** | | |
| string list: add/insert/remove/clear/disable/item access | `v08_list_string` | 08 |
| int/float/bool lists, wrong-format no-op returning 0 (Q-3) | `v09_list_int_float_bool` | 09 |
| creation rules: editable refusal, invalid/out-of-range accepted | `v10_list_creation_rules` | 10 |
| **Frame rate (R36–R40)** | | |
| options + ranges, add/insert/count/getters/clears, zero denominators stored | `v13_frame_rate` | 13 |
| **Callbacks (R16–R19)** | | |
| modified v1/v2, return value pass-through, v1 wins (Q-5) | `v17_modified_callbacks` | 17 |
| nested callbacks see the TOP-most container | `v17_modified_callbacks` | 17 |
| modified2 priv (incl. NULL) | `v17_modified_callbacks` | 17 |
| apply_settings order: reverse of depth-first pre-gather (Q-8), applied container passed | `v18_apply_settings_order` | 18 |
| button click: context data vs priv, non-button false | `v11_button_clicks` | 11 |
| callbacks edit the layout (add/remove/hide/disable/re-describe, list refill) | `v23_callbacks_edit_layout` | 23 |
| shared priv slot between button2 and modified2 (Q-4) | `v24_priv_sharing_quirk` | 24 |
| group content additions findable from the top | `v22_group_content_adds` | 22 |
| D-6: property removed during apply is skipped | `d6_remove_during_apply_skipped` | — |

## Not covered yet (C-checkout dependent / later crates)

- The 25 conformance scripts themselves (runner needs `../obs-studio`).
- The two logged messages of R09 (duplicate) and R27 (editable combo refusal):
  no logger in this crate yet (platform crate, S-023); the rejections are
  pinned.
- cbindgen header diff vs `libobs/obs-properties.h`; `OBS_ENABLE_RUST` build
  switch; fuzz/golden vs C.

## Documented deviations

- C `NULL` container/property/name arguments are unrepresentable in the safe
  Rust API (noted per spec function).
- String getters return owned copies; the C `const char *` stability into the
  property is an FFI concern.
- `usize::MAX` encodes the C `(size_t)-1` return of the frame-rate add
  functions on a non-FRAME_RATE property.
