//! S-023 (R100–R101): monotonic clock and sleep primitives for unix.
//!
//! Spec-driven port of P1-06.1 (clock) + P1-06.2 (sleep) from
//! `libobs/util/platform.h` + `platform-nix.c` (the non-Apple unix part is
//! the port source). The Windows strategies (QPC / `Sleep` + yield spin)
//! and the macOS `CLOCK_UPTIME_RAW` clock are P1-05-style trailing work for
//! the platform crate (S-023); on non-Linux unix targets the sleep
//! functions are already the shared nix code, and the clock uses
//! `CLOCK_MONOTONIC` (the C reference uses `CLOCK_UPTIME_RAW` on macOS,
//! which pauses while the machine sleeps — documented deviation until the
//! macOS strategy lands).
//!
//! - `os_gettime_ns` (R100): nanoseconds of the monotonic clock
//!   (`CLOCK_MONOTONIC`) — non-zero and never decreasing.
//! - `os_sleepto_ns` (R101): interruptible `nanosleep` loop to the
//!   absolute deadline; `false` at once when the deadline is already past.
//! - `os_sleepto_ns_fast` (R101): repeated `usleep` of at most 999999
//!   microseconds, remaining time rounded up to whole microseconds; the
//!   same return contract.
//! - `os_sleep_ms` (R100): `usleep` of the requested milliseconds.
//!
//! R102 (`os_nstime_to_timespec`) is not part of this module's scope;
//! conformance vector `platform/34-nstime-to-timespec` pins it against the
//! C reference.
//!
//! Documented deviation (S-023 Q-16): the C reference computes
//! `ms * 1000` in 32 bits for `os_sleep_ms`, so for `ms >= 4_294_968` the
//! product wraps mod 2^32 (e.g. `4_294_968` wraps to 704 and the reference
//! sleeps 704 microseconds — conformance vector `platform/41-sleep_ms`
//! pins the C behaviour). The port computes the product in 64 bits and
//! sleeps the full time.
//!
//! The `unsafe` blocks are `libc` clock/sleep FFI calls (the platform
//! tier, same class as `dl`/`pipe`/`os_proc`; ADR 0005 rule 1).

/// `os_gettime_ns` (S-023 R100): nanoseconds of the monotonic clock
/// (`CLOCK_MONOTONIC`) — non-zero and never decreasing.
pub fn os_gettime_ns() -> u64 {
    // SAFETY: `timespec` is plain old data; the zeroed storage is overwritten by `clock_gettime` before read.
    let mut ts: libc::timespec = unsafe { std::mem::zeroed() };
    // SAFETY: `ts` is valid storage for a timespec. `clock_gettime` with
    // `CLOCK_MONOTONIC` does not fail on unix; like the C reference the
    // return value is discarded (the zeroed start reports 0, never
    // garbage, if it ever did).
    let _ = unsafe { libc::clock_gettime(libc::CLOCK_MONOTONIC, &mut ts) };
    ts.tv_sec as u64 * 1_000_000_000 + ts.tv_nsec as u64
}

/// `os_sleepto_ns` (S-023 R101): sleeps until the monotonic clock reaches
/// `nanos` (an absolute `os_gettime_ns` timestamp) and returns `true`;
/// returns `false` at once when the deadline is already past. An
/// interrupted `nanosleep` resumes with the remaining time, as in the C
/// loop.
pub fn os_sleepto_ns(nanos: u64) -> bool {
    let current = os_gettime_ns();
    if nanos < current {
        return false;
    }
    let remain = nanos - current;
    let mut req = libc::timespec {
        tv_sec: (remain / 1_000_000_000) as libc::time_t,
        tv_nsec: (remain % 1_000_000_000) as libc::c_long,
    };
    loop {
        let mut rem = libc::timespec {
            tv_sec: 0,
            tv_nsec: 0,
        };
        // SAFETY: `req`/`rem` are valid, initialized timespecs.
        if unsafe { libc::nanosleep(&req, &mut rem) } == 0 {
            return true;
        }
        // Interrupted (EINTR): resume with the remaining time, as in the
        // C loop (`req = remain; memset(&remain, 0)`).
        req = rem;
    }
}

/// `os_sleepto_ns_fast` (S-023 R101): the fast variant — repeated `usleep`
/// of at most 999999 microseconds, the remaining time rounded up to whole
/// microseconds; same `false`-at-past-deadline return contract as
/// [`os_sleepto_ns`].
pub fn os_sleepto_ns_fast(nanos: u64) -> bool {
    let mut current = os_gettime_ns();
    if nanos < current {
        return false;
    }
    loop {
        // Round the remainder up to whole microseconds (the C formula;
        // `nanos - current` is non-negative at the loop top).
        let remain_us = (nanos - current).div_ceil(1000);
        let us = if remain_us >= 1_000_000 {
            999_999
        } else {
            remain_us
        };
        // SAFETY: `us` <= 999999 fits `useconds_t`; the C reference chunks
        // to at most 999999 the same way (and avoids its own 32-bit
        // truncation of huge remainders by construction).
        unsafe { libc::usleep(us as libc::useconds_t) };
        current = os_gettime_ns();
        if nanos <= current {
            break;
        }
    }
    true
}

