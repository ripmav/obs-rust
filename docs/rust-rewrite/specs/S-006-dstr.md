# S-006 `dstr` dynamic string and string helpers

| | |
|---|---|
| Status | Ready for review |
| Reference | libobs 33.0.1 `util/dstr.c`, `util/dstr.h` |
| Target crate | `crates/obs-util` (module `dstr`) |
| Depends on | S-001 (allocator), S-005 (growth convention) |
| Conformance | `test/conformance/scripts/dstr/` (10 scripts) |

## 1. Scope

`struct dstr` is OBS's growable C string. Most of its API is `static inline` in the header
(compiled into plugins); the *exported* functions are the ones in `dstr.c`. A Rust crate
replaces the exported functions and provides a layout-compatible `Dstr` type whose
behaviour — including **capacity values** — matches, because plugins read `len`/`capacity`/
`array` directly and call both inline and exported functions on the same object.

Variadic/`va_list` functions (`dstr_printf`, `dstr_catf`, `dstr_vprintf`, `dstr_vcatf`) cannot be
written in stable Rust; they belong to the C shim (primer §5.5). Their *behaviour* is
specified here (R60–R63) because the shim must call the Rust helpers consistently.

## 2. Conventions

`len` excludes the terminating NUL. `cap` = `capacity` field in bytes. "Empty" means
`array == NULL` or `len == 0` or `*array == 0` (`dstr_is_empty`). Unless a rule says
otherwise, an operation that makes the string empty by **freeing** leaves
`array = NULL, len = 0, capacity = 0`. Out-of-range indexes/counts are caller errors
(undefined behaviour); scripts never use them.

## 3. Interface

```c
struct dstr { char *array; size_t len; size_t capacity; };       /* sizeof = 3W */

/* exported (dstr.c) */
int astrcmpi(const char*, const char*);   int wstrcmpi(const wchar_t*, const wchar_t*);
int astrcmp_n(const char*, const char*, size_t);  int wstrcmp_n(...);
int astrcmpi_n(const char*, const char*, size_t); int wstrcmpi_n(...);
char *astrstri(const char *str, const char *find);  wchar_t *wstrstri(...);
char *strdepad(char *str);                wchar_t *wcsdepad(wchar_t *str);
char **strlist_split(const char *str, char split_ch, bool include_empty);
void strlist_free(char **strlist);
void dstr_init_copy_strref(struct dstr*, const struct strref*);
void dstr_copy(struct dstr*, const char*);            void dstr_copy_strref(struct dstr*, const struct strref*);
void dstr_ncopy(struct dstr*, const char*, size_t);   void dstr_ncopy_dstr(struct dstr*, const struct dstr*, size_t);
void dstr_cat_dstr(struct dstr*, const struct dstr*); void dstr_cat_strref(struct dstr*, const struct strref*);
void dstr_ncat(struct dstr*, const char*, size_t);    void dstr_ncat_dstr(struct dstr*, const struct dstr*, size_t);
void dstr_insert(struct dstr*, size_t idx, const char*);  void dstr_insert_dstr(struct dstr*, size_t, const struct dstr*);
void dstr_insert_ch(struct dstr*, size_t idx, char);
void dstr_remove(struct dstr*, size_t idx, size_t count);
void dstr_printf(struct dstr*, const char *fmt, ...);   void dstr_catf(struct dstr*, const char *fmt, ...);
void dstr_vprintf(struct dstr*, const char*, va_list);  void dstr_vcatf(struct dstr*, const char*, va_list);
void dstr_safe_printf(struct dstr*, const char *fmt, const char *v1, const char *v2, const char *v3, const char *v4);
void dstr_replace(struct dstr*, const char *find, const char *replace);
void dstr_depad(struct dstr*);
void dstr_left(struct dstr *dst, const struct dstr *src, size_t pos);
void dstr_mid(struct dstr *dst, const struct dstr *src, size_t start, size_t count);
void dstr_right(struct dstr *dst, const struct dstr *src, size_t pos);
void dstr_from_mbs(struct dstr*, const char *mbstr);   char *dstr_to_mbs(const struct dstr*);
wchar_t *dstr_to_wcs(const struct dstr*);              void dstr_from_wcs(struct dstr*, const wchar_t*);
void dstr_to_upper(struct dstr*);  void dstr_to_lower(struct dstr*);

/* header-inline (stay in C; behaviour reproduced by Rust callers) */
dstr_init, dstr_init_move_array, dstr_init_move, dstr_init_copy, dstr_init_copy_dstr, dstr_free,
dstr_array_free, dstr_move_array, dstr_move, dstr_ensure_capacity, dstr_copy_dstr, dstr_reserve,
dstr_resize, dstr_is_empty, dstr_cat, dstr_cat_ch, dstr_find_i, dstr_find, dstr_cmp, dstr_cmpi,
dstr_ncmp, dstr_ncmpi, dstr_end
```

