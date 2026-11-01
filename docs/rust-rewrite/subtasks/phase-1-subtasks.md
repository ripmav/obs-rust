# Phase 1 subtasks

Parent file: [../phase-1-leaf-libraries.md](../phase-1-leaf-libraries.md).
**Clean-room specs written for this phase** (use them instead of reading the C): S-001 allocator/logging (P1-01, P1-13), S-002 calldata (P1-20), S-003 declaration language (P1-21), S-004 signals/procs (P1-22, P1-23), S-005 darray/deque (P1-03, P1-04). Index: [../specs/README.md](../specs/README.md).
 Each subtask has **Files**, **Steps**, **Done when**.

**Recipe** (referred to as "apply recipe"): (1) safe Rust API in the crate; (2) `extern "C"` shim with the exact C signature in `obs-ffi`; (3) `cbindgen` output diffed against the existing header; (4) golden/unit/fuzz test passes against the C reference dump; (5) the C source is excluded when `OBS_ENABLE_RUST=ON`; (6) matrix file in `docs/tests/`.

Corrections to the earlier plan, verified in the tree: the ring buffer is `libobs/util/deque.h` (there is **no** `circlebuf_*` compatibility alias in this tree; `source-profiler.c` has an unrelated private `ucirclebuf`), and `libobs/util` also contains `bitstream.*`, `serializer.h`, `array-serializer.*`, `file-serializer.*`, `buffered-file-serializer.*`, `pipe*.c`, `task.*`, `platform-nix-dbus.c`, `platform-nix-portal.c`, `util_uint64.h`, `util_uint128.h`, `uthash.h`, `curl/` and `windows/`. These are covered in P1-14.

## A. `obs-util` (`libobs/util/`)

### P1-01 Memory helpers
- [ ] **P1-01.1 Allocator wrappers**
  - Files: `libobs/util/bmem.c/.h`
  - Steps: `bmalloc`, `brealloc`, `bfree`, `bzalloc`, `bmemdup`, `bstrdup` using the system allocator; alignment ≥ 16 bytes.
  - Done when: unit tests for zero-size and null-free match C.
- [ ] **P1-01.2 Counter and hooks** — `bnum_allocs()` (`base_set_allocator` does not exist); atomic counter. Spec: `specs/S-001-bmem-base.md` (R04–R09). Done when: concurrent test (8 threads × 100k allocs) returns 0 at end.
- [ ] **P1-01.3 Apply recipe** and compare with `test/cmocka` style leak check.

### P1-02 `dstr`
- [ ] **P1-02.1 Type and growth**
  - Files: `libobs/util/dstr.c/.h`, `dstr.hpp`
  - Steps: `struct dstr {char*, len, capacity}` layout; capacity policy identical.
  - Done when: layout test passes.
- [ ] **P1-02.2 Core ops** — `copy`, `cat`, `ncat`, `insert`, `erase`, `resize`, `compare`, `find`, `find_i`. Done when: table tests including empty and overlapping inputs.
- [ ] **P1-02.3 Replace and format** — `dstr_replace`, `cat_strref`, case variants, `strip`. `dstr_printf`, `dstr_catf`, `dstr_vprintf`, `dstr_vcatf` are variadic/`va_list`: implement in the C shim (primer §5.5), not in Rust. Done when: matches C on 1000 generated cases.
- [ ] **P1-02.4 Fuzz and recipe** — `cargo-fuzz` target applying random op sequences to Rust and C builds and comparing; apply recipe.

### P1-03 Dynamic arrays
- [ ] **P1-03.1 Layout** — `struct darray {void* array; size_t num; size_t capacity}`; `DArray<T>` with `repr(C)`. Done when: `offset_of` test.
- [ ] **P1-03.2 C helpers** — functions behind `da_push_back`, `da_insert`, `da_erase`, `da_move_item`; macros in the header stay. Done when: `test/cmocka/test_darray.c` passes unchanged.
- [ ] **P1-03.3 Recipe** — apply and add matrix.

### P1-04 Deque (`libobs/util/deque.h`)
- [ ] **P1-04.1 Data structure**
  - Files: `libobs/util/deque.h`
  - Steps: `deque_push_back/front`, `pop_front/back`, `peek_front/back`, `place`, `push_back_zero`, `reserve`, `free`.
  - Done when: wraparound property test equals the C result for random op sequences.
