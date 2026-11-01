/*
 * Conformance harness for spec S-040 (source core: lifecycle, filters, async video, transitions).
 *
 * The real obs-source.c is #included (so its file-local helpers are reachable); obs-source-deinterlace.c and
 * obs-source-transition.c are linked as they are.  Everything those files reach that is not source logic is
 * replaced here by a fake world:
 *   - a fake `obs` core (data / video / audio parts, global signal handler, source type table),
 *   - a fake clock (os_gettime_ns) and fake video clock (obs->video.video_time),
 *   - a trivial context layer (names, public-source registry used by obs_get_source_by_name),
 *   - a deferred-destruction task queue that only runs when the script says "drain",
 *   - no-op graphics (every gs_* call succeeds and draws nothing),
 *   - scripted plugin sources ("kinds") whose callbacks print trace lines.
 * Test tooling, not part of libobs.
 */
#include "obs-source.c"

#include "obssource_api.h"

struct obs_core *obs;

/* ---------------------------------------------------------------- clocks */

static uint64_t os_clock_ns;
void os_set_clock(uint64_t ns)
{
	os_clock_ns = ns;
}
uint64_t os_gettime_ns(void)
{
	return os_clock_ns;
}

/* ---------------------------------------------------------------- fake audio output (sizes only) */

static struct audio_output_info fake_ai = {.samples_per_sec = 48000, .speakers = SPEAKERS_STEREO,
					   .format = AUDIO_FORMAT_FLOAT_PLANAR};
uint32_t audio_output_get_sample_rate(const audio_t *a)
{
	(void)a;
	return fake_ai.samples_per_sec;
}
size_t audio_output_get_channels(const audio_t *a)
{
	(void)a;
	return 2;
}
const struct audio_output_info *audio_output_get_info(const audio_t *a)
{
	(void)a;
	return &fake_ai;
}
size_t audio_output_get_planes(const audio_t *a)
{
	(void)a;
	return 2;
}
size_t audio_output_get_block_size(const audio_t *a)
{
	(void)a;
	return 4;
}
audio_resampler_t *audio_resampler_create(const struct resample_info *d, const struct resample_info *s)
{
	(void)d, (void)s;
	return NULL;
}
void audio_resampler_destroy(audio_resampler_t *r)
{
	(void)r;
}
bool audio_resampler_resample(audio_resampler_t *r, uint8_t *o[], uint32_t *of, uint64_t *to,
			      const uint8_t *const i[], uint32_t n)
{
	(void)r, (void)o, (void)of, (void)to, (void)i, (void)n;
	return false;
}
void audio_monitor_destroy(struct audio_monitor *m)
{
	(void)m;
}
struct audio_monitor *audio_monitor_create(obs_source_t *s)
{
	(void)s;
	return NULL;
}
bool devices_match(const char *a, const char *b)
{
	(void)a, (void)b;
	return false;
}

/* ---------------------------------------------------------------- context layer */

#define MAX_PUBLIC 64
static obs_source_t *public_srcs[MAX_PUBLIC];
static unsigned uuid_counter;

