/*
 * Conformance harness (area ffsources, spec S-047): recording fakes for everything the
 * obs-ffmpeg source and encoder wrappers call that is not the code under test: the OBS host
 * (source/hotkey/proc/missing-files/encoder/audio accessors), the media-playback library and
 * libavcodec. libavutil is the real system library. Every fake prints one line per call
 * ("host.xxx", "mp.xxx", "av.xxx") so the order and arguments of every call are part of the
 * expected output. Test tooling, not part of OBS.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>

#include "obs.h"
#include "obs-module.h"
#include "util/platform.h"
#include "util/threading.h"
#include "callback/proc.h"
#include "callback/calldata.h"
#include "media-playback/media-playback.h"
#include <libavcodec/avcodec.h>
#include <libavutil/pixdesc.h>
#include "ffsources_api.h"

struct fs_host g_host = {.width = 1920,
			 .height = 1080,
			 .fps_num = 60,
			 .fps_den = 1,
			 .format = VIDEO_FORMAT_NV12,
			 .colorspace = VIDEO_CS_709,
			 .range = VIDEO_RANGE_PARTIAL,
			 .channels = 2,
			 .speakers = SPEAKERS_STEREO,
			 .sample_rate = 48000,
			 .media_has_video = 1,
			 .recv_n = 0};

char *os_quick_read_utf8_file(const char *p)
{
	(void)p;
	abort();
}
bool os_quick_write_utf8_file(const char *p, const char *s, size_t l, bool m)
{
	(void)p, (void)s, (void)l, (void)m;
	abort();
}
bool os_quick_write_utf8_file_safe(const char *p, const char *s, size_t l, bool m, const char *a, const char *b)
{
	(void)p, (void)s, (void)l, (void)m, (void)a, (void)b;
	abort();
}
int os_rename(const char *a, const char *b)
{
	return rename(a, b);
}
uint64_t os_gettime_ns(void)
{
	return g_host.now_ns;
}

/* Translation table: obs_module_text(KEY) returns KEY, except for the two keys whose English
 * text contains the %1/%2 placeholders the encoder wrapper substitutes. */
const char *obs_module_text(const char *key)
{
	if (!strcmp(key, "Encoder.Error"))
		return "Failed to open %1: %2";
	if (!strcmp(key, "Encoder.Timeout"))
		return "Encoder %1 is taking too long to encode (timeout: %2 seconds)";
	return key;
}

static void q(const char *s)
{
	if (!s) {
		fputs("@null", stdout);
		return;
	}
	putchar('"');
	for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
		if (*p == '"' || *p == '\\')
			putchar('\\'), putchar(*p);
		else if (*p == '\n')
			fputs("\\n", stdout);
		else if (*p < 0x20 || *p >= 0x7f)
			printf("\\x%02x", *p);
		else
			putchar(*p);
	}
	putchar('"');
}

/* ------------------------------------------------------------------------- */
/* OBS source host */

static int src_dummy;
const char *obs_source_get_name(const obs_source_t *s)
{
	(void)s;
	return "src";
}
bool obs_source_active(const obs_source_t *s)
{
	(void)s;
	return g_host.active;
}
bool obs_source_showing(const obs_source_t *s)
{
	(void)s;
	return g_host.showing;
}
void obs_source_output_video(obs_source_t *s, const struct obs_source_frame *f)
{
	(void)s;
	printf("host.output_video %s\n", f ? "frame" : "null");
}
void obs_source_preload_video(obs_source_t *s, const struct obs_source_frame *f)
{
	(void)s, (void)f;
	printf("host.preload_video\n");
}
void obs_source_show_preloaded_video(obs_source_t *s)
{
	(void)s;
	printf("host.show_preloaded_video\n");
}
void obs_source_set_video_frame(obs_source_t *s, const struct obs_source_frame *f)
{
	(void)s, (void)f;
	printf("host.set_video_frame\n");
}
void obs_source_output_audio(obs_source_t *s, const struct obs_source_audio *a)
{
	(void)s, (void)a;
	printf("host.output_audio\n");
}
void obs_source_media_play_pause(obs_source_t *s, bool pause)
{
	(void)s;
	printf("host.media_play_pause pause=%d\n", pause);
}
void obs_source_media_restart(obs_source_t *s)
{
	(void)s;
	printf("host.media_restart\n");
}
void obs_source_media_stop(obs_source_t *s)
{
	(void)s;
	printf("host.media_stop\n");
}
void obs_source_media_started(obs_source_t *s)
{
	(void)s;
	printf("host.media_started\n");
}
void obs_source_media_ended(obs_source_t *s)
{
	(void)s;
	printf("host.media_ended\n");
}
obs_data_t *obs_source_get_settings(const obs_source_t *s)
{
	(void)s;
	obs_data_addref(g_host.src_settings);
	return g_host.src_settings;
}
void obs_source_update(obs_source_t *s, obs_data_t *settings)
{
	(void)s;
	printf("host.source_update local_file=");
	q(obs_data_get_string(settings, "local_file"));
	putchar('\n');
}
proc_handler_t *obs_source_get_proc_handler(const obs_source_t *s)
{
	(void)s;
	return (proc_handler_t *)&src_dummy;
}

