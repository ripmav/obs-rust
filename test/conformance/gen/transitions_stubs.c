/*
 * Conformance harness (area transitions, spec S-029): a recording fake of the libobs core and of the
 * graphics API, as seen by the transitions plugin (plugins/obs-transitions).
 *
 * The real plugin sources are compiled into transitions_runner.c.  Everything the plugin calls that is not
 * plugin code is replaced here by a fake that prints one "  ev ..." line per call, so the order and the
 * arguments of every graphics/core call are part of the expected output.  The real libobs/obs-data.c,
 * obs-properties.c, obs-missing-files.c and callback/calldata.c are linked.
 *
 * Every scripted accessor of the fake world (sizes, color space, media durations, audio timestamps, ...)
 * is an INPUT documented in spec S-029 ("fake world").  The fake core helpers (obs_transition_video_render
 * and friends) model the documented contract of the real helpers in libobs/obs-source-transition.c; they
 * are not part of the specified component.
 *
 * Test tooling, not part of libobs.
 */
#include <dirent.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "obs.h"
#include "obs-module.h"
#include "obs-missing-files.h"
#include "graphics/image-file.h"
#include "graphics/vec2.h"
#include "graphics/vec4.h"
#include "util/bmem.h"
#include "util/base.h"
#include "util/dstr.h"
#include "util/platform.h"
#include "util/text-lookup.h"
#include "callback/calldata.h"
#include "callback/proc.h"
#include "script.h"

/* ------------------------------------------------------------------------- */
/* data directory: derived from the location of this file so that it is independent of the cwd */

static char data_dir[1024];

static const char *get_data_dir(void)
{
	if (!data_dir[0]) {
		snprintf(data_dir, sizeof(data_dir), "%s", __FILE__);
		char *p = strrchr(data_dir, '/');
		if (p)
			*p = 0; /* .../test/conformance/gen */
		size_t n = strlen(data_dir);
		snprintf(data_dir + n, sizeof(data_dir) - n, "/../../../plugins/obs-transitions/data");
	}
	return data_dir;
}

/* path -> printable ("@data/<rest>" for paths below the data directory) */
static const char *printable(const char *path, char *buf, size_t size)
{
	if (!path)
		return "(null)";
	const char *d = get_data_dir();
	size_t n = strlen(d);
	if (!strncmp(path, d, n) && path[n] == '/') {
		snprintf(buf, size, "@data%s", path + n);
		return buf;
	}
	return path;
}

/* ------------------------------------------------------------------------- */
/* fake world */

struct fake_world {
	enum gs_color_space gfx_space; /* gs_get_color_space() */
	bool fb_srgb;                  /* gs_framebuffer_srgb_enabled() */
	bool linear_srgb;              /* gs_get_linear_srgb() */
	bool effect_fail;              /* gs_effect_create_from_file() fails */
	float sdr_white;               /* obs_get_video_sdr_white_level() */
	bool texrender_begin_ok;       /* gs_texrender_begin_with_color_space() result */
	bool direct_ret;               /* obs_transition_video_render_direct() result */
	bool audio_ret;                /* obs_transition_audio_render() result */
	uint64_t audio_ts_out;         /* *ts_out written by obs_transition_audio_render */
	float audio_base;              /* sample value written by obs_transition_audio_render */
	enum gs_color_space tr_space;  /* obs_transition_video_get_color_space() */
	char missing[4][64];           /* module files that "do not exist" */
	int num_missing;
	char exists[8][160]; /* os_file_exists() true for these */
	int num_exists;
	/* video render helper inputs */
	uintptr_t tex_a, tex_b, placeholder;
	float t;
	uint32_t cx, cy;
};

struct media_defaults {
	uint32_t w, h;
	bool active;
	int state;
	bool audio_pending;
	uint64_t audio_ts;
	int space;
	int64_t dur_ns, frames;
	float child_fill;
};

struct obs_source {
	int ordinal; /* 0 = a transition made by the runner, >0 = private source in creation order */
	char name[160];
	struct obs_source_info info;
	void *data;
	obs_data_t *settings;
	int defer;
	int refs;
	int active_child_refs;
	uint32_t w, h;
	bool active;
	int state;
	bool audio_pending;
	uint64_t audio_ts;
	int space;
	int64_t dur_ns, frames;
	float child_fill;
	bool fixed;
	uint32_t fixed_ms;
	float time;
	bool muted, monitoring;
	char id[32];
};

static struct fake_world W;
static struct media_defaults M;
static struct obs_source *sources[64];
static int num_sources;
static int num_private;
static int next_texrender;
static int next_image;
static struct obs_source_info registry[16];
static int num_registry;

struct fake_effect {
	char file[160];
	int loop_state; /* 0 idle, 1 in loop */
};
struct fake_param {
	struct fake_effect *effect;
	char name[48];
};
struct fake_texrender {
	int id;
	enum gs_color_format format;
};

/* %.9g with a canonical spelling for NaN/inf (glibc prints "-nan" for NaNs with the sign bit set) */
const char *fk_f9(double v)
{
	static char ring[8][32];
	static int idx;
	char *b = ring[idx++ & 7];
	if (v != v)
		return "nan";
	snprintf(b, 32, "%.9g", v);
	return b;
}

