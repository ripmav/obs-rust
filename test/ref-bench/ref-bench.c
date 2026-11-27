/*
 * P0-17 C baseline runner (WP-41).
 *
 * Measures the pinned 33.0.1 C reference workloads with a fixed workload +
 * a fixed number of timed runs (no adaptive iteration), and prints the
 * canonical bench JSON (schema 1 — see test/ref-bench/README.md) on stdout.
 *
 * Workloads:
 *   matmul4     — 1,000,000 dependent matrix4_mul over the pinned
 *                 libobs/graphics/matrix4.c. The multiplier is a bounded
 *                 affine matrix (rotation + small translation) so the
 *                 dependent chain never overflows f32.
 *   nv12_to_rgb — one 1920x1080 frame through decompress_nv12
 *                 (libobs/media-io/format-conversion.c): NV12 -> the packed
 *                 444 output, the CPU step of the NV12 -> RGB pipeline in
 *                 33.0.1 (the YUV -> RGB colour matrix is a shader there).
 *   audio_mix   — the REAL mix path: mix_audio (libobs/obs-audio.c:90-124,
 *                 33.0.1) copied VERBATIM with minimal stand-in types (the
 *                 ref-dump precedent, test/ref-dump/ref-dump-media.c).
 *                 Workload: 8 input sources ("8 tracks"), each contributing
 *                 AUDIO_OUTPUT_FRAMES (1024) frames at 48 kHz stereo into
 *                 all MAX_AUDIO_MIXES (6) mixes.
 *
 * No scene-render bench: the C per-item render path (obs-scene.c ->
 * obs_source_video_render) needs the full graphics runtime (texrender,
 * shaders) which this standalone build does not have (documented
 * deviation, README). The Rust-side stand-in lives in
 * crates/obs-math/benches/p017.rs and mirrors the per-item CPU math of
 * obs-scene.c (33.0.1: set_item_transform lines 657-661 + the per-frame
 * gs_matrix_mul(&item->draw_transform) at line 964).
 *
 * Usage: ref-bench [--list] [--runs N] [--bench NAME]
 *
 * Build: test/ref-bench/build.sh (env: LIBOBS, OUT, CC, CONFORMANCE_EXTRA_CFLAGS).
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "graphics/axisang.h"
#include "graphics/matrix4.h"
#include "media-io/audio-io.h"
#include "media-io/format-conversion.h"
#include "util/util_uint64.h"

#define MMUL_ITERS 1000000u
#define NV12_W 1920u
#define NV12_H 1080u
#define MIX_SOURCES 8
#define MIX_CHANNELS 2
#define SAMPLE_RATE 48000
#define DEFAULT_RUNS 30
#define WARMUP_RUNS 2

/* ------------------------------------------------------------------ */
/* audio_mix: the real mix path                                        */
/* ------------------------------------------------------------------ */

/* mix_audio is `static inline` in libobs/obs-audio.c (33.0.1, lines
 * 90-124) and is the function the production audio tick calls per
 * source. Verbatim copy of the function body (same precedent as
 * test/ref-dump/ref-dump-media.c); `struct audio_output_data` and the
 * MAX_AUDIO_MIXES/MAX_AUDIO_CHANNELS/AUDIO_OUTPUT_FRAMES constants are
 * the real ones from media-io/audio-io.h. Only `struct obs_source_t` is
 * a stand-in with the two fields mix_audio touches. */
struct rb_source
{
	uint64_t audio_ts;
	float *audio_output_buf[MAX_AUDIO_MIXES][MAX_AUDIO_CHANNELS];
};
struct rb_ts_info
{
	uint64_t start;
	uint64_t end;
};
static size_t convert_time_to_frames(size_t sample_rate, uint64_t t)
{
	return (size_t)util_mul_div64(t, sample_rate, 1000000000ULL);
}
static void mix_audio(struct audio_output_data *mixes, struct rb_source *source, size_t channels, size_t sample_rate,
		      struct rb_ts_info *ts)
{
	size_t total_floats = AUDIO_OUTPUT_FRAMES;
	size_t start_point = 0;

