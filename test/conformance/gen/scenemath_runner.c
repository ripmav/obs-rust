/*
 * Conformance runner for spec S-033 (scene-item transform / geometry / order / group logic of obs-scene.c).
 *
 * obs-scene.c is #included (not linked) so that the script can reach its static helpers. Everything outside the
 * scene code is replaced by a small fake "libobs": a fake core with a main canvas (+ one alternative canvas),
 * fake sources whose size is set from the script, no-op graphics, a fake obs_data, a signal log instead of real
 * signal handlers, and a recording fake transition. Test tooling, not part of libobs.
 *
 * The script language is documented in test/conformance/README.md (section "scenemath runner").
 */
#include "obs-scene.c"

#include <math.h>
#include "harness.h"

struct obs_core *obs;

/* All output goes through here so that the sign of a NaN (platform dependent) never reaches a vector. */
static int sm_printf(const char *fmt, ...)
{
	char buf[8192];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	for (char *p = strstr(buf, "-nan"); p; p = strstr(p, "-nan"))
		memmove(p, p + 1, strlen(p));
	fputs(buf, stdout);
	return n;
}
#define printf sm_printf

/* ================================================================ fake environment */

static struct obs_core sm_core;
static struct obs_canvas sm_main_canvas, sm_alt_canvas;
static struct obs_core_video_mix sm_main_mix, sm_alt_mix;
static bool sm_abs, sm_fail_child;
static uint64_t sm_clock;

uint64_t os_gettime_ns(void)
{
	return sm_clock += 1000;
}

struct sm_dim {
	uint32_t w, h;
};

struct sm_src {
	char name[48];
	obs_source_t *src;
	struct sm_dim *dim; /* NULL for scenes */
};
struct sm_item {
	char name[48];
	obs_sceneitem_t *item;
};
static struct sm_src sm_srcs[96];
static int sm_nsrc;
static struct sm_item sm_items[160];
static int sm_nitem;

/* log of signals / transition calls, flushed by the "log" command */
static char sm_log[16384];
static size_t sm_loglen;

static void sm_logf(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (sm_loglen < sizeof(sm_log) - 256)
		sm_loglen += (size_t)vsnprintf(sm_log + sm_loglen, sizeof(sm_log) - sm_loglen, fmt, ap);
	va_end(ap);
}

static const char *sm_item_name(const obs_sceneitem_t *item)
{
	for (int i = 0; i < sm_nitem; i++)
		if (sm_items[i].item == item)
			return sm_items[i].name;
	return "?";
}

/* ---- signals: handler == the owning source pointer */

signal_handler_t *obs_source_get_signal_handler(const obs_source_t *source)
{
	return (signal_handler_t *)source;
}
bool signal_handler_add(signal_handler_t *h, const char *d)
{
	(void)h, (void)d;
	return true;
}
void signal_handler_connect(signal_handler_t *h, const char *s, signal_callback_t cb, void *p)
{
	(void)h, (void)s, (void)cb, (void)p;
}
void signal_handler_disconnect(signal_handler_t *h, const char *s, signal_callback_t cb, void *p)
{
	(void)h, (void)s, (void)cb, (void)p;
}
void signal_handler_signal(signal_handler_t *h, const char *signal, calldata_t *params)
{
	const obs_source_t *src = (const obs_source_t *)h;
	void *item = NULL;
	bool b;
	calldata_get_ptr(params, "item", &item);
	sm_logf("signal %s scene=%s", signal, src->context.name);
	if (item)
		sm_logf(" item#%lld", (long long)((obs_sceneitem_t *)item)->id);
	if (calldata_get_bool(params, "visible", &b))
		sm_logf(" visible=%d", b);
	if (calldata_get_bool(params, "locked", &b))
		sm_logf(" locked=%d", b);
	sm_logf("\n");
}

/* ---- obs_data stand-in: only what the transform code touches */

obs_data_t *obs_data_create(void)
{
	return bzalloc(8);
}
void obs_data_release(obs_data_t *d)
{
	bfree(d);
}
void obs_data_apply(obs_data_t *d, obs_data_t *s)
{
	(void)d, (void)s;
}
bool obs_data_get_bool(obs_data_t *d, const char *name)
{
	(void)d;
	return !strcmp(name, "AbsoluteCoordinates") && sm_abs;
}

