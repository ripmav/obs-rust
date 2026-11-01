/*
 * Conformance runner (area filtersrest, spec S-054). Links the REAL vendored RNNoise, the REAL libspeexdsp, the real
 * noise-suppress-filter.c and the real obs-filters.c module table. Only the OBS host (sources, audio info) is faked.
 * Test tooling, not part of OBS.
 */
#include <math.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "obs.h"
#include "harness.h"
#include "../../../plugins/obs-filters/noise-suppress-filter.c"

bool filtersrest_module_load(void);
const char *filtersrest_module_description(void);

/* ---- fake host ---- */
struct fsrc {
	char name[32];
	enum speaker_layout layout;
	struct fsrc *parent;
};
static uint32_t g_rate = 48000;
static size_t g_ch = 2;
char *os_quick_read_utf8_file(const char *p) { (void)p; abort(); }
bool os_quick_write_utf8_file(const char *p, const char *s, size_t l, bool m) { (void)p, (void)s, (void)l, (void)m; abort(); }
bool os_quick_write_utf8_file_safe(const char *p, const char *s, size_t l, bool m, const char *t, const char *b)
{
	(void)p, (void)s, (void)l, (void)m, (void)t, (void)b;
	abort();
}
int os_rename(const char *a, const char *b) { return rename(a, b); }
const char *obs_module_text(const char *key) { return key; }
audio_t *obs_get_audio(void) { return (audio_t *)&g_rate; }
uint32_t audio_output_get_sample_rate(const audio_t *a) { (void)a; return g_rate; }
size_t audio_output_get_channels(const audio_t *a) { (void)a; return g_ch; }
const char *obs_source_get_name(const obs_source_t *s) { return ((const struct fsrc *)s)->name; }
obs_source_t *obs_filter_get_parent(const obs_source_t *f) { return (obs_source_t *)((const struct fsrc *)f)->parent; }
enum speaker_layout obs_source_get_speaker_layout(obs_source_t *s) { return s ? ((struct fsrc *)s)->layout : SPEAKERS_UNKNOWN; }

static const struct obs_source_info *reg[64];
static int nreg;
void obs_register_source_s(const struct obs_source_info *info, size_t size)
{
	(void)size;
	if (nreg < 64)
		reg[nreg++] = info;
}

/* ---- deterministic signal generator ----
 * kind: silence | sine F A | harm F A | noise A SEED | speechlike F A NA SEED (harmonic 1/k up to 8 + uniform noise NA)
 * Amplitudes are in units of the final sample scale (the caller states which). Phase is a running sample counter. */
struct gen {
	char kind[16];
	double f, a, na;
	uint32_t seed;
	uint64_t pos;
	uint32_t rng;
};
static double rnd(struct gen *g)
{
	g->rng = g->rng * 1664525u + 1013904223u;
	return ((double)(g->rng >> 8) / 8388608.0) - 1.0; /* [-1,1) */
}
static double gen_next(struct gen *g, double rate)
{
	double t = (double)g->pos++ / rate, v = 0;
	if (!strcmp(g->kind, "silence"))
		v = 0;
	else if (!strcmp(g->kind, "sine"))
		v = g->a * sin(2 * M_PI * g->f * t);
	else if (!strcmp(g->kind, "harm") || !strcmp(g->kind, "speechlike")) {
		for (int k = 1; k <= 8; k++)
			v += sin(2 * M_PI * g->f * k * t) / k;
		v *= g->a / 2.0;
		if (!strcmp(g->kind, "speechlike"))
			v += g->na * rnd(g);
	} else if (!strcmp(g->kind, "noise"))
		v = g->a * rnd(g);
	return v;
}
static int parse_gen(struct gen *g, char **a, int n)
{
	memset(g, 0, sizeof(*g));
	snprintf(g->kind, sizeof(g->kind), "%s", a[0]);
	int u = 1;
	if (!strcmp(a[0], "sine") || !strcmp(a[0], "harm")) {
		g->f = atof(a[1]), g->a = atof(a[2]), u = 3;
	} else if (!strcmp(a[0], "noise")) {
		g->a = atof(a[1]), g->rng = (uint32_t)strtoul(a[2], NULL, 0), u = 3;
	} else if (!strcmp(a[0], "speechlike")) {
		g->f = atof(a[1]), g->a = atof(a[2]), g->na = atof(a[3]), g->rng = (uint32_t)strtoul(a[4], NULL, 0), u = 5;
	} else if (strcmp(a[0], "silence")) {
		fprintf(stderr, "unknown generator %s\n", a[0]);
		exit(2);
	}
	(void)n;
	return u;
}

