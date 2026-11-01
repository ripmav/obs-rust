/*
 * Conformance runner for media playback (spec S-036): shared/media-playback (media.c, decode.c, cache.c,
 * media-playback.c, closest-format.h). The C sources are compiled unchanged against a scripted FFmpeg
 * (gen/mediaplayback_stubs.c), a fake clock and recording callbacks.
 *
 * media.c and cache.c are #included (not linked) so that the runner can call their file-local helpers and drive
 * the playback state machine one step at a time without threads ("sync" commands); the real threaded
 * public API (media_playback_*) is covered by the "t_" commands.
 *
 * Test tooling, not part of libobs. Script language: test/conformance/README.md, section "mediaplayback runner".
 */
#include <unistd.h>
#include <pthread.h>

#include "mediaplayback_api.h"
#include "script.h"
#include "util/base.h"

/* ---- the code under test ---- */
#include "../../../shared/media-playback/media-playback/media.c"
/* cache.c has file-local helpers with the same names as media.c's: rename them for this translation unit */
#include "../../../shared/media-playback/media-playback/cache.h"
#define base_sys_ts cache_base_sys_ts
#define reset_ts(x) cache_reset_ts(x)
#define seek_to cache_seek_to
#define mp_kill_thread cache_kill_thread
#include "../../../shared/media-playback/media-playback/cache.c"
#undef base_sys_ts
#undef reset_ts
#undef seek_to
#undef mp_kill_thread

/* all output goes through the ordered queue */
#define printf mpt_out

/* ------------------------------------------------------------------ names */

static const char *vf_name(enum video_format f)
{
	switch (f) {
	case VIDEO_FORMAT_NONE: return "NONE";
	case VIDEO_FORMAT_I420: return "I420";
	case VIDEO_FORMAT_NV12: return "NV12";
	case VIDEO_FORMAT_YVYU: return "YVYU";
	case VIDEO_FORMAT_YUY2: return "YUY2";
	case VIDEO_FORMAT_UYVY: return "UYVY";
	case VIDEO_FORMAT_RGBA: return "RGBA";
	case VIDEO_FORMAT_BGRA: return "BGRA";
	case VIDEO_FORMAT_BGRX: return "BGRX";
	case VIDEO_FORMAT_Y800: return "Y800";
	case VIDEO_FORMAT_I444: return "I444";
	case VIDEO_FORMAT_BGR3: return "BGR3";
	case VIDEO_FORMAT_I422: return "I422";
	case VIDEO_FORMAT_I40A: return "I40A";
	case VIDEO_FORMAT_I42A: return "I42A";
	case VIDEO_FORMAT_YUVA: return "YUVA";
	case VIDEO_FORMAT_AYUV: return "AYUV";
	case VIDEO_FORMAT_I010: return "I010";
	case VIDEO_FORMAT_P010: return "P010";
	case VIDEO_FORMAT_I210: return "I210";
	case VIDEO_FORMAT_I412: return "I412";
	case VIDEO_FORMAT_YA2L: return "YA2L";
	default: return "OTHER";
	}
}

static const char *af_name(enum audio_format f)
{
	switch (f) {
	case AUDIO_FORMAT_UNKNOWN: return "UNKNOWN";
	case AUDIO_FORMAT_U8BIT: return "U8BIT";
	case AUDIO_FORMAT_16BIT: return "16BIT";
	case AUDIO_FORMAT_32BIT: return "32BIT";
	case AUDIO_FORMAT_FLOAT: return "FLOAT";
	case AUDIO_FORMAT_U8BIT_PLANAR: return "U8BIT_PLANAR";
	case AUDIO_FORMAT_16BIT_PLANAR: return "16BIT_PLANAR";
	case AUDIO_FORMAT_32BIT_PLANAR: return "32BIT_PLANAR";
	case AUDIO_FORMAT_FLOAT_PLANAR: return "FLOAT_PLANAR";
	default: return "OTHER";
	}
}

static const char *sp_name(enum speaker_layout s)
{
	switch (s) {
	case SPEAKERS_UNKNOWN: return "UNKNOWN";
	case SPEAKERS_MONO: return "MONO";
	case SPEAKERS_STEREO: return "STEREO";
	case SPEAKERS_2POINT1: return "2POINT1";
	case SPEAKERS_4POINT0: return "4POINT0";
	case SPEAKERS_4POINT1: return "4POINT1";
	case SPEAKERS_5POINT1: return "5POINT1";
	case SPEAKERS_7POINT1: return "7POINT1";
	default: return "OTHER";
	}
}

static const char *cs_name(enum video_colorspace s)
{
	switch (s) {
	case VIDEO_CS_DEFAULT: return "DEFAULT";
	case VIDEO_CS_601: return "601";
	case VIDEO_CS_709: return "709";
	case VIDEO_CS_SRGB: return "SRGB";
	case VIDEO_CS_2100_PQ: return "2100_PQ";
	case VIDEO_CS_2100_HLG: return "2100_HLG";
	default: return "OTHER";
	}
}

static const char *trc_name(enum video_trc t)
{
	switch (t) {
	case VIDEO_TRC_DEFAULT: return "DEFAULT";
	case VIDEO_TRC_SRGB: return "SRGB";
	case VIDEO_TRC_PQ: return "PQ";
	case VIDEO_TRC_HLG: return "HLG";
	default: return "OTHER";
	}
}

static const char *range_name(enum video_range_type r)
{
	return r == VIDEO_RANGE_FULL ? "FULL" : (r == VIDEO_RANGE_PARTIAL ? "PARTIAL" : "DEFAULT");
}

/* ------------------------------------------------------- callbacks (shared) */

