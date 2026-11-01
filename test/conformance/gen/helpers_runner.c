/*
 * Conformance runner for the small helper modules (spec S-024, area "helpers").
 *
 * One runner, one script language; the first word of a command selects the module:
 *
 *   opts_*   shared/opts-parser          u128_* / mul_div64   libobs/util/util_uint{64,128}.h
 *   args_*   libobs/util/pipe.c (os_process_args_*)
 *   hk_*     libobs/obs-hotkey-name-map.c    mf_*  libobs/obs-missing-files.c
 *   task_*   libobs/util/task.c              p_*   libobs/util/profiler.c (fake clock)
 *   bpm_*    shared/bpm/bpm.c (fake counters) he_*  shared/happy-eyeballs (address ordering only)
 *   fu_*     shared/file-updater (fake curl + in-memory file system)
 *   const    constants table (obs-defs.h, obs-config.h, enums)
 *
 * Several reference files are #included (not linked) because the behaviour under test lives in
 * file-local (static) functions or private structs. Their file-local names must not clash with
 * the runner: the runner's own identifiers use the prefix hx_ / h_.
 *
 * See test/conformance/README.md ("helpers runner") for the command reference.
 */
#define _GNU_SOURCE
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdarg.h>
#define CURL_DISABLE_TYPECHECK
#include <curl/curl.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "harness.h"
#include "util/bmem.h"
#include "util/dstr.h"
#include "util/darray.h"
#include "util/platform.h"
#include "util/threading.h"
#include "util/pipe.h"
#include "util/profiler.h"
#include "util/util_uint64.h"
#include "util/util_uint128.h"

#include "obs.h"
#include "obs-internal.h"
#include "obs-missing-files.h"

#include "../../../shared/opts-parser/opts-parser.h"

/* ------------------------------------------------------------------------- */
/* helpers                                                                   */

static uint64_t h_u64(const char *s)
{
	return strtoull(s, NULL, 0);
}

static int64_t h_i64(const char *s)
{
	return strtoll(s, NULL, 0);
}

static const char *h_str(const char *s)
{
	return strcmp(s, "@null") == 0 ? NULL : s;
}

static void h_quoted_or_null(const char *s)
{
	if (!s)
		fputs("@null", stdout);
	else
		script_print_quoted(s);
}

/* Decodes a hex string ("-" = empty). Returns the malloc'ed buffer (never NULL) and its length. */
static uint8_t *h_unhex(const char *hex, size_t *len)
{
	size_t n = 0;
	uint8_t *out;
	if (strcmp(hex, "-") == 0)
		hex = "";
	out = malloc(strlen(hex) / 2 + 1);
	for (size_t i = 0; hex[i] && hex[i + 1]; i += 2)
		out[n++] = (uint8_t)(script_hexval(hex[i]) * 16 + script_hexval(hex[i + 1]));
	*len = n;
	return out;
}

/* Runs fn in a forked child; prints "crash" if the child dies from a signal. Only for
 * single-threaded situations (never after a queue thread exists). */
static void h_fork_crash_marker(void (*fn)(void *), void *arg, const char *label)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		fn(arg);
		fflush(stdout);
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFSIGNALED(st))
		printf("%s crash\n", label);
}

/* ------------------------------------------------------------------------- */
/* opts-parser                                                               */

static void h_opts_parse(int argc, char **argv)
{
	const char *in = argc > 1 ? h_str(argv[1]) : NULL;
	struct obs_options o = obs_parse_options(in);

	fputs("opts_parse ", stdout);
	h_quoted_or_null(in);
	putchar('\n');
	printf("  count=%zu ignored_count=%zu ptrs options=%d ignored=%d input=%d\n", o.count, o.ignored_word_count,
	       o.options != NULL, o.ignored_words != NULL, o.input_words != NULL);
	for (size_t i = 0; i < o.count; i++) {
		printf("  opt %zu name=", i);
		script_print_quoted(o.options[i].name);
		fputs(" value=", stdout);
		script_print_quoted(o.options[i].value);
		putchar('\n');
	}
	for (size_t i = 0; i < o.ignored_word_count; i++) {
		printf("  ignored %zu ", i);
		script_print_quoted(o.ignored_words[i]);
		putchar('\n');
	}
	if (o.input_words) {
		size_t n = 0;
		while (o.input_words[n])
			n++;
		printf("  input_words=%zu\n", n);
		for (size_t i = 0; i < n; i++) {
			printf("  word %zu ", i);
			script_print_quoted(o.input_words[i]);
			putchar('\n');
		}
	}
	obs_free_options(o);
}

/* ------------------------------------------------------------------------- */
/* uint64 / uint128                                                          */

static void h_print128(const char *label, util_uint128_t v)
{
	printf("%s high=%016" PRIx64 " low=%016" PRIx64 "\n", label, v.high, v.low);
}

static void h_u128_cmd(int argc, char **argv)
{
	const char *c = argv[0];
	if (!strcmp(c, "u128_layout")) {
		util_uint128_t v = {{{0}}};
		v.i32[0] = 1;
		v.i32[1] = 2;
		v.i32[2] = 3;
		v.i32[3] = 4;
		printf("u128_layout size=%zu low=%016" PRIx64 " high=%016" PRIx64 "\n", sizeof(v), v.low, v.high);
	} else if (!strcmp(c, "u128_add") && argc == 5) {
		util_uint128_t a = {.low = h_u64(argv[1]), .high = h_u64(argv[2])};
		util_uint128_t b = {.low = h_u64(argv[3]), .high = h_u64(argv[4])};
		h_print128("u128_add", util_add128(a, b));
	} else if (!strcmp(c, "u128_mul64") && argc == 3) {
		h_print128("u128_mul64", util_mul64_64(h_u64(argv[1]), h_u64(argv[2])));
	} else if (!strcmp(c, "u128_div32") && argc == 4) {
		util_uint128_t a = {.low = h_u64(argv[1]), .high = h_u64(argv[2])};
		h_print128("u128_div32", util_div128_32(a, (uint32_t)h_u64(argv[3])));
	} else if (!strcmp(c, "u128_shift32") && argc == 2) {
		h_print128("u128_shift32", util_lshift64_internal_32(h_u64(argv[1])));
	} else if (!strcmp(c, "u128_shift64") && argc == 2) {
		h_print128("u128_shift64", util_lshift64_internal_64(h_u64(argv[1])));
	} else if (!strcmp(c, "mul_div64") && argc == 4) {
		uint64_t n = h_u64(argv[1]), m = h_u64(argv[2]), d = h_u64(argv[3]);
		unsigned __int128 exact = ((unsigned __int128)n * m) / d;
		printf("mul_div64 ref=%" PRIu64 " exact=", util_mul_div64(n, m, d));
		if (exact >> 64)
			fputs("overflow", stdout);
		else
			printf("%" PRIu64, (uint64_t)exact);
		putchar('\n');
	} else if (!strcmp(c, "mul_div64_by_zero")) {
		/* precondition violation: division by zero (SIGFPE) */
		volatile uint64_t zero = 0;
		(void)util_mul_div64(5, 3, zero);
	}
}

static void h_div_zero_child(void *unused)
{
	(void)unused;
	volatile uint64_t zero = 0;
	util_uint128_t a = {.low = 1, .high = 0};
	(void)util_mul_div64(5, 3, zero);
	(void)util_div128_32(a, (uint32_t)zero);
}

/* ------------------------------------------------------------------------- */
/* os_process_args                                                           */

#define H_MAX_ARGS 4
static struct os_process_args *h_args[H_MAX_ARGS];

static struct os_process_args *h_args_slot(const char *s)
{
	int i = atoi(s);
	return (i >= 0 && i < H_MAX_ARGS) ? h_args[i] : NULL;
}

static void h_args_cmd(int argc, char **argv)
{
	const char *c = argv[0];
	int slot = argc > 1 ? atoi(argv[1]) : 0;
	if (slot < 0 || slot >= H_MAX_ARGS)
		return;
	if (!strcmp(c, "args_create") && argc == 3) {
		h_args[slot] = os_process_args_create(h_str(argv[2]));
	} else if (!strcmp(c, "args_add") && argc == 3) {
		os_process_args_add_arg(h_args[slot], h_str(argv[2]));
	} else if (!strcmp(c, "args_addf_str") && argc == 4) {
		/* format with one %s */
		os_process_args_add_argf(h_args[slot], argv[2], argv[3]);
	} else if (!strcmp(c, "args_addf_int") && argc == 4) {
		/* format with one %d */
		os_process_args_add_argf(h_args[slot], argv[2], (int)h_i64(argv[3]));
	} else if (!strcmp(c, "args_addf_plain") && argc == 3) {
		os_process_args_add_argf(h_args[slot], "%s", argv[2]);
	} else if (!strcmp(c, "args_dump")) {
		struct os_process_args *a = h_args[slot];
		size_t n = os_process_args_get_argc(a);
		char **v = os_process_args_get_argv(a);
		printf("args_dump argc=%zu\n", n);
		for (size_t i = 0; i <= n; i++) {
			printf("  argv[%zu]=", i);
			h_quoted_or_null(v[i]);
			putchar('\n');
		}
	} else if (!strcmp(c, "args_destroy")) {
		os_process_args_destroy(h_args[slot]);
		h_args[slot] = NULL;
	} else if (!strcmp(c, "args_destroy_null")) {
		os_process_args_destroy(NULL);
		puts("args_destroy_null ok");
	}
}

