# profiler (S-024 M7, §4.7) — test matrix

WP-18 (P1-12.1/P1-12.2 spec-side), crate `crates/obs-util-small`, module
`profiler` (`src/profiler.rs`). Spec: `S-024-small-helpers.md` §4.7
(R35–R49). Names are interned handles compared by identity, never by text
(Q-7); `profile_store_name` returns a new handle on every call. The clock
is an injectable monotonic source (M8 precedent — P1-06 is not a WP yet);
the `blog` lines go through an injectable log sink (default: stderr;
P1-13 is a later WP). Tests are serialized on `TEST_LOCK` (W9 fence
pattern) because the profiler state is process-global.

| Area | Pinned by test | Conformance vector |
| --- | --- | --- |
| **names (R46, Q-7)** | | |
| fresh identity handles per call, never de-duplicated; text only for diagnostics | `q7_names_are_fresh_identity_handles` | `prof-*` (helpers) |
| **state (R39, Q-8)** | | |
| default enabled=false/thread_enabled=true; a root completing while stopped is discarded and disables the thread until `reenable_thread` while running | `default_state_disabled_profiler` | `prof-*` |
| **recording (R38, R40, R49)** | | |
| root with child, exact (ns+500)/1000 µs samples; children get no between samples; first completion has no between | `basic_root_child_sampling` | `prof-*` |
| between sample = start-to-start of consecutive root completions, only with `expected != 0`, from the second call on | `between_sample_uses_prev_root_start` | `prof-*` |
| `profile_end` with no open call: ERROR, nothing happens | `end_without_open_call_logs_error` | `prof-*` |
| mismatching name: ERROR; ancestor present -> open calls closed innermost first with fresh clock reads, the named call gets the original end value | `end_ancestor_closes_open_call_first` | `prof-*` |
| name not on the stack: ERROR, nothing closed | `end_unknown_name_closes_nothing` | `prof-*` |
| roots in creation order, children in first-appearance order (R36) | `entry_order_first_appearance` | `prof-*` |
| **snapshot (R35–R37, R41, Q-9)** | | |
| descending-sorted `times`, counts, min/max; Q-9 sentinels (no samples: `u64::MAX`/0/0; `expected==0` between fields zeroed); `expected_time_between_calls = expected` | `snapshot_fields_and_q9_sentinels` | `prof-*` |
| independent copy (later recording does not change it) | `snapshot_is_independent_copy` | `prof-*` |
| `filter_roots` loop: remove (later roots shift down) + stop-on-false | `filter_roots_loop_semantics` | `prof-*` |
| accessors accept NULL (0/NULL) | `accessors_accept_null` | `36-null-arguments` |
| enumerate roots/children in order, stop on false | `enumerate_roots_children` | `prof-*` |
| **report text (R42, R43, R44, Q-10)** | | |
| header/footer byte exact, single-value line (min == max) with NBSP ms | `print_single_value_line` | report goldens (helpers) |
| full statistics line: min/median/max/99th, P%-below only with `expected != 0`; Q-10 median=0 for two distinct calls | `print_full_stats_and_q10_median_zero` | report goldens (helpers) |
| calls-per-parent suffix (`lround(10r) != 10` && `calls != parent_calls`); indent ` ┣`/` ┗` for siblings | `print_parent_suffix_and_indent` | report goldens (helpers) |
| deep indent bit rules: ` ┃`/` ┣` above, ` ┗` last child, two spaces for cleared bits | `print_deep_indent_bits` | report goldens (helpers) |
| no samples -> all stats 0 -> "0 ms" | `print_no_samples` | report goldens (helpers) |
| C `%g` shapes (6 significant digits, e±06 exponents, trailing-zero strip) | `g_fmt_shapes` | report goldens (helpers) |
| **between-calls report (R45)** | | |
| mixed window: median, P/L/H accumulation algorithm (lo/hi ±2% truncated) | `between_calls_mixed_window` | report goldens (helpers) |
| `min >= hi` -> H=100 done | `between_calls_all_above` | report goldens (helpers) |
| `found_hi && min >= lo` -> P=100 done | `between_calls_all_inside` | report goldens (helpers) |
| no between samples -> all zero; `expected == 0` roots skipped; children never printed | `between_calls_zero_and_filtering` | report goldens (helpers) |
| **CSV (R47)** | | |
| depth-first rows (times with 0, between with expected, then children), header with U+00B5, `(nil)` parents for roots, first-appearance `#n` id tokens | `csv_layout_depth_first` | `prof-csv` (helpers) |
| stable unique tokens: a child shares its parent's name token; distinct handles (Q-7) get distinct tokens | `csv_tokens_stable_unique` | `prof-csv` (helpers) |
| `profiler_snapshot_dump_csv`: file content, ok on success, false on bad path | `csv_dump_to_file` | `prof-csv` (helpers) |
