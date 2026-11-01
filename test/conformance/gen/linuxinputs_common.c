/*
 * Conformance fakes shared by all linuxinputs wrappers (spec S-062): deterministic output from
 * several threads, fake clock, fake obs_source_output_audio recorder, fake module text, property dump.
 * Test tooling, not part of OBS.
 */
#define _GNU_SOURCE
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <obs-module.h>
#include "harness.h"
#include "linuxinputs_api.h"

obs_data_t *li_settings;
obs_properties_t *li_props;

static pthread_t main_thr;
static pthread_mutex_t say_lock = PTHREAD_MUTEX_INITIALIZER;
static struct pend {
	char *text;
	pthread_t thr;
} pending[8192];
static int npending;
static int (*classifier)(pthread_t);
void li_set_classifier(int (*fn)(pthread_t)) { classifier = fn; }
static volatile uint64_t g_now;

void li_init(void) { main_thr = pthread_self(); }

void li_say(const char *fmt, ...)
{
	char buf[2048];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	if (pthread_equal(pthread_self(), main_thr)) {
		puts(buf);
		return;
	}
	pthread_mutex_lock(&say_lock);
	if (npending < 8192)
		pending[npending++] = (struct pend){strdup(buf), pthread_self()};
	pthread_mutex_unlock(&say_lock);
}

/* Prints (in arrival order) what other threads said since the last flush. Main thread only. */
void li_flush(void)
{
	pthread_mutex_lock(&say_lock);
	/* lines of different worker threads are ordered by thread class (stable), so that output does not depend on scheduling */
	if (classifier)
		for (int i = 1; i < npending; i++) {
			struct pend t = pending[i];
			int c = classifier(t.thr), j = i - 1;
			while (j >= 0 && classifier(pending[j].thr) > c) {
				pending[j + 1] = pending[j];
				j--;
			}
			pending[j + 1] = t;
		}
	for (int i = 0; i < npending; i++) {
		puts(pending[i].text);
		free(pending[i].text);
	}
	npending = 0;
	pthread_mutex_unlock(&say_lock);
}

void li_sleep_ms(int ms)
{
	struct timespec ts = {ms / 1000, (ms % 1000) * 1000000L};
	nanosleep(&ts, NULL);
}

void li_set_now(uint64_t ns) { g_now = ns; }
uint64_t li_get_now(void) { return g_now; }
uint64_t os_gettime_ns(void) { return g_now; }

const char *obs_module_text(const char *key) { return key; }

/* Log lines go through li_say; the platform text after ", errno: " is masked. */
static void li_log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	for (char *p = msg; *p; p++)
		if (*p == '\n')
			*p = ' ';
	char *e = strstr(msg, ", errno: ");
	if (e)
		strcpy(e + 9, "<E>");
	li_say("log %s %s", level == LOG_ERROR ? "error" : level == LOG_WARNING ? "warning" : level == LOG_INFO ? "info" : "debug", msg);
}
void li_install_log(void) { base_set_log_handler(li_log_handler, NULL); }

const char *li_afmt(enum audio_format f)
{
	switch (f) {
	case AUDIO_FORMAT_UNKNOWN: return "UNKNOWN";
	case AUDIO_FORMAT_U8BIT: return "U8BIT";
	case AUDIO_FORMAT_16BIT: return "16BIT";
	case AUDIO_FORMAT_32BIT: return "32BIT";
	case AUDIO_FORMAT_FLOAT: return "FLOAT";
	case AUDIO_FORMAT_32BIT_PLANAR: return "32BIT_PLANAR";
	case AUDIO_FORMAT_FLOAT_PLANAR: return "FLOAT_PLANAR";
	default: return "OTHER";
	}
}
const char *li_spk(enum speaker_layout s)
{
	switch (s) {
	case SPEAKERS_UNKNOWN: return "UNKNOWN";
	case SPEAKERS_MONO: return "MONO";
	case SPEAKERS_STEREO: return "STEREO";
	case SPEAKERS_2POINT1: return "2.1";
	case SPEAKERS_4POINT0: return "4.0";
	case SPEAKERS_4POINT1: return "4.1";
	case SPEAKERS_5POINT1: return "5.1";
	case SPEAKERS_7POINT1: return "7.1";
	default: return "OTHER";
	}
}

/* ------------------------------------------------------------ audio recorder */
struct arec {
	int speakers, format;
	unsigned rate, frames;
	unsigned long long ts;
	uint8_t head[8];
	int hlen;
};
static struct arec recs[4096];
static volatile unsigned nrecs;
static pthread_mutex_t arec_lock = PTHREAD_MUTEX_INITIALIZER;
volatile int li_audio_framebytes = 1;