static bool g_print_pix = true;
static int g_v_count, g_a_count, g_stop_count, g_pre_count, g_seek_count;
static media_playback_t *g_mp; /* threaded object, used by the auto-stop hook */
static int g_stop_after_v = -1;
static pthread_mutex_t ev_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t ev_cond = PTHREAD_COND_INITIALIZER;
static int g_events;

static void event_done(int *counter)
{
	pthread_mutex_lock(&ev_lock);
	(*counter)++;
	g_events++;
	pthread_cond_broadcast(&ev_cond);
	pthread_mutex_unlock(&ev_lock);
}

static void print_frame(const char *tag, struct obs_source_frame *f)
{
	printf("cb %s ts=%lld fmt=%s %ux%u flip=%d full=%d trc=%s maxlum=%u flags=%u", tag, (long long)f->timestamp,
	       vf_name(f->format), f->width, f->height, f->flip, f->full_range, trc_name(f->trc), f->max_luminance,
	       f->flags);
	if (g_print_pix && f->data[0])
		printf(" ls0=%u d0=%02x", f->linesize[0], f->data[0][0]);
	printf(" mat0=%.6g min0=%.6g max0=%.6g\n", f->color_matrix[0], f->color_range_min[0], f->color_range_max[0]);
}

static void on_video(void *op, struct obs_source_frame *f)
{
	(void)op;
	print_frame("video", f);
	if (g_mp && g_stop_after_v == g_v_count + 1)
		media_playback_stop(g_mp);
	event_done(&g_v_count);
}

static void on_preload(void *op, struct obs_source_frame *f)
{
	(void)op;
	print_frame("preload", f);
	event_done(&g_pre_count);
}

static void on_seek(void *op, struct obs_source_frame *f)
{
	(void)op;
	print_frame("seek", f);
	event_done(&g_seek_count);
}

static void on_audio(void *op, struct obs_source_audio *a)
{
	(void)op;
	printf("cb audio ts=%lld fmt=%s spk=%s rate=%u frames=%u", (long long)a->timestamp, af_name(a->format),
	       sp_name(a->speakers), a->samples_per_sec, a->frames);
	if (g_print_pix && a->data[0])
		printf(" d0=%02x", a->data[0][0]);
	printf("\n");
	event_done(&g_a_count);
}

static void on_stop(void *op)
{
	(void)op;
	printf("cb stop\n");
	event_done(&g_stop_count);
}

static void log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[2048];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	for (char *p = msg; *p; p++)
		if (*p == '\n')
			*p = ' ';
	const char *name = level == LOG_ERROR     ? "error"
			   : level == LOG_WARNING ? "warning"
			   : level == LOG_INFO    ? "info"
						  : "debug";
	/* only the level and the stable "MP:" prefix texts are part of the contract; see spec */
	mpt_out("log %s %s\n", name, msg);
}

/* ------------------------------------------------------------ arg helpers */

static const char *kv(struct script_line *sl, int from, const char *key)
{
	size_t kl = strlen(key);
	for (int i = from; i < sl->argc; i++)
		if (!strncmp(sl->argv[i], key, kl) && sl->argv[i][kl] == '=')
			return sl->argv[i] + kl + 1;
	return NULL;
}

static long long kvi(struct script_line *sl, int from, const char *key, long long def)
{
	const char *v = kv(sl, from, key);
	return v ? strtoll(v, NULL, 10) : def;
}

static long long num(const char *s)
{
	if (!strcmp(s, "none") || !strcmp(s, "nopts"))
		return AV_NOPTS_VALUE;
	return strtoll(s, NULL, 10);
}

static int tok_cs(const char *s)
{
	if (!s)
		return AVCOL_SPC_UNSPECIFIED;
	if (!strcmp(s, "709")) return AVCOL_SPC_BT709;
	if (!strcmp(s, "fcc")) return AVCOL_SPC_FCC;
	if (!strcmp(s, "470bg")) return AVCOL_SPC_BT470BG;
	if (!strcmp(s, "170m")) return AVCOL_SPC_SMPTE170M;
	if (!strcmp(s, "240m")) return AVCOL_SPC_SMPTE240M;
	if (!strcmp(s, "2020")) return AVCOL_SPC_BT2020_NCL;
	if (!strcmp(s, "rgb")) return AVCOL_SPC_RGB;
	return AVCOL_SPC_UNSPECIFIED;
}

static int tok_trc(const char *s)
{
	if (!s)
		return AVCOL_TRC_UNSPECIFIED;
	if (!strcmp(s, "709")) return AVCOL_TRC_BT709;
	if (!strcmp(s, "g22")) return AVCOL_TRC_GAMMA22;
	if (!strcmp(s, "g28")) return AVCOL_TRC_GAMMA28;
	if (!strcmp(s, "170m")) return AVCOL_TRC_SMPTE170M;
	if (!strcmp(s, "240m")) return AVCOL_TRC_SMPTE240M;
	if (!strcmp(s, "srgb")) return AVCOL_TRC_IEC61966_2_1;
	if (!strcmp(s, "pq")) return AVCOL_TRC_SMPTE2084;
	if (!strcmp(s, "hlg")) return AVCOL_TRC_ARIB_STD_B67;
	return AVCOL_TRC_UNSPECIFIED;
}

static int tok_codec(const char *s, int audio)
{
	if (!s)
		return audio ? AV_CODEC_ID_AAC : AV_CODEC_ID_H264;
	if (!strcmp(s, "vp8")) return AV_CODEC_ID_VP8;
	if (!strcmp(s, "vp9")) return AV_CODEC_ID_VP9;
	if (!strcmp(s, "png")) return AV_CODEC_ID_PNG;
	if (!strcmp(s, "none")) return AV_CODEC_ID_NONE;
	return audio ? AV_CODEC_ID_AAC : AV_CODEC_ID_H264;
}

static void parse_rat(const char *s, int *n, int *d, int dn, int dd)
{
	*n = dn;
	*d = dd;
	if (s)
		sscanf(s, "%d/%d", n, d);
}

