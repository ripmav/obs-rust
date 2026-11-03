# Developer primer: read this before your first task

Audience: an engineer who knows Rust and has not worked on OBS internals. Everything here was verified against this checkout (`libobs` 33.0.1). Where something is a proposal, it says **Proposal**. Per-phase detail is in `phase-N-guide.md` next to this file; task lists are in `../phase-N-*.md` and `../subtasks/`.

## 1. Build and run the existing C OBS first

You cannot replace what you have not run. Do this once.

1. Presets are in `CMakePresets.json` (`ubuntu`, `ubuntu-ci`, `macos`, `macos-ci`, `windows-x64`, `windows-arm64`, `*-ci-*`, plus a `dependencies` preset). Linux: `cmake --preset ubuntu` then `cmake --build --preset ubuntu` (read `cmake/common/buildspec_common.cmake` for how prebuilt dependencies are fetched).
2. Unit tests are gated by `ENABLE_UNIT_TESTS` (see `test/CMakeLists.txt`; it adds `test/cmocka`, needs the `cmocka` package). `BUILD_TESTS` builds `test/test-input`, a plugin used by tests. Run with `ctest --test-dir build_<preset>`.
3. Git submodules: `plugins/obs-websocket` (5.7.6), `plugins/obs-browser` (2.23.4) and `deps/libdshowcapture/src` (no-atl) are initialized in this checkout (a fresh clone still needs `git submodule update --init`); their source files are part of the coverage matrix.
4. Run the app once, make a scene with a Colour Source and a Text source, record 10 seconds. Open `~/.config/obs-studio/logs/` (Linux) and read the log start-up sequence: it prints every module loaded, the graphics adapter, the video/audio settings. Expect the same lines from the Rust build.

## 2. Repository map (what lives where)

| Path | What it is | Rewritten in |
|---|---|---|
| `libobs/util`, `libobs/callback`, `libobs/graphics/*math*` | C utility layer: strings, arrays, threading, config, signals, math | Phase 1 |
| `libobs/media-io` | video/audio output threads, converters, resamplers, remux | Phase 2 |
| `libobs/audio-monitoring` | per-OS monitoring backends (`null`, `osx`, `pulse`, `win32`) | Phase 2 |
| `libobs/graphics` (+ `libobs/data/*.effect`) | `gs_*` API, effect and shader parsers | Phase 3 |
| `libobs-opengl`, `libobs-d3d11`, `libobs-metal` (Swift), `libobs-winrt` | graphics backends loaded as shared modules | Phase 3 |
| `libobs/obs-*.c` | sources, scenes, outputs, encoders, services, hotkeys, modules, canvases | Phase 4 |
| `plugins/*` | 41 modules (5 with their own Qt UI: `aja-output-ui`, `decklink-captions`, `decklink-output-ui`, `frontend-tools`, `obs-vst`; some are submodules) | Phase 5 |
| `frontend/` | Qt6 application (≈88k C++ lines, 36 `.ui` forms, 77 locales) | Phase 6 |
| `shared/` | helpers used by several components (`obs-scripting`, `media-playback`, `properties-view`, …) | Phases 5–7 |
| `deps/` | `blake2`, `glad`, `json11`, `libcaption`, `libdshowcapture`, `w32-pthreads` | Phases 1, 2, 7 |
| `test/` | cmocka tests + `test-input` plugin | extended in Phase 0 |

## 3. Vocabulary

- **Source** (`obs_source_t`): anything that produces video and/or audio (capture, image, browser, media, scene). Types: input, filter, transition, scene.
- **Filter**: a source attached to another source that modifies its video or audio.
- **Scene / scene item**: a scene is a source containing items; an item is a source plus a transform (position, rotation, scale, crop, bounds).
- **Transition**: a source that blends two other sources over time.
- **Encoder** (`obs_encoder_t`): turns raw frames/samples into packets (x264, NVENC, AAC, …).
- **Output** (`obs_output_t`): consumes raw or encoded data (RTMP stream, file muxer, virtual camera).
- **Service** (`obs_service_t`): URL/key/protocol settings for a streaming destination.
- **Module**: a shared library exporting `obs_module_load` etc. Registers the above types.
- **Mix / track**: libobs has 6 audio mixes (`MAX_AUDIO_MIXES`); users call them tracks.
- **Canvas / view**: a canvas owns a video mix (its own resolution and output); a view holds the source channels rendered into it.
- **Graphics thread**: the single thread that owns the GPU context; most `gs_*` calls are only valid on it (after `gs_enter_context`).
- **Effect**: a `.effect` file with HLSL-like shaders plus techniques/passes, parsed by libobs and translated per backend.
- **calldata / signal / proc**: untyped key-value stack, event emitter, and named function table used for loose coupling (events like `source_add`, procs like `get_hotkey_bindings`).
- **obs_data / obs_properties**: JSON-like settings object / description of UI controls for a source or encoder.