/* proc handler: records declarations and callbacks */
struct fs_proc fs_procs[64];
int fs_proc_n;
void proc_handler_add(proc_handler_t *h, const char *decl, proc_handler_proc_t fn, void *data)
{
	(void)h;
	snprintf(fs_procs[fs_proc_n].decl, sizeof(fs_procs[0].decl), "%s", decl);
	fs_procs[fs_proc_n].fn = fn;
	fs_procs[fs_proc_n].data = data;
	fs_proc_n++;
	printf("host.proc_add ");
	q(decl);
	putchar('\n');
}

/* hotkeys */
struct fs_hotkey fs_hotkeys[64];
int fs_hotkey_n;
static size_t next_hk = 100;
obs_hotkey_id obs_hotkey_register_source(obs_source_t *s, const char *name, const char *desc, obs_hotkey_func func,
					 void *data)
{
	(void)s;
	printf("host.hotkey_register %s desc=%s id=%zu\n", name, desc, next_hk);
	snprintf(fs_hotkeys[fs_hotkey_n].name, sizeof(fs_hotkeys[0].name), "%s", name);
	fs_hotkeys[fs_hotkey_n].fn = func;
	fs_hotkeys[fs_hotkey_n++].data = data;
	return next_hk++;
}
obs_hotkey_pair_id obs_hotkey_pair_register_source(obs_source_t *s, const char *n0, const char *d0, const char *n1,
						   const char *d1, obs_hotkey_active_func f0, obs_hotkey_active_func f1,
						   void *data0, void *data1)
{
	(void)s;
	printf("host.hotkey_pair_register %s/%s desc=%s/%s id=%zu\n", n0, n1, d0, d1, next_hk);
	snprintf(fs_hotkeys[fs_hotkey_n].name, sizeof(fs_hotkeys[0].name), "%s", n0);
	fs_hotkeys[fs_hotkey_n].afn = f0;
	fs_hotkeys[fs_hotkey_n++].data = data0;
	snprintf(fs_hotkeys[fs_hotkey_n].name, sizeof(fs_hotkeys[0].name), "%s", n1);
	fs_hotkeys[fs_hotkey_n].afn = f1;
	fs_hotkeys[fs_hotkey_n++].data = data1;
	return next_hk++;
}
void obs_hotkey_unregister(obs_hotkey_id id)
{
	printf("host.hotkey_unregister id=%zu\n", (size_t)id);
}

/* missing files */
struct fs_mfile fs_last_missing[4];
int fs_last_missing_n;
obs_missing_files_t *obs_missing_files_create(void)
{
	fs_last_missing_n = 0;
	return (obs_missing_files_t *)calloc(1, 16);
}
obs_missing_file_t *obs_missing_file_create(const char *path, obs_missing_file_cb cb, int type, void *src, void *data)
{
	(void)data;
	struct fs_mfile *f = calloc(1, sizeof(*f));
	snprintf(f->path, sizeof(f->path), "%s", path);
	f->cb = cb;
	f->src = src;
	f->type = type;
	return (obs_missing_file_t *)f;
}
void obs_missing_files_add_file(obs_missing_files_t *files, obs_missing_file_t *file)
{
	struct fs_mfile *f = (struct fs_mfile *)file;
	fs_last_missing[fs_last_missing_n++] = *f;
	printf("host.missing_file path=");
	q(f->path);
	printf(" type=%d\n", f->type);
	free(f);
	free(files);
}

