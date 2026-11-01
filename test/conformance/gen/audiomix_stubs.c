/*
 * Conformance harness for spec S-032 (audio mixer, faders, volmeters): the minimal "libobs" around the real
 * obs-source.c / obs-audio.c / obs-audio-controls.c.
 *
 * obs-source.c is #included (not linked) so that this file can call its static initialiser and finaliser
 * (obs_source_init, obs_source_init_finalize, the push-to-talk hotkey callbacks). Everything that source code reaches
 * but that is not audio logic is replaced here:
 *   - a fake `obs` core (audio / data / video structs only),
 *   - a fake audio output (the audio_output_* getters; the real audio-io.c owns a thread and is not linked),
 *   - a fake clock (os_gettime_ns),
 *   - a fake resampler (hook contract only, NOT the ffmpeg resampler),
 *   - a trivial context / refcount layer and obs-data stand-ins,
 *   - "abort" stubs for graphics / hotkeys / transitions / scenes that audio code never reaches.
 * Test tooling, not part of libobs.
 */
#include "obs-source.c"

#include "audiomix_api.h"

struct obs_core *obs;

/* ---------------------------------------------------------------- fake clock */

static uint64_t am_clock_ns;

void am_set_clock(uint64_t ns)
{
	am_clock_ns = ns;
}

uint64_t os_gettime_ns(void)
{
	return am_clock_ns;
}

/* ---------------------------------------------------------------- fake audio output */

struct audio_output {
	struct audio_output_info info;
};
static struct audio_output am_audio_out;

static struct audio_output *am_get_audio(void)
{
	return &am_audio_out;
}

uint32_t audio_output_get_sample_rate(const audio_t *audio)
{
	return ((const struct audio_output *)audio)->info.samples_per_sec;
}

size_t audio_output_get_channels(const audio_t *audio)
{
	return get_audio_channels(((const struct audio_output *)audio)->info.speakers);
}

const struct audio_output_info *audio_output_get_info(const audio_t *audio)
{
	return &((const struct audio_output *)audio)->info;
}

size_t audio_output_get_planes(const audio_t *audio)
{
	const struct audio_output_info *i = &((const struct audio_output *)audio)->info;
	return get_audio_planes(i->format, i->speakers);
}

size_t audio_output_get_block_size(const audio_t *audio)
{
	const struct audio_output_info *i = &((const struct audio_output *)audio)->info;
	return get_audio_bytes_per_channel(i->format) * (is_audio_planar(i->format) ? 1 : get_audio_channels(i->speakers));
}

bool obs_get_audio_info(struct obs_audio_info *oai)
{
	if (!obs->audio.audio)
		return false;
	oai->samples_per_sec = am_audio_out.info.samples_per_sec;
	oai->speakers = am_audio_out.info.speakers;
	return true;
}

/* ---------------------------------------------------------------- fake resampler (hook contract only) */

struct audio_resampler {
	struct resample_info out, in;
	float *planes[MAX_AV_PLANES];
};
uint64_t am_resampler_ts_offset; /* value reported through *offset on every call */
int am_resampler_creates;
bool am_resampler_fail;

audio_resampler_t *audio_resampler_create(const struct resample_info *dst, const struct resample_info *src)
{
	if (am_resampler_fail)
		return NULL;
	struct audio_resampler *r = bzalloc(sizeof(*r));
	r->out = *dst;
	r->in = *src;
	am_resampler_creates++;
	return r;
}

void audio_resampler_destroy(audio_resampler_t *r)
{
	if (!r)
		return;
	for (int i = 0; i < MAX_AV_PLANES; i++)
		bfree(r->planes[i]);
	bfree(r);
}

/* Zero-order-hold rate conversion of float planar input; channel i maps to channel i, missing channels are silent. */
bool audio_resampler_resample(audio_resampler_t *r, uint8_t *output[], uint32_t *out_frames, uint64_t *ts_offset,
			      const uint8_t *const input[], uint32_t in_frames)
{
	uint32_t n = (uint32_t)((uint64_t)in_frames * r->out.samples_per_sec / r->in.samples_per_sec);
	size_t in_ch = get_audio_channels(r->in.speakers), out_ch = get_audio_channels(r->out.speakers);
	for (size_t c = 0; c < out_ch; c++) {
		bfree(r->planes[c]);
		r->planes[c] = bzalloc(sizeof(float) * (n ? n : 1));
		if (c < in_ch && input[c])
			for (uint32_t i = 0; i < n; i++)
				r->planes[c][i] = ((const float *)input[c])[(uint64_t)i * r->in.samples_per_sec /
									  r->out.samples_per_sec];
		output[c] = (uint8_t *)r->planes[c];
	}
	*out_frames = n;
	*ts_offset = am_resampler_ts_offset;
	return true;
}

/* ---------------------------------------------------------------- context / refcount layer */

