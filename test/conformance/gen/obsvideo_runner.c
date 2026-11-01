/*
 * Conformance runner for area obsvideo (spec S-058), part 1: the graphics-thread frame loop.
 *
 * The REAL libobs/obs-video.c and libobs/obs-video-gpu-encode.c are #included here (their file-local
 * functions are exercised through obs_graphics_thread_loop / obs_graphics_thread / init_gpu_encoding /
 * the gpu encode thread body). Everything those files reach outside themselves is a recording fake:
 * the graphics device (gs_*), the video output (video_output_*), effects, sources, encoders, the view
 * renderer, the profiler, the clock, thread creation and the encode semaphore. The clock is scripted
 * so that frame timing is deterministic; the gpu encode thread body is run synchronously on the
 * runner thread with a token budget. The Linux platform glue is in obsvideo_nix.c (part 2).
 * Test tooling, not part of libobs.
 */
#include <inttypes.h>
#include <pthread.h>
#include <stddef.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "obs-internal.h"
#include "graphics/graphics.h"
#include "graphics/vec4.h"
#include "media-io/video-io.h"
#include "media-io/video-frame.h"
#include "util/platform.h"
#include "obsvideo_api.h"

/* ---- scripted clock / sleep / semaphore / thread creation (macros apply to the included files only) ---- */
static uint64_t g_now;
static int g_sleep_result = 1;      /* os_sleepto_ns result when not scripted per call */
static int g_sleep_seq[64];         /* optional per-call results (consumed in order) */
static int g_sleep_seq_n, g_sleep_seq_i;
static int g_sem_budget;            /* tokens the encode thread may consume before its wait fails */
static int g_sem_posts;
static int g_pthread_create_result;
static int g_pthread_joins;
static bool g_trace = true;

static void ev(const char *fmt, ...)
{
	if (!g_trace)
		return;
	va_list ap;
	va_start(ap, fmt);
	fputs("ev ", stdout);
	vprintf(fmt, ap);
	fputc('\n', stdout);
	va_end(ap);
}

static uint64_t g_step; /* the clock advances by this much after every os_gettime_ns call */
static uint64_t fake_gettime_ns(void)
{
	uint64_t r = g_now;
	g_now += g_step;
	return r;
}

static bool fake_sleepto_ns(uint64_t t)
{
	bool r = g_sleep_result;
	if (g_sleep_seq_i < g_sleep_seq_n)
		r = g_sleep_seq[g_sleep_seq_i++];
	ev("sleepto %" PRIu64 " -> %d", t, (int)r);
	if (r)
		g_now = t;
	return r;
}

static int fake_sem_wait(os_sem_t *sem)
{
	(void)sem;
	if (g_sem_budget > 0) {
		g_sem_budget--;
		return 0;
	}
	return -1;
}

static int fake_sem_post(os_sem_t *sem)
{
	(void)sem;
	g_sem_posts++;
	ev("sem_post");
	return 0;
}

static void *(*g_thread_fn)(void *);
static void *g_thread_arg;
static int fake_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
	(void)a;
	*t = 0;
	g_thread_fn = fn;
	g_thread_arg = arg;
	ev("pthread_create -> %d", g_pthread_create_result);
	return g_pthread_create_result;
}

static int fake_pthread_join(pthread_t t, void **r)
{
	(void)t;
	(void)r;
	g_pthread_joins++;
	ev("pthread_join");
	return 0;
}

#define os_gettime_ns fake_gettime_ns
#define os_sleepto_ns fake_sleepto_ns
#define os_sem_wait fake_sem_wait
#define os_sem_post fake_sem_post
#define pthread_create fake_pthread_create
#define pthread_join fake_pthread_join

/* ---- fake graphics objects ---- */
struct ftex {
	char name[48];
	uint32_t w, h;
};
struct fsurf {
	char name[48];
	int ch;
	uint32_t linesize;
	uint32_t rows;
	uint8_t *buf;
	bool fail_map;
	bool mapped;
};
struct feffect {
	char name[24];
	bool base_dim;   /* has base_dimension / base_dimension_i params */
	int passes;
	int loop;
};
struct fparam {
	struct feffect *e;
	char name[48];
};
struct ftech {
	struct feffect *e;
	char name[24];
};

static int g_ntex;
static gs_texture_t *tex_new(const char *name, uint32_t w, uint32_t h)
{
	struct ftex *t = bzalloc(sizeof(*t));
	snprintf(t->name, sizeof t->name, "%s", name);
	t->w = w;
	t->h = h;
	g_ntex++;
	return (gs_texture_t *)t;
}
#define TN(t) ((t) ? ((struct ftex *)(t))->name : "null")

static struct feffect *effect_new(const char *name, bool base_dim)
{
	struct feffect *e = bzalloc(sizeof(*e));
	snprintf(e->name, sizeof e->name, "%s", name);
	e->base_dim = base_dim;
	e->passes = 1;
	return e;
}
#define EN(e) ((e) ? ((struct feffect *)(e))->name : "null")

static int g_ctx_depth, g_ctx_underflow;
static int g_dest_tex;
static int g_stage_fail_map_all;

