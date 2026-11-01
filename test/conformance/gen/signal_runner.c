/*
 * Conformance runner for signal handlers, global callbacks and proc handlers
 * (spec S-004). See test/conformance/README.md for the script language.
 *
 * Commands
 *   handler <h>                           signal_handler_create
 *   destroy <h>                           signal_handler_destroy
 *   add <h> <decl>                        signal_handler_add
 *   callback <cb> [action...]             define a scripted callback (see below)
 *   gcallback <gcb> [action...]           define a scripted global callback
 *   connect <h> <sig> <cb>                signal_handler_connect
 *   connect_ref <h> <sig> <cb>            signal_handler_connect_ref
 *   disconnect <h> <sig> <cb>             signal_handler_disconnect
 *   connect_global <h> <gcb>              signal_handler_connect_global
 *   disconnect_global <h> <gcb>           signal_handler_disconnect_global
 *   emit <h> <sig> [name=int ...]         signal_handler_signal with int params
 *   procs <p>                             proc_handler_create
 *   proc_add <p> <decl> <label> [action]  proc_handler_add (callback prints, optional ret)
 *   proc_call <p> <name> [name=int ...]   proc_handler_call
 *   remove_current                        signal_handler_remove_current (outside callbacks)
 *
 * Actions (executed in order each time a callback runs, after it prints its
 * trace line):
 *   remove_current                 signal_handler_remove_current()
 *   disconnect:<h>:<sig>:<cb>      disconnect another (or the same) callback
 *   connect:<h>:<sig>:<cb>         connect a callback
 *   connect_ref:<h>:<sig>:<cb>
 *   disconnect_global:<h>:<gcb>    disconnect a global callback
 *   connect_global:<h>:<gcb>
 *   emit:<h>:<sig>                 emit with the same params (nested emit, depth-limited)
 *   inc:<name>                     add 1 to int param <name> (in the calldata being signalled)
 *   ret:<int>                      (proc callbacks) set int "return"
 */
#include "callback/decl.h"
#include "callback/proc.h"
#include "callback/signal.h"
#include "harness.h"

#define MAXN 32
#define MAXACT 8
#define MAX_DEPTH 6

struct scripted {
	char label[32];
	char *actions[MAXACT];
	int nactions;
	bool global;
	bool proc;
	bool used;
};

static struct scripted cbs[MAXN];
static signal_handler_t *handlers[MAXN];
static char handler_names[MAXN][32];
static proc_handler_t *procs[MAXN];
static char proc_names[MAXN][32];
static int depth;

static signal_handler_t *find_handler(const char *name)
{
	for (int i = 0; i < MAXN; i++)
		if (handlers[i] && !strcmp(handler_names[i], name))
			return handlers[i];
	fprintf(stderr, "script: unknown handler %s\n", name);
	exit(2);
}

static struct scripted *find_cb(const char *name)
{
	for (int i = 0; i < MAXN; i++)
		if (cbs[i].used && !strcmp(cbs[i].label, name))
			return &cbs[i];
	fprintf(stderr, "script: unknown callback %s\n", name);
	exit(2);
}

static void split_action(const char *action, char parts[4][64], int *n)
{
	*n = 0;
	const char *p = action;
	while (*p && *n < 4) {
		const char *q = strchr(p, ':');
		size_t len = q ? (size_t)(q - p) : strlen(p);
		memcpy(parts[*n], p, len);
		parts[*n][len] = 0;
		(*n)++;
		if (!q)
			break;
		p = q + 1;
	}
}

/* Real callback entry points. The "data" pointer identifies the scripted callback. */
static void exec_action(struct scripted *self, const char *action, calldata_t *params);

static void cb_entry(void *data, calldata_t *params)
{
	struct scripted *self = data;
	long long n = 0;
	if (calldata_get_int(params, "n", &n))
		printf("call %s n=%lld\n", self->label, n);
	else
		printf("call %s\n", self->label);
	for (int i = 0; i < self->nactions; i++)
		exec_action(self, self->actions[i], params);
}

static void gcb_entry(void *data, const char *signal, calldata_t *params)
{
	struct scripted *self = data;
	long long n = 0;
	if (calldata_get_int(params, "n", &n))
		printf("gcall %s signal=%s n=%lld\n", self->label, signal, n);
	else
		printf("gcall %s signal=%s\n", self->label, signal);
	for (int i = 0; i < self->nactions; i++)
		exec_action(self, self->actions[i], params);
}

