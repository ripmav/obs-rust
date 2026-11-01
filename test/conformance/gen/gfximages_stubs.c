/*
 * Fake graphics backend for the gfximages runner (spec S-053). Test tooling, not part of libobs.
 *
 * Provides what graphics.c needs to come up (a device, shader creation that runs the real shader parser and
 * exposes the text's global variables as shader parameters, no event printing) plus a texture object that
 * keeps its pixel data, so that image-file.c can be observed through gs_texture_create / gs_texture_set_image.
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

/* ---- textures ---- */

struct fake_tex {
	uint32_t cx, cy;
	enum gs_color_format fmt;
	uint32_t flags, levels;
	uint32_t linesize;
	uint8_t *data;
	int set_image_calls; /* number of times pixel data was mapped (initial data counts as 0) */
};

struct fake_tex *fake_last_texture;
int fake_textures_alive;

static gs_texture_t *fake_texture_create(gs_device_t *d, uint32_t cx, uint32_t cy, enum gs_color_format fmt,
					 uint32_t levels, const uint8_t **data, uint32_t flags)
{
	(void)d;
	struct fake_tex *t = bzalloc(sizeof(*t));
	t->cx = cx, t->cy = cy, t->fmt = fmt, t->flags = flags, t->levels = levels;
	t->linesize = cx * gs_get_format_bpp(fmt) / 8;
	t->data = bzalloc((size_t)t->linesize * cy + 1);
	if (data && data[0])
		memcpy(t->data, data[0], (size_t)t->linesize * cy);
	fake_last_texture = t;
	fake_textures_alive++;
	return (gs_texture_t *)t;
}
static void fake_texture_destroy(gs_texture_t *t)
{
	struct fake_tex *ft = (struct fake_tex *)t;
	if (fake_last_texture == ft)
		fake_last_texture = NULL;
	bfree(ft->data);
	bfree(ft);
	fake_textures_alive--;
}
static uint32_t fake_tex_w(const gs_texture_t *t)
{
	return ((const struct fake_tex *)t)->cx;
}
static uint32_t fake_tex_h(const gs_texture_t *t)
{
	return ((const struct fake_tex *)t)->cy;
}
static enum gs_color_format fake_tex_fmt(const gs_texture_t *t)
{
	return ((const struct fake_tex *)t)->fmt;
}
static bool fake_tex_map(gs_texture_t *t, uint8_t **ptr, uint32_t *linesize)
{
	struct fake_tex *ft = (struct fake_tex *)t;
	ft->set_image_calls++;
	*ptr = ft->data;
	*linesize = ft->linesize;
	return true;
}
static void fake_tex_unmap(gs_texture_t *t)
{
	(void)t;
}

/* ---- shaders (parameters only) ---- */

struct fake_sparam {
	char *name;
	enum gs_shader_param_type type;
};
struct fake_shader {
	size_t num_params;
	struct fake_sparam *params;
};

static gs_shader_t *fake_shader_create(const char *text, const char *file, char **err)
{
	struct fake_shader *sh = bzalloc(sizeof(*sh));
	struct shader_parser sp;
	shader_parser_init(&sp);
	if (!shader_parse(&sp, text, file)) {
		*err = shader_parser_geterrors(&sp);
		shader_parser_free(&sp);
		bfree(sh);
		return NULL;
	}
	sh->num_params = sp.params.num;
	sh->params = bzalloc(sizeof(struct fake_sparam) * (sp.params.num + 1));
	for (size_t i = 0; i < sp.params.num; i++) {
		sh->params[i].name = bstrdup(sp.params.array[i].name);
		sh->params[i].type = get_shader_param_type(sp.params.array[i].type);
	}
	shader_parser_free(&sp);
	return (gs_shader_t *)sh;
}
static gs_shader_t *fake_vs_create(gs_device_t *d, const char *s, const char *f, char **e)
{
	(void)d;
	return fake_shader_create(s, f, e);
}
static void fake_shader_destroy(gs_shader_t *s)
{
	struct fake_shader *sh = (struct fake_shader *)s;
	if (!sh)
		return;
	for (size_t i = 0; i < sh->num_params; i++)
		bfree(sh->params[i].name);
	bfree(sh->params);
	bfree(sh);
}
static gs_sparam_t *fake_param_by_name(gs_shader_t *s, const char *name)
{
	struct fake_shader *sh = (struct fake_shader *)s;
	for (size_t i = 0; i < sh->num_params; i++)
		if (!strcmp(sh->params[i].name, name))
			return (gs_sparam_t *)(sh->params + i);
	return NULL;
}
static gs_sparam_t *fake_param_by_idx(gs_shader_t *s, uint32_t idx)
{
	struct fake_shader *sh = (struct fake_shader *)s;
	return idx < sh->num_params ? (gs_sparam_t *)(sh->params + idx) : NULL;
}
static int fake_num_params(const gs_shader_t *s)
{
	return (int)((const struct fake_shader *)s)->num_params;
}
static void fake_param_info(const gs_sparam_t *p, struct gs_shader_param_info *info)
{
	const struct fake_sparam *sp = (const struct fake_sparam *)p;
	info->name = sp->name;
	info->type = sp->type;
}
static void fake_set_val(gs_sparam_t *p, const void *v, size_t n)
{
	(void)p, (void)v, (void)n;
}
static void fake_set_texture(gs_sparam_t *p, gs_texture_t *t)
{
	(void)p, (void)t;
}
static void fake_set_sampler(gs_sparam_t *p, gs_samplerstate_t *s)
{
	(void)p, (void)s;
}
static void fake_load_shader(gs_device_t *d, gs_shader_t *s)
{
	(void)d, (void)s;
}

/* ---- device ---- */

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
static const char *fake_preprocessor_name(void)
{
	return "_OPENGL";
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

bool load_graphics_imports(struct gs_exports *exports, void *module, const char *module_name)
{
	(void)module, (void)module_name;
	memset(exports, 0, sizeof(*exports));
	exports->device_preprocessor_name = fake_preprocessor_name;
	exports->device_create = fake_device_create;
	exports->device_destroy = fake_device_destroy;
	exports->device_enter_context = fake_enter;
	exports->device_leave_context = fake_enter;
	exports->device_vertexshader_create = fake_vs_create;
	exports->device_pixelshader_create = fake_vs_create;
	exports->device_vertexbuffer_create = fake_vb_create;
	exports->gs_vertexbuffer_destroy = fake_vb_destroy;
	exports->device_load_vertexshader = fake_load_shader;
	exports->device_load_pixelshader = fake_load_shader;
	exports->gs_shader_destroy = fake_shader_destroy;
	exports->gs_shader_get_num_params = fake_num_params;
	exports->gs_shader_get_param_by_idx = fake_param_by_idx;
	exports->gs_shader_get_param_by_name = fake_param_by_name;
	exports->gs_shader_get_param_info = fake_param_info;
	exports->gs_shader_set_val = fake_set_val;
	exports->gs_shader_set_texture = fake_set_texture;
	exports->gs_shader_set_next_sampler = fake_set_sampler;
	exports->device_blend_function_separate = fake_blend_sep;
	exports->device_blend_op = fake_blend_op;
	exports->device_texture_create = fake_texture_create;
	exports->gs_texture_destroy = fake_texture_destroy;
	exports->gs_texture_get_width = fake_tex_w;
	exports->gs_texture_get_height = fake_tex_h;
	exports->gs_texture_get_color_format = fake_tex_fmt;
	exports->gs_texture_map = fake_tex_map;
	exports->gs_texture_unmap = fake_tex_unmap;
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

int64_t os_ftelli64(FILE *file)
{
	return ftello(file);
}
