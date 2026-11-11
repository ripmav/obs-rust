# Test matrix: `calldata` parameter stack (obs-signal)

- Test file: `crates/obs-signal/src/calldata.rs` (unit tests per conformance vector)
- Task: P1-06 (spec S-002 R01–R16, R20–R25)
- Golden files: none (the 12 C conformance scripts arrive with the C checkout)

| Case | Input | Expected | Platforms | Status |
|---|---|---|---|---|
| layout | size/offset of `Calldata` | 4W; `stack` 0, `size` W, `capacity` 2W, `fixed` 3W | Linux, Windows, macOS | pass |
| vec01 basic (worked example) | `init; set_int("a", 1)` | exactly 34 bytes: name_size 2, `"a\0"`, data_size 8, LE 1, terminator; `size = 34`, `capacity = 128` (R11) | Linux, Windows, macOS | pass |
| vec02 type-is-only-size | int 1 read as float/ptr/bool | 8-byte reads succeed (Q-1: bits reinterpreted, `i64(1)` as f64 = 5e-324); bool (1 byte) fails; `get_string` succeeds on the int record (Q-3) | Linux, Windows, macOS | pass |
| vec03 overwrite-resize | int → 6-byte → 12-byte on `"a"` with `"b"` behind | in-place replace, tail shift both directions, record order preserved, name not rewritten (R12) | Linux, Windows, macOS | pass |
| vec04 null/empty strings | `None`, `Some("")`, `Some("hi")`, raw size-0 | data_size 0 → `get_string` NULL; empty string data_size 1; R15 no read from `in` | Linux, Windows, macOS | pass |
| vec05 invalid names / empty stack | empty name set/get; cleared stack read | no-op, no allocation (R10); `size == 0` reads fail (R01) | Linux, Windows, macOS | pass |
| vec06 capacity growth | 40 × 64-byte appends | `size < capacity` after every write (R14 strict invariant); capacity doubles | Linux, Windows, macOS | pass |
| vec07 fixed stack | 64-byte fixed buffer; fill to 60; overflow writes | first write via append path (R16); overflow: exact R14 log text, **no change** (R14); shrink allowed; no realloc | Linux, Windows, macOS | pass |
| vec08 clear-and-reuse | two ints, `clear`, new int | `size = W` (zero terminator), capacity unchanged, old params gone, next set appends via R13 (R21) | Linux, Windows, macOS | pass |
| vec09 raw data | 5-byte binary with embedded NUL; size-0 payload | byte-exact round trip; `size == 0` read succeeds only on zero-length payload (R03) | Linux, Windows, macOS | pass |
| vec10 order-and-duplicates | a,b,c; overwrite b; add `A`; re-add c | order a,b,c[,A]; case-sensitive names; re-add = replace, not append (R12/R20) | Linux, Windows, macOS | pass |
| vec11 long names / binary strings | 100-char name; string with embedded NULs | exact round trips | Linux, Windows, macOS | pass |
| vec12 fixed boundary (Q-4) | minimal record landing exactly on capacity; then +1 byte | `new_size == capacity` → overflow (strict `<`); `new_size = capacity - 1` fits | Linux, Windows, macOS | pass |
| R22 free leaves stale fields | set, `calldata_free` | fields stale (Q-2); `Drop` frees + resets | Linux, Windows, macOS | pass |
| R25 cd_copy_data | int + string + float source | byte-identical deep copy in a new stack; empty source resets dst | Linux, Windows, macOS | pass |

Notes / findings:
- **Root-cause bug found in review**: the spec's search walk is
  `pos += W (name) → compare → pos = name_end (data_size field) →
  read data_size → pos = data_end`. The first draft double-counted W
  and read payload bytes as `data_size` — corrupting every multi-record
  calldata. Fixed to the spec algorithm (S-002 §6) in `find_record`,
  `cd_copy_data` and the order helper.
- **Strict capacity (Q-4)**: `ensure` accepts only `new_size < capacity`;
  equality overflows. A fixed overflow logs the exact R14 text
  (`Tried to go above fixed calldata stack size!`, currently stderr
  until obs-log lands) and the write is abandoned **before** any move
  (R14: "no change at all").
- **Growable first write (R11)**: allocates `max(128, 3W + name_len +
  size)`; bytes beyond `size` are left uninitialised (Q-5) — dumps and
  Miri reads stay within `[0, size)`.
- **Not in this WP** (documented, deferred): `cd_getparam` cursor access
  (S-004 proc handlers), `cd_serialize_*` (format pinned in S-010, not
  yet written), `calldata_create/destroy` (Rust: `Calldata::init()` /
  `Drop`).
- C-side remainder: the 12 conformance scripts, cbindgen header diff,
  `OBS_ENABLE_RUST` switch — blocked on the C checkout.
