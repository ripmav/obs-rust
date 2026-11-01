# S-022 MP4 / MOV muxer (hybrid fragmented-then-remuxed ISO BMFF writer)

| | |
|---|---|
| Status | Ready for review |
| Reference | libobs 33.0.1 `plugins/obs-outputs` `mp4-mux.c`, `mp4-mux.h`, `mp4-mux-internal.h` (+ the packet/extradata helpers they call: `obs-avc.c`, `obs-hevc.c`, `rtmp-hevc.c`, `rtmp-av1.c`) |
| Target crate | `crates/obs-mp4-mux` |
| Depends on | S-001 (packet blocks), S-010 (serializer interface: `s_w8/s_wb16/s_wb24/s_wb32/s_wb64/s_write`, `get_pos`, `seek`, array serializer), S-012 (AVC/HEVC packet and AVC header conversion), S-014 (`obs_data` JSON, only for the optional encoder-info atom) |
| Conformance | `test/conformance/scripts/mp4/` (30 scripts, vectors in `vectors/mp4/`) |

## 1. Scope

The muxer turns a stream of encoded packets (video: H.264, HEVC, AV1, ProRes; audio: AAC, Opus, FLAC,
ALAC, PCM s16/s24/f32) into one `.mp4` or `.mov` file written through a *seekable* serializer. The
file is produced in two phases:

1. **Fragmented phase.** While packets arrive, the muxer writes a fragmented MP4 (ftyp, a 16-byte
   placeholder box, an initial `moov` with `mvex`, then `moof`+`mdat` pairs). The file is therefore
   playable/recoverable after a crash.
2. **Finalisation (soft remux).** `mp4_mux_finalise` appends a complete, non-fragmented `moov` at the end
   of the file and then overwrites the beginning: `ftyp` again, and the placeholder becomes the header of
   one huge `mdat` that *contains* the whole fragmented file (initial moov, all moof/mdat). Chunk
   offsets of the final `moov` point into that data. The result is a normal "faststart-less" MP4.

In scope: every byte of the produced file and the *moments* at which bytes reach the serializer
(visible as the serializer position); track construction from the output's encoders; fragment
splitting; edit lists, chapters, metadata atoms; the transformations applied to submitted packets;
all quirks that change the file. Out of scope: `mp4-output.c` (the `obs_output` wrapper: file naming,
split-file handling, signals), building an `hvcC` or `av1C` from raw parameter sets (see 3.3; needs its own spec), `FLAVOR_CMAF` (declared, "not yet implemented": behaves exactly like MP4, see Q-14).

Callers: `plugins/obs-outputs/mp4-output.c` only. The muxer reads configuration of the output and
encoders through libobs accessors (3.2); a re-implementation takes these as plain input data.

## 2. Terminology and conventions

RFC 2119 words. All multi-byte integers in the file are **big-endian** ("be16/be32/be64", `u8/u16/u24`).
`fourcc("abcd")` = the four ASCII bytes. "tick" = unit of a track timescale. `div` = integer division
truncating toward zero on the signed 64-bit values stated; `mul_div(a, b, c)` = `a*b/c` computed in
unsigned 64-bit arithmetic with a 128-bit intermediate (`util_mul_div64`: exact quotient, truncated;
`c != 0`). `usec(p) = p.pts * 1_000_000 / p.timebase_den` (signed 64-bit, truncating, overflows for
`|pts| > 9.2e12`; not guarded).

Box notation (ISO 14496-12 section 4.2):

```
box(T, payload)          = be32(8 + len(payload)) fourcc(T) payload          -- size <= 0xFFFFFFFF
box(T, payload) [large]  = be32(1) fourcc(T) be64(16 + len(payload)) payload  -- size > 0xFFFFFFFF
fullbox(T, v, f, payload)= box(T, u8(v) u24(f) payload)
```
The large form is only produced by the generic helper for sizes beyond 4 GiB (never reached by the
vectors; the sizes of ordinary boxes here are tiny). Many boxes are written with a placeholder size that
is patched after the content; only the final byte image matters. "Size N" below always means the
final total size of the box including its 8-byte (or 12-byte fullbox) header.

Pseudocode uses `+`/`-` on exact integers unless a cast `(u32)` / `(i32)` / `(u16)` is written, which
truncates modulo 2^N like C. Where two casts matter (`(u32)(a*b/c)`), the operation order is as written.

## 3. Interface

### 3.1 API

```c
enum mp4_flavor { FLAVOR_MP4 = 0, FLAVOR_MOV = 1, FLAVOR_CMAF = 2 };
enum mp4_mux_flags {
    MP4_USE_MDTA_KEY_VALUE = 1 << 0,  // mdta key/value metadata instead of QuickTime/iTunes keys
    MP4_WRITE_ENCODER_INFO = 1 << 1,  // track udta gets encoder id + settings JSON
    MP4_SKIP_FINALISATION  = 1 << 2,  // leave the file fragmented
    MP4_USE_NEGATIVE_CTS   = 1 << 3,  // negative composition offsets instead of edit list delay
};
struct mp4_mux *mp4_mux_create(obs_output_t *output, struct serializer *s, enum mp4_mux_flags, enum mp4_flavor);
void   mp4_mux_destroy(struct mp4_mux *);
bool   mp4_mux_submit_packet(struct mp4_mux *, struct encoder_packet *pkt);   // false: no track for pkt->encoder
bool   mp4_mux_add_chapter (struct mp4_mux *, int64_t dts_usec, const char *name); // false: dts_usec < 0
bool   mp4_mux_finalise    (struct mp4_mux *);                                // always true
```
`struct serializer` must support write, `get_pos` (the position at which the next byte would be written,
i.e. `ftell`) and `seek(SERIALIZE_SEEK_START)` to any previously valid position (S-010). The array
serializer's `get_pos` returns its *length*, not its cursor: the muxer relies on `get_pos` meaning the
write cursor and uses array serializers only for append-only scratch buffers (see 6.3), so a sink for
the *real* output MUST be a file-like object whose position is its cursor.

**R01** (calling contract) Calls are made from one thread, in the order: `create`, any number of
`submit_packet` / `add_chapter`, then `finalise` at most once, then `destroy`. `destroy` without
`finalise` is allowed (queued packets are released, nothing more is written). Submitting or finalising
after `finalise`, and finalising twice, are outside the contract (they keep appending to a file that was
already rewritten; vectors do not cover them).

`mp4_mux_submit_packet` takes its own reference of the packet data (`obs_encoder_packet_ref`); the caller
keeps its reference. Packet data blocks follow S-012 section 3.2 (reference count in the 8 bytes
before the data).

### 3.2 Inputs the muxer queries (stubbed in `gen/mp4_stubs.c`)

An implementation receives these as data. Each row names the libobs accessor, the stub in
`gen/mp4_stubs.c`, and the script key (`enc ... key value`) that sets it.

