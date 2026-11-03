# Phase 1 subtasks

Parent file: [../phase-1-leaf-libraries.md](../phase-1-leaf-libraries.md).
**Clean-room specs written for this phase** (use them instead of reading the C): S-001 allocator/logging (P1-01, P1-13), S-002 calldata (P1-20), S-003 declaration language (P1-21), S-004 signals/procs (P1-22, P1-23), S-005 darray/deque (P1-03, P1-04), S-006 dstr (P1-02), S-007 config-file (P1-09), S-008 vec3/4/quat/matrix4/axisang (P1-15…P1-17), S-009 vec2/matrix3/plane/bounds/math-extra/half/sRGB (P1-15, P1-16, P1-17, P1-18; Draft — second-author check pending), S-010 serializers/bitstream/text-lookup (P1-08.4, P1-14.2), S-011 lexer/cf-parser (P1-08.2, P1-08.3), S-014 obs_data (P1-24, P1-25), S-015 obs_properties (P1-27), S-034 hotkeys (P1-28). No spec yet: threading primitives and time/sleep (P1-05, P1-06 — D-27, old backlog S-017); the rest of the phase is specified: P1-07 → S-023, P1-10.2 → S-055 (crc32), P1-11 → S-055 (pipes) + S-024 M3 (process args), P1-12 → S-024 M7, P1-14.3 → S-024 M2/M6, P1-24/P1-25 → S-014, P1-27 → S-015, P1-28 → S-034 (P1-10.3 blake2 is vector-only, no spec). Index: [../specs/README.md](../specs/README.md).
 Each subtask has **Files**, **Steps**, **Done when**.

**Recipe** (referred to as "apply recipe"): (1) safe Rust API in the crate; (2) `extern "C"` shim with the exact C signature in `obs-ffi`; (3) `cbindgen` output diffed against the existing header; (4) golden/unit/fuzz test passes against the C reference dump; (5) the C source is excluded when `OBS_ENABLE_RUST=ON`; (6) matrix file in `docs/tests/`. Every task that swaps a C source group ends with an "apply recipe" subtask; where the C source stays (P1-10.3 `deps/blake2`, P1-13 `base.c`, the P1-33 C files until their consumers are ported) the remaining recipe steps apply and the subtask says so.

Corrections to the earlier plan, verified in the tree: the ring buffer is `libobs/util/deque.h` (there is **no** `circlebuf_*` compatibility alias in this tree; `source-profiler.c` has an unrelated private `ucirclebuf`), and `libobs/util` also contains `bitstream.*`, `serializer.h`, `array-serializer.*`, `file-serializer.*`, `buffered-file-serializer.*`, `pipe*.c`, `task.*`, `platform-nix-dbus.c`, `platform-nix-portal.c`, `util_uint64.h`, `util_uint128.h`, `uthash.h`, `curl/` and `windows/`. These are covered by P1-07.3 (portal/dbus), P1-11.2 (pipes) and P1-14 (the rest).

## A. `obs-util` (`libobs/util/`)

### P1-01 Memory helpers
- [ ] **P1-01.1 Allocator wrappers**
  - Files: `libobs/util/bmem.c/.h`
  - Steps: `bmalloc`, `brealloc`, `bfree`, `bzalloc`, `bmemdup`, `bstrdup` using the system allocator; 32-byte alignment (`base_get_alignment()` returns 32: `_aligned_malloc` on Windows, offset-header hack on POSIX).
  - Done when: unit tests for zero-size and null-free match C.
- [ ] **P1-01.2 Counter and hooks** — `bnum_allocs()` (`base_set_allocator` does not exist); atomic counter. Spec: `specs/S-001-bmem-base.md` (R04–R09). Done when: concurrent test (8 threads × 100k allocs) returns 0 at end.
- [ ] **P1-01.3 Apply recipe** and compare with `test/cmocka` style leak check.

### P1-02 `dstr`
- [ ] **P1-02.1 Type and growth**
  - Files: `libobs/util/dstr.c/.h`, `dstr.hpp`
  - Steps: `struct dstr {char*, len, capacity}` layout; capacity policy identical.
  - Done when: layout test passes.
