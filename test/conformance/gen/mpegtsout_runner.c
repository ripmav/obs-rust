/*
 * Conformance runner for spec S-059 (native MPEG-TS network output of obs-ffmpeg). The REAL plugins/obs-ffmpeg/
 * obs-ffmpeg-mpegts.c is #included (with obs-ffmpeg-srt.h, -rist.h, -url.h, -output.h, -formats.h); the muxer is the real
 * FFmpeg MPEG-TS muxer, libobs is a fake (mpegtsout_stubs.c), the network is a scripted fake (mpegtsout_net.c).
 * Script language: see test/conformance/README.md ("mpegtsout runner"). Test tooling, not part of OBS.
 */
#include <inttypes.h>
#include <errno.h>
#include <time.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netdb.h>
#include <obs-module.h>
#include <util/threading.h>
#include <util/deque.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/avstring.h>
#include <libavutil/mem.h>
#include <libavutil/pixdesc.h>
#include <libavcodec/avcodec.h>
#include "harness.h"
#include "mpegtsout_api.h"

/* ---- hooks (applied after all system / libav / libobs headers so that only the reference's own call sites change) ---- */
static int mts_write_hook(AVFormatContext *s, AVPacket *pkt);
static int mts_sem_post(os_sem_t *s);
static int mts_sem_wait(os_sem_t *s);
static int mts_event_try(os_event_t *e);
static int mw_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res);
static void mw_freeaddrinfo(struct addrinfo *ai);
static int mw_clock_gettime(clockid_t id, struct timespec *ts);
#define av_interleaved_write_frame mts_write_hook
#define os_sem_post mts_sem_post
#define os_sem_wait mts_sem_wait
#define os_event_try mts_event_try
#define getaddrinfo mw_getaddrinfo
#define freeaddrinfo mw_freeaddrinfo
#define clock_gettime mw_clock_gettime
#include "../../../plugins/obs-ffmpeg/obs-ffmpeg-mpegts.c"
#undef av_interleaved_write_frame
#undef os_sem_post
#undef os_sem_wait
#undef os_event_try
#undef getaddrinfo
#undef freeaddrinfo
#undef clock_gettime

/* ---- state of the hooks ---- */
static volatile int posted, iters_done;
static __thread int in_iter;
static int write_calls, write_fail_at, write_fail_ret;
static double clock_skew;
static volatile int hold_write, blind_count;

static int mts_sem_post(os_sem_t *s)
{
	__atomic_add_fetch(&posted, 1, __ATOMIC_SEQ_CST);
	return os_sem_post(s);
}
static int mts_sem_wait(os_sem_t *s)
{
	if (in_iter) {
		in_iter = 0;
		__atomic_add_fetch(&iters_done, 1, __ATOMIC_SEQ_CST);
	}
	int r = os_sem_wait(s);
	if (r == 0) {
		in_iter = 1;
		while (__atomic_load_n(&hold_write, __ATOMIC_SEQ_CST))
			usleep(1000);
	}
	return r;
}
/* "blind N": the write thread's check of the stop event reports "not signalled" N times (to reach code that a real stop
 * races past), afterwards the real event state is reported again */
