# S-055 libobs util remainder: CRC-32, child-process pipes, source profiler, D-Bus / portal sleep inhibition, PulseAudio audio monitor

| | |
|---|---|
| Status | Ready for review (Rust sufficiency check skipped by assignment) |
| Reference | libobs 33.0.1: `libobs/util/crc32.c` (+ `.h`), `libobs/util/pipe.c`, `libobs/util/pipe-posix.c`, `libobs/util/source-profiler.c` (+ `.h`), `libobs/util/platform-nix-dbus.c`, `libobs/util/platform-nix-portal.c`, `libobs/audio-monitoring/pulse/pulseaudio-wrapper.c` (+ `.h`), `pulseaudio-output.c`, `pulseaudio-enum-devices.c`, `pulseaudio-monitoring-available.c` |
| Target crates | `obs-util-small` (crc32, pipe), `obs-source-profiler`, `obs-sleep-inhibit` (dbus + portal), `obs-audio-monitor-pulse` |
| Depends on | S-001 (allocation), S-005 (deque, as detail), S-023 (platform helpers, `os_gettime_ns`), S-024 (`os_process_args_*` argument list container, `profiler.c` name-based profiler) |
| Conformance | `test/conformance/scripts/utilrest/` (26 scripts, vectors in `vectors/utilrest/`), runner `gen/utilrest_runner.c` |

## 1. Scope

| # | Module | In scope | Not covered |
|---|---|---|---|
| U1 | `crc32.c` | the whole function | |
| U2 | `pipe-posix.c` + `os_process_args_*` of `pipe.c` | create (shell and argument-list form), read, read_err, write, destroy | `pipe-windows.c` (Windows) |
| U3 | `source-profiler.c` | complete: enable / GPU enable, frame lifecycle, tick and render sampling, async timestamps, result computation, removal | the real graphics timers (replaced by a fake, see 8) |
| U4 | `platform-nix-dbus.c` | service probing, inhibit / uninhibit | |
| U5 | `platform-nix-portal.c` | xdg-desktop-portal Inhibit request flow | |
| U6 | `audio-monitoring/pulse/*` | the whole PulseAudio monitoring backend against a fake server | the real PulseAudio server |

`audio-monitoring/windows/wasapi-*`, `audio-monitoring/apple/*`, `null-audio-monitoring.c`, `util/pipe-windows.c`, `util/platform-windows.c`, `platform-cocoa.m` are platform code and out of scope (Windows/macOS differences are listed in section 9). `util/profiler.c` is S-024 M7. `platform-nix.c` is S-023.

Conventions: RFC 2119 words; 64-bit little-endian; "ns" are nanoseconds. Times in U3 come from `os_gettime_ns()`; the conformance runner replaces it with a variable.

## 2. U1 CRC-32

```c
uint32_t calc_crc32(uint32_t crc, const void *buf, size_t size);
```

- **S-055-R01** The result is the standard CRC-32 (IEEE 802.3: reflected, polynomial 0xEDB88320, table driven) over `size` bytes, where the *running value* is passed in and returned in finished form: the function XORs `0xFFFFFFFF` into the input value before and into the result after processing. Consequently `calc_crc32(0, data)` equals zlib's `crc32(0, data)` and a checksum can be continued by passing the previous result as `crc`. Worked: `crc32("123456789") = cbf43926`; `crc32("1234") = 9be3e0a3`; continuing with `"56789"` gives `cbf43926`; `crc32("1") = 83dcefb7`.
- **S-055-R02** `size == 0` returns `crc` unchanged (`buf` may be NULL then). Any seed value is allowed (`0xffffffff`, `0xdeadbeef`).

## 3. U2 Child-process pipes (POSIX)

```c
os_process_pipe_t *os_process_pipe_create(const char *cmd_line, const char *type);
os_process_pipe_t *os_process_pipe_create2(const os_process_args_t *args, const char *type);
int    os_process_pipe_destroy(os_process_pipe_t *pp);
size_t os_process_pipe_read(os_process_pipe_t *pp, uint8_t *data, size_t len);
size_t os_process_pipe_read_err(os_process_pipe_t *pp, uint8_t *data, size_t len);
size_t os_process_pipe_write(os_process_pipe_t *pp, const uint8_t *data, size_t len);
os_process_args_t *os_process_args_create(const char *exe); /* add_arg, add_argf, get_argv, get_argc, destroy */
```