- [ ] **P1-02.2 Core ops** — the exported ops (41 in `dstr.h`): the `dstr_copy*`/`dstr_cat*`/`dstr_ncat*`/`dstr_insert*` families (incl. `_strref`/`_dstr` variants), `dstr_ncopy(_dstr)`, `dstr_init_copy_strref`, `dstr_remove`, `dstr_replace`, `dstr_left/mid/right`, `dstr_from_mbs/to_mbs/from_wcs/to_wcs`, `dstr_to_upper/lower`, `astrcmpi(_n)`, `astrcmp_n`, `astrstri`, `strdepad`, `strlist_free/split`, `wcsdepad`, `wstrcmpi(_n)`, `wstrcmp_n`, `wstrstri`; `static inline` (layout-compatible only): `dstr_copy_dstr`, `dstr_end`, `dstr_init*`, `dstr_resize`, `dstr_find*`, `dstr_cmp*`. Done when: table tests including empty and overlapping inputs.
- [ ] **P1-02.3 Replace and format** — `dstr_replace`, `dstr_cat_strref`, the case-insensitive `astrcmpi`/`astrstri` helpers, `strdepad`, `strlist_split`, `dstr_depad`, and `dstr_safe_printf` (fixed 4 args — ports to Rust, no `va_list`). `dstr_printf`, `dstr_catf`, `dstr_vprintf`, `dstr_vcatf` are variadic/`va_list`: implement in the C shim (primer §5.5), not in Rust. Done when: matches C on 1000 generated cases.
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
  - Steps: `os_event_init/destroy/wait/timedwait/try/signal/reset` for auto and manual reset (`os_event_try` returns `EAGAIN` when not signalled — the audio thread loops on it: `while (os_event_try(audio->stop_event) == EAGAIN)` in `media-io/audio-io.c:205`).
  - Done when: signal-before-wait and timeout tests pass.
- [ ] **P1-05.2 Semaphores and thread names** — `os_sem_*`, `os_set_thread_name`. Done when: thread name visible in debugger on each OS.
- [ ] **P1-05.3 Windows parity** — compare behaviour with `threading-windows.c` (events/semaphores are `HANDLE`s from `CreateEvent`/`CreateSemaphore`, driven by `WaitForSingleObject`; no SRW locks). Done when: Windows CI green.
- [ ] **P1-05.4 Stress and recipe** — 64 threads × 1M signals; apply recipe.

### P1-06 Time and sleep
- [ ] **P1-06.1 Clock** — `os_gettime_ns` monotonic (`CLOCK_MONOTONIC` in `platform-nix.c`; `CLOCK_UPTIME_RAW` via `clock_gettime_nsec_np` in `platform-cocoa.m` on macOS — the nix copy is `#if !defined(__APPLE__)`; QPC in `platform-windows.c`). There is no `os_gettime_ms` in this tree. Done when: never decreases in a 10 s loop.
- [ ] **P1-06.2 Sleep** — copy the per-OS strategies verbatim: `os_sleepto_ns` (interruptible `nanosleep` loop on nix incl. macOS; `Sleep(ms-1)` + `QueryPerformanceCounter`/`YieldProcessor` spin on Windows, returns whether it stalled), `os_sleepto_ns_fast` (`usleep(≤999999)` loop on nix; `Sleep(remain_ms)` loop on Windows), `os_sleep_ms` (`usleep` on nix; `Sleep` with the Win8+ precision decrement on Windows). Done when: jitter histogram is attached.
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
- [ ] **P1-08.5 Apply recipe (added)** — for the ported lexers and text-lookup (`utf8` is replaced by `std`, no shim needed).

### P1-09 Config file
- [ ] **P1-09.1 Parser** — `libobs/util/config-file.c`: sections, `key=value`, comments, BOM handling. Done when: 10 real files parse.
- [ ] **P1-09.2 Typed API** — `config_get_string/int/uint/bool/double`, `set_*`, `set_default_*`, `has_user_value`. Done when: default vs user precedence tested.
- [ ] **P1-09.3 Save** — atomic write via temp file + rename, `.bak` behaviour. Done when: power-loss simulation (kill after temp write) leaves old file intact.
- [ ] **P1-09.4 Round trip** — byte-identical for corpus. Done when: golden.
- [ ] **P1-09.5 Apply recipe (added)**

