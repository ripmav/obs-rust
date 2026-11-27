/*
 * obs-ref-dump (WP-39, P0-14.4): media dump.
 *
 *  - mix_audio: the C function is `static inline` in libobs/obs-audio.c
 *    (33.0.1, line ~90) and needs the full obs_source_t. This is a
 *    verbatim copy of the function body with minimal stand-in types that
 *    keep every field access and every arithmetic step identical.
 *  - format-conversion.c: all six exported YUV compress/decompress
 *    functions over a generated 64x36 gradient, FNV-1a-64 hashed.
 *
 * Determinism: the "sine" waves are pure-arithmetic (fixed-point triangle
 * ramps), NOT libm sinf — libm sinf differs by ULPs across glibc/musl/
 * macOS and would break the 3-OS golden. The mix_audio arithmetic (the
 * behaviour under test) is waveform-independent.
 */
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "media-io/audio-io.h"
#include "media-io/format-conversion.h"
#include "util/util_uint64.h"

extern uint32_t next_u32(void);
extern void rd_reset_prng(void);

#define CASES 32
#define W 64
#define H 36
#define SR 48000

/* --- mix_audio: verbatim copy (obs-audio.c:90-124, 33.0.1) --- */
struct rd_audio_output_data
{
	float *data[MAX_AUDIO_CHANNELS];
};
struct rd_source
{
	uint64_t audio_ts;
	float *audio_output_buf[MAX_AUDIO_MIXES][MAX_AUDIO_CHANNELS];
};
struct rd_ts_info
{
	uint64_t start;
	uint64_t end;
};
static size_t convert_time_to_frames(size_t sample_rate, uint64_t t)
{
	return (size_t)util_mul_div64(t, sample_rate, 1000000000ULL);
}
static void mix_audio(struct rd_audio_output_data *mixes, struct rd_source *source, size_t channels, size_t sample_rate,
		      struct rd_ts_info *ts)
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

/* FNV-1a 64 */
static uint64_t fnv1a64(const uint8_t *data, size_t len)
{
	uint64_t h = 1469598103934665603ULL;
	for (size_t i = 0; i < len; i++) {
		h ^= data[i];
		h *= 1099511628211ULL;
	}
	return h;
}
static uint64_t fnv_f32(const float *data, size_t n)
{
	return fnv1a64((const uint8_t *)data, n * sizeof(float));
}

/* fixed-point triangle wave: period 1024 samples, amplitude 1.0, no libm */
static void fill_wave(float *buf, size_t n, int period, float gain)
{
	for (size_t i = 0; i < n; i++) {
		int p = (int)(i % (size_t)period);
		float t;
		if (p < period / 4)
			t = 4.0f * ((float)p / (float)period) + 0.5f;
		else if (p < period / 2)
			t = 1.0f;
		else if (p < 3 * period / 4)
			t = 3.0f - 4.0f * ((float)p / (float)period) - 0.5f;
		else
			t = -1.0f;
		buf[i] = t * gain;
	}
}

