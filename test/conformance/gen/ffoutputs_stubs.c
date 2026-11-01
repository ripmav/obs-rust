/*
 * S-046 ffoutputs: fake libobs / platform world for the real obs-ffmpeg output sources.
 * Faked: obs_data (tiny typed store, user value over default), obs_output / obs_encoder / obs_service accessors,
 * signal / proc handlers (recorded and printed), the helper-process pipe (decodes the ffm_packet_info byte stream and
 * prints it), the file system (virtual: existence set only), os_generate_formatted_filename (S-023 owns the real one),
 * os_sem / os_event (gated so that a worker thread only runs when the script pumps it), pthread_create/join (see runner).
 * Test tooling, not part of libobs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <stdarg.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/pipe.h>
#include <util/threading.h>
#include <util/base.h>
#include <callback/calldata.h>
#include "ffoutputs_api.h"
#include "../../../plugins/obs-ffmpeg/ffmpeg-mux/ffmpeg-mux.h"

struct ffo_world W;
uint8_t *wire_buf;
size_t wire_len;

/* ------------------------------------------------------------------ names */
static const char *stop_name(int c)
{
	switch (c) {
	case 0: return "SUCCESS";
	case -1: return "BAD_PATH";
	case -2: return "CONNECT_FAILED";
	case -3: return "INVALID_STREAM";
	case -4: return "ERROR";
	case -5: return "DISCONNECTED";
	case -6: return "UNSUPPORTED";
	case -7: return "NO_SPACE";
	case -8: return "ENCODE_ERROR";
	}
	return "?";
}

/* ------------------------------------------------------------------ obs_data */
struct obs_data *fd_create(void)
{
	return calloc(1, sizeof(struct obs_data));
}
void fd_free(struct obs_data *d)
{
	if (!d)
		return;
	for (int i = 0; i < d->n; i++) {
		free(d->kv[i].us);
		free(d->kv[i].ds);
	}
	for (int i = 0; i < d->na; i++) {
		for (int j = 0; j < d->arrays[i]->n; j++)
			fd_free(d->arrays[i]->items[j]);
		free(d->arrays[i]);
	}
	free(d);
}
static struct kv_ent *kv_find(struct obs_data *d, const char *k, bool create)
{
	for (int i = 0; i < d->n; i++)
		if (!strcmp(d->kv[i].key, k))
			return &d->kv[i];
	if (!create || d->n >= MAXKV)
		return NULL;
	struct kv_ent *e = &d->kv[d->n++];
	snprintf(e->key, sizeof(e->key), "%s", k);
	return e;
}
const char *obs_data_get_string(obs_data_t *d, const char *k)
{
	if (!d)
		return "";
	struct kv_ent *e = kv_find(d, k, false);
	if (!e)
		return "";
	if (e->utype == 3)
		return e->us;
	if (e->dtype == 3)
		return e->ds;
	return "";
}
long long obs_data_get_int(obs_data_t *d, const char *k)
{
	if (!d)
		return 0;
	struct kv_ent *e = kv_find(d, k, false);
	if (!e)
		return 0;
	if (e->utype == 1)
		return e->ui;
	if (e->utype == 0 && e->dtype == 1)
		return e->di;
	return 0;
}
bool obs_data_get_bool(obs_data_t *d, const char *k)
{
	if (!d)
		return false;
	struct kv_ent *e = kv_find(d, k, false);
	if (!e)
		return false;
	if (e->utype == 2)
		return e->ui != 0;
	if (e->utype == 0 && e->dtype == 2)
		return e->di != 0;
	return false;
}
void obs_data_set_default_int(obs_data_t *d, const char *k, long long v)
{
	struct kv_ent *e = kv_find(d, k, true);
	if (e) {
		e->dtype = 1;
		e->di = v;
	}
}
void obs_data_set_default_bool(obs_data_t *d, const char *k, bool v)
{
	struct kv_ent *e = kv_find(d, k, true);
	if (e) {
		e->dtype = 2;
		e->di = v;
	}
}
void obs_data_set_default_string(obs_data_t *d, const char *k, const char *v)
{
	struct kv_ent *e = kv_find(d, k, true);
	if (e) {
		free(e->ds);
		e->dtype = 3;
		e->ds = strdup(v);
	}
}
void obs_data_set_bool(obs_data_t *d, const char *k, bool v)
{
	struct kv_ent *e = kv_find(d, k, true);
	if (e) {
		e->utype = 2;
		e->ui = v;
	}
}
void obs_data_set_int(obs_data_t *d, const char *k, long long v)
{
	struct kv_ent *e = kv_find(d, k, true);
	if (e) {
		e->utype = 1;
		e->ui = v;
	}
}
void obs_data_set_string(obs_data_t *d, const char *k, const char *v)
{
	struct kv_ent *e = kv_find(d, k, true);
	if (e) {
		free(e->us);
		e->utype = 3;
		e->us = strdup(v);
	}
}
void obs_data_release(obs_data_t *d)
{
	(void)d; /* the fake world owns every settings object */
}
obs_data_array_t *obs_data_get_array(obs_data_t *d, const char *k)
{
	for (int i = 0; i < d->na; i++)
		if (!strcmp(d->akey[i], k))
			return d->arrays[i];
	return NULL;
}
obs_data_t *obs_data_array_item(obs_data_array_t *a, size_t idx)
{
	return idx < (size_t)a->n ? a->items[idx] : NULL;
}
void obs_data_array_release(obs_data_array_t *a)
{
	(void)a;
}
void fd_dump(const struct obs_data *d)
{
	for (int i = 0; i < d->n; i++) {
		const struct kv_ent *e = &d->kv[i];
		printf("  %s", e->key);
		if (e->utype == 1)
			printf(" int %lld", (long long)e->ui);
		else if (e->utype == 2)
			printf(" bool %d", (int)e->ui);
		else if (e->utype == 3)
			printf(" str \"%s\"", e->us);
		if (e->dtype == 1)
			printf(" default-int %lld", (long long)e->di);
		else if (e->dtype == 2)
			printf(" default-bool %d", (int)e->di);
		else if (e->dtype == 3)
			printf(" default-str \"%s\"", e->ds);
		printf("\n");
	}
}

