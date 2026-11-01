/*
 * Conformance harness (area obscore, spec S-042): scripted fakes for everything obs.c, obs-canvas.c,
 * obs-view.c and obs-display.c reach outside themselves: the graphics subsystem (a recording fake
 * device), the graphics thread, the audio-thread callback, the hotkey subsystem, the source /
 * output / encoder / service object layer (reference counting is the real libobs one; only the
 * constructors/destructors are fakes) and the module registration entry point.
 *
 * Every fake call that is observable in the spec prints one line "ev <text>" when its trace
 * category is enabled (runner command "trace"). Test tooling, not part of libobs.
 */
#include <inttypes.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "obs-internal.h"
#include "graphics/graphics.h"

/* ---- trace --------------------------------------------------------------------------------- */
bool trace_gs;  /* graphics device calls */
bool trace_sub; /* hotkeys / profiler / misc subsystem calls */
bool trace_src; /* fake source activation / render */

void ev(bool on, const char *fmt, ...)
{
	if (!on)
		return;
	va_list ap;
	va_start(ap, fmt);
	fputs("ev ", stdout);
	vprintf(fmt, ap);
	fputc('\n', stdout);
	va_end(ap);
}

/* ---- scripted failures --------------------------------------------------------------------- */
int fake_gs_error;           /* value returned by gs_create (0 = GS_SUCCESS) */
int fake_device = GS_DEVICE_DIRECT3D_11;
char fake_effect_fail[16][64]; /* effect file base names that fail to compile */
int fake_effect_fail_n;
int fake_tex_fail_at;        /* 1-based index of the gs_texture_create call that fails (0 = never) */
int fake_tex_calls;
int fake_surface_fail_at;    /* same for gs_stagesurface_create */
int fake_surface_calls;
bool fake_swap_fail;
bool fake_hotkey_platform_fail;
bool fake_present_ready = true;
int fake_color_space = GS_CS_SRGB;
bool fake_nv12 = true;
bool fake_p010 = true;
bool fake_data_found = true; /* find_libobs_data_file finds every file */
int fake_gs_live;            /* live fake graphics objects (textures, surfaces, effects, ...) */
int fake_ctx_depth;          /* gs_enter_context minus gs_leave_context */
int fake_ctx_underflow;

/* ---- platform data lookup ------------------------------------------------------------------ */
char *find_libobs_data_file(const char *file)
{
	if (!fake_data_found)
		return NULL;
	size_t n = strlen(file) + 6;
	char *r = bmalloc(n);
	snprintf(r, n, "DATA/%s", file);
	return r;
}
void log_system_info(void) {}

/* ---- graphics device ----------------------------------------------------------------------- */
static int fake_gfx_dummy;

int gs_create(graphics_t **graphics, const char *module, uint32_t adapter)
{
	ev(trace_gs, "gs_create module=%s adapter=%u", module ? module : "(null)", adapter);
	if (fake_gs_error)
		return fake_gs_error;
	*graphics = (graphics_t *)&fake_gfx_dummy;
	fake_gs_live++;
	return GS_SUCCESS;
}
void gs_destroy(graphics_t *graphics)
{
	(void)graphics;
	ev(trace_gs, "gs_destroy");
	fake_gs_live--;
}
void gs_enter_context(graphics_t *graphics)
{
	(void)graphics;
	fake_ctx_depth++;
}
void gs_leave_context(void)
{
	if (fake_ctx_depth == 0)
		fake_ctx_underflow++;
	else
		fake_ctx_depth--;
}
int gs_get_device_type(void)
{
	return fake_device;
}
enum gs_color_space gs_get_color_space(void)
{
	return fake_color_space;
}
bool gs_nv12_available(void)
{
	return fake_nv12;
}
bool gs_p010_available(void)
{
	return fake_p010;
}

