# E37 — Hotkeys, modules, display

**Milestone:** M6 — Phase 4: core object model (section E)
**Subtasks:** [phase-4-subtasks.md](../rust-rewrite/subtasks/phase-4-subtasks.md) § E

**Why:** hotkeys, the module loader (libloading) and display settings.

| Story | I want | Acceptance | Status |
|---|---|---|---|
| **E37S1** P4-28 Hotkey system | Port Hotkey system to Rust (`obs-hotkey.c`, `obs-hotkey.h`) | spec S-034, S-024; vectors: hotkeys (01-register-basic, 02-register-contexts,…; ids stable | open |
| **E37S2** P4-29 Module loader | Port Module loader to Rust (`obs-module.c`, `obs-module.h`) | spec S-035; vectors: modservice (10-module-open, 11-module-open-success); all resolved via libloading | open |
| **E37S3** P4-30 Locale and text lookup | Port Locale and text lookup to Rust (`obs-module.c`) | spec S-035; vectors: modservice (23-module-files-locale); tests | open |
| **E37S4** P4-31 Display and views | Port Display and views to Rust (`obs-display.c`) | spec S-042; vectors: obscore (060_display, 061_display_failure_colorspace,…; tests with null graphics | open |
| **E37S5** P4-32 Platform integration | Port Platform integration to Rust (`obs-nix-x11.c`, `obs-nix-x11.h`) | spec S-058, S-034; vectors: hotkeys/; hotkey fires | open |
| **E37S6** P4-33 Canvases | Port Canvases to Rust (`obs-canvas.c`) | spec S-042; vectors: obscore (040_canvas_main … 049_canvas_video_active); second canvas | open |
