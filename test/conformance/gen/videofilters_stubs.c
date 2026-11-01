/*
 * Conformance harness (area videofilters, spec S-028): fake OBS host + recording fake graphics API.
 * Every call a filter makes into libobs/graphics is printed as one line "call NAME args"; no pointers are
 * ever printed (objects have small ids: eN effect, tN texture, rN texrender, sN sampler, S source name).
 * Effects are not compiled: the .effect file is scanned for "uniform TYPE NAME" and "technique NAME" so that
 * parameter and technique names used by the filters can be checked. Test tooling, not part of OBS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/mman.h>
#include "obs.h"
#include "obs-module.h"
#include "obs-missing-files.h"
#include "graphics/graphics.h"
#include "graphics/image-file.h"
#include "util/bmem.h"
#include "util/platform.h"
#include "videofilters_api.h"

struct vf_host g_vf = {.sdr_white = 300.f, .begin_result = 1, .ovi = {.base_width = 1920, .base_height = 1080, .fps_num = 60, .fps_den = 1}};

const char *vf_basename(const char *p)
{
	const char *s = strrchr(p, '/');
	return s ? s + 1 : p;
}

static const char *csn(enum gs_color_space s)
{
	static const char *n[] = {"SRGB", "SRGB_16F", "709_EXTENDED", "709_SCRGB"};
	return (unsigned)s < 4 ? n[s] : "?";
}
static const char *fmtn(enum gs_color_format f)
{
	switch (f) {
	case GS_RGBA: return "RGBA";
	case GS_RGBA16F: return "RGBA16F";
	case GS_BGRA: return "BGRA";
	case GS_UNKNOWN: return "UNKNOWN";
	default: return "OTHER";
	}
}
static const char *bln[] = {"ZERO", "ONE", "SRCCOLOR", "INVSRCCOLOR", "SRCALPHA", "INVSRCALPHA", "DSTCOLOR", "INVDSTCOLOR", "DSTALPHA", "INVDSTALPHA", "SRCALPHASAT"};
static const char *adn[] = {"CLAMP", "WRAP", "MIRROR", "BORDER", "MIRRORONCE"};

/* ---------------- module / host ---------------- */
const char *obs_module_text(const char *k) { return k; }
obs_module_t *obs_current_module(void) { return NULL; }
char *obs_find_module_file(obs_module_t *m, const char *f)
{
	(void)m;
	char b[256];
	snprintf(b, sizeof(b), "@data/%s", f);
	return bstrdup(b);
}
void obs_enter_graphics(void) {}
void obs_leave_graphics(void) {}
bool obs_get_video_info(struct obs_video_info *o) { *o = g_vf.ovi; return true; }
float obs_get_video_sdr_white_level(void) { return g_vf.sdr_white; }
uint64_t obs_get_video_frame_time(void) { return g_vf.frame_time; }
enum gs_color_space gs_get_color_space(void) { return g_vf.cur_space; }

obs_source_t *obs_filter_get_target(const obs_source_t *s) { return s->target; }
obs_source_t *obs_filter_get_parent(const obs_source_t *s) { return s->parent; }
uint32_t obs_source_get_base_width(obs_source_t *s) { return s ? s->bw : 0; }
uint32_t obs_source_get_base_height(obs_source_t *s) { return s ? s->bh : 0; }
uint32_t obs_source_get_width(obs_source_t *s) { return s ? s->w : 0; }
uint32_t obs_source_get_height(obs_source_t *s) { return s ? s->h : 0; }
uint32_t obs_source_get_output_flags(const obs_source_t *s) { return s->flags; }
enum gs_color_space obs_source_get_color_space(obs_source_t *s, size_t c, const enum gs_color_space *p)
{
	(void)c, (void)p;
	return s ? s->space : GS_CS_SRGB;
}
obs_data_t *obs_source_get_settings(const obs_source_t *s)
{
	obs_data_addref(s->settings);
	return s->settings;
}
/* Same contract as libobs: video sources defer the update to the next tick. */
void obs_source_update(obs_source_t *s, obs_data_t *settings)
{
	if (settings)
		obs_data_apply(s->settings, settings);
	if (s->info->output_flags & OBS_SOURCE_VIDEO)
		s->defer_update_count++;
	else if (s->data && s->info->update)
		s->info->update(s->data, s->settings);
	printf("call source_update %s deferred=%d\n", s->name, (s->info->output_flags & OBS_SOURCE_VIDEO) ? 1 : 0);
}
void obs_source_skip_video_filter(obs_source_t *f) { printf("call skip_video_filter %s\n", f->name); }
void obs_source_default_render(obs_source_t *s) { printf("call source_default_render %s\n", s->name); }
void obs_source_video_render(obs_source_t *s) { printf("call source_video_render %s\n", s->name); }

