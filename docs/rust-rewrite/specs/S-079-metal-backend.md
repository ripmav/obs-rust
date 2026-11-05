# S-079 Metal graphics backend (macOS)

| | |
|---|---|
| Status | Draft (round 9 — behaviour catalog written from C reference; no vectors yet) |
| Spec version | initial |
| Reference | OBS Studio 33.0.1, `libobs-metal/` (Swift 5, Apple Metal) |
| Target crate | `crates/obs-graphics-metal` |
| Depends on | S-044 (the `gs_*` device contract this backend implements), S-011 (the shader parser the transpiler reuses), S-020 (effect/shader source format) |
| Conformance | planned — none yet (GPU behaviour is not vector-testable headless; see §11) |

## 1. Scope

The macOS/Metal implementation of the `gs_*` device API: one `MTLDevice` wrapper
(`MetalDevice`) that owns the render loop state, a **GLSL-like → MSL source transpiler**
(`OBSShader`) that compiles libobs effect shaders at `device_*shader_create` time, the
buffer/texture/swapchain wrappers around `MTLBuffer`/`MTLTexture`/`CAMetalLayer`, and the
`@_cdecl` glue exporting the `gs_*`/`device_*` entry points. The presentation model is
different from the other backends: libobs renders into a shared texture, and a
`CVDisplayLink` callback **blits** that result into each `CAMetalLayer` drawable
per-screen.

### Reference files (all cited)

- `libobs-metal/metal-subsystem.swift` — the `device_*` entry points (device create/destroy,
  blend/depth/stencil state, render target, frame begin/end, draw, clear, present)
- `libobs-metal/MetalDevice.swift` — the device object: render state, pipeline cache,
  display link, command buffer, clear/draw/copy/stage passes
- `libobs-metal/metal-shader.swift` — `device_*shader_create/load` + the `gs_shader_*`
  parameter API
- `libobs-metal/MetalShader.swift` — the compiled-shader wrapper (uniform buffers,
  `MTLFunction`, vertex/sampler descriptors)
- `libobs-metal/OBSShader.swift` — the effect→MSL transpiler
- `libobs-metal/OBSSwapChain.swift` — one `CAMetalLayer` per projection view (+ EDR headroom)
- `libobs-metal/metal-swapchain.swift` — swapchain create/resize/color-space entry points
- `libobs-metal/MetalTexture.swift` — `MTLTexture` wrapper (surface bind, sRGB views,
  map/unmap, upload/download)
- `libobs-metal/metal-texture2d.swift` — 2D/cube texture create, copy, stage, map entry points
- `libobs-metal/metal-texture3d.swift` — 3D texture create + accessors
- `libobs-metal/MetalBuffer.swift` — buffer base (16-byte-aligned upload, dynamic update)
- `libobs-metal/metal-vertexbuffer.swift` — vertex buffer entry points
- `libobs-metal/metal-indexbuffer.swift` — index buffer entry points
- `libobs-metal/MetalStageBuffer.swift` — staging buffer wrapper
- `libobs-metal/metal-stagesurf.swift` — stage-surface entry points (stage buffer as texture)
- `libobs-metal/metal-zstencilbuffer.swift` — depth/stencil buffer entry points
- `libobs-metal/metal-samplerstate.swift` — sampler state entry points
- `libobs-metal/libobs+Extensions.swift` — all `gs_*` → Metal enum/format/param-size tables
- `libobs-metal/MTLTexture+Extensions.swift` — `getRetained`/`getUnretained` for `MTLTexture`
- `libobs-metal/libobs+SignalHandlers.swift` — the `video_reset` signal trampoline
- `libobs-metal/metal-unimplemented.swift` — the exported no-op stubs
- `libobs-metal/metal-subsystem.swift` also defines `OBSLog` and the retained/unretained
  pointer helpers shared by every entry point

## 2. Conventions

RFC 2119. Entry points are `@_cdecl`-exported C functions; opaque pointers cross the
boundary with Swift `Unmanaged` retain/release semantics — every object exposes
`getRetained()` (refcount +1) and `getUnretained()`; a handle returned to libobs is retained,
and a `gs_*_destroy` entry point re-converts to a retained reference, which deinit releases.
The reference's Swift `assertionFailure` sites are the error paths: the entry point logs and
returns NULL (or the default) rather than trapping the process.

## 3. Device bring-up and render loop (`MetalDevice.swift`, `metal-subsystem.swift`)