static const char *cur_signal_for_emit;

static void exec_action(struct scripted *self, const char *action, calldata_t *params)
{
	char parts[4][64];
	int n;
	split_action(action, parts, &n);
	const char *a = parts[0];
	(void)self;

	if (!strcmp(a, "remove_current")) {
		signal_handler_remove_current();
	} else if (!strcmp(a, "disconnect")) {
		struct scripted *t = find_cb(parts[3]);
		signal_handler_disconnect(find_handler(parts[1]), parts[2], cb_entry, t);
	} else if (!strcmp(a, "connect")) {
		struct scripted *t = find_cb(parts[3]);
		signal_handler_connect(find_handler(parts[1]), parts[2], cb_entry, t);
	} else if (!strcmp(a, "connect_ref")) {
		struct scripted *t = find_cb(parts[3]);
		signal_handler_connect_ref(find_handler(parts[1]), parts[2], cb_entry, t);
	} else if (!strcmp(a, "connect_global")) {
		signal_handler_connect_global(find_handler(parts[1]), gcb_entry, find_cb(parts[2]));
	} else if (!strcmp(a, "disconnect_global")) {
		signal_handler_disconnect_global(find_handler(parts[1]), gcb_entry, find_cb(parts[2]));
	} else if (!strcmp(a, "emit")) {
		if (depth < MAX_DEPTH) {
			depth++;
			printf("nested-emit %s\n", parts[2]);
			signal_handler_signal(find_handler(parts[1]), parts[2], params);
			depth--;
		}
	} else if (!strcmp(a, "inc")) {
		long long v = 0;
		calldata_get_int(params, parts[1], &v);
		calldata_set_int(params, parts[1], v + 1);
	} else if (!strcmp(a, "ret")) {
		calldata_set_int(params, "return", strtoll(parts[1], NULL, 10));
	} else {
		fprintf(stderr, "script: unknown action %s\n", a);
		exit(2);
	}
}

static void proc_entry(void *data, calldata_t *params)
{
	struct scripted *self = data;
	long long n = 0;
	if (calldata_get_int(params, "n", &n))
		printf("pcall %s n=%lld\n", self->label, n);
	else
		printf("pcall %s\n", self->label);
	for (int i = 0; i < self->nactions; i++)
		exec_action(self, self->actions[i], params);
}

static void fill_params(calldata_t *cd, const struct script_line *sl, int first)
{
	for (int i = first; i < sl->argc; i++) {
		char *eq = strchr(sl->argv[i], '=');
		if (!eq) {
			fprintf(stderr, "script: bad param '%s'\n", sl->argv[i]);
			exit(2);
		}
		*eq = 0;
		calldata_set_int(cd, sl->argv[i], strtoll(eq + 1, NULL, 10));
	}
}

static struct scripted *define_cb(const struct script_line *sl, bool global, bool proc, int first_action)
{
	for (int i = 0; i < MAXN; i++) {
		if (!cbs[i].used) {
			struct scripted *c = &cbs[i];
			c->used = true;
			c->global = global;
			c->proc = proc;
			snprintf(c->label, sizeof(c->label), "%s", sl->argv[1]);
			for (int a = first_action; a < sl->argc && c->nactions < MAXACT; a++)
				c->actions[c->nactions++] = strdup(sl->argv[a]);
			return c;
		}
	}
	fprintf(stderr, "script: too many callbacks\n");
	exit(2);
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;