static int mts_event_try(os_event_t *e)
{
	if (__atomic_load_n(&blind_count, __ATOMIC_SEQ_CST) > 0) {
		__atomic_sub_fetch(&blind_count, 1, __ATOMIC_SEQ_CST);
		return EAGAIN;
	}
	return os_event_try(e);
}
static int mts_write_hook(AVFormatContext *s, AVPacket *pkt)
{
	int n = ++write_calls;
	if (write_fail_at && n >= write_fail_at)
		return write_fail_ret;
	return av_interleaved_write_frame(s, pkt);
}
static int mw_clock_gettime(clockid_t id, struct timespec *ts)
{
	int r = clock_gettime(id, ts);
	ts->tv_sec += (time_t)clock_skew;
	return r;
}
static int mw_getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
	const char *list[2] = {NULL, NULL};
	int n = 1;
	if (!node || !*node) {
		list[0] = (hints && (hints->ai_flags & AI_PASSIVE)) ? "0.0.0.0" : "127.0.0.1";
	} else if (!strcmp(node, "localhost")) {
		list[0] = "127.0.0.1";
	} else if (!strcmp(node, "multi.test")) {
		list[0] = "192.0.2.1";
		list[1] = "192.0.2.2";
		n = 2;
	} else {
		list[0] = node;
	}
	struct addrinfo *head = NULL, **tail = &head;
	int port = service ? atoi(service) : 0;
	for (int i = 0; i < n; i++) {
		struct sockaddr_in a4 = {0};
		struct sockaddr_in6 a6 = {0};
		struct addrinfo *ai = calloc(1, sizeof(*ai) + sizeof(struct sockaddr_storage));
		if (inet_pton(AF_INET, list[i], &a4.sin_addr) == 1) {
			a4.sin_family = AF_INET;
			a4.sin_port = htons((uint16_t)port);
			ai->ai_family = AF_INET;
			ai->ai_addrlen = sizeof(a4);
			ai->ai_addr = (struct sockaddr *)(ai + 1);
			memcpy(ai->ai_addr, &a4, sizeof(a4));
		} else if (inet_pton(AF_INET6, list[i], &a6.sin6_addr) == 1) {
			a6.sin6_family = AF_INET6;
			a6.sin6_port = htons((uint16_t)port);
			ai->ai_family = AF_INET6;
			ai->ai_addrlen = sizeof(a6);
			ai->ai_addr = (struct sockaddr *)(ai + 1);
			memcpy(ai->ai_addr, &a6, sizeof(a6));
		} else {
			free(ai);
			mw_freeaddrinfo(head);
			return EAI_NONAME;
		}
		ai->ai_socktype = SOCK_DGRAM;
		*tail = ai;
		tail = &ai->ai_next;
	}
	*res = head;
	return 0;
}
static void mw_freeaddrinfo(struct addrinfo *ai)
{
	while (ai) {
		struct addrinfo *nx = ai->ai_next;
		free(ai);
		ai = nx;
	}
}

/* ---- helpers ---- */
extern struct obs_output_info ffmpeg_mpegts_muxer;
static void *ST; /* struct ffmpeg_output * */
static int loglevel = LOG_WARNING;
static int64_t I(const char *s)
{
	return (int64_t)strtoll(s, NULL, 0);
}
static bool B(const char *s)
{
	return s[0] == '1' || s[0] == 't' || !strcmp(s, "on");
}
static const char *code_name(int c)
{
	switch (c) {
	case OBS_OUTPUT_SUCCESS:
		return "SUCCESS";
	case OBS_OUTPUT_BAD_PATH:
		return "BAD_PATH";
	case OBS_OUTPUT_CONNECT_FAILED:
		return "CONNECT_FAILED";
	case OBS_OUTPUT_INVALID_STREAM:
		return "INVALID_STREAM";
	case OBS_OUTPUT_ERROR:
		return "ERROR";
	case OBS_OUTPUT_DISCONNECTED:
		return "DISCONNECTED";
	case OBS_OUTPUT_UNSUPPORTED:
		return "UNSUPPORTED";
	case OBS_OUTPUT_NO_SPACE:
		return "NO_SPACE";
	case OBS_OUTPUT_ENCODE_ERROR:
		return "ENCODE_ERROR";
	case OBS_OUTPUT_TIMEDOUT:
		return "TIMEDOUT";
	}
	static char num[16][24];
	static int ni;
	ni = (ni + 1) & 15;
	snprintf(num[ni], sizeof(num[ni]), "code(%d)", c);
	return num[ni];
}
static void log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096];
	(void)param;
	if (level > loglevel)
		return;
	vsnprintf(msg, sizeof(msg), format, args);
	if (strstr(msg, "version") || strstr(msg, "Output format name"))
		return; /* FFmpeg / libsrt / librist version dependent */
	for (char *p = msg; *p; p++)
		if (*p == '\n' || *p == '\t')
			*p = ' ';
	if (net_port()) { /* the port of the loopback test socket is random: mask it */
		char portbuf[16], out[4096];
		snprintf(portbuf, sizeof(portbuf), ":%d", net_port());
		char *q = strstr(msg, portbuf);
		if (q) {
			*q = 0;
			snprintf(out, sizeof(out), "%s:@PORT@%s", msg, q + strlen(portbuf));
			snprintf(msg, sizeof(msg), "%s", out);
		}
	}
	char *gai = strstr(msg, "Failed to resolve hostname");
	if (gai && (gai = strchr(gai, ':')))
		*gai = 0; /* the libc resolver error text (gai_strerror) is platform dependent: dropped */
	printf("log %s %s\n", level == LOG_ERROR ? "error" : level == LOG_WARNING ? "warning" : level == LOG_INFO ? "info" : "debug",
	       msg);
}
static void sleep_ms(int ms)
{
	usleep((useconds_t)ms * 1000);
}
static struct ffmpeg_output *S(void)
{
	return (struct ffmpeg_output *)ST;
}

