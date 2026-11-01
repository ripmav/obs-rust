# S-021 FLV muxer, AMF0 codec, HEVC/AV1 configuration records (Enhanced RTMP)

| | |
|---|---|
| Status | Draft |
| Spec version | base commit `65c320b46` (libobs 33.0.1) + this file |
| Reference | C implementation, libobs 33.0.1, plugin `obs-outputs`: `flv-mux.c/.h`, `rtmp-helpers.h`, `rtmp-hevc.c/.h`, `rtmp-av1.c/.h`, vendored `librtmp/amf.c/.h` (names for the spec author only) |
| Target crate | `crates/obs-flv` (modules `amf0`, `flv`, `hevc_config`, `av1_config`) |
| Depends on | S-010 (array serializer = a growable big-endian byte writer), S-012 (Annex-B start-code finder, `encoder_packet` layout) |
| Conformance | `test/conformance/scripts/flv/` (34 scripts), runner section `flv runner (spec S-021)` of `test/conformance/README.md` |

## 1. Scope

In scope:

1. **AMF0 codec** of the vendored librtmp: every encoder (`AMF_Encode*`, `AMFProp_Encode`,
   `AMF_Encode`/`AMF_EncodeEcmaArray`/`AMF_EncodeArray`), every decoder (`AMF_Decode*`,
   `AMFProp_Decode`, `AMF_Decode`, `AMF_DecodeArray`), the object/property model
   (`AMF_AddProp`, `AMF_GetProp`, `AMF_CountProp`, `AMFProp_Get*`, `AMF_Reset`).
2. **FLV muxing** (`flv-mux.c`): the FLV file header, the `@setDataFrame`/`onMetaData` script tag,
   `write_file_info` (duration/file size patching), legacy video/audio tags (`flv_packet_mux`), the
   Enhanced-RTMP ("Y2023") video tags (`flv_packet_start/_frames/_end/_metadata`), Enhanced-RTMP audio
   tags (`flv_packet_audio_start/_frames`), multitrack variants, timestamp and composition-time
   computation, helper conversions (`to_video_type`, `to_audio_type`, `get_ms_time`).
3. **Codec configuration record builders**: Annex-B HEVC parameter sets to an `hvcC` record
   (`obs_parse_hevc_header`, including its SPS/VPS/PPS bit parser), AV1 temporal unit to an `av1C`
   record (`obs_parse_av1_header`, including its sequence-header parser) and the AV1 packet filter
   (`obs_parse_av1_packet`).

Out of scope: RTMP handshake/chunking/network I/O (`rtmp-stream.c`, `rtmp.c`), AMF3
(see Q-10 and section 11), the MP4 muxer, AMF object *dumping* (`AMF_Dump`, `AMFProp_Dump` only log),
the Windows-only `rtmp-windows.c`.

Callers: `rtmp-stream.c` (RTMP output: sends `flv_meta_data(write_header=false)` as `@setDataFrame`,
the sequence headers, frames), `flv-output.c` (FLV file output: `write_header=true`,
`write_file_info` after closing), `rtmp-stream.c` for Enhanced RTMP multitrack. The `obs_parse_*_header`
functions are called by the encoder-header extraction of the output plugins.

## 2. Terminology and conventions

RFC 2119 words. `W` = 8. All multi-byte integers in FLV and AMF0 are **big endian** unless stated.
`u8/u16/u24/u32` unsigned, `s24` = two's complement 24 bit. `BE(x,n)` = x written in n bytes big endian
after truncation to the low `n*8` bits. A *packet* is an `encoder_packet` (layout in S-012 section 3,
only the fields listed in 3.2 are used here). `ms(p, v)` is defined in rule R27.
Wherever this document says "the reference logs", a log message is produced through the librtmp log callback
(levels CRIT 0, ERROR 1, WARNING 2, INFO 3, DEBUG 4); **only messages of level WARNING or worse are part of the
contract** (the runner prints CRIT/ERROR as `log error ...` and WARNING as `log warning ...`; text is informational; the reference emits ERROR messages only in the paths below), debug/info messages
(e.g. "AMF_DATE", "Empty buffer") are not.

**Comparison tolerance.** There is none: every double is printed as `%.17g` *and* as its 16-hex-digit bit pattern (`bits=`), and normal lines are compared byte for byte, i.e. doubles must be bit-exact (NaN payloads included).

Naming of the failure value: *NULL* means the C function returns a null pointer; an implementation
returns `None`/`Err`.

## 3. Interface

### 3.1 AMF0 data model

```
enum AmfType (u8):  NUMBER=0 BOOLEAN=1 STRING=2 OBJECT=3 MOVIECLIP=4 NULL=5 UNDEFINED=6 REFERENCE=7
                    ECMA_ARRAY=8 OBJECT_END=9 STRICT_ARRAY=10 DATE=11 LONG_STRING=12 UNSUPPORTED=13
                    RECORDSET=14 XML_DOC=15 TYPED_OBJECT=16 AVMPLUS=17   INVALID=0xff
AVal        { av_val: ptr/bytes, av_len: i32 }        // a byte string; av_val may be NULL when av_len <= 0
AMFObject   { o_num: i32, o_props: [AMFObjectProperty; o_num] }   // growable array (capacity grows by 16, unobservable)
AMFObjectProperty { p_name: AVal, p_type: AmfType, p_vu: union { p_number: f64, p_aval: AVal, p_object: AMFObject }, p_UTCoffset: i16 }
```

A property of type NUMBER, BOOLEAN and DATE uses `p_number` (booleans hold 0.0/1.0); STRING and XML_DOC use
`p_aval`; OBJECT, ECMA_ARRAY and STRICT_ARRAY use `p_object`; NULL and INVALID carry nothing. Strings
decoded from a buffer *refer into* that buffer (no copy). The shared constants `AMFProp_Invalid`
(type INVALID, empty name) and the "invalid object" (`o_num = 0`) are returned by lookups.

Functions (all operate on plain memory; no global state except the log callback):

| Function | Signature (C) | Notes |
|---|---|---|
| `AMF_EncodeInt16/24/32` | `char *(char *out, char *outend, short/int v)` | returns new position or NULL (R01) |
| `AMF_EncodeNumber` | `(out, outend, double)` | R02 |
| `AMF_EncodeBoolean` | `(out, outend, int)` | R03 |
| `AMF_EncodeString` | `(out, outend, const AVal *)` | R04 |
| `AMF_EncodeNamedString/Number/Boolean` | `(out, outend, const AVal *name, value)` | R05 |
| `AMFProp_Encode` | `(AMFObjectProperty *, out, outend)` | R06 |
| `AMF_Encode` / `AMF_EncodeEcmaArray` / `AMF_EncodeArray` | `(AMFObject *, out, outend)` | R07-R10 |
| `AMF_DecodeInt16/24/32` | `(const char *) -> u16/u32/u32` | R11 |
| `AMF_DecodeString/LongString` | `(const char *, AVal *)` | R12 |
| `AMF_DecodeBoolean/Number` | `(const char *) -> int/double` | R11 |
| `AMFProp_Decode` | `(prop, buf, size, bDecodeName) -> int` | R13, R14 |
| `AMF_Decode` | `(obj, buf, size, bDecodeName) -> int` | R15, R17 |
| `AMF_DecodeArray` | `(obj, buf, size, count, bDecodeName) -> int` | R16, R18 |
| `AMF_AddProp / AMF_CountProp / AMF_GetProp / AMF_Reset` | | R19 |
| `AMFProp_GetType/GetName/GetNumber/GetBoolean/GetString/GetObject/IsValid/Reset` and setters | | R19 |

Not specified: `AMF3_Decode`, `AMF3Prop_Decode`, `AMF3CD_*`, `AMF_Dump`, `AMFProp_Dump` (Q-10).

Encoders take an *output window* `[out, outend)` of caller memory and return the position after the
last byte written (so `len = ret - out`), or NULL. In Rust: `fn encode_*(buf: &mut [u8]) -> Option<usize>`
(the window is the slice; `outend - out` is its length). **A conforming implementation need not leave any
particular bytes in the window after a NULL return.**

### 3.2 FLV muxer API (`flv-mux.h`)

