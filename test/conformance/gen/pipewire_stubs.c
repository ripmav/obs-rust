/*
 * Conformance fakes (area pipewire, spec S-056). Test tooling, not part of OBS.
 *
 * The REAL plugins/linux-pipewire sources are linked; everything they call that is not part of
 * them is replaced here by scripted fakes that print one "<layer>.<call> ..." line per call:
 *   host.*   OBS core (source registration, frame output, settings save, video info)
 *   gs.*     graphics (textures, dmabuf import, matrix/blend/sprite drawing, sync objects)
 *   dbus.*   GIO / xdg-desktop-portal (proxy, request signals, session Close, unix-fd lists)
 *   pw.*     libpipewire (thread loop, context, core, registry, proxies/nodes, streams, loop events)
 * Replies, signals, stream callbacks and buffers are delivered by script commands.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <gio/gio.h>
#include <gio/gunixfdlist.h>
#include <glad/glad.h>
#include <libdrm/drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/debug/types.h>
#include <spa/param/props.h>
#include <spa/param/param.h>
#include <spa/param/type-info.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/type-info.h>
#include <spa/pod/builder.h>
#include <spa/pod/iter.h>
#include <spa/utils/result.h>

#include <obs-module.h>
#include "formats.h"
#include <util/bmem.h>
#include "pipewire_api.h"

#define P(...) printf(__VA_ARGS__)
#define IS(s) (strcmp(a[0], (s)) == 0)

/* ---------------------------------------------------------------- platform stubs for obs-data/obs-properties */
char *os_quick_read_utf8_file(const char *path) { (void)path; abort(); }
bool os_quick_write_utf8_file(const char *p, const char *s, size_t l, bool m) { (void)p, (void)s, (void)l, (void)m; abort(); }
bool os_quick_write_utf8_file_safe(const char *p, const char *s, size_t l, bool m, const char *t, const char *b)
{
	(void)p, (void)s, (void)l, (void)m, (void)t, (void)b;
	abort();
}
int os_rename(const char *o, const char *n) { return rename(o, n); }
/* obs_module_text() itself is the real one from OBS_MODULE_USE_DEFAULT_LOCALE; with an empty lookup it returns the key. */
#include <util/text-lookup.h>
bool text_lookup_getstr(lookup_t *lookup, const char *lookup_val, const char **out) { (void)lookup, (void)lookup_val, (void)out; return false; }
void text_lookup_destroy(lookup_t *lookup) { (void)lookup; }
lookup_t *obs_module_load_locale(obs_module_t *module, const char *default_locale, const char *locale)
{
	(void)module, (void)default_locale, (void)locale;
	return NULL;
}

/* ---------------------------------------------------------------- instances / registered sources */
static struct pwx_inst g_inst[8];
static struct obs_source_info g_reg[8];
static int g_nreg;

struct pwx_inst *pwx_inst_get(const char *name, bool create)
{
	for (int i = 0; i < 8; i++)
		if (g_inst[i].used && !strcmp(g_inst[i].name, name))
			return &g_inst[i];
	if (!create)
		return NULL;
	for (int i = 0; i < 8; i++)
		if (!g_inst[i].used) {
			memset(&g_inst[i], 0, sizeof(g_inst[i]));
			g_inst[i].used = true;
			snprintf(g_inst[i].name, sizeof(g_inst[i].name), "%s", name);
			g_inst[i].settings = obs_data_create();
			return &g_inst[i];
		}
	abort();
}

const struct obs_source_info *pwx_find_info(const char *id)
{
	for (int i = 0; i < g_nreg; i++)
		if (!strcmp(g_reg[i].id, id))
			return &g_reg[i];
	return NULL;
}

void obs_register_source_s(const struct obs_source_info *info, size_t size)
{
	struct obs_source_info *r = &g_reg[g_nreg++];
	memset(r, 0, sizeof(*r));
	memcpy(r, info, size < sizeof(*r) ? size : sizeof(*r));
	P("host.register_source %s type=%d flags=0x%x icon=%d\n", r->id, (int)r->type, r->output_flags, (int)r->icon_type);
}

/* ---------------------------------------------------------------- host fakes */
static struct obs_video_info g_vi = {.fps_num = 60, .fps_den = 1, .output_width = 1920, .output_height = 1080};

uint32_t obs_source_get_output_flags(const obs_source_t *s)
{
	return ((const struct pwx_inst *)s)->flags;
}

void obs_source_save(obs_source_t *s)
{
	struct pwx_inst *i = (struct pwx_inst *)s;
	if (i->info->save)
		i->info->save(i->data, i->settings);
	const char *t = obs_data_get_string(i->settings, "RestoreToken");
	P("host.source_save RestoreToken=\"%s\"\n", t ? t : "");
}

void obs_source_update_properties(obs_source_t *s)
{
	P("host.update_properties %s\n", ((struct pwx_inst *)s)->name);
}

bool obs_get_video_info(struct obs_video_info *ovi)
{
	*ovi = g_vi;
	return true;
}

void obs_enter_graphics(void) {}
void obs_leave_graphics(void) {}

void obs_source_output_video(obs_source_t *s, const struct obs_source_frame *f)
{
	(void)s;
	if (!f) {
		P("host.output_video NULL\n");
		return;
	}
	P("host.output_video %ux%u format=%d linesize0=%u range=%.6g..%.6g m0=%.6g,%.6g,%.6g,%.6g\n", f->width, f->height,
	  (int)f->format, f->linesize[0], f->color_range_min[0], f->color_range_max[0], f->color_matrix[0],
	  f->color_matrix[1], f->color_matrix[5], f->color_matrix[10]);
	for (int i = 0; i < MAX_AV_PLANES; i++)
		if (f->data[i])
			P("  plane%d data0=%02x%02x%02x%02x\n", i, f->data[i][0], f->data[i][1], f->data[i][2], f->data[i][3]);
}

/* ---------------------------------------------------------------- GL / graphics fakes */
struct fake_tex {
	GLuint gl;
	int id;
};
static int g_tex_next = 1, g_tex_alive;
static bool g_dmabuf_ok = true;
static bool g_sync_caps;
static enum gs_dmabuf_flags g_dmabuf_flags;
static bool g_dmabuf_caps;
static uint32_t g_dmabuf_fmts[16];
static size_t g_n_dmabuf_fmts;
static struct {
	uint32_t fmt;
	uint64_t mods[8];
	size_t n;
} g_mods[16];
static int g_n_mods;

static void fake_glBindTexture(GLenum t, GLuint tex) { P("gl.BindTexture 0x%x %u\n", t, tex); }
static void fake_glTexParameteri(GLenum t, GLenum p, GLint v) { P("gl.TexParameteri 0x%x 0x%x 0x%x\n", t, p, v); }
PFNGLBINDTEXTUREPROC glad_glBindTexture = fake_glBindTexture;
PFNGLTEXPARAMETERIPROC glad_glTexParameteri = fake_glTexParameteri;
int gladLoadGL(void)
{
	P("gl.gladLoadGL\n");
	return 1;
}

static int fd_tag(int fd)
{
	struct stat st;
	if (fd < 0)
		return -1;
	if (fstat(fd, &st) != 0)
		return -2;
	return (int)st.st_size;
}

static void print4(const uint8_t *d)
{
	if (d)
		P(" data0=%02x%02x%02x%02x", d[0], d[1], d[2], d[3]);
}

gs_texture_t *gs_texture_create(uint32_t w, uint32_t h, enum gs_color_format f, uint32_t lv, const uint8_t **d,
				uint32_t fl)
{
	struct fake_tex *t = bzalloc(sizeof(*t));
	t->id = g_tex_next++;
	t->gl = 100 + t->id;
	g_tex_alive++;
	P("gs.texture_create #%d %ux%u fmt=%d levels=%u flags=0x%x", t->id, w, h, (int)f, lv, fl);
	print4(d ? d[0] : NULL);
	P("\n");
	return (gs_texture_t *)t;
}

gs_texture_t *gs_texture_create_from_dmabuf(unsigned int w, unsigned int h, uint32_t drm, enum gs_color_format cf,
					    uint32_t n_planes, const int *fds, const uint32_t *strides,
					    const uint32_t *offsets, const uint64_t *modifiers)
{
	P("gs.texture_create_from_dmabuf %ux%u drm=0x%08x cf=%d planes=%u", w, h, drm, (int)cf, n_planes);
	for (uint32_t i = 0; i < n_planes; i++) {
		P(" [fd#%d stride=%u off=%u", fd_tag(fds[i]), strides[i], offsets[i]);
		if (modifiers)
			P(" mod=0x%llx", (unsigned long long)modifiers[i]);
		P("]");
	}
	if (!modifiers)
		P(" nomods");
	if (!g_dmabuf_ok) {
		P(" -> NULL\n");
		return NULL;
	}
	struct fake_tex *t = bzalloc(sizeof(*t));
	t->id = g_tex_next++;
	t->gl = 100 + t->id;
	g_tex_alive++;
	P(" -> #%d\n", t->id);
	return (gs_texture_t *)t;
}