/* ------------------------------------------------------------------ module / misc */
const char *obs_module_text(const char *k)
{
	/* the real text is locale data; only this key is expanded because the code post-processes it ("%1" -> path) */
	if (!strcmp(k, "UnableToWritePath"))
		return "UnableToWritePath:%1";
	return k;
}
const char *obs_get_version_string(void)
{
	return "33.0.1";
}
uint64_t os_gettime_ns(void)
{
	return W.now_ns;
}
char *os_get_executable_path_ptr(const char *name)
{
	char b[64];
	snprintf(b, sizeof(b), "<bin>/%s", name);
	return bstrdup(b);
}
float obs_get_video_hdr_nominal_peak_level(void)
{
	return W.hdr_peak;
}

/* ------------------------------------------------------------------ video / audio info */
static int dummy_video, dummy_audio;
video_t *obs_get_video(void)
{
	return (video_t *)&dummy_video;
}
video_t *obs_output_video(const obs_output_t *o)
{
	(void)o;
	return (video_t *)&dummy_video;
}
audio_t *obs_get_audio(void)
{
	return (audio_t *)&dummy_audio;
}
const struct video_output_info *video_output_get_info(const video_t *v)
{
	(void)v;
	static struct video_output_info vi;
	vi.format = (enum video_format)W.vformat;
	vi.fps_num = W.fps_num;
	vi.fps_den = W.fps_den;
	vi.colorspace = (enum video_colorspace)W.colorspace;
	vi.range = (enum video_range_type)W.range;
	return &vi;
}
enum video_format video_output_get_format(const video_t *v)
{
	(void)v;
	return (enum video_format)W.vformat;
}
size_t audio_output_get_channels(const audio_t *a)
{
	(void)a;
	return W.channels;
}
bool obs_get_video_info(struct obs_video_info *ovi)
{
	if (!W.have_video_info)
		return false;
	memset(ovi, 0, sizeof(*ovi));
	ovi->fps_num = W.fps_num;
	ovi->fps_den = W.fps_den;
	ovi->base_width = W.base_w;
	ovi->base_height = W.base_h;
	return true;
}
bool obs_get_audio_info(struct obs_audio_info *oai)
{
	if (!W.have_audio_info)
		return false;
	memset(oai, 0, sizeof(*oai));
	oai->samples_per_sec = W.sample_rate;
	oai->speakers = (enum speaker_layout)W.speakers;
	return true;
}

