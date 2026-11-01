/*
 * Conformance harness (area hwencoders, spec S-050): wrapper for plugins/obs-ffmpeg/obs-ffmpeg-vaapi.c.
 * The real source is #included. libav / libva HEADERS are the real ones; every libav call and
 * every vaapi-utils.c helper is a recording or scripted fake. Device enumeration through /dev/dri
 * and libpci (vaapi_properties_internal) is NOT exercised. Test tooling, not part of OBS.
 */
#define ENABLE_HEVC 1
#define rate_control_modified va_rate_control_modified
#include "hwencoders_api.h"
#include "../../../plugins/obs-ffmpeg/obs-ffmpeg-vaapi.c"
#include <stdio.h>
#include "script.h"

/* ---- vaapi-utils fakes (scripted device capabilities) ---- */
static struct {
	bool open_ok;
	bool rc[4]; /* CBR VBR QVBR CQP */
	bool bframe;
	bool codec[3]; /* h264 hevc av1 */
} g_va = {.open_ok = true};
void hw_va_caps(const char *k, int v)
{
	if (!strcmp(k, "open")) g_va.open_ok = v;
	else if (!strcmp(k, "cbr")) g_va.rc[0] = v;
	else if (!strcmp(k, "vbr")) g_va.rc[1] = v;
	else if (!strcmp(k, "qvbr")) g_va.rc[2] = v;
	else if (!strcmp(k, "cqp")) g_va.rc[3] = v;
	else if (!strcmp(k, "bframe")) g_va.bframe = v;
	else if (!strcmp(k, "h264")) g_va.codec[0] = v;
	else if (!strcmp(k, "hevc")) g_va.codec[1] = v;
	else if (!strcmp(k, "av1")) g_va.codec[2] = v;
}
VADisplay vaapi_open_device(int *fd, const char *path, const char *fn)
{
	printf("va.open_device \"%s\" by %s -> %s\n", path ? path : "(null)", fn, g_va.open_ok ? "ok" : "fail");
	*fd = -1;
	return g_va.open_ok ? (VADisplay)&g_va : NULL;
}
void vaapi_close_device(int *fd, VADisplay dpy)
{
	(void)fd, (void)dpy;
}
bool vaapi_device_rc_supported(VAProfile p, VADisplay d, uint32_t rc, const char *path)
{
	(void)p, (void)d, (void)path;
	return rc == VA_RC_CBR ? g_va.rc[0] : rc == VA_RC_VBR ? g_va.rc[1] : rc == VA_RC_QVBR ? g_va.rc[2] : rc == VA_RC_CQP ? g_va.rc[3] : false;
}
bool vaapi_device_bframe_supported(VAProfile p, VADisplay d)
{
	(void)p, (void)d;
	return g_va.bframe;
}
bool vaapi_display_h264_supported(VADisplay d, const char *p) { (void)d, (void)p; return g_va.codec[0]; }
bool vaapi_display_hevc_supported(VADisplay d, const char *p) { (void)d, (void)p; return g_va.codec[1]; }
bool vaapi_display_av1_supported(VADisplay d, const char *p) { (void)d, (void)p; return g_va.codec[2]; }
bool vaapi_device_h264_supported(const char *p) { (void)p; return g_va.codec[0]; }
bool vaapi_device_hevc_supported(const char *p) { (void)p; return g_va.codec[1]; }
bool vaapi_device_av1_supported(const char *p) { (void)p; return g_va.codec[2]; }
const char *vaapi_get_h264_default_device(void) { return "/dev/dri/renderD128"; }
const char *vaapi_get_hevc_default_device(void) { return "/dev/dri/renderD129"; }
const char *vaapi_get_av1_default_device(void) { return "/dev/dri/renderD130"; }