static void full_reset(void)
{
	mw_world_reset();
	net_reset();
	write_calls = write_fail_at = 0;
	posted = iters_done = 0;
	clock_skew = 0;
}

static enum video_format vfmt(const char *s)
{
	static const struct {
		const char *n;
		enum video_format v;
	} t[] = {{"none", VIDEO_FORMAT_NONE}, {"i420", VIDEO_FORMAT_I420}, {"nv12", VIDEO_FORMAT_NV12},
		 {"yuy2", VIDEO_FORMAT_YUY2}, {"uyvy", VIDEO_FORMAT_UYVY}, {"rgba", VIDEO_FORMAT_RGBA},
		 {"bgra", VIDEO_FORMAT_BGRA}, {"bgrx", VIDEO_FORMAT_BGRX}, {"y800", VIDEO_FORMAT_Y800},
		 {"i444", VIDEO_FORMAT_I444}, {"bgr3", VIDEO_FORMAT_BGR3}, {"i422", VIDEO_FORMAT_I422},
		 {"i010", VIDEO_FORMAT_I010}, {"p010", VIDEO_FORMAT_P010}, {"i210", VIDEO_FORMAT_I210},
		 {"i412", VIDEO_FORMAT_I412}, {"p216", VIDEO_FORMAT_P216}, {"p416", VIDEO_FORMAT_P416}};
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcmp(t[i].n, s))
			return t[i].v;
	return (enum video_format)I(s);
}

