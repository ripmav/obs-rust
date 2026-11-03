# Phase 3 subtasks

Parent file: [../phase-3-graphics.md](../phase-3-graphics.md). Each subtask has **Files**, **Steps**, **Done when**.

Verified file map (libobs 33.0.1 checkout, line counts): `libobs/graphics/` (46 files, 17,060 lines) has the non-math files `graphics.c` (3312), `graphics.h` (1025), `effect-parser.c/.h` (1972/290), `shader-parser.c/.h` (717/273), `effect.c/.h` (556/190), `graphics-imports.c` (259), `graphics-internal.h` (335), `device-exports.h` (177), `graphics-ffmpeg.c` (787), `image-file.c/.h` (409/58), `texture-render.c` (151), `input.h` (34) and `libnsgif/` (`gif.c` 2076, `lzw.c` 613, `lzw.h` 137, `nsgif.h` 527, + LICENSE/`.clang-format`); the remaining ~20 files are math/geometry support (`matrix4.c` 333, `vec3.c` 87, `vec2.c` 51, `vec4.c` 41, `quat.c` 215, `axisang.c` 38, `matrix3.c` 138, `plane.c` 151, `bounds.c` 314, `math-extra.c` 131, `srgb.h` 177, `half.h` 100 + headers). Built-in effects in `libobs/data/` (21 `.effect` files): `area`, `bicubic_scale`, `bilinear_lowres_scale`, `color`, `default`, `default_rect`, `deinterlace_{base,blend,blend_2x,discard,discard_2x,linear,linear_2x,yadif,yadif_2x}`, `format_conversion`, `lanczos_scale`, `opaque`, `premultiplied_alpha`, `repeat`, `solid`. Plugin effects: 28 files — `plugins/obs-filters/data/` (18), `plugins/obs-transitions/data/` (6), `plugins/nv-filters/data/` (3: `color`, `rtx_blur`, `rtx_greenscreen`), `plugins/text-freetype2/data/text_default.effect` (1); plus `frontend/data/striped_line.effect`. Backends: `libobs-opengl/` (27 files, 8,984 lines incl. CMake: `gl-subsystem.c` 1627, `gl-subsystem.h` 677, `gl-shader.c` 763, `gl-shaderparser.c` 773, `gl-x11-egl.c` 666, `gl-cocoa.m` 623, `gl-windows.c` 605, `gl-egl-common.c` 581, `gl-wayland-egl.c` 476, `gl-texture2d.c` 267, `gl-vertexbuffer.c` 257, `gl-stagesurf.c` 230, +15 more), `libobs-d3d11/` (16 files, 7,340 lines: `d3d11-subsystem.cpp` 3435, `d3d11-subsystem.hpp` 1046, `d3d11-rebuild.cpp` 605, `d3d11-shader.cpp` 546, `d3d11-texture2d.cpp` 381, `d3d11-duplicator.cpp` 315, `d3d11-shaderprocessor.cpp` 247, `d3d11-texture3d.cpp` 236, +8 more), `libobs-metal/` (a real Swift backend, 37 files, 8,530 lines: `OBSShader.swift` 1603, `metal-subsystem.swift` 985, `libobs+Extensions.swift` 486, `MetalDevice.swift` 789, `metal-shader.swift` 593, `metal-texture2d.swift` 528, +31 more — the earlier "≈32 lines" figure only counted C/C++/ObjC and understated it). `libobs-winrt/` is **not** a graphics backend (it is the Windows.Graphics.Capture module, P5-33). Deinterlace effects were missing from the earlier effect list and are added to P3-24.

**Ordering note (see `../guides/phase-3-guide.md`):** the `wgpu` backend (P3-13…P3-23) can be built first as a loadable `libobs-wgpu` module exporting the `device_*` symbols of `graphics-internal.h:30` under the unmodified C `graphics.c`. P3-26 ports the render functions of `obs-video.c` in Phase 3 (static test scene); their callers (`render_video`, `output_frames`, the scene walk) wire up in Phase 4 (P4-05).

## A. Effect language

### P3-01 Effect-file preprocessing
- [ ] **P3-01.1 Backend define**
  - Files: `libobs/graphics/effect-parser.c` (1972) — the define injection; the lexer/preprocessor engine is `libobs/util/cf-lexer.c` (1376), `cf-parser.c` (60), `lexer.c` (305) (P1-08.3, S-011 — already ported in Phase 1).
  - Steps: drive the Phase-1 cf parser on `.effect` files; inject the `gs_preprocessor_name()` define (e.g. `OPENGL`) as an API definition (`cf_preprocessor_add_def`) before `cf_parser_parse`.
  - Spec: S-020 §5.1 (entry), S-011 (lexer/preprocessor).
  - Vectors: effect (41 scripts), lexer (25 scripts).
  - Done when: `#ifdef`-per-backend effect files parse equal to C.
- [ ] **P3-01.2 Include resolution**
  - Files: `libobs/graphics/effect-parser.c` (1972).
  - Steps: resolve `#include "..."` relative to the including effect file (the lexer/preprocessor engine itself is P1-08.3, S-011); cover nested includes and the missing-file error.
  - Spec: S-020 §5.1, D-8.
  - Pitfalls: S-020 D-8 — tokens from an `#include`d file keep a pointer to a dead stack object in C, so reference errors print garbage file/row/col; report the included file's name and its own row/column instead.
  - Done when: cases with nested includes and missing file error.
- [ ] **P3-01.3 Goldens**
  - Files: all 21 `libobs/data/*.effect`, the 28 `plugins/*/data/*.effect`, `frontend/data/striped_line.effect`.
  - Steps: dump the preprocessed token stream for every file; commit as `.golden`.
  - Vectors: effect.
  - Done when: committed `.golden`.

