/*
 * Conformance runner for the video filters of plugins/obs-filters (spec S-028). Command list: see the
 * "videofilters runner" section of test/conformance/README.md. Drives the real filter sources through their
 * obs_source_info callbacks; the OBS host and the graphics API are recording fakes (videofilters_stubs.c).
 */
#include <math.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include "obs.h"
#include "obs-properties.h"
#include "util/bmem.h"
#include "harness.h"
#include "videofilters_api.h"

extern struct obs_source_info color_filter, color_filter_v2, color_grade_filter, chroma_key_filter,
	chroma_key_filter_v2, color_key_filter, color_key_filter_v2, luma_key_filter, luma_key_filter_v2,
	sharpness_filter, sharpness_filter_v2, scroll_filter, crop_filter, scale_filter, mask_filter, mask_filter_v2,
	gpu_delay_filter, hdr_tonemap_filter, sdr_on_hdr_filter;

static struct obs_source_info *infos[] = {&color_filter, &color_filter_v2, &color_grade_filter, &chroma_key_filter,
	&chroma_key_filter_v2, &color_key_filter, &color_key_filter_v2, &luma_key_filter, &luma_key_filter_v2,
	&sharpness_filter, &sharpness_filter_v2, &scroll_filter, &crop_filter, &scale_filter, &mask_filter,
	&mask_filter_v2, &gpu_delay_filter, &hdr_tonemap_filter, &sdr_on_hdr_filter};

#define MAXN 600
static struct obs_source srcs[MAXN];
static int nsrc;
static struct { char name[24]; obs_data_t *d; } datas[MAXN];
static int ndata;

static void die(const char *m, const char *a)
{
	fprintf(stderr, "script: %s %s\n", m, a ? a : "");
	exit(2);
}
static void on_alarm(int s) { (void)s; _exit(124); }

static struct obs_source *src_find(const char *n)
{
	if (!strcmp(n, "@null")) return NULL;
	for (int i = 0; i < nsrc; i++) if (!strcmp(srcs[i].name, n)) return &srcs[i];
	die("unknown source", n);
	return NULL;
}
static obs_data_t *dat(const char *n)
{
	if (!strcmp(n, "@null")) return NULL;
	for (int i = 0; i < ndata; i++) if (!strcmp(datas[i].name, n)) return datas[i].d;
	die("unknown data", n);
	return NULL;
}
static const struct obs_source_info *info_find(const char *id, int ver)
{
	for (size_t i = 0; i < sizeof(infos) / sizeof(*infos); i++)
		if (!strcmp(infos[i]->id, id) && (int)infos[i]->version == ver) return infos[i];
	die("unknown filter", id);
	return NULL;
}

static const char *spn[] = {"SRGB", "SRGB_16F", "709_EXTENDED", "709_SCRGB"};
static enum gs_color_space space_arg(const char *s)
{
	for (int i = 0; i < 4; i++) if (!strcmp(s, spn[i])) return (enum gs_color_space)i;
	die("unknown space", s);
	return 0;
}
static uint32_t flags_arg(const char *s)
{
	uint32_t f = 0;
	if (strstr(s, "video")) f |= OBS_SOURCE_VIDEO;
	if (strstr(s, "async")) f |= OBS_SOURCE_ASYNC;
	if (strstr(s, "custom_draw")) f |= OBS_SOURCE_CUSTOM_DRAW;
	return f;
}
static enum gs_color_format cfmt_arg(const char *s)
{
	if (!strcmp(s, "RGBA")) return GS_RGBA;
	if (!strcmp(s, "RGBA16F")) return GS_RGBA16F;
	if (!strcmp(s, "BGRA")) return GS_BGRA;
	die("unknown format", s);
	return 0;
}

