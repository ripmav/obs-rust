# S-035 Service objects, module loader and plugin registration

| | |
|---|---|
| Status | Ready for review (Rust sufficiency check NOT performed, see section 11) |
| Reference | libobs 33.0.1 `obs-service.c`, `obs-module.c` (+ header-inline code in `obs-module.h`, `obs-service.h`, and the per-platform core-module loader) |
| Target crate | `crates/obs-core` (modules `service`, `module`) |
| Depends on | S-001 (bmem), S-004 (signals/procs), S-010 (text lookup), S-014 (obs_data), S-015 (properties), S-023 (dlopen, glob, file-exists) |
| Conformance | `test/conformance/scripts/modservice/` (23 scripts), runner `gen/modservice_runner.c` |

## 1. Scope

Two coupled subsystems of the libobs core:

1. **Services** (`obs_service_t`): plugin-implemented descriptions of a streaming destination (server URL, key, protocol,
   limits). Registration of a service *type*, creation / update / destruction of service *objects*, reference counting
   (strong + weak), and the wiring between a service and an output (activation, deferred destruction).
2. **Modules** (plugins): discovery of module files and data directories from path templates, `dlopen`, export
   resolution, version gating, the safe-mode / user-disabled / core / legacy policy, `obs_module_load` / `post_load` /
   `unload`, the registration entry points that modules call from `obs_module_load` (`obs_register_{source,output,
   encoder,service}_s`) and the bookkeeping that attributes registered ids to the module that registered them, module
   metadata (`manifest.json`), data-file lookup, config-path building and locale lookup.

Out of scope: the output object itself (which holds the other end of the service/output wiring), the generic
`obs_context_data_*` layer (name de-duplication, UUIDs, hotkeys; treated as a collaborator, see 3.1), source/encoder/output
*object* lifecycles, the frontend's load sequence (it calls the functions below in a fixed order, see 6.8), and platform
details of Windows/macOS (listed in 9).

All C names below are the ABI that plugins link against and MUST be exported with C linkage and the same signatures.

## 2. Conventions

RFC 2119 words. `W = 8`. "Core" is the global `obs` object; most functions are documented to be no-ops or to return an error
when the core does not exist (`obs == NULL`). `LOG(level)` rules say only the level MUST match; texts are informative.
Strings are NUL-terminated UTF-8; paths use `/` internally. "Module template" = a path containing the literal token
`%module%`. All counts in vectors use 64-bit sizes.

## 3. Interface

### 3.1 Collaborators (not specified here)

* **Context layer** (`obs_context_data`): `init(type=SERVICE, settings, name, uuid=NULL, hotkeys, private)` stores
  `settings` by reference-counted share (a NULL settings argument yields a fresh empty `obs_data`), copies the name
  (`NULL`/`""` for a non-private object becomes `__unnamedNNNN`, a 4-digit zero-padded counter shared by all object types,
  starting at 0; a private object with `NULL` name keeps a NULL name; a private object with `""` also gets `__unnamed`),
  creates an *empty* signal handler and proc handler (a service declares no signals/procs), and generates **no** UUID for
  services. `insert` links the object at the **head** of the core's service list under the service mutex; `remove` unlinks it
  (idempotent); `free` releases settings/handlers/name and unlinks. Private services are linked in the same list.
* **Reference counts** (header-inline in libobs): a control block `{ strong: i64, weak: i64, service* }` where the counters hold
  *(owners - 1)*: a fresh object has `strong = 0, weak = 0`; `release` decrements and reports "last owner" when the result is `-1`;
  `get_ref` increments only while `strong > -1` (CAS loop) and fails otherwise. Control block lifetime: freed when `weak` reaches -1.
* `obs_data` (S-014), `obs_properties` (S-015), `text_lookup` (S-010), `os_dlopen/os_dlsym` (S-023: appends `.so` when the path contains
  no `.so` substring, logs `LOG_ERROR "os_dlopen(path->resolved): <dlerror>"` on failure), `os_glob` (S-023: bytewise-sorted,
  returns `(path, is_directory)` pairs, dot-files are not matched by `*`), `os_file_exists`, `strlist_split` (S-006).

### 3.2 Service type descriptor (`struct obs_service_info`, 216 bytes, W = 8)

Field order and offsets (every member is a pointer, 8 bytes): `id` const char* @0, `get_name(type_data)->const char*` @8,
`create(settings, service)->void*` @16, `destroy(data)` @24, `activate(data, settings)` @32, `deactivate(data)` @40,
`update(data, settings)` @48, `get_defaults(settings)` @56, `get_properties(data)->obs_properties_t*` @64,
`initialize(data, output)->bool` @72, `get_url(data)` @80, `get_key` @88, `get_username` @96, `get_password` @104 (the last four
and `deprecated_1` @112 are legacy: stored, never called by the core), `apply_encoder_settings(data, video, audio)` @120,
`type_data` void* @128, `free_type_data(type_data)` @136, `get_output_type(data)->const char*` @144,
`get_supported_resolutions(data, struct obs_service_resolution **res, size_t *count)` @152, `get_max_fps(data, int*)` @160,
`get_max_bitrate(data, int *video, int *audio)` @168, `get_supported_video_codecs(data)->const char**` @176 (NULL-terminated),
`get_protocol(data)->const char*` @184, `get_supported_audio_codecs(data)->const char**` @192,
`get_connect_info(data, uint32_t type)->const char*` @200, `can_try_to_connect(data)->bool` @208.
`struct obs_service_resolution { int cx, cy; }`. Connect-info type constants: SERVER_URL 0, STREAM_ID = STREAM_KEY 2, USERNAME 4,
PASSWORD 6, ENCRYPT_PASSPHRASE 8, BEARER_TOKEN 10.

Plugins register with the macro `obs_register_service(info)` = `obs_register_service_s(info, sizeof(struct obs_service_info))`, where the
`sizeof` is the size **of the plugin's compile-time struct**. Fields at or beyond that size are absent (see R01-R03).

