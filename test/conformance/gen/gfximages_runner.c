/*
 * Conformance runner for libobs image decoding and the built-in effects (spec S-053).
 * Links the real image-file.c, graphics-ffmpeg.c, the vendored libnsgif (gif.c, lzw.c), graphics.c and the
 * effect parser, on a fake graphics backend (gfximages_stubs.c). Commands are documented in
 * test/conformance/README.md ("gfximages runner").
 *
 * Test tooling, not part of libobs.
 */
#include <errno.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "graphics/image-file.h"
#include "graphics/libnsgif/nsgif.h"
#include "graphics/effect.h"
#include "graphics/effect-parser.h"
#include "graphics/shader-parser.h"
#include "graphics/graphics.h"
#include "util/platform.h"
#include <libavutil/log.h>
#include "harness.h"

uint8_t *gfx_hook_reformat(const char *pix, int w, int h, int linesize, const uint8_t *src, int orient, int alpha_mode,
			   int *gs_format, int *cx, int *cy);

struct fake_tex {
	uint32_t cx, cy;
	enum gs_color_format fmt;
	uint32_t flags, levels;
	uint32_t linesize;
	uint8_t *data;
	int set_image_calls;
};
extern struct fake_tex *fake_last_texture;
extern int fake_textures_alive;

static graphics_t *gfx;
static char tmpdir[400];
static char repo_root[1024];
#define IS(s) (!strcmp(sl->argv[0], (s)))

static void die(const char *m)
{
	fprintf(stderr, "runner: %s\n", m);
	exit(2);
}

/* In forked children the libobs crash handler (bcrash) prints its message and ends the child with status 7;
 * the default handler calls exit(0), which would also flush the script reader's buffer in a child. */
static void child_crash_handler(const char *format, va_list args, void *param)
{
	char msg[512];
	(void)param;
	vsnprintf(msg, sizeof msg, format, args);
	printf("crash handler: %s\n", msg);
	fflush(stdout);
	_exit(7);
}

/* Like the common harness handler, but the FFmpeg error text that libobs appends after "': " is replaced by a
 * placeholder: it differs between FFmpeg versions and is not part of the contract. */
static void log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	char *cut = strstr(msg, "': ");
	if (cut && strstr(msg, "ailed to ") && !strstr(msg, "scale") && !strstr(msg, "Failed to initialize gif"))
		strcpy(cut + 1, " <ffmpeg-error>");
	for (char *p = msg; *p; p++)
		if (*p == '\n')
			*p = ' ';
	printf("log %s %s\n", level == LOG_ERROR ? "error" : level == LOG_WARNING ? "warning" : level == LOG_INFO ? "info" : "debug",
	       msg);
}

static void ensure_gfx(void)
{
	if (gfx)
		return;
	if (gs_create(&gfx, "fake", 0) != GS_SUCCESS)
		die("gs_create failed");
	gs_enter_context(gfx);
}

static void hex(const uint8_t *p, size_t n)
{
	script_print_hex(p, n);
}

static const char *fmt_name(int f)
{
	switch (f) {
	case GS_RGBA:
		return "RGBA";
	case GS_BGRA:
		return "BGRA";
	case GS_BGRX:
		return "BGRX";
	case GS_RGBA16:
		return "RGBA16";
	case GS_UNKNOWN:
		return "UNKNOWN";
	default:
		return "other";
	}
}

static int alpha_mode(const char *s)
{
	if (!strcmp(s, "straight"))
		return GS_IMAGE_ALPHA_STRAIGHT;
	if (!strcmp(s, "srgb"))
		return GS_IMAGE_ALPHA_PREMULTIPLY_SRGB;
	if (!strcmp(s, "premul"))
		return GS_IMAGE_ALPHA_PREMULTIPLY;
	die("bad alpha mode (straight|srgb|premul)");
	return 0;
}

static char *path_of(const char *name)
{
	static char buf[600];
	/* the working directory is the scratch directory: file names (and so log messages) stay relative */
	snprintf(buf, sizeof buf, "%s", name);
	return buf;
}

/* ---------------------------------------------------------------- bitmap callbacks for direct nsgif use */

