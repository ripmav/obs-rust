/*
 * Fake libobs core for spec S-035 (service + module loader): only what obs-service.c and obs-module.c reach.
 * Test tooling, not part of libobs. The context layer mirrors the reference obs_context_data_* behaviour that the
 * service code depends on (name defaulting, settings ref, control block, list insertion).
 */
#include <obs-internal.h>
#include <obs-core-modules.h>
#include <util/platform.h>
#include <util/profiler.h>
#include "modservice_api.h"

struct obs_core *obs;
static struct obs_core fake_core;

void ms_core_init(void)
{
	memset(&fake_core, 0, sizeof(fake_core));
	pthread_mutex_init(&fake_core.data.services_mutex, NULL);
	obs = &fake_core;
}

void ms_core_set(const char *locale, const char *cfg)
{
	bfree(fake_core.locale);
	bfree(fake_core.module_config_path);
	fake_core.locale = locale ? bstrdup(locale) : NULL;
	fake_core.module_config_path = cfg ? bstrdup(cfg) : NULL;
}

void ms_core_null(bool null_core)
{
	obs = null_core ? NULL : &fake_core;
}

struct obs_core *ms_core(void)
{
	return &fake_core;
}

const char *get_module_extension(void)
{
	return ".so";
}

/* core module list: configured by the script (the real list is generated at build time) */
static char *core_names[32];
static unsigned core_count;
static char *core_bin_tpl, *core_data_tpl;

const char *obs_core_modules[1];
const unsigned int obs_core_modules_count = 0;

void ms_core_modules_set(char **names, int n, const char *bin_tpl, const char *data_tpl)
{
	for (unsigned i = 0; i < core_count; i++)
		bfree(core_names[i]);
	core_count = 0;
	for (int i = 0; i < n && i < 32; i++)
		core_names[core_count++] = bstrdup(names[i]);
	bfree(core_bin_tpl);
	bfree(core_data_tpl);
	core_bin_tpl = bstrdup(bin_tpl);
	core_data_tpl = bstrdup(data_tpl);
}

bool obs_in_core_module_list(const char *name)
{
	for (unsigned i = 0; i < core_count; i++)
		if (strcmp(core_names[i], name) == 0)
			return true;
	return false;
}

extern bool find_core_module(struct obs_runtime_module_info *info, obs_find_module_callback2_t callback, void *data);

/* Same algorithm as obs-core-modules-loader-nix.c, with the executable-relative prefix replaced by script paths. */
void load_core_modules(obs_find_module_callback2_t callback, void *data)
{
	for (unsigned i = 0; i < core_count; i++) {
		const char *name = core_names[i];
		struct dstr bin_path, data_path;
		dstr_init_copy(&bin_path, core_bin_tpl);
		dstr_init_copy(&data_path, core_data_tpl);
		dstr_replace(&bin_path, "%module%", name);
		dstr_replace(&data_path, "%module%", name);

		struct obs_runtime_module_info mi = {.path_info = {.binary = bin_path.array, .data = data_path.array},
						     .type = OBS_MODULE_TYPE_CORE,
						     .name = name};
		bool found = find_core_module(&mi, callback, data);
		dstr_free(&bin_path);
		dstr_free(&data_path);
		if (!found) {
			blog(LOG_ERROR, "Failed to load core module %s", name);
			break;
		}
	}
}

/* ------------------------------------------------------------ profiler: no-ops */
profiler_name_store_t *obs_get_profiler_name_store(void)
{
	return NULL;
}
const char *profile_store_name(profiler_name_store_t *store, const char *format, ...)
{
	(void)store;
	(void)format;
	return "";
}
void profile_start(const char *name)
{
	(void)name;
}
void profile_end(const char *name)
{
	(void)name;
}

/* ------------------------------------------------------------ type lookups owned by other files */
struct obs_source_info *get_source_info2(const char *unversioned_id, uint32_t ver)
{
	for (size_t i = 0; i < obs->source_types.num; i++) {
		struct obs_source_info *info = &obs->source_types.array[i];
		if (strcmp(info->unversioned_id, unversioned_id) == 0 && info->version == ver)
			return info;
	}
	return NULL;
}

const struct obs_output_info *find_output(const char *id)
{
	for (size_t i = 0; i < obs->output_types.num; i++)
		if (strcmp(obs->output_types.array[i].id, id) == 0)
			return obs->output_types.array + i;
	return NULL;
}

struct obs_encoder_info *find_encoder(const char *id)
{
	for (size_t i = 0; i < obs->encoder_types.num; i++)
		if (strcmp(obs->encoder_types.array[i].id, id) == 0)
			return obs->encoder_types.array + i;
	return NULL;
}


/* ------------------------------------------------------------ context layer */
static char *dup_name(const char *name, bool private)
{
	if (private && !name)
		return NULL;
	if (!name || !*name) {
		struct dstr unnamed = {0};
		dstr_printf(&unnamed, "__unnamed%04lld", obs->data.unnamed_index++);
		return unnamed.array;
	}
	return bstrdup(name);
}

void obs_hotkeys_context_release(struct obs_context_data *context)
{
	(void)context;
}

void obs_context_data_remove(struct obs_context_data *context)
{
	if (context && context->prev_next) {
		pthread_mutex_lock(context->mutex);
		*context->prev_next = context->next;
		if (context->next)
			context->next->prev_next = context->prev_next;
		context->prev_next = NULL;
		pthread_mutex_unlock(context->mutex);
	}
}

void obs_context_data_free(struct obs_context_data *context)
{
	obs_hotkeys_context_release(context);
	signal_handler_destroy(context->signals);
	proc_handler_destroy(context->procs);
	obs_data_release(context->settings);
	obs_context_data_remove(context);
	pthread_mutex_destroy(&context->rename_cache_mutex);
	bfree(context->name);
	bfree((void *)context->uuid);
	for (size_t i = 0; i < context->rename_cache.num; i++)
		bfree(context->rename_cache.array[i]);
	da_free(context->rename_cache);
	memset(context, 0, sizeof(*context));
}

bool obs_context_data_init(struct obs_context_data *context, enum obs_obj_type type, obs_data_t *settings,
			   const char *name, const char *uuid, obs_data_t *hotkey_data, bool private)
{
	(void)uuid;
	memset(context, 0, sizeof(*context));
	context->private = private;
	context->type = type;
	pthread_mutex_init(&context->rename_cache_mutex, NULL);
	context->signals = signal_handler_create();
	context->procs = proc_handler_create();
	context->name = dup_name(name, private);
	context->settings = obs_data_newref(settings);
	context->hotkey_data = obs_data_newref(hotkey_data);
	return true;
}

void obs_context_init_control(struct obs_context_data *context, void *object, obs_destroy_cb destroy)
{
	context->control = bzalloc(sizeof(obs_weak_object_t));
	context->control->object = object;
	context->destroy = destroy;
}

void obs_context_data_insert(struct obs_context_data *context, pthread_mutex_t *mutex, void *pfirst)
{
	struct obs_context_data **first = pfirst;
	context->mutex = mutex;
	pthread_mutex_lock(mutex);
	context->prev_next = first;
	context->next = *first;
	*first = context;
	if (context->next)
		context->next->prev_next = &context->next;
	pthread_mutex_unlock(mutex);
}

/* platform.c references the video info (frame timing helpers) */
bool obs_get_video_info(struct obs_video_info *ovi)
{
	(void)ovi;
	return false;
}