void obs_source_output_audio(obs_source_t *s, const struct obs_source_audio *a)
{
	(void)s;
	pthread_mutex_lock(&arec_lock);
	if (nrecs < 4096) {
		struct arec *r = &recs[nrecs];
		r->speakers = a->speakers;
		r->format = a->format;
		r->rate = a->samples_per_sec;
		r->frames = a->frames;
		r->ts = a->timestamp;
		size_t total = (size_t)a->frames * (size_t)li_audio_framebytes;
		r->hlen = total < 8 ? (int)total : 8;
		if (a->data[0])
			memcpy(r->head, a->data[0], r->hlen);
		nrecs++;
	}
	pthread_mutex_unlock(&arec_lock);
}
void li_audio_reset(void)
{
	pthread_mutex_lock(&arec_lock);
	nrecs = 0;
	pthread_mutex_unlock(&arec_lock);
}
unsigned li_audio_count(void) { return nrecs; }
bool li_audio_wait(unsigned n, int timeout_ms)
{
	for (int i = 0; i < timeout_ms && nrecs < n; i++)
		li_sleep_ms(1);
	return nrecs >= n;
}
void li_audio_dump(void)
{
	pthread_mutex_lock(&arec_lock);
	for (unsigned i = 0; i < nrecs; i++) {
		struct arec *r = &recs[i];
		printf("audio[%u] speakers=%s format=%s rate=%u frames=%u ts=%llu head=", i, li_spk(r->speakers), li_afmt(r->format), r->rate,
		       r->frames, r->ts);
		script_print_hex(r->head, r->hlen);
		printf("\n");
	}
	pthread_mutex_unlock(&arec_lock);
}

/* ------------------------------------------------------------ property dump */
static const char *ptype(enum obs_property_type t)
{
	switch (t) {
	case OBS_PROPERTY_BOOL: return "bool";
	case OBS_PROPERTY_INT: return "int";
	case OBS_PROPERTY_FLOAT: return "float";
	case OBS_PROPERTY_TEXT: return "text";
	case OBS_PROPERTY_LIST: return "list";
	case OBS_PROPERTY_GROUP: return "group";
	default: return "other";
	}
}
void li_dump_props(obs_properties_t *props, int depth)
{
	if (!props) {
		printf("%*sprops (null)\n", depth * 2, "");
		return;
	}
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		enum obs_property_type t = obs_property_get_type(p);
		printf("%*sprop %s type=%s desc=", depth * 2, "", obs_property_name(p), ptype(t));
		script_print_quoted(obs_property_description(p));
		printf(" visible=%d enabled=%d", obs_property_visible(p), obs_property_enabled(p));
		if (t == OBS_PROPERTY_INT)
			printf(" min=%d max=%d step=%d suffix=\"%s\"", obs_property_int_min(p), obs_property_int_max(p), obs_property_int_step(p),
			       obs_property_int_suffix(p) ? obs_property_int_suffix(p) : "");
		if (t == OBS_PROPERTY_TEXT)
			printf(" texttype=%d", (int)obs_property_text_type(p));
		if (t == OBS_PROPERTY_LIST) {
			enum obs_combo_format f = obs_property_list_format(p);
			printf(" format=%s items=%zu", f == OBS_COMBO_FORMAT_INT ? "int" : f == OBS_COMBO_FORMAT_STRING ? "string" : "other",
			       obs_property_list_item_count(p));
		}
		printf("\n");
		if (t == OBS_PROPERTY_LIST) {
			size_t n = obs_property_list_item_count(p);
			enum obs_combo_format f = obs_property_list_format(p);
			for (size_t i = 0; i < n; i++) {
				printf("%*s- ", depth * 2 + 2, "");
				script_print_quoted(obs_property_list_item_name(p, i));
				if (f == OBS_COMBO_FORMAT_INT)
					printf(" = %lld", obs_property_list_item_int(p, i));
				else
					printf(" = \"%s\"", obs_property_list_item_string(p, i));
				printf(" disabled=%d\n", obs_property_list_item_disabled(p, i));
			}
		}
	}
}

/* obs-data.c file helpers: never reached by these scripts */
char *os_quick_read_utf8_file(const char *path) { (void)path; return NULL; }
int os_rename(const char *a, const char *b) { (void)a; (void)b; return -1; }
bool os_quick_write_utf8_file(const char *p, const char *c, size_t n, bool m) { (void)p; (void)c; (void)n; (void)m; return false; }
bool os_quick_write_utf8_file_safe(const char *p, const char *c, size_t n, bool m, const char *t, const char *b)
{
	(void)p; (void)c; (void)n; (void)m; (void)t; (void)b;
	return false;
}
