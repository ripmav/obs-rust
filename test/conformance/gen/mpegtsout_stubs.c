/*
 * S-059 mpegtsout: fake libobs core for the real plugins/obs-ffmpeg/obs-ffmpeg-mpegts.c. One fake output with a video
 * encoder, up to six audio encoders and an optional service; settings are real obs_data objects (libobs/obs-data.c).
 * Test tooling, not part of OBS.
 */
#include <obs-module.h>
#include <media-io/video-io.h>
#include <media-io/audio-io.h>
#include <util/platform.h>
#include <util/bmem.h>
#include <pthread.h>
#include "mpegtsout_api.h"

struct mw_world W;

struct obs_encoder {
	int kind; /* 0 video, 1 audio */
	int idx;
};
struct obs_service {
	int dummy;
};
struct obs_output {
	struct obs_data *settings;
	struct obs_encoder venc, aenc[6];
	struct obs_service svc;
};

struct obs_output *mw_output;
static pthread_mutex_t evt_lock = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_vextra[] = {0, 0, 0, 1, 0x67, 0x42, 0xc0, 0x1e, 0xd9, 0x00, 0xa0, 0x47, 0xfe, 0xc8, 0, 0, 0, 1, 0x68, 0xce, 0x3c, 0x80};
static uint8_t g_aextra[] = {0x12, 0x10}; /* AudioSpecificConfig: AAC-LC, 44100 Hz, stereo */

void mw_set_str(char **dst, const char *v)
{
	free(*dst);
	*dst = (v && strcmp(v, "-")) ? strdup(v) : NULL;
}

void mw_world_reset(void)
{
	free(W.url);
	free(W.user);
	free(W.pass);
	free(W.streamid);
	free(W.passphrase);
	memset(&W, 0, sizeof(W));
	W.fps_num = 30;
	W.fps_den = 1;
	W.colorspace = VIDEO_CS_709;
	W.range = VIDEO_RANGE_PARTIAL;
	W.vformat = VIDEO_FORMAT_NV12;
	W.width = 1280;
	W.height = 720;
	W.hdr_peak = 1000.0f;
	W.have_vinfo = true;
	W.vbitrate = 2500;
	W.keyint_sec = 2;
	strcpy(W.vcodec, "h264");
	W.sample_rate = 44100;
	W.speakers = SPEAKERS_STEREO;
	W.have_ainfo = true;
	W.ntracks = 1;
	for (int i = 0; i < 6; i++) {
		W.abitrate[i] = 160;
		W.aextra[i] = true;
	}
	W.frame_size = 1024;
	strcpy(W.acodec, "aac");
	W.vextra = true;
	W.have_service = true;
	W.url = strdup("srt://192.0.2.10:9000");
	W.can_begin = true;
	W.init_ok = true;
}

struct obs_output *mw_output_create(void)
{
	mw_output_free();
	mw_output = calloc(1, sizeof(*mw_output));
	mw_output->settings = obs_data_create();
	mw_output->venc.kind = 0;
	for (int i = 0; i < 6; i++) {
		mw_output->aenc[i].kind = 1;
		mw_output->aenc[i].idx = i;
	}
	return mw_output;
}

void mw_output_free(void)
{
	if (mw_output) {
		obs_data_release(mw_output->settings);
		free(mw_output);
		mw_output = NULL;
	}
}

const char *obs_module_text(const char *k)
{
	return k;
}

/* ---- video / audio info ---- */
static struct video_output_info g_voi;
float obs_get_video_hdr_nominal_peak_level(void)
{
	return W.hdr_peak;
}
bool obs_get_video_info(struct obs_video_info *ovi)
{
	if (!W.have_vinfo)
		return false;
	memset(ovi, 0, sizeof(*ovi));
	ovi->fps_num = W.fps_num;
	ovi->fps_den = W.fps_den;
	return true;
}
bool obs_get_audio_info(struct obs_audio_info *oai)
{
	if (!W.have_ainfo)
		return false;
	memset(oai, 0, sizeof(*oai));
	oai->samples_per_sec = W.sample_rate;
	oai->speakers = W.speakers;
	return true;
}
video_t *obs_encoder_video(const obs_encoder_t *e)
{
	(void)e;
	return (video_t *)&g_voi;
}
enum video_format video_output_get_format(const video_t *v)
{
	(void)v;
	return W.vformat;
}
const struct video_output_info *video_output_get_info(const video_t *v)
{
	(void)v;
	g_voi.format = W.vformat;
	g_voi.range = W.range;
	g_voi.colorspace = W.colorspace;
	g_voi.fps_num = W.fps_num;
	g_voi.fps_den = W.fps_den;
	g_voi.width = W.width;
	g_voi.height = W.height;
	return &g_voi;
}

