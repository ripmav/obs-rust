# S-050 Hardware encoders: NVENC, QSV, VAAPI, FFmpeg-NVENC (behavioural catalog)

| | |
|---|---|
| Reference | OBS Studio 33.0.1: `plugins/obs-nvenc/{nvenc,nvenc-helpers,nvenc-compat}.c`, `plugins/obs-qsv11/{obs-qsv11,obs-qsv11-plugin-main}.c`, `plugins/obs-ffmpeg/{obs-ffmpeg-nvenc,obs-ffmpeg-vaapi}.c`, `texture-amf*.{cpp,hpp}` (rules only) |
| Tier | C (behavioural catalog): driver interaction is a call trace, not byte-exact |
| Suite | `test/conformance/gen/hwencoders_*`, `scripts/hwencoders/*.script` (57 scripts), `vectors/hwencoders/*.expected` |
| Rust sufficiency check | **Skipped** (cost control, per coordinator instruction). Vectors are therefore verified only against the C reference. |

## 1. Scope

In scope (everything an encoder plugin does between the host and the vendor library, other than option parsing and property lists):

- **obs-nvenc**: settings to `NV_ENC_INITIALIZE_PARAMS`/`NV_ENC_CONFIG` (preset, tune, multipass, rate control, GOP, b-frames, buffer sizing, lookahead, AQ, per-codec profile/VUI/HDR/bit depth), live reconfigure, create-time format and HDR gates, texture/soft routing, per-picture parameters (buffer format, HDR metadata, ROI map), packet fetch and dts/priority post-processing, flush on destroy, SDK error mapping, library/driver version gate, the capability table (INI from `obs-nvenc-test`), the legacy `jim_*` compat encoders and the registration table.
- **obs-qsv11**: create gates and texture fallbacks, header assembly, 90 kHz timestamp conversion, packet post-processing, ROI forwarding, module registration.
- **obs-ffmpeg-nvenc** (`ffmpeg_nvenc`, `ffmpeg_hevc_nvenc`): settings to libavcodec private options, create gates and the psycho-AQ retry, init-error text, first-packet header extraction and the dts correction, reconfigure, `video_info`.
- **obs-ffmpeg-vaapi**: rate-control table, settings to `AVCodecContext`, init chain failures, defaults and device-modified callback driven by capability probes, packet post-processing, `video_info`, registration.

Not specified here (already covered, do not duplicate): nvenc defaults/properties/`rate_control_modified`/user-option parsing (S-025 section 6), QSV defaults/properties/migrations/`update_params`/`video_info` size cap (S-025 section 7), the shared `ffmpeg_video_encoder_*` layer (S-025/S-047; here a recorded call boundary), Annex-B/OBU header extraction (S-012; used as real code).

**Not covered** (listed honestly): AMF (section 9, rules from reading only, no vectors), `vaapi-utils.c` device probing (libva), `vaapi_properties_internal` (needs `/dev/dri` and libpci), VAAPI texture/surface paths, NVENC CUDA/D3D11/OpenGL backends (`nvenc-cuda.c`, `nvenc-d3d11.c`, `nvenc-opengl.c`, `cuda-helpers.c`: stubbed), `obs-nvenc-test` / `obs-qsv-test` helper programs, QSV library internals (`QSV_Encoder*.cpp`), `obs-ffmpeg-av1.c` and `-openh264.c`, nvenc compat build option `ENABLE_NVENC_FFMPEG_IDS` (extra `ffmpeg_nvenc` ids registered; compile-time, not exercised), Windows/macOS code paths.

## 2. Conventions

- Rule ids `S-050-Rnn`; QUIRK `Q-n` (should match), DEFECT `D-n` (required behaviour stated).
- "Settings" means an `obs_data` object; absent keys read as `0`/`""`/`false` (not the defaults unless the defaults function ran).
- Log text matters only where a rule says so; vectors contain it, a Rust port may differ in wording but must log at the same level.
- Translation keys are shown as-is (identity `obs_module_text`).
- `astrcmpi` = ASCII case-insensitive compare, `strcmp` = case-sensitive. The difference matters (Q-1).

## 3. What was shimmed, unverified, or could not be linked