/* scripted file existence table */
static char exists_path[4][64];
static int exists_val[4];
static int exists_n;
void fs_set_exists(const char *p, int v)
{
	for (int i = 0; i < exists_n; i++)
		if (!strcmp(exists_path[i], p)) {
			exists_val[i] = v;
			return;
		}
	snprintf(exists_path[exists_n], 64, "%s", p);
	exists_val[exists_n++] = v;
}
bool fs_os_file_exists(const char *p)
{
	for (int i = 0; i < exists_n; i++)
		if (!strcmp(exists_path[i], p))
			return exists_val[i];
	return false;
}

/* pthread / os_event substitutes: single-threaded, deterministic (the wrapper redefines the
 * plugin's calls to these). A "thread" runs when it is joined (event already signalled by then) or
 * when the script says so (event unsignalled, the wait times out). */
struct fs_event {
	int signaled, manual;
};
int fs_os_event_init(os_event_t **ev, enum os_event_type type)
{
	if (g_host.event_init_fail) {
		printf("event.init fail\n");
		return -1;
	}
	struct fs_event *e = calloc(1, sizeof(*e));
	e->manual = type == OS_EVENT_TYPE_MANUAL;
	*ev = (os_event_t *)e;
	printf("event.init %s\n", e->manual ? "manual" : "auto");
	return 0;
}
void fs_os_event_destroy(os_event_t *ev)
{
	printf("event.destroy\n");
	free(ev);
}
int fs_os_event_signal(os_event_t *ev)
{
	printf("event.signal\n");
	((struct fs_event *)ev)->signaled = 1;
	return 0;
}
void fs_os_event_reset(os_event_t *ev)
{
	printf("event.reset\n");
	((struct fs_event *)ev)->signaled = 0;
}
int fs_os_event_timedwait(os_event_t *ev, unsigned long ms)
{
	struct fs_event *e = (struct fs_event *)ev;
	if (e->signaled) {
		printf("event.timedwait ms=%lu -> signaled\n", ms);
		if (!e->manual)
			e->signaled = 0;
		return 0;
	}
	printf("event.timedwait ms=%lu -> timeout\n", ms);
	return ETIMEDOUT;
}
int fs_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a)
{
	if (g_host.mutex_init_fail) {
		printf("mutex.init fail\n");
		return EAGAIN;
	}
	return pthread_mutex_init(m, a);
}
static struct {
	void *(*fn)(void *);
	void *arg;
	int pending;
} thr;
int fs_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
	(void)a;
	if (g_host.thread_create_fail) {
		printf("thread.create fail\n");
		return EAGAIN;
	}
	thr.fn = fn;
	thr.arg = arg;
	thr.pending = 1;
	*t = 1;
	printf("thread.create\n");
	return 0;
}
int fs_pthread_join(pthread_t t, void **r)
{
	(void)t, (void)r;
	printf("thread.join pending=%d\n", thr.pending);
	if (thr.pending) {
		thr.pending = 0;
		thr.fn(thr.arg);
	}
	return 0;
}
void fsw_thread_run(void)
{
	if (!thr.pending) {
		printf("thread.run none\n");
		return;
	}
	printf("thread.run\n");
	thr.pending = 0;
	thr.fn(thr.arg);
	printf("thread.done\n");
}

