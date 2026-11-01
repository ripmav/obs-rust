# S-043 Graphics API frontend (gs_* above the backend)

| | |
|---|---|
| Status | Ready (Rust sufficiency check skipped, see 11) |
| Reference | C implementation, libobs 33.0.1: graphics.c, texture-render.c, graphics-imports.c, header-inline helpers of graphics.h |
| Target crate | `crates/libobs-graphics` (frontend only; backends implement the `GsExports` trait) |
| Depends on | S-008 (matrix/vec/quat math), S-020 (effect language; effect creation is only touched here for file caching) |
| Conformance | `test/conformance/scripts/graphics/` (31 scripts, runner `gen/graphics_runner.c`) |

## 1. Scope
The `gs_*` API that plugins and libobs call. It is a thin, validating layer over a **backend** (OpenGL, D3D11, Metal),
reached through a table of `device_*` / `gs_*` functions (`gs_exports`). In scope: context enter/leave and
thread-locality, the matrix stack, viewport stack, blend-state stack, immediate and retained vertex building, sprite
drawing, texture creation sanitising, NULL/no-context handling, the `texrender` helpers, shader-parameter wrappers,
the pass-through functions, and the header-inline helpers. Out of scope: backends, the effect language (S-020), image
decoding (`gs_texture_create_from_file` only forwards decoded data), Windows/macOS-only wrappers (listed in 3.6).

## 2. Conventions
`ctx` = the graphics object current on the calling thread. "Backend call" = call into the export table. Floats are
printed `%.9g` in vectors; comparison tolerance 1e-6 relative (absolute 1e-6 near zero), and `-0` equals `0`.
Enumerator names (`GS_RGBA`...) are the numeric values of graphics.h (the vector files print numbers for results).

## 3. Interface
### 3.1 Backend contract
The backend provides every function of `gs_exports` for the platform. **Mandatory** exports: all except the optional
ones. **Optional** (may be absent, frontend supplies the default noted): `gpu_get_driver_version`, `gpu_get_renderer`
(NULL), `gpu_get_dmem`, `gpu_get_smem` (0), `device_enum_adapters` (see R40), `gs_texture_is_rect` (false),
`device_nv12_available`, `device_p010_available` (false), `device_texture_create_nv12/p010` (R32), `gs_get_adapter_count` (0).
The device handle is opaque to the frontend. The frontend passes the device as first argument of all `device_*` calls.
### 3.2 Types used
`struct gs_rect {int x,y,cx,cy}`; `struct gs_vb_data {size_t num; vec3 *points,*normals,*tangents; u32 *colors; size_t num_tex; gs_tvertarray *tvarray}`
with `gs_tvertarray {size_t width; void *array}`; `gs_sampler_info {filter,address_u,address_v,address_w,int max_anisotropy,u32 border_color}`;
`gs_init_data {window, cx, cy, num_backbuffers, format, zsformat, adapter}`; `blend_state {enabled, src_c,dest_c,src_a,dest_a, op}`.
Enums are those of graphics.h; relevant numeric values: GS_RGBA=3, GS_BGRA=5, GS_BUILD_MIPMAPS=1, GS_DYNAMIC=2,
GS_RENDER_TARGET=4, GS_DUP_BUFFER=16, GS_FLIP_U=1, GS_FLIP_V=2, draw modes POINTS..TRISTRIP=0..4, GS_TEXTURE_2D/3D/CUBE=0/1/2,
error codes GS_SUCCESS=0, GS_ERROR_FAIL=-1, GS_ERROR_MODULE_NOT_FOUND=-2, GS_ERROR_NOT_SUPPORTED=-3.
### 3.3 Graphics object state (per object)
matrix stack (initially one identity), `cur_matrix` index, viewport stack (empty), blend-state stack (empty), current blend
mirror, `linear_srgb` flag (false), the 4 frontend vertex buffers (immediate `IMM`, `sprite`, `subregion`, `flipped`), immediate arrays,
effect cache list, a mutex, a use count.
### 3.4 Functions
All `gs_*` of graphics.h not listed in 3.6. They fall in the groups of section 5.
### 3.5 Header-inline helpers (plugins compile these)
`gs_vbdata_create` (zeroed struct) / `gs_vbdata_destroy` (frees the arrays, tvarray[i].array, tvarray and the struct; NULL ok);
`gs_get_format_bpp`: A8,R8,DXT3,DXT5=8; R16,R16F,R8G8=16; RGBA,BGRX,BGRA,R10G10B10A2,RG16F,R32F,RGBA_UNORM,BGRX_UNORM,BGRA_UNORM,RG16=32;
RGBA16,RGBA16F,RG32F=64; RGBA32F=128; DXT1=4; UNKNOWN and anything else=0.
`gs_is_compressed_format` (DXT1/3/5); `gs_is_srgb_format` (RGBA,BGRX,BGRA only); `gs_generalize_format` (_UNORM variants -> RGBA/BGRX/BGRA, else identity);
`gs_get_format_from_space`: SRGB->RGBA, the other three spaces -> RGBA16F; `gs_get_total_levels(w,h,d)`: m=max(w,h,d), n=1, while m>1 {m/=2; n++}, return n.
### 3.6 Platform wrappers not covered by vectors
Windows (GDI, duplicator, shared handles, NV12/P010 stage surfaces, loss callbacks) and macOS (IOSurface) wrappers: each checks
context and optional-export presence and returns NULL/false/-1/GS_INVALID_HANDLE/80.0f(duplicator white level)/GS_CS_SRGB; behaviour read from source, untested here.