### 3.3 Service object API (all `EXPORT`ed)

```c
obs_service_t *obs_service_create(const char *id, const char *name, obs_data_t *settings, obs_data_t *hotkey_data);
obs_service_t *obs_service_create_private(const char *id, const char *name, obs_data_t *settings);
void  obs_service_destroy(obs_service_t *);              /* internal-ish but exported */
void  obs_service_release(obs_service_t *);
obs_service_t *obs_service_get_ref(obs_service_t *);
obs_weak_service_t *obs_service_get_weak_service(obs_service_t *);
obs_service_t *obs_weak_service_get_service(obs_weak_service_t *);
void  obs_weak_service_addref(obs_weak_service_t *);  void obs_weak_service_release(obs_weak_service_t *);
bool  obs_weak_service_references_service(obs_weak_service_t *, obs_service_t *);
const char *obs_service_get_name / get_type / get_id (const obs_service_t *);   /* get_type == get_id == type id */
void  obs_service_update(obs_service_t *, obs_data_t *settings);
obs_data_t *obs_service_get_settings(const obs_service_t *);         /* adds a reference */
signal_handler_t *obs_service_get_signal_handler(const obs_service_t *);  proc_handler_t *obs_service_get_proc_handler(...);
obs_properties_t *obs_service_properties(const obs_service_t *);  obs_properties_t *obs_get_service_properties(const char *id);
obs_data_t *obs_service_defaults(const char *id);  const char *obs_service_get_display_name(const char *id);
void *obs_service_get_type_data(obs_service_t *);
const char *obs_service_get_protocol / get_preferred_output_type (const obs_service_t *);
const char *obs_service_get_connect_info(const obs_service_t *, uint32_t type);
bool  obs_service_can_try_to_connect(const obs_service_t *);
void  obs_service_get_supported_resolutions(const obs_service_t *, struct obs_service_resolution **, size_t *count);
void  obs_service_get_max_fps(const obs_service_t *, int *);  void obs_service_get_max_bitrate(const obs_service_t *, int *v, int *a);
const char **obs_service_get_supported_video_codecs / _audio_codecs (const obs_service_t *);
void  obs_service_apply_encoder_settings(obs_service_t *, obs_data_t *video, obs_data_t *audio);
obs_module_t *obs_service_get_module(const char *id);  enum obs_module_load_state obs_service_load_state(const char *id);
/* core-internal, used by the output object */
void obs_service_activate(obs_service_t *);  void obs_service_deactivate(obs_service_t *, bool remove);
bool obs_service_initialize(obs_service_t *, obs_output_t *);  const struct obs_service_info *find_service(const char *id);
```

Object layout (core-internal but the output code reads/writes `output` and `active`): `{ context, info (a COPY of the registered
descriptor), owns_info_id (never set for services; dead), active, destroy, output* }`. The output object keeps a back pointer
`output->service`; the service side keeps `service->output`.

### 3.4 Module API

```c
enum obs_module_load_state { INVALID=0, ENABLED=1, MISSING=2, DISABLED=3, DISABLED_SAFE=4, FAILED_TO_OPEN=5, FAILED_TO_INITIALIZE=6 };
enum obs_runtime_module_type { UNDEFINED=0, CORE=1, PLUGIN=2, LEGACY_PLUGIN=3 };
#define MODULE_SUCCESS 0  MODULE_ERROR -1  MODULE_FAILED_TO_OPEN -2  MODULE_MISSING_EXPORTS -3  MODULE_INCOMPATIBLE_VER -4  MODULE_HARDCODED_SKIP -5
struct obs_module_info2 { const char *bin_path, *data_path, *name; enum obs_runtime_module_type type; };   /* offsets 0,8,16,24 */
struct obs_runtime_module_info { struct { const char *binary, *data; } path_info; enum obs_runtime_module_type type; const char *name; };
struct obs_module_failure_info { char **failed_modules; size_t count; };

int  obs_open_module(obs_module_t **module, const char *path, const char *data_path);
bool obs_create_disabled_module(obs_module_t **module, const char *path, const char *data_path, enum obs_module_load_state state);
bool obs_init_module(obs_module_t *);
void obs_post_load_modules(void);   void obs_log_loaded_modules(void);
void obs_add_module_path(const char *bin, const char *data);  void obs_add_safe_module(const char *);  void obs_add_disabled_module(const char *);
void obs_find_modules2(void (*cb)(void *param, const struct obs_module_info2 *), void *param);   /* obs_find_modules: ABI-compatible cast */
void obs_enum_modules(void (*cb)(void *param, obs_module_t *), void *param);
bool obs_load_core_modules(void);   void obs_load_plugins(struct obs_runtime_module_info *, struct obs_module_failure_info *);
void obs_module_failure_info_free(struct obs_module_failure_info *);   /* obs_load_all_modules / obs_load_all_modules2: deprecated NO-OPS */
obs_module_t *obs_get_module(const char *name);  obs_module_t *obs_get_disabled_module(const char *name);   bool obs_get_module_allow_disable(const char *name);
bool obs_is_core_module(obs_module_t *);  bool obs_is_legacy_module(obs_module_t *);
const char *obs_get_module_file_name / _name / _author / _description / _binary_path / _data_path / _id / _version (obs_module_t *);
void *obs_get_module_lib(obs_module_t *);
char *obs_find_module_file(obs_module_t *, const char *file);   char *obs_module_get_config_path(obs_module_t *, const char *file);   /* bfree results */
void obs_module_add_source / _output / _encoder / _service (obs_module_t *, const char *id);
bool obs_module_get_locale_string(const obs_module_t *, const char *lookup, const char **translated);
const char *obs_module_get_locale_text(const obs_module_t *, const char *text);
lookup_t *obs_module_load_locale(obs_module_t *, const char *default_locale, const char *locale);
int  obs_module_load_metadata(obs_module_t *);          /* always returns MODULE_SUCCESS */
void obs_register_source_s / _output_s / _encoder_s / _service_s (const struct <kind>_info *, size_t size);
void free_module(obs_module_t *);  bool find_core_module(struct obs_runtime_module_info *, callback2, void *);   /* internal, exported to the platform loader */
extern obs_module_t *loadingModule;     /* "the module whose obs_module_load is running"; NULL otherwise */
```

