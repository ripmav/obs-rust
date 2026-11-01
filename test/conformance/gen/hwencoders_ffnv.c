/*
 * Conformance harness (area hwencoders, spec S-050): wrapper for plugins/obs-ffmpeg/obs-ffmpeg-nvenc.c
 * (NVENC through libavcodec's h264_nvenc / hevc_nvenc). The real source is #included; libav
 * HEADERS are the real ones, but every libav call is a recording fake (av_opt_set*, av_strerror) and
 * the shared ffmpeg_video_encoder_* layer (specified by S-047/S-025) is a recorded call boundary.
 * Test tooling, not part of OBS.
 */
#define ENABLE_HEVC 1
/* the plugin exports these names, and obs-nvenc/nvenc-properties.c does too: rename in this TU */
#define h264_nvenc_defaults ff_h264_nvenc_defaults
#define hevc_nvenc_defaults ff_hevc_nvenc_defaults
#define av1_nvenc_defaults ff_av1_nvenc_defaults
#define nvenc_properties_internal ff_nvenc_properties_internal
#include "hwencoders_api.h"
#include "../../../plugins/obs-ffmpeg/obs-ffmpeg-nvenc.c"
#include <stdio.h>
#include "script.h"

/* ---- libav fakes ---- */
int av_opt_set_int(void *obj, const char *name, int64_t val, int flags)
{
	(void)obj, (void)flags;
	printf("av.opt_set_int %s=%lld\n", name, (long long)val);
	return 0;
}

/* ---- fake of the shared ffmpeg video encoder layer ---- */
static int g_priv;
int g_ff_init_codec_ret = 1;  /* result of init_codec: 1 ok, 0 fail */
int g_ff_init_error = 0;      /* error code reported to on_init_error when init_codec fails */
int g_ff_init_fail_once = 0;  /* fail only the first init_codec call (psycho-aq retry) */
static int g_init_calls;
static int64_t g_pkt_dts;
static uint8_t g_pkt_data[256];
static int g_pkt_size;
int g_ff_first_dts_expect;

bool ffmpeg_video_encoder_init(struct ffmpeg_video_encoder *enc, void *parent, obs_encoder_t *encoder,
			       const char *lib, const char *lib2, const char *name, init_error_cb on_err,
			       first_packet_cb on_first)
{
	printf("ffve.init lib=%s lib2=%s name=\"%s\"\n", lib, lib2, name);
	enc->encoder = encoder;
	enc->enc_name = name;
	enc->parent = parent;
	enc->on_init_error = on_err;
	enc->on_first_packet = on_first;
	enc->context = bzalloc(sizeof(AVCodecContext));
	enc->context->priv_data = &g_priv;
	enc->context->time_base.num = 1;
	enc->first_packet = true;
	return true;
}
void ffmpeg_video_encoder_free(struct ffmpeg_video_encoder *enc)
{
	printf("ffve.free\n");
	bfree(enc->context);
	da_free(enc->buffer);
}
void ffmpeg_video_encoder_update(struct ffmpeg_video_encoder *enc, int bitrate, int keyint_sec,
				 const struct video_output_info *voi, const struct video_scale_info *info,
				 const char *opts)
{
	printf("ffve.update bitrate=%d keyint_sec=%d fps=%u/%u size=%ux%u info.format=%d opts=\"%s\" ctx{global_quality=%d "
	       "rc_max_rate=%lld rc_min_rate=%lld max_b_frames=%d}\n",
	       bitrate, keyint_sec, voi->fps_num, voi->fps_den, voi->width, voi->height, (int)info->format, opts,
	       enc->context->global_quality, (long long)enc->context->rc_max_rate,
	       (long long)enc->context->rc_min_rate, enc->context->max_b_frames);
	enc->context->width = voi->width;
	enc->height = voi->height;
	enc->context->gop_size = keyint_sec * (int)voi->fps_num / (int)voi->fps_den;
	enc->context->bit_rate = bitrate * 1000;
}
bool ffmpeg_video_encoder_init_codec(struct ffmpeg_video_encoder *enc)
{
	g_init_calls++;
	bool fail = !g_ff_init_codec_ret || (g_ff_init_fail_once && g_init_calls == 1);
	printf("ffve.init_codec #%d -> %s\n", g_init_calls, fail ? "fail" : "ok");
	if (fail && enc->on_init_error)
		enc->on_init_error(enc->parent, g_ff_init_error);
	return !fail;
}
bool ffmpeg_video_encode(struct ffmpeg_video_encoder *enc, struct encoder_frame *frame,
			 struct encoder_packet *packet, bool *received)
{
	(void)frame;
	AVPacket avp = {0};
	avp.data = g_pkt_data;
	avp.size = g_pkt_size;
	avp.dts = g_pkt_dts;
	if (enc->first_packet && enc->on_first_packet) {
		struct darray out;
		memset(&out, 0, sizeof(out));
		enc->on_first_packet(enc->parent, &avp, &out);
		enc->first_packet = false;
		/* the real layer copies the (header-stripped) data into the packet */
		packet->data = out.array;
		packet->size = out.num;
	}
	else {
		packet->data = g_pkt_data;
		packet->size = g_pkt_size;
	}
	packet->dts = g_pkt_dts;
	*received = true;
	return true;
}

void hw_ff_set_packet(const uint8_t *d, size_t n, int64_t dts)
{
	memcpy(g_pkt_data, d, n);
	g_pkt_size = (int)n;
	g_pkt_dts = dts;
}
void hw_ff_set_ctx_bframes_timebase(void *enc, int bf, int tb)
{
	struct nvenc_encoder *e = enc;
	e->ffve.context->max_b_frames = bf;
	e->ffve.context->time_base.num = tb;
}
void *hw_ff_create(int hevc, obs_data_t *s)
{
	g_init_calls = 0;
	return hevc ? hevc_nvenc_create(s, (obs_encoder_t *)&g_hw) : h264_nvenc_create(s, (obs_encoder_t *)&g_hw);
}
bool hw_ff_reconfigure(void *enc, obs_data_t *s)
{
	bool r = nvenc_reconfigure(enc, s);
	struct nvenc_encoder *e = enc;
	printf("reconfigure: bit_rate=%lld rc_max_rate=%lld\n", (long long)e->ffve.context->bit_rate,
	       (long long)e->ffve.context->rc_max_rate);
	return r;
}
void hw_ff_destroy(void *enc)
{
	nvenc_destroy(enc);
}
bool hw_ff_encode(void *enc, struct encoder_packet *p, bool *got)
{
	return nvenc_encode(enc, NULL, p, got);
}
void hw_ff_state(void *data)
{
	struct nvenc_encoder *e = data;
	printf("state: hevc=%d gpu=%d dts_offset=%lld header=", e->hevc, e->gpu, (long long)e->dts_offset);
	script_print_hex(e->header.array, e->header.num);
	printf(" sei=");
	script_print_hex(e->sei.array, e->sei.num);
	printf(" max_b_frames=%d\n", e->ffve.context->max_b_frames);
}
void hw_ff_video_info(int pref, int cur, int *out)
{
	struct nvenc_encoder fake;
	struct video_scale_info info = {.format = cur};
	g_hw.pref_format = pref;
	fake.ffve.encoder = (obs_encoder_t *)&g_hw;
	nvenc_video_info(&fake, &info);
	*out = info.format;
}
void hw_ff_register(void)
{
	obs_register_encoder(&h264_nvenc_encoder_info);
	obs_register_encoder(&hevc_nvenc_encoder_info);
}
void hw_ff_defaults(int codec, obs_data_t *s)
{
	nvenc_defaults_base((enum codec_type)codec, s);
}
