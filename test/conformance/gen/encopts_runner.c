/*
 * Conformance runner for encoder option logic (spec S-025). See the section
 * "encopts runner" in test/conformance/README.md for the command list.
 *
 * The runner drives the REAL settings/property/option code of the encoder plugins
 * (obs-x264, obs-nvenc, obs-qsv11, obs-libfdk, obs-ffmpeg video+audio encoders and
 * the shared option-string parser) against the real obs-data.c and the real
 * obs-properties.c. Everything the plugins call outside of that (libx264, libavcodec,
 * QuickSync, the host application) is a recording fake in encopts_stubs.c.
 */
#include "obs.h"
#include "obs-properties.h"
#include "util/bmem.h"
#include "opts-parser.h"
#include "harness.h"
#include "encopts_api.h"

#define MAXN 64

struct named_data {
	char name[32];
	obs_data_t *d;
	bool used;
};
struct named_props {
	char name[32];
	obs_properties_t *p;
	bool used;
};
struct named_ptr {
	char name[32];
	void *p;
	int kind;
	bool used;
};

static struct named_data datas[MAXN];
static struct named_props propss[MAXN];
static struct named_ptr ptrs[MAXN];

static void die(const char *m, const char *a)
{
	fprintf(stderr, "script: %s %s\n", m, a ? a : "");
	exit(2);
}

static struct named_data *dat_find(const char *n)
{
	for (int i = 0; i < MAXN; i++)
		if (datas[i].used && !strcmp(datas[i].name, n))
			return &datas[i];
	die("unknown data", n);
	return NULL;
}
static obs_data_t *dat(const char *n)
{
	return dat_find(n)->d;
}
static struct named_props *pr_find(const char *n)
{
	for (int i = 0; i < MAXN; i++)
		if (propss[i].used && !strcmp(propss[i].name, n))
			return &propss[i];
	die("unknown props", n);
	return NULL;
}
static struct named_ptr *ptr_find(const char *n)
{
	for (int i = 0; i < MAXN; i++)
		if (ptrs[i].used && !strcmp(ptrs[i].name, n))
			return &ptrs[i];
	die("unknown handle", n);
	return NULL;
}

static void bind_data(const char *n, obs_data_t *d)
{
	for (int i = 0; i < MAXN; i++)
		if (datas[i].used && !strcmp(datas[i].name, n)) {
			obs_data_release(datas[i].d);
			datas[i].d = d;
			return;
		}
	for (int i = 0; i < MAXN; i++)
		if (!datas[i].used) {
			snprintf(datas[i].name, sizeof(datas[i].name), "%s", n);
			datas[i].d = d;
			datas[i].used = true;
			return;
		}
	die("too many data objects", n);
}
static void bind_props(const char *n, obs_properties_t *p)
{
	for (int i = 0; i < MAXN; i++)
		if (propss[i].used && !strcmp(propss[i].name, n)) {
			obs_properties_destroy(propss[i].p);
			propss[i].p = p;
			return;
		}
	for (int i = 0; i < MAXN; i++)
		if (!propss[i].used) {
			snprintf(propss[i].name, sizeof(propss[i].name), "%s", n);
			propss[i].p = p;
			propss[i].used = true;
			return;
		}
	die("too many props", n);
}
static void bind_ptr(const char *n, void *p, int kind)
{
	for (int i = 0; i < MAXN; i++)
		if (!ptrs[i].used) {
			snprintf(ptrs[i].name, sizeof(ptrs[i].name), "%s", n);
			ptrs[i].p = p;
			ptrs[i].kind = kind;
			ptrs[i].used = true;
			return;
		}
	die("too many handles", n);
}

/* ------------------------------------------------------------------------- */
/* name tables */

