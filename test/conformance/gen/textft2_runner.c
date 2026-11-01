/*
 * Conformance runner for the FreeType2 text source (spec S-038). The plugin's own C files
 * (text-freetype2.c, text-functionality.c, obs-convenience.c) are compiled unchanged into this translation
 * unit (so internal state can be inspected) and driven through their obs_source_info callbacks. FreeType,
 * fontconfig lookup and the graphics subsystem are faked in textft2_stubs.c with scripted glyph metrics;
 * obs-data, obs-properties and obs-missing-files are the real libobs code. Script language: see
 * test/conformance/README.md, section "textft2 runner".
 */
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <obs-module.h>
#include <obs-properties.h>
#include <obs-missing-files.h>
#include <util/platform.h>
#include "textft2_api.h"

/* the plugin reads the clock through os_gettime_ns(): route it to the scripted clock */
#define os_gettime_ns fake_clock_get
#include "../../../plugins/text-freetype2/text-freetype2.c"
#include "../../../plugins/text-freetype2/text-functionality.c"
#include "../../../plugins/text-freetype2/obs-convenience.c"
#undef os_gettime_ns

#include "harness.h"

/* ---------------------------------------------------------------- fake host */
struct obs_source {
	char name[32];
	const struct obs_source_info *info;
	void *data;
	obs_data_t *settings;
};
static struct obs_source srcs[8];
static int nsrcs;
static const struct obs_source_info *infos[4];
static int ninfos;
static char tmpdir[512] = ".";

void obs_register_source_s(const struct obs_source_info *info, size_t size)
{
	(void)size;
	infos[ninfos++] = info;
}
void obs_source_update(obs_source_t *s, obs_data_t *settings)
{
	/* NOTE: the real host defers updates of video sources to the next tick; scripts call `update` explicitly. */
	if (settings)
		obs_data_apply(s->settings, settings);
}
const char *obs_source_get_name(const obs_source_t *s)
{
	return s->name;
}
void obs_source_replace_missing_file(obs_missing_file_cb cb, obs_source_t *s, const char *new_path, void *data)
{
	cb(s->data, new_path, data);
}
bool obs_get_video_info(struct obs_video_info *ovi)
{
	(void)ovi;
	return false;
}
obs_data_t *obs_source_get_settings(const obs_source_t *s)
{
	obs_data_addref(s->settings);
	return s->settings;
}

static const struct obs_source_info *find_info(int ver)
{
	for (int i = 0; i < ninfos; i++)
		if (infos[i]->version == (uint32_t)(ver == 1 ? 0 : ver))
			return infos[i];
	fprintf(stderr, "script error: module not loaded or bad version %d\n", ver);
	exit(2);
}
static struct obs_source *need(const char *name)
{
	for (int i = 0; i < nsrcs; i++)
		if (!strcmp(srcs[i].name, name))
			return &srcs[i];
	fprintf(stderr, "script error: unknown source %s\n", name);
	exit(2);
}

static void log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	/* hide the scratch directory */
	size_t tl = strlen(tmpdir);
	char out[4096];
	size_t o = 0;
	for (char *p = msg; *p && o < sizeof(out) - 8; p++) {
		if (tl > 1 && !strncmp(p, tmpdir, tl)) {
			memcpy(out + o, "$TMP", 4);
			o += 4;
			p += tl - 1;
		} else {
			out[o++] = *p == '\n' ? ' ' : *p;
		}
	}
	out[o] = 0;
	const char *name = level == LOG_ERROR ? "error" : level == LOG_WARNING ? "warning" : level == LOG_INFO ? "info" : "debug";
	printf("log %s %s\n", name, out);
}

/* "@TMP@/x" -> "<tmpdir>/x" */
static char *expand(const char *s)
{
	static char buf[8][1024];
	static int n;
	char *b = buf[n++ & 7];
	if (!strncmp(s, "@TMP@", 5))
		snprintf(b, 1024, "%s%s", tmpdir, s + 5);
	else
		snprintf(b, 1024, "%s", s);
	return b;
}

/* file-system commands take names relative to the scratch directory */
static char *fpath(const char *name)
{
	static char buf[1024];
	snprintf(buf, sizeof(buf), "%s/%s", tmpdir, name);
	return buf;
}