/* media-playback fake */
struct media_playback {
	int id;
};
static int media_ids;
struct mp_media_info fs_last_info;
int fs_have_info;
media_playback_t *media_playback_create(const struct mp_media_info *i)
{
	fs_last_info = *i;
	fs_have_info = 1;
	printf("mp.create path=");
	q(i->path);
	printf(" format=");
	q(i->format);
	printf(" options=");
	q(i->ffmpeg_options);
	printf("\n    buffering=%d speed=%d force_range=%d linear_alpha=%d hw=%d local=%d reconnecting=%d "
	       "preload=%d full_decode=%d cbs=%d%d%d%d%d\n",
	       i->buffering, i->speed, (int)i->force_range, i->is_linear_alpha, i->hardware_decoding,
	       i->is_local_file, i->reconnecting, i->request_preload, i->full_decode, i->v_cb != NULL,
	       i->v_preload_cb != NULL, i->v_seek_cb != NULL, i->a_cb != NULL, i->stop_cb != NULL);
	if (g_host.media_fail) {
		printf("mp.create -> null\n");
		return NULL;
	}
	struct media_playback *m = calloc(1, sizeof(*m));
	m->id = ++media_ids;
	return m;
}
void media_playback_destroy(media_playback_t *m)
{
	printf("mp.destroy #%d\n", m->id);
	free(m);
}
void media_playback_play(media_playback_t *m, bool looping, bool reconnecting)
{
	printf("mp.play #%d looping=%d reconnecting=%d\n", m->id, looping, reconnecting);
}
void media_playback_play_pause(media_playback_t *m, bool pause)
{
	printf("mp.play_pause #%d pause=%d\n", m->id, pause);
}
void media_playback_stop(media_playback_t *m)
{
	printf("mp.stop #%d\n", m->id);
}
void media_playback_set_looping(media_playback_t *m, bool l)
{
	printf("mp.set_looping #%d %d\n", m->id, l);
}
void media_playback_set_is_linear_alpha(media_playback_t *m, bool l)
{
	printf("mp.set_linear_alpha #%d %d\n", m->id, l);
}
void media_playback_preload_frame(media_playback_t *m)
{
	if (m)
		printf("mp.preload_frame #%d\n", m->id);
	else
		printf("mp.preload_frame null\n"); /* the real function ignores NULL */
}
int64_t media_playback_get_current_time(media_playback_t *m)
{
	return m ? g_host.media_time : 0; /* the real function returns 0 for NULL */
}
void media_playback_seek(media_playback_t *m, int64_t pos)
{
	printf("mp.seek #%d %lld\n", m->id, (long long)pos);
}
int64_t media_playback_get_frames(media_playback_t *m)
{
	return m ? g_host.media_frames : 0;
}
int64_t media_playback_get_duration(media_playback_t *m)
{
	return m ? g_host.media_duration_us : 0;
}
bool media_playback_has_video(media_playback_t *m)
{
	(void)m;
	return g_host.media_has_video;
}
bool media_playback_has_audio(media_playback_t *m)
{
	(void)m;
	return true;
}

void fsw_media_cb(const char *which)
{
	static struct obs_source_frame fr;
	static struct obs_source_audio au;
	if (!fs_have_info) {
		fprintf(stderr, "script: no media created\n");
		exit(2);
	}
	struct mp_media_info *i = &fs_last_info;
	if (!strcmp(which, "video"))
		i->v_cb(i->opaque, &fr);
	else if (!strcmp(which, "preload"))
		i->v_preload_cb(i->opaque, &fr);
	else if (!strcmp(which, "seek"))
		i->v_seek_cb(i->opaque, &fr);
	else if (!strcmp(which, "audio"))
		i->a_cb(i->opaque, &au);
	else if (!strcmp(which, "stop"))
		i->stop_cb(i->opaque);
	else {
		fprintf(stderr, "script: unknown media cb %s\n", which);
		exit(2);
	}
}

/* ------------------------------------------------------------------------- */
/* encoder / video / audio host accessors */