/* ------------------------------------------------------------------ encoders */
const char *obs_encoder_get_codec(const obs_encoder_t *e)
{
	return e->codec;
}
const char *obs_encoder_get_name(const obs_encoder_t *e)
{
	return e->name;
}
obs_data_t *obs_encoder_get_settings(const obs_encoder_t *e)
{
	return e->settings;
}
uint32_t obs_encoder_get_sample_rate(const obs_encoder_t *e)
{
	return e->sample_rate;
}
size_t obs_encoder_get_frame_size(const obs_encoder_t *e)
{
	return e->frame_size;
}
uint32_t obs_encoder_get_priming_samples(const obs_encoder_t *e)
{
	return e->priming;
}
bool obs_encoder_paused(const obs_encoder_t *e)
{
	return e->paused;
}
void obs_encoder_update(obs_encoder_t *e, obs_data_t *s)
{
	(void)s;
	e->updates++;
	printf("encoder_update codec=%s repeat_headers=%d\n", e->codec,
	       (int)obs_data_get_bool(e->settings, "repeat_headers"));
}
bool obs_encoder_get_extra_data(const obs_encoder_t *e, uint8_t **d, size_t *n)
{
	if (!e->has_extra)
		return false;
	*d = (uint8_t *)e->extra;
	*n = e->extra_size;
	return true;
}
void obs_encoder_packet_ref(struct encoder_packet *dst, struct encoder_packet *src)
{
	*dst = *src;
	W.pkt_refs++;
}
void obs_encoder_packet_release(struct encoder_packet *p)
{
	(void)p;
	W.pkt_refs--;
}

/* ------------------------------------------------------------------ output */
const char *obs_output_get_name(const obs_output_t *o)
{
	return o->name;
}
uint32_t obs_output_get_flags(const obs_output_t *o)
{
	return o->flags;
}
obs_data_t *obs_output_get_settings(const obs_output_t *o)
{
	return o->settings;
}
obs_encoder_t *obs_output_get_video_encoder(const obs_output_t *o)
{
	return o->venc;
}
obs_encoder_t *obs_output_get_audio_encoder(const obs_output_t *o, size_t idx)
{
	return idx < (size_t)o->naenc ? (obs_encoder_t *)&o->aenc[idx] : NULL;
}
obs_service_t *obs_output_get_service(const obs_output_t *o)
{
	return (obs_service_t *)o->service;
}
const char *obs_service_get_connect_info(const obs_service_t *s, uint32_t type)
{
	const struct obs_service *sv = (const struct obs_service *)s;
	return type == OBS_SERVICE_CONNECT_INFO_SERVER_URL ? sv->url
	       : type == OBS_SERVICE_CONNECT_INFO_STREAM_KEY ? sv->key
							      : NULL;
}
uint32_t obs_output_get_width(const obs_output_t *o)
{
	return o->width;
}
uint32_t obs_output_get_height(const obs_output_t *o)
{
	return o->height;
}
size_t obs_output_get_mixers(const obs_output_t *o)
{
	return o->mixers;
}
uint64_t obs_output_get_pause_offset(obs_output_t *o)
{
	return o->pause_offset;
}
bool obs_output_can_begin_data_capture(const obs_output_t *o, uint32_t f)
{
	(void)f;
	return o->can_begin;
}
bool obs_output_initialize_encoders(obs_output_t *o, uint32_t f)
{
	(void)f;
	return o->init_enc_ok;
}
bool obs_output_begin_data_capture(obs_output_t *o, uint32_t f)
{
	(void)f;
	o->begin_calls++;
	printf("begin_data_capture\n");
	return true;
}
void obs_output_end_data_capture(obs_output_t *o)
{
	o->end_calls++;
	printf("end_data_capture\n");
}
void obs_output_signal_stop(obs_output_t *o, int code)
{
	(void)o;
	printf("signal_stop %s\n", stop_name(code));
}
void obs_output_set_last_error(obs_output_t *o, const char *m)
{
	(void)o;
	snprintf(W.last_error, sizeof(W.last_error), "%s", m);
	printf("last_error \"%s\"\n", m);
}
void obs_output_set_video_conversion(obs_output_t *o, const struct video_scale_info *c)
{
	(void)o;
	printf("set_video_conversion %s\n", c ? "set" : "NULL");
}
void obs_output_set_audio_conversion(obs_output_t *o, const struct audio_convert_info *c)
{
	(void)o;
	printf("set_audio_conversion format=%d\n", (int)c->format);
}

