/*
 * Conformance harness (area encopts): wrapper for plugins/obs-nvenc option handling
 * (nvenc-opts-parser.c, nvenc-properties.c are linked as separate translation units;
 * only their header is used here). The NVENC capability probe is faked. Test tooling.
 */
#include "../../../plugins/obs-nvenc/nvenc-internal.h"
#include "encopts_api.h"
#include "script.h"

static struct encoder_caps g_caps;

struct encoder_caps *get_encoder_caps(enum codec_type codec)
{
	(void)codec;
	return &g_caps;
}
int num_encoder_devices(void)
{
	return g_host.nv_devices;
}
bool is_codec_supported(enum codec_type codec)
{
	(void)codec;
	return g_host.nv_av1_supported;
}
bool has_broken_split_encoding(void)
{
	return g_host.nv_split_broken;
}

void nvw_set_caps(int bframes, int bref, int engines, int lossless, int uhq, int ten_bit, int lookahead)
{
	memset(&g_caps, 0, sizeof(g_caps));
	g_caps.bframes = bframes;
	g_caps.bref_modes = bref;
	g_caps.engines = engines;
	g_caps.lossless = lossless;
	g_caps.uhq = uhq;
	g_caps.ten_bit = ten_bit;
	g_caps.lookahead = lookahead;
}

void nvw_defaults(int codec, obs_data_t *s)
{
	if (codec == 0)
		h264_nvenc_defaults(s);
	else if (codec == 1)
		hevc_nvenc_defaults(s);
	else
		av1_nvenc_defaults(s);
}

obs_properties_t *nvw_props(int codec)
{
	return codec == 0 ? h264_nvenc_properties(NULL)
	       : codec == 1 ? hevc_nvenc_properties(NULL)
			    : av1_nvenc_properties(NULL);
}

static void q(const char *s)
{
	if (!s)
		fputs("@null", stdout);
	else
		script_print_quoted(s);
}

void nvw_read(obs_data_t *s)
{
	struct nvenc_properties p;
	memset(&p, 0, sizeof(p));
	nvenc_properties_read(&p, s);
	printf("nvprops bitrate=%lld max_bitrate=%lld keyint_sec=%lld cqp=%lld device=%lld bf=%lld\n",
	       (long long)p.bitrate, (long long)p.max_bitrate, (long long)p.keyint_sec, (long long)p.cqp,
	       (long long)p.device, (long long)p.bf);
	printf("nvprops bframe_ref_mode=%lld split_encode=%lld target_quality=%lld\n", (long long)p.bframe_ref_mode,
	       (long long)p.split_encode, (long long)p.target_quality);
	printf("nvprops rate_control=");
	q(p.rate_control);
	printf(" preset=");
	q(p.preset);
	printf(" profile=");
	q(p.profile);
	printf(" tune=");
	q(p.tune);
	printf(" multipass=");
	q(p.multipass);
	printf(" opts_str=");
	q(p.opts_str);
	putchar('\n');
	printf("nvprops aq=%d lookahead=%d disable_scenecut=%d repeat_headers=%d force_cuda_tex=%d\n",
	       p.adaptive_quantization, p.lookahead, p.disable_scenecut, p.repeat_headers, p.force_cuda_tex);
	printf("nvprops opts count=%d ignored=%d\n", (int)p.opts.count, (int)p.opts.ignored_word_count);
	obs_free_options(p.opts);
	obs_data_release(p.data); /* the reader took a reference */
}

static void print_qp(const char *n, const NV_ENC_QP *q_)
{
	printf(" %s=%u/%u/%u", n, q_->qpInterP, q_->qpInterB, q_->qpIntra);
}

void nvw_apply(int codec, obs_data_t *s)
{
	struct nvenc_data enc;
	memset(&enc, 0, sizeof(enc));
	enc.encoder = (obs_encoder_t *)&g_host;
	enc.codec = codec == 0 ? CODEC_H264 : codec == 1 ? CODEC_HEVC : CODEC_AV1;
	nvenc_properties_read(&enc.props, s);

	bool ok = apply_user_args(&enc);
	const NV_ENC_RC_PARAMS *rc = &enc.config.rcParams;
	printf("nv_apply success=%d\n", ok ? 1 : 0);
	printf("nvconfig gopLength=%u frameIntervalP=%d\n", enc.config.gopLength, enc.config.frameIntervalP);
	printf("nvrc avg=%u max=%u vbvbuf=%u vbvdelay=%u tq=%u tqlsb=%u cb=%d cr=%d", rc->averageBitRate, rc->maxBitRate,
	       rc->vbvBufferSize, rc->vbvInitialDelay, rc->targetQuality, rc->targetQualityLSB, rc->cbQPIndexOffset,
	       rc->crQPIndexOffset);
	printf(" lookaheadLevel=%u\n", (unsigned)rc->lookaheadLevel);
	printf("nvrc");
	print_qp("constQP", &rc->constQP);
	print_qp("minQP", &rc->minQP);
	print_qp("maxQP", &rc->maxQP);
	print_qp("initialRCQP", &rc->initialRCQP);
	putchar('\n');
	printf("nvrc bits minQP=%u maxQP=%u initialRCQP=%u aq=%u lookahead=%u disableIadapt=%u disableBadapt=%u "
	       "temporalAQ=%u aqStrength=%u\n",
	       rc->enableMinQP, rc->enableMaxQP, rc->enableInitialRCQP, rc->enableAQ, rc->enableLookahead,
	       rc->disableIadapt, rc->disableBadapt, rc->enableTemporalAQ, rc->aqStrength);
	if (codec == 0) {
		const NV_ENC_CONFIG_H264 *c = &enc.config.encodeCodecConfig.h264Config;
		printf("nvcodec h264 level=%u idrPeriod=%u useBFramesAsRef=%u tfLevel=%u filler=%u\n", c->level,
		       c->idrPeriod, (unsigned)c->useBFramesAsRef, (unsigned)c->tfLevel,
		       c->enableFillerDataInsertion);
	} else if (codec == 1) {
		const NV_ENC_CONFIG_HEVC *c = &enc.config.encodeCodecConfig.hevcConfig;
		printf("nvcodec hevc level=%u tier=%u idrPeriod=%u useBFramesAsRef=%u tfLevel=%u filler=%u\n",
		       c->level, c->tier, c->idrPeriod, (unsigned)c->useBFramesAsRef, (unsigned)c->tfLevel,
		       c->enableFillerDataInsertion);
	} else {
		const NV_ENC_CONFIG_AV1 *c = &enc.config.encodeCodecConfig.av1Config;
		printf("nvcodec av1 level=%u tier=%u tiles=%ux%u idrPeriod=%u useBFramesAsRef=%u tfLevel=%u padding=%u\n",
		       c->level, c->tier, c->numTileColumns, c->numTileRows, c->idrPeriod,
		       (unsigned)c->useBFramesAsRef, (unsigned)c->tfLevel, c->enableBitstreamPadding);
	}
	obs_free_options(enc.props.opts);
	obs_data_release(enc.props.data);
}

void nvw_userarg(obs_data_t *s, const char *name)
{
	struct nvenc_data enc;
	memset(&enc, 0, sizeof(enc));
	enc.encoder = (obs_encoder_t *)&g_host;
	nvenc_properties_read(&enc.props, s);
	int val = 7777;
	bool found = get_user_arg_int(&enc, name, &val);
	printf("nv_userarg %s found=%d value=%d\n", name, found ? 1 : 0, val);
	obs_free_options(enc.props.opts);
	obs_data_release(enc.props.data);
}
