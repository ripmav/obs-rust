# S-053 Graphics image decoding (image-file, graphics-ffmpeg, libnsgif) and the built-in effects

| | |
|---|---|
| Status | Draft (vectors complete and produced by the real C; Rust sufficiency check NOT performed, see section 10) |
| Spec version | initial |
| Reference | C implementation, libobs 33.0.1: `libobs/graphics/image-file.c`, `graphics-ffmpeg.c`, `libnsgif/{gif.c,lzw.c,nsgif.h}`, `srgb.h` helpers, `libobs/data/*.effect` |
| Target crate | `crates/graphics-image` (decoding) and `crates/graphics-effects` (embedded effect text + manifest) |
| Depends on | S-020 (effect language and parser: read it, not repeated here), S-008 (sRGB helpers), S-043 (graphics API), S-030 (image source = the main consumer of the animation clock) |
| Conformance | `test/conformance/scripts/gfximages/` (21 scripts), runner `gen/gfximages_runner.c` |

## 1. Scope

Part A: decoding image files into pixel data and the animated-GIF state machine of `gs_image_file_ex_t`.
(a) `gs_image_file_ex_init / init_texture / tick / update_texture / free`; (b) `gs_create_texture_file_data{,2,3}`
(still images through FFmpeg: pixel-format mapping, alpha modes, EXIF orientation); (c) the vendored GIF decoder
**libnsgif** (`nsgif_*`): scan, frame table, loop count, delays, disposal, transparency, composition, LZW, errors.
The GIF decoder may be replaced by a crate in the Rust port; section 6.9 states what must stay observable.

Part B: the 21 effect files shipped in `libobs/data/`: parameters, techniques, passes and the meaning of each
pixel/vertex shader. The effect *language* and parser (S-020) are not respecified; here the files are loaded
through the real parser to pin their structure.

Out of scope: Windows WIC fallback (`wic_image_init`, HDR PQ conversion; Windows only, see 9.5), real GPU shader
compilation, the image *source* plugin (S-030), FFmpeg's own decoders.

## 2. Conventions

MUST/SHOULD as RFC 2119. Rule ids `S-053-Rnn`. Pixel bytes are written as hex in memory order. Time: GIF delays are
centiseconds (cs); the clock of `tick` is nanoseconds; 1 cs = 10,000,000 ns. "Canvas" is the logical screen
bitmap. `RGBA` below is byte order `r,g,b,a` (libnsgif `NSGIF_BITMAP_FMT_R8G8B8A8`, what OBS requests).
Comparison of all vectors is exact text (no floating point is printed). FFmpeg error strings that libobs appends to log
lines after `': ` are replaced by `<ffmpeg-error>` in the vectors and are not part of the contract.

## 3. Interface

### 3.1 Image file object (`struct gs_image_file_ex`)

| Field | Meaning |
|---|---|
| `texture` | texture created by `init_texture` (null before) |
| `format` | `GS_RGBA` (animated), or the format returned by the still decoder (3.2) |
| `cx, cy` | size in pixels |
| `is_animated_gif`, `loaded`, `frame_updated` | flags; `frame_updated` is never touched by this code (stays 0; the caller owns it) |
| `cur_time` (ns), `cur_frame`, `cur_loop` | animation clock state |
| `texture_data` | still image pixels until the texture exists, then null |
| `mem_usage` | bytes accounted (R15) |
| `alpha_mode`, `space` | present in the struct but **never written** by init (always 0) |
| `internal` | private (gif handle, frame cache, ...) |

Functions: `init(image, path, alpha_mode)`, `free(image)`, `init_texture(image)`, `tick(image, elapsed_ns) -> bool`,
`update_texture(image)`. All take a graphics context for texture calls (`gs_texture_create`, `gs_texture_set_image`,
`gs_texture_destroy`); the runner provides a fake device whose textures keep their pixels.

### 3.2 Still-image decoding

`gs_create_texture_file_data(file, &format, &cx, &cy)` = straight alpha. `..._data2(file, mode, ...)` and `..._data3(file,
mode, ..., &space)` take a mode: `STRAIGHT`, `PREMULTIPLY_SRGB`, `PREMULTIPLY`. Returns a `bmalloc` buffer of
`cx*cy*bpp/8` bytes or NULL (null/empty file name -> NULL silently). `space` is always `GS_CS_SRGB` on success.
Result `format`: RGBA (source RGBA, or RGBA64BE after conversion), BGRA (source BGRA, and every format converted by
swscale), BGRX (source BGR0), other source formats never reach the mapping (R33).

### 3.3 libnsgif (public contract used by OBS)

`create(bitmap callbacks, fmt)`, `data_scan(size, data)`, `data_complete`, `get_info` (`width, height, frame_count,
loop_max, background, global_palette`), `get_frame_info(n)` (`display, transparency, local_palette, interlaced,
disposal, delay, rect{x0,y0,x1,y1}`), `frame_decode(n) -> bitmap`, `frame_prepare(area, delay, frame)`, `reset`,
`global_palette / local_palette`, `set_frame_delay_behaviour(min, default)`, `strerror`, `destroy`. Error enum
(positive numbers, strings): OK "Success"; OOM "Out of memory"; DATA "Invalid source data"; BAD_FRAME "Requested
frame does not exist"; DATA_FRAME "Invalid frame data"; END_OF_DATA "Unexpected end of GIF source data";
DATA_COMPLETE "Can't add data to completed GIF"; FRAME_DISPLAY "Frame can't be displayed"; ANIMATION_END "Animation
complete". **All error codes are >= 0** (see D-1).

## 4. Data formats

### 4.1 GIF worked example (`two.gif`, 93 bytes, the fixture of `10-gif-basic`)

