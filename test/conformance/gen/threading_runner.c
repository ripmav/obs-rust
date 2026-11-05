/*
 * Conformance runner for threading primitives (spec S-061). Drives the real
 * os_event/os_sem implementation (libobs/util/threading-posix.c, linked via the
 * common sources) plus os_set_thread_name and the pthread mutex-init helpers.
 *
 * Blocking operations run on a single persistent worker thread. Every worker
 * command blocks the main thread until the worker has printed its result line,
 * so the output order is deterministic. Scripts must only issue a wait that
 * will be satisfied (pre-signalled, signalled during the command, or a timeout).
 * The `wake` forms have the main thread post/signal right after the worker is
 * dispatched; both orderings (post before the wait starts vs. waking a wait
 * that already started) print the same result, so no output depends on
 * scheduling.
 *
 * Main-thread commands:
 *   evt NAME auto|manual   os_event_init (prints ok / fail rc=R)
 *   esig NAME              os_event_signal (prints the return code)
 *   etry NAME              os_event_try
 *   ereset NAME            os_event_reset
 *   edest NAME|@null       os_event_destroy
 *   sem NAME N             os_sem_init (prints ok / fail rc=R; the slot is
 *                          left empty on failure — never sdest it)
 *   spost NAME [K]         os_sem_post K times (default 1; prints the rc)
 *   sdest NAME|@null       os_sem_destroy
 *   snull                  os_sem_wait(NULL) and os_sem_post(NULL)
 *   mval / mrec            pthread_mutex_init_value / pthread_mutex_init_recursive
 *   mlock / munlock        pthread_mutex_lock / pthread_mutex_unlock
 *   echo TEXT...
 * Worker commands:
 *   wwait NAME             os_event_wait
 *   wtimed NAME MS         os_event_timedwait
 *   wtimedwake NAME MS     os_event_timedwait; the main thread signals NAME
 *                          right after dispatch (result 0 in both orderings)
 *   wtry NAME              os_event_try
 *   wsem NAME              os_sem_wait
 *   wsemwake NAME [K]      os_sem_wait; the main thread posts K times (default 1)
 *                          right after dispatch
 *   wname NAME             os_set_thread_name(NAME), then prints the name read
 *                          back with pthread_getname_np (got="..." or rc=R)
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "harness.h"
#include "util/threading.h"

#define MAX_SLOTS 16
#define NAME_MAX 15

static char ev_names[MAX_SLOTS][NAME_MAX + 1];
static os_event_t *events[MAX_SLOTS];
static int ev_count;
static char sem_names[MAX_SLOTS][NAME_MAX + 1];
static os_sem_t *sems[MAX_SLOTS];
static int sem_count;

static int slot_index(char names[][NAME_MAX + 1], int count, const char *name)
{
	for (int i = 0; i < count; i++)
		if (strcmp(names[i], name) == 0)
			return i;
	return -1;
}

static int slot_add(char names[][NAME_MAX + 1], int *count, const char *name)
{
	if (*count >= MAX_SLOTS || strlen(name) > NAME_MAX) {
		fprintf(stderr, "script: too many slots or name too long\n");
		exit(2);
	}
	int i = (*count)++;
	strncpy(names[i], name, NAME_MAX);
	names[i][NAME_MAX] = 0;
	return i;
}

/* Worker handshake: g_work (worker idles here), g_done (main waits here). */
static os_event_t *g_work;
static os_event_t *g_done;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

enum { K_EXIT, K_WWAIT, K_WTIMED, K_WTIMEDWAKE, K_WTRY, K_WSEM, K_WSEM_WAKE, K_WNAME };

struct work {
	int kind;
	os_event_t *ev;
	os_sem_t *sem;
	unsigned long ms;
	int posts;
	const char *name;
};
static struct work g_w;

static void *worker(void *arg)
{
	(void)arg;
	for (;;) {
		os_event_wait(g_work);
		pthread_mutex_lock(&g_lock);
		struct work w = g_w;
		pthread_mutex_unlock(&g_lock);
		int r = 0;
		if (w.kind == K_EXIT) {
			os_event_signal(g_done);
			return NULL;
		}
		switch (w.kind) {
		case K_WWAIT:
			r = os_event_wait(w.ev);
			printf("wwait %s %d\n", w.name, r);
			break;
		case K_WTIMED:
			r = os_event_timedwait(w.ev, w.ms);
			printf("wtimed %s %lu %d\n", w.name, w.ms, r);
			break;
		case K_WTIMEDWAKE:
			r = os_event_timedwait(w.ev, w.ms);
			printf("wtimedwake %s %lu %d\n", w.name, w.ms, r);
			break;
		case K_WTRY:
			r = os_event_try(w.ev);
			printf("wtry %s %d\n", w.name, r);
			break;
		case K_WSEM:
			r = os_sem_wait(w.sem);
			printf("wsem %s %d\n", w.name, r);
			break;
		case K_WSEM_WAKE:
			r = os_sem_wait(w.sem);
			printf("wsemwake %s %d\n", w.name, r);
			break;
		case K_WNAME:
			os_set_thread_name(w.name);
			{
				char buf[64] = {0};
				r = pthread_getname_np(pthread_self(), buf, sizeof(buf));
				if (r == 0) {
					printf("wname %s got=", w.name);
					script_print_quoted(buf);
					printf("\n");
				} else {
					printf("wname %s rc=%d\n", w.name, r);
				}
			}
			break;
		}
		os_event_signal(g_done);
	}
}

