# Clean-room specifications

Specifications precise enough for an engineer or AI agent to implement a component in
Rust **without reading the C code**, checked mechanically against an executable
conformance suite (`test/conformance/`). Process, roles and rules:
[../clean-room/PROCESS.md](../clean-room/PROCESS.md); instructions for AI agents:
[../clean-room/AGENT-BRIEF.md](../clean-room/AGENT-BRIEF.md); template:
[../clean-room/SPEC-TEMPLATE.md](../clean-room/SPEC-TEMPLATE.md).

## How to use a spec

1. Read the spec top to bottom; rule IDs (`S-002-R14`) are the unit of conformance.
2. Read `test/conformance/README.md` (script language, comparison rules).
3. Implement the crate; implement a runner for the area's script language; run all scripts.
4. Anything the spec does not answer is a **spec question** — never look at the C code.

## Status

| ID | Component | Reference size | Scripts | Status | Sufficiency check (see below) |
|---|---|---|---|---|---|
| [S-001](S-001-bmem-base.md) | Allocator, logging, crash hooks | 290 lines | 5 | Ready for review | not run (platform allocator) |
| [S-002](S-002-calldata.md) | `calldata` parameter stack | 430 lines | 12 | Ready for review | 12/12 |
| [S-003](S-003-declaration-language.md) | Declaration language | 270 lines (+ generic lexer) | 9 | Ready for review | 9/9 |
| [S-004](S-004-signals-and-procs.md) | Signal handlers, global callbacks, proc handlers | 640 lines | 17 | Ready for review | 17/17 |
| [S-005](S-005-darray-deque.md) | `darray`, `deque` | 925 lines | 4 + 5 | Ready for review | 9/9 |
| [S-006](S-006-dstr.md) | `dstr` and string helpers | 1,100 lines | 10 | Ready for review | not run |
| [S-007](S-007-config-file.md) | `config-file` (INI) | 890 lines | 11 | Ready for review | not run |
| [S-008](S-008-vec-quat-matrix4.md) | `vec3/4`, `quat`, `matrix4`, `axisang` | ≈ 1,000 lines | 8 | Ready for review | not run |
| [S-009](S-009-math-rest.md) | `vec2`, `matrix3`, `plane`, `bounds`, `math-extra`, `half`, sRGB | ≈ 500 lines | 28 | Draft (second author needed) | 579/579 lines (author had read C) |
| [S-010](S-010-serializers-bitstream-textlookup.md) | serializers, bit reader, text-lookup | 880 + 270 lines | 28 | Ready for review | 28/28 (author had read C) |
| [S-011](S-011-lexer-cf-parser.md) | base lexer, `cf-lexer`, `cf-parser` | 2,500 lines | 25 | Ready for review | 25/25 (author had read C) |
| [S-012](S-012-nal-avc-hevc-av1.md) | NAL / AVC / HEVC / AV1 helpers | 800 lines | 19 | Ready for review | 19/19 (+ 6,000-case differential) |
| [S-013](S-013-media-conversion.md) | pixel/colour conversion, video frame, audio math | 800 lines | 27 | Ready for review | 27/27 within tolerance |
| [S-014](S-014-obs-data.md) | `obs_data` (settings objects, JSON read/write) | 2,500 lines | 20 | Ready for review | 20/20 (author had read C) |
| [S-015](S-015-obs-properties.md) | `obs_properties` | 1,800 lines | 25 | Ready for review | 25/25 (author had read C) |
| [S-020](S-020-effect-language.md) | effect/shader language: parsers, runtime, backend hand-off | 3,200 lines | 41 | Ready for review | 41/41 + 8,500-case differential |
| [S-021](S-021-flv-amf0-enhanced-rtmp.md) | FLV muxer, AMF0, enhanced RTMP, hvcC/av1C builders | ≈ 3,000 lines | 34 | Ready for review | 34/34 (author had read C) |
| [S-022](S-022-mp4-muxer.md) | MP4 muxer (all boxes, fragmented/hybrid) | 3,000 lines | 30 | Ready for review | 30/30 (author had read C; ffprobe sanity) |
| [S-023](S-023-platform-utf8.md) | platform utilities (files, paths, globs, time format) and UTF-8 | ≈ 2,200 lines | 38 | Ready for review | 38/38 (partly libc FFI) |
| [S-024](S-024-small-helpers.md) | opts-parser, 128-bit math, process args, hotkey table, missing-files, task queue, profiler, BPM, happy-eyeballs order, file-updater | ≈ 2,500 lines | 75 | Ready for review | 75/75 (author had read C) |
| [S-025](S-025-encoder-options.md) | x264 / NVENC / QSV / FFmpeg / FDK option logic | ≈ 3,000 lines | 56 | Ready for review | 56/56 |
| [S-026](S-026-rtmp-services.md) | rtmp-services JSON loader and selection | ≈ 1,500 lines | 28 | Ready for review | see spec |
| [S-027](S-027-audio-filters.md) | audio filters (DSP) | part of 7,600 lines | 15 | Ready for review | see spec |
| [S-028](S-028-video-filters.md) | video filters (13 filters, recorded graphics calls) | part of 7,600 lines | 27 | Ready for review | not run |
| [S-029](S-029-transitions.md) | obs-transitions | ≈ 1,900 lines | 16 | Ready for review | see spec |
| [S-030](S-030-image-source.md) | image source, colour source, slideshow (v1 and mk2) | ≈ 2,800 lines | 46 | Ready for review | not run |
| [S-031](S-031-rtmp-protocol.md) | RTMP protocol (handshake, chunking, commands) | ≈ 8,300 lines (vendored) | 35 | Ready for review | see spec |
| [S-032](S-032-audio-mixer-controls.md) | audio mixer and controls (fader, volmeter) | ≈ 1,600 lines | 39 | Ready for review | not run |
| [S-033](S-033-scene-transform-math.md) | scene-item transforms, groups, z-order, visibility (obs-scene) | ≈ 4,300 lines | 40 | Ready for review | not run |
| [S-034](S-034-hotkeys.md) | hotkeys: registration, bindings, polling, pairs, save/load | ≈ 1,400 lines | 15 | Ready for review | not run |
| [S-035](S-035-service-and-module-loader.md) | obs-service and module loader (dlopen fake plugins) | ≈ 3,100 lines | 23 | Ready for review | not run |
| [S-036](S-036-media-playback.md) | media-playback (media thread, cache, mapping tables) | ≈ 2,400 lines | 33 | Draft | not run |
| [S-037](S-037-ffmpeg-mux-protocol.md) | ffmpeg-mux stdin protocol (receiver linked; sender from source) | ≈ 1,500 lines | 22 | Ready for review | not run |
| [S-038](S-038-text-freetype2.md) | text-freetype2 (fake FreeType, atlas, wrap, chat-log) | ≈ 2,100 lines | 22 | Ready for review | not run |
| [S-039](S-039-scripting-api.md) | scripting core (language-independent; Lua/Python backends not covered) | ≈ 5,400 lines (part) | 14 | Ready for review | not run |
| [S-040](S-040-obs-source.md) | sources, filters, async video, transitions (obs-source) | ≈ 7,600 lines | 51 | Ready for review | not run |
| [S-041](S-041-encoder-output-delay.md) | encoder, output, interleaver, reconnect, delay | ≈ 5,800 lines | 33 | Ready for review (partial coverage) | not run |
| [S-042](S-042-obs-core.md) | core start-up, reset_video/audio, canvas, view, display | ≈ 4,600 lines | 48 | Draft (partial coverage) | not run |
| [S-043](S-043-graphics-api.md) | `gs_*` API semantics above the backend (fake backend) | ≈ 3,300 lines | 31 | Ready for review | not run |
| [S-044](S-044-audio-video-io.md) | video-io and audio-io threads (lock-step fake clock) | ≈ 1,100 lines | 26 | Ready for review | not run |
| [S-045](S-045-rtmp-stream-net-if.md) | rtmp-stream and net-if (fake sockets/librtmp) | ≈ 2,300 lines | 30 | Ready for review | not run |
| [S-046](S-046-obs-ffmpeg-outputs.md) | obs-ffmpeg outputs: mux sender, replay buffer, HLS, ffmpeg_output | ≈ 5,000 lines | 39 | Ready for review | not run |
| [S-047](S-047-obs-ffmpeg-sources-encoders.md) | obs-ffmpeg media source and encoder wrappers (runtime half) | ≈ 3,000 lines | 20 | Ready for review | not run |
| [S-048](S-048-frontend-formats-and-cli.md) | frontend (non-GUI): CLI flags, INI files and migrations, profile/collection formats, file names, API catalog. Vectors from a C transcription, not the C++ binary | part of 75k (C++) | 7 | Draft (Tier C) | not run |
| [S-049](S-049-linux-capture-plugins.md) | Linux capture: V4L2, PulseAudio, xshm (vectors); ALSA, JACK, xcomposite (rules only) | ≈ 6,000 lines | 26 | Draft (Tier C) | not run |
| [S-050](S-050-hardware-encoders.md) | hardware encoders: obs-nvenc, QSV, VAAPI, ffmpeg-nvenc (hand-written SDK shims); AMF rules only | ≈ 9,000 lines | 57 | Draft (Tier C) | not run |
| [S-051](S-051-windows-capture-plugins.md) | Windows capture: window matching, hook structs, virtual-camera queue (real C); other logic MODEL/SRC | ≈ 12,000 lines | 28 | Draft (Tier C) | not run |
| [S-052](S-052-macos-plugins.md) | macOS plugins: all vectors MODEL (source-derived), numeric Apple constants need checking on a Mac | ≈ 8,000 lines | 11 | Draft (Tier C) | not run |
| [S-053](S-053-graphics-images-and-builtin-effects.md) | image decoding (image-file, graphics-ffmpeg, libnsgif) and the built-in effects (`libobs/data/*.effect`) | ≈ 8,100 lines | 21 | Draft (vectors complete from real C; sufficiency check not performed) | not run |
| [S-054](S-054-obs-filters-remainder.md) | obs-filters remainder: module table, vendored RNNoise/SpeexDSP contract, effect inventory | ≈ 650 lines (+ 13,500 vendored) | 8 | Ready for review (sufficiency check skipped) | skipped |
| [S-055](S-055-libobs-util-remainder.md) | CRC-32, child-process pipes, source profiler, D-Bus/portal sleep inhibition, PulseAudio audio monitor | ≈ 2,300 lines | 26 | Ready for review (sufficiency check skipped) | skipped |
| [S-056](S-056-linux-pipewire.md) | linux-pipewire: screencast/camera portals, formats | ≈ 4,000 lines | 56 | complete for scripted-fake-reachable behaviour | not run |
| [S-057](S-057-opengl-backend.md) | OpenGL backend (`libobs-opengl`), real C on a real software GL context | ≈ 7,100 lines | 22 | Ready for review (sufficiency check skipped) | skipped |
| [S-058](S-058-video-render-loop-and-nix-glue.md) | graphics-thread frame loop, GPU encode, nix/X11 glue — Wayland **not** covered | ≈ 3,300 lines | 23 | Ready for review (sufficiency check not performed) | not run |
| [S-059](S-059-mpegts-srt-rist-output.md) | `mpegtsout`: native MPEG-TS network output (SRT / RIST / UDP / TCP) | ≈ 2,700 lines | 32 | Ready for review (sufficiency check skipped) | skipped |
| [S-060](S-060-lua-scripting-backend.md) | Lua scripting backend (obslua) | ≈ 2,400 lines | 25 | Draft (sufficiency check skipped) | not run |
| [S-061](S-061-threading-primitives.md) | threading primitives: `os_event` (auto/manual), `os_sem`, thread naming, platform atomics | ≈ 430 lines | 6 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 4 corrected; 6 vectors round 11) | not run |
| [S-062](S-062-obs-websocket.md) | obs-websocket plugin: 5.x wire protocol, identify/auth, request/event dispatch, vendor C API v3 | ≈ 9,000 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 1 corrected; no vectors yet) | not run |
| [S-063](S-063-python-scripting-backend.md) | Python scripting backend: dynamic CPython import, GIL bridging, Python frontend API | ≈ 630 lines | 25 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 5 corrected; 25 vectors round 12) | not run |
| [S-068](S-068-frontend-settings.md) | frontend settings dialog (pages, dirty/apply/save, hotkeys) | ≈ 8,200 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 6 corrected; no vectors yet) | not run |
| [S-069](S-069-frontend-main-window.md) | frontend main window, controls, preview host & per-area widgets (preview interaction → S-067) | ≈ 21,900 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 3 corrected; no vectors yet) | not run |
| [S-070](S-070-frontend-scene-collections.md) | frontend scene collections, profiles & importers (import rules, profile copy, migration) | ≈ 5,000 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, clean; no vectors yet) | not run |
| [S-071](S-071-audio-mixer-widgets.md) | frontend audio mixer dock & volume widgets (mixer state, pin/hide, meters, sliders) | ≈ 3,100 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 4 corrected; no vectors yet) | not run |
| [S-072](S-072-frontend-dialogs-toolbars.md) | frontend dialogs, source pickers, toolbars & tree widgets | ≈ 5,400 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 4 corrected; no vectors yet) | not run |
| [S-073](S-073-frontend-output-ui.md) | frontend output pipeline UI (simple/advanced output, remux, multitrack, media controls) | ≈ 5,700 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 2 corrected; no vectors yet) | not run |
| [S-074](S-074-frontend-updater-oauth-youtube.md) | frontend updater, OAuth & YouTube | ≈ 6,700 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 2 corrected; no vectors yet) | not run |
| [S-075](S-075-frontend-plugin-manager.md) | frontend plugin manager, crash handler & system info (install = drop files; no install flow in 33.0.1) | ≈ 3,600 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 1 corrected; no vectors yet) | not run |
| [S-076](S-076-properties-view-ui.md) | properties view & Qt widget kit | ≈ 3,800 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, clean; no vectors yet) | not run |
| [S-077](S-077-frontend-api.md) | frontend API (obs_frontend_api) & utility glue (catalog in S-048) | ≈ 1,700 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, clean; no vectors yet) | not run |
| [S-078](S-078-d3d11-backend.md) | D3D11 `gs` backend: device/swap chain, resources, shaders, device-loss rebuild, duplicator | ≈ 5,400 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 8 corrected; no vectors yet) | not run |
| [S-079](S-079-metal-backend.md) | Metal `gs` backend: device/display-link render loop, GLSL→MSL transpiler, IOSurface/CAMetalLayer presentation | ≈ 3,900 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 2 corrected; no vectors yet) | not run |
| [S-080](S-080-obs-webrtc-whip.md) | obs-webrtc WHIP output: offer POST, 401 challenge, Link header, answer SDP, simulcast, DELETE | ≈ 900 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 7 corrected; no vectors yet) | not run |
| [S-081](S-081-dshow-ffi.md) | libdshowcapture FFI contract (ported to Rust, D-19) | ≈ 5,300 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 3 corrected; no vectors yet) | not run |
| [S-082](S-082-obs-browser-cef-ffi.md) | obs-browser / CEF FFI contract (CEF kept as library, glue in Rust — D-09) | ≈ 4,150 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 2 corrected; no vectors yet) | not run |
| [S-083](S-083-decklink-ffi.md) | Blackmagic DeckLink FFI contract (SDK kept, glue Rust, D-04/D-10) | ≈ 3,050 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, clean; no vectors yet) | not run |
| [S-084](S-084-aja-ffi.md) | AJA FFI contract (Kona SDK kept, glue Rust, D-04/D-10) | ≈ 6,950 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 3 corrected; no vectors yet) | not run |
| [S-085](S-085-vlc-vst-ffi.md) | vlc-video + obs-vst FFI contract (both ports; VST2 ABI — D-11) | ≈ 2,150 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, clean; no vectors yet) | not run |
| [S-086](S-086-graphics-hook-ffi.md) | win-capture graphics-hook FFI contract (ported to Rust, D-08) | ≈ 7,300 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 4 corrected; no vectors yet) | not run |
| [S-087](S-087-frontend-tools.md) | frontend-tools plugin (auto-scene-switcher, output-timer, captions, scripts UI) | ≈ 2,500 lines | 0 | Draft (2nd-author checked round 10: 8/8 rules re-derived, 1 corrected; no vectors yet) | not run |

