# Test matrix: `dstr` dynamic string (obs-util)

- Test file: `crates/obs-util/src/dstr.rs` (unit tests) + counter/drop-leak assertions in `crates/obs-util/tests/leak_counters.rs`
- Task: P1-02 (spec S-006 R01–R05, R10–R16, R20, R21–R25, R30–R34, R40, R63, R64–R66)
- Golden files: none (the 10 C conformance scripts arrive with the C checkout)

| Case | Input | Expected | Platforms | Status |
|---|---|---|---|---|
| layout | size/offset of `Dstr`, `StrRef` | 3×usize (C field order); `StrRef` 2×usize | Linux, Windows, macOS | pass |
| R01 growth (vector 02) | `cat a` / `cat b` / `cat cdefgh` on empty | cap 2 → 4 → 9 (`new = cap ? cap*2 : n`, `max(new, n)`) | Linux, Windows, macOS | pass |
| R01 ensure noop | `ensure(4)` at cap 4 | no realloc | Linux, Windows, macOS | pass |
| R02 reserve | cap 5, len 4: `reserve(4)`, `reserve(8)`, `reserve(2)` | no-op (compares **len**, not cap); exact 8; no-op | Linux, Windows, macOS | pass |
| R03 resize | 5, then 0 | len/cap updated, NUL at `[n]`, 0 frees | Linux, Windows, macOS | pass |
| R04 free/init | copy then free | all fields zero | Linux, Windows, macOS | pass |
| R05 move array | null; owned `"xy\0"`; struct move | cap 1 for null; cap 3 for 2 bytes; source reset (double-free regression test) | Linux, Windows, macOS | pass |
| R10 copy | `"hello"` then `"hi"` then `""` | cap 6 kept across copies; empty frees | Linux, Windows, macOS | pass |
| R11 copy_dstr | 11-byte source; empty source | cap exactly len+1; empty → stays empty | Linux, Windows, macOS | pass |
| R12 ncopy | `"abcdef", 3`; `("", 5)` | `"abc"`, cap 4; stays empty | Linux, Windows, macOS | pass |
| R13 cat family | `""`, ncat 2 of `"ab\0cd"`, cat_dstr, ncat_dstr 1/99 | exact-byte appends, no clamping to strlen; no-ops on empty | Linux, Windows, macOS | pass |
| R14 cat_ch | `\0` then `c` | NUL is a real byte, len grows | Linux, Windows, macOS | pass |
| R15 insert | insert `"XY"@2`, `""`, ch `@0`, ch `@len`, insert_dstr | tail shift (with NUL) up by n; append fast-paths | Linux, Windows, macOS | pass |
| R16 remove | `(2,0)`, `(2,2)`, `(0,len)`, `(1,3)` | no-op; gap closed; **count==len frees**; capacity kept on partial | Linux, Windows, macOS | pass |
| R20 find | hits, miss, empty needle, NULL array | offset; `None`; **0** for empty needle; **no crash** for NULL (Q-5) | Linux, Windows, macOS | pass |
| R30/R31 compare | equal, case, prefix, NULL | signs -1/0/1; `_n` at most n bytes, `n==0` → 0; NULL ≡ `""` | Linux, Windows, macOS | pass |
| R33 astrstri | hit, case-insensitive, empty needle, NULLs | first case-insensitive offset; `""` → 0; NULL → `None` | Linux, Windows, macOS | pass |
| R32 dstr_cmp | equal, less, cmpi, ncmp(2) | strcmp/astrcmpi semantics on `d->array ?: ""` | Linux, Windows, macOS | pass |
| R34 is_empty/end | empty, `"abc"` | NULL/len0/*array==0; last byte | Linux, Windows, macOS | pass |
| R21 replace (vector 05) | `"abcabc"`/`"abc"`→`""`; `"aaa"`/`"aa"`→`b`; grow; empty find; empty d | empty result **keeps array**, cap unchanged; non-overlapping resume after insert; empty `find` no-op (Q-6) | Linux, Windows, macOS | pass |
| R22 depad | `"  \t hello \r\n "`; `"   "`; NULL array | trimmed; all-pad frees; NULL-safe | Linux, Windows, macOS | pass |
| R23 left/mid/right | 5/0; 6/5, 6/0, 6/99; right 6/0; self-truncate | truncation via resize; exact byte ranges; self case = `dstr_truncate` | Linux, Windows, macOS | pass |
| R24 case | `"hEllO WoRld 123"` | ASCII fold both ways; non-ASCII untouched (Q-7, C locale) | Linux, Windows, macOS | pass |
| R25 wide (Q-2) | `"héllo"`; invalid UTF-8 | mbs copy with **cap 0**; wcs u32 round trip; invalid → `None` | Linux, Windows, macOS | pass |
| R63 safe_printf (Q-4) | `"hello $1, $2"`; `$1`→`"$2"` chain; missing values | sequential substitution (inserted text rescanned); untouched `$i` | Linux, Windows, macOS | pass |
| R40 strlist_split (vector 09) | `"a,,b"` ±empty; `",a,"` +empty; `""` ±empty; `","` +empty; NULL | one S-001 block: pointer table + segment strings; NULL input → None | Linux, Windows, macOS | pass |
| R66 cat_quoted/strref | `"hi"`; strref copy/cat, empty strref | `"\"hi\""`; borrowed-segment ops, no NUL requirement | Linux, Windows, macOS | pass |

Notes / findings:
- **Rust-vs-C divergences (all documented in code + ODD L9)**:
  1. `dstr_move`: the source reset must be field-wise — `*src = Dstr::init()`
     runs the old value's Drop first (frees the buffer the destination now
     owns) — a double free Miri/glibc would catch.
  2. Self-aliasing `dstr_left(d, d, pos)` is legal in C, unrepresentable
     with Rust references (Miri: Stacked Borrows retag violation) —
     provided as `dstr_truncate`.
  3. Q-5: `dstr_find` on a NULL array returns `None` (C crashes).
  4. Q-6: `dstr_replace` with an empty `find` is a no-op (C loops forever).
  5. R12/Q-1: `ncopy` reads exactly `n` bytes (C reads `n+1`); identical
     for valid input.
  6. `resize`/insert zero the extended region (C leaves it uninitialised;
     observably identical, Miri-clean).
- **Not in this WP** (deferred, listed in spec/subtask): the variadic
  `dstr_printf`/`dstr_catf`/`dstr_vprintf`/`dstr_vcatf` (R60–R62, C shim
  per primer §5.5); the wide-string compare/depad family
  (`wstrcmpi`, `wstrcmp_n`, `wstrstri`, `wcsdepad` — S-006 §3 export
  list; they operate on `wchar_t` slices and are shim-side); UTF-8↔wide
  edge cases for invalid sequences (spec §13, platform/utf8 spec).
- C-side remainder: the 10 conformance scripts `test/conformance/scripts/dstr/`,
  cbindgen header diff, `OBS_ENABLE_RUST` switch — blocked on the C checkout.