static void cmd_world(int argc, char **argv)
{
	const char *k = argv[1], *v = argc > 2 ? argv[2] : "";
	if (!strcmp(k, "fps")) {
		W.fps_num = (uint32_t)I(v);
		W.fps_den = argc > 3 ? (uint32_t)I(argv[3]) : 1;
	} else if (!strcmp(k, "size")) {
		W.width = (uint32_t)I(v);
		W.height = (uint32_t)I(argv[3]);
	} else if (!strcmp(k, "vformat")) {
		W.vformat = vfmt(v);
	} else if (!strcmp(k, "colorspace")) {
		W.colorspace = !strcmp(v, "601")    ? VIDEO_CS_601
			       : !strcmp(v, "709")  ? VIDEO_CS_709
			       : !strcmp(v, "srgb") ? VIDEO_CS_SRGB
			       : !strcmp(v, "pq")   ? VIDEO_CS_2100_PQ
			       : !strcmp(v, "hlg")  ? VIDEO_CS_2100_HLG
			       : !strcmp(v, "default") ? VIDEO_CS_DEFAULT
						    : (int)I(v);
	} else if (!strcmp(k, "range")) {
		W.range = !strcmp(v, "full") ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
	} else if (!strcmp(k, "vcodec")) {
		snprintf(W.vcodec, sizeof(W.vcodec), "%s", v);
	} else if (!strcmp(k, "acodec")) {
		snprintf(W.acodec, sizeof(W.acodec), "%s", v);
	} else if (!strcmp(k, "tracks")) {
		W.ntracks = (int)I(v);
	} else if (!strcmp(k, "abitrate")) {
		W.abitrate[I(v)] = (int)I(argv[3]);
	} else if (!strcmp(k, "vbitrate")) {
		W.vbitrate = (int)I(v);
	} else if (!strcmp(k, "keyint")) {
		W.keyint_sec = (int)I(v);
	} else if (!strcmp(k, "framesize")) {
		W.frame_size = (uint32_t)I(v);
	} else if (!strcmp(k, "samplerate")) {
		W.sample_rate = (uint32_t)I(v);
	} else if (!strcmp(k, "speakers")) {
		W.speakers = !strcmp(v, "mono")     ? SPEAKERS_MONO
			     : !strcmp(v, "stereo") ? SPEAKERS_STEREO
			     : !strcmp(v, "4.1")    ? SPEAKERS_4POINT1
			     : !strcmp(v, "5.1")    ? SPEAKERS_5POINT1
						    : (int)I(v);
	} else if (!strcmp(k, "vinfo")) {
		W.have_vinfo = B(v);
	} else if (!strcmp(k, "ainfo")) {
		W.have_ainfo = B(v);
	} else if (!strcmp(k, "vextra")) {
		W.vextra = B(v);
	} else if (!strcmp(k, "aextra")) {
		W.aextra[I(v)] = B(argv[3]);
	} else if (!strcmp(k, "service")) {
		W.have_service = B(v);
	} else if (!strcmp(k, "url")) {
		/* "@PORT@" = port of the loopback socket opened by sock_listen */
		char buf[512];
		const char *at = strstr(v, "@PORT@");
		if (at)
			snprintf(buf, sizeof(buf), "%.*s%d%s", (int)(at - v), v, net_port(), at + 6);
		else
			snprintf(buf, sizeof(buf), "%s", v);
		mw_set_str(&W.url, buf);
	} else if (!strcmp(k, "user")) {
		mw_set_str(&W.user, v);
	} else if (!strcmp(k, "pass")) {
		mw_set_str(&W.pass, v);
	} else if (!strcmp(k, "streamid")) {
		mw_set_str(&W.streamid, v);
	} else if (!strcmp(k, "passphrase")) {
		mw_set_str(&W.passphrase, v);
	} else if (!strcmp(k, "can_begin")) {
		W.can_begin = B(v);
	} else if (!strcmp(k, "init_ok")) {
		W.init_ok = B(v);
	} else if (!strcmp(k, "pause_offset")) {
		W.pause_offset = (uint64_t)I(v);
	} else if (!strcmp(k, "hdr_peak")) {
		W.hdr_peak = (float)atof(v);
	} else {
		printf("world: unknown key %s\n", k);
	}
}

static void wait_thread(void)
{
	struct ffmpeg_output *s = S();
	if (!s)
		return;
	bool active = os_atomic_load_bool(&s->start_stop_thread_active);
	pthread_t h = s->start_stop_thread;
	for (int i = 0; i < 5000 && os_atomic_load_bool(&s->start_stop_thread_active); i++)
		sleep_ms(1);
	/* the reference only joins this thread while the flag is still set; join it here so that nothing leaks */
	if (active)
		pthread_join(h, NULL);
}

static void print_events(void)
{
	printf("events");
	for (int i = 0; i < W.nevt && i < MW_MAXEVT; i++)
		printf(" %s", code_name(W.evt[i]));
	if (!W.nevt)
		printf(" -");
	printf("\n");
	W.nevt = 0;
	if (W.last_error[0]) {
		printf("last_error \"%s\"\n", W.last_error);
		W.last_error[0] = 0;
	}
}

