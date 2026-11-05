# S-004 Signal handlers, global callbacks and procedure handlers

| | |
|---|---|
| Status | Ready for review |
| Reference | libobs 33.0.1 `callback/signal.c`, `signal.h`, `proc.c`, `proc.h` |
| Target crate | `crates/obs-signal` (modules `signal`, `proc`) |
| Depends on | S-001, S-002, S-003 |
| Conformance | `test/conformance/scripts/signal/` (17 scripts) |

## 1. Scope

The event and RPC layer of libobs. Each source, output, encoder, service, scene, the core,
and the frontend owns a **signal handler** (named events with typed parameters that
callbacks subscribe to) and often a **proc handler** (named synchronous procedures).
Plugins and the UI connect to them through the C ABI; obs-websocket and scripts rely on
the exact event semantics.

## 2. Conventions

"Callback identity" = the pair (function pointer, user-data pointer). A *signal name* is
the function name of its declaration (S-003). Names are compared with `strcmp`.

## 3. Interface

```c
typedef struct signal_handler signal_handler_t;          /* opaque */
typedef struct proc_handler   proc_handler_t;            /* opaque */
typedef void (*signal_callback_t)(void *data, calldata_t *params);
typedef void (*global_signal_callback_t)(void *data, const char *signal, calldata_t *params);
typedef void (*proc_handler_proc_t)(void *data, calldata_t *params);

signal_handler_t *signal_handler_create(void);
void  signal_handler_destroy(signal_handler_t *handler);
bool  signal_handler_add(signal_handler_t *handler, const char *signal_decl);
/* inline */ bool signal_handler_add_array(signal_handler_t *h, const char **decls);
void  signal_handler_connect(signal_handler_t *h, const char *signal, signal_callback_t cb, void *data);
void  signal_handler_connect_ref(signal_handler_t *h, const char *signal, signal_callback_t cb, void *data);
void  signal_handler_disconnect(signal_handler_t *h, const char *signal, signal_callback_t cb, void *data);
void  signal_handler_connect_global(signal_handler_t *h, global_signal_callback_t cb, void *data);
void  signal_handler_disconnect_global(signal_handler_t *h, global_signal_callback_t cb, void *data);
void  signal_handler_remove_current(void);
void  signal_handler_signal(signal_handler_t *h, const char *signal, calldata_t *params);

proc_handler_t *proc_handler_create(void);
void  proc_handler_destroy(proc_handler_t *handler);
void  proc_handler_add(proc_handler_t *handler, const char *decl_string, proc_handler_proc_t proc, void *data);
bool  proc_handler_call(proc_handler_t *handler, const char *name, calldata_t *params);
```

`signal_handler_add_array` (header inline): `NULL` array → `false`; otherwise calls
`signal_handler_add` for each string until the NULL terminator and returns `false` if any
call returned `false` (it continues after a failure).

## 4. State model

```
signal_handler:
   signals            ordered list of Signal (insertion order), looked up by name
   refs               integer, starts at 1
   lock               non-recursive mutex (protects `signals`)
   global_callbacks   ordered list of GlobalCb, protected by:
   global_lock        RECURSIVE mutex
Signal:
   decl               parsed declaration (S-003)
   callbacks          ordered list of Cb
   lock               RECURSIVE mutex
   signalling         bool
Cb:       { fn, data, remove: bool, keep_ref: bool }
GlobalCb: { fn, data, signaling: counter, remove: bool }
```

Thread-local state: `current_cb` (pointer to the Cb being run on this thread, or none) and
`current_global_cb`.

## 5. Behaviour — signal handlers

### 5.1 Lifecycle

- **S-004-R01** `create` returns a handler with `refs == 1`, no signals, no globals. (Returns NULL if a mutex cannot be created, logging `LOG_ERROR`.)
- **S-004-R02** `destroy(NULL)` is a no-op. Otherwise `refs` is decremented; **only when it reaches 0** are all signals, callbacks and the handler freed. A handler that still has `connect_ref` connections therefore survives `destroy` and remains fully usable (vector `11-refs`).

