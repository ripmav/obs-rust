/*
 * Conformance harness (area audiofilters, spec S-027): fake OBS core and recording fakes for
 * SpeexDSP / RNNoise / the audio resampler. Test tooling, not part of OBS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "obs.h"
#include "obs-module.h"
#include "util/platform.h"
#include "media-io/audio-resampler.h"
#include <speex/speex_preprocess.h>
#include <rnnoise.h>
#include "audiofilters_api.h"

struct af_host g_af = {.rate = 48000, .channels = 2, .clock = 10000000000ULL};
struct af_source *g_sources[AF_MAX_SOURCES];
int g_nsources;

#define EV(...)                              \
	do {                                 \
		if (g_af.events) {           \
			printf("host " __VA_ARGS__); \
			printf("\n");        \
		}                            \
	} while (0)

/* platform stubs needed by obs-data.c */
char *os_quick_read_utf8_file(const char *p) { (void)p; abort(); }
bool os_quick_write_utf8_file(const char *p, const char *s, size_t l, bool m) { (void)p, (void)s, (void)l, (void)m; abort(); }
bool os_quick_write_utf8_file_safe(const char *p, const char *s, size_t l, bool m, const char *t, const char *b)
{
	(void)p, (void)s, (void)l, (void)m, (void)t, (void)b;
	abort();
}
int os_rename(const char *a, const char *b) { return rename(a, b); }

const char *obs_module_text(const char *key) { return key; }
uint64_t os_gettime_ns(void) { return g_af.clock; }

audio_t *obs_get_audio(void) { return (audio_t *)&g_af; }
uint32_t audio_output_get_sample_rate(const audio_t *a) { (void)a; return g_af.rate; }
size_t audio_output_get_channels(const audio_t *a) { (void)a; return g_af.channels; }
bool obs_get_audio_info(struct obs_audio_info *oai)
{
	memset(oai, 0, sizeof(*oai));
	oai->samples_per_sec = g_af.rate;
	return true;
}

struct af_source *af_find_source(const char *name)
{
	for (int i = 0; i < g_nsources; i++)
		if (!strcmp(g_sources[i]->name, name))
			return g_sources[i];
	return NULL;
}

const char *obs_source_get_name(const obs_source_t *s) { return ((const struct af_source *)s)->name; }
obs_source_t *obs_filter_get_parent(const obs_source_t *f) { return (obs_source_t *)((const struct af_source *)f)->parent; }
enum speaker_layout obs_source_get_speaker_layout(obs_source_t *s) { return s ? ((struct af_source *)s)->layout : SPEAKERS_UNKNOWN; }
uint32_t obs_source_get_output_flags(const obs_source_t *s) { return ((const struct af_source *)s)->audio ? OBS_SOURCE_AUDIO : OBS_SOURCE_VIDEO; }

obs_source_t *obs_get_source_by_name(const char *name)
{
	struct af_source *s = af_find_source(name);
	if (s && s->dead)
		s = NULL;
	if (s)
		s->refs++;
	EV("get_source_by_name %s -> %s", name, s ? "ok" : "null");
	return (obs_source_t *)s;
}
void obs_source_release(obs_source_t *s)
{
	if (!s)
		return;
	((struct af_source *)s)->refs--;
	EV("source_release %s", ((struct af_source *)s)->name);
}
struct af_weak {
	struct af_source *s;
};
obs_weak_source_t *obs_source_get_weak_source(obs_source_t *s)
{
	struct af_weak *w = malloc(sizeof(*w));
	w->s = (struct af_source *)s;
	g_af.weak_live++;
	return (obs_weak_source_t *)w;
}
obs_source_t *obs_weak_source_get_source(obs_weak_source_t *w)
{
	struct af_source *s = ((struct af_weak *)w)->s;
	if (s->dead) {
		EV("weak_get_source %s -> null", s->name);
		return NULL;
	}
	s->refs++;
	EV("weak_get_source %s -> ok", s->name);
	return (obs_source_t *)s;
}
void obs_weak_source_release(obs_weak_source_t *w)
{
	if (!w)
		return;
	g_af.weak_live--;
	EV("weak_release %s", ((struct af_weak *)w)->s->name);
	free(w);
}
void obs_source_add_audio_capture_callback(obs_source_t *s, obs_source_audio_capture_t cb, void *param)
{
	struct af_source *a = (struct af_source *)s;
	EV("add_audio_capture_callback %s", a->name);
	if (a->ncb < AF_MAX_CB) {
		a->cbs[a->ncb].cb = cb;
		a->cbs[a->ncb].param = param;
		a->ncb++;
	}
}
void obs_source_remove_audio_capture_callback(obs_source_t *s, obs_source_audio_capture_t cb, void *param)
{
	struct af_source *a = (struct af_source *)s;
	EV("remove_audio_capture_callback %s", a->name);
	for (int i = 0; i < a->ncb; i++)
		if (a->cbs[i].cb == cb && a->cbs[i].param == param) {
			memmove(&a->cbs[i], &a->cbs[i + 1], (size_t)(a->ncb - i - 1) * sizeof(a->cbs[0]));
			a->ncb--;
			return;
		}
}
void obs_enum_sources(bool (*proc)(void *, obs_source_t *), void *param)
{
	for (int i = 0; i < g_nsources; i++)
		if (!proc(param, (obs_source_t *)g_sources[i]))
			break;
}
void obs_source_release_frame(obs_source_t *s, struct obs_source_frame *f)
{
	EV("release_frame parent=%s tag=%u ts=%llu", s ? ((struct af_source *)s)->name : "-", f->width,
	   (unsigned long long)f->timestamp);
}

