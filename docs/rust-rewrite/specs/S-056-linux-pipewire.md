# S-056 — Linux PipeWire plugin (`plugins/linux-pipewire`)

Status: complete for the behaviour reachable through the scripted fakes (see "Not linked / not covered").
Reference files (all linked and run by `test/conformance/gen/pipewire_runner.c`): `plugins/linux-pipewire/formats.c`, `formats.h`,
`portal.c`, `portal.h`, `screencast-portal.c`, `screencast-portal.h`, `camera-portal.c`, `camera-portal.h`, `pipewire.c`, `pipewire.h`,
`linux-pipewire.c`. Related: S-049 (other Linux capture plugins), S-032/S-042 (source/property/data APIs this plugin consumes).
Rust sufficiency check: **skipped by instruction** (cost control); the vectors have only been produced by the C reference.

## 1. Overview

The module registers up to four OBS input sources:

| id | flags | role |
|---|---|---|
| `pipewire-camera-source` | `ASYNC_VIDEO` (0x5), icon camera(9) | camera through the Camera portal |
| `pipewire-desktop-capture-source` | `VIDEO\|DO_NOT_DUPLICATE\|CAP_OBSOLETE` (0x481), icon desktop(6) | monitor capture |
| `pipewire-window-capture-source` | same (0x481), icon window(7) | window capture |
| `pipewire-screen-capture-source` | `VIDEO\|DO_NOT_DUPLICATE` (0x81), icon desktop(6) | unified monitor+window, name key `PipeWireDesktopCapture` |

Layers: **portal** (`portal.c`, D-Bus request/session path helpers and Response-signal subscription) → **screencast / camera
portal** (flows that yield a PipeWire remote fd) → **stream engine** (`pipewire.c`: connection, format negotiation, buffer
processing, render) → **formats** table. All user-visible texts are locale keys (`obs_module_text(KEY)`).

## 2. Data formats

### 2.1 Supported video format table (`obs_pw_video_format_from_spa_format`)
Lookup by SPA video format id (linear scan; unknown, `UNKNOWN`(0), `ENCODED`(1) → false and out param untouched; out param may be NULL).

| SPA id (name) | DRM fourcc | GS format | OBS video format | swap R/B | bpp | pretty name |
|---|---|---|---|---|---|---|
| 12 BGRA | ARGB8888 | BGRA | BGRA | no | 4 | ARGB8888 |
| 11 RGBA | ABGR8888 | RGBA | RGBA | no | 4 | ABGR8888 |
| 8 BGRx | XRGB8888 | BGRX | BGRX | no | 4 | XRGB8888 |
| 7 RGBx | XBGR8888 | BGRX | NONE | **yes** | 4 | XBGR8888 |
| 4 YUY2 | YUYV | UNKNOWN | YUY2 | no | 2 | YUYV422 |
| 23 NV12 | NV12 | UNKNOWN | NV12 | no | 2 | NV12 |
| 85 ABGR_210LE | ABGR2101010 | R10G10B10A2 | NONE | no | 10 | ABGR2101010 |
| 81 xBGR_210LE | XBGR2101010 | R10G10B10A2 | NONE | no | 10 | XBGR2101010 |

(The two 10-bit rows exist when built against PipeWire >= 0.3.41.) `bpp` is only used for the async linesize (R27), the value 10 for
10-bit rows is as-is. SPA ids are the numeric values of `spa_video_format`; `ENCODED` = 1 is also used internally by the camera.

### 2.2 D-Bus strings
Request path `/org/freedesktop/portal/desktop/request/<SENDER>/obs<N>`, session path `/org/freedesktop/portal/desktop/session/<SENDER>/obs<M>`;
tokens are `obs<N>` / `obs<M>`. `<SENDER>` = unique bus name without its first char with every `.` → `_` (`:1.42` → `1_42`, `:1.2.3` → `1_2_3`).