Handle state: mode (read / write), child pid, a buffered stdio stream on the main pipe, a buffered stdio stream on the child's stderr.

- **S-055-R03** `create(cmd, type)`: returns NULL if `cmd` or `type` is NULL. Otherwise the child is `/bin/sh -c cmd` (argv0 `sh`). `create2(args, type)` runs `argv[0]` of the list *directly* with the whole list as argv (no shell, no PATH lookup: a bare name such as `echo` fails). Both inherit the environment.
- **S-055-R04** The mode is *read* iff the first character of `type` is `r`; every other string (`"w"`, `"x"`, `""`) is *write* mode. `"rw"` is read mode.
- **S-055-R05** Read mode: the child's stdout is connected to the handle (`read`); write mode: the child's stdin is connected to the handle (`write`). In both modes the child's stderr is connected to the handle (`read_err`). In read mode the child's stdin is inherited; in write mode the child's stdout is inherited (not captured). All parent-side pipe ends are close-on-exec.
- **S-055-R06** If the process cannot be started (`posix_spawn` fails: missing executable in the `create2` form) the result is NULL and no descriptor is leaked. A command run through the shell that does not exist is *not* an error at this level: the handle is returned and `destroy` reports 127.
- **S-055-R07** `read` / `read_err` use stdio `fread` semantics: they block until `len` bytes arrived or end of file and return the number of bytes read (0 at EOF). `read` on a write-mode handle returns 0 without touching the stream; `read_err` works in both modes. NULL handle: all three I/O functions return 0.
- **S-055-R08** `write` on a read-mode handle returns 0. Otherwise it loops `fwrite` until all bytes are written or a write returns 0 and returns the number of bytes written (partial count on error, e.g. a closed child). `len == 0` returns 0. Written data is buffered in the stdio stream (see Q-1).
- **S-055-R09** `destroy(NULL)` returns 0. Otherwise it closes the main stream (flushing buffered writes; the child sees EOF), closes the stderr stream, waits for the child (retrying on EINTR) and returns the child's exit status converted as `(int)(signed char)status`: 7 -> 7, 200 -> -56, 255 -> -1, 128 -> -128. It frees the handle.
- **S-055-R10** (`os_process_args_*`, listed for the `create2` form): `args_create(exe)` starts a list holding `exe`; `add_arg` appends a copy; `add_argf` appends the `printf`-formatted string; `get_argv` returns the NULL-terminated vector, `get_argc` the number of strings without the terminator; `destroy` frees everything. Arguments are passed literally (`$HOME`, `*`, `;` stay as they are). (Semantics shared with S-024 M3.)

## 4. U3 Source profiler

Global state (single instance): `enabled`, `gpu_enabled` (effective), `enable_next`, `gpu_enable_next` (requested; written by other threads), a hash `samples` (per source, owned by the video thread), a hash `entries` (per source, protected by a reader/writer lock), `sample_capacity`, GPU timer ranges ring. `FRAME_BUFFER_SIZE = NUM_TEXTURES = 2` frame slots per source.

```c
void source_profiler_enable(bool); void source_profiler_gpu_enable(bool);
void source_profiler_reset_video(struct obs_video_info *ovi);
void source_profiler_frame_begin(void); void source_profiler_frame_collect(void);
void source_profiler_render_begin(void); void source_profiler_render_end(void);
uint64_t source_profiler_source_tick_start(void);
void source_profiler_source_tick_end(obs_source_t *, uint64_t start);
uint64_t source_profiler_source_render_begin(gs_timer_t **timer);
void source_profiler_source_render_end(obs_source_t *, uint64_t start, gs_timer_t *timer);
void source_profiler_async_frame_received(obs_source_t *);
void source_profiler_remove_source(obs_source_t *);
bool source_profiler_fill_result(obs_source_t *, profiler_result_t *);
profiler_result_t *source_profiler_get_result(obs_source_t *);
```

`profiler_result_t`: `tick_avg/max`, `render_avg/max`, `render_gpu_avg/max`, `render_sum`, `render_gpu_sum` (all u64 ns), `async_input`, `async_rendered` (double fps), `async_input_best/worst`, `async_rendered_best/worst` (u64 ns).

