/*
 * Conformance runner for area rtmpstream (spec S-045).
 *
 * The real plugins/obs-outputs/rtmp-stream.c and net-if.c are #included into this translation
 * unit so that their static functions (drop logic, dynamic bitrate, error mapping, connect
 * sequence, send loop) can be driven directly. Everything the plugin calls outward is replaced by
 * the fakes below (libobs object model, vendored librtmp, FLV muxer, kernel interface list,
 * threads, clock). Threads are NOT used: pthread_create/join/detach are fakes, the "send thread"
 * body is run synchronously by the script command "run_send". This makes every run deterministic.
 *
 * Test tooling, not part of libobs. See test/conformance/README.md (section rtmpstream).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sys/ioctl.h>
#include <ifaddrs.h>
#include <signal.h>
#include <unistd.h>
#include <sys/wait.h>

/* ---- renamed system calls (must precede every include that declares them) ---- */
#define pthread_create fake_pthread_create
#define pthread_join fake_pthread_join
#define pthread_detach fake_pthread_detach
#define getifaddrs fake_getifaddrs
#define freeifaddrs fake_freeifaddrs
#define ioctl fake_ioctl

int fake_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
int fake_pthread_join(pthread_t t, void **r);
int fake_pthread_detach(pthread_t t);
int fake_getifaddrs(struct ifaddrs **out);
void fake_freeifaddrs(struct ifaddrs *a);
int fake_ioctl(int fd, unsigned long req, ...);

#include "harness.h"
#include "../../../plugins/obs-outputs/net-if.c"
#include "../../../plugins/obs-outputs/rtmp-stream.c"
/* ------------------------------------------------------------------------------------------- */
/* fake state                                                                                  */
/* ------------------------------------------------------------------------------------------- */

#define MAXKV 24
struct kv {
	char key[48];
	int kind; /* 0 int, 1 bool, 2 string */
	long long i;
	char s[4096];
	bool user;
	bool used;
};
struct fake_data {
	struct kv kv[MAXKV];
};

static struct fake_data out_settings;
static struct fake_data enc_settings_v[MAX_OUTPUT_VIDEO_ENCODERS];
static struct fake_data enc_settings_a[MAX_OUTPUT_AUDIO_ENCODERS];

struct fake_enc {
	bool present;
	char codec[16];
	char name[24];
	uint32_t caps;
	bool no_video; /* obs_encoder_video returns NULL */
	bool no_extra; /* obs_encoder_get_extra_data fails */
};
static struct fake_enc venc[MAX_OUTPUT_VIDEO_ENCODERS];
static struct fake_enc aenc[MAX_OUTPUT_AUDIO_ENCODERS];

static struct {
	bool have_service;
	bool can_begin;
	bool init_enc_ok;
	int64_t delay;
	char url[256], key[256], user[64], pass[64];
	bool url_null, key_null, user_null, pass_null;
	uint64_t now;
	int pthread_create_fail; /* N: fail the Nth pthread_create call since it was set (0 = never) */
	int pthread_create_count;
	/* librtmp */
	bool setup_ok, connect_ok, connectstream_ok;
	int connect_err;
	int write_fail_at; /* 1-based index of the RTMP_Write call that fails (0 = never) */
	int write_calls;
	int write_len; /* bytes the fake FLV muxer reports for a packet */
	bool readpacket_ok;
	int pending; /* what FIONREAD reports */
	int ioctl_fbio_fail;
	int peak;
} F;

static int released;
static int tls_free_calls;

static struct rtmp_stream *S;
static char output_obj, service_obj, video_obj;
static struct video_output_info vinfo;

/* ------------------------------------------------------------------------------------------- */
/* fake data store                                                                             */
/* ------------------------------------------------------------------------------------------- */

static struct kv *kv_find(struct fake_data *d, const char *k, bool create)
{
	for (int i = 0; i < MAXKV; i++)
		if (d->kv[i].used && !strcmp(d->kv[i].key, k))
			return &d->kv[i];
	if (!create)
		return NULL;
	for (int i = 0; i < MAXKV; i++)
		if (!d->kv[i].used) {
			d->kv[i].used = true;
			snprintf(d->kv[i].key, sizeof(d->kv[i].key), "%s", k);
			return &d->kv[i];
		}
	fprintf(stderr, "kv full\n");
	exit(2);
}

long long obs_data_get_int(obs_data_t *data, const char *name)
{
	struct kv *e = kv_find((struct fake_data *)data, name, false);
	return e ? e->i : 0;
}
bool obs_data_get_bool(obs_data_t *data, const char *name)
{
	struct kv *e = kv_find((struct fake_data *)data, name, false);
	return e ? e->i != 0 : false;
}
const char *obs_data_get_string(obs_data_t *data, const char *name)
{
	struct kv *e = kv_find((struct fake_data *)data, name, false);
	return e && e->kind == 2 ? e->s : "";
}
bool obs_data_has_user_value(obs_data_t *data, const char *name)
{
	struct kv *e = kv_find((struct fake_data *)data, name, false);
	return e && e->user;
}
void obs_data_set_int(obs_data_t *data, const char *name, long long v)
{
	struct kv *e = kv_find((struct fake_data *)data, name, true);
	e->kind = 0, e->i = v, e->user = true;
}
void obs_data_set_default_int(obs_data_t *data, const char *name, long long v)
{
	struct kv *e = kv_find((struct fake_data *)data, name, true);
	if (!e->user)
		e->kind = 0, e->i = v;
}
void obs_data_set_default_bool(obs_data_t *data, const char *name, bool v)
{
	struct kv *e = kv_find((struct fake_data *)data, name, true);
	if (!e->user)
		e->kind = 1, e->i = v;
}
void obs_data_set_default_string(obs_data_t *data, const char *name, const char *v)
{
	struct kv *e = kv_find((struct fake_data *)data, name, true);
	if (!e->user)
		e->kind = 2, snprintf(e->s, sizeof(e->s), "%s", v);
}
void obs_data_release(obs_data_t *data)
{
	(void)data;
}

/* ------------------------------------------------------------------------------------------- */
/* fake libobs                                                                                 */
/* ------------------------------------------------------------------------------------------- */

