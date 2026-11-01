/*
 * Conformance harness (area hwencoders, spec S-050): interface between the script runner
 * (hwencoders_runner.c), the per-plugin wrapper translation units (each #includes the real
 * plugin source so that its static functions are callable) and the recording fakes
 * (hwencoders_host.c). Test tooling, not part of OBS.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "obs.h"

#define HW_MAX_ROI 8

struct hw_host {
	enum video_format format;
	enum video_colorspace colorspace;
	enum video_range_type range;
	uint32_t fps_num, fps_den;
	uint32_t width, height;
	enum video_format pref_format;
	bool tex_nv12, tex_p010;
	bool scaling, gpu_scaling;
	int hdr_peak;
	int64_t timebase_num;
	bool has_roi;
	uint32_t roi_increment;
	int nroi;
	struct obs_encoder_roi roi[HW_MAX_ROI];
	char last_error[512];
};
extern struct hw_host g_hw;

/* fake obs_register_encoder table */
#define HW_MAX_ENC 32
extern struct obs_encoder_info g_hw_registered[HW_MAX_ENC];
extern int g_hw_nregistered;

/* nvenc wrapper (hwencoders_nvenc.c) */
int hw_nv_create_instance(void *list); /* fake NvEncodeAPICreateInstance */
void *hw_nv_create(int codec, bool tex, obs_data_t *s);
bool hw_nv_update(void *enc, obs_data_t *s);
void hw_nv_destroy(void *enc);
bool hw_nv_encode(void *enc, int64_t pts, struct encoder_packet *pkt, bool *got);
void hw_nv_state(void *enc);
bool hw_nv_extra(void *enc, const uint8_t **p, size_t *n);
bool hw_nv_sei(void *enc);
void hw_nv_register(void);
void hw_nv_defaults(int codec, obs_data_t *s);

/* fake SDK controls (hwencoders_nvenc.c) */
void hw_nv_cap(const char *name, int v);
void hw_nv_status(const char *fn, int code);
void hw_nv_preset_field(const char *key, int v);
void hw_nv_lockpic(int type, int64_t pts, int size);
void hw_nv_set_header(const uint8_t *b, size_t n);
void hw_nv_set_errstr(const char *s);

/* nvenc helpers wrapper (hwencoders_nvhelp.c) */
extern const char *g_hw_ini;
extern int g_hw_dll;        /* 0 = os_dlopen fails */
extern int g_hw_sym_maxver; /* 0 = sym missing */
extern int g_hw_sym_create;
extern uint32_t g_hw_maxver;
extern int g_hw_pipe_fail;
void hw_nh_supported(void);
void hw_nh_failed(int err, const char *session_str);
const char *hw_nh_errname(int err);
bool hw_nh_broken_split(void);
void hw_nh_caps(int codec);
void hw_nh_load(void);
bool hw_nh_init(void);
void hw_nh_version_info(void);

/* nvenc compat wrapper (hwencoders_nvcompat.c) */
void hw_nc_register(void);

/* ffmpeg nvenc wrapper (hwencoders_ffnv.c) */
extern int g_ff_init_codec_ret, g_ff_init_error, g_ff_init_fail_once;
void hw_ff_set_packet(const uint8_t *d, size_t n, int64_t dts);
void hw_ff_set_ctx_bframes_timebase(void *enc, int bf, int tb);
void *hw_ff_create(int hevc, obs_data_t *s);
bool hw_ff_reconfigure(void *enc, obs_data_t *s);
void hw_ff_destroy(void *enc);
bool hw_ff_encode(void *enc, struct encoder_packet *p, bool *got);
void hw_ff_state(void *enc);
void hw_ff_video_info(int pref, int cur, int *out);
void hw_ff_register(void);
void hw_ff_defaults(int codec, obs_data_t *s);

/* vaapi wrapper (hwencoders_vaapi.c) */
void hw_va_caps(const char *k, int v);
void hw_va_fail(int step);
void *hw_va_create(int codec, obs_data_t *s);
bool hw_va_update(void *e, obs_data_t *s);
void hw_va_destroy(void *e);
bool hw_va_encode(void *e, const uint8_t *d, size_t n, int64_t pts, int64_t dts, int send_ret, int recv_ret,
		  struct encoder_packet *p, bool *got);
void hw_va_video_info(void *e, int pref, int cur, int *out);
void hw_va_defaults(int codec, obs_data_t *s);
void hw_va_register(void);
const char *hw_va_rc(const char *name, int *qp, int *br, int *mr);
void hw_va_rc_modified(obs_properties_t *p, obs_data_t *s);
bool hw_va_device_modified(obs_properties_t *p, obs_data_t *s);
obs_properties_t *hw_va_mkprops(void);

/* qsv wrapper (hwencoders_qsv.c) */
void hw_q_adapter(int idx, int intel, int dgpu, int av1, int hevc);
void hw_q_set(const char *k, int v);
void hw_q_headers(const uint8_t *vps, size_t nv, const uint8_t *sps, size_t ns, const uint8_t *pps, size_t np);
void hw_q_bitstream(const uint8_t *d, size_t n, unsigned frame_type, long long ts, long long dts);
void *hw_q_create(int codec, bool tex, obs_data_t *s);
void hw_q_destroy(void *e);
bool hw_q_encode(void *e, long long pts, int tex, struct encoder_packet *p, bool *got);
void hw_q_extra(void *e);
void hw_q_ts(long long ts, int is_obs);
void hw_q_load(void);
