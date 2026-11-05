# S-078 D3D11 graphics backend (`libobs-d3d11`)

| | |
|---|---|
| Status | Draft (round 8 — behavioural catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1, `libobs-d3d11/` |
| Target crate | `crates/obs-d3d11` (the `gs` backend for Windows) |
| Depends on | S-043 (graphics API — `gs_*` surface, color formats), S-020 (effect language) |
| Conformance | planned `test/conformance/scripts/d3d11/` — none yet (Windows-only; vectors via a headless D3D11 device) |

## 1. Scope

The D3D11 implementation of the `gs_*` device API: device/factory/adapter bring-up,
swap chains (legacy and flip-model), texture/buffer/sampler/zstencil/stage-surface
resources, shader compilation from the S-020 effect language, device-loss rebuild, and
the DXGI output-duplication path used by monitor capture.
Out of scope: `gs_*` semantics (S-043), effect-language parsing (S-020), the Windows
capture plugins that consume duplicator textures (S-051 / S-086).

### Reference files (all cited)

- `libobs-d3d11/d3d11-subsystem.cpp`, `libobs-d3d11/d3d11-subsystem.hpp`
- `libobs-d3d11/d3d11-rebuild.cpp`
- `libobs-d3d11/d3d11-shader.cpp`, `libobs-d3d11/d3d11-shaderprocessor.cpp`
- `libobs-d3d11/d3d11-duplicator.cpp`
- `libobs-d3d11/d3d11-texture2d.cpp`, `libobs-d3d11/d3d11-texture3d.cpp`
- `libobs-d3d11/d3d11-vertexbuffer.cpp`, `libobs-d3d11/d3d11-indexbuffer.cpp`, `libobs-d3d11/d3d11-samplerstate.cpp`, `libobs-d3d11/d3d11-zstencilbuffer.cpp`, `libobs-d3d11/d3d11-stagesurf.cpp`

## 2. Conventions

RFC 2119. COM objects are held in `ComPtr` (refcounted); a failed `HRESULT` (`!= S_OK` /
`S_FALSE` where noted) aborts the current operation with a log line — the conforming
crate MUST keep the same success/failure split even if it does not use COM.
W = 64-bit (Windows). The backend exports the full S-043 `gs_*` set; this spec pins the
D3D11-specific behaviour of each.

## 3. Device bring-up (`d3d11-subsystem.cpp`)

- **S-078-R01** `InitFactory`: create the DXGI factory (factory1/2 as available) and
  query the monitor color API; failure aborts device creation.
- **S-078-R02** `InitAdapter(adapterIdx)`: `factory->EnumAdapters1(adapterIdx, &adapter)`;
  any failure (including an out-of-range index) → `throw UnsupportedHWError("Failed to enumerate DXGIAdapter", hr)` — no default-adapter fallback. The adapter description is queried locally where needed, not cached.
- **S-078-R03** `InitDevice`: `D3D11CreateDevice(adapter, D3D_DRIVER_TYPE_UNKNOWN,
  nullptr, createFlags, featureLevels, D3D11_SDK_VERSION, …)` with
  `featureLevels = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1,
  D3D_FEATURE_LEVEL_10_0 }`; the device logs `D3D11 loaded successfully, feature level
  used: %x`. `createFlags` is always `D3D11_CREATE_DEVICE_BGRA_SUPPORT` — the
  `D3D11_CREATE_DEVICE_DEBUG` line is present but **commented out** (no debug layer).
  After creation: `increase_maximum_frame_latency` (comment: "prevent stalls sometimes seen in
  Present calls"), HAGS status log, GPU thread-priority tweak on non-Intel.
- **S-078-R04** `HasBadNV12Output()`: hardware-quirk **probe** — renders a 128×128 NV12 Y/UV
  test pattern with inline shaders, stages the Y surface and returns true when the pixel read
  past the Y plane is zero; on detection logs "Bad NV12 texture handling detected! Disabling
  NV12 texture support." `nv12Supported = CheckFormat(NV12) && !HasBadNV12Output()`; WARP
  (VendorId `0x1414`/DeviceId `0x8c`) skips NV12 entirely in `InitDevice` (QUIRK Q1).
- **S-078-R05** `GetMonitorColorInfo(hMonitor)`: matches the monitor against `IDXGIOutput6::GetDesc1` across all adapters/outputs; `hdr = (ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020)` (HDR10 only — no HLG classification), plus `BitsPerColor` and `GetSdrMaxNits`; result cached in `monitor_to_hdr` (cleared when the factory is re-initialised); fallback `{false, 8, 80}`. Feeds the `gs_get_monitor_info` API (S-043).

## 4. Swap chain (`gs_swap_chain`, `d3d11-subsystem.cpp`)

- **S-078-R06** Effect selection: legacy `DXGI_SWAP_EFFECT_DISCARD` by default;
  `DXGI_SWAP_EFFECT_FLIP_DISCARD` when the factory implements `IDXGIFactory5` (adds
  `DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT` and clamps `num_backbuffers` to ≥2).
  The selected effect drives the colour-space choice (R08).
- **S-078-R07** `BufferCount = init.num_backbuffers`; swap chain format follows the
  requested `gs_color_format` (mapped per S-043).
- **S-078-R08** `get_next_space(device, hwnd, effect)`: with `FLIP_DISCARD` the swap
  chain advertises the extended (full-range / HDR-capable) colour space when the output
  supports it; legacy DISCARD uses the basic space.
- **S-078-R09** `ResizeBuffers(BufferCount, cx, cy, format, flags)` on resize; target
  texture and z/stencil are re-created after (`InitTarget`, `InitZStencilBuffer`).
- **S-078-R10** `device_present`: `swap->Present(interval, 0)` on the current swap
  chain (`interval = 1` when the waitable object is set); `DXGI_ERROR_DEVICE_REMOVED`/`DEVICE_RESET` → `RebuildDevice()`. Without an active swap, `device_is_present_ready` is false and both `device_present` and `device_is_present_ready` log their own `…(D3D11): No active swap` warning. (The only Present-stall comment in the reference belongs to `increase_maximum_frame_latency`, R03.)
- **S-078-R11** Multiple swap chains coexist (`curSwapChain`); per-swap-chain
  z/stencil, target and colour state are updated on switch (`UpdateZStencilState`,
  `UpdateRasterState`, `UpdateBlendState`, `UpdateViewProjMatrix`,
  `FlushOutputViews`).

## 5. Resources

- **S-078-R12** Format mapping: obs `gs_color_format` → D3D11 format per the S-043
  colour table; texture creation additionally requires the D3D11 format to report
  `D3D11_FORMAT_SUPPORT_TEXTURE2D | D3D11_FORMAT_SUPPORT_RENDER_TARGET` (missing
  support → creation fails). Q2: exact per-row table (S-043 pins the obs side).
- **S-078-R13** `d3d11-texture2d.cpp` / `d3d11-texture3d.cpp` / cube + volume paths
  (`device_texture_create`, `device_cubetexture_create`, `device_voltexture_create`):
  usage/mipcount semantics, `gs_texture_resize`, data update (subresource copy),
  map/unmap. Texture3D additionally exposes 3-D sampling (S-043).
- **S-078-R14** `d3d11-zstencilbuffer.cpp` (`device_zstencil_create`): depth-stencil
  view + resource creation for the given format (S-043 zstencil formats).
- **S-078-R15** `d3d11-stagesurf.cpp` (`device_stagesurface_create`): shared
  stage-surface texture used by the capture/duplicator interop.
- **S-078-R16** `d3d11-vertexbuffer.cpp` / `d3d11-indexbuffer.cpp`: vertex/index
  buffer creation + dynamic data update semantics (S-043 buffer API).
- **S-078-R17** `d3d11-samplerstate.cpp`: sampler creation from S-043 filter/addr
  parameters.
- **S-078-R18** `gs_obj` (all resources): holds `device` + `gs_type`; the destructor
  releases the COM object; resource identity/validity checks (`gs_obj_valid`) follow
  S-043.

## 6. Shaders (`d3d11-shader.cpp`, `d3d11-shaderprocessor.cpp`)

- **S-078-R19** Pipeline creation (`gs_shader_create` / pipeline variants): the effect
  source goes through `ShaderProcessor`: `Process(shaderString, file)` (S-020 tokens
  via the cf preprocessor) → `BuildString` (processed HLSL) → `D3DCompile(src, file,
  "main", target)` → device shaders. `BuildParams` produces the parameter table
  (name → S-020 param type), `BuildInputLayout` the input layout, `BuildSamplers` the
  sampler array.
- **S-078-R20** Failure paths: missing `main`, compile error or unsupported target →
  log + null shader (S-043 error model). Compute pipelines are created for CS effects.
- **S-078-R21** `d3d11-shaderprocessor.cpp` details: comment stripping, parameter
  extraction order, uniform/texture/sampler binding slots per S-020 §binding rules.

## 7. Device-loss rebuild (`d3d11-rebuild.cpp`)

- **S-078-R22** A rebuild context is created with the device; when the device is lost
  (GPU reset / `DXGI_ERROR_DEVICE_REMOVED` path), the rebuild path re-runs
  `D3D11CreateDevice` (same adapter/feature-level rules as R03), re-creates shared
  textures (failure logs `Failed to rebuild shared texture: 0x%08lX`, runs `RebuildSharedTextureFallback()` and continues),
  then notifies the observers via the `device_loss_rebuild(device, data)` callbacks; a
  failure of the overall rebuild `bcrash`es ("Failed to recreate D3D11").
- **S-078-R23** Rebuild is idempotent and serialised: a second loss during rebuild is
  queued, not re-entrant (S-043 rebuild contract).

## 8. Duplicator (`d3d11-duplicator.cpp`)

- **S-078-R24** `gs_duplicator_create`: `IDXGIOutput::DuplicationOutput` (or factory2
  equivalent) on the given DXGI output; `gs_duplicator_get_texture`:
  `AcquireNextFrame` (timeout per S-043) → shared texture → obs texture handle;
  `gs_duplicator_release_texture`: release the frame back to the duplicator.
- **S-078-R25** Access lost (`DXGI_ERROR_ACCESS_LOST`) → recreates the duplication;
  no new frame within timeout → null texture (S-043 semantics).

## 9. Concurrency

The `gs_device` serialises state updates (view/raster/blend/projection) under its
internal lock; presents happen on the render thread; rebuild runs on the thread that
observed the loss and takes the device lock (Q3).

## 10. Quirks

- Q1: `HasBadNV12Output` — some drivers mishandle NV12 output; detected at runtime by the
  render probe (R04), not by a device blocklist (only WARP is hard-skipped).
- Q2: the full obs↔D3D11 format table is the S-043 table plus the R12 support check;
  rows not re-derived in this round are listed in §11.
- Q3: rebuild lock ordering vs an in-flight present (source: rebuild takes the lock
  after the present slot is released).

## 11. Open questions

1. Per-row D3D11 format table for exotic formats (BGR444, RGB9E5, compressed) — cross
   check against S-043 §colour formats when implementing.
2. Adapter enumeration behaviour when `adapterIdx` exceeds the count — resolved by the
   Round 10 check: `EnumAdapters1` failure throws `UnsupportedHWError("Failed to enumerate
   DXGIAdapter", hr)`; there is no default-adapter fallback.
3. HDR monitor-info API differences between DXGI factory1 and factory2 (R05).
4. `gs_init_data` flag effects on `createFlags` beyond debug (safe mode, no-RT
   flags).
5. Round 10 second-author check (2026-11-05): corrected R02 (was "out of range → default
   adapter" + "desc1 cached" — it throws on any `EnumAdapters1` failure and the desc is not
   cached), R03 (was "debug layer when OBS runs in a debug/safe-mode context" — the debug flag
   is commented out), R04 (was "specific GPU blocks" — it is a runtime NV12 render probe), R05
   (was "output technology + luminance signature → SDR/HDR10/HLG" — it is an HDR10 ColorSpace
   bool + `BitsPerColor` + `GetSdrMaxNits`, no HLG), R06 (was "swap chain 3 capability" — the
   gate is `IDXGIFactory5`), R10 (no flip-mode Present-stall comment exists) and Q1 (no device
   blocklist).
