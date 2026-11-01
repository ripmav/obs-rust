/*
 * Conformance runner for the Lua scripting backend (spec S-060).
 * Links the REAL shared/obs-scripting Lua backend (obs-scripting-lua*.c) on LuaJIT together with the real
 * scripting core, driven by Lua source embedded in the conformance scripts. libobs is faked
 * (gen/luascript_stubs.c). See test/conformance/README.md, section "luascript runner".
 */
#include <inttypes.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>

#include <obs.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <callback/calldata.h>
#include <callback/signal.h>
#include "harness.h"
#include "luascript_fake.h"
#include "obs-scripting-internal.h"
#include "obs-scripting-callback.h"
#include "obs-scripting-lua.h"

#define NS 6
#define NP 4

static obs_script_t *scripts[NS];
static obs_properties_t *props[NP];
static const char *tmpdir;
static int quiet;

static void signal_fn(void *p)
{
	os_event_signal((os_event_t *)p);
}
static void barrier(void)
{
	os_event_t *ev;
	os_event_init(&ev, OS_EVENT_TYPE_AUTO);
	defer_call_post(signal_fn, ev);
	os_event_wait(ev);
	os_event_destroy(ev);
}

static int idx(const char *s, char letter, int max)
{
	if (s[0] == letter && s[1] >= '0' && s[1] < '0' + max)
		return s[1] - '0';
	fprintf(stderr, "bad handle %s\n", s);
	exit(2);
}

/* Log lines become "log <level> <message>"; the environment dependent chunk names inside Lua error messages
 * ([string "<path...>"]) are replaced by [string "<chunk>"], and the temp directory by $TMP. */
static void luascript_log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096], out[4096];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	size_t o = 0;
	for (const char *p = msg; *p && o < sizeof(out) - 32;) {
		if (!strncmp(p, "[string \"", 9)) {
			const char *e = strstr(p, "\"]");
			if (e) {
				o += (size_t)snprintf(out + o, sizeof(out) - o, "[string \"<chunk>\"]");
				p = e + 2;
				continue;
			}
		}
		if (tmpdir && !strncmp(p, tmpdir, strlen(tmpdir))) {
			o += (size_t)snprintf(out + o, sizeof(out) - o, "$TMP");
			p += strlen(tmpdir);
			continue;
		}
		out[o++] = *p == '\n' ? ' ' : *p;
		p++;
	}
	out[o] = 0;
	const char *name = level == LOG_ERROR     ? "error"
			   : level == LOG_WARNING ? "warning"
			   : level == LOG_INFO    ? "info"
			   : level == LOG_DEBUG   ? "debug"
						  : "level?";
	if (!strcmp(name, "level?"))
		printf("log %d %s\n", level, out);
	else
		printf("log %s %s\n", name, out);
}

static void quiet_log(int level, const char *format, va_list args, void *param)
{
	(void)level, (void)format, (void)args, (void)param;
}

static void path_of(char *out, size_t n, const char *name)
{
	snprintf(out, n, "%s/%s", tmpdir, name);
}

/* "s:key=value" "i:key=n" "b:key=0|1" "d:key=x" -> settings object */
static obs_data_t *parse_settings(struct script_line *sl, int from)
{
	obs_data_t *d = obs_data_create();
	for (int i = from; i < sl->argc; i++) {
		char *a = sl->argv[i];
		char *eq = strchr(a, '=');
		if (!eq || a[1] != ':')
			continue;
		*eq = 0;
		const char *key = a + 2, *val = eq + 1;
		switch (a[0]) {
		case 's':
			obs_data_set_string(d, key, val);
			break;
		case 'i':
			obs_data_set_int(d, key, atoll(val));
			break;
		case 'b':
			obs_data_set_bool(d, key, atoi(val) != 0);
			break;
		case 'd':
			obs_data_set_double(d, key, atof(val));
			break;
		}
		*eq = '=';
	}
	return d;
}

