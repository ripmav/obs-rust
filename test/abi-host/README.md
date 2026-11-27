# ABI conformance harness (WP-40, P0-15 + P1-27.5)

Proves the C-vs-Rust ABI identity of the `OBS_ENABLE_RUST` swap set at the
module level: a trace plugin is loaded through the real
`obs_open_module`/`obs_init_module` dlopen path, one object of each kind
(source/encoder/output/service) is created, a fixed workload is driven, and
the resulting trace is compared **byte-for-byte** between the two builds:

| build  | mode  | what links where                                                                    | OBS_ENABLE_RUST |
| ------ | ----- | ----------------------------------------------------------------------------------- | --------------- |
| `c`    | all-C | all pinned 33.0.1 libobs C sources                                                  | `OFF`           |
| `rust` | swap  | the 11 swapped util files dropped, `target/release/libobs_ffi.a` (+`-lcurl`) linked | `ON`            |

The swap set is the 17-file set of the `OBS_ENABLE_RUST` CMake patch
(`test/conformance/run.sh` `apply_swap`); the mini build compiles 11 of
them, and exactly those 11 are dropped in rust mode. `obs-data.c`,
`obs-properties.c` and the graphics math stay C in **both** builds (not in
the swap set). The trace identity therefore certifies: the dlopen/ABI
boundary, the module-load path (text lookup), the data round-trips,
threading (destroy queue), config-file I/O (startup/shutdown) and the
source profiler (inside every `video_tick`).

## Files

- `abi-host.c` — the driver (loads the plugin, drives the workload, writes
  `trace.txt` = `argv[1]`).
- `trace-plugin.c` — the plugin (`libtest-input.so`): source `abi_trace`
  (builds EVERY S-015 §5 property kind in `get_properties`), dummy encoder
  `abi_enc`, dummy output `abi_out`, dummy service `abi_svc`.
- `abitrace.h` — the single trace channel (host + plugin, one atomic
  `write()` per line under a mutex).
- `glstubs.c` / `nixstubs.c` — no-op `gs_*` family + no-op X11 hotkeys
  vtable (see the file headers).
- `cfg/obsconfig.h.in` — CMake-`obsconfig.h` stand-in, configured by
  `build.sh` into `<OUT>/obsconfig.h`.