/* ------------------------------------------------------------------------- */
/* hotkey name map                                                           */

struct obs_core *obs = NULL;
const char *obs_key_to_name(obs_key_t key);
obs_key_t obs_key_from_name(const char *name);
void obs_hotkey_name_map_free(void);

static const char *const h_hk_names[] = {
#define OBS_HOTKEY(x) #x,
#include "obs-hotkeys.h"
#undef OBS_HOTKEY
};

static void h_hk_from_child(void *name)
{
	printf("hk_from %d\n", (int)obs_key_from_name(name));
}

static void h_hk_cmd(int argc, char **argv)
{
	const char *c = argv[0];
	if (!strcmp(c, "hk_mode") && argc == 2) {
		if (!strcmp(argv[1], "null")) {
			obs = NULL;
		} else {
			if (!obs)
				obs = bzalloc(sizeof(*obs));
		}
		printf("hk_mode %s\n", argv[1]);
	} else if (!strcmp(c, "hk_count")) {
		printf("hk_count %d\n", (int)OBS_KEY_LAST_VALUE);
	} else if (!strcmp(c, "hk_name") && argc == 2) {
		int64_t k = h_i64(argv[1]);
		const char *nm = obs_key_to_name((obs_key_t)k);
		printf("hk_name %" PRId64 " ", k);
		script_print_quoted(nm);
		putchar('\n');
	} else if (!strcmp(c, "hk_from") && argc == 2) {
		const char *n = h_str(argv[1]);
		if (!n && !obs) {
			/* NULL name in fallback mode: reference crashes (DEFECT) */
			h_fork_crash_marker(h_hk_from_child, NULL, "hk_from @null");
			return;
		}
		int id = (int)obs_key_from_name(n);
		printf("hk_from ");
		h_quoted_or_null(n);
		printf(" %d\n", id);
	} else if (!strcmp(c, "hk_dump")) {
		/* complete table: index, name, and the reverse lookup of that name */
		int bad = 0;
		for (int i = 0; i < (int)OBS_KEY_LAST_VALUE; i++) {
			const char *n = obs_key_to_name((obs_key_t)i);
			int back = (int)obs_key_from_name(n);
			printf("%d %s %d\n", i, n, back);
			if (back != i)
				bad++;
			if (strcmp(n, h_hk_names[i]))
				bad++;
		}
		printf("hk_dump total=%d mismatches=%d\n", (int)OBS_KEY_LAST_VALUE, bad);
	} else if (!strcmp(c, "hk_free")) {
		obs_hotkey_name_map_free();
		puts("hk_free done");
		if (obs) {
			bfree(obs);
			obs = NULL;
		}
	}
}

/* ------------------------------------------------------------------------- */
/* missing files                                                             */

#include "../../../libobs/obs-missing-files.c"

struct h_fake_source {
	const char *name;
};

const char *obs_source_get_name(const obs_source_t *source)
{
	return source ? ((const struct h_fake_source *)source)->name : NULL;
}

static void h_mf_cb(void *src, const char *new_path, void *data)
{
	printf("  callback src=%s new_path=", src ? (const char *)src : "@null");
	h_quoted_or_null(new_path);
	printf(" data=%s\n", data ? (const char *)data : "@null");
}

void obs_source_replace_missing_file(obs_missing_file_cb cb, obs_source_t *source, const char *new_path, void *data)
{
	/* real function: ignored if source is NULL, else cb(source->context.data, ...); the runner
	 * models context.data as the source's name */
	if (!source)
		return;
	cb((void *)((struct h_fake_source *)source)->name, new_path, data);
}

#define H_MAX_MF 8
static obs_missing_files_t *h_lists[H_MAX_MF];
static obs_missing_file_t *h_files[H_MAX_MF];
static struct h_fake_source h_srcs[H_MAX_MF];
static char h_src_names[H_MAX_MF][64];

static void h_mf_cmd(int argc, char **argv)
{
	const char *c = argv[0];
	int a = argc > 1 ? atoi(argv[1]) : 0;
	if (a < 0 || a >= H_MAX_MF)
		return;
	if (!strcmp(c, "mf_list_new")) {
		h_lists[a] = obs_missing_files_create();
	} else if (!strcmp(c, "mf_file_new") && argc >= 4) {
		/* mf_file_new F PATH source|script [SRCNAME] */
		int type = !strcmp(argv[3], "source") ? OBS_MISSING_FILE_SOURCE : OBS_MISSING_FILE_SCRIPT;
		void *src = NULL;
		if (argc >= 5) {
			snprintf(h_src_names[a], sizeof(h_src_names[a]), "%s", argv[4]);
			h_srcs[a].name = h_src_names[a];
			src = &h_srcs[a];
		}
		h_files[a] = obs_missing_file_create(argv[2], h_mf_cb, type, src, (void *)"userdata");
	} else if (!strcmp(c, "mf_add") && argc == 3) {
		obs_missing_files_add_file(h_lists[a], h_files[atoi(argv[2])]);
	} else if (!strcmp(c, "mf_count")) {
		printf("mf_count %d %zu\n", a, obs_missing_files_count(h_lists[a]));
	} else if (!strcmp(c, "mf_get") && argc == 3) {
		int idx = atoi(argv[2]);
		obs_missing_file_t *f = obs_missing_files_get_file(h_lists[a], idx);
		printf("mf_get %d %d path=", a, idx);
		h_quoted_or_null(obs_missing_file_get_path(f));
		fputs(" source_name=", stdout);
		h_quoted_or_null(obs_missing_file_get_source_name(f));
		printf(" ref=%ld same_as_file=", (long)f->ref);
		int same = -1;
		for (int i = 0; i < H_MAX_MF; i++)
			if (h_files[i] == f)
				same = i;
		printf("%d\n", same);
	} else if (!strcmp(c, "mf_append") && argc == 3) {
		obs_missing_files_append(h_lists[a], h_lists[atoi(argv[2])]);
	} else if (!strcmp(c, "mf_ref")) {
		printf("mf_ref %d %ld\n", a, (long)h_files[a]->ref);
	} else if (!strcmp(c, "mf_issue") && argc == 3) {
		printf("mf_issue %d\n", a);
		obs_missing_file_issue_callback(h_files[a], h_str(argv[2]));
	} else if (!strcmp(c, "mf_release")) {
		obs_missing_file_release(h_files[a]);
		printf("mf_release %d\n", a);
	} else if (!strcmp(c, "mf_release_null")) {
		obs_missing_file_release(NULL);
		puts("mf_release_null ok");
	} else if (!strcmp(c, "mf_destroy")) {
		obs_missing_files_destroy(h_lists[a]);
		h_lists[a] = NULL;
		printf("mf_destroy %d\n", a);
	} else if (!strcmp(c, "mf_destroy_file")) {
		/* force-destroy irrespective of the reference count */
		obs_missing_file_destroy(h_files[a]);
		h_files[a] = NULL;
		printf("mf_destroy_file %d\n", a);
	}
}

/* ------------------------------------------------------------------------- */
/* logging: like harness.h but masks pointers and can capture output as data */

static int h_capture;

static void h_log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096], masked[4096];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	size_t o = 0;
	for (const char *p = msg; *p && o < sizeof(masked) - 8; p++) {
		if (p[0] == '0' && p[1] == 'x' && script_hexval(p[2]) >= 0) {
			p += 2;
			while (script_hexval(p[1]) >= 0)
				p++;
			memcpy(masked + o, "0xPTR", 5);
			o += 5;
		} else {
			masked[o++] = *p;
		}
	}
	masked[o] = 0;
	if (h_capture) {
		fputs("text ", stdout);
		script_print_quoted(masked);
		putchar('\n');
		return;
	}
	for (char *p = masked; *p; p++)
		if (*p == '\n')
			*p = ' ';
	const char *name = level == LOG_ERROR     ? "error"
			   : level == LOG_WARNING ? "warning"
			   : level == LOG_INFO    ? "info"
						  : "debug";
	printf("log %s %s\n", name, masked);
}

/* ------------------------------------------------------------------------- */
/* task queue                                                                */

#include "../../../libobs/util/task.c"

static pthread_mutex_t h_task_log_mutex = PTHREAD_MUTEX_INITIALIZER;
static char h_task_log[8192];
static size_t h_task_log_len;
static os_event_t *h_gate_entered;
static os_event_t *h_gate_release;
#define H_MAX_TQ 4
static os_task_queue_t *h_tq[H_MAX_TQ];

static void h_task_append(const char *fmt, ...)
{
	va_list va;
	pthread_mutex_lock(&h_task_log_mutex);
	va_start(va, fmt);
	h_task_log_len += (size_t)vsnprintf(h_task_log + h_task_log_len, sizeof(h_task_log) - h_task_log_len, fmt, va);
	va_end(va);
	pthread_mutex_unlock(&h_task_log_mutex);
}

struct h_task_arg {
	int q;
	int target;
	char label[32];
	char label2[32];
};

static void h_task_run_log(void *p)
{
	struct h_task_arg *a = p;
	h_task_append("run %s\n", a->label);
	bfree(a);
}

