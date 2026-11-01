/*
 * Conformance runner (area audiofilters, spec S-027): drives the obs_source_info vtables of the
 * OBS audio filters (gain, invert polarity, compressor, limiter, expander, upward compressor,
 * noise gate, async delay, 3-band EQ, noise suppress) with scripted input. Test tooling.
 */
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <math.h>
#include "obs.h"
#include "harness.h"
#include "audiofilters_api.h"

extern struct obs_source_info gain_filter, invert_polarity_filter, compressor_filter, limiter_filter,
	expander_filter, upward_compressor_filter, noise_gate_filter, async_delay_filter, eq_filter,
	noise_suppress_filter, noise_suppress_filter_v2;
void af_state_gain(void *), af_state_invert(void *), af_state_limiter(void *), af_state_compressor(void *),
	af_state_expander(void *), af_state_gate(void *), af_state_delay(void *), af_state_eq(void *),
	af_state_ns(void *);

struct ftab {
	const char *name;
	struct obs_source_info *info;
	void (*state)(void *);
};
static struct ftab ftabs[] = {
	{"gain", &gain_filter, af_state_gain},
	{"invert", &invert_polarity_filter, af_state_invert},
	{"compressor", &compressor_filter, af_state_compressor},
	{"limiter", &limiter_filter, af_state_limiter},
	{"expander", &expander_filter, af_state_expander},
	{"upward", &upward_compressor_filter, af_state_expander},
	{"gate", &noise_gate_filter, af_state_gate},
	{"delay", &async_delay_filter, af_state_delay},
	{"eq", &eq_filter, af_state_eq},
	{"ns", &noise_suppress_filter, af_state_ns},
	{"ns2", &noise_suppress_filter_v2, af_state_ns},
};
static struct ftab *ftab_find(const char *n)
{
	for (size_t i = 0; i < sizeof(ftabs) / sizeof(ftabs[0]); i++)
		if (!strcmp(ftabs[i].name, n))
			return &ftabs[i];
	fprintf(stderr, "unknown filter %s\n", n);
	exit(2);
}

#define MAXI 16
struct inst {
	char id[32];
	struct ftab *f;
	void *data;
	obs_data_t *live;
	struct af_source *ctx;
	uint64_t pos;
	bool used;
};
static struct inst insts[MAXI];
static struct { char name[32]; obs_data_t *d; } datas[32];
static int ndatas;

static struct inst *inst_get(const char *id)
{
	for (int i = 0; i < MAXI; i++)
		if (insts[i].used && !strcmp(insts[i].id, id))
			return &insts[i];
	fprintf(stderr, "unknown instance %s\n", id);
	exit(2);
}
static obs_data_t *dref(const char *n, bool create)
{
	if (n[0] == '@')
		return inst_get(n + 1)->live;
	for (int i = 0; i < ndatas; i++)
		if (!strcmp(datas[i].name, n))
			return datas[i].d;
	if (!create) {
		fprintf(stderr, "unknown data %s\n", n);
		exit(2);
	}
	snprintf(datas[ndatas].name, 32, "%s", n);
	datas[ndatas].d = obs_data_create();
	return datas[ndatas++].d;
}