| Input | Accessor (reference) | Script key | Default | Used for |
|---|---|---|---|---|
| output name | `obs_output_get_name` | (fixed `conformance`) | `conformance` | log prefix only |
| video encoders, slot i | `obs_output_get_video_encoder2(out, i)`, `i < MAX_OUTPUT_VIDEO_ENCODERS` | `slot` (video) | next free slot | track order |
| audio encoders, slot i | `obs_output_get_audio_encoder(out, i)`, `i < MAX_OUTPUT_AUDIO_ENCODERS` (= 6) | `slot` (audio) | next free slot | track order |
| encoder type | `obs_encoder_get_type` | `enc NAME video\|audio` | | VIDEO/AUDIO |
| codec string | `obs_encoder_get_codec` | `codec` | `h264` / `aac` | selects `mp4_codec` (R10) |
| encoder id | `obs_encoder_get_id` | `id` (`-` = NULL) | `stub_encoder` | `compressorname`, `(c)enc` atom |
| encoder name | `obs_encoder_get_name` | `name` (`-` = NULL) | `Stub Encoder` | track `name` atom |
| video size | `obs_encoder_get_width/height` | `width`, `height` | 1920, 1080 | tkhd, visual sample entry |
| video output info | `video_output_get_info(obs_encoder_video(e))`: `fps_num`, `fps_den`, `colorspace` (`VIDEO_CS_DEFAULT/601/709/SRGB/2100_PQ/2100_HLG`), `range` (`FULL` or other) | `fps N/D`, `cs`, `range` | 30/1, `709`, `partial` | timebase, `colr`, `clli`, `mdcv` |
| audio sample rate (encoder) | `obs_encoder_get_sample_rate` | `rate` | 48000 | audio timescale |
| audio output info | `audio_output_get_info(obs_encoder_audio(e))->speakers`, `audio_output_get_channels`, `audio_output_get_sample_rate` | `speakers` (`mono stereo 2.1 4.0 4.1 5.1 7.1 unknown`), `arate` | stereo, 48000 | channel counts, layouts, MOV version choice |
| frame size | `obs_encoder_get_frame_size` | `frame_size` | 1024 | MOV v1 header only |
| extra data | `obs_encoder_get_extra_data` (returns false when absent) | `extra HEX` or `-` | absent | avcC/hvcC/av1C/esds/dOps/dfLa/alac |
| settings | `obs_encoder_get_settings` (new reference, may be NULL): ints `bitrate` (kbps), `codec_type` (ProRes FourCC), arbitrary other keys for the JSON | `bitrate`, `codec_type`, `setting_str name=value`, `settings none\|empty` | NULL | esds bitrate, ProRes tag, `json` atom |
| HDR peak | `obs_get_video_hdr_nominal_peak_level` (float) | `peak F` | 1000 | `clli`, `mdcv` |
| version string | `obs_get_version_string` | `version S` | `33.0.1` | metadata atoms |
| locale | `obs_module_text("MP4Output.StartChapter")` | `start_chapter_text S` | `Start` | automatic first chapter name |
| clock | `time(NULL)` | `clock SECONDS` | 1700000000 | creation time |

Encoder `video`/`audio` accessors returning NULL are a precondition violation (D-5, D-6).
Accessors given a NULL encoder (this happens for the chapter track, which has no encoder) return 0 /
NULL / false and log a debug line in libobs; the harness stubs return the same values without the log.
An implementation represents the chapter track as a track without encoder where `width = height = 0`,
`name = None`, `id = None`, `settings = None`.

### 3.3 Helper transformations the muxer depends on

* AVC packets: `obs_parse_avc_packet` (S-012-R22). AVC extra data: `obs_parse_avc_header` (S-012-R30..R35).
* HEVC packets: `obs_parse_hevc_packet` (S-012-R53).
* HEVC extra data: `obs_parse_hevc_header(extra)`. Specified here only for the branch the vectors use:
  if `extra` does not start with an Annex-B start code (`00 00 01` or `00 00 00 01` at offset 0) the result
  is an exact copy of `extra` (a finished `hvcC`). Otherwise (Annex-B parameter sets) the result is a
  newly computed `hvcC` or empty (size 0); that algorithm is not part of this spec. An empty result
  yields an empty `hvcC` box (size 8).
* AV1 extra data: `obs_parse_av1_header(extra)`: if `extra[0] & 0x80`: if `(extra[0] & 0x7f) != 1`
  or `len < 4` the result is empty (size 0); else an exact copy. If the high bit is clear the sequence
  header OBU is parsed and an `av1C` built (not part of this spec); invalid input gives an empty result.
  An empty result yields an empty `av1C` box (size 8). (`extra` with len 0 is not defined; precondition.)
* AV1 packets: `obs_parse_av1_packet`, specified in section 5.8 (R80..R82) because it lives in
  `rtmp-av1.c` and is not covered by S-012.

## 4. Data formats

### 4.1 File layout

Fragmented phase (also the final layout when `MP4_SKIP_FINALISATION` is set):

```
offset 0                ftyp                       (R20)
placeholder_offset      free|wide, 16 bytes        be32(16) "free" be64(0)    ("wide" for MOV)
                        moov (initial)             tracks + mvex; no samples; durations 0 (R50)
                        { moof  mdat }*            one pair per fragment (R40)
```

After `finalise` (default):

```
0                       ftyp                       same size as before, brands differ (R20)
placeholder_offset      mdat header                be32(size) "mdat"  (8 bytes), then the 8 zero bytes that used to
                                                   be the payload of the free box, then the complete
                                                   old fragmented file from the initial moov to the end of the
                                                   last mdat; size = data_end - placeholder_offset
data_end                moov (final, non-fragmented)  (R60)
```
If `data_end - placeholder_offset > 0xFFFFFFFF` the 16-byte header `be32(1) "mdat" be64(size)` is used
instead (this is why the placeholder is 16 bytes); not covered by vectors.

Worked example (vector `01-basic-av`: H.264 640x360 30 fps, three frames with payloads of 20/12/13 bytes (key,
non-key, non-key), AAC 48 kHz stereo with extra data `1190`, three packets of 10/11/12 bytes, default clock
so `creation = 1700000000 + 0x7C25B080 = 0xE179A180`). One fragment is written at finalisation:

```
final file, 3134 bytes
@0     ftyp   size 36   "isom" 00000000 | isom obs1 iso2 avc1 mp41
@36    mdat   size 1675 = 8 (header) + 8 (zero bytes of the former free box) + 1349 + 224 + 86
@52      moov size 1349 (initial, fragmented: mvhd, 2 trak, mvex, udta)
@1401    moof size 224  (mfhd, traf video, traf audio)   trun.data_offset of the first track = 224 + 8 = 232 (0xe8)
@1625    mdat size 86   = 8 + 20+12+13 (video) + 10+11+12 (audio)
@1711  moov   size 1423 (final: mvhd, trak video, trak audio, udta)
```
Video sample data starts at 1633 (`stco = 0x661`), audio at 1633 + 45 = 1678 (`0x68e`). Final-moov tables: video
`stts 1x(3, 1)`, `stss {1}`, `stsc 1x(first 1, 3 samples, desc 1)`, `stsz {20,12,13}`, `elst {duration 100 ms,
media_time 0}`, `mdhd.duration 3`; audio `stts 1x(3, 1024)`, `stsz {10,11,12}`, `mdhd.duration 3072`,
`sgpd 01000000 726f6c6c 00000002 00000001 ffff`, `sbgp 00000000 726f6c6c 00000001 00000003 00000001`.
The full byte image is in `vectors/mp4/01-basic-av.expected`.

### 4.2 Box layouts

All sizes in the table are *total* box sizes. `[c]` = variable; formulas given.

**ftyp** (R20). MP4: `fourcc(major) be32(0) fourcc(major) [ "isom" if major != "isom" ] (fragmented && major != "iso6" ? "iso6" : "obs1") "iso2" [ "avc1" ] "mp41"`.
MOV: `"qt  " be32(0x20140200) "qt  "` (size 20).

**free/wide**: 16 bytes `be32(16) "free"|"wide" be64(0)`.

**mvhd** (v0 size 108, v1 size 120): `fullbox("mvhd", v, 0, creation, modification, be32(1000), duration, be32(0x00010000) be16(0x0100) be16(0) be32(0) be32(0) matrix[9] be32(0)*6 be32(next_track_ID))`. creation/modification/duration are be32 (v0) or be64 (v1). `matrix` = `0x00010000 0 0 0 0x00010000 0 0 0 0x40000000`.

**tkhd** (v0 92, v1 104): `fullbox("tkhd", v, 3, creation, modification, be32(track_id), be32(0), duration, be32(0) be32(0), be16(0) /*layer*/, be16(audio?1:0) /*alternate_group*/, be16(audio?0x0100:0) /*volume*/, be16(0), matrix[9], be32(width<<16), be32(height<<16))`; width/height are `0` for audio tracks; for chapters `obs_encoder_get_width(NULL) = 0`. `(width << 16)` is u32 arithmetic (width >= 65536 loses the high bits).

**edts / elst** (size 36 / 28): `box("edts", fullbox("elst", 0, 0, be32(1), be32((u32)segment_duration), be32((u32)media_time), be32(0x00010000)))` (R52).