void gs_texture_destroy(gs_texture_t *tex)
{
	struct fake_tex *t = (struct fake_tex *)tex;
	if (!t)
		return;
	P("gs.texture_destroy #%d\n", t->id);
	g_tex_alive--;
	bfree(t);
}
void *gs_texture_get_obj(gs_texture_t *tex) { return &((struct fake_tex *)tex)->gl; }

bool gs_query_dmabuf_capabilities(enum gs_dmabuf_flags *fl, uint32_t **fm, size_t *n)
{
	P("gs.query_dmabuf_capabilities -> %d\n", g_dmabuf_caps);
	if (!g_dmabuf_caps)
		return false;
	*fl = g_dmabuf_flags;
	*n = g_n_dmabuf_fmts;
	*fm = bmemdup(g_dmabuf_fmts, sizeof(uint32_t) * (g_n_dmabuf_fmts ? g_n_dmabuf_fmts : 1));
	return true;
}

bool gs_query_dmabuf_modifiers_for_format(uint32_t fmt, uint64_t **m, size_t *n)
{
	for (int i = 0; i < g_n_mods; i++)
		if (g_mods[i].fmt == fmt) {
			*n = g_mods[i].n;
			*m = bmemdup(g_mods[i].mods, sizeof(uint64_t) * (g_mods[i].n ? g_mods[i].n : 1));
			P("gs.query_dmabuf_modifiers 0x%08x -> %zu\n", fmt, *n);
			return true;
		}
	P("gs.query_dmabuf_modifiers 0x%08x -> false\n", fmt);
	return false;
}

bool gs_query_sync_capabilities(void)
{
	P("gs.query_sync_capabilities -> %d\n", g_sync_caps);
	return g_sync_caps;
}
gs_sync_t *gs_sync_create(void)
{
	P("gs.sync_create\n");
	return bzalloc(1);
}
gs_sync_t *gs_sync_create_from_syncobj_timeline_point(int fd, uint64_t pt)
{
	P("gs.sync_create_from_syncobj fd#%d point=%llu\n", fd_tag(fd), (unsigned long long)pt);
	return bzalloc(1);
}
void gs_sync_destroy(gs_sync_t *s) { bfree(s); }
bool gs_sync_export_syncobj_timeline_point(gs_sync_t *s, int fd, uint64_t pt)
{
	(void)s;
	P("gs.sync_export fd#%d point=%llu\n", fd_tag(fd), (unsigned long long)pt);
	return true;
}
bool gs_sync_signal_syncobj_timeline_point(int fd, uint64_t pt)
{
	P("gs.sync_signal fd#%d point=%llu\n", fd_tag(fd), (unsigned long long)pt);
	return true;
}
bool gs_sync_wait(gs_sync_t *s)
{
	(void)s;
	P("gs.sync_wait\n");
	return true;
}

void gs_matrix_push(void) { P("gs.matrix_push\n"); }
void gs_matrix_pop(void) { P("gs.matrix_pop\n"); }
void gs_matrix_rotaa4f(float x, float y, float z, float a) { P("gs.matrix_rotaa4f %.6g %.6g %.6g %.6g\n", x, y, z, a); }
void gs_matrix_translate3f(float x, float y, float z) { P("gs.matrix_translate3f %.6g %.6g %.6g\n", x, y, z); }
void gs_blend_state_push(void) { P("gs.blend_push\n"); }
void gs_blend_state_pop(void) { P("gs.blend_pop\n"); }
void gs_blend_function(enum gs_blend_type s, enum gs_blend_type d) { P("gs.blend_function %d %d\n", (int)s, (int)d); }
void gs_draw_sprite(gs_texture_t *t, uint32_t flip, uint32_t w, uint32_t h)
{
	P("gs.draw_sprite #%d flip=%u %ux%u\n", t ? ((struct fake_tex *)t)->id : 0, flip, w, h);
}
void gs_draw_sprite_subregion(gs_texture_t *t, uint32_t flip, uint32_t x, uint32_t y, uint32_t w, uint32_t h)
{
	P("gs.draw_sprite_subregion #%d flip=%u x=%u y=%u %ux%u\n", t ? ((struct fake_tex *)t)->id : 0, flip, x, y, w, h);
}
gs_eparam_t *gs_effect_get_param_by_name(const gs_effect_t *e, const char *n)
{
	(void)e;
	P("gs.effect_get_param %s\n", n);
	return (gs_eparam_t *)bzalloc(1);
}
void gs_effect_set_texture(gs_eparam_t *p, gs_texture_t *t)
{
	P("gs.effect_set_texture #%d\n", t ? ((struct fake_tex *)t)->id : 0);
	(void)p;
}

/* ---------------------------------------------------------------- GIO / D-Bus fakes */
static bool g_bus_ok = true;
static char g_unique[32] = ":1.42";
static int g_conn_dummy;
static struct {
	char iface[16];
	bool fail;
} g_proxy_cfg[2] = {{"ScreenCast", false}, {"Camera", false}};
static struct {
	GObject *obj;
	char iface[64];
} g_proxies[4];
static struct {
	char iface[16], name[32];
	GVariant *v;
} g_props[16];
static int g_nprops;

struct pcall {
	bool used;
	bool fd;
	char method[32];
	GAsyncReadyCallback cb;
	gpointer ud;
	GObject *src;
};
static struct pcall g_calls[32];
static int g_ncalls;

struct sub {
	int id;
	bool active;
	char path[160];
	GDBusSignalCallback cb;
	gpointer ud;
};
static struct sub g_subs[32];
static int g_nsubs, g_sub_id;

static GVariant *g_cur_reply;
static GError *g_cur_error;
static GUnixFDList *g_cur_fdlist;

GDBusConnection *g_bus_get_sync(GBusType t, GCancellable *c, GError **error)
{
	(void)c;
	P("dbus.bus_get type=%d -> %s\n", (int)t, g_bus_ok ? "ok" : "error");
	if (!g_bus_ok) {
		g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "bus unavailable");
		return NULL;
	}
	return (GDBusConnection *)&g_conn_dummy;
}
const gchar *g_dbus_connection_get_unique_name(GDBusConnection *c)
{
	(void)c;
	return g_unique;
}

GDBusProxy *g_dbus_proxy_new_sync(GDBusConnection *c, GDBusProxyFlags fl, GDBusInterfaceInfo *info, const gchar *name,
				  const gchar *path, const gchar *iface, GCancellable *cancel, GError **error)
{
	(void)c, (void)info, (void)cancel;
	P("dbus.proxy_new %s %s %s flags=%d\n", name, path, iface, (int)fl);
	const char *short_name = strrchr(iface, '.') ? strrchr(iface, '.') + 1 : iface;
	if (!c) /* GIO: g_return_val_if_fail(G_IS_DBUS_CONNECTION) -> NULL without a GError */
		return NULL;
	for (int i = 0; i < 2; i++)
		if (!strcmp(g_proxy_cfg[i].iface, short_name) && g_proxy_cfg[i].fail) {
			g_set_error_literal(error, G_IO_ERROR, G_IO_ERROR_FAILED, "no such portal");
			return NULL;
		}
	GObject *o = g_object_new(G_TYPE_OBJECT, NULL);
	for (int i = 0; i < 4; i++)
		if (!g_proxies[i].obj) {
			g_proxies[i].obj = o;
			snprintf(g_proxies[i].iface, sizeof(g_proxies[i].iface), "%s", short_name);
			break;
		}
	return (GDBusProxy *)o;
}

GVariant *g_dbus_proxy_get_cached_property(GDBusProxy *p, const gchar *name)
{
	const char *iface = "";
	for (int i = 0; i < 4; i++)
		if (g_proxies[i].obj == (GObject *)p)
			iface = g_proxies[i].iface;
	for (int i = 0; i < g_nprops; i++)
		if (!strcmp(g_props[i].iface, iface) && !strcmp(g_props[i].name, name))
			return g_variant_ref(g_props[i].v);
	return NULL;
}

static int new_call(const char *m, GAsyncReadyCallback cb, gpointer ud, GObject *src, bool fd, GVariant *params)
{
	struct pcall *c = &g_calls[g_ncalls++];
	c->used = true;
	c->fd = fd;
	snprintf(c->method, sizeof(c->method), "%s", m);
	c->cb = cb;
	c->ud = ud;
	c->src = src;
	char *s = g_variant_print(params, TRUE);
	P("dbus.call#%d %s %s\n", g_ncalls, m, s);
	g_free(s);
	return g_ncalls;
}