### 2.3 PipeWire pods emitted (worked examples; printed by the fakes)
EnumFormat object (type 0x40003, id 3, i.e. `SPA_PARAM_EnumFormat`), properties in this order:
`mediaType=Id video(2)`, `mediaSubtype=Id raw(1)`, `format=Id <spa id>`, [`modifier` flags MANDATORY|DONT_FIXATE (0x18) = Choice Enum of Long],
`size=Choice Range<default,min,max>` (Rectangle), `framerate=Choice Range<default,min,max>` (Fraction).
Example (no overrides, 1920x1080@60 video config):
`Object(type=0x40003,id=3){mediaType=Id:2(video); mediaSubtype=Id:1(raw); format=Id:12(BGRA); size=Choice:Range<Rect:1920x1080,Rect:1x1,Rect:8192x4320>; framerate=Choice:Range<Frac:60/1,Frac:0/1,Frac:360/1>}`.
Modifier choice example: `modifier[flags=0x18]=Choice:Enum<Long:0,Long:0,Long:72057594037927937>` — the **first element repeats modifiers[0]**
(preferred value) and then the full list follows.

## 3. Rules

### Portal helpers
- **S-056-R01** `portal_create_request_path/session_path` increment a process-global counter per call and produce `obs<counter>`; request and session counters
  are independent; token and path of one call share the number. Sender name per 2.2.
- **S-056-R02** `portal_signal_subscribe(path, cancellable, cb, ud)` subscribes (`org.freedesktop.portal.Desktop`, interface `org.freedesktop.portal.Request`,
  member `Response`, object path = request path, no arg0, flags `NO_MATCH_RULE`=1). On the first Response the callback runs with the signal parameters, then
  the subscription is dropped. If the cancellable fires first: log INFO `[portals] Request cancelled`, call `Request.Close` on the request path (no params, no reply
  handler), then drop the subscription. A subscription is also dropped from its own callback (unsubscribe is observable after the callback ran / before in call order of the fakes).
- **S-056-R03** If the session bus cannot be obtained a WARNING `[portals] Error retrieving D-Bus connection: <msg>` is logged and the connection stays NULL (retried on the next use);
  proxy creation with a NULL connection yields NULL silently. The ScreenCast/Camera proxy is created once (WARNING `[portals] Error retrieving D-Bus proxy: <msg>` on failure, retried later).

### Screencast sources
- **S-056-R04** Registration (`screencast_portal_load`): read cached `AvailableSourceTypes` of the proxy (0 if no proxy/property). 0 → INFO `[pipewire] No capture sources available`, nothing registered.
  Else INFO `Available capture sources:` + `    - Monitor source` (bit 1) / `    - Window source` (bit 2) lines; monitor and window sources are registered individually;
  the unified source is registered whenever at least one type exists. `screencast_portal_unload` releases the proxy.
- **S-056-R05** Defaults: `ShowCursor=true`, `RestoreToken` = empty. Properties: button `Reload` (label key `PipeWireSelectMonitor` / `PipeWireSelectWindow` / `PipeWireSelectScreenCast` by capture type;
  callback = R14) then checkbox `ShowCursor` (description key `ShowCursor`). `get_name`: desktop & unified → `PipeWireDesktopCapture`, window → `PipeWireWindowCapture`.
  Capture type values requested from the portal: monitor 1, window 2, unified 3.
- **S-056-R06** Creation stores `ShowCursor`, a copy of `RestoreToken`, then (no bus/proxy → source stays inert, FALSE) logs INFO `PipeWire initialized` and starts R07.
- **S-056-R07** Flow, each step started only by the (zero) Response of the previous one: (1) `CreateSession({handle_token, session_handle_token})`; Response → INFO `[pipewire] Screencast session created`,
  `session_handle` stored; (2) `SelectSources(session, {types:u, multiple:false, handle_token, cursor_mode:u, [persist_mode:u=2, restore_token:s]})`; Response → DEBUG `Response to select source received`;
  (3) INFO `[pipewire] Asking for <monitor|window|monitor and window>`, `Start(session, "", {handle_token})`; Response → R10; (4) `OpenPipeWireRemote(session, {})` with fd list → R11.
  Dict key order is exactly as listed. Request subscription for step N+1 is created *before* its call is issued. Each step subscribes with the source's cancellable.
- **S-056-R08** `cursor_mode`: if available cursor modes (cached `AvailableCursorModes`, 0 if absent) contain METADATA(4) → 4; else if EMBEDDED(2) is available **and** `ShowCursor` → 2; else HIDDEN(1).
  The `ShowCursor` value used is the one captured at creation/update time.