static void ev(const char *fmt, ...)
{
	va_list ap;
	fputs("  ev ", stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

static void tex_str(char *buf, size_t n, const void *t)
{
	if (!t)
		snprintf(buf, n, "null");
	else
		snprintf(buf, n, "%lu", (unsigned long)(uintptr_t)t);
}

void fk_reset(void)
{
	memset(&W, 0, sizeof(W));
	W.gfx_space = GS_CS_SRGB;
	W.sdr_white = 300.0f;
	W.texrender_begin_ok = true;
	W.direct_ret = true;
	W.audio_ret = true;
	W.tr_space = GS_CS_SRGB;
	W.tex_a = 1;
	W.tex_b = 2;
	W.placeholder = 9000;
	W.t = 0.5f;
	W.cx = 1920;
	W.cy = 1080;
	memset(&M, 0, sizeof(M));
	M.active = true;
	M.audio_ts = 1000;
	M.child_fill = 0.25f;
	for (int i = 0; i < num_sources; i++) {
		obs_data_release(sources[i]->settings);
		bfree(sources[i]);
		sources[i] = NULL;
	}
	num_sources = 0;
	num_private = 0;
	num_registry = 0;
	next_texrender = 0;
	next_image = 0;
}

/* ------------------------------------------------------------------------- */
/* host command interface used by the runner */

static enum gs_color_space parse_space(const char *s)
{
	if (!strcmp(s, "srgb"))
		return GS_CS_SRGB;
	if (!strcmp(s, "srgb_16f"))
		return GS_CS_SRGB_16F;
	if (!strcmp(s, "709_extended"))
		return GS_CS_709_EXTENDED;
	if (!strcmp(s, "709_scrgb"))
		return GS_CS_709_SCRGB;
	return (enum gs_color_space)atoi(s);
}

/* tex ids: a number, or "-" for NULL */
static uintptr_t parse_tex(const char *s)
{
	return strcmp(s, "-") ? (uintptr_t)strtoull(s, NULL, 0) : 0;
}

static struct obs_source *find_source(const char *name)
{
	if (!strncmp(name, "media#", 6)) {
		int k = atoi(name + 6);
		for (int i = 0; i < num_sources; i++)
			if (sources[i]->ordinal == k)
				return sources[i];
		return NULL;
	}
	for (int i = 0; i < num_sources; i++)
		if (!sources[i]->ordinal && !strcmp(sources[i]->name, name))
			return sources[i];
	return NULL;
}

/* key=value parser for the generic "gfx", "media", "mdef", "src" commands */
static void apply_kv(struct fake_world *w, struct media_defaults *md, struct obs_source *src, const char *kv)
{
	char key[48];
	const char *eq = strchr(kv, '=');
	if (!eq || (size_t)(eq - kv) >= sizeof(key)) {
		fprintf(stderr, "script: bad key=value '%s'\n", kv);
		exit(2);
	}
	memcpy(key, kv, (size_t)(eq - kv));
	key[eq - kv] = 0;
	const char *v = eq + 1;
	double d = strtod(v, NULL);
	long long i = strtoll(v, NULL, 0);
	bool found = true;

	if (w) {
		if (!strcmp(key, "space"))
			w->gfx_space = parse_space(v);
		else if (!strcmp(key, "fb_srgb"))
			w->fb_srgb = i != 0;
		else if (!strcmp(key, "linear"))
			w->linear_srgb = i != 0;
		else if (!strcmp(key, "effect_fail"))
			w->effect_fail = i != 0;
		else if (!strcmp(key, "white"))
			w->sdr_white = (float)d;
		else if (!strcmp(key, "begin_ok"))
			w->texrender_begin_ok = i != 0;
		else if (!strcmp(key, "direct_ret"))
			w->direct_ret = i != 0;
		else if (!strcmp(key, "audio_ret"))
			w->audio_ret = i != 0;
		else if (!strcmp(key, "ts_out"))
			w->audio_ts_out = (uint64_t)strtoull(v, NULL, 0);
		else if (!strcmp(key, "base"))
			w->audio_base = (float)d;
		else if (!strcmp(key, "tr_space"))
			w->tr_space = parse_space(v);
		else if (!strcmp(key, "placeholder"))
			w->placeholder = parse_tex(v);
		else
			found = false;
		if (found)
			return;
		found = true;
	}
	if (md) {
		if (!strcmp(key, "w"))
			md->w = (uint32_t)i;
		else if (!strcmp(key, "h"))
			md->h = (uint32_t)i;
		else if (!strcmp(key, "active"))
			md->active = i != 0;
		else if (!strcmp(key, "state"))
			md->state = (int)i;
		else if (!strcmp(key, "pending"))
			md->audio_pending = i != 0;
		else if (!strcmp(key, "ats"))
			md->audio_ts = (uint64_t)strtoull(v, NULL, 0);
		else if (!strcmp(key, "space"))
			md->space = (int)parse_space(v);
		else if (!strcmp(key, "dur_ns"))
			md->dur_ns = i;
		else if (!strcmp(key, "frames"))
			md->frames = i;
		else if (!strcmp(key, "fill"))
			md->child_fill = (float)d;
		else
			found = false;
		if (found)
			return;
		found = true;
	}
	if (src) {
		if (!strcmp(key, "w"))
			src->w = (uint32_t)i;
		else if (!strcmp(key, "h"))
			src->h = (uint32_t)i;
		else if (!strcmp(key, "active"))
			src->active = i != 0;
		else if (!strcmp(key, "state"))
			src->state = (int)i;
		else if (!strcmp(key, "pending"))
			src->audio_pending = i != 0;
		else if (!strcmp(key, "ats"))
			src->audio_ts = (uint64_t)strtoull(v, NULL, 0);
		else if (!strcmp(key, "space"))
			src->space = (int)parse_space(v);
		else if (!strcmp(key, "dur_ns"))
			src->dur_ns = i;
		else if (!strcmp(key, "frames"))
			src->frames = i;
		else if (!strcmp(key, "fill"))
			src->child_fill = (float)d;
		else if (!strcmp(key, "time"))
			src->time = (float)d;
		else
			found = false;
		if (found)
			return;
	}
	fprintf(stderr, "script: unknown key '%s'\n", key);
	exit(2);
}

/* Handles the commands that only configure the fake world. Returns false if sl is not one of them. */
bool fk_command(const struct script_line *sl)
{
	const char *c = sl->argv[0];
	if (!strcmp(c, "gfx")) { /* gfx key=value... : graphics/core scripted state */
		for (int i = 1; i < sl->argc; i++)
			apply_kv(&W, NULL, NULL, sl->argv[i]);
		return true;
	}
	if (!strcmp(c, "mdef")) { /* mdef key=value... : defaults for private sources created later */
		for (int i = 1; i < sl->argc; i++)
			apply_kv(NULL, &M, NULL, sl->argv[i]);
		return true;
	}
	if (!strcmp(c, "src")) { /* src NAME key=value... : NAME is a transition name or media#K */
		script_need(sl, 2);
		struct obs_source *s = find_source(sl->argv[1]);
		if (!s) {
			printf("src %s missing\n", sl->argv[1]);
			return true;
		}
		for (int i = 2; i < sl->argc; i++)
			apply_kv(NULL, NULL, s, sl->argv[i]);
		return true;
	}
	if (!strcmp(c, "view")) { /* view T A B CX CY [PLACEHOLDER] : inputs of the fake video render helper */
		script_need(sl, 6);
		W.t = (float)atof(sl->argv[1]);
		W.tex_a = parse_tex(sl->argv[2]);
		W.tex_b = parse_tex(sl->argv[3]);
		W.cx = (uint32_t)atoi(sl->argv[4]);
		W.cy = (uint32_t)atoi(sl->argv[5]);
		return true;
	}
	if (!strcmp(c, "missing_file")) { /* module file that obs_module_file() cannot find */
		script_need(sl, 2);
		if (W.num_missing < 4)
			snprintf(W.missing[W.num_missing++], 64, "%s", sl->argv[1]);
		return true;
	}
	if (!strcmp(c, "exists")) { /* os_file_exists(PATH) answers true */
		script_need(sl, 2);
		if (W.num_exists < 8)
			snprintf(W.exists[W.num_exists++], 160, "%s", sl->argv[1]);
		return true;
	}
	return false;
}

/* ------------------------------------------------------------------------- */
/* sources: creation sequence of the real core (obs_source_create_internal) */

const struct obs_source_info *fk_registry_find(const char *id)
{
	for (int i = 0; i < num_registry; i++)
		if (!strcmp(registry[i].id, id))
			return registry + i;
	return NULL;
}

int fk_registry_count(void)
{
	return num_registry;
}

const struct obs_source_info *fk_registry_at(int i)
{
	return registry + i;
}

/* Models obs_source_create(): settings object (user settings applied, then the default layer from
 * get_defaults), then info.create(settings, source). */
obs_source_t *fk_create(const struct obs_source_info *info, const char *name, obs_data_t *user)
{
	struct obs_source *s = bzalloc(sizeof(*s));
	snprintf(s->name, sizeof(s->name), "%s", name);
	s->info = *info;
	s->refs = 1;
	s->settings = obs_data_create();
	if (user)
		obs_data_apply(s->settings, user);
	if (info->get_defaults)
		info->get_defaults(s->settings);
	sources[num_sources++] = s;
	s->data = info->create(s->settings, s);
	return s;
}

void *fk_data(obs_source_t *s)
{
	return s->data;
}

const struct obs_source_info *fk_info(obs_source_t *s)
{
	return &s->info;
}

obs_data_t *fk_settings(obs_source_t *s)
{
	return s->settings;
}

int fk_defer_count(obs_source_t *s)
{
	return s->defer;
}

obs_source_t *fk_find(const char *name)
{
	return find_source(name);
}

/* Models obs_source_video_tick(): deferred update, then the transition's own video_tick. */
void fk_tick(obs_source_t *s, float seconds)
{
	if (s->defer > 0 && s->data && s->info.update) {
		s->info.update(s->data, s->settings);
		s->defer = 0;
	}
	if (s->info.video_tick && s->data)
		s->info.video_tick(s->data, seconds);
}

void fk_destroy(obs_source_t *s)
{
	if (s->info.destroy && s->data)
		s->info.destroy(s->data);
	s->data = NULL;
}

void fk_print_sources(void)
{
	for (int i = 0; i < num_sources; i++) {
		struct obs_source *s = sources[i];
		if (!s->ordinal)
			continue;
		printf("  source media#%d name=\"%s\" id=%s refs=%d active_child_refs=%d muted=%d monitoring=%d\n",
		       s->ordinal, s->name, s->id, s->refs, s->active_child_refs, (int)s->muted,
		       (int)s->monitoring);
	}
}

/* Audio hooks for the runner. */
float fk_audio_base(void)
{
	return W.audio_base;
}

/* ------------------------------------------------------------------------- */
/* module / platform */

bool text_lookup_getstr(lookup_t *lookup, const char *lookup_val, const char **out)
{
	(void)lookup, (void)lookup_val, (void)out;
	return false; /* identity: obs_module_text(KEY) returns KEY */
}

void text_lookup_destroy(lookup_t *lookup)
{
	(void)lookup;
}

lookup_t *obs_module_load_locale(obs_module_t *module, const char *default_locale, const char *locale)
{
	(void)module, (void)default_locale, (void)locale;
	return NULL;
}

char *obs_find_module_file(obs_module_t *module, const char *file)
{
	(void)module;
	if (!file)
		file = "";
	for (int i = 0; i < W.num_missing; i++)
		if (!strcmp(W.missing[i], file))
			return NULL;
	struct dstr out = {0};
	dstr_copy(&out, get_data_dir());
	dstr_cat_ch(&out, '/');
	dstr_cat(&out, file);
	struct stat st;
	if (stat(out.array, &st) != 0)
		dstr_free(&out);
	return out.array;
}

char *os_quick_read_utf8_file(const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = bmalloc((size_t)n + 1);
	size_t got = fread(buf, 1, (size_t)n, f);
	buf[got] = 0;
	fclose(f);
	return buf;
}

bool os_quick_write_utf8_file(const char *path, const char *str, size_t len, bool marker)
{
	(void)path, (void)str, (void)len, (void)marker;
	abort();
}

bool os_quick_write_utf8_file_safe(const char *path, const char *str, size_t len, bool marker, const char *temp_ext,
				   const char *backup_ext)
{
	(void)path, (void)str, (void)len, (void)marker, (void)temp_ext, (void)backup_ext;
	abort();
}

int os_rename(const char *old_path, const char *new_path)
{
	return rename(old_path, new_path);
}

/* ------------------------------------------------------------------------- */
/* registration */

void obs_register_source_s(const struct obs_source_info *info, size_t size)
{
	struct obs_source_info data = {0};
	if (size > sizeof(data) || num_registry >= 16) {
		printf("register_failed\n");
		return;
	}
	memcpy(&data, info, size);
	if (data.type == OBS_SOURCE_TYPE_TRANSITION) {
		if (data.get_width)
			blog(LOG_WARNING, "get_width ignored registering transition");
		if (data.get_height)
			blog(LOG_WARNING, "get_height ignored registering transition");
		data.output_flags |= OBS_SOURCE_COMPOSITE | OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW;
	}
	registry[num_registry++] = data;
}

/* ------------------------------------------------------------------------- */
/* graphics */

void obs_enter_graphics(void)
{
	ev("enter_graphics");
}

void obs_leave_graphics(void)
{
	ev("leave_graphics");
}

/* A parameter exists only if the effect file declares a global "uniform <type> <name>;". */
static bool effect_declares(const char *file, const char *name)
{
	char *text = os_quick_read_utf8_file(file);
	bool found = false;
	if (!text)
		return false;
	size_t nl = strlen(name);
	for (const char *p = text; (p = strstr(p, "uniform ")) != NULL; p++) {
		const char *e = strchr(p, ';');
		if (!e)
			break;
		const char *q = e;
		while (q > p && (*(q - 1) == ' ' || *(q - 1) == '\t'))
			q--;
		const char *b = q;
		while (b > p && *(b - 1) != ' ' && *(b - 1) != '\t')
			b--;
		if ((size_t)(q - b) == nl && !strncmp(b, name, nl)) {
			found = true;
			break;
		}
	}
	bfree(text);
	return found;
}

gs_effect_t *gs_effect_create_from_file(const char *file, char **error_string)
{
	char pb[300];
	ev("effect_create file=%s", printable(file, pb, sizeof(pb)));
	if (error_string)
		*error_string = NULL;
	if (!file || W.effect_fail) {
		if (error_string)
			*error_string = bstrdup("fake: effect creation failed");
		ev("  -> fail");
		return NULL;
	}
	struct fake_effect *e = bzalloc(sizeof(*e));
	snprintf(e->file, sizeof(e->file), "%s", file);
	return (gs_effect_t *)e;
}

void gs_effect_destroy(gs_effect_t *effect)
{
	struct fake_effect *e = (struct fake_effect *)effect;
	char pb[300];
	if (!e)
		return;
	ev("effect_destroy file=%s", printable(e->file, pb, sizeof(pb)));
	bfree(e);
}

gs_eparam_t *gs_effect_get_param_by_name(const gs_effect_t *effect, const char *name)
{
	struct fake_effect *e = (struct fake_effect *)effect;
	if (!e)
		return NULL;
	/* the base effect (default.effect) is not a plugin asset: its parameters always exist */
	bool base = !strcmp(e->file, "<base:default>");
	if (!base && !effect_declares(e->file, name)) {
		ev("get_param %s -> null", name);
		return NULL;
	}
	struct fake_param *p = bzalloc(sizeof(*p));
	p->effect = e;
	snprintf(p->name, sizeof(p->name), "%s", name);
	return (gs_eparam_t *)p;
}

static const char *pname(const gs_eparam_t *p)
{
	return p ? ((const struct fake_param *)p)->name : "(null-param)";
}

void gs_effect_set_texture(gs_eparam_t *param, gs_texture_t *tex)
{
	char t[32];
	tex_str(t, sizeof(t), tex);
	ev("set_texture %s tex=%s srgb=0", pname(param), t);
}

void gs_effect_set_texture_srgb(gs_eparam_t *param, gs_texture_t *tex)
{
	char t[32];
	tex_str(t, sizeof(t), tex);
	ev("set_texture %s tex=%s srgb=1", pname(param), t);
}

void gs_effect_set_float(gs_eparam_t *param, float val)
{
	ev("set_float %s %.9g", pname(param), (double)val);
}

void gs_effect_set_bool(gs_eparam_t *param, bool val)
{
	ev("set_bool %s %d", pname(param), (int)val);
}

void gs_effect_set_vec2(gs_eparam_t *param, const struct vec2 *val)
{
	ev("set_vec2 %s %.9g %.9g", pname(param), (double)val->x, (double)val->y);
}

void gs_effect_set_vec4(gs_eparam_t *param, const struct vec4 *val)
{
	ev("set_vec4 %s %.9g %.9g %.9g %.9g", pname(param), (double)val->x, (double)val->y, (double)val->z,
	   (double)val->w);
}

bool gs_effect_loop(gs_effect_t *effect, const char *name)
{
	struct fake_effect *e = (struct fake_effect *)effect;
	if (!e->loop_state) {
		e->loop_state = 1;
		ev("effect_loop technique=%s pass=0", name);
		return true;
	}
	e->loop_state = 0;
	return false;
}

void gs_draw_sprite(gs_texture_t *tex, uint32_t flip, uint32_t width, uint32_t height)
{
	char t[32];
	tex_str(t, sizeof(t), tex);
	ev("draw_sprite tex=%s flip=%u w=%u h=%u", t, flip, width, height);
}

bool gs_framebuffer_srgb_enabled(void)
{
	return W.fb_srgb;
}

void gs_enable_framebuffer_srgb(bool enable)
{
	ev("enable_framebuffer_srgb %d", (int)enable);
	W.fb_srgb = enable;
}

enum gs_color_space gs_get_color_space(void)
{
	return W.gfx_space;
}

bool gs_get_linear_srgb(void)
{
	return W.linear_srgb;
}

bool gs_set_linear_srgb(bool linear)
{
	bool prev = W.linear_srgb;
	ev("set_linear_srgb %d (previous %d)", (int)linear, (int)prev);
	W.linear_srgb = linear;
	return prev;
}

void gs_matrix_push(void)
{
	ev("matrix_push");
}

void gs_matrix_pop(void)
{
	ev("matrix_pop");
}

void gs_matrix_scale3f(float x, float y, float z)
{
	ev("matrix_scale3f %.9g %.9g %.9g", (double)x, (double)y, (double)z);
}

void gs_matrix_translate3f(float x, float y, float z)
{
	ev("matrix_translate3f %.9g %.9g %.9g", (double)x, (double)y, (double)z);
}

void gs_clear(uint32_t clear_flags, const struct vec4 *color, float depth, uint8_t stencil)
{
	ev("clear flags=%u color=%.9g,%.9g,%.9g,%.9g depth=%.9g stencil=%u", clear_flags, (double)color->x,
	   (double)color->y, (double)color->z, (double)color->w, (double)depth, stencil);
}

void gs_ortho(float left, float right, float top, float bottom, float znear, float zfar)
{
	ev("ortho l=%.9g r=%.9g t=%.9g b=%.9g near=%.9g far=%.9g", (double)left, (double)right, (double)top,
	   (double)bottom, (double)znear, (double)zfar);
}

void gs_blend_state_push(void)
{
	ev("blend_state_push");
}

void gs_blend_state_pop(void)
{
	ev("blend_state_pop");
}

void gs_enable_blending(bool enable)
{
	ev("enable_blending %d", (int)enable);
}

gs_texrender_t *gs_texrender_create(enum gs_color_format format, enum gs_zstencil_format zsformat)
{
	struct fake_texrender *t = bzalloc(sizeof(*t));
	t->id = ++next_texrender;
	t->format = format;
	ev("texrender_create id=%d format=%d zs=%d", t->id, (int)format, (int)zsformat);
	return (gs_texrender_t *)t;
}

void gs_texrender_destroy(gs_texrender_t *texrender)
{
	struct fake_texrender *t = (struct fake_texrender *)texrender;
	if (!t)
		return;
	ev("texrender_destroy id=%d", t->id);
	bfree(t);
}

bool gs_texrender_begin_with_color_space(gs_texrender_t *texrender, uint32_t cx, uint32_t cy,
					 enum gs_color_space space)
{
	struct fake_texrender *t = (struct fake_texrender *)texrender;
	ev("texrender_begin id=%d cx=%u cy=%u space=%d -> %d", t ? t->id : -1, cx, cy, (int)space,
	   (int)W.texrender_begin_ok);
	return W.texrender_begin_ok;
}

void gs_texrender_end(gs_texrender_t *texrender)
{
	struct fake_texrender *t = (struct fake_texrender *)texrender;
	ev("texrender_end id=%d", t ? t->id : -1);
}

void gs_texrender_reset(gs_texrender_t *texrender)
{
	struct fake_texrender *t = (struct fake_texrender *)texrender;
	if (!t) {
		ev("texrender_reset null");
		return;
	}
	ev("texrender_reset id=%d", t->id);
}

gs_texture_t *gs_texrender_get_texture(const gs_texrender_t *texrender)
{
	const struct fake_texrender *t = (const struct fake_texrender *)texrender;
	return t ? (gs_texture_t *)(uintptr_t)(100 + t->id) : NULL;
}

enum gs_color_format gs_texrender_get_format(const gs_texrender_t *texrender)
{
	const struct fake_texrender *t = (const struct fake_texrender *)texrender;
	return t ? t->format : GS_UNKNOWN;
}

/* --- image files (luma wipe): the PNG header is parsed, no pixel data is decoded --- */

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/* Reads width/height/bit depth/color type from a PNG IHDR. Returns false for anything else. */
bool fk_png_info(const char *path, uint32_t *w, uint32_t *h, int *depth, int *ctype, uint64_t *fnv, size_t *len)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return false;
	uint8_t hdr[33];
	size_t got = fread(hdr, 1, sizeof(hdr), f);
	bool ok = got == sizeof(hdr) && !memcmp(hdr, "\x89PNG\r\n\x1a\n", 8) && !memcmp(hdr + 12, "IHDR", 4);
	if (ok) {
		*w = be32(hdr + 16);
		*h = be32(hdr + 20);
		*depth = hdr[24];
		*ctype = hdr[25];
	}
	uint64_t hash = 0xcbf29ce484222325ULL;
	size_t n = 0;
	for (size_t i = 0; i < got; i++) {
		hash ^= hdr[i];
		hash *= 1099511628211ULL;
	}
	n = got;
	int c;
	while ((c = fgetc(f)) != EOF) {
		hash ^= (uint8_t)c;
		hash *= 1099511628211ULL;
		n++;
	}
	fclose(f);
	if (fnv)
		*fnv = hash;
	if (len)
		*len = n;
	return ok;
}

void gs_image_file_ex_init(gs_image_file_ex_t *image, const char *file, enum gs_image_alpha_mode alpha_mode)
{
	char pb[300];
	int depth, ctype;
	if (!image)
		return;
	memset(image, 0, sizeof(*image));
	ev("image_init file=%s alpha_mode=%d", printable(file, pb, sizeof(pb)), (int)alpha_mode);
	if (!file)
		return;
	image->alpha_mode = alpha_mode;
	if (fk_png_info(file, &image->cx, &image->cy, &depth, &ctype, NULL, NULL)) {
		image->loaded = true;
		image->format = GS_RGBA;
		ev("  -> loaded %ux%u depth=%d color_type=%d", image->cx, image->cy, depth, ctype);
	} else {
		blog(LOG_WARNING, "Failed to load file '%s'", pb);
		ev("  -> failed");
		memset(image, 0, sizeof(*image));
	}
}

void gs_image_file_ex_free(gs_image_file_ex_t *image)
{
	if (!image)
		return;
	ev("image_free loaded=%d", (int)image->loaded);
	memset(image, 0, sizeof(*image));
}

void gs_image_file_ex_init_texture(gs_image_file_ex_t *image)
{
	if (!image->loaded) {
		ev("image_init_texture (not loaded)");
		return;
	}
	image->texture = (gs_texture_t *)(uintptr_t)(300 + ++next_image);
	ev("image_init_texture -> texture %lu", (unsigned long)(uintptr_t)image->texture);
}

/* ------------------------------------------------------------------------- */
/* core: sources */

static const char *srcname(const obs_source_t *s)
{
	static char ring[4][160];
	static int idx;
	const struct obs_source *o = (const struct obs_source *)s;
	char *b = ring[idx++ & 3];
	if (!o)
		return "(null)";
	if (o->ordinal)
		snprintf(b, 160, "media#%d", o->ordinal);
	else
		snprintf(b, 160, "%s", o->name);
	return b;
}

obs_source_t *obs_source_create_private(const char *id, const char *name, obs_data_t *settings)
{
	struct obs_source *s = bzalloc(sizeof(*s));
	s->ordinal = ++num_private;
	snprintf(s->name, sizeof(s->name), "%s", name ? name : "(null)");
	snprintf(s->id, sizeof(s->id), "%s", id);
	s->refs = 1;
	s->settings = settings ? obs_data_create() : NULL;
	if (settings)
		obs_data_apply(s->settings, settings);
	s->w = M.w;
	s->h = M.h;
	s->active = M.active;
	s->state = M.state;
	s->audio_pending = M.audio_pending;
	s->audio_ts = M.audio_ts;
	s->space = M.space;
	s->dur_ns = M.dur_ns;
	s->frames = M.frames;
	s->child_fill = M.child_fill;
	sources[num_sources++] = s;
	ev("source_create_private media#%d id=%s name=%s%s%s", s->ordinal, id, name ? "\"" : "", name ? name : "(null)",
	   name ? "\"" : "");
	if (settings) {
		const char *keys[] = {"local_file", "hw_decode", "looping", "full_decode", "is_stinger", "is_track_matte"};
		for (int i = 0; i < 6; i++) {
			if (!obs_data_has_user_value(settings, keys[i]))
				continue;
			if (i == 0)
				ev("  setting local_file=\"%s\"", obs_data_get_string(settings, keys[i]));
			else
				ev("  setting %s=%d", keys[i], (int)obs_data_get_bool(settings, keys[i]));
		}
	}
	return s;
}

void obs_source_release(obs_source_t *source)
{
	struct obs_source *s = (struct obs_source *)source;
	if (!s)
		return;
	s->refs--;
	ev("source_release %s refs=%d", srcname(source), s->refs);
}

const char *obs_source_get_name(const obs_source_t *source)
{
	return source ? ((const struct obs_source *)source)->name : NULL;
}

obs_data_t *obs_source_get_settings(const obs_source_t *source)
{
	struct obs_source *s = (struct obs_source *)source;
	obs_data_addref(s->settings);
	return s->settings;
}

void obs_source_update(obs_source_t *source, obs_data_t *settings)
{
	struct obs_source *s = (struct obs_source *)source;
	if (!s)
		return;
	if (settings)
		obs_data_apply(s->settings, settings);
	/* transitions carry OBS_SOURCE_VIDEO: the update is deferred to the next video tick */
	if (s->info.output_flags & OBS_SOURCE_VIDEO) {
		s->defer++;
		ev("source_update %s deferred (count %d)", s->name, s->defer);
	} else if (s->data && s->info.update) {
		s->info.update(s->data, s->settings);
	}
}

uint32_t obs_source_get_width(obs_source_t *source)
{
	return source ? ((struct obs_source *)source)->w : 0;
}

uint32_t obs_source_get_height(obs_source_t *source)
{
	return source ? ((struct obs_source *)source)->h : 0;
}

bool obs_source_active(const obs_source_t *source)
{
	return source ? ((const struct obs_source *)source)->active : false;
}

enum gs_color_space obs_source_get_color_space(obs_source_t *source, size_t count, const enum gs_color_space *preferred)
{
	(void)count, (void)preferred;
	return source ? (enum gs_color_space)((struct obs_source *)source)->space : GS_CS_SRGB;
}

void obs_source_video_render(obs_source_t *source)
{
	ev("source_video_render %s", srcname(source));
}

bool obs_source_add_active_child(obs_source_t *parent, obs_source_t *child)
{
	struct obs_source *c = (struct obs_source *)child;
	if (c)
		c->active_child_refs++;
	ev("add_active_child %s <- %s", srcname(parent), srcname(child));
	return true;
}

void obs_source_remove_active_child(obs_source_t *parent, obs_source_t *child)
{
	struct obs_source *c = (struct obs_source *)child;
	if (c)
		c->active_child_refs--;
	ev("remove_active_child %s <- %s", srcname(parent), srcname(child));
}

void obs_source_set_muted(obs_source_t *source, bool muted)
{
	struct obs_source *s = (struct obs_source *)source;
	ev("source_set_muted %s %d", srcname(source), (int)muted);
	if (s)
		s->muted = muted;
}

void obs_source_set_monitoring_enabled(obs_source_t *source, bool enabled)
{
	struct obs_source *s = (struct obs_source *)source;
	ev("source_set_monitoring %s %d", srcname(source), (int)enabled);
	if (s)
		s->monitoring = enabled;
}

enum obs_media_state obs_source_media_get_state(obs_source_t *source)
{
	return source ? (enum obs_media_state)((struct obs_source *)source)->state : OBS_MEDIA_STATE_NONE;
}

bool obs_source_audio_pending(const obs_source_t *source)
{
	return source ? ((const struct obs_source *)source)->audio_pending : true;
}

uint64_t obs_source_get_audio_timestamp(const obs_source_t *source)
{
	return source ? ((const struct obs_source *)source)->audio_ts : 0;
}

void obs_source_get_audio_mix(const obs_source_t *source, struct obs_source_audio_mix *audio)
{
	static float buffers[MAX_AUDIO_MIXES][MAX_AUDIO_CHANNELS][AUDIO_OUTPUT_FRAMES];
	float fill = source ? ((const struct obs_source *)source)->child_fill : 0.0f;
	ev("source_get_audio_mix %s fill=%.9g", srcname(source), (double)fill);
	for (int m = 0; m < MAX_AUDIO_MIXES; m++)
		for (int c = 0; c < MAX_AUDIO_CHANNELS; c++) {
			for (int i = 0; i < AUDIO_OUTPUT_FRAMES; i++)
				buffers[m][c][i] = fill;
			audio->output[m].data[c] = buffers[m][c];
		}
}

proc_handler_t *obs_source_get_proc_handler(const obs_source_t *source)
{
	return (proc_handler_t *)source; /* the fake proc handler is the source itself */
}

bool proc_handler_call(proc_handler_t *handler, const char *name, calldata_t *params)
{
	struct obs_source *s = (struct obs_source *)handler;
	if (!s) {
		ev("proc_call (null handler) %s -> false", name);
		return false;
	}
	ev("proc_call %s %s", srcname((obs_source_t *)handler), name);
	if (!strcmp(name, "get_duration"))
		calldata_set_int(params, "duration", s->dur_ns);
	else if (!strcmp(name, "get_nb_frames"))
		calldata_set_int(params, "num_frames", s->frames);
	return true;
}

gs_effect_t *obs_get_base_effect(enum obs_base_effect effect)
{
	static struct fake_effect base = {"<base:default>", 0};
	ev("get_base_effect %d", (int)effect);
	return (gs_effect_t *)&base;
}

float obs_get_video_sdr_white_level(void)
{
	return W.sdr_white;
}

/* ------------------------------------------------------------------------- */
/* core: transition helpers (contract of libobs/obs-source-transition.c) */

void obs_transition_enable_fixed(obs_source_t *transition, bool enable, uint32_t duration_ms)
{
	struct obs_source *s = (struct obs_source *)transition;
	ev("transition_enable_fixed %d duration_ms=%u", (int)enable, duration_ms);
	s->fixed = enable;
	s->fixed_ms = duration_ms;
}

float obs_transition_get_time(obs_source_t *transition)
{
	return ((struct obs_source *)transition)->time;
}

enum gs_color_space obs_transition_video_get_color_space(obs_source_t *transition)
{
	(void)transition;
	return W.tr_space;
}

static void do_video_render(obs_source_t *transition, obs_transition_video_render_callback_t cb, gs_texture_t *ph,
			    const char *what)
{
	struct obs_source *s = (struct obs_source *)transition;
	char phs[32];
	tex_str(phs, sizeof(phs), ph);
	ev("transition_%s callback=%s placeholder=%s", what, cb ? "yes" : "none", phs);
	if (!cb) {
		ev("  default render (no callback)");
		return;
	}
	gs_texture_t *a = (gs_texture_t *)W.tex_a;
	gs_texture_t *b = (gs_texture_t *)W.tex_b;
	if (!a)
		a = ph;
	if (!b)
		b = ph;
	char as[32], bs[32];
	tex_str(as, sizeof(as), a);
	tex_str(bs, sizeof(bs), b);
	if (!W.cx || !W.cy) {
		ev("  callback skipped (size 0)");
		return;
	}
	ev("  callback a=%s b=%s t=%.9g cx=%u cy=%u", as, bs, (double)W.t, W.cx, W.cy);
	cb(s->data, a, b, W.t, W.cx, W.cy);
}

void obs_transition_video_render(obs_source_t *transition, obs_transition_video_render_callback_t callback)
{
	/* the real helper passes obs->video.transparent_texture as the placeholder */
	do_video_render(transition, callback, (gs_texture_t *)W.placeholder, "video_render");
}

void obs_transition_video_render2(obs_source_t *transition, obs_transition_video_render_callback_t callback,
				  gs_texture_t *placeholder_texture)
{
	do_video_render(transition, callback, placeholder_texture, "video_render2");
}

bool obs_transition_video_render_direct(obs_source_t *transition, enum obs_transition_target target)
{
	(void)transition;
	ev("transition_video_render_direct target=%s -> %d", target == OBS_TRANSITION_SOURCE_A ? "A" : "B",
	   (int)W.direct_ret);
	return W.direct_ret;
}

/* t values for which obs_transition_audio_render evaluates the mix callbacks (set by the runner) */
double fk_mix_t[16];
int fk_mix_n;

bool obs_transition_audio_render(obs_source_t *transition, uint64_t *ts_out, struct obs_source_audio_mix *audio,
				 uint32_t mixers, size_t channels, size_t sample_rate,
				 obs_transition_audio_mix_callback_t mix_a_callback,
				 obs_transition_audio_mix_callback_t mix_b_callback)
{
	struct obs_source *s = (struct obs_source *)transition;
	ev("transition_audio_render mixers=0x%x channels=%zu rate=%zu", mixers, channels, sample_rate);
	for (int i = 0; i < fk_mix_n; i++) {
		float t = (float)fk_mix_t[i];
		float a = mix_a_callback(s->data, t);
		float b = mix_b_callback(s->data, t);
		ev("  mix t=%s a=%s b=%s", fk_f9(t), fk_f9(a), fk_f9(b));
	}
	*ts_out = W.audio_ts_out;
	for (int m = 0; m < MAX_AUDIO_MIXES; m++) {
		if (!(mixers & (1u << m)))
			continue;
		for (size_t c = 0; c < channels; c++)
			for (int i = 0; i < AUDIO_OUTPUT_FRAMES; i++)
				audio->output[m].data[c][i] = W.audio_base;
	}
	return W.audio_ret;
}

/* missing-file bookkeeping is the real obs-missing-files.c; nothing to fake here */

/* obs-missing-files.c calls this when a missing-file callback is issued; the fake only forwards. */
void obs_source_replace_missing_file(obs_missing_file_cb cb, obs_source_t *source, const char *new_path, void *data)
{
	cb(source ? ((struct obs_source *)source)->data : NULL, new_path, data);
}
