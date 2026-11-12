# S-051 Windows capture plugins (`win-capture`, `win-wasapi`, `win-dshow`) – behavioural catalog

| | |
|---|---|
| Status | Ready for review (Tier C catalog; Windows-only code, partly verified on Linux through shims) |
| Reference | OBS Studio 33.0.1: `plugins/win-capture` (incl. `graphics-hook/` consumers), `shared/obs-hook-config/*`, `libobs/util/windows/window-helpers.*`, `plugins/win-wasapi`, `plugins/win-dshow` |
| Target crate | `crates/obs-win-capture` (hook protocol, window matching, settings), `crates/obs-win-wasapi`, `crates/obs-win-dshow` |
| Depends on | S-004 (signals/procs, for the `hooked`/`unhooked` signals), S-007 (config file, for offsets ini), obs-data/obs-properties specs |
| Conformance | `test/conformance/scripts/wincapture/` (28 scripts, runner `gen/wincapture_runner.c`) |

## Verification classes used in this document

Every rule carries a tag. Be precise about what is *proven by running the reference* and what is not.

- **[REAL]** The vector was produced by running the **unmodified reference C** (`window-helpers.c`, `graphics-hook-info.h`, `shared-memory-queue.c`, `tiny-nv12-scale.c`, `compat-helpers.c`) on Linux against a small fake Win32 layer (`gen/wincapture_shim`). The fake layer (window table, named file mappings) is part of the test harness, not of the reference.
- **[MODEL]** The logic lives in static functions inside Windows-only C/C++ files that cannot be compiled here. It was re-implemented in the runner by reading the source; the vectors are *source-derived, not reference-produced*. An implementation must follow the rule text; the vectors only pin down the author's reading.
- **[SRC]** Source-derived only, **no vector at all** (needs a real Windows host, a GPU, COM, a game process, libdshowcapture, ...). Implementers must test these on Windows.

## 1. Scope

Covered (priority order as requested): (1) the hook protocol and shared structs (`graphics-hook-info.h`, `hook-helpers.h`, `graphics-hook-ver.h`) and the plugin side of the hook handshake; (2) window identity strings and window matching (`window-helpers.c`), used by window capture, game capture and WASAPI process capture; (3) WASAPI source settings and channel-layout / default-device rules; (4) `win-capture` source settings, defaults and method selection (window capture, game capture, monitor/display capture), audio-helpers, `compatibility.json` lookup; (5) `win-dshow` source settings, capability/format selection, buffering, the DirectShow hardware encoder sizing rules, and the virtual-camera shared-memory queue.

**Not covered** (listed again in §11): all rendering and GPU code (D3D8/9/10/11/12, DXGI, DDraw, OpenGL, Vulkan hooks; texture sharing; HDR/colour-space handling in `game_capture_render`; `dc-capture`, `cursor-capture`, WGC/DXGI duplicator frame paths), process injection (`inject-helper`, `inject_library_obf`, `nt-stuff`, `app-helpers` AppContainer paths), `get-graphics-offsets` binaries, the COM mechanics of WASAPI (device enumeration, notification client, RTWQ work items, buffer/timestamp handling), libdshowcapture internals (device enumeration, `DecodeDeviceId`/`EncodeDeviceId`, the `VideoFormat` enum numbering, crossbar, MJPEG/H.264 decode in `ffmpeg-decode.c`), the `virtualcam-module` DirectShow filter, `tiny-nv12-scale` interpolation (only its geometry contract is exercised), UI strings and translations.

## 2. Terminology and conventions

- *Window string*: `"<title>:<class>:<executable>"` with each part escaped (§4.2). Stored as the `window` setting of window capture, game capture and WASAPI process-output sources.
- *Priority*: `WINDOW_PRIORITY_CLASS = 0`, `TITLE = 1`, `EXE = 2` (stored as int setting `priority`).
- *Rating*: non-negative integer, lower is better, `0x7FFFFFFF` = no match.
- Strings are UTF-8 in settings; Windows APIs return UTF-16; "case-insensitive" compare is `toupper` per byte (ASCII only; bytes ≥ 0x80 compare by raw value after `toupper` of the C locale).
- All hook-info integers are little endian. `HWND` values cross the hook boundary as **32-bit** values (`uint32_t window`).
- Settings value for a bool/int that is absent and has no default reads as `false`/`0`; an absent string reads as `""` (obs-data semantics, see obs-data spec).

## 3. Interface

Sources registered (ids matter for saved scenes, **must be kept**): `window_capture`, `game_capture`, `monitor_capture`, `display_capture`, `wasapi_input_capture`, `wasapi_output_capture`, `wasapi_process_output_capture`, `dshow_input`, DirectShow encoders `dshow_c985_h264` / `dshow_c353_h264` (codec `h264`) [SRC for registration details]. `window_capture`, `game_capture` and `wasapi_process_output_capture` expose the proc `get_hooked(out bool hooked, out string title, out string class, out string executable)` and the signals `hooked(ptr source, string title, string class, string executable)` / `unhooked(ptr source)` (the title/class/exe strings are `""` when not hooked) [SRC]. `wasapi_process_output_capture` also has proc `reroute_audio(in ptr target)`.

## 4. Data formats

### 4.1 Hook protocol structures (`graphics-hook-info.h`) [REAL]

All structs are `#pragma pack(8)`. Sizes and offsets below are proven by `hook_layout.script`.

```
struct hook_info (648 bytes)               offset
  u32 hook_ver_major                          0
  u32 hook_ver_minor                          4
  i32 type            (0 MEMORY, 1 TEXTURE)   8
  u32 window                                 12
  u32 format          (DXGI_FORMAT number)   16
  u32 cx, cy                                 20, 24
  u32 UNUSED_base_cx, UNUSED_base_cy         28, 32
  u32 pitch                                  36
  u32 map_id                                 40
  u32 map_size                               44
  u8  flip (+7 pad)                          48
  u64 frame_interval  (nanoseconds)          56
  u8 UNUSED_use_scale                        64
  u8 force_shmem                             65
  u8 capture_overlay                         66
  u8 allow_srgb_alias                        67
  struct graphics_offsets offsets (76 B)     68
  u32 reserved[126]                         144 .. 648
struct graphics_offsets (76 B): d3d8 @0 (1 u32: present), d3d9 @4 (5 u32: present, present_ex,
  present_swap, d3d9_clsoff, is_d3d9ex_clsoff), dxgi @24 (3 u32: present, resize, present1),
  ddraw @36 (8 u32: surface_create, surface_restore, surface_release, surface_unlock, surface_blt,
  surface_flip, surface_set_palette, palette_set_entries), dxgi2 @68 (1 u32: release),
  d3d12 @72 (1 u32: execute_command_lists)
struct shmem_data (12 B): i32 last_tex (volatile) @0, u32 tex1_offset @4, u32 tex2_offset @8
struct shtex_data (4 B):  u32 tex_handle @0
```

Worked example (`hook_bytes.script`): `ver_major=1, ver_minor=8, type=1, window=0x12345678, format=87 (BGRA), cx=1920, cy=1080, pitch=7680, map_id=7, map_size=12, flip=1, frame_interval=0x0123456789ABCDEF, force_shmem=capture_overlay=allow_srgb_alias=1` serialises (bytes 0..67, struct zero-initialised so padding bytes are 0) as
`01000000 08000000 01000000 78563412 57000000 80070000 38040000 00000000 00000000 001e0000 07000000 0c000000 01000000 00000000 efcdab8967452301 00 01 01 01`
(`flip` is the single byte at offset 48, followed by 3 padding bytes and then 4 more padding bytes up to the 8-aligned `frame_interval`). Padding is not guaranteed to be zero in a live mapping; a consumer must not read it. The 648-byte size is an ABI assertion; the layout is identical for the 32-bit and 64-bit hook (`graphics-hook32.dll` / `graphics-hook64.dll`), which is why every field is fixed width.

Hook version (`graphics-hook-ver.h`): `HOOK_VER_MAJOR = 1`, `HOOK_VER_MINOR = 8`, `HOOK_VER_PATCH = 8`, name string `"1.8.8.0"`.

### 4.2 Kernel object names (all pure functions of `pid`) [REAL for the constants and the hook-info name, SRC for the rest]

| Object | Name (ASCII, stored UTF-16) | Creator |
|---|---|---|
| event restart | `CaptureHook_Restart<pid>` | hook |
| event stop | `CaptureHook_Stop<pid>` | hook |
| event hook ready | `CaptureHook_HookReady<pid>` | hook |
| event hook exit | `CaptureHook_Exit<pid>` | hook |
| event init | `CaptureHook_Initialize<pid>` | hook |
| keep-alive mutex | `CaptureHook_KeepAlive<pid>` | **plugin** (`CreateMutexW`) |
| texture mutexes | `CaptureHook_TextureMutex1<pid>`, `...2<pid>` | hook |
| hook info mapping | `CaptureHook_HookInfo<pid>` (size 648) | hook (`create_hook_info`) |
| shmem/shtex data mapping | `CaptureHook_Texture_<top-level-HWND-as-u64>_<map_id>` | hook (`map_id` = counter, stored in `hook_info.map_id`) |
| named pipe (log) | `CaptureHook_Pipe<pid>` | plugin (server) |

