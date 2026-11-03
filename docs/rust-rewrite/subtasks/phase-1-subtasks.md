# Phase 1 subtasks

Parent file: [../phase-1-leaf-libraries.md](../phase-1-leaf-libraries.md).
**Clean-room specs written for this phase** (use them instead of reading the C): S-001 allocator/logging (P1-01, P1-13), S-002 calldata (P1-20), S-003 declaration language (P1-21), S-004 signals/procs (P1-22, P1-23), S-005 darray/deque (P1-03, P1-04), S-006 dstr (P1-02), S-007 config-file (P1-09), S-008 vec3/4/quat/matrix4/axisang (P1-15…P1-17), S-009 vec2/matrix3/plane/bounds/math-extra/half/sRGB (P1-15, P1-16, P1-17, P1-18; Draft — second-author check pending), S-010 serializers/bitstream/text-lookup (P1-08.4, P1-14.2), S-011 lexer/cf-parser (P1-08.2, P1-08.3), S-014 obs_data (P1-24, P1-25), S-015 obs_properties (P1-27), S-034 hotkeys (P1-28). No spec yet: threading primitives and time/sleep (P1-05, P1-06 — D-27, old backlog S-017; S-023 §5.8 specifies only the Linux property contract for P1-06); the rest of the phase is specified: P1-07 → S-023, P1-10.2 → S-055 (crc32), P1-11 → S-055 (pipes) + S-024 M3 (process args), P1-12 → S-024 M7, P1-14.3 → S-024 M2/M6, P1-24/P1-25 → S-014, P1-27 → S-015, P1-28 → S-034 (P1-10.3 blake2 is vector-only, no spec). Index: [../specs/README.md](../specs/README.md).
  Each subtask has **Files**, **Steps**, **Done when**. C line counts are measured in the read-only checkout `~/github/obs-studio` (libobs 33.0.1): `git ls-files <path> | xargs wc -l`.

**Recipe** (referred to as "apply recipe"): (1) safe Rust API in the crate; (2) `extern "C"` shim with the exact C signature in `obs-ffi`; (3) `cbindgen` output diffed against the existing header; (4) golden/unit/fuzz test passes against the C reference dump; (5) the C source is excluded when `OBS_ENABLE_RUST=ON`; (6) matrix file in `docs/tests/`. Every task that swaps a C source group ends with an "apply recipe" subtask; where the C source stays (P1-10.3 `deps/blake2`, P1-13 `base.c`, the P1-33 C files until their consumers are ported) the remaining recipe steps apply and the subtask says so.

Corrections to the earlier plan, verified in the tree: the ring buffer is `libobs/util/deque.h` (there is **no** `circlebuf_*` compatibility alias in this tree; `source-profiler.c` has an unrelated private `ucirclebuf`), and `libobs/util` also contains `bitstream.*`, `serializer.h`, `array-serializer.*`, `file-serializer.*`, `buffered-file-serializer.*`, `pipe*.c`, `task.*`, `platform-nix-dbus.c`, `platform-nix-portal.c`, `util_uint64.h`, `util_uint128.h`, `uthash.h`, `curl/` and `windows/`. These are covered by P1-07.3 (portal/dbus), P1-11.2 (pipes) and P1-14 (the rest).

## A. `obs-util` (`libobs/util/`)

### P1-01 Memory helpers
- [ ] **P1-01.1 Allocator wrappers**
  - Files: `libobs/util/bmem.c` (164), `bmem.h` (94)
  - Steps: port `bmalloc`, `brealloc`, `bfree`, `bzalloc`, `bmemdup`, `bstrdup` to `crates/obs-util` on the system allocator; 32-byte alignment (`base_get_alignment()` returns 32: `_aligned_malloc` on Windows, offset-header hack on POSIX, S-001-R01/R10).
  - Spec: S-001 (R01–R13)
  - Vectors: `test/conformance/scripts/bmem/`
  - Pitfall: S-001-R02/R07 — zero-size `bmalloc`/`brealloc` is **fatal** (`bcrash` with exact message); `bmalloc` never returns NULL.
  - Done when: unit tests for zero-size and null-free match C.
- [ ] **P1-01.2 Counter and hooks** — `bnum_allocs()` (`base_set_allocator` does not exist); atomic counter. Spec: `specs/S-001-bmem-base.md` (R04–R09). Vectors: `test/conformance/scripts/bmem/`. Pitfall: S-001 Q-1 — the counter is a plain `long` in C; the port MUST use an atomic. Done when: concurrent test (8 threads × 100k allocs) returns 0 at end.
- [ ] **P1-01.3 Apply recipe** — Files: `libobs/CMakeLists.txt` (396, exclude `bmem.c` when `OBS_ENABLE_RUST=ON`), `obs-ffi` shim. Compare with `test/cmocka` style leak check. Vectors: `test/conformance/scripts/bmem/`.

### P1-02 `dstr`
- [ ] **P1-02.1 Type and growth**
  - Files: `libobs/util/dstr.c` (787), `dstr.h` (320), `dstr.hpp` (46)
  - Steps: `#[repr(C)]` `struct dstr {char*, len, capacity}` in `crates/obs-util`; capacity policy identical (S-006 §4).
  - Spec: S-006 (R01–R05, §4–§9)
  - Vectors: `test/conformance/scripts/dstr/`
  - Pitfall: S-006 Q-2 — `from_mbs` leaves `capacity == 0` although memory is allocated.
  - Done when: layout test passes.
- [ ] **P1-02.2 Core ops** — the exported ops (41 in `dstr.h`): the `dstr_copy*`/`dstr_cat*`/`dstr_ncat*`/`dstr_insert*` families (incl. `_strref`/`_dstr` variants), `dstr_ncopy(_dstr)`, `dstr_init_copy_strref`, `dstr_remove`, `dstr_replace`, `dstr_left/mid/right`, `dstr_from_mbs/to_mbs/from_wcs/to_wcs`, `dstr_to_upper/lower`, `astrcmpi(_n)`, `astrcmp_n`, `astrstri`, `strdepad`, `strlist_free/split`, `wcsdepad`, `wstrcmpi(_n)`, `wstrcmp_n`, `wstrstri`; `static inline` (layout-compatible only): `dstr_copy_dstr`, `dstr_end`, `dstr_init*`, `dstr_resize`, `dstr_find*`, `dstr_cmp*`. Spec: S-006 §5–7. Vectors: `test/conformance/scripts/dstr/`. Pitfall: S-006 Q-1 — `ncopy`/`ncat`/`mid` read one byte past the promised `n`; the port SHOULD read exactly `n` (identical for valid input). Done when: table tests including empty and overlapping inputs.
- [ ] **P1-02.3 Replace and format** — `dstr_replace`, `dstr_cat_strref`, the case-insensitive `astrcmpi`/`astrstri` helpers, `strdepad`, `strlist_split`, `dstr_depad`, and `dstr_safe_printf` (fixed 4 args — ports to Rust, no `va_list`). `dstr_printf`, `dstr_catf`, `dstr_vprintf`, `dstr_vcatf` are variadic/`va_list`: implement in the C shim (primer §5.5), not in Rust. Pitfall: S-006 Q-6 — `replace` with an empty `find` never terminates in C; the port MUST treat it as a no-op. Done when: matches C on 1000 generated cases.
- [ ] **P1-02.4 Fuzz and recipe** — `cargo-fuzz` target applying random op sequences to Rust and C builds and comparing; apply recipe (Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim). Vectors: `test/conformance/scripts/dstr/`.

