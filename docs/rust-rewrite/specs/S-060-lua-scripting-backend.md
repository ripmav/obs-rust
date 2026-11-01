# S-060 Lua scripting backend

| | |
|---|---|
| Status | Draft (Rust sufficiency check skipped by instruction) |
| Reference | libobs 33.0.1 `shared/obs-scripting/`: `obs-scripting-lua.c`, `obs-scripting-lua.h` (header-inline callback helpers), `obs-scripting-lua-source.c`, `obs-scripting-lua-frontend.c`; descriptive only: `obslua/obslua.i`, `obs-scripting-lua*.h` |
| Target crate | `crates/obs-scripting-lua` |
| Depends on | S-039 (scripting core: dispatch, deferred queue, callback records, log channel), S-004 (signals), S-014 (obs_data), S-015 (obs_properties), S-034 (hotkeys) |
| Conformance | `test/conformance/scripts/luascript/` (25 scripts), runner `gen/luascript_runner.c` |

## 1. Scope

The Lua backend of the scripting subsystem: it implements the nine backend entry points the core (S-039) calls
(`obs_lua_load/unload`, `obs_lua_script_create/load/unload/destroy/update/get_properties/save`), runs one private
interpreter per script, calls the script entry points, hosts the callbacks a script registers (timers, tick,
render, signals, hotkeys, buttons, modified callbacks, frontend events, sources defined in Lua) and replaces a few
libobs functions of the `obslua` module with script-aware versions. Language independent parts (record type,
deferred queue, logging, public API) are S-039 and not repeated.

**What is executed for real.** The conformance runner links the real `obs-scripting-lua.c`, `-lua-source.c`,
`-lua-frontend.c` together with the real core, `obs-data.c`, `obs-properties.c`, calldata and the signal handler, on
LuaJIT 2.1. Lua source is embedded in the scripts (`lua_begin NAME` ... `lua_end`) and written to a private directory.

**What is NOT available, and what that means.** SWIG is not installed, no generated wrapper is committed in the tree.
The reference needs two generated artefacts: the runtime header `swig/swigluarun.h` (pointer marshalling used by the
hand-written files) and the module `obslua` (all remaining libobs bindings: `obs_data_*`, `obs_properties_*`,
`obs_source_*`, `gs_*`, math, ... as thin pass-through wrappers; constants). The runner replaces them with
a small hand-written stand-in (`gen/luascript_swig/swig/swigluarun.h`, `gen/luascript_stubs.c`): typed full-userdata
pointers, `nil` for NULL, and ~30 `obslua` functions plus a handful of constants and test fixtures (`fake_*`).
Consequences: (a) the **hand-written** backend (all of the rules below) is covered by real C; (b) the **generated
bindings are not covered**: which libobs functions exist in `obslua`, argument/return conversion of every wrapped
function, pointer ownership flags, type checking and type-cast rules of SWIG, the exact text of SWIG errors are NOT
specified here; a Rust implementation must expose the public libobs API to scripts under the same names, and
S-039 section 7 item 5 lists it; (c) behaviour that depends on SWIG's conversion (rules marked *[stand-in]*)
was only observed with the stand-in. Further fakes: libobs itself (tick/render lists, sources, scene, hotkeys,
frontend), `os_quick_read_utf8_file`, `os_get_executable_path_ptr` (returns `/fakeroot/bin/<arg>`), and
`luaL_openlibs` (interposed so that `require "obslua"` finds the stand-in through an extra package loader).

Error message texts produced by the Lua interpreter itself (`attempt to index ...`, syntax errors) are those of
LuaJIT and are NOT normative; chunk names are masked in the vectors (`[string "<chunk>"]`). Only prefix and level are.

## 2. Conventions

Rule IDs `S-060-Rnn`. "Script" = `obs_lua_script` (S-039 `obs_script` header first). "Record" = S-039 callback
record. "Deferred" = posted with `defer_call_post` (needs a barrier before libobs sees the effect). Log lines are
`script_log` lines (`[Lua: <file>] ...`) unless written `[Lua] ...` (= `blog` with prefix `[Lua] `, no file).
`ts` = `obs_get_video_frame_time()` in ns.

## 3. Interface