## 4. Threads you will meet (verified in code)

| Thread | Created by | Drives | Notes |
|---|---|---|---|
| Graphics | `obs_graphics_thread_loop` (`libobs/obs-video.c:1094`) | ticks sources, renders, converts, calls `output_frames()`, renders displays, `video_sleep()` to the frame time | owns GPU context |
| video-io | `video_thread` (`media-io/video-io.c:190`) | waits on `update_semaphore`, hands cached frames to `video_input` callbacks (encoders/raw outputs) | graphics thread posts the semaphore |
| audio-io | `audio_thread` (`media-io/audio-io.c:205`) | wakes every `AUDIO_OUTPUT_FRAMES` (1024) samples using `os_sleepto_ns_fast`, calls `input_cb` (= `audio_callback` in `obs-audio.c:548`) to fill 6 mix buffers, clamps to ±1, passes to connected inputs | Windows: MMCSS "Audio" class |
| Encoder threads | `obs-encoder.c` | pull from video/audio-io, call `encode` | one per encoder |
| Output threads | per output type | muxing / network send | e.g. `rtmp-stream.c` |
| Hotkey thread | `obs-hotkey.c` | translate OS events | |
| UI thread | frontend | Qt event loop | most UI-side API calls must be here |

## 5. FFI cookbook (applies to Phases 1–7)

OBS's compatibility promise is a C ABI, not a Rust API. Follow these rules in every crate that exports symbols.

**5.1 Layout.** Anything that crosses the boundary is `#[repr(C)]` and has a layout test:
```rust
#[repr(C)]
pub struct DArray { pub array: *mut c_void, pub num: usize, pub capacity: usize }
const _: () = {
    assert!(size_of::<DArray>() == 3 * size_of::<usize>());
    assert!(core::mem::offset_of!(DArray, num) == size_of::<usize>());
};
```
Generate these assertions from the C headers where possible (P4-35).

**5.2 Exports.** Edition 2024 syntax:
```rust
#[unsafe(no_mangle)]
pub unsafe extern "C" fn bfree(ptr: *mut c_void) { /* … */ }
```
Names must match the C header exactly. Keep the symbol list diff (P1-29, P4-34) in CI.

**5.3 Panics must not unwind into C.** Wrap every exported function body (use a macro) with `catch_unwind`; on panic log and `abort()`. Do not rely on `panic = "abort"` alone: tests need unwinding.

**5.4 Header `static inline` code is part of the ABI.** `libobs/util/darray.h`, `dstr.h`, `deque.h` and `calldata.h` are mostly `static inline` code (grep counts 30, 45, 16 and 21 matching lines respectively; that includes forward declarations, so the number of distinct functions is lower). Plugins compile these *into themselves*: a plugin can `da_push_back` into an array that libobs later frees with `bfree`, or build a `calldata` that libobs reads. Therefore (a) the struct layouts and growth rules are frozen, (b) memory must be allocated and freed through the S-001 allocator (`bmalloc/brealloc/bfree`: 32-byte aligned, with a live-allocation counter; **not** plain `malloc`/`free`, and there is no function to swap the allocator); the inline ones stay in the C headers.

