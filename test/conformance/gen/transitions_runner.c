/*
 * Conformance runner for the OBS transitions plugin (spec S-029).  See README "transitions runner".
 * The real plugin sources are #included (static names of the per-file mix_a/mix_b are renamed).
 */
#include <dirent.h>
#include <sys/wait.h>
#include <unistd.h>

#include "harness.h"

#define mix_a cut_mix_a
#define mix_b cut_mix_b
#include "../../../plugins/obs-transitions/transition-cut.c"
#undef mix_a
#undef mix_b
#define mix_a fade_mix_a
#define mix_b fade_mix_b
#include "../../../plugins/obs-transitions/transition-fade.c"
#undef mix_a
#undef mix_b
#define mix_a ftc_mix_a
#define mix_b ftc_mix_b
#include "../../../plugins/obs-transitions/transition-fade-to-color.c"
#undef mix_a
#undef mix_b
#define mix_a luma_mix_a
#define mix_b luma_mix_b
#include "../../../plugins/obs-transitions/transition-luma-wipe.c"
#undef mix_a
#undef mix_b
#define mix_a slide_mix_a
#define mix_b slide_mix_b
#include "../../../plugins/obs-transitions/transition-slide.c"
#undef mix_a
#undef mix_b
#define mix_a swipe_mix_a
#define mix_b swipe_mix_b
#include "../../../plugins/obs-transitions/transition-swipe.c"
#undef mix_a
#undef mix_b
#include "../../../plugins/obs-transitions/transition-stinger.c"
#include "../../../plugins/obs-transitions/obs-transitions.c"

/* stubs API */
extern bool fk_command(const struct script_line *sl);
extern void fk_reset(void);
extern const struct obs_source_info *fk_registry_find(const char *id);
extern int fk_registry_count(void);
extern const struct obs_source_info *fk_registry_at(int i);
extern obs_source_t *fk_create(const struct obs_source_info *info, const char *name, obs_data_t *user);
extern void *fk_data(obs_source_t *s);
extern const struct obs_source_info *fk_info(obs_source_t *s);
extern obs_data_t *fk_settings(obs_source_t *s);
extern int fk_defer_count(obs_source_t *s);
extern obs_source_t *fk_find(const char *name);
extern void fk_tick(obs_source_t *s, float seconds);
extern void fk_destroy(obs_source_t *s);
extern void fk_print_sources(void);
extern bool fk_png_info(const char *path, uint32_t *w, uint32_t *h, int *depth, int *ctype, uint64_t *fnv,
			size_t *len);
extern const char *fk_f9(double v);
extern double fk_mix_t[16];
extern int fk_mix_n;

static void f9(const char *label, double v)
{
	printf("  %s=%.9g\n", label, v);
}

/* "type:key=value" settings tokens; types: int str bool dbl */
static void apply_setting(obs_data_t *d, const char *tok)
{
	const char *colon = strchr(tok, ':'), *eq = strchr(tok, '=');
	if (!colon || !eq || eq < colon) {
		fprintf(stderr, "script: bad setting '%s'\n", tok);
		exit(2);
	}
	char type[8], key[64];
	snprintf(type, sizeof(type), "%.*s", (int)(colon - tok), tok);
	snprintf(key, sizeof(key), "%.*s", (int)(eq - colon - 1), colon + 1);
	const char *v = eq + 1;
	if (!strcmp(type, "int"))
		obs_data_set_int(d, key, strtoll(v, NULL, 0));
	else if (!strcmp(type, "str"))
		obs_data_set_string(d, key, v);
	else if (!strcmp(type, "bool"))
		obs_data_set_bool(d, key, atoi(v) != 0);
	else if (!strcmp(type, "dbl"))
		obs_data_set_double(d, key, strtod(v, NULL));
	else {
		fprintf(stderr, "script: bad setting type '%s'\n", type);
		exit(2);
	}
}

