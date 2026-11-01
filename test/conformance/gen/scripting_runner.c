/*
 * Conformance runner for the language-independent scripting core (spec S-039).
 * Links the real obs-scripting.c / obs-scripting-logging.c / cstrcache.cpp and the header-inline
 * obs-scripting-callback.h against a fake "lua" backend (scripting_stubs.c). See test/conformance/README.md.
 */
#include <inttypes.h>
#include <pthread.h>

#include <obs.h>
#include <util/threading.h>
#include <util/bmem.h>
#include <callback/calldata.h>
#include "harness.h"
#include "obs-scripting-internal.h"
#include "obs-scripting-callback.h"
#include "cstrcache.h"
#include "scripting_fake.h"

EXPORT obs_module_t *obs_current_module(void);

#define NS 8
#define NC 16
#define NL 4
#define NP 2048

static obs_script_t *scripts[NS];
static struct script_callback *cbs[NC];
static struct script_callback *lists[NL];
static const char *cstr_ptr[NP];
static char *cstr_txt[NP];
static int ncstr;

/* ---- deferred-call log (written by the defer thread, read after a barrier) ---- */
static pthread_mutex_t jl_mutex = PTHREAD_MUTEX_INITIALIZER;
static char joblog[64][16];
static int njobs;
static int deferred_count(void)
{
	pthread_mutex_lock(&jl_mutex);
	int n = njobs;
	pthread_mutex_unlock(&jl_mutex);
	return n;
}
static void job_fn(void *p)
{
	pthread_mutex_lock(&jl_mutex);
	if (njobs < 64)
		snprintf(joblog[njobs++], 16, "%s", (const char *)p);
	pthread_mutex_unlock(&jl_mutex);
	free(p);
}
static void signal_fn(void *p)
{
	os_event_signal((os_event_t *)p);
}

static bool is_null(const char *s)
{
	return !strcmp(s, "-");
}

static int script_idx(const char *s)
{
	if (s[0] == 'S' && s[1] >= '0' && s[1] < '0' + NS)
		return s[1] - '0';
	fprintf(stderr, "bad script handle %s\n", s);
	exit(2);
}
static obs_script_t *hs(const char *s)
{
	return is_null(s) ? NULL : scripts[script_idx(s)];
}
static int cb_idx(const char *s)
{
	if (s[0] == 'C')
		return atoi(s + 1);
	fprintf(stderr, "bad cb handle %s\n", s);
	exit(2);
}
static int list_idx(const char *s)
{
	return s[0] == 'L' ? atoi(s + 1) : 0;
}
static int cb_id_of(struct script_callback *cb)
{
	for (int i = 0; i < NC; i++)
		if (cbs[i] == cb)
			return i;
	return -1;
}
static const char *lang_name(enum obs_script_lang l)
{
	return l == OBS_SCRIPT_LANG_LUA ? "lua" : l == OBS_SCRIPT_LANG_PYTHON ? "python" : "unknown";
}
static void print_list(const char *name, struct script_callback *head)
{
	printf("list %s:", name);
	if (!head)
		printf(" -");
	for (struct script_callback *c = head; c; c = c->next)
		printf(" C%d", cb_id_of(c));
	printf("\n");
}
static void on_remove_hook(void *p)
{
	struct script_callback *cb = p;
	printf("on_remove C%d removed=%d in_detached_head=%d\n", cb_id_of(cb), script_callback_removed(cb) ? 1 : 0,
	       detached_callbacks == cb ? 1 : 0);
}