static const struct {
	const char *n;
	int v;
} vfmt[] = {{"none", VIDEO_FORMAT_NONE},   {"I420", VIDEO_FORMAT_I420}, {"NV12", VIDEO_FORMAT_NV12},
	    {"I444", VIDEO_FORMAT_I444},   {"I010", VIDEO_FORMAT_I010}, {"P010", VIDEO_FORMAT_P010},
	    {"P216", VIDEO_FORMAT_P216},   {"P416", VIDEO_FORMAT_P416}, {"BGRA", VIDEO_FORMAT_BGRA},
	    {"I422", VIDEO_FORMAT_I422},   {"YUY2", VIDEO_FORMAT_YUY2}, {"RGBA", VIDEO_FORMAT_RGBA},
	    {"Y800", VIDEO_FORMAT_Y800},   {NULL, 0}};
static const struct {
	const char *n;
	int v;
} vcs[] = {{"default", VIDEO_CS_DEFAULT}, {"601", VIDEO_CS_601},     {"709", VIDEO_CS_709},
	   {"srgb", VIDEO_CS_SRGB},       {"pq", VIDEO_CS_2100_PQ}, {"hlg", VIDEO_CS_2100_HLG},
	   {NULL, 0}};

static int lookup(const char *what, const char *n, const void *tab_)
{
	const struct {
		const char *n;
		int v;
	} *tab = tab_;
	for (; tab->n; tab++)
		if (!strcmp(tab->n, n))
			return tab->v;
	die(what, n);
	return 0;
}
static const char *rev(const void *tab_, int v)
{
	const struct {
		const char *n;
		int v;
	} *tab = tab_;
	for (; tab->n; tab++)
		if (tab->v == v)
			return tab->n;
	return "?";
}
#define FMT(n) lookup("video format", n, vfmt)

static const char *ptype_name(enum obs_property_type t)
{
	static const char *const names[] = {"invalid", "bool",  "int",  "float",      "text",   "path",
					    "list",    "color", "button", "font",    "editable_list", "frame_rate",
					    "group",   "color_alpha"};
	return (unsigned)t < sizeof(names) / sizeof(*names) ? names[t] : "?";
}

static void print_q(const char *s)
{
	if (!s)
		fputs("@null", stdout);
	else
		script_print_quoted(s);
}

/* ------------------------------------------------------------------------- */
/* dumps */

static void dump_props(const char *label, obs_properties_t *props, bool full)
{
	if (!full) {
		printf("vis %s", label);
		for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p))
			printf(" %s:%d%d", obs_property_name(p), obs_property_visible(p) ? 1 : 0,
			       obs_property_enabled(p) ? 1 : 0);
		putchar('\n');
		return;
	}
	printf("props %s\n", label);
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		enum obs_property_type t = obs_property_get_type(p);
		printf("  %s %s desc=", obs_property_name(p), ptype_name(t));
		print_q(obs_property_description(p));
		printf(" visible=%d enabled=%d", obs_property_visible(p) ? 1 : 0, obs_property_enabled(p) ? 1 : 0);
		if (obs_property_long_description(p)) {
			printf(" long=");
			print_q(obs_property_long_description(p));
		}
		if (t == OBS_PROPERTY_INT) {
			printf(" min=%d max=%d step=%d numtype=%d", obs_property_int_min(p), obs_property_int_max(p),
			       obs_property_int_step(p), (int)obs_property_int_type(p));
			if (obs_property_int_suffix(p)) {
				printf(" suffix=");
				print_q(obs_property_int_suffix(p));
			}
		} else if (t == OBS_PROPERTY_TEXT) {
			printf(" texttype=%d", (int)obs_property_text_type(p));
		} else if (t == OBS_PROPERTY_LIST) {
			enum obs_combo_format f = obs_property_list_format(p);
			printf(" combo=%d format=%d count=%d", (int)obs_property_list_type(p), (int)f,
			       (int)obs_property_list_item_count(p));
			putchar('\n');
			for (size_t i = 0; i < obs_property_list_item_count(p); i++) {
				printf("    item %d ", (int)i);
				print_q(obs_property_list_item_name(p, i));
				printf(" = ");
				if (f == OBS_COMBO_FORMAT_STRING)
					print_q(obs_property_list_item_string(p, i));
				else if (f == OBS_COMBO_FORMAT_INT)
					printf("%lld", obs_property_list_item_int(p, i));
				else if (f == OBS_COMBO_FORMAT_FLOAT)
					printf("%.17g", obs_property_list_item_float(p, i));
				else
					printf("%d", obs_property_list_item_bool(p, i) ? 1 : 0);
				if (obs_property_list_item_disabled(p, i))
					printf(" disabled");
				if (i + 1 < obs_property_list_item_count(p))
					putchar('\n');
			}
		}
		putchar('\n');
	}
	printf("end props %s\n", label);
}

