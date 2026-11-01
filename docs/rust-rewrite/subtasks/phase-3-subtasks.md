# Phase 3 subtasks

Parent file: [../phase-3-graphics.md](../phase-3-graphics.md). Each subtask has **Files**, **Steps**, **Done when**.

Verified file map: `libobs/graphics/` has `effect.c`, `effect-parser.c`, `shader-parser.c`, `graphics.c`, `graphics-imports.c`, `graphics-internal.h`, `device-exports.h`, `graphics-ffmpeg.c`, `image-file.c`, `texture-render.c`, `libnsgif/` and the math files. Built-in effects in `libobs/data/`: `area`, `bicubic_scale`, `bilinear_lowres_scale`, `color`, `default`, `default_rect`, `deinterlace_{base,blend,blend_2x,discard,discard_2x,linear,linear_2x,yadif,yadif_2x}`, `format_conversion`, `lanczos_scale`, `opaque`, `premultiplied_alpha`, `repeat`, `solid` (`.effect`). Backends: `libobs-opengl/` (`gl-*.c`, `gl-nix.c`, `gl-x11-egl.c`, `gl-wayland-egl.c`, `gl-windows.c`, `gl-cocoa.m`), `libobs-d3d11/` (`d3d11-*.cpp`), `libobs-metal/` (a real Swift backend: `MTL*+Extensions.swift`, `CVPixelFormat+Extensions.swift`, … — the earlier "≈32 lines" figure only counted C/C++/ObjC and understated it), `libobs-winrt/`. Deinterlace effects were missing from the earlier effect list and are added to P3-24.

**Ordering note (see `../guides/phase-3-guide.md`):** the `wgpu` backend (P3-13…P3-23) can be built first as a loadable `libobs-wgpu` module exporting the `device_*` symbols of `graphics-internal.h:30` under the unmodified C `graphics.c`. P3-26 (`render_video`, `output_frames`) lives in `obs-video.c` and belongs with Phase 4.

## A. Effect language

### P3-01 Lexer and preprocessor
- [ ] **P3-01.1 Tokens** — `libobs/util/cf-lexer.c/.h`: token kinds, whitespace/comments, string/number literals. Done when: Rust lexer passes the same token dump for all inputs.
- [ ] **P3-01.2 Preprocessor** — `#include` (relative to effect file), `#define`, `#ifdef`, line markers. Done when: cases with nested includes and missing file error.
- [ ] **P3-01.3 Goldens** — dump tokens for every file in `libobs/data/*.effect` and `plugins/*/data/*.effect`. Done when: committed `.golden`.

### P3-02 Shader parser
- [ ] **P3-02.1 AST** — types for `shader-parser.h`: `shader_var`, `shader_struct`, `shader_func`, `cf_token`s, expressions. Done when: AST can print itself.
- [ ] **P3-02.2 Declarations** — structs, globals, functions, semantics, `uniform`, `in/out`. Done when: parse all shipped effects.
- [ ] **P3-02.3 Statements/expressions** — precedence table from `shader-parser.c` (casts, swizzles, ternary, `mul`, calls). Done when: 100 expression cases equal to C dump.
- [ ] **P3-02.4 Goldens** — AST dump per effect file.

### P3-03 Effect parser
- [ ] **P3-03.1 Parameters** — `effect-parser.c` params with annotations and defaults (`uniform float4 color = {...}`). Done when: all param types.
- [ ] **P3-03.2 Samplers** — `sampler_state` fields (`Filter`, `AddressU/V/W`, `BorderColor`, `MaxAnisotropy`). Done when: golden.
- [ ] **P3-03.3 Techniques/passes** — `technique Draw { pass { vertex_shader = ...; pixel_shader = ...; } }`. Done when: all effect files parse.
- [ ] **P3-03.4 Errors** — messages `file(line,col): error ...` identical. Done when: 20 broken inputs compare to C's error text.

### P3-04 Effect runtime
- [ ] **P3-04.1 Param storage** — `gs_effect_set_float/int/bool/vec2/3/4/matrix4/texture/val/default_val`. Done when: type-mismatch behaviour equal.
- [ ] **P3-04.2 Lookup** — `gs_effect_get_param_by_name/idx`, `_by_idx`, `gs_effect_get_technique`. Done when: tests.
- [ ] **P3-04.3 Loop** — `gs_effect_loop`, `gs_technique_begin/end/_pass`. Done when: pass count semantics.
- [ ] **P3-04.4 Tests** for each type.