/* ---- SpeexDSP fake: x[i] = x[i] / 2 (C integer division) ---- */
struct SpeexPreprocessState_ {
	int id, frame_size, rate, level;
};
static int g_spx_id, g_rnn_id, g_rs_id;
SpeexPreprocessState *speex_preprocess_state_init(int fs, int rate)
{
	SpeexPreprocessState *s = calloc(1, sizeof(*s));
	s->id = g_spx_id++;
	s->frame_size = fs;
	s->rate = rate;
	EV("speex.init id=%d frame_size=%d rate=%d", s->id, fs, rate);
	return s;
}
void speex_preprocess_state_destroy(SpeexPreprocessState *s)
{
	EV("speex.destroy id=%d", s->id);
	free(s);
}
int speex_preprocess_ctl(SpeexPreprocessState *s, int req, void *ptr)
{
	if (req == SPEEX_PREPROCESS_SET_NOISE_SUPPRESS)
		s->level = *(int *)ptr;
	EV("speex.ctl id=%d req=%d level=%d", s->id, req, s->level);
	return 0;
}
int speex_preprocess_run(SpeexPreprocessState *s, spx_int16_t *x)
{
	EV("speex.run id=%d level=%d in0=%d in1=%d in%d=%d", s->id, s->level, x[0], s->frame_size > 1 ? x[1] : 0,
	   s->frame_size - 1, x[s->frame_size - 1]);
	for (int i = 0; i < s->frame_size; i++)
		x[i] = (spx_int16_t)(x[i] / 2);
	return 1;
}

/* ---- RNNoise fake: out[i] = in[i] * 0.5f, returns 0.5f ---- */
struct DenoiseState {
	int id;
};
DenoiseState *rnnoise_create(RNNModel *m)
{
	(void)m;
	DenoiseState *s = calloc(1, sizeof(*s));
	s->id = g_rnn_id++;
	EV("rnnoise.create id=%d", s->id);
	return s;
}
void rnnoise_destroy(DenoiseState *s)
{
	EV("rnnoise.destroy id=%d", s->id);
	free(s);
}
float rnnoise_process_frame(DenoiseState *s, float *out, const float *in)
{
	EV("rnnoise.process id=%d in0=%.9g in1=%.9g in479=%.9g", s->id, (double)in[0], (double)in[1], (double)in[479]);
	for (int i = 0; i < 480; i++)
		out[i] = in[i] * 0.5f;
	return 0.5f;
}

/* ---- resampler fake: nearest-neighbour, out_frames = in*dst/src unless forced ---- */
struct audio_resampler {
	int id, ch;
	uint32_t src, dst;
	float buf[8][4096];
};
static int speakers_to_ch(enum speaker_layout l)
{
	switch (l) {
	case SPEAKERS_MONO: return 1;
	case SPEAKERS_STEREO: return 2;
	case SPEAKERS_2POINT1: return 3;
	case SPEAKERS_4POINT0: return 4;
	case SPEAKERS_4POINT1: return 5;
	case SPEAKERS_5POINT1: return 6;
	case SPEAKERS_7POINT1: return 8;
	default: return 2;
	}
}
audio_resampler_t *audio_resampler_create(const struct resample_info *dst, const struct resample_info *src)
{
	struct audio_resampler *r = calloc(1, sizeof(*r));
	r->id = g_rs_id++;
	r->src = src->samples_per_sec;
	r->dst = dst->samples_per_sec;
	r->ch = speakers_to_ch(src->speakers);
	EV("resampler.create id=%d src=%u dst=%u channels=%d", r->id, r->src, r->dst, r->ch);
	return (audio_resampler_t *)r;
}
void audio_resampler_destroy(audio_resampler_t *r)
{
	EV("resampler.destroy id=%d", ((struct audio_resampler *)r)->id);
	free(r);
}
bool audio_resampler_resample(audio_resampler_t *rs, uint8_t *output[], uint32_t *out_frames, uint64_t *ts_offset,
			      const uint8_t *const input[], uint32_t in_frames)
{
	struct audio_resampler *r = (struct audio_resampler *)rs;
	uint32_t n = g_af.resampler_out_frames ? g_af.resampler_out_frames : (uint32_t)((uint64_t)in_frames * r->dst / r->src);
	if (n > 4096)
		n = 4096;
	for (int c = 0; c < r->ch; c++) {
		const float *in = (const float *)input[c];
		for (uint32_t k = 0; k < n; k++) {
			uint64_t idx = (uint64_t)k * r->src / r->dst;
			if (idx >= in_frames)
				idx = in_frames - 1;
			r->buf[c][k] = in[idx];
		}
		output[c] = (uint8_t *)r->buf[c];
	}
	*out_frames = n;
	*ts_offset = 0;
	EV("resampler.resample id=%d in=%u out=%u", r->id, in_frames, n);
	return true;
}