/* ------------------------------------------------------------ media defs */

static struct mpt_media *def_cur;

static void def_begin(struct script_line *sl)
{
	script_need(sl, 4);
	if (g_media_count >= MPT_MAX_MEDIA) {
		fprintf(stderr, "too many media\n");
		exit(2);
	}
	def_cur = &g_media[g_media_count++];
	memset(def_cur, 0, sizeof(*def_cur));
	snprintf(def_cur->name, sizeof(def_cur->name), "%s", sl->argv[1]);
	def_cur->duration = num(sl->argv[2]);
	def_cur->start_time = num(sl->argv[3]);
}

static void def_line(struct script_line *sl)
{
	const char *c = sl->argv[0];
	struct mpt_media *d = def_cur;
	if (!strcmp(c, "video") || !strcmp(c, "audio")) {
		bool audio = c[0] == 'a';
		struct mpt_stream *s = &d->streams[d->nstreams++];
		memset(s, 0, sizeof(*s));
		s->audio = audio;
		s->tb_num = atoi(sl->argv[1]);
		s->tb_den = atoi(sl->argv[2]);
		if (audio) {
			script_need(sl, 6);
			s->sample_rate = atoi(sl->argv[3]);
			s->channels = atoi(sl->argv[4]);
			s->sample_fmt = av_get_sample_fmt(sl->argv[5]);
		} else {
			script_need(sl, 6);
			s->w = atoi(sl->argv[3]);
			s->h = atoi(sl->argv[4]);
			s->pix_fmt = av_get_pix_fmt(sl->argv[5]);
			if (s->pix_fmt == AV_PIX_FMT_NONE) {
				fprintf(stderr, "bad pix fmt %s\n", sl->argv[5]);
				exit(2);
			}
		}
		int from = 6;
		s->nb_frames = kvi(sl, from, "frames", 0);
		parse_rat(kv(sl, from, "fps"), &s->fps_num, &s->fps_den, 0, 1);
		parse_rat(kv(sl, from, "dectb"), &s->dectb_num, &s->dectb_den, 1, 25);
		s->cs = tok_cs(kv(sl, from, "cs"));
		s->trc = tok_trc(kv(sl, from, "trc"));
		s->pri = kv(sl, from, "pri") ? AVCOL_PRI_BT2020 : AVCOL_PRI_UNSPECIFIED;
		s->range = kv(sl, from, "range") ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG;
		s->codec_id = tok_codec(kv(sl, from, "codec"), audio);
		s->alpha_mode = (int)kvi(sl, from, "alpha", 0);
		s->cll = (int)kvi(sl, from, "cll", 0);
		s->mdcv = (int)kvi(sl, from, "mdcv", 0);
		s->latency = (int)kvi(sl, from, "latency", 0);
		s->flip = kvi(sl, from, "flip", 0) != 0;
		s->open_fail = kvi(sl, from, "openfail", 0) != 0;
	} else if (!strcmp(c, "pkt")) {
		script_need(sl, 3);
		struct mpt_pkt *p = &d->pkts[d->npkts++];
		memset(p, 0, sizeof(*p));
		p->stream = atoi(sl->argv[1]);
		p->nopts = !strcmp(sl->argv[2], "nopts");
		p->pts = p->nopts ? 0 : num(sl->argv[2]);
		p->size = (int)kvi(sl, 3, "size", 4);
		p->key = kvi(sl, 3, "key", 1) != 0;
		p->fdur = kvi(sl, 3, "fdur", 0);
		p->has_bet = kv(sl, 3, "bet") != NULL;
		p->nopts_bet = kv(sl, 3, "bet") && !strcmp(kv(sl, 3, "bet"), "nopts");
		p->bet = p->has_bet && !p->nopts_bet ? num(kv(sl, 3, "bet")) : 0;
		p->samples = (int)kvi(sl, 3, "samples", 0);
		p->decerr = kvi(sl, 3, "decerr", 0) != 0;
	} else if (!strcmp(c, "rderr")) {
		script_need(sl, 2);
		struct mpt_pkt *p = &d->pkts[d->npkts++];
		memset(p, 0, sizeof(*p));
		p->rderr = !strcmp(sl->argv[1], "exit") ? AVERROR_EXIT : AVERROR(EIO);
	} else if (!strcmp(c, "fail")) {
		script_need(sl, 2);
		if (!strcmp(sl->argv[1], "open")) d->open_fail = true;
		if (!strcmp(sl->argv[1], "info")) d->info_fail = true;
		if (!strcmp(sl->argv[1], "seek")) d->seek_fail = true;
	} else {
		fprintf(stderr, "line %d: unknown media-definition command %s\n", sl->lineno, c);
		exit(2);
	}
}

/* --------------------------------------------------- generic field access */

struct field {
	const char *name;
	size_t off;
	int size; /* 1 = bool, 4 = int, 8 = int64 */
};

#define F(T, n, s) {#n, offsetof(T, n), s}
static const struct field m_fields[] = {
	F(mp_media_t, eof, 1), F(mp_media_t, active, 1), F(mp_media_t, stopping, 1), F(mp_media_t, looping, 1),
	F(mp_media_t, pause, 1), F(mp_media_t, reset, 1), F(mp_media_t, seek, 1), F(mp_media_t, seek_next_ts, 1),
	F(mp_media_t, reset_ts, 1), F(mp_media_t, preload_frame, 1), F(mp_media_t, kill, 1),
	F(mp_media_t, request_preload, 1), F(mp_media_t, thread_valid, 1), F(mp_media_t, is_local_file, 1), F(mp_media_t, reconnecting, 1),
	F(mp_media_t, speed, 4), F(mp_media_t, play_sys_ts, 8), F(mp_media_t, next_ns, 8),
	F(mp_media_t, base_ts, 8), F(mp_media_t, start_ts, 8), F(mp_media_t, next_pts_ns, 8),
	F(mp_media_t, seek_pos, 8), {NULL, 0, 0}};
