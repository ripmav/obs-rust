# S-039 Scripting core (language-independent part)

| | |
|---|---|
| Status | Draft (no Rust sufficiency check performed, by instruction) |
| Reference | libobs 33.0.1 `shared/obs-scripting/` : `obs-scripting.c`, `obs-scripting-logging.c`, `obs-scripting-callback.h` (header-inline), `cstrcache.cpp`, `obs-scripting-internal.h`; descriptive only: `obs-scripting-lua.c/.h`, `obs-scripting-python.c`, `obslua.i`, `obspython.i` |
| Target crate | `crates/obs-scripting-core` |
| Depends on | S-002 (calldata), S-004 (signals), S-014 (obs_data), S-015 (obs_properties), S-034 (hotkeys) |
| Conformance | `test/conformance/scripts/scripting/` (14 scripts) |

## 1. Scope

Scripts (Lua, Python) extend OBS. The *core* decides which language a file belongs to, owns the
public `obs_script_*` API the frontend calls, a worker thread that serialises "deferred" calls,
the record type that tracks every callback a script registers, the script log channel and a
string interning helper. Everything language specific (interpreter, bindings, entry-point
calling) lives in a *backend* that the core calls through a fixed set of functions.

**Covered by executable conformance:** all of the above, tested against a *fake backend* (`gen/scripting_stubs.c`) that
stands in for Lua. No Lua or Python runtime is needed.

**Not covered by conformance (described in section 7 from reading the code, normative only where marked):** the Lua and
Python backends themselves: interpreter start-up, `script_*` entry-point discovery, the generated bindings
(`obslua`/`obspython`, SWIG), marshalling of libobs objects, and the per-language timers, tick list, signal, hotkey,
button and modified callbacks. The Python-enabled build paths of the core (`Python_FOUND`) were not compiled; they
are copies of the Lua dispatch (see R15) and the test build defines only `LUAJIT_FOUND`. A build with no backend
at all (empty format list) was not compiled either; its behaviour follows from the guards in R10..R16.

## 2. Conventions

Rule IDs `S-039-Rnn`. Log lines are `blog` lines; level and text are given where text is MUST.
"Script" = `obs_script_t`. `W` = 8. Types: `enum obs_script_lang { UNKNOWN = 0, LUA = 1, PYTHON = 2 }`.

## 3. Interface

```c
struct obs_script {                 /* common header; backends embed it as the FIRST member */
    enum obs_script_lang type;      /* offset 0 */
    bool loaded;                    /* offset 4 */
    obs_data_t *settings;           /* 8  : owned (one reference) */
    struct dstr path, file, desc;   /* each {char *array; size_t len, capacity}; array NULL while never set */
};
```

Public API (all `EXPORT`): `obs_scripting_set_module(m)`, `obs_scripting_load() -> bool`, `obs_scripting_unload()`,
`obs_scripting_supported_formats() -> const char **` (NULL terminated), `obs_scripting_set_log_callback(handler, param)`,
`obs_scripting_python_runtime_linked/python_version/python_loaded/load_python`, `obs_script_create(path, settings)`,
`obs_script_destroy`, `obs_script_get_description/get_path/get_file/get_lang`, `obs_script_get_properties`,
`obs_script_save`, `obs_script_get_settings`, `obs_script_update(script, settings)`, `obs_script_loaded`,
`obs_script_reload`. Also exported to backends/plugins: `obs_current_module()` (defined here, not in libobs core).
`typedef void (*scripting_log_handler_t)(void *param, obs_script_t *script, int level, const char *msg)`.

Internal (shared with backends): `defer_call_post(void (*call)(void*), void *arg)`, `script_log/script_log_va`
(+ `script_error/warn/info/debug`), the callback record (section 6).

Backend contract (functions the core calls, per language `L` in {lua, python}):
`L_script_create(path, settings) -> script*`, `L_script_load(s) -> bool`, `L_script_unload(s)`, `L_script_destroy(s)`,
`L_load()`, `L_unload()`, `L_script_get_properties(s) -> props*|NULL`, `L_script_update(s, settings|NULL)`, `L_script_save(s)`.

## 4. Data formats

None on disk. The only textual contract is the log prefix (R41). Settings are `obs_data_t` (S-014); the script
settings object is private to the script and handed out with an extra reference.

## 5. Behaviour

