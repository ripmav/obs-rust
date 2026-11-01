# S-058 Graphics-thread frame loop and Linux platform glue (`obsvideo`)

| | |
|---|---|
| Status | Ready for review (Rust sufficiency check **not performed**, see section 11) |
| Spec version | working tree on `docs/rust-rewrite-plan` (uncommitted) |
| Reference | C implementation, libobs 33.0.1: `obs-video.c`, `obs-video-gpu-encode.c`, `obs-nix.c`, `obs-nix-platform.c`, `obs-nix-x11.c` (+ the key tables they use). **Not covered:** `obs-nix-wayland.c` (see section 11) |
| Target crate | `crates/libobs-core` (module `video_loop`) and `crates/libobs-platform-linux` |
| Depends on | S-042 (core: creates/frees mixes, owns `obs`, graphics init), S-043 (graphics API used here), S-044 (`video_output_*`), S-040 (sources: `obs_source_video_tick`), S-034 (hotkeys: key names, translations), S-041 (encoders: `send_off_encoder_packet`, `video_pause_check`), S-023 (platform helpers) |
| Related, not duplicated | S-042 owns `obs_create_video_mix` / `obs_free_video_mix` / `obs_get_main_texture` and the render-texture getters; S-044 owns the ring the loop writes into |
| Conformance | `test/conformance/scripts/obsvideo/` (23 scripts), runners `gen/obsvideo_runner.c` + `gen/obsvideo_nix.c` |

## 1. Scope

Part A (`obs-video.c`, `obs-video-gpu-encode.c`): the **graphics thread**. One thread renders every video mix once per
frame interval, ticks sources and tick callbacks, renders extra displays, runs queued graphics tasks, accounts frame
timing/lag, feeds raw frames into the video output ring and GPU frames into the GPU encode queue.

Part B (`obs-nix.c`, `obs-nix-platform.c`, `obs-nix-x11.c`): Linux platform glue: platform/display selector, data file search
order, system information logging, and the X11 hotkey backend (keysym tables, keyboard/mouse polling, key-to-string).

Out of scope: what a view/source renders (S-040/S-042), effect contents, the video output worker (S-044), encoders (S-041).
The graphics API, the video output, sources, encoders and the profiler are *injected dependencies*; the vectors replace them
with recording fakes. Part B: the X server is a scripted fake.

## 2. Terminology and conventions

MUST/SHOULD/MAY as in RFC 2119. Times are nanoseconds (`u64`). "Mix" = `obs_core_video_mix` (one render target chain + one
`video_output`). `NUM_TEXTURES = 2` (double buffered staging slots), `NUM_CHANNELS = 3` (planes), `NUM_ENCODE_TEXTURES = 10`
(GPU encode frame pool), `NUM_ENCODE_TEXTURE_FRAMES_TO_WAIT = 1`. "Context" = the graphics context lock (`gs_enter_context`
/`gs_leave_context`, re-entrant counted). "Slot" = index into the `NUM_TEXTURES` ring. Colour spaces: `GS_CS_SRGB=0,
SRGB_16F=1, 709_EXTENDED=2, 709_SCRGB=3`. Scale types: `DISABLE=0, POINT=1, BICUBIC=2, BILINEAR=3, LANCZOS=4, AREA=5`.
Float values in vectors print with `%.9g`; compare exactly (all values are derived by single operations).

## 3. Interface

```
bool  obs_graphics_thread_loop(struct obs_graphics_context *ctx);   // one frame; false = thread should exit
void *obs_graphics_thread(void *param);                              // thread body: loops until the loop returns false
void  add_ready_encoder_group(obs_encoder_t *encoder);               // registers a weak ref for group start-timestamp setting
bool  init_gpu_encoding(struct obs_core_video_mix *mix);
void  stop_gpu_encoding_thread(struct obs_core_video_mix *mix);
void  free_gpu_encoding(struct obs_core_video_mix *mix);
// GPU encode thread body (private, started by init_gpu_encoding): gpu_encode_thread(mix)

struct obs_graphics_context { u64 last_time, interval, frame_time_total_ns, fps_total_ns; u32 fps_total_frames; const char *video_thread_name; }
struct obs_vframe_info { u64 timestamp; i32 count; }                 // queue element: one frame slot owed to a consumer
struct obs_tex_frame   { tex; tex_uv; u32 handle; u64 timestamp; u64 lock_key; i32 count; bool released; }

// Linux glue
const char *get_module_extension(void);                       // ".so"
char *find_libobs_data_file(const char *file);                // heap string or NULL
void  log_system_info(void);
void  obs_set_nix_platform(enum {INVALID=0, X11_EGL=1, WAYLAND=2});  void *... obs_get_nix_platform_display()/obs_set_nix_platform_display(void*)
bool  obs_hotkeys_platform_init(hotkeys*);  void obs_hotkeys_platform_free(hotkeys*);
bool  obs_hotkeys_platform_is_pressed(ctx, obs_key_t);
void  obs_key_to_str(obs_key_t, dstr*);  obs_key_t obs_key_from_virtual_key(int);  int obs_key_to_virtual_key(obs_key_t);
void  obs_key_combination_to_str(obs_key_combination_t, dstr*);
```