- [ ] **P1-04.2 Layout sharing** — the `struct deque` fields (`data,size,start_pos,end_pos,capacity`) are touched directly by C inline code; keep them identical (spec `S-005`, Q-D1: `end_pos` may equal `capacity`). Done when: a C test drives a Rust-owned deque with C inline functions and gets the same dump as `deque` vectors.
- [ ] **P1-04.3 Recipe**

### P1-05 Threading primitives
- [ ] **P1-05.1 Events**
  - Files: `libobs/util/threading.h`, `threading-posix.c/.h`, `threading-windows.c/.h`
  - Steps: `os_event_init/wait/timedwait/signal/reset/destroy` for auto and manual reset.
  - Done when: signal-before-wait and timeout tests pass.
- [ ] **P1-05.2 Semaphores and thread names** — `os_sem_*`, `os_set_thread_name`. Done when: thread name visible in debugger on each OS.
- [ ] **P1-05.3 Windows parity** — compare behaviour with `threading-windows.c` (SRW locks, `WaitForSingleObject`). Done when: Windows CI green.
- [ ] **P1-05.4 Stress and recipe** — 64 threads × 1M signals; apply recipe.

### P1-06 Time and sleep
- [ ] **P1-06.1 Clock** — `os_gettime_ns` monotonic; `os_gettime_ms`. Done when: never decreases in a 10 s loop.
- [ ] **P1-06.2 Sleep** — `os_sleepto_ns`, `os_sleepto_ns_fast`, `os_sleep_ms` using `clock_nanosleep` / waitable timers / `mach_wait_until`. Done when: jitter histogram is attached.
- [ ] **P1-06.3 Compare and recipe** — jitter ≤ C's p99.

### P1-07 File system utilities
- [ ] **P1-07.1 Open/rename/mkdir**
  - Files: `libobs/util/platform.c`, `platform-nix.c`, `platform-windows.c`, `platform-cocoa.m`
  - Steps: `os_fopen`, `os_wfopen`, `os_mkdir`, `os_mkdirs`, `os_rename`, `os_copyfile`, `os_unlink`.
  - Done when: tests with unicode and long (>260) paths on Windows.
- [ ] **P1-07.2 Directory iteration** — `os_opendir/readdir/closedir`, `os_glob`. Done when: sorted result equals C.
- [ ] **P1-07.3 Config/data paths** — `os_get_config_path`, `os_get_program_data_path`, `os_get_abs_path`, `os_get_free_disk_space`, portal/dbus helpers in `platform-nix-dbus.c`/`platform-nix-portal.c` (Flatpak). Done when: Flatpak path test.
- [ ] **P1-07.4 Recipe and matrix**

### P1-08 Text encoding and lexers
- [ ] **P1-08.1 UTF-8/wide helpers** — `utf8.c`, `os_utf8_to_wcs*`, `os_wcs_to_utf8*` using `std`. Done when: invalid-sequence behaviour documented and equal.
- [ ] **P1-08.2 `lexer.c`** — `lexer_getbasetoken`, strref helpers. Done when: token golden for 5 sample files.
- [ ] **P1-08.3 `cf-lexer.c` / `cf-parser.c`** — preprocessor tokens, error list. Done when: golden dump for every `*.effect` under `libobs/data/`.
- [ ] **P1-08.4 `text-lookup.c`** — locale `.ini` lookup with fallback. Done when: loads `frontend/data/locale/en-US.ini` and gets identical values.

### P1-09 Config file
- [ ] **P1-09.1 Parser** — `libobs/util/config-file.c`: sections, `key=value`, comments, BOM handling. Done when: 10 real files parse.
- [ ] **P1-09.2 Typed API** — `config_get_string/int/uint/bool/double`, `set_*`, `set_default_*`, `has_user_value`. Done when: default vs user precedence tested.
- [ ] **P1-09.3 Save** — atomic write via temp file + rename, `.bak` behaviour. Done when: power-loss simulation (kill after temp write) leaves old file intact.
- [ ] **P1-09.4 Round trip** — byte-identical for corpus. Done when: golden.

### P1-10 Base64, CRC32, hashes
- [ ] **P1-10.1 base64** — `libobs/util/base.c` helpers/`base64` in `libobs/util`; use `base64` crate; padding behaviour equal. Done when: RFC 4648 vectors.
- [ ] **P1-10.2 CRC32** — `crc32.c`; `crc32fast`. Done when: vectors.
- [ ] **P1-10.3 blake2** — replace `deps/blake2` users with `blake2`. Done when: known-answer vectors and all callers compile.

