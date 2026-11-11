//! `math-extra` (S-009 §5.7; `libobs/graphics/math-extra.{c,h}`).
//!
//! Small pure helpers. `rand_float` (R96): the spec does not pin the
//! reference's sequence or seeding (C library `rand()`); §7 explicitly
//! allows any thread-safe generator — a thread-local xorshift64 seeded
//! from a deterministic atomic counter ^ thread identity (no wall
//! clock, so Miri isolation stays green).

use std::cell::Cell;
use std::sync::atomic::{AtomicU64, Ordering};

thread_local! {
    static RAND_STATE: Cell<u64> = const { Cell::new(0) };
}

static SEED_COUNTER: AtomicU64 = AtomicU64::new(0x9E37_79B9_7F4A_7C15);

/// Deterministic, syscall-free seed (Miri isolation forbids the wall
/// clock): golden-ratio counter ^ thread identity.
fn rand_seed() -> u64 {
    use std::hash::{Hash, Hasher};
    let mut h = std::collections::hash_map::DefaultHasher::new();
    std::thread::current().id().hash(&mut h);
    let c = SEED_COUNTER.fetch_add(1, Ordering::Relaxed);
    (c ^ h.finish()).max(1)
}

/// S-009-R96: `rand_float(positive_only)` — uniformly distributed `f32`
/// in `[0, 1]` when `positive_only`, else in `[-1, 1]`.
#[must_use]
pub fn rand_float(positive_only: bool) -> f32 {
    let x = RAND_STATE.with(|s| {
        let mut x = s.get();
        if x == 0 {
            x = rand_seed();
            s.set(x);
        }
        // xorshift64
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        s.set(x);
        x
    });
    // 53-bit mantissa → [0, 1)
    let u = (x >> 11) as f64 * (1.0 / 9_007_199_254_740_992.0);
    if positive_only {
        u as f32
    } else {
        (u * 2.0 - 1.0) as f32
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn rand_float_range() {
        // S-009-R96: range contract only (sequence unspecified)
        let mut seen = std::collections::HashSet::new();
        for _ in 0..1000 {
            let p = rand_float(true);
            assert!((0.0..=1.0).contains(&p), "positive: {p}");
            let s = rand_float(false);
            assert!((-1.0..=1.0).contains(&s), "signed: {s}");
            seen.insert(p.to_bits());
        }
        assert!(seen.len() > 1, "values are not all identical");
    }
}
