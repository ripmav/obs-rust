/*
 * Conformance harness (area hwencoders, spec S-050): wrapper for plugins/obs-nvenc/nvenc-helpers.c
 * (error mapping, driver/version gate, capability table from the obs-nvenc-test INI, split-encode
 * gate). The platform calls it makes (dlopen/dlsym, executable lookup, process pipe, profiler) are
 * faked here and scripted by the runner. Test tooling, not part of OBS.
 */
#include "hwencoders_api.h"
#include "../../../plugins/obs-nvenc/nvenc-helpers.c"
#include <stdio.h>
#include <util/pipe.h>
#include <util/profiler.h>

const char *g_hw_ini = "";
int g_hw_dll = 1;
int g_hw_sym_maxver = 1;
int g_hw_sym_create = 1;
uint32_t g_hw_maxver = 0xD0;
int g_hw_pipe_fail;

void *os_dlopen(const char *path)
{
	printf("os.dlopen \"%s\" -> %s\n", path, g_hw_dll ? "ok" : "fail");
	return g_hw_dll ? (void *)&g_hw_dll : NULL;
}
static NVENCSTATUS fake_maxver(uint32_t *v)
{
	*v = g_hw_maxver;
	return 0;
}
void *os_dlsym(void *m, const char *name)
{
	(void)m;
	if (!strcmp(name, "NvEncodeAPIGetMaxSupportedVersion"))
		return g_hw_sym_maxver ? (void *)fake_maxver : NULL;
	if (!strcmp(name, "NvEncodeAPICreateInstance"))
		return g_hw_sym_create ? (void *)hw_nv_create_instance : NULL;
	return NULL;
}
char *os_get_executable_path_ptr(const char *name)
{
	return bstrdup(name);
}
void profile_start(const char *n)
{
	(void)n;
}
void profile_end(const char *n)
{
	(void)n;
}

/* process pipe: a single scripted read of g_hw_ini, then EOF */
static size_t g_off;
os_process_pipe_t *os_process_pipe_create2(const os_process_args_t *a, const char *t)
{
	(void)a, (void)t;
	g_off = 0;
	return g_hw_pipe_fail ? NULL : (os_process_pipe_t *)&g_off;
}
int os_process_pipe_destroy(os_process_pipe_t *p)
{
	(void)p;
	return 0;
}
size_t os_process_pipe_read(os_process_pipe_t *p, uint8_t *d, size_t len)
{
	(void)p;
	size_t n = strlen(g_hw_ini) - g_off;
	if (n > len)
		n = len;
	memcpy(d, g_hw_ini + g_off, n);
	g_off += n;
	return n;
}
struct os_process_args *os_process_args_create(const char *e)
{
	(void)e;
	return (struct os_process_args *)&g_off;
}
void os_process_args_destroy(struct os_process_args *a)
{
	(void)a;
}

void hw_nh_supported(void)
{
	bool r = nvenc_supported();
	printf("nvenc_supported -> %d\n", r);
}
void hw_nh_load(void)
{
	obs_nvenc_load();
}
bool hw_nh_init(void)
{
	return init_nvenc((obs_encoder_t *)&g_hw);
}
void hw_nh_failed(int err, const char *session_str)
{
	extern void hw_nv_set_errstr(const char *);
	hw_nv_set_errstr(session_str);
	g_hw.last_error[0] = 0;
	bool r = nv_failed2((obs_encoder_t *)&g_hw, session_str ? (void *)&g_off : NULL, err, "func", "call");
	printf("nv_failed2(%s) -> %d\n", nv_error_name(err), r);
}
const char *hw_nh_errname(int err)
{
	return nv_error_name(err);
}
bool hw_nh_broken_split(void)
{
	return has_broken_split_encoding();
}
void hw_nh_caps(int codec)
{
	const struct encoder_caps *c = get_encoder_caps(codec);
	printf("caps[%s]: supported=%d bframes=%d bref=%d engines=%d max=%dx%d tf=%d la_level=%d dyn_br=%d lookahead=%d "
	       "lossless=%d taq=%d uhq=%d 10bit=%d 444=%d 422=%d\n",
	       get_codec_name(codec), is_codec_supported(codec), c->bframes, c->bref_modes, c->engines, c->max_width,
	       c->max_height, c->temporal_filter, c->lookahead_level, c->dyn_bitrate, c->lookahead, c->lossless,
	       c->temporal_aq, c->uhq, c->ten_bit, c->four_four_four, c->four_two_two);
}
void hw_nh_version_info(void)
{
	printf("devices=%d driver=%d.%d\n", num_encoder_devices(), driver_version_major, driver_version_minor);
}