Module object (internal): `{ mod_name, file, bin_path, data_path, lib handle, loaded, load_state, function pointers load / unload /
post_load / set_locale / get_string / free_locale / ver / set_pointer / name / description / author, metadata*, next, sources[],
outputs[], encoders[], services[] (id string lists), module_type }`. Enabled and disabled modules live in **two singly linked
lists** `first_module` / `first_disabled_module`; new entries are pushed at the head.

### 3.5 Exports a plugin provides (resolved by name with `dlsym`)

Required: `bool obs_module_load(void)`, `void obs_module_set_pointer(obs_module_t*)`, `uint32_t obs_module_ver(void)`.
Optional: `void obs_module_unload(void)`, `void obs_module_post_load(void)`, `void obs_module_set_locale(const char*)`,
`void obs_module_free_locale(void)`, `const char *obs_module_name(void)`, `obs_module_description(void)`, `obs_module_author(void)`,
`bool obs_module_get_string(const char *key, const char **out)`. `obs_module_ver` returns `(major<<24)|(minor<<16)|patch` of the libobs the
plugin was built against; the host's constant is `LIBOBS_API_VER` (33.0.1 for the committed vectors; vectors only use relative versions).
Header-inline helpers compiled into plugins: `OBS_DECLARE_MODULE()` (defines `set_pointer`, `obs_current_module`, `obs_module_ver`),
`OBS_MODULE_USE_DEFAULT_LOCALE(name, default)` (defines `obs_module_set_locale` = destroy old lookup then `obs_module_load_locale(
obs_current_module(), default, locale)`, `obs_module_free_locale`, `obs_module_get_string` = `text_lookup_getstr`, `obs_module_text`),
`obs_module_file(f)` = `obs_find_module_file(obs_current_module(), f)`, `obs_module_config_path(f)`.

## 4. Data formats

### 4.1 `manifest.json` (module metadata)

Located at `<data_path>[/]manifest.json` (a `/` is inserted unless `data_path` is empty or already ends in `/`). Parsed with the
`obs_data` JSON reader. Recognised keys: strings `display_name`, `id`, `version`, `os_arch`, `name`, `description`, `long_description`;
object `urls` with strings `repository`, `website`, `support`; booleans `has_banner`, `has_icon`. Example (vector
`13-module-metadata`):

```json
{"display_name":"Full Name","id":"com.example.full","version":"1.2.3","os_arch":"linux-x86_64","name":"full",
 "description":"short","long_description":"long text","urls":{"repository":"https://r","website":"https://w","support":"https://s"},
 "has_banner":true,"has_icon":true}
```

A missing, wrongly typed or unparsable value reads as `""` (strings) / `false` (booleans): metadata then exists with empty fields.

### 4.2 Locale files

`<data_path>/locale/<locale>.ini`, parsed by text lookup (S-010): lines `Key="Value"`.

## 5. Behaviour

### 5.1 Service type registration (`obs_register_service_s`)

* **S-035-R01** Let `size` be the passed size and `known` = 216. Order of checks: (1) if a type with the same `id` exists:
  `LOG_WARNING`, reject. (2) Required values `get_name`, `create`, `destroy`, `get_protocol`, each tested in this order: a value is
  present iff `offset+8 <= size` and the pointer is non-NULL; first missing one: `LOG_ERROR`, reject. (3) If `size == 0`: `LOG_ERROR`, return without
  further effect (unreachable after step 2 because `get_name` is absent). (4) If `size > known`: `LOG_ERROR`, reject. (5) Otherwise copy
  `size` bytes into a zeroed descriptor and append it to the type table (registration order is table order); if `loadingModule != NULL`
  append a copy of `id` to its `services` list (only here, i.e. only for accepted types).
* **S-035-R02** *Reject* = call `free_type_data(type_data)` of the **rejected** descriptor iff `size != 0`, the copied part (min(size, known) bytes) contains both
  a non-NULL `type_data` and a non-NULL `free_type_data` (fields at offset >= `size` read as NULL). Accepted registrations never call it.
* **S-035-R03** A descriptor shorter than `known` is valid: absent trailing members behave as NULL (all later optional getters answer their "absent" value).
* **S-035-R04** The registered table stores a *copy*; `id` and `type_data` pointers are kept as given (the plugin keeps them alive).

### 5.2 Creation and destruction

* **S-035-R10** `obs_service_create(id, name, settings, hotkeys)`: unknown `id` -> `LOG_ERROR`, return NULL (nothing allocated). Otherwise: allocate a zeroed object,
  initialise the context (3.1; failure -> free and return NULL), set `info` = copy of the registered descriptor (including its `type_data`),
  call `info.create(context.settings, service)` (the descriptor is already visible through `obs_service_get_type_data(service)` during this call) and store the result as
  `context.data`, create the control block (3.1, `strong=0`), link at the head of the service list, `LOG_DEBUG`, return the object.
  Defaults are **not** applied to `settings` (unlike sources).
* **S-035-R11** If `create` returns NULL: `LOG_ERROR "Failed to create service"`, **but the object is still fully created, linked and returned** (Q-1). Every later callback is then
  invoked with `data == NULL`; `destroy` is skipped for it (R16).
* **S-035-R12** `obs_service_create_private(id, name, settings)` = create with `private = true`, no hotkey data; the object is still linked in the list.
* **S-035-R13** `obs_service_destroy(NULL)` is a no-op. Otherwise unlink from the list (always), set `destroy = true`, and if `active == false` perform the *final destruction* immediately;
  an active service is destroyed by the later `deactivate` (R41). Repeating `destroy` on a pending object does nothing more.
