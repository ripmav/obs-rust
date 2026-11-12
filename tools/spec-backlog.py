#!/usr/bin/env python3
"""Spec-coverage backlog: every non-trivial first-party file not cited by a spec
(S2 = 0 in the function inventory) gets an action — extend an existing spec or
write a new one (specs/BACKLOG.md). The spec series must be complete: the goal
is that every behaviour in the C reference is pinned by a spec.

Usage: python3 tools/spec-backlog.py [--write]

The FILE_GROUPS mapping below is the plan (hand-curated, pinned to 33.0.1);
the file->action verification is computed from the inventory, so any file a
new C release adds shows up as UNASSIGNED and the script exits non-zero.
"""

import argparse
import csv
import collections
import os
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSV_PATH = os.path.join(REPO, "docs/rust-rewrite/coverage/function-inventory.csv")
OUT_PATH = os.path.join(REPO, "docs/rust-rewrite/specs/BACKLOG.md")

# action -> (title, kind, task)  — new specs start at S-068 (S-061..S-066 are the
# DECISIONS-NEEDED backlog, S-067 = preview interaction, P6-07.9)
ACTIONS = {
    # --- libobs & shared sweeps (P1-34) ---
    "E-S001-S011": ("libobs container/math/lexer header inlines + core glue", "extend", "P1-34.1"),
    "E-S013-S010-S023-S024": ("media conversion, serializers, platform/util, small helpers", "extend", "P1-34.2"),
    "E-S041-S042-S035": ("output, core remainder, module loader", "extend", "P1-34.3"),
    "E-S032-S065": ("audio controls + audio-monitoring backends", "extend", "P1-34.4"),
    "S-061": ("threading primitives (round 5, DECISIONS D-27)", "new", "P1-05.5"),
    "S-062": ("obs-websocket protocol (round 5, DECISIONS D-28)", "new", "P5-20.12"),
    "S-051-wasapi": ("S-051 §5.G WASAPI enumeration/capture gap (round 5, D-32)", "extend", "P5-30.9"),
    "S-063": ("Python scripting backend (round 5, D-29)", "new", "P6-37.6"),
    # --- graphics backends (P3-32) ---
    "S-078": ("D3D11 backend", "new", "P3-32.1"),
    "S-079": ("Metal backend", "new", "P3-32.2"),
    "E-S057": ("OpenGL backend file citations + uncited helpers", "extend", "P3-32.3"),
    # --- plugin coverage (P5-51) ---
    "E-S027-S028-S054": ("obs-filters: 13 video filters, audio filters, RNNoise contract", "extend", "P5-51.1"),
    "E-S029": ("obs-transitions: stinger, luma-wipe, fade-to-color, slide, swipe, fade, cut", "extend", "P5-51.2"),
    "E-S030": ("image-source: slideshow v1/mk2, image, colour source", "extend", "P5-51.3"),
    "E-S038": ("text: freetype2 (incl. find-font per OS) + GDI text source (obs-text)", "extend", "P5-51.4"),
    "E-S046-S047-S037": ("obs-ffmpeg: source, encoders (nvenc/openh264/amf), logging, mux remux", "extend", "P5-51.5"),
    "E-S050-S025": ("encoder runtimes: qsv11, nvenc, x264, libfdk, nv-filters, test plugins", "extend", "P5-51.6"),
    "E-S026": ("rtmp-services: amazon-ivs, twitch service-specific", "extend", "P5-51.7"),
    "E-S049": ("Linux capture + audio backends: v4l2, capture, alsa, pulseaudio, sndio, oss, jack", "extend", "P5-51.8"),
    "S-080": ("obs-webrtc / WHIP output", "new", "P5-51.9"),
    "E-S041-outputs": ("obs-outputs plugin: null output, module glue", "extend", "P5-51.10"),
    # --- Windows capture (P5-51.11/.12/.13) ---
    "E-S051-win": ("win-capture host: game/window/monitor/dc/duplicator/cursor, winrt WGC, d3d8 api, inject library", "extend", "P5-51.11"),
    "S-086": ("win-capture graphics-hook FFI contract (hook stays C++, D-08)", "new", "P5-51.12"),
    "E-S051-dshow": ("win-dshow host: dshow_input, encoders, virtualcam glue", "extend", "P5-51.13"),
    "S-081": ("libdshowcapture FFI contract (library stays C++, D-19)", "new", "P5-51.14"),
    # --- macOS (P5-51.15) ---
    "E-S052": ("macOS plugins: mac-capture, mac-avcapture, mac-virtualcam, mac-syphon, coreaudio-encoder", "extend", "P5-51.15"),
    # --- FFI-kept vendor/third-party (P5-51.16..19) ---
    "S-082": ("obs-browser / CEF FFI contract (CEF stays, D-09)", "new", "P5-51.16"),
    "S-083": ("decklink FFI contract (decklink, output-ui, captions)", "new", "P5-51.17"),
    "S-084": ("aja FFI contract (aja, output-ui)", "new", "P5-51.18"),
    "S-085": ("vlc-video + obs-vst FFI contract (D-10)", "new", "P5-51.19"),
    # --- frontend series (P6-50) ---
    "S-068": ("frontend settings dialog (settings/*)", "new", "P6-50.1"),
    "S-069": ("frontend main window & controls", "new", "P6-50.2"),
    "S-070": ("frontend scene collections, profiles & importers", "new", "P6-50.3"),
    "S-071": ("frontend audio mixer & volume widgets", "new", "P6-50.4"),
    "S-072": ("frontend dialogs & source pickers/toolbars", "new", "P6-50.5"),
    "S-073": ("frontend output pipeline UI (simple/advanced output, remux, multitrack)", "new", "P6-50.6"),
    "S-074": ("frontend updater, oauth & YouTube", "new", "P6-50.7"),
    "S-075": ("frontend plugin manager, crash handler & system info", "new", "P6-50.8"),
    "S-076": ("properties view & Qt widget kit (properties-view, idian, spinboxes, wrappers)", "new", "P6-50.9"),
    "S-077": ("frontend API (obs-frontend-api) & utility glue", "new", "P6-50.10"),
    "S-087": ("frontend-tools plugin: auto-scene-switcher, output-timer, captions, scripts UI", "new", "P6-50.11"),
}