static void h_task_run_gate(void *p)
{
	(void)p;
	h_task_append("run gate-enter\n");
	os_event_signal(h_gate_entered);
	os_event_wait(h_gate_release);
	h_task_append("run gate-exit\n");
}

static void h_task_run_inside(void *p)
{
	struct h_task_arg *a = p;
	h_task_append("run %s inside(q%d)=%d\n", a->label, a->target, os_task_queue_inside(h_tq[a->target]) ? 1 : 0);
	bfree(a);
}

static void h_task_run_requeue(void *p)
{
	struct h_task_arg *a = p;
	h_task_append("run %s queues %s\n", a->label, a->label2);
	struct h_task_arg *n = bzalloc(sizeof(*n));
	snprintf(n->label, sizeof(n->label), "%s", a->label2);
	bool ok = os_task_queue_queue_task(h_tq[a->q], h_task_run_log, n);
	h_task_append("run %s queue_task=%d\n", a->label, ok ? 1 : 0);
	bfree(a);
}

struct h_helper {
	pthread_t th;
	os_task_queue_t *tq;
	int mode; /* 0 = wait for tq->waiting, 1 = wait for tasks.size >= n */
	size_t n;
};

static void *h_helper_thread(void *p)
{
	struct h_helper *h = p;
	for (;;) {
		bool done;
		pthread_mutex_lock(&h->tq->mutex);
		done = h->mode == 0 ? h->tq->waiting : h->tq->tasks.size >= h->n * sizeof(struct os_task_info);
		pthread_mutex_unlock(&h->tq->mutex);
		if (done)
			break;
		usleep(200);
	}
	os_event_signal(h_gate_release);
	return NULL;
}

static void h_task_print_log(void)
{
	pthread_mutex_lock(&h_task_log_mutex);
	char *s = h_task_log;
	while (*s) {
		char *nl = strchr(s, '\n');
		if (nl)
			*nl = 0;
		printf("  %s\n", s);
		if (!nl)
			break;
		s = nl + 1;
	}
	h_task_log[0] = 0;
	h_task_log_len = 0;
	pthread_mutex_unlock(&h_task_log_mutex);
}

static void h_task_cmd(int argc, char **argv)
{
	const char *c = argv[0];
	int q = argc > 1 ? atoi(argv[1]) : 0;
	if (q < 0 || q >= H_MAX_TQ)
		return;
	if (!h_gate_entered) {
		os_event_init(&h_gate_entered, OS_EVENT_TYPE_AUTO);
		os_event_init(&h_gate_release, OS_EVENT_TYPE_AUTO);
	}
	if (!strcmp(c, "task_create")) {
		h_tq[q] = os_task_queue_create();
		printf("task_create %d %s\n", q, h_tq[q] ? "ok" : "fail");
	} else if (!strcmp(c, "task_queue") && argc == 3) {
		struct h_task_arg *a = bzalloc(sizeof(*a));
		snprintf(a->label, sizeof(a->label), "%s", argv[2]);
		printf("task_queue %d %s -> %d\n", q, argv[2], os_task_queue_queue_task(h_tq[q], h_task_run_log, a));
	} else if (!strcmp(c, "task_gate")) {
		os_task_queue_queue_task(h_tq[q], h_task_run_gate, NULL);
		os_event_wait(h_gate_entered);
		printf("task_gate %d entered\n", q);
	} else if (!strcmp(c, "task_inside_q") && argc == 4) {
		struct h_task_arg *a = bzalloc(sizeof(*a));
		snprintf(a->label, sizeof(a->label), "%s", argv[2]);
		a->target = atoi(argv[3]);
		os_task_queue_queue_task(h_tq[q], h_task_run_inside, a);
		printf("task_inside_q %d %s queued\n", q, argv[2]);
	} else if (!strcmp(c, "task_requeue") && argc == 4) {
		struct h_task_arg *a = bzalloc(sizeof(*a));
		a->q = q;
		snprintf(a->label, sizeof(a->label), "%s", argv[2]);
		snprintf(a->label2, sizeof(a->label2), "%s", argv[3]);
		os_task_queue_queue_task(h_tq[q], h_task_run_requeue, a);
		printf("task_requeue %d %s queued\n", q, argv[2]);
	} else if (!strcmp(c, "task_main_inside")) {
		printf("task_main_inside %d %d\n", q, os_task_queue_inside(h_tq[q]) ? 1 : 0);
	} else if (!strcmp(c, "task_wait")) {
		bool r = os_task_queue_wait(h_tq[q]);
		printf("task_wait %d -> %d\n", q, r ? 1 : 0);
	} else if (!strcmp(c, "task_sync")) {
		/* wait() without printing its (timing dependent) result */
		os_task_queue_wait(h_tq[q]);
		printf("task_sync %d\n", q);
	} else if (!strcmp(c, "task_wait_release")) {
		struct h_helper h = {.tq = h_tq[q], .mode = 0};
		pthread_create(&h.th, NULL, h_helper_thread, &h);
		bool r = os_task_queue_wait(h_tq[q]);
		pthread_join(h.th, NULL);
		printf("task_wait_release %d -> %d\n", q, r ? 1 : 0);
	} else if (!strcmp(c, "task_destroy")) {
		os_task_queue_destroy(h_tq[q]);
		h_tq[q] = NULL;
		printf("task_destroy %d done\n", q);
	} else if (!strcmp(c, "task_destroy_release") && argc == 3) {
		/* helper opens the gate once N tasks (not counting the running gate) are pending, i.e.
		 * once destroy() has queued its stop task */
		struct h_helper h = {.tq = h_tq[q], .mode = 1, .n = (size_t)atoi(argv[2])};
		pthread_create(&h.th, NULL, h_helper_thread, &h);
		os_task_queue_destroy(h_tq[q]);
		pthread_join(h.th, NULL);
		h_tq[q] = NULL;
		printf("task_destroy_release %d done\n", q);
	} else if (!strcmp(c, "task_log")) {
		printf("task_log\n");
		h_task_print_log();
	} else if (!strcmp(c, "task_null")) {
		struct h_task_arg a = {0};
		printf("task_null queue=%d wait=%d\n", os_task_queue_queue_task(NULL, h_task_run_log, &a),
		       os_task_queue_wait(NULL));
		os_task_queue_destroy(NULL);
		puts("task_null destroy ok");
	}
}

/* ------------------------------------------------------------------------- */
/* profiler (fake clock)                                                     */

uint64_t h_clock_ns;
uint64_t os_gettime_ns(void)
{
	return h_clock_ns;
}

#define H_MAX_NAMES 16
static const char *h_pnames[H_MAX_NAMES];
static profiler_name_store_t *h_store;
static profiler_snapshot_t *h_snap;

static const char *h_pname(const char *id)
{
	int i = atoi(id);
	return (i >= 0 && i < H_MAX_NAMES) ? h_pnames[i] : NULL;
}

static void h_dump_times(const char *label, profiler_time_entries_t *t)
{
	printf("%s", label);
	for (size_t i = 0; i < t->num; i++)
		printf(" %" PRIu64 ":%" PRIu64, t->array[i].time_delta, t->array[i].count);
	putchar('\n');
}

static bool h_dump_entry(void *ctx, profiler_snapshot_entry_t *e);
static bool h_dump_entry(void *ctx, profiler_snapshot_entry_t *e)
{
	int depth = *(int *)ctx;
	printf("%*sentry name=", depth * 2, "");
	script_print_quoted(profiler_snapshot_entry_name(e));
	printf(" count=%" PRIu64 " min=%" PRIu64 " max=%" PRIu64 " expected=%" PRIu64 " bcount=%" PRIu64
	       " bmin=%" PRIu64 " bmax=%" PRIu64 " children=%zu\n",
	       profiler_snapshot_entry_overall_count(e), profiler_snapshot_entry_min_time(e),
	       profiler_snapshot_entry_max_time(e), profiler_snapshot_entry_expected_time_between_calls(e),
	       profiler_snapshot_entry_overall_between_calls_count(e),
	       profiler_snapshot_entry_min_time_between_calls(e), profiler_snapshot_entry_max_time_between_calls(e),
	       profiler_snapshot_num_children(e));
	printf("%*s", depth * 2 + 2, "");
	h_dump_times("times", profiler_snapshot_entry_times(e));
	printf("%*s", depth * 2 + 2, "");
	h_dump_times("between", profiler_snapshot_entry_times_between_calls(e));
	int d = depth + 1;
	profiler_snapshot_enumerate_children(e, h_dump_entry, &d);
	return true;
}

static bool h_filter_prefix(void *data, const char *name, bool *remove)
{
	const char *pre = data;
	*remove = strncmp(name, pre, strlen(pre)) == 0;
	/* stop enumerating after the entry named "stop" */
	return strcmp(name, "stop") != 0;
}