/// `os_sleep_ms` (S-023 R100): sleeps at least `ms` milliseconds.
///
/// Documented deviation (S-023 Q-16): the C reference computes
/// `ms * 1000` in 32 bits, so for `ms >= 4_294_968` the product wraps
/// mod 2^32 (e.g. `4_294_968` -> 704 microseconds, pinned by conformance
/// vector `platform/41-sleep_ms`). The port computes the product in 64
/// bits and sleeps the full time.
pub fn os_sleep_ms(ms: u32) {
    // 64-bit product (S-023 Q-16; see the function docs).
    let mut remain_us = u64::from(ms) * 1000;
    while remain_us > 0 {
        // `usleep` takes a 32-bit `useconds_t`: chunk counts above it
        // (only reachable in the Q-16 deviation region, ms >= 4_294_968).
        let chunk = remain_us.min(u64::from(libc::useconds_t::MAX));
        remain_us -= chunk;
        // SAFETY: `chunk` fits `useconds_t` by construction.
        unsafe { libc::usleep(chunk as libc::useconds_t) };
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // vector 39-monotonic (S-023 R100): 10M consecutive reads (the same
    // loop count as the C vector) never decrease and are non-zero — the
    // C runner prints `monotonic 1`.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn v39_monotonic() {
        let mut prev = os_gettime_ns();
        assert_ne!(prev, 0, "os_gettime_ns must be non-zero");
        for _ in 0..10_000_000 {
            let t = os_gettime_ns();
            assert!(t >= prev, "monotonic clock decreased: {t} < {prev}");
            prev = t;
        }
    }

    // vector 40-nstime (S-023 R100/R102): the value and monotonicity
    // contract of os_gettime_ns that 40 pins — non-zero, and strictly
    // advancing across a 1 ms sleep. (The ns->timespec relationship
    // itself — linearity, anchor-once, tv_nsec range — is pinned by
    // vector 34 against the C reference, and its Rust port is out of this
    // module's scope.)
    #[test]
    #[cfg_attr(miri, ignore)]
    fn v40_nstime_value_contract() {
        let a = os_gettime_ns();
        assert!(a > 0, "os_gettime_ns must be non-zero");
        os_sleep_ms(1);
        let b = os_gettime_ns();
        assert!(b > a, "clock must advance across a 1 ms sleep: {b} <= {a}");
    }

    // vector 41-sleep_ms (S-023 R100): the 50 ms case — elapsed is in the
    // [50 ms, 5 s) bucket (the C runner prints `ge=1 lt=1`).
    #[test]
    #[cfg_attr(miri, ignore)]
    fn v41_sleep_ms_buckets() {
        let a = os_gettime_ns();
        os_sleep_ms(50);
        let el = os_gettime_ns() - a;
        assert!(el >= 50_000_000, "slept {el} ns < 50 ms");
        assert!(el < 5_000_000_000, "slept {el} ns >= 5 s");
    }

    // vector 41-sleep_ms, Q-16 line (S-023 Q-16): the C reference
    // computes ms * 1000 in 32 bits; 4_294_968 * 1000 wraps mod 2^32 to
    // 704, so C sleeps 704 microseconds (the vector prints `ge=0 lt=1`).
    // The port's documented deviation is the 64-bit product — the full
    // 4_294_968 s, which is not slept here; the arithmetic on both sides
    // is pinned instead. 4_294_967 is the last non-wrapping value: both
    // sides agree there.
    #[test]
    fn v41_sleep_ms_q16() {
        // The C reference (32-bit product).
        assert_eq!(4_294_967u32.wrapping_mul(1000), 4_294_967_000);
        assert_eq!(4_294_968u32.wrapping_mul(1000), 704);
        // The port (64-bit product): the full time, no wrap.
        assert_eq!(u64::from(4_294_968u32) * 1000, 4_294_968_000);
        assert_eq!(u64::from(u32::MAX) * 1000, 4_294_967_295_000);
    }

    // vector 42-sleepto (S-023 R101): a deadline 50 ms in the past ->
    // `false` at once; a deadline 50 ms in the future -> `true` and the
    // clock reached it (`reached=1`).
    #[test]
    #[cfg_attr(miri, ignore)]
    fn v42_sleepto() {
        assert!(
            !os_sleepto_ns(os_gettime_ns() - 50_000_000),
            "past deadline -> false"
        );
        let a = os_gettime_ns();
        let target = a + 50_000_000;
        assert!(os_sleepto_ns(target), "future deadline -> true");
        let el = os_gettime_ns() - a;
        assert!(os_gettime_ns() >= target, "deadline not reached");
        assert!(el >= 50_000_000, "slept {el} ns < 50 ms");
        assert!(el < 5_000_000_000, "slept {el} ns >= 5 s");
    }

    // vector 43-sleepto_fast (S-023 R101): same contract via the usleep
    // chunks.
    #[test]
    #[cfg_attr(miri, ignore)]
    fn v43_sleepto_fast() {
        assert!(
            !os_sleepto_ns_fast(os_gettime_ns() - 50_000_000),
            "past deadline -> false"
        );
        let a = os_gettime_ns();
        let target = a + 50_000_000;
        assert!(os_sleepto_ns_fast(target), "future deadline -> true");
        let el = os_gettime_ns() - a;
        assert!(os_gettime_ns() >= target, "deadline not reached");
        assert!(el >= 50_000_000, "slept {el} ns < 50 ms");
        assert!(el < 5_000_000_000, "slept {el} ns >= 5 s");
    }
}