**Shimmed (hand-written, `gen/hwencoders_inc/ffnvcodec/{nvEncodeAPI,dynlink_cuda}.h`)**: the NVENC SDK header (declared as SDK 13.0). Identifier *names* were derived from what the plugin sources use. Struct layouts, enum values (except that status codes follow the published order from memory), GUID values and `*_VER` constants are fabricated; GUIDs are printed symbolically. The below-12.2 branches (`pixelBitDepthMinus8`, `NVENC_12_2_OR_LATER` off) and the `NVENC_12_1_OR_LATER` macro were not compiled and are **unverified**.
**Fakes (`hwencoders_host.c`, `_nvenc.c`, `_nvhelp.c`, `_ffnv.c`, `_vaapi.c`, `_qsv.c`, `_stubs.c`)**: the whole NVENC function list; `os_dlopen`/`os_dlsym`/process pipe/profiler; obs encoder/video accessors, registration and rerouting; the QuickSync library (`qsv_encoder_*`, adapter probe); every libavcodec/libavutil/libva call made by the ffmpeg-based plugins (`av_opt_set*`, `av_hwdevice_ctx_create`, `avcodec_open2`, `avcodec_send_frame/receive_packet`, ...) and all of `vaapi-utils.c`; the shared `ffmpeg_video_encoder_*` layer; graphics (`gs_*`) and CUDA backends.
**Real code linked**: `libobs/obs-data.c`, `obs-properties.c`, `util/config-file.c`, `obs-avc.c`, `obs-hevc.c`, `obs-av1.c`, `obs-nal.c`, `shared/opts-parser`, `nvenc-properties.c`, `nvenc-opts-parser.c`. **System libraries needed to build**: libjansson, libavutil, libavcodec, libva, libpci and their headers; the real oneVPL headers (`/usr/include/vpl`) are used for QSV types, the real libav/libva headers for the ffmpeg plugins.
**Could not be exercised**: anything needing a GPU, a driver, `/dev/dri`, or real SDK semantics (what a preset config contains, what NVENC does with the values). The fake preset config returns all-zero fields unless a script sets them (`nvpreset`), so "SDK default" values never appear in vectors.

## 4. obs-nvenc

### 4.1 Capability table and library gate (`nvenc-helpers.c`)