- **S-055-R11** `enable(b)` only sets `enable_next = b`. `gpu_enable(b)` sets `gpu_enable_next = b AND enable_next` (so GPU profiling requested before the profiler is enabled is ignored). Neither changes `enabled` / `gpu_enabled` immediately.
- **S-055-R12** `frame_begin`: if `!enabled && enable_next` then `enabled = true`. If `!gpu_enabled && enabled && gpu_enable_next` then `gpu_enabled = true`; otherwise, if `gpu_enabled`, the timer range index advances by one modulo 2. (The index does *not* advance in the frame in which GPU becomes enabled.)
- **S-055-R13** `reset_video(ovi)` sets `sample_capacity = ceil(fps_num / fps_den) * 5` (using double division) and discards all per-source samples, entries and GPU timer ranges. It is called with the video thread stopped.
- **S-055-R14** While `!enabled` all of these are inert: `tick_start` returns 0, `tick_end`, `source_render_begin` (returns 0, does not touch `*timer`), `source_render_end`, `async_frame_received`, `remove_source` (nothing queued), `frame_collect`, and `fill_result` returns false without touching the output (Q-2). `get_result` returns NULL.
- **S-055-R15** `tick_start` returns the current time in ns. `tick_end(src, start)` computes `delta = now - start` and looks up (creating on first use) the source's frame samples: on creation `frame_idx = 0`; when the source already exists, `frame_idx = (frame_idx + 1) mod 2` first. It stores `delta` as that slot's `tick`. A delta of 0 is indistinguishable from "no data" (R18).
- **S-055-R16** `source_render_begin(&timer)`: if GPU profiling is effective it creates a GPU timer, begins it and stores it in `*timer`, otherwise `*timer = NULL`; returns the current time. `source_render_end(src, start, timer)`: ends the timer if non-NULL, computes the CPU delta, and appends the delta (and the timer, if any) to the lists of the source's *current* slot. If the source has no samples yet the CPU delta is dropped and a non-NULL timer is destroyed.
- **S-055-R17** `render_begin` / `render_end` (global): only when GPU profiling is effective. `render_begin` creates the timer range for the current index if absent and begins it; `render_end` ends it (nothing if the range does not exist). Both run inside the graphics context (enter / leave must balance).
- **S-055-R18** `frame_collect`, when enabled: for each source with samples take slot `(frame_idx + 1) mod 2` (the *older* slot: processing lags one frame behind). If that slot's `tick == 0` the source is skipped. Otherwise find or create the source's history entry (circular buffers of `sample_capacity` u64 each: tick, cpu, gpu, cpu-sum, gpu-sum, async-frame timestamps, async-rendered timestamps) and
  1. push `tick`;
  2. if the slot has CPU render times: push the first one into `render_cpu` and the sum of all into `render_cpu_sum`, clear the list; else push 0 / 0;
  3. if the slot has GPU timers: for each timer, only when the GPU range of the previous index is ready and not disjoint, `gs_timer_get_data` is queried; successful ticks are converted to ns as `ticks * 1e9 / frequency` (128-bit safe) and accumulated; `first` = the first accumulated value; every timer is destroyed. If any `first != 0` it is pushed to `render_gpu` and the sum to `render_gpu_sum` (nothing is pushed if no timer produced data); if the slot has no timers push 0 / 0;
  4. if the source is an async video source (`output_flags & OBS_SOURCE_ASYNC_VIDEO == OBS_SOURCE_ASYNC_VIDEO`) push `obs_source_get_last_async_ts(source)` into `async_rendered_ts`.
  A circular buffer overwrites its oldest value when full. When GPU is effective the previous index' range is queried (`get_data` -> disjoint flag, frequency); if disjoint a warning "GPU Timers were disjoint, discarding samples." is logged and no GPU values are used. After processing: if `!enable_next` -> `enabled = gpu_enabled = false`, free all samples / entries / ranges; else if `!gpu_enable_next` -> `gpu_enabled = false`, destroy the timer ranges.
- **S-055-R19** `async_frame_received(src)` pushes the current time into the source's `async_frame_ts` buffer *only if an entry exists* (entries are created by `frame_collect`, so frames received before the first collected sample are lost).
- **S-055-R20** `fill_result(src, out)`: false if `!enabled` or `out == NULL` (no write). Otherwise zero `*out`, then if an entry exists return true with:
  - `tick_avg = floor(sum / n)` and `tick_max` over the n stored ticks (n = number of valid samples, at most the capacity); 0 when n = 0;
  - `render_avg = floor(sum(render_cpu) / n)`, `render_max`, `render_sum = floor(sum(render_cpu_sum) / n)`;
  - only if GPU is effective: `render_gpu_avg`, `render_gpu_max`, `render_gpu_sum` likewise over the GPU buffers (with n = number of stored GPU values);
  - for async sources: `async_input` / `async_rendered` with best / worst from the two timestamp buffers using the *fps rule* below.
  If no entry exists the output stays zeroed and the result is false (Q-2). `get_result` allocates, calls `fill_result` and returns the buffer, or NULL (and frees it) on false.