```
47 49 46 38 39 61                         "GIF89a"
02 00 02 00 91 00 00                      screen 2x2, flags 91 = global table, 2 bits (4 entries); bg index 0; aspect 0
ff 00 00  00 ff 00  00 00 ff  ff ff ff    palette: red green blue white
21 ff 0b "NETSCAPE2.0" 03 01 00 00 00     application extension, loop count 0 (forever)
21 f9 04 00 0a 00 00 00                   graphic control: flags 00, delay 10 cs, transparent index 0 (unused), end
2c 00 00 00 00 02 00 02 00 00             image descriptor x=0 y=0 w=2 h=2 flags 00
02  03 44 34 05 00                        LZW min code size 2; one sub-block of 3 bytes; terminator
   (codes, 3 bits each, LSB first: 4(clear) 0 1 2 3 5(end))
21 f9 04 00 14 00 00 00 2c 00 00 00 00 02 00 02 00 00 02 03 9c 02 05 00      frame 1: delay 20 cs, pixels 3 2 1 0
3b                                        trailer
```
Decoded (RGBA): frame 0 = `ff0000ff 00ff00ff 0000ffff ffffffff`; frame 1 = `ffffffff 0000ffff 00ff00ff ff0000ff`.
Info: `width=2 height=2 frames=2 loop_max=0 background=0xff0000ff global_palette=1`; frame delays 10 and 20.
(`background` is the u32 whose memory bytes are `r,g,b,a`: red = `0xff0000ff` on little endian.)

### 4.2 Fixtures for still images

PNG files (8-bit RGBA/RGB/gray/gray+alpha/palette, tRNS, 16-bit RGBA/RGB/gray), BMP (24 bit) and PPM are generated by
`gen/gfximages_fixtures.py` (zlib + hand-built chunks), embedded as hex in the scripts.

## 5. Behaviour (numbered rules)

### 5.1 libnsgif

- **S-053-R01 Signature.** The first three bytes MUST be `GIF`, else `scan` returns DATA (and the info stays all zero).
  Bytes 3..5 are not checked (`GIF87a`, `GIF89a`, `GIF99a` are all accepted).
- **S-053-R02 Incremental scan.** `scan(n, buf)` receives the *cumulative* buffer (always from byte 0, `n` = bytes
  available so far). It returns OK when every block up to a block boundary at/before `n` was understood and the trailer
  or the end of data was reached cleanly at a boundary; END_OF_DATA when `n` ends inside a block. Frames whose image
  data lies completely inside `n` are kept (frame count after each of `n = 5, 13, 20, 60` of the 93-byte example: 0; after
  70: 1; after 93: 2). Header information (size, global palette, loop count) becomes visible as soon as it is complete
  (`n >= 13` gives size 2x2, `n >= 30` the background colour). A missing trailer byte is not an error (92 of 93 bytes: OK,
  2 frames; a file without `3b`: OK).
- **S-053-R03 Complete.** `complete()` ends input; further `scan` returns DATA_COMPLETE; calling `complete` twice is harmless.
  `info` and frames stay unchanged by it.
- **S-053-R04 Canvas size.** `info.width/height` start as the logical screen size and are enlarged to cover every frame
  rectangle (`width = max(width, x+w)`, same for height): a 2x2 screen with a 4x4 frame gives 4x4; a 1,1 offset 2x2 frame gives 3x3; a
  frame at 5,5 gives 7x7; a 0x0 screen with a 1x1 frame gives 1x1. A 0x0 *frame* is accepted (rect `0,0,0,0`; decodes to a
  canvas of transparent black).