/* ---------- generators ---------- */
static uint64_t splitmix(uint64_t z)
{
	z += 0x9E3779B97F4A7C15ULL;
	z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ULL;
	z = (z ^ (z >> 27)) * 0x94D049BB133111EBULL;
	return z ^ (z >> 31);
}
static float gen_eval(const char *spec, int64_t n)
{
	char buf[512];
	snprintf(buf, sizeof(buf), "%s", spec);
	int64_t w0 = 0, wl = -1;
	char *at = strchr(buf, '@');
	if (at) {
		*at = 0;
		sscanf(at + 1, "%lld+%lld", (long long *)&w0, (long long *)&wl);
		if (n < w0 || n >= w0 + wl)
			return 0.0f;
	}
	char *kind = buf, *a[4] = {0}, *p = strchr(buf, ':');
	int na = 0;
	if (p) {
		*p++ = 0;
		if (!strcmp(kind, "list")) {
			a[0] = p;
		} else {
			while (p && na < 4) {
				a[na++] = p;
				p = strchr(p, ':');
				if (p)
					*p++ = 0;
			}
		}
	}
	double v[4] = {0};
	if (strcmp(kind, "list"))
		for (int i = 0; i < na; i++)
			v[i] = strtod(a[i], NULL);
	if (!strcmp(kind, "zero"))
		return 0.0f;
	if (!strcmp(kind, "const"))
		return (float)v[0];
	if (!strcmp(kind, "sine"))
		return (float)(v[0] * sin(2.0 * 3.14159265358979323846 * v[1] * (double)n / (double)g_af.rate));
	if (!strcmp(kind, "step"))
		return (float)(n < (int64_t)v[2] ? v[0] : v[1]);
	if (!strcmp(kind, "impulse"))
		return (float)(n == (int64_t)v[1] ? v[0] : 0.0);
	if (!strcmp(kind, "pulse"))
		return (float)(n >= (int64_t)v[1] && n < (int64_t)v[1] + (int64_t)v[2] ? v[0] : 0.0);
	if (!strcmp(kind, "ramp"))
		return (float)(v[0] + v[1] * (double)n);
	if (!strcmp(kind, "noise")) {
		uint64_t z = splitmix((uint64_t)n + (uint64_t)v[1] * 0x9E3779B97F4A7C15ULL);
		double u = (double)(z >> 40) / 8388608.0 - 1.0;
		return (float)(v[0] * u);
	}
	if (!strcmp(kind, "list")) {
		char *s = a[0];
		for (int64_t i = 0; s && *s; i++) {
			char *c = strchr(s, ',');
			if (i == n)
				return (float)strtod(s, NULL);
			s = c ? c + 1 : NULL;
		}
		return 0.0f;
	}
	fprintf(stderr, "bad generator %s\n", spec);
	exit(2);
}
static uint32_t fnv(const float *x, uint32_t n)
{
	uint32_t h = 2166136261u;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t b;
		memcpy(&b, &x[i], 4);
		if (b == 0x80000000u)
			b = 0;
		for (int k = 0; k < 4; k++) {
			h ^= (b >> (8 * k)) & 0xff;
			h *= 16777619u;
		}
	}
	return h;
}

/* planes: allocated with exact size so that sanitizers see over-reads */
static void alloc_planes(uint8_t **data, int nplanes, char **gens, uint32_t frames, uint64_t pos)
{
	for (int p = 0; p < MAX_AV_PLANES; p++)
		data[p] = NULL;
	for (int p = 0; p < nplanes; p++) {
		if (!strcmp(gens[p], "-"))
			continue;
		float *f = malloc((frames ? frames : 1) * sizeof(float));
		for (uint32_t i = 0; i < frames; i++)
			f[i] = gen_eval(gens[p], (int64_t)(pos + i));
		data[p] = (uint8_t *)f;
	}
}
static void print_samples(const char *label, int p, const float *x, uint32_t frames, const char *spec)
{
	uint32_t from = 0, to = frames, step = 1;
	if (!strcmp(spec, "none"))
		return;
	if (!strcmp(spec, "stats")) {
		double mn = INFINITY, mx = -INFINITY, sum = 0, sq = 0;
		bool nan = false;
		for (uint32_t i = 0; i < frames; i++) {
			if (isnan(x[i])) { nan = true; continue; }
			mn = fmin(mn, x[i]); mx = fmax(mx, x[i]); sum += x[i]; sq += (double)x[i] * x[i];
		}
		printf("  %s p%d stats n=%u nan=%d crc=%08x", label, p, frames, nan, fnv(x, frames));
		AF_F("min", mn); AF_F("max", mx); AF_F("sum", sum); AF_F("sumsq", sq);
		printf("\n");
		return;
	}
	if (strcmp(spec, "all")) { /* P:FROM:TO:STEP */
		int pp = -1;
		unsigned f1 = 0, t1 = 0, s1 = 1;
		if (sscanf(spec, "%d:%u:%u:%u", &pp, &f1, &t1, &s1) < 3)
			return;
		if (pp != p)
			return;
		from = f1; to = t1 < frames ? t1 : frames; step = s1 ? s1 : 1;
	}
	int col = 0;
	for (uint32_t i = from; i < to; i += step) {
		if (col == 0)
			printf("  %s p%d [%u]", label, p, i);
		printf(" ");
		af_pf(x[i]);
		if (++col == 16) { printf("\n"); col = 0; }
	}
	if (col)
		printf("\n");
}

