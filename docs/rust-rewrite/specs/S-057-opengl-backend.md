# S-057 OpenGL graphics backend (libobs-opengl)

| | |
|---|---|
| Status | Ready for review (Rust sufficiency check skipped by decision, see 10) |
| Reference | libobs 33.0.1 `libobs-opengl/` (`gl-subsystem.c/.h`, `gl-helpers.*`, `gl-texture2d/3d/cube.c`, `gl-stagesurf.c`, `gl-zstencil.c`, `gl-vertexbuffer.c`, `gl-indexbuffer.c`, `gl-shader.c`, `gl-shaderparser.*`, `gl-nix.*`, `gl-x11-egl.*`, `gl-egl-common.*`) |
| Target crate | `crates/obs-gl-backend` (module `device`, `texture`, `shader`, `state`) |
| Depends on | S-043 (graphics API above the backend), S-020 (effect language), S-008 (matrices) |
| Conformance | `test/conformance/scripts/glbackend/` (22 scripts), REAL C run on a REAL (software) OpenGL context |

## 1. Scope

The loadable graphics backend `libobs-opengl`: a module that exports ~176 `device_*` / `gs_*`
functions (the "gs_exports" table that `graphics.c`, S-043, calls through). This spec covers the
**observable behaviour of those exports**: device lifecycle, 2D / cube / volume textures, stage
surfaces (read-back), render targets and z-stencil buffers, blend / depth / stencil / cull /
scissor / viewport state, vertex and index buffers, shaders (OBS-HLSL text to GLSL, see S-020 for
the language), uniform upload and sampler handling. What this spec does NOT repeat: the effect
language and the generic `gs_*` wrappers (S-020, S-043).

**Not covered** (platform glue or hardware features that cannot be checked headless on this
host): swap chains and `present` (`device_swapchain_create/resize/load_swapchain`, needs a real X
window), Wayland (`gl-wayland-egl.c`), Windows WGL (`gl-windows.c`), macOS (`gl-cocoa.m`),
dma-buf / pixmap import (`device_texture_create_from_dmabuf/pixmap`, `device_query_dmabuf_*`),
GPU sync objects (`device_sync_*`), GPU timers (`gs_timer_*`), debug markers, HDR/colour-space
queries, `device_get_device_obj`, NV12/P010 planar textures, cube-map rendering (cube creation
fails, D-1), 3D textures beyond creation, and compressed-texture contents. These stay "open"
(see 11).

## 2. Terminology and conventions

* **Device** = `gs_device_t`, one GL context. Every call must be made between
  `device_enter_context` and `device_leave_context` (the reference does not check).
* A **stage surface** is a pixel pack buffer; "read-back" = `device_stage_texture` + `gs_stagesurface_map`.
* **Row 0** of a read-back is the first row in GL memory. When drawing into a render target the
  backend flips the projection (see S-057-R30) so row 0 is the *top* of a picture drawn with a
  y-down orthographic projection (`ortho(0, W, 0, H)`), as in Direct3D.
* Colours below are written as bytes `RRGGBBAA`.
* Driver: vectors were produced with **Mesa llvmpipe 26.2 (software), OpenGL 4.6 core profile**,
  in a private Xvfb. Results that rely on rasterisation precision are marked *(driver-dep.)*.
  The runner forces Mesa software rendering and its own Xvfb so that the user's GPU is never used.
* Logging: only ERROR and WARNING lines are part of the vectors (`log error ...`). Text that
  contains driver output or GL error numbers other than those shown is not.

## 3. Interface

The behaviour is defined per exported function; pseudo-signatures use Rust-like types.

### 3.1 Device