static void *bm_create(int w, int h)
{
	return calloc(1, (size_t)4 * w * h + 1);
}
static void bm_destroy(void *b)
{
	free(b);
}
static uint8_t *bm_buf(void *b)
{
	return b;
}
static nsgif_bitmap_cb_vt cbs = {bm_create, bm_destroy, bm_buf, NULL, NULL, NULL, NULL};
static nsgif_t *gif;
static uint8_t *gif_data;
static size_t gif_size;

static void gif_load_file(const char *name)
{
	free(gif_data);
	FILE *f = fopen(path_of(name), "rb");
	if (!f)
		die("gif: cannot open fixture");
	fseek(f, 0, SEEK_END);
	gif_size = (size_t)ftell(f);
	fseek(f, 0, SEEK_SET);
	gif_data = malloc(gif_size + 1);
	if (fread(gif_data, 1, gif_size, f) != gif_size)
		die("gif: short read");
	fclose(f);
}

/* ---------------------------------------------------------------- image file state */

static gs_image_file_ex_t img;

static void print_img(void)
{
	printf("image loaded=%d animated=%d cx=%u cy=%u format=%s frame_updated=%d cur_frame=%d cur_loop=%d "
	       "cur_time=%llu mem_usage=%llu alpha_mode=%d space=%d texture=%s texture_data=%s\n",
	       img.loaded, img.is_animated_gif, img.cx, img.cy, fmt_name(img.format), img.frame_updated, img.cur_frame,
	       img.cur_loop, (unsigned long long)img.cur_time, (unsigned long long)img.mem_usage, (int)img.alpha_mode,
	       (int)img.space, img.texture ? "yes" : "no", img.texture_data ? "yes" : "no");
}

static void print_tex(void)
{
	if (!img.texture) {
		printf("texture none\n");
		return;
	}
	struct fake_tex *t = (struct fake_tex *)img.texture;
	printf("texture cx=%u cy=%u format=%s flags=%u map_calls=%d data=", t->cx, t->cy, fmt_name(t->fmt), t->flags,
	       t->set_image_calls);
	hex(t->data, (size_t)t->linesize * t->cy);
	putchar('\n');
}

/* ---------------------------------------------------------------- effects */

static void pq(const char *s)
{
	script_print_quoted(s);
}

static const char *ptype(int t)
{
	static const char *n[] = {"UNKNOWN", "BOOL", "FLOAT", "INT", "STRING", "VEC2", "VEC3", "VEC4",
				  "INT2",    "INT3", "INT4",  "MATRIX4X4", "TEXTURE"};
	return t >= 0 && t < 13 ? n[t] : "?";
}

static void print_default(enum gs_shader_param_type t, const uint8_t *v, size_t n)
{
	if ((t == GS_SHADER_PARAM_STRING || t == GS_SHADER_PARAM_TEXTURE) && n) {
		pq((const char *)v);
		return;
	}
	if (!n) {
		fputs("-", stdout);
		return;
	}
	if (t == GS_SHADER_PARAM_INT || t == GS_SHADER_PARAM_INT2 || t == GS_SHADER_PARAM_INT3 ||
	    t == GS_SHADER_PARAM_INT4 || t == GS_SHADER_PARAM_BOOL) {
		putchar('[');
		for (size_t i = 0; i + sizeof(long) <= n; i += sizeof(long)) {
			long l;
			memcpy(&l, v + i, sizeof l);
			printf("%s%ld", i ? "," : "", l);
		}
		putchar(']');
		return;
	}
	putchar('[');
	for (size_t i = 0; i + 4 <= n; i += 4) {
		float f;
		memcpy(&f, v + i, 4);
		printf("%s%.9g", i ? "," : "", f);
	}
	putchar(']');
}

static void print_tokens(const cf_token_array_t *a)
{
	/* the entry function name is the first token; print the whole call text compactly */
	for (const struct cf_token *t = a->array; t && t < a->array + a->num && t->type != CFTOKEN_NONE; t++)
		printf("%.*s", (int)t->str.len, t->str.array);
}

