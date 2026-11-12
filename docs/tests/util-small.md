# obs-util-small (S-024 M2/M6) — test matrix

WP-16 (P1-14.3), crate `crates/obs-util-small`. Spec:
`docs/rust-rewrite/specs/S-024-small-helpers.md` §4.2 (M2 128-bit) and
§4.6 (M6 task queue), rules R08-R10 and R29-R34.

| Area | Pinned by test | Conformance vector |
| --- | --- | --- |
| **M2 layout (R10)** | | |
| `size_of == 16`, `i32 = [1,2,3,4]` ↔ `low=…0000000200000001 high=…0000000400000003` | `u128_layout` | `helpers/u128-add` (layout line) |
| **M2 add (R08)** | | |
| 10 cases: zero, carry into high, 2^128 wrap, max+max, mixed-limb sums | `u128_add` | `helpers/u128-add` |
| **M2 mul (R08)** | | |
| 13 cases: zeros, max×1, max×max (high = 2^64−2), 2^32×2^32, 10^9×10^9, 90000×48000, cross-limb products | `u128_mul64` | `helpers/u128-mul64` |
| **M2 div (R08)** | | |
| 12 cases: small, max/1, max/0xffffffff (quotient > 64 bits), /2, 2^64/2, 2^64/3, truncation (100/200) | `u128_div32` | `helpers/u128-div32` |
| **M2 shift (R08)** | | |
| `a*2^32` (5 cases incl. max → high 0xffffffff) and `a*2^64` (3 cases) | `u128_shift` | `helpers/u128-shift` |
| **M2 mul_div64 (R09/Q-2)** | | |
| 15 cases where the reference and the exact value agree (pinned against the `exact=` column) | `mul_div64` | `helpers/muldiv64` |
| Q-2: the exact value wins over the reference's wrapping formula (10^14×10^14 → 10842021 not 1; 5×max/max → 5 not 0) | `mul_div64_quirk_overflow` | `helpers/muldiv64-quirk-overflow` |
| Q-2: an exact quotient that does not fit in u64 saturates to `u64::MAX` (the documented choice) | `mul_div64_quirk_overflow` | `helpers/muldiv64-quirk-overflow` |
| **D-1 (divisor 0)** | | |
| `util_div128_32(_, 0)` and `util_mul_div64(_, _, 0)` panic (the reference raises SIGFPE; no value returned) | `u128_defect_div0`, `mul_div64_by_zero` | `helpers/u128-defect-div0` |
| **M6 task queue (R29-R34)** | | |
| FIFO order on the worker thread, `wait` returns true after a batch | `task_order` | `helpers/task-order` |
| R31: a task queued from a running task runs before `wait()` returns (the marker is deferred behind it) | `task_requeue` | `helpers/task-requeue` |
| R32: idle wait → false; a gate already in flight when the wait starts does not count (the worker took it before the marker) | `task_wait_idle` | `helpers/task-wait-idle` |
| R34: `inside` true only on the queue's own worker, false on the main thread and another queue's worker | `task_inside` | `helpers/task-inside` |
| R33: tasks queued before `destroy` still run; destroy joins after they complete | `task_destroy` | `helpers/task-destroy` |
| R30 (representable part): wait on a fresh idle queue → false | `task_null` | `helpers/task-null` |

## Documented deviations (see the module docs)

- **Q-2 saturation**: for an exact quotient > 2^64−1 the port
  saturates to `u64::MAX` (the spec allows panic, saturate or wrap;
  the reference's value is meaningless there).
- **C-NULL arguments** (`queue_task(NULL, …)`, `wait(NULL)`,
  `destroy(NULL)`, `inside(NULL)`): unrepresentable in the safe API;
  the FFI layer maps them per R30 (the reference dereferences
  `inside(NULL)` and is out of contract there).
- **Thread names**: the worker thread is named `obs-task-queue` (the
  C thread name is not pinned by the spec).

## Not covered yet (C-checkout dependent / later phases)

- The P1-14 apply recipe steps for these files (the `extern "C"`
  shims, cbindgen diff, `OBS_ENABLE_RUST=ON`) — C checkout.
- M6 thread-creation failure (returns NULL) and allocation-failure
  paths (S-024 §7: not covered by vectors).
