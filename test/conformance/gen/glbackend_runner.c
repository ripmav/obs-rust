/*
 * Conformance runner for the OpenGL graphics backend (libobs-opengl), spec S-057.
 *
 * The real backend sources (device_* / gs_* exports, X11/EGL glue, GLSL shader translation) are
 * linked and driven against a real headless OpenGL context: an Xvfb display (started by the
 * runner if DISPLAY is unset) and Mesa software rendering.  Results are read back through
 * stage surfaces.  Script language: see the "glbackend runner" section of
 * test/conformance/README.md.  Test tooling, not part of libobs.
 */
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/prctl.h>
#include <math.h>

#include "util/base.h"
#include "util/bmem.h"
#include "graphics/graphics.h"
#include "graphics/device-exports.h"
#include "graphics/matrix4.h"
#include "graphics/matrix3.h"
#include "graphics/vec2.h"
#include "script.h"

extern struct matrix4 glbackend_view_matrix;

/* ---------------------------------------------------------------- logging */

static bool log_enabled = true;

static void log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096];
	(void)param;
	if (getenv("GLBACKEND_VERBOSE")) { /* debugging aid for script authors, never used by run.sh */
		vsnprintf(msg, sizeof(msg), format, args);
		fprintf(stderr, "[%d] %s\n", level, msg);
		return;
	}
	if (!log_enabled || level > LOG_WARNING)
		return;
	vsnprintf(msg, sizeof(msg), format, args);
	/* driver compiler output is not stable: keep only the first line of GLSL errors */
	if (!strncmp(msg, "Error compiling shader", 22))
		msg[22] = 0;
	for (char *p = msg; *p; p++)
		if (*p == '\n')
			*p = ' ';
	printf("log %s %s\n", level == LOG_ERROR ? "error" : "warning", msg);
}

/* ---------------------------------------------------------------- names */

struct nameval {
	const char *name;
	int value;
};
#define N(x) {#x, GS_##x}
static const struct nameval names[] = {
	N(UNKNOWN), N(A8), N(R8), N(RGBA), N(BGRX), N(BGRA), N(R10G10B10A2), N(RGBA16), N(R16), N(RGBA16F),
	N(RGBA32F), N(RG16F), N(RG32F), N(R16F), N(R32F), N(DXT1), N(DXT3), N(DXT5), N(R8G8), N(RGBA_UNORM),
	N(BGRX_UNORM), N(BGRA_UNORM), N(RG16),
	N(ZS_NONE), N(Z16), N(Z24_S8), N(Z32F), N(Z32F_S8X24),
	N(BLEND_ZERO), N(BLEND_ONE), N(BLEND_SRCCOLOR), N(BLEND_INVSRCCOLOR), N(BLEND_SRCALPHA),
	N(BLEND_INVSRCALPHA), N(BLEND_DSTCOLOR), N(BLEND_INVDSTCOLOR), N(BLEND_DSTALPHA), N(BLEND_INVDSTALPHA),
	N(BLEND_SRCALPHASAT),
	N(BLEND_OP_ADD), N(BLEND_OP_SUBTRACT), N(BLEND_OP_REVERSE_SUBTRACT), N(BLEND_OP_MIN), N(BLEND_OP_MAX),
	N(NEVER), N(LESS), N(LEQUAL), N(EQUAL), N(GEQUAL), N(GREATER), N(NOTEQUAL), N(ALWAYS),
	N(STENCIL_FRONT), N(STENCIL_BACK), N(STENCIL_BOTH),
	N(KEEP), N(ZERO), N(REPLACE), N(INCR), N(DECR), N(INVERT),
	N(BACK), N(FRONT), N(NEITHER),
	N(POINTS), N(LINES), N(LINESTRIP), N(TRIS), N(TRISTRIP),
	N(UNSIGNED_SHORT), N(UNSIGNED_LONG),
	N(ADDRESS_CLAMP), N(ADDRESS_WRAP), N(ADDRESS_MIRROR), N(ADDRESS_BORDER), N(ADDRESS_MIRRORONCE),
	N(FILTER_POINT), N(FILTER_LINEAR), N(FILTER_ANISOTROPIC), N(FILTER_MIN_MAG_POINT_MIP_LINEAR),
	N(FILTER_MIN_POINT_MAG_LINEAR_MIP_POINT), N(FILTER_MIN_POINT_MAG_MIP_LINEAR),
	N(FILTER_MIN_LINEAR_MAG_MIP_POINT), N(FILTER_MIN_LINEAR_MAG_POINT_MIP_LINEAR),
	N(FILTER_MIN_MAG_LINEAR_MIP_POINT),
	N(SHADER_PARAM_UNKNOWN), N(SHADER_PARAM_BOOL), N(SHADER_PARAM_FLOAT), N(SHADER_PARAM_INT),
	N(SHADER_PARAM_STRING), N(SHADER_PARAM_VEC2), N(SHADER_PARAM_VEC3), N(SHADER_PARAM_VEC4),
	N(SHADER_PARAM_INT2), N(SHADER_PARAM_INT3), N(SHADER_PARAM_INT4), N(SHADER_PARAM_MATRIX4X4),
	N(SHADER_PARAM_TEXTURE),
	N(TEXTURE_2D), N(TEXTURE_3D), N(TEXTURE_CUBE),
	{"BUILD_MIPMAPS", GS_BUILD_MIPMAPS}, {"DYNAMIC", GS_DYNAMIC}, {"RENDER_TARGET", GS_RENDER_TARGET},
	{"GL_DUMMYTEX", GS_GL_DUMMYTEX},
	{"CLEAR_COLOR", GS_CLEAR_COLOR}, {"CLEAR_DEPTH", GS_CLEAR_DEPTH}, {"CLEAR_STENCIL", GS_CLEAR_STENCIL},
};