static void print_val(obs_data_t *d, const char *spec)
{
	/* spec = "s:key" etc. */
	const char *key = spec + 2;
	switch (spec[0]) {
	case 's':
		printf(" %s=", key);
		script_print_quoted(obs_data_get_string(d, key));
		break;
	case 'i':
		printf(" %s=%lld", key, obs_data_get_int(d, key));
		break;
	case 'b':
		printf(" %s=%d", key, obs_data_get_bool(d, key) ? 1 : 0);
		break;
	case 'd':
		printf(" %s=%.9g", key, obs_data_get_double(d, key));
		break;
	case 'u':
		printf(" %s?=%d", key, obs_data_has_user_value(d, key) ? 1 : 0);
		break;
	}
}

static const char *ptype(enum obs_property_type t)
{
	switch (t) {
	case OBS_PROPERTY_BOOL:
		return "bool";
	case OBS_PROPERTY_INT:
		return "int";
	case OBS_PROPERTY_FLOAT:
		return "float";
	case OBS_PROPERTY_TEXT:
		return "text";
	case OBS_PROPERTY_BUTTON:
		return "button";
	case OBS_PROPERTY_LIST:
		return "list";
	default:
		return "other";
	}
}

static int count_cbs(struct script_callback *cb, int *removed)
{
	int n = 0;
	*removed = 0;
	for (; cb; cb = cb->next) {
		n++;
		if (script_callback_removed(cb))
			(*removed)++;
	}
	return n;
}