### P1-03 Dynamic arrays
- [ ] **P1-03.1 Layout** — Files: `libobs/util/darray.h` (606, header-only). `struct darray {void* array; size_t num; size_t capacity}`; `DArray<T>` with `repr(C)` in `crates/obs-util`. Spec: S-005 §3–4 (R-A01–R-A14). Vectors: `test/conformance/scripts/darray/`. Done when: `offset_of` test.
- [ ] **P1-03.2 C helpers** — functions behind `da_push_back`, `da_insert`, `da_erase`, `da_move_item`; macros in the header stay. Pitfall: S-005 Q-A2 — `da_find` `memcmp`s the whole element incl. padding; mirror byte-wise. Done when: `test/cmocka/test_darray.c` (31) passes unchanged.
- [ ] **P1-03.3 Recipe** — apply and add matrix in `docs/tests/`. Vectors: `test/conformance/scripts/darray/`.

### P1-04 Deque (`libobs/util/deque.h`)
- [ ] **P1-04.1 Data structure**
  - Files: `libobs/util/deque.h` (319, header-only)
  - Steps: `Deque` in `crates/obs-util` with fields `{data,size,start_pos,end_pos,capacity}`; implement `deque_push_back/front`, `pop_front/back`, `peek_front/back`, `place`, `push_back_zero`, `reserve`, `free`.
  - Spec: S-005 §5–6 (R-D01–R-D13)
  - Vectors: `test/conformance/scripts/deque/`
  - Pitfall: S-005 Q-D1 — `end_pos` may equal `capacity` (not 0); the struct is shared with C inline code.
  - Done when: wraparound property test equals the C result for random op sequences.
- [ ] **P1-04.2 Layout sharing** — the `struct deque` fields (`data,size,start_pos,end_pos,capacity`) are touched directly by C inline code; keep them identical (spec `S-005`, Q-D1: `end_pos` may equal `capacity`). Vectors: `test/conformance/scripts/deque/`. Done when: a C test drives a Rust-owned deque with C inline functions and gets the same dump as `deque` vectors.
- [ ] **P1-04.3 Recipe** — Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim. Vectors: `test/conformance/scripts/deque/`.

### P1-05 Threading primitives
- [ ] **P1-05.1 Events**
  - Files: `libobs/util/threading.h` (103), `threading-posix.c` (273), `threading-posix.h` (77), `threading-windows.c` (213), `threading-windows.h` (142)
  - Steps: `os_event_init/destroy/wait/timedwait/try/signal/reset` for auto and manual reset in `crates/obs-util` (`os_event_timedwait` returns `ETIMEDOUT` on timeout).
  - Spec: no clean-room spec (gap S-061, decision D-27) — port from C.
  - Pitfall: `os_event_try` returns `EAGAIN` when not signalled — the audio thread loops on it: `while (os_event_try(audio->stop_event) == EAGAIN)` in `media-io/audio-io.c` (455):205.
  - Done when: signal-before-wait and timeout tests pass.
- [ ] **P1-05.2 Semaphores and thread names** — `os_sem_init/post/wait/destroy`, `os_set_thread_name` from `threading-posix.c`. Spec: no spec (S-061 gap). Pitfall: `threading-posix.c` carries a separate `#ifdef __APPLE__` `os_sem_init` path (lines ~170–220; macOS lacks unnamed POSIX semaphores) — read it before porting, test macOS separately. Done when: thread name visible in debugger on each OS.
- [ ] **P1-05.3 Windows parity** — compare behaviour with `threading-windows.c` (213) (events/semaphores are `HANDLE`s from `CreateEvent`/`CreateSemaphore`, driven by `WaitForSingleObject`; no SRW locks). Done when: Windows CI green.
- [ ] **P1-05.4 Stress and recipe** — 64 threads × 1M signals; apply recipe (Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim).
- [ ] **P1-05.5 Write S-061 (added)** — threading-primitives spec (D-27)
  - Files: `libobs/util/threading.h` (103), `threading-posix.c` (273), `threading-windows.c` (213).
  - Steps: 1. read the C and enumerate the contract: `os_event_init/post/wait/destroy` (auto-reset), `os_sem_init/post/wait/destroy`, `os_set_thread_name`, the Windows `HANDLE` mapping and the pthread path. 2. write `specs/S-061-threading.md` (format like S-034). 3. add the row to `specs/README.md`.
  - Pitfalls: prerequisite of P1-05.1 (D-27: "write S-061 before starting P1-05"); `os_set_thread_name` (threading-windows.c:168) carries an `#ifdef NO_SEH_MINGW` dual `__try`/`__except` branch — resolve it in the spec.
  - Done when: S-061 committed and listed in `specs/README.md`.

### P1-06 Time and sleep
- [ ] **P1-06.1 Clock** — Files: `libobs/util/platform-nix.c` (1108), `platform-cocoa.m` (530), `platform-windows.c` (1374), `platform.h` (224). `os_gettime_ns` monotonic (`CLOCK_MONOTONIC` in `platform-nix.c`; `CLOCK_UPTIME_RAW` via `clock_gettime_nsec_np` in `platform-cocoa.m` on macOS — the nix copy is `#if !defined(__APPLE__)`; QPC in `platform-windows.c`). There is no `os_gettime_ms` in this tree. Spec: S-023 R100 (property contract, Linux); Windows/macOS strategy unspecified — port from C. Vectors: `test/conformance/scripts/platform/` (`monotonic`, `nstime`). Done when: never decreases in a 10 s loop.
- [ ] **P1-06.2 Sleep** — copy the per-OS strategies verbatim: `os_sleepto_ns` (interruptible `nanosleep` loop on nix incl. macOS; `Sleep(ms-1)` + `QueryPerformanceCounter`/`YieldProcessor` spin on Windows, returns whether it stalled), `os_sleepto_ns_fast` (`usleep(≤999999)` loop on nix; `Sleep(remain_ms)` loop on Windows), `os_sleep_ms` (`usleep` on nix; `Sleep` with the Win8+ precision decrement on Windows). Spec: S-023 R101–R102 (contract only). Pitfall: S-023 Q-16 — the reference computes `ms*1000` in 32 bits for `os_sleep_ms`; a port MAY sleep the full time. Done when: jitter histogram is attached.
- [ ] **P1-06.3 Compare and recipe** — jitter ≤ C's p99. Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim. Vectors: `test/conformance/scripts/platform/` (`sleep_ms`, `sleepto`, `sleepto_fast`).

