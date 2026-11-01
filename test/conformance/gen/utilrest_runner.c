/*
 * Conformance runner for spec S-055 (utilrest): util/crc32.c, util/pipe.c + pipe-posix.c, util/source-profiler.c,
 * util/platform-nix-dbus.c, util/platform-nix-portal.c and audio-monitoring/pulse (wrapper, output, enum, availability).
 * The script language is documented in test/conformance/README.md (section "utilrest runner").
 * Real C is linked; only the environment is faked (see utilrest_stubs.c, utilrest_pafake.c); D-Bus runs against a
 * private dbus-daemon with an in-process fake service.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdarg.h>
#include <time.h>
#include <unistd.h>
#include <gio/gio.h>
#include <pulse/sample.h>

#include "util/bmem.h"
#include "util/pipe.h"
#include "util/crc32.h"
#include "util/platform.h"
#include "util/source-profiler.h"
#include "obs-internal.h"
#include "harness.h"
#include "utilrest_api.h"

/* ---- functions of the linked files that have no public header ---- */
struct dbus_sleep_info;
struct dbus_sleep_info *dbus_sleep_info_create(void);
void dbus_sleep_info_destroy(struct dbus_sleep_info *info);
void dbus_inhibit_sleep(struct dbus_sleep_info *info, const char *reason, bool active);
struct portal_inhibit_info;
struct portal_inhibit_info *portal_inhibit_info_create(void);
void portal_inhibit_info_destroy(struct portal_inhibit_info *info);
void portal_inhibit(struct portal_inhibit_info *info, const char *reason, bool active);
void get_default_id(char **id);
bool devices_match(const char *id1, const char *id2);
int_fast32_t pulseaudio_init(void);
void pulseaudio_unref(void);
struct audio_monitor;
struct audio_monitor *audio_monitor_create(obs_source_t *source);
void audio_monitor_reset(struct audio_monitor *m);
void audio_monitor_destroy(struct audio_monitor *m);
void process_volume(const struct audio_monitor *monitor, float vol, uint8_t *const *resample_data, uint32_t resample_frames);
bool obs_audio_monitoring_available(void);
void obs_enum_audio_monitoring_devices(obs_enum_audio_device_cb cb, void *data);

static void fatal(const struct script_line *sl, const char *why)
{
	fprintf(stderr, "script: line %d: %s\n", sl->lineno, why);
	exit(2);
}
static const char *dash(const char *s) { return strcmp(s, "-") == 0 ? NULL : s; }
static uint64_t u64(const char *s) { return strtoull(s, NULL, 0); }

/* log handler: the harness one, but error texts that embed system / GLib wording are cut at the first ": " */
static void ur_log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	if (!strncmp(msg, "Could not create dbus connection", 32) || !strncmp(msg, "Failed to call ", 15) ||
	    !strncmp(msg, "Failed to inhibit", 17) || !strncmp(msg, "Error uninhibiting", 18)) {
		char *p = strstr(msg, ": ");
		if (p) p[1] = 0;
	}
	printf("log %s ", level == LOG_ERROR ? "error" : level == LOG_WARNING ? "warning" : level == LOG_INFO ? "info" : "debug");
	for (const unsigned char *p = (const unsigned char *)msg; *p; p++) {
		if (*p == '\n') putchar(' ');
		else if (*p < 0x20 || *p >= 0x7f) printf("\\x%02x", *p);
		else putchar(*p);
	}
	putchar('\n');
}

void ur_pa_log(const char *fmt, ...)
{
	va_list ap;
	fputs("pa ", stdout);
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
	putchar('\n');
}

/* ================================================================ pipes */
#define NPIPE 8
static os_process_pipe_t *pipes[NPIPE];
static struct os_process_args *args_h;

static void print_buf(const uint8_t *b, size_t n)
{
	char *s = bmalloc(n + 1);
	memcpy(s, b, n);
	s[n] = 0;
	script_print_quoted(s);
	bfree(s);
}

/* ================================================================ profiler */
#define NSRC 8
static struct obs_source srcs[NSRC];
static struct obs_core core;