## 4. Data formats
Immediate vertex buffer: fixed capacity 512 vertices: one vec3 points[512], vec3 normals[512], u32 colors[512] (pre-filled 0xFFFFFFFF), num_tex=1 with one vec2 array[512]; created dynamic (flags GS_DYNAMIC) at init.
Sprite vertex data (4 vertices, tristrip order): points (0,0,0),(1,0,0),(0,1,0),(1,1,0); `sprite` uv = (0,0),(1,0),(0,1),(1,1); `flipped` uv = (0,1),(1,1),(0,0),(1,0). `sprite` created with GS_DUP_BUFFER, `subregion` with GS_DUP_BUFFER|GS_DYNAMIC, `flipped` flags 0. Creation order: IMM, sprite, subregion, flipped (vectors 01, 08).

## 5. Behaviour
### 5.1 Validation conventions
- S-043-R01 Functions marked "ctx-checked" return early when no context is current, logging at DEBUG `<fn>: called while not in a graphics context`. Defaults: bool false, int/uint 0, pointers NULL, `gs_get_device_type` -1, `gs_get_color_space` SRGB(0), `gs_get_texture_type` 2D(0), cull mode NEITHER, strings NULL.
- S-043-R02 Pointer-parameter checks log at DEBUG `<fn>: Null '<param>' parameter` (param is the C identifier) and return the default. Which parameters are checked per function: vectors 11, 18, 26. The destroy functions and unmap return silently for NULL where noted by vectors (destroy of NULL: no log, no backend call; texture/stagesurface unmap NULL logs).
- S-043-R03 Functions without NULL check forward NULL to the backend: gs_stage_texture, gs_timer_range_get_data, gs_load_* (NULL means unbind), gs_set_scissor_rect(NULL) (disable), gs_clear(color NULL), gs_set_render_target*, gs_copy_texture_region(src NULL) (only dst checked), gs_debug_marker_*.
- S-043-R04 `gs_copy_texture` requires both textures; `gs_copy_texture_region` only dst.
### 5.2 Creation and destruction
- S-043-R05 `gs_create(&g, module, adapter)`: open module (failure -> GS_ERROR_MODULE_NOT_FOUND, `*g` untouched), resolve imports (every missing mandatory export logs ERROR `Could not load function '<name>' from module '<module>'` and result is GS_ERROR_FAIL), call `device_create(&dev, adapter)`; a non-zero result is returned as is. Then init: push identity matrix; enter backend context (no frontend lock); set thread context = g; create IMM, sprite, subregion, flipped (in that order; any failure -> GS_ERROR_FAIL); create mutex objects; program blend: `device_blend_function_separate(SRCALPHA, INVSRCALPHA, ONE, INVSRCALPHA)` then `device_blend_op(ADD)`; mirror = enabled=true, those factors, op ADD (no `device_enable_blending` call); leave backend context; thread context = NULL (Q-1). On any failure the partially built object is destroyed and the module closed.
- S-043-R06 `gs_destroy(g)`: NULL -> debug log. Leaves all context levels of the calling thread, then (if a device exists) sets thread context = g, enters backend context, destroys cached effects (list order, newest first), destroys subregion, flipped, sprite, IMM vertex buffers (in that order), `device_destroy`, thread context = NULL; frees stacks, closes module (see D-3 for the failed-create case).
### 5.3 Context
- S-043-R07 `gs_enter_context(g)`: NULL -> debug `Null 'graphics' parameter`. If another context is current on this thread it is left completely first (all nesting levels, backend leave called once). If g is not already current: lock g's mutex, `device_enter_context`, set thread context. Then increment the use count (so nested enters only count).
- S-043-R08 `gs_leave_context()`: no context -> R01 debug log. Decrement the count; at zero: `device_leave_context`, unlock mutex, clear thread context.
- S-043-R09 The current context is thread-local; other threads see none (vector 03). A second thread may enter after the first left. `gs_get_context()` returns the current or NULL (never logs).
### 5.4 Matrix stack (frontend only, no backend calls)
- S-043-R10 `push` appends a copy of the top; `pop` removes the top. Pop with a single entry logs ERROR `Tried to pop last matrix on stack` and does nothing.
- S-043-R11 identity/transpose/set/get/mul/rotquat/rotaa/translate/scale/rotaa4f/translate3f/scale3f operate on the top via the S-008 matrix4 operations: `mul`: top = top * m (same as `matrix4_mul(top, m, top)`); translate/scale/rotate: the `_i` (inverse-order "post") variants `matrix4_translate3v_i`, `matrix4_scale_i`, `matrix4_rotate_i`, `matrix4_rotate_aa_i` with dst = top. Vectors 04 show results (translate then scale gives diag(2,3,4) with translation (1,2,3)).
- S-043-R12 Matrices are per graphics object. Without context: R01 (matrix_get leaves its output untouched), except Q-/D-: `gs_matrix_scale3f` has no check (D-1).
### 5.5 Vertex building
- S-043-R13 `gs_render_start(new)`: using_immediate = !new. Clear the five attribute arrays. new: allocate a zeroed vb_data. Not new: take `gs_vertexbuffer_get_data(IMM)` (one backend call), memset its colors to 0xFF (all 512), and make the attribute arrays alias the IMM arrays with capacity 512 and length 0 (unit 0 only for texcoords).
- S-043-R14 `gs_vertex2f/3f/2v/3v`, `gs_normal3f/3v`, `gs_color`, `gs_texcoord(x,y,unit)/texcoord2v` append one entry to the respective array (vertex 2D forms use z=0). In immediate mode, appending when that array already holds 512 entries logs ERROR `<what>: tried to use over 512 for immediate rendering` (what = `gs_vertex`, `gs_normal`, `gs_color`, `gs_texcoord`) and drops the value. `gs_color4v` and `gs_get_input` (returns NULL) are stubs. `unit` MUST be 0..15 (precondition).
- S-043-R15 `gs_render_stop(mode)`, n = vertex count. n = 0: retained mode frees all arrays and the vb_data; nothing else happens (both modes). Count checks, each logging ERROR when the array is non-empty and its length != n: `gs_render_stop: normal count does not match vertex count`, `... color count ...`, `... texture vertex count ...` (texverts unit 0); n = min(n, that length). Immediate: `gs_vertexbuffer_flush(IMM)`, `gs_load_vertexbuffer(IMM)`, `gs_load_indexbuffer(NULL)`, `gs_draw(mode,0,n)`, arrays reset. Retained: save (R16), load vb, load index NULL, `gs_draw(mode,0,0)`, destroy vb. Retained mode ignores n: the min() is only logged.
- S-043-R16 `gs_render_save()`: immediate mode -> NULL. n = 0 -> destroys the vb_data, NULL. Otherwise vb_data.points/normals/colors = array pointers (NULL if empty), num = n, num_tex = count of leading non-empty texcoord units (stop at first empty unit), tvarray[i] width 2, then arrays reset and `gs_vertexbuffer_create(vbd, 0)`.
### 5.6 Sprites
- S-043-R17 `gs_draw_sprite(tex,flip,w,h)` = `gs_draw_quadf(tex,flip,(float)w,(float)h)`.
- S-043-R18 `gs_draw_quadf`: tex given and `gs_get_texture_type != 2D` -> ERROR `A sprite must be a 2D texture`. tex NULL and (w==0 or h==0) -> ERROR `A sprite cannot be drawn without a width/height`. fcx = w if w != 0 else tex width; fcy likewise (width queried from the backend only when needed). push matrix; scale3f(fcx,fcy,1); `gs_load_indexbuffer(NULL)`; if tex and `gs_texture_is_rect(tex)`: take subregion vb data, write points (0,0),(1,0),(0,1),(1,1) and uv in **texel units**: u from 0..texW (flip U: texW..0), v 0..texH (flip V reversed) with texW/H queried from the backend, flush, load, `gs_draw(TRISTRIP,0,0)`; else load `flipped` if flip != 0 else `sprite`, draw TRISTRIP 0,0. pop matrix. (Q-2: any non-zero flip selects the vertically flipped buffer.)
- S-043-R19 `gs_draw_sprite_subregion(tex,flip,x,y,cx,cy)`: non-2D tex -> same ERROR. If x=y=0 and (cx,cy) equals the texture size (queried): `gs_draw_sprite(tex,flip,0,0)`. Else subregion vb data: points (0,0),(cx,0),(0,cy),(cx,cy) (so no matrix scale), uv start_u = x/W, end_u=(x+cx)/W (swapped if FLIP_U), v likewise with y,cy,H, flush, load subregion, load index NULL, draw TRISTRIP 0,0. NULL tex: sizes read as 0 (debug logs), degenerates into `gs_draw_sprite(NULL,..,0,0)` and its ERROR.
### 5.7 Viewport, projection, 2D helpers
- S-043-R20 `gs_viewport_push`: append current backend viewport (`device_get_viewport`). `gs_viewport_pop`: empty stack -> nothing; else `device_set_viewport` with the top then remove. `gs_reset_viewport`: `device_get_size`, `device_set_viewport(0,0,w,h)`. `gs_get_viewport(NULL)` -> R02.
- S-043-R21 `gs_set_2d_mode`: `device_get_size` then `device_ortho(0,w,0,h,-1,-1024)` (args left,right,top,bottom,znear,zfar). `gs_set_3d_mode`, `gs_draw_cube_backdrop`, `gs_cubetexture_set_image`: stubs (no calls).
- S-043-R22 `gs_perspective(angle,aspect,near,far)`: ymax = near*tanf(RAD(angle)*0.5f), ymin=-ymax, xmin=ymin*aspect, xmax=ymax*aspect; `device_frustum(xmin,xmax,ymin,ymax,near,far)` — note the backend parameters named top/bottom receive ymin/ymax.
- S-043-R23 `gs_ortho/frustum/projection_push/pop/set_viewport/set_scissor_rect/resize/update_color_space/get_size/get_width/get_height/flush/present/...` are plain forwards. `gs_get_size` writes the backend answers.
### 5.8 Blend state
- S-043-R24 `gs_enable_blending(b)`: mirror.enabled = b, forward. `gs_blend_function(s,d)`: mirror src_c=src_a=s, dest_c=dest_a=d; forwards **non-separate** `device_blend_function`. `gs_blend_function_separate` mirrors 4 values, forwards. `gs_blend_op` mirrors and forwards. Other enable/depth/stencil/color/cull functions are forwards (cull getter asks the backend).
- S-043-R25 `gs_blend_state_push` appends the mirror. `gs_blend_state_pop`: empty -> nothing; else `gs_enable_blending(enabled)`, `gs_blend_function_separate(src_c,dest_c,src_a,dest_a)`, `gs_blend_op(op)` (backend calls in that order; always the separate form), remove top.
- S-043-R26 `gs_reset_blend_state`: if mirror disabled -> `gs_enable_blending(true)`; if any of the four factors differs from (SRCALPHA,INVSRCALPHA,ONE,INVSRCALPHA) -> `gs_blend_function_separate(defaults)` then `gs_blend_op(ADD)`. The op is not reset otherwise (Q-3). Its context-check log text names `gs_preprocessor_name` (Q-4).
### 5.9 Resources
- S-043-R27 `gs_texture_create(w,h,fmt,levels,data,flags)`: pow2 means value >= 2 and a power of two (1 is not). uses_mipmaps = (flags&BUILD_MIPMAPS) || levels != 1. If uses_mipmaps and not (w and h both pow2): WARNING `Cannot use mipmaps with a non-power-of-two texture.  Disabling mipmaps for this texture.`, clear the flag, levels=1. Then if still uses_mipmaps (flag recomputed from the *original* variable; after the first rule it is false) and RENDER_TARGET: WARNING `Cannot use mipmaps with render targets.  Disabling mipmaps for this texture.`, clear flag, levels=1 (the data pointer is kept). Forward to `device_texture_create`.
- S-043-R28 `gs_cubetexture_create(size,...)`: same two rules with pow2(size); in the render-target case the data pointer is additionally set NULL. Volume, zstencil, stagesurface, samplerstate (NULL info -> R02), vertex/pixel shader, timer, timer range creation forward (shader creation passes text, file name, error string pointer; text NULL -> R02).
- S-043-R29 `gs_vertexbuffer_create(data,flags)`: if data non-NULL, data->num != 0 and flags has DUP_BUFFER: deep copy (points, normals, tangents, colors each `num` elements, tvarray entries each `width*sizeof(float)*num` bytes, num_tex kept) and pass the copy; the caller keeps ownership of the original. Otherwise (including DUP with num = 0, Q-5) the original pointer is passed through and the backend owns it. `gs_indexbuffer_create`: DUP with indices non-NULL and num != 0 copies `num*(2 or 4)` bytes (2 for UNSIGNED_SHORT).
- S-043-R30 Accessors forward with object NULL checks (vector 11); `gs_texture_map` on NULL returns false; destroy functions of NULL do nothing; the destroy of a texture etc. forwards to the type-specific export (`gs_cubetexture_destroy` -> `gs_cubetexture_destroy`).
- S-043-R31 `gs_texture_set_image(tex,data,linesize,flip)`: tex or data NULL -> R02. Map (false -> return, no unmap). row_copy = min(linesize, mapped linesize), h = texture height (queried after map). flip: rows copied from the last source row up (dest row i <- source row h-1-i). Else if linesizes are equal: one copy of row_copy*h bytes. Else row by row advancing each side by its own stride. Unmap.
- S-043-R32 `gs_texture_create_nv12/p010(&y,&uv,w,h,flags)`: odd w or h -> ERROR `NV12 textures must have dimensions divisible by 2.` (`P010 ...`), return false, outputs untouched. If the optional native export exists, call it and return true on success. Fallback: y = texture(w,h,R8|R16,1,NULL,flags), uv = texture(w/2,h/2,R8G8|RG16,1,NULL,flags); if either fails destroy the other, set both NULL, return false.
- S-043-R33 `gs_texture_create_from_file(file)`: decode via the image helper (not specified here); on success `gs_texture_create(cx,cy,format,1,&data,0)` and free the data; failure -> NULL.
- S-043-R34 `gs_*shader_create_from_file`: file NULL -> R02; unreadable -> ERROR `Could not load vertex shader file '<f>'` / `pixel shader` / `effect`, return NULL; else create with (text, file name, error_string).
- S-043-R35 `gs_effect_create_from_file(file)`: before reading, search the context's effect list for an effect whose path equals `file` and return it (no I/O, no backend calls). Effects created with a path are inserted at the list head (newest first, under the effect mutex); `gs_effect_create(text,name,err)` with NULL text -> R02.
- S-043-R36 `gs_swapchain_create(data)`: copies data; num_backbuffers 0 becomes 1; forwards the copy. (NULL data: D-2.)
- S-043-R37 `gs_set_linear_srgb(b)` stores the frontend flag and returns the previous value; `gs_get_linear_srgb` reads it; the framebuffer sRGB functions and `gs_get_color_space` forward.
- S-043-R38 Debug markers: `gs_debug_marker_begin(color,name)`: NULL name -> "(null)". `_begin_format(color,fmt,...)`: NULL fmt -> as NULL name; otherwise vsnprintf into a 63-character buffer (truncation). `_end` forwards. No context check except for the first-level none (calls need a context; vectors 21 only inside).
- S-043-R39 `gs_nv12_available`/`gs_p010_available`: false if the export is absent else forwarded. `gs_get_adapter_count`: 0 if absent.
- S-043-R40 `gs_enum_adapters(cb,param)`: cb NULL -> R02. If `device_enum_adapters` exists and returns true: done. Otherwise (absent or false) additionally call `cb(param,"Default",0)` (Q-6: after a false return the callback may already have been called for real adapters).
### 5.10 texrender (texture-render.c)
- S-043-R41 `gs_texrender_create(fmt,zsfmt)`: zeroed object (rendered=false, size 0). destroy: NULL ok; destroys target and zstencil. `get_texture(NULL)` NULL; `get_format(NULL)` D-4.
- S-043-R42 `gs_texrender_begin(tr,cx,cy)` = begin_with_color_space(...,SRGB). begin: false if tr NULL, tr.rendered, or cx==0 or cy==0. If (cx,cy) != stored size: destroy target and zstencil, store the new size (even if allocation then fails, Q-7), create target `gs_texture_create(cx,cy,format,1,NULL,RENDER_TARGET)`; if zsformat != NONE create zstencil, destroying the new target on failure; creation failure -> false. If no target -> false. Then: viewport_push, projection_push, matrix_push+identity, remember current render target, zstencil target and color space, `gs_set_render_target_with_color_space(target,zs,space)`, `gs_set_viewport(0,0,cx,cy)`; true.
- S-043-R43 `gs_texrender_end`: NULL ok; restore render target/zstencil with the remembered color space, matrix_pop, projection_pop, viewport_pop (backend order shown in vector 16), set rendered=true. `reset` clears rendered. begin is refused while rendered (until reset).
### 5.11 Shader-parameter wrappers
- S-043-R44 `gs_shader_get_param_*`, `_get_viewproj_matrix`, `_get_world_matrix`, `_get_num_params`, `_get_param_info(param, info)` and the setters forward after checking the shader/param (and, for matrix/vec setters, the value pointer; `get_param_by_name` also the name; `get_param_info` also info). Texture/sampler/`set_val` payload NULLs are forwarded. Effect parameters by name: S-020.
### 5.12 Unchecked wrappers
- S-043-R45 (reference behaviour is D-1/D-5) A conforming implementation SHOULD treat the unchecked functions of D-1, D-5 and D-4 exactly like checked ones.

