# Spec-coverage backlog (round 6, 2026-11)

Goal: **every behaviour in the C reference is pinned by a spec** — extend an existing spec or write a new one. Measured from `function-inventory.csv`: first-party, non-trivial functions whose file no spec cites (`S2 = 0`), 33.0.1@65c320b46: **308 files / 3189 functions**.

New spec IDs continue the series (S-061…S-066 = the DECISIONS-NEEDED backlog written by round-5 subtasks, S-067 = preview interaction from P6-07.9). Regenerate this file with `python3 tools/spec-backlog.py --write`; it fails if a file has no action.

## Actions

| Action | Kind | Task | Title | files | fns | lines |
|---|---|---|---|---|---|---|
| E-S001-S011 | extend | P1-34.1 | libobs container/math/lexer header inlines + core glue | 32 | 361 | 4788 |
| E-S013-S010-S023-S024 | extend | P1-34.2 | media conversion, serializers, platform/util, small helpers | 22 | 241 | 5392 |
| E-S041-S042-S035 | extend | P1-34.3 | output, core remainder, module loader | 10 | 233 | 5448 |
| E-S027-S028-S054 | extend | P5-51.1 | obs-filters: 13 video filters, audio filters, RNNoise contract | 21 | 230 | 5967 |
| E-S057 | extend | P3-32.3 | OpenGL backend file citations + uncited helpers | 11 | 212 | 3659 |
| S-084 | new | P5-51.18 | aja FFI contract (aja, output-ui) | 13 | 210 | 6929 |
| S-079 | new | P3-32.2 | Metal backend | 21 | 184 | 3907 |
| S-083 | new | P5-51.17 | decklink FFI contract (decklink, output-ui, captions) | 23 | 153 | 3046 |
| S-082 | new | P5-51.16 | obs-browser / CEF FFI contract (CEF stays, D-09) | 21 | 149 | 4149 |
| S-070 | new | P6-50.3 | frontend scene collections, profiles & importers | 11 | 148 | 4980 |
| S-075 | new | P6-50.8 | frontend plugin manager, crash handler & system info | 26 | 134 | 3590 |
| E-S051-dshow | extend | P5-51.13 | win-dshow host: dshow_input, encoders, virtualcam glue | 9 | 124 | 2599 |
| E-S030 | extend | P5-51.3 | image-source: slideshow v1/mk2, image, colour source | 4 | 119 | 2182 |
| E-S046-S047-S037 | extend | P5-51.5 | obs-ffmpeg: source, encoders (nvenc/openh264/amf), logging, mux remux | 7 | 95 | 1955 |
| E-S029 | extend | P5-51.2 | obs-transitions: stinger, luma-wipe, fade-to-color, slide, swipe, fade, cut | 7 | 92 | 1467 |
| E-S038 | extend | P5-51.4 | text: freetype2 (incl. find-font per OS) + GDI text source (obs-text) | 10 | 91 | 2649 |
| S-085 | new | P5-51.19 | vlc-video + obs-vst FFI contract (D-10) | 11 | 86 | 2126 |
| S-051-wasapi | extend | P5-30.9 | S-051 §5.G WASAPI enumeration/capture gap (round 5, D-32) | 4 | 60 | 1428 |
| E-S032-S065 | extend | P1-34.4 | audio controls + audio-monitoring backends | 4 | 54 | 1025 |
| E-S049 | extend | P5-51.8 | Linux capture + audio backends: v4l2, capture, alsa, pulseaudio, sndio, oss, jack | 10 | 37 | 938 |
| S-061 | new | P1-05.5 | threading primitives (round 5, DECISIONS D-27) | 4 | 33 | 426 |
| S-063 | new | P6-37.6 | Python scripting backend (round 5, D-29) | 4 | 31 | 634 |
| S-080 | new | P5-51.9 | obs-webrtc / WHIP output | 4 | 29 | 894 |
| E-S041-outputs | extend | P5-51.10 | obs-outputs plugin: null output, module glue | 3 | 16 | 135 |
| E-S026 | extend | P5-51.7 | rtmp-services: amazon-ivs, twitch service-specific | 2 | 3 | 17 |

