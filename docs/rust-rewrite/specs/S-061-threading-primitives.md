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

RFC 2119. 0 = success. The `os_event_*` functions return the platform error code on
failure (POSIX: the raw errno of the pthread call; Windows: `EAGAIN`/`ETIMEDOUT`/
`EINVAL`); a timeout returns `ETIMEDOUT` and `os_event_try` with no signal returns
`EAGAIN` — on both platforms, and callers compare against these constants. `os_sem_*`
return 0/-1. Timeouts take milliseconds (`unsigned long`) on the public API; the
platform layer uses them as-is.

## 3. `os_event` (`threading-posix.c` / `threading-windows.c`)

`os_event_init(os_event_t **event, enum os_event_type type)` with
`OS_EVENT_TYPE_AUTO` (resets when *any* waiter is woken — the default) and
`OS_EVENT_TYPE_MANUAL` (stays signalled until explicitly reset).

- **S-061-R01** `os_event_wait(event)`: blocks until signalled; returns 0 on wake, a
  non-zero errno on error (POSIX: the raw pthread errno; Windows: `EINVAL`).
  `os_event_timedwait(event, unsigned long timeout_ms)`: 0 on wake, `ETIMEDOUT` on
  timeout, non-zero errno on error (the `ETIMEDOUT` return is part of the public
  contract — callers compare against it). `os_event_try(event)`: non-blocking — 0 if
  signalled (and consumed, for AUTO), `EAGAIN` if not, non-zero errno on error.
- **S-061-R02** `os_event_signal(event)`: wakes waiters; an
  AUTO event is consumed by the first waiter, a MANUAL event stays set. `os_event_reset`
  clears a MANUAL event. `os_event_destroy` releases the object.
- **S-061-R03** POSIX implementation: `pthread_mutex` + `pthread_cond` + a `signalled`
  flag and a `manual` flag; `os_event_timedwait` builds the deadline from the wall clock
  (`CLOCK_REALTIME`; `gettimeofday` on Apple/MinGW) and `add_ms_to_ts` does the exact
  ms→`struct timespec` conversion (no rounding). Windows implementation: Win32
  `CreateEvent` (manual-reset flag from the type) / `WaitForSingleObject`. Implementers
  MUST preserve the public semantics (R01/R02), not the internal mechanism.

## 4. `os_sem` (both platform files)

- **S-061-R04** `os_sem_init(sem, int initial_count)`: counting semaphore with the
  given initial count. `os_sem_post(sem)`: increments/wakes. `os_sem_wait(sem)`: blocks
  until count > 0, decrements. `os_sem_destroy(sem)`. The Windows side uses `CreateSemaphore`
  (max count = `INT_MAX`); the POSIX side uses `sem_*` (Mach semaphores on Apple).

## 5. Thread naming and small helpers

- **S-061-R05** `os_set_thread_name(const char *name)`: best-effort (POSX `pthread_setname_np`
  where available — truncated to the platform limit; Windows `SetThreadDescription` with the
  UTF-16 conversion; unsupported → silently ignored). Never fails the calling code.
- **S-061-R06** `threading-windows.h`: the Windows atomic helpers `os_atomic_store_long`,
  `os_atomic_load_long`, `os_atomic_compare_exchange_long`, `os_atomic_store_bool`,
  `os_atomic_set_bool`, `os_atomic_load_bool`, plus `os_atomic_inc_long`,
  `os_atomic_dec_long`, `os_atomic_set_long`/`os_atomic_exchange_long`,
  `os_atomic_compare_swap_long` and `os_atomic_exchange_bool` — the interlocked forms used
  by the platform-neutral atomics (x86: interlocked intrinsics; ARM: acquire/release
  barrier forms).
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
2. `os_set_thread_name` truncation length per platform — the source pins 15 chars on
   glibc (`bstrdup_n(name, 15)`); Apple/FreeBSD paths pass the name through untruncated.

Round 10 second-author check (2026-11-05): corrected the timeout unit and return-value
contract (was: nanoseconds `uint64_t` with 1-on-timeout / 1-when-not-signalled / -1
errors; source: `unsigned long milliseconds`, `ETIMEDOUT` on timeout, `EAGAIN` from
`os_event_try`, raw errno on error — confirmed by call sites in obs-output.c,
obs-hotkey.c, happy-eyeballs.c); corrected R03 clock/mechanism claims (was: monotonic
clock, counter flag, ms-boundary rounding; source: wall clock, signalled+manual bools,
exact ms→timespec); corrected R04 param type (was `uint32_t`; source `int`); completed
the R06 helper list and dropped the unverified seq-cst claim (ARM paths are
acquire/release); pinned the glibc 15-char thread-name limit.