- **S-056-R09** `persist_mode=2` and (if the stored token is non-empty) `restore_token` are added only when the cached portal property `version` >= 4 (absent = 0).
- **S-056-R10** Start response: code != 0 → WARNING `[pipewire] Failed to start screencast, denied or cancelled by user`, stop. Otherwise `streams` array: if it does not contain exactly 1 entry log WARNING
  `[pipewire] Received more than one stream when only one was expected. This is probably a bug in the desktop portal implementation you are using.` and use the **last** entry; node id = first field (u). If portal version >= 4: the stored token is
  dropped, replaced by `restore_token` of the reply if present (else NULL), then `obs_source_save` is called (host runs the save callback: R13). Then INFO `[pipewire] source selected, setting up screencast` and R07 step 4.
- **S-056-R11** OpenPipeWireRemote reply `(h)`: fd from the fd list, `obs_pipewire_connect_fd(fd, NULL, NULL)`; if it succeeds `obs_pipewire_connect_stream(node, {stream_name "OBS Studio",
  properties media.type=Video, media.category=Capture, media.role=Screen, screencast.cursor_visible})`. Errors in any async reply are logged ERROR (`[pipewire] Error creating screencast session: <msg>`,
  `Error selecting screencast source: <msg>` — **also used for SelectSources and Start failures**, `Error retrieving pipewire fd: <msg>`) unless the error is G_IO_ERROR_CANCELLED (silent); the flow then stops.
  A denied (non-zero) Response logs WARNING `[pipewire] Failed to create session, denied or cancelled by user` / `Failed to select source, denied or cancelled by user` / (R10).
- **S-056-R12** `update`: stores `ShowCursor` and forwards to the stream (`cursor.visible`). `show`/`hide`: `pw_stream_set_active(true/false)` under the thread-loop lock if a stream exists. `get_width/height`: engine size (R29) or 0 without stream. `video_render`: engine render (R28).
- **S-056-R13** `save`: writes `RestoreToken` = current stored token (NULL when none → the host stores the empty string).
- **S-056-R14** Reload button: drop stored token, destroy stream, destroy the PipeWire connection; if a session exists log DEBUG `[pipewire] Cleaning previous session <handle>`, call `Session.Close` on it (no reply handler), forget it; restart R06 (fresh tokens). Returns false. The `RestoreToken` *setting* is not touched until the next save.
- **S-056-R15** Destroy order: `Session.Close` (if session), free token, destroy stream, destroy connection, cancel the cancellable (pending Request subscriptions are closed per R02), free.

### Stream engine (`pipewire.c`)
- **S-056-R16** `obs_pipewire_connect_fd`: thread loop "PipeWire thread loop", context, start (failure: WARNING `Error starting threaded mainloop`, returns NULL); `pw_context_connect_fd(dup(fd, >=5, CLOEXEC))` (failure: WARNING `Error creating PipeWire core: <strerror>`, NULL);
  core listener, `pw_core_sync` + wait (the core `info` event gives the server version, the `done` event with matching id/seq wakes the waiter); registry (with listener) only if registry events were passed (INFO `[pipewire] Created registry <p>`). Returns the connection.
  `info`: INFO `Server version: <v>`, `Library version: <v>`, `Header version: <v>`; `sscanf("%d.%d.%d")` must match 3 values else WARNING `[pipewire] failed to parse server version` — **but the parsed prefix is kept** (Q-2). A missing `info` event leaves 0.0.0. `core error` event: ERROR `[pipewire] Error id:%u seq:%d res:%d (<strerror(res)>): <msg>` and wake waiter. `done` for other ids/seqs is ignored.
- **S-056-R17** Version check helper `v >= (a,b,c)` is lexicographic on (major, minor, micro). Gates: modifiers in params need server >= 0.3.33; DmaBuf buffers without modifier in the format need >= 0.3.24; VideoTransform meta needs >= 0.3.62; modifier removal strategy differs below 0.3.40 (R22).
- **S-056-R18** Format list (`init_format_info`): with a `selected_format` (camera) → exactly that spa/drm format, no modifiers. Else by source output flags: not VIDEO → empty list; `ASYNC` bit set → [RGBA, YUY2], no modifiers, no graphics access; else (graphics): for [BGRA, RGBA, BGRx, RGBx, ABGR_210LE, xBGR_210LE]
  (table lookup; entries with GS format UNKNOWN skipped) in graphics context: query dmabuf capabilities (flags, DRM format list); a format whose DRM format is not advertised (or the query failed) gets an empty modifier list; otherwise modifiers = backend modifiers for that DRM format (if query ok) followed by `DRM_FORMAT_MOD_INVALID` (0x00ffffffffffffff) when `IMPLICIT_MODIFIERS_SUPPORTED` (flag bit 0) is set.