/* ------------------------------------------------------------------ signals / procs */
static int dummy_sh;
signal_handler_t *obs_output_get_signal_handler(const obs_output_t *o)
{
	(void)o;
	return (signal_handler_t *)&dummy_sh;
}
proc_handler_t *obs_output_get_proc_handler(const obs_output_t *o)
{
	return (proc_handler_t *)o; /* the output doubles as its own proc handler */
}
bool signal_handler_add(signal_handler_t *h, const char *decl)
{
	(void)h;
	printf("signal_add \"%s\"\n", decl);
	return true;
}
void signal_handler_signal(signal_handler_t *h, const char *name, calldata_t *cd)
{
	(void)h;
	printf("signal %s", name);
	const char *s = NULL;
	if (cd && calldata_get_string(cd, "next_file", &s))
		printf(" next_file=\"%s\"", s);
	printf("\n");
}
void proc_handler_add(proc_handler_t *h, const char *decl, proc_handler_proc_t fn, void *param)
{
	struct obs_output *o = (struct obs_output *)h;
	struct proc_rec *p = &o->procs[o->nprocs++];
	/* name = word before '(' */
	const char *paren = strchr(decl, '(');
	const char *sp = paren;
	while (sp > decl && sp[-1] != ' ')
		sp--;
	snprintf(p->name, sizeof(p->name), "%.*s", (int)(paren - sp), sp);
	p->fn = fn;
	p->param = param;
	printf("proc_add \"%s\"\n", decl);
}

/* ------------------------------------------------------------------ properties */
obs_properties_t *obs_properties_create(void)
{
	static int p;
	return (obs_properties_t *)&p;
}
obs_property_t *obs_properties_add_text(obs_properties_t *p, const char *n, const char *d, enum obs_text_type t)
{
	(void)p;
	printf("property text name=%s desc=%s type=%d\n", n, d, (int)t);
	return NULL;
}

/* ------------------------------------------------------------------ virtual file system */
bool os_file_exists(const char *p)
{
	for (int i = 0; i < W.nfiles; i++)
		if (!strcmp(W.files[i], p))
			return true;
	return false;
}
int os_mkdirs(const char *p)
{
	printf("mkdirs \"%s\"\n", p);
	return 0;
}
FILE *os_fopen(const char *p, const char *m)
{
	(void)m;
	for (int i = 0; i < W.ndeny; i++)
		if (!strcmp(W.deny[i], p))
			return NULL;
	return fopen("/dev/null", "wb");
}
int os_unlink(const char *p)
{
	printf("unlink \"%s\"\n", p);
	return 0;
}
static int name_counter;
char *os_generate_formatted_filename(const char *ext, bool space, const char *fmt)
{
	snprintf(W.fmt_seen, sizeof(W.fmt_seen), "%s", fmt);
	char out[400], tmp[400];
	snprintf(tmp, sizeof(tmp), "%s", fmt);
	char *n = strstr(tmp, "%N");
	if (n) {
		char c[16];
		snprintf(c, sizeof(c), "%d", ++name_counter);
		snprintf(out, sizeof(out), "%.*s%s%s", (int)(n - tmp), tmp, c, n + 2);
	} else {
		snprintf(out, sizeof(out), "%s", tmp);
	}
	if (!space)
		for (char *p = out; *p; p++)
			if (*p == ' ')
				*p = '_';
	if (ext && *ext) {
		strcat(out, ".");
		strcat(out, ext);
	}
	return bstrdup(out);
}

