# Phase 1: Leaf libraries

Goal: replace the dependency-free building blocks with Rust crates, exposed through `obs-ffi` with the original C headers still valid. Each task swaps one C file group behind `OBS_ENABLE_RUST`.

Pattern for every task: (1) Rust crate with a safe API, (2) `extern "C"` shim with the exact C signature, (3) `cbindgen` check that the generated header is compatible with the existing one, (4) goldens from P0-14 pass, (5) C file kept but excluded from the build when `OBS_ENABLE_RUST=ON`.

## A. `obs-util` (`libobs/util/`)

- [ ] **P1-01 Memory helpers** (S): `bmem.c/h` (`bmalloc`, `brealloc`, `bfree`, `bmemdup`, `bstrdup`, leak counter). 32-byte-aligned blocks, fatal zero-size allocation and the `bnum_allocs()` counter exactly as in `specs/S-001-bmem-base.md`. Done when: allocation counter test matches C.
- [ ] **P1-02 String buffer `dstr`** (M): `dstr.c/h`. Safe `DString` type + C shim with identical behaviour for the exported ops (`dstr_copy*`, `dstr_cat*`, `dstr_ncat*`, `dstr_insert*`, `dstr_remove`, `dstr_replace`, printf variants); `dstr_find*`/`dstr_resize` are `static inline` — layout-compatible only. Done when: fuzz test (`cargo-fuzz`) agrees with C for 1M random ops.
- [ ] **P1-03 Dynamic arrays `darray`** (M): `darray.h`. Keep the macro-compatible C layout (`{array, num, capacity}`); Rust side offers `DArray<T>`. Done when: layout test via `size_of`/`offset_of`.
- [ ] **P1-04 Byte ring buffer `deque`** (S): `libobs/util/deque.h`. Layout-compatible Rust type with the same push/pop/peek/place/reserve behaviour (`specs/S-005-darray-deque.md`). Done when: wraparound property test equals C for random op sequences.
- [ ] **P1-05 Threading primitives** (M): `threading.h`, `threading-posix.c`, `threading-windows.c`: `os_event_*` (incl. `os_event_try`), `os_sem_*`, `os_set_thread_name`. Use `std::sync` + `parking_lot` only if justified. Done when: stress tests pass on 3 OSes.
- [ ] **P1-06 Time and sleep** (S): `os_gettime_ns` (monotonic) and `os_sleepto_ns`/`os_sleepto_ns_fast`/`os_sleep_ms` from `platform-nix.c` (macOS: sleep functions are the nix implementation; the clock is `CLOCK_UPTIME_RAW` in `platform-cocoa.m`) and `platform-windows.c` (QPC), using `Instant` + platform timers. Done when: monotonicity and jitter tests pass on 3 OSes.
- [ ] **P1-07 File system utilities** (M): `platform*.c` (`os_fopen`, `os_mkdirs`, `os_glob`, `os_get_config_path`, `os_rename`, `os_quick_write_utf8_file`). UTF-8 on Windows. Done when: tests cover long paths and non-ASCII names.
- [ ] **P1-08 Text encoding and lexers** (M): `utf8.c`, `text-lookup.c/h`, `lexer.c/h`, `cf-lexer.c`, `cf-parser.c` (≈ 2,400 lines; `cf-lexer` alone is 1,376). Replace utf8 helpers with `std`; port the lexers with golden token dumps (effect parser depends on them in Phase 3). Done when: golden token dumps match C for the sample corpus.
- [ ] **P1-09 Config file `config-file`** (M): `config-file.c/h` (INI read/write, defaults, `config_get_*`/`set_*`). Done when: round trip of 10 real `global.ini`/`basic.ini` files is byte-identical.
- [ ] **P1-10 Base64, CRC32, hash** (S): `crc32.c` -> `crc32fast` crate; `deps/blake2` -> `blake2` crate (its only C users are the Windows `frontend/updater` and the what's-new feed `frontend/utility/WhatsNewInfoThread.cpp`, both Phase 6 — no libobs user). There is no `base64.c` in libobs (the only base64 in the tree is the in-tree librtmp copy `plugins/obs-outputs/librtmp/cencode.c/.h`, P5-17.4). Done when: known-answer tests.
- [ ] **P1-11 Process and module loading helpers** (M): `dstr` + `platform` `os_dlopen/os_dlsym/os_dlclose`, `os_process_*` -> `libloading`, `std::process`. Done when: echo-process roundtrip passes on 3 OSes and missing-lib errors match C.
- [ ] **P1-12 Profiler** (M): `profiler.c/h`, `profiler.hpp` (spec S-024 M7) + `source-profiler.c/h`. Rust implementation with identical named-scope API and the text/CSV snapshot output (`profiler_print`, `profiler_snapshot_dump_csv`). Done when: text/CSV goldens match C for a scripted run.
- [ ] **P1-13 Logging** (S): `base.c/h` (126 lines; `blog`, `base_set_log_handler`, `bcrash`). Keep the C callback interface (base.c stays C in `obs-ffi/c/`); internal API uses `log`/`tracing` facade. Done when: the handler receives identical strings and the level mapping is tested.
- [ ] **P1-14 Miscellaneous util** (M): the rest of `libobs/util/` — port `bitstream.*`/serializers (P1-14.2) and `task.*`/`util_uint64.h`/`util_uint128.h` (P1-14.3); decide keep/port/drop for `uthash.h` (replace with Rust collections), `util.hpp`, `windows/` (COM helpers) (P1-14.1); document `curl/` as FFI until Phase 6 (P1-14.4). Done when: every P1-14 file has a recorded decision and the P1-14.2/14.3 tests pass.

## B. `obs-math` (`libobs/graphics/*.c`)

- [ ] **P1-15 `vec2/vec3/vec4`** (M): repr(C) types for `vec*.h` plus the shared `basemath.hpp`/`math-defs.h` defs, all non-inline exported `vec*_` functions (`vec3_transform`, `vec3_rotate`, `vec3_rand`, `vec2_norm`, ...); the inline ops (`vec3_add`, `vec3_norm`, ...) stay in the C headers, layout-compatible only. Use `glam` internally only if layout can be kept; else hand-rolled. Done when: size/align tests pass and 10k goldens match within 1e-6.
- [ ] **P1-16 `matrix3/matrix4`** (M): multiply, inverse, from/to quat, `matrix4_transpose`, `matrix4_determinant`. Done when: goldens within 1e-6 (S-008) and benchmark does not regress.
- [ ] **P1-17 `quat`, `axisang`, `plane`, `bounds`** (M): `quat.c`, `axisang.c`, `plane.c`, `bounds.c` (exported functions only; the `static inline` ops stay in the headers). Done when: goldens match C for the seeded corpus.
- [ ] **P1-18 `math-extra`, `half`, `srgb`** (S): `math-extra.c`, `half.h`, `srgb.h`; use the `half` crate. Done when: all f16 conversion edge cases (NaN, subnormal, inf) match.
- [ ] **P1-19 SIMD paths** (M): replace SSE/NEON intrinsics with `std::arch` or `std::simd`; dispatch at runtime. Done when: SIMD results equal the scalar ones and are not slower than C on x86_64/aarch64.

## C. `obs-signal` (`libobs/callback/`)

- [ ] **P1-20 `calldata`** (M): `calldata.c/h`: typed key-value stack with pointers, strings, ints, floats, bools. Keep the C-visible layout. Done when: byte layout equals C and the fuzz test passes.
- [ ] **P1-21 `decl` and `proc`** (M): `decl.c/h`, `proc.c/h`: declaration string parser (`"void foo(in int x, out string y)"`), proc handlers. Done when: 50 declaration goldens parse identically to C.
- [ ] **P1-22 `signal`** (L): `signal.c/h`: signal handler with add/remove callbacks, thread-safe emit, re-entrancy (callbacks that disconnect others while emitting). Done when: re-entrancy and deadlock tests pass; concurrency tested with `loom`.
- [ ] **P1-23 Typed Rust API over signals** (M): `Signal<Args>` generic API for in-tree Rust code, bridged to `calldata`. Done when: typed args round-trip through `calldata` and the design docs are merged.

## D. `obs-data` (`libobs/obs-data.c`, `obs-properties.c`)

- [ ] **P1-24 JSON engine** (M): replace the `jansson` use in `obs-data.c` with `serde_json` (`deps/json11` belongs to the frontend, Phase 6), preserving key order and number formatting that the saved JSON relies on. Done when: 20 real collections round-trip byte-identical.
- [ ] **P1-25 `obs_data_t` core** (L): ref-counted object with typed get/set, defaults, `obs_data_item_*`, `obs_data_array`, `obs_data_set_*_default`, `obs_data_erase`, `obs_data_apply`, `obs_data_get_json`. Done when: C reference dump matches for the corpus.
- [ ] **P1-26 `obs_data` autoselect and `obs_data_set_*_array` ownership** (S): reference-count semantics tests (no leaks, no double free) under Miri and ASan. Done when: Miri and ASan are clean on the refcount corpus.
- [ ] **P1-27 `obs_properties`** (L): `obs-properties.c/h`: all property kinds (list, path, editable list, font, group, button, color), modified callbacks, `obs_property_list_*`. Done when: ABI plugin test (P0-15) exercises each kind.
- [ ] **P1-28 `obs-hotkey` data layer** (S): `obs-hotkey-name-map.c` (key names); combo save/load is specified by S-034 and implemented in P4-28.2 (vectors land here). Core hotkey system comes in Phase 4. Done when: all key names equal C and the S-034 binding vectors are verified.

## E. `shared/` leaf helpers

- [ ] **P1-33 Port the `shared/` leaf helpers** (L) (added): the five dependency-free helpers under `shared/` (≈ 2,500 lines) that no earlier task owned — `shared/bpm` (P1-33.1), `shared/file-updater` (P1-33.2), `shared/happy-eyeballs` (P1-33.3), `shared/obs-inject-library` (P1-33.4), `shared/opts-parser` (P1-33.5). bpm, file-updater, happy-eyeballs and opts-parser are specified by S-024 (M8/M10/M9/M1) and conformance-tested by the `helpers` area; `obs-inject-library` is a thin `LoadLibrary`/`dlopen` helper with no spec — implement from the C (it is 134 lines, no clean-room value). Done when: the S-024 `helpers` conformance scripts pass and the crates are in the workspace. The C files stay in the build during Phase 1 — none of them is part of `libobs`; they are linked by unported C consumers (bpm: `obs-outputs` mp4 output P5-18 + frontend multitrack output P6-31.3; file-updater: `rtmp-services` P5-06 + `win-capture` P5-31; happy-eyeballs: `obs-outputs` P5-17; opts-parser: `obs-ffmpeg` P5-11/P5-12/P5-41.3 + `obs-x264` P5-07 + `obs-nvenc` P5-39 + `obs-outputs` mp4 P5-18; inject-library: `win-capture` P5-31.6/P5-31.7) and are excluded only when those consumers are ported. Note: `shared/ipc-util` is owned by P5-31.6 (its only consumers are win-capture game capture and its hook build), `shared/obs-d3d8-api`/`shared/obs-hook-config` stay C per D-08, and `shared/qt`/`shared/properties-view` are frontend (Phase 6).

## F. Integration

- [ ] **P1-29 `obs-ffi` header diff check** (S): CI step comparing `cbindgen` output with the committed `libobs/*.h` for all migrated functions. Done when: the CI diff report is empty for all migrated headers.
- [ ] **P1-30 Switch default** (S): turn `OBS_ENABLE_RUST` on for Phase 1 crates in nightly builds, run for two weeks, then default on. Done when: two crash-free weeks on nightly, then the default flips and the docs are updated.
- [ ] **P1-31 Remove vendored deps** (S): verify no libobs target links `deps/blake2` or `deps/json11` (both are frontend-only: blake2 — `frontend/updater` P6-32 + what's-new feed P6-23.2; json11 — the frontend's OAuth P6-29 / importers P6-26); the deletion itself happens in P7-04 once those C consumers are ported. Done when: the usage scan shows no libobs user and the deferral is recorded (P1-31.2).
- [ ] **P1-32 Phase 1 review** (S): benchmarks vs baseline, crash-free soak, tick this file. Done when: review notes land and this file is ticked.

Exit criteria: all `libobs/util`, math, `callback` and `obs-data` code is served by Rust crates on all OSes; goldens, ABI test and benchmarks pass.
