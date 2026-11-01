/*
 * Conformance harness (area hwencoders, spec S-050): wrapper for plugins/obs-nvenc/nvenc.c.
 * The real source is #included so that its static functions (init_encoder_*, nvenc_update,
 * get_encoded_packet, ...) run unmodified against a recording fake of the NVENC function
 * list. The CUDA / D3D / OpenGL backend functions are replaced by trivial stubs below.
 * Test tooling, not part of OBS.
 */
#define ENABLE_HEVC 1
#include "hwencoders_api.h"
#include "../../../plugins/obs-nvenc/nvenc.c"
#include <stdio.h>

int g_hw_dump = 2; /* 0 = none, 1 = compact, 2 = full */
#define NOISY if (g_hw_dump == 2)

/* ---- backend stubs (nvenc-cuda.c / nvenc-opengl.c are not linked) ---- */
CudaFunctions *cu;
bool init_cuda(obs_encoder_t *e)
{
	(void)e;
	return true;
}
bool cuda_ctx_init(struct nvenc_data *enc, obs_data_t *s, bool tex)
{
	(void)s, (void)tex;
	enc->cu_ctx = (CUcontext)0;
	NOISY printf("cuda.ctx_init\n");
	return true;
}
void cuda_ctx_free(struct nvenc_data *enc)
{
	(void)enc;
	NOISY printf("cuda.ctx_free\n");
}
bool cuda_init_surfaces(struct nvenc_data *enc)
{
	/* the real function creates one surface per bitstream buffer */
	enc->surface_format = NV_ENC_BUFFER_FORMAT_NV12;
	da_resize(enc->surfaces, enc->buf_count);
	memset(enc->surfaces.array, 0, sizeof(struct nv_cuda_surface) * enc->buf_count);
	NOISY printf("cuda.init_surfaces n=%u\n", enc->buf_count);
	return true;
}
void cuda_free_surfaces(struct nvenc_data *enc)
{
	(void)enc;
}
void cuda_opengl_free(struct nvenc_data *enc)
{
	(void)enc;
}
bool cuda_opengl_encode(void *d, struct encoder_texture *t, int64_t pts, uint64_t lk, uint64_t *nk,
			struct encoder_packet *p, bool *r)
{
	(void)d, (void)t, (void)pts, (void)lk, (void)nk, (void)p, (void)r;
	return true;
}
bool cuda_encode(void *d, struct encoder_frame *f, struct encoder_packet *p, bool *r)
{
	(void)d, (void)f, (void)p, (void)r;
	return true;
}
bool cuda_get_error_desc(CUresult r, const char **n, const char **d)
{
	(void)r, (void)n, (void)d;
	return false;
}
bool cuda_error_check(struct nvenc_data *enc, CUresult r, const char *f, const char *c)
{
	(void)enc, (void)r, (void)f, (void)c;
	return true;
}
void obs_cuda_load(void) {}
void obs_cuda_unload(void) {}

/* ---- fake NVENC function list ---- */

#define MAXCAP 8
static int g_caps[MAXCAP + 1];
static int g_status[16];
enum { ST_OPEN, ST_CAPS, ST_PRESET, ST_INIT, ST_RECONF, ST_BS, ST_ENC, ST_LOCK, ST_UNLOCK, ST_SEQ, ST_UNMAP, ST_DESTROY };
static const char *st_names[] = {"open", "caps", "preset", "init", "reconf", "bitstream", "encode",
				 "lock", "unlock", "seqparams", "unmap", "destroy"};
static NV_ENC_CONFIG g_preset_cfg;
static const char *g_errstr;
static uint8_t g_header[256];
static size_t g_header_n;
static struct {
	int type, size;
	int64_t pts;
} g_lockq[64];
static int g_lock_h, g_lock_t;
static int g_session_token;