### P1-07 File system utilities
- [ ] **P1-07.1 Open/rename/mkdir**
  - Files: `libobs/util/platform.c` (846), `platform-nix.c` (1108), `platform-windows.c` (1374), `platform-cocoa.m` (530), `platform.h` (224)
  - Steps: `os_fopen`, `os_wfopen`, `os_mkdir`, `os_mkdirs`, `os_rename`, `os_copyfile`, `os_unlink` in `crates/obs-util` (UTF-8↔UTF-16 for every Windows call).
  - Spec: S-023 §5.5–5.6 (Linux; Windows/macOS from C)
  - Vectors: `test/conformance/scripts/platform/`
  - Pitfall: S-023 D-1 — `os_utf8_to_wcs` writes the NUL one unit past `dst` when the text exactly fills it; MUST NOT write outside `dst`. Q-9 — `os_safe_replace` has no rollback when the final rename fails.
  - Done when: tests with unicode and long (>260) paths on Windows.
- [ ] **P1-07.2 Directory iteration** — `os_opendir/readdir/closedir`, `os_glob`/`os_globfree`. Spec: S-023 §5.6. Vectors: `test/conformance/scripts/platform/` (`ls`, `glob`). Pitfall: S-023 D-5 — the reference keeps the caller's `path` pointer (UAF on a temporary); the port MUST copy it. Done when: sorted result equals C.
- [ ] **P1-07.3 Config/data paths** — Files: `platform-nix-dbus.c` (166), `platform-nix-portal.c` (223) (Flatpak). `os_get_config_path`, `os_get_program_data_path`, `os_get_abs_path(_ptr)`, `os_get_free_disk_space` + portal/dbus helpers. Spec: S-023 §5.6 (paths), S-055 U4/U5 (dbus/portal sleep inhibition). Vectors: `test/conformance/scripts/platform/` (+ `utilrest/` for dbus). Pitfall: S-023 Q-11 — `os_get_config_path_ptr(NULL or "")` ends with `/`, the buffer form does not. Done when: Flatpak path test.
- [ ] **P1-07.4 Recipe and matrix** — Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim, `docs/tests/` matrix. Vectors: `test/conformance/scripts/platform/`.

### P1-08 Text encoding and lexers
- [ ] **P1-08.1 UTF-8/wide helpers** — Files: `libobs/util/utf8.c` (366), `utf8.h` (35). `utf8_to_wchar`, `wchar_to_utf8`, `os_utf8_to_wcs*`, `os_wcs_to_utf8*` using `std` (locale `os_mbs_*` stay in the C shim). Spec: S-023 §4.1, §5.1–5.3. Vectors: `test/conformance/scripts/platform/`. Pitfall: S-023 Q-1 — the reference accepts overlong and 5/6-byte forms and values > U+10FFFF (rejects only lead bytes `C0 C1 F5 FF` and surrogates); MUST match. Done when: invalid-sequence behaviour documented and equal.
- [ ] **P1-08.2 `lexer.c`** — Files: `libobs/util/lexer.c` (305), `lexer.h` (273). `lexer_getbasetoken`, strref helpers (`strref_cmp*`, `is_empty*`, `valid_int_str`/`valid_float_str`) in `crates/obs-util`. Spec: S-011 §5.1. Vectors: `test/conformance/scripts/lexer/`. Pitfall: S-011 Q-2 — `valid_float_str` rejects `1e+5`/`1e-5` but accepts `1e5+3`. Done when: token golden for 5 sample files.
- [ ] **P1-08.3 `cf-lexer.c` / `cf-parser.c`** — Files: `libobs/util/cf-lexer.c` (1376), `cf-lexer.h` (199), `cf-parser.c` (60), `cf-parser.h` (281). Preprocessor tokens, error list, parser helpers. Spec: S-011 §5.2–5.5. Vectors: `test/conformance/scripts/lexer/`. Pitfall: S-011 Q-5 — STRING span is one byte too long (reproduce); S-011 D-7 — recursive macros recurse to stack overflow in C; MUST terminate. Done when: golden dump for every `*.effect` under `libobs/data/` (22 files).
- [ ] **P1-08.4 `text-lookup.c`** — Files: `libobs/util/text-lookup.c` (271), `text-lookup.h` (45). Locale `.ini` lookup with fallback. Spec: S-010 §5.7. Vectors: `test/conformance/scripts/serial/` (text-lookup slots). Pitfall: S-010 Q-6 — `A==b` stores `A -> "="`, `=x` stores `"=" -> "x"`; S-010 D-1 — a lone `"` at EOF must not crash (treat like `B=""`). Done when: loads `frontend/data/locale/en-US.ini` (1 of 77) and gets identical values.
- [ ] **P1-08.5 Apply recipe (added)** — for the ported lexers and text-lookup (`utf8` is replaced by `std`, no shim needed). Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim. Vectors: `test/conformance/scripts/lexer/`, `serial/`.

### P1-09 Config file
- [ ] **P1-09.1 Parser** — Files: `libobs/util/config-file.c` (789), `config-file.h` (103). `config_open(_string/_defaults)`: sections, `key=value`, comments, BOM handling in `crates/obs-util`. Spec: S-007 §4.1. Vectors: `test/conformance/scripts/config/`. Pitfall: S-007 Q-1 — duplicate section/item names are both kept, lookup returns the last inserted; Q-5 — a header missing `]` still creates a section. Done when: 10 real files parse.
- [ ] **P1-09.2 Typed API** — `config_get_string/int/uint/bool/double`, `set_*`, `set_default_*`, `has_user_value`. Spec: S-007 §5. Pitfall: S-007 Q-3 — doubles are stored with `%g` formatting (`0.1` stays `0.1`). Done when: default vs user precedence tested.
- [ ] **P1-09.3 Save** — atomic write via temp file + rename, `.bak` behaviour (`config_save_safe` → `os_safe_replace`, port it together). Spec: S-007 §4.2 + S-023 R61. Pitfall: S-023 D-2 — the reference's `os_dtostr` leaves stale tail bytes for exponent values (corrupts config files, e.g. `1e-5` → `…e-55`); the port MUST make text and return value consistent. Done when: power-loss simulation (kill after temp write) leaves old file intact.
- [ ] **P1-09.4 Round trip** — byte-identical for corpus. Vectors: `test/conformance/scripts/config/`. Done when: golden.
- [ ] **P1-09.5 Apply recipe (added)** — Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim. Vectors: `test/conformance/scripts/config/`.