# file-prefix -> action (longest prefix wins)
FILE_GROUPS = {
    "libobs/callback/": "E-S001-S011",
    "libobs/graphics/": "E-S001-S011",
    "libobs/util/darray.h": "E-S001-S011",
    "libobs/util/deque.h": "E-S001-S011",
    "libobs/util/dstr.h": "E-S001-S011",
    "libobs/util/cf-lexer.c": "E-S001-S011",
    "libobs/util/cf-parser.h": "E-S001-S011",
    "libobs/util/cf-parser.c": "E-S001-S011",
    "libobs/util/lexer.h": "E-S001-S011",
    "libobs/util/lexer.c": "E-S001-S011",
    "libobs/util/cf-lexer.h": "E-S001-S011",
    "libobs/util/threading": "S-061",
    "libobs/util/base.c": "E-S001-S011",
    "libobs/util/bmem.c": "E-S001-S011",
    "libobs/util/bmem.h": "E-S001-S011",
    "libobs/util/util.hpp": "E-S001-S011",
    "libobs/media-io/": "E-S013-S010-S023-S024",
    "libobs/util/serializer.h": "E-S013-S010-S023-S024",
    "libobs/util/file-serializer.c": "E-S013-S010-S023-S024",
    "libobs/util/buffered-file-serializer.c": "E-S013-S010-S023-S024",
    "libobs/util/array-serializer.c": "E-S013-S010-S023-S024",
    "libobs/util/bitstream.c": "E-S013-S010-S023-S024",
    "libobs/util/text-lookup.c": "E-S013-S010-S023-S024",
    "libobs/util/platform.c": "E-S013-S010-S023-S024",
    "libobs/util/windows/": "E-S013-S010-S023-S024",
    "libobs/util/platform-windows.c": "E-S013-S010-S023-S024",
    "libobs/util/utf8.c": "E-S013-S010-S023-S024",
    "libobs/util/profiler.hpp": "E-S013-S010-S023-S024",
    "shared/file-updater/": "E-S013-S010-S023-S024",
    "shared/happy-eyeballs/": "E-S013-S010-S023-S024",
    "shared/bpm/": "E-S013-S010-S023-S024",
    "shared/opts-parser/": "E-S013-S010-S023-S024",
    "libobs/obs-output.c": "E-S041-S042-S035",
    "libobs/obs-output-delay.c": "E-S041-S042-S035",
    "libobs/obs-cocoa.m": "E-S041-S042-S035",
    "libobs/obs-windows.c": "E-S041-S042-S035",
    "libobs/obs-win-crash-handler.c": "E-S041-S042-S035",
    "libobs/obs.hpp": "E-S041-S042-S035",
    "libobs/obs-core-modules": "E-S041-S042-S035",
    "libobs/audio-monitoring/": "E-S032-S065",
    "libobs/obs-audio-controls.c": "E-S032-S065",
    "libobs-d3d11/": "S-078",
    "libobs-metal/": "S-079",
    "libobs-opengl/": "E-S057",
    "plugins/obs-filters/": "E-S027-S028-S054",
    "plugins/obs-transitions/": "E-S029",
    "plugins/image-source/": "E-S030",
    "plugins/text-freetype2/": "E-S038",
    "plugins/obs-text/": "E-S038",
    "plugins/obs-ffmpeg/": "E-S046-S047-S037",
    "plugins/obs-qsv11/": "E-S050-S025",
    "plugins/obs-nvenc/": "E-S050-S025",
    "plugins/obs-x264/": "E-S050-S025",
    "plugins/obs-libfdk/": "E-S050-S025",
    "plugins/nv-filters/": "E-S050-S025",
    "plugins/rtmp-services/": "E-S026",
    "plugins/linux-v4l2/": "E-S049",
    "plugins/linux-capture/": "E-S049",
    "plugins/linux-alsa/": "E-S049",
    "plugins/linux-pulseaudio/": "E-S049",
    "plugins/sndio/": "E-S049",
    "plugins/oss-audio/": "E-S049",
    "plugins/linux-jack/": "E-S049",
    "plugins/obs-webrtc/": "S-080",
    "plugins/obs-outputs/": "E-S041-outputs",
    "plugins/win-capture/game-capture": "E-S051-win",
    "plugins/win-capture/window-capture.c": "E-S051-win",
    "plugins/win-capture/monitor-capture.c": "E-S051-win",
    "plugins/win-capture/dc-capture.c": "E-S051-win",
    "plugins/win-capture/duplicator-monitor-capture.c": "E-S051-win",
    "plugins/win-capture/cursor-capture.c": "E-S051-win",
    "plugins/win-capture/app-helpers.c": "E-S051-win",
    "plugins/win-capture/audio-helpers.c": "E-S051-win",
    "plugins/win-capture/plugin-main.c": "E-S051-win",
    "plugins/win-capture/nt-stuff.c": "E-S051-win",
    "plugins/win-capture/graphics-hook/": "S-086",
    "plugins/win-capture/inject-helper/": "S-086",
    "plugins/win-capture/get-graphics-offsets/": "S-086",
    "plugins/win-capture/load-graphics-offsets.c": "S-086",
    "plugins/win-dshow/": "E-S051-dshow",
    "plugins/win-wasapi/": "S-051-wasapi",
    "shared/obs-scripting/obs-scripting-python": "S-063",
    "deps/libdshowcapture/": "S-081",
    "libobs-winrt/": "E-S051-win",
    "shared/obs-d3d8-api/": "E-S051-win",
    "shared/obs-inject-library/": "E-S051-win",
    "plugins/mac-capture/": "E-S052",
    "plugins/mac-avcapture/": "E-S052",
    "plugins/mac-virtualcam/": "E-S052",
    "plugins/mac-syphon/": "E-S052",
    "plugins/coreaudio-encoder/": "E-S052",
    "plugins/obs-websocket/": "S-062",
    "plugins/obs-browser/": "S-082",
    "plugins/decklink/": "S-083",
    "plugins/decklink-output-ui/": "S-083",
    "plugins/decklink-captions/": "S-083",
    "plugins/aja/": "S-084",
    "plugins/aja-output-ui/": "S-084",
    "plugins/vlc-video/": "S-085",
    "plugins/obs-vst/": "S-085",
    # frontend: directory-level (checked before filename rules)
    "frontend/settings/": "S-068",
    "frontend/importers/": "S-070",
    "frontend/importer/": "S-070",
    "frontend/updater/": "S-074",
    "frontend/oauth/": "S-074",
    "frontend/plugin-manager/": "S-075",
    "frontend/api/": "S-077",
    "shared/properties-view/": "S-076",
    "shared/qt/": "S-076",
    "plugins/frontend-tools/": "S-087",
    # frontend: filename stems (S-073 output pipeline first: SimpleOutput etc.)
    "SimpleOutput": "S-073",
    "AdvancedOutput": "S-073",
    "BasicOutputHandler": "S-073",
    "OBSBasic_OutputHandler": "S-073",
    "audio-encoders": "S-073",
    "FFmpegCodec": "S-073",
    "FFmpegFormat": "S-073",
    "FFmpegShared": "S-073",
    "RemuxQueueModel": "S-073",
    "RemuxWorker": "S-073",
    "RemuxEntryPathItemDelegate": "S-073",
    "OBSRemux": "S-073",
    "MultitrackVideo": "S-073",
    "multitrack-video": "S-073",
    "WHIPSimulcast": "S-073",
    "QuickTransition": "S-073",
    "AudioMixer": "S-071",
    "VolumeControl": "S-071",
    "VolumeMeter": "S-071",
    "VolumeName": "S-071",
    "VolumeSlider": "S-071",
    "VolumeAccessibleInterface": "S-071",
    "BalanceSlider": "S-071",
    "AbsoluteSlider": "S-071",
    "MuteCheckBox": "S-071",
    "MenuCheckBox": "S-071",
    "OBSBasic_SceneCollections": "S-070",
    "OBSBasic_Profiles": "S-070",
    "OBSBasic_YouTube": "S-074",
    "YouTubeAppDock": "S-074",
    "YouTubeApiWrappers": "S-074",
    "YoutubeApiWrappers": "S-074",
    "YouTubeChatDock": "S-074",
    "BrowserDock": "S-074",
    "GoLiveAPI": "S-074",
    "WhatsNew": "S-074",
    "whatsnew": "S-074",
    "RemoteTextThread": "S-074",
    "CrashHandler": "S-075",
    "system-info": "S-075",
    "platform-windows.cpp": "S-075",
    "platform-osx": "S-075",
    "platform-x11": "S-075",
    "win-dll-blocklist": "S-075",
    "OBSSparkle": "S-075",
    "OBSUpdateDelegate": "S-075",
    "MacUpdateThread": "S-075",
    "crypto-helpers": "S-075",
    "NativeEventFilter": "S-075",
    "ThumbnailManager": "S-077",
    "ThumbnailItem": "S-077",
    "ThumbnailView": "S-077",
    "undo_stack": "S-077",
    "OBSTranslator": "S-077",
    "UIValidation": "S-077",
    "ResizeSignaler": "S-077",
    "SurfaceEventFilter": "S-077",
    "SettingsEventFilter": "S-077",
    "item-widget-helpers": "S-077",
    "display-helpers": "S-077",
    "models/": "S-077",
    "obf.c": "S-077",
    "OBSBasic": "S-069",
    "OBSApp": "S-069",
    "OBSQTDisplay": "S-069",
    "Multiview": "S-069",
    "OBSProjector": "S-069",
    "OBSBasic_Dropfiles": "S-069",
    "OBSBasicInteraction": "S-069",
    "OBSBasic_Clipboard": "S-069",
    "OBSBasic_Screenshots": "S-069",
    "OBSBasic_Hotkeys": "S-069",
    "OBSBasic_SysTray": "S-069",
    "OBSBasic_Icons": "S-069",
    "OBSBasic_Browser": "S-069",
    "OBSBasic_Canvases": "S-069",
    "OBSBasic_Service": "S-069",
    "OBSBasic_ReplayBuffer": "S-069",
    "OBSBasic_VirtualCam": "S-069",
    "OBSBasicVCamConfig": "S-069",
    "OBSBasic_StudioMode": "S-069",
    "OBSBasicPreview": "S-069",
    "OBSBasic_Preview": "S-069",
    "PreviewProgramSizeObserver": "S-069",
    "OBSCanvas": "S-069",
    "OBSPreviewScaling": "S-069",
    "OBSBasic_Docks": "S-069",
    "OBSDock": "S-069",
    "OAuthLogin": "S-074",
    "OBSYoutubeActions": "S-074",
    "AutoUpdateThread": "S-074",
    "update-helpers": "S-074",
    "OBSProxyStyle": "S-077",
    "ScreenshotObj": "S-077",
    "SilentUpdate": "S-073",
    "StartMultiTrackVideo": "S-073",
    "frontend/models/": "S-077",
    "OBSBasicFilters": "S-072",
    "OBSBasicProperties": "S-072",
    "OBSBasicSourceSelect": "S-072",
    "SourceSelectButton": "S-072",
    "OBSBasicTransform": "S-072",
    "OBSBasicAdvAudio": "S-072",
    "OBSAdvAudioCtrl": "S-072",
    "OBSMissingFiles": "S-072",
    "MissingFilesModel": "S-072",
    "MissingFilesPathItemDelegate": "S-072",
    "NameDialog": "S-072",
    "OBSPermissions": "S-072",
    "OBSAbout": "S-072",
    "OBSLogViewer": "S-072",
    "OBSUpdate": "S-072",
    "OBSWhatsNew": "S-072",
    "OBSExtraBrowsers": "S-072",
    "ExtraBrowsersModel": "S-072",
    "ExtraBrowsersDelegate": "S-072",
    "LogUploadDialog": "S-072",
    "OBSIdianPlayground": "S-072",
    "OBSHotkey": "S-072",
    "SourceTree": "S-072",
    "AlignmentSelector": "S-072",
    "AccessibleAlignment": "S-072",
    "SourceToolbar": "S-072",
    "SourceLabel": "S-072",
    "VisibilityItem": "S-072",
    "SceneTree": "S-072",
    "SceneRenameDelegate": "S-072",
    "ColorSourceToolbar": "S-072",
    "GameCaptureToolbar": "S-072",
    "TextSourceToolbar": "S-072",
    "BrowserToolbar": "S-072",
    "DeviceCaptureToolbar": "S-072",
    "DisplayCaptureToolbar": "S-072",
    "AudioCaptureToolbar": "S-072",
    "ApplicationAudioCaptureToolbar": "S-072",
    "WindowCaptureToolbar": "S-072",
    "ImageSourceToolbar": "S-072",
    "ComboSelectToolbar": "S-072",
    "MenuButton": "S-072",
    "UrlPushButton": "S-072",
    "ClickableLabel": "S-072",
    "FlowLayout": "S-072",
    "FlowFrame": "S-072",
    "FocusList": "S-072",
    "AutoConfig": "S-069",
    "TestMode": "S-069",
    "MediaControls": "S-073",
    "OBSBasic_Recording": "S-069",
    "OBSBasic_Streaming": "S-069",
    "OBSBasic_Transitions": "S-069",
    "OBSBasic_Scenes": "S-069",
    "OBSBasic_MainControls": "S-069",
    "OBSBasicControls": "S-069",
    "OBSBasicStatusBar": "S-069",
    "OBSBasic_StatusBar": "S-069",
    "OBSBasicStats": "S-069",
    "OBSBasic_ContextToolbar": "S-069",
}

