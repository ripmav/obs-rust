# cf-lexer (S-011 §5.2/§5.4, R20–R30, R60) — test matrix

WP-21 (P1-08.3 cf-lexer/cf-parser), work unit W21a, crate
`crates/obs-util`, module `cf_lexer` (`src/cf_lexer.rs`). Spec:
`S-011-*.md` §5.2 (R20–R30, algorithm 6.1), §5.4 (R60). The preprocessor
(R40–R54) and parser helpers (R70–R80) are W21b/W21c and add their rows
here when delivered. The tests encode the committed C reference vectors
`test/conformance/vectors/lexer/{10,11,12,13,14,15,33}` in the conformance
runner's print format (`gen/lexer_runner.c` + `script.h`), so the vectors
are the oracle, byte values included. Tokens are views into the owning
lexer (`str` → `reformatted`, `span` → original text); valid until the
lexer is re-lexed or dropped. `Lexer::set_offset` (added to `lexer.rs`)
exposes the C `lex->offset` cursor assignment the cf layer needs for
splice skips and break rewinds.

| Area | Pinned by test | Conformance vector |
| --- | --- | --- |
| **token production (R20–R23)** | | |
| NAME/NUM classification (ALPHA/DIGIT starts incl. `_` → NAME, `.`+DIGIT → NUM), OTHER chars, row/col of every token | `v10_cf_lex_basic` (case 1) | `10-cf-lex-basic` |
| every newline shape (`\r\n`, `\n\r`, lone `\r`, lone `\n`) as NEWLINE; `\x0b`/`\x0c` as OTHER; high bytes (`\xc3\xa9`) as single OTHER tokens | `v10_cf_lex_basic` (case 2, 3) | `10-cf-lex-basic` |
| **lifecycle / empty input (R20)** | | |
| empty text → `lex false`, zero tokens, no `reformatted` output; whitespace-only and lone-newline input → one token + NONE | `v11_cf_lex_empty` | `11-cf-lex-empty` |
| **comments (R25, Q-3)** | | |
| line and block comments collapse to one space; comment token = SPACETAB with full source span; `unexpected_eof` flag on unterminated block comment; comment-adjacent tokens (`./*c*/`, `/*/`, `/**/`); comment after a token does not merge with preceding non-blank token | `v12_cf_lex_comments` | `12-cf-lex-comments` |
| **strings (R26, Q-4, Q-5)** | | |
| unterminated string at EOF; escaped delimiter (`\'`, `\"`) does not terminate; string stops before a newline (newline stays a separate token); Q-5 span one byte longer than the text (always `u=` in the dump) | `v13_cf_lex_strings` (cases 1, 2) | `13-cf-lex-strings` |
| **include strings (R27)** | | |
| `<...>` is a STRING only after `# include`/`#import` (newline or stream start before the `#`); the token walk skips blanks, matches `include`/`import` whole-token, then `#`; escapes disabled in include strings; `# include` with spaces, `<c>` mid-line (no preceding NEWLINE), and a comment token before `#` all still count | `v13_cf_lex_strings` (case 3, 43 tokens) | `13-cf-lex-strings` |
| **splices (R24)** | | |
| `\<newline>` removed from `str`, kept in the source span (NAME `abcd` over `ab\<LF>cd`); splice inside comments (Q-3: char after a splice not examined), inside strings, at end of input; `\<not-newline>` stays an OTHER `\` token; `\r\n` splice pair | `v14_cf_lex_splices` | `14-cf-lex-splices` |
| **row/col through tokens (R04)** | | |
| every token's `@row:col` across CRLF/LFCR/lone-newline pairs; NONE at the end position | `v15_cf_lex_rows` | `15-cf-lex-rows` |
| **literal decoding (R60, Q-10, D-8)** | | |
| quote validation: same-quote only, count truncation, `@null` on unterminated/mismatched/quoteless input; explicit count shorter than the text | `v33_literal` (cases 1–8, 15) | `33-literal` |
| simple escapes (`\n \t \a \b \f \r \v \0 \\ \' \" \?`) with the Q-10 loop quirk: an escape consumes its source chars in one iteration, so the closing quote leaks into the result of a longer literal | `v33_literal` (cases 9–14, 16–21) | `33-literal` |
| octal mis-parse pinned exactly as C: the switch consumes the first escape char, `isdigit` checks the next char, `strtoul(…, 8)` reads from there, `src += 3` (`\101z` → `\x01"`, `\123` → `\x13`, `\1` → `"`); hex `\x41` → `A'`; `\xzz`/`\q` → no output, no skip past the escape char | `v33_literal` (cases 11, 12, 16, 18, 19, 20) | `33-literal` |
| D-8: no out-of-bounds read for counts shorter/longer than the buffer (the `ch()` helper returns NUL past the end) | `v33_literal` (cases 4, 15, 18) | `33-literal` |