## Documented exclusions

- `test/` — C-reference test harness (test-input plugin, cmocka unit tests, osx/win test hosts) — no port target; exercised by the conformance suite instead. (64 fns / 1153 lines)

## Files per action

### E-S001-S011 — libobs container/math/lexer header inlines + core glue (P1-34.1)

- `libobs/util/cf-lexer.c` (55 fns / 1182 lines)
- `libobs/util/darray.h` (26 fns / 336 lines)
- `libobs/graphics/vec3.h` (20 fns / 119 lines)
- `libobs/util/cf-parser.h` (18 fns / 183 lines)
- `libobs/util/dstr.h` (18 fns / 130 lines)
- `libobs/graphics/bounds.c` (17 fns / 275 lines)
- `libobs/callback/signal.c` (15 fns / 308 lines)
- `libobs/util/deque.h` (15 fns / 255 lines)
- `libobs/graphics/vec4.h` (14 fns / 104 lines)
- `libobs/util/lexer.h` (13 fns / 83 lines)
- `libobs/graphics/shader-parser.h` (12 fns / 139 lines)
- `libobs/graphics/effect-parser.h` (11 fns / 113 lines)
- `libobs/graphics/matrix3.c` (11 fns / 105 lines)
- `libobs/util/lexer.c` (11 fns / 269 lines)
- `libobs/callback/calldata.c` (10 fns / 184 lines)
- `libobs/callback/decl.c` (8 fns / 188 lines)
- `libobs/graphics/quat.h` (8 fns / 51 lines)
- `libobs/graphics/vec2.h` (8 fns / 41 lines)
- `libobs/util/util.hpp` (8 fns / 47 lines)
- `libobs/graphics/math-extra.c` (7 fns / 102 lines)
- `libobs/graphics/plane.c` (7 fns / 124 lines)
- `libobs/util/base.c` (7 fns / 85 lines)
- `libobs/util/bmem.c` (7 fns / 100 lines)
- `libobs/graphics/bounds.h` (6 fns / 30 lines)
- `libobs/graphics/matrix3.h` (5 fns / 34 lines)
- `libobs/graphics/matrix4.h` (5 fns / 37 lines)
- `libobs/util/bmem.h` (5 fns / 43 lines)
- `libobs/graphics/plane.h` (4 fns / 26 lines)
- `libobs/util/cf-lexer.h` (4 fns / 24 lines)
- `libobs/graphics/axisang.h` (3 fns / 21 lines)
- `libobs/util/cf-parser.c` (2 fns / 41 lines)
- `libobs/graphics/vec2.c` (1 fns / 9 lines)

### E-S013-S010-S023-S024 — media conversion, serializers, platform/util, small helpers (P1-34.2)

- `libobs/util/platform-windows.c` (67 fns / 1175 lines)
- `libobs/util/platform.c` (37 fns / 769 lines)
- `shared/file-updater/file-updater/file-updater.c` (22 fns / 473 lines)
- `shared/happy-eyeballs/happy-eyeballs.c` (15 fns / 524 lines)
- `libobs/util/serializer.h` (13 fns / 75 lines)
- `shared/bpm/bpm.c` (12 fns / 580 lines)
- `libobs/util/text-lookup.c` (11 fns / 222 lines)
- `libobs/util/file-serializer.c` (9 fns / 137 lines)
- `libobs/media-io/media-remux.c` (8 fns / 226 lines)
- `libobs/media-io/video-scaler-ffmpeg.c` (7 fns / 230 lines)
- `libobs/media-io/format-conversion.c` (6 fns / 204 lines)
- `libobs/util/buffered-file-serializer.c` (6 fns / 315 lines)
- `libobs/media-io/audio-resampler-ffmpeg.c` (5 fns / 167 lines)
- `libobs/util/array-serializer.c` (5 fns / 65 lines)
- `libobs/util/utf8.c` (5 fns / 39 lines)
- `libobs/util/bitstream.c` (4 fns / 38 lines)
- `shared/opts-parser/opts-parser.c` (3 fns / 71 lines)
- `libobs/util/windows/obfuscate.c` (2 fns / 26 lines)
- `libobs/util/profiler.hpp` (1 fns / 9 lines)
- `libobs/util/windows/WinHandle.hpp` (1 fns / 6 lines)
- `libobs/util/windows/device-enum.c` (1 fns / 25 lines)
- `libobs/util/windows/win-version.h` (1 fns / 16 lines)

