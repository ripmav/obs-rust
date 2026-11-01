/*
 * Conformance runner for the Python scripting backend (spec S-061).
 * Links the REAL shared/obs-scripting Python backend (obs-scripting-python.c, -python-frontend.c) on an embedded
 * CPython together with the real scripting core, driven by Python source embedded in the conformance scripts.
 * libobs is faked (gen/pyscript_stubs.c). See test/conformance/README.md, section "pyscript runner".
 */
#include <Python.h>
#include <inttypes.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>

#include <obs.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <callback/calldata.h>
#include <callback/signal.h>
#include "harness.h"
#include "pyscript_fake.h"
#include "obs-scripting-internal.h"
#include "obs-scripting-callback.h"
#include "obs-scripting-python.h"

#define NS 6
#define NP 4

static obs_script_t *scripts[NS];
static obs_properties_t *props[NP];
static const char *tmpdir;
static int quiet;

extern PyObject *PyInit_obspython(void);

static void signal_fn(void *p)
{
	os_event_signal((os_event_t *)p);
}
static int in_fork_child, child_left;
static bool subsystem;
static void barrier(void)
{
	if (in_fork_child || !subsystem) /* no worker thread */
		return;
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

/* Log lines become "log <level> <message>". Environment dependent text is normalised:
 *  - the private directory becomes $TMP
 *  - Python tracebacks (printed through sys.stderr) are reduced to the header, the frames of the script's own files
 *    (path replaced, "line N" and function kept) and the final exception line; interpreter frames, source lines and
 *    caret markers are dropped; syntax error messages (interpreter text) are replaced by "<message>"
 *  - the C line number of "Python failure in FUNC:LINE:" becomes <line> */
static void pyscript_log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096], out[4096];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	size_t o = 0;
	for (const char *p = msg; *p && o < sizeof(out) - 32;) {
		if (tmpdir && !strncmp(p, tmpdir, strlen(tmpdir))) {
			o += (size_t)snprintf(out + o, sizeof(out) - o, "$TMP");
			p += strlen(tmpdir);
			continue;
		}
		out[o++] = *p == '\n' ? ' ' : *p;
		p++;
	}
	out[o] = 0;

	/* split "[Python: file] content" */
	char *content = out;
	if (!strncmp(out, "[Python: ", 9) || !strncmp(out, "[Unknown Script] ", 17)) {
		char *e = strstr(out, "] ");
		if (e)
			content = e + 2;
	}
	if (level == LOG_ERROR && content != out) {
		if (!strncmp(content, "  File \"", 8)) {
			if (!strstr(content, "$TMP"))
				return; /* interpreter / stdlib frame */
		} else if (content[0] == ' ' || content[0] == '\t' || !*content) {
			return; /* source line or caret marker */
		} else if (!strncmp(content, "SyntaxError", 11) || !strncmp(content, "IndentationError", 16) ||
			   !strncmp(content, "TabError", 8)) {
			char *colon = strchr(content, ':');
			if (colon)
				strcpy(colon + 1, " <message>");
		}
	}
	/* "[Python] FUNC:LINE: text" (SWIG conversion warnings): mask the C line number */
	if (!strncmp(out, "[Python] ", 9)) {
		char *c1 = strchr(out + 9, ':');
		if (c1 && c1[1] >= '0' && c1[1] <= '9') {
			char *end = c1 + 1;
			while (*end >= '0' && *end <= '9')
				end++;
			if (*end == ':') {
				memmove(c1 + 7, end, strlen(end) + 1);
				memcpy(c1 + 1, "<line>", 6);
			}
		}
	}
	char *pf = strstr(out, "Python failure in ");
	if (pf) {
		char *colon = strchr(pf + 18, ':');
		if (colon && colon[1] >= '0' && colon[1] <= '9') {
			char *end = colon + 1;
			while (*end >= '0' && *end <= '9')
				end++;
			memmove(colon + 7, end, strlen(end) + 1);
			memcpy(colon + 1, "<line>", 6);
		}
	}
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
	snprintf(sub, sizeof(sub), "%s/py-%d", tmpdir, (int)getpid());
	mkdir(sub, 0755);
	tmpdir = sub;
	base_set_log_handler(pyscript_log_handler, NULL);
	/* make the embedded interpreter independent of the environment */
	setenv("PYTHONDONTWRITEBYTECODE", "1", 1);
	setenv("PYTHONNOUSERSITE", "1", 1);
	setenv("PYTHONSAFEPATH", "1", 1);
	unsetenv("PYTHONPATH");
	unsetenv("PYTHONSTARTUP");
	unsetenv("PYTHONINSPECT");
	PyImport_AppendInittab("obspython", PyInit_obspython);

	for (int i = 0; i < 4; i++) {
		lf_handlers[i] = signal_handler_create();
	}
	lf_scene.nitems = 3;
	for (int i = 0; i < 4; i++)
		lf_items[i].id = 100 + i;

	struct script_line sl;
	int lineno = 0;
	char raw[SCRIPT_MAX_LINE];
	while (fgets(raw, sizeof(raw), f)) {
		lineno++;
		sl.lineno = lineno;

		/* verbatim Python block: "py_begin NAME" ... "py_end" */
		if (!strncmp(raw, "py_begin ", 9)) {
			char name[128];
			sscanf(raw + 9, "%127s", name);
			char path[600];
			path_of(path, sizeof(path), name);
			FILE *o = fopen(path, "wb");
			while (fgets(raw, sizeof(raw), f)) {
				lineno++;
				if (!strncmp(raw, "py_end", 6))
					break;
				fputs(raw, o);
			}
			fclose(o);
			continue;
		}
		if (!script_tokenize(&sl, raw))
			continue;
		bool is_try = !strcmp(sl.argv[0], "try") && sl.argc > 1;
		bool is_seq = !strcmp(sl.argv[0], "tryseq") && sl.argc > 1;
		if ((is_try || is_seq) && !child_left) {
			/* try <command...>   : run the command in a forked child;
			 * tryseq N           : run the next N commands in a forked child, in sequence.
			 * The parent only reports whether the child crashed (no signal number: it differs between Python
			 * versions) and skips the N commands. The worker thread does not exist in the child, so barrier()
			 * is a no-op there. */
			int n = is_seq ? atoi(sl.argv[1]) : 1;
			fflush(stdout);
			pid_t pid = fork();
			if (pid != 0) {
				int st = 0;
				waitpid(pid, &st, 0);
				if (is_seq) {
					/* skip the commands the child ran (blocks and comments do not count) */
					for (int k = 0; k < n && fgets(raw, sizeof(raw), f);) {
						lineno++;
						if (!strncmp(raw, "py_begin ", 9)) {
							while (fgets(raw, sizeof(raw), f) && strncmp(raw, "py_end", 6))
								lineno++;
							continue;
						}
						if (script_tokenize(&sl, raw))
							k++;
					}
				}
				if (WIFSIGNALED(st))
					printf("%s %s -> crash\n", is_seq ? "tryseq" : "try", sl.argv[1]);
				else
					printf("%s %s -> exit %d\n", is_seq ? "tryseq" : "try", sl.argv[1],
					       WEXITSTATUS(st));
				continue;
			}
			in_fork_child = 1;
			freopen("/dev/null", "w", stderr);
			struct rlimit rl = {0, 0};
			setrlimit(RLIMIT_CORE, &rl);
			child_left = n;
			if (is_try) {
				sl.argc--;
				for (int i = 0; i < sl.argc; i++)
					sl.argv[i] = sl.argv[i + 1];
			} else
				continue;
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
				printf("scripting load -> %d python=%d ticks=%d\n", ok, obs_scripting_python_loaded(), lf_nticks);
			} else {
				/* like the frontend: scripts are destroyed before the subsystem is unloaded (the backend
				 * must not be called after the interpreter is finalized, see D-3 in the spec) */
				for (int i = 0; i < NS; i++)
					if (scripts[i]) {
						obs_script_destroy(scripts[i]);
						scripts[i] = NULL;
						barrier();
						printf("auto-destroy S%d\n", i);
					}
				obs_scripting_unload();
				subsystem = false;
				printf("scripting unload ticks=%d\n", lf_nticks);
			}
		} else if (!strcmp(cmd, "pyexec")) {
			/* pyexec "CODE": run code in __main__ with the GIL held and NO current script (like a Python thread
			 * started by a script would). Harness driver, not part of the backend. */
			script_need(&sl, 2);
			PyGILState_STATE gs = PyGILState_Ensure();
			int rc = PyRun_SimpleString(sl.argv[1]);
			PyGILState_Release(gs);
			printf("pyexec -> %d\n", rc);
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
			barrier(); /* deferred registrations / unregistrations posted by the load must finish first */
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
			bool rl = obs_script_reload(scripts[n]);
			barrier();
			printf("reload %s -> %d\n", sl.argv[1], rl);
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
			int total = count_cbs(((struct obs_python_script *)scripts[n])->first_callback, &rem);
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
			barrier(); /* the backend runs the Python callback on its deferred-call thread */
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
		} else {
			fprintf(stderr, "line %d: unknown command %s\n", lineno, cmd);
			return 2;
		}
		if (child_left && --child_left == 0) {
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