bool obs_context_data_init(struct obs_context_data *context, enum obs_obj_type type, obs_data_t *settings,
			   const char *name, const char *uuid, obs_data_t *hotkey_data, bool private)
{
	(void)uuid, (void)hotkey_data;
	context->type = type;
	context->private = private;
	context->name = bstrdup(name);
	char u[40];
	snprintf(u, sizeof(u), "uuid-%u", ++uuid_counter);
	context->uuid = bstrdup(u);
	context->settings = obs_data_create();
	if (settings)
		obs_data_apply(context->settings, settings);
	context->signals = signal_handler_create();
	return context->signals != NULL;
}
void obs_context_data_free(struct obs_context_data *context)
{
	signal_handler_destroy(context->signals);
	obs_data_release(context->settings);
	bfree(context->name);
	bfree(context->uuid);
}
void obs_context_init_control(struct obs_context_data *context, void *object, obs_destroy_cb destroy)
{
	context->control = bzalloc(sizeof(obs_weak_object_t));
	context->control->object = object;
	context->destroy = destroy;
}
void obs_context_data_insert_name(struct obs_context_data *c, pthread_mutex_t *m, void *f)
{
	(void)m, (void)f;
	for (int i = 0; i < MAX_PUBLIC; i++)
		if (!public_srcs[i]) {
			public_srcs[i] = (obs_source_t *)((char *)c - offsetof(struct obs_source, context));
			return;
		}
}
void obs_context_data_remove_name(struct obs_context_data *c, pthread_mutex_t *m, void *f)
{
	(void)m, (void)f;
	obs_source_t *s = (obs_source_t *)((char *)c - offsetof(struct obs_source, context));
	for (int i = 0; i < MAX_PUBLIC; i++)
		if (public_srcs[i] == s)
			public_srcs[i] = NULL;
}
void obs_context_data_insert_uuid(struct obs_context_data *c, pthread_mutex_t *m, void *f)
{
	(void)c, (void)m, (void)f;
}
void obs_context_data_remove_uuid(struct obs_context_data *c, pthread_mutex_t *m, void *f)
{
	(void)c, (void)m, (void)f;
}
void obs_context_data_setname(struct obs_context_data *c, const char *name)
{
	bfree(c->name);
	c->name = bstrdup(name);
}
void obs_context_data_setname_ht(struct obs_context_data *c, const char *name, void *ht)
{
	(void)ht;
	obs_context_data_setname(c, name);
}
void obs_context_wait(struct obs_context_data *c)
{
	(void)c;
}
obs_source_t *obs_get_source_by_name(const char *name)
{
	for (int i = 0; i < MAX_PUBLIC; i++)
		if (public_srcs[i] && !strcmp(public_srcs[i]->context.name, name))
			return obs_source_get_ref(public_srcs[i]);
	return NULL;
}

/* ---------------------------------------------------------------- deferred destruction */

#define MAX_TASKS 64
static struct {
	os_task_t fn;
	void *param;
} tasks[MAX_TASKS];
static int ntasks;

bool os_task_queue_queue_task(os_task_queue_t *q, os_task_t task, void *param)
{
	(void)q;
	if (ntasks >= MAX_TASKS)
		abort();
	tasks[ntasks].fn = task;
	tasks[ntasks].param = param;
	ntasks++;
	return true;
}
int os_drain_tasks(void (*before)(void *param))
{
	int n = 0;
	while (n < ntasks) { /* tasks queued while draining run too */
		if (before)
			before(tasks[n].param);
		tasks[n].fn(tasks[n].param);
		n++;
	}
	ntasks = 0;
	return n;
}
int os_pending_tasks(void)
{
	return ntasks;
}

/* ---------------------------------------------------------------- stubs for everything non-source */

void source_profiler_async_frame_received(obs_source_t *s)
{
	(void)s;
}
void source_profiler_remove_source(obs_source_t *s)
{
	(void)s;
}
uint64_t source_profiler_source_render_begin(gs_timer_t **t)
{
	(void)t;
	return 0;
}
void source_profiler_source_render_end(obs_source_t *s, uint64_t st, gs_timer_t *t)
{
	(void)s, (void)st, (void)t;
}
obs_hotkey_id obs_hotkey_register_source(obs_source_t *s, const char *n, const char *d, obs_hotkey_func f, void *p)
{
	(void)s, (void)n, (void)d, (void)f, (void)p;
	return 1;
}
obs_hotkey_pair_id obs_hotkey_pair_register_source(obs_source_t *s, const char *a, const char *b, const char *c,
						   const char *d, obs_hotkey_active_func e, obs_hotkey_active_func f,
						   void *g, void *h)
{
	(void)s, (void)a, (void)b, (void)c, (void)d, (void)e, (void)f, (void)g, (void)h;
	return 1;
}
void obs_hotkey_unregister(obs_hotkey_id id)
{
	(void)id;
}
void obs_hotkey_pair_unregister(obs_hotkey_pair_id id)
{
	(void)id;
}
bool obs_source_type_is_scene(const char *id)
{
	(void)id;
	return false;
}
bool obs_source_type_is_group(const char *id)
{
	(void)id;
	return false;
}
obs_scene_t *obs_scene_from_source(const obs_source_t *s)
{
	(void)s;
	return NULL;
}
obs_scene_t *obs_group_from_source(const obs_source_t *s)
{
	(void)s;
	return NULL;
}
obs_source_t *obs_scene_get_source(const obs_scene_t *s)
{
	(void)s;
	return NULL;
}
obs_scene_t *obs_scene_duplicate(obs_scene_t *s, const char *n, enum obs_scene_duplicate_type t)
{
	(void)s, (void)n, (void)t;
	return NULL;
}
void obs_canvas_insert_source(obs_canvas_t *c, obs_source_t *s)
{
	(void)c, (void)s;
}
void obs_canvas_remove_source(obs_source_t *s)
{
	(void)s;
}
void obs_canvas_rename_source(obs_source_t *s, const char *n)
{
	(void)s, (void)n;
}
obs_canvas_t *obs_weak_canvas_get_canvas(obs_weak_canvas_t *w)
{
	(void)w;
	return NULL;
}
void obs_properties_apply_settings(obs_properties_t *p, obs_data_t *s)
{
	(void)p, (void)s;
}
obs_missing_files_t *obs_missing_files_create(void)
{
	return NULL;
}
obs_source_t *obs_load_source(obs_data_t *d)
{
	printf("  load_source id=%s name=%s\n", obs_data_get_string(d, "id"), obs_data_get_string(d, "name"));
	return obs_source_create_private(obs_data_get_string(d, "id"), obs_data_get_string(d, "name"), NULL);
}
obs_data_t *obs_save_source(obs_source_t *s)
{
	obs_data_t *d = obs_data_create();
	obs_data_set_string(d, "id", s->info.id);
	obs_data_set_string(d, "name", s->context.name);
	return d;
}
float obs_get_video_sdr_white_level(void)
{
	return 300.0f;
}
float obs_get_video_hdr_nominal_peak_level(void)
{
	return 1000.0f;
}
/* Used by obs_transition_tick's manual-torque helper; linked from graphics/math-extra.c. */
void add_alignment(struct vec2 *v, uint32_t align, int cx, int cy)
{
	if (align & OBS_ALIGN_RIGHT)
		v->x += (float)cx;
	else if ((align & OBS_ALIGN_LEFT) == 0)
		v->x += (float)(cx / 2);
	if (align & OBS_ALIGN_BOTTOM)
		v->y += (float)cy;
	else if ((align & OBS_ALIGN_TOP) == 0)
		v->y += (float)(cy / 2);
}

