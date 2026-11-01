/*
 * Conformance runner for spec S-044 (video-io.c / audio-io.c). The real libobs files are #included so that the
 * real thread loops run; the clock, sleep, profiler hooks, scaler and resampler are fakes (avio_stubs.c) and the
 * driver synchronises with the io threads through semaphores so that every script is deterministic.
 * Script language: see test/conformance/README.md ("avio runner").
 */
#include <math.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "harness.h"
#include "avio_stubs.h"

#include "media-io/video-io.c"
#include "media-io/audio-io.c"

#define NV 8
static struct {
	char name[16];
	video_t *v;
	bool child;
} vtab[NV];

static video_t *V(const char *n)
{
	for (int i = 0; i < NV; i++)
		if (vtab[i].v && !strcmp(vtab[i].name, n))
			return vtab[i].v;
	if (!strcmp(n, "-"))
		return NULL;
	fprintf(stderr, "unknown video %s\n", n);
	exit(2);
}

static void vadd(const char *n, video_t *v, bool child)
{
	for (int i = 0; i < NV; i++)
		if (!vtab[i].v) {
			snprintf(vtab[i].name, sizeof(vtab[i].name), "%s", n);
			vtab[i].v = v;
			vtab[i].child = child;
			return;
		}
	exit(2);
}

static void vdel(video_t *v)
{
	for (int i = 0; i < NV; i++)
		if (vtab[i].v == v)
			vtab[i].v = NULL;
}

#define E(x) {#x, x}
struct nv {
	const char *n;
	int v;
};
static const struct nv vfmts[] = {
	E(VIDEO_FORMAT_NONE), E(VIDEO_FORMAT_I420), E(VIDEO_FORMAT_NV12), E(VIDEO_FORMAT_YVYU), E(VIDEO_FORMAT_YUY2),
	E(VIDEO_FORMAT_UYVY), E(VIDEO_FORMAT_RGBA), E(VIDEO_FORMAT_BGRA),  E(VIDEO_FORMAT_BGRX), E(VIDEO_FORMAT_Y800),
	E(VIDEO_FORMAT_I444), E(VIDEO_FORMAT_BGR3), E(VIDEO_FORMAT_I422),  E(VIDEO_FORMAT_P010), E(VIDEO_FORMAT_I010),
	{0, 0}};
static const struct nv afmts[] = {E(AUDIO_FORMAT_UNKNOWN),     E(AUDIO_FORMAT_U8BIT),       E(AUDIO_FORMAT_16BIT),
				  E(AUDIO_FORMAT_32BIT),       E(AUDIO_FORMAT_FLOAT),       E(AUDIO_FORMAT_U8BIT_PLANAR),
				  E(AUDIO_FORMAT_16BIT_PLANAR), E(AUDIO_FORMAT_32BIT_PLANAR), E(AUDIO_FORMAT_FLOAT_PLANAR),
				  {0, 0}};
static const struct nv spks[] = {E(SPEAKERS_UNKNOWN), E(SPEAKERS_MONO),    E(SPEAKERS_STEREO),
				 E(SPEAKERS_2POINT1), E(SPEAKERS_4POINT0), E(SPEAKERS_4POINT1),
				 E(SPEAKERS_5POINT1), E(SPEAKERS_7POINT1), {0, 0}};
static const struct nv ranges[] = {E(VIDEO_RANGE_DEFAULT), E(VIDEO_RANGE_PARTIAL), E(VIDEO_RANGE_FULL), {0, 0}};
static const struct nv spaces[] = {E(VIDEO_CS_DEFAULT), E(VIDEO_CS_601),      E(VIDEO_CS_709),
				   E(VIDEO_CS_SRGB),    E(VIDEO_CS_2100_PQ), E(VIDEO_CS_2100_HLG), {0, 0}};