```c
enum audio_id_t { AUDIO_CODEC_NONE = 0, AUDIO_CODEC_AAC = 1 };
enum video_id_t { CODEC_NONE = 0, CODEC_H264 = 1, CODEC_AV1 = 2, CODEC_HEVC = 3 };
enum audio_id_t to_audio_type(const char *codec);    // "aac" -> 1, anything else 0
enum video_id_t to_video_type(const char *codec);    // "h264" 1, "av1" 2, "hevc" 3 (built with HEVC support), else 0
int32_t get_ms_time(struct encoder_packet *p, int64_t v);   // R27
void write_file_info(FILE *f, int64_t duration_ms, int64_t size);      // R25
void flv_meta_data(obs_output_t *o, uint8_t **out, size_t *size, bool write_header);   // R22-R24
void flv_packet_mux(packet, int32_t dts_offset, uint8_t **out, size_t *size, bool is_header);  // R28-R30
void flv_packet_start (packet, enum video_id_t, uint8_t **out, size_t *size, size_t idx);          // R31
void flv_packet_frames(packet, enum video_id_t, int32_t dts_offset, uint8_t **out, size_t *size, size_t idx);
void flv_packet_end   (packet, enum video_id_t, uint8_t **out, size_t *size, size_t idx);
void flv_packet_metadata(enum video_id_t, uint8_t **out, size_t *size, int bits_per_raw_sample,
                         uint8_t color_primaries, int color_trc, int color_space,
                         int min_luminance, int max_luminance, size_t idx);                       // R34
void flv_packet_audio_start (packet, enum audio_id_t, uint8_t **out, size_t *size, size_t idx);   // R33
void flv_packet_audio_frames(packet, enum audio_id_t, int32_t dts_offset, uint8_t **out, size_t *size, size_t idx);
// exported but not in the header (used by the wrappers above):
void flv_packet_ex(packet, enum video_id_t, int32_t dts_offset, uint8_t **out, size_t *size, int type, size_t idx);
void flv_packet_audio_ex(packet, enum audio_id_t, int32_t dts_offset, uint8_t **out, size_t *size, int type, size_t idx);
```

Output ownership: `*out` is a freshly allocated buffer of `*size` bytes owned by the caller
(`bfree`). Fields of `encoder_packet` that are read: `data`, `size` (payload), `pts`, `dts`, `timebase_den`
(`timebase_num` is never read), `type` (`OBS_ENCODER_VIDEO = 0`, `OBS_ENCODER_AUDIO = 1`), `keyframe`.
Rust: `fn mux(...) -> Result<Vec<u8>, FlvError>`; the "no output" cases of R30 map to `Ok(None)`/empty.

Constants: tag types AUDIO 0x08, VIDEO 0x09, SCRIPT (info) 0x12; `MILLISECOND_DEN = 1000`.

### 3.3 Inputs of `flv_meta_data` (stubbed in the conformance build)

`flv_meta_data` pulls these values from the libobs objects of the output; they are inputs of the muxer.
A Rust port takes them as a plain struct:

| Accessor (stubbed in `gen/flv_stubs.c`) | Meaning | `cfg` key |
|---|---|---|
| `obs_output_get_video_encoder(o)`, `obs_output_get_audio_encoder(o, 0)` | the encoders (audio track 0) | - |
| `obs_encoder_get_codec(venc)` | video codec name string (`"h264"`, `"av1"`, `"hevc"`, other) | `vcodec` |
| `obs_encoder_get_width/height(venc)` | `u32` | `width`, `height` |
| `obs_encoder_get_settings(enc)` + `obs_data_get_double(s, "bitrate")` | video/audio bitrate in kbit/s as double (0.0 if absent) | `vbitrate`, `abitrate` |
| `obs_encoder_video(venc)` + `video_output_get_frame_rate(v)` | frame rate (double) | `fps` |
| `obs_encoder_get_sample_rate(aenc)` | `u32` | `arate` |
| `obs_encoder_audio(aenc)` + `audio_output_get_channels(a)` | channel count (`size_t`) | `channels` |
| `obs_get_version_string()` | version text | `version` |

Defaults used by the scripts: `h264`, 1920, 1080, 2500, 60, 160, 48000, 2, `0.0.0-conformance`. The module
name is the constant `"obs-output module"`.

### 3.4 Codec record builders

```c
size_t obs_parse_hevc_header(uint8_t **header, const uint8_t *data, size_t size);   // R37-R44
size_t obs_parse_av1_header (uint8_t **header, const uint8_t *data, size_t size);   // R45-R49
void   obs_parse_av1_packet (struct encoder_packet *out, const struct encoder_packet *src);  // R50-R51
uint8_t *ff_nal_unit_extract_rbsp(uint8_t *dst, const uint8_t *src, int src_len, uint32_t *dst_len, int header_len); // R44
```

`*_header` return the length of the allocated record (0 = failure) and store the buffer in `*header`
(caller frees). In Rust: `fn hevc_header(&[u8]) -> Option<Vec<u8>>` (empty record never happens).
`obs_parse_av1_packet` copies `*src` field by field (pts, dts, ... unchanged), replaces `data/size` by a new
buffer holding the kept OBUs (the buffer is preceded in memory by a `long` reference count of 1, an FFI detail),
and sets `keyframe`, `priority`, `drop_priority` (R50).

## 4. Data formats

### 4.1 FLV file header and tags

```
FLV header (13 bytes, only flv_meta_data(write_header=true)):
  'F' 'L' 'V'  01  05  00 00 00 09  00 00 00 00
   sig        ver flags(audio|video) data offset=9  PreviousTagSize0 = 0
Tag:
  type u8 | DataSize u24 | Timestamp u24 | TimestampExtended u8 | StreamID u24 (= 0) | body (DataSize bytes)
  | PreviousTagSize u32 = 11 + DataSize
Timestamp (ms, i32 t):  low24 = t & 0xFFFFFF  ;  ext = (t >> 24) & 0x7F     (arithmetic shift; bit 7 never set)
```

Worked example (vector `14-flv-legacy-video`, legacy video, key frame, 5-byte payload `6500888400`,
dts = pts = 0, timebase 1/1000):

```
09 00000a 000000 00 000000 | 17 01 000000 | 65 00 88 84 00 | 00000015
type size=10 ts       ext sid  key|AVC  nalu cts=0          payload        prevsize=21
```

### 4.2 AMF0 wire format

| Type | Wire bytes |
|---|---|
| Number | `00` + IEEE-754 binary64 big endian (9 bytes). Every bit pattern is preserved, including NaN payloads and -0.0 |
| Boolean | `01` + `00`/`01` |
| String (len < 65536) | `02` + u16 length + bytes |
| Long string (len >= 65536) | `0c` + u32 length + bytes |
| Object | `03` + (property)* + `00 00 09` |
| Null | `05` |
| Undefined / Unsupported | `06` / `0d` (decoded as Null) |
| ECMA array | `08` + u32 count + (property)* + `00 00 09` |
| Strict array | `0a` + u32 count + (value)* (no end marker, values nameless) |
| Date | `0b` + double (ms) + s16 UTC offset (decode only) |
| XML document | `0f` + u32 length + bytes (decode only) |
| property (named) | u16 name length + name bytes + value; (nameless: just the value) |

Worked example (vector `04-amf-encode-objects`): an object with `width` = 1920.0, `stereo` = true, `name` = "abc"
encodes as

```
03 | 0005 7769647468 00 409e000000000000 | 0006 737465726571 01 01 | 0004 6e616d65 02 0003 616263 | 000009
```

### 4.3 `onMetaData` script tag body (R22-R24)

`flv_meta_data(write_header=false)` of the default configuration (vector `10-flv-meta`):
tag type `12`, DataSize 414 (`00019e`), timestamp 0, stream id 0, body:

```
02 000d "@setDataFrame"   02 000a "onMetaData"   08 00000014
  0008 duration 00 <0.0> | 0008 fileSize 00 <0.0> | 0005 width 00 <1920.0> | 0006 height 00 <1080.0> |
  000c videocodecid 00 <7.0> | 000d videodatarate 00 <2500.0> | 0009 framerate 00 <60.0> |
  000c audiocodecid 00 <10.0> | 000d audiodatarate 00 <160.0> | 000f audiosamplerate 00 <48000.0> |
  000f audiosamplesize 00 <16.0> | 000d audiochannels 00 <2.0> |
  0006 stereo 01 01 | 0003 2.1 01 00 | 0003 3.1 01 00 | 0003 4.0 01 00 | 0003 4.1 01 00 |
  0003 5.1 01 00 | 0003 7.1 01 00 |
  0007 encoder 02 0034 "obs-output module (libobs version 0.0.0-conformance)"
00 00 09                                  then PreviousTagSize = 00 00 01 a9 (425)
```

### 4.4 `hvcC` and `av1C` records

hvcC (R42): `01 | space<<6 | tier<<5 | profile_idc | compat u32 | constraint (6 bytes) | level_idc |
f0 + min_spatial u12 (u16, 0xF000 | v) | fc | parallelismType | fc | chromaFormat | f8 | bitDepthLumaMinus8 |
f8 | bitDepthChromaMinus8 | avgFrameRate u16 = 0 | constFR(0)<<6 | numTemporalLayers<<3 | nestedFlag<<2 | 3 |
numOfArrays | arrays`. Each array: `completeness<<7 | nal_type`, count u16, then per NAL: length u16, NAL bytes
(with their 2-byte header, emulation prevention bytes kept).
Worked example (vector `30-hevc-header-basic`, 1080p Main, level 4.0): header
`01 01 60000000 900000000000 78 f000 fc fd f8 f8 0000 0f 03` + VPS array `a0 0001 0017 <23 bytes>` +
SPS array `a1 0001 001d <29 bytes>` + PPS array `a2 0001 0006 <6 bytes>` (96 bytes in total).

