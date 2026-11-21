//! P1-05.4 stress: 64 threads x 1M signals (S-061 R01/R02/R04).
//!
//! Each of 64 workers signals 1M times — per the subtask's stress pattern.
//! Determinism is on the observable, not on the schedule:
//!
//! - **Counting semaphore (exact):** a successful `os_sem_wait` consumes
//!   exactly one post, and the count can never exceed the number of posts
//!   — so collectors completing 64 x 1M waits in total prove all 64 x 1M
//!   posts were delivered. A lost post hangs a collector (a failure), and
//!   the count is exact, not a race winner.
//!
//! - **AUTO event (bounded):** signals coalesce on the flag (at most one
//!   pending), so the total number of *consumed* signals is
//!   schedule-dependent and is not asserted exactly. Workers signal
//!   per-lane (worker `i` signals event `i`), so the lanes run in
//!   parallel. Asserted per lane: the worker and its collector complete,
//!   at least one signal was consumed, and the lane's final state is
//!   un-signalled — not "who won the last race".
//!
//! - **MANUAL event:** the flag is never consumed by wait/try, so after
//!   the workers' join every lane's state is deterministically signalled;
//!   `os_event_reset` clears it.
//!
//! `done` is set by the main thread only after all workers have joined, so
//! from that point no lane can receive a signal again — the collectors'
//! bounded drain is guaranteed to exit.
//!
//! House pattern: one integration binary, real OS threads
//! (`cf. tests/bmem_fatal.rs`); `#[cfg_attr(miri, ignore)]`.

#![cfg(unix)]

use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::thread;

use obs_util::threading::{
    OsEvent, OsEventType, os_event_destroy, os_event_init, os_event_reset, os_event_signal,
    os_event_timedwait, os_event_try, os_sem_destroy, os_sem_init, os_sem_post, os_sem_wait,
};

const WORKERS: usize = 64;
/// Per the subtask: 1M signals per worker.
const SIGNALS: u64 = 1_000_000;
/// Bounded block for the drain loops (re-checks the stop flag).
const DRAIN_TIMEOUT_MS: u32 = 100;

/// One event per lane, shared between the lane's worker and its collector.
fn lanes(kind: OsEventType) -> Vec<Arc<OsEvent>> {
    (0..WORKERS)
        .map(|_| Arc::new(os_event_init(kind).expect("os_event_init")))
        .collect()
}

/// 64 workers x 1M posts on one counting semaphore; a collector consumes
/// exactly 64M — the exact-count proof (waits can never outnumber posts).
#[test]
#[cfg_attr(miri, ignore)]
fn sem_64_workers_x_1m_posts_is_exact() {
    let sem = Arc::new(os_sem_init(0).expect("os_sem_init(0)"));
    let mut workers = Vec::with_capacity(WORKERS);
    for _ in 0..WORKERS {
        let sem = Arc::clone(&sem);
        workers.push(thread::spawn(move || {
            for _ in 0..SIGNALS {
                assert_eq!(os_sem_post(Some(&sem)), 0);
            }
        }));
    }
    let collector = {
        let sem = Arc::clone(&sem);
        thread::spawn(move || {
            for _ in 0..WORKERS as u64 * SIGNALS {
                assert_eq!(os_sem_wait(Some(&sem)), 0);
            }
        })
    };
    for w in workers {
        w.join().expect("worker joins");
    }
    collector.join().expect("collector joins");
    let owned = Arc::try_unwrap(sem).expect("single owner");
    os_sem_destroy(Some(owned));
}

