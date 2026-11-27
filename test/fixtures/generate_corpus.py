#!/usr/bin/env python3
"""generate_corpus.py — WP-42 (P0-18.2, per D4): synthetic scene-collection
fixtures for test/fixtures/scenes/.

D4 (decided 2026-11-23): the corpus is licence-compliant PUBLIC material +
SYNTHETIC fixtures. The synthetic half is generated here: 33.0.1-format
scene collections (the pinned reference's on-disk format, see
frontend/widgets/OBSBasic_SceneCollections.cpp Save/Load) that exercise the
source/filter/transition ids of the in-tree plugins.

Every generated file is passed through anonymise.py before writing, so the
committed corpus is uniform (any absolute path / URL / stream key / window
title the builders seed arrives as an ANON_* token).

Usage:
  generate_corpus.py OUTDIR     regenerate all synthetic fixtures

Stdlib only. Deterministic (fixed uuid counter, no randomness, no clocks).
"""

import base64
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import anonymise  # noqa: E402  (same directory)

# (33 << 24) | (0 << 16) | 1 — LIBOBS_API_VER of the pinned 33.0.1 reference
PREV_VER = 553648129

_uuid_counter = 0


def _uuid():
    global _uuid_counter
    _uuid_counter += 1
    return f"c0ffe00{(_uuid_counter // 0x100000000) % 10}-0000-4000-8000-{_uuid_counter:012x}"


def vec(x, y):
    """vec2 values are doubles in the OBS save format (0.0, not 0)."""
    return {"x": float(x), "y": float(y)}


def source(id, name, settings=None, filters=None, mixers=0, sync=0, flags=0,
           volume=1.0, balance=0.5, enabled=True, muted=False,
           push_to_talk=False, ptt_delay=0, monitoring_enabled=False,
           monitoring_type=0, deinterlace_mode=0, deinterlace_field_order=0,
           hotkeys=None, canvas_uuid=None, private_settings=None,
           versioned_id=None, **extra):
    """One source entry with the full 33.0.1 obs_save_source key set."""
    s = {
        "prev_ver": PREV_VER,
        "name": name,
        "uuid": _uuid(),
        "id": id,
        "versioned_id": versioned_id or id,
        "settings": settings if settings is not None else {},
        "mixers": mixers,
        "sync": sync,
        "flags": flags,
        "volume": volume,
        "balance": balance,
        "enabled": enabled,
        "muted": muted,
        "push-to-mute": False,
        "push-to-mute-delay": 0,
        "push-to-talk": push_to_talk,
        "push-to-talk-delay": ptt_delay,
        "hotkeys": hotkeys if hotkeys is not None else {},
        "deinterlace_mode": deinterlace_mode,
        "deinterlace_field_order": deinterlace_field_order,
        "monitoring_type": monitoring_type,
        "monitoring_enabled": monitoring_enabled,
        "private_settings": private_settings if private_settings is not None else {},
    }
    if canvas_uuid is not None:
        s["canvas_uuid"] = canvas_uuid
    if filters:
        s["filters"] = filters
    s.update(extra)
    return s


def scene(name, items, id_counter=None, custom_size=False, cx=0, cy=0,
          canvas_uuid=None):
    settings = {
        "id_counter": id_counter if id_counter is not None else (len(items) + 1),
        "custom_size": custom_size,
    }
    if custom_size:
        settings["cx"] = cx
        settings["cy"] = cy
    settings["items"] = items
    return source("scene", name, settings, canvas_uuid=canvas_uuid)


def group(name, items, id_counter=None):
    """A group source: its settings carry the group-local items (the
    scene_save_item group_item_backup copies live in the parent scene)."""
    return source("group", name,
                  {"id_counter": id_counter if id_counter is not None
                   else len(items) + 1,
                   "custom_size": False,
                   "items": items})