/* ---- main ------------------------------------------------------------------------------------------ */

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	tmpdir = getenv("CONFORMANCE_TMPDIR");
	if (!tmpdir) {
		fprintf(stderr, "CONFORMANCE_TMPDIR not set\n");
		return 2;
	}
	char sub[512];
	snprintf(sub, sizeof(sub), "%s/lua-%d", tmpdir, (int)getpid());
	mkdir(sub, 0755);
	tmpdir = sub;
	base_set_log_handler(luascript_log_handler, NULL);

	for (int i = 0; i < 4; i++) {
		lf_handlers[i] = signal_handler_create();
	}
	lf_scene.nitems = 3;
	for (int i = 0; i < 4; i++)
		lf_items[i].id = 100 + i;
	bool subsystem = false;

	struct script_line sl;
	int lineno = 0;
	char raw[SCRIPT_MAX_LINE];
	while (fgets(raw, sizeof(raw), f)) {
		lineno++;
		sl.lineno = lineno;

		/* verbatim Lua block: "lua_begin NAME" ... "lua_end" */
		if (!strncmp(raw, "lua_begin ", 10)) {
			char name[128];
			sscanf(raw + 10, "%127s", name);
			char path[600];
			path_of(path, sizeof(path), name);
			FILE *o = fopen(path, "wb");
			while (fgets(raw, sizeof(raw), f)) {
				lineno++;
				if (!strncmp(raw, "lua_end", 7))
					break;
				fputs(raw, o);
			}
			fclose(o);
			continue;
		}
		if (!script_tokenize(&sl, raw))
			continue;
		bool in_child = false;
		if (!strcmp(sl.argv[0], "try") && sl.argc > 1) {
			/* try <command...>: run the command in a forked child; report a crash by signal number.
			 * Only for commands that need no deferred calls (the worker thread does not exist in the child). */
			fflush(stdout);
			pid_t pid = fork();
			if (pid != 0) {
				int st = 0;
				waitpid(pid, &st, 0);
				if (WIFSIGNALED(st))
					printf("try %s -> crash signal %d\n", sl.argv[1], WTERMSIG(st));
				else
					printf("try %s -> exit %d\n", sl.argv[1], WEXITSTATUS(st));
				continue;
			}
			in_child = true;
			sl.argc--;
			for (int i = 0; i < sl.argc; i++)
				sl.argv[i] = sl.argv[i + 1];
		}
		const char *cmd = sl.argv[0];

		if (!strcmp(cmd, "echo")) {
			printf("echo");
			for (int i = 1; i < sl.argc; i++)
				printf(" %s", sl.argv[i]);
			printf("\n");
		} else if (!strcmp(cmd, "scripting")) {
			script_need(&sl, 2);
			if (!strcmp(sl.argv[1], "load")) {
				bool ok = obs_scripting_load();
				subsystem = ok;
				printf("scripting load -> %d ticks=%d\n", ok, lf_nticks);
			} else {
				obs_scripting_unload();
				subsystem = false;
				printf("scripting unload ticks=%d\n", lf_nticks);
			}
		} else if (!strcmp(cmd, "barrier")) {
			barrier();
		} else if (!strcmp(cmd, "create")) {
			script_need(&sl, 3);
			int n = idx(sl.argv[1], 'S', NS);
			char path[600];
			if (!strncmp(sl.argv[2], "raw:", 4))
				snprintf(path, sizeof(path), "%s", sl.argv[2] + 4);
			else
				path_of(path, sizeof(path), sl.argv[2]);
			obs_data_t *st = sl.argc > 3 ? parse_settings(&sl, 3) : NULL;
			scripts[n] = obs_script_create(strcmp(sl.argv[2], "-") ? path : NULL, st);
			obs_data_release(st);
			obs_script_t *s = scripts[n];
			if (!s) {
				printf("create %s -> NULL\n", sl.argv[1]);
			} else {
				const char *d = obs_script_get_description(s);
				printf("create %s -> loaded=%d lang=%d file=%s desc=", sl.argv[1], obs_script_loaded(s),
				       obs_script_get_lang(s), obs_script_get_file(s));
				if (d)
					script_print_quoted(d);
				else
					printf("NULL");
				printf("\n");
			}
		} else if (!strcmp(cmd, "destroy")) {
			int n = idx(sl.argv[1], 'S', NS);
			obs_script_destroy(scripts[n]);
			scripts[n] = NULL;
			barrier(); /* deferred work posted by the unload (hotkey unregister) must finish first */
			printf("destroy %s\n", sl.argv[1]);
		} else if (!strcmp(cmd, "reload")) {
			int n = idx(sl.argv[1], 'S', NS);
			printf("reload %s -> %d\n", sl.argv[1], obs_script_reload(scripts[n]));
		} else if (!strcmp(cmd, "update")) {
			int n = idx(sl.argv[1], 'S', NS);
			obs_data_t *st = sl.argc > 2 ? parse_settings(&sl, 2) : NULL;
			obs_script_update(scripts[n], st);
			obs_data_release(st);
			barrier();
			printf("update %s done\n", sl.argv[1]);
		} else if (!strcmp(cmd, "save")) {
			int n = idx(sl.argv[1], 'S', NS);
			obs_data_t *d = obs_script_save(scripts[n]);
			printf("save %s", sl.argv[1]);
			for (int i = 2; i < sl.argc; i++)
				print_val(d, sl.argv[i]);
			printf("\n");
			obs_data_release(d);
		} else if (!strcmp(cmd, "settings")) {
			int n = idx(sl.argv[1], 'S', NS);
			obs_data_t *d = obs_script_get_settings(scripts[n]);
			printf("settings %s", sl.argv[1]);
			for (int i = 2; i < sl.argc; i++)
				print_val(d, sl.argv[i]);
			printf("\n");
			obs_data_release(d);
		} else if (!strcmp(cmd, "callbacks")) {
			int n = idx(sl.argv[1], 'S', NS);
			int rem;
			int total = count_cbs(((struct obs_lua_script *)scripts[n])->first_callback, &rem);
			printf("callbacks %s total=%d removed=%d\n", sl.argv[1], total, rem);
		} else if (!strcmp(cmd, "props")) {
			int n = idx(sl.argv[1], 'S', NS);
			int p = idx(sl.argv[2], 'P', NP);
			obs_properties_destroy(props[p]);
			props[p] = obs_script_get_properties(scripts[n]);
			printf("props %s ->", sl.argv[2]);
			if (!props[p])
				printf(" NULL");
			else
				for (obs_property_t *pr = obs_properties_first(props[p]); pr; obs_property_next(&pr)) {
					printf(" %s:%s:", obs_property_name(pr), ptype(obs_property_get_type(pr)));
					script_print_quoted(obs_property_description(pr));
					if (!obs_property_enabled(pr))
						printf("(disabled)");
				}
			printf("\n");
		} else if (!strcmp(cmd, "click")) {
			int p = idx(sl.argv[1], 'P', NP);
			obs_property_t *pr = obs_properties_get(props[p], sl.argv[2]);
			bool r = obs_property_button_clicked(pr, NULL);
			printf("click %s %s -> %d\n", sl.argv[1], sl.argv[2], r);
		} else if (!strcmp(cmd, "modify")) {
			int p = idx(sl.argv[1], 'P', NP);
			int n = idx(sl.argv[3], 'S', NS);
			obs_property_t *pr = obs_properties_get(props[p], sl.argv[2]);
			obs_data_t *d = obs_script_get_settings(scripts[n]);
			bool r = obs_property_modified(pr, d);
			obs_data_release(d);
			printf("modify %s %s -> %d\n", sl.argv[1], sl.argv[2], r);
		} else if (!strcmp(cmd, "time")) {
			lf_video_time = strtoull(sl.argv[1], NULL, 10);
		} else if (!strcmp(cmd, "tick")) {
			/* tick SECONDS [NEW_TIME_NS]: advance frame time, call the registered tick callbacks */
			float sec = (float)atof(sl.argv[1]);
			if (sl.argc > 2)
				lf_video_time = strtoull(sl.argv[2], NULL, 10);
			struct lf_tick snap[LF_MAX_CB];
			int cnt = lf_nticks;
			memcpy(snap, lf_ticks, sizeof(snap));
			for (int i = 0; i < cnt; i++)
				snap[i].fn(snap[i].p, sec);
		} else if (!strcmp(cmd, "render")) {
			struct lf_render snap[LF_MAX_CB];
			int cnt = lf_nrenders;
			memcpy(snap, lf_renders, sizeof(snap));
			for (int i = 0; i < cnt; i++)
				snap[i].fn(snap[i].p, (uint32_t)atoi(sl.argv[1]), (uint32_t)atoi(sl.argv[2]));
		} else if (!strcmp(cmd, "counts")) {
			printf("counts ticks=%d renders=%d hotkeys=%d fe_event=%d fe_save=%d\n", lf_nticks, lf_nrenders,
			       lf_nhotkeys, lf_fe_nevent, lf_fe_nsave);
		} else if (!strcmp(cmd, "handler")) {
			int h = idx(sl.argv[1], 'H', 4);
			signal_handler_add(lf_handlers[h], sl.argv[2]);
		} else if (!strcmp(cmd, "emit")) {
			int h = idx(sl.argv[1], 'H', 4);
			calldata_t cd;
			calldata_init(&cd);
			for (int i = 3; i < sl.argc; i++) {
				char *a = sl.argv[i];
				char *eq = strchr(a, '=');
				if (!eq)
					continue;
				*eq = 0;
				if (a[0] == 's')
					calldata_set_string(&cd, a + 2, eq + 1);
				else
					calldata_set_int(&cd, a + 2, atoll(eq + 1));
			}
			signal_handler_signal(lf_handlers[h], sl.argv[2], &cd);
			calldata_free(&cd);
		} else if (!strcmp(cmd, "hotkey")) {
			int id = atoi(sl.argv[1]);
			bool pressed = atoi(sl.argv[2]) != 0;
			if (id >= 1 && id <= lf_nhotkeys && lf_hotkeys[id - 1].live)
				lf_hotkeys[id - 1].func(lf_hotkeys[id - 1].data, (obs_hotkey_id)id, NULL, pressed);
			else
				printf("hotkey %d not registered\n", id);
		} else if (!strcmp(cmd, "fe_event")) {
			int ev = atoi(sl.argv[1]);
			for (int i = 0; i < lf_fe_nevent; i++)
				if (lf_fe_event_cb[i])
					lf_fe_event_cb[i]((enum obs_frontend_event)ev, lf_fe_event_p[i]);
		} else if (!strcmp(cmd, "fe_save")) {
			obs_data_t *d = obs_data_create();
			for (int i = 0; i < lf_fe_nsave; i++)
				if (lf_fe_save_cb[i])
					lf_fe_save_cb[i](d, atoi(sl.argv[1]) != 0, lf_fe_save_p[i]);
			printf("fe_save result");
			for (int i = 2; i < sl.argc; i++)
				print_val(d, sl.argv[i]);
			printf("\n");
			obs_data_release(d);
		} else if (!strcmp(cmd, "fe_state")) {
			printf("fe_state duration=%d collection=%s profile=%s\n", lf_fe_duration, lf_fe_collection,
			       lf_fe_profile);
		} else if (!strcmp(cmd, "swig_hide")) {
			printf("swig_hide %s -> %d\n", sl.argv[1], lf_hide_type(sl.argv[1], atoi(sl.argv[2])));
		} else if (!strcmp(cmd, "fake_source")) {
			/* fake_source NAME [nfilters]: appends a plain source (index = order of creation) */
			struct obs_source *s = &lf_sources[lf_nsources++];
			memset(s, 0, sizeof(*s));
			snprintf(s->name, sizeof(s->name), "%s", sl.argv[1]);
			s->refs = 1;
			s->settings = obs_data_create();
			int nf = sl.argc > 2 ? atoi(sl.argv[2]) : 0;
			for (int i = 0; i < nf; i++) {
				struct obs_source *fl = &lf_sources[lf_nsources++];
				memset(fl, 0, sizeof(*fl));
				snprintf(fl->name, sizeof(fl->name), "%s-f%d", sl.argv[1], i);
				fl->refs = 1;
				fl->settings = obs_data_create();
				s->filters[s->nfilters++] = fl;
			}
		} else if (!strcmp(cmd, "refs")) {
			printf("refs");
			for (int i = 0; i < lf_nsources; i++)
				printf(" %s=%d", lf_sources[i].name, lf_sources[i].refs);
			for (int i = 0; i < lf_scene.nitems; i++)
				printf(" item%d=%d", lf_items[i].id, lf_items[i].refs);
			printf("\n");
		} else if (!strcmp(cmd, "scene_items")) {
			lf_scene.nitems = atoi(sl.argv[1]);
		} else if (!strcmp(cmd, "src_types")) {
			printf("src_types");
			for (int i = 0; i < LF_MAX_TYPES; i++) {
				struct fake_type *t = &lf_types[i];
				if (!t->used)
					continue;
				printf(" [%s type=%d flags=%u enabled=%d name=", t->info.id, (int)t->info.type,
				       t->info.output_flags, t->enabled);
				script_print_quoted(t->info.get_name(t->info.type_data));
				printf("]");
			}
			printf("\n");
		} else if (!strcmp(cmd, "src_defaults")) {
			for (int i = 0; i < LF_MAX_TYPES; i++)
				if (lf_types[i].used && !strcmp(lf_types[i].info.id, sl.argv[1])) {
					obs_data_t *d = obs_data_create();
					lf_types[i].info.get_defaults2(lf_types[i].info.type_data, d);
					printf("src_defaults %s", sl.argv[1]);
					for (int k = 2; k < sl.argc; k++)
						print_val(d, sl.argv[k]);
					printf("\n");
					obs_data_release(d);
				}
		} else if (!strcmp(cmd, "src_create")) {
			/* src_create TYPEID NAME [kv...]: instantiate a Lua-registered type; slot = order of creation */
			struct fake_type *t = NULL;
			for (int i = 0; i < LF_MAX_TYPES; i++)
				if (lf_types[i].used && !strcmp(lf_types[i].info.id, sl.argv[1]))
					t = &lf_types[i];
			struct obs_source *s = &lf_sources[lf_nsources];
			memset(s, 0, sizeof(*s));
			snprintf(s->name, sizeof(s->name), "%s", sl.argv[2]);
			s->type = t;
			s->refs = 1;
			s->settings = parse_settings(&sl, 3);
			s->data = t ? t->info.create(s->settings, s) : NULL;
			printf("src_create %s -> data=%s\n", sl.argv[2], s->data ? "yes" : "NULL");
			if (s->data)
				lf_nsources++;
		} else if (!strcmp(cmd, "src_call")) {
			/* src_call NAME OP [arg] */
			struct obs_source *s = NULL;
			for (int i = 0; i < lf_nsources; i++)
				if (!strcmp(lf_sources[i].name, sl.argv[1]))
					s = &lf_sources[i];
			if (!s || !s->type) {
				printf("src_call %s: no such lua source\n", sl.argv[1]);
				goto next;
			}
			struct obs_source_info *i = &s->type->info;
			const char *op = sl.argv[2];
			printf("src_call %s %s\n", sl.argv[1], op);
			if (!strcmp(op, "width"))
				printf("  -> %u\n", i->get_width(s->data));
			else if (!strcmp(op, "height"))
				printf("  -> %u\n", i->get_height(s->data));
			else if (!strcmp(op, "update")) {
				obs_data_t *d = parse_settings(&sl, 3);
				i->update(s->data, d);
				obs_data_release(d);
			} else if (!strcmp(op, "activate"))
				i->activate(s->data);
			else if (!strcmp(op, "deactivate"))
				i->deactivate(s->data);
			else if (!strcmp(op, "show"))
				i->show(s->data);
			else if (!strcmp(op, "hide"))
				i->hide(s->data);
			else if (!strcmp(op, "tick"))
				i->video_tick(s->data, 0.5f);
			else if (!strcmp(op, "render"))
				i->video_render(s->data, (gs_effect_t *)&s->refs);
			else if (!strcmp(op, "save")) {
				obs_data_t *d = obs_data_create();
				i->save(s->data, d);
				for (int k = 3; k < sl.argc; k++)
					print_val(d, sl.argv[k]);
				printf("\n");
				obs_data_release(d);
			} else if (!strcmp(op, "load")) {
				obs_data_t *d = parse_settings(&sl, 3);
				i->load(s->data, d);
				obs_data_release(d);
			} else if (!strcmp(op, "props")) {
				obs_properties_t *p = i->get_properties(s->data);
				printf("  ->");
				if (!p)
					printf(" NULL");
				else {
					for (obs_property_t *pr = obs_properties_first(p); pr; obs_property_next(&pr))
						printf(" %s", obs_property_name(pr));
					obs_properties_destroy(p);
				}
			} else if (!strcmp(op, "destroy")) {
				i->destroy(s->data);
				s->data = NULL;
			}
			printf("\n");
		} else if (!strcmp(cmd, "src_free")) {
			/* what libobs does when a source type is finally dropped */
			for (int i = 0; i < LF_MAX_TYPES; i++)
				if (lf_types[i].used && !strcmp(lf_types[i].info.id, sl.argv[1])) {
					lf_types[i].info.free_type_data(lf_types[i].info.type_data);
					lf_types[i].used = false;
					printf("src_free %s\n", sl.argv[1]);
				}
		} else {
			fprintf(stderr, "line %d: unknown command %s\n", lineno, cmd);
			return 2;
		}
	next:
		if (in_child) {
			fflush(stdout);
			_exit(0);
		}
	}

	/* silent teardown */
	base_set_log_handler(quiet_log, NULL);
	quiet = 1;
	for (int i = 0; i < NS; i++)
		obs_script_destroy(scripts[i]);
	for (int i = 0; i < NP; i++)
		obs_properties_destroy(props[i]);
	if (subsystem)
		obs_scripting_unload();
	(void)quiet;
	return 0;
}