| Function | Behaviour |
|---|---|
| `device_create(adapter) -> (code, device)` | creates an EGL context on a windowless X11 display (`XOpenDisplay(NULL)` unless the host supplied one), makes it current, loads GL 3.3+ core, creates an empty VAO and a *raw load* sampler (point filter, border addressing), then releases the context (`gl_clear_context`). Returns `GS_SUCCESS` (0). On failure returns `GS_ERROR_FAIL` or `GS_ERROR_NOT_SUPPORTED` (missing required extension) and a NULL device. |
| `device_get_name()` | `"OpenGL"` |
| `device_get_type()` | `GS_DEVICE_OPENGL` = 1 |
| `device_preprocessor_name()` | `"_OPENGL"` |
| `device_enum_adapters(cb)` | calls back for each GPU found by the platform glue; returns true. The number and names are driver/host dependent. |
| `device_enter_context` / `leave` | make current / release. |
| `device_destroy` | destroys all cached GL programs, the raw load sampler, the VAO and the context. |
| `device_get_size/width/height` | size of the current swap chain; with none: logs **warning** `device_get_size (GL): No active swap` (`..._width` / `..._height` for the other two) and returns 0. |

### 3.2 Objects

`device_texture_create(w, h, fmt, levels, data[], flags)`, `device_cubetexture_create`,
`device_voltexture_create`, `device_zstencil_create(w, h, fmt)`, `device_stagesurface_create(w, h, fmt)`,
`device_samplerstate_create(info)`, `device_vertexbuffer_create(vbdata, flags)`,
`device_indexbuffer_create(type, indices, num, flags)`, `device_vertexshader_create(text, file, &err)`,
`device_pixelshader_create(...)`. Each returns NULL on failure after logging the error lines given below.
Flags: `BUILD_MIPMAPS=1`, `DYNAMIC=2`, `RENDER_TARGET=4`, `GL_DUMMYTEX=8`.

### 3.3 State and drawing

`device_set_render_target[_with_color_space]`, `device_set_cube_render_target`, `device_clear`,
`device_set_viewport`, `device_set_scissor_rect`, `device_ortho`, `device_frustum`,
`device_projection_push/pop`, `device_set_cull_mode`, `device_enable_blending/depth_test/stencil_test/
stencil_write/color`, `device_blend_function[_separate]`, `device_blend_op`, `device_depth_function`,
`device_stencil_function/op`, `device_enable_framebuffer_srgb`, `device_load_vertexbuffer/indexbuffer/
texture/samplerstate/vertexshader/pixelshader`, `device_draw`, `device_copy_texture[_region]`,
`device_stage_texture`, `device_flush`. Shader parameters: `gs_shader_get_num_params/param_by_idx/
param_by_name/param_info/viewproj_matrix/world_matrix`, `gs_shader_set_bool/float/int/vec2/vec3/vec4/
matrix3/matrix4/texture/val/default/next_sampler`.

The backend also **calls back** into libobs: `gs_matrix_get(&view)` (current view matrix, once per
draw), `gs_get_effect()` / `gs_effect_update_params(effect)` (before each draw if an effect is
active), `obs_get_nix_platform()` / `obs_get_nix_platform_display()`. A Rust implementation
needs the same hooks; the conformance runner stubs them (identity view matrix, no effect, X11/EGL).

## 4. Data formats

### 4.1 Colour formats

| `gs_color_format` | bytes/pixel | GL internal | GL upload/readback format, type |
|---|---|---|---|
| A8 | 1 | R8 (swizzled to alpha when sampled) | RED, UNSIGNED_BYTE |
| R8 | 1 | R8 | RED, UNSIGNED_BYTE |
| RGBA | 4 | SRGB8_ALPHA8 | RGBA, UNSIGNED_BYTE |
| BGRA | 4 | SRGB8_ALPHA8 | BGRA, UNSIGNED_BYTE |
| BGRX | 4 | SRGB8 | BGRA, UNSIGNED_BYTE |
| RGBA_UNORM, BGRA_UNORM, BGRX_UNORM | 4 | RGBA8 / RGBA8 / RGB8 | as above |
| R10G10B10A2 | 4 | RGB10_A2 | RGBA, UNSIGNED_INT_2_10_10_REV |
| R8G8 | 2 | RG8 | RG, UNSIGNED_BYTE |
| R16, RG16, RGBA16 | 2,4,8 | R16, RG16, RGBA16 | RED/RG/RGBA, UNSIGNED_SHORT |
| R16F, RG16F, RGBA16F | 2,4,8 | R16F, RG16F, RGBA16F | HALF_FLOAT |
| R32F, RG32F, RGBA32F | 4,8,16 | R32F, RG32F, RGBA32F | FLOAT |
| DXT1/3/5 | block compressed | S3TC | compressed upload (4x4 blocks: 8/16/16 bytes) |
| UNKNOWN or out of range | | | creation fails (see R04) |