gs_effect_t *gs_effect_create_from_file(const char *file, char **error_string)
{
	if (error_string)
		*error_string = NULL;
	if (!file) {
		ev(trace_gs, "effect (null) -> fail");
		return NULL;
	}
	const char *base = strrchr(file, '/');
	base = base ? base + 1 : file;
	for (int i = 0; i < fake_effect_fail_n; i++)
		if (!strcmp(base, fake_effect_fail[i])) {
			ev(trace_gs, "effect %s -> fail", base);
			return NULL;
		}
	ev(trace_gs, "effect %s", base);
	fake_gs_live++;
	return (gs_effect_t *)malloc(1);
}
void gs_effect_destroy(gs_effect_t *effect)
{
	if (effect) {
		fake_gs_live--;
		free(effect);
	}
}
gs_samplerstate_t *gs_samplerstate_create(const struct gs_sampler_info *info)
{
	ev(trace_gs, "sampler max_anisotropy=%d", info->max_anisotropy);
	fake_gs_live++;
	return (gs_samplerstate_t *)malloc(1);
}
void gs_samplerstate_destroy(gs_samplerstate_t *s)
{
	if (s) {
		fake_gs_live--;
		free(s);
	}
}
gs_texture_t *gs_texture_create(uint32_t cx, uint32_t cy, enum gs_color_format fmt, uint32_t levels,
				const uint8_t **data, uint32_t flags)
{
	(void)data;
	fake_tex_calls++;
	if (fake_tex_fail_at && fake_tex_calls == fake_tex_fail_at) {
		ev(trace_gs, "texture %ux%u fmt=%d levels=%u flags=%u -> fail", cx, cy, (int)fmt, levels, flags);
		return NULL;
	}
	ev(trace_gs, "texture %ux%u fmt=%d levels=%u flags=%u", cx, cy, (int)fmt, levels, flags);
	fake_gs_live++;
	return (gs_texture_t *)malloc(1);
}
bool gs_texture_create_nv12(gs_texture_t **tex_y, gs_texture_t **tex_uv, uint32_t width, uint32_t height,
			    uint32_t flags)
{
	ev(trace_gs, "texture_nv12 %ux%u flags=%u", width, height, flags);
	*tex_y = malloc(1);
	*tex_uv = malloc(1);
	fake_gs_live += 2;
	return true;
}
bool gs_texture_create_p010(gs_texture_t **tex_y, gs_texture_t **tex_uv, uint32_t width, uint32_t height,
			    uint32_t flags)
{
	ev(trace_gs, "texture_p010 %ux%u flags=%u", width, height, flags);
	*tex_y = malloc(1);
	*tex_uv = malloc(1);
	fake_gs_live += 2;
	return true;
}
void gs_texture_destroy(gs_texture_t *t)
{
	if (t) {
		fake_gs_live--;
		free(t);
	}
}
gs_stagesurf_t *gs_stagesurface_create(uint32_t width, uint32_t height, enum gs_color_format fmt)
{
	fake_surface_calls++;
	if (fake_surface_fail_at && fake_surface_calls == fake_surface_fail_at) {
		ev(trace_gs, "stagesurface %ux%u fmt=%d -> fail", width, height, (int)fmt);
		return NULL;
	}
	ev(trace_gs, "stagesurface %ux%u fmt=%d", width, height, (int)fmt);
	fake_gs_live++;
	return (gs_stagesurf_t *)malloc(1);
}
void gs_stagesurface_destroy(gs_stagesurf_t *s)
{
	if (s) {
		fake_gs_live--;
		free(s);
	}
}

/* display / swap chain */
gs_swapchain_t *gs_swapchain_create(const struct gs_init_data *data)
{
	ev(trace_gs, "swapchain_create %ux%u", data->cx, data->cy);
	if (fake_swap_fail)
		return NULL;
	fake_gs_live++;
	return (gs_swapchain_t *)malloc(1);
}
void gs_swapchain_destroy(gs_swapchain_t *s)
{
	ev(trace_gs, "swapchain_destroy");
	if (s) {
		fake_gs_live--;
		free(s);
	}
}
void gs_load_swapchain(gs_swapchain_t *s)
{
	(void)s;
	ev(trace_gs, "load_swapchain");
}
void gs_resize(uint32_t cx, uint32_t cy)
{
	ev(trace_gs, "resize %ux%u", cx, cy);
}
void gs_update_color_space(void)
{
	ev(trace_gs, "update_color_space");
}
bool gs_is_present_ready(void)
{
	return fake_present_ready;
}
void gs_begin_scene(void)
{
	ev(trace_gs, "begin_scene");
}
void gs_end_scene(void)
{
	ev(trace_gs, "end_scene");
}
void gs_present(void)
{
	ev(trace_gs, "present");
}
void gs_clear(uint32_t flags, const struct vec4 *color, float depth, uint8_t stencil)
{
	ev(trace_gs, "clear flags=%u color=%.9g,%.9g,%.9g,%.9g depth=%.9g stencil=%u", flags, color->x, color->y,
	   color->z, color->w, depth, stencil);
}
void gs_enable_depth_test(bool enable)
{
	ev(trace_gs, "depth_test %d", enable);
}
void gs_set_cull_mode(enum gs_cull_mode mode)
{
	ev(trace_gs, "cull_mode %d", (int)mode);
}
void gs_ortho(float left, float right, float top, float bottom, float znear, float zfar)
{
	ev(trace_gs, "ortho %.9g %.9g %.9g %.9g %.9g %.9g", left, right, top, bottom, znear, zfar);
}
void gs_set_viewport(int x, int y, int width, int height)
{
	ev(trace_gs, "viewport %d %d %d %d", x, y, width, height);
}
gs_eparam_t *gs_effect_get_param_by_name(const gs_effect_t *effect, const char *name)
{
	(void)effect;
	(void)name;
	return NULL;
}
void gs_effect_set_vec4(gs_eparam_t *p, const struct vec4 *v)
{
	(void)p;
	ev(trace_gs, "solid_color %.9g,%.9g,%.9g,%.9g", v->x, v->y, v->z, v->w);
}
bool gs_effect_loop(gs_effect_t *effect, const char *name)
{
	(void)effect;
	static int n;
	/* one pass per loop */
	if (n++ % 2 == 0) {
		ev(trace_gs, "effect_loop %s", name);
		return true;
	}
	return false;
}
void gs_draw_sprite(gs_texture_t *tex, uint32_t flip, uint32_t width, uint32_t height)
{
	(void)tex;
	ev(trace_gs, "draw_sprite flip=%u %ux%u", flip, width, height);
}