Totals: **1,561 scripts / 1,554 committed vectors**, 59 areas, `test/conformance/run.sh check` passes against the C reference.

"Ready for review" means the spec author finished it; it becomes **Ready** only after the
second-author check in PROCESS.md section 4.

### What the sufficiency check is (and is not)

To find holes in the specs, one Rust implementation per area was written **from the spec and
the conformance README only** and run against all vectors:

| Area | Result |
|---|---|
| calldata | 12/12 identical |
| decl | 9/9 (after fixing two spec bugs it exposed: end-of-input after a parameter, and log lines for reference-crash cases) |
| signal + proc | 17/17 identical |
| darray | 4/4 identical |
| deque | 5/5 identical |

Limits: the same person wrote the spec and that check implementation and had read the
C source, so this is **not** a clean-room result; it shows the *documents are sufficient to
drive a correct implementation*, which a real clean-room implementer must still confirm.
These check implementations were throw-away and are not committed (they would not be
clean-room provenance). `bmem` was not implemented in the check.

Known gaps: `run.sh` needs `CONFORMANCE_BUILD_DIR` exported on every call, otherwise it builds
into a new temp directory; run it from `test/conformance`. The SRT/RIST MPEG-TS output gap is
closed by S-059. The remaining spec gaps (threading, obs-websocket, Python backend, Wayland,
audio-monitoring win32/osx/null, win-wasapi, vendor SDKs) are tracked in
[../DECISIONS-NEEDED.md](../DECISIONS-NEEDED.md) section D.

