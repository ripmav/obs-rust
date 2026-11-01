/*
 * Conformance harness (area imagesource, spec S-030): the fake OBS host.
 * Replaces libobs sources/transitions/hotkeys/task queues, the graphics API, the image decoder and the
 * file system for the plugin code under test. Every input of the fake is scriptable and documented in
 * spec S-030 (section "Fake host"). Test tooling, not part of libobs.
 */
#include <dirent.h>
#include <unistd.h>
#include <inttypes.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <obs-module.h>
#include <obs-hotkey.h>
#include <obs-missing-files.h>
#include <util/platform.h>
#include <util/task.h>
#include <util/dstr.h>
#include <util/bmem.h>
#include <graphics/graphics.h>
#include <graphics/image-file.h>
#include <graphics/vec4.h>
#include "script.h"
#include "imagesource_host.h"

uint64_t host_clock_ns;
int host_srgb_fb, host_linear_srgb, host_task_auto;
uint32_t host_base_w = 1920, host_base_h = 1080;

static void ev(const char *fmt, ...)
{
	va_list ap;
	fputs("ev ", stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

/* ---------------- libc interposition: rand and stat ---------------- */
static int rand_vals[256], rand_n, rand_pos;
void host_add_rand(const int *v, int n);
void host_reset_rand(const int *v, int n)
{
	rand_n = 0;
	rand_pos = 0;
	host_add_rand(v, n);
}
void host_add_rand(const int *v, int n)
{
	for (int i = 0; i < n && rand_n < 256; i++)
		rand_vals[rand_n++] = v[i];
}
int rand(void)
{
	if (rand_pos >= rand_n) {
		printf("fatal rand sequence exhausted\n");
		fflush(stdout);
		_exit(3);
	}
	int v = rand_vals[rand_pos++];
	printf("ev rand -> %d\n", v);
	return v;
}

/* ---------------- fake file system ---------------- */
struct ffile {
	char path[256];
	long long mtime;
	bool used;
};
struct fdir {
	char path[256];
	int n;
	char ent[16][64];
	bool isdir[16];
	bool used;
};
static struct ffile files[64];
static struct fdir dirs[16];
void host_fs_file(const char *path, long long mtime)
{
	for (int i = 0; i < 64; i++)
		if (files[i].used && !strcmp(files[i].path, path)) {
			files[i].mtime = mtime;
			return;
		}
	for (int i = 0; i < 64; i++)
		if (!files[i].used) {
			files[i].used = true;
			snprintf(files[i].path, 256, "%s", path);
			files[i].mtime = mtime;
			return;
		}
}
void host_fs_rm(const char *path)
{
	for (int i = 0; i < 64; i++)
		if (files[i].used && !strcmp(files[i].path, path))
			files[i].used = false;
}
void host_fs_dir(const char *path, int n, char **entries)
{
	for (int i = 0; i < 16; i++)
		if (!dirs[i].used) {
			dirs[i].used = true;
			snprintf(dirs[i].path, 256, "%s", path);
			dirs[i].n = n;
			for (int k = 0; k < n && k < 16; k++) {
				size_t l = strlen(entries[k]);
				dirs[i].isdir[k] = l && entries[k][l - 1] == '/';
				snprintf(dirs[i].ent[k], 64, "%.*s", (int)(dirs[i].isdir[k] ? l - 1 : l), entries[k]);
			}
			return;
		}
}
int stat(const char *path, struct stat *st)
{
	for (int i = 0; i < 64; i++)
		if (files[i].used && !strcmp(files[i].path, path)) {
			memset(st, 0, sizeof(*st));
			st->st_mtime = (time_t)files[i].mtime;
			return 0;
		}
	return -1;
}
bool host_os_file_exists(const char *path)
{
	struct stat st;
	return stat(path, &st) == 0;
}
const char *os_get_path_extension(const char *path)
{
	for (size_t pos = strlen(path); pos > 0; pos--) {
		switch (path[pos - 1]) {
		case '.':
			return path + pos - 1;
		case '/':
		case '\\':
			return NULL;
		}
	}
	return NULL;
}
struct os_dir {
	struct fdir *d;
	int pos;
	struct os_dirent out;
};
os_dir_t *os_opendir(const char *path)
{
	for (int i = 0; i < 16; i++)
		if (dirs[i].used && !strcmp(dirs[i].path, path)) {
			struct os_dir *d = bzalloc(sizeof(*d));
			d->d = &dirs[i];
			return d;
		}
	return NULL;
}
struct os_dirent *os_readdir(os_dir_t *dir)
{
	if (!dir || dir->pos >= dir->d->n)
		return NULL;
	snprintf(dir->out.d_name, sizeof(dir->out.d_name), "%s", dir->d->ent[dir->pos]);
	dir->out.directory = dir->d->isdir[dir->pos];
	dir->pos++;
	return &dir->out;
}
void os_closedir(os_dir_t *dir)
{
	bfree(dir);
}

/* ---------------- fake image decoder ---------------- */
struct fimg {
	char path[256];
	int w, h, anim, loop_max, filesize, n, delay[64];
	bool used;
};
static struct fimg imgs[32];
static struct fimg *img_find(const char *p)
{
	for (int i = 0; i < 32; i++)
		if (imgs[i].used && !strcmp(imgs[i].path, p))
			return &imgs[i];
	return NULL;
}
static struct fimg *img_new(const char *p)
{
	struct fimg *f = img_find(p);
	for (int i = 0; !f && i < 32; i++)
		if (!imgs[i].used)
			f = &imgs[i];
	memset(f, 0, sizeof(*f));
	f->used = true;
	snprintf(f->path, 256, "%s", p);
	return f;
}
void host_img_still(const char *path, int w, int h)
{
	struct fimg *f = img_new(path);
	f->w = w;
	f->h = h;
}
void host_img_anim(const char *path, int w, int h, int loop_max, int filesize, int n, const int *delays)
{
	struct fimg *f = img_new(path);
	f->w = w;
	f->h = h;
	f->anim = 1;
	f->loop_max = loop_max;
	f->filesize = filesize;
	f->n = n > 64 ? 64 : n;
	for (int i = 0; i < f->n; i++)
		f->delay[i] = delays[i];
}
struct gs_image_file_internal {
	struct fimg *img;
};
struct gs_texture {
	int id;
};
static int tex_ids;

void gs_image_file_ex_init(gs_image_file_ex_t *image, const char *file, enum gs_image_alpha_mode alpha_mode)
{
	memset(image, 0, sizeof(*image));
	if (!file)
		return;
	ev("image_init %s alpha=%d", file, (int)alpha_mode);
	image->internal = bzalloc(sizeof(*image->internal));
	struct fimg *f = img_find(file);
	size_t len = strlen(file);
	if (len > 4 && astrcmpi(file + len - 4, ".gif") == 0) {
		if (!f) {
			blog(LOG_WARNING, "gs_image_file_ex_init_internal: Failed to open file '%s'", file);
			bfree(image->internal);
			image->internal = NULL;
			return;
		}
		if (f->anim && f->n > 1) {
			image->internal->img = f;
			image->is_animated_gif = true;
			image->cx = (uint32_t)f->w;
			image->cy = (uint32_t)f->h;
			image->format = GS_RGBA;
			image->mem_usage = (uint64_t)f->n * sizeof(uint8_t *) + (uint64_t)f->w * f->h * 4 * f->n +
					   (uint64_t)4 * f->w * f->h + (uint64_t)f->filesize;
			image->loaded = true;
			return;
		}
	}
	if (f) {
		image->internal->img = f;
		image->cx = (uint32_t)f->w;
		image->cy = (uint32_t)f->h;
		image->format = GS_RGBA;
		image->texture_data = bzalloc(1);
		image->mem_usage = (uint64_t)image->cx * image->cy * 4;
		image->loaded = true;
	} else {
		blog(LOG_WARNING, "gs_image_file_ex_init_internal: Failed to load file '%s'", file);
		bfree(image->internal);
		image->internal = NULL;
	}
}
void gs_image_file_ex_free(gs_image_file_ex_t *image)
{
	if (image->loaded && image->texture) {
		ev("texture_destroy %d", image->texture->id);
		bfree(image->texture);
	}
	bfree(image->texture_data);
	bfree(image->internal);
	memset(image, 0, sizeof(*image));
}
void gs_image_file_ex_init_texture(gs_image_file_ex_t *image)
{
	if (!image->loaded)
		return;
	image->texture = bzalloc(sizeof(*image->texture));
	image->texture->id = ++tex_ids;
	ev("texture_create %d %ux%u dynamic=%d", image->texture->id, image->cx, image->cy, image->is_animated_gif);
	if (!image->is_animated_gif) {
		bfree(image->texture_data);
		image->texture_data = NULL;
	}
}
static uint64_t frame_ns(gs_image_file_ex_t *im, int i)
{
	uint64_t v = (uint64_t)im->internal->img->delay[i] * 10000000ULL;
	return v ? v : 100000000ULL;
}
bool gs_image_file_ex_tick(gs_image_file_ex_t *image, uint64_t elapsed_time_ns)
{
	if (!image->is_animated_gif || !image->loaded || !image->internal)
		return false;
	int loops = image->internal->img->loop_max;
	if (loops >= 0xFFFF)
		loops = 0;
	if (!loops || image->cur_loop < loops) {
		int nf = image->cur_frame;
		image->cur_time += elapsed_time_ns;
		for (;;) {
			uint64_t t = frame_ns(image, nf);
			if (image->cur_time <= t)
				break;
			image->cur_time -= t;
			if ((unsigned)++nf == (unsigned)image->internal->img->n) {
				if (!loops || ++image->cur_loop < loops) {
					nf = 0;
				} else if (image->cur_loop == loops) {
					nf--;
					break;
				}
			}
		}
		if (nf != image->cur_frame) {
			image->cur_frame = nf;
			return true;
		}
	}
	return false;
}
void gs_image_file_ex_update_texture(gs_image_file_ex_t *image)
{
	if (!image->is_animated_gif || !image->loaded || !image->internal)
		return;
	ev("texture_set_image %d frame=%d", image->texture->id, image->cur_frame);
}

/* ---------------- fake graphics ---------------- */
struct gs_effect {
	const char *name;
};
struct gs_effect_param {
	char name[32];
};
struct gs_effect_technique {
	char name[32];
};
static struct gs_effect fx_solid = {"solid"}, fx_default = {"default"};
static struct gs_effect_param params[16];
static struct gs_effect_technique techs[4];
static int nparams, ntechs;
gs_effect_t *host_default_effect(void)
{
	return &fx_default;
}
gs_effect_t *obs_get_base_effect(enum obs_base_effect e)
{
	(void)e;
	return &fx_solid;
}
gs_eparam_t *gs_effect_get_param_by_name(const gs_effect_t *e, const char *n)
{
	(void)e;
	struct gs_effect_param *p = &params[nparams++ % 16];
	snprintf(p->name, 32, "%s", n);
	return p;
}
gs_technique_t *gs_effect_get_technique(const gs_effect_t *e, const char *n)
{
	(void)e;
	struct gs_effect_technique *t = &techs[ntechs++ % 4];
	snprintf(t->name, 32, "%s", n);
	return t;
}
void gs_effect_set_vec4(gs_eparam_t *p, const struct vec4 *v)
{
	printf("gfx set_vec4 %s %.9g %.9g %.9g %.9g\n", p->name, (double)v->x, (double)v->y, (double)v->z, (double)v->w);
}
void gs_effect_set_texture_srgb(gs_eparam_t *p, gs_texture_t *t)
{
	printf("gfx set_texture_srgb %s tex=%d\n", p->name, t ? t->id : -1);
}
size_t gs_technique_begin(gs_technique_t *t)
{
	printf("gfx technique_begin %s\n", t->name);
	return 1;
}
bool gs_technique_begin_pass(gs_technique_t *t, size_t pass)
{
	(void)t;
	printf("gfx begin_pass %zu\n", pass);
	return true;
}
void gs_technique_end_pass(gs_technique_t *t)
{
	(void)t;
	printf("gfx end_pass\n");
}
void gs_technique_end(gs_technique_t *t)
{
	(void)t;
	printf("gfx technique_end\n");
}
void gs_draw_sprite(gs_texture_t *tex, uint32_t flip, uint32_t w, uint32_t h)
{
	printf("gfx draw_sprite tex=%d flip=%u %ux%u\n", tex ? tex->id : -1, flip, w, h);
}
bool gs_framebuffer_srgb_enabled(void)
{
	return host_srgb_fb;
}
void gs_enable_framebuffer_srgb(bool e)
{
	host_srgb_fb = e;
	printf("gfx enable_framebuffer_srgb %d\n", e);
}
bool gs_get_linear_srgb(void)
{
	return host_linear_srgb;
}
void gs_blend_state_push(void)
{
	printf("gfx blend_push\n");
}
void gs_blend_state_pop(void)
{
	printf("gfx blend_pop\n");
}
void gs_blend_function(enum gs_blend_type s, enum gs_blend_type d)
{
	printf("gfx blend_function %d %d\n", (int)s, (int)d);
}
void obs_enter_graphics(void) {}
void obs_leave_graphics(void) {}

/* ---------------- module / misc stubs ---------------- */
bool text_lookup_getstr(lookup_t *l, const char *v, const char **out)
{
	(void)l, (void)v, (void)out;
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
uint64_t obs_get_video_frame_time(void)
{
	return host_clock_ns;
}
bool obs_get_video_info(struct obs_video_info *ovi)
{
	memset(ovi, 0, sizeof(*ovi));
	ovi->base_width = host_base_w;
	ovi->base_height = host_base_h;
	return true;
}

/* ---------------- task queues ---------------- */
struct os_task_queue {
	int n;
	os_task_t t[64];
	void *p[64];
	bool used;
};
static struct os_task_queue queues[16];
os_task_queue_t *os_task_queue_create(void)
{
	for (int i = 0; i < 16; i++)
		if (!queues[i].used) {
			memset(&queues[i], 0, sizeof(queues[i]));
			queues[i].used = true;
			return &queues[i];
		}
	return NULL;
}
static void queue_run(struct os_task_queue *q)
{
	for (int i = 0; i < q->n; i++)
		q->t[i](q->p[i]);
	q->n = 0;
}
bool os_task_queue_queue_task(os_task_queue_t *q, os_task_t t, void *p)
{
	if (!q)
		return false;
	q->t[q->n] = t;
	q->p[q->n++] = p;
	ev("task_queued");
	if (host_task_auto)
		queue_run(q);
	return true;
}
void os_task_queue_destroy(os_task_queue_t *q)
{
	if (!q)
		return;
	ev("task_queue_destroy pending=%d", q->n);
	queue_run(q);
	q->used = false;
}
void host_run_tasks(void)
{
	for (int i = 0; i < 16; i++)
		if (queues[i].used)
			queue_run(&queues[i]);
}

/* ---------------- sources ---------------- */
#define MAXSRC 256
struct obs_source {
	int serial;
	char *id, *name;
	bool is_private, dead, has_info, is_tr;
	struct obs_source_info info;
	void *data;
	obs_data_t *settings;
	int refs, show_refs, activate_refs;
	bool showing_cb, active_cb;
	int defer;
	int nact;
	int act[64];
	bool actp[64];
	signal_handler_t *sh;
	proc_handler_t *ph;
	/* transition */
	obs_source_t *tsrc[2];
	bool tactive[2], transitioning;
	uint64_t tstart, tdur;
	uint32_t tcx, tcy;
};
static struct obs_source *all[MAXSRC];
static int nall;

struct obs_weak_source {
	obs_source_t *src;
	int refs;
};
static struct obs_weak_source *weaks[MAXSRC];
static int nweak;

static struct obs_source_info reg[16];
static char *reg_id[16];
static int nreg;
int host_num_infos(void)
{
	return nreg;
}
const struct obs_source_info *host_info_at(int i)
{
	return &reg[i];
}
const struct obs_source_info *host_find_info(const char *id)
{
	for (int i = 0; i < nreg; i++)
		if (!strcmp(reg_id[i], id))
			return &reg[i];
	return NULL;
}
void obs_register_source_s(const struct obs_source_info *info, size_t size)
{
	(void)size;
	struct dstr d = {0};
	reg[nreg] = *info;
	if (info->version)
		dstr_printf(&d, "%s_v%d", info->id, (int)info->version);
	else
		dstr_printf(&d, "%s", info->id);
	reg_id[nreg] = d.array;
	nreg++;
}

const char *imagesource_label(obs_source_t *s)
{
	static char bufs[8][300];
	static int k;
	char *b = bufs[k++ & 7];
	if (!s) {
		snprintf(b, 300, "null");
		return b;
	}
	const char *id = s->has_info ? s->id : s->id;
	if (!strcmp(id, "image_source") && s->settings)
		snprintf(b, 300, "%s#%d[%s]", id, s->serial, obs_data_get_string(s->settings, "file"));
	else
		snprintf(b, 300, "%s#%d", id, s->serial);
	return b;
}
const char *host_name(obs_source_t *s)
{
	return s->name ? s->name : "";
}
const char *obs_source_get_name(const obs_source_t *s)
{
	return s->name ? s->name : "";
}
obs_source_t *host_find(const char *name)
{
	for (int i = 0; i < nall; i++)
		if (!all[i]->dead && all[i]->name && !strcmp(all[i]->name, name))
			return all[i];
	return NULL;
}
int host_count(void)
{
	return nall;
}
obs_source_t *obs_source_get_ref(obs_source_t *s)
{
	if (!s || s->dead)
		return NULL;
	s->refs++;
	return s;
}
static void src_destroy(obs_source_t *s);
void obs_source_release(obs_source_t *s)
{
	if (!s)
		return;
	if (--s->refs <= 0)
		src_destroy(s);
}
static void adjust(obs_source_t *s, int dshow, int dact);
static void set_child_active(obs_source_t *parent, obs_source_t *child, int sign)
{
	adjust(child, sign * parent->show_refs, sign * parent->activate_refs);
}
struct kids {
	obs_source_t *k[4];
	int n;
};
static void collect_cb(obs_source_t *parent, obs_source_t *child, void *p)
{
	(void)parent;
	struct kids *k = p;
	k->k[k->n++] = child;
}
static void children(obs_source_t *s, struct kids *k)
{
	k->n = 0;
	if (s->is_tr) {
		for (int i = 0; i < 2; i++)
			if (s->tsrc[i] && s->tactive[i])
				k->k[k->n++] = s->tsrc[i];
	} else if (s->info.enum_active_sources && s->data) {
		s->info.enum_active_sources(s->data, collect_cb, k);
	}
}
static void adjust(obs_source_t *s, int dshow, int dact)
{
	struct kids k;
	s->show_refs += dshow;
	s->activate_refs += dact;
	children(s, &k);
	for (int i = 0; i < k.n; i++)
		adjust(k.k[i], dshow, dact);
}
bool obs_source_add_active_child(obs_source_t *p, obs_source_t *c)
{
	if (!p || !c || p == c)
		return false;
	set_child_active(p, c, 1);
	return true;
}
void obs_source_remove_active_child(obs_source_t *p, obs_source_t *c)
{
	if (p && c)
		set_child_active(p, c, -1);
}
void host_set_refs(obs_source_t *s, int show, int act)
{
	adjust(s, show - s->show_refs, act - s->activate_refs);
}
bool obs_source_showing(const obs_source_t *s)
{
	return s->show_refs != 0;
}

static obs_source_t *src_create(const char *id, const char *name, obs_data_t *settings, bool priv)
{
	const struct obs_source_info *info = host_find_info(id);
	bool tr = !info && strstr(id, "_transition");
	if (!info && !tr) {
		fprintf(stderr, "script error: unknown source id %s\n", id);
		exit(2);
	}
	struct obs_source *s = bzalloc(sizeof(*s));
	s->serial = nall + 1;
	all[nall++] = s;
	s->id = bstrdup(id);
	s->name = name ? bstrdup(name) : NULL;
	s->is_private = priv;
	s->refs = 1;
	s->is_tr = tr;
	s->has_info = info != NULL;
	if (info)
		s->info = *info;
	s->settings = settings ? obs_data_newref(settings) : obs_data_create();
	if (!settings) {
		/* newref(NULL) semantic of the reference: a fresh empty object */
	}
	s->sh = signal_handler_create();
	s->ph = proc_handler_create();
	if (info && info->get_defaults)
		info->get_defaults(s->settings);
	ev("create %s %s", imagesource_label(s), priv ? "private" : "public");
	if (info && info->create)
		s->data = info->create(s->settings, s);
	return s;
}
obs_source_t *obs_source_create_private(const char *id, const char *name, obs_data_t *settings)
{
	return src_create(id, name, settings, true);
}
obs_source_t *host_create(const char *name, const char *id, obs_data_t *settings)
{
	return src_create(id, name, settings, false);
}
static void src_destroy(obs_source_t *s)
{
	ev("destroy %s", imagesource_label(s));
	s->dead = true;
	for (int i = 0; i < nweak; i++)
		if (weaks[i]->src == s)
			weaks[i]->src = NULL;
	if (s->data && s->info.destroy)
		s->info.destroy(s->data);
	if (s->is_tr)
		for (int i = 0; i < 2; i++) {
			obs_source_release(s->tsrc[i]);
			s->tsrc[i] = NULL;
		}
	obs_data_release(s->settings);
	signal_handler_destroy(s->sh);
	proc_handler_destroy(s->ph);
}
void *obs_obj_get_data(void *o)
{
	obs_source_t *s = o;
	return s->data;
}
obs_data_t *obs_source_get_settings(const obs_source_t *s)
{
	obs_data_addref(s->settings);
	return s->settings;
}
signal_handler_t *obs_source_get_signal_handler(const obs_source_t *s)
{
	return s->sh;
}
proc_handler_t *obs_source_get_proc_handler(const obs_source_t *s)
{
	return s->ph;
}
uint32_t obs_source_get_width(obs_source_t *s)
{
	return (s->info.get_width && s->data) ? s->info.get_width(s->data) : 0;
}
uint32_t obs_source_get_height(obs_source_t *s)
{
	return (s->info.get_height && s->data) ? s->info.get_height(s->data) : 0;
}
enum gs_color_space obs_source_get_color_space(obs_source_t *s, size_t n, const enum gs_color_space *p)
{
	(void)s, (void)n, (void)p;
	return GS_CS_SRGB;
}
void obs_source_update(obs_source_t *s, obs_data_t *settings)
{
	if (settings)
		obs_data_apply(s->settings, settings);
	if (s->info.output_flags & OBS_SOURCE_VIDEO)
		s->defer++;
	else if (s->data && s->info.update)
		s->info.update(s->data, s->settings);
}
void host_update(obs_source_t *s, obs_data_t *d)
{
	obs_source_update(s, d);
}
struct obs_weak_source *obs_source_get_weak_source(obs_source_t *s)
{
	struct obs_weak_source *w = bzalloc(sizeof(*w));
	w->src = s;
	w->refs = 1;
	weaks[nweak++] = w;
	return w;
}
obs_source_t *obs_weak_source_get_source(obs_weak_source_t *w)
{
	return w->src ? obs_source_get_ref(w->src) : NULL;
}
void obs_weak_source_release(obs_weak_source_t *w)
{
	if (w)
		w->refs--;
}

/* ---- media / signals ---- */
static void media_queue(obs_source_t *s, int a, bool p)
{
	s->act[s->nact] = a;
	s->actp[s->nact++] = p;
}
void obs_source_media_play_pause(obs_source_t *s, bool pause)
{
	if ((s->info.output_flags & OBS_SOURCE_CONTROLLABLE_MEDIA) && s->info.media_play_pause)
		media_queue(s, 0, pause);
}
void obs_source_media_restart(obs_source_t *s)
{
	if ((s->info.output_flags & OBS_SOURCE_CONTROLLABLE_MEDIA) && s->info.media_restart)
		media_queue(s, 1, 0);
}
void obs_source_media_stop(obs_source_t *s)
{
	if ((s->info.output_flags & OBS_SOURCE_CONTROLLABLE_MEDIA) && s->info.media_stop)
		media_queue(s, 2, 0);
}
void obs_source_media_next(obs_source_t *s)
{
	if ((s->info.output_flags & OBS_SOURCE_CONTROLLABLE_MEDIA) && s->info.media_next)
		media_queue(s, 3, 0);
}
void obs_source_media_previous(obs_source_t *s)
{
	if ((s->info.output_flags & OBS_SOURCE_CONTROLLABLE_MEDIA) && s->info.media_previous)
		media_queue(s, 4, 0);
}
void obs_source_media_started(obs_source_t *s)
{
	if (s->info.output_flags & OBS_SOURCE_CONTROLLABLE_MEDIA)
		ev("signal media_started %s", imagesource_label(s));
}
void obs_source_media_ended(obs_source_t *s)
{
	if (s->info.output_flags & OBS_SOURCE_CONTROLLABLE_MEDIA)
		ev("signal media_ended %s", imagesource_label(s));
}
void host_media(obs_source_t *s, const char *w)
{
	if (!strcmp(w, "play"))
		obs_source_media_play_pause(s, false);
	else if (!strcmp(w, "pause"))
		obs_source_media_play_pause(s, true);
	else if (!strcmp(w, "restart"))
		obs_source_media_restart(s);
	else if (!strcmp(w, "stop"))
		obs_source_media_stop(s);
	else if (!strcmp(w, "next"))
		obs_source_media_next(s);
	else if (!strcmp(w, "previous"))
		obs_source_media_previous(s);
}
static void process_media(obs_source_t *s)
{
	while (s->nact) {
		int a = s->act[0];
		bool p = s->actp[0];
		memmove(s->act, s->act + 1, sizeof(int) * (size_t)(--s->nact));
		memmove(s->actp, s->actp + 1, sizeof(bool) * (size_t)s->nact);
		switch (a) {
		case 0:
			s->info.media_play_pause(s->data, p);
			ev("signal %s %s", p ? "media_pause" : "media_play", imagesource_label(s));
			break;
		case 1:
			s->info.media_restart(s->data);
			ev("signal media_restart %s", imagesource_label(s));
			break;
		case 2:
			s->info.media_stop(s->data);
			ev("signal media_stopped %s", imagesource_label(s));
			break;
		case 3:
			s->info.media_next(s->data);
			ev("signal media_next %s", imagesource_label(s));
			break;
		case 4:
			s->info.media_previous(s->data);
			ev("signal media_previous %s", imagesource_label(s));
			break;
		}
	}
}
void host_media_state(obs_source_t *s)
{
	if (s->info.media_get_state)
		printf("media_state %d\n", (int)s->info.media_get_state(s->data));
	else
		printf("media_state none\n");
}

/* ---- tick ---- */
static void tick_one(obs_source_t *s, float seconds)
{
	if (s->is_tr || s->dead)
		return;
	if ((s->info.output_flags & OBS_SOURCE_CONTROLLABLE_MEDIA))
		process_media(s);
	if (s->defer > 0) {
		int c = s->defer;
		if (s->data && s->info.update)
			s->info.update(s->data, s->settings);
		if (s->defer == c)
			s->defer = 0;
	}
	bool ns = s->show_refs != 0;
	if (ns != s->showing_cb) {
		if (s->data && (ns ? s->info.show : s->info.hide))
			(ns ? s->info.show : s->info.hide)(s->data);
		ev("%s %s", ns ? "show" : "hide", imagesource_label(s));
		s->showing_cb = ns;
	}
	bool na = s->activate_refs != 0;
	if (na != s->active_cb) {
		if (s->data && (na ? s->info.activate : s->info.deactivate))
			(na ? s->info.activate : s->info.deactivate)(s->data);
		ev("%s %s", na ? "activate" : "deactivate", imagesource_label(s));
		s->active_cb = na;
	}
	if (s->data && s->info.video_tick)
		s->info.video_tick(s->data, seconds);
}
void host_tick_all(float seconds)
{
	host_clock_ns += (uint64_t)llround((double)seconds * 1e9);
	int n = nall;
	for (int i = 0; i < n; i++)
		tick_one(all[i], seconds);
}

/* ---- hotkeys ---- */
struct hk {
	obs_source_t *s;
	obs_hotkey_id id;
	char name[48];
	obs_hotkey_func f;
	void *d;
};
static struct hk hks[32];
static int nhk;
obs_hotkey_id obs_hotkey_register_source(obs_source_t *s, const char *name, const char *desc, obs_hotkey_func f,
					 void *d)
{
	if (!s || s->is_private)
		return OBS_INVALID_HOTKEY_ID;
	struct hk *h = &hks[nhk++];
	h->s = s;
	h->id = (obs_hotkey_id)nhk;
	snprintf(h->name, 48, "%s", name);
	h->f = f;
	h->d = d;
	ev("hotkey_register %s id=%zu name=%s desc=%s", imagesource_label(s), (size_t)h->id, name, desc);
	return h->id;
}
void host_hotkey(obs_source_t *s, const char *name, bool pressed)
{
	for (int i = 0; i < nhk; i++)
		if (hks[i].s == s && !strcmp(hks[i].name, name))
			hks[i].f(hks[i].d, hks[i].id, NULL, pressed);
}

/* ---- transitions ---- */
#define TR(t) (t)
static float tr_time(obs_source_t *t)
{
	uint64_t ts = host_clock_ns;
	if (ts <= t->tstart)
		return 0.0f;
	uint64_t end = t->tdur;
	ts -= t->tstart;
	if (ts >= end || end == 0)
		return 1.0f;
	return (float)((long double)ts / (long double)end);
}
float obs_transition_get_time(obs_source_t *t)
{
	return tr_time(t);
}
static void tr_set(obs_source_t *t, obs_source_t *src)
{
	src = obs_source_get_ref(src);
	obs_source_t *s[2] = {t->tsrc[0], t->tsrc[1]};
	bool a[2] = {t->tactive[0], t->tactive[1]};
	t->tsrc[0] = src;
	t->tsrc[1] = NULL;
	t->tactive[0] = true;
	t->tactive[1] = false;
	t->transitioning = false;
	for (int i = 0; i < 2; i++) {
		if (s[i] && a[i])
			obs_source_remove_active_child(t, s[i]);
		obs_source_release(s[i]);
	}
	if (src)
		obs_source_add_active_child(t, src);
}
void obs_transition_set(obs_source_t *t, obs_source_t *src)
{
	ev("tr_set %s %s", imagesource_label(t), imagesource_label(src));
	tr_set(t, src);
}
bool obs_transition_start(obs_source_t *t, enum obs_transition_mode mode, uint32_t ms, obs_source_t *dest)
{
	if (t->transitioning)
		tr_set(t, t->tsrc[1]);
	bool same_src = dest == t->tsrc[0], same_dest = dest == t->tsrc[1];
	bool active = t->transitioning;
	if (same_src && !active) {
		ev("tr_start %s mode=%d ms=%u dest=%s -> ignored", imagesource_label(t), (int)mode, ms,
		   imagesource_label(dest));
		return false;
	}
	ev("tr_start %s mode=%d ms=%u dest=%s -> started", imagesource_label(t), (int)mode, ms, imagesource_label(dest));
	t->tstart = host_clock_ns;
	t->tdur = (uint64_t)ms * 1000000ULL;
	if (dest != t->tsrc[1]) {
		obs_source_t *nc = obs_source_get_ref(dest);
		obs_source_t *old = t->tsrc[1];
		if (!t->tactive[1] && nc) {
			obs_source_add_active_child(t, nc);
			t->tactive[1] = true;
			t->transitioning = true;
		}
		t->tsrc[1] = nc;
		obs_source_release(old);
	}
	if (dest == NULL && same_dest && !same_src)
		t->transitioning = true;
	return true;
}
void obs_transition_set_size(obs_source_t *t, uint32_t cx, uint32_t cy)
{
	t->tcx = cx;
	t->tcy = cy;
	ev("tr_size %s %ux%u", imagesource_label(t), cx, cy);
}
void obs_transition_set_alignment(obs_source_t *t, uint32_t a)
{
	ev("tr_align %s %u", imagesource_label(t), a);
}
void obs_transition_set_scale_type(obs_source_t *t, enum obs_transition_scale_type s)
{
	ev("tr_scale %s %d", imagesource_label(t), (int)s);
}
obs_source_t *obs_transition_get_active_source(obs_source_t *t)
{
	return obs_source_get_ref(t->transitioning ? t->tsrc[1] : t->tsrc[0]);
}
void obs_source_video_render(obs_source_t *s)
{
	if (s->is_tr) {
		float t = tr_time(s);
		if (t >= 1.0f && s->transitioning) {
			obs_source_t *old = s->tsrc[0];
			if (old && s->tactive[0])
				obs_source_remove_active_child(s, old);
			obs_source_release(old);
			s->tactive[0] = true;
			s->tactive[1] = false;
			s->tsrc[0] = s->tsrc[1];
			s->tsrc[1] = NULL;
			s->transitioning = false;
			ev("tr_stop %s", imagesource_label(s));
		}
		ev("render %s t=%.9g transitioning=%d", imagesource_label(s), (double)t, s->transitioning);
		obs_source_t *a = s->tsrc[0], *b = s->transitioning ? s->tsrc[1] : NULL;
		if (a)
			obs_source_video_render(a);
		if (b)
			obs_source_video_render(b);
	} else {
		ev("render %s", imagesource_label(s));
		if (s->data && s->info.video_render)
			s->info.video_render(s->data, &fx_default);
	}
}
void host_render(obs_source_t *s)
{
	obs_source_video_render(s);
}
void host_tr_print(obs_source_t *s)
{
	(void)s;
}
size_t obs_source_audio_pending_dummy;

/* accessors for the runner */
const struct obs_source_info *host_info_of(obs_source_t *s)
{
	return &s->info;
}
signal_handler_t *host_sh(obs_source_t *s)
{
	return s->sh;
}
proc_handler_t *host_ph(obs_source_t *s)
{
	return s->ph;
}
void *host_data(obs_source_t *s)
{
	return s->data;
}

/* audio path of the slideshows: not exercised (documented as out of scope in S-030) */
bool obs_source_audio_pending(const obs_source_t *s)
{
	(void)s;
	return true;
}
uint64_t obs_source_get_audio_timestamp(const obs_source_t *s)
{
	(void)s;
	return 0;
}
void obs_source_get_audio_mix(const obs_source_t *s, struct obs_source_audio_mix *m)
{
	(void)s, (void)m;
}
/* the host forwards a missing-file replacement to the callback registered by the source */
void obs_source_replace_missing_file(obs_missing_file_cb cb, obs_source_t *source, const char *new_path, void *data)
{
	printf("ev missing_file_replace %s new=%s\n", imagesource_label(source), new_path ? new_path : "@null");
	cb(source->data, new_path, data);
}
const char *host_reg_id(int i)
{
	return reg_id[i];
}