/* ---------------------------------------------------------------- printing helpers */
static void pwide(const wchar_t *w)
{
	putchar('"');
	if (w)
		for (; *w; w++) {
			unsigned c = (unsigned)*w;
			if (c == '\\')
				fputs("\\\\", stdout);
			else if (c == '"')
				fputs("\\\"", stdout);
			else if (c == '\n')
				fputs("\\n", stdout);
			else if (c == '\r')
				fputs("\\r", stdout);
			else if (c >= 0x20 && c < 0x7f)
				putchar((int)c);
			else
				printf("\\u{%X}", c);
		}
	putchar('"');
}
static void pstr(const char *s)
{
	if (!s)
		fputs("NULL", stdout);
	else
		script_print_quoted(s);
}

static void dump_state(struct obs_source *s)
{
	struct ft2_source *d = s->data;
	printf("state %s font=", s->name);
	pstr(d->font_name);
	printf(" style=");
	pstr(d->font_style);
	printf(" size=%u flags=%u face=%d from_file=%d aa=%d\n", d->font_size, d->font_flags, d->font_face != NULL,
	       d->from_file, d->antialiasing);
	printf("state %s outline=%d shadow=%d outline_width=%u word_wrap=%d custom_width=%u log_mode=%d log_lines=%u color=%08x,%08x\n",
	       s->name, d->outline_text, d->drop_shadow, d->outline_width, d->word_wrap, d->custom_width, d->log_mode,
	       d->log_lines, d->color[0], d->color[1]);
	printf("state %s cx=%u cy=%u max_h=%u texbuf=%d,%d tex=%d vbuf=%d effect=%d\n", s->name, d->cx, d->cy, d->max_h,
	       d->texbuf_x, d->texbuf_y, d->tex != NULL, d->vbuf != NULL, d->draw_effect != NULL);
	printf("state %s file=", s->name);
	pstr(d->text_file ? (strncmp(d->text_file, tmpdir, strlen(tmpdir)) ? d->text_file : d->text_file + strlen(tmpdir)) : NULL);
	printf(" load_failed=%d update_file=%d mtime=%lld last_checked_ms=%llu\n", d->file_load_failed, d->update_file,
	       (long long)d->m_timestamp, (unsigned long long)(d->last_checked / 1000000));
	printf("state %s text=", s->name);
	if (d->text)
		pwide(d->text);
	else
		fputs("NULL", stdout);
	putchar('\n');
}

static void dump_glyphs(struct obs_source *s, int argc, char **argv)
{
	struct ft2_source *d = s->data;
	int n = 0;
	for (uint32_t i = 0; i < num_cache_slots; i++) {
		struct glyph_info *g = d->cacheglyphs[i];
		if (!g)
			continue;
		n++;
		if (argc > 0) { /* only the listed glyph indices */
			bool want = false;
			for (int k = 0; k < argc; k++)
				if ((uint32_t)strtoul(argv[k], NULL, 0) == i)
					want = true;
			if (!want)
				continue;
		}
		printf("glyph %u u=%.9g v=%.9g u2=%.9g v2=%.9g w=%d h=%d xoff=%d yoff=%d xadv=%ld\n", i, (double)g->u, (double)g->v,
		       (double)g->u2, (double)g->v2, g->w, g->h, g->xoff, g->yoff, (long)g->xadv);
	}
	printf("glyphs cached=%d\n", n);
}

static void dump_verts(struct obs_source *s, int maxq)
{
	struct ft2_source *d = s->data;
	if (!d->vbuf) {
		puts("verts none");
		return;
	}
	struct gs_vb_data *v = gs_vertexbuffer_get_data(d->vbuf);
	struct vec2 *uv = (struct vec2 *)v->tvarray[0].array;
	printf("verts num=%zu quads=%zu\n", v->num, v->num / 6);
	for (size_t q = 0; q < v->num / 6; q++) {
		if (maxq >= 0 && (int)q >= maxq)
			break;
		struct vec3 *p = v->points + q * 6;
		struct vec2 *t = uv + q * 6;
		uint32_t *c = v->colors + q * 6;
		printf("quad %zu pos=%.9g,%.9g..%.9g,%.9g uv=%.9g,%.9g..%.9g,%.9g col=%08x,%08x,%08x,%08x,%08x,%08x\n", q,
		       (double)p[0].x, (double)p[0].y, (double)p[5].x, (double)p[5].y, (double)t[0].x, (double)t[0].y,
		       (double)t[5].x, (double)t[5].y, c[0], c[1], c[2], c[3], c[4], c[5]);
	}
}

