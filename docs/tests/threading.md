# threading (S-061 R01–R05, R07) — test matrix

WP-24 (P1-05.1 events + P1-05.2 semaphores/thread names, spec-side),
WP-25 (P1-05.4 stress), crate `crates/obs-util`, module `threading`
(`src/threading.rs`, `#[cfg(unix)]` — the Windows HANDLE port is
P1-05.3). Spec: `S-061-threading-primitives.md` (R01–R05, R07; R06 is
the Windows-only interlocked helper set, out of scope here). The tests
encode the committed C reference vectors
`test/conformance/vectors/threading/01..06` (generated from
`libobs/util/threading-posix.c`); the vectors are the oracle, return
codes included (`0` / `11` EAGAIN / `110` ETIMEDOUT on glibc).
Mechanism note: the std `Mutex`/`Condvar` and `sem_t` back the C
mechanism (R03 frees the mechanism and pins the public semantics); the
timedwait keeps the C loop's absolute-deadline recompute.

| Area | Pinned by test | Conformance vector |
| --- | --- | --- |
| **os_event auto (R01, R02)** | | |
| AUTO lifecycle: try on un-signalled is `EAGAIN`; signal; try consumes (returns `0` then `EAGAIN`); wait consumes; reset clears; destroy incl. NULL no-op | `v01_events_auto` | `01-events-auto` |
| AUTO consumption by timedwait (pre-signalled wait returns `0` and the event is consumed) | `r01_timedwait_consumes_auto` | `01-events-auto` (coverage-table row R01) |
| **os_event manual (R02, R03)** | | |
| MANUAL not consumed by try (two consecutive `0`s); not consumed by wait (two worker waits both `0`); reset clears; timedwait after reset is `ETIMEDOUT`; double signal + wait | `v02_events_manual` | `02-events-manual` |
| **timeouts (R01, R03)** | | |
| pure timeout `ETIMEDOUT`; pre-signal `0`; signal-during-wait `0` (deterministic by the runner's worker guarantee); MANUAL stays set through timedwait; AUTO try from a worker `0` then `EAGAIN` | `v03_event_timeout` | `03-event-timeout` |
| **os_sem (R04)** | | |
| counting from 2 (two waits `0`), post, wait; counting semaphore wakes an in-flight wait (two `wsemwake` shapes, post-before-wait and wake-during-wait both `0`); negative initial count fails with `rc=-1` and is never destroyed; NULL guards: wait `-1`, post `-1`, destroy no-op | `v04_semaphores` | `04-semaphores` |
| counting: each wait decrements, each post increments; at count 0 the wait blocks until a post (is_finished probe) | `r04_counting` | `04-semaphores` (coverage-table row R04) |
| **thread name (R05)** | | |
| glibc read-back via `pthread_getname_np`: short name, exact 15, 16 truncated to 15, spaces, empty name — each on a worker thread (the C runner's `wname` shape) | `v05_thread_name` | `05-thread-name` |
| pure truncation logic: NUL cut (`abc\0def` → `abc`) and the glibc 15-byte cap, exact-15 pass-through | `thread_name_bytes_truncates` | `05-thread-name` (S-061 Q2) |
| **errno contract (§2)** | | |
| glibc constants match the conformance host (`EAGAIN=11`, `ETIMEDOUT=110` — the public contract the vectors print) | `errno_constants_match_conformance_host` (linux) | — (host check) |
| **mutex init (R07)** | | |
| `pthread_mutex_init_value` (value init, lock/unlock `0`); `pthread_mutex_init_recursive` (`0`, re-entrant double lock/unlock); NULL: `_value` no-op, `_recursive` `EINVAL` (C would deref NULL — the port never does) | `v06_mutex_init` | `06-mutex-init` |
| **stress (P1-05.4, 64 threads x 1M signals)** — `tests/threading_stress.rs` (integration, real OS threads; per-lane AUTO/MANUAL events + shared counting semaphore; `done` set by main after all workers join, so the bounded drains cannot hang) | | |
| counting semaphore exact: 64 workers x 1M posts, one collector consumes exactly 64M — a wait can only succeed by consuming a post (the count never exceeds the post count), so completion proves no post was lost (a lost post hangs the collector) | `sem_64_workers_x_1m_posts_is_exact` | — (S-061 R04 stress) |
| AUTO fan-in per lane: all workers + all lane collectors complete; each lane consumes >= 1 (the flag starts un-signalled, so the lane's first signal is non-coalesced, and the collector exits only un-signalled); final state per lane un-signalled — the exact consumed total is schedule-dependent (coalescing) and not asserted | `auto_event_64_workers_x_1m_signals_waiters_complete_final_unsignalled` | — (S-061 R01/R02 stress) |
| MANUAL per lane: after the workers' join each lane is still signalled (wait/try never consume), `os_event_reset` clears it | `manual_event_64_workers_x_1m_signals_final_signalled_reset_clears` | — (S-061 R02 stress) |

Not covered here (by design): R06 Windows interlocked helpers and the
`CreateEvent`/`CreateSemaphore` HANDLE implementations (P1-05.3,
windows-latest CI); the C-side recipe/swap of the threading module
(P1-05.4, `docs/tests/ffi-recipe.md` — `run.sh swap threading` 6/6).
Miri: the thread-spawning tests are
`#[cfg_attr(miri, ignore)]` (house pattern; the stress tests included);
the pure-logic tests (`thread_name_bytes_truncates`) run under Miri.