av1C (R48): `81 | profile<<5 | level | tier<<7|hbd<<6|twelve<<5|mono<<4|subx<<3|suby<<2|pos | 00 |
sequence header OBU | [last metadata OBU]`. Example (vector `40-av1-header-sequence`):
`81 08 0c 00 0a 0b 00000042abbfc37709d001` (profile 0, level 8, 8-bit 4:2:0, OBU of 13 bytes).

## 5. Behaviour (numbered rules)

### 5.1 AMF0 encoding

Let `room = outend - out` (bytes of the window) at the call.

**S-021-R01 (fixed-width integers).** `AMF_EncodeInt16/24/32(out, end, v)` returns NULL when
`room < 2/3/4`; otherwise it writes the low 16/24/32 bits of `v` big endian and returns `out + 2/3/4`.
The argument is a C `short`/`int`: higher bits are discarded (`enc_int16 0x12345` writes `2345`; negative values
wrap, `-1` -> `ffff`). An exact fit succeeds.

**S-021-R02 (number).** `AMF_EncodeNumber` needs `room >= 9`, else NULL. Writes `00` + the 8 bytes of the double,
most significant first. NaN payloads, infinities, -0.0 and subnormals are written bit-exact (`bits:` vectors).

**S-021-R03 (boolean).** Needs `room >= 2`, else NULL. Writes `01`, then `01` if the int is non-zero else `00`.

**S-021-R04 (string).** Let `L = av_len`. The function returns NULL when `(L < 65536 and room < 3 + L)` **or**
`room < 5 + L` (QUIRK Q-1: the second condition applies to short strings as well, so a short string needs
`L + 5` bytes of room although it writes `L + 3`; the `L + 3 .. L + 4` window is rejected). Otherwise:
`L < 65536`: writes `02`, u16 `L`, the bytes; else `0c`, u32 `L`, the bytes. `L = 0` writes `02 00 00`.
Embedded NUL bytes are kept.

**S-021-R05 (named values).** `AMF_EncodeNamedString/Number/Boolean(out, end, name, value)`:
NULL if `room < 2 + name.len`; else writes u16 `name.len` (truncated to 16 bits), the name bytes (no type byte), and then
the value with `AMF_EncodeString/Number/Boolean` **applied to the rest of the window** (so the value's own room
rule R04/R02/R03 is evaluated after the name; e.g. `ab`/`cd` string needs 4 + 7 = 11 bytes).
`flv-mux.c` uses these for every `onMetaData` entry.

**S-021-R06 (property).** `AMFProp_Encode(prop, out, end)`:
1. type INVALID -> NULL (no log).
2. If type != NULL: when `room <= name.len + 3` (i.e. `out + name.len + 2 + 1 >= end`) -> NULL. (Applies to all
   other types, also nameless ones: a nameless property needs `room >= 4` before the value rules are even consulted.)
3. If type != NULL and `name.len > 0`: write u16 `name.len` and the name. **A NULL-typed property never writes its
   name** (Q-2): it is a single `05` byte, and requires `room >= 2` (`out + 1 >= end` -> NULL).
4. By type: NUMBER -> R02 (value `p_number`); BOOLEAN -> R03 (`p_number != 0`); STRING -> R04; OBJECT -> R07;
   ECMA_ARRAY -> R08; STRICT_ARRAY -> R09; every other type (DATE, UNDEFINED, REFERENCE, ...) -> log ERROR
   `"%s, invalid type. %d"` and return NULL.
Names are written for properties inside strict arrays as well when non-empty.

**S-021-R07 (object).** `AMF_Encode(obj, out, end)`: NULL if `room <= 4` (`out + 4 >= end`). Write `03`. For each
property in order call R06 on the remaining window; on NULL **log ERROR** `"AMF_Encode - failed to encode property in
index %d"` and stop iterating (the properties before it stay in the output; later ones are dropped) - the
function does *not* fail (R10). Then, if the remaining room is `<= 3` return NULL, else write `00 00 09`.

**S-021-R08 (ECMA array).** As R07 but the header is `08` + u32 `o_num` (the *declared* count, even if properties were
dropped), with the entry check `room <= 4` -> NULL, and the same end marker rule (`00 00 09`, NULL if remaining
room <= 3).

**S-021-R09 (strict array).** `AMF_EncodeArray`: NULL if `room <= 4`; writes `0a`, u32 `o_num`, then the properties as in R07
(including logging and stopping on a failed one). **No end marker** is written and no room is required for one.

**S-021-R10 (containment).** A property that does not fit never makes the container fail: the container is closed with what was written so far (R07/R08),
provided the end-marker room rule holds (otherwise NULL). The vectors `05-amf-encode-object-limits` and the exhaustive sweeps `06-amf-encode-sweeps` (every capacity from 0
to 47/51 for the object, ECMA array and strict array with three properties; and for single properties) define the exact boundaries; the formulas of R01-R09 reproduce all of them.
Worked example (script `06`): the object `{width: 1920, stereo: true, name: "abc"}` is 42 bytes when complete. By window size: `room >= 43` -> all 42 bytes;
`room = 41, 42` -> NULL (all three properties fit, but the end marker needs `room_after > 3`); `room = 31..40` -> 30 bytes, the string property was dropped (log error, index 2) and the marker fits;
`room = 27..30` -> NULL; `room = 21..26` -> 20 bytes (only `width`); `room = 17..20` -> NULL; `room = 5..16` -> 4 bytes (empty object, log error index 0); `room <= 4` -> NULL.
The ECMA array (46 bytes complete) behaves alike with its 5-byte header (complete at `room >= 47`); the strict array (43 bytes complete, no end marker) is complete at `room >= 45`.

### 5.2 AMF0 decoding

**S-021-R11 (fixed-width decoders).** `AMF_DecodeInt16/24/32` read 2/3/4 bytes big endian into unsigned values (no
bounds check; the caller guarantees the bytes). `AMF_DecodeBoolean` = `byte != 0`. `AMF_DecodeNumber` = 8 bytes
big endian reinterpreted as binary64 (bit-exact, NaN payloads kept).

**S-021-R12 (string decoders).** `AMF_DecodeString(data)`: `av_len = u16 at data`; `av_val = av_len > 0 ? data + 2 : NULL`.
`AMF_DecodeLongString`: `av_len = (i32) u32 at data` (values >= 2^31 are **negative**, Q-19), `av_val = av_len > 0 ? data + 4 : NULL`.

**S-021-R13 (property decode, framing).** `AMFProp_Decode(prop, buf, size, bDecodeName) -> int` (bytes consumed, or -1):
1. `prop.p_name` is cleared (length 0, NULL).
2. `size == 0` (or NULL buffer) -> -1.
3. If `bDecodeName`: `size < 4` -> -1. `nameLen = u16(buf)`; `nameLen > size - 2` -> -1. Name = the `nameLen` bytes
   (NULL value when `nameLen == 0`); `size -= 2 + nameLen`, advance.
4. `size == 0` -> -1. Read the type byte (`size -= 1`) and decode the value as in R14.
5. Return `original_size - remaining_size`.
No other validation happens. In particular the container cases may return a value **larger than the size supplied**
(Q-4) and NULL/Undefined consume only the type byte.

**S-021-R14 (value decoding; `size` = bytes left after the type byte).**

| Type byte | Rule |
|---|---|
| 0 NUMBER | `size < 8` -> -1; `p_number` = double; consumes 8 |
| 1 BOOLEAN | `size < 1` -> -1; `p_number` = 0.0 or 1.0; consumes 1 |
| 2 STRING | reads u16 `n` (**before checking `size >= 2`**, D-4); `size < n + 2` -> -1; string value (`av_len = n`, `av_val = NULL` when `n == 0`); consumes `2 + n` |
| 3 OBJECT | `AMF_Decode(obj, buf, size, TRUE)` (R15), -1 -> -1; consumes what it consumed |
| 4 MOVIECLIP, 7 REFERENCE, 14 RECORDSET, 16 TYPED_OBJECT | log ERROR (`"AMF_MOVIECLIP reserved!"`, `"AMF_REFERENCE not supported!"`, `"AMF_RECORDSET reserved!"`, `"AMF_TYPED_OBJECT not supported!"`) and -1 |
| 5 NULL, 6 UNDEFINED, 13 UNSUPPORTED | type becomes NULL; consumes 0 |
| 8 ECMA_ARRAY | `size -= 4` (**not checked**, Q-4); `AMF_Decode(obj, buf + 4, size, TRUE)`; -1 -> -1; the 4-byte count is ignored; consumes `4 + n` |
| 9 OBJECT_END | -1 |
| 10 STRICT_ARRAY | reads u32 `n` (**before checking `size >= 4`**, D-4); `size -= 4`; `AMF_DecodeArray(obj, buf + 4, size, (int)n, FALSE)` (R16, R18); -1 -> -1 |
| 11 DATE | `size < 10` -> -1; `p_number` = double, `p_UTCoffset` = **signed** 16 bit of the next 2 bytes; type stays DATE; consumes 10 |
| 12 LONG_STRING, 15 XML_DOC | reads u32 `n` (before checking, D-4); `size < n + 4` (64-bit arithmetic) -> -1; value as `AMF_DecodeLongString`; LONG_STRING becomes type **STRING**, XML_DOC stays XML_DOC; consumes `4 + n` |
| 17 AVMPLUS | AMF3 switch, unspecified (Q-10); a conforming implementation SHOULD return -1 |
| any other (18..254, 255) | -1 |

