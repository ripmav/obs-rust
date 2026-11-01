/*
 * Conformance harness for the MP4 muxer (spec S-022): stand-ins for the obs_output / obs_encoder /
 * video_t / audio_t accessors that plugins/obs-outputs/mp4-mux.c queries, plus the few platform
 * and libobs-internal functions it links against. Every accessor documents which spec input it
 * represents. Test tooling, not part of libobs.
 *
 * The muxer never owns anything but references (obs_encoder_get_ref / obs_encoder_release); the
 * stub encoder counts them so that scripts can observe leaks.
 */
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>

#include <obs.h>
#include <obs-module.h>
#include <util/bmem.h>
#include <util/threading.h>


/* Interface to mp4_runner.c (declared there as well) */
void mp4stub_reset(void);
obs_output_t *mp4stub_output(void);
void mp4stub_set_output_name(const char *name);
void mp4stub_set_peak(float peak);
void mp4stub_set_version(const char *v);
void mp4stub_set_start_chapter_text(const char *v);
obs_encoder_t *mp4stub_encoder_new(bool video);
long mp4stub_encoder_refs(obs_encoder_t *e);
void mp4stub_encoder_set_slot(obs_encoder_t *e, size_t idx);
bool mp4stub_encoder_set(obs_encoder_t *e, const char *key, const char *val, size_t extra_len, const uint8_t *extra);
enum obs_encoder_type mp4stub_encoder_type(obs_encoder_t *e);
uint32_t mp4stub_encoder_timebase_num(obs_encoder_t *e);
uint32_t mp4stub_encoder_timebase_den(obs_encoder_t *e);
const char *mp4stub_encoder_codec(obs_encoder_t *e);
void mp4stub_encoder_free(obs_encoder_t *e);

struct obs_output {
	char *name;
	struct obs_encoder *video[MAX_OUTPUT_VIDEO_ENCODERS];
	struct obs_encoder *audio[MAX_OUTPUT_AUDIO_ENCODERS];
};

struct obs_encoder {
	long refs;
	enum obs_encoder_type type;
	char *codec;
	char *id;
	char *name;
	uint32_t width, height;
	uint32_t sample_rate;                 /* obs_encoder_get_sample_rate */
	size_t frame_size;                    /* obs_encoder_get_frame_size */
	bool has_extra;                       /* obs_encoder_get_extra_data returns this */
	uint8_t *extra;
	size_t extra_size;
	obs_data_t *settings;                 /* NULL: obs_encoder_get_settings returns NULL */
	bool no_video, no_audio;              /* obs_encoder_video() / obs_encoder_audio() return NULL */
	struct video_output_info vinfo;       /* video_output_get_info */
	struct audio_output_info ainfo;       /* audio_output_get_info */
	uint32_t audio_rate;                  /* audio_output_get_sample_rate */
};

static struct obs_output g_output;
static float g_peak = 1000.0f;
static char g_version[64] = "33.0.1";
static char g_start_chapter[64] = "Start";

static char *dup_or_null(const char *s)
{
	return s ? bstrdup(s) : NULL;
}

void mp4stub_reset(void)
{
	for (size_t i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++)
		g_output.video[i] = NULL;
	for (size_t i = 0; i < MAX_OUTPUT_AUDIO_ENCODERS; i++)
		g_output.audio[i] = NULL;
	bfree(g_output.name);
	g_output.name = bstrdup("conformance");
	g_peak = 1000.0f;
	snprintf(g_version, sizeof(g_version), "33.0.1");
	snprintf(g_start_chapter, sizeof(g_start_chapter), "Start");
}

obs_output_t *mp4stub_output(void)
{
	if (!g_output.name)
		g_output.name = bstrdup("conformance");
	return &g_output;
}

void mp4stub_set_output_name(const char *name)
{
	bfree(g_output.name);
	g_output.name = dup_or_null(name);
}

void mp4stub_set_peak(float peak)
{
	g_peak = peak;
}

void mp4stub_set_version(const char *v)
{
	snprintf(g_version, sizeof(g_version), "%s", v);
}

void mp4stub_set_start_chapter_text(const char *v)
{
	snprintf(g_start_chapter, sizeof(g_start_chapter), "%s", v);
}