- **S-050-R01** `nvenc_supported() = load_nvenc_lib() && nvenc_check()`. The library name is `libnvidia-encode.so.1` (Linux), `nvEncodeAPI64.dll` (Windows), `nvEncodeAPI.dll` (ARM64). A load failure short-circuits: the helper process is not started.
- **S-050-R02** `nvenc_check` runs the helper `obs-nvenc-test` (resolved next to the executable), reads its stdout to EOF, and parses it as an INI document. Failure paths (all `LOG warning`, all return false): cannot start the pipe ("Failed to launch the NVENC test process I guess"); empty output ("Seems the NVENC test subprocess crashed. Better there than here I guess."); `[general] nvenc_supported` false/absent/section missing ("Test process failed: <reason or unknown>", `reason` read from `[general]`). A document that `config_open_string` rejects logs "Failed to open config string" (not exercised: no reproducer found).
- **S-050-R03** Success reads `[general] nvenc_devices` (int) and per codec section (`h264`, `hevc`, `av1`): `codec_supported` (bool; if false the section's other keys are **not** read and the caps stay zero), ints `bframes bref engines max_width max_height temporal_filter lookahead_level`, bools `dynamic_bitrate lookahead lossless temporal_aq 10bit yuv_444 yuv_422 uhq`. Bools accept `true`/`1` (config-file rules, S-007). Absent keys read 0/false. One `LOG info` line reports compiled vs driver NVENC version, CUDA version and whether AV1 is supported.
- **S-050-R04** `[general] driver_ver` is parsed as `"%d.%d"` into major/minor; `has_broken_split_encoding() = major < 555`.
- **D-2** `driver_ver` missing in an otherwise supported document: the reference passes NULL to `sscanf` and **crashes** (vector `32-...-defect-missing-driver-ver`, recorded as `crash: signal 11` in a forked child). Required: treat as `0.0` (split encoding broken), no crash.
- **S-050-R05** `init_nvenc` (once per process, mutex protected, result cached): resolves `NvEncodeAPIGetMaxSupportedVersion` (missing, or reporting 0 -> last error `"Missing NvEncodeAPIGetMaxSupportedVersion, check your video card drivers are up to date."`), requires `version >= (MAJOR<<4|MINOR)` of the compiled SDK else last error `OutdatedDriver` and a log error; resolves `NvEncodeAPICreateInstance` (missing -> `"Missing NvEncodeAPICreateInstance, check your video card drivers are up to date."`); calls it to fill the function table. A driver reporting a newer version is accepted.
- **Q-2** The failure is **sticky**: after the first failed `init_nvenc` every later call returns false immediately and sets no last error.
- **S-050-R06** `nv_failed2(encoder, session, status, func, call)`: `SUCCESS` -> returns false, no report. Otherwise returns true and sets the encoder last error: `OUT_OF_MEMORY`, `INCOMPATIBLE_CLIENT_KEY` -> `TooManySessions`; `NO_ENCODE_DEVICE`, `UNSUPPORTED_DEVICE` -> `UnsupportedDevice`; `INVALID_VERSION` -> `OutdatedDriver`; every other code -> `"NVENC Error: " + X` where X is `"<sdk text> (<NAME>)"` if the session reports a non-empty last-error string (leading `:` characters stripped; if nothing is left it counts as empty) else `"<func>: <call> failed: <code> (<NAME>)"`. The log line (error) always has `"<func>: <call> failed: <code> (<NAME>)"` plus `": <sdk text>"` when present. `<NAME>` is the SDK enumerator name for codes 0..26 and `"Unknown Error"` otherwise. `nv_fail2(msg)` sets `"NVENC Error: <msg>"` and logs it.

### 4.2 Create-time routing and gates

- **S-050-R07** `nvenc_create_base(codec, settings, texture)`: read `device` (int) and whether it has a user value; `force_cuda_tex` (Linux only; false on Windows). Reroute to the soft encoder (`obs_nvenc_{h264,hevc,av1}_soft`) when `texture` and: `device` has a user value, `!= -1`, and `!force_cuda_tex` (log info "different GPU selected by user..."); or encoder scaling is on and GPU scaling is off (log info "CPU scaling enabled"); GPU scaling on only logs info "GPU scaling enabled"; or neither NV12 nor P010 texture is active (info). Otherwise `nvenc_create_internal`; if that returns NULL and `texture`, reroute; if not `texture`: `LOG error "Already in non_texture encoder, can't fall back further!"` and NULL. The return value of a reroute is whatever the host returns.
- **S-050-R08** `nvenc_create_internal` order: read properties (S-025 R31) -> `init_nvenc` -> CUDA init -> create instance -> CUDA context -> `nvEncOpenEncodeSessionEx` (CUDA device on Linux; DX11 for texture on Windows; `apiVersion = NVENCAPI_VERSION`) -> `init_encoder` -> create `buf_count` bitstream buffers -> backend surfaces/textures. Any failure destroys what exists and returns NULL; `nvenc_destroy` of a half-built encoder must be safe (vector 25).
- **S-050-R09** `init_encoder` gates, in order: query caps `10BIT`, `YUV444`. `in_format`: preferred format of the encoder, else the output format; map I010/P010 -> P010, RGBA/BGRA/BGRX/I444 -> I444, else NV12. I444 without cap -> `NV_FAIL("444Unsupported")`; 10-bit (P010 input) without cap -> `NV_FAIL("10bitUnsupported")`; output format not I010/P010 with BT.2100 PQ/HLG -> `NV_FAIL("8bitUnsupportedHdr")`. `is_10_bit` = (soft encoder: in_format == P010; texture encoder: P010 texture active). `NV_FAIL` sets last error `"NVENC Error: <key>"`.
- **S-050-R10** HDR metadata (SDK >= 13.0): for PQ or HLG output allocate `mdi`/`cll`: peak = `hdr_nominal_peak_level` (PQ) or 1000 (HLG); primaries R(13250,34500) G(7500,3000) B(34000,16000) white (15635,16450); `maxLuma = peak * 10000` (u32), `minLuma = 0`; `maxContentLightLevel = maxPicAverageLightLevel = peak` (u16). Allocated for **every** codec (even H.264) but only attached to HEVC/AV1 pictures (R22) and flagged in the HEVC/AV1 config (R15/R16).

### 4.3 Encoder configuration (`init_encoder_base` and codec functions)

Let `rc` = `rate_control` string, `cqvbr = rc =ci "CQVBR"`, `vbr = cqvbr || rc =ci "VBR"`, `lossless = rc == "lossless"` (case-sensitive), `cqp = rc == "CQP"` (case-sensitive) (**Q-1**: the default written by S-025 R30 is the lowercase `"cbr"`, so the default is neither VBR nor CQP nor "CBR"-exact; lowercase `"cqp"` silently becomes CBR).

- **S-050-R11** Preset: `p1..p4,p6,p7` (case-insensitive) -> that preset, anything else (including `p5`, `""`, `p8`) -> P5. Tuning: `ll` -> LOW_LATENCY, `ull` -> ULTRA_LOW_LATENCY, `uhq` -> ULTRA_HIGH_QUALITY (SDK >= 12.2), anything else -> HIGH_QUALITY. Multipass: `qres` -> QUARTER, `fullres` -> FULL, else DISABLED.
- **S-050-R12** `lossless`: tuning := LOSSLESS, multipass := DISABLED, `adaptive_quantization := false`, `cqp := 0`, and the property `rate_control := "Lossless"` (the log shows it). The preset config is fetched with `nvEncGetEncodePresetConfigEx(codec, preset, tuning)`; failure -> NULL with `nv_failed`.
- **S-050-R13** Base config := the preset config. `keyint = keyint_sec * fps_num / fps_den` (signed int, truncating); the user option `keyint` (S-025 R38) replaces it. `gopLength = keyint > 0 ? keyint : 250`. With tuning UHQ and `bf < 4`: `bf := 4` (log warning). `frameIntervalP = (gopLength == 1) ? 0 : bf + 1`. Initialize-params: encode/dar size = output size, `frameRateNum/Den` = fps, `enableEncodeAsync = 0`, `enablePTD = 1`, `tuningInfo`, `splitEncodeMode = split_encode` (SDK >= 12.1, passed unvalidated).
- **S-050-R14** Buffers and lookahead:
  ```
  la = cap(LOOKAHEAD) && (props.lookahead || preset.enableLookahead)
  depth = la ? (preset.enableLookahead ? preset.lookaheadDepth : 8) : 0 ; if la: user option lookaheadDepth overrides
  buf = max(4, 4 * frameIntervalP) ; if la: buf = max(buf, frameIntervalP + depth + 5) ; buf = min(buf, 64)
  output_delay = buf - 1
  if la: bound = output_delay - frameIntervalP - 4
         if bound >= 0: enableLookahead = 1, lookaheadDepth = min(depth, bound), disableIadapt = disableBadapt = 0
         else: la = false          // Q-3: the preset's own enableLookahead/lookaheadDepth are left as returned
  disableIadapt = disable_scenecut   // unconditional, after the above
  ```
  `max/min` on signed ints; `lookaheadDepth` is u16.
- **S-050-R15 AQ**: if `adaptive_quantization`: `enableAQ = 1`, `aqStrength = 8`, `enableTemporalAQ = cap(TEMPORAL_AQ)`. If false, preset bits are not cleared.
- **S-050-R16 Rate control**: `can_change_bitrate = cap(DYN_BITRATE_CHANGE)`. Start as VBR with `averageBitRate = bitrate*1000`, `maxBitRate = vbr ? max_bitrate*1000 : bitrate*1000`, `vbvBufferSize = bitrate*1000` (all u32; **D-1** the C computes the product in signed int, undefined above 2147483 kbps although the property allows 4294967: required behaviour is unsigned 32-bit modular arithmetic, which equals what the reference produces in practice). Then: `cqp||lossless` -> mode CONSTQP with `qpInterP=qpInterB=qpIntra = (AV1 ? cqp*4 : cqp)`, `can_change_bitrate := false`; else not `vbr` -> mode CBR; else `cqvbr` -> `targetQuality = target_quality` (u8), `averageBitRate = 0`, `vbvBufferSize = 0` (mode stays VBR). Always `multiPass = multipass`, `qpMapMode = DELTA`. One info log "settings:" with codec, rate_control, bitrate (if non-zero and not CQVBR), max_bitrate (if vbr), cqp, cq, keyint, preset, tuning, multipass, profile, size, b-frames, b-ref-mode, `lookahead: <bool> (<depth> frames)`, aq, split encode (if non-zero), user opts (if any).
- **S-050-R17 H.264**: if `repeat_headers`: `repeatSPSPPS=1, disableSPSPPS=0, outputAUD=1`. `idrPeriod = gopLength` **before** user options are applied (**Q-4**: a user `gopLength` changes the config but not `idrPeriod`). `sliceMode=3, sliceModeData=1`, `useBFramesAsRef = bframe_ref_mode`. CBR mode -> `enableFillerDataInsertion=1`. VUI: signal-type present=1, full range = (range == FULL), colour description present=1; colours: 601 -> (6,6,6); DEFAULT/709 -> (1,1,1); SRGB -> (1,13,1); anything else (PQ/HLG) leaves 0 (not reachable on 8-bit; reachable with a 10-bit output, **Q-5**: H.264 gets no HDR VUI). `lossless` -> `qpPrimeYZeroTransformBypassFlag=1`; else if `rc == "CBR"` (exact, **Q-1**) -> `outputBufferingPeriodSEI=1`. `outputPictureTimingSEI=1`. Profile: I444 input -> HIGH_444 + `chromaFormatIDC=3`; (SDK 13) `high10` -> HIGH_10; else 10-bit input -> HIGH_10 with a warning; `main`, `baseline` (case-insensitive); else HIGH unless lossless (profile left as the preset's). SDK 13: `inputBitDepth = 10 if is_10_bit else 8`, `outputBitDepth = 10 iff profile == HIGH_10`.
- **S-050-R18 HEVC**: same as R17 with: HDR VUI for PQ (9,16,9) and HLG (9,18,9) plus chroma sample location flag=1 top=bottom=2; `outputBufferingPeriodSEI = 1` iff `rc =ci "cbr"` (case-insensitive: differs from H.264); profile I444 -> FREXT + chroma 3, `main10` -> MAIN10, 10-bit input -> MAIN10 (warning), else MAIN; `inputBitDepth` by input, `outputBitDepth = 10 iff MAIN10`; with 10-bit input and BT.2100: `outputMasteringDisplay = outputMaxCll = 1`.
- **S-050-R19 AV1**: `idrPeriod = gopLength`, `useBFramesAsRef`, `colorRange = (range == FULL)`, `enableBitstreamPadding=1` in CBR mode, `numTileColumns = 2` iff `width*height >= 3840*2160`; colours: 601 (6,6,6), 709/default (1,1,1), sRGB (1,13,1), PQ (9,16,9), HLG (9,18,9) as colorPrimaries/transfer/matrix; profile MAIN, tier 0, level AUTOSELECT(0), `chromaFormatIDC=1`, `inputBitDepth=outputBitDepth = 10 if is_10_bit else 8`, `numFwdRefs=numBwdRefs=1`, `repeatSeqHdr=1`; HDR flags as HEVC.
- **S-050-R20** All three: `apply_user_args` failure (S-025 R34 result false) -> last error `Opts.Invalid`, create fails **without** calling `nvEncInitializeEncoder`; user args are applied after the generic config above, so they override it. Then `nvEncInitializeEncoder`; failure -> NULL.

### 4.4 Update, encode, packets

- **S-050-R21** `nvenc_update(settings)`: only if `can_change_bitrate`; else returns true with no SDK call. Re-read `bitrate`, `max_bitrate`. `cqvbr` is taken from the create-time `rate_control` property, `vbr` from the *configured mode* (CQVBR counts as VBR). CQVBR: `targetQuality = target_quality`, `averageBitRate = 0`; else `targetQuality = 0`, `averageBitRate = bitrate*1000`. `maxBitRate = vbr ? max_bitrate*1000 : bitrate*1000` (u32). `nvEncReconfigureEncoder(reInitEncodeParams = the init params, resetEncoder=1, forceIDR=1)`; failure -> false (last error set).
- **S-050-R22** Per picture (`nvenc_encode_base`): `inputTimeStamp = pts`, `frameIdx = (u32)pts`, input size = encoder size, `inputPitch = width`, `pictureStruct = FRAME`. Buffer format: soft encoder -> the surface format; texture -> `YUV420_10BIT` iff the P010 texture is active else `NV12`. HEVC/AV1 with `cll`/`mdi` allocated: attach pointers. ROI map when the encoder has ROIs (R24). `nvEncEncodePicture`: success or `NEED_MORE_INPUT` continue; anything else -> `nv_failed`, return false **without** counting the buffer. Then `buffers_queued++`, `next_bitstream` wraps at `buf_count`, then `get_encoded_packet(finalize=false)`.
- **S-050-R23** `get_encoded_packet(finalize)`: clears `packet_data`; returns true with nothing if no buffers are queued, or (not finalize and `buffers_queued < output_delay`). Count = all queued if finalize else 1. For each: lock the bitstream at `cur_bitstream` (failure -> false; the buffer **stays** queued); on the first packet ever, fetch the sequence header (`nvEncGetSequenceParams`, 256-byte buffer) and keep a copy (D-4: a zero-length header makes `bmemdup` call `bmalloc(0)`; the reference logs "Allocating 0 bytes is broken behavior" and misbehaves; required: store an empty header and report `get_extra_data` false); copy the data, `packet_pts = outputTimeStamp`, `packet_keyframe = (pictureType == IDR)`; priority: I, BI, IDR, SWITCH -> 3; P -> 2; B, NONREF_P and everything else -> 0; unlock; unmap the input resource if mapped; advance `cur_bitstream` (wrap at `buf_count`), `buffers_queued--`.
  Output: if data was produced: pop the oldest dts from `dts_list` (pushed by the backend encode function, one per submitted frame), `dts -= bf * packet.timebase_num` for H.264/HEVC (not AV1; `bf` is the configured b-frame count property), fill `data,size,type=VIDEO,pts,dts,keyframe,priority`, `received_packet=true`; else `received_packet=false`.
- **S-050-R24 ROI**: macroblock size 16 (H.264), 32 (HEVC), 64 (AV1); map = `ceil(w/mb) * ceil(h/mb)` int8, zero-initialised, row-major; rebuilt only when the encoder's ROI increment differs from the cached one (**Q-6**: ROI edits without an increment change are ignored; the first map is built at the first encode even if the increment is 0). Each ROI in enumeration order sets `qp = (int8)(-51 * priority)` (AV1: `-128 * priority`, float truncated toward zero) in the cells `left/mb .. (right-1)/mb` x `top/mb .. (bottom-1)/mb`; later ROIs overwrite earlier. Priorities outside [-1, 1] overflow the cast (undefined; do not rely).
- **S-050-R25** `nvenc_destroy`: if any frame was encoded, send an EOS picture (`encodePicFlags = EOS`) and `get_encoded_packet(finalize=true)` (the packets are dropped); destroy bitstream buffers, backend resources, the session, the CUDA context, header/roi/HDR buffers.
- **S-050-R26** `get_extra_data` returns false until the first packet header exists; `get_sei_data` always returns false (the `sei` buffer is never filled, **Q-7**) and the AV1 encoders do not register it at all.

### 4.5 Compat encoders (`nvenc-compat.c`) and registration

- **S-050-R27** `register_encoders`: `obs_nvenc_{h264,hevc,av1}_tex` (caps `PASS_TEXTURE|DYN_BITRATE|MULTITRACK_DYN_BITRATE|ROI`, `encode_texture2`) and `_soft` (caps `DYN_BITRATE|MULTITRACK_DYN_BITRATE|ROI|INTERNAL`, `encode`, `get_video_info` = set format to `in_format`; the AV1 pair omits `get_sei_data`); AV1 only if the probe reports AV1 supported. Then `register_compat_encoders`: `jim_nvenc`, `obs_nvenc_h264_cuda`, `jim_hevc_nvenc`, `obs_nvenc_hevc_cuda`, and (AV1 supported) `jim_av1_nvenc`, `obs_nvenc_av1_cuda`, all with `DEPRECATED` (the `jim_*` ones with `PASS_TEXTURE`). Exact order and flags: vectors 34.
- **S-050-R28** Compat `create`: `preset := string(preset2)`, `adaptive_quantization := bool(psycho_aq)`, and iff `gpu` has a user value and `num_encoder_devices() > 1`: `device := int(gpu)`; then reroute to `obs_nvenc_<codec>_tex` (jim ids) or `_soft` (cuda ids). Missing source keys migrate as `""`/`false`. Compat defaults/properties use the old names (`preset2`, `psycho_aq`, `gpu`; bitrate 2500, max_bitrate 5000, rate_control `"CBR"`, bf 2, gpu 0, bitrate range 50..300000, bf 0..4); compat `rate_control_modified` is fully case-insensitive and toggles `preset2`/`tune` for lossless.

## 5. obs-qsv11 (beyond S-025)

- **S-050-R30** Module load: probe adapters; any Intel adapter -> register `obs_qsv11_v2` (tex), `obs_qsv11` (tex v1, deprecated), `obs_qsv11_soft_v2`, `obs_qsv11_soft` (deprecated); any Intel adapter with AV1 -> `obs_qsv11_av1`, `obs_qsv11_av1_soft`; with HEVC -> `obs_qsv11_hevc`, `obs_qsv11_hevc_soft` (flags in vectors 60).
- **S-050-R31** `create`: format gates identical to S-025 R44 (10-bit AVC, 16-bit, HDR on 8-bit), then `qsv_encoder_open`; NULL -> warning, destroy and return NULL; AV1/AVC `extra_data = SPS [+ PPS unless AV1]`, `sei = a single zero byte` (AV1 registers no SEI callback); HEVC `extra_data = VPS+SPS+PPS`, `sei` empty; a library-version info line is always logged; `os_request_high_performance("qsv encoding")`. `get_extra_data`/`get_sei_data` return false without a context.
- **S-050-R32** Texture fallback (reroute ids `obs_qsv11_soft_v2`, `obs_qsv11_av1_soft`, `obs_qsv11_hevc_soft`; v1 texture uses `obs_qsv11_soft`): not on an Intel adapter (`adapters[ovi.adapter].is_intel`); AV1 and that adapter lacks AV1 (HEVC support is **not** checked); no NV12 texture (AVC) / neither NV12 nor P010 texture (AV1, HEVC); CPU scaling. GPU scaling logs and proceeds.
- **S-050-R33** Timestamps: obs->mfx `ts * 90000 / fps_num` (i64, truncating; `fps_den` unused); mfx->obs `(ts*fps_num + s*div/2) / div * fps_den`, `div = 90000*fps_den`, `s = +1` for `ts >= 0` else `-1` (rounds half away from zero).
- **S-050-R34** Packets: no bitstream or `DataLength == 0` -> no packet. `pts/dts` via R33; `keyframe = (FrameType & IDR(0x80))` stored in a bool; priority: `I(0x01)` -> 3, else `P(0x02)` or `REF(0x40)` -> 2, else 0. AVC only: for every NAL after a start code whose type (low 5 bits) is 1 or 5, set `ref_idc` (bits 5-6) to `priority` (so `priority 3` -> `0x65`, 0 clears). `DataLength` of the library bitstream is zeroed after consumption. Library error (<0) -> warning, false. The texture entry fails (`false`, `next_key := lock_key`) on null texture planes.
- **S-050-R35** ROI: when the encoder has ROIs and the increment differs from the cached one (initially 0): `qsv_encoder_clear_roi`, then add the ROIs in **reverse** enumeration order, cache the increment. **Q-6** applies (increment 0 never sends).

## 6. obs-ffmpeg-nvenc

- **S-050-R40** Defaults: `bitrate 5000`, `max_bitrate 6000`, `keyint_sec 0`, `cqp 20`, `rate_control "CBR"` (uppercase), `preset2 p5`, `multipass qres`, `tune hq`, `profile high` (main for HEVC/AV1), `psycho_aq true`, `gpu 0`, `bf 2`, `repeat_headers false`. Properties: preset/tune/multipass lists as obs-nvenc compat with `NVENC.*` keys; registered as `ffmpeg_nvenc` / `ffmpeg_hevc_nvenc` (`caps DYN_BITRATE`).
- **S-050-R41** `update` trace (all via `av_opt_set*` on `priv_data`): force CBR (warning) if bool `cbr`; `info.format` via R46; `cbr=0`; `profile`; preset: if `preset` has a user value and `preset2` does not: `preset` (with `mq` -> `hq`), else `preset=preset2`, `tune`, `multipass`. Rate control (**case-insensitive**): `cqp` -> bitrate := 0, `global_quality = cqp`; `lossless` -> bitrate := 0, `cqp := 0`, then `tune=lossless`, `multipass=disabled`; not `vbr` -> `cbr=1`, `rc_max_rate = rc_min_rate = bitrate*1000`, `cqp := 0`. Then `level=auto`, `gpu`, `no-scenecut`, `spatial-aq`/`temporal-aq` (= psycho_aq), `max_b_frames = bf`, then the shared `ffmpeg_video_encoder_update(bitrate, keyint_sec, ..., ffmpeg_opts)` and `init_codec`.
- **S-050-R42** `create` gates (before any libav call, last error + log error): H.264: I010/P010 -> `NVENC.10bitUnsupported`; HEVC: I010 -> `NVENC.I010Unsupported` (P010 allowed); both: P216/P416 -> `NVENC.16bitUnsupported`; otherwise BT.2100 on non-10-bit -> `NVENC.8bitUnsupportedHdr` (H.264 also refuses via the 10-bit rule). With `psycho_aq` true and a failed first attempt: warning and retry once with it false; NULL if that fails or `psycho_aq` was already false.
- **S-050-R43** `on_init_error(ret)`: last error = `NVENC.Error` with `%1` replaced by the libav error text + `<br><br>` + (gpu > 0: `NVENC.BadGPUIndex` with `%1` = gpu; else if `ret == AVERROR_EXTERNAL`: `NVENC.GenericError`; else `NVENC.CheckDrivers`). `gpu` is remembered separately because libav wipes `priv_data` on error.
- **S-050-R44** First packet: extract headers with the AVC/HEVC extractor (S-012) into `header`/`sei`, returned data has them removed. If `max_b_frames != 0` and `pkt.dts != -(max_b_frames * time_base.num)`: `dts_offset = expected - pkt.dts` (info "Applying DTS value corrections"). Every encoded packet gets `dts += dts_offset`. `get_extra_data`/`get_sei_data` always return true (possibly empty).
- **S-050-R45** `reconfigure`: for `rate_control` =ci CBR or VBR only: `bit_rate = rc_max_rate = bitrate*1000`; always returns true.
- **S-050-R46** `video_info`: preferred format if in {I420, NV12, I444, I010, P010}, else the input format if valid, else NV12.

## 7. obs-ffmpeg-vaapi

- **S-050-R50** RC table (case-insensitive lookup): `CBR` {bitrate}, `CQP` {qp}, `VBR` {bitrate, maxrate}, `QVBR` {qp, bitrate, maxrate}. `rate_control_modified` toggles visibility of `qp`, `bitrate`, `maxrate` per table. An unknown or empty name yields the table's NUL sentinel (name NULL, all flags false) and so hides all three.
- **D-3** `vaapi_update` calls `strcmp(rc_mode->name, "CBR")` on that sentinel: the reference **crashes** for an unknown/empty `rate_control` (vector 51, forked children). Required: unknown mode behaves as `CBR` (the code's own `rc_mode ? rc_mode : RC_MODES` fallback intent).
- **S-050-R51** `vaapi_update` mapping: `qp = rc.qp ? qp : 0`; `global_quality = AV1 ? qp*5 : qp`; `bitrate = rc.bitrate ? bitrate : 0`; `maxrate = rc.maxrate ? maxrate : 0`; CBR: `maxrate := bitrate`; modes with maxrate: `maxrate != 0 && maxrate < bitrate` -> 0. `bit_rate = bitrate*1000`, `rc_max_rate = maxrate*1000`, `rc_initial_buffer_occupancy = (maxrate ? maxrate : bitrate)*1000`, `profile`, `level`, `max_b_frames = bf`, size = encoder size, `time_base = fps_den/fps_num`, `gop_size = keyint_sec > 0 ? keyint_sec*fps_num/fps_den : 120`. HEVC `profile == Main` with a P010 output -> Main10 (warning). Pixel format from `video_info` (R55). Colour: 601 -> (SMPTE170M, SMPTE170M, SMPTE170M); default/709 -> (BT709 x3); sRGB -> (BT709, IEC61966_2_1, BT709); PQ -> (BT2020, SMPTE2084, BT2020_NCL); HLG -> (BT2020, ARIB_STD_B67, BT2020_NCL); `color_range` JPEG iff full; chroma location from the pix fmt and matrix. User `ffmpeg_opts` are passed to `av_opt_set` one by one. The info log "settings:" follows.
- **S-050-R52** Init chain (`vaapi_init_codec`), each failure logs a warning and fails create (encoder destroyed): `av_hwdevice_ctx_create(VAAPI, device)` (device path from `vaapi_device`), `av_hwframe_ctx_alloc`, frames init (format VAAPI, sw_format = pix_fmt, size), `av_frame_alloc`, `av_frame_get_buffer`, `avcodec_open2` with `pix_fmt = VAAPI` and the frames context. Encoder not found (`h264_vaapi`/`hevc_vaapi`/`av1_vaapi`) -> warning and NULL.
- **S-050-R53** Defaults: `vaapi_device` (per-codec default device), `profile` (HEVC Main, H.264 High, AV1 Main), `level` unknown(-99), `bitrate 6000`, `keyint_sec 0`, `bf 0`, `qp 20`, `maxrate 0`; `rate_control` only if the device opens: CBR if supported, else VBR, else CQP (probe at the codec's profile).
- **S-050-R54** `vaapi_device_modified`: clears the `rate_control` list; if the device does not open or the codec/profile is unsupported, leaves it empty; otherwise adds `CBR`, `VBR`, `QVBR`, `CQP` (in that order) for the supported modes and sets `bf` visible iff b-frames are supported. Always returns true.
- **S-050-R55** `video_info`: H.264 accepts NV12 only; HEVC/AV1 NV12 and P010; preferred if valid, else input if valid, else NV12.
- **S-050-R56** Packets: `send_frame` OK or EAGAIN then `receive_packet`; EAGAIN or EOF -> no error and no packet; other error -> warning + false. A packet with `size > 0`: first one -> codec header extractor (header/sei kept, AV1: header only, the data is returned **unchanged** because the AV1 extractor does not strip it), later ones are copied; `pts/dts` from the packet; keyframe by the codec's detector (S-012). Empty packets leave the encoder "first".
- **S-050-R57** Registration: `ffmpeg_vaapi` (INTERNAL, CPU copy) and `ffmpeg_vaapi_tex` (PASS_TEXTURE) for H.264; `av1_ffmpeg_vaapi[_tex]`; `hevc_ffmpeg_vaapi[_tex]`.

## 8. Concurrency

NVENC `init_nvenc` is guarded by a process mutex; the QSV library calls (`open`, `close`, `encode`) are serialised by one global mutex. Encoders are otherwise single-threaded per instance (libobs calls `encode` from one thread). Vectors are single-threaded.

## 9. AMF (rules from reading only, **unverified, no vectors**)

`texture-amf.cpp` needs the AMD AMF SDK (absent). Read-only findings: defaults `rate_control "CBR"`, `bitrate 6000`, `cqp 20`, `preset "quality"`, `profile "high"`, `bf 2`; rate-control names (case-insensitive) `cqp, cbr, vbr, vbr_lat, qvbr, hqvbr, hqcbr` -> CONSTANT_QP, CBR, PEAK_CONSTRAINED_VBR, LATENCY_CONSTRAINED_VBR, QUALITY_VBR, HIGH_QUALITY_VBR, HIGH_QUALITY_CBR; unknown -> CBR. `bitrate` and `cqp` visibility: `bitrate` hidden for CQP and QVBR, `cqp` shown for CQP and QVBR. For modes other than CQP/QVBR: target = peak = VBV = bitrate; CBR adds filler data; PEAK_CONSTRAINED_VBR and HQ_VBR use peak = VBV = bitrate*1.5; LATENCY_CONSTRAINED_VBR uses VBV = (bitrate / (fps_num/fps_den)) * 1.1. CQP/QVBR set QP_I/QP_P to `cqp`. User options (`texture-amf-opts.hpp`) are matched by exact name (`g`/`keyint`, `usage`, `profile`, `level`, `quality`, `rc`, `enforce_hrd`, `filler_data`, `vbaq`, `qp_i`, `qp_p`, `me_half_pel`, `me_quarter_pel`, `aud`, `max_au_size`, plus codec-specific names); booleans parse `"1"` true, `"0"` false, then `true`/`false` case-insensitive, anything else false. A re-implementation needs a vendor-SDK fake to verify these.

## 10. Reference defects and quirks (index)

| id | Where | Required behaviour |
|---|---|---|
| Q-1 | nvenc rate-control matching: `strcmp` for `CQP`/`lossless`/H.264 buffering-period SEI `"CBR"`, `astrcmpi` elsewhere; default is lowercase `cbr` | match (SHOULD) |
| Q-2 | `init_nvenc` failure is sticky and silent after the first call | match |
| Q-3 | lookahead dropped by a negative bound leaves the preset's lookahead bits | match |
| Q-4 | H.264/HEVC/AV1 `idrPeriod` copied before user options | match |
| Q-5 | H.264 sets no HDR VUI although `mdi`/`cll` are allocated | match |
| Q-6 | ROI cache by increment (nvenc: first map always built; qsv: increment 0 never sent) | match |
| Q-7 | `get_sei_data` of obs-nvenc always false | match |
| D-1 | `bitrate*1000` in signed int (vector 26, reported by UBSan) | u32 modular |
| D-2 | `driver_ver` missing -> NULL `sscanf` crash (vector 32 defect) | treat as 0.0 |
| D-3 | VAAPI unknown rate control -> `strcmp(NULL)` crash (vector 51) | treat as CBR |
| D-4 | empty sequence header -> `bmalloc(0)` | store empty, report no extra data |

## 11. Implementation notes

Model the vendor SDKs behind traits (session, capability query, preset config, encode, lock) so the rules above become unit tests with a fake SDK; keep all arithmetic on the exact integer widths stated (u32 bitrates, u16 lookahead depth, u8 target quality, i8 ROI values). Do not depend on the numeric values of SDK enums: vectors print symbolic names.

## 12. Coverage

Scripts live in `test/conformance/scripts/hwencoders/`; the runner command language is documented in `test/conformance/README.md` ("hwencoders runner").

| Rule | Scripts |
|---|---|
| R01-R06 | 30, 31-*, 32-* |
| R07-R10 | 20, 21, 25, 28 |
| R11 | 10 |
| R12-R16 | 11, 12, 13, 14, 15, 16, 26, 29 |
| R17-R20 | 17, 18, 19, 25, 29 |
| R21 | 22 |
| R22-R26 | 23, 24, 27, 28 |
| R27-R28 | 33-*, 34-* |
| R30-R35 | 60-66 |
| R40-R46 | 40-45 |
| R50-R57 | 50-57 |
| D-2 / D-3 | 32-...-defect-missing-driver-ver / 51 |

## 13. Open questions

1. NVENC SDK < 13 (pre-12.2 field names) branches are not verified.
2. `config_open_string` rejection of the helper output could not be reproduced.
3. `ENABLE_NVENC_FFMPEG_IDS` (extra legacy ids) is a build option; behaviour is by reading only.
4. QSV texture-encode (`obs_qsv_encode_tex` success path with a real texture) and the VAAPI/NVENC texture backends are not covered.
5. Whether signed-overflow in D-1 can actually trap on any supported compiler (it did not change the observed output).