**S-021-R15 (object body, `AMF_Decode`).** `AMF_Decode(obj, buf, size, bDecodeName)` first resets `obj` (0 properties). Loop
`while size > 0`:
(a) if `size >= 3` and the next three bytes are `00 00 09`: `size -= 3`, **stop successfully** (end marker consumed);
(b) otherwise `AMFProp_Decode(&prop, buf, size, bDecodeName)`; result -1 -> the whole call returns -1 (the partial list
is unspecified; the caller resets it); `size -= res`; `size < 0` -> return -1; `buf += res`; append the property.
After the loop return `original_size - size`. There is **no error recovery** (the reference contains a
"skip to the next end marker" branch that can never run, Q-6).
Consequences, all CORE: an object/ECMA array/top-level list ends at the end marker **or at the end of the data**
(an unterminated container is accepted, Q-5); an empty input gives 0 properties and returns 0; a name check fails
when 1-3 bytes remain that are not an end marker. The end marker is recognised only *between* properties.

**S-021-R16 (strict array count).** The count read for STRICT_ARRAY is an unsigned 32-bit number passed as a C `int`: counts
>= 2^31 are negative and make `AMF_DecodeArray` loop zero times (Q-7: success with an **empty** array that consumed only
`1 + 4` bytes). Counts that are positive but larger than the remaining values fail with -1 once the data is used up.

**S-021-R17 (top-level semantics).** Script bodies are decoded with `bDecodeName = FALSE`: the result is a flat list of
anonymous values. With `TRUE` each property starts with a name.

**S-021-R18 (`AMF_DecodeArray(obj, buf, size, n, names)`).** Resets `obj`; repeats `n` times (`n <= 0` -> no iteration, returns 0):
`size <= 0` -> -1; `AMFProp_Decode`; -1 -> -1; advance. No end marker is looked for. Returns `original_size - size`.

**S-021-R19 (object model).** `AMF_AddProp` appends a copy of the property. `AMF_CountProp` = `o_num`. `AMF_GetProp(obj, name, idx)`:
`idx >= 0`: the property at `idx` if `idx < o_num` else the INVALID property; `idx < 0`: the **first** property whose name has the
same length and the same bytes as `name`, else INVALID (the `name` is ignored when `idx >= 0`). `AMFProp_GetType` = `p_type`;
`GetNumber` = `p_number` (whatever the type); `GetBoolean` = `p_number != 0`; `GetString` = the string only for type STRING
(XML_DOC gives the empty string), else `{NULL, 0}`; `GetObject` = the object **only for type OBJECT** (ECMA_ARRAY and
STRICT_ARRAY give the invalid object with 0 properties, Q-9); `IsValid` = `p_type != INVALID`. `AMF_Reset` releases all
properties recursively and leaves `o_num = 0`; `AMFProp_Reset` releases an object-typed property recursively and sets
the type to INVALID. A Rust port uses owned values and drops them.

**S-021-R20 (AMF3).** The marker 0x11 selects the AMF3 reader of the reference, whose results depend on uninitialised
memory for several well-formed inputs. It is not specified. No vector contains it (Q-10).

### 5.3 FLV muxer

**S-021-R21 (common tag header).** Every tag written by `flv-mux.c` has: type u8, DataSize u24, timestamp as in section 4.1,
StreamID u24 = 0, body, PreviousTagSize u32 = `11 + DataSize` (the tag length minus 4). DataSize is the body length truncated to
24 bits (D-6).

**S-021-R22 (file header).** `flv_meta_data(write_header=true)` starts with the 13 bytes of section 4.1; with `false` the output is
exactly the script tag. The script tag: type 0x12, DataSize = body length (`u24`), timestamp 0 and extension 0 (written as a single
`u32 0`), StreamID 0, body, PreviousTagSize `11 + body`.

**S-021-R23 (onMetaData body).** The body is: AMF string `"@setDataFrame"`, AMF string `"onMetaData"`, an ECMA array whose declared
count is **20** followed by these 20 entries in this order (name: type = value) and the end marker `00 00 09`; values are doubles
unless noted:

1. `duration` = 0.0; 2. `fileSize` = 0.0; 3. `width` = encoder width; 4. `height` = encoder height;
5. `videocodecid` (R24); 6. `videodatarate` = video encoder `bitrate` setting; 7. `framerate` = video output frame rate;
8. `audiocodecid` = 10.0; 9. `audiodatarate` = audio encoder `bitrate`; 10. `audiosamplerate` = audio encoder sample rate;
11. `audiosamplesize` = 16.0; 12. `audiochannels` = channel count;
13-19 (booleans): `stereo` = (ch == 2); `2.1` = (ch == 3); `3.1` = (ch == 4); `4.0` = (ch == 4); `4.1` = (ch == 5);
`5.1` = (ch == 6); `7.1` = (ch == 8). So 4 channels set both `3.1` and `4.0`; 0, 1, 7 and > 8 channels set none (Q-13).
20. `encoder` = string `"obs-output module (libobs version " + version + ")"`.

Encoding of each entry is R05 (so name = u16 length + bytes, then the typed value). `u32 -> double`, `size_t -> double` conversions are exact
for the ranges used; `NaN`/`inf`/huge doubles are written as is (vector `10-flv-meta`, last case).

**S-021-R24 (codec id and the 4096-byte limit).** `videocodecid`: codec string exactly `"h264"` -> 7.0; `"av1"` -> 1635135537.0 (FourCC `av01`); `"hevc"` -> 1752589105.0
(`hvc1`); anything else (also differently cased) -> 0.0.
The body is built in a **4096-byte buffer** with the encoder rules of R04/R05 (so each `enc_*` entry is subject to its own room check) and the closing `00 00 09` is then written
**raw, without any room check** (three unchecked bytes). The reference is therefore only correct while the complete body, closing bytes included, is at most 4096 bytes; the
`encoder` entry is the only variable-length one, so this is equivalent to `len(version) <= 3699` (default body 414 bytes with a 17-byte version; each extra version byte adds one;
the 3699-byte version fills the buffer exactly, output size 4111). For longer versions the reference overruns the stack buffer (3700) or dereferences NULL (>= 3701, the string
entry is rejected by R04) (D-5). A conforming implementation MUST return an error (no output) when `len(version) > 3699` and must produce the normal output for 3699;
it must NOT apply the end-marker rule of R07 to the closing bytes. The vectors record `crash` for 3700, 3701 and 8000 bytes.

**S-021-R25 (`write_file_info`).** Seeks to absolute offset **58** and writes, with R05 encodings, `"duration"` = `duration_ms / 1000.0`
(double division) and `"fileSize"` = `(double) size`: 19 bytes each (2+8 name, 1+8 value), 38 bytes in total, no other bytes. 58 is the position of the
first entry of the ECMA array in a file produced by `flv_meta_data(true)` (13 + 11 + 16 + 13 + 5). It does not look at the file: with a file made by
`flv_meta_data(false)` the same offset hits other bytes (Q-8; vector `13-flv-fileinfo` last case). Negative and huge values are converted by the double
conversion without checks.

**S-021-R26 (output shape).** A successful tag function returns exactly one complete tag, `11 + DataSize + 4` bytes (preceded by the 13-byte FLV header for `flv_meta_data(true)`); no function returns a partial tag, and no function returns more than one tag.

**S-021-R27 (`ms`).** `ms(p, v) = (i32)((v * 1000) / p.timebase_den)`: 64-bit signed multiplication, signed 64-bit division truncating toward
zero by `timebase_den` (a signed 32-bit number), then truncation to 32 bits (wrap-around modulo 2^32). `timebase_num` is never used. A zero
denominator is a precondition violation (SIGFPE, D-3); the caller must keep `v * 1000` within `i64`. `pts` and `dts` are converted **separately**.
`time(t) = ms(p, p.dts) - dts_offset` (i32 subtraction; may be negative, then the timestamp is written with the sign bits as in 4.1:
low 24 bits of the two's complement and `ext = 0x7f` for small negatives).
`cts = ms(p, p.pts) - ms(p, p.dts)` (i32), written as `BE(cts, 3)` = low 24 bits of the two's complement (so
values outside -2^23..2^23-1 wrap).