obs_encoder_t *mp4stub_encoder_new(bool video)
{
	struct obs_encoder *e = bzalloc(sizeof(*e));
	e->refs = 1;
	e->type = video ? OBS_ENCODER_VIDEO : OBS_ENCODER_AUDIO;
	e->codec = bstrdup(video ? "h264" : "aac");
	e->id = bstrdup("stub_encoder");
	e->name = bstrdup("Stub Encoder");
	e->width = 1920;
	e->height = 1080;
	e->sample_rate = 48000;
	e->audio_rate = 48000;
	e->frame_size = 1024;
	e->vinfo.fps_num = 30;
	e->vinfo.fps_den = 1;
	e->vinfo.colorspace = VIDEO_CS_709;
	e->vinfo.range = VIDEO_RANGE_PARTIAL;
	e->ainfo.samples_per_sec = 48000;
	e->ainfo.speakers = SPEAKERS_STEREO;
	return e;
}

long mp4stub_encoder_refs(obs_encoder_t *e)
{
	return e->refs;
}

void mp4stub_encoder_set_slot(obs_encoder_t *e, size_t idx)
{
	if (e->type == OBS_ENCODER_VIDEO)
		g_output.video[idx] = e;
	else
		g_output.audio[idx] = e;
}

/* Returns false for an unknown key. A value of "-" means NULL for the string keys. */
bool mp4stub_encoder_set(obs_encoder_t *e, const char *key, const char *val, size_t extra_len, const uint8_t *extra)
{
	bool null = !strcmp(val, "-");
	if (!strcmp(key, "codec")) {
		bfree(e->codec);
		e->codec = dup_or_null(null ? NULL : val);
	} else if (!strcmp(key, "id")) {
		bfree(e->id);
		e->id = dup_or_null(null ? NULL : val);
	} else if (!strcmp(key, "name")) {
		bfree(e->name);
		e->name = dup_or_null(null ? NULL : val);
	} else if (!strcmp(key, "width")) {
		e->width = (uint32_t)strtoul(val, NULL, 0);
	} else if (!strcmp(key, "height")) {
		e->height = (uint32_t)strtoul(val, NULL, 0);
	} else if (!strcmp(key, "fps")) {
		unsigned n = 0, d = 1;
		sscanf(val, "%u/%u", &n, &d);
		e->vinfo.fps_num = n;
		e->vinfo.fps_den = d;
	} else if (!strcmp(key, "cs")) {
		e->vinfo.colorspace = !strcmp(val, "default") ? VIDEO_CS_DEFAULT
				      : !strcmp(val, "601")   ? VIDEO_CS_601
				      : !strcmp(val, "709")   ? VIDEO_CS_709
				      : !strcmp(val, "srgb")  ? VIDEO_CS_SRGB
				      : !strcmp(val, "pq")    ? VIDEO_CS_2100_PQ
							      : VIDEO_CS_2100_HLG;
	} else if (!strcmp(key, "range")) {
		e->vinfo.range = !strcmp(val, "full") ? VIDEO_RANGE_FULL
				 : !strcmp(val, "default") ? VIDEO_RANGE_DEFAULT
							   : VIDEO_RANGE_PARTIAL;
	} else if (!strcmp(key, "rate")) {
		e->sample_rate = (uint32_t)strtoul(val, NULL, 0);
	} else if (!strcmp(key, "arate")) {
		e->audio_rate = (uint32_t)strtoul(val, NULL, 0);
		e->ainfo.samples_per_sec = e->audio_rate;
	} else if (!strcmp(key, "speakers")) {
		e->ainfo.speakers = !strcmp(val, "unknown") ? SPEAKERS_UNKNOWN
				    : !strcmp(val, "mono")  ? SPEAKERS_MONO
				    : !strcmp(val, "stereo") ? SPEAKERS_STEREO
				    : !strcmp(val, "2.1")   ? SPEAKERS_2POINT1
				    : !strcmp(val, "4.0")   ? SPEAKERS_4POINT0
				    : !strcmp(val, "4.1")   ? SPEAKERS_4POINT1
				    : !strcmp(val, "5.1")   ? SPEAKERS_5POINT1
							    : SPEAKERS_7POINT1;
	} else if (!strcmp(key, "frame_size")) {
		e->frame_size = (size_t)strtoul(val, NULL, 0);
	} else if (!strcmp(key, "extra")) {
		bfree(e->extra);
		e->extra = NULL;
		e->extra_size = 0;
		e->has_extra = !null;
		if (!null) {
			e->extra = bmemdup(extra_len ? extra : (const uint8_t *)"", extra_len ? extra_len : 1);
			e->extra_size = extra_len;
		}
	} else if (!strcmp(key, "bitrate") || !strcmp(key, "codec_type")) {
		if (!e->settings)
			e->settings = obs_data_create();
		obs_data_set_int(e->settings, key, (long long)strtoll(val, NULL, 0));
	} else if (!strcmp(key, "settings")) {
		/* "none": obs_encoder_get_settings returns NULL; "empty": an empty settings object */
		if (e->settings)
			obs_data_release(e->settings);
		e->settings = NULL;
		if (!strcmp(val, "empty"))
			e->settings = obs_data_create();
	} else if (!strcmp(key, "setting_str")) {
		/* val = "name=value" */
		char *eq = strchr(val, '=');
		if (!eq)
			return false;
		char name[64];
		snprintf(name, sizeof(name), "%.*s", (int)(eq - val), val);
		if (!e->settings)
			e->settings = obs_data_create();
		obs_data_set_string(e->settings, name, eq + 1);
	} else if (!strcmp(key, "novideo")) {
		e->no_video = strtol(val, NULL, 0) != 0;
	} else if (!strcmp(key, "noaudio")) {
		e->no_audio = strtol(val, NULL, 0) != 0;
	} else {
		return false;
	}
	return true;
}