- **S-056-R19** Params (`build_format_params`): empty format list → ERROR `[pipewire] No format found while building param pointers`, connect_stream returns NULL (stream not created). If server >= 0.3.33: one EnumFormat per format with a non-empty modifier list (modifier choice per 2.3), in list order; then, always, one plain EnumFormat per format (no modifier). Framerate: if set by the caller `default=min=max=that`; else default = video fps (num/den), min 0/1, max 360/1. Size: if set `default=min=max`; else default = output size, min 1x1, max 8192x4320.
  `obs_pipewire_connect_stream` then connects with direction INPUT, the node id, flags `AUTOCONNECT|MAP_BUFFERS` (0x5), logs INFO `Created stream <p>` / `Playing stream <p>`, registers a loop event (DEBUG `registered event <p>`) used for renegotiation (R22/R24).
- **S-056-R20** `param_changed`: ignored unless param != NULL, id == Format, media type video and subtype raw. Otherwise parse the raw format; `buffer_types = MemPtr` plus `DmaBuf` when (the Format pod has a modifier property **or** server >= 0.3.24) **and** the source is not exactly `ASYNC_VIDEO` (flags & 0x5 != 0x5); in that case (and only then) graphics is asked for `gs_query_sync_capabilities` (explicit sync, PW >= 1.2).
  Logs INFO `Negotiated format:`, `    Format: <id> (<spa name>)`, [`    Modifier: 0x<hex>` if the modifier property exists], `    Size: WxH`, `    Framerate: n/d`.
  Replies with `update_params` in this order: (a) Meta VideoCrop size 16; (b) Meta Cursor size Choice Range<16432, 52, 4194352>; (c) if explicit sync: Buffers{dataType=DmaBuf(1<<3=8), metaType (MANDATORY flag 0x8)=1<<SyncTimeline=512};
  (d) Buffers{dataType=buffer_types} (value 2 = MemPtr, 10 = MemPtr|DmaBuf); (e) if explicit sync: Meta SyncTimeline size 24; (f) Meta Header size 32; (g) if server >= 0.3.62: Meta VideoTransform size 4. Sets `negotiated`.
- **S-056-R21** State change: INFO `[pipewire] Stream <p> state: "<pw_stream_state_as_string>" (error: <text|none>)`.
- **S-056-R22** Renegotiation event: INFO `[pipewire] Renegotiating stream`, under lock rebuild params (R19) from the *current* format list/framerate/resolution and `update_params`; if params cannot be built tear the PipeWire connection down (thread loop stop, registry/context/loop destroyed, fd closed).
- **S-056-R23** Sync process (graphics sources; runs inside the graphics context): dequeue **all** buffers, keep the last, return each older one unused (`pw_stream_queue_buffer`; with explicit sync and a trailing SyncObj datas + SyncTimeline meta, first signal its release point). No buffer → DEBUG `[pipewire] Out of buffers!`.
  Header meta with `CORRUPTED`(1<<1) → ERROR `[pipewire] buffer is corrupt`, return buffer, nothing else. Chunk size 0 on data 0 (kwin/mutter workaround) → skip texture but still read cursor. DmaBuf data: planes = number of leading DmaBuf datas; unsupported format or GS format UNKNOWN → ERROR `unsupported DMA buffer format: <id>`;
  per-plane fd/offset/stride; modifier array = negotiated modifier unless it is MOD_INVALID (then none); any plane chunk with `CORRUPTED`(1<<0) → DEBUG `buffer contains corrupted data`, keep the old texture; else replace texture by `gs_texture_create_from_dmabuf(w,h,drm,GS_BGRX,planes,...)`.
  Memory data: DEBUG `Buffer has memory texture`; unsupported/GS-UNKNOWN format → ERROR `unsupported buffer format: <id>`; corrupted flag → DEBUG; size 0 → DEBUG `buffer contains empty data`; else replace texture by `gs_texture_create(w,h,gs_format,1,&data0,GS_DYNAMIC)`. `swap_red_blue` formats (RGBx) get GL swizzle B←RED, R←BLUE. Then crop/transform/cursor metadata (R25); the buffer is always queued back at the end.