### P3-02 Shader parser
- [ ] **P3-02.1 AST**
  - Files: `libobs/graphics/shader-parser.c` (717), `libobs/graphics/shader-parser.h` (273).
  - Steps: define the AST types for `shader_var`, `shader_struct`, `shader_func`, `cf_token`s, expressions in `crates/obs-graphics` (S-020 target crate); the AST must print itself.
  - Spec: S-020 §4 (data formats), §5.7 (shader parser).
  - Done when: AST can print itself.
- [ ] **P3-02.2 Declarations**
  - Files: `libobs/graphics/shader-parser.c` (717).
  - Steps: parse structs, globals, functions, semantics, `uniform`, `in/out`; parse all shipped effects (21 libobs + 28 plugin).
  - Spec: S-020 §5.7.
  - Vectors: effect.
  - Done when: parse all shipped effects.
- [ ] **P3-02.3 Statements/expressions**
  - Files: `libobs/graphics/shader-parser.c` (717).
  - Steps: port the precedence table from `shader-parser.c` (casts, swizzles, ternary, `mul`, calls); 100 expression cases equal to the C dump.
  - Spec: S-020 §5.7.
  - Vectors: effect.
  - Done when: 100 expression cases equal to C dump.
- [ ] **P3-02.4 Goldens**
  - Steps: AST dump per effect file; commit.
  - Vectors: effect.
  - Done when: goldens committed and green.

### P3-03 Effect parser
- [ ] **P3-03.1 Parameters**
  - Files: `libobs/graphics/effect-parser.c` (1972), `libobs/graphics/effect-parser.h` (290).
  - Steps: parse params with annotations and defaults (`uniform float4 color = {...}`) in all param types.
  - Spec: S-020 §5.2 (declarations and parameters), §4.2 (stored defaults).
  - Pitfalls: Q-1 — int defaults use base-10 `strtol`: `0x10` is 0, `007` is 7, `1.5` is 1. Q-2 — float defaults use C `strtod`: hex floats (`0x10`, `0x1p3`) accepted. Q-3 — `1e-3`/`2E+4` cannot be written (lexer splits at the sign; declaration dropped + `Expected 'type'`). Q-6 — int/bool defaults are C `long` (8 bytes on LP64) but the setters store 4 bytes: backends must accept both sizes. Q-J — an array/annotation/default in the "wrong" order is dropped; the fixed order is `[N] <...> = value`.
  - Done when: all param types.
- [ ] **P3-03.2 Samplers**
  - Files: `libobs/graphics/effect-parser.c` (1972).
  - Steps: parse `sampler_state` fields (`Filter`, `AddressU/V/W`, `BorderColor`, `MaxAnisotropy`).
  - Spec: S-020 §5.3 (structs and samplers).
  - Pitfalls: Q-K — a sampler without `Filter` is POINT (zero-init), not LINEAR. Q-E/Q-F — `BorderColor = #RRGGBB` fails through the shader parser; the first character of the value is skipped by `shader_sampler_convert`.
  - Done when: golden.
- [ ] **P3-03.3 Techniques/passes**
  - Files: `libobs/graphics/effect-parser.c` (1972).
  - Steps: parse `technique Draw { pass { vertex_shader = ...; pixel_shader = ...; } }` including stage calls.
  - Spec: S-020 §5.4 (functions, techniques, passes).
  - Pitfalls: Q-H — a technique that contains something other than passes silently loses everything after it; a pass name token is never validated; a second assignment to the same stage is ignored.
  - Done when: all effect files parse.
- [ ] **P3-03.4 Errors**
  - Files: `libobs/graphics/effect-parser.c` (1972).
  - Steps: emit messages in the C format `file(line,col): error ...` from the S-020 §4.4 error catalogue; 20 broken inputs compare to C's error text.
  - Spec: S-020 §4.4 (error message catalogue).
  - Pitfalls: Q-I — errors in a declaration frequently produce *no* message (dropped struct members, `float x[2;`); parsing resumes mid-statement.
  - Done when: 20 broken inputs compare to C's error text.

### P3-04 Effect runtime
- [ ] **P3-04.1 Param storage**
  - Files: `libobs/graphics/effect.c` (556), `libobs/graphics/effect.h` (190).
  - Steps: port `gs_effect_set_float/int/bool/vec2/3/4/matrix4/texture/val/default_val`; match the type-mismatch behaviour.
  - Spec: S-020 §5.6 (effect runtime).
  - Pitfalls: S-020 D-5 — `gs_effect_set_default(NULL)` must log ERROR `effect_setval_inline: invalid param` and no-op (C crashes).
  - Done when: type-mismatch behaviour equal.
- [ ] **P3-04.2 Lookup**
  - Files: `libobs/graphics/effect.c` (556).
  - Steps: port `gs_effect_get_param_by_name/idx`, `_by_idx`, `gs_effect_get_technique`.
  - Spec: S-020 §5.6, D-3, D-6, D-7.
  - Pitfalls: S-020 D-3/D-6 — NULL technique or param name must return NULL/false, no crash (C crashes on `strcmp`). D-7 — `gs_technique_get_pass_by_idx`/`gs_param_get_annotation_by_idx` out of range must return NULL (C returns one past the end).
  - Done when: tests.
- [ ] **P3-04.3 Loop**
  - Files: `libobs/graphics/effect.c` (556).
  - Steps: port `gs_effect_loop`, `gs_technique_begin/end/_pass`; match pass count semantics.
  - Spec: S-020 §5.6.
  - Done when: pass count semantics.
- [ ] **P3-04.4 Tests** for each type.
  - Vectors: effect `60-runtime-lookup`, `61-runtime-values`, `62-runtime-loop`.
  - Done when: all param types round-trip.