static void rd_dump_audio(void)
{
	printf("# media/mix_audio\n");
	rd_reset_prng();

	static float wave0[AUDIO_OUTPUT_FRAMES];
	static float wave1[AUDIO_OUTPUT_FRAMES];
	fill_wave(wave0, AUDIO_OUTPUT_FRAMES, 256, 0.5f);
	fill_wave(wave1, AUDIO_OUTPUT_FRAMES, 512, 0.25f);

	/* ts offsets: 0, 1 frame, quarter, half, near-end, outside */
	/* 0, 1 frame (25000ns -> 1), 2 frames, 512 frames, 1024 frames (start_point ==
	 * AUDIO_OUTPUT_FRAMES -> early return), outside the window */
	static const uint64_t ts_offsets[7] = {0, 25000ULL, 50000ULL, 10666667ULL, 21333333ULL, 21333334ULL, 2000000000ULL};
	static const uint64_t ts_start = 500000000ULL;
	/* wide window (the function does not check its length); offsets are kept to
	 * start_point <= 1024 because start_point > AUDIO_OUTPUT_FRAMES is an
	 * unguarded size_t underflow in the C (latent defect, pinned in the ODD) */
	static const uint64_t ts_end = 600000000ULL; /* start + 100 ms */

	for (int c = 0; c < CASES; c++) {
		uint64_t offset = ts_offsets[c % 7];
		int channels = 1 + (c % 3); /* 1..3 */
		static float mix_buf[MAX_AUDIO_MIXES][MAX_AUDIO_CHANNELS][AUDIO_OUTPUT_FRAMES];
		static float src_buf[MAX_AUDIO_MIXES][MAX_AUDIO_CHANNELS][AUDIO_OUTPUT_FRAMES];

		for (int mi = 0; mi < MAX_AUDIO_MIXES; mi++)
			for (int ch = 0; ch < MAX_AUDIO_CHANNELS; ch++) {
				memset(mix_buf[mi][ch], 0, AUDIO_OUTPUT_FRAMES * sizeof(float));
				if (ch < channels) {
					memcpy(src_buf[mi][ch], (ch == 0) ? wave0 : wave1, AUDIO_OUTPUT_FRAMES * sizeof(float));
				} else {
					memset(src_buf[mi][ch], 0, AUDIO_OUTPUT_FRAMES * sizeof(float));
				}
			}

		struct rd_source src;
		memset(&src, 0, sizeof(src));
		src.audio_ts = ts_start + offset;
		for (int mi = 0; mi < MAX_AUDIO_MIXES; mi++)
			for (int ch = 0; ch < MAX_AUDIO_CHANNELS; ch++)
				src.audio_output_buf[mi][ch] = src_buf[mi][ch];

		struct rd_audio_output_data mixes[MAX_AUDIO_MIXES];
		for (int mi = 0; mi < MAX_AUDIO_MIXES; mi++)
			for (int ch = 0; ch < MAX_AUDIO_CHANNELS; ch++)
				mixes[mi].data[ch] = mix_buf[mi][ch];

		struct rd_ts_info ts = {ts_start, ts_end};
		mix_audio(mixes, &src, (size_t)channels, (size_t)SR, &ts);

		printf("case %d: ts_offset=%llu channels=%d ch0_hash=0x%016llx ch1_hash=0x%016llx first8=%.9g %.9g %.9g %.9g %.9g %.9g %.9g %.9g\n",
		       c, (unsigned long long)offset, channels, (unsigned long long)fnv_f32(mix_buf[0][0], AUDIO_OUTPUT_FRAMES),
		       (unsigned long long)fnv_f32(mix_buf[0][1], AUDIO_OUTPUT_FRAMES), mix_buf[0][0][0], mix_buf[0][0][1],
		       mix_buf[0][0][2], mix_buf[0][0][3], mix_buf[0][0][4], mix_buf[0][0][5], mix_buf[0][0][6],
		       mix_buf[0][0][7]);
	}
}

/* 64x36 uyvx gradient (u=y*2, v=x*2, lum=(x*3+y*5)&0xff) */
static void make_uyvx(uint8_t *out)
{
	for (int y = 0; y < H; y++) {
		for (int x = 0; x < W; x++) {
			uint8_t lum = (uint8_t)((x * 3 + y * 5) & 0xff);
			uint8_t u = (uint8_t)((y * 2) & 0xff);
			uint8_t v = (uint8_t)((x * 2) & 0xff);
			out[(y * W + x) * 4 + 0] = u;
			out[(y * W + x) * 4 + 1] = lum;
			out[(y * W + x) * 4 + 2] = v;
			out[(y * W + x) * 4 + 3] = lum;
		}
	}
}

#define PLANE(name) static uint8_t name[H * W]
PLANE(i420_y);
PLANE(i420_u);
PLANE(i420_v);
static uint8_t nv12_y[W * H];
static uint8_t nv12_uv[W * H];
PLANE(i444_y);
PLANE(i444_u);
PLANE(i444_v);
static uint8_t uyvx[W * H * 4];
static uint8_t out_uyvx[W * H * 4];
static uint8_t out_uyvx2[W * H * 4];
static uint8_t yuv422[W * H]; /* per row: W/2 uint32 words [y0,u,y1,v] */