static void dump_data(const char *label, obs_data_t *d)
{
	printf("data %s\n", label);
	for (obs_data_item_t *it = obs_data_first(d); it; obs_data_item_next(&it)) {
		const char *src = obs_data_item_has_user_value(it) ? "user" : "default";
		printf("  %s ", obs_data_item_get_name(it));
		switch (obs_data_item_gettype(it)) {
		case OBS_DATA_STRING:
			printf("string ");
			print_q(obs_data_item_get_string(it));
			break;
		case OBS_DATA_NUMBER:
			if (obs_data_item_numtype(it) == OBS_DATA_NUM_INT)
				printf("int %lld", obs_data_item_get_int(it));
			else
				printf("double %.17g", obs_data_item_get_double(it));
			break;
		case OBS_DATA_BOOLEAN:
			printf("bool %d", obs_data_item_get_bool(it) ? 1 : 0);
			break;
		default:
			printf("other");
		}
		printf(" %s\n", src);
	}
	printf("end data %s\n", label);
}

static void print_options(const char *label, const struct obs_options *o)
{
	printf("opts %s count=%d ignored=%d\n", label, (int)o->count, (int)o->ignored_word_count);
	for (size_t i = 0; i < o->count; i++) {
		printf("  opt %d ", (int)i);
		print_q(o->options[i].name);
		putchar(' ');
		print_q(o->options[i].value);
		putchar('\n');
	}
	for (size_t i = 0; i < o->ignored_word_count; i++) {
		printf("  ignored %d ", (int)i);
		print_q(o->ignored_words[i]);
		putchar('\n');
	}
}

/* ------------------------------------------------------------------------- */

static int codec_arg(const char *s)
{
	if (!strcmp(s, "h264"))
		return 0;
	if (!strcmp(s, "hevc"))
		return 1;
	if (!strcmp(s, "av1"))
		return 2;
	die("unknown codec", s);
	return 0;
}
static int qsv_kind(const char *s)
{
	if (!strcmp(s, "h264v1"))
		return 0;
	if (!strcmp(s, "h264v2"))
		return 1;
	if (!strcmp(s, "av1"))
		return 2;
	if (!strcmp(s, "hevc"))
		return 3;
	die("unknown qsv kind", s);
	return 0;
}

#define IS(c) (!strcmp(a[0], c))