static void cmd_effect(const char *label, const char *path)
{
	ensure_gfx();
	/* "#include" is resolved relative to the effect's file name, so the working directory must be the repository
	 * root while parsing */
	char cwd[1024];
	if (!getcwd(cwd, sizeof cwd) || chdir(repo_root))
		die("effect: chdir");
	char *src = os_quick_read_utf8_file(path);
	if (!src)
		die("effect: cannot read file");
	gs_effect_t *e = bzalloc(sizeof(struct gs_effect));
	struct effect_parser ep;
	e->graphics = gfx;
	e->effect_path = bstrdup(label);
	ep_init(&ep);
	bool ok = ep_parse(&ep, e, src, label);
	printf("effect %s parse=%s params=%zu techniques=%zu viewproj=%s world=%s\n", label, ok ? "ok" : "fail",
	       e->params.num, e->techniques.num, e->view_proj ? e->view_proj->name : "-",
	       e->world ? e->world->name : "-");
	if (ok) {
		for (size_t i = 0; i < e->params.num; i++) {
			struct gs_effect_param *p = e->params.array + i;
			printf("  param %zu %s %s default=", i, p->name, ptype(p->type));
			print_default(p->type, p->default_val.array, p->default_val.num);
			for (size_t j = 0; j < p->annotations.num; j++) {
				struct gs_effect_param *a = p->annotations.array + j;
				printf(" ann(%s %s=", ptype(a->type), a->name);
				print_default(a->type, a->default_val.array, a->default_val.num);
				putchar(')');
			}
			putchar('\n');
		}
		for (size_t i = 0; i < ep.techniques.num; i++) {
			struct ep_technique *et = ep.techniques.array + i;
			struct gs_effect_technique *t = e->techniques.array + i;
			printf("  technique %s passes=%zu\n", t->name, t->passes.num);
			for (size_t j = 0; j < t->passes.num; j++) {
				struct gs_effect_pass *p = t->passes.array + j;
				struct ep_pass *ept = et->passes.array + j;
				printf("    pass %zu name=", j);
				if (p->name)
					pq(p->name);
				else
					fputs("@null", stdout);
				fputs(" vertex=", stdout);
				print_tokens(&ept->vertex_program);
				fputs(" pixel=", stdout);
				print_tokens(&ept->fragment_program);
				printf("\n      vs_params=[");
				for (size_t k = 0; k < p->vertshader_params.num; k++)
					printf("%s%s", k ? "," : "", p->vertshader_params.array[k].eparam->name);
				printf("] ps_params=[");
				for (size_t k = 0; k < p->pixelshader_params.num; k++)
					printf("%s%s", k ? "," : "", p->pixelshader_params.array[k].eparam->name);
				puts("]");
			}
		}
		printf("  funcs=%zu structs=%zu samplers=%zu\n", ep.funcs.num, ep.structs.num, ep.samplers.num);
		for (size_t i = 0; i < ep.samplers.num; i++) {
			struct ep_sampler *s = ep.samplers.array + i;
			printf("  sampler %s", s->name);
			for (size_t j = 0; j < s->states.num; j++)
				printf(" %s=%s", s->states.array[j], s->values.array[j]);
			putchar('\n');
		}
	} else {
		char *errs = error_data_buildstring(&ep.cfp.error_list);
		printf("  errors=");
		pq(errs ? errs : "");
		putchar('\n');
		bfree(errs);
	}
	effect_free(e);
	bfree(e);
	ep_free(&ep);
	bfree(src);
	if (chdir(cwd))
		die("effect: chdir back");
}

/* ---------------------------------------------------------------- main */