static void print_state(void)
{
	struct ffmpeg_output *s = S();
	if (!s) {
		printf("state no-stream\n");
		return;
	}
	printf("state running=%d stopping=%d write_thread_active=%d has_connected=%d got_headers=%d queued=%zu\n",
	       (int)os_atomic_load_bool(&s->running), (int)os_atomic_load_bool(&s->stopping), (int)s->write_thread_active,
	       (int)s->has_connected, (int)s->got_headers, s->packets.num);
	printf("state total_bytes=%" PRIu64 " initialized=%d begin_calls=%d stop_ts=%" PRIu64 "\n", ffmpeg_mpegts_total_bytes(s),
	       (int)s->ff_data.initialized, W.begin_calls, s->stop_ts);
}

static void drain(void)
{
	for (int i = 0; i < 4000; i++) {
		if (__atomic_load_n(&iters_done, __ATOMIC_SEQ_CST) >= __atomic_load_n(&posted, __ATOMIC_SEQ_CST))
			return;
		sleep_ms(1);
	}
	printf("drain timeout\n");
}

static void print_cfg(const char *what)
{
	struct ffmpeg_cfg c;
	int code = OBS_OUTPUT_ERROR;
	bool all = !strcmp(what, "all");
	memset(&c, 0, sizeof(c));
	bool ok = fetch_service_info(S(), &c, &code);
	if (all || !strcmp(what, "service")) {
		printf("cfg service=%d", ok);
		if (!ok) {
			printf(" code=%s\n", code_name(code));
			return;
		}
#define Q(x) (c.x ? c.x : "(null)")
		printf(" url=%s username=%s password=%s stream_id=%s passphrase=%s\n", Q(url), Q(username), Q(password),
		       Q(stream_id), Q(encrypt_passphrase));
		printf("cfg format=%s mime=%s is_srt=%d is_rist=%d srt_pkt_size=%d\n", Q(format_name), Q(format_mime_type),
		       (int)c.is_srt, (int)c.is_rist, c.srt_pkt_size);
	}
#undef Q
#define Q(x) (c.x ? c.x : "(null)")
	if (all || !strcmp(what, "video")) {
		ok = setup_video_settings(S(), &c, &code);
		printf("cfg video=%d", ok);
		if (!ok) {
			printf(" code=%s\n", code_name(code));
		} else {
			printf(" size=%dx%d venc=%s id=%d pix_fmt=%s range=%d pri=%d trc=%d cs=%d bitrate=%d gop=%d\n", c.width,
			       c.height, Q(video_encoder), c.video_encoder_id, av_get_pix_fmt_name(c.format),
			       (int)c.color_range, (int)c.color_primaries, (int)c.color_trc, (int)c.colorspace,
			       c.video_bitrate, c.gop_size);
		}
	}
	if ((all || !strcmp(what, "audio")) && W.ntracks > 0) {
		setup_audio_settings(S(), &c);
		printf("cfg audio tracks=%d aenc=%s id=%d frame_size=%d bitrates=", c.audio_mix_count, Q(audio_encoder),
		       c.audio_encoder_id, c.frame_size);
		for (int i = 0; i < c.audio_mix_count; i++)
			printf("%s%d", i ? "," : "", c.audio_bitrates[i]);
		printf("\n");
	}
	if (all || !strcmp(what, "muxer")) {
		setup_muxer_settings(S(), &c);
		printf("cfg muxer_settings=\"%s\" protocol_settings=\"%s\"\n", Q(muxer_settings), Q(protocol_settings));
	}
#undef Q
}

/* make a payload: video = Annex B access unit, audio = opaque bytes */
static uint8_t *make_payload(bool video, bool key, size_t size, unsigned seed)
{
	uint8_t *d = malloc(size < 8 ? 8 : size);
	unsigned x = seed * 2654435761u + 12345u;
	for (size_t i = 0; i < size; i++) {
		x = x * 1103515245u + 12345u;
		d[i] = (uint8_t)(0x10 + ((x >> 16) % 0xe0)); /* never 0x00/0x01: no accidental start codes */
	}
	if (video && size >= 5) {
		d[0] = d[1] = d[2] = 0;
		d[3] = 1;
		d[4] = key ? 0x65 : 0x41;
	}
	return d;
}