static struct video_output_info g_voi;
const char *obs_encoder_get_name(const obs_encoder_t *e)
{
	(void)e;
	return "enc";
}
const char *obs_encoder_get_last_error(obs_encoder_t *e)
{
	(void)e;
	return g_host.has_last_error ? g_host.last_error : NULL;
}
void obs_encoder_set_last_error(obs_encoder_t *e, const char *msg)
{
	(void)e;
	snprintf(g_host.last_error, sizeof(g_host.last_error), "%s", msg);
	g_host.has_last_error = 1;
	printf("host.set_last_error ");
	q(msg);
	putchar('\n');
}
uint64_t obs_encoder_get_pause_offset(const obs_encoder_t *e)
{
	(void)e;
	return g_host.pause_offset;
}
uint32_t obs_encoder_get_width(const obs_encoder_t *e)
{
	(void)e;
	return g_host.width;
}
uint32_t obs_encoder_get_height(const obs_encoder_t *e)
{
	(void)e;
	return g_host.height;
}
video_t *obs_encoder_video(const obs_encoder_t *e)
{
	(void)e;
	return (video_t *)&g_voi;
}
const struct video_output_info *video_output_get_info(const video_t *v)
{
	(void)v;
	g_voi.fps_num = g_host.fps_num;
	g_voi.fps_den = g_host.fps_den;
	g_voi.width = g_host.width;
	g_voi.height = g_host.height;
	return &g_voi;
}
audio_t *obs_encoder_audio(const obs_encoder_t *e)
{
	(void)e;
	return (audio_t *)&g_host;
}
size_t audio_output_get_channels(const audio_t *a)
{
	(void)a;
	return (size_t)g_host.channels;
}
uint32_t audio_output_get_sample_rate(const audio_t *a)
{
	(void)a;
	return g_host.sample_rate;
}
const struct audio_output_info *audio_output_get_info(const audio_t *a)
{
	static struct audio_output_info i;
	(void)a;
	i.samples_per_sec = g_host.sample_rate;
	i.format = AUDIO_FORMAT_FLOAT_PLANAR;
	i.speakers = (enum speaker_layout)g_host.speakers;
	return &i;
}
size_t obs_encoder_get_mixer_index(const obs_encoder_t *e)
{
	(void)e;
	return 0;
}

/* ------------------------------------------------------------------------- */
/* fake libavcodec */

#define MAXC 32
static struct {
	char name[32];
	int present, lossless;
	enum AVCodecID id;
} codecs[MAXC] = {{"aac", 1, 0, AV_CODEC_ID_AAC},
		  {"libopus", 1, 0, AV_CODEC_ID_OPUS},
		  {"opus", 1, 0, AV_CODEC_ID_OPUS},
		  {"pcm_s16le", 1, 1, AV_CODEC_ID_PCM_S16LE},
		  {"pcm_s24le", 1, 1, AV_CODEC_ID_PCM_S24LE},
		  {"pcm_f32le", 1, 1, AV_CODEC_ID_PCM_F32LE},
		  {"alac", 1, 1, AV_CODEC_ID_ALAC},
		  {"flac", 1, 1, AV_CODEC_ID_FLAC}};
static int ncodecs = 8;
static AVCodec fake_codec[MAXC];
static enum AVSampleFormat fmt_list[16];
static int fmt_set;
static int rate_list[16];
static int rate_set;
static int priv;

void fs_codec(const char *name, int present)
{
	for (int i = 0; i < ncodecs; i++)
		if (!strcmp(codecs[i].name, name)) {
			codecs[i].present = present;
			return;
		}
	snprintf(codecs[ncodecs].name, 32, "%s", name);
	codecs[ncodecs].present = present;
	codecs[ncodecs].id = AV_CODEC_ID_H264;
	ncodecs++;
}
void fs_codec_lossless(const char *name, int v)
{
	for (int i = 0; i < ncodecs; i++)
		if (!strcmp(codecs[i].name, name))
			codecs[i].lossless = v;
}
static const struct {
	const char *n;
	enum AVSampleFormat f;
} sfmts[] = {{"u8", AV_SAMPLE_FMT_U8},     {"s16", AV_SAMPLE_FMT_S16},   {"s32", AV_SAMPLE_FMT_S32},
	     {"flt", AV_SAMPLE_FMT_FLT},   {"u8p", AV_SAMPLE_FMT_U8P},   {"s16p", AV_SAMPLE_FMT_S16P},
	     {"s32p", AV_SAMPLE_FMT_S32P}, {"fltp", AV_SAMPLE_FMT_FLTP}, {"dbl", AV_SAMPLE_FMT_DBL}};