## 6. Algorithms
Sprite UV for rect textures: `u0 = flipU ? W : 0; u1 = flipU ? 0 : W` (v likewise with H); vertex i uses (u_{i&1}, v_{i>>1}). Subregion: `u0 = x/W, u1 = (x+cx)/W` swapped when flipped. Immediate dump semantics for the vectors: points/normals/colors/texcoords are listed up to their current element counts, colors for max(vertex count, color count) entries.

## 7. Concurrency
One mutex per graphics object, locked from the first `gs_enter_context` on a thread until the matching last leave; the context and the use count are thread-local resp. protected by that mutex. Different threads serialise on the mutex. The effect list has its own mutex. A `gs_create` clears the creating thread's context (Q-1).

## 8. Quirks and defects
- Q-1 (QUIRK, script 90) `gs_create` ends with no current context on the calling thread. If the thread was inside another object, that object's mutex stays held and its count 1; entering it again blocks forever. Required: SHOULD preserve the previous thread context across creation.
- Q-2 (QUIRK, 08) For normal (non-rect) 2D textures any non-zero flip bits use the vertically flipped buffer; GS_FLIP_U alone does not flip horizontally. MUST match (files depend on it).
- Q-3 (QUIRK, 13) `gs_reset_blend_state` leaves a non-ADD op alone unless the function was also non-default.
- Q-4 (QUIRK, 27) `gs_reset_blend_state`'s no-context log text is `gs_reset_blend_state` misnamed `gs_preprocessor_name`. Log text is DEBUG only; MAY differ.
- Q-5 (QUIRK, 22) DUP_BUFFER with `num == 0` passes the caller's pointer through (backend owns it).
- Q-6 (QUIRK, 19) adapter enumeration "Default" is appended after a backend that returned false.
- Q-7 (QUIRK, 16) a failed texrender allocation remembers the new size, so a repeated begin with the same size fails without retrying.
- Q-8 (QUIRK, 10) render-target mipmap sanitising keeps the data pointer for 2D textures but drops it for cube textures.
- D-1 (DEFECT, 91) `gs_matrix_scale3f`, `gs_query_sync_capabilities`, `gs_sync_create`, `gs_sync_destroy`, `gs_sync_wait`, `gs_sync_signal_syncobj_timeline_point`, `gs_texture_create_from_dmabuf`, `gs_query_dmabuf_*`, `gs_texture_create_from_pixmap` dereference the current context without check: crash (SIGSEGV) outside a context. Required: R01 behaviour (debug log, default return false/NULL).
- D-2 (DEFECT, 91) `gs_swapchain_create(NULL)` crashes (copies the struct before the check). Required: R02 and NULL.
- D-3 (DEFECT, 91) A failure inside `gs_create` after entering the backend context (e.g. the vertex-buffer creation) leaves the thread context set with count 0; `gs_destroy` of the partial object then loops forever (hang). Required: clean up and return the error.
- D-4 (DEFECT, 91) `gs_texrender_get_format(NULL)` crashes. Required: return GS_UNKNOWN (0).
- D-5 (DEFECT, 92, leak checker only) Texture-coordinate arrays of units not part of the finished buffer are leaked. Required: free them.
- D-6 (DEFECT, outside preconditions) `gs_texcoord(..., unit >= 16)` indexes out of bounds; retained mode with attribute counts different from the vertex count hands the backend arrays shorter than `num`. Required: Rust implementation SHOULD pad or reject.