- **S-079-R01** `device_create`: creates the `MetalDevice` around the chosen `MTLDevice`
  (adapter from `gs_init_data`); the init creates the command queue, a 1-byte private
  **fallback vertex buffer**, and a compiled **NOP vertex function**
  (`[[vertex]] float4 vsNop() { return (float4)0; }`, fast-math) used by the clear pass and
  for pipelines without a real vertex stage. `device_destroy` tears down the display link
  and command queue; `device_get_name` = `"metal"`, `device_get_type` = the Metal gs device
  type, `device_preprocessor_name` = the Metal preprocessor name libobs shaders are matched
  against. `OBSLog` routes Swift logs into libobs logging with the OBS level enum
  (error=100 … debug=400, `libobs+Extensions.swift`).
- **S-079-R02** Render state: `MetalRenderState` holds the current view/projection/viewproj
  matrices (identity at init), scissor-rect flag, colour space, the active swap chain, blend
  and depth/stencil state. `device_begin_frame`/`device_end_scene` bound/unbound the per-frame
  command-buffer scope; `device_draw(drawMode, start, count)` maps `gs_draw_mode` →
  `MTLPrimitiveType` and records the draw call; `device_clear` builds the `ClearState`
  (load actions + colour/depth/stencil values from the `gs_clear_type` flags) and either
  applies them as render-pass load actions or renders the clear through the NOP pipeline.
- **S-079-R03** Pipeline/state caches: `pipelines` (keyed int → `MTLRenderPipelineState`) and
  `depthStencilStates` are cached per combined state key; `device_set_render_target` /
  `device_set_render_target_with_color_space` select the target texture + z-stencil and
  refresh the descriptors; `device_enable_framebuffer_srgb`/`device_framebuffer_srgb_enabled`
  toggle the sRGB framebuffer path; the blend entry points (`device_blend_function`,
  `_separate`, `device_blend_op`) translate through the tables in R10 into the current blend
  state.
- **S-079-R04** Presentation: a `CVDisplayLink` (active displays) drives
  `displayLinkCallback` → `blitSwapChains()`, which encodes a blit of the projector result
  texture into each `OBSSwapChain`'s current `CAMetalLayer` drawable and presents it;
  `device_is_present_ready` reflects drawable availability, `device_present` signals it.
  `finishDisplayRenderStage`/`finishPendingCommands` commit/`waitUntilCompleted` the command
  buffers at the right stages; `ensureCommandBuffer` lazies the per-frame command buffer.
- **S-079-R05** `video_reset` signal: `metal_video_reset_handler`
  (`libobs+SignalHandlers.swift`) is libobs's signal callback; it bit-casts the param back to
  `MetalDevice` and calls `dispatchSignal(.videoReset)` → the registered callback **stops
  and restarts the CVDisplayLink** (video reset changes screen refresh rates).
- **S-079-R06** Texture copy/stage entry points on the device: `copyTexture`
  (texture→texture blit), `stageTexture` (texture→staging), `stageTextureToBuffer` /
  `stageBufferToTexture` (texture↔staging buffer via blit), `copyTextureRegion` (region
  blit with origin/size) — all record on the command buffer and return the command-buffer
  id the caller synchronises on.

## 4. Shader pipeline: effect → MSL (`OBSShader.swift`, `metal-shader.swift`, `MetalShader.swift`)

- **S-079-R07** `device_vertexshader_create` / `device_pixelshader_create`: parse the source
  with the S-011 shader parser (`shader_parse`), run the transpiler (R08–R12) to produce an
  MSL string, build the `ShaderData` metadata (R13), then compile the MSL string into an
  `MTLLibrary`/`MTLFunction` and wrap it in `MetalShader`. Parse/transpile/compile failures
  log (with the file location) and return NULL; the error is never thrown across the C
  boundary. `device_load_vertexshader`/`device_load_pixelshader` add a file-name argument
  but the path is diagnostic only — the source text is always supplied.
- **S-079-R08** Transpilation pipeline (`OBSShader.transpiled`): `analyzeUniforms` →
  `analyzeParameters` → `analyzeFunctions` (walk the parsed AST, collecting uniforms,
  parameters, structs, functions in declaration order) → `transpileUniforms` +
  `transpileStructs` + `transpileFunctions` → `buildMetadata`. Output =
  `MSLTemplates.header` + the transpiled blocks joined by blank lines. The source language is
  the libobs effect language (HLSL-like); MSL does not allow global state, so **uniforms
  become a per-buffer struct** and all other HLSL-isms are rewritten in place.