static void cmd_pkt(int argc, char **argv)
{
	/* pkt v|a TRACK PTS DTS KEY SIZE [SYS_DTS_USEC] */
	static unsigned seq;
	struct encoder_packet p;
	memset(&p, 0, sizeof(p));
	bool video = argv[1][0] == 'v';
	p.type = video ? OBS_ENCODER_VIDEO : OBS_ENCODER_AUDIO;
	p.track_idx = (size_t)I(argv[2]);
	p.pts = I(argv[3]);
	p.dts = I(argv[4]);
	p.keyframe = B(argv[5]);
	p.size = (size_t)I(argv[6]);
	p.sys_dts_usec = argc > 7 ? I(argv[7]) : 0;
	p.data = make_payload(video, p.keyframe, p.size, ++seq);
	ffmpeg_mpegts_muxer.encoded_packet(ST, &p);
	free(p.data);
	wait_thread(); /* a failing packet starts an asynchronous stop */
}

/* direct SRT / RIST URL layer (no AVIO, no muxer) */
static URLContext *UC;
static void uc_free(void)
{
	if (UC) {
		av_freep(&UC->priv_data);
		av_freep(&UC);
	}
}
static URLContext *uc_new(bool rist, const char *url)
{
	uc_free();
	UC = av_mallocz(sizeof(URLContext) + strlen(url) + 1);
	UC->url = (char *)url;
	UC->max_packet_size = rist ? RIST_MAX_PAYLOAD_SIZE : SRT_LIVE_DEFAULT_PAYLOAD_SIZE;
	UC->priv_data = rist ? av_mallocz(sizeof(RISTContext)) : av_mallocz(sizeof(SRTContext));
	return UC;
}
static char urlbuf[2048];
static void cmd_srt_open(int argc, char **argv)
{
	/* srt_open URL [STREAMID|- [PASSPHRASE|-]] */
	snprintf(urlbuf, sizeof(urlbuf), "%s", argv[1]);
	URLContext *h = uc_new(false, urlbuf);
	SRTContext *c = h->priv_data;
	if (argc > 2 && strcmp(argv[2], "-"))
		c->streamid = av_strdup(argv[2]);
	if (argc > 3 && strcmp(argv[3], "-"))
		c->passphrase = av_strdup(argv[3]);
	int r = libsrt_open(h, urlbuf);
	printf("srt_open ret=%s(%d) max_packet_size=%d rw_timeout=%" PRId64 " mode=%d transtype=%d\n", code_name(r), r,
	       h->max_packet_size, h->rw_timeout, (int)c->mode, (int)c->transtype);
	printf("srt_open last_error obs=%s srt=%d listen_timeout=%" PRId64 " payload_size=%d\n",
	       code_name(c->last_error.obs_output_error_number), c->last_error.srt_error_code, c->listen_timeout,
	       c->payload_size);
}
static void cmd_rist_open(int argc, char **argv)
{
	/* rist_open URL [SECRET|- [USER|- [PASS|-]]] */
	snprintf(urlbuf, sizeof(urlbuf), "%s", argv[1]);
	URLContext *h = uc_new(true, urlbuf);
	RISTContext *c = h->priv_data;
	if (argc > 2 && strcmp(argv[2], "-"))
		c->secret = bstrdup(argv[2]);
	if (argc > 3 && strcmp(argv[3], "-"))
		c->username = bstrdup(argv[3]);
	if (argc > 4 && strcmp(argv[4], "-"))
		c->password = bstrdup(argv[4]);
	int r = librist_open(h, urlbuf);
	printf("rist_open ret=%s(%d) max_packet_size=%d buffer_size=%d profile=%d packet_size=%d fifo_shift=%d statsinterval=%d\n",
	       code_name(r), r, h->max_packet_size, c->buffer_size, c->profile, c->packet_size, c->fifo_shift,
	       c->statsinterval);
}