static double rms(const float *x, size_t n)
{
	double s = 0;
	for (size_t i = 0; i < n; i++)
		s += (double)x[i] * x[i];
	return n ? sqrt(s / (double)n) : 0;
}
static void pf(double v, const char *fmt)
{
	if (isnan(v)) fputs("nan", stdout);
	else if (isinf(v)) fputs(v < 0 ? "-inf" : "inf", stdout);
	else if (fabs(v) < 5e-4 && !strcmp(fmt, "%.3f")) fputs("0", stdout);
	else printf(fmt, v);
}

/* ---- direct RNNoise streams ---- */
#define MAXS 8
static struct rs {
	char id[16];
	DenoiseState *st;
	struct gen g;
	bool used;
} rss[MAXS];
static struct rs *rfind(const char *id, bool create)
{
	for (int i = 0; i < MAXS; i++)
		if (rss[i].used && !strcmp(rss[i].id, id))
			return &rss[i];
	if (!create) { fprintf(stderr, "no rnnoise stream %s\n", id); exit(2); }
	for (int i = 0; i < MAXS; i++)
		if (!rss[i].used) { rss[i].used = true; snprintf(rss[i].id, sizeof(rss[i].id), "%s", id); return &rss[i]; }
	exit(2);
}

/* ---- speex streams ---- */
static struct ss {
	char id[16];
	SpeexPreprocessState *st;
	struct gen g;
	bool used;
} sss[MAXS];
static struct ss *sfind(const char *id, bool create)
{
	for (int i = 0; i < MAXS; i++)
		if (sss[i].used && !strcmp(sss[i].id, id))
			return &sss[i];
	if (!create) { fprintf(stderr, "no speex stream %s\n", id); exit(2); }
	for (int i = 0; i < MAXS; i++)
		if (!sss[i].used) { sss[i].used = true; snprintf(sss[i].id, sizeof(sss[i].id), "%s", id); return &sss[i]; }
	exit(2);
}

/* ---- filter instances ---- */
static struct fi {
	char id[16];
	struct obs_source_info *info;
	void *data;
	struct fsrc ctx, parent;
	struct gen g[8];
	uint64_t in_pos;
	bool used;
} fis[MAXS];
static struct fi *ffind(const char *id)
{
	for (int i = 0; i < MAXS; i++)
		if (fis[i].used && !strcmp(fis[i].id, id))
			return &fis[i];
	fprintf(stderr, "no filter %s\n", id);
	exit(2);
}

static void do_try(struct script_line *sl);
static void run_cmd(struct script_line *sl);