void hw_nv_cap(const char *name, int v)
{
	static const char *n[] = {"", "10bit", "444", "dyn_bitrate", "lookahead", "temporal_aq"};
	for (int i = 1; i <= 5; i++)
		if (!strcmp(name, n[i]))
			g_caps[i] = v;
}
void hw_nv_status(const char *fn, int code)
{
	for (int i = 0; i < 12; i++)
		if (!strcmp(fn, st_names[i]))
			g_status[i] = code;
}
void hw_nv_set_errstr(const char *s)
{
	g_errstr = s && *s ? bstrdup(s) : NULL;
}
void hw_nv_set_header(const uint8_t *b, size_t n)
{
	memcpy(g_header, b, n);
	g_header_n = n;
}
void hw_nv_lockpic(int type, int64_t pts, int size)
{
	g_lockq[g_lock_t].type = type;
	g_lockq[g_lock_t].pts = pts;
	g_lockq[g_lock_t].size = size;
	g_lock_t++;
}
void hw_nv_preset_field(const char *key, int v)
{
	NV_ENC_RC_PARAMS *rc = &g_preset_cfg.rcParams;
	if (!strcmp(key, "enableLookahead"))
		rc->enableLookahead = v;
	else if (!strcmp(key, "lookaheadDepth"))
		rc->lookaheadDepth = v;
	else if (!strcmp(key, "disableIadapt"))
		rc->disableIadapt = v;
	else if (!strcmp(key, "disableBadapt"))
		rc->disableBadapt = v;
	else if (!strcmp(key, "enableAQ"))
		rc->enableAQ = v;
	else if (!strcmp(key, "aqStrength"))
		rc->aqStrength = v;
	else if (!strcmp(key, "targetQuality"))
		rc->targetQuality = v;
	else if (!strcmp(key, "gopLength"))
		g_preset_cfg.gopLength = v;
	else if (!strcmp(key, "frameIntervalP"))
		g_preset_cfg.frameIntervalP = v;
	else if (!strcmp(key, "qpMapMode"))
		rc->qpMapMode = v;
	else if (!strcmp(key, "multiPass"))
		rc->multiPass = v;
	else if (!strcmp(key, "rateControlMode"))
		rc->rateControlMode = v;
	else
		printf("fake: unknown preset field %s\n", key);
}

const char *hw_nv_guid_name(const GUID *g)
{
#define G(x)                      \
	if (g->Data1 == x.Data1) \
		return #x;
	G(NV_ENC_CODEC_H264_GUID) G(NV_ENC_CODEC_HEVC_GUID) G(NV_ENC_CODEC_AV1_GUID) G(NV_ENC_PRESET_P1_GUID)
	G(NV_ENC_PRESET_P2_GUID) G(NV_ENC_PRESET_P3_GUID) G(NV_ENC_PRESET_P4_GUID) G(NV_ENC_PRESET_P5_GUID)
	G(NV_ENC_PRESET_P6_GUID) G(NV_ENC_PRESET_P7_GUID) G(NV_ENC_H264_PROFILE_BASELINE_GUID)
	G(NV_ENC_H264_PROFILE_MAIN_GUID) G(NV_ENC_H264_PROFILE_HIGH_GUID) G(NV_ENC_H264_PROFILE_HIGH_10_GUID)
	G(NV_ENC_H264_PROFILE_HIGH_444_GUID) G(NV_ENC_HEVC_PROFILE_MAIN_GUID) G(NV_ENC_HEVC_PROFILE_MAIN10_GUID)
	G(NV_ENC_HEVC_PROFILE_FREXT_GUID) G(NV_ENC_AV1_PROFILE_MAIN_GUID)
#undef G
	return g->Data1 == 0 ? "(zero)" : "(unknown)";
}

static const char *tuning_name(int t)
{
	static const char *n[] = {"UNDEFINED", "HIGH_QUALITY", "LOW_LATENCY", "ULTRA_LOW_LATENCY", "LOSSLESS",
				  "ULTRA_HIGH_QUALITY"};
	return t >= 0 && t < 6 ? n[t] : "?";
}
static const char *rc_name(int m)
{
	return m == NV_ENC_PARAMS_RC_CONSTQP ? "CONSTQP" : m == NV_ENC_PARAMS_RC_VBR ? "VBR" : "CBR";
}
static const char *mp_name(int m)
{
	return m == 0 ? "DISABLED" : m == 1 ? "QUARTER" : "FULL";
}

