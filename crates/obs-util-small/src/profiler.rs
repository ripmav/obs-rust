//! S-024 M7: profiler (spec-side port of `libobs 33.0.1 util/profiler.c`).
//!
//! Spec: `docs/rust-rewrite/specs/S-024-small-helpers.md` §4.7. Names are
//! interned handles compared by identity, never by text (Q-7); the clock is
//! injectable for tests (M8 precedent); the `blog(INFO/ERROR)` lines go
//! through an injectable log sink (default: stderr).

use std::collections::HashMap;
use std::sync::atomic::{AtomicBool, AtomicU64, Ordering};
use std::sync::{Mutex, OnceLock};
use std::time::Instant;

/// Log level passed to the sink (C `blog` levels the spec compares).
pub const LOG_INFO: u32 = 0;
/// Log level passed to the sink.
pub const LOG_ERROR: u32 = 1;

/// Interned profiler name (Q-7: compared by identity; `profile_store_name`
/// returns a new handle on every call, even for equal text — never
/// de-duplicated).
#[derive(Clone, Copy, Debug, PartialEq, Eq, Hash)]
pub struct ProfilerName(usize);

static NAMES: Mutex<Vec<String>> = Mutex::new(Vec::new());

/// Returns a new name handle for `name` (a fresh handle on every call).
pub fn profile_store_name(name: &str) -> ProfilerName {
    NAMES.lock().unwrap().push(name.to_string());
    ProfilerName(NAMES.lock().unwrap().len() - 1)
}

/// The stored text of `name` (diagnostics/CSV only; never for comparison).
pub fn profile_name_text(name: ProfilerName) -> String {
    NAMES.lock().unwrap()[name.0].clone()
}

// ---------------------------------------------------------------------------
// Clock (injectable; the reference uses os_gettime_ns — P1-06 is not a WP
// yet, so the monotonic source lives here behind a test seam)

static CLOCK_BOOT: OnceLock<Instant> = OnceLock::new();
/// u64::MAX = real monotonic clock; otherwise the fixed fake time (ns).
static FAKE_TIME: AtomicU64 = AtomicU64::new(u64::MAX);

fn now_ns() -> u64 {
    match FAKE_TIME.load(Ordering::Relaxed) {
        u64::MAX => now_ns_real(),
        t => t,
    }
}

fn now_ns_real() -> u64 {
    let boot = CLOCK_BOOT.get_or_init(Instant::now);
    boot.elapsed().as_nanos() as u64
}

/// Test seam: switch to a fixed fake clock at `t` ns.
#[doc(hidden)]
pub fn profiler_set_fake_clock(t: u64) {
    FAKE_TIME.store(t, Ordering::Relaxed);
}

/// Test seam: advance the fake clock by `delta` ns (real mode: no-op).
#[doc(hidden)]
pub fn profiler_step_fake_clock(delta: u64) {
    if FAKE_TIME.load(Ordering::Relaxed) != u64::MAX {
        FAKE_TIME.fetch_add(delta, Ordering::Relaxed);
    }
}

// ---------------------------------------------------------------------------
// Log sink (C `blog`; P1-13 logging is a later WP — injectable until then)

static LOG_SINK: Mutex<Option<Box<dyn FnMut(u32, &str) + Send>>> = Mutex::new(None);

/// Test seam: route profiler log lines to `sink` (replaces the default).
#[doc(hidden)]
pub fn profiler_set_log_sink(sink: impl FnMut(u32, &str) + Send + 'static) {
    *LOG_SINK.lock().unwrap() = Some(Box::new(sink));
}

fn log(level: u32, msg: &str) {
    let mut sink = LOG_SINK.lock().unwrap();
    if let Some(s) = sink.as_mut() {
        s(level, msg);
    } else {
        eprintln!("[profiler] {msg}");
    }
}