const char *obs_module_text(const char *k)
{
	static char buf[4][128];
	static int n;
	n = (n + 1) & 3;
	snprintf(buf[n], sizeof(buf[n]), "T:%s", k);
	return buf[n];
}
const char *obs_output_get_name(const obs_output_t *o)
{
	(void)o;
	return "out";
}
bool obs_output_can_begin_data_capture(const obs_output_t *o, uint32_t flags)
{
	(void)o, (void)flags;
	return F.can_begin;
}
bool obs_output_initialize_encoders(obs_output_t *o, uint32_t flags)
{
	(void)o, (void)flags;
	return F.init_enc_ok;
}
bool obs_output_begin_data_capture(obs_output_t *o, uint32_t flags)
{
	(void)o, (void)flags;
	printf("fake begin_data_capture\n");
	return true;
}
void obs_output_end_data_capture(obs_output_t *o)
{
	(void)o;
	printf("fake end_data_capture\n");
}
void obs_output_signal_stop(obs_output_t *o, int code)
{
	(void)o;
	printf("fake signal_stop %d\n", code);
}
void obs_output_set_last_error(obs_output_t *o, const char *msg)
{
	(void)o;
	printf("fake set_last_error \"%s\"\n", msg);
}
obs_service_t *obs_output_get_service(const obs_output_t *o)
{
	(void)o;
	return F.have_service ? (obs_service_t *)&service_obj : NULL;
}
obs_data_t *obs_output_get_settings(const obs_output_t *o)
{
	(void)o;
	return (obs_data_t *)&out_settings;
}
uint32_t obs_output_get_delay(const obs_output_t *o)
{
	(void)o;
	return (uint32_t)F.delay;
}
const char *obs_service_get_connect_info(const obs_service_t *s, uint32_t type)
{
	(void)s;
	switch (type) {
	case OBS_SERVICE_CONNECT_INFO_SERVER_URL:
		return F.url_null ? NULL : F.url;
	case OBS_SERVICE_CONNECT_INFO_STREAM_KEY:
		return F.key_null ? NULL : F.key;
	case OBS_SERVICE_CONNECT_INFO_USERNAME:
		return F.user_null ? NULL : F.user;
	case OBS_SERVICE_CONNECT_INFO_PASSWORD:
		return F.pass_null ? NULL : F.pass;
	}
	return NULL;
}
obs_encoder_t *obs_output_get_video_encoder2(const obs_output_t *o, size_t idx)
{
	(void)o;
	return idx < MAX_OUTPUT_VIDEO_ENCODERS && venc[idx].present ? (obs_encoder_t *)&venc[idx] : NULL;
}
obs_encoder_t *obs_output_get_audio_encoder(const obs_output_t *o, size_t idx)
{
	(void)o;
	return idx < MAX_OUTPUT_AUDIO_ENCODERS && aenc[idx].present ? (obs_encoder_t *)&aenc[idx] : NULL;
}
static bool is_v(const obs_encoder_t *e)
{
	return (const char *)e >= (const char *)venc && (const char *)e < (const char *)(venc + MAX_OUTPUT_VIDEO_ENCODERS);
}
const char *obs_encoder_get_codec(const obs_encoder_t *e)
{
	return ((const struct fake_enc *)e)->codec;
}
const char *obs_encoder_get_name(const obs_encoder_t *e)
{
	return ((const struct fake_enc *)e)->name;
}
uint32_t obs_encoder_get_caps(const obs_encoder_t *e)
{
	return ((const struct fake_enc *)e)->caps;
}
obs_data_t *obs_encoder_get_settings(const obs_encoder_t *e)
{
	if (is_v(e))
		return (obs_data_t *)&enc_settings_v[(const struct fake_enc *)e - venc];
	return (obs_data_t *)&enc_settings_a[(const struct fake_enc *)e - aenc];
}
void obs_encoder_update(obs_encoder_t *e, obs_data_t *s)
{
	(void)s;
	printf("fake encoder_update track=%d bitrate=%lld\n", (int)((struct fake_enc *)e - venc),
	       obs_data_get_int(obs_encoder_get_settings(e), "bitrate"));
}
bool obs_encoder_get_extra_data(const obs_encoder_t *e, uint8_t **data, size_t *size)
{
	static uint8_t extra[4] = {1, 2, 3, 4};
	if (((const struct fake_enc *)e)->no_extra)
		return false;
	*data = extra;
	*size = sizeof(extra);
	return true;
}
video_t *obs_encoder_video(const obs_encoder_t *e)
{
	return ((const struct fake_enc *)e)->no_video ? NULL : (video_t *)&video_obj;
}
const struct video_output_info *video_output_get_info(const video_t *v)
{
	(void)v;
	return &vinfo;
}
video_t *obs_get_video(void)
{
	return (video_t *)&video_obj;
}
float obs_get_video_hdr_nominal_peak_level(void)
{
	return (float)F.peak;
}
void obs_encoder_packet_ref(struct encoder_packet *dst, struct encoder_packet *src)
{
	*dst = *src;
}
void obs_encoder_packet_release(struct encoder_packet *p)
{
	(void)p;
	released++;
}

/* codec-specific packet parsers: identity (their behaviour belongs to S-012/S-021) */
void obs_parse_avc_packet(struct encoder_packet *d, const struct encoder_packet *s)
{
	*d = *s;
}
void obs_parse_av1_packet(struct encoder_packet *d, const struct encoder_packet *s)
{
	*d = *s;
}
void obs_parse_hevc_packet(struct encoder_packet *d, const struct encoder_packet *s)
{
	*d = *s;
}
static size_t fake_hdr(uint8_t **h, const uint8_t *data, size_t size)
{
	*h = bmemdup(data, size);
	return size;
}
size_t obs_parse_avc_header(uint8_t **h, const uint8_t *d, size_t s)
{
	return fake_hdr(h, d, s);
}
size_t obs_parse_av1_header(uint8_t **h, const uint8_t *d, size_t s)
{
	return fake_hdr(h, d, s);
}
size_t obs_parse_hevc_header(uint8_t **h, const uint8_t *d, size_t s)
{
	return fake_hdr(h, d, s);
}

