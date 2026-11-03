# Phase 1 guide: leaf libraries

Read [00-developer-primer.md](00-developer-primer.md) (especially §5 FFI cookbook). Tasks: [../phase-1-leaf-libraries.md](../phase-1-leaf-libraries.md); subtasks: [../subtasks/phase-1-subtasks.md](../subtasks/phase-1-subtasks.md).

> **Clean-room specs exist for most of this phase:** [S-001](../specs/S-001-bmem-base.md) (allocator, logging), [S-002](../specs/S-002-calldata.md), [S-003](../specs/S-003-declaration-language.md), [S-004](../specs/S-004-signals-and-procs.md), [S-005](../specs/S-005-darray-deque.md), [S-006](../specs/S-006-dstr.md), [S-007](../specs/S-007-config-file.md), [S-008](../specs/S-008-vec-quat-matrix4.md), [S-009](../specs/S-009-math-rest.md) (Draft), [S-010](../specs/S-010-serializers-bitstream-textlookup.md), [S-011](../specs/S-011-lexer-cf-parser.md), [S-014](../specs/S-014-obs-data.md), [S-015](../specs/S-015-obs-properties.md), [S-034](../specs/S-034-hotkeys.md) — plus S-023/S-024/S-055 for the platform/pipe/profiler/helper areas (full mapping in [../subtasks/phase-1-subtasks.md](../subtasks/phase-1-subtasks.md)). Where a spec exists it is authoritative and supersedes the descriptions below.

## Strategy for the whole phase

These modules have no dependency on the rest of libobs, so they can be swapped one at a time. Two kinds of code exist and they are treated differently:

1. **Exported (non-inline) functions** (`bmalloc`, `dstr_copy`, `calldata_get_data`, `signal_handler_signal`, `obs_data_*`, …): re-implement in Rust, export with the same names.
2. **`static inline` functions and macros in headers** (`da_push_back`, `dstr_free`, `calldata_get_int`, `deque_push_back`): they are compiled into every plugin and **stay as they are**. Rust must reproduce the *struct layout and invariants* they rely on, nothing more. Rust-side users get a safe type (`DArray<T>`, `DString`) that is layout-compatible.

## Per-area notes

### `bmem` (P1-01)
`bmalloc/brealloc/bfree/bzalloc/bmemdup/bstrdup` + `bnum_allocs()`; **full spec: [S-001](../specs/S-001-bmem-base.md)**. Everything in the codebase allocates through these; plugins also free memory libobs allocated. Key facts (verified): blocks are **32-byte aligned** on every platform (POSIX uses an offset-header shim over `malloc`; Windows `_aligned_malloc`); `bmalloc(0)`, `brealloc(p,0)` and therefore `bmemdup(p,0)`/`bzalloc(0)` are **fatal** (`bcrash`); `brealloc(NULL,n)` is counted as a new allocation; there is no allocator-replacement function. Use the libobs rules, not Rust's global allocator and not plain `malloc`.

### `dstr` (P1-02)
```c
struct dstr { char *array; size_t len; /* excl. NUL */ size_t capacity; };
```
Many functions are `static inline` in `dstr.h` (45 of them: `dstr_init`, `dstr_free`, `dstr_copy_dstr`, `dstr_resize`, `dstr_find*`, …). The exported ones to port are `dstr_copy*`, `dstr_cat*`, `dstr_ncat*`, `dstr_insert*`, `dstr_remove`, `dstr_replace`, `dstr_printf/vprintf/catf/vcatf` (**variadic/`va_list`: C shim, see primer §5.5**), `astrcmpi`, `astrstri`, `strdepad`, `strlist_split`, `dstr_depad`, … Check `capacity` growth: tests that compare layouts after a series of appends will notice if you change the growth policy.

### `darray` / `deque` (P1-03, P1-04)
`struct darray { void *array; size_t num; size_t capacity; }` and `struct deque { void *data; size_t size, start_pos, end_pos, capacity; }`. Both are header-only (inline). The Rust job is the layout-compatible types and a test that C and Rust agree after the same operation sequences. There is no `circlebuf_*` alias in this tree. Spec: [S-005](../specs/S-005-darray-deque.md).

### Threading (P1-05)
`os_event_t` (auto/manual reset), `os_sem_t`, thread naming. Behavioural contract to copy exactly: `os_event_try` returns `EAGAIN` when not signalled (used by `audio_thread` in `media-io/audio-io.c:205`: `while (os_event_try(audio->stop_event) == EAGAIN)`), `os_event_timedwait` returns `ETIMEDOUT`. Windows version uses `HANDLE`s; `threading-posix.c` has a separate `#ifdef __APPLE__` implementation of `os_sem_init` (around line 170–220; macOS lacks unnamed POSIX semaphores) — read it before porting and test macOS separately.

