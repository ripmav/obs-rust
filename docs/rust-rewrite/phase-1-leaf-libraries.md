# Phase 1: Leaf libraries

Goal: replace the dependency-free building blocks with Rust crates, exposed through `obs-ffi` with the original C headers still valid. Each task swaps one C file group behind `OBS_ENABLE_RUST`.

Pattern for every task: (1) Rust crate with a safe API, (2) `extern "C"` shim with the exact C signature, (3) `cbindgen` check that the generated header is compatible with the existing one, (4) goldens from P0-14 pass, (5) C file kept but excluded from the build when `OBS_ENABLE_RUST=ON`.

## A. `obs-util` (`libobs/util/`)

- [ ] **P1-01 Memory helpers** (S): `bmem.c/h` (`bmalloc`, `brealloc`, `bfree`, `bmemdup`, `bstrdup`, leak counter). 32-byte-aligned blocks, fatal zero-size allocation and the `bnum_allocs()` counter exactly as in `specs/S-001-bmem-base.md`. Done when: allocation counter test matches C.
- [ ] **P1-02 String buffer `dstr`** (M): `dstr.c/h`. Safe `DString` type + C shim with identical behaviour for `dstr_cat`, `dstr_replace`, `dstr_find`, `dstr_insert`, printf variants. Done when: fuzz test (`cargo-fuzz`) agrees with C for 1M random ops.
- [ ] **P1-03 Dynamic arrays `darray`** (M): `darray.h`. Keep the macro-compatible C layout (`{ptr, num, capacity}`); Rust side offers `DArray<T>`. Done when: layout test via `size_of`/`offset_of`.
- [ ] **P1-04 Byte ring buffer `deque`** (S): `libobs/util/deque.h`. Layout-compatible Rust type with the same push/pop/peek/place/upsize behaviour (`specs/S-005-darray-deque.md`).
- [ ] **P1-05 Threading primitives** (M): `threading.h`, `threading-posix.c`, `threading-windows.c`: `os_event_*`, `os_sem_*`, `pthread_mutex` wrappers, `os_set_thread_name`. Use `std::sync` + `parking_lot` only if justified. Done when: stress tests pass on 3 OSes.
- [ ] **P1-06 Time and sleep** (S): `platform.c` time functions (`os_gettime_ns`, `os_sleepto_ns`, `os_sleep_ms`) using `Instant` + platform high-res timers.
- [ ] **P1-07 File system utilities** (M): `platform*.c` (`os_fopen`, `os_mkdirs`, `os_glob`, `os_get_config_path`, `os_rename`, `os_quick_write_utf8_file`). UTF-8 on Windows. Done when: tests cover long paths and non-ASCII names.
- [ ] **P1-08 Text encoding** (S): `utf8.c`, `text-lookup.c/h`, `lexer.c/h`, `cf-lexer.c`, `cf-parser.c`. Replace utf8 helpers with `std`; port the lexers with golden token dumps (effect parser depends on them in Phase 3).
- [ ] **P1-09 Config file `config-file`** (M): `config-file.c/h` (INI read/write, defaults, `config_get_*`/`set_*`). Done when: round trip of 10 real `global.ini`/`basic.ini` files is byte-identical.
- [ ] **P1-10 Base64, CRC32, hash** (S): `base64.c`, `crc32.c`, `deps/blake2` -> `base64`, `crc32fast`, `blake2` crates. Done when: known-answer tests.
- [ ] **P1-11 Process and module loading helpers** (M): `dstr` + `platform` `os_dlopen/os_dlsym/os_dlclose`, `os_process_*` -> `libloading`, `std::process`.
- [ ] **P1-12 Profiler** (M): `profiler.c/h`, `profiler.hpp`. Rust implementation with identical named-scope API and the text/JSON snapshot output.
- [ ] **P1-13 Logging** (S): `base.c/h` (`blog`, `base_set_log_handler`, `bcrash`). Keep the C callback interface; internal API uses `log`/`tracing` facade.
- [ ] **P1-14 Miscellaneous util** (S): `sse-intrin.h`, `source-profiler`, `simde` wrappers: decide per file keep or drop; document.

## B. `obs-math` (`libobs/graphics/*.c`)

