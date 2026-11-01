/*
 * Conformance runner for area "graphics" (spec S-043): drives the real libobs/graphics/graphics.c (and
 * texture-render.c) against a scripted, tracing fake backend (graphics_backend*.c).  Command language:
 * see test/conformance/README.md, section "graphics runner".
 * Test tooling, not part of libobs.
 */
#include <pthread.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include "harness.h"
#include "graphics_backend.h"
#include "graphics/graphics.h"
#include "graphics/matrix4.h"
#include "graphics/vec2.h"
#include "graphics/vec3.h"
#include "graphics/vec4.h"
#include "graphics/quat.h"
#include "graphics/axisang.h"

extern void fake_state_reset(void);
extern const char *gs_preprocessor_name(void);

/* ---- symbols and tokens ---- */
static const struct {
	const char *n;
	long long v;
} syms[] = {
#include "graphics_syms.inc"
	{NULL, 0}};

static long long symval(const char *s)
{
	long long total = 0;
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", s);
	for (char *tok = strtok(buf, "|"); tok; tok = strtok(NULL, "|")) {
		bool found = false;
		for (int i = 0; syms[i].n; i++)
			if (!strcmp(syms[i].n, tok)) {
				total |= syms[i].v;
				found = true;
				break;
			}
		if (!found)
			total |= strtoll(tok, NULL, 0);
	}
	return total;
}

#define MAXTOK 256
static struct {
	char name[32];
	void *p;
} toks[MAXTOK];
static int ntoks;
static void *last_ptr;

static void tok_bind(const char *name, void *p)
{
	for (int i = 0; i < ntoks; i++)
		if (!strcmp(toks[i].name, name)) {
			toks[i].p = p;
			return;
		}
	if (ntoks >= MAXTOK) {
		fprintf(stderr, "too many tokens\n");
		exit(2);
	}
	snprintf(toks[ntoks].name, sizeof(toks[ntoks].name), "%s", name);
	toks[ntoks++].p = p;
}
static void *tok_get(const char *name, int lineno)
{
	if (!strcmp(name, "-"))
		return NULL;
	for (int i = 0; i < ntoks; i++)
		if (!strcmp(toks[i].name, name))
			return toks[i].p;
	fprintf(stderr, "line %d: unknown token '%s'\n", lineno, name);
	exit(2);
}

/* ---- result printing ---- */
static void out_bool(bool v) { printf("ret %s\n", v ? "true" : "false"); }
static void out_i(long long v) { printf("ret %lld\n", v); }
static void out_u(unsigned long long v) { printf("ret %llu\n", v); }
static void out_f(double v) { printf("ret %.9g\n", v); }
static void out_s(const char *s)
{
	printf("ret ");
	if (s)
		script_print_quoted(s);
	else
		printf("null");
	putchar('\n');
}
static void out_p(const void *p)
{
	last_ptr = (void *)p;
	if (!p)
		printf("ret null\n");
	else if (fh_lookup(p))
		printf("ret #%d\n", fh_id(p));
	else
		printf("ret ptr\n");
}
#define RES(e)                                                                                                    \
	do {                                                                                                      \
		__typeof__(e) r_ = (e);                                                                           \
		_Generic(r_, bool: out_bool, int: out_i, long: out_i, long long: out_i, unsigned: out_u,          \
			 unsigned long: out_u, unsigned long long: out_u, float: out_f, double: out_f,            \
			 const char *: out_s, char *: out_s, default: out_p)(r_);                                 \
	} while (0)

/* ---- argument access ---- */
static struct script_line *cl;
static int argc_;
static char **argv_;
static const char *A(int n)
{
	if (n >= argc_) {
		fprintf(stderr, "line %d: '%s' needs more arguments\n", cl->lineno, argv_[0]);
		exit(2);
	}
	return argv_[n];
}
#define I(n) ((int)symval(A(n)))
#define U(n) ((uint32_t)symval(A(n)))
#define F(n) ((float)strtod(A(n), NULL))
#define B(n) (!strcmp(A(n), "1") || !strcmp(A(n), "true"))
#define S(n) (strcmp(A(n), "-") ? A(n) : NULL)
#define P(n) tok_get(A(n), cl->lineno)

static graphics_t *gfx[16];
static int ngfx;
static int gfx_index(graphics_t *g)
{
	for (int i = 0; i < ngfx; i++)
		if (gfx[i] == g)
			return i;
	return -1;
}

static void print_matrix(const struct matrix4 *m)
{
	const float *f = (const float *)m;
	printf("matrix");
	for (int i = 0; i < 16; i++)
		printf(" %.9g", f[i]);
	putchar('\n');
}
static void read_matrix(struct matrix4 *m, int first)
{
	float *f = (float *)m;
	for (int i = 0; i < 16; i++)
		f[i] = F(first + i);
}

