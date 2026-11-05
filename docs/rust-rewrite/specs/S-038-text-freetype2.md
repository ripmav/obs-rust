# S-038 FreeType2 text source (text_ft2_source)

| | |
|---|---|
| Status | Draft |
| Spec version | initial |
| Reference | C implementation, libobs 33.0.1, plugin `text-freetype2` |
| Target crate | `crates/text-freetype2` |
| Depends on | S-014 (obs_data), S-015 (obs_properties), S-023 (UTF-8/wide conversion) |
| Conformance | `test/conformance/scripts/textft2/` (22 scripts, vectors in `vectors/textft2/`) |

## 1. Scope
The deprecated-but-shipped "Text (FreeType 2)" source: settings and defaults, text input (manual or file, chat-log tail),
glyph cache and texture atlas packing, vertex layout (line breaks, wrapping, outline offset), and the draw sequence for
outline / drop shadow. Out of scope and NOT verifiable by vectors: real FreeType rasterisation, font discovery
(fontconfig/DirectWrite/CoreText), the GPU. They are replaced by a **fake host** (section 3). Gradient = two vertex colours
(`color1` on the top vertices, `color2` on the bottom ones); there is no separate gradient parameter.

Not linked in the reference harness: FreeType, fontconfig (a minimal header shim and scripted metrics are used), the GPU.
Real code: obs-data, obs-properties, obs-missing-files, platform/UTF-8 helpers.

## 2. Terminology
`wchar` = platform wide char (4 bytes on Linux, vectors are Linux; UTF-16 unit on Windows). All text below is a sequence of
wchar; on Linux one wchar = one code point. Pixel advance = `advance26_6 >> 6` (arithmetic shift, floor). Floats are printed
`%.9g` in vectors; compare with abs 1e-6 + rel 1e-6 (all values are exact binary fractions of 1/2048 in practice).
`tex W,H` = atlas size, default 2048x2048 (A8, one byte per texel).

## 3. Interface
Registered twice with id `text_ft2_source`: version 0 (flags `VIDEO|CAP_OBSOLETE|CUSTOM_DRAW` = 0x409, no `missing_files`) and
version 2 (flags `VIDEO|CUSTOM_DRAW` = 0x9 on non-Windows, + `DEPRECATED` on Windows; has `missing_files`). Type input, icon
TEXT(10), name key `TextFreetype2`. Callbacks: create, destroy, update, get_width, get_height, video_render, video_tick,
get_defaults, get_properties.