/* ------------------------------------------------------------------ pipe */
struct os_process_pipe {
	int dummy;
};
static struct os_process_pipe the_pipe;
static int wstate; /* 0 expect info, 1 expect payload */
static struct ffm_packet_info cur_info;

os_process_pipe_t *os_process_pipe_create2(const os_process_args_t *args, const char *type)
{
	W.pipe_creates++;
	if (W.pipe_fail) {
		printf("pipe create FAIL\n");
		return NULL;
	}
	printf("pipe create type=%s\n", type);
	char **argv = os_process_args_get_argv(args);
	size_t argc = os_process_args_get_argc((struct os_process_args *)args);
	for (size_t i = 0; i < argc; i++)
		printf("  argv[%zu] \"%s\"\n", i, argv[i]);
	W.pipe_writes = 0;
	wstate = 0;
	return &the_pipe;
}
int os_process_pipe_destroy(os_process_pipe_t *p)
{
	if (!p)
		return -1; /* never reached by the sources in a way that matters */
	printf("pipe destroy -> %d\n", W.destroy_ret);
	return W.destroy_ret;
}
size_t os_process_pipe_read_err(os_process_pipe_t *p, uint8_t *d, size_t n)
{
	(void)p;
	size_t l = strlen(W.err_text);
	if (l > n)
		l = n;
	memcpy(d, W.err_text, l);
	return l;
}
size_t os_process_pipe_write(os_process_pipe_t *p, const uint8_t *d, size_t n)
{
	(void)p;
	if (n == 0)
		return 0; /* an empty payload write: nothing to decode */
	W.pipe_writes++;
	if (W.write_fail_at && W.pipe_writes == W.write_fail_at) {
		printf("pipe write #%d FAIL\n", W.pipe_writes);
		return 0;
	}
	if (wstate == 0) {
		if (n != sizeof(cur_info)) {
			printf("wire BAD_INFO_LEN %zu\n", n);
			return n;
		}
		memcpy(&cur_info, d, sizeof(cur_info));
		if (cur_info.type == FFM_PACKET_CHANGE_FILE || cur_info.size) {
			wstate = 1;
			return n;
		}
		printf("wire %s idx=%d key=%d pts=%lld dts=%lld size=0\n",
		       cur_info.type == FFM_PACKET_VIDEO ? "V" : "A", cur_info.index, (int)cur_info.keyframe,
		       (long long)cur_info.pts, (long long)cur_info.dts);
		return n;
	}
	wstate = 0;
	if (cur_info.type == FFM_PACKET_CHANGE_FILE) {
		printf("wire CHANGE_FILE \"%.*s\"\n", (int)n, (const char *)d);
		return n;
	}
	printf("wire %s idx=%d key=%d pts=%lld dts=%lld size=%u data=", cur_info.type == FFM_PACKET_VIDEO ? "V" : "A",
	       cur_info.index, (int)cur_info.keyframe, (long long)cur_info.pts, (long long)cur_info.dts, cur_info.size);
	for (size_t i = 0; i < n && i < 6; i++)
		printf("%02x", d[i]);
	printf("\n");
	return n;
}

/* ------------------------------------------------------------------ gated semaphore / event */
struct os_sem_data {
	int count;
};
struct os_event_data {
	int set;
};
static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t GC = PTHREAD_COND_INITIALIZER;
static int g_tokens, g_blocked, g_events, g_running;
static struct os_sem_data *g_wsem;