// ---------------------------------------------------------------------------
// State (spec §4.7 "State"): global enabled (default false), per-thread
// thread_enabled (default true), per-thread stack of open calls

static ENABLED: AtomicBool = AtomicBool::new(false);

/// A profiled call; `children` are arena indices (creation order).
#[derive(Debug)]
struct Call {
    name: ProfilerName,
    start: u64,
    /// 0 until the call is closed (the end time, read first in profile_end).
    end: u64,
    children: Vec<usize>,
}

/// Per-thread call arena + the stack of open calls (indices into `calls`;
/// every open call — root or nested — is on the stack, and a nested call is
/// also referenced by its parent's `children`).
struct Arena {
    calls: Vec<Call>,
    stack: Vec<usize>,
}

thread_local! {
    static THREAD_ENABLED: std::cell::Cell<bool> = const { std::cell::Cell::new(true) };
    static ARENA: std::cell::RefCell<Arena> = const { std::cell::RefCell::new(Arena { calls: Vec::new(), stack: Vec::new() }) };
}

/// A root (or nested) aggregate entry in the global profile tree.
#[derive(Debug)]
struct Entry {
    name: ProfilerName,
    /// Expected time in µs ((ns + 500) / 1000); 0 = unset.
    expected: u64,
    /// time samples {µs -> count}.
    times: HashMap<u64, u64>,
    /// between samples {µs -> count} (roots only).
    between: HashMap<u64, u64>,
    /// children ordered by first appearance.
    children: Vec<Entry>,
    /// the start time of this root's previous completed call (the between
    /// sample uses `prev.start` — that is the only field ever read).
    prev_start: Option<u64>,
}

static PROFILES: Mutex<Vec<Entry>> = Mutex::new(Vec::new());

/// Start the profiler (global `enabled = true`).
pub fn profiler_start() {
    ENABLED.store(true, Ordering::Relaxed);
}

/// Stop the profiler (global `enabled = false`).
pub fn profiler_stop() {
    ENABLED.store(false, Ordering::Relaxed);
}

/// Sets `thread_enabled = enabled` if it was false (spec §4.7 "State").
pub fn profiler_reenable_thread() {
    THREAD_ENABLED.with(|te| {
        if !te.get() {
            te.set(ENABLED.load(Ordering::Relaxed));
        }
    });
}

/// Reset all profiler state (C `profiler_free`).
pub fn profiler_free() {
    NAMES.lock().unwrap().clear();
    PROFILES.lock().unwrap().clear();
    ENABLED.store(false, Ordering::Relaxed);
    ARENA.with(|a| a.borrow_mut().calls.clear());
}

/// Spec: "if `!enabled` -> `thread_enabled = false`, return false".
fn lock_root() -> bool {
    if ENABLED.load(Ordering::Relaxed) {
        true
    } else {
        THREAD_ENABLED.with(|te| te.set(false));
        false
    }
}

/// Begin a profiled region `name` (no-op if `!thread_enabled`; the call is
/// pushed as a child of the open call, or as a new root call if none).
pub fn profile_start(name: ProfilerName) {
    if !THREAD_ENABLED.with(|te| te.get()) {
        return;
    }
    let now = now_ns();
    ARENA.with(|cell| {
        let mut a = cell.borrow_mut();
        let idx = a.calls.len();
        a.calls.push(Call {
            name,
            start: now,
            end: 0,
            children: Vec::new(),
        });
        if let Some(&top) = a.stack.last() {
            a.calls[top].children.push(idx);
        }
        a.stack.push(idx);
    });
}