**mdia**: `box("mdia", mdhd hdlr minf)`. **mdhd** (v0 32, v1 44): `fullbox("mdhd", v, 0, creation, modification, be32(timescale), duration, be16(lang), be16(0))`. In v1 the creation/modification are be64, timescale be32 and the duration field is be64 holding the *32-bit-truncated* value (Q-3). `lang` = 21956 (0x55C4 "und") for MP4, 32767 for MOV.

**hdlr** (size 8+4+4+4+4+12 + name): `fullbox("hdlr", 0, 0, A, fourcc(handler), be32(0)*3, name)`. MP4: `A = be32(0)`, `name` = string + NUL. MOV: `A = fourcc("mhlr")` for track hdlr and `"dhlr"` for the data-handler hdlr inside minf; `name` = Pascal string (u8 length + bytes). handler = `vide` (video), `text` (chapters), `soun` (audio); the minf data hdlr uses `"url "`. Names: `OBS Video Handler`, `OBS Chapter Handler`, `OBS Audio Handler`, `OBS Data Handler`.

**vmhd** (20): `fullbox("vmhd", 0, 1, be16(0)*4)`. **smhd** (16): `fullbox("smhd", 0, 0, be16(0), be16(0))`. **gmhd** (chapters): `box("gmhd", gmin text)`; `gmin = fullbox("gmin",0,0, be16(0x40) be16(0x8000)*3 be16(0) be16(0))` (24); `text = box("text", be16(1) be32(0) be32(0) be32(0) be32(1) be32(0) be32(0) be32(0) be32(0x4000) be16(0))` (44).

**dinf** (36): `box("dinf", fullbox("dref", 0, 0, be32(1), fullbox("url ", 0, 1, "")))` (dref 28, url 12).

**minf** order: vmhd | gmhd | smhd; (MOV only) `hdlr` with no track; dinf; stbl.

**stbl** order: `stsd stts [stss] [ctts] stsc stsz stco|co64 [sgpd sbgp]` (R53..R59).

**stsd**: `fullbox("stsd", V, 0, be32(1), entry?)` where `V` = 1 for MP4 audio tracks whose audio output has more than 2 channels, else 0 (R70). Entry per codec (R72..R78). Unknown audio codec or a track kind without entry: count is still 1, the entry is absent (the box is 16 bytes).

**Visual sample entry body** (78 bytes after the entry header; `VSE`):
```
u8[6] 0, be16(1) /*data_ref_index*/, be16(0), be16(0),
MP4: be32(0) be32(0) be32(0)   |   MOV: "OBSS" be32(0x200) be32(0x200),
be16((u16)width) be16((u16)height), be32(0x00480000) be32(0x00480000), be32(0), be16(1) /*frame_count*/,
compressorname[32], be16(0x0018), be16(0xFFFF)
```
`compressorname`: 32 zero bytes; if the encoder id is not NULL: byte 0 = `min(strlen(id), 31)`, followed by that many id bytes.

**avc1** = `box("avc1", VSE avcC colr pasp)`; **hvc1** = `box("hvc1", VSE hvcC colr clli mdcv pasp)`; **av01** = `box("av01", VSE av1C colr clli mdcv pasp)`; **ProRes** = `box(tag, VSE colr clli mdcv pasp)`. `clli` and `mdcv` exist only for PQ/HLG. `avcC`, `hvcC` and `av1C` are *omitted* when the encoder has no extra data (the entry shrinks). When extra data exist but
the conversion of section 3.3 fails (empty result), the box is present with an empty payload (size 8).
`avcC/hvcC/av1C = box(T, converted_extra_data)` (size `8 + len`).

**colr** (19): `box("colr", "nclx" be16(pri) be16(trc) be16(spc) u8(full_range << 7))`; the triples by colour space: `601 -> 6,6,6`; `DEFAULT, 709 -> 1,1,1`; `SRGB -> 1,13,1`; `PQ -> 9,16,9`; `HLG -> 9,18,9`; `full_range = 1` iff `range == VIDEO_RANGE_FULL`.
**pasp** (16): `box("pasp", be32(1) be32(1))`.
**clli** (12): `box("clli", be16((u16)peak) be16((u16)peak))` with `peak = (float)obs_get_video_hdr_nominal_peak_level()`; `(u16)peak` of a float is `((u32)peak) & 0xFFFF` for 0 <= peak < 2^32.
**mdcv** (32): `box("mdcv", be16(13250) be16(34500) be16(7500) be16(3000) be16(34000) be16(16000) be16(15635) be16(16450) be32(max_lum) be32(0))` with `max_lum = (u32)peak * 10000` (the float is converted to u32 first, then multiplied in 32-bit).

**Audio sample entry body** `ASE(version)` (28 bytes for v0/v1 body, more for MOV): 
```
u8[6] 0, be16(1), be16(version) /*entry_version, equal to the stsd version for MP4*/, be16(0), be16(0), be16(0),
if MOV and version == 2:  be16(3) be16(16) be16(0xFFFE) be16(0) be32(0x00010000) be32(72)
                          be64(bits of double(timescale)) be32(channels) be32(0x7F000000)
                          be32(pcm ? sample_size / channels * 8 : 0) be32(lpcm_flags)
                          be32(pcm ? sample_size : 0) be32(pcm ? 1 : 0)
else:                     be16(channels) be16(samplesize) be16(pre_defined) be16(0) be32(rate16 << 16)
                          [MOV and version == 1: be32(pcm ? 1 : frame_size) be32(pcm ? sample_size/channels : 0) be32(pcm ? sample_size : 0) be32(2)]
```
`channels` = `audio_output_get_channels`; `rate16` = `timescale`, or 0 if `timescale > 65535` (warning R77); `samplesize` = 24 for ALAC in MP4 else 16; `pre_defined` = 0xFFFE for MOV non-PCM else 0; `(rate16 << 16)` is a u32 shift. `lpcm_flags` = Float|Packed (`1|8`) for f32; SignedInteger|Packed (`4|8`) for s16/s24. `pcm` = codec is one of the three PCM codecs.

**mp4a** (MP4) = `box("mp4a", ASE(V) esds [chnl if V == 1])`. **Opus** = `box("Opus", ASE(V) dOps [chnl if V==1])`; **fLaC** = `box("fLaC", ASE(V) dfLa [chnl if V==1])`; **alac** (MP4) = `box("alac", ASE(V) cookie [chnl if V==1])` (cookie = extra data bytes, no wrapper; if the encoder has no extra data the whole entry is omitted); **ipcm / fpcm** = `box(codec==f32 ? "fpcm" : "ipcm", ASE(V) chnl pcmC)` (chnl always, also for V=0).

**esds** (R73): see section 5.7. **dOps** = `box("dOps", u8(0) u8(ch) be16(preskip) be32(rate) be16(gain) u8(map) [table])` copied from OpusHead (R74). **dfLa** = `fullbox("dfLa",0,0, u8(0x80) u24(len) extra)` (R75). **pcmC** = `fullbox("pcmC",0,0, u8(1) u8(bits))` with bits 16/24/32 (size 14).

**chnl** (R76): `fullbox("chnl", 0, 0, u8(1) ...)` then either `u8(defined_layout) be64(0)` (size 8+4+1+1+8 = 22) or `u8(0) speaker_positions[n]` (size 18 + n).

**MOV audio** (R79): entry type `"lpcm"` (PCM), `"mp4a"` (AAC) or `"alac"`; `box(tag, ASE(V) [wave if V==1] [chan])`, `V = 2` for PCM or `channels > 2` or `audio_output_get_sample_rate > 65535`, else 1. **wave** = `box("wave", box("frma", tag) [esds (AAC) | cookie bytes (ALAC)] be32(8) be32(0))`. **chan** = `fullbox("chan",0,0, be32(layout_tag) be32(bitmap) be32(0))` (size 24).

**stts / stss / ctts / stsc / stsz / stco / co64** (R53..R58). **sgpd / sbgp** (R59). **udta** (R100, R101), **tref** `box("tref", box("chap", be32(chapter_track_id)))` (`tref` 20, `chap` 12; R93).