static void run_cmd(struct script_line *sl)
{
	char **a = sl->argv;
	const char *c = a[0];
	if (!strcmp(c, "echo")) {
		for (int i = 1; i < sl->argc; i++)
			printf("%s%s", i > 1 ? " " : "", a[i]);
		printf("\n");
	} else if (!strcmp(c, "try")) {
		do_try(sl);
	} else if (!strcmp(c, "audio")) { /* audio RATE CHANNELS */
		script_need(sl, 3);
		g_rate = (uint32_t)atoi(a[1]);
		g_ch = (size_t)atoi(a[2]);
	} else if (!strcmp(c, "rnn_info")) {
		printf("rnn_info frame_size=%d sample_rate=%d size_positive=%d\n", RNNOISE_FRAME_SIZE, RNNOISE_SAMPLE_RATE,
		       rnnoise_get_size() > 0);
	} else if (!strcmp(c, "rnn_create")) { /* rnn_create S */
		script_need(sl, 2);
		struct rs *r = rfind(a[1], true);
		r->st = rnnoise_create(NULL);
		printf("rnn_create %s ok=%d\n", a[1], r->st != NULL);
	} else if (!strcmp(c, "rnn_destroy")) {
		script_need(sl, 2);
		struct rs *r = rfind(a[1], false);
		rnnoise_destroy(r->st);
		r->st = NULL, r->used = false;
		printf("rnn_destroy %s\n", a[1]);
	} else if (!strcmp(c, "rnn_gen")) { /* rnn_gen S KIND ... (amplitudes in int16 units) */
		script_need(sl, 3);
		struct rs *r = rfind(a[1], false);
		parse_gen(&r->g, a + 2, sl->argc - 2);
	} else if (!strcmp(c, "rnn_run")) { /* rnn_run S N [inplace]: N frames of 480; prints per frame vad/rms */
		script_need(sl, 3);
		struct rs *r = rfind(a[1], false);
		int n = atoi(a[2]);
		bool inplace = false;
		int every = 1;
		for (int i = 3; i < sl->argc; i++) {
			if (!strcmp(a[i], "inplace")) inplace = true;
			else if (!strncmp(a[i], "every=", 6)) every = atoi(a[i] + 6);
		}
		for (int f = 0; f < n; f++) {
			float in[480], out[480];
			for (int i = 0; i < 480; i++)
				in[i] = (float)gen_next(&r->g, 48000.0);
			double rin = rms(in, 480);
			float vad;
			if (inplace) {
				memcpy(out, in, sizeof(in));
				vad = rnnoise_process_frame(r->st, out, out);
			} else
				vad = rnnoise_process_frame(r->st, out, in);
			double ro = rms(out, 480), pk = 0;
			for (int i = 0; i < 480; i++)
				pk = fmax(pk, fabs(out[i]));
			if (f % every != 0 && f != n - 1)
				continue;
			printf("rnn %s frame=%d vad=", a[1], f);
			pf(vad, "%.3f");
			printf(" rms_in=%.0f rms_out=%.0f peak_out=%.0f\n", rin, ro, pk);
		}
	} else if (!strcmp(c, "rnn_cmp")) { /* rnn_cmp S1 S2 N : both fed the SAME generator spec (S1's), compare exactly */
		script_need(sl, 4);
		struct rs *r1 = rfind(a[1], false), *r2 = rfind(a[2], false);
		int n = atoi(a[3]);
		struct gen g1 = r1->g, g2 = r1->g;
		int diff = 0;
		for (int f = 0; f < n; f++) {
			float in1[480], in2[480], o1[480], o2[480];
			for (int i = 0; i < 480; i++)
				in1[i] = (float)gen_next(&g1, 48000.0), in2[i] = (float)gen_next(&g2, 48000.0);
			rnnoise_process_frame(r1->st, o1, in1);
			rnnoise_process_frame(r2->st, o2, in2);
			if (memcmp(o1, o2, sizeof(o1)))
				diff++;
		}
		printf("rnn_cmp %s %s frames=%d identical=%d\n", a[1], a[2], n, diff == 0);
	} else if (!strcmp(c, "spx_create")) { /* spx_create S FRAMES RATE */
		script_need(sl, 4);
		struct ss *s = sfind(a[1], true);
		s->st = speex_preprocess_state_init(atoi(a[2]), atoi(a[3]));
		printf("spx_create %s ok=%d\n", a[1], s->st != NULL);
	} else if (!strcmp(c, "spx_destroy")) {
		script_need(sl, 2);
		struct ss *s = sfind(a[1], false);
		speex_preprocess_state_destroy(s->st);
		s->st = NULL, s->used = false;
	} else if (!strcmp(c, "spx_ctl")) { /* spx_ctl S LEVEL */
		script_need(sl, 3);
		struct ss *s = sfind(a[1], false);
		int lv = atoi(a[2]);
		int r = speex_preprocess_ctl(s->st, SPEEX_PREPROCESS_SET_NOISE_SUPPRESS, &lv);
		int back = 12345;
		speex_preprocess_ctl(s->st, SPEEX_PREPROCESS_GET_NOISE_SUPPRESS, &back);
		printf("spx_ctl %s set=%d get=%d ret=%d\n", a[1], lv, back, r);
	} else if (!strcmp(c, "spx_gen")) {
		script_need(sl, 3);
		parse_gen(&sfind(a[1], false)->g, a + 2, sl->argc - 2);
	} else if (!strcmp(c, "spx_run")) { /* spx_run S N FRAMES RATE: i16 path exactly as the plugin (x32767 trunc) */
		script_need(sl, 5);
		struct ss *s = sfind(a[1], false);
		int n = atoi(a[2]), fr = atoi(a[3]);
		int every = sl->argc > 5 && !strncmp(a[5], "every=", 6) ? atoi(a[5] + 6) : 1;
		double rate = atof(a[4]);
		spx_int16_t *buf = malloc((size_t)fr * sizeof(spx_int16_t));
		for (int f = 0; f < n; f++) {
			double ri = 0, ro = 0;
			for (int i = 0; i < fr; i++) {
				float v = (float)gen_next(&s->g, rate);
				v = v > 1.0f ? 1.0f : v < -1.0f ? -1.0f : v;
				buf[i] = (spx_int16_t)(v * 32767.0f);
				ri += (double)buf[i] * buf[i];
			}
			speex_preprocess_run(s->st, buf);
			for (int i = 0; i < fr; i++)
				ro += (double)buf[i] * buf[i];
			if (f % every != 0 && f != n - 1)
				continue;
			printf("spx %s frame=%d rms_in=%.0f rms_out=%.0f\n", a[1], f, sqrt(ri / fr), sqrt(ro / fr));
		}
		free(buf);
	} else if (!strcmp(c, "module")) {
		nreg = 0;
		bool ok = filtersrest_module_load();
		printf("module description=\"%s\" load=%d registered=%d\n", filtersrest_module_description(), ok, nreg);
		for (int i = 0; i < nreg; i++)
			printf("  reg %d id=%s version=%u type=%d flags=0x%x\n", i, reg[i]->id, reg[i]->version, (int)reg[i]->type,
			       reg[i]->output_flags);
	} else if (!strcmp(c, "ns_create")) { /* ns_create ID v1|v2 METHOD LEVEL [mono-parent] */
		script_need(sl, 5);
		struct fi *f = NULL;
		for (int i = 0; i < MAXS; i++)
			if (!fis[i].used) { f = &fis[i]; break; }
		memset(f, 0, sizeof(*f));
		f->used = true;
		snprintf(f->id, sizeof(f->id), "%s", a[1]);
		f->info = !strcmp(a[2], "v1") ? &noise_suppress_filter : &noise_suppress_filter_v2;
		snprintf(f->ctx.name, sizeof(f->ctx.name), "ns-%s", a[1]);
		f->ctx.parent = &f->parent;
		snprintf(f->parent.name, sizeof(f->parent.name), "parent");
		f->parent.layout = (sl->argc > 5 && !strcmp(a[5], "mono-parent")) ? SPEAKERS_MONO : SPEAKERS_STEREO;
		obs_data_t *d = obs_data_create();
		f->info->get_defaults(d);
		if (strcmp(a[3], "-")) obs_data_set_string(d, "method", a[3]);
		obs_data_set_int(d, "suppress_level", atoi(a[4]));
		f->data = f->info->create(d, (obs_source_t *)&f->ctx);
		obs_data_release(d);
		struct noise_suppress_data *ng = f->data;
		printf("ns_create %s frames=%zu channels=%zu use_rnnoise=%d latency=%llu resampler=%d\n", a[1], ng->frames,
		       ng->channels, ng->use_rnnoise, (unsigned long long)ng->latency, ng->rnn_resampler != NULL);
	} else if (!strcmp(c, "ns_gen")) { /* ns_gen ID CH KIND ... (amplitudes in float units, full scale 1.0) */
		script_need(sl, 4);
		parse_gen(&ffind(a[1])->g[atoi(a[2])], a + 3, sl->argc - 3);
	} else if (!strcmp(c, "ns_process")) { /* ns_process ID FRAMES TS : generate FRAMES per channel, call filter_audio */
		script_need(sl, 4);
		struct fi *f = ffind(a[1]);
		struct noise_suppress_data *ng = f->data;
		uint32_t frames = (uint32_t)atoi(a[2]);
		float *buf[8];
		struct obs_audio_data ad = {0};
		for (size_t ch = 0; ch < ng->channels; ch++) {
			buf[ch] = malloc(frames * sizeof(float));
			for (uint32_t i = 0; i < frames; i++)
				buf[ch][i] = (float)gen_next(&f->g[ch], (double)g_rate);
			ad.data[ch] = (uint8_t *)buf[ch];
		}
		ad.frames = frames;
		ad.timestamp = strtoull(a[3], NULL, 10);
		double rin = rms(buf[0], frames);
		struct obs_audio_data *out = f->info->filter_audio(f->data, &ad);
		printf("ns_process %s in_frames=%u in_ts=%llu ->", a[1], frames, (unsigned long long)ad.timestamp);
		if (!out)
			printf(" null");
		else {
			printf(" frames=%u ts=%llu", out->frames, (unsigned long long)out->timestamp);
			for (size_t ch = 0; ch < ng->channels; ch++) {
				const float *o = (const float *)out->data[ch];
				printf(" rms%zu=", ch);
				pf(rms(o, out->frames), "%.3f");
			}
		}
		printf(" rms_in0=");
		pf(rin, "%.3f");
		printf(" queued_in=%zu queued_out=%zu\n", ng->input_buffers[0].size / 4, ng->output_buffers[0].size / 4);
		for (size_t ch = 0; ch < ng->channels; ch++)
			free(buf[ch]);
	} else if (!strcmp(c, "ns_destroy")) {
		script_need(sl, 2);
		struct fi *f = ffind(a[1]);
		f->info->destroy(f->data);
		f->used = false;
		printf("ns_destroy %s\n", a[1]);
	} else {
		fprintf(stderr, "line %d: unknown command %s\n", sl->lineno, c);
		exit(2);
	}
}

static void do_try(struct script_line *sl)
{
	struct script_line sub = *sl;
	sub.argc--;
	for (int i = 0; i < sub.argc; i++)
		sub.argv[i] = sl->argv[i + 1];
	fflush(stdout);
	printf("try-begin\n");
	fflush(stdout);
	pid_t p = fork();
	if (p == 0) {
		alarm(20);
		run_cmd(&sub);
		fflush(stdout);
		_exit(0);
	}
	int st = 0;
	waitpid(p, &st, 0);
	printf("try-end %s\n", WIFEXITED(st) && WEXITSTATUS(st) == 0 ? "ok" : WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM ? "hang" : "crash");
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	while (script_next(f, &sl, &lineno))
		run_cmd(&sl);
	return 0;
}
