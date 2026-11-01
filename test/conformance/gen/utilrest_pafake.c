/*
 * S-055 harness: a fake libpulse. Replaces the server connection (threaded mainloop, context, operations, stream)
 * with a synchronous in-process model; the pure helpers (proplist, sample spec, channel map) come from the real
 * libpulse. Every operation completes inside the call that starts it. Test tooling, not part of libobs.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pulse/pulseaudio.h>
#include "utilrest_api.h"

struct ur_pa_cfg ur_pa;
int ur_pa_null_mainloop_calls, ur_pa_contexts_created, ur_pa_contexts_freed, ur_pa_loops_created, ur_pa_loops_freed;

void ur_pa_reset(void)
{
	memset(&ur_pa, 0, sizeof(ur_pa));
	ur_pa.begin_ret = 0;
}

struct pa_threaded_mainloop { pthread_mutex_t m; };
struct pa_context { pa_context_state_t state; pa_context_notify_cb_t cb; void *cbdata; };
struct pa_operation { pa_operation_state_t state; };
struct pa_stream {
	int refs; bool corked; bool connected;
	pa_stream_request_cb_t wcb; void *wdata; pa_stream_notify_cb_t ucb; void *udata;
};

#define LOOP_OK(m) ((m) ? 1 : (ur_pa_null_mainloop_calls++, 0))

pa_threaded_mainloop *pa_threaded_mainloop_new(void)
{
	pa_threaded_mainloop *m = calloc(1, sizeof(*m));
	pthread_mutexattr_t a;
	pthread_mutexattr_init(&a);
	pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&m->m, &a);
	ur_pa_loops_created++;
	return m;
}
int pa_threaded_mainloop_start(pa_threaded_mainloop *m) { return LOOP_OK(m) ? 0 : -1; }
void pa_threaded_mainloop_stop(pa_threaded_mainloop *m) { LOOP_OK(m); }
void pa_threaded_mainloop_free(pa_threaded_mainloop *m) { if (m) { free(m); ur_pa_loops_freed++; } else LOOP_OK(m); }
pa_mainloop_api *pa_threaded_mainloop_get_api(pa_threaded_mainloop *m) { LOOP_OK(m); return (pa_mainloop_api *)m; }
void pa_threaded_mainloop_lock(pa_threaded_mainloop *m) { if (LOOP_OK(m)) pthread_mutex_lock(&m->m); }
void pa_threaded_mainloop_unlock(pa_threaded_mainloop *m) { if (LOOP_OK(m)) pthread_mutex_unlock(&m->m); }
void pa_threaded_mainloop_wait(pa_threaded_mainloop *m) { LOOP_OK(m); }
void pa_threaded_mainloop_signal(pa_threaded_mainloop *m, int w) { (void)w; LOOP_OK(m); }
void pa_threaded_mainloop_accept(pa_threaded_mainloop *m) { LOOP_OK(m); }

pa_context *pa_context_new_with_proplist(pa_mainloop_api *api, const char *name, const pa_proplist *p)
{
	(void)api; (void)name; (void)p;
	ur_pa_contexts_created++;
	return calloc(1, sizeof(pa_context));
}
void pa_context_set_state_callback(pa_context *c, pa_context_notify_cb_t cb, void *d) { c->cb = cb; c->cbdata = d; }
int pa_context_connect(pa_context *c, const char *server, pa_context_flags_t f, const pa_spawn_api *s)
{
	(void)server; (void)f; (void)s;
	c->state = ur_pa.connect_fails ? PA_CONTEXT_FAILED : PA_CONTEXT_READY;
	if (c->cb) c->cb(c, c->cbdata);
	return 0;
}
pa_context_state_t pa_context_get_state(const pa_context *c) { return c ? c->state : PA_CONTEXT_FAILED; }
void pa_context_disconnect(pa_context *c) { c->state = PA_CONTEXT_TERMINATED; }
void pa_context_unref(pa_context *c) { ur_pa_contexts_freed++; free(c); }

static pa_operation *new_op(void)
{
	if (ur_pa.ops_fail) return NULL;
	pa_operation *o = calloc(1, sizeof(*o));
	o->state = PA_OPERATION_DONE;
	return o;
}
pa_operation_state_t pa_operation_get_state(const pa_operation *o) { return o->state; }
void pa_operation_unref(pa_operation *o) { free(o); }

static void fill_sink(pa_sink_info *i, const struct ur_pa_sink *s)
{
	memset(i, 0, sizeof(*i));
	i->name = s->name; i->description = s->desc;
	i->sample_spec.format = (pa_sample_format_t)s->format;
	i->sample_spec.rate = s->rate; i->sample_spec.channels = s->channels;
}
pa_operation *pa_context_get_sink_info_list(pa_context *c, pa_sink_info_cb_t cb, void *d)
{
	pa_operation *o = new_op();
	if (!o) return NULL;
	for (int k = 0; k < ur_pa.nsinks; k++) { pa_sink_info i; fill_sink(&i, &ur_pa.sinks[k]); cb(c, &i, 0, d); }
	cb(c, NULL, 1, d);
	return o;
}
pa_operation *pa_context_get_sink_info_by_name(pa_context *c, const char *name, pa_sink_info_cb_t cb, void *d)
{
	pa_operation *o = new_op();
	if (!o) return NULL;
	for (int k = 0; k < ur_pa.nsinks; k++)
		if (!strcmp(ur_pa.sinks[k].name, name)) {
			pa_sink_info i; fill_sink(&i, &ur_pa.sinks[k]);
			cb(c, &i, 0, d); cb(c, NULL, 1, d);
			return o;
		}
	cb(c, NULL, -1, d); /* unknown sink: error callback */
	return o;
}
pa_operation *pa_context_get_server_info(pa_context *c, pa_server_info_cb_t cb, void *d)
{
	pa_operation *o = new_op();
	if (!o) return NULL;
	pa_server_info i; memset(&i, 0, sizeof(i));
	i.server_name = ur_pa.server_name; i.server_version = ur_pa.server_version;
	i.default_sink_name = ur_pa.default_sink_null ? NULL : ur_pa.default_sink;
	cb(c, &i, d);
	return o;
}
/* source queries are not used by the monitor; present for link completeness */
pa_operation *pa_context_get_source_info_list(pa_context *c, pa_source_info_cb_t cb, void *d)
{ pa_operation *o = new_op(); if (o) cb(c, NULL, 1, d); return o; }
pa_operation *pa_context_get_source_info_by_name(pa_context *c, const char *n, pa_source_info_cb_t cb, void *d)
{ (void)n; pa_operation *o = new_op(); if (o) cb(c, NULL, 1, d); return o; }

