/*
 * Conformance runner for the libobs core context (spec S-042): startup/shutdown, video and audio
 * reset, canvases, views, displays, registries/enumeration/lookup, context-data name handling.
 * Command list: section "obscore runner" in test/conformance/README.md.
 *
 * The runner #includes libobs/obs.c, obs-canvas.c, obs-view.c and obs-display.c (so that file-local
 * helpers are reachable and the real global core pointer `obs` is used). Graphics, the graphics
 * thread, hotkeys and the source/output/encoder/service object layer are fakes
 * (gen/obscore_stubs.c); video-io.c / audio-io.c / format-conversion.c, obs-data.c, the signal and
 * proc handlers, the task queue, the profiler name store and the platform layer are the real code.
 */
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "harness.h"
#include "obs-internal.h"
#include "obs-data.h"
#include "obs.c"
#include "obs-canvas.c"
#include "obs-view.c"
#include "obs-display.c"

/* ---- fakes from obscore_stubs.c ---- */
extern bool trace_gs, trace_sub, trace_src;
extern int fake_gs_error, fake_device, fake_effect_fail_n, fake_tex_fail_at, fake_tex_calls, fake_surface_fail_at,
	fake_surface_calls, fake_color_space, fake_gs_live, fake_ctx_depth, fake_ctx_underflow;
extern char fake_effect_fail[16][64];
extern bool fake_swap_fail, fake_hotkey_platform_fail, fake_present_ready, fake_nv12, fake_p010, fake_data_found;
extern void ev(bool on, const char *fmt, ...);

static void die(const char *msg, const char *what)
{
	fprintf(stderr, "script error: %s %s\n", msg, what ? what : "");
	exit(2);
}

/* ---- handle table ---- */
enum kind { K_SRC, K_OUT, K_ENC, K_SVC, K_CANVAS, K_VIEW, K_DISP, K_NONE };
#define MAXH 96
static struct ent {
	char name[32];
	void *p;
	int kind;
	bool used;
} tbl[MAXH];

static void h_put(const char *name, void *p, int kind)
{
	for (int i = 0; i < MAXH; i++)
		if (tbl[i].used && !strcmp(tbl[i].name, name)) {
			tbl[i].p = p;
			tbl[i].kind = kind;
			return;
		}
	for (int i = 0; i < MAXH; i++)
		if (!tbl[i].used) {
			snprintf(tbl[i].name, sizeof tbl[i].name, "%s", name);
			tbl[i].p = p;
			tbl[i].kind = kind;
			tbl[i].used = true;
			return;
		}
	die("handle table full", name);
}
static void *h_get(const char *name)
{
	if (!strcmp(name, "-") || !strcmp(name, "null"))
		return NULL;
	if (!strcmp(name, "main"))
		return obs ? obs->data.main_canvas : NULL;
	for (int i = 0; i < MAXH; i++)
		if (tbl[i].used && !strcmp(tbl[i].name, name))
			return tbl[i].p;
	die("unknown handle", name);
	return NULL;
}
static const char *h_name(const void *p)
{
	if (!p)
		return "NULL";
	if (obs && p == obs->data.main_canvas)
		return "main";
	for (int i = 0; i < MAXH; i++)
		if (tbl[i].used && tbl[i].p == p)
			return tbl[i].name;
	return "?";
}
static const char *ctx_name(const void *obj)
{
	return obj ? ((const struct obs_context_data *)obj)->name : "NULL";
}

/* ---- fake objects ---- */
static int dummy_data;

void fake_free_object(void *obj, int kind)
{
	struct obs_context_data *c = obj;
	if (kind == 0) {
		struct obs_source *s = obj;
		obs_context_data_remove_uuid(c, &obs->data.sources_mutex, &obs->data.sources);
		if (s->canvas)
			obs_canvas_remove_source(s);
		if (s->default_flags == 0xC0DE) /* marker: object is in the public source table */
			obs_context_data_remove_name(c, &obs->data.sources_mutex, &obs->data.public_sources);
		bfree((void *)s->info.id);
	} else {
		obs_context_data_remove(c);
	}
	obs_context_data_free(c);
	bfree(obj);
}

static const char *kv(struct script_line *sl, int from, const char *key, const char *def)
{
	size_t n = strlen(key);
	for (int i = from; i < sl->argc; i++)
		if (!strncmp(sl->argv[i], key, n) && sl->argv[i][n] == '=')
			return sl->argv[i] + n + 1;
	return def;
}

static int pick_flag(const char *s, const char *const *names, const int *vals, int n, const char *what)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(s, names[i]))
			return vals[i];
	if (s[0] >= '0' && s[0] <= '9')
		return atoi(s);
	die(what, s);
	return 0;
}

static void *make_object(int kind, const char *name, struct script_line *sl, int from)
{
	bool priv = atoi(kv(sl, from, "private", "0")) != 0;
	const char *uuid = kv(sl, from, "uuid", NULL);
	const char *id = kv(sl, from, "id", "fake");
	const char *type = kv(sl, from, "type", "input");
	if (!strcmp(name, "-"))
		name = NULL;
	if (kind == K_SRC) {
		struct obs_source *s = bzalloc(sizeof *s);
		s->info.id = bstrdup(id);
		s->info.type = !strcmp(type, "scene")	       ? OBS_SOURCE_TYPE_SCENE
			       : !strcmp(type, "filter")     ? OBS_SOURCE_TYPE_FILTER
			       : !strcmp(type, "transition") ? OBS_SOURCE_TYPE_TRANSITION
							     : OBS_SOURCE_TYPE_INPUT;
		obs_context_data_init(&s->context, OBS_OBJ_TYPE_SOURCE, NULL, name, uuid, NULL, priv);
		s->context.data = &dummy_data;
		obs_context_init_control(&s->context, s, (obs_destroy_cb)obs_source_destroy);
		const char *cv = kv(sl, from, "canvas", NULL);
		obs_context_data_insert_uuid(&s->context, &obs->data.sources_mutex, &obs->data.sources);
		if (cv)
			obs_canvas_insert_source(h_get(cv), s);
		else if (!priv) {
			obs_context_data_insert_name(&s->context, &obs->data.sources_mutex, &obs->data.public_sources);
			s->default_flags = 0xC0DE;
		}
		return s;
	}
	size_t sz = kind == K_OUT ? sizeof(struct obs_output)
		    : kind == K_ENC ? sizeof(struct obs_encoder)
				    : sizeof(struct obs_service);
	struct obs_context_data *c = bzalloc(sz);
	enum obs_obj_type t = kind == K_OUT   ? OBS_OBJ_TYPE_OUTPUT
			      : kind == K_ENC ? OBS_OBJ_TYPE_ENCODER
					      : OBS_OBJ_TYPE_SERVICE;
	obs_context_data_init(c, t, NULL, name, uuid, NULL, priv);
	/* info.id lives right behind the context in all three structs */
	const char **idp = kind == K_OUT   ? (const char **)&((struct obs_output *)c)->info.id
			   : kind == K_ENC ? (const char **)&((struct obs_encoder *)c)->info.id
					   : (const char **)&((struct obs_service *)c)->info.id;
	*idp = bstrdup(id);
	c->data = (kv(sl, from, "nodata", NULL) ? NULL : &dummy_data);
	obs_destroy_cb d = kind == K_OUT   ? (obs_destroy_cb)obs_output_destroy
			   : kind == K_ENC ? (obs_destroy_cb)obs_encoder_destroy
					   : (obs_destroy_cb)obs_service_destroy;
	obs_context_init_control(c, c, d);
	if (kind == K_OUT)
		obs_context_data_insert(c, &obs->data.outputs_mutex, &obs->data.first_output);
	else if (kind == K_ENC)
		obs_context_data_insert(c, &obs->data.encoders_mutex, &obs->data.first_encoder);
	else
		obs_context_data_insert(c, &obs->data.services_mutex, &obs->data.first_service);
	return c;
}

/* ---- names for enums ---- */
static const char *vcode(int c)
{
	switch (c) {
	case OBS_VIDEO_SUCCESS:
		return "success";
	case OBS_VIDEO_FAIL:
		return "fail";
	case OBS_VIDEO_NOT_SUPPORTED:
		return "not_supported";
	case OBS_VIDEO_INVALID_PARAM:
		return "invalid_param";
	case OBS_VIDEO_CURRENTLY_ACTIVE:
		return "currently_active";
	case OBS_VIDEO_MODULE_NOT_FOUND:
		return "module_not_found";
	}
	return "?";
}