**Fragment boxes** (R40..R45): `mfhd` = `fullbox("mfhd",0,0,be32(seq))` (16); `tfhd`, `tfdt` (20), `trun`, `mvex` = `box("mvex", trex*)`, `trex` = `fullbox("trex",0,0, be32(track_id) be32(1) be32(0) be32(0) be32(0))` (32).

Chapter sample entry (`text` in the stsd of the chapter track): `fullbox("text", 0, 0, be32(1), STUB)` where `STUB` is the fixed 43-byte string

```
00000001 0000 00000000 00000000 00000000 00000000 0001 0000 00000000     (30 bytes: flags, justification, colours, box and style records)
0000000d "ftab" 0001 0001 00                                              (13 bytes: font table)
```
(TextSampleEntry, BoxRecord, StyleRecord and a `ftab` font table, copied from FFmpeg.) Total size 12 + 4 + 43 = 59. Chapter sample payload: `be16(len) name[len] 0000000c "encd" 00000100`.

## 5. Behaviour (rules)

### 5.1 Track construction (`mp4_mux_create`)

**R10** `get_codec(codec_string)`: exact `strcmp` matches: `h264 -> H264`, `hevc -> HEVC`, `av1 -> AV1`, `prores -> PRORES`, `aac -> AAC`, `opus -> OPUS`, `flac -> FLAC`, `alac -> ALAC`, `pcm_s16le -> PCM_I16`, `pcm_s24le -> PCM_I24`, `pcm_f32le -> PCM_F32`, anything else `UNKNOWN`. (A NULL codec string crashes in the reference, D-2.)
**R11** Track creation order: video slots `0 .. MAX_OUTPUT_VIDEO_ENCODERS-1` that are non-NULL, in slot order, then audio slots `0..5` that are non-NULL. Each gets `track_id = ++counter` starting at 1. The muxer holds one encoder reference per track for its lifetime (released by `destroy`).
**R12** Video track: `timebase_num = fps_den`, `timebase_den = fps_num`, `timescale = fps_num`. (`timebase_num` of tracks and of packets is never used again.) Audio track: `sample_rate = encoder sample rate`, forced to 48000 for Opus; `timebase = 1/sample_rate`, `timescale = sample_rate`. Track type is VIDEO iff the encoder type is video, else AUDIO.
**R13** Fixed sample size (audio only): `PCM_F32: channels*4`, `PCM_I24: channels*3`, `PCM_I16: channels*2`, where `channels` = channel count of the speaker layout of the audio output (mono 1, stereo 2, 2.1 3, 4.0 4, 4.1 5, 5.1 6, 7.1 8, unknown 0); 0 for all other codecs or when the audio output is NULL. A PCM track on an `unknown` layout therefore has fixed size 0 and is treated as a variable-size track (Q-12).
**R14** `creation_time = time(NULL) + 0x7C25B080` (seconds since 1904). For MOV, if that exceeds `0xFFFFFFFF`, warning `Creation time too large for MOV, setting to 0 (unset).` and `creation_time = 0`.
**R15** Every track starts with `samples = duration = 0`, `first_pts = 0`, `last_pts_usec = 0`, empty packet queue and sample tables, `needs_ctts = false`, `dts_offset = 0`. `mux` state: `fragments_written = 0`, `next_frag_pts = 0`, no chapter track, `placeholder_offset` unset.

### 5.2 Packet intake (`mp4_mux_submit_packet`)

**R16** The track is the one whose encoder is `pkt->encoder`. If none: warning `Could not find track for packet of type <video|audio> with track id <pkt->track_idx>!` and return `false`; no state changes.
**R17** `fragment_ready = next_frag_pts > 0 AND for EVERY A/V track t: t.last_pts_usec >= next_frag_pts` (the chapter track is not among them; a track that never received a packet has `last_pts_usec = 0` and blocks the split forever). It is evaluated before the new packet is inserted. If true, one fragment is flushed (5.4) first.
**R18** Stored packet: audio -> the packet itself (reference taken). Video -> H264: `obs_parse_avc_packet`; HEVC: `obs_parse_hevc_packet`; AV1: R80; ProRes: the packet itself. (A video track with an UNKNOWN codec leaves the stored packet uninitialised: D-7; callers never do this.) For video the stored copy's `keyframe` flag (possibly raised by the parser, never lowered) and `pts/dts/size` are what the rest of the muxer sees.
**R19** If the stored VIDEO packet has `keyframe && pts > 0`: `next_frag_pts = usec(packet)` (this overwrites a previous value that was not yet flushed). Audio packets never set it. Finally `track.last_pts_usec = max(last_pts_usec, usec(packet))` and the packet is appended to the track's queue; return `true`.

### 5.3 ftyp, free, header decisions

**R20** `ftyp` contents per 4.2. `fragmented` is true when written at the first flush, false when rewritten at finalisation; the *total size is identical*. The `avc1` brand is added iff the **first video track** (track order) has codec H264. Major brand (MP4) is `isom`, or with `MP4_USE_NEGATIVE_CTS`: `iso6` (fragmented) / `iso4` (final).
**R21** The `ftyp` is the first thing written at the first flush, followed by the 16-byte free/wide box; `placeholder_offset` = position of that box (= `size(ftyp)`).

### 5.4 Flush (`mp4_flush_fragment`) -- the heart of the muxer

```
flush():
  if fragments_written == 0:
      write ftyp(fragmented=true); placeholder_offset = pos; write free/wide
  scratch = new append-only buffer
  if fragments_written == 0:  write moov(fragmented=true) into scratch; copy scratch to the file; clear scratch   -- R50
  fragments_written += 1
  mdat_size = 8
  for each A/V track t in track order: process_packets(t, &mdat_size)                                           -- R30
  if next_frag_pts == 0 and chapter track exists:                                                               -- R92
      dur_ms = max over A/V tracks of mul_div(t.duration, 1000, t.timebase_den)
      create chapter packet (dts_usec = dur_ms * 1000, name "Dummy") and push it on the chapter queue
      process_packets(chapter, &mdat_size)
  moof_start = file position
  size = len(moof(moof_size = 0, moof_start))            -- written once to measure it; trun data offsets are garbage in this pass
  clear scratch; write moof(moof_size = size, moof_start) into scratch; copy scratch to the file
  write mdat header: if mdat_size > 0xFFFFFFFF: be32(1) "mdat" be64(mdat_size + 8) else be32(mdat_size) "mdat"
  for each A/V track t: write_packets(t)                                                                       -- R34
  if next_frag_pts == 0 and chapter track exists: write_packets(chapter)
  next_frag_pts = 0
```
**R30** `process_packets(t)`:
```
n = number of queued packets; if n == 0: return
final = (next_frag_pts == 0)
last = final ? n : n - 1              -- unless final, the last queued packet is held back
for i in 0 .. last-1:
    p = queue[i]
    if !final and usec(p) >= next_frag_pts: break
    if i < n-1:  duration = (u32)(queue[i+1].dts - p.dts)           -- distance to the next packet, u32 wrap-around
    else:                                                              -- i == n-1, only when final
         if t.fragment_samples is empty: break                        -- Q-5: lone last packet is dropped
         duration = t.fragment_samples.last.duration
    size = (u32)p.size;  offset = (i32)(p.pts - p.dts)
    if t is VIDEO and NEGATIVE_CTS flag:
         if t.offsets is empty: t.dts_offset = offset              -- first sample ever of the track
         offset = offset - t.dts_offset                            -- i32 wrap-around arithmetic
    t.fragment_samples.push({size, offset, duration}); mdat_size += size
    t.duration += duration
    count = 1
    if t.sample_size != 0: count = size / t.sample_size; duration = 1     -- PCM: durations are in frames
    if t.samples == 0: t.first_pts = p.pts
    t.samples += count
    if t.deltas empty or t.deltas.last.delta != duration:  push {count, delta = duration}  else t.deltas.last.count += count
    if t.sample_size == 0: t.sample_sizes.push(size)
    if t is not VIDEO: continue
    if p.keyframe: t.sync_samples.push(t.samples)                  -- 1-based sample number
    if offset != 0: t.needs_ctts = true
    if t.offsets empty or t.offsets.last.offset != offset: push {count = 1, offset}  else t.offsets.last.count += 1
```
The tables use 32-bit counters/deltas (`u32` fields); `t.duration` is u64 and sums the *original* duration (not the PCM `1`). Note `track->samples` is only incremented for packets that are processed; `deltas`, `offsets`, `sync_samples` and `sample_sizes` therefore describe exactly the written samples.
**R31** The queue is a FIFO. Packets whose index is `>= last` or that stop the loop stay in the queue for later flushes.
**R32** `fragment_samples` of every track is the list of samples of the *current* fragment: entries are `{size, offset (ct-dt, possibly negcts-adjusted), duration (in track units, before the PCM override)}`.
**R33** For tracks with `fragment_samples` empty the `moof` has no `traf`; the track may still have queued packets.
**R34** `write_packets(t)`: if the queue is empty or `fragment_samples` is empty: nothing. Else add chunk `{offset = file position, samples = fragment_samples.len}`; pop `fragment_samples.len` packets from the front of the queue and write their payloads back to back; `chunk.size = bytes written`; if `t.sample_size != 0`: `chunk.samples = chunk.size / t.sample_size`; clear `fragment_samples`. Every flush therefore adds at most one chunk per track, and the chunk offset is an absolute file position.