- **S-056-R24** DMA-BUF import failure (NULL texture): remove the negotiated modifier from that format's list (server >= 0.3.40: every occurrence of that value; older: all entries except the last, since the erase range is `[0, n-1)`), signal the renegotiation event (R22). Old texture was already destroyed.
- **S-056-R25** Metadata: VideoCrop valid when both extents non-zero (stored crop position/size, else crop invalid); VideoTransform value stored (absent → None). Effective crop = valid and (x≠0 or y≠0 or w < frame w or h < frame h). Cursor: valid when id ≠ 0 (cursor meta present); when `cursor.visible` and valid: if a bitmap is present its old texture is dropped; if the bitmap is non-empty and its format is in the table with a GS format a new texture (`GS_DYNAMIC`, pixels at bitmap offset, swizzled for RGBx) is created and hotspot/size stored; position is always stored. Without bitmap the previous cursor texture is kept. When not visible nothing of this is read.
- **S-056-R26** Explicit sync (PW >= 1.2): before creating the texture, if a release fd is held and its point was not yet exported by a render, signal it; close both held fds. If the SyncTimeline meta exists and `n_datas == planes + 2`, dup (`F_DUPFD_CLOEXEC`, >=5) the two SyncObj fds (acquire = data[planes], release = data[planes+1]) with the meta's points and mark `sync.set`; else `sync.set=false`.
- **S-056-R27** Async process (sources with the `ASYNC` bit, e.g. cameras): latest buffer, `chunk.size == 0` → skipped; frame width/height from the negotiated size; colorimetry: matrix RGB/other→`VIDEO_CS_DEFAULT`, BT601→601, BT709→709; range 0_255→FULL, 16_235→PARTIAL, else DEFAULT (via `video_format_get_parameters`); format must be in the table with an OBS video format ≠ NONE (else ERROR `[pipewire] Couldn't prepare frame`); `linesize[0] = round_up4(width*bpp)`; each of the first `min(n_datas, 8)` data pointers is a plane, a NULL pointer → ERROR `[pipewire] Failed to access data`; then `obs_source_output_video`. Buffer queued back in every case.
- **S-056-R28** Render: no texture → nothing. With `sync.set`: wait on the acquire point first. Set effect param `image` to the texture. Rotation (R29 table) pushes `matrix_push; rotaa4f(0,0,1,rad); translate(-ox,-oy,0)` before drawing and pops after. Blend state push, `blend_function(ONE, INVSRCALPHA)`; effective crop → `draw_sprite_subregion(tex, flip, x, y, w, h)`, else `draw_sprite(tex, flip, 0, 0)`. Then, if cursor visible & valid & has texture: push, translate(x-hotspot_x, y-hotspot_y), set `image` to the cursor texture, `draw_sprite(<main texture>, 0, cursor_w, cursor_h)` (the *main* texture is passed, only for geometry), pop. Blend pop. With `sync.set`: export the release point on a fresh sync, mark it will-signal.
  Rotation table (transform → angle, offset x, offset y): 90/Flipped90 → 90°, (0, H); 180/Flipped180 → 180°, (W, H); 270/Flipped270 → 270°, (W, 0); None/Flipped → none; W,H = crop size if effective crop else frame size. Flip: Flipped, Flipped180 → `GS_FLIP_U`(1); Flipped90, Flipped270 → `GS_FLIP_V`(2); else 0. Sign of zero offsets prints as `-0`.
- **S-056-R29** Size: 0 until the first successful `param_changed`; else width/height of crop (if effective) or frame, swapped for 90/270 (and flipped variants).
- **S-056-R30** `set_framerate` / `set_resolution` (camera): no change when equal (including both unset); otherwise store (NULL → unset, stored as 0/0) and signal the renegotiation event.
- **S-056-R31** Stream destroy: if the source flags have any of the `ASYNC_VIDEO` bits (`flags & 0x5`, i.e. **any video source**, Q-1) → `obs_source_output_video(source, NULL)`; remove from the connection's list; destroy textures in the graphics context; disconnect and destroy the pw stream under lock; close held sync fds; free format list. Connection destroy: destroy all streams, stop thread loop, destroy registry proxy (if any), context, thread loop; close fd.