/* normalises pointers in the csv dump to ordinals */
static void h_csv_print(const char *path)
{
	FILE *f = fopen(path, "rb");
	char line[1024];
	char seen[64][32];
	int nseen = 0;
	if (!f) {
		puts("csv open failed");
		return;
	}
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		char out[1024];
		size_t o = 0;
		int field = 0;
		bool header = strncmp(line, "id,", 3) == 0;
		while (*p) {
			char *comma = strchr(p, ',');
			char *end = comma ? comma : p + strlen(p);
			size_t n = (size_t)(end - p);
			if (!header && field < 4) {
				char key[32] = {0};
				memcpy(key, p, n < 31 ? n : 31);
				int idx = -1;
				if (!strcmp(key, "(nil)")) {
					o += (size_t)snprintf(out + o, sizeof(out) - o, "nil");
				} else {
					for (int i = 0; i < nseen; i++)
						if (!strcmp(seen[i], key))
							idx = i;
					if (idx < 0) {
						idx = nseen;
						memcpy(seen[nseen++], key, 32);
					}
					o += (size_t)snprintf(out + o, sizeof(out) - o, "#%d", idx);
				}
			} else {
				memcpy(out + o, p, n);
				o += n;
			}
			field++;
			if (!comma) {
				break;
			}
			out[o++] = ',';
			p = comma + 1;
		}
		out[o] = 0;
		printf("csv %s", out);
		if (!strchr(out, '\n'))
			putchar('\n');
	}
	fclose(f);
}

static void h_prof_cmd(int argc, char **argv)
{
	const char *c = argv[0];
	if (!h_store)
		h_store = profiler_name_store_create();
	if (!strcmp(c, "p_clock") && argc == 2) {
		h_clock_ns = h_u64(argv[1]);
	} else if (!strcmp(c, "p_tick") && argc == 2) {
		h_clock_ns += h_u64(argv[1]);
	} else if (!strcmp(c, "p_name") && argc == 3) {
		/* every call produces a distinct pointer, even for equal text */
		int i = atoi(argv[1]);
		if (i >= 0 && i < H_MAX_NAMES)
			h_pnames[i] = profile_store_name(h_store, "%s", argv[2]);
	} else if (!strcmp(c, "p_name_fmt") && argc == 4) {
		int i = atoi(argv[1]);
		if (i >= 0 && i < H_MAX_NAMES)
			h_pnames[i] = profile_store_name(h_store, "%s-%d", argv[2], atoi(argv[3]));
	} else if (!strcmp(c, "p_begin")) {
		profiler_start();
	} else if (!strcmp(c, "p_stop")) {
		profiler_stop();
	} else if (!strcmp(c, "p_reenable")) {
		profile_reenable_thread();
	} else if (!strcmp(c, "p_root") && argc == 3) {
		profile_register_root(h_pname(argv[1]), h_u64(argv[2]));
	} else if (!strcmp(c, "p_start") && argc == 2) {
		profile_start(h_pname(argv[1]));
	} else if (!strcmp(c, "p_end") && argc == 2) {
		profile_end(h_pname(argv[1]));
	} else if (!strcmp(c, "p_call") && argc == 3) {
		/* start ID, advance clock by NS, end ID */
		profile_start(h_pname(argv[1]));
		h_clock_ns += h_u64(argv[2]);
		profile_end(h_pname(argv[1]));
	} else if (!strcmp(c, "p_burst") && argc == 6) {
		/* p_burst ID COUNT DUR0 DURSTEP GAP: COUNT calls of duration DUR0 + i*DURSTEP ns,
		 * GAP ns between the end of one and the start of the next */
		uint64_t n = h_u64(argv[2]), d0 = h_u64(argv[3]), ds = h_u64(argv[4]), gap = h_u64(argv[5]);
		for (uint64_t i = 0; i < n; i++) {
			profile_start(h_pname(argv[1]));
			h_clock_ns += d0 + i * ds;
			profile_end(h_pname(argv[1]));
			h_clock_ns += gap;
		}
	} else if (!strcmp(c, "p_snap")) {
		if (h_snap)
			profile_snapshot_free(h_snap);
		h_snap = profile_snapshot_create();
		printf("p_snap roots=%zu\n", profiler_snapshot_num_roots(h_snap));
	} else if (!strcmp(c, "p_dump")) {
		printf("p_dump roots=%zu\n", profiler_snapshot_num_roots(h_snap));
		int depth = 1;
		profiler_snapshot_enumerate_roots(h_snap, h_dump_entry, &depth);
	} else if (!strcmp(c, "p_filter") && argc == 2) {
		profiler_snapshot_filter_roots(h_snap, h_filter_prefix, argv[1]);
		printf("p_filter roots=%zu\n", profiler_snapshot_num_roots(h_snap));
	} else if (!strcmp(c, "p_print") || !strcmp(c, "p_print_between")) {
		int use_snap = argc > 1 && !strcmp(argv[1], "snap");
		printf("%s\n", c);
		h_capture = 1;
		if (!strcmp(c, "p_print"))
			profiler_print(use_snap ? h_snap : NULL);
		else
			profiler_print_time_between_calls(use_snap ? h_snap : NULL);
		h_capture = 0;
	} else if (!strcmp(c, "p_csv")) {
		const char *dir = getenv("CONFORMANCE_TMPDIR");
		char path[1024];
		snprintf(path, sizeof(path), "%s/helpers-prof-%d.csv", dir ? dir : ".", (int)getpid());
		printf("p_csv %d\n", profiler_snapshot_dump_csv(h_snap, path));
		h_csv_print(path);
		remove(path);
	} else if (!strcmp(c, "p_snap_free")) {
		profile_snapshot_free(h_snap);
		h_snap = NULL;
	} else if (!strcmp(c, "p_free")) {
		if (h_snap)
			profile_snapshot_free(h_snap);
		h_snap = NULL;
		profiler_free();
		profiler_name_store_free(h_store);
		h_store = NULL;
		puts("p_free done");
	} else if (!strcmp(c, "p_null")) {
		printf("p_null roots=%zu children=%zu name=%s count=%" PRIu64 "\n", profiler_snapshot_num_roots(NULL),
		       profiler_snapshot_num_children(NULL),
		       profiler_snapshot_entry_name(NULL) ? "x" : "@null", profiler_snapshot_entry_overall_count(NULL));
	}
}

/* ------------------------------------------------------------------------- */
/* BPM (shared/bpm/bpm.c) with scripted counters                             */

#include "../../../shared/bpm/bpm.c"

static struct {
	int out_total, out_dropped;
	uint32_t rendered, lagged;
	uint32_t v_total, v_skipped, enc_out;
	int has_video;
	char codec[16];
} h_bpm = {.has_video = 1, .codec = "h264"};

int obs_output_get_total_frames(const obs_output_t *o)
{
	(void)o;
	return h_bpm.out_total;
}
int obs_output_get_frames_dropped(const obs_output_t *o)
{
	(void)o;
	return h_bpm.out_dropped;
}
uint32_t obs_get_total_frames(void)
{
	return h_bpm.rendered;
}
uint32_t obs_get_lagged_frames(void)
{
	return h_bpm.lagged;
}
video_t *obs_encoder_video(const obs_encoder_t *e)
{
	(void)e;
	return h_bpm.has_video ? (video_t *)&h_bpm : NULL;
}
uint32_t video_output_get_total_frames(const video_t *v)
{
	(void)v;
	return h_bpm.v_total;
}
uint32_t video_output_get_skipped_frames(const video_t *v)
{
	(void)v;
	return h_bpm.v_skipped;
}
uint32_t obs_encoder_get_encoded_frames(const obs_encoder_t *e)
{
	(void)e;
	return h_bpm.enc_out;
}
const char *obs_encoder_get_name(const obs_encoder_t *e)
{
	(void)e;
	return "enc";
}
const char *obs_encoder_get_codec(const obs_encoder_t *e)
{
	(void)e;
	return h_bpm.codec;
}
/* the runner owns the input buffers: releasing is a no-op */
void obs_encoder_packet_release(struct encoder_packet *p)
{
	(void)p;
}

/* wall clock model: nstime N -> UNIX time N ns (the reference adds a start-up offset) */
struct timespec *os_nstime_to_timespec(uint64_t timestamp, struct timespec *storage)
{
	if (!storage)
		return NULL;
	storage->tv_sec = (time_t)(timestamp / 1000000000ULL);
	storage->tv_nsec = (long)(timestamp % 1000000000ULL);
	return storage;
}

static char h_outputs[4];