int os_sem_init(os_sem_t **s, int v)
{
	*s = calloc(1, sizeof(**s));
	(*s)->count = v;
	return 0;
}
void os_sem_destroy(os_sem_t *s)
{
	free(s);
}
int os_sem_post(os_sem_t *s)
{
	pthread_mutex_lock(&G);
	s->count++;
	pthread_cond_broadcast(&GC);
	pthread_mutex_unlock(&G);
	return 0;
}
int os_sem_wait(os_sem_t *s)
{
	pthread_mutex_lock(&G);
	g_blocked++;
	g_wsem = s;
	pthread_cond_broadcast(&GC);
	while (!(g_events > 0 || (s->count > 0 && g_tokens > 0)))
		pthread_cond_wait(&GC, &G);
	g_blocked--;
	if (!(g_events > 0)) {
		s->count--;
		g_tokens--;
	}
	pthread_mutex_unlock(&G);
	return 0;
}
int os_event_init(os_event_t **e, enum os_event_type t)
{
	(void)t;
	*e = calloc(1, sizeof(**e));
	return 0;
}
void os_event_destroy(os_event_t *e)
{
	free(e);
}
int os_event_signal(os_event_t *e)
{
	pthread_mutex_lock(&G);
	if (!e->set) {
		e->set = 1;
		g_events++;
	}
	pthread_cond_broadcast(&GC);
	pthread_mutex_unlock(&G);
	return 0;
}
int os_event_try(os_event_t *e)
{
	int r = 1; /* EAGAIN-like: not signalled */
	pthread_mutex_lock(&G);
	if (e->set) {
		e->set = 0;
		g_events--;
		r = 0;
	}
	pthread_mutex_unlock(&G);
	return r;
}

void ffo_thread_started(void)
{
	pthread_mutex_lock(&G);
	g_running++;
	pthread_mutex_unlock(&G);
}
void ffo_thread_ended(void)
{
	pthread_mutex_lock(&G);
	g_running--;
	pthread_cond_broadcast(&GC);
	pthread_mutex_unlock(&G);
}
/* Let the worker run up to n queue items, then wait until it is idle again (or gone). */
void ffo_gate_pump(int n)
{
	pthread_mutex_lock(&G);
	g_tokens += n;
	pthread_cond_broadcast(&GC);
	while (!(g_running == 0 || (g_blocked > 0 && (g_tokens == 0 || (g_wsem && g_wsem->count == 0)))))
		pthread_cond_wait(&GC, &G);
	g_tokens = 0;
	pthread_mutex_unlock(&G);
}

/* ------------------------------------------------------------------ thread wrappers */
int ffo_thread_mode;
void *(*ffo_pending_fn)(void *);
void *ffo_pending_arg;
static pthread_t real_tids[4];
static int n_real;
static int n_fake;
struct tramp {
	void *(*fn)(void *);
	void *arg;
};
static void *trampoline(void *p)
{
	struct tramp t = *(struct tramp *)p;
	free(p);
	void *r = t.fn(t.arg);
	ffo_thread_ended();
	return r;
}
int ffo_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
	(void)a;
	if (ffo_thread_mode == 2) {
		struct tramp *tp = malloc(sizeof(*tp));
		tp->fn = fn;
		tp->arg = arg;
		ffo_thread_started();
		int r = pthread_create(t, NULL, trampoline, tp);
		if (r == 0 && n_real < 4)
			real_tids[n_real++] = *t;
		return r;
	}
	*t = (pthread_t)(0xFEED0000u + (unsigned)(n_fake++));
	if (ffo_thread_mode == 1) {
		ffo_pending_fn = fn;
		ffo_pending_arg = arg;
		return 0;
	}
	fn(arg);
	return 0;
}
void ffo_run_pending(void)
{
	void *(*fn)(void *) = ffo_pending_fn;
	ffo_pending_fn = NULL;
	if (fn)
		fn(ffo_pending_arg);
}
int ffo_pthread_join(pthread_t t, void **ret)
{
	if (ret)
		*ret = NULL;
	for (int i = 0; i < n_real; i++)
		if (pthread_equal(real_tids[i], t)) {
			if (pthread_equal(t, pthread_self()))
				return 35; /* EDEADLK: joining oneself */
			int r = pthread_join(t, NULL);
			real_tids[i] = real_tids[--n_real];
			return r;
		}
	ffo_run_pending(); /* a deferred "thread" completes when it is joined */
	return 0;
}