- `traces/trace.golden` — the committed trace (generated from the `c`
  build; the `rust` build's trace must equal it byte-for-byte).

## Build and run

```sh
# C build (OBS_ENABLE_RUST "OFF")
LIBOBS=/path/to/obs-studio-3301/libobs \
JANSSON=/path/to/jansson-prefix \
OUT=/tmp/abi-c bash test/abi-host/build.sh c
/tmp/abi-c/abi-host /tmp/abi-c/trace.txt

# Rust build (OBS_ENABLE_RUST "ON") — needs the staticlib first:
cargo build -p obs-ffi --release --features c-swap
LIBOBS=... JANSSON=... OUT=/tmp/abi-rust bash test/abi-host/build.sh rust
/tmp/abi-rust/abi-host /tmp/abi-rust/trace.txt

# The conformance check:
diff /tmp/abi-c/trace.txt /tmp/abi-rust/trace.txt   # must be empty
diff /tmp/abi-c/trace.txt test/abi-host/traces/trace.golden
```

Env: `LIBOBS`, `JANSSON`, `OUT`, `CC`, `CONFORMANCE_EXTRA_CFLAGS` (same
contract as `test/conformance/run.sh` / `test/ref-dump/build.sh`). `OUT` is
the output directory: it contains `abi-host`, `libtest-input.so`,
`data/libobs/*.effect` (copied from `$LIBOBS/data`) and the configured
`obsconfig.h`.

CI (conformance job, `rust.yml` step "ABI conformance C vs Rust") builds
both modes against the pinned 33.0.1 checkout, runs both hosts and diffs
both traces against each other and against `traces/trace.golden`.

## The workload (fixed, in `abi-host.c`)

1. `obs_startup("en-US", <scratch dir>, NULL)` (module config file is
   written/read through the cf-lexer/cf-parser path — swapped in rust mode).
1. `obs_open_module`/`obs_init_module` on the built plugin (dlopen,
   `-rdynamic` export table, text-lookup locale load).
1. Source `abi_trace`: create with 21 non-default settings values (value
   round-trip #1), 10× `obs_source_video_tick(0.016f)` +
   `obs_source_video_render`, `obs_source_properties` → full enumeration of
   every property (name/type/visible/enabled + kind-specific attributes,
   list item values incl. disabled items, frame-rate options/ranges, group
   children), `obs_properties_apply_settings` → modified/modified2
   callbacks (round-trip #3, P1-27.4 machinery), `obs_property_button_clicked`,
   destroy, 2× `obs_source_update` (round-trip #2), release.
1. Encoder `abi_enc`: create with settings, `obs_encoder_initialize`
   (fires the plugin's create), 3× `obs_encoder_update`,
   `obs_encoder_get_extra_data`, release.
1. Output `abi_out`: create, start, stop, release.
1. Service `abi_svc`: create, release.
1. `obs_shutdown` (drains the destroy queue, unloads the module).

## Documented deviations (subtask text → delivered design)

The subtask texts name `test/test-input/` + CMake + ctest. Delivered: a
standalone mini-libobs build **in this repo** (like WP-39's ref-dump), no
CMake, no ctest, and no change to the pinned obs-studio tree:

1. **No in-tree CMake/ctest** — the pinned obs-studio build would need a
   full OBS configure (GL, glib, ffmpeg, …) that this repo's CI does not
   run; `build.sh` is the single build path, locally and in CI.
1. **Plugin lives here, not in `test/test-input/`** —
   `test/abi-host/trace-plugin.c` (the pinned tree is read-only reference;
   the subtask's intent — load a module with dummy objects of each kind —
   is met 1:1).
1. **No `obs_reset_video`** — 33.0.1's `obs_init_graphics` needs a real
   graphics module (libobs-opengl + a GL context); the harness runs
   headless and drives ticks/renders directly (`obs_source_video_tick` /
   `obs_source_video_render`), with the `gs_*` no-op family. `gs_create`
   must fail so no graphics init is attempted.
1. **33.0.1 API divergences the harness must respect** (verified against
   the pinned headers, not memory):
   - `obs_startup(locale, module_config_path, store)` (not `obs_init`).
   - Source registration requires `get_name` (+`get_width`/`get_height`
     for non-async video sources); `output_flags` must include
     `OBS_SOURCE_VIDEO`, and `OBS_SOURCE_CUSTOM_DRAW` routes to the
     plugin's `video_render` (NULL effect).
   - Output registration with `OBS_OUTPUT_ENCODED` requires
     `encoded_packet` (and skips the headless-crashing
     `obs_get_video()`); without a raw-data capture thread
     `output->active` never gets set, so the harness drives
     `obs_output_actual_stop(force)` and re-signals `stopping_event`
     (which the capture thread normally signals on exit) or
     `obs_output_destroy` waits forever.
   - Encoder lifecycle moved: the plugin's create/update/destroy callbacks
     fire inside `obs_encoder_initialize()`, which requires
     `encoder->media` — there is no public frame-queue API in 33.0.1
     (queuing is owned by the video system). The harness sets the internal
     `media` field to a sentinel (a video encoder with no scaling
     configured never dereferences it) and calls `obs_encoder_initialize`;
     the "3× encoder control" of the subtask is 3× `obs_encoder_update`
     (+`get_extra_data`).
   - Service has no public tick/heartbeat API in 33.0.1 — create/release
     only.
1. **Trace normalisation** (the identity contract): lines contain literals
   and values only — never pointers, addresses, uuids, times or paths
   (scratch dirs are excluded from every line); multi-line setting values
   are rendered as literal `\n`; the output `stop` timestamp is not traced;
   font face/style names are not traced (the C source-creation path
   rewrites them from the platform's font list — environment-dependent).
   Same line format in both builds.

## Findings pinned by this harness (33.0.1)

- `dvtcc.c` is corrupt upstream (broken operators in the 33.0.1 tag) — the
  CMake caption build excludes it, so does `build.sh` (vtt.c compiles fine).
- The graphics-module wall above (no headless video reset).
- `obs_output_create` calls `obs_get_video()` unguarded for non-encoded
  outputs → crashes headless (`main_canvas->mix` is NULL).
- `obs_wait_for_destroy_queue()` early-returns when no video/audio threads
  exist (obs.c:3364) even though destroys are still queued asynchronously on
  the destruction task thread — a release therefore returns before the
  plugin's destroy callback ran. The first harness version used it as the
  drain, which made the trace order (destroy line vs. next object's create
  line) a main-thread-vs-destroy-thread race: CI's x86_64 caught the C and
  Rust builds interleaving differently (`enc create` before/after
  `source destroy`). The host now drains `obs->destruction_task_thread`
  directly (`os_task_queue_wait`, looping until quiescent — destroy
  callbacks can re-queue cascading destroys), which is what makes the trace
  order deterministic.
- The C source-creation path rewrites font `face`/`style` strings from the
  platform's font list (CI: set `Serif`/`Bold` came back `Serif`/`Serif`
  on x86_64 and `Bold`/`Bold` on aarch64) — environment-dependent, so the
  font create-value trace line carries only `size`/`flags`. The C-vs-Rust
  identity is unaffected (the font code is C in both builds).
- The plugin resolves all libobs symbols against the host's `-rdynamic`
  export table at dlopen time; rust mode additionally needs `-lcurl`
  (the staticlib's happy-eyeballs bridge references it).