def item(name, src, pos=None, pos_rel=None, scale=None, scale_rel=None,
         scale_ref=None, rot=0.0, visible=True, locked=False, align=5,
         bounds_type=0, bounds_align=0, bounds_crop=False, bounds=None,
         bounds_rel=None, crop=0, item_id=1, group_item_backup=False,
         scale_filter="disable", blend_method="default", blend_type="normal",
         show=None, hide=None, private_settings=None):
    """One scene item with the full 33.0.1 scene_save_item key set."""
    i = {
        "name": name,
        "source_uuid": src.get("uuid", "") if isinstance(src, dict) else src,
        "visible": visible,
        "locked": locked,
        "rot": rot,
        "scale_ref": scale_ref or vec(1, 1),
        "align": align,
        "bounds_type": bounds_type,
        "bounds_align": bounds_align,
        "bounds_crop": bounds_crop,
        "crop_left": crop,
        "crop_top": crop,
        "crop_right": crop,
        "crop_bottom": crop,
        "id": item_id,
        "group_item_backup": group_item_backup,
        "pos": pos or vec(0, 0),
        "pos_rel": pos_rel or vec(0, 0),
        "scale": scale or vec(1, 1),
        "scale_rel": scale_rel or vec(1, 1),
        "bounds": bounds or vec(0, 0),
        "bounds_rel": bounds_rel or vec(0, 0),
        "scale_filter": scale_filter,
        "blend_method": blend_method,
        "blend_type": blend_type,
        "show_transition": show if show is not None else {"duration": 0},
        "hide_transition": hide if hide is not None else {"duration": 0},
        "private_settings": private_settings if private_settings is not None else {},
    }
    return i


def transition(name, id, settings=None, source_a="", alignment=0, mode=0,
               scale_type=0, cx=0, cy=0):
    """One entry of the top-level transitions array (SaveTransitions)."""
    return {
        "name": name,
        "id": id,
        "settings": settings if settings is not None else {},
        "transition_source_a": source_a,
        "transition_alignment": alignment,
        "transition_mode": mode,
        "transition_scale_type": scale_type,
        "transition_cx": cx,
        "transition_cy": cy,
    }


def quick(name, duration=300, hotkeys=None, id=1, fade_to_black=False):
    return {"name": name, "duration": duration,
            "hotkeys": hotkeys if hotkeys is not None else [],
            "id": id, "fade_to_black": fade_to_black}


def canvas(name, flags=0):
    return {"info": {"name": name, "uuid": _uuid(), "private": False,
                     "flags": flags}}


def collection(name, sources, scene_names, current_scene=None,
               current_program_scene=None, groups=None, transitions=None,
               quick_transitions=None, current_transition="Cut",
               transition_duration=300, canvases=None, audio=None,
               projectors=None, preview_locked=False, scaling_enabled=False,
               scaling_level=0, scaling_off_x=0.0, scaling_off_y=0.0,
               virtual_camera=None, modules=None, resolution=(1920, 1080),
               version=2, migration_resolution=None):
    """Top-level collection in the 33.0.1 OBSBasic::Save key order."""
    d = {"name": name}
    if audio is not None:
        d["audio"] = audio
    d["sources"] = sources
    d["groups"] = groups or []
    d["scene_order"] = [{"name": n} for n in scene_names]
    d["current_scene"] = current_scene or (scene_names[0] if scene_names else "")
    d["current_program_scene"] = (
        current_program_scene
        or current_scene
        or (scene_names[0] if scene_names else ""))
    d["canvases"] = canvases or []
    d["current_transition"] = current_transition
    d["transition_duration"] = transition_duration
    d["transitions"] = transitions or []
    d["quick_transitions"] = (
        quick_transitions
        if quick_transitions is not None
        else [quick("Cut"), quick("Fade", id=2)])
    d["saved_projectors"] = projectors or []
    d["preview_locked"] = preview_locked
    d["scaling_enabled"] = scaling_enabled
    d["scaling_level"] = scaling_level
    d["scaling_off_x"] = scaling_off_x
    d["scaling_off_y"] = scaling_off_y
    if virtual_camera is not None:
        d["virtual-camera"] = virtual_camera
    if modules is not None:
        d["modules"] = modules
    d["resolution"] = {"x": resolution[0], "y": resolution[1]}
    if migration_resolution is not None:
        d["migration_resolution"] = migration_resolution
    d["version"] = version
    return d


MEDIA_HOTKEYS = {
    "MediaSource.Restart": [], "MediaSource.Play": [], "MediaSource.Pause": [],
    "MediaSource.Stop": [],
}

# ---------------------------------------------------------------------------
# the 18 synthetic collections
# ---------------------------------------------------------------------------


def s01_browser_basic():
    site = source("browser_source", "Site", {
        "url": "https://stream.example.com/live?token=fake0001",
        "fps": 60,
        "scroll": {"x": 0.0, "y": 0.0},
        "css": "",
        "panels": False,
        "cache": False,
    })
    sc = scene("Browser Demo", [item("Site", site)])
    return collection("syn-browser-basic", [sc, site], ["Browser Demo"])


