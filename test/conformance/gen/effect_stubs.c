/*
 * Conformance harness (area effect, spec S-020): a fake graphics backend.
 *
 * The real libobs/graphics/graphics.c is linked.  gs_create() "loads" a module through
 * os_dlopen()/load_graphics_imports(); both are replaced here and install the function table
 * below.  The table models just enough of a GPU backend for the effect code:
 *
 *   - shaders are objects that remember the exact text and location string they were created with
 *     (every creation prints a "backend ..." event line, see test/conformance/README.md);
 *   - shader creation parses the text with the real shader_parser (like the OpenGL backend does),
 *     fails with the parser's error text when that fails, and exposes every global variable of the
 *     text as a shader parameter (type via get_shader_param_type);
 *   - parameter uploads (set_val/set_texture/set_next_sampler), shader loads and shader destruction
 *     are printed as event lines;
 *   - textures and sampler states are opaque handles carrying only a small integer id, so that no
 *     pointer ever reaches the output.
 *
 * Test tooling, not part of libobs.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "util/bmem.h"
#include "util/base.h"
#include "util/dstr.h"
#include "util/platform.h"
#include "graphics/graphics-internal.h"
#include "graphics/shader-parser.h"

/* ---- configuration (set by the runner's "backend" command) ---- */

int fake_text_mode = 2;        /* 0 = nothing, 1 = hash only, 2 = hash + full text */
bool fake_parse_shaders = true;    /* run shader_parse on the text and expose its globals */
char *fake_fail_msg[2];            /* [0] vertex, [1] pixel: creation fails with this error text */
char *fake_warn_msg[2];            /* creation succeeds but an error text is returned */
const char *fake_preprocessor;     /* returned by device_preprocessor_name */
int fake_verbose_events = 1;       /* print set_val/load/destroy events */

/* ---- opaque handles with an id ---- */

struct fake_handle {
	int id;
};

static struct fake_handle *all_handles[256];
static int num_handles;

void *fake_handle_new(int id)
{
	struct fake_handle *h = bzalloc(sizeof(*h));
	h->id = id;
	if (num_handles < 256)
		all_handles[num_handles++] = h;
	return h;
}

/* Releases every handle created so far (called when the runner resets the graphics context). */
void fake_handles_free(void)
{
	while (num_handles)
		bfree(all_handles[--num_handles]);
}

int fake_handle_id(const void *h)
{
	return h ? ((const struct fake_handle *)h)->id : -1;
}

/* ---- shaders ---- */

struct fake_sparam {
	struct fake_shader *shader;
	char *name;
	enum gs_shader_param_type type;
	int array_count;
};

struct fake_shader {
	enum gs_shader_type type;
	char *location;
	size_t num_params;
	struct fake_sparam *params;
};

static const char *type_name(enum gs_shader_param_type t)
{
	switch (t) {
	case GS_SHADER_PARAM_UNKNOWN:
		return "UNKNOWN";
	case GS_SHADER_PARAM_BOOL:
		return "BOOL";
	case GS_SHADER_PARAM_FLOAT:
		return "FLOAT";
	case GS_SHADER_PARAM_INT:
		return "INT";
	case GS_SHADER_PARAM_STRING:
		return "STRING";
	case GS_SHADER_PARAM_VEC2:
		return "VEC2";
	case GS_SHADER_PARAM_VEC3:
		return "VEC3";
	case GS_SHADER_PARAM_VEC4:
		return "VEC4";
	case GS_SHADER_PARAM_INT2:
		return "INT2";
	case GS_SHADER_PARAM_INT3:
		return "INT3";
	case GS_SHADER_PARAM_INT4:
		return "INT4";
	case GS_SHADER_PARAM_MATRIX4X4:
		return "MATRIX4X4";
	case GS_SHADER_PARAM_TEXTURE:
		return "TEXTURE";
	}
	return "?";
}

const char *fake_param_type_name(int t)
{
	return type_name((enum gs_shader_param_type)t);
}

/* Directory of the scratch files; a file name that starts with it is printed as "@tmp<rest>". */
char fake_tmpdir[400];

static void print_quoted(const char *s)
{
	size_t tl = strlen(fake_tmpdir);
	putchar('"');
	if (s && tl && !strncmp(s, fake_tmpdir, tl)) {
		fputs("@tmp", stdout);
		s += tl;
	}
	for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
		if (*p == '\\')
			fputs("\\\\", stdout);
		else if (*p == '"')
			fputs("\\\"", stdout);
		else if (*p == '\n')
			fputs("\\n", stdout);
		else if (*p == '\t')
			fputs("\\t", stdout);
		else if (*p == '\r')
			fputs("\\r", stdout);
		else if (*p < 0x20 || *p >= 0x7f)
			printf("\\x%02x", *p);
		else
			putchar(*p);
	}
	putchar('"');
}

static uint64_t fnv1a(const char *s)
{
	uint64_t h = 0xcbf29ce484222325ULL;
	for (const unsigned char *p = (const unsigned char *)s; p && *p; p++) {
		h ^= *p;
		h *= 1099511628211ULL;
	}
	return h;
}