/* graphics: everything succeeds, nothing is drawn */
static int dummy_gs;
void gs_enter_context(graphics_t *g) { (void)g; }
void gs_leave_context(void) {}
void obs_enter_graphics(void) {}
void obs_leave_graphics(void) {}
gs_texture_t *gs_texture_create(uint32_t w, uint32_t h, enum gs_color_format f, uint32_t l, const uint8_t **d,
				uint32_t fl)
{
	(void)w, (void)h, (void)f, (void)l, (void)d, (void)fl;
	return (gs_texture_t *)&dummy_gs;
}
void gs_texture_destroy(gs_texture_t *t) { (void)t; }
gs_texrender_t *gs_texrender_create(enum gs_color_format f, enum gs_zstencil_format z)
{
	(void)f, (void)z;
	return (gs_texrender_t *)&dummy_gs;
}
void gs_texrender_destroy(gs_texrender_t *t) { (void)t; }
void gs_texrender_reset(gs_texrender_t *t) { (void)t; }
bool gs_texrender_begin(gs_texrender_t *t, uint32_t w, uint32_t h) { (void)t, (void)w, (void)h; return true; }
bool gs_texrender_begin_with_color_space(gs_texrender_t *t, uint32_t w, uint32_t h, enum gs_color_space s)
{
	(void)t, (void)w, (void)h, (void)s;
	return true;
}
void gs_texrender_end(gs_texrender_t *t) { (void)t; }
gs_texture_t *gs_texrender_get_texture(const gs_texrender_t *t) { (void)t; return (gs_texture_t *)&dummy_gs; }
enum gs_color_format gs_texrender_get_format(const gs_texrender_t *t) { (void)t; return GS_RGBA; }
void gs_blend_state_push(void) {}
void gs_blend_state_pop(void) {}
void gs_blend_function(enum gs_blend_type a, enum gs_blend_type b) { (void)a, (void)b; }
void gs_clear(uint32_t f, const struct vec4 *c, float d, uint8_t s) { (void)f, (void)c, (void)d, (void)s; }
void gs_ortho(float l, float r, float t, float b, float n, float f) { (void)l, (void)r, (void)t, (void)b, (void)n, (void)f; }
void gs_matrix_push(void) {}
void gs_matrix_pop(void) {}
void gs_matrix_mul(const struct matrix4 *m) { (void)m; }
enum gs_color_space gs_get_color_space(void) { return GS_CS_SRGB; }
void gs_blend_function_separate(enum gs_blend_type a, enum gs_blend_type b, enum gs_blend_type c, enum gs_blend_type d) { (void)a, (void)b, (void)c, (void)d; }
void gs_draw(enum gs_draw_mode m, uint32_t s, uint32_t n) { (void)m, (void)s, (void)n; }
void gs_draw_sprite(gs_texture_t *t, uint32_t f, uint32_t w, uint32_t h) { (void)t, (void)f, (void)w, (void)h; }
gs_eparam_t *gs_effect_get_param_by_name(const gs_effect_t *e, const char *n) { (void)e, (void)n; return (gs_eparam_t *)&dummy_gs; }
gs_technique_t *gs_effect_get_technique(const gs_effect_t *e, const char *n) { (void)e, (void)n; return (gs_technique_t *)&dummy_gs; }
bool gs_effect_loop(gs_effect_t *e, const char *n) { (void)e, (void)n; return false; }
void gs_effect_set_bool(gs_eparam_t *p, bool v) { (void)p, (void)v; }
void gs_effect_set_float(gs_eparam_t *p, float v) { (void)p, (void)v; }
void gs_effect_set_int(gs_eparam_t *p, int v) { (void)p, (void)v; }
void gs_effect_set_matrix4(gs_eparam_t *p, const struct matrix4 *v) { (void)p, (void)v; }
void gs_effect_set_texture(gs_eparam_t *p, gs_texture_t *v) { (void)p, (void)v; }
void gs_effect_set_texture_srgb(gs_eparam_t *p, gs_texture_t *v) { (void)p, (void)v; }
void gs_effect_set_val(gs_eparam_t *p, const void *v, size_t n) { (void)p, (void)v, (void)n; }
void gs_effect_set_vec2(gs_eparam_t *p, const struct vec2 *v) { (void)p, (void)v; }
void gs_effect_set_vec4(gs_eparam_t *p, const struct vec4 *v) { (void)p, (void)v; }
void gs_enable_blending(bool e) { (void)e; }
void gs_enable_framebuffer_srgb(bool e) { (void)e; }
bool gs_framebuffer_srgb_enabled(void) { return false; }
gs_effect_t *gs_get_effect(void) { return (gs_effect_t *)&dummy_gs; }
bool gs_get_linear_srgb(void) { return false; }
void gs_matrix_rotaa4f(float x, float y, float z, float a) { (void)x, (void)y, (void)z, (void)a; }
void gs_matrix_translate3f(float x, float y, float z) { (void)x, (void)y, (void)z; }
bool gs_set_linear_srgb(bool l) { (void)l; return false; }
size_t gs_technique_begin(gs_technique_t *t) { (void)t; return 0; }
bool gs_technique_begin_pass(gs_technique_t *t, size_t p) { (void)t, (void)p; return false; }
void gs_technique_end(gs_technique_t *t) { (void)t; }
void gs_technique_end_pass(gs_technique_t *t) { (void)t; }
enum gs_color_format gs_texture_get_color_format(const gs_texture_t *t) { (void)t; return GS_RGBA; }
uint32_t gs_texture_get_height(const gs_texture_t *t) { (void)t; return 0; }
uint32_t gs_texture_get_width(const gs_texture_t *t) { (void)t; return 0; }
void gs_texture_set_image(gs_texture_t *t, const uint8_t *d, uint32_t l, bool i) { (void)t, (void)d, (void)l, (void)i; }
gs_effect_t *obs_load_effect(gs_effect_t **e, const char *p) { (void)e, (void)p; return (gs_effect_t *)&dummy_gs; }
gs_effect_t *obs_get_base_effect(enum obs_base_effect e) { (void)e; return (gs_effect_t *)&dummy_gs; }

