# Phase 3 guide: graphics

Read [00-developer-primer.md](00-developer-primer.md) first. Tasks: [../phase-3-graphics.md](../phase-3-graphics.md); subtasks: [../subtasks/phase-3-subtasks.md](../subtasks/phase-3-subtasks.md).

## The key architectural fact

Graphics is split in **three layers**, and each can be replaced independently:

```
plugins / libobs render code
        │  gs_* API  (libobs/graphics/graphics.h, ~hundreds of EXPORTs)
        ▼
graphics subsystem  (libobs/graphics/graphics.c, 3312 lines)
  matrix stack, immediate-mode vertex buffer, effect list, viewport stack, blend stack
        │  struct gs_exports  (graphics-internal.h:30, ~176 function pointers: device_*)
        ▼
backend module, loaded with os_dlopen:  libobs-opengl | libobs-d3d11 | libobs-metal
```

- `gs_create(&graphics, module_name, adapter)` (`graphics.c:198`) does `os_dlopen(module)`, `load_graphics_imports()` (`graphics-imports.c`) to resolve all `device_*` symbols into `graphics->exports`, then calls `exports.device_create`.
- The **effect parser is above the backends.** `gs_effect_create()` → `effect_parser` (`effect-parser.c`, 1972 lines) → for each pass it extracts the shader text and calls `gs_vertexshader_create(shader_str, location, &errors)` / `gs_pixelshader_create(...)` (`effect-parser.c:1851,1858`) → backend `device_vertexshader_create(device, const char *shader, const char *file, char **error_string)`. **The backend receives HLSL-like text**; each backend converts it itself (`libobs-opengl/gl-shaderparser.c`, 773 lines, emits GLSL; `libobs-d3d11/d3d11-shaderprocessor.cpp` prepares HLSL for the D3D compiler).

### What this means for the plan

1. A **`wgpu` backend can be built as a new loadable module** (`libobs-wgpu`) that exports the same `device_*` symbols and plugs into today's unmodified C `graphics.c`. It does not need Phase 4. Do this first (P3-13…P3-23): it proves the rendering path with the C engine above it.
2. The **WGSL transpiler (P3-05) lives inside that backend**: input is the shader text the effect parser passes in. It can reuse the C `shader_parser` through libobs or the Rust port of it (P3-02) — decide in the P3-05.1 spike.
3. The **Rust port of the effect parser and `graphics.c`** (P3-01…P3-12) is optional until you want to delete C; port it afterwards, behind the same `gs_*` and `device_*` boundaries. The golden token/AST dumps (P3-01.3, P3-02.4) are what make it safe.
4. Correction to P3-26 ("Render loop"): `render_video`, `render_main_texture`, `output_frames` live in `obs-video.c` and depend on `obs_core`, scenes and sources. They move in **Phase 4**, not Phase 3; Phase 3 only needs them to exist for tests via the C code.

## Reading list (in order)

1. `libobs/graphics/graphics.h` – the public `gs_*` API; skim the sections: devices/adapters, textures, buffers, shaders, state, drawing, immediate mode, effects.
2. `libobs/graphics/graphics-internal.h` – `gs_exports` (what a backend must implement) and `graphics_subsystem` (what the subsystem keeps: `matrix_stack`, `viewport_stack`, immediate-mode `verts/norms/colors/texverts[16]`, `first_effect` list, `blend_state_stack`, `linear_srgb`).
3. `libobs/graphics/effect-parser.c` + `shader-parser.c` – grammar. Walk through `libobs/data/default.effect` and `format_conversion.effect` with the parser in a debugger.
4. `libobs-opengl/gl-subsystem.c` and `gl-shaderparser.c` – the reference backend. `libobs-d3d11/d3d11-subsystem.cpp` for the other.
5. `libobs/obs-video.c` – `obs_init_graphics` in `obs.c:478` (shows which effects load at start: `default.effect`, `default_rect.effect` (OpenGL only), `opaque.effect`, `solid.effect`, …), then `render_video`.

## Effect files (verified inventory)

`libobs/data/`: `area`, `bicubic_scale`, `bilinear_lowres_scale`, `color`, `default`, `default_rect`, `deinterlace_{base,blend,blend_2x,discard,discard_2x,linear,linear_2x,yadif,yadif_2x}`, `format_conversion`, `lanczos_scale`, `opaque`, `premultiplied_alpha`, `repeat`, `solid` (all `.effect`), plus ones shipped by plugins under `plugins/*/data/` and `frontend/data/striped_line.effect`. The test in P3-25 must glob them all; any HLSL feature used by *any* of them (e.g. `Texture2D.Load`, loops, `discard`, derivative instructions in the yadif shaders) is a requirement for the transpiler.