/* ---- fake FLV muxer: emits a buffer holding a tag name; declared size is F.write_len ---- */
static void flv_fake_out(const char *what, uint8_t **o, size_t *size)
{
	uint8_t *b = bmalloc(8);
	memset(b, 0, 8);
	snprintf((char *)b, 8, "%s", what);
	*o = b;
	*size = (size_t)F.write_len;
}
void flv_meta_data(obs_output_t *c, uint8_t **o, size_t *s, bool w)
{
	(void)c, (void)w;
	flv_fake_out("meta", o, s);
}
void flv_packet_mux(struct encoder_packet *p, int32_t off, uint8_t **o, size_t *s, bool h)
{
	(void)p;
	printf("fake flv_packet_mux dts_offset=%d header=%d\n", off, h);
	flv_fake_out("mux", o, s);
}
void flv_packet_start(struct encoder_packet *p, enum video_id_t c, uint8_t **o, size_t *s, size_t i)
{
	(void)p;
	printf("fake flv_packet_start codec=%d idx=%zu\n", c, i);
	flv_fake_out("vstart", o, s);
}
void flv_packet_frames(struct encoder_packet *p, enum video_id_t c, int32_t off, uint8_t **o, size_t *s, size_t i)
{
	(void)p;
	printf("fake flv_packet_frames codec=%d dts_offset=%d idx=%zu\n", c, off, i);
	flv_fake_out("vframe", o, s);
}
void flv_packet_end(struct encoder_packet *p, enum video_id_t c, uint8_t **o, size_t *s, size_t i)
{
	(void)p;
	printf("fake flv_packet_end codec=%d idx=%zu\n", c, i);
	flv_fake_out("vend", o, s);
}
void flv_packet_metadata(enum video_id_t c, uint8_t **o, size_t *s, int bits, uint8_t pri, int trc, int spc, int minl,
			 int maxl, size_t i)
{
	printf("fake flv_packet_metadata codec=%d bits=%d pri=%d trc=%d spc=%d minlum=%d maxlum=%d idx=%zu\n", c, bits,
	       pri, trc, spc, minl, maxl, i);
	flv_fake_out("vmeta", o, s);
}
void flv_packet_audio_start(struct encoder_packet *p, enum audio_id_t c, uint8_t **o, size_t *s, size_t i)
{
	(void)p;
	printf("fake flv_packet_audio_start codec=%d idx=%zu\n", c, i);
	flv_fake_out("astart", o, s);
}
void flv_packet_audio_frames(struct encoder_packet *p, enum audio_id_t c, int32_t off, uint8_t **o, size_t *s,
			     size_t i)
{
	(void)p;
	printf("fake flv_packet_audio_frames codec=%d dts_offset=%d idx=%zu\n", c, off, i);
	flv_fake_out("aframe", o, s);
}

/* ---- clock, threads ---- */
uint64_t os_gettime_ns(void)
{
	return F.now;
}
int fake_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
	(void)a, (void)fn, (void)arg;
	F.pthread_create_count++;
	*t = pthread_self();
	if (F.pthread_create_fail && F.pthread_create_count == F.pthread_create_fail) {
		printf("fake pthread_create -> fail\n");
		return EAGAIN;
	}
	printf("fake pthread_create #%d\n", F.pthread_create_count);
	return 0;
}
int fake_pthread_join(pthread_t t, void **r)
{
	(void)t, (void)r;
	printf("fake pthread_join\n");
	return 0;
}
int fake_pthread_detach(pthread_t t)
{
	(void)t;
	printf("fake pthread_detach\n");
	return 0;
}

/* ---- fake librtmp ---- */
int RTMP_Write(RTMP *r, const char *buf, int size, int queue)
{
	(void)r, (void)queue;
	F.write_calls++;
	bool fail = F.write_fail_at && F.write_calls == F.write_fail_at;
	printf("fake RTMP_Write #%d kind=%s len=%d -> %d\n", F.write_calls, buf, size, fail ? -1 : size);
	return fail ? -1 : size;
}
void RTMP_Init(RTMP *r)
{
	memset(r, 0, sizeof(*r));
	r->m_sb.sb_socket = -1;
	printf("fake RTMP_Init\n");
}
int RTMP_SetupURL(RTMP *r, char *url)
{
	static char tc[300];
	printf("fake RTMP_SetupURL \"%s\" -> %d\n", url, F.setup_ok);
	snprintf(tc, sizeof(tc), "tc:%s", url);
	r->Link.tcUrl.av_val = tc;
	r->Link.tcUrl.av_len = (int)strlen(tc);
	return F.setup_ok;
}
void RTMP_EnableWrite(RTMP *r)
{
	(void)r;
	printf("fake RTMP_EnableWrite\n");
}
int RTMP_AddStream(RTMP *r, const char *s)
{
	(void)r;
	printf("fake RTMP_AddStream \"%s\"\n", s ? s : "(null)");
	return 1;
}
int RTMP_Connect(RTMP *r, RTMPPacket *cp)
{
	(void)cp;
	/* what the plugin configured before connecting */
	printf("fake RTMP_Connect pubUser=\"%.*s\" pubPasswd=\"%.*s\" flashVer=\"%.*s\" swfUrl_is_tcUrl=%d "
	       "outChunk=%d sendChunkInfo=%d nagle=%d bindLen=%d",
	       r->Link.pubUser.av_len, r->Link.pubUser.av_val ? r->Link.pubUser.av_val : "", r->Link.pubPasswd.av_len,
	       r->Link.pubPasswd.av_val ? r->Link.pubPasswd.av_val : "", r->Link.flashVer.av_len,
	       r->Link.flashVer.av_val ? r->Link.flashVer.av_val : "",
	       r->Link.swfUrl.av_val == r->Link.tcUrl.av_val && r->Link.swfUrl.av_len == r->Link.tcUrl.av_len,
	       r->m_outChunkSize, r->m_bSendChunkSizeInfo, r->m_bUseNagle, (int)r->m_bindIP.addrLen);
	if (r->m_bindIP.addrLen && r->m_bindIP.addr.ss_family) {
		char b[INET6_ADDRSTRLEN] = {0};
		netif_addr_to_str(&r->m_bindIP.addr, b, INET6_ADDRSTRLEN);
		printf(" bindAddr=%s", b);
	}
	printf("\n");
	r->last_error_code = F.connect_err;
	if (F.connect_ok) {
		struct sockaddr_in *a = (struct sockaddr_in *)&r->m_sb.sb_addr;
		a->sin_family = AF_INET;
		inet_pton(AF_INET, "192.0.2.7", &a->sin_addr);
	}
	return F.connect_ok;
}
int RTMP_ConnectStream(RTMP *r, int seekTime)
{
	(void)r;
	printf("fake RTMP_ConnectStream seek=%d -> %d\n", seekTime, F.connectstream_ok);
	return F.connectstream_ok;
}
void RTMP_Close(RTMP *r)
{
	(void)r;
	printf("fake RTMP_Close\n");
}
void RTMP_TLS_Free(RTMP *r)
{
	(void)r;
	tls_free_calls++;
}
int RTMP_ReadPacket(RTMP *r, RTMPPacket *p)
{
	(void)p;
	printf("fake RTMP_ReadPacket -> %d\n", F.readpacket_ok);
	r->last_error_code = 0;
	return F.readpacket_ok;
}
void RTMPPacket_Free(RTMPPacket *p)
{
	(void)p;
}
void RTMP_LogSetCallback(RTMP_LogCallback *cb)
{
	(void)cb;
}
void RTMP_LogSetLevel(RTMP_LogLevel l)
{
	(void)l;
}