void fs_formats(const char *list)
{
	fmt_set = 0;
	if (!strcmp(list, "none"))
		return;
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", list);
	int n = 0;
	for (char *t = strtok(buf, ","); t && n < 15; t = strtok(NULL, ","))
		for (size_t i = 0; i < sizeof(sfmts) / sizeof(*sfmts); i++)
			if (!strcmp(sfmts[i].n, t))
				fmt_list[n++] = sfmts[i].f;
	fmt_list[n] = AV_SAMPLE_FMT_NONE;
	fmt_set = 1;
}
void fs_rates(const char *list)
{
	rate_set = 0;
	if (!strcmp(list, "none"))
		return;
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", list);
	int n = 0;
	for (char *t = strtok(buf, ","); t && n < 15; t = strtok(NULL, ","))
		rate_list[n++] = atoi(t);
	rate_list[n] = 0;
	rate_set = 1;
}

const AVCodec *avcodec_find_encoder_by_name(const char *name)
{
	for (int i = 0; i < ncodecs; i++)
		if (!strcmp(codecs[i].name, name)) {
			printf("av.find_encoder %s -> %s\n", name, codecs[i].present ? "found" : "missing");
			if (!codecs[i].present)
				return NULL;
			fake_codec[i].name = codecs[i].name;
			fake_codec[i].id = codecs[i].id;
			return &fake_codec[i];
		}
	printf("av.find_encoder %s -> missing\n", name);
	return NULL;
}
const AVCodecDescriptor *avcodec_descriptor_get(enum AVCodecID id)
{
	static AVCodecDescriptor d;
	d.id = id;
	d.props = 0;
	for (int i = 0; i < ncodecs; i++)
		if (codecs[i].id == id && codecs[i].lossless)
			d.props = AV_CODEC_PROP_LOSSLESS;
	return &d;
}
AVCodecContext *avcodec_alloc_context3(const AVCodec *codec)
{
	(void)codec;
	AVCodecContext *c = calloc(1, sizeof(*c));
	c->sample_fmt = AV_SAMPLE_FMT_NONE;
	c->priv_data = &priv;
	return c;
}
void avcodec_free_context(AVCodecContext **c)
{
	printf("av.free_context\n");
	free(*c);
	*c = NULL;
}
int avcodec_get_supported_config(const AVCodecContext *ctx, const AVCodec *codec, enum AVCodecConfig config,
				 unsigned flags, const void **out, int *n)
{
	(void)ctx, (void)codec, (void)flags, (void)n;
	*out = NULL;
	if (config == AV_CODEC_CONFIG_SAMPLE_FORMAT && fmt_set)
		*out = fmt_list;
	else if (config == AV_CODEC_CONFIG_SAMPLE_RATE && rate_set)
		*out = rate_list;
	return 0;
}
int av_opt_set(void *obj, const char *name, const char *val, int flags)
{
	(void)obj;
	printf("av.opt_set %s=%s flags=%d\n", name, val, flags);
	return 0;
}
int avcodec_open2(AVCodecContext *ctx, const AVCodec *codec, AVDictionary **opts)
{
	(void)opts;
	printf("av.open2 codec=%s bit_rate=%lld sample_rate=%d channels=%d sample_fmt=%d strict=%d flags=%d "
	       "%dx%d pix_fmt=%d\n",
	       codec->name, (long long)ctx->bit_rate, ctx->sample_rate, ctx->ch_layout.nb_channels,
	       (int)ctx->sample_fmt, ctx->strict_std_compliance, ctx->flags, ctx->width, ctx->height,
	       (int)ctx->pix_fmt);
	if (g_host.open_rc)
		return g_host.open_rc;
	/* The fake gives the opened context a frame size and (audio) a time base the way an encoder
	 * would; the values are scripted. */
	ctx->frame_size = g_host.frame_size_after_open;
	ctx->initial_padding = g_host.initial_padding;
	if (g_host.extradata_size) {
		static uint8_t ed[16] = {0xde, 0xad, 0xbe, 0xef, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
		ctx->extradata = ed;
		ctx->extradata_size = g_host.extradata_size;
	}
	if (ctx->sample_rate) {
		ctx->time_base.num = g_host.tb_num ? g_host.tb_num : 1;
		ctx->time_base.den = g_host.tb_den ? g_host.tb_den : ctx->sample_rate;
	}
	return 0;
}
int avcodec_fill_audio_frame(AVFrame *f, int ch, enum AVSampleFormat fmt, const uint8_t *buf, int size, int align)
{
	printf("av.fill_audio_frame channels=%d fmt=%d buf_size=%d align=%d nb_samples=%d -> %d\n", ch, (int)fmt,
	       size, align, f->nb_samples, g_host.fill_rc);
	if (g_host.fill_rc)
		return g_host.fill_rc;
	f->data[0] = (uint8_t *)buf;
	return size;
}

static void hexn(const uint8_t *p, int n)
{
	for (int i = 0; i < n; i++)
		printf("%02x", p[i]);
}

/* Fills the buffers of a video frame with a sentinel so that copied and untouched regions are
 * both visible in the frame dump. */
void fs_fill_sentinel(AVFrame *f)
{
	for (int i = 0; i < AV_NUM_DATA_POINTERS && f->buf[i]; i++)
		memset(f->buf[i]->data, 0xEE, f->buf[i]->size);
}
static void dump_video_frame(const AVFrame *f)
{
	int hs = 0, vs = 0;
	av_pix_fmt_get_chroma_sub_sample(f->format, &hs, &vs);
	printf("av.send_frame video pts=%lld %dx%d fmt=%d\n", (long long)f->pts, f->width, f->height, f->format);
	for (int p = 0; p < AV_NUM_DATA_POINTERS && f->data[p]; p++) {
		int sh = (p && p < 3) ? vs : 0;
		int rows = (f->height + (1 << sh) - 1) >> sh;
		printf("    plane %d linesize=%d rows:", p, f->linesize[p]);
		for (int y = 0; y < rows; y++) {
			const uint8_t *row = f->data[p] + (size_t)y * (size_t)f->linesize[p];
			uint32_t h = 2166136261u; /* FNV-1a over the whole row incl. padding (sentinel) */
			for (int x = 0; x < f->linesize[p]; x++)
				h = (h ^ row[x]) * 16777619u;
			putchar(' ');
			hexn(row, 4);
			printf(":%08x", h);
		}
		putchar('\n');
	}
}
int avcodec_send_frame(AVCodecContext *c, const AVFrame *f)
{
	(void)c;
	if (!f)
		printf("av.send_frame NULL");
	else if (f->width > 0)
		dump_video_frame(f);
	else {
		printf("av.send_frame audio pts=%lld nb_samples=%d fmt=%d channels=%d rate=%d data=", (long long)f->pts,
		       f->nb_samples, f->format, f->ch_layout.nb_channels, f->sample_rate);
		if (f->data[0])
			hexn(f->data[0], 8);
		else
			printf("-");
	}
	int rc = 0;
	if (g_host.send_n) {
		rc = g_host.send_q[0];
		memmove(g_host.send_q, g_host.send_q + 1, sizeof(int) * (size_t)--g_host.send_n);
	}
	if (f && f->width > 0)
		printf("    send -> %d\n", rc);
	else
		printf(" -> %d\n", rc);
	return rc;
}
int avcodec_receive_packet(AVCodecContext *c, AVPacket *pkt)
{
	(void)c;
	static uint8_t data[64];
	if (!g_host.recv_n) {
		printf("av.receive_packet -> eagain\n");
		return AVERROR(EAGAIN);
	}
	int rc = g_host.recv_q[0].rc, size = g_host.recv_q[0].size;
	pkt->pts = g_host.recv_q[0].pts;
	pkt->dts = g_host.recv_q[0].dts;
	pkt->flags = g_host.recv_q[0].flags;
	memmove(g_host.recv_q, g_host.recv_q + 1, sizeof(g_host.recv_q[0]) * (size_t)--g_host.recv_n);
	for (int i = 0; i < 64; i++)
		data[i] = (uint8_t)(0xA0 + i);
	pkt->data = size ? data : NULL;
	pkt->size = size;
	printf("av.receive_packet -> %d size=%d pts=%lld dts=%lld flags=%d\n", rc, size, (long long)pkt->pts,
	       (long long)pkt->dts, pkt->flags);
	return rc;
}
void av_packet_unref(AVPacket *pkt)
{
	(void)pkt;
	printf("av.packet_unref\n");
}