static const char *name_of(int v, const char *prefix)
{
	size_t pl = strlen(prefix);
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (names[i].value == v && !strncmp(names[i].name, prefix, pl))
			return names[i].name;
	return "?";
}

/* An enum/flag token: NAME, GS_NAME, a number, or several joined by '|'. */
static int ev(const char *tok)
{
	int total = 0;
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", tok);
	for (char *s = strtok(buf, "|"); s; s = strtok(NULL, "|")) {
		const char *n = strncmp(s, "GS_", 3) ? s : s + 3;
		bool found = false;
		for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
			if (!strcmp(names[i].name, n)) {
				total |= names[i].value;
				found = true;
				break;
			}
		if (!found)
			total |= (int)strtol(s, NULL, 0);
	}
	return total;
}

/* ---------------------------------------------------------------- objects */

enum kind { K_NONE, K_TEX, K_CUBE, K_VOL, K_ZS, K_SAMPLER, K_VS, K_PS, K_VB, K_IB };
struct obj {
	char name[40];
	enum kind kind;
	void *p;
};
static struct obj objs[128];
static int nobjs;
static gs_device_t *dev;

static struct obj *find_obj(const char *name)
{
	for (int i = 0; i < nobjs; i++)
		if (objs[i].kind != K_NONE && !strcmp(objs[i].name, name))
			return &objs[i];
	return NULL;
}

/* "-" means NULL */
static void *get(const char *name, enum kind k)
{
	if (!strcmp(name, "-"))
		return NULL;
	struct obj *o = find_obj(name);
	if (!o || o->kind != k) {
		fprintf(stderr, "script: unknown object %s\n", name);
		exit(2);
	}
	return o->p;
}

static void put(const char *name, enum kind k, void *p)
{
	struct obj *o = find_obj(name);
	if (!o) {
		if (nobjs >= 128) {
			fprintf(stderr, "script: too many objects\n");
			exit(2);
		}
		o = &objs[nobjs++];
	}
	snprintf(o->name, sizeof(o->name), "%s", name);
	o->kind = k;
	o->p = p;
}

static void release(struct obj *o)
{
	switch (o->kind) {
	case K_TEX:
	case K_CUBE:
	case K_VOL:
		gs_texture_destroy(o->p);
		break;
	case K_ZS:
		gs_zstencil_destroy(o->p);
		break;
	case K_SAMPLER:
		gs_samplerstate_destroy(o->p);
		break;
	case K_VS:
	case K_PS:
		gs_shader_destroy(o->p);
		break;
	case K_VB:
		gs_vertexbuffer_destroy(o->p);
		break;
	case K_IB:
		gs_indexbuffer_destroy(o->p);
		break;
	default:
		break;
	}
	o->kind = K_NONE;
}

static const char *objname_of(void *p)
{
	if (!p)
		return "-";
	for (int i = 0; i < nobjs; i++)
		if (objs[i].kind != K_NONE && objs[i].p == p)
			return objs[i].name;
	return "?";
}

/* ---------------------------------------------------------------- helpers */

static float fl(const char *s) { return (float)strtod(s, NULL); }

static void print_hex(const uint8_t *d, size_t n)
{
	if (!n)
		printf("-");
	for (size_t i = 0; i < n; i++)
		printf("%02x", d[i]);
}