static void print_result(const profiler_result_t *r)
{
	printf("tick_avg=%llu tick_max=%llu render_avg=%llu render_max=%llu render_sum=%llu\n", (unsigned long long)r->tick_avg,
	       (unsigned long long)r->tick_max, (unsigned long long)r->render_avg, (unsigned long long)r->render_max,
	       (unsigned long long)r->render_sum);
	printf("gpu_avg=%llu gpu_max=%llu gpu_sum=%llu\n", (unsigned long long)r->render_gpu_avg, (unsigned long long)r->render_gpu_max,
	       (unsigned long long)r->render_gpu_sum);
	printf("async_input=%.9g in_best=%llu in_worst=%llu async_rendered=%.9g out_best=%llu out_worst=%llu\n", r->async_input,
	       (unsigned long long)r->async_input_best, (unsigned long long)r->async_input_worst, r->async_rendered,
	       (unsigned long long)r->async_rendered_best, (unsigned long long)r->async_rendered_worst);
}

/* per-source open timers (render passes in flight) */
static gs_timer_t *cur_timer;
static uint64_t cur_start;
static uint64_t clock_ns;
static void adv(uint64_t d) { clock_ns += d; ur_set_clock(clock_ns); }

/* ================================================================ pulse monitor */
#define NMON 4
static struct audio_monitor *mons[NMON];
static obs_source_t *mon_src[NMON];

static int enum_count, enum_stop_after;
static bool enum_cb(void *data, const char *name, const char *id)
{
	(void)data;
	printf("enum name=%s id=%s\n", name, id);
	enum_count++;
	return enum_stop_after && enum_count >= enum_stop_after ? false : true;
}

/* ================================================================ D-Bus fake */
static pid_t bus_pid;
static FILE *bus_fp;
static char bus_addr[512];
static GMutex svc_mu;
static guint svc_total, svc_base;
static GPtrArray *svc_log;
static GHashTable *svc_fail;
static char svc_names[4][64];
static int svc_nnames;
static GDBusConnection *svc_conn;
static GMainLoop *svc_loop;
static GThread *svc_thread;
static volatile gint svc_ready;
static char last_portal_path[512], last_sender[64];

static void svc_add(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	g_mutex_lock(&svc_mu);
	g_ptr_array_add(svc_log, g_strdup(buf));
	svc_total++;
	g_mutex_unlock(&svc_mu);
}
static guint svc_count(void)
{
	g_mutex_lock(&svc_mu);
	guint n = svc_total - svc_base;
	g_mutex_unlock(&svc_mu);
	return n;
}

/* Replaces run-time dependent pieces: random tokens and unique bus names. */
static char *normalise(const char *in)
{
	GString *o = g_string_new(NULL);
	for (const char *p = in; *p;) {
		if (!strncmp(p, "obs_inhibit_portal", 18)) {
			g_string_append(o, "obs_inhibit_portal#");
			p += 18;
			while (g_ascii_isdigit(*p)) p++;
		} else if (!strncmp(p, "/request/", 9)) {
			g_string_append(o, "/request/<S>/");
			p += 9;
			while (*p && *p != '/') p++;
			if (*p == '/') p++;
		} else
			g_string_append_c(o, *p++);
	}
	return g_string_free(o, FALSE);
}

static GDBusMessage *svc_reply(GDBusMessage *m, GVariant *body)
{
	GDBusMessage *r = g_dbus_message_new_method_reply(m);
	g_dbus_message_set_body(r, body);
	return r;
}

static GMainContext *svc_ctx;
static int svc_delay_ms;
struct delayed { GDBusConnection *c; GDBusMessage *r; };
static gboolean delayed_send(gpointer p)
{
	struct delayed *d = p;
	g_dbus_connection_send_message(d->c, d->r, G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL);
	g_object_unref(d->r);
	g_object_unref(d->c);
	g_free(d);
	return G_SOURCE_REMOVE;
}