static const char *expand(const char *s, char *buf, size_t n)
{
	if (!strncmp(s, "@TMP/", 5)) { snprintf(buf, n, "%s/%s", g_vf.tmpdir, s + 5); return buf; }
	return s;
}
static void print_q(const char *s)
{
	size_t tl = strlen(g_vf.tmpdir);
	if (!s) { fputs("@null", stdout); return; }
	if (tl && !strncmp(s, g_vf.tmpdir, tl)) {
		char m[700];
		snprintf(m, sizeof(m), "@TMP%s", s + tl);
		script_print_quoted(m);
		return;
	}
	script_print_quoted(s);
}
static void set_value(obs_data_t *d, char **a)
{
	char b[600];
	if (!strcmp(a[2], "int")) obs_data_set_int(d, a[1], strtoll(a[3], NULL, 0));
	else if (!strcmp(a[2], "str")) obs_data_set_string(d, a[1], expand(a[3], b, sizeof(b)));
	else if (!strcmp(a[2], "bool")) obs_data_set_bool(d, a[1], atoi(a[3]) != 0);
	else if (!strcmp(a[2], "dbl")) obs_data_set_double(d, a[1], strtod(a[3], NULL));
	else die("bad type", a[2]);
}
static void dump_data(const char *label, obs_data_t *d)
{
	printf("data %s\n", label);
	for (obs_data_item_t *it = obs_data_first(d); it; obs_data_item_next(&it)) {
		printf("  %s ", obs_data_item_get_name(it));
		switch (obs_data_item_gettype(it)) {
		case OBS_DATA_STRING: printf("string "); print_q(obs_data_item_get_string(it)); break;
		case OBS_DATA_NUMBER:
			if (obs_data_item_numtype(it) == OBS_DATA_NUM_INT) printf("int %lld", obs_data_item_get_int(it));
			else printf("double %.17g", obs_data_item_get_double(it));
			break;
		case OBS_DATA_BOOLEAN: printf("bool %d", obs_data_item_get_bool(it) ? 1 : 0); break;
		default: printf("other");
		}
		printf(" %s\n", obs_data_item_has_user_value(it) ? "user" : "default");
	}
	printf("end data %s\n", label);
}

static void dump_props(const char *label, obs_properties_t *props, bool full)
{
	static const char *const tn[] = {"invalid", "bool", "int", "float", "text", "path", "list", "color", "button",
					 "font", "editable_list", "frame_rate", "group", "color_alpha"};
	if (!full) {
		printf("vis %s", label);
		for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p))
			printf(" %s:%d", obs_property_name(p), obs_property_visible(p) ? 1 : 0);
		putchar('\n');
		return;
	}
	printf("props %s\n", label);
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		enum obs_property_type t = obs_property_get_type(p);
		printf("  %s %s desc=", obs_property_name(p), tn[t]);
		print_q(obs_property_description(p));
		printf(" visible=%d enabled=%d", obs_property_visible(p) ? 1 : 0, obs_property_enabled(p) ? 1 : 0);
		if (t == OBS_PROPERTY_INT) {
			printf(" min=%d max=%d step=%d numtype=%d", obs_property_int_min(p), obs_property_int_max(p), obs_property_int_step(p), (int)obs_property_int_type(p));
			if (obs_property_int_suffix(p)) { printf(" suffix="); print_q(obs_property_int_suffix(p)); }
		} else if (t == OBS_PROPERTY_FLOAT) {
			printf(" min=%.9g max=%.9g step=%.9g numtype=%d", obs_property_float_min(p), obs_property_float_max(p), obs_property_float_step(p), (int)obs_property_float_type(p));
		} else if (t == OBS_PROPERTY_TEXT) {
			printf(" texttype=%d", (int)obs_property_text_type(p));
		} else if (t == OBS_PROPERTY_PATH) {
			printf(" pathtype=%d filter=", (int)obs_property_path_type(p));
			print_q(obs_property_path_filter(p));
			printf(" default=");
			print_q(obs_property_path_default_path(p));
		} else if (t == OBS_PROPERTY_LIST) {
			enum obs_combo_format f = obs_property_list_format(p);
			printf(" combo=%d format=%d count=%d", (int)obs_property_list_type(p), (int)f, (int)obs_property_list_item_count(p));
			for (size_t i = 0; i < obs_property_list_item_count(p); i++) {
				printf("\n    item %d ", (int)i);
				print_q(obs_property_list_item_name(p, i));
				printf(" = ");
				if (f == OBS_COMBO_FORMAT_STRING) print_q(obs_property_list_item_string(p, i));
				else printf("%lld", obs_property_list_item_int(p, i));
			}
		}
		putchar('\n');
	}
	printf("end props %s\n", label);
}