**S-021-R28 (legacy video).** `flv_packet_mux` with a packet of type VIDEO (`is_header` selects the AVC packet type). If `data` is NULL or
`size == 0`: no tag (R30). Otherwise the tag has type 0x09, DataSize = `size + 5`, timestamp `time(dts)`, StreamID 0, body:
`frame byte` (`0x17` when `keyframe` else `0x27`), `avc packet type` (0 when `is_header` else 1), `BE(cts, 3)`, the payload; then PreviousTagSize.
`dts_offset` is subtracted from the dts timestamp only; the keyframe flag is any non-zero value.

**S-021-R29 (legacy audio).** Any packet whose type is not VIDEO is muxed as audio: if `data` is NULL or `size == 0` no tag (R30), else type 0x08,
DataSize = `size + 2`, `time(dts)` (pts ignored), body: `0xaf` (AAC, 44 kHz, 16 bit, stereo - fixed whatever the actual format), `0` when `is_header`
else `1`, payload; PreviousTagSize.

**S-021-R30 (empty payloads; the three families differ, Q-16).**
- `flv_packet_mux` (video and audio) with NULL/empty data: produces no tag; `*out = NULL`, `*size = 0`.
- `flv_packet_audio_start/_frames` (Enhanced audio) with NULL/empty data: produces no tag **and does not write `*out` or `*size`** (the runner prefills
  sentinels and prints `untouched`). A Rust port returns `None`.
- `flv_packet_start/_frames/_end` (Enhanced video): a tag with an **empty payload is produced** (an empty slice is fine; a NULL pointer with a non-zero size is a
  caller bug).

**S-021-R31 (Enhanced-RTMP video).** `flv_packet_ex(packet, codec, dts_offset, type, idx)` requires a VIDEO packet and `codec != NONE` (D-1, D-2).
Let `multitrack = (idx > 0)`; `T = type`; frame type `F = 0x10` if `keyframe` else `0x20`
(for *every* packet type including sequence start/end). Packet types: 0 SEQUENCE_START, 1 CODED_FRAMES, 2 SEQUENCE_END, 3 CODED_FRAMES_X,
4 METADATA, 5 MPEG2TS_SEQUENCE_START, 6 MULTITRACK. Body:
- not multitrack: `0x80 | F | T`, FourCC, [cts], payload.
- multitrack: `0x80 | F | 0x06`, `0x00 | T` (multitrack type ONE_TRACK = 0 in the high nibble), FourCC, `idx & 0xff`, [cts], payload.
- `[cts]` = `BE(cts, 3)` (R27) present **only** when the codec is H.264 or HEVC **and** `T == 1`.
FourCC: H264 `avc1`, AV1 `av01`, HEVC `hvc1`. DataSize = `size + 5` (+3 with cts) (+2 multitrack). Timestamp = `time(dts)` with the given `dts_offset`.
Wrappers: `flv_packet_start` = type 0, `dts_offset` 0; `flv_packet_end` = type 2, `dts_offset` 0 (both use the packet's own dts as the timestamp);
`flv_packet_frames` = type 1, except **type 3** when the codec is H.264/HEVC **and `pts == dts` as raw tick values** (not as milliseconds, so
`pts = dts + 1` at 90 kHz gives type 1 with `cts = 0`); AV1 always uses type 1 and never has a cts.
`idx` is truncated to 8 bits for the trackId but `multitrack` stays true (idx 256 -> trackId 0, Q-17).
`flv_packet_ex` can be driven with any `T`; no validation (vector `ex_raw` cases).

**S-021-R32 (multitrack audio).** `flv_packet_audio_ex(packet, codec, dts_offset, type, idx)` requires an AUDIO packet and `codec != NONE`
(D-1, D-2). If `data` is NULL or empty: no tag, outputs untouched (R30). `multitrack = idx > 0`. Body: not multitrack: `0x90 | T`, `"mp4a"`, payload; multitrack: `0x90 | 0x05`,
`0x00 | T`, `"mp4a"`, `idx & 0xff`, payload. DataSize = `size + 5` (+2). Timestamp `time(dts)` (pts ignored). Audio packet types: 0 SEQUENCE_START, 1 CODED_FRAMES, 4
MULTICHANNEL_CONFIG, 5 MULTITRACK. `flv_packet_audio_start` = type 0, offset 0; `flv_packet_audio_frames` = type 1 with the given offset.

**S-021-R33 (audio FourCC).** AAC -> `mp4a`; `AUDIO_CODEC_NONE` is a precondition violation (assert).

**S-021-R34 (colour metadata packet, `flv_packet_metadata`).** Builds the AMF payload
`02 0009 "colorInfo"  03 { 000b "colorConfig" 03 { 0008 "bitDepth" 00 <bits> | 000e "colorPrimaries" 00 <prim> | 0017 "transferCharacteristics" 00 <trc> |
0012 "matrixCoefficients" 00 <space> } 00 00 09 [ 0007 "hdrMdcv" 03 { 000c "maxLuminance" 00 <max> | 000c "minLuminance" 00 <min> } 00 00 09 ] } 00 00 09`,
the `hdrMdcv` object being present **only if `max_luminance != 0`** (a non-zero min alone is dropped). All values are doubles converted from the integer arguments
(`color_primaries` is a `u8`). Tag: type 0x09, DataSize = payload + 5 (+2 multitrack), timestamp 0 (ext 0), StreamID 0; body: not multitrack: `0x84` (ExHeader flag + packet type 4, **no frame type bits**),
FourCC, payload; multitrack: `0x86`, `0x04`, FourCC, `idx & 0xff`, payload. PreviousTagSize. `codec` must not be NONE.

**S-021-R35 (helpers).** `to_video_type`: exact, case-sensitive string compare against `"h264"` (1), `"av1"` (2), `"hevc"` (3, only in builds with HEVC support; the
conformance vectors assume it is enabled), else 0. `to_audio_type`: `"aac"` -> 1 else 0. A NULL string is a precondition violation.

**S-021-R36 (preconditions that trap).** The following are assertion failures in the reference (CRASH in the vectors, D-1..D-3, D-6) and a conforming port
MUST return an error instead: video functions given a non-video packet; audio functions given a non-audio packet; codec NONE in any function that writes a FourCC;
`timebase_den == 0` in any function that converts a timestamp; a tag whose body would not fit the 24-bit DataSize.

### 5.4 HEVC configuration record

**S-021-R37 (entry).** `obs_parse_hevc_header(header, data, size)`. A start code is present iff (`size > 3` and `data[0..3] == 00 00 01`) or (`size > 4` and
`data[0..4] == 00 00 00 01`). If there is **no** start code at the beginning: the output is an exact copy of the input and the return value is `size`
(also for sizes 1-4 and for any contents). Size 0 is D-8 (the reference aborts in the allocator): required result 0 / no header.
With a start code: `size < 6` -> return 0, nothing stored. (The reference's "already hvcC" test on `data[0] == 1` is dead code, because a start code
begins with 0.)

**S-021-R38 (splitting).** Using the start-code finder of S-012 (`obs_nal_find_startcode(p, end)`: the smallest position `q >= p` with `q <= end - 4` and `data[q..q+3] == 00 00 01`, moved back by one if `q > p` and `data[q-1] == 0`;
`end` if there is none - **a start code that lies in the last 3 bytes of the data is not found** (it needs at least one byte after it), so e.g. `... 00 00 00 01` at the very end of the input stays inside the last NAL (Q-15)), the data is split into NAL units: skip to the first start code; for each start code skip its zero bytes and the `01`; if that reaches the end stop; the NAL
extends to the next start code (or the end), i.e. **trailing zero bytes that are not part of a following 4-byte start code stay in the NAL**; empty NALs are not special-cased
(their length is stored as 0 and they are ignored later). Every NAL is stored as `length u32 | bytes` in a temporary buffer; `collected = sum(4 + length)`. If no NAL was found the
function returns 0.

**S-021-R39 (classification).** The temporary buffer is walked while at least 5 bytes remain: `len = min(u32, remaining - 4)`, `type = (first_nal_byte >> 1) & 0x3f`; the NAL
(`len` bytes after the length) is added to the array whose type is VPS (32), SPS (33), PPS (34), SEI prefix (39) or SEI suffix (40); all other types are skipped. Adding a NAL
to its array (a) appends `u16 length | NAL bytes` (the NAL **as found in the stream**, emulation-prevention bytes included, length truncated to 16 bits, Q-18),
(b) increments the array's NAL count, (c) records the array's `nal_unit_type` = the type parsed from the NAL's own header, (d) for VPS/SPS/PPS arrays sets completeness 1 (SEI arrays keep
0), (e) increments `numOfArrays` when the array just became non-empty, (f) parses the NAL (R40). The parse reads an unescaped copy of the NAL (R44 with header length 2) through a bit
reader that returns 0 for bits beyond the NAL end (guaranteed by the reference for at most 64 bytes past the end; behaviour farther past the end is undefined and is not tested).