`<pid>`, `<HWND>`, `<map_id>` are rendered in unsigned decimal, no padding (`hook_mapping.script` shows pid 0 and 4294967295). For UWP/AppContainer targets the plugin opens the objects as `\Sessions\<active console session>\AppContainerNamedObjects\<app SID>\<name>` instead (§5.A, [SRC]). Events are auto-reset, initially non-signalled; the plugin opens them with `EVENT_MODIFY_STATE|SYNCHRONIZE`, mutexes with `SYNCHRONIZE`; a mutex/event counts as signalled when `WaitForSingleObject(h, 0) == WAIT_OBJECT_0`.

### 4.3 Shared-memory capture layout (written by the hook) [MODEL, `hook_protocol_model.script`]

For `type == MEMORY`: `tex_size = cy * pitch` (u32); `aligned_header = align32(12) = 32`; `aligned_tex = align32(tex_size)`; `map_size = 32 + 2*aligned_tex + 32`. The first texture starts at the first 32-byte-aligned *address* ≥ base+32 (`tex1_offset = ((base + 32) & ~31) - base`, plus 32 if that is < 12); `tex2_offset = tex1_offset + aligned_tex`. For a page-aligned mapping (always) `tex1_offset = 32`. `last_tex` starts at −1. For `type == TEXTURE`: `map_size = 4`, payload is `shtex_data`.

### 4.4 Virtual-camera frame queue (`shared-memory-queue.c`) [REAL]

Mapping name `OBSVirtualCamVideo`; header (80 bytes, little endian):

```
u32 write_idx @0 | u32 read_idx @4 | u32 state @8 (0 INVALID,1 STARTING,2 READY,3 STOPPING)
u32 offsets[3] @12 | u32 type @24 (0 video) | u32 cx @28 | u32 cy @32 | (pad) | u64 interval @40
u32 reserved[8] @48
```

Frame slot i (i = 0..2) at `offsets[i]`: 32-byte frame header whose first 8 bytes are the u64 timestamp, then `cx*cy*3/2` bytes of NV12 (luma `cy` rows of `linesize` bytes — the writer copies `linesize[0]*cy` luma bytes and half that many chroma bytes). `offsets[0] = align32(80) = 96`; each following offset is `align32(previous + frame_size + 32)`; total size = `align32(offsets[2] + frame_size + 32)`. Example `cx=4,cy=2`: frame_size 12, offsets 96, 160, 224 (`vqueue.script`).

## 5. Behaviour (numbered rules)

Notation: `R` = `S-051-R`.

### 5.A Hook handshake (plugin side) [SRC unless tagged]