/* ---------------- shader models (the spec author's reference implementation of the shader pseudo-code) ---------- */
static float sat(float x) { return x < 0 ? 0 : (x > 1 ? 1 : x); }
static float smooth_(float a, float b, float x) { float t = sat((x - a) / (b - a)); return t * t * (3 - 2 * t); }
static float nl(float u) { return u <= 0.0031308f ? 12.92f * u : 1.055f * powf(u, 1.f / 2.4f) - 0.055f; }
static float lin(float u) { return u <= 0.04045f ? u / 12.92f : powf(u / 1.055f + 0.055f / 1.055f, 2.4f); }
static void unpremul(const float *in, float *o)
{
	float k = in[3] > 0.f ? 1.f / in[3] : 0.f;
	o[0] = in[0] * k; o[1] = in[1] * k; o[2] = in[2] * k; o[3] = in[3];
}
static void calc_color(float *c, float contrast, float brightness, float gamma)
{
	for (int i = 0; i < 3; i++) c[i] = powf(c[i], gamma) * contrast + brightness;
}
static float st2084(float x) { float c = powf(fabsf(x), 0.1593017578f); return powf((0.8359375f + 18.8515625f * c) / (1.f + 18.6875f * c), 78.84375f); }
static float st2084_inv(float u)
{
	float c = powf(fabsf(u), 1.f / 78.84375f);
	float n = c - 0.8359375f; if (n < 0) n = 0;
	return powf(fabsf(n / (18.8515625f - 18.6875f * c)), 1.f / 0.1593017578f);
}
static void r709_2020(float *v)
{
	float r = v[0]*0.62740389593469903f + v[1]*0.32928303837788370f + v[2]*0.043313065687417225f;
	float g = v[0]*0.069097289358232075f + v[1]*0.91954039507545871f + v[2]*0.011362315566309178f;
	float b = v[0]*0.016391438875150280f + v[1]*0.088013307877225749f + v[2]*0.89559525324762401f;
	v[0] = r; v[1] = g; v[2] = b;
}
static void r2020_709(float *v)
{
	float r = v[0]*1.6604910021084345f + v[1]*-0.58764113878854951f + v[2]*-0.072849863319884883f;
	float g = v[0]*-0.12455047452159074f + v[1]*1.1328998971259603f + v[2]*-0.0083494226043694768f;
	float b = v[0]*-0.018150763354905303f + v[1]*-0.10057889800800739f + v[2]*1.1187296613629127f;
	v[0] = r; v[1] = g; v[2] = b;
}
static float eetf(float m_pq, float Lw, float Lmax)
{
	float Lw_pq = st2084(Lw / 10000.f);
	float E1 = sat(m_pq / Lw_pq);
	float maxLum = st2084(Lmax / 10000.f) / Lw_pq;
	float KS = 1.5f * maxLum - 0.5f, E2 = E1;
	if (E1 > KS) {
		float T = (E1 - KS) / (1.f - KS), T2 = T * T, T3 = T2 * T;
		E2 = (2.f*T3 - 3.f*T2 + 1.f) * KS + (T3 - 2.f*T2 + T) * (1.f - KS) + (-2.f*T3 + 3.f*T2) * maxLum;
	}
	return E2 * Lw_pq;
}
static void maxrgb(float *v, float Lw, float Lmax)
{
	float m = fmaxf(fmaxf(v[0], v[1]), v[2]);
	float pq = st2084(m), m2 = st2084_inv(eetf(pq, Lw, Lmax));
	m = fmaxf(6.10352e-5f, m);
	for (int i = 0; i < 3; i++) v[i] *= m2 / m;
}
static void reinhard(float *v)
{
	for (int i = 0; i < 3; i++) { v[i] = sat(v[i] / (v[i] + 1.f)); v[i] = lin(powf(v[i], 1.f / 2.4f)); }
}
static void pr(const char *n, const float *o)
{
	printf("model %s -> %.9g %.9g %.9g %.9g\n", n, (double)o[0], (double)o[1], (double)o[2], (double)o[3]);
}
static float F(char **a, int i) { return strtof(a[i], NULL); }