bool obs_context_data_init(struct obs_context_data *context, enum obs_obj_type type, obs_data_t *settings,
			   const char *name, const char *uuid, obs_data_t *hotkey_data, bool private)
{
	(void)settings;
	(void)uuid;
	(void)hotkey_data;
	context->type = type;
	context->private = private;
	context->name = bstrdup(name);
	context->signals = signal_handler_create();
	return context->signals != NULL;
}

void obs_context_data_free(struct obs_context_data *context)
{
	signal_handler_destroy(context->signals);
	bfree(context->name);
}

void obs_context_init_control(struct obs_context_data *context, void *object, obs_destroy_cb destroy)
{
	context->control = bzalloc(sizeof(obs_weak_object_t));
	context->control->object = object;
	context->destroy = destroy;
}

void obs_context_data_insert_name(struct obs_context_data *c, pthread_mutex_t *m, void *f)
{
	(void)c, (void)m, (void)f;
}
void obs_context_data_insert_uuid(struct obs_context_data *c, pthread_mutex_t *m, void *f)
{
	(void)c, (void)m, (void)f;
}
void obs_context_data_remove_name(struct obs_context_data *c, pthread_mutex_t *m, void *f)
{
	(void)c, (void)m, (void)f;
}
void obs_context_data_remove_uuid(struct obs_context_data *c, pthread_mutex_t *m, void *f)
{
	(void)c, (void)m, (void)f;
}

obs_data_t *obs_data_create(void)
{
	return bzalloc(8);
}
void obs_data_release(obs_data_t *d)
{
	bfree(d);
}

/* Everything else obs-source.c references (graphics, hotkeys, transitions, scenes, profiler, deinterlacing, task
 * queue, obs-data arrays, ...) is never reached by the scripts. gen/audiomix.sources links with
 * -Wl,--unresolved-symbols=ignore-all, so a script that strays there ends in a crash (the runner is fork-per-case
 * for exactly the commands where that could matter) instead of needing ~100 individual stubs. */

/* ---------------------------------------------------------------- scripted sources */

/* Per-source script state of the harness sources. */
struct am_src {
	bool ok;       /* audio_render / audio_mix result */
	uint64_t ts;   /* reported timestamp (AM_TS_TICK = start of the tick being mixed) */
	float value;   /* sample value written (times channel+1) */
	float gain;    /* filter: multiplier */
	bool drop;     /* filter: returns NULL */
	obs_source_t *children[8];
	int nchildren;
	struct obs_audio_data filtered; /* filter output scratch */
	float *fbuf[MAX_AV_PLANES];
	size_t fcap;
	uint64_t render_calls;
};

uint64_t am_tick_start; /* start timestamp of the audio_callback currently running */

static bool am_render(void *data, uint64_t *ts_out, struct obs_source_audio_mix *audio, uint32_t mixers,
		      size_t channels, size_t sample_rate)
{
	struct am_src *s = data;
	(void)sample_rate;
	s->render_calls++;
	if (!s->ok)
		return false;
	*ts_out = s->ts == AM_TS_TICK ? am_tick_start : s->ts;
	for (size_t m = 0; m < MAX_AUDIO_MIXES; m++) {
		if (!(mixers & (1u << m)))
			continue;
		for (size_t c = 0; c < channels; c++)
			for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++)
				audio->output[m].data[c][i] += s->value * (float)(c + 1);
	}
	return true;
}

static bool am_audio_mix(void *data, uint64_t *ts_out, struct audio_output_data *out, size_t channels,
			 size_t sample_rate)
{
	struct am_src *s = data;
	(void)sample_rate;
	if (!s->ok)
		return false;
	*ts_out = s->ts == AM_TS_TICK ? am_tick_start : s->ts;
	for (size_t c = 0; c < channels; c++)
		for (size_t i = 0; i < AUDIO_OUTPUT_FRAMES; i++)
			out->data[c][i] = s->value * (float)(c + 1);
	return true;
}

static void am_enum_children(void *data, obs_source_enum_proc_t cb, void *param)
{
	struct am_src *s = data;
	for (int i = 0; i < s->nchildren; i++)
		cb(NULL, s->children[i], param);
}

static struct obs_audio_data *am_filter_audio(void *data, struct obs_audio_data *in)
{
	struct am_src *s = data;
	if (s->drop)
		return NULL;
	size_t n = get_audio_channels(am_audio_out.info.speakers);
	if (s->fcap < in->frames) {
		for (size_t c = 0; c < MAX_AV_PLANES; c++) {
			bfree(s->fbuf[c]);
			s->fbuf[c] = bzalloc(sizeof(float) * (in->frames ? in->frames : 1));
		}
		s->fcap = in->frames;
	}
	memset(&s->filtered, 0, sizeof(s->filtered));
	for (size_t c = 0; c < n; c++) {
		for (uint32_t i = 0; i < in->frames; i++)
			s->fbuf[c][i] = ((float *)in->data[c])[i] * s->gain;
		s->filtered.data[c] = (uint8_t *)s->fbuf[c];
	}
	s->filtered.frames = in->frames;
	s->filtered.timestamp = in->timestamp;
	return &s->filtered;
}