Fake host (harness): `get_font_path(face,size,style,flags)` returns a scripted path or NULL; `FT_New_Face` may fail;
glyph tables are scripted: code point -> glyph index (unmapped -> 0, the .notdef glyph) and per index `w,h,left,top,adv26_6`.
Rendering produces `w x h` pixels; AA value `(idx*31 + x*17 + y*29) % 251 + 1`, mono bit `(idx+x+y)&1`, AA pitch = w + pad.
Source updates are applied by the script command `update` (the real host defers a video source's update to the next tick).

Per-source state (observable through `state`): font name/style/size/flags, face, from_file, antialiasing, text_file, text,
m_timestamp, update_file, last_checked, cx, cy, max_h, custom_width, outline_width, atlas cursor (texbuf_x/y), colors[2],
tex, vbuf, effect, outline_text, drop_shadow, log_mode, word_wrap, log_lines, file_load_failed, glyph cache (slot per glyph
index, 65535 slots).

## 4. Data formats
Glyph record: `u = x/W, v = y/H, u2 = (x+w)/W, v2 = (y+h)/H` (float division), `w,h` bitmap size, `xoff = bitmap_left`,
`yoff = bitmap_top`, `xadv = advance26_6 >> 6`. Vertex buffer: 6 vertices per wchar of the text (two triangles per quad:
(x,y),(x+w,y),(x,y+h),(x,y+h),(x+w,y),(x+w,y+h)), uv likewise, colour per quad `[c1,c1,c2,c2,c1,c2]`. Quads beyond the
number of drawn glyphs stay zero (positions, uv and colours 0). Worked example (vector `13-layout-lines`): text "ab",
glyphs 5x8 top 8 adv 6, max_h 12: quad0 pos (0,4)-(5,12), quad1 (6,4)-(11,12).

## 5. Behaviour
### Module / defaults / properties
- S-038-R01 `obs_module_load` registers version 0 then version 2; returns true; creates the module config dir.
- S-038-R02 Version-specific info tables as in section 3.
- S-038-R03 Defaults (both versions): `font` = {face "Sans Serif" (Windows "Arial", macOS "Helvetica"), size 32 (v0) / 256 (v2), flags 0, style ""}, antialiasing true, word_wrap false, outline false, drop_shadow false, log_lines 6, color1 = color2 = 0xFFFFFFFF. No defaults for from_file, log_mode, custom_width, text, text_file (read as false/0/"").
- S-038-R04 Properties in order: font(font), from_file(list radio, bool items: Manual=false, FromFile=true), text_file(path, file, filter key `TextFileFilter`, long description `TextFile.Encoding`), text(multiline), antialiasing(bool), log_mode(bool), log_lines(int 1..1000 step 1), color1(color+alpha), color2(color+alpha), outline(bool), drop_shadow(bool), custom_width(int 0..4096 step 1), word_wrap(bool). Same for both versions. Descriptions are the locale keys (`Font`, `TextInputMode`, `TextFile`, `Text`, `Antialiasing`, `ChatLogMode`, `ChatLogLines`, `Color1`, `Color2`, `Outline`, `DropShadow`, `CustomWidth`, `WordWrap`).
- S-038-R05 Modified callback of `from_file`: `text` visible = !from_file, `text_file` visible = from_file; returns true.
- S-038-R06 Module unload frees the font list and FreeType only if a source ever initialised the plugin.
- S-038-R07 `create` initialises FreeType once (retried on every create while it fails; failure logs warning `FT2-text: Failed to initialize FT2.`), then requests an update.

### Update
- S-038-R10 outline_width = 6 if outline and drop_shadow, 4 if exactly one, else 0.
- S-038-R11 custom_width setting >= 100 is stored; otherwise stored as 0. get_width = cx + outline_width, get_height = cy + outline_width (Q10).
- S-038-R12 Colours are the settings truncated to 32 bit.
- S-038-R13 If FreeType is unavailable, update stops after storing these values (font, text, glyphs untouched; effect not created).
- S-038-R14 The draw effect `text_default.effect` is created on first update (failure leaves it NULL).
- S-038-R15 Antialiasing change: clear atlas, clear glyph cache, cursor to 0,0, re-cache the standard glyph set.
- S-038-R16 Font identity = (face, style, flags, size). If unchanged, no reload. Otherwise: names replaced, `max_h` = 0, then lookup -> open face (old face freed only after a successful lookup) -> pixel size = font size, unicode charmap -> atlas buffer re-allocated zeroed -> standard glyph set cached. Lookup failure or open failure: warning `FT2-text: Failed to load font <face>` and the update ends there (text is NOT refreshed). See Q4.
- S-038-R17 `from_file`: path empty or non-existent -> text = " " (one space) and warning `FT2-text: Failed to open <path> for reading`, `text_file` unchanged. Existing path: if equal to the stored path and no layout-affecting setting changed in this update (custom_width, word_wrap, colours, log_lines, log_mode, font size, from_file toggle, font change) the update ends without re-reading (Q8); otherwise store the path and read it (R30..R40) and record last_checked = now.
- S-038-R18 Manual mode: text = UTF-8 setting converted to wchar.
- S-038-R19 With a face: cache the glyphs of the text, then rebuild the vertex buffer (R50..R60). `file_load_failed` is cleared on every update.

### Glyph cache
- S-038-R20 Standard set (cached on font load/AA change): `a-z A-Z 1234567890 !@#$%^&*()-_=+,<.>/?\|[]{}`~ space ' "` in that order (no `:;`).
- S-038-R21 Cache key = glyph index of the code point; already-cached indices are skipped (shared indices and unmapped chars share).
- S-038-R22 Per new glyph: load (mono target if AA off), render (normal or mono), `max_h = max(max_h, h)`, then if `x + w >= W`: x = 0, y += max_h + 1; if `y + h >= H`: warning `Out of space trying to render glyphs` and STOP (remaining glyphs uncached, cursor saved after the wrap); else record, copy bitmap at (x,y), x += w + 1; if now `x >= W`: x = 0, y += max_h (no +1; Q1). Cursor persists between calls.
- S-038-R23 Bitmap copy: row r of the bitmap is read at byte offset `r*|pitch|`; AA: byte copied; mono: bit `7-(x%8)` of byte `x/8`, set -> 255 else 0.
- S-038-R24 If any glyph was cached the A8 texture is recreated from the whole atlas.
- S-038-R25 Zero-size glyphs (space) are cached with w=h=0 and still advance the cursor by 1.

### Width and layout
- S-038-R30 Text width = widest line, in pixels; '\n' resets the running width; every other char (including '\r') adds the cached advance, or for an uncached glyph the loaded glyph's `advance>>6`; nothing is cached by the query.
- S-038-R31 Vertex build: cx = custom_width if >= 100 else text width; cy initial = max_h. Old buffer destroyed; empty text -> no buffer (cx, cy still set).
- S-038-R32 Word wrap pre-pass runs only if custom_width > 100 (Q2) and word_wrap (algorithm 6.1); it modifies the stored text.
- S-038-R33 Fill pass (algorithm 6.2): origin x = 2 if outline else 0 (drop shadow does not shift); baseline y = max_h; '\n' -> x = origin, y += max_h + 4 (repeated newlines each add a line); '\r' and glyph-less chars produce no quad (no cached glyph found for the index = skip; unmapped chars use glyph 0); with custom_width >= 100, if `x + xadv > custom_width` then x = origin, y += max_h + 4 before placing; quad rect = (x + xoff, y - yoff, w, h); x += xadv; cy = max over glyph bottoms `y - yoff + h` (starting at max_h); trailing newlines do not raise cy.
- S-038-R34 Colour per quad (R4 format); uv from glyph.

### Rendering
- S-038-R40 Render does nothing if texture, buffer missing or text empty. Otherwise: reset blend state; if outline: 8 draws with matrix translations accumulated (-2,0),(0,-2),(2,0),(2,0),(0,2),(0,2),(-2,0),(-2,0) i.e. positions (-2,0),(-2,-2),(0,-2),(2,-2),(2,0),(2,2),(0,2),(-2,2), `use_color` false (black); if drop_shadow: one draw at (4,4), use_color false; finally one draw with vertex colours. Each draw uses 6*wcslen(text) vertices. In linear-sRGB mode the texture is bound sRGB and framebuffer sRGB is toggled for the draw.

### Text file reading
- S-038-R50 Non-log read (`load_text_from_file`): open failure -> warning `Failed to open file <path>` once per `file_load_failed` cycle, text unchanged. File starting with bytes FF FE is UTF-16 (D2). Otherwise bytes up to the first NUL are converted from UTF-8; a UTF-8 BOM becomes U+FEFF (Q6); invalid UTF-8 -> empty text (Q5); all U+000D removed.
- S-038-R51 Log read (`read_from_end`): scan backwards byte by byte from end counting '\n' until count > log_lines or offset 0; if stopped on a newline at offset > 0, start = that offset + 1, else start = 0 (Q3). Remainder converted as R50 (NUL truncation, CR removal). A trailing newline counts as a break; thus a file without trailing newline shows log_lines+1 lines.
- S-038-R52 Text file mode uses the same layout as manual text.

### Tick
- S-038-R60 Only if from_file and text_file set. If >= 1 s elapsed since last_checked: t = mtime of file (-1 on error); last_checked = now; if update_file: re-read (log mode ? tail : full), cache glyphs, rebuild buffer, clear update_file; then if t != m_timestamp: m_timestamp = t, update_file = true. So a change is noticed on one check and applied on the next (>= 1 s later). m_timestamp starts at 0 so the first check always schedules a reload.
- S-038-R61 A failed reload after deletion warns once (flag `file_load_failed`), keeps the old text.

### Missing files (v2)
- S-038-R70 If from_file and text_file non-empty and the path does not exist: one missing-file entry (source kind) whose callback stores the new path into `text_file` and updates the source. Otherwise empty list.

## 6. Algorithms
### 6.1 Word wrap (custom_width > 100, word_wrap)
```
x = 0; word = 0; last_space = 0
for i in 0..=len:
  at_end = (i == len)
  if at_end or text[i] in {' ', '\n'}:
     if x + word > custom_width: if last_space != 0: text[last_space] = '\n'; x = 0
     if at_end: break
     x += word; word = 0
     if text[i]=='\n': x = 0
     if text[i]==' ': last_space = i
  if not at_end: word += cached_advance(text[i])  # the delimiter's own advance joins the NEXT word; uncached glyph adds 0
```
(Even when the space at `last_space` was not the one just before the overflowing word the same index is used: the most recent space.)
### 6.2 Fill: see R33.

## 7. Concurrency
Update, tick and render run on the graphics/video thread; graphics calls are wrapped in enter/leave graphics. No internal locks.

## 8. Quirks and defects
- Q1 QUIRK: post-placement row wrap steps `max_h` (no gap); pre-placement wrap steps `max_h+1`. Script 11.
- Q2 QUIRK: word wrap only when custom_width > 100; width 100 is character-wrapped only. Script 15.
- Q3 QUIRK: log tail keeps a leading newline when the scan reaches offset 0 on one. Script 19.
- Q4 QUIRK: a failed font lookup records the new name (not retried until the settings change), keeps the old face/atlas, zeroes max_h. Scripts 05/06.
- Q5 QUIRK: invalid UTF-8 in a file yields empty text. Script 17.
- Q6 QUIRK: UTF-8 BOM not stripped. Script 17.
- Q7 QUIRK: CR counts in the width query but produces no quad. Script 12/13.
- Q8 QUIRK: same file path is not re-read by update unless a layout-affecting setting changed. Script 18.
- Q9 QUIRK: first tick always schedules a reload (m_timestamp starts 0). Script 20.
- Q10 QUIRK: reported size adds outline_width to both dimensions. Script 04.
- D1 DEFECT: reload on tick with NULL face dereferences NULL (script 22 records the crash). Required: skip reload when no face.
- D2 DEFECT (not vectorised): UTF-16 (FF FE) files are read raw into wchar buffers; correct only for 16-bit wchar (Windows) and the Linux build reads out of bounds; the tail path (R51) allocates exactly the data size without a terminator. Required: decode UTF-16LE code units, skip BOM, strip CR, always terminate.
- D3 DEFECT (not vectorised): glyph indices >= 65535 index outside the cache. Required: treat as uncached/.notdef.
- D4 DEFECT (not vectorised): line counter is 16 bit, log_lines > 65535 wraps. Required: use a wide counter.

## 9. Implementation notes
Model the font through a trait (lookup, load glyph metrics, render bitmap); everything above is deterministic on that trait.
Atlas cursor logic and wrap pre-pass must be reproduced exactly (the stored text is mutated).

## 10. Coverage
| Rules | Scripts |
|---|---|
| R01-R06 | 01-module-info, 02-properties, 03-lifecycle-font |
| R07, R13 | 07-settings-init-fail |
| R10-R12, R14 | 04-settings-effects |
| R16 | 03, 05-settings-font-change, 06-settings-face-fail |
| R15, R20-R25 | 08-glyph-cache-basic, 09-glyph-cache-pack, 10-glyph-render-modes, 11-glyph-cache-wrap-quirk |
| R30 | 12-text-width |
| R31, R33, R34 | 13-layout-lines, 14-layout-outline-width |
| R32 | 15-word-wrap |
| R40 | 16-render-draws |
| R17-R19, R50, R52 | 17-file-read, 18-file-settings-flow |
| R51 | 19-chatlog |
| R60, R61 | 20-tick-watch |
| R70 | 21-missing-files |
| D1 | 22-defect-tick-no-face |

## 11. Open questions
Windows 16-bit wchar behaviour (surrogate pairs, UTF-16 files) and real font rasterisation are unverified. Real FreeType hinting changes advances/bitmaps; the fake uses scripted metrics.

## 12. Round 9: font discovery (contract level) + GDI text source (added)

Section 1 replaced font discovery by the fake host; the find-font files are now pinned at contract
level ([SRC], the implementer reads the C reference for the API calls). The GDI text source
(`plugins/obs-text`, a separate plugin) is a new component for this spec. The freetype2 core files
map to the existing rules.

### Reference files (all cited)

- `plugins/obs-text/gdiplus/obs-text.cpp`, `plugins/text-freetype2/text-freetype2.c`, `plugins/text-freetype2/text-functionality.c`, `plugins/text-freetype2/find-font.c`, `plugins/text-freetype2/find-font-cocoa.m`, `plugins/text-freetype2/find-font-windows.c`, `plugins/text-freetype2/find-font-iconv.c`, `plugins/text-freetype2/obs-convenience.c`, `plugins/text-freetype2/find-font-unix.c`, `plugins/text-freetype2/find-font.h`

### 12.1 File-to-rule map: freetype2 core

- **S-038-R71 [SRC]** `text-freetype2.c` (17 fns): registration/defaults/properties/create/update/
  missing-files/module-unload — R01–R07, R10–R19, R70.
- **S-038-R72 [SRC]** `text-functionality.c` (15 fns): glyph cache R20–R25, text width R30, vertex
  build R31–R34 (`fill_vertex_buffer`), render R40 (`draw_outlines`/`drop_shadow`), file reading
  R50–R52 (`load_text_from_file`, `read_from_end`, `get_ft2_text_width`).
- **S-038-R73 [SRC]** `obs-convenience.c` (2 fns): `create_uv_vbuffer(num_verts, add_color)` (the
  vertex buffer of R31: uv + optional colour attribute) and `draw_uv_vbuffer` (the draw of R40).

### 12.2 Font discovery (contract level, [SRC] throughout)

- **S-038-R74 [SRC]** `find-font.h`: `struct font_path_info { char *face_and_style; uint32_t full_len,
  face_len; bool is_bitmap; uint32_t num_sizes; int *sizes; bool bold, italic; char *path; FT_Long
  index }`; exports `build_font_path_info`, `sfnt_name_to_utf8`, `load_cached_os_font_list`,
  `load_os_font_list`, `free_os_font_list`, `get_font_path`.
- **S-038-R75 [SRC]** `find-font.c` (11 fns; the shared machinery used by the Windows and Cocoa
  builds): a module-global darray `font_list` of `font_path_info`, filled by
  `build_font_path_info(face, idx, path)` (face_and_style from the face's sfnt family + style names
  via `sfnt_name_to_utf8`, `face_len` = family part length, `is_bitmap` = fixed-size face, the
  bitmap strike `sizes`, bold/italic face bits, `index` = face index). Cache file
  `font_data.bin` in the module config dir: read = u32 version (must be 1) + u32 font checksum (must
  equal `get_font_checksum()`) + the list; any mismatch rebuilds. Save writes the same layout via
  the "tmp" suffix atomic rename (skipped when the checksum is 0). `get_font_path(family, size,
  style, flags, idx)`: compare string = family (+ " " + style when style non-empty); per entry:
  rating = case-insensitive (toupper) common-prefix length of (face_and_style, compare string);
  rating < face_len → skip; bitmap entry: rating /= (best_diff + 1), best_diff = min over strikes of
  |strike − size| (starting 1000); rating += 1 per matching bold flag, += 1 per matching italic
  flag; strict `rating > best_rating` (start 0.0) updates the result. Returns the best path (or
  NULL) and sets `*idx` to that entry's face index.
- **S-038-R76 [SRC]** `find-font-unix.c` (fontconfig build): no font list and no cache —
  `load_os_font_list`/`free_os_font_list` are empty, `load_cached_os_font_list` returns true.
  `get_font_path` builds an FcPattern (FAMILY, STYLE, WEIGHT bold?BOLD:REGULAR, SLANT
  italic?ITALIC:ROMAN, SIZE), runs `FcConfigSubstitute(NULL, pattern, FcMatchPattern)` +
  `FcDefaultSubstitute`, then `FcFontMatch`; on match: `FcPatternFormat(match, "%{file}")` copied
  (strncpy, 511) into a `static __thread char result[512]`, `*idx` from FC_INDEX (default 0),
  returns `&result[0]`; on no match: WARNING `no matching font for '%s' found` (family) and NULL.
- **S-038-R77 [SRC]** `find-font-windows.c` (7 fns): `get_font_checksum`: `SHGetFolderPathA(CSIDL_FONTS)`
  (failure: WARNING `Error finding windows font folder`, 0); `FindFirstFileA(dir + "\\*.*")`;
  checksum = CRC32 accumulated over each entry's `ftLastWriteTime` (whole FILETIME) then
  `cFileName` (strlen bytes). `load_os_font_list`: same scan, directories skipped, each file opened
  with `FT_New_Face` per face index (loop while it succeeds, `num_faces` bounds) →
  `build_font_path_info` per face. `wide_to_utf8`: UTF-16 → UTF-8 including surrogate pairs.
- **S-038-R78 [SRC]** `find-font-iconv.c` (3 fns): `sfnt_name_to_utf8(name)` picks an iconv charset
  by `platform_id`: APPLE_UNICODE → "UTF-16BE"; MACINTOSH → the 27-entry table
  (ROMAN: ENGLISH/ANY "macintosh", ICELANDIC "x-mac-icelandic", TURKISH/POLISH/CZECH/SLOVAK
  "x-mac-ce", ROMANIAN "x-mac-romanian"; JAPANESE "Shift_JIS"; KOREAN "EUC-KR"; ARABIC
  ARABIC/ANY "x-mac-arabic", URDU/FARSI "x-mac-farsi"; HEBREW "x-mac-hebrew"; GREEK "x-mac-greek";
  RUSSIAN "x-mac-cyrillic"; DEVANAGARI/GURMUKHI/GUJARATI "x-mac-<script>"; TRADITIONAL_CHINESE
  "Big5"; SIMPLIFIED_CHINESE "GB2312") with exact (encoding,language) then (encoding, ANY)
  fallback; ISO → iso_codes[encoding_id] {0 "us-ascii", 1 none, 2 "iso-8859-1"}; MICROSOFT →
  ms_codes[encoding_id] {0,1 "UTF-16BE", 2 "Shift_JIS", 4 "Big5", 10 "UTF-16BE"}; anything else →
  NULL (DEBUG log with the three ids). Conversion: `iconv_open("UTF-8", charset)` (failure: DEBUG
  log, NULL), iconv into a 256-byte buffer (failure: WARNING `couldn't convert font name text:
  errno = %d`), NUL-terminated, returned as a copy.
- **S-038-R79 [SRC]** `find-font-cocoa.m` (7 fns): `load_os_font_list` walks
  `NSSearchPathForDirectoriesInDomains(NSLibraryDirectory, NSAllDomainsMask)` → `<dir>/Fonts`
  (when a directory) recursively via `NSFileManager contentsOfDirectoryAtPath`; every file gets the
  same `FT_New_Face`-per-face + `build_font_path_info` loop as Windows. `get_font_checksum`: CRC32
  over the full paths of the files in those `Fonts` directories (non-recursive). The rest
  (list, cache, `get_font_path`) is the shared machinery of R75.

### 12.3 GDI text source (`obs-text/gdiplus/obs-text.cpp`, new component)

- **S-038-R80** Registered three times, all id `text_gdiplus`, type INPUT, icon TEXT, name key
  `TextGDIPlus`, flags `VIDEO|CUSTOM_DRAW|CAP_OBSOLETE|SRGB` — version 0 (flags as-is), version 2
  (same flags, version field 2), version 3 (version 3, CAP_OBSOLETE cleared). All three share
  create/update/tick/render/missing-files/get_properties/get_width/get_height; only `get_defaults`
  (R81, the size default) and the create flag differ: versions 0 and 2 construct the source with
  `old_extents = true`, version 3 with `old_extents = false` (it changes the extents/wrap sizing in
  `CalculateTextSizes`/`TransformText` [SRC]). Module load registers the three infos then runs
  `GdiplusStartup` (default-initialized input) — the GDI+ token is process-global.
- **S-038-R81** Defaults (both size classes, `ver == 1` only for version 0): font object
  `face = "Arial"`, `size = ver == 1 ? 36 : 256`, `flags = 0`, `style = ""`; `align = "left"`,
  `valign = "top"`, `color = 0xFFFFFF`, `opacity = 100`, `gradient_color = 0xFFFFFF`,
  `gradient_opacity = 100`, `gradient_dir = 90.0`, `bkcolor = 0x000000`, `bkopacity = 0`,
  `outline_size = 2`, `outline_color = 0xFFFFFF`, `outline_opacity = 100`, `chatlog_lines = 6`,
  `extents_wrap = true`, `extents_cx = 100`, `extents_cy = 100`, `transform = none`,
  `antialiasing = true`.
- **S-038-R82** Tick: no-op unless reading from a file; accumulate `seconds` into
  `update_time_elapsed`; when >= 1.0: query the file's mtime (−1 on error), reset the accumulator;
  if a reload was scheduled: `LoadFileText` + `TransformText` + `RenderText`, clear the flag; then if
  the mtime differs from the stored one: store it and schedule the reload. As S-038-R60 for the
  FreeType source: a change is noticed on one check and applied on the next.
- **S-038-R83** Render: no-op without a texture; otherwise the base `OBS_EFFECT_DEFAULT` effect,
  technique `Draw`, pass 0: framebuffer-sRGB enabled (previous state restored after),
  `set_texture_srgb("image", tex)`, `draw_sprite(tex, 0, cx, cy)`. The texture is built in
  `RenderText` by GDI+ drawing (`StringFormat` generic-typographic, a `LinearGradientBrush` between
  the two opacity-applied colours along `gradient_dir`, background fill of the extents rectangle when
  the text overflows the box) into a 32bpp ARGB bitmap, uploaded as a texture.
- **S-038-R84** missing_files: as S-038-R70 (the `use_file`/`file` keys, source kind, the callback
  stores the new path and updates the source). Update flow: `LoadFileText` (file text, UTF-8 or
  UTF-16 by BOM [SRC]), `TransformText` (layout, extents, wrap, chat-log tail), `RenderText`
  (R83's bitmap) — run together on update and on the tick's scheduled reload.