/* ---------------------------------------------------------------- stream */
static uint8_t wbuf[1 << 16];
void ur_pa_log(const char *fmt, ...);
pa_stream *pa_stream_new_with_proplist(pa_context *c, const char *name, const pa_sample_spec *ss, const pa_channel_map *map, pa_proplist *p)
{
	(void)c; (void)p;
	if (ur_pa.stream_new_fails) return NULL;
	pa_stream *s = calloc(1, sizeof(*s));
	s->refs = 1;
	char sp[PA_SAMPLE_SPEC_SNPRINT_MAX];
	pa_sample_spec_snprint(sp, sizeof(sp), ss);
	ur_pa_log("stream_new \"%s\" spec=%s map_channels=%u map0=%d map3=%d map4=%d", name, sp, map->channels, map->map[0], map->map[3], map->map[4]);
	return s;
}
int pa_stream_connect_playback(pa_stream *s, const char *dev, const pa_buffer_attr *a, pa_stream_flags_t f, const pa_cvolume *v, pa_stream *sync)
{
	(void)v; (void)sync;
	ur_pa_log("stream_connect dev=\"%s\" tlength=%u fragsize=%u maxlength=%u minreq=%u prebuf=%u flags=0x%x", dev, a->tlength, a->fragsize, a->maxlength, a->minreq, a->prebuf, (unsigned)f);
	if (ur_pa.connect_ret < 0) return ur_pa.connect_ret;
	s->connected = true;
	s->corked = (f & PA_STREAM_START_CORKED) != 0;
	return 0;
}
void pa_stream_set_write_callback(pa_stream *s, pa_stream_request_cb_t cb, void *d) { s->wcb = cb; s->wdata = d; ur_pa_log("stream_set_write_callback %s", cb ? "set" : "cleared"); }
void pa_stream_set_underflow_callback(pa_stream *s, pa_stream_notify_cb_t cb, void *d) { s->ucb = cb; s->udata = d; }
int pa_stream_disconnect(pa_stream *s) { s->connected = false; ur_pa_log("stream_disconnect"); return 0; }
void pa_stream_unref(pa_stream *s) { ur_pa_log("stream_unref"); if (--s->refs <= 0) free(s); }
pa_operation *pa_stream_set_buffer_attr(pa_stream *s, const pa_buffer_attr *a, pa_stream_success_cb_t cb, void *d)
{
	(void)s; (void)cb; (void)d;
	ur_pa_log("stream_set_buffer_attr tlength=%u fragsize=%u maxlength=%u minreq=%u prebuf=%u", a->tlength, a->fragsize, a->maxlength, a->minreq, a->prebuf);
	return NULL;
}
int pa_stream_is_corked(const pa_stream *s) { return s->corked; }
pa_operation *pa_stream_cork(pa_stream *s, int b, pa_stream_success_cb_t cb, void *d)
{ (void)cb; (void)d; s->corked = b; ur_pa_log("stream_cork %d", b); return NULL; }
int pa_stream_begin_write(pa_stream *s, void **data, size_t *nbytes)
{
	(void)s;
	if (ur_pa.begin_ret) { ur_pa_log("stream_begin_write fail"); return ur_pa.begin_ret; }
	if (ur_pa.begin_size) *nbytes = ur_pa.begin_size;
	if (*nbytes > sizeof(wbuf)) *nbytes = sizeof(wbuf);
	*data = wbuf;
	return 0;
}
int pa_stream_cancel_write(pa_stream *s) { (void)s; ur_pa_log("stream_cancel_write"); return 0; }
int pa_stream_write(pa_stream *s, const void *data, size_t n, pa_free_cb_t f, int64_t off, pa_seek_mode_t m)
{
	(void)s; (void)f; (void)off; (void)m;
	char hex[200] = "";
	for (size_t i = 0; i < n && i < 24; i++) sprintf(hex + 2 * i, "%02x", ((const uint8_t *)data)[i]);
	ur_pa_log("stream_write bytes=%zu first=%s%s", n, n ? hex : "-", n > 24 ? "..." : "");
	return 0;
}