### P1-10 Base64, CRC32, hashes
- [ ] **P1-10.1 base64 (none in libobs)** — there is no `base64.c`/`base64.h` in `libobs/`; the only base64 in the tree is the in-tree librtmp copy `plugins/obs-outputs/librtmp/cencode.c` (111)/`cencode.h` (31) (Phase 5, P5-17.4 — it is part of obs-outputs, not `deps/`). Done when: absence re-verified by grep and the decision (skip) noted in ADR 0005.
- [ ] **P1-10.2 CRC32** — Files: `libobs/util/crc32.c` (64), `crc32.h` (29). `calc_crc32` → `crates/obs-util` via `crc32fast`; only in-tree user: `plugins/text-freetype2/find-font-windows.c` (261). Spec: S-055 U1. Vectors: `test/conformance/scripts/utilrest/`. Done when: vectors.
- [ ] **P1-10.3 blake2** — Files: `deps/blake2/src/blake2b-ref.c`, `blake2.h`, `blake2-impl.h`. Port `deps/blake2` (blake2b) to the `blake2` crate with known-answer vectors; its only C callers are `frontend/updater/hash.cpp` (81; Windows updater, P6-32) and `frontend/utility/WhatsNewInfoThread.cpp` (297; what's-new feed, P6-23.2; linked via `frontend/cmake/{os-windows,feature-whatsnew,feature-macos-update}.cmake`), which keep the vendored copy in use until Phase 6 (the directory itself is deleted by P7-04) — no libobs caller exists. Done when: vectors pass.
- [ ] **P1-10.4 Apply recipe (added)** — crc32 only (Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim); `deps/blake2` stays in the build until P7-04 deletes it (see P1-10.3). Vectors: `test/conformance/scripts/utilrest/`.

### P1-11 Process and module loading
- [ ] **P1-11.1 Dynamic libs** — Files: `libobs/util/platform.c` (846: `os_dlopen/dlsym/dlclose`), `platform.h` (224). `os_dlopen` (append `.so` if no `.so` substring, `RTLD_NOW`, exact error log), `os_dlsym`, `os_dlclose` in `crates/obs-util`. Spec: S-023 R107. Vectors: `test/conformance/scripts/platform/` (`dl`). Done when: loading missing lib returns NULL + same message class.
- [ ] **P1-11.2 Processes and pipes** — Files: `libobs/util/pipe.c` (75), `pipe.h` (51), `pipe-posix.c` (207), `pipe-windows.c` (279). `os_process_pipe_create/read/write/destroy`, `os_get_executable_path_ptr` in `crates/obs-util` (arg-list container is S-024 M3). Spec: S-055 U2 (pipes) + S-024 M3 (args). Vectors: `test/conformance/scripts/utilrest/` (+ `helpers/` `args-*`). Pitfall: S-055 Q-1 — pipe writes are buffered stdio, delivered at latest on `destroy`; S-055 D-1 — `destroy` returns the pid (not an exit status) for a signalled child. Done when: echo-process roundtrip on 3 OSes.
- [ ] **P1-11.3 Edge cases** — paths with spaces, non-UTF-8 bytes, closed pipe. Vectors: `test/conformance/scripts/utilrest/`. Done when: tests.
- [ ] **P1-11.4 Apply recipe (added)** — Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim. Vectors: `test/conformance/scripts/utilrest/`.

### P1-12 Profiler
- [ ] **P1-12.1 Core** — Files: `libobs/util/profiler.c` (1117), `profiler.h` (97), `profiler.hpp` (47). Names, call tree, time stats in `crates/obs-util` (S-024 M7). Spec: S-024 §4.7/§5 M7 (R35–R49). Vectors: `test/conformance/scripts/helpers/` (`prof-*`). Pitfall: S-024 M7 Q-8 — a root call completing while stopped disables the calling thread until `reenable`; Q-10 — the median quirk (R44). Done when: tree structure equals C for a scripted run.
- [ ] **P1-12.2 Output** — `profiler_print`, `profiler_print_time_between_calls`, `profiler_snapshot_dump_csv` (byte-exact per R42/R45/R47). Vectors: `test/conformance/scripts/helpers/` (report lines compared byte for byte). Done when: text/CSV goldens.
- [ ] **P1-12.3 RAII + recipe** — Files: `libobs/util/source-profiler.c` (630), `source-profiler.h` (66). Scoped guard for Rust users; port `source-profiler.c` (S-055 U3). Vectors: `test/conformance/scripts/utilrest/`. Pitfall: S-055 Q-7 — the history lags one frame behind and a zero tick means "no data"; Q-2 — `fill_result` returns false with a zeroed struct for unknown sources. Done when: goldens pass and recipe applied.

### P1-13 Logging
- [ ] **P1-13.1 `blog` (stays C)** — Files: `libobs/util/base.c` (126), `base.h` (97). `blog`, `blogva`, `bcrash` are variadic/`va_list` and the handlers (`base_set_log_handler`, `base_set_crash_handler`) receive a `va_list`; stable Rust cannot define or consume these. Keep `base.c` as a small C file in `obs-ffi/c/` and expose `obs_ffi_log(level, msg)` to Rust. Spec: S-001 §4.2 (R20–R25). Vectors: `test/conformance/scripts/bmem/` (`log`, `fatal`). Pitfall: S-001 Q-3 — the default handler silently drops levels outside the four constants; R25 — a crash handler can be installed at most once per process. Done when: handler receives identical strings and the primer §5.5 decision is referenced.
- [ ] **P1-13.2 Crash hook** — `bcrash`, `base_set_crash_handler` (same C shim). Spec: S-001 R24. Pitfall: S-001 R24 — a `bcrash` inside a crash handler writes "Crashed in the crash handler" and exits 2; `bcrash` never returns. Done when: handler invoked in test.
- [ ] **P1-13.3 `log` facade** — Rust `log` records forwarded to `blog`. Done when: level mapping table tested.

### P1-14 Miscellaneous util
- [ ] **P1-14.1 Inventory** — Files: `libobs/util/uthash.h` (34), `util.hpp` (155), `windows/` (12 files: `ComPtr.hpp` 186, `CoTaskMemPtr.hpp` 52, `HRError.hpp` 24, `WinHandle.hpp` 81, `device-enum.c` 30, `device-enum.h` 14, `obfuscate.c` 38, `obfuscate.h` 16, `window-helpers.c` 575, `window-helpers.h` 47, `win-registry.h` 37, `win-version.h` 57). Table per the coverage matrix with a keep/port/drop decision each (`uthash.h` → Rust collections; `curl/` (35) is owned by P1-14.4). The other leftover util files already have owning tasks: `bitstream.*`/serializers → P1-14.2, `task.*`/`util_uint64.h`/`util_uint128.h` → P1-14.3, `sse-intrin.h` → P1-19.1, `source-profiler.*` → P1-12.3, `pipe.*` → P1-11.2, `apple/` → P1-07.1. Done when: every P1-14 file has a decision.
- [ ] **P1-14.2 Port serializers and bitstream** — Files: `libobs/util/bitstream.c` (51)/`bitstream.h` (28), `serializer.h` (158), `array-serializer.c` (93)/`.h` (37), `file-serializer.c` (183)/`.h` (34), `buffered-file-serializer.c` (394)/`.h` (32). Existing `test/cmocka/test_bitstream.c` (36) and `test_serializer.c` (36) must pass against the Rust build. Spec: S-010. Vectors: `test/conformance/scripts/serial/`. Pitfall: S-010 Q-3 — `bitstream_reader.pos` is 8 bits (wraps to byte 0 after byte 255 for `len > 255`); Q-8 — three different SEEK_END conventions (array/file/buffered). Done when: they do.
- [ ] **P1-14.3 Port task queue and uint128** — Files: `libobs/util/task.c` (164)/`task.h` (22), `util_uint64.h` (34), `util_uint128.h` (108). `os_task_queue` (S-024 M6), `mul_div_u64`/u128 helpers (S-024 M2) → `crates/obs-util-small`. Vectors: `test/conformance/scripts/helpers/` (`u128-*`, `muldiv64*`, `task-*`). Done when: unit tests.
- [ ] **P1-14.4 Document leftovers** — Files: `libobs/util/curl/curl-helper.h` (35). Curl helpers remain FFI until Phase 6. Done when: noted in ADR 0005.
- [ ] **P1-14.5 Apply recipe (added)** — for the ported serializers/bitstream and task/uint128 code (P1-14.2/14.3); the keep-C files need no recipe. Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim. Vectors: `test/conformance/scripts/serial/`, `helpers/`.

## B. `obs-math`

### P1-15 vec2/vec3/vec4
- [ ] **P1-15.1 Types** — Files: `libobs/graphics/vec2.h` (148), `vec3.h` (224), `vec3.c` (87), `vec4.h` (241), `vec4.c` (41), `math-defs.h` (45), `basemath.hpp` (3). `repr(C, align(16))` `Vec2/Vec3/Vec4` in `crates/obs-math` (C union with `ptr[4]`/SSE `m128` = four f32 lanes). Spec: S-008 §3 (layouts), S-009 §3 (vec2). Vectors: `test/conformance/scripts/math/` + `math2/`. Pitfall: S-008 Q-1 — vec3 `dot`/`mul`/`mulf` include the `w` lane. Done when: size/align tests.
- [ ] **P1-15.2 Functions** — the non-inline exported `vec*_` functions: vec2 `abs/floor/ceil/close/norm` (S-009 §5.2), vec3 `from_vec4/plane_dist/rotate/transform/transform3x4/mirror/mirrorv/rand`, vec4 `from_vec3/transform`; `add/sub/mul/div/dot/cross/norm...` are `static inline` in the headers and stay in C. Vectors: `test/conformance/scripts/math/` + `math2/`. Pitfall: S-009 Q-1 — `vec2_norm` leaves the destination unwritten for a zero vector; S-008 Q-6 — `vec3_rand` uses libc `rand()` (not specified). Done when: list diff with the headers is empty.
- [ ] **P1-15.3 Goldens** — 10k seeded random cases from `obs-ref-dump`; tolerance 1e-6 relative. Vectors: `test/conformance/scripts/math/` + `math2/`. Done when: pass.
- [ ] **P1-15.4 Recipe** — Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim.

### P1-16 matrix3/matrix4
- [ ] **P1-16.1 Basics** — Files: `libobs/graphics/matrix3.h` (98), `matrix3.c` (138), `matrix4.h` (102), `matrix4.c` (333). Identity, copy, multiply, transpose, determinant in `crates/obs-math` (S-008 R30–R32). Spec: S-008 §6. Vectors: `test/conformance/scripts/math/`. Pitfall: S-008 R33 — `matrix4_inv` returns false and leaves `d` unmodified for |det| < 0.0005. Done when: golden.
- [ ] **P1-16.2 Inverse and transforms** — `matrix4_inv`, translate/rotate/scale (+ `_i` inverse-order variants, S-008 R35–R36), `matrix4_from_quat`, `matrix4_from_axisang` (R34). Done when: inverse·M ≈ I within 1e-6 (S-008).
- [ ] **P1-16.3 matrix3** — conversions to/from `matrix4`, `matrix3_from_*` (S-009 §5.4: 3x4, rows `x,y,z,t`, each a 4-lane vec3). Vectors: `test/conformance/scripts/math2/`. Pitfall: S-009 Q-12 — `matrix3_mul` dots rows with rows and subtracts the translation; `matrix3_inv` is the ordinary 4x4 inverse and does not invert that product. Done when: golden.
- [ ] **P1-16.4 Benchmarks** — vs baseline (`docs/rust-rewrite/benchmarks.md`). Done when: ≥ C speed.
- [ ] **P1-16.5 Apply recipe (added)** — Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim. Vectors: `test/conformance/scripts/math/` + `math2/`.

### P1-17 quat, axisang, plane, bounds
- [ ] **P1-17.1 quat** — Files: `libobs/graphics/quat.h` (170), `quat.c` (215), `axisang.h` (65), `axisang.c` (38). The non-inline exports: `quat_mul`, `quat_from_axisang`, `quat_from_matrix3/4`, `quat_get_dir`, `quat_set_look_dir`, `quat_log`, `quat_exp`, `quat_interpolate`, `quat_get_tangent`, `quat_interpolate_cubic` (`quat_set`/`quat_norm`/… are `static inline`); the only axisang export is `axisang_from_quat` (there is no axisang→quat function — that direction is `quat_from_axisang`). Spec: S-008 §5 (R10–R21). Vectors: `test/conformance/scripts/math/`. Pitfall: S-008 D-1/D-2 — `quat_set` args are reversed and `quat_inv` leaves `w` untouched (ABI-visible, reproduce); Q-4 — `quat_interpolate` does not clamp. Done when: golden.
- [ ] **P1-17.2 axisang/plane** — Files: `libobs/graphics/plane.h` (85), `plane.c` (151). Plane: `plane_from_tri`, `plane_transform(3x4)`, `plane_intersection_ray/line`, `plane_tri_inside/line_inside` (`plane_copy/set/setf/close/coplanar` are inline) (S-009 §5.5). Vectors: `test/conformance/scripts/math2/`. Pitfall: S-009 Q-5 — `plane_tri_inside`/`plane_line_inside` are true when any vertex is ≥ `prec` away (misnomer); Q-4 — `plane_transform` shifts the normal by the translation (the 3x4 variant never changes `dist`). Done when: golden.
- [ ] **P1-17.3 bounds** — Files: `libobs/graphics/bounds.h` (108), `bounds.c` (314). The exported `bounds_move/scale/merge(2)/get_point/get_center/transform(3x4)/intersection_ray/line/plane_test/under_plane/intersects/intersects_obb(3x4)/min_dist` (`bounds_intersects_ray` is `static inline`, keep it) (S-009 §5.6). Vectors: `test/conformance/scripts/math2/`. Pitfall: S-009 Q-7 — `bounds_get_point` accepts index 8 (acts like 6); D-2 — `bounds_plane_test` folds 1/2/3 to `true`; Q-13 — negative `bounds_scale` leaves `min > max`. Done when: golden for 100 shapes.
- [ ] **P1-17.4 Recipe** — Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim.

### P1-18 math-extra, half, srgb
- [ ] **P1-18.1 math-extra** — Files: `libobs/graphics/math-extra.c` (131), `math-extra.h` (61). `rand_float`, `polar_to_cart`, `cart_to_polar`, `polar_to_norm`, `norm_to_polar`, `calc_torque(f)` (S-009 §5.7). Vectors: `test/conformance/scripts/math2/`. Pitfall: S-009 Q-8 — `calc_torque` snaps to v2 (signed `vec3_close`, v1 <= v2 componentwise); D-3 — `polar_to_cart`/`cart_to_polar` leave `w` uninitialised (write 0). Done when: deterministic functions match.
- [ ] **P1-18.2 half** — Files: `libobs/graphics/half.h` (100). Conversions via `half` crate in `crates/obs-math` (S-009 §5.8); special values. Vectors: `test/conformance/scripts/math2/`. Pitfall: S-009 Q-10 — overflow to inf for any magnitude > 65504; Q-11 — the subnormal path truncates before rounding; D-6 — shift count ≥ 32 is UB in C, the port returns signed zero. Done when: all 65536 f16 values round trip identically to C.
- [ ] **P1-18.3 sRGB** — Files: `libobs/graphics/srgb.h` (177). Linear↔sRGB (`srgb_linear_to_nonlinear`, `srgb_nonlinear_to_linear`, u8/float helpers, S-009 §5.9). Vectors: `test/conformance/scripts/math2/`. Pitfall: S-009 Q-14 — `gs_float_to_u8` is not clamped (defined range only). Done when: 256-point golden within 1e-6.
- [ ] **P1-18.4 Apply recipe (added)** — Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim.

### P1-19 SIMD
- [ ] **P1-19.1 Find intrinsics** — Files: `libobs/util/sse-intrin.h` (41). Grep `sse-intrin.h`, `simde` use in `libobs/graphics/*.h`; list functions. Spec: no spec — port from C. Done when: list.
- [ ] **P1-19.2 Implement** — `std::arch` (x86_64 SSE2/AVX2 with runtime check) and NEON. Done when: scalar fallback results equal.
- [ ] **P1-19.3 Benchmark** — on x86_64 and aarch64. Done when: not slower than C.

## C. `obs-signal` (`libobs/callback/`)

### P1-20 calldata
- [ ] **P1-20.1 Storage** — Files: `libobs/callback/calldata.c` (238), `calldata.h` (195). Byte stack `[size_t name_len][name+NUL][size_t data_size][data]…` with zero-`size_t` terminator and `fixed` mode, in `crates/obs-signal` (S-002 §4). Spec: S-002. Vectors: `test/conformance/scripts/calldata/`. Pitfall: S-002 Q-4 — `new_size == capacity` counts as overflow (fixed) / grows (growable); Q-5 — growth leaves unwritten bytes beyond `size`. Done when: byte layout equals C.
- [ ] **P1-20.2 Exported API** — the only non-inline exports are `calldata_get_data`, `calldata_set_data`, `calldata_get_string`; the typed `calldata_get/set_int/float/bool/ptr` are `static inline` and rely on the byte layout. Pitfall: S-002 Q-1 — a read succeeds whenever the stored byte count equals the requested size, even if written as a different type; Q-3 — `get_string` succeeds on non-string records. Done when: mixed-type test building calldata with C inline setters and reading with Rust (and vice versa).
- [ ] **P1-20.3 Fuzz + recipe** — random set/get sequences compared with C. Vectors: `test/conformance/scripts/calldata/`.

### P1-21 decl and proc
- [ ] **P1-21.1 Parser** — Files: `libobs/callback/decl.c` (231), `decl.h` (61). `parse_decl_string` for `void name(in int x, out string y)`; error codes, in `crates/obs-signal`. Spec: S-003. Vectors: `test/conformance/scripts/decl/`. Pitfall: S-003 Q-1–Q-4 — leading whitespace fails, trailing junk after `)` is ignored, a stray token after a parameter stops silently, trailing comma is OK; D-1 — a non-NAME function name crashes the reference (MUST NOT crash, return false + log). Done when: 50 declarations plus invalid cases.
- [ ] **P1-21.2 Proc handler** — Files: `libobs/callback/proc.c` (128), `proc.h` (52). Add/remove/call, thread safety (S-004 §6). Vectors: `test/conformance/scripts/signal/` (`proc_add`/`proc_call` commands). Done when: concurrent add/call test.
- [ ] **P1-21.3 Golden** — parsed output dumps. Vectors: `test/conformance/scripts/decl/`.
- [ ] **P1-21.4 Apply recipe (added)** — Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim.

### P1-22 signal
- [ ] **P1-22.1 Handler** — Files: `libobs/callback/signal.c` (391), `signal.h` (73). Add signal by declaration, connect/disconnect in `crates/obs-signal` (S-004 §5.1–5.4). Spec: S-004. Vectors: `test/conformance/scripts/signal/`. Done when: ordering equals C (callbacks run in connection order).
- [ ] **P1-22.2 Emit** — replicate `signal.c:290–351` (`signal_handler_signal`): take the per-signal **recursive** mutex for the whole emit, iterate by index (array may grow), skip entries with `remove == true`, then erase removed entries after the loop; `signal_handler_disconnect` during emit only sets `remove` (thread-local `current_signal_cb` handles "disconnect self"). Then run global callbacks under `global_callbacks_mutex`, and adjust `handler->refs` by the `keep_ref` removals. Pitfall: S-004 Q-1 — entries connected during an emit run in the same emit; Q-4 — `disconnect` then `connect` of the same pair within one emit loses the connection. Done when: tests disconnect self, disconnect others, connect new, and emit the same signal recursively.
- [ ] **P1-22.3 Threads** — keep the lock structure (per-signal recursive mutex, handler mutex, global-callbacks recursive mutex); document any deviation. Pitfall: S-004 Q-2 — `remove_current` is cleared (not restored) after any callback returns, incl. nested emits. Done when: no deadlock under `loom` for connect-from-other-thread during emit.
- [ ] **P1-22.4 Recipe** — Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim. Vectors: `test/conformance/scripts/signal/`.

### P1-23 Typed Rust API
- [ ] **P1-23.1 Design** — Files: `crates/obs-signal/` (new `Signal<A>` + docs). `Signal<A>` + `Connection` guard in docs. Done when: ADR addendum.
- [ ] **P1-23.2 Bridge** — typed args to/from `calldata` (S-002 byte layout). Done when: round trip test.
- [ ] **P1-23.3 Docs/tests** — Files: `crates/obs-signal/` docs + tests.

## D. `obs-data`

### P1-24 JSON engine
- [ ] **P1-24.1 Library choice** — Files: `libobs/obs-data.c` (2181; jansson usage, system jansson 2.15 per `libobs/CMakeLists.txt` (396)), `obs-data.h` (314). `serde_json` with `preserve_order`; compare to jansson usage in `libobs/obs-data.c`. Spec: S-014 §6 (JSON algorithms). Vectors: `test/conformance/scripts/obsdata/`. Pitfall: S-014 Q5 — NaN/inf doubles and invalid UTF-8 strings/keys are silently dropped from the JSON text. Done when: note on differences (number formatting, escapes).
- [ ] **P1-24.2 Formatting** — indentation and float output equal to C (`%.17g` doubles compared as exact strings); verify from corpus. Vectors: `test/conformance/scripts/obsdata/` (`json*` commands). Done when: diff empty.
- [ ] **P1-24.3 Corpus round trip** — all `test/fixtures` collections byte-identical. Vectors: `test/conformance/scripts/obsdata/`.

### P1-25 `obs_data_t` core
- [ ] **P1-25.1 Object** — Files: `libobs/obs-data.c` (2181), `obs-data.h` (314). Ref-counted item map, ordered, in `crates/obs-data` (S-014 §4–5.1). Spec: S-014. Vectors: `test/conformance/scripts/obsdata/`. Pitfall: S-014 Q1 — growth moves an item to the end (iteration-order quirk); D4 — item handles MUST survive growth (the C reference dangles them). Done when: iteration order equals C.
- [ ] **P1-25.2 Typed API** — string/int/double/bool/obj/array, `*_default`, `*_autoselect` (S-014 §5.2–5.3). Pitfall: S-014 D1 — `set_default_obj/array` on an existing item without a user layer undercounts the refcount in C; the port MUST always take the reference. Done when: all accessors in `obs-data.h` (314) implemented.
- [ ] **P1-25.3 Arrays and items** — `obs_data_array_*`, `obs_data_item_*` iteration and mutation during iteration (S-014 §5.8–5.9). Vectors: `test/conformance/scripts/obsdata/`. Done when: tests from existing behaviour.
- [ ] **P1-25.4 Utility** — `apply`, `erase`, `clear`, `get_json` (incl. `obs_data_get_json_with_defaults`, `_pretty`, `_pretty_with_defaults`), `create_from_json(_file)`, `save_json`/`save_json_safe` (S-014 §5.5–5.7 + platform helpers §3.4). Pitfall: S-014 Q6 — the safe loader consumes the backup even when the primary merely does not exist. Done when: file ops atomic.
- [ ] **P1-25.5 Reference dump** — compare on the corpus. Vectors: `test/conformance/scripts/obsdata/`. Done when: pass.
- [ ] **P1-25.6 Apply recipe (added)** — Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim. Excludes `obs-data.c` (the jansson use of P1-24 leaves with it) when `OBS_ENABLE_RUST=ON`.

### P1-26 Ownership
- [ ] **P1-26.1 Semantics tests** — Files: `libobs/obs-data.c` (2181). `get_obj` returns new ref, setters take their own ref (S-014 §5.1). Vectors: `test/conformance/scripts/obsdata/` (`refs`/`a_refs` commands). Done when: refcount observed through debug API equal to C.
- [ ] **P1-26.2 Miri/ASan** — run on pure crates. Done when: clean.
- [ ] **P1-26.3 Fix findings**

### P1-27 `obs_properties`
- [ ] **P1-27.1 Container** — Files: `libobs/obs-properties.c` (1415), `obs-properties.h` (364). Ordering, `obs_properties_apply_settings`, flags, parent/group in `crates/obs-data`. Spec: S-015. Vectors: `test/conformance/scripts/props/`. Pitfall: S-015 Q-1 — `add_group` checks only direct children for duplicates; Q-2 — a cycle-forming `add_group` returns NULL silently. Done when: ordering test.
- [ ] **P1-27.2 Simple kinds** — bool, int, float, text (+types), path (+filters), button (S-015 §5). Vectors: `test/conformance/scripts/props/`. Done when: each reachable via ABI test.
- [ ] **P1-27.3 Complex kinds** — list (string/int/float, disabled items), editable list, font, color, frame rate (ranges/options), group. Pitfall: S-015 Q-3 — `list_add_*` on a wrong format returns 0 (not an error). Done when: same.
- [ ] **P1-27.4 Callbacks** — modified, button clicked, enabled/visible. Pitfall: S-015 Q-5 — v1 beats v2 when both are set; `apply_settings` passes its own root, `modified`/`button_clicked` pass the top-most container. Done when: callback returns `true` triggers refresh.
- [ ] **P1-27.5 ABI test** — extend the plugin of P0-15 (`test/test-input/`).
- [ ] **P1-27.6 Apply recipe (added)** — Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim. Vectors: `test/conformance/scripts/props/`.

### P1-28 Hotkey data layer
- [ ] **P1-28.1 Name map** — Files: `libobs/obs-hotkey-name-map.c` (134), `libobs/obs-hotkeys.h` (653, key list). Key id↔name table both lookups → `crates/obs-hotkey-names` (S-024 M4). Vectors: `test/conformance/scripts/helpers/` (`hotkeys-*` scripts). Done when: all key names equal.
- [ ] **P1-28.2 Combo JSON** — `obs_hotkey_binding` save/load format is specified by S-034 (persistence section) and vectorized in `test/conformance/scripts/hotkeys/03-bindings-load-save.script`; the implementation belongs to P4-28.2. C reference behaviour: `libobs/obs-hotkey.c` (1400). Done when: vectors verified against C `obs-hotkey.c` behaviour (no Phase-1 code).
- [ ] **P1-28.3 Golden** — Vectors: `test/conformance/scripts/helpers/` (`hotkeys-*`), `hotkeys/`.
- [ ] **P1-28.4 Apply recipe (added)** — for the name map (P1-28.1; Files: `libobs/CMakeLists.txt` (396), `obs-ffi` shim); P1-28.2 is vectors only.

## E. `shared/` leaf helpers

### P1-33 `shared/` leaf helpers (added)
- [ ] **P1-33.1 `shared/bpm`** — Files: `shared/bpm/bpm.c` (598), `bpm.h` (25), `bpm-internal.h` (105). Apply recipe for S-024 M8 in `crates/obs-bpm` (counters, timestamps, SEI payloads, per-codec packet injection). The AV1 `metadata_obu` framing is inlined from S-012 (formula given in S-024 §4.8) — no build dependency on the Phase 2 NAL tasks (P2-17…P2-20). Pitfall: S-024 R56 — a HEVC packet without an Annex-B start code is left unchanged but counters still advance; R57 — unknown codec: packet bytes unchanged, counters advanced. Done when: the `bpm` scripts in `test/conformance/scripts/helpers/` pass against the C reference.
- [ ] **P1-33.2 `shared/file-updater`** — Files: `shared/file-updater/file-updater/file-updater.c` (541), `file-updater.h` (22). Apply recipe for S-024 M10 in `crates/obs-file-updater` (version comparison, fake-HTTP/in-memory-filesystem driven; the module has no hashes). Vectors: `test/conformance/scripts/helpers/` (`fu-*` scripts). Done when: M10 scripts pass.
- [ ] **P1-33.3 `shared/happy-eyeballs`** — Files: `shared/happy-eyeballs/happy-eyeballs.c` (739), `happy-eyeballs.h` (184). Apply recipe for S-024 M9 in `crates/obs-netutil` (address-list ordering, resolver hints, constants only — the socket race is out of scope and stays behind the C until a spec exists). Vectors: `test/conformance/scripts/helpers/` (`he-*` scripts). Done when: M9 scripts pass.
- [ ] **P1-33.4 `shared/obs-inject-library`** — Files: `shared/obs-inject-library/inject-library.c` (134), `inject-library.h` (15). No spec (thin `LoadLibrary`/`dlopen` helper, 134 lines): port from the C, unit-test symbol resolution and the failure paths. Consumed only by `plugins/win-capture`: `game-capture.c` (P5-31.6) and `inject-helper/` (P5-31.7). Done when: crate passes its unit tests and the C file stays linked for P5-31.6/P5-31.7 (excluded only when win-capture is ported).
- [ ] **P1-33.5 `shared/opts-parser`** — Files: `shared/opts-parser/opts-parser.c` (81), `opts-parser.h` (27). Apply recipe for S-024 M1 in `crates/obs-util-small` (option-string parsing). Vectors: `test/conformance/scripts/helpers/` (`opts-*` scripts). Done when: M1 scripts pass.

## F. Integration

### P1-29 `obs-ffi` header diff
- [ ] **P1-29.1 cbindgen config** — Files: `crates/*/` cbindgen configs (new), per crate. `cpp_compat`, prefix none. Done when: header generated.
- [ ] **P1-29.2 Diff script** — normalise whitespace; compare each function declaration to `libobs/**/*.h`. Done when: report of mismatches empty.
- [ ] **P1-29.3 CI** step. Files: `.github/workflows/`.

### P1-30 Switch default
- [ ] **P1-30.1 Nightly** with `OBS_ENABLE_RUST=ON` (`CMakePresets.json`).
- [ ] **P1-30.2 Observe** two weeks: crash reports, perf. Done when: no new crash signatures.
- [ ] **P1-30.3 Flip** default and update docs. Files: `CMakeLists.txt` (37), `CMakePresets.json`.

### P1-31 Remove vendored deps
- [ ] **P1-31.1 Usage scan** — Files: `deps/blake2/`, `deps/json11/` (scan targets). `grep -rn "blake2\|json11" --include=CMakeLists.txt --include=*.c* --include=*.cpp .`. Done when: the only remaining users are frontend — `deps/blake2`: `frontend/updater/hash.cpp` (81); `deps/json11`: `frontend/` OAuth/importer code — and nothing in `libobs/`.
- [ ] **P1-31.2 Deletion (deferred to P7-04)** — cannot delete in Phase 1: `frontend/updater` (P6-32) still links `deps/blake2` and the C frontend still links `deps/json11` until Phase 6 ports them. Record the deferral (with both consumers named) so P7-04 deletes them. Done when: deferral recorded in the P1-32 review notes.

### P1-32 Phase 1 review
- [ ] **P1-32.1 Benchmarks** vs baseline table (`docs/rust-rewrite/benchmarks.md`).
- [ ] **P1-32.2 Goldens/ABI** on 3 OSes.
- [ ] **P1-32.3 Tick** parent tasks and update estimates. Files: `docs/rust-rewrite/phase-1-leaf-libraries.md`.

### P1-34 libobs & shared spec sweep (added)
- Goal: every `libobs/` + `shared/` file cited by a spec (round-6 target: S2 = 100% on the function inventory; per-file lists in `docs/rust-rewrite/specs/BACKLOG.md`).
- [ ] **P1-34.1 Extend S-001…S-011** — container/math/lexer header inlines + core glue (32 files / 361 fns)
  - Files: `libobs/callback/`, `libobs/graphics/`, `libobs/util/{darray.h, deque.h, dstr.h, cf-lexer.*, cf-parser.*, lexer.*, base.c, bmem.*, util.hpp}` — full list in `specs/BACKLOG.md`.
  - Steps: 1. extend each of S-001/002/003/004/005/006/008/009/010/011 to cite its remaining files and add rules for the uncited functions (header inlines, e.g. `vec3.h` 20, `darray.h` 26, `dstr.h` 18, `cf-lexer.c` 55).
  - Done when: the 32 files cite (S2 = 1 in the inventory) and no file behaviour is left unspecified.
- [ ] **P1-34.2 Extend S-010/S-013/S-023/S-024** — media conversion, serializers, platform/util, small helpers (24 files / 254 fns)
  - Files: `libobs/media-io/`, the serializer files, `libobs/util/{platform.c, platform-windows.c, utf8.c, profiler.hpp, windows/}`, `shared/{file-updater, happy-eyeballs, bpm, opts-parser}`.
  - Steps: 1. cite the files in the owning spec and add rules for the uncited functions (`platform-windows.c` 67 fns is the largest).
  - Done when: same.
- [ ] **P1-34.3 Extend S-041/S-042/S-035** — output, core remainder, module loader (10 files / 233 fns)
  - Files: `libobs/obs-output.c` (146 fns — S-041 is marked "partial coverage" for exactly this file), `obs-output-delay.c`, `obs-cocoa.m`, `obs-windows.c`, `obs-win-crash-handler.c`, `obs.hpp`, `obs-core-modules*`.
  - Steps: 1. finish S-041 for `obs-output.c` + delay; 2. extend S-042 with the platform vtables + crash handler + module-loader glue.
  - Done when: same; S-041 no longer "partial coverage".
- [ ] **P1-34.4 Extend S-032/S-065** — audio controls + audio-monitoring backends (4 files / 54 fns)
  - Files: `libobs/obs-audio-controls.c` (40 fns), `libobs/audio-monitoring/{win32,osx}/` enumeration files.
  - Done when: same (S-065 is written by P1-05.5's sibling P2-15.6 — coordinate).