static const char *alw(int a) { return a == OBS_ALLOW_DIRECT_RENDERING ? "ALLOW_DIRECT" : "NO_DIRECT"; }
bool obs_source_process_filter_begin(obs_source_t *f, enum gs_color_format fmt, enum obs_allow_direct_render a)
{
	printf("call filter_begin %s format=%s allow=%s -> %d\n", f->name, fmtn(fmt), alw(a), g_vf.begin_result);
	return g_vf.begin_result;
}
bool obs_source_process_filter_begin_with_color_space(obs_source_t *f, enum gs_color_format fmt, enum gs_color_space sp,
						      enum obs_allow_direct_render a)
{
	printf("call filter_begin_cs %s format=%s space=%s allow=%s -> %d\n", f->name, fmtn(fmt), csn(sp), alw(a),
	       g_vf.begin_result);
	return g_vf.begin_result;
}

/* ---------------- effects ---------------- */
struct fx {
	int id;
	bool base;
	char label[40];
	char names[96][40];
	int n;
	char tech[32][40];
	int nt;
	int loop;
};
struct fparam {
	struct fx *fx;
	char name[40];
};
static struct fx *fxs[64];
static int nfx;
static struct fparam *fps[512];
static int nfp;

static void scan(struct fx *e, const char *path)
{
	FILE *f = fopen(path, "rb");
	if (!f) {
		printf("call effect_scan_failed\n");
		return;
	}
	char tok[64][1]; (void)tok;
	char w[3][64] = {"", "", ""};
	int c, len = 0, state = 0;
	char cur[40];
	while ((c = fgetc(f)) != EOF || len) {
		if (c != EOF && (isalnum(c) || c == '_')) {
			if (len < 39) cur[len++] = (char)c;
			continue;
		}
		if (len) {
			cur[len] = 0;
			if (state == 1) { /* after uniform: type */ state = 2; }
			else if (state == 2) {
				if (e->n < 96) snprintf(e->names[e->n++], 40, "%s", cur);
				state = 0;
			} else if (state == 3) {
				if (e->nt < 32) snprintf(e->tech[e->nt++], 40, "%s", cur);
				state = 0;
			} else if (!strcmp(cur, "uniform")) state = 1;
			else if (!strcmp(cur, "technique")) state = 3;
			len = 0;
		}
		if (c == EOF) break;
	}
	(void)w;
	fclose(f);
}

static struct fx *fx_new(const char *label_prefix, const char *path, bool base)
{
	struct fx *e = bzalloc(sizeof(*e));
	e->id = nfx + 1;
	e->base = base;
	snprintf(e->label, sizeof(e->label), "%s%d", label_prefix, e->id);
	scan(e, path);
	if (nfx < 64) fxs[nfx++] = e;
	return e;
}

