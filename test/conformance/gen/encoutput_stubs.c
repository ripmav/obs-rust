/*
 * S-041 conformance: the fake "libobs core" around the real obs-encoder.c / obs-output.c / obs-output-delay.c.
 *   - an `obs` core with only the plugin tables, video counters and mutexes
 *   - a context layer (name, signal / proc handlers, control block) instead of obs.c
 *   - fake video_t / audio_t whose raw-frame connections are driven by the script
 *   - a settable clock
 * Test tooling, not part of libobs.
 */
#include <inttypes.h>
#include <string.h>
#include "encoutput_api.h"
#include "util/threading.h"
#include "media-io/audio-io.h"
#include "media-io/video-io.h"

struct obs_core *obs;

/* ------------------------------------------------------------ clock */
static uint64_t eo_clock;
void eo_set_clock(uint64_t ns) { eo_clock = ns; }
uint64_t os_gettime_ns(void) { return eo_clock; }

float rand_float(int positive_only)
{
	(void)positive_only;
	return 0.0f; /* the reference uses 1.5f + rand_float(0) * 0.05f as reconnect exponent */
}

/* ------------------------------------------------------------ profiler no-ops */
profiler_name_store_t *obs_get_profiler_name_store(void) { return NULL; }
const char *profile_store_name(profiler_name_store_t *s, const char *f, ...)
{
	(void)s;
	return f;
}
void profile_start(const char *n) { (void)n; }
void profile_end(const char *n) { (void)n; }

/* ------------------------------------------------------------ context layer */
bool obs_context_data_init(struct obs_context_data *c, enum obs_obj_type type, obs_data_t *settings, const char *name,
			   const char *uuid, obs_data_t *hotkey_data, bool priv)
{
	(void)settings, (void)uuid, (void)hotkey_data, (void)priv;
	memset(c, 0, sizeof(*c));
	c->type = type;
	c->signals = signal_handler_create();
	c->procs = proc_handler_create();
	c->name = bstrdup(name);
	return true;
}
void obs_context_init_control(struct obs_context_data *c, void *object, obs_destroy_cb destroy)
{
	c->control = bzalloc(sizeof(obs_weak_object_t));
	c->control->object = object;
	c->destroy = destroy;
}
void obs_context_data_insert(struct obs_context_data *c, pthread_mutex_t *m, void *first)
{
	(void)c, (void)m, (void)first;
}
void obs_context_data_remove(struct obs_context_data *c) { (void)c; }
void obs_context_data_free(struct obs_context_data *c)
{
	signal_handler_destroy(c->signals);
	proc_handler_destroy(c->procs);
	bfree(c->name);
	if (c->control)
		bfree(c->control);
	memset(c, 0, sizeof(*c));
}
void obs_weak_object_release(obs_weak_object_t *w)
{
	if (w && obs_weak_ref_release(&w->ref))
		bfree(w);
}
void obs_weak_object_addref(obs_weak_object_t *w)
{
	if (w)
		obs_weak_ref_addref(&w->ref);
}
obs_data_t *obs_data_create(void) { return NULL; }
void obs_data_release(obs_data_t *d) { (void)d; }
void obs_data_addref(obs_data_t *d) { (void)d; }

/* ------------------------------------------------------------ service */
bool obs_service_can_try_to_connect(const obs_service_t *s) { return s != NULL; }
bool obs_service_initialize(obs_service_t *s, struct obs_output *o) { (void)o; return s != NULL; }
void obs_service_activate(obs_service_t *s) { (void)s; }
void obs_service_deactivate(obs_service_t *s, bool f) { (void)s, (void)f; }

/* ------------------------------------------------------------ fake media */
struct video_output {
	struct video_output_info info;
};
struct audio_output {
	struct audio_output_info info;
};
static struct video_output eo_vid;
static struct audio_output eo_aud;