* **S-035-R14** `obs_service_release(NULL)` no-op; else decrement `strong`; if that was the last owner (6.5), call `obs_service_destroy`, **then** release one weak reference (order matters: name lookups rely on live weak refs while the object is listed).
* **S-035-R15** `obs_service_get_ref(NULL)` -> NULL; otherwise = `obs_weak_service_get_service(control)`: increments `strong` and returns the service, or NULL if already dead (cannot happen for a live handle).
  `obs_service_get_weak_service(NULL)` -> NULL; otherwise adds a weak reference and returns the control block. `obs_weak_service_get_service(NULL)` -> NULL; a dead control (`strong == -1`) -> NULL; else
  increments `strong` and returns the service. `obs_weak_service_addref/release(NULL)` no-ops. `obs_weak_service_references_service(w, s)` = `w && s && w->service == s`.
* **S-035-R16** *Final destruction*: if `context.data != NULL` call `info.destroy(data)`; if `service->output != NULL` set `service->output->service = NULL`; `LOG_DEBUG`; free the context (removes from the list if still linked); free the object.
  The `type_data` is never freed by the core (`free_type_data` is called only by R02).

### 5.3 Settings, update, properties, defaults

* **S-035-R20** `obs_service_update(s, settings)`: `s == NULL` -> `LOG_DEBUG`, return. Otherwise merge `settings` into the object's own settings with `obs_data_apply` (a NULL `settings` merges nothing),
  then call `info.update(data, merged_settings)` if present (also when `settings == NULL` and also when `data == NULL`).
* **S-035-R21** `obs_service_get_settings(s)`: NULL for NULL service, else the object's settings with its reference count incremented (caller releases).
* **S-035-R23** `obs_service_defaults(id)`: unknown id -> NULL; else a fresh `obs_data` after `info.get_defaults(settings)` (if present) fills it with *default values* (they are not "set" values).
  `obs_get_service_properties(id)`: unknown id or no `get_properties` -> NULL; else defaults object `d`, `props = get_properties(NULL)` (class-level, `data == NULL`), apply `d` to `props` (`obs_properties_apply_settings`), release `d`, return `props`.
  `obs_service_properties(s)`: NULL service -> NULL; no callback -> NULL; else `props = get_properties(data)`, apply the **object's** settings, return `props`. If the callback returns NULL the result is NULL (no crash).
* **S-035-R24** `obs_service_get_display_name(id)`: unknown -> NULL else `get_name(type_data)`.
* **S-035-R25** `obs_service_get_name/type/id/type_data/get_signal_handler/get_proc_handler`: NULL service -> `LOG_DEBUG` and NULL; otherwise the context name / type id (`info.id`, same string for `get_type` and `get_id`) / `info.type_data` / handlers.

### 5.4 Optional-callback getters

For a non-NULL service (NULL services: see R29):

* **S-035-R26** `get_protocol` -> `info.get_protocol(data)` (required callback). `get_preferred_output_type` -> `get_output_type(data)` or NULL if absent. `get_connect_info(type)` -> callback or NULL. `can_try_to_connect` -> callback or **true** if absent.
* **S-035-R27** `get_supported_resolutions(&res,&n)`: sets `*res = NULL, *n = 0` first, then calls the callback if present. `get_max_fps(&f)`: `*f = 0` first. `get_max_bitrate(&v,&a)`: each non-NULL out-pointer is zeroed first (this happens **even for a NULL service**), then the callback gets the pointers as given (NULL pointers are passed through).
* **S-035-R28** `get_supported_video_codecs` / `_audio_codecs`: callback result or NULL if absent. `apply_encoder_settings(v, a)`: callback is invoked only if at least one of `v`, `a` is non-NULL and the callback exists; arguments are passed unchanged.
* **S-035-R29** NULL-service behaviour: every getter above logs at `LOG_DEBUG` and returns NULL / false / does nothing, **leaving out-parameters untouched** (exception: `get_max_bitrate` zeroing, R27), and `can_try_to_connect(NULL)` is **false**. `get_supported_video_codecs(NULL)` and `_audio_codecs(NULL)` crash in the reference: **D-1**.
* **S-035-R30** `obs_service_initialize(s, output)`: invalid `s` or NULL `output` -> `LOG_DEBUG`, `false` (callback not called). Else `info.initialize(data, output)` or `true` if absent.

### 5.5 Output wiring

The output side (not specified here) sets `service->output = out` and `out->service = service` when a service is assigned.

* **S-035-R40** `obs_service_activate(s)`: invalid -> return. `s->output == NULL` -> `LOG_WARNING` (the text names *deactivate*, Q-3) and return. `active` already -> return. Else call `info.activate(data, settings)` if present and set `active = true`.
* **S-035-R41** `obs_service_deactivate(s, remove)`: invalid -> return; no output -> `LOG_WARNING`, return; not active -> return (state unchanged, `remove` ignored). Else call `info.deactivate(data)` if present, `active = false`; then if `destroy` is pending -> final destruction (R16, `remove` ignored); else if `remove` -> `service->output = NULL`.
* **S-035-R42** While `active`, `obs_service_destroy` / the last `release` only unlink the object and set `destroy`; it stays fully usable (queries, `activate` no-op, `deactivate`) until `deactivate` finishes the destruction. The output's back pointer is cleared only at final destruction (R16).
* **S-035-R43** Final destruction of an *inactive* service that has `output` set clears `output->service`.

### 5.6 Module lookup by service id

* **S-035-R50** `obs_service_get_module(id)`: scan enabled modules (list order), then disabled modules; return the first whose `services` id list contains `id`; else NULL. `obs_service_load_state(id)`: that module's `load_state`, or `MISSING` if none.
  Services registered while `loadingModule == NULL` belong to no module -> `MISSING`.