static void rd_dump_pixels(void)
{
	printf("# media/pixels\n");
	make_uyvx(uyvx);

	uint8_t *i420_out[3] = {i420_y, i420_u, i420_v};
	const uint8_t *i420_in[3] = {i420_y, i420_u, i420_v};
	uint32_t i420_linesize[3] = {W, W / 2, W / 2};
	uint8_t *nv12_out[2] = {nv12_y, nv12_uv};
	const uint8_t *nv12_in[2] = {nv12_y, nv12_uv};
	uint32_t nv12_linesize[2] = {W, W};
	uint8_t *i444_out[3] = {i444_y, i444_u, i444_v};
	uint32_t i444_linesize[3] = {W, W, W};

	memset(i420_y, 0, sizeof(i420_y));
	memset(i420_u, 0, sizeof(i420_u));
	memset(i420_v, 0, sizeof(i420_v));
	compress_uyvx_to_i420(uyvx, W * 4, 0, H, i420_out, i420_linesize);
	printf("i420: y=0x%016llx u=0x%016llx v=0x%016llx\n", (unsigned long long)fnv1a64(i420_y, sizeof(i420_y)),
	       (unsigned long long)fnv1a64(i420_u, sizeof(i420_u)), (unsigned long long)fnv1a64(i420_v, sizeof(i420_v)));

	memset(nv12_y, 0, sizeof(nv12_y));
	memset(nv12_uv, 0, sizeof(nv12_uv));
	compress_uyvx_to_nv12(uyvx, W * 4, 0, H, nv12_out, nv12_linesize);
	printf("nv12: y=0x%016llx uv=0x%016llx\n", (unsigned long long)fnv1a64(nv12_y, sizeof(nv12_y)),
	       (unsigned long long)fnv1a64(nv12_uv, sizeof(nv12_uv)));

	memset(i444_y, 0, sizeof(i444_y));
	memset(i444_u, 0, sizeof(i444_u));
	memset(i444_v, 0, sizeof(i444_v));
	convert_uyvx_to_i444(uyvx, W * 4, 0, H, i444_out, i444_linesize);
	printf("i444: y=0x%016llx u=0x%016llx v=0x%016llx\n", (unsigned long long)fnv1a64(i444_y, sizeof(i444_y)),
	       (unsigned long long)fnv1a64(i444_u, sizeof(i444_u)), (unsigned long long)fnv1a64(i444_v, sizeof(i444_v)));

	/* round trip nv12 -> uyvx */
	memset(out_uyvx, 0, sizeof(out_uyvx));
	decompress_nv12(nv12_in, nv12_linesize, 0, H, out_uyvx, W * 4);
	printf("nv12_roundtrip: uyvx=0x%016llx equal_to_source=%d\n", (unsigned long long)fnv1a64(out_uyvx, sizeof(out_uyvx)),
	       memcmp(out_uyvx, uyvx, sizeof(uyvx)) == 0);

	/* round trip i420 -> uyvx */
	memset(out_uyvx2, 0, sizeof(out_uyvx2));
	decompress_420(i420_in, i420_linesize, 0, H, out_uyvx2, W * 4);
	printf("i420_roundtrip: uyvx=0x%016llx equal_to_source=%d\n", (unsigned long long)fnv1a64(out_uyvx2, sizeof(out_uyvx2)),
	       memcmp(out_uyvx2, uyvx, sizeof(uyvx)) == 0);

	/* 422: interleaved little-endian 32-bit words [y0, u, y1, v] per
	 * pixel pair (the layout decompress_422 reads); row = W/2 words */
	for (int y = 0; y < H; y++) {
		for (int k = 0; k < W / 2; k++) {
			uint8_t y0 = uyvx[(y * W + 2 * k) * 4 + 1];
			uint8_t y1 = uyvx[(y * W + 2 * k + 1) * 4 + 1];
			uint8_t u = uyvx[(y * W + 2 * k) * 4 + 0];
			uint8_t v = uyvx[(y * W + 2 * k) * 4 + 2];
			((uint32_t *)yuv422)[y * (W / 2) + k] = (uint32_t)(y0 | (u << 8) | (y1 << 16) | (v << 24));
		}
	}
	for (int leading = 0; leading < 2; leading++) {
		memset(out_uyvx, 0, sizeof(out_uyvx));
		decompress_422(yuv422, W, 0, H, out_uyvx, W * 4, (bool)leading);
		printf("yuv422_leading_lum=%d: uyvx=0x%016llx equal_to_source=%d\n", leading,
		       (unsigned long long)fnv1a64(out_uyvx, sizeof(out_uyvx)), memcmp(out_uyvx, uyvx, sizeof(uyvx)) == 0);
	}
}

/* partial-range conversion: rows 4..8 only (exercises start_y/end_y) */
static void rd_dump_pixels_range(void)
{
	printf("# media/pixels_range\n");
	uint8_t *i420_out[3] = {i420_y, i420_u, i420_v};
	uint32_t linesize[3] = {W, W / 2, W / 2};
	memset(i420_y, 0, sizeof(i420_y));
	memset(i420_u, 0, sizeof(i420_u));
	memset(i420_v, 0, sizeof(i420_v));
	compress_uyvx_to_i420(uyvx, W * 4, 4, 8, i420_out, linesize);
	printf("i420 rows4..8: y=0x%016llx u=0x%016llx v=0x%016llx\n", (unsigned long long)fnv1a64(i420_y, sizeof(i420_y)),
	       (unsigned long long)fnv1a64(i420_u, sizeof(i420_u)), (unsigned long long)fnv1a64(i420_v, sizeof(i420_v)));
}

void obs_ref_dump_media(void)
{
	rd_dump_audio();
	rd_dump_pixels();
	rd_dump_pixels_range();
}