/// End the profiled region `name` (spec §4.7 "Recording", incl. the
/// ancestor-search end semantics and the ERROR log lines).
pub fn profile_end(name: ProfilerName) {
    // the end time is read first
    let end = now_ns();
    if !THREAD_ENABLED.with(|te| te.get()) {
        return;
    }
    let root_idx: Option<usize> = ARENA.with(|cell| {
        let mut a = cell.borrow_mut();
        if a.stack.is_empty() {
            log(LOG_ERROR, "profile_end: no open call");
            return None;
        }
        let top = *a.stack.last().unwrap();
        if a.calls[top].name != name {
            log(
                LOG_ERROR,
                "profile_end: open call does not match the given name",
            );
            // search the ancestors (not the open call) for one named `name`
            let in_ancestors = a.stack[..a.stack.len() - 1]
                .iter()
                .any(|&i| a.calls[i].name == name);
            if !in_ancestors {
                return None;
            }
            // repeatedly end the open call (fresh clock read each) until the
            // call named `name` is open; those calls have a parent, so they
            // are simply closed
            while let Some(&t) = a.stack.last() {
                if a.calls[t].name == name {
                    break;
                }
                a.calls[t].end = now_ns();
                a.stack.pop();
            }
        }
        let t = *a.stack.last().unwrap();
        a.calls[t].end = end;
        a.stack.pop();
        // it was a root call iff nothing is left on the stack below
        if a.stack.is_empty() {
            Some(t)
        } else {
            None
        }
    });
    if let Some(idx) = root_idx {
        merge(idx);
    }
}

/// Spec §4.7 "Recording": merge the completed root call (arena index `idx`)
/// into the tree. When the stack is empty, the arena holds exactly this
/// root's subtree, so it is reset afterwards.
fn merge(root_idx: usize) {
    ARENA.with(|cell| {
        let mut a = cell.borrow_mut();
        if !lock_root() {
            // discarded; thread_enabled is now false (Q-8)
            a.calls.clear();
            return;
        }
        let mut profiles = PROFILES.lock().unwrap();
        let eidx = match profiles
            .iter()
            .position(|e| e.name == a.calls[root_idx].name)
        {
            Some(i) => i,
            None => {
                profiles.push(new_entry(a.calls[root_idx].name));
                profiles.len() - 1
            }
        };
        let prev_start = profiles[eidx].prev_start;
        let this_start = a.calls[root_idx].start;
        merge_call(&mut profiles[eidx], root_idx, &a.calls, prev_start);
        profiles[eidx].prev_start = Some(this_start);
        a.calls.clear();
    });
}

fn new_entry(name: ProfilerName) -> Entry {
    Entry {
        name,
        expected: 0,
        times: HashMap::new(),
        between: HashMap::new(),
        children: Vec::new(),
        prev_start: None,
    }
}

/// Spec §4.7 "merge_call": children first (first-appearance order), then the
/// between sample (roots with an expected and a prev), then the time sample.
fn merge_call(entry: &mut Entry, idx: usize, arena: &[Call], prev_start: Option<u64>) {
    let call = &arena[idx];
    for &cidx in &call.children {
        let cpos = match entry
            .children
            .iter()
            .position(|e| e.name == arena[cidx].name)
        {
            Some(i) => i,
            None => {
                entry.children.push(new_entry(arena[cidx].name));
                entry.children.len() - 1
            }
        };
        merge_call(&mut entry.children[cpos], cidx, arena, None);
    }
    if entry.expected != 0 {
        if let Some(ps) = prev_start {
            let sample = call.start.wrapping_sub(ps).wrapping_add(500) / 1000;
            *entry.between.entry(sample).or_insert(0) += 1;
        }
    }
    let sample = call.end.wrapping_sub(call.start).wrapping_add(500) / 1000;
    *entry.times.entry(sample).or_insert(0) += 1;
}

/// Register a root name with an expected time (spec: `expected = (ns+500)/1000`
/// µs; nothing happens if `lock_root()` fails).
pub fn profile_register_root(name: ProfilerName, expected_time_ns: u64) {
    if !lock_root() {
        return;
    }
    let mut profiles = PROFILES.lock().unwrap();
    let idx = match profiles.iter().position(|e| e.name == name) {
        Some(i) => i,
        None => {
            profiles.push(new_entry(name));
            profiles.len() - 1
        }
    };
    profiles[idx].expected = (expected_time_ns + 500) / 1000;
}

