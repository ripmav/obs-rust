# S-063 Python scripting backend

| | |
|---|---|
| Status | Draft (round 9 — behaviour catalog written from C reference; conformance coverage in section 8, round 12) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1, `shared/obs-scripting` (Python backend) |
| Target crate | `crates/obs-scripting` (python backend module) |
| Depends on | S-039 (scripting core API — script lifecycle, callbacks, the obs_* bridge), S-048 (the frontend API catalog these functions wrap) |
| Conformance | existing `test/conformance/scripts/pyscript/` (25 vectors) + planned additions |

## 1. Scope

The Python backend of the scripting API: loading CPython at runtime (dynamic, any
installable path + version), the GIL handling around callbacks, and the Python-side
**frontend API** — scenes, transitions, scene collections, profiles, and the save/event
callback registration that the frontend exposes to scripts. S-039 pins the scripting core
(script object, `obs_script_*`, the callback dispatch); this spec pins what the Python
backend adds on top.

### Reference files (all cited)

- `shared/obs-scripting/obs-scripting-python-frontend.c`
- `shared/obs-scripting/obs-scripting-python.h`
- `shared/obs-scripting/obs-scripting-python-import.h`, `shared/obs-scripting/obs-scripting-python-import.c`

## 2. Conventions

RFC 2119. The backend is only compiled when Python support is enabled. On Windows and
macOS all CPython symbols are **dynamically imported** (`RUNTIME_LINK` — never linked; the
process must work with a Python that is not present at build time and with a user-chosen
install path); on Linux Python is linked at build time instead (`RUNTIME_LINK=0`,
`import_python` is never called).

## 3. Interpreter import (`obs-scripting-python-import.c` / `.h`, Windows/macOS only)

- **S-063-R01** `import_python(const char *python_path, python_version_t *python_version)`:
  resolves the CPython shared library under `python_path` (backslashes normalised to `/`,
  `/` appended), probing the platform library-name pattern — Windows `python3NN.dll`,
  macOS `Python.framework/Versions/3.NN/lib/libpython3.NN.dylib` — from minor 3.12 down to
  3.5 (`access` + `os_dlopen`); `python_version` is out-only and receives the resolved 3.x.
  It then imports the API entry points with the `IMPORT_FUNC` macro (a missing symbol is
  logged and fails the import). Imported symbols include the GIL set
  (`PyGILState_Ensure`, `PyGILState_GetThisThreadState`, `PyGILState_Release`),
  `Py_Initialize`, and the core object API. A missing library or a missing symbol fails the
  import (scripting for Python is unavailable, logged).
- **S-063-R02** `obs-scripting-python-import.h`: the *private* CPython deallocator
  `_Py_Dealloc` is imported (not linked, because it is not part of the stable ABI) for
  Python ≥ 3.9 (or `Py_DEBUG`); `Import__Py_DECREF` / `Import__Py_XDECREF` are the header's
  *static-inline* wrappers (Python ≥ 3.8) that re-map `Py_DECREF`/`Py_XDECREF` to a direct
  `ob_refcnt--` + `_Py_Dealloc` so references can be dropped without the public-API
  indirection. The conforming backend MUST behave the same (reference counting is
  CPython-internal; the spec pins the *import mechanism*, not the refcount details).

## 4. Callback bridging (`obs-scripting-python.h`)

