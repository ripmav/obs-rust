/*
 * Conformance harness (area scripting, spec S-039): FAKE "lua" language backend.
 *
 * obs-scripting.c dispatches by file extension to obs_lua_script_* functions. Here those functions are
 * implemented by a tiny fake that follows the contract the real Lua backend honours towards the core
 * (path/file split, settings copy, load-on-create, description, update-after-load) and reports every
 * call as a "backend ..." line. Virtual scripts are declared by the conformance script (fake_define).
 * Also supplies the few platform functions the core links against.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <util/bmem.h>
#include <util/dstr.h>
#include <util/base.h>
#include <obs.h>
#include "obs-scripting-internal.h"
#include "scripting_fake.h"

int os_rename(const char *old_path, const char *new_path)
{
	return rename(old_path, new_path);
}

char *os_quick_read_utf8_file(const char *path)
{
	(void)path;
	abort();
}
bool os_quick_write_utf8_file(const char *path, const char *str, size_t len, bool marker)
{
	(void)path, (void)str, (void)len, (void)marker;
	abort();
}
bool os_quick_write_utf8_file_safe(const char *path, const char *str, size_t len, bool marker, const char *temp_ext,
				   const char *backup_ext)
{
	(void)path, (void)str, (void)len, (void)marker, (void)temp_ext, (void)backup_ext;
	abort();
}

struct fake_def fake_defs[FAKE_MAX_DEFS];
int fake_ndefs;
int (*fake_deferred_counter)(void);

static struct fake_def *find_def(const char *path)
{
	for (int i = 0; i < fake_ndefs; i++)
		if (!strcmp(fake_defs[i].path, path))
			return &fake_defs[i];
	return NULL;
}

struct fake_script {
	obs_script_t base;
	struct fake_def *def;
};

void obs_lua_load(void)
{
	printf("backend lua_load\n");
}
void obs_lua_unload(void)
{
	printf("backend lua_unload\n");
}

void obs_lua_script_update(obs_script_t *s, obs_data_t *settings);

bool obs_lua_script_load(obs_script_t *s)
{
	struct fake_script *f = (struct fake_script *)s;
	if (!s->loaded) {
		s->loaded = f->def ? f->def->load_ok : true;
		printf("backend load %s -> %d deferred_done=%d\n", s->file.array, s->loaded ? 1 : 0,
		       fake_deferred_counter ? fake_deferred_counter() : 0);
		if (s->loaded) {
			if (f->def && f->def->desc[0])
				dstr_copy(&s->desc, f->def->desc);
			obs_lua_script_update(s, NULL);
		}
	}
	return s->loaded;
}

obs_script_t *obs_lua_script_create(const char *path, obs_data_t *settings)
{
	struct fake_script *f = bzalloc(sizeof(*f));
	f->base.type = OBS_SCRIPT_LANG_LUA;
	f->def = find_def(path);
	dstr_copy(&f->base.path, path);
	const char *slash = *path ? strrchr(path, '/') : NULL;
	dstr_copy(&f->base.file, slash ? slash + 1 : path);
	f->base.settings = obs_data_create();
	if (settings)
		obs_data_apply(f->base.settings, settings);
	printf("backend create %s\n", path);
	obs_lua_script_load(&f->base);
	return &f->base;
}

void obs_lua_script_unload(obs_script_t *s)
{
	if (!s->loaded)
		return;
	printf("backend unload %s\n", s->file.array);
	s->loaded = false;
}

void obs_lua_script_destroy(obs_script_t *s)
{
	if (!s)
		return;
	printf("backend destroy %s\n", s->file.array);
	dstr_free(&s->path);
	dstr_free(&s->file);
	dstr_free(&s->desc);
	obs_data_release(s->settings);
	bfree(s);
}

void obs_lua_script_update(obs_script_t *s, obs_data_t *settings)
{
	if (!s->loaded)
		return;
	if (settings)
		obs_data_apply(s->settings, settings);
	printf("backend update %s k=\"%s\"\n", s->file.array, obs_data_get_string(s->settings, "k"));
}

obs_properties_t *obs_lua_script_get_properties(obs_script_t *s)
{
	struct fake_script *f = (struct fake_script *)s;
	if (!s->loaded)
		return NULL;
	if (!f->def || f->def->nprops < 0) {
		printf("backend get_properties %s -> NULL\n", s->file.array);
		return NULL;
	}
	printf("backend get_properties %s -> %d\n", s->file.array, f->def->nprops);
	obs_properties_t *p = obs_properties_create();
	for (int i = 0; i < f->def->nprops; i++) {
		char name[16];
		snprintf(name, sizeof(name), "p%d", i);
		obs_properties_add_text(p, name, name, OBS_TEXT_DEFAULT);
	}
	return p;
}

void obs_lua_script_save(obs_script_t *s)
{
	if (!s->loaded)
		return;
	printf("backend save %s\n", s->file.array);
	obs_data_set_string(s->settings, "saved", "1");
}