- **S-055-R21** FPS rule (per timestamp buffer, `num` valid entries): for index i in 0..num-1, stop at the first timestamp equal to 0; `prev = ts[i-1]`, but for i = 0 `prev = ts[num-1]` (wrap-around); skip when `prev == 0` or `prev >= ts[i]`; delta = `ts[i] - prev`; track best (minimum) and worst (maximum) delta, sum of deltas and count. If count and sum are non-zero: `fps = 1e9 / (sum / count)` (double), best, worst. Otherwise all three stay 0. Because of the wrap-around, a buffer that has wrapped (not in chronological order) or whose first element is the smallest produces a very large first delta (see `srcprof_async`: worst = 1016000000).
- **S-055-R22** `remove_source(src)`: when enabled, queue a task on the *graphics* queue (non-waiting) that removes the source's samples (destroying their lists / timers) and its history entry. The data stays visible to `fill_result` until the task runs. When disabled nothing is queued. Removing an unknown source is harmless.

## 5. U4 D-Bus sleep inhibition

```c
struct dbus_sleep_info *dbus_sleep_info_create(void);
void dbus_inhibit_sleep(struct dbus_sleep_info *, const char *reason, bool active);
void dbus_sleep_info_destroy(struct dbus_sleep_info *);
```

Probe table (fixed order, index = service type):

| order | bus name | object path | interface | uninhibit method |
|---|---|---|---|---|
| 0 | `org.freedesktop.ScreenSaver` | `/ScreenSaver` | same as name | `UnInhibit` |
| 1 | `org.freedesktop.PowerManagement.Inhibit` | `/org/freedesktop/PowerManagement` | same as name | `UnInhibit` |
| 2 | `org.mate.SessionManager` | `/org/mate/SessionManager` | same as name | `Uninhibit` |
| 3 | `org.gnome.SessionManager` | `/org/gnome/SessionManager` | same as name | `Uninhibit` |

- **S-055-R23** `create`: connect to the *session* bus (failure: log error "Could not create dbus connection: <reason>", return NULL). For each service in order call `org.freedesktop.DBus.GetNameOwner(name)` (no auto-start). The first one that answers is selected (debug log "Found dbus service: <name>", return the handle); if none does, return NULL. `destroy(NULL)` is a no-op.
- **S-055-R24** `inhibit_sleep(info, reason, active)`: if `active == (cookie != 0)` do nothing. Inhibit calls `Inhibit` on the selected service: for types 0 and 1 with `(program, reason)` = `("libobs", reason)` strings; for types 2 and 3 with `(program, xid, reason, flags)` = `("libobs", u32 0, reason, u32 0xC)`. The reply `(u cookie)` is stored. Uninhibit calls the type's uninhibit method with `(u cookie)` and resets the cookie to 0. The `reason` is ignored when uninhibiting.
- **S-055-R25** If the call returns an error: log error "Failed to call <method>: <message>" and leave the state unchanged (cookie stays 0 after a failed Inhibit, non-zero after a failed uninhibit). `destroy` drops the connection and frees the handle; it does **not** uninhibit (Q-4).

## 6. U5 xdg-desktop-portal Inhibit

```c
struct portal_inhibit_info *portal_inhibit_info_create(void);
void portal_inhibit(struct portal_inhibit_info *, const char *reason, bool active);
void portal_inhibit_info_destroy(struct portal_inhibit_info *);
```

State: connection, cancellable (pending Inhibit call), signal subscription id, sender name, request path, `active`.