	if (source->audio_ts < ts->start || ts->end <= source->audio_ts)
		return;

	if (source->audio_ts != ts->start) {
		start_point = convert_time_to_frames(sample_rate, source->audio_ts - ts->start);
		if (start_point == AUDIO_OUTPUT_FRAMES)
			return;

		total_floats -= start_point;
	}

	for (size_t mix_idx = 0; mix_idx < MAX_AUDIO_MIXES; mix_idx++) {
		for (size_t ch = 0; ch < channels; ch++) {
			register float *mix = mixes[mix_idx].data[ch];
			register float *aud = source->audio_output_buf[mix_idx][ch];
			register float *end;

			mix += start_point;
			end = aud + total_floats;

			while (aud < end)
				*(mix++) += *(aud++);
		}
	}
}

/* ------------------------------------------------------------------ */
/* workload data (deterministic, no libm — ULP-portable)               */
/* ------------------------------------------------------------------ */

static struct matrix4 mmul_a;
static volatile float sink;

static uint8_t nv12_y[NV12_W * NV12_H];
static uint8_t nv12_uv[NV12_W * NV12_H / 2];
static uint8_t packed444[NV12_W * NV12_H * 4]; // packed 444: 4 bytes/pixel

static float src_buf[MIX_SOURCES][MAX_AUDIO_MIXES][MIX_CHANNELS][AUDIO_OUTPUT_FRAMES];
static float mix_buf[MAX_AUDIO_MIXES][MIX_CHANNELS][AUDIO_OUTPUT_FRAMES];
static struct rb_source sources[MIX_SOURCES];
static struct audio_output_data mixes[MAX_AUDIO_MIXES];
static volatile float audio_sink;

static void init_data(void)
{
	/* matmul4: bounded affine (rotation 1.0 rad about (0.3,0.5,0.7)
	 * + small translation) — powers stay bounded (rotation block
	 * orthogonal, translation converges to a fixed point). */
	struct axisang aa = {.x = 0.3f, .y = 0.5f, .z = 0.7f, .w = 1.0f};
	matrix4_from_axisang(&mmul_a, &aa);
	mmul_a.t.x = 0.1f;
	mmul_a.t.y = 0.2f;
	mmul_a.t.z = 0.05f;

	/* nv12_to_rgb: deterministic gradient (NV12: Y plane + interleaved
	 * UV plane, both W bytes per row). */
	for (uint32_t y = 0; y < NV12_H; y++)
		for (uint32_t x = 0; x < NV12_W; x++)
			nv12_y[y * NV12_W + x] = (uint8_t)((x * 7u + y * 13u) & 0xffu);
	for (uint32_t y = 0; y < NV12_H / 2; y++)
		for (uint32_t x = 0; x < NV12_W / 2; x++) {
			nv12_uv[y * NV12_W + x * 2] = (uint8_t)((x * 5u + y * 11u + 128u) & 0xffu);
			nv12_uv[y * NV12_W + x * 2 + 1] = (uint8_t)((x * 9u + y * 3u + 128u) & 0xffu);
		}

	/* audio_mix: bounded deterministic ramp per (source, mix, channel,
	 * frame) in [-0.5, 0.5). */
	for (uint32_t s = 0; s < MIX_SOURCES; s++)
		for (uint32_t m = 0; m < MAX_AUDIO_MIXES; m++)
			for (uint32_t c = 0; c < MIX_CHANNELS; c++)
				for (uint32_t f = 0; f < AUDIO_OUTPUT_FRAMES; f++)
					src_buf[s][m][c][f] = (((s * 131u + m * 17u + c * 7u + f) % 256u) / 255.0f) -
							       0.5f;
	for (uint32_t s = 0; s < MIX_SOURCES; s++) {
		for (uint32_t m = 0; m < MAX_AUDIO_MIXES; m++)
			for (uint32_t c = 0; c < MIX_CHANNELS; c++)
				sources[s].audio_output_buf[m][c] = &src_buf[s][m][c][0];
		sources[s].audio_ts = 0; /* aligned to the window start */
	}
	for (uint32_t m = 0; m < MAX_AUDIO_MIXES; m++)
		for (uint32_t c = 0; c < MIX_CHANNELS; c++)
			mixes[m].data[c] = &mix_buf[m][c][0];
}