/* ---- hotkeys: scenes are created private, so adding items never registers hotkeys, but moving items between a
 * group and its parent (reorder_items2) always re-registers them; the stand-ins just accept everything. */

obs_hotkey_pair_id obs_hotkey_pair_register_source(obs_source_t *s, const char *a, const char *b, const char *c,
						   const char *d, obs_hotkey_active_func f1, obs_hotkey_active_func f2,
						   void *d1, void *d2)
{
	(void)s, (void)a, (void)b, (void)c, (void)d, (void)f1, (void)f2, (void)d1, (void)d2;
	return 1;
}
void obs_hotkey_pair_save(obs_hotkey_pair_id id, obs_data_array_t **a, obs_data_array_t **b)
{
	(void)id;
	*a = *b = NULL;
}
void obs_hotkey_pair_load(obs_hotkey_pair_id id, obs_data_array_t *a, obs_data_array_t *b)
{
	(void)id, (void)a, (void)b;
}
obs_data_array_t *obs_data_get_array(obs_data_t *d, const char *n)
{
	(void)d, (void)n;
	return NULL;
}
void obs_data_array_release(obs_data_array_t *a)
{
	(void)a;
}

void obs_hotkey_pair_unregister(obs_hotkey_pair_id id)
{
	(void)id;
}

/* ---- canvases */

obs_canvas_t *obs_weak_canvas_get_canvas(obs_weak_canvas_t *weak)
{
	return (obs_canvas_t *)weak;
}
void obs_canvas_release(obs_canvas_t *c)
{
	(void)c;
}

/* ---- sources */

static obs_source_t *sm_new_source(const struct obs_source_info *info, const char *name, void *data,
				   obs_canvas_t *canvas)
{
	struct obs_source *s = bzalloc(sizeof(*s));
	s->info = *info;
	s->context.name = bstrdup(name);
	s->context.private = true;
	s->context.signals = (signal_handler_t *)s;
	s->canvas = (obs_weak_canvas_t *)canvas;
	s->context.data = data;
	return s;
}

static obs_source_t *sm_make_scene_source(const char *id, const char *name, obs_canvas_t *canvas)
{
	const struct obs_source_info *info = !strcmp(id, group_info.id) ? &group_info : &scene_info;
	struct obs_source *s = sm_new_source(info, name, NULL, canvas);
	s->context.data = scene_create(NULL, s);
	return s;
}

obs_source_t *obs_source_create_canvas(obs_canvas_t *canvas, const char *id, const char *name, obs_data_t *settings,
				       obs_data_t *hotkey)
{
	(void)settings, (void)hotkey;
	obs_source_t *s = sm_make_scene_source(id, name, canvas);
	/* groups made by obs_scene_insert_group must be addressable from the script */
	if (sm_nsrc < 96) {
		snprintf(sm_srcs[sm_nsrc].name, sizeof(sm_srcs[0].name), "%s", name);
		sm_srcs[sm_nsrc].src = s;
		sm_srcs[sm_nsrc].dim = NULL;
		sm_nsrc++;
	}
	return s;
}

static uint32_t sm_dim_w(void *d)
{
	return ((struct sm_dim *)d)->w;
}
static uint32_t sm_dim_h(void *d)
{
	return ((struct sm_dim *)d)->h;
}

uint32_t obs_source_get_width(obs_source_t *s)
{
	return (s && s->context.data && s->info.get_width) ? s->info.get_width(s->context.data) : 0;
}
uint32_t obs_source_get_height(obs_source_t *s)
{
	return (s && s->context.data && s->info.get_height) ? s->info.get_height(s->context.data) : 0;
}
bool obs_source_removed(const obs_source_t *s)
{
	return s->removed;
}
obs_source_t *obs_source_get_ref(obs_source_t *s)
{
	return s;
}
void obs_source_addref(obs_source_t *s)
{
	(void)s;
}
void obs_source_release(obs_source_t *s)
{
	(void)s;
}
const char *obs_source_get_name(const obs_source_t *s)
{
	return s->context.name;
}
bool obs_source_add_active_child(obs_source_t *parent, obs_source_t *child)
{
	/* the real call fails on infinite recursion; scripts can switch the failure on */
	(void)parent, (void)child;
	return !sm_fail_child;
}
void obs_source_remove_active_child(obs_source_t *parent, obs_source_t *child)
{
	(void)parent, (void)child;
}