### 5.7 Module open / create / init / free

* **S-035-R60** `obs_open_module(&m, path, data_path)` returns `MODULE_ERROR` if `m == NULL`, `path == NULL` or the core does not exist. Else in order: `LOG_DEBUG` separator; `os_dlopen(path)` (constructors of the library run); failure -> `LOG_WARNING`, `MODULE_FAILED_TO_OPEN` (-2).
* **S-035-R61** Resolve `obs_module_load`, `obs_module_set_pointer`, `obs_module_ver` in that order; the first missing one -> `LOG_DEBUG`, `MODULE_MISSING_EXPORTS` (-3). Then resolve the optional exports (list in 3.5). No module object exists after a failure; the library stays loaded.
* **S-035-R62** Version gate: `v = module_ver() & 0xFFFF0000`; if `v > LIBOBS_API_VER` (the host constant *including* its patch bits) -> `LOG_WARNING`, `MODULE_INCOMPATIBLE_VER` (-4). Equal/lower major.minor and any patch level load (vectors: minor+1 and major+1 rejected; patch+5, major-1 and 0 accepted).
* **S-035-R63** On success build the module: `bin_path = copy(path)`, `file` = the part after the last `/` (or the whole path), `mod_name = file` with the **last `ext_len` bytes removed** where `ext_len` is the platform module extension length (3 for `.so`) regardless of whether the file really ends in it (Q-5), `data_path = copy(data_path)` (may stay NULL), `load_state = ENABLED`, empty id lists, metadata loaded (R70), module pushed at the **head** of the enabled list. Then `set_pointer(module)`, then `set_locale(core.locale)` if exported (the core locale string may be NULL and is passed as is). Return `MODULE_SUCCESS`. `obs_module_load` is **not** called here.
  Opening the same library twice yields two independent module objects (same library handle, constructors not re-run).
* **S-035-R64** `obs_create_disabled_module(&m, path, data_path, state)`: builds a module object as in R63 with no library handle (`obs_get_module_lib` NULL), given `load_state`, metadata loaded, pushed at the head of the **disabled** list; always returns true.
* **S-035-R65** `obs_init_module(m)`: `m == NULL` or no core -> false; `m->loaded` -> true (no call); else set `loadingModule = m`, `m->loaded = m->load()`, `loadingModule = NULL`; failure -> `LOG_WARNING`; return `m->loaded`. A module whose load failed stays in whichever list it is in.
* **S-035-R66** `obs_post_load_modules()`: for every module in the **enabled** list, head to tail (= reverse of open order), call `post_load` if exported, regardless of `loaded`.
* **S-035-R67** `free_module(m)`: NULL -> no-op. If the library handle exists: call `free_locale` if exported, then `unload` iff `m->loaded && unload exported` (the library is never `dlclose`d). Unlink from the list selected by `load_state == ENABLED` (else the disabled list); free names, paths, id lists and metadata.
* **S-035-R68** `obs_enum_modules(cb, p)`: no core -> nothing; else call `cb(p, m)` for each enabled module, head to tail. `obs_log_loaded_modules()` logs `LOG_INFO` "  Loaded Modules:" then one line per enabled module with its `file`.
* **S-035-R69** `obs_get_module(name)` / `obs_get_disabled_module(name)`: first module in the respective list whose `mod_name` equals `name` (case-sensitive), else NULL. `obs_get_module_allow_disable(name)` = `name` is **not** in the core-module list. `obs_is_core_module(m)` = `m != NULL && m->module_type == CORE`, `obs_is_legacy_module` likewise for LEGACY_PLUGIN (NULL -> false).
* **S-035-R70** Metadata (`obs_module_load_metadata`): path = `data_path` + `/` unless empty/ending in `/` + `manifest.json`. If the file does not exist `m->metadata = NULL`. If it exists metadata is allocated and **every** field is filled from the parsed object (4.1); unparsable JSON logs the `obs_data` error and yields all-empty fields. Returns `MODULE_SUCCESS`.
* **S-035-R71** Accessors, all NULL-safe for a NULL module (-> NULL/false): `file_name` -> `file`; `name` -> `metadata->display_name` if metadata exists (even `""`, Q-4), else `obs_module_name()` if exported, else NULL; `author` / `description` -> the export or NULL (metadata is **not** consulted); `binary_path`, `data_path`; `id` / `version` -> metadata field or NULL when no metadata; `lib` -> library handle or NULL.
* **S-035-R72** `obs_module_add_{source,output,encoder,service}(m, id)`: copy `id` and append to the module's list; `m == NULL` -> ignored (nothing stored). Duplicates are kept.

### 5.8 Registration of other kinds (`obs_register_{source,output,encoder}_s`)

Same shape as R01/R02 (`HANDLE_ERROR` calls `free_type_data` under the same condition). Required checks in order:

* **S-035-R80 output**: duplicate id (`LOG_WARNING`); required `get_name`, `create`, `destroy`, `start`, `stop`; with flag `SERVICE` also `protocols`; with `ENCODED` also `encoded_packet`, else: flag `VIDEO` -> `raw_video`; flag `AUDIO` -> `raw_audio2` if `MULTI_TRACK` else `raw_audio`. A pure `size` check follows (> known: error), then append. On success, for `SERVICE` outputs the `;`-separated `protocols` string is split (empty items dropped) and each protocol not yet in the core's protocol list is appended (first-seen order, exact string compare); then the id is recorded in `loadingModule->outputs` if set.
* **S-035-R81 encoder**: duplicate id; texture+scaling caps together (`PASS_TEXTURE` and `SCALING`) -> `LOG_WARNING`, reject; required `get_name`, `create`, `destroy`; with `PASS_TEXTURE` at least one of `encode_texture` / `encode_texture2`, else `encode`; audio encoders also need `get_frame_size`; then size check, append, record in `loadingModule->encoders`.
* **S-035-R82 source**: (1) `type` must be INPUT / FILTER / TRANSITION / SCENE, else `LOG_WARNING`, reject. (2) duplicate = same *unversioned* id **and** same `version` -> reject. (3) `size > known` -> reject. (4) **record the id in `loadingModule->sources` now** (before the remaining checks, Q-9). (5) copy; filters without `OBS_SOURCE_VIDEO` get `OBS_SOURCE_ASYNC` added; transitions: warn if `get_width`/`get_height` present (they are kept in the copy, but see note) and OR `COMPOSITE|VIDEO|CUSTOM_DRAW` into the flags; (6) composite sources (flag COMPOSITE) must not have `AUDIO` or `ASYNC` (`LOG_WARNING`, reject). (7) required `get_name`; for non-filter non-transition sources with `VIDEO` and not `ASYNC`: `get_width` and `get_height`; for composite: `audio_render`. (8) the stored `id` is `"<id>_v<version>"` if `version != 0`, else a copy of `id`; `unversioned_id` keeps the plugin's pointer. Append to the global table, and to the per-type table (input / filter / transition) except SCENE.
* **S-035-R83** In all three, `size == 0` -> required-value failure (nothing called). Registered outputs/encoders are looked up by exact id.

### 5.9 Path templates and discovery

* **S-035-R90** `obs_add_module_path(bin, data)`: ignored when the core does not exist or either argument is NULL (the vectors print the resulting path count); else append copies in call order. `obs_add_safe_module(name)` and `obs_add_disabled_module(name)`: ignored for NULL; else appended (duplicates kept).
* **S-035-R91** `obs_find_modules2(cb, p)`: no core -> nothing; for each configured path, in order, run *find in path* (6.1).
* **S-035-R92** *Find in path* (6.1) builds the glob pattern from `bin`; entries are visited in `os_glob` order (bytewise ascending by full path).
* **S-035-R93** For each visited entry whose kind matches the layout (files for flat layout, directories for `%module%` layout), compute the module `name` = last path component with everything from its **last `.`** removed (dot-containing names: Q-11), skipping `.` and `..`. Flat layout: `bin_path` = the file; directory layout: 6.2. `data_path` = 6.3. If both exist, call `cb(info{bin_path, data_path, name, type})`; `type` is the configured path's type (D-2).
* **S-035-R94** `find_core_module(info, cb, p)`: `info.name` is set to the part of `info.path_info.binary` after the last `/` (whole string if none) **before** the type check; `type != CORE` -> `LOG_WARNING`, false. `bin_path = binary + extension`; `data_path` = 6.3 applied to (`name`, `path_info.data`); if the file `bin_path` exists call `cb(info{bin_path, data_path, name, CORE})` and return true; else false (the callback is not called).
* **S-035-R95** The platform core loader (`load_core_modules`, Linux algorithm; the list of core module names is generated at build time): for each name in order: substitute `%module%` in the executable-relative templates `<exe_dir>/../<plugin dir>/%module%` and `<exe_dir>/../<data dir>/obs-modules/core/%module%`, call `find_core_module` with type CORE; on `false`: `LOG_ERROR "Failed to load core module <name>"` and **stop** the loop. (The runner substitutes script-defined templates for the executable-relative ones.)

### 5.10 Loading policy

* **S-035-R100** `load_all_callback(info)` (the callback used by both loaders), in order:
  1. platform check "is this file an OBS plugin" (on Linux always true; false -> `LOG_WARNING`, return).
  2. if the safe-module list is **non-empty** and `info.name` is not in it -> create a disabled module (`DISABLED_SAFE`), `LOG_WARNING`, return. (Applies to every type.)
  3. if `type != CORE` and the name is in the user-disabled list -> disabled module (`DISABLED`), `LOG_WARNING`, return.
  4. if `type == LEGACY_PLUGIN` and the name is in the core-module list -> `LOG_WARNING`, return.
  5. if `type != CORE` and an enabled module with that `mod_name` already exists -> `LOG_WARNING`, return (CORE-type modules may duplicate names).
  6. `obs_open_module`: `MISSING_EXPORTS` -> `LOG_DEBUG`, return, **not a failure, no module object**. `FAILED_TO_OPEN` -> disabled module (`FAILED_TO_OPEN`) and **failure**. `MODULE_ERROR` -> **failure**, no module. `INCOMPATIBLE_VER` -> disabled module (`FAILED_TO_OPEN`) and **failure**. `HARDCODED_SKIP` -> return silently.
  7. `obs_init_module`: false -> `free_module(module)` then disabled module (`FAILED_TO_INITIALIZE`) with `module_type = info.type` (**not** a failure); true -> `module.module_type = info.type`.
  A *failure* appends `info.name;` to the failure string and increments the failure count.
* **S-035-R101** `obs_load_plugins(info, error)`: run *find in path* for the single path (`binary`, `data`, `type`) with the callback above; `error->count` = failure count; `error->failed_modules` = the failure string split on `;` (empty items dropped; NULL when there are no failures). `obs_module_failure_info_free` frees the list and sets it to NULL (idempotent).
* **S-035-R102** `obs_load_core_modules()`: if `core_modules_loaded` -> true immediately. Else run the platform core loader (R95) with the callback above; set `core_modules_loaded = (failure_count == 0)`; return it. A missing core *file* is **not** counted as a failure (Q-7); a core module whose library fails to open is.
* **S-035-R103** Modules loaded by the callback are pushed at the head of the lists, so the enabled list is the reverse of load order; load order within one path is the glob order.

### 5.11 Files, config paths, locale