static void h_bpm_cmd(int argc, char **argv)
{
	const char *c = argv[0];
	if (!strcmp(c, "bpm_set") && argc == 3) {
		const char *k = argv[1];
		uint64_t v = h_u64(argv[2]);
		if (!strcmp(k, "out_total"))
			h_bpm.out_total = (int)v;
		else if (!strcmp(k, "out_dropped"))
			h_bpm.out_dropped = (int)v;
		else if (!strcmp(k, "rendered"))
			h_bpm.rendered = (uint32_t)v;
		else if (!strcmp(k, "lagged"))
			h_bpm.lagged = (uint32_t)v;
		else if (!strcmp(k, "v_total"))
			h_bpm.v_total = (uint32_t)v;
		else if (!strcmp(k, "v_skipped"))
			h_bpm.v_skipped = (uint32_t)v;
		else if (!strcmp(k, "enc_out"))
			h_bpm.enc_out = (uint32_t)v;
		else if (!strcmp(k, "has_video"))
			h_bpm.has_video = (int)v;
		else if (!strcmp(k, "codec"))
			snprintf(h_bpm.codec, sizeof(h_bpm.codec), "%s", argv[2]);
	} else if (!strcmp(c, "bpm_inject") && argc == 12) {
		/* bpm_inject OUT TRACK PTS DTS KEYFRAME VIDEO DATAHEX CTS FER FERC PIR   (CTS @notime = no timing) */
		int out = atoi(argv[1]);
		size_t len;
		uint8_t *data = h_unhex(argv[7], &len);
		struct encoder_packet pkt = {0};
		struct encoder_packet_time pt = {0};
		pkt.data = data;
		pkt.size = len;
		pkt.track_idx = (size_t)h_u64(argv[2]);
		pkt.pts = h_i64(argv[3]);
		pkt.dts = h_i64(argv[4]);
		pkt.keyframe = atoi(argv[5]) != 0;
		pkt.type = atoi(argv[6]) ? OBS_ENCODER_VIDEO : OBS_ENCODER_AUDIO;
		pkt.encoder = (obs_encoder_t *)&h_bpm;
		bool has_time = strcmp(argv[8], "@notime") != 0;
		if (has_time) {
			pt.pts = pkt.pts;
			pt.cts = h_u64(argv[8]);
			pt.fer = h_u64(argv[9]);
			pt.ferc = h_u64(argv[10]);
			pt.pir = h_u64(argv[11]);
		}
		bpm_inject((obs_output_t *)&h_outputs[out], &pkt, has_time ? &pt : NULL, NULL);
		if (pkt.data == data) {
			printf("bpm_inject out=%d track=%s pts=%s: unchanged size=%zu\n", out, argv[2], argv[3], pkt.size);
		} else {
			printf("bpm_inject out=%d track=%s pts=%s: size=%zu data=", out, argv[2], argv[3], pkt.size);
			script_print_hex(pkt.data, pkt.size);
			putchar('\n');
			bfree(pkt.data - sizeof(long));
		}
		free(data);
	} else if (!strcmp(c, "bpm_destroy") && argc == 2) {
		bpm_destroy((obs_output_t *)&h_outputs[atoi(argv[1])]);
		printf("bpm_destroy %s\n", argv[1]);
	} else if (!strcmp(c, "bpm_null")) {
		struct encoder_packet pkt = {0};
		bpm_inject(NULL, &pkt, NULL, NULL);
		bpm_inject((obs_output_t *)&h_outputs[0], NULL, NULL, NULL);
		puts("bpm_null ok");
	}
}

/* ------------------------------------------------------------------------- */
/* happy eyeballs: address ordering only (getaddrinfo is faked)              */

static int h_he_hint_family;
static char h_he_port[32];
static char h_he_host[128];
static int h_he_resolve_err;
static int h_he_families[32];
static int h_he_nfam;

int getaddrinfo(const char *node, const char *service, const struct addrinfo *hints, struct addrinfo **res)
{
	snprintf(h_he_host, sizeof(h_he_host), "%s", node ? node : "@null");
	snprintf(h_he_port, sizeof(h_he_port), "%s", service ? service : "@null");
	h_he_hint_family = hints->ai_family;
	if (h_he_resolve_err) {
		*res = NULL;
		return h_he_resolve_err;
	}
	struct addrinfo *head = NULL, **tail = &head;
	for (int i = 0; i < h_he_nfam; i++) {
		struct addrinfo *a = calloc(1, sizeof(*a));
		a->ai_family = h_he_families[i];
		a->ai_socktype = hints->ai_socktype;
		a->ai_protocol = i + 1; /* identifies the node (its position in the resolver result) */
		*tail = a;
		tail = &a->ai_next;
	}
	*res = head;
	return 0;
}

void freeaddrinfo(struct addrinfo *res)
{
	while (res) {
		struct addrinfo *n = res->ai_next;
		free(res);
		res = n;
	}
}

#include "../../../shared/happy-eyeballs/happy-eyeballs.c"

static void h_he_cmd(int argc, char **argv)
{
	const char *c = argv[0];
	if (!strcmp(c, "he_resolve") && argc >= 2) {
		/* he_resolve BINDLEN HOST PORT FAMILY... : FAMILY = 4 | 6 | x (other, e.g. AF_UNIX) */
		(void)0;
	}
	if (!strcmp(c, "he_order") && argc >= 5) {
		/* he_order BINDLEN HOST PORT F F F ... */
		struct happy_eyeballs_ctx ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.bind_addr_len = (socklen_t)h_u64(argv[1]);
		h_he_nfam = 0;
		h_he_resolve_err = 0;
		for (int i = 4; i < argc && h_he_nfam < 32; i++)
			h_he_families[h_he_nfam++] = argv[i][0] == '4' ? AF_INET : argv[i][0] == '6' ? AF_INET6 : AF_UNIX;
		int r = build_addr_list(argv[2], (int)h_i64(argv[3]), &ctx);
		printf("he_order bind_len=%s host=%s port=%s: result=%d hint_family=%s resolver_saw_host=%s service=%s\n",
		       argv[1], argv[2], argv[3], r,
		       h_he_hint_family == AF_INET     ? "inet"
		       : h_he_hint_family == AF_INET6 ? "inet6"
		       : h_he_hint_family == AF_UNSPEC ? "unspec"
						       : "other",
		       h_he_host, h_he_port);
		printf("  order:");
		for (struct addrinfo *a = ctx.addresses; a; a = a->ai_next)
			printf(" %d%c", a->ai_protocol, a->ai_family == AF_INET ? 'a' : a->ai_family == AF_INET6 ? 'b' : 'x');
		putchar('\n');
		freeaddrinfo(ctx.addresses);
	} else if (!strcmp(c, "he_fail") && argc == 2) {
		struct happy_eyeballs_ctx ctx;
		memset(&ctx, 0, sizeof(ctx));
		h_he_resolve_err = EAI_NONAME;
		int r = build_addr_list("h", 1, &ctx);
		h_he_resolve_err = 0;
		printf("he_fail result=%d addresses=%s\n", r, ctx.addresses ? "set" : "null");
	} else if (!strcmp(c, "he_consts")) {
		printf("he_consts delay_ms=%d max_attempts=%d connection_timeout_ms=%d\n", HAPPY_EYEBALLS_DELAY_MS,
		       HAPPY_EYEBALLS_MAX_ATTEMPTS, HAPPY_EYEBALLS_CONNECTION_TIMEOUT_MS);
	}
}

/* ------------------------------------------------------------------------- */
/* file-updater (fake curl, in-memory flat file system)                      */

#define H_VFS_MAX 64
static struct {
	char *path;
	char *data;
	size_t len;
} h_vfs[H_VFS_MAX];

static int h_vfs_find(const char *p)
{
	for (int i = 0; i < H_VFS_MAX; i++)
		if (h_vfs[i].path && !strcmp(h_vfs[i].path, p))
			return i;
	return -1;
}

static void h_vfs_set(const char *p, const char *data, size_t len)
{
	int i = h_vfs_find(p);
	if (i < 0) {
		for (i = 0; i < H_VFS_MAX && h_vfs[i].path; i++)
			;
		if (i == H_VFS_MAX) {
			fprintf(stderr, "vfs full\n");
			exit(2);
		}
		h_vfs[i].path = strdup(p);
	} else {
		free(h_vfs[i].data);
	}
	h_vfs[i].data = malloc(len + 1);
	memcpy(h_vfs[i].data, data, len);
	h_vfs[i].data[len] = 0;
	h_vfs[i].len = len;
}

static void h_vfs_del(int i)
{
	free(h_vfs[i].path);
	free(h_vfs[i].data);
	h_vfs[i].path = NULL;
	h_vfs[i].data = NULL;
}

char *os_quick_read_utf8_file(const char *path)
{
	int i = h_vfs_find(path);
	printf("fs read %s -> %s\n", path, i < 0 ? "missing" : "ok");
	return i < 0 ? NULL : bstrdup(h_vfs[i].data);
}

bool os_quick_write_utf8_file(const char *path, const char *str, size_t len, bool marker)
{
	(void)marker;
	printf("fs write %s len=%zu\n", path, len);
	h_vfs_set(path, str, len);
	return true;
}

bool os_quick_write_utf8_file_safe(const char *path, const char *str, size_t len, bool marker, const char *temp_ext,
				   const char *backup_ext)
{
	(void)marker, (void)temp_ext, (void)backup_ext;
	printf("fs write_safe %s len=%zu\n", path, len);
	h_vfs_set(path, str, len);
	return true;
}

int os_copyfile(const char *in, const char *out)
{
	int i = h_vfs_find(in);
	printf("fs copy %s -> %s : %s\n", in, out, i < 0 ? "fail" : "ok");
	if (i < 0)
		return -1;
	h_vfs_set(out, h_vfs[i].data, h_vfs[i].len);
	return 0;
}

int os_unlink(const char *path)
{
	int i = h_vfs_find(path);
	printf("fs unlink %s : %s\n", path, i < 0 ? "fail" : "ok");
	if (i < 0)
		return -1;
	h_vfs_del(i);
	return 0;
}

int os_rename(const char *from, const char *to)
{
	int i = h_vfs_find(from);
	printf("fs rename %s -> %s : %s\n", from, to, i < 0 ? "fail" : "ok");
	if (i < 0)
		return -1;
	h_vfs_set(to, h_vfs[i].data, h_vfs[i].len);
	h_vfs_del(h_vfs_find(from));
	return 0;
}

