/*
 * Conformance harness (area hwencoders, spec S-050): fake of the OBS host calls the hardware
 * encoder plugins make (encoder/video accessors, registration, rerouting, ROI) and of the
 * platform helpers obs-data.c / obs-properties.c need. Each interesting call prints one line.
 * Test tooling, not part of OBS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "obs.h"
#include "obs-module.h"
#include "util/platform.h"
#include "hwencoders_api.h"

struct hw_host g_hw = {
	.format = VIDEO_FORMAT_NV12,
	.colorspace = VIDEO_CS_709,
	.range = VIDEO_RANGE_PARTIAL,
	.fps_num = 60,
	.fps_den = 1,
	.width = 1920,
	.height = 1080,
	.pref_format = VIDEO_FORMAT_NONE,
	.tex_nv12 = true,
	.hdr_peak = 1000,
	.timebase_num = 1,
};
struct obs_encoder_info g_hw_registered[HW_MAX_ENC];
int g_hw_nregistered;

char *os_quick_read_utf8_file(const char *path)
{
	(void)path;
	abort();
}
bool os_quick_write_utf8_file(const char *path, const char *str, size_t len, bool marker)
{
	(void)path, (void)str, (void)len, (void)marker;
	abort();
}
bool os_quick_write_utf8_file_safe(const char *path, const char *str, size_t len, bool marker, const char *a,
				   const char *b)
{
	(void)path, (void)str, (void)len, (void)marker, (void)a, (void)b;
	abort();
}
int os_rename(const char *o, const char *n)
{
	return rename(o, n);
}
/* identity translation table: vectors show the locale keys */
const char *obs_module_text(const char *key)
{
	return key;
}

static struct video_output_info g_voi;
const char *obs_encoder_get_name(const obs_encoder_t *e)
{
	(void)e;
	return "enc";
}
video_t *obs_encoder_video(const obs_encoder_t *e)
{
	(void)e;
	return (video_t *)&g_voi;
}
const struct video_output_info *video_output_get_info(const video_t *v)
{
	(void)v;
	g_voi.format = g_hw.format;
	g_voi.colorspace = g_hw.colorspace;
	g_voi.range = g_hw.range;
	g_voi.fps_num = g_hw.fps_num;
	g_voi.fps_den = g_hw.fps_den;
	g_voi.width = g_hw.width;
	g_voi.height = g_hw.height;
	return &g_voi;
}
uint32_t obs_encoder_get_width(const obs_encoder_t *e)
{
	(void)e;
	return g_hw.width;
}
uint32_t obs_encoder_get_height(const obs_encoder_t *e)
{
	(void)e;
	return g_hw.height;
}
enum video_format obs_encoder_get_preferred_video_format(const obs_encoder_t *e)
{
	(void)e;
	return g_hw.pref_format;
}
void obs_encoder_set_last_error(obs_encoder_t *e, const char *msg)
{
	(void)e;
	snprintf(g_hw.last_error, sizeof(g_hw.last_error), "%s", msg ? msg : "");
	printf("host.set_last_error \"%s\"\n", g_hw.last_error);
}
bool obs_encoder_video_tex_active(const obs_encoder_t *e, enum video_format f)
{
	(void)e;
	return f == VIDEO_FORMAT_NV12 ? g_hw.tex_nv12 : f == VIDEO_FORMAT_P010 ? g_hw.tex_p010 : false;
}
float obs_get_video_hdr_nominal_peak_level(void)
{
	return (float)g_hw.hdr_peak;
}
bool obs_encoder_scaling_enabled(const obs_encoder_t *e)
{
	(void)e;
	return g_hw.scaling;
}
bool obs_encoder_gpu_scaling_enabled(obs_encoder_t *e)
{
	(void)e;
	return g_hw.gpu_scaling;
}
void *obs_encoder_create_rerouted(obs_encoder_t *e, const char *id)
{
	(void)e;
	printf("host.create_rerouted \"%s\"\n", id);
	return (void *)0x1;
}
bool obs_encoder_has_roi(const obs_encoder_t *e)
{
	(void)e;
	return g_hw.has_roi;
}
uint32_t obs_encoder_get_roi_increment(const obs_encoder_t *e)
{
	(void)e;
	return g_hw.roi_increment;
}
void obs_encoder_enum_roi(obs_encoder_t *e, void (*cb)(void *, struct obs_encoder_roi *), void *p)
{
	(void)e;
	for (int i = 0; i < g_hw.nroi; i++)
		cb(p, &g_hw.roi[i]);
}
void obs_register_encoder_s(const struct obs_encoder_info *info, size_t size)
{
	(void)size;
	if (g_hw_nregistered < HW_MAX_ENC)
		g_hw_registered[g_hw_nregistered++] = *info;
}

/* ---- shared libav fakes (used by the ffmpeg-based wrappers) ---- */
int av_opt_set(void *obj, const char *name, const char *val, int flags)
{
	(void)obj, (void)flags;
	printf("av.opt_set %s=%s\n", name, val ? val : "(null)");
	return 0;
}
int av_strerror(int errnum, char *buf, size_t size)
{
	snprintf(buf, size, "averr(%d)", errnum);
	return 0;
}