static const struct field c_fields[] = {
	F(mp_cache_t, eof, 1), F(mp_cache_t, active, 1), F(mp_cache_t, stopping, 1), F(mp_cache_t, looping, 1),
	F(mp_cache_t, pause, 1), F(mp_cache_t, reset, 1), F(mp_cache_t, seek, 1), F(mp_cache_t, seek_next_ts, 1),
	F(mp_cache_t, reset_ts, 1), F(mp_cache_t, preload_frame, 1), F(mp_cache_t, kill, 1),
	F(mp_cache_t, request_preload, 1), F(mp_cache_t, thread_valid, 1), F(mp_cache_t, speed, 4), F(mp_cache_t, play_sys_ts, 8),
	F(mp_cache_t, next_ns, 8), F(mp_cache_t, base_ts, 8), F(mp_cache_t, start_ts, 8),
	F(mp_cache_t, next_pts_ns, 8), F(mp_cache_t, seek_pos, 8), F(mp_cache_t, next_v_ts, 8),
	F(mp_cache_t, next_a_ts, 8), F(mp_cache_t, start_time, 8), F(mp_cache_t, media_duration, 8),
	{NULL, 0, 0}};

static void set_field(const struct field *tab, void *obj, const char *name, long long v)
{
	for (const struct field *f = tab; f->name; f++) {
		if (strcmp(f->name, name))
			continue;
		char *p = (char *)obj + f->off;
		if (f->size == 1)
			*(bool *)p = v != 0;
		else if (f->size == 4)
			*(int *)p = (int)v;
		else
			*(int64_t *)p = v;
		return;
	}
	fprintf(stderr, "unknown field %s\n", name);
	exit(2);
}

/* ------------------------------------------------------------- sync media */

static mp_media_t M;
static bool m_alive;
static mp_cache_t C;
static bool c_alive;
static char path_buf[128], fmt_buf[64], opt_buf[256];

static bool has_cb(const char *list, const char *name)
{
	size_t n = strlen(name);
	for (const char *p = list; *p;) {
		const char *e = strchr(p, ',');
		size_t l = e ? (size_t)(e - p) : strlen(p);
		if (l == n && !strncmp(p, name, n))
			return true;
		p += l + (e ? 1 : 0);
	}
	return false;
}

static void build_info(struct script_line *sl, int from, struct mp_media_info *info)
{
	memset(info, 0, sizeof(*info));
	snprintf(path_buf, sizeof(path_buf), "%s", sl->argv[1]);
	info->path = path_buf;
	const char *f = kv(sl, from, "format");
	if (f) {
		snprintf(fmt_buf, sizeof(fmt_buf), "%s", f);
		info->format = fmt_buf;
	}
	const char *o = kv(sl, from, "options");
	if (o) {
		snprintf(opt_buf, sizeof(opt_buf), "%s", o);
		for (char *p = opt_buf; *p; p++)
			if (*p == '_')
				*p = ' ';
		info->ffmpeg_options = opt_buf;
	}
	info->is_local_file = kvi(sl, from, "local", 0) != 0;
	info->speed = (int)kvi(sl, from, "speed", 100);
	info->buffering = (int)kvi(sl, from, "buffering", 0);
	info->reconnecting = kvi(sl, from, "reconnecting", 0) != 0;
	info->request_preload = kvi(sl, from, "request_preload", 0) != 0;
	info->is_linear_alpha = kvi(sl, from, "linear", 0) != 0;
	info->hardware_decoding = false;
	const char *r = kv(sl, from, "range");
	info->force_range = r && !strcmp(r, "full")      ? VIDEO_RANGE_FULL
			    : r && !strcmp(r, "partial") ? VIDEO_RANGE_PARTIAL
							  : VIDEO_RANGE_DEFAULT;
	const char *cbs = kv(sl, from, "cbs");
	if (!cbs)
		cbs = "v,a,pre,seek,stop";
	info->v_cb = has_cb(cbs, "v") ? on_video : NULL;
	info->a_cb = has_cb(cbs, "a") ? on_audio : NULL;
	info->v_preload_cb = has_cb(cbs, "pre") ? on_preload : NULL;
	info->v_seek_cb = has_cb(cbs, "seek") ? on_seek : NULL;
	info->stop_cb = has_cb(cbs, "stop") ? on_stop : NULL;
	info->full_decode = false;
}

static void print_decode(const char *n, struct mp_decode *d)
{
	printf("  %s ready=%d pts=%lld next_pts=%lld last_dur=%lld eof=%d key=%d pending=%d npkts=%zu\n", n,
	       d->frame_ready, (long long)d->frame_pts, (long long)d->next_pts, (long long)d->last_duration, d->eof,
	       d->got_first_keyframe, d->packet_pending, d->packets.size / sizeof(void *));
}

static void m_state(void)
{
	mp_media_t *m = &M;
	printf("mstate eof=%d active=%d stopping=%d looping=%d pause=%d reset=%d seek=%d seek_next_ts=%d reset_ts=%d "
	       "preload_frame=%d has_v=%d has_a=%d\n",
	       m->eof, m->active, m->stopping, m->looping, m->pause, m->reset, m->seek, m->seek_next_ts, m->reset_ts,
	       m->preload_frame, m->has_video, m->has_audio);
	printf("  base_ts=%lld start_ts=%lld play_sys_ts=%lld next_pts_ns=%lld next_ns=%llu seek_pos=%lld speed=%d "
	       "cur_space=%s cur_range=%s\n",
	       (long long)m->base_ts, (long long)m->start_ts, (long long)m->play_sys_ts, (long long)m->next_pts_ns,
	       (unsigned long long)m->next_ns, (long long)m->seek_pos, m->speed, cs_name(m->cur_space),
	       range_name(m->cur_range));
	if (m->has_video)
		print_decode("v", &m->v);
	if (m->has_audio)
		print_decode("a", &m->a);
}