### Camera portal
- **S-056-R32** Registration `camera_portal_load` registers `pipewire-camera-source` (module load registers it before the screencast sources, even without a ScreenCast portal). Defaults: `device_id` = empty. Name key `PipeWireCamera`.
- **S-056-R33** Creation: `device_id` copy; `framerate` setting parsed (R39); `format` setting present → `parse_format`; else legacy: `pixelformat` (SPA id, table) + `resolution` JSON `{width,height}` → on success the converted `format` JSON `{"encoded":false,"video_format":<id>,"width":W,"height":H}` is written to the settings. Then `access_camera`: if the shared connection already has a PipeWire connection → stream immediately; else create the connection on first use, append the source, and **only if not already initializing** subscribe a Request and call `AccessCamera({handle_token})`, set `initializing=true` (never reset, Q-5).
  Response 0 → INFO `[camera-portal] Successfully accessed cameras`, `OpenPipeWireRemote({})` (no session) → connect with registry listener, then a roundtrip. Non-zero → WARNING `[camera-portal] Failed to create session, denied or cancelled by user`. Call errors: ERROR `[camera-portal] Error accessing camera: <msg>` / `Error retrieving PipeWire fd: <msg>` (cancelled = silent).
- **S-056-R34** Registry `global` of type `PipeWire:Interface:Node`: id → device (`node.name` key, `node.description` for display), INFO `[camera-portal] Found device <name>`, bind (failure: WARNING `[camera-portal] Failed to bind device <name>`, ignored), listeners, table insert; then for every source: `obs_source_update_properties`, and stream if `device_id` equals. `global_remove(id)`: drop matching devices (INFO `[pipewire-camera] Removed device <name>`, defect D-1), refresh every source's properties; running streams are untouched.
- **S-056-R35** Param bookkeeping per device: `node.info` with PARAMS change: for each changed param (user flag set) clear and `enum_params(++seq, id)` if READ(1<<1); async result stored as the expected seq; `proxy_sync`. Params received are pending until the proxy `done` event for the sync seq commits them, discarding pending params whose seq differs from the committed expected seq. (`param` events must carry the seq returned by enum_params.)
- **S-056-R36** Format list (selecting a device): one entry per EnumFormat that is video + raw + table format + a plain `Rectangle` size. Text: `"<W>x<H>"`, ` (<aspect>)`, then if a framerate property exists: ` - <fps list> FPS` (None → the value; Enum → entries 1.. (entry 0 is the default) printed in **reverse**; `Range`/`Step` → WARNING `Ranged framerates not supported` / `Stepped framerates not supported` and the whole entry is skipped; non-Fraction → WARNING `Framerate is not a fraction` skipped; fps text `%u` if denominator 1 else `%.2f`; separator `, `), then ` - <pretty name>`. Value = JSON `{"encoded":false,"video_format":<id>,"width":W,"height":H}`. Non-raw subtypes and unknown formats are skipped silently.
  Aspect: `g = gcd(w,h)`; if g <= 50: `w>h` → `"%.2f:1"` (w/h) else `"1:%.2f"` (h/w); else `w/g:h/g` with `8:5` → `16:10`.
- **S-056-R37** Controls (group `controls`, rebuilt on device selection): from PropInfo params with `id`, optional description, `type` choice, optional `container`, optional `labels`. Int + Enum choice with labels → int list (labels `int,string` pairs; no labels → no property); Int otherwise → int slider (min = v[1], max = v[2], step = v[3] or (max-min)/256 truncated; **a Range choice exposes only 3 values**, so the step is the truncated default; a Step choice gives the real step); Bool → checkbox; Float → float slider (step default (max-min)/256); other types skipped. Property name = description. Default value = v[0]. Change → `Props` object `{<id>: Int|Bool|Float}` via `node set_param(Props)`; unknown device → ERROR `unknown device <id>` (false). A PropInfo without description is undefined in the reference (D-5); implementations must skip it.
- **S-056-R38** Selection callbacks: `device_selected` (INFO `[camera-portal] selected camera '<id>'`, unknown device → false) updates the stored id (`restart_stream` if changed), refills the format list, rebuilds controls and then triggers `format` modified. `format_selected` (INFO `Selected format for '<id>'`): parse JSON (failure → false; ERROR from obs-data for empty text); stream restart if no stream, restart flag, or subtype/pixel format changed, else `set_resolution`; refills the framerate list for (format, resolution) (sorted ascending by value, duplicates kept, JSON `{"framerate":{"numerator":n,"denominator":d}}`, text `%.2f`), enables the list only if non-empty, and triggers `framerate` modified (selected → `set_framerate`) or, with no framerates, resets the stream framerate. **The stored framerate setting is re-applied after a format change even if the new format lacks it** (Q-8). `update`: new `device_id` differing from stored → restart stream.
- **S-056-R39** `parse_format`: needs `video_format` and `encoded`; encoded → subtype = video_format int (format untouched); raw → table lookup (unknown → false), subtype raw; width/height both present → stored resolution. `parse_framerate`: JSON → `media_frames_per_second` (fails → unset). `stream_camera`: destroy old stream; device lookup (missing → stop, silent); INFO `[camera-portal] streaming camera '<id>'`; stream props media.type=Video, category=Capture, role=Camera, stream name "OBS PipeWire Camera"; the selected format is always passed (a zero `spa_format` when nothing selected → `Id:0` in the pod).
- **S-056-R40** Devices list: items "description"→"node.name" in hash-table order (unspecified order); if the configured device is absent it is appended as a **disabled** item (name = id). `show/hide/width/height` forward to the stream. Destroy: removed from the shared list, stream destroyed. `camera_portal_unload` frees the connection: cancels, destroys devices (proxies), the PipeWire connection.