### Subsystem lifecycle
- **S-039-R01** `set_module(m)` stores one process-global pointer (NULL allowed); `obs_current_module()` returns the last stored value, NULL initially.
- **S-039-R02** `supported_formats()` returns a static NULL-terminated array, independent of load state: `"lua"` if the Lua backend is built in, then `"py"` if Python is. Extensions are given **without** the dot.
- **S-039-R03** `obs_scripting_load()` initialises the detach mutex, the defer machinery (R30) and starts the defer worker, then calls each built-in backend's `L_load()` (Lua first, then Python); with Python it additionally attempts `load_python(NULL)` on platforms other than Windows/macOS. It returns true on success; if any primitive cannot be created it undoes the earlier ones and returns false without calling a backend. Afterwards the subsystem counts as *loaded*.
- **S-039-R04** `obs_scripting_unload()` does nothing when the subsystem is not loaded. Otherwise, in this order: backends' `L_unload()` (Lua, then Python); release the cached file-filter string; free every record on the detached list (R41) counting them; log `INFO "[Scripting] Total detached callbacks: %d"`; shut the defer worker down (R34); mark the subsystem not loaded.
- **S-039-R05** Scripts that still exist when unload runs are not touched by it (the frontend destroys them first).

### Script creation and dispatch
- **S-039-R10** `obs_script_create(path, settings)` returns NULL, **silently**, when the subsystem is not loaded.
- **S-039-R11** `path == NULL`: WARNING `obs-scripting: [obs_script_create] path is null`, return NULL.
- **S-039-R12** The extension is everything from the **last** `'.'` in the whole path string (a dot inside a directory name counts). No `'.'` -> return NULL silently.
- **S-039-R13** Extension `".lua"` -> Lua `create`, `".py"` -> Python `create` (only when that backend is built in). Comparison is exact and case-sensitive. Anything else (including `".LUA"`, `"."`, `".d/file"`, and `.py` in a build without Python) -> WARNING `Unsupported/unknown script type: <path>`, return NULL.
- **S-039-R14** The core returns exactly what the backend's `create` returns; it does no other validation. In particular a script whose first load failed is still returned (loaded == false).
- **S-039-R15** Every API function taking a script dispatches on `script->type`, in this order Lua then Python; a type with no matching built-in backend (UNKNOWN, or PYTHON in a build without Python) falls through to the default shown per function.
- **S-039-R16** Backend contract assumed by the core (observed from the Lua backend and reproduced by the fake): `create` sets `type`, copies `path`, sets `file` to the text after the last `'/'` (or the whole path when none), creates `settings` as a fresh object into which the supplied settings (if any) are applied, then loads the script (so `loaded` and `desc` are meaningful on return). A successful load calls `update(script, NULL)` once. `desc.array` stays NULL until the script supplies a description.

### Accessors
All functions below log WARNING `obs-scripting: [<function>] <param> is null` for a NULL script argument; `<function>` is the C function name and `<param>` the C parameter name (`script`).
- **S-039-R20** `get_description` -> `desc.array` (possibly NULL even for a valid script); NULL script -> NULL.
- **S-039-R21** `get_path`/`get_file` -> the string, or `""` when the stored array is NULL **or** the script is NULL (never NULL).
- **S-039-R22** `get_lang` -> `type`; NULL script -> UNKNOWN. `loaded(script)` -> flag; NULL -> false.
- **S-039-R23** `get_settings` returns the script's own settings object with one extra reference (the caller releases). NULL script -> NULL.
- **S-039-R24** `get_properties`: NULL script -> NULL. Otherwise dispatch to the backend (R15); if the result is NULL (not loaded, no `script_properties`, UNKNOWN type) return a new **empty** property set. The caller owns the result.
- **S-039-R25** `save(script)`: dispatch to the backend's `save` (which, if loaded and a save hook exists, lets the script modify settings); then ALWAYS return the settings object with one extra reference (also for unknown types and unloaded scripts). NULL script -> NULL.
- **S-039-R26** `update(script, settings)`: NULL script warns and returns; otherwise dispatches to the backend (settings may be NULL, no merge then). For UNKNOWN type: nothing happens.
- **S-039-R27** `reload(script)`: false silently when the subsystem is not loaded; false with a warning for a NULL script; otherwise for a Lua/Python script performs, in order, backend `unload`, *drain the deferred queue* (post a signalling job and wait for it; all earlier posted jobs have completed when this returns), backend `load`; returns the script's `loaded` flag afterwards (the backend's load result is not consulted otherwise). UNKNOWN type: no calls, returns the current flag.
- **S-039-R28** `destroy(script)`: NULL is silently ignored. Lua/Python: backend `unload` (no-op when not loaded) then backend `destroy`, which frees everything. UNKNOWN type: does nothing (the object is leaked, Q-3). No subsystem-loaded check.