gs_effect_t *gs_effect_create_from_file(const char *file, char **err)
{
	(void)err;
	char path[512];
	printf("call effect_create file=%s", vf_basename(file));
	if (g_vf.effect_fail) {
		printf(" -> null\n");
		return NULL;
	}
	snprintf(path, sizeof(path), "../../plugins/obs-filters/data/%s", vf_basename(file));
	struct fx *e = fx_new("e", path, false);
	printf(" -> %s uniforms=%d techniques=%d\n", e->label, e->n, e->nt);
	return (gs_effect_t *)e;
}
void gs_effect_destroy(gs_effect_t *e)
{
	if (e) printf("call effect_destroy %s\n", ((struct fx *)e)->label);
}
gs_effect_t *obs_get_base_effect(enum obs_base_effect t)
{
	static const char *n[] = {"default", "default_rect", "opaque", "solid", "bicubic_scale", "lanczos_scale",
				  "bilinear_lowres_scale", "premultiplied_alpha", "repeat", "area"};
	static struct fx *cache[10];
	if ((unsigned)t > 9) t = 0;
	if (!cache[t]) {
		char path[256];
		snprintf(path, sizeof(path), "../../libobs/data/%s.effect", n[t]);
		cache[t] = fx_new("base", path, true);
		snprintf(cache[t]->label, sizeof(cache[t]->label), "base:%s", n[t]);
	}
	return (gs_effect_t *)cache[t];
}
gs_eparam_t *gs_effect_get_param_by_name(const gs_effect_t *ef, const char *name)
{
	struct fx *e = (struct fx *)ef;
	bool found = false;
	for (int i = 0; i < e->n; i++)
		if (!strcmp(e->names[i], name)) found = true;
	for (int i = 0; i < nfp; i++)
		if (fps[i]->fx == e && !strcmp(fps[i]->name, name)) return found ? (gs_eparam_t *)fps[i] : NULL;
	printf("call get_param %s:%s -> %s\n", e->label, name, found ? "ok" : "null");
	if (!found) return NULL;
	struct fparam *p = bzalloc(sizeof(*p));
	p->fx = e;
	snprintf(p->name, sizeof(p->name), "%s", name);
	if (nfp < 512) fps[nfp++] = p;
	return (gs_eparam_t *)p;
}
static void pn(const gs_eparam_t *p)
{
	const struct fparam *q = (const struct fparam *)p;
	if (!q) printf("@null");
	else printf("%s:%s", q->fx->label, q->name);
}
void gs_effect_set_float(gs_eparam_t *p, float v) { printf("call set_float "); pn(p); printf(" %.9g\n", (double)v); }
void gs_effect_set_vec2(gs_eparam_t *p, const struct vec2 *v) { printf("call set_vec2 "); pn(p); printf(" %.9g %.9g\n", (double)v->x, (double)v->y); }
void gs_effect_set_vec3(gs_eparam_t *p, const struct vec3 *v) { printf("call set_vec3 "); pn(p); printf(" %.9g %.9g %.9g\n", (double)v->x, (double)v->y, (double)v->z); }
void gs_effect_set_vec4(gs_eparam_t *p, const struct vec4 *v) { printf("call set_vec4 "); pn(p); printf(" %.9g %.9g %.9g %.9g\n", (double)v->x, (double)v->y, (double)v->z, (double)v->w); }
void gs_effect_set_matrix4(gs_eparam_t *p, const struct matrix4 *m)
{
	printf("call set_matrix4 ");
	pn(p);
	const float *f = (const float *)m;
	for (int i = 0; i < 16; i++) printf(" %.9g", (double)f[i]);
	putchar('\n');
}

/* ---------------- textures / samplers / texrender ---------------- */
struct ftex { int id, w, h, d; enum gs_color_format fmt; };
struct fsamp { int id; };
struct ftr { int id; enum gs_color_format fmt; uint32_t cx, cy; bool rendered; struct ftex *target; };
static int tex_ids, samp_ids, tr_ids;

