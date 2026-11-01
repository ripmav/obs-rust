/*
 * Conformance harness (area hotkeys, spec S-034): recording fakes for everything obs-hotkey.c
 * reaches outside the hotkey system: signal handler, weak references of encoders/outputs/
 * services/sources, the platform key-state backend, the profiler and thread naming.
 * Test tooling, not part of libobs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include "obs-internal.h"
#include "callback/signal.h"
#include "util/profiler.h"

/* the global core pointer that libobs code dereferences (NULL = "core not initialised") */
struct obs_core *obs;

/* ---- platform backend: scripted set of pressed keys ---- */
bool fake_pressed[OBS_KEY_LAST_VALUE + 1];

bool obs_hotkeys_platform_is_pressed(obs_hotkeys_platform_t *context, obs_key_t key)
{
	(void)context;
	return key < OBS_KEY_LAST_VALUE && fake_pressed[key];
}

bool obs_hotkeys_platform_init(struct obs_core_hotkeys *h)
{
	(void)h;
	return true;
}
void obs_hotkeys_platform_free(struct obs_core_hotkeys *h)
{
	(void)h;
}

/* ---- signals: each emission is printed ---- */
void signal_handler_signal(signal_handler_t *handler, const char *signal, calldata_t *params)
{
	(void)handler;
	void *key = NULL;
	calldata_get_ptr(params, "key", &key);
	printf("signal %s id=", signal);
	obs_hotkey_id id = obs_hotkey_get_id(key);
	if (id == (size_t)-1)
		printf("INVALID");
	else
		printf("%zu", id);
	printf("\n");
}

/* ---- weak references: counted, never dereferenced ---- */
int fake_weak_live;
int fake_weak_null_releases;

static void *weak_new(void)
{
	fake_weak_live++;
	return malloc(1);
}
static void weak_release(void *w)
{
	if (!w) {
		fake_weak_null_releases++;
		return;
	}
	fake_weak_live--;
	free(w);
}

obs_weak_encoder_t *obs_encoder_get_weak_encoder(obs_encoder_t *e)
{
	(void)e;
	return weak_new();
}
obs_weak_output_t *obs_output_get_weak_output(obs_output_t *o)
{
	(void)o;
	return weak_new();
}
obs_weak_service_t *obs_service_get_weak_service(obs_service_t *s)
{
	(void)s;
	return weak_new();
}
obs_weak_source_t *obs_source_get_weak_source(obs_source_t *s)
{
	(void)s;
	return weak_new();
}
void obs_weak_encoder_release(obs_weak_encoder_t *w)
{
	weak_release(w);
}
void obs_weak_output_release(obs_weak_output_t *w)
{
	weak_release(w);
}
void obs_weak_service_release(obs_weak_service_t *w)
{
	weak_release(w);
}
void obs_weak_source_release(obs_weak_source_t *w)
{
	weak_release(w);
}

/* ---- hotkey thread support (the thread is not started by the runner) ---- */
profiler_name_store_t *obs_get_profiler_name_store(void)
{
	return NULL;
}
const char *profile_store_name(profiler_name_store_t *store, const char *format, ...)
{
	(void)store;
	return format;
}
void profile_register_root(const char *name, uint64_t expected_time_between_calls)
{
	(void)name;
	(void)expected_time_between_calls;
}
void profile_start(const char *name)
{
	(void)name;
}
void profile_end(const char *name)
{
	(void)name;
}
void profile_reenable_thread(void) {}