### E-S026 — rtmp-services: amazon-ivs, twitch service-specific (P5-51.7)

- `plugins/rtmp-services/service-specific/amazon-ivs.c` (2 fns / 11 lines)
- `plugins/rtmp-services/service-specific/twitch.c` (1 fns / 6 lines)

### E-S027-S028-S054 — obs-filters: 13 video filters, audio filters, RNNoise contract (P5-51.1)

- `plugins/obs-filters/chroma-key-filter.c` (19 fns / 381 lines)
- `plugins/obs-filters/color-key-filter.c` (19 fns / 343 lines)
- `plugins/obs-filters/mask-filter.c` (19 fns / 311 lines)
- `plugins/obs-filters/rnnoise/src/denoise.c` (19 fns / 490 lines)
- `plugins/obs-filters/color-grade-filter.c` (15 fns / 433 lines)
- `plugins/obs-filters/gpu-delay.c` (15 fns / 302 lines)
- `plugins/obs-filters/scroll-filter.c` (15 fns / 278 lines)
- `plugins/obs-filters/color-correction-filter.c` (14 fns / 554 lines)
- `plugins/obs-filters/crop-filter.c` (13 fns / 241 lines)
- `plugins/obs-filters/scale-filter.c` (13 fns / 495 lines)
- `plugins/obs-filters/rnnoise/src/kiss_fft.c` (12 fns / 505 lines)
- `plugins/obs-filters/luma-key-filter.c` (10 fns / 146 lines)
- `plugins/obs-filters/hdr-tonemap-filter.c` (9 fns / 188 lines)
- `plugins/obs-filters/sdr-on-hdr-filter.c` (9 fns / 131 lines)
- `plugins/obs-filters/rnnoise/src/pitch.c` (7 fns / 472 lines)
- `plugins/obs-filters/sharpness-filter.c` (7 fns / 116 lines)
- `plugins/obs-filters/rnnoise/src/celt_lpc.c` (4 fns / 242 lines)
- `plugins/obs-filters/rnnoise/src/rnn.c` (4 fns / 123 lines)
- `plugins/obs-filters/rnnoise/src/pitch.h` (3 fns / 88 lines)
- `plugins/obs-filters/rnnoise/src/arch.h` (2 fns / 5 lines)
- `plugins/obs-filters/rnnoise/src/rnn_reader.c` (2 fns / 123 lines)

### E-S029 — obs-transitions: stinger, luma-wipe, fade-to-color, slide, swipe, fade, cut (P5-51.2)

- `plugins/obs-transitions/transition-stinger.c` (26 fns / 712 lines)
- `plugins/obs-transitions/transition-fade-to-color.c` (14 fns / 160 lines)
- `plugins/obs-transitions/transition-luma-wipe.c` (13 fns / 171 lines)
- `plugins/obs-transitions/transition-slide.c` (11 fns / 129 lines)
- `plugins/obs-transitions/transition-swipe.c` (11 fns / 125 lines)
- `plugins/obs-transitions/transition-fade.c` (9 fns / 118 lines)
- `plugins/obs-transitions/transition-cut.c` (8 fns / 52 lines)

### E-S030 — image-source: slideshow v1/mk2, image, colour source (P5-51.3)

- `plugins/image-source/obs-slideshow-mk2.c` (44 fns / 910 lines)
- `plugins/image-source/obs-slideshow.c` (40 fns / 865 lines)
- `plugins/image-source/image-source.c` (24 fns / 304 lines)
- `plugins/image-source/color-source.c` (11 fns / 103 lines)