void g_dbus_proxy_call(GDBusProxy *p, const gchar *method, GVariant *params, GDBusCallFlags fl, gint timeout,
		       GCancellable *c, GAsyncReadyCallback cb, gpointer ud)
{
	(void)fl, (void)timeout, (void)c;
	GVariant *pv = g_variant_ref_sink(params);
	new_call(method, cb, ud, (GObject *)p, false, pv);
	g_variant_unref(pv);
}
void g_dbus_proxy_call_with_unix_fd_list(GDBusProxy *p, const gchar *method, GVariant *params, GDBusCallFlags fl,
					 gint timeout, GUnixFDList *l, GCancellable *c, GAsyncReadyCallback cb,
					 gpointer ud)
{
	(void)fl, (void)timeout, (void)c, (void)l;
	GVariant *pv = g_variant_ref_sink(params);
	new_call(method, cb, ud, (GObject *)p, true, pv);
	g_variant_unref(pv);
}

GVariant *g_dbus_proxy_call_finish(GDBusProxy *p, GAsyncResult *r, GError **error)
{
	(void)p, (void)r;
	if (g_cur_error) {
		g_propagate_error(error, g_error_copy(g_cur_error));
		return NULL;
	}
	return g_cur_reply ? g_variant_ref(g_cur_reply) : NULL;
}
GVariant *g_dbus_proxy_call_with_unix_fd_list_finish(GDBusProxy *p, GUnixFDList **out, GAsyncResult *r, GError **error)
{
	(void)p, (void)r;
	if (g_cur_error) {
		g_propagate_error(error, g_error_copy(g_cur_error));
		return NULL;
	}
	if (out)
		*out = g_cur_fdlist ? g_object_ref(g_cur_fdlist) : NULL;
	return g_cur_reply ? g_variant_ref(g_cur_reply) : NULL;
}

void g_dbus_connection_call(GDBusConnection *c, const gchar *bus, const gchar *path, const gchar *iface,
			    const gchar *method, GVariant *params, const GVariantType *rt, GDBusCallFlags fl, gint timeout,
			    GCancellable *cancel, GAsyncReadyCallback cb, gpointer ud)
{
	(void)c, (void)rt, (void)fl, (void)timeout, (void)cancel, (void)cb, (void)ud;
	P("dbus.connection_call %s %s %s.%s params=%s\n", bus, path, iface, method, params ? "set" : "NULL");
}

guint g_dbus_connection_signal_subscribe(GDBusConnection *c, const gchar *sender, const gchar *iface, const gchar *member,
					 const gchar *path, const gchar *arg0, GDBusSignalFlags fl,
					 GDBusSignalCallback cb, gpointer ud, GDestroyNotify dn)
{
	(void)c, (void)dn;
	struct sub *s = &g_subs[g_nsubs++];
	s->id = ++g_sub_id;
	s->active = true;
	snprintf(s->path, sizeof(s->path), "%s", path);
	s->cb = cb;
	s->ud = ud;
	P("dbus.subscribe#%d %s %s.%s path=%s arg0=%s flags=%d\n", g_nsubs, sender, iface, member, path, arg0 ? arg0 : "NULL",
	  (int)fl);
	return s->id;
}
void g_dbus_connection_signal_unsubscribe(GDBusConnection *c, guint id)
{
	(void)c;
	for (int i = 0; i < g_nsubs; i++)
		if (g_subs[i].id == (int)id && g_subs[i].active) {
			g_subs[i].active = false;
			P("dbus.unsubscribe#%d\n", i + 1);
			return;
		}
	P("dbus.unsubscribe unknown id\n");
}

static bool all_digits(const char *s)
{
	if (!*s)
		return false;
	for (; *s; s++)
		if (*s < '0' || *s > '9')
			return false;
	return true;
}

static GVariant *parse_variant(const char *type, const char *text)
{
	GError *e = NULL;
	GVariant *v = g_variant_parse(type ? G_VARIANT_TYPE(type) : NULL, text, NULL, NULL, &e);
	if (!v) {
		fprintf(stderr, "script: bad variant '%s': %s\n", text, e->message);
		exit(2);
	}
	return g_variant_ref_sink(v);
}

/* ---------------------------------------------------------------- libpipewire fakes */
struct fake_loop_src {
	struct spa_source src;
	spa_source_event_func_t fn;
	void *data;
	bool pending;
};
static struct fake_loop_src g_evsrc[4];
static int g_nev;

static struct spa_source *lu_add_event(void *o, spa_source_event_func_t fn, void *data)
{
	(void)o;
	struct fake_loop_src *s = &g_evsrc[g_nev++];
	s->fn = fn;
	s->data = data;
	P("pw.loop_add_event#%d\n", g_nev);
	return &s->src;
}
static int lu_signal_event(void *o, struct spa_source *src)
{
	(void)o;
	struct fake_loop_src *s = (struct fake_loop_src *)src;
	s->pending = true;
	P("pw.loop_signal_event#%d\n", (int)(s - g_evsrc) + 1);
	return 0;
}
static const struct spa_loop_utils_methods g_lu_methods = {SPA_VERSION_LOOP_UTILS_METHODS, .add_event = lu_add_event,
							    .signal_event = lu_signal_event};
static struct spa_loop_utils g_lu = {{SPA_TYPE_INTERFACE_LoopUtils, SPA_VERSION_LOOP_UTILS, SPA_CALLBACKS_INIT(&g_lu_methods, NULL)}};
static struct pw_loop g_loop = {.utils = &g_lu};

static bool g_thread_ok = true, g_connect_ok = true, g_bind_ok = true;
static char g_server_version[32] = "1.0.0";
static bool g_have_info = true;
static int g_lock_depth;
static int g_thread_loop_dummy, g_context_dummy;

struct pw_thread_loop *pw_thread_loop_new(const char *name, const struct spa_dict *p)
{
	(void)p;
	P("pw.thread_loop_new \"%s\"\n", name);
	return (struct pw_thread_loop *)&g_thread_loop_dummy;
}
struct pw_loop *pw_thread_loop_get_loop(struct pw_thread_loop *l)
{
	(void)l;
	return &g_loop;
}
int pw_thread_loop_start(struct pw_thread_loop *l)
{
	(void)l;
	P("pw.thread_loop_start -> %d\n", g_thread_ok ? 0 : -1);
	return g_thread_ok ? 0 : -1;
}
void pw_thread_loop_stop(struct pw_thread_loop *l)
{
	(void)l;
	P("pw.thread_loop_stop\n");
}
void pw_thread_loop_destroy(struct pw_thread_loop *l)
{
	(void)l;
	P("pw.thread_loop_destroy lock_depth=%d\n", g_lock_depth);
}
void pw_thread_loop_lock(struct pw_thread_loop *l) { (void)l, g_lock_depth++; }
void pw_thread_loop_unlock(struct pw_thread_loop *l) { (void)l, g_lock_depth--; }
void pw_thread_loop_wait(struct pw_thread_loop *l) { (void)l; }
void pw_thread_loop_signal(struct pw_thread_loop *l, bool a)
{
	(void)l, (void)a;
	P("pw.thread_loop_signal\n");
}
struct pw_context *pw_context_new(struct pw_loop *l, struct pw_properties *p, size_t s)
{
	(void)l, (void)p, (void)s;
	return (struct pw_context *)&g_context_dummy;
}
void pw_context_destroy(struct pw_context *c)
{
	(void)c;
	P("pw.context_destroy\n");
}

struct fake_core {
	struct spa_interface iface;
	const struct pw_core_events *ev;
	void *evd;
	int seq;
	bool info_sent;
};
struct fake_registry {
	struct spa_interface iface;
	const struct pw_registry_events *ev;
	void *evd;
};
struct fake_proxy {
	struct spa_interface iface; /* must be first: pw_node_* inline wrappers cast the proxy */
	uint32_t id;
	char type[48];
	bool alive;
	struct spa_hook_list proxy_hooks, node_hooks;
	int sync_seq;
};
static struct fake_core g_core;
static struct fake_registry g_registry;
static struct fake_proxy g_proxy[8];
static int g_nproxy;

