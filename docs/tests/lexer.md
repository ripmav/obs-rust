# lexer (S-011 §5.1, R01–R12) — test matrix

WP-20 (P1-08.2 base lexer), crate `crates/obs-util`, module `lexer`
(`src/lexer.rs`). Spec: `S-011-*.md` §5.1 (R01–R12); the CF lexer,
preprocessor and parser (R20–R80) are P1-08.3 and have their own matrix
row set (conformance scripts 10+). The tests encode the committed C
reference vectors `test/conformance/vectors/lexer/01..06` (generated from
`libobs/util/lexer.c`); the vectors are the oracle, byte values included.
`StrRef<'a>` borrows its buffer (no raw pointers, crate policy); its
trusted `len` may exceed the visible buffer after `add` exactly like C.

| Area                                                                                                                                                                                  | Pinned by test                                     | Conformance vector |
| ------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | -------------------------------------------------- | ------------------ |
| **base tokens (R01, R02, R03)**                                                                                                                                                       |                                                    |                    |
| ALPHA/DIGIT maximal runs; one-char WHITESPACE/OTHER; CRLF and LFCR pairs are one two-char token; mixed high bytes as single OTHER tokens; end-of-text returns false with final offset | `v01_base_tokens` (first `lall`)                   | `01-base-tokens`   |
| `start`/`start_move`/`reset`/`free` lifecycle; empty and NULL text give `none` at offset 0; control byte `\x01` as OTHER                                                              | `v01_base_tokens` (rest)                           | `01-base-tokens`   |
| ignore-whitespace mode skips whitespace (offset advances past it); one token after `reset`                                                                                            | `v01_base_tokens` (`lall ignore` + `lnext ignore`) | `01-base-tokens`   |
| **predicates (R05)**                                                                                                                                                                  |                                                    |                    |
| whitespace/newline/space-or-tab per byte (`\x0b`, `\x0c`, `_`, `a`, `0` are none); newline pair only `\r\n`/`\n\r`; `newline_size` 2/1/0 shapes incl. empty                           | `v02_base_classify`                                | `02-base-classify` |
| **strref (R06–R09)**                                                                                                                                                                  |                                                    |                    |
| `strref_cmp` 13 cases: NUL-padded span vs C string, empty/NULL spans, sign-only results                                                                                               | `v03_strref_cmp` (`cmp_cases`)                     | `03-strref-cmp`    |
| `strref_cmpi` 3 cases (`A^` vs `a_` ordering via unsigned `^`/`_` fold identity)                                                                                                      | `v03_strref_cmp` (`cmpi_cases`)                    | `03-strref-cmp`    |
| signed-char semantics for bytes >= 0x80 (`0xE9` vs `a` is -1 for `cmp`, +1 reversed, -1 for `cmpi`)                                                                                   | `v03_strref_cmp` (high-byte cases)                 | `03-strref-cmp`    |
| `strref_cmp_strref`/`cmpi_strref` 14 cases incl. Q-1 asymmetry (non-empty vs empty is -1) and the len-1 truncation case                                                               | `v03_strref_cmp` (span cases)                      | `03-strref-cmp`    |
| `is_empty`: NULL array, zero len, empty buf, NUL first byte                                                                                                                           | `v03_strref_cmp` (is_empty)                        | `03-strref-cmp`    |
| `strref_add`: null-array copies, non-null grows len without adjacency check (len may exceed the buffer)                                                                               | `v03_strref_cmp` (ref_add)                         | `03-strref-cmp`    |
| **number validators (R10, R11, Q-2)**                                                                                                                                                 |                                                    |                    |
| `valid_int_str`: 14 table cases (sign, N-count after sign, no-digit) + NULL/empty                                                                                                     | `v04_valid_numbers`                                | `04-valid-numbers` |
| `valid_float_str`: 26 table cases incl. the Q-2 quirk (`1e+5`/`1e-5` false, `1e5+3`/`1e5-3` true, `1E5` false, `1e`/`e5` false) + NULL/empty                                          | `v04_valid_numbers`                                | `04-valid-numbers` |
| **error data (R12)**                                                                                                                                                                  |                                                    |                    |
| add/item/type_count/has_errors; buildstring `"<file> (<row>, <col>): <msg>\n"` byte exact; NULL-if-empty before and after free                                                        | `v05_error_data`                                   | `05-error-data`    |
| **row/column lookup (R04)**                                                                                                                                                           |                                                    |                    |
| 1-based row/col; CRLF and LFCR pairs are one line break (incl. pointer inside the pair); NULL pos leaves outputs untouched; lone `\n\n`, `\r\n`, tab cases                            | `v06_stroffset`                                    | `06-stroffset`     |