static void model(char **a, int n)
{
	float px[4], u[4], o[4];
	const char *m = a[1];
	if (!strcmp(m, "chroma2") && n == 15) {
		/* texel R G B A (premultiplied), key cb cr, similarity smoothness spill, opacity contrast brightness gamma */
		for (int i = 0; i < 4; i++) px[i] = F(a, 2 + i);
		float kcb = F(a, 6), kcr = F(a, 7), sim = F(a, 8), smo = F(a, 9), sp = F(a, 10), op = F(a, 11), co = F(a, 12), br = F(a, 13), ga = F(a, 14);
		unpremul(px, u);
		float tap[3] = {nl(px[0]), nl(px[1]), nl(px[2])}, cen[3] = {nl(u[0]), nl(u[1]), nl(u[2])};
		float dt = hypotf(kcb - (-0.100644f*tap[0] - 0.338572f*tap[1] + 0.439216f*tap[2] + 0.501961f), kcr - (0.439216f*tap[0] - 0.398942f*tap[1] - 0.040274f*tap[2] + 0.501961f));
		float dc = hypotf(kcb - (-0.100644f*cen[0] - 0.338572f*cen[1] + 0.439216f*cen[2] + 0.501961f), kcr - (0.439216f*cen[0] - 0.398942f*cen[1] - 0.040274f*cen[2] + 0.501961f));
		float dist = (dt * 8.f + dc) / 9.f, base = dist - sim;
		float full = powf(sat(base / smo), 1.5f), spill = powf(sat(base / sp), 1.5f);
		u[3] *= op; u[3] *= full;
		float des = u[0]*0.2126f + u[1]*0.7152f + u[2]*0.0722f;
		for (int i = 0; i < 3; i++) u[i] = des + (u[i] - des) * spill;
		calc_color(u, co, br, ga);
		for (int i = 0; i < 3; i++) u[i] *= u[3];
		pr(m, u);
	} else if (!strcmp(m, "colorkey_v2") && n == 15) {
		for (int i = 0; i < 4; i++) px[i] = F(a, 2 + i);
		float k[3] = {F(a, 6), F(a, 7), F(a, 8)}, sim = F(a, 9), smo = F(a, 10), op = F(a, 11), co = F(a, 12), br = F(a, 13), ga = F(a, 14);
		unpremul(px, u);
		u[3] *= op;
		float d = hypotf(hypotf(k[0] - nl(u[0]), k[1] - nl(u[1])), k[2] - nl(u[2]));
		u[3] *= sat(fmaxf(d - sim, 0.f) / smo);
		calc_color(u, co, br, ga);
		for (int i = 0; i < 3; i++) u[i] *= u[3];
		pr(m, u);
	} else if (!strcmp(m, "colorkey_v1") && n == 19) {
		/* texel, key rgb, sim smooth, color rgba, contrast brightness gamma */
		for (int i = 0; i < 4; i++) px[i] = F(a, 2 + i);
		float k[3] = {F(a, 6), F(a, 7), F(a, 8)}, sim = F(a, 9), smo = F(a, 10), c[4] = {F(a, 11), F(a, 12), F(a, 13), F(a, 14)}, co = F(a, 16), br = F(a, 17), ga = F(a, 18);
		unpremul(px, u);
		for (int i = 0; i < 4; i++) u[i] *= c[i];
		float d = sqrtf((k[0]-u[0])*(k[0]-u[0]) + (k[1]-u[1])*(k[1]-u[1]) + (k[2]-u[2])*(k[2]-u[2]));
		u[3] *= sat(fmaxf(d - sim, 0.f) / smo);
		calc_color(u, co, br, ga);
		pr(m, u);
	} else if (!strcmp(m, "luma_v2") && n == 10) {
		for (int i = 0; i < 4; i++) px[i] = F(a, 2 + i);
		float lmax = F(a, 6), lmin = F(a, 7), smax = F(a, 8), smin = F(a, 9);
		unpremul(px, u);
		float l = u[0]*0.2126f + u[1]*0.7152f + u[2]*0.0722f;
		float mask = smooth_(lmin, lmin + smin, l) * (1.f - smooth_(lmax - smax, lmax, l));
		u[3] *= mask;
		for (int i = 0; i < 3; i++) u[i] *= u[3];
		pr(m, u);
	} else if (!strcmp(m, "luma_v1") && n == 10) {
		for (int i = 0; i < 4; i++) px[i] = F(a, 2 + i);
		float lmax = F(a, 6), lmin = F(a, 7), smax = F(a, 8), smin = F(a, 9);
		unpremul(px, u);
		float l = u[0]*0.2989f + u[1]*0.5870f + u[2]*0.1140f; /* dot with a float4 whose w coefficient is 0 */
		float mask = smooth_(lmin, lmin + smin, l) * (1.f - smooth_(lmax - smax, lmax, l));
		o[0] = u[0]; o[1] = u[1]; o[2] = u[2]; o[3] = mask;
		pr(m, o);
	} else if (!strcmp(m, "reinhard") && n == 6) {
		/* R G B multiplier */
		float v[3] = {F(a, 2), F(a, 3), F(a, 4)}, mul = F(a, 5);
		for (int i = 0; i < 3; i++) v[i] *= mul;
		r709_2020(v); reinhard(v); r2020_709(v);
		o[0] = v[0]; o[1] = v[1]; o[2] = v[2]; o[3] = 0;
		pr(m, o);
	} else if ((!strcmp(m, "maxrgb") || !strcmp(m, "maxrgb_sdr")) && n == 8) {
		/* R G B multiplier input_nits output_nits */
		float v[3] = {F(a, 2), F(a, 3), F(a, 4)}, mul = F(a, 5), in = F(a, 6), out = F(a, 7);
		for (int i = 0; i < 3; i++) v[i] *= mul;
		r709_2020(v); maxrgb(v, in, out); r2020_709(v);
		if (!strcmp(m, "maxrgb")) { for (int i = 0; i < 3; i++) v[i] *= 1.f / mul; }
		else for (int i = 0; i < 3; i++) { v[i] *= 10000.f / out; v[i] = lin(powf(sat(v[i]), 1.f / 2.4f)); }
		o[0] = v[0]; o[1] = v[1]; o[2] = v[2]; o[3] = 0;
		pr(m, o);
	} else die("bad model", m);
}