static const char *ptype(enum obs_property_type t)
{
	switch (t) {
	case OBS_PROPERTY_BOOL: return "bool";
	case OBS_PROPERTY_INT: return "int";
	case OBS_PROPERTY_FLOAT: return "float";
	case OBS_PROPERTY_TEXT: return "text";
	case OBS_PROPERTY_PATH: return "path";
	case OBS_PROPERTY_LIST: return "list";
	case OBS_PROPERTY_COLOR: return "color";
	case OBS_PROPERTY_COLOR_ALPHA: return "color_alpha";
	case OBS_PROPERTY_FONT: return "font";
	default: return "other";
	}
}

static void dump_props(obs_properties_t *props)
{
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		enum obs_property_type t = obs_property_get_type(p);
		printf("prop %s type=%s desc=%s visible=%d enabled=%d", obs_property_name(p), ptype(t), obs_property_description(p),
		       obs_property_visible(p), obs_property_enabled(p));
		const char *ld = obs_property_long_description(p);
		if (ld)
			printf(" long=%s", ld);
		if (t == OBS_PROPERTY_INT)
			printf(" min=%d max=%d step=%d", obs_property_int_min(p), obs_property_int_max(p), obs_property_int_step(p));
		if (t == OBS_PROPERTY_TEXT)
			printf(" text_type=%d", (int)obs_property_text_type(p));
		if (t == OBS_PROPERTY_PATH)
			printf(" path_type=%d filter=%s", (int)obs_property_path_type(p), obs_property_path_filter(p));
		if (t == OBS_PROPERTY_LIST) {
			printf(" list_type=%d format=%d", (int)obs_property_list_type(p), (int)obs_property_list_format(p));
			for (size_t i = 0; i < obs_property_list_item_count(p); i++)
				printf(" item[%zu]=%s:%d", i, obs_property_list_item_name(p, i), obs_property_list_item_bool(p, i));
		}
		putchar('\n');
	}
}

/* ---------------------------------------------------------------- script commands */
static long long num(const char *s)
{
	return strtoll(s, NULL, 0);
}
static bool onoff(const char *s)
{
	return !strcmp(s, "on") || !strcmp(s, "1") || !strcmp(s, "true");
}

static void write_file(const char *name, const uint8_t *data, size_t len)
{
	FILE *f = fopen(fpath(name), "wb");
	if (!f) {
		fprintf(stderr, "script error: cannot write %s\n", name);
		exit(2);
	}
	if (len)
		fwrite(data, 1, len, f);
	fclose(f);
}

static void set_value(obs_data_t *d, const char *key, const char *val, bool force_str)
{
	char *end;
	if (!force_str && (!strcmp(val, "true") || !strcmp(val, "false"))) {
		obs_data_set_bool(d, key, !strcmp(val, "true"));
		return;
	}
	if (!force_str && *val) {
		long long v = strtoll(val, &end, 0);
		if (!*end) {
			obs_data_set_int(d, key, v);
			return;
		}
	}
	obs_data_set_string(d, key, expand(val));
}

static void run_cmd(struct script_line *sl);

/* `try CMD ...`: run one command in a child process; report a crash instead of dying. */
static void run_try(struct script_line *sl)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		struct script_line inner = *sl;
		inner.argc--;
		for (int i = 0; i < inner.argc; i++)
			inner.argv[i] = sl->argv[i + 1];
		run_cmd(&inner);
		fflush(stdout);
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFSIGNALED(st))
		printf("crash signal=%s\n", WTERMSIG(st) == SIGSEGV ? "SIGSEGV" : WTERMSIG(st) == SIGABRT ? "SIGABRT" : "other");
	else
		puts("no-crash");
}