### Python fallbacks (build without Python)
- **S-039-R29** Without a Python backend: `load_python(path)` -> false, `python_loaded()` -> false, `python_runtime_linked()` -> **true** (Q-1), `python_version(buf, len)` writes the empty string (`buf[0] = 0`, `len` ignored).

### Deferred-call queue
- **S-039-R30** `defer_call_post(fn, arg)` appends `(fn, arg)` to a FIFO and wakes the worker; it never blocks on the call itself. Posting while the queue is shut down (R34) silently drops the item.
- **S-039-R31** One worker thread (named `scripting: defer`) pops entries in FIFO order and runs each call to completion before the next. A call runs on that thread, never on the poster's thread.
- **S-039-R32** Therefore a job posted after a signalling job runs after it, and when a signalling job has fired every earlier job has finished (the barrier used by R27 and by the tests: `deferred (N)` lists).
- **S-039-R33** Entries are removed under the queue mutex but executed outside it, so a call may itself post.
- **S-039-R34** Shutdown (inside R04): under the mutex set the exit flag and free the queue, then wake the worker and join it. Entries not yet executed are discarded (unspecified which); the worker exits without running anything once it sees the flag.

### Script callback records
Header-inline in the reference, so plugins/backends compile it: see section 6.
- **S-039-R40** `add(first_ptr, script, extra_size)` allocates a zero-filled record of `sizeof(record) + extra_size` bytes, sets `script`, inserts it at the **head** of the list headed by `*first_ptr` and returns it; the `extra_size` tail bytes (directly after the record) are zero.
- **S-039-R41** `remove(cb)`: set `removed` (atomic) first; unlink from its owner list; insert at the head of the global *detached* list (under the detach mutex); then, if set, call `on_remove(cb)` -- i.e. the hook sees `removed == true` and the record already at the head of the detached list. Memory, tail bytes and `extra` calldata stay intact.
- **S-039-R42** `free(cb)`: under the detach mutex unlink from whatever list it is on (detached or still owner-attached, the same fix-up applies), then release `extra` and the record. The `on_remove` hook is not called.
- **S-039-R43** `just_free(cb)`: release `extra` and the record without unlinking (used by unload, R04).
- **S-039-R44** Invariants: for a listed record, `*p_prev_next == record` where `p_prev_next` points to the head variable or the previous record's `next`; removal/insertion keep them consistent (verified by `cb_links`). `script_callback_removed(cb)` is an atomic load.
- **S-039-R45** Unload (R04) counts and frees only the records on the detached list; records still on an owner list are not counted.

### Logging
- **S-039-R50** `script_log(script, level, fmt, ...)` formats into a 2048-byte buffer that starts with a prefix, then the message (truncated so the whole buffer is at most 2047 characters + NUL).
- **S-039-R51** Prefix: `"[<Lang>: <file>] "` with `<Lang>` = `Lua`, `Python` or `(Unknown language)` by `script->type`, `<file>` = `script->file.array`; for a NULL script the prefix is `"[Unknown Script] "`.
- **S-039-R52** If a log handler is installed it is called FIRST with `(param, script, level, message-without-prefix)` (script may be NULL); then the prefixed text is logged with `blog(level, "%s", ...)`. `set_log_callback(NULL, ...)` removes the handler. Level numbers are libobs's (ERROR 100, WARNING 200, INFO 300, DEBUG 400).

### String cache
- **S-039-R60** `cstrcache_get(s)`: NULL or `""` -> `""` (a shared constant, not a cache entry). Otherwise return a pointer to an interned copy: equal content (byte-wise up to NUL) gives the **same pointer** every time; the pointer is never the argument; it stays valid and its content unchanged for the life of the process, however many other strings are interned later. The table is never freed; no locking (not thread-safe, Q-6).

## 6. Algorithms (own pseudocode)