/* ---- graphics thread / audio thread / profiler glue ----------------------------------------- */
extern THREAD_LOCAL bool is_graphics_thread;

/* Minimal stand-in for the product's graphics thread: it runs queued graphics tasks until the main
 * canvas video output is stopped. It never touches the graphics fakes or stdout. */
void *obs_graphics_thread(void *param)
{
	(void)param;
	is_graphics_thread = true;
	for (;;) {
		struct obs_task_info info;
		for (;;) {
			bool have = false;
			pthread_mutex_lock(&obs->video.task_mutex);
			if (obs->video.tasks.size) {
				deque_pop_front(&obs->video.tasks, &info, sizeof(info));
				have = true;
			}
			pthread_mutex_unlock(&obs->video.task_mutex);
			if (!have)
				break;
			info.task(info.param);
		}
		pthread_mutex_lock(&obs->video.mixes_mutex);
		bool stopped = obs->video.mixes.num == 0 || video_output_stopped(obs->video.mixes.array[0]->video);
		pthread_mutex_unlock(&obs->video.mixes_mutex);
		if (stopped)
			break;
		os_sleep_ms(1);
	}
	return NULL;
}

/* Audio thread callback: only the task queue part of the product's implementation. */
bool audio_callback(void *param, uint64_t start_ts_in, uint64_t end_ts_in, uint64_t *out_ts, uint32_t mixers,
		    struct audio_output_data *mixes)
{
	(void)param;
	(void)start_ts_in;
	(void)end_ts_in;
	(void)out_ts;
	(void)mixers;
	(void)mixes;
	struct obs_core_audio *audio = &obs->audio;
	for (;;) {
		struct obs_task_info info;
		bool have = false;
		pthread_mutex_lock(&audio->task_mutex);
		if (audio->tasks.size) {
			deque_pop_front(&audio->tasks, &info, sizeof(info));
			have = true;
		}
		pthread_mutex_unlock(&audio->task_mutex);
		if (!have)
			break;
		info.task(info.param);
	}
	return false;
}

void source_profiler_reset_video(struct obs_video_info *ovi)
{
	ev(trace_sub, "source_profiler_reset_video %ux%u", ovi->output_width, ovi->output_height);
}

/* ---- hotkey subsystem ---------------------------------------------------------------------- */
bool obs_hotkeys_platform_init(struct obs_core_hotkeys *h)
{
	(void)h;
	ev(trace_sub, "hotkeys_platform_init");
	return !fake_hotkey_platform_fail;
}
void obs_hotkeys_platform_free(struct obs_core_hotkeys *h)
{
	(void)h;
	ev(trace_sub, "hotkeys_platform_free");
}
void *obs_hotkey_thread(void *arg)
{
	(void)arg;
	os_event_wait(obs->hotkeys.stop_event);
	return NULL;
}
void obs_hotkeys_free(void)
{
	ev(trace_sub, "hotkeys_free");
}
void obs_hotkey_name_map_free(void)
{
	ev(trace_sub, "hotkey_name_map_free");
}
void obs_hotkeys_context_release(struct obs_context_data *context)
{
	(void)context;
}

