/*
 * Conformance harness (area encopts): drives ffmpeg_video_encoder_update() of the real
 * plugins/obs-ffmpeg/obs-ffmpeg-video-encoders.c (linked as a separate translation unit)
 * against a calloc'ed AVCodecContext and a recording fake of av_opt_set. Test tooling.
 */
#include "../../../plugins/obs-ffmpeg/obs-ffmpeg-video-encoders.h"
#include "encopts_api.h"

static int g_priv;

/* Fake of libavutil's av_opt_set: records the call. Option names that start with
 * "nosuch" are reported as unknown (AVERROR_OPTION_NOT_FOUND); everything else succeeds. */
int av_opt_set(void *obj, const char *name, const char *val, int search_flags)
{
	(void)obj;
	int rc = strncmp(name, "nosuch", 6) == 0 ? AVERROR_OPTION_NOT_FOUND : 0;
	printf("av.opt_set %s=%s flags=%d -> %s\n", name, val, search_flags, rc ? "notfound" : "0");
	return rc;
}

static const char *pixname(int f)
{
	switch (f) {
	case AV_PIX_FMT_NV12:
		return "nv12";
	case AV_PIX_FMT_YUV420P:
		return "yuv420p";
	case AV_PIX_FMT_YUV444P:
		return "yuv444p";
	case AV_PIX_FMT_P010LE:
		return "p010le";
	case AV_PIX_FMT_BGRA:
		return "bgra";
	case AV_PIX_FMT_RGBA:
		return "rgba";
	case AV_PIX_FMT_YUV422P:
		return "yuv422p";
	case AV_PIX_FMT_YUV420P10LE:
		return "yuv420p10le";
	case AV_PIX_FMT_NONE:
		return "none";
	default:
		return "other";
	}
}

void vidw_update(int bitrate, int keyint_sec, const char *opts)
{
	struct ffmpeg_video_encoder enc;
	memset(&enc, 0, sizeof(enc));
	enc.encoder = (obs_encoder_t *)&g_host;
	enc.context = calloc(1, sizeof(AVCodecContext));
	enc.context->priv_data = &g_priv;
	enc.context->gop_size = 12345; /* sentinel: untouched when keyint_sec is 0 */

	const struct video_output_info *voi = video_output_get_info(NULL);
	struct video_scale_info info = {0};
	info.format = g_host.format;
	info.colorspace = g_host.colorspace;
	info.range = g_host.range;
	ffmpeg_video_encoder_update(&enc, bitrate, keyint_sec, voi, &info, opts);

	AVCodecContext *c = enc.context;
	/* the kbps->bps multiplication overflows int for bitrate > 2147483: UB in the reference,
	 * the line is printed with the informational prefix then (spec D-1) */
	printf("%svid bit_rate=%lld rc_buffer_size=%d size=%dx%d time_base=%d/%d framerate=%d/%d pix_fmt=%s\n",
	       bitrate > 2147483 ? "info_" : "", (long long)c->bit_rate, c->rc_buffer_size, c->width, c->height,
	       c->time_base.num, c->time_base.den,
	       c->framerate.num, c->framerate.den, pixname(c->pix_fmt));
	printf("vid color_range=%d primaries=%d trc=%d colorspace=%d chroma_loc=%d gop_size=%d height=%d\n",
	       (int)c->color_range, (int)c->color_primaries, (int)c->color_trc, (int)c->colorspace,
	       (int)c->chroma_sample_location, c->gop_size, enc.height);
	free(c);
}

/* Fake of av_pix_fmt_desc_get: only the chroma subsampling shifts are reproduced, for
 * the pixel formats obs_to_ffmpeg_video_format() can return. */
const AVPixFmtDescriptor *av_pix_fmt_desc_get(enum AVPixelFormat pix_fmt)
{
	static AVPixFmtDescriptor d;
	int w = -1, h = -1;
	switch (pix_fmt) {
	case AV_PIX_FMT_YUV420P:
	case AV_PIX_FMT_NV12:
	case AV_PIX_FMT_YUV420P10LE:
	case AV_PIX_FMT_P010LE:
	case AV_PIX_FMT_YUVA420P:
		w = 1, h = 1;
		break;
	case AV_PIX_FMT_YUV422P:
	case AV_PIX_FMT_YUYV422:
	case AV_PIX_FMT_UYVY422:
	case AV_PIX_FMT_YVYU422:
	case AV_PIX_FMT_YUV422P10LE:
	case AV_PIX_FMT_P216LE:
	case AV_PIX_FMT_YUVA422P:
		w = 1, h = 0;
		break;
	case AV_PIX_FMT_YUV444P:
	case AV_PIX_FMT_YUV444P12LE:
	case AV_PIX_FMT_P416LE:
	case AV_PIX_FMT_YUVA444P:
	case AV_PIX_FMT_YUVA444P12LE:
	case AV_PIX_FMT_RGBA:
	case AV_PIX_FMT_BGRA:
	case AV_PIX_FMT_BGR24:
	case AV_PIX_FMT_GRAY8:
		w = 0, h = 0;
		break;
	default:
		return NULL;
	}
	d.log2_chroma_w = (uint8_t)w;
	d.log2_chroma_h = (uint8_t)h;
	return &d;
}