Because the 8-bit RGB formats use sRGB internal formats and `GL_FRAMEBUFFER_SRGB` is disabled by
default, **bytes written and read back are not converted**: what you upload is what `read` returns.
Sampling uses `GL_TEXTURE_SRGB_DECODE_EXT` = skip-decode for `device_load_texture` and decode for
`device_load_texture_srgb`.

### 4.2 Pixel row layout (alignment)

GL's default pixel-store alignment of **4 bytes** applies to uploads and read-backs (the backend
never changes it). Worked example, R8 3x3, upload data (memory order) `01 02 03 00 04 05 06 00 07 08 09 00`;
read-back returns the same 12 bytes but `gs_stagesurface_map` reports `linesize = 3`, so a reader
using linesize sees rows `010203`, `000405`, `060007` (Q-1).

### 4.3 Vertex buffer data (`gs_vb_data`)

`num` vertices; `points` (vec3 = 16 bytes in memory!), optional `normals`, `tangents`, `colors`
(u32, bytes R,G,B,A in memory), `num_tex` texcoord arrays with `width` floats per vertex.
Shader inputs map by semantic: `POSITION`, `NORMAL`, `TANGENT`, `COLOR`, `TEXCOORDn`.

## 5. Behaviour (numbered rules)

**Device**

* **S-057-R01** `device_create` returns 0 with a usable device when a GL >= 3.3 core context can be
  created; the device is *not* current afterwards (call `enter` first). Initialisation logs INFO lines (driver strings; not normative).
* **S-057-R02** Identity getters: name `OpenGL`, type 1, preprocessor `_OPENGL`.
* **S-057-R03** `device_get_size/width/height` without a swap chain return 0 and log one WARNING each, see 3.1.

**Textures**

* **S-057-R04** `device_texture_create` succeeds for the formats of 4.1 and returns a 2D texture
  whose `get_width/height/color_format` echo the arguments, `get_texture_type` = `GS_TEXTURE_2D`, `is_rect` = false.
  Format `GS_UNKNOWN` fails: logs `glTexImage2D failed, glGetError returned GL_INVALID_VALUE(0x501)` then
  `device_texture_create (GL) failed`, returns NULL. A 0x0 texture is created successfully.
* **S-057-R05** `levels = 0` allocates the full mip chain (`floor(log2(max(w,h)))+1` levels),
  otherwise `levels` levels; `data` is an array of pointers, one per level (NULL data allocates
  without initialising; contents then undefined and not testable). `BUILD_MIPMAPS` only records the flag.
* **S-057-R06** Level-0 data is read with GL's 4-byte row alignment (4.2).
* **S-057-R07** Stage surface read-back returns exactly the uploaded bytes (no sRGB conversion,
  no byte swizzling) for every format in 4.1 except compressed ones.
* **S-057-R08** `gs_texture_map` / `unmap` only work for `DYNAMIC` textures: map returns a pointer to a
  pixel unpack buffer and `linesize = align4(w * bpp)`; `unmap` uploads it as level 0.
  On a non-dynamic texture: logs `Texture is not dynamic`, `gs_texture_map (GL) failed`, returns false.
* **S-057-R09** `GL_DUMMYTEX` textures have no storage upload and `MAX_LEVEL = 0`.
* **S-057-R10** Calling a 2D-only accessor on a 3D/cube texture logs `<fn> (GL) failed:  Not a 2D texture`
  and returns 0 / false.
* **S-057-R11** (D-1) Cube texture creation always fails on a conforming core GL: 12 x
  `glBindTexture failed, glGetError returned GL_INVALID_ENUM(0x500)` followed by
  `device_cubetexture_create (GL) failed`; NULL. The Rust port MUST report failure the same way
  (a working implementation is a possible improvement but then the vectors change).