/* ---- scene / group info (registered by obs_init) -------------------------------------------- */
static const char *fake_name(void *u)
{
	(void)u;
	return "fake";
}
const struct obs_source_info scene_info = {.id = "scene", .type = OBS_SOURCE_TYPE_SCENE, .get_name = fake_name};
const struct obs_source_info group_info = {.id = "group", .type = OBS_SOURCE_TYPE_SCENE, .get_name = fake_name};

/* Registration (the real one lives in the module loader, spec S-035): this fake keeps only the part
 * of its observable effect that the registries in obs.c expose: the versioned id, the unversioned
 * id, the per-type array and the global array. Order of insertion is preserved. */
void obs_register_source_s(const struct obs_source_info *info, size_t size)
{
	struct obs_source_info data = {0};
	obs_source_info_array_t *array = NULL;
	if (info->type == OBS_SOURCE_TYPE_INPUT)
		array = &obs->input_types;
	else if (info->type == OBS_SOURCE_TYPE_FILTER)
		array = &obs->filter_types;
	else if (info->type == OBS_SOURCE_TYPE_TRANSITION)
		array = &obs->transition_types;
	memcpy(&data, info, size < sizeof(data) ? size : sizeof(data));
	data.unversioned_id = data.id;
	if (data.version) {
		struct dstr v = {0};
		dstr_printf(&v, "%s_v%d", data.id, (int)data.version);
		data.id = v.array;
	} else {
		data.id = bstrdup(data.id);
	}
	if (array)
		da_push_back(*array, &data);
	da_push_back(obs->source_types, &data);
}

/* ---- source / output / encoder / service object layer ----------------------------------------
 * Objects are created by the runner (struct with a real obs_context_data, real control block).
 * The "destroy" fakes unlink the object from every table libobs may have put it in. */
extern void fake_free_object(void *obj, int kind);

obs_source_t *obs_source_get_ref(obs_source_t *source)
{
	return source ? (void *)obs_object_get_ref((obs_object_t *)source) : NULL;
}
void obs_source_release(obs_source_t *source)
{
	obs_object_release((obs_object_t *)source);
}
obs_output_t *obs_output_get_ref(obs_output_t *o)
{
	return o ? (void *)obs_object_get_ref((obs_object_t *)o) : NULL;
}
obs_encoder_t *obs_encoder_get_ref(obs_encoder_t *o)
{
	return o ? (void *)obs_object_get_ref((obs_object_t *)o) : NULL;
}
obs_service_t *obs_service_get_ref(obs_service_t *o)
{
	return o ? (void *)obs_object_get_ref((obs_object_t *)o) : NULL;
}
void obs_source_destroy(struct obs_source *s)
{
	fake_free_object(s, 0);
}
void obs_output_destroy(obs_output_t *o)
{
	fake_free_object(o, 1);
}
void obs_encoder_destroy(obs_encoder_t *o)
{
	fake_free_object(o, 2);
}
void obs_service_destroy(obs_service_t *o)
{
	fake_free_object(o, 3);
}
bool obs_source_is_scene(const obs_source_t *source)
{
	return source && source->info.type == OBS_SOURCE_TYPE_SCENE;
}
bool obs_source_is_group(const obs_source_t *source)
{
	return source && !strcmp(source->info.id, "group");
}
obs_scene_t *obs_scene_from_source(const obs_source_t *source)
{
	return obs_source_is_scene(source) ? source->context.data : NULL;
}
void obs_scene_enum_items(obs_scene_t *scene, bool (*cb)(obs_scene_t *, obs_sceneitem_t *, void *), void *p)
{
	(void)scene;
	(void)cb;
	(void)p;
}
void obs_source_activate(obs_source_t *source, enum view_type type)
{
	ev(trace_src, "activate %s type=%s", source->context.name, type == MAIN_VIEW ? "main" : "aux");
}
void obs_source_deactivate(obs_source_t *source, enum view_type type)
{
	ev(trace_src, "deactivate %s type=%s", source->context.name, type == MAIN_VIEW ? "main" : "aux");
}
void obs_source_video_render(obs_source_t *source)
{
	ev(trace_src, "render %s", source->context.name);
}

/* build-generated version strings (the real values are build specific, vectors never print them) */
const char *OBS_VERSION = "0.0.0-conformance";
const char *OBS_VERSION_CANONICAL = "0.0.0-conformance";

/* audio-io.c links against the resampler only when a consumer asks for a conversion; scripts never do. */
void audio_resampler_destroy(audio_resampler_t *rs)
{
	(void)rs;
}
