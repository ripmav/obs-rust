# Test matrix: data golden ref-dump (P1-25.5)

- Test file: `crates/obs-data/tests/golden_refdump.rs` (WP-43)
- Task: P1-25.5 Reference dump — obs-data compared on the P0-14 corpus
- Golden file: `test/golden/ref-dump/data.golden` (P0-14; pinned 33.0.1
  obs_data + jansson 2.15.1, PRNG seed `0x0B5` splitmix64)

| Case                                               | Input                                                                                                                                            | Expected                                                                                                                                                                                  | Platforms          | Status |
| -------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------ | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------------------ | ------ |
| obs_data_build_matches_c_refdump_golden            | the `# data/obs_data_build` section: build/get/defaults/apply/erase/clear over all types + nested objects/arrays, replayed through the Rust port | every getter compared (ints/bools exact, doubles exact f64, strings byte-exact) and every JSON dump **byte-exact** (S-014 §6.2: sorted compact, jansson-2.15.1 shortest-round-trip reals) | Linux (local + CI) | pass   |
| obs_data_double_roundtrip_matches_c_refdump_golden | the `# data/obs_data_double_roundtrip` section: 32 doubles (values listed in the golden)                                                         | set → JSON text **byte-exact** (the `%.17g`/shortest-real printing the P1-24.2 bar needs) → reparse exact f64 (`roundtrip=1`)                                                             | Linux (local + CI) | pass   |
| obs_data_fixtures_match_c_refdump_golden           | the `# data/obs_data_fixtures` section: the four `test/fixtures/data-dump/*.json` files (P0-18 corpus)                                           | load → sorted JSON **byte-exact** → reparse → sorted JSON **byte-exact**                                                                                                                  | Linux (local + CI) | pass   |

Notes:

- The calldata section of the same golden is compared in
  `crates/obs-signal/tests/golden_refdump.rs` (see
  `docs/tests/signal-golden-refdump.md`).
- The double round-trip section is the corpus verification the P1-24.2
  formatting bar asks for: the Rust `real_fmt` (jansson-2.15.1 shortest
  round-trip real, round-half-even only at the exact midpoint) reproduces
  the C bytes for all 32 values.
- Container JSON is compared byte-exact after the spec's sorted-key
  canonical form (C insertion order is implementation detail, S-014 §6.2).
- The test always runs: the golden and fixtures are committed to the repo.