### E-S032-S065 — audio controls + audio-monitoring backends (P1-34.4)

- `libobs/obs-audio-controls.c` (40 fns / 681 lines)
- `libobs/audio-monitoring/osx/coreaudio-enum-devices.c` (9 fns / 199 lines)
- `libobs/audio-monitoring/win32/wasapi-enum-devices.c` (4 fns / 136 lines)
- `libobs/audio-monitoring/osx/mac-helpers.h` (1 fns / 9 lines)

### E-S038 — text: freetype2 (incl. find-font per OS) + GDI text source (obs-text) (P5-51.4)

- `plugins/obs-text/gdiplus/obs-text.cpp` (27 fns / 803 lines)
- `plugins/text-freetype2/text-freetype2.c` (17 fns / 488 lines)
- `plugins/text-freetype2/text-functionality.c` (15 fns / 484 lines)
- `plugins/text-freetype2/find-font.c` (11 fns / 373 lines)
- `plugins/text-freetype2/find-font-cocoa.m` (7 fns / 111 lines)
- `plugins/text-freetype2/find-font-windows.c` (7 fns / 196 lines)
- `plugins/text-freetype2/find-font-iconv.c` (3 fns / 77 lines)
- `plugins/text-freetype2/obs-convenience.c` (2 fns / 71 lines)
- `plugins/text-freetype2/find-font-unix.c` (1 fns / 40 lines)
- `plugins/text-freetype2/find-font.h` (1 fns / 6 lines)

### E-S041-S042-S035 — output, core remainder, module loader (P1-34.3)

- `libobs/obs-output.c` (146 fns / 3043 lines)
- `libobs/obs-cocoa.m` (26 fns / 548 lines)
- `libobs/obs-windows.c` (25 fns / 1137 lines)
- `libobs/obs-win-crash-handler.c` (16 fns / 373 lines)
- `libobs/obs-output-delay.c` (10 fns / 160 lines)
- `libobs/obs.hpp` (5 fns / 43 lines)
- `libobs/obs-core-modules-loader-mac.m` (2 fns / 66 lines)
- `libobs/obs-core-modules-loader-nix.c` (1 fns / 33 lines)
- `libobs/obs-core-modules-loader-win.c` (1 fns / 37 lines)
- `libobs/obs-core-modules.c.in` (1 fns / 8 lines)

### E-S041-outputs — obs-outputs plugin: null output, module glue (P5-51.10)

- `plugins/obs-outputs/null-output.c` (7 fns / 53 lines)
- `plugins/obs-outputs/obs-outputs.c` (6 fns / 52 lines)
- `plugins/obs-outputs/utils.h` (3 fns / 30 lines)

### E-S046-S047-S037 — obs-ffmpeg: source, encoders (nvenc/openh264/amf), logging, mux remux (P5-51.5)

- `plugins/obs-ffmpeg/obs-ffmpeg-source.c` (38 fns / 668 lines)
- `plugins/obs-ffmpeg/obs-ffmpeg-audio-encoders.c` (19 fns / 339 lines)
- `plugins/obs-ffmpeg/obs-ffmpeg-openh264.c` (11 fns / 182 lines)
- `plugins/obs-ffmpeg/obs-ffmpeg.c` (11 fns / 251 lines)
- `plugins/obs-ffmpeg/obs-ffmpeg-video-encoders.c` (6 fns / 260 lines)
- `plugins/obs-ffmpeg/obs-amf-test/obs-amf-test.cpp` (5 fns / 138 lines)
- `plugins/obs-ffmpeg/obs-ffmpeg-logging.c` (5 fns / 117 lines)

### E-S049 — Linux capture + audio backends: v4l2, capture, alsa, pulseaudio, sndio, oss, jack (P5-51.8)