static NVENCSTATUS f_open(NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS *p, void **session)
{
	NOISY printf("nv.OpenEncodeSessionEx deviceType=%s apiVersion=%u.%u\n",
	       p->deviceType == NV_ENC_DEVICE_TYPE_CUDA ? "CUDA" : "DIRECTX", p->apiVersion & 0xffffff,
	       p->apiVersion >> 24);
	if (g_status[ST_OPEN])
		return g_status[ST_OPEN];
	*session = &g_session_token;
	return 0;
}
static NVENCSTATUS f_caps(void *s, GUID g, NV_ENC_CAPS_PARAM *p, int *v)
{
	(void)s, (void)g;
	*v = g_caps[p->capsToQuery];
	return g_status[ST_CAPS];
}
static NVENCSTATUS f_preset(void *s, GUID codec, GUID preset, NV_ENC_TUNING_INFO t, NV_ENC_PRESET_CONFIG *pc)
{
	(void)s;
	NOISY printf("nv.GetEncodePresetConfigEx codec=%s preset=%s tuning=%s\n", hw_nv_guid_name(&codec),
	       hw_nv_guid_name(&preset), tuning_name(t));
	if (g_status[ST_PRESET])
		return g_status[ST_PRESET];
	pc->presetCfg = g_preset_cfg;
	return 0;
}

static void dump_cfg(const NV_ENC_INITIALIZE_PARAMS *p, int codec_is)
{
	const NV_ENC_CONFIG *c = p->encodeConfig;
	const NV_ENC_RC_PARAMS *rc = &c->rcParams;
	printf("  params: codec=%s preset=%s tuning=%s size=%ux%u dar=%ux%u fps=%u/%u async=%u ptd=%u split=%d\n",
	       hw_nv_guid_name(&p->encodeGUID), hw_nv_guid_name(&p->presetGUID), tuning_name(p->tuningInfo),
	       p->encodeWidth, p->encodeHeight, p->darWidth, p->darHeight, p->frameRateNum, p->frameRateDen,
	       p->enableEncodeAsync, p->enablePTD, (int)p->splitEncodeMode);
	printf("  config: profile=%s gop=%u frameIntervalP=%d\n", hw_nv_guid_name(&c->profileGUID), c->gopLength,
	       c->frameIntervalP);
	printf("  rc: mode=%s avg=%u max=%u vbv=%u vbvInit=%u tq=%u cqP=%u cqB=%u cqI=%u aq=%u aqStrength=%u taq=%u\n",
	       rc_name(rc->rateControlMode), rc->averageBitRate, rc->maxBitRate, rc->vbvBufferSize,
	       rc->vbvInitialDelay, rc->targetQuality, rc->constQP.qpInterP, rc->constQP.qpInterB,
	       rc->constQP.qpIntra, rc->enableAQ, rc->aqStrength, rc->enableTemporalAQ);
	printf("  rc: lookahead=%u depth=%u iadapt_off=%u badapt_off=%u multipass=%s qpmap=%d\n", rc->enableLookahead,
	       rc->lookaheadDepth, rc->disableIadapt, rc->disableBadapt, mp_name(rc->multiPass),
	       (int)rc->qpMapMode);
	if (codec_is == CODEC_H264) {
		const NV_ENC_CONFIG_H264 *h = &c->encodeCodecConfig.h264Config;
		const NV_ENC_CONFIG_H264_VUI_PARAMETERS *v = &h->h264VUIParameters;
		printf("  h264: level=%u idr=%u bref=%d filler=%u repeatSPSPPS=%u disableSPSPPS=%u aud=%u bufSEI=%u "
		       "timingSEI=%u lossless=%u slice=%u/%u chroma=%u inDepth=%d outDepth=%d\n",
		       h->level, h->idrPeriod, (int)h->useBFramesAsRef, h->enableFillerDataInsertion,
		       h->repeatSPSPPS, h->disableSPSPPS, h->outputAUD, h->outputBufferingPeriodSEI,
		       h->outputPictureTimingSEI, h->qpPrimeYZeroTransformBypassFlag, h->sliceMode,
		       h->sliceModeData, h->chromaFormatIDC, (int)h->inputBitDepth, (int)h->outputBitDepth);
		printf("  vui: signal=%u full=%u colour=%u prim=%u trc=%u matrix=%u\n",
		       v->videoSignalTypePresentFlag, v->videoFullRangeFlag, v->colourDescriptionPresentFlag,
		       v->colourPrimaries, v->transferCharacteristics, v->colourMatrix);
	} else if (codec_is == CODEC_HEVC) {
		const NV_ENC_CONFIG_HEVC *h = &c->encodeCodecConfig.hevcConfig;
		const NV_ENC_CONFIG_HEVC_VUI_PARAMETERS *v = &h->hevcVUIParameters;
		printf("  hevc: level=%u tier=%u idr=%u bref=%d filler=%u repeatSPSPPS=%u disableSPSPPS=%u aud=%u "
		       "bufSEI=%u timingSEI=%u mdi=%u cll=%u slice=%u/%u chroma=%u inDepth=%d outDepth=%d\n",
		       h->level, h->tier, h->idrPeriod, (int)h->useBFramesAsRef, h->enableFillerDataInsertion,
		       h->repeatSPSPPS, h->disableSPSPPS, h->outputAUD, h->outputBufferingPeriodSEI,
		       h->outputPictureTimingSEI, h->outputMasteringDisplay, h->outputMaxCll, h->sliceMode,
		       h->sliceModeData, h->chromaFormatIDC, (int)h->inputBitDepth, (int)h->outputBitDepth);
		printf("  vui: signal=%u full=%u colour=%u prim=%u trc=%u matrix=%u chromaLoc=%u/%u/%u\n",
		       v->videoSignalTypePresentFlag, v->videoFullRangeFlag, v->colourDescriptionPresentFlag,
		       v->colourPrimaries, v->transferCharacteristics, v->colourMatrix, v->chromaSampleLocationFlag,
		       v->chromaSampleLocationTop, v->chromaSampleLocationBot);
	} else {
		const NV_ENC_CONFIG_AV1 *a = &c->encodeCodecConfig.av1Config;
		printf("  av1: level=%u tier=%u tiles=%ux%u idr=%u bref=%d padding=%u repeatSeqHdr=%u mdi=%u cll=%u "
		       "range=%u prim=%u trc=%u matrix=%u chroma=%u inDepth=%d outDepth=%d refs=%u/%u\n",
		       a->level, a->tier, a->numTileColumns, a->numTileRows, a->idrPeriod, (int)a->useBFramesAsRef,
		       a->enableBitstreamPadding, a->repeatSeqHdr, a->outputMasteringDisplay, a->outputMaxCll,
		       a->colorRange, a->colorPrimaries, a->transferCharacteristics, a->matrixCoefficients,
		       a->chromaFormatIDC, (int)a->inputBitDepth, (int)a->outputBitDepth, a->numFwdRefs,
		       a->numBwdRefs);
	}
}
static int g_cur_codec;

