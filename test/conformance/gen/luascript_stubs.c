/*
 * Conformance harness (area luascript, spec S-060): fake libobs surface + stand-ins for what SWIG would
 * generate, for the REAL shared/obs-scripting Lua backend. Test tooling, not part of libobs.
 *
 *  - os_quick_read_utf8_file: plain file read (the script file is written by the runner)
 *  - fake obs.h: video frame time, tick/render callback lists, sources, scene, hotkeys, source registry
 *  - fake obs-frontend-api
 *  - SWIG runtime stand-in (SWIG_TypeQuery/ConvertPtr/NewPointerObj) and a mini "obslua" module that the
 *    reference backend expects `require "obslua"` to provide (the real one is SWIG output)
 *  - luaL_openlibs interposer: makes `require "obslua"` find our module through an extra package loader,
 *    because the startup script of the backend clears package.preload before requiring it
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include <util/bmem.h>
#include <util/base.h>
#include <callback/calldata.h>
#include <callback/signal.h>
#include <obs.h>
#include <obs-module.h>
#include <obs-source.h>
#include "luascript_fake.h"
#include "swig/swigluarun.h"

/* ------------------------------------------------------------------ */
/* platform                                                            */

/* fake executable directory: "/fakeroot/bin/" + name (the real one resolves against /proc/self/exe) */
char *os_get_executable_path_ptr(const char *name)
{
	size_t n = strlen("/fakeroot/bin/") + (name ? strlen(name) : 0) + 1;
	char *s = bmalloc(n);
	snprintf(s, n, "/fakeroot/bin/%s", name ? name : "");
	return s;
}

int os_rename(const char *old_path, const char *new_path)
{
	return rename(old_path, new_path);
}