**5.5 Variadic functions cannot be written in stable Rust.** Exported variadics: `blog`, `bcrash` (`util/base.h`), `dstr_printf`, `dstr_catf` (`util/dstr.h`), `os_process_args_add_argf` (`util/pipe.h`), `profile_store_name` (`util/profiler.h`), `gs_debug_marker_begin_format` (`graphics/graphics.h`). `va_list` also appears in `blogva`, `dstr_vprintf`, `dstr_vcatf`, and in the log/crash handler callback signatures (`base_set_log_handler`, `base_set_crash_handler`). **Proposal:** keep a tiny C file in `obs-ffi` (`c/variadic.c`, built with the `cc` crate) that implements these entry points using `vsnprintf`/`va_list` and forwards a plain `(const char*)` to Rust. Logging (`blog` → handler with a `va_list`) stays in C for as long as handlers receive `va_list`; Rust code logs through `obs_ffi_log(level, msg)` which calls `blog("%s", msg)`.

**5.6 Strings.** C strings are bytes. Do not assume UTF-8 on Linux or Windows narrow APIs; convert at the boundary with `CStr::to_bytes()` and `String::from_utf8_lossy` only where the C code treated them as text. `bstrdup`-allocated strings returned to C must be freed by C with `bfree`: allocate with the libobs allocator, never with Rust's `CString::into_raw` (which pairs with `CString::from_raw`).

**5.7 Ownership.** For every exported function write down in a doc comment: who allocates, who frees, whether the returned pointer is a new reference (`obs_data_get_obj` returns a new ref) or borrowed (`obs_source_get_name` returns an internal pointer valid until the source is destroyed or renamed). Mistakes here are the most common source of crashes in plugins.

**5.8 Callbacks.** C callbacks are `unsafe extern "C" fn(*mut c_void, …)` plus a `*mut c_void` user pointer. Store both; never assume the callback is `Send`. Document the thread it is invoked on.

**5.9 Globals.** The C code has one global `obs` (`obs-internal.h`) and one graphics context per thread (`gs_enter_context`). In Rust keep one `Core` behind an explicit handle plus a thread-local "current graphics". No `static mut`.

## 6. Test strategy used throughout

1. **Golden tests** compare Rust output to files produced by the C code (`test/golden/*.golden`, tool from P0-13/P0-14). Regenerate only with `UPDATE_GOLDEN=1` on the C side, never on the Rust side.
2. **Differential tests/fuzzing** run C and Rust on the same random input (`cargo-fuzz`).
3. **ABI tests** load a real C plugin (P0-15) into the Rust core.
4. **Behavioural tests** with a fake clock (`Clock` trait) for anything time-based.
5. **Benchmarks** against the C baseline (P0-17); a >2% regression fails CI.
6. **Clean-room work** uses the specs in `docs/rust-rewrite/specs/` and the conformance suite in `test/conformance/` (see `docs/rust-rewrite/clean-room/PROCESS.md`).
7. Add a test-matrix file in `docs/tests/` for every test file (project rule); name it after the issue number when there is one (`I-<n>-<name>.md`).

## 7. Project rules every PR must follow (from the project guidelines, `AGENTS.md`)

- Never work on `master`. Branch name `<type>/<issue>-<short-description>` (`feat`, `fix`, `refactor`, `docs`, `test`, `ci`, `perf`, `build`, `chore`).
- Stack PRs; start a stack with `docs:` PRs (ADRs, specs).
- Tests for Windows: `.gitattributes` has `*.text -text` and `*.golden -text`.
- Update README/rustdoc/comments with every change; tick the task in the phase file.
- Commit/PR footer: `Assisted-by: [Model] ([Effort]) via [Tool]`.
- Before commit (Rust): `cargo fmt --check`, `cargo clippy --all-targets -- -D warnings`, `cargo deny check`, `cargo audit`, `cargo test --workspace`, plus the C build and `ctest` while C code remains. (The Go checklist in the global rules applies only to Go code.)
- Naming: use `with_` for builder methods that take a special parameter (e.g. `Config::with_context(…)`), in line with the project naming guideline.

## 8. Definition of done for any task

- [ ] Tests listed under "Done when" exist and pass on Linux, Windows, macOS (or the task states which OS applies).
- [ ] Symbol/layout checks pass for every exported item touched.
- [ ] Benchmarks within 2% of baseline for performance-sensitive code.
- [ ] Docs updated, test matrix added, phase/subtask checkboxes ticked.
- [ ] No new `unsafe` without `// SAFETY:`; no new dependency without a one-line justification.