enum obs_encoder_type mp4stub_encoder_type(obs_encoder_t *e)
{
	return e->type;
}

uint32_t mp4stub_encoder_timebase_num(obs_encoder_t *e)
{
	return e->type == OBS_ENCODER_VIDEO ? e->vinfo.fps_den : 1;
}

uint32_t mp4stub_encoder_timebase_den(obs_encoder_t *e)
{
	return e->type == OBS_ENCODER_VIDEO ? e->vinfo.fps_num : e->sample_rate;
}

const char *mp4stub_encoder_codec(obs_encoder_t *e)
{
	return e->codec;
}

void mp4stub_encoder_free(obs_encoder_t *e)
{
	bfree(e->codec);
	bfree(e->id);
	bfree(e->name);
	bfree(e->extra);
	if (e->settings)
		obs_data_release(e->settings);
	bfree(e);
}

/* ---------------------------------------------------------------------- */
/* obs_output                                                              */

const char *obs_output_get_name(const obs_output_t *output)
{
	return output->name;
}

/* Spec input: video encoder slots (track order = video slots ascending, then audio slots). */
obs_encoder_t *obs_output_get_video_encoder2(const obs_output_t *output, size_t idx)
{
	return idx < MAX_OUTPUT_VIDEO_ENCODERS ? output->video[idx] : NULL;
}

obs_encoder_t *obs_output_get_audio_encoder(const obs_output_t *output, size_t idx)
{
	return idx < MAX_OUTPUT_AUDIO_ENCODERS ? output->audio[idx] : NULL;
}

/* ---------------------------------------------------------------------- */
/* obs_encoder                                                             */

obs_encoder_t *obs_encoder_get_ref(obs_encoder_t *encoder)
{
	if (!encoder)
		return NULL;
	encoder->refs++;
	return encoder;
}

void obs_encoder_release(obs_encoder_t *encoder)
{
	if (!encoder)
		return;
	encoder->refs--;
}

enum obs_encoder_type obs_encoder_get_type(const obs_encoder_t *e)
{
	if (!e)
		return OBS_ENCODER_AUDIO;
	return e->type;
}

const char *obs_encoder_get_codec(const obs_encoder_t *e)
{
	if (!e)
		return NULL;
	return e->codec;
}

const char *obs_encoder_get_id(const obs_encoder_t *e)
{
	if (!e)
		return NULL;
	return e->id;
}

const char *obs_encoder_get_name(const obs_encoder_t *e)
{
	if (!e)
		return NULL;
	return e->name;
}

uint32_t obs_encoder_get_width(const obs_encoder_t *e)
{
	if (!e)
		return 0;
	return e->width;
}

