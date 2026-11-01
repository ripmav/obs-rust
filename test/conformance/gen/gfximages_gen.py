#!/usr/bin/env python3
"""Generates the gfximages conformance scripts (spec S-053) into ../scripts/gfximages/.

Usage:  python3 gfximages_gen.py        (from test/conformance/gen)
Every script embeds its fixture files as `file NAME HEX` lines (hand-built by gfximages_fixtures.py), so the
scripts are self-contained. The expected output is produced by `./run.sh generate gfximages`.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from gfximages_fixtures import *  # noqa

OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "scripts", "gfximages")
scripts = {}


def script(name, header, lines):
    scripts[name] = header.strip().splitlines(), lines


def F(name, data):
    return "file %s %s" % (name, data.hex() or "-")


R, G, B, W, K = (255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255), (0, 0, 0)
P5 = [R, G, B, W, K]


def solid(n, v):
    return [v] * n


# ---------------------------------------------------------------- direct libnsgif
two = gif(2, 2, [frame(2, 2, [0, 1, 2, 3], delay=10), frame(2, 2, [3, 2, 1, 0], delay=20)], palette=RGBW, loop=0)
still = gif(2, 2, [frame(2, 2, [0, 1, 2, 3])], palette=RGBW)
g87 = gif(2, 2, [frame(2, 2, [0, 1, 2, 3])], palette=RGBW, version=b"GIF87a")
cmt = gif(2, 2, [frame(2, 2, [0, 1, 2, 3], delay=3)], palette=RGBW, comment=b"hello", loop=5)
script("10-gif-basic", """
# libnsgif (vendored GIF decoder): header, logical screen, frame table, decoded canvas bytes.
# Fixtures are 2x2 GIFs with the palette red, green, blue, white. Pixels are RGBA bytes (NSGIF_BITMAP_FMT_R8G8B8A8).
# Demonstrates: scan/complete lifecycle, info fields (loop_max, background, global palette), per-frame fields,
# GIF87a acceptance, comment/application extensions, the four bitmap byte orders, out-of-range frame access.
""", [
    F("two.gif", two), F("still.gif", still), F("g87.gif", g87), F("cmt.gif", cmt),
    "echo == two frames, loop 0", "gif_new two.gif", "gif_scan", "gif_complete", "gif_info", "gif_frame 0", "gif_frame 1",
    "gif_frame 2", "gif_palette global", "gif_palette local 0", "gif_palette local 5",
    "echo == still image", "gif_new still.gif", "gif_scan", "gif_complete", "gif_info", "gif_frame 0",
    "echo == GIF87a", "gif_new g87.gif", "gif_scan", "gif_complete", "gif_info", "gif_frame 0",
    "echo == comment extension and NETSCAPE loop 5, delay 3 (below the default minimum)", "gif_new cmt.gif", "gif_scan",
    "gif_complete", "gif_info",
    "echo == byte orders of the decoded bitmap", "gif_new two.gif bgra", "gif_scan", "gif_complete", "gif_frame 0",
    "gif_new two.gif argb", "gif_scan", "gif_complete", "gif_frame 0",
    "gif_new two.gif abgr", "gif_scan", "gif_complete", "gif_frame 0",
])

# ---- timing
def tgif(delays, loop=None):
    return gif(1, 1, [frame(1, 1, [i % 2], delay=d) for i, d in enumerate(delays)], palette=[R, G], loop=loop)

script("11-gif-timing", """
# libnsgif frame delay handling (centiseconds). gif_info shows the delay exactly as stored in the file
# (frame_info.delay is NOT adjusted). nsgif_frame_prepare returns the delay after the substitution rule
# "delay < min -> default"; gif_delay MIN DEFAULT sets that rule (nsgif_set_frame_delay_behaviour). The last frame of
# a finite animation reports an infinite delay (4294967295). nsgif_reset restarts the animation.
""", [
    F("d.gif", tgif([0, 1, 2, 3, 10, 255, 65535])), F("two.gif", two),
    F("loop1.gif", tgif([4, 5], loop=1)), F("loop2.gif", tgif([4, 5], loop=2)),
    "echo == default behaviour", "gif_new d.gif", "gif_scan", "gif_complete", "gif_info", "gif_prepare 8",
    "echo == min 0 default 0 (no substitution)", "gif_new d.gif", "gif_delay 0 0", "gif_scan", "gif_complete", "gif_prepare 8",
    "echo == min 3 default 7", "gif_new d.gif", "gif_delay 3 7", "gif_scan", "gif_complete", "gif_prepare 8",
    "echo == min 65535 default 1", "gif_new d.gif", "gif_delay 65535 1", "gif_scan", "gif_complete", "gif_prepare 8",
    "echo == delay rule set after scanning", "gif_new d.gif", "gif_scan", "gif_delay 3 7", "gif_complete", "gif_prepare 8",
    "echo == frame_prepare over a finite animation (NETSCAPE count 1), then reset", "gif_new loop1.gif", "gif_scan",
    "gif_complete", "gif_info", "gif_prepare 6", "gif_reset", "gif_prepare 3",
    "echo == NETSCAPE count 2", "gif_new loop2.gif", "gif_scan", "gif_complete", "gif_prepare 8",
    "echo == infinite animation", "gif_new two.gif", "gif_scan", "gif_complete", "gif_prepare 6",
])

# ---- loop counts
def lgif(loop):
    return gif(1, 1, [frame(1, 1, [0], delay=5), frame(1, 1, [1], delay=5)], palette=[R, G], loop=loop)

lines = []
for lp in (None, 0, 1, 2, 255, 65534, 65535):
    lines += [F("l.gif", lgif(lp)), "echo == NETSCAPE loop count %s" % ("absent" if lp is None else lp), "gif_new l.gif",
              "gif_scan", "gif_complete", "gif_info"]
# malformed loop extensions
base = bytearray(lgif(0))
i = base.index(b"NETSCAPE2.0")
b1 = bytes(base); b2 = bytearray(base); b2[i + 11] = 2  # sub-block id 2 instead of 1
b3 = bytearray(base); b3[i + 12:i + 14] = b"\x00\x00"  # (unchanged semantic) keep
b4 = bytearray(base)
b4[i + 11 - 1] = 2  # block length 2 (too short for the loop count)
lines += [F("lbadid.gif", bytes(b2)), "echo == NETSCAPE sub-block id 2", "gif_new lbadid.gif", "gif_scan", "gif_complete", "gif_info",
          F("lbadlen.gif", bytes(b4)), "echo == NETSCAPE sub-block length 2", "gif_new lbadlen.gif", "gif_scan", "gif_complete", "gif_info"]
ani = bytearray(lgif(3)); j = ani.index(b"NETSCAPE2.0"); ani[j:j + 11] = b"ANIMEXTS1.0"
lines += [F("lani.gif", bytes(ani)), "echo == ANIMEXTS1.0 identifier instead of NETSCAPE2.0", "gif_new lani.gif", "gif_scan", "gif_complete", "gif_info"]
script("12-gif-loop", """
# libnsgif loop count (info.loop_max) as a function of the NETSCAPE2.0 / ANIMEXTS1.0 application extension.
# loop_max is the number of plays: 0 = forever (count 0), otherwise count + 1; without an extension it is 1.
# Malformed extensions are shown as well.
""", lines)

# ---- disposal
fr = [
    frame(3, 3, solid(9, 0), delay=5, disposal=1),
    frame(2, 2, solid(4, 1), x=1, y=1, delay=5, disposal=2),
    frame(1, 1, [2], x=0, y=0, delay=5, disposal=3),
    frame(1, 2, [3, 3], x=2, y=0, delay=5, disposal=0),
]
dg = gif(3, 3, fr, palette=P5, bg=4)
lines = [F("disp.gif", dg), "gif_new disp.gif", "gif_scan", "gif_complete", "gif_info"]
lines += ["gif_frame %d" % i for i in range(4)]
lines += ["echo == decode again out of order: 3, 1, 0, 2", "gif_frame 3", "gif_frame 1", "gif_frame 0", "gif_frame 2"]
for d in (0, 1, 2, 3, 4, 5, 6, 7):
    f2 = [frame(2, 2, solid(4, 0), delay=5, disposal=d), frame(1, 1, [1], x=1, y=0, delay=5, transp=None)]
    lines += [F("d%d.gif" % d, gif(2, 2, f2, palette=P5, bg=4)), "echo == disposal %d on frame 0 (frame 1 is one green pixel at 1,0)" % d,
              "gif_new d%d.gif" % d, "gif_scan", "gif_complete", "gif_info", "gif_frame 0", "gif_frame 1"]
for bgi in (0, 2):
    f2 = [frame(2, 2, solid(4, 3), delay=5, disposal=2), frame(1, 1, [0], x=1, y=0, delay=5)]
    lines += [F("bg%d.gif" % bgi, gif(2, 2, f2, palette=P5, bg=bgi)),
              "echo == restore-to-background uses the global table colour of the background index (%d)" % bgi,
              "gif_new bg%d.gif" % bgi, "gif_scan", "gif_complete", "gif_info", "gif_frame 1"]
script("13-gif-disposal", """
# libnsgif disposal methods and frame composition. Canvas 3x3, palette red/green/blue/white/black, background
# index 4 (black). Frame 0 fills the canvas (disposal 1 = keep), frame 1 is a 2x2 green square at (1,1) with
# disposal 2 (restore to background), frame 2 is one blue pixel at (0,0) with disposal 3 (restore previous),
# frame 3 a white 1x2 strip at (2,0). The second part tries every 3-bit disposal value 0..7.
""", lines)

# ---- transparency
f = [frame(2, 2, [0, 1, 2, 3], delay=5, transp=1, disposal=1), frame(2, 2, [1, 4, 1, 4], delay=5, transp=1, disposal=1)]
tg = gif(2, 2, f, palette=P5, bg=4)
f = [frame(2, 2, [0, 1, 2, 3], delay=5, transp=1, disposal=2), frame(2, 2, [1, 0, 1, 0], delay=5, transp=1, disposal=0)]
tg2 = gif(2, 2, f, palette=P5, bg=4)
f = [frame(2, 2, [0, 1, 2, 3], delay=5, transp=200, disposal=1), frame(2, 2, [0, 0, 0, 0], delay=5, disposal=1)]
tg3 = gif(2, 2, f, palette=P5, bg=4)
f = [frame(2, 2, [0, 1, 0, 1], delay=5, transp=0, palette=[B, W]), frame(2, 2, [1, 0, 1, 0], delay=5)]
tg4 = gif(2, 2, f, palette=P5)
script("14-gif-transparency", """
# libnsgif transparency via the graphic control extension. Shows: the pixel value written for the transparent
# index (alpha 0, colour bytes as listed), compositing a transparent pixel over the previous frame
# (disposal 1) versus over the background (disposal 2), a transparent index outside the palette (200), and a
# transparent index that refers to a local palette entry. frame_info.transparency reports the flag.
""", [
    F("t1.gif", tg), F("t2.gif", tg2), F("t3.gif", tg3), F("t4.gif", tg4),
    "echo == transparent index 1, frame 1 keeps previous (disposal 1)", "gif_new t1.gif", "gif_scan", "gif_complete", "gif_info",
    "gif_frame 0", "gif_frame 1",
    "echo == transparent index 1, frame 0 restored to background (disposal 2)", "gif_new t2.gif", "gif_scan", "gif_complete",
    "gif_info", "gif_frame 0", "gif_frame 1",
    "echo == transparent index 200 outside a 5 entry palette", "gif_new t3.gif", "gif_scan", "gif_complete", "gif_info",
    "gif_frame 0", "gif_frame 1",
    "echo == transparent index 0 with a local palette", "gif_new t4.gif", "gif_scan", "gif_complete", "gif_info",
    "gif_frame 0", "gif_frame 1",
])

# ---- palettes / geometry / interlace
rows = [i % 4 for i in range(4 * 8)]
il = gif(4, 8, [frame(4, 8, [(y * 4 + x) % 4 + (y % 3) for y in range(8) for x in range(4)], interlaced=True)], palette=P5 + [(9, 9, 9)] * 3)
nl = gif(4, 8, [frame(4, 8, [(y * 4 + x) % 4 + (y % 3) for y in range(8) for x in range(4)], interlaced=False)], palette=P5 + [(9, 9, 9)] * 3)
nopal = gif(2, 2, [frame(2, 2, [0, 1, 0, 1], delay=5)], palette=None)
big = gif(2, 2, [frame(4, 4, [0, 1, 2, 3] * 4, delay=5)], palette=RGBW)
off = gif(2, 2, [frame(2, 2, [0, 1, 2, 3], x=1, y=1, delay=5)], palette=RGBW)
off2 = gif(2, 2, [frame(2, 2, [0, 1, 2, 3], x=5, y=5, delay=5), frame(2, 2, [3, 2, 1, 0], delay=5)], palette=RGBW)
script("15-gif-palette-geometry", """
# libnsgif colour tables and frame geometry: interlaced vs. sequential row order of the same pixel data, a GIF
# without any colour table, a frame larger than the logical screen, a frame partly or fully outside the screen.
""", [
    F("il.gif", il), F("nl.gif", nl), F("nopal.gif", nopal), F("big.gif", big), F("off.gif", off), F("off2.gif", off2),
    "echo == interlaced (row order 0 | 4 | 2 6 | 1 3 5 7 in the stream)", "gif_new il.gif", "gif_scan", "gif_complete", "gif_info", "gif_frame 0",
    "echo == the same pixels, sequential", "gif_new nl.gif", "gif_scan", "gif_complete", "gif_info", "gif_frame 0",
    "echo == no colour table anywhere", "gif_new nopal.gif", "gif_scan", "gif_complete", "gif_info", "gif_frame 0", "gif_palette global",
    "echo == frame 4x4 on a 2x2 screen", "gif_new big.gif", "gif_scan", "gif_complete", "gif_info", "gif_frame 0",
    "echo == frame at (1,1) extends past the screen", "gif_new off.gif", "gif_scan", "gif_complete", "gif_info", "gif_frame 0",
    "echo == first frame entirely outside the screen", "gif_new off2.gif", "gif_scan", "gif_complete", "gif_info", "gif_frame 0", "gif_frame 1",
])

# ---- errors
ok2 = two
lines = [F("ok.gif", ok2)]
lines += ["echo == scan the first N bytes only (progressive input), then the rest"]
lines += ["gif_new ok.gif", "gif_scan 5", "gif_scan 13", "gif_scan 20", "gif_scan 60", "gif_info", "gif_scan", "gif_info"]
for n in (0, 3, 6, 10, 13, 14, 20, 30, 50, 70, 92):
    lines += ["trunc ok.gif t%d.gif %d" % (n, n), "echo == truncated to %d bytes" % n, "gif_new t%d.gif" % n, "gif_scan", "gif_info",
              "gif_complete", "gif_info", "gif_frame 0", "gif_frame 1"]
bad = bytearray(ok2); bad[0:6] = b"GIF99a"
lines += [F("badsig.gif", bytes(bad)), "echo == bad version", "gif_new badsig.gif", "gif_scan", "gif_complete", "gif_info"]
bad = bytearray(ok2); bad[0:3] = b"PNG"
lines += [F("badmagic.gif", bytes(bad)), "echo == bad magic", "gif_new badmagic.gif", "gif_scan", "gif_complete", "gif_info"]
nt = gif(2, 2, [frame(2, 2, [0, 1, 2, 3], delay=5)], palette=RGBW, trailer=False)
lines += [F("notrailer.gif", nt), "echo == no trailer byte", "gif_new notrailer.gif", "gif_scan", "gif_info", "gif_complete", "gif_info", "gif_frame 0"]
z = gif(0, 0, [frame(1, 1, [0], delay=5)], palette=RGBW)
lines += [F("zero.gif", z), "echo == logical screen 0x0", "gif_new zero.gif", "gif_scan", "gif_complete", "gif_info"]
z = gif(2, 2, [frame(0, 0, [], delay=5)], palette=RGBW)
lines += [F("zeroframe.gif", z), "echo == frame 0x0", "gif_new zeroframe.gif", "gif_scan", "gif_complete", "gif_info", "gif_frame 0"]
z = gif(2, 2, [], palette=RGBW)
lines += [F("noframes.gif", z), "echo == header and trailer only", "gif_new noframes.gif", "gif_scan", "gif_complete", "gif_info", "gif_frame 0"]
lines += ["echo == scan after complete, complete twice, destroy", "gif_new ok.gif", "gif_scan", "gif_complete", "gif_complete", "gif_scan", "gif_info", "gif_destroy"]
script("16-gif-errors", """
# libnsgif error handling: progressive scanning, truncated files at many offsets, bad signature, missing
# trailer, degenerate dimensions. Every result is the library's error string for the return code
# (nsgif_strerror). `trunc` cuts a fixture; gif_scan N feeds only the first N bytes.
""", lines)

# ---- LZW
import random
rnd = random.Random(53)
lines = []
for mc in (2, 3, 4, 5, 6, 7, 8):
    npal = 1 << mc
    pal = [(i, 255 - i, (i * 7) & 255) for i in range(npal)]
    px = [rnd.randrange(npal) for _ in range(16)]
    lines += [F("m%d.gif" % mc, gif(4, 4, [frame(4, 4, px, delay=5, min_code=mc)], palette=pal)), "echo == min code size %d (palette %d)" % (mc, npal),
              "gif_new m%d.gif" % mc, "gif_scan", "gif_complete", "gif_frame 0"]
# table overflow: 80x80 pseudo random 16-colour image forces 12-bit codes and table clears
px = [rnd.randrange(16) for _ in range(80 * 80)]
pal16 = [(i * 16, i * 8, 255 - i * 16) for i in range(16)]
big = gif(80, 80, [frame(80, 80, px, delay=5, min_code=4)], palette=pal16)
lines += [F("big.gif", big), "echo == 80x80 random, 12-bit codes and table clear (bytes in file: %d)" % len(big), "gif_new big.gif", "gif_scan", "gif_complete", "gif_info", "gif_frame 0"]
# runs: highly compressible
px = [0] * 4096
lines += [F("run.gif", gif(64, 64, [frame(64, 64, px, delay=5, min_code=2)], palette=RGBW)), "echo == 64x64 single colour (long runs)",
          "gif_new run.gif", "gif_scan", "gif_complete", "gif_frame 0"]
# minimum code sizes 0, 1, 9..12 declared
for mc in (0, 1, 9, 12, 13):
    data = lzw_encode([0, 1, 0, 1], 2)
    lines += [F("bm%d.gif" % mc, gif(2, 2, [frame(2, 2, [0, 1, 0, 1], delay=5, min_code=mc, raw_data=sub_blocks(data))], palette=RGBW)),
              "echo == declared LZW minimum code size %d (data encoded for 2)" % mc, "gif_new bm%d.gif" % mc, "gif_scan", "gif_complete", "gif_info",
              "gif_frame 0"]
# corrupt streams
good = lzw_encode([0, 1, 2, 3], 2)
for nm, data in (("empty", b""), ("junk", b"\xff\xff\xff\xff"), ("short", good[:2]), ("noeoi", good[:-1]),
                 ("badcode", bytes([0b01001100, 0b11111111, 0xff]))):
    lines += [F("c_%s.gif" % nm, gif(2, 2, [frame(2, 2, [0, 1, 2, 3], delay=5, raw_data=sub_blocks(data))], palette=RGBW)),
              "echo == LZW data: %s" % nm, "gif_new c_%s.gif" % nm, "gif_scan", "gif_complete", "gif_info", "gif_frame 0"]
script("17-gif-lzw", """
# libnsgif LZW layer (lzw.c): all legal minimum code sizes 2..8, a 12-bit table overflow with clear codes,
# long runs, illegal declared minimum code sizes (0, 1, 9, 12, 13), and corrupt code streams (empty,
# junk, truncated, missing end code, code beyond the table).
""", lines)

# ---------------------------------------------------------------- gs_image_file_ex with GIF

def agif(frames, loop=None, w=2, h=2, pal=RGBW, bg=0):
    return gif(w, h, frames, palette=pal, loop=loop, bg=bg)

f3 = [frame(2, 2, solid(4, 0), delay=10), frame(2, 2, solid(4, 1), delay=5), frame(2, 2, solid(4, 2), delay=0)]
lines = [F("a3.gif", agif(f3, loop=0)), F("a3l1.gif", agif(f3, loop=1)), F("a3l2.gif", agif(f3, loop=2))]
lines += ["echo == infinite animation (3 frames, delays 10, 5, 0 cs)", "img_init a3.gif straight", "img_tex_init", "img_tex",
          "img_tick 99999999", "img_tick 1", "img_tick 1", "img_update", "img_tex",
          "img_tick 50000000 2", "img_update", "img_tex", "img_tick 100000000 3", "img_update", "img_tex",
          "echo == state after a 1 s tick (frame 2 has delay 0 = 100 ms)", "img_tick 1000000000", "img_state", "img_free"]
lines += ["echo == NETSCAPE count 1 = loop_max 2 = two plays: stops on the last frame", "img_init a3l1.gif straight", "img_tex_init",
          "img_tick 150000000", "img_update", "img_tex", "img_tick 150000000", "img_tick 1000000000 3", "img_state", "img_update", "img_tex", "img_free"]
lines += ["echo == NETSCAPE count 2 = three plays", "img_init a3l2.gif straight", "img_tex_init", "img_tick 150000000", "img_tick 150000000", "img_tick 1000000000",
          "img_tick 150000000", "img_tick 1000000000 2", "img_state", "img_free"]
lines += ["echo == one huge tick spans several loops", "img_init a3l2.gif straight", "img_tex_init", "img_tick 10000000000", "img_state", "img_free"]
lines += ["echo == one huge tick on an infinite animation", "img_init a3.gif straight", "img_tex_init", "img_tick 10000000000", "img_state", "img_free"]
script("20-image-gif-animation", """
# gs_image_file_ex_* with animated GIFs: the clock (nanoseconds, 1 cs = 10,000,000 ns), looping, the stop at the
# end of a finite animation and the texture contents after gs_image_file_ex_update_texture. The texture lives
# on a fake backend that keeps its pixels, so `img_tex` shows exactly what the caller would upload.
# Fixture frames are solid colour (frame 0 red, 1 green, 2 blue); delays 10, 5 and 0 centiseconds.
""", lines)

# alpha modes with transparency
tf = [frame(2, 2, [0, 1, 2, 3], delay=5, transp=1, disposal=2), frame(2, 2, [1, 1, 3, 3], delay=5, transp=1, disposal=2)]
ta = agif(tf, loop=0, pal=P5)
lines = [F("ta.gif", ta)]
for mode in ("straight", "srgb", "premul"):
    lines += ["echo == alpha mode %s" % mode, "img_init ta.gif %s" % mode, "img_state", "img_tex_init", "img_tex", "img_tick 60000000", "img_update",
              "img_tex", "img_tick 60000000", "img_update", "img_tex", "img_free"]
script("21-image-gif-alpha", """
# gs_image_file_ex_init alpha modes for animated GIFs. Only the frame decoded by init is premultiplied; frames
# decoded later by tick/update_texture are always straight (the exported entry points pass STRAIGHT). Frame
# 0 has a transparent pixel (index 1); with disposal 2 the next frame is composed on the background.
""", lines)

# dimension limits / failures
w4097 = agif([frame(4097, 1, [0] * 4097, delay=5)], w=4097, h=1)
w4097b = agif([frame(4097, 1, [0] * 4097, delay=5), frame(4097, 1, [1] * 4097, delay=5)], w=4097, h=1)
h4096 = agif([frame(1, 4096, [0] * 4096, delay=5), frame(1, 4096, [1] * 4096, delay=5)], w=1, h=4096)
w4096 = agif([frame(4096, 1, [0] * 4096, delay=5), frame(4096, 1, [1] * 4096, delay=5)], w=4096, h=1)
one = agif([frame(2, 2, [0, 1, 2, 3])])
badlzw = gif(2, 2, [frame(2, 2, [0, 1, 0, 1], delay=5, min_code=12, raw_data=sub_blocks(lzw_encode([0, 1, 0, 1], 2)))], palette=RGBW)
lines = [F("badlzw.gif", badlzw), F("w4097.gif", w4097), F("w4097b.gif", w4097b), F("h4096.gif", h4096), F("w4096.gif", w4096), F("one.gif", one),
         F("fake.gif", png(1, 1, 6, 8, [bytes([255, 0, 0, 255])])), F("junk.gif", b"not a gif at all"), F("empty.gif", b""),
         F("x.GIF", two), F("ab.gif", two)]
lines += ["echo == 4097x1, one frame: rejected even though a still image would be fine elsewhere", "img_init w4097.gif straight", "img_state",
          "echo == 4097x1, two frames", "img_init w4097b.gif straight", "img_state",
          "echo == 1x4096 two frames (limit is inclusive)", "img_init h4096.gif straight", "img_state", "img_free",
          "echo == 4096x1 two frames", "img_init w4096.gif straight", "img_state", "img_free",
          "echo == a single-frame GIF is not animated: loaded by the generic still-image path", "img_init one.gif straight", "img_state", "img_pixels", "img_free",
          "echo == a PNG named .gif: DEFECT D-1, the reference never returns (scan error is not detected)", "img_try fake.gif straight",
          "echo == garbage named .gif (same hang)", "img_try junk.gif straight",
          "echo == empty file named .gif: DEFECT D-2, bmalloc(0) invokes the crash handler", "img_try empty.gif straight",
          "echo == upper-case extension .GIF is recognised (case-insensitive)", "img_init x.GIF straight", "img_state", "img_free",
          F("two.gif", two),
          "echo == truncated animated GIFs (img_try: the child is killed after 2 s)",
          "trunc two.gif cut13.gif 13", "img_try cut13.gif straight", "trunc two.gif cut50.gif 50", "img_try cut50.gif straight",
          "trunc two.gif cut70.gif 70", "img_try cut70.gif straight", "trunc two.gif cut92.gif 92", "img_try cut92.gif straight",
          "echo == a GIF whose first frame has an illegal LZW minimum code size", "img_try badlzw.gif straight",
          "echo == missing file", "img_init nothere.gif straight", "img_state",
          "echo == NULL path (clears the image)", "img_init_null", "img_free", "img_free"]
script("22-image-gif-limits", """
# gs_image_file_ex_init on GIF paths: the 4096 limit, single-frame GIFs, misnamed files and missing files.
# Whether a path is treated as GIF is decided only by its last four characters (case-insensitive ".gif",
# and the name must be longer than four characters).
""", lines)

# frame sequencing: decode order / caching quirks
f6 = [frame(1, 1, [i % 4], delay=1 + i) for i in range(6)]
lines = [F("s6.gif", agif(f6, loop=0, w=1, h=1)), "img_init s6.gif straight", "img_tex_init", "img_tex", "echo == skip ahead", "img_tick 100000000 1"]
# delay of 1 cs is below the library minimum (2): becomes the default 10
lines += ["img_state"]
lines += ["img_tick 100000000", "img_update", "img_tex", "img_tick 300000000", "img_update", "img_tex", "img_tick 1000000000", "img_update", "img_tex",
          "img_state", "img_free"]
script("23-image-gif-sequencing", """
# gs_image_file_ex tick/update with jumps over several frames at once (frames in between are decoded to keep
# the library's canvas consistent), and a looped-around jump. 1x1 pixel, six frames with delays 1..6 cs.
""", lines)

# frame that fails to decode
junk = sub_blocks(b"\xff\xff\xff\xff")
bf = [frame(2, 2, solid(4, 0), delay=5), frame(2, 2, solid(4, 1), delay=5, raw_data=junk), frame(2, 2, solid(4, 2), delay=5)]
lines = [F("bf.gif", agif(bf, loop=0)),
         "echo == frame 1 holds a corrupt LZW stream: init logs it and goes on", "img_init bf.gif straight", "img_tex_init", "img_tex",
         "echo == update_texture before any tick: frame 0 is not cached yet and is decoded now", "img_update", "img_tex",
         "echo == tick onto the frame that failed at init: the tick reports a change and the frame later shows the previous canvas", "img_tick 50000001", "img_state",
         "img_update", "img_tex",
         "echo == tick on to frame 2", "img_tick 50000000", "img_state",
         "img_update", "img_tex", "img_tick 50000000 3", "img_free"]
script("24-image-gif-decode-failure", """
# gs_image_file_ex with an animation whose middle frame cannot be decoded (corrupt LZW data, header and table
# scan fine). Shows the error handling in init/tick/update_texture: the failure is only logged (init); later
# ticks switch to the frame normally and the frame then shows the previous canvas.
""", lines)

# ---------------------------------------------------------------- still images
rgba = png(2, 2, 6, 8, [bytes([255, 0, 0, 255, 0, 255, 0, 128]), bytes([0, 0, 255, 0, 255, 255, 255, 64])])
rgb = png(2, 2, 2, 8, [bytes([255, 0, 0, 0, 255, 0]), bytes([0, 0, 255, 255, 255, 255])])
gray = png(2, 2, 0, 8, [bytes([0, 85]), bytes([170, 255])])
ga = png(2, 2, 4, 8, [bytes([0, 255, 85, 128]), bytes([170, 64, 255, 0])])
pal = png(2, 2, 3, 8, [bytes([0, 1]), bytes([2, 3])], plte=[R, G, B, W])
pal_t = png(2, 2, 3, 8, [bytes([0, 1]), bytes([2, 3])], plte=[R, G, B, W], trns=bytes([255, 128, 0]))
rgb_t = png(2, 1, 2, 8, [bytes([255, 0, 0, 0, 255, 0])], trns=struct.pack(">HHH", 255, 0, 0))
rgba16 = png(2, 1, 6, 16, [struct.pack(">8H", 65535, 0, 0, 65535, 0, 32768, 65535, 32768)])
rgb16 = png(1, 1, 2, 16, [struct.pack(">3H", 65535, 32768, 0)])
g16 = png(1, 1, 0, 16, [struct.pack(">H", 40000)])
wide = png(3, 2, 6, 8, [bytes([255, 0, 0, 255, 0, 255, 0, 255, 0, 0, 255, 255]), bytes([1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12])])
one_px = png(1, 1, 6, 8, [bytes([200, 100, 50, 200])])
lines = []
for nm, data in (("rgba", rgba), ("rgb", rgb), ("gray", gray), ("ga", ga), ("pal", pal), ("pal_t", pal_t), ("rgb_t", rgb_t),
                 ("rgba16", rgba16), ("rgb16", rgb16), ("g16", g16), ("wide", wide)):
    lines.append(F(nm + ".png", data))
    for mode in ("straight", "srgb", "premul"):
        lines.append("echo == %s.png alpha mode %s" % (nm, mode))
        lines.append("texfile %s.png %s" % (nm, mode))
script("30-still-png", """
# gs_create_texture_file_data3 on hand-built PNG files (decoded by FFmpeg): output format (RGBA / BGRA / BGRX /
# RGBA16 by the decoder's pixel format), pixel bytes and the three alpha modes. 8-bit RGBA is copied (or
# premultiplied) as RGBA; 16-bit RGBA becomes 8-bit through float conversion (format reported as in the source
# path); everything else goes through swscale to BGRA. Colour tables and tRNS exercise the conversion.
""", lines)

lines = [F("bmp24.bmp", bmp24(2, 2, [[R, G], [B, W]])), F("img.ppm", ppm(2, 2, [[R, G], [B, W]])),
         F("noext", rgba), F("trunc.png", rgba[:40]), F("junk.png", b"this is not an image"), F("empty.png", b""), F("w3.png", wide), F("one.png", one_px)]
for nm in ("bmp24.bmp", "img.ppm", "noext", "trunc.png", "junk.png", "empty.png", "one.png"):
    lines += ["echo == %s" % nm, "texfile %s straight" % nm]
lines += ["echo == file errors", "texfile_missing"]
lines += ["echo == gs_image_file_ex_init on still images", "img_init rgba.png premul" if False else "img_init one.png premul", "img_state", "img_pixels", "img_tex_init", "img_tex", "img_state", "img_free",
          "img_init one.png srgb", "img_pixels", "img_free", "img_init one.png straight", "img_pixels", "img_free",
          "img_init junk.png straight", "img_state", "img_init nothere.png straight", "img_state",
          "echo == still tick/update are no-ops", "img_init one.png straight", "img_tex_init", "img_tick 1000000000", "img_update", "img_tex", "img_free"]
lines.insert(0, F("rgba.png", rgba))
script("31-still-formats-errors", """
# Other still formats (BMP, PPM), a PNG without file extension (content sniffing), damaged files, and the
# gs_image_file_ex_init path for non-animated images: texture data ownership, mem_usage and the texture hand-over
# (the still pixels are freed once the texture exists).
""", lines)


# ---------------------------------------------------------------- frame reformatting and orientation (fabricated frames)
def px(i):  # 4 bytes naming the pixel: value i in the first byte
    return bytes([i, 0x10 + i, 0x20 + i, 0xFF])

img32 = b"".join(px(i) for i in range(6))  # 3 wide, 2 high
lines = []
for o in range(0, 9):
    lines.append("reformat bgr0 3 2 12 %d straight %s" % (o, img32.hex()))
lines.insert(0, "echo == orientation values 0..8 on a 3x2 image, pixel i = bytes (i, 16+i, 32+i, ff)")
lines += ["echo == orientation on a 4x3 image (non-square, odd tile edges)"]
img43 = b"".join(px(i) for i in range(12))
for o in (2, 3, 4, 5, 6, 7, 8):
    lines.append("reformat rgba 4 3 16 %d straight %s" % (o, img43.hex()))
big = b"".join(bytes([i & 255, (i >> 8) & 255, 0, 255]) for i in range(70 * 3))  # width 70 crosses the 64 pixel tile
lines += ["echo == 70x3 image rotated (tile size boundary)"]
lines.append("reformat bgr0 70 3 280 6 straight %s" % big.hex())
lines.append("reformat bgr0 70 3 280 5 straight %s" % big.hex())
script("32-reformat-orientation", """
# graphics-ffmpeg.c ffmpeg_image_reformat_frame / ffmpeg_image_orient on fabricated decoded frames. The frame
# metadata entry "Orientation" selects one of the EXIF orientations 1..8 (0 and 1 = untouched; values above 8
# are outside the contract: the reference then leaves most of the output buffer uninitialised, so they are not tested). Orientations 5..8 swap width and height. The
# reference only reaches this code when the decoder reports the entry as metadata (FFmpeg < 7.1 does; the
# FFmpeg installed for the vectors reports a display matrix instead, so real-file vectors never rotate).
""", lines)

rgba_in = bytes([255, 0, 0, 255, 0, 255, 0, 128, 0, 0, 255, 0, 200, 100, 50, 200])
bgra_in = rgba_in
pad = lambda rows, extra: b"".join(r + b"\xee" * extra for r in rows)
lines = []
for pix in ("rgba", "bgra"):
    for mode in ("straight", "srgb", "premul"):
        lines.append("reformat %s 2 2 8 0 %s %s" % (pix, mode, rgba_in.hex()))
lines.append("echo == padded lines (linesize larger than the row)")
for mode in ("straight", "srgb", "premul"):
    lines.append("reformat rgba 2 2 12 0 %s %s" % (mode, pad([rgba_in[0:8], rgba_in[8:16]], 4).hex()))
lines.append("echo == bgr0: straight copy whatever the alpha mode; the fourth byte is not touched")
for mode in ("straight", "premul"):
    lines.append("reformat bgr0 2 2 8 0 %s %s" % (mode, rgba_in.hex()))
lines.append("echo == 16-bit RGBA big-endian")
rgba64 = struct.pack(">8H", 65535, 0, 0, 65535, 0, 32768, 65535, 32768)
for mode in ("straight", "srgb", "premul"):
    lines.append("reformat rgba64be 2 1 16 0 %s %s" % (mode, rgba64.hex()))
lines.append("reformat rgba64be 1 1 12 0 straight %s" % (rgba64[:8] + b"\x00" * 4).hex())
lines.append("echo == formats converted through swscale to BGRA, then premultiplied by the alpha mode")
lines.append("reformat rgb24 2 1 6 0 straight ff000000ff00")
lines.append("reformat rgb24 2 1 6 0 premul ff000000ff00")
lines.append("reformat gray8 2 1 2 0 straight 40c0")
lines.append("reformat yuv420p 2 2 2 0 straight 10f080a0")
lines.append("reformat yuv420p 2 2 2 0 srgb 10f080a0")
lines.append("reformat nosuchformat 1 1 4 0 straight 00000000")
lines.append("echo == orientation combined with premultiplication")
lines.append("reformat rgba 2 2 8 6 premul %s" % rgba_in.hex())
lines.append("reformat gray8 2 1 2 8 straight 40c0")
script("33-reformat-pixel-formats", """
# graphics-ffmpeg.c ffmpeg_image_reformat_frame by input pixel format and alpha mode (fabricated decoded
# frames): which formats are copied, which are converted by swscale (always to BGRA), 16-bit conversion to 8-bit,
# line padding, and the resulting texture format (RGBA / BGRA / BGRX).
""", lines)

# ---------------------------------------------------------------- built-in effects
effects = sorted(os.path.basename(p) for p in os.listdir(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "..", "libobs", "data")) if p.endswith(".effect"))
groups = {
    "40-effects-default-color": ["default.effect", "default_rect.effect", "solid.effect", "color.effect", "opaque.effect",
                                 "premultiplied_alpha.effect", "repeat.effect"],
    "41-effects-scaling": ["area.effect", "bicubic_scale.effect", "bilinear_lowres_scale.effect", "lanczos_scale.effect"],
    "42-effects-deinterlace": ["deinterlace_base.effect"] + [e for e in effects if e.startswith("deinterlace_") and e != "deinterlace_base.effect"],
    "43-effects-format-conversion": ["format_conversion.effect"],
}
for gname, files in groups.items():
    script(gname, """
# Built-in effect files of libobs/data/ loaded through the real effect parser (spec S-020) against a fake
# backend. Per file: parser result, parameters (type, default, annotations), techniques, passes with the
# entry functions of both stages and the effect parameters each stage reads, the number of functions, structs and
# samplers, and sampler states. The repository relative path is also the label.
""", ["effect libobs/data/%s" % f for f in files])

os.makedirs(OUT, exist_ok=True)
for name, (hdr, lines) in sorted(scripts.items()):
    with open(os.path.join(OUT, name + ".script"), "w") as fh:
        for h in hdr:
            fh.write("# " + h.lstrip("# ").rstrip() + "\n" if not h.startswith("#") else h + "\n")
        for l in lines:
            fh.write(l + "\n")
print("wrote %d scripts" % len(scripts))
