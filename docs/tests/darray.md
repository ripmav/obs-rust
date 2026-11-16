# Test matrix: darray (obs-util)

- Test files: `crates/obs-util/src/darray.rs` (unit + property tests),
  `crates/obs-util/tests/leak_counters.rs` (process-wide counter / drop-leak)
- Task: P1-03 (spec S-005, darray half; `deque` half landed with P1-04)
- Golden files: none (byte-level expectations are inline from S-005 rules)

| Case | Input | Expected | Platforms | Status |
|---|---|---|---|---|
| layout_matches_c_struct | `size_of`/`offset_of` | 3×`usize`, offsets 0…2w (S-005 §3 C field order) | Linux, Windows, macOS | pass |
| init_and_free | new / push / free | null + zeroed after free (R-A01) | Linux, Windows, macOS | pass |
| growth_sequence | 6 single pushes | capacity 1,2,4,4,8,8 — ensure only grows when `num+1 > cap` (R-A02) | Linux, Windows, macOS | pass |
| ensure_grows_to_n_when_larger_than_double | `ensure_capacity` beyond doubling | `new_cap = max(capacity*2, n)`, `num` untouched (R-A02) | Linux, Windows, macOS | pass |
| reserve_exact | reserve 7 / 3 / 0 | exact growth, shrink + zero are no-ops (R-A03) | Linux, Windows, macOS | pass |
| clear_keeps_capacity | clear after 5 pushes | `num = 0`, capacity kept (R-A04) | Linux, Windows, macOS | pass |
| resize_zero_fills_and_shrinks | resize 3→6→2→0 | grow zero-fills, shrink keeps capacity (R-A05) | Linux, Windows, macOS | pass |
| push_back_and_new | pushes + `push_back_new` | index return; zeroed slot pointer (R-A06) | Linux, Windows, macOS | pass |
| push_back_array_and_darray | append 3, empty, darray | old-`num` return; empty returns `num` (R-A07) | Linux, Windows, macOS | pass |
| insert_variants | insert 0/mid/end, `insert_new`, `insert_array` | tail shifts up, slot zeroed (R-A08) | Linux, Windows, macOS | pass |
| erase_variants | erase mid/first/last, `erase_item`, `erase_range` (partial, whole) | tail shifts down, capacity unchanged (R-A09) | Linux, Windows, macOS | pass |
| find_first_match_from_and_absent | 5 elements, `from` 0…num | first `i >= from`; `None` when absent (R-A10) | Linux, Windows, macOS | pass |
| find_compares_whole_element_bytes | 4-byte `repr(C)` elements differing in last byte | whole-element compare (R-A10/Q-A2) | Linux, Windows, macOS | pass |
| copy_move_and_arrays | `copy_from`, empty-src, `copy_array`, `move_from` | empty src frees; move empties the source (R-A11) | Linux, Windows, macOS | pass |
| pop_join_and_split | pops, `join_from`, `split_into` (idx 1, idx 0, single) | Q-A1: `dst1` gets the whole source; single-element split loses the element (C defect, reproduced) (R-A12) | Linux, Windows, macOS | pass |
| move_item_and_swap | `move_item` both directions, `swap_items` | element move/byte-swap, no-ops on equal indices (R-A13) | Linux, Windows, macOS | pass |
| end_and_alloc_size | empty / 2 elements | last-element ref, `es * num` (R-A14) | Linux, Windows, macOS | pass |
| random_ops_match_vec_model | 2000 random ops (push/insert/erase/swap/resize/erase_range/find) | logical element sequence equal a `Vec` model after every op | Linux, Windows, macOS | pass |
| process_wide_counter_is_balanced | drop-leak for `DArray<u64>` (+ bmem + deque, same binary) | `bnum_allocs()` back to baseline (R-A01 + S-001) | Linux, Windows, macOS | pass |

Notes:
- The counter/drop-leak checks live in `tests/leak_counters.rs`: a
  process-wide counter cannot be asserted in parallel with the unit tests,
  so they run sequentially in a dedicated binary (one test function).
- **Q-A2 is pinned by construction, not by a padding test**: the compare
  covers `size_of::<T>()` bytes, but Rust padding bytes are unspecified and
  the compiler normalises them on copies — a dirty-padding value cannot be
  manufactured in Rust. The observable C quirk (memcmp over padding) is
  covered by the C conformance substep.
- **`darray_swap` (C) semantics**: the reference does not distinguish in
  this spec whether `swap` exchanges two elements of one array or two
  whole arrays; the port implements element swap (`swap_items`, the
  "a == b → nothing" reading of R-A13). R-A13 has no conformance vector;
  confirm the exact C signature against the checkout (S2).
- C-side remainder: conformance scripts `test/conformance/scripts/darray/`
  (4), C-driven layout test, `extern "C"` shim, cbindgen diff,
  `OBS_ENABLE_RUST` — blocked on the C checkout (phase-1 doc S2).
