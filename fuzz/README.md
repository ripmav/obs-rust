# `obs-rust-fuzz`

`cargo-fuzz` targets for the Rust rewrite (P1-02.4). The harness is a
standalone crate (excluded from the workspace by its own `[workspace]`
table) — run it from this directory with a **nightly** toolchain:

```sh
cd fuzz
cargo +nightly fuzz build          # compile the target(s)
cargo +nightly fuzz run dstr_ops   # run until a crash (Ctrl-C to stop)
cargo +nightly fuzz run dstr_ops -- -max_total_time=30   # bounded smoke run
```

Seeds live in `corpus/dstr_ops/`; `cargo fuzz` keeps interesting new
inputs there (the corpus grows at runtime — commit only seeds, and
`target/`/`artifacts/` are git-ignored).

## `dstr_ops` (P1-02.4)

Random **op sequences** for `dstr`, Rust vs C. Each fuzz input is parsed
as an op stream (`op, p1, p2` triples); the same sequence is applied to
the Rust `obs_util::dstr` and to the pinned C `libobs/util/dstr.c`
(compiled into the harness by `build.rs`, together with
`libobs/util/utf8.c` and the real `os_utf8_to_wcs`/`os_utf8_to_wcs_ptr`
from `platform.c` — a NULL-returning conversion stub makes the C case
ops no-ops and silently hides every case-op divergence). After every
op the two states must agree byte-for-byte: `len`, `capacity`, content,
and the NUL terminator (for `len > 0`), plus a secondary dstr pair used
by the `*_dstr` ops. Runs under ASan+LSan.

The 16 ops: `init`, `free`, `copy`, `ncopy`, `cat`, `ncat`, `insert`,
`insert_ch`, `insert_dstr`, `reserve`, `resize`, `ensure`, `cat_ch`,
`depad`, `to_upper`, `to_lower`.

Caller-contract restrictions (the C reference corrupts or leaks outside
these; the port's defensive deviations are documented in
`fuzz_targets/dstr_ops.rs` and S-006):

- `init` on an unallocated dstr only (C `dstr_init` zeroes without
  freeing — a re-init of an allocated dstr leaks in C but not in the
  port, which frees first).
- `insert*` with `idx <= len` (with `idx > len` the C shift count
  `len - idx + 1` double-wraps to 0 — silent corruption in C).
- `resize` with `n <= len` (an extending resize leaves `[old_len, n)`
  uninitialised in C, S-006-R03, while the port zero-fills).
- `depad` skipped on a reserved-but-empty state (C runs `strlen` over
  the uninitialised buffer).
- the case ops (`to_upper`/`to_lower`) on **NUL-free ASCII** content
  only: S-006-R24 specifies ASCII behaviour (Q-7: non-ASCII mapping is
  locale-dependent in the C reference), and the C wide round-trip
  additionally truncates at the first embedded NUL — both off-domain for
  the byte-wise port.

C `dstr_insert_ch` non-fast path (S-006 Q-8): the C reference shifts one
byte past the guaranteed tail when the block has no slack (1-byte heap
over-read + over-write). The port is correct; the harness gives the C
block one grown generation of headroom for those ops and normalises the
observable capacity afterwards, so the stray byte lands past the NUL
(unobservable).

Findings (all fixed in the port, regression tests in
`crates/obs-util/src/dstr.rs`):

- the NUL-leading `!array || !*array` guards missing from `copy` /
  `ncat` / `insert` (C treats a NUL-leading buffer as empty);
- `cat_dstr` / `insert_dstr` delegating to the guarded paths (C copies
  `str->len` bytes verbatim — no guard);
- `depad` working on the len-bounded view instead of C's strlen view.

Status: two 300 s runs (2,182,909 runs, aarch64, ASan+LSan) — zero
crashes, leaks, or divergences. Matrix:
[`docs/tests/dstr-fuzz.md`](../docs/tests/dstr-fuzz.md).