## Backlog (not yet specified)

Most of the original backlog is now written. The rows below that were once "not yet specified"
have been absorbed into a numbered spec; the rest are the genuine gaps, each with a decision
owner in [../DECISIONS-NEEDED.md](../DECISIONS-NEEDED.md) section D.

### Absorbed (now specified)
| Old backlog item | Now in |
|---|---|
| `profiler` (was S-016) | S-024 (M7) |
| platform file/process utilities + UTF-8 (was S-018 / S-019) | S-023 |
| effect/shader language and parsers | S-020 |
| built-in effects | S-053 |
| `gs_*` API semantics | S-043 |
| OpenGL backend contract | S-057 |
| video-io / audio-io threads | S-044 |
| audio mixing and controls | S-032 |
| audio resampler / scaler | S-013, S-044 |
| sources, filters, transitions | S-040 |
| scenes | S-033 |
| encoders / outputs / services | S-041 |
| hotkeys | S-034 |
| `obs_core` start-up, canvas, view, display | S-042, S-043 |
| module loading and locale | S-035 |
| FLV/RTMP, MP4 wire formats | S-021, S-031, S-022 (WebSocket protocol is gap S-062) |
| frontend CLI / INI / API catalog | S-048 |
| per-plugin specs (≈ 40) | S-025…S-032, S-038, S-045…S-056 |