### P3-05 Shader transpiler to WGSL
- [ ] **P3-05.1 Design note** — `naga` IR vs direct text emitter; pick one. Done when: written decision with a spike.
- [ ] **P3-05.2 Types/IO** — HLSL `float4x4` ↔ `mat4x4<f32>` (row/column-major difference!), semantics `POSITION`, `TEXCOORD0`, `COLOR`, `SV_Target`. Done when: matrix-multiply order test.
- [ ] **P3-05.3 Intrinsics** — `mul`, `lerp`→`mix`, `saturate`→`clamp`, `frac`→`fract`, `rcp`, `atan2`, `step`, `pow`, `dot`, `normalize`, `clip`/`discard`. Done when: mapping table with test per entry.
- [ ] **P3-05.4 Textures** — `Texture2D`, `.Sample`, `.Load`, `.SampleLevel`, texture arrays, sampler pairing. Done when: sampler/texture binding layout documented.
- [ ] **P3-05.5 Control flow** — `if/for/while`, uniformity analysis for derivatives. Done when: loops compile under `naga` validation.
- [ ] **P3-05.6 Goldens** — WGSL per shipped effect, validated with `naga`.

### P3-06 Legacy transpilers
- [ ] **P3-06.1 GLSL** — port `libobs-opengl/gl-shaderparser.c` (emits GLSL from the shader AST). Done when: text equal on all effects.
- [ ] **P3-06.2 HLSL** — emitter used by `libobs-d3d11/d3d11-shaderprocessor.cpp`. Done when: equal.
- [ ] **P3-06.3 Compile check** — compile resulting GLSL/HLSL with driver or `glslang`/`fxc`. Done when: all effects compile.

## B. `gs_*` API

### P3-07 Trait design
- [ ] **P3-07.1 API table** — list every exported function in `graphics.h` and `device-exports.h` with group (texture, buffer, shader, state, draw). Done when: table in docs.
- [ ] **P3-07.2 Handles** — opaque IDs vs `Arc`; lifetime rules; `gs_texture_t*` etc. as pointers for C. Done when: ownership table.
- [ ] **P3-07.3 Errors/device-lost** — `GS_SUCCESS/GS_ERROR_*`, `gs_device_loss` callbacks. Done when: spec.

### P3-08 C shim
- [ ] **P3-08.1 Context** — `gs_enter_context/leave_context`, thread-local current graphics, recursion count. Done when: nested enter test.
- [ ] **P3-08.2 Resources exports** — create/destroy for texture, stagesurface, zstencil, samplerstate, vertexbuffer, indexbuffer, shader, effect. Done when: symbol check.
- [ ] **P3-08.3 State/draw exports** — blend, depth, stencil, raster, viewport, scissor, `gs_draw`, `gs_load_*`. Done when: symbol check.
- [ ] **P3-08.4 Symbol diff** vs `device-exports.h` and `graphics.h`.

### P3-09 Matrix stack and transforms
- [ ] **P3-09.1 Stack** — `gs_matrix_push/pop/identity/transpose/set/get/mul/translate/rotate/scale`. Done when: golden.
- [ ] **P3-09.2 Projection** — `gs_ortho`, `gs_frustum`, `gs_perspective`, `gs_set_viewport`, `gs_set_cur_context`. Done when: golden.
- [ ] **P3-09.3 Golden**

### P3-10 Immediate-mode vertex API
- [ ] **P3-10.1 Recording** — `gs_render_start(b_new)`, `gs_vertex*`, `gs_color`, `gs_texcoord*`, `gs_normal3f`, `gs_render_stop`. Done when: vertex data equal.
- [ ] **P3-10.2 Helpers** — `gs_draw_sprite`, `gs_draw_sprite_subregion`, `gs_draw_cube_backdrop`, `gs_draw_quadf`. Done when: vertex goldens.
- [ ] **P3-10.3 Pooling + benchmark**

### P3-11 Texture utilities
- [ ] **P3-11.1 texrender** — `libobs/graphics/texture-render.c`. Done when: begin/end with zstencil.
- [ ] **P3-11.2 stagesurface** — create/stage/map. Done when: pixel read-back equal.
- [ ] **P3-11.3 map/unmap/copy** — `gs_texture_map`, `gs_copy_texture(_region)`. Done when: tests.
- [ ] **P3-11.4 Mipmaps** — `GS_GEN_MIPMAPS`. Done when: level count test.