int fake_ioctl(int fd, unsigned long req, ...)
{
	va_list ap;
	va_start(ap, req);
	int *arg = va_arg(ap, int *);
	va_end(ap);
	(void)fd;
	if (req == FIONREAD) {
		*arg = F.pending;
		return 0;
	}
	return F.ioctl_fbio_fail ? -1 : 0;
}

/* ---- properties (record only) ---- */
obs_properties_t *obs_properties_create(void)
{
	static char p;
	return (obs_properties_t *)&p;
}
obs_property_t *obs_properties_add_int(obs_properties_t *p, const char *n, const char *d, int mi, int ma, int st)
{
	(void)p;
	printf("prop int %s label=%s min=%d max=%d step=%d\n", n, d, mi, ma, st);
	return (obs_property_t *)n;
}
obs_property_t *obs_properties_add_list(obs_properties_t *p, const char *n, const char *d, enum obs_combo_type t,
					enum obs_combo_format f)
{
	(void)p;
	printf("prop list %s label=%s type=%d format=%d\n", n, d, t, f);
	return (obs_property_t *)n;
}
obs_property_t *obs_properties_add_bool(obs_properties_t *p, const char *n, const char *d)
{
	(void)p;
	printf("prop bool %s label=%s\n", n, d);
	return (obs_property_t *)n;
}
void obs_property_int_set_suffix(obs_property_t *p, const char *s)
{
	(void)p;
	printf("prop suffix \"%s\"\n", s);
}
size_t obs_property_list_add_string(obs_property_t *p, const char *n, const char *v)
{
	(void)p;
	printf("prop item name=\"%s\" value=\"%s\"\n", n, v);
	return 0;
}

/* ---- kernel interface list ---- */
#define MAXIFA 16
static struct {
	char name[16], addr[64];
	bool null_addr;
	int family;
} ifa_list[MAXIFA];
static int ifa_n;
static bool ifa_fail;
int fake_getifaddrs(struct ifaddrs **out)
{
	if (ifa_fail)
		return -1;
	struct ifaddrs *head = NULL, **tail = &head;
	for (int i = 0; i < ifa_n; i++) {
		struct ifaddrs *e = calloc(1, sizeof(*e));
		e->ifa_name = ifa_list[i].name;
		if (!ifa_list[i].null_addr) {
			if (ifa_list[i].family == AF_INET) {
				struct sockaddr_in *a = calloc(1, sizeof(*a));
				a->sin_family = AF_INET;
				inet_pton(AF_INET, ifa_list[i].addr, &a->sin_addr);
				e->ifa_addr = (struct sockaddr *)a;
			} else if (ifa_list[i].family == AF_INET6) {
				struct sockaddr_in6 *a = calloc(1, sizeof(*a));
				a->sin6_family = AF_INET6;
				inet_pton(AF_INET6, ifa_list[i].addr, &a->sin6_addr);
				e->ifa_addr = (struct sockaddr *)a;
			} else {
				struct sockaddr *a = calloc(1, sizeof(struct sockaddr_in6));
				a->sa_family = (sa_family_t)ifa_list[i].family;
				e->ifa_addr = a;
			}
		}
		*tail = e;
		tail = &e->ifa_next;
	}
	*out = head;
	return 0;
}
void fake_freeifaddrs(struct ifaddrs *a)
{
	while (a) {
		struct ifaddrs *n = a->ifa_next;
		free(a->ifa_addr);
		free(a);
		a = n;
	}
}

/* ------------------------------------------------------------------------------------------- */
/* the units under test                                                                        */
/* ------------------------------------------------------------------------------------------- */

/* ------------------------------------------------------------------------------------------- */
/* script commands                                                                             */
/* ------------------------------------------------------------------------------------------- */

static void do_reset(void)
{
	S = NULL;
	memset(&out_settings, 0, sizeof(out_settings));
	memset(enc_settings_v, 0, sizeof(enc_settings_v));
	memset(enc_settings_a, 0, sizeof(enc_settings_a));
	memset(venc, 0, sizeof(venc));
	memset(aenc, 0, sizeof(aenc));
	memset(&F, 0, sizeof(F));
	memset(&vinfo, 0, sizeof(vinfo));
	ifa_n = 0, ifa_fail = false;
	F.have_service = true;
	F.can_begin = true;
	F.init_enc_ok = true;
	F.setup_ok = F.connect_ok = F.connectstream_ok = true;
	F.write_len = 10;
	F.readpacket_ok = true;
	F.peak = 1000;
	F.now = 1000000000ULL;
	snprintf(F.url, sizeof(F.url), "rtmp://example.invalid/live");
	snprintf(F.key, sizeof(F.key), "KEY");
	released = 0, tls_free_calls = 0;
	S = rtmp_stream_create(NULL, (obs_output_t *)&output_obj);
	if (!S) {
		printf("create failed\n");
		exit(2);
	}
	S->rtmp.m_sb.sb_socket = -1;
	os_sem_init(&S->send_sem, 0);
	/* libobs applies the output's registered defaults to its settings object */
	rtmp_stream_defaults((obs_data_t *)&out_settings);
	printf("reset ok\n");
}

