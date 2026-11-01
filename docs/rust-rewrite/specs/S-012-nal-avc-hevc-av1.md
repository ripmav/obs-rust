# S-012 Video bitstream helpers: Annex-B start codes, H.264 (AVC), H.265 (HEVC), AV1

| | |
|---|---|
| Status | Ready for review |
| Reference | libobs 33.0.1 `obs-nal.c/.h`, `obs-avc.c/.h`, `obs-hevc.c/.h`, `obs-av1.c/.h` |
| Target crate | `crates/obs-video-bitstream` |
| Depends on | S-001 (allocation: results are `bmalloc` blocks), S-005 (growable byte arrays, only as an implementation detail) |
| Conformance | `test/conformance/scripts/nal/` (19 scripts, vectors in `vectors/nal/`) |

## 1. Scope

Pure functions over byte buffers that live between video encoders and outputs: find Annex-B start
codes, decide whether an access unit is a keyframe, compute packet priority, convert Annex-B to the
length-prefixed form used by FLV/MP4/RTMP, build an `AVCDecoderConfigurationRecord`, and split
parameter sets / SEI / metadata OBUs from a coded access unit. Callers: encoder plugins
(`obs-x264`, `obs-nvenc`, `obs-qsv11`, `obs-ffmpeg`, ...), `libobs` (`obs-encoder.c`), outputs
(`obs-outputs`, `obs-ffmpeg`). Out of scope: decoding of any syntax element other than the few
named below, all other `encoder_packet` handling.

## 2. Terminology and conventions

RFC 2119 words. `n` = input length. Offsets are 0-based. All bytes are unsigned. Hex strings are
byte sequences. "Start code" = `00 00 01` (3-byte) optionally preceded by one more `00`
(4-byte form). "Header byte" = first byte after the start code's `01`. 64-bit little-endian target.

## 3. Interface

All functions are thread-safe (pure, no shared state). Pointer results are blocks from `bmalloc`
(S-001) which the caller frees with `bfree` unless stated; `NULL` is returned for an empty result
of the `extract` functions (size 0).

```c
const uint8_t *obs_nal_find_startcode(const uint8_t *p, const uint8_t *end);          /* R01-R05 */
const uint8_t *obs_avc_find_startcode(const uint8_t *p, const uint8_t *end);          /* identical */
bool   obs_avc_keyframe (const uint8_t *data, size_t size);
bool   obs_hevc_keyframe(const uint8_t *data, size_t size);
bool   obs_av1_keyframe (const uint8_t *data, size_t size);
void   obs_parse_avc_packet (struct encoder_packet *out, const struct encoder_packet *src);
void   obs_parse_hevc_packet(struct encoder_packet *out, const struct encoder_packet *src);
int    obs_parse_avc_packet_priority (const struct encoder_packet *pkt);
int    obs_parse_hevc_packet_priority(const struct encoder_packet *pkt);
size_t obs_parse_avc_header(uint8_t **header, const uint8_t *data, size_t size);
void   obs_extract_avc_headers (const uint8_t *packet, size_t size, uint8_t **new_data, size_t *new_size,
                                uint8_t **header, size_t *header_size, uint8_t **sei, size_t *sei_size);
void   obs_extract_hevc_headers(... same signature ...);
void   obs_extract_av1_headers (const uint8_t *packet, size_t size, uint8_t **new_data, size_t *new_size,
                                uint8_t **header, size_t *header_size);
void   metadata_obu(const uint8_t *src, size_t len, uint8_t **out, size_t *out_size, uint8_t metadata_type);
void   metadata_obu_itu_t35(const uint8_t *src, size_t len, uint8_t **out, size_t *out_size);
```

Priority constants: `OBS_NAL_PRIORITY_DISPOSABLE=0, LOW=1, HIGH=2, HIGHEST=3`.
AVC NAL types: SLICE=1, DPA=2, DPB=3, DPC=4, SLICE_IDR=5, SEI=6, SPS=7, PPS=8, AUD=9, FILLER=12.
HEVC NAL types: 0..31 VCL (TRAIL_N 0, TRAIL_R 1, TSA_N 2, TSA_R 3, STSA_N 4, STSA_R 5, RADL_N 6,
RADL_R 7, RASL_N 8, RASL_R 9, reserved 10..15, BLA_W_LP 16, BLA_W_RADL 17, BLA_N_LP 18,
IDR_W_RADL 19, IDR_N_LP 20, CRA_NUT 21, RSV_IRAP 22/23, RSV_VCL 24..31), VPS 32, SPS 33, PPS 34,
AUD 35, EOS 36, EOB 37, FD 38, SEI_PREFIX 39, SEI_SUFFIX 40.
AV1 OBU types: SEQUENCE_HEADER 1, TEMPORAL_DELIMITER 2, FRAME_HEADER 3, TILE_GROUP 4, METADATA 5,
FRAME 6, REDUNDANT_FRAME_HEADER 7, TILE_LIST 8, PADDING 15. Metadata types: HDR_CLL 1, HDR_MDCV 2,
SCALABILITY 3, ITUT_T35 4, TIMECODE 5, USER_PRIVATE_6 6.