static const char *const spk_names[] = {"unknown", "mono", "stereo", "2.1", "4.0", "4.1", "5.1", "x7", "7.1"};
static int parse_speakers(const char *s)
{
	for (int i = 0; i < 9; i++)
		if (!strcmp(s, spk_names[i]))
			return i;
	return atoi(s);
}
static int parse_vfmt(const char *s)
{
	for (int i = 0; i < 40; i++)
		if (!strcmp(s, get_video_format_name(i)) && strcmp(s, "None"))
			return i;
	if (!strcmp(s, "none"))
		return VIDEO_FORMAT_NONE;
	return atoi(s);
}
static int parse_cs(const char *s)
{
	static const char *const n[] = {"default", "601", "709", "srgb", "pq", "hlg"};
	for (int i = 0; i < 6; i++)
		if (!strcmp(s, n[i]))
			return i;
	return atoi(s);
}
static int parse_range(const char *s)
{
	static const char *const n[] = {"default", "partial", "full"};
	for (int i = 0; i < 3; i++)
		if (!strcmp(s, n[i]))
			return i;
	return atoi(s);
}
static int parse_scale(const char *s)
{
	static const char *const n[] = {"disable", "point", "bicubic", "bilinear", "lanczos", "area"};
	for (int i = 0; i < 6; i++)
		if (!strcmp(s, n[i]))
			return i;
	return atoi(s);
}

static void parse_ovi(struct obs_video_info *o, struct script_line *sl, int from)
{
	memset(o, 0, sizeof *o);
	unsigned a = 1920, b = 1080, c = 1280, d = 720, fn = 30, fd = 1;
	sscanf(kv(sl, from, "base", "1920x1080"), "%ux%u", &a, &b);
	sscanf(kv(sl, from, "out", "1280x720"), "%ux%u", &c, &d);
	sscanf(kv(sl, from, "fps", "30/1"), "%u/%u", &fn, &fd);
	o->base_width = a;
	o->base_height = b;
	o->output_width = c;
	o->output_height = d;
	o->fps_num = fn;
	o->fps_den = fd;
	o->output_format = parse_vfmt(kv(sl, from, "fmt", "NV12"));
	o->colorspace = parse_cs(kv(sl, from, "cs", "709"));
	o->range = parse_range(kv(sl, from, "range", "partial"));
	o->scale_type = parse_scale(kv(sl, from, "scale", "bicubic"));
	o->gpu_conversion = atoi(kv(sl, from, "gpu", "1"));
	o->adapter = atoi(kv(sl, from, "adapter", "0"));
	const char *m = kv(sl, from, "module", "fake-gfx");
	o->graphics_module = strcmp(m, "-") ? m : NULL;
}

static void print_ovi(const char *tag, const struct obs_video_info *o)
{
	printf("%s base=%ux%u out=%ux%u fps=%u/%u fmt=%s cs=%d range=%d scale=%d gpu=%d adapter=%u\n", tag,
	       o->base_width, o->base_height, o->output_width, o->output_height, o->fps_num, o->fps_den,
	       get_video_format_name(o->output_format), (int)o->colorspace, (int)o->range, (int)o->scale_type,
	       o->gpu_conversion, o->adapter);
}

static void print_mix(const char *tag, struct obs_core_video_mix *m)
{
	if (!m) {
		printf("%s none\n", tag);
		return;
	}
	printf("%s gpu_conversion=%d conversion_needed=%d techs=%s,%s,%s wi=%.9g hi=%.9g nv12=%d p010=%d "
	       "render_space=%d mix_audio=%d view=%s\n",
	       tag, m->gpu_conversion, m->conversion_needed, m->conversion_techs[0] ? m->conversion_techs[0] : "-",
	       m->conversion_techs[1] ? m->conversion_techs[1] : "-",
	       m->conversion_techs[2] ? m->conversion_techs[2] : "-", m->conversion_width_i, m->conversion_height_i,
	       m->using_nv12_tex, m->using_p010_tex, (int)m->render_space, m->mix_audio, m->view ? "set" : "none");
	printf("%s matrix", tag);
	for (int i = 0; i < 16; i++)
		printf(" %.9g", m->color_matrix[i]);
	printf("\n");
	print_ovi("  ovi", &m->ovi);
}

static struct obs_core_video_mix *canvas_mix(const char *h)
{
	struct obs_canvas *c = h_get(h);
	return c ? c->mix : NULL;
}

/* ---- signal logging ---- */
static void sig_cb(void *param, calldata_t *cd)
{
	const char *name = param;
	printf("signal %s", name);
	void *p = NULL;
	if (calldata_get_ptr(cd, "canvas", &p) && p)
		printf(" canvas=%s", h_name(p));
	p = NULL;
	if (calldata_get_ptr(cd, "source", &p) && p)
		printf(" source=%s", ctx_name(p));
	p = NULL;
	if (calldata_get_ptr(cd, "prev_source", &p) && p)
		printf(" prev_source=%s", ctx_name(p));
	long long ch;
	if (calldata_get_int(cd, "channel", &ch))
		printf(" channel=%lld", ch);
	const char *s;
	if (calldata_get_string(cd, "new_name", &s) && s)
		printf(" new_name=\"%s\"", s);
	if (calldata_get_string(cd, "prev_name", &s) && s)
		printf(" prev_name=\"%s\"", s);
	printf("\n");
}

/* ---- enumeration callbacks ---- */
struct enum_ctx {
	int seen;
	int stop;
};
static bool enum_cb(void *param, void *obj)
{
	struct enum_ctx *e = param;
	e->seen++;
	printf("  visit %s\n", ctx_name(obj));
	return !(e->stop && e->seen >= e->stop);
}

static bool vinfo_cb(void *p, struct obs_video_info *o)
{
	struct enum_ctx *e = p;
	e->seen++;
	printf("  video_info out=%ux%u fps=%u/%u\n", o->output_width, o->output_height, o->fps_num, o->fps_den);
	return !(e->stop && e->seen >= e->stop);
}

/* ---- callbacks lists ---- */
static void audio_sink(void *p, size_t mix, struct audio_data *d)
{
	(void)p;
	(void)mix;
	(void)d;
}
static void tick_a(void *p, float s)
{
	(void)p;
	(void)s;
}
static void tick_b(void *p, float s)
{
	(void)p;
	(void)s;
}
static void draw_a(void *p, uint32_t cx, uint32_t cy)
{
	printf("  draw %s %ux%u\n", (const char *)p, cx, cy);
}
static void rendered_a(void *p)
{
	(void)p;
}
static void task_cb(void *p)
{
	printf("  task %s thread=%s\n", (const char *)p,
	       is_graphics_thread ? "graphics"
	       : is_audio_thread  ? "audio"
	       : is_ui_thread	  ? "ui"
				  : "other");
}
static void ui_handler(obs_task_t task, void *param, bool wait)
{
	printf("  ui_handler wait=%d\n", wait);
	task(param);
}

static const char *obj_type_name(int t)
{
	switch (t) {
	case OBS_OBJ_TYPE_INVALID:
		return "invalid";
	case OBS_OBJ_TYPE_SOURCE:
		return "source";
	case OBS_OBJ_TYPE_OUTPUT:
		return "output";
	case OBS_OBJ_TYPE_ENCODER:
		return "encoder";
	case OBS_OBJ_TYPE_SERVICE:
		return "service";
	case OBS_OBJ_TYPE_CANVAS:
		return "canvas";
	}
	return "?";
}

static int parse_canvas_flags(const char *s)
{
	static const char *const n[] = {"main", "activate", "mix_audio", "scene_ref", "ephemeral", "program", "preview", "device", "removed"};
	static const int v[] = {MAIN, ACTIVATE, MIX_AUDIO, SCENE_REF, EPHEMERAL, PROGRAM, PREVIEW, DEVICE, (int)(1u << 31)};
	int r = 0;
	char buf[128];
	snprintf(buf, sizeof buf, "%s", s);
	for (char *t = strtok(buf, "|"); t; t = strtok(NULL, "|"))
		r |= pick_flag(t, n, v, 9, "canvas flag");
	return r;
}