* **S-057-R12** Volume textures are created (`device_voltexture_create` ok) but all getters are stubs (D-2):
  width, height, depth = 0, colour format = `GS_UNKNOWN`; `get_texture_type` = `GS_TEXTURE_3D`.
* **S-057-R13** `device_zstencil_create` succeeds for `Z16`, `Z24_S8`, `Z32F`, `Z32F_S8X24`; `GS_ZS_NONE`
  fails with `glRenderbufferStorage failed, glGetError returned GL_INVALID_ENUM(0x500)` + `device_zstencil_create (GL) failed`.

**Stage surfaces and copies**

* **S-057-R14** `device_stage_texture(dst, src)` requires same colour format (exact enum equality) and same
  size; otherwise logs `Source and destination formats do not match` / `Source and destination must have the same dimensions` then `device_stage_texture (GL) failed`, leaves dst unchanged. `gs_stagesurface_map` reports `linesize = width * bpp`.
* **S-057-R15** `device_copy_texture_region(dst, dx, dy, src, sx, sy, w, h)`: `w == 0` means "to the right
  edge of the source", `h == 0` "to the bottom". Errors (each followed by `device_copy_texture (GL) failed`):
  `Source texture is NULL`, `Destination texture is NULL`, `Source and destination textures must be 2D textures`,
  `Source and destination formats do not match` (comparison after `gs_generalize_format`; RGBA vs BGRA are different, BGRA vs BGRX differ too in practice),
  `Destination texture region is not big enough to hold the source region`.
  `device_copy_texture` = region copy of the whole source at (0,0).

**Render targets and clear**

* **S-057-R16** `device_set_render_target(tex|NULL, zs|NULL)` requires a 2D texture created with `RENDER_TARGET`;
  otherwise logs `Texture is not a render target` (or `Texture is not a 2D texture`) then
  `device_set_render_target (GL) failed` and leaves the target unchanged. `NULL` selects the default framebuffer.
  `get_render_target` / `get_zstencil_target` return what was set.
* **S-057-R17** `device_clear(flags, colour, depth, stencil)` clears only the buffers named by
  `CLEAR_COLOR=1 / CLEAR_DEPTH=2 / CLEAR_STENCIL=4`, honouring the current scissor, colour mask
  and stencil write mask (Q-2: `stencil_write off` also prevents clearing stencil).
* **S-057-R18** `device_set_viewport(x, y, w, h)` stores the rectangle as given (`get_viewport` echoes it, no validation);
  for the default framebuffer y is converted to bottom-up, for render targets it is passed unchanged.
* **S-057-R19** `device_set_scissor_rect(rect)` enables GL scissor with the rectangle passed unchanged
  (origin: first memory row); `NULL` disables it.

**Drawing**

* **S-057-R20** `device_draw(mode, start, num)` precondition checks in this order, each logging an ERROR and
  then `device_draw (GL) failed` without drawing: `No vertex shader specified`, `No pixel shader specified`,
  `No vertex buffer specified` (only when no vertex buffer and `num == 0`), `No active swap chain or render target`.
  Drawing without a vertex buffer but with `num > 0` is allowed (an empty VAO is bound).
* **S-057-R21** With an index buffer: `start` is an index offset, `num == 0` means all indices; without: `start`/`num` count
  vertices, `num == 0` means all vertices of the buffer. Modes: POINTS, LINES, LINESTRIP, TRIS, TRISTRIP.
* **S-057-R22** Before each draw: `gs_matrix_get(view)` is called; `ViewProj = transpose(view * proj')` where
  `proj'` is the current projection with its y-row negated when a render target (FBO) is bound, and is uploaded to the vertex shader's `ViewProj` parameter if it exists; the front-face winding is clockwise for FBO targets and counter-clockwise for the default framebuffer. All uniforms are then uploaded (R40).