### P3-12 Image loading
- [ ] **P3-12.1 Static** — PNG, JPG, BMP, TGA via `image` crate (vs `graphics-ffmpeg.c`). Done when: pixel hash equal for 30 files.
- [ ] **P3-12.2 Animated** — GIF (`libnsgif`), APNG, WebP frame delays. Done when: delays equal.
- [ ] **P3-12.3 FFmpeg fallback** — formats the crate does not cover. Done when: list documented.
- [ ] **P3-12.4 Hash goldens**

## C. Backends

### P3-13 `wgpu` device init
- [ ] **P3-13.1 Adapter** — selection by index/name, power preference, `gs_init_data`. Done when: adapter enumerated like `gs_enum_adapters`.
- [ ] **P3-13.2 Features/limits** — required: BGRA, 16F, timestamps optional. Done when: negotiation logged.
- [ ] **P3-13.3 Swap chain X11/Wayland** — mapped from `libobs-opengl/gl-x11-egl.c` & `gl-wayland-egl.c` behaviour. Done when: window shows output.
- [ ] **P3-13.4 Swap chain Win32/Cocoa** — from `gl-windows.c`, `gl-cocoa.m`, `d3d11-subsystem.cpp`. Done when: same on Windows and macOS.

### P3-14 `wgpu` resources
- [ ] **P3-14.1 Format map** — table for each `gs_color_format` (A8, R8, RGBA, BGRX, BGRA, R10G10B10A2, RGBA16, R16, RGBA16F, RGBA32F, RG16F, RG32F, R16F, R32F, DXT1/3/5, R8G8, RGBA_UNORM, BGRX_UNORM, BGRA_UNORM, RG16). Done when: unsupported ones listed with fallback.
- [ ] **P3-14.2 Textures** — 2D/3D/cube, dynamic/render-target flags. Done when: create/destroy test.
- [ ] **P3-14.3 Buffers/samplers** — vertex/index, sampler states. Done when: test.
- [ ] **P3-14.4 Coverage test** — draw each format to a target.

### P3-15 Pipeline cache
- [ ] **P3-15.1 Key** — shader pair + blend + depth + stencil + raster + formats + topology. Done when: hashing tested.
- [ ] **P3-15.2 LRU** — bounded size. Done when: eviction test.
- [ ] **P3-15.3 Metrics** — pipelines created per frame < 1 after warm-up.

### P3-16 Render and readback
- [ ] **P3-16.1 Targets/clear** — `gs_set_render_target`, `gs_clear`. Done when: pixel test.
- [ ] **P3-16.2 Draw** — submit passes lazily and batch encoder. Done when: 1000 draws/frame benchmark.
- [ ] **P3-16.3 Staging** — synchronous `gs_stagesurface_map` facade over async map. Done when: no deadlock under single-thread use.

### P3-17 Frame pacing and presentation
- [ ] **P3-17.1 Display** — `obs_display_create/resize/destroy` from `libobs/obs-display.c`. Done when: resize without flicker.
- [ ] **P3-17.2 Multiple + callbacks** — draw callbacks, background colour. Done when: two displays at once.
- [ ] **P3-17.3 Present modes** — vsync off default, `gs_present`. Done when: configurable.

### P3-18 Shared texture import: Linux
- [ ] **P3-18.1 DMA-BUF** — `plugins/linux-pipewire/pipewire.c` produces them; import with modifiers via Vulkan external memory. Done when: screencast shows.
- [ ] **P3-18.2 EGL fallback** — `libobs-opengl/gl-egl-common.c` path (`gs_texture_create_from_dmabuf`). Done when: works on NVIDIA proprietary.
- [ ] **P3-18.3 Test** GNOME + KDE.

### P3-19 Shared texture import: Windows
- [ ] **P3-19.1 Open handle** — `gs_texture_open_shared` / `_nt_shared`. Done when: D3D11 sample texture visible.
- [ ] **P3-19.2 Sync** — keyed mutex / fence. Done when: no tearing in game capture.
- [ ] **P3-19.3 Test** with `plugins/win-capture/graphics-hook` samples.

### P3-20 Shared texture import: macOS
- [ ] **P3-20.1 IOSurface** — `gs_texture_create_from_iosurface`, `gs_texture_rebind_iosurface`. Done when: Syphon frame visible.
- [ ] **P3-20.2 Test** with `plugins/mac-capture/mac-sck-video-capture.m`.