void gs_enter_context(graphics_t *g)
{
	(void)g;
	g_ctx_depth++;
	ev("gs_enter_context");
}
void gs_leave_context(void)
{
	if (--g_ctx_depth < 0) {
		g_ctx_underflow++;
		g_ctx_depth = 0;
	}
	ev("gs_leave_context");
}
void gs_begin_frame(void) { ev("gs_begin_frame"); }
void gs_begin_scene(void) { ev("gs_begin_scene"); }
void gs_end_scene(void) { ev("gs_end_scene"); }
void gs_flush(void) { ev("gs_flush"); }
void gs_enable_depth_test(bool e) { ev("gs_enable_depth_test %d", (int)e); }
void gs_enable_blending(bool e) { ev("gs_enable_blending %d", (int)e); }
void gs_enable_framebuffer_srgb(bool e) { ev("gs_enable_framebuffer_srgb %d", (int)e); }
void gs_set_cull_mode(enum gs_cull_mode m) { ev("gs_set_cull_mode %d", (int)m); }
void gs_ortho(float l, float r, float t, float b, float zn, float zf)
{
	ev("gs_ortho %g %g %g %g %g %g", l, r, t, b, zn, zf);
}
void gs_set_viewport(int x, int y, int w, int h) { ev("gs_set_viewport %d %d %d %d", x, y, w, h); }
void gs_set_render_target(gs_texture_t *t, gs_zstencil_t *z)
{
	(void)z;
	ev("gs_set_render_target %s", TN(t));
}
void gs_set_render_target_with_color_space(gs_texture_t *t, gs_zstencil_t *z, enum gs_color_space cs)
{
	(void)z;
	ev("gs_set_render_target_cs %s cs=%d", TN(t), (int)cs);
}
void gs_clear(uint32_t flags, const struct vec4 *c, float depth, uint8_t stencil)
{
	ev("gs_clear flags=%u color=%g,%g,%g,%g depth=%g stencil=%u", flags, c->x, c->y, c->z, c->w, depth,
	   (unsigned)stencil);
}
uint32_t gs_texture_get_width(const gs_texture_t *t) { return ((const struct ftex *)t)->w; }
uint32_t gs_texture_get_height(const gs_texture_t *t) { return ((const struct ftex *)t)->h; }
void gs_texture_destroy(gs_texture_t *t)
{
	ev("gs_texture_destroy %s", TN(t));
}
bool gs_texture_create_nv12(gs_texture_t **y, gs_texture_t **uv, uint32_t w, uint32_t h, uint32_t flags)
{
	char n[48];
	if (g_dest_tex < 0) { /* scripted failure */
		*y = *uv = NULL;
		ev("gs_texture_create_nv12 %ux%u flags=%u -> fail", w, h, flags);
		return false;
	}
	snprintf(n, sizeof n, "enc%d.y", g_dest_tex);
	*y = tex_new(n, w, h);
	snprintf(n, sizeof n, "enc%d.uv", g_dest_tex++);
	*uv = tex_new(n, w / 2, h / 2);
	ev("gs_texture_create_nv12 %ux%u flags=%u", w, h, flags);
	return true;
}
bool gs_texture_create_p010(gs_texture_t **y, gs_texture_t **uv, uint32_t w, uint32_t h, uint32_t flags)
{
	char n[48];
	if (g_dest_tex < 0) {
		*y = *uv = NULL;
		ev("gs_texture_create_p010 %ux%u flags=%u -> fail", w, h, flags);
		return false;
	}
	snprintf(n, sizeof n, "enc%d.y", g_dest_tex);
	*y = tex_new(n, w, h);
	snprintf(n, sizeof n, "enc%d.uv", g_dest_tex++);
	*uv = tex_new(n, w / 2, h / 2);
	ev("gs_texture_create_p010 %ux%u flags=%u", w, h, flags);
	return true;
}
void gs_stage_texture(gs_stagesurf_t *s, gs_texture_t *t)
{
	ev("gs_stage_texture %s <- %s", ((struct fsurf *)s)->name, TN(t));
}
void gs_copy_texture(gs_texture_t *dst, gs_texture_t *src)
{
	ev("gs_copy_texture %s <- %s", TN(dst), TN(src));
}
bool gs_stagesurface_map(gs_stagesurf_t *s, uint8_t **data, uint32_t *linesize)
{
	struct fsurf *f = (struct fsurf *)s;
	if (f->fail_map || g_stage_fail_map_all) {
		ev("gs_stagesurface_map %s -> fail", f->name);
		return false;
	}
	f->mapped = true;
	*data = f->buf;
	*linesize = f->linesize;
	ev("gs_stagesurface_map %s linesize=%u", f->name, f->linesize);
	return true;
}
void gs_stagesurface_unmap(gs_stagesurf_t *s)
{
	struct fsurf *f = (struct fsurf *)s;
	f->mapped = false;
	ev("gs_stagesurface_unmap %s", f->name);
}
void gs_debug_marker_begin(const float c[4], const char *n)
{
	(void)c;
	ev("marker_begin %s", n);
}
void gs_debug_marker_end(void) { ev("marker_end"); }

gs_eparam_t *gs_effect_get_param_by_name(const gs_effect_t *e, const char *name)
{
	struct feffect *fe = (struct feffect *)e;
	if (!strcmp(name, "base_dimension") || !strcmp(name, "base_dimension_i")) {
		if (!fe->base_dim)
			return NULL;
	}
	struct fparam *p = bzalloc(sizeof(*p)); /* leaked on purpose: test tooling */
	p->e = fe;
	snprintf(p->name, sizeof p->name, "%s", name);
	return (gs_eparam_t *)p;
}
gs_technique_t *gs_effect_get_technique(const gs_effect_t *e, const char *name)
{
	struct ftech *t = bzalloc(sizeof(*t));
	t->e = (struct feffect *)e;
	snprintf(t->name, sizeof t->name, "%s", name);
	return (gs_technique_t *)t;
}
#define PN(p) ((p) ? ((struct fparam *)(p))->name : "null")
void gs_effect_set_texture_srgb(gs_eparam_t *p, gs_texture_t *t)
{
	ev("effect_set_texture_srgb %s.%s = %s", p ? EN(((struct fparam *)p)->e) : "null", PN(p), TN(t));
}
void gs_effect_set_texture(gs_eparam_t *p, gs_texture_t *t)
{
	ev("effect_set_texture %s.%s = %s", p ? EN(((struct fparam *)p)->e) : "null", PN(p), TN(t));
}
void gs_effect_set_vec2(gs_eparam_t *p, const struct vec2 *v)
{
	ev("effect_set_vec2 %s = %g,%g", PN(p), v->x, v->y);
}
void gs_effect_set_vec4(gs_eparam_t *p, const struct vec4 *v)
{
	ev("effect_set_vec4 %s = %g,%g,%g,%g", PN(p), v->x, v->y, v->z, v->w);
}
void gs_effect_set_float(gs_eparam_t *p, float v) { ev("effect_set_float %s = %.9g", PN(p), (double)v); }
bool gs_effect_loop(gs_effect_t *e, const char *tech)
{
	struct feffect *fe = (struct feffect *)e;
	if (fe->loop == 0) {
		fe->loop = 1;
		ev("effect_loop %s %s begin", fe->name, tech);
		return true;
	}
	fe->loop = 0;
	return false;
}
void gs_draw_sprite(gs_texture_t *t, uint32_t flip, uint32_t w, uint32_t h)
{
	ev("gs_draw_sprite %s flip=%u %ux%u", TN(t), flip, w, h);
}
void gs_draw(enum gs_draw_mode m, uint32_t start, uint32_t n)
{
	ev("gs_draw mode=%d start=%u n=%u", (int)m, start, n);
}
size_t gs_technique_begin(gs_technique_t *t)
{
	struct ftech *ft = (struct ftech *)t;
	ev("technique_begin %s.%s passes=%d", ft->e->name, ft->name, ft->e->passes);
	return (size_t)ft->e->passes;
}
bool gs_technique_begin_pass(gs_technique_t *t, size_t i)
{
	(void)t;
	ev("technique_pass %zu", i);
	return true;
}
void gs_technique_end_pass(gs_technique_t *t) { (void)t; }
void gs_technique_end(gs_technique_t *t) { (void)t; ev("technique_end"); }

/* ---- fake profiler / source profiler ---- */
void profile_register_root(const char *n, uint64_t t)
{
	ev("profile_register_root %s %" PRIu64, n, t);
}
void profile_start(const char *n) { (void)n; }
void profile_end(const char *n) { (void)n; }
void profile_reenable_thread(void) {}
const char *profile_store_name(profiler_name_store_t *s, const char *fmt, ...)
{
	static char buf[128];
	va_list ap;
	(void)s;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof buf, fmt, ap);
	va_end(ap);
	return buf;
}
profiler_name_store_t *obs_get_profiler_name_store(void) { return NULL; }
void source_profiler_frame_begin(void) { ev("sp_frame_begin"); }
void source_profiler_frame_collect(void) { ev("sp_frame_collect"); }
void source_profiler_render_begin(void) { ev("sp_render_begin"); }
void source_profiler_render_end(void) { ev("sp_render_end"); }
uint64_t source_profiler_source_tick_start(void) { return 0; }
void source_profiler_source_tick_end(obs_source_t *s, uint64_t st)
{
	(void)s;
	(void)st;
}
void os_set_thread_name_unused(void);