```c
struct obs_lua_script {          /* first member: struct obs_script base (S-039) */
    struct dstr dir, log_chunk;  /* dir: path up to and INCLUDING the last '/'; "" when no '/' */
    pthread_mutex_t mutex;       /* recursive; guards the Lua state */
    lua_State *script;
    struct script_callback *first_callback;   /* list head of this script's records */
    int update, get_properties, save, tick;   /* registry refs; LUA_REFNIL (-1) when absent */
    struct obs_lua_script *next_tick, **p_prev_next_tick;   /* global tick list */
    bool defined_sources;
};
struct lua_obs_callback { struct script_callback base; lua_State *script; int reg_idx; /* + extra bytes */ };
```
Process globals: `tick_mutex` + head of the tick-script list; `timer_mutex` (recursive) + timer list;
`lua_source_def_mutex` + list of Lua-defined source types; startup script text. Thread-locals `current_lua_script`,
`current_lua_cb`.

Entry points called by the core: see S-039 section 3 (`L_script_create/load/unload/destroy/update/get_properties/save`, `L_load/unload`).

## 4. Data formats

None on disk. Textual contract: the startup script (R40) and the log prefix texts.

## 5. Behaviour

### Backend start-up (`obs_lua_load` / `obs_lua_unload`)
- **S-060-R01** `load` creates the three mutexes, builds the startup script text and registers ONE libobs tick callback (the dispatcher of R50) with parameter NULL. `unload` removes that tick callback, frees the startup text and destroys the mutexes. (`01`, any script: `ticks=1` after load, `0` after unload.)
- **S-060-R02** The startup script (run in every new interpreter before anything else) (a) deletes every entry of `package.preload`, (b) appends to `package.cpath` the entries `"./?.so"`, `<exe dir>/../<SCRIPT_DIR>/?.so` (not on Windows/macOS) and `<SCRIPT_DIR>/?.so`, in this order, (c) executes `require "obslua"`. `SO` is `dll` on Windows. (`70`)

### Script load (`create` / `load`)
- **S-060-R10** `create` allocates the script, sets `type = LUA`, `tick = LUA_REFNIL`, copies the path, splits it: `file` = text after the last `'/'`, `dir` = everything up to and including that `'/'`; a path without `'/'` (or empty) gives `file = path`, `dir = ""`. A fresh `settings` object is created and the supplied settings applied to it. Then `load` runs; the (possibly failed) script is returned. (`01`, `02`, `71`)
- **S-060-R11** `load` does nothing when already loaded. It creates a new interpreter, opens the standard libraries and `ffi`, runs the startup script, then `function script_path() return "<dir>" end` and appends `"<dir>/?.so"` to `package.cpath` and `"<dir>/?.lua"` to `package.path` (note: `<dir>` ends with `/`, so the entries contain `//`). It then installs the script-aware functions (R60..) and runs the file with the path as chunk name. (`70`)
- **S-060-R12** Failures, each logged WARNING on the script channel and each leaving `loaded = false` (state closed): `Error opening file: <text>` (file unreadable; `<text>` is whatever was on the Lua stack: unspecified), `Error loading file: <lua msg>` (syntax), `Error running file: <lua msg>` (runtime). No `Loaded` INFO line. (`02`, `71`)
- **S-060-R13** After the chunk ran the following globals are looked up *as functions* (anything else counts as absent): `script_properties`, `script_update`, `script_save` (their registry refs are stored), then in this order `script_defaults(settings)`, `script_description()`, `script_load(settings)` are called (protected). `settings` is the script's own settings object, passed without ownership. Errors are WARNINGs `Error calling script_defaults: <msg>`, `... script_defaults: <msg>` for a failing description (Q-1), `... script_load: <msg>`; they do not fail the load. (`01`, `03`)
- **S-060-R14** The description is `lua_tostring` of the return value: strings and numbers (number rendered as Lua renders it, `42` -> `"42"`) are stored; anything else (table, nil, boolean, nothing) or `""` clears `desc` to NULL (the previous text is dropped on reload). (`03`)
- **S-060-R15** `script_tick` (if a function) is linked at the HEAD of the global tick list under `tick_mutex` and its registry ref stored. (`12`, `13`, `82`)
- **S-060-R16** On success: state kept, `loaded = true`, INFO `[obs-scripting]: Loaded lua script: <file>` (a `blog`, without `[Lua: file]` prefix), then `update(script, NULL)` runs (R20). Order of observable lines: chunk output, defaults, description, load, `Loaded` line, `script_update`. (`01`)
- **S-060-R17** All Lua stack contents are discarded at the end of loading. A chunk that returns exactly one boolean `false` is meant to refuse loading, but under LuaJIT this never happens (Q-3): a script is never refused by its return value. (`02`)
- **S-060-R18** The load runs under the script's mutex with `current_lua_script` set; both are reset afterwards.