static NVENCSTATUS f_init(void *s, NV_ENC_INITIALIZE_PARAMS *p)
{
	(void)s;
	printf("nv.InitializeEncoder\n");
	if (g_hw_dump == 2) {
		dump_cfg(p, g_cur_codec);
	} else if (g_hw_dump == 1) {
		const NV_ENC_CONFIG *c = p->encodeConfig;
		const NV_ENC_RC_PARAMS *rc = &c->rcParams;
		printf("  compact: preset=%s tuning=%s profile=%s gop=%u P=%d rc=%s avg=%u max=%u vbv=%u tq=%u qp=%u aq=%u la=%u/%u iadapt_off=%u mp=%s\n",
		       hw_nv_guid_name(&p->presetGUID), tuning_name(p->tuningInfo), hw_nv_guid_name(&c->profileGUID),
		       c->gopLength, c->frameIntervalP, rc_name(rc->rateControlMode), rc->averageBitRate,
		       rc->maxBitRate, rc->vbvBufferSize, rc->targetQuality, rc->constQP.qpInterP, rc->enableAQ,
		       rc->enableLookahead, rc->lookaheadDepth, rc->disableIadapt, mp_name(rc->multiPass));
	}
	return g_status[ST_INIT];
}
static NVENCSTATUS f_reconf(void *s, NV_ENC_RECONFIGURE_PARAMS *p)
{
	(void)s;
	const NV_ENC_RC_PARAMS *rc = &p->reInitEncodeParams.encodeConfig->rcParams;
	printf("nv.ReconfigureEncoder reset=%u forceIDR=%u avg=%u max=%u tq=%u\n", p->resetEncoder, p->forceIDR,
	       rc->averageBitRate, rc->maxBitRate, rc->targetQuality);
	return g_status[ST_RECONF];
}
static NVENCSTATUS f_cbs(void *s, NV_ENC_CREATE_BITSTREAM_BUFFER *b)
{
	(void)s;
	static int n;
	b->bitstreamBuffer = (void *)(uintptr_t)(0x1000 + 16 * ++n);
	return g_status[ST_BS];
}
static NVENCSTATUS f_dbs(void *s, NV_ENC_OUTPUT_PTR p)
{
	(void)s, (void)p;
	NOISY printf("nv.DestroyBitstreamBuffer\n");
	return 0;
}
static NVENCSTATUS f_encode(void *s, NV_ENC_PIC_PARAMS *p)
{
	(void)s;
	if (p->encodePicFlags & NV_ENC_PIC_FLAG_EOS) {
		printf("nv.EncodePicture EOS\n");
		return 0;
	}
	printf("nv.EncodePicture ts=%llu frameIdx=%u in=%ux%u pitch=%u fmt=%s", (unsigned long long)p->inputTimeStamp,
	       p->frameIdx, p->inputWidth, p->inputHeight, p->inputPitch,
	       p->bufferFmt == NV_ENC_BUFFER_FORMAT_NV12 ? "NV12" : p->bufferFmt == NV_ENC_BUFFER_FORMAT_YUV420_10BIT ? "YUV420_10BIT" : "OTHER");
	if (p->qpDeltaMap) {
		printf(" qpmap[%u]=", p->qpDeltaMapSize);
		for (uint32_t i = 0; i < p->qpDeltaMapSize; i++)
			printf("%s%d", i ? "," : "", p->qpDeltaMap[i]);
	}
	if (p->codecPicParams.hevcPicParams.pMaxCll) {
		/* hevc and av1 pic params share the same layout in the shim; the plugin sets one by codec */
		printf(" cll=%u/%u", p->codecPicParams.hevcPicParams.pMaxCll->maxContentLightLevel,
		       p->codecPicParams.hevcPicParams.pMaxCll->maxPicAverageLightLevel);
	}
	if (p->codecPicParams.hevcPicParams.pMasteringDisplay) {
		MASTERING_DISPLAY_INFO *m = p->codecPicParams.hevcPicParams.pMasteringDisplay;
		printf(" mdi=r%u,%u g%u,%u b%u,%u w%u,%u luma%u,%u", m->r.x, m->r.y, m->g.x, m->g.y, m->b.x, m->b.y,
		       m->whitePoint.x, m->whitePoint.y, m->maxLuma, m->minLuma);
	}
	printf("\n");
	return g_status[ST_ENC];
}
static uint8_t g_bits[4096];
static NVENCSTATUS f_lock(void *s, NV_ENC_LOCK_BITSTREAM *l)
{
	(void)s;
	if (g_status[ST_LOCK])
		return g_status[ST_LOCK];
	if (g_lock_h >= g_lock_t) {
		printf("fake: lockpic queue empty\n");
		return NV_ENC_ERR_GENERIC;
	}
	int sz = g_lockq[g_lock_h].size;
	for (int i = 0; i < sz && i < 4096; i++)
		g_bits[i] = (uint8_t)(0xA0 + i);
	l->bitstreamBufferPtr = g_bits;
	l->bitstreamSizeInBytes = sz;
	l->outputTimeStamp = g_lockq[g_lock_h].pts;
	l->pictureType = g_lockq[g_lock_h].type;
	g_lock_h++;
	printf("nv.LockBitstream -> type=%d size=%d ts=%lld\n", (int)l->pictureType, sz, (long long)l->outputTimeStamp);
	return 0;
}
static NVENCSTATUS f_unlock(void *s, NV_ENC_OUTPUT_PTR p)
{
	(void)s, (void)p;
	return g_status[ST_UNLOCK];
}
static NVENCSTATUS f_seq(void *s, NV_ENC_SEQUENCE_PARAM_PAYLOAD *p)
{
	(void)s;
	printf("nv.GetSequenceParams bufsize=%u\n", p->inBufferSize);
	memcpy(p->spsppsBuffer, g_header, g_header_n);
	*p->outSPSPPSPayloadSize = (uint32_t)g_header_n;
	return g_status[ST_SEQ];
}
static NVENCSTATUS f_unmap(void *s, NV_ENC_INPUT_PTR p)
{
	(void)s, (void)p;
	return g_status[ST_UNMAP];
}
static NVENCSTATUS f_destroy(void *s)
{
	(void)s;
	printf("nv.DestroyEncoder\n");
	return g_status[ST_DESTROY];
}
static const char *f_errstr(void *s)
{
	(void)s;
	return g_errstr;
}