static void log_handler(void *p, obs_script_t *script, int lvl, const char *msg)
{
	(void)p;
	int id = -1;
	for (int i = 0; i < NS; i++)
		if (script && scripts[i] == script)
			id = i;
	printf("scriptlog level=%d script=%s msg=", lvl, script ? (id >= 0 ? "S" : "?") : "-");
	if (id >= 0)
		printf("%d ", id);
	if (strlen(msg) > 100)
		printf("len=%zu", strlen(msg));
	else
		script_print_quoted(msg);
	printf("\n");
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	fake_deferred_counter = deferred_count;

	while (script_next(f, &sl, &lineno)) {
		const char *cmd = sl.argv[0];

		/* ---- subsystem lifecycle ---- */
		if (!strcmp(cmd, "load")) {
			printf("load -> %d\n", obs_scripting_load() ? 1 : 0);
		} else if (!strcmp(cmd, "unload")) {
			/* slots whose callbacks sit in the detached list are freed by unload */
			for (struct script_callback *c = detached_callbacks; c; c = c->next) {
				int id = cb_id_of(c);
				if (id >= 0)
					cbs[id] = NULL;
			}
			bool dangling_before = false;
			obs_scripting_unload();
			dangling_before = detached_callbacks != NULL;
			printf("info_detached_head_after_unload %s\n", dangling_before ? "dangling" : "null");
			detached_callbacks = NULL; /* keep later scripts inside the preconditions */
			printf("unload done\n");
		} else if (!strcmp(cmd, "formats")) {
			const char **fm = obs_scripting_supported_formats();
			printf("formats:");
			for (; *fm; fm++)
				printf(" %s", *fm);
			printf("\n");
		} else if (!strcmp(cmd, "python")) {
			char ver[8] = "xxxxxxx";
			obs_scripting_python_version(ver, sizeof(ver));
			printf("python load_python=%d loaded=%d linked=%d version=\"%s\"\n",
			       obs_scripting_load_python("/nonexistent") ? 1 : 0, obs_scripting_python_loaded() ? 1 : 0,
			       obs_scripting_python_runtime_linked() ? 1 : 0, ver);
		} else if (!strcmp(cmd, "set_module")) {
			script_need(&sl, 2);
			obs_scripting_set_module(is_null(sl.argv[1]) ? NULL : (obs_module_t *)(uintptr_t)strtoull(sl.argv[1], NULL, 0));
		} else if (!strcmp(cmd, "cur_module")) {
			obs_module_t *m = obs_current_module();
			printf("cur_module %s\n", m ? "set" : "null");
			if (m)
				printf("cur_module value=0x%" PRIxPTR "\n", (uintptr_t)m);

			/* ---- fake backend definitions ---- */
		} else if (!strcmp(cmd, "fake_define")) { /* fake_define PATH load=ok|fail [desc=_] [props=none|N] */
			script_need(&sl, 2);
			struct fake_def *d = &fake_defs[fake_ndefs++];
			snprintf(d->path, sizeof(d->path), "%s", sl.argv[1]);
			d->load_ok = true;
			d->nprops = -1;
			for (int i = 2; i < sl.argc; i++) {
				if (!strncmp(sl.argv[i], "load=", 5))
					d->load_ok = !strcmp(sl.argv[i] + 5, "ok");
				else if (!strncmp(sl.argv[i], "desc=", 5))
					snprintf(d->desc, sizeof(d->desc), "%s", sl.argv[i] + 5);
				else if (!strncmp(sl.argv[i], "props=", 6))
					d->nprops = !strcmp(sl.argv[i] + 6, "none") ? -1 : atoi(sl.argv[i] + 6);
			}

			/* ---- obs_script_* API ---- */
		} else if (!strcmp(cmd, "create")) { /* create S PATH|@null [K V] */
			script_need(&sl, 3);
			int i = script_idx(sl.argv[1]);
			obs_data_t *settings = NULL;
			if (sl.argc >= 5) {
				settings = obs_data_create();
				obs_data_set_string(settings, sl.argv[3], sl.argv[4]);
			}
			scripts[i] = obs_script_create(strcmp(sl.argv[2], "@null") ? sl.argv[2] : NULL, settings);
			obs_data_release(settings);
			printf("create %s -> %s\n", sl.argv[1], scripts[i] ? "ok" : "null");
		} else if (!strcmp(cmd, "info")) {
			script_need(&sl, 2);
			obs_script_t *s = hs(sl.argv[1]);
			const char *d = obs_script_get_description(s);
			const char *path = obs_script_get_path(s);
			const char *file = obs_script_get_file(s);
			enum obs_script_lang lang = obs_script_get_lang(s);
			bool loaded = obs_script_loaded(s);
			printf("info %s desc=", sl.argv[1]);
			if (d)
				script_print_quoted(d);
			else
				printf("@null");
			printf(" path=");
			script_print_quoted(path);
			printf(" file=");
			script_print_quoted(file);
			printf(" lang=%s loaded=%d\n", lang_name(lang), loaded ? 1 : 0);
		} else if (!strcmp(cmd, "settings")) { /* settings S KEY */
			script_need(&sl, 3);
			obs_script_t *s = hs(sl.argv[1]);
			obs_data_t *d = obs_script_get_settings(s);
			if (d) {
				printf("settings %s same=%d %s=", sl.argv[1], d == s->settings ? 1 : 0, sl.argv[2]);
				script_print_quoted(obs_data_get_string(d, sl.argv[2]));
				printf("\n");
				obs_data_release(d);
			} else {
				printf("settings %s null\n", sl.argv[1]);
			}
		} else if (!strcmp(cmd, "props")) {
			script_need(&sl, 2);
			obs_properties_t *p = obs_script_get_properties(hs(sl.argv[1]));
			printf("props %s", sl.argv[1]);
			if (!p) {
				printf(" null\n");
				continue;
			}
			int n = 0;
			for (obs_property_t *it = obs_properties_first(p); it; obs_property_next(&it))
				n++;
			printf(" count=%d", n);
			for (obs_property_t *it = obs_properties_first(p); it; obs_property_next(&it))
				printf(" %s", obs_property_name(it));
			printf("\n");
			obs_properties_destroy(p);
		} else if (!strcmp(cmd, "save")) {
			script_need(&sl, 2);
			obs_script_t *s = hs(sl.argv[1]);
			obs_data_t *d = obs_script_save(s);
			if (d) {
				printf("save %s same=%d saved=", sl.argv[1], d == s->settings ? 1 : 0);
				script_print_quoted(obs_data_get_string(d, "saved"));
				printf("\n");
				obs_data_release(d);
			} else {
				printf("save %s null\n", sl.argv[1]);
			}
		} else if (!strcmp(cmd, "update")) { /* update S K V | update S - */
			script_need(&sl, 3);
			obs_data_t *d = NULL;
			if (sl.argc >= 4) {
				d = obs_data_create();
				obs_data_set_string(d, sl.argv[2], sl.argv[3]);
			}
			obs_script_update(hs(sl.argv[1]), d);
			obs_data_release(d);
		} else if (!strcmp(cmd, "reload")) {
			script_need(&sl, 2);
			printf("reload %s -> %d\n", sl.argv[1], obs_script_reload(hs(sl.argv[1])) ? 1 : 0);
		} else if (!strcmp(cmd, "destroy")) {
			script_need(&sl, 2);
			int i = is_null(sl.argv[1]) ? -1 : script_idx(sl.argv[1]);
			obs_script_destroy(i >= 0 ? scripts[i] : NULL);
			printf("destroy %s done\n", sl.argv[1]);
		} else if (!strcmp(cmd, "forcetype")) { /* forcetype S lua|python|unknown : bypass dispatch-by-extension */
			script_need(&sl, 3);
			obs_script_t *s = hs(sl.argv[1]);
			s->type = !strcmp(sl.argv[2], "lua")      ? OBS_SCRIPT_LANG_LUA
				  : !strcmp(sl.argv[2], "python") ? OBS_SCRIPT_LANG_PYTHON
								  : OBS_SCRIPT_LANG_UNKNOWN;
		} else if (!strcmp(cmd, "free_backend")) { /* release a script that obs_script_destroy cannot (unknown type) */
			script_need(&sl, 2);
			int i = script_idx(sl.argv[1]);
			extern void obs_lua_script_destroy(obs_script_t *);
			obs_lua_script_destroy(scripts[i]);
			scripts[i] = NULL;

			/* ---- logging ---- */
		} else if (!strcmp(cmd, "logcb")) {
			script_need(&sl, 2);
			obs_scripting_set_log_callback(!strcmp(sl.argv[1], "on") ? log_handler : NULL, NULL);
		} else if (!strcmp(cmd, "slog")) { /* slog S LEVEL TEXT */
			script_need(&sl, 4);
			script_log(hs(sl.argv[1]), atoi(sl.argv[2]), "%s", sl.argv[3]);
		} else if (!strcmp(cmd, "slog_fmt")) { /* slog_fmt S LEVEL : exercises printf-style formatting */
			script_need(&sl, 3);
			script_warn(hs(sl.argv[1]), "n=%d s=%s", 42, "str");
		} else if (!strcmp(cmd, "slog_long")) { /* slog_long S LEVEL N : message of N 'x' characters */
			script_need(&sl, 4);
			int n = atoi(sl.argv[3]);
			char *m = malloc(n + 1);
			memset(m, 'x', n);
			m[n] = 0;
			script_log(hs(sl.argv[1]), atoi(sl.argv[2]), "%s", m);
			free(m);

			/* ---- deferred-call queue ---- */
		} else if (!strcmp(cmd, "defer_post")) { /* defer_post LABEL... */
			for (int i = 1; i < sl.argc; i++)
				defer_call_post(job_fn, strdup(sl.argv[i]));
		} else if (!strcmp(cmd, "defer_wait")) {
			os_event_t *ev;
			os_event_init(&ev, OS_EVENT_TYPE_AUTO);
			defer_call_post(signal_fn, ev);
			os_event_wait(ev);
			os_event_destroy(ev);
		} else if (!strcmp(cmd, "defer_try_wait")) {
			os_event_t *ev;
			os_event_init(&ev, OS_EVENT_TYPE_AUTO);
			defer_call_post(signal_fn, ev);
			int r = os_event_timedwait(ev, 500);
			printf("%sdefer_try_wait %s\n", sl.argc > 1 ? "info_" : "", r == 0 ? "completed" : "timeout");
		} else if (!strcmp(cmd, "defer_dump")) {
			pthread_mutex_lock(&jl_mutex);
			printf("deferred (%d):", njobs);
			for (int i = 0; i < njobs; i++)
				printf(" %s", joblog[i]);
			printf("\n");
			pthread_mutex_unlock(&jl_mutex);

			/* ---- script_callback records ---- */
		} else if (!strcmp(cmd, "cb_add")) { /* cb_add L C EXTRA_SIZE [S] */
			script_need(&sl, 4);
			int l = list_idx(sl.argv[1]), c = cb_idx(sl.argv[2]);
			size_t extra = (size_t)atoi(sl.argv[3]);
			obs_script_t *owner = sl.argc > 4 ? hs(sl.argv[4]) : NULL;
			struct script_callback *cb = add_script_callback(&lists[l], owner, extra);
			cbs[c] = cb;
			bool zero = !cb->removed && !cb->on_remove && !cb->extra.stack && !cb->extra.size;
			uint8_t *tail = (uint8_t *)cb + sizeof(*cb);
			for (size_t i = 0; i < extra; i++)
				zero = zero && tail[i] == 0;
			printf("cb_add %s %s zeroed=%d owner_ok=%d head=%d\n", sl.argv[1], sl.argv[2], zero ? 1 : 0,
			       cb->script == owner ? 1 : 0, lists[l] == cb ? 1 : 0);
		} else if (!strcmp(cmd, "cb_hook")) {
			script_need(&sl, 2);
			cbs[cb_idx(sl.argv[1])]->on_remove = on_remove_hook;
		} else if (!strcmp(cmd, "cb_remove")) {
			script_need(&sl, 2);
			struct script_callback *cb = cbs[cb_idx(sl.argv[1])];
			remove_script_callback(cb);
			printf("cb_remove %s removed=%d detached_head=%d\n", sl.argv[1], script_callback_removed(cb) ? 1 : 0,
			       detached_callbacks == cb ? 1 : 0);
		} else if (!strcmp(cmd, "cb_free")) {
			script_need(&sl, 2);
			int c = cb_idx(sl.argv[1]);
			free_script_callback(cbs[c]);
			cbs[c] = NULL;
			printf("cb_free %s done\n", sl.argv[1]);
		} else if (!strcmp(cmd, "cb_removed")) {
			script_need(&sl, 2);
			printf("cb_removed %s %d\n", sl.argv[1], script_callback_removed(cbs[cb_idx(sl.argv[1])]) ? 1 : 0);
		} else if (!strcmp(cmd, "cb_extra_set")) { /* cb_extra_set C KEY VALUE (calldata int) */
			script_need(&sl, 4);
			calldata_set_int(&cbs[cb_idx(sl.argv[1])]->extra, sl.argv[2], strtoll(sl.argv[3], NULL, 0));
		} else if (!strcmp(cmd, "cb_extra_get")) {
			script_need(&sl, 3);
			printf("cb_extra_get %s %s %lld\n", sl.argv[1], sl.argv[2],
			       calldata_int(&cbs[cb_idx(sl.argv[1])]->extra, sl.argv[2]));
		} else if (!strcmp(cmd, "cb_tail_set")) { /* cb_tail_set C OFFSET HEXBYTES */
			script_need(&sl, 4);
			size_t n, off = (size_t)atoi(sl.argv[2]);
			uint8_t *b = script_parse_hex(sl.argv[3], &n);
			memcpy((uint8_t *)cbs[cb_idx(sl.argv[1])] + sizeof(struct script_callback) + off, b, n);
			free(b);
		} else if (!strcmp(cmd, "cb_tail_get")) { /* cb_tail_get C OFFSET N */
			script_need(&sl, 4);
			size_t n = (size_t)atoi(sl.argv[3]), off = (size_t)atoi(sl.argv[2]);
			printf("cb_tail_get %s ", sl.argv[1]);
			script_print_hex((uint8_t *)cbs[cb_idx(sl.argv[1])] + sizeof(struct script_callback) + off, n);
			printf("\n");
		} else if (!strcmp(cmd, "lists")) {
			for (int i = 0; i < NL; i++) {
				char nm[8];
				snprintf(nm, sizeof(nm), "L%d", i);
				print_list(nm, lists[i]);
			}
			pthread_mutex_lock(&detach_mutex);
			print_list("detached", detached_callbacks);
			pthread_mutex_unlock(&detach_mutex);
		} else if (!strcmp(cmd, "cb_links")) { /* verifies the back-pointer invariant of every listed callback */
			bool ok = true;
			for (int i = 0; i <= NL; i++) {
				struct script_callback **head = i < NL ? &lists[i] : &detached_callbacks;
				struct script_callback **prev = head;
				for (struct script_callback *c = *head; c; c = c->next) {
					ok = ok && c->p_prev_next == prev;
					prev = &c->next;
				}
			}
			printf("cb_links %s\n", ok ? "ok" : "BROKEN");

			/* ---- cstrcache ---- */
		} else if (!strcmp(cmd, "cstr")) { /* cstr I TEXT|@null */
			script_need(&sl, 3);
			int i = atoi(sl.argv[1]);
			const char *in = !strcmp(sl.argv[2], "@null") ? NULL : sl.argv[2];
			const char *r = cstrcache_get(in);
			int same = -1;
			for (int j = 0; j < ncstr; j++)
				if (cstr_ptr[j] == r) {
					same = j;
					break;
				}
			cstr_ptr[i] = r;
			cstr_txt[i] = strdup(r);
			if (i >= ncstr)
				ncstr = i + 1;
			printf("cstr %d text=", i);
			script_print_quoted(r);
			printf(" same_ptr_as=%d is_input=%d\n", same, r == in ? 1 : 0);
		} else if (!strcmp(cmd, "cstr_many")) { /* cstr_many N : intern N distinct strings in slots 100.. */
			int n = atoi(sl.argv[1]);
			for (int k = 0; k < n; k++) {
				char b[32];
				snprintf(b, sizeof(b), "many%d", k);
				cstr_ptr[100 + k] = cstrcache_get(b);
				cstr_txt[100 + k] = strdup(b);
			}
			if (ncstr < 100 + n)
				ncstr = 100 + n;
			printf("cstr_many %d done\n", n);
		} else if (!strcmp(cmd, "cstr_check")) { /* every previously returned pointer still valid and unchanged */
			bool ok = true;
			int checked = 0;
			for (int j = 0; j < ncstr; j++)
				if (cstr_ptr[j]) {
					checked++;
					ok = ok && !strcmp(cstr_ptr[j], cstr_txt[j]) &&
					     cstrcache_get(cstr_txt[j]) == cstr_ptr[j];
				}
			printf("cstr_check %s checked=%d\n", ok ? "ok" : "CHANGED", checked);
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, cmd);
			return 2;
		}
	}
	return 0;
}