/* ---------- property dump ---------- */
static const char *ptype[] = {"invalid", "bool", "int", "float", "text", "path", "list", "color", "button", "font",
			      "editable_list", "frame_rate", "group", "color_alpha"};
static void dump_props(obs_properties_t *ps)
{
	size_t n = 0;
	for (obs_property_t *p = obs_properties_first(ps); p; obs_property_next(&p))
		n++;
	printf("props count=%zu\n", n);
	obs_property_t *p = obs_properties_first(ps);
	for (; p; obs_property_next(&p)) {
		enum obs_property_type t = obs_property_get_type(p);
		printf("  prop %s type=%s desc=\"%s\" enabled=%d visible=%d", obs_property_name(p), ptype[t],
		       obs_property_description(p), obs_property_enabled(p), obs_property_visible(p));
		if (t == OBS_PROPERTY_FLOAT) {
			AF_F("min", obs_property_float_min(p)); AF_F("max", obs_property_float_max(p));
			AF_F("step", obs_property_float_step(p));
			printf(" numtype=%d suffix=\"%s\"", obs_property_float_type(p), obs_property_float_suffix(p) ? obs_property_float_suffix(p) : "(null)");
		} else if (t == OBS_PROPERTY_INT) {
			printf(" min=%d max=%d step=%d numtype=%d suffix=\"%s\"", obs_property_int_min(p),
			       obs_property_int_max(p), obs_property_int_step(p), obs_property_int_type(p),
			       obs_property_int_suffix(p) ? obs_property_int_suffix(p) : "(null)");
		} else if (t == OBS_PROPERTY_LIST) {
			printf(" listtype=%d format=%d items=%zu", obs_property_list_type(p), obs_property_list_format(p),
			       obs_property_list_item_count(p));
		}
		printf("\n");
		if (t == OBS_PROPERTY_LIST)
			for (size_t i = 0; i < obs_property_list_item_count(p); i++)
				printf("    item %zu name=\"%s\" value=\"%s\" disabled=%d\n", i,
				       obs_property_list_item_name(p, i), obs_property_list_item_string(p, i),
				       obs_property_list_item_disabled(p, i));
	}
}
static int cmp_names(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}
static void dump_data(const char *label, obs_data_t *d)
{
	printf("data %s (sorted by key)\n", label);
	const char *names[64];
	int nn = 0;
	for (obs_data_item_t *it = obs_data_first(d); it && nn < 64; obs_data_item_next(&it))
		names[nn++] = obs_data_item_get_name(it);
	qsort(names, (size_t)nn, sizeof(names[0]), cmp_names);
	for (int ni = 0; ni < nn; ni++) {
		obs_data_item_t *it = obs_data_item_byname(d, names[ni]);
		enum obs_data_type t = obs_data_item_gettype(it);
		const char *nm = obs_data_item_get_name(it);
		printf("  %s user=%d default=%d", nm, obs_data_item_has_user_value(it), obs_data_item_has_default_value(it));
		if (t == OBS_DATA_STRING)
			printf(" string \"%s\" def=\"%s\"", obs_data_item_get_string(it), obs_data_item_get_default_string(it));
		else if (t == OBS_DATA_NUMBER && obs_data_item_numtype(it) == OBS_DATA_NUM_INT)
			printf(" int %lld def=%lld", obs_data_item_get_int(it), obs_data_item_get_default_int(it));
		else if (t == OBS_DATA_NUMBER)
			printf(" double %.17g def=%.17g", obs_data_item_get_double(it), obs_data_item_get_default_double(it));
		else if (t == OBS_DATA_BOOLEAN)
			printf(" bool %d def=%d", obs_data_item_get_bool(it), obs_data_item_get_default_bool(it));
		else
			printf(" other");
		printf("\n");
		obs_data_item_release(&it);
	}
}

/* ---------- video frames ---------- */
static struct obs_source_frame *frames_alloc[64];
static int nframes_alloc;

static void run_cmd(struct script_line *sl, int o);