static int core_add_listener(void *o, struct spa_hook *l, const struct pw_core_events *ev, void *d)
{
	(void)o, (void)l;
	g_core.ev = ev;
	g_core.evd = d;
	P("pw.core_add_listener\n");
	return 0;
}
static int core_sync(void *o, uint32_t id, int seq)
{
	(void)o;
	int s = ++g_core.seq;
	P("pw.core_sync id=%u seq_in=%d -> %d\n", id, seq, s);
	if (g_core.ev) {
		if (!g_core.info_sent && g_have_info && g_core.ev->info) {
			struct pw_core_info info = {.version = g_server_version};
			g_core.info_sent = true;
			g_core.ev->info(g_core.evd, &info);
		}
		if (g_core.ev->done)
			g_core.ev->done(g_core.evd, id, s);
	}
	return s;
}
static int registry_add_listener(void *o, struct spa_hook *l, const struct pw_registry_events *ev, void *d)
{
	(void)o, (void)l;
	g_registry.ev = ev;
	g_registry.evd = d;
	P("pw.registry_add_listener\n");
	return 0;
}
static int node_add_listener(void *o, struct spa_hook *l, const struct pw_node_events *ev, void *d)
{
	struct fake_proxy *p = o;
	spa_hook_list_append(&p->node_hooks, l, ev, d);
	return 0;
}
static int node_enum_params(void *o, int seq, uint32_t id, uint32_t start, uint32_t num, const struct spa_pod *filter)
{
	struct fake_proxy *p = o;
	(void)start, (void)num, (void)filter;
	P("pw.node_enum_params node#%u seq=%d id=%u\n", p->id, seq, id);
	return SPA_RESULT_RETURN_ASYNC(seq);
}

static void pod_dump(const struct spa_pod *p);
static int node_set_param(void *o, uint32_t id, uint32_t flags, const struct spa_pod *param)
{
	struct fake_proxy *p = o;
	P("pw.node_set_param node#%u id=%u flags=%u ", p->id, id, flags);
	pod_dump(param);
	P("\n");
	return 0;
}
static const struct pw_node_methods g_node_methods = {PW_VERSION_NODE_METHODS, .add_listener = node_add_listener,
						       .enum_params = node_enum_params, .set_param = node_set_param};

static void *registry_bind(void *o, uint32_t id, const char *type, uint32_t version, size_t sz)
{
	(void)o, (void)sz;
	P("pw.registry_bind id=%u type=%s version=%u -> %s\n", id, type, version, g_bind_ok ? "proxy" : "NULL");
	if (!g_bind_ok)
		return NULL;
	struct fake_proxy *p = &g_proxy[g_nproxy++];
	memset(p, 0, sizeof(*p));
	p->id = id;
	p->alive = true;
	snprintf(p->type, sizeof(p->type), "%s", type);
	p->iface = SPA_INTERFACE_INIT(PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, &g_node_methods, p);
	spa_hook_list_init(&p->proxy_hooks);
	spa_hook_list_init(&p->node_hooks);
	return p;
}
static const struct pw_registry_methods g_reg_methods = {PW_VERSION_REGISTRY_METHODS, .add_listener = registry_add_listener,
							  .bind = registry_bind};
static struct pw_registry *core_get_registry(void *o, uint32_t version, size_t sz)
{
	(void)o, (void)sz;
	P("pw.core_get_registry version=%u\n", version);
	g_registry.iface = SPA_INTERFACE_INIT(PW_TYPE_INTERFACE_Registry, PW_VERSION_REGISTRY, &g_reg_methods, &g_registry);
	return (struct pw_registry *)&g_registry;
}
static const struct pw_core_methods g_core_methods = {PW_VERSION_CORE_METHODS, .add_listener = core_add_listener,
						       .sync = core_sync, .get_registry = core_get_registry};

struct pw_core *pw_context_connect_fd(struct pw_context *c, int fd, struct pw_properties *p, size_t sz)
{
	(void)c, (void)p, (void)sz;
	P("pw.context_connect_fd -> %s\n", g_connect_ok ? "core" : "NULL");
	if (!g_connect_ok) {
		close(fd);
		errno = ECONNREFUSED;
		return NULL;
	}
	close(fd);
	memset(&g_core, 0, sizeof(g_core));
	g_core.iface = SPA_INTERFACE_INIT(PW_TYPE_INTERFACE_Core, PW_VERSION_CORE, &g_core_methods, &g_core);
	return (struct pw_core *)&g_core;
}

const char *pw_get_library_version(void) { return "lib-version"; }

void pw_proxy_add_listener(struct pw_proxy *proxy, struct spa_hook *l, const struct pw_proxy_events *ev, void *d)
{
	struct fake_proxy *p = (struct fake_proxy *)proxy;
	spa_hook_list_append(&p->proxy_hooks, l, ev, d);
}
int pw_proxy_sync(struct pw_proxy *proxy, int seq)
{
	struct fake_proxy *p = (struct fake_proxy *)proxy;
	p->sync_seq = ++g_core.seq;
	P("pw.proxy_sync node#%u seq_in=%d -> %d\n", p->id, seq, p->sync_seq);
	return p->sync_seq;
}
void pw_proxy_destroy(struct pw_proxy *proxy)
{
	struct fake_proxy *p = (struct fake_proxy *)proxy;
	if ((void *)proxy == (void *)&g_registry) {
		P("pw.registry_destroy\n");
		return;
	}
	if (!p->alive)
		return;
	P("pw.proxy_destroy node#%u\n", p->id);
	p->alive = false;
	spa_hook_list_call(&p->proxy_hooks, struct pw_proxy_events, destroy, 0);
}

/* streams */
struct fake_stream {
	int n;
	char name[48];
	const struct pw_stream_events *ev;
	void *evd;
	bool alive;
};
static struct fake_stream g_streams[8];
static int g_nstreams, g_cur_stream = -1;

struct pw_stream *pw_stream_new(struct pw_core *core, const char *name, struct pw_properties *props)
{
	(void)core;
	struct fake_stream *s = &g_streams[g_nstreams++];
	s->n = g_nstreams;
	s->alive = true;
	snprintf(s->name, sizeof(s->name), "%s", name);
	g_cur_stream = g_nstreams - 1;
	P("pw.stream_new#%d \"%s\" props={", s->n, name);
	if (props) {
		const struct spa_dict_item *it;
		bool first = true;
		spa_dict_for_each(it, &props->dict) {
			P("%s%s=%s", first ? "" : ",", it->key, it->value);
			first = false;
		}
		pw_properties_free(props);
	}
	P("}\n");
	return (struct pw_stream *)s;
}
void pw_stream_add_listener(struct pw_stream *st, struct spa_hook *l, const struct pw_stream_events *ev, void *d)
{
	struct fake_stream *s = (struct fake_stream *)st;
	(void)l;
	s->ev = ev;
	s->evd = d;
}
static void dump_params(const struct spa_pod **params, uint32_t n)
{
	for (uint32_t i = 0; i < n; i++) {
		P("  param[%u] ", i);
		pod_dump(params[i]);
		P("\n");
	}
}
int pw_stream_connect(struct pw_stream *st, enum pw_direction dir, uint32_t target, enum pw_stream_flags fl,
		      const struct spa_pod **params, uint32_t n)
{
	struct fake_stream *s = (struct fake_stream *)st;
	P("pw.stream_connect#%d dir=%s target=%u flags=0x%x n_params=%u\n", s->n, dir == PW_DIRECTION_INPUT ? "input" : "output",
	  target, (unsigned)fl, n);
	dump_params(params, n);
	return 0;
}
int pw_stream_update_params(struct pw_stream *st, const struct spa_pod **params, uint32_t n)
{
	struct fake_stream *s = (struct fake_stream *)st;
	P("pw.stream_update_params#%d n_params=%u\n", s->n, n);
	dump_params(params, n);
	return 0;
}
int pw_stream_set_active(struct pw_stream *st, bool a)
{
	P("pw.stream_set_active#%d %d\n", ((struct fake_stream *)st)->n, a);
	return 0;
}
int pw_stream_disconnect(struct pw_stream *st)
{
	P("pw.stream_disconnect#%d\n", ((struct fake_stream *)st)->n);
	return 0;
}
void pw_stream_destroy(struct pw_stream *st)
{
	struct fake_stream *s = (struct fake_stream *)st;
	P("pw.stream_destroy#%d\n", s->n);
	s->alive = false;
}

/* buffers */
struct fbuf {
	bool used;
	int id;
	struct spa_buffer sb;
	struct spa_meta metas[6];
	struct spa_data datas[6];
	struct spa_chunk chunks[6];
	uint8_t mem[6][256];
	uint8_t meta_mem[6][512];
	struct pw_buffer pb;
};
static struct fbuf g_bufs[32];
static int g_fifo[16], g_fifo_n;

struct pw_buffer *pw_stream_dequeue_buffer(struct pw_stream *st)
{
	(void)st;
	if (g_fifo_n == 0)
		return NULL;
	int id = g_fifo[0];
	memmove(g_fifo, g_fifo + 1, sizeof(int) * (--g_fifo_n));
	P("pw.dequeue_buffer #%d\n", id);
	return &g_bufs[id].pb;
}
int pw_stream_queue_buffer(struct pw_stream *st, struct pw_buffer *b)
{
	(void)st;
	P("pw.queue_buffer #%d\n", (int)(intptr_t)b->user_data);
	return 0;
}