char *os_quick_read_utf8_file(const char *path)
{
	FILE *f = path ? fopen(path, "rb") : NULL;
	if (!f)
		return NULL;
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	char *buf = bmalloc((size_t)n + 1);
	size_t got = fread(buf, 1, (size_t)n, f);
	buf[got] = 0;
	fclose(f);
	return buf;
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

/* ------------------------------------------------------------------ */
/* fake libobs state                                                   */

struct fake_type lf_types[LF_MAX_TYPES];
struct obs_source lf_sources[LF_MAX_SRC];
int lf_nsources;
struct obs_scene lf_scene;
struct obs_scene_item lf_items[4];
uint64_t lf_video_time;
struct lf_tick lf_ticks[LF_MAX_CB];
int lf_nticks;
struct lf_render lf_renders[LF_MAX_CB];
int lf_nrenders;
struct lf_hotkey lf_hotkeys[LF_MAX_CB];
int lf_nhotkeys;
signal_handler_t *lf_handlers[4];
obs_frontend_event_cb lf_fe_event_cb[LF_MAX_CB];
void *lf_fe_event_p[LF_MAX_CB];
int lf_fe_nevent;
obs_frontend_save_cb lf_fe_save_cb[LF_MAX_CB];
void *lf_fe_save_p[LF_MAX_CB];
int lf_fe_nsave;
int lf_fe_duration = 300;
char lf_fe_scene[32] = "Scene A", lf_fe_collection[32] = "Default", lf_fe_profile[32] = "Untitled";

void lf_reset_fakes(void)
{
	lf_nticks = lf_nrenders = lf_nhotkeys = lf_fe_nevent = lf_fe_nsave = 0;
	lf_video_time = 0;
}

uint64_t obs_get_video_frame_time(void)
{
	return lf_video_time;
}

void obs_add_tick_callback(void (*tick)(void *param, float seconds), void *param)
{
	if (lf_nticks < LF_MAX_CB)
		lf_ticks[lf_nticks++] = (struct lf_tick){tick, param};
}
void obs_remove_tick_callback(void (*tick)(void *param, float seconds), void *param)
{
	for (int i = 0; i < lf_nticks; i++)
		if (lf_ticks[i].fn == tick && lf_ticks[i].p == param) {
			memmove(&lf_ticks[i], &lf_ticks[i + 1], sizeof(lf_ticks[0]) * (size_t)(lf_nticks - i - 1));
			lf_nticks--;
			return;
		}
}
void obs_add_main_render_callback(void (*draw)(void *param, uint32_t cx, uint32_t cy), void *param)
{
	if (lf_nrenders < LF_MAX_CB)
		lf_renders[lf_nrenders++] = (struct lf_render){draw, param};
}
void obs_remove_main_render_callback(void (*draw)(void *param, uint32_t cx, uint32_t cy), void *param)
{
	for (int i = 0; i < lf_nrenders; i++)
		if (lf_renders[i].fn == draw && lf_renders[i].p == param) {
			memmove(&lf_renders[i], &lf_renders[i + 1],
				sizeof(lf_renders[0]) * (size_t)(lf_nrenders - i - 1));
			lf_nrenders--;
			return;
		}
}

/* sources ----------------------------------------------------------- */

void obs_register_source_s(const struct obs_source_info *info, size_t size)
{
	(void)size;
	for (int i = 0; i < LF_MAX_TYPES; i++)
		if (!lf_types[i].used) {
			lf_types[i].used = true;
			lf_types[i].enabled = true;
			lf_types[i].info = *info;
			return;
		}
}
void obs_module_add_source(obs_module_t *module, const char *id)
{
	(void)module;
	printf("fake obs_module_add_source %s\n", id);
}
obs_module_t *obs_current_module(void);
void obs_enable_source_type(const char *name, bool enable)
{
	for (int i = 0; i < LF_MAX_TYPES; i++)
		if (lf_types[i].used && !strcmp(lf_types[i].info.id, name))
			lf_types[i].enabled = enable;
	printf("fake obs_enable_source_type %s %d\n", name, enable ? 1 : 0);
}
void *obs_source_get_type_data(obs_source_t *source)
{
	return source->type ? source->type->info.type_data : NULL;
}
obs_data_t *obs_source_get_settings(const obs_source_t *source)
{
	obs_data_addref(source->settings);
	return source->settings;
}
const char *obs_source_get_name(const obs_source_t *source)
{
	return source->name;
}
obs_source_t *obs_source_get_ref(obs_source_t *source)
{
	source->refs++;
	return source;
}
void obs_source_release(obs_source_t *source)
{
	if (source)
		source->refs--;
}
void obs_enum_sources(bool (*enum_proc)(void *, obs_source_t *), void *param)
{
	for (int i = 0; i < lf_nsources; i++)
		if (!enum_proc(param, &lf_sources[i]))
			break;
}
void obs_source_enum_filters(obs_source_t *source,
			     void (*callback)(obs_source_t *parent, obs_source_t *child, void *param), void *param)
{
	for (int i = 0; i < source->nfilters; i++)
		callback(source, source->filters[i], param);
}
void obs_scene_enum_items(obs_scene_t *scene, bool (*callback)(obs_scene_t *, obs_sceneitem_t *, void *),
			  void *param)
{
	for (int i = 0; i < scene->nitems; i++)
		if (!callback(scene, &lf_items[i], param))
			break;
}
void obs_sceneitem_group_enum_items(obs_sceneitem_t *group,
				    bool (*callback)(obs_scene_t *, obs_sceneitem_t *, void *), void *param)
{
	(void)group;
	obs_scene_enum_items(&lf_scene, callback, param);
}
void obs_sceneitem_addref(obs_sceneitem_t *item)
{
	item->refs++;
}
void obs_sceneitem_release(obs_sceneitem_t *item)
{
	if (item)
		item->refs--;
}

/* hotkeys ----------------------------------------------------------- */

obs_hotkey_id obs_hotkey_register_frontend(const char *name, const char *description, obs_hotkey_func func,
					   void *data)
{
	(void)description;
	if (!strncmp(name, "bad", 3))
		return OBS_INVALID_HOTKEY_ID;
	if (lf_nhotkeys >= LF_MAX_CB)
		return OBS_INVALID_HOTKEY_ID;
	struct lf_hotkey *h = &lf_hotkeys[lf_nhotkeys++];
	h->func = func;
	h->data = data;
	h->live = true;
	snprintf(h->name, sizeof(h->name), "%s", name);
	return (obs_hotkey_id)lf_nhotkeys; /* ids start at 1 */
}
void obs_hotkey_unregister(obs_hotkey_id id)
{
	if (id >= 1 && id <= (obs_hotkey_id)lf_nhotkeys) {
		lf_hotkeys[id - 1].live = false;
		printf("fake obs_hotkey_unregister %s\n", lf_hotkeys[id - 1].name);
	}
}

/* frontend ----------------------------------------------------------- */

/* Like the frontend's own helper: one allocation holding the pointer array followed by the strings,
 * so that the backend's single bfree() releases everything. */
static char **dup_names(const char *a, const char *b)
{
	size_t la = strlen(a) + 1, lb = strlen(b) + 1;
	char **v = bmalloc(sizeof(char *) * 3 + la + lb);
	char *str = (char *)&v[3];
	memcpy(str, a, la);
	memcpy(str + la, b, lb);
	v[0] = str;
	v[1] = str + la;
	v[2] = NULL;
	return v;
}
char **obs_frontend_get_scene_names(void)
{
	return dup_names("Scene A", "Scene B");
}
void obs_frontend_get_scenes(struct obs_frontend_source_list *list)
{
	for (int i = 0; i < lf_nsources && i < 2; i++)
		da_push_back(list->sources, &(obs_source_t *){&lf_sources[i]});
}
obs_source_t *obs_frontend_get_current_scene(void)
{
	return lf_nsources ? &lf_sources[0] : NULL;
}
void obs_frontend_set_current_scene(obs_source_t *scene)
{
	printf("fake set_current_scene %s\n", scene ? scene->name : "(null)");
}
void obs_frontend_get_transitions(struct obs_frontend_source_list *list)
{
	for (int i = 0; i < lf_nsources && i < 1; i++)
		da_push_back(list->sources, &(obs_source_t *){&lf_sources[i]});
}
obs_source_t *obs_frontend_get_current_transition(void)
{
	return NULL;
}
void obs_frontend_set_current_transition(obs_source_t *t)
{
	printf("fake set_current_transition %s\n", t ? t->name : "(null)");
}
int obs_frontend_get_transition_duration(void)
{
	return lf_fe_duration;
}
void obs_frontend_set_transition_duration(int d)
{
	lf_fe_duration = d;
}
char **obs_frontend_get_scene_collections(void)
{
	return dup_names("Default", "Other");
}
char *obs_frontend_get_current_scene_collection(void)
{
	return bstrdup(lf_fe_collection);
}
void obs_frontend_set_current_scene_collection(const char *n)
{
	snprintf(lf_fe_collection, sizeof(lf_fe_collection), "%s", n);
}
char **obs_frontend_get_profiles(void)
{
	return dup_names("Untitled", "Second");
}
char *obs_frontend_get_current_profile(void)
{
	return bstrdup(lf_fe_profile);
}
void obs_frontend_set_current_profile(const char *n)
{
	snprintf(lf_fe_profile, sizeof(lf_fe_profile), "%s", n);
}
void obs_frontend_add_event_callback(obs_frontend_event_cb cb, void *p)
{
	if (lf_fe_nevent < LF_MAX_CB) {
		lf_fe_event_cb[lf_fe_nevent] = cb;
		lf_fe_event_p[lf_fe_nevent++] = p;
	}
}
void obs_frontend_remove_event_callback(obs_frontend_event_cb cb, void *p)
{
	for (int i = 0; i < lf_fe_nevent; i++)
		if (lf_fe_event_cb[i] == cb && lf_fe_event_p[i] == p) {
			lf_fe_event_cb[i] = NULL;
			printf("fake frontend event callback removed\n");
			return;
		}
}
void obs_frontend_add_save_callback(obs_frontend_save_cb cb, void *p)
{
	if (lf_fe_nsave < LF_MAX_CB) {
		lf_fe_save_cb[lf_fe_nsave] = cb;
		lf_fe_save_p[lf_fe_nsave++] = p;
	}
}
void obs_frontend_remove_save_callback(obs_frontend_save_cb cb, void *p)
{
	for (int i = 0; i < lf_fe_nsave; i++)
		if (lf_fe_save_cb[i] == cb && lf_fe_save_p[i] == p) {
			lf_fe_save_cb[i] = NULL;
			printf("fake frontend save callback removed\n");
			return;
		}
}

/* ------------------------------------------------------------------ */
/* SWIG runtime stand-in                                               */

static swig_type_info swig_types[] = {
	{"obs_data_t *", 0},     {"obs_source_t *", 0},     {"calldata_t *", 0},    {"signal_handler_t *", 0},
	{"obs_properties_t *", 0}, {"obs_property_t *", 0}, {"gs_effect_t *", 0},   {"obs_scene_t *", 0},
	{"obs_sceneitem_t *", 0},
};

bool lf_hide_type(const char *name, int hide)
{
	for (size_t i = 0; i < sizeof(swig_types) / sizeof(swig_types[0]); i++)
		if (!strcmp(swig_types[i].name, name)) {
			swig_types[i].hidden = hide;
			return true;
		}
	return false;
}

swig_type_info *SWIG_TypeQuery(lua_State *L, const char *name)
{
	(void)L;
	for (size_t i = 0; i < sizeof(swig_types) / sizeof(swig_types[0]); i++)
		if (!strcmp(swig_types[i].name, name))
			return swig_types[i].hidden ? NULL : &swig_types[i];
	return NULL;
}

struct swig_ud {
	void *ptr;
	swig_type_info *type;
};

int SWIG_ConvertPtr(lua_State *L, int idx, void *ptr, swig_type_info *type, int flags)
{
	(void)flags;
	if (lua_isnil(L, idx)) {
		*(void **)ptr = NULL;
		return SWIG_OK;
	}
	if (!lua_isuserdata(L, idx))
		return SWIG_ERROR;
	struct swig_ud *ud = lua_touserdata(L, idx);
	if (ud->type != type)
		return SWIG_ERROR;
	*(void **)ptr = ud->ptr;
	return SWIG_OK;
}

void SWIG_NewPointerObj(lua_State *L, void *ptr, swig_type_info *type, int own)
{
	(void)own;
	if (!ptr) {
		lua_pushnil(L);
		return;
	}
	struct swig_ud *ud = lua_newuserdata(L, sizeof(*ud));
	ud->ptr = ptr;
	ud->type = type;
}

static void push_typed(lua_State *L, const char *tname, void *p)
{
	SWIG_NewPointerObj(L, p, SWIG_TypeQuery(L, tname), 0);
}
static void *get_typed(lua_State *L, int idx, const char *tname)
{
	void *p = NULL;
	if (!SWIG_IsOK(SWIG_ConvertPtr(L, idx, &p, SWIG_TypeQuery(L, tname), 0)))
		luaL_error(L, "bad argument %d (expected %s)", idx, tname);
	return p;
}

/* ------------------------------------------------------------------ */
/* mini "obslua" module                                                */

#define DATA(i) ((obs_data_t *)get_typed(L, i, "obs_data_t *"))
#define PROPS(i) ((obs_properties_t *)get_typed(L, i, "obs_properties_t *"))

static int l_data_set_string(lua_State *L)
{
	obs_data_set_string(DATA(1), luaL_checkstring(L, 2), luaL_checkstring(L, 3));
	return 0;
}
static int l_data_set_int(lua_State *L)
{
	obs_data_set_int(DATA(1), luaL_checkstring(L, 2), (long long)luaL_checknumber(L, 3));
	return 0;
}
static int l_data_set_bool(lua_State *L)
{
	obs_data_set_bool(DATA(1), luaL_checkstring(L, 2), lua_toboolean(L, 3));
	return 0;
}
static int l_data_set_double(lua_State *L)
{
	obs_data_set_double(DATA(1), luaL_checkstring(L, 2), luaL_checknumber(L, 3));
	return 0;
}
static int l_data_set_default_string(lua_State *L)
{
	obs_data_set_default_string(DATA(1), luaL_checkstring(L, 2), luaL_checkstring(L, 3));
	return 0;
}
static int l_data_set_default_int(lua_State *L)
{
	obs_data_set_default_int(DATA(1), luaL_checkstring(L, 2), (long long)luaL_checknumber(L, 3));
	return 0;
}
static int l_data_get_string(lua_State *L)
{
	lua_pushstring(L, obs_data_get_string(DATA(1), luaL_checkstring(L, 2)));
	return 1;
}
static int l_data_get_int(lua_State *L)
{
	lua_pushnumber(L, (lua_Number)obs_data_get_int(DATA(1), luaL_checkstring(L, 2)));
	return 1;
}
static int l_data_get_bool(lua_State *L)
{
	lua_pushboolean(L, obs_data_get_bool(DATA(1), luaL_checkstring(L, 2)));
	return 1;
}
static int l_data_get_double(lua_State *L)
{
	lua_pushnumber(L, obs_data_get_double(DATA(1), luaL_checkstring(L, 2)));
	return 1;
}
static int l_data_has_user_value(lua_State *L)
{
	lua_pushboolean(L, obs_data_has_user_value(DATA(1), luaL_checkstring(L, 2)));
	return 1;
}
static int l_properties_create(lua_State *L)
{
	push_typed(L, "obs_properties_t *", obs_properties_create());
	return 1;
}
static int l_properties_add_text(lua_State *L)
{
	obs_property_t *p = obs_properties_add_text(PROPS(1), luaL_checkstring(L, 2), luaL_checkstring(L, 3),
						    (enum obs_text_type)luaL_checkinteger(L, 4));
	push_typed(L, "obs_property_t *", p);
	return 1;
}
static int l_properties_add_int(lua_State *L)
{
	obs_property_t *p = obs_properties_add_int(PROPS(1), luaL_checkstring(L, 2), luaL_checkstring(L, 3),
						   luaL_checkinteger(L, 4), luaL_checkinteger(L, 5),
						   luaL_checkinteger(L, 6));
	push_typed(L, "obs_property_t *", p);
	return 1;
}
static int l_properties_add_bool(lua_State *L)
{
	push_typed(L, "obs_property_t *",
		   obs_properties_add_bool(PROPS(1), luaL_checkstring(L, 2), luaL_checkstring(L, 3)));
	return 1;
}
static int l_property_set_description(lua_State *L)
{
	obs_property_set_description(get_typed(L, 1, "obs_property_t *"), luaL_checkstring(L, 2));
	return 0;
}
static int l_property_set_enabled(lua_State *L)
{
	obs_property_set_enabled(get_typed(L, 1, "obs_property_t *"), lua_toboolean(L, 2));
	return 0;
}
static int l_properties_get(lua_State *L)
{
	push_typed(L, "obs_property_t *", obs_properties_get(PROPS(1), luaL_checkstring(L, 2)));
	return 1;
}
static int l_calldata_get_string(lua_State *L)
{
	calldata_t *cd = get_typed(L, 1, "calldata_t *");
	const char *s = NULL;
	if (calldata_get_string(cd, luaL_checkstring(L, 2), &s) && s)
		lua_pushstring(L, s);
	else
		lua_pushnil(L);
	return 1;
}
static int l_calldata_get_int(lua_State *L)
{
	calldata_t *cd = get_typed(L, 1, "calldata_t *");
	lua_pushnumber(L, (lua_Number)calldata_int(cd, luaL_checkstring(L, 2)));
	return 1;
}
static int l_source_get_name(lua_State *L)
{
	obs_source_t *s = get_typed(L, 1, "obs_source_t *");
	lua_pushstring(L, s->name);
	return 1;
}
static int l_sceneitem_get_id(lua_State *L)
{
	obs_sceneitem_t *it = get_typed(L, 1, "obs_sceneitem_t *");
	lua_pushinteger(L, it->id);
	return 1;
}
static int l_get_video_frame_time(lua_State *L)
{
	lua_pushnumber(L, (lua_Number)lf_video_time);
	return 1;
}
/* fixture accessors (not libobs API) */
static int l_fake_handler(lua_State *L)
{
	int n = (int)luaL_checkinteger(L, 1);
	push_typed(L, "signal_handler_t *", n >= 0 && n < 4 ? lf_handlers[n] : NULL);
	return 1;
}
static int l_fake_source(lua_State *L)
{
	int n = (int)luaL_checkinteger(L, 1);
	push_typed(L, "obs_source_t *", n >= 0 && n < lf_nsources ? &lf_sources[n] : NULL);
	return 1;
}
static int l_fake_scene(lua_State *L)
{
	push_typed(L, "obs_scene_t *", &lf_scene);
	return 1;
}
static int l_fake_sceneitem(lua_State *L)
{
	push_typed(L, "obs_sceneitem_t *", &lf_items[0]);
	return 1;
}
static int l_fake_effect(lua_State *L)
{
	static int dummy;
	push_typed(L, "gs_effect_t *", &dummy);
	return 1;
}
static int l_fake_is_userdata(lua_State *L)
{
	lua_pushboolean(L, lua_isuserdata(L, 1));
	return 1;
}

#define ENTRY(n, f) {n, f}
static const luaL_Reg obslua_funcs[] = {
	ENTRY("obs_data_set_string", l_data_set_string),
	ENTRY("obs_data_set_int", l_data_set_int),
	ENTRY("obs_data_set_bool", l_data_set_bool),
	ENTRY("obs_data_set_double", l_data_set_double),
	ENTRY("obs_data_set_default_string", l_data_set_default_string),
	ENTRY("obs_data_set_default_int", l_data_set_default_int),
	ENTRY("obs_data_get_string", l_data_get_string),
	ENTRY("obs_data_get_int", l_data_get_int),
	ENTRY("obs_data_get_bool", l_data_get_bool),
	ENTRY("obs_data_get_double", l_data_get_double),
	ENTRY("obs_data_has_user_value", l_data_has_user_value),
	ENTRY("obs_properties_create", l_properties_create),
	ENTRY("obs_properties_add_text", l_properties_add_text),
	ENTRY("obs_properties_add_int", l_properties_add_int),
	ENTRY("obs_properties_add_bool", l_properties_add_bool),
	ENTRY("obs_property_set_description", l_property_set_description),
	ENTRY("obs_property_set_enabled", l_property_set_enabled),
	ENTRY("obs_properties_get", l_properties_get),
	ENTRY("calldata_get_string", l_calldata_get_string),
	ENTRY("calldata_get_int", l_calldata_get_int),
	ENTRY("obs_source_get_name", l_source_get_name),
	ENTRY("obs_sceneitem_get_id", l_sceneitem_get_id),
	ENTRY("obs_get_video_frame_time", l_get_video_frame_time),
	ENTRY("fake_handler", l_fake_handler),
	ENTRY("fake_source", l_fake_source),
	ENTRY("fake_scene", l_fake_scene),
	ENTRY("fake_sceneitem", l_fake_sceneitem),
	ENTRY("fake_effect", l_fake_effect),
	ENTRY("fake_is_userdata", l_fake_is_userdata),
	{NULL, NULL},
};

static void set_int(lua_State *L, const char *name, lua_Integer v)
{
	lua_pushinteger(L, v);
	lua_setfield(L, -2, name);
}

int luaopen_obslua(lua_State *L)
{
	luaL_register(L, "obslua", obslua_funcs); /* also creates the global, like a SWIG module */
	set_int(L, "LOG_ERROR", LOG_ERROR);
	set_int(L, "LOG_WARNING", LOG_WARNING);
	set_int(L, "LOG_INFO", LOG_INFO);
	set_int(L, "LOG_DEBUG", LOG_DEBUG);
	set_int(L, "OBS_SOURCE_TYPE_INPUT", OBS_SOURCE_TYPE_INPUT);
	set_int(L, "OBS_SOURCE_TYPE_FILTER", OBS_SOURCE_TYPE_FILTER);
	set_int(L, "OBS_SOURCE_VIDEO", OBS_SOURCE_VIDEO);
	set_int(L, "OBS_SOURCE_AUDIO", OBS_SOURCE_AUDIO);
	set_int(L, "OBS_SOURCE_CUSTOM_DRAW", OBS_SOURCE_CUSTOM_DRAW);
	set_int(L, "OBS_TEXT_DEFAULT", OBS_TEXT_DEFAULT);
	return 1;
}

static int obslua_loader(lua_State *L)
{
	const char *name = luaL_checkstring(L, 1);
	if (!strcmp(name, "obslua"))
		lua_pushcfunction(L, luaopen_obslua);
	else
		lua_pushfstring(L, "\n\tno fake module '%s'", name);
	return 1;
}

/* Interposes the libluajit symbol: opens the standard libraries, then appends our loader to package.loaders. */
void luaL_openlibs(lua_State *L)
{
	static void (*real)(lua_State *);
	if (!real)
		real = (void (*)(lua_State *))dlsym(RTLD_NEXT, "luaL_openlibs");
	real(L);
	lua_getglobal(L, "package");
	lua_getfield(L, -1, "loaders");
	lua_pushcfunction(L, obslua_loader);
	lua_rawseti(L, -2, (int)lua_objlen(L, -2) + 1);
	lua_pop(L, 2);
}