/* "-" is 0; names may omit the prefix up to and including the first '_' pair (e.g. I420, FLOAT_PLANAR); numbers are accepted */
static int lookup(const struct nv *t, const char *s, const char *prefix)
{
	if (!strcmp(s, "-"))
		return 0;
	char *end;
	long num = strtol(s, &end, 10);
	if (*s && !*end)
		return (int)num;
	char full[64];
	snprintf(full, sizeof(full), "%s%s", prefix, s);
	for (; t->n; t++)
		if (!strcmp(t->n, full))
			return t->v;
	fprintf(stderr, "unknown name %s\n", s);
	exit(2);
}
#define VF(s) ((enum video_format)lookup(vfmts, s, "VIDEO_FORMAT_"))
#define AF(s) ((enum audio_format)lookup(afmts, s, "AUDIO_FORMAT_"))
#define SP(s) ((enum speaker_layout)lookup(spks, s, "SPEAKERS_"))
#define RG(s) ((enum video_range_type)lookup(ranges, s, "VIDEO_RANGE_"))
#define CS(s) ((enum video_colorspace)lookup(spaces, s, "VIDEO_CS_"))

static void pf(float v)
{
	if (v != v)
		fputs("nan", stdout);
	else if (isinf(v))
		fputs(v < 0 ? "-inf" : "inf", stdout);
	else
		printf("%.9g", v == 0 ? 0.0f : v);
}

static float pfloat(const char *s)
{
	return strtof(s, NULL);
}

/* ------------------------------------------------------------------------------------------------ video */

static sem_t v_entered, v_gate;
static bool v_gate_on;
struct hook {
	bool used;
	int cb;
	long param;
	int action; /* 1 = disconnect self, 2 = disconnect (cb2,param2) */
	int cb2;
	long param2;
};
static struct hook vhooks[8];
static video_t *v_hook_video;

/* A hook is keyed by (callback, param); re-registering replaces it ("none" disables it). */
static struct hook *find_hook(struct hook *tab, int cb, long param)
{
	struct hook *free_slot = NULL;
	for (int i = 0; i < 8; i++) {
		if (tab[i].cb == cb && tab[i].param == param && (tab[i].used || tab[i].action))
			return &tab[i];
		if (!tab[i].used && !tab[i].action && !free_slot)
			free_slot = &tab[i];
	}
	return free_slot;
}

static void vcb_a(void *param, struct video_data *f);
static void vcb_b(void *param, struct video_data *f);
static void (*vcbs[2])(void *, struct video_data *) = {vcb_a, vcb_b};
static int cbid(const char *s)
{
	return s[0] == 'b' ? 1 : 0;
}

static void vcb_common(int id, void *param, struct video_data *f)
{
	printf("vcb %c param=%ld ts=%llu ls0=%u d0=", 'a' + id, (long)(intptr_t)param, (unsigned long long)f->timestamp,
	       f->linesize[0]);
	if (f->data[0])
		printf("%02x%02x", f->data[0][0], f->data[0][1]);
	else
		fputs("null", stdout);
	/* identify where the data lives */
	const char *kind = "other";
	char buf[32] = "";
	for (int t = 0; t < NV; t++) {
		video_t *v = vtab[t].v;
		if (!v || v->parent)
			continue;
		for (size_t i = 0; i < v->info.cache_size; i++)
			if (v->cache[i].frame.data[0] == f->data[0]) {
				snprintf(buf, sizeof(buf), "cache%zu", i);
				kind = buf;
			}
		for (size_t i = 0; i < v->inputs.num; i++)
			for (int k = 0; k < MAX_CONVERT_BUFFERS; k++)
				if (v->inputs.array[i].frame[k].data[0] == f->data[0] && f->data[0]) {
					snprintf(buf, sizeof(buf), "conv%zu.%d", i, k);
					kind = buf;
				}
	}
	printf(" src=%s\n", kind);
	for (int i = 0; i < 8; i++) {
		struct hook *h = &vhooks[i];
		if (h->used && h->cb == id && h->param == (long)(intptr_t)param) {
			if (h->action == 1)
				printf("hook disconnect_self -> %d\n", video_output_disconnect2(v_hook_video, vcbs[id], param));
			else if (h->action == 2)
				printf("hook disconnect_other -> %d\n",
				       video_output_disconnect2(v_hook_video, vcbs[h->cb2], (void *)(intptr_t)h->param2));
		}
	}
	if (v_gate_on) {
		sem_post(&v_entered);
		while (sem_wait(&v_gate) != 0)
			;
	}
}
static void vcb_a(void *p, struct video_data *f)
{
	vcb_common(0, p, f);
}
static void vcb_b(void *p, struct video_data *f)
{
	vcb_common(1, p, f);
}