**S-021-R40 (field extraction).** Initial state: `profile_space = tier = profile_idc = level_idc = 0`, `compat = 0xFFFFFFFF`, `constraint = 0xFFFFFFFFFFFF`, `min_spatial = 4097`,
everything else 0, `lengthSizeMinusOne = 3`. NAL header (16 bits): forbidden(1), type(6), layer_id(6), tid_plus1(3) are skipped except the type; all following bit counts are for the **unescaped** payload after the header. `u(n)` = n bits MSB first,
`ue/se` = Exp-Golomb (section 6.4: note the all-zero behaviour), `skip(n)` = read and drop.
- **VPS**: `skip 12; max_sub_layers_minus1 = u(3); numTemporalLayers = max(.., max_sub_layers_minus1 + 1)` (as u8; 8 wraps the field when shifted, Q-11); `skip 17; PTL(max_sub_layers_minus1)`.
- **SPS**: `skip 4 (vps id); max_sub = u(3); numTemporalLayers = max(.., max_sub + 1); temporalIdNested = u(1); PTL(max_sub); ue (sps id); chromaFormat = ue; if chromaFormat == 3: skip 1;
  ue (width); ue (height); if u(1): 4 x ue (conformance window); bitDepthLumaMinus8 = ue; bitDepthChromaMinus8 = ue (both stored as u8); log2_poc = ue;
  i = (u(1) ? 0 : max_sub); for i..=max_sub: 3 x ue; 6 x ue; if u(1) (scaling_list_enabled) and u(1) (data present): scaling lists; skip 2 (amp, sao);
  if u(1) (pcm): skip 4, skip 4, ue, ue, skip 1; n = ue; n > 64 -> FAIL; for idx in 0..n: RPS(idx, n); if u(1) (long_term_ref_pics_present): k = ue; k > 31 -> FAIL; k times: u(min(log2_poc + 4, 16)) and u(1);
  skip 2 (temporal mvp, strong intra smoothing); if u(1) (vui_present): VUI`. Everything after (and the rest of the VUI) is ignored.
  chromaFormat, bit depths, temporalIdNested are overwritten by **every** SPS (the last one wins); `numTemporalLayers` takes the maximum.
- **PPS**: `ue; ue; skip 7; ue; ue; se; skip 2; if u(1): ue; se; se; skip 4; tiles = u(1); wpp = u(1)`;
  `parallelismType = 3 if wpp and !tiles; 0 if wpp and tiles; 2 if tiles; else 1` (the last PPS wins).
- **PTL(max_sub)**: `space = u(2); tier = u(1); profile = u(5); compat = u(32); constraint = u(48); level = u(8)` -> merge (R41);
  `for i < max_sub: profile_present[i] = u(1); level_present[i] = u(1)`; `if max_sub > 0: skip 2 * (8 - max_sub)`;
  `for i < max_sub: if profile_present[i]: skip 88; if level_present[i]: skip 8`.
- **Scaling lists**: `for sizeId in 0..4: for matrixId in 0..(sizeId == 3 ? 2 : 6): if !u(1): ue; else n = min(64, 1 << (4 + 2*sizeId)); if sizeId > 1: se; n x se`.
- **RPS(idx, num)**: `if idx != 0 and u(1) (inter_ref_pic_set_prediction): idx >= num -> FAIL (cannot happen from an SPS); skip 1; ue; cnt[idx] = 0; for j in 0..=cnt[idx-1]: used = u(1); use_delta = used ? 0 : u(1); if used or use_delta: cnt[idx] += 1;
  else: neg = ue; pos = ue; (pos + neg) * 2 > bits_left -> FAIL (the comparison is unsigned 64-bit: a negative `bits_left`, possible once the reader ran past the NAL end, never fails); cnt[idx] = neg + pos; neg times {ue; skip 1}; pos times {ue; skip 1}`. `bits_left` = payload bits minus bits read (bits of the unescaped NAL; slack bits do not count).
- **VUI** (given `max_sub`): `if u(1): if u(8) == 255: skip 32; if u(1): skip 1; if u(1): skip 4; if u(1): skip 24; if u(1): ue, ue; skip 3; if u(1): 4 x ue;
  if u(1) (timing): skip 32; skip 32; if u(1): ue; if u(1): HRD(max_sub); if u(1) (bitstream_restriction): skip 3; m = ue; min_spatial = min(min_spatial, m) (as u16); 4 x ue`.
- **HRD**(max_sub), common-info present = 1: `nal = u(1); vcl = u(1); sub_pic = 0; if nal or vcl: sub_pic = u(1); if sub_pic: skip 19; skip 8; if sub_pic: skip 4; skip 15.
  for i in 0..=max_sub: fixed_general = u(1); fixed_cvs = 0; if !fixed_general: fixed_cvs = u(1); low_delay = 0; if fixed_cvs: ue else low_delay = u(1);
  cpb = 0; if !low_delay: cpb = ue; cpb > 31 -> stop HRD parsing, silently (the return value is ignored by the caller); if nal: SUBLAYER(cpb); if vcl: SUBLAYER(cpb)`;
  `SUBLAYER(cpb): for 0..=cpb: ue; ue; if sub_pic: ue; ue; skip 1`.
  **Q-12:** when `fixed_pic_rate_general_flag` is 1 the reference does *not* treat `fixed_pic_rate_within_cvs_flag` as 1 (as H.265 requires), so the next bit is read as `low_delay_hrd_flag` instead of an
  Exp-Golomb `elemental_duration`: bit streams written per H.265 are mis-parsed (typically giving `min_spatial = 0`). Conforming ports MUST reproduce this.

**S-021-R41 (PTL merge).** For every PTL parsed (VPS and SPS NAL units, in **NAL stream order**):
`profile_space := new`; `if tier < new_tier { level := new_level } else { level := max(level, new_level) }`; `tier := max(tier, new_tier)`; `profile_idc := max`; `compat &= new`; `constraint &= new` (48 bit).
(A higher tier replaces the level even by a lower one.)

**S-021-R42 (record).** Let `vps, sps, pps` = number of NALs of those types. If any of them is 0, or `vps > 16`, or `sps > 16`, or `pps > 64`: FAIL. Otherwise: if `min_spatial > 4096` then `min_spatial = 0`;
if `min_spatial == 0` then `parallelismType = 0`; `avgFrameRate = 0`, `constantFrameRate = 0`. Output bytes: see section 4.4; `numTemporalLayers << 3` is OR-ed unmasked (a value of 8 would collide with the constantFrameRate bit, Q-11);
`general_profile_space << 6 | tier << 5 | profile_idc` is a plain u8 OR (no masking). Arrays are written in the fixed order VPS, SPS, PPS, SEI prefix, SEI suffix, skipping empty ones; array byte 0 = `completeness << 7 | (nal_unit_type & 0x3f)`,
then u16 count, then the NAL entries. The return value is the record length.

**S-021-R43 (failure, D-7).** FAIL means any of: no NAL found (return 0, which is correct), the counts of R42, a failed SPS parse (R40). In the reference every FAIL except "no NAL" returns the **number of bytes collected in R38 (non-zero)** and leaves `*header`
untouched (the caller then uses garbage). REQUIRED: return failure (0 / `None`). The vectors `35-hevc-header-failures` record the reference's non-zero values with `header=unset`; the comparison treats `ret=N header=unset` as equal to `ret=0 header=unset`.
Failed parses are detected only for the explicit `-1` conditions listed in R40 (num_short_term_ref_pic_sets > 64, long-term count > 31, RPS conditions); a SPS that is merely truncated is parsed with zero bits and produces *some* record (undefined beyond 64 bytes).

**S-021-R44 (`ff_nal_unit_extract_rbsp`).** Copies the first `header_len` bytes verbatim (at most `src_len`), then copies the rest, dropping the `03` of every `00 00 03` (the zeros are tested on the
**source**, starting after the header region, scanning left to right; after dropping, scanning resumes after the `03`), where a pattern that begins at the last 3 bytes of the data counts; then appends 64 zero bytes (not counted). `dst_len` = number of bytes
before the 64 zeros. `dst` must have room for `src_len + 64` bytes. Bytes in the header region never take part in a pattern.

### 5.5 AV1 configuration record

**S-021-R45 (entry).** If `data[0] & 0x80` (reads `data[0]`; empty input is D-9): the data is an `av1C` record: if `(data[0] & 0x7f) != 1` or `size < 4` return 0; else the output is an exact copy of the input (any length >= 4) and the
return value is `size`.