static obs_data_t *parse_settings(const struct script_line *sl, int from)
{
	obs_data_t *d = obs_data_create();
	for (int i = from; i < sl->argc; i++)
		apply_setting(d, sl->argv[i]);
	return d;
}

static const char *ptype(enum obs_property_type t)
{
	static const char *n[] = {"invalid", "bool", "int", "float", "text", "path", "list", "color",
				  "button", "font", "editable_list", "frame_rate", "group", "color_alpha"};
	return (int)t < 14 ? n[t] : "?";
}

static void dump_props(obs_properties_t *props, int indent)
{
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		enum obs_property_type t = obs_property_get_type(p);
		printf("%*sprop %s type=%s desc=\"%s\" visible=%d enabled=%d", indent, "", obs_property_name(p),
		       ptype(t), obs_property_description(p), (int)obs_property_visible(p),
		       (int)obs_property_enabled(p));
		const char *ld = obs_property_long_description(p);
		if (ld)
			printf(" long=\"%s\"", ld);
		if (t == OBS_PROPERTY_INT)
			printf(" min=%d max=%d step=%d numtype=%d suffix=\"%s\"", obs_property_int_min(p),
			       obs_property_int_max(p), obs_property_int_step(p), (int)obs_property_int_type(p),
			       obs_property_int_suffix(p) ? obs_property_int_suffix(p) : "");
		if (t == OBS_PROPERTY_FLOAT)
			printf(" min=%.9g max=%.9g step=%.9g numtype=%d", obs_property_float_min(p),
			       obs_property_float_max(p), obs_property_float_step(p), (int)obs_property_float_type(p));
		if (t == OBS_PROPERTY_PATH)
			printf(" pathtype=%d filter=\"%s\"", (int)obs_property_path_type(p),
			       obs_property_path_filter(p) ? obs_property_path_filter(p) : "");
		if (t == OBS_PROPERTY_GROUP)
			printf(" grouptype=%d", (int)obs_property_group_type(p));
		printf("\n");
		if (t == OBS_PROPERTY_LIST) {
			enum obs_combo_format fmt = obs_property_list_format(p);
			printf("%*s  list type=%d format=%d count=%zu\n", indent, "", (int)obs_property_list_type(p),
			       (int)fmt, obs_property_list_item_count(p));
			for (size_t i = 0; i < obs_property_list_item_count(p); i++) {
				printf("%*s  item %zu name=\"%s\" ", indent, "", i, obs_property_list_item_name(p, i));
				if (fmt == OBS_COMBO_FORMAT_INT)
					printf("value=%lld\n", obs_property_list_item_int(p, i));
				else
					printf("value=\"%s\"\n", obs_property_list_item_string(p, i));
			}
		}
		if (t == OBS_PROPERTY_GROUP)
			dump_props(obs_property_group_content(p), indent + 4);
	}
}

static obs_property_t *find_prop(obs_properties_t *props, const char *name)
{
	return obs_properties_get(props, name);
}

static obs_properties_t *cur_props;