```
add(head_ptr, script, extra):  cb = zeroed(R + extra); cb.script = script
    cb.next = *head_ptr; cb.prev_next = head_ptr
    if cb.next: cb.next.prev_next = &cb.next
    *head_ptr = cb
unlink(cb):  if cb.next: cb.next.prev_next = cb.prev_next ; *cb.prev_next = cb.next
remove(cb):  atomic cb.removed = true ; unlink(cb)
    lock(detach): push_front(detached, cb) ; unlock ; if cb.on_remove: cb.on_remove(cb)
free(cb):    lock(detach) ; unlink(cb) ; unlock ; release(cb.extra) ; release(cb)
record layout (W=8): next@0 prev_next@8 on_remove@16 script@24 extra(calldata: stack@32 size@40 capacity@48 fixed@56) removed@57 ; sizeof = 64
```
The record layout matters only when a Rust implementation shares records with C backends.

## 7. Backend-visible semantics (descriptive, NOT conformance-tested)

Read from the Lua backend; the Python backend mirrors it except where noted. A Rust re-implementation must reproduce these for scripts to behave the same.

1. **Script entry points** (looked up as globals after the file ran): `script_properties`, `script_update`, `script_save`, `script_defaults(settings)`, `script_description()`, `script_load(settings)`, `script_tick(seconds)`, `script_unload()`. A file whose chunk returns exactly one boolean `false` fails to load. Load order after running the chunk: collect properties/update/save hooks, call `script_defaults(settings)`, `script_description()` (string stored in `desc`), `script_load(settings)`, register `script_tick`. Errors in these calls are logged (WARNING via `script_log`) and do not fail the load (the description error text wrongly says "script_defaults", Q-5). Unload: mark all of the script's callbacks removed, undefine script-defined sources, drop tick, call `script_unload()`, `remove` every callback record, close the interpreter, set loaded false.
2. **Deferred registration.** API calls made by a script that connect something to libobs (`timer_add`, `obs_add_tick_callback`, `obs_add_main_render_callback`, `signal_handler_connect[_global]`) allocate a callback record immediately but perform the actual libobs connection through `defer_call_post` (so it happens on the worker thread, never inside a libobs lock the script call may be under). Removal flags the record (`removed`) at once; the dispatch functions check the flag and return (signal callbacks additionally call `signal_handler_remove_current()`), so a removed callback never fires even if the disconnect is still pending. Hotkey callbacks are re-posted to the worker (`pressed`/`unpressed` jobs); `on_remove` of a hotkey record posts an unregister job.
3. **Calling convention.** Before invoking a script function the dispatcher takes the script's recursive mutex, sets thread-local `current script` and `current callback`, calls with protected-call semantics, logs failures as WARNING `Failed to call <fn> for <what>: <error>`, restores the thread-locals and unlocks. `remove_current_callback()` removes the thread-local current callback. Button callbacks and property-modified callbacks return a boolean (non-boolean result = false); signal callbacks receive the calldata, global ones receive `(signal_name, calldata)`.
4. **Timers.** `timer_add(fn, ms)`: Lua ignores the call if `ms == 0` (Python does not check; Q-4). Interval = `ms * 1_000_000` ns; `last_ts` = current video frame time at the call. The worker later links the timer into the global timer list. Each libobs tick (frame time `ts`): for every timer, removed -> unlink; else if `ts - last_ts >= interval`: call once and `last_ts += interval` (at most one call per tick; a late timer catches up one interval per tick). `timer_remove(fn)` removes the **first** matching record in the script's list (= most recently added). Signal disconnect matches function identity + handler + signal name.
5. **Exposed API.** The bindings expose the public libobs headers (graphics math, `obs.h`, `obs-source.h`, `obs-data.h`, `obs-properties.h`, `obs-interaction.h`, `obs-hotkey.h`, calldata/proc/signal, `bmem/base/platform/config-file` helpers, frontend API when built with the frontend), with `blog` replaced by a `(level, message)` wrapper and these replaced by script-aware versions: tick/render callbacks, `obs_enum_sources`, `obs_source_enum_filters`, scene item enumeration (return lists; must be released with `source_list_release`/`sceneitem_list_release`), `obs_properties_add_button`, `obs_property_set_modified_callback`, signal connect/disconnect (+global), `obs_hotkey_register_frontend`/`unregister`, `timer_add/remove`, `remove_current_callback`, `calldata_source/sceneitem`, `script_log`-based `print`/error hooks. Plain pointer-taking libobs functions are exposed mechanically.