/* ---------------------------------------------------------------- scripted plugin sources */

struct fk {
	obs_source_t *src;
	uint32_t w, h;
	obs_source_t *kids[8];
	int nkids;
	int fv_drop;       /* filter_video returns NULL */
	uint32_t hooks;
};

enum {
	H_CREATE = 1 << 0, H_DESTROY = 1 << 1, H_UPDATE = 1 << 2, H_ACTIVATE = 1 << 3, H_SHOW = 1 << 4,
	H_TICK = 1 << 5, H_ENUM = 1 << 6, H_SIZE = 1 << 7, H_DEFAULTS = 1 << 8, H_FADD = 1 << 9, H_FVIDEO = 1 << 10,
	H_TSTART = 1 << 11, H_TSTOP = 1 << 12, H_NOCREATE = 1 << 13, H_MEDIA = 1 << 14,
};

static void trace(void *data, const char *ev)
{
	struct fk *f = data;
	printf("  cb %s %s\n", f->src->context.name, ev);
}

static const char *fk_name(void *td)
{
	(void)td;
	return "fake";
}
static void *fk_create(obs_data_t *settings, obs_source_t *src);
static void fk_destroy(void *data)
{
	struct fk *f = data;
	trace(data, "destroy");
	bfree(f);
}
static void fk_update(void *data, obs_data_t *s)
{
	(void)s;
	trace(data, "update");
}
static void fk_activate(void *d) { trace(d, "activate"); }
static void fk_deactivate(void *d) { trace(d, "deactivate"); }
static void fk_show(void *d) { trace(d, "show"); }
static void fk_hide(void *d) { trace(d, "hide"); }
static void fk_tick(void *data, float sec)
{
	struct fk *f = data;
	printf("  cb %s tick %.9g\n", f->src->context.name, sec);
}
static void fk_enum(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct fk *f = data;
	for (int i = 0; i < f->nkids; i++)
		cb(f->src, f->kids[i], param);
}
static uint32_t fk_w(void *data) { return ((struct fk *)data)->w; }
static uint32_t fk_h(void *data) { return ((struct fk *)data)->h; }
static void fk_defaults(obs_data_t *s) { obs_data_set_default_int(s, "dflt", 7); }
static void fk_fadd(void *data, obs_source_t *parent)
{
	struct fk *f = data;
	printf("  cb %s filter_add parent=%s\n", f->src->context.name, parent->context.name);
}
static void fk_frem(void *data, obs_source_t *parent)
{
	struct fk *f = data;
	printf("  cb %s filter_remove parent=%s\n", f->src->context.name, parent->context.name);
}
static struct obs_source_frame *fk_fvideo(void *data, struct obs_source_frame *fr)
{
	struct fk *f = data;
	printf("  cb %s filter_video ts=%llu %ux%u\n", f->src->context.name, (unsigned long long)fr->timestamp,
	       fr->width, fr->height);
	return f->fv_drop ? NULL : fr;
}
static void fk_mpp(void *d, bool pause) { trace(d, pause ? "media_pause" : "media_play"); }
static void fk_mrestart(void *d) { trace(d, "media_restart"); }
static void fk_mstop(void *d) { trace(d, "media_stop"); }
static void fk_mnext(void *d) { trace(d, "media_next"); }
static void fk_mprev(void *d) { trace(d, "media_previous"); }
static void fk_mset(void *data, int64_t ms)
{
	struct fk *f = data;
	printf("  cb %s media_set_time %lld\n", f->src->context.name, (long long)ms);
}
static int64_t fk_mdur(void *d) { (void)d; return 60000; }
static int64_t fk_mtime(void *d) { (void)d; return 1234; }
static enum obs_media_state fk_mstate(void *d) { (void)d; return OBS_MEDIA_STATE_PAUSED; }
static void fk_tstart(void *d) { trace(d, "transition_start"); }
static void fk_tstop(void *d) { trace(d, "transition_stop"); }

