/*
 * Conformance harness (area textft2, spec S-038): fake FreeType, fake graphics subsystem and the few OBS core
 * functions the text-freetype2 plugin calls. All glyph metrics are scripted; see spec S-038 section "Fake host".
 * Test tooling, not part of libobs.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <obs-module.h>
#include <graphics/graphics.h>
#include <util/bmem.h>
#include <util/text-lookup.h>
#include "textft2_ft.h"
#include "textft2_api.h"

bool fake_events = true;
static void ev(const char *fmt, ...)
{
	if (!fake_events)
		return;
	va_list ap;
	fputs("ev ", stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

/* ------------------------------------------------------------------ fake FreeType */
struct fglyph {
	bool defined;
	int w, h, left, top;
	long adv;
};
#define MAXIDX 4096
static struct fglyph glyphs[MAXIDX];
static struct {
	unsigned long cp;
	unsigned idx;
} cmap[1024];
static int ncmap;
static int pitch_pad;
static bool fail_init, fail_face;

struct face_priv {
	unsigned idx;
	int mode;
	unsigned char *buf;
	FT_GlyphSlotRec slot;
};

void fake_ft_reset(void)
{
	memset(glyphs, 0, sizeof(glyphs));
	ncmap = 0;
	pitch_pad = 0;
	fail_init = fail_face = false;
	/* default .notdef (glyph index 0) */
	glyphs[0] = (struct fglyph){true, 6, 10, 1, 10, 8 * 64};
}
void fake_ft_glyph(unsigned long cp, unsigned index, int w, int h, int left, int top, long adv)
{
	if (index >= MAXIDX || ncmap >= 1024) {
		fprintf(stderr, "fake ft: glyph table full\n");
		exit(2);
	}
	glyphs[index] = (struct fglyph){true, w, h, left, top, adv};
	for (int i = 0; i < ncmap; i++)
		if (cmap[i].cp == cp) {
			cmap[i].idx = index;
			return;
		}
	cmap[ncmap].cp = cp;
	cmap[ncmap++].idx = index;
}
void fake_ft_notdef(int w, int h, int left, int top, long adv)
{
	glyphs[0] = (struct fglyph){true, w, h, left, top, adv};
}
void fake_ft_pitch_pad(int pad)
{
	pitch_pad = pad;
}
void fake_ft_fail_init(bool f)
{
	fail_init = f;
}
void fake_ft_fail_face(bool f)
{
	fail_face = f;
}

FT_Error FT_Init_FreeType(FT_Library *lib)
{
	ev("ft init %s", fail_init ? "fail" : "ok");
	if (fail_init) {
		*lib = NULL;
		return 1;
	}
	*lib = (FT_Library)malloc(1);
	return 0;
}
FT_Error FT_Done_FreeType(FT_Library lib)
{
	ev("ft done_library");
	free(lib);
	return 0;
}
FT_Error FT_New_Face(FT_Library lib, const char *path, FT_Long index, FT_Face *face)
{
	(void)lib;
	ev("ft new_face path=%s index=%ld %s", path, index, fail_face ? "fail" : "ok");
	if (fail_face)
		return 1;
	struct face_priv *p = calloc(1, sizeof(*p));
	FT_Face f = calloc(1, sizeof(*f));
	f->priv = p;
	f->glyph = &p->slot;
	*face = f;
	return 0;
}
FT_Error FT_Done_Face(FT_Face face)
{
	ev("ft done_face");
	struct face_priv *p = face->priv;
	free(p->buf);
	free(p);
	free(face);
	return 0;
}
FT_Error FT_Set_Pixel_Sizes(FT_Face face, FT_UInt w, FT_UInt h)
{
	(void)face;
	ev("ft set_pixel_sizes %u %u", w, h);
	return 0;
}
FT_Error FT_Select_Charmap(FT_Face face, FT_Encoding enc)
{
	(void)face;
	ev("ft select_charmap unicode=%d", enc == FT_ENCODING_UNICODE);
	return 0;
}
FT_UInt FT_Get_Char_Index(FT_Face face, unsigned long code)
{
	(void)face;
	for (int i = 0; i < ncmap; i++)
		if (cmap[i].cp == code)
			return cmap[i].idx;
	return 0;
}
FT_Error FT_Load_Glyph(FT_Face face, FT_UInt index, FT_Int32 flags)
{
	struct face_priv *p = face->priv;
	p->idx = index;
	p->mode = flags == FT_LOAD_TARGET_MONO ? FT_RENDER_MODE_MONO : FT_RENDER_MODE_NORMAL;
	p->slot.advance.x = index < MAXIDX ? glyphs[index].adv : 0;
	p->slot.advance.y = 0;
	return 0;
}
/* Pixel model (documented in the spec): AA value(x,y) = (index*31 + x*17 + y*29) % 251 + 1;
 * mono bit(x,y) = (index + x + y) & 1; AA pitch = width + pad; mono pitch = (width+7)/8. */