### Update / properties / save
- **S-060-R20** `update(settings)`: nothing when not loaded or when there is no `script_update`. Otherwise `settings` (if non-NULL) is **applied** (merged) into the script's settings first, then `script_update(settings)` is called with the script's settings object. Failure: WARNING `Failed to call script_update for obs_lua_script_update: <msg>`. For an unloaded script the supplied settings are NOT merged. (`01`, `03`, `81`)
- **S-060-R21** `get_properties`: NULL when not loaded or `script_properties` absent. Otherwise it is called with no argument and one result; the result is converted to an `obs_properties_t *` (nil -> NULL; a non-pointer -> NULL plus a conversion warning *[stand-in]*). Failure of the call: WARNING `Failed to call script_properties for obs_lua_script_get_properties: <msg>` and NULL. The core turns NULL into an empty set (S-039 R24). (`03`, `40`, `41`)
- **S-060-R22** `save`: nothing when not loaded / no `script_save`; else `script_save(settings)` is called; failure WARNING `Failed to call script_save for obs_lua_script_save: <msg>`. (`01`, `03`, `81`)
- **S-060-R23** Calling convention for these three: `current_lua_script` set, mutex taken for the call. Functions are called with `lua_pcall`; error objects are converted with `lua_tostring`.

### Unload / destroy
- **S-060-R30** `unload` does nothing for a script that is not loaded. Otherwise in this order: (1) under the mutex set `removed` on every record of the script; (2) undefine the script's source types (R90); (3) unlink from the tick list (if linked); (4) call `script_unload()` if it exists (errors silent; `print` still logs); (5) under the mutex `remove` every record (moved to the detached list, `on_remove` hooks run); (6) close the interpreter, `loaded = false`; (7) INFO `[obs-scripting]: Unloaded lua script: <file>`. (`01`, `80`)
- **S-060-R31** After unload no callback of the script runs: timers, tick, render, signals and hotkeys check the `removed` flag before calling Lua. Timer records are unlinked from the global timer list lazily at the next tick (R51); signal/tick/render records are disconnected from libobs the next time libobs invokes them. (`80`)
- **S-060-R32** `destroy` (core calls `unload` first) frees path, file, desc, settings, log buffer and dir, destroys the mutex. A reload creates a completely fresh interpreter: Lua globals are gone, the script settings object survives. (`80`)

### Dispatch, timers, ticks
- **S-060-R50** One libobs tick callback (R01), per call with `seconds`: (a) takes `tick_mutex`, walks the tick list from the head (most recently loaded first), and for each script: sets `current_lua_script`, takes the script mutex, calls `script_tick(seconds)`; failure WARNING `Failed to call tick for lua_tick: <msg>`; (b) takes `timer_mutex` and processes the timer list (R51). (`10`, `12`, `13`, `82`)
- **S-060-R51** Timer processing, with `ts` read ONCE at the start of the tick callback: for each timer from the list head: if its record is `removed`, unlink it; else if `ts - last_ts >= interval` (unsigned arithmetic, `>=` inclusive) call it once and then `last_ts += interval`. A timer therefore fires at most once per tick and catches up one interval per tick; it never skips. The callback is only called if the record is not removed; failure WARNING `Failed to call timer_cb for timer_call: <msg>`. (`10`, `11`)
- **S-060-R52** `timer_add(fn, ms)`: ignored (no record, no warning) if the first argument is not a function or `ms` (read with `lua_tointeger`, converted to `int`: floats truncate, missing/non-numeric = 0) is 0. Otherwise a record with extra space for a timer is added to the script, `interval = ms * 1_000_000`, `last_ts = obs_get_video_frame_time()` AT THE CALL; the timer is linked at the head of the global list by a deferred job. So of several timers added in one chunk the LAST added is called first within a tick. (`10`, `11`)
- **S-060-R53** `timer_remove(fn)`: ignored if not a function; otherwise removes the first record of the script (most recently added) whose registered function is raw-equal to `fn` (regardless of kind: timer, tick, signal ...). (`11`)
- **S-060-R54** `remove_current_callback()` removes the record whose callback is currently executing (thread-local `current_lua_cb`); no-op outside a callback. (`11`, `21`)
- **S-060-R55** Record matching in R53 and in the `remove_*` functions below walks the script's record list from its head; matching is by the Lua function object (raw equality).