/* ---- fake core objects reached by obs-video.c ---- */
struct fvideo {
	struct video_output_info info;
	char name[24];
	int stop_after;   /* video_output_stopped returns true once called more than this many times */
	int stop_calls;
	bool lock_ok;
	uint32_t lock_ls[MAX_AV_PLANES];
	uint8_t *out[MAX_AV_PLANES];
	size_t out_size;
	int texture_frames, texture_skipped;
	uint64_t frame_time;
};
const struct video_output_info *video_output_get_info(const video_t *v)
{
	return &((const struct fvideo *)v)->info;
}
bool video_output_lock_frame(video_t *v, struct video_frame *frame, int count, uint64_t ts)
{
	struct fvideo *f = (struct fvideo *)v;
	ev("video_output_lock_frame %s count=%d ts=%" PRIu64 " -> %d", f->name, count, ts, (int)f->lock_ok);
	if (!f->lock_ok)
		return false;
	for (int i = 0; i < MAX_AV_PLANES; i++) {
		frame->data[i] = f->out[i];
		frame->linesize[i] = f->lock_ls[i];
	}
	return true;
}
void video_output_unlock_frame(video_t *v) { ev("video_output_unlock_frame %s", ((struct fvideo *)v)->name); }
bool video_output_stopped(video_t *v)
{
	struct fvideo *f = (struct fvideo *)v;
	return ++f->stop_calls > f->stop_after;
}
uint64_t video_output_get_frame_time(const video_t *v) { return ((const struct fvideo *)v)->frame_time; }
void video_output_inc_texture_frames(video_t *v)
{
	((struct fvideo *)v)->texture_frames++;
	ev("video_output_inc_texture_frames");
}
void video_output_inc_texture_skipped_frames(video_t *v)
{
	((struct fvideo *)v)->texture_skipped++;
	ev("video_output_inc_texture_skipped_frames");
}

static struct feffect *g_fx[12];
gs_effect_t *obs_get_base_effect(enum obs_base_effect e)
{
	(void)e;
	return (gs_effect_t *)obs->video.default_effect;
}
float obs_get_video_sdr_white_level(void) { return obs->video.sdr_white_level; }

struct view_name {
	struct obs_view *v;
	char name[24];
};
static struct view_name g_views[16];
static int g_nviews;
void obs_view_render(obs_view_t *v)
{
	const char *n = "?";
	for (int i = 0; i < g_nviews; i++)
		if (g_views[i].v == v)
			n = g_views[i].name;
	ev("obs_view_render %s", n);
}

static int g_mixes_freed;
void obs_free_video_mix(struct obs_core_video_mix *m)
{
	(void)m;
	g_mixes_freed++;
	ev("obs_free_video_mix");
}

/* sources */
struct fsrc_name {
	struct obs_source *s;
	char name[24];
	bool removed;
	int refs;
};
static struct fsrc_name g_srcs[16];
static int g_nsrcs;
static struct fsrc_name *src_find(const obs_source_t *s)
{
	for (int i = 0; i < g_nsrcs; i++)
		if (g_srcs[i].s == s)
			return &g_srcs[i];
	return NULL;
}
bool obs_source_removed(const obs_source_t *s) { return src_find(s)->removed; }
obs_source_t *obs_source_get_ref(obs_source_t *s)
{
	struct fsrc_name *f = src_find(s);
	f->refs++;
	return s;
}
void obs_source_release(obs_source_t *s)
{
	struct fsrc_name *f = src_find(s);
	f->refs--;
}
static struct fsrc_name *g_tick_src_hook;
void obs_source_video_tick(obs_source_t *s, float seconds)
{
	struct fsrc_name *f = src_find(s);
	ev("source_tick %s seconds=%.9g%s", f->name, (double)seconds, f->removed ? " (removed)" : "");
	if (f->name[0] == 'R') { /* a source named R* removes the source named in g_tick_src_hook */
		if (g_tick_src_hook)
			g_tick_src_hook->removed = true;
	}
}

/* displays */
struct fdisp_name {
	struct obs_display *d;
	char name[24];
};
static struct fdisp_name g_disps[8];
static int g_ndisps;
void render_display(struct obs_display *d)
{
	for (int i = 0; i < g_ndisps; i++)
		if (g_disps[i].d == d)
			ev("render_display %s ctx_depth=%d", g_disps[i].name, g_ctx_depth);
}

/* encoders */
struct fenc {
	struct obs_encoder e;
	char name[24];
	bool dead;      /* weak ref fails to promote */
	bool pause;
	bool success, received;
	int encodes;
	int refs;
	bool is_tex2;
};
static struct fenc *g_encs[16];
static int g_nencs;
static struct fenc *enc_of(const void *e) { return (struct fenc *)e; }
obs_weak_encoder_t *obs_encoder_get_weak_encoder(obs_encoder_t *e) { return (obs_weak_encoder_t *)e; }
obs_encoder_t *obs_weak_encoder_get_encoder(obs_weak_encoder_t *w)
{
	struct fenc *f = enc_of(w);
	if (f->dead)
		return NULL;
	f->refs++;
	return (obs_encoder_t *)w;
}
void obs_weak_encoder_release(obs_weak_encoder_t *w) { (void)w; }
obs_encoder_t *obs_encoder_get_ref(obs_encoder_t *e)
{
	enc_of(e)->refs++;
	return e;
}
void obs_encoder_release(obs_encoder_t *e) { enc_of(e)->refs--; }
bool video_pause_check(struct pause_data *p, uint64_t ts)
{
	struct fenc *f = (struct fenc *)((char *)p - offsetof(struct obs_encoder, pause));
	ev("video_pause_check %s ts=%" PRIu64 " -> %d", f->name, ts, (int)f->pause);
	return f->pause;
}
void handle_encoder_group_reconfigure_request(obs_encoder_t *e)
{
	ev("group_reconfigure_request %s", enc_of(e)->name);
}
void send_off_encoder_packet(obs_encoder_t *e, bool ok, bool rec, struct encoder_packet *pkt)
{
	ev("send_off_packet %s success=%d received=%d timebase=%d/%d", enc_of(e)->name, (int)ok, (int)rec,
	   (int)pkt->timebase_num, (int)pkt->timebase_den);
}
void full_stop(struct obs_encoder *e) { (void)e; }

THREAD_LOCAL bool is_graphics_thread;

struct obs_core *obs;

#include "obs-video.c"
#include "obs-video-gpu-encode.c"

#undef os_gettime_ns
#undef os_sleepto_ns
#undef os_sem_wait
#undef os_sem_post
#undef pthread_create
#undef pthread_join

/* ---- encoder callbacks ---- */
static bool enc_encode_texture(void *data, uint32_t handle, int64_t pts, uint64_t lock_key, uint64_t *next_key,
			       struct encoder_packet *pkt, bool *received)
{
	struct fenc *f = data;
	(void)pkt;
	f->encodes++;
	ev("encode_texture %s handle=%u pts=%" PRId64 " lock_key=%" PRIu64 " next_key_in=%" PRIu64 " -> %d,%d",
	   f->name, handle, pts, lock_key, *next_key, (int)f->success, (int)f->received);
	*received = f->received;
	return f->success;
}
static bool enc_encode_texture2(void *data, struct encoder_texture *tex, int64_t pts, uint64_t lock_key,
				uint64_t *next_key, struct encoder_packet *pkt, bool *received)
{
	struct fenc *f = data;
	(void)pkt;
	f->encodes++;
	ev("encode_texture2 %s tex0=%s tex1=%s tex2=%s handle=%u pts=%" PRId64 " lock_key=%" PRIu64
	   " next_key_in=%" PRIu64 " -> %d,%d",
	   f->name, TN(tex->tex[0]), TN(tex->tex[1]), TN(tex->tex[2]), tex->handle, pts, lock_key, *next_key,
	   (int)f->success, (int)f->received);
	*received = f->received;
	return f->success;
}
static bool enc_update(void *data, obs_data_t *settings)
{
	(void)settings;
	ev("encoder_update %s", ((struct fenc *)data)->name);
	return true;
}