/* ---- encoders ---- */
const char *obs_encoder_get_codec(const obs_encoder_t *e)
{
	if (!e)
		return NULL; /* the reference passes NULL here when no audio encoder exists (defect) */
	return e->kind == 0 ? W.vcodec : W.acodec;
}
obs_data_t *obs_encoder_get_settings(const obs_encoder_t *e)
{
	obs_data_t *d = obs_data_create();
	if (e->kind == 0) {
		obs_data_set_int(d, "bitrate", W.vbitrate);
		obs_data_set_int(d, "keyint_sec", W.keyint_sec);
	} else {
		obs_data_set_int(d, "bitrate", W.abitrate[e->idx]);
	}
	return d;
}
size_t obs_encoder_get_frame_size(const obs_encoder_t *e)
{
	(void)e;
	return W.frame_size;
}
bool obs_encoder_get_extra_data(const obs_encoder_t *e, uint8_t **data, size_t *size)
{
	if (e->kind == 0) {
		if (!W.vextra)
			return false;
		*data = g_vextra;
		*size = sizeof(g_vextra);
	} else {
		if (!W.aextra[e->idx])
			return false;
		*data = g_aextra;
		*size = sizeof(g_aextra);
	}
	return true;
}

/* ---- output ---- */
const char *obs_output_get_name(const obs_output_t *o)
{
	(void)o;
	return "mpegts";
}
obs_data_t *obs_output_get_settings(const obs_output_t *o)
{
	obs_data_addref(o->settings);
	return o->settings;
}
obs_encoder_t *obs_output_get_video_encoder(const obs_output_t *o)
{
	return (obs_encoder_t *)&o->venc;
}
obs_encoder_t *obs_output_get_audio_encoder(const obs_output_t *o, size_t idx)
{
	return idx < (size_t)W.ntracks && idx < 6 ? (obs_encoder_t *)&o->aenc[idx] : NULL;
}
obs_service_t *obs_output_get_service(const obs_output_t *o)
{
	return W.have_service ? (obs_service_t *)&o->svc : NULL;
}
uint32_t obs_output_get_width(const obs_output_t *o)
{
	(void)o;
	return W.width;
}
uint32_t obs_output_get_height(const obs_output_t *o)
{
	(void)o;
	return W.height;
}
uint64_t obs_output_get_pause_offset(obs_output_t *o)
{
	(void)o;
	return W.pause_offset;
}
const char *obs_service_get_connect_info(const obs_service_t *s, uint32_t type)
{
	(void)s;
	switch (type) {
	case OBS_SERVICE_CONNECT_INFO_SERVER_URL:
		return W.url;
	case OBS_SERVICE_CONNECT_INFO_USERNAME:
		return W.user;
	case OBS_SERVICE_CONNECT_INFO_PASSWORD:
		return W.pass;
	case OBS_SERVICE_CONNECT_INFO_STREAM_ID:
		return W.streamid;
	case OBS_SERVICE_CONNECT_INFO_ENCRYPT_PASSPHRASE:
		return W.passphrase;
	}
	return NULL;
}
bool obs_output_can_begin_data_capture(const obs_output_t *o, uint32_t flags)
{
	(void)o;
	(void)flags;
	return W.can_begin;
}
bool obs_output_initialize_encoders(obs_output_t *o, uint32_t flags)
{
	(void)o;
	(void)flags;
	return W.init_ok;
}
bool obs_output_begin_data_capture(obs_output_t *o, uint32_t flags)
{
	(void)o;
	(void)flags;
	pthread_mutex_lock(&evt_lock);
	W.begin_calls++;
	pthread_mutex_unlock(&evt_lock);
	return true;
}
void obs_output_signal_stop(obs_output_t *o, int code)
{
	(void)o;
	pthread_mutex_lock(&evt_lock);
	if (W.nevt < MW_MAXEVT)
		W.evt[W.nevt] = code;
	W.nevt++;
	pthread_mutex_unlock(&evt_lock);
}
void obs_output_set_last_error(obs_output_t *o, const char *msg)
{
	(void)o;
	snprintf(W.last_error, sizeof(W.last_error), "%s", msg);
}

/* ---- platform stand-ins needed by libobs/obs-data.c ---- */
int os_rename(const char *a, const char *b)
{
	(void)a;
	(void)b;
	return -1;
}
char *os_quick_read_utf8_file(const char *p)
{
	(void)p;
	return NULL;
}
bool os_quick_write_utf8_file(const char *p, const char *s, size_t l, bool m)
{
	(void)p;
	(void)s;
	(void)l;
	(void)m;
	return false;
}
bool os_quick_write_utf8_file_safe(const char *p, const char *s, size_t l, bool m, const char *t, const char *b)
{
	(void)p;
	(void)s;
	(void)l;
	(void)m;
	(void)t;
	(void)b;
	return false;
}

struct obs_data *mw_settings(void)
{
	return mw_output->settings;
}