- **R01 [REAL]** `struct hook_info` is exactly 648 bytes with the offsets of §4.1; the plugin and hook exchange it through the mapping of R03. Any change of layout requires a new major version.
- **R02 [REAL]** Constants of §4.2 (names, `PIPE_NAME = "CaptureHook_Pipe"`) are exact.
- **R03 [REAL]** `create_hook_info(pid)` creates a pagefile-backed mapping of 648 bytes named `CaptureHook_HookInfo<pid>`; the plugin opens it read/write (`FILE_MAP_READ|FILE_MAP_WRITE`) and maps `sizeof(hook_info)` bytes.
- **R04 [SRC]** Plugin start-up order for a target process: create keep-alive mutex → start named-pipe server (`CaptureHook_Pipe<pid>`; text received on the pipe is logged at info level) → if the restart event already exists (a hook is already loaded) signal it and skip injection, else inject `graphics-hook32/64.dll` (picked by the target's bitness; an anti-cheat-compatible helper `inject-helper` is used when the `anti_cheat_hook` setting is true) → open `hook info` mapping → open events (`Restart`, `Stop`, `Initialize`, `HookReady`, `Exit`) and texture mutexes (error code 2 "not found" while the hook is still loading is retried silently, once logged as "hook not loaded yet, retrying..").
- **R05 [SRC]** `init_hook_info` writes into the shared `hook_info`: `offsets` = the 64-bit or 32-bit offsets set loaded from `graphics-hook-offsets` ini (R06); `capture_overlay = capture_overlays` setting; `force_shmem = sli_compatibility` setting; `UNUSED_use_scale = false`; `allow_srgb_alias = true`; `frame_interval` per R07. If `gs_shared_texture_available()` is false, `force_shmem` is forced to `true` (logged).
- **R06 [SRC]** Offsets ini (parsed with the config-file module, S-007; `config_get_uint`, missing key = 0): `[d3d8] present`; `[d3d9] present, present_ex, present_swap, d3d9_clsoff, is_d3d9ex_clsoff`; `[dxgi] present, present1, resize, release` (`release` goes to `dxgi2.release`). **`ddraw` and `d3d12` offsets are not loaded by the plugin** (stay 0 unless the hook computes them itself).
- **R07 [MODEL]** `frame_interval = util_mul_div64(fps_den, 1_000_000_000, fps_num)` computed as `(den / num) * 1e9 + ((den % num) * 1e9) / num` (u64 arithmetic, truncating); halved (`/= 2`, truncating) unless `limit_framerate` is true; 0 when no video info exists. Examples: 60 fps unlimited 8333333, limited 16666666; 60000/1001 unlimited 8341666.
- **R08 [MODEL]** `start_capture` refuses (returns false, warning) when `hook_info.hook_ver_major > HOOK_VER_MAJOR (1)`; any minor is accepted. `type == MEMORY` → shmem path (R09), any other value → shtex path.
- **R09 [SRC]** Shmem capture: open the data mapping named per §4.2 with `map_id`; textures at `base+tex1_offset` / `base+tex2_offset`; the consumer reads `last_tex` (must be 0 or 1, else skip), tries the texture mutex of `last_tex` then the other one with a zero-timeout wait, copies `pitch*cy` bytes (or per-row `min(pitch, texture pitch)` when pitches differ), then releases the mutex it holds. 16-bit formats `DXGI_FORMAT_B5G6R5_UNORM (85)` and `B5G5R5A1_UNORM (86)` are converted to BGRA 8-bit on the CPU.
- **R10 [SRC]** Shtex capture: `gs_texture_open_shared(shtex_data.tex_handle)`; hooks flip the image when `hook_info.flip` is set (render with V flip).
- **R11 [MODEL]** `convert_format(dxgi)`: 28→RGBA, 88→BGRX, 87→BGRA, 24→R10G10B10A2, 11→RGBA16, 10→RGBA16F, 2→RGBA32F, anything else → UNKNOWN (`gc_formats.script`). 85/86 are handled by R09 before this function (sixteen_bit=1).
- **R12 [SRC]** The capture is considered gone when the target process handle is signalled, or (non-"any fullscreen" mode) the window no longer exists; the plugin then stops capture, closes handles and retries target discovery on the next tick (§5.C).

### 5.B Window identity strings and matching [REAL]

- **R20** `ms_build_window_strings(str)`: NULL → all three outputs NULL. Otherwise split on `:` keeping empty parts; if fewer than three parts exist → all three NULL; extra parts (4th, ...) are ignored. Each of the first three parts is decoded: replace `#3A`→`:` **first**, then `#22`→`#` (order matters: `#223A` decodes to `#3A`). **QUIRK Q-2:** a part that is empty after decoding yields NULL (not `""`) for that output, while the other outputs are still produced (`":b:"` → title NULL, class `b`, exe NULL; `"::"` → all NULL) (`window_strings.script`).
- **R21** Encoding (used when building list values and by `reconfigure_audio_source`): in each part replace `#`→`#22` **first**, then `:`→`#3A`; join with `:` (`title:class:exe`). Round trip decode(encode(x)) = x for all non-empty parts.
- **R22** `ms_get_window_exe(window)`: false (and output untouched) when the window belongs to the current process. Otherwise true; output = text after the last `\` of the process image path (NT device path form), or `"unknown"` when the process cannot be opened, the path query fails, or the path contains no `\`. Title: empty if the window text length is 0, else the full UTF-8 title (no length limit). Class: window class name, truncated to 255 UTF-16 units.
- **R23** `ms_fill_window_list` visits top-level windows front-to-back and appends `{name: "[<exe>]: <title>", value: encoded window string}`. A window is skipped when: not visible; has `WS_EX_TOOLWINDOW`; has `WS_CHILD`; (mode `EXCLUDE_MINIMIZED` only) iconic, DWM-cloaked, or client width/height 0. After the validity filter, a *UWP host window* (class `ApplicationFrameWindow` or `WinUIDesktopWin32WindowClass`) is replaced by its first child window that belongs to a different process, if any (children found by `FindWindowEx` enumeration); that child is visited instead (the host itself is not), then enumeration resumes after the host. Per window `add_window` further skips: windows of the current process; exe in the internal list (case-insensitive exact: `startmenuexperiencehost.exe, applicationframehost.exe, peopleexperiencehost.exe, shellexperiencehost.exe, microsoft.notes.exe, systemsettings.exe, textinputhost.exe, searchapp.exe, video.ui.exe, searchui.exe, lockapp.exe, cortana.exe, gamebar.exe, tabtip.exe, time.exe`; or case-insensitive prefix `windowsinternal`); `explorer.exe` (case-sensitive compare! `Explorer.EXE` with an empty title is listed as `[Explorer.EXE]: (null)`) with an empty title; and any window rejected by an optional caller callback. (`window_enum.script`)
- **R24** Enumeration fallback: if `FindWindowEx(desktop, NULL)` returns no window, the search switches to `GetWindow(desktop, GW_CHILD)` + `GetNextWindow(GW_HWNDNEXT)` and must give the same result (`window_find_fallback.script`).
- **R25** Rating `window_rating(w, priority, class, title, exe)`; `cur_*` are the candidate's values; compares are case-insensitive; `title_val = |cmpi(cur_title, title)|` (0 equal, 1 different — `cmpi` returns −1/0/1); NULL class/title/exe compare as `""`:
  1. If the candidate's exe query fails (own process) → `0x7FFFFFFF`.
  2. `generic_class` (searched class contains `Chrome` or `SDL_app`, case-insensitive substring) and `priority == CLASS` → priority becomes `TITLE`.
  3. If `uwp_window` (searched class is exactly `Windows.UI.Core.CoreWindow` or `WinUIDesktopWin32WindowClass`, case-sensitive): `priority == EXE && !exe_match` → none; otherwise `title_val == 0 ? 0 : none` (class is ignored).
  4. `CLASS`: class equal ? `title_val` (+`0x1000` when exe differs) : none. `TITLE`: `title_val == 0 ? 0 : none`. `EXE`: exe equal ? `title_val` : none.
- **R26** `ms_find_window(mode, priority, class, title, exe)`: NULL `class` → NULL result. Candidate order = enumeration order of R23 (same filter, **including UWP-child substitution**); best = strictly lowest rating (first wins ties); a rating of 0 stops the search; returns NULL if every rating is `0x7FFFFFFF`. Invisible/tool/child windows never match; iconic/cloaked/empty windows match only in `INCLUDE_MINIMIZED`.
- **R27 QUIRK Q-3** `ms_find_window_top_level` enumerates only real top-level windows (`EnumWindows`), applies the same validity filter and **always** skips DWM-cloaked windows even in `INCLUDE_MINIMIZED`; it does **not** substitute UWP children (the host frame window itself is rated and returned). Enumeration continues while the rating is > 0, so it stops at the first rating-0 window (`window_find_modes.script`: top-level search returns the frame window 10 where `ms_find_window` returns child 11).
- **R28 [REAL]** `ms_check_window_property_setting(props, list, settings, key, idx)`: if the setting value is non-empty and equals no list item's value, insert at `idx` a **disabled** string item named `"[<exe>]: <title>"` (from the decoded window string) with value = the setting, and return true; otherwise false, list untouched. (`window_property.script`.) **QUIRK Q-9 / DEFECT D-1:** the reference formats the description with a NULL `%s` whenever the exe or title part is NULL/empty (an empty title, or a value with fewer than three parts). The Windows CRT prints the text `(null)` for that argument, so users see `"[Explorer.EXE]: (null)"` for untitled windows (also in R23 list names, vector `window_enum`) and `"[(null)]: (null)"` for malformed values. Required behaviour: reproduce the text `(null)` for an absent part (formally undefined C behaviour in the reference; a Rust implementation must hard-code it).

### 5.C Window capture (`window_capture`) [MODEL]

- **R40** Defaults: `method=0 (auto)`, `cursor=true`, `force_sdr=false`, `compatibility=false`, `client_area=true`; `window` and `priority` have **no** default (priority reads 0 = class) (`wc_settings.script`).
- **R41** Settings parsing: `window` via R20 (title, class, executable); `priority`, `cursor`, `capture_audio`, `force_sdr`, `compatibility`, `client_area` read as stored; `method` chosen by R42; audio child per R70. Updates are serialised against the render/tick thread by the source's update mutex.
- **R42** `choose_method(method, wgc_supported, class)`: if WGC unsupported → BitBlt (1). Else if `method != auto` → that method. Else if class is NULL → BitBlt. Else WGC (2) when the class **contains** (case-insensitive substring) `Chrome` or `Mozilla`, or **equals** (case-insensitive) one of: `ApplicationFrameWindow, Windows.UI.Core.CoreWindow, WinUIDesktopWin32WindowClass, GAMINGSERVICESUI_HOSTING_WINDOW_CLASS, XLMAIN, PPTFrameClass, screenClass, PodiumParent, OpusApp, OMain, Framework::CFrame, rctrl_renwnd32, MSWinPub, OfficeApp-Frame, WindowsWebview2Wrapper, SDL_app, WINDOWSCLIENT`; else BitBlt. (`SDL_app2` is **not** WGC.) The method is stored as chosen (not "auto") for property visibility and for the compatibility check (`source_type` = chosen method: BitBlt→`window_capture`, WGC→`window_capture_wgc`).
- **R43** Property visibility by chosen method: `cursor` visible if BitBlt, or WGC with cursor-toggle support; `compatibility` BitBlt only; `client_area` and `force_sdr` WGC only. Properties order: window (list; first entry `""`/"Select a window" plus R23 list with `INCLUDE_MINIMIZED`... [SRC for list mode]), method (list: auto 0, WGC 2, BitBlt 1 — in that display order), priority (title 1, class 0, exe 2 — display order), compat_info (text-info), capture_audio (only if WASAPI process capture is available), cursor, compatibility, client_area, force_sdr.
- **R44 [SRC]** Tick timers: window re-lookup every `WC_CHECK_TIMER = 1.0 s`, resize check every `0.2 s`, cursor check every `0.2 s`. Window lookup uses `ms_find_window(INCLUDE_MINIMIZED, priority, class, title, executable)` for BitBlt and WGC (WGC additionally requires `winrt_capture_init_window` success; failure sets `previously_failed` to avoid log spam). Capture area: client rect when `client_area` (WGC) / always client area for BitBlt; BitBlt uses `PrintWindow`/`BitBlt` according to `compatibility`.

### 5.D Game capture (`game_capture`) [MODEL]

- **R50** Defaults (`gc_settings.script`): `capture_mode="any_fullscreen"`, `priority=2 (exe)`, `sli_compatibility=false`, `capture_cursor=true`, `allow_transparency=false`, `premultiplied_alpha=false`, `limit_framerate=false`, `capture_overlays=false`, `anti_cheat_hook=true`, `hook_rate=1 (normal)`, `rgb10a2_space="srgb"`. `window` has no default.
- **R51** Mode parsing: if the deprecated key `capture_any_fullscreen` has a **user** value *and* `capture_mode` has **no user value** then mode = `any_fullscreen` if the old bool is true else `window`; otherwise read `capture_mode`. String `"window"` → WINDOW, `"hotkey"` → HOTKEY, anything else (including `""`, other case) → ANY. A default never counts as a user value. A write of `capture_mode` (on property init) migrates old files.
- **R52** Config derivation: window string via R20; `force_shmem = sli_compatibility`; `cursor = capture_cursor`; `is_10a2_2100pq = (rgb10a2_space == "2100pq")` (exact, case-sensitive); `priority` and `hook_rate` read as raw ints (unvalidated).
- **R53** Property visibility: `window` and `priority` visible iff mode is WINDOW (deprecated format: iff `capture_any_fullscreen` is false). `hotkey_start` / `hotkey_stop` hotkeys exist for HOTKEY mode.
- **R54** `hook_rate_to_float`: 0 slow → 2.0, 1 normal → 1.0, 2 fast → 0.5, 3 fastest → 0.1, any other → 1.0 (seconds between target-search attempts) (`%.9g` prints 0.100000001 for the f32 value).
- **R55 [SRC]** Mode semantics: ANY – the foreground fullscreen (or "window covering a whole monitor") process that is not excluded becomes the target; WINDOW – target found by `ms_find_window(INCLUDE_MINIMIZED, priority, class, title, exe)`; HOTKEY – target is the foreground window at the moment the start hotkey fires, until the stop hotkey. Windows of the OBS process, blacklisted classes/exes and UWP hosts are rejected. Compatibility warnings use R90 with `GAME_CAPTURE`.

### 5.E Monitor / display capture [MODEL]

- **R60** `monitor_capture` defaults: `monitor=0`, `capture_cursor=true`, `compatibility=false`. `display_capture` defaults: `method=0 (auto)`, `monitor_id="DUMMY"`, `monitor_wgc=0`, `capture_cursor=true`, `force_sdr=false` (`monitor_settings.script`).
- **R61** Display capture method (`DXGI=1`, `WGC=2`, `auto=0`): if WGC unsupported → DXGI. If auto: DXGI by default; **WGC** if the monitor has no DXGI duplicator index (−1); or if running on battery (`GetSystemPowerStatus` ok and `BatteryFlag < 128`) with ≥ 2 graphics adapters. Failure of `GetSystemPowerStatus`, BatteryFlag ≥ 128 (no battery / unknown) → stay DXGI. An explicit method is never changed (apart from the unsupported rule).
- **R62 [SRC]** `monitor_id` lookup: enumerate monitors, match by device id string; if none matches, fall back to the first monitor (`enum_monitor_fallback`); `"DUMMY"` is the invalid-display placeholder. DXGI duplication resets every `RESET_INTERVAL_SEC = 3.0` s after failure (Access lost, mode change).

### 5.F Audio helper for capture sources [MODEL]

- **R70** `setup_audio_source(parent, child, window, enabled, priority)`: when enabled and no child exists, create a **private** `wasapi_process_output_capture` source named `"<parent name> (<capture audio suffix>)"` with settings `window` (if non-NULL) and `priority`, add it as an active child, send it `reroute_audio(target=parent)` and mark the parent audio-active. If a child exists and `window` is non-NULL, update it only when `settings_changed`: `old_window != new_window` (byte compare) **or** (`old_priority != new_priority` **and** the new window string is non-empty) (`audio_helpers.script`). When disabled: parent audio inactive; if a child exists, `reroute_audio(NULL)`, remove active child, release, clear. Renaming the parent renames the child to `"<new name> (<suffix>)"`.
- **R71** `reconfigure_audio_source(source, hwnd)` updates the child with `window = encode(title:class:exe)` of the window (R21) and `priority = CLASS`.

### 5.G WASAPI sources [MODEL unless noted]

- **R80** Source types: input (`wasapi_input_capture`; flags AUDIO|DO_NOT_DUPLICATE), device output (`wasapi_output_capture`; + DO_NOT_SELF_MONITOR), process output (`wasapi_process_output_capture`; + DO_NOT_SELF_MONITOR). Defaults (`wasapi_defaults.script`): input `device_id="default"`, `use_device_timing=false`; device output `device_id="default"`, `use_device_timing=true`; process output none.
- **R81** Update parameters: `device_id` string; `use_device_timing`; `is_default = (device_id equals "default" ignoring case)`; `priority` int; for **non-input** sources `window` decoded by R20 into class/title/executable (missing parts → empty strings; input sources keep them empty).
- **R82** Restart decision on `Update`: process output → restart iff `priority`, `class`, `title` or `executable` changed; input and device output → restart iff `device_id` differs (**byte-exact, case-sensitive**: `"DEFAULT"` ≠ `"default"` restarts although both mean default). `use_device_timing` alone never restarts. A restart signals the capture thread (the current client is torn down and re-initialised). A device-output source whose `device_id` changed additionally raises `obs_source_audio_output_capture_device_changed(new id)`. Log line `update settings` per type (`wasapi_update.script`).
- **R83** Default-device tracking: ignored unless `is_default`. Input accepts only (`flow=eCapture`, `role=eCommunications`); output/device-output only (`eRender`, `eConsole`). A non-null id different from the tracked id (initially empty) is stored and restarts; the same id does nothing; a null id restarts only if something was tracked, then clears (`wasapi_default_device.script`). Note the tracked id is **not** seeded at construction, so the first default-change notification after creation always restarts.
- **R84** Device lookup: `is_default` → default endpoint (input: `eCapture`/`eCommunications`; output: `eRender`/`eConsole`), else the endpoint with the given id (UTF-8 → UTF-16). Process output: `ms_find_window(INCLUDE_MINIMIZED, priority, class, title, exe)`; fail "Failed to find window" if none; the process id is taken from the window; the virtual device `[VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK]` is activated with `PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE` [SRC].
- **R85** Client format: device sources use the endpoint mix format; process loopback uses a synthesised format: 32-bit float, channels and rate of the **OBS audio output** (`get_audio_channels(oai.speakers)`, `oai.samples_per_sec`), `WAVE_FORMAT_EXTENSIBLE` with `dwChannelMask = layout_to_ks(oai.speakers)`, `nBlockAlign = channels*4`, `nAvgBytesPerSec = rate*blockAlign`, `cbSize = 22`. Output and process sources initialise the client with `AUDCLNT_STREAMFLAGS_LOOPBACK`, event callback, shared mode, buffer duration 5 s (`5*10^7` 100-ns units); device output additionally runs `ClearBuffer` (plays one buffer of silence through a separate render client) to avoid loopback stalling during silence [SRC].
- **R86** Layout mapping from the endpoint format (`wasapi_layout.script`): channel mask is read only if `wFormatTag == WAVE_FORMAT_EXTENSIBLE` (else 0). Mask `0x0B` → 2.1 (3), `0x107` → 4.0 (4), `0x10F` → 4.1 (5), `0x60F` → 5.1 (6), `0x63F` → 7.1 (8); **any other mask (including 0, stereo 0x3, mono 0x4) → layout number = channel count**. **QUIRK Q-1:** 7 channels yields number 7 and >8 channels yields numbers that are not valid `speaker_layout` values; 1 → mono, 2 → stereo, 3 → 2.1, 4 → 4.0, 5 → 4.1, 6 → 5.1, 8 → 7.1 by coincidence of numbering. Sample format is always float; rate is `nSamplesPerSec`.
- **R87** Reverse mapping `layout_to_ks`: stereo `0x3`, 2.1 `0x0B`, 4.0 `0x107`, 4.1 `0x10F`, 5.1 `0x60F`, 7.1 `0x63F`; any other layout value (mono, unknown, invalid) is passed through as the numeric mask value.
- **R88 [SRC]** Reconnect: the capture thread retries initialisation every `RECONNECT_INTERVAL = 3000` ms after failure; for process output the source also stops (`unhooked`) when the target window disappears (`!IsWindow(hwnd)`); `hooked` is emitted once on first successful init with title/class/executable of the window.
- **R89 [SRC]** Property lists: input and device output list `Default` (value `"default"`) followed by enumerated active endpoints (`name`, `id`); process output lists windows via R23 with `INCLUDE_MINIMIZED`, priority list (title 1, class 0, exe 2), and calls the window-changed callback that re-runs `OnWindowChanged` (same restart rule as R82) and R28.

### 5.H Compatibility list (`compat-helpers.c`) [REAL]

- **R90** `check_compatibility(title, class, exe, type)` consults the entries array of `compatibility.json`, searched as `<module config path>/compatibility.json` first, then the module data file. The file must parse as JSON **without duplicate keys**, have integer `format_version == 1`, and contain a key `entries`; otherwise the result is NULL (parse error → warning, wrong version → debug, missing list → warning). The parsed list is cached for the process; failures are not cached.
- **R91** Entries are scanned in order; an entry is skipped unless its boolean flag for the source type is exactly `true`: `game_capture` / `window_capture` (BitBlt) / `window_capture_wgc` (missing, non-boolean → false). `match_flags` bits: 1 executable, 2 title, 4 class. For each set bit all must hold: class `strcmp`-equal (case-sensitive) and non-NULL input; exe case-insensitive equal and non-NULL input; title: the **window title starts with the entry title**, case-insensitive (`strncasecmp(win_title, entry_title, len(entry_title))`), non-NULL input. Bits not set are not tested. A set bit whose entry field is missing/NULL crashes the reference (precondition: well-formed list). First matching entry wins.
- **R92** Result message: `message` replaced by the translation of `translation_key` when found; if the entry has `name` and the message contains `%`, every `%name%` is replaced by it; if `url` starts with `https://` append `"<br>\n<a href=\"URL\">URL-without-scheme</a>"`. `severity` = integer field (0 info, 1 warning, 2 error as `obs_text_info_type`). (`compat_check.script`)
- **R93 DEFECT D-2** `compat_json_free()` decrements the cached list but leaves the pointer set; a later `check_compatibility` would use freed memory. Required: reset the cache to empty on free.

### 5.I DirectShow video capture (`dshow_input`) [MODEL unless noted]

- **R100** Defaults (`dshow_defaults.script`): `frame_interval = -1` (FPS_MATCHING), `res_type = 0` (device-preferred), `video_format = VideoFormat::Any` (numeric value defined by libdshowcapture; **not available in this repository snapshot**, see §11), `active = true`, `color_space = "default"`, `color_range = "default"`, `audio_output_mode = 0` (capture), `autorotation = true`, `hw_decode = false`. `buffering`, `flip_vertically`, `deactivate_when_not_showing`, `use_custom_audio_device`, `audio_device_id`, `video_device_id`, `resolution`, `last_*` have no default. Other setting keys: `res_type`, `resolution` ("WxH"), `frame_interval`, `video_format`, `last_video_device_id`, `last_resolution`, `buffering` (0 auto, 1 on, 2 off), `color_space` (`default|709|601|2100PQ|2100HLG`-ish strings; [SRC]), `color_range` (`default|partial|full`).
- **R101** Resolution strings: `sscanf("%dx%d")` == 2 required (leading blanks skipped, trailing text ignored, `X` uppercase invalid, negative heights allowed); the empty string is invalid for `ResolutionValid`. In configuration `cy < 0` means *flip vertically*: `cy_abs = |cy|`, `cy_flip = cy < 0` (`dshow_resolution.script`).
- **R102** `DetermineResolution` (property side) tries in order: autoselect value of `resolution` (if present and available), the `resolution` setting, the `last_resolution` setting; each must parse and be available on the device; otherwise none [SRC for the property UI].
- **R103** Available-resolution list (`InsertResolution`): for each capability insert (min) then (max) point into a list ordered by `cx`; duplicates are dropped; among equal `cx` the new entry is placed before the first entry whose `cy` is greater than the first-seen `cy` of that `cx` — **QUIRK Q-5:** the list is *not* totally ordered by `cy` (`640x480 640x360 640x100 640x600`) and the order depends on insertion order; keep the algorithm, do not "fix" the sorting (`dshow_resolution.script`).
- **R104** Custom-config selection (`res_type == 1`): fail ("`ResolutionValid failed`") if `resolution` invalid. `interval` = autoselect int of `frame_interval` if it exists else the setting; `-1` → OBS fps interval `fps_den*10^7/fps_num` (R106), `0` = highest. Over all device capabilities, evaluate matchers **in order and short-circuit per capability**: (1) resolution within `[minCX,maxCX]×[minCY,maxCY]`; (2) format: requested `Any`, capability `Any`, or equal — sets `format_match`; (3) closest-interval selector (always true): `cur = clamp(interval, minInterval, maxInterval)`, replace `best` when `|interval-best| > |interval-cur|` (initial `best = i64::MAX`); (4) frame-rate available: `interval ∈ {0,-1}` or `minInterval-20 ≤ interval ≤ maxInterval+20`. `caps_match` = some capability passed all four. Failure ("Video format match failed") iff `!caps_match && !format_match`; otherwise `interval = best` — **QUIRK Q-6:** a requested rate outside every range is accepted and clamped (`dshow_caps_select.script`). The format matcher only runs on capabilities whose resolution matched, so a format mismatch on a non-matching resolution never sets `format_match`.
- **R105** `res_type == 0` (preferred): no resolution/format/interval is requested (`cx=cy=0`, interval 0, format Any, `useDefaultConfig = true`); after `SetVideoConfig` the device-reported `cx`/`cy` must both be non-zero or the update fails with an error log.
- **R106** `GetOBSFPS` interval = `fps_den * 10_000_000 / fps_num` as 64-bit integer division; `0` when no video info. Table of selectable rates (text → interval): 360→`10^7/360`, 240, 144, 120, `119.88 NTSC`→`120000/1001`, 100, 60, `59.94 NTSC`, 50, `48 film`→`48000/1001`, 40, 30, `29.97 NTSC`, 25, `24 film`→`24000/1001`, 20, 15, 10, 5, 4, 3, 2, 1 where integer fps `n` → `10^7 / n` and fractional ones `1001*10^7/num` (`dshow_fps.script`: 30 fps 333333, 29.97 333666, 59.94 166833, 23.976 417083).
- **R107** Buffering: `buffering` 0 (auto) → buffered iff the device is *delayed*: configured format value is greater than MJPEG in libdshowcapture's order (i.e. an encoded/H.264-like format), or the device name contains (case-insensitive) `elgato game capture hd` or `stream engine`; 1 → buffered; 2 and any other value → unbuffered. `async_unbuffered = !buffered`. Independently, devices whose name contains `GV-USB2` (case-insensitive) set `async_decoupled` (`dshow_buffering.script`; the "format greater than MJPEG" input is passed by the script, its numeric source is [SRC]).
- **R108** Format conversions (`ConvertVideoFormat`): ARGB→BGRA, XRGB→BGRX, I420→I420, YV12→I420 (planes swapped by the decoder path), NV12→NV12, Y800→Y800, YVYU→YVYU, YUY2→YUY2, UYVY→UYVY, HDYC→UYVY, P010→P010, anything else→NONE (frame dropped). Audio: Wave16bit→16-bit, WaveFloat→float, else unknown. Channel count → layout: 0→unknown, 1→mono, 2→stereo, 3→2.1, 4→4.0, 5→4.1, 6→5.1, 8→7.1, other (including 7)→unknown. [SRC, no vector]
- **R109 [SRC]** Encoded video (MJPEG/H.264 from the device): decoder (ffmpeg) is recreated when the codec id or the `hw_decode` setting changes; init failure drops the frame with a warning. ffmpeg log messages of level warning/error/fatal are forwarded at warning level with prefixes `warning: `, `error:   `, `fatal:   `; the message `unable to decode APP fields: %s` is logged once only.
- **R110 [SRC]** Audio: `audio_output_mode` 0 capture / 1 DirectSound / 2 WaveOut; `use_custom_audio_device` + `audio_device_id` selects a separate audio device; an audio device attached to the video device is used otherwise (`deviceHasAudio`, `separateAudioFilter`); audio timestamps are matched to video; `active` and `deactivate_when_not_showing` control opening the device only while visible.
- **R111 [SRC]** Device identity: `video_device_id` encodes name and path (libdshowcapture `EncodeDeviceId`, separator and escaping **unknown here**); a device matches iff both decoded name and path are equal; decode failure aborts the update ("DecodeDeviceId failed").

### 5.J DirectShow hardware encoders (`dshow_c985_h264`, `dshow_c353_h264`) [MODEL]

- **R120** Device selection: first enumerated encoder device whose name contains `C985` / `C353`. Settings: `bitrate` (default 1000, UI range 1000..60000 step 1), `keyint_sec` (0 → 2).
- **R121** Output size is forced to **1024×768** if `|aspect − 1024/768| < |aspect − 1280/720|` else **1280×720**, `aspect = width/height` in double. Frame interval `frameInterval = fps_den * 10_000_000 / fps_num` and keyframe interval `keyint = keyint_sec * fps_num / fps_den` (frames). **DEFECT D-3:** both products are evaluated in **unsigned 32-bit**; `fps_den * 10^7` wraps for `fps_den > 429` (e.g. 30000/1001 yields 47335 instead of 333666). Required behaviour: compute in 64-bit signed (`333666`); the vector pins the reference's wrapped value, a conforming implementation SHOULD deviate and the discrepancy is recorded (`dshow_encoder.script`, lines 24000/1001, 30000/1001, 60000/1001).
- **R122** `get_video_info`: encoder input is always I420. If the requested format is I420 and the size is exactly 1280×720 or 1024×768, nothing changes; otherwise format := I420, range := default, colour space := default and size := nearest by R121.
- **R123** Encode: the planes are passed with chroma planes swapped (YV12 order); `pts/dts` are multiplied/divided by `frameInterval`; keyframe flag from the NAL scan (`obs_avc_keyframe`); the **first** packet is split: SPS/PPS NAL units (types 7, 8; including their start codes) go to the encoder `extra_data` (header), all other NALs stay in the packet. Extra data is available only after the first packet (`get_extra_data` returns false while empty). [SRC, no vector]
- **R124 [SRC]** `UpdateDShowEncoder` reconfigures only while the encoder is inactive.

### 5.K Virtual-camera queue (`shared-memory-queue.c`) [REAL]

- **R130** `video_queue_create(cx, cy, interval)`: fails (NULL) if a mapping named `OBSVirtualCamVideo` already exists (opened for read); otherwise creates a pagefile mapping sized per §4.4 (frame size is `DWORD cx*cy*3/2`, **no overflow check**), writes the header with `state = STARTING`, `cx`, `cy`, `interval`, offsets, `type = 0`, everything else zero, and returns a writer.
- **R131** `video_queue_open()` opens the mapping read-only; NULL if absent. `video_queue_get_info` returns header `cx, cy, interval`.
- **R132** `video_queue_write(planes, linesizes, ts)`: `inc = ++write_idx` (plain, non-atomic increment of a volatile); slot = `inc % 3`; stores `ts` in the slot header; copies `linesize[0]*cy` luma bytes then `linesize[0]*cy/2` chroma bytes (the chroma plane must therefore be stored with the same linesize); then `read_idx = inc` and `state = READY` (in that order, no barriers). (`vqueue.script`)
- **R133** `video_queue_state(vq)`: NULL → INVALID. For a reader, the first time it observes `READY` it computes the slot pointers from the header's `offsets` and marks itself ready. **DEFECT D-4 (precondition):** calling `video_queue_read` before `video_queue_state` returned READY once dereferences unset pointers (crash). Required: a Rust reader must either return `false` or lazily initialise.
- **R134** `video_queue_read(scale, dst, &ts)`: false if `state == STOPPING`; reads `read_idx`; if equal to the previously seen index, increments a counter and returns false when the counter reaches **10** (so the tenth repeat and every following repeat of an unchanged index fail; the first nine repeats succeed and return the same frame), otherwise resets the counter and remembers the index; picks slot `read_idx % 3`, returns its timestamp and scales/copies the NV12 frame into `dst` with `nv12_do_scale` (identical size = copy; dst is `dst_cx*dst_cy*3/2` bytes). (`vqueue_reader.script`: 1 new + 9 repeats succeed, then false; a new write resets.)
- **R135** `video_queue_close`: a writer first sets `state = STOPPING`; then the view is unmapped and the handle closed; the mapping disappears when the last handle closes (after the writer and all readers closed, `video_queue_open` returns NULL and a new writer may be created).

## 6. Algorithms

Everything normative is in §5; the only multi-step algorithms are R23/R26 (window enumeration and rating), R104 (capability selection) and R103 (resolution list). Pseudocode for R104:

```
select(cx, cy, interval, fmt, obs_interval):
  if interval == -1: interval = obs_interval
  best = i64::MAX; fmt_match = false; caps_match = false
  for cap in device.caps:                      # no early exit; side effects intended
      ok = cap.min_cx <= cx <= cap.max_cx and cap.min_cy <= cy <= cap.max_cy
      if ok:
          fm = fmt == Any or cap.fmt == Any or fmt == cap.fmt
          fmt_match |= fm; ok = fm
          if ok:
              cur = max(cap.min_i, min(interval, cap.max_i))          # clamp
              if |interval - best| > |interval - cur|: best = cur      # saturating arithmetic advised
              ok = interval in (0, -1) or cap.min_i-20 <= interval <= cap.max_i+20
      caps_match |= ok
  if not caps_match and not fmt_match: fail
  return best
```

## 7. Concurrency

- Window capture: `update_mutex` guards settings strings against the video thread; property callbacks reuse `update_settings`. Game capture runs target discovery and injection on a worker thread that is joined/restarted on update (`stop_capture` from update thread) [SRC].
- WASAPI: one capture thread per source (RTWQ work items when available), controlled by events (`stopSignal`, `restartSignal`, `receiveSignal`, `idleSignal`, `reconnectExitSignal`); `Update` runs on the UI/update thread and only mutates fields then sets `restartSignal`; the string fields are copied by value so races are benign only because restart is edge-triggered [SRC]. Default-device notifications arrive on a COM notification thread and call `SetDefaultDevice`, which touches `default_id` without a lock (reference race; use a mutex).
- Hook-info and shmem are shared across processes without locks: the hook writes the info struct, then signals `HookReady`; the plugin treats the info as stable once the event is signalled. `shmem_data.last_tex` is volatile and each texture is guarded by its named mutex. The virtual-camera queue is lock-free single-writer/multi-reader with the torn-read risk described in R132/R134 (readers compare the index twice implicitly; a torn frame is possible).

## 8. Reference defects and quirks

| Id | Kind | Description | Required behaviour |
|---|---|---|---|
| Q-1 | QUIRK | Unknown channel mask maps to layout = channel count (7 → invalid 7) | match, but clamp unsupported counts to "unknown/stereo" at the audio-API boundary |
| Q-2 | QUIRK | Empty window-string part decodes to NULL | match (treat as empty string consumer-side) |
| Q-3 | QUIRK | `find_top_level` ignores UWP children and always skips cloaked windows | match |
| Q-5 | QUIRK | Resolution list not totally ordered | match |
| Q-6 | QUIRK | Out-of-range frame rate accepted and clamped | match |
| Q-7 | QUIRK | `explorer.exe` empty-title filter compares case-sensitively while all other exe tests are case-insensitive | match |
| Q-8 | QUIRK | `device_id == "default"` compare is case-insensitive for `is_default`, case-sensitive for restart | match |
| D-1 / Q-9 | DEFECT/QUIRK | NULL passed to `%s` for empty title or malformed window strings (prints `(null)` on Windows) | reproduce `(null)` |
| D-2 | DEFECT | `compat_json_free` leaves a dangling cache pointer | reset to empty |
| D-3 | DEFECT | 32-bit wrap in DirectShow encoder `frameInterval` (and `keyint` overflow for huge values) | use 64-bit math |
| D-4 | DEFECT | `video_queue_read` before READY dereferences unset pointers | return false / lazy init |
| D-5 | DEFECT | `video_queue_create` frame size has no overflow/zero check; `cx*cy*3/2` truncates for odd sizes | reject zero or odd dimensions explicitly (not covered by vectors) |

## 9. Implementation notes (non-normative)

- The hook DLL ABI (R01) forces C layout: use `#[repr(C, packed(8))]`-equivalent (`repr(C)` with explicit `u8` bool fields and explicit padding; do not use Rust `bool` for fields written by foreign processes).
- Prefer `windows`/`windows-sys` crates for Win32; model the window table behind a trait so §5.B can be tested on Linux exactly as `wincapture_runner.c` does (fake `FindWindowEx`, `GetClassName`, ...). The vectors in `window_*.script` drive such a trait unchanged.
- Treat `0x7FFFFFFF` rating as `i32::MAX` and keep `+0x1000` arithmetic in `i32` (no overflow possible: `title_val ≤ 1`).
- Keep window-string encode/decode order exactly (R20/R21); it is the on-disk format of saved scenes.
- Match the fixed internal-exe list (R23) literally; it is behaviour users see in the window dropdown.

## 10. Conformance coverage

Runner: `test/conformance/gen/wincapture_runner.c` (+ `wincapture.sources`, `wincapture_shim/`). 28 scripts, vectors in `test/conformance/vectors/wincapture/`. Tag = how the vector was produced (R = reference code, M = model).

| Rules | Script(s) | Tag |
|---|---|---|
| R01, R02, R03 | `hook_layout`, `hook_bytes` | R |
| R07, R08, §4.3 | `hook_protocol_model` | M |
| R11 | `gc_formats` | M |
| R20, R21 | `window_strings` | R |
| R22, R24 | `window_find_fallback` | R |
| R23 | `window_enum` | R |
| R25, R26, R27 | `window_find`, `window_find_modes` | R |
| R28, D-1 | `window_property`, `window_enum` | R |
| R40–R43 | `wc_settings` | M |
| R50–R54 | `gc_settings` | M |
| R60, R61 | `monitor_settings` | M |
| R70 | `audio_helpers` | M |
| R80, R81, R82 | `wasapi_defaults`, `wasapi_update` | M |
| R83 | `wasapi_default_device` | M |
| R86, R87 | `wasapi_layout` | M |
| R90–R93 | `compat_check` | R |
| R100, R120 | `dshow_defaults` | M |
| R101, R103 | `dshow_resolution` | M |
| R104 | `dshow_caps_select` | M |
| R106 | `dshow_fps` | M |
| R107 | `dshow_buffering` | M |
| R121, R122, D-3 | `dshow_encoder` | M |
| R130–R135 | `vqueue`, `vqueue_reader`, `vqueue_scale` | R |
| R04–R06, R09, R10, R12, R44, R55, R62, R71, R84, R85, R88, R89, R102, R105, R108–R111, R123, R124 | none | SRC only |

Floating point: the only printed floats are `hook_rate` multipliers (`%.9g`); compare exactly.

## 11. Open questions / not covered

1. `VideoFormat` / `AudioFormat` values, `DeviceId` encoding, and device-capability enumeration live in `libdshowcapture` (now initialized in this checkout, `deps/libdshowcapture/src`, no-atl branch). Pinned from `dshowcapture.hpp`: `VideoFormat` (:77) = Any 0, Unknown 1, ARGB 100, XRGB 101, RGB24 102, I420 200, NV12 201, YV12 202, Y800 203, P010 204, YVYU 300, YUY2 301, UYVY 302, HDYC 303, MJPEG 400, H264 401, HEVC 402 — so R107's `> MJPEG` rule means `> 400` (H264/HEVC). `AudioFormat` (:105) = Any 0, Unknown 1, Wave16bit 100, WaveFloat 101, AAC 200, AC3 201, MPGA 202. `DeviceId` encoding and capability enumeration remain Windows-runtime behaviour; the vectors keep opaque tokens for the device side.
2. R04–R06/R09–R10/R12: needs a Windows host with a hooked game to validate; `get-graphics-offsets` output format for ddraw/d3d12 offsets is not read by the plugin (R06) — confirm whether the hook computes them itself.
3. `tiny-nv12-scale` interpolation and UYVY/YUY2 targets (only the dimension contract is vector-tested with uniform frames).
4. Whether D-3's wrapped value must be preserved for compatibility: no stored data depends on it, so the spec recommends the corrected arithmetic.
5. Rendering/HDR (`GS_CS_*` selection in `game_capture_render`), `dc-capture`, `cursor-capture`, WGC frame path, injection, AppContainer object paths, WASAPI timestamp logic (`OnStartCapture`, device-timing vs system-timing) and DShow audio/video timestamp matching were **not** specified (cost control: breadth first on protocol, matching and selection rules).
6. Sufficiency check (Rust re-implementation from this spec and the vectors) was **skipped** by instruction (cost control); no pass count is claimed.
7. Round 10 second-author check (2026-11-05): encoder ids corrected to the 33.0.1 values `dshow_c985_h264` / `dshow_c353_h264` (both registered from `win-dshow-encoder.cpp`; the old `obs_dshow_c985` / `obs_dshow_c353` names predate the merge). Known remaining cosmetic issue: the bare `R120–R131` (section 5) and `S-051-R120–R131` (round 8) series overlap numerically.

## 12. Round 8: capture host surface — game/window/monitor/duplicator/cursor/dc/WGC/injection (added)

§5 R04–R107 pin the hook IPC protocol, window matching and vqueue rules; this section
pins the host-side files those rules live in, plus the paths §11 item 5 left un-specified
(rendering/HDR, dc-capture, cursor-capture, WGC frame path, injection). The hook DLL
itself is specified by S-086 (kept C++, D-08); this section is the host side only.

### Reference files (all cited)

- `plugins/win-capture/game-capture.c`, `plugins/win-capture/game-capture-file-init.c`, `plugins/win-capture/window-capture.c`, `plugins/win-capture/duplicator-monitor-capture.c`, `plugins/win-capture/monitor-capture.c`, `plugins/win-capture/dc-capture.c`, `plugins/win-capture/cursor-capture.c`, `plugins/win-capture/app-helpers.c`, `plugins/win-capture/audio-helpers.c`, `plugins/win-capture/nt-stuff.c`, `plugins/win-capture/plugin-main.c`
- `libobs-winrt/winrt-capture.cpp`, `libobs-winrt/winrt-dispatch.cpp`
- `shared/obs-d3d8-api/d3d8.h`, `shared/obs-inject-library/inject-library.c`

### 12.1 Game-capture host

- **S-051-R120 [SRC]** `game-capture.c` owns the game-capture object: window matching
  per R04–R12; `game_capture_update` re-resolves the process and re-configures the hook
  (capture type, colour format, delay) on settings change; the hook message protocol is
  §4/§5. Hook internals (offset tables, renderer hooks) are S-086.
- **S-051-R121 [SRC]** Injection: the hook DLL is staged to a unique temp file
  (`game-capture-file-init.c`: copy + naming, cleanup on unload) and injected per S-086
  §injection; failure → error state + property error text. `inject-library.c` is the
  shared injection library (CreateRemoteThread-style flow, cross-ref S-086).
- **S-051-R122 [SRC]** `nt-stuff.c` resolves NT-native functions by name
  (`NtOpenMutant`, `NtOpenEvent`, `NtOpenSection`, `NtQuerySystemInformation`,
  `NtCreateMutant`, `RtlNtStatusToDosError`, `RtlInitUnicodeString`) to open the hook's
  named IPC objects (mutex/event/section) cross-process; a missing function disables the
  affected path (logged, capture unavailable).
- **S-051-R123 [SRC]** `audio-helpers.c`: per-window audio device selection helper
  (default vs specific device, feeding the audio rules in §5); `app-helpers.c`:
  window-class/app-path helpers used by the matchers.

### 12.2 Window-capture host

- **S-051-R124 [SRC]** `window-capture.c`: same hook protocol as game capture but
  window-only (no per-process hook state machine); colour conversion and delay per the
  §5 rules; bounds from the window rectangle with monitor-offset handling.

### 12.3 Monitor capture (DXGI + WGC)

- **S-051-R125 [SRC]** `duplicator-monitor-capture.c`: method enum
  DXGI (legacy duplicator) / WGC; `choose_method(requested, wgc_supported, monitor)`:
  WGC unsupported → DXGI; per-monitor restrictions fall back to DXGI (Q1); the chosen
  method persists in the `method` setting (label `Method.WindowsGraphicsCapture` for
  WGC).
- **S-051-R126 [SRC]** DXGI path (`monitor-capture.c`): per-monitor DXGI output
  duplication (S-078 R24–R25), cursor overlay option, scaling for multi-monitor setups.
- **S-051-R127 [SRC]** WGC path (`winrt-capture.cpp`, `winrt-dispatch.cpp`): wraps
  Windows.Graphics.Capture (create for monitor/window, frame pool, Closed/Failed
  events); `winrt-dispatch.cpp` pumps the WinRT events on its thread; frames land as
  textures (S-078); first-use permission request is a Windows runtime flow (Q2).
- **S-051-R128 [SRC]** `dc-capture.c`: DirectComposition window capture
  (Windows 10 2004+): captures the window's DComp surface by HWND; used as the
  "direct composition" method of game capture, falling back to window capture when the
  OS build does not support it.

### 12.4 Cursor overlay

- **S-051-R129 [SRC]** `cursor-capture.c`: when cursor capture is enabled and the
  cursor is inside the capture bounds, each frame reads the cursor bitmap
  (XOR colour + AND mask): the AND mask is inverted into alpha (`bit_to_alpha` with
  invert), XOR supplies the colour; a 32-bit bitmap with its own alpha channel is used
  as-is; a zero AND mask means opaque. The cursor sprite renders at the cursor position
  scaled by the monitor DPI and capture scale.

### 12.5 D3D8 API + plugin glue

- **S-051-R130 [SRC]** `shared/obs-d3d8-api/d3d8.h`: the D3D8-compatible surface
  (function-pointer tables + hook trampoline types) the hook uses for D3D8 games; the
  host side only references it for offset-table layout (hook behaviour: S-086).
- **S-051-R131 [SRC]** `plugin-main.c`: registers the capture source ids
  (game/window/monitor/dc) and loads the hook DLL path (plugin dir or temp staging,
  R121).

### 12.6 Quirks (added)

- Q1: exact per-monitor WGC restrictions in `choose_method` (which virtual outputs are
  rejected and why).
- Q2: WGC permission-request flow (user consent dialog) — Windows runtime behaviour,
  not derivable from the repository.
- Q3: `nt-stuff.c` fallback list when an NT function is absent (which paths degrade).

## 13. Round 9: DirectShow host/virtual-camera + WASAPI host (added)

§5.I–§5.K pin the dshow_input settings/capability behaviour, the C985/C353 encoders and the
shared-memory virtual-camera queue; §5.G pins the WASAPI sources. This section pins the host-side
files those rules live in (the DirectShow input object and its action loop, the device-id codec,
the virtual-camera output and driver, the plugin glue) and the four WASAPI host files. One section
per action target, with subsections.

### Reference files (all cited)

- `plugins/win-dshow/win-dshow.cpp`, `plugins/win-dshow/win-dshow-encoder.cpp`, `plugins/win-dshow/virtualcam.c`, `plugins/win-dshow/virtualcam-module/virtualcam-module.cpp`, `plugins/win-dshow/virtualcam-module/virtualcam-filter.cpp`, `plugins/win-dshow/virtualcam-module/placeholder.cpp`, `plugins/win-dshow/virtualcam-module/sleepto.c`, `plugins/win-dshow/encode-dstr.hpp`, `plugins/win-dshow/dshow-plugin.cpp`, `plugins/win-wasapi/win-wasapi.cpp`, `plugins/win-wasapi/wasapi-notify.cpp`, `plugins/win-wasapi/enum-wasapi.cpp`, `plugins/win-wasapi/plugin-main.cpp`

### 13.1 DirectShow host (`win-dshow.cpp`)

- **S-051-R136 [SRC]** The DirectShow work runs on one OS thread per input ("win-dshow: DShowThread",
  `CoInitialize`'d): `DShowLoop` loops on `MsgWaitForMultipleObjects(1, semaphore, INFINITE,
  QS_ALLINPUT)` — a window message is pumped (`PeekMessage`/`TranslateMessage`/`DispatchMessage`
  until empty) and re-waited; the semaphore hands out actions from a critical-section-guarded
  queue; anything else exits the loop. `enum class Action { None, Activate, ActivateBlock,
  Deactivate, Shutdown, ConfigVideo, ConfigAudio, ConfigCrossbar1, ConfigCrossbar2 }`.
  Activate/ActivateBlock: take the source settings; `Activate(settings)` — on failure
  `obs_source_output_video2(source, nullptr)` (blank frame); ActivateBlock additionally signals the
  activated event. Deactivate → `Deactivate()`; Shutdown → `device.ShutdownGraph()` and return
  (thread exit); ConfigVideo/ConfigAudio/ConfigCrossbar1/ConfigCrossbar2 →
  `device.OpenDialog(nullptr, DialogType::ConfigVideo/ConfigAudio/Crossbar/Crossbar2)`. All
  §5.I behaviour (open/configure/decode/output) executes on this thread — the "single thread per
  device" invariant the comment states. `FPS_HIGHEST = 0` / `FPS_MATCHING = -1` are the file-local
  interval constants behind R104/R106.

### 13.2 Device-id codec, encoders, plugin glue

- **S-051-R137 [SRC]** `encode-dstr.hpp` (5 fns) — the codec R111 left unknown:
  `encode_dstr(s)`: replace `#` → `#22`, then `:` → `#3A`; `decode_dstr(s)`: the inverse order
  (`#3A` → `:` first, then `#22` → `#`). `EncodeDeviceId(encoded, name, path)`: UTF-16 → UTF-8,
  encode both halves, `encoded = name + ":" + path` — the separator is the one literal unencoded
  `:`. `DecodeDeviceDstr(name, path, id)`: false on empty id; split on the **first** `:`
  (name = before, path = after); decode both; true. `DecodeDeviceId(out, id)`: decodes into the
  libdshowcapture `DeviceId`; false on failure (the R111 "DecodeDeviceId failed" abort).
- **S-051-R138 [SRC]** `win-dshow-encoder.cpp` (12 fns): registers the two encoder infos
  `dshow_c985_h264` / `dshow_c353_h264`; each selects the first enumerated DirectShow encoder whose
  friendly name contains `L"C985"` / `L"C353"` respectively. `get_video_info` / update / encode /
  extra-data behaviour: R120–R124.
- **S-051-R139 [SRC]** `dshow-plugin.cpp` (2 fns): module description "Windows DirectShow
  source/encoder", locale en-US. Load: `RegisterDShowSource()` + `RegisterDShowEncoders()`; when
  built with `VIRTUALCAM_AVAILABLE` and `vcam_installed(false)`: register `virtualcam_info`.
  `vcam_installed(b64)`: `StringFromGUID2(CLSID_OBS_VirtualVideo)` →
  `RegOpenKeyExW(HKEY_CLASSES_ROOT, L"CLSID\\<guid>", 0, KEY_READ | (b64 ? KEY_WOW64_64KEY :
  KEY_WOW64_32KEY))` — the caller only ever passes `false`, so only the 32-bit registry view is
  checked (Q4).

### 13.3 Virtual camera (output side + driver side)

- **S-051-R140 [SRC]** `virtualcam.c` (7 fns): the virtual-camera **output** (writer). start:
  `interval = fps_den * 10000000ULL / fps_num` from the current video info; write
  `"%dx%dx%lld"` (width, height, interval) to `<config dir>/obs-virtualcam.txt` via the safe
  temp-write (overwritten on every start); `video_queue_create(width, height, interval)` (R130) —
  failure (a mapping already exists): WARNING `starting virtual-output failed`, start fails; set
  the output's video conversion to NV12 at the output size, `active = true`, `stopping = false`,
  INFO `Virtual output started`, `obs_output_begin_data_capture(output, 0)`. stop/deactivate:
  `obs_output_end_data_capture` + `video_queue_close` (R135). encoded_packet: `video_queue_write`
  (R132) with the frame's planes/linesize/timestamp.
- **S-051-R141 [SRC]** `virtualcam-module/virtualcam-module.cpp` (14 fns): the DirectShow
  **driver** (reader, a separate in-proc COM module): `VCamFactory` is the `IClassFactory` for
  `CLSID_OBS_VirtualVideo` (friendly name "OBS Virtual Camera"); server registration writes the
  CLSID key and maps the filter into the capture category via `IFilterMapper2`.
- **S-051-R142 [SRC]** `virtualcam-module/virtualcam-filter.cpp` (12 fns): the filter opens the
  shared-memory queue (R131), reads its cx/cy/interval and advertises NV12 + I420 + YUY2 media
  types at that size/interval (NV12 is the default). Per output request: convert the queue frame
  to the negotiated format with `nv12_scale_init` (NV12/I420/YUY2 targets); when no fresh frame
  is available (queue not READY, or R134's repeated-index limit) it delivers the default frame —
  `ShowDefaultFrame` memsets the whole output buffer to **127** (neutral grey) — or the
  placeholder image scaled into the same format.
- **S-051-R143 [SRC]** `virtualcam-module/placeholder.cpp` (5 fns): loads
  `placeholder.png` from the module directory (bitmap decoded to RGB), converts RGB → NV12
  (planar, `width*height*3/2` bytes) once, and serves the buffer pointer to R142.
- **S-051-R144 [SRC]** `virtualcam-module/sleepto.c` (3 fns): `gettime_100ns` =
  `QueryPerformanceCounter` scaled to 100-ns units through a double; `sleepto_100ns(target)`:
  already past → false; else `Sleep(ms - 1)` for `ms = (target - t) / 10000` when `ms > 1`, then
  spin `Sleep(0)` until the target is reached → true (a high-resolution sleep without a timer
  object, used to pace the driver's frame delivery).

### 13.4 WASAPI host

- **S-051-R145 [SRC]** `win-wasapi.cpp` (46 fns): the per-source object — COM pointers
  (enumerator/client/capture), the source + a `reroute_target` weak source, `default_id` (wide),
  `device_id`/`device_name` (narrow), the mmdevapi module plus the dynamically resolved RTWQ
  functions (`ActivateAudioInterfaceAsync`, `RtwqUnlockWorkQueue`, `RtwqLockSharedWorkQueue`,
  `RtwqCreateAsyncResult`, `RtwqPutWorkItem`, `RtwqPutWaitingWorkItem`) and `rtwq_supported`
  (RTWorkQ.dll present; RTWQ is Win 8.1+ but "silently fails" on some systems per the comment),
  the window fields (priority/class/title/executable/hwnd/process id), `sourceType`, atomics
  `useDeviceTiming`/`isDefaultDevice`/`sawBadTimestamp`, `hooked`, and the reconnect thread +
  its three events. RTWQ is used to marshal the two ASIO-style callbacks (start-capture,
  sample-ready) onto a shared work queue; the async result objects are created at init. The
  capture thread (`CoInitializeEx(MULTITHREADED)`) drives init (R84–R86) and then
  `ProcessCaptureData`: per packet the format/speakers/rate come from the client format;
  timestamp = `ts * 100` (100-ns device timestamp) for process output, else
  `useDeviceTiming ? ts * 100 : now_ns - frames * 1e9 / rate`; the audio goes to
  `reroute_target` when set (resolved through the weak reference), else the own source; the
  buffer is released afterwards. On init failure the source arms the reconnect thread
  (`reconnectDuration = RECONNECT_INTERVAL = 3000` ms, R88): it waits on (exit, reconnect)
  signals, and on each reconnect signal — after the duration, interrupted by the stop signal —
  calls `Start()` again; the exit signal ends it.
- **S-051-R146 [SRC]** `wasapi-notify.cpp` (8 fns): `WASAPINotify` creates an
  `IMMDeviceEnumerator` and registers one `NotificationClient`
  (`IMMNotificationClient`, refcount from 1): `OnDefaultDeviceChanged(flow, role, id)` forwards to
  the stored callback only when both the callback and `id` are non-null; the other four
  notifications (device added/removed/state-changed, property changed) are no-ops returning
  S_OK. The callback table is registered at construction and unregistered at destruction;
  `AddDefaultDeviceChangedCallback(module, cb)` binds the callback (the module is retained so the
  callback can address the owning module).
- **S-051-R147 [SRC]** `enum-wasapi.cpp` (3 fns): `GetDeviceName(device)` = the
  `PKEY_Device_FriendlyName` property-store value as UTF-8 (empty string when absent or empty);
  `GetWASAPIAudioDevices(input)` enumerates the **active** endpoints of `input ? eCapture :
  eRender` (any HRESULT failure throws an `HRError` with the failing call) and returns one record
  per device (name via `GetDeviceName`, id, ...). This is the list backing R89's `Default` +
  endpoints property lists.
- **S-051-R148 [SRC]** `plugin-main.cpp` (3 fns): module description "Windows WASAPI audio
  input/output sources", locale en-US. Load: compute `process_filter_supported` =
  Win10 build `>= 19041.0` (the comment notes MS documents 20348, "but process filtering seems to
  work earlier"); register input + device output always, process output only when supported;
  create the process-global `WASAPINotify` and add the default-device-changed callback: it reads
  the current monitoring device (`obs_get_audio_monitoring_device(nullptr, &id)`), acts only when
  it is the literal `"default"` (NULL or other → ignore) **and** `flow == eRender`, then queues an
  `OBS_TASK_UI` task running `obs_reset_audio_monitoring()`. Unload: delete the notify object.
  `GetNotify()` exposes the global.

### 13.5 Quirks (added)

- Q4: `vcam_installed` accepts a `b64` flag but the only call site passes `false` — a
  64-bit-only virtual-camera installation is invisible to the dshow plugin on 64-bit Windows.
- Q5: `obs-virtualcam.txt` is rewritten on every virtual-camera start (no locking, atomic
  temp-rename); its only consumer is external tooling — the driver side never reads it.
- Q6: the WASAPI process-output gate (build 19041) is looser than Microsoft's documented build
  (20348): process loopback is registered on builds where the API is officially unsupported.