* **S-035-R110** `obs_find_module_file(m, file)`: `m == NULL` -> NULL; `file == NULL` is treated as `""`; result = `data_path` (NULL = empty) + `/` (only if the result so far is non-empty, does not end with `/`, **and** `file` is non-empty) + `file`; return a copy iff the file/dir exists, else NULL. So `""` and `NULL` return the data directory itself when it exists, and a data path ending in `/` is not doubled.
* **S-035-R111** `obs_module_get_config_path(m, file)` = `core.module_config_path` (NULL = empty) + `/` (if non-empty and not ending in `/`) + `m->mod_name` + `/` + `file`. No existence check, never NULL (documentation claims NULL when unset, Q-6). `m == NULL` crashes in the reference: **D-3**.
* **S-035-R112** `obs_module_load_locale(m, default_locale, locale)`: any NULL argument -> `LOG_WARNING`, NULL. Load `locale/<default>.ini` through `obs_find_module_file`; missing/unreadable -> `LOG_WARNING "Failed to load '<default>'..."`, NULL. If `locale` equals `default_locale` ignoring case -> return the lookup. Else load `/locale/<locale>.ini` (leading slash; harmless) and merge it **over** the default (later wins); if that file cannot be added -> `LOG_WARNING`, but the default lookup is still returned.
* **S-035-R113** `obs_module_get_locale_string(m, key, &out)`: forwards to the module's `obs_module_get_string` when exported (return its result; `out` as set by it) else false with `out` untouched. `obs_module_get_locale_text(m, text)` = the translated string if found else `text` itself. (The reference asserts `m != NULL` in debug builds; calling it with NULL is outside the contract.)
* **S-035-R114** The core calls `set_locale(core.locale)` only at open (R63); changing locale later is the frontend's job.

## 6. Algorithms

### 6.1 Find in path

```
find_in_path(omp, cb, param):
  pattern = omp.bin
  if pattern contains "%module%":  directories = true;  pattern = text before the FIRST "%module%"
  else directories = false
  if pattern != "" and pattern does not end with '/': pattern += '/'
  pattern += "*";   if not directories: pattern += module_extension       // ".so"
  for each entry in glob(pattern):                  // sorted bytewise; "*" does not match names starting with '.'
     if entry.is_directory == directories: process(omp, entry.path, directories, cb, param)
process(omp, path, directories, cb, param):
  file = text after last '/' of path;  if file in {".", ".."} return
  name = file with everything from its LAST '.' removed
  if not directories: bin = path
  else: bin = directory_binary(omp.bin, name)  or fail
  data = data_dir(name, omp.data)                 // always succeeds, see 6.3
  if bin found: cb({bin, data, name, omp.type})
```

### 6.2 `directory_binary(bin_template, name)`

```
dir = bin_template with every "%module%" replaced by name;  if dir does not end with '/': dir += '/'
cand = dir + name + ext
if exists(cand) return cand
cand = dir + "lib" + name + ext
if exists(cand) return cand
return NOT FOUND                                // (the intermediate "legacy .so" candidate in the reference is dead code)
```

Consequence: a directory named `delta.v2` yields name `delta`, whose binary path uses `delta`, not `delta.v2` -> normally not found -> skipped.

### 6.3 `data_dir(name, data_template)`

```
d = data_template with every "%module%" replaced by name; remove ONE trailing '/' if present
if not exists(d) and name starts with "lib" (ASCII case-insensitive, first 3 bytes):
     d = data_template with "%module%" replaced by name[3..]; remove one trailing '/'
return d                                           // returned even if it does not exist
```

### 6.4 Module name derivation

`mod_name(file) = file[0 .. len(file) - ext_len]` with `ext_len = len(".so")`. A file name shorter than `ext_len` is outside the contract.

### 6.5 Reference-count protocol

`create: strong=0, weak=0`. `get_ref: strong++` (only if `strong >= 0`). `release: strong--; if strong == -1 -> destroy(); weak--`. `weak_ref: weak++`. `weak_release: weak--; if weak == -1 -> free control`.
Worked example (vector `03-service-refs`): create (0,0); `get_ref` (1,0); `weak` (1,1); `weak_to_service` (2,1); three releases -> strong -1, destroy, weak 1->0; later `weak_to_service` -> NULL; `weak_addref` (1), `weak_release` (0), `weak_release` (-1) frees the block.

### 6.6 Version gate