/* Dispatch a work item and block until the worker printed its result. */
static void dispatch(int kind, os_event_t *ev, os_sem_t *sem, unsigned long ms, int posts, const char *name)
{
	pthread_mutex_lock(&g_lock);
	g_w = (struct work){kind, ev, sem, ms, posts, name};
	pthread_mutex_unlock(&g_lock);
	os_event_signal(g_work);
	if (kind == K_WTIMEDWAKE)
		os_event_signal(ev);
	if (kind == K_WSEM_WAKE)
		for (int i = 0; i < posts; i++)
			os_sem_post(sem);
	os_event_wait(g_done);
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	pthread_mutex_t g_mutex;
	int g_mutex_ready = 0;

	os_event_init(&g_work, OS_EVENT_TYPE_AUTO);
	os_event_init(&g_done, OS_EVENT_TYPE_AUTO);
	pthread_t th;
	if (pthread_create(&th, NULL, worker, NULL) != 0)
		return 2;

	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		if (!strcmp(c, "echo")) {
			printf("echo");
			for (int i = 1; i < sl.argc; i++)
				printf(" %s", sl.argv[i]);
			printf("\n");
		} else if (!strcmp(c, "evt")) {
			script_need(&sl, 3);
			if (slot_index(ev_names, ev_count, sl.argv[1]) >= 0) {
				fprintf(stderr, "script: line %d: event '%s' exists\n", sl.lineno, sl.argv[1]);
				return 2;
			}
			os_event_t *ev = NULL;
			int r = os_event_init(&ev, sl.argv[2][0] == 'm' ? OS_EVENT_TYPE_MANUAL : OS_EVENT_TYPE_AUTO);
			if (r == 0) {
				int i = slot_add(ev_names, &ev_count, sl.argv[1]);
				events[i] = ev;
				printf("evt %s ok\n", sl.argv[1]);
			} else {
				printf("evt %s fail rc=%d\n", sl.argv[1], r);
			}
		} else if (!strcmp(c, "esig") || !strcmp(c, "etry") || !strcmp(c, "ereset") ||
			   !strcmp(c, "edest")) {
			script_need(&sl, 2);
			os_event_t *ev = NULL;
			if (strcmp(sl.argv[1], "@null") != 0) {
				int i = slot_index(ev_names, ev_count, sl.argv[1]);
				if (i < 0) {
					fprintf(stderr, "script: line %d: no event '%s'\n", sl.lineno, sl.argv[1]);
					return 2;
				}
				ev = events[i];
			}
			if (ev == NULL && strcmp(c, "edest") != 0) {
				/* os_event_destroy(NULL) is a guarded no-op; the other three
				 * would crash — a script precondition violation. */
				fprintf(stderr, "script: line %d: '%s' needs an event (no @null)\n", sl.lineno, c);
				return 2;
			}
			if (!strcmp(c, "esig"))
				printf("esig %s %d\n", sl.argv[1], os_event_signal(ev));
			else if (!strcmp(c, "etry"))
				printf("etry %s %d\n", sl.argv[1], os_event_try(ev));
			else if (!strcmp(c, "ereset")) {
				os_event_reset(ev);
				printf("ereset %s\n", sl.argv[1]);
			} else {
				os_event_destroy(ev);
				if (ev)
					events[slot_index(ev_names, ev_count, sl.argv[1])] = NULL;
				printf("edest %s\n", sl.argv[1]);
			}
		} else if (!strcmp(c, "sem")) {
			script_need(&sl, 3);
			if (slot_index(sem_names, sem_count, sl.argv[1]) >= 0) {
				fprintf(stderr, "script: line %d: sem '%s' exists\n", sl.lineno, sl.argv[1]);
				return 2;
			}
			os_sem_t *sem = NULL;
			int r = os_sem_init(&sem, (int)strtol(sl.argv[2], NULL, 10));
			if (r == 0) {
				int i = slot_add(sem_names, &sem_count, sl.argv[1]);
				sems[i] = sem;
				printf("sem %s ok\n", sl.argv[1]);
			} else {
				printf("sem %s fail rc=%d\n", sl.argv[1], r);
			}
		} else if (!strcmp(c, "spost") || !strcmp(c, "sdest")) {
			script_need(&sl, 2);
			os_sem_t *sem = NULL;
			if (strcmp(sl.argv[1], "@null") != 0) {
				int i = slot_index(sem_names, sem_count, sl.argv[1]);
				if (i < 0) {
					fprintf(stderr, "script: line %d: no sem '%s'\n", sl.lineno, sl.argv[1]);
					return 2;
				}
				sem = sems[i];
			}
			if (!strcmp(c, "spost")) {
				int k = sl.argc > 2 ? (int)strtol(sl.argv[2], NULL, 10) : 1;
				int r = 0;
				for (int i = 0; i < k; i++)
					r = os_sem_post(sem);
				printf("spost %s %d %d\n", sl.argv[1], k, r);
			} else {
				os_sem_destroy(sem);
				if (sem)
					sems[slot_index(sem_names, sem_count, sl.argv[1])] = NULL;
				printf("sdest %s\n", sl.argv[1]);
			}
		} else if (!strcmp(c, "snull")) {
			printf("snull wait=%d post=%d\n", os_sem_wait(NULL), os_sem_post(NULL));
		} else if (!strcmp(c, "mval")) {
			g_mutex_ready = 0;
			pthread_mutex_init_value(&g_mutex);
			g_mutex_ready = 1;
			printf("mval\n");
		} else if (!strcmp(c, "mrec")) {
			g_mutex_ready = 0;
			printf("mrec %d\n", pthread_mutex_init_recursive(&g_mutex));
			g_mutex_ready = 1;
		} else if (!strcmp(c, "mlock") || !strcmp(c, "munlock")) {
			if (!g_mutex_ready) {
				fprintf(stderr, "script: line %d: no mutex yet (mval/mrec first)\n", sl.lineno);
				return 2;
			}
			int r = !strcmp(c, "mlock") ? pthread_mutex_lock(&g_mutex) : pthread_mutex_unlock(&g_mutex);
			printf("%s %d\n", c, r);
		} else if (!strcmp(c, "wwait") || !strcmp(c, "wtry")) {
			script_need(&sl, 2);
			int i = slot_index(ev_names, ev_count, sl.argv[1]);
			if (i < 0) {
				fprintf(stderr, "script: line %d: no event '%s'\n", sl.lineno, sl.argv[1]);
				return 2;
			}
			dispatch(!strcmp(c, "wwait") ? K_WWAIT : K_WTRY, events[i], NULL, 0, 0, ev_names[i]);
		} else if (!strcmp(c, "wtimed") || !strcmp(c, "wtimedwake")) {
			script_need(&sl, 3);
			int i = slot_index(ev_names, ev_count, sl.argv[1]);
			if (i < 0) {
				fprintf(stderr, "script: line %d: no event '%s'\n", sl.lineno, sl.argv[1]);
				return 2;
			}
			unsigned long ms = strtoul(sl.argv[2], NULL, 10);
			dispatch(!strcmp(c, "wtimed") ? K_WTIMED : K_WTIMEDWAKE, events[i], NULL, ms, 0, ev_names[i]);
		} else if (!strcmp(c, "wsem") || !strcmp(c, "wsemwake")) {
			script_need(&sl, 2);
			int i = slot_index(sem_names, sem_count, sl.argv[1]);
			if (i < 0) {
				fprintf(stderr, "script: line %d: no sem '%s'\n", sl.lineno, sl.argv[1]);
				return 2;
			}
			int posts = sl.argc > 2 ? (int)strtol(sl.argv[2], NULL, 10) : 1;
			dispatch(!strcmp(c, "wsem") ? K_WSEM : K_WSEM_WAKE, NULL, sems[i], 0, posts, sem_names[i]);
		} else if (!strcmp(c, "wname")) {
			script_need(&sl, 2);
			dispatch(K_WNAME, NULL, NULL, 0, 0, sl.argv[1]);
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, c);
			return 2;
		}
	}

	dispatch(K_EXIT, NULL, NULL, 0, 0, NULL);
	pthread_join(th, NULL);
	for (int i = 0; i < ev_count; i++)
		os_event_destroy(events[i]);
	for (int i = 0; i < sem_count; i++)
		os_sem_destroy(sems[i]);
	os_event_destroy(g_work);
	os_event_destroy(g_done);
	fclose(f);
	return 0;
}