**S-021-R46 (OBU scan).** Otherwise the data is a sequence of OBUs. `parse_obu_header(buf, remaining)`: look at the first `min(remaining, 10)` bytes only: `forbidden = bit7 -> must be 0`; `type = bits 6..3`; `ext = bit 2`; `has_size = bit 1`; bit 0 reserved;
if `ext`: one extension byte follows (ignored); if `has_size`: LEB128 `obu_size` (1-8 bytes, 7 bits per byte little-endian, stops at the first byte with bit 7 clear; at most 8 bytes are read) else `obu_size = remaining - 1 - ext`.
`start = 1 + ext + (leb length)`; if the header does not fit the window (bits read > bits available) or `obu_size + start > remaining` the OBU is **invalid** (-1). The OBU's length is `obu_size + start` (header + payload).
Scan: for each OBU: invalid -> FAIL(0). Type 1 (sequence header): `nb_seq += 1`; `obu_size == 0` or `nb_seq > 1` -> FAIL; parse the payload (R47), failure -> FAIL; remember the OBU (header + payload) as `seq`. Type 5 (metadata): `obu_size == 0` -> FAIL; remember the OBU as `meta` (the last one wins).
All other types are skipped. After the scan `nb_seq == 0` -> FAIL.

**S-021-R47 (sequence header parse).** Let `P` be the payload. Its bit length is: drop all trailing `00` bytes; if none are left the length is 0; else `8 * bytes - (trailing_zero_bits(last byte) + 1)` (the trailing one-bit and the zeros after it are not part of the syntax).
Syntax (bits MSB first), with the rule that **the syntax must consume exactly that many bits, otherwise FAIL**:
```
profile=u(3); skip 1 (still_picture); reduced=u(1);
if reduced: level = u(5); tier = 0
else:
   timing = u(1); dm = 0
   if timing: skip 32; skip 32; if u(1): uvlc();  dm = u(1);  if dm: dl = u(5) + 1; skip 32; skip 10
   idd = u(1); ops = u(5) + 1
   for i in 0..ops: skip 12; lvl = u(5); tier_i = (lvl > 7) ? u(1) : 0;
        if dm: if u(1): skip dl; skip dl; skip 1
        if idd: if u(1): skip 4
        if i == 0: level = lvl; tier = tier_i
wb = u(4) + 1; hb = u(4) + 1; skip wb; skip hb
if !reduced: if u(1): skip 7
skip 3
if !reduced: skip 4; oh = u(1); if oh: skip 2;
             force = u(1) ? 2 : u(1);  if force != 0: if !u(1): skip 1;   if oh: skip 3
skip 3
color_config:
   hbd = u(1); twelve = (profile == 2 and hbd) ? u(1) : 0; bitdepth = 8 + 2*hbd + 2*twelve
   mono = (profile == 1) ? 0 : u(1)
   cd = u(1); if cd: cp = u(8); tc = u(8); mc = u(8) else cp = tc = mc = 2
   if mono: range = u(1); subx = suby = 1; pos = 0; (return: no further bits here - separate_uv_delta_q is NOT read)
   else if cp == 1 and tc == 13 and mc == 0: subx = suby = 0 (pos stays 0; no range bit)
   else: range = u(1);
        profile 0: subx = suby = 1; profile 1: subx = suby = 0;
        other profiles: if twelve: subx = u(1); suby = subx ? u(1) : 0   else subx = 1, suby = 0
        if subx and suby: pos = u(2)
   unless mono: skip 1 (separate_uv_delta_q)
skip 1 (film_grain_params_present)
```
`uvlc`: count zero bits until a one bit (stop also at the end of the data); if the count is >= 32 read nothing more, else skip that many bits. Initial values `pos = 0`.

**S-021-R48 (record).** The output is `81`, `profile << 5 | level`, `tier << 7 | (bitdepth > 8) << 6 | (bitdepth == 12) << 5 | mono << 4 | subx << 3 | suby << 2 | pos`, `00`, then the complete `seq` OBU bytes (as in the input, including its header and size field), then (if any) the
complete last `meta` OBU. The return value is the total length. Note `level` is `seq_level_idx[0]` and `tier` its tier bit.

**S-021-R49 (failure and over-reads, D-9).** FAIL returns 0 and stores nothing. The reference reads 1-2 bytes beyond the end of the input for cut-off OBU headers, cut-off size fields and sequence headers whose syntax is longer or shorter than their payload (the
vectors `42-av1-header-defects` are `crash`). REQUIRED: such inputs are FAIL (0); in particular a payload whose syntax would need bits past its end (and one that leaves unread bits) is a failure, an OBU header/size field that is cut off is invalid.

**S-021-R50 (`obs_parse_av1_packet`).** Output packet = copy of `src` with new `data/size`; scanning the OBUs of `src.data` exactly as in R46 (an invalid OBU **ends the scan silently** keeping the OBUs copied so far; there is no failure). OBU types 2 (temporal delimiter), 7 (redundant frame header) and 8 (tile list) are dropped; every other
OBU (including sequence header, metadata, padding, tile group, OBUs with extension, OBUs without size field) is copied complete (header + payload). Before copying a type 6 (frame) or type 3 (frame header) OBU, `PRIORITY(payload)` is evaluated (R51).
At the end `drop_priority := priority`. `pts/dts/timebase/type/...` are untouched.

**S-021-R51 (`PRIORITY`).** If the packet's `priority` is already non-zero: nothing happens. Else take the **first payload byte** `b` of the OBU (this reads `buf[start]` even when `obu_size == 0`: D-9, required: skip the evaluation): if `b & 0x80` (show_existing_frame) nothing; else
`frame_type = (b >> 5) & 3`, `show = (b >> 4) & 1`: type 0 (KEY): `keyframe = true; priority = 3`; type 1 (INTER): `priority = show ? 1 : 2`; type 2 (INTRA_ONLY): 3; type 3 (SWITCH): 2. `keyframe` is never cleared; later OBUs find a non-zero
priority and do nothing. A show-existing OBU leaves the priority 0 so that a later frame OBU in the same packet can still set it.

## 6. Algorithms

### 6.1 Timestamp pipeline (pseudocode)

```
fn ms(p, v) -> i32 { ((v * 1000) / p.timebase_den as i64) as i32 }       // wraps mod 2^32
fn tag_ts(p, dts_offset) -> (u24, u8) { t = ms(p, p.dts) - dts_offset;  (t & 0xFFFFFF, (t >> 24) & 0x7F) }
fn cts(p) -> i32 { ms(p, p.pts) - ms(p, p.dts) }
```

### 6.2 Container encoding (pseudocode)

```
fn encode_container(kind, obj, w):               // w = remaining window of length room
   if room <= 4: return NULL
   write marker(kind); if kind in {ecma, strict}: write u32 obj.count
   for p in obj.props: r = encode_prop(p, w'); if r == NULL: log_error(index); break; else advance
   if kind == strict: return pos
   if remaining_room <= 3: return NULL
   write 00 00 09; return pos
```

### 6.3 Decoding loop (pseudocode)

```
fn decode_list(buf, size, names) -> (props, consumed) | -1:
   left = size; props = []
   while left > 0:
      if left >= 3 and buf[..3] == 00 00 09:  left -= 3; break
      n = decode_prop(buf, left, names) ; if n == -1: return -1
      left -= n; if left < 0: return -1
      buf += n; props.push(prop)
   return size - left
```

### 6.4 Exp-Golomb reading used by the HEVC parser

`ue()`: let `x` = the next 32 bits without consuming them (bits beyond the NAL read 0). `lz = 31 - floor(log2(x | 1))` (so `x = 0` gives 31); skip `lz` bits; value = (read `lz + 1` bits) - 1 (as u32; with `lz = 31` and
all-zero bits this is 0xFFFFFFFF = 2^32 - 1, consuming 63 bits). `se()`: `k = ue(); sign = (k & 1) ? +1 : -1; value = sign * ((k + 1) >> 1)` (modulo 2^32 arithmetic; only the bit consumption matters here, results are discarded).
Reading beyond the NAL yields zero bits; after 64 bytes of such slack the reference reads unrelated memory (not tested).

## 7. Concurrency

None of these functions use shared state except the log callback of librtmp. All are re-entrant; the muxer functions operate on caller-owned
packets and buffers. The output plugins call them from the output thread (muxing) while the encoder threads are separate.

## 8. Reference defects and quirks

Classes: **QUIRK** (SHOULD match), **DEFECT** (crash / UB; required behaviour given). Demonstration scripts are listed.