### Genuine gaps (no spec yet)
| Suggested ID | Component | Why it is a gap | Blocked task | Decision |
|---|---|---|---|---|
| S-061 | threading primitives (`os_event`, `os_sem`, `pthread_mutex`, `os_set_thread_name`) and time/sleep (was S-017) | P1-05 / P1-06 would read the C | P1-05, P1-06 | D-27 |
| S-062 | `obs-websocket` protocol | most-integrated third-party plugin; no S-xxx | P5-20 | D-28 |
| S-063 | Python scripting backend (25 `pyscript` vectors exist; S-039/S-060 cover only the core and Lua) | P6-37.2 would read the C | P6-37.2 | D-29 |
| S-064 | Wayland hotkey platform (`obs-nix-wayland.c`, 42 KB: `wl_keyboard` + xkbcommon; no portal code — that is the pipewire/dbus glue in `platform-nix-*.c`) | S-058 explicitly excludes it | P4-32.2 | D-30 |
| S-065 | audio-monitoring win32 / osx / null backends | S-055 covers PulseAudio only | P2-15.3, P2-15.4 | D-31 |
| S-066 | `win-wasapi` enumeration/capture | S-051 covers window matching/hook/vqueue, not WASAPI | P5-30 | D-32 |
| — | vendor-SDK behavioural tables (obs-nvenc helpers, coreaudio-encoder, nv-filters, vlc-video, decklink, aja) | mostly "kept as FFI" per D-04/D-10 | P5-38…P5-43 | D-33 |