def s02_browser_multiscene():
    site_a = source("browser_source", "Main Site", {
        "url": "https://a.example.com/index.html?page=2&sort=asc",
        "fps": 30,
        "css": "body { margin: 0; }",
        "panels": False,
        "cache": False,
    })
    site_b = source("browser_source", "Ticker", {
        "url": "http://b.example.com/ticker",
        "fps": 10,
        "css": "",
        "panels": True,
        "cache": True,
    })
    g_items = [item("Main Site", site_a, item_id=1),
               item("Ticker", site_b, pos=vec(0, 640), item_id=2)]
    g = group("Overlays", g_items)
    sc1 = scene("Show A", [item("Overlays", g, item_id=1)])
    sc2 = scene("Show B", [item("Main Site", site_a,
                                scale=vec(2, 2), rot=15.0, item_id=1),
                           item("Ticker", site_b, visible=False, item_id=2)])
    return collection("syn-browser-multiscene", [sc1, sc2, site_a, site_b],
                      ["Show A", "Show B"], groups=[g],
                      current_transition="Fade",
                      transitions=[transition("Fade", "fade_transition",
                                              {"color": 0, "duration": 300})],
                      quick_transitions=[quick("Cut"),
                                         quick("Fade", id=2)])


def s03_game_capture():
    game = source("game_capture", "Game", {
        "type": "auto",
        "window": "1189:0x000403e6:Example Game Window",
        "prefer_shared_memory": True,
        "auto_capture_rules_path":
            "/home/observer/.config/obs-studio/auto_capture_rules.json",
        "auto_placeholder_image": "/home/observer/pictures/placeholder.png",
        "capture_cursor": True,
    })
    win = source("window_capture", "Window", {
        "window": "1189:0x00040a2c:Capture Target",
        "cursor": True,
    })
    mon = source("monitor_capture", "Monitor", {"monitor_id": 1})
    sc = scene("Game Show", [item("Game", game, item_id=1),
                             item("Window", win, pos=vec(1280, 0),
                                  scale=vec(0.5, 0.5), item_id=2),
                             item("Monitor", mon, pos=vec(0, 360),
                                  scale=vec(0.5, 0.5), item_id=3)])
    return collection("syn-game-capture", [sc, game, win, mon], ["Game Show"])


def s04_media_playback():
    loop = source("ffmpeg_source", "Loop", {
        "local_file": "/home/observer/media/loop1080p.mp4",
        "local_restart": True,
        "hotkeys": True,
    }, hotkeys=MEDIA_HOTKEYS)
    clip = source("ffmpeg_source", "Clip", {
        "local_file": "C:\\Users\\observer\\media\\clip1080p.mp4",
        "local_restart": False,
    }, sync=250, deinterlace_mode=2, deinterlace_field_order=1,
           hotkeys=MEDIA_HOTKEYS)
    stream = source("ffmpeg_source", "Stream", {
        "input": "rtmp://ingest.example.com/app/fakekey?secret=fake0002",
    }, hotkeys=MEDIA_HOTKEYS)
    sc = scene("Media", [item("Loop", loop, item_id=1),
                         item("Clip", clip, pos=vec(960, 0), item_id=2),
                         item("Stream", stream, visible=False, item_id=3)])
    return collection("syn-media-playback", [sc, loop, clip, stream],
                      ["Media"])


def s05_vst_filters():
    backdrop = source("color_source", "Backdrop",
                      {"color": 16711936, "width": 1920, "height": 1080},
                      filters=[
                          source("vst_filter", "Reverb", {
                              "plugin_path":
                                  "/home/observer/VSTs/fake_reverb_vst.so",
                              "chunk_hash": "0123456789abcdef",
                              "chunk_data": "ZmFrZWNodW5rZGF0YQ==",
                          }),
                          source("vst3_filter", "EQ", {
                              "plugin_path": "/home/observer/VSTs/fake_vst3.so",
                              "chunk_hash": "fedcba9876543210",
                              "chunk_data": "ZmFrZWNodW5rZGF0YTI=",
                          }),
                      ])
    cam = source("dshow_input", "Camera", {
        "video_device_id": "fake-dshow-uuid-0001",
        "audio_device_id": "fake-dshow-uuid-0002",
        "video_format": "1280x720@30",
    }, filters=[
        source("vst3_filter", "Keyer", {
            "plugin_path": "C:\\Users\\observer\\VSTs\\fake_keyer_vst3.so",
            "chunk_hash": "a1b2c3d4e5f60718",
            "chunk_data": "ZmFrZWNodW5rZGF0YTM=",
        }),
    ])
    sc = scene("VST Demo", [item("Backdrop", backdrop, item_id=1),
                            item("Camera", cam, pos=vec(1520, 0),
                                 scale=vec(0.5, 0.5), item_id=2)])
    return collection("syn-vst-filters", [sc, backdrop, cam], ["VST Demo"])


