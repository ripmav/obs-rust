/*
 * Conformance runner for spec S-035 (service objects + module loader). Links the real libobs/obs-service.c and
 * libobs/obs-module.c against the fake core in modservice_stubs.c. Fake plugin modules are compiled at run time.
 * Script language: see test/conformance/README.md, section "modservice runner".
 */
#include <dlfcn.h>
#include <stddef.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <obs-internal.h>
#include <obs-module.h>
#include <util/platform.h>
#include <util/text-lookup.h>

#include "harness.h"
#include "modservice_api.h"

extern signal_handler_t *obs_service_get_signal_handler(const obs_service_t *service);
extern proc_handler_t *obs_service_get_proc_handler(const obs_service_t *service);
extern int obs_module_load_metadata(struct obs_module *mod);

/* ------------------------------------------------------------------ output with scrubbing of the temp dir */
static char tmpdir[512];
static size_t tmplen;

static void emit(const char *s)
{
	for (const char *p = s; *p;) {
		if (tmplen && !strncmp(p, tmpdir, tmplen)) {
			fputs("@TMP", stdout);
			p += tmplen;
		} else
			fputc(*p++, stdout);
	}
}

static void P(const char *fmt, ...)
{
	char buf[8192];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	emit(buf);
}

static void ms_log(int level, const char *format, va_list args, void *param)
{
	char msg[4096];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	/* dlerror text is system specific: keep only "os_dlopen(a->b)" */
	if (!strncmp(msg, "os_dlopen(", 10)) {
		char *e = strstr(msg, "): ");
		if (e)
			e[1] = 0;
	}
	for (char *p = msg; *p; p++)
		if (*p == '\n')
			*p = ' ';
	size_t n = strlen(msg);
	while (n && msg[n - 1] == ' ')
		msg[--n] = 0;
	const char *name = level == LOG_ERROR     ? "error"
			   : level == LOG_WARNING ? "warning"
			   : level == LOG_INFO    ? "info"
						  : "debug";
	P("log %s ", name);
	P("%s\n", msg);
}

/* the fake modules print through this */
void modservice_event(const char *fmt, ...)
{
	char buf[1024];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	P("ev %s\n", buf);
}

static const char *nul(const char *s)
{
	return s ? s : "(null)";
}
static const char *opt(const char *s)
{
	return strcmp(s, "-") ? s : NULL;
}

/* ------------------------------------------------------------------ generic service types */
struct tinfo {
	char id[48];
	char name[48];
	bool used;
	bool create_fail, props_null;
	int init_ret, can_connect;
	char protocol[64], outtype[64];
	struct obs_service_info info;
	size_t size;
};
static struct tinfo types[32];
static int inst_counter;

struct inst {
	int id;
	struct tinfo *t;
	char buf[64];
};

static int iid(void *data)
{
	return data ? ((struct inst *)data)->id : 0;
}
static const char *iname(void *data)
{
	return data ? ((struct inst *)data)->t->id : "-";
}

static const char *g_get_name(void *type_data)
{
	return ((struct tinfo *)type_data)->id;
}
static void *g_create(obs_data_t *settings, obs_service_t *service)
{
	struct tinfo *t = obs_service_get_type_data(service);
	if (t->create_fail) {
		P("cb %s create -> fail\n", t->id);
		return NULL;
	}
	struct inst *i = bzalloc(sizeof(*i));
	i->id = ++inst_counter;
	i->t = t;
	P("cb %s create d%d settings=%s\n", t->id, i->id, obs_data_get_json(settings));
	return i;
}
static void g_destroy(void *data)
{
	P("cb %s destroy d%d\n", iname(data), iid(data));
	bfree(data);
}
static void g_update(void *data, obs_data_t *s)
{
	P("cb %s update d%d settings=%s\n", iname(data), iid(data), obs_data_get_json(s));
}
static void g_activate(void *data, obs_data_t *s)
{
	P("cb %s activate d%d settings=%s\n", iname(data), iid(data), obs_data_get_json(s));
}
static void g_deactivate(void *data)
{
	P("cb %s deactivate d%d\n", iname(data), iid(data));
}
static bool g_initialize(void *data, obs_output_t *out)
{
	P("cb %s initialize d%d output=%s\n", iname(data), iid(data), out ? "set" : "null");
	return data ? ((struct inst *)data)->t->init_ret != 0 : true;
}
static void g_apply(void *data, obs_data_t *v, obs_data_t *a)
{
	P("cb %s apply_encoder_settings d%d video=%s", iname(data), iid(data), v ? obs_data_get_json(v) : "null");
	P(" audio=%s\n", a ? obs_data_get_json(a) : "null");
}
static void g_defaults(obs_data_t *s)
{
	P("cb get_defaults\n");
	obs_data_set_default_string(s, "url", "rtmp://default");
	obs_data_set_default_int(s, "n", 7);
}
static obs_properties_t *g_props(void *data)
{
	P("cb get_properties d%d\n", iid(data));
	if (data && ((struct inst *)data)->t->props_null)
		return NULL;
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_text(p, "url", "Url", OBS_TEXT_DEFAULT);
	obs_properties_add_int(p, "n", "N", 0, 100, 1);
	return p;
}
static obs_properties_t *g_props_class(void *data)
{
	(void)data;
	P("cb get_properties class\n");
	obs_properties_t *p = obs_properties_create();
	obs_properties_add_text(p, "url", "Url", OBS_TEXT_DEFAULT);
	obs_properties_add_int(p, "n", "N", 0, 100, 1);
	return p;
}
static const char *g_outtype(void *data)
{
	return ((struct inst *)data)->t->outtype;
}
static void g_res(void *data, struct obs_service_resolution **r, size_t *c)
{
	static struct obs_service_resolution res[2] = {{1920, 1080}, {1280, 720}};
	(void)data;
	*r = res;
	*c = 2;
}
static void g_fps(void *data, int *fps)
{
	(void)data;
	*fps = 60;
}
static void g_br(void *data, int *v, int *a)
{
	(void)data;
	if (v)
		*v = 6000;
	if (a)
		*a = 160;
}
static const char **g_vc(void *data)
{
	static const char *c[] = {"h264", "hevc", NULL};
	(void)data;
	return c;
}
static const char **g_ac(void *data)
{
	static const char *c[] = {"aac", NULL};
	(void)data;
	return c;
}
static const char *g_proto(void *data)
{
	return ((struct inst *)data)->t->protocol;
}
static const char *g_ci(void *data, uint32_t type)
{
	struct inst *i = data;
	snprintf(i->buf, sizeof(i->buf), "ci%u", type);
	return i->buf;
}
static bool g_can(void *data)
{
	return ((struct inst *)data)->t->can_connect != 0;
}
static void g_free_td(void *td)
{
	P("cb %s free_type_data\n", ((struct tinfo *)td)->id);
}

#define FIELDS(X) \
	X(id) X(get_name) X(create) X(destroy) X(activate) X(deactivate) X(update) X(get_defaults) X(get_properties) \
	X(initialize) X(get_url) X(get_key) X(get_username) X(get_password) X(deprecated_1) X(apply_encoder_settings) \
	X(type_data) X(free_type_data) X(get_output_type) X(get_supported_resolutions) X(get_max_fps) \
	X(get_max_bitrate) X(get_supported_video_codecs) X(get_protocol) X(get_supported_audio_codecs) \
	X(get_connect_info) X(can_try_to_connect)

static struct tinfo *find_type(const char *name)
{
	for (int i = 0; i < 32; i++)
		if (types[i].used && !strcmp(types[i].name, name))
			return &types[i];
	return NULL;
}

