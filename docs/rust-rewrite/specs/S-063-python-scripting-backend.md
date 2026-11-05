# S-063 Python scripting backend

| | |
|---|---|
| Status | Draft (round 9 — behaviour catalog written from C reference; no vectors yet; 25 `pyscript` conformance vectors already exist from round 5) |
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

RFC 2119. The backend is only compiled when Python support is enabled. All CPython symbols
are **dynamically imported** (never linked) — the process must work with a Python that is
not present at build time and with a user-chosen install path.

## 3. Interpreter import (`obs-scripting-python-import.c` / `.h`)

- **S-063-R01** `import_python(const char *python_path, python_version_t *python_version)`:
  loads the CPython shared library from the resolved `python_path` (the user's Python home
  directory + the standard library layout for the requested version), then imports the API
  entry points with the `IMPORT_FUNC` macro. Imported symbols include the GIL set
  (`PyGILState_Ensure`, `PyGILState_GetThisThreadState`, `PyGILState_Release`),
  `Py_Initialize`, and the core object API. A missing library or a missing symbol fails the
  import (scripting for Python is unavailable, logged).
- **S-063-R02** `obs-scripting-python-import.h`: `Import__Py_DECREF` / `Import__Py_XDECREF`
  — the *private* CPython deallocator symbols, imported (not linked) because they are not
  part of the stable ABI; the reference imports them so it can drop references without the
  public-API indirection. The conforming backend MUST behave the same (reference counting
  is CPython-internal; the spec pins the *import mechanism*, not the refcount details).

## 4. Callback bridging (`obs-scripting-python.h`)

- **S-063-R03** obs-signal → Python: `add_python_obs_callback_extra` registers a Python
  callable as the handler for an obs signal/proc (the extra data carries the Python
  callback + user data); `find_next_python_obs_callback` iterates the registered handlers
  in the dispatch loop; `remove_python_obs_callback` unregisters on script unload. The
  dispatch holds the GIL (`PyGILState_Ensure`/`Release` around the call, R01's imports).
- **S-063-R04** Argument plumbing: `parse_args_` converts the obs calldata (S-002) into
  Python arguments for the callback; `py_error_` reports a Python exception back into the
  scripting error channel (the script sees an error line, the process does not crash);
  `python_none` is the NULL → `None` bridge for optional arguments.

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
  `frontend_save_callback` trampoline is invoked by the frontend with the profile name and
  a save flag), and `add_event_callback` / `remove_event_callback` (the
  `frontend_event_callback` trampoline is invoked with the event name). The trampolines
  hold the GIL and forward to the script's registered Python callables; removal must be
  safe from within the callback itself (the script may remove its own callback while it is
  running — the dispatch iterates a snapshot).

## 6. Concurrency

Script callbacks run on whatever thread the obs/frontend callback fires from, under the
GIL (R03/R07). The scripting core's own threading rules (S-039) apply; the Python backend
adds the GIL as the only extra lock.

## 7. Open questions

1. The exact `python_version_t` → library-name mapping per platform (the resolution rules
   in `import_python`'s path construction are platform data, not re-derived here).
2. Which of the 25 existing `pyscript` vectors already exercise R05–R07 vs. which need new
   vectors (the vector set predates this spec).