static void run(struct script_line *sl)
{
	if (IS("echo")) {
		for (int i = 1; i < sl->argc; i++)
			printf("%s%s", i > 1 ? " " : "", sl->argv[i]);
		putchar('\n');
	} else if (IS("file")) {
		script_need(sl, 3);
		size_t n;
		uint8_t *b = script_parse_hex(sl->argv[2], &n);
		FILE *f = fopen(path_of(sl->argv[1]), "wb");
		if (!f)
			die("file: cannot create");
		fwrite(b, 1, n, f);
		fclose(f);
		free(b);
		printf("file %s %zu bytes\n", sl->argv[1], n);
	} else if (IS("trunc")) { /* trunc NAME NEW NBYTES: copy the first NBYTES of NAME to NEW */
		script_need(sl, 4);
		FILE *f = fopen(path_of(sl->argv[1]), "rb");
		if (!f)
			die("trunc: open");
		size_t n = (size_t)atol(sl->argv[3]);
		uint8_t *b = malloc(n + 1);
		size_t got = fread(b, 1, n, f);
		fclose(f);
		f = fopen(path_of(sl->argv[2]), "wb");
		fwrite(b, 1, got, f);
		fclose(f);
		free(b);
		printf("trunc %s %zu bytes\n", sl->argv[2], got);
	}
	/* ---- decode a still image to pixel data ---- */
	else if (IS("texfile")) { /* texfile NAME MODE */
		script_need(sl, 3);
		enum gs_color_format f = GS_UNKNOWN;
		uint32_t cx = 0, cy = 0;
		enum gs_color_space sp = GS_CS_709_EXTENDED;
		uint8_t *d = gs_create_texture_file_data3(path_of(sl->argv[1]), alpha_mode(sl->argv[2]), &f, &cx, &cy, &sp);
		if (!d) {
			printf("texfile null\n");
			return;
		}
		printf("texfile format=%s cx=%u cy=%u space=%d data=", fmt_name(f), cx, cy, (int)sp);
		hex(d, (size_t)cx * cy * gs_get_format_bpp(f) / 8);
		putchar('\n');
		bfree(d);
	} else if (IS("texfile_missing")) {
		enum gs_color_format f = GS_UNKNOWN;
		uint32_t cx = 0, cy = 0;
		uint8_t *d = gs_create_texture_file_data(path_of("does-not-exist.png"), &f, &cx, &cy);
		printf("texfile_missing %s\n", d ? "data" : "null");
		bfree(d);
		d = gs_create_texture_file_data(NULL, &f, &cx, &cy);
		printf("texfile_null %s\n", d ? "data" : "null");
		d = gs_create_texture_file_data("", &f, &cx, &cy);
		printf("texfile_empty %s\n", d ? "data" : "null");
	}
	else if (IS("reformat")) { /* reformat PIX W H LINESIZE ORIENT MODE HEX: a fabricated decoded frame */
		script_need(sl, 8);
		size_t n;
		uint8_t *b = script_parse_hex(sl->argv[7], &n);
		int w = atoi(sl->argv[2]), h = atoi(sl->argv[3]), ls = atoi(sl->argv[4]);
		if ((size_t)ls * h != n)
			die("reformat: hex length != LINESIZE*H");
		int f = 0, cx = 0, cy = 0;
		uint8_t *d = gfx_hook_reformat(sl->argv[1], w, h, ls, b, atoi(sl->argv[5]), alpha_mode(sl->argv[6]), &f, &cx, &cy);
		if (!d) {
			printf("reformat %s null\n", sl->argv[1]);
		} else {
			printf("reformat %s orient=%s %s -> format=%s cx=%d cy=%d data=", sl->argv[1], sl->argv[5], sl->argv[6],
			       fmt_name(f), cx, cy);
			hex(d, (size_t)cx * cy * gs_get_format_bpp(f) / 8);
			putchar('\n');
			bfree(d);
		}
		free(b);
	}
	/* ---- gs_image_file_ex ---- */
	else if (IS("img_init")) { /* img_init NAME MODE */
		ensure_gfx();
		script_need(sl, 3);
		gs_image_file_ex_init(&img, path_of(sl->argv[1]), alpha_mode(sl->argv[2]));
		print_img();
	} else if (IS("img_init_null")) {
		ensure_gfx();
		gs_image_file_ex_init(&img, NULL, GS_IMAGE_ALPHA_STRAIGHT);
		print_img();
	} else if (IS("img_try")) { /* img_try NAME MODE: init in a forked child that is killed after 2 s (hang/crash capture) */
		ensure_gfx();
		script_need(sl, 3);
		fflush(stdout);
		pid_t pid = fork();
		if (pid == 0) {
			alarm(2);
			base_set_crash_handler(child_crash_handler, NULL);
			gs_image_file_ex_init(&img, path_of(sl->argv[1]), alpha_mode(sl->argv[2]));
			print_img();
			fflush(stdout);
			_exit(0);
		}
		int st = 0;
		waitpid(pid, &st, 0);
		if (WIFSIGNALED(st))
			printf("img_try %s: %s\n", sl->argv[1], WTERMSIG(st) == SIGALRM ? "hang (no return within 2 s)" : "crash");
		else
			printf("img_try %s: %s\n", sl->argv[1], WEXITSTATUS(st) == 7 ? "terminated by the crash handler" : "returned");
	} else if (IS("img_state")) {
		print_img();
	} else if (IS("img_pixels")) { /* the still texture_data (non-animated) */
		if (img.texture_data) {
			printf("pixels ");
			hex(img.texture_data, (size_t)img.cx * img.cy * gs_get_format_bpp(img.format) / 8);
			putchar('\n');
		} else {
			puts("pixels none");
		}
	} else if (IS("img_tex_init")) {
		ensure_gfx();
		gs_image_file_ex_init_texture(&img);
		print_img();
	} else if (IS("img_tex")) {
		print_tex();
	} else if (IS("img_tick")) { /* img_tick NS [COUNT] */
		script_need(sl, 2);
		int count = sl->argc > 2 ? atoi(sl->argv[2]) : 1;
		for (int i = 0; i < count; i++) {
			bool r = gs_image_file_ex_tick(&img, strtoull(sl->argv[1], NULL, 10));
			printf("tick %s -> %d cur_frame=%d cur_loop=%d cur_time=%llu\n", sl->argv[1], r, img.cur_frame,
			       img.cur_loop, (unsigned long long)img.cur_time);
		}
	} else if (IS("img_update")) {
		gs_image_file_ex_update_texture(&img);
		puts("update_texture");
	} else if (IS("img_set_frame_updated")) {
		img.frame_updated = atoi(sl->argv[1]);
	} else if (IS("img_free")) {
		gs_image_file_ex_free(&img);
		print_img();
		printf("textures_alive=%d\n", fake_textures_alive);
	}
	/* ---- direct libnsgif ---- */
	else if (IS("gif_new")) { /* gif_new NAME [rgba|bgra|argb|abgr] */
		script_need(sl, 2);
		gif_load_file(sl->argv[1]);
		nsgif_bitmap_fmt_t f = NSGIF_BITMAP_FMT_R8G8B8A8;
		if (sl->argc > 2 && !strcmp(sl->argv[2], "bgra"))
			f = NSGIF_BITMAP_FMT_B8G8R8A8;
		if (sl->argc > 2 && !strcmp(sl->argv[2], "argb"))
			f = NSGIF_BITMAP_FMT_A8R8G8B8;
		if (sl->argc > 2 && !strcmp(sl->argv[2], "abgr"))
			f = NSGIF_BITMAP_FMT_A8B8G8R8;
		if (gif)
			nsgif_destroy(gif);
		nsgif_error r = nsgif_create(&cbs, f, &gif);
		printf("gif_new size=%zu -> %s\n", gif_size, nsgif_strerror(r));
	} else if (IS("gif_delay")) {
		nsgif_set_frame_delay_behaviour(gif, (uint16_t)atoi(sl->argv[1]), (uint16_t)atoi(sl->argv[2]));
		printf("gif_delay min=%s default=%s\n", sl->argv[1], sl->argv[2]);
	} else if (IS("gif_scan")) { /* gif_scan [LEN]: feed the first LEN bytes (default all) */
		size_t n = sl->argc > 1 ? (size_t)atol(sl->argv[1]) : gif_size;
		if (n > gif_size)
			n = gif_size;
		nsgif_error r = nsgif_data_scan(gif, n, gif_data);
		printf("gif_scan %zu -> %s\n", n, nsgif_strerror(r));
	} else if (IS("gif_complete")) {
		nsgif_data_complete(gif);
		puts("gif_complete");
	} else if (IS("gif_info")) {
		const nsgif_info_t *i = nsgif_get_info(gif);
		printf("gif_info width=%u height=%u frames=%u loop_max=%d background=%08x global_palette=%d\n", i->width,
		       i->height, i->frame_count, i->loop_max, i->background, i->global_palette);
		for (uint32_t k = 0; k < i->frame_count; k++) {
			const nsgif_frame_info_t *f = nsgif_get_frame_info(gif, k);
			printf("  frame %u display=%d transparency=%d local_palette=%d interlaced=%d disposal=%s(%u) "
			       "delay=%u rect=%u,%u,%u,%u\n",
			       k, f->display, f->transparency, f->local_palette, f->interlaced,
			       nsgif_str_disposal(f->disposal), f->disposal, f->delay, f->rect.x0, f->rect.y0, f->rect.x1,
			       f->rect.y1);
		}
		const nsgif_frame_info_t *bad = nsgif_get_frame_info(gif, i->frame_count);
		printf("  frame_info(frame_count) %s\n", bad ? "non-null" : "null");
	} else if (IS("gif_frame")) { /* gif_frame N */
		nsgif_bitmap_t *bm = NULL;
		uint32_t n = (uint32_t)atol(sl->argv[1]);
		nsgif_error r = nsgif_frame_decode(gif, n, &bm);
		printf("gif_frame %u -> %s", n, nsgif_strerror(r));
		const nsgif_info_t *i = nsgif_get_info(gif);
		if (r == NSGIF_OK && bm) {
			fputs(" pixels=", stdout);
			hex(bm, (size_t)i->width * i->height * 4);
		}
		putchar('\n');
	} else if (IS("gif_prepare")) { /* gif_prepare [COUNT] */
		int count = sl->argc > 1 ? atoi(sl->argv[1]) : 1;
		for (int k = 0; k < count; k++) {
			nsgif_rect_t a = {0};
			uint32_t delay = 12345, fn = 12345;
			nsgif_error r = nsgif_frame_prepare(gif, &a, &delay, &fn);
			printf("gif_prepare -> %s", nsgif_strerror(r));
			if (r == NSGIF_OK)
				printf(" area=%u,%u,%u,%u delay=%u frame=%u", a.x0, a.y0, a.x1, a.y1, delay, fn);
			putchar('\n');
		}
	} else if (IS("gif_reset")) {
		printf("gif_reset -> %s\n", nsgif_strerror(nsgif_reset(gif)));
	} else if (IS("gif_palette")) { /* gif_palette global | local N */
		uint32_t t[NSGIF_MAX_COLOURS] = {0};
		size_t n = 0;
		bool ok = true;
		if (!strcmp(sl->argv[1], "global"))
			nsgif_global_palette(gif, t, &n);
		else
			ok = nsgif_local_palette(gif, (uint32_t)atol(sl->argv[2]), t, &n);
		printf("gif_palette %s ok=%d entries=%zu", sl->argv[1], ok, n);
		for (size_t k = 0; k < n && k < 8; k++)
			printf(" %08x", t[k]);
		putchar('\n');
	} else if (IS("gif_destroy")) {
		nsgif_destroy(gif);
		gif = NULL;
		puts("gif_destroy");
	}
	/* ---- effects ---- */
	else if (IS("effect")) { /* effect PATH (repository relative) */
		script_need(sl, 2);
		cmd_effect(sl->argv[1], sl->argv[1]);
	} else {
		fprintf(stderr, "runner: unknown command '%s' at line %d\n", sl->argv[0], sl->lineno);
		exit(2);
	}
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	base_set_log_handler(log_handler, NULL);
	av_log_set_level(AV_LOG_QUIET);
	const char *t = getenv("CONFORMANCE_TMPDIR");
	snprintf(tmpdir, sizeof tmpdir, "%s", t ? t : "/tmp");
	/* the repository root (effect files are given relative to it); the working directory is the scratch directory */
	{
		const char *marker = "/test/conformance/gen/gfximages_runner.c";
		const char *at = strstr(__FILE__, marker);
		const char *env = getenv("OBS_REPO_ROOT");
		if (env)
			snprintf(repo_root, sizeof repo_root, "%s", env);
		else if (at && at != (const char *)__FILE__)
			snprintf(repo_root, sizeof repo_root, "%.*s", (int)(at - __FILE__), __FILE__);
		else
			snprintf(repo_root, sizeof repo_root, "../..");
		if (chdir(tmpdir))
			die("chdir CONFORMANCE_TMPDIR");
	}
	struct script_line sl;
	int lineno = 0;
	while (script_next(f, &sl, &lineno))
		run(&sl);
	fclose(f);
	return 0;
}