static void vwait_n(int n)
{
	for (int i = 0; i < n; i++)
		while (sem_wait(&avio_video_done) != 0)
			;
}

static void vdump(video_t *v)
{
	printf("vdump first=%zu last=%zu avail=%zu skipped=%ld total=%ld inputs=%zu raw=%d gpu=%ld stop=%d\n",
	       v->first_added, v->last_added, v->available_frames, v->skipped_frames, v->total_frames, v->inputs.num,
	       (int)v->raw_active, v->gpu_refs, (int)v->stop);
	for (size_t i = 0; i < v->info.cache_size; i++)
		printf("  slot%zu count=%d skipped=%d ts=%llu\n", i, v->cache[i].count, v->cache[i].skipped,
		       (unsigned long long)v->cache[i].frame.timestamp);
}

/* ------------------------------------------------------------------------------------------------ audio */

static audio_t *A;
static char aname[32];
static bool in_ret = true;
static int in_newts_mode; /* 0 = leave untouched, 1 = end + delta, 2 = absolute */
static int64_t in_newts_val;
struct feed {
	int mix, plane;
	size_t start, count;
	float val;
	bool add;
};
static struct feed feeds[16];
static int nfeeds;
static size_t probes[8] = {0, 1, 2, 3};
static int nprobes = 4;
static struct hook ahooks[8];

static void aout_a(void *param, size_t mix, struct audio_data *d);
static void aout_b(void *param, size_t mix, struct audio_data *d);
static audio_output_callback_t aouts[2] = {aout_a, aout_b};

static bool ain_cb(void *param, uint64_t start_ts, uint64_t end_ts, uint64_t *new_ts, uint32_t active,
		   struct audio_output_data *mixes)
{
	(void)param;
	if (avio_shutdown)
		return false;
	printf("in start=%llu end=%llu active=0x%02x nonnull=[", (unsigned long long)start_ts, (unsigned long long)end_ts,
	       active);
	bool clean = true;
	for (int m = 0; m < MAX_AUDIO_MIXES; m++) {
		int nn = 0;
		for (int p = 0; p < MAX_AUDIO_CHANNELS; p++)
			if (mixes[m].data[p]) {
				nn++;
				for (int i = 0; i < AUDIO_OUTPUT_FRAMES; i++)
					if (mixes[m].data[p][i] != 0.0f)
						clean = false;
			}
		printf("%s%d", m ? "," : "", nn);
	}
	printf("] clean=%s\n", clean ? "yes" : "no");
	for (int i = 0; i < nfeeds; i++) {
		struct feed *f = &feeds[i];
		float *buf = mixes[f->mix].data[f->plane];
		if (!buf)
			continue;
		for (size_t k = f->start; k < f->start + f->count && k < AUDIO_OUTPUT_FRAMES * 2; k++)
			buf[k] = f->add ? buf[k] + f->val : f->val;
	}
	if (in_newts_mode == 1)
		*new_ts = end_ts + (uint64_t)in_newts_val;
	else if (in_newts_mode == 2)
		*new_ts = (uint64_t)in_newts_val;
	return in_ret;
}

static void aout_common(int id, void *param, size_t mix, struct audio_data *d)
{
	const char *kind = "resampled";
	if ((float *)d->data[0] == A->mixes[mix].buffer[0])
		kind = "clamped";
	else if ((float *)d->data[0] == A->mixes[mix].buffer_unclamped[0])
		kind = "unclamped";
	printf("out %c param=%ld mix=%zu ts=%llu frames=%u buf=%s", 'a' + id, (long)(intptr_t)param, mix,
	       (unsigned long long)d->timestamp, d->frames, kind);
	for (int p = 0; p < 2; p++) {
		if (p && !(A->planes > 1 || strcmp(kind, "resampled") == 0))
			break;
		float *f = (float *)d->data[p];
		if (!f) {
			printf(" p%d=null", p);
			continue;
		}
		printf(" p%d=[", p);
		for (int i = 0; i < nprobes; i++) {
			if (i)
				putchar(' ');
			pf(f[probes[i]]);
		}
		putchar(']');
	}
	putchar('\n');
	for (int i = 0; i < 8; i++) {
		struct hook *h = &ahooks[i];
		if (h->used && h->cb == id && h->param == (long)(intptr_t)param) {
			if (h->action == 1) {
				audio_output_disconnect(A, mix, aouts[id], param);
				puts("hook disconnect_self");
			} else if (h->action == 2) {
				audio_output_disconnect(A, mix, aouts[h->cb2], (void *)(intptr_t)h->param2);
				puts("hook disconnect_other");
			}
		}
	}
}
static void aout_a(void *p, size_t m, struct audio_data *d)
{
	aout_common(0, p, m, d);
}
static void aout_b(void *p, size_t m, struct audio_data *d)
{
	aout_common(1, p, m, d);
}