static void run_cmd(struct script_line *sl)
{
	char **a = sl->argv;
	const char *c = a[0];
#define IS(x) (!strcmp(c, x))
	if (IS("try")) {
		script_need(sl, 2);
		run_try(sl);
	} else if (IS("echo")) {
		for (int i = 1; i < sl->argc; i++)
			printf("%s%s", i > 1 ? " " : "", a[i]);
		putchar('\n');
	} else if (IS("events")) {
		script_need(sl, 2);
		fake_events = onoff(a[1]);
	} else if (IS("reset")) {
		fake_ft_reset();
	} else if (IS("texdim")) {
		script_need(sl, 3);
		texbuf_w = (uint32_t)num(a[1]);
		texbuf_h = (uint32_t)num(a[2]);
	} else if (IS("ascii")) { /* W H LEFT TOP ADV(26.6): glyph index == code point for 0x20..0x7e; space is empty */
		script_need(sl, 6);
		for (unsigned cp = 0x20; cp < 0x7f; cp++) {
			if (cp == 0x20)
				fake_ft_glyph(cp, cp, 0, 0, 0, 0, (long)num(a[5]));
			else
				fake_ft_glyph(cp, cp, (int)num(a[1]), (int)num(a[2]), (int)num(a[3]), (int)num(a[4]), (long)num(a[5]));
		}
	} else if (IS("glyph")) { /* CP INDEX W H LEFT TOP ADV(26.6) */
		script_need(sl, 8);
		fake_ft_glyph((unsigned long)num(a[1]), (unsigned)num(a[2]), (int)num(a[3]), (int)num(a[4]), (int)num(a[5]),
			      (int)num(a[6]), (long)num(a[7]));
	} else if (IS("notdef")) {
		script_need(sl, 6);
		fake_ft_notdef((int)num(a[1]), (int)num(a[2]), (int)num(a[3]), (int)num(a[4]), (long)num(a[5]));
	} else if (IS("pitch_pad")) {
		script_need(sl, 2);
		fake_ft_pitch_pad((int)num(a[1]));
	} else if (IS("fail_init")) {
		fake_ft_fail_init(onoff(a[1]));
	} else if (IS("fail_face")) {
		fake_ft_fail_face(onoff(a[1]));
	} else if (IS("fail_effect")) {
		fake_gfx_fail_effect(onoff(a[1]));
	} else if (IS("linear")) {
		fake_gfx_linear(onoff(a[1]));
	} else if (IS("font")) { /* NAME STYLE FLAGS PATH */
		script_need(sl, 5);
		fake_font_table(a[1], strcmp(a[2], "-") ? a[2] : "", (uint32_t)num(a[3]), a[4]);
	} else if (IS("clock")) { /* ms */
		script_need(sl, 2);
		fake_clock_set((uint64_t)num(a[1]) * 1000000ull);
	} else if (IS("load")) {
		printf("load -> %d\n", obs_module_load());
		for (int i = 0; i < ninfos; i++)
			printf("registered id=%s version=%u\n", infos[i]->id, infos[i]->version);
	} else if (IS("unload")) {
		obs_module_unload();
	} else if (IS("info")) {
		script_need(sl, 2);
		const struct obs_source_info *i = find_info((int)num(a[1]));
		printf("info id=%s version=%u type=%d flags=0x%x icon=%d name=%s\n", i->id, i->version, (int)i->type,
		       i->output_flags, (int)i->icon_type, i->get_name(NULL));
		printf("info callbacks create=%d destroy=%d update=%d get_width=%d get_height=%d video_render=%d video_tick=%d get_defaults=%d get_properties=%d missing_files=%d\n",
		       !!i->create, !!i->destroy, !!i->update, !!i->get_width, !!i->get_height, !!i->video_render,
		       !!i->video_tick, !!i->get_defaults, !!i->get_properties, !!i->missing_files);
	} else if (IS("defaults")) {
		script_need(sl, 2);
		const struct obs_source_info *i = find_info((int)num(a[1]));
		obs_data_t *d = obs_data_create();
		i->get_defaults(d);
		obs_data_t *f = obs_data_get_obj(d, "font");
		printf("defaults font.face=");
		pstr(obs_data_get_string(f, "face"));
		printf(" font.size=%lld font.flags=%lld font.style=", obs_data_get_int(f, "size"), obs_data_get_int(f, "flags"));
		pstr(obs_data_get_string(f, "style"));
		printf("\ndefaults antialiasing=%d word_wrap=%d outline=%d drop_shadow=%d from_file=%d log_mode=%d\n",
		       obs_data_get_bool(d, "antialiasing"), obs_data_get_bool(d, "word_wrap"), obs_data_get_bool(d, "outline"),
		       obs_data_get_bool(d, "drop_shadow"), obs_data_get_bool(d, "from_file"), obs_data_get_bool(d, "log_mode"));
		printf("defaults log_lines=%lld color1=0x%llx color2=0x%llx custom_width=%lld text=", obs_data_get_int(d, "log_lines"),
		       (unsigned long long)obs_data_get_int(d, "color1"), (unsigned long long)obs_data_get_int(d, "color2"),
		       obs_data_get_int(d, "custom_width"));
		pstr(obs_data_get_string(d, "text"));
		putchar('\n');
		static const char *const keys[] = {"antialiasing", "word_wrap", "outline", "drop_shadow", "log_lines",
						   "color1", "color2", "from_file", "log_mode", "custom_width", "text",
						   "text_file", "font"};
		for (size_t ki = 0; ki < sizeof(keys) / sizeof(keys[0]); ki++)
			printf("defaults has %s=%d\n", keys[ki], obs_data_has_default_value(d, keys[ki]));
		obs_data_release(f);
		obs_data_release(d);
	} else if (IS("props")) {
		script_need(sl, 2);
		obs_properties_t *p = find_info((int)num(a[1]))->get_properties(NULL);
		dump_props(p);
		obs_properties_destroy(p);
	} else if (IS("props_modify")) { /* VER from_file 0|1: run the modified callback of "from_file" */
		script_need(sl, 3);
		obs_properties_t *p = find_info((int)num(a[1]))->get_properties(NULL);
		obs_data_t *d = obs_data_create();
		obs_data_set_bool(d, "from_file", onoff(a[2]));
		obs_property_t *pr = obs_properties_get(p, "from_file");
		printf("modified -> %d\n", obs_property_modified(pr, d));
		printf("visible text=%d text_file=%d\n", obs_property_visible(obs_properties_get(p, "text")),
		       obs_property_visible(obs_properties_get(p, "text_file")));
		obs_data_release(d);
		obs_properties_destroy(p);
	} else if (IS("create")) { /* NAME VER */
		script_need(sl, 3);
		struct obs_source *s = &srcs[nsrcs++];
		snprintf(s->name, 32, "%s", a[1]);
		s->info = find_info((int)num(a[2]));
		s->settings = obs_data_create();
		s->info->get_defaults(s->settings);
		s->data = s->info->create(s->settings, s);
		printf("create %s -> %d\n", s->name, s->data != NULL);
	} else if (IS("destroy")) {
		struct obs_source *s = need(a[1]);
		s->info->destroy(s->data);
		s->data = NULL;
	} else if (IS("set")) { /* NAME KEY VALUE */
		script_need(sl, 4);
		set_value(need(a[1])->settings, a[2], a[3], false);
	} else if (IS("sets")) { /* NAME KEY STRING (always a string) */
		script_need(sl, 4);
		set_value(need(a[1])->settings, a[2], a[3], true);
	} else if (IS("setfont")) { /* NAME FACE STYLE SIZE FLAGS */
		script_need(sl, 6);
		obs_data_t *f = obs_data_create();
		obs_data_set_string(f, "face", a[2]);
		obs_data_set_string(f, "style", strcmp(a[3], "-") ? a[3] : "");
		obs_data_set_int(f, "size", num(a[4]));
		obs_data_set_int(f, "flags", num(a[5]));
		obs_data_set_obj(need(a[1])->settings, "font", f);
		obs_data_release(f);
	} else if (IS("unsetfont")) {
		obs_data_unset_user_value(need(a[1])->settings, "font");
		obs_data_erase(need(a[1])->settings, "font");
	} else if (IS("update")) {
		struct obs_source *s = need(a[1]);
		s->info->update(s->data, s->settings);
		puts("update done");
	} else if (IS("tick")) { /* NAME */
		struct obs_source *s = need(a[1]);
		s->info->video_tick(s->data, 0.0f);
	} else if (IS("render")) {
		struct obs_source *s = need(a[1]);
		s->info->video_render(s->data, NULL);
		puts("render done");
	} else if (IS("size")) {
		struct obs_source *s = need(a[1]);
		printf("size %s %ux%u\n", s->name, s->info->get_width(s->data), s->info->get_height(s->data));
	} else if (IS("state")) {
		dump_state(need(a[1]));
	} else if (IS("glyphs")) {
		dump_glyphs(need(a[1]), sl->argc - 2, a + 2);
	} else if (IS("verts")) {
		dump_verts(need(a[1]), sl->argc > 2 ? (int)num(a[2]) : -1);
	} else if (IS("texbuf")) { /* NAME X Y W H */
		script_need(sl, 6);
		struct ft2_source *d = need(a[1])->data;
		long x = (long)num(a[2]), y = (long)num(a[3]), w = (long)num(a[4]), h = (long)num(a[5]);
		for (long j = 0; j < h; j++) {
			printf("texrow %ld:", y + j);
			for (long i = 0; i < w; i++)
				printf(" %02x", d->texbuf[(size_t)(y + j) * texbuf_w + (size_t)(x + i)]);
			putchar('\n');
		}
	} else if (IS("texsum")) { /* NAME: count and sum of non-zero texels */
		struct ft2_source *d = need(a[1])->data;
		unsigned long long nz = 0, sum = 0;
		for (size_t i = 0; i < (size_t)texbuf_w * texbuf_h; i++)
			if (d->texbuf[i]) {
				nz++;
				sum += d->texbuf[i];
			}
		printf("texsum nonzero=%llu sum=%llu\n", nz, sum);
	} else if (IS("cache")) { /* NAME UTF8: cache_glyphs on the given text */
		struct obs_source *s = need(a[1]);
		wchar_t *w = NULL;
		os_utf8_to_wcs_ptr(a[2], strlen(a[2]), &w);
		cache_glyphs(s->data, w);
		bfree(w);
		puts("cache done");
	} else if (IS("width")) { /* NAME UTF8: get_ft2_text_width */
		struct obs_source *s = need(a[1]);
		wchar_t *w = NULL;
		os_utf8_to_wcs_ptr(a[2], strlen(a[2]), &w);
		printf("width %u\n", get_ft2_text_width(w, s->data));
		bfree(w);
	} else if (IS("file")) { /* NAME "content": write text file into the scratch dir */
		script_need(sl, 3);
		write_file(a[1], (const uint8_t *)a[2], strlen(a[2]));
	} else if (IS("filehex")) {
		script_need(sl, 3);
		size_t n;
		uint8_t *b = script_parse_hex(a[2], &n);
		write_file(a[1], b, n);
		free(b);
	} else if (IS("touch")) { /* NAME EPOCH_SECONDS */
		script_need(sl, 3);
		struct timespec ts[2] = {{(time_t)num(a[2]), 0}, {(time_t)num(a[2]), 0}};
		utimensat(AT_FDCWD, fpath(a[1]), ts, 0);
	} else if (IS("rm")) {
		unlink(fpath(a[1]));
	} else if (IS("missing")) { /* NAME [NEWPATH]: list missing files; with NEWPATH invoke the first callback */
		struct obs_source *s = need(a[1]);
		obs_missing_files_t *f = s->info->missing_files(s->data);
		printf("missing count=%zu\n", obs_missing_files_count(f));
		for (size_t i = 0; i < obs_missing_files_count(f); i++) {
			obs_missing_file_t *m = obs_missing_files_get_file(f, (int)i);
			const char *p = obs_missing_file_get_path(m);
			printf("missing[%zu] path=", i);
			pstr(!strncmp(p, tmpdir, strlen(tmpdir)) ? p + strlen(tmpdir) : p);
			putchar('\n');
			if (sl->argc > 2) {
				obs_missing_file_issue_callback(m, expand(a[2]));
				const char *np = obs_data_get_string(s->settings, "text_file");
				printf("callback text_file=");
				pstr(!strncmp(np, tmpdir, strlen(tmpdir)) ? np + strlen(tmpdir) : np);
				putchar('\n');
			}
		}
		obs_missing_files_destroy(f);
	} else {
		fprintf(stderr, "script error: line %d: unknown command %s\n", sl->lineno, c);
		exit(2);
	}
#undef IS
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	const char *t = getenv("CONFORMANCE_TMPDIR");
	if (t)
		snprintf(tmpdir, sizeof(tmpdir), "%s", t);
	base_set_log_handler(log_handler, NULL);
	fake_ft_reset();
	struct script_line sl;
	int lineno = 0;
	while (script_next(f, &sl, &lineno)) {
		printf("> ");
		for (int i = 0; i < sl.argc; i++)
			printf("%s%s", i ? " " : "", sl.argv[i]);
		putchar('\n');
		run_cmd(&sl);
	}
	fclose(f);
	return 0;
}