/* ---- script state ---- */
struct mixent {
	char name[24];
	struct obs_core_video_mix *m;
	struct fvideo *v;
};
static struct mixent g_mix[8];
static int g_nmix;
static struct obs_graphics_context g_ctx;
static bool g_ctx_ready;

static void die(const char *m, const char *w)
{
	fprintf(stderr, "script error: %s %s\n", m, w ? w : "");
	exit(2);
}

static struct mixent *mix_find(const char *n)
{
	for (int i = 0; i < g_nmix; i++)
		if (!strcmp(g_mix[i].name, n))
			return &g_mix[i];
	die("unknown mix", n);
	return NULL;
}
static struct fsrc_name *srcn_find(const char *n)
{
	for (int i = 0; i < g_nsrcs; i++)
		if (!strcmp(g_srcs[i].name, n))
			return &g_srcs[i];
	die("unknown source", n);
	return NULL;
}
static struct fenc *enc_find(const char *n)
{
	for (int i = 0; i < g_nencs; i++)
		if (!strcmp(g_encs[i]->name, n))
			return g_encs[i];
	die("unknown encoder", n);
	return NULL;
}

/* callbacks recorded as events */
struct cbname {
	char name[16];
};
static struct cbname g_cbn[24];
static int g_ncbn;
static void cb_tick(void *p, float s) { ev("tick_callback %s seconds=%.9g", (char *)p, (double)s); }
static void cb_draw(void *p, uint32_t cx, uint32_t cy) { ev("draw_callback %s %ux%u", (char *)p, cx, cy); }
static void cb_rendered(void *p) { ev("rendered_callback %s", (char *)p); }
static char *cbname_new(const char *n)
{
	snprintf(g_cbn[g_ncbn].name, sizeof g_cbn[0].name, "%s", n);
	return g_cbn[g_ncbn++].name;
}
static int g_task_runs;
static void task_fn(void *p)
{
	ev("graphics_task %s", (char *)p);
	g_task_runs++;
}

static void core_reset(uint64_t interval)
{
	obs = bzalloc(sizeof(*obs));
	pthread_mutex_init(&obs->data.draw_callbacks_mutex, NULL);
	pthread_mutex_init(&obs->data.sources_mutex, NULL);
	pthread_mutex_init(&obs->data.displays_mutex, NULL);
	pthread_mutex_init(&obs->video.mixes_mutex, NULL);
	pthread_mutex_init(&obs->video.encoder_group_mutex, NULL);
	pthread_mutex_init(&obs->video.task_mutex, NULL);
	obs->data.valid = true;
	obs->video.graphics = (graphics_t *)obs; /* any non-NULL value */
	obs->video.video_frame_interval_ns = interval;
	obs->video.sdr_white_level = 300.0f;
	obs->video.hdr_nominal_peak_level = 1000.0f;
	g_fx[0] = effect_new("default", false);
	g_fx[1] = effect_new("bicubic", true);
	g_fx[2] = effect_new("lanczos", true);
	g_fx[3] = effect_new("area", true);
	g_fx[4] = effect_new("bilinear_lowres", false);
	g_fx[5] = effect_new("conversion", false);
	obs->video.default_effect = (gs_effect_t *)g_fx[0];
	obs->video.bicubic_effect = (gs_effect_t *)g_fx[1];
	obs->video.lanczos_effect = (gs_effect_t *)g_fx[2];
	obs->video.area_effect = (gs_effect_t *)g_fx[3];
	obs->video.bilinear_lowres_effect = (gs_effect_t *)g_fx[4];
	obs->video.conversion_effect = (gs_effect_t *)g_fx[5];
	g_nmix = g_nsrcs = g_ndisps = g_nencs = g_nviews = g_ncbn = 0;
	g_ctx_ready = false;
}

static struct fsurf *surf_new(const char *name, int ch, uint32_t ls, uint32_t rows)
{
	struct fsurf *s = bzalloc(sizeof(*s));
	snprintf(s->name, sizeof s->name, "%s", name);
	s->ch = ch;
	s->linesize = ls;
	s->rows = rows;
	s->buf = bzalloc((size_t)ls * rows + 8);
	for (size_t k = 0; k < (size_t)ls * rows; k++)
		s->buf[k] = (uint8_t)((k * 7 + (size_t)ch * 13 + 1) & 0xff);
	return s;
}

static enum video_format parse_format(const char *s)
{
	static const struct {
		const char *n;
		enum video_format f;
	} t[] = {{"I420", VIDEO_FORMAT_I420}, {"NV12", VIDEO_FORMAT_NV12}, {"I444", VIDEO_FORMAT_I444},
		 {"I010", VIDEO_FORMAT_I010}, {"P010", VIDEO_FORMAT_P010}, {"P216", VIDEO_FORMAT_P216},
		 {"P416", VIDEO_FORMAT_P416}, {"BGRA", VIDEO_FORMAT_BGRA}, {"RGBA", VIDEO_FORMAT_RGBA},
		 {"BGRX", VIDEO_FORMAT_BGRX}, {"YUY2", VIDEO_FORMAT_YUY2}, {"NONE", VIDEO_FORMAT_NONE}};
	for (size_t i = 0; i < sizeof t / sizeof t[0]; i++)
		if (!strcmp(s, t[i].n))
			return t[i].f;
	die("bad format", s);
	return VIDEO_FORMAT_NONE;
}

/* mix_add NAME has_view BASE_W BASE_H OUT_W OUT_H FORMAT GPU_CONV SCALE SPACE
 * Creates a mix whose planes follow FORMAT with tight line sizes (+ pad for the first plane when
 * script later calls surf_ls). */