- `plugins/oss-audio/oss-input.c` (17 fns / 549 lines)
- `plugins/sndio/sndio-input.c` (10 fns / 294 lines)
- `plugins/linux-capture/linux-capture.c` (2 fns / 17 lines)
- `plugins/linux-v4l2/v4l2-helpers.h` (2 fns / 40 lines)
- `plugins/linux-alsa/linux-alsa.c` (1 fns / 5 lines)
- `plugins/linux-jack/linux-jack.c` (1 fns / 5 lines)
- `plugins/linux-pulseaudio/linux-pulseaudio.c` (1 fns / 6 lines)
- `plugins/linux-v4l2/linux-v4l2.c` (1 fns / 12 lines)
- `plugins/oss-audio/oss-audio.c` (1 fns / 5 lines)
- `plugins/sndio/sndio.c` (1 fns / 5 lines)

### E-S051-dshow — win-dshow host: dshow_input, encoders, virtualcam glue (P5-51.13)

- `plugins/win-dshow/win-dshow.cpp` (64 fns / 1463 lines)
- `plugins/win-dshow/virtualcam-module/virtualcam-module.cpp` (14 fns / 218 lines)
- `plugins/win-dshow/virtualcam-module/virtualcam-filter.cpp` (12 fns / 325 lines)
- `plugins/win-dshow/win-dshow-encoder.cpp` (12 fns / 225 lines)
- `plugins/win-dshow/virtualcam.c` (7 fns / 92 lines)
- `plugins/win-dshow/encode-dstr.hpp` (5 fns / 68 lines)
- `plugins/win-dshow/virtualcam-module/placeholder.cpp` (5 fns / 135 lines)
- `plugins/win-dshow/virtualcam-module/sleepto.c` (3 fns / 41 lines)
- `plugins/win-dshow/dshow-plugin.cpp` (2 fns / 32 lines)

### E-S057 — OpenGL backend file citations + uncited helpers (P3-32.3)

- `libobs-opengl/gl-subsystem.c` (85 fns / 1415 lines)
- `libobs-opengl/gl-x11-egl.c` (31 fns / 497 lines)
- `libobs-opengl/gl-helpers.h` (23 fns / 167 lines)
- `libobs-opengl/gl-egl-common.c` (19 fns / 489 lines)
- `libobs-opengl/gl-subsystem.h` (15 fns / 380 lines)
- `libobs-opengl/gl-nix.c` (12 fns / 86 lines)
- `libobs-opengl/gl-texture2d.c` (11 fns / 233 lines)
- `libobs-opengl/gl-helpers.c` (5 fns / 129 lines)
- `libobs-opengl/gl-texturecube.c` (5 fns / 92 lines)
- `libobs-opengl/gl-texture3d.c` (4 fns / 142 lines)
- `libobs-opengl/gl-shaderparser.h` (2 fns / 29 lines)

### S-051-wasapi — S-051 §5.G WASAPI enumeration/capture gap (round 5, D-32) (P5-30.9)

- `plugins/win-wasapi/win-wasapi.cpp` (46 fns / 1217 lines)
- `plugins/win-wasapi/wasapi-notify.cpp` (8 fns / 77 lines)
- `plugins/win-wasapi/enum-wasapi.cpp` (3 fns / 85 lines)
- `plugins/win-wasapi/plugin-main.cpp` (3 fns / 49 lines)

### S-061 — threading primitives (round 5, DECISIONS D-27) (P1-05.5)

- `libobs/util/threading-posix.c` (13 fns / 170 lines)
- `libobs/util/threading-windows.c` (12 fns / 151 lines)
- `libobs/util/threading-windows.h` (6 fns / 82 lines)
- `libobs/util/threading.h` (2 fns / 23 lines)

### S-063 — Python scripting backend (round 5, D-29) (P6-37.6)

- `shared/obs-scripting/obs-scripting-python-frontend.c` (22 fns / 387 lines)
- `shared/obs-scripting/obs-scripting-python.h` (6 fns / 59 lines)
- `shared/obs-scripting/obs-scripting-python-import.h` (2 fns / 21 lines)
- `shared/obs-scripting/obs-scripting-python-import.c` (1 fns / 167 lines)

### S-070 — frontend scene collections, profiles & importers (P6-50.3)