static GDBusMessage *svc_filter(GDBusConnection *c, GDBusMessage *m, gboolean incoming, gpointer ud)
{
	(void)ud;
	if (!incoming || g_dbus_message_get_message_type(m) != G_DBUS_MESSAGE_TYPE_METHOD_CALL)
		return m;
	const char *member = g_dbus_message_get_member(m), *iface = g_dbus_message_get_interface(m);
	const char *path = g_dbus_message_get_path(m), *dest = g_dbus_message_get_destination(m);
	GVariant *body = g_dbus_message_get_body(m);
	if (!member || !iface || !path || !dest || (!strncmp(iface, "org.freedesktop.DBus", 20) && !strstr(path, "portal")))
		return m;
	char *bs = body ? g_variant_print(body, FALSE) : g_strdup("()");
	char *nb = normalise(bs), *np = normalise(path);
	svc_add("call %s.%s path=%s body=%s", iface, member, np, nb);
	g_free(bs); g_free(nb); g_free(np);

	GDBusMessage *r = NULL;
	if (g_hash_table_contains(svc_fail, member)) {
		r = g_dbus_message_new_method_error(m, "org.example.Failure", "forced failure");
	} else if (!strcmp(member, "Inhibit") && !strcmp(iface, "org.freedesktop.portal.Inhibit")) {
		/* handle = /org/freedesktop/portal/desktop/request/<sender>/<token> */
		const char *token = "";
		GVariant *opts = g_variant_get_child_value(body, 2);
		GVariant *tv = g_variant_lookup_value(opts, "handle_token", G_VARIANT_TYPE_STRING);
		if (tv) token = g_variant_get_string(tv, NULL);
		g_snprintf(last_sender, sizeof(last_sender), "%s", g_dbus_message_get_sender(m));
		char *sender = g_strdup(g_dbus_message_get_sender(m) + 1);
		for (char *q = sender; *q; q++) if (*q == '.') *q = '_';
		g_snprintf(last_portal_path, sizeof(last_portal_path), "/org/freedesktop/portal/desktop/request/%s/%s", sender, token);
		r = svc_reply(m, g_variant_new("(o)", last_portal_path));
		g_free(sender);
		if (tv) g_variant_unref(tv);
		g_variant_unref(opts);
	} else if (!strcmp(member, "Get") && !strcmp(iface, "org.freedesktop.DBus.Properties")) {
		r = svc_reply(m, g_variant_new("(v)", g_variant_new_uint32(4)));
	} else if (!strcmp(member, "Inhibit")) {
		r = svc_reply(m, g_variant_new("(u)", !strcmp(dest, "org.mate.SessionManager") || !strcmp(dest, "org.gnome.SessionManager") ? 43u : 42u));
	} else {
		r = svc_reply(m, g_variant_new("()"));
	}
	if (svc_delay_ms && !strcmp(member, "Inhibit")) {
		struct delayed *d = g_new0(struct delayed, 1);
		d->c = g_object_ref(c);
		d->r = r;
		GSource *src = g_timeout_source_new((guint)svc_delay_ms);
		g_source_set_callback(src, delayed_send, d, NULL);
		g_source_attach(src, svc_ctx);
		g_source_unref(src);
	} else {
		g_dbus_connection_send_message(c, r, G_DBUS_SEND_MESSAGE_FLAGS_NONE, NULL, NULL);
		g_object_unref(r);
	}
	return NULL; /* handled */
}

static gpointer svc_main(gpointer unused)
{
	(void)unused;
	GMainContext *ctx = svc_ctx = g_main_context_new();
	g_main_context_push_thread_default(ctx);
	svc_loop = g_main_loop_new(ctx, FALSE);
	svc_conn = g_dbus_connection_new_for_address_sync(bus_addr, G_DBUS_CONNECTION_FLAGS_AUTHENTICATION_CLIENT |
								G_DBUS_CONNECTION_FLAGS_MESSAGE_BUS_CONNECTION,
							  NULL, NULL, NULL);
	if (!svc_conn) { fprintf(stderr, "service connection failed\n"); exit(2); }
	g_dbus_connection_add_filter(svc_conn, svc_filter, NULL, NULL);
	for (int i = 0; i < svc_nnames; i++) {
		GVariant *rep = g_dbus_connection_call_sync(svc_conn, "org.freedesktop.DBus", "/org/freedesktop/DBus", "org.freedesktop.DBus",
							    "RequestName", g_variant_new("(su)", svc_names[i], 0u), NULL,
							    G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL);
		if (rep) g_variant_unref(rep);
	}
	g_atomic_int_set(&svc_ready, 1);
	g_main_loop_run(svc_loop);
	return NULL;
}

static void bus_start(void)
{
	if (bus_pid) return;
	bus_fp = popen("dbus-daemon --session --nofork --print-address=1 --print-pid=1 2>/dev/null", "r");
	char pid[32] = "";
	if (!bus_fp || !fgets(bus_addr, sizeof(bus_addr), bus_fp) || !fgets(pid, sizeof(pid), bus_fp)) {
		fprintf(stderr, "cannot start dbus-daemon\n");
		exit(2);
	}
	bus_addr[strcspn(bus_addr, "\n")] = 0;
	bus_pid = atoi(pid);
	setenv("DBUS_SESSION_BUS_ADDRESS", bus_addr, 1);
	svc_log = g_ptr_array_new_with_free_func(g_free);
	svc_fail = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
}