static gs_shader_t *fake_shader_create(enum gs_shader_type type, const char *text, const char *file,
				       char **error_string)
{
	const char *kind = type == GS_SHADER_VERTEX ? "vertexshader" : "pixelshader";
	int idx = type == GS_SHADER_VERTEX ? 0 : 1;
	struct fake_shader *sh = bzalloc(sizeof(*sh));
	struct shader_parser sp;
	bool parsed_ok = true;

	sh->type = type;
	sh->location = bstrdup(file);

	printf("backend %s_create location=", kind);
	print_quoted(file ? file : "(null)");
	printf(" len=%zu fnv=%016llx\n", strlen(text), (unsigned long long)fnv1a(text));
	if (fake_text_mode >= 2) {
		fputs("  text ", stdout);
		print_quoted(text);
		putchar('\n');
	}

	if (fake_fail_msg[idx]) {
		printf("  result fail (forced)\n");
		*error_string = bstrdup(fake_fail_msg[idx]);
		bfree(sh->location);
		bfree(sh);
		return NULL;
	}

	if (fake_parse_shaders) {
		shader_parser_init(&sp);
		parsed_ok = shader_parse(&sp, text, file);
		if (!parsed_ok) {
			char *errs = shader_parser_geterrors(&sp);
			printf("  result fail (shader_parse) errors=");
			print_quoted(errs ? errs : "");
			putchar('\n');
			*error_string = errs;
			shader_parser_free(&sp);
			bfree(sh->location);
			bfree(sh);
			return NULL;
		}

		sh->num_params = sp.params.num;
		sh->params = bzalloc(sizeof(struct fake_sparam) * (sp.params.num ? sp.params.num : 1));
		for (size_t i = 0; i < sp.params.num; i++) {
			struct shader_var *v = sp.params.array + i;
			sh->params[i].shader = sh;
			sh->params[i].name = bstrdup(v->name);
			sh->params[i].type = get_shader_param_type(v->type);
			sh->params[i].array_count = v->array_count;
			printf("  sparam %zu name=%s type=%s array=%d\n", i, v->name, type_name(sh->params[i].type),
			       v->array_count);
		}
		shader_parser_free(&sp);
	}

	if (fake_warn_msg[idx]) {
		*error_string = bstrdup(fake_warn_msg[idx]);
		printf("  result ok (with error text)\n");
	} else {
		printf("  result ok\n");
	}
	return (gs_shader_t *)sh;
}

static gs_shader_t *fake_vertexshader_create(gs_device_t *d, const char *shader, const char *file, char **err)
{
	(void)d;
	return fake_shader_create(GS_SHADER_VERTEX, shader, file, err);
}

static gs_shader_t *fake_pixelshader_create(gs_device_t *d, const char *shader, const char *file, char **err)
{
	(void)d;
	return fake_shader_create(GS_SHADER_PIXEL, shader, file, err);
}

static void fake_shader_destroy(gs_shader_t *s)
{
	struct fake_shader *sh = (struct fake_shader *)s;
	if (!sh)
		return;
	if (fake_verbose_events)
	{
		printf("backend shader_destroy location=");
		print_quoted(sh->location ? sh->location : "(null)");
		putchar('\n');
	}
	for (size_t i = 0; i < sh->num_params; i++)
		bfree(sh->params[i].name);
	bfree(sh->params);
	bfree(sh->location);
	bfree(sh);
}

static gs_sparam_t *fake_get_param_by_name(gs_shader_t *s, const char *name)
{
	struct fake_shader *sh = (struct fake_shader *)s;
	for (size_t i = 0; i < sh->num_params; i++)
		if (!strcmp(sh->params[i].name, name))
			return (gs_sparam_t *)(sh->params + i);
	return NULL;
}

static gs_sparam_t *fake_get_param_by_idx(gs_shader_t *s, uint32_t idx)
{
	struct fake_shader *sh = (struct fake_shader *)s;
	return idx < sh->num_params ? (gs_sparam_t *)(sh->params + idx) : NULL;
}

static int fake_get_num_params(const gs_shader_t *s)
{
	return (int)((const struct fake_shader *)s)->num_params;
}

static void fake_get_param_info(const gs_sparam_t *p, struct gs_shader_param_info *info)
{
	const struct fake_sparam *sp = (const struct fake_sparam *)p;
	info->name = sp->name;
	info->type = sp->type;
}

static void ev_head(const char *what, const struct fake_sparam *sp)
{
	printf("backend %s shader=%s param=%s", what, sp->shader->type == GS_SHADER_VERTEX ? "vertex" : "pixel",
	       sp->name);
}

static void fake_set_val(gs_sparam_t *p, const void *val, size_t size)
{
	const struct fake_sparam *sp = (const struct fake_sparam *)p;
	if (!fake_verbose_events)
		return;
	ev_head("set_val", sp);
	if (sp->type == GS_SHADER_PARAM_TEXTURE && size == sizeof(struct gs_shader_texture)) {
		const struct gs_shader_texture *t = val;
		printf(" texture=%d srgb=%d\n", fake_handle_id(t->tex), t->srgb ? 1 : 0);
		return;
	}
	printf(" size=%zu hex=", size);
	if (!size)
		putchar('-');
	for (size_t i = 0; i < size; i++)
		printf("%02x", ((const uint8_t *)val)[i]);
	putchar('\n');
}