* **S-057-R23** `device_ortho(l, r, t, b, n, f)` builds: `x.x = 2/(r-l)`, `t.x = -(l+r)/(r-l)`, `y.y = 2/(t-b)` (note: `-1/(b-t)` form), `t.y = (b+t)/(b-t)`,
  `z.z = -2/(f-n)`, `t.z = -(f+n)/(f-n)`, `t.w = 1`, all else 0 (a y-down projection when `t < b`).
  `device_frustum` is the usual asymmetric perspective matrix (`z.w = -1`). `projection_push/pop` is a stack; pop on an empty stack is ignored.
* **S-057-R24** Culling: the default is `GS_BACK`. In the y-down space of R23, `GS_BACK` keeps triangles that are
  counter-clockwise (they are "front"), `GS_FRONT` keeps clockwise ones, `GS_NEITHER` keeps both; `get_cull_mode` echoes.
* **S-057-R25** Blend function/op: direct mapping to `glBlendFunc[Separate]` / `glBlendEquation`
  (`BLEND_ZERO..SRCALPHASAT`, `OP_ADD, SUBTRACT, REVERSE_SUBTRACT, MIN, MAX`). `device_enable_blending(false)` replaces the destination.
* **S-057-R26** Depth: `enable_depth_test`, `depth_function` map to GL; the depth buffer is a render-target z-stencil renderbuffer.
* **S-057-R27** Stencil: `stencil_function(side, test)` uses reference 0 and mask `0xFFFFFFFF`; `stencil_op(side, fail, zfail, zpass)`; `enable_stencil_write(false)` = `glStencilMask(0)`.
* **S-057-R28** `device_enable_color(r,g,b,a)` is `glColorMask`.
* **S-057-R29** `device_enable_framebuffer_srgb(on)` toggles `GL_FRAMEBUFFER_SRGB`; the getter reflects it (default off).
* **S-057-R30** Rendering into a texture is vertically flipped relative to GL's window convention so that a y-down
  projection yields row 0 = top (see 2); scissor/viewport are not flipped.

**Samplers**

* **S-057-R31** Filters map to GL min/mag filters (`GS_FILTER_POINT` = nearest/nearest-mipmap-nearest, `LINEAR` = linear, `ANISOTROPIC` = linear + anisotropy, mixed modes accordingly); address modes CLAMP -> clamp-to-edge, WRAP -> repeat, MIRROR -> mirrored repeat, BORDER -> clamp-to-border, MIRRORONCE -> mirror-clamp.
* **S-057-R32** (D-4) The border colour is converted from the sampler info only on the path that corrects
  `max_anisotropy` (when it is < 1 or > the driver maximum). With a valid anisotropy (e.g. 1) the border
  colour stays transparent black `(0,0,0,0)`.
* **S-057-R33** A shader's `sampler_state` declaration only establishes the sampler *slot*. The sampler applied at draw
  time is the one handed to the texture parameter with `gs_shader_set_next_sampler`, otherwise the device's *raw load* sampler (point, border).
* **S-057-R34** (Q-3) `device_load_texture(tex, unit)` returns immediately when `tex` is already recorded for `unit`.
  `device_stage_texture` (and other internal binds) unbind GL's current texture without updating that record, so a texture
  loaded before a read-back is silently *unbound* afterwards and samples black until a different texture is loaded on that unit.

**Shaders and parameters**

* **S-057-R35** A shader text must contain a function called `main` (the entry point; S-020's compiler renames
  the technique entry to `main`). Missing: logs `function 'main' not found`; parse errors log a WARNING
  `Shader parser errors/warnings: <file> (<line>, <col>): <message>`; GLSL compile errors log
  `Error compiling shader:` (+ driver text, not normative); each is followed by `device_<vertex|pixel>shader_create (GL) failed`,
  NULL, and `*error_string` is set non-empty for GLSL compile failures. An empty text fails too.