# documented exclusion (not ported behaviour)
EXCLUDED = {
    "test/": "C-reference test harness (test-input plugin, cmocka unit tests, "
             "osx/win test hosts) — no port target; exercised by the conformance suite instead.",
}


def action_for(f):
    for ex in EXCLUDED:
        if f.startswith(ex):
            return None
    best = None
    for pref, act in FILE_GROUPS.items():
        if f.startswith(pref) or (not pref.endswith("/") and pref in os.path.basename(f)) and best is None:
            if best is None or len(pref) > len(best[0]):
                best = (pref, act)
    return best[1] if best else None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()

    rows = list(csv.DictReader(open(CSV_PATH)))
    gap = [r for r in rows if r["vendored"] == "0"
           and (not r["body_lines"] or int(r["body_lines"]) >= 5)
           and r["S2"] == "0"]
    byfile = {}
    for r in gap:
        e = byfile.setdefault(r["file"], [0, 0])
        e[0] += 1
        e[1] += int(r["body_lines"] or 0)

    assigned = collections.defaultdict(lambda: [0, 0, 0])
    unassigned = []
    excluded = collections.defaultdict(lambda: [0, 0])
    for f in sorted(byfile):
        act = action_for(f)
        if act is None and any(f.startswith(ex) for ex in EXCLUDED):
            e = excluded[EXCLUDED_NEXT(f)]
            e[0] += byfile[f][0]; e[1] += byfile[f][1]
            continue
        if act is None:
            unassigned.append(f)
            continue
        a = assigned[act]
        a[0] += byfile[f][0]; a[1] += byfile[f][1]; a[2] += 1

    if unassigned:
        print(f"UNASSIGNED ({len(unassigned)}):", file=sys.stderr)
        for f in unassigned:
            print(f"  {f}", file=sys.stderr)
        sys.exit(1)

    md = []
    md.append("# Spec-coverage backlog (round 6, 2026-11)")
    md.append("")
    md.append("Goal: **every behaviour in the C reference is pinned by a spec** — extend an existing "
              "spec or write a new one. Measured from `function-inventory.csv`: first-party, non-trivial "
              "functions whose file no spec cites (`S2 = 0`), 33.0.1@65c320b46: "
              f"**{len(byfile)} files / {sum(v[0] for v in byfile.values())} functions**.")
    md.append("")
    md.append("New spec IDs continue the series (S-061…S-066 = the DECISIONS-NEEDED backlog written by "
              "round-5 subtasks, S-067 = preview interaction from P6-07.9). Regenerate this file with "
              "`python3 tools/spec-backlog.py --write`; it fails if a file has no action.")
    md.append("")
    md.append("## Actions")
    md.append("")
    md.append("| Action | Kind | Task | Title | files | fns | lines |")
    md.append("|---|---|---|---|---|---|---|")
    for act, (n, loc, nf) in sorted(assigned.items(), key=lambda kv: (kv[1][2] == 0, -kv[1][0])):
        title, kind, task = ACTIONS[act]
        md.append(f"| {act} | {kind} | {task} | {title} | {nf} | {n} | {loc} |")
    md.append("")
    md.append("## Documented exclusions")
    md.append("")
    for ex in EXCLUDED:
        md.append(f"- `{ex}` — {EXCLUDED[ex]} "
                  f"({sum(v[0] for k, v in excluded.items())} fns / {sum(v[1] for v in excluded.values())} lines)")
    md.append("")
    md.append("## Files per action")
    md.append("")
    for act in sorted(assigned):
        title, kind, task = ACTIONS[act]
        md.append(f"### {act} — {title} ({task})")
        md.append("")
        files = sorted((f for f in byfile if action_for(f) == act), key=lambda f: -byfile[f][0])
        for f in files:
            md.append(f"- `{f}` ({byfile[f][0]} fns / {byfile[f][1]} lines)")
        md.append("")

    text = "\n".join(md) + "\n"
    n_excl = sum(1 for f in byfile if action_for(f) is None)
    print(f"actions: {len(assigned)} | files: {len(byfile)} | excluded files: {n_excl}")
    if args.write:
        with open(OUT_PATH, "w") as fh:
            fh.write(text)
        print(f"wrote {OUT_PATH}")


def EXCLUDED_NEXT(f):
    for ex in EXCLUDED:
        if f.startswith(ex):
            return ex
    return f


if __name__ == "__main__":
    main()