- **S-053-R05 Frame table.** Every image descriptor yields a frame with `rect = (x, y, x+w, y+h)`, `interlaced`, `local_palette`,
  `transparency` (a graphic control extension with the transparent-colour flag), `disposal` (3 bits of the GCE, stored raw 0..7),
  `delay` (u16 centiseconds exactly as stored, **not adjusted**) and `display=1`. A frame without a preceding GCE has delay 10 (the library's default delay, not 0), disposal 0,
  no transparency; a GCE with delay 0 stores 0. `get_frame_info(frame_count)` returns NULL.
- **S-053-R06 Loop count.** `info.loop_max` = number of plays: 1 when the file has no loop extension; a NETSCAPE2.0 or ANIMEXTS1.0
  application extension (sub-block id 1, length 3) with count `c` gives `0` if `c == 0` (forever) else `c + 1` (c=1 -> 2, c=65535 -> 65536).
  A sub-block id other than 1 makes the scan fail with DATA_FRAME ("Invalid frame data", 0 frames); a sub-block length other than 3 is ignored (loop_max stays 1).
- **S-053-R07 Frame preparation (not used by OBS, pinned for crate replacement).** `frame_prepare` returns the next frame, its
  `area` (dirty rectangle) and `delay` = stored delay, but replaced by `default` when `delay < min` (library defaults min 2, default 10; changeable
  with `set_frame_delay_behaviour`, also after scanning). The last frame of a finite animation reports delay `0xFFFFFFFF`; the following call wraps to frame 0
  once more and the call after that returns ANIMATION_END ("Animation complete"); `reset` restarts. Infinite animations never end.
- **S-053-R08 Decoding.** `frame_decode(n)` returns the composed canvas after frame `n` (`width*height*4` bytes, format per R14), independent of the order
  in which frames are requested (random access gives the same bytes as sequential access). `n >= frame_count` -> BAD_FRAME.
- **S-053-R09 Pixel and transparency model.** An opaque pixel is the palette colour with alpha 255. A pixel with the frame's transparent index leaves the canvas pixel
  unchanged (shows the previous content). Canvas areas never painted are `00000000` (transparent black). A transparent index outside the palette is accepted
  (flag set, no pixel affected). A frame without any colour table (no global, no local) uses a default two-entry table: index 0 black, 1 white (`global_palette` stays 0 in `info`,
  `global_palette()` reports 2 entries `0xff000000, 0xffffffff`). `local_palette(n)` is false for a frame without local table and when `n` is out of range.
- **S-053-R10 Disposal** (applies after a frame, before the next one is composed). 0/unspecified and 1/none: keep. 2/restore background: the frame's rectangle
  is reset to the background colour taken from the global table (opaque) -- unless the *frame has a transparent index*, in which case it is reset to `00000000`.
  3/restore previous: the rectangle is restored from the canvas as it was before the frame was drawn (for the first frame: `00000000`). 4 is an alias of 3. 5, 6, 7 are
  stored as such and treated as "keep". The last frame's disposal is irrelevant.
- **S-053-R11 Interlace.** Interlaced rows are stored in the order 0 mod 8, 4 mod 8, 2 mod 4, 1 mod 2 and decoded to their final row positions (the interlaced and sequential
  encodings of the same picture decode identically).
- **S-053-R12 LZW.** Legal minimum code sizes 2..8 decode normally, including code width growth to 12 bits and a clear code when the table is full (80x80 random, 4,000 bytes).
  A pixel stream shorter than the frame (missing end code or truncated data) leaves the remaining pixels `00000000` and still succeeds. Garbage codes make
  `frame_decode` fail with DATA_FRAME *after* a successful scan. Declared sizes 12 and 13 fail the scan (DATA_FRAME, frame not recorded). Declared sizes 0, 1, 9
  decode implementation-specifically (10 and 11 are untested) (vectors pin the reference output; a replacement decoder MAY differ, QUIRK Q-5).
- **S-053-R13 Palette API.** `global_palette()` returns up to 256 entries as u32 (memory bytes `r,g,b,a`, alpha 255). 
- **S-053-R14 Output byte order.** For the four requested formats the bytes of one pixel are `r,g,b,a` (R8G8B8A8), `b,g,r,a` (B8G8R8A8), `a,r,g,b`, `a,b,g,r`.
- **S-053-R15 Truncated input.** Cut at a block boundary: OK with the frames read so far; cut inside a block: END_OF_DATA with frames before it kept.
  A zero-byte input yields END_OF_DATA and zero everything.

### 5.2 `gs_image_file_ex` (image-file.c)

- **S-053-R20 Classification.** A path is tried as animated GIF iff `len(path) > 4` and its last four characters equal `.gif` ignoring ASCII case.
  Everything else (and a GIF with a single frame, R25) goes through the still decoder. `init(NULL image)` is outside the contract; `init(image, NULL)` leaves a
  zeroed, unloaded image.
- **S-053-R21 GIF load sequence.** open (failure: warning `Failed to open file '<path>'`), read the whole file, loop `scan(size, data)` until it returns OK
  (see D-1/D-2 for the inputs where this does not terminate), `complete`. Dimensions `width > 4096 || height > 4096` -> warning `Bad texture dimensions (WxH) in '<path>'`
  (4096 itself is allowed). The multiplication overflow check (`w*h*4*frames` in `int` equals the 64-bit product) is a no-op on realistic input.
- **S-053-R22 No fallback.** Once a path ends in `.gif`, the GIF path alone decides: every failure after classification (missing file, scan error, dimensions) leaves
  `loaded = 0` and the still decoder is NOT tried (a PNG named `x.gif` does not load; see D-1). The only exception is R25.
- **S-053-R23 Animated init.** If `frame_count > 1`: decode frames 0..n-1 in order (failure: warning `Couldn't decode frame %u of '<path>'`, loading continues), then decode
  frame 0 again; `cx, cy` = info size, `format = GS_RGBA`; a frame cache of `frame_count` empty slots and a buffer of `frame_count*w*h*4` bytes are allocated.
  The shown frame is the library canvas after frame 0, premultiplied in place by the alpha mode (`PREMULTIPLY_SRGB` / `PREMULTIPLY`: R31 formulas). For GIF this is observable
  only through alpha-0 pixels, which are already `00000000`, so **all three alpha modes produce identical GIF output**. `loaded = 1`, `cur_*` = 0.
- **S-053-R24 Memory accounting.** Animated: `mem_usage = 8*frames + w*h*4*frames + w*h*4 + file_size` (2x2, 2 frames, 93 bytes: 157). Still image: `cx*cy*bpp/8`
  (bpp from the format: RGBA/BGRA/BGRX 32, RGBA16 64). Failed image: 0.
- **S-053-R25 Single frame.** `frame_count <= 1` (also a GIF cut after its first frame): the GIF handle is destroyed and the file is loaded by the still decoder (R30); the result
  is a still image (BGRA from FFmpeg, `is_animated_gif = 0`, `texture_data` set). But if the dimension check (R21) fails, the image is NOT loaded even with one frame (4097x1 single-frame GIF).
- **S-053-R26 `init_texture`.** Not loaded: nothing. Animated: `gs_texture_create(cx, cy, RGBA, 1 level, frame0 canvas, GS_DYNAMIC)`. Still: `gs_texture_create(cx, cy, format, 1, texture_data, 0)`, then
  `texture_data` is freed and set to NULL.
- **S-053-R27 Tick clock.** Specified in S-030 section 6.4 and confirmed by the real-C vectors: gate `loops = loop_max (>= 65535 -> 0)`; if `loops == 0 || cur_loop < loops` then
  `cur_time += elapsed` and the frame search runs: `t(f) = delay(f) cs in ns, 0 -> 100,000,000`; while `cur_time > t(nf)`: `cur_time -= t(nf)`, `nf += 1`; when `nf == frame_count`: if
  `loops == 0 || ++cur_loop < loops` then `nf = 0` else (the final increment `cur_loop == loops`) `nf -= 1` and stop. Comparison is `<=` (exactly `t` stays on the frame). Notes pinned by vectors:
  the raw delay is used (a 1 cs frame lasts 10 ms; there is NO minimum of 2 cs and the nsgif default delay rule is not applied); once finished (`cur_loop == loops`) later ticks do nothing and return false and `cur_time` keeps its leftover; a huge elapsed
  value consumes whole loops in one call (3 plays of 250 ms = 100 + 50 + 100 ms; leftover 9,250,000,000 ns for a 10 s tick, `cur_loop = 3`); `loop_max = 2` (NETSCAPE count 1) yields two plays and ends on the last frame with `cur_loop = 2`.
  Not animated or not loaded: false, no state change. Return value is `nf != cur_frame` (after the decode step of R28).
- **S-053-R28 Frame switch (`decode_new_frame`).** If the target frame is not cached: first decode the frames between `last_decoded_frame + 1` (or 0 when the target is below `last_decoded_frame`, i.e. after looping)
  and `target - 1` in order; any failure there aborts silently; then decode the target; on success cache a copy in the frame buffer (slot `target`), set `last_decoded_frame = target` (initial value 0). `cur_frame` is
  set to the target in every case, **even if decoding failed** (QUIRK Q-3). Tick passes `STRAIGHT`, so frames after the first are never premultiplied (QUIRK Q-2).
- **S-053-R29 `update_texture`.** Not animated / not loaded: nothing. Otherwise if `cache[cur_frame]` is empty run R28 for `cur_frame` (so right after init the cache of frame 0 is empty and frame 0 is decoded again, `last_decoded_frame` stays 0),
  then `gs_texture_set_image(texture, cache[cur_frame], width*4, flip=false)`. A decode that fails leaves the slot empty and the texture call then receives a NULL pointer, which the graphics layer ignores; Rust MUST NOT upload from an empty slot.
  Observed (vector `24`): a frame whose decode failed during init decodes without error when requested again later and then shows the unchanged previous canvas (frame 0's pixels), so in practice the slot is filled.
- **S-053-R30 Still path.** `data3(path, alpha_mode, &format, &cx, &cy, &space)`; failure -> warning `Failed to load file '<path>'`, image freed (all zero). `mem_usage` by R24.
- **S-053-R31 Free.** Zeroes the struct. Loaded animated: destroys the gif, frees the cache and buffer, destroys the texture; loaded still: destroys the texture if any and frees `texture_data`. A not loaded image frees its leftovers
  (`gif_data`, internal). Idempotent on a zeroed struct.

### 5.3 Still decoding (graphics-ffmpeg.c)

- **S-053-R32 Open/decode.** FFmpeg opens the file by name (content sniffing: a PNG without extension loads; BMP, PPM, TIFF ... as FFmpeg supports), takes the best video stream, decodes the first packet. Failures log
  warnings and return NULL: `Failed to open file '<f>': <err>` (missing or empty file), `Could not find stream info ...`, `Couldn't find video stream ...`, `Failed to find decoder ...`, `Failed to open video codec ...`, `Failed to read image frame ...`,
  `Failed to decode frame for '<f>': <err>` (garbage or truncated PNG). Exactly one frame is read; animated formats other than GIF are not specially handled.
- **S-053-R33 Pixel formats** (of the decoded frame): `BGR0` -> copy rows, format BGRX (alpha mode ignored, 4th byte copied untouched). `RGBA` / `BGRA` -> format RGBA / BGRA; STRAIGHT copies, the premultiply modes apply R35 per pixel
  (alpha byte unchanged). `RGBA64BE` -> R34, format RGBA. Anything else (gray, RGB24, palette, YUV, gray+alpha ...) -> swscale (POINT) to BGRA, then the premultiply of the mode, format BGRA.
  The returned format is derived from the *post-conversion* pixel format; `GS_RGBA16` is never returned.
- **S-053-R34 16-bit RGBA.** Per channel `f = bswap16(v) / 65535.0f` (float32). STRAIGHT: `u8 = (uint8)(f*255 + 0.5)`. PREMULTIPLY: `rgb *= a` then convert. PREMULTIPLY_SRGB: sRGB->linear, `rgb *= a`, linear->sRGB, convert.
  The alpha byte is `(uint8)(a*255+0.5)` in all modes.
- **S-053-R35 Premultiply formulas** (float32): `f = u8/255.0f`; PREMULTIPLY: `rgb = f_rgb * f_a`; PREMULTIPLY_SRGB: `rgb = to_srgb(from_srgb(f_rgb) * f_a)` with `from_srgb(u) = u <= 0.04045 ? u/12.92 : powf((u+0.055)/1.055, 2.4)`,
  `to_srgb(u) = u <= 0.0031308 ? 12.92u : 1.055*powf(u, 1/2.4) - 0.055`; back to `(uint8)(f*255 + 0.5)`. Example `(200,100,50,200)`: PREMULTIPLY `(157,78,39,200)`; SRGB `(179,89,44,200)`.
- **S-053-R36 Row padding.** Source line padding is skipped (`linesize` may exceed `w*4`); a source line narrower than `w*4` copies only the available bytes.
- **S-053-R37 Orientation.** If the frame metadata has `Orientation = n` (decimal string; EXIF 1..8) the result is re-arranged after conversion: n = 0/1 none; 2 flip X; 3 rotate 180; 4 flip Y; 5 transpose; 6 rotate 90 clockwise; 7 anti-transpose;
  8 rotate 270 clockwise (exact destination mapping in the vectors, `32-reformat-orientation`: the first pixel of a 3x2 image goes to index 2 for n=2, 5 for n=3, 3 for n=4, 0 for n=5, 1 for n=6, 5 for n=7, 4 for n=8, in the output's own numbering). n = 5..8 swap `cx`/`cy`.
  Other values (>= 9) are outside the contract (the reference leaves most of the buffer uninitialised, D-3). With FFmpeg >= 7.1/8 decoders report orientation as display-matrix side data instead of the metadata entry, so decoding real files
  never rotates there; a Rust port that reads EXIF itself SHOULD apply the table above (open question OQ-1).

### 5.4 Built-in effects (`libobs/data`)

Common conventions of all files: the vertex input is `float4 pos : POSITION; float2 uv : TEXCOORD0` unless stated, `ViewProj` transforms `float4(pos.xyz, 1)`; the pass has no name; each technique has exactly one pass; `image` is the
source texture; `multiplier` (float) scales rgb to convert SDR-white-relative linear to the compositing range; "Tonemap" = `rec709_to_rec2020`, `reinhard`, `rec2020_to_rec709`; the effect's `view_proj` pointer is the parameter named `ViewProj`
when present (all files except `format_conversion`, which is `-`); `world` is never present. The parser results below are exactly the vectors `40..43`.

- **S-053-R40 `color.effect`**: an include-only library: 0 parameters, 0 techniques, 21 functions (sRGB transfer both ways incl. `_channel`, `rec709_to_rec2020`, `d65p3_to_rec709`, `rec2020_to_rec709` as 3x3 dot products, `reinhard`, ST2084 PQ both ways,
  the max-RGB EETF helpers, `linear_to_hlg(rgb, Lw)`, `hlg_to_linear(rgb, exponent)`). Included (`#include "color.effect"`, resolved relative to the including file, S-020) by `bilinear_lowres_scale`, `bicubic_scale`, `area`, `opaque`, `default_rect`, `default`, `deinterlace_base`, `lanczos_scale` and `format_conversion`; not by `solid`, `premultiplied_alpha`, `repeat`.
- **S-053-R41 `default.effect`**: params `ViewProj` mat4, `image` texture, `multiplier` float (no defaults); sampler `def_sampler` Linear/Clamp/Clamp; vertex shader `VSDefault` (pos transform, uv copy). 14 techniques, all `VSDefault`:
  `Draw` (PSDrawBare: sample), `DrawAlphaDivide` (rgb *= 1/a if a>0 else 0), `DrawAlphaDivideTonemap`, `DrawAlphaDivideR10L` (alpha divide times `multiplier`, rec709->rec2020, PQ encode, pack 10-bit RGB into BGRA8 "R10L" little-endian
  words: `b = (B10 & 0x3F)<<2`, `g = (B10>>6)|((G10&0xF)<<4)`, `r = (G10>>4)|((R10&3)<<6)`, `a = R10>>2`, 10-bit values `uint(rgb*876 + 64.5)`), `DrawNonlinearAlpha` (sRGB-encode, rgb*=a, decode), `DrawNonlinearAlphaMultiply`, `DrawSrgbDecompress`,
  `DrawSrgbDecompressMultiply`, `DrawMultiply`, `DrawTonemap`, `DrawMultiplyTonemap`, `DrawPQ` (st2084->linear * multiplier, rec2020->rec709), `DrawTonemapPQ`, `DrawD65P3` (sRGB decode then `d65p3_to_rec709`). Pixel parameters bound per technique: `image`, plus `multiplier` for the Multiply/R10L/PQ variants.
- **S-053-R42 `default_rect.effect`**: like `default` for `texture_rect` (parameter type still TEXTURE): params `ViewProj`, `image`; techniques `Draw`, `DrawD65P3`, `DrawOpaque` (alpha forced to 1), `DrawSrgbDecompress`.
- **S-053-R43 `opaque.effect`**: params `ViewProj`, `image`, `multiplier`; techniques (alpha output always 1.0): `Draw`, `DrawSrgbDecompress`, `DrawSrgbDecompressMultiply`, `DrawMultiply`, `DrawTonemap`, `DrawMultiplyTonemap`, `DrawPQ`, `DrawTonemapPQ`.
- **S-053-R44 `premultiplied_alpha.effect`**: params `ViewProj`, `image`; technique `Draw`: sample, `rgb /= a` when a>0, `saturate` the result (un-premultiply).
- **S-053-R45 `repeat.effect`**: params `ViewProj`, `image`, `scale` (vec2); sampler Linear/Repeat/Repeat; technique `Draw`: vertex shader multiplies uv by `scale` (VS reads `ViewProj`, `scale`), pixel shader samples.
- **S-053-R46 `solid.effect`**: params `ViewProj`, `color` (vec4, default 1,1,1,1), `randomvals1..3` (vec4). Techniques: `Solid` (output `color`), `SolidColored` (vertex colour * `color`; the vertex struct has `float4 color : COLOR`),
  `Random` (`0.5 + 0.5*frac(sin(dot(pos.xy, rv.xy)) * rv.z)` per channel with `randomvals1/2/3`, alpha 1; uses pixel position `pos`). Quirk Q-6: for `SolidColored` the vertex stage lists `color` among its parameters
  (a struct member named like a uniform is matched by name), vertex params `[ViewProj, color]`.
- **S-053-R47 `area.effect`** (box/area downscale, exact coverage weights): params `ViewProj`, `base_dimension` (vec2, source size), `base_dimension_i` (its reciprocal), `image`, `multiplier`; no sampler use (point `Load`).
  `DrawArea`: from `uv` and its screen-space derivatives (`ddx(uv.x)`, `ddy(uv.y)`, y made `abs` when `obs_glsl_compile`) it computes the covered source texels and accumulates `area * Load(texel)` over a variable-size loop.
  `DrawAreaUpscale` snaps the sample position for magnification. Techniques: `Draw`, `DrawMultiply`, `DrawTonemap`, `DrawMultiplyTonemap` and the same four with the `Upscale` infix.
- **S-053-R48 `bicubic_scale.effect`**: params `ViewProj`, `image`, `base_dimension`, `base_dimension_i`, `undistort_factor` (float, default 1), `multiplier`. Vertex shader scales uv by `base_dimension` (pixel units). 4x4 taps with
  Catmull-Rom-family weights `B=0, C=0.75` (`weight4`); `undistort_factor` blends the horizontal coordinate through `(1-a)x^5 + a x` (aspect-ratio "undistort"). Pixel entry has a `bool undistort` argument supplied in the technique call.
  Techniques: `Draw`, `DrawMultiply`, `DrawTonemap`, `DrawMultiplyTonemap`, `DrawUndistort`, `DrawUndistortMultiply`, `DrawUndistortTonemap`, `DrawUndistortMultiplyTonemap`. VS params `[base_dimension, ViewProj]`.
- **S-053-R49 `lanczos_scale.effect`**: same parameters and technique names as bicubic; 6x6 taps with weights `3*sin(pi x)*sin(pi x/3)/(pi x)^2` (Lanczos a=3; the centre tap `min(1, w)` replaces NaN), normalised to sum 1.
- **S-053-R50 `bilinear_lowres_scale.effect`**: params `ViewProj`, `image`, `multiplier`; 8 bilinear taps at fixed fractions (1/16, 3/16, 5/16, 7/16) of the per-pixel uv step (`ddx`, `ddy`), averaged `* 0.125`. Techniques `Draw`, `DrawMultiply`, `DrawTonemap`, `DrawMultiplyTonemap`.
- **S-053-R51 `deinterlace_base.effect`** (include only: 0 techniques, 49 functions, macro `TECHNIQUE(ps, ps_multiply, ps_tonemap, ps_multiply_tonemap)` that defines the three derived pixel shaders and the four techniques `Draw`, `DrawMultiply`,
  `DrawTonemap`, `DrawMultiplyTonemap`, all with `VSDefault`). Params: `ViewProj`, `image`, `multiplier`, `previous_image`, `dimensions` (vec2 pixels), `field_order` (int), `frame2` (bool). Texel = `int2(uv * dimensions)`; reads use `Load` with integer offsets.
  Algorithms per texel and `field`: **discard** `Load(image, (x, y/2*2 + field))`; **blend** `(Load(y) + Load(y+1)) / 2` (2x: first output field uses `previous_image` row y+1 with image row y); **linear** keep the row if `y%2 == field` else average rows y-1 and y+1;
  **yadif** (mode 0 = with spatial check, uses `previous_image` and image rows -3..+3 with the YADIF_CHECK macros), "yadif_discard" averages yadif and discard. `_2x` variants (double frame rate) flip the field with `frame2`. File -> shaders: `deinterlace_blend` PSBlendRGBA,
  `_blend_2x` PSBlendRGBA_2x, `_discard` PSDiscardRGBA, `_discard_2x`, `_linear` PSLinearRGBA, `_linear_2x`, `_yadif` PSYadifMode0RGBA, `_yadif_2x` PSYadifMode0RGBA_2x. Entry points `PSYadifMode2RGBA(_2x)` and `PSYadifDiscardRGBA(_2x)` exist but no file instantiates them.
  Each of the eight files: 7 params (same as base) and 4 techniques; bound pixel params per technique: single-rate: `[field_order, image, dimensions]` (+ `multiplier` first for Multiply variants); 2x blend: also `frame2`, `previous_image` (`[field_order, frame2, image, previous_image, dimensions]`).
- **S-053-R52 `format_conversion.effect`** (78 techniques, 22 params, `view_proj` absent): converts between planar/packed YUV, high bit depth, HDR and RGB on the GPU. Params: `width height width_i height_i width_d2 height_d2 width_x2_i height_x2_i` (floats, sizes and
  reciprocals), `maximum_over_sdr_white_nits sdr_white_nits_over_maximum hlg_exponent hdr_lw hdr_lmax` (floats), `color_vec0..2` (vec4 matrix rows: out = `dot(row.xyz, in) + row.w`), `color_range_min` (default 0,0,0) / `color_range_max` (default 1,1,1) (vec3 clamp bounds for YUV input),
  `image image1 image2 image3` textures. Vertex shaders: `VSPos(id)` generates a full-screen triangle from the vertex id (`x = (id>>1)*4-1, y = (id&1)*4-1`, no vertex buffer), `VSTexPos_*`/`VS420Left_Reverse`/`VSPacked422Left_Reverse`... compute per-fragment source texture coordinates (the
  sampling position of the subsampled chroma, flipped in V unless `obs_glsl_compile`). **Forward** (RGB -> planes; techniques named `<fmt>_<plane>`, one plane per pass, output single channel `float`, `float2` for UV): `Planar_Y/U/V/U_Left/V_Left`, `NV12_Y/UV`, `I010_{PQ,HLG,SRGB}_{Y,U,V}`, `P010_{...}_{Y,UV}`, `P216_{...}`,
  `P416_{...}`: Y = `dot(color_vec0.xyz, rgb) + color_vec0.w`, U/V from rows 1/2 after averaging the two or four source samples covering the chroma site; the HDR variants first multiply by `sdr_white_nits_over_maximum`, go rec709->rec2020, apply PQ (`linear_to_st2084`) or HLG (`linear_to_hlg(rgb, hdr_lw)`), the 10/16-bit
  variants quantise (`floor(saturate(y)*1023 + 0.5) * (64/65535)` for P010 so that the value sits in the upper 10 bits of a 16-bit unorm). **Reverse** (planes/packed -> RGBA, techniques `<fmt>_Reverse`): `UYVY, YUY2 (+PQ, HLG), YVYU, I420 (+PQ, HLG), I40A, I422, I210 (+PQ, HLG), I42A, I444, I412 (+PQ, HLG), YUVA, YA2L, AYUV, NV12 (+PQ, HLG), I010_{SRGB,PQ_2020_709,HLG_2020_709}, P010_{...}, V210_{...}`:
  `YUV_to_RGB` = clamp to `[color_range_min, color_range_max]` then the three `dot` rows; PQ -> `st2084_to_linear_eetf(pq, hdr_lw, hdr_lmax) * maximum_over_sdr_white_nits` then `rec2020_to_rec709`; HLG -> `hlg_to_linear(rgb, hlg_exponent) * maximum_over_sdr_white_nits` then `rec2020_to_rec709`; SRGB -> `srgb_nonlinear_to_linear`.
  Alpha formats (YUVA, YA2L, AYUV, I40A, I42A) return `float4` with the alpha plane; others return `float3`/`float4` with alpha 1. **Packed RGB:** `Y800_Limited/Full` (gray to RGB; limited range expands `(255/219)*v - 16/219`), `RGB_Limited`, `BGR3_Limited/Full` (24-bit packed, three bytes per pixel reassembled from a texture
  three times narrower), `R10L_{SRGB,PQ_2020_709,HLG_2020_709}_{Full,Limited}_Reverse` (unpack 10-bit RGB from BGRA8 words, inverse of R41 `DrawAlphaDivideR10L`; limited range `rgb*(1/876) - 16/219`).
  The exact list of the 78 techniques with their vertex/pixel entry functions and bound parameters is the vector `43-effects-format-conversion`.
- **S-053-R53 Technique selection contract** (callers, for the Rust renderer): a technique is selected by name via the effect API (S-020); these files only guarantee the names above. Name suffixes encode: `Multiply` (also sets `multiplier`), `Tonemap` (HDR->SDR display), `Undistort`/`Upscale` (resampler variants),
  `_PQ/_HLG/_SRGB` (transfer function), `_Reverse` (to RGB). A renderer MUST set exactly the parameters the technique's passes bind (the vector lists them) -- setting an unbound parameter is harmless.

## 6. Algorithms

### 6.1 Premultiply and 16-bit conversion
See R34/R35. In Rust use `f32` arithmetic and `powf`; compare exactly (the vectors contain the u8 results).

### 6.2 Tick (pseudocode)
```
tick(img, ns):
  if !img.animated or !img.loaded: return false
  loops = img.gif.loop_max ; if loops >= 65535 { loops = 0 }
  if !(loops == 0 or img.cur_loop < loops): return false
  nf = img.cur_frame ; img.cur_time += ns
  loop:
     t = delay_cs(nf) * 10_000_000 ; if t == 0 { t = 100_000_000 }
     if img.cur_time <= t: break
     img.cur_time -= t ; nf += 1
     if nf == frame_count:
        img.cur_loop += 1 only evaluated when loops != 0
        if loops == 0 or img.cur_loop < loops: nf = 0
        else if img.cur_loop == loops: nf -= 1 ; break
  if nf != img.cur_frame { switch_frame(img, nf) ; return true }   // R28
  return false
```
Precise detail: `++cur_loop` happens only when `loops != 0` (short-circuit); in the finite case `cur_loop` is incremented at every wrap including the final one.

### 6.3 Frame switch (R28)
```
switch_frame(img, target):
  if cache[target] empty:
     start = (target < last_decoded) ? 0 : last_decoded + 1
     for i in start .. target-1: if decode(i) fails: goto done      // silent
     if decode(target) ok: cache[target] = copy(canvas) ; last_decoded = target
  done: img.cur_frame = target
```
### 6.4 Worked example (infinite 3-frame animation, delays 10, 5, 0 cs; vector 20)
`tick(99,999,999)` false (cur_time 99,999,999); `tick(1)` false (cur_time 100,000,000 = t, stays); `tick(1)` true, frame 1, cur_time 1; `tick(50,000,000)` true, frame 2; next `tick(50,000,000)` false (50,000,001 <= 100,000,000, the 0-cs frame lasts 100 ms).

### 6.5 Orientation mapping
The destination index is computed per pixel in tiles of 64x64 (tile size has no observable effect; a 70x3 image rotated is in the vectors). Use the table of R37.

### 6.6 R10L packing (R41)
`r10 = uint(rgb.r*876 + 64.5)` etc.; pack as in R41 into `float4(uint4(r,g,b,a)) / 255`.

### 6.7 Disposal example (vector 13)
3x3 canvas, bg index 4 (black). Frame 0 fills red (disposal 1). Frame 1: 2x2 green at (1,1), disposal 2. Frame 2: blue at (0,0), disposal 3. Frame 3: white 1x2 at (2,0). Canvases: after 1: red everywhere but the 2x2 block green; after 2 (frame 1 disposed to opaque black first): black 2x2 block at (1,1), blue pixel at (0,0), rest red; after 3 (frame 2 restored to the canvas before it, i.e. the black block stays and (0,0) returns to red): white at (2,0),(2,1).

### 6.8 Error mapping for Rust
`load(path) -> Result<Image, LoadError>`; reference conditions that make `loaded = false` map to errors; warnings are log lines (level warning) with the texts in R21..R32.

### 6.9 What a GIF crate replacement must preserve
Observable through `gs_image_file_ex`: frame count (frames >1 => animated), canvas size (union of screen and frame rects, R04), RGBA canvas bytes per frame with R09/R10 semantics (disposal incl. restore-to-background colour/transparent rule, transparent black for unpainted area, previous-frame transparency),
raw delays (cs) and `loop_max` as "number of plays, 0 = forever" (R06), a hard failure for corrupt files (never hang, D-1). Typical crates (`gif`) decode frames independently; the replacement must compose them exactly as in R08..R10. libnsgif's separate `frame_prepare` delay rule (R07) is not used by OBS.

## 7. Concurrency

None in this area: image loading and ticking are called from the video/graphics thread by the caller (S-030). `gs_image_file_ex_init` is blocking file I/O; not thread-safe on one object. The graphics calls need the graphics context entered. The effect files are immutable data.

## 8. Reference defects and quirks

- **D-1 (DEFECT) GIF scan error never detected -> infinite loop.** `init_animated_gif` loops `do { result = scan(...); if (result < 0) fail } while (result != OK)`. All `nsgif_error` values are non-negative, so the failure test never fires and the loop repeats forever for any
  input whose scan returns a non-OK code: a file named `.gif` that is not a GIF (garbage, PNG), a GIF truncated inside a block, a GIF with an illegal LZW minimum code size, a malformed loop extension. Vectors `22-image-gif-limits` record the `hang` (child killed after 2 s). **Required Rust behaviour:** treat any non-OK scan
  result as failure: warning `Failed to initialize gif '<path>', possible file corruption`, `loaded = false`, no fallback to the still decoder (R22). Files cut exactly at a block boundary (scan returns OK) load with the frames present (R15).
- **D-2 (DEFECT) empty `.gif` file.** `bmalloc(0)` invokes the libobs crash handler (the process ends). Required: treat a zero-length file as failed to load.
- **D-3 (DEFECT) orientation outside 1..8** leaves the output buffer uninitialised (only pixel 0 is written, from the last source pixel). Required: treat unknown values as 1.
- **Q-1 (QUIRK) misnamed files.** The GIF decision is by file name only (R20/R22); the converse (a GIF without `.gif` extension) is decoded by FFmpeg's GIF decoder as a single still frame (first frame only).
- **Q-2 (QUIRK) alpha mode applies only to the first GIF frame.** Later frames are straight; invisible for GIF (R23).
- **Q-3 (QUIRK) frame switch after a failed decode.** `cur_frame` is set to the target and tick returns true even if the decode failed (R28); a frame that failed once at init later shows the previous canvas (R29).
- **Q-4 (QUIRK) `alpha_mode`/`space` fields of the object are never filled.** Callers must not read them.
- **Q-5 (QUIRK) illegal LZW minimum code sizes** 0, 1, 9 decode to reference-specific pixels (see `17-gif-lzw`); 12 and 13 fail.
- **Q-6 (QUIRK) uniform matched by struct member name** (R46).
- **Q-7 (QUIRK) loop count 65534/65535 means forever.** `loop_max >= 65535` is mapped to 0 by the tick, so NETSCAPE counts 65534 and 65535 loop forever.

## 9. Implementation notes (non-normative)

9.1 Rust: `image` + `gif` crates can supply the stills and frames, but FFmpeg-exact outputs (swscale BGRA conversion of gray/palette/YUV, sRGB premultiply rounding) must be reproduced; the pure functions of R34/R35/R37 are trivial to port exactly. The PNG gray/palette cases in the vectors pin swscale results
(`gray 0x40 -> BGRA 40 40 40 ff`, palette colours exact, RGB24 exact); for YUV sources swscale's matrix and range handling is not reproduced by a hand-written port (Open question OQ-2).
9.2 Keep the frame cache (R28): decoding frame n needs frame n-1's canvas, so random access is O(n) without a cache.
9.3 Effects: embed the 21 files as `include_str!` constants keyed by the file name; `#include` resolution is by relative file name (`color.effect`, `deinterlace_base.effect`). The renderer back-end (wgpu/WGSL) must supply its own shader translation; the technique names and parameter lists of R41..R52 are the interface.
9.4 `obs_glsl_compile` is a built-in boolean of the effect language (true for GLSL back-ends, false for D3D): it switches V flips in `area.effect` and `format_conversion.effect`.
9.5 Windows: after the FFmpeg path fails on Windows, `wic_image_init` (WIC decoder, HDR PQ -> scRGB conversion) is tried; not reproduced here.

## 10. Conformance coverage

The runner `gen/gfximages_runner.c` links the real `image-file.c`, `graphics-ffmpeg.c` (through `gfximages_ffwrap.c`, which includes it and adds hooks into its static functions), `libnsgif/gif.c`, `libnsgif/lzw.c`, `graphics.c`,
the effect parser and shader parser, over a fake backend (`gfximages_stubs.c`). Every vector is produced by the C code. The `reformat` command feeds fabricated decoded frames (fabricated frame content, real conversion code); `effect` runs the real effect parser. **The Rust sufficiency check was not performed (cost control decision).**

| Rule | Scripts |
|---|---|
| R01 R02 R03 R05 R13 R14 R15 | 10-gif-basic, 16-gif-errors |
| R04 R09 (default table) | 15-gif-palette-geometry |
| R06 | 12-gif-loop |
| R07 | 11-gif-timing |
| R08 R10 | 13-gif-disposal |
| R09 (transparency) | 14-gif-transparency |
| R11 | 15-gif-palette-geometry |
| R12 | 17-gif-lzw |
| R20 R21 R22 R24 R25 D-1 D-2 | 22-image-gif-limits |
| R23 R26 R27 R28 R29 R31 | 20-image-gif-animation, 23-image-gif-sequencing, 24-image-gif-decode-failure |
| R23 (alpha) Q-2 | 21-image-gif-alpha |
| R30 R32 R33 R34 R35 | 30-still-png, 31-still-formats-errors, 33-reformat-pixel-formats |
| R36 R37 D-3 | 32-reformat-orientation, 33-reformat-pixel-formats |
| R40..R44 R45 R46 | 40-effects-default-color |
| R47..R50 | 41-effects-scaling |
| R51 | 42-effects-deinterlace |
| R52 | 43-effects-format-conversion |

Not covered by vectors: R36 for lines narrower than the row (code reading only), R53 (descriptive), WIC fallback, FFmpeg-version-dependent text, `obs_glsl_compile` branches (parser level in S-020). The shader *arithmetic* of the effects is specified from the source text; no GPU executes it in the vectors.

## 11. Open questions

- OQ-1: Should the Rust port apply EXIF orientation by reading the file's EXIF (the reference does so only with older FFmpeg builds; with FFmpeg 8 the orientation is dropped)? Recommended: apply, matching the reference on the FFmpeg versions OBS actually ships.
- OQ-2: YUV/gray sources decoded by swscale: whether the port reproduces swscale's exact BT.601 limited-range matrix or relies on a crate; only gray8 and RGB24 are pinned by vectors.
- OQ-3: The vectors were produced with FFmpeg 8.1 (libavcodec 63); other versions may differ in error log text (masked) and in orientation metadata (see R37).