static void print_canvas(const char *h)
{
	struct obs_canvas *c = h_get(h);
	if (!c) {
		printf("canvas %s NULL\n", h);
		return;
	}
	printf("canvas %s name=%s%s%s flags=0x%x private=%d removed=%d has_video=%d video_active=%d", h,
	       obs_canvas_get_name(c) ? "\"" : "", obs_canvas_get_name(c) ? obs_canvas_get_name(c) : "NULL",
	       obs_canvas_get_name(c) ? "\"" : "", obs_canvas_get_flags(c), c->context.private, obs_canvas_removed(c),
	       obs_canvas_has_video(c), obs_canvas_video_active(c));
	if (c->flags & MAIN)
		printf(" uuid=%s", obs_canvas_get_uuid(c));
	else
		printf(" uuid_len=%zu", strlen(obs_canvas_get_uuid(c)));
	printf(" view_type=%s\n", c->view.type == MAIN_VIEW ? "main" : "aux");
}

static void print_state(void)
{
	if (!obs) {
		printf("state core=none\n");
		return;
	}
	printf("state core=up locale=%s modcfg=%s name_store_owned=%d\n", obs->locale ? obs->locale : "NULL",
	       obs->module_config_path ? "set" : "NULL", obs->name_store_owned);
	printf("  types source=%zu input=%zu filter=%zu transition=%zu output=%zu encoder=%zu service=%zu\n",
	       obs->source_types.num, obs->input_types.num, obs->filter_types.num, obs->transition_types.num,
	       obs->output_types.num, obs->encoder_types.num, obs->service_types.num);
	printf("  graphics=%s audio=%s mixes=%zu thread_initialized=%d valid=%d\n",
	       obs->video.graphics ? "yes" : "no", obs->audio.audio ? "yes" : "no", obs->video.mixes.num,
	       obs->video.thread_initialized, obs->data.valid);
	printf("  main_canvas name=\"%s\" flags=0x%x mix=%s\n", obs->data.main_canvas ? obs->data.main_canvas->context.name : "NULL",
	       obs->data.main_canvas ? obs->data.main_canvas->flags : 0,
	       obs->data.main_canvas && obs->data.main_canvas->mix ? "yes" : "none");
}

static obs_data_t *g_prev_private;
static int live_mark;

/* "TMP" = temp dir with trailing slash, "TMPNOSLASH" = temp dir without; anything else literally */
static const char *tmp_path(const char *s)
{
	static char buf[2][1024];
	const char *t = getenv("CONFORMANCE_TMPDIR");
	if (!strcmp(s, "TMP")) {
		snprintf(buf[0], sizeof buf[0], "%s/", t);
		return buf[0];
	}
	if (!strcmp(s, "TMPNOSLASH")) {
		snprintf(buf[1], sizeof buf[1], "%s", t);
		return buf[1];
	}
	return s;
}