- **S-063-R03** obs-signal → Python: `add_python_obs_callback_extra` registers a Python
  callable in the script's callback list (the entry is extended by `extra_size` bytes of
  caller-defined trailing data; the func is XINCREFed into the entry);
  `find_next_python_obs_callback` searches that list for the entry whose `func` matches
  (used to locate a callback for removal); `remove_python_obs_callback` unlinks the entry
  and XDECREFs the func. The dispatch holds the GIL (`PyGILState_Ensure`/`Release` around
  the call, R01's imports).
- **S-063-R04** Argument plumbing: `parse_args_` wraps `PyArg_VaParse` (format prefixed
  `"%s:%s"` format+func) converting the *Python* arguments of the exposed functions into C
  values — it is not a calldata converter; `py_error_` logs a WARNING
  ("Python failure in <func>:<line>:") and prints the pending exception via `PyErr_Print`
  when `PyErr_Occurred()` (the process does not crash); `python_none` returns a fresh
  `Py_None` reference (the wrappers' None return value).

## 5. Python frontend API (`obs-scripting-python-frontend.c`)

The frontend functions exposed to scripts (the S-048 catalog, Python-wrapped). Behaviour
= the underlying frontend call; the wrapper pins only the mapping (return None/bool on
failure, list-of-strings for names, etc.):

- **S-063-R05** Scenes: `get_scene_names` (list of scene names), `get_scenes` (list of scene
  objects), `get_current_scene` / `set_current_scene`, `get_transitions` /
  `get_current_transition` / `set_current_transition`, `get_transition_duration` /
  `set_transition_duration`.
- **S-063-R06** Collections/profiles: `get_scene_collections` /
  `get_current_scene_collection` / `set_current_scene_collection`, `get_profiles` /
  `get_current_profile` / `set_current_profile` — the switch semantics (what is reloaded,
  what stays) are the frontend's (S-048 / S-070), not the wrapper's.
- **S-063-R07** Frontend callbacks: `add_save_callback` / `remove_save_callback` (the
  `frontend_save_callback` trampoline is invoked by the frontend with the profile
  `obs_data_t` + save flag; the Python callable receives `(data, saving)`), and
  `add_event_callback` / `remove_event_callback` (the `frontend_event_callback` trampoline
  is invoked with the event *enum value*; Python receives an int). The trampolines hold the
  GIL and forward to the script's registered Python callables; removal must be safe from
  within the callback itself (the frontend dispatch iterates its callback vector in
  reverse with per-entry copies, and the trampoline checks the removed flag and
  unregisters itself; registration is deferred via `defer_call_post`, so add/remove inside
  a running callback cannot corrupt the in-flight dispatch).

## 6. Concurrency

Script callbacks run on whatever thread the obs/frontend callback fires from, under the
GIL (R03/R07). The scripting core's own threading rules (S-039) apply; the Python backend
adds the GIL as the only extra lock.

## 7. Open questions

1. (resolved) library-name patterns + the minor-version probe 3.12→3.5 are pinned in R01;
   Linux links Python at build time and never calls `import_python`.
2. (resolved round 12) which of the 25 existing `pyscript` vectors exercise R05–R07:
   section 8.
3. (documented round 12) the `Unloaded python script` log and the deferred
   `obs_hotkey_unregister` from the same destroy are unsynchronised in the reference
   (main thread vs. worker thread; either order can occur). The conformance suite
   serialises them at the runner's `barrier` — see `test/conformance/README.md`,
   pyscript runner, "Worker-thread determinism".

Round 10 second-author check (2026-11-05): corrected the Conventions/R01 platform split
(was: all symbols dynamically imported "for the requested version"; source: `RUNTIME_LINK`
only on Windows/macOS — Linux links Python; version is probed 3.12→3.5, out-only);
corrected R02 (was: `Import__Py_DECREF`/`Import__Py_XDECREF` are the imported private
symbols; source: they are static-inline wrappers — the imported private symbol is
`_Py_Dealloc`); corrected R03 (`find_next_python_obs_callback` searches by func, does not
iterate dispatch) and R04 (`parse_args_` is a `PyArg_VaParse` wrapper for Python args, not
a calldata converter; `py_error_` logs + `PyErr_Print`s); corrected R07 trampoline payloads
(was: profile name / event name; source: `obs_data_t` + save flag / event enum value as
int) and the self-removal mechanism (reverse iteration + removed-flag check, not a
snapshot).

## 8. Conformance coverage

`test/conformance/scripts/pyscript/` — area `pyscript`, runner `gen/pyscript_runner.c`
(script language, the fake libobs and the worker-thread determinism described in
`test/conformance/README.md`, pyscript runner). The runner links the real backend on an
embedded CPython 3.14; `obs-scripting-python-import.c` is not linked (Windows/macOS only).
Vectors regenerate bit-identically (checked round 12).

| Rule | Scripts |
|---|---|
| R01 | `70-lifecycle` (Linux links the interpreter at build time, so only the linked path — interpreter present after `scripting load` — is testable on POSIX; the dynamic-import path is Windows/macOS-only and its file is not linked here) |
| R02 | — (Windows/macOS-only: the private-deallocator header is not linked into the POSIX runner) |
| R03 | `20-signals` (connect/disconnect by signal name, the self-removing callback, non-callables silently ignored), `21-signals-global` (global connect, matching by handler only, an exception is logged and delivery continues) |
| R04 | `03-entry-errors` (an exception in an entry point is reported as a warning + traceback and does not fail the load), `13-tick-errors` (tick-callback exceptions), `61-object-lists` (wrong arguments return None with the error left pending) |
| R05 | `60-frontend` (scene names/objects, current scene, transitions, duration), `61-object-lists` (enumeration lists take an extra reference; the `*_release` functions drop it) |
| R06 | `60-frontend` (collection and profile queries + setters) |
| R07 | `60-frontend` (event callbacks receive the enum as an int, save callbacks receive `(data, saving)`, removal from within the callback), `91-defect-no-current-script` (frontend (de)registration with no current script — the reference defect D-2) |

The remaining scripts (`01-entrypoints`, `02-load-failures`, `04-stdout-stderr`,
`05-sys-path`, `06-module-and-reload`, `07-description-edge`, `08-script-log`, `09-settings`,
`10-timers`, `11-timer-edge`, `12-tick`, `30-hotkeys`, `40-properties`, `41-properties-misc`,
`80-threads`, `90-defect-description`, `92-defect-reinit`) pin the scripting core's lifecycle,
GIL rules and defects — S-039 territory, run by the same vectors.