- **S-055-R26** `create`: session bus (failure: error log as R23, NULL). `sender_name` = unique name without the leading `:` and with every `.` replaced by `_`. It then calls `org.freedesktop.DBus.Properties.Get("org.freedesktop.portal.Inhibit", "version")` on `org.freedesktop.portal.Desktop` `/org/freedesktop/portal/desktop`; success -> debug log "Found portal inhibitor" and the handle; failure -> destroy and NULL.
- **S-055-R27** `portal_inhibit(info, reason, active)`: nothing if `active == info->active`. Inhibit: `active = true`; token = `"obs_inhibit_portal" + decimal(rand())`; `request_path = "/org/freedesktop/portal/desktop/request/" + sender_name + "/" + token`; subscribe (no bus match rule) to signal `org.freedesktop.portal.Request.Response` of `org.freedesktop.portal.Desktop` on `request_path`; asynchronously call `org.freedesktop.portal.Inhibit.Inhibit` with `("", u32 0xC, {handle_token: s token, reason: s reason})` on the object `/org/freedesktop/portal/desktop`, with a cancellable.
- **S-055-R28** Inhibit reply handling: success -> drop the cancellable. Error -> (unless the error is "cancelled") error log "Failed to inhibit: <message>", unsubscribe, forget the request path, `active = false`; the cancellable is always dropped.
- **S-055-R29** Response signal `(u response, a{sv})`: if `response != 0` -> (for `1`: warning "Inhibit denied by user") forget the request path and set `active = false`; in every case unsubscribe.
- **S-055-R30** Uninhibit: if the Inhibit call is still pending (cancellable present) cancel it and send nothing else; otherwise asynchronously call `org.freedesktop.portal.Request.Close()` on `request_path` (error reply -> warning "Error uninhibiting: <message>"). Then forget the request path and set `active = false`.
- **S-055-R31** `destroy(NULL)` is a no-op; otherwise cancel any pending call, unsubscribe, forget the path, free the sender name and release the connection. (It does not send `Close`.)

## 7. U6 PulseAudio monitoring backend

### 7.1 Wrapper (`pulseaudio-wrapper.c`)

- **S-055-R32** A process-wide counter `refs` (unsigned, initially 0) protected by a mutex. `pulseaudio_init()`: if `refs == 0` create and start a threaded mainloop and a context named "OBS-Monitor" with properties `application.name=OBS`, `application.icon_name=obs`, `media.role=production`, state callback (signals the mainloop), connect with `NOAUTOSPAWN` to the default server; then `refs++`; always returns 0. `pulseaudio_unref()`: `refs--` first, and if the *new* value is 0 disconnect and free the context and stop and free the mainloop.
- **S-055-R33** Every query helper (`get_source_info_list`, `get_source_info`, `get_sink_info_list`, `get_sink_info`, `get_server_info`, `stream_new`, `connect_playback`, `write_callback`, `set_underflow_callback`) first waits for the context: if its state is not "good" (failed / terminated) return -1 (NULL for `stream_new`, nothing for the callback setters); otherwise wait until ready. The query helpers then start the operation under the mainloop lock; a NULL operation returns -1; otherwise they wait until the operation is no longer running and return 0. A by-name query for an unknown name calls the callback with `eol < 0`.
- **S-055-R34** `get_default_id(&id)` does `init`, one server-info query and `unref`; `*id` = a newly allocated copy of `default_sink_name`, or the empty string if the name is NULL or empty. `devices_match(id1, id2)`: false if either is NULL. `name1 = id1`, or `default_sink + ".monitor"` if `id1 == "default"`. `name2 = id2 + ".monitor"` (with `id2 == "default"` replaced by the default sink first). The result is `name1 == name2`. So a capture device id matches a monitoring output iff it is that output's `.monitor` (Q-5); with no default sink name, `"default"` becomes `".monitor"`.

### 7.2 Enumeration and availability

- **S-055-R35** `obs_audio_monitoring_available()` returns true. `obs_enum_audio_monitoring_devices(cb, data)`: `init`, one sink-list query, `unref`. For each sink (not for the terminating `eol` call) the callback receives `(description, name)` in the server's order; if it returns false no further sinks are delivered (the query still completes). If the context cannot be established or the operation cannot start, nothing is delivered.

### 7.3 Monitor (`pulseaudio-output.c`)

```c
struct audio_monitor *audio_monitor_create(obs_source_t *source);
void audio_monitor_reset(struct audio_monitor *);
void audio_monitor_destroy(struct audio_monitor *);
```

Inputs from the engine: `monitoring_device_id` (NULL = none), the engine's output format (`samples_per_sec`, `speakers`), the source's `user_volume`, `activate_refs`, `output_flags` and settings (`device_id`).