/* ---------------- commands ---------------- */
#define IS(c) (!strcmp(a[0], c))

static void run(struct script_line *sl)
{
	char **a = sl->argv;
	int n = sl->argc;
	char b[600];
	if (IS("echo")) {
		printf("echo");
		for (int i = 1; i < n; i++) printf(" %s", a[i]);
		putchar('\n');
	} else if (IS("video")) { /* W H FPS_NUM FPS_DEN */
		g_vf.ovi.base_width = (uint32_t)atoi(a[1]); g_vf.ovi.base_height = (uint32_t)atoi(a[2]);
		g_vf.ovi.fps_num = (uint32_t)strtoul(a[3], NULL, 0); g_vf.ovi.fps_den = (uint32_t)strtoul(a[4], NULL, 0);
	} else if (IS("sdr_white")) g_vf.sdr_white = strtof(a[1], NULL);
	else if (IS("space")) g_vf.cur_space = space_arg(a[1]);
	else if (IS("time")) g_vf.frame_time = strtoull(a[1], NULL, 0);
	else if (IS("knob")) {
		int v = atoi(a[2]);
		if (!strcmp(a[1], "effect_fail")) g_vf.effect_fail = v;
		else if (!strcmp(a[1], "begin")) g_vf.begin_result = v;
		else if (!strcmp(a[1], "texrender_fail")) g_vf.texrender_fail = v;
		else if (!strcmp(a[1], "tex_fail")) g_vf.tex_fail = v;
		else if (!strcmp(a[1], "fb_srgb")) g_vf.fb_srgb = v;
		else die("unknown knob", a[1]);
	} else if (IS("source")) { /* S bw bh w h SPACE FLAGS */
		if (nsrc >= MAXN) die("too many sources", a[1]);
		struct obs_source *s = &srcs[nsrc++];
		memset(s, 0, sizeof(*s));
		snprintf(s->name, sizeof(s->name), "%s", a[1]);
		s->bw = (uint32_t)strtoul(a[2], NULL, 0); s->bh = (uint32_t)strtoul(a[3], NULL, 0);
		s->w = (uint32_t)strtoul(a[4], NULL, 0); s->h = (uint32_t)strtoul(a[5], NULL, 0);
		s->space = space_arg(a[6]); s->flags = flags_arg(a[7]);
		static struct obs_source_info plain;
		s->info = &plain;
	} else if (IS("resize")) { /* S bw bh w h */
		struct obs_source *s = src_find(a[1]);
		s->bw = (uint32_t)strtoul(a[2], NULL, 0); s->bh = (uint32_t)strtoul(a[3], NULL, 0);
		s->w = (uint32_t)strtoul(a[4], NULL, 0); s->h = (uint32_t)strtoul(a[5], NULL, 0);
	} else if (IS("srcspace")) src_find(a[1])->space = space_arg(a[2]);
	else if (IS("srcflags")) src_find(a[1])->flags = flags_arg(a[2]);
	else if (IS("data")) {
		snprintf(datas[ndata].name, 24, "%s", a[1]);
		datas[ndata++].d = obs_data_create();
	} else if (IS("dset")) set_value(dat(a[1]), a + 1);
	else if (IS("ddump")) dump_data(a[1], dat(a[1]));
	else if (IS("file")) { /* NAME TEXT: write $TMP/NAME */
		snprintf(b, sizeof(b), "%s/%s", g_vf.tmpdir, a[1]);
		FILE *f = fopen(b, "wb");
		if (!f) die("cannot write", a[1]);
		fputs(a[2], f);
		fclose(f);
	} else if (IS("touch")) { /* NAME SEC */
		snprintf(b, sizeof(b), "%s/%s", g_vf.tmpdir, a[1]);
		struct timespec ts[2] = {{atol(a[2]), 0}, {atol(a[2]), 0}};
		utimensat(AT_FDCWD, b, ts, 0);
	} else if (IS("image")) /* NAME W H FORMAT LOADED ANIMATED */
		vf_define_image(a[1], (uint32_t)atoi(a[2]), (uint32_t)atoi(a[3]), cfmt_arg(a[4]), atoi(a[5]), atoi(a[6]));
	else if (IS("defaults")) { /* ID VER */
		const struct obs_source_info *i = info_find(a[1], atoi(a[2]));
		obs_data_t *d = obs_data_create();
		if (i->get_defaults) i->get_defaults(d);
		snprintf(b, sizeof(b), "%s.v%s", a[1], a[2]);
		dump_data(b, d);
		obs_data_release(d);
	} else if (IS("info")) {
		const struct obs_source_info *i = info_find(a[1], atoi(a[2]));
		printf("info %s v%u type=%d flags=0x%x callbacks:", i->id, i->version, (int)i->type, i->output_flags);
		if (i->video_tick) printf(" tick");
		if (i->get_width) printf(" size");
		if (i->show) printf(" show");
		if (i->video_get_color_space) printf(" colorspace");
		if (i->missing_files) printf(" missing_files");
		putchar('\n');
	} else if (IS("filter")) { /* F ID VER DATA|@null [TARGET [PARENT]] */
		if (nsrc >= MAXN) die("too many sources", a[1]);
		struct obs_source *s = &srcs[nsrc++];
		memset(s, 0, sizeof(*s));
		snprintf(s->name, sizeof(s->name), "%s", a[1]);
		s->info = info_find(a[2], atoi(a[3]));
		s->settings = obs_data_create();
		if (s->info->get_defaults) s->info->get_defaults(s->settings);
		if (strcmp(a[4], "@null")) obs_data_apply(s->settings, dat(a[4]));
		s->target = n > 5 ? src_find(a[5]) : NULL;
		s->parent = n > 6 ? src_find(a[6]) : NULL;
		s->data = s->info->create(s->settings, s);
		printf("create %s -> %s\n", s->name, s->data ? "ok" : "null");
	} else if (IS("retarget")) { /* F TARGET PARENT */
		struct obs_source *s = src_find(a[1]);
		s->target = src_find(a[2]); s->parent = src_find(a[3]);
	} else if (IS("fset")) set_value(src_find(a[1])->settings, a + 1);
	else if (IS("fdump")) dump_data(a[1], src_find(a[1])->settings);
	else if (IS("update")) obs_source_update(src_find(a[1]), dat(a[2]));
	else if (IS("tick")) {
		struct obs_source *s = src_find(a[1]);
		if (s->defer_update_count > 0 && s->data && s->info->update) {
			s->info->update(s->data, s->settings);
			s->defer_update_count = 0;
			printf("call deferred_update %s\n", s->name);
		}
		if (s->info->video_tick) s->info->video_tick(s->data, strtof(a[2], NULL));
		printf("tick %s %s\n", s->name, a[2]);
	} else if (IS("render")) {
		struct obs_source *s = src_find(a[1]);
		printf("render %s\n", s->name);
		s->info->video_render(s->data, NULL);
	} else if (IS("size")) {
		struct obs_source *s = src_find(a[1]);
		printf("size %s width=", s->name);
		if (s->info->get_width) printf("%u", s->info->get_width(s->data)); else printf("-");
		printf(" height=");
		if (s->info->get_height) printf("%u\n", s->info->get_height(s->data)); else printf("-\n");
	} else if (IS("cs")) { /* F COUNT SPACE... */
		struct obs_source *s = src_find(a[1]);
		enum gs_color_space pref[8];
		int c = atoi(a[2]);
		for (int i = 0; i < c; i++) pref[i] = space_arg(a[3 + i]);
		printf("cs %s -> %s\n", s->name, spn[s->info->video_get_color_space(s->data, (size_t)c, pref)]);
	} else if (IS("show")) {
		struct obs_source *s = src_find(a[1]);
		s->info->show(s->data);
		printf("show %s\n", s->name);
	} else if (IS("destroy")) {
		struct obs_source *s = src_find(a[1]);
		s->info->destroy(s->data);
		s->data = NULL;
		printf("destroy %s\n", s->name);
	} else if (IS("props") || IS("pvis")) {
		struct obs_source *s = src_find(a[1]);
		if (IS("props")) {
			if (s->props) obs_properties_destroy(s->props);
			s->props = s->info->get_properties(s->data);
		}
		dump_props(a[1], s->props, IS("props"));
	} else if (IS("pmod")) { /* F NAME: run the property's modified callback with the filter's settings */
		struct obs_source *s = src_find(a[1]);
		obs_property_t *p = obs_properties_get(s->props, a[2]);
		if (!p) die("no such property", a[2]);
		bool r = obs_property_modified(p, s->settings);
		printf("pmod %s %s -> %d\n", a[1], a[2], r ? 1 : 0);
	} else if (IS("papply")) {
		struct obs_source *s = src_find(a[1]);
		obs_properties_apply_settings(s->props, s->settings);
		printf("papply %s\n", a[1]);
	} else if (IS("missing")) {
		struct obs_source *s = src_find(a[1]);
		obs_missing_files_t *f = s->info->missing_files(s->data);
		printf("missing %s -> %s\n", a[1], f ? "list" : "null");
	} else if (IS("resolve")) {
		vf_resolve_missing(expand(a[1], b, sizeof(b)));
		printf("resolve\n");
	} else if (IS("model")) model(a, n);
	else die("unknown command", a[0]);
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	const char *t = getenv("CONFORMANCE_TMPDIR");
	snprintf(g_vf.tmpdir, sizeof(g_vf.tmpdir), "%s", t ? t : "/tmp");
	struct script_line sl;
	int lineno = 0;
	while (script_next(f, &sl, &lineno)) {
		if (!strcmp(sl.argv[0], "try") || !strcmp(sl.argv[0], "ignore")) {
			bool quiet = sl.argv[0][0] == 'i';
			if (sl.argc < 2) die("try needs a command", NULL);
			printf(quiet ? "ignore-begin\n" : "try-begin\n");
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				struct script_line sub = sl;
				for (int i = 0; i < sl.argc; i++) sub.argv[i] = sub.storage + (sl.argv[i] - sl.storage);
				for (int i = 1; i < sub.argc; i++) sub.argv[i - 1] = sub.argv[i];
				sub.argc--;
				signal(SIGALRM, on_alarm);
				alarm(10);
				if (quiet && !freopen("/dev/null", "w", stdout)) _exit(3);
				run(&sub);
				fflush(stdout);
				_exit(0);
			}
			int st = 0;
			waitpid(pid, &st, 0);
			if (quiet) printf("ignore-end\n");
			else if (WIFEXITED(st) && WEXITSTATUS(st) == 0) printf("try-end ok\n");
			else if (WIFEXITED(st) && WEXITSTATUS(st) == 124) printf("try-end hang\n");
			else printf("try-end crash\n");
			continue;
		}
		run(&sl);
	}
	fclose(f);
	return 0;
}