### P3-05 Shader transpiler to WGSL
- [ ] **P3-05.1 Design note**
  - Steps: spike `naga` IR vs direct text emitter; write the decision.
  - Note: no spec covers WGSL output (wgpu has no C counterpart); the input is the assembled shader text of S-020 §4.3.
  - Done when: written decision with a spike.
- [ ] **P3-05.2 Types/IO**
  - Steps: map HLSL `float4x4` ↔ `mat4x4<f32>`, semantics `POSITION`, `TEXCOORD0`, `COLOR`, `SV_Target`; one test shader for the matrix-multiply order.
  - Spec: S-020 §4.3 (assembled shader text, worked example).
  - Pitfalls: HLSL `mul(v, M)` vs WGSL column-major `M * v` — get the order wrong and everything renders mirrored/rotated; write one test shader for it first (guide "Pitfalls").
  - Done when: matrix-multiply order test.
- [ ] **P3-05.3 Intrinsics**
  - Steps: build the mapping table (`mul`, `lerp`→`mix`, `saturate`→`clamp`, `frac`→`fract`, `rcp`, `atan2`, `step`, `pow`, `dot`, `normalize`, `clip`/`discard`) with one test per entry.
  - Done when: mapping table with test per entry.
- [ ] **P3-05.4 Textures**
  - Steps: map `Texture2D`, `.Sample`, `.Load`, `.SampleLevel`, texture arrays, sampler pairing; document the binding layout.
  - Done when: sampler/texture binding layout documented.
- [ ] **P3-05.5 Control flow**
  - Steps: `if/for/while`, uniformity analysis for derivatives; loops must compile under `naga` validation.
  - Done when: loops compile under `naga` validation.
- [ ] **P3-05.6 Goldens**
  - Steps: WGSL per shipped effect, validated with `naga`; commit.
  - Done when: goldens committed and `naga`-validated.