static void run(struct script_line *sl)
{
	char **a = sl->argv;
	int n = sl->argc;

	/* ---- generic ---- */
	if (IS("echo")) {
		printf("echo");
		for (int i = 1; i < n; i++)
			printf(" %s", a[i]);
		putchar('\n');
	} else if (IS("video")) {
		script_need(sl, 8);
		g_host.format = FMT(a[1]);
		g_host.colorspace = lookup("colorspace", a[2], vcs);
		g_host.range = !strcmp(a[3], "full") ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
		g_host.fps_num = (uint32_t)atoi(a[4]);
		g_host.fps_den = (uint32_t)atoi(a[5]);
		g_host.width = (uint32_t)atoi(a[6]);
		g_host.height = (uint32_t)atoi(a[7]);
	} else if (IS("pref_format")) {
		script_need(sl, 2);
		g_host.pref_format = FMT(a[1]);
	} else if (IS("cpu_platform")) {
		script_need(sl, 2);
		g_host.cpu_platform = atoi(a[1]);
	} else if (IS("hdr_peak")) {
		script_need(sl, 2);
		g_host.hdr_peak = atoi(a[1]);
	} else if (IS("tex_active")) {
		script_need(sl, 3);
		if (!strcmp(a[1], "nv12"))
			g_host.tex_nv12 = atoi(a[2]);
		else
			g_host.tex_p010 = atoi(a[2]);
	} else if (IS("audio")) {
		script_need(sl, 4);
		g_host.channels = atoi(a[1]);
		g_host.speakers = atoi(a[2]);
		g_host.sample_rate = (uint32_t)atoi(a[3]);
	}
	/* ---- settings objects ---- */
	else if (IS("data")) {
		script_need(sl, 2);
		bind_data(a[1], obs_data_create());
	} else if (IS("release")) {
		script_need(sl, 2);
		struct named_data *d = dat_find(a[1]);
		obs_data_release(d->d);
		d->used = false;
	} else if (IS("set")) {
		script_need(sl, 5);
		obs_data_t *d = dat(a[1]);
		if (!strcmp(a[3], "int"))
			obs_data_set_int(d, a[2], strtoll(a[4], NULL, 0));
		else if (!strcmp(a[3], "str"))
			obs_data_set_string(d, a[2], a[4]);
		else if (!strcmp(a[3], "bool"))
			obs_data_set_bool(d, a[2], atoi(a[4]) != 0);
		else if (!strcmp(a[3], "dbl"))
			obs_data_set_double(d, a[2], atof(a[4]));
		else
			die("bad type", a[3]);
	} else if (IS("setdef")) {
		script_need(sl, 5);
		obs_data_t *d = dat(a[1]);
		if (!strcmp(a[3], "int"))
			obs_data_set_default_int(d, a[2], strtoll(a[4], NULL, 0));
		else if (!strcmp(a[3], "str"))
			obs_data_set_default_string(d, a[2], a[4]);
		else if (!strcmp(a[3], "bool"))
			obs_data_set_default_bool(d, a[2], atoi(a[4]) != 0);
		else if (!strcmp(a[3], "dbl"))
			obs_data_set_default_double(d, a[2], atof(a[4]));
		else
			die("bad type", a[3]);
	} else if (IS("unset")) {
		script_need(sl, 3);
		obs_data_unset_user_value(dat(a[1]), a[2]);
	} else if (IS("dump")) {
		script_need(sl, 2);
		dump_data(a[1], dat(a[1]));
	}
	/* ---- properties ---- */
	else if (IS("props_dump") || IS("props_vis")) {
		script_need(sl, 2);
		dump_props(a[1], pr_find(a[1])->p, IS("props_dump"));
	} else if (IS("props_apply")) {
		script_need(sl, 3);
		obs_properties_apply_settings(pr_find(a[1])->p, dat(a[2]));
		printf("props_apply %s\n", a[1]);
	} else if (IS("props_modify")) {
		script_need(sl, 4);
		obs_property_t *p = obs_properties_get(pr_find(a[1])->p, a[2]);
		if (!p)
			die("no such property", a[2]);
		bool r = obs_property_modified(p, dat(a[3]));
		printf("props_modify %s %s -> %d\n", a[1], a[2], r ? 1 : 0);
	} else if (IS("props_free")) {
		script_need(sl, 2);
		struct named_props *p = pr_find(a[1]);
		obs_properties_destroy(p->p);
		p->used = false;
	}
	/* ---- shared option-string parser ---- */
	else if (IS("opts")) {
		script_need(sl, 2);
		struct obs_options o = obs_parse_options(!strcmp(a[1], "@null") ? NULL : a[1]);
		print_options("parse", &o);
		obs_free_options(o);
	}
	/* ---- x264 ---- */
	else if (IS("x264_defaults")) {
		script_need(sl, 2);
		x264w_defaults(dat(a[1]));
	} else if (IS("x264_props")) {
		script_need(sl, 2);
		bind_props(a[1], x264w_props());
	} else if (IS("x264_settings")) {
		script_need(sl, 2);
		bool ok = x264w_settings_only(dat(a[1]), n > 2 && !strcmp(a[2], "update"));
		printf("x264_settings %s\n", ok ? "ok" : "fail");
		x264w_print_params();
	} else if (IS("x264_create")) {
		script_need(sl, 3);
		void *h = x264w_create(dat(a[2]));
		printf("x264_create %s %s\n", a[1], h ? "ok" : "null");
		if (h) {
			x264w_print_handle_params(h);
			bind_ptr(a[1], h, 1);
		}
	} else if (IS("x264_update")) {
		script_need(sl, 3);
		void *h = ptr_find(a[1])->p;
		bool ok = x264w_update(h, dat(a[2]));
		printf("x264_update %s %d\n", a[1], ok ? 1 : 0);
		x264w_print_handle_params(h);
	} else if (IS("x264_destroy")) {
		script_need(sl, 2);
		struct named_ptr *p = ptr_find(a[1]);
		x264w_destroy(p->p);
		p->used = false;
	} else if (IS("x264_video_info")) {
		script_need(sl, 2);
		struct video_scale_info info = {0};
		info.format = FMT(a[1]);
		x264w_video_info(NULL, &info);
		printf("x264_video_info %s -> %s\n", a[1], rev(vfmt, info.format));
	} else if (IS("x264_fail")) {
		script_need(sl, 2);
		if (!strcmp(a[1], "preset"))
			g_host.x264_fail_preset = n > 2 ? atoi(a[2]) : 1;
		else if (!strcmp(a[1], "profile"))
			g_host.x264_fail_profile = n > 2 ? atoi(a[2]) : 1;
		else if (!strcmp(a[1], "open"))
			g_host.x264_fail_open = n > 2 ? atoi(a[2]) : 1;
		else if (!strcmp(a[1], "param") && n > 2 && g_host.x264_fail_param_n < 8)
			snprintf(g_host.x264_fail_param[g_host.x264_fail_param_n++], 32, "%s", a[2]);
		else if (!strcmp(a[1], "clear"))
			g_host.x264_fail_preset = g_host.x264_fail_profile = g_host.x264_fail_open =
				g_host.x264_fail_param_n = 0;
		else
			die("bad x264_fail", a[1]);
	}
	/* ---- nvenc ---- */
	else if (IS("nv_caps")) {
		script_need(sl, 8);
		nvw_set_caps(atoi(a[1]), atoi(a[2]), atoi(a[3]), atoi(a[4]), atoi(a[5]), atoi(a[6]), atoi(a[7]));
	} else if (IS("nv_devices")) {
		script_need(sl, 2);
		g_host.nv_devices = atoi(a[1]);
	} else if (IS("nv_av1_support")) {
		script_need(sl, 2);
		g_host.nv_av1_supported = atoi(a[1]);
	} else if (IS("nv_split_broken")) {
		script_need(sl, 2);
		g_host.nv_split_broken = atoi(a[1]);
	} else if (IS("nv_defaults")) {
		script_need(sl, 3);
		nvw_defaults(codec_arg(a[1]), dat(a[2]));
	} else if (IS("nv_props")) {
		script_need(sl, 3);
		bind_props(a[2], nvw_props(codec_arg(a[1])));
	} else if (IS("nv_read")) {
		script_need(sl, 2);
		nvw_read(dat(a[1]));
	} else if (IS("nv_apply")) {
		script_need(sl, 3);
		nvw_apply(codec_arg(a[1]), dat(a[2]));
	} else if (IS("nv_userarg")) {
		script_need(sl, 3);
		nvw_userarg(dat(a[1]), a[2]);
	}
	/* ---- qsv ---- */
	else if (IS("qsv_defaults")) {
		script_need(sl, 3);
		qsvw_defaults(qsv_kind(a[1]), dat(a[2]));
	} else if (IS("qsv_props")) {
		script_need(sl, 3);
		bind_props(a[2], qsvw_props(qsv_kind(a[1])));
	} else if (IS("qsv_migrate")) {
		script_need(sl, 2);
		qsvw_migrate(dat(a[1]));
	} else if (IS("qsv_create")) {
		script_need(sl, 4);
		void *h = qsvw_create(qsv_kind(a[1]), dat(a[3]));
		printf("qsv_create %s %s\n", a[2], h ? "ok" : "null");
		if (h) {
			qsvw_print_params(h);
			bind_ptr(a[2], h, 2);
		}
	} else if (IS("qsv_update")) {
		script_need(sl, 3);
		bool ok = qsvw_update(ptr_find(a[1])->p, dat(a[2]));
		printf("qsv_update %s %d\n", a[1], ok ? 1 : 0);
		qsvw_print_params(ptr_find(a[1])->p);
	} else if (IS("qsv_destroy")) {
		script_need(sl, 2);
		struct named_ptr *p = ptr_find(a[1]);
		qsvw_destroy(p->p);
		p->used = false;
	} else if (IS("qsv_dgpu")) {
		script_need(sl, 2);
		g_host.qsv_dgpu = atoi(a[1]);
	} else if (IS("qsv_video_info")) {
		script_need(sl, 3);
		struct video_scale_info info = {0};
		info.format = FMT(a[2]);
		qsvw_video_info(qsv_kind(a[1]), &info);
		printf("qsv_video_info %s %s -> %s %ux%u\n", a[1], a[2], rev(vfmt, info.format), info.width,
		       info.height);
	}
	/* ---- libfdk ---- */
	else if (IS("fdk_defaults")) {
		script_need(sl, 2);
		fdkw_defaults(dat(a[1]));
	} else if (IS("fdk_props")) {
		script_need(sl, 2);
		bind_props(a[1], fdkw_props());
	} else if (IS("fdk_create")) {
		script_need(sl, 2);
		bool ok = fdkw_create(dat(a[1]));
		printf("fdk_create %s\n", ok ? "ok" : "fail");
	}
	else if (IS("fdk_fail")) {
		script_need(sl, 2);
		fdkw_fail(atoi(a[1]));
	}
	/* ---- ffmpeg video ---- */
	else if (IS("vid_update")) {
		script_need(sl, 4);
		vidw_update(atoi(a[1]), atoi(a[2]), !strcmp(a[3], "@null") ? NULL : a[3]);
	}
	/* ---- ffmpeg audio ---- */
	else if (IS("aud_defaults")) {
		script_need(sl, 2);
		audw_defaults(dat(a[1]));
	} else if (IS("aud_props")) {
		script_need(sl, 2);
		bind_props(a[1], audw_props());
	} else if (IS("aud_codec")) {
		script_need(sl, 3);
		audw_codec(a[1], atoi(a[2]));
	} else if (IS("aud_lossless")) {
		script_need(sl, 3);
		audw_codec_lossless(a[1], atoi(a[2]));
	} else if (IS("aud_formats")) {
		script_need(sl, 2);
		audw_formats(a[1]);
	} else if (IS("aud_rates")) {
		script_need(sl, 2);
		audw_rates(a[1]);
	} else if (IS("aud_open_fail")) {
		script_need(sl, 2);
		g_host.ff_open_fail = atoi(a[1]);
	} else if (IS("aud_create")) {
		script_need(sl, 3);
		audw_create(a[1], dat(a[2]));
	} else {
		die("unknown command", a[0]);
	}
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	while (script_next(f, &sl, &lineno))
		run(&sl);
	fclose(f);
	return 0;
}
