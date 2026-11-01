# Phase 3: Graphics

Goal: port the effect/shader language and the `gs_*` abstraction, then add a `wgpu` backend. Legacy backends (`libobs-opengl`, `libobs-d3d11`, `libobs-metal`) stay selectable until `wgpu` reaches parity.

Source map: `libobs/graphics/*` (non-math), `libobs/data/*.effect`, `libobs-opengl/`, `libobs-d3d11/`, `libobs-metal/`, `libobs-winrt/`, `deps/glad`, `libobs/obs-display.c`, `obs-view.c`, `obs-video.c` (render part), `obs-source.c` (render part), `obs-nix-*.c`, `obs-cocoa.m`, `obs-windows.c`.

## A. Effect language

- [ ] **P3-01 Lexer and preprocessor** (M): `cf-lexer.c`, `cf-parser.c` token streams, `#include`, `#define`. Golden: token dump of every `libobs/data/*.effect` and `plugins/**/data/*.effect`.
- [ ] **P3-02 Shader parser (HLSL-like)** (L): `shader-parser.c`: types, structs, functions, statements, expressions. AST golden dumps for all effect files.
- [ ] **P3-03 Effect parser** (L): `effect-parser.c`: techniques, passes, parameters, annotations, samplers, `vertex_shader`/`pixel_shader` entry points. Error messages must keep line/column format.
- [ ] **P3-04 Effect runtime** (M): `effect.c`: parameters set/get, `gs_effect_set_*`, loop over passes (`gs_effect_loop`), default values, `gs_effect_get_param_by_name`.
- [ ] **P3-05 Shader transpiler to WGSL** (XL, split into: expressions, intrinsics map, texture sampling, struct I/O, discard): via `naga` IR or direct emitter. Golden WGSL for every shipped effect + `naga` validation.
- [ ] **P3-06 Legacy transpilers** (L): keep the GLSL and HLSL emitters in Rust so GL/D3D11 backends keep working without the C parser (needed to drop the C parser).

## B. `gs_*` API

- [ ] **P3-07 Trait design** (M): `GraphicsDevice` trait covering `graphics.h`: textures (2D/3D/cube/stage/shared), render targets, zstencil, vertex/index buffers, shaders, samplers, blend/depth/stencil/raster state, viewport/scissor, matrices stack. Doc with C function mapping table.
- [ ] **P3-08 C shim `gs_*`** (L): `graphics.c`, `graphics-imports.c`, `device-exports.h`: all ~200 exported functions dispatch to the active backend through a vtable; thread-local graphics context (`gs_enter_context`, `gs_leave_context`) preserved.
- [ ] **P3-09 Matrix stack and transforms** (S): `gs_matrix_*`, projection, `gs_set_viewport`, `gs_ortho`, `gs_perspective`.
- [ ] **P3-10 Immediate-mode vertex API** (M): `gs_render_start/stop`, `gs_vertex2f`, `gs_color`, `gs_texcoord`, `gs_draw_sprite`, `gs_draw_quadf`; vertex buffer pooling.
- [ ] **P3-11 Texture utilities** (M): `texture-render.c`, `gs_texrender`, `gs_stagesurface`, `gs_texture_map/unmap`, `gs_copy_texture`, mip generation.
- [ ] **P3-12 Image loading** (M): `image-file.c`, `graphics-ffmpeg.c` (still/animated image via FFmpeg and `deps/libnsgif`), `gs_image_file_*`. Use `image` crate for PNG/JPG/GIF/WebP; keep FFmpeg for the rest. Golden pixel hash for 30 sample images.

## C. Backends

- [ ] **P3-13 `wgpu` device init** (M): adapter selection, surface, limits, features; mapping to `gs_init_data` (swap chain from native window handles for X11, Wayland, Win32, Cocoa).
- [ ] **P3-14 `wgpu` resources** (L): textures (all `gs_color_format` values incl. `RGBA16F`, `R10G10B10A2`, sRGB variants), buffers, samplers. Format support table and fallback.
- [ ] **P3-15 `wgpu` pipeline cache** (M): state combination -> render pipeline cache keyed on shader+blend+depth+raster; avoid per-frame creation.
- [ ] **P3-16 `wgpu` render and readback** (M): render targets, clear, draw, staging readback with async map but a sync API facade (`gs_stagesurface`).
- [ ] **P3-17 Frame pacing and presentation** (M): `obs-display.c`, vsync off by default, preview resizing, multiple displays (`obs_display_*`), draw callbacks.
- [ ] **P3-18 Shared texture import: Linux** (L): DMA-BUF import via `wgpu` Vulkan HAL (for `linux-pipewire`, `linux-capture`), with modifiers; GL EGL image fallback. Test with PipeWire screencast.
- [ ] **P3-19 Shared texture import: Windows** (L): DXGI shared handle / NT handle import (D3D12/Vulkan interop) for `win-capture` (game capture hooks); keep D3D11 fallback.
- [ ] **P3-20 Shared texture import: macOS** (L): IOSurface -> Metal texture for `mac-capture`, `mac-syphon`, `mac-virtualcam`.
- [ ] **P3-21 GPU encoder texture sharing** (L): the NVENC/QSV/AMF/VideoToolbox paths need a GPU texture handle (`gs_texture_get_obj`, `obs_encoder_*texture*`); define an escape hatch exposing native handles.
- [ ] **P3-22 Legacy backend adapters** (M): wrap `libobs-opengl`, `libobs-d3d11`, `libobs-metal` as `GraphicsDevice` implementations through the C vtable so both can be selected at runtime (`--graphics wgpu|legacy`).
- [ ] **P3-23 HDR and colour management** (M): HDR10/scRGB swap chains, SDR white level, `gs_get_hdr_nominal_peak_level`, sRGB framebuffer behaviour. Visual tests.

## D. Built-in effects and render loop

- [ ] **P3-24 Port built-in effects** (M): `libobs/data/*.effect` (default, solid, opaque, format_conversion, bicubic/lanczos/bilinear scale, premultiplied alpha, repeat, area). Image-diff test (PSNR > 60 dB vs legacy renders).
- [ ] **P3-25 Plugin effects** (M): effects shipped by `obs-filters`, `obs-transitions`, `image-source`, `text-freetype2`, `nv-filters` compile through the transpiler; test list auto-generated.
- [ ] **P3-26 Render loop** (L): `obs-video.c` render part: render sources -> main texture -> output texture -> scale/convert -> stage -> copy to CPU; GPU-conversion path. Golden: 120 frames of a deterministic scene as PNG hashes.
- [ ] **P3-27 Canvas and projectors** (M): `obs-canvas.c`, `obs-view.c`, multiple canvases, projector windows.

## E. Cleanup

- [ ] **P3-28 Backend selection UI/CLI and fallback** (S): auto-fallback to legacy if `wgpu` init fails; log adapter info.
- [ ] **P3-29 Performance gate** (M): GPU time per frame vs legacy on 3 reference scenes per OS; allowed regression <= 5%.
- [ ] **P3-30 Remove `deps/glad`** (S): only when the legacy GL backend no longer needs it.
- [ ] **P3-31 Phase 3 review** (S): visual diffs, performance, tick this file.

Exit criteria: all built-in and plugin effects compile, `wgpu` backend renders deterministic goldens, capture sources can share textures on the three OSes, legacy backend retained as a fallback.