static void atick(void)
{
	sem_post(&avio_audio_go);
	while (sem_wait(&avio_audio_done) != 0)
		;
	printf("tick sleepto=%llu\n", (unsigned long long)avio_consumed_target);
}

static void adump(void)
{
	printf("adump");
	for (int m = 0; m < MAX_AUDIO_MIXES; m++) {
		printf(" mix%d=[", m);
		for (size_t i = 0; i < A->mixes[m].inputs.num; i++) {
			struct audio_input *in = &A->mixes[m].inputs.array[i];
			printf("%s(%s %ld rate=%u fmt=%d spk=%d clip=%d rs=%d)", i ? " " : "",
			       in->callback == aout_a ? "a" : "b", (long)(intptr_t)in->param, in->conversion.samples_per_sec,
			       in->conversion.format, in->conversion.speakers, in->conversion.allow_clipping,
			       in->resampler != NULL);
		}
		putchar(']');
	}
	putchar('\n');
}

/* ------------------------------------------------------------------------------------------------ misc */

static void crash_probe(const char *what)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		volatile uint64_t sink = 0;
		if (!strcmp(what, "video_connect_null"))
			sink = video_output_connect(NULL, NULL, vcb_a, NULL);
		else if (!strcmp(what, "video_connect2_null"))
			sink = video_output_connect2(NULL, NULL, 1, vcb_a, NULL);
		else if (!strcmp(what, "video_skipped_null"))
			sink = video_output_get_skipped_frames(NULL);
		else if (!strcmp(what, "video_total_null"))
			sink = video_output_get_total_frames(NULL);
		else if (!strcmp(what, "video_inc_texture_frames_null"))
			video_output_inc_texture_frames(NULL);
		else if (!strcmp(what, "video_inc_texture_skipped_null"))
			video_output_inc_texture_skipped_frames(NULL);
		else if (!strcmp(what, "video_inc_encoders_null"))
			video_output_inc_texture_encoders(NULL);
		else if (!strcmp(what, "video_dec_encoders_null"))
			video_output_dec_texture_encoders(NULL);
		else if (!strcmp(what, "audio_block_size_null"))
			sink = audio_output_get_block_size(NULL);
		else if (!strcmp(what, "audio_planes_null"))
			sink = audio_output_get_planes(NULL);
		else if (!strcmp(what, "audio_channels_null"))
			sink = audio_output_get_channels(NULL);
		else if (!strcmp(what, "audio_rate_null"))
			sink = audio_output_get_sample_rate(NULL);
		else
			_exit(3);
		(void)sink;
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFSIGNALED(st))
		printf("crash %s signal=%d\n", what, WTERMSIG(st));
	else
		printf("nocrash %s exit=%d\n", what, WEXITSTATUS(st));
}

static struct video_scale_info parse_conv(char **a)
{
	struct video_scale_info c = {.format = VF(a[0]),
				     .width = (uint32_t)atoi(a[1]),
				     .height = (uint32_t)atoi(a[2]),
				     .range = RG(a[3]),
				     .colorspace = CS(a[4])};
	return c;
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	sem_init(&avio_audio_go, 0, 0);
	sem_init(&avio_audio_done, 0, 0);
	sem_init(&avio_clock_read, 0, 0);
	sem_init(&avio_video_done, 0, 0);
	sem_init(&v_entered, 0, 0);
	sem_init(&v_gate, 0, 0);