### P3-21 GPU encoder texture sharing
- [ ] **P3-21.1 Escape hatch** — `gs_texture_get_obj` returning native handle plus enum of kind. Done when: documented.
- [ ] **P3-21.2 D3D** — `d3d11-texture2d.cpp` semantics, `obs-video-gpu-encode.c`. Done when: NVENC D3D11 path receives a handle.
- [ ] **P3-21.3 CUDA/VAAPI/Metal** — export via external memory / `VkExternal` / `MTLTexture`. Done when: one per platform proven.

### P3-22 Legacy backend adapters
- [ ] **P3-22.1 OpenGL** — load `libobs-opengl` via the C `gs_exports` vtable. Done when: renders test scene.
- [ ] **P3-22.2 D3D11** — same for `libobs-d3d11`.
- [ ] **P3-22.3 Metal** — same for the Swift `libobs-metal` backend (it is part of the C ABI vtable, written in Swift; check how CMake builds it first). Done when: renders the test scene on macOS and the build note is recorded.
- [ ] **P3-22.4 Runtime switch** — `--graphics wgpu|legacy` and settings key.

### P3-23 HDR and colour management
- [ ] **P3-23.1 HDR targets** — `RGBA16F` + scRGB/HDR10 swap chains. Done when: displays on HDR monitor.
- [ ] **P3-23.2 SDR white level** — `gs_is_monitor_hdr`, `gs_duplicator_get_sdr_white_level` (backend side); `obs_get_video_sdr_white_level` and `obs_get_video_hdr_nominal_peak_level` are libobs-level (Phase 4). Done when: values correct on an HDR monitor.
- [ ] **P3-23.3 Visual regression** — SDR on SDR, SDR on HDR, HDR on HDR scenes.

## D. Built-in effects and render loop

### P3-24 Built-in effects
- [ ] **P3-24.1 Compile** — every file in `libobs/data/*.effect` including the nine `deinterlace_*.effect`. Done when: all compile on `wgpu`.
- [ ] **P3-24.2 Reference render** — render fixed inputs with the legacy backend. Done when: PNGs stored.
- [ ] **P3-24.3 Compare** — PSNR > 60 dB (or exact for integer paths).

### P3-25 Plugin effects
- [ ] **P3-25.1 Enumerate** — glob `plugins/*/data/**/*.effect`. Done when: list generated in CI.
- [ ] **P3-25.2 Compile test** for all.
- [ ] **P3-25.3 Fix gaps** — missing intrinsics.

### P3-26 Render loop
- [ ] **P3-26.1 Main render** — `obs-video.c`: `render_main_texture`, `render_output_texture`. Done when: frame hash equal.
- [ ] **P3-26.2 Output/scale/convert** — `render_convert_texture`, scaling with `bilinear`/`bicubic`/`lanczos`/`area`. Done when: golden per scale type.
- [ ] **P3-26.3 Stage/copy** — `stage_output_texture`, `set_gpu_converted_data`. Done when: CPU frame equals.
- [ ] **P3-26.4 GPU encode path** — `obs-video-gpu-encode.c`. Done when: NVENC path unchanged.
- [ ] **P3-26.5 Deterministic golden** — 120 frames hashed.

### P3-27 Canvas and projectors
- [ ] **P3-27.1 Canvas** — `libobs/obs-canvas.c`, `obs-view.c`. Done when: second canvas renders.
- [ ] **P3-27.2 Projectors** — display callbacks. Done when: windowed projector.
- [ ] **P3-27.3 Test** with 2 canvases.

## E. Cleanup

### P3-28 Backend selection and fallback
- [ ] **P3-28.1 Fallback** on init failure. Done when: forced failure test.
- [ ] **P3-28.2 Logging** — adapter name, driver, features. Done when: appears in log.
- [ ] **P3-28.3 Config** key and CLI.

### P3-29 Performance gate
- [ ] **P3-29.1 Scenes** — three reference scenes per OS (simple, 20 sources with filters, 4K). Done when: stored in `test/fixtures`.
- [ ] **P3-29.2 Measure** GPU time both backends.
- [ ] **P3-29.3 Gate** at 5%.

### P3-30 Remove `deps/glad`
- [ ] **P3-30.1 Users** — grep `glad`. Done when: only `libobs-opengl`.
- [ ] **P3-30.2 Delete** when legacy GL is gone (Phase 7).

### P3-31 Phase 3 review
- [ ] **P3-31.1 Review** visual diffs and perf.
- [ ] **P3-31.2 Tick** parents.