- **S-079-R09** Type conversion (`convertToMTLType`): `texture2d/3d/_cube` →
  `texture2d<float>/texture3d<float>/texturecube<float>`; `half*` → `float*`;
  `min16float*` → `half*`; `min16int*` → `short*`; `min16uint*` → `ushort*`; everything
  else passes through unchanged. `texture_rect`, `min10float`, `min13int`, `double` are
  **unsupported** (transpile error). `convertToMTLMapping`: `POSITION` → `[[position]]`,
  `VERTEXID` → `[[vertex_id]]`.
- **S-079-R10** Body rewrites (`transpileFunctionContent` + helpers): `ddx`/`ddy` →
  `dfdx`/`dfdy`, `frac` → `fract`, `lerp` → `mix` (`convertToMTLIntrinsic`; `clip` is
  unsupported); scalar/vector comparisons against `== != < <= >= >` are wrapped as
  `all(lhs op rhs)` because MSL compares vectors element-wise into a bool vector
  (`convertToMTLComparison`); `mul(a,b)` → `(a) * (b)` and `mad(a,b,c)` →
  `((a) * (b)) + (c)` (`convertToMTLMultiplication`/`convertToMTLMultiplyAdd`, recursing
  into the argument lists); `texture`/`texCube`/… sample calls are rewritten to MSL
  `texture.sample(sampler, coord)` form with a `MTLSamplerState` (`createSampler`,
  `createTextureCall`), including the `SampleVariant` for the LOD/mip forms.
- **S-079-R11** Uniforms (R08 step 1): each uniform becomes a `ShaderUniform` with its
  `gs_shader_param_type`, a byte offset inside the uniform buffer, and (for textures) the
  next texture/sampler slot. The order is **load-bearing** — struct size/stride/alignment
  follow declaration order; `gs_shader_param_type.size`/`.mtlSize`/`.mtlAlignment`
  (`libobs+Extensions.swift`) define the byte layout the CPU writes.
- **S-079-R12** `generateAdditionalArguments`: functions whose parameters reference uniforms
  get the uniform-buffer argument injected so the transpiled MSL can access them.
- **S-079-R13** `MetalShader` (`MetalShader.swift`): holds the `MTLLibrary`/`MTLFunction`,
  the `ShaderUniform` list, the `MTLVertexDescriptor` (vertex layout from
  `ShaderData.vertexDescriptor`) and sampler states. `ShaderUniform.setParameter` stores raw
  bytes (`hasUpdates`); at draw time `MetalDevice.draw` (R02) converts updated uniforms into
  the uniform `MTLBuffer` bytes (`updateUniform`/`uploadShaderParameters` in
  `MetalShader.swift`) — the per-frame CPU→GPU uniform path.
- **S-079-R14** `gs_shader_*` API (`metal-shader.swift`): `gs_shader_get_num_params`,
  `gs_shader_get_param_by_idx`/`_by_name`, `gs_shader_get_param_info` (name/type/flags),
  `gs_shader_set_param_*` (typed setters funnel into `setParameter`),
  `gs_shader_get_viewproj_matrix`/`gs_shader_get_world_matrix` (the matrices the effect
  expects), `gs_shader_set_viewproj_matrix`/`gs_shader_set_world_matrix`.

## 5. Buffers (`MetalBuffer.swift`, `metal-vertexbuffer.swift`, `metal-indexbuffer.swift`,
`MetalStageBuffer.swift`, `metal-stagesurf.swift`)

- **S-079-R15** `MetalBuffer` base: `createOrUpdateBuffer` writes the data into an
  `MTLBuffer` with size rounded up to a **16-byte multiple**
  (`.cpuCacheModeWriteCombined`, `.storageModeShared`); dynamic buffers are updated
  in place when the aligned size is unchanged. `BufferDataType`
  (vertex/normal/tangent/color/texcoord) names the vertex attributes; `setupBuffers` /
  `getShaderBuffers` (×2 overloads) materialise the per-attribute `MTLBuffer` set for a
  draw call.
- **S-079-R16** `device_vertexbuffer_create` / `device_indexbuffer_create`
  (`metal-vertexbuffer.swift`, `metal-indexbuffer.swift`): take ownership of the libobs
  memory (`gs_vb_data` / raw indices) — the buffer is not copied, the pointer is retained
  as-is; `GS_DYNAMIC` selects the in-place update path (R15). Index type maps through
  `gs_index_type.mtlType` (16/32-bit); an invalid type → NULL. `gs_*_flush_direct` write
  back dynamic data, `gs_*_get_data` return the storage pointer, `gs_indexbuffer_get_num_indices`
  / `gs_indexbuffer_get_type` return the accessors.