int os_rmdir(const char *path)
{
	printf("fs rmdir %s\n", path);
	return 0;
}

int os_mkdir(const char *path)
{
	printf("fs mkdir %s\n", path);
	return 0;
}

/* fake libcurl */
#define H_HTTP_MAX 16
static struct {
	char *url;
	int code; /* -1 = transfer failure */
	char *body;
	char *etag;
} h_http[H_HTTP_MAX];

struct h_curl {
	const char *url;
	size_t (*wfn)(char *, size_t, size_t, void *);
	void *wdata;
	size_t (*hfn)(char *, size_t, size_t, void *);
	void *hdata;
	char *err;
	struct curl_slist *hdrs;
	long code;
};

CURL *curl_easy_init(void)
{
	return calloc(1, sizeof(struct h_curl));
}

void curl_easy_cleanup(CURL *c)
{
	free(c);
}

CURLcode curl_easy_setopt(CURL *handle, CURLoption opt, ...)
{
	struct h_curl *c = handle;
	va_list va;
	va_start(va, opt);
	switch (opt) {
	case CURLOPT_URL:
		c->url = va_arg(va, const char *);
		break;
	case CURLOPT_WRITEFUNCTION:
		c->wfn = va_arg(va, void *);
		break;
	case CURLOPT_WRITEDATA:
		c->wdata = va_arg(va, void *);
		break;
	case CURLOPT_HEADERFUNCTION:
		c->hfn = va_arg(va, void *);
		break;
	case CURLOPT_HEADERDATA:
		c->hdata = va_arg(va, void *);
		break;
	case CURLOPT_ERRORBUFFER:
		c->err = va_arg(va, char *);
		break;
	case CURLOPT_HTTPHEADER:
		c->hdrs = va_arg(va, struct curl_slist *);
		break;
	default:
		break;
	}
	va_end(va);
	return CURLE_OK;
}

struct curl_slist *curl_slist_append(struct curl_slist *list, const char *s)
{
	struct curl_slist *n = calloc(1, sizeof(*n));
	n->data = strdup(s);
	if (!list)
		return n;
	struct curl_slist *t = list;
	while (t->next)
		t = t->next;
	t->next = n;
	return list;
}

void curl_slist_free_all(struct curl_slist *list)
{
	while (list) {
		struct curl_slist *n = list->next;
		free(list->data);
		free(list);
		list = n;
	}
}

CURLcode curl_easy_perform(CURL *handle)
{
	struct h_curl *c = handle;
	int idx = -1;
	printf("http GET %s", c->url);
	for (struct curl_slist *h = c->hdrs; h; h = h->next) {
		fputs(" ", stdout);
		script_print_quoted(h->data);
	}
	putchar('\n');
	for (int i = 0; i < H_HTTP_MAX; i++)
		if (h_http[i].url && !strcmp(h_http[i].url, c->url))
			idx = i;
	if (idx < 0 || h_http[idx].code < 0) {
		snprintf(c->err, CURL_ERROR_SIZE, "transfer failed");
		return CURLE_COULDNT_CONNECT;
	}
	c->code = h_http[idx].code;
	if (c->code >= 400) { /* CURLOPT_FAILONERROR is set by the reference */
		snprintf(c->err, CURL_ERROR_SIZE, "HTTP error");
		return CURLE_HTTP_RETURNED_ERROR;
	}
	if (c->hfn && h_http[idx].etag) {
		char hdr[256];
		snprintf(hdr, sizeof(hdr), "%s", h_http[idx].etag);
		c->hfn(hdr, 1, strlen(hdr), c->hdata);
	}
	if (c->wfn && h_http[idx].body && *h_http[idx].body)
		c->wfn(h_http[idx].body, 1, strlen(h_http[idx].body), c->wdata);
	return CURLE_OK;
}

CURLcode curl_easy_getinfo(CURL *handle, CURLINFO info, ...)
{
	struct h_curl *c = handle;
	va_list va;
	va_start(va, info);
	if (info == CURLINFO_RESPONSE_CODE)
		*va_arg(va, long *) = c->code;
	va_end(va);
	return CURLE_OK;
}

#include "../../../shared/file-updater/file-updater/file-updater.c"
#undef info
#undef warn

static int h_fu_confirm_mode; /* 0 accept, 1 reject, 2 replace */
static char h_fu_replace[256];

static bool h_fu_confirm(void *param, struct file_download_data *file)
{
	(void)param;
	printf("confirm name=");
	h_quoted_or_null(file->name);
	printf(" version=%d size=%zu\n", file->version, file->buffer.num);
	if (h_fu_confirm_mode == 2) {
		/* the callback may replace the buffer; the reference writes the replaced buffer */
		da_resize(file->buffer, 0);
		da_push_back_array(file->buffer, h_fu_replace, strlen(h_fu_replace));
		uint8_t z = 0;
		da_push_back(file->buffer, &z);
	}
	return h_fu_confirm_mode != 1;
}

static struct update_info *h_fu_make(const char *local, const char *cache, const char *url_base, const char *ua,
				     bool with_cb)
{
	struct dstr dir = {0};
	struct update_info *i = bzalloc(sizeof(*i));
	i->log_prefix = bstrdup("[fu] ");
	i->user_agent = bstrdup(ua);
	dstr_copy(&dir, cache);
	if (dstr_end(&dir) != '/' && dstr_end(&dir) != '\\')
		dstr_cat_ch(&dir, '/');
	dstr_cat(&dir, ".temp");
	i->temp = dir.array;
	i->local = bstrdup(local);
	i->cache = bstrdup(cache);
	i->url = get_path(url_base, "package.json");
	i->callback = with_cb ? h_fu_confirm : NULL;
	return i;
}

static void h_fu_cmd(int argc, char **argv)
{
	const char *c = argv[0];
	if (!strcmp(c, "fu_put") && argc == 3) {
		h_vfs_set(argv[1], argv[2], strlen(argv[2]));
	} else if (!strcmp(c, "fu_cat") && argc == 2) {
		int i = h_vfs_find(argv[1]);
		printf("fu_cat %s ", argv[1]);
		if (i < 0)
			puts("@missing");
		else {
			script_print_quoted(h_vfs[i].data);
			putchar('\n');
		}
	} else if (!strcmp(c, "fu_json_get") && argc == 3) {
		int i = h_vfs_find(argv[1]);
		printf("fu_json_get %s %s ", argv[1], argv[2]);
		if (i < 0) {
			puts("@missing");
			return;
		}
		obs_data_t *d = obs_data_create_from_json(h_vfs[i].data);
		if (!d) {
			puts("@invalid");
			return;
		}
		h_quoted_or_null(obs_data_get_string(d, argv[2]));
		putchar('\n');
		obs_data_release(d);
	} else if (!strcmp(c, "fu_ls")) {
		/* sorted listing */
		printf("fu_ls\n");
		char last[512] = "";
		for (;;) {
			int best = -1;
			for (int i = 0; i < H_VFS_MAX; i++)
				if (h_vfs[i].path && strcmp(h_vfs[i].path, last) > 0 &&
				    (best < 0 || strcmp(h_vfs[i].path, h_vfs[best].path) < 0))
					best = i;
			if (best < 0)
				break;
			printf("  %s (%zu bytes)\n", h_vfs[best].path, h_vfs[best].len);
			snprintf(last, sizeof(last), "%s", h_vfs[best].path);
		}
	} else if (!strcmp(c, "fu_http") && argc >= 4) {
		/* fu_http URL CODE BODY [ETAG-HEADER-LINE]: CODE -1 = transfer failure */
		int i;
		for (i = 0; i < H_HTTP_MAX && h_http[i].url && strcmp(h_http[i].url, argv[1]); i++)
			;
		if (i == H_HTTP_MAX)
			return;
		free(h_http[i].url);
		free(h_http[i].body);
		free(h_http[i].etag);
		h_http[i].url = strdup(argv[1]);
		h_http[i].code = atoi(argv[2]);
		h_http[i].body = strdup(argv[3]);
		h_http[i].etag = argc > 4 ? strdup(argv[4]) : NULL;
	} else if (!strcmp(c, "fu_confirm") && argc >= 2) {
		h_fu_confirm_mode = !strcmp(argv[1], "reject") ? 1 : !strcmp(argv[1], "replace") ? 2 : 0;
		if (argc > 2)
			snprintf(h_fu_replace, sizeof(h_fu_replace), "%s", argv[2]);
	} else if (!strcmp(c, "fu_run") && argc >= 5) {
		/* fu_run LOCAL CACHE URLBASE USERAGENT [nocb]: runs update_thread synchronously */
		struct update_info *i = h_fu_make(argv[1], argv[2], argv[3], argv[4], argc < 6);
		printf("fu_run local=%s cache=%s temp=%s url=%s\n", i->local, i->cache, i->temp, i->url);
		update_thread(i);
		update_info_destroy(i);
		puts("fu_run done");
	} else if (!strcmp(c, "fu_single") && argc == 2) {
		struct update_info *i = bzalloc(sizeof(*i));
		i->log_prefix = bstrdup("");
		i->url = bstrdup(argv[1]);
		i->callback = h_fu_confirm;
		single_file_thread(i);
		update_info_destroy(i);
		puts("fu_single done");
	} else if (!strcmp(c, "fu_get_path") && argc == 3) {
		char *p = get_path(h_str(argv[1]), argv[2]);
		printf("fu_get_path ");
		h_quoted_or_null(p);
		putchar('\n');
		bfree(p);
	}
}