	while (script_next(f, &sl, &lineno)) {
		const char *cmd = sl.argv[0];

		if (!strcmp(cmd, "handler")) {
			script_need(&sl, 2);
			for (int i = 0; i < MAXN; i++) {
				if (!handlers[i]) {
					handlers[i] = signal_handler_create();
					snprintf(handler_names[i], 32, "%s", sl.argv[1]);
					break;
				}
			}
		} else if (!strcmp(cmd, "destroy")) {
			script_need(&sl, 2);
			for (int i = 0; i < MAXN; i++) {
				if (handlers[i] && !strcmp(handler_names[i], sl.argv[1])) {
					signal_handler_destroy(handlers[i]);
					/* keep the pointer: with outstanding connect_ref references the
					 * handler is intentionally still usable (see spec) */
					printf("destroy %s\n", sl.argv[1]);
					break;
				}
			}
		} else if (!strcmp(cmd, "add")) {
			script_need(&sl, 3);
			bool ok = signal_handler_add(find_handler(sl.argv[1]), sl.argv[2]);
			printf("add %s %s\n", sl.argv[1], ok ? "ok" : "fail");
		} else if (!strcmp(cmd, "callback")) {
			script_need(&sl, 2);
			define_cb(&sl, false, false, 2);
		} else if (!strcmp(cmd, "gcallback")) {
			script_need(&sl, 2);
			define_cb(&sl, true, false, 2);
		} else if (!strcmp(cmd, "connect")) {
			script_need(&sl, 4);
			signal_handler_connect(find_handler(sl.argv[1]), sl.argv[2], cb_entry, find_cb(sl.argv[3]));
		} else if (!strcmp(cmd, "connect_ref")) {
			script_need(&sl, 4);
			signal_handler_connect_ref(find_handler(sl.argv[1]), sl.argv[2], cb_entry, find_cb(sl.argv[3]));
		} else if (!strcmp(cmd, "disconnect")) {
			script_need(&sl, 4);
			signal_handler_disconnect(find_handler(sl.argv[1]), sl.argv[2], cb_entry, find_cb(sl.argv[3]));
		} else if (!strcmp(cmd, "connect_global")) {
			script_need(&sl, 3);
			signal_handler_connect_global(find_handler(sl.argv[1]), gcb_entry, find_cb(sl.argv[2]));
		} else if (!strcmp(cmd, "disconnect_global")) {
			script_need(&sl, 3);
			signal_handler_disconnect_global(find_handler(sl.argv[1]), gcb_entry, find_cb(sl.argv[2]));
		} else if (!strcmp(cmd, "emit")) {
			script_need(&sl, 3);
			calldata_t cd;
			calldata_init(&cd);
			fill_params(&cd, &sl, 3);
			printf("emit %s\n", sl.argv[2]);
			cur_signal_for_emit = sl.argv[2];
			signal_handler_signal(find_handler(sl.argv[1]), sl.argv[2], &cd);
			long long n;
			if (calldata_get_int(&cd, "n", &n))
				printf("emit-done n=%lld\n", n);
			else
				printf("emit-done\n");
			calldata_free(&cd);
		} else if (!strcmp(cmd, "remove_current")) {
			signal_handler_remove_current();
			printf("remove_current outside-callback done\n");
		} else if (!strcmp(cmd, "procs")) {
			script_need(&sl, 2);
			for (int i = 0; i < MAXN; i++) {
				if (!procs[i]) {
					procs[i] = proc_handler_create();
					snprintf(proc_names[i], 32, "%s", sl.argv[1]);
					break;
				}
			}
		} else if (!strcmp(cmd, "proc_add")) {
			script_need(&sl, 4);
			proc_handler_t *p = NULL;
			for (int i = 0; i < MAXN; i++)
				if (procs[i] && !strcmp(proc_names[i], sl.argv[1]))
					p = procs[i];
			struct scripted *c = NULL;
			for (int i = 0; i < MAXN; i++)
				if (!cbs[i].used) {
					c = &cbs[i];
					break;
				}
			c->used = true;
			c->proc = true;
			snprintf(c->label, sizeof(c->label), "%s", sl.argv[3]);
			for (int a = 4; a < sl.argc && c->nactions < MAXACT; a++)
				c->actions[c->nactions++] = strdup(sl.argv[a]);
			proc_handler_add(p, sl.argv[2], proc_entry, c);
			printf("proc_add %s\n", sl.argv[2]);
		} else if (!strcmp(cmd, "proc_call")) {
			script_need(&sl, 3);
			proc_handler_t *p = NULL;
			for (int i = 0; i < MAXN; i++)
				if (procs[i] && !strcmp(proc_names[i], sl.argv[1]))
					p = procs[i];
			calldata_t cd;
			calldata_init(&cd);
			fill_params(&cd, &sl, 3);
			bool ok = proc_handler_call(p, sl.argv[2], &cd);
			long long r;
			if (ok && calldata_get_int(&cd, "return", &r))
				printf("proc_call %s %s return=%lld\n", sl.argv[2], "ok", r);
			else
				printf("proc_call %s %s\n", sl.argv[2], ok ? "ok" : "fail");
			calldata_free(&cd);
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, cmd);
			return 2;
		}
	}
	(void)cur_signal_for_emit;
	fclose(f);
	return 0;
}