FT_Error FT_Render_Glyph(FT_GlyphSlot slot, FT_Render_Mode mode)
{
	/* slot is &priv->slot */
	struct face_priv *p = (struct face_priv *)((char *)slot - offsetof(struct face_priv, slot));
	struct fglyph *g = &glyphs[p->idx];
	free(p->buf);
	p->buf = NULL;
	slot->bitmap.rows = (unsigned)g->h;
	slot->bitmap.width = (unsigned)g->w;
	slot->bitmap_left = g->left;
	slot->bitmap_top = g->top;
	int pitch = mode == FT_RENDER_MODE_MONO ? (g->w + 7) / 8 : g->w + pitch_pad;
	slot->bitmap.pitch = pitch;
	if (g->w > 0 && g->h > 0) {
		p->buf = calloc((size_t)pitch * (size_t)g->h, 1);
		for (int y = 0; y < g->h; y++)
			for (int x = 0; x < g->w; x++) {
				if (mode == FT_RENDER_MODE_MONO) {
					if ((p->idx + x + y) & 1)
						p->buf[y * pitch + x / 8] |= (unsigned char)(0x80 >> (x % 8));
				} else {
					p->buf[y * pitch + x] = (unsigned char)((p->idx * 31 + x * 17 + y * 29) % 251 + 1);
				}
			}
	}
	slot->bitmap.buffer = p->buf;
	return 0;
}

/* ------------------------------------------------------------------ font lookup (find-font.h) */
static struct {
	char name[64], style[64], path[64];
	uint32_t flags;
} ftab[32];
static int nftab;
void fake_font_table(const char *name, const char *style, uint32_t flags, const char *path)
{
	snprintf(ftab[nftab].name, 64, "%s", name);
	snprintf(ftab[nftab].style, 64, "%s", style);
	snprintf(ftab[nftab].path, 64, "%s", path);
	ftab[nftab++].flags = flags;
}
const char *get_font_path(const char *family, uint16_t size, const char *style, uint32_t flags, FT_Long *idx)
{
	ev("font_lookup face=%s size=%u style=%s flags=%u", family, size, style, flags);
	*idx = 0;
	for (int i = 0; i < nftab; i++)
		if (!strcmp(ftab[i].name, family) && !strcmp(ftab[i].style, style) && ftab[i].flags == flags) {
			*idx = i;
			return ftab[i].path;
		}
	return NULL;
}
bool load_cached_os_font_list(void)
{
	return true;
}
void load_os_font_list(void) {}
void free_os_font_list(void)
{
	ev("font_list_freed");
}

/* ------------------------------------------------------------------ fake graphics */
static bool fail_effect, linear;
void fake_gfx_fail_effect(bool f)
{
	fail_effect = f;
}
void fake_gfx_linear(bool v)
{
	linear = v;
}

uint64_t fake_clock_ns;
void fake_clock_set(uint64_t ns)
{
	fake_clock_ns = ns;
}
uint64_t fake_clock_get(void)
{
	return fake_clock_ns;
}

struct gs_vertex_buffer {
	struct gs_vb_data *data;
};
struct gs_texture {
	uint32_t w, h;
};
struct gs_effect {
	int x;
};

static float mstack[32][2];
static int msp;
static gs_vertbuffer_t *cur_vb;
static bool fb_srgb;