static long long num(const char *s)
{
	return strtoll(s, NULL, 0);
}

static void print_state(void)
{
	printf("state packets=%zu dropped=%d min_priority=%d congestion=%.9g last_dts_usec=%lld drop_thr_usec=%lld "
	       "pframe_thr_usec=%lld total_bytes=%llu active=%d disconnected=%d encode_error=%d sent_headers=%d "
	       "start_dts_offset=%lld got_first=%d\n",
	       num_buffered_packets(S), S->dropped_frames, S->min_priority, (double)S->congestion,
	       (long long)S->last_dts_usec, (long long)S->drop_threshold_usec, (long long)S->pframe_drop_threshold_usec,
	       (unsigned long long)S->total_bytes_sent, active(S), disconnected(S), os_atomic_load_bool(&S->encode_error),
	       S->sent_headers, (long long)S->start_dts_offset, S->got_first_packet);
}

static void print_dbr(void)
{
	printf("dbr enabled=%d cur=%ld orig=%ld prev=%ld est=%ld inc=%ld audio=%ld data_size=%zu inc_timeout=%llu "
	       "frames=%zu\n",
	       S->dbr_enabled, S->dbr_cur_bitrate, S->dbr_orig_bitrate, S->dbr_prev_bitrate, S->dbr_est_bitrate,
	       S->dbr_inc_bitrate, S->audio_bitrate, S->dbr_data_size, (unsigned long long)S->dbr_inc_timeout,
	       S->dbr_frames.size / sizeof(struct dbr_frame));
}

static void print_table(void)
{
	printf("table n=%zu\n", S->dbr_interpolation_table.num);
	for (size_t i = 0; i < S->dbr_interpolation_table.num; i++) {
		printf("  [%zu]", i);
		for (int j = 0; j < MAX_OUTPUT_VIDEO_ENCODERS; j++)
			printf(" %ld", S->dbr_interpolation_table.array[i].bitrates[j]);
		printf("\n");
	}
}

static void print_queue(void)
{
	size_t n = num_buffered_packets(S);
	printf("queue n=%zu\n", n);
	for (size_t i = 0; i < n; i++) {
		struct encoder_packet *p = deque_data(&S->packets, i * sizeof(*p));
		printf("  [%zu] %s dts_usec=%lld prio=%d kf=%d track=%zu\n", i, p->type == OBS_ENCODER_VIDEO ? "video" : "audio",
		       (long long)p->dts_usec, p->drop_priority, p->keyframe, p->track_idx);
	}
}

static void print_sockaddr(const char *label, struct sockaddr_storage *a, int len)
{
	printf("%s len=%d family=%d", label, len, a->ss_family);
	unsigned char *b = (unsigned char *)a;
	if (a->ss_family == AF_INET)
		printf(" bytes=%02x%02x%02x%02x", b[4], b[5], b[6], b[7]);
	else if (a->ss_family == AF_INET6) {
		printf(" bytes=");
		for (int i = 8; i < 24; i++)
			printf("%02x", b[i]);
	}
	printf("\n");
}

static struct fake_enc *getenc(char kind, int idx)
{
	if (idx < 0 || idx >= (kind == 'v' ? MAX_OUTPUT_VIDEO_ENCODERS : MAX_OUTPUT_AUDIO_ENCODERS)) {
		fprintf(stderr, "bad encoder index\n");
		exit(2);
	}
	return kind == 'v' ? &venc[idx] : &aenc[idx];
}

static void setfield(const char *f, const char *v)
{
	long long n = num(v);
#define FI(name, expr)          \
	if (!strcmp(f, name)) { \
		expr;           \
		return;         \
	}
	FI("path", dstr_copy(&S->path, v))
	FI("key", dstr_copy(&S->key, v))
	FI("username", dstr_copy(&S->username, v))
	FI("password", dstr_copy(&S->password, v))
	FI("bind_ip", dstr_copy(&S->bind_ip, v))
	FI("addrlen_hint", S->addrlen_hint = (socklen_t)n)
	FI("min_priority", S->min_priority = (int)n)
	FI("congestion", S->congestion = (float)strtod(v, NULL))
	FI("last_dts_usec", S->last_dts_usec = n)
	FI("drop_threshold_usec", S->drop_threshold_usec = n)
	FI("pframe_drop_threshold_usec", S->pframe_drop_threshold_usec = n)
	FI("dropped_frames", S->dropped_frames = (int)n)
	FI("total_bytes_sent", S->total_bytes_sent = (uint64_t)n)
	FI("dbr_enabled", S->dbr_enabled = n != 0)
	FI("dbr_cur_bitrate", S->dbr_cur_bitrate = (long)n)
	FI("dbr_orig_bitrate", S->dbr_orig_bitrate = (long)n)
	FI("dbr_prev_bitrate", S->dbr_prev_bitrate = (long)n)
	FI("dbr_est_bitrate", S->dbr_est_bitrate = (long)n)
	FI("dbr_inc_bitrate", S->dbr_inc_bitrate = (long)n)
	FI("dbr_inc_timeout", S->dbr_inc_timeout = (uint64_t)n)
	FI("audio_bitrate", S->audio_bitrate = (long)n)
	FI("dbr_data_size", S->dbr_data_size = (size_t)n)
	FI("active", os_atomic_set_bool(&S->active, n != 0))
	FI("disconnected", os_atomic_set_bool(&S->disconnected, n != 0))
	FI("encode_error", os_atomic_set_bool(&S->encode_error, n != 0))
	FI("connecting", os_atomic_set_bool(&S->connecting, n != 0))
	FI("sent_headers", S->sent_headers = n != 0)
	FI("stop_ts", S->stop_ts = (uint64_t)n)
	FI("shutdown_timeout_ts", S->shutdown_timeout_ts = (uint64_t)n)
	FI("max_shutdown_time_sec", S->max_shutdown_time_sec = (int)n)
	FI("new_socket_loop", S->new_socket_loop = n != 0)
	FI("low_latency_mode", S->low_latency_mode = n != 0)
	FI("write_buf_len", S->write_buf_len = (size_t)n)
	FI("write_buf_size", S->write_buf_size = (size_t)n)
	FI("last_error_code", S->rtmp.last_error_code = (int)n)
	FI("connect_time_ms", S->rtmp.connect_time_ms = (int)n)
	FI("start_dts_offset", S->start_dts_offset = n)
	FI("got_first_packet", S->got_first_packet = n != 0)
	FI("stop_event", if (n) os_event_signal(S->stop_event); else os_event_reset(S->stop_event))
#undef FI
	fprintf(stderr, "unknown field %s\n", f);
	exit(2);
}

