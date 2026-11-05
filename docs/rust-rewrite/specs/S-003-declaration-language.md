# S-003 Declaration language (`parse_decl_string`)

| | |
|---|---|
| Status | Ready for review |
| Reference | libobs 33.0.1 `callback/decl.c`, `decl.h` (uses the generic C-like lexer/parser `util/cf-*.c`, specified separately) |
| Target crate | `crates/obs-signal` (module `decl`) |
| Depends on | S-001 |
| Conformance | `test/conformance/scripts/decl/` (9 scripts) |

## 1. Scope

A tiny C-like function-declaration language used to describe **signals** (events) and
**procedures** (S-004), e.g. `"void source_create(ptr source)"`. The parser is called by
`signal_handler_add` and `proc_handler_add` with strings that are mostly compiled into the
program and plugins. Plugins register their own signals/procs with it, so compatibility
for valid input is essential; behaviour for malformed input matters less but is
characterised below because the reference accepts some malformed strings.

This spec describes the language directly (it does not depend on the generic lexer spec).

## 2. Conventions

`NAME` = `[A-Za-z_][A-Za-z0-9_]*` (ASCII only). `WS` = spaces, tabs, newlines, `/* … */`
comments, `// …` comments up to end of line. A *token* is a NAME, a number, a quoted string,
or a single punctuation character; any other byte (including bytes ≥ 0x80) forms a
single "unknown" token that is not a NAME.

## 3. Interface

```c
enum call_param_type {
    CALL_PARAM_TYPE_VOID = 0, CALL_PARAM_TYPE_INT = 1, CALL_PARAM_TYPE_FLOAT = 2,
    CALL_PARAM_TYPE_BOOL = 3, CALL_PARAM_TYPE_PTR = 4, CALL_PARAM_TYPE_STRING = 5
};
#define CALL_PARAM_IN  (1 << 0)
#define CALL_PARAM_OUT (1 << 1)

struct decl_param { char *name; enum call_param_type type; uint32_t flags; };
struct decl_info  { char *name; const char *decl_string; DARRAY(struct decl_param) params; };

bool parse_decl_string(struct decl_info *decl, const char *decl_string);
/* inline: decl_param_free(p), decl_info_free(d) — free names with bfree, zero the struct */
```

`DARRAY(T)` is `{ T *array; size_t num; size_t capacity; }` (S-005).

## 4. Grammar

```
declaration := RET_TYPE NAME '(' [ param_list ] ')'      anything after the first ')' is ignored
param_list  := param { ',' param } [ ',' ]
param       := { 'in' | 'out' } TYPE NAME
RET_TYPE    := 'void' | TYPE
TYPE        := 'int' | 'float' | 'bool' | 'ptr' | 'string'
```

Keywords (`in out int float bool ptr string void return`) are **case-sensitive**.
Whitespace and comments may appear between any two tokens **except before the first
token** (see Q-1).

## 5. Behaviour

- **S-003-R01** On success returns `true`, `decl->name` is a `bstrdup` copy of the function name, `decl->decl_string` is the *pointer passed in* (not copied; it must outlive the decl), and `decl->params` holds the parameters in declaration order.
- **S-003-R02** Each parameter gets `flags`: bit 0 = `in`, bit 1 = `out`. Writing `in` and/or `out` (any order, each at most once) sets the bits; a parameter with **no** specifier gets `CALL_PARAM_IN`. `in out` and `out in` both yield `IN|OUT`.
- **S-003-R03** If the return type is not `void`, after all declared parameters a final parameter is appended: `{ name = "return", type = RET_TYPE, flags = CALL_PARAM_OUT }`. For `void` nothing is appended. (This is why `int f()` has `params = 1`.)
- **S-003-R04** `void` is valid only as the return type; as a parameter type it is an error ("Expected 'type'").
- **S-003-R05** Reserved names: `int float bool ptr string void return`. A function name or a parameter name equal to one of them is an error. Names `in` and `out` are **not** reserved as names (`void in(int out)` is valid).
- **S-003-R06** Parameter names must be unique within a declaration (exact match); a repeated name is an error. A parameter may share its name with the function.
- **S-003-R07** Specifiers: `in` repeated, or `out` repeated, in one parameter is an error ("`in` specifier already exists").
- **S-003-R08** A parameter must have a TYPE then a NAME. After any specifiers, a token that is not a TYPE keyword → error "Expected 'type'"; a missing/non-NAME name → error "Expected 'parameter name'".
- **S-003-R09** The return type must be one of RET_TYPE, otherwise error "Expected 'return type'"; the function name must be a NAME followed by `(`.
- **S-003-R10** On **any** error the result is failure (`false`); `decl` is freed (`name == NULL`, no params). Errors do not stop parsing early in all cases, but the result is the same: failure. Whether several errors are logged is informational.
- **S-003-R11** On failure (but not for an *empty* string — see Q-5) one message at `LOG_WARNING` is logged: `Errors/warnings for '<decl_string>':\n\n<error lines>`, where each error line is `declaration (<line>, <column>): <message>` (1-based line and column). The **level is MUST**; the text is SHOULD (useful diagnostics).
- **S-003-R12** The parser has no limits on the number of parameters or on identifier length (16-parameter and 90-character names accepted).