### 3.1 `struct encoder_packet` (64-bit)

| field | type | offset |
|---|---|---|
| data | `uint8_t *` | 0 |
| size | `size_t` | 8 |
| pts | `int64` | 16 |
| dts | `int64` | 24 |
| timebase_num | `int32` | 32 |
| timebase_den | `int32` | 36 |
| type | `enum obs_encoder_type` (int32; AUDIO=0, VIDEO=1) | 40 |
| keyframe | `bool` | 44 |
| dts_usec | `int64` | 48 |
| sys_dts_usec | `int64` | 56 |
| priority | `int` | 64 |
| drop_priority | `int` | 68 |
| track_idx | `size_t` | 72 |
| encoder | pointer | 80 |

`sizeof = 88` (printed by the `layout` command in every build of the runner; `layout` is not used in vectors).

Only `data`, `size`, `keyframe`, `priority`, `drop_priority` are touched by this component.

### 3.2 Packet data ownership (`obs_parse_*_packet`)

The output `data` pointer is **not** the start of its allocation. The allocation starts 8 bytes (one
`long`) earlier and holds a reference counter initialised to **1**; `data = block + 8`.
`obs_encoder_packet_release` decrements the counter and frees `block` at 0; `obs_encoder_packet_ref`
increments it. A Rust implementation that exposes this over FFI MUST reproduce that layout; a pure
Rust API returns `Vec<u8>`. The source packet is never modified and its data is not referenced.

## 4. Data formats

### 4.1 Annex-B byte stream

`... [00] 00 00 01 <NAL unit bytes> [00] 00 00 01 <NAL unit bytes> ...`. Emulation-prevention bytes
are never inspected (except in R40's SPS bit reader). Example (AVC access unit, vector
`05-avc-extract`): `00000001 0910 | 00000001 6742c01ed900a047fe88 | 00000001 68ce3c80 | 00000001 6588840021ffe0`.

### 4.2 Length-prefixed packet (`obs_parse_*_packet` output)

Per NAL: `uint32 big-endian length | NAL bytes`. Start codes are removed. The above access unit becomes
`00000002 0910 | 0000000a 6742... | 00000004 68ce3c80 | 00000007 6588840021ffe0`.

### 4.3 AVCDecoderConfigurationRecord (`obs_parse_avc_header`)

```
01 | sps[1] sps[2] sps[3] | ff | e1 | be16(len sps) | sps (incl. NAL header byte) | 01 | be16(len pps) | pps
[ fc|chroma_format_idc, f8|bit_depth_luma_minus8, f8|bit_depth_chroma_minus8, 00 ]   (profile_idc 100,110,122,244 only)
```
Worked example (baseline, vector `06-avc-header`): SPS `6742c01ed900a047fe88`, PPS `68ce3c80` ->
`0142c01e ffe1 000a 6742c01ed900a047fe88 01 0004 68ce3c80`.

### 4.4 AV1 OBU

Header byte: `forbidden(1) type(4) extension_flag(1) has_size_field(1) reserved(1)`; optional extension byte;
optional LEB128 `obu_size`; payload. LEB128: little-endian 7-bit groups, continuation bit 0x80, at most 8 bytes.
Example: `32 05 10 aa bb cc dd` = FRAME OBU, 5 payload bytes. LEB128 `e58e26` = 624485.

### 4.5 Metadata OBU (`metadata_obu`)

`2a | leb128(len+2) | metadata_type | payload | 80`. Example: `metadata_obu(aabbcc, type 4)` -> `2a0504aabbcc80`.

## 5. Behaviour (rules)

### 5.1 Start code search

