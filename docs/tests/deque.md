# Test matrix: deque (obs-util)

- Test files: `crates/obs-util/src/deque.rs` (unit + property tests)
- Task: P1-04 (spec S-005, deque half; `darray` half lands with P1-03)
- Golden files: none (byte-level expectations are inline from S-005 algorithms)

| Case | Input | Expected | Platforms | Status |
|---|---|---|---|---|
| layout_matches_c_struct | `size_of`/`offset_of` | 5×`usize`, offsets 0…4w (S-005 §5 C field order) | Linux, Windows, macOS | pass |
| init_is_zeroed | `Deque::new()` | null data, all counters 0 (R-D01) | Linux, Windows, macOS | pass |
| push_back_growth_sequence | 5 single-byte pushes | capacity sequence 1,2,4,4,8 (R-D02) | Linux, Windows, macOS | pass |
| q_d1_end_pos_may_equal_capacity | fill to exactly capacity | `end_pos == capacity` (not 0) (Q-D1) | Linux, Windows, macOS | pass |
| push_back_growth_kills_wrap | pushes through 3 growths | capacity read AFTER `ensure_capacity` — growth absorbs the wrap (R-D05) | Linux, Windows, macOS | pass |
| push_back_real_wraparound | n > capacity, no growth | split copy at `capacity - end_pos` (algorithm 6.1) | Linux, Windows, macOS | pass |
| push_front_empty_and_plain | push on empty / non-empty | empty: start 0 end n; plain: start −= n; start 0 with growth: wrap branch (R-D06) | Linux, Windows, macOS | pass |
| push_front_wraparound | start < n after reserve | head at buffer end, tail at 0 (algorithm 6.2) | Linux, Windows, macOS | pass |
| pop_back_retreats_to_buffer_end | retreat with `end_pos <= n` | `end_pos = capacity - (n - end_pos)`; can land exactly on capacity (R-D09, Q-D1) | Linux, Windows, macOS | pass |
| pop_back_end_pos_le_n_branch | wrap positions | byte-correct pops incl. emptying to (0,0,0) (R-D09) | Linux, Windows, macOS | pass |
| place_overwrites_and_upsizes | `place` across wrap, `pos+n > size` | overwrites in logical order; upsizes to `pos+n` first (R-D10) | Linux, Windows, macOS | pass |
| upsize_zero_fills_and_wraps_end | upsize with wrap | zeros only in the added logical tail (R-D11) | Linux, Windows, macOS | pass |
| upsize_with_end_at_capacity_does_not_hang | Q-D1 state, upsize | zero-fill wraps, no stall (Q-D1 × R-D11) | Linux, Windows, macOS | pass |
| reserve_exact_and_reorder_wrapped | `reserve(16)` on wrapped data | exact growth (no doubling); tail moved up by the size difference (R-D03/R-D04) | Linux, Windows, macOS | pass |
| peek_front_back_and_deque_data | peeks + `deque_data` | logical-order copies, wrapping reads; `idx >= size` → None (R-D08/R-D12) | Linux, Windows, macOS | pass |
| zero_pushes | `push_back_zero`/`push_front_zero` | same layout effects as pushes, zero bytes (R-D07) | Linux, Windows, macOS | pass |
| random_ops_match_vecdeque_model | 2000 random ops (push/pop/peek/place/upsize/reserve) | logical bytes equal a `VecDeque` model after every op | Linux, Windows, macOS | pass |
| drop_returns_memory_to_bmem | 40 bytes through 10 push cycles | `bnum_allocs()` back to baseline (R-D01 + S-001) | Linux, Windows, macOS | pass |

Notes:
- The random-op model test is the primary logic safety net (deterministic
  LCG seed `0x5eed_c0de`). Comparison against the C implementation
  (conformance scripts `test/conformance/scripts/deque/`, 5 scripts) is the
  C-checkout substep, blocked on `../obs-studio` (phase-1 doc S2).
- Layout compatibility vs the real C header (`util/deque.h`) is likewise a
  C-checkout substep; until then the field order/widths are pinned from
  S-005 §5.
- Memory flows exclusively through the S-001 allocator
  (`bmem::brealloc_raw`/`bfree_raw`); the drop test pins that no allocation
  is leaked or double-freed. Miri runs the whole module green.
- `end_pos == 0` with `size > 0` is believed unreachable through the public
  operations (S-005 §6.3) — debug-asserted in `peek_back`.