/* ---- fake transitions: record what obs_sceneitem_do_transition asks for */

static bool sm_trans_busy;
void obs_transition_force_stop(obs_source_t *t)
{
	sm_logf("transition %s force_stop\n", t->context.name);
	t->transitioning_video = false;
	t->transitioning_audio = false;
}
void obs_transition_set_size(obs_source_t *t, uint32_t cx, uint32_t cy)
{
	sm_logf("transition %s size %u %u\n", t->context.name, cx, cy);
}
void obs_transition_set_alignment(obs_source_t *t, uint32_t a)
{
	sm_logf("transition %s align %u\n", t->context.name, a);
}
void obs_transition_set_scale_type(obs_source_t *t, enum obs_transition_scale_type s)
{
	sm_logf("transition %s scale_type %d\n", t->context.name, (int)s);
}
void obs_transition_set(obs_source_t *t, obs_source_t *s)
{
	sm_logf("transition %s set %s\n", t->context.name, s ? s->context.name : "-");
}
bool obs_transition_start(obs_source_t *t, enum obs_transition_mode m, uint32_t ms, obs_source_t *dest)
{
	sm_logf("transition %s start mode=%d ms=%u dest=%s\n", t->context.name, (int)m, ms,
		dest ? dest->context.name : "-");
	t->transitioning_video = sm_trans_busy;
	return true;
}

/* ================================================================ script helpers */

static struct sm_src *sm_find_src(const char *name)
{
	for (int i = 0; i < sm_nsrc; i++)
		if (!strcmp(sm_srcs[i].name, name))
			return &sm_srcs[i];
	fprintf(stderr, "script: unknown source %s\n", name);
	exit(2);
}
static obs_sceneitem_t *IT(const char *name)
{
	for (int i = 0; i < sm_nitem; i++)
		if (!strcmp(sm_items[i].name, name))
			return sm_items[i].item;
	fprintf(stderr, "script: unknown item %s\n", name);
	exit(2);
}
static obs_scene_t *SC(const char *name)
{
	struct sm_src *s = sm_find_src(name);
	return s->src->context.data;
}
static void sm_reg_item(const char *name, obs_sceneitem_t *item)
{
	for (int i = 0; i < sm_nitem; i++)
		if (!strcmp(sm_items[i].name, name)) {
			sm_items[i].item = item;
			if (item)
				obs_sceneitem_addref(item);
			return;
		}
	snprintf(sm_items[sm_nitem].name, sizeof(sm_items[0].name), "%s", name);
	sm_items[sm_nitem].item = item;
	if (item)
		obs_sceneitem_addref(item);
	sm_nitem++;
}

static void sm_reset(void)
{
	memset(&sm_core, 0, sizeof(sm_core));
	memset(&sm_main_canvas, 0, sizeof(sm_main_canvas));
	memset(&sm_alt_canvas, 0, sizeof(sm_alt_canvas));
	sm_main_canvas.mix = &sm_main_mix;
	sm_alt_canvas.mix = &sm_alt_mix;
	sm_main_mix.ovi.base_width = sm_main_canvas.ovi.base_width = 1920;
	sm_main_mix.ovi.base_height = sm_main_canvas.ovi.base_height = 1080;
	sm_alt_mix.ovi.base_width = sm_alt_canvas.ovi.base_width = 1280;
	sm_alt_mix.ovi.base_height = sm_alt_canvas.ovi.base_height = 720;
	sm_core.data.main_canvas = &sm_main_canvas;
	sm_core.hotkeys.sceneitem_show = bstrdup("Show '%1'");
	sm_core.hotkeys.sceneitem_hide = bstrdup("Hide '%1'");
	obs = &sm_core;
	sm_abs = false;
	sm_fail_child = false;
	sm_nsrc = sm_nitem = 0;
	sm_loglen = 0;
	sm_log[0] = 0;
	sm_clock = 0;
	sm_trans_busy = false;
}