- **S-079-R17** Stage buffer: `MetalStageBuffer` is a `MTLBuffer` sized for
  `width*height*bytesPerPixel`; `device_stagesurface_create` (metal-stagesurf.swift) wraps
  it as a `gs_stagesurface` (the "texture-shaped buffer" libobs uses for staging and
  CPU mapping); width/height accessors and `gs_stagesurface_map` (read/write mapping over
  the buffer) round out the file.

## 6. Textures (`MetalTexture.swift`, `metal-texture2d.swift`, `metal-texture3d.swift`,
`MTLTexture+Extensions.swift`)

- **S-079-R18** `MetalTexture` wrapper: holds the `MTLTexture`, its descriptor, a UUID
  `resourceID` (identity used by the copy/stage entry points), a `MetalTextureMapMode`
  (unmapped/read/write), and an optional **sRGB view** — `updateSRGBView` creates a
  `MTLTextureView` in the sRGB variant (`bgra8Unorm→bgra8Unorm_srgb`,
  `rgba8Unorm→rgba8Unorm_srgb`, `r8/rg8` likewise, `bgra10_xr→bgra10_xr_srgb`) for every
  non-framebuffer-only texture. Three constructors: from descriptor, from `IOSurfaceRef`
  (`bindSurface` — pixel format via `MTLPixelFormat(osType:)`, shader-read only, plane 0),
  and from a bare `MTLTexture`; plus a 2×2 bgra8 **placeholder** constructor for handles that
  are filled later.
- **S-079-R19** `rebind(surface:)`: Metal has no GL "rebind" — a new `MTLTexture` is created
  from the new `IOSurface` and the wrapper swapped (then `updateSRGBView`). Mapping
  (`map`/`unmap`): read/write access is a blit to/from the staging path (the texture must be
  non-framebuffer-only; framebuffer-only textures cannot be mapped — the entry point logs
  and fails). `upload`/`download` blit between texture and staging buffer; the
  BGRA / ARGB2101010LE surface pixel formats are the two accepted IOSurface formats
  (`kCVPixelFormatType_32BGRA`, `kCVPixelFormatType_ARGB2101010LEPacked`).
- **S-079-R20** `metal-texture2d.swift`: `device_texture_create` (from `gs_texture_data`,
  honouring `GS_TEXTURE_DYNAMIC`), `device_cubetexture_create` (6 faces),
  `device_get_texture_type`, `device_load_texture` / `device_load_texture_srgb` (sRGB
  selects the sRGB view, R18), `device_copy_texture_region` / `device_copy_texture`
  (→ R06), `device_stage_texture` (→ R06), width/height accessors, `gs_texture_get_color_format`
  (inverse of the `mtlFormat` table, R10-table below), `gs_texture_map`/`gs_texture_unmap`
  (R19).
- **S-079-R21** `metal-texture3d.swift`: `device_voltexture_create` (volume texture) + the
  width/height/depth/format accessors. `MTLTexture+Extensions.swift`: the retained/
  unretained pair for raw `MTLTexture` pointers crossing the C boundary.

## 7. Swap chains (`OBSSwapChain.swift`, `metal-swapchain.swift`)

- **S-079-R22** `OBSSwapChain`: one `CAMetalLayer` per projection view — created with
  `framebufferOnly=false`, `bgra8Unorm_srgb`, sRGB colorspace, an `MTLFence`, and
  `colorRange` (sdr / hdrPQ / hdrHLG). `updateView` (main thread) sets the view's layer to
  the managed `CAMetalLayer`; `updateEdrHeadroom` reads the view's screen
  `maximumPotentialExtendedDynamicRangeColorComponentValue` (default 1.0) so HDR output
  tracks the actual display; `resize` updates the drawable size and invalidates the
  `renderTarget` (the Qt-managed view's resize is routed into libobs manually — the swap
  chain is not in the view hierarchy's control).
- **S-079-R23** `metal-swapchain.swift`: `device_swapchain_create` takes the
  `gs_init_data` (NSView + size + format), builds the `OBSSwapChain`, `updateView`s, and
  appends it to the device's `swapChains` under `swapChainQueue` (a
  `userInteractive`-QoS serial dispatch queue — swap-chain mutation is serialised and
  main-thread-free). `device_resize` resizes the active swap chain's drawable;
  `device_update_color_space` hops `@MainActor` to refresh EDR headroom; `device_get_size` /
  `device_get_width` / `device_get_height` return the current size; `device_load_swapchain`
  re-attaches an existing handle; `gs_swapchain_destroy` releases it.