### P1-10 Base64, CRC32, hashes
- [ ] **P1-10.1 base64 (none in libobs)** — there is no `base64.c`/`base64.h` in `libobs/`; the only base64 in the tree is the in-tree librtmp copy `plugins/obs-outputs/librtmp/cencode.c/.h` (Phase 5, P5-17.4 — it is part of obs-outputs, not `deps/`). Done when: absence re-verified by grep and the decision (skip) noted in ADR 0005.
- [ ] **P1-10.2 CRC32** — `crc32.c` (`calc_crc32`; only in-tree user: `plugins/text-freetype2/find-font-windows.c`); `crc32fast`. Done when: vectors.
- [ ] **P1-10.3 blake2** — port `deps/blake2` (blake2b) to the `blake2` crate with known-answer vectors; its only C callers are `frontend/updater/hash.cpp` (Windows updater, P6-32) and `frontend/utility/WhatsNewInfoThread.cpp` (what's-new feed, P6-23.2; linked via `frontend/cmake/{os-windows,feature-whatsnew,feature-macos-update}.cmake`), which keep the vendored copy in use until Phase 6 (the directory itself is deleted by P7-04) — no libobs caller exists. Done when: vectors pass.
- [ ] **P1-10.4 Apply recipe (added)** — crc32 only; `deps/blake2` stays in the build until P7-04 deletes it (see P1-10.3).

### P1-11 Process and module loading
- [ ] **P1-11.1 Dynamic libs** — `os_dlopen`, `os_dlsym`, `os_dlclose`, `os_dlopen` error strings. Done when: loading missing lib returns NULL + same message class.
- [ ] **P1-11.2 Processes and pipes** — `os_process_pipe_create/read/write/destroy` (`libobs/util/pipe*.c`), `os_get_executable_path_ptr`. Done when: echo-process roundtrip on 3 OSes.
- [ ] **P1-11.3 Edge cases** — paths with spaces, non-UTF-8 bytes, closed pipe. Done when: tests.
- [ ] **P1-11.4 Apply recipe (added)**

### P1-12 Profiler
- [ ] **P1-12.1 Core** — `libobs/util/profiler.c/.h`: names, call tree, time stats. Done when: tree structure equals C for a scripted run.
- [ ] **P1-12.2 Output** — `profiler_print`, `profiler_print_time_between_calls`, `profiler_snapshot_dump_csv`. Done when: text/CSV goldens.
- [ ] **P1-12.3 RAII + recipe** — scoped guard for Rust users; also `source-profiler.c`.

### P1-13 Logging
- [ ] **P1-13.1 `blog` (stays C)** — `libobs/util/base.c/.h`: `blog`, `blogva`, `bcrash` are variadic/`va_list` and the handlers (`base_set_log_handler`, `base_set_crash_handler`) receive a `va_list`; stable Rust cannot define or consume these. Keep `base.c` as a small C file in `obs-ffi/c/` and expose `obs_ffi_log(level, msg)` to Rust. Done when: handler receives identical strings and the primer §5.5 decision is referenced.
- [ ] **P1-13.2 Crash hook** — `bcrash`, `base_set_crash_handler` (same C shim). Done when: handler invoked in test.
- [ ] **P1-13.3 `log` facade** — Rust `log` records forwarded to `blog`. Done when: level mapping table tested.

### P1-14 Miscellaneous util
- [ ] **P1-14.1 Inventory** — table for the files P1-14.1 owns per the coverage matrix: `uthash.h`, `util.hpp`, `windows/` (COM helpers), with a keep/port/drop decision each (`curl/` is owned by P1-14.4). The other leftover util files already have owning tasks: `bitstream.*`/serializers → P1-14.2, `task.*`/`util_uint64.h`/`util_uint128.h` → P1-14.3, `sse-intrin.h` → P1-19.1, `source-profiler.*` → P1-12.3, `pipe.*` → P1-11.2, `apple/` → P1-07.1. Done when: every P1-14 file has a decision.
- [ ] **P1-14.2 Port serializers and bitstream** — existing `test/cmocka/test_bitstream.c` and `test_serializer.c` must pass against the Rust build. Done when: they do.
- [ ] **P1-14.3 Port task queue and uint128** — `os_task_queue`, 128-bit helpers. Done when: unit tests.
- [ ] **P1-14.4 Document leftovers** — curl helpers remain FFI until Phase 6. Done when: noted in ADR 0005.
- [ ] **P1-14.5 Apply recipe (added)** — for the ported serializers/bitstream and task/uint128 code (P1-14.2/14.3); the keep-C files need no recipe.

## B. `obs-math`

### P1-15 vec2/vec3/vec4
- [ ] **P1-15.1 Types** — `repr(C)` `Vec2/Vec3/Vec4` matching `libobs/graphics/vec*.h` (including the union with `ptr[4]`/SSE `m128`). Done when: size/align tests.
- [ ] **P1-15.2 Functions** — the non-inline exported `vec*_` functions: vec2 `abs/floor/ceil/close/norm`, vec3 `from_vec4/plane_dist/rotate/transform/transform3x4/mirror/mirrorv/rand`, vec4 `from_vec3/transform`; `add/sub/mul/div/dot/cross/norm...` are `static inline` in the headers and stay in C. Done when: list diff with the headers is empty.
- [ ] **P1-15.3 Goldens** — 10k seeded random cases from `obs-ref-dump`; tolerance 1e-6 relative. Done when: pass.
- [ ] **P1-15.4 Recipe**

### P1-16 matrix3/matrix4
- [ ] **P1-16.1 Basics** — identity, copy, multiply, transpose, determinant. Done when: golden.
- [ ] **P1-16.2 Inverse and transforms** — `matrix4_inv`, translate/rotate/scale, `matrix4_from_quat`, `matrix4_from_axisang`. Done when: inverse·M ≈ I within 1e-6 (S-008).
- [ ] **P1-16.3 matrix3** — conversions to/from `matrix4`, `matrix3_from_*`. Done when: golden.
- [ ] **P1-16.4 Benchmarks** — vs baseline. Done when: ≥ C speed.
- [ ] **P1-16.5 Apply recipe (added)**

### P1-17 quat, axisang, plane, bounds
- [ ] **P1-17.1 quat** — the non-inline exports: `quat_mul`, `quat_from_axisang`, `quat_from_matrix3/4`, `quat_get_dir`, `quat_set_look_dir`, `quat_log`, `quat_exp`, `quat_interpolate`, `quat_get_tangent`, `quat_interpolate_cubic` (`quat_set`/`quat_norm`/… are `static inline`). Done when: golden.
- [ ] **P1-17.2 axisang/plane** — the only axisang export is `axisang_from_quat` (`axisang_zero/copy/set` are `static inline`; there is no axisang→quat function — that direction is `quat_from_axisang`, P1-17.1); plane: `plane_from_tri`, `plane_transform(3x4)`, `plane_intersection_ray/line`, `plane_tri_inside/line_inside` (`plane_copy/set/setf/close/coplanar` are inline). Done when: golden.
- [ ] **P1-17.3 bounds** — the exported `bounds_move/scale/merge(2)/get_point/get_center/transform(3x4)/intersection_ray/line/plane_test/under_plane/intersects/intersects_obb(3x4)/min_dist` (`bounds_intersects_ray` is `static inline`, keep it). Done when: golden for 100 shapes.
- [ ] **P1-17.4 Recipe**

### P1-18 math-extra, half, srgb
- [ ] **P1-18.1 math-extra** — `libobs/graphics/math-extra.c`: `rand_float`, `polar_to_cart`, `cart_to_polar`, `polar_to_norm`, `norm_to_polar`, `calc_torque(f)`. Done when: deterministic functions match.
- [ ] **P1-18.2 half** — `half.h` conversions via `half` crate; special values. Done when: all 65536 f16 values round trip identically to C.
- [ ] **P1-18.3 sRGB** — `srgb.h` linear↔sRGB. Done when: 256-point golden within 1e-6.
- [ ] **P1-18.4 Apply recipe (added)**

### P1-19 SIMD
- [ ] **P1-19.1 Find intrinsics** — grep `sse-intrin.h`, `simde` use in math; list functions. Done when: list.
- [ ] **P1-19.2 Implement** — `std::arch` (x86_64 SSE2/AVX2 with runtime check) and NEON. Done when: scalar fallback results equal.
- [ ] **P1-19.3 Benchmark** — on x86_64 and aarch64. Done when: not slower than C.

## C. `obs-signal` (`libobs/callback/`)

### P1-20 calldata
- [ ] **P1-20.1 Storage** — `calldata.c/.h` byte stack with name/size/data packing and `fixed` mode. Done when: byte layout equals C.
- [ ] **P1-20.2 Exported API** — the only non-inline exports are `calldata_get_data`, `calldata_set_data`, `calldata_get_string`; the typed `calldata_get/set_int/float/bool/ptr` are `static inline` and rely on the byte layout. Done when: mixed-type test building calldata with C inline setters and reading with Rust (and vice versa).
- [ ] **P1-20.3 Fuzz + recipe** — random set/get sequences compared with C.

### P1-21 decl and proc
- [ ] **P1-21.1 Parser** — `decl.c`: `void name(in int x, out string y)`; error codes. Done when: 50 declarations plus invalid cases.
- [ ] **P1-21.2 Proc handler** — `proc.c`: add/remove/call, thread safety. Done when: concurrent add/call test.
- [ ] **P1-21.3 Golden** — parsed output dumps.
- [ ] **P1-21.4 Apply recipe (added)**

### P1-22 signal
- [ ] **P1-22.1 Handler** — `signal.c`: add signal by declaration, connect/disconnect. Done when: ordering equals C (callbacks run in connection order).
- [ ] **P1-22.2 Emit** — replicate `signal.c:290–351` (`signal_handler_signal`): take the per-signal **recursive** mutex for the whole emit, iterate by index (array may grow), skip entries with `remove == true`, then erase removed entries after the loop; `signal_handler_disconnect` during emit only sets `remove` (thread-local `current_signal_cb` handles "disconnect self"). Then run global callbacks under `global_callbacks_mutex`, and adjust `handler->refs` by the `keep_ref` removals. Done when: tests disconnect self, disconnect others, connect new, and emit the same signal recursively.
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
- [ ] **P1-25.4 Utility** — `apply`, `erase`, `clear`, `get_json` (incl. `obs_data_get_json_with_defaults`, `_pretty`, `_pretty_with_defaults`), `create_from_json(_file)`, `save_json`/`save_json_safe`. Done when: file ops atomic.
- [ ] **P1-25.5 Reference dump** — compare on the corpus. Done when: pass.
- [ ] **P1-25.6 Apply recipe (added)** — excludes `obs-data.c` (the jansson use of P1-24 leaves with it) when `OBS_ENABLE_RUST=ON`.

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
- [ ] **P1-27.6 Apply recipe (added)**

### P1-28 Hotkey data layer
- [ ] **P1-28.1 Name map** — `libobs/obs-hotkey-name-map.c`. Done when: all key names equal.
- [ ] **P1-28.2 Combo JSON** — `obs_hotkey_binding` save/load format is specified by S-034 (persistence section) and vectorized in `hotkeys/03-bindings-load-save.script`; the implementation belongs to P4-28.2. Done when: vectors verified against C `obs-hotkey.c` behaviour (no Phase-1 code).
- [ ] **P1-28.3 Golden**
- [ ] **P1-28.4 Apply recipe (added)** — for the name map (P1-28.1); P1-28.2 is vectors only.

## E. `shared/` leaf helpers

### P1-33 `shared/` leaf helpers (added)
- [ ] **P1-33.1 `shared/bpm`** — apply recipe for S-024 M8 (counters, timestamps, SEI payloads, per-codec packet injection). The AV1 `metadata_obu` framing is inlined from S-012 (formula given in S-024 §4.8) — no build dependency on the Phase 2 NAL tasks (P2-17…P2-20). Done when: the `bpm` scripts in `test/conformance/scripts/helpers/` pass against the C reference.
- [ ] **P1-33.2 `shared/file-updater`** — apply recipe for S-024 M10 (version comparison, fake-HTTP/in-memory-filesystem driven; the module has no hashes). Done when: M10 scripts pass.
- [ ] **P1-33.3 `shared/happy-eyeballs`** — apply recipe for S-024 M9 (address-list ordering, resolver hints, constants only — the socket race is out of scope and stays behind the C until a spec exists). Done when: M9 scripts pass.
- [ ] **P1-33.4 `shared/obs-inject-library`** — no spec (thin `LoadLibrary`/`dlopen` helper, 134 lines): port from the C, unit-test symbol resolution and the failure paths. Consumed only by `plugins/win-capture`: `game-capture.c` (P5-31.6) and `inject-helper/` (P5-31.7). Done when: crate passes its unit tests and the C file stays linked for P5-31.6/P5-31.7 (excluded only when win-capture is ported).
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
- [ ] **P1-31.1 Usage scan** — `grep -rn "blake2\|json11" --include=CMakeLists.txt --include=*.c* --include=*.cpp .`. Done when: the only remaining users are frontend — `deps/blake2`: `frontend/updater/hash.cpp`; `deps/json11`: `frontend/` OAuth/importer code — and nothing in `libobs/`.
- [ ] **P1-31.2 Deletion (deferred to P7-04)** — cannot delete in Phase 1: `frontend/updater` (P6-32) still links `deps/blake2` and the C frontend still links `deps/json11` until Phase 6 ports them. Record the deferral (with both consumers named) so P7-04 deletes them. Done when: deferral recorded in the P1-32 review notes.

### P1-32 Phase 1 review
- [ ] **P1-32.1 Benchmarks** vs baseline table.
- [ ] **P1-32.2 Goldens/ABI** on 3 OSes.
- [ ] **P1-32.3 Tick** parent tasks and update estimates.