Verification (W21a): `cargo test -p obs-util` 173 passed; `cargo clippy
-p obs-util --all-targets -- -D warnings` clean (host + x86_64);
`cargo fmt --check` clean; nextest workspace 447/447.

## Preprocessor (S-011 §5.3, R40–R54) — W21b

Module `cf_preprocessor` (`src/cf_preprocessor.rs`). Same oracle: the
committed vectors `test/conformance/vectors/lexer/{20,21,22,23,24,25,26,
50,90}` in the runner's print format; `run.sh check lexer` is green
against the linked C reference, so the vectors and the reference agree.
ErrorData items now carry an **owned** file name (C holds a pointer; the
Rust item copies it so error lists are self-contained — `ErrorData` lost
its lifetime parameter). Output tokens are views into `main`, the
dependency lexers (`TokenSource::Dep`), caller-owned API side lexers
(`TokenSource::Side`) or synthesized (`Synth`, one-byte space filler);
`src=-` in the dump means `Synth` span, `u=` shows the owning span when
it differs from the token text.

| Area | Pinned by test | Conformance vector |
| --- | --- | --- |
| **define/undef (R45, R54, Q-6)** | | |
| object-like define + expansion (leading synthetic blank, body tokens keep their source positions), `#undef`, undef of a missing name (no-op), first definition wins (second `#define` of a name does not replace) | `v20_pp_define`, `v50_pp_quirks` (Q-6 case) | `20-pp-define`, `50-quirks` |
| directive error paths: `#define` without a name, `#define 1`, `#define` at EOL, `#ifdef`/`#undef` without a name, `#ifdef 5`, `#pragma` unknown directive (name skipped, rest of the line hits the expected-newline error) | `v20_pp_define` (case 3), `v22_pp_ifdef` (case 7), `v25_pp_errors`, `v50_pp_quirks` (unknown directive) | `20-pp-define`, `22-pp-ifdef`, `25-pp-errors`, `50-quirks` |
| **function-like macros (R46, Q-8, Q-9, D-6)** | | |
| multi-param expansion with nested-paren args (`F(g(1,2),3)`), params expanded with their stored tokens, synthetic blanks carrying the macro-name span (`u="S"`/`u="F"`) | `v21_pp_macro` (case 1) | `21-pp-macro` |
| zero-param macro call `N()`, extra args `N(1)`; `M(a)` called with `M(1,2)` — mismatch error at the closing `)` (reference position, not the extra arg); `#define M(a,)` → "Expected identifier" at `)`; `F (1)` (blank before `(`) still expands | `v21_pp_macro` (cases 2, 3), `v50_pp_quirks` (Q-9 case) | `21-pp-macro`, `50-quirks` |
| macro name without `(`: "Expected '('" at the following token, which is consumed (the rest of the line is dropped); `#define Z() 0` → "Expected identifier" and `Z` stays undefined | `v50_pp_quirks` (Q-8, Q-9) | `50-quirks` |
| **conditional blocks (R50, R51, R44)** | | |
| `#ifdef`/`#ifndef`/`#else`/`#endif`, nesting, ignored branches emit nothing; unterminated block → "Unexpected end of file before #endif" at the NONE token; stray `#else`/`#endif`/`#elif` outside a block → per-directive error | `v22_pp_ifdef` (cases 1–4) | `22-pp-ifdef` |
| trailing junk after a directive ("expected newline" at the first non-blank, line dropped); Q-13: `#elif` after a TRUE block reports the spurious "expected newline" at the `#elif` operand | `v22_pp_ifdef` (case 5), `v50_pp_quirks` (Q-13) | `22-pp-ifdef`, `50-quirks` |
| `#if` with NUM/NAME operands (NAME follows defines recursively), all six comparators, bare value (`#elif V`), `#elif` chains, `#else`; Q-14 `sscanf("%i")` semantics (hex `0x10`, `08` → 0, `1.5` → 1 over the rest of the buffer); malformed conditions: `=` / `~` comparators, undefined/missing operands, "Expected identifier or number" / "Expected comparator" at the reference positions, `#if 1 == 1 junk` → expected-newline | `v23_pp_if` (all cases) | `23-pp-if` |
| **include (R48, R49, Q-7, D-4)** | | |
| local include: dependency lexed with its path as file, tokens emitted with their dependency positions + `f="file"`; a `#define` inside a dependency registers in the shared define list (first wins on re-include); repeated include re-preprocesses the existing dependency; system `<...>` includes accepted and ignored; BOM stripped from the dependency text | `v24_pp_include` (both cases) | `24-pp-include` |
| Q-7: "Could not open file" embeds the rest of the buffer from the string token; `#include foo` → "Expected string"; `#include "x" y` → open error + expected-newline at `y`; nested/relative resolution against the main file's directory | `v24_pp_include` (case 1) | `24-pp-include` |
| **API definitions (R53, D-5)** | | |
| `cf_preprocessor_add_def` / `remove_def`: API defs carry exactly the caller's tokens (no leading blank), `#ifdef` sees them, expansion emits the body tokens verbatim, `remove_def` by name, missing name no-op | `v26_pp_api_defs` | `26-pp-api-defs` |
| **defects (D-3, D-4, D-7)** | | |
| D-3: a definition with an empty body expands to nothing (no crash); D-4: an empty included file yields no tokens and no crash; D-7: self-referencing (`#define A A`) and mutually recursive (`A → B → A`) macros terminate, the name emitted verbatim | `v90_defects` | `90-defects` (try-blocks: output not compared, must not crash) |