/* Log handler: like the harness one, but generated UUIDs (random) are masked as <uuid>. */
static void mask_uuids(char *m)
{
	for (char *p = m; *p; p++) {
		static const int dash[] = {8, 13, 18, 23};
		bool ok = strlen(p) >= 36;
		for (int i = 0; ok && i < 36; i++) {
			bool isd = i == dash[0] || i == dash[1] || i == dash[2] || i == dash[3];
			ok = isd ? p[i] == '-' : isxdigit((unsigned char)p[i]) != 0;
		}
		if (ok) {
			memcpy(p, "<uuid>", 6);
			memmove(p + 6, p + 36, strlen(p + 36) + 1);
		}
	}
}
static void masked_log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	mask_uuids(msg);
	for (char *q = strstr(msg, "(null)"); q; q = strstr(q, "(null)")) {
		memcpy(q, "<null>", 6); /* glibc's rendering of a NULL %s is not portable */
	}
	for (char *p = msg; *p; p++)
		if (*p == '\n' || *p == '\t')
			*p = ' ';
	printf("log %s %s\n", level == LOG_ERROR ? "error" : level == LOG_WARNING ? "warning" : level == LOG_INFO ? "info" : "debug", msg);
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	base_set_log_handler(masked_log_handler, NULL);
	struct script_line sl;
	int lineno = 0;
	profiler_name_store_t *ext_store = NULL;
	const char *tmpdir = getenv("CONFORMANCE_TMPDIR");

	while (script_next(f, &sl, &lineno)) {
		char **a = sl.argv;
		const char *cmd = a[0];

		if (!strcmp(cmd, "echo")) {
			for (int i = 1; i < sl.argc; i++)
				printf("%s%s", i > 1 ? " " : "", a[i]);
			printf("\n");
		} else if (!strcmp(cmd, "trace")) { /* trace gs|sub|src on|off */
			script_need(&sl, 3);
			bool on = !strcmp(a[2], "on");
			if (!strcmp(a[1], "gs"))
				trace_gs = on;
			else if (!strcmp(a[1], "sub"))
				trace_sub = on;
			else if (!strcmp(a[1], "src"))
				trace_src = on;
			else
				die("trace category", a[1]);
		} else if (!strcmp(cmd, "fake")) { /* fake KEY VALUE */
			script_need(&sl, 3);
			const char *k = a[1], *v = a[2];
			if (!strcmp(k, "gs_error")) {
				fake_gs_error = !strcmp(v, "success")		  ? 0
						: !strcmp(v, "fail")		  ? GS_ERROR_FAIL
						: !strcmp(v, "module_not_found") ? GS_ERROR_MODULE_NOT_FOUND
						: !strcmp(v, "not_supported")	  ? GS_ERROR_NOT_SUPPORTED
										  : atoi(v);
			} else if (!strcmp(k, "device")) {
				fake_device = !strcmp(v, "opengl") ? GS_DEVICE_OPENGL : GS_DEVICE_DIRECT3D_11;
			} else if (!strcmp(k, "effect_fail")) {
				snprintf(fake_effect_fail[fake_effect_fail_n++], 64, "%s", v);
			} else if (!strcmp(k, "effect_fail_clear")) {
				fake_effect_fail_n = 0;
			} else if (!strcmp(k, "tex_fail_at")) {
				fake_tex_fail_at = atoi(v);
				fake_tex_calls = 0;
			} else if (!strcmp(k, "surface_fail_at")) {
				fake_surface_fail_at = atoi(v);
				fake_surface_calls = 0;
			} else if (!strcmp(k, "swap_fail")) {
				fake_swap_fail = atoi(v);
			} else if (!strcmp(k, "hotkey_platform_fail")) {
				fake_hotkey_platform_fail = atoi(v);
			} else if (!strcmp(k, "present_ready")) {
				fake_present_ready = atoi(v);
			} else if (!strcmp(k, "color_space")) {
				fake_color_space = !strcmp(v, "srgb")	     ? GS_CS_SRGB
						   : !strcmp(v, "srgb16f")   ? GS_CS_SRGB_16F
						   : !strcmp(v, "709ext")    ? GS_CS_709_EXTENDED
									     : atoi(v);
			} else if (!strcmp(k, "nv12")) {
				fake_nv12 = atoi(v);
			} else if (!strcmp(k, "p010")) {
				fake_p010 = atoi(v);
			} else if (!strcmp(k, "data_found")) {
				fake_data_found = atoi(v);
			} else {
				die("unknown fake", k);
			}
		} else if (!strcmp(cmd, "report")) {
			printf("report ctx_depth=%d ctx_underflow=%d\n", fake_ctx_depth, fake_ctx_underflow);
		} else if (!strcmp(cmd, "live_mark")) { /* remember the number of live fake graphics objects */
			live_mark = fake_gs_live;
		} else if (!strcmp(cmd, "live_delta")) {
			printf("live_delta %d\n", fake_gs_live - live_mark);
		} else if (!strcmp(cmd, "state")) {
			print_state();

			/* ---- lifecycle ---- */
		} else if (!strcmp(cmd, "startup")) { /* startup LOCALE|- CONFIG|- [store] */
			script_need(&sl, 3);
			if (sl.argc > 3 && !strcmp(a[3], "store") && !ext_store)
				ext_store = profiler_name_store_create();
			bool r = obs_startup(strcmp(a[1], "-") ? a[1] : NULL, strcmp(a[2], "-") ? a[2] : NULL,
					     sl.argc > 3 && !strcmp(a[3], "store") ? ext_store : NULL);
			printf("startup -> %s\n", r ? "true" : "false");
		} else if (!strcmp(cmd, "shutdown")) {
			printf("shutdown begin\n");
			obs_shutdown();
			printf("shutdown end obs=%s\n", obs ? "set" : "NULL");
		} else if (!strcmp(cmd, "free_ext_store")) {
			profiler_name_store_free(ext_store);
			ext_store = NULL;
		} else if (!strcmp(cmd, "initialized")) {
			printf("initialized -> %d\n", obs_initialized());
		} else if (!strcmp(cmd, "version")) {
			uint32_t v = obs_get_version();
			printf("version major=%u minor=%u patch=%u string_nonempty=%d\n", v >> 24, (v >> 16) & 0xff,
			       v & 0xffff, strlen(obs_get_version_string()) > 0);
		} else if (!strcmp(cmd, "locale_set")) {
			script_need(&sl, 2);
			obs_set_locale(strcmp(a[1], "-") ? a[1] : NULL);
		} else if (!strcmp(cmd, "locale_get")) {
			const char *l = obs_get_locale();
			printf("locale -> %s\n", l ? l : "NULL");
		} else if (!strcmp(cmd, "cmdline_set")) {
			const char *av[16];
			for (int i = 1; i < sl.argc; i++)
				av[i - 1] = a[i];
			obs_set_cmdline_args(sl.argc - 1, av);
		} else if (!strcmp(cmd, "cmdline_get")) {
			struct obs_cmdline_args c = obs_get_cmdline_args();
			printf("cmdline argc=%d argv=", c.argc);
			if (!c.argv)
				printf("NULL");
			for (int i = 0; c.argv && i < c.argc; i++)
				printf("%s%s", i ? "," : "", c.argv[i]);
			printf(" terminator=%s\n", c.argv && !c.argv[c.argc] ? "NULL" : "n/a");
		} else if (!strcmp(cmd, "cmdline_argc")) { /* only the count: safe even after shutdown */
			printf("cmdline_argc -> %d\n", obs_get_cmdline_args().argc);
		} else if (!strcmp(cmd, "name_store")) {
			profiler_name_store_t *s = obs_get_profiler_name_store();
			printf("name_store non_null=%d external=%d\n", s != NULL, s != NULL && s == ext_store);
		} else if (!strcmp(cmd, "data_path_add")) {
			script_need(&sl, 2);
			obs_add_data_path(tmp_path(a[1]));
		} else if (!strcmp(cmd, "data_path_remove")) {
			script_need(&sl, 2);
			printf("data_path_remove -> %d\n", obs_remove_data_path(tmp_path(a[1])));
		} else if (!strcmp(cmd, "touch")) { /* touch NAME: create an empty file in the temp dir */
			script_need(&sl, 2);
			char path[1024];
			snprintf(path, sizeof path, "%s/%s", tmpdir, a[1]);
			FILE *t = fopen(path, "wb");
			if (t)
				fclose(t);
		} else if (!strcmp(cmd, "find_data")) {
			script_need(&sl, 2);
			char *r = obs_find_data_file(a[1]);
			size_t tl = tmpdir ? strlen(tmpdir) : 0;
			printf("find_data %s -> %s\n", a[1],
			       !r ? "NULL" : (tmpdir && !strncmp(r, tmpdir, tl) ? "in_TMP" : "elsewhere"));
			bfree(r);

			/* ---- video / audio ---- */
		} else if (!strcmp(cmd, "reset_video")) { /* reset_video [key=value ...] */
			struct obs_video_info ovi;
			parse_ovi(&ovi, &sl, 1);
			int r = obs_reset_video(&ovi);
			printf("reset_video -> %s\n", vcode(r));
			print_ovi("  after_call", &ovi);
		} else if (!strcmp(cmd, "probe_reset_video") || !strcmp(cmd, "probe_reset_audio")) {
			/* run the call in a forked child so that a crash of the reference becomes a result line */
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				freopen("/dev/null", "w", stdout);
				if (!strcmp(cmd, "probe_reset_video")) {
					struct obs_video_info ovi;
					parse_ovi(&ovi, &sl, 1);
					_exit(obs_reset_video(&ovi) == OBS_VIDEO_SUCCESS ? 0 : 1);
				} else {
					struct obs_audio_info2 o = {0};
					o.samples_per_sec = (uint32_t)atoll(kv(&sl, 1, "sps", "48000"));
					o.speakers = parse_speakers(kv(&sl, 1, "speakers", "stereo"));
					o.max_buffering_ms = (uint32_t)atoll(kv(&sl, 1, "max_ms", "0"));
					o.fixed_buffering = atoi(kv(&sl, 1, "fixed", "0"));
					_exit(obs_reset_audio2(&o) ? 0 : 1);
				}
			}
			int st = 0;
			waitpid(pid, &st, 0);
			if (WIFSIGNALED(st))
				printf("%s -> crash signal=%s\n", cmd, WTERMSIG(st) == SIGFPE ? "SIGFPE" : WTERMSIG(st) == SIGSEGV ? "SIGSEGV" : WTERMSIG(st) == SIGABRT ? "SIGABRT" : "other");
			else
				printf("%s -> %s\n", cmd, WEXITSTATUS(st) == 0 ? "returned success" : "returned failure");
		} else if (!strcmp(cmd, "video_info")) {
			struct obs_video_info o;
			memset(&o, 0xAB, sizeof o);
			bool r = obs_get_video_info(&o);
			printf("video_info -> %d\n", r);
			if (r)
				print_ovi("  ", &o);
		} else if (!strcmp(cmd, "mix_info")) { /* mix_info CANVAS */
			script_need(&sl, 2);
			print_mix("mix", canvas_mix(a[1]));
		} else if (!strcmp(cmd, "mixes")) {
			printf("mixes count=%zu\n", obs->video.mixes.num);
			for (size_t i = 0; i < obs->video.mixes.num; i++) {
				struct obs_core_video_mix *m = obs->video.mixes.array[i];
				const char *owner = "view";
				for (int t = 0; t < MAXH; t++)
					if (tbl[t].used && tbl[t].kind == K_CANVAS && ((struct obs_canvas *)tbl[t].p)->mix == m)
						owner = tbl[t].name;
				if (obs->data.main_canvas->mix == m)
					owner = "main";
				printf("  [%zu] owner=%s out=%ux%u fps=%u/%u view=%s\n", i, owner, m->ovi.output_width,
				       m->ovi.output_height, m->ovi.fps_num, m->ovi.fps_den, m->view ? "set" : "none");
			}
		} else if (!strcmp(cmd, "mix_active")) { /* mix_active CANVAS raw=N gpu=N */
			struct obs_core_video_mix *m = canvas_mix(a[1]);
			if (!m)
				die("no mix", a[1]);
			m->raw_active = atoi(kv(&sl, 2, "raw", "0"));
			m->gpu_encoder_active = atoi(kv(&sl, 2, "gpu", "0"));
		} else if (!strcmp(cmd, "video_active")) {
			printf("video_active -> %d\n", obs_video_active());
		} else if (!strcmp(cmd, "intervals")) {
			printf("intervals frame_interval_ns=%" PRIu64 " half=%" PRIu64 "\n", obs_get_frame_interval_ns(),
			       obs->video.video_half_frame_interval_ns);
		} else if (!strcmp(cmd, "set_levels")) {
			script_need(&sl, 3);
			obs_set_video_levels((float)atof(a[1]), (float)atof(a[2]));
		} else if (!strcmp(cmd, "levels")) {
			printf("levels sdr=%.9g hdr=%.9g\n", obs_get_video_sdr_white_level(),
			       obs_get_video_hdr_nominal_peak_level());
		} else if (!strcmp(cmd, "reset_audio")) { /* reset_audio sps=N speakers=S [max_ms=N] [fixed=0|1] */
			struct obs_audio_info2 o = {0};
			o.samples_per_sec = (uint32_t)atoll(kv(&sl, 1, "sps", "48000"));
			o.speakers = parse_speakers(kv(&sl, 1, "speakers", "stereo"));
			o.max_buffering_ms = (uint32_t)atoll(kv(&sl, 1, "max_ms", "0"));
			o.fixed_buffering = atoi(kv(&sl, 1, "fixed", "0"));
			printf("reset_audio -> %d\n", obs_reset_audio2(&o));
		} else if (!strcmp(cmd, "reset_audio_null")) {
			printf("reset_audio2(NULL) -> %d\n", obs_reset_audio2(NULL));
		} else if (!strcmp(cmd, "reset_audio_v1")) {
			struct obs_audio_info o;
			o.samples_per_sec = (uint32_t)atoll(kv(&sl, 1, "sps", "48000"));
			o.speakers = parse_speakers(kv(&sl, 1, "speakers", "stereo"));
			printf("reset_audio_v1 -> %d\n", obs_reset_audio(&o));
		} else if (!strcmp(cmd, "audio_info")) {
			struct obs_audio_info o = {0xdead, 0xbeef};
			bool r = obs_get_audio_info(&o);
			printf("audio_info -> %d", r);
			if (r)
				printf(" sps=%u speakers=%d", o.samples_per_sec, (int)o.speakers);
			printf("\n");
			struct obs_audio_info2 o2 = {0xdead, 0xbeef, 0xfeed, true};
			r = obs_get_audio_info2(&o2);
			printf("audio_info2 -> %d", r);
			if (r)
				printf(" sps=%u speakers=%d max_ms=%u fixed=%d", o2.samples_per_sec, (int)o2.speakers,
				       o2.max_buffering_ms, o2.fixed_buffering);
			printf("\n");
			printf("audio_info(NULL) -> %d\n", obs_get_audio_info(NULL));
		} else if (!strcmp(cmd, "audio_connect") || !strcmp(cmd, "audio_disconnect")) {
			/* attach / detach a (never called) consumer to mix 0: makes the audio output "active" */
			if (!strcmp(cmd, "audio_connect"))
				printf("audio_connect -> %d\n", audio_output_connect(obs->audio.audio, 0, NULL, audio_sink, NULL));
			else
				audio_output_disconnect(obs->audio.audio, 0, audio_sink, NULL);
		} else if (!strcmp(cmd, "audio_state")) {
			printf("audio_state ticks=%u fixed=%d mon_name=%s mon_id=%s audio=%s\n",
			       (unsigned)obs->audio.max_buffering_ticks, obs->audio.fixed_buffer,
			       obs->audio.monitoring_device_name ? obs->audio.monitoring_device_name : "NULL",
			       obs->audio.monitoring_device_id ? obs->audio.monitoring_device_id : "NULL",
			       obs->audio.audio ? "yes" : "no");
			/* ---- registries ---- */
		} else if (!strcmp(cmd, "reg_source")) { /* reg_source ID type=input|filter|transition|scene [version=N] */
			script_need(&sl, 2);
			struct obs_source_info i = {0};
			const char *t = kv(&sl, 2, "type", "input");
			i.id = strdup(a[1]); /* registry keeps the pointer, so it must outlive the script line */
			i.type = !strcmp(t, "scene")	      ? OBS_SOURCE_TYPE_SCENE
				 : !strcmp(t, "filter")	      ? OBS_SOURCE_TYPE_FILTER
				 : !strcmp(t, "transition")   ? OBS_SOURCE_TYPE_TRANSITION
							      : OBS_SOURCE_TYPE_INPUT;
			i.version = (uint32_t)atoi(kv(&sl, 2, "version", "0"));
			obs_register_source_s(&i, sizeof i);
		} else if (!strcmp(cmd, "reg_output") || !strcmp(cmd, "reg_encoder") || !strcmp(cmd, "reg_service")) {
			script_need(&sl, 2);
			/* direct registry fill (the real registration is spec S-035) */
			if (!strcmp(cmd, "reg_output")) {
				struct obs_output_info i = {.id = strdup(a[1])};
				da_push_back(obs->output_types, &i);
			} else if (!strcmp(cmd, "reg_encoder")) {
				struct obs_encoder_info i = {.id = strdup(a[1])};
				da_push_back(obs->encoder_types, &i);
			} else {
				struct obs_service_info i = {.id = strdup(a[1])};
				da_push_back(obs->service_types, &i);
			}
		} else if (!strcmp(cmd, "enum_types")) { /* enum_types source|input|filter|transition|output|encoder|service */
			script_need(&sl, 2);
			printf("enum_types %s\n", a[1]);
			size_t i = 0;
			for (;; i++) {
				const char *id = NULL;
				bool r = !strcmp(a[1], "source")      ? obs_enum_source_types(i, &id)
					 : !strcmp(a[1], "input")     ? obs_enum_input_types(i, &id)
					 : !strcmp(a[1], "filter")    ? obs_enum_filter_types(i, &id)
					 : !strcmp(a[1], "transition") ? obs_enum_transition_types(i, &id)
					 : !strcmp(a[1], "output")    ? obs_enum_output_types(i, &id)
					 : !strcmp(a[1], "encoder")   ? obs_enum_encoder_types(i, &id)
								      : obs_enum_service_types(i, &id);
				if (!r)
					break;
				printf("  [%zu] %s\n", i, id);
			}
			const char *untouched = "UNTOUCHED";
			bool r = obs_enum_source_types(i + 1000, &untouched);
			printf("  past_end -> %d out=%s\n", r, untouched);
		} else if (!strcmp(cmd, "enum_input2")) { /* enum_input2 IDX [noid] [nouv] */
			script_need(&sl, 2);
			const char *id = "UNTOUCHED", *uv = "UNTOUCHED";
			bool noid = false, nouv = false;
			for (int i = 2; i < sl.argc; i++) {
				noid |= !strcmp(a[i], "noid");
				nouv |= !strcmp(a[i], "nouv");
			}
			bool r = obs_enum_input_types2((size_t)atoi(a[1]), noid ? NULL : &id, nouv ? NULL : &uv);
			printf("enum_input2 %s -> %d id=%s unversioned=%s\n", a[1], r, id, uv);
		} else if (!strcmp(cmd, "latest_input")) {
			script_need(&sl, 2);
			const char *r = obs_get_latest_input_type_id(strcmp(a[1], "-") ? a[1] : NULL);
			printf("latest_input %s -> %s\n", a[1], r ? r : "NULL");
		} else if (!strcmp(cmd, "protocol_add")) {
			script_need(&sl, 2);
			char *p = bstrdup(a[1]);
			da_push_back(obs->data.protocols, &p);
		} else if (!strcmp(cmd, "protocol_registered")) {
			script_need(&sl, 2);
			printf("protocol_registered %s -> %d\n", a[1], obs_is_output_protocol_registered(a[1]));
		} else if (!strcmp(cmd, "protocols")) {
			char *p = NULL;
			for (size_t i = 0; obs_enum_output_protocols(i, &p); i++)
				printf("  protocol[%zu] %s\n", i, p);
			printf("  past_end -> %d\n", obs_enum_output_protocols(obs->data.protocols.num, &p));

			/* ---- objects ---- */
		} else if (!strcmp(cmd, "mk_source") || !strcmp(cmd, "mk_output") || !strcmp(cmd, "mk_encoder") ||
			   !strcmp(cmd, "mk_service")) { /* mk_X H NAME|- [private=1] [id=..] [type=..] [uuid=..] */
			script_need(&sl, 3);
			int k = !strcmp(cmd, "mk_source")   ? K_SRC
				: !strcmp(cmd, "mk_output") ? K_OUT
				: !strcmp(cmd, "mk_encoder") ? K_ENC
							     : K_SVC;
			void *o = make_object(k, a[2], &sl, 3);
			h_put(a[1], o, k);
			printf("%s %s name=%s%s%s private=%d uuid=%s\n", cmd, a[1], ((struct obs_context_data *)o)->name ? "\"" : "",
			       ((struct obs_context_data *)o)->name ? ((struct obs_context_data *)o)->name : "NULL",
			       ((struct obs_context_data *)o)->name ? "\"" : "",
			       ((struct obs_context_data *)o)->private,
			       ((struct obs_context_data *)o)->uuid ? "set" : "NULL");
		} else if (!strcmp(cmd, "rel")) {
			script_need(&sl, 2);
			obs_object_release(h_get(a[1]));
		} else if (!strcmp(cmd, "ref")) {
			script_need(&sl, 2);
			void *o = obs_object_get_ref(h_get(a[1]));
			printf("ref %s -> %s\n", a[1], o ? "ok" : "NULL");
		} else if (!strcmp(cmd, "refs")) {
			script_need(&sl, 2);
			struct obs_context_data *c = h_get(a[1]);
			printf("refs %s strong=%ld weak=%ld\n", a[1], c->control->ref.refs, c->control->ref.weak_refs);
		} else if (!strcmp(cmd, "enum")) { /* enum KIND [stop=N] */
			script_need(&sl, 2);
			struct enum_ctx e = {0, atoi(kv(&sl, 2, "stop", "0"))};
			printf("enum %s\n", a[1]);
			if (!strcmp(a[1], "sources"))
				obs_enum_sources((bool (*)(void *, obs_source_t *))enum_cb, &e);
			else if (!strcmp(a[1], "all_sources"))
				obs_enum_all_sources((bool (*)(void *, obs_source_t *))enum_cb, &e);
			else if (!strcmp(a[1], "scenes"))
				obs_enum_scenes((bool (*)(void *, obs_source_t *))enum_cb, &e);
			else if (!strcmp(a[1], "outputs"))
				obs_enum_outputs((bool (*)(void *, obs_output_t *))enum_cb, &e);
			else if (!strcmp(a[1], "encoders"))
				obs_enum_encoders((bool (*)(void *, obs_encoder_t *))enum_cb, &e);
			else if (!strcmp(a[1], "services"))
				obs_enum_services((bool (*)(void *, obs_service_t *))enum_cb, &e);
			else if (!strcmp(a[1], "canvases"))
				obs_enum_canvases((bool (*)(void *, obs_canvas_t *))enum_cb, &e);
			else if (!strcmp(a[1], "canvas_scenes"))
				obs_canvas_enum_scenes(h_get(a[2]), (bool (*)(void *, obs_source_t *))enum_cb, &e);
			else
				die("enum kind", a[1]);
			printf("  visited=%d\n", e.seen);
		} else if (!strcmp(cmd, "get")) { /* get KIND KEY  (KEY may be uuid:HANDLE) */
			script_need(&sl, 3);
			const char *key = a[2];
			char *tmp = NULL;
			if (!strncmp(key, "uuid:", 5)) {
				const char *u = ((struct obs_context_data *)h_get(key + 5))->uuid;
				tmp = bstrdup(u);
				key = tmp;
			}
			void *r = NULL;
			const char *k = a[1];
			if (!strcmp(k, "source_by_name"))
				r = obs_get_source_by_name(key);
			else if (!strcmp(k, "source_by_uuid"))
				r = obs_get_source_by_uuid(key);
			else if (!strcmp(k, "canvas_by_name"))
				r = obs_get_canvas_by_name(key);
			else if (!strcmp(k, "canvas_by_uuid"))
				r = obs_get_canvas_by_uuid(key);
			else if (!strcmp(k, "transition_by_name"))
				r = obs_get_transition_by_name(key);
			else if (!strcmp(k, "transition_by_uuid"))
				r = obs_get_transition_by_uuid(key);
			else if (!strcmp(k, "output_by_name"))
				r = obs_get_output_by_name(key);
			else if (!strcmp(k, "encoder_by_name"))
				r = obs_get_encoder_by_name(key);
			else if (!strcmp(k, "service_by_name"))
				r = obs_get_service_by_name(key);
			else if (!strcmp(k, "canvas_source_by_name"))
				r = obs_canvas_get_source_by_name(h_get(a[3]), key);
			else if (!strcmp(k, "canvas_scene_by_name"))
				r = obs_canvas_get_scene_by_name(h_get(a[3]), key);
			else if (!strcmp(k, "main_canvas"))
				r = obs_get_main_canvas();
			else
				die("get kind", k);
			if (r && !strcmp(k, "canvas_scene_by_name")) {
				printf("get %s %s -> scene(ok)\n", k, a[2]);
			} else if (r) {
				struct obs_context_data *c = r;
				printf("get %s %s -> \"%s\" obj=%s strong=%ld\n", k, a[2], c->name, h_name(r), c->control->ref.refs);
				obs_object_release(r);
			} else {
				printf("get %s %s -> NULL\n", k, a[2]);
			}
			bfree(tmp);
		} else if (!strcmp(cmd, "obj_api")) { /* obj_api H|- */
			script_need(&sl, 2);
			void *o = h_get(a[1]);
			const char *id = obs_obj_get_id(o);
			printf("obj_api %s type=%s id=%s invalid=%d data=%s private=%d\n", a[1],
			       obj_type_name(obs_obj_get_type(o)), id ? id : "NULL", obs_obj_invalid(o),
			       obs_obj_get_data(o) ? "set" : "NULL", obs_obj_is_private(o));
		} else if (!strcmp(cmd, "uuid_reset")) { /* uuid_reset H... : prints whether uuid changed */
			char *old[16];
			int n = sl.argc - 1;
			for (int i = 0; i < n; i++)
				old[i] = bstrdup(((struct obs_context_data *)h_get(a[i + 1]))->uuid);
			obs_reset_source_uuids();
			for (int i = 0; i < n; i++) {
				const char *nu = ((struct obs_context_data *)h_get(a[i + 1]))->uuid;
				printf("uuid_reset %s changed=%d len=%zu\n", a[i + 1], strcmp(old[i], nu) != 0, strlen(nu));
				bfree(old[i]);
			}
			obs_source_t *found = n > 0 ? obs_get_source_by_uuid(((struct obs_context_data *)h_get(a[1]))->uuid) : NULL;
			printf("  after: get by new uuid ok=%d\n", found != NULL);
			obs_source_release(found);
		} else if (!strcmp(cmd, "src_rename")) { /* src_rename H NAME : obs_canvas_rename_source */
			script_need(&sl, 3);
			obs_canvas_rename_source(h_get(a[1]), a[2]);
			printf("src_rename %s -> \"%s\"\n", a[1], ctx_name(h_get(a[1])));
		} else if (!strcmp(cmd, "src_canvas_remove")) {
			script_need(&sl, 2);
			obs_canvas_remove_source(h_get(a[1]));
			printf("src_canvas_remove %s canvas=%s\n", a[1], ((struct obs_source *)h_get(a[1]))->canvas ? "set" : "NULL");
		} else if (!strcmp(cmd, "src_canvas_insert")) {
			script_need(&sl, 3);
			obs_canvas_insert_source(h_get(a[1]), h_get(a[2]));
		} else if (!strcmp(cmd, "src_removed")) { /* mark a source removed (for view_render) */
			((struct obs_source *)h_get(a[1]))->removed = true;

			/* ---- canvases ---- */
		} else if (!strcmp(cmd, "canvas_create") || !strcmp(cmd, "canvas_create_private")) {
			/* canvas_create H NAME|- [flags=a|b] [video=1 + reset_video keys] */
			script_need(&sl, 3);
			struct obs_video_info ovi;
			parse_ovi(&ovi, &sl, 3);
			bool with_video = atoi(kv(&sl, 3, "video", "0"));
			uint32_t flags = parse_canvas_flags(kv(&sl, 3, "flags", "0"));
			const char *name = strcmp(a[2], "-") ? a[2] : NULL;
			obs_canvas_t *c = !strcmp(cmd, "canvas_create") ? obs_canvas_create(name, with_video ? &ovi : NULL, flags)
									 : obs_canvas_create_private(name, with_video ? &ovi : NULL, flags);
			if (c)
				h_put(a[1], c, K_CANVAS);
			printf("%s %s -> %s\n", cmd, a[1], c ? "ok" : "NULL");
			if (c)
				print_canvas(a[1]);
		} else if (!strcmp(cmd, "canvas_info")) {
			script_need(&sl, 2);
			print_canvas(a[1]);
		} else if (!strcmp(cmd, "canvas_release")) {
			script_need(&sl, 2);
			obs_canvas_release(h_get(a[1]));
		} else if (!strcmp(cmd, "canvas_ref")) {
			script_need(&sl, 2);
			obs_canvas_t *c = obs_canvas_get_ref(h_get(a[1]));
			printf("canvas_ref %s -> %s\n", a[1], c ? "ok" : "NULL");
		} else if (!strcmp(cmd, "canvas_weak")) { /* canvas_weak W C : obtain weak ref */
			script_need(&sl, 3);
			h_put(a[1], obs_canvas_get_weak_canvas(h_get(a[2])), K_NONE);
		} else if (!strcmp(cmd, "weak_get")) { /* weak_get W : try to upgrade (leaks the new strong ref intentionally released) */
			script_need(&sl, 2);
			obs_canvas_t *c = obs_weak_canvas_get_canvas(h_get(a[1]));
			printf("weak_get %s -> %s\n", a[1], c ? ctx_name(c) : "NULL");
			if (c)
				obs_canvas_release(c);
		} else if (!strcmp(cmd, "weak_release")) {
			script_need(&sl, 2);
			obs_weak_canvas_release(h_get(a[1]));
		} else if (!strcmp(cmd, "canvas_name")) {
			script_need(&sl, 3);
			obs_canvas_set_name(h_get(a[1]), strcmp(a[2], "-") ? a[2] : NULL);
			printf("canvas_name %s -> \"%s\"\n", a[1], obs_canvas_get_name(h_get(a[1])));
		} else if (!strcmp(cmd, "canvas_remove")) {
			script_need(&sl, 2);
			obs_canvas_remove(h_get(a[1]));
			printf("canvas_remove %s removed=%d\n", a[1], obs_canvas_removed(h_get(a[1])));
		} else if (!strcmp(cmd, "canvas_reset_video")) { /* canvas_reset_video C [null | keys] */
			script_need(&sl, 2);
			struct obs_video_info ovi;
			parse_ovi(&ovi, &sl, 2);
			bool nul = sl.argc > 2 && !strcmp(a[2], "null");
			bool r = obs_canvas_reset_video(h_get(a[1]), nul ? NULL : &ovi);
			printf("canvas_reset_video %s -> %d\n", a[1], r);
		} else if (!strcmp(cmd, "canvas_video_info")) {
			script_need(&sl, 2);
			struct obs_video_info o;
			memset(&o, 0, sizeof o);
			bool r = obs_canvas_get_video_info(h_get(a[1]), &o);
			printf("canvas_video_info %s -> %d video=%s\n", a[1], r,
			       obs_canvas_get_video(h_get(a[1])) ? "set" : "NULL");
			if (r)
				print_ovi("  ", &o);
		} else if (!strcmp(cmd, "canvas_valid_ovi")) {
			script_need(&sl, 2);
			printf("canvas_has_valid_video_info %s -> %d\n", a[1], obs_canvas_has_valid_video_info(h_get(a[1])));
		} else if (!strcmp(cmd, "canvas_save")) {
			script_need(&sl, 2);
			obs_data_t *d = obs_save_canvas(h_get(a[1]));
			printf("canvas_save %s ->", a[1]);
			if (!d)
				printf(" NULL\n");
			else {
				bool main_ = obs_canvas_get_flags(h_get(a[1])) & MAIN;
				printf(" name=\"%s\" uuid=%s private=%d flags=%lld\n", obs_data_get_string(d, "name"),
				       main_ ? obs_data_get_string(d, "uuid") : "(generated)", obs_data_get_bool(d, "private"),
				       obs_data_get_int(d, "flags"));
				obs_data_release(d);
			}
		} else if (!strcmp(cmd, "canvas_load")) { /* canvas_load H name uuid|- private flags */
			script_need(&sl, 6);
			obs_data_t *d = obs_data_create();
			obs_data_set_string(d, "name", a[2]);
			if (strcmp(a[3], "-"))
				obs_data_set_string(d, "uuid", a[3]);
			obs_data_set_bool(d, "private", atoi(a[4]));
			obs_data_set_int(d, "flags", (long long)strtoull(a[5], NULL, 0));
			obs_canvas_t *c = obs_load_canvas(d);
			obs_data_release(d);
			h_put(a[1], c, K_CANVAS);
			print_canvas(a[1]);
			if (c && strcmp(a[3], "-"))
				printf("  uuid_matches=%d\n", strcmp(obs_canvas_get_uuid(c), a[3]) == 0);
		} else if (!strcmp(cmd, "canvas_set_channel")) { /* canvas_set_channel C CH SRC|- */
			script_need(&sl, 4);
			obs_canvas_set_channel(h_get(a[1]), (uint32_t)atoi(a[2]), h_get(a[3]));
		} else if (!strcmp(cmd, "canvas_get_channel")) {
			script_need(&sl, 3);
			obs_source_t *s = obs_canvas_get_channel(h_get(a[1]), (uint32_t)atoi(a[2]));
			printf("canvas_get_channel %s %s -> %s", a[1], a[2], s ? ctx_name(s) : "NULL");
			if (s)
				printf(" strong=%ld", s->context.control->ref.refs);
			printf("\n");
			obs_source_release(s);
		} else if (!strcmp(cmd, "set_output_source")) {
			script_need(&sl, 3);
			obs_set_output_source((uint32_t)atoi(a[1]), h_get(a[2]));
		} else if (!strcmp(cmd, "get_output_source")) {
			script_need(&sl, 2);
			obs_source_t *s = obs_get_output_source((uint32_t)atoi(a[1]));
			printf("get_output_source %s -> %s\n", a[1], s ? ctx_name(s) : "NULL");
			obs_source_release(s);
		} else if (!strcmp(cmd, "canvas_render")) {
			script_need(&sl, 2);
			obs_canvas_render(h_get(a[1]));
		} else if (!strcmp(cmd, "sig_obs")) { /* sig_obs NAME... : connect printing handlers on the core signals */
			for (int i = 1; i < sl.argc; i++)
				signal_handler_connect(obs->signals, a[i], sig_cb, strdup(a[i]));
		} else if (!strcmp(cmd, "sig_canvas")) { /* sig_canvas C NAME... */
			script_need(&sl, 3);
			obs_canvas_t *c = h_get(a[1]);
			for (int i = 2; i < sl.argc; i++) {
				char *tag = bzalloc(strlen(a[i]) + strlen(a[1]) + 4);
				sprintf(tag, "%s:%s", a[1], a[i]);
				signal_handler_connect(obs_canvas_get_signal_handler(c), a[i], sig_cb, tag);
			}

			/* ---- views ---- */
		} else if (!strcmp(cmd, "view_create")) {
			script_need(&sl, 2);
			obs_view_t *v = obs_view_create();
			h_put(a[1], v, K_VIEW);
			printf("view_create %s type=%s\n", a[1], v->type == MAIN_VIEW ? "main" : "aux");
		} else if (!strcmp(cmd, "view_destroy")) {
			script_need(&sl, 2);
			obs_view_destroy(h_get(a[1]));
		} else if (!strcmp(cmd, "view_set")) {
			script_need(&sl, 4);
			obs_view_set_source(h_get(a[1]), (uint32_t)atoi(a[2]), h_get(a[3]));
		} else if (!strcmp(cmd, "view_get")) {
			script_need(&sl, 3);
			obs_source_t *s = obs_view_get_source(h_get(a[1]), (uint32_t)atoi(a[2]));
			printf("view_get %s %s -> %s\n", a[1], a[2], s ? ctx_name(s) : "NULL");
			obs_source_release(s);
		} else if (!strcmp(cmd, "view_render")) {
			script_need(&sl, 2);
			obs_view_render(h_get(a[1]));
		} else if (!strcmp(cmd, "view_channels")) {
			script_need(&sl, 2);
			obs_view_t *v = h_get(a[1]);
			printf("view_channels %s:", a[1]);
			for (int i = 0; i < MAX_CHANNELS; i++)
				if (v->channels[i])
					printf(" %d=%s", i, ctx_name(v->channels[i]));
			printf("\n");
		} else if (!strcmp(cmd, "view_add")) { /* view_add V [keys]  (keys given -> add2) */
			script_need(&sl, 2);
			struct obs_video_info ovi;
			parse_ovi(&ovi, &sl, 2);
			video_t *r = sl.argc > 2 ? obs_view_add2(h_get(a[1]), &ovi) : obs_view_add(h_get(a[1]));
			printf("view_add %s -> %s mixes=%zu\n", a[1], r ? "video" : "NULL", obs->video.mixes.num);
		} else if (!strcmp(cmd, "view_add2_null")) {
			script_need(&sl, 2);
			printf("view_add2(NULL ovi) -> %s\n", obs_view_add2(h_get(a[1]), NULL) ? "video" : "NULL");
		} else if (!strcmp(cmd, "view_remove")) {
			script_need(&sl, 2);
			obs_view_remove(h_get(a[1]));
		} else if (!strcmp(cmd, "view_enum")) { /* view_enum V [stop=N] */
			script_need(&sl, 2);
			struct enum_ctx e = {0, atoi(kv(&sl, 2, "stop", "0"))};
			obs_view_enum_video_info(h_get(a[1]), vinfo_cb, &e);
			printf("  visited=%d\n", e.seen);

			/* ---- displays ---- */
		} else if (!strcmp(cmd, "display_create")) { /* display_create D CX CY BG|nogfx */
			script_need(&sl, 5);
			struct gs_init_data gd;
			memset(&gd, 0, sizeof gd);
			gd.cx = atoi(a[2]);
			gd.cy = atoi(a[3]);
			bool nogfx = !strcmp(a[4], "nogfx");
			obs_display_t *d = obs_display_create(nogfx ? NULL : &gd, nogfx ? 0 : (uint32_t)strtoul(a[4], NULL, 0));
			if (d)
				h_put(a[1], d, K_DISP);
			printf("display_create %s -> %s\n", a[1], d ? "ok" : "NULL");
		} else if (!strcmp(cmd, "display_destroy")) {
			script_need(&sl, 2);
			obs_display_destroy(h_get(a[1]));
		} else if (!strcmp(cmd, "display_resize")) {
			script_need(&sl, 4);
			obs_display_resize(h_get(a[1]), atoi(a[2]), atoi(a[3]));
		} else if (!strcmp(cmd, "display_render")) {
			script_need(&sl, 2);
			render_display(h_get(a[1]));
		} else if (!strcmp(cmd, "display_enable")) {
			script_need(&sl, 3);
			obs_display_set_enabled(h_get(a[1]), atoi(a[2]));
		} else if (!strcmp(cmd, "display_bg")) {
			script_need(&sl, 3);
			obs_display_set_background_color(h_get(a[1]), (uint32_t)strtoul(a[2], NULL, 0));
		} else if (!strcmp(cmd, "display_color_space")) {
			script_need(&sl, 2);
			obs_display_update_color_space(h_get(a[1]));
		} else if (!strcmp(cmd, "display_cb_add") || !strcmp(cmd, "display_cb_remove")) {
			script_need(&sl, 3);
			if (!strcmp(cmd, "display_cb_add"))
				obs_display_add_draw_callback(h_get(a[1]), draw_a, bstrdup(a[2]));
			else {
				/* remove matches (function, param) pairs: use the exact label string registered */
				obs_display_t *d = h_get(a[1]);
				for (size_t i = 0; d && i < d->draw_callbacks.num; i++)
					if (!strcmp(d->draw_callbacks.array[i].param, a[2])) {
						obs_display_remove_draw_callback(d, draw_a, d->draw_callbacks.array[i].param);
						break;
					}
			}
		} else if (!strcmp(cmd, "display_info")) {
			script_need(&sl, 2);
			obs_display_t *d = h_get(a[1]);
			uint32_t w = 0xdead, h = 0xbeef;
			obs_display_size(d, &w, &h);
			printf("display_info %s enabled=%d size=%ux%u", a[1], obs_display_enabled(d), w, h);
			if (d)
				printf(" next=%ux%u bg=0x%x update_cs=%d callbacks=%zu swap=%s in_list=%d",
				       d->next_cx, d->next_cy, d->background_color, d->update_color_space,
				       d->draw_callbacks.num, d->swap ? "yes" : "no",
				       obs->data.first_display == d || d->prev_next != NULL);
			printf("\n");
		} else if (!strcmp(cmd, "display_list")) {
			printf("display_list:");
			for (struct obs_display *d = obs->data.first_display; d; d = d->next)
				printf(" %s", h_name(d));
			printf("\n");
		} else if (!strcmp(cmd, "display_null")) { /* NULL display handling */
			uint32_t w = 1, h = 1;
			obs_display_resize(NULL, 1, 1);
			obs_display_update_color_space(NULL);
			obs_display_add_draw_callback(NULL, draw_a, NULL);
			obs_display_remove_draw_callback(NULL, draw_a, NULL);
			obs_display_set_enabled(NULL, true);
			obs_display_set_background_color(NULL, 1);
			obs_display_size(NULL, &w, &h);
			render_display(NULL);
			obs_display_destroy(NULL);
			printf("display_null enabled=%d size=%ux%u\n", obs_display_enabled(NULL), w, h);

			/* ---- callbacks and tasks ---- */
		} else if (!strcmp(cmd, "tick_add")) { /* tick_add a|b */
			script_need(&sl, 2);
			obs_add_tick_callback(!strcmp(a[1], "a") ? tick_a : tick_b, NULL);
		} else if (!strcmp(cmd, "tick_remove")) {
			script_need(&sl, 2);
			obs_remove_tick_callback(!strcmp(a[1], "a") ? tick_a : tick_b, NULL);
		} else if (!strcmp(cmd, "render_cb_add")) { /* render_cb_add LABEL */
			script_need(&sl, 2);
			obs_add_main_render_callback(draw_a, bstrdup(a[1]));
		} else if (!strcmp(cmd, "render_cb_remove")) { /* remove by label (finds the param) */
			script_need(&sl, 2);
			for (size_t i = 0; i < obs->data.draw_callbacks.num; i++)
				if (!strcmp(obs->data.draw_callbacks.array[i].param, a[1])) {
					obs_remove_main_render_callback(draw_a, obs->data.draw_callbacks.array[i].param);
					break;
				}
		} else if (!strcmp(cmd, "rendered_cb")) { /* rendered_cb add|remove */
			script_need(&sl, 2);
			if (!strcmp(a[1], "add"))
				obs_add_main_rendered_callback(rendered_a, NULL);
			else
				obs_remove_main_rendered_callback(rendered_a, NULL);
		} else if (!strcmp(cmd, "callbacks")) {
			printf("callbacks tick=[");
			for (size_t i = 0; i < obs->data.tick_callbacks.num; i++)
				printf("%s%s", i ? "," : "", obs->data.tick_callbacks.array[i].tick == tick_a ? "a" : "b");
			printf("] draw=[");
			for (size_t i = 0; i < obs->data.draw_callbacks.num; i++)
				printf("%s%s", i ? "," : "", (const char *)obs->data.draw_callbacks.array[i].param);
			printf("] rendered=%zu\n", obs->data.rendered_callbacks.num);
		} else if (!strcmp(cmd, "ui_handler")) { /* ui_handler on|off */
			script_need(&sl, 2);
			obs_set_ui_task_handler(!strcmp(a[1], "on") ? ui_handler : NULL);
			printf("ui_handler %s\n", a[1]);
		} else if (!strcmp(cmd, "queue_task")) { /* queue_task graphics|audio|ui|destroy wait=0|1 LABEL */
			script_need(&sl, 4);
			enum obs_task_type t = !strcmp(a[1], "graphics") ? OBS_TASK_GRAPHICS
					       : !strcmp(a[1], "audio")  ? OBS_TASK_AUDIO
					       : !strcmp(a[1], "ui")	 ? OBS_TASK_UI
								 : OBS_TASK_DESTROY;
			if (t == OBS_TASK_DESTROY)
				obs_queue_task(t, (obs_task_t)task_cb, a[3], atoi(a[2]));
			else
				obs_queue_task(t, task_cb, a[3], atoi(a[2]));
		} else if (!strcmp(cmd, "wait_destroy")) {
			printf("wait_destroy -> %d\n", obs_wait_for_destroy_queue());
		} else if (!strcmp(cmd, "in_task_thread")) {
			printf("in_task_thread graphics=%d audio=%d ui=%d\n", obs_in_task_thread(OBS_TASK_GRAPHICS),
			       obs_in_task_thread(OBS_TASK_AUDIO), obs_in_task_thread(OBS_TASK_UI));
		} else if (!strcmp(cmd, "base_effect")) {
			printf("base_effect default=%d rect=%d opaque=%d solid=%d repeat=%d bicubic=%d lanczos=%d area=%d "
			       "lowres=%d premult=%d bad=%d\n",
			       obs_get_base_effect(OBS_EFFECT_DEFAULT) != NULL, obs_get_base_effect(OBS_EFFECT_DEFAULT_RECT) != NULL,
			       obs_get_base_effect(OBS_EFFECT_OPAQUE) != NULL, obs_get_base_effect(OBS_EFFECT_SOLID) != NULL,
			       obs_get_base_effect(OBS_EFFECT_REPEAT) != NULL, obs_get_base_effect(OBS_EFFECT_BICUBIC) != NULL,
			       obs_get_base_effect(OBS_EFFECT_LANCZOS) != NULL, obs_get_base_effect(OBS_EFFECT_AREA) != NULL,
			       obs_get_base_effect(OBS_EFFECT_BILINEAR_LOWRES) != NULL,
			       obs_get_base_effect(OBS_EFFECT_PREMULTIPLIED_ALPHA) != NULL, obs_get_base_effect(99) != NULL);
		} else if (!strcmp(cmd, "private_data")) { /* private_data set KEY VAL | get KEY */
			script_need(&sl, 3);
			if (!strcmp(a[1], "apply")) {
				obs_data_t *d = obs_data_create();
				obs_data_set_string(d, a[2], a[3]);
				obs_apply_private_data(d);
				obs_data_release(d);
			} else if (!strcmp(a[1], "set")) {
				obs_data_t *d = obs_data_create();
				obs_data_set_string(d, a[2], a[3]);
				obs_set_private_data(d);
				obs_data_release(d);
			} else {
				obs_data_t *d = obs_get_private_data();
				printf("private_data %s -> \"%s\"\n", a[2], obs_data_get_string(d, a[2]));
				obs_data_release(d);
			}
		} else {
			die("unknown command", cmd);
		}
	}
	(void)g_prev_private;
	fclose(f);
	return 0;
}