## 9. Implementation notes
Model the backend as a trait object plus a dyn-handle type per resource kind. Use a thread-local `Option<Arc<Graphics>>` with a reentrant guard that holds the mutex; the use count is per thread. Do not store the immediate arrays aliasing the backend buffer: copy into the backend buffer at flush, but keep `gs_vertexbuffer_get_data(IMM)` call and ordering visible. The matrix stack is a `Vec<Matrix4>` plus index (index always len-1).

## 10. Coverage
| Rules | Scripts |
|---|---|
| R05, R06, R40 | 01, 19, 90, 91 |
| R07-R09, R01 | 02, 03, 27, 90 |
| R10-R12 | 04, 27 |
| R13-R16 | 05, 06, 07, 92 |
| R17-R19 | 08, 09 |
| R20-R23 | 14, 15 |
| R24-R26 | 13 |
| R27-R29, R30, R36 | 10, 11, 22, 23 |
| R31, R32, R33, R34, R35 | 17, 12, 20, 25 |
| R38, R39 | 21, 12, 19 |
| R41-R43 | 16 |
| R44, R02-R04 | 18, 26 |
| 3.5 helpers | 28 |
| Q/D | 08, 10, 13, 16, 19, 22, 90, 91, 92 |

## 11. Open questions
- Rust sufficiency check was skipped (cost control); the spec has not been validated by an independent implementation.
- Windows/macOS wrappers are described from source only.
- R03 lists unchecked parameters from vector 26 behaviour; the complete per-function table is that vector.
- Effect creation internals (shader text) are S-020; this suite only prints the text length.