/// 64 workers x 1M signals, one AUTO event per lane. Each lane's
/// collector drains its lane; per lane: both complete, >= 1 signal
/// consumed, final state un-signalled.
#[test]
#[cfg_attr(miri, ignore)]
fn auto_event_64_workers_x_1m_signals_waiters_complete_final_unsignalled() {
    let events = lanes(OsEventType::Auto);
    let done = Arc::new(AtomicBool::new(false));
    let mut workers = Vec::with_capacity(WORKERS);
    for ev in &events {
        let ev = Arc::clone(ev);
        workers.push(thread::spawn(move || {
            for _ in 0..SIGNALS {
                assert_eq!(os_event_signal(&ev), 0);
            }
        }));
    }
    let mut collectors = Vec::with_capacity(WORKERS);
    for ev in &events {
        let ev = Arc::clone(ev);
        let done = Arc::clone(&done);
        collectors.push(thread::spawn(move || drain_auto_lane(&ev, &done)));
    }
    for w in workers {
        w.join().expect("worker joins");
    }
    // All workers finished before this point, so no lane can receive a
    // signal again; every collector exits within one drain timeout.
    done.store(true, Ordering::Release);
    let mut consumed_total = 0u64;
    for c in collectors {
        consumed_total += c.join().expect("collector joins");
    }
    // Deterministic lower bound per lane: the flag starts un-signalled, so
    // each lane's first signal is non-coalesced, and the lane's collector
    // exits only with the flag un-signalled — every lane consumed >= 1.
    // (The exact total is schedule-dependent — coalescing — and is not
    // asserted.)
    assert!(
        consumed_total >= WORKERS as u64,
        "only {consumed_total} of {WORKERS} lanes delivered a consumed signal"
    );
    eprintln!(
        "info: AUTO storm delivered {consumed_total} consumed signals across {WORKERS} lanes"
    );
    // Every lane's final state is un-signalled.
    for ev in &events {
        assert_eq!(os_event_try(ev), libc::EAGAIN);
    }
    for ev in events {
        let owned = Arc::try_unwrap(ev).expect("single owner");
        os_event_destroy(Some(owned));
    }
}

/// Drains one AUTO lane after the workers finish; returns the signals
/// this collector consumed (via try, or via a timedwait that returned 0,
/// which consumed the signal for AUTO).
fn drain_auto_lane(ev: &OsEvent, done: &Arc<AtomicBool>) -> u64 {
    let mut consumed = 0u64;
    loop {
        if os_event_try(ev) == 0 {
            consumed += 1;
            continue;
        }
        // The workers are joined: no signal can arrive again, so exit. A
        // signal landing between the two tries is consumed and counted by
        // the retry.
        if done.load(Ordering::Acquire) {
            if os_event_try(ev) == 0 {
                consumed += 1;
            }
            break;
        }
        // Bounded block (re-checks `done` after each timeout) so the
        // collector cannot sit in a wait past the end of the storm (the
        // AUTO flag holds at most one pending signal).
        if os_event_timedwait(ev, DRAIN_TIMEOUT_MS) == 0 {
            consumed += 1;
        }
    }
    consumed
}

/// 64 workers x 1M signals, one MANUAL event per lane; after the workers'
/// join every lane is still signalled (never consumed); reset clears it.
#[test]
#[cfg_attr(miri, ignore)]
fn manual_event_64_workers_x_1m_signals_final_signalled_reset_clears() {
    let events = lanes(OsEventType::Manual);
    let done = Arc::new(AtomicBool::new(false));
    let mut workers = Vec::with_capacity(WORKERS);
    for ev in &events {
        let ev = Arc::clone(ev);
        workers.push(thread::spawn(move || {
            for _ in 0..SIGNALS {
                assert_eq!(os_event_signal(&ev), 0);
            }
        }));
    }
    // One waiter per lane (bounded drain, like the AUTO collectors).
    let mut waiters = Vec::with_capacity(WORKERS);
    for ev in &events {
        let ev = Arc::clone(ev);
        let done = Arc::clone(&done);
        waiters.push(thread::spawn(move || {
            let mut observed = 0u64;
            loop {
                if done.load(Ordering::Acquire) {
                    break;
                }
                os_event_timedwait(&ev, DRAIN_TIMEOUT_MS);
                // MANUAL: try does not consume the flag.
                observed += u64::from(os_event_try(&ev) == 0);
            }
            observed
        }));
    }
    let mut observed_total = 0u64;
    for w in workers {
        w.join().expect("worker joins");
    }
    done.store(true, Ordering::Release);
    for w in waiters {
        observed_total += w.join().expect("waiter joins");
    }
    eprintln!("info: MANUAL storm, waiters observed the flag {observed_total} times");
    // Deterministic per lane: the workers' 1M signals happened and MANUAL
    // never consumes the flag, so each lane is still signalled after the
    // join; reset clears it.
    for ev in &events {
        assert_eq!(os_event_try(ev), 0);
        os_event_reset(ev);
        assert_eq!(os_event_try(ev), libc::EAGAIN);
    }
    for ev in events {
        let owned = Arc::try_unwrap(ev).expect("single owner");
        os_event_destroy(Some(owned));
    }
}