- `frontend/widgets/OBSBasic_SceneCollections.cpp` (42 fns / 1557 lines)
- `frontend/widgets/OBSBasic_Profiles.cpp` (30 fns / 1015 lines)
- `frontend/importers/classic.cpp` (15 fns / 521 lines)
- `frontend/importers/sl.cpp` (12 fns / 492 lines)
- `frontend/importers/xsplit.cpp` (12 fns / 497 lines)
- `frontend/importer/OBSImporter.cpp` (8 fns / 204 lines)
- `frontend/importer/ImporterEntryPathItemDelegate.cpp` (7 fns / 123 lines)
- `frontend/importers/importers.cpp` (6 fns / 73 lines)
- `frontend/importers/studio.cpp` (6 fns / 257 lines)
- `frontend/importer/ImporterModel.cpp` (5 fns / 172 lines)
- `frontend/importers/importers.hpp` (5 fns / 69 lines)

### S-075 — frontend plugin manager, crash handler & system info (P6-50.8)

- `frontend/utility/platform-windows.cpp` (19 fns / 356 lines)
- `frontend/utility/platform-osx.mm` (18 fns / 267 lines)
- `frontend/utility/CrashHandler.cpp` (16 fns / 311 lines)
- `frontend/plugin-manager/PluginManager.cpp` (11 fns / 294 lines)
- `frontend/utility/platform-x11.cpp` (9 fns / 186 lines)
- `frontend/utility/system-info-posix.cpp` (8 fns / 591 lines)
- `frontend/plugin-manager/PluginManagerWindow.cpp` (7 fns / 283 lines)
- `frontend/utility/system-info-windows.cpp` (6 fns / 233 lines)
- `frontend/utility/win-dll-blocklist.c` (4 fns / 159 lines)
- `frontend/plugin-manager/InstalledPluginRow.cpp` (3 fns / 141 lines)
- `frontend/plugin-manager/PluginModuleLoader_BSD.cpp` (3 fns / 137 lines)
- `frontend/plugin-manager/PluginModuleLoader_Flatpak.cpp` (3 fns / 77 lines)
- `frontend/plugin-manager/PluginModuleLoader_Linux.cpp` (3 fns / 142 lines)
- `frontend/plugin-manager/PluginModuleLoader_Windows.cpp` (3 fns / 115 lines)
- `frontend/utility/MacUpdateThread.cpp` (3 fns / 53 lines)
- `frontend/plugin-manager/PluginModuleLoader.hpp` (2 fns / 31 lines)
- `frontend/utility/CrashHandler_FreeBSD.cpp` (2 fns / 12 lines)
- `frontend/utility/CrashHandler_Linux.cpp` (2 fns / 12 lines)
- `frontend/utility/CrashHandler_Windows.cpp` (2 fns / 50 lines)
- `frontend/utility/OBSSparkle.mm` (2 fns / 28 lines)
- `frontend/utility/OBSUpdateDelegate.mm` (2 fns / 12 lines)
- `frontend/utility/system-info-macos.mm` (2 fns / 5 lines)
- `frontend/utility/NativeEventFilter.cpp` (1 fns / 5 lines)
- `frontend/utility/NativeEventFilter_Windows.cpp` (1 fns / 38 lines)
- `frontend/utility/crypto-helpers-mac.mm` (1 fns / 22 lines)
- `frontend/utility/crypto-helpers-mbedtls.cpp` (1 fns / 30 lines)

### S-079 — Metal backend (P3-32.2)