/* iterates the default main context for ms milliseconds (async portal callbacks run there) */
static void pump(int ms)
{
	struct timespec ts = {0, 1000000};
	for (int i = 0; i < ms; i++) {
		while (g_main_context_iteration(NULL, FALSE)) {}
		nanosleep(&ts, NULL);
	}
}

static void svc_start_thread(void)
{
	if (svc_thread) return;
	svc_thread = g_thread_new("svc", svc_main, NULL);
	while (!g_atomic_int_get(&svc_ready)) usleep(1000);
}

/* ================================================================ main */
int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	base_set_log_handler(ur_log_handler, NULL);
	srand(1);
	obs = &core;
	core.video.graphics = (graphics_t *)&core; /* any non-NULL */
	pthread_mutex_init(&core.audio.monitoring_mutex, NULL);
	ur_pa_reset();
	for (int i = 0; i < NSRC; i++) { srcs[i].context.name = bstrdup("src"); srcs[i].user_volume = 1.0f; srcs[i].activate_refs = 1; }

	struct script_line sl;
	int lineno = 0;
	struct dbus_sleep_info *dsi[2] = {0};
	struct portal_inhibit_info *pii[2] = {0};

	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
#define IS(x) (strcmp(c, x) == 0)
		if (IS("print")) {
			script_need(&sl, 2);
			puts(sl.argv[1]);
		/* ---------------- crc32 */
		} else if (IS("crc32")) { /* crc32 <seed-hex> <hex|-> */
			script_need(&sl, 3);
			size_t n; uint8_t *b = script_parse_hex(sl.argv[2], &n);
			printf("crc32 %08x\n", calc_crc32((uint32_t)strtoul(sl.argv[1], NULL, 16), b, n));
			free(b);
		} else if (IS("crc32s")) { /* crc32s <seed-hex> "text" */
			script_need(&sl, 3);
			printf("crc32 %08x\n", calc_crc32((uint32_t)strtoul(sl.argv[1], NULL, 16), sl.argv[2], strlen(sl.argv[2])));
		} else if (IS("crc32_null0")) { /* size 0 with a NULL buffer */
			script_need(&sl, 2);
			printf("crc32 %08x\n", calc_crc32((uint32_t)strtoul(sl.argv[1], NULL, 16), NULL, 0));
		/* ---------------- pipes */
		} else if (IS("popen")) { /* popen <slot> <type|-> <cmdline|-> */
			script_need(&sl, 4);
			int s = atoi(sl.argv[1]);
			pipes[s] = os_process_pipe_create(dash(sl.argv[3]), dash(sl.argv[2]));
			printf("popen %d %s\n", s, pipes[s] ? "ok" : "null");
		} else if (IS("args")) { /* args <exe> [arg...]; arguments starting with %% are added with addf("%s") */
			script_need(&sl, 2);
			if (args_h) os_process_args_destroy(args_h);
			args_h = os_process_args_create(sl.argv[1]);
			for (int i = 2; i < sl.argc; i++) {
				if (sl.argv[i][0] == '%') os_process_args_add_argf(args_h, "%s-%d", sl.argv[i] + 1, i);
				else os_process_args_add_arg(args_h, sl.argv[i]);
			}
			char **av = os_process_args_get_argv(args_h);
			printf("args argc=%zu argv:", os_process_args_get_argc(args_h));
			for (size_t i = 0; av[i]; i++) { putchar(' '); script_print_quoted(av[i]); }
			printf(" terminator=%s\n", av[os_process_args_get_argc(args_h)] ? "BAD" : "null");
		} else if (IS("popen2")) { /* popen2 <slot> <type|-> : uses the last 'args' */
			script_need(&sl, 3);
			int s = atoi(sl.argv[1]);
			pipes[s] = os_process_pipe_create2(args_h, dash(sl.argv[2]));
			printf("popen2 %d %s\n", s, pipes[s] ? "ok" : "null");
		} else if (IS("pread") || IS("pread_err")) { /* pread <slot> <len> */
			script_need(&sl, 3);
			int s = atoi(sl.argv[1]);
			size_t len = (size_t)atol(sl.argv[2]);
			uint8_t *b = bmalloc(len + 1);
			size_t n = IS("pread") ? os_process_pipe_read(pipes[s], b, len) : os_process_pipe_read_err(pipes[s], b, len);
			printf("%s %d n=%zu ", c, s, n);
			print_buf(b, n);
			putchar('\n');
			bfree(b);
		} else if (IS("pwrite")) { /* pwrite <slot> "data" */
			script_need(&sl, 3);
			int s = atoi(sl.argv[1]);
			printf("pwrite %d n=%zu\n", s, os_process_pipe_write(pipes[s], (const uint8_t *)sl.argv[2], strlen(sl.argv[2])));
		} else if (IS("pwrite_big")) { /* pwrite_big <slot> <n> : n bytes of 'a' */
			int s = atoi(sl.argv[1]);
			size_t n = (size_t)atol(sl.argv[2]);
			uint8_t *b = bmalloc(n);
			memset(b, 'a', n);
			printf("pwrite_big %d n=%zu\n", s, os_process_pipe_write(pipes[s], b, n));
			bfree(b);
		} else if (IS("pclose")) {
			script_need(&sl, 2);
			int s = atoi(sl.argv[1]);
			int r = os_process_pipe_destroy(pipes[s]);
			pipes[s] = NULL;
			printf("pclose %d ret=%d\n", s, r);
		} else if (IS("pclose_signalled")) { /* child killed by SIGKILL: the result is not an exit status */
			script_need(&sl, 2);
			int s = atoi(sl.argv[1]);
			int r = os_process_pipe_destroy(pipes[s]);
			pipes[s] = NULL;
			printf("pclose %d ret_is_positive=%d\n", s, r > 0);
		} else if (IS("pclose_null")) {
			printf("pclose_null ret=%d\n", os_process_pipe_destroy(NULL));
		} else if (IS("pnull_ops")) { /* every I/O function on a NULL handle */
			uint8_t b[4];
			printf("pnull read=%zu read_err=%zu write=%zu\n", os_process_pipe_read(NULL, b, 4), os_process_pipe_read_err(NULL, b, 4),
			       os_process_pipe_write(NULL, b, 4));
		/* ---------------- source profiler */
		} else if (IS("sp_clock")) { clock_ns = u64(sl.argv[1]); ur_set_clock(clock_ns);
		} else if (IS("sp_adv")) { adv(u64(sl.argv[1]));
		} else if (IS("sp_enable")) { source_profiler_enable(atoi(sl.argv[1]));
		} else if (IS("sp_gpu")) { source_profiler_gpu_enable(atoi(sl.argv[1]));
		} else if (IS("sp_reset_video")) {
			struct obs_video_info ovi = {.fps_num = (uint32_t)atol(sl.argv[1]), .fps_den = (uint32_t)atol(sl.argv[2])};
			source_profiler_reset_video(&ovi);
		} else if (IS("sp_frame_begin")) { source_profiler_frame_begin();
		} else if (IS("sp_frame_collect")) { source_profiler_frame_collect();
		} else if (IS("sp_render_begin")) { source_profiler_render_begin();
		} else if (IS("sp_render_end")) { source_profiler_render_end();
		} else if (IS("sp_source")) { /* sp_source <i> <async 0|1> */
			srcs[atoi(sl.argv[1])].info.output_flags = atoi(sl.argv[2]) ? OBS_SOURCE_ASYNC_VIDEO : 0;
		} else if (IS("sp_tick")) { /* sp_tick <i> <ns> */
			uint64_t st = source_profiler_source_tick_start();
			adv(u64(sl.argv[2]));
			source_profiler_source_tick_end(&srcs[atoi(sl.argv[1])], st);
			printf("tick_start_was_zero=%d\n", st == 0);
		} else if (IS("sp_render")) { /* sp_render <i> <cpu-ns> [timer-ticks] */
			if (sl.argc > 3) ur_gfx_cfg(u64(sl.argv[3]), 0, 1000000000ULL, 1);
			gs_timer_t *t = (gs_timer_t *)0x1;
			uint64_t st = source_profiler_source_render_begin(&t);
			adv(u64(sl.argv[2]));
			source_profiler_source_render_end(&srcs[atoi(sl.argv[1])], st, t);
			printf("render timer=%s\n", t ? "yes" : "none");
		} else if (IS("sp_gfx_cfg")) { /* sp_gfx_cfg <ticks> <disjoint> <freq> <data_ok> */
			ur_gfx_cfg(u64(sl.argv[1]), atoi(sl.argv[2]), u64(sl.argv[3]), atoi(sl.argv[4]));
		} else if (IS("sp_async_ts")) { ur_set_async_ts(u64(sl.argv[1]));
		} else if (IS("sp_async_recv")) { /* sp_async_recv <i> : frame received at the current clock */
			source_profiler_async_frame_received(&srcs[atoi(sl.argv[1])]);
		} else if (IS("sp_remove")) { source_profiler_remove_source(&srcs[atoi(sl.argv[1])]); printf("tasks_queued=%d\n", ur_tasks_queued());
		} else if (IS("sp_run_tasks")) { printf("tasks_run=%d\n", ur_run_tasks());
		} else if (IS("sp_result")) { /* fill_result into a 0xff-filled struct */
			profiler_result_t r;
			memset(&r, 0xff, sizeof(r));
			bool ok = source_profiler_fill_result(&srcs[atoi(sl.argv[1])], &r);
			printf("fill=%d\n", ok);
			if (ok) print_result(&r);
			else printf("untouched=%d\n", r.tick_avg == UINT64_MAX);
		} else if (IS("sp_get")) {
			profiler_result_t *r = source_profiler_get_result(&srcs[atoi(sl.argv[1])]);
			printf("get=%s\n", r ? "result" : "null");
			if (r) { print_result(r); bfree(r); }
		} else if (IS("sp_fill_null")) { printf("fill_null=%d\n", source_profiler_fill_result(&srcs[0], NULL));
		} else if (IS("sp_gfx")) {
			printf("gfx timers create=%d destroy=%d begin=%d end=%d ranges create=%d destroy=%d begin=%d end=%d ctx_unbalanced=%d\n", ur_gfx.timer_create,
			       ur_gfx.timer_destroy, ur_gfx.timer_begin, ur_gfx.timer_end, ur_gfx.range_create, ur_gfx.range_destroy,
			       ur_gfx.range_begin, ur_gfx.range_end, ur_gfx_unbalanced());
		/* ---------------- pulse */
		} else if (IS("pa_server")) { /* pa_server <name> <version> <default_sink|-> */
			snprintf(ur_pa.server_name, sizeof(ur_pa.server_name), "%s", sl.argv[1]);
			snprintf(ur_pa.server_version, sizeof(ur_pa.server_version), "%s", sl.argv[2]);
			ur_pa.default_sink_null = !dash(sl.argv[3]);
			snprintf(ur_pa.default_sink, sizeof(ur_pa.default_sink), "%s", ur_pa.default_sink_null ? "" : sl.argv[3]);
		} else if (IS("pa_sink")) { /* pa_sink <name> <desc> <fmt> <rate> <channels> */
			script_need(&sl, 6);
			struct ur_pa_sink *s = &ur_pa.sinks[ur_pa.nsinks++];
			snprintf(s->name, sizeof(s->name), "%s", sl.argv[1]);
			snprintf(s->desc, sizeof(s->desc), "%s", sl.argv[2]);
			s->format = !strcmp(sl.argv[3], "u8") ? PA_SAMPLE_U8 : !strcmp(sl.argv[3], "s16") ? PA_SAMPLE_S16LE :
				    !strcmp(sl.argv[3], "s32") ? PA_SAMPLE_S32LE : !strcmp(sl.argv[3], "f32") ? PA_SAMPLE_FLOAT32LE :
				    !strcmp(sl.argv[3], "s24") ? PA_SAMPLE_S24LE : PA_SAMPLE_ALAW;
			s->rate = atoi(sl.argv[4]); s->channels = atoi(sl.argv[5]);
		} else if (IS("pa_cfg")) { /* pa_cfg <key> <value> */
			script_need(&sl, 3);
			const char *k = sl.argv[1]; long v = atol(sl.argv[2]);
			if (!strcmp(k, "connect_fails")) ur_pa.connect_fails = v;
			else if (!strcmp(k, "ops_fail")) ur_pa.ops_fail = v;
			else if (!strcmp(k, "stream_new_fails")) ur_pa.stream_new_fails = v;
			else if (!strcmp(k, "connect_ret")) ur_pa.connect_ret = (int)v;
			else if (!strcmp(k, "begin_size")) ur_pa.begin_size = (size_t)v;
			else if (!strcmp(k, "begin_ret")) ur_pa.begin_ret = (int)v;
			else fatal(&sl, "bad pa_cfg key");
		} else if (IS("pa_init")) { printf("pa_init=%d\n", (int)pulseaudio_init());
		} else if (IS("pa_unref")) { pulseaudio_unref();
		} else if (IS("pa_stats")) {
			printf("pa loops created=%d freed=%d contexts created=%d freed=%d null_mainloop_calls=%d\n", ur_pa_loops_created, ur_pa_loops_freed,
			       ur_pa_contexts_created, ur_pa_contexts_freed, ur_pa_null_mainloop_calls);
		} else if (IS("pa_enum")) { /* pa_enum [stop_after] */
			enum_count = 0; enum_stop_after = sl.argc > 1 ? atoi(sl.argv[1]) : 0;
			obs_enum_audio_monitoring_devices(enum_cb, NULL);
			printf("enum done count=%d\n", enum_count);
		} else if (IS("pa_available")) { printf("available=%d\n", obs_audio_monitoring_available());
		} else if (IS("pa_default_id")) {
			char *id = NULL; get_default_id(&id);
			printf("default_id="); script_print_quoted(id); putchar('\n'); bfree(id);
		} else if (IS("pa_match")) { /* pa_match <id1|-> <id2|-> */
			printf("match=%d\n", devices_match(dash(sl.argv[1]), dash(sl.argv[2])));
		} else if (IS("mon_dev")) { /* mon_dev <id|-> : obs->audio.monitoring_device_id */
			bfree(core.audio.monitoring_device_id);
			core.audio.monitoring_device_id = dash(sl.argv[1]) ? bstrdup(sl.argv[1]) : NULL;
		} else if (IS("mon_src")) { /* mon_src <i> <flags-int> <volume> <setting-device_id|-> <activate_refs> */
			int i = atoi(sl.argv[1]);
			srcs[i].info.output_flags = (uint32_t)atol(sl.argv[2]);
			srcs[i].user_volume = (float)atof(sl.argv[3]);
			ur_dev_setting = dash(sl.argv[4]);
			srcs[i].activate_refs = atol(sl.argv[5]);
		} else if (IS("ao")) { /* ao <rate> <speakers-int> : the engine's output format */
			ur_set_ao((uint32_t)atol(sl.argv[1]), (enum speaker_layout)atoi(sl.argv[2]));
		} else if (IS("resampler_fail")) { ur_resampler_fail(atoi(sl.argv[1]));
		} else if (IS("mon_create")) { /* mon_create <slot> <src> */
			int s = atoi(sl.argv[1]);
			mon_src[s] = &srcs[atoi(sl.argv[2])];
			mons[s] = audio_monitor_create(mon_src[s]);
			printf("mon_create %d %s monitors_registered=%zu cb=%d adds=%d removes=%d\n", s, mons[s] ? "ok" : "null", core.audio.monitors.num,
			       ur_have_cb(), ur_cap_adds, ur_cap_removes);
		} else if (IS("mon_reset")) { audio_monitor_reset(mons[atoi(sl.argv[1])]); printf("mon_reset cb=%d adds=%d removes=%d\n", ur_have_cb(), ur_cap_adds, ur_cap_removes);
		} else if (IS("mon_destroy")) {
			int s = atoi(sl.argv[1]);
			audio_monitor_destroy(mons[s]); mons[s] = NULL;
			printf("mon_destroy monitors_registered=%zu\n", core.audio.monitors.num);
		} else if (IS("mon_destroy_null")) { audio_monitor_destroy(NULL); puts("mon_destroy_null ok");
		} else if (IS("mon_play")) { /* mon_play <slot> <frames> <first-sample> <step> : planar float, L=v R=-v */
			int s = atoi(sl.argv[1]);
			uint32_t frames = (uint32_t)atol(sl.argv[2]);
			float *l = bmalloc(frames * 4 + 4), *r = bmalloc(frames * 4 + 4);
			float v = (float)atof(sl.argv[3]), st = (float)atof(sl.argv[4]);
			for (uint32_t i = 0; i < frames; i++, v += st) { l[i] = v; r[i] = -v; }
			struct audio_data ad = {.frames = frames};
			ad.data[0] = (uint8_t *)l; ad.data[1] = (uint8_t *)r;
			ur_feed(mon_src[s], &ad);
			bfree(l); bfree(r);
		/* ---------------- dbus */
		} else if (IS("dbus_bus")) { /* dbus_bus none|private */
			if (!strcmp(sl.argv[1], "none")) setenv("DBUS_SESSION_BUS_ADDRESS", "unix:path=/nonexistent/obs-test-bus", 1);
			else bus_start();
		} else if (IS("dbus_service")) { /* dbus_service ss|pm|mate|gnome|portal ... : own these names (before dbus_serve) */
			for (int i = 1; i < sl.argc; i++) {
				const char *n = !strcmp(sl.argv[i], "ss") ? "org.freedesktop.ScreenSaver" :
						!strcmp(sl.argv[i], "pm") ? "org.freedesktop.PowerManagement.Inhibit" :
						!strcmp(sl.argv[i], "mate") ? "org.mate.SessionManager" :
						!strcmp(sl.argv[i], "gnome") ? "org.gnome.SessionManager" : "org.freedesktop.portal.Desktop";
				snprintf(svc_names[svc_nnames++], 64, "%s", n);
			}
			bus_start();
			svc_start_thread();
		} else if (IS("dbus_delay")) { svc_delay_ms = atoi(sl.argv[1]);
		} else if (IS("dbus_fail")) { g_hash_table_add(svc_fail, g_strdup(sl.argv[1]));
		} else if (IS("dbus_ok")) { g_hash_table_remove(svc_fail, sl.argv[1]);
		} else if (IS("dsi_create")) { int s = atoi(sl.argv[1]); dsi[s] = dbus_sleep_info_create(); printf("dsi_create %d %s\n", s, dsi[s] ? "ok" : "null");
		} else if (IS("dsi_inhibit")) { dbus_inhibit_sleep(dsi[atoi(sl.argv[1])], sl.argv[2], atoi(sl.argv[3])); printf("dsi_inhibit done\n");
		} else if (IS("dsi_destroy")) { dbus_sleep_info_destroy(dsi[atoi(sl.argv[1])]); dsi[atoi(sl.argv[1])] = NULL; puts("dsi_destroy done");
		} else if (IS("dsi_destroy_null")) { dbus_sleep_info_destroy(NULL); puts("dsi_destroy_null ok");
		} else if (IS("pii_create")) { int s = atoi(sl.argv[1]); pii[s] = portal_inhibit_info_create(); printf("pii_create %d %s\n", s, pii[s] ? "ok" : "null");
		} else if (IS("pii_inhibit")) { portal_inhibit(pii[atoi(sl.argv[1])], sl.argv[2], atoi(sl.argv[3])); printf("pii_inhibit done\n");
		} else if (IS("pii_destroy")) { portal_inhibit_info_destroy(pii[atoi(sl.argv[1])]); pii[atoi(sl.argv[1])] = NULL; puts("pii_destroy done");
		} else if (IS("pii_destroy_null")) { portal_inhibit_info_destroy(NULL); puts("pii_destroy_null ok");
		} else if (IS("dbus_wait")) { /* dbus_wait <n> : pump until the service has seen n NEW calls since the last dbus_wait / dbus_log (max 5 s), then 150 ms more */
			for (int i = 0; i < 5000 && svc_count() < (guint)atoi(sl.argv[1]); i++) pump(1);
			pump(150);
			g_mutex_lock(&svc_mu); svc_base = svc_total; g_mutex_unlock(&svc_mu);
		} else if (IS("dbus_response")) { /* dbus_response <code> : the portal answers the last request */
			GVariantBuilder b; g_variant_builder_init(&b, G_VARIANT_TYPE_VARDICT);
			g_dbus_connection_emit_signal(svc_conn, last_sender, last_portal_path, "org.freedesktop.portal.Request", "Response",
						      g_variant_new("(u@a{sv})", (guint32)atoi(sl.argv[1]), g_variant_builder_end(&b)), NULL);
			pump(200);
		} else if (IS("dbus_log")) {
			g_mutex_lock(&svc_mu);
			for (guint i = 0; i < svc_log->len; i++) printf("svc %s\n", (char *)g_ptr_array_index(svc_log, i));
			g_ptr_array_set_size(svc_log, 0);
			svc_base = svc_total;
			g_mutex_unlock(&svc_mu);
		} else {
			fatal(&sl, "unknown command");
		}
	}
	/* cleanup */
	if (args_h) os_process_args_destroy(args_h);
	for (int i = 0; i < NPIPE; i++) if (pipes[i]) os_process_pipe_destroy(pipes[i]);
	if (bus_pid) { kill(bus_pid, SIGTERM); if (bus_fp) pclose(bus_fp); }
	fflush(stdout);
	_exit(0);
}