Mix fields used by the loop (all initialised by S-042 `obs_create_video_mix`): `view`, `render_texture`, `output_texture`,
`render_space`, `ovi.{base_width,base_height,scale_type}`, `video` (the `video_output`), `gpu_conversion`,
`convert_textures[3]`, `convert_textures_encode[2]`, `copy_surfaces[2][3]`, `active_copy_surfaces[2][3]`,
`mapped_surfaces[3]`, `conversion_techs[3]`, `conversion_width_i/height_i`, `color_matrix[16]`, atomic counters
`raw_active` / `gpu_encoder_active`, latches `raw_was_active/gpu_was_active/was_active`, `texture_rendered`,
`texture_converted`, `textures_copied[2]`, `cur_texture`, deques `vframe_info_buffer` (raw) / `vframe_info_buffer_gpu`,
`gpu_encoder_queue`, `gpu_encoder_avail_queue`, `gpu_encoders`, `gpu_encoder_mutex`, `gpu_encode_semaphore`,
`gpu_encode_inactive`, `gpu_encode_stop`, `gpu_encode_thread_initialized`.
Core fields: `video.{mixes,mixes_mutex,video_time,video_frame_interval_ns,total_frames,lagged_frames,video_fps,
video_avg_frame_time_ns,tasks,task_mutex,ready_encoder_groups,encoder_group_mutex,sdr_white_level,hdr_nominal_peak_level,effects}`,
`data.{valid,first_display,displays_mutex,draw_callbacks,rendered_callbacks,tick_callbacks,draw_callbacks_mutex,sources,
sources_mutex,sources_to_tick}`.

## 4. Data formats

Line sizes and plane geometry of GPU-converted frames (`W`,`H` = output video size, widths in **bytes** per row):

| Format | Plane 0 | Plane 1 | Plane 2 |
|---|---|---|---|
| I420 | W x H | W/2 x H/2 | W/2 x H/2 |
| NV12 | W x H | W x H/2 (interleaved UV) | - |
| I444 | W x H | W x H | W x H |
| I010 | 2W x H | W x H/2 | W x H/2 |
| P010 | 2W x H | 2W x H/2 | - |
| P216 | 2W x H | 2W x H | - |
| P416 | 2W x H | 4W x H | - |

Worked example (vector `19_gpu_download_formats`): staging surface bytes `b[k] = (k*7 + ch*13 + 1) mod 256`; I420 4x4 with tight
line sizes yields plane 0 `01 08 0f 16 1d 24 2b 32 39 40 47 4e 55 5c 63 6a`, plane 1 `0e 15 1c 23`, plane 2 `1b 22 29 30`.
With output line size 8 each row of plane 0 is followed by 4 untouched bytes.

## 5. Behaviour (numbered rules)

### 5.1 One frame (`obs_graphics_thread_loop`)

* **S-058-R01** Order within one call: (1) update active states (R02), (2) in the graphics context call `gs_begin_frame`,
  (3) tick (R04), (4) render all mixes in array order (R10), (5) render displays (R20), (6) execute graphics tasks (R21),
  (7) measure frame time, (8) `video_sleep` (R23), (9) statistics (R22), (10) return `!stop_requested()` (R24). Source-profiler
  hooks run at the start of the frame (`frame_begin`), around steps (4)-(5) (`render_begin/end`) and after (6) (`frame_collect`).
* **S-058-R02** `update_active_states`, under `mixes_mutex`, for every mix: `raw_active = raw_active_counter > 0`,
  `gpu_active = gpu_encoder_active_counter > 0`, `active = raw||gpu`. Rising edges: `!was_active && active` -> *base reset*
  (`texture_rendered=false`, `texture_converted=false`, free `vframe_info_buffer`, `cur_texture=0`); `!raw_was && raw` ->
  clear `textures_copied[]` and free the raw vframe buffer; `!gpu_was && gpu` -> free the gpu vframe buffer. Then store the
  three latches. Falling edges do not clear anything. Counters <= 0 (including negative) mean inactive.
* **S-058-R03** `gs_begin_frame` is called with the context held (enter/leave around it).

### 5.2 Tick

* **S-058-R04** `tick_sources(cur_time = video_time, last_time)`: if `last_time == 0` it becomes
  `cur_time - video_frame_interval_ns`. `seconds = (float)((double)(cur_time-last_time) / 1e9)`. Under
  `draw_callbacks_mutex` call every tick callback in **reverse registration order** with `seconds`. Then build `sources_to_tick`
  (cleared first) by walking `data.sources` (the UUID table, unspecified order) and taking a **reference** on every source that
  is not removed; then call `obs_source_video_tick(source, seconds)` for each that is **still not removed at that moment**
  (removal between snapshot and tick skips the tick), and release every reference. The loop remembers `last_time = cur_time`
  (returned value) for the next frame. The first-ever tick therefore reports exactly one interval.

### 5.3 Per-mix render

* **S-058-R10** `output_frames` runs under `mixes_mutex`. For each mix in array order: if `mix.view != NULL` run
  `output_frame(mix)` (R11); otherwise free the mix (`obs_free_video_mix`), erase it from the array and continue with the same
  index. A mix without view is **not rendered**. (With S-042: canvases/encoders orphan their mix this way.)
* **S-058-R11** `output_frame(mix)`: `raw = raw_was_active`, `gpu = gpu_was_active`, `cur = cur_texture`,
  `prev = (cur == 0) ? 1 : cur-1`. Enter context; `render_video` (R12); if `raw`: `download_frame(prev)` (R16); `gs_flush`;
  leave context. If `raw && frame_ready`: pop one `obs_vframe_info` from `vframe_info_buffer`, set `frame.timestamp =
  info.timestamp`, `output_video_data(frame, info.count)` (R17). Finally `cur_texture = (cur+1) mod 2`. The raw entry is consumed
  **only when a frame is delivered**.