/* helpers producing deterministic buffers */
static uint8_t pattern[1 << 16];
static struct gs_vb_data *make_vbd(size_t n, bool norm, bool col, int ntex)
{
	struct gs_vb_data *d = gs_vbdata_create();
	d->num = n;
	d->points = bzalloc(sizeof(struct vec3) * (n ? n : 1));
	for (size_t i = 0; i < n; i++)
		vec3_set(&d->points[i], (float)i, (float)i * 2, (float)i * 3);
	if (norm) {
		d->normals = bzalloc(sizeof(struct vec3) * (n ? n : 1));
		for (size_t i = 0; i < n; i++)
			vec3_set(&d->normals[i], 0, 0, 1);
	}
	if (col) {
		d->colors = bzalloc(sizeof(uint32_t) * (n ? n : 1));
		for (size_t i = 0; i < n; i++)
			d->colors[i] = 0xff000000u | (uint32_t)i;
	}
	d->num_tex = (size_t)ntex;
	if (ntex) {
		d->tvarray = bzalloc(sizeof(struct gs_tvertarray) * ntex);
		for (int t = 0; t < ntex; t++) {
			d->tvarray[t].width = 2;
			struct vec2 *a = bzalloc(sizeof(struct vec2) * (n ? n : 1));
			for (size_t i = 0; i < n; i++)
				vec2_set(&a[i], (float)i * 0.5f, (float)t);
			d->tvarray[t].array = a;
		}
	}
	return d;
}

static bool enum_cb(void *param, const char *name, uint32_t id)
{
	(void)param;
	printf("  adapter %u ", id);
	script_print_quoted(name);
	putchar('\n');
	return true;
}

/* ---- the command interpreter ---- */
static int run_line(struct script_line *sl);

static void report_child(int status)
{
	if (WIFEXITED(status))
		printf("child exit=%d\n", WEXITSTATUS(status));
	else if (WIFSIGNALED(status))
		printf("child signal=%d\n", WTERMSIG(status));
}

struct thread_job {
	struct script_line sl[8];
	int n;
};
static void *thread_main(void *arg)
{
	struct thread_job *j = arg;
	for (int i = 0; i < j->n; i++)
		run_line(&j->sl[i]);
	return NULL;
}