static void cmd_mix_add(struct script_line *sl)
{
	script_need(sl, 11);
	struct mixent *e = &g_mix[g_nmix++];
	snprintf(e->name, sizeof e->name, "%s", sl->argv[1]);
	struct obs_core_video_mix *m = bzalloc(sizeof(*m));
	struct fvideo *v = bzalloc(sizeof(*v));
	snprintf(v->name, sizeof v->name, "%s", sl->argv[1]);
	uint32_t bw = (uint32_t)atoi(sl->argv[3]), bh = (uint32_t)atoi(sl->argv[4]);
	uint32_t ow = (uint32_t)atoi(sl->argv[5]), oh = (uint32_t)atoi(sl->argv[6]);
	v->info.width = ow;
	v->info.height = oh;
	v->info.format = parse_format(sl->argv[7]);
	v->lock_ok = true;
	v->frame_time = obs->video.video_frame_interval_ns;
	e->v = v;
	e->m = m;
	m->video = (video_t *)v;
	m->ovi.base_width = bw;
	m->ovi.base_height = bh;
	m->ovi.scale_type = (enum obs_scale_type)atoi(sl->argv[9]);
	m->render_space = (enum gs_color_space)atoi(sl->argv[10]);
	m->gpu_conversion = atoi(sl->argv[8]) != 0;
	pthread_mutex_init(&m->gpu_encoder_mutex, NULL);
	char n[48];
	snprintf(n, sizeof n, "%s.render", e->name);
	m->render_texture = tex_new(n, bw, bh);
	snprintf(n, sizeof n, "%s.output", e->name);
	m->output_texture = tex_new(n, ow, oh);
	/* view */
	if (atoi(sl->argv[2])) {
		struct obs_view *view = bzalloc(sizeof(*view));
		m->view = view;
		snprintf(g_views[g_nviews].name, sizeof g_views[0].name, "%s.view", e->name);
		g_views[g_nviews++].v = view;
	}
	/* planes: convert textures and copy surfaces by format */
	int planes = 1;
	uint32_t w[3] = {ow, ow, ow}, h[3] = {oh, oh, oh}; /* plane width in BYTES and rows */
	switch (v->info.format) {
	case VIDEO_FORMAT_I420:
		planes = 3;
		w[1] = w[2] = ow / 2;
		h[1] = h[2] = oh / 2;
		break;
	case VIDEO_FORMAT_NV12:
		planes = 2;
		h[1] = oh / 2;
		break;
	case VIDEO_FORMAT_I444:
		planes = 3;
		break;
	case VIDEO_FORMAT_I010:
		planes = 3;
		w[0] = ow * 2;
		w[1] = w[2] = ow;
		h[1] = h[2] = oh / 2;
		break;
	case VIDEO_FORMAT_P010:
		planes = 2;
		w[0] = w[1] = ow * 2;
		h[1] = oh / 2;
		break;
	case VIDEO_FORMAT_P216:
		planes = 2;
		w[0] = w[1] = ow * 2;
		break;
	case VIDEO_FORMAT_P416:
		planes = 2;
		w[0] = ow * 2;
		w[1] = ow * 4;
		break;
	default:
		break;
	}
	for (int c = 0; c < planes && m->gpu_conversion; c++) {
		snprintf(n, sizeof n, "%s.conv%d", e->name, c);
		m->convert_textures[c] = tex_new(n, w[c], h[c]);
		m->conversion_techs[c] = c == 0 ? "Tech0" : (c == 1 ? "Tech1" : "Tech2");
	}
	for (int t = 0; t < NUM_TEXTURES; t++) {
		int cn = m->gpu_conversion ? planes : 1;
		for (int c = 0; c < cn; c++) {
			snprintf(n, sizeof n, "%s.surf%d_%d", e->name, t, c);
			uint32_t ls = m->gpu_conversion ? w[c] : ow * 4;
			m->copy_surfaces[t][c] = (gs_stagesurf_t *)surf_new(n, c, ls, m->gpu_conversion ? h[c] : oh);
		}
	}
	snprintf(n, sizeof n, "%s.encY", e->name);
	m->convert_textures_encode[0] = tex_new(n, ow, oh);
	snprintf(n, sizeof n, "%s.encUV", e->name);
	m->convert_textures_encode[1] = tex_new(n, ow / 2, oh / 2);
	for (int i = 0; i < 16; i++)
		m->color_matrix[i] = (float)i + 0.5f;
	m->conversion_width_i = 1.0f / (float)ow;
	m->conversion_height_i = 1.0f / (float)oh;
	/* output frame buffers (what video_output_lock_frame hands out) */
	v->out_size = (size_t)ow * oh * 8 + 64;
	for (int i = 0; i < MAX_AV_PLANES; i++)
		v->out[i] = bzalloc(v->out_size);
	v->lock_ls[0] = (v->info.format == VIDEO_FORMAT_BGRA || v->info.format == VIDEO_FORMAT_RGBA ||
			 v->info.format == VIDEO_FORMAT_BGRX)
				? ow * 4
				: ow;
	v->lock_ls[1] = v->lock_ls[2] = ow;
	pthread_mutex_lock(&obs->video.mixes_mutex);
	da_push_back(obs->video.mixes, &m);
	pthread_mutex_unlock(&obs->video.mixes_mutex);
}

static void dump_mix(struct mixent *e)
{
	struct obs_core_video_mix *m = e->m;
	printf("mix %s raw_active=%ld gpu_active=%ld raw_was=%d gpu_was=%d was=%d cur_texture=%d rendered=%d "
	       "converted=%d copied=%d,%d vframes=%zu vframes_gpu=%zu gpu_q=%zu gpu_avail=%zu\n",
	       e->name, m->raw_active, m->gpu_encoder_active, (int)m->raw_was_active, (int)m->gpu_was_active,
	       (int)m->was_active, m->cur_texture, (int)m->texture_rendered, (int)m->texture_converted,
	       (int)m->textures_copied[0], (int)m->textures_copied[1], m->vframe_info_buffer.size / sizeof(struct obs_vframe_info),
	       m->vframe_info_buffer_gpu.size / sizeof(struct obs_vframe_info),
	       m->gpu_encoder_queue.size / sizeof(struct obs_tex_frame),
	       m->gpu_encoder_avail_queue.size / sizeof(struct obs_tex_frame));
	printf("mix %s active_surfaces:", e->name);
	for (int t = 0; t < NUM_TEXTURES; t++)
		for (int c = 0; c < NUM_CHANNELS; c++)
			printf(" [%d][%d]=%s", t, c,
			       m->active_copy_surfaces[t][c] ? ((struct fsurf *)m->active_copy_surfaces[t][c])->name : "null");
	printf("\nmix %s mapped:", e->name);
	for (int c = 0; c < NUM_CHANNELS; c++)
		printf(" %d", m->mapped_surfaces[c] ? 1 : 0);
	printf("\n");
}

static void dump_counters(void)
{
	printf("counters total_frames=%u lagged_frames=%u video_time=%" PRIu64 " fps=%.9g avg_frame_time_ns=%" PRIu64
	       " mixes=%zu ctx_depth=%d underflow=%d\n",
	       obs->video.total_frames, obs->video.lagged_frames, obs->video.video_time, obs->video.video_fps,
	       obs->video.video_avg_frame_time_ns, obs->video.mixes.num, g_ctx_depth, g_ctx_underflow);
}