* **S-058-R12** `render_video`: `gs_begin_scene`; depth test off; cull `GS_NEITHER`; `render_main_texture` (R13). If
  `raw || gpu`: `output = render_output_texture` (R14); if `gpu`: planes come from `convert_textures_encode`
  (Windows: `copy_surfaces_encode`, 1 channel; other platforms the encode textures but the normal `copy_surfaces[cur]`
  and 3 channels); if `gpu_conversion`: `render_convert_texture` (R15); if `gpu`: `output_gpu_encoders` (R18); if `raw`:
  `stage_output_texture` (R16). End: `gs_set_render_target(NULL,NULL)`, blending on, `gs_end_scene`. Mixes with neither consumer
  still run the main render (R13) every frame.
* **S-058-R13** `render_main_texture`: set render target `render_texture` with `render_space`, clear colour to (0,0,0,0)
  with depth 1/stencil 0, depth test off, cull none, `gs_ortho(0, base_w, 0, base_h, -100, 100)`, viewport `(0,0,base_w,base_h)`.
  Under `draw_callbacks_mutex` call draw callbacks in **reverse** order with `(base_w, base_h)`. Then **reuse**: if some mix with
  a smaller index than this one has the same `view`, the same `render_space`, the same `ovi.base_width/height` and
  `texture_rendered == true`, draw that mix's `render_texture` (default effect, technique `Draw`, sRGB texture param `image`,
  framebuffer sRGB on during the draw, sprite of size 0x0 flip 0) instead of calling `obs_view_render`; else
  `obs_view_render(view)`. Then `texture_rendered = true`; then call rendered callbacks in **registration order** (under the
  same mutex). The draw/rendered callbacks run once **per mix per frame**.
* **S-058-R14** `render_output_texture`: let `(w,h)` = size of `output_texture`. If equal to `ovi.base_width/height` the
  `render_texture` is the output (no pass). Otherwise: effect = R14a; technique `Draw`; set render target `output_texture`,
  projection/viewport to `(w,h)`; if the effect has `base_dimension` set it to `(base_w,base_h)`, if it has `base_dimension_i`
  set `(1/base_w,1/base_h)`; set `image` to `render_texture` (sRGB-aware setter); framebuffer sRGB on, blending **off**; for
  each pass: `begin_pass`, `gs_draw_sprite(render_texture, 0, w, h)`, `end_pass`; blending back on, sRGB off; result is
  `output_texture`.
  * **R14a** effect choice: if `|base_w - w| <= 16 && |base_h - h| <= 16` -> `default`. Else if the **video output** size is under
    half the base size on **both** axes (`width < base_w/2 && height < base_h/2`, integer halves) -> `bilinear_lowres`.
    Else by `scale_type`: BILINEAR -> `default`, LANCZOS -> `lanczos`, AREA -> `area`, everything else (DISABLE, POINT, BICUBIC,
    unknown) -> `bicubic`. If the selected effect pointer is NULL: use `bicubic` if present, else `default`. (The "within 16"
    test uses the output *texture* size; the "half" test uses the output *video* size - the same in practice.)
* **S-058-R15** `render_convert_texture` (only if `gpu_conversion`), blending off, effect `conversion_effect`:
  `vec0 = color_matrix[4..7]`, `vec1 = color_matrix[0..3]`, `vec2 = color_matrix[8..11]`, `multiplier = sdr_white_level / 10000`
  (float arithmetic), `hdr_lw = hdr_nominal_peak_level`. If `convert_textures[0]` exists (otherwise nothing is drawn):
  plane 0: set `image=output texture`, `color_vec0`, `sdr_white_nits_over_maximum`, `hdr_lw`, render plane with `conversion_techs[0]`.
  If `convert_textures[1]` exists: `image`, `color_vec1`, **and `color_vec2` if there is no third plane**, `width_i`, `height_i`
  (`conversion_width_i/height_i`), multiplier, `hdr_lw`, render with `conversion_techs[1]`; if `convert_textures[2]` exists: `image`,
  `color_vec2`, `width_i`, `height_i`, multiplier, `hdr_lw`, `conversion_techs[2]`. A plane render = set target to the plane texture,
  projection/viewport to its size, run the technique's passes drawing one triangle (`gs_draw(GS_TRIS,0,3)`) per pass. Blending
  back on. `texture_converted = true` (even if plane 0 is absent).
* **S-058-R16** `stage_output_texture` (raw consumers): first unmap every entry of `mapped_surfaces[]` (set NULL). Without
  `gpu_conversion`: `gs_stage_texture(copy_surfaces[cur][0], output_texture)` if that surface exists; `active_copy_surfaces[cur][0] =
  that surface` (possibly NULL), `active[cur][1..2] = NULL`, `textures_copied[cur] = true`. With `gpu_conversion`, only if
  `texture_converted`: for `i < channel_count` stage `copy_surfaces[cur][i]` <- `convert_textures[i]` (skipping NULL surfaces)
  and set `active[cur][i]` to the surface (NULL allowed); `active[cur][i >= channel_count] = NULL`; `textures_copied[cur] = true`.
  `download_frame(prev)`: false when `textures_copied[prev]` is false; otherwise for each channel with a non-NULL
  `active[prev][c]` map it (`frame.data[c]`, `frame.linesize[c]`) and remember it in `mapped_surfaces[c]`; a map failure returns
  false (surfaces mapped so far stay mapped and are unmapped by the next stage). Consequence: a raw frame is delivered with
  **one frame of latency**; the frame right after activation delivers nothing.