static const char *am_get_name(void *type_data)
{
	(void)type_data;
	return "am";
}

obs_source_t *am_source_create(const char *name, enum am_kind kind)
{
	struct obs_source_info info = {.id = "am", .get_name = am_get_name};
	struct am_src *s = bzalloc(sizeof(*s));
	s->ok = true;
	s->ts = AM_TS_TICK;
	s->gain = 1.0f;

	switch (kind) {
	case AM_INPUT:
		info.type = OBS_SOURCE_TYPE_INPUT;
		info.output_flags = OBS_SOURCE_AUDIO;
		break;
	case AM_CUSTOM:
		info.type = OBS_SOURCE_TYPE_INPUT;
		info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_COMPOSITE;
		info.audio_render = am_render;
		info.enum_active_sources = am_enum_children;
		break;
	case AM_CUSTOM_AUDIO: /* audio_render without the composite flag */
		info.type = OBS_SOURCE_TYPE_INPUT;
		info.output_flags = OBS_SOURCE_AUDIO;
		info.audio_render = am_render;
		break;
	case AM_SUBMIX:
		info.type = OBS_SOURCE_TYPE_INPUT;
		info.output_flags = OBS_SOURCE_AUDIO | OBS_SOURCE_SUBMIX;
		info.audio_mix = am_audio_mix;
		break;
	case AM_FILTER:
		info.type = OBS_SOURCE_TYPE_FILTER;
		info.output_flags = OBS_SOURCE_AUDIO;
		info.filter_audio = am_filter_audio;
		break;
	}

	struct obs_source *source = bzalloc(sizeof(*source));
	source->info = info;
	if (!obs_source_init_context(source, NULL, name, NULL, NULL, true) || !obs_source_init(source))
		abort();
	source->context.data = s;
	source->flags = source->default_flags;
	source->enabled = true;
	source->audio_is_duplicated = false;
	if (kind != AM_FILTER)
		obs_source_init_finalize(source, NULL);
	return source;
}

void am_cfg(obs_source_t *source, bool ok, uint64_t ts, float value, float gain, bool drop)
{
	struct am_src *s = source->context.data;
	s->ok = ok;
	s->ts = ts;
	s->value = value;
	s->gain = gain;
	s->drop = drop;
}

void am_add_child(obs_source_t *parent, obs_source_t *child)
{
	struct am_src *s = parent->context.data;
	if (s->nchildren < 8)
		s->children[s->nchildren++] = child;
}

void am_add_filter(obs_source_t *source, obs_source_t *filter)
{
	da_push_back(source->filters, &filter);
}

void am_hotkey_ptm(obs_source_t *source, bool pressed)
{
	obs_source_hotkey_push_to_mute(source, 0, NULL, pressed);
}

void am_hotkey_ptt(obs_source_t *source, bool pressed)
{
	obs_source_hotkey_push_to_talk(source, 0, NULL, pressed);
}

/* ---------------------------------------------------------------- core */

void am_core_reset(uint32_t rate, enum speaker_layout speakers)
{
	static bool first = true;
	if (first) {
		obs = bzalloc(sizeof(*obs));
		pthread_mutex_init(&obs->data.audio_sources_mutex, NULL);
		pthread_mutex_init(&obs->data.sources_mutex, NULL);
		pthread_mutex_init(&obs->video.mixes_mutex, NULL);
		pthread_mutex_init(&obs->audio.task_mutex, NULL);
		first = false;
	}
	am_audio_out.info.samples_per_sec = rate;
	am_audio_out.info.speakers = speakers;
	am_audio_out.info.format = AUDIO_FORMAT_FLOAT_PLANAR;
	obs->audio.audio = (audio_t *)am_get_audio();
	obs->audio.max_buffering_ticks = 45;
	obs->audio.fixed_buffer = false;
	obs->audio.total_buffering_ticks = 0;
	obs->audio.buffering_wait_ticks = 0;
	obs->audio.buffered_ts = 0;
	deque_free(&obs->audio.buffered_timestamps);
	obs->audio.monitoring_duplicating_source = NULL;
	am_resampler_ts_offset = 0;
	am_resampler_fail = false;
	am_resampler_creates = 0;
}

struct obs_core *am_obs(void)
{
	return obs;
}

void am_add_video_mix(bool mix_audio)
{
	struct obs_core_video_mix *m = bzalloc(sizeof(*m));
	m->view = bzalloc(sizeof(struct obs_view));
	pthread_mutex_init(&m->view->channels_mutex, NULL);
	m->mix_audio = mix_audio;
	da_push_back(obs->video.mixes, &m);
}

void am_set_channel(size_t mix, uint32_t channel, obs_source_t *source)
{
	obs->video.mixes.array[mix]->view->channels[channel] = source;
}

void am_set_task(void (*fn)(void *), void *param)
{
	struct obs_task_info info = {fn, param};
	deque_push_back(&obs->audio.tasks, &info, sizeof(info));
}