static struct ftex *tex_new(int w, int h, int d, enum gs_color_format f)
{
	struct ftex *t = bzalloc(sizeof(*t));
	t->id = ++tex_ids; t->w = w; t->h = h; t->d = d; t->fmt = f;
	return t;
}
void gs_effect_set_texture_srgb(gs_eparam_t *p, gs_texture_t *t)
{
	printf("call set_texture_srgb "); pn(p);
	if (t) printf(" t%d\n", ((struct ftex *)t)->id); else printf(" @null\n");
}
void gs_effect_set_next_sampler(gs_eparam_t *p, gs_samplerstate_t *s)
{
	printf("call set_next_sampler "); pn(p);
	if (s) printf(" s%d\n", ((struct fsamp *)s)->id); else printf(" @null\n");
}
bool gs_effect_loop(gs_effect_t *ef, const char *name)
{
	struct fx *e = (struct fx *)ef;
	if (!e->loop) {
		bool ok = false;
		for (int i = 0; i < e->nt; i++) if (!strcmp(e->tech[i], name)) ok = true;
		printf("call effect_loop %s %s technique_exists=%d\n", e->label, name, ok);
		e->loop = 1;
		return true;
	}
	e->loop = 0;
	return false;
}
void obs_source_process_filter_end(obs_source_t *f, gs_effect_t *ef, uint32_t w, uint32_t h)
{
	printf("call filter_end %s %s size=%ux%u\n", f->name, ((struct fx *)ef)->label, w, h);
}
void obs_source_process_filter_tech_end(obs_source_t *f, gs_effect_t *ef, uint32_t w, uint32_t h, const char *tech)
{
	struct fx *e = (struct fx *)ef;
	bool ok = false;
	for (int i = 0; i < e->nt; i++) if (!strcmp(e->tech[i], tech)) ok = true;
	printf("call filter_tech_end %s %s size=%ux%u technique=%s exists=%d\n", f->name, e->label, w, h, tech, ok);
}
void gs_blend_state_push(void) { printf("call blend_push\n"); }
void gs_blend_state_pop(void) { printf("call blend_pop\n"); }
void gs_blend_function(enum gs_blend_type s, enum gs_blend_type d) { printf("call blend_function %s %s\n", bln[s], bln[d]); }
void gs_enable_framebuffer_srgb(bool en) { g_vf.fb_srgb = en; printf("call enable_framebuffer_srgb %d\n", en); }
bool gs_framebuffer_srgb_enabled(void) { return g_vf.fb_srgb; }
void gs_clear(uint32_t fl, const struct vec4 *c, float d, uint8_t s)
{
	printf("call clear flags=%u color=%.9g,%.9g,%.9g,%.9g depth=%.9g stencil=%u\n", fl, (double)c->x, (double)c->y, (double)c->z, (double)c->w, (double)d, s);
}
void gs_ortho(float l, float r, float t, float b, float n, float f)
{
	printf("call ortho %.9g %.9g %.9g %.9g %.9g %.9g\n", (double)l, (double)r, (double)t, (double)b, (double)n, (double)f);
}
void gs_draw_sprite(gs_texture_t *t, uint32_t flip, uint32_t w, uint32_t h)
{
	printf("call draw_sprite "); if (t) printf("t%d", ((struct ftex *)t)->id); else printf("@null");
	printf(" flip=%u size=%ux%u\n", flip, w, h);
}
gs_samplerstate_t *gs_samplerstate_create(const struct gs_sampler_info *i)
{
	static const char *fn[] = {"POINT", "LINEAR"};
	struct fsamp *s = bzalloc(sizeof(*s));
	s->id = ++samp_ids;
	printf("call samplerstate_create s%d filter=%s u=%s v=%s w=%s aniso=%d border=%u\n", s->id, i->filter < 2 ? fn[i->filter] : "OTHER",
	       adn[i->address_u], adn[i->address_v], adn[i->address_w], i->max_anisotropy, i->border_color);
	return (gs_samplerstate_t *)s;
}
void gs_samplerstate_destroy(gs_samplerstate_t *s)
{
	if (s) printf("call samplerstate_destroy s%d\n", ((struct fsamp *)s)->id);
}

