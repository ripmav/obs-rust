/*
 * Conformance runner for spec S-040 (source core: lifecycle, filters, async video, deinterlace config, transitions).
 * Links the real obs-source.c (via gen/obssource_stubs.c), obs-source-deinterlace.c and obs-source-transition.c.
 * Script language: see test/conformance/README.md, section "obssource runner".
 */
#include <math.h>
#include <stdlib.h>
#include "harness.h"
#include "obssource_api.h"
#include "obs-internal.h"

#define MAXN 48
static obs_source_t *srcs[MAXN];
static char *toks[MAXN];
static int nsrc;
static obs_weak_source_t *weaks[MAXN];
static char *wtoks[MAXN];
static int nweak;
static struct obs_source_frame *held[MAXN]; /* frames obtained with get_frame, indexed like srcs */

static int find_tok(const char *t)
{
	for (int i = 0; i < nsrc; i++)
		if (toks[i] && !strcmp(toks[i], t))
			return i;
	return -1;
}
static obs_source_t *S(const char *t)
{
	if (!strcmp(t, "-"))
		return NULL;
	int i = find_tok(t);
	if (i < 0) {
		fprintf(stderr, "unknown source %s\n", t);
		exit(2);
	}
	return srcs[i];
}
static void bind(const char *t, obs_source_t *s)
{
	int i = find_tok(t);
	if (i < 0)
		i = nsrc++;
	else
		free(toks[i]);
	srcs[i] = s;
	toks[i] = strdup(t);
}
static const char *T(const obs_source_t *s)
{
	if (!s)
		return "-";
	for (int i = 0; i < nsrc; i++)
		if (srcs[i] == s)
			return toks[i];
	return "?";
}
static const char *B(bool b)
{
	return b ? "1" : "0";
}
static void before_destroy(void *param)
{
	for (int i = 0; i < nsrc; i++)
		if (srcs[i] == param) {
			printf("  destroying %s\n", toks[i]);
			srcs[i] = NULL;
			free(toks[i]);
			toks[i] = NULL;
		}
}

/* ---- signal tracing */
static const char *sig_names[] = {"destroy", "remove", "update", "save", "load", "activate", "deactivate", "show", "hide",
				  "mute", "enable", "rename", "volume", "update_flags", "update_properties", "audio_sync", "audio_mixers",
				  "filter_add", "filter_remove", "reorder_filters", "transition_start",
				  "transition_video_stop", "transition_stop", "media_play", "media_pause", "media_restart", "media_stopped", "media_next",
				  "media_previous", "media_started", "media_ended", NULL};
static const char *gsig_names[] = {"source_create", "source_destroy", "source_remove", "source_update",
				   "source_activate", "source_deactivate", "source_show", "source_hide", "source_rename",
				   "source_filter_add", "source_filter_remove", "source_transition_start",
				   "source_transition_video_stop", "source_transition_stop", NULL};

static void sig_common(const char *prefix, const char *name, calldata_t *cd)
{
	void *p;
	long long iv;
	bool bv;
	const char *sv;
	printf("%s %s", prefix, name);
	if (calldata_get_ptr(cd, "source", &p))
		printf(" source=%s", ((obs_source_t *)p)->context.name);
	if (calldata_get_ptr(cd, "filter", &p))
		printf(" filter=%s", ((obs_source_t *)p)->context.name);
	if (calldata_get_bool(cd, "enabled", &bv))
		printf(" enabled=%d", bv);
	if (calldata_get_int(cd, "flags", &iv))
		printf(" flags=%lld", iv);
	if (calldata_get_string(cd, "new_name", &sv))
		printf(" new_name=%s", sv);
	if (calldata_get_string(cd, "prev_name", &sv))
		printf(" prev_name=%s", sv);
	printf("\n");
}
static void sig_cb(void *param, calldata_t *cd)
{
	sig_common("  sig", param, cd);
}
static void gsig_cb(void *param, calldata_t *cd)
{
	sig_common("  gsig", param, cd);
}

static obs_data_array_t *backup;
static const char *cur_enum;
static void enum_cb(obs_source_t *parent, obs_source_t *child, void *param)
{
	(void)param;
	printf(" (%s>%s)", T(parent), T(child));
}

static void print_filters(obs_source_t *s)
{
	printf("filters %s count=%zu", T(s), s->filters.num);
	for (size_t i = 0; i < s->filters.num; i++) {
		obs_source_t *f = s->filters.array[i];
		printf(" [%zu]=%s(target=%s,parent=%s)", i, T(f), T(f->filter_target), T(f->filter_parent));
	}
	printf("\n");
}

static void print_state(obs_source_t *s)
{
	printf("state %s show_refs=%ld activate_refs=%ld showing=%s active=%s obs_showing=%s obs_active=%s\n", T(s),
	       (long)s->show_refs, (long)s->activate_refs, B(s->showing), B(s->active), B(obs_source_showing(s)),
	       B(obs_source_active(s)));
}