void eo_config_video(uint32_t n, uint32_t d, uint32_t w, uint32_t h)
{
	eo_vid.info.fps_num = n;
	eo_vid.info.fps_den = d;
	eo_vid.info.width = w;
	eo_vid.info.height = h;
	eo_vid.info.format = VIDEO_FORMAT_NV12;
	if (!obs)
		obs = bzalloc(sizeof(*obs));
}
void eo_config_audio(uint32_t rate)
{
	eo_aud.info.samples_per_sec = rate;
	eo_aud.info.format = AUDIO_FORMAT_FLOAT_PLANAR;
	eo_aud.info.speakers = SPEAKERS_STEREO;
}
video_t *eo_video(void) { return (video_t *)&eo_vid; }
audio_t *eo_audio(void) { return (audio_t *)&eo_aud; }
video_t *obs_get_video(void) { return (video_t *)&eo_vid; }
audio_t *obs_get_audio(void) { return (audio_t *)&eo_aud; }
bool obs_get_video_info(struct obs_video_info *ovi)
{
	memset(ovi, 0, sizeof(*ovi));
	ovi->fps_num = eo_vid.info.fps_num;
	ovi->fps_den = eo_vid.info.fps_den;
	return true;
}
const struct video_output_info *video_output_get_info(const video_t *v) { return &((const struct video_output *)v)->info; }
uint32_t video_output_get_width(const video_t *v) { return ((const struct video_output *)v)->info.width; }
uint32_t video_output_get_height(const video_t *v) { return ((const struct video_output *)v)->info.height; }
video_t *video_output_create_with_frame_rate_divisor(video_t *v, uint32_t d)
{
	(void)d;
	return v;
}
void video_output_free_frame_rate_divisor(video_t *v) { (void)v; }
const struct audio_output_info *audio_output_get_info(const audio_t *a) { return &((const struct audio_output *)a)->info; }
uint32_t audio_output_get_sample_rate(const audio_t *a) { return ((const struct audio_output *)a)->info.samples_per_sec; }
size_t audio_output_get_channels(const audio_t *a) { return get_audio_channels(((const struct audio_output *)a)->info.speakers); }
struct obs_core_video_mix *get_mix_for_video(video_t *v)
{
	(void)v;
	return NULL;
}

struct vconn {
	void (*cb)(void *, struct video_data *);
	void *param;
};
struct aconn {
	size_t mix;
	audio_output_callback_t cb;
	void *param;
};
static struct vconn vconns[16];
static size_t nv;
static struct aconn aconns[16];
static size_t na;
static pthread_mutex_t conn_mutex = PTHREAD_MUTEX_INITIALIZER;

void start_raw_video(video_t *video, const struct video_scale_info *conv, uint32_t div,
		     void (*cb)(void *, struct video_data *), void *param)
{
	(void)video, (void)conv, (void)div;
	pthread_mutex_lock(&conn_mutex);
	vconns[nv++] = (struct vconn){cb, param};
	pthread_mutex_unlock(&conn_mutex);
}
void stop_raw_video(video_t *video, void (*cb)(void *, struct video_data *), void *param)
{
	(void)video;
	pthread_mutex_lock(&conn_mutex);
	for (size_t i = 0; i < nv; i++)
		if (vconns[i].cb == cb && vconns[i].param == param) {
			memmove(&vconns[i], &vconns[i + 1], (nv - i - 1) * sizeof(vconns[0]));
			nv--;
			break;
		}
	pthread_mutex_unlock(&conn_mutex);
}
bool audio_output_connect(audio_t *a, size_t mix, const struct audio_convert_info *c, audio_output_callback_t cb, void *p)
{
	(void)a, (void)c;
	pthread_mutex_lock(&conn_mutex);
	aconns[na++] = (struct aconn){mix, cb, p};
	pthread_mutex_unlock(&conn_mutex);
	return true;
}
void audio_output_disconnect(audio_t *a, size_t mix, audio_output_callback_t cb, void *p)
{
	(void)a;
	pthread_mutex_lock(&conn_mutex);
	for (size_t i = 0; i < na; i++)
		if (aconns[i].mix == mix && aconns[i].cb == cb && aconns[i].param == p) {
			memmove(&aconns[i], &aconns[i + 1], (na - i - 1) * sizeof(aconns[0]));
			na--;
			break;
		}
	pthread_mutex_unlock(&conn_mutex);
}
size_t eo_video_conns(void) { return nv; }
size_t eo_audio_conns(void) { return na; }

/* Frames are delivered in connection order (the real video thread does the same). */
void eo_feed_video(uint64_t ts)
{
	static uint8_t plane[64];
	struct video_data f;
	for (size_t i = 0; i < nv; i++) {
		memset(&f, 0, sizeof(f));
		f.data[0] = plane;
		f.linesize[0] = 8;
		f.timestamp = ts;
		vconns[i].cb(vconns[i].param, &f);
	}
}
void eo_feed_audio(size_t mix, uint64_t ts, uint32_t frames)
{
	static float zeros[8192];
	struct audio_data d;
	if (frames > 8192)
		frames = 8192;
	for (size_t i = 0; i < na; i++) {
		if (aconns[i].mix != mix)
			continue;
		memset(&d, 0, sizeof(d));
		d.data[0] = (uint8_t *)zeros;
		d.data[1] = (uint8_t *)zeros;
		d.frames = frames;
		d.timestamp = ts;
		aconns[i].cb(aconns[i].param, mix, &d);
	}
}