/* ---------------------------------------------------------------- pod dump / builders */
static const struct spa_type_info *keytable(uint32_t objtype)
{
	switch (objtype) {
	case SPA_TYPE_OBJECT_Format:
		return spa_type_format;
	case SPA_TYPE_OBJECT_ParamBuffers:
		return spa_type_param_buffers;
	case SPA_TYPE_OBJECT_ParamMeta:
		return spa_type_param_meta;
	case SPA_TYPE_OBJECT_Props:
		return spa_type_props;
	default:
		return NULL;
	}
}

static void value_dump(uint32_t type, const void *body, uint32_t size)
{
	switch (type) {
	case SPA_TYPE_Bool:
		P("Bool:%d", *(const int32_t *)body != 0);
		break;
	case SPA_TYPE_Id:
		P("Id:%u", *(const uint32_t *)body);
		break;
	case SPA_TYPE_Int:
		P("Int:%d", *(const int32_t *)body);
		break;
	case SPA_TYPE_Long:
		P("Long:%lld", (long long)*(const int64_t *)body);
		break;
	case SPA_TYPE_Float:
		P("Float:%.6g", *(const float *)body);
		break;
	case SPA_TYPE_Rectangle: {
		const struct spa_rectangle *r = body;
		P("Rect:%ux%u", r->width, r->height);
		break;
	}
	case SPA_TYPE_Fraction: {
		const struct spa_fraction *f = body;
		P("Frac:%u/%u", f->num, f->denom);
		break;
	}
	case SPA_TYPE_String:
		P("Str:\"%s\"", (const char *)body);
		break;
	default:
		P("Type%u(size=%u)", type, size);
	}
}

static void pod_dump(const struct spa_pod *p)
{
	if (!p) {
		P("NULL");
		return;
	}
	switch (SPA_POD_TYPE(p)) {
	case SPA_TYPE_Object: {
		const struct spa_pod_object *o = (const void *)p;
		const struct spa_type_info *kt = keytable(o->body.type);
		const struct spa_pod_prop *pr;
		P("Object(type=0x%x,id=%u){", o->body.type, o->body.id);
		bool first = true;
		SPA_POD_OBJECT_FOREACH(o, pr) {
			const char *kn = kt ? spa_debug_type_find_short_name(kt, pr->key) : NULL;
			P("%s", first ? "" : "; ");
			first = false;
			if (kn)
				P("%s", kn);
			else
				P("key0x%x", pr->key);
			if (pr->flags)
				P("[flags=0x%x]", pr->flags);
			P("=");
			pod_dump(&pr->value);
			if (SPA_POD_TYPE(&pr->value) == SPA_TYPE_Id && kt) {
				const struct spa_type_info *vt = NULL;
				uint32_t id = *(const uint32_t *)SPA_POD_BODY_CONST(&pr->value);
				if (o->body.type == SPA_TYPE_OBJECT_Format && pr->key == SPA_FORMAT_mediaType)
					vt = spa_type_media_type;
				else if (o->body.type == SPA_TYPE_OBJECT_Format && pr->key == SPA_FORMAT_mediaSubtype)
					vt = spa_type_media_subtype;
				else if (o->body.type == SPA_TYPE_OBJECT_Format && pr->key == SPA_FORMAT_VIDEO_format)
					vt = spa_type_video_format;
				else if (o->body.type == SPA_TYPE_OBJECT_ParamMeta && pr->key == SPA_PARAM_META_type)
					vt = spa_type_meta_type;
				const char *vn = vt ? spa_debug_type_find_short_name(vt, id) : NULL;
				if (vn)
					P("(%s)", vn);
			}
		}
		P("}");
		break;
	}
	case SPA_TYPE_Choice: {
		const struct spa_pod_choice *c = (const void *)p;
		static const char *const cn[] = {"None", "Range", "Step", "Enum", "Flags"};
		uint32_t n = (c->pod.size - sizeof(c->body)) / (c->body.child.size ? c->body.child.size : 1);
		P("Choice:%s<", c->body.type <= 4 ? cn[c->body.type] : "?");
		const uint8_t *v = SPA_PTROFF(&c->body.child, sizeof(struct spa_pod), uint8_t);
		for (uint32_t i = 0; i < n; i++) {
			if (i)
				P(",");
			value_dump(c->body.child.type, v + i * c->body.child.size, c->body.child.size);
		}
		P(">");
		break;
	}
	case SPA_TYPE_Struct: {
		const struct spa_pod *e;
		bool first = true;
		P("Struct[");
		SPA_POD_STRUCT_FOREACH((const struct spa_pod_struct *)p, e) {
			P("%s", first ? "" : ",");
			first = false;
			pod_dump(e);
		}
		P("]");
		break;
	}
	default:
		value_dump(SPA_POD_TYPE(p), SPA_POD_BODY_CONST(p), SPA_POD_BODY_SIZE(p));
	}
}

static struct {
	char name[16];
	uint8_t buf[2048];
} g_pods[48];
static int g_npods;

static const struct {
	const char *name;
	uint32_t id;
} g_vfmt[] = {{"BGRA", SPA_VIDEO_FORMAT_BGRA},   {"RGBA", SPA_VIDEO_FORMAT_RGBA},   {"BGRx", SPA_VIDEO_FORMAT_BGRx},
	      {"RGBx", SPA_VIDEO_FORMAT_RGBx},   {"YUY2", SPA_VIDEO_FORMAT_YUY2},   {"NV12", SPA_VIDEO_FORMAT_NV12},
	      {"ABGR_210LE", SPA_VIDEO_FORMAT_ABGR_210LE}, {"xBGR_210LE", SPA_VIDEO_FORMAT_xBGR_210LE},
	      {"I420", SPA_VIDEO_FORMAT_I420},   {"ARGB", SPA_VIDEO_FORMAT_ARGB},   {"ENCODED", SPA_VIDEO_FORMAT_ENCODED},
	      {"UNKNOWN", SPA_VIDEO_FORMAT_UNKNOWN}};

static uint32_t vfmt_id(const char *n)
{
	for (size_t i = 0; i < SPA_N_ELEMENTS(g_vfmt); i++)
		if (!strcmp(g_vfmt[i].name, n))
			return g_vfmt[i].id;
	return (uint32_t)strtoul(n, NULL, 0);
}

static const struct spa_pod *pod_find(const char *name)
{
	for (int i = 0; i < g_npods; i++)
		if (!strcmp(g_pods[i].name, name))
			return (const struct spa_pod *)g_pods[i].buf;
	fprintf(stderr, "script: no pod '%s'\n", name);
	exit(2);
}

static struct spa_pod_builder pod_start(const char *name)
{
	int i = g_npods;
	for (int k = 0; k < g_npods; k++)
		if (!strcmp(g_pods[k].name, name))
			i = k;
	if (i == g_npods)
		g_npods++;
	snprintf(g_pods[i].name, sizeof(g_pods[i].name), "%s", name);
	struct spa_pod_builder b = SPA_POD_BUILDER_INIT(g_pods[i].buf, sizeof(g_pods[i].buf));
	return b;
}

static void parse_frac(const char *s, struct spa_fraction *f)
{
	unsigned n = 0, d = 1;
	sscanf(s, "%u/%u", &n, &d);
	f->num = n;
	f->denom = d;
}

/* "kind:a,b,c" with kind none|range|enum|step|int|omit */
static void build_framerate(struct spa_pod_builder *b, const char *spec)
{
	char kind[16] = {0}, rest[200] = {0};
	const char *colon = strchr(spec, ':');
	snprintf(kind, sizeof(kind), "%.*s", colon ? (int)(colon - spec) : (int)strlen(spec), spec);
	if (colon)
		snprintf(rest, sizeof(rest), "%s", colon + 1);
	if (!strcmp(kind, "omit"))
		return;
	spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_framerate, 0);
	if (!strcmp(kind, "int")) {
		spa_pod_builder_int(b, atoi(rest));
		return;
	}
	struct spa_pod_frame f;
	enum spa_choice_type ct = !strcmp(kind, "range") ? SPA_CHOICE_Range : !strcmp(kind, "enum") ? SPA_CHOICE_Enum
			: !strcmp(kind, "step") ? SPA_CHOICE_Step : SPA_CHOICE_None;
	spa_pod_builder_push_choice(b, &f, ct, 0);
	for (char *tok = strtok(rest, ","); tok; tok = strtok(NULL, ",")) {
		struct spa_fraction fr;
		parse_frac(tok, &fr);
		spa_pod_builder_fraction(b, fr.num, fr.denom);
	}
	spa_pod_builder_pop(b, &f);
}

