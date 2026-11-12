//! S-024 M6: `os_task_queue` (one worker thread per queue, FIFO).
//!
//! `wait` semantics (R31/R32): the wait marker is deferred behind
//! tasks queued while it is already waiting, so a task queued from a
//! running task still runs before `wait()` returns; `wait` returns
//! true iff a user task ran after the wait started. `destroy` defers
//! its stop marker behind every pending task, joins the worker and
//! frees the queue (R33).
//!
//! C-NULL arguments (`queue_task(NULL, …)`, `wait(NULL)`,
//! `destroy(NULL)`, `inside(NULL)`) are unrepresentable in the safe
//! API (R30 maps them to `false`/no-op in the FFI layer; the reference
//! dereferences `inside(NULL)` and is out of contract there).
//!
//! Task bodies must not call `wait` on their own queue (deadlock) nor
//! destroy it (S-024 §6).

use std::sync::{Arc, Condvar, Mutex};
use std::thread::{self, JoinHandle};

enum Item {
    Task(Box<dyn FnOnce() + Send + 'static>),
    Wait(Arc<WaitSlot>),
    Stop,
}

struct WaitSlot {
    processed: Mutex<bool>,
    flag: Mutex<bool>,
    cv: Condvar,
}

struct Inner {
    lock: Mutex<std::collections::VecDeque<Item>>,
    cv: Condvar,
}

/// A task queue with one worker thread (S-024 M6).
pub struct TaskQueue {
    inner: Arc<Inner>,
    worker: Mutex<Option<JoinHandle<()>>>,
    worker_thread: Arc<Mutex<Option<thread::ThreadId>>>,
}

fn worker_loop(inner: &Inner) {
    loop {
        let item = {
            let mut g = inner.lock.lock().unwrap();
            loop {
                if g.is_empty() {
                    g = inner.cv.wait(g).unwrap();
                    continue;
                }
                // R31: a wait marker at the head with a task queued
                // behind it is moved to the back — the next task runs
                // first
                if matches!(g.front(), Some(Item::Wait(_)))
                    && g.iter().any(|i| matches!(i, Item::Task(_)))
                {
                    let w = match g.pop_front() {
                        Some(Item::Wait(w)) => w,
                        _ => unreachable!(),
                    };
                    g.push_back(Item::Wait(w));
                    continue;
                }
                break;
            }
            // a non-marker task taken while a wait marker is in the
            // queue: the wait returns true (R32)
            if matches!(g.front(), Some(Item::Task(_))) {
                for item in g.iter() {
                    if let Item::Wait(slot) = item {
                        *slot.processed.lock().unwrap() = true;
                    }
                }
            }
            g.pop_front().unwrap()
        };
        match item {
            Item::Task(task) => task(),
            Item::Wait(slot) => {
                *slot.flag.lock().unwrap() = true;
                slot.cv.notify_one();
            }
            Item::Stop => break,
        }
    }
}

/// `os_task_queue_create`: one queue, one worker thread.
pub fn task_queue_create() -> TaskQueue {
    let inner = Arc::new(Inner {
        lock: Mutex::new(std::collections::VecDeque::new()),
        cv: Condvar::new(),
    });
    let worker_thread = Mutex::new(None);
    let tid = Arc::new(worker_thread);
    let tid_w = Arc::clone(&tid);
    let inner_w = Arc::clone(&inner);
    let handle = thread::Builder::new()
        .name("obs-task-queue".into())
        .spawn(move || {
            *tid_w.lock().unwrap() = Some(thread::current().id());
            worker_loop(&inner_w);
        })
        .expect("task queue worker thread");
    TaskQueue {
        inner,
        worker: Mutex::new(Some(handle)),
        worker_thread: tid,
    }
}

/// `os_task_queue_queue_task`: append the task (FIFO) and wake the
/// worker. Returns true; a NULL queue is unrepresentable (R30: the FFI
/// layer maps it to false).
pub fn task_queue_queue_task<F>(q: &TaskQueue, task: F) -> bool
where
    F: FnOnce() + Send + 'static,
{
    {
        let mut queue = q.inner.lock.lock().unwrap();
        queue.push_back(Item::Task(Box::new(task)));
    }
    q.inner.cv.notify_one();
    true
}

/// `os_task_queue_wait` (R31/R32): true iff a user task ran after the
/// wait started; false on an idle queue. Returns only after every task
/// queued before the wait has run. A NULL queue is unrepresentable
/// (R30: the FFI layer maps it to false).
pub fn task_queue_wait(q: &TaskQueue) -> bool {
    let slot = Arc::new(WaitSlot {
        processed: Mutex::new(false),
        flag: Mutex::new(false),
        cv: Condvar::new(),
    });
    {
        let mut queue = q.inner.lock.lock().unwrap();
        queue.push_back(Item::Wait(slot.clone()));
    }
    q.inner.cv.notify_one();
    let mut g = slot.flag.lock().unwrap();
    while !*g {
        g = slot.cv.wait(g).unwrap();
    }
    drop(g);
    let processed = *slot.processed.lock().unwrap();
    processed
}

/// `os_task_queue_destroy` (R33): runs every task queued before this
/// call, joins the worker, frees the queue. A NULL queue is a no-op
/// (unrepresentable here).
pub fn task_queue_destroy(q: &TaskQueue) {
    {
        let mut queue = q.inner.lock.lock().unwrap();
        queue.push_back(Item::Stop);
    }
    q.inner.cv.notify_one();
    let handle = q.worker.lock().unwrap().take().expect("destroyed twice");
    let _ = handle.join();
}

/// `os_task_queue_inside` (R34): true only on the queue's own worker
/// thread.
pub fn task_queue_inside(q: &TaskQueue) -> bool {
    *q.worker_thread.lock().unwrap() == Some(thread::current().id())
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc;
    use std::time::Duration;

    type Log = Arc<Mutex<Vec<String>>>;

    fn log(l: &Log, line: &str) {
        l.lock().unwrap().push(line.to_string());
    }

    /// A gate task keeps the worker busy until released, so a whole
    /// batch can be queued before any of it runs. The runner's
    /// `task_gate` command prints `entered` only once the gate has
    /// actually started, and the tests rely on the same ordering: an
    /// optional `entered` sender is signalled when the gate runs.
    fn gate(
        name: &'static str,
        l: Log,
        rx: mpsc::Receiver<()>,
        entered: Option<mpsc::Sender<()>>,
    ) -> Box<dyn FnOnce() + Send> {
        Box::new(move || {
            log(&l, &format!("run {name}-enter"));
            if let Some(t) = entered {
                let _ = t.send(());
            }
            // the gate is released (or the sender dropped) outside
            let _ = rx.recv();
            log(&l, &format!("run {name}-exit"));
        })
    }

    // `task_wait_release` in the vectors: the wait blocks and the gate
    // is released from another thread while it is still blocking, so
    // the batch is taken off the queue after the wait started
    fn release_later(tx: mpsc::Sender<()>) -> JoinHandle<()> {
        thread::spawn(move || {
            thread::sleep(Duration::from_millis(50));
            drop(tx);
        })
    }

    // vector task-order (R29, R32)
    #[test]
    fn task_order() {
        let l = Log::default();
        let q = Arc::new(task_queue_create());
        let (tx, rx) = mpsc::channel::<()>();
        let (entered_tx, entered_rx) = mpsc::channel::<()>();
        task_queue_queue_task(&q, gate("gate", l.clone(), rx, Some(entered_tx)));
        entered_rx.recv().unwrap();
        for t in ["t1", "t2", "t3", "t4"] {
            let l = l.clone();
            assert!(task_queue_queue_task(&q, move || log(
                &l,
                &format!("run {t}")
            )));
        }
        let r = release_later(tx);
        assert!(task_queue_wait(&q));
        r.join().unwrap();
        task_queue_destroy(&q);
        assert_eq!(
            l.lock().unwrap().as_slice(),
            [
                "run gate-enter",
                "run gate-exit",
                "run t1",
                "run t2",
                "run t3",
                "run t4"
            ]
        );
    }

    // vector task-requeue (R31): the wait marker is deferred behind the
    // task queued from the running task
    #[test]
    fn task_requeue() {
        let l = Log::default();
        let q = Arc::new(task_queue_create());
        let (tx, rx) = mpsc::channel::<()>();
        let (entered_tx, entered_rx) = mpsc::channel::<()>();
        task_queue_queue_task(&q, gate("gate", l.clone(), rx, Some(entered_tx)));
        entered_rx.recv().unwrap();
        {
            let l = l.clone();
            let q2 = q.clone();
            task_queue_queue_task(&q, move || {
                log(&l, "run a queues a-child");
                let l_child = l.clone();
                assert!(task_queue_queue_task(&q2, move || log(
                    &l_child,
                    "run a-child"
                )));
                log(&l, "run a queue_task=1");
            });
        }
        {
            let l = l.clone();
            task_queue_queue_task(&q, move || log(&l, "run b"));
        }
        let r = release_later(tx);
        assert!(task_queue_wait(&q));
        r.join().unwrap();
        task_queue_destroy(&q);
        assert_eq!(
            l.lock().unwrap().as_slice(),
            [
                "run gate-enter",
                "run gate-exit",
                "run a queues a-child",
                "run a queue_task=1",
                "run b",
                "run a-child"
            ]
        );
    }

    // vector task-wait-idle (R32): idle wait -> false
    #[test]
    fn task_wait_idle() {
        let l = Log::default();
        let q = Arc::new(task_queue_create());
        assert!(!task_queue_wait(&q));
        let (tx, rx) = mpsc::channel::<()>();
        let (entered_tx, entered_rx) = mpsc::channel::<()>();
        task_queue_queue_task(&q, gate("gate", l.clone(), rx, Some(entered_tx)));
        entered_rx.recv().unwrap();
        {
            let l = l.clone();
            task_queue_queue_task(&q, move || log(&l, "run a"));
        }
        let r = release_later(tx);
        assert!(task_queue_wait(&q));
        r.join().unwrap();
        assert!(!task_queue_wait(&q));
        // a gate that is already in flight when the wait starts does
        // not count as processed (the worker took it before the
        // marker) — the vector's `task_wait_release` still releases it
        let (tx2, rx2) = mpsc::channel::<()>();
        let (entered_tx2, entered_rx2) = mpsc::channel::<()>();
        task_queue_queue_task(&q, gate("gate", l.clone(), rx2, Some(entered_tx2)));
        // the gate has been taken off the queue and is running: the
        // wait marker behind it is alone when the gate releases
        entered_rx2.recv().unwrap();
        let t2 = release_later(tx2);
        assert!(!task_queue_wait(&q));
        t2.join().unwrap();
        task_queue_destroy(&q);
        assert_eq!(
            l.lock().unwrap().as_slice(),
            [
                "run gate-enter",
                "run gate-exit",
                "run a",
                "run gate-enter",
                "run gate-exit"
            ]
        );
    }

    // vector task-inside (R34): true only on the queue's own worker
    #[test]
    fn task_inside() {
        let l = Log::default();
        let q0 = Arc::new(task_queue_create());
        let q1 = Arc::new(task_queue_create());
        assert!(!task_queue_inside(&q0));
        assert!(!task_queue_inside(&q1));
        for (self_q, other_q, name) in [(&q0, &q1, 0), (&q1, &q0, 1)] {
            let l = l.clone();
            let self_q_c = self_q.clone();
            let other_q_c = other_q.clone();
            task_queue_queue_task(self_q, move || {
                assert!(task_queue_inside(&self_q_c));
                assert!(!task_queue_inside(&other_q_c));
                log(&l, &format!("run q{name} self=1 other=0"));
            });
            task_queue_wait(self_q);
        }
        task_queue_destroy(&q0);
        task_queue_destroy(&q1);
        assert_eq!(
            l.lock().unwrap().as_slice(),
            ["run q0 self=1 other=0", "run q1 self=1 other=0"]
        );
    }

    // vector task-destroy (R33): tasks queued before destroy still run
    #[test]
    fn task_destroy() {
        let l = Log::default();
        let q = Arc::new(task_queue_create());
        let (tx, rx) = mpsc::channel::<()>();
        let (entered_tx, _entered_rx) = mpsc::channel::<()>();
        task_queue_queue_task(&q, gate("gate", l.clone(), rx, Some(entered_tx)));
        for t in ["first", "second", "third"] {
            let l = l.clone();
            task_queue_queue_task(&q, move || log(&l, &format!("run {t}")));
        }
        // destroy while the gate still holds the worker; the stop
        // marker is deferred behind the pending tasks
        let q_d = q.clone();
        let d = thread::spawn(move || task_queue_destroy(&q_d));
        thread::sleep(Duration::from_millis(50));
        drop(tx); // release the gate
        d.join().unwrap();
        assert_eq!(
            l.lock().unwrap().as_slice(),
            [
                "run gate-enter",
                "run gate-exit",
                "run first",
                "run second",
                "run third"
            ]
        );
    }

    // R30 (representable part): wait on a fresh idle queue is false
    // (NULL queue handling belongs to the FFI layer)
    #[test]
    fn task_null() {
        let q = task_queue_create();
        assert!(!task_queue_wait(&q));
        task_queue_destroy(&q);
    }
}