/* ------------------------------------------------------------------------- */
/* constants                                                                 */

#define C(x) printf("%s %lld\n", #x, (long long)(x))
static void h_const_cmd(void)
{
	C(MAX_CHANNELS);
	C(OBS_ALIGN_CENTER);
	C(OBS_ALIGN_LEFT);
	C(OBS_ALIGN_RIGHT);
	C(OBS_ALIGN_TOP);
	C(OBS_ALIGN_BOTTOM);
	C(MODULE_SUCCESS);
	C(MODULE_ERROR);
	C(MODULE_FAILED_TO_OPEN);
	C(MODULE_MISSING_EXPORTS);
	C(MODULE_INCOMPATIBLE_VER);
	C(MODULE_HARDCODED_SKIP);
	C(OBS_OUTPUT_SUCCESS);
	C(OBS_OUTPUT_BAD_PATH);
	C(OBS_OUTPUT_CONNECT_FAILED);
	C(OBS_OUTPUT_INVALID_STREAM);
	C(OBS_OUTPUT_ERROR);
	C(OBS_OUTPUT_DISCONNECTED);
	C(OBS_OUTPUT_UNSUPPORTED);
	C(OBS_OUTPUT_NO_SPACE);
	C(OBS_OUTPUT_ENCODE_ERROR);
	C(OBS_OUTPUT_HDR_DISABLED);
	C(OBS_VIDEO_SUCCESS);
	C(OBS_VIDEO_FAIL);
	C(OBS_VIDEO_NOT_SUPPORTED);
	C(OBS_VIDEO_INVALID_PARAM);
	C(OBS_VIDEO_CURRENTLY_ACTIVE);
	C(OBS_VIDEO_MODULE_NOT_FOUND);
	C(LIBOBS_API_MAJOR_VER);
	C(LIBOBS_API_MINOR_VER);
	C(LIBOBS_API_PATCH_VER);
	C(OBS_SOURCE_FRAME_LINEAR_ALPHA);
	C(OBS_SOURCE_FLAG_UNUSED_1);
	C(OBS_SOURCE_FLAG_FORCE_MONO);
	C(OBS_OUTPUT_DELAY_PRESERVE);
	C(OBS_ORDER_MOVE_UP);
	C(OBS_ORDER_MOVE_DOWN);
	C(OBS_ORDER_MOVE_TOP);
	C(OBS_ORDER_MOVE_BOTTOM);
	C(OBS_NO_DIRECT_RENDERING);
	C(OBS_ALLOW_DIRECT_RENDERING);
	C(OBS_SCALE_DISABLE);
	C(OBS_SCALE_POINT);
	C(OBS_SCALE_BICUBIC);
	C(OBS_SCALE_BILINEAR);
	C(OBS_SCALE_LANCZOS);
	C(OBS_SCALE_AREA);
	C(OBS_BLEND_METHOD_DEFAULT);
	C(OBS_BLEND_METHOD_SRGB_OFF);
	C(OBS_BLEND_NORMAL);
	C(OBS_BLEND_ADDITIVE);
	C(OBS_BLEND_SUBTRACT);
	C(OBS_BLEND_SCREEN);
	C(OBS_BLEND_MULTIPLY);
	C(OBS_BLEND_LIGHTEN);
	C(OBS_BLEND_DARKEN);
	C(OBS_BOUNDS_NONE);
	C(OBS_BOUNDS_STRETCH);
	C(OBS_BOUNDS_SCALE_INNER);
	C(OBS_BOUNDS_SCALE_OUTER);
	C(OBS_BOUNDS_SCALE_TO_WIDTH);
	C(OBS_BOUNDS_SCALE_TO_HEIGHT);
	C(OBS_BOUNDS_MAX_ONLY);
	C(OBS_MODULE_INVALID);
	C(OBS_MODULE_ENABLED);
	C(OBS_MODULE_MISSING);
	C(OBS_MODULE_DISABLED);
	C(OBS_MODULE_DISABLED_SAFE);
	C(OBS_MODULE_FAILED_TO_OPEN);
	C(OBS_MODULE_FAILED_TO_INITIALIZE);
	C(OBS_MODULE_TYPE_UNDEFINED);
	C(OBS_MODULE_TYPE_CORE);
	C(OBS_MODULE_TYPE_PLUGIN);
	C(OBS_MODULE_TYPE_LEGACY_PLUGIN);
	C(OBS_EFFECT_DEFAULT);
	C(OBS_EFFECT_DEFAULT_RECT);
	C(OBS_EFFECT_OPAQUE);
	C(OBS_EFFECT_SOLID);
	C(OBS_EFFECT_BICUBIC);
	C(OBS_EFFECT_LANCZOS);
	C(OBS_EFFECT_BILINEAR_LOWRES);
	C(OBS_EFFECT_PREMULTIPLIED_ALPHA);
	C(OBS_EFFECT_REPEAT);
	C(OBS_EFFECT_AREA);
	C(OBS_OBJ_TYPE_INVALID);
	C(OBS_OBJ_TYPE_SOURCE);
	C(OBS_OBJ_TYPE_OUTPUT);
	C(OBS_OBJ_TYPE_ENCODER);
	C(OBS_OBJ_TYPE_SERVICE);
	C(OBS_OBJ_TYPE_CANVAS);
	C(OBS_TASK_UI);
	C(OBS_TASK_GRAPHICS);
	C(OBS_TASK_AUDIO);
	C(OBS_TASK_DESTROY);
	C(OBS_DEINTERLACE_MODE_DISABLE);
	C(OBS_DEINTERLACE_MODE_DISCARD);
	C(OBS_DEINTERLACE_MODE_RETRO);
	C(OBS_DEINTERLACE_MODE_BLEND);
	C(OBS_DEINTERLACE_MODE_BLEND_2X);
	C(OBS_DEINTERLACE_MODE_LINEAR);
	C(OBS_DEINTERLACE_MODE_LINEAR_2X);
	C(OBS_DEINTERLACE_MODE_YADIF);
	C(OBS_DEINTERLACE_MODE_YADIF_2X);
	C(OBS_DEINTERLACE_FIELD_ORDER_TOP);
	C(OBS_DEINTERLACE_FIELD_ORDER_BOTTOM);
	C(OBS_MONITORING_TYPE_NONE);
	C(OBS_MONITORING_TYPE_MONITOR_ONLY);
	C(OBS_MONITORING_TYPE_MONITOR_AND_OUTPUT);
	C(OBS_TRANSITION_SOURCE_A);
	C(OBS_TRANSITION_SOURCE_B);
	C(OBS_TRANSITION_MODE_AUTO);
	C(OBS_TRANSITION_MODE_MANUAL);
	C(OBS_TRANSITION_SCALE_MAX_ONLY);
	C(OBS_TRANSITION_SCALE_ASPECT);
	C(OBS_TRANSITION_SCALE_STRETCH);
	C(OBS_SCENE_DUP_REFS);
	C(OBS_SCENE_DUP_COPY);
	C(OBS_SCENE_DUP_PRIVATE_REFS);
	C(OBS_SCENE_DUP_PRIVATE_COPY);
	C(MAIN);
	C(ACTIVATE);
	C(MIX_AUDIO);
	C(SCENE_REF);
	C(EPHEMERAL);
	C(PROGRAM);
	C(PREVIEW);
	C(DEVICE);
	C(OBS_SOURCE_VIDEO);
	C(OBS_SOURCE_AUDIO);
	C(OBS_SOURCE_ASYNC);
	C(OBS_SOURCE_CUSTOM_DRAW);
	C(OBS_SOURCE_INTERACTION);
	C(OBS_SOURCE_COMPOSITE);
	C(OBS_SOURCE_DO_NOT_DUPLICATE);
	C(OBS_SOURCE_DEPRECATED);
	C(OBS_SOURCE_DO_NOT_SELF_MONITOR);
	C(OBS_SOURCE_CAP_DISABLED);
	C(OBS_SOURCE_MONITOR_BY_DEFAULT);
	C(OBS_SOURCE_SUBMIX);
	C(OBS_SOURCE_CONTROLLABLE_MEDIA);
	C(OBS_SOURCE_CEA_708);
	C(OBS_SOURCE_SRGB);
	C(OBS_SOURCE_CAP_DONT_SHOW_PROPERTIES);
	C(OBS_SOURCE_REQUIRES_CANVAS);
	C(OBS_SOURCE_TYPE_INPUT);
	C(OBS_SOURCE_TYPE_FILTER);
	C(OBS_SOURCE_TYPE_TRANSITION);
	C(OBS_SOURCE_TYPE_SCENE);
	C(OBS_BALANCE_TYPE_SINE_LAW);
	C(OBS_BALANCE_TYPE_SQUARE_LAW);
	C(OBS_BALANCE_TYPE_LINEAR);
	C(OBS_ICON_TYPE_UNKNOWN);
	C(OBS_ICON_TYPE_IMAGE);
	C(OBS_ICON_TYPE_COLOR);
	C(OBS_ICON_TYPE_SLIDESHOW);
	C(OBS_ICON_TYPE_AUDIO_INPUT);
	C(OBS_ICON_TYPE_AUDIO_OUTPUT);
	C(OBS_ICON_TYPE_DESKTOP_CAPTURE);
	C(OBS_ICON_TYPE_WINDOW_CAPTURE);
	C(OBS_ICON_TYPE_GAME_CAPTURE);
	C(OBS_ICON_TYPE_CAMERA);
	C(OBS_ICON_TYPE_TEXT);
	C(OBS_ICON_TYPE_MEDIA);
	C(OBS_ICON_TYPE_BROWSER);
	C(OBS_ICON_TYPE_CUSTOM);
	C(OBS_ICON_TYPE_PROCESS_AUDIO_OUTPUT);
	C(OBS_MEDIA_STATE_NONE);
	C(OBS_MEDIA_STATE_PLAYING);
	C(OBS_MEDIA_STATE_OPENING);
	C(OBS_MEDIA_STATE_BUFFERING);
	C(OBS_MEDIA_STATE_PAUSED);
	C(OBS_MEDIA_STATE_STOPPED);
	C(OBS_MEDIA_STATE_ENDED);
	C(OBS_MEDIA_STATE_ERROR);
	C(OBS_ENCODER_CAP_DEPRECATED);
	C(OBS_ENCODER_CAP_PASS_TEXTURE);
	C(OBS_ENCODER_CAP_DYN_BITRATE);
	C(OBS_ENCODER_CAP_INTERNAL);
	C(OBS_ENCODER_CAP_ROI);
	C(OBS_ENCODER_CAP_SCALING);
	C(OBS_ENCODER_CAP_MULTITRACK_DYN_BITRATE);
	C(OBS_ENCODER_AUDIO);
	C(OBS_ENCODER_VIDEO);
	C(OBS_OUTPUT_VIDEO);
	C(OBS_OUTPUT_AUDIO);
	C(OBS_OUTPUT_ENCODED);
	C(OBS_OUTPUT_SERVICE);
	C(OBS_OUTPUT_MULTI_TRACK);
	C(OBS_OUTPUT_CAN_PAUSE);
	C(OBS_OUTPUT_MULTI_TRACK_VIDEO);
	C(MAX_OUTPUT_AUDIO_ENCODERS);
	C(MAX_OUTPUT_VIDEO_ENCODERS);
	C(OBS_PROPERTIES_DEFER_UPDATE);
	C(OBS_FONT_BOLD);
	C(OBS_FONT_ITALIC);
	C(OBS_FONT_UNDERLINE);
	C(OBS_FONT_STRIKEOUT);
	C(OBS_PROPERTY_INVALID);
	C(OBS_PROPERTY_BOOL);
	C(OBS_PROPERTY_INT);
	C(OBS_PROPERTY_FLOAT);
	C(OBS_PROPERTY_TEXT);
	C(OBS_PROPERTY_PATH);
	C(OBS_PROPERTY_LIST);
	C(OBS_PROPERTY_COLOR);
	C(OBS_PROPERTY_BUTTON);
	C(OBS_PROPERTY_FONT);
	C(OBS_PROPERTY_EDITABLE_LIST);
	C(OBS_PROPERTY_FRAME_RATE);
	C(OBS_PROPERTY_GROUP);
	C(OBS_PROPERTY_COLOR_ALPHA);
	C(OBS_COMBO_FORMAT_INVALID);
	C(OBS_COMBO_FORMAT_INT);
	C(OBS_COMBO_FORMAT_FLOAT);
	C(OBS_COMBO_FORMAT_STRING);
	C(OBS_COMBO_FORMAT_BOOL);
	C(OBS_COMBO_TYPE_INVALID);
	C(OBS_COMBO_TYPE_EDITABLE);
	C(OBS_COMBO_TYPE_LIST);
	C(OBS_COMBO_TYPE_RADIO);
	C(OBS_EDITABLE_LIST_TYPE_STRINGS);
	C(OBS_EDITABLE_LIST_TYPE_FILES);
	C(OBS_EDITABLE_LIST_TYPE_FILES_AND_URLS);
	C(OBS_PATH_FILE);
	C(OBS_PATH_FILE_SAVE);
	C(OBS_PATH_DIRECTORY);
	C(OBS_TEXT_DEFAULT);
	C(OBS_TEXT_PASSWORD);
	C(OBS_TEXT_MULTILINE);
	C(OBS_TEXT_INFO);
	C(OBS_TEXT_INFO_NORMAL);
	C(OBS_TEXT_INFO_WARNING);
	C(OBS_TEXT_INFO_ERROR);
	C(OBS_NUMBER_SCROLLER);
	C(OBS_NUMBER_SLIDER);
	C(OBS_COMBO_INVALID);
	C(OBS_GROUP_NORMAL);
	C(OBS_GROUP_CHECKABLE);
	C(OBS_BUTTON_DEFAULT);
	C(OBS_BUTTON_URL);
	C(OBS_DATA_NULL);
	C(OBS_DATA_STRING);
	C(OBS_DATA_NUMBER);
	C(OBS_DATA_BOOLEAN);
	C(OBS_DATA_OBJECT);
	C(OBS_DATA_ARRAY);
	C(OBS_DATA_NUM_INVALID);
	C(OBS_DATA_NUM_INT);
	C(OBS_DATA_NUM_DOUBLE);
	C(INTERACT_NONE);
	C(INTERACT_CAPS_KEY);
	C(INTERACT_SHIFT_KEY);
	C(INTERACT_CONTROL_KEY);
	C(INTERACT_ALT_KEY);
	C(INTERACT_MOUSE_LEFT);
	C(INTERACT_MOUSE_MIDDLE);
	C(INTERACT_MOUSE_RIGHT);
	C(INTERACT_COMMAND_KEY);
	C(INTERACT_NUMLOCK_KEY);
	C(INTERACT_IS_KEY_PAD);
	C(INTERACT_IS_LEFT);
	C(INTERACT_IS_RIGHT);
	C(MOUSE_LEFT);
	C(MOUSE_MIDDLE);
	C(MOUSE_RIGHT);
	C(OBS_FADER_CUBIC);
	C(OBS_FADER_IEC);
	C(OBS_FADER_LOG);
	C(SAMPLE_PEAK_METER);
	C(TRUE_PEAK_METER);
	C(OBS_SERVICE_CONNECT_INFO_SERVER_URL);
	C(OBS_SERVICE_CONNECT_INFO_STREAM_ID);
	C(OBS_SERVICE_CONNECT_INFO_STREAM_KEY);
	C(OBS_SERVICE_CONNECT_INFO_USERNAME);
	C(OBS_SERVICE_CONNECT_INFO_PASSWORD);
	C(OBS_SERVICE_CONNECT_INFO_ENCRYPT_PASSPHRASE);
	C(OBS_SERVICE_CONNECT_INFO_BEARER_TOKEN);
	C(LIBOBS_API_VER);
	C(OBS_KEY_LAST_VALUE);
}