#define F "%.9g"
static void pv2(const char *label, float x, float y)
{
	printf(" %s=" F "," F, label, x, y);
}
static void print_matrix(const char *label, const struct matrix4 *m)
{
	printf("%s", label);
	const struct vec4 *r[4] = {&m->x, &m->y, &m->z, &m->t};
	for (int i = 0; i < 4; i++)
		printf(" [" F "," F "," F "," F "]", r[i]->x, r[i]->y, r[i]->z, r[i]->w);
	printf("\n");
}

static void print_item(const char *name)
{
	obs_sceneitem_t *it = IT(name);
	printf("item %s id=%lld parent=%s group=%d scene=%d", name, (long long)it->id,
	       it->parent ? it->parent->source->context.name : "-", it->is_group, it->is_scene);
	printf(" abs=%d vis=%d user_vis=%d sel=%d lock=%d need_update=%d\n", it->absolute_coordinates, it->visible,
	       it->user_visible, it->selected, it->locked, it->update_transform);
	printf("  stored");
	pv2("pos", it->pos.x, it->pos.y);
	printf(" rot=" F, it->rot);
	pv2("scale", it->scale.x, it->scale.y);
	pv2("scale_ref", it->scale_ref.x, it->scale_ref.y);
	printf(" align=%u\n", it->align);
	printf("  bounds type=%d align=%u", (int)it->bounds_type, it->bounds_align);
	pv2("size", it->bounds.x, it->bounds.y);
	printf(" crop_to_bounds=%d\n", it->crop_to_bounds);
	printf("  crop l=%d r=%d t=%d b=%d bounds_crop l=%d r=%d t=%d b=%d last=%u,%u\n", it->crop.left, it->crop.right,
	       it->crop.top, it->crop.bottom, it->bounds_crop.left, it->bounds_crop.right, it->bounds_crop.top,
	       it->bounds_crop.bottom, it->last_width, it->last_height);
	printf(" ");
	pv2("output_scale", it->output_scale.x, it->output_scale.y);
	pv2("box_scale", it->box_scale.x, it->box_scale.y);
	printf("\n  ");
	print_matrix("draw", &it->draw_transform);
	printf("  ");
	print_matrix("box", &it->box_transform);
}

static void print_order(obs_scene_t *s)
{
	bool first = true;
	for (obs_sceneitem_t *i = s->first_item; i; i = i->next) {
		printf("%s%s", first ? "" : " ", sm_item_name(i));
		first = false;
		if (i->is_group) {
			obs_scene_t *g = i->source->context.data;
			printf("{");
			bool f2 = true;
			for (obs_sceneitem_t *j = g->first_item; j; j = j->next) {
				printf("%s%s", f2 ? "" : " ", sm_item_name(j));
				f2 = false;
			}
			printf("}");
		}
	}
	if (first)
		printf("-");
	printf("\n");
}

static float NUM(struct script_line *sl, int i)
{
	if (i >= sl->argc) {
		fprintf(stderr, "script: line %d: missing argument %d\n", sl->lineno, i);
		exit(2);
	}
	return (float)strtod(sl->argv[i], NULL);
}
static long INT(struct script_line *sl, int i)
{
	if (i >= sl->argc) {
		fprintf(stderr, "script: line %d: missing argument %d\n", sl->lineno, i);
		exit(2);
	}
	return strtol(sl->argv[i], NULL, 0);
}

/* The sequence scene_video_render() performs before drawing (non-group scenes only). */
static void sm_update_scene(obs_scene_t *scene)
{
	obs_scene_item_ptr_array_t remove_items;
	da_init(remove_items);
	video_lock(scene);
	if (!scene->is_group) {
		bool size_changed = scene_size_changed(scene);
		update_transforms_and_prune_sources(scene, &remove_items, NULL, size_changed);
	}
	video_unlock(scene);
	for (size_t i = 0; i < remove_items.num; i++)
		obs_sceneitem_release(remove_items.array[i]);
	da_free(remove_items);
}

