# S-086 win-capture graphics-hook FFI contract (hook stays C++, D-08)

| | |
|---|---|
| Status | Draft (round 8 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1 (`plugins/win-capture/graphics-hook/` + injection/offset tooling) |
| Target crate | `crates/capture-hook-ffi` (FFI contract, name/layout tables, injection client). Per **D-08** the hook DLL itself remains the C/C++ `graphics-hook.dll`; the Rust side MUST consume it, not reimplement it. |
| Depends on | S-051 §4 (hook protocol structures §4.1, kernel object names §4.2, shmem layout §4.3), S-074 R09 (hook DLL copy + vulkan registry post-update) |
| Conformance | planned, none yet |

## 1. Scope

In scope: the hook DLL's observable contract — DLL lifecycle, kernel-object IPC
(events/mutexes/pipe/shmem), the capture primitives (shmem copy thread, frame pacing,
stop/keepalive), per-API hooking behaviour (Vulkan, D3D12, D3D9, DXGI→D3D11/10/12, GL,
D3D8) including format tables, and the injection/offset tooling the host uses. The
**host side** (who creates the named objects, reads the frames, `game_capture`/
`window_capture` sources) is S-051 §5; this spec pins what the hook side does so the
contract can't drift. Out of scope: ddraw (dead code, commented out), the plugin's
COM/property layer (S-051).

### Reference files (20, full paths relative to `obs-studio/`)

- `plugins/win-capture/graphics-hook/graphics-hook.c` — DLL entry, IPC init, capture loop, shmem copy thread
- `plugins/win-capture/graphics-hook/graphics-hook.h` — shared in-DLL contract, `frame_ready`/`capture_should_stop`, `capture_init_*`
- `plugins/win-capture/graphics-hook/vulkan-capture.c` — Vulkan implicit layer
- `plugins/win-capture/graphics-hook/vulkan-capture.h` — intercepted entrypoints, `vk_format_to_dxgi` table
- `plugins/win-capture/graphics-hook/d3d12-capture.cpp` — D3D12 `ExecuteCommandLists` hook
- `plugins/win-capture/graphics-hook/d3d9-capture.cpp` — D3D9 present hooks + capture
- `plugins/win-capture/graphics-hook/d3d9-patches.hpp` — SEH-safe vtable patch helpers
- `plugins/win-capture/graphics-hook/dxgi-capture.cpp` — DXGI Present/Resize hooks, device dispatch
- `plugins/win-capture/graphics-hook/dxgi-helpers.hpp` — `apply_dxgi_format_typeless`, swapchain debug
- `plugins/win-capture/graphics-hook/d3d11-capture.cpp` — D3D11 back-buffer copy
- `plugins/win-capture/graphics-hook/d3d10-capture.cpp` — D3D10 back-buffer copy
- `plugins/win-capture/graphics-hook/d3d8-capture.cpp` — D3D8 present/reset hooks + LockRect capture
- `plugins/win-capture/graphics-hook/gl-capture.c` — OpenGL SwapBuffers hooks + window discovery
- `plugins/win-capture/load-graphics-offsets.c` — offset cache load/validate (versioned)
- `plugins/win-capture/inject-helper/inject-helper.c` — obfuscated remote injection
- `plugins/win-capture/get-graphics-offsets/get-graphics-offsets.c` — offset generator (main)
- `plugins/win-capture/get-graphics-offsets/get-graphics-offsets.h` — `graphics_offsets` struct
- `plugins/win-capture/get-graphics-offsets/d3d9-offsets.cpp` — D3D9 vtable offset probe
- `plugins/win-capture/get-graphics-offsets/d3d8-offsets.cpp` — D3D8 offset probe (32-bit sysdll)
- `plugins/win-capture/get-graphics-offsets/dxgi-offsets.cpp` — DXGI offset probe
- (related, not counted: `shared/obs-hook-config/graphics-hook-info.h` — `struct hook_info` + name constants; `graphics-hook/obs-vulkan{32,64}.json` — layer manifests)

## 2. Conventions

RFC 2119 words. All kernel object names are `"<base><pid>"` (S-051 §4.2) unless noted.
"Offsets" = the `graphics_offsets` INI values (module-relative present-function pointers)
cached by the plugin; zero = "not hookable this way, fall back to manual probing".
Detour = Microsoft Detours attach on an out-of-line function pointer.

## 3. Behaviour

### 3.1 DLL lifecycle (`graphics-hook.c`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-086-R01 | `graphics-hook.c` | `DllMain(ATTACH)` | Order: (1) dup-guard — open `graphics_hook_dup_mutex<pid>`; if it exists → `DbgOut "[OBS] Duplicate hook library"` and **fail DllMain**; else create it. (2) duplicate own thread handle (synchronize). (3) `init_signals` — open 5 events `CaptureHook_Restart/Stop/HookReady/Exit/Initialize<pid>` (any missing → fail DllMain). (4) system path. (5) `init_hook_info` — open/create + map the `CaptureHook_HookInfo<pid>` file mapping (`struct hook_info`, S-051 §4.1). (6) `init_mutexes` — `CaptureHook_TextureMutex1/2<pid>`. (7) **self-`LoadLibraryW` of its own full path** — deliberately holds a second refcount so a later `FreeLibrary` (by the injector) cannot unload it. (8) spawn `main_capture_thread`. | handles, refcount |
| S-086-R02 | `graphics-hook.c` | `DllMain(DETACH)` | If the dup mutex is absent (failed attach) → do nothing. Else: `stop_loop = true`, wait **300 ms** for the capture thread, `free_hook` (unmap hook_info, close events/mutexes, free pipe). | — |
| S-086-R03 | `graphics-hook.c` | `main_capture_thread` | `init_hook`: wait for DllMain to finish (100 ms on the duplicated thread handle); compute keepalive name `CaptureHook_KeepAlive<pid>`; spawn the **dummy window thread**: class `temp_d3d_window_4039785` (`CS_OWNDC`, 1×1 popup, own message loop) — the device host for manual D3D9 probing; log `graphics-hook.dll loaded against process: <exe>` (+ the `(half life scientist) everything..  seems to be in order` line); `SetEvent(CaptureHook_Restart)` (tells the host: the hook is attached and can be restarted into). | threads, events |
| S-086-R04 | `graphics-hook.c` | `capture_loop` | `WaitForSingleObject(signal_init, INFINITE)` — the host arms the hook via `CaptureHook_Initialize<pid>`. Then `while (!attempt_hook()) Sleep(40)`. Once hooked: loop `Sleep(40)`; every **100th** iteration (~4 s) re-run `attempt_hook()` (a second API may appear later, e.g. D3D11 after the window was created). | — |
| S-086-R05 | `graphics-hook.c` | `attempt_hook` | Fixed order, first success wins: **vulkan → d3d12 → d3d9 → dxgi → gl → d3d8** (ddraw is commented-out dead code — do not resurrect). Each API: if its hookable-check (non-zero offsets, R17–R24) is false → mark "hooked" (never retried) and continue; else attempt; success → return true. Vulkan/D3D12 attempt unconditionally (they self-detect). | per-API flags |
| S-086-R06 | `graphics-hook.c` | logging | `hlog` → single-line (1024-byte) message written to the IPC pipe `CaptureHook_Pipe<pid>` **and** `OutputDebugStringA("[OBS] " + msg)`; a failed pipe write frees the pipe client (logs continue to the debugger only). `hlog_hr` appends `FormatMessageA` text: `"<text> (0x%08lX): <system msg>"`. | pipe |

### 3.2 Capture primitives (`graphics-hook.c`, `graphics-hook.h`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-086-R07 | `graphics-hook.c` | `capture_init_shtex(data, window, cx, cy, format, flip, handle)` | Create named shmem `CaptureHook_Texture<topmost-window-addr>_<counter++>` (counter is process-global); map as `shtex_data{tex_handle}`; fill `hook_info`: HOOK_VER major/minor, `window`, `type = CAPTURE_TYPE_TEXTURE`, `format`, `flip`, `map_id`, `map_size = sizeof(shtex_data)`, `cx/cy` (+`UNUSED_base_cx/cy` = same); `SetEvent(CaptureHook_HookReady)`; `active = true`. The host reads `tex_handle` (a D3D11 texture) and imports it. | hook_info, shmem |
| S-086-R08 | `graphics-hook.c` | `capture_init_shmem(data, window, cx, cy, pitch, format, flip)` | Size = `ALIGN(sizeof(shmem_data),32) + 2*ALIGN(cy*pitch,32) + 32`. Texture offsets: header end rounded up to a **32-byte (256-bit) alignment** relative to the mapping base (if that lands below the header, add 32); `shmem_data{last_tex = -1, tex1_offset, tex2_offset = tex1 + aligned_tex}`. `hook_info`: `type = CAPTURE_TYPE_MEMORY`, `pitch`, `cx/cy`, `map_size = total`. Spawn the copy thread (R09), `SetEvent(HookReady)`, `active = true`. | hook_info, shmem |
| S-086-R09 | `graphics-hook.c` | `copy_thread` | `WaitForMultipleObjects({copy_event, stop_event})` — anything but the copy event exits. Under `data_mutex`: take `cur_tex`/`cur_data`. Per-buffer critical section, then `try_lock_shmem_tex(id)`: non-blocking try of the *other* texture mutex first is **not** the rule — it tries `id`, then the other, accepting `WAIT_ABANDONED`; both busy → drop the frame. Copy = `memcpy(dst, cur_data, pitch*cy)`; write `shmem_data.last_tex = lock_id`; next attempt starts from the opposite buffer (0↔1 ping-pong). | shmem textures |
| S-086-R10 | `graphics-hook.c` | capture-side API | `shmem_copy_data(idx, data)`: publish `cur_tex/cur_data` + `locked_textures[idx]=true` under `data_mutex`, `SetEvent(copy_event)`. `shmem_texture_data_lock(idx)`: if `locked_textures[idx]` → enter the buffer CS and return true (caller may read the source now); else false (frame not fresh). `shmem_texture_data_unlock(idx)` clears the flag + leaves the CS. `NUM_BUFFERS = 3` source buffers (triple buffering). | buffer locks |
| S-086-R11 | `graphics-hook.h` | per-frame gate | `frame_ready(interval)`: `interval == 0` → always true; `elapsed < interval` → false; else `last_time = (elapsed > 2*interval) ? now : last_time + interval` — **drift correction**: after a long stall the clock resets, otherwise the next deadline is the previous deadline + interval (no accumulated phase error). `capture_ready() = active && frame_ready(hook_info->frame_interval)`; `capture_stopped()/capture_restarted()` = non-blocking `WaitForSingleObject(0)` on the stop/restart events. | — |
| S-086-R12 | `graphics-hook.h` / `graphics-hook.c` | stop detection | `capture_should_stop()`: only when active — stop event set → true; otherwise the keepalive mutex `CaptureHook_KeepAlive<pid>` is **re-checked at most every 5 s** (`OpenMutexW`; host died → true). `capture_free()`: set stop event, wait **500 ms** for the copy thread (`TerminateThread` on timeout), close events/CS, unmap shmem, `SetEvent(Restart)`, `active = false`. | active |

### 3.3 Vulkan (`vulkan-capture.c/.h`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-086-R13 | `vulkan-capture.c` + related JSON | layer registration | Implicit **GLOBAL** layer, `file_format_version 1.1.2`: name `VK_LAYER_OBS_HOOK`, `library_path .\graphics-hook{32,64}.dll`, `api_version 1.3.216`, `implementation_version 1`, description "Open Broadcaster Software hook", `functions.vkNegotiateLoaderLayerInterfaceVersion = "OBS_Negotiate"`, `disable_environment {DISABLE_VULKAN_OBS_CAPTURE: "1"}`. Installed in `HKCU`/`HKLM VulkanImplicitLayers` by install/update (S-074 R09); the hook's `hook_vulkan()` is still attempted per R05. | registry |
| S-086-R14 | `vulkan-capture.c` | instance/device creation | Intercepts `vkCreateInstance`/`vkCreateDevice`; loader negotiate detected via `sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO / _DEVICE_CREATE_INFO && function == VK_LAYER_LINK_INFO` → returns a dispatch table that chains the layer and hooks: `vkCreateInstance`, `vkCreateDevice`, `vkQueuePresentKHR`, swapchain create/destroy, `vkGetSwapchainImagesKHR`, image/`ImageView`/`Framebuffer` create, `vkCreateWin32SurfaceKHR`, memory alloc/bind. Per queue family: records `supports_transfer` from the queue flags. | layer state |
| S-086-R15 | `vulkan-capture.c` | `OBS_QueuePresentKHR` | Captures only when the swapchain's recorded data is `valid` **and** the presenting queue `supports_transfer` (presenting on a graphics-only queue is skipped). Re-entrancy: TLS `vk_presenting` counter — non-zero at entry logs `flog("non-zero vk_presenting: %d")`; increment before / decrement after the present work. On present: if `capture_ready()` (R11) → capture the back image (R16) + `capture_should_stop()` (R12) check. | frame |
| S-086-R16 | `vulkan-capture.h` | format mapping | `vk_format_to_dxgi(VkFormat)` — generated static table (the file is ~800 lines mostly table) mapping every capturable VkFormat to its DXGI_FORMAT equivalent; unmapped → error. The swapchain image is re-interpreted as a D3D11 texture via `GetSwapchainImagesKHR`/`vkGetDeviceMemory` chaining; texture creation uses `apply_dxgi_format_typeless` (R22). | — |

### 3.4 Direct3D paths (`d3d12`, `d3d9`, `dxgi`, `d3d11/10`, `d3d8`, patches, helpers)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-086-R17 | `d3d12-capture.cpp` | `hook_d3d12` | Detour `ID3D12CommandQueue::ExecuteCommandLists` — offset from `graphics_offsets.d3d12.execute_command_lists`, else **manual fallback**: resolve the queue vtable and take `vtable[10]`. On execute: if `capture_ready()` → set up/capture from the command queue's device; `capture_should_stop()` → `capture_free`. | detour |
| S-086-R18 | `d3d9-capture.cpp` | `hook_d3d9` | Hookable check = all of `d3d9.present/present_ex/present_swap` offsets non-zero. Detours: `Present`, `PresentEx` (plus `Present` on the swap chain via `present_swap` offset). **Manual path** (offsets absent): `hlog "D3D9 values invalid, manually obtaining"`; `Direct3DCreate9Ex(D3D_SDK_VERSION)` + `CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, dummy_window, HW vertex processing | NOWINDOWCHANGES, {Windowed=1, Flip, A8R8G8B8, 1 back buffer, IMMEDIATE})` → `present = vtable[17]`, `present_ex = vtable[121]`, `present_swap = swap_vtable[3]`. | detours |
| S-086-R19 | `d3d9-capture.cpp` | present hit | Windowed: `GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO)`; fullscreen: `GetRenderTarget(0)`. shtex mode: `StretchRect(backbuffer → copy texture, D3DTEXF_NONE)` (failure → `hlog_hr "d3d9_shtex_capture: StretchRect failed"`). shmem mode: `GetRenderTargetData` → copy → source `LockRect` read (D3DLOCK_READONLY). Format table maps D3DFMT (e.g. `D3DFMT_A8R8G8B8` → `DXGI_FORMAT_B8G8R8A8_UNORM`), `flip` from windowed state. | frame |
| S-086-R20 | `dxgi-capture.cpp` | `hook_dxgi` | Hookable check = `dxgi.present && dxgi.resize` offsets non-zero. Detours `IDXGISwapChain::Present`, `Present1`, `ResizeBuffers`. On present: `GetDevice` probe in fixed order **`ID3D11Device` → `ID3D10Device` → `ID3D12Device`** → hand the device to the matching per-API capture setup (`d3d11_capture` / `d3d10_capture` / `d3d12_capture`). `ResizeBuffers` → re-init capture with the new size/format (swapchain desc logged via `print_swap_desc`). | detours, device |
| S-086-R21 | `d3d11-capture.cpp` / `d3d10-capture.cpp` | present hit | Copy back buffer 0 → intermediate texture/surface (D3D11: `CopyResource` into a copy resource + map; D3D10: same via its device context), publish via `shmem_copy_data` (memory) or hand the copy texture to the host (shtex, when `nv_capture`-style texture capture is used and not `force_shmem`). | frame |
| S-086-R22 | `dxgi-helpers.hpp` | texture creation | `apply_dxgi_format_typeless(format, allow_srgb_alias)`: only when `allow_srgb_alias` (set by the host in `hook_info`): `B8G8R8A8_UNORM → B8G8R8A8_TYPELESS`, `R8G8B8A8_UNORM → R8G8B8A8_TYPELESS`, else unchanged. Used by all D3D texture imports so sRGB-backed sources stay addressable. | — |
| S-086-R23 | `d3d8-capture.cpp` | `hook_d3d8` | Hookable = `d3d8.present` offset non-zero. Detours `IPresentation::Present` (offset or manual) and device `Reset` (re-init on mode change). Capture: `LockRect` (D3DLOCK_READONLY) of the front surface / render target into the shmem path. | detour |
| S-086-R24 | `d3d9-patches.hpp` | vtable patching | `safe_memcmp` (SEH-guarded — a torn/invalid read returns -1 instead of crashing the host process) + `patch_info{size, data}` used by the D3D9 path to patch/verify vtable slots in place where detours can't reach. | — |

### 3.5 OpenGL (`gl-capture.c`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-086-R25 | `gl-capture.c` | `hook_gl` | Load `opengl32.dll` from the system path. **Blacklist**: process name `yo_cm_client.exe` or `cm_client.exe` ("Life is Feudal") → `hlog "Ignoring opengl for game: %s"` + return *true* (counts as hooked, nothing captured — those games mix GL+D3D and GL capture breaks them). Window discovery: a `WH_GETMESSAGE`-style message hook on the dummy window's message loop (marker `WM_USER + 432`); the hook uninstalls itself after the first match (via obfuscated `UnhookWindowsHookEx` lookup). | message hook |
| S-086-R26 | `gl-capture.c` | detours | `SwapBuffers` always; plus `wglSwapBuffers` and `wglSwapLayerBuffers` when resolvable, `wglDeleteContext` tracked for cleanup. On first swap: `gl_init(hdc)` — window from DC; client rect gives `cx/cy` (**except** the `darkest_dungeon_fix`: hardcoded 1920×1080 for that title); `format = DXGI_FORMAT_B8G8R8A8_UNORM`, `flip = true`; shtex path when the NVIDIA texture-capture route is available and `!force_shmem` and no earlier shmem fallback, else shmem; init failure codes: `INIT_FAILED` / `INIT_SHTEX_FAILED`. | detours, capture |

### 3.6 Injection + offset tooling (`inject-helper.c`, `load-graphics-offsets.c`, `get-graphics-offsets*`)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-086-R27 | `inject-helper.c` | inject | Enable `SE_DEBUG` first. All sensitive APIs are resolved through the obfuscated lookup (`ms_get_obfuscated_func`): `OpenProcess` under name `"HxjcQrmkb|~"` (key `0xc82efdf78201df87`), and the injection primitives similarly obfuscated. Three modes: `inject_library` (plain `CreateRemoteThread` + `LoadLibraryW`), `inject_library_safe` (suspends the *target's* threads then injects), `inject_library_full` (suspends **all** process threads, then the safe path). The caller passes `use_safe_inject`; failure of the safe mode inside `full` falls back to the plain remote thread. | target process |
| S-086-R28 | `load-graphics-offsets.c` | plugin load | Load the offsets INI + a `version.ini` cache (module config path) holding the Windows module versions (major/minor/build/revis) the offsets were generated for — for 32-bit system DLLs (e.g. d3d8 on a 64-bit OS) the version is read from the **WoW64** copy via `GetSystemWow64DirectoryW`. `cached_versions_match`: any section's stored version ≠ live module version → cache invalid → the host must re-run `get-graphics-offsets` (or fall back to manual probing, R18). Sections: `[d3d8] present`; `[d3d9] present, present_ex, present_swap, d3d9_clsoff, is_d3d9ex_clsoff`; `[dxgi] present, present1, resize, release`. | offset cache |
| S-086-R29 | `get-graphics-offsets.c` | generation | Standalone tool: hardens its own loading first (`SEM_FAILCRITICALERRORS`, `BASE_SEARCH_PATH_ENABLE_SAFE_SEARCHMODE \| BASE_SEARCH_PATH_PERMANENT`, `SetDefaultDllDirectories(LOAD_LIBRARY_SEARCH_DEFAULT_DIRS)`, empty `DllDirectory`), registers the dummy window class, then creates **real** devices to read vtables (d3d9 via `Direct3DCreate9Ex` HAL as in R18; d3d8 from the 32-bit system DLL; dxgi via a swap chain). Offsets are computed as `vtable[n] − module_base` and printed as INI to stdout (the exact key set of R28). The plugin stores the output for later `load-graphics-offsets` runs. | INI output |

### 3.7 The FFI surface (summary for implementers)

| # | component/file | trigger | behaviour | state changed |
|---|---|---|---|---|
| S-086-R30 | contract | host ↔ hook | Everything the Rust host may rely on is: (1) the kernel object names, all pure functions of `pid` (S-051 §4.2); (2) the 648-byte packed `struct hook_info` (S-051 §4.1, `static_assert`-sized) — fields written only by the hook (R07/R08), read by the host; (3) the shmem layouts (S-051 §4.3); (4) the log pipe `CaptureHook_Pipe<pid>`; (5) the event handshake: host creates everything → hook opens on attach (R01) → hook signals `Restart` when attached (R03) → host sets `Initialize` (R04) → hook signals `HookReady` when a capture is configured (R07/R08) → host signals `Stop`/`Restart`/`Exit` (R12). **D-08**: the Rust rewrite MUST keep `graphics-hook.dll` byte-compatible on this surface — name, size, field order, alignment and the `CaptureHook_Texture<addr>_<n>` shmem naming (the host resolves the mapping by name pattern). | — |

## 4. Reference defects and quirks

- **Q-1** (`graphics-hook.c` R01): DllMain **fails** on a duplicate hook — the second
  `LoadLibrary` returns a dead module; the injector treats "already injected" as success.
- **Q-2** (`graphics-hook.c` R09): a frame is silently dropped when both shmem texture
  mutexes are held (host not consuming fast enough) — there is no backpressure signal.
- **Q-3** (`graphics-hook.c` R02): detach only waits 300 ms and `thread_data_free` only
  500 ms, then `TerminateThread` — a stuck copy thread is killed mid-memcpy in the host
  process (accepted risk in the reference).
- **Q-4** (`gl-capture.c`): the Life-is-Feudal blacklist makes `hook_gl` return *hooked
  but never capturing* — the capture loop stops probing other APIs once GL "succeeds".
- **Q-5** (`load-graphics-offsets.c`): offset validity is version-gated per Windows build;
  a Windows feature update silently changes d3d8/9/dxgi module versions and forces the
  re-generation path — the cache, not the code, decides capture works.

## 5. Concurrency (summary)

- One capture thread (probing), one copy thread (shmem), N render/present threads calling
  the detours; shared state guarded by the two shmem texture mutexes (cross-process),
  per-buffer critical sections (in-process), `data_mutex` (copy queue), and TLS
  `vk_presenting` (Vulkan re-entrancy).
- `hook_info` is written by the hook on (re)init and read by the host without a lock —
  the `HookReady` event is the release barrier.

## 6. Open questions

1. Confirm the rewrite's `crates/capture-hook-ffi` ships the **same** DLL (built from the
   33.0.1 sources, unmodified) vs. a re-hosted build — D-08 says "hook stays C++"; if the
   build system changes (MSVC version, Detours version), R18/R23/R27 behaviour must be
   re-verified, since offsets and obfuscated lookups are toolchain-sensitive.
2. `get-graphics-offsets` runs *inside* a fresh process that loads real d3d9/dxgi — on
   modern Windows (no d3d8) confirm which sections legitimately stay zero and that the
   version cache treats missing modules as "always valid".
3. Vulkan layer `api_version 1.3.216` is pinned in the JSON (R13); confirm the rewrite
   keeps the 1.3 loader interface (`OBS_Negotiate`) or may move to the classic
   `vkNegotiateLoaderLayerInterfaceVersion`-less path — plugin users on older drivers
   branch on this.
