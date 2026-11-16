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