Verification (W21b): `cargo test -p obs-util` 177 + 4 + 1 passed;
`cargo clippy -p obs-util --all-targets -- -D warnings` clean (host +
x86_64); `cargo fmt --check` clean; nextest workspace 456/456.

## Parser helpers (S-011 §5.5, R70–R80) — W21c

Module `cf_parser` (`src/cf_parser.rs`): `CfParser` owns its main lexer,
preprocessor and error list; the cursor is an index into the
preprocessed stream. D-2 (the reference crashes on a NULL cursor after a
failed parse) has no Rust equivalent: a fresh/failed parser treats the
stream as empty, every helper is safe and reports the end-of-stream NONE
sentinel. `parse` does not clear preprocessor definitions (like the
runner; `reset` does). Same oracle: committed vectors `30, 31, 32, 50
(parser parts), 90`.

| Area | Pinned by test | Conformance vector |
| --- | --- | --- |
| **navigation (R72, R73, R74, R76, R77, Q-11)** | | |
| Q-11: the first `next` on a stream starting with a non-blank token skips that token; `next`/`next_valid` with cursor positions, `peek` (token line + restored cursor), `token_is` (exact case-sensitive), `goto_type` (no pre-advance; NONE = false) | `v30_parser_nav` (all cases), `v50_parser_quirks` (Q-11 case) | `30-parser-nav`, `50-quirks` |
| `pass_pair` nesting and recursion: `f ( { [ ( ) ] } ) g` consumes through the nested `(...)` pair to the space after `)`; non-OTHER or non-matching first char returns `cur.type != NONE` without moving | `v30_parser_nav` (cases 2, 3) | `30-parser-nav` |
| **should_be / is_type (R78, R79)** | | |
| `should_be` SUCCESS / CONTINUE with `Expected '<s>'` at the post-jump cursor / EOF without an error when the goto misses; `next_should_be` with the same trio; `is_type` SUCCESS / CONTINUE+expecting / EOF (goto miss adds `Unexpected EOF`) | `v30_parser_nav` (case 1), `v31_parser_names` (case 4) | `30-parser-nav`, `31-parser-names` |
| **goto and Q-12 (R75)** | | |
| `go_to_token(s1, s2)` scanning with `{...}` block skipping; Q-12: `a {}x y` — the token right after a skipped block without a preceding blank is not examined (goto misses, ends at NONE); `a { } x y` finds it; the token's first char (any token type) triggers the pair skip | `v30_parser_nav` (case 2), `v50_parser_quirks` (Q-12 cases) | `30-parser-nav`, `50-quirks` |
| **name/copy helpers (R80)** | | |
| `next_name`/`next_name_ref`/`get_name_ref`/`next_copy`/`copy` with the PARSE codes (SUCCESS/CONTINUE/EOF), `name=`/`ref=`/`copy=` values (`@null` on failure), EOF on missing tokens with `Unexpected EOF` at the NONE position | `v31_parser_names` (cases 1–3) | `31-parser-names` |
| **adderror formatting (R71)** | | |
| positions and files from the cursor's span lexer; verbatim message when all values are NULL; sequential `$1/$2/$3` global substitution (a value's `$n` is subject to later passes — pinned via `only2 $1$2$3` with a NULL middle value staying literal), the three fixed helpers, `errstr` (buildstring, NULL file prints `(null)`) | `v32_parser_errors` | `32-parser-errors` |
| **defects (D-2, D-3, D-4, D-7)** | | |
| D-2: `parse ""` fails and every helper (next/peek/adderror) on the never-parsed parser is safe (empty stream, no crash); D-3/API: an empty definition expands to nothing; D-4: an empty included file is loaded without a crash; D-7: self- and mutually recursive macros terminate (reference crashes in all seven try-blocks: `try-end crash`) | `v90_defects` (no output comparison, no-panic asserts) | `90-defects` (try-blocks) |

Verification (W21c): `cargo test -p obs-util` 182 + 4 + 1 passed;
`cargo clippy -p obs-util --all-targets -- -D warnings` clean (host +
x86_64); `cargo fmt --check` clean; nextest workspace 461/461.