int hw_nv_create_instance(void *list)
{
	NV_ENCODE_API_FUNCTION_LIST *l = list;
	l->nvEncOpenEncodeSessionEx = f_open;
	l->nvEncGetEncodeCaps = f_caps;
	l->nvEncGetEncodePresetConfigEx = f_preset;
	l->nvEncInitializeEncoder = f_init;
	l->nvEncReconfigureEncoder = f_reconf;
	l->nvEncCreateBitstreamBuffer = f_cbs;
	l->nvEncDestroyBitstreamBuffer = f_dbs;
	l->nvEncEncodePicture = f_encode;
	l->nvEncLockBitstream = f_lock;
	l->nvEncUnlockBitstream = f_unlock;
	l->nvEncGetSequenceParams = f_seq;
	l->nvEncUnmapInputResource = f_unmap;
	l->nvEncDestroyEncoder = f_destroy;
	l->nvEncGetLastErrorString = f_errstr;
	return 0;
}

/* ---- entry points used by the runner ---- */

void *hw_nv_create(int codec, bool tex, obs_data_t *s)
{
	g_cur_codec = codec;
	return nvenc_create_base((enum codec_type)codec, s, (obs_encoder_t *)&g_hw, tex);
}
bool hw_nv_update(void *enc, obs_data_t *s)
{
	return nvenc_update(enc, s);
}
void hw_nv_destroy(void *enc)
{
	nvenc_destroy(enc);
}
bool hw_nv_encode(void *data, int64_t pts, struct encoder_packet *pkt, bool *got)
{
	struct nvenc_data *enc = data;
	struct nv_bitstream *bs = &enc->bitstreams.array[enc->next_bitstream];
	/* what cuda_encode()/d3d11_encode()/cuda_opengl_encode() do before calling the shared path */
	deque_push_back(&enc->dts_list, &pts, sizeof(pts));
	return nvenc_encode_base(enc, bs, (void *)(uintptr_t)0x77, pts, pkt, got);
}
void hw_nv_state(void *data)
{
	struct nvenc_data *enc = data;
	printf("state: codec=%s cx=%u cy=%u in_format=%d buf_count=%u output_delay=%d can_change_bitrate=%d "
	       "non_texture=%d queued=%d header_size=%zu bitstreams=%zu hdr_mdi=%d hdr_cll=%d\n",
	       get_codec_name(enc->codec), enc->cx, enc->cy, (int)enc->in_format, enc->buf_count, enc->output_delay,
	       enc->can_change_bitrate, enc->non_texture, enc->buffers_queued, enc->header_size, enc->bitstreams.num,
	       enc->mdi != NULL, enc->cll != NULL);
	if (enc->mdi)
		printf("state: mdi r=%u,%u g=%u,%u b=%u,%u white=%u,%u maxLuma=%u minLuma=%u cll=%u/%u\n", enc->mdi->r.x,
		       enc->mdi->r.y, enc->mdi->g.x, enc->mdi->g.y, enc->mdi->b.x, enc->mdi->b.y,
		       enc->mdi->whitePoint.x, enc->mdi->whitePoint.y, enc->mdi->maxLuma, enc->mdi->minLuma,
		       enc->cll->maxContentLightLevel, enc->cll->maxPicAverageLightLevel);
	printf("state: props rate_control=%s preset=%s tune=%s multipass=%s profile=%s bf=%lld aq=%d\n",
	       enc->props.rate_control, enc->props.preset, enc->props.tune, enc->props.multipass, enc->props.profile,
	       (long long)enc->props.bf, enc->props.adaptive_quantization);
}
bool hw_nv_extra(void *enc, const uint8_t **p, size_t *n)
{
	uint8_t *h;
	bool r = nvenc_extra_data(enc, &h, n);
	*p = h;
	return r;
}
bool hw_nv_sei(void *enc)
{
	uint8_t *p;
	size_t n;
	return nvenc_sei_data(enc, &p, &n);
}
void hw_nv_register(void)
{
	register_encoders();
}
void hw_nv_defaults(int codec, obs_data_t *s)
{
	if (codec == 0)
		h264_nvenc_defaults(s);
	else if (codec == 1)
		hevc_nvenc_defaults(s);
	else
		av1_nvenc_defaults(s);
}