def s06_image_text_color():
    logo = source("image_source", "Logo",
                  {"local_file": "/home/observer/pictures/logo.png"})
    title = source("text_ft2_source", "Title", {
        "text": "Corpus Title",
        "font": "DejaVu Sans",
        "size": 48.0,
        "color": 4294967295,
        "outline_color": 4278190080,
        "shadow_color": 1610612736,
        "outline": 2,
        "shadow": 0,
        "line_spacing": 0,
        "text_align": "left",
        "wrap": True,
        "radial_size": 0,
    })
    bg = source("color_source", "BG",
                {"color": 16711680, "width": 1920, "height": 1080})
    slides = source("slideshow", "Slides", {
        "images": ["/home/observer/slides/first.png",
                   "/home/observer/slides/second.png"],
        "duration": 5000,
        "fade": 500,
        "loop": True,
        "shuffle": False,
    })
    sc = scene("Graphics", [item("BG", bg, item_id=1),
                            item("Slides", slides, item_id=2),
                            item("Logo", logo, pos=vec(1600, 0),
                                 scale=vec(0.5, 0.5), item_id=3),
                            item("Title", title, pos=vec(60, 0), item_id=4)])
    return collection("syn-image-text-color", [sc, logo, title, bg, slides],
                      ["Graphics"])


def s07_capture_devices():
    dshow = source("dshow_input", "DShow Cam", {
        "video_device_id": "fake-dshow-uuid-0101",
        "audio_device_id": "fake-dshow-uuid-0102",
        "video_format": "1920x1080@30",
        "use_custom_video_format": False,
    })
    av = source("av_capture_input", "AVFoundation Cam",
                {"device": "fake-av-capture-device"})
    v4l2 = source("v4l2_input", "V4L2 Cam", {
        "video_device": "/dev/video0",
        "video_format": "1280x720@30",
        "video_input": 0,
        "audio_device": "",
        "fps_num": 30,
        "fps_den": 1,
    })
    xshm = source("xshm_input_v2", "X11 Screen", {"monitor": 0})
    xcomp = source("xcomposite_input", "X11 Window",
                   {"window": "fake-x11-window-id"})
    pw = source("pipewire-camera-source", "PipeWire Cam",
                {"device": "fake-pipewire-node"})
    dl = source("decklink-input", "DeckLink", {
        "video_device_id": "fake-decklink-0",
        "video_format": "1080p50",
        "power_line_frequency": 50,
        "hardware_convert": True,
        "color_space": 1,
        "color_range": 1,
    })
    pw_screen = source("pipewire-screen-capture-source", "PipeWire Screen",
                       {"device": "fake-pipewire-node-screen"})
    pw_window = source("pipewire-window-capture-source", "PipeWire Window",
                       {"device": "fake-pipewire-node-window"})
    syphon = source("syphon-input", "Syphon", {})
    dev = [dshow, av, v4l2, xshm, xcomp, pw, pw_screen, pw_window, syphon, dl]
    sc = scene("Devices", [item(s["name"], s, item_id=i + 1)
                           for i, s in enumerate(dev)])
    return collection("syn-capture-devices", [sc] + dev, ["Devices"])


def s08_audio_devices():
    audio = {
        "DesktopAudioDevice1":
            source("wasapi_output_capture", "Desktop Audio",
                   {"device_id": "default"}, mixers=255, muted=True),
        "DesktopAudioDevice2":
            source("coreaudio_output_capture", "Desktop Audio (2)",
                   {"device_id": "default"}, mixers=255),
        "AuxAudioDevice1":
            source("pulse_input_capture", "Mic (Pulse)",
                   {"device_id": "default"}, mixers=255),
        "AuxAudioDevice2":
            source("alsa_input_capture", "Mic (ALSA)",
                   {"device_id": "default"}, mixers=255),
        "AuxAudioDevice3":
            source("jack_output_capture", "Jack Bus", {"device_id": "0"},
                   mixers=255, monitoring_enabled=True, monitoring_type=2),
        "AuxAudioDevice4":
            source("oss_input_capture", "OSS Mic", {"device_id": "/dev/dsp"},
                   mixers=255, push_to_talk=True, ptt_delay=500),
    }
    sndio = source("sndio_output_capture", "SndIO Out", {"device_id": "0"},
                   mixers=0)
    proc = source("wasapi_process_output_capture", "App Audio", {
        "device_id": "default",
        "target": "1189:0x000403e6:Example Game Window",
    }, mixers=0)
    sc = scene("Audio Check", [item("SndIO Out", sndio, item_id=1),
                               item("App Audio", proc, item_id=2)])
    return collection("syn-audio-devices", [sc, sndio, proc],
                      ["Audio Check"], audio=audio)


