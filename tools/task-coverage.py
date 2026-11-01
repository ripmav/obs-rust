#!/usr/bin/env python3
"""Map every source file of the OBS Studio checkout to a rewrite task.

Usage (from the repository root):
    python3 tools/task-coverage.py [--src ../obs-studio] [--write]

`--src` is the OBS Studio checkout (read-only). `--write` regenerates
docs/rust-rewrite/coverage/coverage-matrix.md. Without `--write` the script only
prints a summary and lists unmapped files.

A file is "tracked" when it has a source extension (.c .cpp .cc .h .hpp .m .mm .swift),
is a Qt form (.ui) or an effect file (.effect). Every tracked file must match a rule;
anything unmatched is a coverage gap and fails the run (exit 1). All other files
(CMake, locale, images, config templates) are build/data and are summarised per area.

Rules are (glob, task, spec, note); first match wins. Keep them in dependency order.
Task IDs refer to docs/rust-rewrite/phase-N-*.md; spec IDs to docs/rust-rewrite/specs/.
"""
import argparse
import fnmatch
import os
import re
import subprocess

SRC_EXT = (".c", ".cpp", ".cc", ".h", ".hpp", ".m", ".mm", ".swift")
TRACKED_EXT = SRC_EXT + (".ui", ".effect")

# (glob, task, spec, note)
RULES = [
    # ---------- libobs root ----------
    ("libobs/obs.c", "P4-03", "S-042", ""),
    ("libobs/obs-internal.h", "P4-01, P4-04", "S-042", ""),
    ("libobs/obs-defs.h", "P4-03", "S-042", ""),
    ("libobs/obs-config.h", "P0-02", "", "frozen ABI header; version semantics"),
    ("libobs/obsversion.h", "P7-23", "", "bumped at release time"),
    ("libobs/obs.h", "P4-34", "", "C ABI surface"),
    ("libobs/obs-ffmpeg-compat.h", "P5-11", "", ""),
    ("libobs/obs-data.*", "P1-25", "S-014", ""),
    ("libobs/obs-properties.*", "P1-27", "S-015", ""),
    ("libobs/obs-audio.c", "P2-13", "S-032", ""),
    ("libobs/obs-audio-controls.*", "P2-14", "S-032", ""),
    ("libobs/obs-avc.*", "P2-18", "S-012", ""),
    ("libobs/obs-hevc.*", "P2-19", "S-012", ""),
    ("libobs/obs-av1.*", "P2-20", "S-012", ""),
    ("libobs/obs-nal.*", "P2-17", "S-012", ""),
    ("libobs/obs-canvas.c", "P4-33", "S-042", ""),
    ("libobs/obs-core-modules*.h", "P4-29", "S-035", ""),
    ("libobs/obs-core-modules*.c", "P4-29", "S-035", ""),
    ("libobs/obs-core-modules-loader-mac.m", "P4-29", "S-035", ""),
    ("libobs/obs-cocoa.m", "P4-32.4", "", "macOS platform glue (objc2)"),
    ("libobs/obs-hotkey-name-map.c", "P4-28", "S-034", ""),
    ("libobs/obs.hpp", "P4-34", "", "C++ wrapper over the frozen C ABI"),
    ("libobs/obs-display.c", "P3-17, P4-31", "S-043", ""),
    ("libobs/obs-encoder.*", "P4-21", "S-041", ""),
    ("libobs/obs-hotkey.*", "P4-28", "S-034", ""),
    ("libobs/obs-hotkeys.h", "P4-28", "S-034", ""),
    ("libobs/obs-interaction.h", "P4-14", "S-040", ""),
    ("libobs/obs-missing-files.*", "P4-15", "S-024", ""),
    ("libobs/obs-module.*", "P4-29", "S-035", ""),
    ("libobs/obs-nix-platform.*", "P4-32.5", "S-058", ""),
    ("libobs/obs-nix-x11.*", "P4-32.1", "S-058", ""),
    ("libobs/obs-nix-wayland.*", "P4-32.2", "D-30", "spec gap: S-058 excludes it"),
    ("libobs/obs-nix.*", "P4-32", "S-058", ""),
    ("libobs/obs-output-delay.c", "P2-23", "S-041", ""),
    ("libobs/obs-output.*", "P4-23", "S-041", ""),
    ("libobs/obs-scene.*", "P4-16", "S-033", ""),
    ("libobs/obs-service.*", "P4-25", "S-035", ""),
    ("libobs/obs-source-deinterlace.c", "P4-07.3", "S-040", ""),
    ("libobs/obs-source-transition.c", "P4-11", "S-040", ""),
    ("libobs/obs-source.*", "P4-05", "S-040", ""),
    ("libobs/obs-video-gpu-encode.c", "P3-26.4", "S-058", ""),
    ("libobs/obs-video.c", "P2-10, P3-26", "S-058", ""),
    ("libobs/obs-view.c", "P4-31", "S-042", ""),
    ("libobs/obs-win-crash-handler.c", "P4-32.3", "", ""),
    ("libobs/obs-windows.c", "P4-32.3", "", ""),
    # ---------- libobs/callback ----------
    ("libobs/callback/calldata.*", "P1-20", "S-002", ""),
    ("libobs/callback/decl.*", "P1-21", "S-003", ""),
    ("libobs/callback/proc.*", "P1-21", "S-004", ""),
    ("libobs/callback/signal.*", "P1-22", "S-004", ""),
    # ---------- libobs/data ----------
    ("libobs/data/*.effect", "P3-24", "S-053", ""),
    # ---------- libobs/graphics ----------
    ("libobs/graphics/vec2.*", "P1-15", "S-009", ""),
    ("libobs/graphics/vec3.*", "P1-15", "S-008", ""),
    ("libobs/graphics/vec4.*", "P1-15", "S-008", ""),
    ("libobs/graphics/matrix3.*", "P1-16", "S-009", ""),
    ("libobs/graphics/matrix4.*", "P1-16", "S-008", ""),
    ("libobs/graphics/quat.*", "P1-17", "S-008", ""),
    ("libobs/graphics/axisang.*", "P1-17", "S-008", ""),
    ("libobs/graphics/plane.*", "P1-17", "S-009", ""),
    ("libobs/graphics/bounds.*", "P1-17", "S-009", ""),
    ("libobs/graphics/math-extra.*", "P1-18", "S-009", ""),
    ("libobs/graphics/half.h", "P1-18", "S-009", ""),
    ("libobs/graphics/srgb.h", "P1-18", "S-009", ""),
    ("libobs/graphics/basemath.hpp", "P1-15", "S-008/S-009", "shared defs"),
    ("libobs/graphics/math-defs.h", "P1-15", "S-008/S-009", "shared defs"),
    ("libobs/graphics/effect-parser.*", "P3-03", "S-020", ""),
    ("libobs/graphics/shader-parser.*", "P3-02", "S-020", ""),
    ("libobs/graphics/effect.*", "P3-04", "S-020", ""),
    ("libobs/graphics/graphics-internal.h", "P3-07", "S-043", ""),
    ("libobs/graphics/graphics-imports.c", "P3-08", "S-043", ""),
    ("libobs/graphics/graphics-ffmpeg.c", "P3-12", "S-053", ""),
    ("libobs/graphics/graphics.*", "P3-08", "S-043", ""),
    ("libobs/graphics/device-exports.h", "P3-07", "S-043", ""),
    ("libobs/graphics/input.h", "P3-07", "S-043", ""),
    ("libobs/graphics/image-file.*", "P3-12", "S-053", ""),
    ("libobs/graphics/texture-render.c", "P3-11", "S-043", ""),
    ("libobs/graphics/libnsgif/*", "P3-12.2", "S-053", ""),
    # ---------- libobs/media-io ----------
    ("libobs/media-io/video-io.h", "P2-01", "S-044", ""),
    ("libobs/media-io/audio-io.h", "P2-01", "S-044", ""),
    ("libobs/media-io/media-io-defs.h", "P2-01", "S-044", ""),
    ("libobs/media-io/video-io.c", "P2-09", "S-044", ""),
    ("libobs/media-io/audio-io.c", "P2-08", "S-044", ""),
    ("libobs/media-io/audio-math.h", "P2-07", "S-013", ""),
    ("libobs/media-io/audio-resampler*", "P2-06", "S-013/S-044", ""),
    ("libobs/media-io/format-conversion.*", "P2-04", "S-013", ""),
    ("libobs/media-io/frame-rate.h", "P2-09.5", "", ""),
    ("libobs/media-io/media-remux.*", "P2-29", "", ""),
    ("libobs/media-io/video-fourcc.c", "P2-01.3", "S-013", ""),
    ("libobs/media-io/video-frame.*", "P2-03", "S-013", ""),
    ("libobs/media-io/video-matrices.c", "P2-02", "S-013", ""),
    ("libobs/media-io/video-scaler*", "P2-03", "S-013", ""),
    # ---------- libobs/audio-monitoring ----------
    ("libobs/audio-monitoring/pulse/*", "P2-15.2", "S-055", ""),
    ("libobs/audio-monitoring/null/*", "P2-15.4", "D-31", "spec gap"),
    ("libobs/audio-monitoring/osx/*", "P2-15.4", "D-31", "spec gap"),
    ("libobs/audio-monitoring/win32/*", "P2-15.3", "D-31", "spec gap"),
    # ---------- libobs/util ----------
    ("libobs/util/apple/*", "P1-07.1", "S-023", ""),
    ("libobs/util/array-serializer.*", "P1-14.2", "S-010", ""),
    ("libobs/util/buffered-file-serializer.*", "P1-14.2", "S-010", ""),
    ("libobs/util/file-serializer.*", "P1-14.2", "S-010", ""),
    ("libobs/util/serializer.h", "P1-14.2", "S-010", ""),
    ("libobs/util/bitstream.*", "P1-14.2", "S-010", ""),
    ("libobs/util/base.*", "P1-13", "S-001", ""),
    ("libobs/util/bmem.*", "P1-01", "S-001", ""),
    ("libobs/util/c99defs.h", "P1-01", "S-001", ""),
    ("libobs/util/cf-lexer.*", "P1-08.3", "S-011", ""),
    ("libobs/util/cf-parser.*", "P1-08.3", "S-011", ""),
    ("libobs/util/config-file.*", "P1-09", "S-007", ""),
    ("libobs/util/crc32.*", "P1-10.2", "S-055", ""),
    ("libobs/util/curl/*", "P1-14.4", "", "FFI until Phase 6"),
    ("libobs/util/darray.h", "P1-03", "S-005", ""),
    ("libobs/util/deque.h", "P1-04", "S-005", ""),
    ("libobs/util/dstr.*", "P1-02", "S-006", ""),
    ("libobs/util/lexer.*", "P1-08.2", "S-011", ""),
    ("libobs/util/pipe*posix*", "P1-11.2", "S-055", ""),
    ("libobs/util/pipe*windows*", "P1-11.2", "S-055", ""),
    ("libobs/util/pipe.*", "P1-11.2", "S-055", ""),
    ("libobs/util/platform-cocoa.m", "P1-07.1", "S-023", ""),
    ("libobs/util/platform-nix-dbus.c", "P1-07.3", "S-055", ""),
    ("libobs/util/platform-nix-portal.c", "P1-07.3", "S-055", ""),
    ("libobs/util/platform-nix.c", "P1-07.1", "S-023", ""),
    ("libobs/util/platform-windows.c", "P1-07.1", "S-023", ""),
    ("libobs/util/platform.*", "P1-06, P1-07", "S-023", ""),
    ("libobs/util/profiler.*", "P1-12", "S-024", ""),
    ("libobs/util/source-profiler.*", "P1-12.3", "S-055", ""),
    ("libobs/util/sse-intrin.h", "P1-19.1", "", ""),
    ("libobs/util/task.*", "P1-14.3", "S-024", ""),
    ("libobs/util/text-lookup.*", "P1-08.4", "S-010", ""),
    ("libobs/util/threading-posix.*", "P1-05", "D-27", "spec gap: threading primitives"),
    ("libobs/util/threading-windows.*", "P1-05", "D-27", "spec gap: threading primitives"),
    ("libobs/util/threading.*", "P1-05", "D-27", "spec gap: threading primitives"),
    ("libobs/util/utf8.*", "P1-08.1", "S-023", ""),
    ("libobs/util/uthash.h", "P1-14.1", "", "replace with Rust collections"),
    ("libobs/util/util.hpp", "P1-14.1", "", ""),
    ("libobs/util/util_uint128.h", "P1-14.3", "S-024", ""),
    ("libobs/util/util_uint64.h", "P1-14.3", "S-024", ""),
    ("libobs/util/windows/*", "P1-14.1", "", "port with P4-32.3/P5-30 as needed"),
    # ---------- graphics backends ----------
    ("libobs-opengl/*", "P3-22.1", "S-057", "kept C fallback until P7-01"),
    ("libobs-d3d11/*", "P3-22.2", "D-13", "kept C fallback until P7-01"),
    ("libobs-metal/*", "P3-22.3", "D-13", "Swift; kept C-level fallback until P7-01"),
    ("libobs-winrt/*", "P5-33", "", ""),
    # ---------- frontend ----------
    ("frontend/OBSApp_Themes.cpp", "P6-03.2", "", ""),
    ("frontend/OBSApp.*", "P6-02", "", ""),
    ("frontend/OBSStudioAPI.*", "P6-36.4", "", ""),
    ("frontend/obs-main.cpp", "P6-01", "S-048", ""),
    ("frontend/api/obs-frontend-api.h", "P6-36.1", "", "99 functions / 44 events"),
    ("frontend/api/obs-frontend-api.cpp", "P6-36", "", ""),
    ("frontend/api/obs-frontend-internal.hpp", "P6-36", "", ""),
    ("frontend/components/AbsoluteSlider.*", "P6-10.2", "", ""),
    ("frontend/components/AccessibleAlignment*.*", "P6-16.6", "", ""),
    ("frontend/components/AlignmentSelector.*", "P6-16.6", "", ""),
    ("frontend/components/*Toolbar.*", "P6-14", "", ""),
    ("frontend/components/BalanceSlider.hpp", "P6-10.2", "", ""),
    ("frontend/components/MediaControls.*", "P6-17", "", ""),
    ("frontend/components/Multiview.*", "P6-07.8", "", ""),
    ("frontend/components/MuteCheckBox.hpp", "P6-10.3", "", ""),
    ("frontend/components/OBSAdvAudioCtrl.*", "P6-10.5", "", ""),
    ("frontend/components/OBSPreviewScaling*.*", "P6-07.5", "", ""),
    ("frontend/components/OBSSourceLabel.*", "P6-09.2", "", ""),
    ("frontend/components/SceneTree.*", "P6-08", "", ""),
    ("frontend/components/SourceSelectButton.*", "P6-16.6", "", ""),
    ("frontend/components/SourceTree*.*", "P6-09", "", ""),
    ("frontend/components/VisibilityItem*.*", "P6-09.2", "", ""),
    ("frontend/components/VolumeAccessibleInterface.*", "P6-10.1, P6-18", "", ""),
    ("frontend/components/Volume*.*", "P6-10", "", ""),
    ("frontend/components/*", "P6-49", "", "shared Qt components (sweep)"),
    ("frontend/dialogs/LogUploadDialog.*", "P6-05.2", "", ""),
    ("frontend/dialogs/NameDialog.*", "P6-08", "", "rename dialog"),
    ("frontend/dialogs/OAuthLogin.*", "P6-29.1", "", ""),
    ("frontend/dialogs/OBSAbout.*", "P6-23.1", "", ""),
    ("frontend/dialogs/OBSBasicAdvAudio.*", "P6-10.5", "", ""),
    ("frontend/dialogs/OBSBasicFilters.*", "P6-16.6", "", ""),
    ("frontend/dialogs/OBSBasicInteraction.*", "P6-16.6", "", ""),
    ("frontend/dialogs/OBSBasicProperties.*", "P6-16.6", "", ""),
    ("frontend/dialogs/OBSBasicSourceSelect.*", "P6-16.6", "", ""),
    ("frontend/dialogs/OBSBasicTransform.*", "P6-16.6", "", ""),
    ("frontend/dialogs/OBSBasicVCamConfig.*", "P6-34", "", ""),
    ("frontend/dialogs/OBSExtraBrowsers.*", "P6-15.2", "", ""),
    ("frontend/dialogs/OBSIdianPlayground.*", "P6-35", "", ""),
    ("frontend/dialogs/OBSLogViewer.*", "P6-05.1", "", ""),
    ("frontend/dialogs/OBSMissingFiles.*", "P6-27", "", ""),
    ("frontend/dialogs/OBSPermissions.*", "P6-23.3", "", ""),
    ("frontend/dialogs/OBSRemux.*", "P6-28", "", ""),
    ("frontend/dialogs/OBSUpdate.*", "P6-32.3", "", ""),
    ("frontend/dialogs/OBSWhatsNew.*", "P6-23.2", "", ""),
    ("frontend/dialogs/OBSYoutubeActions.*", "P6-30.1", "", ""),
    ("frontend/docks/BrowserDock.*", "P6-15.2", "", ""),
    ("frontend/docks/OBSDock.*", "P6-15.1", "", ""),
    ("frontend/docks/YouTube*Dock.*", "P6-30.3", "", ""),
    ("frontend/forms/AutoConfig*.ui", "P6-22", "", ""),
    ("frontend/forms/ColorSelect.ui", "P6-16.4", "", ""),
    ("frontend/forms/LogUploadDialog.ui", "P6-05.2", "", ""),
    ("frontend/forms/OBSAbout.ui", "P6-23.1", "", ""),
    ("frontend/forms/OBSAdvAudio.ui", "P6-10.5", "", ""),
    ("frontend/forms/OBSBasicControls.ui", "P6-12", "", ""),
    ("frontend/forms/OBSBasicFilters.ui", "P6-16.6", "", ""),
    ("frontend/forms/OBSBasicInteraction.ui", "P6-16.6", "", ""),
    ("frontend/forms/OBSBasicProperties.ui", "P6-16.6", "", ""),
    ("frontend/forms/OBSBasicSettings.ui", "P6-19", "", ""),
    ("frontend/forms/OBSBasicSourceSelect.ui", "P6-16.6", "", ""),
    ("frontend/forms/OBSBasicTransform.ui", "P6-16.6", "", ""),
    ("frontend/forms/OBSBasicVCamConfig.ui", "P6-34", "", ""),
    ("frontend/forms/OBSBasic.ui", "P6-06", "", ""),
    ("frontend/forms/OBSExtraBrowsers.ui", "P6-15.2", "", ""),
    ("frontend/forms/OBSIdianPlayground.ui", "P6-35", "", ""),
    ("frontend/forms/OBSImporter.ui", "P6-26", "", ""),
    ("frontend/forms/OBSLogViewer.ui", "P6-05.1", "", ""),
    ("frontend/forms/OBSMissingFiles.ui", "P6-27", "", ""),
    ("frontend/forms/OBSPermissions.ui", "P6-23.3", "", ""),
    ("frontend/forms/OBSRemux.ui", "P6-28", "", ""),
    ("frontend/forms/OBSUpdate.ui", "P6-32.3", "", ""),
    ("frontend/forms/OBSYoutubeActions.ui", "P6-30.1", "", ""),
    ("frontend/forms/PluginManagerWindow.ui", "P6-33", "", ""),
    ("frontend/forms/StatusBarWidget.ui", "P6-06.2", "", ""),
    ("frontend/forms/source-toolbar/*.ui", "P6-14", "", ""),
    ("frontend/importer/*", "P6-26", "", "importer dialog UI (P6-26.6)"),
    ("frontend/importers/classic.cpp", "P6-26.1", "", ""),
    ("frontend/importers/sl.cpp", "P6-26.2", "", ""),
    ("frontend/importers/studio.cpp", "P6-26.3", "", ""),
    ("frontend/importers/xsplit.cpp", "P6-26.4", "", ""),
    ("frontend/importers/*", "P6-26", "", ""),
    ("frontend/models/Rect.*", "P6-16.6", "", ""),
    ("frontend/models/SceneCollection.*", "P6-24", "", ""),
    ("frontend/oauth/AuthListener.*", "P6-29.1", "", ""),
    ("frontend/oauth/OAuth.*", "P6-29.1", "", ""),
    ("frontend/oauth/TwitchAuth.*", "P6-29.3", "", ""),
    ("frontend/oauth/YoutubeAuth.*", "P6-29.4", "", ""),
    ("frontend/oauth/RestreamAuth.*", "P6-29.5", "", ""),
    ("frontend/oauth/Auth.*", "P6-29.1", "", ""),
    ("frontend/plugin-manager/*", "P6-33", "", ""),
    ("frontend/settings/OBSBasicSettings_A11y.cpp", "P6-20.6", "", ""),
    ("frontend/settings/OBSBasicSettings_Appearance.cpp", "P6-20.1", "", ""),
    ("frontend/settings/OBSBasicSettings_Stream.cpp", "P6-20.2", "", ""),
    ("frontend/settings/OBSHotkey*.*", "P6-21", "", ""),
    ("frontend/settings/OBSBasicSettings.*", "P6-19", "", ""),
    ("frontend/updater/*", "P6-32", "", "P6-32.1…P6-32.5"),
    ("frontend/utility/AdvancedOutput.*", "P6-20.4", "", ""),
    ("frontend/utility/SimpleOutput.*", "P6-20.3", "", ""),
    ("frontend/utility/audio-encoders.*", "P6-20.3", "", ""),
    ("frontend/utility/FFmpeg*.*", "P6-20.4", "", ""),
    ("frontend/utility/AutoUpdateThread.*", "P6-32.3", "", ""),
    ("frontend/utility/BasicOutputHandler.*", "P6-12.1", "", ""),
    ("frontend/utility/CrashHandler*.*", "P6-01.4", "", ""),
    ("frontend/utility/MacUpdateThread.*", "P6-32.4", "", ""),
    ("frontend/utility/OBSSparkle.*", "P6-32.4", "", ""),
    ("frontend/utility/OBSUpdateDelegate.*", "P6-32.3", "", ""),
    ("frontend/utility/update-helpers.*", "P6-32.1", "", ""),
    ("frontend/utility/win-dll-blocklist.c", "P6-32.3", "", ""),
    ("frontend/utility/crypto-helpers*.*", "P6-32.2", "", ""),
    ("frontend/utility/GoLiveAPI_*", "P6-31.3", "", ""),
    ("frontend/utility/MultitrackVideo*.*", "P6-31.3", "", ""),
    ("frontend/utility/StartMultiTrackVideoStreamingGuard.hpp", "P6-31.3", "", ""),
    ("frontend/utility/models/branches.hpp", "P6-20.6", "", "update channels"),
    ("frontend/utility/models/multitrack-video.hpp", "P6-31.3", "", ""),
    ("frontend/utility/models/whatsnew.hpp", "P6-23.2", "", ""),
    ("frontend/utility/QuickTransition.*", "P6-11.3", "", ""),
    ("frontend/utility/OBSTheme*.*", "P6-03", "", ""),
    ("frontend/utility/BaseLexer.hpp", "P6-03.1", "", ""),
    ("frontend/utility/OBSEventFilter.hpp", "P6-03.2", "", ""),
    ("frontend/utility/OBSProxyStyle.*", "P6-03.2", "", ""),
    ("frontend/utility/SurfaceEventFilter.hpp", "P6-03.2", "", ""),
    ("frontend/utility/OBSTranslator.*", "P6-04", "", ""),
    ("frontend/utility/MissingFiles*.*", "P6-27", "", ""),
    ("frontend/utility/Remux*.*", "P6-28", "", ""),
    ("frontend/utility/ExtraBrowsers*.*", "P6-15.2", "", ""),
    ("frontend/utility/SceneRenameDelegate.*", "P6-08.2", "", ""),
    ("frontend/utility/NativeEventFilter*.*", "P6-07.6", "", ""),
    ("frontend/utility/PreviewProgramSizeObserver.*", "P6-07.7", "", ""),
    ("frontend/utility/item-widget-helpers.*", "P6-09", "", ""),
    ("frontend/utility/Thumbnail*.*", "P6-24.3", "", ""),
    ("frontend/utility/ScreenshotObj.*", "P6-08.3", "", ""),
    ("frontend/utility/undo_stack.*", "P6-09.3", "", ""),
    ("frontend/utility/platform*.*", "P6-41.3", "", ""),
    ("frontend/utility/display-helpers.hpp", "P6-41.3", "", ""),
    ("frontend/utility/system-info*.*", "P6-41.4", "", ""),
    ("frontend/utility/obf.*", "P6-02.2", "", "stream-key obfuscation"),
    ("frontend/utility/OBSCanvas.*", "P6-06.3", "", ""),
    ("frontend/utility/RemoteTextThread.*", "P6-23.2", "", ""),
    ("frontend/utility/WhatsNew*.*", "P6-23.2", "", ""),
    ("frontend/utility/WHIPSimulcastEncoders.hpp", "P6-20.3", "", ""),
    ("frontend/utility/VCamConfig.hpp", "P6-34", "", ""),
    ("frontend/utility/YoutubeApiWrappers.*", "P6-30.2", "", ""),
    ("frontend/utility/ResizeSignaler.hpp", "P6-07.1", "", ""),
    ("frontend/utility/SettingsEventFilter.hpp", "P6-19.2", "", ""),
    ("frontend/widgets/AudioMixer.*", "P6-10.4", "", ""),
    ("frontend/widgets/ColorSelect.*", "P6-16.4", "", ""),
    ("frontend/widgets/OBSBasic_MainControls.*", "P6-06.1", "", ""),
    ("frontend/widgets/OBSBasic_Docks.cpp", "P6-06.3", "", ""),
    ("frontend/widgets/OBSBasic_Canvases.cpp", "P6-06.3", "", ""),
    ("frontend/widgets/OBSBasic_Icons.cpp", "P6-06.1", "", ""),
    ("frontend/widgets/OBSBasic_Hotkeys.cpp", "P6-21", "", ""),
    ("frontend/widgets/OBSBasic_SysTray.cpp", "P6-41.1", "", ""),
    ("frontend/widgets/OBSBasic_StudioMode.cpp", "P6-06.4", "", ""),
    ("frontend/widgets/OBSBasicPreview.*", "P6-07", "", ""),
    ("frontend/widgets/OBSBasic_Preview.cpp", "P6-07", "", ""),
    ("frontend/widgets/OBSQTDisplay.*", "P6-07.1", "", ""),
    ("frontend/widgets/OBSBasic_Dropfiles.cpp", "P6-07.4", "", ""),
    ("frontend/widgets/OBSBasic_Scenes.cpp", "P6-08", "", ""),
    ("frontend/widgets/OBSBasic_SceneItems.cpp", "P6-09", "", ""),
    ("frontend/widgets/OBSBasic_Clipboard.cpp", "P6-09", "", ""),
    ("frontend/widgets/OBSBasic_Screenshots.cpp", "P6-08.3", "", ""),
    ("frontend/widgets/OBSBasic_ContextToolbar.cpp", "P6-14", "", ""),
    ("frontend/widgets/OBSBasicControls.*", "P6-12", "", ""),
    ("frontend/widgets/OBSBasic_Streaming.cpp", "P6-12.1", "", ""),
    ("frontend/widgets/OBSBasic_Recording.cpp", "P6-12.1", "", ""),
    ("frontend/widgets/OBSBasic_ReplayBuffer.cpp", "P6-12.2", "", ""),
    ("frontend/widgets/OBSBasic_VirtualCam.cpp", "P6-12.2", "", ""),
    ("frontend/widgets/OBSBasic_OutputHandler.cpp", "P6-12.1", "", ""),
    ("frontend/widgets/OBSBasicStats.*", "P6-13", "", ""),
    ("frontend/widgets/OBSBasic_StatusBar.cpp", "P6-06.2", "", ""),
    ("frontend/widgets/OBSBasicStatusBar.*", "P6-06.2", "", ""),
    ("frontend/widgets/StatusBarWidget.*", "P6-06.2", "", ""),
    ("frontend/widgets/OBSBasic_Transitions.cpp", "P6-11", "", ""),
    ("frontend/widgets/OBSBasic_SceneCollections.cpp", "P6-24", "", ""),
    ("frontend/widgets/OBSBasic_Profiles.cpp", "P6-25", "", ""),
    ("frontend/widgets/OBSBasic_Service.cpp", "P6-31", "", ""),
    ("frontend/widgets/OBSBasic_Updater.cpp", "P6-32", "", ""),
    ("frontend/widgets/OBSBasic_YouTube.cpp", "P6-30.3", "", ""),
    ("frontend/widgets/OBSBasic_Browser.cpp", "P6-15.2", "", ""),
    ("frontend/widgets/OBSProjector.*", "P6-07.8", "", ""),
    ("frontend/widgets/OBSBasic_Projectors.cpp", "P6-07.8", "", ""),
    ("frontend/widgets/OBSBasic.*", "P6-06", "", ""),
    ("frontend/widgets/OBSMainWindow.hpp", "P6-06", "", ""),
    ("frontend/wizards/*", "P6-22", "", ""),
    ("frontend/*", "P6-49", "", "sweep"),
    # ---------- plugins (bookends S1/S2/E1/E2 cover the whole dir; overrides below) ----------
    ("plugins/obs-filters/rnnoise/*", "P5-01.7", "S-054", ""),
    ("plugins/obs-filters/*", "P5-01", "S-027/S-028/S-054", ""),
    ("plugins/obs-transitions/*", "P5-02", "S-029", ""),
    ("plugins/image-source/*", "P5-03", "S-030", ""),
    ("plugins/obs-text/*", "P5-04", "", ""),
    ("plugins/text-freetype2/*", "P5-05", "S-038", ""),
    ("plugins/rtmp-services/*", "P5-06", "S-026", ""),
    ("plugins/obs-x264/*", "P5-07", "S-025", ""),
    ("plugins/obs-libfdk/*", "P5-08", "S-025", ""),
    ("plugins/coreaudio-encoder/*", "P5-09", "", ""),
    ("plugins/nv-filters/*", "P5-10", "", ""),
    ("plugins/obs-ffmpeg/ffmpeg-mux/*", "P5-14.3", "S-037", ""),
    ("plugins/obs-ffmpeg/obs-amf-test/*", "P5-41.4", "S-050", ""),
    ("plugins/obs-ffmpeg/texture-amf.*", "P5-41.3", "S-050", ""),
    ("plugins/obs-ffmpeg/obs-ffmpeg-mpegts.c", "P5-15.4", "S-059", ""),
    ("plugins/obs-ffmpeg/obs-ffmpeg-srt.h", "P5-15.4", "S-059", ""),
    ("plugins/obs-ffmpeg/obs-ffmpeg-rist.h", "P5-15.5", "S-059", ""),
    ("plugins/obs-ffmpeg/obs-ffmpeg-url.h", "P5-15.5", "S-059", ""),
    ("plugins/obs-ffmpeg/obs-ffmpeg-hls-mux.c", "P5-15.5", "S-046", ""),
    ("plugins/obs-ffmpeg/obs-ffmpeg-vaapi.c", "P5-12", "S-050", ""),
    ("plugins/obs-ffmpeg/vaapi-utils.*", "P5-12.3", "S-050", ""),
    ("plugins/obs-ffmpeg/obs-ffmpeg-nvenc.c", "P5-13", "S-050", ""),
    ("plugins/obs-ffmpeg/obs-ffmpeg-source.c", "P5-16", "S-047", ""),
    ("plugins/obs-ffmpeg/obs-ffmpeg-mux.*", "P5-14.5", "S-037", ""),
    ("plugins/obs-ffmpeg/*", "P5-11", "S-047/S-046/S-025", "module core, encoders, formats, logging"),
    ("plugins/obs-outputs/librtmp/*", "P5-17.4", "S-031/S-021", "D-15: port or FFI"),
    ("plugins/obs-outputs/flv-mux.*", "P5-17.3", "S-021", ""),
    ("plugins/obs-outputs/flv-output.c", "P5-17.3", "S-021", ""),
    ("plugins/obs-outputs/mp4-mux.*", "P5-18.5", "S-022", ""),
    ("plugins/obs-outputs/mp4-output.c", "P5-18.5", "S-022", ""),
    ("plugins/obs-outputs/null-output.c", "P5-18.4", "", ""),
    ("plugins/obs-outputs/*", "P5-17", "S-045", ""),
    ("plugins/obs-webrtc/*", "P5-19", "", "D-16"),
    ("plugins/obs-websocket/*", "P5-20", "D-28", "spec gap: protocol spec not written"),
    ("plugins/obs-vst/*", "P5-21", "", "D-11: probably stays C++"),
    ("plugins/vlc-video/*", "P5-22", "", ""),
    ("plugins/linux-pipewire/*", "P5-23", "S-056", ""),
    ("plugins/linux-capture/*", "P5-24", "S-049", ""),
    ("plugins/linux-v4l2/*", "P5-25", "S-049", ""),
    ("plugins/linux-pulseaudio/*", "P5-26", "S-049", ""),
    ("plugins/linux-alsa/*", "P5-27", "S-049", ""),
    ("plugins/linux-jack/*", "P5-28", "S-049", ""),
    ("plugins/oss-audio/*", "P5-29", "S-049", ""),
    ("plugins/sndio/*", "P5-29", "S-049", ""),
    ("plugins/win-wasapi/*", "P5-30", "D-32", "spec gap"),
    ("plugins/win-capture/graphics-hook/*", "P5-31.7", "", "D-08: stays C/C++ (hook DLL)"),
    ("plugins/win-capture/get-graphics-offsets/*", "P5-31.7", "", "D-08: stays C/C++"),
    ("plugins/win-capture/inject-helper/*", "P5-31.7", "", "D-08: stays C/C++"),
    ("plugins/win-capture/*", "P5-31", "S-051", ""),
    ("plugins/win-dshow/*", "P5-32", "S-051", ""),
    ("plugins/mac-capture/*", "P5-34", "S-052", ""),
    ("plugins/mac-avcapture/*", "P5-35", "", ""),
    ("plugins/mac-syphon/*", "P5-36", "", ""),
    ("plugins/mac-virtualcam/src/camera-extension/*", "P5-37.4", "S-052", ""),
    ("plugins/mac-virtualcam/src/dal-plugin/*", "P5-37.3", "", "D-14: legacy DAL, remove with port"),
    ("plugins/mac-virtualcam/src/common/*", "P5-37.3", "", "Mach protocol shared by both"),
    ("plugins/mac-virtualcam/src/obs-plugin/*", "P5-37.4", "S-052", ""),
    ("plugins/mac-virtualcam/*", "P5-37", "S-052", ""),
    ("plugins/mac-videotoolbox/*", "P5-38", "S-052", ""),
    ("plugins/obs-nvenc/*", "P5-39", "S-050", ""),
    ("plugins/obs-qsv11/*", "P5-40", "S-050", ""),
    ("plugins/decklink/*decklink-sdk/*", "P5-42.1", "", "D-10: vendor SDK, FFI only"),
    ("plugins/decklink/*", "P5-42", "", "D-10"),
    ("plugins/decklink-captions/*", "P5-42.4", "", "D-10"),
    ("plugins/decklink-output-ui/*", "P6-40.1", "", "D-10"),
    ("plugins/aja/*", "P5-43", "", "D-10"),
    ("plugins/aja-output-ui/*", "P6-40.2", "", "D-10"),
    ("plugins/frontend-tools/auto-scene-switcher*.*", "P6-38", "", ""),
    ("plugins/frontend-tools/captions*.*", "P6-39", "", ""),
    ("plugins/frontend-tools/output-timer.*", "P6-39.1", "", ""),
    ("plugins/frontend-tools/forms/*.ui", "P6-37.4", "", ""),
    ("plugins/frontend-tools/*", "P6-37.4", "", "module shell + script manager UI"),
    ("plugins/obs-browser/*", "P5-45", "", "D-09: CEF stays C++ (submodule)"),
    # ---------- shared ----------
    ("shared/bpm/*", "P1-33.1", "S-024", ""),
    ("shared/file-updater/*", "P1-33.2", "S-024", "used by P5-06.5"),
    ("shared/happy-eyeballs/*", "P1-33.3", "S-024", ""),
    ("shared/ipc-util/*", "P5-31.6", "S-055", "used by win-capture game capture"),
    ("shared/media-playback/*", "P5-16", "S-036", ""),
    ("shared/obs-d3d8-api/*", "P5-31.7", "", "D-08: stays C (hook DLL headers)"),
    ("shared/obs-hook-config/*", "P5-31.7", "", "D-08: stays C (hook config)"),
    ("shared/obs-inject-library/*", "P1-33.4", "", "used by P5-31.7, P5-32.6"),
    ("shared/obs-scripting/obs-scripting-lua*.*", "P6-37.1", "S-060", ""),
    ("shared/obs-scripting/obs-scripting-python*.*", "P6-37.2", "D-29", "spec gap: Python backend"),
    ("shared/obs-scripting/obslua/*", "P6-37.1", "S-060", ""),
    ("shared/obs-scripting/obspython/*", "P6-37.2", "D-29", "spec gap: Python backend"),
    ("shared/obs-scripting/*", "P6-37", "S-039", "language-independent core"),
    ("shared/obs-shared-memory-queue/*", "P5-32.6", "S-051", ""),
    ("shared/obs-tiny-nv12-scale/*", "P5-32.4", "S-051", ""),
    ("shared/opts-parser/*", "P1-33.5", "S-024", ""),
    ("shared/properties-view/*", "P6-16.1", "", ""),
    ("shared/qt/*", "P6-49", "", "D-01: carried over per toolkit decision"),
    # ---------- deps ----------
    ("deps/blake2/*", "P1-10.3, P1-31.2", "", "drop, use blake2 crate"),
    ("deps/glad/*", "P3-30", "", "drop when legacy GL is gone"),
    ("deps/json11/*", "P1-24.1, P1-31.2", "", "drop, use serde_json"),
    ("deps/libcaption/*", "P2-21", "", "D-18: probably FFI"),
    ("deps/libdshowcapture/*", "P5-32.3", "", "D-19: FFI (submodule)"),
    ("deps/w32-pthreads/*", "P1-05.3, P7-04.2", "", "D-26: absorb into Rust threading"),
    # ---------- test / build / docs ----------
    ("test/cmocka/*", "P0-14.1, P7-25", "", "kept, extended, then retired"),
    ("test/test-input/*", "P0-15, P7-25", "", "ABI test plugin until P7-25"),
    ("test/osx/*", "P7-25", "", "retire"),
    ("test/win/*", "P7-25", "", "retire"),
    ("test/*", "P7-25", "", "C test harness"),
    ("build-aux/*", "P6-42/P6-43/P6-44", "", "packaging; kept by P7-06.3"),
    ("cmake/*", "P0-08, P7-06.3", "", "Corrosion hook, removed in Phase 7"),
    ("additional_install_files/*", "P6-42.1", "", "Windows installer layout"),
    ("docs/sphinx/*", "P7-14.1", "", "docs rewritten in Phase 7"),
    ("scripts/*", "P7-25", "", "top-level build scripts"),
]