### P1-11 Process and module loading
- [ ] **P1-11.1 Dynamic libs** — `os_dlopen`, `os_dlsym`, `os_dlclose`, `os_dlopen` error strings. Done when: loading missing lib returns NULL + same message class.
- [ ] **P1-11.2 Processes and pipes** — `os_process_pipe_create/read/write/destroy` (`libobs/util/pipe*.c`), `os_get_exe_path`. Done when: echo-process roundtrip on 3 OSes.
- [ ] **P1-11.3 Edge cases** — paths with spaces, non-UTF-8 bytes, closed pipe. Done when: tests.

### P1-12 Profiler
- [ ] **P1-12.1 Core** — `libobs/util/profiler.c/.h`: names, call tree, time stats. Done when: tree structure equals C for a scripted run.
- [ ] **P1-12.2 Output** — `profiler_print`, `profiler_print_time_between_calls`, `profiler_snapshot_dump_csv`. Done when: text/CSV goldens.
- [ ] **P1-12.3 RAII + recipe** — scoped guard for Rust users; also `source-profiler.c`.

### P1-13 Logging
- [ ] **P1-13.1 `blog` (stays C)** — `libobs/util/base.c/.h`: `blog`, `blogva`, `bcrash` are variadic/`va_list` and the handlers (`base_set_log_handler`, `base_set_crash_handler`) receive a `va_list`; stable Rust cannot define or consume these. Keep `base.c` as a small C file in `obs-ffi/c/` and expose `obs_ffi_log(level, msg)` to Rust. Done when: handler receives identical strings and the primer §5.5 decision is referenced.
- [ ] **P1-13.2 Crash hook** — `bcrash`, `base_set_crash_handler` (same C shim). Done when: handler invoked in test.
- [ ] **P1-13.3 `log` facade** — Rust `log` records forwarded to `blog`. Done when: level mapping table tested.

### P1-14 Miscellaneous util
- [ ] **P1-14.1 Inventory** — table of: `bitstream.*`, `serializer.h`, `array-serializer.*`, `file-serializer.*`, `buffered-file-serializer.*`, `task.*`, `pipe.*`, `uthash.h`, `util_uint64.h`, `util_uint128.h`, `sse-intrin.h`, `source-profiler.*`, `curl/`, `windows/`, `apple/` with decision keep/port/drop. Done when: every file has a decision.
- [ ] **P1-14.2 Port serializers and bitstream** — existing `test/cmocka/test_bitstream.c` and `test_serializer.c` must pass against the Rust build. Done when: they do.
- [ ] **P1-14.3 Port task queue and uint128** — `os_task_queue`, 128-bit helpers. Done when: unit tests.
- [ ] **P1-14.4 Document leftovers** — curl helpers remain FFI until Phase 6. Done when: noted in ADR 0005.

## B. `obs-math`

### P1-15 vec2/vec3/vec4
- [ ] **P1-15.1 Types** — `repr(C)` `Vec2/Vec3/Vec4` matching `libobs/graphics/vec*.h` (including the union with `ptr[4]`/SSE `m128`). Done when: size/align tests.
- [ ] **P1-15.2 Functions** — all exported `vec*_` functions (add, sub, mul, div, dot, cross, norm, dist, transform, min/max, `vec3_rand`). Done when: list diff with header is empty.
- [ ] **P1-15.3 Goldens** — 10k seeded random cases from `obs-ref-dump`; tolerance 1e-6 relative. Done when: pass.
- [ ] **P1-15.4 Recipe**

### P1-16 matrix3/matrix4
- [ ] **P1-16.1 Basics** — identity, copy, multiply, transpose, determinant. Done when: golden.
- [ ] **P1-16.2 Inverse and transforms** — `matrix4_inv`, translate/rotate/scale, `matrix4_from_quat`, `matrix4_from_axisang`. Done when: inverse·M ≈ I within 1e-5.
- [ ] **P1-16.3 matrix3** — conversions to/from `matrix4`, `matrix3_from_*`. Done when: golden.
- [ ] **P1-16.4 Benchmarks** — vs baseline. Done when: ≥ C speed.

