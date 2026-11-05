# S-085 vlc-video + obs-vst FFI contract (both ports; VST2 ABI — D-11)

| | |
|---|---|
| Status | Draft (round 9 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1: `plugins/vlc-video/*` (C, libvlc 5 kept), `plugins/obs-vst/*` (C++, VST 2.x kept) |
| Target crate | `crates/vlc-vst-ffi` (host glue: plugin discovery, chunk/MD5 state, settings mapping). vlc-video is ported to Rust via the libvlc FFI (P5-22); obs-vst is ported to Rust per D-11 (confirmed 2026-11-05) — the contract below pins the VST2 ABI the Rust host must honour. |
| Depends on | libobs core (source info, audio frame layout, hotkeys, missing-files framework, proc handler), Qt (VST editor host) |
| Conformance | planned, none yet |

## 1. Scope

In scope: the native surface the host depends on — VLC library discovery/loading and the
libvlc call sequence a `vlc_source` instance makes; the VST host contract — per-platform
effect loading, dispatcher call order, host callbacks, audio block processing, chunk
(persisted state) serialization, and the editor window. Out of scope: libvlc/VST2
implementation internals, plugin UI theming, the libobs side of source registration.

### Reference files (11, full paths relative to `obs-studio/`)

- `plugins/vlc-video/vlc-video-source.c` — `vlc_source` input: playlist, controls, callbacks
- `plugins/vlc-video/vlc-video-plugin.c` — libvlc discovery, dynamic loading, function table
- `plugins/obs-vst/obs-vst.cpp` — `vst_filter` source info, properties, plugin scan, chunk save
- `plugins/obs-vst/VSTPlugin.cpp` — VST host: effect lifecycle, audio processing, chunk get/set
- `plugins/obs-vst/linux/VSTPlugin-linux.cpp` — Linux effect load/unload
- `plugins/obs-vst/win/VSTPlugin-win.cpp` — Windows effect load/unload
- `plugins/obs-vst/mac/VSTPlugin-osx.mm` — macOS effect load/unload (CFBundle)
- `plugins/obs-vst/EditorWidget.cpp` — editor base widget (close handling)
- `plugins/obs-vst/linux/EditorWidget-linux.cpp` — Linux editor container
- `plugins/obs-vst/win/EditorWidget-win.cpp` — Windows editor container (native window)
- `plugins/obs-vst/mac/EditorWidget-osx.mm` — macOS editor container (NSView)

## 2. Conventions

RFC 2119 words. "Effect" = the loaded `AEffect`. "Chunk" = the plugin's serialized state
(persisted as base64 in source settings). "Host callback" = `audioMaster*` dispatch the
effect performs against the host. Both plugins are registered sources with fixed ids:
`vlc_source` (input, controllable media) and `vst_filter` (audio-only filter).

## 3. Interface — the native surface the host depends on

### 3.1 VLC: library discovery & function table

| Platform | Library loaded (all via `os_dlopen`) |
|---|---|
| Windows | `<HKLM\SOFTWARE\VideoLAN\VLC InstallDir>\libvlc.dll` (registry lookup) |
| macOS | `/Applications/VLC.app/Contents/MacOS/lib/libvlccore.dylib` **first**, then `.../lib/libvlc.5.dylib`; also sets `VLC_PLUGIN_PATH=.../MacOS/plugins` |
| Linux/BSD | `libvlc.so.5` (soname **pinned to v5**) |

Not found ⇒ log `"Couldn't find VLC installation, VLC video source disabled"`, module
still loads, source **not registered**. All ~45 libvlc entry points are resolved through
`os_dlsym` (core: `libvlc_new/release/get_version/clock/event_attach`; media:
`media_new_path/location/add_option/release/retain/get_meta/tracks_get/tracks_release`;
media player: `player_new/new_from_media/release`, `video_set_callbacks/
video_set_format_callbacks`, `audio_set_callbacks/audio_set_format_callbacks`,
`player_play/stop/get_time/set_time`, `video_get_size`, `player_event_manager/
get_state/get_length/get_media`; media list: `list_new/release/add_media/lock/unlock/
event_manager`; media list player: `new/release/play/pause/stop/set_media_player/
set_media_list/event_manager/set_playback_mode/next/previous`). Any missing symbol ⇒
load fails (source not registered). `load_libvlc()` = `libvlc_new(0, 0)` + global
`time_start = libvlc_clock() * 1000` (clock epoch used to convert VLC µs/pts into obs
ns-relative timestamps). Unload: `libvlc_release`, then dlclose (macOS closes the core
module first).

### 3.2 VLC: per-instance call order

Create: hotkey registration (`VLCSource.PlayPause/Restart/Stop/PlaylistNext/PlaylistPrev`)
→ mutex init → `load_libvlc()` → `media_list_player_new` → `media_player_new` →
`media_list_player_set_media_player` → `video_set_callbacks(lock, NULL-unlock, display)`
→ `video_set_format_callbacks` → `audio_set_callbacks(play)` +
`audio_set_format_callbacks` → attach `MediaPlayerEndReached` (stopped) and
`MediaPlayerOpening` (started) → proc handler `get_metadata(tag_id)→tag_data` →
`obs_source_update(source, NULL)`.

Update (per settings change): stop list player → rebuild file array (below) → swap under
mutex → optional shuffle → build `media_list` (lock/add all/unlock) →
`set_media_list` → `set_playback_mode(loop ? loop : default)` → play if files exist and
(always-play behavior or source active), else output a NULL video frame.

Destroy: close players, release all media, free paths, destroy mutex.

### 3.3 VST: per-effect call order

Load (`loadEffectFromPath`): path changed ⇒ full unload first. Platform load (below) ⇒
check `effect->magic == kEffectMagic` ⇒ channel count `max(numInputs, numOutputs)` in
`0..256` ⇒ allocate per-channel I/O buffers ⇒ `effGetEffectName`,
`effGetVendorString` ⇒ reject synth/non-replacing plugins ⇒ `effIdentify` ⇒
`effOpen` ⇒ fill time info (sample rate from the audio output, tempo 120, 4/4,
`kVstTempoValid|kVstNanosValid|kVstTransportPlaying`) ⇒ `effSetSampleRate` ⇒
`effSetBlockSize(512)` (`BLOCK_SIZE`) ⇒ `effMainsChanged(1)` ⇒ ready.

Unload: `effMainsChanged(0)` ⇒ `effClose` ⇒ platform `unloadLibrary`.

Per audio block (`process`, block = 512 samples): silence output buffers ⇒ map input
planes (audio plane if present, else scratch) ⇒ `processReplacing(effect, inputs,
outputs, frames)` ⇒ copy back channels `< min(numOutputs, MAX_AV_PLANES)`.

Host callbacks (`audioMaster*`): `audioMasterVersion → 2400`;
`audioMasterGetCurrentProcessLevel → 1`; `audioMasterWillReplaceOrAccumulate → 1`
(always replace); `audioMasterGetSampleRate →` audio output rate;
`audioMasterGetVendorString → "OBS Studio"`; `audioMasterGetTime →` VstTimeInfo with
`nanoSeconds = os_gettime_ns()/1000000`; `audioMasterSizeWindow(w,h) →` editor
`handleResizeRequest(w, h)`; everything else `0`.

## 4. Behaviour (numbered rules)

### 4.1 VLC module & source

| Rule | Statement |
|---|---|
| S-085-R01 | VLC absence is non-fatal: the module loads and logs, but `vlc_source` is not registered; the Rust host must surface "source unavailable" the same way (no registration, not an error). |
| S-085-R02 | libvlc is loaded from the platform-specific location in §3.1 (soname 5 on Unix, registry `InstallDir` on Windows); no bundled fallback. |
| S-085-R03 | Source registration: id `vlc_source`, type input, flags `ASYNC_VIDEO | AUDIO | DO_NOT_DUPLICATE | CONTROLLABLE_MEDIA`, media icon; exposes `media_play_pause/restart/stop/next/previous/get_duration/get_time/set_time/get_state` and 5 source hotkeys. |
| S-085-R04 | Settings: `playlist` (editable files+URLs list), `loop` (default **true**), `shuffle` (false), `playback_behavior` ∈ {`stop_restart` (default), `pause_unpause`, `always_play`}, `network_caching` ms (default **400**, range 100–60000 step 10), `track` (default 1, range 1–10), `subtitle_enable` (false), `subtitle` (default 1, range 1–1000). Properties defer updates. |
| S-085-R05 | Playlist entry handling: an entry that is a **directory** expands to the files inside with valid media extensions (`EXTENSIONS_MEDIA` = video+audio+playlist lists in `vlc-video-plugin.h`); URLs (`"://"`) are media locations, files are paths (Windows: `/` → `\` for local paths). |
| S-085-R06 | Per-media options: URLs get `:network-caching=<ms>`; all get `:audio-track=<track-1>`; subtitles: `:sub-track=<subtitle-1>` when enabled, empty `:sub-track=` when disabled (Q-6). Media objects are **reused** across updates when the same path already has one. |
| S-085-R07 | Update order: stop → swap file list → shuffle (Fisher–Yates with `rand()`, only when >1 file and shuffle on) → free old media → set list + playback mode → play or clear (`obs_source_output_video(source, NULL)`). |
| S-085-R08 | Video path is **zero-copy**: the format callback converts the VLC chroma to an obs format (RGBA/BGRA/NV12/I420/UYVY/YUY2/YVYU/I444/Y800; unmapped ⇒ forced BGRA; `J420`/`J444` marked full-range), recomputes display size from the media's video track (**SAR-adjusted**, orientation variants swap width/height), reuses the obs frame unless format/size changed; the lock callback hands VLC the obs frame planes directly; display outputs the frame with `timestamp = libvlc_clock()*1000 - time_start`. |
| S-085-R09 | Audio path: the setup callback reports the audio output's format/layout, clamping requested channels down to the output layout; VLC does the conversion; the play callback memcpys into a single growing `data[0]` buffer (Q-1), `timestamp = pts*1000 - time_start`. |
| S-085-R10 | Behaviour on activate/deactivate: `stop_restart` ⇒ play / (stop + clear frame); `pause_unpause` ⇒ play / pause; `always_play` ⇒ nothing in either direction. Events: `Opening` ⇒ `media_started`; `EndReached` ⇒ clear frame **only when not looping** + `media_ended`. |
| S-085-R11 | Media control surface: play/pause only acts from the matching state (Playing→pause, Paused→play); restart = stop+play; stop = stop + clear frame; next/previous = list-player next/previous; duration/time via `get_length`/`get_time`, seek via `set_time`. **There is no snapshot surface in 33.0.1** (open question O-1). |
| S-085-R12 | State mapping: `NothingSpecial→NONE, Opening→OPENING, Buffering→BUFFERING, Playing→PLAYING, Paused→PAUSED, Stopped→STOPPED, Ended→ENDED, Error→ERROR`. |
| S-085-R13 | Missing files: every non-URL playlist entry whose path does not exist is reported via the missing-files framework; the callback rewrites the entry's `value` to the replacement path or erases it, then re-updates the source. |
| S-085-R14 | Proc handler `get_metadata(tag_id)→tag_data` maps 25 ids (title, artist, genre, copyright, album, track_number, description, rating, date, setting, url, language, now_playing, publisher, encoded_by, artwork_url, track_id, track_total, director, season, episode, show_name, actors, album_artist, disc_number, disc_total) to `libvlc_media_get_meta` on the current media. |

### 4.2 VST module & source

| Rule | Statement |
|---|---|
| S-085-R15 | Source registration: id `vst_filter`, type filter, `output_flags = OBS_SOURCE_AUDIO` (audio only); module description `"VST 2.x Plug-in filter"`; locale `obs-vst/en-US`. |
| S-085-R16 | `plugin_path` property = string list `"Name=path"` entries, placeholder first item `"{Please select a plug-in}"` (null value); scan is recursive with symlink following over: macOS `/Library/Audio/Plug-Ins/VST/` + `~/Library/Audio/Plug-ins/VST/` (`*.vst`); Windows 64-bit `%ProgramFiles%`/`%CommonProgramFiles%` Steinberg+VST2+VSTPlugins dirs (`*.dll`, 32-bit-on-64 uses the x86 variants); Linux `$VST_PATH` (colon-split) else `/usr{,/local}/lib{64,}/{vst,lxvst,linux_vst}/` + `~/.vst/` + `~/.lxvst/` (`*.so`, `*.o`); extension stripped from the display name; list `stable_sort`ed (case-sensitive). |
| S-085-R17 | Editor buttons `open_vst_settings` / `close_vst_settings`: both hidden when no effect loaded; open shown when loaded+closed; close shown when loaded+open; visibility recomputed on property change and on open/close clicks. `open_when_active_vst_settings` bool: open the editor automatically when the effect loads. |
| S-085-R18 | Update: empty `plugin_path` ⇒ unload; otherwise load the effect; then restore the chunk **iff** stored `chunk_hash` equals the current file's MD5 **or** stored `chunk_hash` is empty (Q-4). Save: `chunk_data` = base64(getChunk()), `chunk_hash` = MD5 of the effect file (MD5 via QCryptographicHash, hex; empty string when unreadable). |
| S-085-R19 | Flatpak migration (Linux only): if `/.flatpak-info` exists and `plugin_path` starts with `/app/extensions/Plugins/lxvst`, rewrite it to `/app/extensions/Plugins/vst` in the settings before loading. |
| S-085-R20 | Per-platform effect load: Linux `os_dlopen` + entry `VSTPluginMain` → `VstPluginMain()` → `main`; Windows `LoadLibraryW` + same three (`ERROR_BAD_EXE_FORMAT` ⇒ "wrong architecture" warning; `main()` exceptions caught ⇒ "VST plugin initialization failed"); macOS `CFBundleCreate(isBundle)` + `VSTPluginMain` → `main_macho` (pre-2.4 SDK). All set `effect->user = this` (host callback routing). |
| S-085-R21 | Effect validation: bad magic number, channel count outside 0..256, or `effFlagsIsSynth`/`!effFlagsCanReplacing` ⇒ warn and leave the filter a **silent pass-through with `effectReady = false`** while `vstLoaded()` still reports true (Q-3). |
| S-085-R22 | Chunk serialization: with `effFlagsProgramChunks` — `effGetChunk(1)` buffer / `effSetChunk(1, len, buf)`, base64'd for storage; without — raw `float` array of `getParameter(0..numParams-1)` (little-endian memory layout) base64'd; set side decodes and **silently drops** the chunk when the float count != `numParams`. |
| S-085-R23 | Destroy: close editor (queued) + `deleteLater`; the destructor unloads the effect and frees channel buffers. `filter_audio` calls `process()` then `getSourceNames()` (parent + filter names feed the editor window title: `"<source>: <filter> - <effect>"`, source defaults to `"VST 2.x"`). |

### 4.3 Editor window

| Rule | Statement |
|---|---|
| S-085-R24 | Opening the editor requires `effFlagsHasEditor` (else warn); creates a top-level `EditorWidget`, builds the platform container, then shows it; closing (user or programmatic) ⇒ `onEditorClosed`: delete widget + `effEditClose`. |
| S-085-R25 | Container per platform: **Linux** — `effEditOpen(winId)` then `effEditGetRect` → `setFixedSize`; resize requests ⇒ `setFixedSize(w,h)`. **macOS** — 300×300 `NSView` container, read rect, frame + resize + `setFixedSize`, **then** `effEditOpen(view)` (order differs from Linux); resize ⇒ `setFixedSize`. **Windows** — register class `"Minimal VST host - Guest VST Window Frame"` with bare `DefWindowProcW` (the plugin paints the window itself), create overlapped resizable window, store `AEffect*` in `GWLP_USERDATA`, wrap in `QWidget::createWindowContainer`, zero-margin grid; `effEditOpen(handle)`; size from `effEditGetRect` **divided by `devicePixelRatioF()`** (DPI quirk), fallback 300×300; `audioMasterSizeWindow` handler re-reads the rect the same way. |
| S-085-R26 | Editor base: window flags gain `Qt::MSWindowsFixedSizeDialogHint`; `closeEvent` routes to `VSTPlugin::onEditorClosed` (which is what makes `effEditClose` fire). |

## 5. Concurrency

- **VLC**: libvlc runs its own threads; the video path is zero-copy into the obs frame
  (the display callback calls `obs_source_output_video` from VLC's thread — the standard
  async-source contract). The audio buffer grows via `brealloc` from VLC's audio thread.
  The source mutex protects only the playlist/file array (update-swap, property read).
  `time_start` is a process-global set once at libvlc init.
- **VST**: `process()` runs on the audio render thread; effect access is guarded by a
  `recursive_mutex`, with an `effectReady` flag checked before the lock (to avoid
  blocking behind an in-flight `unloadEffect`) and re-checked after. The Qt editor runs
  on the main thread and calls `effEditOpen/GetRect/Close` without holding that mutex —
  the VST2 plugin itself is not re-entrancy-safe; the host relies on the mutex plus the
  ready flag only.

## 6. Reference defects and quirks

- **D-1**: the macOS editor container builds `NSMakeRect(vstRect->left, vstRect->top,
  vstRect->right, vstRect->bottom)` — using the rect's right/bottom **positions** as
  width/height, so the frame is mis-sized for any plugin whose rect isn't origin-based
  (most are 0,0,w,h, which masks the bug).
- **Q-1**: VLC audio uses a single plane (`data[0]`); multi-plane layouts would be
  truncated. Channel counts above the audio output layout are clamped down.
- **Q-2**: `EndReached` with loop on keeps the last frame on screen; only non-loop
  clears it.
- **Q-3**: a rejected effect (synth / non-replacing) leaves `vstLoaded()` true, so the
  UI shows the open/close buttons while the filter silently passes audio through.
- **Q-4**: a stored chunk with an **empty** `chunk_hash` is applied to whatever file is
  currently selected, even if it was saved for a different file.
- **Q-5**: the Windows editor window is a bare `DefWindowProcW` class; the VST paints
  into it directly, and all sizes are divided by `devicePixelRatioF()`.
- **Q-6**: VLC always appends a `:sub-track=` media option, empty when subtitles are
  disabled.
- **Q-7**: the VLC soname is pinned to major 5 (`libvlc.so.5`, `libvlc.5.dylib`); a
  machine with only a newer libvlc gets no source at all.
- **Q-8**: `shuffle` uses `rand()` (unseeded beyond the default) — order is
  non-deterministic and not reproducible across runs.

## 7. Open questions

1. **Snapshot**: the round-9 task text lists "snapshot" in the VLC wrapper surface, but
   the 33.0.1 `vlc_source` implements no snapshot capture (no `media_snapshot`-style
   callback, no VLC frame-grab API). Confirm whether the rewrite should drop it from the
   contract or add it out-of-conformance.
2. The `EXTENSIONS_MEDIA` whitelist (in `vlc-video-plugin.h`, not in the S-085 file
   list) drives directory expansion and the file-picker filter — confirm it is pinned by
   this spec's §4.1 R05 citation or needs its own line in a later round.
3. VST chunk float arrays assume little-endian memory layout; all supported hosts are
   LE, but the Rust side must pin this explicitly when (re)implementing chunk handling.
4. The `get_media` reuse compares **normalized** paths (Windows `\` form) — on Windows a
   file entered with mixed separators may be double-instantiated; replicate or normalize
   at the host?