**S-012-R01** `find(p, end)` with `p <= end`: let `k` be the smallest offset in `[p, end-4]` (i.e. `k+4 <= end`)
with bytes `00 00 01` at `k..k+2`. None -> result `end`.
**S-012-R02** If a `k` was found and `k > p` and byte `k-1` is `00` the result is `k-1`, else `k`. (At most
one extra zero is included; further leading zeros are not.)
**S-012-R03** Consequence (QUIRK Q-1): a start code that ends exactly at `end` is not found, i.e. at least one byte
must follow `00 00 01`. The result never depends on the alignment of `p`.
**S-012-R04** The result is always within `[p, end]`. `obs_avc_find_startcode` is identical.
**S-012-R05** An empty range returns `end` (= `p`).

### 5.2 NAL iteration (used by every function below)

```
nal_scan(data, n):                      // yields (code_start, hdr, nal_end)
    pos = find(0, n)
    loop:
        code_start = pos
        while pos < n:                  // skip zeros and the first non-zero byte (the start code's 01)
            b = data[pos]; pos += 1
            if b != 0: break
        if pos == n: stop               // also when the 01 was the last byte
        hdr = pos                       // header byte index (data[hdr] exists)
        nal_end = find(hdr, n)
        yield (code_start, hdr, nal_end)    // NAL unit = data[hdr .. nal_end]
        pos = nal_end
```
**S-012-R06** Bytes before the first start code are ignored. Input without a found start code yields no NAL.
**S-012-R07** Trailing zero bytes before `n` belong to the last NAL (only the one zero in front of a *found*
4-byte start code is stripped, R02). A NAL can be empty (`nal_end == hdr`) when two start codes are adjacent;
its "header byte" is then the first byte of the next start code (Q-4).

### 5.3 AVC

**S-012-R10** `obs_avc_keyframe`: iterate NALs; for the first with `type = hdr_byte & 0x1f` equal to 1 or 5 return
`type == 5`. Types 2,3,4 and all others are skipped. No such NAL -> false. `nal_ref_idc`, forbidden bit ignored.
**S-012-R20** `avc_nal_priority(b) = b >> 5` (0..7: `nal_ref_idc` plus the forbidden bit as value 4; Q-2),
for every NAL regardless of type.
**S-012-R21** `obs_parse_avc_packet_priority(pkt)`: `max(pkt.priority, avc_nal_priority(hdr_byte) for all NALs)`; with
no NAL, `pkt.priority`. The packet is not modified.
**S-012-R22** `obs_parse_avc_packet(out, src)`: `*out = *src` (all fields), then `data`/`size` are replaced by the
length-prefixed form (4.2: for each NAL, `be32(nal_end - hdr)` then the NAL bytes; empty NALs produce a
zero length and no bytes). `out.keyframe` is set to true if any NAL has type 5 (never cleared if `src.keyframe` was true,
Q-6). `out.priority = max(src.priority, R20 over all NALs)`. `out.drop_priority = out.priority` (the source's value is
discarded). `data` follows 3.2 and is always non-NULL even when `size == 0`.
**S-012-R30** `obs_parse_avc_header(&h, data, n)`: `n <= 6` -> return 0, `*h` untouched.
**S-012-R31** If `data[0..2] != 00 00 01` and `data[0..3] != 00 00 00 01` (tested only at offset 0) the input is assumed
to be a finished record: `*h` = exact copy of the input, return `n` (Q-5).
**S-012-R32** Otherwise find parameter sets with R06 iteration: the *last* NAL with `type = hdr & 0x1f == 7` is the SPS, the last
with type 8 the PPS (NAL bytes = `data[hdr..nal_end]`, including header byte; sizes include trailing zeros per R07).
If SPS or PPS is missing or `len(SPS) < 4` -> return 0, `*h` untouched.
**S-012-R33** Output per 4.3. The 16-bit lengths are big-endian. `profile_idc = sps[1]`; for 100,110,122,244 the four extra
bytes are appended where the values come from R34.
**S-012-R34** SPS high-profile fields: take `sps[1..]` (profile_idc first), remove emulation-prevention bytes (every `00 00 03`
triple loses its `03` — scanning left to right, the two zeros are kept, the scan continues after the `03`; a triple needs all three
bytes inside the buffer, remaining bytes are copied verbatim), then read with an MSB-first bit reader that returns 0 for every bit
past the end: 24 bits (profile, constraint flags, level); `ue()` (seq_parameter_set_id); `chroma_format_idc = ue()`; if it equals 3 one bit
(separate_colour_plane_flag); `bit_depth_luma_minus8 = ue()`; `bit_depth_chroma_minus8 = ue()`. Each is stored in a byte
(values are truncated to 8 bits). `ue()`: count leading zero bits `i` (stops at 32, data exhausted counts as zeros), then
`value = (low 8 bits of the next i bits as read) + (2^i - 1)`, truncated to 8 bits (for `i <= 30`; larger `i` is D-4).
**S-012-R35** The result of a successful call is `bmalloc`ed; the 16-bit lengths are truncated modulo 65536 when an SPS/PPS is longer
than 65535 bytes while the full NAL is still written (reference defect D-6; a conforming implementation MUST return 0 instead).
**S-012-R40** `obs_extract_avc_headers`: for each NAL (R06) append `data[code_start .. nal_end]` (start code zeros and the `01` included)
to `header` if `type = hdr & 0x1f` is 7 or 8, to `sei` if 6, else to `new`. Order within each output is input order.
Bytes before the first start code are dropped. Empty outputs have size 0 and a NULL pointer. `bfree(NULL)` is legal.

