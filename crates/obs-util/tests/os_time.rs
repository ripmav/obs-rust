//! P1-06.1 Done-when: `os_gettime_ns` never decreases in a 10 s loop.
//!
//! Kept in its own integration binary (the `threading_stress.rs` house
//! pattern) so the lib test run stays fast; takes ~10 s.

#![cfg(unix)]

use obs_util::os_time::os_gettime_ns;

/// R100: tight read loop for 10 s — no read may be smaller than the
/// previous one.
#[test]
#[cfg_attr(miri, ignore)]
fn monotonic_10s() {
    let start = os_gettime_ns();
    let end = start + 10_000_000_000;
    let mut prev = start;
    while os_gettime_ns() < end {
        let t = os_gettime_ns();
        assert!(t >= prev, "monotonic clock decreased: {t} < {prev}");
        prev = t;
    }
}