- `libobs-metal/metal-subsystem.swift` (38 fns / 471 lines)
- `libobs-metal/metal-shader.swift` (24 fns / 324 lines)
- `libobs-metal/OBSShader.swift` (20 fns / 1293 lines)
- `libobs-metal/metal-texture2d.swift` (19 fns / 259 lines)
- `libobs-metal/MetalDevice.swift` (16 fns / 556 lines)
- `libobs-metal/MetalTexture.swift` (9 fns / 202 lines)
- `libobs-metal/metal-swapchain.swift` (8 fns / 173 lines)
- `libobs-metal/metal-indexbuffer.swift` (6 fns / 54 lines)
- `libobs-metal/MetalBuffer.swift` (5 fns / 148 lines)
- `libobs-metal/MetalShader.swift` (5 fns / 99 lines)
- `libobs-metal/OBSSwapChain.swift` (5 fns / 37 lines)
- `libobs-metal/libobs+Extensions.swift` (5 fns / 56 lines)
- `libobs-metal/metal-stagesurf.swift` (5 fns / 45 lines)
- `libobs-metal/metal-texture3d.swift` (5 fns / 46 lines)
- `libobs-metal/metal-vertexbuffer.swift` (4 fns / 32 lines)
- `libobs-metal/MTLTexture+Extensions.swift` (2 fns / 10 lines)
- `libobs-metal/MetalStageBuffer.swift` (2 fns / 10 lines)
- `libobs-metal/metal-samplerstate.swift` (2 fns / 54 lines)
- `libobs-metal/metal-zstencilbuffer.swift` (2 fns / 26 lines)
- `libobs-metal/libobs+SignalHandlers.swift` (1 fns / 7 lines)
- `libobs-metal/metal-unimplemented.swift` (1 fns / 5 lines)

### S-080 — obs-webrtc / WHIP output (P5-51.9)

- `plugins/obs-webrtc/whip-output.cpp` (16 fns / 718 lines)
- `plugins/obs-webrtc/whip-utils.h` (7 fns / 90 lines)
- `plugins/obs-webrtc/whip-service.cpp` (5 fns / 79 lines)
- `plugins/obs-webrtc/obs-webrtc.cpp` (1 fns / 7 lines)

### S-082 — obs-browser / CEF FFI contract (CEF stays, D-09) (P5-51.16)

- `plugins/obs-browser/panel/browser-panel.cpp` (25 fns / 493 lines)
- `plugins/obs-browser/browser-client.cpp` (23 fns / 596 lines)
- `plugins/obs-browser/obs-browser-source.cpp` (23 fns / 609 lines)
- `plugins/obs-browser/obs-browser-plugin.cpp` (22 fns / 756 lines)
- `plugins/obs-browser/panel/browser-panel-client.cpp` (16 fns / 439 lines)
- `plugins/obs-browser/browser-app.cpp` (13 fns / 398 lines)
- `plugins/obs-browser/browser-dummy-client.cpp` (3 fns / 21 lines)
- `plugins/obs-browser/obs-browser-page/obs-browser-page-main.cpp` (3 fns / 93 lines)
- `plugins/obs-browser/panel/browser-panel.hpp` (3 fns / 53 lines)
- `plugins/obs-browser/deps/base64/base64.cpp` (2 fns / 82 lines)
- `plugins/obs-browser/deps/ip-string-posix.cpp` (2 fns / 12 lines)
- `plugins/obs-browser/deps/ip-string-windows.cpp` (2 fns / 12 lines)
- `plugins/obs-browser/deps/signal-restore.cpp` (2 fns / 15 lines)
- `plugins/obs-browser/deps/wide-string.cpp` (2 fns / 35 lines)
- `plugins/obs-browser/drm-format.cpp` (2 fns / 36 lines)
- `plugins/obs-browser/browser-app.hpp` (1 fns / 8 lines)
- `plugins/obs-browser/browser-client.hpp` (1 fns / 8 lines)
- `plugins/obs-browser/browser-scheme.cpp` (1 fns / 61 lines)
- `plugins/obs-browser/linux-keyboard-helpers.hpp` (1 fns / 400 lines)
- `plugins/obs-browser/obs-browser-source.hpp` (1 fns / 16 lines)
- `plugins/obs-browser/panel/browser-panel-client.hpp` (1 fns / 6 lines)

### S-083 — decklink FFI contract (decklink, output-ui, captions) (P5-51.17)