### P1-17 quat, axisang, plane, bounds
- [ ] **P1-17.1 quat** — `quat_set`, mul, slerp/lerp, `quat_from_matrix`, `quat_get_dir`. Done when: golden.
- [ ] **P1-17.2 axisang/plane** — from/to quat, plane from tri, distance, intersection. Done when: golden.
- [ ] **P1-17.3 bounds** — `bounds_transform`, `bounds_intersects`, `bounds_intersects_ray`, `bounds_get_point`. Done when: golden for 100 shapes.
- [ ] **P1-17.4 Recipe**

### P1-18 math-extra, half, srgb
- [ ] **P1-18.1 math-extra** — `libobs/graphics/math-extra.c`: `rand_float`, `polar_to_cart`, `norm_to_...`. Done when: deterministic functions match.
- [ ] **P1-18.2 half** — `half.h` conversions via `half` crate; special values. Done when: all 65536 f16 values round trip identically to C.
- [ ] **P1-18.3 sRGB** — `srgb.h` linear↔sRGB. Done when: 256-point golden within 1e-6.

### P1-19 SIMD
- [ ] **P1-19.1 Find intrinsics** — grep `sse-intrin.h`, `simde` use in math; list functions. Done when: list.
- [ ] **P1-19.2 Implement** — `std::arch` (x86_64 SSE2/AVX2 with runtime check) and NEON. Done when: scalar fallback results equal.
- [ ] **P1-19.3 Benchmark** — on x86_64 and aarch64. Done when: not slower than C.

## C. `obs-signal` (`libobs/callback/`)

### P1-20 calldata
- [ ] **P1-20.1 Storage** — `calldata.c/.h` byte stack with name/size/data packing and `fixed` mode. Done when: byte layout equals C.
- [ ] **P1-20.2 Typed accessors** — `calldata_set/get_int/float/bool/ptr/string`, `_data`. Done when: mixed-type test.
- [ ] **P1-20.3 Fuzz + recipe** — random set/get sequences compared with C.

### P1-21 decl and proc
- [ ] **P1-21.1 Parser** — `decl.c`: `void name(in int x, out string y)`; error codes. Done when: 50 declarations plus invalid cases.
- [ ] **P1-21.2 Proc handler** — `proc.c`: add/remove/call, thread safety. Done when: concurrent add/call test.
- [ ] **P1-21.3 Golden** — parsed output dumps.

### P1-22 signal
- [ ] **P1-22.1 Handler** — `signal.c`: add signal by declaration, connect/disconnect. Done when: ordering equals C (callbacks run in connection order).
- [ ] **P1-22.2 Emit** — replicate `signal.c:290–350`: take the per-signal **recursive** mutex for the whole emit, iterate by index (array may grow), skip entries with `remove == true`, then erase removed entries after the loop; `signal_handler_disconnect` during emit only sets `remove` (thread-local `current_signal_cb` handles "disconnect self"). Then run global callbacks under `global_callbacks_mutex`. Done when: tests disconnect self, disconnect others, connect new, and emit the same signal recursively.
- [ ] **P1-22.3 Threads** — keep the lock structure (per-signal recursive mutex, handler mutex, global-callbacks recursive mutex); document any deviation. Done when: no deadlock under `loom` for connect-from-other-thread during emit.
- [ ] **P1-22.4 Recipe**

### P1-23 Typed Rust API
- [ ] **P1-23.1 Design** — `Signal<A>` + `Connection` guard in docs. Done when: ADR addendum.
- [ ] **P1-23.2 Bridge** — typed args to/from `calldata`. Done when: round trip test.
- [ ] **P1-23.3 Docs/tests**

## D. `obs-data`

### P1-24 JSON engine
- [ ] **P1-24.1 Library choice** — `serde_json` with `preserve_order`; compare to jansson usage in `libobs/obs-data.c`. Done when: note on differences (number formatting, escapes).
- [ ] **P1-24.2 Formatting** — indentation (4 spaces?) and float output equal to C; verify from corpus. Done when: diff empty.
- [ ] **P1-24.3 Corpus round trip** — all `test/fixtures` collections byte-identical.