int main(int argc, char **argv)
{
	nix_reexec(argv);
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno;
	core_reset(16666667);
	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		if (nix_command(&sl))
			continue;
		if (!strcmp(c, "trace")) {
			script_need(&sl, 2);
			g_trace = atoi(sl.argv[1]) != 0;
		} else if (!strcmp(c, "echo")) {
			for (int i = 1; i < sl.argc; i++)
				printf("%s%s", i > 1 ? " " : "", sl.argv[i]);
			printf("\n");
		} else if (!strcmp(c, "core")) { /* core INTERVAL_NS */
			script_need(&sl, 2);
			core_reset((uint64_t)strtoull(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "clock")) {
			script_need(&sl, 2);
			g_now = (uint64_t)strtoull(sl.argv[1], NULL, 10);
		} else if (!strcmp(c, "clock_step")) {
			script_need(&sl, 2);
			g_step = (uint64_t)strtoull(sl.argv[1], NULL, 10);
		} else if (!strcmp(c, "video_time")) {
			script_need(&sl, 2);
			obs->video.video_time = (uint64_t)strtoull(sl.argv[1], NULL, 10);
		} else if (!strcmp(c, "sleep_result")) { /* default result, or a sequence for the next calls */
			script_need(&sl, 2);
			g_sleep_result = atoi(sl.argv[1]);
			g_sleep_seq_n = g_sleep_seq_i = 0;
			for (int i = 2; i < sl.argc && g_sleep_seq_n < 64; i++)
				g_sleep_seq[g_sleep_seq_n++] = atoi(sl.argv[i]);
		} else if (!strcmp(c, "mix_add")) {
			cmd_mix_add(&sl);
		} else if (!strcmp(c, "mix_active")) { /* mix_active NAME RAW GPU */
			script_need(&sl, 4);
			struct mixent *e = mix_find(sl.argv[1]);
			e->m->raw_active = atol(sl.argv[2]);
			e->m->gpu_encoder_active = atol(sl.argv[3]);
		} else if (!strcmp(c, "mix_view_none")) {
			script_need(&sl, 2);
			mix_find(sl.argv[1])->m->view = NULL;
		} else if (!strcmp(c, "mix_view_copy")) { /* mix_view_copy DST SRC: share the view of SRC */
			script_need(&sl, 3);
			mix_find(sl.argv[1])->m->view = mix_find(sl.argv[2])->m->view;
		} else if (!strcmp(c, "mix_set")) { /* mix_set NAME FIELD VALUE */
			script_need(&sl, 4);
			struct obs_core_video_mix *m = mix_find(sl.argv[1])->m;
			const char *k = sl.argv[2];
			long v = atol(sl.argv[3]);
			if (!strcmp(k, "texture_rendered"))
				m->texture_rendered = v;
			else if (!strcmp(k, "render_space"))
				m->render_space = (enum gs_color_space)v;
			else if (!strcmp(k, "base_w"))
				m->ovi.base_width = (uint32_t)v;
			else if (!strcmp(k, "base_h"))
				m->ovi.base_height = (uint32_t)v;
			else if (!strcmp(k, "scale"))
				m->ovi.scale_type = (enum obs_scale_type)v;
			else if (!strcmp(k, "cur_texture"))
				m->cur_texture = (int)v;
			else if (!strcmp(k, "gpu_conversion"))
				m->gpu_conversion = v;
			else if (!strcmp(k, "convert_null")) /* convert_textures[v] = NULL */
				m->convert_textures[v] = NULL;
			else if (!strcmp(k, "surf_null")) /* copy_surfaces[both][v] = NULL */
				m->copy_surfaces[0][v] = m->copy_surfaces[1][v] = NULL;
			else
				die("mix_set field", k);
		} else if (!strcmp(c, "render_tex_size")) { /* render_tex_size NAME W H (output texture size) */
			script_need(&sl, 4);
			struct ftex *t = (struct ftex *)mix_find(sl.argv[1])->m->output_texture;
			t->w = (uint32_t)atoi(sl.argv[2]);
			t->h = (uint32_t)atoi(sl.argv[3]);
			mix_find(sl.argv[1])->v->info.width = t->w; /* the video output reports the same size */
			mix_find(sl.argv[1])->v->info.height = t->h;
		} else if (!strcmp(c, "effect_passes")) { /* effect_passes NAME N */
			script_need(&sl, 3);
			for (int i = 0; i < 6; i++)
				if (!strcmp(g_fx[i]->name, sl.argv[1]))
					g_fx[i]->passes = atoi(sl.argv[2]);
		} else if (!strcmp(c, "effect_missing")) { /* effect_missing NAME: pointer becomes NULL */
			script_need(&sl, 2);
			if (!strcmp(sl.argv[1], "bicubic")) obs->video.bicubic_effect = NULL;
			else if (!strcmp(sl.argv[1], "lanczos")) obs->video.lanczos_effect = NULL;
			else if (!strcmp(sl.argv[1], "area")) obs->video.area_effect = NULL;
			else if (!strcmp(sl.argv[1], "bilinear_lowres")) obs->video.bilinear_lowres_effect = NULL;
			else die("effect_missing", sl.argv[1]);
		} else if (!strcmp(c, "surf_cfg")) { /* surf_cfg MIX T C LINESIZE [fail] */
			script_need(&sl, 5);
			struct obs_core_video_mix *m = mix_find(sl.argv[1])->m;
			struct fsurf *s = (struct fsurf *)m->copy_surfaces[atoi(sl.argv[2])][atoi(sl.argv[3])];
			if (!s)
				die("surf_cfg: no surface", sl.argv[1]);
			uint32_t ls = (uint32_t)atoi(sl.argv[4]);
			uint32_t rows = s->rows;
			bfree(s->buf);
			s->linesize = ls;
			s->buf = bzalloc((size_t)ls * rows * 2 + 8);
			for (size_t k = 0; k < (size_t)ls * rows * 2; k++)
				s->buf[k] = (uint8_t)((k * 7 + (size_t)s->ch * 13 + 1) & 0xff);
			s->fail_map = sl.argc > 5 && !strcmp(sl.argv[5], "fail");
		} else if (!strcmp(c, "surf_rows")) { /* surf_rows MIX T C ROWS: surface holds ROWS rows (single plane NV12 case) */
			script_need(&sl, 5);
			struct obs_core_video_mix *m = mix_find(sl.argv[1])->m;
			struct fsurf *s = (struct fsurf *)m->copy_surfaces[atoi(sl.argv[2])][atoi(sl.argv[3])];
			bfree(s->buf);
			s->rows = (uint32_t)atoi(sl.argv[4]);
			s->buf = bzalloc((size_t)s->linesize * s->rows + 8);
			for (size_t k = 0; k < (size_t)s->linesize * s->rows; k++)
				s->buf[k] = (uint8_t)((k * 7 + (size_t)s->ch * 13 + 1) & 0xff);
		} else if (!strcmp(c, "lock_cfg")) { /* lock_cfg MIX OK LS0 LS1 LS2 */
			script_need(&sl, 6);
			struct fvideo *v = mix_find(sl.argv[1])->v;
			v->lock_ok = atoi(sl.argv[2]) != 0;
			for (int i = 0; i < 3; i++)
				v->lock_ls[i] = (uint32_t)atoi(sl.argv[3 + i]);
		} else if (!strcmp(c, "stop_after")) { /* stop_after MIX N */
			script_need(&sl, 3);
			struct fvideo *v = mix_find(sl.argv[1])->v;
			v->stop_after = atoi(sl.argv[2]);
			v->stop_calls = 0;
		} else if (!strcmp(c, "out_dump")) { /* out_dump MIX PLANE LEN: first LEN bytes of the frame buffer */
			script_need(&sl, 4);
			struct fvideo *v = mix_find(sl.argv[1])->v;
			printf("out %s plane%s ", sl.argv[1], sl.argv[2]);
			script_print_hex(v->out[atoi(sl.argv[2])], (size_t)atoi(sl.argv[3]));
			printf("\n");
		} else if (!strcmp(c, "out_clear")) {
			script_need(&sl, 2);
			struct fvideo *v = mix_find(sl.argv[1])->v;
			for (int i = 0; i < MAX_AV_PLANES; i++)
				memset(v->out[i], 0, v->out_size);
		} else if (!strcmp(c, "source_add")) { /* source_add NAME [removed] */
			script_need(&sl, 2);
			struct obs_source *s = bzalloc(sizeof(*s));
			struct fsrc_name *e = &g_srcs[g_nsrcs++];
			e->s = s;
			snprintf(e->name, sizeof e->name, "%s", sl.argv[1]);
			e->removed = sl.argc > 2 && !strcmp(sl.argv[2], "removed");
			/* singly linked through hh_uuid.next, newest first (like the real table iteration order is
			 * unspecified; the runner fixes it to "most recently added first") */
			s->context.hh_uuid.next = obs->data.sources;
			obs->data.sources = s;
		} else if (!strcmp(c, "source_removes")) { /* source_removes R TARGET */
			script_need(&sl, 3);
			g_tick_src_hook = srcn_find(sl.argv[2]);
		} else if (!strcmp(c, "source_removed")) {
			script_need(&sl, 3);
			srcn_find(sl.argv[1])->removed = atoi(sl.argv[2]) != 0;
		} else if (!strcmp(c, "source_refs")) {
			script_need(&sl, 2);
			printf("source %s refs=%d\n", sl.argv[1], srcn_find(sl.argv[1])->refs);
		} else if (!strcmp(c, "cb_add")) { /* cb_add tick|draw|rendered NAME */
			script_need(&sl, 3);
			char *nm = cbname_new(sl.argv[2]);
			if (!strcmp(sl.argv[1], "tick")) {
				struct tick_callback cb = {cb_tick, nm};
				da_push_back(obs->data.tick_callbacks, &cb);
			} else if (!strcmp(sl.argv[1], "draw")) {
				struct draw_callback cb = {cb_draw, nm};
				da_push_back(obs->data.draw_callbacks, &cb);
			} else {
				struct rendered_callback cb = {cb_rendered, nm};
				da_push_back(obs->data.rendered_callbacks, &cb);
			}
		} else if (!strcmp(c, "display_add")) {
			script_need(&sl, 2);
			struct obs_display *d = bzalloc(sizeof(*d));
			g_disps[g_ndisps].d = d;
			snprintf(g_disps[g_ndisps++].name, sizeof g_disps[0].name, "%s", sl.argv[1]);
			if (!obs->data.first_display)
				obs->data.first_display = d;
			else {
				struct obs_display *p = obs->data.first_display;
				while (p->next)
					p = p->next;
				p->next = d;
			}
		} else if (!strcmp(c, "data_valid")) {
			script_need(&sl, 2);
			obs->data.valid = atoi(sl.argv[1]) != 0;
		} else if (!strcmp(c, "task_add")) {
			script_need(&sl, 2);
			struct obs_task_info ti = {task_fn, cbname_new(sl.argv[1])};
			deque_push_back(&obs->video.tasks, &ti, sizeof(ti));
		} else if (!strcmp(c, "ctx_new")) { /* ctx_new INTERVAL [LAST_TIME] */
			script_need(&sl, 2);
			memset(&g_ctx, 0, sizeof g_ctx);
			g_ctx.interval = (uint64_t)strtoull(sl.argv[1], NULL, 10);
			g_ctx.last_time = sl.argc > 2 ? (uint64_t)strtoull(sl.argv[2], NULL, 10) : 0;
			g_ctx.video_thread_name = "test";
			g_ctx_ready = true;
		} else if (!strcmp(c, "loop")) { /* loop N [ADVANCE_NS_BEFORE_EACH] */
			script_need(&sl, 2);
			if (!g_ctx_ready)
				die("loop before ctx_new", NULL);
			int n = atoi(sl.argv[1]);
			for (int i = 0; i < n; i++) {
				bool r = obs_graphics_thread_loop(&g_ctx);
				printf("loop -> %d last_time=%" PRIu64 " frame_time_total=%" PRIu64 " fps_total_ns=%" PRIu64
				       " fps_frames=%u\n",
				       (int)r, g_ctx.last_time, g_ctx.frame_time_total_ns, g_ctx.fps_total_ns,
				       g_ctx.fps_total_frames);
			}
		} else if (!strcmp(c, "thread")) { /* run obs_graphics_thread on this thread until stopped */
			obs_graphics_thread(NULL);
			printf("thread returned\n");
		} else if (!strcmp(c, "counters")) {
			dump_counters();
		} else if (!strcmp(c, "mix_dump")) {
			script_need(&sl, 2);
			dump_mix(mix_find(sl.argv[1]));
		} else if (!strcmp(c, "vframe_push")) { /* vframe_push MIX raw|gpu TS COUNT */
			script_need(&sl, 5);
			struct obs_core_video_mix *m = mix_find(sl.argv[1])->m;
			struct obs_vframe_info vi = {(uint64_t)strtoull(sl.argv[3], NULL, 10), atoi(sl.argv[4])};
			deque_push_back(!strcmp(sl.argv[2], "raw") ? &m->vframe_info_buffer : &m->vframe_info_buffer_gpu, &vi,
					sizeof(vi));
		} else if (!strcmp(c, "vframe_clear")) { /* vframe_clear MIX raw|gpu */
			script_need(&sl, 3);
			struct obs_core_video_mix *m = mix_find(sl.argv[1])->m;
			struct deque *d = !strcmp(sl.argv[2], "raw") ? &m->vframe_info_buffer : &m->vframe_info_buffer_gpu;
			deque_free(d);
			deque_init(d);
		} else if (!strcmp(c, "ready_group")) { /* ready_group ENC */
			script_need(&sl, 2);
			add_ready_encoder_group((obs_encoder_t *)enc_find(sl.argv[1]));
		} else if (!strcmp(c, "enc_add")) {
			/* enc_add NAME MIX TB_NUM TB_DEN DIVISOR TEX2(0/1) SUCCESS RECEIVED [START_TS] */
			script_need(&sl, 9);
			struct fenc *e = bzalloc(sizeof(*e));
			snprintf(e->name, sizeof e->name, "%s", sl.argv[1]);
			e->e.timebase_num = (uint32_t)atoi(sl.argv[3]);
			e->e.timebase_den = (uint32_t)atoi(sl.argv[4]);
			e->e.frame_rate_divisor = (uint32_t)atoi(sl.argv[5]);
			e->is_tex2 = atoi(sl.argv[6]) != 0;
			e->success = atoi(sl.argv[7]) != 0;
			e->received = atoi(sl.argv[8]) != 0;
			e->e.start_ts = sl.argc > 9 ? (uint64_t)strtoull(sl.argv[9], NULL, 10) : 0;
			e->e.context.data = e;
			if (e->is_tex2)
				e->e.info.encode_texture2 = enc_encode_texture2;
			else
				e->e.info.encode_texture = enc_encode_texture;
			e->e.info.update = enc_update;
			g_encs[g_nencs++] = e;
			struct obs_core_video_mix *m = mix_find(sl.argv[2])->m;
			obs_encoder_t *ep = (obs_encoder_t *)e;
			da_push_back(m->gpu_encoders, &ep);
		} else if (!strcmp(c, "enc_set")) { /* enc_set NAME FIELD VALUE */
			script_need(&sl, 4);
			struct fenc *e = enc_find(sl.argv[1]);
			const char *k = sl.argv[2];
			long long v = strtoll(sl.argv[3], NULL, 10);
			if (!strcmp(k, "dead")) e->dead = v;
			else if (!strcmp(k, "first_received")) e->e.first_received = v;
			else if (!strcmp(k, "first_raw_ts")) e->e.first_raw_ts = (uint64_t)v;
			else if (!strcmp(k, "start_ts")) e->e.start_ts = (uint64_t)v;
			else if (!strcmp(k, "cur_pts")) e->e.cur_pts = v;
			else if (!strcmp(k, "reconfigure")) e->e.reconfigure_requested = v;
			else if (!strcmp(k, "divisor_counter")) e->e.frame_rate_divisor_counter = (uint32_t)v;
			else if (!strcmp(k, "pause")) e->pause = v;
			else if (!strcmp(k, "success")) e->success = v;
			else if (!strcmp(k, "received")) e->received = v;
			else die("enc_set field", k);
		} else if (!strcmp(c, "enc_pair")) { /* enc_pair ENC OTHER: OTHER is a paired (audio) encoder */
			script_need(&sl, 3);
			obs_weak_encoder_t *w = (obs_weak_encoder_t *)enc_find(sl.argv[2]);
			da_push_back(enc_find(sl.argv[1])->e.paired_encoders, &w);
		} else if (!strcmp(c, "enc_group")) { /* enc_group ENC GROUPNAME N_ENC STARTED START_TS */
			script_need(&sl, 6);
			static struct obs_encoder_group grp[4];
			static char gnames[4][16];
			int gi = 0;
			while (gi < 3 && gnames[gi][0] && strcmp(gnames[gi], sl.argv[2]))
				gi++;
			if (!gnames[gi][0]) {
				snprintf(gnames[gi], 16, "%s", sl.argv[2]);
				pthread_mutex_init(&grp[gi].mutex, NULL);
			}
			struct fenc *e = enc_find(sl.argv[1]);
			e->e.encoder_group = &grp[gi];
			grp[gi].encoders.num = (size_t)atoi(sl.argv[3]);
			grp[gi].num_encoders_started = (uint32_t)atoi(sl.argv[4]);
			grp[gi].start_timestamp = (uint64_t)strtoull(sl.argv[5], NULL, 10);
		} else if (!strcmp(c, "group_ts")) { /* group_ts ENC: print the group's start timestamp */
			script_need(&sl, 2);
			struct fenc *e = enc_find(sl.argv[1]);
			printf("group_ts %s %" PRIu64 "\n", e->name, e->e.encoder_group ? e->e.encoder_group->start_timestamp : 0);
		} else if (!strcmp(c, "enc_dump")) {
			script_need(&sl, 2);
			struct fenc *e = enc_find(sl.argv[1]);
			printf("encoder %s encodes=%d cur_pts=%" PRId64 " start_ts=%" PRIu64 " divisor_counter=%u reconfigure=%d "
			       "packet_times=%zu refs=%d\n",
			       e->name, e->encodes, e->e.cur_pts, e->e.start_ts, e->e.frame_rate_divisor_counter,
			       (int)e->e.reconfigure_requested, e->e.encoder_packet_times.num, e->refs);
			for (size_t i = 0; i < e->e.encoder_packet_times.num; i++) {
				struct encoder_packet_time *t = &e->e.encoder_packet_times.array[i];
				printf("  ept pts=%" PRId64 " cts=%" PRIu64 " fer_set=%d ferc_set=%d\n", t->pts, t->cts,
				       (int)(t->fer != 0), (int)(t->ferc != 0));
			}
		} else if (!strcmp(c, "gpu_init")) { /* gpu_init MIX [tex_fail] */
			script_need(&sl, 2);
			struct mixent *e = mix_find(sl.argv[1]);
			g_dest_tex = (sl.argc > 2 && !strcmp(sl.argv[2], "tex_fail")) ? -1 : 0;
			bool r = init_gpu_encoding(e->m);
			printf("init_gpu_encoding -> %d initialized=%d avail=%zu\n", (int)r, (int)e->m->gpu_encode_thread_initialized,
			       e->m->gpu_encoder_avail_queue.size / sizeof(struct obs_tex_frame));
		} else if (!strcmp(c, "pthread_create_result")) {
			script_need(&sl, 2);
			g_pthread_create_result = atoi(sl.argv[1]);
		} else if (!strcmp(c, "gpu_thread")) { /* gpu_thread MIX BUDGET: run the encode thread body with BUDGET semaphore tokens */
			script_need(&sl, 3);
			struct mixent *e = mix_find(sl.argv[1]);
			g_sem_budget = atoi(sl.argv[2]);
			gpu_encode_thread(e->m);
			printf("gpu_thread done budget_left=%d\n", g_sem_budget);
			g_sem_budget = 0;
		} else if (!strcmp(c, "gpu_stop")) {
			stop_gpu_encoding_thread(mix_find(sl.argv[1])->m);
			printf("gpu_stop stop_flag=%d joins=%d\n", (int)mix_find(sl.argv[1])->m->gpu_encode_stop, g_pthread_joins);
		} else if (!strcmp(c, "gpu_free")) {
			free_gpu_encoding(mix_find(sl.argv[1])->m);
		} else if (!strcmp(c, "gpu_queue_dump")) { /* print every tex_frame of the encode queues without consuming */
			script_need(&sl, 2);
			struct obs_core_video_mix *m = mix_find(sl.argv[1])->m;
			struct deque *qs[2] = {&m->gpu_encoder_queue, &m->gpu_encoder_avail_queue};
			const char *qn[2] = {"queue", "avail"};
			bool full = sl.argc > 2 && !strcmp(sl.argv[2], "full");
			printf("avail_count=%zu\n", qs[1]->size / sizeof(struct obs_tex_frame));
			for (int q = 0; q < (full ? 2 : 1); q++) {
				size_t n = qs[q]->size / sizeof(struct obs_tex_frame);
				for (size_t i = 0; i < n; i++) {
					struct obs_tex_frame tf;
					deque_pop_front(qs[q], &tf, sizeof tf); /* rotate: pop front, push back */
					deque_push_back(qs[q], &tf, sizeof tf);
					printf("%s[%zu] tex=%s uv=%s count=%d ts=%" PRIu64 " key=%" PRIu64 " released=%d\n", qn[q], i,
					       TN(tf.tex), TN(tf.tex_uv), tf.count, tf.timestamp, tf.lock_key, (int)tf.released);
				}
			}
			printf("texture_frames=%d skipped=%d\n", mix_find(sl.argv[1])->v->texture_frames,
			       mix_find(sl.argv[1])->v->texture_skipped);
		} else if (!strcmp(c, "gpu_queue_push")) { /* gpu_queue_push MIX TEX_NAME COUNT TS LOCKKEY RELEASED : push a frame into the encode queue */
			script_need(&sl, 7);
			struct obs_core_video_mix *m = mix_find(sl.argv[1])->m;
			struct obs_tex_frame tf = {0};
			char n[48];
			snprintf(n, sizeof n, "%s.y", sl.argv[2]);
			tf.tex = tex_new(n, 8, 8);
			snprintf(n, sizeof n, "%s.uv", sl.argv[2]);
			tf.tex_uv = tex_new(n, 4, 4);
			tf.handle = (uint32_t)-1;
			tf.count = atoi(sl.argv[3]);
			tf.timestamp = (uint64_t)strtoull(sl.argv[4], NULL, 10);
			tf.lock_key = (uint64_t)strtoull(sl.argv[5], NULL, 10);
			tf.released = atoi(sl.argv[6]) != 0;
			deque_push_back(&m->gpu_encoder_queue, &tf, sizeof tf);
		} else if (!strcmp(c, "gpu_avail_trim")) { /* gpu_avail_trim MIX N: keep only N avail frames */
			script_need(&sl, 3);
			struct obs_core_video_mix *m = mix_find(sl.argv[1])->m;
			size_t keep = (size_t)atoi(sl.argv[2]);
			while (m->gpu_encoder_avail_queue.size / sizeof(struct obs_tex_frame) > keep) {
				struct obs_tex_frame tf;
				deque_pop_back(&m->gpu_encoder_avail_queue, &tf, sizeof tf);
			}
		} else if (!strcmp(c, "gpu_queue_clear")) {
			script_need(&sl, 2);
			struct obs_core_video_mix *m = mix_find(sl.argv[1])->m;
			deque_free(&m->gpu_encoder_queue);
			deque_init(&m->gpu_encoder_queue);
		} else if (!strcmp(c, "sem")) {
			printf("sem_posts=%d\n", g_sem_posts);
		} else if (!strcmp(c, "freed")) {
			printf("mixes_freed=%d\n", g_mixes_freed);
		} else if (!strcmp(c, "tasks_run")) {
			printf("tasks_run=%d\n", g_task_runs);
		} else {
			die("unknown command", c);
		}
	}
	fclose(f);
	return 0;
}
