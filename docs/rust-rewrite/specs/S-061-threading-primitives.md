# S-061 Threading primitives (os_event, os_sem, platform threading glue)

| | |
|---|---|
| Status | Draft (round 9 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1, `libobs/util/threading-*` |
| Target crate | `crates/obs-util` (module `threading`) |
| Depends on | S-001 (allocators used by the glue) |
| Conformance | planned `test/conformance/scripts/threading/` — none yet |

## 1. Scope

The platform abstraction over OS threading primitives that the rest of libobs builds on:
`os_event` (auto/manual reset), `os_sem` (counting semaphore), thread naming, the Windows
atomic long/bool helpers, and the POSIX mutex-init helpers. The thread *creation* API
(`os_thread_*`, `os_waitable_event`, `os_message_queue`) is pinned where the callers need
it (S-024 / the graphics-thread spec); this spec covers the files of the P1-05.5 backlog.

### Reference files (all cited)

- `libobs/util/threading.h`
- `libobs/util/threading-posix.c`
- `libobs/util/threading-windows.c`, `libobs/util/threading-windows.h`

## 2. Conventions

RFC 2119. Success = 0; failure = -1 with `errno` set (POSIX) or the Win32 error converted
to an errno-style value (Windows). Timeouts take nanoseconds (`uint64_t`) on the public
API; the platform layer converts them to the OS unit.

## 3. `os_event` (`threading-posix.c` / `threading-windows.c`)

`os_event_init(os_event_t **event, enum os_event_type type)` with
`OS_EVENT_TYPE_AUTO` (resets when *any* waiter is woken — the default) and
`OS_EVENT_TYPE_MANUAL` (stays signalled until explicitly reset).

- **S-061-R01** `os_event_wait(event)`: blocks until signalled; returns 0 on wake, -1 on
  error. `os_event_timedwait(event, uint64_t timeout_ns)`: 0 on wake, 1 on timeout, -1 on
  error (the 1-on-timeout return is part of the public contract). `os_event_try(event)`:
  non-blocking — 0 if signalled (and consumed, for AUTO), 1 if not, -1 on error.
- **S-061-R02** `os_event_signal(event)`: wakes waiters; an
  AUTO event is consumed by the first waiter, a MANUAL event stays set. `os_event_reset`
  clears a MANUAL event. `os_event_destroy` releases the object.
- **S-061-R03** POSIX implementation: `pthread_mutex` + `pthread_cond` + a counter/manual
  flag; `add_ms_to_ts` converts the timeout to a `struct timespec` (the monotonic-clock
  arithmetic — rounding up to the next ms boundary is the helper's job). Windows
  implementation: Win32 `CreateEvent` (manual-reset flag from the type) / `WaitForSingleObject`.
  Implementers MUST preserve the public semantics (R01/R02), not the internal mechanism.

## 4. `os_sem` (both platform files)

- **S-061-R04** `os_sem_init(sem, uint32_t initial_count)`: counting semaphore with the
  given initial count. `os_sem_post(sem)`: increments/wakes. `os_sem_wait(sem)`: blocks
  until count > 0, decrements. `os_sem_destroy(sem)`. The Windows side uses `CreateSemaphore`
  (max count = `INT_MAX`); the POSIX side uses `sem_*`.

## 5. Thread naming and small helpers

- **S-061-R05** `os_set_thread_name(const char *name)`: best-effort (POSX `pthread_setname_np`
  where available — truncated to the platform limit; Windows `SetThreadDescription` with the
  UTF-16 conversion; unsupported → silently ignored). Never fails the calling code.
- **S-061-R06** `threading-windows.h`: the Windows atomic helpers `os_atomic_store_long`,
  `os_atomic_load_long`, `os_atomic_compare_exchange_long`, `os_atomic_store_bool`,
  `os_atomic_set_bool`, `os_atomic_load_bool` — the interlocked/`_Atomic` forms used by the
  platform-neutral atomics (semantics: seq-cst per the call sites).
- **S-061-R07** `threading.h`: `pthread_mutex_init_value` (init to `PTHREAD_MUTEX_INITIALIZER`,
  NULL-checked) and `pthread_mutex_init_recursive` (recursive attribute) — the two mutex
  initialisation styles the rest of libobs uses.

## 6. Concurrency

None of these primitives add behaviour beyond the OS primitive they wrap; the timeout
returns (R01) and the AUTO/MANUAL distinction (R03) are the two things implementers get
wrong.

## 7. Open questions

1. The exact Windows errno mapping table for event/semaphore failures (the source maps a
   handful of Win32 codes; the full table is not re-derived here).
2. `os_set_thread_name` truncation length per platform (platform limit, not specified here).