/* One iteration of the media thread's main loop with the semaphore wait removed (the script decides when the
 * thread "wakes"). Mirrors mp_media_thread(); every branch calls the real functions. */
static const char *m_tick(mp_media_t *m, int stale_active, int stale_pause)
{
	bool reset, kill, is_active, seek, pause, reset_time, preload_frame;
	int64_t seek_pos;
	bool timeout = false;

	pthread_mutex_lock(&m->mutex);
	is_active = m->active;
	pause = m->pause;
	pthread_mutex_unlock(&m->mutex);
	/* stale_*: the thread went to sleep on the semaphore with these values read before the wait */
	if (stale_active >= 0) {
		is_active = stale_active != 0;
		pause = stale_pause != 0;
	}

	if (!is_active || pause) {
		if (pause)
			reset_ts(m);
	} else {
		timeout = mp_media_sleep(m);
	}

	pthread_mutex_lock(&m->mutex);
	reset = m->reset;
	kill = m->kill;
	m->reset = false;
	m->kill = false;
	preload_frame = m->preload_frame;
	pause = m->pause;
	seek_pos = m->seek_pos;
	seek = m->seek;
	reset_time = m->reset_ts;
	m->preload_frame = false;
	m->seek = false;
	m->reset_ts = false;
	pthread_mutex_unlock(&m->mutex);

	if (kill)
		return "kill";
	if (reset) {
		mp_media_reset(m);
		return "reset";
	}
	if (seek) {
		m->seek_next_ts = true;
		seek_to(m, seek_pos);
		return "seek";
	}
	if (reset_time) {
		reset_ts(m);
		return "reset_ts";
	}
	if (pause)
		return "paused";
	if (preload_frame && m->obsframe.data[0] && !is_active) {
		m->v_preload_cb(m->opaque, &m->obsframe);
	}
	if (is_active && !timeout) {
		if (m->has_video)
			mp_media_next_video(m, false);
		if (m->has_audio)
			mp_media_next_audio(m);
		if (!mp_media_prepare_frames(m)) {
			if (m->stop_cb)
				m->stop_cb(m->opaque);
			return "thread-exit";
		}
		if (mp_media_eof(m))
			return "eof";
		mp_media_calc_next_ns(m);
		return "played";
	}
	if (is_active && timeout)
		return "timeout";
	return "idle";
}

static void c_state(void)
{
	mp_cache_t *c = &C;
	printf("cstate frames=%zu segs=%zu has_v=%d has_a=%d active=%d stopping=%d looping=%d pause=%d seek_next_ts=%d\n",
	       c->video_frames.num, c->audio_segments.num, c->has_video, c->has_audio, c->active, c->stopping,
	       c->looping, c->pause, c->seek_next_ts);
	printf("  cur_v=%zu next_v=%zu cur_a=%zu next_a=%zu next_v_ts=%lld next_a_ts=%lld final_v=%lld final_a=%lld\n",
	       c->cur_v_idx, c->next_v_idx, c->cur_a_idx, c->next_a_idx, (long long)c->next_v_ts,
	       (long long)c->next_a_ts, (long long)c->final_v_duration, (long long)c->final_a_duration);
	printf("  base_ts=%lld start_ts=%lld play_sys_ts=%lld next_pts_ns=%lld next_ns=%llu start_time=%lld "
	       "media_duration=%lld speed=%d\n",
	       (long long)c->base_ts, (long long)c->start_ts, (long long)c->play_sys_ts, (long long)c->next_pts_ns,
	       (unsigned long long)c->next_ns, (long long)c->start_time, (long long)c->media_duration, c->speed);
}

static const char *c_tick(mp_cache_t *c, int stale_active, int stale_pause)
{
	bool reset, kill, is_active, seek, pause, reset_time, preload_frame;
	int64_t seek_pos;
	bool timeout = false;

	pthread_mutex_lock(&c->mutex);
	is_active = c->active;
	pause = c->pause;
	pthread_mutex_unlock(&c->mutex);
	/* stale_*: the thread went to sleep on the semaphore with these values read before the wait */
	if (stale_active >= 0) {
		is_active = stale_active != 0;
		pause = stale_pause != 0;
	}

	if (!is_active || pause) {
		if (pause)
			cache_reset_ts(c);
	} else {
		timeout = mp_cache_sleep(c);
	}

	pthread_mutex_lock(&c->mutex);
	reset = c->reset;
	kill = c->kill;
	c->reset = false;
	c->kill = false;
	preload_frame = c->preload_frame;
	pause = c->pause;
	seek_pos = c->seek_pos;
	seek = c->seek;
	reset_time = c->reset_ts;
	c->preload_frame = false;
	c->seek = false;
	c->reset_ts = false;
	pthread_mutex_unlock(&c->mutex);

	if (kill)
		return "kill";
	if (reset) {
		mp_cache_reset(c);
		return "reset";
	}
	if (seek) {
		c->seek_next_ts = true;
		cache_seek_to(c, seek_pos);
		return "seek";
	}
	if (reset_time) {
		cache_reset_ts(c);
		return "reset_ts";
	}
	if (pause)
		return "paused";
	if (preload_frame)
		c->v_preload_cb(c->opaque, &c->video_frames.array[0]);
	if (is_active && !timeout) {
		if (c->has_video)
			mp_cache_next_video(c, false);
		if (c->has_audio)
			mp_cache_next_audio(c);
		if (mp_cache_eof(c))
			return "eof";
		mp_cache_calc_next_ns(c);
		return "played";
	}
	if (is_active && timeout)
		return "timeout";
	return "idle";
}