// ---------------------------------------------------------------------------
// Snapshot (spec §4.7 "Snapshot", R35-R37, R41)

/// One entry of a snapshot (an independent copy; R41).
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct SnapshotEntry {
    pub name: ProfilerName,
    /// Expected time in µs (0 = unset).
    pub expected: u64,
    /// `(µs, count)` pairs sorted by **descending** µs (R35).
    pub times: Vec<(u64, u64)>,
    /// Sum of the `times` counts.
    pub overall_count: u64,
    /// Smallest µs; `u64::MAX` without samples (Q-9, R37).
    pub min_time: u64,
    /// Largest µs; 0 without samples (Q-9, R37).
    pub max_time: u64,
    /// `(µs, count)` pairs sorted by descending µs; empty when
    /// `expected == 0` (R38).
    pub times_between_calls: Vec<(u64, u64)>,
    /// Sum of the between counts; 0 when `expected == 0`.
    pub overall_between_calls_count: u64,
    /// Between min; `u64::MAX` without samples, 0 when `expected == 0`.
    pub min_time_between_calls: u64,
    /// Between max; 0 without samples, 0 when `expected == 0`.
    pub max_time_between_calls: u64,
    /// `= expected`.
    pub expected_time_between_calls: u64,
    /// Children in order (R36).
    pub children: Vec<SnapshotEntry>,
}

/// An independent copy of the profile tree (creation order preserved).
#[derive(Debug, Clone, Default, PartialEq, Eq)]
pub struct ProfilerSnapshot {
    pub roots: Vec<SnapshotEntry>,
}

fn sorted_desc(pairs: &HashMap<u64, u64>) -> (Vec<(u64, u64)>, u64, u64, u64) {
    let mut v: Vec<(u64, u64)> = pairs.iter().map(|(k, c)| (*k, *c)).collect();
    v.sort_by_key(|(k, _)| std::cmp::Reverse(*k));
    let overall: u64 = pairs.values().sum();
    let min = if overall == 0 {
        u64::MAX
    } else {
        v.last().unwrap().0
    }; // descending sort
    let max = if overall == 0 { 0 } else { v[0].0 };
    (v, overall, min, max)
}

/// Copy the tree under the global lock (spec: `profile_snapshot_create`).
pub fn profile_snapshot_create() -> ProfilerSnapshot {
    let profiles = PROFILES.lock().unwrap();
    ProfilerSnapshot {
        roots: profiles.iter().map(copy_entry).collect(),
    }
}

fn copy_entry(e: &Entry) -> SnapshotEntry {
    let (times, overall_count, min_time, max_time) = sorted_desc(&e.times);
    let (times_between_calls, obc, bmin, bmax) = if e.expected != 0 {
        sorted_desc(&e.between)
    } else {
        (Vec::new(), 0, 0, 0)
    };
    SnapshotEntry {
        name: e.name,
        expected: e.expected,
        times,
        overall_count,
        min_time,
        max_time,
        times_between_calls,
        overall_between_calls_count: obc,
        min_time_between_calls: bmin,
        max_time_between_calls: bmax,
        expected_time_between_calls: e.expected,
        children: e.children.iter().map(copy_entry).collect(),
    }
}

/// `i = 0; while i < n: remove = false; res = f(name_i, &remove);
/// if remove: delete root i (later roots shift down); if !res: stop;
/// if !remove: i += 1` (spec §4.7, R41).
pub fn filter_roots(snap: &mut ProfilerSnapshot, mut f: impl FnMut(&str, &mut bool) -> bool) {
    let mut i = 0;
    while i < snap.roots.len() {
        let mut remove = false;
        let name = profile_name_text(snap.roots[i].name);
        let res = f(&name, &mut remove);
        if remove {
            snap.roots.remove(i);
        }
        if !res {
            break;
        }
        if !remove {
            i += 1;
        }
    }
}