/* hex string, or rep:COUNT:HEX (HEX repeated COUNT times); returns malloc'd bytes */
static uint8_t *parse_hex(const char *s, size_t *len)
{
	size_t count = 1;
	if (!strncmp(s, "rep:", 4)) {
		count = strtoul(s + 4, NULL, 10);
		s = strchr(s + 4, ':') + 1;
	}
	size_t n = strlen(s) / 2;
	uint8_t *out = malloc(n * count + 1);
	for (size_t i = 0; i < n; i++)
		out[i] = (uint8_t)(script_hexval(s[2 * i]) * 16 + script_hexval(s[2 * i + 1]));
	for (size_t c = 1; c < count; c++)
		memcpy(out + c * n, out, n);
	*len = n * count;
	return out;
}

static size_t bpp_bytes(enum gs_color_format f) { return gs_get_format_bpp(f) / 8; }

/* Read back a 2D texture through a stage surface. */
static void cmd_read(const char *name)
{
	gs_texture_t *t = get(name, K_TEX);
	uint32_t w = gs_texture_get_width(t), h = gs_texture_get_height(t);
	enum gs_color_format f = gs_texture_get_color_format(t);
	gs_stagesurf_t *s = device_stagesurface_create(dev, w, h, f);
	if (!s) {
		printf("read %s stage-create-failed\n", name);
		return;
	}
	device_stage_texture(dev, s, t);
	uint8_t *data;
	uint32_t ls;
	if (!gs_stagesurface_map(s, &data, &ls)) {
		printf("read %s map-failed\n", name);
	} else {
		printf("read %s %ux%u linesize=%u\n", name, w, h, ls);
		for (uint32_t y = 0; y < h; y++) {
			printf("  row %u ", y);
			print_hex(data + (size_t)y * ls, (size_t)w * bpp_bytes(f));
			printf("\n");
		}
		gs_stagesurface_unmap(s);
	}
	gs_stagesurface_destroy(s);
}

/* ---------------------------------------------------------------- Xvfb */

static pid_t xvfb_pid;
static void stop_xvfb(void);
static void start_xvfb(void)
{
	int fds[2];
	if (pipe(fds))
		return;
	xvfb_pid = fork();
	if (xvfb_pid == 0) {
		char fdarg[16];
		snprintf(fdarg, sizeof(fdarg), "%d", fds[1]);
		close(fds[0]);
		prctl(PR_SET_PDEATHSIG, SIGTERM); /* never outlive the runner */
		freopen("/dev/null", "w", stdout);
		freopen("/dev/null", "w", stderr);
		execlp("Xvfb", "Xvfb", "-displayfd", fdarg, "-screen", "0", "64x64x24", "-nolisten", "tcp", (char *)NULL);
		_exit(127);
	}
	close(fds[1]);
	atexit(stop_xvfb);
	char num[16] = {0};
	ssize_t n = read(fds[0], num, sizeof(num) - 1);
	if (n > 0) {
		char d[32];
		snprintf(d, sizeof(d), ":%d", atoi(num));
		setenv("DISPLAY", d, 1);
	}
	close(fds[0]);
}

static void stop_xvfb(void)
{
	if (xvfb_pid > 0) {
		kill(xvfb_pid, SIGTERM);
		waitpid(xvfb_pid, NULL, 0);
		xvfb_pid = 0;
	}
}

/* ---------------------------------------------------------------- shaders */

static char *read_block(FILE *f, int *lineno)
{
	size_t cap = 4096, len = 0;
	char *buf = malloc(cap), line[SCRIPT_MAX_LINE];
	while (fgets(line, sizeof(line), f)) {
		(*lineno)++;
		if (!strncmp(line, "end", 3) && (line[3] == '\n' || line[3] == 0 || line[3] == '\r'))
			break;
		size_t l = strlen(line);
		if (len + l + 1 > cap) {
			cap *= 2;
			buf = realloc(buf, cap);
		}
		memcpy(buf + len, line, l);
		len += l;
	}
	buf[len] = 0;
	return buf;
}

static gs_sparam_t *param_of(gs_shader_t *sh, const char *pname)
{
	gs_sparam_t *p = gs_shader_get_param_by_name(sh, pname);
	if (!p)
		printf("  no-such-param %s\n", pname);
	return p;
}

static gs_shader_t *get_shader(const char *name)
{
	struct obj *o = find_obj(name);
	if (!o || (o->kind != K_VS && o->kind != K_PS)) {
		fprintf(stderr, "script: unknown shader %s\n", name);
		exit(2);
	}
	return o->p;
}

/* ---------------------------------------------------------------- main */

