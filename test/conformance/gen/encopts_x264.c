/*
 * Conformance harness (area encopts): wrapper around the real plugins/obs-x264/obs-x264.c.
 * The plugin source is included so that its static functions can be called; the x264
 * library is replaced by the recording fake in encopts_stubs.c. Test tooling.
 */
#include "../../../plugins/obs-x264/obs-x264.c"
#include "encopts_api.h"

static struct obs_x264 g_ctxless;

void x264w_defaults(obs_data_t *s)
{
	obs_x264_defaults(s);
}

obs_properties_t *x264w_props(void)
{
	return obs_x264_props(NULL);
}

static void print_params(const x264_param_t *p)
{
	printf("params csp=%d size=%dx%d fps=%d/%d timebase=%d/%d keyint_max=%d bframe=%d vfr=%d\n", p->i_csp,
	       p->i_width, p->i_height, p->i_fps_num, p->i_fps_den, p->i_timebase_num, p->i_timebase_den,
	       p->i_keyint_max, p->i_bframe, (int)p->b_vfr_input);
	printf("params rc method=%d bitrate=%d vbv_max=%d vbv_buf=%d rf=%.9g filler=%d nal_hrd=%d\n",
	       p->rc.i_rc_method, p->rc.i_bitrate, p->rc.i_vbv_max_bitrate, p->rc.i_vbv_buffer_size,
	       (double)p->rc.f_rf_constant, (int)p->rc.b_filler, p->i_nal_hrd);
	printf("params vui sar=%dx%d fullrange=%d colorprim=%d transfer=%d colmatrix=%d\n", p->vui.i_sar_width,
	       p->vui.i_sar_height, (int)p->vui.b_fullrange, p->vui.i_colorprim, p->vui.i_transfer,
	       p->vui.i_colmatrix);
	printf("params headers repeat=%d annexb=%d aud=%d loglevel=%d\n", (int)p->b_repeat_headers, (int)p->b_annexb,
	       (int)p->b_aud, p->i_log_level);
}

bool x264w_settings_only(obs_data_t *s, bool update)
{
	memset(&g_ctxless, 0, sizeof(g_ctxless));
	bool ok = update_settings(&g_ctxless, s, update);
	return ok;
}

void x264w_print_params(void)
{
	print_params(&g_ctxless.params);
}

void *x264w_create(obs_data_t *s)
{
	return obs_x264_create(s, (obs_encoder_t *)&g_host);
}

bool x264w_update(void *h, obs_data_t *s)
{
	return obs_x264_update(h, s);
}

void x264w_destroy(void *h)
{
	obs_x264_destroy(h);
}

void x264w_print_handle_params(void *h)
{
	print_params(&((struct obs_x264 *)h)->params);
}

void x264w_video_info(void *h, struct video_scale_info *info)
{
	struct obs_x264 tmp = {0};
	tmp.encoder = (obs_encoder_t *)&g_host;
	(void)h;
	obs_x264_video_info(&tmp, info);
}