- **S-055-R36** `create`: init in a local struct; on failure run the free routine on the local struct and return NULL (nothing registered). On success copy to the heap, register it in the engine's monitor list (under the monitoring mutex) and, unless it is in "ignore" mode, add a capture callback `on_audio_playback` to the source.
- **S-055-R37** Init steps in order; each failure is `false`:
  1. `monitoring_device_id == NULL` -> false (no log, **before** `init`, see D-2).
  2. If the source has `OBS_SOURCE_DO_NOT_SELF_MONITOR` and `devices_match(settings.device_id, monitoring_device_id)`: set `ignore`, log info "Prevented feedback-loop in '<device_id>'" and return **true** (no stream, no callback, no pulse init).
  3. `pulseaudio_init()`. Device = `get_default_id` if the id is `"default"` else a copy of the id.
  4. Server info query (info log "Server name: '<name> <version>'"); failure (-1) -> error "Unable to get server info !".
  5. Sink-by-name query for the device: failure -> error "Unable to get sink info !". The callback with `eol < 0` marks the format invalid -> error "An error occurred while getting the source info!". With a sink: info "Audio format: <format>, <rate> Hz, <channels> channels".
  6. Format: S16LE, S32LE, U8, FLOAT32LE are native (to OBS 16-bit, 32-bit, 8-bit, float); any other falls back to FLOAT32LE with info "Sample format <fmt> not supported by OBS,using <fmt2> instead for recording". Channels 1,2,3,4,5,6,8 map to MONO, STEREO, 2.1, 4.0, 4.1, 5.1, 7.1; others (0, 7, > 8) fall back to 2 with an info log (the log prints the two numbers with `%c`, D-4). Rate = the sink's rate.
  7. Invalid sample spec (e.g. rate 0) -> error "Sample spec is not valid".
  8. Create the resampler from (engine rate, speakers, planar float) to (sink rate, speakers for the channel count, native format); failure -> warning "audio_monitor_init: Failed to create resampler".
  9. `bytes_per_frame` = frame size of the spec; channel map per layout (see 7.5); create the stream named after the source (NULL -> error "Unable to create stream").
  10. Buffer attributes: `fragsize = maxlength = minreq = prebuf = 0xFFFFFFFF`, `tlength = bytes for 25 000 us` of the spec (`rate * channels * bytes_per_sample * 25000 / 1e6`, truncated). Flags `INTERPOLATE_TIMING | AUTO_TIMING_UPDATE | START_CORKED` (0xB in the fake log). Connect playback to the device; failure -> stop playback (disconnect, clear the write callback, unref the stream; info "Stopped Monitoring in '<dev>'" and "Got N packets with M frames") and error "Unable to connect to stream".
  11. Info "Started Monitoring in '<device>'".
- **S-055-R38** Free routine (used by create-failure, reset, destroy): if not `ignore`: remove the capture callback (even if it was never added, Q-3), destroy the resampler, free the pending-data deque, stop playback if a stream exists, `pulseaudio_unref()` and free the device. In `ignore` mode it does nothing.
- **S-055-R39** `on_audio_playback(source, audio, muted)`: try-lock the monitor's mutex, return if busy. Skip data while `source.activate_refs == 0`. Resample the planar float input to the target format (failure: skip). If `|user_volume - 1| >= EPSILON (1e-4)` scale the converted frames in place (R40). Append the bytes (`bytes_per_frame * frames`) to the pending-data deque; `packets++`, `frames += n`. Unlock, then run the stream write (R41).
- **S-055-R40** Volume scaling by native format: U8: `out = (in - 128) * vol + 128` computed in float then truncated to u8; S16 and S32: `out = (intN)(in * vol)` computed in float (wraps on overflow, no clipping); FLOAT32: `out = in * vol`. Other formats untouched.
- **S-055-R41** Stream write (under the mainloop lock and the monitor mutex): (1) if the pending size is `> 2 * tlength`, set all attrs except `tlength` to `0xFFFFFFFF`, `tlength = pending size` and call `set_buffer_attr`; (2) if the stream is corked: when pending `>= tlength` uncork, else return (data stays queued); (3) while pending > 0: `begin_write(&buf, &n)` with `n = pending size`; on error return; if PulseAudio returns `n > pending` call `cancel_write` and return; otherwise pop `n` bytes into `buf` and `write(buf, n)` (so a smaller offered chunk produces several writes).
- **S-055-R42** Stop playback: disconnect, clear the write callback, unref the stream, clear the pointer; log info "Stopped Monitoring in '<device>'" then "Got <packets> packets with <frames> frames"; reset both counters. Called from the free routine only if a stream exists.
- **S-055-R43** `destroy(NULL)` is a no-op. Otherwise run the free routine, remove the monitor from the engine list (mutex) and free it.
- **S-055-R44** `reset(monitor)`: run the free routine on the *existing* struct, then, holding its mutex, initialise a fresh local struct for the same source; on success overwrite the monitor with it and add the capture callback again; on failure free the new struct and leave the old one as is (see D-3).