static void do_try(struct script_line *sl)
{
	fflush(stdout);
	printf("try-begin\n");
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		alarm(10);
		run_cmd(sl, 1);
		fflush(stdout);
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	const char *r = (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? "ok" : WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM ? "hang" : "crash";
	printf("try-end %s\n", r);
}

#define ARGC(n) do { if (sl->argc < o + (n)) { fprintf(stderr, "line %d: too few args\n", sl->lineno); exit(2); } } while (0)

static void run_cmd(struct script_line *sl, int o)
{
	char **a = sl->argv + o;
	const char *c = a[0];
	if (!strcmp(c, "echo")) {
		printf("echo");
		for (int i = o + 1; i < sl->argc; i++)
			printf(" %s", sl->argv[i]);
		printf("\n");
	} else if (!strcmp(c, "try")) {
		struct script_line sub = *sl;
		(void)sub;
		do_try(sl); /* sl->argv[1..] is the command; do_try runs run_cmd(sl,1) */
	} else if (!strcmp(c, "audio")) { /* audio CHANNELS RATE */
		ARGC(3);
		g_af.channels = (size_t)atoi(a[1]);
		g_af.rate = (uint32_t)atoi(a[2]);
	} else if (!strcmp(c, "clock")) {
		ARGC(2);
		g_af.clock = strtoull(a[1], NULL, 10);
	} else if (!strcmp(c, "advance")) {
		ARGC(2);
		g_af.clock += strtoull(a[1], NULL, 10);
	} else if (!strcmp(c, "events")) {
		ARGC(2);
		g_af.events = !strcmp(a[1], "on");
	} else if (!strcmp(c, "resampler_frames")) {
		ARGC(2);
		g_af.resampler_out_frames = (uint32_t)atoi(a[1]);
	} else if (!strcmp(c, "source")) { /* source NAME audio|video [mono|stereo|unknown] */
		ARGC(3);
		struct af_source *s = calloc(1, sizeof(*s));
		snprintf(s->name, sizeof(s->name), "%s", a[1]);
		s->audio = !strcmp(a[2], "audio");
		s->layout = SPEAKERS_STEREO;
		if (sl->argc > o + 3)
			s->layout = !strcmp(a[3], "mono") ? SPEAKERS_MONO : !strcmp(a[3], "unknown") ? SPEAKERS_UNKNOWN : SPEAKERS_STEREO;
		g_sources[g_nsources++] = s;
	} else if (!strcmp(c, "source_dead")) {
		ARGC(2);
		af_find_source(a[1])->dead = true;
	} else if (!strcmp(c, "refs")) {
		ARGC(2);
		struct af_source *s = af_find_source(a[1]);
		printf("refs %s strong=%d callbacks=%d weak_live=%d\n", a[1], s->refs, s->ncb, g_af.weak_live);
	} else if (!strcmp(c, "data")) {
		ARGC(2);
		dref(a[1], true);
	} else if (!strcmp(c, "set") || !strcmp(c, "setdef")) { /* set D KEY int|double|string|bool VAL */
		ARGC(5);
		obs_data_t *d = dref(a[1], true);
		bool def = c[3] == 'd';
		if (!strcmp(a[3], "int")) {
			long long v = strtoll(a[4], NULL, 0);
			def ? obs_data_set_default_int(d, a[2], v) : obs_data_set_int(d, a[2], v);
		} else if (!strcmp(a[3], "double")) {
			double v = strtod(a[4], NULL);
			def ? obs_data_set_default_double(d, a[2], v) : obs_data_set_double(d, a[2], v);
		} else if (!strcmp(a[3], "bool")) {
			bool v = !strcmp(a[4], "1");
			def ? obs_data_set_default_bool(d, a[2], v) : obs_data_set_bool(d, a[2], v);
		} else {
			def ? obs_data_set_default_string(d, a[2], a[4]) : obs_data_set_string(d, a[2], a[4]);
		}
	} else if (!strcmp(c, "dump")) {
		ARGC(2);
		dump_data(a[1], dref(a[1], false));
	} else if (!strcmp(c, "info")) {
		ARGC(2);
		struct ftab *f = ftab_find(a[1]);
		struct obs_source_info *i = f->info;
		printf("info %s id=%s version=%u type=%d flags=0x%x name=\"%s\"\n", f->name, i->id, i->version, i->type,
		       i->output_flags, i->get_name(NULL));
		printf("  has create=%d destroy=%d update=%d filter_audio=%d filter_video=%d video_tick=%d filter_remove=%d get_defaults=%d get_properties=%d\n",
		       !!i->create, !!i->destroy, !!i->update, !!i->filter_audio, !!i->filter_video, !!i->video_tick,
		       !!i->filter_remove, !!i->get_defaults, !!i->get_properties);
	} else if (!strcmp(c, "defaults")) { /* defaults FILTER D */
		ARGC(3);
		struct ftab *f = ftab_find(a[1]);
		if (f->info->get_defaults)
			f->info->get_defaults(dref(a[2], true));
	} else if (!strcmp(c, "create")) { /* create ID FILTER [D|-] [parent=NAME] [nodefaults] */
		ARGC(3);
		struct inst *in = NULL;
		for (int i = 0; i < MAXI && !in; i++)
			if (!insts[i].used)
				in = &insts[i];
		memset(in, 0, sizeof(*in));
		in->used = true;
		snprintf(in->id, sizeof(in->id), "%s", a[1]);
		in->f = ftab_find(a[2]);
		in->live = obs_data_create();
		bool defs = true;
		for (int i = 3; i < sl->argc - o; i++) {
			if (!strncmp(a[i], "parent=", 7))
				in->ctx = NULL, (void)0;
			else if (!strcmp(a[i], "nodefaults"))
				defs = false;
			else if (strcmp(a[i], "-"))
				obs_data_apply(in->live, dref(a[i], false));
		}
		in->ctx = calloc(1, sizeof(*in->ctx));
		snprintf(in->ctx->name, sizeof(in->ctx->name), "%s", in->id);
		in->ctx->layout = SPEAKERS_STEREO;
		for (int i = 3; i < sl->argc - o; i++)
			if (!strncmp(a[i], "parent=", 7))
				in->ctx->parent = af_find_source(a[i] + 7);
		if (defs && in->f->info->get_defaults)
			in->f->info->get_defaults(in->live);
		in->data = in->f->info->create(in->live, (obs_source_t *)in->ctx);
		printf("create %s -> %s\n", in->id, in->data ? "ok" : "null");
	} else if (!strcmp(c, "update")) { /* update ID D|- */
		ARGC(3);
		struct inst *in = inst_get(a[1]);
		if (strcmp(a[2], "-"))
			obs_data_apply(in->live, dref(a[2], false));
		in->f->info->update(in->data, in->live);
	} else if (!strcmp(c, "reset_settings")) { /* host obs_source_reset_settings: clear then update */
		ARGC(3);
		struct inst *in = inst_get(a[1]);
		obs_data_clear(in->live);
		if (strcmp(a[2], "-"))
			obs_data_apply(in->live, dref(a[2], false));
		in->f->info->update(in->data, in->live);
	} else if (!strcmp(c, "state")) {
		ARGC(2);
		struct inst *in = inst_get(a[1]);
		in->f->state(in->data);
	} else if (!strcmp(c, "destroy")) {
		ARGC(2);
		struct inst *in = inst_get(a[1]);
		in->f->info->destroy(in->data);
		printf("destroy %s\n", in->id);
		in->used = false;
	} else if (!strcmp(c, "tick")) {
		ARGC(2);
		struct inst *in = inst_get(a[1]);
		in->f->info->video_tick(in->data, 0.0f);
	} else if (!strcmp(c, "seek")) {
		ARGC(3);
		inst_get(a[1])->pos = strtoull(a[2], NULL, 10);
	} else if (!strcmp(c, "props") || !strcmp(c, "props_null")) {
		ARGC(2);
		void *data = NULL;
		struct obs_source_info *info;
		if (c[5] == '_') {
			info = ftab_find(a[1])->info;
		} else {
			struct inst *in = inst_get(a[1]);
			info = in->f->info;
			data = in->data;
		}
		obs_properties_t *ps = info->get_properties(data);
		dump_props(ps);
		if (sl->argc > o + 3 && !strcmp(a[2], "modified")) { /* props ID modified PROP D */
			obs_property_t *p = obs_properties_get(ps, a[3]);
			bool r = obs_property_modified(p, dref(a[4], false));
			printf("modified %s -> %d\n", a[3], r);
			dump_props(ps);
		}
		obs_properties_destroy(ps);
	} else if (!strcmp(c, "process")) { /* process ID FRAMES [ts=N] [out=SPEC] GEN... */
		ARGC(3);
		struct inst *in = inst_get(a[1]);
		uint32_t frames = (uint32_t)strtoul(a[2], NULL, 10);
		uint64_t ts = 0;
		const char *out = "all";
		char *gens[MAX_AV_PLANES];
		int ng = 0;
		for (int i = 3; i < sl->argc - o; i++) {
			if (!strncmp(a[i], "ts=", 3))
				ts = strtoull(a[i] + 3, NULL, 10);
			else if (!strncmp(a[i], "out=", 4))
				out = a[i] + 4;
			else
				gens[ng++] = a[i];
		}
		struct obs_audio_data ad;
		memset(&ad, 0, sizeof(ad));
		alloc_planes(ad.data, ng, gens, frames, in->pos);
		ad.frames = frames;
		ad.timestamp = ts;
		printf("process %s frames=%u ts=%llu pos=%llu in-crc:", in->id, frames, (unsigned long long)ts,
		       (unsigned long long)in->pos);
		for (int p = 0; p < ng; p++)
			if (ad.data[p])
				printf(" %08x", fnv((float *)ad.data[p], frames));
			else
				printf(" -");
		printf("\n");
		struct obs_audio_data *r = in->f->info->filter_audio(in->data, &ad);
		in->pos += frames;
		if (!r) {
			printf("  ret null\n");
		} else {
			printf("  ret %s frames=%u ts=%llu\n", r == &ad ? "same" : "own", r->frames, (unsigned long long)r->timestamp);
			for (int p = 0; p < MAX_AV_PLANES; p++)
				if (r->data[p])
					print_samples("out", p, (float *)r->data[p], r->frames, out);
		}
		for (int p = 0; p < MAX_AV_PLANES; p++)
			free(ad.data[p]);
	} else if (!strcmp(c, "sc_push")) { /* sc_push SRC FRAMES muted GEN... */
		ARGC(4);
		struct af_source *s = af_find_source(a[1]);
		uint32_t frames = (uint32_t)atoi(a[2]);
		bool muted = !strcmp(a[3], "1");
		char *gens[MAX_AV_PLANES];
		int ng = 0;
		for (int i = 4; i < sl->argc - o; i++)
			gens[ng++] = a[i];
		struct audio_data ad;
		memset(&ad, 0, sizeof(ad));
		alloc_planes(ad.data, ng, gens, frames, s->pos);
		ad.frames = frames;
		s->pos += frames;
		printf("sc_push %s frames=%u muted=%d callbacks=%d\n", s->name, frames, muted, s->ncb);
		for (int i = 0; i < s->ncb; i++)
			s->cbs[i].cb(s->cbs[i].param, (obs_source_t *)s, &ad, muted);
		for (int p = 0; p < MAX_AV_PLANES; p++)
			free(ad.data[p]);
	} else if (!strcmp(c, "video")) { /* video ID TS */
		ARGC(3);
		struct inst *in = inst_get(a[1]);
		struct obs_source_frame *f = calloc(1, sizeof(*f));
		f->timestamp = strtoull(a[2], NULL, 10);
		f->width = ++g_af.seq;
		frames_alloc[nframes_alloc++] = f;
		struct obs_source_frame *r = in->f->info->filter_video(in->data, f);
		printf("video %s ts=%llu tag=%u -> ", in->id, (unsigned long long)f->timestamp, f->width);
		if (r)
			printf("tag=%u ts=%llu\n", r->width, (unsigned long long)r->timestamp);
		else
			printf("null\n");
	} else if (!strcmp(c, "remove")) {
		ARGC(2);
		struct inst *in = inst_get(a[1]);
		in->f->info->filter_remove(in->data, (obs_source_t *)in->ctx->parent);
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
	while (script_next(f, &sl, &lineno))
		run_cmd(&sl, 0);
	fclose(f);
	return 0;
}