### 5.4 HEVC

NAL type `t = (hdr_byte & 0x7f) >> 1`. The second header byte is never read; the forbidden bit is ignored.
**S-012-R50** `obs_hevc_keyframe`: the first NAL with `t <= 23` decides: `t >= 16`. NALs with `t >= 24` are skipped. None -> false.
**S-012-R51** per-NAL priority: `t` in 16..=23 (incl. reserved IRAP 22, 23): HIGHEST (3) and "keyframe"; `t` in {1,3,5,7,9}: HIGH (2); every
other `t` (0,2,4,6,8, 10..15, 24..63): DISPOSABLE (0).
**S-012-R52** `obs_parse_hevc_priority`: `max(pkt.priority, R51 over all NALs)`.
**S-012-R53** `obs_parse_hevc_packet`: exactly like R22 with R51 (keyframe = any IRAP NAL; sticky; `drop_priority = priority`).
**S-012-R54** `obs_extract_hevc_headers`: like R40; `header` for t in {32,33,34}, `sei` for 39 and 40, rest `new`.

### 5.5 AV1

`leb128(buf, avail)`: value = sum of `(b_i & 0x7f) << 7i` (full 64-bit arithmetic, D-3) over bytes `i < min(8, avail)` stopping after the
first byte with bit 7 clear; `len` = bytes consumed (may be 0 when `avail == 0`, and may end on a byte with bit 7 set when data ran out).
**S-012-R70** `parse_obu(buf, avail)` (`avail >= 1`; for `avail == 0` all outputs 0): `type = (b0 >> 3) & 15`, `ext = (b0 >> 2) & 1`,
`has_size = (b0 >> 1) & 1`. `start = 1 + ext`. If `has_size`: `size = leb128(buf+start, max(avail-start,0))`, `start += len`; else `size = avail - 1`
(the extension byte is *not* subtracted: Q-7). Reported even when `start`/`size` exceed `avail` (callers clamp, D-1).
**S-012-R71** `obs_av1_keyframe`: `pos = 0`; while `pos < n`: parse at `pos`; if `size != 0` and type is FRAME(6) or FRAME_HEADER(3): let `v` be the byte at
`pos+start`; if `pos+start >= n` return false (D-2); if `v & 0x80` (show_existing_frame) return false; else return `((v >> 5) & 3) == 0`.
Otherwise `pos += start + size`; stop when `pos >= n` or the sum exceeds the remaining data. End -> false.
**S-012-R72** `obs_extract_av1_headers`: walk as in R71 (no early exit); for each OBU the chunk is `buf[pos .. pos+start+size]`, clamped to the data present (D-1).
Chunks of type SEQUENCE_HEADER (1) or METADATA (5) are appended to `header`. **Every** chunk is appended to `new` (Q-3), so `new` equals the
whole input and `header` is a subsequence of it. Trailing bytes (e.g. one zero byte) are walked as OBUs too (type 0, no size -> consumes the rest).
**S-012-R73** `metadata_obu`: output `2a || leb128(len+2) || type || payload || 80`, `len(out) = 1 + len(leb) + 1 + len + 1`; the LEB128 encoder is the
minimal one (no redundant groups). `metadata_obu_itu_t35(p, n)` = `metadata_obu(p, n, ..., 4)`. The result is `bmalloc`ed and zero-initialised before the writes. Size 0
payload is allowed (`2a020480` for type 4).

### 5.6 NULL / empty input

**S-012-R80** `size == 0` (data pointer non-NULL or NULL, D-5) behaves exactly as an empty buffer: keyframe false, priority `pkt.priority`, packet with `size == 0`
and keyframe/priority as R22, header 0, extract all-empty.

## 6. Algorithms