/// Number of roots; NULL snapshot -> 0 (R41).
pub fn profiler_snapshot_num_roots(snap: Option<&ProfilerSnapshot>) -> usize {
    snap.map(|s| s.roots.len()).unwrap_or(0)
}

/// Number of children of an entry; NULL -> 0 (R41).
pub fn profiler_snapshot_num_children(snap: Option<&SnapshotEntry>) -> usize {
    snap.map(|e| e.children.len()).unwrap_or(0)
}

/// Roots in order; `f` returning false stops the enumeration.
pub fn profiler_snapshot_enumerate_roots(
    snap: &ProfilerSnapshot,
    mut f: impl FnMut(&str, &SnapshotEntry) -> bool,
) {
    for r in &snap.roots {
        let name = profile_name_text(r.name);
        if !f(&name, r) {
            break;
        }
    }
}

/// Children of an entry in order; `f` returning false stops.
pub fn profiler_snapshot_enumerate_children(
    entry: &SnapshotEntry,
    mut f: impl FnMut(&str, &SnapshotEntry) -> bool,
) {
    for c in &entry.children {
        let name = profile_name_text(c.name);
        if !f(&name, c) {
            break;
        }
    }
}

#[cfg(test)]
fn open_call_count() -> usize {
    ARENA.with(|c| c.borrow().stack.len())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::atomic::AtomicUsize;
    use std::sync::Arc;

    // The profiler state is process-global (PROFILES, NAMES, ENABLED, the
    // clock, the log sink); serialize the tests on it (W9 fence pattern).
    static TEST_LOCK: Mutex<()> = Mutex::new(());

    /// Test seam: a fresh deterministic world (fake clock, log collector,
    /// cleared state). Returns handles for the collectors.
    fn setup() -> (Arc<AtomicUsize>, Arc<Mutex<Vec<String>>>) {
        let n = Arc::new(AtomicUsize::new(0));
        let errs = Arc::new(Mutex::new(Vec::new()));
        let (n2, e2) = (Arc::clone(&n), Arc::clone(&errs));
        profiler_set_log_sink(move |level, msg| {
            if level == LOG_ERROR {
                e2.lock().unwrap().push(msg.to_string());
                n2.fetch_add(1, Ordering::Relaxed);
            }
        });
        profiler_free();
        profiler_set_fake_clock(0);
        (n, errs)
    }

    fn names() -> (ProfilerName, ProfilerName, ProfilerName) {
        (
            profile_store_name("rootA"),
            profile_store_name("childB"),
            profile_store_name("rootC"),
        )
    }

    // Q-7: identity handles, never de-duplicated
    #[test]
    fn q7_names_are_fresh_identity_handles() {
        let _lock = TEST_LOCK.lock().unwrap();
        setup();
        let a1 = profile_store_name("same");
        let a2 = profile_store_name("same");
        let b = profile_store_name("same");
        assert_ne!(a1, a2);
        assert_ne!(a1, b);
        assert_eq!(a1, a1);
        assert_eq!(profile_name_text(a1), "same");
    }

    // default state: enabled=false, thread_enabled=true
    #[test]
    fn default_state_disabled_profiler() {
        let _lock = TEST_LOCK.lock().unwrap();
        let (_n, _e) = setup();
        let (a, _, _) = names();
        // recording happens on the thread stack (thread_enabled defaults
        // true), but the completed root is discarded by lock_root (Q-8)
        profile_start(a);
        profiler_step_fake_clock(1_000_000);
        profile_end(a);
        assert!(PROFILES.lock().unwrap().is_empty());
        // thread_enabled is now false: further starts are no-ops
        profile_start(a);
        assert_eq!(open_call_count(), 0);
        // reenable with enabled=false keeps it false
        profiler_reenable_thread();
        assert!(!THREAD_ENABLED.with(|te| te.get()));
        // start + reenable makes recording work again
        profiler_start();
        profiler_reenable_thread();
        assert!(THREAD_ENABLED.with(|te| te.get()));
    }

    // basic recording: root with one child, exact µs samples
    #[test]
    fn basic_root_child_sampling() {
        let _lock = TEST_LOCK.lock().unwrap();
        let (_n, _e) = setup();
        profiler_start();
        let (a, b, _) = names();
        profile_register_root(a, 2_000_000); // expected = 2000 µs
                                             // t=0: A starts; t=500ns: B starts; t=1500ns: B ends (1000ns = 1 µs
                                             // sample: (1500-500+500)/1000 = 1); t=2000ns: A ends (2 µs)
        profile_start(a);
        profiler_step_fake_clock(500);
        profile_start(b);
        profiler_step_fake_clock(1000);
        profile_end(b);
        profiler_step_fake_clock(500);
        profile_end(a);
        let p = PROFILES.lock().unwrap();
        assert_eq!(p.len(), 1);
        let root = &p[0];
        assert_eq!(root.name, a);
        assert_eq!(root.expected, 2000);
        assert_eq!(root.times.get(&2), Some(&1)); // A: 2 µs once
        assert_eq!(root.children.len(), 1);
        assert_eq!(root.children[0].name, b);
        assert_eq!(root.children[0].times.get(&1), Some(&1)); // B: 1 µs once
        assert!(root.children[0].between.is_empty()); // children: no between
        assert!(root.between.is_empty()); // first completion: no prev yet
    }

    // between samples: second completion of an expected root
    #[test]
    fn between_sample_uses_prev_root_start() {
        let _lock = TEST_LOCK.lock().unwrap();
        let (_n, _e) = setup();
        profiler_start();
        let (a, _, _) = names();
        profile_register_root(a, 1_000_000);
        // A#1: t=0..1000 (1 µs)
        profile_start(a);
        profiler_step_fake_clock(1000);
        profile_end(a);
        // A#2: t=5000..6000 (1 µs); between = (5000 - 0 + 500)/1000 = 5 µs
        profiler_step_fake_clock(4000);
        profile_start(a);
        profiler_step_fake_clock(1000);
        profile_end(a);
        let p = PROFILES.lock().unwrap();
        assert_eq!(p[0].times.get(&1), Some(&2));
        assert_eq!(p[0].between.get(&5), Some(&1));
    }

    // profile_end with no open call: ERROR, nothing happens
    #[test]
    fn end_without_open_call_logs_error() {
        let _lock = TEST_LOCK.lock().unwrap();
        let (n, errs) = setup();
        profiler_start();
        let (a, _, _) = names();
        profile_end(a);
        assert_eq!(n.load(Ordering::Relaxed), 1);
        assert!(!errs.lock().unwrap().is_empty());
        assert!(PROFILES.lock().unwrap().is_empty());
    }

    // end with a name that is on the stack (ancestor): the open call is
    // closed first (fresh clock read), then the named call with the
    // original end value
    #[test]
    fn end_ancestor_closes_open_call_first() {
        let _lock = TEST_LOCK.lock().unwrap();
        let (n, _e) = setup();
        profiler_start();
        let (a, b, _) = names();
        // A: t=0, B: t=1000; end(A) read at t=3000 -> B ends at t=3000
        // (2000ns = 2 µs), A ends at the original t=3000 (3 µs)
        profile_start(a);
        profiler_step_fake_clock(1000);
        profile_start(b);
        profiler_step_fake_clock(2000);
        profile_end(a);
        assert_eq!(n.load(Ordering::Relaxed), 1); // the mismatch ERROR
        let p = PROFILES.lock().unwrap();
        assert_eq!(p[0].times.get(&3), Some(&1)); // A: (3000-0+500)/1000
        assert_eq!(p[0].children[0].times.get(&2), Some(&1)); // B: (3000-1000+500)/1000
        assert_eq!(open_call_count(), 0);
    }

    // end with a name not on the stack: ERROR, nothing closed
    #[test]
    fn end_unknown_name_closes_nothing() {
        let _lock = TEST_LOCK.lock().unwrap();
        let (n, _e) = setup();
        profiler_start();
        let (a, b, c) = names();
        profile_start(a);
        profile_start(b);
        profile_end(c);
        assert_eq!(n.load(Ordering::Relaxed), 1);
        // both calls still open
        assert_eq!(open_call_count(), 2);
        // cleanup: close properly
        profiler_step_fake_clock(1000);
        profile_end(b);
        profile_end(a);
        assert_eq!(PROFILES.lock().unwrap().len(), 1);
    }

    // root creation order is the snapshot order; children by first appearance
    #[test]
    fn entry_order_first_appearance() {
        let _lock = TEST_LOCK.lock().unwrap();
        let (_n, _e) = setup();
        profiler_start();
        let (a, b, c) = names();
        // C first, then A (with child B)
        profile_start(c);
        profiler_step_fake_clock(1000);
        profile_end(c);
        profile_start(a);
        profile_start(b);
        profiler_step_fake_clock(1000);
        profile_end(b);
        profiler_step_fake_clock(1000);
        profile_end(a);
        let p = PROFILES.lock().unwrap();
        assert_eq!(p[0].name, c);
        assert_eq!(p[1].name, a);
        assert_eq!(p[1].children[0].name, b);
    }

    // snapshot: sorted desc, counts, min/max, Q-9 sentinels, between fields
    #[test]
    fn snapshot_fields_and_q9_sentinels() {
        let _lock = TEST_LOCK.lock().unwrap();
        let (_n, _e) = setup();
        profiler_start();
        let (a, b, c) = names();
        profile_register_root(a, 2_000_000); // expected, but no between samples yet
        profile_start(a);
        profiler_step_fake_clock(500);
        profile_start(b);
        profiler_step_fake_clock(1000);
        profile_end(b); // B: 1 µs
        profiler_step_fake_clock(500);
        profile_end(a); // A: 2 µs
                        // A again: 3 µs -> A samples {3:1, 2:1}
        profiler_step_fake_clock(3000);
        profile_start(a);
        profiler_step_fake_clock(3000);
        profile_end(a);
        // a registered-but-never-called root: no samples (Q-9)
        profile_register_root(c, 1_000_000);
        let snap = profile_snapshot_create();
        assert_eq!(snap.roots.len(), 2);
        let r = &snap.roots[0];
        assert_eq!(r.name, a);
        assert_eq!(r.times, vec![(3, 1), (2, 1)]); // descending µs
        assert_eq!(r.overall_count, 2);
        assert_eq!(r.min_time, 2);
        assert_eq!(r.max_time, 3);
        // A completed twice with expected != 0: one between sample
        // (5000 - 0 + 500)/1000 = 5 µs
        assert_eq!(r.times_between_calls, vec![(5, 1)]);
        assert_eq!(r.overall_between_calls_count, 1);
        assert_eq!(r.min_time_between_calls, 5);
        assert_eq!(r.max_time_between_calls, 5);
        assert_eq!(r.expected_time_between_calls, 2000);
        let c0 = &r.children[0];
        assert_eq!(c0.name, b);
        assert_eq!(c0.times, vec![(1, 1)]);
        assert_eq!(c0.min_time, 1);
        assert_eq!(c0.max_time, 1);
        assert_eq!(c0.expected, 0);
        assert_eq!(c0.min_time_between_calls, 0); // expected == 0 -> zeros
        assert_eq!(c0.expected_time_between_calls, 0);
        // the uncalled registered root (creation order: a, c)
        let u = &snap.roots[1];
        assert_eq!(u.name, c);
        assert!(u.times.is_empty());
        assert_eq!(u.overall_count, 0);
        assert_eq!(u.min_time, u64::MAX);
        assert_eq!(u.max_time, 0);
        assert_eq!(u.expected, 1000);
        assert_eq!(u.min_time_between_calls, u64::MAX);
    }

    // snapshot is an independent copy (R41)
    #[test]
    fn snapshot_is_independent_copy() {
        let _lock = TEST_LOCK.lock().unwrap();
        let (_n, _e) = setup();
        profiler_start();
        let (a, _, _) = names();
        profile_start(a);
        profiler_step_fake_clock(1000);
        profile_end(a);
        let snap = profile_snapshot_create();
        // more recording after the snapshot
        profiler_step_fake_clock(5000);
        profile_start(a);
        profiler_step_fake_clock(9000);
        profile_end(a);
        assert_eq!(snap.roots[0].overall_count, 1); // unchanged
        assert_eq!(PROFILES.lock().unwrap()[0].times.len(), 2);
    }

    // filter_roots: the spec loop (remove + stop semantics)
    #[test]
    fn filter_roots_loop_semantics() {
        let _lock = TEST_LOCK.lock().unwrap();
        let (_n, _e) = setup();
        profiler_start();
        let (a, b, c) = names();
        for n in [a, b, c] {
            profile_start(n);
            profiler_step_fake_clock(1000);
            profile_end(n);
        }
        // remove the middle root (b); later roots shift down
        let mut seen = Vec::new();
        let mut snap = profile_snapshot_create();
        filter_roots(&mut snap, |name, remove| {
            seen.push(name.to_string());
            if name == "childB" {
                *remove = true;
            }
            true
        });
        assert_eq!(seen, vec!["rootA", "childB", "rootC"]);
        assert_eq!(snap.roots.len(), 2);
        assert_eq!(snap.roots[0].name, a);
        assert_eq!(snap.roots[1].name, c);
        // f returning false stops the enumeration (root a untouched)
        let mut snap2 = profile_snapshot_create();
        filter_roots(&mut snap2, |_name, _remove| false);
        assert_eq!(snap2.roots.len(), 3);
    }

    // accessors accept NULL (R41)
    #[test]
    fn accessors_accept_null() {
        let _lock = TEST_LOCK.lock().unwrap();
        setup();
        assert_eq!(profiler_snapshot_num_roots(None), 0);
        assert_eq!(profiler_snapshot_num_children(None), 0);
        let snap = profile_snapshot_create();
        assert_eq!(profiler_snapshot_num_roots(Some(&snap)), 0);
    }

    // enumerate stops on false
    #[test]
    fn enumerate_roots_children() {
        let _lock = TEST_LOCK.lock().unwrap();
        let (_n, _e) = setup();
        profiler_start();
        let (a, b, c) = names();
        profile_start(a);
        profile_start(b);
        profiler_step_fake_clock(1000);
        profile_end(b);
        profiler_step_fake_clock(1000);
        profile_end(a);
        profile_start(c);
        profiler_step_fake_clock(1000);
        profile_end(c);
        let snap = profile_snapshot_create();
        let mut roots = Vec::new();
        profiler_snapshot_enumerate_roots(&snap, |name, _e| {
            roots.push(name.to_string());
            true
        });
        assert_eq!(roots, vec!["rootA", "rootC"]);
        let mut only_first = Vec::new();
        profiler_snapshot_enumerate_roots(&snap, |name, _e| {
            only_first.push(name.to_string());
            false // stop after the first
        });
        assert_eq!(only_first, vec!["rootA"]);
        let mut kids = Vec::new();
        profiler_snapshot_enumerate_children(&snap.roots[0], |name, _e| {
            kids.push(name.to_string());
            true
        });
        assert_eq!(kids, vec!["childB"]);
    }
}