static void run_cmd(struct script_line *sl)
{
	char **a = sl->argv;
	int n = sl->argc;
	const char *c = a[0];
	if (!strcmp(c, "reset")) {
		full_reset();
	} else if (!strcmp(c, "world")) {
		cmd_world(n, a);
	} else if (!strcmp(c, "setting")) {
		obs_data_set_string(mw_settings(), a[1], strcmp(a[2], "-") ? a[2] : "");
	} else if (!strcmp(c, "log")) {
		loglevel = !strcmp(a[1], "error")  ? LOG_ERROR
			   : !strcmp(a[1], "warning") ? LOG_WARNING
			   : !strcmp(a[1], "info")   ? LOG_INFO
						     : LOG_DEBUG;
	} else if (!strcmp(c, "info")) {
		printf("info id=%s flags=0x%x protocols=%s video_codecs=%s audio_codecs=%s name=%s\n", ffmpeg_mpegts_muxer.id,
		       ffmpeg_mpegts_muxer.flags, ffmpeg_mpegts_muxer.protocols, ffmpeg_mpegts_muxer.encoded_video_codecs,
		       ffmpeg_mpegts_muxer.encoded_audio_codecs, ffmpeg_mpegts_muxer.get_name(NULL));
		printf("info callbacks create=%d destroy=%d start=%d stop=%d encoded_packet=%d total_bytes=%d properties=%d\n",
		       !!ffmpeg_mpegts_muxer.create, !!ffmpeg_mpegts_muxer.destroy, !!ffmpeg_mpegts_muxer.start,
		       !!ffmpeg_mpegts_muxer.stop, !!ffmpeg_mpegts_muxer.encoded_packet, !!ffmpeg_mpegts_muxer.get_total_bytes,
		       !!ffmpeg_mpegts_muxer.get_properties);
	} else if (!strcmp(c, "props")) {
		obs_properties_t *p = ffmpeg_mpegts_muxer.get_properties(NULL);
		for (obs_property_t *pr = obs_properties_first(p); pr; obs_property_next(&pr))
			printf("prop name=%s desc=%s type=%d text_type=%d\n", obs_property_name(pr), obs_property_description(pr),
			       (int)obs_property_get_type(pr), (int)obs_property_text_type(pr));
		obs_properties_destroy(p);
	} else if (!strcmp(c, "create")) {
		if (!mw_output)
			mw_output_create();
		ST = ffmpeg_mpegts_muxer.create(mw_settings(), mw_output);
		printf("create %s\n", ST ? "ok" : "null");
	} else if (!strcmp(c, "start")) {
		bool r = ffmpeg_mpegts_muxer.start(ST);
		printf("start returned %d\n", r);
		wait_thread();
		posted = iters_done = 0; /* the write thread of the previous run is gone */
	} else if (!strcmp(c, "stop")) {
		ffmpeg_mpegts_muxer.stop(ST, (uint64_t)I(a[1]));
		wait_thread();
	} else if (!strcmp(c, "stop_async")) {
		ffmpeg_mpegts_muxer.stop(ST, (uint64_t)I(a[1]));
	} else if (!strcmp(c, "hold")) {
		hold_write = B(a[1]);
	} else if (!strcmp(c, "blind")) {
		blind_count = (int)I(a[1]);
	} else if (!strcmp(c, "wait")) {
		wait_thread();
	} else if (!strcmp(c, "destroy")) {
		ffmpeg_mpegts_muxer.destroy(ST);
		ST = NULL;
		printf("destroyed\n");
	} else if (!strcmp(c, "pkt")) {
		cmd_pkt(n, a);
	} else if (!strcmp(c, "pktnull")) {
		ffmpeg_mpegts_muxer.encoded_packet(ST, NULL);
		wait_thread();
	} else if (!strcmp(c, "drain")) {
		drain();
	} else if (!strcmp(c, "wait_event")) {
		/* wait_event N: until N stop signals have been recorded since the last "events" (5 s limit) */
		for (int i = 0; i < 5000 && W.nevt < (int)I(a[1]); i++)
			sleep_ms(1);
		if (W.nevt < (int)I(a[1]))
			printf("wait_event timeout\n");
	} else if (!strcmp(c, "settle")) {
		sleep_ms(50);
	} else if (!strcmp(c, "state")) {
		print_state();
	} else if (!strcmp(c, "events")) {
		print_events();
	} else if (!strcmp(c, "cfg")) {
		print_cfg(n > 1 ? a[1] : "service");
	} else if (!strcmp(c, "ts")) {
		net_ts_report();
	} else if (!strcmp(c, "sends")) {
		printf("sends any=%d max_chunk=%zu\n", net_sends() > 0, net_max_chunk());
	} else if (!strcmp(c, "write_fail")) {
		write_fail_at = (int)I(a[1]);
		write_fail_ret = n > 2 ? (int)I(a[2]) : AVERROR(EIO);
		if (n > 2 && !strcmp(a[2], "INVALIDDATA"))
			write_fail_ret = AVERROR_INVALIDDATA;
		if (n > 2 && !strcmp(a[2], "EINVAL"))
			write_fail_ret = -EINVAL;
		write_calls = 0;
	} else if (!strcmp(c, "clock_skew")) {
		clock_skew = atof(a[1]);
	} else if (!strcmp(c, "srt_open")) {
		cmd_srt_open(n, a);
	} else if (!strcmp(c, "srt_close")) {
		printf("srt_close ret=%d\n", UC ? libsrt_close(UC) : -99);
	} else if (!strcmp(c, "srt_write")) {
		uint8_t buf[2048] = {0};
		printf("srt_write ret=%d\n", UC ? libsrt_write(UC, buf, (int)I(a[1])) : -99);
	} else if (!strcmp(c, "rist_open")) {
		cmd_rist_open(n, a);
	} else if (!strcmp(c, "rist_close")) {
		printf("rist_close ret=%d\n", UC ? librist_close(UC) : -99);
	} else if (!strcmp(c, "rist_write")) {
		uint8_t buf[2048] = {0};
		printf("rist_write ret=%d\n", UC ? librist_write(UC, buf, (int)I(a[1])) : -99);
	} else if (!strcmp(c, "risterr")) {
		printf("risterr %d -> %s\n", (int)I(a[1]), risterr2ret((int)I(a[1])) == AVERROR(ENOMEM) ? "ENOMEM" : "EXTERNAL");
	} else if (!strcmp(c, "crash_start")) {
		/* run create+start in a child so that a reference crash is reported instead of killing the runner */
		fflush(stdout);
		pid_t pid = fork();
		if (pid == 0) {
			ST = ffmpeg_mpegts_muxer.create(mw_settings(), mw_output);
			ffmpeg_mpegts_muxer.start(ST);
			wait_thread();
			_exit(0);
		}
		int st = 0;
		waitpid(pid, &st, 0);
		if (WIFSIGNALED(st))
			printf("crash signal=%d\n", WTERMSIG(st));
		else
			printf("no crash exit=%d\n", WEXITSTATUS(st));
	} else if (!strcmp(c, "srt_reset_net")) {
		net_reset();
	} else if (strncmp(c, "srt_", 4) == 0 || strncmp(c, "rist_", 5) == 0 || strncmp(c, "sock_", 5) == 0 || !strcmp(c, "capture_clear") || !strcmp(c, "capture")) {
		net_command(n, a);
	} else {
		printf("unknown command %s\n", c);
	}
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	base_set_log_handler(log_handler, NULL);
	/* ffmpeg_mpegts_create() does this; the direct srt_/rist_ commands run without a create */
	av_log_set_callback(ffmpeg_mpegts_log_callback);
	full_reset();
	mw_output_create();
	while (script_next(f, &sl, &lineno)) {
		printf("> ");
		for (int i = 0; i < sl.argc; i++)
			printf("%s%s", i ? " " : "", sl.argv[i]);
		printf("\n");
		run_cmd(&sl);
	}
	fclose(f);
	return 0;
}