static void *fk_create(obs_data_t *settings, obs_source_t *src)
{
	(void)settings;
	struct fk *f = bzalloc(sizeof(*f));
	f->src = src;
	f->hooks = (uint32_t)(uintptr_t)src->info.type_data;
	if (f->hooks & H_NOCREATE) {
		bfree(f);
		printf("  cb %s create-fail\n", src->context.name);
		return NULL;
	}
	trace(f, "create");
	return f;
}

#define MAX_KINDS 16
static char *kind_ids[MAX_KINDS];

bool fk_kind_add(const char *id, enum obs_source_type type, uint32_t output_flags, uint32_t hooks)
{
	struct obs_source_info info = {0};
	info.id = kind_ids[obs->source_types.num % MAX_KINDS] = strdup(id);
	info.unversioned_id = info.id;
	info.type = type;
	info.output_flags = output_flags;
	info.type_data = (void *)(uintptr_t)hooks;
	info.get_name = fk_name;
	info.create = fk_create;
	info.destroy = fk_destroy;
	if (hooks & H_UPDATE) info.update = fk_update;
	if (hooks & H_ACTIVATE) {
		info.activate = fk_activate;
		info.deactivate = fk_deactivate;
	}
	if (hooks & H_SHOW) {
		info.show = fk_show;
		info.hide = fk_hide;
	}
	if (hooks & H_TICK) info.video_tick = fk_tick;
	if (hooks & H_ENUM) info.enum_active_sources = fk_enum;
	if (hooks & H_SIZE) {
		info.get_width = fk_w;
		info.get_height = fk_h;
	}
	if (hooks & H_DEFAULTS) info.get_defaults = fk_defaults;
	if (hooks & H_FADD) {
		info.filter_add = fk_fadd;
		info.filter_remove = fk_frem;
	}
	if (hooks & H_FVIDEO) info.filter_video = fk_fvideo;
	if (hooks & H_MEDIA) {
		info.media_play_pause = fk_mpp;
		info.media_restart = fk_mrestart;
		info.media_stop = fk_mstop;
		info.media_next = fk_mnext;
		info.media_previous = fk_mprev;
		info.media_set_time = fk_mset;
		info.media_get_duration = fk_mdur;
		info.media_get_time = fk_mtime;
		info.media_get_state = fk_mstate;
	}
	if (hooks & H_TSTART) info.transition_start = fk_tstart;
	if (hooks & H_TSTOP) info.transition_stop = fk_tstop;
	da_push_back(obs->source_types, &info);
	return true;
}

