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

Totals: **1,309 scripts / 1,309 committed vectors**, 49 areas, `test/conformance/run.sh check` passes against the C reference.

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

Known gaps: the native SRT/RIST MPEG-TS output (`obs-ffmpeg-mpegts.c`) needs its own spec; `run.sh` needs `CONFORMANCE_BUILD_DIR` exported on every call, otherwise it builds into a new temp directory; run it from `test/conformance`.

## Backlog (not yet specified)

Sizes are lines of C/C++ in the reference (rough measure of spec effort). Order follows the
phase plan. **Nothing below exists yet.**

### Phase 1 – leaf libraries (remaining)
| Next ID | Component | C lines |
|---|---|---|
| S-016 | `profiler` | 1,100 |
| S-017 | threading primitives (`os_event`, `os_sem`, …) and time/sleep | 600 + part of 2,200 |
| S-018 | platform file/process utilities (`os_fopen`, `os_mkdirs`, `os_glob`, `os_fread_utf8`, `os_dtostr`, `os_strtod`, `os_safe_replace`, pipes, paths) | ≈ 2,200 |
| S-019 | UTF-8 / wide conversion (`utf8.c`) | small |

### Phase 2 – media (remaining)
IDs are assigned when work starts (S-020 onward): video-io and audio-io threads (1,100 lines);
audio mixing and controls (1,600); audio resampler/scaler wrappers; audio-monitoring backends.

### Phase 3 – graphics
Effect/shader language and parsers (3,200); `gs_*` API semantics (3,300 + headers); backend contract
(`gs_exports`, ≈176 functions); built-in effects.

### Phase 4 – core
Module loading and locale; sources, filters, transitions (7,600); scenes (4,100); encoders/outputs/
services (5,800); hotkeys (1,400); `obs_core` start-up and video/audio reset (3,500); canvases, views, displays.

### Phases 5–7
One spec per plugin (≈ 40), each with: registered ids, settings keys and defaults, behaviours,
wire formats where applicable (e.g. FLV/RTMP, MP4 boxes, WebSocket protocol), locale keys.
Frontend: file formats, CLI flags, frontend API (99 functions, 44 events), UI behaviour tables.

## Scale — an honest estimate

The forty-eight specs above cover about **165k of ≈460k lines** (≈ 36 %), though the Tier C ones are partial, most of Phase 1 and the pure parts of Phase 2. The remaining specs are
larger and less self-contained (threads, GPU state, network protocols, UI), so the effort per
line will be higher than for this first, deliberately well-contained tranche. UI-heavy and
hardware-dependent parts (preview, properties view, capture plugins, encoders with vendor SDKs)
cannot be fully specified as text + vectors; they need *behavioural* specs with screenshot
or device-in-the-loop acceptance tests, and some will remain judgement calls. Plan for the
spec effort to be a large fraction of the total project, run in parallel with the first
implementations rather than before them.
