# E30 — Backends

**Milestone:** M5 — Phase 3: graphics (section C)
**Subtasks:** [phase-3-subtasks.md](../rust-rewrite/subtasks/phase-3-subtasks.md) § C

**Why:** the wgpu backend replaces GL/D3D11/Metal (T8); D3D11/Metal are ported (S-078/S-079), OpenGL stays C.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E30S1** P3-13 `wgpu` device init | Port `wgpu` device init to Rust (`gl-x11-egl.c`, `gl-wayland-egl.c`) | spec S-043; adapter enumerated like gs_enum_adapters | open |
| **E30S2** P3-14 `wgpu` resources | Port `wgpu` resources to Rust | spec S-043; unsupported ones listed with fallback | open |
| **E30S3** P3-15 Pipeline cache | Port Pipeline cache to Rust | hashing tested | open |
| **E30S4** P3-16 Render and readback | Port Render and readback to Rust | spec S-043; vectors: graphics 15-render-targets; pixel test | open |
| **E30S5** P3-17 Frame pacing and presentation | Port Frame pacing and presentation to Rust (`obs-display.c`) | spec S-058; resize without flicker | open |
| **E30S6** P3-18 Shared texture import: Linux | Port Shared texture import: Linux to Rust (`pipewire.c`, `device-exports.h`) | spec S-056, S-043; vectors: pipewire (56 scripts); screencast shows | open |
| **E30S7** P3-19 Shared texture import: Windows | Port Shared texture import: Windows to Rust (`device-exports.h`) | spec S-051; D3D11 sample texture visible | open |
| **E30S8** P3-20 Shared texture import: macOS | Port Shared texture import: macOS to Rust | spec S-052; Syphon frame visible | open |
| **E30S9** P3-21 GPU encoder texture sharing | Port GPU encoder texture sharing to Rust (`d3d11-texture2d.c`, `obs-video-gpu-encode.c`) | spec S-043, S-058; documented | open |
| **E30S10** P3-22 Legacy backend adapters | Port Legacy backend adapters to Rust (`gl-subsystem.c`) | spec S-057, S-078, S-079; vectors: glbackend (22 scripts); renders test scene | open |
| **E30S11** P3-23 HDR and colour management | Port HDR and colour management to Rust (`d3d11-duplicator.c`) | spec S-043; displays on HDR monitor | open |