* **S-058-R17** `output_video_data(frame, count)`: `video_output_lock_frame(mix.video, &out, count, frame.timestamp)`; if it
  fails the frame is dropped silently. If locked: GPU-converted -> R17a, else RGBX copy; then `video_output_unlock_frame`.
  RGBX copy: if input and output `linesize[0]` are equal, one copy of `linesize * output_height` bytes; else per row
  `width*4` bytes (rows advance by each side's line size).
  * **R17a** `set_gpu_converted_data` by **output video format** (table in section 4). A plane is copied with one memcpy when
    `plane_width == in_linesize == out_linesize` (input pointer advances by `width*height`), otherwise row by row (`plane_width`
    bytes per row, pointers advance by their own line sizes). Planes: I420 {Y, U, V}; NV12 and P010 {Y, UV}: if `input.linesize[1] != 0` the UV plane is read from surface 1, otherwise (single surface)
    UV is read from surface 0 **directly after the Y rows**, i.e. at `data[0] + height * linesize[0]`, still using `linesize[0]` as the input
    line size (Q-9); I444 {Y,U,V} full size; I010 {Y(2W), U(W), V(W)}; P216 {Y(2W), UV(2W) full height};
    P416 {Y(2W), UV(4W) full height}. All other formats (NONE, packed YUV, RGB, I412, I422, I210, I40A, I42A, YUVA, YA2L, AYUV, V210, R10L)
    copy nothing (frame still locked/unlocked).

### 5.4 GPU encode hand-off

* **S-058-R18** `output_gpu_encoders(mix, raw_active)`: nothing unless `texture_converted` **and** the gpu vframe buffer is
  non-empty. Pop one `obs_vframe_info`; under `gpu_encoder_mutex` call `queue_frame` until it returns 0:
  `duplicate = avail_queue.empty OR (encode_queue non-empty AND info.count > 1)`.
  * Duplicate: take the **newest** frame of the encode queue; if the queue is empty return false (stop). Else `frame.count += 1`,
    post the semaphore, return `--info.count`.
  * Otherwise: pop the oldest avail frame; if `raw_active || info.count > 1` GPU-copy `convert_textures_encode[0]` into `frame.tex`
    and `[1]` into `frame.tex_uv` (the second copy is omitted on Windows); else **swap**: the mix's `convert_textures_encode[0/1]`
    become the avail frame's textures and vice versa. `frame.count = 1`, `frame.timestamp = info.timestamp`, `frame.released = true`;
    push to the back of the encode queue; post the semaphore; return `--info.count`.
  * Windows additionally acquires/releases the keyed mutex (`lock_key`) and publishes the shared handle; on other platforms
    `handle = 0xFFFFFFFF`.
* **S-058-R19** (consequence, see Q-3/Q-4) an entry with `count = n` produces `n` posts: while the encode queue is non-empty and
  `count > 1` the newest queued frame's count grows; the last unit (count 1) always queues a new frame.

### 5.5 Displays, tasks, sleep, statistics

* **S-058-R20** `render_displays`: if `!data.valid` do nothing (no context switch). Otherwise enter the context and, under
  `displays_mutex`, call `render_display` for each display in list order; leave the context.
* **S-058-R21** `execute_graphics_tasks`: under `task_mutex` pop tasks FIFO; the mutex is **released while a task runs**
  (tasks may queue tasks, which then run in the same drain).
* **S-058-R22** After sleeping: `frame_time_ns = os_gettime_ns() - frame_start` measured before sleeping (frame_start taken at
  loop entry). `frame_time_total += frame_time`; `fps_total_ns += video_time - last_time` (both **after** the sleep); `fps_total_frames
  += 1`. When `fps_total_ns >= 1e9`: `video_fps = frames / (fps_total_ns/1e9)` (double), `video_avg_frame_time_ns =
  frame_time_total / frames` (integer division), then the three accumulators are zeroed.
* **S-058-R23** `video_sleep(video_time, interval)`: `t = video_time + interval`. If `os_sleepto_ns(t)` succeeds (deadline
  reached): `video_time = t`, `count = 1`. Otherwise `diff = (i64)(now - cur)` (so a clock behind `cur` is negative);
  `clamped = diff > interval ? diff : interval`; `count = clamped / interval` (integer); `video_time = cur + interval*count`.
  Then `total_frames += count`, `lagged_frames += count - 1`. Build `vframe_info{timestamp = cur (video_time before the update), count}`.
  **Encoder groups**: under `encoder_group_mutex`, for every weak encoder in `ready_encoder_groups`: release the weak ref, resolve it
  (skip dead ones); if the encoder has a group and, under `group.mutex`, `num_encoders_started >= group.encoders.count` and
  `start_timestamp == 0`, set `start_timestamp = video_time` (the **new** `video_time`). Release the strong ref. Clear the list.
  Then under `mixes_mutex`, for every mix: if `raw_was_active` push the info to `vframe_info_buffer`; if `gpu_was_active` push it
  to `vframe_info_buffer_gpu`. The info for frame N is therefore consumed by frame N+1 (one frame of latency).
* **S-058-R24** `stop_requested`: under `mixes_mutex`, returns true iff `video_output_stopped(mix.video)` holds for **every**
  mix; the loop returns `!stop_requested()`. See Q-1 for the empty case.
* **S-058-R25** `add_ready_encoder_group(enc)`: takes a weak reference (`obs_encoder_get_weak_encoder`) and appends it under
  `encoder_group_mutex`.

### 5.6 Thread entry

* **S-058-R26** `obs_graphics_thread`: marks the thread as the graphics thread; `interval = video_frame_interval_ns` (read once);
  `video_time = os_gettime_ns()`; sets the thread name; registers profiler root `obs_graphics_thread(%g<NBSP>ms)` (`interval / 1e6`,
  `%g`, NBSP = U+00A0) with the interval; seeds `rand()` with the wall clock; context = `{interval, 0, 0, 0, 0, last_time=0}`; loops
  until the loop function returns false. Windows additionally loads the WinRT helper module and pumps window messages each frame;
  macOS wraps each frame in an autorelease pool. Neither is part of the vectors.

### 5.7 GPU encode pool and thread

* **S-058-R30** `init_gpu_encoding`: clear the stop flag; reserve the avail deque; create `NUM_ENCODE_TEXTURES` (10) pairs: P010
  output -> `gs_texture_create_p010`, any other format -> `gs_texture_create_nv12`, both with `GS_RENDER_TARGET | GS_SHARED_KM_TEX`
  and the output video size; a NULL `tex` returns false immediately (earlier pairs stay queued). Each frame: `{tex, tex_uv, handle}`
  (`handle = 0xFFFFFFFF` off Windows, `count=0`) pushed to avail. Then create the counting semaphore (0), the manual-reset event, the
  thread; any failure returns false. Finally signal the event and set `gpu_encode_thread_initialized = true`; return true.
* **S-058-R31** `stop_gpu_encoding_thread`: only if initialised: set stop flag, post the semaphore, join, clear the flag. Idempotent.
* **S-058-R32** `free_gpu_encoding`: destroy semaphore and event (pointers set NULL); drain both deques destroying `tex` and `tex_uv`
  of every frame (so each texture is destroyed exactly once); free the deques. Idempotent.
* **S-058-R33** Encode thread: `interval = video_output_get_frame_time`. Loop `while sem_wait == 0`: exit if the stop flag is set;
  while `wait_frames (1) > 0` swallow the token (no pop). Else reset the "inactive" event; under `gpu_encoder_mutex` pop the **oldest**
  encode-queue frame `tf`, `timestamp = lock_key_in = tf.timestamp/lock_key`, `next_key = tf.lock_key`, `video_output_inc_texture_frames`,
  take a strong ref to each live GPU encoder. Per encoder (list order):
  1. packet template: `timebase_num = enc.timebase_num * enc.frame_rate_divisor`, `timebase_den = enc.timebase_den`.
  2. **group gate**: if the encoder has a group and `start_ts == 0`: continue (skip) unless `group.start_timestamp == timestamp`.
  3. **pair gate**: if `!first_received` and it has paired encoders: skip while any paired encoder that still resolves has
     `!first_received || first_raw_ts > timestamp`. Dead paired encoders are ignored.
  4. `video_pause_check(pause, timestamp)` true -> skip.
  5. **divisor**: `skip = counter++; if counter == divisor then counter = 0; if skip != 0 continue`.
  6. `handle_encoder_group_reconfigure_request`; if `reconfigure_requested`: clear it and call `info.update(data, settings)`.
  7. if `start_ts == 0` then `start_ts = timestamp`.
  8. `lock_count += 1; next_key = (lock_count == N) ? 0 : next_key + 1` where `N` is the number of encoders referenced for this frame **including skipped ones** (so if any encoder was skipped `next_key` never becomes 0).
  9. `fer_ts = now`; encode: `encode_texture2(data, {handle, tex[0]=tf.tex, tex[1]=tf.tex_uv, tex[2]=NULL}, cur_pts, lock_key, &next_key, &pkt, &received)`
     if present, else `encode_texture(data, handle, cur_pts, lock_key, &next_key, &pkt, &received)`.
  10. if `tf.timestamp != 0` append a packet-time record `{pts = cur_pts, cts = tf.timestamp, fer = fer_ts, ferc = success ? now : 0}`.
  11. `send_off_encoder_packet(enc, success, received, &pkt)`; `lock_key = next_key`; `cur_pts += timebase_num * frame_rate_divisor`.
  Skipped encoders (steps 2-5) do not count for `lock_count`, do not advance `cur_pts` and do not touch `lock_key`.
  After the encoders: under the mutex `tf.lock_key = next_key`; `--tf.count`: if still > 0, `tf.timestamp += interval`, push to the **front**
  of the encode queue and `video_output_inc_texture_skipped_frames`; else push to the back of avail. Signal the event, release refs.
  The thread returns when the wait fails or the stop flag is set.

### 5.8 Linux platform glue

* **S-058-R40** Platform selector: default platform is `X11_EGL (1)`, display pointer NULL. `obs_set_nix_platform(INVALID)` aborts via
  assertion (debug builds; not exercised). `get_module_extension()` = `".so"`.
* **S-058-R41** `find_libobs_data_file(file)` tries in order and returns the first path that exists (as a new string, else NULL):
  1. `<OBS_DATA_PATH>/libobs/<file>` (relative to the working directory), 2. `<executable dir>/../<OBS_DATA_PATH>/libobs/<file>`
  (`<exe dir>/` is `os_get_executable_path_ptr("../<OBS_DATA_PATH>/libobs/")`), 3. only if `OBS_INSTALL_PREFIX` is non-empty:
  `<OBS_INSTALL_PREFIX>/<OBS_DATA_PATH>/libobs/<file>`. `file` is appended verbatim (empty string -> the directory itself).
* **S-058-R42** `log_system_info` (info level, in order): CPU name/speed per physical CPU from `/proc/cpuinfo` (silently nothing if
  unreadable), `Physical Cores: %d, Logical Cores: %d`, `Physical Memory: <total>MB Total, <free+buffers>MB Free`, `Kernel Version: <sysname> <release>`;
  then if `/.flatpak-info` exists the Flatpak block (Branch, Arch, Commit, Runtime, `App Extensions:`/`Runtime Extensions:` with
  ` - <name>` per entry without the `=commit`, `Flatpak Framework Version`; error log `Unable to open .flatpak-info file` when it cannot be
  opened), else `Distribution: <NAME> <VERSION_ID>` (`Unknown` defaults, `Distribution: Missing /etc/os-release !`); then
  desktop session: both `XDG_CURRENT_DESKTOP` and `XDG_SESSION_DESKTOP` -> `Desktop Environment: <cur> (<session>)`, exactly one ->
  `Desktop Environment: <that>`, none -> no line; `XDG_SESSION_TYPE` -> `Session Type: <t>`. Finally, for the X11 platform:
  no display -> `Unable to open X display`; else `Window System: X<proto>.<rev>` + `, Vendor: <v>, Version: <rel/10^7>.<(rel/10^5)%100>.<(rel/1000)%100>`
  when the vendor string contains `X.Org`, else ` - vendor string: <v> - vendor release: <rel>`. The Wayland platform logs nothing extra.
* **S-058-R43** Hotkey dispatch: `obs_hotkeys_platform_init` selects the X11 vtable for `X11_EGL` (the Wayland vtable only if built with
  Wayland support) and forwards; `..._free` forwards then forgets the vtable; `is_pressed`, `obs_key_to_str`, `obs_key_from_virtual_key`,
  `obs_key_to_virtual_key` forward to the vtable. After `free` the forwarders MUST NOT be called (D-1).
* **S-058-R44** X11 init: `XOpenDisplay(NULL)` (own connection; failure -> return false, no context). Allocate the zeroed platform context;
  register for raw button press/release (event mask `0x18000`, all master devices) on the **root window of the default screen** (0 when the
  screen index does not exist) and flush; fill `base_keysyms[key]` for every key in `[0, OBS_KEY_LAST_VALUE)` from the static table
  (R45); read the keyboard mapping for `[min_keycode, max_keycode]`, remember `min_keycode`, a copy of the keysym array, `syms_per_code`
  and `num_keysyms = (max-min+1)*syms_per_code`; then for each keycode, scanning its keysyms in column order: a zero keysym ends the
  scan; `Super_L` -> `super_l_code = code`, end; `Super_R` -> `super_r_code = code`, end; otherwise `key_from_base_keysym(sym)` (linear
  search of `base_keysyms` for the lowest obs key with an equal keysym, then aliases `Shift_R->SHIFT`, `Control_R->CONTROL`, `Alt_R->ALT`);
  a hit appends the keycode to that key's list (debug log `found alternate keycode %d for %s which already has keycode %d` for the 2nd+)
  and ends the scan. A failed mapping request logs warning `xcb_get_keyboard_mapping_reply failed` and leaves tables empty; init still
  returns true. Returns true after opening.
* **S-058-R45** Static tables: `base_keysym(key)`: letters A..Z -> `XK_A..XK_Z` (upper case), digits -> `XK_0..9`, numpad -> `XK_KP_*`,
  function keys F1..F35, arrows/navigation, modifiers L variants (`Shift_L, Control_L, Alt_L`; `META` has **no** base keysym = 0), punctuation
  and Latin-1 symbols, media/volume `XF86XK_*`, mouse buttons MOUSE1..29 -> `(n << 16)`, unknown keys 0. (The table has ~500 entries in
  `obs-nix-x11.c`; vectors check representatives. An implementation MUST reproduce the table from the X11 keysym definitions, see section 11.)
* **S-058-R46** `obs_hotkeys_platform_free` (X11): free every keycode list and the keysym copy, close the display, free the context, set
  `platform_context = NULL`; NULL context is a no-op.
* **S-058-R47** `is_pressed`: for `MOUSE1..MOUSE29` use R48; else query the keymap (`xcb_query_keymap`): on error log warning
  `xcb_query_keymap failed` and return false; `META` -> `Super_L code down || Super_R code down`; otherwise any learned keycode down
  (`keys[code/8] & (1 << code%8)`); no keycode -> false.
* **S-058-R48** Mouse polling (XInput): clear `pressed[]`/`update[]` (33 entries); drain **all** queued events: ignore non-generic events;
  generic `RAW_BUTTON_PRESS`: if `detail < 33` set `pressed[detail-1] = update[detail-1] = true` else warn `Unsupported button`;
  `RAW_BUTTON_RELEASE`: if `detail < 33` set `update[detail-1]`, else the same warning; other generic types ignored. Answer for the key:
  `MOUSE1 -> index 0`, `MOUSE2 -> 2` (X button 3), `MOUSE3 -> 1` (X button 2), `MOUSE4..MOUSE29 -> index 7..32` (i.e. X buttons 4..7 wheel
  axes are skipped): `ret = pressed[i] || button_pressed[i]`. **After** computing the answer: for every `i` with `update[i]`,
  `button_pressed[i] = pressed[i]`. (Without XInput support the build falls back to `xcb_query_pointer` masks for MOUSE1..3; not
  vectored.)
* **S-058-R49** `obs_key_to_str` (X11), result replaces/fills `dstr`: (a) `MOUSE1..29`: translation (`obs->hotkeys.translations[key]`) else
  `"Mouse N"` (N = 1-based index); (b) `NUM0..NUM9`: translation else `"Numpad N"`; (c) fixed list with default English text via
  `obs_get_hotkey_translation(key, default)` (copy): Insert, Delete, Home, End, "Page Up", "Page Down", "Num Lock", "Scroll Lock",
  "Caps Lock", Backspace, Tab, Print, Pause, Left, Right, Up, Down, Shift, Alt, Control, META -> "Super", Menu, "Numpad *", "Numpad +",
  "Numpad -", "Numpad ,", "Numpad .", "Numpad /", Space, Escape; (d) `F1..F35` -> `"F<n>"` (printf into the dstr, replaces); (e) otherwise for each
  learned keycode in order build a key event and call `XLookupString` (lookup with root = default screen root); the first non-empty
  result is upper-cased and **copied (`ncopy`) over** the dstr and stops; (f) if still empty and `key != NONE` copy the obs key name
  (`obs_key_to_name`, e.g. `OBS_KEY_B`). See Q-8 for pre-filled destinations.
* **S-058-R50** `obs_key_from_virtual_key(sym)`: 0 -> NONE; else find the first index `i` of the **copied keysym table** equal to
  `sym`; keycode = `min_keycode + i / syms_per_code`; result = the obs key whose learned keycode list contains it (lowest key index),
  NONE if none. So shifted symbols map to the physical key (`!` -> `OBS_KEY_1`), and `Super_L` -> NONE (never added to a list).
* **S-058-R51** `obs_key_to_virtual_key(key)`: `META` -> `XK_Super_L (0xffeb)`; else `base_keysyms[key]` (0 if none).
* **S-058-R52** `obs_key_combination_to_str(comb)`: append, separated by `" + "`, in this fixed order: Control (if `INTERACT_CONTROL_KEY=4`),
  META (`INTERACT_COMMAND_KEY=0x80`), Alt (`8`), Shift (`2`), then `comb.key` if not NONE; each part is `obs_key_to_str`; an empty part adds
  nothing (no separator). Other modifier bits (caps `1`, mouse buttons, ...) are ignored. The dstr is appended to (existing content kept
  and the first part gets a separator).

## 6. Algorithms

```
frame(ctx):
  update_active_states()
  begin_frame()                                   # in context
  last = tick_sources(video_time, ctx.last_time)  # ctx.last_time := last (reverse tick callbacks, snapshot, tick)
  for mix in mixes (index based, mixes may shrink): view? output_frame(mix) : free+erase
  render_displays() ; run_tasks()
  ft = now - start
  video_sleep()                                   # may change video_time, counters, vframe buffers
  stats update ; return not all_outputs_stopped
output_frame(mix): render_video ; if raw: ready = download(prev) ; flush ; if raw and ready: deliver(pop vframe) ; cur ^= 1
video_sleep: if sleep_until(cur+iv) {vt=cur+iv; n=1} else {d=(i64)(now-cur); n=max(d,iv)/iv; vt=cur+iv*n}
             total+=n; lagged+=n-1; mark groups; push {cur,n} to every active consumer buffer
queue_frame(mix, raw, info):
  if avail.empty or (queue.nonempty and info.count>1): if queue.empty: return false; queue.newest.count++; post; return --info.count
  f = avail.pop ; if raw or info.count>1: copy planes else swap planes ; f.count=1 f.ts=info.ts ; queue.push(f) ; post ; return --info.count
```

## 7. Concurrency

The graphics thread is the only writer of `video_time`, counters, `cur_texture`, `texture_*` flags. Cross-thread: `mixes_mutex`
(guards the mix array and the vframe buffers, held across the whole `output_frames` and the end of `video_sleep`), `draw_callbacks_mutex`
(tick/draw/rendered callback arrays, held while the callbacks run - callbacks MUST NOT add/remove callbacks), `sources_mutex` (only for the
snapshot, not during ticks), `displays_mutex`, `encoder_group_mutex`, `task_mutex` (released while a task runs), per mix `gpu_encoder_mutex`
(both encode queues and the encoder list) and `group.mutex`. `raw_active` / `gpu_encoder_active` are atomics written by consumers, read once
per frame. The GPU encode thread is woken only through the semaphore; `gpu_encode_inactive` is signalled while idle so that other threads may
wait for it. The graphics context is entered by the loop only around begin_frame, each mix and the display pass, and **never held across
the sleep**.

## 8. Reference defects and quirks

* **Q-1 (QUIRK)** With **no mixes** `stop_requested()` is vacuously true: the loop returns false after one executed frame (ticks run,
  counters advance). Conforming code SHOULD reproduce this; S-042 never calls the thread without a main mix. Script `11_stop_and_empty`.
* **Q-2 (QUIRK)** `texture_converted` is only reset by the *base reset* (R02); it stays true after the first conversion even if
  `gpu_conversion` is later cleared. Script `20_gpu_queue_frame`.
* **Q-3 (QUIRK/leak)** A mix with a GPU consumer but `gpu_conversion == false` never sets `texture_converted`, so its gpu vframe entries
  are queued every frame and never consumed (unbounded growth while active). Script `20_gpu_queue_frame` (last section). Required:
  reproduce the visible behaviour (no hand-off); a Rust port MAY cap the buffer.
* **Q-4 (QUIRK)** An entry `count = n >= 2` on an **empty** encode queue creates two real frames (copy path, same timestamp) instead of one
  frame with count 2. Script `20_gpu_queue_frame`.
* **Q-5 (QUIRK)** Mouse release is observed **one poll late** (answer computed before the latch update). Script `35_x11_is_pressed`.
* **Q-6 (QUIRK)** A press and a release drained in the same poll leave the button latched pressed until another release event. Script `35`.
* **Q-7 (QUIRK)** Button detail 33 is rejected (`< 33`) although slot 32 exists, so `OBS_KEY_MOUSE29` (slot 32) can never become pressed.
  Script `35`.
* **Q-8 (QUIRK)** `obs_key_to_str` on a **pre-filled** dstr: named keys, F-keys and mouse/numpad rules *replace* the content; the keycode
  lookup replaces it on success; if the key has no usable keycode and the dstr is non-empty the obs-name fallback is **not** applied (content
  kept). Script `36_x11_key_to_str`.
* **Q-9 (QUIRK)** NV12/P010 single-surface case: UV offset is `height * linesize[0]` from the start of surface 0 (in both copy strategies);
  the UV rows use `linesize[0]` as input stride. Script `19`.
* **D-1 (DEFECT)** `obs_hotkeys_platform_init` with `OBS_NIX_PLATFORM_WAYLAND` in a build **without** Wayland support, and any forwarder
  (`is_pressed`, `obs_key_to_str`, `obs_key_from/to_virtual_key`) after `obs_hotkeys_platform_free`, dereference a NULL vtable (crash). Required:
  init returns false; forwarders return false / empty / `OBS_KEY_NONE` / 0. Not scriptable (crash); not covered.
* **D-2 (DEFECT)** X11 mouse press with `detail == 0` indexes `pressed[-1]` (`detail - 1`). Required: ignore the event. Not scripted (UB).
* **D-3 (DEFECT)** `fill_keycodes` returns `true` on **error** (inverted), ignored by the caller; no observable effect. Required: none.
* **D-4 (DEFECT, theoretical)** The GPU encode thread pops the encode queue for every non-swallowed token; if a consumer posts the semaphore without a
  queued frame (not done by the reference producers) it reads an empty queue. Required: ignore tokens when the queue is empty.

## 9. Implementation notes (non-normative)

Keep the loop a plain function over a context struct so that a deterministic clock/sleep can be injected (vectors do this). Model `video_output`,
`gs_*`, sources and encoders as traits; the vectors need only the call order and arguments listed in the rules. Use `Arc<Mutex<>>` per mix
for the encode queues and `AtomicI64` for the activity counters. Windows differences (not vectored): keyed-mutex `lock_key` handling,
`copy_surfaces_encode`/1-channel staging, shared handles, WinRT capture thread, message pump. macOS: autorelease pool around each frame.
The X11 key table is mechanical: generate it from the X11 keysym headers with a build script and test with the keymap scripts.

## 10. Conformance coverage

Runner commands are documented in `test/conformance/README.md` ("obsvideo runner"). All vectors run the REAL C files.

| Rules | Scripts |
|---|---|
| R01, R03, R04, R10, R20, R21 | `10_frame_basic`, `11_stop_and_empty` |
| R02 | `14_active_state` |
| R13 | `15_render_main_and_reuse` |
| R14 | `16_output_scaling` |
| R15, R16 | `18_gpu_conversion`, `17_raw_cpu_path` |
| R11, R16, R17 | `17_raw_cpu_path`, `19_gpu_download_formats` |
| R17a, Q-9 | `19_gpu_download_formats` |
| R18, R19, Q-2, Q-3, Q-4 | `20_gpu_queue_frame` |
| R22, R23, R24 | `12_timing_lag`, `13_fps_stats`, `11_stop_and_empty` |
| R25, R23 (groups) | `23_encoder_groups` |
| R26 | `24_thread_entry` |
| R30, R31, R32 | `22_gpu_init_free` |
| R33 | `21_gpu_encode_thread` |
| R40 | `30_nix_platform_state` |
| R41 | `31_nix_find_data_file` |
| R42 | `32_nix_system_info` |
| R43, R44, R45 (sample), R46 | `33_x11_hotkey_platform_init`, `34_x11_virtual_keys` |
| R47, R48, Q-5, Q-6, Q-7 | `35_x11_is_pressed` |
| R49, Q-8 | `36_x11_key_to_str` |
| R50, R51 | `34_x11_virtual_keys` |
| R52 | `37_key_combination_str` |
| D-1..D-4 | not scripted (crash/UB) |

## 11. Open questions

* The Rust sufficiency check was **not performed** (cost control, per assignment). Rules were validated only against the C reference.
* `obs-nix-wayland.c` (1630 lines: Wayland registry, xkbcommon keymap, `wl_*` listeners, `key_to_str`/`is_pressed` backend) is not specified and
  not linked: it needs a Wayland compositor, libxkbcommon and generated protocol code. A follow-up spec is required.
* The full `get_keysym` table (about 500 keys) is only sampled by the vectors; an implementation must be checked against the X11 headers.
* Iteration order of `data.sources` when ticking is the order of the UUID hash table (unspecified); vectors use the fake's order
  (most recently added first). Implementations MUST NOT depend on it.
* The non-XInput fallback for mouse polling (`xcb_query_pointer`) and the empty-`OBS_INSTALL_PREFIX` branch of `find_libobs_data_file` are
  compiled out / skipped in the vectors (build flag and the generated `obsconfig.h`); behaviour is stated from reading only.
* Behaviour with `os_sleepto_ns` using a real clock (jitter) is not vectored; only the arithmetic of R23 is.