def s09_transitions_all():
    trans = [
        transition("Cut", "cut_transition"),
        transition("Fade", "fade_transition",
                   {"color": 0, "duration": 300}, source_a="Game Show"),
        transition("Fade to Black", "fade_to_color_transition",
                   {"color": 0, "duration": 500}),
        transition("Fade to White", "fade_to_color_transition",
                   {"color": 4294967295, "duration": 500}),
        transition("Swipe", "swipe_transition",
                   {"duration": 400, "direction": 1, "angle": 0,
                    "fade": 10}),
        transition("Slide", "slide_transition",
                   {"duration": 400, "direction": 1, "fade": 10}),
        transition("Wipe", "wipe_transition", {"image": ""}),
        transition("Stinger", "obs_stinger_transition", {
            "source": "",
            "transition_id": 0,
            "show_duration": 1000,
            "fade_duration": 300,
        }),
    ]
    sc1 = scene("A", [item("A", source("color_source", "A Color",
                                       {"color": 16711680, "width": 1920,
                                        "height": 1080}), item_id=1)],
                id_counter=1)
    sc2 = scene("B", [], id_counter=0)
    return collection("syn-transitions-all", [sc1, sc2], ["A", "B"],
                      current_transition="Fade", transition_duration=400,
                      transitions=trans,
                      quick_transitions=[
                          quick("Cut"),
                          quick("Fade", id=2),
                          quick("Fade to Black", id=3, fade_to_black=True,
                                hotkeys=[{"control": True, "key": "F9"}]),
                      ])


def s10_groups_items():
    text = source("text_ft2_source", "Lower Text", {
        "text": "Lower Third", "font": "DejaVu Sans", "size": 24.0,
        "color": 4294967295, "outline": 2, "shadow": 0, "line_spacing": 0,
        "text_align": "left", "wrap": True,
    })
    img = source("image_source", "Lower Image",
                 {"local_file": "/home/observer/pictures/lower.png"})
    color = source("color_source", "Lower BG",
                   {"color": 33554432, "width": 1920, "height": 140})
    lower = group("Lower Thirds",
                  [item("Lower BG", color, item_id=1),
                   item("Lower Image", img, pos=vec(40, 10), item_id=2),
                   item("Lower Text", text, pos=vec(260, 30), item_id=3)])
    cam = source("dshow_input", "Overlay Cam", {
        "video_device_id": "fake-dshow-uuid-0201",
        "audio_device_id": "fake-dshow-uuid-0202",
        "video_format": "640x480@30",
    })
    overlay = group("Cam Overlay", [item("Overlay Cam", cam, item_id=1)])
    # the parent scene keeps the group_item_backup copies (skipped on load,
    # kept for older-version compatibility) in addition to the group items
    # inside each group's own settings
    main_items = [
        item("Main Cam", source("dshow_input", "Main Cam", {
            "video_device_id": "fake-dshow-uuid-0203",
            "audio_device_id": "fake-dshow-uuid-0204",
            "video_format": "1920x1080@30",
        }), item_id=1),
        item("Lower Thirds", lower, pos=vec(0, 940), item_id=2),
        item("Lower BG", color, group_item_backup=True, item_id=3),
        item("Lower Image", img, group_item_backup=True, pos=vec(40, 950),
             item_id=4),
        item("Lower Text", text, group_item_backup=True, pos=vec(260, 970),
             item_id=5),
        item("Cam Overlay", overlay, pos=vec(1520, 800), scale=vec(0.5, 0.5),
             locked=True, item_id=6),
        item("Overlay Cam", cam, group_item_backup=True, item_id=7),
    ]
    sc = scene("Grouped Show", main_items, id_counter=8)
    srcs = [lower, overlay, text, img, color, cam]
    return collection("syn-groups-items", [sc] + srcs, ["Grouped Show"],
                      groups=[lower, overlay])


