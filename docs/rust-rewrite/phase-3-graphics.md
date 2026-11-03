# Phase 3: Graphics

Goal: port the effect/shader language and the `gs_*` abstraction, then add a `wgpu` backend. Legacy backends (`libobs-opengl`, `libobs-d3d11`, `libobs-metal`) stay selectable until `wgpu` reaches parity.

Source map (libobs 33.0.1, line counts): `libobs/graphics/*` (non-math: 16 files + `libnsgif/` (4); key: `graphics.c` 3312, `effect-parser.c` 1972, `shader-parser.c` 717, `effect.c` 556, `graphics-ffmpeg.c` 787, `image-file.c` 409), `libobs/data/*.effect` (21 files), `libobs-opengl/` (27 files, 8,984 lines), `libobs-d3d11/` (16, 7,340), `libobs-metal/` (37, 8,530, mostly Swift), `deps/glad`, `libobs/obs-display.c` (311; presentation part; the `obs_display_*` API is P4-31), `libobs/obs-video.c` (1200; render part), `obs-video-gpu-encode.c` (312). Not here: `libobs-winrt/` (P5-33), `obs-canvas.c`/`obs-view.c` (P4-33/P4-31), `obs-source.c` render path (P4-05), platform window glue `obs-nix-*.c`/`obs-cocoa.m`/`obs-windows.c` (P4-32).

## A. Effect language

- [ ] **P3-01 Effect-file preprocessing** (S): drive the S-011 lexer/preprocessor (P1-08.3, `cf-lexer.c`/`cf-parser.c`) on `.effect` files: the `gs_preprocessor_name()` backend define (e.g. `OPENGL`) that `effect-parser.c` injects, plus `#include` resolution relative to the effect file. Done when: preprocessed token dump of every `libobs/data/*.effect` and `plugins/**/data/*.effect` equals the C dump.
- [ ] **P3-02 Shader parser (HLSL-like)** (L): `shader-parser.c`: types, structs, functions, statements, expressions. Done when: AST golden dumps for all effect files.
- [ ] **P3-03 Effect parser** (L): `effect-parser.c`: techniques, passes, parameters, annotations, samplers, `vertex_shader`/`pixel_shader` entry points. Done when: all shipped effects parse and error messages keep the C line/column format.
- [ ] **P3-04 Effect runtime** (M): `effect.c`: parameters set/get, `gs_effect_set_*`, loop over passes (`gs_effect_loop`), default values, `gs_effect_get_param_by_name`. Done when: set/get round-trips and pass loops behave equal to C.
- [ ] **P3-05 Shader transpiler to WGSL** (XL, split into: expressions, intrinsics map, texture sampling, struct I/O, discard): via `naga` IR or direct emitter. Done when: golden WGSL for every shipped effect passes `naga` validation.
- [ ] **P3-06 Legacy transpilers** (L): keep the GLSL and HLSL emitters in Rust so GL/D3D11 backends keep working without the C parser (needed to drop the C parser). Done when: GLSL/HLSL emitter output equals C for all shipped effects.

## B. `gs_*` API

- [ ] **P3-07 Trait design** (M): `GraphicsDevice` trait covering `graphics.h`: textures (2D/3D/cube/stage/shared), render targets, zstencil, vertex/index buffers, shaders, samplers, blend/depth/stencil/raster state, viewport/scissor, matrices stack. Done when: C function mapping table covers every export in `graphics.h` and `device-exports.h`.
- [ ] **P3-08 C shim `gs_*`** (L): `graphics.c`, `graphics-imports.c`: all `gs_*` exports of `graphics.h` (291) dispatch to the active backend through the 190 entries of `struct gs_exports` (`graphics-internal.h:30`; 115 `device_*` + 71 `gs_*` + 4 `gpu_*`); thread-local graphics context (`gs_enter_context`/`gs_leave_context`, refcounted) preserved. Done when: symbol diff vs `graphics.h`/`device-exports.h` is empty and the nested-context test passes.
- [ ] **P3-09 Matrix stack and transforms** (S): `gs_matrix_*`, projection, `gs_set_viewport`, `gs_ortho`, `gs_perspective`. Done when: matrix/projection goldens equal C.
- [ ] **P3-10 Immediate-mode vertex API** (M): `gs_render_start/stop`, `gs_vertex2f`, `gs_color`, `gs_texcoord`, `gs_draw_sprite`, `gs_draw_quadf`; vertex buffer pooling. Done when: immediate-mode vertex goldens equal C and the pool holds under a benchmark scene.
- [ ] **P3-11 Texture utilities** (M): `texture-render.c`, `gs_texrender`, `gs_stagesurface`, `gs_texture_map/unmap`, `gs_copy_texture`, mip generation. Done when: texrender/stagesurface round-trips and mipmap levels equal C.
- [ ] **P3-12 Image loading** (M): `image-file.c` (`gs_image_file_ex_*`), `graphics-ffmpeg.c` (FFmpeg decode of everything). Use the `image` crate for still PNG/JPG/BMP/TGA/WebP; animated GIF stays on the `libnsgif` path; keep FFmpeg for the rest. Done when: golden pixel hash for 30 sample images.

## C. Backends

