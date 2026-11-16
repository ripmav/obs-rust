# obs-hotkey-names (S-024 M4) — test matrix

Crate: `crates/obs-hotkey-names` (WP-14, P1-28.1). Spec:
`docs/rust-rewrite/specs/S-024-small-helpers.md` §4.4 (M4 hotkey
names), S-034 for the consumer context. The 636 key ids are the
0-based positions in the key list of `obs-hotkeys.h`
(`OBS_KEY_NONE = 0`, `OBS_KEY_LAST_VALUE = 636` is not a key).

| Area | Pinned by test | Conformance vector |
| --- | --- | --- |
| **Table (R17)** | | |
| all 636 entries equal the normative vector, line by line (`to_name` + `from_name`) | `table_matches_normative_vector` | `helpers/hotkeys-fallback-table` |
| round trip `from_name(to_name(i)) == i` for all ids | `round_trip_is_identity` | — |
| spot values (NONE, RETURN, ENTER, HYPER_L, NUMASTERISK, A, EDIAERESIS, MOUSE29, `0x07`, VK_OEM_CLEAR) | `known_names` | `helpers/hotkeys-lookup` |
| no duplicate names in the table | `no_duplicate_names` | — |
| **to_name (R19)** | | |
| out of range (636, 100000, `u32::MAX`) → `""` | `to_name_out_of_range` | `helpers/hotkeys-lookup` |
| **from_name (R18)** | | |
| exact match: NONE→0, A→121, RETURN→1, MOUSE29→461, `0x07`→470 | `from_name_edge_cases` | `helpers/hotkeys-lookup` |
| wrongly-cased, padded, no-prefix, empty, `OBS_KEY_LAST_VALUE`, unknown → 0, no error | `from_name_edge_cases` | `helpers/hotkeys-lookup` |

## Not covered yet (C-checkout dependent / later phases)

- The 4 `helpers/hotkeys-*` scripts through the C runner (needs
  `../obs-studio`).
- The two C lookup paths (lazy hash map with core, linear without) —
  the port has one; the hash path's behaviour (including R21 free +
  rebuild) is not reproducible and not needed (S-024: "a port needs
  one").
- The whole hotkey registry of S-034 (registration, bindings,
  polling, pairs, persistence, translations) — Phase 4 (P4-28).

## Documented deviations

- The S-024-R19 ERROR log for out-of-range `to_name` is not emitted
  yet (no logger in the crate — platform crate, S-023); the `""`
  result is pinned; the FFI layer wires the log line.
- C-negative key ids are unrepresentable in the safe API (`u32`);
  the FFI layer maps them to the same `""` result.
- S-024-R21 (`obs_hotkey_name_map_free`) has no equivalent — there is
  no lazily built map to free.