## 8. Depth/stencil and samplers

- **S-079-R24** `metal-zstencilbuffer.swift`: `device_zstencil_create` (format via the
  `gs_zstencil_format.mtlFormat` table: Z16 → depth16Unorm, Z24_S8 → depth24_stencil8,
  Z32F → depth32Float, Z32F_S8X24 → depth32Float_stencil8; NONE → invalid/NULL) and
  `device_get_zstencil_target` (the current target from the render state).
- **S-079-R25** `metal-samplerstate.swift`: `device_samplerstate_create` builds an
  `MTLSamplerDescriptor` from the libobs sampler settings — address modes through
  `gs_address_mode.mtlMode` (WRAP→repeat, CLAMP→clampToEdge, MIRROR→mirrorRepeat,
  BORDER→clampToBorderColor, MIRRORONCE→mirrorClampToEdge), min/mag/mip filters through
  `gs_sample_filter` (POINT→nearest, LINEAR→linear, the mixed/anisotropic variants pick
  nearest or linear per the table); `device_load_samplerstate` re-attaches an existing
  handle.

## 9. Format tables (`libobs+Extensions.swift`)

- **S-079-R26** The `gs_*` → Metal mapping tables in this file are the backend's
  vocabulary: `gs_color_format.mtlFormat` (A8/R8/R8G8/R16/R16F/RG16*/R32F*/RGBA/BGRA*/
  R10G10B10A2/RGBA16*/RGBA32F/DXT1-3-5 → the matching `MTLPixelFormat`; anything else →
  `.invalid`), `sRGBVariant` (only RGBA and BGRX/BGRA have sRGB counterparts),
  `gs_color_space` (colour-space → format/pixel-format: 709_EXTENDED → `bgra10_xr_srgb`,
  SRGB_16F/709_SCRGB → RGBA16F, rest → RGBA), depth functions, stencil ops, blend
  factors/ops, cull modes, draw modes, viewport/scissor conversion from `gs_rect`,
  index types, address modes, sample filters, and the shader-param size/alignment tables
  (R11). Unsupported combinations return nil → the entry point logs and fails.

## 10. Exported no-op stubs (`metal-unimplemented.swift`)

- **S-079-R27** These are **exported but unimplemented** — the entry points exist so the
  `gs_*` surface links, and must return the "unsupported" contract values:
  `device_load_default_samplerstate` (no-op), `device_enter_context`/`device_leave_context`
  (no-op — Metal is thread-confined, no GL-style context switching), `device_timer_create` /
  `device_timer_range_create` / `gs_timer_*` (timers unimplemented; `gs_timer_get_data` /
  `gs_timer_range_get_data` return false), `device_debug_marker_begin`/`_end` (no-op), and
  `device_set_cube_render_target` (cube render targets unsupported on this backend —
  libobs must fall back to 6 individual renders).

## 11. Concurrency and conformance

- The render loop is **single-threaded on the graphics thread** (the command buffer is
  created/committed there, R04); `swapChainQueue` serialises swap-chain structural
  changes; `@MainActor` only for view/EDR access. The CVDisplayLink callback runs on a
  system thread and must only touch the blit path (R04) — no render-state mutation.
- Conformance: GPU output cannot be compared byte-exact headlessly. The vector-testable
  surface is (a) the **MSL transpiler** (source → MSL string is deterministic — golden
  vectors are feasible: type rewrites R09, comparison/mul/mad/intrinsic rewrites R10,
  uniform layout R11/R26) and (b) the format tables (R26). Presentation and GPU timing are
  out of scope for vectors.

## 12. Open questions

1. Exact contents of `MSLTemplates.header` (the fixed MSL preamble: `#include <metal_stdlib>`,
   usings, the sampler type alias) — re-derived at port time, not pinned here.
2. The pipeline cache key format (the combined int encoding of render state) — internal;
   behaviour is "same state → same `MTLRenderPipelineState`".
3. `bgra10_xr` vs `bgra10_xr_srgb` selection for HDR pipelines — the reference's choice
   per colour-space is tabled (R26) but the interaction with the EDR headroom (R22) is not
   characterised.
4. What happens on `MTLDevice` loss / GPU hang (Metal error handler behaviour) — the
   reference relies on Metal's own recovery; not specified.