* **S-057-R36** Every global `uniform` of the text becomes a parameter, in declaration order, with type
  `float` -> FLOAT, `int` -> INT, `bool` -> BOOL, `float2/3/4` -> VEC2/3/4, `int2/3/4` -> INT2/3/4,
  `float4x4` -> MATRIX4X4, `texture2d/3d/cube` -> TEXTURE; arrays are listed once with the element type
  (`float4 arr[2]` -> VEC4). `ViewProj` / `World` (float4x4) are additionally returned by `gs_shader_get_viewproj_matrix` / `world_matrix`. Initialisers in the text are the default values and the initial value.
* **S-057-R37** `gs_shader_set_float/int/bool/vec2/vec4/matrix4/texture` store the value; `bool` is stored as `int` (any non-zero -> 1);
  `set_matrix3` widens to a matrix4 (`matrix4_from_matrix3`, last column `0,0,0,1`); `set_default` restores the default.
* **S-057-R38** `gs_shader_set_val(param, bytes, size)` requires `size = element size * max(array_count,1)` where element sizes are:
  float 4, bool/int 4, int2 8, int3 12, int4 16, vec2 8, vec3 12, vec4 16, matrix4 64; otherwise logs
  `gs_shader_set_val (GL): Size of shader param does not match the size of the input` and changes nothing.
* **S-057-R39** (D-5) `gs_shader_set_vec3` stores 16 bytes (the padded `struct vec3`), which can never match the 12 bytes the
  upload expects: the draw logs `Parameter '<name>' set to invalid size 16, expected 12` and the uniform keeps its prior value.
  The same check (`Parameter '<name>' set to invalid size N, expected M`) applies to all types; only matching sizes are uploaded.
* **S-057-R40** At draw time each parameter present in the linked program is uploaded: ints/bools with `glUniform1iv`,
  floats/vecs with `glUniform{1,2,3,4}fv` (count 1: **array parameters upload only element 0**), matrices with `glUniformMatrix4fv(transpose=false)`,
  textures bind the texture to its unit with the sampler of R33. In a pixel shader `mul(v, m)` evaluates `channel c = dot(stored row c of m, v)` (rows `x,y,z,t` as passed to `set_matrix4`).
* **S-057-R41** A vertex buffer that lacks an input required by the vertex shader logs
  `Vertex buffer does not have the required inputs for vertex shader`; the draw proceeds with GL's default attribute value.
* **S-057-R42** Vertex colour is read as 4 normalised bytes (R,G,B,A in memory order); texcoords as `width` floats.
* **S-057-R43** `device_indexbuffer_create` reports `num` and type through `gs_indexbuffer_get_num_indices/type`.

## 6. Algorithms

The backend's only algorithmic piece is the OBS-HLSL to GLSL translation (`gl-shaderparser.c`, ~770
lines). Its observable contract is R35/R36/R40 plus S-020; the generated GLSL text is not
normative (driver compile errors differ), only whether it compiles and what it computes. Texture
layout: `row_size = width * bpp / 8; upload offset of row r = r * align4(row_size)` (R06).

## 7. Concurrency

One GL context per device; the context is current on at most one thread (`enter/leave`). The
reference takes no locks; callers (S-043) serialise. Deleting objects needs the context current.

## 8. Reference defects and quirks

| ID | Class | Description | Required behaviour |
|---|---|---|---|
| D-1 | DEFECT | cube textures cannot be created (per-face `glBindTexture`) | creation returns NULL as in R11 |
| D-2 | DEFECT | `gs_voltexture_get_width/height/depth/color_format` are stubs | return 0 / UNKNOWN (R12) |
| D-4 | DEFECT | border colour ignored for in-range anisotropy | R32 |
| D-5 | DEFECT | `gs_shader_set_vec3` can never upload | R39; a port may fix this but then the vector `17_shader_uniforms` changes |
| D-6 | DEFECT | array uniforms upload only element 0 | R40 (not covered by a vector, read from source) |
| Q-1 | QUIRK | 4-byte row alignment for odd R8/A8 widths in upload and read-back, `linesize` unpadded | R06 / 4.2 (`04_texture_quirks`) |
| Q-2 | QUIRK | stencil clear is masked by `stencil_write off` | R17 (`13_stencil`) |
| Q-3 | QUIRK | stale texture record after read-back | R34 (`15_texture_binding_quirk`) |
| Q-4 | QUIRK | `device_load_samplerstate` applies GL sampler parameters to whatever texture is bound; the intended path is `set_next_sampler` | use R33 |
| Q-5 | QUIRK | `device_get_size` etc. warn every call without swap chain | R03 |