int main(int argc, char **argv)
{
	FILE *f;
	if (argc != 2) {
		fprintf(stderr, "usage: %s <script>\n", argv[0]);
		return 2;
	}
	f = fopen(argv[1], "r");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	base_set_log_handler(log_handler, NULL);
	setvbuf(stdout, NULL, _IOLBF, 0);
	/* Reproducible environment: Mesa software rendering on a private Xvfb, never the user's GPU. */
	setenv("LIBGL_ALWAYS_SOFTWARE", "1", 1);
	setenv("GALLIUM_DRIVER", "llvmpipe", 1);
	if (access("/usr/share/glvnd/egl_vendor.d/50_mesa.json", R_OK) == 0)
		setenv("__EGL_VENDOR_LIBRARY_FILENAMES", "/usr/share/glvnd/egl_vendor.d/50_mesa.json", 1);
	unsetenv("WAYLAND_DISPLAY");
	alarm(100); /* a wedged context must not hang the suite */
	matrix4_identity(&glbackend_view_matrix);
	start_xvfb();
	if (!getenv("DISPLAY")) {
		fprintf(stderr, "glbackend: could not start Xvfb\n");
		return 3;
	}

	static struct script_line sl;
	int lineno = 0;
	while (script_next(f, &sl, &lineno)) {
		char **a = sl.argv;
		int n = sl.argc;
		const char *c = a[0];
#define IS(x) (!strcmp(c, x))
#define NEED(k)                                                                          \
	if (n < (k)) {                                                                   \
		fprintf(stderr, "script: line %d: %s needs %d args\n", sl.lineno, c, (k) - 1); \
		return 2;                                                                \
	}
		if (IS("echo")) {
			printf("# %s\n", n > 1 ? a[1] : "");
		} else if (IS("create")) {
			log_enabled = false; /* initialisation logs driver strings */
			gs_device_t *d = NULL;
			int rc = device_create(&d, n > 1 ? (uint32_t)atoi(a[1]) : 0);
			log_enabled = true;
			printf("create rc=%d device=%s\n", rc, d ? "yes" : "no");
			dev = d;
		} else if (IS("info")) {
			printf("name %s\ntype %d\npreprocessor %s\n", device_get_name(), device_get_type(),
			       device_preprocessor_name());
		} else if (IS("adapters")) {
			bool counter(void *p, const char *nm, uint32_t id)
			{
				(void)nm, (void)id;
				(*(int *)p)++;
				return true;
			}
			int cnt = 0;
			bool r = device_enum_adapters(dev, counter, &cnt);
			printf("adapters ret=%d count=%d\n", r, cnt);
		} else if (IS("destroy")) {
			for (int i = 0; i < nobjs; i++)
				if (objs[i].kind != K_NONE)
					release(&objs[i]);
			device_destroy(dev);
			dev = NULL;
			printf("destroyed\n");
		} else if (IS("enter")) {
			device_enter_context(dev);
		} else if (IS("leave")) {
			device_leave_context(dev);
		} else if (IS("size")) {
			uint32_t x = 99, y = 99;
			device_get_size(dev, &x, &y);
			uint32_t w = device_get_width(dev);
			uint32_t h = device_get_height(dev);
			printf("size %u %u width %u height %u\n", x, y, w, h);
		} else if (IS("tex")) { /* tex NAME W H FMT LEVELS FLAGS [HEX[,HEX...]|-] */
			NEED(7);
			const uint8_t *lv[16] = {0};
			uint8_t *owned[16] = {0};
			int nl = 0;
			if (n > 7 && strcmp(a[7], "-")) {
				char *copy = strdup(a[7]);
				for (char *s = strtok(copy, ","); s && nl < 16; s = strtok(NULL, ",")) {
					size_t l;
					owned[nl] = parse_hex(s, &l);
					lv[nl] = owned[nl];
					nl++;
				}
				free(copy);
			}
			gs_texture_t *t = device_texture_create(dev, atoi(a[2]), atoi(a[3]), ev(a[4]), atoi(a[5]), nl ? lv : NULL,
								ev(a[6]));
			printf("tex %s %s\n", a[1], t ? "ok" : "fail");
			if (t)
				put(a[1], K_TEX, t);
			for (int i = 0; i < nl; i++)
				free(owned[i]);
		} else if (IS("texinfo")) {
			gs_texture_t *t = get(a[1], K_TEX);
			printf("texinfo %s %ux%u fmt=%s type=%s rect=%d\n", a[1], gs_texture_get_width(t),
			       gs_texture_get_height(t), name_of(gs_texture_get_color_format(t), ""),
			       name_of(device_get_texture_type(t), "TEXTURE_"), gs_texture_is_rect(t));
		} else if (IS("read")) {
			cmd_read(a[1]);
		} else if (IS("texmap")) { /* texmap NAME HEX : map a dynamic texture, write tight rows, unmap */
			gs_texture_t *t = get(a[1], K_TEX);
			uint8_t *p = NULL;
			uint32_t ls = 0;
			bool ok = gs_texture_map(t, &p, &ls);
			printf("texmap %s ok=%d linesize=%u\n", a[1], ok, ls);
			if (ok) {
				size_t l;
				uint8_t *d = parse_hex(a[2], &l);
				uint32_t rb = gs_texture_get_width(t) * (uint32_t)bpp_bytes(gs_texture_get_color_format(t));
				for (uint32_t y = 0; y * rb < l; y++)
					memcpy(p + (size_t)y * ls, d + (size_t)y * rb, rb);
				free(d);
				gs_texture_unmap(t);
			}
		} else if (IS("cube")) { /* cube NAME SIZE FMT LEVELS FLAGS */
			gs_texture_t *t = device_cubetexture_create(dev, atoi(a[2]), ev(a[3]), atoi(a[4]), NULL, ev(a[5]));
			printf("cube %s %s\n", a[1], t ? "ok" : "fail");
			if (t)
				put(a[1], K_CUBE, t);
		} else if (IS("cubeinfo")) {
			gs_texture_t *t = get(a[1], K_CUBE);
			printf("cubeinfo %s size=%u fmt=%s type=%s\n", a[1], gs_cubetexture_get_size(t),
			       name_of(gs_cubetexture_get_color_format(t), ""),
			       name_of(device_get_texture_type(t), "TEXTURE_"));
		} else if (IS("vol")) { /* vol NAME W H D FMT LEVELS FLAGS */
			gs_texture_t *t = device_voltexture_create(dev, atoi(a[2]), atoi(a[3]), atoi(a[4]), ev(a[5]),
								    atoi(a[6]), NULL, ev(a[7]));
			printf("vol %s %s\n", a[1], t ? "ok" : "fail");
			if (t)
				put(a[1], K_VOL, t);
		} else if (IS("volinfo")) {
			gs_texture_t *t = get(a[1], K_VOL);
			printf("volinfo %s %ux%ux%u fmt=%s type=%s\n", a[1], gs_voltexture_get_width(t),
			       gs_voltexture_get_height(t), gs_voltexture_get_depth(t),
			       name_of(gs_voltexture_get_color_format(t), ""),
			       name_of(device_get_texture_type(t), "TEXTURE_"));
		} else if (IS("zs")) { /* zs NAME W H FMT */
			gs_zstencil_t *z = device_zstencil_create(dev, atoi(a[2]), atoi(a[3]), ev(a[4]));
			printf("zs %s %s\n", a[1], z ? "ok" : "fail");
			if (z)
				put(a[1], K_ZS, z);
		} else if (IS("free")) {
			struct obj *o = find_obj(a[1]);
			if (o)
				release(o);
		} else if (IS("stage")) { /* stage W H FMT SRC : stage surface of arbitrary format from a texture */
			gs_stagesurf_t *s = device_stagesurface_create(dev, atoi(a[1]), atoi(a[2]), ev(a[3]));
			printf("stage %s\n", s ? "ok" : "fail");
			if (s) {
				device_stage_texture(dev, s, get(a[4], K_TEX));
				printf("stage info %ux%u fmt=%s\n", gs_stagesurface_get_width(s), gs_stagesurface_get_height(s),
				       name_of(gs_stagesurface_get_color_format(s), ""));
				gs_stagesurface_destroy(s);
			}
		} else if (IS("copy")) {
			device_copy_texture(dev, get(a[1], K_TEX), get(a[2], K_TEX));
		} else if (IS("copyr")) { /* copyr DST dx dy SRC sx sy sw sh */
			device_copy_texture_region(dev, get(a[1], K_TEX), atoi(a[2]), atoi(a[3]), get(a[4], K_TEX), atoi(a[5]),
						   atoi(a[6]), atoi(a[7]), atoi(a[8]));
		} else if (IS("set_rt")) { /* set_rt TEX|- [ZS|-] */
			device_set_render_target(dev, get(a[1], K_TEX), n > 2 ? get(a[2], K_ZS) : NULL);
		} else if (IS("set_cube_rt")) { /* set_cube_rt CUBE SIDE [ZS] */
			device_set_cube_render_target(dev, get(a[1], K_CUBE), atoi(a[2]), n > 3 ? get(a[3], K_ZS) : NULL);
		} else if (IS("get_rt")) {
			printf("rt %s zs %s\n", objname_of(device_get_render_target(dev)),
			       objname_of(device_get_zstencil_target(dev)));
		} else if (IS("clear")) { /* clear FLAGS R G B A DEPTH STENCIL */
			struct vec4 col;
			vec4_set(&col, fl(a[2]), fl(a[3]), fl(a[4]), fl(a[5]));
			device_clear(dev, ev(a[1]), &col, n > 6 ? fl(a[6]) : 1.f, n > 7 ? (uint8_t)atoi(a[7]) : 0);
		} else if (IS("viewport")) {
			device_set_viewport(dev, atoi(a[1]), atoi(a[2]), atoi(a[3]), atoi(a[4]));
		} else if (IS("get_viewport")) {
			struct gs_rect r;
			device_get_viewport(dev, &r);
			printf("viewport %d %d %d %d\n", r.x, r.y, r.cx, r.cy);
		} else if (IS("scissor")) { /* scissor X Y W H | scissor off */
			struct gs_rect r;
			if (!strcmp(a[1], "off")) {
				device_set_scissor_rect(dev, NULL);
			} else {
				r.x = atoi(a[1]), r.y = atoi(a[2]), r.cx = atoi(a[3]), r.cy = atoi(a[4]);
				device_set_scissor_rect(dev, &r);
			}
		} else if (IS("cull")) {
			device_set_cull_mode(dev, ev(a[1]));
		} else if (IS("get_cull")) {
			{
				static const char *cn[] = {"BACK", "FRONT", "NEITHER"};
				int cm = device_get_cull_mode(dev);
				printf("cull %s\n", cm >= 0 && cm < 3 ? cn[cm] : "?");
			}
		} else if (IS("blend")) {
			device_enable_blending(dev, !strcmp(a[1], "on"));
		} else if (IS("blendfn")) {
			device_blend_function(dev, ev(a[1]), ev(a[2]));
		} else if (IS("blendfn_sep")) {
			device_blend_function_separate(dev, ev(a[1]), ev(a[2]), ev(a[3]), ev(a[4]));
		} else if (IS("blendop")) {
			device_blend_op(dev, ev(a[1]));
		} else if (IS("depth")) {
			device_enable_depth_test(dev, !strcmp(a[1], "on"));
		} else if (IS("depthfn")) {
			device_depth_function(dev, ev(a[1]));
		} else if (IS("stencil")) {
			device_enable_stencil_test(dev, !strcmp(a[1], "on"));
		} else if (IS("stencil_write")) {
			device_enable_stencil_write(dev, !strcmp(a[1], "on"));
		} else if (IS("stencilfn")) {
			device_stencil_function(dev, ev(a[1]), ev(a[2]));
		} else if (IS("stencilop")) {
			device_stencil_op(dev, ev(a[1]), ev(a[2]), ev(a[3]), ev(a[4]));
		} else if (IS("colormask")) { /* colormask R G B A (0/1) */
			device_enable_color(dev, atoi(a[1]), atoi(a[2]), atoi(a[3]), atoi(a[4]));
		} else if (IS("srgb")) {
			device_enable_framebuffer_srgb(dev, !strcmp(a[1], "on"));
		} else if (IS("get_srgb")) {
			printf("srgb %d\n", device_framebuffer_srgb_enabled(dev));
		} else if (IS("ortho")) {
			device_ortho(dev, fl(a[1]), fl(a[2]), fl(a[3]), fl(a[4]), fl(a[5]), fl(a[6]));
		} else if (IS("frustum")) {
			device_frustum(dev, fl(a[1]), fl(a[2]), fl(a[3]), fl(a[4]), fl(a[5]), fl(a[6]));
		} else if (IS("proj_push")) {
			device_projection_push(dev);
		} else if (IS("proj_pop")) {
			device_projection_pop(dev);
		} else if (IS("view_translate")) { /* the matrix stack's current matrix (stub) */
			matrix4_identity(&glbackend_view_matrix);
			glbackend_view_matrix.t.x = fl(a[1]);
			glbackend_view_matrix.t.y = fl(a[2]);
			glbackend_view_matrix.t.z = fl(a[3]);
		} else if (IS("sampler")) { /* sampler NAME FILTER U V W ANISO BORDER */
			struct gs_sampler_info si = {ev(a[2]), ev(a[3]), ev(a[4]), ev(a[5]), atoi(a[6]),
						     (uint32_t)strtoul(a[7], NULL, 0)};
			gs_samplerstate_t *s = device_samplerstate_create(dev, &si);
			printf("sampler %s %s\n", a[1], s ? "ok" : "fail");
			if (s)
				put(a[1], K_SAMPLER, s);
		} else if (IS("load_sampler")) { /* load_sampler NAME|- UNIT */
			device_load_samplerstate(dev, get(a[1], K_SAMPLER), atoi(a[2]));
		} else if (IS("load_tex")) {
			device_load_texture(dev, get(a[1], K_TEX), atoi(a[2]));
		} else if (IS("vshader") || IS("pshader")) { /* vshader NAME  <block> end */
			char *src = read_block(f, &lineno);
			char *err = NULL;
			gs_shader_t *s = IS("vshader") ? device_vertexshader_create(dev, src, "script", &err)
						       : device_pixelshader_create(dev, src, "script", &err);
			printf("%s %s %s\n", c, a[1], s ? "ok" : "fail");
			if (!s && err)
				printf("  error-text %s\n", err[0] ? "present" : "empty");
			if (s)
				put(a[1], IS("vshader") ? K_VS : K_PS, s);
			bfree(err);
			free(src);
		} else if (IS("load_vs")) {
			device_load_vertexshader(dev, n > 1 && strcmp(a[1], "-") ? get(a[1], K_VS) : NULL);
		} else if (IS("load_ps")) {
			device_load_pixelshader(dev, n > 1 && strcmp(a[1], "-") ? get(a[1], K_PS) : NULL);
		} else if (IS("cur_shaders")) {
			printf("vs %s ps %s\n", objname_of(device_get_vertex_shader(dev)),
			       objname_of(device_get_pixel_shader(dev)));
		} else if (IS("params")) {
			gs_shader_t *sh = get_shader(a[1]);
			int np = gs_shader_get_num_params(sh);
			printf("params %s count=%d\n", a[1], np);
			for (int i = 0; i < np; i++) {
				struct gs_shader_param_info pi;
				gs_shader_get_param_info(gs_shader_get_param_by_idx(sh, i), &pi);
				printf("  %d %s %s\n", i, pi.name, name_of(pi.type, "SHADER_PARAM_"));
			}
			printf("  viewproj=%s world=%s\n", gs_shader_get_viewproj_matrix(sh) ? "yes" : "no",
			       gs_shader_get_world_matrix(sh) ? "yes" : "no");
		} else if (IS("set")) {
			/* set SHADER PARAM KIND values...  KIND: bool int float vec2 vec3 vec4 mat4 mat3 tex sampler
			 * default raw:HEX */
			gs_shader_t *sh = get_shader(a[1]);
			gs_sparam_t *p = param_of(sh, a[2]);
			const char *k = a[3];
			if (!p) {
			} else if (!strcmp(k, "bool")) {
				gs_shader_set_bool(p, atoi(a[4]));
			} else if (!strcmp(k, "int")) {
				gs_shader_set_int(p, atoi(a[4]));
			} else if (!strcmp(k, "float")) {
				gs_shader_set_float(p, fl(a[4]));
			} else if (!strcmp(k, "vec2")) {
				struct vec2 v = {{fl(a[4]), fl(a[5])}};
				gs_shader_set_vec2(p, &v);
			} else if (!strcmp(k, "vec3")) {
				struct vec3 v;
				vec3_set(&v, fl(a[4]), fl(a[5]), fl(a[6]));
				gs_shader_set_vec3(p, &v);
			} else if (!strcmp(k, "vec4")) {
				struct vec4 v;
				vec4_set(&v, fl(a[4]), fl(a[5]), fl(a[6]), fl(a[7]));
				gs_shader_set_vec4(p, &v);
			} else if (!strcmp(k, "mat4")) { /* 16 floats row-major as stored */
				struct matrix4 m;
				struct vec4 *rows4[4] = {&m.x, &m.y, &m.z, &m.t};
				for (int i = 0; i < 16; i++)
					rows4[i / 4]->ptr[i % 4] = fl(a[4 + i]);
				gs_shader_set_matrix4(p, &m);
			} else if (!strcmp(k, "mat3")) { /* 12 floats */
				struct matrix3 m;
				memset(&m, 0, sizeof(m));
				struct vec3 *rows[4] = {&m.x, &m.y, &m.z, &m.t};
				for (int r = 0; r < 4; r++)
					for (int cc = 0; cc < 3; cc++)
						rows[r]->ptr[cc] = fl(a[4 + 3 * r + cc]);
				gs_shader_set_matrix3(p, &m);
			} else if (!strcmp(k, "tex")) {
				gs_shader_set_texture(p, get(a[4], K_TEX));
			} else if (!strcmp(k, "sampler")) {
				gs_shader_set_next_sampler(p, get(a[4], K_SAMPLER));
			} else if (!strcmp(k, "default")) {
				gs_shader_set_default(p);
			} else if (!strncmp(k, "raw", 3)) { /* raw HEX : gs_shader_set_val with that many bytes */
				size_t l;
				uint8_t *d = parse_hex(a[4], &l);
				gs_shader_set_val(p, d, l);
				free(d);
			} else {
				fprintf(stderr, "script: bad set kind %s\n", k);
				return 2;
			}
		} else if (IS("vb")) {
			/* vb NAME NUM FLAGS COLORS NTEX  then NUM lines "x y z [AARRGGBB] [u v]..." then end.
			 * COLORS and NTEX are 0/1 and 0..n (2-wide coordinates). */
			char vbname[40];
			snprintf(vbname, sizeof(vbname), "%s", a[1]);
			uint32_t vbflags = (uint32_t)ev(a[3]);
			size_t num = atoi(a[2]);
			bool colors = atoi(a[4]);
			size_t ntex = atoi(a[5]);
			struct gs_vb_data *d = gs_vbdata_create();
			d->num = num;
			d->points = bzalloc(sizeof(struct vec3) * num);
			if (colors)
				d->colors = bzalloc(sizeof(uint32_t) * num);
			d->num_tex = ntex;
			if (ntex) {
				d->tvarray = bzalloc(sizeof(struct gs_tvertarray) * ntex);
				for (size_t t = 0; t < ntex; t++) {
					d->tvarray[t].width = 2;
					d->tvarray[t].array = bzalloc(sizeof(float) * 2 * num);
				}
			}
			for (size_t i = 0; i < num; i++) {
				if (!script_next(f, &sl, &lineno))
					break;
				int k = 0;
				vec3_set(&d->points[i], fl(sl.argv[k]), fl(sl.argv[k + 1]), fl(sl.argv[k + 2]));
				k += 3;
				if (colors)
					d->colors[i] = (uint32_t)strtoul(sl.argv[k++], NULL, 16);
				for (size_t t = 0; t < ntex; t++, k += 2) {
					((float *)d->tvarray[t].array)[2 * i] = fl(sl.argv[k]);
					((float *)d->tvarray[t].array)[2 * i + 1] = fl(sl.argv[k + 1]);
				}
			}
			script_next(f, &sl, &lineno); /* "end" */
			gs_vertbuffer_t *vb = device_vertexbuffer_create(dev, d, vbflags);
			printf("vb %s %s\n", vbname, vb ? "ok" : "fail");
			if (vb)
				put(vbname, K_VB, vb);
		} else if (IS("ib")) { /* ib NAME TYPE IDX... */
			enum gs_index_type ty = ev(a[2]);
			size_t cnt = n - 3;
			void *idx = bmalloc(cnt * (ty == GS_UNSIGNED_SHORT ? 2 : 4));
			for (size_t i = 0; i < cnt; i++) {
				if (ty == GS_UNSIGNED_SHORT)
					((uint16_t *)idx)[i] = (uint16_t)atoi(a[3 + i]);
				else
					((uint32_t *)idx)[i] = (uint32_t)atoi(a[3 + i]);
			}
			gs_indexbuffer_t *ib = device_indexbuffer_create(dev, ty, idx, cnt, 0);
			printf("ib %s %s\n", a[1], ib ? "ok" : "fail");
			if (ib) {
				put(a[1], K_IB, ib);
				printf("ib info num=%zu type=%s\n", gs_indexbuffer_get_num_indices(ib),
				       name_of(gs_indexbuffer_get_type(ib), "UNSIGNED_"));
			}
		} else if (IS("load_vb")) {
			device_load_vertexbuffer(dev, get(a[1], K_VB));
		} else if (IS("load_ib")) {
			device_load_indexbuffer(dev, get(a[1], K_IB));
		} else if (IS("draw")) { /* draw MODE START NUM */
			device_draw(dev, ev(a[1]), atoi(a[2]), atoi(a[3]));
		} else if (IS("flush")) {
			device_flush(dev);
		} else if (IS("avail")) { /* driver dependent capability queries */
			printf("nv12=%d p010=%d\n", device_nv12_available(dev), device_p010_available(dev));
		} else {
			fprintf(stderr, "script: line %d: unknown command %s\n", sl.lineno, c);
			return 2;
		}
	}
	return 0;
}