/* ---- video */
static const struct {
	const char *n;
	enum video_format f;
} fmts[] = {{"none", VIDEO_FORMAT_NONE}, {"i420", VIDEO_FORMAT_I420}, {"nv12", VIDEO_FORMAT_NV12},
	    {"yuy2", VIDEO_FORMAT_YUY2}, {"uyvy", VIDEO_FORMAT_UYVY}, {"rgba", VIDEO_FORMAT_RGBA},
	    {"bgra", VIDEO_FORMAT_BGRA}, {"bgrx", VIDEO_FORMAT_BGRX}, {"y800", VIDEO_FORMAT_Y800},
	    {"i444", VIDEO_FORMAT_I444}};
static enum video_format F(const char *n)
{
	for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++)
		if (!strcmp(fmts[i].n, n))
			return fmts[i].f;
	fprintf(stderr, "bad format %s\n", n);
	exit(2);
}
static const char *Fn(enum video_format f)
{
	for (size_t i = 0; i < sizeof(fmts) / sizeof(fmts[0]); i++)
		if (fmts[i].f == f)
			return fmts[i].n;
	return "other";
}
static uint32_t plane_h(enum video_format f, int plane, uint32_t h)
{
	if (plane == 0)
		return h;
	if (f == VIDEO_FORMAT_I420 || f == VIDEO_FORMAT_NV12)
		return (h + 1) / 2;
	return h;
}
static struct obs_source_frame *mkframe(enum video_format f, uint32_t w, uint32_t h, uint64_t ts, bool full, int fill,
					bool flip)
{
	struct obs_source_frame *fr = obs_source_frame_create(f, w, h);
	for (int i = 0; i < MAX_AV_PLANES; i++)
		if (fr->data[i] && fr->linesize[i])
			memset(fr->data[i], fill, (size_t)fr->linesize[i] * plane_h(f, i, h));
	fr->timestamp = ts;
	fr->full_range = full;
	fr->flip = flip;
	return fr;
}
static void print_frame(const char *tag, const struct obs_source_frame *fr)
{
	if (!fr) {
		printf("%s none\n", tag);
		return;
	}
	printf("%s ts=%llu %ux%u fmt=%s full_range=%s flip=%s fill=%d refs=%ld prev_frame=%s\n", tag,
	       (unsigned long long)fr->timestamp, fr->width, fr->height, Fn(fr->format), B(fr->full_range), B(fr->flip),
	       fr->data[0] ? fr->data[0][0] : -1, (long)fr->refs, B(fr->prev_frame));
}
static void print_astate(obs_source_t *s)
{
	size_t used = 0;
	for (size_t i = 0; i < s->async_cache.num; i++)
		used += s->async_cache.array[i].used;
	printf("astate %s active=%s last_frame_ts=%llu queue=[", T(s), B(s->async_active),
	       (unsigned long long)s->last_frame_ts);
	for (size_t i = 0; i < s->async_frames.num; i++)
		printf("%s%llu", i ? "," : "", (unsigned long long)s->async_frames.array[i]->timestamp);
	printf("] cache=%zu used=%zu [", s->async_cache.num, used);
	for (size_t i = 0; i < s->async_cache.num; i++)
		printf("%s%d/%d", i ? "," : "", s->async_cache.array[i].used, s->async_cache.array[i].unused_count);
	printf("] cur=");
	if (s->cur_async_frame)
		printf("%llu", (unsigned long long)s->cur_async_frame->timestamp);
	else
		printf("-");
	printf(" prev=");
	if (s->prev_async_frame)
		printf("%llu", (unsigned long long)s->prev_async_frame->timestamp);
	else
		printf("-");
	printf(" tex=%ux%u fmt=%s full_range=%s gpu=%s rotation=%ld unbuffered=%s\n", s->async_width, s->async_height,
	       Fn(s->async_format), B(s->async_full_range), B(s->async_gpu_conversion), s->async_rotation,
	       B(s->async_unbuffered));
}

/* ---- transitions */
static float mix_a(void *d, float t)
{
	(void)d;
	return 1.0f - t;
}
static float mix_b(void *d, float t)
{
	(void)d;
	return t;
}
static void pf(float v)
{
	if (v != v)
		fputs("nan", stdout);
	else if (isinf(v))
		fputs(v < 0 ? "-inf" : "inf", stdout);
	else
		printf("%.9g", v == 0 ? 0.0f : v);
}
static void vr_cb(void *data, gs_texture_t *a, gs_texture_t *b, float t, uint32_t cx, uint32_t cy)
{
	(void)data, (void)a, (void)b;
	printf("  vcb t=");
	pf(t);
	printf(" size=%ux%u\n", cx, cy);
}
static void print_tr(obs_source_t *t)
{
	printf("tr %s A=%s(act=%s) B=%s(act=%s) video=%s audio=%s mode=%d manual_val=", T(t),
	       T(t->transition_sources[0]), B(t->transition_source_active[0]), T(t->transition_sources[1]),
	       B(t->transition_source_active[1]), B(t->transitioning_video), B(t->transitioning_audio),
	       (int)t->transition_mode);
	pf(t->transition_manual_val);
	printf(" target=");
	pf(t->transition_manual_target);
	printf(" size=%ux%u actual=%ux%u start=%llu dur=%llu\n", t->transition_cx, t->transition_cy,
	       t->transition_actual_cx, t->transition_actual_cy, (unsigned long long)t->transition_start_time,
	       (unsigned long long)t->transition_duration);
}

