# Test matrix: signal golden ref-dump (P1-25.5)

- Test file: `crates/obs-signal/tests/golden_refdump.rs` (WP-43)
- Task: P1-25.5 Reference dump — calldata compared on the P0-14 corpus
- Golden file: `test/golden/ref-dump/data.golden` (the `# data/calldata`
  section; P0-14, pinned 33.0.1 calldata)

| Case                              | Input                                                                                                                                                                                                                      | Expected                                                                                                                                                      | Platforms          | Status |
| --------------------------------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------ | ------ |
| calldata_matches_c_refdump_golden | the `# data/calldata` section: per-type set/get (bool/int/float/string/ptr/data), raw data, the exact-size get quirk, type-mismatch and unset lookups, overwrite, zero-length data, clear — replayed through the Rust port | every line compared: success flags, values (byte-exact strings, exact ints/bools), the exact-size `get_data` quirk (S-002), the zero-length data special case | Linux (local + CI) | pass   |

Notes:

- The C "call if you know your data is valid" getters (`calldata_int`
  & co.) are `static inline` in calldata.h — the port expresses them as
  `calldata_get_*(…).unwrap_or(default)` (same value).
- The `get_ptr` value line is not address-comparable (the C dumper used
  the literal bit pattern `0xdeadbeef0123`); it is compared on the
  success flag + stored-ness only.
- The obs-data sections of the same golden are compared in
  `crates/obs-data/tests/golden_refdump.rs` (see
  `docs/tests/data-golden-refdump.md`).
- The test always runs: the golden is committed to the repo.