static int run_line(struct script_line *sl)
{
	struct script_line *saved_cl = cl;
	int saved_argc = argc_;
	char **saved_argv = argv_;
	cl = sl;
	argc_ = sl->argc;
	argv_ = sl->argv;
	const char *c = argv_[0];
	const char *bind = NULL;
	if (argc_ >= 3 && !strcmp(argv_[argc_ - 2], ">")) {
		bind = argv_[argc_ - 1];
		argc_ -= 2;
	}
	last_ptr = NULL;
	int rc = 0;

#define C(name) else if (!strcmp(c, #name))
	if (!strcmp(c, "echo")) {
		for (int i = 1; i < argc_; i++)
			printf("%s%s", i > 1 ? " " : "", argv_[i]);
		putchar('\n');
	}
	/* ---- backend scripting ---- */
	C(ret) { fake_set_ret(A(1), A(2)); }
	C(fail) { fake_set_fail(A(1), B(2)); }
	C(same) { printf("same %s\n", P(1) == P(2) ? "true" : "false"); }
	C(failnth) { fake_set_fail_nth(A(1), atoi(A(2))); }
	C(missing) { fake_set_missing(A(1), B(2)); }
	C(size) { fake_w = U(1), fake_h = U(2); }
	C(adapter) { fake_add_adapter(A(1)); }
	C(file) { fake_add_file(A(1), A(2)); }
	C(dump) { fake_dump_full = !strcmp(A(1), "full"); }
	C(trace) { tr_enabled = B(1); }
	C(reset_backend)
	{
		fake_reset_script();
		fake_state_reset();
	}
	C(reset_handles) { fh_reset(); ntoks = 0; }
	/* ---- test-harness helpers ---- */
	C(crash)
	{ /* crash CMD ARGS...: run the command in a forked child and report how it ended */
		fflush(stdout);
		pid_t pid = fork();
		if (pid == 0) {
			alarm(2); /* a hang is reported as signal 14 (SIGALRM) */
			struct script_line sub = *sl;
			sub.argc = sl->argc - 1;
			for (int i = 0; i < sub.argc; i++)
				sub.argv[i] = sl->argv[i + 1];
			run_line(&sub);
			fflush(stdout);
			_exit(0);
		}
		int st = 0;
		waitpid(pid, &st, 0);
		report_child(st);
	}
	C(thread)
	{ /* thread CMD... ; CMD... : run the commands, in order, on a fresh thread and join it */
		struct thread_job *j = calloc(1, sizeof(*j));
		int cur = 0, start = 1;
		for (int i = 1; i <= sl->argc; i++) {
			if (i == sl->argc || !strcmp(sl->argv[i], ";")) {
				struct script_line *t = &j->sl[cur++];
				t->lineno = sl->lineno;
				t->argc = i - start;
				for (int k = 0; k < t->argc; k++)
					t->argv[k] = sl->argv[start + k];
				start = i + 1;
			}
		}
		j->n = cur;
		fflush(stdout);
		pthread_t th;
		pthread_create(&th, NULL, thread_main, j);
		pthread_join(th, NULL);
		free(j);
	}
	/* ---- context ---- */
	C(gs_create)
	{ /* gs_create G [MODULE] [ADAPTER] */
		graphics_t *g = NULL;
		int r = gs_create(&g, argc_ > 2 ? A(2) : "fake", argc_ > 3 ? U(3) : 0);
		printf("ret %d %s\n", r, g ? "created" : "null");
		if (g) {
			gfx[ngfx++] = g;
			tok_bind(A(1), g);
		}
	}
	C(gs_destroy)
	{
		graphics_t *g = P(1);
		gs_destroy(g);
		int i = gfx_index(g);
		if (i >= 0)
			gfx[i] = NULL;
		tok_bind(A(1), NULL);
	}
	C(gs_enter_context) { gs_enter_context(P(1)); }
	C(gs_leave_context) { gs_leave_context(); }
	C(gs_get_context)
	{
		graphics_t *g = gs_get_context();
		printf("ret %s\n", !g ? "none" : "G");
		if (g) {
			for (int i = 0; i < MAXTOK && i < ntoks; i++)
				if (toks[i].p == g) {
					printf("ret is %s\n", toks[i].name);
					break;
				}
		}
	}
	C(gs_get_device_obj) { RES(gs_get_device_obj()); }
	C(gs_get_device_name) { RES(gs_get_device_name()); }
	C(gs_get_driver_version) { RES(gs_get_driver_version()); }
	C(gs_get_renderer) { RES(gs_get_renderer()); }
	C(gs_get_gpu_dmem) { RES(gs_get_gpu_dmem()); }
	C(gs_get_gpu_smem) { RES(gs_get_gpu_smem()); }
	C(gs_get_device_type) { RES(gs_get_device_type()); }
	C(gs_preprocessor_name) { RES(gs_preprocessor_name()); }
	C(gs_get_adapter_count) { RES(gs_get_adapter_count()); }
	C(gs_enum_adapters) { gs_enum_adapters(argc_ > 1 && !strcmp(A(1), "nullcb") ? NULL : enum_cb, NULL); }
	/* ---- matrix stack ---- */
	C(gs_matrix_push) { gs_matrix_push(); }
	C(gs_matrix_pop) { gs_matrix_pop(); }
	C(gs_matrix_identity) { gs_matrix_identity(); }
	C(gs_matrix_transpose) { gs_matrix_transpose(); }
	C(gs_matrix_set)
	{
		struct matrix4 m;
		read_matrix(&m, 1);
		gs_matrix_set(&m);
	}
	C(gs_matrix_mul)
	{
		struct matrix4 m;
		read_matrix(&m, 1);
		gs_matrix_mul(&m);
	}
	C(gs_matrix_get)
	{
		struct matrix4 m;
		memset(&m, 0, sizeof(m));
		gs_matrix_get(&m);
		print_matrix(&m);
	}
	C(gs_matrix_translate3f) { gs_matrix_translate3f(F(1), F(2), F(3)); }
	C(gs_matrix_scale3f) { gs_matrix_scale3f(F(1), F(2), F(3)); }
	C(gs_matrix_rotaa4f) { gs_matrix_rotaa4f(F(1), F(2), F(3), F(4)); }
	C(gs_matrix_translate)
	{
		struct vec3 v;
		vec3_set(&v, F(1), F(2), F(3));
		gs_matrix_translate(&v);
	}
	C(gs_matrix_scale)
	{
		struct vec3 v;
		vec3_set(&v, F(1), F(2), F(3));
		gs_matrix_scale(&v);
	}
	C(gs_matrix_rotquat)
	{
		struct quat q;
		quat_set(&q, F(1), F(2), F(3), F(4));
		gs_matrix_rotquat(&q);
	}
	C(gs_matrix_rotaa)
	{
		struct axisang a;
		axisang_set(&a, F(1), F(2), F(3), F(4));
		gs_matrix_rotaa(&a);
	}
	/* ---- immediate / retained vertex building ---- */
	C(gs_render_start) { gs_render_start(B(1)); }
	C(gs_render_stop) { gs_render_stop(I(1)); }
	C(gs_render_save) { RES(gs_render_save()); }
	C(gs_vertex2f) { gs_vertex2f(F(1), F(2)); }
	C(gs_vertex3f) { gs_vertex3f(F(1), F(2), F(3)); }
	C(gs_normal3f) { gs_normal3f(F(1), F(2), F(3)); }
	C(gs_color) { gs_color(U(1)); }
	C(gs_texcoord) { gs_texcoord(F(1), F(2), I(3)); }
	C(gs_vertex2v)
	{
		struct vec2 v;
		vec2_set(&v, F(1), F(2));
		gs_vertex2v(&v);
	}
	C(gs_vertex3v)
	{
		struct vec3 v;
		vec3_set(&v, F(1), F(2), F(3));
		gs_vertex3v(&v);
	}
	C(gs_normal3v)
	{
		struct vec3 v;
		vec3_set(&v, F(1), F(2), F(3));
		gs_normal3v(&v);
	}
	C(gs_texcoord2v)
	{
		struct vec2 v;
		vec2_set(&v, F(1), F(2));
		gs_texcoord2v(&v, I(3));
	}
	C(gs_color4v)
	{
		struct vec4 v;
		vec4_set(&v, F(1), F(2), F(3), F(4));
		gs_color4v(&v);
	}
	C(gs_get_input) { RES(gs_get_input()); }
	C(gs_get_effect) { RES(gs_get_effect()); }
	/* ---- effects / shaders from files ---- */
	C(gs_effect_create_from_file) { RES(gs_effect_create_from_file(S(1), NULL)); }
	C(gs_vertexshader_create_from_file)
	{
		char *err = NULL;
		RES(gs_vertexshader_create_from_file(S(1), &err));
		if (err) {
			printf("error ");
			script_print_quoted(err);
			putchar('\n');
			bfree(err);
		}
	}
	C(gs_pixelshader_create_from_file)
	{
		char *err = NULL;
		RES(gs_pixelshader_create_from_file(S(1), &err));
		if (err) {
			printf("error ");
			script_print_quoted(err);
			putchar('\n');
			bfree(err);
		}
	}
	C(gs_vertexshader_create)
	{
		char *err = NULL;
		RES(gs_vertexshader_create(S(1), S(2), &err));
		if (err) {
			printf("error ");
			script_print_quoted(err);
			putchar('\n');
			bfree(err);
		}
	}
	C(gs_pixelshader_create) { RES(gs_pixelshader_create(S(1), S(2), NULL)); }
	/* ---- drawing helpers ---- */
	C(gs_draw_quadf) { gs_draw_quadf(P(1), U(2), F(3), F(4)); }
	C(gs_draw_sprite) { gs_draw_sprite(P(1), U(2), U(3), U(4)); }
	C(gs_draw_sprite_subregion) { gs_draw_sprite_subregion(P(1), U(2), U(3), U(4), U(5), U(6)); }
	C(gs_draw_cube_backdrop) { gs_draw_cube_backdrop(P(1), NULL, 0, 0, 0, 0, 0); }
	C(gs_reset_viewport) { gs_reset_viewport(); }
	C(gs_set_2d_mode) { gs_set_2d_mode(); }
	C(gs_set_3d_mode) { gs_set_3d_mode(1, 1, 1); }
	C(gs_viewport_push) { gs_viewport_push(); }
	C(gs_viewport_pop) { gs_viewport_pop(); }
	C(gs_perspective) { gs_perspective(F(1), F(2), F(3), F(4)); }
	C(gs_blend_state_push) { gs_blend_state_push(); }
	C(gs_blend_state_pop) { gs_blend_state_pop(); }
	C(gs_reset_blend_state) { gs_reset_blend_state(); }
	/* ---- swapchain / device size ---- */
	C(gs_swapchain_create)
	{ /* gs_swapchain_create CX CY NUM_BACKBUFFERS FORMAT ZSFORMAT ADAPTER  |  gs_swapchain_create null */
		struct gs_init_data d;
		memset(&d, 0, sizeof(d));
		if (!strcmp(A(1), "null")) {
			RES(gs_swapchain_create(NULL));
		} else {
			d.cx = U(1), d.cy = U(2), d.num_backbuffers = U(3), d.format = I(4), d.zsformat = I(5),
			d.adapter = U(6);
			RES(gs_swapchain_create(&d));
		}
	}
	C(gs_swapchain_destroy) { gs_swapchain_destroy(P(1)); }
	C(gs_resize) { gs_resize(U(1), U(2)); }
	C(gs_update_color_space) { gs_update_color_space(); }
	C(gs_get_size)
	{
		uint32_t x = 0xdead, y = 0xbeef;
		gs_get_size(&x, &y);
		printf("size %u %u\n", x, y);
	}
	C(gs_get_width) { RES(gs_get_width()); }
	C(gs_get_height) { RES(gs_get_height()); }
	/* ---- resource creation ---- */
	C(gs_texture_create)
	{ /* gs_texture_create W H FMT LEVELS FLAGS [data] */
		const uint8_t *d = pattern;
		RES(gs_texture_create(U(1), U(2), I(3), U(4), argc_ > 6 && !strcmp(A(6), "data") ? &d : NULL, U(5)));
	}
	C(gs_texture_create_from_file) { RES(gs_texture_create_from_file(S(1))); }
	C(gs_cubetexture_create)
	{
		const uint8_t *d = pattern;
		RES(gs_cubetexture_create(U(1), I(2), U(3), argc_ > 5 && !strcmp(A(5), "data") ? &d : NULL, U(4)));
	}
	C(gs_voltexture_create) { RES(gs_voltexture_create(U(1), U(2), U(3), I(4), U(5), NULL, U(6))); }
	C(gs_zstencil_create) { RES(gs_zstencil_create(U(1), U(2), I(3))); }
	C(gs_stagesurface_create) { RES(gs_stagesurface_create(U(1), U(2), I(3))); }
	C(gs_samplerstate_create)
	{
		struct gs_sampler_info si = {I(1), I(2), I(3), I(4), I(5), U(6)};
		RES(gs_samplerstate_create(&si));
	}
	C(gs_samplerstate_create_null) { RES(gs_samplerstate_create(NULL)); }
	C(gs_vertexbuffer_create)
	{ /* gs_vertexbuffer_create N FLAGS NORM COL NTEX | gs_vertexbuffer_create null FLAGS */
		if (!strcmp(A(1), "null")) {
			RES(gs_vertexbuffer_create(NULL, U(2)));
		} else {
			struct gs_vb_data *d = make_vbd((size_t)atoi(A(1)), B(3), B(4), atoi(A(5)));
			uint32_t flags = U(2);
			gs_vertbuffer_t *vb = gs_vertexbuffer_create(d, flags);
			RES(vb);
			if (!gs_get_context())
				gs_vbdata_destroy(d); /* never reached the backend */
			if ((flags & GS_DUP_BUFFER) && d->num)
				gs_vbdata_destroy(d); /* the frontend copied it: the caller still owns the original */
		}
	}
	C(gs_indexbuffer_create)
	{ /* gs_indexbuffer_create TYPE N FLAGS */
		int type = I(1);
		size_t n = (size_t)atoi(A(2));
		uint32_t flags = U(3);
		size_t sz = type == GS_UNSIGNED_SHORT ? 2 : 4;
		void *idx = bzalloc(sz * (n ? n : 1));
		for (size_t i = 0; i < n; i++) {
			if (sz == 2)
				((uint16_t *)idx)[i] = (uint16_t)(i * 3);
			else
				((uint32_t *)idx)[i] = (uint32_t)(i * 3);
		}
		RES(gs_indexbuffer_create(type, idx, n, flags));
		if (!gs_get_context() || ((flags & GS_DUP_BUFFER) && n))
			bfree(idx); /* otherwise the backend owns it */
	}
	C(gs_timer_create) { RES(gs_timer_create()); }
	C(gs_timer_range_create) { RES(gs_timer_range_create()); }
	C(gs_texture_create_nv12)
	{
		gs_texture_t *y = (gs_texture_t *)0x1, *uv = (gs_texture_t *)0x1;
		bool r = gs_texture_create_nv12(&y, &uv, U(1), U(2), U(3));
		printf("ret %s y=%d uv=%d\n", r ? "true" : "false", y == (void *)0x1 ? -2 : fh_id(y),
		       uv == (void *)0x1 ? -2 : fh_id(uv));
	}
	C(gs_texture_create_p010)
	{
		gs_texture_t *y = (gs_texture_t *)0x1, *uv = (gs_texture_t *)0x1;
		bool r = gs_texture_create_p010(&y, &uv, U(1), U(2), U(3));
		printf("ret %s y=%d uv=%d\n", r ? "true" : "false", y == (void *)0x1 ? -2 : fh_id(y),
		       uv == (void *)0x1 ? -2 : fh_id(uv));
	}
	C(gs_nv12_available) { RES(gs_nv12_available()); }
	C(gs_p010_available) { RES(gs_p010_available()); }
	C(gs_is_monitor_hdr) { RES(gs_is_monitor_hdr(NULL)); }
	/* ---- destruction ---- */
	C(gs_texture_destroy) { gs_texture_destroy(P(1)); }
	C(gs_cubetexture_destroy) { gs_cubetexture_destroy(P(1)); }
	C(gs_voltexture_destroy) { gs_voltexture_destroy(P(1)); }
	C(gs_stagesurface_destroy) { gs_stagesurface_destroy(P(1)); }
	C(gs_zstencil_destroy) { gs_zstencil_destroy(P(1)); }
	C(gs_samplerstate_destroy) { gs_samplerstate_destroy(P(1)); }
	C(gs_vertexbuffer_destroy) { gs_vertexbuffer_destroy(P(1)); }
	C(gs_indexbuffer_destroy) { gs_indexbuffer_destroy(P(1)); }
	C(gs_timer_destroy) { gs_timer_destroy(P(1)); }
	C(gs_timer_range_destroy) { gs_timer_range_destroy(P(1)); }
	C(gs_shader_destroy) { gs_shader_destroy(P(1)); }
	/* ---- object queries ---- */
	C(gs_get_texture_type) { RES(gs_get_texture_type(P(1))); }
	C(gs_texture_get_width) { RES(gs_texture_get_width(P(1))); }
	C(gs_texture_get_height) { RES(gs_texture_get_height(P(1))); }
	C(gs_texture_get_color_format) { RES(gs_texture_get_color_format(P(1))); }
	C(gs_texture_is_rect) { RES(gs_texture_is_rect(P(1))); }
	C(gs_texture_get_obj) { RES(gs_texture_get_obj(P(1))); }
	C(gs_texture_map)
	{
		uint8_t *p = NULL;
		uint32_t ls = 0;
		bool r = gs_texture_map(P(1), &p, &ls);
		printf("ret %s linesize=%u %s\n", r ? "true" : "false", ls, p ? "mapped" : "null");
	}
	C(gs_texture_unmap) { gs_texture_unmap(P(1)); }
	C(gs_cubetexture_get_size) { RES(gs_cubetexture_get_size(P(1))); }
	C(gs_cubetexture_get_color_format) { RES(gs_cubetexture_get_color_format(P(1))); }
	C(gs_voltexture_get_width) { RES(gs_voltexture_get_width(P(1))); }
	C(gs_voltexture_get_height) { RES(gs_voltexture_get_height(P(1))); }
	C(gs_voltexture_get_depth) { RES(gs_voltexture_get_depth(P(1))); }
	C(gs_voltexture_get_color_format) { RES(gs_voltexture_get_color_format(P(1))); }
	C(gs_stagesurface_get_width) { RES(gs_stagesurface_get_width(P(1))); }
	C(gs_stagesurface_get_height) { RES(gs_stagesurface_get_height(P(1))); }
	C(gs_stagesurface_get_color_format) { RES(gs_stagesurface_get_color_format(P(1))); }
	C(gs_stagesurface_map)
	{
		uint8_t *p = NULL;
		uint32_t ls = 0;
		bool r = gs_stagesurface_map(P(1), &p, &ls);
		printf("ret %s linesize=%u %s\n", r ? "true" : "false", ls, p ? "mapped" : "null");
	}
	C(gs_stagesurface_unmap) { gs_stagesurface_unmap(P(1)); }
	C(gs_vertexbuffer_get_data)
	{
		struct gs_vb_data *d = gs_vertexbuffer_get_data(P(1));
		printf("ret %s num=%zu\n", d ? "data" : "null", d ? d->num : 0);
	}
	C(gs_vertexbuffer_flush) { gs_vertexbuffer_flush(P(1)); }
	C(gs_vertexbuffer_flush_direct) { gs_vertexbuffer_flush_direct(P(1), NULL); }
	C(gs_indexbuffer_flush) { gs_indexbuffer_flush(P(1)); }
	C(gs_indexbuffer_flush_direct) { gs_indexbuffer_flush_direct(P(1), NULL); }
	C(gs_indexbuffer_get_data) { void *d = gs_indexbuffer_get_data(P(1)); printf("ret %s\n", d ? "data" : "null"); }
	C(gs_indexbuffer_get_num_indices) { RES(gs_indexbuffer_get_num_indices(P(1))); }
	C(gs_indexbuffer_get_type) { RES(gs_indexbuffer_get_type(P(1))); }
	C(gs_timer_begin) { gs_timer_begin(P(1)); }
	C(gs_timer_end) { gs_timer_end(P(1)); }
	C(gs_timer_get_data)
	{
		uint64_t t = 7;
		bool r = gs_timer_get_data(P(1), &t);
		printf("ret %s ticks=%llu\n", r ? "true" : "false", (unsigned long long)t);
	}
	C(gs_timer_range_begin) { gs_timer_range_begin(P(1)); }
	C(gs_timer_range_end) { gs_timer_range_end(P(1)); }
	C(gs_timer_range_get_data)
	{
		bool dj = true;
		uint64_t fq = 9;
		bool r = gs_timer_range_get_data(P(1), &dj, &fq);
		printf("ret %s disjoint=%d freq=%llu\n", r ? "true" : "false", dj, (unsigned long long)fq);
	}
	C(gs_texture_set_image)
	{ /* gs_texture_set_image TEX LINESIZE FLIP  (source = pattern bytes i%251) */
		static uint8_t src[1 << 14];
		for (size_t i = 0; i < sizeof(src); i++)
			src[i] = (uint8_t)(i % 251);
		gs_texture_set_image(P(1), argc_ > 4 && !strcmp(A(4), "nulldata") ? NULL : src, U(2), B(3));
	}
	/* ---- loading / current state ---- */
	C(gs_load_vertexbuffer) { gs_load_vertexbuffer(P(1)); }
	C(gs_load_indexbuffer) { gs_load_indexbuffer(P(1)); }
	C(gs_load_texture) { gs_load_texture(P(1), I(2)); }
	C(gs_load_samplerstate) { gs_load_samplerstate(P(1), I(2)); }
	C(gs_load_vertexshader) { gs_load_vertexshader(P(1)); }
	C(gs_load_pixelshader) { gs_load_pixelshader(P(1)); }
	C(gs_load_default_samplerstate) { gs_load_default_samplerstate(B(1), I(2)); }
	C(gs_get_vertex_shader) { RES(gs_get_vertex_shader()); }
	C(gs_get_pixel_shader) { RES(gs_get_pixel_shader()); }
	C(gs_get_color_space) { RES(gs_get_color_space()); }
	C(gs_get_render_target) { RES(gs_get_render_target()); }
	C(gs_get_zstencil_target) { RES(gs_get_zstencil_target()); }
	C(gs_set_render_target) { gs_set_render_target(P(1), P(2)); }
	C(gs_set_render_target_with_color_space) { gs_set_render_target_with_color_space(P(1), P(2), I(3)); }
	C(gs_set_cube_render_target) { gs_set_cube_render_target(P(1), I(2), P(3)); }
	C(gs_enable_framebuffer_srgb) { gs_enable_framebuffer_srgb(B(1)); }
	C(gs_framebuffer_srgb_enabled) { RES(gs_framebuffer_srgb_enabled()); }
	C(gs_get_linear_srgb) { RES(gs_get_linear_srgb()); }
	C(gs_set_linear_srgb) { RES(gs_set_linear_srgb(B(1))); }
	C(gs_copy_texture) { gs_copy_texture(P(1), P(2)); }
	C(gs_copy_texture_region) { gs_copy_texture_region(P(1), U(2), U(3), P(4), U(5), U(6), U(7), U(8)); }
	C(gs_stage_texture) { gs_stage_texture(P(1), P(2)); }
	/* ---- frame ---- */
	C(gs_begin_frame) { gs_begin_frame(); }
	C(gs_begin_scene) { gs_begin_scene(); }
	C(gs_draw) { gs_draw(I(1), U(2), U(3)); }
	C(gs_end_scene) { gs_end_scene(); }
	C(gs_load_swapchain) { gs_load_swapchain(P(1)); }
	C(gs_clear)
	{ /* gs_clear FLAGS R G B A DEPTH STENCIL | gs_clear FLAGS null DEPTH STENCIL */
		if (!strcmp(A(2), "null")) {
			gs_clear(U(1), NULL, F(3), (uint8_t)U(4));
		} else {
			struct vec4 col;
			vec4_set(&col, F(2), F(3), F(4), F(5));
			gs_clear(U(1), &col, F(6), (uint8_t)U(7));
		}
	}
	C(gs_is_present_ready) { RES(gs_is_present_ready()); }
	C(gs_present) { gs_present(); }
	C(gs_flush) { gs_flush(); }
	/* ---- pipeline state ---- */
	C(gs_set_cull_mode) { gs_set_cull_mode(I(1)); }
	C(gs_get_cull_mode) { RES(gs_get_cull_mode()); }
	C(gs_enable_blending) { gs_enable_blending(B(1)); }
	C(gs_enable_depth_test) { gs_enable_depth_test(B(1)); }
	C(gs_enable_stencil_test) { gs_enable_stencil_test(B(1)); }
	C(gs_enable_stencil_write) { gs_enable_stencil_write(B(1)); }
	C(gs_enable_color) { gs_enable_color(B(1), B(2), B(3), B(4)); }
	C(gs_blend_function) { gs_blend_function(I(1), I(2)); }
	C(gs_blend_function_separate) { gs_blend_function_separate(I(1), I(2), I(3), I(4)); }
	C(gs_blend_op) { gs_blend_op(I(1)); }
	C(gs_depth_function) { gs_depth_function(I(1)); }
	C(gs_stencil_function) { gs_stencil_function(I(1), I(2)); }
	C(gs_stencil_op) { gs_stencil_op(I(1), I(2), I(3), I(4)); }
	C(gs_set_viewport) { gs_set_viewport(I(1), I(2), I(3), I(4)); }
	C(gs_get_viewport)
	{
		struct gs_rect r = {-1, -1, -1, -1};
		if (argc_ > 1 && !strcmp(A(1), "null"))
			gs_get_viewport(NULL);
		else {
			gs_get_viewport(&r);
			printf("viewport %d %d %d %d\n", r.x, r.y, r.cx, r.cy);
		}
	}
	C(gs_set_scissor_rect)
	{
		if (argc_ > 1 && !strcmp(A(1), "null"))
			gs_set_scissor_rect(NULL);
		else {
			struct gs_rect r = {I(1), I(2), I(3), I(4)};
			gs_set_scissor_rect(&r);
		}
	}
	C(gs_ortho) { gs_ortho(F(1), F(2), F(3), F(4), F(5), F(6)); }
	C(gs_frustum) { gs_frustum(F(1), F(2), F(3), F(4), F(5), F(6)); }
	C(gs_projection_push) { gs_projection_push(); }
	C(gs_projection_pop) { gs_projection_pop(); }
	/* ---- shader parameters ---- */
	C(gs_shader_get_num_params) { RES(gs_shader_get_num_params(P(1))); }
	C(gs_shader_get_param_by_idx) { RES(gs_shader_get_param_by_idx(P(1), U(2))); }
	C(gs_shader_get_param_by_name) { RES(gs_shader_get_param_by_name(P(1), S(2))); }
	C(gs_shader_get_viewproj_matrix) { RES(gs_shader_get_viewproj_matrix(P(1))); }
	C(gs_shader_get_world_matrix) { RES(gs_shader_get_world_matrix(P(1))); }
	C(gs_shader_get_param_info)
	{
		struct gs_shader_param_info info = {GS_SHADER_PARAM_UNKNOWN, NULL};
		if (argc_ > 2 && !strcmp(A(2), "null"))
			gs_shader_get_param_info(P(1), NULL);
		else {
			gs_shader_get_param_info(P(1), &info);
			printf("info type=%d name=%s\n", info.type, info.name ? info.name : "null");
		}
	}
	C(gs_shader_set_bool) { gs_shader_set_bool(P(1), B(2)); }
	C(gs_shader_set_float) { gs_shader_set_float(P(1), F(2)); }
	C(gs_shader_set_int) { gs_shader_set_int(P(1), I(2)); }
	C(gs_shader_set_matrix4)
	{
		struct matrix4 m;
		read_matrix(&m, 2);
		gs_shader_set_matrix4(P(1), &m);
	}
	C(gs_shader_set_matrix3)
	{
		struct matrix3 m;
		memset(&m, 0, sizeof(m));
		gs_shader_set_matrix3(P(1), &m);
	}
	C(gs_shader_set_vec2)
	{
		struct vec2 v;
		vec2_set(&v, F(2), F(3));
		gs_shader_set_vec2(P(1), &v);
	}
	C(gs_shader_set_vec3)
	{
		struct vec3 v;
		vec3_set(&v, F(2), F(3), F(4));
		gs_shader_set_vec3(P(1), &v);
	}
	C(gs_shader_set_vec4)
	{
		struct vec4 v;
		vec4_set(&v, F(2), F(3), F(4), F(5));
		gs_shader_set_vec4(P(1), &v);
	}
	C(gs_shader_set_vec2_null) { gs_shader_set_vec2(P(1), NULL); }
	C(gs_shader_set_texture) { gs_shader_set_texture(P(1), P(2)); }
	C(gs_shader_set_val)
	{
		size_t n;
		uint8_t *b = script_parse_hex(A(2), &n);
		gs_shader_set_val(P(1), b, n);
		free(b);
	}
	C(gs_shader_set_default) { gs_shader_set_default(P(1)); }
	C(gs_shader_set_next_sampler) { gs_shader_set_next_sampler(P(1), P(2)); }
	/* ---- debug markers ---- */
	C(gs_debug_marker_begin)
	{
		float col[4] = {F(1), F(2), F(3), F(4)};
		gs_debug_marker_begin(col, S(5));
	}
	C(gs_debug_marker_begin_format)
	{ /* gs_debug_marker_begin_format NAME (printf-style, one %s allowed) ARG */
		float col[4] = {0.5f, 0.5f, 0.5f, 1.0f};
		if (!strcmp(A(1), "null"))
			gs_debug_marker_begin_format(col, NULL);
		else
			gs_debug_marker_begin_format(col, A(1), argc_ > 2 ? A(2) : "");
	}
	C(gs_debug_marker_end) { gs_debug_marker_end(); }
	/* ---- texrender ---- */
	C(gs_texrender_create) { RES(gs_texrender_create(I(1), I(2))); }
	C(gs_texrender_destroy) { gs_texrender_destroy(P(1)); }
	C(gs_texrender_begin) { RES(gs_texrender_begin(P(1), U(2), U(3))); }
	C(gs_texrender_begin_with_color_space) { RES(gs_texrender_begin_with_color_space(P(1), U(2), U(3), I(4))); }
	C(gs_texrender_end) { gs_texrender_end(P(1)); }
	C(gs_texrender_reset) { gs_texrender_reset(P(1)); }
	C(gs_texrender_get_texture) { RES(gs_texrender_get_texture(P(1))); }
	C(gs_texrender_get_format) { RES(gs_texrender_get_format(P(1))); }
	/* ---- Linux-only dmabuf / sync wrappers ---- */
	C(gs_query_sync_capabilities) { RES(gs_query_sync_capabilities()); }
	C(gs_sync_create) { RES(gs_sync_create()); }
	C(gs_sync_destroy) { gs_sync_destroy(P(1)); }
	C(gs_sync_wait) { RES(gs_sync_wait(P(1))); }
	C(gs_sync_signal_syncobj_timeline_point) { RES(gs_sync_signal_syncobj_timeline_point(I(1), (uint64_t)atoll(A(2)))); }
	C(gs_texture_create_from_dmabuf)
	{
		int fd = 5;
		uint32_t stride = 16, off = 0;
		uint64_t mod = 0;
		RES(gs_texture_create_from_dmabuf(U(1), U(2), U(3), I(4), 1, &fd, &stride, &off, &mod));
	}
	/* ---- header-inline helpers that plugins compile in ---- */
	C(inline_format_info)
	{
		int f = I(1);
		printf("format %d bpp=%u compressed=%d srgb=%d generalized=%d\n", f, gs_get_format_bpp(f),
		       gs_is_compressed_format(f), gs_is_srgb_format(f), gs_generalize_format(f));
	}
	C(inline_format_from_space) { RES(gs_get_format_from_space(I(1))); }
	C(inline_total_levels) { RES(gs_get_total_levels(U(1), U(2), U(3))); }
	else
	{
		fprintf(stderr, "line %d: unknown command '%s'\n", sl->lineno, c);
		rc = 2;
	}
#undef C
	if (bind)
		tok_bind(bind, last_ptr);
	cl = saved_cl;
	argc_ = saved_argc;
	argv_ = saved_argv;
	return rc;
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	for (size_t i = 0; i < sizeof(pattern); i++)
		pattern[i] = (uint8_t)(i % 251);
	while (script_next(f, &sl, &lineno)) {
		if (strcmp(sl.argv[0], "echo")) {
			printf("> ");
			for (int i = 0; i < sl.argc; i++)
			{
				if (i)
					putchar(' ');
				if (strpbrk(sl.argv[i], " \t\n\r\"")) {
					script_print_quoted(sl.argv[i]);
				} else
					fputs(sl.argv[i], stdout);
			}
			putchar('\n');
		}
		if (run_line(&sl) != 0)
			return 2;
	}
	return 0;
}