def s11_item_transforms():
    bg = source("image_source", "BG Image",
                {"local_file": "/home/observer/pictures/wall.png"})
    a = source("image_source", "Alpha",
               {"local_file": "/home/observer/pictures/a.png"})
    b = source("image_source", "Beta",
               {"local_file": "/home/observer/pictures/b.png"})
    c = source("image_source", "Gamma",
               {"local_file": "/home/observer/pictures/c.png"})
    d = source("image_source", "Delta",
               {"local_file": "/home/observer/pictures/d.png"})
    e = source("image_source", "Epsilon",
               {"local_file": "/home/observer/pictures/e.png"})
    f = source("color_source", "Fill",
               {"color": 16711935, "width": 640, "height": 360})
    show = {"id": "fade", "versioned_id": "fade", "name": "Fade",
            "transition": {"color": 0}, "duration": 300}
    hide = {"id": "fade", "versioned_id": "fade", "name": "Fade",
            "transition": {"color": 0}, "duration": 500}
    items = [
        item("BG Image", bg, item_id=1),
        item("Alpha", a, rot=90.0, bounds_type=2, bounds_align=7,
             bounds=vec(480, 270), bounds_crop=True, crop=32,
             scale_filter="point", item_id=2),
        item("Beta", b, pos_rel=vec(-0.25, 0.5), scale_rel=vec(1.5, 1.5),
             scale_ref=vec(1920, 1080), scale_filter="bilinear", item_id=3),
        item("Gamma", c, scale_filter="bicubic", blend_type="additive",
             item_id=4),
        item("Delta", d, scale_filter="lanczos", blend_type="multiply",
             blend_method="srgb_off", item_id=5),
        item("Epsilon", e, scale_filter="area", blend_type="screen",
             show=show, hide=hide, item_id=6),
        item("Fill", f, locked=True, visible=False, item_id=7),
    ]
    sc = scene("Transforms", items, id_counter=8, custom_size=True,
               cx=2560, cy=1440)
    return collection("syn-item-transforms", [sc, bg, a, b, c, d, e, f],
                      ["Transforms"], resolution=(2560, 1440))


def s12_unicode():
    title = source("text_ft2_source", "タイトル – 标题", {
        "text": "Ümläuts öäü 日本語 テスト 한국어 🎥",
        "font": "Noto Sans CJK JP",
        "size": 32.0,
        "color": 4294967295,
        "outline": 1,
        "shadow": 0,
        "line_spacing": 4,
        "text_align": "center",
        "wrap": True,
    })
    web = source("browser_source", "Веб", {
        "url": "https://example.com/ümlaut?x=1&y=2",
        "fps": 30,
        "css": "",
        "panels": False,
        "cache": False,
    })
    cam = source("dshow_input", "カメラ", {
        "video_device_id": "fake-dshow-uuid-0301",
        "audio_device_id": "fake-dshow-uuid-0302",
        "video_format": "1280x720@30",
    })
    sc = scene("Szène 日本語", [
        item("BG", source("color_source", "背景",
                          {"color": 3355443, "width": 1920, "height": 1080}),
             item_id=1),
        item("タイトル – 标题", title, pos=vec(100, 100), item_id=2),
        item("Веб", web, pos=vec(100, 400), item_id=3),
        item("カメラ", cam, pos=vec(1300, 700), scale=vec(0.5, 0.5),
             item_id=4),
    ], id_counter=5)
    return collection("Szène – 日本語 – 🎥", [sc, title, web, cam],
                      ["Szène 日本語"])


def s13_canvases():
    main = canvas("Main Canvas")
    side = canvas("Secondary Canvas")
    main_uuid = main["info"]["uuid"]
    side_uuid = side["info"]["uuid"]
    browser = source("browser_source", "Browser (Main)", {
        "url": "https://main.example.com",
        "fps": 30,
        "css": "",
        "panels": False,
        "cache": False,
    }, canvas_uuid=main_uuid)
    media = source("ffmpeg_source", "Media (Side)", {
        "local_file": "/home/observer/media/side.mp4",
        "local_restart": True,
    }, canvas_uuid=side_uuid, hotkeys=MEDIA_HOTKEYS)
    sc_main = scene("Main Scene", [item("Browser (Main)", browser,
                                        item_id=1)], canvas_uuid=main_uuid)
    sc_side = scene("Side Scene", [item("Media (Side)", media, item_id=1)],
                    canvas_uuid=side_uuid)
    return collection("syn-canvases",
                      [sc_main, sc_side, browser, media],
                      ["Main Scene", "Side Scene"], canvases=[main, side],
                      current_program_scene="Side Scene")