/* Same steps as mp_cache_init() minus pthread_create(); the decode pass then runs synchronously. */
static bool c_init(const struct mp_media_info *info)
{
	mp_cache_t *c = &C;
	memset(c, 0, sizeof(*c));
	struct mp_media_info info2 = *info;
	info2.opaque = c;
	info2.v_cb = fill_video;
	info2.a_cb = fill_audio;
	info2.v_preload_cb = NULL;
	info2.v_seek_cb = NULL;
	info2.stop_cb = NULL;
	info2.full_decode = true;

	mp_media_t *m = &c->m;
	pthread_mutex_init_value(&c->mutex);
	if (!mp_media_init(m, &info2)) {
		mp_cache_free(c);
		return false;
	}
	if (!mp_media_init2(m)) {
		mp_cache_free(c);
		return false;
	}
	c->opaque = info->opaque;
	c->v_cb = info->v_cb;
	c->a_cb = info->a_cb;
	c->stop_cb = info->stop_cb;
	c->ffmpeg_options = info->ffmpeg_options;
	c->v_seek_cb = info->v_seek_cb;
	c->v_preload_cb = info->v_preload_cb;
	c->request_preload = info->request_preload;
	c->speed = info->speed;
	c->media_duration = m->fmt->duration;
	c->has_video = m->has_video;
	c->has_audio = m->has_audio;
	if (!cache_base_sys_ts)
		cache_base_sys_ts = (int64_t)os_gettime_ns();
	pthread_mutex_init(&c->mutex, NULL);
	os_sem_init(&c->sem, 0);
	c->path = info->path ? bstrdup(info->path) : NULL;
	c->format_name = info->format ? bstrdup(info->format) : NULL;
	return true;
}

/* -------------------------------------------------------------- threaded */

static void wait_events(int n)
{
	pthread_mutex_lock(&ev_lock);
	while (g_events < n)
		pthread_cond_wait(&ev_cond, &ev_lock);
	pthread_mutex_unlock(&ev_lock);
}

static void wait_stop(int n)
{
	pthread_mutex_lock(&ev_lock);
	while (g_stop_count < n)
		pthread_cond_wait(&ev_cond, &ev_lock);
	pthread_mutex_unlock(&ev_lock);
}