### Module
- **S-056-R41** `obs_module_load`: in the graphics context load GL functions; `pw_init`; register camera (PW >= 0.3.60) then screencast sources; returns true. `obs_module_unload`: screencast unload, camera unload, `pw_deinit` (PW >= 0.3.49).

## 4. Algorithms (pseudocode)

```
cursor_mode(avail, show): 4 if avail&4; elif avail&2 and show: 2; else 1
screencast_flow(src): sub(req1,cb1); CreateSession ; on resp0: store session; sub(req2); SelectSources ; on resp0: sub(req3); Start ; on resp0: pick last stream; maybe token+save; OpenPipeWireRemote ; fd -> engine
process_sync(buf=latest dequeued): corrupt-header? requeue : { if data0.size>0 { dmabuf? import : memptr? upload }; crop; transform; cursor; requeue }
modifier_failure(fmt, mod): if server>=0.3.40 remove all==mod else keep last element only; signal reneg
```

## 5. Concurrency
PipeWire callbacks run on the thread-loop thread; all `pw_stream_*` and `pw_core_*` calls from OBS threads happen under `pw_thread_loop_lock`. `on_process` takes the graphics context itself. D-Bus callbacks run on the GLib default main context (OBS UI thread). `obs_pipewire_destroy` destroys streams first, then stops the loop. The fakes run everything on one thread; lock depth is checked (`lock_depth=0` at loop destruction).

## 6. Quirks and defects

- **Q-1 QUIRK** `pw_stream_destroy` path tests `flags & OBS_SOURCE_ASYNC_VIDEO` as a *bitmask*, so every video source clears async frames with `obs_source_output_video(NULL)`. Harmless; SHOULD match (or simply always do it).
- **Q-2 QUIRK** Server version `"1.2"` fails the 3-field parse (warning) but leaves major=1, minor=2, micro=0, so version gates evaluate as 1.2.0 (not 0.0.0). Missing info event = 0.0.0.
- **Q-3 QUIRK** A format advertised in the DRM list whose modifier query returns nothing still gets `[MOD_INVALID]` as its only modifier when implicit modifiers are supported (an Enum with one element repeated twice).
- **Q-4 QUIRK** Old-server (< 0.3.40) modifier removal erases *all but the last* entry.
- **Q-5 QUIRK** Camera `initializing` is never cleared: after a denial/failure no later source asks again until `camera_portal_unload`.
- **Q-6 QUIRK** Cursor sprite is drawn with the main texture object (the cursor bitmap is bound through the effect param).
- **Q-7 QUIRK** PropInfo Range choices only expose 3 values (SPA clamps) so slider steps for Range are `(max-min)/256` truncated.
- **Q-8 QUIRK** Stale framerate re-applied after format changes (R38). Also the `RestoreToken` setting keeps the old value after Reload until next save.
- **Q-9 QUIRK** Request subscriptions of failed calls are never removed unless the cancellable fires (camera uses no cancellable → subscription leaks).
- **D-1 DEFECT** `on_registry_global_remove_cb` logs `device_id` after `g_hash_table_iter_remove` freed it (use after free; ASAN reports it). Required: log the device name; the vectors mask the text.
- **D-2 DEFECT** Start reply with 0 streams: `n_streams - 1` underflows (size_t) and the skip loop runs essentially forever; with a missing `streams` or `session_handle` entry the code dereferences NULL. Required: treat as failure (log, stop). Not exercised (would hang/crash).
- **D-3 DEFECT** Failure paths leak: `obs_pipewire_connect_fd` failures free the object without destroying thread loop/context/fd; `obs_pipewire_connect_stream` failure frees the stream struct but leaves the `pw_stream` and loop event alive; `teardown_pipewire` never disconnects the core. Required: release everything.
- **D-4 DEFECT** PropInfo without `description`: `name` is used uninitialised (R37). Required: skip.