- [ ] **P1-15 `vec2/vec3/vec4`** (M): repr(C) types, all C functions (`vec3_add`, `vec3_norm`, `vec3_transform`, ...). Use `glam` internally only if layout can be kept; else hand-rolled. Golden: 10k random vectors.
- [ ] **P1-16 `matrix3/matrix4`** (M): multiply, inverse, from/to quat, `matrix4_transpose`, `matrix4_determinant`. Tolerance 1e-6, benchmark must not regress.
- [ ] **P1-17 `quat`, `axisang`, `plane`, `bounds`** (M): `quat.c`, `axisang.c`, `plane.c`, `bounds.c`.
- [ ] **P1-18 `math-extra`, `half`, `srgb`** (S): `math-extra.c`, `half.h`, `srgb.h`; use the `half` crate. Done when: all f16 conversion edge cases (NaN, subnormal, inf) match.
- [ ] **P1-19 SIMD paths** (M): replace SSE/NEON intrinsics with `std::arch` or `std::simd`; dispatch at runtime.

## C. `obs-signal` (`libobs/callback/`)

- [ ] **P1-20 `calldata`** (M): `calldata.c/h`: typed key-value stack with pointers, strings, ints, floats, bools. Keep the C-visible layout. Fuzz against C.
- [ ] **P1-21 `decl` and `proc`** (M): `decl.c/h`, `proc.c/h`: declaration string parser (`"void foo(in int x, out string y)"`), proc handlers.
- [ ] **P1-22 `signal`** (L): `signal.c/h`: signal handler with add/remove callbacks, thread-safe emit, re-entrancy (callbacks that disconnect others while emitting). Done when: re-entrancy and deadlock tests pass; concurrency tested with `loom`.
- [ ] **P1-23 Typed Rust API over signals** (M): `Signal<Args>` generic API for in-tree Rust code, bridged to `calldata`.

## D. `obs-data` (`libobs/obs-data.c`, `obs-properties.c`)

- [ ] **P1-24 JSON engine** (M): replace `jansson` use and `deps/json11` with `serde_json`, preserving key order and number formatting that the saved JSON relies on. Done when: 20 real collections round-trip byte-identical.
- [ ] **P1-25 `obs_data_t` core** (L): ref-counted object with typed get/set, defaults, `obs_data_item_*`, `obs_data_array`, `obs_data_set_*_default`, `obs_data_erase`, `obs_data_apply`, `obs_data_get_json`. Done when: C reference dump matches for the corpus.
- [ ] **P1-26 `obs_data` autoselect and `obs_data_set_*_array` ownership** (S): reference-count semantics tests (no leaks, no double free) under Miri and ASan.
- [ ] **P1-27 `obs_properties`** (L): `obs-properties.c/h`: all property kinds (list, path, editable list, font, group, button, color), modified callbacks, `obs_property_list_*`. Done when: ABI plugin test (P0-15) exercises each kind.
- [ ] **P1-28 `obs-hotkey` data layer** (S): `obs-hotkey-name-map.c`, key combo serialisation. Core hotkey system comes in Phase 4.

## E. `shared/` leaf helpers

- [ ] **P1-33 Port the `shared/` leaf helpers** (M) (added): the five dependency-free helpers under `shared/` that no earlier task owned — `shared/bpm` (P1-33.1), `shared/file-updater` (P1-33.2), `shared/happy-eyeballs` (P1-33.3), `shared/obs-inject-library` (P1-33.4), `shared/opts-parser` (P1-33.5). bpm, file-updater, happy-eyeballs and opts-parser are specified by S-024 (M8/M10/M9/M1) and conformance-tested by the `helpers` area; `obs-inject-library` is a thin `LoadLibrary`/`dlopen` helper with no spec — implement from the C (it is ~100 lines, no clean-room value). Done when: S-024 helpers scripts pass and the C files are excluded from the build behind `OBS_ENABLE_RUST=ON`. Note: `shared/ipc-util` is owned by P5-31.6 (its only consumers are win-capture game capture and its hook build), `shared/obs-d3d8-api`/`shared/obs-hook-config` stay C per D-08, and `shared/qt`/`shared/properties-view` are frontend (Phase 6).

## F. Integration

- [ ] **P1-29 `obs-ffi` header diff check** (S): CI step comparing `cbindgen` output with the committed `libobs/*.h` for all migrated functions.
- [ ] **P1-30 Switch default** (S): turn `OBS_ENABLE_RUST` on for Phase 1 crates in nightly builds, run for two weeks, then default on.
- [ ] **P1-31 Remove vendored deps** (S): delete `deps/blake2`, `deps/json11` once nothing links them.
- [ ] **P1-32 Phase 1 review** (S): benchmarks vs baseline, crash-free soak, tick this file.

Exit criteria: all `libobs/util`, math, `callback` and `obs-data` code is served by Rust crates on all OSes; goldens, ABI test and benchmarks pass.