### P1-25 `obs_data_t` core
- [ ] **P1-25.1 Object** — ref-counted item map, ordered. Done when: iteration order equals C.
- [ ] **P1-25.2 Typed API** — string/int/double/bool/obj/array, `*_default`, `*_autoselect`. Done when: all accessors in `obs-data.h` implemented.
- [ ] **P1-25.3 Arrays and items** — `obs_data_array_*`, `obs_data_item_*` iteration and mutation during iteration. Done when: tests from existing behaviour.
- [ ] **P1-25.4 Utility** — `apply`, `erase`, `clear`, `get_json`, `create_from_json(_file)`, `save_json_safe`, `obs_data_get_full_json`. Done when: file ops atomic.
- [ ] **P1-25.5 Reference dump** — compare on the corpus. Done when: pass.

### P1-26 Ownership
- [ ] **P1-26.1 Semantics tests** — `get_obj` returns new ref, setters take their own ref. Done when: refcount observed through debug API equal to C.
- [ ] **P1-26.2 Miri/ASan** — run on pure crates. Done when: clean.
- [ ] **P1-26.3 Fix findings**

### P1-27 `obs_properties`
- [ ] **P1-27.1 Container** — ordering, `obs_properties_apply_settings`, flags, parent/group. Done when: ordering test.
- [ ] **P1-27.2 Simple kinds** — bool, int, float, text (+types), path (+filters), button. Done when: each reachable via ABI test.
- [ ] **P1-27.3 Complex kinds** — list (string/int/float, disabled items), editable list, font, color, frame rate (ranges/options), group. Done when: same.
- [ ] **P1-27.4 Callbacks** — modified, button clicked, enabled/visible. Done when: callback returns `true` triggers refresh.
- [ ] **P1-27.5 ABI test** — extend the plugin of P0-15.

### P1-28 Hotkey data layer
- [ ] **P1-28.1 Name map** — `libobs/obs-hotkey-name-map.c`. Done when: all key names equal.
- [ ] **P1-28.2 Combo JSON** — `obs_hotkey_binding` save/load format. Done when: round trip of corpus hotkeys.
- [ ] **P1-28.3 Golden**

## E. `shared/` leaf helpers

### P1-33 `shared/` leaf helpers (added)
- [ ] **P1-33.1 `shared/bpm`** — apply recipe for S-024 M8 (counters, timestamps, SEI payloads, per-codec packet injection). Done when: the `bpm` scripts in `test/conformance/scripts/helpers/` pass against the C reference.
- [ ] **P1-33.2 `shared/file-updater`** — apply recipe for S-024 M10 (version comparison, fake-HTTP/in-memory-filesystem driven; the module has no hashes). Done when: M10 scripts pass.
- [ ] **P1-33.3 `shared/happy-eyeballs`** — apply recipe for S-024 M9 (address-list ordering, resolver hints, constants only — the socket race is out of scope and stays behind the C until a spec exists). Done when: M9 scripts pass.
- [ ] **P1-33.4 `shared/obs-inject-library`** — no spec (thin `LoadLibrary`/`dlopen` helper): port from the C, unit-test symbol resolution and the failure paths. Consumed by P5-31.7 (game-capture injection) and P5-32.6. Done when: C file excluded from the build, consumers link the crate.
- [ ] **P1-33.5 `shared/opts-parser`** — apply recipe for S-024 M1 (option-string parsing). Done when: M1 scripts pass.

## F. Integration

### P1-29 `obs-ffi` header diff
- [ ] **P1-29.1 cbindgen config** per crate with `cpp_compat`, prefix none. Done when: header generated.
- [ ] **P1-29.2 Diff script** — normalise whitespace; compare each function declaration to `libobs/**/*.h`. Done when: report of mismatches empty.
- [ ] **P1-29.3 CI** step.

### P1-30 Switch default
- [ ] **P1-30.1 Nightly** with `OBS_ENABLE_RUST=ON`.
- [ ] **P1-30.2 Observe** two weeks: crash reports, perf. Done when: no new crash signatures.
- [ ] **P1-30.3 Flip** default and update docs.

### P1-31 Remove vendored deps
- [ ] **P1-31.1 Usage scan** — `grep -r "blake2\|json11" --include=CMakeLists.txt --include=*.c*`. Done when: no users.
- [ ] **P1-31.2 Delete** `deps/blake2`, `deps/json11` and CMake references.

### P1-32 Phase 1 review
- [ ] **P1-32.1 Benchmarks** vs baseline table.
- [ ] **P1-32.2 Goldens/ABI** on 3 OSes.
- [ ] **P1-32.3 Tick** parent tasks and update estimates.
