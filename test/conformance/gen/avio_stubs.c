/*
 * Fakes for the S-044 avio runner: clock, sleep, profiler hooks, video scaler, audio resampler.
 * Test tooling, not part of libobs.
 */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "media-io/video-io.h"
#include "media-io/video-scaler.h"
#include "media-io/audio-io.h"
#include "media-io/audio-resampler.h"
#include "util/platform.h"
#include "util/profiler.h"
#include "util/bmem.h"
#include "avio_stubs.h"

uint64_t avio_fake_now = 1000000000ULL;
uint64_t avio_last_sleep_target;
uint64_t avio_consumed_target;
bool avio_audio_lockstep;
sem_t avio_audio_go, avio_audio_done, avio_clock_read, avio_video_done;
volatile bool avio_shutdown;

int avio_scaler_create_result;
bool avio_scaler_scale_ok = true;
bool avio_resampler_create_ok = true;
bool avio_resampler_resample_ok = true;
uint64_t avio_resampler_offset;
uint32_t avio_resampler_out_frames;

/* ---- clock / sleep / names ---- */

uint64_t os_gettime_ns(void)
{
	sem_post(&avio_clock_read);
	return avio_fake_now;
}

bool os_sleepto_ns_fast(uint64_t target)
{
	avio_last_sleep_target = target;
	if (avio_audio_lockstep) {
		while (sem_wait(&avio_audio_go) != 0)
			;
		avio_consumed_target = target; /* written only after the driver released this tick */
	}
	return true;
}

/* The format string itself is the "stored name"; the runner recognises the two thread names by prefix. */
const char *profile_store_name(profiler_name_store_t *store, const char *format, ...)
{
	(void)store;
	return format;
}
void profile_start(const char *name)
{
	(void)name;
}
void profile_end(const char *name)
{
	if (!strncmp(name, "video_thread", 12))
		sem_post(&avio_video_done);
	else if (!strncmp(name, "audio_thread", 12))
		sem_post(&avio_audio_done);
}
void profile_reenable_thread(void) {}
profiler_name_store_t *obs_get_profiler_name_store(void)
{
	return NULL;
}

/* ---- fake video scaler ---- */

struct video_scaler {
	struct video_scale_info dst, src;
	int scales;
};

int video_scaler_create(video_scaler_t **scaler, const struct video_scale_info *dst,
			const struct video_scale_info *src, enum video_scale_type type)
{
	video_scaler_t *s;
	printf("scaler create dst=(%d %ux%u r%d cs%d) src=(%d %ux%u r%d cs%d) type=%d\n", dst->format, dst->width,
	       dst->height, dst->range, dst->colorspace, src->format, src->width, src->height, src->range,
	       src->colorspace, (int)type);
	if (avio_scaler_create_result != 0)
		return avio_scaler_create_result;
	s = calloc(1, sizeof(*s));
	s->dst = *dst;
	s->src = *src;
	*scaler = s;
	return VIDEO_SCALER_SUCCESS;
}

void video_scaler_destroy(video_scaler_t *s)
{
	if (s) {
		printf("scaler destroy scales=%d\n", s->scales);
		free(s);
	}
}

/* Output: every plane's first two bytes = { input plane0 byte0 ^ 0xff, number of scale calls so far }. */
bool video_scaler_scale(video_scaler_t *s, uint8_t *output[], const uint32_t out_linesize[],
			const uint8_t *const input[], const uint32_t in_linesize[])
{
	(void)out_linesize;
	(void)in_linesize;
	if (!avio_scaler_scale_ok)
		return false;
	s->scales++;
	for (int i = 0; i < 8; i++) {
		if (!output[i])
			continue;
		output[i][0] = (uint8_t)(input[0][0] ^ 0xff);
		output[i][1] = (uint8_t)s->scales;
	}
	return true;
}

/* ---- fake audio resampler ---- */

struct audio_resampler {
	struct resample_info dst, src;
	float out[MAX_AUDIO_CHANNELS][AUDIO_OUTPUT_FRAMES * 4];
};

audio_resampler_t *audio_resampler_create(const struct resample_info *dst, const struct resample_info *src)
{
	printf("resampler create dst=(%u %d %d) src=(%u %d %d)\n", dst->samples_per_sec, dst->format, dst->speakers,
	       src->samples_per_sec, src->format, src->speakers);
	if (!avio_resampler_create_ok)
		return NULL;
	audio_resampler_t *r = calloc(1, sizeof(*r));
	r->dst = *dst;
	r->src = *src;
	return r;
}

void audio_resampler_destroy(audio_resampler_t *r)
{
	if (r) {
		printf("resampler destroy\n");
		free(r);
	}
}

/* Output planes (of the destination layout) hold the input float samples * 0.5. */
bool audio_resampler_resample(audio_resampler_t *r, uint8_t *output[], uint32_t *out_frames, uint64_t *ts_offset,
			      const uint8_t *const input[], uint32_t in_frames)
{
	if (!avio_resampler_resample_ok)
		return false;
	uint32_t n = avio_resampler_out_frames
			     ? avio_resampler_out_frames
			     : (uint32_t)((uint64_t)in_frames * r->dst.samples_per_sec / r->src.samples_per_sec);
	if (n > AUDIO_OUTPUT_FRAMES * 4)
		n = AUDIO_OUTPUT_FRAMES * 4;
	size_t planes = get_audio_planes(r->dst.format, r->dst.speakers);
	size_t src_planes = get_audio_planes(r->src.format, r->src.speakers);
	for (size_t p = 0; p < planes && p < MAX_AUDIO_CHANNELS; p++) {
		const float *in = (const float *)input[p < src_planes ? p : 0];
		for (uint32_t i = 0; i < n; i++)
			r->out[p][i] = (i < AUDIO_OUTPUT_FRAMES ? in[i] : 0.0f) * 0.5f;
		output[p] = (uint8_t *)r->out[p];
	}
	*out_frames = n;
	*ts_offset = avio_resampler_offset;
	return true;
}