uint32_t obs_encoder_get_height(const obs_encoder_t *e)
{
	if (!e)
		return 0;
	return e->height;
}

uint32_t obs_encoder_get_sample_rate(const obs_encoder_t *e)
{
	if (!e)
		return 0;
	return e->sample_rate;
}

size_t obs_encoder_get_frame_size(const obs_encoder_t *e)
{
	if (!e)
		return 0;
	return e->frame_size;
}

bool obs_encoder_get_extra_data(const obs_encoder_t *e, uint8_t **extra_data, size_t *size)
{
	if (!e)
		return false;
	if (!e->has_extra)
		return false;
	*extra_data = e->extra;
	*size = e->extra_size;
	return true;
}

obs_data_t *obs_encoder_get_settings(const obs_encoder_t *e)
{
	if (!e)
		return NULL;
	if (!e->settings)
		return NULL;
	obs_data_addref(e->settings);
	return e->settings;
}

video_t *obs_encoder_video(const obs_encoder_t *e)
{
	if (!e)
		return NULL;
	return e->no_video ? NULL : (video_t *)e;
}

audio_t *obs_encoder_audio(const obs_encoder_t *e)
{
	if (!e)
		return NULL;
	return e->no_audio ? NULL : (audio_t *)e;
}

const struct video_output_info *video_output_get_info(const video_t *video)
{
	if (!video)
		return NULL;
	return &((const struct obs_encoder *)video)->vinfo;
}

const struct audio_output_info *audio_output_get_info(const audio_t *audio)
{
	if (!audio)
		return NULL;
	return &((const struct obs_encoder *)audio)->ainfo;
}

size_t audio_output_get_channels(const audio_t *audio)
{
	return get_audio_channels(((const struct obs_encoder *)audio)->ainfo.speakers);
}

uint32_t audio_output_get_sample_rate(const audio_t *audio)
{
	return ((const struct obs_encoder *)audio)->audio_rate;
}

/* ---------------------------------------------------------------------- */
/* libobs globals                                                          */

float obs_get_video_hdr_nominal_peak_level(void)
{
	return g_peak;
}

const char *obs_get_version_string(void)
{
	return g_version;
}

const char *obs_module_text(const char *lookup)
{
	if (!strcmp(lookup, "MP4Output.StartChapter"))
		return g_start_chapter;
	return lookup;
}

/* Reproduced from libobs/obs-encoder.c (reference counted packet data, count lives in the
 * `long` in front of the payload). */
void obs_encoder_packet_ref(struct encoder_packet *dst, struct encoder_packet *src)
{
	if (!src)
		return;
	if (src->data) {
		long *p_refs = ((long *)src->data) - 1;
		os_atomic_inc_long(p_refs);
	}
	*dst = *src;
}

void obs_encoder_packet_release(struct encoder_packet *pkt)
{
	if (!pkt)
		return;
	if (pkt->data) {
		long *p_refs = ((long *)pkt->data) - 1;
		if (os_atomic_dec_long(p_refs) == 0)
			bfree(p_refs);
	}
	memset(pkt, 0, sizeof(struct encoder_packet));
}

/* ---------------------------------------------------------------------- */
/* platform functions (file-serializer.c, obs-data.c)                      */

int os_fseeki64(FILE *file, int64_t offset, int origin)
{
	return fseeko(file, (off_t)offset, origin);
}

int64_t os_ftelli64(FILE *file)
{
	return (int64_t)ftello(file);
}

int os_unlink(const char *path)
{
	return unlink(path);
}

int os_rename(const char *old_path, const char *new_path)
{
	return rename(old_path, new_path);
}

/* obs-data.c file helpers: never reached by the MP4 scripts. */
char *os_quick_read_utf8_file(const char *path)
{
	(void)path;
	return NULL;
}

bool os_quick_write_utf8_file(const char *path, const char *str, size_t len, bool marker)
{
	(void)path, (void)str, (void)len, (void)marker;
	return false;
}

bool os_quick_write_utf8_file_safe(const char *path, const char *str, size_t len, bool marker, const char *temp_ext,
				   const char *backup_ext)
{
	(void)path, (void)str, (void)len, (void)marker, (void)temp_ext, (void)backup_ext;
	return false;
}
