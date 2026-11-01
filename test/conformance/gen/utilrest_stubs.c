/*
 * S-055 harness: the minimal fake libobs around source-profiler.c and the pulse audio monitor.
 * Fakes: obs core (audio / video fields only), clock, graphics timers, task queue, audio output info,
 * a simple resampler (float planar -> interleaved target format, same rate), source / data stand-ins.
 * Test tooling, not part of libobs.
 */
#include "obs-internal.h"
#include "util/platform.h"
#include "utilrest_api.h"

struct obs_core *obs;

/* ------------------------------------------------------------ clock */
static uint64_t ur_clock;
void ur_set_clock(uint64_t ns) { ur_clock = ns; }
uint64_t os_gettime_ns(void) { return ur_clock; }

void profile_start(const char *name) { (void)name; }
void profile_end(const char *name) { (void)name; }

/* ------------------------------------------------------------ graphics timers */
struct gs_timer { int id; uint64_t ticks; };
struct gs_timer_range { int id; };
static int timer_ids, ctx_depth, ctx_unbalanced;
static uint64_t next_ticks;
struct ur_gfx_stats ur_gfx;
static bool range_disjoint;
static uint64_t range_freq = 1000000000ULL;
static bool timer_data_ok = true;

void ur_gfx_cfg(uint64_t ticks, bool disjoint, uint64_t freq, bool data_ok)
{
	next_ticks = ticks; range_disjoint = disjoint; range_freq = freq; timer_data_ok = data_ok;
}
void gs_enter_context(graphics_t *g) { (void)g; ur_gfx.enter++; ctx_depth++; }
void gs_leave_context(void) { ur_gfx.leave++; ctx_depth--; if (ctx_depth < 0) ctx_unbalanced++; }
gs_timer_t *gs_timer_create(void) { struct gs_timer *t = bzalloc(sizeof(*t)); t->id = ++timer_ids; t->ticks = next_ticks; ur_gfx.timer_create++; return t; }
void gs_timer_destroy(gs_timer_t *t) { ur_gfx.timer_destroy++; bfree(t); }
void gs_timer_begin(gs_timer_t *t) { (void)t; ur_gfx.timer_begin++; }
void gs_timer_end(gs_timer_t *t) { (void)t; ur_gfx.timer_end++; }
bool gs_timer_get_data(gs_timer_t *t, uint64_t *ticks) { if (!timer_data_ok) return false; *ticks = t->ticks; return true; }
gs_timer_range_t *gs_timer_range_create(void) { ur_gfx.range_create++; return bzalloc(sizeof(struct gs_timer_range)); }
void gs_timer_range_destroy(gs_timer_range_t *r) { ur_gfx.range_destroy++; bfree(r); }
void gs_timer_range_begin(gs_timer_range_t *r) { (void)r; ur_gfx.range_begin++; }
void gs_timer_range_end(gs_timer_range_t *r) { (void)r; ur_gfx.range_end++; }
bool gs_timer_range_get_data(gs_timer_range_t *r, bool *disjoint, uint64_t *freq) { (void)r; *disjoint = range_disjoint; *freq = range_freq; return true; }
int ur_gfx_unbalanced(void) { return ctx_unbalanced + ctx_depth; }

/* ------------------------------------------------------------ task queue (run only on request) */
struct task { obs_task_t fn; void *param; };
static struct task tasks[64];
static int ntasks;
int ur_tasks_queued(void) { return ntasks; }
void obs_queue_task(enum obs_task_type type, obs_task_t task, void *param, bool wait)
{
	(void)type; (void)wait;
	if (ntasks < 64) { tasks[ntasks].fn = task; tasks[ntasks].param = param; ntasks++; }
}
int ur_run_tasks(void)
{
	int n = ntasks;
	for (int i = 0; i < n; i++) tasks[i].fn(tasks[i].param);
	ntasks = 0;
	return n;
}

static uint64_t last_async_ts;
void ur_set_async_ts(uint64_t ts) { last_async_ts = ts; }
uint64_t obs_source_get_last_async_ts(const obs_source_t *s) { (void)s; return last_async_ts; }