/* ------------------------------------------------------------------ */
/* the three workloads                                                 */
/* ------------------------------------------------------------------ */

static uint64_t work_matmul4(void)
{
	struct matrix4 m = mmul_a;
	for (uint32_t i = 0; i < MMUL_ITERS; i++)
		matrix4_mul(&m, &m, &mmul_a);
	sink += m.t.w; /* defeat elimination */
	return MMUL_ITERS;
}

static uint64_t work_nv12_to_rgb(void)
{
	const uint8_t *input[2] = {nv12_y, nv12_uv};
	const uint32_t in_linesize[2] = {NV12_W, NV12_W};
	decompress_nv12(input, in_linesize, 0, NV12_H, packed444, NV12_W * 4);
	sink += (float)packed444[0]; /* keep the call observable */
	return 1; /* one 1080p frame */
}

static uint64_t work_audio_mix(void)
{
	struct rb_ts_info ts = {0, (uint64_t)AUDIO_OUTPUT_FRAMES * 1000000000ULL / SAMPLE_RATE};
	memset(mix_buf, 0, sizeof(mix_buf)); /* zeroed per tick, as in production */
	for (uint32_t s = 0; s < MIX_SOURCES; s++)
		mix_audio(mixes, &sources[s], MIX_CHANNELS, SAMPLE_RATE, &ts);
	audio_sink += mix_buf[0][0][0] + mix_buf[MAX_AUDIO_MIXES - 1][MIX_CHANNELS - 1][AUDIO_OUTPUT_FRAMES - 1];
	return 1; /* one 8-track x 1024-frame mix pass */
}

/* One untimed pass with self-checks: catches wrong arguments/sizes before
 * any number is trusted (the timed runs then measure, not validate). */
static void self_check(void)
{
	const uint8_t *input[2] = {nv12_y, nv12_uv};
	const uint32_t in_linesize[2] = {NV12_W, NV12_W};
	/* decompress_nv12 pixel 0 (verified empirically against the pinned
	 * 33.0.1 code): byte 0 = Y, byte 1 = U (chroma[0]), byte 2 = V
	 * (chroma[1]), byte 3 = 0. NOTE: the 33.0.1 compress_uyvx_to_nv12
	 * reads Y from byte 1 — the pair is NOT an inverse there (recorded
	 * reference finding, ODD L91). */
	decompress_nv12(input, in_linesize, 0, NV12_H, packed444, NV12_W * 4);
	if (packed444[0] != nv12_y[0] || packed444[1] != nv12_uv[0] || packed444[2] != nv12_uv[1] || packed444[3] != 0) {
		fprintf(stderr, "ref-bench: nv12 self-check failed\n");
		exit(2);
	}

	/* mix_audio: recompute one channel in the identical add order —
	 * bit-exact (same order, same f32). */
	struct rb_ts_info ts = {0, (uint64_t)AUDIO_OUTPUT_FRAMES * 1000000000ULL / SAMPLE_RATE};
	memset(mix_buf, 0, sizeof(mix_buf));
	for (uint32_t s = 0; s < MIX_SOURCES; s++)
		mix_audio(mixes, &sources[s], MIX_CHANNELS, SAMPLE_RATE, &ts);
	for (uint32_t f = 0; f < AUDIO_OUTPUT_FRAMES; f++) {
		float expect = 0.0f;
		for (uint32_t s = 0; s < MIX_SOURCES; s++)
			expect += src_buf[s][0][0][f];
		if (expect != mix_buf[0][0][f]) {
			fprintf(stderr, "ref-bench: audio self-check failed at frame %u\n", f);
			exit(2);
		}
	}
}

/* ------------------------------------------------------------------ */
/* timing + JSON                                                       */
/* ------------------------------------------------------------------ */