/* ---- libav fakes ---- */
static AVCodec g_codec;
static int g_fail_step; /* 1 device, 2 frames alloc, 3 frames init, 4 frame alloc, 5 frame buffer, 6 open */
static AVHWDeviceContext g_hwdev;
static AVVAAPIDeviceContext g_vactx;
static AVHWFramesContext g_hwframes;
static AVBufferRef g_devref = {.data = (uint8_t *)&g_hwdev};
static AVBufferRef g_frmref = {.data = (uint8_t *)&g_hwframes};

const AVCodec *avcodec_find_encoder_by_name(const char *n)
{
	printf("av.find_encoder %s\n", n);
	return g_fail_step == 100 ? NULL : &g_codec;
}
AVCodecContext *avcodec_alloc_context3(const AVCodec *c)
{
	(void)c;
	AVCodecContext *x = calloc(1, sizeof(*x));
	x->priv_data = calloc(1, 16);
	return x;
}
void avcodec_free_context(AVCodecContext **x)
{
	if (*x) { free((*x)->priv_data); free(*x); }
	*x = NULL;
}
int av_hwdevice_ctx_create(AVBufferRef **r, enum AVHWDeviceType t, const char *dev, AVDictionary *o, int f)
{
	(void)t, (void)o, (void)f;
	printf("av.hwdevice_ctx_create vaapi \"%s\"\n", dev ? dev : "(null)");
	if (g_fail_step == 1) return -22;
	g_hwdev.hwctx = &g_vactx;
	g_vactx.display = (VADisplay)&g_va;
	*r = &g_devref;
	return 0;
}
AVBufferRef *av_hwframe_ctx_alloc(AVBufferRef *d)
{
	(void)d;
	return g_fail_step == 2 ? NULL : &g_frmref;
}
int av_hwframe_ctx_init(AVBufferRef *r)
{
	AVHWFramesContext *f = (AVHWFramesContext *)r->data;
	printf("av.hwframe_ctx_init format=%d sw_format=%d size=%dx%d\n", f->format, f->sw_format, f->width, f->height);
	return g_fail_step == 3 ? -22 : 0;
}
static AVFrame g_frame;
AVFrame *av_frame_alloc(void)
{
	return g_fail_step == 4 ? NULL : &g_frame;
}
int av_frame_get_buffer(AVFrame *f, int align)
{
	(void)align;
	printf("av.frame_get_buffer format=%d size=%dx%d colorspace=%d range=%d chroma_loc=%d\n", f->format, f->width,
	       f->height, f->colorspace, f->color_range, f->chroma_location);
	return g_fail_step == 5 ? -22 : 0;
}
AVBufferRef *av_buffer_ref(const AVBufferRef *r) { return (AVBufferRef *)r; }
void av_buffer_unref(AVBufferRef **r) { *r = NULL; }
void av_frame_unref(AVFrame *f) { (void)f; }
void av_frame_free(AVFrame **f) { *f = NULL; }
int avcodec_open2(AVCodecContext *c, const AVCodec *k, AVDictionary **o)
{
	(void)k, (void)o;
	printf("av.codec_open pix_fmt=%d profile=%d level=%d bit_rate=%lld rc_max_rate=%lld rc_buf_occ=%lld gq=%d bf=%d gop=%d size=%dx%d tb=%d/%d "
	       "range=%d prim=%d trc=%d cs=%d chroma_loc=%d\n",
	       c->pix_fmt, c->profile, c->level, (long long)c->bit_rate, (long long)c->rc_max_rate,
	       (long long)c->rc_initial_buffer_occupancy, c->global_quality, c->max_b_frames, c->gop_size, c->width,
	       c->height, c->time_base.num, c->time_base.den, c->color_range, c->color_primaries, c->color_trc,
	       c->colorspace, c->chroma_sample_location);
	return g_fail_step == 6 ? -22 : 0;
}
static AVPacket g_pkt;
AVPacket *av_packet_alloc(void) { return &g_pkt; }
void av_packet_free(AVPacket **p) { *p = NULL; }
void av_packet_unref(AVPacket *p) { p->size = 0; }
/* scripted encode result */
static uint8_t g_pdata[256];
static int g_psize, g_send_ret, g_recv_ret;
static int64_t g_ppts, g_pdts;
int avcodec_send_frame(AVCodecContext *c, const AVFrame *f)
{
	(void)c, (void)f;
	return g_send_ret;
}
int avcodec_receive_packet(AVCodecContext *c, AVPacket *p)
{
	(void)c;
	if (g_recv_ret)
		return g_recv_ret;
	p->data = g_pdata;
	p->size = g_psize;
	p->pts = g_ppts;
	p->dts = g_pdts;
	g_recv_ret = AVERROR(EAGAIN); /* one packet per script command */
	return 0;
}