### Time and sleep (P1-06)
`os_gettime_ns` must be monotonic (`CLOCK_MONOTONIC` in `platform-nix.c`; `CLOCK_UPTIME_RAW` via `clock_gettime_nsec_np` in `platform-cocoa.m` on macOS — the nix `os_gettime_ns` is excluded there; QPC on Windows). The *sleep* functions in `platform-nix.c` (`os_sleepto_ns`, `os_sleepto_ns_fast`, `os_sleep_ms`) are not `__APPLE__`-guarded, so they do serve macOS. `os_sleepto_ns_fast` (used by the audio thread) returns `false` immediately if the target is already past, otherwise loops `usleep(min(remaining_us, 999999))` until `os_gettime_ns() >= target` (verified in `platform-nix.c`, which `libobs/cmake/os-macos.cmake` compiles next to `platform-cocoa.m`; the Windows version in `platform-windows.c` is different: `Sleep(remain_ms)` loop — read it before porting). `os_sleepto_ns` on Windows is a `Sleep(ms-1)` + `QueryPerformanceCounter`/`YieldProcessor` spin; there is no busy-spin on Linux. Its wake-up jitter directly affects audio buffering, so compare wake-up histograms against C on each OS (P1-06.3) and copy the per-OS strategy rather than "improving" it.

### Filesystem (P1-07)
Subtle behaviours: `os_mkdirs` creates intermediate directories; `os_rename` must replace an existing destination on Windows (uses `MoveFileEx` with replace); `os_get_config_path` returns different directories per OS and honours Flatpak/XDG (`platform-nix.c`, `platform-nix-portal.c`, `platform-nix-dbus.c`). UTF-8 ↔ UTF-16 conversion for every Windows call. Add tests with paths > 260 chars and with emoji.

### Lexer / config / text lookup (P1-08, P1-09)
- `cf-lexer`/`cf-parser` are used by the **effect parser** (Phase 3); an exact token stream matters more than the API. Dump tokens for all `libobs/data/*.effect` and `plugins/*/data/*.effect` and compare.
- `config-file.c`: INI with sections (uthash-backed). `config_save_safe(config, temp_ext, backup_ext)` (`config-file.c:398`) writes `<file>.<temp_ext>`, then calls `os_safe_replace(file, temp, backup)` (platform-specific atomic replace that optionally keeps `<file>.<backup_ext>`). Port `os_safe_replace` together with it (POSIX `rename`; Windows `ReplaceFileW`/`MoveFileEx`); `os_rename` on Windows uses `MOVEFILE_REPLACE_EXISTING`.
- `text-lookup`: locale `.ini` files mapping keys to strings; used by `obs_module_text`.

### Hashes (P1-10)
There is no base64 in libobs (the only base64 in the tree is the in-tree librtmp copy `plugins/obs-outputs/librtmp/cencode.c/.h`, P5-17.4). `deps/blake2` (blake2b) is used only by the frontend — the Windows updater (`frontend/updater/hash.cpp`) and the what's-new feed (`frontend/utility/WhatsNewInfoThread.cpp`) — no libobs or plugin user. `crc32.c` has exactly one in-tree user (`plugins/text-freetype2/find-font-windows.c`). Known-answer tests from the BLAKE2 reference vectors; blake2 stays in the build until P7-04 (its C consumers are ported in Phase 6).

### Processes and pipes (P1-11)
`os_process_pipe_*` is used by `obs-ffmpeg-mux` (P5-14.5), the NVENC/QSV/AMF helpers (`nvenc-helpers.c`, `obs-qsv11/common_utils_windows.cpp`, `texture-amf.cpp`), the win-capture graphics-offsets loader (P5-31.6) and the frontend profile import. Windows: handle inheritance flags; POSIX: `posix_spawn` vs `fork`. Test with an echo child.

### Profiler (P1-12)
Call-tree profiler with named scopes (`profile_start("name")`); heavily used in render loops. Names are interned in a store (`profiler_name_store_t`; `profile_store_name` is variadic → C shim). Output format is parsed by external tools; keep text and CSV identical.

### Logging (P1-13)
`blog` formats with `vsnprintf` then calls the registered handler `void (*)(int lvl, const char *fmt, va_list args, void *param)` — the handler receives the *unformatted* format string and the `va_list`. That means formatting must stay in C. Keep `base.c` (126 lines) as C in `obs-ffi/c/`, and give Rust a function `obs_ffi_log`. Do not try to be clever with nightly `c_variadic`.