static void run_cmd(struct script_line *sl);

/* probe CMD ARGS...: run one command in a forked child; report a crash instead of dying.
 * State changes made by the child are discarded. */
static void run_probe(struct script_line *sl)
{
	struct script_line inner = *sl;
	for (int i = 1; i < sl->argc; i++)
		inner.argv[i - 1] = sl->argv[i];
	inner.argc = sl->argc - 1;
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		run_cmd(&inner);
		fflush(stdout);
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFSIGNALED(st))
		printf("probe crash signal=%s\n", WTERMSIG(st) == SIGSEGV ? "SIGSEGV" : WTERMSIG(st) == SIGABRT ? "SIGABRT" : "other");
	else
		printf("probe exit=%d\n", WEXITSTATUS(st));
}

static void run_cmd(struct script_line *sl)
{
	char **a = sl->argv;
	int n = sl->argc;
	const char *c = a[0];
	if (!strcmp(c, "probe")) {
		printf("> ");
		for (int i = 0; i < n; i++)
			printf("%s%s", i ? " " : "", a[i]);
		printf("\n");
		run_probe(sl);
		return;
	}
	printf("> ");
	for (int i = 0; i < n; i++)
		printf("%s%s", i ? " " : "", a[i]);
	printf("\n");

	if (!strcmp(c, "reset")) {
		do_reset();
	} else if (!strcmp(c, "opt")) { /* opt int|bool|str KEY VALUE */
		struct kv *e = kv_find(&out_settings, a[2], true);
		e->user = true;
		if (!strcmp(a[1], "int"))
			e->kind = 0, e->i = num(a[3]);
		else if (!strcmp(a[1], "bool"))
			e->kind = 1, e->i = num(a[3]);
		else
			e->kind = 2, snprintf(e->s, sizeof(e->s), "%s", a[3]);
	} else if (!strcmp(c, "no_defaults")) { /* drop the registered defaults from the output settings */
		memset(&out_settings, 0, sizeof(out_settings));
	} else if (!strcmp(c, "defaults")) {
		struct fake_data d;
		memset(&d, 0, sizeof(d));
		rtmp_stream_defaults((obs_data_t *)&d);
		for (int i = 0; i < MAXKV; i++)
			if (d.kv[i].used) {
				if (d.kv[i].kind == 2)
					printf("default %s string \"%s\"\n", d.kv[i].key, d.kv[i].s);
				else
					printf("default %s %s %lld\n", d.kv[i].key, d.kv[i].kind ? "bool" : "int", d.kv[i].i);
			}
	} else if (!strcmp(c, "service")) { /* service URL KEY USER PASS ("-" = NULL) */
		F.url_null = !strcmp(a[1], "-"), F.key_null = !strcmp(a[2], "-");
		F.user_null = !strcmp(a[3], "-"), F.pass_null = !strcmp(a[4], "-");
		snprintf(F.url, sizeof(F.url), "%s", a[1]);
		snprintf(F.key, sizeof(F.key), "%s", a[2]);
		snprintf(F.user, sizeof(F.user), "%s", a[3]);
		snprintf(F.pass, sizeof(F.pass), "%s", a[4]);
	} else if (!strcmp(c, "no_service")) {
		F.have_service = false;
	} else if (!strcmp(c, "venc")) { /* venc IDX CODEC BITRATE [CAPS] [NAME] */
		struct fake_enc *e = getenc('v', (int)num(a[1]));
		e->present = true;
		snprintf(e->codec, sizeof(e->codec), "%s", a[2]);
		snprintf(e->name, sizeof(e->name), "%s", n > 5 ? a[5] : "enc");
		e->caps = n > 4 ? (uint32_t)num(a[4]) : 0;
		struct kv *k = kv_find(&enc_settings_v[num(a[1])], "bitrate", true);
		k->kind = 0, k->i = num(a[3]), k->user = true;
	} else if (!strcmp(c, "aenc")) { /* aenc IDX CODEC BITRATE */
		struct fake_enc *e = getenc('a', (int)num(a[1]));
		e->present = true;
		snprintf(e->codec, sizeof(e->codec), "%s", a[2]);
		struct kv *k = kv_find(&enc_settings_a[num(a[1])], "bitrate", true);
		k->kind = 0, k->i = num(a[3]), k->user = true;
	} else if (!strcmp(c, "enc_flag")) { /* enc_flag v|a IDX no_video|no_extra BOOL */
		struct fake_enc *e = getenc(a[1][0], (int)num(a[2]));
		if (!strcmp(a[3], "no_video"))
			e->no_video = num(a[4]);
		else if (!strcmp(a[3], "no_extra"))
			e->no_extra = num(a[4]);
	} else if (!strcmp(c, "colorspace")) { /* colorspace ENUMINT FORMATINT */
		vinfo.colorspace = (enum video_colorspace)num(a[1]);
		vinfo.format = (enum video_format)num(a[2]);
	} else if (!strcmp(c, "peak")) {
		F.peak = (int)num(a[1]);
	} else if (!strcmp(c, "delay")) {
		F.delay = num(a[1]);
	} else if (!strcmp(c, "fake")) { /* fake FIELD VALUE: fake-environment knobs */
		const char *f = a[1];
		long long v = num(a[2]);
		if (!strcmp(f, "setup_ok"))
			F.setup_ok = v;
		else if (!strcmp(f, "connect_ok"))
			F.connect_ok = v;
		else if (!strcmp(f, "connectstream_ok"))
			F.connectstream_ok = v;
		else if (!strcmp(f, "connect_err"))
			F.connect_err = (int)v;
		else if (!strcmp(f, "write_fail_at"))
			F.write_fail_at = (int)v, F.write_calls = 0;
		else if (!strcmp(f, "write_len"))
			F.write_len = (int)v;
		else if (!strcmp(f, "pthread_create_fail"))
			F.pthread_create_fail = (int)v, F.pthread_create_count = 0;
		else if (!strcmp(f, "pending"))
			F.pending = (int)v;
		else if (!strcmp(f, "readpacket_ok"))
			F.readpacket_ok = v;
		else if (!strcmp(f, "can_begin"))
			F.can_begin = v;
		else if (!strcmp(f, "init_enc_ok"))
			F.init_enc_ok = v;
		else if (!strcmp(f, "now"))
			F.now = (uint64_t)v;
		else if (!strcmp(f, "advance"))
			F.now += (uint64_t)v;
		else {
			fprintf(stderr, "unknown fake %s\n", f);
			exit(2);
		}
	} else if (!strcmp(c, "set")) {
		setfield(a[1], a[2]);
	} else if (!strcmp(c, "codec")) { /* codec IDX NAME: stream->video_codec[idx] = to_video_type(name) */
		S->video_codec[num(a[1])] = to_video_type(a[2]);
	} else if (!strcmp(c, "state")) {
		print_state();
	} else if (!strcmp(c, "dbr")) {
		print_dbr();
	} else if (!strcmp(c, "table")) {
		print_table();
	} else if (!strcmp(c, "queue")) {
		print_queue();
	} else if (!strcmp(c, "init_connect")) {
		bool r = init_connect(S);
		printf("init_connect -> %d\n", r);
		printf("conn path=\"%s\" key=\"%s\" user=\"%s\" pass=\"%s\" bind_ip=\"%s\" addrlen_hint=%d "
		       "max_shutdown=%d new_socket_loop=%d low_latency=%d\n",
		       S->path.array ? S->path.array : "", S->key.array ? S->key.array : "",
		       S->username.array ? S->username.array : "", S->password.array ? S->password.array : "",
		       S->bind_ip.array ? S->bind_ip.array : "", (int)S->addrlen_hint, S->max_shutdown_time_sec,
		       S->new_socket_loop, S->low_latency_mode);
		printf("codecs v:");
		for (int i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++)
			printf(" %d", S->video_codec[i]);
		printf(" a:");
		for (int i = 0; i < MAX_OUTPUT_AUDIO_ENCODERS; i++)
			printf(" %d", S->audio_codec[i]);
		printf("\n");
		print_dbr();
		print_table();
		print_state();
	} else if (!strcmp(c, "dbr_json")) {
		bool r = build_dbr_interpolation_table(S, a[1]);
		printf("build_dbr_interpolation_table -> %d\n", r);
		print_table();
	} else if (!strcmp(c, "dbr_frame")) { /* dbr_frame BEG_NS END_NS SIZE */
		struct dbr_frame f = {
			.send_beg = (uint64_t)num(a[1]), .send_end = (uint64_t)num(a[2]), .size = (size_t)num(a[3])};
		dbr_add_frame(S, &f);
		print_dbr();
	} else if (!strcmp(c, "dbr_lower")) {
		bool r = dbr_bitrate_lowered(S);
		printf("dbr_bitrate_lowered -> %d\n", r);
		print_dbr();
	} else if (!strcmp(c, "dbr_inc")) {
		dbr_inc_bitrate(S);
		print_dbr();
	} else if (!strcmp(c, "dbr_set")) {
		dbr_set_bitrate(S);
	} else if (!strcmp(c, "check_drop")) { /* check_drop b|p */
		check_to_drop_frames(S, a[1][0] == 'p');
		print_state();
	} else if (!strcmp(c, "pkt")) { /* pkt video|audio DTS_USEC PRIO KF [TRACK] */
		struct encoder_packet p;
		memset(&p, 0, sizeof(p));
		p.type = a[1][0] == 'v' ? OBS_ENCODER_VIDEO : OBS_ENCODER_AUDIO;
		p.dts_usec = num(a[2]);
		p.sys_dts_usec = p.dts_usec;
		p.dts = p.dts_usec;
		p.pts = p.dts_usec;
		p.timebase_num = 1;
		p.timebase_den = 1000000;
		p.drop_priority = (int)num(a[3]);
		p.keyframe = num(a[4]) != 0;
		p.track_idx = n > 5 ? (size_t)num(a[5]) : 0;
		p.size = 100;
		int before = released;
		size_t q = num_buffered_packets(S);
		rtmp_stream_data(S, &p);
		printf("pkt queued=%d released=%d\n", num_buffered_packets(S) > q, released - before);
		print_state();
	} else if (!strcmp(c, "pkt_null")) {
		rtmp_stream_data(S, NULL);
		printf("encode_error=%d\n", os_atomic_load_bool(&S->encode_error));
	} else if (!strcmp(c, "err")) { /* err CODE: set_output_error */
		S->rtmp.last_error_code = (int)num(a[1]);
		set_output_error(S);
	} else if (!strcmp(c, "try_connect")) {
		int r = try_connect(S);
		printf("try_connect -> %d wbuf_size=%zu new_socket_loop=%d low_latency=%d\n", r, S->write_buf_size,
		       S->new_socket_loop, S->low_latency_mode);
		print_sockaddr("bind", &S->rtmp.m_bindIP.addr, (int)S->rtmp.m_bindIP.addrLen);
		printf("after active=%d disconnected=%d total_bytes=%llu\n", active(S), disconnected(S),
		       (unsigned long long)S->total_bytes_sent);
	} else if (!strcmp(c, "start")) {
		bool r = rtmp_stream_start(S);
		printf("start -> %d connecting=%d\n", r, connecting(S));
	} else if (!strcmp(c, "connect_thread")) {
		os_atomic_set_bool(&S->connecting, true);
		connect_thread(S);
		printf("connecting=%d\n", connecting(S));
	} else if (!strcmp(c, "stop")) { /* stop TS_NS */
		rtmp_stream_stop(S, (uint64_t)num(a[1]));
		printf("stop_ts=%llu shutdown_timeout_ts=%llu stopping=%d\n", (unsigned long long)S->stop_ts,
		       (unsigned long long)S->shutdown_timeout_ts, stopping(S));
	} else if (!strcmp(c, "post")) { /* post N: add N tokens to the send semaphore */
		for (int i = 0; i < num(a[1]); i++)
			os_sem_post(S->send_sem);
	} else if (!strcmp(c, "run_send")) {
		send_thread(S);
		print_state();
	} else if (!strcmp(c, "send_headers")) {
		bool r = send_headers(S);
		printf("send_headers -> %d\n", r);
	} else if (!strcmp(c, "send_footers")) {
		bool r = send_footers(S);
		printf("send_footers -> %d\n", r);
	} else if (!strcmp(c, "getters")) {
		printf("getters bytes=%llu dropped=%d congestion=%.9g connect_time=%d\n",
		       (unsigned long long)rtmp_stream_total_bytes_sent(S), rtmp_stream_dropped_frames(S),
		       (double)rtmp_stream_congestion(S), rtmp_stream_connect_time(S));
	} else if (!strcmp(c, "info")) {
		printf("info id=%s flags=0x%x protocols=%s video=%s audio=%s\n", rtmp_output_info.id,
		       (unsigned)rtmp_output_info.flags, rtmp_output_info.protocols, rtmp_output_info.encoded_video_codecs,
		       rtmp_output_info.encoded_audio_codecs);
	} else if (!strcmp(c, "props")) {
		rtmp_stream_properties(NULL);
	} else if (!strcmp(c, "destroy")) {
		rtmp_stream_destroy(S);
		S = NULL;
		printf("destroyed tls_free=%d\n", tls_free_calls);
		do_reset();
	} else if (!strcmp(c, "released")) {
		printf("released=%d\n", released);
	} else if (!strcmp(c, "drop_frames")) { /* drop_frames PRIO */
		drop_frames(S, "x", (int)num(a[1]), false);
		print_state();
	} else if (!strcmp(c, "find_first")) {
		struct encoder_packet f;
		memset(&f, 0, sizeof(f));
		bool r = find_first_video_packet(S, &f);
		printf("find_first -> %d dts_usec=%lld\n", r, (long long)(r ? f.dts_usec : 0));
	} else if (!strcmp(c, "can_shutdown")) { /* can_shutdown SYS_DTS_USEC */
		struct encoder_packet p;
		memset(&p, 0, sizeof(p));
		p.sys_dts_usec = num(a[1]);
		printf("can_shutdown -> %d\n", can_shutdown_stream(S, &p));
	} else if (!strcmp(c, "ifa")) { /* ifa NAME v4|v6|other|null ADDR */
		int i = ifa_n++;
		snprintf(ifa_list[i].name, sizeof(ifa_list[i].name), "%s", a[1]);
		ifa_list[i].null_addr = !strcmp(a[2], "null");
		ifa_list[i].family = !strcmp(a[2], "v4") ? AF_INET : !strcmp(a[2], "v6") ? AF_INET6 : AF_PACKET;
		if (n > 3)
			snprintf(ifa_list[i].addr, sizeof(ifa_list[i].addr), "%s", a[3]);
	} else if (!strcmp(c, "ifa_clear")) {
		ifa_n = 0;
		ifa_fail = false;
	} else if (!strcmp(c, "ifa_fail")) {
		ifa_fail = true;
	} else if (!strcmp(c, "netif_get")) {
		struct netif_saddr_data d = {0};
		netif_get_addrs(&d);
		printf("netif_get n=%zu\n", d.addrs.num);
		for (size_t i = 0; i < d.addrs.num; i++)
			printf("  name=\"%s\" addr=\"%s\"\n", d.addrs.array[i].name, d.addrs.array[i].addr);
		netif_log_saddrs(&d);
		netif_saddr_data_free(&d);
	} else if (!strcmp(c, "str_to_addr")) { /* str_to_addr TEXT|- */
		struct sockaddr_storage ss;
		memset(&ss, 0xAA, sizeof(ss));
		int len = 99;
		bool r = netif_str_to_addr(&ss, &len, !strcmp(a[1], "-") ? NULL : a[1]);
		printf("netif_str_to_addr -> %d\n", r);
		print_sockaddr("out", &ss, len);
	} else if (!strcmp(c, "addr_to_str")) { /* addr_to_str v4|v6|other HEXBYTES LEN */
		struct sockaddr_storage ss;
		memset(&ss, 0, sizeof(ss));
		char buf[80];
		memset(buf, 'Z', sizeof(buf));
		int fam = !strcmp(a[1], "v4") ? AF_INET : !strcmp(a[1], "v6") ? AF_INET6 : AF_UNIX;
		ss.ss_family = (sa_family_t)fam;
		const char *h = a[2];
		unsigned char *dst = fam == AF_INET ? (unsigned char *)&((struct sockaddr_in *)&ss)->sin_addr
						    : (unsigned char *)&((struct sockaddr_in6 *)&ss)->sin6_addr;
		for (int i = 0; h[0] && h[1] && h[0] != '-'; i++, h += 2)
			dst[i] = (unsigned char)(script_hexval(h[0]) * 16 + script_hexval(h[1]));
		bool r = netif_addr_to_str(&ss, buf, (int)num(a[3]));
		printf("netif_addr_to_str -> %d", r);
		if (r)
			printf(" \"%s\" rest_zeroed=%d", buf, buf[(int)num(a[3]) - 1] == 0);
		printf("\n");
		bool r2 = netif_addr_to_str(NULL, buf, 64);
		bool r3 = netif_addr_to_str(&ss, NULL, 64);
		printf("null_in -> %d null_out -> %d\n", r2, r3);
	} else {
		fprintf(stderr, "unknown command %s (line %d)\n", c, sl->lineno);
		exit(2);
	}
}

/* The text after "table failed: " comes from the JSON library and varies by version: mask it. */
static void masking_log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	char *p = strstr(msg, "interpolation table failed: ");
	if (p)
		strcpy(p + strlen("interpolation table failed: "), "<json-error>");
	const char *name = level == LOG_ERROR ? "error" : level == LOG_WARNING ? "warning" : level == LOG_INFO ? "info" : "debug";
	printf("log %s %s\n", name, msg);
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	base_set_log_handler(masking_log_handler, NULL);
	alarm(20); /* a script that would block the (synchronous) send loop is killed */
	struct script_line sl;
	int lineno = 0;
	do_reset();
	while (script_next(f, &sl, &lineno))
		run_cmd(&sl);
	fclose(f);
	return 0;
}