def s14_projectors_vcam():
    browser = source("browser_source", "Stream Deck", {
        "url": "http://deck.example.com",
        "fps": 30,
        "css": "",
        "panels": False,
        "cache": False,
    })
    sc = scene("Studio", [item("Stream Deck", browser, item_id=1)])
    geo = base64.b64encode(
        b"\x00\x00\x04\xd4\x00\x00\x01\x00\x00\x00\x00\x03\xe8"
        b"\x00\x00\x01\xd0\x00\x00\x00\x00"
    ).decode("ascii")
    projectors = [
        {"name": "Studio", "monitor": 1, "type": 0, "geometry": geo,
         "alwaysOnTop": True, "alwaysOnTopOverridden": True},
        {"name": "Stream Deck", "monitor": 0, "type": 1, "geometry": geo,
         "alwaysOnTopOverridden": False},
    ]
    return collection("syn-projectors-vcam", [sc, browser], ["Studio"],
                      projectors=projectors,
                      virtual_camera={"type2": 3, "scene": "Studio"},
                      preview_locked=True, scaling_enabled=True,
                      scaling_level=2, scaling_off_x=12.5,
                      scaling_off_y=-7.0)


VIDEO_FILTERS = [
    ("crop_filter", "Crop", {"left": 0, "top": 40, "right": 0, "bottom": 40}),
    ("scale_filter", "Scale", {"scale_type": 1, "scale_x": 1920,
                               "scale_y": 1080, "filter": 3}),
    ("color_filter", "Color", {"brightness": 10.0, "contrast": 5.0,
                               "gamma": 2.0, "hue": 10.0, "saturation": -5.0,
                               "vibrance": 0.0, "white_level": 0.0,
                               "black_level": 0.0}),
    ("color_key_filter", "Key", {"color": 4278255360, "opacity": 80,
                                 "similarity": 10, "smoothness": 5,
                                 "mix": 10}),
    ("chroma_key_filter", "Chroma", {"method": 2, "similarity": 0.5,
                                     "smoothness": 0.1,
                                     "key_color": 4278255360,
                                     "apply_chroma": 0.2}),
    ("luma_key_filter", "Luma", {"opacity": 20, "similarity": 10,
                                 "smoothness": 5}),
    ("sharpness_filter", "Sharp", {"strength": 30}),
    ("scroll_filter", "Scroll", {"duration": 3000, "vertical": False,
                                 "wrap": True}),
    ("gpu_delay", "Delay", {"duration": 100, "buffer_size": 500}),
    ("async_delay_filter", "Async", {"duration": 50, "buffer_size": 250,
                                     "mode": 0}),
    ("sdr_on_hdr_filter", "SDRonHDR", {"tonemap_algorithm": 3}),
    ("hdr_tonemap_filter", "Tonemap", {"mode": 1, "sdr_white_point": 100}),
    ("clut_filter", "CLUT",
                   {"file": "/home/observer/luts/fake_clut.cube",
                    "opacity": 100}),
    ("mask_filter", "Mask", {"source": "Mask Source"}),
]


def s15_video_filters():
    mask = source("image_source", "Mask Source",
                  {"local_file": "/home/observer/pictures/mask.png"})
    filters = [source(fid, fname, fset) for fid, fname, fset in VIDEO_FILTERS]
    cam = source("dshow_input", "Filtered Cam", {
        "video_device_id": "fake-dshow-uuid-0401",
        "audio_device_id": "fake-dshow-uuid-0402",
        "video_format": "1920x1080@30",
    }, filters=filters)
    sc = scene("Filter Gallery", [item("Filtered Cam", cam, item_id=1)])
    return collection("syn-video-filters", [sc, cam, mask],
                      ["Filter Gallery"])


AUDIO_FILTERS = [
    ("gain_filter", "Gain", {"db": 3.0}),
    ("compressor_filter", "Compressor", {"threshold": -12, "ratio": 4,
                                         "attack": 3, "release": 100,
                                         "knee": 4, "makeup": 6, "auto_makeup": True}),
    ("expander_filter", "Expander", {"threshold": -30, "ratio": 3,
                                     "attack": 5, "release": 100,
                                     "knee": 6, "auto_gain": True}),
    ("upward_compressor_filter", "Upward", {"threshold": -20, "ratio": 2,
                                            "attack": 5, "release": 100,
                                            "knee": 4, "makeup": 0,
                                            "auto_makeup": False}),
    ("noise_gate_filter", "Gate", {"threshold": -50, "attack": 1,
                                   "release": 50, "auto_gain": True}),
    ("noise_suppress_filter", "NS", {"RNNoise": True, "NSIntensity": 0.7}),
    ("limiter_filter", "Limiter", {"limiter": -1.0, "comp": -6.0,
                                   "comp_threshold": -20.0,
                                   "comp_ratio": 3.0}),
    ("basic_eq_filter", "EQ", {
        "band_1": {"bandwidth": 0.3, "type": 0, "enabled": False, "gain": 0.0},
        "band_2": {"frequency": 100.0, "bandwidth": 1.0, "type": 1,
                   "enabled": True, "gain": 2.0},
        "band_3": {"frequency": 1000.0, "bandwidth": 1.0, "type": 2,
                   "enabled": True, "gain": -2.0},
        "band_4": {"frequency": 10000.0, "bandwidth": 1.0, "type": 3,
                   "enabled": False, "gain": 0.0},
    }),
    ("invert_polarity_filter", "Invert", {}),
]