static uint64_t fnv(const uint8_t *d, size_t n)
{
	uint64_t h = 1469598103934665603ULL;
	for (size_t i = 0; i < n; i++) { h ^= d[i]; h *= 1099511628211ULL; }
	return h;
}
static gs_texture_t *make_tex(const char *kind, int w, int h, int d, enum gs_color_format f, const uint8_t *data)
{
	if (g_vf.tex_fail) { printf("call %s_create %dx%dx%d format=%s -> null\n", kind, w, h, d, fmtn(f)); return NULL; }
	struct ftex *t = tex_new(w, h, d, f);
	printf("call %s_create t%d %dx%dx%d format=%s", kind, t->id, w, h, d, fmtn(f));
	if (data) {
		size_t bytes = (size_t)w * h * d * (gs_get_format_bpp(f) / 8);
		printf(" bytes=%zu fnv=%016llx", bytes, (unsigned long long)fnv(data, bytes));
		if (bytes <= 1024) { printf(" data="); for (size_t i = 0; i < bytes; i++) printf("%02x", data[i]); }
		else if (gs_get_format_bpp(f) == 32 && d > 1) {
			static const int v[][3] = {{0,0,0},{1,0,0},{0,1,0},{0,0,1},{63,0,0},{0,63,0},{0,0,63},{5,7,9},{63,63,63}};
			printf(" probe");
			for (int i = 0; i < 9; i++) {
				const uint8_t *p = data + 4 * ((size_t)v[i][2] * h * w + (size_t)v[i][1] * w + v[i][0]);
				printf(" (%d,%d,%d)=%u", v[i][0], v[i][1], v[i][2], p[0] | (p[1] << 8) | (p[2] << 16));
			}
		}
	} else printf(" nodata");
	putchar('\n');
	return (gs_texture_t *)t;
}
gs_texture_t *gs_texture_create(uint32_t w, uint32_t h, enum gs_color_format f, uint32_t lv, const uint8_t **data, uint32_t fl)
{
	(void)lv, (void)fl;
	return make_tex("texture", (int)w, (int)h, 1, f, data ? data[0] : NULL);
}
gs_texture_t *gs_voltexture_create(uint32_t w, uint32_t h, uint32_t d, enum gs_color_format f, uint32_t lv, const uint8_t **data, uint32_t fl)
{
	(void)lv, (void)fl;
	return make_tex("voltexture", (int)w, (int)h, (int)d, f, data ? data[0] : NULL);
}
void gs_voltexture_destroy(gs_texture_t *t) { if (t) printf("call voltexture_destroy t%d\n", ((struct ftex *)t)->id); }
uint32_t gs_texture_get_width(const gs_texture_t *t) { return (uint32_t)((const struct ftex *)t)->w; }
uint32_t gs_texture_get_height(const gs_texture_t *t) { return (uint32_t)((const struct ftex *)t)->h; }

/* texrender with the semantics of the real library: begin fails if already rendered or a dimension is 0;
 * the target texture is (re)created when the size changes; end sets rendered; reset clears it. */
gs_texrender_t *gs_texrender_create(enum gs_color_format f, enum gs_zstencil_format z)
{
	struct ftr *r = bzalloc(sizeof(*r));
	r->id = ++tr_ids; r->fmt = f;
	printf("call texrender_create r%d format=%s zs=%d\n", r->id, fmtn(f), (int)z);
	return (gs_texrender_t *)r;
}
void gs_texrender_destroy(gs_texrender_t *t) { if (t) printf("call texrender_destroy r%d\n", ((struct ftr *)t)->id); }
bool gs_texrender_begin_with_color_space(gs_texrender_t *t, uint32_t cx, uint32_t cy, enum gs_color_space sp)
{
	struct ftr *r = (struct ftr *)t;
	bool ok = true;
	if (r->rendered || !cx || !cy) ok = false;
	else {
		if (r->cx != cx || r->cy != cy) {
			r->cx = cx; r->cy = cy;
			r->target = g_vf.texrender_fail ? NULL : tex_new((int)cx, (int)cy, 1, r->fmt);
		}
		if (!r->target) ok = false;
	}
	printf("call texrender_begin r%d %ux%u space=%s -> %d\n", r->id, cx, cy, csn(sp), ok);
	return ok;
}
void gs_texrender_end(gs_texrender_t *t) { struct ftr *r = (struct ftr *)t; r->rendered = true; printf("call texrender_end r%d\n", r->id); }
void gs_texrender_reset(gs_texrender_t *t) { struct ftr *r = (struct ftr *)t; r->rendered = false; printf("call texrender_reset r%d\n", r->id); }
gs_texture_t *gs_texrender_get_texture(const gs_texrender_t *t)
{
	const struct ftr *r = (const struct ftr *)t;
	printf("call texrender_get_texture r%d -> ", r->id);
	if (r->target) printf("t%d\n", r->target->id); else printf("@null\n");
	return (gs_texture_t *)r->target;
}
enum gs_color_format gs_texrender_get_format(const gs_texrender_t *t) { return ((const struct ftr *)t)->fmt; }

