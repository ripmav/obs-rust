/*
 * Conformance harness (area pyscript, spec S-061): fake libobs surface + stand-ins for what SWIG would
 * generate for the Python backend (shared/obs-scripting/obs-scripting-python*.c):
 *  - platform functions (executable path, absolute path, file read)
 *  - fake libobs: tick list, scene/source/sceneitem fixtures, hotkeys, frontend API
 *  - SWIG runtime stand-in (SWIG_TypeQuery/ConvertPtr/NewPointerObj on PyCapsule) and a mini built-in
 *    "obspython" module (the real one is SWIG output; the backend adds its own hook functions to it)
 * Everything else the backend uses (obs-data, obs-properties, calldata, signal handler) is the real libobs code.
 * Test tooling, not part of libobs.
 */
#include <Python.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <util/bmem.h>
#include <util/base.h>
#include <callback/calldata.h>
#include <callback/signal.h>
#include <obs.h>
#include <obs-module.h>
#include <obs-source.h>
#include "pyscript_fake.h"
#include "swig/swigpyrun.h"

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
/* A deferred hotkey unregister runs on the scripting worker thread, which can
 * print it before the main thread's "Unloaded python script" log (no sync
 * between the two). To keep vectors deterministic, non-main-thread
 * unregistrations are recorded here and printed by the runner's barrier(). */