/* ------------------------------------------------------------------------- */

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	base_set_log_handler(h_log_handler, NULL);
	struct script_line sl;
	int lineno = 0;

	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		int n = sl.argc;
		char **v = sl.argv;
		if (!strcmp(c, "echo")) {
			printf("echo %s\n", n > 1 ? v[1] : "");
		} else if (!strcmp(c, "opts_parse")) {
			h_opts_parse(n, v);
		} else if (!strncmp(c, "u128_", 5) || !strncmp(c, "mul_div64", 9)) {
			if (!strcmp(c, "mul_div64_by_zero")) {
				h_fork_crash_marker(h_div_zero_child, NULL, "mul_div64_by_zero");
				continue;
			}
			h_u128_cmd(n, v);
		} else if (!strncmp(c, "args_", 5)) {
			h_args_cmd(n, v);
		} else if (!strncmp(c, "hk_", 3)) {
			h_hk_cmd(n, v);
		} else if (!strncmp(c, "mf_", 3)) {
			h_mf_cmd(n, v);
		} else if (!strncmp(c, "task_", 5)) {
			h_task_cmd(n, v);
		} else if (!strncmp(c, "p_", 2)) {
			h_prof_cmd(n, v);
		} else if (!strncmp(c, "bpm_", 4)) {
			h_bpm_cmd(n, v);
		} else if (!strncmp(c, "he_", 3)) {
			h_he_cmd(n, v);
		} else if (!strncmp(c, "fu_", 3)) {
			h_fu_cmd(n, v);
		} else if (!strcmp(c, "semver") && n == 4) {
			int ma = (int)h_i64(v[1]), mi = (int)h_i64(v[2]), pa = (int)h_i64(v[3]);
			printf("semver %d %d %d = 0x%08x\n", ma, mi, pa, (unsigned)MAKE_SEMANTIC_VERSION(ma, mi, pa));
		} else if (!strcmp(c, "const")) {
			h_const_cmd();
		} else {
			fprintf(stderr, "helpers: unknown command '%s' at line %d\n", c, sl.lineno);
			return 2;
		}
	}
	fclose(f);
	return 0;
}