### 5.2 Declaring signals

- **S-004-R10** `add(h, decl)`: parse `decl` (S-003). On parse failure log `LOG_ERROR "Signal declaration invalid: <decl>"` and return `false`.
- **S-004-R11** If a signal with the same name exists: log `LOG_WARNING "Signal declaration '<name>' exists"` and return `false` (the existing declaration is unchanged). Otherwise append the signal at the **end** of the signal list and return `true`.
- **S-004-R12** Signal names are case-sensitive and unique per handler. The parameter list of the declaration is stored but never used to validate `emit` parameters.

### 5.3 Connecting

- **S-004-R20** `connect(h, sig, cb, data)` and `connect_ref`: no-op if `h == NULL`. If no signal named `sig` exists: log `LOG_WARNING "signal_handler_connect: signal '<sig>' not found"` and return.
- **S-004-R21** Plain `connect`: if an entry with identity (cb, data) already exists in the signal's list, **nothing is added**; otherwise the entry `{cb, data, remove=false, keep_ref=false}` is appended. Entries marked `remove` still count as existing (see Q-4).
- **S-004-R22** `connect_ref`: increments `h->refs` **every time** and **always appends** an entry with `keep_ref = true`, even if the identity already exists. Two `connect_ref` of the same pair produce two entries (and the callback runs twice per emit).
- **S-004-R23** `connect_global(h, cb, data)`: no-op if `h` or `cb` is NULL; appends `{cb, data, signaling=0, remove=false}` unless an entry with the same identity already exists (dedupe, see Q-6).

### 5.4 Disconnecting

- **S-004-R30** `disconnect(h, sig, cb, data)`: nothing happens if the signal does not exist (no log) or no entry matches. It acts on the **first** entry (in list order) with identity (cb, data), including one already marked `remove`.
  - If the signal is currently being emitted (`signalling == true`): the entry is only marked `remove = true`.
  - Otherwise the entry is erased immediately; if it was a `keep_ref` entry, `refs` is decremented afterwards and, **if it reached 0, the handler is destroyed** (R02).