/* ---------------------------------------------------------------- command handling */
static struct fbuf *buf_get(const char *s)
{
	int id = atoi(s);
	g_bufs[id].used = true;
	g_bufs[id].id = id;
	g_bufs[id].pb.buffer = &g_bufs[id].sb;
	g_bufs[id].pb.user_data = (void *)(intptr_t)id;
	g_bufs[id].sb.metas = g_bufs[id].metas;
	g_bufs[id].sb.datas = g_bufs[id].datas;
	return &g_bufs[id];
}

static int memfd_tag(int tag)
{
	int fd = memfd_create("pwx", 0);
	if (ftruncate(fd, tag) != 0)
		abort();
	return fd;
}

static struct fake_stream *cur_stream(void)
{
	if (g_cur_stream < 0) {
		fprintf(stderr, "script: no stream\n");
		exit(2);
	}
	return &g_streams[g_cur_stream];
}

static struct fake_proxy *proxy_by_id(uint32_t id)
{
	for (int i = 0; i < g_nproxy; i++)
		if (g_proxy[i].id == id)
			return &g_proxy[i];
	fprintf(stderr, "script: no proxy %u\n", id);
	exit(2);
}

static uint32_t drm_by_name(const char *n)
{
	static const struct {
		const char *n;
		uint32_t v;
	} t[] = {{"ARGB8888", DRM_FORMAT_ARGB8888}, {"ABGR8888", DRM_FORMAT_ABGR8888}, {"XRGB8888", DRM_FORMAT_XRGB8888},
		 {"XBGR8888", DRM_FORMAT_XBGR8888}, {"YUYV", DRM_FORMAT_YUYV},         {"NV12", DRM_FORMAT_NV12},
		 {"ABGR2101010", DRM_FORMAT_ABGR2101010}, {"XBGR2101010", DRM_FORMAT_XBGR2101010}};
	for (size_t i = 0; i < SPA_N_ELEMENTS(t); i++)
		if (!strcmp(t[i].n, n))
			return t[i].v;
	return (uint32_t)strtoul(n, NULL, 0);
}

/* key=value options of "param_changed" */
static const char *opt(const struct script_line *sl, const char *key, const char *def)
{
	size_t kl = strlen(key);
	for (int i = 1; i < sl->argc; i++)
		if (!strncmp(sl->argv[i], key, kl) && sl->argv[i][kl] == '=')
			return sl->argv[i] + kl + 1;
	return def;
}

static bool run_line(const char *text)
{
	static struct script_line l;
	l.lineno = 0;
	script_tokenize(&l, text);
	return pwx_cmd(&l);
}

static struct sub *newest_active_sub(void)
{
	for (int i = g_nsubs - 1; i >= 0; i--)
		if (g_subs[i].active)
			return &g_subs[i];
	return NULL;
}

