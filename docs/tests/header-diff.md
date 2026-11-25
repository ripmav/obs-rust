# header_diff (P1-29.1/.2, WP-31) — test matrix

- Test file / issue: `crates/obs-ffi/tests/header_diff.rs`
- Task: P1-29
- Golden files: none

The test generates the obs-ffi C header with cbindgen (dev-dependency,
config `crates/obs-ffi/cbindgen.toml` — `language = "C"`,
`cpp_compat = true`, no function prefix, exclusion list for the 12
non-bridge declarations). The declaration diff lives in
`test/conformance/cbindgen-header-diff.py` (the script drives this test
for the generation half; see its module docstring for scope,
normalisation and the out-of-scope list).

| Case              | Input                                                                                                                                                                                                                                               | Expected                                                                                                                                                                                                                                                                    | Platforms                         | Status |
| ----------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | --------------------------------- | ------ |
| `generate_header` | the 45 `#[no_mangle] extern "C"` bridges + the `cbindgen.toml` config                                                                                                                                                                               | cbindgen writes the header to `$HDR_DIFF_OUT` (or `target/obs-ffi-generated.h`); the header declares the bridges and the `#[repr(C)]` types they use                                                                                                                        | Linux, Windows, macOS             | pass   |
| header diff       | generated header vs the C reference declarations: `util/text-lookup.h` (4 EXPORT), `util/threading.h` (14), `util/lexer.h` (10 EXPORT), `util/cf-lexer.h` (9 EXPORT), `util/cf-parser.h` (2 EXPORT), `util/platform-nix.c` (6 extern dbus\_/portal) | the mismatch report is empty (45/45 signatures match after whitespace/parameter-name/default-argument normalisation + the opaque-typedef/enum/`size_t` alias table); a deliberate type drift, a removed or added bridge reds the report (negative self-checks done locally) | linux-x86_64 (CI conformance job) | pass   |

Notes: the diff is linux-only by design — the CI step runs in the ubuntu
conformance job with the pinned obs-studio 33.0.1 checkout, and the
reference declarations are the unix ones (`platform-nix.c` has no header
in the 33.0.1 tree; the diff scope is its `GIO_FOUND` extern block).
The Windows/macOS runs of `generate_header` generate only (no libobs
link, no diff).