**R35** The split decision uses microseconds, durations/tables use track ticks. For video, `timescale == timebase_den` so `mul_div(x, timescale, timebase_den) == x` everywhere in the file; the same holds for audio and the chapter track. An implementation MAY drop these conversions but MUST keep `mul_div` for the millisecond values (R51, R52, R92).

### 5.5 Fragment boxes

**R40** `moof = box("moof", mfhd traf*)`, one `traf` per track *in track order* that has `fragment_samples`. `mfhd.sequence_number = fragments_written` (after increment: 1 for the first). `samples_mdat_offset` starts at 0 for each moof and is advanced by the sample bytes of each `trun`.
**R41** `traf = box("traf", tfhd tfdt trun)`.
**R42** `tfhd = fullbox("tfhd", 0, flags, be32(track_id) be64(moof_start) [be32(def_dur)] [be32(def_size)] be32(def_flags))`. flags = `0x01` (base-data-offset) | `0x20` (default flags) | `0x08` if `durations_match` | `0x10` if `sizes_match`. Selection:
```
if t.sample_size != 0:   duration = 1; size = t.sample_size; both match = true
else:  duration = fs[0].duration; size = fs[0].size; durations_match = sizes_match = true
       for idx in 1 .. len-1:   durations_match = (fs[idx].duration == duration); sizes_match = (fs[idx].size == size)   -- Q-1: each iteration overwrites
if durations_match and VIDEO: duration = (u32)mul_div(duration, timescale, timebase_den)
def_flags = VIDEO ? 0x01010000 : 0x02000000
```
`base_data_offset = moof_start` (absolute position of the moof box).
**R43** `tfdt = fullbox("tfdt", 1, 0, be64(base))` (size 20), `base = t.duration - sum(fragment_samples[*].duration)` (for VIDEO converted by `mul_div(base, timescale, timebase_den)`; the sum uses the original durations, even for PCM).
**R44** `trun = fullbox("trun", v, flags, be32(count) be32(data_offset) [be32(0x02000000) if VIDEO] per-sample...)`; `v = 1` iff NEGATIVE_CTS else 0. flags: `0x001` always; `0x200` if `t.sample_size == 0`; VIDEO adds `0x004` (first_sample_flags) and `0x800` (composition offsets). `data_offset = moof_size + 8 + samples_mdat_offset` (u32), where `moof_size` is the total size of the moof and `8` is the mdat header (a 16-byte mdat header, only above 4 GiB, makes this wrong: Q-13). `count = len(fragment_samples)`; for fixed-size tracks `count = total_bytes / sample_size` and `samples_mdat_offset += total_bytes` and **no per-sample entries** follow. Otherwise per sample: `be32(size)` then for VIDEO `be32((u32)(offset * timescale / timebase_den))` (computed as signed 64-bit: `(i64)offset * timescale / timebase_den`, truncating toward zero; identity for video). No sample durations are ever written to a `trun` (Q-1).
**R45** The `mdat` header of the fragment is written after the moof (R40..R44); its size field is `8 + sum of all processed sample sizes of this fragment, chapters included on the final flush`. Chapter samples (R92) are in the final mdat but have no traf.
**R46** `trex`/`mvex` (only in the initial moov): one `trex` per A/V track (chapter track excluded) with `track_ID = index + 1`.

### 5.6 Non-fragmented (final) and initial moov

**R50** The initial `moov` (written at the first flush, before any packet is processed) = `box("moov", mvhd trak* mvex udta)` with `fragmented = true`: every A/V track gets a `trak` (also those without packets); stts/stsc/stco have `entry_count 0` (16 bytes each), stsz = `fullbox("stsz",0,0,be32(0) be32(0))` (20 bytes), no stss/ctts/sgpd/sbgp; `duration` fields are 0 because `t.duration == 0`. The chapter track is NOT written even if it exists, but the `tref` is (R93). The elst delay comes from the queue (R52).
**R51** `mvhd.duration` (ms) = `mul_div(t.duration, 1000, t.timebase_den)` of the FIRST video track (track order), 0 if there is no video track (Q-2). `next_track_ID = track_counter + 1` (counts the chapter track if it exists). `version = 1` iff `duration > 0xFFFFFFFF` or `creation_time > 0xFFFFFFFF`. `tkhd.duration = mul_div(t.duration, 1000, t.timebase_den)` per track, same version rule. `mdhd.duration`: VIDEO `mul_div(t.duration, timescale, timebase_den)`, other `t.duration`; version 1 iff that exceeds 0xFFFFFFFF or `creation_time > 0xFFFFFFFF` (MOV additionally warns `Duration too large for MOV, this file may be unplayable in QuickTime!`). All other timestamps in v1 are be64.
**R52** Edit list (every trak): `entry_count 1`; `segment_duration = (u32) mul_div(t.duration, 1000, t.timebase_den)` ms; `media_time` per type:
- VIDEO and not NEGATIVE_CTS: `dts_offset` = offset of the FIRST ctts run if `t.offsets` is non-empty (that is `p0.pts - p0.dts` of the first processed sample), else if the queue is non-empty `queue[0].pts - queue[0].dts`, else 0; `media_time = (u32) mul_div(dts_offset, timescale, timebase_den)` (negative offsets wrap through the unsigned cast); `media_rate = 0x00010000`.
- AUDIO with `first_pts < 0`: `delay = mul_div(|first_pts|, timescale, timebase_den)`; `media_time = delay`; `segment_duration = (u32)(mul_div(t.duration,1000,t.timebase_den) - mul_div(delay, 1000, timescale))` (u64 subtraction then cast).
- everything else (incl. VIDEO with NEGATIVE_CTS, chapters): `media_time = 0`.
`first_pts` is the pts of the first processed sample of the track, so it is 0 in the initial moov (Q-6).
**R53** `stts`: one entry per delta run `{count, (u32) mul_div(delta, timescale, timebase_den)}`; `fragmented` form is entry_count 0.
**R54** `stss` (final moov, VIDEO and not ProRes only): omitted if there are no sync samples; else `entry_count` + the sample numbers (u32). `ctts` (final only): if `needs_ctts`: version 1 iff NEGATIVE_CTS else 0, `entry_count = len(offsets)`, entries `{count, (u32)((i64)offset * timescale / timebase_den)}`.
**R55** `stsc` (final): runs over chunks: a new entry `{first_chunk = idx + 1, samples_per_chunk, description 1}` whenever a chunk's `samples` differs from the previous chunk's (the first chunk always starts an entry).
**R56** `stsz` (final): fixed size: `be32(sample_size) be32((u32)samples)`, no table; else `be32(0) be32(len(sample_sizes))` and the table. A warning `Track <id> has too many samples, its duration may not be read correctly. Remuxing the file to another format such as MKV may be required.` is logged first when `samples > 0xFFFFFFFF` (not testable).
**R57** `stco`/`co64` (final): `co64` (8-byte offsets) iff the LAST chunk's offset exceeds `0xFFFFFFFF`, else `stco`; `entry_count = len(chunks)`. (A track without chunks never reaches here: R58.)
**R58** The final moov contains a `trak` only for tracks that have at least one chunk (so a track whose samples were never written is absent, and a file without samples has just `mvhd`+`udta`). Tracks keep their original `track_id`s. Chapter track last, after all A/V tracks, also only if it has chunks.
**R59** `sgpd`+`sbgp` (final only): AAC: `fullbox("sgpd",1,0, "roll" be32(2) be32(1) be16(0xFFFF))` (26 bytes) then `fullbox("sbgp",0,0, "roll" be32(1) be32((u32)samples) be32(1))` (28 bytes). Opus: `sgpd` as AAC but `roll_distance = -preroll_count`, then `sbgp` with two entries `{preroll_count, 0}`, `{(u32)samples - preroll_count, 1}` (size 36); `preroll_count` = number of leading samples needed until the sum of their deltas reaches 3840 (walk the delta runs sample by sample: `remaining = 3840; for each run, for j in 0..count: if remaining <= 0 stop; remaining -= run.delta; n += 1`), stored in a u16, so a track with fewer samples than needed gives `preroll_count = samples`.
**R60** The final moov: `box("moov", mvhd trak* [chapter trak] udta)` with `fragmented = false`; no `mvex`. It is appended at `data_end`. Then (R61) the head of the file is rewritten.
**R61** `finalise`:
```
next_frag_pts = 0; flush();                     -- the final fragment (may contain no samples at all)
log info "Flushing final fragment..." (before) / "Number of fragments: <n>" (after the flush)
if SKIP_FINALISATION: log warning "Skipping finalization!"; return true      -- nothing else is written
data_end = file position
write final moov at data_end (built in a scratch buffer); log info "Full moov size: <bytes/1024> KiB"
seek 0; write ftyp(fragmented=false)
data_size = data_end - placeholder_offset; seek placeholder_offset
write mdat header (8 bytes, or 16 if data_size > 0xFFFFFFFF); log info "Final mdat size: <data_size/1024> KiB"; return true
```
(`Flushing final fragment...` is logged before the flush.) After `finalise` the serializer position is `placeholder_offset + 8` (or `+16`).