### 5.1 Parameter-list loop (precise)

After `(`:

1. If the next token (after WS) is `)` → empty list, done.
2. If the next token is a NAME → parse a `param`, then go to step 4.
3. If it is end of input → error ("Unexpected EOF"). Any other token → error "Expected ')'".
4. After a param: skip WS; if the next token is `)` → done; if `,` → consume it and go to step 5; **end of input → error ("Unexpected EOF")**; **any other token → stop parsing parameters silently and accept** (Q-3).
5. After a comma: skip WS; if the next token is `)` → done (**trailing comma accepted**, Q-4); if a NAME → parse a `param` and go to step 4; if EOF → error; anything else → error "Expected ')'".

## 6. Reference defects and quirks

- **Q-1 (leading WS).** A declaration that begins with whitespace or a comment fails: the first token must be the return type with nothing before it (`" void f()"`, `"/* c */ void f()"` fail). Trailing WS/comments after `)` are fine. Vectors `05`. Required: reproduce (SHOULD).
- **Q-2 (trailing junk).** Everything after the first `)` that ends the list is ignored: `"void f() extra"`, `"void f(int a))"`, `"void f(int a) )"` succeed. Required: reproduce (SHOULD).
- **Q-3 (silent stop).** After a parameter, a token that is neither `,` nor `)` ends the list without error: `"void f(int a int b)"` → one parameter `a`; `"void f(int a-b)"`, `"void f(int a; int b)"` → one parameter; `"void f(int a, int b c)"` → two. Required: reproduce (SHOULD).
- **Q-4 (trailing comma).** `"void f(int a,)"` and `"void f(int a, int b, )"` succeed. Required: reproduce (SHOULD).
- **Q-5 (empty string).** `parse_decl_string("")` fails **without** logging. Required: reproduce (SHOULD).
- **D-1 (crash on non-NAME function name).** If the token after the return type is not a NAME and not `(` — e.g. `"void 1f()"`, `"void 123()"`, `"void +()"`, `"void \"s\"()"`, `"void é()"`, `"int 1f(int a)"` — the reference dereferences a NULL name and **crashes** (vectors `06`, `08`). Required (MUST): return `false`, log the error as usual (R11), never crash. The expected files contain no log line for these inputs because the reference died first; comparison ignores log lines for `parse crash` entries (conformance README, rule 3).
- **Q-6 (error message wording).** The message texts come from the generic parser; only their level is normative.

## 7. Concurrency

`parse_decl_string` has no shared state and is thread-safe. It allocates with S-001.

## 8. Implementation notes (non-normative)

- A hand-written scanner over `&[u8]` is sufficient; implement WS skipping once and apply it everywhere except at the very first token (Q-1).
- Produce `decl.name`/param names with the S-001 allocator (`bstrdup`) because C code frees them with `bfree`.
- Keep `decl_string` as a borrowed pointer; do not copy it.
- Keep the parser behind a Rust API returning `Result<Decl, Vec<Diagnostic>>`; the C shim maps `Err` to `false` and logs R11.

## 9. Conformance coverage

| Rule | Scripts |
|---|---|
| R01, R02, R03 | `01-valid`, `04-return-parameter`, `03-case-and-keywords` |
| R04 | `02-invalid` (`void f(void a)`) |
| R05 | `02-invalid`, `03-case-and-keywords` |
| R06 | `02-invalid` (`void f(int a, int a)`), `07-many-and-long` |
| R07 | `02-invalid` |
| R08, R09 | `02-invalid`, `03-case-and-keywords`, `09-parameter-list-edges`, `08-lexical` |
| R10, R11 | all scripts (log lines present before `parse fail`) |
| R12 | `07-many-and-long` |
| §5.1 | `09-parameter-list-edges`, `05-lenient-quirks` |
| Q-1…Q-5 | `05-lenient-quirks` |
| D-1 | `06-reference-crashes`, `08-lexical` (`void é()`) |
| Lexical (NAME, WS, comments) | `08-lexical`, `01-valid` |

## 10. Open questions

- Behaviour for NUL bytes, invalid UTF-8 and extremely long inputs is not characterised
  (not reachable through the C API, which takes a C string).
- The reference's use of a generic parser means numbers/strings/punctuation form tokens; tokens
  that are neither NAME nor `(` `)` `,` only matter in the positions listed above.

## 11. Round 9: `decl.c` parse internals (added)

### Reference files (all cited)

- `libobs/callback/decl.c`

- **S-003-R13** `parse_decl_string`: the entry point — tokenizes the declaration string with the cf-lexer (S-011), splits it into params (`parse_params`, one per `parse_param` call); errors accumulate in the lexer error data and are rendered by `print_errors` (one line per error, with position).
- **S-003-R14** Name rules: reserved param names — `int`, `float`, `bool`, `ptr`, `string`, `void`, `return` — are rejected (`err_reserved_name`); a param name already present in the decl is rejected (`name_exists`).
- **S-003-R15** Type resolution: `get_type` maps the type token to the calldata type; `is_in_out_specifier` recognizes in/out/in-out markers; type defaults and the resulting param order follow the earlier sections.