### 7.4 Pending-data and counters

The pending-data deque is a plain byte queue (S-005). The monitor counts `packets` and `frames` only for accepted packets.

### 7.5 Channel maps

Positions start as FL, FR, FC, LFE, RL, RR, SL, SR for all layouts, with overrides: MONO -> `[MONO]`; STEREO -> 2 ch; 2.1 -> 3 ch with position 2 = LFE; 4.0 -> 4 ch with position 3 = REAR_CENTER; 4.1 -> 5 ch with position 4 = REAR_CENTER; 5.1 -> 6 ch; 7.1 -> 8 ch. Unknown layout -> 0 channels.

## 8. Reference defects and quirks

- **Q-1** `os_process_pipe_write` is a buffered stdio write that is never flushed. The child sees data only when the buffer fills (platform dependent size) or at `destroy`. Required (SHOULD): keep write semantics "delivered at latest on destroy"; a Rust implementation MAY flush earlier.
- **Q-2** `source_profiler_fill_result` zero-fills the output *before* looking up the source, so it returns false with a zeroed struct for unknown sources (but leaves it untouched when the profiler is disabled or the pointer is NULL).
- **Q-3** The monitor free routine removes the capture callback even for a monitor that never added it (failed `create`).
- **Q-4** `dbus_sleep_info_destroy` does not uninhibit: an active inhibition stays with the service until the connection closes (bus semantics).
- **Q-5** `devices_match` is asymmetric: `id1` is a capture device (compared as is), `id2` an output (`.monitor` appended).
- **Q-6** Log text typos are part of the vectors (missing space in "not supported by OBS,using ..."); logs are compared by level and text in this area (stricter than the README's level-only rule), implementers SHOULD match the text.
- **Q-7** The profiler's history lags one frame behind (R18) and treats a zero tick as "no data".
- **D-1** `os_process_pipe_destroy` returns the exit status for a normally exiting child, but for a child killed by a signal it returns the `waitpid` result, i.e. the *positive pid*. Required: return a value that cannot be mistaken for an exit status (e.g. 128 + signal, or a negative value); the vectors only assert that the result is positive (`pclose_signalled`). Related race: destroy closes the child's stderr before waiting, so a child that still writes to stderr dies by SIGPIPE.
- **D-2** `audio_monitor_create` with `monitoring_device_id == NULL` fails before `pulseaudio_init()` but its failure path calls `pulseaudio_unref()`: the unsigned counter wraps to its maximum, later `init` calls never create a mainloop/context, and all following PulseAudio use runs against a NULL mainloop (a crash with the real library). Required: a failed create must not change the counter; the wrapper must work afterwards (`pulse_monitor_refcount_defect` records the reference behaviour: no loop created, enumeration reports nothing and 2 operations hit a NULL loop).
- **D-3** `audio_monitor_reset` frees the monitor's resources in place before knowing whether re-initialisation succeeds. After a failed reset the structure holds dangling `device` / `resampler` pointers and a subsequent `audio_monitor_destroy` double-frees them (and unrefs PulseAudio again). Required: after a failed reset the monitor MUST be inert (all pointers cleared, no callback, safe to destroy). The vector stops after the failed reset on purpose.
- **D-4** The "channels not supported" log prints the channel counts with `%c`, producing raw control characters (e.g. `\x07 ... \x02` for 7 -> 2). Required: print the numbers (the vector keeps the raw bytes as `\x07`, `\x02` so a faithful clone is detectable; a corrected clone documents the deviation).

## 9. Platform notes

Windows: `pipe-windows.c` (`CreatePipe`/`CreateProcess`) has the same API, exit codes are `DWORD`s; WASAPI monitoring (`wasapi-output.c`) and macOS CoreAudio (`coreaudio-output.c`) replace section 7; neither is specified. The D-Bus / portal code is Linux/BSD only.

## 10. Test setup (how the vectors are produced)

The runner links the real files listed above with:
- **fake clock** (`os_gettime_ns`), **fake graphics** (`gs_timer_*`, `gs_enter/leave_context` with a balance counter), **fake task queue** (`obs_queue_task` queues, `sp_run_tasks` runs), fake `obs` core and `obs_source` stand-ins;
- **fake libpulse** (`utilrest_pafake.c`): a synchronous in-process model of the threaded mainloop, context, operations and playback stream (prints `pa ...` lines for stream calls); the pure helpers (`pa_sample_spec_valid`, `pa_frame_size`, `pa_usec_to_bytes`, `pa_sample_format_to_string`, proplists) are the **real libpulse** (so format names such as `float32le`, `aLaw`, `s24le` come from it);
- **fake resampler**: float planar -> interleaved target format at the same rate; the nearest input plane is used per output channel; u8 `x*127+128`, s16 `x*32767`, s32 `x*2147483647`, float copy (no clamping, so scripts keep |x| < 1);
- **private `dbus-daemon --session`** started by the runner (`dbus-daemon` MUST be installed) and an in-process GDBus service on a second connection that owns the requested names and answers calls (cookie 42 for freedesktop services, 43 for MATE / GNOME; portal replies `version=4`, an `(o)` request handle and, on demand, the `Response` signal).
Not tested: real PulseAudio / real desktop services; timing (the dbus runner pumps the main loop 150 ms after each asynchronous step).

Log lines are printed as `log <level> <text>` with non-printable bytes escaped; error texts that embed system wording are cut after the first `": "`.

## 11. Coverage table

| Rule(s) | Script(s) |
|---|---|
| R01, R02 | `crc32` |
| R03, R04, R05, R06, R07, R09 | `pipe_read`, `pipe_null_and_args` |
| R05, R08, R09 (write mode, Q-1) | `pipe_write` |
| R03, R10, R06 (`create2`) | `pipe_null_and_args` |
| D-1 | `pipe_read` (end) |
| R11, R12, R14, R15, R18, R20 | `srcprof_basic`, `srcprof_ring` |
| R13 | `srcprof_ring`, `srcprof_basic` |
| R16, R17, R18 (GPU), R11 (gpu ANDs enable) | `srcprof_gpu` |
| R19, R21 | `srcprof_async` |
| R22 | `srcprof_remove` |
| R23, R24, R25 | `dbus_inhibit_ss`, `dbus_inhibit_order`, `dbus_inhibit_gnome`, `dbus_inhibit_mate`, `dbus_noservice`, `dbus_nobus` |
| R26-R31 | `portal_inhibit`, `portal_errors`, `dbus_nobus` |
| R32, R33, R35 | `pulse_enum` |
| R34 | `pulse_default_match` |
| R36, R37, R38, R42, R43, R44 | `pulse_monitor_basic`, `pulse_monitor_errors`, `pulse_monitor_lifecycle` |
| R37 (formats / channels), 7.5 | `pulse_monitor_formats` |
| R39, R40 | `pulse_monitor_volume`, `pulse_monitor_write` |
| R41 | `pulse_monitor_write`, `pulse_monitor_basic` |
| D-2, D-3, D-4 | `pulse_monitor_refcount_defect`, `pulse_monitor_lifecycle`, `pulse_monitor_formats` |

## 12. Implementation notes and open questions

- Rust: `crc32fast`/`crc` crates give the same values for R01 (seed handling: pass the previous finished value as the initial state, XOR with `!0` on entry and exit as the reference does). For pipes use `std::process::Command` with pipes; keep "child stderr readable" and the exit-status rule R09 (map signals as D-1 requires).
- The profiler MUST keep per-frame work allocation-light (it runs on the video thread) and the history under a reader/writer lock; the fps rule R21 should be copied exactly, including the wrap-around (a "fixed" version changes the numbers for wrapped buffers).
- Open: whether clones SHOULD fix D-4 (text) is a project decision; the real `swresample` conversion (rounding, dithering) is **not** specified here (see S-044 resampler); only the monitor logic around it is.
- Open: the portal flow was exercised only with success / denial / error / cancellation; the `Response` for the *Close* path is not modelled.
- Skipped on purpose: the Rust sufficiency check (cost control, as assigned).