- [ ] **P3-13 `wgpu` device init** (M): adapter selection, surface, limits, features; mapping to `gs_init_data` (swap chain from native window handles for X11, Wayland, Win32, Cocoa). Done when: wgpu presents a window on X11, Wayland, Win32 and Cocoa.
- [ ] **P3-14 `wgpu` resources** (L): textures (all `gs_color_format` values incl. `RGBA16F`, `R10G10B10A2`, sRGB variants), buffers, samplers. Done when: every `gs_color_format` maps or has a documented fallback.
- [ ] **P3-15 `wgpu` pipeline cache** (M): state combination -> render pipeline cache keyed on shader+blend+depth+raster; avoid per-frame creation. Done when: pipelines created per frame < 1 after warm-up.
- [ ] **P3-16 `wgpu` render and readback** (M): render targets, clear, draw, staging readback with async map but a sync API facade (`gs_stagesurface`). Done when: staged readback bytes equal C.
- [ ] **P3-17 Frame pacing and presentation** (M): `obs-display.c`, preview resizing, multiple displays (`obs_display_*`), draw callbacks, `gs_present` pacing (C: D3D11 legacy swap chain presents with interval 1 = vsync on; flip model paces via the frame-latency waitable object; macOS has a `DisableOSXVSync` setting — `gs_init_data` has no vsync field). Done when: present path matches C on each OS.
- [ ] **P3-18 Shared texture import: Linux** (L): DMA-BUF import via `wgpu` Vulkan HAL (for `linux-pipewire`, `linux-capture`), with modifiers; GL EGL image fallback. Done when: PipeWire screencast renders via the import path.
- [ ] **P3-19 Shared texture import: Windows** (L): DXGI shared handle / NT handle import (D3D12/Vulkan interop) for `win-capture` (game capture hooks); keep D3D11 fallback. Done when: game capture hooks render via the import path on Windows.
- [ ] **P3-20 Shared texture import: macOS** (L): IOSurface -> Metal texture for `mac-capture`, `mac-syphon`, `mac-virtualcam`. Done when: mac-capture/mac-syphon render via IOSurface import.
- [ ] **P3-21 GPU encoder texture sharing** (L): the NVENC/QSV/AMF/VideoToolbox paths need a GPU texture handle (`encode_texture`/`encode_texture2` callbacks, `struct encoder_texture`, `gs_texture_get_obj`); define an escape hatch exposing native handles. Done when: the escape hatch feeds the NVENC/QSV/AMF/VideoToolbox paths.
- [ ] **P3-22 Legacy backend adapters** (M): wrap `libobs-opengl`, `libobs-d3d11`, `libobs-metal` as `GraphicsDevice` implementations through the C vtable so both can be selected at runtime (`--graphics wgpu|legacy`). Done when: `--graphics legacy` renders goldens equal to the C build.
- [ ] **P3-23 HDR and colour management** (M): HDR10/scRGB swap chains, SDR white level (`gs_is_monitor_hdr`, `gs_duplicator_get_sdr_white_level` on the backend side; `obs_get_video_sdr_white_level` / `obs_get_video_hdr_nominal_peak_level` are libobs-level, Phase 4), sRGB framebuffer behaviour. Done when: SDR/HDR visual regressions match C.

## D. Built-in effects and render loop

- [ ] **P3-24 Port built-in effects** (M): all 21 `libobs/data/*.effect` (default, default_rect, solid, opaque, color, repeat, area, bicubic/lanczos/bilinear_lowres scale, format_conversion, premultiplied alpha, and the nine `deinterlace_*.effect`). Done when: image-diff passes (PSNR > 60 dB vs legacy renders).
- [ ] **P3-25 Plugin effects** (M): every `.effect` shipped by plugins (28 files: `obs-filters` 18, `obs-transitions` 6, `nv-filters` 3, `text-freetype2` 1) compiles through the transpiler; test list auto-generated by glob. Done when: all 28 plugin effects compile.
- [ ] **P3-26 Render loop** (L): `obs-video.c` render part: main texture -> output texture -> scale/convert -> stage -> copy to CPU (`render_main_texture`, `render_output_texture`, `render_convert_texture`, `stage_output_texture`, `set_gpu_converted_data`); the scene walk itself is P4-05 — drive these with a static test scene. Done when: 120 frames as PNG hashes equal the C reference.
- [ ] **P3-27 Multi-canvas render support** (S): the render loop must serve every mix/canvas (`obs->video.mixes`, per-mix output textures). The canvas/view objects themselves are P4-33/P4-31. Done when: two fake mixes render their own output textures.

## E. Cleanup

- [ ] **P3-28 Backend selection UI/CLI and fallback** (S): auto-fallback to legacy if `wgpu` init fails; log adapter info. Done when: fallback triggers on a forced `wgpu` init failure.
- [ ] **P3-29 Performance gate** (M): GPU time per frame vs legacy on 3 reference scenes per OS; allowed regression <= 5%. Done when: per-scene GPU time within 5% of legacy on all three OSes.
- [ ] **P3-30 Remove `deps/glad`** (S): only when the legacy GL backend no longer needs it. Done when: `deps/glad` removed and the legacy GL backend still builds.
- [ ] **P3-31 Phase 3 review** (S): visual diffs, performance, tick this file. Done when: review complete and this file ticked.

Exit criteria: all built-in and plugin effects compile, `wgpu` backend renders deterministic goldens, capture sources can share textures on the three OSes, legacy backend retained as a fallback.
