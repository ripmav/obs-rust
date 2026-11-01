/*
 * Conformance harness (area encopts): wrapper around the real plugins/obs-qsv11/obs-qsv11.c
 * (settings translation only) with a recording fake of the QuickSync encoder library
 * (qsv_encoder_*). Test tooling.
 */
#include "../../../plugins/obs-qsv11/obs-qsv11.c"
#include "encopts_api.h"

static int g_ctx_dummy;

/* defined by the real plugin's platform code; faked here (only is_dgpu is consulted) */
struct adapter_info adapters[MAX_ADAPTERS];
size_t adapter_count;
size_t adapter_index;

enum qsv_cpu_platform qsv_get_cpu_platform()
{
	return (enum qsv_cpu_platform)g_host.cpu_platform;
}

qsv_t *qsv_encoder_open(qsv_param_t *p, enum qsv_codec codec, bool tex)
{
	(void)p;
	printf("qsv.encoder_open codec=%d tex=%d\n", (int)codec, tex ? 1 : 0);
	return (qsv_t *)&g_ctx_dummy;
}

int qsv_encoder_close(qsv_t *c)
{
	(void)c;
	printf("qsv.encoder_close\n");
	return 0;
}

void qsv_encoder_version(unsigned short *major, unsigned short *minor)
{
	*major = 2;
	*minor = 9;
}

int qsv_encoder_headers(qsv_t *c, uint8_t **sps, uint8_t **pps, uint16_t *nsps, uint16_t *npps)
{
	(void)c, (void)sps, (void)pps;
	*nsps = *npps = 0;
	return 0;
}

int qsv_hevc_encoder_headers(qsv_t *c, uint8_t **vps, uint8_t **sps, uint8_t **pps, uint16_t *nvps, uint16_t *nsps,
			     uint16_t *npps)
{
	(void)c, (void)vps, (void)sps, (void)pps;
	*nvps = *nsps = *npps = 0;
	return 0;
}

int qsv_encoder_reconfig(qsv_t *c, qsv_param_t *p)
{
	(void)c;
	printf("qsv.encoder_reconfig target_bitrate=%d\n", (int)p->nTargetBitRate);
	return 1; /* the plugin treats nonzero as success */
}

void qsvw_defaults(int kind, obs_data_t *s)
{
	switch (kind) {
	case 0:
		obs_qsv_defaults_h264_v1(s);
		break;
	case 1:
		obs_qsv_defaults_h264_v2(s);
		break;
	case 2:
		obs_qsv_defaults_av1(s);
		break;
	default:
		obs_qsv_defaults_hevc(s);
	}
}

obs_properties_t *qsvw_props(int kind)
{
	switch (kind) {
	case 0:
		return obs_qsv_props_h264(NULL);
	case 1:
		return obs_qsv_props_h264_v2(NULL);
	case 2:
		return obs_qsv_props_av1(NULL);
	default:
		return obs_qsv_props_hevc(NULL);
	}
}

void *qsvw_create(int kind, obs_data_t *s)
{
	static const enum qsv_codec codecs[] = {QSV_CODEC_AVC, QSV_CODEC_AVC, QSV_CODEC_AV1, QSV_CODEC_HEVC};
	return obs_qsv_create(codecs[kind], s, (obs_encoder_t *)&g_host, false);
}

bool qsvw_update(void *h, obs_data_t *s)
{
	return obs_qsv_update(h, s);
}

void qsvw_destroy(void *h)
{
	obs_qsv_destroy(h);
}

void qsvw_migrate(obs_data_t *s)
{
	update_latency(s);
	update_targetusage(s);
	update_ratecontrol(s);
}

void qsvw_print_params(void *h)
{
	const qsv_param_t *p = &((struct obs_qsv *)h)->params;
	printf("qsvparams usage=%d profile=%d rc=%d async=%d la=%d\n", p->nTargetUsage, p->nCodecProfile,
	       p->nRateControl, p->nAsyncDepth, p->nLADEPTH);
	printf("qsvparams bitrate=%d max=%d qpi=%d qpp=%d qpb=%d icq=%d keyint_sec=%d bframes=%d\n", p->nTargetBitRate,
	       p->nMaxBitRate, p->nQPI, p->nQPP, p->nQPB, p->nICQQuality, p->nKeyIntSec, p->nbFrames);
	printf("qsvparams size=%dx%d fps=%d/%d repeat_headers=%d fmt10=%d\n", p->nWidth, p->nHeight, p->nFpsNum,
	       p->nFpsDen, (int)p->bRepeatHeaders, (int)p->video_fmt_10bit);
	printf("qsvparams vui fmt=%d fullrange=%d prim=%d trc=%d matrix=%d chroma=%d/%d\n", p->VideoFormat,
	       p->VideoFullRange, p->ColourPrimaries, p->TransferCharacteristics, p->MatrixCoefficients,
	       p->ChromaSampleLocTypeTopField, p->ChromaSampleLocTypeBottomField);
	printf("qsvparams hdr dispx=%d,%d,%d dispy=%d,%d,%d white=%d,%d maxlum=%u minlum=%u cll=%d fall=%d\n",
	       p->DisplayPrimariesX[0], p->DisplayPrimariesX[1], p->DisplayPrimariesX[2], p->DisplayPrimariesY[0],
	       p->DisplayPrimariesY[1], p->DisplayPrimariesY[2], p->WhitePointX, p->WhitePointY,
	       p->MaxDisplayMasteringLuminance, p->MinDisplayMasteringLuminance, p->MaxContentLightLevel,
	       p->MaxPicAverageLightLevel);
}

void qsvw_video_info(int kind, struct video_scale_info *info)
{
	struct obs_qsv tmp;
	memset(&tmp, 0, sizeof(tmp));
	tmp.encoder = (obs_encoder_t *)&g_host;
	adapter_index = 0;
	adapters[0].is_dgpu = g_host.qsv_dgpu != 0;
	if (kind >= 2)
		obs_qsv_video_plus_hdr_info(&tmp, info);
	else
		obs_qsv_video_info(&tmp, info);
}
