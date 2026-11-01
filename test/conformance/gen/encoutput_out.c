/*
 * S-041 conformance: obs-output.c is #included so that the reconnect timer and thread creation can be replaced by
 * deterministic fakes. The renames are applied before the reference code (and the headers it includes) is read;
 * the replacement bodies are below the include. Test tooling, not part of libobs.
 */
#define os_event_timedwait eo_event_timedwait
#define pthread_create eo_pthread_create
#define pthread_join eo_pthread_join
#define os_event_wait eo_event_wait
#include "obs-output.c"
#undef os_event_timedwait
#undef pthread_create
#undef pthread_join
#undef os_event_wait

#include <time.h>
#include "encoutput_api.h"

extern int pthread_join(pthread_t, void **);
extern int os_event_wait(os_event_t *event);
extern int os_event_timedwait(os_event_t *event, unsigned long milliseconds);
extern int pthread_create(pthread_t *restrict, const pthread_attr_t *restrict, void *(*)(void *), void *restrict);

volatile long eo_reconnect_created;
volatile long eo_waits_recorded;
volatile long eo_waits_done;
volatile long eo_threads_finished;
volatile long eo_gate;
uint32_t eo_wait_ms[64];

/* The end-of-capture thread is held back until the script command that created it has finished, so that the
 * order of its effects relative to the main thread's output is reproducible. */
volatile int eo_release;
struct tramp {
	void *(*fn)(void *);
	void *arg;
};
static void *tramp_main(void *p)
{
	struct tramp t = *(struct tramp *)p;
	free(p);
	while (!eo_release) {
		struct timespec ts = {0, 100000};
		nanosleep(&ts, NULL);
	}
	return t.fn(t.arg);
}

struct rtramp {
	void *(*fn)(void *);
	void *arg;
};
static void *rtramp_main(void *p)
{
	struct rtramp t = *(struct rtramp *)p;
	free(p);
	void *r = t.fn(t.arg);
	__sync_fetch_and_add(&eo_threads_finished, 1);
	return r;
}

int eo_pthread_create(pthread_t *restrict t, const pthread_attr_t *restrict a, void *(*fn)(void *), void *restrict arg)
{
	if (fn == reconnect_thread) {
		__sync_fetch_and_add(&eo_reconnect_created, 1);
		struct rtramp *tr = malloc(sizeof(*tr));
		tr->fn = fn;
		tr->arg = arg;
		return pthread_create(t, a, rtramp_main, tr);
	}
	if (fn == end_data_capture_thread) {
		struct tramp *tr = malloc(sizeof(*tr));
		tr->fn = fn;
		tr->arg = arg;
		return pthread_create(t, a, tramp_main, tr);
	}
	return pthread_create(t, a, fn, arg);
}

/* The reconnect timer never really elapses: it "expires" (ETIMEDOUT) only when the script posts the gate,
 * and returns 0 when the stop event is signalled first. The requested time is recorded. */
int eo_event_timedwait(os_event_t *event, unsigned long ms)
{
	long n = eo_waits_recorded;
	if (n < 64)
		eo_wait_ms[n] = (uint32_t)ms;
	__sync_fetch_and_add(&eo_waits_recorded, 1);
	for (;;) {
		if (os_event_try(event) == 0) {
			__sync_fetch_and_add(&eo_waits_done, 1);
			return 0;
		}
		long g = eo_gate;
		if (g > 0 && __sync_bool_compare_and_swap(&eo_gate, g, g - 1)) {
			__sync_fetch_and_add(&eo_waits_done, 1);
			return ETIMEDOUT;
		}
		struct timespec ts = {0, 200000};
		nanosleep(&ts, NULL);
	}
}

/* A blocking wait or join in the main thread lets the held-back end-of-capture thread run (it is what the
 * main thread is waiting for). */
int eo_event_wait(os_event_t *event)
{
	eo_release = 1;
	return os_event_wait(event);
}
int eo_pthread_join(pthread_t t, void **ret)
{
	eo_release = 1;
	return pthread_join(t, ret);
}