static void print_fields(const char *id, void *data)
{
	if (!data) {
		printf("  fields (no instance)\n");
		return;
	}
	if (!strcmp(id, "fade_to_color_transition")) {
		struct fade_to_color_info *s = data;
		printf("  fields color=%.9g,%.9g,%.9g,%.9g srgb=%.9g,%.9g,%.9g,%.9g switch_point=%.9g\n",
		       (double)s->color.x, (double)s->color.y, (double)s->color.z, (double)s->color.w,
		       (double)s->color_srgb.x, (double)s->color_srgb.y, (double)s->color_srgb.z,
		       (double)s->color_srgb.w, (double)s->switch_point);
	} else if (!strcmp(id, "slide_transition")) {
		struct slide_info *s = data;
		printf("  fields dir=%.9g,%.9g\n", (double)s->dir.x, (double)s->dir.y);
	} else if (!strcmp(id, "swipe_transition")) {
		struct swipe_info *s = data;
		printf("  fields dir=%.9g,%.9g swipe_in=%d\n", (double)s->dir.x, (double)s->dir.y, (int)s->swipe_in);
	} else if (!strcmp(id, "wipe_transition")) {
		struct luma_wipe_info *s = data;
		printf("  fields invert=%d softness=%.9g luma_loaded=%d luma_size=%ux%u luma_tex=%d wipes_list=%d\n",
		       (int)s->invert_luma, (double)s->softness, (int)s->luma_image.loaded, s->luma_image.cx,
		       s->luma_image.cy, s->luma_image.texture ? 1 : 0, s->wipes_list ? 1 : 0);
	} else if (!strcmp(id, "obs_stinger_transition")) {
		struct stinger_info *s = data;
		printf("  fields tp_is_frame=%d tp_ns=%llu tp_frame=%llu tp=%s a_mul=%s b_mul=%s\n",
		       (int)s->transition_point_is_frame, (unsigned long long)s->transition_point_ns,
		       (unsigned long long)s->transition_point_frame, fk_f9(s->transition_point),
		       fk_f9(s->transition_a_mul), fk_f9(s->transition_b_mul));
		printf("  fields duration_ns=%llu duration_frames=%llu transitioning=%d monitoring=%d fade_style=%d\n",
		       (unsigned long long)s->duration_ns, (unsigned long long)s->duration_frames,
		       (int)s->transitioning, (int)s->monitoring, (int)s->fade_style);
		printf("  fields matte_enabled=%d layout=%d wf=%.9g hf=%.9g invert=%d texrender=%d rendered=%d media=%d matte_src=%d mix_fn=%s\n",
		       (int)s->track_matte_enabled, s->matte_layout, (double)s->matte_width_factor,
		       (double)s->matte_height_factor, (int)s->invert_matte, (int)s->do_texrender,
		       (int)s->matte_rendered, s->media_source ? 1 : 0, s->matte_source ? 1 : 0,
		       s->mix_a == mix_a_cross_fade ? "cross" : "fadeinout");
	} else {
		printf("  fields (none)\n");
	}
}

static void run(struct script_line *sl);

static obs_source_t *need_src(const struct script_line *sl, int idx)
{
	script_need(sl, idx + 1);
	obs_source_t *s = fk_find(sl->argv[idx]);
	if (!s) {
		fprintf(stderr, "script: line %d: no transition '%s'\n", sl->lineno, sl->argv[idx]);
		exit(2);
	}
	return s;
}