## 4. Behaviour — allocation

- **S-006-R01** `ensure_capacity(n)`: nothing if `n <= cap`; else `new = cap ? cap * 2 : n`, `new = max(new, n)`, `brealloc` to `new`, `cap = new`. (`cat a` on empty gives cap 2; then `cat b` → 4; `cat cdefgh` (needs 9) → 9: vector `02`.)
- **S-006-R02** `reserve(c)`: nothing if `c == 0 || c <= len` (**compares with `len`, not `cap`**); otherwise `brealloc` to exactly `c` and `cap = c` (may shrink capacity).
- **S-006-R03** `resize(n)`: `n == 0` → free; else `ensure_capacity(n + 1)`, `array[n] = 0`, `len = n`. Bytes in `[old_len, n)` are **uninitialised**.
- **S-006-R04** `free`: `bfree(array)`; `array = NULL, len = 0, cap = 0`. `init`: all zero.
- **S-006-R05** `init_move_array(d, s)` / `move_array(d, s)` (the latter frees `d` first): take ownership of `s`; `len = s ? strlen(s) : 0`; `cap = len + 1` (even for `s == NULL`: cap 1). `init_move`/`move` struct-copy and reset the source with `init`.

## 5. Behaviour — copy, concatenate, insert, remove

- **S-006-R10** `copy(d, s)`: `s == NULL || *s == 0` → `free(d)`. Else `ensure_capacity(strlen+1)` (so repeated copies reuse capacity: `copy hello; copy hi` keeps cap 6), copy including NUL, `len = strlen(s)`.
- **S-006-R11** `copy_dstr(d, src)`: `free(d)` first, then if `src.len != 0` allocate **exactly** `src.len + 1` and copy (cap = len+1).
- **S-006-R12** `ncopy(d, s, n)`: `free(d)` if allocated; `n == 0` → stays empty. Else `array = bmemdup(s, n + 1)` — **reads `n + 1` bytes from `s`** — `array[n] = 0`, `len = n`, `cap = n + 1`. `ncopy_dstr(d, src, n)`: free; `n == 0` → empty; `m = min(n, src.len)`; same with `m`.
- **S-006-R13** `cat(d, s)`: no-op if `s` NULL or empty; else `ncat(d, s, strlen(s))`. `ncat(d, s, n)`: no-op if `s == NULL || *s == 0 || n == 0`; else `ensure_capacity(len + n + 1)`, append exactly `n` bytes (**not clamped to `strlen(s)`**), terminate. `cat_dstr(d, src)`: no-op if `src.len == 0`; else `ensure(len + src.len + 1)`, append `src.len + 1` bytes. `ncat_dstr(d, src, n)`: no-op if `src.array == NULL || *src.array == 0 || n == 0`; append `min(n, src.len)` bytes.
- **S-006-R14** `cat_ch(d, c)`: `ensure_capacity(len + 2)`, append `c`, terminate (a `\0` character is appended as a real byte; `len` grows).
- **S-006-R15** `insert(d, idx, s)`: no-op if `s` NULL/empty; `idx == len` → `cat`; else `ensure(len + n + 1)`, move the tail (with NUL) up by `n`, copy `s`. `insert_dstr` same with `src.len == 0` → no-op. `insert_ch(d, idx, c)`: `idx == len` → `cat_ch`; else `ensure(len + 2)`, shift tail up one, store `c`.
- **S-006-R16** `remove(d, idx, count)`: `count == 0` → no-op; `count == len` → `free(d)` (not just empty); else close the gap; `len -= count`; capacity unchanged.