### 5.7 Codec-specific boxes

**R70** `stsd` version `V`: MP4 flavor and AUDIO track: `1` iff `audio_output_get_channels(audio) > 2`, else 0; everything else (video, chapters, all MOV) 0. The same `V` is the `entry_version` of audio sample entries in MP4.
**R71** Entry selection (MP4): VIDEO by codec (H264 avc1, HEVC hvc1, AV1 av01, PRORES tag); AUDIO AAC mp4a, OPUS Opus, FLAC fLaC, ALAC alac, PCM ipcm/fpcm; chapter: `text` (4.2). Unknown codec: no entry. MOV audio: R79; MOV video is identical to MP4 except for the visual sample entry fields (4.2). 
**R72** ProRes entry type: `tag = (u32) obs_data_get_int(settings, "codec_type")` (0 if settings NULL or key absent); the four type bytes are the big-endian bytes of `tag` (e.g. `0x61707368` -> `apch`).
**R73** `esds = fullbox("esds", 0, 0, ES_Descriptor)`; `descr(tag, n) = u8(tag) u8((n>>21)|0x80) u8((n>>14)|0x80) u8((n>>7)|0x80) u8(n & 0x7F)` (always four length bytes; the shifts are `(n >> 7*i) | 0x80` truncated to u8). With `E` = extra data length (0 if absent), `D = E ? E + 5 : 0`:
```
descr(0x03, 3 + 5 + 13 + D + 5 + 1)  be16(track_id) u8(0)
descr(0x04, 13 + D)  u8(0x40) u8(0x15) u24(0) be32(bitrate) be32(bitrate)
if E: descr(0x05, E) extra
descr(0x06, 1) u8(0x02)
```
`bitrate = (u32)(settings["bitrate"] * 1000)` (int64 arithmetic then cast), 0 if no settings or key is 0/absent.
**R74** `dOps` (Opus): if the encoder has no extra data the `dOps` box is entirely absent. Given OpusHead `x` (`"OpusHead"` at offset 0, version byte at 8): `u8(0)`, `channels = x[9]`, `be16(le16(x[10..12]))` pre-skip, `be32(le32(x[12..16]))` rate, `be16(le16(x[16..18]))` gain, `map = x[18]`; if `map != 0` append `x[19 .. 19 + 2 + channels]`. Extra data shorter than 19 bytes (or `map != 0` and shorter than `21 + channels`) is read out of bounds (D-4); a conforming implementation SHOULD treat missing bytes as 0 and not fail.
**R75** `dfLa`: absent if no extra data; else `fullbox("dfLa",0,0, u8(0x80) u24(len(extra)) extra)`.
**R76** `chnl` (MP4 only, V == 1 or PCM): `speakers` from the audio output. `(positions, iso_layout)`: mono `[2],1`; stereo and unknown `[0,1],2`; 2.1 `[0,1,3],0`; 4.0 `[0,1,2,10],4`; 4.1 `[0,1,2,3,10],0`; 5.1 `[0,1,2,3,8,9],0`; 7.1 `[0,1,2,3,8,9,13,14],12`. If `iso_layout == 0`: warning `No ISO layout available for speaker layout <enum value>, this may not be supported by all applications!` (enum: unknown 0, mono 1, stereo 2, 2.1 3, 4.0 4, 4.1 5, 5.1 6, 7.1 8), then `u8(0)` + positions; else `u8(iso_layout)` + `be64(0)`.
**R77** If `timescale > 65535` in an audio entry (not MOV v2): warning `Sample rate too high for MP4, file may not play back correctly.` and the 16.16 rate field is 0.
**R78** `mp4_write_audio_sample_entry` details per 4.2; `channels` for the v2 entry is the channel count (a PCM track on `unknown` speakers makes the v2 entry divide by zero: D-3).
**R79** MOV audio (`mp4_write_mov_audio_tag`): `V = (channels > 2 || audio_output_get_sample_rate > 65535) ? 2 : 1`; PCM codecs force `V = 2` and tag `lpcm`; AAC tag `mp4a`; ALAC tag `alac`; others (Opus, FLAC, unknown): nothing is written (empty stsd). `wave` only when `V == 1` (esds for AAC; cookie for ALAC; if ALAC has no extra data only frma+terminator). `chan`: `layout = mono: 0x00640001; stereo: 0x00650002; 2.1: AAC -> 0x00850003, others -> 0x00010000 (use bitmap); all other layouts: 0x00010000`; `bitmap` is only non-zero for layout `0x00010000`: mono `FC`, stereo `FL|FR`, 2.1 `FL|FR|LFE`, 4.0 `FL|FR|FC|RC`, 4.1 `FL|FR|FC|LFE|RC`, 5.1 `FL|FR|FC|LFE|RL|RR`, 7.1 `FL|FR|FC|LFE|RL|RR|SL|SR`, unknown 0 with bit values `FL=1 FR=2 FC=4 LFE=8 RL=0x10 RR=0x20 RC=0x100 SL=0x200 SR=0x400`. If layout is the bitmap tag and the bitmap is 0 (unknown): warning `No valid speaker layout found, not writing chan box. File may not play back correctly!` and no `chan`. The audio sample entry in MOV uses `rate16 = timescale` (track timescale, not the output's rate).
**R80..R82 AV1 packet transform** (see 5.8).

### 5.8 AV1 packets (`rtmp-av1.c`)

Let `parse_obu(buf, avail)` read at most the first `min(avail, 10)` bytes: header byte `b0`; if `b0 & 0x80` (forbidden bit) fail; `type = (b0 >> 3) & 15`; `ext = (b0 >> 2) & 1`; `has_size = (b0 >> 1) & 1`; `start = 1 + ext`; if `has_size`: `size = leb128` of up to 8 bytes starting at `start` (little-endian 7-bit groups, stops at first byte with bit 7 clear), `start += bytes_consumed`; else `size = avail - 1 - ext`. Fail if the bits needed run past the bytes read (header or leb128 truncated), or `start + size > avail`. Total OBU length `len = start + size`.
**R80** `out = *src` (all fields); the output data = for each OBU of `src` in order: types 2 (temporal delimiter), 7 (redundant frame header) and 8 (tile list) are dropped; types 3 (frame header) and 6 (frame) are kept and additionally run R81; every other type is kept. Kept OBUs are copied *verbatim* (header, size field and payload as received). A failing `parse_obu` ends the walk: the output holds what was copied so far.
**R81** `priority/keyframe` for types 3/6: only if `out.priority == 0` at that moment: look at the first payload byte `v` (the byte at `start`) -- if bit 7 (show_existing_frame) is set: nothing; else `frame_type = (v >> 5) & 3`, `show_frame = (v >> 4) & 1`: `KEY (0)`: `out.keyframe = true`, `priority = 3`; `INTER (1)`: `priority = show_frame ? 1 : 2`; `INTRA_ONLY (2)`: 3; `SWITCH (3)`: 2. (This presumes `reduced_still_picture_header = 0`.) The keyframe flag is never lowered (like S-012-R22). An OBU with `size == 0` reads the byte after the OBU (D-8).
**R82** `out.drop_priority = out.priority`. The data is a fresh block with reference count 1 (S-012 3.2); an empty result has size 0 and non-NULL data.

### 5.9 Chapters

**R90** `add_chapter(dts_usec, name)`: `dts_usec < 0` -> `false`, nothing created. Else create the chapter track if absent: `type CHAPTERS, codec TEXT, timescale 1000, timebase 1/1000, track_id = ++counter` (the next free id, i.e. count of A/V tracks + 1; no encoder). If `dts_usec > 0` and the chapter queue is empty: first add a chapter at 0 with the localised `MP4Output.StartChapter` text. Then create the chapter packet and append it (the queue is NOT sorted; it only updates `last_pts_usec`). Return `true`.
**R91** Chapter packet: `dts = pts = dts_usec / 1000` (truncating division), `timebase = 1/1000`, data `be16(len) name[len] be32(12) "encd" be32(0x100)`, `len = min(strlen(name), 65535)` (the name bytes are copied up to `len`; an empty name gives `0000`).
**R92** On the final flush only (`next_frag_pts == 0`), after the A/V tracks have been processed: push a `Dummy` chapter packet (R91 with `dts_usec = dur_ms * 1000`, i.e. `dts = dur_ms`), where `dur_ms` is the longest A/V track duration in milliseconds (`mul_div(duration, 1000, timebase_den)`; 0 if nothing was processed), then run R30 on the chapter queue. Chapter durations follow R30: the delta to the next packet *in queue order* (u32 wrap-around if chapters were not added in ascending order, Q-8); the last packet (`Dummy`) takes the previous chapter's duration. The chapter queue is never processed earlier. Chapter samples are written after all A/V samples of the final fragment (R34), are counted in that fragment's mdat size, and are listed in the final moov only.
**R93** In the final moov every A/V `trak` has `tref` (between `edts` and `mdia`) = `box("tref", box("chap", be32(chapter_track_id)))` if the chapter track exists *at the time the moov is written* -- this includes the initial moov (R50) when the chapter was added before the first flush, in which case it refers to a track that is not in that moov (Q-9). The chapter trak itself has no tref.
**R94** The chapter `trak`: `tkhd` as for video (flags 3, alt group 0, volume 0, width/height 0); `elst` `media_time 0`, `segment_duration = duration` ms; `mdhd` timescale 1000; `hdlr` `text`; `minf` = `gmhd` + (MOV: dinf-hdlr) + dinf + stbl; stsd = `text` (4.2); no stss; `udta` is an EMPTY box of size 8 (no encoder name).

### 5.10 Metadata atoms

**R100** Track `udta = box("udta", [box("name", name) if name != NULL] [ENC_INFO...])`. `name` atoms contain the raw bytes without NUL. With `MP4_WRITE_ENCODER_INFO`: `if name != NULL: box("\xA9enc", id)` (the reference tests `name` where `id` is meant; id NULL with name set crashes: D-1); then if the settings object is non-NULL: `box("json", obs_data_get_json_with_defaults(settings))` (S-014; an empty settings object gives `{}`).
**R101** Movie `udta`: MP4 flavor, or MOV with `MP4_USE_MDTA_KEY_VALUE`: `box("udta", meta)`; `meta = fullbox("meta", 0, 0, hdlr (ilst | keys ilst))`. Without the MDTA flag: `hdlr = fullbox("hdlr",0,0, be32(0) "mdir" "appl" be32(0) be32(0) u8(0))` (33 bytes) and `ilst = box("ilst", box("\xA9too", data))` with `data = box("data", be32(1) be32(0) "OBS Studio (<version>)")`. With the MDTA flag: `hdlr = fullbox("hdlr",0,0, be32(0) "mdta" be32(0) be32(0) be32(0) u8(0))` (33 bytes), `keys = fullbox("keys", 0, 0, be32(1) box("mdta", "tool"))` (size 28), `ilst = box("ilst", item)` with `item = be32(8 + len(data)) be32(1) data` (the key index 1 stands where an atom type would be; `data` is the complete data atom of the previous sentence).
MOV without MDTA: `box("udta", box("\xA9swr", be16(len) 55 C4 text))` where `text = "OBS Studio (<version>)"` and the two bytes `0x55 0xC4` are the language.
**R102** The version text is whatever `obs_get_version_string()` returns, inserted verbatim.

### 5.11 Lifecycle

**R110** `destroy`: releases every track (its encoder reference, queued packets, tables), the chapter track and the mux. Nothing is written. The file is left as it was (possibly empty if no flush happened).
**R111** Logging: all messages are prefixed `[<mp4|mov> muxer: '<output name>'] ` (`mov` iff flavor MOV, else `mp4`, also for CMAF). Levels: the `Flushing final fragment...`, `Number of fragments`, `Full moov size`, `Final mdat size` lines are INFO; every other message in this spec is WARNING.

## 6. Algorithms

### 6.1 Building the file in memory

Because the moov and moof sizes depend on content, an implementation builds boxes into a `Vec<u8>` and patches the size, as the C code does with seek-and-patch. Two mechanisms matter for observable behaviour: the *initial moov* and each `moof` are written in one call to the real sink (R50, 5.4); everything else (ftyp, free, mdat header, sample payloads) goes straight to the sink. A recorder for "bytes handed to the sink" (the vectors print the sink position) therefore observes: [ftyp, free] [initial moov] [moof] [mdat header] [packets...] and the moov at finalisation as ONE write.

### 6.2 Fragment sizes: moof written twice

The `moof` is generated twice: once with `moof_size = 0` to learn its size, then with the real size (only `trun.data_offset` differs). `tfhd.base_data_offset` is the absolute file position of the moof in both passes.

### 6.3 Why the sink position matters

All offsets (`base_data_offset`, chunk offsets, `placeholder_offset`, `data_end`) are file positions obtained from the sink. If the sink is a file opened at position 0 these are plain byte counts from the start of the file.

## 7. Concurrency

None: one thread owns the muxer. Packet data blocks use an atomic reference count (S-012 3.2).

## 8. Reference defects and quirks

| ID | Kind | Reference behaviour | Required | Script |
|---|---|---|---|---|
| Q-1 | QUIRK | `tfhd`: the "all durations/sizes equal" loop keeps only the last comparison, and `trun` never carries durations; fragments with varying durations get a wrong default duration (or none) | match (SHOULD); the final moov is unaffected | `27` |
| Q-2 | QUIRK | `mvhd.duration` is taken from the first video track; audio-only files have `mvhd.duration = 0` while `tkhd/mdhd` carry real durations | match | `07`, `22` |
| Q-3 | QUIRK | `mdhd` version 1: the 64-bit duration is written from a `(u32)` cast | match | `24` |
| Q-4 | QUIRK | Packet `timebase_num` is ignored everywhere; microseconds are `pts * 1e6 / timebase_den` | match | `21` |
| Q-5 | QUIRK | A lone last packet in the final fragment is dropped (no duration source); empty/1-packet tracks are omitted from the final moov, a file without samples has no `trak` | match | `22`, `29` |
| Q-6 | QUIRK | The initial moov's audio edit list never has the priming delay; its video edit list uses the first QUEUED packet | match | `07`, `05` |
| Q-7 | QUIRK | `MP4_USE_NEGATIVE_CTS` ftyp: `iso6` fragmented -> `iso4` final | match | `02`, `06` |
| Q-8 | QUIRK | Unsorted chapters give u32 wrap-around durations | match | `19` |
| Q-9 | QUIRK | `tref/chap` points at a track that does not exist in the initial moov; `next_track_ID`/trex mismatch | match | `19` |
| Q-10 | QUIRK | AAC esds bitrate `(u32)(kbps*1000)` wraps; no settings -> 0 | match | `12` |
| Q-11 | QUIRK | Track `udta` is written even when empty (8 bytes); `name` used where `id` is meant for `(c)enc` | match | `18`, `19` |
| Q-12 | QUIRK | PCM + `unknown` speakers => fixed sample size 0 => treated as a variable-size track (MP4 flavor; in MOV this becomes D-3) | match | `30` |
| Q-13 | QUIRK | `trun.data_offset` uses an 8-byte mdat header even when the mdat header is 16 bytes (fragment > 4 GiB); co64/large mdat paths | not testable | |
| Q-14 | QUIRK | `FLAVOR_CMAF` is not special-cased: it behaves exactly like MP4 | match | `30` |
| Q-15 | QUIRK | Keyframe flag: parsers only raise it; the first video packet with `pts <= 0` never sets the split point | match | `04`, `23` |
| D-1 | DEFECT | `MP4_WRITE_ENCODER_INFO` with encoder name set and id NULL: `strlen(NULL)` crash | write `box("\xA9enc", "")` (empty) and continue | `25` |
| D-2 | DEFECT | NULL codec string: `strcmp(NULL)` crash at create | treat as UNKNOWN | `25` |
| D-3 | DEFECT | MOV v2 audio entry with 0 channels and PCM: division by zero | write `0` for the two derived fields | `25` |
| D-4 | DEFECT | `dOps` reads OpusHead fields beyond short extra data (heap over-read) | treat missing bytes as 0, no crash | `25` |
| D-5 | DEFECT | audio encoder without audio output: `audio_output_get_channels(NULL)` crash | fail `create`/treat channels as 0 | `25` |
| D-6 | DEFECT | video encoder without video output: NULL `video_output_get_info` dereference in track creation | fail `create` cleanly | `25` |
| D-7 | DEFECT | video track with an UNKNOWN codec: the stored packet is uninitialised stack memory | return `false` from submit (nothing stored) | not vectored (non-deterministic) |
| D-8 | DEFECT | AV1 packets: the bit reader reads 1-2 bytes past the packet end for a truncated OBU header (the result is then rejected), and a frame/frame-header OBU with `size 0` reads the byte after the OBU to decide keyframe/priority (garbage if it is the last OBU). Not detectable with ASan in the harness because `bmalloc` over-allocates | truncated header: parse failure (R80); `size 0`: no keyframe/priority change | `25` (only checks that the call returns) |
| D-9 | DEFECT (UB) | `rl32` shifts a promoted `int` into the sign bit (`ptr[3] << 24` for `ptr[3] >= 0x80`) | compute in u32 | |

## 9. Implementation notes (non-normative)

- Model the muxer as a state machine over `Vec<Track>`; serialise boxes with a small `BoxWriter` that returns `Vec<u8>` (nested helper `fn boxed(ty:[u8;4], body)`); the only non-linear step is finalisation's seek-and-overwrite.
- Sinks: a `trait Sink { write, pos, seek }` with a `Vec`-backed and a file-backed implementation; the vectors' `pos` lines require `pos` to equal the byte count handed over so far.
- The two ftyp bodies have identical length: compute the ftyp once for "fragmented", and re-run with `fragmented=false` into the same length at finalisation (assert the lengths match).
- Use `u64` for durations, wrapping `u32`/`i32` casts (`as`) for the table fields exactly as written; never `try_into`.
- Do not assume `fps_num` divides anything: with 30000/1001 one frame is 1001 ticks and packet pts increase by 1001 (see `21`).
- For the Rust FFI: packet data must keep the C layout of S-012 3.2.

## 10. Conformance coverage

Scripts are `test/conformance/scripts/mp4/NN-*.script`; below only the number is given.

| Rule | Scripts |
|---|---|
| R01, R110 | `26` |
| R10 | `01`, `09`, `10`, `11`, `13`, `14`, `15`, `23` (unknown audio codec) |
| R11 | `20` |
| R12 | `21`, `13` (Opus 48000), `01` |
| R13 | `15`, `30` |
| R14 | `24` |
| R15 | `01`, `22` |
| R16 | `20`, `26` |
| R17 | `03`, `04`, `29` |
| R18 | `09`, `10`, `23` |
| R19 | `03`, `04`, `23` |
| R20, R21 | `02`, `06`, `28` |
| R30 | `03`, `05`, `22`, `29` |
| R31, R32, R33 | `03`, `27`, `29`, `30` |
| R34 | `01`, `03`, `15`, `30` |
| R35 | `21` |
| R40, R41, R43 | `03`, `30` |
| R42 | `03`, `15`, `27`, `30` |
| R44 | `03`, `06`, `15`, `30` |
| R45 | `03`, `19` |
| R46 | `02`, `20` |
| R50 | `01`, `05`, `07`, `19` |
| R51 | `01`, `07`, `22`, `24` |
| R52 | `05`, `06`, `07` |
| R53, R54 | `01`, `05`, `06`, `11` |
| R55, R56, R57 | `03`, `15`, `30` (co64 and the stsz overflow warning are not covered) |
| R58 | `22`, `29` |
| R59 | `01`, `13` |
| R60, R61 | `01`, `02`, `22`, `24` |
| R70 | `01`, `16` |
| R71 | `08`..`17`, `23` |
| R72 | `11` |
| R73 | `12`, `17` |
| R74 | `13`, `25` |
| R75 | `14` |
| R76, R77 | `15`, `16` |
| R78 | `15`, `17`, `30` |
| R79 | `17` |
| R80, R81, R82 | `10`, `25` |
| R90, R91, R92, R93, R94 | `19` |
| R100, R101, R102 | `18`, `25` |
| R111 | every script (log lines) |
| Q-1 | `27` |
| Q-2 | `07`, `22` |
| Q-3, Q-13 | `24` (Q-3); Q-13 not testable |
| Q-4 | `21` |
| Q-5 | `22`, `29` |
| Q-6 | `05`, `07` |
| Q-7 | `02`, `06` |
| Q-8, Q-9, Q-11 | `19` (Q-11 also `18`) |
| Q-10 | `12` |
| Q-12, Q-14 | `30` |
| Q-15 | `04`, `23` |
| D-1..D-6, D-8 | `25` (fork per case) |
| D-7, D-9 | not vectored (non-deterministic / compile-time UB) |

## 11. Open questions

- The HEVC `hvcC` builder (`obs_parse_hevc_header` for Annex-B input) and the AV1 sequence-header parser (`obs_parse_av1_header`) are not specified here; vectors use finished `hvcC`/`av1C` extra data. They need their own spec section.
- 64-bit paths (`co64`, `mdat` > 4 GiB, `stsz` overflow warning) need files above 4 GiB and are not covered by vectors; behaviour is taken from the source.
- Windows: `min()` comes from the CRT, no other difference visible.
- The chapter name length limit (65535) and `stco` -> `co64` switch cannot be exercised with the script token limit and a < 4 GiB test file.
- The runner harness replaces `time()` with a constant; real wall-clock behaviour is only the formula of R14.