`accept(v) = (v & 0xFFFF0000) <= HOST_VER` where `HOST_VER = (major<<24)|(minor<<16)|patch` of the host. (Because the host's patch bits are part of the right-hand side, a module built for host major.minor is accepted regardless of patch.)

### 6.7 Service lifecycle summary

`create -> [linked, active=false] --(activate, needs output)--> active --(destroy/release)--> [unlinked, destroy=true, still alive] --deactivate--> freed`; or `create -> destroy/release -> freed immediately`.

### 6.8 Frontend order (informative)

`obs_add_module_path`* (+ safe/disabled lists) -> `obs_load_core_modules` -> `obs_load_plugins` per plugin path -> `obs_post_load_modules`. `obs_load_all_modules(2)` do nothing in this version.

## 7. Concurrency

The service list is protected by the core's service mutex during link/unlink only (R10, R13). Module functions (`obs_open_module`, `obs_init_module`, `obs_load_*`, `free_module`, registration) are single-threaded startup/shutdown operations touching unsynchronised lists and the global `loadingModule`; they MUST NOT be called concurrently. Reference counts are atomic (R14/R15 may run on any thread). Service callbacks are invoked on the caller's thread with no core lock held.

## 8. Reference defects and quirks

* **Q-1** (`06`) `create` returning NULL does not fail creation: the object exists, `data == NULL`, `update`/`activate`/... are still called with NULL data, `destroy` is not called. SHOULD match.
* **Q-2** (`02`) Private services are in the same list as public ones (`service_list`). SHOULD match.
* **Q-3** (`04`) The `activate` warning text says `obs_service_deactivate`. Text only.
* **Q-4** (`13`) With a manifest present, `obs_get_module_name` returns `display_name` even when it is the empty string, hiding `obs_module_name()`. SHOULD match.
* **Q-5** (`11`) The module name drops the last 3 bytes of the file name even if it has no extension (`libpre-mod` -> `libpre-`). SHOULD match.
* **Q-6** (`23`) `obs_module_get_config_path` with no configured directory returns the relative `"<mod>/<file>"`, not NULL as documented. SHOULD match (callers rely on non-NULL).
* **Q-7** (`21`,`22`) A missing core module file logs an error and ends the core loop but does not count as failure, so `core_modules_loaded` becomes true and the remaining core modules are never loaded. SHOULD match.
* **Q-8** (`18`) When `obs_module_load` returns false after registering types, the registrations stay in the global tables (their ids are not attributed to any module afterwards, `service_module` -> MISSING) and a retry reports "already exists". SHOULD match.
* **Q-9** (`25`) Rejected sources that pass the type/duplicate/size checks stay recorded in the loading module's `sources` list. SHOULD match.
* **Q-10** (`15`) `lib`-prefix data fallback is applied even if neither directory exists. MUST match.
* **Q-11** (`15`,`16`) Names are cut at the last `.`; a directory `delta.v2` is therefore skipped and a flat file `dotted.name.so` is named `dotted.name`. MUST match.
* **Q-12** (`18`,`22`) Every retry creates another disabled-module entry for the same failing file (lists accumulate). SHOULD match.
* **Q-13** (`02`) `obs_service_update(s, NULL)` still invokes the plugin's `update`. SHOULD match.
* **D-1** (`06`,`26`) `obs_service_get_supported_{video,audio}_codecs(NULL)` dereference NULL. REQUIRED: log at debug like the other getters and return NULL.
* **D-2** (`26`) `obs_add_module_path` leaves the path's `type` field uninitialised; `obs_find_modules2` hands that garbage to callbacks. REQUIRED: `OBS_MODULE_TYPE_UNDEFINED` (0). Vectors do not print this value for `find`.
* **D-3** (`26`) `obs_module_get_config_path(NULL, f)` dereferences NULL; REQUIRED: return NULL. Also: `obs_module_get_locale_string(NULL, ...)` asserts in debug builds; REQUIRED: return false.
* **D-4** (code reading) `obs_module_load_metadata` called a second time leaks the previous metadata; never done by the core. No requirement.

## 9. Implementation notes (non-normative)

* Plugins call these functions through the C ABI: export `#[no_mangle] extern "C"` functions, `#[repr(C)]` descriptors with the offsets of 3.2 and the `obs_module_info2` / `obs_runtime_module_info` layouts. Keep `obs_service_info` copy semantics (the table owns a copy; `type_data` is borrowed).
* Use `libloading` (or raw `dlopen`) with `RTLD_NOW`; do not `dlclose` (R67). `os_dlopen`'s ".so appended when the path has no `.so`" is a platform rule (S-023).
* Glob order is bytewise on the full path in the "C" locale; do not use locale collation. Use `read_dir` + sort for the same result for patterns of the shape `<dir>/*[ext]`.
* `mod_name` truncation (R63) must be reproduced byte-wise; use bytes, not chars.
* The two module lists have O(n) head-push and unlink-by-predecessor operations; a `Vec` with front insertion or an intrusive list both work; iteration order is observable.
* Windows: extension `.dll`; "is OBS plugin" check inspects the PE export table; `obs_post_load_modules` first resets symbol paths; core loader uses different templates. macOS: extension `.so` first then `.so` fallback, hard-coded skip (`MODULE_HARDCODED_SKIP`) for an obsolete obs-browser under "Library/Application Support/obs-studio", loader is `.m`. None of this is covered by vectors.

## 10. Conformance coverage

Runner commands are in `test/conformance/README.md` (section "modservice runner"). Rule -> scripts (all under `scripts/modservice/`):

| Rule | Scripts |
|---|---|
| R01-R04 | 01-service-register |
| R10-R12 | 02-service-create-update, 06-service-null-and-defects |
| R13-R16 | 02-service-create-update, 03-service-refs, 04-service-output |
| R20-R25 | 02-service-create-update, 06-service-null-and-defects |
| R26-R30 | 05-service-queries, 06-service-null-and-defects, 04-service-output (initialize) |
| R40-R43 | 04-service-output |
| R50 | 14-module-registration, 18-module-load-plugins |
| R60-R63 | 10-module-open, 11-module-open-success |
| R64-R69 | 12-module-init-free, 11-module-open-success, 18, 19 |
| R70-R72 | 13-module-metadata, 14-module-registration |
| R80-R83 | 24-register-other-kinds, 25-register-module-recording |
| R90-R93 | 15-module-discovery, 16-module-discovery-dirs, 19, 20 (list policies) |
| R94 | 17-module-find-core |
| R95, R102 | 21-module-core-loader, 22-module-core-loader-failure |
| R100-R101, R103 | 18-module-load-plugins, 19-module-load-policy, 20-module-safe-mode |
| R110-R113 | 23-module-files-locale |
| R114 | 11-module-open-success, 23-module-files-locale |
| Q/D | 26-module-defects plus the scripts named in section 8 |

## 11. Open questions / not done

* The **Rust sufficiency check was not performed** (explicit cost-control instruction). Treat the spec as unverified by an independent implementation.
* The interplay with the real `obs_context_data_*` layer (name de-duplication in `obs_context_data_insert_name`, UUIDs, hotkey registration for services) is outside this spec; the runner uses a faithful minimal stand-in (`gen/modservice_stubs.c`) whose observable behaviour (naming, settings sharing) is in 3.1.
* Platform-specific loaders (Windows, macOS) and `get_plugin_info` on Windows were read but not testable here.
* `LOG_WARNING` vs `LOG_ERROR` levels are as printed in the vectors; texts are informative.
* `obs_module_get_locale_string(NULL)` aborts through `assert` in debug builds; release behaviour (return false) is assumed.
