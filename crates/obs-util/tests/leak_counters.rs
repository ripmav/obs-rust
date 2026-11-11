//! Process-wide allocation-counter checks (spec S-001 R04–R09).
//!
//! These assertions read `bmem::bnum_allocs()`, which is a PROCESS-wide
//! counter: they must not run in parallel with the unit tests (whose
//! containers allocate live blocks). This dedicated binary runs a single
//! test function, sequentially, in its own process.

use obs_util::bmem::{
    BMem, bfree_raw, bmalloc_raw, bmemdup, bnum_allocs, brealloc_raw, bstrdup, bstrdup_n, bzalloc,
};
use obs_util::darray::DArray;
use obs_util::deque::Deque;

#[test]
fn process_wide_counter_is_balanced() {
    // S-001-R04/R05/R09: alloc/free cycle returns the counter to baseline.
    let base = bnum_allocs();
    {
        let a = unsafe { bmalloc_raw(10) };
        let b = unsafe { bmalloc_raw(100) };
        let c = unsafe { bmalloc_raw(1000) };
        assert_eq!(bnum_allocs(), base + 3);
        unsafe { bfree_raw(c) };
        unsafe { bfree_raw(b) };
        assert_eq!(bnum_allocs(), base + 1);
        unsafe { bfree_raw(a) };
    }
    assert_eq!(bnum_allocs(), base);

    // R05: bfree(NULL) is a no-op.
    let base = bnum_allocs();
    unsafe { bfree_raw(std::ptr::null_mut()) };
    assert_eq!(bnum_allocs(), base);

    // R06: brealloc(NULL, n) is an alloc — net +1.
    let p = unsafe { brealloc_raw(std::ptr::null_mut(), 64) };
    assert_eq!(bnum_allocs(), base + 1);
    unsafe { bfree_raw(p) };
    assert_eq!(bnum_allocs(), base);

    // R08: realloc keeps the live-block count (1 in, 1 out).
    let old = unsafe { bmalloc_raw(16) };
    let grown = unsafe { brealloc_raw(old, 256) };
    assert_eq!(bnum_allocs(), base + 1);
    let shrunk = unsafe { brealloc_raw(grown, 32) };
    assert_eq!(bnum_allocs(), base + 1);
    unsafe { bfree_raw(shrunk) };
    assert_eq!(bnum_allocs(), base);

    // R11/R12/R13: the safe helpers each add exactly one live block.
    let d = bmemdup(&[1u8, 2, 3, 4, 5]);
    let z = bzalloc(64);
    let s = bstrdup(b"hola");
    let n = bstrdup_n(b"hola", 2);
    assert_eq!(bnum_allocs(), base + 4);
    drop((d, z));
    drop(s);
    drop(n);
    assert_eq!(bnum_allocs(), base);

    // R05/R09: into_raw/from_raw is counter-neutral.
    let s = bstrdup(b"abc");
    let raw = s.into_raw();
    assert_eq!(bnum_allocs(), base + 1);
    let _back = unsafe { BMem::from_raw(raw, 4) };
    assert_eq!(bnum_allocs(), base + 1);
    drop(_back);
    assert_eq!(bnum_allocs(), base);

    // S-001 + S-005: the containers free exactly what they allocated.
    let base = bnum_allocs();
    {
        let mut d = DArray::<u64>::new();
        for _ in 0..10 {
            d.push_back(&1);
            d.resize(d.len() + 2);
        }
        assert_eq!(d.len(), 30);
    }
    assert_eq!(bnum_allocs(), base);

    let base = bnum_allocs();
    {
        let mut q = Deque::new();
        for _ in 0..10 {
            q.push_back(&[1u8, 2, 3]);
            q.push_front(&[4u8]);
        }
        assert_eq!(q.len(), 40);
    }
    assert_eq!(bnum_allocs(), base);

    // And still consistent after all of the above.
    assert!(bnum_allocs() >= 0);
}