### P3-06 Legacy transpilers
- [ ] **P3-06.1 GLSL**
  - Files: `libobs-opengl/gl-shaderparser.c` (773), `libobs-opengl/gl-shaderparser.h` (88).
  - Steps: port the OBS-HLSL → GLSL text emitter (the GL backend's only algorithmic piece) into `crates/obs-gl-backend` (S-057 target crate).
  - Spec: S-057 §6 (contract is R35/R36/R40 + S-020; the generated GLSL text itself is not normative — only that it compiles and computes right).
  - Vectors: glbackend (22 scripts).
  - Done when: text equal on all effects (where the vectors pin it) and it compiles.
- [ ] **P3-06.2 HLSL**
  - Files: `libobs-d3d11/d3d11-shaderprocessor.cpp` (247), `d3d11-shaderprocessor.hpp` (38), `d3d11-shader.cpp` (546).
  - Steps: port the HLSL emitter used by `d3d11-shaderprocessor.cpp` (prepares the shader text for the D3D compiler).
  - Done when: equal.
- [ ] **P3-06.3 Compile check**
  - Steps: compile the resulting GLSL/HLSL with the driver or `glslang`/`fxc`.
  - Vectors: glbackend (GLSL compile errors are part of the contract, S-057 R35/R36).
  - Done when: all effects compile.

## B. `gs_*` API

### P3-07 Trait design
- [ ] **P3-07.1 API table**
  - Files: `libobs/graphics/graphics.h` (1025), `libobs/graphics/device-exports.h` (177).
  - Steps: list every exported function in `graphics.h` and `device-exports.h` with a group (texture, buffer, shader, state, draw).
  - Spec: S-043 §3 (interface, backend contract).
  - Done when: table in docs.
- [ ] **P3-07.2 Handles**
  - Files: `libobs/graphics/graphics.h` (1025), `libobs/graphics/graphics-internal.h` (335).
  - Steps: decide opaque IDs vs `Arc`; document lifetime rules; `gs_texture_t*` etc. as pointers for C.
  - Spec: S-043 §3.2–3.3 (types, object state).
  - Done when: ownership table.
- [ ] **P3-07.3 Errors/device-lost**
  - Steps: map `GS_SUCCESS`/`GS_ERROR_*`, `gs_device_loss` callbacks.
  - Spec: S-043 §3.1 (backend contract).
  - Done when: spec.

### P3-08 C shim
- [ ] **P3-08.1 Context**
  - Files: `libobs/graphics/graphics.c` (3312).
  - Steps: port `gs_enter_context`/`gs_leave_context` (R07–R09): thread-local current graphics, reentrant use count, "leave previous context completely first".
  - Spec: S-043 §5.3 R07–R09.
  - Vectors: graphics (31 scripts; `02-context`, `03-threads`).
  - Pitfalls: S-043 Q-1 — `gs_create` ends with **no** current context on the calling thread; if the thread was inside another object its mutex stays held and re-entering blocks forever — SHOULD preserve the previous thread context across creation.
  - Done when: nested enter test.
- [ ] **P3-08.2 Resources exports**
  - Files: `libobs/graphics/graphics.c` (3312).
  - Steps: create/destroy for texture, stagesurface, zstencil, samplerstate, vertexbuffer, indexbuffer, shader, effect per R27–R39.
  - Spec: S-043 §5.9 R27–R39.
  - Vectors: graphics (`10-texture-creation`, `11-texture-queries`, `22-buffers`, `25-effect-cache`).
  - Pitfalls: Q-2 — for normal 2D textures any non-zero flip bits use the vertically flipped buffer; `GS_FLIP_U` alone does not flip horizontally (files depend on it). Q-5 — `DUP_BUFFER` with `num == 0` passes the caller's pointer through (backend owns it). Q-8 — render-target mipmap sanitising keeps the data pointer for 2D but drops it for cube textures. D-2 — `gs_swapchain_create(NULL)` must not crash (R02 + NULL).
  - Done when: symbol check.
- [ ] **P3-08.3 State/draw exports**
  - Files: `libobs/graphics/graphics.c` (3312).
  - Steps: blend (R24–R26), depth, stencil, raster, viewport/scissor (R20–R23), `gs_draw`, `gs_load_*`.
  - Spec: S-043 §5.7–5.8 R20–R26.
  - Vectors: graphics (`13-blend-state`, `14-viewport-projection`, `23-load-calls`).
  - Pitfalls: Q-3 — `gs_reset_blend_state` leaves a non-ADD op alone unless a factor was also non-default. D-1 — the D-1 function list (incl. `gs_matrix_scale3f`, `gs_texture_create_from_dmabuf`) dereferences the context without a check in C: outside a context return the R01 defaults (debug log, false/NULL) instead of crashing.
  - Done when: symbol check.
- [ ] **P3-08.4 Symbol diff** vs `device-exports.h` and `graphics.h`.
  - Steps: diff the 291 `graphics.h` exports and the 190 `gs_exports` entries (115 `device_*` + 71 `gs_*` + 4 `gpu_*`) against the shim.
  - Spec: S-043 §3.1.
  - Done when: symbol diff empty.

### P3-09 Matrix stack and transforms
- [ ] **P3-09.1 Stack**
  - Files: `libobs/graphics/graphics.c` (3312; the matrix section), `libobs/graphics/matrix4.c` (333), `matrix4.h` (102).
  - Steps: port `gs_matrix_push/pop/identity/transpose/set/get/mul/translate/rotaa/rotquat/scale` (+ the `...3f`/`4f` f32 variants); frontend only, no backend calls.
  - Spec: S-043 §5.4 R10–R12; S-008 (matrix4 operations, target crate `obs-math`).
  - Vectors: graphics `04-matrix-stack`; math (S-008, 8 scripts).
  - Pitfalls: R10 — pop with a single entry logs ERROR `Tried to pop last matrix on stack` and does nothing; `mul` is `top = top * m`; translate/scale/rotate use the `_i` (inverse-order) variants.
  - Done when: golden.
- [ ] **P3-09.2 Projection**
  - Files: `libobs/graphics/graphics.c` (3312).
  - Steps: port `gs_ortho`, `gs_frustum`, `gs_perspective`, `gs_set_viewport`, `gs_get_context`.
  - Spec: S-043 §5.7 R20–R23.
  - Pitfalls: R22 — `gs_perspective` computes `ymax = near*tanf(RAD(angle)*0.5f)` and the backend's top/bottom parameters receive `ymin`/`ymax` (not swapped).
  - Done when: golden.
- [ ] **P3-09.3 Golden**
  - Steps: matrix/projection goldens from the vectors.
  - Vectors: graphics.
  - Done when: goldens committed and green.

### P3-10 Immediate-mode vertex API
- [ ] **P3-10.1 Recording**
  - Files: `libobs/graphics/graphics.c` (3312; the immediate-mode section).
  - Steps: port `gs_render_start(b_new)`, `gs_vertex*`, `gs_color`, `gs_texcoord*`, `gs_normal3f`, `gs_render_stop` per R13–R16 (512-entry immediate arrays, count checks, retained vs immediate flush).
  - Spec: S-043 §5.5 R13–R16.
  - Vectors: graphics (`05-immediate-mode`, `06-immediate-limits`, `07-retained-vertex-building`).
  - Pitfalls: R14 — appending a 513rd immediate entry logs ERROR `<what>: tried to use over 512 for immediate rendering` and drops the value. D-6 — `gs_texcoord(unit >= 16)` indexes out of bounds in C; Rust SHOULD pad or reject.
  - Done when: vertex data equal.
- [ ] **P3-10.2 Helpers**
  - Files: `libobs/graphics/graphics.c` (3312).
  - Steps: port `gs_draw_sprite`, `gs_draw_sprite_subregion`, `gs_draw_quadf` (R17–R19: texel-unit UVs for rect textures, subregion buffer); `gs_draw_cube_backdrop` is a stub (R21).
  - Spec: S-043 §5.6 R17–R19, §5.7 R21.
  - Vectors: graphics (`08-sprites`, `09-sprite-null-and-sizes`).
  - Pitfalls: Q-2 — any non-zero flip selects the vertically flipped buffer (`gs_draw_sprite(tex, flip, ...)`).
  - Done when: vertex goldens.
- [ ] **P3-10.3 Pooling + benchmark**
  - Steps: pool the immediate/retained buffers; criterion bench vs the P0-17 scene-render baseline.
  - Done when: bench report committed.

### P3-11 Texture utilities
- [ ] **P3-11.1 texrender**
  - Files: `libobs/graphics/texture-render.c` (151).
  - Steps: port `gs_texrender_create/begin/end/reset` per R41–R43 (viewport/projection/matrix pushes, render-target restore, `rendered` latch).
  - Spec: S-043 §5.10 R41–R43.
  - Vectors: graphics `16-texrender`.
  - Pitfalls: Q-7 — a failed texrender allocation remembers the new size, so a repeated begin with the same size fails without retrying. D-4 — `gs_texrender_get_format(NULL)` must return `GS_UNKNOWN` (C crashes).
  - Done when: begin/end with zstencil.
- [ ] **P3-11.2 stagesurface**
  - Files: `libobs/graphics/graphics.c` (3312; `gs_stagesurface*`).
  - Steps: create/stage/map for stage surfaces.
  - Spec: S-043 §5.9 (R27–R30).
  - Vectors: graphics (`17-texture-set-image`, `22-buffers`).
  - Done when: pixel read-back equal.
- [ ] **P3-11.3 map/unmap/copy**
  - Files: `libobs/graphics/graphics.c` (3312).
  - Steps: port `gs_texture_map`, `gs_texture_set_image` (R31 row copy with flip), `gs_copy_texture(_region)` (R03/R04 NULL rules).
  - Spec: S-043 §5.9 R30–R31, §5.1 R03–R04.
  - Done when: tests.
- [ ] **P3-11.4 Mipmaps**
  - Files: `libobs/graphics/graphics.c` (3312).
  - Steps: port the `GS_BUILD_MIPMAPS` flag + `levels` param handling of `gs_texture_create` (R27) and cube (R28), including the warning text.
  - Spec: S-043 §5.9 R27–R28.
  - Pitfalls: R27 — non-pow2 with mipmaps: WARNING `Cannot use mipmaps with a non-power-of-two texture.  Disabling mipmaps for this texture.`, flag cleared, `levels = 1`; render targets lose the mipmaps too (second warning) but keep the data pointer (cube drops it, Q-8).
  - Done when: level count + warning test.

### P3-12 Image loading
- [ ] **P3-12.1 Static**
  - Files: `libobs/graphics/image-file.c` (409), `image-file.h` (58), `libobs/graphics/graphics-ffmpeg.c` (787).
  - Steps: decode PNG/JPG/BMP/TGA/WebP with the `image` crate in `crates/graphics-image` (S-053 target crate); reproduce the C swscale BGRA conversion (rounding) for gray/palette/YUV inputs; 30 sample files.
  - Spec: S-053 §5.2 (`gs_image_file_ex`), §5.3 (still decoding).
  - Vectors: gfximages (`30-still-png`, `31-still-formats-errors`, `32-reformat-orientation`, `33-reformat-pixel-formats`).
  - Pitfalls: D-3 (S-053) — EXIF orientation outside 1..8 leaves the output buffer uninitialised in C (only pixel 0 written); treat unknown values as 1.
  - Done when: pixel hash equal for 30 files.
- [ ] **P3-12.2 Animated**
  - Files: `libobs/graphics/libnsgif/`: `gif.c` (2076), `lzw.c` (613), `lzw.h` (137), `nsgif.h` (527); APNG/WebP via the `image` crate.
  - Steps: keep the libnsgif path (or a crate replacement that preserves raw cs delays, `loop_max` semantics and the frame composition of S-053 §6.9); port `gs_image_file_ex` animation (R28–R29 frame switch, tick).
  - Spec: S-053 §3.3, §5.1 (libnsgif R01..R10), §5.2, §6.2–6.9.
  - Vectors: gfximages (`10-gif-basic` … `17-gif-lzw`, `20`–`24`).
  - Pitfalls: D-1 — the C GIF scan-error test never fires (`nsgif_error` values are non-negative), so any corrupt/truncated/illegal-LZW input loops forever; treat any non-OK scan result as failure (warning `Failed to initialize gif '<path>', possible file corruption`, no fallback to the still decoder). D-2 — an empty `.gif` calls `bmalloc(0)` and crashes; treat zero-length as failed. Q-5 — illegal LZW minimum code sizes 0, 1, 9 decode to reference-specific pixels; 12 and 13 fail. Q-7 — loop counts 65534/65535 mean forever.
  - Done when: delays equal.
- [ ] **P3-12.3 FFmpeg fallback**
  - Files: `libobs/graphics/graphics-ffmpeg.c` (787).
  - Steps: keep FFmpeg for the formats the crate does not cover; document the list.
  - Spec: S-053 §5.3.
  - Done when: list documented.
- [ ] **P3-12.4 Hash goldens**
  - Steps: pixel-hash goldens for the 30 stills + the animated fixtures.
  - Vectors: gfximages.
  - Done when: goldens committed and green.

## C. Backends

### P3-13 `wgpu` device init
- [ ] **P3-13.1 Adapter**
  - Steps: adapter selection by index/name and power preference; map to `gs_init_data`; enumerate adapters like `gs_enum_adapters`.
  - Spec: S-043 R40 (adapter enumeration).
  - Pitfalls: S-043 Q-6 — "Default" is appended after a backend that returned `false` (the callback may already have been called for real adapters).
  - Done when: adapter enumerated like `gs_enum_adapters`.
- [ ] **P3-13.2 Features/limits**
  - Steps: require BGRA and 16F; timestamps optional; log the negotiation.
  - Done when: negotiation logged.
- [ ] **P3-13.3 Swap chain X11/Wayland**
  - Files: `libobs-opengl/gl-x11-egl.c` (666), `libobs-opengl/gl-wayland-egl.c` (476) — the reference window/swap-chain behaviour to mirror.
  - Steps: create the wgpu surface from the native window handle; map the C open/resize/teardown sequence.
  - Done when: window shows output.
- [ ] **P3-13.4 Swap chain Win32/Cocoa**
  - Files: `libobs-opengl/gl-windows.c` (605), `libobs-opengl/gl-cocoa.m` (623), `libobs-d3d11/d3d11-subsystem.cpp` (3435; swap-chain handling).
  - Steps: same mapping for Win32 HWND and Cocoa NSView.
  - Done when: same on Windows and macOS.

### P3-14 `wgpu` resources
- [ ] **P3-14.1 Format map**
  - Steps: table every `gs_color_format` (A8, R8, RGBA, BGRX, BGRA, R10G10B10A2, RGBA16, R16, RGBA16F, RGBA32F, RG16F, RG32F, R16F, R32F, DXT1/3/5, R8G8, RGBA_UNORM, BGRX_UNORM, BGRA_UNORM, RG16) to a wgpu format.
  - Spec: S-043 §5.9 (R27–R28 creation, R32 NV12/P010).
  - Note: the `*_UNORM` variants mean "no sRGB conversion" (libobs does manual linear/sRGB via the `linear_srgb` flag) — do not map them to sRGB wgpu formats.
  - Done when: unsupported ones listed with fallback.
- [ ] **P3-14.2 Textures**
  - Steps: 2D/3D/cube, dynamic/render-target flags.
  - Spec: S-043 §5.9 R27–R28.
  - Done when: create/destroy test.
- [ ] **P3-14.3 Buffers/samplers**
  - Steps: vertex/index buffers, sampler states.
  - Spec: S-043 §5.9 R29 (DUP_BUFFER copy rules).
  - Done when: test.
- [ ] **P3-14.4 Coverage test**
  - Steps: draw each format to a target.
  - Done when: every mapped format draws and reads back.

### P3-15 Pipeline cache
- [ ] **P3-15.1 Key**
  - Steps: key = shader pair + blend + depth + stencil + raster + formats + topology; test the hashing.
  - Done when: hashing tested.
- [ ] **P3-15.2 LRU** — bounded size.
  - Done when: eviction test.
- [ ] **P3-15.3 Metrics** — pipelines created per frame < 1 after warm-up.
  - Done when: metrics show < 1.

### P3-16 Render and readback
- [ ] **P3-16.1 Targets/clear**
  - Steps: `gs_set_render_target`, `gs_clear`.
  - Spec: S-043 §5.9 (render-target handling, R27/R28).
  - Vectors: graphics `15-render-targets`.
  - Done when: pixel test.
- [ ] **P3-16.2 Draw**
  - Steps: submit passes lazily; batch the encoder.
  - Done when: 1000 draws/frame benchmark.
- [ ] **P3-16.3 Staging**
  - Steps: synchronous `gs_stagesurface_map` facade over async map (staging ring, poll on the graphics thread — never block the main thread, guide "Pitfalls").
  - Done when: no deadlock under single-thread use.

### P3-17 Frame pacing and presentation
- [ ] **P3-17.1 Display**
  - Files: `libobs/obs-display.c` (311).
  - Steps: port `obs_display_create/resize/destroy` rendering (the `obs_display_*` object model itself is P4-31/S-042); `render_displays` iterates the display list in order.
  - Spec: S-058 §5.5 R20 (render_displays).
  - Done when: resize without flicker.
- [ ] **P3-17.2 Multiple + callbacks**
  - Steps: draw callbacks, background colour.
  - Spec: S-058 §5.5 R20.
  - Done when: two displays at once.
- [ ] **P3-17.3 Present modes**
  - Files: `libobs-d3d11/d3d11-subsystem.cpp` (3435) — the reference present behaviour.
  - Steps: `gs_present` pacing: D3D11 legacy swap chain = interval 1 (vsync on), flip model paced by the frame-latency waitable object; `gs_init_data` has no vsync field (macOS: `DisableOSXVSync` setting).
  - Note: no spec covers the D3D11/flip-model pacing (S-057 is GL, S-058 the loop) — port by reading the C.
  - Done when: present path matches C on each OS.

### P3-18 Shared texture import: Linux
- [ ] **P3-18.1 DMA-BUF**
  - Files: `plugins/linux-pipewire/pipewire.c` (1471; the producer), `libobs/graphics/device-exports.h` (177; `gs_texture_create_from_dmabuf`).
  - Steps: import with modifiers via Vulkan external memory.
  - Spec: S-056 (linux-pipewire producer, 56 scripts); S-043 D-1 (the dmabuf functions are context-unchecked in C — add the check).
  - Vectors: pipewire (56 scripts).
  - Done when: screencast shows.
- [ ] **P3-18.2 EGL fallback**
  - Files: `libobs-opengl/gl-egl-common.c` (581) — the C `gs_texture_create_from_dmabuf` EGL path.
  - Steps: keep the EGL image fallback.
  - Done when: works on NVIDIA proprietary.
- [ ] **P3-18.3 Test** GNOME + KDE.
  - Done when: both DEs verified.

### P3-19 Shared texture import: Windows
- [ ] **P3-19.1 Open handle**
  - Files: `plugins/win-capture/graphics-hook/` (the producer), `libobs/graphics/device-exports.h` (177).
  - Steps: port `gs_texture_open_shared` / `_nt_shared` (DXGI shared handle / NT handle).
  - Spec: S-051 (win-capture logic).
  - Done when: D3D11 sample texture visible.
- [ ] **P3-19.2 Sync**
  - Steps: keyed mutex / fence.
  - Done when: no tearing in game capture.
- [ ] **P3-19.3 Test** with `plugins/win-capture/graphics-hook` samples.
  - Done when: hook samples render.

### P3-20 Shared texture import: macOS
- [ ] **P3-20.1 IOSurface**
  - Steps: port `gs_texture_create_from_iosurface`, `gs_texture_rebind_iosurface` (Metal `newTextureWithDescriptor:iosurface:plane:`).
  - Spec: S-052 (mac plugins, MODEL vectors).
  - Done when: Syphon frame visible.
- [ ] **P3-20.2 Test** with `plugins/mac-capture/mac-sck-video-capture.m`.
  - Done when: sck capture renders.

### P3-21 GPU encoder texture sharing
- [ ] **P3-21.1 Escape hatch**
  - Steps: `gs_texture_get_obj` returning the native handle plus a kind enum; document it.
  - Spec: S-043 §3.1 (backend contract — native export point).
  - Done when: documented.
- [ ] **P3-21.2 D3D**
  - Files: `libobs-d3d11/d3d11-texture2d.cpp` (381), `libobs/obs-video-gpu-encode.c` (312).
  - Steps: match `d3d11-texture2d.cpp` texture-sharing semantics so the NVENC D3D11 path (`encode_texture`/`encode_texture2`, `struct encoder_texture`) receives a handle.
  - Spec: S-058 §5.4 (GPU encode hand-off), §5.7 (GPU encode pool and thread).
  - Done when: NVENC D3D11 path receives a handle.
- [ ] **P3-21.3 CUDA/VAAPI/Metal**
  - Steps: export via external memory / `VkExternal` / `MTLTexture`.
  - Done when: one per platform proven.

### P3-22 Legacy backend adapters
- [ ] **P3-22.1 OpenGL**
  - Files: `libobs-opengl/gl-subsystem.c` (1627) + 26 more files in `libobs-opengl/` (see file map).
  - Steps: load `libobs-opengl` via the C `gs_exports` vtable (no Rust re-implementation).
  - Spec: S-057 (22 scripts).
  - Vectors: glbackend (22 scripts).
  - Pitfalls: S-057 D-1 — GL cannot create cube textures (per-face `glBindTexture`): creation returns NULL (R11). D-2 — `gs_voltexture_get_*` are stubs (return 0 / UNKNOWN, R12).
  - Done when: renders test scene.
- [ ] **P3-22.2 D3D11**
  - Files: `libobs-d3d11/d3d11-subsystem.cpp` (3435) + 15 more files in `libobs-d3d11/`.
  - Steps: same for `libobs-d3d11` via the vtable.
  - Done when: renders test scene.
- [ ] **P3-22.3 Metal**
  - Files: `libobs-metal/metal-subsystem.swift` (985) + 30 more files in `libobs-metal/` (see file map).
  - Steps: same for the Swift `libobs-metal` backend (it is part of the C ABI vtable, written in Swift; check how CMake builds it first — `libobs-metal/CMakeLists.txt`, 70).
  - Done when: renders the test scene on macOS and the build note is recorded.
- [ ] **P3-22.4 Runtime switch**
  - Steps: `--graphics wgpu|legacy` and a settings key.
  - Done when: both backends selectable at runtime.

### P3-23 HDR and colour management
- [ ] **P3-23.1 HDR targets**
  - Steps: `RGBA16F` + scRGB/HDR10 swap chains.
  - Spec: S-043 §5.9 (colour-space handling, R37 `gs_set_linear_srgb`).
  - Done when: displays on HDR monitor.
- [ ] **P3-23.2 SDR white level**
  - Files: `libobs-d3d11/d3d11-duplicator.cpp` (315) — the reference `gs_duplicator_get_sdr_white_level`.
  - Steps: backend-side `gs_is_monitor_hdr`, `gs_duplicator_get_sdr_white_level`; `obs_get_video_sdr_white_level` and `obs_get_video_hdr_nominal_peak_level` are libobs-level (Phase 4).
  - Done when: values correct on an HDR monitor.
- [ ] **P3-23.3 Visual regression** — SDR on SDR, SDR on HDR, HDR on HDR scenes.
  - Done when: visual regressions match C.

## D. Built-in effects and render loop

### P3-24 Built-in effects
- [ ] **P3-24.1 Compile**
  - Files: all 21 `libobs/data/*.effect` (incl. the nine `deinterlace_*.effect`; `format_conversion.effect` is 43.7 KB).
  - Steps: compile every file on `wgpu` through the P3-05 transpiler.
  - Spec: S-053 §5.4 R40–R53 — the normative per-effect table (params, techniques, sampler state, algorithms) for every built-in effect.
  - Vectors: gfximages (`40-effects-default-color`, `41-effects-scaling`, `42-effects-deinterlace`, `43-effects-format-conversion`).
  - Pitfalls: S-053 Q-6 — a uniform is matched by struct member name: `solid.effect` `SolidColored` lists `color` among its vertex parameters (a struct member named like a uniform).
  - Done when: all compile on `wgpu`.
- [ ] **P3-24.2 Reference render**
  - Steps: render fixed inputs with the legacy backend; store the PNGs.
  - Done when: PNGs stored.
- [ ] **P3-24.3 Compare**
  - Steps: image-diff the wgpu renders against the legacy PNGs.
  - Done when: PSNR > 60 dB (or exact for integer paths).

### P3-25 Plugin effects
- [ ] **P3-25.1 Enumerate**
  - Files: `plugins/obs-filters/data/` (18), `plugins/obs-transitions/data/` (6), `plugins/nv-filters/data/` (3), `plugins/text-freetype2/data/text_default.effect` (1) — 28 total.
  - Steps: glob `plugins/*/data/**/*.effect`; generate the list in CI.
  - Done when: list generated in CI.
- [ ] **P3-25.2 Compile test** for all.
  - Vectors: effect (S-020 corpus covers shipped effects).
  - Done when: all 28 compile.
- [ ] **P3-25.3 Fix gaps** — missing intrinsics.
  - Done when: no missing intrinsics remain; the P3-05.3 table grows accordingly.

### P3-26 Render loop
- [ ] **P3-26.1 Main render**
  - Files: `libobs/obs-video.c` (1200).
  - Steps: port `render_main_texture` (R13: render target + `render_space`, clear (0,0,0,0)) and `render_output_texture` (R14) into `crates/libobs-core` module `video_loop` (S-058 target crate), driven by a static test scene.
  - Spec: S-058 §5.3 R10–R17.
  - Vectors: obsvideo (23 scripts).
  - Done when: frame hash equal.
- [ ] **P3-26.2 Output/scale/convert**
  - Files: `libobs/obs-video.c` (1200).
  - Steps: port `render_convert_texture` (R15, only when `gpu_conversion`) and scaling with the `bilinear`/`bicubic`/`lanczos`/`area` built-in effects.
  - Spec: S-058 §5.3 R14–R15; S-053 §5.4 (the scale effects).
  - Done when: golden per scale type.
- [ ] **P3-26.3 Stage/copy**
  - Files: `libobs/obs-video.c` (1200).
  - Steps: port `stage_output_texture` (R16: unmap previous entries, stage to CPU) and `set_gpu_converted_data` (R17 `output_video_data` hand-off to `video-io`).
  - Spec: S-058 §5.3 R16–R17.
  - Pitfalls: S-058 Q-3 — a mix with a GPU consumer but `gpu_conversion == false` never sets `texture_converted`, so its gpu vframe entries are queued every frame and never consumed (unbounded growth while active); reproduce the visible behaviour (no hand-off), a cap is allowed.
  - Done when: CPU frame equals.
- [ ] **P3-26.4 GPU encode path**
  - Files: `libobs/obs-video-gpu-encode.c` (312).
  - Steps: port the GPU encode hand-off (vframe queue, semaphore, pool thread).
  - Spec: S-058 §5.4, §5.7.
  - Pitfalls: S-058 Q-2 — `texture_converted` is only reset by the *base reset* (R02); it stays true after the first conversion even if `gpu_conversion` is later cleared. Q-4 — an entry `count >= 2` on an **empty** encode queue creates two real frames (copy path, same timestamp), not one frame with count 2.
  - Done when: NVENC path unchanged.
- [ ] **P3-26.5 Deterministic golden**
  - Steps: 120 frames hashed (PNG hashes) against the C reference.
  - Vectors: obsvideo.
  - Done when: 120 frames hashed and equal.

### P3-27 Multi-canvas render support
- [ ] **P3-27.1 Mixes**
  - Files: `libobs/obs-video.c` (1200).
  - Steps: the render loop iterates `obs->video.mixes` (per-mix `render_texture`/`output_texture`, R10: each mix in array order, only when `mix.view != NULL`); the canvas/view objects themselves are P4-33/P4-31.
  - Spec: S-058 §5.3 R10–R11.
  - Done when: two fake mixes render.
- [ ] **P3-27.2 Per-mix staging**
  - Steps: per-mix `stage_output_texture` to CPU.
  - Spec: S-058 §5.3 R16.
  - Done when: each mix's frame equals its expected output.
- [ ] **P3-27.3 Test** with 2 mixes.
  - Done when: 2-mix test green.

## E. Cleanup

### P3-28 Backend selection and fallback
- [ ] **P3-28.1 Fallback** on init failure.
  - Steps: force a `wgpu` init failure; select a legacy backend.
  - Spec: S-043 R05 (creation failure paths — `GS_ERROR_MODULE_NOT_FOUND`, missing exports, `device_create` failure).
  - Done when: forced failure test.
- [ ] **P3-28.2 Logging** — adapter name, driver, features.
  - Done when: appears in log.
- [ ] **P3-28.3 Config** key and CLI.
  - Done when: both honoured.

### P3-29 Performance gate
- [ ] **P3-29.1 Scenes**
  - Steps: three reference scenes per OS (simple, 20 sources with filters, 4K); store in `test/fixtures/`.
  - Done when: stored in `test/fixtures`.
- [ ] **P3-29.2 Measure** GPU time both backends.
  - Done when: numbers recorded per scene/OS.
- [ ] **P3-29.3 Gate** at 5%.
  - Done when: within 5% on all scenes, reported.

### P3-30 Remove `deps/glad`
- [ ] **P3-30.1 Users**
  - Files: `deps/glad/` (in the checkout).
  - Steps: grep `glad` across the tree.
  - Done when: only `libobs-opengl`.
- [ ] **P3-30.2 Delete** when legacy GL is gone (Phase 7).
  - Done when: deleted in Phase 7, GL backend still builds.

### P3-31 Phase 3 review
- [ ] **P3-31.1 Review** visual diffs and perf.
  - Done when: review notes committed.
- [ ] **P3-31.2 Tick** parents.
  - Done when: parent tasks ticked in `../phase-3-graphics.md`.

### P3-32 Graphics backend spec coverage (added)
- [ ] **P3-32.1 Write S-078** — D3D11 backend (13 files / 272 fns: `libobs-d3d11/*`)
  - Steps: 1. write `specs/S-078-d3d11-backend.md`: gs_* → ID3D11 device mapping, texture/vertex/index buffer lifecycles, shader compile + processing, rebuild rules, duplicator interop (`d3d11-subsystem.cpp` 156 fns, `d3d11-rebuild.cpp` 20). 2. add the row to `specs/README.md`.
  - Done when: S-078 committed; `libobs-d3d11/` fully cited.
- [ ] **P3-32.2 Write S-079** — Metal backend (21 files / 184 fns: `libobs-metal/*`)
  - Steps: 1. write `specs/S-079-metal-backend.md` (Swift: device/swapchain/texture/buffer/shader semantics, MTL → gs_* mapping; `metal-subsystem.swift` 38, `OBSShader.swift` 20, `MetalDevice.swift` 16). 2. add the row.
  - Done when: S-079 committed; `libobs-metal/` fully cited.
- [ ] **P3-32.3 Extend S-057** — OpenGL backend file citations (11 files / 212 fns: `libobs-opengl/*`)
  - Steps: 1. cite the remaining files (`gl-subsystem.c` 85, `gl-x11-egl.c` 31, `gl-egl-common.c` 19, …) and add rules for the uncited helpers.
  - Done when: `libobs-opengl/` fully cited.