### Math (P1-15…P1-19)
Types in `libobs/graphics/vec2.h`, `vec3.h`, `vec4.h`, `matrix3.h`, `matrix4.h`, `quat.h`, `plane.h`, `axisang.h`, `bounds.h`. **`struct vec3` is 16 bytes, not 12:** it is a union of `{float x, y, z, w;}`, `float ptr[4]` and `__m128 m` (verified in `vec3.h:34` and `vec4.h:32`), 16-byte aligned. Rust types need `#[repr(C, align(16))]` with a fourth `w` field, otherwise `matrix3/matrix4` (arrays of these) get a different size and every plugin that includes the header breaks. Also note `vec3_zero` etc. are `static inline` using SSE intrinsics (`_mm_*`) in the header — they stay in C; on aarch64 the header relies on `simde` (see `util/sse-intrin.h`). Tolerance in goldens: compare bit patterns when the C code does only add/mul in the same order, use relative 1e-6 where SSE vs scalar changes rounding (document each case).

### calldata (P1-20)
Layout (verified in `calldata.c`): `stack` is a packed buffer of records `[size_t name_len][name bytes incl NUL][size_t data_size][data bytes]…` terminated by a zero `size_t` (note `calldata_clear` writes a single `size_t` of 0 and sets `size = sizeof(size_t)`). The inline getters (`calldata_get_int`, …) walk this buffer in the plugin, so **the byte format is the ABI**. Implement `calldata_get_data/set_data` exactly; test by building calldata with C inline setters and reading with Rust, and the reverse.

### decl / proc / signal (P1-21, P1-22)
`decl.c` parses strings like `"void source_add(ptr source)"`. Signal semantics you must reproduce (read `callback/signal.c` lines 290–351, `signal_handler_signal`):

- each signal has its own mutex (`sig->mutex`), taken for the entire emit, and `sig->signalling = true`;
- callbacks are called in registration order **while that mutex is held**; a callback may disconnect itself or others — disconnecting during emit sets `cb->remove = true` (a thread-local `current_signal_cb` supports "disconnect self"), and the entries are physically erased *after* the loop;
- then the handler's `global_callbacks` are called under `global_callbacks_mutex`;
- `keep_ref` adjusts `handler->refs` after erasure.

Consequences for Rust: the emit loop uses indices (the array can grow during iteration if a callback connects another), the per-signal mutex **is recursive** (`pthread_mutex_init_recursive(&si->mutex)` at `signal.c:47`; also `global_callbacks_mutex`, while the handler-level `handler->mutex` is a normal mutex), so a callback may emit the same signal, and connecting from another thread during an emit blocks until it ends. Reproduce this rather than inventing a snapshot design; test with callbacks that connect/disconnect during emit.

### obs_data / properties (P1-24…P1-28)
`struct obs_data { volatile long ref; char *json; struct obs_data_item *items; }` (private, `obs-data.c`) — items in a uthash table; each `obs_data_item` is "one allocation" with name, value, default, autoselect values packed after the struct. Refcounting is manual (`obs_data_addref/release`); `obs_data_get_obj` returns a **new reference**. The JSON dialect is jansson-compatible; saved files must stay byte-for-byte stable for users (indent, key order = insertion order). Property kinds and callbacks are in `obs-properties.c` (109 exports in the header): `obs_properties_add_*` families, list items, modified callbacks `bool (*)(obs_properties_t*, obs_property_t*, obs_data_t*)`.

## Order of work inside the phase

util (P1-01…14) → math (15…19) → calldata/decl/proc (20,21) → signal (22,23) → obs_data (24…28) → shared helpers (33) → integration (29…32). `obs_data` depends on `dstr`, `darray`, `bmem`, JSON; `signal` depends on `calldata`, threading; P1-33 uses S-001/S-006 only as implementation details, its file-updater module (M10) uses `obs_data`'s JSON semantics (P1-25), and its bpm AV1 framing inlines S-012's `metadata_obu` formula — so P1-33 runs after P1-25, with no Phase 2 build dependency.

## Verification checklist

- [ ] `test/cmocka/test_darray.c`, `test_bitstream.c`, `test_serializer.c`, `test_os_path.c` pass against the Rust build.
- [ ] Symbol list for each migrated header matches (P1-29).
- [ ] Differential fuzz run of 1M iterations each for `dstr`, `calldata`, `obs_data` JSON.
- [ ] Real `global.ini` / scene collection round-trips byte-for-byte.