- `plugins/decklink/decklink-device-instance.cpp` (24 fns / 754 lines)
- `plugins/decklink-output-ui/decklink-ui-main.cpp` (15 fns / 440 lines)
- `plugins/decklink/OBSVideoFrame.cpp` (14 fns / 173 lines)
- `plugins/decklink/decklink-output.cpp` (11 fns / 265 lines)
- `plugins/decklink/decklink-source.cpp` (11 fns / 259 lines)
- `plugins/decklink-output-ui/DecklinkOutputUI.cpp` (10 fns / 120 lines)
- `plugins/decklink-captions/decklink-captions.cpp` (8 fns / 119 lines)
- `plugins/decklink/decklink-device-discovery.cpp` (8 fns / 118 lines)
- `plugins/decklink/decklink-device-mode.cpp` (7 fns / 65 lines)
- `plugins/decklink/DecklinkInput.cpp` (6 fns / 133 lines)
- `plugins/decklink/DecklinkOutput.cpp` (6 fns / 79 lines)
- `plugins/decklink/decklink-device.cpp` (6 fns / 164 lines)
- `plugins/decklink/OBSVideoFrame.h` (5 fns / 43 lines)
- `plugins/decklink/audio-repack.c` (5 fns / 77 lines)
- `plugins/decklink/win/platform.cpp` (4 fns / 34 lines)
- `plugins/decklink/decklink-device-instance.hpp` (3 fns / 38 lines)
- `plugins/decklink/decklink-device-discovery.hpp` (2 fns / 29 lines)
- `plugins/decklink/plugin-main.cpp` (2 fns / 47 lines)
- `plugins/decklink/util.cpp` (2 fns / 40 lines)
- `plugins/decklink-captions/decklink-captions.h` (1 fns / 12 lines)
- `plugins/decklink/decklink-devices.cpp` (1 fns / 11 lines)
- `plugins/decklink/linux/platform.cpp` (1 fns / 11 lines)
- `plugins/decklink/mac/platform.cpp` (1 fns / 15 lines)

### S-084 — aja FFI contract (aja, output-ui) (P5-51.18)

- `plugins/aja/aja-common.cpp` (40 fns / 1002 lines)
- `plugins/aja/aja-output.cpp` (36 fns / 1041 lines)
- `plugins/aja/aja-source.cpp` (31 fns / 937 lines)
- `plugins/aja/aja-card-manager.cpp` (25 fns / 502 lines)
- `plugins/aja-output-ui/aja-ui-main.cpp` (18 fns / 383 lines)
- `plugins/aja/aja-props.cpp` (15 fns / 199 lines)
- `plugins/aja-output-ui/AJAOutputUI.cpp` (11 fns / 201 lines)
- `plugins/aja/audio-repack.c` (10 fns / 196 lines)
- `plugins/aja/aja-widget-io.cpp` (9 fns / 100 lines)
- `plugins/aja/aja-routing.cpp` (7 fns / 505 lines)
- `plugins/aja/aja-presets.cpp` (4 fns / 1811 lines)
- `plugins/aja/aja-vpid-data.cpp` (2 fns / 26 lines)
- `plugins/aja/main.cpp` (2 fns / 26 lines)

### S-085 — vlc-video + obs-vst FFI contract (D-10) (P5-51.19)

- `plugins/vlc-video/vlc-video-source.c` (41 fns / 1013 lines)
- `plugins/obs-vst/VSTPlugin.cpp` (16 fns / 371 lines)
- `plugins/obs-vst/obs-vst.cpp` (13 fns / 299 lines)
- `plugins/vlc-video/vlc-video-plugin.c` (5 fns / 158 lines)
- `plugins/obs-vst/linux/VSTPlugin-linux.cpp` (2 fns / 42 lines)
- `plugins/obs-vst/mac/VSTPlugin-osx.mm` (2 fns / 67 lines)
- `plugins/obs-vst/win/EditorWidget-win.cpp` (2 fns / 68 lines)
- `plugins/obs-vst/win/VSTPlugin-win.cpp` (2 fns / 63 lines)
- `plugins/obs-vst/EditorWidget.cpp` (1 fns / 5 lines)
- `plugins/obs-vst/linux/EditorWidget-linux.cpp` (1 fns / 11 lines)
- `plugins/obs-vst/mac/EditorWidget-osx.mm` (1 fns / 29 lines)

