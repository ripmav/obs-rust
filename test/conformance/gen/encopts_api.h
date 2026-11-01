/*
 * Conformance harness (area encopts, spec S-025): interface between the script
 * runner (encopts_runner.c), the per-plugin wrapper translation units
 * (encopts_x264.c, encopts_qsv.c, encopts_fdk.c, encopts_nvenc.c, encopts_vid.c,
 * encopts_aud.c; each #includes the real plugin source so that its static
 * functions can be called) and the recording fakes (encopts_stubs.c).
 * Test tooling, not part of OBS.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "obs.h"

/* Scripted state of the fake host application (what obs_encoder_*() & co. return). */
struct eo_host {
	enum video_format format;       /* video_output_get_info()->format */
	enum video_colorspace colorspace;
	enum video_range_type range;
	uint32_t fps_num, fps_den;
	uint32_t width, height;         /* obs_encoder_get_width/height */
	enum video_format pref_format;  /* obs_encoder_get_preferred_video_format */
	bool tex_nv12, tex_p010;        /* obs_encoder_video_tex_active */
	int cpu_platform;               /* qsv_get_cpu_platform (enum qsv_cpu_platform) */
	int hdr_peak;                   /* obs_get_video_hdr_nominal_peak_level */
	/* audio */
	int channels, speakers;         /* speaker_layout enum */
	uint32_t sample_rate;
	/* results */
	char last_error[256];
	/* x264 fake library failure switches */
	int x264_fail_preset, x264_fail_profile, x264_fail_open;
	char x264_fail_param[8][32];
	int x264_fail_param_n;
	/* nvenc fake capability table */
	int nv_devices, nv_split_broken, nv_av1_supported;
	/* ffmpeg audio fake */
	int ff_open_fail;
	int qsv_dgpu;
};
extern struct eo_host g_host;

/* x264 */
void x264w_defaults(obs_data_t *s);
obs_properties_t *x264w_props(void);
bool x264w_settings_only(obs_data_t *s, bool update); /* update_settings() on a context-less object */
void x264w_print_params(void);                        /* params of the last settings_only/create */
void *x264w_create(obs_data_t *s);
bool x264w_update(void *h, obs_data_t *s);
void x264w_destroy(void *h);
void x264w_print_handle_params(void *h);
void x264w_video_info(void *h, struct video_scale_info *info);

/* QSV: kind 0 = h264 v1, 1 = h264 v2, 2 = av1, 3 = hevc */
void qsvw_defaults(int kind, obs_data_t *s);
obs_properties_t *qsvw_props(int kind);
void *qsvw_create(int kind, obs_data_t *s);
bool qsvw_update(void *h, obs_data_t *s);
void qsvw_destroy(void *h);
void qsvw_print_params(void *h);
void qsvw_migrate(obs_data_t *s); /* the three legacy-setting migrations (update_latency/targetusage/ratecontrol) */
void qsvw_video_info(int kind, struct video_scale_info *info);
void *qsvw_new_ctx(int kind);

/* libfdk */
void fdkw_defaults(obs_data_t *s);
obs_properties_t *fdkw_props(void);
bool fdkw_create(obs_data_t *s);
void fdkw_fail(int at);

/* nvenc: codec 0 = h264, 1 = hevc, 2 = av1 */
void nvw_defaults(int codec, obs_data_t *s);
obs_properties_t *nvw_props(int codec);
void nvw_read(obs_data_t *s);
void nvw_apply(int codec, obs_data_t *s);
void nvw_userarg(obs_data_t *s, const char *name);
void nvw_set_caps(int bframes, int bref, int engines, int lossless, int uhq, int ten_bit, int lookahead);

/* ffmpeg video encoder (obs-ffmpeg-video-encoders.c) */
void vidw_update(int bitrate, int keyint_sec, const char *opts);

/* ffmpeg audio encoders */
void audw_defaults(obs_data_t *s);
obs_properties_t *audw_props(void);
void audw_create(const char *kind, obs_data_t *s);
void audw_codec(const char *name, int present);
void audw_codec_lossless(const char *name, int v);
void audw_formats(const char *list);
void audw_rates(const char *list);