- **S-004-R31** `disconnect_global(h, cb, data)`: no-op if `h` or `cb` is NULL. Finds the entry with the same identity; if its `signaling` counter is non-zero it is only marked `remove`, otherwise erased immediately. (But see Q-5: during that callback's own execution the entry is not found.)
- **S-004-R32** `signal_handler_remove_current()`: if the calling thread is currently inside a signal callback (see R41), mark that entry `remove = true`; otherwise, if inside a global callback, mark that one; otherwise do nothing.

### 5.5 Emitting

`signal_handler_signal(h, sig, params)`:

- **S-004-R40** If `h` is NULL, or `sig` is not declared: return immediately. **Global callbacks are not called for undeclared signals.**
- **S-004-R41** Otherwise, in this order:
  1. Lock the signal's recursive lock, set `signalling = true`.
  2. For `i = 0; i < callbacks.length; i++` (length re-read every iteration, so entries appended during the emit **are reached by this emit**): if the entry is not marked `remove`, set `current_cb` to it, call `fn(data, params)`, then set `current_cb` to **none**.
  3. Erase every entry marked `remove` (scan from the end to the start); count how many of those had `keep_ref`.
  4. `signalling = false`; unlock.
  5. Lock the global lock; for `i = 0; i < globals.length; i++`: if not marked `remove`: `signaling += 1`, set `current_global_cb`, call `fn(data, sig, params)`, set it to none, `signaling -= 1`. Then erase (scan from the end) every entry that is marked `remove` **and** has `signaling == 0`. Unlock.
  6. If step 3 counted `k > 0` removed `keep_ref` entries: `refs = refs - k` (**no** destruction check here — Q-7).
- **S-004-R42** All callbacks and global callbacks of one emit receive the **same** `params` pointer, in order; modifications made by one are seen by the next (vector `10`).
- **S-004-R43** Order of invocation: the signal's own callbacks in connection order (with entries appended during the emit at the end), then the global callbacks in connection order.
- **S-004-R44** Callbacks may call any function of this API on the same handler: emit (including the same signal, R50), connect, disconnect, remove_current.
- **S-004-R45** A `remove`d entry that has not yet run in the current emit is **skipped**; one that already ran is erased at the end (vector `04`).

### 5.6 Re-entrancy

- **S-004-R50** The signal lock is recursive: a callback may emit the same signal again; the nested emit follows R41 completely, including steps 3–4 (erasing marked entries and clearing `signalling`) **while the outer emit is still iterating** (vectors `07`, `17`). `signalling` is a boolean, **not a counter**: after a nested emit of the same signal returns, `signalling == false` while the outer emit continues (Q-3).

## 6. Behaviour — procedure handlers

- **S-004-R60** `proc_handler_create` returns an empty handler guarded by a **recursive** mutex.
- **S-004-R61** `proc_handler_add(h, decl, proc, data)`: no-op if `h == NULL`. Parse `decl` (S-003); on failure log `LOG_ERROR "Function declaration invalid: <decl>"` and return. If a procedure with the same name exists: log `LOG_WARNING "Procedure '<name>' already exists"`; the **first registration wins**. Otherwise append.
- **S-004-R62** `proc_handler_call(h, name, params)`: `false` if `h == NULL` or no such procedure (no log). Otherwise find the procedure under the lock, **copy its record, release the lock, then call `proc(data, params)` outside the lock**, and return `true`. Parameters are neither validated nor completed from the declaration; the procedure communicates results through `params` (e.g. a `return` value by convention).
- **S-004-R63** `proc_handler_destroy(NULL)` is a no-op; otherwise it frees everything (no reference counting).

## 7. Concurrency

- Emission holds the signal's recursive lock for the whole duration of the callbacks (step 2–4), so connect/disconnect/emit of **that signal** from other threads block until it finishes; other signals are independent.
- Global callbacks are protected by the recursive global lock, held during step 5.
- The handler lock is held only while looking up a signal by name or adding one; it is never held while callbacks run.
- Lock order: handler lock → (released) → signal lock; global lock after the signal lock is released.
- `current_cb`/`current_global_cb` are thread-local.
- Callbacks run on the emitting thread.

## 8. Reference defects and quirks

- **Q-1 (iteration by index).** Entries appended during an emit run in the same emit (R41.2). Required: reproduce (CORE; vector `06`).
- **Q-2 (remove-current is cleared, not restored).** `current_cb` is set to none after *any* callback returns, including nested ones. After a nested emit made from inside a callback has returned, `remove_current()` called later by the outer callback no longer refers to it and does nothing (vector `16`). Required: reproduce (SHOULD).
- **Q-3 (boolean `signalling`).** See R50. Consequence: a `disconnect` issued by an outer callback after a nested emit of the same signal erases immediately, shifting indexes under the running outer loop. Required: implement `signalling` as a boolean exactly as described (SHOULD); observable consequences are rare.
- **Q-4 (marked entries block re-connect).** `disconnect` followed by `connect` of the same pair **within one emit** loses the connection: the marked entry makes `connect` a no-op and is erased at the end (vector `15`). Required: reproduce (SHOULD).
- **Q-5 (global self-disconnect by name).** `disconnect_global` locates entries by comparing the *whole record* (callback, data, `signaling`, `remove`), so a global callback that disconnects itself while it is running does not match and stays connected (vector `14`). `remove_current` works (vector `09`). Disconnecting it from outside any emit works. Required: reproduce (SHOULD).
- **Q-6 (global dedupe compares raw record bytes).** `connect_global` and `disconnect_global` find entries with a byte-wise comparison of the whole record, including the `signaling` counter, the `remove` flag **and compiler padding**. For an idle entry this means "same (callback, data)" and the vectors (`08`) rely only on that. For an entry that is executing at that moment, or when padding bytes differ, the result is not reliable (an experiment with a global callback re-connecting itself produced duplicate entries, which depends on stack contents and is therefore deliberately **not** a vector). Required: dedupe by (callback, data) among entries that are not executing; behaviour for an executing entry is implementation-defined.
- **Q-7 (refs adjusted without destroy).** R41.6 subtracts directly from `refs`; if that makes it 0 the handler is not freed there. Required: reproduce (MAY deviate by freeing at 0; no vector).
- **Q-8 (record padding).** See Q-6. Implementations compare fields, never raw bytes.

## 9. Implementation notes (non-normative)

- Store callbacks in a `Vec<Cb>`; iterate by index with `while i < v.len()` to get Q-1 for free.
- Use `parking_lot::ReentrantMutex<RefCell<…>>` (or equivalent) for the signal and global locks. The data guarded by a recursive lock needs interior mutability (`RefCell`): do **not** hold a `RefCell` borrow across a callback invocation; re-borrow by index after each call.
- `connect_ref` increments an atomic counter; destroy-on-zero must use `fetch_sub` and act when the previous value was 1 (R02, R30).
- C ABI: callbacks are `extern "C" fn`; the three function-pointer types differ in arity.
- Wrap each callback call in the S-001/primer "no unwinding into C" guard.

## 10. Conformance coverage

| Rule | Scripts |
|---|---|
| R01, R02 | `01`, `11-refs` |
| R10–R12 | `01-basic`, `13-declarations` |
| R20–R22 | `01`, `02-connect-dedupe`, `13` |
| R23 | `08-global-callbacks` |
| R30 | `03-disconnect`, `04-disconnect-during-emit`, `11-refs` |
| R31, R32 | `08`, `09-global-remove-current`, `05-remove-current` |
| R40–R45 | `01`, `04`, `05`, `06-connect-during-emit`, `08`, `10-params-shared` |
| R50 | `07-nested-emit`, `17-nested-emit-with-disconnect` |
| R60–R63 | `12-procs` |
| Q-1 | `06` |
| Q-2 | `16-quirk-remove-current-after-nested-emit` |
| Q-4 | `15-quirk-reconnect-during-emit` |
| Q-5 | `14-quirk-global-disconnect-by-name` |
| Q-3, Q-6 (executing entry), Q-7 | not covered by vectors (unit tests in the crate) |

CORE: all scripts except `14`, `15`, `16` (QUIRK).

## 11. Open questions

- Behaviour under concurrent connect/emit from multiple threads is defined by the locks in section 7 but not covered by vectors; the crate needs a `loom`/stress test.
- Memory ordering of `refs`: the reference uses sequentially consistent atomics; implementers SHOULD use `SeqCst`.

## 12. Round 9: `signal.c` uncited functions (added)

### Reference files (all cited)

- `libobs/callback/signal.c`

- **S-004-R64** Signal registry: `signal_info_create`/`signal_info_destroy` own the signal object (name, callback array, global-callback list, its mutex); `signal_get_callback_idx` resolves a callback id to its index; `getsignal`/`getsignal_locked` look up a signal object by name.
- **S-004-R65** Handler lifecycle: `signal_handler_create`/`signal_handler_destroy` (the latter via `signal_handler_actually_destroy`); `signal_handler_add` appends a callback (unique id); `signal_handler_disconnect` removes one — reference-count rules per the earlier sections.
- **S-004-R66** `signal_handler_signal` (emit): iterates the callbacks under the lock in registration order; `signal_handler_remove_current` lets a callback remove itself mid-emit (re-entrancy rules per the earlier sections).
- **S-004-R67** `signal_handler_connect_global`/`signal_handler_connect_internal`: global callbacks (signal-name-independent) live in a separate mutex-guarded list, deduplicated by (callback, data); they are invoked for every signal on the handler.