/* ---------------- images ---------------- */
struct img { char name[64]; uint32_t cx, cy; enum gs_color_format fmt; bool loaded, animated; };
static struct img imgs[16];
static int nimg;
void vf_define_image(const char *name, uint32_t cx, uint32_t cy, enum gs_color_format fmt, bool loaded, bool animated)
{
	struct img *i = &imgs[nimg++ % 16];
	snprintf(i->name, sizeof(i->name), "%s", name);
	i->cx = cx; i->cy = cy; i->fmt = fmt; i->loaded = loaded; i->animated = animated;
}
static size_t map_len(const gs_image_file_ex_t *im) { return (size_t)im->cx * im->cy * 4; }
void gs_image_file_ex_init(gs_image_file_ex_t *im, const char *file, enum gs_image_alpha_mode am)
{
	memset(im, 0, sizeof(*im));
	im->alpha_mode = am;
	printf("call image_init %s\n", vf_basename(file));
	for (int k = 0; k < 16; k++) {
		struct img *i = &imgs[k];
		if (!i->name[0] || strcmp(i->name, vf_basename(file))) continue;
		im->loaded = i->loaded; im->cx = i->cx; im->cy = i->cy; im->format = i->fmt; im->is_animated_gif = i->animated;
		if (!i->loaded) return;
		/* pixel n = bytes (n, n>>8, n>>16, 255); the data ends exactly at a PROT_NONE guard page */
		size_t len = map_len(im), pg = 4096, tot = (len + pg - 1) / pg * pg;
		uint8_t *m = mmap(NULL, tot + pg, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		mprotect(m + tot, pg, PROT_NONE);
		uint8_t *d = m + tot - len;
		for (size_t n = 0; n < (size_t)im->cx * im->cy; n++) { d[4*n] = n & 255; d[4*n+1] = (n >> 8) & 255; d[4*n+2] = (n >> 16) & 255; d[4*n+3] = 255; }
		im->texture_data = d;
		im->mem_usage = tot;
		return;
	}
}
void gs_image_file_ex_free(gs_image_file_ex_t *im)
{
	printf("call image_free\n");
	if (im->texture_data) {
		size_t len = map_len(im), pg = 4096, tot = (len + pg - 1) / pg * pg;
		munmap(im->texture_data - (tot - len), tot + pg);
	}
	memset(im, 0, sizeof(*im));
}
void gs_image_file_ex_init_texture(gs_image_file_ex_t *im)
{
	if (im->loaded) im->texture = (gs_texture_t *)tex_new((int)im->cx, (int)im->cy, 1, im->format);
	printf("call image_init_texture -> %s\n", im->texture ? "ok" : "none");
}
bool gs_image_file_ex_tick(gs_image_file_ex_t *im, uint64_t ns) { (void)im; printf("call image_tick elapsed_ns=%llu\n", (unsigned long long)ns); return true; }
void gs_image_file_ex_update_texture(gs_image_file_ex_t *im) { (void)im; printf("call image_update_texture\n"); }

/* ---------------- missing files ---------------- */
static struct { void *src; obs_missing_file_cb cb; void *data; } mf;
obs_missing_files_t *obs_missing_files_create(void) { printf("call missing_files_create\n"); return (obs_missing_files_t *)&mf; }
obs_missing_file_t *obs_missing_file_create(const char *path, obs_missing_file_cb cb, int type, void *src, void *data)
{
	printf("call missing_file_create path=%s type=%d\n", vf_basename(path), type);
	mf.src = src; mf.cb = cb; mf.data = data;
	return (obs_missing_file_t *)&mf;
}
void obs_missing_files_add_file(obs_missing_files_t *f, obs_missing_file_t *file) { (void)f, (void)file; printf("call missing_files_add\n"); }
void vf_resolve_missing(const char *np) { mf.cb(((struct obs_source *)mf.src)->data, np, mf.data); }