struct bench
{
	const char *name;
	uint64_t (*work)(void); /* returns the work units of one run */
};

static const struct bench benches[] = {
	{"matmul4", work_matmul4},
	{"nv12_to_rgb", work_nv12_to_rgb},
	{"audio_mix", work_audio_mix},
};
#define N_BENCHES (sizeof(benches) / sizeof(benches[0]))

static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static int cmp_u64(const void *a, const void *b)
{
	uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b;
	return x < y ? -1 : x > y;
}

static void cpu_model(char *out, size_t n)
{
	out[0] = 0;
#ifdef __linux__
	FILE *f = fopen("/proc/cpuinfo", "r");
	if (!f)
		return;
	char line[256];
	while (fgets(line, sizeof(line), f)) {
		const char *val = NULL;
		if (strncmp(line, "model name", 10) == 0)
			val = line + 12; /* skip "model name\t: " */
		else if (strncmp(line, "Model", 5) == 0)
			val = line + 7;
		else if (strncmp(line, "Hardware", 8) == 0)
			val = line + 10;
		if (val) {
			while (*val == ':' || *val == ' ' || *val == '\t')
				val++;
			size_t l = strlen(val);
			while (l && (val[l - 1] == '\n' || val[l - 1] == '\r'))
				l--;
			if (l >= n)
				l = n - 1;
			memcpy(out, val, l);
			out[l] = 0;
			break;
		}
	}
	fclose(f);
#endif
	if (!out[0])
		strcpy(out, "unknown");
}

int main(int argc, char **argv)
{
	unsigned runs = DEFAULT_RUNS;
	const char *only = NULL;
	bool list = false;

	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--list") == 0)
			list = true;
		else if (strcmp(argv[i], "--runs") == 0 && i + 1 < argc)
			runs = (unsigned)atoi(argv[++i]);
		else if (strcmp(argv[i], "--bench") == 0 && i + 1 < argc)
			only = argv[++i];
		else {
			fprintf(stderr, "usage: ref-bench [--list] [--runs N] [--bench NAME]\n");
			return 2;
		}
	}
	if (runs < 3)
		runs = 3;

	if (list) {
		for (size_t i = 0; i < N_BENCHES; i++)
			printf("%s\n", benches[i].name);
		return 0;
	}

	init_data();
	self_check();

	char cpu[256];
	cpu_model(cpu, sizeof(cpu));

	printf("{\n");
	printf("  \"schema\": 1,\n");
	printf("  \"side\": \"c\",\n");
#ifdef _WIN32
	printf("  \"os\": \"windows\",\n");
#else
	printf("  \"os\": \"linux\",\n");
#endif
#if defined(__x86_64__)
	printf("  \"arch\": \"x86_64\",\n");
#elif defined(__aarch64__)
	printf("  \"arch\": \"aarch64\",\n");
#else
	printf("  \"arch\": \"unknown\",\n");
#endif
	printf("  \"cpu\": \"%s\",\n", cpu);
	printf("  \"benches\": {\n");

	bool first = true;
	for (size_t bi = 0; bi < N_BENCHES; bi++) {
		const struct bench *b = &benches[bi];
		if (only && strcmp(b->name, only) != 0)
			continue;

		for (int w = 0; w < WARMUP_RUNS; w++)
			b->work();
		uint64_t *samples = malloc(runs * sizeof(uint64_t));
		uint64_t iters = 0;
		for (unsigned r = 0; r < runs; r++) {
			uint64_t t0 = now_ns();
			iters = b->work();
			uint64_t t1 = now_ns();
			samples[r] = (t1 - t0) / iters; /* ns per work unit */
		}
		qsort(samples, runs, sizeof(uint64_t), cmp_u64);
		double med = (double)samples[runs / 2];

		if (!first)
			printf(",\n");
		first = false;
		printf("    \"%s\": {\"iters\": %llu, \"runs\": %u, \"ns_per_op\": %.9g}", b->name,
		       (unsigned long long)iters, runs, med);
		free(samples);
	}
	printf("\n  }\n}\n");
	return 0;
}