void obs_enter_graphics(void) {}
void obs_leave_graphics(void) {}
void gs_reset_blend_state(void)
{
	msp = 0;
	mstack[0][0] = mstack[0][1] = 0;
	ev("gfx reset_blend");
}
void gs_matrix_push(void)
{
	memcpy(mstack[msp + 1], mstack[msp], sizeof(mstack[0]));
	msp++;
}
void gs_matrix_pop(void)
{
	if (msp > 0)
		msp--;
}
void gs_matrix_identity(void)
{
	mstack[msp][0] = mstack[msp][1] = 0;
}
void gs_matrix_translate3f(float x, float y, float z)
{
	(void)z;
	mstack[msp][0] += x;
	mstack[msp][1] += y;
}
bool gs_get_linear_srgb(void)
{
	return linear;
}
bool gs_framebuffer_srgb_enabled(void)
{
	return fb_srgb;
}
void gs_enable_framebuffer_srgb(bool e)
{
	fb_srgb = e;
}
void gs_load_vertexbuffer(gs_vertbuffer_t *v)
{
	cur_vb = v;
}
void gs_load_indexbuffer(gs_indexbuffer_t *i)
{
	(void)i;
}
gs_technique_t *gs_effect_get_technique(const gs_effect_t *e, const char *n)
{
	(void)e, (void)n;
	return (gs_technique_t *)&fail_effect;
}
gs_eparam_t *gs_effect_get_param_by_name(const gs_effect_t *e, const char *n)
{
	(void)e;
	return (gs_eparam_t *)n;
}
size_t gs_technique_begin(gs_technique_t *t)
{
	(void)t;
	return 1;
}
bool gs_technique_begin_pass(gs_technique_t *t, size_t pass)
{
	(void)t, (void)pass;
	return true;
}
void gs_technique_end_pass(gs_technique_t *t)
{
	(void)t;
}
void gs_technique_end(gs_technique_t *t)
{
	(void)t;
}
void gs_effect_set_texture(gs_eparam_t *p, gs_texture_t *t)
{
	(void)p, (void)t;
	ev("gfx set_texture srgb=0");
}
void gs_effect_set_texture_srgb(gs_eparam_t *p, gs_texture_t *t)
{
	(void)p, (void)t;
	ev("gfx set_texture srgb=1");
}
static int last_use_color;
void gs_effect_set_bool(gs_eparam_t *p, bool v)
{
	(void)p;
	last_use_color = v;
}
void gs_draw(enum gs_draw_mode m, uint32_t start, uint32_t n)
{
	(void)m;
	(void)cur_vb;
	ev("gfx draw tris start=%u verts=%u use_color=%d offset=%.9g,%.9g", start, n, last_use_color,
	   (double)mstack[msp][0], (double)mstack[msp][1]);
}
gs_effect_t *gs_effect_create_from_file(const char *file, char **error_string)
{
	(void)error_string;
	ev("gfx effect_create file=%s %s", file, fail_effect ? "fail" : "ok");
	return fail_effect ? NULL : calloc(1, sizeof(struct gs_effect));
}
void gs_effect_destroy(gs_effect_t *e)
{
	ev("gfx effect_destroy");
	free(e);
}
gs_vertbuffer_t *gs_vertexbuffer_create(struct gs_vb_data *data, uint32_t flags)
{
	(void)flags;
	struct gs_vertex_buffer *v = calloc(1, sizeof(*v));
	v->data = data;
	ev("gfx vertexbuffer_create verts=%zu colors=%d", data->num, data->colors != NULL);
	return v;
}
void gs_vertexbuffer_destroy(gs_vertbuffer_t *vb)
{
	ev("gfx vertexbuffer_destroy");
	if (vb) {
		struct gs_vb_data *d = vb->data;
		bfree(d->points);
		bfree(d->tvarray[0].array);
		bfree(d->tvarray);
		bfree(d->colors);
		bfree(d);
		free(vb);
	}
}
void gs_vertexbuffer_flush(gs_vertbuffer_t *vb)
{
	(void)vb;
	ev("gfx vertexbuffer_flush");
}
struct gs_vb_data *gs_vertexbuffer_get_data(const gs_vertbuffer_t *vb)
{
	return vb->data;
}
gs_texture_t *gs_texture_create(uint32_t w, uint32_t h, enum gs_color_format fmt, uint32_t lv, const uint8_t **data,
				uint32_t flags)
{
	(void)lv, (void)data, (void)flags;
	struct gs_texture *t = calloc(1, sizeof(*t));
	t->w = w;
	t->h = h;
	ev("gfx texture_create %ux%u a8=%d", w, h, fmt == GS_A8);
	return t;
}
void gs_texture_destroy(gs_texture_t *t)
{
	ev("gfx texture_destroy");
	free(t);
}

/* ------------------------------------------------------------------ OBS core pieces */
bool text_lookup_getstr(lookup_t *l, const char *key, const char **out)
{
	(void)l, (void)key, (void)out;
	return false;
}
void text_lookup_destroy(lookup_t *l)
{
	(void)l;
}
lookup_t *obs_module_load_locale(obs_module_t *m, const char *d, const char *l)
{
	(void)m, (void)d, (void)l;
	return NULL;
}
char *obs_find_module_file(obs_module_t *m, const char *file)
{
	(void)m;
	return bstrdup(file);
}
char *obs_module_get_config_path(obs_module_t *m, const char *file)
{
	(void)m;
	const char *t = getenv("CONFORMANCE_TMPDIR");
	char buf[1024];
	snprintf(buf, sizeof(buf), "%s/ft2cfg%s%s", t ? t : ".", file ? "/" : "", file ? file : "");
	return bstrdup(buf);
}