## 6. Behaviour — search, replace, substrings

- **S-006-R20** `find(d, s)` = `strstr(d->array, s)` (**crashes if `array == NULL`**); `find_i` = `astrstri` (NULL-safe, returns NULL). Empty needle: `find` returns `array` (offset 0); `astrstri(str, "")` returns `str`.
- **S-006-R21** `replace(d, find, repl)`: no-op if `d` is empty (`dstr_is_empty`). `repl == NULL` ≡ `""`. `find` must be non-empty (an empty needle loops forever). Scans left to right; after each replacement scanning resumes **after the inserted text** (replacement text is never rescanned; matches do not overlap). `len` is updated; capacity changes only when the result is longer: `ensure_capacity(new_len + 1)` (shorter/equal results keep `cap`). The result may be the empty string with `len == 0` and the array **retained** (`cap` unchanged, `str=""`), not freed (vector `05`: `abcabc` / `abc` → `""`).
- **S-006-R22** `depad(d)`: if `array != NULL`: `strdepad` it, then `len = strlen`, or `free(d)` if the result is empty. `strdepad(s)`: NULL → NULL; removes leading and trailing characters from `{space, tab, LF, CR}` in place; returns `s`.
- **S-006-R23** `left(d, src, pos)`: `resize(d, pos)` (so `pos == 0` frees `d`), then, if `d != src`, copy the first `pos` bytes of `src` (the terminator is already placed by `resize`). When `d == src` this truncates. `mid(d, src, start, count)`: copy `src` to a temporary; `ncopy(d, tmp + start, count)` (`count == 0` → empty); free the temporary. `right(d, src, pos)`: temporary = `ncopy(src.array + pos, src.len - pos)`; `copy_dstr(d, tmp)`; free tmp.
- **S-006-R24** `to_upper`/`to_lower`: no-op if empty; otherwise convert UTF-8 → wide string, apply `towupper`/`towlower` per wide character, convert back (`from_wcs`: empty result frees, otherwise `resize(len)`). ASCII behaviour is specified; non-ASCII mapping is locale-dependent in the reference.
- **S-006-R25** `from_mbs(d, s)`: `free(d)` then `d->len` = length of the converted string and `d->array` the converted allocation; **`capacity` is left 0** (Q-2). `to_mbs`, `to_wcs` return new `bmalloc` strings owned by the caller (convert to/from UTF-8 via platform helpers, S-0xx).

## 7. Behaviour — comparison helpers

- **S-006-R30** All comparison functions treat a NULL argument as `""`. Return values are negative/zero/positive (**only the sign is specified**; scripts print -1/0/1).
- **S-006-R31** `astrcmpi`/`astrcmpi_n`: compare byte by byte after `toupper` (C locale; ASCII specified); stop at the first difference or at the NUL; `_n` variants compare at most `n` bytes and return 0 for `n == 0`. `astrcmp_n`: same without case folding. Non-ASCII bytes are compared as **`char`** (signed on x86, unsigned on ARM): the sign for bytes ≥ 0x80 is platform-dependent and unspecified.
- **S-006-R32** `dstr_cmp(d, s)` = `strcmp(d->array ?: "", s ?: "")`; `cmpi` = `astrcmpi(d->array, s)`; `ncmp`/`ncmpi` = `astrcmp_n`/`astrcmpi_n(d->array, s, n)`.
- **S-006-R33** `astrstri(str, find)`: NULL if either is NULL; otherwise the first position (scanning every offset, including the terminating NUL position) where `astrcmpi_n(p, find, strlen(find)) == 0`. Case-insensitive search for `""` matches at offset 0.
- **S-006-R34** `dstr_is_empty`: true if `array` is NULL, `len == 0`, or `*array == 0`. `dstr_end(d)`: 0 if empty, else the last byte (`array[len-1]`).