**Round 6 (2026-11) — full spec-coverage gap.** The user decision: *every behaviour in a spec, not
only the tested ones*. The complete gap — every first-party non-trivial file no spec cites
(748 files / 7,975 functions at 33.0.1, incl. the rows above, the vendor-SDK FFI surfaces and the
whole frontend) — is tracked in [BACKLOG.md](BACKLOG.md): 23 new specs (S-061…S-063 from round 5,
S-068…S-087 incl. the FFI-surface contracts S-081…S-086 that supersede the D-33 "only if ported"
deferral for the kept-C surfaces) + 18 extensions of existing specs, scheduled as subtasks
**P1-34, P3-32, P5-51, P6-50** plus the round-5 spec subtasks (P1-05.5, P2-15.6, P4-32.6,
P5-20.12, P5-30.9, P6-37.6, P6-07.9).
Done criterion: `S2 = 1` for every first-party non-trivial file in `function-inventory.csv`
(`python3 tools/spec-backlog.py` fails while any file has no action).

## Scale — an honest estimate

The 56 specs above cover about **168,640 of 313,755 non-frontend lines (53.7 %)**, measured by
`tools/coverage.py` against libobs 33.0.1 (target ≥ 85 %; the remaining ~98k lines are the gaps
above plus platform/vendor-SDK code that is kept as FFI by decision — since round 6 the kept-C
FFI surfaces are specified as contracts S-081…S-086 rather than left unspecified). The
Tier C specs are partial. The frontend (~109k lines) is deliberately specified as behavioural
tables (S-048) rather than text + vectors: UI-heavy and hardware-dependent parts (preview,
properties view, capture plugins, encoders with vendor SDKs) need screenshot or
device-in-the-loop acceptance tests, and some will remain judgement calls. The remaining
non-frontend gaps are larger and less self-contained (threads, GPU state, network protocols),
so the effort per line will be higher than for this first, deliberately well-contained tranche.
Plan for the spec effort to be a large fraction of the total project, run in parallel with the
first implementations rather than before them.