static void fake_set_texture(gs_sparam_t *p, gs_texture_t *tex)
{
	const struct fake_sparam *sp = (const struct fake_sparam *)p;
	if (!fake_verbose_events)
		return;
	ev_head("set_texture", sp);
	printf(" texture=%d\n", fake_handle_id(tex));
}

static void fake_set_next_sampler(gs_sparam_t *p, gs_samplerstate_t *s)
{
	const struct fake_sparam *sp = (const struct fake_sparam *)p;
	if (!fake_verbose_events)
		return;
	ev_head("set_next_sampler", sp);
	printf(" sampler=%d\n", fake_handle_id(s));
}

static void fake_load_vertexshader(gs_device_t *d, gs_shader_t *s)
{
	(void)d;
	if (fake_verbose_events)
	{
		printf("backend load_vertexshader ");
		print_quoted(s ? ((struct fake_shader *)s)->location : "none");
		putchar('\n');
	}
}

static void fake_load_pixelshader(gs_device_t *d, gs_shader_t *s)
{
	(void)d;
	if (fake_verbose_events)
	{
		printf("backend load_pixelshader ");
		print_quoted(s ? ((struct fake_shader *)s)->location : "none");
		putchar('\n');
	}
}

/* ---- the rest of the device needed to bring graphics.c up ---- */

static int device_dummy;

static int fake_device_create(gs_device_t **device, uint32_t adapter)
{
	(void)adapter;
	*device = (gs_device_t *)&device_dummy;
	return GS_SUCCESS;
}
static void fake_device_destroy(gs_device_t *d)
{
	(void)d;
}
static void fake_enter(gs_device_t *d)
{
	(void)d;
}
static void fake_leave(gs_device_t *d)
{
	(void)d;
}
static const char *fake_preprocessor_name(void)
{
	return fake_preprocessor;
}
static gs_vertbuffer_t *fake_vb_create(gs_device_t *d, struct gs_vb_data *data, uint32_t flags)
{
	(void)d, (void)flags;
	gs_vbdata_destroy(data);
	return (gs_vertbuffer_t *)bzalloc(8);
}
static void fake_vb_destroy(gs_vertbuffer_t *vb)
{
	bfree(vb);
}
static void fake_blend_sep(gs_device_t *d, enum gs_blend_type a, enum gs_blend_type b, enum gs_blend_type c,
			   enum gs_blend_type e)
{
	(void)d, (void)a, (void)b, (void)c, (void)e;
}
static void fake_blend_op(gs_device_t *d, enum gs_blend_op_type op)
{
	(void)d, (void)op;
}

/* Replacement for the dynamic-module loader in graphics-imports.c. */
bool load_graphics_imports(struct gs_exports *exports, void *module, const char *module_name)
{
	(void)module, (void)module_name;
	memset(exports, 0, sizeof(*exports));
	exports->device_preprocessor_name = fake_preprocessor_name;
	exports->device_create = fake_device_create;
	exports->device_destroy = fake_device_destroy;
	exports->device_enter_context = fake_enter;
	exports->device_leave_context = fake_leave;
	exports->device_vertexshader_create = fake_vertexshader_create;
	exports->device_pixelshader_create = fake_pixelshader_create;
	exports->device_vertexbuffer_create = fake_vb_create;
	exports->gs_vertexbuffer_destroy = fake_vb_destroy;
	exports->device_load_vertexshader = fake_load_vertexshader;
	exports->device_load_pixelshader = fake_load_pixelshader;
	exports->device_blend_function_separate = fake_blend_sep;
	exports->device_blend_op = fake_blend_op;
	exports->gs_shader_destroy = fake_shader_destroy;
	exports->gs_shader_get_num_params = fake_get_num_params;
	exports->gs_shader_get_param_by_idx = fake_get_param_by_idx;
	exports->gs_shader_get_param_by_name = fake_get_param_by_name;
	exports->gs_shader_get_param_info = fake_get_param_info;
	exports->gs_shader_set_val = fake_set_val;
	exports->gs_shader_set_texture = fake_set_texture;
	exports->gs_shader_set_next_sampler = fake_set_next_sampler;
	return true;
}

void *os_dlopen(const char *path)
{
	(void)path;
	return &device_dummy;
}
void os_dlclose(void *module)
{
	(void)module;
}
void gs_init_image_deps(void) {}
void gs_free_image_deps(void) {}

uint8_t *gs_create_texture_file_data(const char *file, enum gs_color_format *format, uint32_t *cx, uint32_t *cy)
{
	(void)file, (void)format, (void)cx, (void)cy;
	abort();
}

/* Reads a whole file (used by gs_effect_create_from_file); no BOM handling needed by the scripts. */
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