/* ------------------------------------------------------------------ main */

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <script>\n", argv[0]);
		return 2;
	}
	FILE *f = fopen(argv[1], "r");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	base_set_log_handler(log_handler, NULL);
	setvbuf(stdout, NULL, _IOLBF, 0);

	struct script_line sl;
	int lineno = 0;
	bool threaded = false;

	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		if (def_cur) {
			if (!strcmp(c, "endmedia"))
				def_cur = NULL;
			else
				def_line(&sl);
			continue;
		}
		if (!strcmp(c, "media")) {
			def_begin(&sl);
		} else if (!strcmp(c, "libvpx")) {
			script_need(&sl, 2);
			g_have_libvpx = atoi(sl.argv[1]) != 0;
		} else if (!strcmp(c, "clock")) {
			script_need(&sl, 2);
			mpt_set_clock((uint64_t)strtoull(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "pix")) {
			script_need(&sl, 2);
			g_print_pix = !strcmp(sl.argv[1], "on");
		} else if (!strcmp(c, "trace_sleep")) {
			script_need(&sl, 2);
			mp_trace_sleep = atoi(sl.argv[1]);
		} else if (!strcmp(c, "echo")) {
			printf("# ");
			for (int i = 1; i < sl.argc; i++)
				printf("%s%s", i > 1 ? " " : "", sl.argv[i]);
			printf("\n");
		}
		/* ---- pure helpers ---- */
		else if (!strcmp(c, "closest")) {
			script_need(&sl, 2);
			enum AVPixelFormat in = av_get_pix_fmt(sl.argv[1]);
			if (in == AV_PIX_FMT_NONE && strcmp(sl.argv[1], "none")) {
				fprintf(stderr, "bad pix fmt\n");
				return 2;
			}
			enum AVPixelFormat out = closest_format(in);
			printf("closest %s -> %s -> %s\n", sl.argv[1], av_get_pix_fmt_name(out),
			       vf_name(convert_pixel_format(out)));
		} else if (!strcmp(c, "cv_pixfmt")) {
			script_need(&sl, 2);
			printf("cv_pixfmt %s -> %s\n", sl.argv[1], vf_name(convert_pixel_format(av_get_pix_fmt(sl.argv[1]))));
		} else if (!strcmp(c, "cv_sample")) {
			script_need(&sl, 2);
			printf("cv_sample %s -> %s\n", sl.argv[1], af_name(convert_sample_format(av_get_sample_fmt(sl.argv[1]))));
		} else if (!strcmp(c, "cv_speakers")) {
			script_need(&sl, 2);
			printf("cv_speakers %s -> %s\n", sl.argv[1], sp_name(convert_speaker_layout((uint8_t)atoi(sl.argv[1]))));
		} else if (!strcmp(c, "cv_space")) {
			script_need(&sl, 4);
			printf("cv_space %s %s %s -> %s\n", sl.argv[1], sl.argv[2], sl.argv[3],
			       cs_name(convert_color_space(tok_cs(sl.argv[1]), tok_trc(sl.argv[2]),
							   strcmp(sl.argv[3], "2020") ? AVCOL_PRI_UNSPECIFIED
										       : AVCOL_PRI_BT2020)));
		} else if (!strcmp(c, "cv_range")) {
			script_need(&sl, 2);
			printf("cv_range %s -> %s\n", sl.argv[1],
			       range_name(convert_color_range(!strcmp(sl.argv[1], "jpeg") ? AVCOL_RANGE_JPEG : AVCOL_RANGE_MPEG)));
		}
		/* ---- sync media ---- */
		else if (!strcmp(c, "minit")) {
			script_need(&sl, 2);
			struct mp_media_info info;
			build_info(&sl, 2, &info);
			/* full_decode=true makes mp_media_init skip the thread; the flag itself is not stored */
			info.full_decode = true;
			bool ok = mp_media_init(&M, &info);
			m_alive = ok;
			printf("minit -> %s\n", ok ? "ok" : "fail");
			if (ok && kvi(&sl, 2, "init2", 1)) {
				bool ok2 = mp_media_init2(&M);
				printf("minit2 -> %s has_v=%d has_a=%d\n", ok2 ? "ok" : "fail", M.has_video, M.has_audio);
			}
		} else if (!strcmp(c, "minit2")) {
			bool ok2 = mp_media_init2(&M);
			printf("minit2 -> %s has_v=%d has_a=%d\n", ok2 ? "ok" : "fail", M.has_video, M.has_audio);
		} else if (!strcmp(c, "mreset")) {
			printf("mreset -> %d\n", mp_media_reset(&M));
		} else if (!strcmp(c, "mprepare")) {
			printf("mprepare -> %d\n", mp_media_prepare_frames(&M));
		} else if (!strcmp(c, "mdecode")) {
			script_need(&sl, 2);
			printf("mdecode %s -> %d\n", sl.argv[1], mp_decode_next(sl.argv[1][0] == 'a' ? &M.a : &M.v));
		} else if (!strcmp(c, "mnextv")) {
			mp_media_next_video(&M, sl.argc > 1 && !strcmp(sl.argv[1], "preload"));
		} else if (!strcmp(c, "mnexta")) {
			mp_media_next_audio(&M);
		} else if (!strcmp(c, "mnextpkt")) {
			int r = mp_media_next_packet(&M);
			printf("mnextpkt -> %s\n", r >= 0 ? "ok" : (r == AVERROR_EOF ? "eof" : (r == AVERROR_EXIT ? "exit" : "error")));
		} else if (!strcmp(c, "meof")) {
			printf("meof -> %d\n", mp_media_eof(&M));
		} else if (!strcmp(c, "mcalc")) {
			mp_media_calc_next_ns(&M);
		} else if (!strcmp(c, "mfull")) {
			script_need(&sl, 2);
			M.full_decode = atoi(sl.argv[1]) != 0;
		} else if (!strcmp(c, "mtick")) {
			int n = sl.argc > 1 ? atoi(sl.argv[1]) : 1;
			int sa = -1, sp = -1;
			for (int i = 0; i < n; i++)
				printf("tick -> %s\n", m_tick(&M, sa, sp));
		} else if (!strcmp(c, "mtickw")) {
			script_need(&sl, 3);
			int sa = atoi(sl.argv[1]), sp = atoi(sl.argv[2]);
			printf("tick -> %s\n", m_tick(&M, sa, sp));
		} else if (!strcmp(c, "mplay")) {
			script_need(&sl, 2);
			mp_media_play(&M, atoi(sl.argv[1]) != 0, sl.argc > 2 && atoi(sl.argv[2]) != 0);
		} else if (!strcmp(c, "mstop")) {
			mp_media_stop(&M);
		} else if (!strcmp(c, "mpause")) {
			script_need(&sl, 2);
			mp_media_play_pause(&M, atoi(sl.argv[1]) != 0);
		} else if (!strcmp(c, "mseek")) {
			script_need(&sl, 2);
			mp_media_seek(&M, strtoll(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "mpreload")) {
			mp_media_preload_frame(&M);
		} else if (!strcmp(c, "mset")) {
			script_need(&sl, 3);
			set_field(m_fields, &M, sl.argv[1], strtoll(sl.argv[2], NULL, 10));
		} else if (!strcmp(c, "mstate")) {
			m_state();
		} else if (!strcmp(c, "mtime")) {
			printf("mtime -> %lld\n", (long long)mp_media_get_current_time(&M));
		} else if (!strcmp(c, "mframes")) {
			printf("mframes -> %lld\n", (long long)mp_media_get_frames(&M));
		} else if (!strcmp(c, "mduration")) {
			printf("mduration -> %lld\n", (long long)mp_media_get_duration(&M));
		} else if (!strcmp(c, "mint")) {
			if (M.fmt && M.fmt->interrupt_callback.callback)
				printf("mint -> %d\n", M.fmt->interrupt_callback.callback(M.fmt->interrupt_callback.opaque));
			else
				printf("mint -> none\n");
		} else if (!strcmp(c, "mfree")) {
			mp_media_free(&M);
			m_alive = false;
			printf("mfree\n");
		}
		/* ---- sync cache ---- */
		else if (!strcmp(c, "cinit")) {
			script_need(&sl, 2);
			struct mp_media_info info;
			build_info(&sl, 2, &info);
			info.is_local_file = true;
			bool ok = c_init(&info);
			c_alive = ok;
			printf("cinit -> %s\n", ok ? "ok" : "fail");
			if (ok) {
				bool d = mp_cache_decode(&C);
				printf("cdecode -> %s frames=%zu segs=%zu\n", d ? "ok" : "fail", C.video_frames.num,
				       C.audio_segments.num);
			}
		} else if (!strcmp(c, "ctickw")) {
			script_need(&sl, 3);
			int sa = atoi(sl.argv[1]), sp = atoi(sl.argv[2]);
			printf("tick -> %s\n", c_tick(&C, sa, sp));
		} else if (!strcmp(c, "cplay")) {
			script_need(&sl, 2);
			mp_cache_play(&C, atoi(sl.argv[1]) != 0);
		} else if (!strcmp(c, "cstop")) {
			mp_cache_stop(&C);
		} else if (!strcmp(c, "cpause")) {
			script_need(&sl, 2);
			mp_cache_play_pause(&C, atoi(sl.argv[1]) != 0);
		} else if (!strcmp(c, "cseek")) {
			script_need(&sl, 2);
			mp_cache_seek(&C, strtoll(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "cseekraw")) {
			script_need(&sl, 2);
			cache_seek_to(&C, strtoll(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "cpreload")) {
			mp_cache_preload_frame(&C);
		} else if (!strcmp(c, "creset")) {
			printf("creset -> %d\n", mp_cache_reset(&C));
		} else if (!strcmp(c, "ctick")) {
			int n = sl.argc > 1 ? atoi(sl.argv[1]) : 1;
			int sa = -1, sp = -1;
			for (int i = 0; i < n; i++)
				printf("tick -> %s\n", c_tick(&C, sa, sp));
		} else if (!strcmp(c, "cset")) {
			script_need(&sl, 3);
			set_field(c_fields, &C, sl.argv[1], strtoll(sl.argv[2], NULL, 10));
		} else if (!strcmp(c, "cstate")) {
			c_state();
		} else if (!strcmp(c, "cdump")) {
			for (size_t i = 0; i < C.video_frames.num; i++)
				printf("cframe %zu ts=%lld %ux%u fmt=%s\n", i, (long long)C.video_frames.array[i].timestamp,
				       C.video_frames.array[i].width, C.video_frames.array[i].height,
				       vf_name(C.video_frames.array[i].format));
			for (size_t i = 0; i < C.audio_segments.num; i++)
				printf("csegment %zu ts=%lld frames=%u rate=%u\n", i,
				       (long long)C.audio_segments.array[i].timestamp, C.audio_segments.array[i].frames,
				       C.audio_segments.array[i].samples_per_sec);
		} else if (!strcmp(c, "ctime")) {
			printf("ctime -> %lld\n", (long long)mp_cache_get_current_time(&C));
		} else if (!strcmp(c, "cframes")) {
			printf("cframes -> %lld\n", (long long)mp_cache_get_frames(&C));
		} else if (!strcmp(c, "cduration")) {
			printf("cduration -> %lld\n", (long long)mp_cache_get_duration(&C));
		} else if (!strcmp(c, "cfree")) {
			mp_cache_free(&C);
			c_alive = false;
			printf("cfree\n");
		}
		/* ---- threaded public API ---- */
		else if (!strcmp(c, "t_create")) {
			script_need(&sl, 2);
			struct mp_media_info info;
			build_info(&sl, 2, &info);
			info.full_decode = kvi(&sl, 2, "full", 0) != 0;
			threaded = true;
			printf("t_create %s\n", sl.argv[1]);
			g_mp = media_playback_create(&info);
			if (!g_mp)
				printf("t_create -> null\n");
		} else if (!strcmp(c, "t_wait")) {
			script_need(&sl, 2);
			wait_events(atoi(sl.argv[1]));
		} else if (!strcmp(c, "t_wait_stop")) {
			script_need(&sl, 2);
			wait_stop(atoi(sl.argv[1]));
		} else if (!strcmp(c, "t_stop_after")) {
			script_need(&sl, 2);
			g_stop_after_v = atoi(sl.argv[1]);
		} else if (!strcmp(c, "t_play")) {
			script_need(&sl, 2);
			media_playback_play(g_mp, atoi(sl.argv[1]) != 0, sl.argc > 2 && atoi(sl.argv[2]) != 0);
		} else if (!strcmp(c, "t_stop")) {
			media_playback_stop(g_mp);
		} else if (!strcmp(c, "t_pause")) {
			script_need(&sl, 2);
			media_playback_play_pause(g_mp, atoi(sl.argv[1]) != 0);
		} else if (!strcmp(c, "t_seek")) {
			script_need(&sl, 2);
			media_playback_seek(g_mp, strtoll(sl.argv[1], NULL, 10));
		} else if (!strcmp(c, "t_preload")) {
			media_playback_preload_frame(g_mp);
		} else if (!strcmp(c, "t_loop")) {
			script_need(&sl, 2);
			media_playback_set_looping(g_mp, atoi(sl.argv[1]) != 0);
		} else if (!strcmp(c, "t_linear")) {
			script_need(&sl, 2);
			media_playback_set_is_linear_alpha(g_mp, atoi(sl.argv[1]) != 0);
		} else if (!strcmp(c, "t_query")) {
			printf("t_query time=%lld frames=%lld duration=%lld has_video=%d has_audio=%d\n",
			       (long long)media_playback_get_current_time(g_mp), (long long)media_playback_get_frames(g_mp),
			       (long long)media_playback_get_duration(g_mp), media_playback_has_video(g_mp),
			       media_playback_has_audio(g_mp));
		} else if (!strcmp(c, "t_null")) {
			/* NULL-handle tolerance of the dispatcher */
			media_playback_play(NULL, true, false);
			media_playback_play_pause(NULL, true);
			media_playback_stop(NULL);
			media_playback_preload_frame(NULL);
			media_playback_destroy(NULL);
			printf("t_null query time=%lld frames=%lld duration=%lld has_video=%d has_audio=%d\n",
			       (long long)media_playback_get_current_time(NULL), (long long)media_playback_get_frames(NULL),
			       (long long)media_playback_get_duration(NULL), media_playback_has_video(NULL),
			       media_playback_has_audio(NULL));
		} else if (!strcmp(c, "t_flush")) {
			/* nothing: output is flushed after every command below */
		} else if (!strcmp(c, "t_destroy")) {
			media_playback_destroy(g_mp);
			g_mp = NULL;
			printf("t_destroy\n");
		} else {
			fprintf(stderr, "line %d: unknown command %s\n", lineno, c);
			return 2;
		}
		(void)threaded;
		mpt_flush();
	}
	if (m_alive)
		mp_media_free(&M);
	if (c_alive)
		mp_cache_free(&C);
	mpt_flush();
	return 0;
}
