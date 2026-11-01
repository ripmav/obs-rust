/*
 * Conformance harness (area encopts, spec S-025): recording fakes for everything the
 * encoder plugins call that is not the code under test: the OBS host (encoder/video
 * accessors), libx264, the NVENC capability probe, libavcodec/libavutil option setters,
 * libfdk-aac and the QuickSync library. Each fake prints one line per call
 * ("x264.param_parse ...") so that the order and arguments of every library call are
 * part of the expected output. Test tooling, not part of OBS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "obs.h"
#include "obs-module.h"
#include "util/platform.h"
#include "encopts_api.h"

/* ------------------------------------------------------------------------- */
/* platform / module stubs required by obs-data.c, obs-properties.c */

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

bool os_quick_write_utf8_file_safe(const char *path, const char *str, size_t len, bool marker, const char *temp_ext,
				   const char *backup_ext)
{
	(void)path, (void)str, (void)len, (void)marker, (void)temp_ext, (void)backup_ext;
	abort();
}

int os_rename(const char *old_path, const char *new_path)
{
	return rename(old_path, new_path);
}

/* The fake translation table is the identity: obs_module_text(KEY) returns KEY. The
 * expected output therefore shows the locale keys the plugins use. */
const char *obs_module_text(const char *key)
{
	return key;
}

/* ------------------------------------------------------------------------- */
/* fake host */

struct eo_host g_host = {
	.format = VIDEO_FORMAT_NV12,
	.colorspace = VIDEO_CS_709,
	.range = VIDEO_RANGE_PARTIAL,
	.fps_num = 60,
	.fps_den = 1,
	.width = 1920,
	.height = 1080,
	.pref_format = VIDEO_FORMAT_NONE,
	.channels = 2,
	.speakers = SPEAKERS_STEREO,
	.sample_rate = 48000,
	.hdr_peak = 1000,
	.cpu_platform = 0,
	.nv_devices = 1,
	.nv_av1_supported = 1,
};

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
	g_voi.format = g_host.format;
	g_voi.colorspace = g_host.colorspace;
	g_voi.range = g_host.range;
	g_voi.fps_num = g_host.fps_num;
	g_voi.fps_den = g_host.fps_den;
	g_voi.width = g_host.width;
	g_voi.height = g_host.height;
	return &g_voi;
}

uint32_t obs_encoder_get_width(const obs_encoder_t *e)
{
	(void)e;
	return g_host.width;
}

uint32_t obs_encoder_get_height(const obs_encoder_t *e)
{
	(void)e;
	return g_host.height;
}

enum video_format obs_encoder_get_preferred_video_format(const obs_encoder_t *e)
{
	(void)e;
	return g_host.pref_format;
}

void obs_encoder_set_last_error(obs_encoder_t *e, const char *msg)
{
	(void)e;
	snprintf(g_host.last_error, sizeof(g_host.last_error), "%s", msg);
	printf("host.set_last_error \"%s\"\n", msg);
}

bool obs_encoder_video_tex_active(const obs_encoder_t *e, enum video_format f)
{
	(void)e;
	return f == VIDEO_FORMAT_NV12 ? g_host.tex_nv12 : f == VIDEO_FORMAT_P010 ? g_host.tex_p010 : false;
}

float obs_get_video_hdr_nominal_peak_level(void)
{
	return (float)g_host.hdr_peak;
}

os_performance_token_t *os_request_high_performance(const char *reason)
{
	(void)reason;
	return NULL;
}

void os_end_high_performance(os_performance_token_t *t)
{
	(void)t;
}

/* ------------------------------------------------------------------------- */
/* libx264 (recording fake). Real x264 sets hundreds of fields in
 * x264_param_default_preset; the fake zeroes the structure and then seeds four
 * sentinel values (see the README contract). */
#define _STDINT_H_INCLUDED
#include <x264.h>

int x264_param_default_preset(x264_param_t *p, const char *preset, const char *tune)
{
	printf("x264.param_default_preset preset=%s tune=%s\n", preset ? preset : "<null>", tune ? tune : "<null>");
	memset(p, 0, sizeof(*p));
	/* sentinels: values the plugin must overwrite show up as these numbers */
	p->i_keyint_max = -1111;
	p->i_bframe = -2222;
	p->rc.i_rc_method = -3333;
	p->rc.f_rf_constant = -3.5f;
	return g_host.x264_fail_preset ? -1 : 0;
}

int x264_param_parse(x264_param_t *p, const char *name, const char *value)
{
	int rc = 0;
	for (int i = 0; i < g_host.x264_fail_param_n; i++)
		if (!strcmp(g_host.x264_fail_param[i], name))
			rc = -1;
	printf("x264.param_parse %s=%s -> %d\n", name, value ? value : "<null>", rc);
	if (rc)
		return rc;
	if (!strcmp(name, "keyint"))
		p->i_keyint_max = atoi(value);
	else if (!strcmp(name, "bframes"))
		p->i_bframe = atoi(value);
	else if (!strcmp(name, "bitrate"))
		p->rc.i_bitrate = atoi(value);
	else if (!strcmp(name, "crf"))
		p->rc.f_rf_constant = (float)atof(value);
	else if (!strcmp(name, "vbv-maxrate"))
		p->rc.i_vbv_max_bitrate = atoi(value);
	else if (!strcmp(name, "vbv-bufsize"))
		p->rc.i_vbv_buffer_size = atoi(value);
	return 0;
}

int x264_param_apply_profile(x264_param_t *p, const char *profile)
{
	(void)p;
	printf("x264.param_apply_profile %s -> %d\n", profile, g_host.x264_fail_profile ? -1 : 0);
	return g_host.x264_fail_profile ? -1 : 0;
}

static int dummy_x264;

x264_t *x264_encoder_open(x264_param_t *p)
{
	(void)p;
	printf("x264.encoder_open -> %s\n", g_host.x264_fail_open ? "NULL" : "ok");
	return g_host.x264_fail_open ? NULL : (x264_t *)&dummy_x264;
}

int x264_encoder_reconfig(x264_t *h, x264_param_t *p)
{
	(void)h, (void)p;
	printf("x264.encoder_reconfig\n");
	return 0;
}

void x264_encoder_close(x264_t *h)
{
	(void)h;
	printf("x264.encoder_close\n");
}

int x264_encoder_headers(x264_t *h, x264_nal_t **nal, int *n)
{
	(void)h;
	*nal = NULL;
	*n = 0;
	printf("x264.encoder_headers\n");
	return 0;
}

/* ------------------------------------------------------------------------- */
/* audio host accessors (libfdk, ffmpeg audio encoders) */
#include "media-io/audio-io.h"

audio_t *obs_encoder_audio(const obs_encoder_t *e)
{
	(void)e;
	return (audio_t *)&g_host;
}

size_t audio_output_get_channels(const audio_t *a)
{
	(void)a;
	return (size_t)g_host.channels;
}

uint32_t audio_output_get_sample_rate(const audio_t *a)
{
	(void)a;
	return g_host.sample_rate;
}

const struct audio_output_info *audio_output_get_info(const audio_t *a)
{
	static struct audio_output_info i;
	(void)a;
	i.samples_per_sec = g_host.sample_rate;
	i.format = AUDIO_FORMAT_FLOAT_PLANAR;
	i.speakers = (enum speaker_layout)g_host.speakers;
	return &i;
}

size_t obs_encoder_get_mixer_index(const obs_encoder_t *e)
{
	(void)e;
	return 0;
}