/* ---- entry points ---- */
void hw_va_fail(int step) { g_fail_step = step; }
void *hw_va_create(int codec, obs_data_t *s)
{
	return vaapi_create_internal(s, (obs_encoder_t *)&g_hw, (enum codec_type)codec);
}
bool hw_va_update(void *e, obs_data_t *s) { return vaapi_update(e, s); }
void hw_va_destroy(void *e) { vaapi_destroy(e); }
bool hw_va_encode(void *data, const uint8_t *d, size_t n, int64_t pts, int64_t dts, int send_ret, int recv_ret,
		  struct encoder_packet *p, bool *got)
{
	struct vaapi_encoder *e = data;
	memcpy(g_pdata, d, n);
	g_psize = (int)n;
	g_ppts = pts; g_pdts = dts; g_send_ret = send_ret; g_recv_ret = recv_ret;
	bool r = vaapi_encode_internal(e, NULL, p, got);
	printf("state: header=");
	script_print_hex(e->header, e->header_size);
	printf(" sei=");
	script_print_hex(e->sei, e->sei_size);
	printf(" first_packet=%d\n", e->first_packet);
	return r;
}
void hw_va_video_info(void *data, int pref, int cur, int *out)
{
	struct vaapi_encoder *e = data;
	struct video_scale_info info = {.format = cur};
	g_hw.pref_format = pref;
	e->encoder = (obs_encoder_t *)&g_hw;
	vaapi_video_info(e, &info);
	*out = info.format;
}
void hw_va_defaults(int codec, obs_data_t *s) { vaapi_defaults_internal(s, (enum codec_type)codec); }
void hw_va_register(void)
{
	obs_register_encoder(&h264_vaapi_encoder_info);
	obs_register_encoder(&h264_vaapi_encoder_tex_info);
	obs_register_encoder(&av1_vaapi_encoder_info);
	obs_register_encoder(&av1_vaapi_encoder_tex_info);
	obs_register_encoder(&hevc_vaapi_encoder_info);
	obs_register_encoder(&hevc_vaapi_encoder_tex_info);
}
const char *hw_va_rc(const char *name, int *qp, int *br, int *mr)
{
	const rc_mode_t *m = get_rc_mode(name);
	*qp = m->qp; *br = m->bitrate; *mr = m->maxrate;
	return m->name;
}
void hw_va_rc_modified(obs_properties_t *p, obs_data_t *s) { va_rate_control_modified(p, NULL, s); }
bool hw_va_device_modified(obs_properties_t *p, obs_data_t *s) { return vaapi_device_modified(p, NULL, s); }
obs_properties_t *hw_va_mkprops(void)
{
	obs_properties_t *props = obs_properties_create();
	obs_property_t *l = obs_properties_add_list(props, "rate_control", "RateControl", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	(void)l;
	obs_properties_add_int(props, "bitrate", "Bitrate", 0, 300000, 50);
	obs_properties_add_int(props, "maxrate", "MaxBitrate", 0, 300000, 50);
	obs_properties_add_int(props, "qp", "QP", 0, 51, 1);
	obs_properties_add_int(props, "bf", "BFrames", 0, 4, 1);
	return props;
}