/* ------------------------------------------------------------ sources / data / audio output */
static char fake_device_id[128];
const char *ur_dev_setting;
const char *obs_source_get_name(const obs_source_t *s) { return s->context.name; }
obs_data_t *obs_source_get_settings(const obs_source_t *s) { (void)s; return (obs_data_t *)&ur_dev_setting; }
const char *obs_data_get_string(obs_data_t *d, const char *name)
{
	(void)d;
	if (strcmp(name, "device_id")) return "";
	return ur_dev_setting ? ur_dev_setting : "";
}
void obs_data_release(obs_data_t *d) { (void)d; }

static obs_source_audio_capture_t cap_cb; static void *cap_param; int ur_cap_adds, ur_cap_removes;
void obs_source_add_audio_capture_callback(obs_source_t *s, obs_source_audio_capture_t cb, void *p)
{ (void)s; cap_cb = cb; cap_param = p; ur_cap_adds++; }
void obs_source_remove_audio_capture_callback(obs_source_t *s, obs_source_audio_capture_t cb, void *p)
{ (void)s; (void)cb; (void)p; cap_cb = NULL; ur_cap_removes++; }
void ur_feed(obs_source_t *s, const struct audio_data *ad)
{
	if (cap_cb) cap_cb(cap_param, s, ad, false);
}
bool ur_have_cb(void) { return cap_cb != NULL; }

static struct audio_output_info ao_info = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO};
void ur_set_ao(uint32_t rate, enum speaker_layout sp) { ao_info.samples_per_sec = rate; ao_info.speakers = sp; }
const struct audio_output_info *audio_output_get_info(const audio_t *a) { (void)a; return &ao_info; }

/* ------------------------------------------------------------ fake resampler */
struct audio_resampler { struct resample_info to, from; uint8_t *buf; };
static bool resampler_fail;
void ur_resampler_fail(bool f) { resampler_fail = f; }
audio_resampler_t *audio_resampler_create(const struct resample_info *dst, const struct resample_info *src)
{
	if (resampler_fail) return NULL;
	struct audio_resampler *r = bzalloc(sizeof(*r));
	r->to = *dst; r->from = *src;
	return r;
}
void audio_resampler_destroy(audio_resampler_t *r) { if (r) { bfree(r->buf); bfree(r); } }
bool audio_resampler_resample(audio_resampler_t *r, uint8_t *output[], uint32_t *out_frames, uint64_t *ts_offset,
			      const uint8_t *const input[], uint32_t frames)
{
	size_t ch = get_audio_channels(r->to.speakers);
	size_t fch = get_audio_channels(r->from.speakers);
	size_t bps = get_audio_bytes_per_channel(r->to.format);
	bfree(r->buf);
	r->buf = bzalloc(frames * ch * bps + 1);
	for (uint32_t f = 0; f < frames; f++)
		for (size_t c = 0; c < ch; c++) {
			float x = ((const float *)input[c < fch ? c : fch - 1])[f];
			uint8_t *o = r->buf + (f * ch + c) * bps;
			switch (r->to.format) {
			case AUDIO_FORMAT_U8BIT: *o = (uint8_t)(x * 127.0f + 128.0f); break;
			case AUDIO_FORMAT_16BIT: { int16_t v = (int16_t)(x * 32767.0f); memcpy(o, &v, 2); break; }
			case AUDIO_FORMAT_32BIT: { int32_t v = (int32_t)(x * 2147483647.0f); memcpy(o, &v, 4); break; }
			default: memcpy(o, &x, 4);
			}
		}
	output[0] = r->buf; *out_frames = frames; *ts_offset = 0;
	return true;
}

/* ------------------------------------------------------------ platform hooks that bmem.c / dstr.c reference */
void os_breakpoint(void) {}
void os_oom(void) {}
size_t os_mbs_to_utf8_ptr(const char *str, size_t len, char **dst) { (void)str; (void)len; *dst = NULL; return 0; }
size_t os_utf8_to_wcs_ptr(const char *str, size_t len, wchar_t **dst) { (void)str; (void)len; *dst = NULL; return 0; }