AREA = {
    "libobs/": "libobs core (`libobs/`)",
    "libobs-opengl/": "OpenGL backend (`libobs-opengl/`)",
    "libobs-d3d11/": "D3D11 backend (`libobs-d3d11/`)",
    "libobs-metal/": "Metal backend (`libobs-metal/`, Swift)",
    "libobs-winrt/": "Windows.Graphics.Capture (`libobs-winrt/`)",
    "frontend/": "Qt6 frontend (`frontend/`)",
    "plugins/": "Plugins (`plugins/`)",
    "shared/": "Shared helpers (`shared/`)",
    "deps/": "Vendored deps (`deps/`)",
    "test/": "C test harness (`test/`)",
    "build-aux/": "Packaging (`build-aux/`)",
    "cmake/": "Build system (`cmake/`)",
    "additional_install_files/": "Installer layout (`additional_install_files/`)",
    "docs/": "Docs (`docs/sphinx/`)",
    "scripts/": "Top-level scripts (`scripts/`)",
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--src", default="../obs-studio")
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()
    src = args.src

    files = subprocess.check_output(["git", "-C", src, "ls-files"], text=True).split("\n")
    files = [f for f in files if f]
    rev = subprocess.check_output(["git", "-C", src, "describe", "--always"], text=True).strip()

    tracked = [f for f in files if f.endswith(TRACKED_EXT)]
    untracked = [f for f in files if not f.endswith(TRACKED_EXT)]

    def match(f):
        for pat, task, spec, note in RULES:
            if fnmatch.fnmatch(f, pat):
                return task, spec, note
        return None

    gaps = []
    rows = {}  # area -> list of (file, task, spec, note)
    for f in tracked:
        m = match(f)
        if m is None:
            gaps.append(f)
            continue
        area = next(k for k in AREA if f.startswith(k))
        rows.setdefault(area, []).append((f, *m))

    def loc(f):
        try:
            with open(os.path.join(src, f), errors="ignore") as fh:
                return sum(1 for _ in fh)
        except OSError:
            return 0

    total_loc = sum(loc(f) for f in tracked)
    kept_c = [f for f in tracked if match(f) and ("stays C" in match(f)[2] or "kept C" in match(f)[2])]

    print(f"checkout: {src} @ {rev}")
    print(f"tracked source/ui/effect files: {len(tracked)} ({total_loc} lines)")
    print(f"build/data files (summarised per area): {len(untracked)}")
    if gaps:
        print(f"\nUNMAPPED FILES ({len(gaps)}) — coverage gaps:")
        for f in gaps:
            print("  " + f)
    else:
        print("all tracked files mapped to a task")
    print(f"kept-C / FFI-only entries: {len(kept_c)}")

    if args.write and not gaps:
        out = []
        out.append("# Coverage matrix: every file of the C reference -> rewrite task\n")
        out.append(f"Generated by `tools/task-coverage.py` against OBS Studio `{rev}`. "
                   "Regenerate after updating the checkout; do not edit by hand. "
                   "Task IDs refer to `docs/rust-rewrite/phase-N-*.md` (subtasks in "
                   "`subtasks/`), spec IDs to `docs/rust-rewrite/specs/`, "
                   "decision IDs to `docs/rust-rewrite/DECISIONS-NEEDED.md`.\n")
        out.append("Tracked = every `.c .cpp .cc .h .hpp .m .mm .swift`, `.ui` and `.effect` "
                   "file (source, Qt forms, shaders). All other files (CMake, locale, images, "
                   "config templates, manifests) are build/data and are summarised at the end "
                   "of each area with their owning task.\n")
        out.append("| Area | Files | Lines | Owns |\n|---|---|---|---|")
        for area in sorted(rows):
            r = rows[area]
            own = sorted({re.split(r"[,;]", t)[0].strip() for f, t, s, n in r})
            out.append(f"| [{AREA[area]}](#{area.strip('/').replace('/', '-').lower()}) "
                       f"| {len(r)} | {sum(loc(f) for f, *_ in r)} | {', '.join(own[:4])}{'…' if len(own) > 4 else ''} |")
        out.append(f"| **Total** | **{len(tracked)}** | **{total_loc}** | |")
        for area in sorted(rows):
            out.append(f"\n## {AREA[area]}\n")
            out.append("| File | Task | Spec | Note |")
            out.append("|---|---|---|---|")
            for f, t, s, n in sorted(rows[area]):
                out.append(f"| `{f}` | {t} | {s or '—'} | {n or ''} |")
            # build/data summary for this area
            data = [f for f in untracked if f.startswith(area)]
            if data:
                dloc = sum(loc(f) for f in data)
                out.append(f"\nBuild/data files in this area: **{len(data)}** ({dloc} lines) — "
                           "owner: same task as the area's source files; packaging owned by P6-42…P6-44, "
                           "CMake removal by P7-06.3.\n")
        path = "docs/rust-rewrite/coverage/coverage-matrix.md"
        os.makedirs(os.path.dirname(path), exist_ok=True)
        with open(path, "w") as fh:
            fh.write("\n".join(out) + "\n")
        print(f"wrote {path}")

    return 1 if gaps else 0


if __name__ == "__main__":
    raise SystemExit(main())
