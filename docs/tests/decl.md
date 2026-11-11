# decl — test matrix

Spec: `docs/rust-rewrite/specs/S-003-declaration-language.md`
C reference: `libobs/callback/decl.c` (90 lines, handwritten scanner).
Implementation: `crates/obs-signal/src/decl.rs`.

## Coverage

| Vector / case | Test | Status |
|---|---|---|
| 01-valid: `void source_create(ptr source)` → 1 IN param | `vec01_valid` | implemented |
| 01-valid: `int add(int a, int b)` → 3 params (`a`, `b`, `return` OUT) | `vec01_valid` | implemented |
| 01-valid: `void f()` → 0 params | `vec01_valid` | implemented |
| 01-valid: string/ptr param types | `vec01_valid` | implemented |
| 02-invalid: `void a` → `Expected 'type'` (R04) | `vec02_invalid` | implemented |
| 02-invalid: duplicate names (R06) | `vec02_invalid` | implemented |
| 02-invalid: double `in`/`out` (R07) | `vec02_invalid` | implemented |
| 02-invalid: reserved function names (R05) | `vec02_invalid` | implemented |
| 02-invalid: missing name / type (R08), EOF, `void f(void)` | `vec02_invalid` | implemented |
| 03-case-and-keywords: `VOID` fails (case-sensitive) | `vec03_case_and_keywords` | implemented |
| 03-case-and-keywords: `IN` is not a specifier; `in`/`out` are names (R05) | `vec03_case_and_keywords` | implemented |
| 04-return-parameter: `int f()` → 1 param `return` OUT (R03) | `vec04_return_parameter` | implemented |
| 04-return-parameter: `in out` / `out in` → both bits; `out` only | `vec04_return_parameter` | implemented |
| 05-lenient-quirks: Q-1 leading WS (space/tab/comment) fails | `vec05_lenient_quirks` | implemented |
| 05-lenient-quirks: Q-2 trailing junk ignored | `vec05_lenient_quirks` | implemented |
| 05-lenient-quirks: Q-3 silent stop | `vec05_lenient_quirks` | implemented |
| 05-lenient-quirks: Q-4 trailing comma | `vec05_lenient_quirks` | implemented |
| 05-lenient-quirks: Q-5 empty string → fail, **no** diagnostics | `vec05_lenient_quirks` | implemented |
| 06-reference-crashes (D-1): number/quote/high-byte function names → fail + log, never crash | `vec06_reference_crashes_d1` | implemented |
| 07-many-and-long (R12): 16 params, 90-char name | `vec07_many_and_long` | implemented |
| 08-lexical: comments between tokens; bytes ≥ 0x80 are unknown tokens; numbers/strings are not names | `vec08_lexical` | implemented |
| 09-parameter-list-edges: `f()`, EOFs, Q-3 after param | `vec09_parameter_list_edges` | implemented |
| R11: diagnostics carry 1-based line/col | `diagnostics_carry_positions` | implemented |
| R01/R10: `parse_decl_string` borrow + reset on failure | `c_facing_roundtrip_and_free` | implemented |
| Layout: `CallParamType` 4B (C enum), `DeclParam` 16B, `DeclInfo` 5W, C field order | `layout_is_c_compatible` | implemented |

## Evidence

- `cargo test -p obs-signal --lib` — 28/28 (12 decl tests).
- `cargo +nightly miri test -p obs-signal --lib` — 28/28.
- Workspace: 21 test binaries green. `clippy` 0 warnings. `cargo deny` ok.

## C-side remainder (blocked on C checkout)

- `test/conformance/scripts/decl/` 01–09 vs C binary.
- cbindgen header diff for `decl.h`.