static bool field_info(struct tinfo *t, const char *f, size_t *off, size_t *sz, void **ptr)
{
#define X(n)                                                                   \
	if (!strcmp(f, #n)) {                                                  \
		*off = offsetof(struct obs_service_info, n);                   \
		*sz = sizeof(t->info.n);                                       \
		*ptr = &t->info.n;                                             \
		return true;                                                   \
	}
	FIELDS(X)
#undef X
	return false;
}

static void svc_def(struct script_line *sl)
{
	script_need(sl, 3);
	struct tinfo *t = find_type(sl->argv[1]);
	if (!t) {
		for (int i = 0; i < 32; i++)
			if (!types[i].used) {
				t = &types[i];
				break;
			}
	}
	memset(t, 0, sizeof(*t));
	t->used = true;
	snprintf(t->name, sizeof(t->name), "%s", sl->argv[1]);
	snprintf(t->id, sizeof(t->id), "%s", sl->argv[2]);
	snprintf(t->protocol, sizeof(t->protocol), "proto-%s", t->id);
	snprintf(t->outtype, sizeof(t->outtype), "out-%s", t->id);
	t->init_ret = 1;
	t->can_connect = 1;
	struct obs_service_info *i = &t->info;
	i->id = t->id;
	i->get_name = g_get_name;
	i->create = g_create;
	i->destroy = g_destroy;
	i->get_protocol = g_proto;
	i->type_data = t;
	t->size = sizeof(*i);
	for (int a = 3; a < sl->argc; a++) {
		char *k = sl->argv[a], *v = strchr(k, '=');
		if (v)
			*v++ = 0;
		if (!strcmp(k, "update"))
			i->update = g_update;
		else if (!strcmp(k, "activate"))
			i->activate = g_activate;
		else if (!strcmp(k, "deactivate"))
			i->deactivate = g_deactivate;
		else if (!strcmp(k, "initialize")) {
			i->initialize = g_initialize;
			if (v)
				t->init_ret = atoi(v);
		} else if (!strcmp(k, "apply"))
			i->apply_encoder_settings = g_apply;
		else if (!strcmp(k, "defaults"))
			i->get_defaults = g_defaults;
		else if (!strcmp(k, "props"))
			i->get_properties = g_props_class;
		else if (!strcmp(k, "props_null")) {
			i->get_properties = g_props;
			t->props_null = true;
		} else if (!strcmp(k, "output_type")) {
			i->get_output_type = g_outtype;
			if (v)
				snprintf(t->outtype, sizeof(t->outtype), "%s", v);
		} else if (!strcmp(k, "resolutions"))
			i->get_supported_resolutions = g_res;
		else if (!strcmp(k, "max_fps"))
			i->get_max_fps = g_fps;
		else if (!strcmp(k, "max_bitrate"))
			i->get_max_bitrate = g_br;
		else if (!strcmp(k, "vcodecs"))
			i->get_supported_video_codecs = g_vc;
		else if (!strcmp(k, "acodecs"))
			i->get_supported_audio_codecs = g_ac;
		else if (!strcmp(k, "connect_info"))
			i->get_connect_info = g_ci;
		else if (!strcmp(k, "can_connect")) {
			i->can_try_to_connect = g_can;
			if (v)
				t->can_connect = atoi(v);
		} else if (!strcmp(k, "free_type_data"))
			i->free_type_data = g_free_td;
		else if (!strcmp(k, "create_fail"))
			t->create_fail = true;
		else if (!strcmp(k, "protocol") && v)
			snprintf(t->protocol, sizeof(t->protocol), "%s", v);
		else if (!strcmp(k, "no_name"))
			i->get_name = NULL;
		else if (!strcmp(k, "no_create"))
			i->create = NULL;
		else if (!strcmp(k, "no_destroy"))
			i->destroy = NULL;
		else if (!strcmp(k, "no_protocol"))
			i->get_protocol = NULL;
		else if (!strcmp(k, "no_id"))
			; /* keeps id (id is read by the dup check before anything else) */
		else if (!strcmp(k, "no_type_data"))
			i->type_data = NULL;
		else {
			fprintf(stderr, "svc_def: bad option %s\n", k);
			exit(2);
		}
	}
}

/* size spec: full | N | upto:FIELD (struct truncated right after FIELD) | before:FIELD */
static size_t parse_size(struct tinfo *t, const char *s)
{
	if (!strcmp(s, "full"))
		return sizeof(struct obs_service_info);
	if (!strncmp(s, "upto:", 5) || !strncmp(s, "before:", 7)) {
		bool upto = s[0] == 'u';
		size_t off, sz;
		void *p;
		if (!field_info(t, strchr(s, ':') + 1, &off, &sz, &p)) {
			fprintf(stderr, "bad field %s\n", s);
			exit(2);
		}
		return upto ? off + sz : off;
	}
	return (size_t)strtoull(s, NULL, 0);
}

/* ------------------------------------------------------------------ handles */
static obs_service_t *svcs[32];
static char *svc_names[32];
static obs_weak_service_t *weaks[32];
static char *weak_names[32];
static struct obs_output *outs[8];
static char *out_names[8];
static obs_module_t *mods[32];
static char *mod_names[32];
static lookup_t *lookups[16];
static char *lk_names[16];

static int slot(char **names, int n, const char *name, bool create)
{
	for (int i = 0; i < n; i++)
		if (names[i] && !strcmp(names[i], name))
			return i;
	if (!create)
		return -1;
	for (int i = 0; i < n; i++)
		if (!names[i]) {
			names[i] = strdup(name);
			return i;
		}
	fprintf(stderr, "out of handle slots\n");
	exit(2);
}
#define SVC(name) (svcs[slot(svc_names, 32, name, true)])
#define WEAK(name) (weaks[slot(weak_names, 32, name, true)])
#define OUT(name) (outs[slot(out_names, 8, name, true)])
static obs_module_t *find_any(const char *name);
static obs_module_t *mod_h(const char *name)
{
	int i = slot(mod_names, 32, name, false);
	if (i >= 0 && mods[i])
		return mods[i];
	obs_module_t *m = find_any(name);
	if (!m) {
		fprintf(stderr, "unknown module handle %s\n", name);
		exit(2);
	}
	return m;
}
#define MOD(name) mod_h(name)

static const char *ynb(bool b)
{
	return b ? "1" : "0";
}

/* ------------------------------------------------------------------ expand @TMP */
static char *expand(const char *s)
{
	static char bufs[8][1024];
	static int n;
	char *b = bufs[n++ & 7];
	if (s && !strncmp(s, "@TMP", 4))
		snprintf(b, 1024, "%s%s", tmpdir, s + 4);
	else
		snprintf(b, 1024, "%s", s ? s : "");
	return b;
}
static const char *xp(const char *s)
{
	if (!strcmp(s, "-"))
		return NULL;
	return expand(s);
}

/* ------------------------------------------------------------------ fake modules */
static const char *mod_template =
	"#include <obs-module.h>\n"
	"#include <util/text-lookup.h>\n"
	"extern void modservice_event(const char *fmt, ...);\n"
	"extern void modservice_register(const char *kind, const char *id);\n"
	"#ifdef BADSYM\n extern void ms_missing_symbol(void);\n#endif\n"
	"static obs_module_t *ptr;\n"
	"obs_module_t *obs_current_module(void) { return ptr; }\n"
	"#ifndef NOSET\n MODULE_EXPORT void obs_module_set_pointer(obs_module_t *m);\n"
	" void obs_module_set_pointer(obs_module_t *m) { ptr = m; modservice_event(\"mod %s set_pointer\", NAME); }\n#endif\n"
	"#ifndef NOVER\n MODULE_EXPORT uint32_t obs_module_ver(void);\n"
	" uint32_t obs_module_ver(void) { return VER; }\n#endif\n"
	"__attribute__((constructor)) static void ctor(void) { modservice_event(\"mod %s ctor\", NAME); }\n"
	"#ifndef NOLOAD\n"
	"bool obs_module_load(void)\n{\n modservice_event(\"mod %s load\", NAME);\n"
	"#ifdef BADSYM\n ms_missing_symbol();\n#endif\n"
	"#ifdef REG\n REG\n#endif\n return LOADRET;\n}\n#endif\n"
	"#ifdef UNLOAD\n void obs_module_unload(void) { modservice_event(\"mod %s unload\", NAME); }\n#endif\n"
	"#ifdef POSTLOAD\n void obs_module_post_load(void) { modservice_event(\"mod %s post_load\", NAME); }\n#endif\n"
	"#ifdef DEFLOC\n OBS_MODULE_USE_DEFAULT_LOCALE(NAME, \"en-US\")\n#endif\n"
	"#ifdef SETLOC\n void obs_module_set_locale(const char *l) { modservice_event(\"mod %s set_locale %s\", NAME, l ? l : \"(null)\"); }\n#endif\n"
	"#ifdef FREELOC\n void obs_module_free_locale(void) { modservice_event(\"mod %s free_locale\", NAME); }\n#endif\n"
	"#ifdef GETSTR\n bool obs_module_get_string(const char *k, const char **o) { if (!strcmp(k, \"a\")) { *o = \"A!\"; return true; } return false; }\n#endif\n"
	"#ifdef MODNAME\n const char *obs_module_name(void) { return MODNAME; }\n#endif\n"
	"#ifdef MODDESC\n const char *obs_module_description(void) { return MODDESC; }\n#endif\n"
	"#ifdef MODAUTHOR\n const char *obs_module_author(void) { return MODAUTHOR; }\n#endif\n";

static void mkdir_p(const char *path)
{
	char tmp[1024];
	snprintf(tmp, sizeof(tmp), "%s", path);
	for (char *p = tmp + 1; *p; p++)
		if (*p == '/') {
			*p = 0;
			mkdir(tmp, 0755);
			*p = '/';
		}
	mkdir(tmp, 0755);
}

static void write_file(const char *path, const char *text)
{
	char dir[1024];
	snprintf(dir, sizeof(dir), "%s", path);
	char *sl = strrchr(dir, '/');
	if (sl) {
		*sl = 0;
		mkdir_p(dir);
	}
	FILE *f = fopen(path, "wb");
	if (!f) {
		perror(path);
		exit(2);
	}
	fputs(text, f);
	fclose(f);
}

/* mkmod PATH OPT... : compile a fake plugin to PATH (".so" appended if missing) */
static void cmd_mkmod(struct script_line *sl)
{
	script_need(sl, 3);
	const char *name = sl->argv[1];
	const char *path = expand(sl->argv[2]);
	char defs[2048] = "";
	char reg[2048] = "";
	char ver[128];
	snprintf(ver, sizeof(ver), "LIBOBS_API_VER");
	char loadret[8] = "true";
	for (int a = 3; a < sl->argc; a++) {
		char *k = sl->argv[a], *v = strchr(k, '=');
		if (v)
			*v++ = 0;
		char one[1024];
		if (!strcmp(k, "ver")) {
			unsigned maj = LIBOBS_API_MAJOR_VER, min = LIBOBS_API_MINOR_VER, pat = LIBOBS_API_PATCH_VER;
			if (!strcmp(v, "api"))
				;
			else if (!strcmp(v, "minor+1"))
				min++;
			else if (!strcmp(v, "major+1"))
				maj++;
			else if (!strcmp(v, "patch+5"))
				pat += 5;
			else if (!strcmp(v, "major-1"))
				maj--;
			else if (!strcmp(v, "zero")) {
				maj = min = pat = 0;
			} else {
				fprintf(stderr, "bad ver\n");
				exit(2);
			}
			snprintf(ver, sizeof(ver), "((%uu<<24)|(%uu<<16)|%uu)", maj, min, pat);
		} else if (!strcmp(k, "load")) {
			snprintf(loadret, sizeof(loadret), "%s", atoi(v) ? "true" : "false");
		} else if (!strcmp(k, "svc") || !strcmp(k, "out") || !strcmp(k, "enc") || !strcmp(k, "src")) {
			char *list = strdup(v);
			for (char *id = strtok(list, ","); id; id = strtok(NULL, ",")) {
				char r[256];
				snprintf(r, sizeof(r), "modservice_register(\"%s\", \"%s\"); ", k, id);
				strcat(reg, r);
			}
			free(list);
		} else if (!strcmp(k, "modname") || !strcmp(k, "desc") || !strcmp(k, "author")) {
			const char *m = !strcmp(k, "modname") ? "MODNAME" : !strcmp(k, "desc") ? "MODDESC" : "MODAUTHOR";
			snprintf(one, sizeof(one), "-D%s='\"%s\"' ", m, v);
			strcat(defs, one);
		} else {
			const char *m = !strcmp(k, "noload")      ? "NOLOAD"
					: !strcmp(k, "noset")     ? "NOSET"
					: !strcmp(k, "nover")     ? "NOVER"
					: !strcmp(k, "unload")    ? "UNLOAD"
					: !strcmp(k, "postload")  ? "POSTLOAD"
					: !strcmp(k, "loc")       ? "DEFLOC"
					: !strcmp(k, "setloc")    ? "SETLOC"
					: !strcmp(k, "freeloc")   ? "FREELOC"
					: !strcmp(k, "getstr")    ? "GETSTR"
					: !strcmp(k, "badsym")    ? "BADSYM"
								  : NULL;
			if (!m) {
				fprintf(stderr, "mkmod: bad option %s\n", k);
				exit(2);
			}
			snprintf(one, sizeof(one), "-D%s ", m);
			strcat(defs, one);
		}
	}
	char src[1100];
	snprintf(src, sizeof(src), "%s/ms-%s.c", tmpdir, name);
	write_file(src, mod_template);
	char out[1024];
	snprintf(out, sizeof(out), "%s", path);
	{
		char dir[1024];
		snprintf(dir, sizeof(dir), "%s", out);
		char *s = strrchr(dir, '/');
		if (s) {
			*s = 0;
			mkdir_p(dir);
		}
	}
	/* libobs include dir derived from this file's path (…/test/conformance/gen/modservice_runner.c) */
	char inc[1024], gen[1024];
	snprintf(inc, sizeof(inc), "%s", __FILE__);
	snprintf(gen, sizeof(gen), "%s", __FILE__);
	char *g = strstr(inc, "test/conformance/gen/");
	if (g)
		strcpy(g, "libobs");
	else
		strcpy(inc, "../../libobs");
	char *gs = strrchr(gen, '/');
	if (gs)
		*gs = 0;
	else
		strcpy(gen, ".");
	char cmd[16384];
	const char *cc = getenv("CC") ? getenv("CC") : "cc";
	/* Same extra includes (e.g. simde) as the runner build: the fake plugin includes
	 * libobs headers, which need them on machines where they are not in the default path. */
	const char *extra = getenv("CONFORMANCE_EXTRA_CFLAGS");
	snprintf(cmd, sizeof(cmd),
		 "%s -shared -fPIC -w -std=gnu11 -I'%s' -I'%s' -DNAME='\"%s\"' -DVER='%s' -DLOADRET=%s %s%s%s%s %s -o '%s' '%s'",
		 cc, inc, gen, name, ver, loadret, defs, reg[0] ? "-DREG='" : "", reg, reg[0] ? "' " : "",
		 extra ? extra : "", out, src);
	if (system(cmd) != 0) {
		fprintf(stderr, "mkmod failed: %s\n", cmd);
		exit(2);
	}
	P("mkmod %s ok\n", name);
}

/* registration helpers called from fake modules during obs_module_load */
static void reg_service_generic(const char *id)
{
	struct tinfo *t = find_type(id);
	if (!t) {
		char *a[] = {"svc_def", (char *)id, (char *)id};
		struct script_line sl;
		sl.argc = 3;
		for (int i = 0; i < 3; i++)
			sl.argv[i] = a[i];
		svc_def(&sl);
		t = find_type(id);
	}
	obs_register_service_s(&t->info, sizeof(t->info));
}

static const char *gen_name(void *td)
{
	return (const char *)td;
}
static void *gen_create(obs_data_t *s, obs_output_t *o)
{
	(void)s;
	(void)o;
	return NULL;
}
static void gen_destroy(void *d)
{
	(void)d;
}
static bool gen_start(void *d)
{
	(void)d;
	return true;
}
static void gen_stop(void *d, uint64_t t)
{
	(void)d;
	(void)t;
}
static void gen_pkt(void *d, struct encoder_packet *p)
{
	(void)d;
	(void)p;
}
static void gen_rv(void *d, struct video_data *f)
{
	(void)d;
	(void)f;
}
static void gen_ra(void *d, struct audio_data *f)
{
	(void)d;
	(void)f;
}
static void gen_ra2(void *d, size_t idx, struct audio_data *f)
{
	(void)d;
	(void)idx;
	(void)f;
}
static void *gen_ecreate(obs_data_t *s, obs_encoder_t *e)
{
	(void)s;
	(void)e;
	return NULL;
}
static bool gen_encode(void *d, struct encoder_frame *f, struct encoder_packet *p, bool *r)
{
	(void)d;
	(void)f;
	(void)p;
	(void)r;
	return true;
}
static size_t gen_fs(void *d)
{
	(void)d;
	return 1024;
}
static void *gen_screate(obs_data_t *s, obs_source_t *src)
{
	(void)s;
	(void)src;
	return NULL;
}

static char gen_ids[64][48];
static int gen_n;
static const char *keep(const char *s)
{
	snprintf(gen_ids[gen_n % 64], 48, "%s", s);
	return gen_ids[gen_n++ % 64];
}

void modservice_register(const char *kind, const char *id)
{
	if (!strcmp(kind, "svc")) {
		reg_service_generic(id);
	} else if (!strcmp(kind, "out")) {
		struct obs_output_info i = {0};
		i.id = keep(id);
		i.flags = OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED | OBS_OUTPUT_SERVICE;
		i.get_name = gen_name;
		i.create = gen_create;
		i.destroy = gen_destroy;
		i.start = gen_start;
		i.stop = gen_stop;
		i.encoded_packet = gen_pkt;
		i.protocols = "RTMP;RTMPS";
		i.type_data = (void *)i.id;
		obs_register_output_s(&i, sizeof(i));
	} else if (!strcmp(kind, "enc")) {
		struct obs_encoder_info i = {0};
		i.id = keep(id);
		i.type = OBS_ENCODER_VIDEO;
		i.get_name = gen_name;
		i.create = gen_ecreate;
		i.destroy = gen_destroy;
		i.encode = gen_encode;
		obs_register_encoder_s(&i, sizeof(i));
	} else if (!strcmp(kind, "src")) {
		struct obs_source_info i = {0};
		i.id = keep(id);
		i.type = OBS_SOURCE_TYPE_INPUT;
		i.output_flags = OBS_SOURCE_AUDIO;
		i.get_name = gen_name;
		i.create = gen_screate;
		i.destroy = gen_destroy;
		obs_register_source_s(&i, sizeof(i));
	}
}

/* ------------------------------------------------------------------ module discovery callbacks */
static void find_cb(void *param, const struct obs_module_info2 *info)
{
	const char *show_type = param;
	P("found name=%s bin=%s data=%s", info->name, info->bin_path, info->data_path);
	if (show_type)
		P(" type=%d", (int)info->type);
	P("\n");
}

static void enum_cb(void *param, obs_module_t *m)
{
	(void)param;
	P("enum %s\n", nul(obs_get_module_file_name(m)));
}

static void print_list(const char *label, obs_module_t *first)
{
	P("%s", label);
	for (obs_module_t *m = first; m; m = m->next)
		P(" %s", nul(m->mod_name));
	P("\n");
}

static void print_strs(const char *label, char **a, size_t n)
{
	P("%s %zu", label, n);
	for (size_t i = 0; i < n; i++)
		P(" %s", a[i]);
	P("\n");
}

static const char *state_name(int s)
{
	static const char *n[] = {"invalid", "enabled", "missing", "disabled", "disabled_safe", "failed_to_open",
				  "failed_to_initialize"};
	return s >= 0 && s <= 6 ? n[s] : "?";
}

static obs_module_t *find_any(const char *name)
{
	obs_module_t *m = obs_get_module(name);
	return m ? m : obs_get_disabled_module(name);
}

static obs_data_t *json_arg(const char *s)
{
	if (!strcmp(s, "-"))
		return NULL;
	obs_data_t *d = obs_data_create_from_json(s);
	if (!d) {
		fprintf(stderr, "bad json %s\n", s);
		exit(2);
	}
	return d;
}

static void print_str_list(const char **l)
{
	if (!l) {
		P(" null\n");
		return;
	}
	for (; *l; l++)
		P(" %s", *l);
	P("\n");
}

/* ------------------------------------------------------------------ command dispatch */
static int run_cmd(struct script_line *sl);
extern obs_module_t *loadingModule;

static int run_try(struct script_line *sl)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		struct script_line sub = *sl;
		sub.argc--;
		for (int i = 0; i < sub.argc; i++)
			sub.argv[i] = sl->argv[i + 1];
		P("try-begin\n");
		run_cmd(&sub);
		fflush(stdout);
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	P("try-end %s\n", (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? "ok" : "crash");
	return 0;
}

static int run_cmd(struct script_line *sl)
{
	const char *c = sl->argv[0];
	char **v = sl->argv;
	for (int i = 1; i < sl->argc; i++)
		if (!strncmp(v[i], "@TMP", 4))
			v[i] = expand(v[i]);

	if (!strcmp(c, "try"))
		return run_try(sl);
	/* ------------- fake core */
	else if (!strcmp(c, "core")) {
		script_need(sl, 3);
		ms_core_set(opt(v[1]), opt(v[2]));
	} else if (!strcmp(c, "core_null")) {
		script_need(sl, 2);
		ms_core_null(atoi(v[1]));
	} else if (!strcmp(c, "core_modules")) { /* core_modules BIN_TPL DATA_TPL NAME... */
		script_need(sl, 3);
		ms_core_modules_set(v + 3, sl->argc - 3, v[1], v[2]);
	} else if (!strcmp(c, "mkdir")) {
		script_need(sl, 2);
		mkdir_p(v[1]);
	} else if (!strcmp(c, "write")) {
		script_need(sl, 3);
		write_file(v[1], v[2]);
	} else if (!strcmp(c, "mkmod")) {
		cmd_mkmod(sl);
	}
	/* ------------- service types */
	else if (!strcmp(c, "svc_def")) {
		svc_def(sl);
	} else if (!strcmp(c, "svc_register")) { /* svc_register TYPE [SIZE] */
		script_need(sl, 2);
		struct tinfo *t = find_type(v[1]);
		size_t size = sl->argc > 2 ? parse_size(t, v[2]) : sizeof(t->info);
		P("register %s size=%s\n", v[1], sl->argc > 2 ? v[2] : "full");
		obs_register_service_s(&t->info, size);
		P("service_types=%zu\n", obs->service_types.num);
	} else if (!strcmp(c, "svc_types")) {
		P("types");
		for (size_t i = 0; i < obs->service_types.num; i++)
			P(" %s", obs->service_types.array[i].id);
		P("\n");
	}
	/* ------------- service objects */
	else if (!strcmp(c, "service_create") || !strcmp(c, "service_create_private")) {
		script_need(sl, 5); /* H ID NAME|- SETTINGS|- [HOTKEY|-] */
		obs_data_t *s = json_arg(v[4]);
		obs_service_t *svc;
		if (!strcmp(c, "service_create"))
			svc = obs_service_create(v[2], opt(v[3]), s, sl->argc > 5 ? json_arg(v[5]) : NULL);
		else
			svc = obs_service_create_private(v[2], opt(v[3]), s);
		obs_data_release(s);
		svcs[slot(svc_names, 32, v[1], true)] = svc;
		P("create %s -> %s\n", v[1], svc ? "ok" : "null");
	} else if (!strcmp(c, "service_info")) {
		script_need(sl, 2);
		obs_service_t *s = SVC(v[1]);
		P("info %s name=%s type=%s id=%s", v[1], nul(obs_service_get_name(s)), nul(obs_service_get_type(s)),
		  nul(obs_service_get_id(s)));
		P(" signals=%s procs=%s\n", ynb(obs_service_get_signal_handler(s) != NULL),
		  ynb(obs_service_get_proc_handler(s) != NULL));
	} else if (!strcmp(c, "service_state")) {
		script_need(sl, 2);
		obs_service_t *s = SVC(v[1]);
		P("state %s active=%s destroy=%s output=%s listed=%s\n", v[1], ynb(s->active), ynb(s->destroy),
		  s->output ? "set" : "null", ynb(s->context.prev_next != NULL));
	} else if (!strcmp(c, "service_list")) {
		P("list");
		for (struct obs_context_data *d = (struct obs_context_data *)obs->data.first_service; d; d = d->next)
			P(" %s", d->name);
		P("\n");
	} else if (!strcmp(c, "service_update")) {
		script_need(sl, 3);
		obs_data_t *d = json_arg(v[2]);
		obs_service_update(SVC(v[1]), d);
		obs_data_release(d);
	} else if (!strcmp(c, "service_settings")) {
		script_need(sl, 2);
		obs_data_t *d = obs_service_get_settings(SVC(v[1]));
		P("settings %s %s\n", v[1], d ? obs_data_get_json(d) : "null");
		obs_data_release(d);
	} else if (!strcmp(c, "service_destroy")) {
		script_need(sl, 2);
		obs_service_destroy(SVC(v[1]));
	} else if (!strcmp(c, "service_release")) {
		script_need(sl, 2);
		obs_service_release(SVC(v[1]));
	} else if (!strcmp(c, "service_get_ref")) { /* service_get_ref NEW H */
		script_need(sl, 3);
		svcs[slot(svc_names, 32, v[1], true)] = obs_service_get_ref(SVC(v[2]));
		P("get_ref %s -> %s\n", v[1], SVC(v[1]) ? "ok" : "null");
	} else if (!strcmp(c, "service_weak")) { /* service_weak W H */
		script_need(sl, 3);
		weaks[slot(weak_names, 32, v[1], true)] = obs_service_get_weak_service(SVC(v[2]));
		P("weak %s -> %s\n", v[1], WEAK(v[1]) ? "ok" : "null");
	} else if (!strcmp(c, "weak_to_service")) { /* weak_to_service NEW W */
		script_need(sl, 3);
		svcs[slot(svc_names, 32, v[1], true)] = obs_weak_service_get_service(WEAK(v[2]));
		P("weak_to_service %s -> %s\n", v[1], SVC(v[1]) ? "ok" : "null");
	} else if (!strcmp(c, "weak_addref")) {
		obs_weak_service_addref(WEAK(v[1]));
	} else if (!strcmp(c, "weak_release")) {
		obs_weak_service_release(WEAK(v[1]));
	} else if (!strcmp(c, "weak_refs")) { /* weak_refs W H|- */
		script_need(sl, 3);
		P("weak_refs %s %s -> %s\n", v[1], v[2],
		  ynb(obs_weak_service_references_service(strcmp(v[1], "-") ? WEAK(v[1]) : NULL,
							 strcmp(v[2], "-") ? SVC(v[2]) : NULL)));
	} else if (!strcmp(c, "ref_counts")) { /* ref_counts H */
		script_need(sl, 2);
		struct obs_weak_service *w = (struct obs_weak_service *)SVC(v[1])->context.control;
		P("ref_counts %s strong=%ld weak=%ld\n", v[1], (long)w->ref.refs, (long)w->ref.weak_refs);
	} else if (!strcmp(c, "service_null_calls")) {
		/* all getters with a NULL service; every call is made (and logged) before its result is printed */
		int mfps = 123, vb = 123, ab = 123;
		struct obs_service_resolution *r = (void *)1;
		size_t n = 99;
#define NC(label, expr) \
	do {            \
		const char *r_ = (expr); \
		P("null %s -> %s\n", label, nul(r_)); \
	} while (0)
		NC("get_name", obs_service_get_name(NULL));
		NC("get_type", obs_service_get_type(NULL));
		NC("get_id", obs_service_get_id(NULL));
		NC("get_settings", obs_service_get_settings(NULL) ? "x" : NULL);
		NC("properties", obs_service_properties(NULL) ? "x" : NULL);
		NC("get_type_data", obs_service_get_type_data(NULL) ? "x" : NULL);
		NC("get_protocol", obs_service_get_protocol(NULL));
		NC("preferred_output_type", obs_service_get_preferred_output_type(NULL));
		NC("connect_info", obs_service_get_connect_info(NULL, 1));
		NC("can_try_to_connect", ynb(obs_service_can_try_to_connect(NULL)));
		NC("signal_handler", obs_service_get_signal_handler(NULL) ? "x" : NULL);
		NC("proc_handler", obs_service_get_proc_handler(NULL) ? "x" : NULL);
		obs_service_get_max_fps(NULL, &mfps);
		obs_service_get_max_bitrate(NULL, &vb, &ab);
		obs_service_get_supported_resolutions(NULL, &r, &n);
		P("null out-params fps=%d vb=%d ab=%d res=%s n=%zu\n", mfps, vb, ab, r == (void *)1 ? "untouched" : "changed",
		  n);
		obs_service_update(NULL, NULL);
		obs_service_apply_encoder_settings(NULL, NULL, NULL);
		obs_service_destroy(NULL);
		obs_service_release(NULL);
		NC("get_ref", obs_service_get_ref(NULL) ? "x" : NULL);
		NC("get_weak_service", obs_service_get_weak_service(NULL) ? "x" : NULL);
		NC("weak_get_service", obs_weak_service_get_service(NULL) ? "x" : NULL);
		obs_weak_service_addref(NULL);
		obs_weak_service_release(NULL);
		NC("initialize", ynb(obs_service_initialize(NULL, NULL)));
	} else if (!strcmp(c, "null_vcodecs")) {
		print_str_list(obs_service_get_supported_video_codecs(NULL));
	} else if (!strcmp(c, "null_acodecs")) {
		print_str_list(obs_service_get_supported_audio_codecs(NULL));
	}
	/* ------------- by-id queries */
	else if (!strcmp(c, "display_name")) {
		script_need(sl, 2);
		P("display_name %s -> %s\n", v[1], nul(obs_service_get_display_name(v[1])));
	} else if (!strcmp(c, "defaults_by_id")) {
		script_need(sl, 2);
		obs_data_t *d = obs_service_defaults(v[1]);
		P("defaults %s ->", v[1]);
		if (d)
			P(" json=%s url=%s n=%lld url_default=%s\n", obs_data_get_json(d), obs_data_get_string(d, "url"),
			  obs_data_get_int(d, "n"), ynb(obs_data_has_default_value(d, "url")));
		else
			P(" null\n");
		obs_data_release(d);
	} else if (!strcmp(c, "props_by_id") || !strcmp(c, "props")) {
		script_need(sl, 2);
		bool byid = !strcmp(c, "props_by_id");
		obs_properties_t *p = byid ? obs_get_service_properties(v[1]) : obs_service_properties(SVC(v[1]));
		P("%s %s ->", c, v[1]);
		if (!p)
			P(" null");
		else {
			for (obs_property_t *x = obs_properties_first(p); x; obs_property_next(&x)) {
				P(" %s:%d", obs_property_name(x), (int)obs_property_get_type(x));
			}
		}
		P("\n");
		obs_properties_destroy(p);
	} else if (!strcmp(c, "service_module")) {
		script_need(sl, 2);
		obs_module_t *m = obs_service_get_module(v[1]);
		P("service_module %s -> %s state=%s\n", v[1], m ? nul(m->mod_name) : "null",
		  state_name(obs_service_load_state(v[1])));
	}
	/* ------------- instance queries */
	else if (!strcmp(c, "q")) { /* q H WHAT [ARG] */
		script_need(sl, 3);
		obs_service_t *s = SVC(v[1]);
		const char *w = v[2];
		if (strcmp(w, "initialize"))
			P("q %s %s ->", v[1], w);
		else
			P("q %s %s%s ->", v[1], w, sl->argc > 3 ? " out" : " null");
		if (!strcmp(w, "protocol"))
			P(" %s\n", nul(obs_service_get_protocol(s)));
		else if (!strcmp(w, "outtype"))
			P(" %s\n", nul(obs_service_get_preferred_output_type(s)));
		else if (!strcmp(w, "connect_info"))
			P(" %s\n", nul(obs_service_get_connect_info(s, (uint32_t)strtoul(v[3], NULL, 0))));
		else if (!strcmp(w, "can_connect"))
			P(" %s\n", ynb(obs_service_can_try_to_connect(s)));
		else if (!strcmp(w, "fps")) {
			int f = -5;
			obs_service_get_max_fps(s, &f);
			P(" %d\n", f);
		} else if (!strcmp(w, "bitrate")) {
			int a = -5, b = -5;
			obs_service_get_max_bitrate(s, &a, &b);
			P(" %d %d\n", a, b);
		} else if (!strcmp(w, "bitrate_null")) {
			obs_service_get_max_bitrate(s, NULL, NULL);
			P(" ok\n");
		} else if (!strcmp(w, "resolutions")) {
			struct obs_service_resolution *r = (void *)1;
			size_t n = 77;
			obs_service_get_supported_resolutions(s, &r, &n);
			P(" n=%zu", n);
			for (size_t i = 0; i < n; i++)
				P(" %dx%d", r[i].cx, r[i].cy);
			P("%s\n", n == 0 && r ? " (ptr set)" : "");
		} else if (!strcmp(w, "vcodecs"))
			print_str_list(obs_service_get_supported_video_codecs(s));
		else if (!strcmp(w, "acodecs"))
			print_str_list(obs_service_get_supported_audio_codecs(s));
		else if (!strcmp(w, "typedata"))
			P(" %s\n", obs_service_get_type_data(s) ? "set" : "null");
		else if (!strcmp(w, "initialize")) {
			P(" %s\n", ynb(obs_service_initialize(s, sl->argc > 3 ? OUT(v[3]) : NULL)));
		} else {
			fprintf(stderr, "bad q\n");
			exit(2);
		}
	} else if (!strcmp(c, "apply_enc")) { /* apply_enc H VIDEO|- AUDIO|- */
		script_need(sl, 4);
		obs_data_t *a = json_arg(v[2]), *b = json_arg(v[3]);
		obs_service_apply_encoder_settings(SVC(v[1]), a, b);
		obs_data_release(a);
		obs_data_release(b);
	}
	/* ------------- output wiring */
	else if (!strcmp(c, "out_new")) {
		script_need(sl, 2);
		outs[slot(out_names, 8, v[1], true)] = bzalloc(sizeof(struct obs_output));
	} else if (!strcmp(c, "attach")) { /* attach H O : what obs_output_set_service does to the service side */
		script_need(sl, 3);
		obs_service_t *s = SVC(v[1]);
		struct obs_output *o = OUT(v[2]);
		s->output = o;
		o->service = s;
	} else if (!strcmp(c, "out_service")) {
		script_need(sl, 2);
		P("out_service %s -> %s\n", v[1], OUT(v[1])->service ? "set" : "null");
	} else if (!strcmp(c, "activate")) {
		obs_service_activate(SVC(v[1]));
	} else if (!strcmp(c, "deactivate")) {
		script_need(sl, 3);
		obs_service_deactivate(SVC(v[1]), atoi(v[2]));
	}
	/* ------------- registration of other kinds */
	else if (!strcmp(c, "out_register")) { /* out_register ID FLAGS|- [missing=a,b] [size=N|full] */
		script_need(sl, 3);
		struct obs_output_info i = {0};
		i.id = keep(v[1]);
		i.get_name = gen_name;
		i.create = gen_create;
		i.destroy = gen_destroy;
		i.start = gen_start;
		i.stop = gen_stop;
		i.encoded_packet = gen_pkt;
		i.raw_video = gen_rv;
		i.raw_audio = gen_ra;
		i.raw_audio2 = gen_ra2;
		i.protocols = "RTMP;RTMPS";
		i.type_data = (void *)i.id;
		i.free_type_data = NULL;
		const char *f = v[2];
		for (; *f && strcmp(f, "-"); f++)
			i.flags |= *f == 'S'   ? OBS_OUTPUT_SERVICE
				   : *f == 'E' ? OBS_OUTPUT_ENCODED
				   : *f == 'V' ? OBS_OUTPUT_VIDEO
				   : *f == 'A' ? OBS_OUTPUT_AUDIO
				   : *f == 'M' ? OBS_OUTPUT_MULTI_TRACK
					       : 0;
		for (int a = 3; a < sl->argc; a++) {
			if (!strncmp(v[a], "missing=", 8)) {
				char *list = strdup(v[a] + 8);
				for (char *k = strtok(list, ","); k; k = strtok(NULL, ",")) {
					if (!strcmp(k, "get_name")) i.get_name = NULL;
					else if (!strcmp(k, "create")) i.create = NULL;
					else if (!strcmp(k, "destroy")) i.destroy = NULL;
					else if (!strcmp(k, "start")) i.start = NULL;
					else if (!strcmp(k, "stop")) i.stop = NULL;
					else if (!strcmp(k, "protocols")) i.protocols = NULL;
					else if (!strcmp(k, "encoded_packet")) i.encoded_packet = NULL;
					else if (!strcmp(k, "raw_video")) i.raw_video = NULL;
					else if (!strcmp(k, "raw_audio")) i.raw_audio = NULL;
					else if (!strcmp(k, "raw_audio2")) i.raw_audio2 = NULL;
				}
				free(list);
			}
		}
		size_t size = sizeof(i);
		for (int a = 3; a < sl->argc; a++)
			if (!strncmp(v[a], "size=", 5))
				size = strcmp(v[a] + 5, "full") ? (size_t)strtoull(v[a] + 5, NULL, 0) : sizeof(i);
		obs_register_output_s(&i, size);
		P("outputs=%zu protocols", obs->output_types.num);
		for (size_t k = 0; k < obs->data.protocols.num; k++)
			P(" %s", obs->data.protocols.array[k]);
		P("\n");
	} else if (!strcmp(c, "enc_register")) { /* enc_register ID video|audio CAPS|- [missing=…] */
		script_need(sl, 4);
		struct obs_encoder_info i = {0};
		i.id = keep(v[1]);
		i.type = !strcmp(v[2], "audio") ? OBS_ENCODER_AUDIO : OBS_ENCODER_VIDEO;
		i.get_name = gen_name;
		i.create = gen_ecreate;
		i.destroy = gen_destroy;
		i.encode = gen_encode;
		i.get_frame_size = gen_fs;
		i.encode_texture = (void *)gen_encode;
		i.encode_texture2 = (void *)gen_encode;
		for (const char *f = v[3]; *f && strcmp(f, "-"); f++)
			i.caps |= *f == 'P' ? OBS_ENCODER_CAP_PASS_TEXTURE : *f == 'S' ? OBS_ENCODER_CAP_SCALING : 0;
		for (int a = 4; a < sl->argc; a++)
			if (!strncmp(v[a], "missing=", 8)) {
				char *list = strdup(v[a] + 8);
				for (char *k = strtok(list, ","); k; k = strtok(NULL, ",")) {
					if (!strcmp(k, "get_name")) i.get_name = NULL;
					else if (!strcmp(k, "create")) i.create = NULL;
					else if (!strcmp(k, "destroy")) i.destroy = NULL;
					else if (!strcmp(k, "encode")) i.encode = NULL;
					else if (!strcmp(k, "encode_texture")) i.encode_texture = NULL;
					else if (!strcmp(k, "encode_texture2")) i.encode_texture2 = NULL;
					else if (!strcmp(k, "get_frame_size")) i.get_frame_size = NULL;
				}
				free(list);
			}
		obs_register_encoder_s(&i, sizeof(i));
		P("encoders=%zu\n", obs->encoder_types.num);
	} else if (!strcmp(c, "src_register")) { /* src_register ID TYPE FLAGS|- VERSION [missing=…] */
		script_need(sl, 5);
		struct obs_source_info i = {0};
		i.id = keep(v[1]);
		i.type = !strcmp(v[2], "input")        ? OBS_SOURCE_TYPE_INPUT
			 : !strcmp(v[2], "filter")     ? OBS_SOURCE_TYPE_FILTER
			 : !strcmp(v[2], "transition") ? OBS_SOURCE_TYPE_TRANSITION
			 : !strcmp(v[2], "scene")      ? OBS_SOURCE_TYPE_SCENE
						       : (enum obs_source_type)99;
		for (const char *f = v[3]; *f && strcmp(f, "-"); f++)
			i.output_flags |= *f == 'V'   ? OBS_SOURCE_VIDEO
					  : *f == 'A' ? OBS_SOURCE_AUDIO
					  : *f == 'Y' ? OBS_SOURCE_ASYNC
					  : *f == 'C' ? OBS_SOURCE_COMPOSITE
						      : 0;
		i.version = (uint32_t)atoi(v[4]);
		i.get_name = gen_name;
		i.create = gen_screate;
		i.destroy = gen_destroy;
		i.audio_render = (void *)gen_encode;
		for (int a = 5; a < sl->argc; a++) {
			if (!strncmp(v[a], "missing=", 8)) {
				char *list = strdup(v[a] + 8);
				for (char *k = strtok(list, ","); k; k = strtok(NULL, ",")) {
					if (!strcmp(k, "get_name")) i.get_name = NULL;
					else if (!strcmp(k, "audio_render")) i.audio_render = NULL;
				}
				free(list);
			} else if (!strncmp(v[a], "wh=", 3)) { /* which of get_width (w) / get_height (h) are provided */
				if (strchr(v[a] + 3, 'w'))
					i.get_width = (void *)gen_fs;
				if (strchr(v[a] + 3, 'h'))
					i.get_height = (void *)gen_fs;
			}
		}
		obs_register_source_s(&i, sizeof(i));
		P("sources=%zu input=%zu filter=%zu transition=%zu", obs->source_types.num, obs->input_types.num,
		  obs->filter_types.num, obs->transition_types.num);
		for (size_t k = 0; k < obs->source_types.num; k++) {
			struct obs_source_info *x = &obs->source_types.array[k];
			if (!strcmp(x->unversioned_id, i.id))
				P(" [id=%s flags=0x%x]", x->id, x->output_flags);
		}
		P("\n");
	}
	/* ------------- modules: open / create / init / free */
	else if (!strcmp(c, "open_module")) { /* open_module M PATH DATA|- */
		script_need(sl, 4);
		obs_module_t *m = NULL;
		int code = obs_open_module(&m, opt(v[2]), opt(v[3]));
		mods[slot(mod_names, 32, v[1], true)] = code == MODULE_SUCCESS ? m : NULL;
		P("open_module %s -> %d\n", v[1], code);
	} else if (!strcmp(c, "loading")) { /* loading M|- : set the "module being loaded" global */
		script_need(sl, 2);
		loadingModule = strcmp(v[1], "-") ? MOD(v[1]) : NULL;
	} else if (!strcmp(c, "open_module_nullout")) {
		int code = obs_open_module(NULL, v[1], "d");
		P("open_module_nullout -> %d\n", code);
	} else if (!strcmp(c, "create_disabled")) { /* create_disabled M PATH DATA STATE */
		script_need(sl, 5);
		obs_module_t *m = NULL;
		bool r = obs_create_disabled_module(&m, v[2], v[3], (enum obs_module_load_state)atoi(v[4]));
		mods[slot(mod_names, 32, v[1], true)] = m;
		P("create_disabled %s -> %s\n", v[1], ynb(r));
	} else if (!strcmp(c, "init_module")) {
		script_need(sl, 2);
		P("init_module %s -> %s\n", v[1], ynb(obs_init_module(strcmp(v[1], "-") ? MOD(v[1]) : NULL)));
	} else if (!strcmp(c, "free_module")) {
		script_need(sl, 2);
		free_module(strcmp(v[1], "-") ? MOD(v[1]) : NULL);
		if (strcmp(v[1], "-")) mods[slot(mod_names, 32, v[1], false)] = NULL;
	} else if (!strcmp(c, "mod_info")) { /* mod_info M */
		script_need(sl, 2);
		obs_module_t *m = MOD(v[1]);
		P("mod_info %s file=%s name=%s author=%s desc=%s bin=%s data=%s id=%s ver=%s lib=%s core=%s legacy=%s "
		  "state=%s loaded=%s mtype=%d\n",
		  v[1], nul(obs_get_module_file_name(m)), nul(obs_get_module_name(m)), nul(obs_get_module_author(m)),
		  nul(obs_get_module_description(m)), nul(obs_get_module_binary_path(m)),
		  nul(obs_get_module_data_path(m)), nul(obs_get_module_id(m)), nul(obs_get_module_version(m)),
		  obs_get_module_lib(m) ? "set" : "null", ynb(obs_is_core_module(m)), ynb(obs_is_legacy_module(m)),
		  state_name(m->load_state), ynb(m->loaded), (int)m->module_type);
	} else if (!strcmp(c, "mod_null_info")) {
		P("mod_null file=%s name=%s author=%s desc=%s bin=%s data=%s id=%s ver=%s lib=%s core=%s legacy=%s "
		  "ff=%s\n",
		  nul(obs_get_module_file_name(NULL)), nul(obs_get_module_name(NULL)), nul(obs_get_module_author(NULL)),
		  nul(obs_get_module_description(NULL)), nul(obs_get_module_binary_path(NULL)),
		  nul(obs_get_module_data_path(NULL)), nul(obs_get_module_id(NULL)), nul(obs_get_module_version(NULL)),
		  obs_get_module_lib(NULL) ? "set" : "null", ynb(obs_is_core_module(NULL)), ynb(obs_is_legacy_module(NULL)),
		  nul(obs_find_module_file(NULL, "x")));
	} else if (!strcmp(c, "mod_meta")) { /* all metadata fields */
		script_need(sl, 2);
		obs_module_t *m = MOD(v[1]);
		struct obs_module_metadata *md = m->metadata;
		if (!md)
			P("mod_meta %s none\n", v[1]);
		else
			P("mod_meta %s display_name=%s id=%s version=%s os_arch=%s name=%s desc=%s long=%s repo=%s web=%s "
			  "support=%s banner=%s icon=%s\n",
			  v[1], md->display_name, md->id, md->version, md->os_arch, md->name, md->description,
			  md->long_description, md->repository_url, md->website_url, md->support_url,
			  ynb(md->has_banner), ynb(md->has_icon));
	} else if (!strcmp(c, "mod_lists")) { /* registered ids */
		script_need(sl, 2);
		obs_module_t *m = MOD(v[1]);
		print_strs("sources", m->sources.array, m->sources.num);
		print_strs("outputs", m->outputs.array, m->outputs.num);
		print_strs("encoders", m->encoders.array, m->encoders.num);
		print_strs("services", m->services.array, m->services.num);
	} else if (!strcmp(c, "mod_add")) { /* mod_add M KIND ID  (obs_module_add_*) ; M=- for NULL */
		script_need(sl, 4);
		obs_module_t *m = strcmp(v[1], "-") ? MOD(v[1]) : NULL;
		if (!strcmp(v[2], "source")) obs_module_add_source(m, v[3]);
		else if (!strcmp(v[2], "output")) obs_module_add_output(m, v[3]);
		else if (!strcmp(v[2], "encoder")) obs_module_add_encoder(m, v[3]);
		else obs_module_add_service(m, v[3]);
	} else if (!strcmp(c, "lists")) {
		print_list("enabled", obs->first_module);
		print_list("disabled", obs->first_disabled_module);
	} else if (!strcmp(c, "get_module")) { /* get_module NAME */
		script_need(sl, 2);
		obs_module_t *m = obs_get_module(v[1]), *d = obs_get_disabled_module(v[1]);
		P("get_module %s -> enabled=%s disabled=%s", v[1], m ? m->mod_name : "null", d ? d->mod_name : "null");
		P(" allow_disable=%s\n", ynb(obs_get_module_allow_disable(v[1])));
	} else if (!strcmp(c, "enum_modules")) {
		obs_enum_modules(enum_cb, NULL);
	} else if (!strcmp(c, "log_loaded")) {
		obs_log_loaded_modules();
	} else if (!strcmp(c, "post_load")) {
		obs_post_load_modules();
	}
	/* ------------- module paths & discovery */
	else if (!strcmp(c, "add_path")) { /* add_path BIN|- DATA|- */
		script_need(sl, 3);
		obs_add_module_path(opt(v[1]), opt(v[2]));
		P("module_paths=%zu\n", obs->module_paths.num);
	} else if (!strcmp(c, "add_safe")) {
		obs_add_safe_module(opt(v[1]));
		P("safe=%zu\n", obs->safe_modules.num);
	} else if (!strcmp(c, "add_disabled")) {
		obs_add_disabled_module(opt(v[1]));
		P("disabled=%zu\n", obs->disabled_modules.num);
	} else if (!strcmp(c, "find")) {
		obs_find_modules2(find_cb, NULL);
		P("find done\n");
	} else if (!strcmp(c, "find_v1")) {
		obs_find_modules2(find_cb, NULL);
	} else if (!strcmp(c, "load_core")) {
		bool r = obs_load_core_modules();
		P("load_core -> %s core_modules_loaded=%s\n", ynb(r), ynb(obs->core_modules_loaded));
	} else if (!strcmp(c, "load_plugins")) { /* load_plugins BIN DATA TYPE */
		script_need(sl, 4);
		struct obs_runtime_module_info mi = {.path_info = {.binary = v[1], .data = v[2]},
						     .type = (enum obs_runtime_module_type)atoi(v[3])};
		struct obs_module_failure_info fi = {0};
		obs_load_plugins(&mi, &fi);
		P("load_plugins -> count=%zu failed", fi.count);
		if (fi.failed_modules)
			for (char **p = fi.failed_modules; *p; p++)
				P(" %s", *p);
		P("\n");
		obs_module_failure_info_free(&fi);
		obs_module_failure_info_free(&fi);
	} else if (!strcmp(c, "find_core")) { /* find_core BIN DATA TYPE */
		script_need(sl, 4);
		struct obs_runtime_module_info mi = {.path_info = {.binary = v[1], .data = v[2]},
						     .type = (enum obs_runtime_module_type)atoi(v[3])};
		bool r = find_core_module(&mi, find_cb, (void *)1);
		P("find_core -> %s name=%s\n", ynb(r), nul(mi.name));
	}
	/* ------------- module files / locale */
	else if (!strcmp(c, "find_file")) { /* find_file M REL|- */
		script_need(sl, 3);
		char *p = obs_find_module_file(MOD(v[1]), opt(v[2]));
		P("find_file %s %s -> %s\n", v[1], v[2], nul(p));
		bfree(p);
	} else if (!strcmp(c, "config_path")) { /* config_path M FILE */
		script_need(sl, 3);
		char *p = obs_module_get_config_path(MOD(v[1]), v[2]);
		P("config_path %s %s -> %s\n", v[1], v[2], nul(p));
		bfree(p);
	} else if (!strcmp(c, "config_path_null")) { /* obs_module_get_config_path(NULL, "x") */
		char *p = obs_module_get_config_path(NULL, "x");
		P("config_path_null -> %s\n", nul(p));
		bfree(p);
	} else if (!strcmp(c, "load_locale")) { /* load_locale L M DEFAULT LOCALE */
		script_need(sl, 5);
		int s = slot(lk_names, 16, v[1], true);
		lookups[s] = obs_module_load_locale(strcmp(v[2], "-") ? MOD(v[2]) : NULL, opt(v[3]), opt(v[4]));
		P("load_locale %s -> %s\n", v[1], lookups[s] ? "ok" : "null");
	} else if (!strcmp(c, "lk")) { /* lk L KEY */
		script_need(sl, 3);
		const char *out = NULL;
		bool r = text_lookup_getstr(lookups[slot(lk_names, 16, v[1], true)], v[2], &out);
		P("lk %s -> %s %s\n", v[2], ynb(r), r ? out : "");
	} else if (!strcmp(c, "locale_string")) { /* locale_string M KEY */
		script_need(sl, 3);
		const char *out = "unchanged";
		bool r = obs_module_get_locale_string(MOD(v[1]), v[2], &out);
		P("locale_string %s -> %s %s\n", v[2], ynb(r), out);
	} else if (!strcmp(c, "locale_text")) {
		script_need(sl, 3);
		P("locale_text %s -> %s\n", v[2], obs_module_get_locale_text(MOD(v[1]), v[2]));
	} else if (!strcmp(c, "load_metadata")) { /* re-run metadata load for M */
		script_need(sl, 2);
		obs_module_t *m = MOD(v[1]);
		P("load_metadata -> %d\n", obs_module_load_metadata(m));
	} else if (!strcmp(c, "echo")) {
		for (int i = 1; i < sl->argc; i++)
			P("%s%s", i > 1 ? " " : "", v[i]);
		P("\n");
	} else {
		fprintf(stderr, "modservice_runner: unknown command '%s' (line %d)\n", c, sl->lineno);
		exit(2);
	}
	return 0;
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		fprintf(stderr, "usage: %s <script>\n", argv[0]);
		return 2;
	}
	FILE *f = fopen(argv[1], "r");
	if (!f) {
		perror(argv[1]);
		return 2;
	}
	const char *base = getenv("CONFORMANCE_TMPDIR");
	snprintf(tmpdir, sizeof(tmpdir), "%s/ms-XXXXXX", base ? base : ".");
	if (!mkdtemp(tmpdir)) {
		perror("mkdtemp");
		return 2;
	}
	{ /* resolve to an absolute path so that scrubbing and dlopen paths agree */
		char *rp = realpath(tmpdir, NULL);
		if (rp) {
			snprintf(tmpdir, sizeof(tmpdir), "%s", rp);
			free(rp);
		}
	}
	tmplen = strlen(tmpdir);
	base_set_log_handler(ms_log, NULL);
	setvbuf(stdout, NULL, _IOLBF, 0);
	ms_core_init();
	ms_core_set("en-US", NULL);

	struct script_line sl;
	int lineno = 0;
	while (script_next(f, &sl, &lineno))
		run_cmd(&sl);
	return 0;
}
