#!/usr/bin/env python3
"""Hand-built GIF / PNG / BMP / PPM byte streams for the gfximages conformance scripts (spec S-053).

Only the Python standard library is used. The functions return `bytes`; gfximages_gen.py turns them into
`file NAME HEX` script lines. Nothing here is derived from the C implementation.
"""
import struct
import zlib


def lzw_encode(indices, min_code_size):
    """Standard GIF LZW (variable code width 3..12 bits, clear/end codes, table reset when full)."""
    clear = 1 << min_code_size
    eoi = clear + 1
    out = bytearray()
    acc = 0
    nbits = 0

    def emit(code, width):
        nonlocal acc, nbits
        acc |= code << nbits
        nbits += width
        while nbits >= 8:
            out.append(acc & 0xFF)
            acc >>= 8
            nbits -= 8

    width = min_code_size + 1
    table = {}
    nxt = eoi + 1
    emit(clear, width)
    if not indices:
        emit(eoi, width)
    else:
        cur = (indices[0],)
        for v in indices[1:]:
            cand = cur + (v,)
            if cand in table or len(cand) == 1:
                cur = cand
                continue
            code = table[cur] if len(cur) > 1 else cur[0]
            emit(code, width)
            table[cand] = nxt
            nxt += 1
            if nxt > (1 << width) and width < 12:
                width += 1
            if nxt >= 4096:
                emit(clear, width)
                table = {}
                nxt = eoi + 1
                width = min_code_size + 1
            cur = (v,)
        code = table[cur] if len(cur) > 1 else cur[0]
        emit(code, width)
        emit(eoi, width)
    if nbits:
        out.append(acc & 0xFF)
    return bytes(out)


def sub_blocks(data):
    out = bytearray()
    for i in range(0, len(data), 255):
        chunk = data[i:i + 255]
        out.append(len(chunk))
        out += chunk
    out.append(0)
    return bytes(out)


def palette_bytes(colors):
    return b"".join(bytes(c) for c in colors)


def pad_palette(colors):
    n = 2
    while n < len(colors):
        n *= 2
    return list(colors) + [(0, 0, 0)] * (n - len(colors)), n.bit_length() - 1


def interlace_order(h):
    rows = list(range(0, h, 8)) + list(range(4, h, 8)) + list(range(2, h, 4)) + list(range(1, h, 2))
    return rows


def frame(w, h, pixels, x=0, y=0, delay=None, disposal=0, transp=None, palette=None, interlaced=False,
          min_code=None, raw_data=None, gce=True):
    return dict(w=w, h=h, pixels=pixels, x=x, y=y, delay=delay, disposal=disposal, transp=transp, palette=palette,
                interlaced=interlaced, min_code=min_code, raw_data=raw_data, gce=gce)


def gif(width, height, frames, palette=None, loop=None, bg=0, version=b"GIF89a", trailer=True, comment=None,
        plain_ext=False):
    """frames: list of frame(); palette: global colour table (list of RGB) or None; loop: NETSCAPE count or None."""
    out = bytearray(version)
    flags = 0
    gp = b""
    if palette is not None:
        padded, bits = pad_palette(palette)
        flags = 0x80 | (bits - 1) << 4 | (bits - 1)
        gp = palette_bytes(padded)
    out += struct.pack("<HHBBB", width, height, flags, bg, 0) + gp
    if loop is not None:
        out += b"\x21\xFF\x0BNETSCAPE2.0\x03\x01" + struct.pack("<H", loop) + b"\x00"
    if comment is not None:
        out += b"\x21\xFE" + sub_blocks(comment)
    for f in frames:
        if f["gce"] and (f["delay"] is not None or f["disposal"] or f["transp"] is not None):
            fl = (f["disposal"] & 7) << 2 | (1 if f["transp"] is not None else 0)
            out += b"\x21\xF9\x04" + struct.pack("<BHB", fl, f["delay"] or 0, f["transp"] or 0) + b"\x00"
        lp = f["palette"]
        fl = 0
        lpb = b""
        pal_for_bits = lp if lp is not None else palette
        if lp is not None:
            padded, bits = pad_palette(lp)
            fl |= 0x80 | (bits - 1)
            lpb = palette_bytes(padded)
        if f["interlaced"]:
            fl |= 0x40
        out += b"\x2C" + struct.pack("<HHHHB", f["x"], f["y"], f["w"], f["h"], fl) + lpb
        px = f["pixels"]
        if f["interlaced"]:
            rows = interlace_order(f["h"])
            px = [p for r in rows for p in f["pixels"][r * f["w"]:(r + 1) * f["w"]]]
        mc = f["min_code"]
        if mc is None:
            n = len(pal_for_bits) if pal_for_bits else 4
            mc = max(2, (max(n, 2) - 1).bit_length())
        if f["raw_data"] is not None:
            out += bytes([mc]) + f["raw_data"]
        else:
            out += bytes([mc]) + sub_blocks(lzw_encode(px, mc))
    if trailer:
        out += b"\x3B"
    return bytes(out)


RGBW = [(255, 0, 0), (0, 255, 0), (0, 0, 255), (255, 255, 255)]


def png(w, h, color_type, depth, rows, plte=None, trns=None, extra_chunks=(), gamma=None):
    """rows: list of raw scanline byte strings (without filter byte). Filter 0 for all rows."""
    def chunk(t, d):
        c = struct.pack(">I", len(d)) + t + d
        return c + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)

    out = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, depth, color_type, 0, 0, 0))
    for t, d in extra_chunks:
        out += chunk(t, d)
    if plte:
        out += chunk(b"PLTE", bytes(c for rgb in plte for c in rgb))
    if trns:
        out += chunk(b"tRNS", trns)
    raw = b"".join(b"\x00" + r for r in rows)
    out += chunk(b"IDAT", zlib.compress(raw))
    out += chunk(b"IEND", b"")
    return out


def bmp24(w, h, rgb_rows_topdown):
    """24-bit uncompressed BMP; rows given top-down as lists of (r,g,b)."""
    rowsize = (w * 3 + 3) // 4 * 4
    data = b""
    for row in reversed(rgb_rows_topdown):
        r = b"".join(bytes((b, g, rr)) for rr, g, b in row)
        data += r + b"\x00" * (rowsize - len(r))
    hdr = struct.pack("<2sIHHI", b"BM", 54 + len(data), 0, 0, 54)
    dib = struct.pack("<IiiHHIIiiII", 40, w, h, 1, 24, 0, len(data), 2835, 2835, 0, 0)
    return hdr + dib + data


def ppm(w, h, rgb_rows):
    return b"P6\n%d %d\n255\n" % (w, h) + b"".join(bytes(c) for row in rgb_rows for c in row)