### Tick / render callbacks
- **S-060-R56** `obs_add_tick_callback(fn)` / `obs_add_main_render_callback(fn)`: exactly one argument that is a function, else WARNING `Wrong number of parameters for <c function>` / `Wrong parameter type for parameter 1 of <c function>` (`[Lua] ` prefix; c function = `obs_lua_add_tick_callback`, `obs_lua_remove_tick_callback`, `obs_lua_add_main_render_callback`, `obs_lua_remove_main_render_callback`) and nothing else. Valid: add a record, connect to libobs by deferred job. `fn(seconds)` is called with a number; render `fn(cx, cy)` with integers. Failures: WARNING `Failed to call obs_lua_tick_callback for obs_lua_tick_callback: <msg>` (render: `obs_lua_main_render_callback` twice). (`12`)
- **S-060-R57** `obs_remove_tick_callback(fn)`, `obs_remove_main_render_callback(fn)`: same argument check; remove the first matching record. A removed callback is not called; when libobs next invokes it the dispatcher disconnects it from libobs without calling Lua. (`12`)

### Signals
- **S-060-R60** `signal_handler_connect(handler, signal, fn)`: silently ignored unless `handler` converts to `signal_handler_t *`, `signal` is string-convertible and `fn` a function (a failed pointer conversion also logs a warning *[stand-in]*). Otherwise a record is added that stores handler and signal name in its extra calldata (`handler`, `signal`) and the libobs connect is deferred. The callback is called as `fn(calldata)`; the same function may be connected several times. If the record is `removed` when libobs calls it, the dispatcher calls `signal_handler_remove_current()` and returns. Failure: WARNING `Failed to call calldata_signal_callback for calldata_signal_callback: <msg>`. (`20`, `21`)
- **S-060-R61** `signal_handler_disconnect(handler, signal, fn)`: same validation; removes the first record of the script whose function equals `fn`, whose stored handler pointer equals `handler` and whose stored signal name equals `signal`. Otherwise nothing. (`20`)
- **S-060-R62** `signal_handler_connect_global(handler, fn)`: ignored unless handler converts and `fn` is a function; callback `fn(signal_name, calldata)`; failure text `... calldata_signal_callback_global ...`. (`21`)
- **S-060-R63** `signal_handler_disconnect_global`: see D-2 (does not honour `fn`). (`91`)
- **S-060-R64** `calldata_source(cd, name)` / `calldata_sceneitem(cd, name)`: returns the pointer stored under `name` pushed as `obs_source_t *` / `obs_sceneitem_t *` (nil when NULL); returns nothing when the arguments are invalid. *[stand-in]* (not scripted)

### Hotkeys
- **S-060-R70** `obs_hotkey_register_frontend(name, desc, fn)`: needs string `name`, string `desc`, function `fn`, else returns nothing (zero results). Otherwise adds a record whose `on_remove` posts the libobs unregister, registers with libobs (callback data = the record), stores the id in the record's extra calldata (`id`), and returns the id as an integer. If libobs returns the invalid id the record is removed again and that id (the value of `OBS_INVALID_HOTKEY_ID`, which prints as `-1`) is still returned. (`30`)
- **S-060-R71** Hotkey events from libobs are not run in libobs' thread: the libobs callback checks `removed`, then posts a deferred job that calls `fn(pressed)` (boolean) with the record locked as in R80; a removed record at job time is skipped. (`30`)
- **S-060-R72** `obs_hotkey_unregister(fn)`: exactly one function argument (else the warnings of R56 with c function `hotkey_unregister`); removes the first matching record; its `on_remove` posts the libobs unregister of the stored id unless it is the invalid id. (`30`)