bool pwx_cmd(const struct script_line *sl)
{
	char *const *a = sl->argv;

	/* ---- D-Bus configuration */
	if (IS("bus")) {
		script_need(sl, 2);
		g_bus_ok = !strcmp(a[1], "ok");
		return true;
	}
	if (IS("unique_name")) {
		script_need(sl, 2);
		snprintf(g_unique, sizeof(g_unique), "%s", a[1]);
		return true;
	}
	if (IS("proxy")) { /* proxy ScreenCast|Camera ok|fail */
		script_need(sl, 3);
		for (int i = 0; i < 2; i++)
			if (!strcmp(g_proxy_cfg[i].iface, a[1]))
				g_proxy_cfg[i].fail = !strcmp(a[2], "fail");
		return true;
	}
	if (IS("prop")) { /* prop IFACE NAME VARIANT-TEXT */
		script_need(sl, 4);
		snprintf(g_props[g_nprops].iface, 16, "%s", a[1]);
		snprintf(g_props[g_nprops].name, 32, "%s", a[2]);
		g_props[g_nprops++].v = parse_variant(NULL, a[3]);
		return true;
	}
	if (IS("finish")) { /* finish N ok VARIANT | finish N fd | finish N error cancelled|failed MSG */
		script_need(sl, 3);
		int n = atoi(a[1]);
		struct pcall *c = &g_calls[n - 1];
		g_cur_reply = NULL;
		g_cur_error = NULL;
		g_cur_fdlist = NULL;
		if (!strcmp(a[2], "ok"))
			g_cur_reply = parse_variant(NULL, sl->argc > 3 ? a[3] : "()");
		else if (!strcmp(a[2], "fd")) {
			int fd = open("/dev/null", O_RDONLY);
			g_cur_fdlist = g_unix_fd_list_new_from_array(&fd, 1);
			g_cur_reply = parse_variant(NULL, "(@h 0,)");
		} else {
			bool canc = sl->argc > 3 && !strcmp(a[3], "cancelled");
			g_cur_error = g_error_new_literal(G_IO_ERROR, canc ? G_IO_ERROR_CANCELLED : G_IO_ERROR_FAILED,
							  sl->argc > 4 ? a[4] : "failed");
		}
		P("dbus.finish#%d %s\n", n, c->method);
		c->cb(c->src, (GAsyncResult *)&g_conn_dummy, c->ud);
		if (g_cur_reply)
			g_variant_unref(g_cur_reply);
		if (g_cur_error)
			g_error_free(g_cur_error);
		if (g_cur_fdlist)
			g_object_unref(g_cur_fdlist);
		g_cur_reply = NULL;
		g_cur_error = NULL;
		g_cur_fdlist = NULL;
		return true;
	}
	if (IS("response")) { /* response INDEX|PATH VARIANT-TEXT of type (ua{sv}) */
		script_need(sl, 3);
		struct sub *s = NULL;
		if (all_digits(a[1]))
			s = &g_subs[atoi(a[1]) - 1];
		else
			for (int i = 0; i < g_nsubs; i++)
				if (g_subs[i].active && !strcmp(g_subs[i].path, a[1]))
					s = &g_subs[i];
		if (!s || !s->active) {
			P("response: no active subscription\n");
			return true;
		}
		GVariant *v = parse_variant("(ua{sv})", a[2]);
		P("dbus.signal Response -> %s\n", s->path);
		s->cb((GDBusConnection *)&g_conn_dummy, ":1.1", s->path, "org.freedesktop.portal.Request", "Response", v, s->ud);
		g_variant_unref(v);
		return true;
	}

	if (IS("portal_ok")) { /* portal_ok NODE : drive the newest pending portal flow to the end (all steps granted) */
		script_need(sl, 2);
		char line[200];
		for (int step = 0; step < 3; step++) {
			struct sub *sb = newest_active_sub();
			if (!sb)
				break;
			snprintf(line, sizeof(line), "response %s \"(0, %s)\"", sb->path,
				 step == 0 ? "{'session_handle': <'/s/auto'>}"
				 : step == 1 ? "{}" : "{'streams': <[(@u NODE, @a{sv} {})]>}");
			char *at = strstr(line, "NODE");
			if (at) {
				char tmp[200];
				snprintf(tmp, sizeof(tmp), "%.*s%s%s", (int)(at - line), line, a[1], at + 4);
				snprintf(line, sizeof(line), "%s", tmp);
			}
			run_line(line);
		}
		snprintf(line, sizeof(line), "finish %d fd", g_ncalls);
		run_line(line);
		return true;
	}

	/* ---- graphics configuration */
	if (IS("video")) { /* video W H NUM DEN */
		script_need(sl, 5);
		g_vi.output_width = atoi(a[1]);
		g_vi.output_height = atoi(a[2]);
		g_vi.fps_num = atoi(a[3]);
		g_vi.fps_den = atoi(a[4]);
		return true;
	}
	if (IS("gs_dmabuf")) { /* gs_dmabuf FLAGS DRMNAME... */
		g_dmabuf_caps = true;
		g_dmabuf_flags = (enum gs_dmabuf_flags)strtol(a[1], NULL, 0);
		g_n_dmabuf_fmts = 0;
		for (int i = 2; i < sl->argc; i++)
			g_dmabuf_fmts[g_n_dmabuf_fmts++] = drm_by_name(a[i]);
		return true;
	}
	if (IS("gs_dmabuf_none")) {
		g_dmabuf_caps = false;
		return true;
	}
	if (IS("gs_modifiers")) { /* gs_modifiers DRMNAME MOD... */
		g_mods[g_n_mods].fmt = drm_by_name(a[1]);
		g_mods[g_n_mods].n = 0;
		for (int i = 2; i < sl->argc; i++)
			g_mods[g_n_mods].mods[g_mods[g_n_mods].n++] = strtoull(a[i], NULL, 0);
		g_n_mods++;
		return true;
	}
	if (IS("gs_sync")) {
		g_sync_caps = atoi(a[1]);
		return true;
	}
	if (IS("dmabuf_import")) {
		g_dmabuf_ok = !strcmp(a[1], "ok");
		return true;
	}

	/* ---- libpipewire configuration */
	if (IS("pw_server_version")) { /* pw_server_version 1.2.3 | - (no info event) */
		g_have_info = strcmp(a[1], "-") != 0;
		if (g_have_info)
			snprintf(g_server_version, sizeof(g_server_version), "%s", a[1]);
		return true;
	}
	if (IS("pw_connect")) {
		g_connect_ok = !strcmp(a[1], "ok");
		return true;
	}
	if (IS("pw_thread")) {
		g_thread_ok = !strcmp(a[1], "ok");
		return true;
	}
	if (IS("pw_bind")) {
		g_bind_ok = !strcmp(a[1], "ok");
		return true;
	}
	if (IS("core_error")) { /* core_error ID SEQ RES MSG */
		script_need(sl, 5);
		if (g_core.ev && g_core.ev->error)
			g_core.ev->error(g_core.evd, atoi(a[1]), atoi(a[2]), atoi(a[3]), a[4]);
		return true;
	}
	if (IS("core_done")) { /* core_done ID SEQ */
		script_need(sl, 3);
		if (g_core.ev && g_core.ev->done)
			g_core.ev->done(g_core.evd, atoi(a[1]), atoi(a[2]));
		return true;
	}
	if (IS("run_events")) {
		for (int i = 0; i < g_nev; i++)
			if (g_evsrc[i].pending) {
				g_evsrc[i].pending = false;
				P("pw.run_event#%d\n", i + 1);
				g_evsrc[i].fn(g_evsrc[i].data, 1);
			}
		return true;
	}
	if (IS("stream")) { /* select current stream */
		g_cur_stream = atoi(a[1]) - 1;
		return true;
	}
	if (IS("state_changed")) { /* state_changed OLD NEW [ERR] ; state numbers as pw_stream_state */
		struct fake_stream *s = cur_stream();
		s->ev->state_changed(s->evd, atoi(a[1]), atoi(a[2]), sl->argc > 3 ? a[3] : NULL);
		return true;
	}
	if (IS("process")) {
		struct fake_stream *s = cur_stream();
		P("pw.event process\n");
		s->ev->process(s->evd);
		return true;
	}

	/* ---- pods */
	if (IS("pod_enum_format")) { /* pod_enum_format NAME SUBTYPE FMT W H FRAMERATE [sizerange] */
		script_need(sl, 7);
		struct spa_pod_builder b = pod_start(a[1]);
		struct spa_pod_frame f;
		uint32_t sub = !strcmp(a[2], "raw") ? SPA_MEDIA_SUBTYPE_raw : !strcmp(a[2], "mjpg") ? SPA_MEDIA_SUBTYPE_mjpg
										      : SPA_MEDIA_SUBTYPE_h264;
		spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
		spa_pod_builder_add(&b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), SPA_FORMAT_mediaSubtype,
				    SPA_POD_Id(sub), 0);
		if (sub == SPA_MEDIA_SUBTYPE_raw && strcmp(a[3], "-"))
			spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(vfmt_id(a[3])), 0);
		if (sl->argc > 7 && !strcmp(a[7], "sizerange")) {
			struct spa_rectangle r = SPA_RECTANGLE(atoi(a[4]), atoi(a[5])), mn = SPA_RECTANGLE(1, 1), mx = SPA_RECTANGLE(8192, 8192);
			spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&r, &mn, &mx), 0);
		} else {
			spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(atoi(a[4]), atoi(a[5]))), 0);
		}
		build_framerate(&b, a[6]);
		spa_pod_builder_pop(&b, &f);
		return true;
	}
	if (IS("pod_prop_info")) {
		/* pod_prop_info NAME ID DESC TYPE CHOICE LABELS V1 V2 ... ; TYPE int|bool|float, CHOICE none|range|enum|step,
		 * LABELS "-" or "id:text,id:text" */
		script_need(sl, 7);
		struct spa_pod_builder b = pod_start(a[1]);
		struct spa_pod_frame f, cf, sf;
		spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_PropInfo, SPA_PARAM_PropInfo);
		spa_pod_builder_add(&b, SPA_PROP_INFO_id, SPA_POD_Id((uint32_t)strtoul(a[2], NULL, 0)),
				    SPA_PROP_INFO_description, SPA_POD_String(a[3]), 0);
		spa_pod_builder_prop(&b, SPA_PROP_INFO_type, 0);
		enum spa_choice_type ct = !strcmp(a[5], "range") ? SPA_CHOICE_Range : !strcmp(a[5], "enum") ? SPA_CHOICE_Enum
				: !strcmp(a[5], "step") ? SPA_CHOICE_Step : SPA_CHOICE_None;
		spa_pod_builder_push_choice(&b, &cf, ct, 0);
		for (int i = 7; i < sl->argc; i++) {
			if (!strcmp(a[4], "float"))
				spa_pod_builder_float(&b, strtof(a[i], NULL));
			else if (!strcmp(a[4], "bool"))
				spa_pod_builder_bool(&b, atoi(a[i]) != 0);
			else
				spa_pod_builder_int(&b, atoi(a[i]));
		}
		spa_pod_builder_pop(&b, &cf);
		if (strcmp(a[6], "-")) {
			char tmp[200];
			snprintf(tmp, sizeof(tmp), "%s", a[6]);
			spa_pod_builder_prop(&b, SPA_PROP_INFO_labels, 0);
			spa_pod_builder_push_struct(&b, &sf);
			for (char *tok = strtok(tmp, ","); tok; tok = strtok(NULL, ",")) {
				char *c = strchr(tok, ':');
				*c = 0;
				spa_pod_builder_int(&b, atoi(tok));
				spa_pod_builder_string(&b, c + 1);
			}
			spa_pod_builder_pop(&b, &sf);
		}
		spa_pod_builder_pop(&b, &f);
		return true;
	}
	if (IS("pod_dump")) {
		P("pod %s = ", a[1]);
		pod_dump(pod_find(a[1]));
		P("\n");
		return true;
	}

	/* ---- stream param_changed:  param_changed [null] [id=N] [subtype=raw|mjpg] [format=NAME] [size=WxH] [fps=n/d] [modifier=HEX]
	 *      [cm=N] [cr=N] [media=video|audio] */
	if (IS("param_changed")) {
		struct fake_stream *s = cur_stream();
		uint8_t buf[1024];
		struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buf, sizeof(buf));
		struct spa_pod_frame f;
		uint32_t id = strtoul(opt(sl, "id", "4"), NULL, 0); /* SPA_PARAM_Format == 4 */
		const char *sub = opt(sl, "subtype", "raw");
		if (sl->argc > 1 && !strcmp(a[1], "null")) {
			P("pw.event param_changed id=%u NULL\n", id);
			s->ev->param_changed(s->evd, id, NULL);
			return true;
		}
		unsigned w = 0, h = 0;
		sscanf(opt(sl, "size", "1920x1080"), "%ux%u", &w, &h);
		struct spa_fraction fr;
		parse_frac(opt(sl, "fps", "60/1"), &fr);
		spa_pod_builder_push_object(&b, &f, SPA_TYPE_OBJECT_Format, SPA_PARAM_Format);
		spa_pod_builder_add(&b, SPA_FORMAT_mediaType,
				    SPA_POD_Id(!strcmp(opt(sl, "media", "video"), "audio") ? SPA_MEDIA_TYPE_audio : SPA_MEDIA_TYPE_video),
				    SPA_FORMAT_mediaSubtype, SPA_POD_Id(!strcmp(sub, "raw") ? SPA_MEDIA_SUBTYPE_raw : SPA_MEDIA_SUBTYPE_mjpg),
				    SPA_FORMAT_VIDEO_format, SPA_POD_Id(vfmt_id(opt(sl, "format", "BGRx"))), 0);
		if (opt(sl, "modifier", NULL))
			spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_modifier, SPA_POD_Long((int64_t)strtoull(opt(sl, "modifier", "0"), NULL, 0)), 0);
		spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_size, SPA_POD_Rectangle(&SPA_RECTANGLE(w, h)), SPA_FORMAT_VIDEO_framerate,
				    SPA_POD_Fraction(&fr), 0);
		if (opt(sl, "cm", NULL))
			spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_colorMatrix, SPA_POD_Id(atoi(opt(sl, "cm", "0"))), 0);
		if (opt(sl, "cr", NULL))
			spa_pod_builder_add(&b, SPA_FORMAT_VIDEO_colorRange, SPA_POD_Id(atoi(opt(sl, "cr", "0"))), 0);
		struct spa_pod *pod = spa_pod_builder_pop(&b, &f);
		P("pw.event param_changed id=%u ", id);
		pod_dump(pod);
		P("\n");
		s->ev->param_changed(s->evd, id, pod);
		return true;
	}

	/* ---- buffers */
	if (IS("buf_data")) { /* buf_data ID IDX TYPE TAG OFFSET STRIDE SIZE FLAGS FILL */
		script_need(sl, 10);
		struct fbuf *fb = buf_get(a[1]);
		int i = atoi(a[2]);
		struct spa_data *d = &fb->datas[i];
		memset(d, 0, sizeof(*d));
		d->chunk = &fb->chunks[i];
		fb->chunks[i] = (struct spa_chunk){.offset = atoi(a[5]), .size = atoi(a[7]), .stride = atoi(a[6]), .flags = atoi(a[8])};
		if (!strncmp(a[3], "memptr", 6)) {
			d->type = SPA_DATA_MemPtr;
			for (int k = 0; k < 256; k++)
				fb->mem[i][k] = (uint8_t)(strtol(a[9], NULL, 0) + k);
			d->data = !strcmp(a[3], "memptr-null") ? NULL : fb->mem[i];
		} else {
			d->type = !strcmp(a[3], "dmabuf") ? SPA_DATA_DmaBuf : SPA_DATA_SyncObj;
			d->fd = memfd_tag(atoi(a[4]));
		}
		if (i + 1 > (int)fb->sb.n_datas)
			fb->sb.n_datas = i + 1;
		return true;
	}
	if (IS("buf_meta")) {
		/* buf_meta ID header FLAGS | crop X Y W H | transform T | cursor CID X Y HX HY [FMT W H FILL] | sync ACQ REL */
		script_need(sl, 3);
		struct fbuf *fb = buf_get(a[1]);
		int k = fb->sb.n_metas++;
		uint8_t *m = fb->meta_mem[k];
		memset(m, 0, 512);
		fb->metas[k].data = m;
		if (!strcmp(a[2], "header")) {
			struct spa_meta_header *h = (void *)m;
			h->flags = strtoul(a[3], NULL, 0);
			fb->metas[k].type = SPA_META_Header;
			fb->metas[k].size = sizeof(*h);
		} else if (!strcmp(a[2], "crop")) {
			struct spa_meta_region *r = (void *)m;
			r->region.position.x = atoi(a[3]);
			r->region.position.y = atoi(a[4]);
			r->region.size.width = atoi(a[5]);
			r->region.size.height = atoi(a[6]);
			fb->metas[k].type = SPA_META_VideoCrop;
			fb->metas[k].size = sizeof(*r);
		} else if (!strcmp(a[2], "transform")) {
			struct spa_meta_videotransform *t = (void *)m;
			t->transform = atoi(a[3]);
			fb->metas[k].type = SPA_META_VideoTransform;
			fb->metas[k].size = sizeof(*t);
		} else if (!strcmp(a[2], "sync")) {
			struct spa_meta_sync_timeline *t = (void *)m;
			t->acquire_point = strtoull(a[3], NULL, 0);
			t->release_point = strtoull(a[4], NULL, 0);
			fb->metas[k].type = SPA_META_SyncTimeline;
			fb->metas[k].size = sizeof(*t);
		} else if (!strcmp(a[2], "cursor")) {
			struct spa_meta_cursor *c = (void *)m;
			c->id = atoi(a[3]);
			c->position.x = atoi(a[4]);
			c->position.y = atoi(a[5]);
			c->hotspot.x = atoi(a[6]);
			c->hotspot.y = atoi(a[7]);
			fb->metas[k].type = SPA_META_Cursor;
			fb->metas[k].size = sizeof(*c);
			if (sl->argc > 8) {
				struct spa_meta_bitmap *bm = (void *)(m + sizeof(*c));
				c->bitmap_offset = sizeof(*c);
				bm->format = vfmt_id(a[8]);
				bm->size.width = atoi(a[9]);
				bm->size.height = atoi(a[10]);
				bm->stride = bm->size.width * 4;
				bm->offset = sizeof(*bm);
				for (unsigned q = 0; q < bm->size.width * bm->size.height * 4 && q < 128; q++)
					m[sizeof(*c) + sizeof(*bm) + q] = (uint8_t)(strtol(a[11], NULL, 0) + q);
				fb->metas[k].size = sizeof(*c) + sizeof(*bm) + 128;
			}
		}
		return true;
	}
	if (IS("buf_push")) {
		g_fifo[g_fifo_n++] = atoi(a[1]);
		return true;
	}

	/* ---- registry / node events */
	if (IS("registry_global")) { /* registry_global ID TYPE VERSION key=val ... */
		script_need(sl, 4);
		struct spa_dict_item items[8];
		uint32_t n = 0;
		for (int i = 4; i < sl->argc && n < 8; i++) {
			char *eq = strchr(a[i], '=');
			*eq = 0;
			items[n++] = SPA_DICT_ITEM_INIT(a[i], eq + 1);
		}
		struct spa_dict d = SPA_DICT_INIT(items, n);
		P("pw.event registry_global id=%s type=%s\n", a[1], a[2]);
		g_registry.ev->global(g_registry.evd, atoi(a[1]), 0, a[2], atoi(a[3]), &d);
		return true;
	}
	if (IS("registry_global_remove")) {
		P("pw.event registry_global_remove id=%s\n", a[1]);
		g_registry.ev->global_remove(g_registry.evd, atoi(a[1]));
		return true;
	}
	if (IS("node_info")) { /* node_info ID PARAMID:FLAGS ... (change_mask = PARAMS) */
		script_need(sl, 2);
		struct fake_proxy *p = proxy_by_id(atoi(a[1]));
		struct spa_param_info pi[8];
		uint32_t n = 0;
		for (int i = 2; i < sl->argc && n < 8; i++) {
			unsigned id = 0, fl = 0;
			sscanf(a[i], "%u:%x", &id, &fl);
			pi[n++] = SPA_PARAM_INFO(id, fl);
		}
		struct pw_node_info info = {.id = p->id, .change_mask = PW_NODE_CHANGE_MASK_PARAMS, .n_params = n, .params = pi};
		P("pw.event node_info node#%u n_params=%u\n", p->id, n);
		spa_hook_list_call(&p->node_hooks, struct pw_node_events, info, 0, &info);
		return true;
	}
	if (IS("node_param")) { /* node_param ID SEQ PARAMID PODNAME */
		script_need(sl, 5);
		struct fake_proxy *p = proxy_by_id(atoi(a[1]));
		/* "async:N" delivers the value SPA_RESULT_RETURN_ASYNC(N) that enum_params returned for request N */
		int seq = !strncmp(a[2], "async:", 6) ? SPA_RESULT_RETURN_ASYNC(atoi(a[2] + 6)) : atoi(a[2]);
		P("pw.event node_param node#%u seq=%s id=%s\n", p->id, a[2], a[3]);
		spa_hook_list_call(&p->node_hooks, struct pw_node_events, param, 0, seq, (uint32_t)atoi(a[3]), 0, 0,
				   strcmp(a[4], "-") ? pod_find(a[4]) : NULL);
		return true;
	}
	if (IS("proxy_done")) { /* proxy_done ID SEQ */
		struct fake_proxy *p = proxy_by_id(atoi(a[1]));
		P("pw.event proxy_done node#%u seq=%s\n", p->id, a[2]);
		spa_hook_list_call(&p->proxy_hooks, struct pw_proxy_events, done, 0, atoi(a[2]));
		return true;
	}
	if (IS("proxy_removed")) {
		struct fake_proxy *p = proxy_by_id(atoi(a[1]));
		P("pw.event proxy_removed node#%u\n", p->id);
		spa_hook_list_call(&p->proxy_hooks, struct pw_proxy_events, removed, 0);
		return true;
	}
	if (IS("format")) { /* format NAME|ID : table lookup of obs_pw_video_format_from_spa_format */
		struct obs_pw_video_format fi = {0};
		uint32_t id = vfmt_id(a[1]);
		bool ok = obs_pw_video_format_from_spa_format(id, &fi);
		P("format %s id=%u found=%d", a[1], id, ok);
		if (ok)
			P(" drm=0x%08x gs=%d video=%d swap_rb=%d bpp=%u name=%s", fi.drm_format, (int)fi.gs_format,
			  (int)fi.video_format, fi.swap_red_blue, fi.bpp, fi.pretty_name);
		P("\n");
		return true;
	}
	if (IS("format_null")) {
		P("format_null -> %d\n", obs_pw_video_format_from_spa_format(vfmt_id(a[1]), NULL));
		return true;
	}
	if (IS("leaks")) {
		P("leaks textures=%d lock_depth=%d\n", g_tex_alive, g_lock_depth);
		return true;
	}
	return false;
}