	while (script_next(f, &sl, &lineno)) {
		char **a = sl.argv;
		const char *c = a[0];
		/* echo the command so that the expected file is readable */
		printf("> %s", c);
		for (int i = 1; i < sl.argc; i++)
			printf(" %s", a[i]);
		putchar('\n');

		/* ---------------- video ---------------- */
		if (!strcmp(c, "vopen")) { /* name fmt num den w h cache [range cs] */
			script_need(&sl, 8);
			struct video_output_info info = {.name = "avio-test",
							 .format = VF(a[2]),
							 .fps_num = (uint32_t)atoi(a[3]),
							 .fps_den = (uint32_t)atoi(a[4]),
							 .width = (uint32_t)atoi(a[5]),
							 .height = (uint32_t)atoi(a[6]),
							 .cache_size = (size_t)atoi(a[7]),
							 .range = sl.argc > 8 ? RG(a[8]) : VIDEO_RANGE_DEFAULT,
							 .colorspace = sl.argc > 9 ? CS(a[9]) : VIDEO_CS_DEFAULT};
			video_t *v = NULL;
			int rc = video_output_open(&v, &info);
			printf("= %d\n", rc);
			if (rc == 0)
				vadd(a[1], v, false);
		} else if (!strcmp(c, "vclose")) {
			script_need(&sl, 2);
			video_t *v = V(a[1]);
			video_output_close(v);
			vdel(v);
			puts("= closed");
		} else if (!strcmp(c, "vinfo")) {
			script_need(&sl, 2);
			video_t *v = V(a[1]);
			const struct video_output_info *i = video_output_get_info(v);
			printf("info fmt=%d %ux%u fps=%u/%u cache=%zu range=%d cs=%d\n", i->format, i->width, i->height,
			       i->fps_num, i->fps_den, i->cache_size, i->range, i->colorspace);
			printf("get fmt=%d w=%u h=%u rate=%.17g frame_time=%llu\n", video_output_get_format(v),
			       video_output_get_width(v), video_output_get_height(v), video_output_get_frame_rate(v),
			       (unsigned long long)video_output_get_frame_time(v));
		} else if (!strcmp(c, "vnull")) { /* getters on a NULL video */
			printf("null info=%d active=%d stopped=%d fmt=%d w=%u h=%u rate=%.17g ft=%llu\n",
			       video_output_get_info(NULL) != NULL, video_output_active(NULL), video_output_stopped(NULL),
			       video_output_get_format(NULL), video_output_get_width(NULL), video_output_get_height(NULL),
			       video_output_get_frame_rate(NULL), (unsigned long long)video_output_get_frame_time(NULL));
			struct video_frame fr;
			printf("null lock=%d connect_cb_null=%d disconnect_null=%d create_div_null=%d\n",
			       video_output_lock_frame(NULL, &fr, 1, 0), video_output_connect(vtab[0].v, NULL, NULL, NULL),
			       video_output_disconnect2(NULL, vcb_a, NULL), video_output_create_with_frame_rate_divisor(NULL, 2) != NULL);
			video_output_unlock_frame(NULL);
			video_output_stop(NULL);
			video_output_close(NULL);
			video_output_free_frame_rate_divisor(NULL);
			puts("null done");
		} else if (!strcmp(c, "vconnect") || !strcmp(c, "vconnect2")) {
			/* vconnect V cb param [divisor|-] [fmt w h range cs] ; "vconnect2" always passes the divisor */
			script_need(&sl, 4);
			video_t *v = V(a[1]);
			int id = cbid(a[2]);
			long param = atol(a[3]);
			uint32_t div = sl.argc > 4 && strcmp(a[4], "-") ? (uint32_t)atoi(a[4]) : 1;
			struct video_scale_info conv;
			bool has = sl.argc >= 10;
			if (has)
				conv = parse_conv(a + 5);
			if (!v_hook_video)
				v_hook_video = v;
			bool ok = !strcmp(c, "vconnect") && div == 1
					  ? video_output_connect(v, has ? &conv : NULL, vcbs[id], (void *)(intptr_t)param)
					  : video_output_connect2(v, has ? &conv : NULL, div, vcbs[id], (void *)(intptr_t)param);
			printf("= %d\n", ok);
		} else if (!strcmp(c, "vconnect_nocb")) {
			script_need(&sl, 2);
			printf("= %d\n", video_output_connect2(V(a[1]), NULL, 1, NULL, NULL));
		} else if (!strcmp(c, "vdisconnect") || !strcmp(c, "vdisconnect2")) {
			script_need(&sl, 4);
			video_t *v = V(a[1]);
			int id = cbid(a[2]);
			if (!strcmp(c, "vdisconnect")) {
				video_output_disconnect(v, vcbs[id], (void *)(intptr_t)atol(a[3]));
				puts("= void");
			} else {
				printf("= %d\n", video_output_disconnect2(v, vcbs[id], (void *)(intptr_t)atol(a[3])));
			}
		} else if (!strcmp(c, "vdisconnect_nocb")) {
			script_need(&sl, 2);
			printf("= %d\n", video_output_disconnect2(V(a[1]), NULL, NULL));
		} else if (!strcmp(c, "vlock")) { /* V count ts [fill] */
			script_need(&sl, 4);
			video_t *v = V(a[1]);
			struct video_frame fr;
			memset(&fr, 0, sizeof(fr));
			bool ok = video_output_lock_frame(v, &fr, atoi(a[2]), strtoull(a[3], NULL, 10));
			int slot = -1;
			video_t *root = v->parent ? v->parent : v;
			for (size_t i = 0; ok && i < root->info.cache_size; i++)
				if (root->cache[i].frame.data[0] == fr.data[0])
					slot = (int)i;
			printf("= %d slot=%d\n", ok, slot);
			if (ok && sl.argc > 4 && fr.data[0])
				fr.data[0][0] = (uint8_t)strtol(a[4], NULL, 0), fr.data[0][1] = 0x5a;
		} else if (!strcmp(c, "vunlock")) { /* synchronous: one thread iteration */
			script_need(&sl, 2);
			video_output_unlock_frame(V(a[1]));
			vwait_n(1);
			puts("= done");
		} else if (!strcmp(c, "vunlock_async")) {
			script_need(&sl, 2);
			video_output_unlock_frame(V(a[1]));
			puts("= posted");
		} else if (!strcmp(c, "vwait")) {
			script_need(&sl, 2);
			vwait_n(atoi(a[1]));
			puts("= done");
		} else if (!strcmp(c, "vgate")) {
			script_need(&sl, 2);
			v_gate_on = !strcmp(a[1], "on");
		} else if (!strcmp(c, "venter")) {
			while (sem_wait(&v_entered) != 0)
				;
			puts("= entered");
		} else if (!strcmp(c, "vrelease")) {
			script_need(&sl, 2);
			for (int i = 0; i < atoi(a[1]); i++)
				sem_post(&v_gate);
		} else if (!strcmp(c, "vhook")) { /* cb param none|disconnect_self|disconnect cb2 param2 */
			script_need(&sl, 4);
			struct hook *h = find_hook(vhooks, cbid(a[1]), atol(a[2]));
			h->used = strcmp(a[3], "none") != 0;
			h->cb = cbid(a[1]);
			h->param = atol(a[2]);
			h->action = !strcmp(a[3], "disconnect_self") ? 1 : 2;
			if (sl.argc > 5) {
				h->cb2 = cbid(a[4]);
				h->param2 = atol(a[5]);
			}
		} else if (!strcmp(c, "vstop")) {
			script_need(&sl, 2);
			video_output_stop(V(a[1]));
			puts("= stopped");
		} else if (!strcmp(c, "vstate")) {
			script_need(&sl, 2);
			video_t *v = V(a[1]);
			printf("active=%d stopped=%d skipped=%u total=%u\n", video_output_active(v), video_output_stopped(v),
			       video_output_get_skipped_frames(v), video_output_get_total_frames(v));
		} else if (!strcmp(c, "vdump")) {
			script_need(&sl, 2);
			video_t *v = V(a[1]);
			vdump(v->parent ? v->parent : v);
		} else if (!strcmp(c, "vinputs")) {
			script_need(&sl, 2);
			video_t *v = V(a[1]);
			v = v->parent ? v->parent : v;
			for (size_t i = 0; i < v->inputs.num; i++) {
				struct video_input *in = &v->inputs.array[i];
				printf("input%zu cb=%c param=%ld div=%u ctr=%u scaler=%d conv=(%d %ux%u r%d cs%d) cur=%d\n", i,
				       in->callback == vcb_a ? 'a' : 'b', (long)(intptr_t)in->param, in->frame_rate_divisor,
				       in->frame_rate_divisor_counter, in->scaler != NULL, in->conversion.format,
				       in->conversion.width, in->conversion.height, in->conversion.range,
				       in->conversion.colorspace, in->cur_frame);
			}
			if (!v->inputs.num)
				puts("no inputs");
		} else if (!strcmp(c, "vtex")) { /* V inc_enc|dec_enc|frames|skipped */
			script_need(&sl, 3);
			video_t *v = V(a[1]);
			if (!strcmp(a[2], "inc_enc"))
				video_output_inc_texture_encoders(v);
			else if (!strcmp(a[2], "dec_enc"))
				video_output_dec_texture_encoders(v);
			else if (!strcmp(a[2], "frames"))
				video_output_inc_texture_frames(v);
			else
				video_output_inc_texture_skipped_frames(v);
			puts("= void");
		} else if (!strcmp(c, "vdivider")) { /* newname V divisor */
			script_need(&sl, 4);
			video_t *n = video_output_create_with_frame_rate_divisor(V(a[2]), (uint32_t)atoi(a[3]));
			printf("= %d\n", n != NULL);
			if (n)
				vadd(a[1], n, true);
		} else if (!strcmp(c, "vfree_divider")) {
			script_need(&sl, 2);
			video_t *v = V(a[1]);
			video_output_free_frame_rate_divisor(v);
			vdel(v);
			puts("= void");
		} else if (!strcmp(c, "vfree_divider_root")) { /* no-op on a root */
			script_need(&sl, 2);
			video_output_free_frame_rate_divisor(V(a[1]));
			puts("= void");
		} else if (!strcmp(c, "scaler")) { /* create ok|bad|fail ; scale ok|fail */
			script_need(&sl, 3);
			if (!strcmp(a[1], "create"))
				avio_scaler_create_result = !strcmp(a[2], "ok") ? 0 : !strcmp(a[2], "bad") ? -1 : -2;
			else
				avio_scaler_scale_ok = !strcmp(a[2], "ok");

			/* ---------------- audio ---------------- */
		} else if (!strcmp(c, "aclock")) {
			script_need(&sl, 2);
			avio_fake_now = strtoull(a[1], NULL, 10);
		} else if (!strcmp(c, "aopen")) { /* name rate fmt spk */
			script_need(&sl, 5);
			snprintf(aname, sizeof(aname), "%s", strcmp(a[1], "-") ? a[1] : "");
			struct audio_output_info info = {.name = strcmp(a[1], "-") ? aname : NULL,
							 .samples_per_sec = (uint32_t)atoi(a[2]),
							 .format = AF(a[3]),
							 .speakers = SP(a[4]),
							 .input_callback = ain_cb};
			avio_audio_lockstep = true;
			avio_shutdown = false;
			audio_t *au = NULL;
			int rc = audio_output_open(&au, &info);
			printf("= %d\n", rc);
			if (rc == 0) {
				A = au;
				while (sem_wait(&avio_clock_read) != 0)
					;
			} else {
				avio_audio_lockstep = false;
			}
		} else if (!strcmp(c, "aclose")) {
			if (A) {
				avio_shutdown = true;
				os_event_signal(A->stop_event);
				sem_post(&avio_audio_go);
				audio_output_close(A);
				A = NULL;
				/* the shutdown tick leaves stale tokens behind */
				while (sem_trywait(&avio_audio_done) == 0)
					;
				while (sem_trywait(&avio_audio_go) == 0)
					;
			}
			avio_audio_lockstep = false;
			puts("= closed");
		} else if (!strcmp(c, "atick")) {
			int n = sl.argc > 1 ? atoi(a[1]) : 1;
			for (int i = 0; i < n; i++)
				atick();
		} else if (!strcmp(c, "ainfo")) {
			const struct audio_output_info *i = audio_output_get_info(A);
			printf("info name=%s rate=%u fmt=%d spk=%d cb=%d\n", i->name, i->samples_per_sec, i->format,
			       i->speakers, i->input_callback == ain_cb);
			printf("get block=%zu planes=%zu channels=%zu rate=%u active=%d\n", audio_output_get_block_size(A),
			       audio_output_get_planes(A), audio_output_get_channels(A), audio_output_get_sample_rate(A),
			       audio_output_active(A));
			printf("null info=%d active=%d\n", audio_output_get_info(NULL) != NULL, audio_output_active(NULL));
		} else if (!strcmp(c, "aconnect")) { /* cb param mix [rate fmt spk clip] */
			script_need(&sl, 4);
			struct audio_convert_info conv;
			bool has = sl.argc >= 8;
			if (has)
				conv = (struct audio_convert_info){.samples_per_sec = (uint32_t)atoi(a[4]),
								   .format = AF(a[5]),
								   .speakers = SP(a[6]),
								   .allow_clipping = atoi(a[7]) != 0};
			printf("= %d\n", audio_output_connect(A, (size_t)atoi(a[3]), has ? &conv : NULL, aouts[cbid(a[1])],
							      (void *)(intptr_t)atol(a[2])));
		} else if (!strcmp(c, "aconnect_null")) { /* connect on a NULL audio */
			printf("= %d\n", audio_output_connect(NULL, 0, NULL, aout_a, NULL));
			audio_output_disconnect(NULL, 0, aout_a, NULL);
		} else if (!strcmp(c, "adisconnect")) {
			script_need(&sl, 4);
			audio_output_disconnect(A, (size_t)atoi(a[3]), aouts[cbid(a[1])], (void *)(intptr_t)atol(a[2]));
			puts("= void");
		} else if (!strcmp(c, "adump")) {
			adump();
		} else if (!strcmp(c, "afeed")) { /* mix plane start count value set|add */
			script_need(&sl, 7);
			feeds[nfeeds++] = (struct feed){.mix = atoi(a[1]),
							.plane = atoi(a[2]),
							.start = (size_t)atoi(a[3]),
							.count = (size_t)atoi(a[4]),
							.val = pfloat(a[5]),
							.add = !strcmp(a[6], "add")};
		} else if (!strcmp(c, "afeed_clear")) {
			nfeeds = 0;
		} else if (!strcmp(c, "anewts")) { /* none | delta N | abs N */
			script_need(&sl, 2);
			in_newts_mode = !strcmp(a[1], "delta") ? 1 : !strcmp(a[1], "abs") ? 2 : 0;
			in_newts_val = sl.argc > 2 ? strtoll(a[2], NULL, 10) : 0;
		} else if (!strcmp(c, "ainret")) {
			script_need(&sl, 2);
			in_ret = !strcmp(a[1], "true");
		} else if (!strcmp(c, "aprobe")) {
			script_need(&sl, 2);
			nprobes = 0;
			for (int i = 1; i < sl.argc && nprobes < 8; i++)
				probes[nprobes++] = (size_t)atoi(a[i]);
		} else if (!strcmp(c, "ahook")) { /* cb param disconnect_self | disconnect cb2 param2 | none */
			script_need(&sl, 4);
			struct hook *h = find_hook(ahooks, cbid(a[1]), atol(a[2]));
			h->used = strcmp(a[3], "none") != 0;
			h->cb = cbid(a[1]);
			h->param = atol(a[2]);
			h->action = !strcmp(a[3], "disconnect_self") ? 1 : 2;
			if (sl.argc > 5) {
				h->cb2 = cbid(a[4]);
				h->param2 = atol(a[5]);
			}
		} else if (!strcmp(c, "resamp")) { /* create ok|fail ; resample ok|fail ; offset N ; frames N */
			script_need(&sl, 3);
			if (!strcmp(a[1], "create"))
				avio_resampler_create_ok = !strcmp(a[2], "ok");
			else if (!strcmp(a[1], "resample"))
				avio_resampler_resample_ok = !strcmp(a[2], "ok");
			else if (!strcmp(a[1], "offset"))
				avio_resampler_offset = strtoull(a[2], NULL, 10);
			else
				avio_resampler_out_frames = (uint32_t)atoi(a[2]);
		} else if (!strcmp(c, "crash")) {
			script_need(&sl, 2);
			crash_probe(a[1]);
		} else {
			fprintf(stderr, "line %d: unknown command %s\n", sl.lineno, c);
			return 2;
		}
	}
	if (A) {
		avio_shutdown = true;
		os_event_signal(A->stop_event);
		sem_post(&avio_audio_go);
		audio_output_close(A);
	}
	for (int i = 0; i < NV; i++)
		if (vtab[i].v && !vtab[i].child) {
			video_output_close(vtab[i].v);
		}
	fclose(f);
	return 0;
}