## 8. Reference defects and quirks

- **Q-1** `obs_scripting_python_runtime_linked()` returns true in a build without Python. SHOULD match. (`13-python-fallbacks`)
- **Q-2** `obs_script_get_description` returns NULL (not `""`) for scripts without description, while path/file never return NULL. MUST match. (`03-getters-values`)
- **Q-3** `obs_script_destroy` of a script with no matching backend does nothing: leak. A Rust version MAY free it; unobservable. (`06-unknown-type-dispatch`)
- **Q-4** Lua `timer_add` ignores `ms == 0`; Python accepts it (interval 0: fires every tick). Negative `ms` wraps to a huge unsigned interval (never fires). Not tested.
- **Q-5** Error text for a failing `script_description` says `script_defaults`. MAY fix.
- **Q-6** `cstrcache_get` is unsynchronised. Required: safe under concurrent use is NOT required, but a Rust version SHOULD simply lock (unobservable).
- **Q-7** `reload` ignores whether the backend load worked other than via the flag; `create` returns a non-NULL script for a failed load. MUST match. (`03`, `05`)
- **D-1** `obs_scripting_load` after `obs_scripting_unload` does not reset the defer exit flag: the new worker exits immediately and every later `defer_call_post` is silently dropped (and `reload` would then wait forever). **Required:** a load after an unload must work like the first load (R30-R32 hold). Recorded by `90-defects` as `info_defer_try_wait timeout`; a conforming runner prints `completed` there (info lines are not compared).
- **D-2** After `obs_scripting_unload` the `detached_callbacks` head still points at freed records, so a second unload (or any `remove` after reload) walks freed memory. **Required:** the list is empty after unload. Runner prints `info_detached_head_after_unload dangling` for the reference, `null` is required.
- **D-3** Lua `signal_handler_disconnect_global` looks the function up at stack index 3 although the function is argument 2, so it never finds a callback and never disconnects. **Required:** disconnect the global callback matching (handler, function) as the Python version does. Not tested (needs Lua).
- **D-4** `script_log_va` computes the message position from `snprintf`'s would-be length of the prefix; a file name of >= 2048 bytes makes the write position lie outside the buffer. **Required:** truncate. Not tested.

## 9. Implementation notes (non-normative)

Rust: `Script` trait object per backend (`create/load/unload/destroy/update/save/properties`) behind an enum of `{Lua, Python}`; keep `type` as an explicit field so unknown types are representable. Defer queue: `std::sync::mpsc` or a mutex+condvar with an explicit generation so reload-after-unload works (fixes D-1). Callback records: `Arc<CallbackRecord{ removed: AtomicBool, script: Weak<Script> }>` with the scripts' list holding `Arc`s; the detached list is only needed to free everything at shutdown and for the count log (R04); keep "head of detached list" ordering only if the count/free order is observable (it is not, apart from `on_remove` seeing `removed == true`). If C backends are kept behind FFI, the exact record layout in section 6 must be preserved. `cstrcache`: `Mutex<HashMap<Box<str>>>` leaking the boxes.

## 10. Conformance coverage

Script language: `test/conformance/README.md`, section "scripting runner".

| Rule | Scripts |
|---|---|
| R01 | 13 |
| R02 | 01, 13 |
| R03, R04, R05 | 01, 02, 11, 13 |
| R10-R14 | 01, 03 |
| R15 | 04, 06 |
| R16 | 03, 04 (fake backend implements the contract; the Lua backend itself is not covered) |
| R20-R22 | 01, 02, 03 |
| R23-R26 | 02, 04, 06 |
| R27 | 05, 06 |
| R28 | 01, 03, 05, 06 |
| R29 | 13 |
| R30-R34 | 05, 08, 90 |
| R40-R45 | 09, 10, 11 |
| R50-R52 | 07 |
| R60 | 12 |
| D-1, D-2 | 90, 11 (`info_` lines) |

Section 7 (and D-3, D-4, Q-4, Q-5) have no conformance scripts.

## 11. Open questions

- Python backend differences beyond the ones listed in Q-4 were not diffed line by line.
- Thread-safety of `reload` versus concurrent worker jobs targeting the reloaded script (a documented `XXX` in the reference) is unspecified.
- Which pending deferred jobs run during unload is unspecified (R34).
- Windows/macOS: Python loading needs a user-supplied path (`load_python(path)`); not specified here.