See R01/R02 (find), R06 (nal_scan), R34 (bit reader), R70/R71/R72 (OBU walk). Examples of R01/R02: `aa 00 00 00 01 bb` -> `k = 2`, byte 1 is zero, result 1. `aa 00 00 01` (n = 4) -> no `k` with `k+4 <= 4`, result 4 (Q-1).

## 7. Concurrency

None; all functions are reentrant. Results are independent allocations.

## 8. Reference defects and quirks

| ID | Kind | Input | Reference | Required |
|---|---|---|---|---|
| Q-1 | QUIRK | start code in the last 3 bytes | not found, bytes stay in the previous NAL | match (SHOULD); scripts `02`, `04`, `05` |
| Q-2 | QUIRK | AVC NAL header with bit 7 set | priority 4..7 | match; `19-avc-quirks` |
| Q-3 | QUIRK | AV1 extract | `new` also contains header OBUs and TDs | match; `14` |
| Q-4 | QUIRK | adjacent start codes | zero-length NAL emitted (packet: length 0), priority from first byte of next start code | match; `19` |
| Q-5 | QUIRK | `parse_avc_header` input not starting with a start code | returned as exact copy (also `00 00 00 00 01`, `aa 00 00 00 01`) | match; `07` |
| Q-6 | QUIRK | `obs_parse_*_packet` | source `keyframe` is only ever raised | match; `19` |
| Q-7 | QUIRK | AV1 OBU with extension flag but no size field | `size = avail - 1` although the extension byte is also in the header | match R70; `12` |
| D-1 | DEFECT | AV1: OBU extent past end (size field too large, ext flag without size, header byte `0e` alone) | out-of-bounds read in extract (copies beyond the buffer; `parse_obu_header` with `size - start` underflow reads the whole address space) | clamp (R72); `17` |
| D-2 | DEFECT | AV1 keyframe: FRAME/FRAME_HEADER with non-zero size but no payload byte present | reads 1 byte past the end | return false (R71); `17` |
| D-3 | DEFECT | AV1 LEB128 groups 5..8 | `int` shift overflow: wrong values, UB | exact 64-bit value (R70); `17` |
| D-4 | DEFECT (UB) | AVC high-profile SPS where `ue()` sees 31/32 zero bits or exhausted data | `1 << 32` UB; reference on x86-64 gives garbage/zero fields | unspecified value, no crash, same output length; `16` records the x86 result |
| D-5 | DEFECT | NULL data with size 0 | every scanning function crashes (pointer arithmetic below 0) | R80; `18` |
| D-6 | DEFECT | AVC SPS/PPS > 65535 bytes | length field truncated, data written in full => corrupt record | return 0 (not covered by a vector: needs a 64 KiB parameter set; MAY be tested by the implementer) |

## 9. Implementation notes (non-normative)

- Implement `find` as the simple linear scan of R01/R02 (the reference uses a word-at-a-time search; results are identical, verified with 10^6 random buffers).
- Return `Vec<u8>` / `Option` from safe functions; only an FFI layer needs `bmalloc` blocks and the refcount prefix of 3.2.
- AV1: use `u64` for sizes, `usize::try_from`, saturating/ checked adds; never index with `start + size` unchecked.
- Never read `data[hdr]` unless `hdr < n` (R06 guarantees it).

## 10. Conformance coverage

| Rule | Scripts |
|---|---|
| R01-R05 | `01-startcode-basic`, `02-startcode-quirks` |
| R06, R07 | `01`, `05-avc-extract`, `04-avc-priority-packet`, `19-avc-quirks` |
| R10 | `03-avc-keyframe` |
| R20, R21 | `04-avc-priority-packet`, `19-avc-quirks` |
| R22 | `04-avc-priority-packet`, `19` |
| R30-R35 | `06-avc-header`, `07-avc-header-edges`, `16-ub-unspecified` |
| R40 | `05-avc-extract` |
| R50, R51 | `08-hevc-types`, `09-hevc-keyframe` |
| R52, R53 | `10-hevc-packet` |
| R54 | `11-hevc-extract` |
| R70 | `12-av1-leb128-obu-header`, `17-defects-av1` |
| R71 | `13-av1-keyframe`, `17` |
| R72 | `14-av1-extract`, `17` |
| R73 | `15-av1-metadata` |
| R80 | `18-defects-null-input` |

## 11. Open questions

- D-6 has no vector. D-4 values are not specified. The runner's `av1_obu`/`av1_leb128` probes expose internal helpers purely for testing.
- Windows/macOS: the same source is used; none of the behaviour depends on the platform.