## 7. Implementation notes (Rust)
Model the engine as a state machine fed by (a) portal replies, (b) PipeWire events, (c) a fake-able graphics trait; keep pod construction byte-exact (order of properties, MANDATORY|DONT_FIXATE on modifiers, repeated first modifier). Compare pods structurally (type/id/key/flags/values), not by memory layout. D-Bus variants: `a{sv}` order of insertion as listed in R07.

## 8. Not linked / not covered
Real GIO D-Bus (bus, proxies, signals), real libpipewire runtime (thread loop, core, registry, streams, `pw_loop`), real graphics (GL/EGL/dma-buf import, sync objects) and the OBS source machinery (`obs_source_*`, frame output) are replaced by scripted fakes; `libpipewire` is linked only for `pw_properties`, `pw_node_info_update/free`, `pw_stream_state_as_string` and header-inline SPA code. Hence: no real timing/threading, no real fd passing (a fake memfd/null fd), no PW version-conditional builds other than 1.6.x headers (all `PW_CHECK_VERSION` branches taken). Unexercised: zero-stream/NULL replies (D-2), GL drawing results, locale files (keys only), data/locale strings, `PW_CHECK_VERSION < 1.2` branches.

## 9. Coverage table (script → rules)

| Rules | Scripts |
|---|---|
| table 2.1 | formats_table, camera_formats |
| R01 | portal_paths, screencast_flow |
| R02 | screencast_destroy_inflight, screencast_flow |
| R03 | screencast_proxy_unavailable |
| R04 | screencast_source_types, screencast_source_types_window, screencast_proxy_unavailable, screencast_unload, module_* |
| R05 R12 | screencast_properties, screencast_lifecycle |
| R06 R07 R11 | screencast_flow, screencast_denied, screencast_call_errors, screencast_multiple_streams |
| R08 R09 | screencast_cursor_modes, screencast_cursor_embedded_shown, screencast_cursor_embedded_hidden, screencast_cursor_none_available |
| R10 R13 | screencast_restore_token, screencast_restore_token_cleared, screencast_restore_token_old_portal, screencast_multiple_streams |
| R14 | screencast_reload |
| R15 | screencast_lifecycle, screencast_destroy_inflight |
| R16 | pw_connect_failures, pw_server_version, pw_core_error, screencast_flow |
| R17 | pw_server_version, pw_negotiate_old_server |
| R18 R19 | pw_formats_dmabuf, pw_formats_nonimplicit, pw_formats_async, pw_formats_none, pw_server_version |
| R20 R21 | pw_negotiate_basic, pw_negotiate_modifier_explicit_sync, pw_negotiate_old_server, pw_negotiate_async, pw_negotiate_ignored, pw_negotiate_colorimetry, pw_state_changed |
| R22 R24 | proc_dmabuf_fail, proc_dmabuf_fail_old_server, camera_stream |
| R23 | proc_memptr, proc_memptr_skips, proc_latest_buffer, proc_dmabuf |
| R25 R28 R29 | proc_crop_transform, proc_cursor |
| R26 | proc_explicit_sync, proc_explicit_sync_mismatch |
| R27 | proc_async |
| R30 R38 | camera_stream |
| R31 | proc_memptr, camera_stream |
| R32 R33 | camera_discovery, camera_denied, camera_two_sources, camera_settings |
| R34 R40 | camera_registry, camera_two_sources, camera_props_missing_device |
| R35 R36 R37 | camera_discovery, camera_formats, camera_stream |
| R39 | camera_settings, camera_registry |
| R41 | module_entry, module_no_screencast |

Comparison: text lines exactly; floats are printed with `%.6g` / `%.9g` (tolerance: exact string). Vectors run under `run.sh check pipewire` (56 scripts).

## 10. Open questions
- Exact `PW_CHECK_VERSION` combinations: only the newest (1.2+) paths were run; older builds drop explicit sync, 10-bit formats, VideoTransform (local enum), camera.
- Behaviour with real multi-threading (renegotiation event racing with `process`) is not modelled.
- Whether `ShowCursor` should force EMBEDDED when METADATA is also available is as in R08 (metadata always preferred).