static uint32_t parse_flags(const char *s)
{
	static const struct {
		const char *n;
		uint32_t v;
	} t[] = {{"video", OBS_SOURCE_VIDEO}, {"audio", OBS_SOURCE_AUDIO}, {"async", OBS_SOURCE_ASYNC},
		 {"asyncvideo", OBS_SOURCE_ASYNC_VIDEO}, {"customdraw", OBS_SOURCE_CUSTOM_DRAW},
		 {"composite", OBS_SOURCE_COMPOSITE}, {"nodup", OBS_SOURCE_DO_NOT_DUPLICATE},
		 {"controllable", OBS_SOURCE_CONTROLLABLE_MEDIA}};
	uint32_t v = 0;
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", s);
	if (!strcmp(buf, "-"))
		return 0;
	for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
		bool ok = false;
		for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
			if (!strcmp(t[i].n, tok))
				v |= t[i].v, ok = true;
		if (!ok) {
			fprintf(stderr, "bad flag %s\n", tok);
			exit(2);
		}
	}
	return v;
}
static uint32_t parse_hooks(const char *s)
{
	uint32_t v = 0;
	char buf[256];
	snprintf(buf, sizeof(buf), "%s", s);
	if (!strcmp(buf, "-"))
		return 0;
	for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
		uint32_t h = fk_hook_from_name(tok);
		if (!h) {
			fprintf(stderr, "bad hook %s\n", tok);
			exit(2);
		}
		v |= h;
	}
	return v;
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	core_reset();
	while (script_next(f, &sl, &lineno)) {
		char **a = sl.argv;
		const char *cmd = a[0];
#define IS(x) (!strcmp(cmd, x))
		if (IS("echo")) {
			script_need(&sl, 2);
			printf("echo %s\n", a[1]);
		} else if (IS("clock")) { /* ns: os_gettime_ns */
			script_need(&sl, 2);
			os_set_clock(strtoull(a[1], 0, 0));
		} else if (IS("vtime")) { /* ns: obs->video.video_time */
			script_need(&sl, 2);
			obs->video.video_time = strtoull(a[1], 0, 0);
		} else if (IS("half")) { /* ns: half frame interval */
			script_need(&sl, 2);
			obs->video.video_half_frame_interval_ns = strtoull(a[1], 0, 0);
		} else if (IS("kind")) { /* id type flags hooks */
			script_need(&sl, 4);
			enum obs_source_type ty = !strcmp(a[2], "input")    ? OBS_SOURCE_TYPE_INPUT
						  : !strcmp(a[2], "filter") ? OBS_SOURCE_TYPE_FILTER
									    : OBS_SOURCE_TYPE_TRANSITION;
			fk_kind_add(a[1], ty, parse_flags(a[3]),
				    sl.argc > 4 ? parse_hooks(a[4]) : 0);
		} else if (IS("create")) { /* tok kind name [private] */
			script_need(&sl, 4);
			bool priv = sl.argc > 4 && !strcmp(a[4], "private");
			obs_source_t *s = priv ? obs_source_create_private(a[2], a[3], NULL)
					       : obs_source_create(a[2], a[3], NULL, NULL);
			printf("create %s %s\n", a[1], s ? "ok" : "fail");
			if (s)
				bind(a[1], s);
		} else if (IS("setting")) { /* tok key int */
			script_need(&sl, 4);
			obs_data_set_int(S(a[1])->context.settings, a[2], strtoll(a[3], 0, 0));
		} else if (IS("get_setting")) {
			script_need(&sl, 3);
			printf("setting %s %s %lld\n", a[1], a[2], (long long)obs_data_get_int(S(a[1])->context.settings, a[2]));
		} else if (IS("update")) { /* tok [key int] */
			script_need(&sl, 2);
			if (sl.argc >= 4) {
				obs_data_t *d = obs_data_create();
				obs_data_set_int(d, a[2], strtoll(a[3], 0, 0));
				obs_source_update(S(a[1]), d);
				obs_data_release(d);
			} else {
				obs_source_update(S(a[1]), NULL);
			}
		} else if (IS("reset_settings")) {
			script_need(&sl, 2);
			obs_source_reset_settings(S(a[1]), NULL);
		} else if (IS("update_props")) {
			script_need(&sl, 2);
			obs_source_update_properties(S(a[1]));
		} else if (IS("deferred")) {
			script_need(&sl, 2);
			printf("deferred %s %ld\n", a[1], (long)S(a[1])->defer_update_count);
		} else if (IS("enum_active") || IS("enum_tree") || IS("enum_full")) {
			script_need(&sl, 2);
			printf("%s %s:", cmd, a[1]);
			cur_enum = a[1];
			if (IS("enum_active"))
				obs_source_enum_active_sources(S(a[1]), enum_cb, NULL);
			else if (IS("enum_tree"))
				obs_source_enum_active_tree(S(a[1]), enum_cb, NULL);
			else
				obs_source_enum_full_tree(S(a[1]), enum_cb, NULL);
			printf("\n");
		} else if (IS("fksize")) {
			script_need(&sl, 4);
			fk_set_size(S(a[1]), (uint32_t)atoi(a[2]), (uint32_t)atoi(a[3]));
		} else if (IS("fkdrop")) {
			script_need(&sl, 3);
			fk_set_drop(S(a[1]), atoi(a[2]));
		} else if (IS("child")) {
			script_need(&sl, 3);
			fk_child_add(S(a[1]), S(a[2]));
		} else if (IS("child_clear")) {
			script_need(&sl, 2);
			fk_child_clear(S(a[1]));
		} else if (IS("addref")) {
			script_need(&sl, 2);
			obs_source_addref(S(a[1]));
		} else if (IS("release")) {
			script_need(&sl, 2);
			obs_source_release(S(a[1]));
		} else if (IS("refs")) { /* number of strong references held */
			script_need(&sl, 2);
			printf("refs %s %ld\n", a[1], (long)S(a[1])->context.control->ref.refs + 1);
		} else if (IS("getref")) { /* tok newtok */
			script_need(&sl, 3);
			obs_source_t *r = obs_source_get_ref(S(a[1]));
			printf("getref %s %s\n", a[1], r ? "ok" : "null");
			if (r)
				bind(a[2], r);
		} else if (IS("weak")) { /* tok wtok */
			script_need(&sl, 3);
			weaks[nweak] = obs_source_get_weak_source(S(a[1]));
			wtoks[nweak++] = strdup(a[2]);
		} else if (IS("weak_get") || IS("weak_expired") || IS("weak_release")) {
			script_need(&sl, 2);
			int w = -1;
			for (int i = 0; i < nweak; i++)
				if (!strcmp(wtoks[i], a[1]))
					w = i;
			if (w < 0)
				exit(2);
			if (IS("weak_expired")) {
				printf("weak_expired %s %s\n", a[1], B(obs_weak_source_expired(weaks[w])));
			} else if (IS("weak_release")) {
				obs_weak_source_release(weaks[w]);
			} else {
				script_need(&sl, 3);
				obs_source_t *r = obs_weak_source_get_source(weaks[w]);
				printf("weak_get %s %s\n", a[1], r ? "ok" : "null");
				if (r)
					bind(a[2], r);
			}
		} else if (IS("drain")) {
			printf("drain ran=%d\n", os_drain_tasks(before_destroy));
		} else if (IS("pending")) {
			printf("pending %d\n", os_pending_tasks());
		} else if (IS("remove")) {
			script_need(&sl, 2);
			obs_source_remove(S(a[1]));
		} else if (IS("removed")) {
			script_need(&sl, 2);
			printf("removed %s %s\n", a[1], B(obs_source_removed(S(a[1]))));
		} else if (IS("rename")) {
			script_need(&sl, 3);
			obs_source_set_name(S(a[1]), a[2]);
		} else if (IS("info")) { /* tok */
			script_need(&sl, 2);
			obs_source_t *s = S(a[1]);
			printf("info %s name=%s id=%s type=%d enabled=%s hidden=%s flags=%u mixers=%u private=%s\n", a[1],
			       obs_source_get_name(s), obs_source_get_id(s), (int)obs_source_get_type(s),
			       B(obs_source_enabled(s)), B(obs_source_is_hidden(s)), obs_source_get_flags(s),
			       obs_source_get_audio_mixers(s), B(s->context.private));
		} else if (IS("enable")) {
			script_need(&sl, 3);
			obs_source_set_enabled(S(a[1]), atoi(a[2]));
		} else if (IS("hidden")) {
			script_need(&sl, 3);
			obs_source_set_hidden(S(a[1]), atoi(a[2]));
		} else if (IS("set_flags")) {
			script_need(&sl, 3);
			obs_source_set_flags(S(a[1]), (uint32_t)strtoul(a[2], 0, 0));
		} else if (IS("set_default_flags")) {
			script_need(&sl, 3);
			obs_source_set_default_flags(S(a[1]), (uint32_t)strtoul(a[2], 0, 0));
		} else if (IS("set_mixers")) {
			script_need(&sl, 3);
			obs_source_set_audio_mixers(S(a[1]), (uint32_t)strtoul(a[2], 0, 0));
		} else if (IS("activate") || IS("deactivate")) { /* tok main|aux */
			script_need(&sl, 3);
			enum view_type vt = !strcmp(a[2], "main") ? MAIN_VIEW : AUX_VIEW;
			if (IS("activate"))
				obs_source_activate(S(a[1]), vt);
			else
				obs_source_deactivate(S(a[1]), vt);
		} else if (IS("inc_showing")) {
			script_need(&sl, 2);
			obs_source_inc_showing(S(a[1]));
		} else if (IS("inc_active")) {
			script_need(&sl, 2);
			obs_source_inc_active(S(a[1]));
		} else if (IS("dec_showing")) {
			script_need(&sl, 2);
			obs_source_dec_showing(S(a[1]));
		} else if (IS("dec_active")) {
			script_need(&sl, 2);
			obs_source_dec_active(S(a[1]));
		} else if (IS("state")) {
			script_need(&sl, 2);
			print_state(S(a[1]));
		} else if (IS("tick")) {
			script_need(&sl, 3);
			obs_source_video_tick(S(a[1]), (float)atof(a[2]));
		} else if (IS("add_active_child") || IS("remove_active_child")) {
			script_need(&sl, 3);
			if (IS("add_active_child"))
				printf("add_active_child %s %s %s\n", a[1], a[2],
				       B(obs_source_add_active_child(S(a[1]), S(a[2]))));
			else
				obs_source_remove_active_child(S(a[1]), S(a[2]));
		} else if (IS("size")) {
			script_need(&sl, 2);
			printf("size %s %ux%u\n", a[1], obs_source_get_width(S(a[1])), obs_source_get_height(S(a[1])));
		} else if (IS("watch")) {
			script_need(&sl, 2);
			for (int i = 0; sig_names[i]; i++)
				signal_handler_connect(obs_source_get_signal_handler(S(a[1])), sig_names[i], sig_cb,
						       (void *)sig_names[i]);
		} else if (IS("watch_global")) {
			for (int i = 0; gsig_names[i]; i++)
				signal_handler_connect(obs->signals, gsig_names[i], gsig_cb, (void *)gsig_names[i]);
		} else if (IS("filter_add")) {
			script_need(&sl, 3);
			obs_source_filter_add(S(a[1]), S(a[2]));
		} else if (IS("filter_remove")) {
			script_need(&sl, 3);
			obs_source_filter_remove(S(a[1]), S(a[2]));
		} else if (IS("filter_order")) { /* src filter up|down|top|bottom */
			script_need(&sl, 4);
			enum obs_order_movement mv = !strcmp(a[3], "up")     ? OBS_ORDER_MOVE_UP
						     : !strcmp(a[3], "down") ? OBS_ORDER_MOVE_DOWN
						     : !strcmp(a[3], "top")  ? OBS_ORDER_MOVE_TOP
									     : OBS_ORDER_MOVE_BOTTOM;
			obs_source_filter_set_order(S(a[1]), S(a[2]), mv);
		} else if (IS("filter_index")) {
			script_need(&sl, 3);
			printf("filter_index %s %s %d\n", a[1], a[2], obs_source_filter_get_index(S(a[1]), S(a[2])));
		} else if (IS("filter_set_index")) {
			script_need(&sl, 4);
			obs_source_filter_set_index(S(a[1]), S(a[2]), (size_t)strtoul(a[3], 0, 0));
		} else if (IS("filters")) {
			script_need(&sl, 2);
			print_filters(S(a[1]));
		} else if (IS("filter_count")) {
			script_need(&sl, 2);
			printf("filter_count %s %zu\n", a[1], obs_source_filter_count(S(a[1])));
		} else if (IS("filter_by_name")) { /* src name tok */
			script_need(&sl, 4);
			obs_source_t *r = obs_source_get_filter_by_name(S(a[1]), a[2]);
			printf("filter_by_name %s %s %s\n", a[1], a[2], r ? "found" : "null");
			if (r)
				bind(a[3], r);
		} else if (IS("filter_target")) {
			script_need(&sl, 2);
			printf("filter_target %s %s\n", a[1], T(obs_filter_get_target(S(a[1]))));
		} else if (IS("enum_filters")) {
			script_need(&sl, 2);
			printf("enum_filters %s:", a[1]);
			obs_source_t *s = S(a[1]);
			for (size_t i = s->filters.num; i > 0; i--)
				printf(" %s", T(s->filters.array[i - 1]));
			printf("\n");
		} else if (IS("copy_filters")) {
			script_need(&sl, 3);
			obs_source_copy_filters(S(a[1]), S(a[2]));
		} else if (IS("copy_filter")) {
			script_need(&sl, 3);
			obs_source_copy_single_filter(S(a[1]), S(a[2]));
		} else if (IS("duplicate")) { /* src newtok newname [private] */
			script_need(&sl, 4);
			bool priv = sl.argc > 4 && !strcmp(a[4], "private");
			obs_source_t *r = obs_source_duplicate(S(a[1]), a[3], priv);
			printf("duplicate %s %s\n", a[1], r ? (r == S(a[1]) ? "same" : "new") : "null");
			if (r)
				bind(a[2], r);
		} else if (IS("names")) { /* all filters' names of a source in array order */
			script_need(&sl, 2);
			obs_source_t *s = S(a[1]);
			printf("names %s:", a[1]);
			for (size_t i = 0; i < s->filters.num; i++)
				printf(" %s", s->filters.array[i]->context.name);
			printf("\n");
		} else if (IS("video") || IS("video2") || IS("preload") || IS("set_video_frame")) {
			/* tok fmt w h ts full fill flip */
			script_need(&sl, 9);
			struct obs_source_frame *fr = mkframe(F(a[2]), (uint32_t)atoi(a[3]), (uint32_t)atoi(a[4]),
							      strtoull(a[5], 0, 0), atoi(a[6]), atoi(a[7]), atoi(a[8]));
			if (IS("video"))
				obs_source_output_video(S(a[1]), fr);
			else if (IS("preload"))
				obs_source_preload_video(S(a[1]), fr);
			else if (IS("set_video_frame"))
				obs_source_set_video_frame(S(a[1]), fr);
			else { /* frame2: full flag selects range 0=default 1=partial 2=full */
				struct obs_source_frame2 f2 = {0};
				for (int i = 0; i < MAX_AV_PLANES; i++)
					f2.data[i] = fr->data[i], f2.linesize[i] = fr->linesize[i];
				f2.width = fr->width, f2.height = fr->height, f2.timestamp = fr->timestamp;
				f2.format = fr->format;
				f2.range = atoi(a[6]) == 0 ? VIDEO_RANGE_DEFAULT
					   : atoi(a[6]) == 1 ? VIDEO_RANGE_PARTIAL
							     : VIDEO_RANGE_FULL;
				f2.flip = fr->flip;
				obs_source_output_video2(S(a[1]), &f2);
			}
			obs_source_frame_destroy(fr);
		} else if (IS("video_null")) {
			script_need(&sl, 2);
			obs_source_output_video(S(a[1]), NULL);
		} else if (IS("show_preloaded")) {
			script_need(&sl, 2);
			obs_source_show_preloaded_video(S(a[1]));
		} else if (IS("astate")) {
			script_need(&sl, 2);
			print_astate(S(a[1]));
		} else if (IS("get_frame")) {
			script_need(&sl, 2);
			int i = find_tok(a[1]);
			held[i] = obs_source_get_frame(srcs[i]);
			print_frame("get_frame", held[i]);
		} else if (IS("release_frame")) {
			script_need(&sl, 2);
			int i = find_tok(a[1]);
			obs_source_release_frame(srcs[i], held[i]);
			held[i] = NULL;
		} else if (IS("unbuffered")) {
			script_need(&sl, 3);
			obs_source_set_async_unbuffered(S(a[1]), atoi(a[2]));
		} else if (IS("rotation")) {
			script_need(&sl, 3);
			obs_source_set_async_rotation(S(a[1]), atol(a[2]));
		} else if (IS("media")) { /* tok play|pause|restart|stop|next|prev|settime N|started|ended */
			script_need(&sl, 3);
			obs_source_t *s = S(a[1]);
			if (!strcmp(a[2], "play"))
				obs_source_media_play_pause(s, false);
			else if (!strcmp(a[2], "pause"))
				obs_source_media_play_pause(s, true);
			else if (!strcmp(a[2], "restart"))
				obs_source_media_restart(s);
			else if (!strcmp(a[2], "stop"))
				obs_source_media_stop(s);
			else if (!strcmp(a[2], "next"))
				obs_source_media_next(s);
			else if (!strcmp(a[2], "prev"))
				obs_source_media_previous(s);
			else if (!strcmp(a[2], "settime"))
				obs_source_media_set_time(s, atoll(a[3]));
			else if (!strcmp(a[2], "started"))
				obs_source_media_started(s);
			else if (!strcmp(a[2], "ended"))
				obs_source_media_ended(s);
			else if (!strcmp(a[2], "query"))
				printf("media %s duration=%lld time=%lld state=%d queued=%zu\n", a[1],
				       (long long)obs_source_media_get_duration(s), (long long)obs_source_media_get_time(s),
				       (int)obs_source_media_get_state(s), s->media_actions.num);
		} else if (IS("backup")) {
			script_need(&sl, 2);
			if (backup)
				obs_data_array_release(backup);
			backup = obs_source_backup_filters(S(a[1]));
			printf("backup %s count=%zu\n", a[1], obs_data_array_count(backup));
		} else if (IS("restore")) {
			script_need(&sl, 2);
			obs_source_restore_filters(S(a[1]), backup);
		} else if (IS("deint_mode")) {
			script_need(&sl, 3);
			obs_source_set_deinterlace_mode(S(a[1]), (enum obs_deinterlace_mode)atoi(a[2]));
		} else if (IS("deint_field")) {
			script_need(&sl, 3);
			obs_source_set_deinterlace_field_order(S(a[1]), (enum obs_deinterlace_field_order)atoi(a[2]));
		} else if (IS("deint_get")) {
			script_need(&sl, 2);
			obs_source_t *s = S(a[1]);
			printf("deint %s mode=%d field=%d half_dur=%u offset=%llu frame_ts=%llu\n", a[1],
			       (int)obs_source_get_deinterlace_mode(s), (int)obs_source_get_deinterlace_field_order(s),
			       s->deinterlace_half_duration, (unsigned long long)s->deinterlace_offset,
			       (unsigned long long)s->deinterlace_frame_ts);
		} else if (IS("tr_start")) { /* t mode ms dest */
			script_need(&sl, 5);
			printf("tr_start %s %s\n", a[1],
			       B(obs_transition_start(S(a[1]), (enum obs_transition_mode)atoi(a[2]),
						      (uint32_t)strtoul(a[3], 0, 0), S(a[4]))));
		} else if (IS("tr_set")) {
			script_need(&sl, 3);
			obs_transition_set(S(a[1]), S(a[2]));
		} else if (IS("tr_clear")) {
			script_need(&sl, 2);
			obs_transition_clear(S(a[1]));
		} else if (IS("tr_state")) {
			script_need(&sl, 2);
			print_tr(S(a[1]));
		} else if (IS("tr_time")) {
			script_need(&sl, 2);
			printf("tr_time %s ", a[1]);
			pf(obs_transition_get_time(S(a[1])));
			printf("\n");
		} else if (IS("tr_manual")) {
			script_need(&sl, 3);
			obs_transition_set_manual_time(S(a[1]), (float)atof(a[2]));
		} else if (IS("tr_torque")) {
			script_need(&sl, 4);
			obs_transition_set_manual_torque(S(a[1]), (float)atof(a[2]), (float)atof(a[3]));
		} else if (IS("tr_fixed")) { /* t on ms */
			script_need(&sl, 4);
			obs_transition_enable_fixed(S(a[1]), atoi(a[2]), (uint32_t)strtoul(a[3], 0, 0));
		} else if (IS("tr_fixed_get")) {
			script_need(&sl, 2);
			printf("tr_fixed %s %s\n", a[1], B(obs_transition_fixed(S(a[1]))));
		} else if (IS("tr_scale")) {
			script_need(&sl, 3);
			obs_transition_set_scale_type(S(a[1]), (enum obs_transition_scale_type)atoi(a[2]));
		} else if (IS("tr_align")) {
			script_need(&sl, 3);
			obs_transition_set_alignment(S(a[1]), (uint32_t)strtoul(a[2], 0, 0));
		} else if (IS("tr_size")) {
			script_need(&sl, 4);
			obs_transition_set_size(S(a[1]), (uint32_t)atoi(a[2]), (uint32_t)atoi(a[3]));
		} else if (IS("tr_get")) {
			script_need(&sl, 2);
			uint32_t cx, cy;
			obs_transition_get_size(S(a[1]), &cx, &cy);
			printf("tr_get %s size=%ux%u scale=%d align=%u active=%s\n", a[1], cx, cy,
			       (int)obs_transition_get_scale_type(S(a[1])), obs_transition_get_alignment(S(a[1])),
			       B(obs_transition_is_active(S(a[1]))));
		} else if (IS("tr_source")) { /* t 0|1 */
			script_need(&sl, 3);
			obs_source_t *r = obs_transition_get_source(S(a[1]), (enum obs_transition_target)atoi(a[2]));
			printf("tr_source %s %s %s\n", a[1], a[2], T(r));
			obs_source_release(r);
		} else if (IS("tr_active_source")) {
			script_need(&sl, 2);
			obs_source_t *r = obs_transition_get_active_source(S(a[1]));
			printf("tr_active_source %s %s\n", a[1], T(r));
			obs_source_release(r);
		} else if (IS("tr_matrix")) {
			script_need(&sl, 3);
			const struct matrix4 *m = &S(a[1])->transition_matrices[atoi(a[2])];
			printf("tr_matrix %s %s scale=", a[1], a[2]);
			pf(m->x.x);
			printf(",");
			pf(m->y.y);
			printf(" pos=");
			pf(m->t.x);
			printf(",");
			pf(m->t.y);
			printf("\n");
		} else if (IS("tr_video")) {
			script_need(&sl, 2);
			obs_transition_video_render(S(a[1]), vr_cb);
		} else if (IS("tr_video_direct")) {
			script_need(&sl, 3);
			printf("tr_video_direct %s %s\n", a[1],
			       B(obs_transition_video_render_direct(S(a[1]), (enum obs_transition_target)atoi(a[2]))));
		} else if (IS("audio_ts")) { /* tok ts pending dup */
			script_need(&sl, 5);
			obs_source_t *s = S(a[1]);
			s->audio_ts = strtoull(a[2], 0, 0);
			s->audio_pending = atoi(a[3]);
			s->audio_is_duplicated = atoi(a[4]);
		} else if (IS("audio_fill")) { /* tok value (mix 0..5, ch 0..1) */
			script_need(&sl, 3);
			obs_source_t *s = S(a[1]);
			for (int m = 0; m < MAX_AUDIO_MIXES; m++)
				for (int c = 0; c < 2; c++)
					for (int i = 0; i < AUDIO_OUTPUT_FRAMES; i++)
						s->audio_output_buf[m][c][i] = (float)atof(a[2]);
		} else if (IS("tr_audio")) { /* t mixers channels rate */
			script_need(&sl, 5);
			static float buf[MAX_AUDIO_MIXES][MAX_AUDIO_CHANNELS][AUDIO_OUTPUT_FRAMES];
			struct obs_source_audio_mix mixb;
			memset(buf, 0, sizeof(buf));
			for (int m = 0; m < MAX_AUDIO_MIXES; m++)
				for (int c = 0; c < MAX_AUDIO_CHANNELS; c++)
					mixb.output[m].data[c] = buf[m][c];
			uint64_t ts = 0;
			bool r = obs_transition_audio_render(S(a[1]), &ts, &mixb, (uint32_t)strtoul(a[2], 0, 0),
							     (size_t)atoi(a[3]), (size_t)atoi(a[4]), mix_a, mix_b);
			printf("tr_audio %s ret=%s ts=%llu mix0ch0[0]=", a[1], B(r), (unsigned long long)ts);
			pf(buf[0][0][0]);
			printf(" [512]=");
			pf(buf[0][0][512]);
			printf(" [1023]=");
			pf(buf[0][0][1023]);
			printf(" mix1ch0[0]=");
			pf(buf[1][0][0]);
			printf(" mix0ch1[0]=");
			pf(buf[0][1][0]);
			printf("\n");
		} else if (IS("tr_swap")) {
			script_need(&sl, 3);
			obs_transition_swap_begin(S(a[1]), S(a[2]));
			obs_transition_swap_end(S(a[1]), S(a[2]));
		} else if (IS("tr_force_stop")) {
			script_need(&sl, 2);
			obs_transition_force_stop(S(a[1]));
		} else if (IS("tr_save")) {
			script_need(&sl, 2);
			obs_data_t *d = obs_data_create();
			obs_transition_save(S(a[1]), d);
			printf("tr_save %s source_a=\"%s\" align=%lld mode=%lld scale=%lld cx=%lld cy=%lld\n", a[1],
			       obs_data_get_string(d, "transition_source_a"), (long long)obs_data_get_int(d, "transition_alignment"),
			       (long long)obs_data_get_int(d, "transition_mode"),
			       (long long)obs_data_get_int(d, "transition_scale_type"), (long long)obs_data_get_int(d, "transition_cx"),
			       (long long)obs_data_get_int(d, "transition_cy"));
			obs_data_release(d);
		} else if (IS("tr_load")) { /* t source_a align mode scale cx cy */
			script_need(&sl, 8);
			obs_data_t *d = obs_data_create();
			obs_data_set_string(d, "transition_source_a", a[2]);
			obs_data_set_int(d, "transition_alignment", atoll(a[3]));
			obs_data_set_int(d, "transition_mode", atoll(a[4]));
			obs_data_set_int(d, "transition_scale_type", atoll(a[5]));
			obs_data_set_int(d, "transition_cx", atoll(a[6]));
			obs_data_set_int(d, "transition_cy", atoll(a[7]));
			obs_transition_load(S(a[1]), d);
			obs_data_release(d);
		} else if (IS("tr_enum")) {
			script_need(&sl, 2);
			obs_source_t *t = S(a[1]);
			printf("tr_enum %s:", a[1]);
			for (int i = 0; i < 2; i++)
				if (t->transition_sources[i])
					printf(" %s", T(t->transition_sources[i]));
			printf("\n");
		} else {
			fprintf(stderr, "line %d: unknown command %s\n", sl.lineno, cmd);
			return 2;
		}
		fflush(stdout);
	}
	return 0;
}