| ID | Description | Script |
|---|---|---|
| Q-1 | Short AMF strings need `len + 5` bytes of room (R04) | `03-amf-encode-capacity` |
| Q-2 | A NULL-typed property inside a container drops its name and is a single `05` byte (R06) | `04-amf-encode-objects` |
| Q-3 | A property that does not fit is dropped (log error) and the container is still closed; ECMA count unchanged (R07, R08, R10) | `05-`, `06-` |
| Q-4 | `AMFProp_Decode` of an ECMA array with fewer than 4 bytes after the marker returns more bytes than supplied; also `08 00000001` consumes the marker (R13, R14) | `08-amf-decode-truncated` |
| Q-5 | Containers (object, ECMA, top-level list) without end marker are accepted when the data ends (R15) | `08-` |
| Q-6 | The error-skipping branch of `AMF_Decode` is dead; a bad property always fails the whole call; 1-3 trailing bytes that are not an end marker fail (R15) | `08-` |
| Q-7 | Strict array counts >= 2^31 decode as empty arrays (R16) | `07-amf-decode-values` |
| Q-8 | `write_file_info` assumes the FLV header is present (offset 58) (R25) | `13-flv-fileinfo` |
| Q-9 | `AMFProp_GetObject` ignores ECMA and strict arrays (R19) | `09-amf-accessors` |
| Q-10 | AMF3 (0x11) not specified: ports SHOULD fail (-1) (R14, R20) | none |
| Q-11 | `numTemporalLayers == 8` (max_sub_layers_minus1 = 7) overflows into the constantFrameRate bit of the record (R42) | `31-hevc-header-ptl-merge` |
| Q-12 | `fixed_pic_rate_general_flag = 1` handled differently from H.265 in the VUI HRD parser (R40) | `32-hevc-header-sps-syntax` |
| Q-13 | The FLV channel flags: 4 channels set `3.1` and `4.0`; 1, 7, > 8 set none (R23) | `11-flv-meta-channels` |
| Q-14 | `flv_packet_start/_end` use the packet's own dts as timestamp (offset 0); the legacy audio byte `0xaf` is fixed; `timebase_num` is ignored (R27, R29, R31) | `14-`, `15-`, `16-` |
| Q-15 | The start-code finder misses a start code in the last 3 bytes of the data (R38) | `30-hevc-header-basic` |
| Q-16 | The three empty-payload behaviours of the muxers (R30) | `20-flv-empty-and-quirks` |
| Q-17 | The multitrack trackId is `idx & 0xff`: idx 256 gives trackId 0 but stays multitrack (R31, R32) | `17-flv-ex-multitrack`, `18-flv-ex-audio`, `20-` |
| Q-18 | A NAL longer than 65535 bytes gets a truncated u16 length in the hvcC array (R39) | `34-hevc-header-arrays` |
| Q-19 | `AMF_DecodeLongString` lengths >= 2^31 are negative `av_len`, value NULL (R12) | `02-amf-strings` |
| Q-20 | `obs_parse_av1_packet` silently keeps the OBUs before a damaged one (R50) | `43-av1-packet` |
| D-1 | wrong packet type (video function with audio packet and vice versa) asserts (R31, R32, R36) | `21-flv-defects` |
| D-2 | codec NONE asserts (R33, R36) | `21-` |
| D-3 | `timebase_den == 0` divides by zero (R27, R36) | `21-` |
| D-4 | `AMFProp_Decode` reads the length field of STRING/STRICT_ARRAY/LONG_STRING/XML_DOC before checking `size`: with 0-1 (resp. 0-3) bytes left it reads past the buffer. REQUIRED: -1. | `08-` |
| D-5 | `flv_meta_data` property buffer (4096 bytes) overrun / NULL deref when the version string exceeds 3699 bytes. REQUIRED: error. | `12-flv-meta-version` |
| D-6 | the 24-bit DataSize wraps for payloads of >= 16 MiB - header. REQUIRED: error (the runner models the failure) | `21-` |
| D-7 | `obs_parse_hevc_header` failure after splitting returns a non-zero size with `*header` unset. REQUIRED: 0. | `35-` |
| D-8 | `obs_parse_hevc_header` with empty input aborts in `bmalloc(0)`. REQUIRED: 0 / no header. | `36-` |
| D-9 | `obs_parse_av1_header` / `obs_parse_av1_packet` read beyond the input for cut-off OBUs, empty input, and a frame OBU with empty payload; REQUIRED: R49, R51. For `av1_packet` the crash case `3200` must yield `size=2 ... data=3200` (the empty-payload frame OBU is copied without computing a priority). | `42-`, `43-` |

## 9. Implementation notes (non-normative)

- Write AMF0 with a small `Vec<u8>` writer plus an optional hard capacity if you want to reproduce the room rules (R01-R10); the capacity
  rules matter only for the 4096-byte `onMetaData` buffer (R24) and for callers that encode into fixed buffers (RTMP connect commands).
  The decoder should produce an owned tree (`enum AmfValue`) but must keep the *consumed byte count* semantics of R13-R18, including Q-4/Q-5.
- Keep names as `Vec<u8>` (bytes, not UTF-8). `Date` keeps `utc_offset: i16`; XML documents keep their bytes.
- The FLV timestamp code is the part most often wrong: use `i32` arithmetic with `wrapping_*`, `i64` for `pts * 1000`.
- The hvcC bit reader can be a plain `u64` accumulator over a `Vec<u8>` extended with 64 zero bytes; make `ue()` tolerate all-zero input as described in 6.4.
- Do not rewrite the stored NAL bytes: copy them from the input (with emulation prevention).
- A `Result`-returning API removes D-1..D-9 naturally; map every `crash` of the vectors to `Err`.
- Build the fake-encoder inputs as a struct (`MetaDataInput`) for `flv_meta_data`.
- The reference leaks the partially built property arrays of a failed nested decode (`AMF_Decode` returning -1 never frees what it collected); owned Rust values need no counterpart. Leak detection was not possible in the fork-per-command conformance runner.

## 10. Conformance coverage

| Rule | Scripts |
|---|---|
| R01 | `01-amf-ints-numbers`, `03-amf-encode-capacity` |
| R02, R03 | `01-amf-ints-numbers`, `03-` |
| R04 | `02-amf-strings`, `03-` |
| R05 | `02-`, `03-` |
| R06 | `04-amf-encode-objects`, `05-amf-encode-object-limits`, `06-amf-encode-sweeps` |
| R07, R08, R09, R10 | `04-`, `05-`, `06-` |
| R11, R12 | `01-`, `02-` |
| R13, R14 | `07-amf-decode-values`, `08-amf-decode-truncated` |
| R15 | `07-`, `08-` |
| R16 | `07-` (Q-7 block), `08-` |
| R17, R18 | `07-` |
| R19 | `09-amf-accessors` |
| R20 | (no vector; Q-10) |
| R21-R24 | `10-flv-meta`, `11-flv-meta-channels`, `12-flv-meta-version` |
| R25 | `13-flv-fileinfo` |
| R26 | see R28, R29 |
| R27 | `14-flv-legacy-video`, `15-flv-legacy-audio`, `16-flv-ex-video`, `22-flv-helpers` |
| R28 | `14-flv-legacy-video` |
| R29 | `15-flv-legacy-audio` |
| R30 | `20-flv-empty-and-quirks` |
| R31 | `16-flv-ex-video`, `17-flv-ex-multitrack` |
| R32, R33 | `18-flv-ex-audio` |
| R34 | `19-flv-ex-metadata` |
| R35 | `22-flv-helpers` |
| R36 | `21-flv-defects` |
| R37 | `30-hevc-header-basic`, `36-hevc-header-passthrough` |
| R38, R39 | `30-`, `34-hevc-header-arrays` |
| R40 | `31-hevc-header-ptl-merge`, `32-hevc-header-sps-syntax`, `33-hevc-header-pps` |
| R41 | `31-` |
| R42 | `30-`, `31-`, `33-`, `34-` |
| R43 | `35-hevc-header-failures` |
| R44 | `37-hevc-rbsp` |
| R45 | `41-av1-header-obus` |
| R46 | `41-` |
| R47, R48 | `40-av1-header-sequence` |
| R49 | `41-`, `42-av1-header-defects` |
| R50, R51 | `43-av1-packet` |

## 11. Open questions

1. AMF3 (`AMF_AVMPLUS` and the `AMF3*` functions) is unspecified because the reference reads uninitialised memory for some well-formed inputs;
   the RTMP command layer of OBS never decodes AMF3 from the server in the supported flows. Confirm that failing (-1) is acceptable.
2. HEVC `ENABLE_HEVC` is a compile-time option of the plugin; the vectors assume it is on. A port must expose it as a feature or a constant.
3. D-5: the exact capacity accounting of the 4096-byte property buffer was characterised for the default entries (limit 3699 version bytes). If entries are
   added later the limit changes; the rule is "the body must fit 4096 bytes with the same encoder rules".
4. Behaviour of `obs_parse_hevc_header` for SPS data truncated by more than 64 bytes (the reference reads foreign memory) is not specified.
5. `AMF_Dump`, `AMFProp_Dump` only log at debug level and are not covered.