pthread_t lf_main_thread;
static char lf_unregs[LF_MAX_CB][64];
static int lf_nunregs;
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
		if (pthread_self() == lf_main_thread) {
			printf("fake obs_hotkey_unregister %s\n", lf_hotkeys[id - 1].name);
		} else {
			/* single worker thread; barrier()'s ping orders it before the flush */
			snprintf(lf_unregs[lf_nunregs++], 64, "%s", lf_hotkeys[id - 1].name);
		}
	}
}
void lf_flush_hotkey_unregs(void)
{
	for (int i = 0; i < lf_nunregs; i++)
		printf("fake obs_hotkey_unregister %s\n", lf_unregs[i]);
	lf_nunregs = 0;
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
/* more platform                                                       */

/* fake absolute path: "/fakeroot/abs/" + name (the real one is realpath-like and fails for missing paths) */
char *os_get_abs_path_ptr(const char *path)
{
	size_t n = strlen("/fakeroot/abs/") + (path ? strlen(path) : 0) + 1;
	char *s = bmalloc(n);
	snprintf(s, n, "/fakeroot/abs/%s", path ? path : "");
	return s;
}

/* ------------------------------------------------------------------ */
/* SWIG runtime stand-in: pointers are PyCapsules named after the C type; None <-> NULL            */

static swig_type_info swig_types[] = {
	{"obs_data_t *", 0},       {"obs_source_t *", 0},   {"calldata_t *", 0},
	{"signal_handler_t *", 0}, {"obs_properties_t *", 0}, {"obs_property_t *", 0},
	{"obs_scene_t *", 0},      {"obs_sceneitem_t *", 0},
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

swig_type_info *SWIG_TypeQuery(const char *name)
{
	for (size_t i = 0; i < sizeof(swig_types) / sizeof(swig_types[0]); i++)
		if (!strcmp(swig_types[i].name, name))
			return swig_types[i].hidden ? NULL : &swig_types[i];
	return NULL;
}

int SWIG_ConvertPtr(PyObject *obj, void **ptr, swig_type_info *type, int flags)
{
	(void)flags;
	if (obj == Py_None) {
		*ptr = NULL;
		return SWIG_OK;
	}
	if (!PyCapsule_CheckExact(obj) || !PyCapsule_IsValid(obj, type->name))
		return SWIG_ERROR;
	*ptr = PyCapsule_GetPointer(obj, type->name);
	return SWIG_OK;
}

PyObject *SWIG_NewPointerObj(void *ptr, swig_type_info *type, int own)
{
	(void)own;
	if (!ptr)
		Py_RETURN_NONE;
	return PyCapsule_New(ptr, type->name, NULL);
}

static PyObject *push_typed(const char *tname, void *p)
{
	return SWIG_NewPointerObj(p, SWIG_TypeQuery(tname), 0);
}
static bool get_typed(PyObject *obj, const char *tname, void **out)
{
	if (!SWIG_IsOK(SWIG_ConvertPtr(obj, out, SWIG_TypeQuery(tname), 0))) {
		PyErr_Format(PyExc_TypeError, "expected %s", tname);
		return false;
	}
	return true;
}

/* ------------------------------------------------------------------ */
/* mini "obspython" module (built-in, registered with PyImport_AppendInittab by the runner)       */

#define DATA_ARG(fmt, ...)                                                                \
	PyObject *o;                                                                      \
	void *d;                                                                          \
	if (!PyArg_ParseTuple(args, "O" fmt, &o, ##__VA_ARGS__) ||                        \
	    !get_typed(o, "obs_data_t *", &d))                                             \
		return NULL

static PyObject *m_data_set_string(PyObject *self, PyObject *args)
{
	const char *k, *v;
	(void)self;
	DATA_ARG("ss", &k, &v);
	obs_data_set_string(d, k, v);
	Py_RETURN_NONE;
}
static PyObject *m_data_set_int(PyObject *self, PyObject *args)
{
	const char *k;
	long long v;
	(void)self;
	DATA_ARG("sL", &k, &v);
	obs_data_set_int(d, k, v);
	Py_RETURN_NONE;
}
static PyObject *m_data_set_bool(PyObject *self, PyObject *args)
{
	const char *k;
	int v;
	(void)self;
	DATA_ARG("sp", &k, &v);
	obs_data_set_bool(d, k, v);
	Py_RETURN_NONE;
}
static PyObject *m_data_set_double(PyObject *self, PyObject *args)
{
	const char *k;
	double v;
	(void)self;
	DATA_ARG("sd", &k, &v);
	obs_data_set_double(d, k, v);
	Py_RETURN_NONE;
}
static PyObject *m_data_set_default_string(PyObject *self, PyObject *args)
{
	const char *k, *v;
	(void)self;
	DATA_ARG("ss", &k, &v);
	obs_data_set_default_string(d, k, v);
	Py_RETURN_NONE;
}
static PyObject *m_data_set_default_int(PyObject *self, PyObject *args)
{
	const char *k;
	long long v;
	(void)self;
	DATA_ARG("sL", &k, &v);
	obs_data_set_default_int(d, k, v);
	Py_RETURN_NONE;
}
static PyObject *m_data_get_string(PyObject *self, PyObject *args)
{
	const char *k;
	(void)self;
	DATA_ARG("s", &k);
	return PyUnicode_FromString(obs_data_get_string(d, k));
}
static PyObject *m_data_get_int(PyObject *self, PyObject *args)
{
	const char *k;
	(void)self;
	DATA_ARG("s", &k);
	return PyLong_FromLongLong(obs_data_get_int(d, k));
}
static PyObject *m_data_get_bool(PyObject *self, PyObject *args)
{
	const char *k;
	(void)self;
	DATA_ARG("s", &k);
	return PyBool_FromLong(obs_data_get_bool(d, k));
}
static PyObject *m_data_get_double(PyObject *self, PyObject *args)
{
	const char *k;
	(void)self;
	DATA_ARG("s", &k);
	return PyFloat_FromDouble(obs_data_get_double(d, k));
}
static PyObject *m_data_has_user_value(PyObject *self, PyObject *args)
{
	const char *k;
	(void)self;
	DATA_ARG("s", &k);
	return PyBool_FromLong(obs_data_has_user_value(d, k));
}

#define PROPS_ARG(fmt, ...)                                                               \
	PyObject *o;                                                                      \
	void *props;                                                                      \
	if (!PyArg_ParseTuple(args, "O" fmt, &o, ##__VA_ARGS__) ||                        \
	    !get_typed(o, "obs_properties_t *", &props))                                   \
		return NULL

static PyObject *m_properties_create(PyObject *self, PyObject *args)
{
	(void)self, (void)args;
	return push_typed("obs_properties_t *", obs_properties_create());
}
static PyObject *m_properties_add_text(PyObject *self, PyObject *args)
{
	const char *n, *d;
	int t;
	(void)self;
	PROPS_ARG("ssi", &n, &d, &t);
	return push_typed("obs_property_t *", obs_properties_add_text(props, n, d, (enum obs_text_type)t));
}
static PyObject *m_properties_add_int(PyObject *self, PyObject *args)
{
	const char *n, *d;
	int lo, hi, st;
	(void)self;
	PROPS_ARG("ssiii", &n, &d, &lo, &hi, &st);
	return push_typed("obs_property_t *", obs_properties_add_int(props, n, d, lo, hi, st));
}
static PyObject *m_properties_add_bool(PyObject *self, PyObject *args)
{
	const char *n, *d;
	(void)self;
	PROPS_ARG("ss", &n, &d);
	return push_typed("obs_property_t *", obs_properties_add_bool(props, n, d));
}
static PyObject *m_properties_get(PyObject *self, PyObject *args)
{
	const char *n;
	(void)self;
	PROPS_ARG("s", &n);
	return push_typed("obs_property_t *", obs_properties_get(props, n));
}
static PyObject *m_property_set_description(PyObject *self, PyObject *args)
{
	PyObject *o;
	const char *d;
	void *p;
	(void)self;
	if (!PyArg_ParseTuple(args, "Os", &o, &d) || !get_typed(o, "obs_property_t *", &p))
		return NULL;
	obs_property_set_description(p, d);
	Py_RETURN_NONE;
}
static PyObject *m_property_set_enabled(PyObject *self, PyObject *args)
{
	PyObject *o;
	int e;
	void *p;
	(void)self;
	if (!PyArg_ParseTuple(args, "Op", &o, &e) || !get_typed(o, "obs_property_t *", &p))
		return NULL;
	obs_property_set_enabled(p, e);
	Py_RETURN_NONE;
}
static PyObject *m_calldata_get_string(PyObject *self, PyObject *args)
{
	PyObject *o;
	const char *k, *s = NULL;
	void *cd;
	(void)self;
	if (!PyArg_ParseTuple(args, "Os", &o, &k) || !get_typed(o, "calldata_t *", &cd))
		return NULL;
	if (calldata_get_string(cd, k, &s) && s)
		return PyUnicode_FromString(s);
	Py_RETURN_NONE;
}
static PyObject *m_calldata_get_int(PyObject *self, PyObject *args)
{
	PyObject *o;
	const char *k;
	void *cd;
	(void)self;
	if (!PyArg_ParseTuple(args, "Os", &o, &k) || !get_typed(o, "calldata_t *", &cd))
		return NULL;
	return PyLong_FromLongLong(calldata_int(cd, k));
}
static PyObject *m_source_get_name(PyObject *self, PyObject *args)
{
	PyObject *o;
	void *s;
	(void)self;
	if (!PyArg_ParseTuple(args, "O", &o) || !get_typed(o, "obs_source_t *", &s))
		return NULL;
	return PyUnicode_FromString(((struct obs_source *)s)->name);
}
static PyObject *m_sceneitem_get_id(PyObject *self, PyObject *args)
{
	PyObject *o;
	void *s;
	(void)self;
	if (!PyArg_ParseTuple(args, "O", &o) || !get_typed(o, "obs_sceneitem_t *", &s))
		return NULL;
	return PyLong_FromLong(((struct obs_scene_item *)s)->id);
}
static PyObject *m_get_video_frame_time(PyObject *self, PyObject *args)
{
	(void)self, (void)args;
	return PyLong_FromUnsignedLongLong(lf_video_time);
}

/* fixtures (not OBS API) */
static PyObject *m_fake_handler(PyObject *self, PyObject *args)
{
	int n;
	(void)self;
	if (!PyArg_ParseTuple(args, "i", &n) || n < 0 || n > 3)
		return NULL;
	return push_typed("signal_handler_t *", lf_handlers[n]);
}
static PyObject *m_fake_source(PyObject *self, PyObject *args)
{
	int n;
	(void)self;
	if (!PyArg_ParseTuple(args, "i", &n) || n < 0 || n >= lf_nsources)
		return NULL;
	return push_typed("obs_source_t *", &lf_sources[n]);
}
static PyObject *m_fake_scene(PyObject *self, PyObject *args)
{
	(void)self, (void)args;
	return push_typed("obs_scene_t *", &lf_scene);
}
static PyObject *m_fake_sceneitem(PyObject *self, PyObject *args)
{
	int n;
	(void)self;
	if (!PyArg_ParseTuple(args, "i", &n) || n < 0 || n > 3)
		return NULL;
	return push_typed("obs_sceneitem_t *", &lf_items[n]);
}
static PyObject *m_fake_is_capsule(PyObject *self, PyObject *args)
{
	PyObject *o;
	(void)self;
	if (!PyArg_ParseTuple(args, "O", &o))
		return NULL;
	return PyBool_FromLong(PyCapsule_CheckExact(o));
}

static PyMethodDef obspython_funcs[] = {
	{"obs_data_set_string", m_data_set_string, METH_VARARGS, NULL},
	{"obs_data_set_int", m_data_set_int, METH_VARARGS, NULL},
	{"obs_data_set_bool", m_data_set_bool, METH_VARARGS, NULL},
	{"obs_data_set_double", m_data_set_double, METH_VARARGS, NULL},
	{"obs_data_set_default_string", m_data_set_default_string, METH_VARARGS, NULL},
	{"obs_data_set_default_int", m_data_set_default_int, METH_VARARGS, NULL},
	{"obs_data_get_string", m_data_get_string, METH_VARARGS, NULL},
	{"obs_data_get_int", m_data_get_int, METH_VARARGS, NULL},
	{"obs_data_get_bool", m_data_get_bool, METH_VARARGS, NULL},
	{"obs_data_get_double", m_data_get_double, METH_VARARGS, NULL},
	{"obs_data_has_user_value", m_data_has_user_value, METH_VARARGS, NULL},
	{"obs_properties_create", m_properties_create, METH_NOARGS, NULL},
	{"obs_properties_add_text", m_properties_add_text, METH_VARARGS, NULL},
	{"obs_properties_add_int", m_properties_add_int, METH_VARARGS, NULL},
	{"obs_properties_add_bool", m_properties_add_bool, METH_VARARGS, NULL},
	{"obs_properties_get", m_properties_get, METH_VARARGS, NULL},
	{"obs_property_set_description", m_property_set_description, METH_VARARGS, NULL},
	{"obs_property_set_enabled", m_property_set_enabled, METH_VARARGS, NULL},
	{"calldata_get_string", m_calldata_get_string, METH_VARARGS, NULL},
	{"calldata_get_int", m_calldata_get_int, METH_VARARGS, NULL},
	{"obs_source_get_name", m_source_get_name, METH_VARARGS, NULL},
	{"obs_sceneitem_get_id", m_sceneitem_get_id, METH_VARARGS, NULL},
	{"obs_get_video_frame_time", m_get_video_frame_time, METH_NOARGS, NULL},
	{"fake_handler", m_fake_handler, METH_VARARGS, NULL},
	{"fake_source", m_fake_source, METH_VARARGS, NULL},
	{"fake_scene", m_fake_scene, METH_NOARGS, NULL},
	{"fake_sceneitem", m_fake_sceneitem, METH_VARARGS, NULL},
	{"fake_is_capsule", m_fake_is_capsule, METH_VARARGS, NULL},
	{NULL, NULL, 0, NULL}};

static struct PyModuleDef obspython_def = {PyModuleDef_HEAD_INIT, "obspython", NULL, -1, obspython_funcs,
					   NULL, NULL, NULL, NULL};

PyObject *PyInit_obspython(void)
{
	PyObject *m = PyModule_Create(&obspython_def);
	if (!m)
		return NULL;
	PyModule_AddIntConstant(m, "LOG_ERROR", LOG_ERROR);
	PyModule_AddIntConstant(m, "LOG_WARNING", LOG_WARNING);
	PyModule_AddIntConstant(m, "LOG_INFO", LOG_INFO);
	PyModule_AddIntConstant(m, "LOG_DEBUG", LOG_DEBUG);
	PyModule_AddIntConstant(m, "OBS_TEXT_DEFAULT", OBS_TEXT_DEFAULT);
	return m;
}