## 9. Implementation notes (non-normative)

* A Rust port can use `glow`/`glutin` or raw EGL; the context must be a core 3.3+ context. Test the
  port against the same scripts under Xvfb + Mesa llvmpipe. Prefer to keep sRGB internal formats (R07).
* The raw-byte read-back vectors (formats, copies, clears, blending with 0/1 values, depth/stencil
  outcomes, 4x4 sampling) are exact on any conforming driver. Interpolated attributes (`20_vertex_index_buffers`
  uv ramps) may differ by 1 LSB between drivers; line/point rasterisation was chosen away from pixel boundaries.
* `99_driver_dependent` contains values that legitimately vary between drivers.

## 10. Conformance coverage

Runner: `test/conformance/gen/glbackend_runner.c` (+ `glbackend_stubs.c`, `glbackend.sources`) links the real
`libobs-opengl` X11/EGL backend (14 files), `deps/glad`, `shader-parser.c`, the math files and the cf-lexer, and drives
the `device_*`/`gs_*` exports directly (`graphics.c` is not linked: the backend itself defines the `gs_*` symbols). It starts a private
`Xvfb` (`-displayfd`) and forces Mesa software rendering. **Rust sufficiency check: not performed** (cost control, by decision of the caller).
Sanitizer run (`-fsanitize=address,undefined`, leak check off because Mesa/X leave allocations): clean for all 22 scripts.

| Script | Rules / defects |
|---|---|
| 01_device | R01-R03 |
| 02_texture_formats | R04, R07 |
| 03_texture_info_errors | R04, R05, R08-R10, R12 |
| 04_texture_quirks | R06, Q-1 |
| 05_stage_copy | R14, R15 |
| 06_cube_vol_zstencil | R11-R13, D-1, D-2 |
| 07_render_target_clear | R16-R19 |
| 08_draw_basic | R21-R23, R36 (defaults), R37, R40 |
| 09_draw_errors | R20 |
| 10_cull_mode | R24 |
| 11_blend | R25, R28 (blend only) |
| 12_depth | R26 |
| 13_stencil | R27, Q-2 |
| 14_sampling | R31-R33, D-4 |
| 15_texture_binding_quirk | R34, Q-3 |
| 16_shader_params | R36 |
| 17_shader_uniforms | R37-R40, D-5 |
| 18_shader_vec3_raw | R38 |
| 19_shader_errors | R35 |
| 20_vertex_index_buffers | R21, R41-R43 |
| 21_state_queries | R18, R24, R29 |
| 99_driver_dependent | capability queries, linear filtering (informative) |

Not covered by any script: R05 mip chain contents, R09, R40 array/INT3 uploads, R40 `World`, R30 default framebuffer,
D-6, Q-4, `device_resize`, swap chains, and everything listed under "Not covered" in 1.

## 11. Open questions

* **OQ-1** With Mesa llvmpipe, in a script that does `draw, read-back, draw` with a pixel shader that has several
  float vector uniforms, the second draw showed one component of a vector uniform as 0 even though the uniform was
  unchanged (reproduced on `float4 v4; float4 w4;`, component `w4.y`). Scalars, ints and matrices were unaffected, and a draw
  without an intervening read-back was correct. Unexplained: the cause may be the backend (state clobbered by the stage-surface read) or the driver. Vectors avoid the situation (float vectors only in the first draw of a script).
* **OQ-2** Behaviour of swap chains / `present` / `device_resize` could not be tested without a real X window.
* **OQ-3** `device_enum_adapters` returned 2 callbacks on this host; the rule for how many is host dependent.
* **OQ-4** Wayland/WGL/Cocoa paths and dma-buf / sync-object / timer queries are untested.