uint32_t fk_hook_from_name(const char *n)
{
	static const struct {
		const char *n;
		uint32_t v;
	} t[] = {{"create", H_CREATE}, {"update", H_UPDATE}, {"activate", H_ACTIVATE}, {"show", H_SHOW},
		 {"tick", H_TICK}, {"enum", H_ENUM}, {"size", H_SIZE}, {"defaults", H_DEFAULTS},
		 {"filteradd", H_FADD}, {"fvideo", H_FVIDEO}, {"tstart", H_TSTART}, {"tstop", H_TSTOP},
		 {"nocreate", H_NOCREATE}, {"media", H_MEDIA}, {"destroy", H_DESTROY}};
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcmp(t[i].n, n))
			return t[i].v;
	return 0;
}

struct fk *fk_of(obs_source_t *s)
{
	return s->context.data;
}
void fk_set_size(obs_source_t *s, uint32_t w, uint32_t h)
{
	struct fk *f = s->context.data;
	f->w = w, f->h = h;
}
void fk_set_drop(obs_source_t *s, int drop)
{
	((struct fk *)s->context.data)->fv_drop = drop;
}
void fk_child_add(obs_source_t *p, obs_source_t *c)
{
	struct fk *f = p->context.data;
	if (f->nkids < 8)
		f->kids[f->nkids++] = c;
}
void fk_child_clear(obs_source_t *p)
{
	((struct fk *)p->context.data)->nkids = 0;
}

/* ---------------------------------------------------------------- core */

static const char *global_signals[] = {
	"void source_create(ptr source)",
	"void source_destroy(ptr source)",
	"void source_remove(ptr source)",
	"void source_update(ptr source)",
	"void source_activate(ptr source)",
	"void source_deactivate(ptr source)",
	"void source_show(ptr source)",
	"void source_hide(ptr source)",
	"void source_rename(ptr source, string new_name, string prev_name)",
	"void source_volume(ptr source, in out float volume)",
	"void source_filter_add(ptr source, ptr filter)",
	"void source_filter_remove(ptr source, ptr filter)",
	"void source_transition_start(ptr source)",
	"void source_transition_video_stop(ptr source)",
	"void source_transition_stop(ptr source)",
	NULL,
};

void core_reset(void)
{
	static bool first = true;
	if (first) {
		obs = bzalloc(sizeof(*obs));
		pthread_mutex_init(&obs->data.audio_sources_mutex, NULL);
		pthread_mutex_init(&obs->data.sources_mutex, NULL);
		obs->signals = signal_handler_create();
		signal_handler_add_array(obs->signals, global_signals);
		obs->destruction_task_thread = (os_task_queue_t *)&dummy_gs;
		obs->video.graphics = (graphics_t *)&dummy_gs;
		obs->audio.audio = (audio_t *)&fake_ai;
		first = false;
	}
	obs->video.video_time = 0;
	obs->video.video_half_frame_interval_ns = 8333333;
	os_clock_ns = 0;
}