## 8. Behaviour — formatted output (C shim)

- **S-006-R60** `vprintf(d, fmt, ap)`: let `n = vsnprintf(NULL, 0, fmt, copy)`; if `n < 0` use 4095. `ensure_capacity(n + 1)`; `vsnprintf(array, n + 1, fmt, ap)`. **If the result string is empty (`*array == 0`) the dstr is freed** and the function returns. Else `len = result` (or `strlen` if the result was negative).
- **S-006-R61** `vcatf(d, fmt, ap)`: same, but formats at `array + len` after `ensure_capacity(len + n + 1)`. The emptiness test (`*dst->array == 0`) looks at the **start of the whole string** (Q-3): appending an empty result to an empty dstr frees it; appending to a non-empty one just leaves it. `len += result`.
- **S-006-R62** `printf`/`catf` = `vprintf`/`vcatf` with a `va_list` built from the variadic arguments.
- **S-006-R63** `safe_printf(d, fmt, v1, v2, v3, v4)`: `copy(d, fmt)`; then for each non-NULL `vi` in order 1..4: `replace(d, "$i", vi)`. Substitution is sequential, so text inserted for `$1` that contains `$2` is substituted again by the `$2` step (Q-4). Missing values leave `$i` untouched.

## 9. Behaviour — `strlist_split`

- **S-006-R40** `strlist_split(s, ch, include_empty)`: `s == NULL` → returns NULL. Otherwise splits on every occurrence of `ch`; a segment is kept if non-empty **or** `include_empty`. Returns **one `bmalloc` block** containing a table of `count + 1` pointers (last = NULL) followed by the NUL-terminated segment strings; `strlist_free` is `bfree` of that block. Examples (vector `09`): `"a,,b"` → `["a","b"]` or `["a","","b"]`; `",a,"` with include_empty → `["","a",""]`; `""` → `[]` or `[""]`; `","` with include_empty → `["",""]`.

## 10. Reference quirks and defects

- **Q-1** `ncopy`/`ncopy_dstr`/`mid`/`insert_ch`/`ncat` read or move one byte beyond what the caller promises (`n + 1` bytes); callers must provide readable memory. A Rust port SHOULD read exactly `n` bytes and write the terminator itself (observably identical for valid input).
- **Q-2** `from_mbs` leaves `capacity == 0` although memory is allocated (harmless: later `ensure_capacity` re-allocates with `brealloc`).
- **Q-3** `vcatf` emptiness test uses `*dst->array` (see R61).
- **Q-4** `safe_printf` re-substitutes (R63).
- **Q-5** `dstr_find` on a NULL array crashes; the Rust port SHOULD return "not found" (DEFECT: required behaviour is *no crash*).
- **Q-6** `replace` with an empty `find` never terminates; port MUST treat it as a no-op (DEFECT).
- **Q-7** Locale dependence of `toupper/towupper` outside ASCII (see R24, R31).

## 11. Implementation notes (non-normative)

- Store `array` with the S-001 allocator; use `realloc`-style growth through `brealloc` so that capacity numbers match R01.
- Because capacity values are observable, do **not** use `Vec<u8>` growth; implement R01 explicitly.
- All the "free when empty" rules matter for memory-leak tests and for `capacity` checks in vectors.

## 12. Conformance coverage

| Rule | Scripts |
|---|---|
| R01, R03, R10–R12 | `01-copy-and-capacity`, `04-remove-all-and-resize` |
| R02 | `04-remove-all-and-resize` |
| R13, R14 | `02-cat-and-growth` |
| R15, R16 | `03-insert-remove` |
| R20 | `08-compare-and-find` |
| R21 | `05-replace` |
| R22, R23 | `06-depad-and-substrings` |
| R24 | `10-case` |
| R30–R34 | `08-compare-and-find` |
| R40 | `09-strlist-split` |
| R60–R63 | `07-printf` |
| R04, R05, R25 | unit tests in the crate (not scripted) |

## 13. Open questions

- UTF-8 ↔ wide conversion edge cases (invalid sequences, surrogates) are in the platform/utf8 spec (not yet written).