def s16_audio_filters():
    mic = source("pulse_input_capture", "Mic", {"device_id": "default"},
                 mixers=255,
                 filters=[source(fid, fname, fset)
                          for fid, fname, fset in AUDIO_FILTERS])
    sc = scene("Audio Lab", [item("Mic", mic, item_id=1)])
    return collection("syn-audio-filters", [sc, mic], ["Audio Lab"])


def s17_text_gdiplus_vlc():
    text = source("text_gdiplus", "GDI+ Text", {
        "text": "GDI+ Caption",
        "font": "Arial",
        "size": 36.0,
        "color": 4294967295,
        "outline": 1,
        "shadow": 0,
        "text_align": "left",
        "wrap": True,
    })
    vlc = source("vlc_source", "VLC Clip", {
        "input": "/home/observer/media/vlc_clip.mkv",
        "playback_speed": 1.0,
    })
    vcam = source("virtualcam_output", "Virtual Cam", {
        "type2": 2,
    })
    sc = scene("Win Extras", [item("GDI+ Text", text, item_id=1),
                              item("VLC Clip", vlc, item_id=2)])
    return collection("syn-text-gdiplus-vlc", [sc, text, vlc, vcam],
                      ["Win Extras"])


def s18_v1_absolute():
    """version 1 (absolute coordinates): items carry pos/scale/bounds only —
    no pos_rel/scale_rel/scale_ref/bounds_rel/bounds_crop (the 33.0.1
    scene_save_item absolute branch)."""
    cam = source("dshow_input", "Cam", {
        "video_device_id": "fake-dshow-uuid-0501",
        "audio_device_id": "fake-dshow-uuid-0502",
        "video_format": "1920x1080@30",
    })
    title = source("text_ft2_source", "Title", {
        "text": "Absolute Layout", "font": "DejaVu Sans", "size": 48.0,
        "color": 4294967295, "outline": 2, "shadow": 0, "line_spacing": 0,
        "text_align": "left", "wrap": True,
    })
    sc = scene("Absolute Scene", [
        # absolute-branch items: the *rel/scale_ref keys are absent
        {k: v for k, v in item("Cam", cam, pos=vec(0, 0),
                               scale=vec(1.92, 1.08), item_id=1).items()
         if k not in ("pos_rel", "scale_rel", "scale_ref", "bounds_rel",
                      "bounds_crop")},
        {k: v for k, v in item("Title", title, pos=vec(80, 40),
                               rot=0.5, item_id=2).items()
         if k not in ("pos_rel", "scale_rel", "scale_ref", "bounds_rel",
                      "bounds_crop")},
    ], id_counter=3)
    return collection("syn-v1-absolute", [sc, cam, title], ["Absolute Scene"],
                      version=1, migration_resolution={"x": 1920,
                                                       "y": 1080})


BUILDERS = [
    s01_browser_basic, s02_browser_multiscene, s03_game_capture,
    s04_media_playback, s05_vst_filters, s06_image_text_color,
    s07_capture_devices, s08_audio_devices, s09_transitions_all,
    s10_groups_items, s11_item_transforms, s12_unicode, s13_canvases,
    s14_projectors_vcam, s15_video_filters, s16_audio_filters,
    s17_text_gdiplus_vlc, s18_v1_absolute,
]


def main(argv):
    outdir = argv[0] if argv else os.path.join(
        os.path.dirname(os.path.abspath(__file__)), "scenes")
    os.makedirs(outdir, exist_ok=True)
    written = 0
    for build in BUILDERS:
        doc = json.dumps(build(), indent=4, ensure_ascii=False) + "\n"
        text, counter = anonymise.anonymise_text(doc)
        fname = f"syn-{build.__name__[4:]}.json".replace("_", "-")
        path = os.path.join(outdir, fname)
        with open(path, "w", encoding="utf-8") as f:
            f.write(text)
        red = sum(counter.values())
        print(f"{fname}: {len(text)} bytes, {red} anonymised value(s)")
        written += 1
    print(f"generated {written} synthetic collection(s) in {outdir}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