## Color formats

`enum gs_color_format` (verified): `GS_UNKNOWN, A8, R8, RGBA, BGRX, BGRA, R10G10B10A2, RGBA16, R16, RGBA16F, RGBA32F, RG16F, RG32F, R16F, R32F, DXT1, DXT3, DXT5, R8G8, RGBA_UNORM, BGRX_UNORM, BGRA_UNORM, RG16`. The `*_UNORM` variants mean "no sRGB conversion on sample/write" and exist because libobs does manual linear/sRGB handling (`linear_srgb` flag in `graphics_subsystem`; colour-space enums `GS_CS_SRGB`, `GS_CS_SRGB_16F`, `GS_CS_709_EXTENDED`, `GS_CS_709_SCRGB`). A wrong sRGB assumption shows as slightly wrong colours, not crashes: add an image-diff test for each format pair early.

## Texture sharing (the risky part)

APIs (grep in `graphics.h`): `gs_texture_open_shared(handle)`, `gs_texture_open_nt_shared(handle)` (Windows DXGI), `gs_texture_create_from_iosurface(void*)` (macOS), `gs_texture_create_from_dmabuf(w, h, drm_format, color_format, n_planes, fds, strides, offsets, modifiers)` (Linux; flags `GS_DMABUF_FLAG_*`), plus `GS_SHARED_TEX` / `GS_SHARED_KM_TEX` creation flags. The capture plugins depend on these; `wgpu` does not expose them through its safe API. Plan time for `wgpu-hal` escape hatches (Vulkan external memory with DRM format modifiers; D3D12/Vulkan import of NT handles; Metal `newTextureWithDescriptor:iosurface:plane:`), and keep the legacy backends selectable until each is proven.

## Backend feature checklist (what `wgpu` must cover)

- Device lifecycle and loss: `device_create`, `device_destroy`, adapter enumeration (`gs_enum_adapters`), `gs_get_device_type` (`GS_DEVICE_OPENGL`, `GS_DEVICE_DIRECT3D_11`, `GS_DEVICE_METAL` — add a new constant and check every `gs_get_device_type()` call in plugins: e.g. `obs_init_graphics` loads `default_rect.effect` only for OpenGL).
- Swap chains (`gs_swapchain_*`), render targets, zstencil, stage surfaces (CPU read-back, synchronous API), mapping/unmapping, copy/region copy.
- Blend/depth/stencil/raster state objects, samplers, viewport/scissor, vertex/index buffers (dynamic), `gs_draw`/`gs_load_*`.
- Timestamp queries (`gs_timer`, `gs_timer_range`) used by stats.
- HDR: `gs_is_monitor_hdr`, `gs_duplicator_get_sdr_white_level`; the nominal-peak / SDR-white values themselves are exposed to plugins at the libobs level (`obs_get_video_hdr_nominal_peak_level`, `obs_get_video_sdr_white_level`, `obs.h`), not by `gs_*`.

## Suggested order

1. P3-13…P3-17 device, resources, pipeline cache, render/readback, presentation (`wgpu` module; run the C engine on top).
2. P3-05 transpiler, then P3-24/25 (compile every effect, image-diff).
3. P3-18…P3-21 sharing (platform by platform, each with a capture plugin as the test).
4. P3-22/23 backend selection, HDR.
5. Optionally P3-01…P3-12, P3-07…P3-11 (Rust `graphics.c`/parsers) when ready to delete C.

## Pitfalls

- HLSL matrices are column-major by default in `mul(v, M)`; WGSL is column-major with `M * v`. The libobs shaders use `mul(float4(pos,1), ViewProj)`. Get the order wrong and everything renders mirrored/rotated. Write one test shader for it first.
- Sampler state combinations (`Filter`, address modes, `MaxAnisotropy`) are separate objects in libobs; in `wgpu` they are objects too but limited sets — build a table of what the effect files use.
- `gs_stagesurface_map` is synchronous in the API; `wgpu` maps are async. Use a staging ring and poll on the graphics thread; do not block on the main thread.
- Some backends need the display and the main renderer on the same thread (macOS Metal layer). Keep `gs_enter_context`/`gs_leave_context` semantics (thread-local current graphics with a recursion counter).
- Never assume the graphics thread is the process main thread.

## Verification checklist

- [ ] Every `.effect` (libobs + plugins + frontend) compiles under the transpiler with a golden WGSL.
- [ ] 120-frame deterministic render matches the legacy backend (PSNR > 60 dB, or bit-exact for integer paths).
- [ ] Capture plugins show frames on each OS through the shared-texture path.
- [ ] Frame time within 5% of legacy on the three reference scenes per OS.