### Callback invocation (shared)
- **S-060-R80** Before calling a record's function the dispatcher saves, then sets, thread-locals `current_lua_cb`/`current_lua_script`, takes the script's recursive mutex, calls with `lua_pcall`, and afterwards unlocks and restores the saved values. Failures: WARNING `Failed to call <fn> for <what>: <msg>` where `<fn>` and `<what>` are the identifiers quoted in the rules (the script channel is the record's script).

### Properties
- **S-060-R85** `obs_properties_add_button(props, name, text, fn)`: needs a converting `props`, string `name` and `text`, function `fn`; else returns nothing. Otherwise adds a record and creates the button (`add_button2`, record as user data); returns the new property (nil if libobs refuses). On click: record removed -> false; else calls `fn(properties, property)`; the result is `true` only if the Lua result is the boolean `true` (anything else, nothing, or an error -> false; error WARNING `Failed to call button_prop_clicked for button_prop_clicked: <msg>`). (`40`)
- **S-060-R86** `obs_property_set_modified_callback(prop, fn)`: needs converting `prop` and function; adds a record and registers it with `set_modified_callback2`. On modification: removed -> false; else `fn(properties, property, settings)`, result true only for boolean `true`; error WARNING `Failed to call modified_callback for modified_callback: <msg>`. (`40`)
- **S-060-R87** Property records live in the script's record list and are flagged/removed at unload like any other.

### Logging hooks
- **S-060-R88** Global `print(x)` is replaced: `lua_tostring(arg1)`; if it is NULL (nil, boolean, table, no argument) nothing is logged; otherwise INFO on the script channel; further arguments are ignored. The line is logged for the script in `current_lua_script` (Q-4). Global `error(x)` is replaced the same way at level ERROR and does NOT raise (Q-2). (`04`)
- **S-060-R89** `obslua.script_log(level, text)`: `level` via `lua_tointeger` (non-numbers = 0, any integer is passed through to `blog`); NULL `text` -> nothing; the text is split at `'\n'` and each piece logged on its own line (a trailing newline yields no extra line, an empty piece in the middle yields an empty line). An empty text crashes the reference (D-1). (`04`, `90`)

### Lua-defined sources
- **S-060-R90** `obslua.obs_register_source(t)`: exactly one table argument (else the R56 style warnings with c function `obs_lua_register_source`). Reads `t.id` (string; absent -> WARNING `obs_lua_register_source: no item 'id' of type string`; empty -> silently rejected), `t.type` (integer, default 0), `t.output_flags` (integer, default 0), calls `t.get_name()` (error or non-string/empty result -> rejected silently). Rejected also: `output_flags == 0`, duplicate of an id whose definition is still owned by a loaded script (silently). Every other field is an optional function; an existing non-nil non-function value logs WARNING `obs_lua_register_source: item '<name>' is not a function` and is treated as absent. Strings (`id`, display name) are interned (S-039 R60). (`50`)
- **S-060-R91** New id: the type is registered with libobs (all fourteen callbacks are always installed: `create destroy get_width get_height get_properties update activate deactivate show hide video_tick video_render save load`, plus `get_name` and `get_defaults2`) with a per-type record as type data; then `obs_module_add_source(current module, id)` is called and the type is put at the head of the global definition list. (`50`, `51`)
- **S-060-R92** Callbacks (names as keys of `t`; `d` = the Lua value returned by `create`, kept in the registry per instance):
  `create(settings, source) -> d` (instance creation fails, i.e. libobs gets NULL, when the type has no live script, no `create`, or `create` returns nil/errors; otherwise the instance is linked into the type's instance list), `destroy(d)`, `get_width(d)`/`get_height(d)` -> integer via `lua_tointeger` (floats truncate; error -> 0; absent -> 0), `get_defaults(settings)` (called on the type, no `d`), `get_properties(d) -> props`, `update(d, settings)`, `activate(d)`, `deactivate(d)`, `show(d)`, `hide(d)`, `video_tick(d, seconds)`, `video_render(d, effect)`, `save(d, settings)`, `load(d, settings)`. All callbacks run under the type's definition mutex and the script mutex, with `current_lua_script` = owner. Errors: WARNING `Failed to call <name> for <display name>: <msg>` (`name` as above, e.g. `hide`). If the type has no live script all callbacks do nothing/return 0. (`51`)
- **S-060-R93** Unload of the owning script (R30 step 2), for each of its types: disable the libobs type (`obs_enable_source_type(id, false)`), call `destroy` on every live instance (the print inside runs with no current script: Q-4), drop all Lua callbacks, mark the type scriptless. Instances remain in libobs and answer 0/nothing. (`52`)
- **S-060-R94** Registering an id whose type exists but has no live script (after unload) re-uses the type: callbacks replaced, owner updated, type re-enabled, and `create(settings, source)` is called again for every surviving instance in list order (newest instance first), with the instance's current settings; the new data replaces the old. This works for a different script too. (`52`)

### Frontend API (built with the frontend)
- **S-060-R95** The following `obslua` functions are added: `obs_frontend_get_scene_names/scenes/current_scene/set_current_scene/get_transitions/current_transition/set_current_transition/transition_duration/set_transition_duration/scene_collections/current_scene_collection/set_current_scene_collection/profiles/current_profile/set_current_profile` and `obs_frontend_add/remove_event_callback`, `add/remove_save_callback`. Name lists become Lua arrays (1-based); source lists arrays of source pointers (NO extra reference taken: the frontend list's references are not released here); current scene/transition may be nil; `set_transition_duration` needs a number, `set_current_scene_collection`/`profile` a string (else ignored); `set_current_scene/transition` call libobs even when the conversion fails (with NULL). (`60`)
- **S-060-R96** Event callback `fn(event_integer)`, save callback `fn(save_data, saving_boolean)`; registration deferred, one function argument required (R56 warnings with c functions `add_event_callback` etc.); removed records are disconnected when libobs next invokes them. Failure text `Failed to call frontend_event_callback for frontend API` / `frontend_save_callback`. (`60`)

### Object lists
- **S-060-R97** `obs_enum_sources()`, `obs_source_enum_filters(source)`, `obs_scene_enum_items(scene)`, `obs_sceneitem_group_enum_items(item)` return a new array; every element has an extra reference taken (`obs_source_get_ref` / `sceneitem_addref`) which `source_list_release(list)` / `sceneitem_list_release(list)` drop (once per element). If the argument does not convert, nothing is returned. If the element type is unknown to the bindings a warning is logged and the array is short *[stand-in]*. (`61`)

## 6. Algorithms (own pseudocode)

```
tick(seconds):
  ts = frame_time()
  lock tick_mutex; for s in tick_list(head first): lock s.mutex; call s.script_tick(seconds); unlock; unlock tick_mutex
  lock timer_mutex
  for t in timer_list (head first, remember next first):
      if t.record.removed: unlink t
      elif ts - t.last_ts >= t.interval:  call(t); t.last_ts += t.interval
  unlock
timer_add(fn, ms):  if not fn or int(ms)==0: return
  r = new_record(script, extra=timer); r.interval = ms*1e6; r.last_ts = frame_time(); defer(link r.timer at list head)
```
Exact interleavings of the deferred link versus ticks are not specified (tests use barriers).

## 7. Concurrency

Every Lua call is made with the owning script's recursive mutex held. Registration of anything with libobs is deferred to the S-039 worker; invocation of script callbacks happens on libobs threads (tick, render, signal emitter) or on the worker (hotkeys). `unload` flags records under the mutex BEFORE closing the state; a callback already past the flag check may still run (reference comment `XXX`; unspecified).

## 8. Reference defects and quirks

- **Q-1** QUIRK: a failing `script_description` is reported as `Error calling script_defaults: ...`. SHOULD match. (`03`)
- **Q-2** QUIRK: `error(msg)` in a script logs at ERROR and returns normally; execution continues. MUST match (scripts rely on neither behaviour; keep the observed one). (`04`)
- **Q-3** QUIRK: the documented "chunk returns `false` => do not load" check is dead under LuaJIT: opening `ffi` leaves values on the stack, so the "exactly one result" test never holds. A script is never refused by its return value. SHOULD match the observed behaviour. (`02`: `refuse.lua` loads)
- **Q-4** QUIRK: log lines produced by `print`/`error` while no script is current (the `destroy` callbacks run during source undefine) are tagged `[Unknown Script]`. SHOULD match. (`52`)
- **Q-5** QUIRK: `timer_add` truncates `ms` to a C `int` (10.9 -> 10); a negative value is accepted and gives an interval of ms*1e6 modulo 2^64 (unspecified, not tested).
- **Q-6** QUIRK: `print` logs only the first argument and only strings/numbers. MUST match. (`04`)
- **Q-7** QUIRK: `timer_remove`, `obs_remove_*_callback`, `obs_hotkey_unregister` remove the first record with an equal function regardless of the kind of record. SHOULD match. (`11`)
- **Q-8** QUIRK: a failed hotkey registration returns the invalid id (`-1`) to Lua instead of nil. MUST match. (`30`)
- **D-1** DEFECT: `obslua.script_log(level, "")` dereferences NULL (the scratch string is freed by the empty copy and then searched) and crashes the host. REQUIRED: log nothing, continue. (`90`: reference crash recorded as `crash signal 11`)
- **D-2** DEFECT: `signal_handler_disconnect_global(handler, fn)` looks `fn` up at stack index 3 (never a user argument), so every record matches: the first record in the script's list (the most recently added) whose stored handler pointer equals `handler` is removed whatever its function is; records without a stored handler never match. REQUIRED (conformance): reproduce that behaviour; the intended behaviour (match `fn`) is not testable against the reference. (`91`)
- **D-3** DEFECT (by reading, not scripted): `free_type_data` of a Lua source type frees the type record but leaves it linked in the global definition list; a later script unload walks freed memory. libobs only frees types at shutdown, after scripts are destroyed, so it is unreachable in normal use. A re-implementation MUST unlink.
- **D-4** DEFECT (by reading, not scripted): `obs_scripting_unload` frees all detached records, but timer records not yet unlinked by a tick stay in the global timer list; a later tick (e.g. after a re-load of the subsystem) would use freed memory. A re-implementation MUST unlink timers when their record is freed.
- **D-5** DEFECT (UB only, benign): `print` in a callback running with no current script forms `&NULL->base`. Not an observable difference.
- **D-6** DEFECT (by reading): `signal_handler_connect_global` callbacks pushed with the calldata of the emitter; if the script keeps the userdata beyond the callback it dangles. Not specified.

## 9. Implementation notes

- A Rust implementation embedding another Lua (e.g. mlua) must keep: one state per script, state opened with all standard libraries + `ffi` if available, the startup order of R02/R11, `script_path()` as a function, `package.path`/`cpath` extended as in R11, an `obslua` module that exists in every state, registry-style references to callback functions (functions are matched by identity), protected calls everywhere, warnings with the texts above.
- The pointer-marshalling layer (SWIG) is the biggest unspecified piece; a typed userdata with a type check against the declared C type, `nil` <-> NULL, is what the stand-in does and what the rules above need.
- Deferral is observable only through ordering; tests always drain the queue with a barrier before libobs-side effects.
- Fixtures in the runner (`fake_handler`, `fake_source`, `fake_scene`, `fake_sceneitem`, `fake_effect`, `fake_is_userdata`) are not OBS API and must not be implemented in the product.

## 10. Coverage table

| Rule | Scripts |
|---|---|
| R01 | all (`ticks=` lines), 12 |
| R02 | 70 |
| R10 | 01, 71 |
| R11 | 01, 70 |
| R12 | 02, 71 |
| R13, R14 | 01, 03 |
| R15 | 12, 13, 82 |
| R16 | 01, 02 |
| R17 | 02 |
| R20 | 01, 03, 81 |
| R21 | 03, 40, 41 |
| R22 | 01, 03, 81 |
| R30-R32 | 01, 80, 52 |
| R50 | 10, 12, 13, 82 |
| R51-R55 | 10, 11 |
| R56, R57 | 12 |
| R60-R62 | 20, 21 |
| R63 | 91 |
| R64 | untested (stand-in conversion) |
| R70-R72 | 30 |
| R80 | 03, 12, 21, 40, 51 |
| R85, R86 | 40 |
| R88, R89 | 04, 90 |
| R90-R92 | 50, 51 |
| R93, R94 | 52 |
| R95, R96 | 60 |
| R97 | 61 |
| Q-1..Q-8, D-1, D-2 | 03, 04, 02, 52, 11, 30, 90, 91 |
| D-3..D-6 | not scripted |

## 11. Open questions

- Behaviour of SWIG-generated conversions (ownership flags, subclass casts, error texts) is unspecified; needs the generated wrapper and a SWIG install to extend this suite.
- The `obslua` function inventory is taken from S-039 section 7, not verified.
- Lua 5.4 vs LuaJIT: the reference only builds with LuaJIT (`luaopen_ffi`); `lua_rawlen` comes from SWIG's runtime on 5.1.
- Behaviour of callbacks racing with `unload` (reference `XXX`) is unspecified.
- Windows/macOS: `SO` extension and bundle-relative import path differ; not exercised.
