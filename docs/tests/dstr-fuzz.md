# Test matrix: `dstr_ops` fuzz target (P1-02.4, code half)

- Test file: `fuzz/fuzz_targets/dstr_ops.rs` (standalone `cargo-fuzz` crate, `fuzz/`)
- Task: P1-02.4 (the fuzz half; the recipe half — CMake exclusion of
  `dstr.c` — is E13S1)
- Golden files: none (seeds in `fuzz/corpus/dstr_ops/`)

The target applies a random **op sequence** (parsed from the fuzz input as
`op, p1, p2` triples) to the Rust `obs_util::dstr` and to the pinned C
`libobs/util/dstr.c` (compiled into the harness by `fuzz/build.rs` together
with `libobs/util/utf8.c` and the real `os_utf8_to_wcs`/`os_utf8_to_wcs_ptr`
from `platform.c`). After **every** op the two states must agree
byte-for-byte: `len`, `capacity`, content, and the NUL terminator (for
`len > 0`), plus the secondary dstr pair.

| Case               | Ops driven (16)                                                                      | Invariant checked                                                                                                                                                                 | Status |
| ------------------ | ------------------------------------------------------------------------------------ | --------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ------ |
| allocation         | `init` (unallocated only), `free`, `reserve`, `ensure`, `resize` (shrink/no-op only) | `len`/`capacity` equal; NUL present when `len > 0`; no leak (ASan/LSan in the harness)                                                                                            | pass   |
| copy/append        | `copy`, `ncopy`, `cat`, `ncat`, `cat_ch` (incl. `0`)                                 | content + `len` + `capacity` equal after every op                                                                                                                                 | pass   |
| insert             | `insert`, `insert_ch` (incl. `0`), `insert_dstr` — `idx <= len` only                 | tail shift incl. the NUL; capacity arithmetic equal                                                                                                                               | pass   |
| NUL-leading guards | all append/insert ops after `ncopy`/`cat_ch(0)` produces a NUL-leading buffer        | `copy`/`ncat`/`insert` free/no-op on a NUL-leading source (S-006-R10/R13/R15: \`!array                                                                                            |        |
| depad              | `depad` on padded, all-pad, NUL-leading, and NUL-embedded states                     | strlen view (S-006-R22): embedded NUL truncates, NUL-leading frees, pads removed — C and Rust agree                                                                               | pass   |
| case ops (gated)   | `to_upper`/`to_lower` on **NUL-free ASCII** content only                             | ASCII fold identical (Q-7: non-ASCII mapping is locale-dependent in the C reference; the C round-trip also truncates at an embedded NUL — both off-domain for the byte-wise port) | pass   |

Gates / caller-contract restrictions (documented in the target module):

- `init` only on an unallocated dstr (C `dstr_init` zeroes without freeing —
  re-initing an allocated dstr leaks in C but not in the port, which frees
  first; the observable divergence is a caller bug, not a port defect).
- `insert*` with `idx <= len` (with `idx > len` the C shift count
  `len - idx + 1` double-wraps to 0 — silent corruption in C; the port
  zero-fills the fresh allocation instead).
- `resize` with `n <= len` (an extending resize leaves `[old_len, n)`
  uninitialised in C, S-006-R03, while the port zero-fills — a documented
  defensive deviation).
- `depad` skipped on a reserved-but-empty state (C runs `strlen` over the
  uninitialised buffer).
- C `dstr_insert_ch` non-fast path (Q-8, S-006): shifts **one byte past**
  the guaranteed tail when the block has no slack (1-byte heap over-read +
  over-write). The port is correct; the harness gives the C block one grown
  generation of headroom for those ops and normalises the capacity
  afterwards, so the stray byte lands past the NUL (unobservable).

Findings the target caught (fixed in the port, regression tests in
`crates/obs-util/src/dstr.rs`):

- `dstr_nul_leading_guards` — `copy`/`ncat`/`insert` were missing the C
  NUL-leading guards; `cat_dstr`/`insert_dstr` were delegating to the
  guarded paths and so wrongly skipped NUL-leading sources (C copies
  `str->len` bytes verbatim).
- `dstr_depad_strlen_view` — `depad` worked on the len-bounded view; C
  works on the strlen view (embedded NUL truncates the string).

Verification: two 300 s runs (aarch64, ASan+LSan): 1,136,322 + 1,046,587
runs, zero crashes, zero leaks, zero divergences.