/* resolve "name" or "group/name" order tokens for reorder2 */
static void sm_order_info(const char *tok, struct obs_sceneitem_order_info *info)
{
	const char *slash = strchr(tok, '/');
	memset(info, 0, sizeof(*info));
	if (slash) {
		char g[48];
		snprintf(g, sizeof(g), "%.*s", (int)(slash - tok), tok);
		info->group = IT(g);
		info->item = IT(slash + 1);
	} else {
		info->item = IT(tok);
	}
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	sm_reset();

	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
#define IS(x) (!strcmp(c, x))
		if (IS("reset")) {
			sm_reset();
		} else if (IS("fail_child")) { /* fail_child on|off : obs_source_add_active_child fails (recursion) */
			sm_fail_child = !strcmp(sl.argv[1], "on");
		} else if (IS("canvas")) { /* canvas W H : main canvas size */
			sm_main_mix.ovi.base_width = sm_main_canvas.ovi.base_width = (uint32_t)INT(&sl, 1);
			sm_main_mix.ovi.base_height = sm_main_canvas.ovi.base_height = (uint32_t)INT(&sl, 2);
		} else if (IS("canvas_mix_only")) { /* canvas_mix_only W H : only the video mix size (canvas object keeps its own) */
			sm_main_mix.ovi.base_width = (uint32_t)INT(&sl, 1);
			sm_main_mix.ovi.base_height = (uint32_t)INT(&sl, 2);
		} else if (IS("canvas_alt")) { /* canvas_alt W H : size of the alternative canvas */
			sm_alt_mix.ovi.base_width = sm_alt_canvas.ovi.base_width = (uint32_t)INT(&sl, 1);
			sm_alt_mix.ovi.base_height = sm_alt_canvas.ovi.base_height = (uint32_t)INT(&sl, 2);
		} else if (IS("absolute")) { /* absolute on|off : affects scenes created afterwards */
			sm_abs = !strcmp(sl.argv[1], "on");
		} else if (IS("source")) { /* source NAME W H [audio] */
			struct obs_source_info info = {.id = "fake_input", .type = OBS_SOURCE_TYPE_INPUT};
			info.output_flags = OBS_SOURCE_VIDEO;
			if (sl.argc > 4 && !strcmp(sl.argv[4], "audio"))
				info.output_flags |= OBS_SOURCE_AUDIO;
			info.get_width = sm_dim_w;
			info.get_height = sm_dim_h;
			struct sm_dim *d = bzalloc(sizeof(*d));
			d->w = (uint32_t)INT(&sl, 2);
			d->h = (uint32_t)INT(&sl, 3);
			snprintf(sm_srcs[sm_nsrc].name, sizeof(sm_srcs[0].name), "%s", sl.argv[1]);
			sm_srcs[sm_nsrc].src = sm_new_source(&info, sl.argv[1], d, &sm_main_canvas);
			sm_srcs[sm_nsrc].dim = d;
			sm_nsrc++;
		} else if (IS("resize_source")) { /* resize_source NAME W H */
			struct sm_src *s = sm_find_src(sl.argv[1]);
			s->dim->w = (uint32_t)INT(&sl, 2);
			s->dim->h = (uint32_t)INT(&sl, 3);
		} else if (IS("scene")) { /* scene NAME [main|alt|none|custom W H] */
			obs_canvas_t *cv = &sm_main_canvas;
			bool custom = false;
			if (sl.argc > 2 && !strcmp(sl.argv[2], "alt"))
				cv = &sm_alt_canvas;
			if (sl.argc > 2 && !strcmp(sl.argv[2], "none"))
				cv = NULL;
			if (sl.argc > 2 && !strcmp(sl.argv[2], "custom"))
				custom = true;
			obs_source_t *s = sm_make_scene_source("scene", sl.argv[1], cv);
			snprintf(sm_srcs[sm_nsrc].name, sizeof(sm_srcs[0].name), "%s", sl.argv[1]);
			sm_srcs[sm_nsrc].src = s;
			sm_srcs[sm_nsrc].dim = NULL;
			sm_nsrc++;
			if (custom) {
				obs_scene_t *sc = s->context.data;
				sc->custom_size = true;
				sc->cx = (uint32_t)INT(&sl, 3);
				sc->cy = (uint32_t)INT(&sl, 4);
			}
		} else if (IS("source_removed")) { /* source_removed NAME : mark the source as removed */
			sm_find_src(sl.argv[1])->src->removed = true;
		} else if (IS("scene_resize")) { /* scene_resize SCENE W H : change a custom-size scene */
			obs_scene_t *sc = SC(sl.argv[1]);
			sc->custom_size = true;
			sc->cx = (uint32_t)INT(&sl, 2);
			sc->cy = (uint32_t)INT(&sl, 3);
		} else if (IS("add")) { /* add SCENE SOURCE ITEM */
			obs_sceneitem_t *it = obs_scene_add(SC(sl.argv[1]), sm_find_src(sl.argv[2])->src);
			printf("add %s %s\n", sl.argv[3], it ? "ok" : "fail");
			sm_reg_item(sl.argv[3], it);
		} else if (IS("group")) { /* group SCENE GROUP [ITEM...] */
			obs_sceneitem_t *items[32];
			size_t n = 0;
			for (int i = 3; i < sl.argc; i++)
				items[n++] = IT(sl.argv[i]);
			obs_sceneitem_t *g = obs_scene_insert_group(SC(sl.argv[1]), sl.argv[2], n ? items : NULL, n);
			printf("group %s %s\n", sl.argv[2], g ? "ok" : "fail");
			sm_reg_item(sl.argv[2], g);
		} else if (IS("ungroup")) { /* ungroup GROUP : new items take the sub-items' script names */
			obs_sceneitem_t *g = IT(sl.argv[1]);
			obs_scene_t *sub = g->source->context.data;
			obs_scene_t *parent = g->parent;
			char names[32][48];
			int n = 0;
			for (obs_sceneitem_t *i = sub->first_item; i && n < 32; i = i->next)
				snprintf(names[n++], 48, "%s", sm_item_name(i));
			obs_sceneitem_group_ungroup(g);
			/* the new items sit in the parent in sub-item order, right where the group was */
			int k = 0;
			for (obs_sceneitem_t *i = parent->first_item; i && k < n; i = i->next) {
				bool known = false;
				for (int j = 0; j < sm_nitem; j++)
					if (sm_items[j].item == i)
						known = true;
				if (!known)
					sm_reg_item(names[k++], i);
			}
			sm_reg_item(sl.argv[1], NULL);
			printf("ungroup %s\n", sl.argv[1]);
		} else if (IS("group_add")) {
			obs_sceneitem_group_add_item(IT(sl.argv[1]), IT(sl.argv[2]));
		} else if (IS("group_remove")) {
			obs_sceneitem_group_remove_item(IT(sl.argv[1]), IT(sl.argv[2]));
		} else if (IS("remove")) {
			obs_sceneitem_remove(IT(sl.argv[1]));
		} else if (IS("update")) { /* update SCENE : what the render thread does before drawing */
			sm_update_scene(SC(sl.argv[1]));
		} else if (IS("pos")) {
			struct vec2 v = {NUM(&sl, 2), NUM(&sl, 3)};
			obs_sceneitem_set_pos(IT(sl.argv[1]), &v);
		} else if (IS("rot")) {
			obs_sceneitem_set_rot(IT(sl.argv[1]), NUM(&sl, 2));
		} else if (IS("scale")) {
			struct vec2 v = {NUM(&sl, 2), NUM(&sl, 3)};
			obs_sceneitem_set_scale(IT(sl.argv[1]), &v);
		} else if (IS("align")) {
			obs_sceneitem_set_alignment(IT(sl.argv[1]), (uint32_t)INT(&sl, 2));
		} else if (IS("bounds_type")) {
			obs_sceneitem_set_bounds_type(IT(sl.argv[1]), (enum obs_bounds_type)INT(&sl, 2));
		} else if (IS("bounds_align")) {
			obs_sceneitem_set_bounds_alignment(IT(sl.argv[1]), (uint32_t)INT(&sl, 2));
		} else if (IS("bounds")) {
			struct vec2 v = {NUM(&sl, 2), NUM(&sl, 3)};
			obs_sceneitem_set_bounds(IT(sl.argv[1]), &v);
		} else if (IS("bounds_crop")) {
			obs_sceneitem_set_bounds_crop(IT(sl.argv[1]), INT(&sl, 2) != 0);
		} else if (IS("crop")) { /* crop ITEM L R T B (ints, may be negative) */
			struct obs_sceneitem_crop cr = {.left = (int)INT(&sl, 2),
							.right = (int)INT(&sl, 3),
							.top = (int)INT(&sl, 4),
							.bottom = (int)INT(&sl, 5)};
			obs_sceneitem_set_crop(IT(sl.argv[1]), &cr);
		} else if (IS("set_info")) { /* set_info ITEM px py rot sx sy align btype balign bx by crop */
			struct obs_transform_info ti;
			ti.pos.x = NUM(&sl, 2), ti.pos.y = NUM(&sl, 3);
			ti.rot = NUM(&sl, 4);
			ti.scale.x = NUM(&sl, 5), ti.scale.y = NUM(&sl, 6);
			ti.alignment = (uint32_t)INT(&sl, 7);
			ti.bounds_type = (enum obs_bounds_type)INT(&sl, 8);
			ti.bounds_alignment = (uint32_t)INT(&sl, 9);
			ti.bounds.x = NUM(&sl, 10), ti.bounds.y = NUM(&sl, 11);
			ti.crop_to_bounds = INT(&sl, 12) != 0;
			obs_sceneitem_set_info2(IT(sl.argv[1]), &ti);
		} else if (IS("set_scale_nan")) { /* set_info with a non-finite scale; other fields from the item */
			obs_sceneitem_t *it = IT(sl.argv[1]);
			struct obs_transform_info ti;
			obs_sceneitem_get_info2(it, &ti);
			ti.scale.x = NAN;
			obs_sceneitem_set_info2(it, &ti);
		} else if (IS("get")) { /* get ITEM : through the public getters */
			obs_sceneitem_t *it = IT(sl.argv[1]);
			struct obs_transform_info ti;
			struct vec2 p, s, b;
			struct obs_sceneitem_crop cr;
			obs_sceneitem_get_pos(it, &p);
			obs_sceneitem_get_scale(it, &s);
			obs_sceneitem_get_bounds(it, &b);
			obs_sceneitem_get_info2(it, &ti);
			obs_sceneitem_get_crop(it, &cr);
			printf("get %s", sl.argv[1]);
			pv2("pos", p.x, p.y);
			printf(" rot=" F, obs_sceneitem_get_rot(it));
			pv2("scale", s.x, s.y);
			printf(" align=%u btype=%d balign=%u", obs_sceneitem_get_alignment(it),
			       (int)obs_sceneitem_get_bounds_type(it), obs_sceneitem_get_bounds_alignment(it));
			pv2("bounds", b.x, b.y);
			printf(" bcrop=%d crop=%d,%d,%d,%d\n", obs_sceneitem_get_bounds_crop(it), cr.left, cr.right, cr.top,
			       cr.bottom);
			printf("info %s", sl.argv[1]);
			pv2("pos", ti.pos.x, ti.pos.y);
			printf(" rot=" F, ti.rot);
			pv2("scale", ti.scale.x, ti.scale.y);
			printf(" align=%u btype=%d balign=%u", ti.alignment, (int)ti.bounds_type, ti.bounds_alignment);
			pv2("bounds", ti.bounds.x, ti.bounds.y);
			printf(" crop_to_bounds=%d\n", ti.crop_to_bounds);
			struct vec2 bs;
			obs_sceneitem_get_box_scale(it, &bs);
			pv2("box_scale", bs.x, bs.y);
			printf("\n");
		} else if (IS("order")) { /* order ITEM up|down|top|bottom */
			enum obs_order_movement m = !strcmp(sl.argv[2], "up")     ? OBS_ORDER_MOVE_UP
						    : !strcmp(sl.argv[2], "down") ? OBS_ORDER_MOVE_DOWN
						    : !strcmp(sl.argv[2], "top")  ? OBS_ORDER_MOVE_TOP
										  : OBS_ORDER_MOVE_BOTTOM;
			obs_sceneitem_set_order(IT(sl.argv[1]), m);
		} else if (IS("order_pos")) { /* order_pos ITEM N : set position */
			obs_sceneitem_set_order_position(IT(sl.argv[1]), (int)INT(&sl, 2));
		} else if (IS("get_order_pos")) {
			printf("order_pos %s %d\n", sl.argv[1], obs_sceneitem_get_order_position(IT(sl.argv[1])));
		} else if (IS("reorder")) { /* reorder SCENE ITEM... */
			obs_sceneitem_t *items[32];
			size_t n = 0;
			for (int i = 2; i < sl.argc; i++)
				items[n++] = IT(sl.argv[i]);
			printf("reorder -> %d\n", obs_scene_reorder_items(SC(sl.argv[1]), items, n));
		} else if (IS("reorder2")) { /* reorder2 SCENE TOKEN... (TOKEN = item | group/item) */
			struct obs_sceneitem_order_info info[32];
			size_t n = 0;
			for (int i = 2; i < sl.argc; i++)
				sm_order_info(sl.argv[i], &info[n++]);
			printf("reorder2 -> %d\n", obs_scene_reorder_items2(SC(sl.argv[1]), info, n));
		} else if (IS("select")) {
			obs_sceneitem_select(IT(sl.argv[1]), INT(&sl, 2) != 0);
		} else if (IS("visible")) {
			printf("visible %s -> %d\n", sl.argv[1], obs_sceneitem_set_visible(IT(sl.argv[1]), INT(&sl, 2) != 0));
		} else if (IS("lock")) {
			printf("lock %s -> %d\n", sl.argv[1], obs_sceneitem_set_locked(IT(sl.argv[1]), INT(&sl, 2) != 0));
		} else if (IS("audio_flush")) { /* apply all queued visibility actions (what the audio thread does) */
			process_all_audio_actions(IT(sl.argv[1]), 48000);
		} else if (IS("transition")) { /* transition ITEM show|hide DURATION_MS [busy] : attach a fake transition */
			struct obs_source_info info = {.id = "fake_transition", .type = OBS_SOURCE_TYPE_TRANSITION};
			char nm[64];
			snprintf(nm, sizeof(nm), "%s_%s", sl.argv[1], sl.argv[2]);
			obs_source_t *t = sm_new_source(&info, nm, NULL, &sm_main_canvas);
			bool show = !strcmp(sl.argv[2], "show");
			obs_sceneitem_set_transition(IT(sl.argv[1]), show, t);
			obs_sceneitem_set_transition_duration(IT(sl.argv[1]), show, (uint32_t)INT(&sl, 3));
			sm_trans_busy = sl.argc > 4 && !strcmp(sl.argv[4], "busy");
		} else if (IS("defer_begin")) {
			obs_sceneitem_defer_update_begin(IT(sl.argv[1]));
		} else if (IS("defer_end")) {
			obs_sceneitem_defer_update_end(IT(sl.argv[1]));
		} else if (IS("defer_group_begin")) {
			obs_sceneitem_defer_group_resize_begin(IT(sl.argv[1]));
		} else if (IS("defer_group_end")) {
			obs_sceneitem_defer_group_resize_end(IT(sl.argv[1]));
		} else if (IS("force_update")) {
			obs_sceneitem_force_update_transform(IT(sl.argv[1]));
		} else if (IS("set_id")) {
			obs_sceneitem_set_id(IT(sl.argv[1]), INT(&sl, 2));
		} else if (IS("print")) {
			print_item(sl.argv[1]);
		} else if (IS("order_of")) { /* order_of SCENE : item names front-to-back as stored (bottom first) */
			printf("order %s:", sl.argv[1]);
			printf(" ");
			print_order(SC(sl.argv[1]));
		} else if (IS("scene_size")) {
			obs_scene_t *s = SC(sl.argv[1]);
			printf("scene_size %s %u %u (cx=%u cy=%u custom=%d last=%u,%u)\n", sl.argv[1], scene_getwidth(s),
			       scene_getheight(s), s->cx, s->cy, s->custom_size, s->last_width, s->last_height);
		} else if (IS("log")) { /* print and clear the signal/transition log */
			fputs(sm_log, stdout);
			printf("log end\n");
			sm_loglen = 0;
			sm_log[0] = 0;
		} else if (IS("echo")) {
			for (int i = 1; i < sl.argc; i++)
				printf("%s%s", i > 1 ? " " : "", sl.argv[i]);
			printf("\n");
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, c);
			return 2;
		}
	}
	fclose(f);
	return 0;
}