static void run(struct script_line *sl)
{
	const char *c = sl->argv[0];

	if (fk_command(sl))
		return;

	if (!strcmp(c, "reset")) {
		fk_reset();
	} else if (!strcmp(c, "load")) {
		bool ok = obs_module_load();
		if (sl->argc > 1) /* "load quiet" */
			return;
		printf("load %d registered=%d\n", (int)ok, fk_registry_count());
		for (int i = 0; i < fk_registry_count(); i++) {
			const struct obs_source_info *in = fk_registry_at(i);
			printf("  id=%s type=%d flags=0x%x version=%u name=\"%s\"\n", in->id, (int)in->type,
			       (unsigned)in->output_flags, in->version, in->get_name(NULL));
			printf("    callbacks:%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n", in->create ? " create" : "",
			       in->destroy ? " destroy" : "", in->update ? " update" : "",
			       in->get_defaults ? " get_defaults" : "", in->get_properties ? " get_properties" : "",
			       in->video_render ? " video_render" : "", in->video_tick ? " video_tick" : "",
			       in->audio_render ? " audio_render" : "", in->video_get_color_space ? " color_space" : "",
			       in->transition_start ? " transition_start" : "",
			       in->transition_stop ? " transition_stop" : "",
			       in->enum_active_sources ? " enum_active" : "",
			       in->enum_all_sources ? " enum_all" : "", in->missing_files ? " missing_files" : "",
			       in->get_width ? " get_width" : "", in->get_height ? " get_height" : "");
		}
		printf("  description=\"%s\"\n", obs_module_description());
		printf("  ver_matches=%d\n", (int)(obs_module_ver() == LIBOBS_API_VER));
	} else if (!strcmp(c, "create")) { /* create NAME ID [type:key=val...] */
		script_need(sl, 3);
		const struct obs_source_info *info = fk_registry_find(sl->argv[2]);
		if (!info) {
			printf("create %s: unknown id\n", sl->argv[2]);
			return;
		}
		obs_data_t *d = parse_settings(sl, 3);
		printf("create %s %s\n", sl->argv[1], sl->argv[2]);
		obs_source_t *s = fk_create(info, sl->argv[1], d);
		obs_data_release(d);
		printf("  instance=%d\n", fk_data(s) ? 1 : 0);
		/* swipe_create() uses bmalloc: dir/swipe_in are uninitialised until the first update */
		if (!strcmp(sl->argv[2], "swipe_transition"))
			printf("  fields (uninitialised until the first update)\n");
		else
			print_fields(sl->argv[2], fk_data(s));
	} else if (!strcmp(c, "settings")) { /* settings NAME type:key=val... : obs_source_update */
		obs_source_t *s = need_src(sl, 1);
		obs_data_t *d = parse_settings(sl, 2);
		printf("settings %s\n", sl->argv[1]);
		obs_source_update(s, d);
		obs_data_release(d);
	} else if (!strcmp(c, "tick")) {
		obs_source_t *s = need_src(sl, 1);
		printf("tick %s\n", sl->argv[1]);
		fk_tick(s, sl->argc > 2 ? (float)atof(sl->argv[2]) : 0.0f);
		print_fields(fk_info(s)->id, fk_data(s));
	} else if (!strcmp(c, "fields")) {
		obs_source_t *s = need_src(sl, 1);
		print_fields(fk_info(s)->id, fk_data(s));
		fk_print_sources();
	} else if (!strcmp(c, "destroy")) {
		obs_source_t *s = need_src(sl, 1);
		printf("destroy %s\n", sl->argv[1]);
		fk_destroy(s);
	} else if (!strcmp(c, "props")) { /* props NAME [null] : get_properties(data or NULL) */
		obs_source_t *s = need_src(sl, 1);
		bool nodata = sl->argc > 2 && !strcmp(sl->argv[2], "null");
		printf("props %s%s\n", sl->argv[1], nodata ? " (data=NULL)" : "");
		if (cur_props)
			obs_properties_destroy(cur_props);
		cur_props = fk_info(s)->get_properties(nodata ? NULL : fk_data(s));
		printf("  flags=%u\n", obs_properties_get_flags(cur_props));
		dump_props(cur_props, 2);
	} else if (!strcmp(c, "modify")) { /* modify NAME PROP : run the modified callback with the current settings */
		obs_source_t *s = need_src(sl, 1);
		script_need(sl, 3);
		obs_property_t *p = cur_props ? find_prop(cur_props, sl->argv[2]) : NULL;
		if (!p) {
			printf("modify %s: no such property\n", sl->argv[2]);
			return;
		}
		bool r = obs_property_modified(p, fk_settings(s));
		printf("modify %s -> %d\n", sl->argv[2], (int)r);
		dump_props(cur_props, 2);
	} else if (!strcmp(c, "render")) { /* render NAME : info.video_render(data, NULL) */
		obs_source_t *s = need_src(sl, 1);
		printf("render %s\n", sl->argv[1]);
		fk_info(s)->video_render(fk_data(s), NULL);
	} else if (!strcmp(c, "mix")) { /* mix NAME MIXERS CHANNELS RATE t... */
		obs_source_t *s = need_src(sl, 1);
		script_need(sl, 5);
		static struct obs_source_audio_mix audio;
		static float bufs[MAX_AUDIO_MIXES][MAX_AUDIO_CHANNELS][AUDIO_OUTPUT_FRAMES];
		uint32_t mixers = (uint32_t)strtoul(sl->argv[2], NULL, 0);
		size_t channels = (size_t)atoi(sl->argv[3]);
		fk_mix_n = 0;
		for (int i = 5; i < sl->argc && fk_mix_n < 16; i++)
			fk_mix_t[fk_mix_n++] = atof(sl->argv[i]);
		for (int m = 0; m < MAX_AUDIO_MIXES; m++)
			for (int ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
				for (int i = 0; i < AUDIO_OUTPUT_FRAMES; i++)
					bufs[m][ch][i] = 7.0f;
				audio.output[m].data[ch] = bufs[m][ch];
			}
		uint64_t ts_out = 0;
		bool r = fk_info(s)->audio_render(fk_data(s), &ts_out, &audio, mixers, channels,
						   (size_t)atoi(sl->argv[4]));
		printf("  result=%d ts_out=%llu\n", (int)r, (unsigned long long)ts_out);
		for (int m = 0; m < MAX_AUDIO_MIXES; m++)
			for (size_t ch = 0; ch < 2; ch++)
				printf("  out mix%d ch%zu first=%.9g last=%.9g\n", m, ch, (double)bufs[m][ch][0],
				       (double)bufs[m][ch][AUDIO_OUTPUT_FRAMES - 1]);
		fk_mix_n = 0;
	} else if (!strcmp(c, "start") || !strcmp(c, "stop")) {
		obs_source_t *s = need_src(sl, 1);
		printf("%s %s\n", c, sl->argv[1]);
		if (c[2] == 'a')
			fk_info(s)->transition_start(fk_data(s));
		else
			fk_info(s)->transition_stop(fk_data(s));
		print_fields(fk_info(s)->id, fk_data(s));
		fk_print_sources();
	} else if (!strcmp(c, "cspace")) { /* cspace NAME TRANSITION_SPACE COUNT p... */
		obs_source_t *s = need_src(sl, 1);
		script_need(sl, 4);
		static const char *names[] = {"srgb", "srgb_16f", "709_extended", "709_scrgb"};
		char b[32];
		snprintf(b, sizeof b, "tr_space=%s", sl->argv[2]);
		struct script_line g = {.argc = 2};
		g.argv[0] = "gfx";
		g.argv[1] = b;
		fk_command(&g);
		enum gs_color_space pref[8];
		size_t n = (size_t)atoi(sl->argv[3]);
		for (size_t i = 0; i < n && i < 8; i++)
			pref[i] = (enum gs_color_space)atoi(sl->argv[4 + i]);
		enum gs_color_space r = fk_info(s)->video_get_color_space(fk_data(s), n, pref);
		printf("cspace %s count=%zu -> %d\n", sl->argv[2], n, (int)r);
		(void)names;
	} else if (!strcmp(c, "missing")) { /* missing NAME : stinger missing_files */
		obs_source_t *s = need_src(sl, 1);
		obs_missing_files_t *f = fk_info(s)->missing_files(fk_data(s));
		size_t n = obs_missing_files_count(f);
		printf("missing_files count=%zu\n", n);
		for (size_t i = 0; i < n; i++) {
			obs_missing_file_t *mf = obs_missing_files_get_file(f, (int)i);
			printf("  file %zu path=\"%s\"\n", i, obs_missing_file_get_path(mf));
		}
		for (int i = 2; i + 1 < sl->argc; i += 2) { /* index new_path : issue the callback */
			obs_missing_file_t *mf = obs_missing_files_get_file(f, atoi(sl->argv[i]));
			obs_missing_file_issue_callback(mf, sl->argv[i + 1]);
			obs_data_t *st = fk_settings(s);
			printf("  fixed %s path=\"%s\" track_matte_path=\"%s\"\n", sl->argv[i],
			       obs_data_get_string(st, "path"), obs_data_get_string(st, "track_matte_path"));
		}
		obs_missing_files_destroy(f);
	} else if (!strcmp(c, "ease")) { /* ease t... : cubic_ease_in_out */
		for (int i = 1; i < sl->argc; i++) {
			float t = (float)atof(sl->argv[i]);
			printf("ease t=%.9g -> %.9g\n", (double)t, (double)cubic_ease_in_out(t));
		}
	} else if (!strcmp(c, "smoothstep")) { /* smoothstep MIN MAX X (fade-to-color helper) */
		script_need(sl, 4);
		printf("smoothstep -> %s\n", fk_f9(smoothstep((float)atof(sl->argv[1]), (float)atof(sl->argv[2]),
								 (float)atof(sl->argv[3]))));
	} else if (!strcmp(c, "assets")) { /* assets : luma image set, wipes.json cross-check, effect file hashes */
		const char *dir = NULL;
		char *wj = obs_module_file("luma_wipes/wipes.json");
		(void)dir;
		char pathbuf[1024];
		snprintf(pathbuf, sizeof pathbuf, "%s", wj ? wj : "");
		bfree(wj);
		char *slash = strrchr(pathbuf, '/');
		if (slash)
			*slash = 0;
		DIR *d = opendir(pathbuf);
		char *names[128];
		int n = 0;
		struct dirent *e;
		while (d && (e = readdir(d)))
			if (strstr(e->d_name, ".png") && n < 128)
				names[n++] = bstrdup(e->d_name);
		if (d)
			closedir(d);
		for (int i = 0; i < n; i++)
			for (int j = i + 1; j < n; j++)
				if (strcmp(names[i], names[j]) > 0) {
					char *t = names[i];
					names[i] = names[j];
					names[j] = t;
				}
		printf("png files=%d\n", n);
		for (int i = 0; i < n; i++) {
			char full[1100];
			uint32_t w, h;
			int depth, ct;
			uint64_t fnv;
			size_t len;
			snprintf(full, sizeof full, "%s/%s", pathbuf, names[i]);
			bool ok = fk_png_info(full, &w, &h, &depth, &ct, &fnv, &len);
			printf("  %s ok=%d size=%ux%u depth=%d ctype=%d len=%zu fnv=%016llx\n", names[i], (int)ok, w, h,
			       depth, ct, len, (unsigned long long)fnv);
			bfree(names[i]);
		}
	} else if (!strcmp(c, "effects")) { /* effects : size + fnv of each .effect asset */
		static const char *files[] = {"fade_to_color_transition.effect", "fade_transition.effect",
					      "luma_wipe_transition.effect", "slide_transition.effect",
					      "stinger_matte_transition.effect", "swipe_transition.effect"};
		for (int i = 0; i < 6; i++) {
			char *p = obs_module_file(files[i]);
			char *t = p ? os_quick_read_utf8_file(p) : NULL;
			uint64_t h = 0xcbf29ce484222325ULL;
			for (const unsigned char *q = (const unsigned char *)(t ? t : ""); *q; q++) {
				h ^= *q;
				h *= 1099511628211ULL;
			}
			printf("effect %s len=%zu fnv=%016llx\n", files[i], t ? strlen(t) : 0, (unsigned long long)h);
			bfree(t);
			bfree(p);
		}
	} else if (!strcmp(c, "echo")) {
		printf("echo");
		for (int i = 1; i < sl->argc; i++)
			printf(" %s", sl->argv[i]);
		printf("\n");
	} else {
		fprintf(stderr, "script: line %d: unknown command '%s'\n", sl->lineno, c);
		exit(2);
	}
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	fk_reset();
	while (script_next(f, &sl, &lineno)) {
		if (!strcmp(sl.argv[0], "fork")) { /* fork CMD... : run in a child, report a crash */
			for (int i = 1; i < sl.argc; i++)
				sl.argv[i - 1] = sl.argv[i];
			sl.argc--;
			printf("fork %s\n", sl.argv[0]);
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				run(&sl);
				fflush(stdout);
				_exit(0);
			}
			int st = 0;
			waitpid(pid, &st, 0);
			if (!WIFEXITED(st) || WEXITSTATUS(st) != 0)
				printf("crash\n");
			continue;
		}
		run(&sl);
	}
	return 0;
}
