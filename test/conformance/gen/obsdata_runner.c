/*
 * Conformance runner for obs-data (spec S-014). See the section "obsdata runner"
 * in test/conformance/README.md for the command list.
 *
 * Handles: three separate tables of named slots (data objects, arrays, items).
 * A slot owns exactly one reference to what it holds (for items: one item
 * reference). `release`/`a_release`/`item_release` drop that reference and
 * clear the slot. The runner releases whatever is left at the end of a script,
 * so leaks and double frees show up under a sanitizer.
 * Files live in $CONFORMANCE_TMPDIR with the prefix "obsdata-f-".
 */
#include <dirent.h>
#include <inttypes.h>
#include <math.h>
#include <sys/wait.h>
#include <unistd.h>
#include "obs-data.h"
#include "graphics/vec2.h"
#include "graphics/vec3.h"
#include "graphics/vec4.h"
#include "graphics/quat.h"
#include "harness.h"

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

/* ---- slots ---- */
#define MAX_SLOTS 128
struct slot {
	char name[48];
	void *p;
	bool used;
};
static struct slot dat[MAX_SLOTS], arr[MAX_SLOTS], itm[MAX_SLOTS];
static const char *tmpdir;

static void die(const char *msg, const char *what)
{
	fprintf(stderr, "script error: %s %s\n", msg, what ? what : "");
	exit(2);
}

static struct slot *slot_find(struct slot *tbl, const char *name, bool create)
{
	for (int i = 0; i < MAX_SLOTS; i++)
		if (tbl[i].used && !strcmp(tbl[i].name, name))
			return &tbl[i];
	if (!create)
		die("unknown handle", name);
	for (int i = 0; i < MAX_SLOTS; i++)
		if (!tbl[i].used) {
			tbl[i].used = true;
			snprintf(tbl[i].name, sizeof tbl[i].name, "%s", name);
			tbl[i].p = NULL;
			return &tbl[i];
		}
	die("too many handles", name);
	return NULL;
}

/* resolves an existing handle; "@null" -> NULL */
static void *get_p(struct slot *tbl, const char *name)
{
	if (!strcmp(name, "@null"))
		return NULL;
	return slot_find(tbl, name, false)->p;
}
static obs_data_t *D(const char *n)
{
	return get_p(dat, n);
}
static obs_data_array_t *A(const char *n)
{
	return get_p(arr, n);
}
static obs_data_item_t *I(const char *n)
{
	return get_p(itm, n);
}
/* output slot: must be empty */
static void **out_slot(struct slot *tbl, const char *name)
{
	struct slot *s = slot_find(tbl, name, true);
	if (s->p)
		die("handle already holds a value:", name);
	return &s->p;
}

struct peek {
	volatile long ref;
};
static long refs_of(void *p)
{
	return p ? ((struct peek *)p)->ref : -1;
}

static const char *arg(const struct script_line *sl, int i)
{
	script_need(sl, i + 1);
	return strcmp(sl->argv[i], "@null") ? sl->argv[i] : NULL;
}

static void path_of(char *buf, size_t n, const char *name)
{
	snprintf(buf, n, "%s/obsdata-f-%s", tmpdir, name);
}

/* ---- canonical dump (public API only) ---- */
static const char *type_name(enum obs_data_type t)
{
	switch (t) {
	case OBS_DATA_NULL:
		return "null";
	case OBS_DATA_STRING:
		return "string";
	case OBS_DATA_NUMBER:
		return "number";
	case OBS_DATA_BOOLEAN:
		return "bool";
	case OBS_DATA_OBJECT:
		return "object";
	case OBS_DATA_ARRAY:
		return "array";
	}
	return "?";
}
static char numtype_char(enum obs_data_number_type t)
{
	return t == OBS_DATA_NUM_INT ? 'I' : t == OBS_DATA_NUM_DOUBLE ? 'D' : 'X';
}

static void dump_data(obs_data_t *d);

/* prints a key: raw if it is plain printable ASCII, else quoted with escapes */
static void pname(const char *n)
{
	bool plain = true;
	for (const unsigned char *p = (const unsigned char *)n; *p; p++)
		if (*p < 0x21 || *p >= 0x7f || *p == '"' || *p == ',' || *p == ':' || *p == '\\')
			plain = false;
	if (plain)
		printf("%s", n);
	else
		script_print_quoted(n);
}

static void dump_value(obs_data_item_t *it, int mode)
{
	enum obs_data_type t = obs_data_item_gettype(it);
	switch (t) {
	case OBS_DATA_STRING: {
		const char *s = mode == 0   ? obs_data_item_get_string(it)
				: mode == 1 ? obs_data_item_get_default_string(it)
					    : obs_data_item_get_autoselect_string(it);
		script_print_quoted(s);
		break;
	}
	case OBS_DATA_NUMBER: {
		double d = mode == 0   ? obs_data_item_get_double(it)
			   : mode == 1 ? obs_data_item_get_default_double(it)
				       : obs_data_item_get_autoselect_double(it);
		bool effective = mode == 0 || (mode == 1 && !obs_data_item_has_user_value(it)) ||
				 (mode == 2 && !obs_data_item_has_user_value(it) && !obs_data_item_has_default_value(it));
		/* converting an out-of-range double to int is undefined: print "~" instead */
		bool int_ok = (effective && obs_data_item_numtype(it) == OBS_DATA_NUM_INT) || (d > -9e18 && d < 9e18);
		printf("num(%c,", effective ? numtype_char(obs_data_item_numtype(it)) : '?');
		if (int_ok) {
			long long i = mode == 0   ? obs_data_item_get_int(it)
				      : mode == 1 ? obs_data_item_get_default_int(it)
						  : obs_data_item_get_autoselect_int(it);
			printf("%lld", i);
		} else
			printf("~");
		printf(",%.17g)", d);
		break;
	}
	case OBS_DATA_BOOLEAN: {
		bool b = mode == 0   ? obs_data_item_get_bool(it)
			 : mode == 1 ? obs_data_item_get_default_bool(it)
				     : obs_data_item_get_autoselect_bool(it);
		printf("%s", b ? "true" : "false");
		break;
	}
	case OBS_DATA_OBJECT: {
		obs_data_t *o = mode == 0   ? obs_data_item_get_obj(it)
				: mode == 1 ? obs_data_item_get_default_obj(it)
					    : obs_data_item_get_autoselect_obj(it);
		if (!o)
			printf("null");
		else
			dump_data(o);
		obs_data_release(o);
		break;
	}
	case OBS_DATA_ARRAY: {
		obs_data_array_t *a = mode == 0   ? obs_data_item_get_array(it)
				      : mode == 1 ? obs_data_item_get_default_array(it)
						  : obs_data_item_get_autoselect_array(it);
		if (!a)
			printf("null");
		else {
			printf("[");
			size_t n = obs_data_array_count(a);
			for (size_t i = 0; i < n; i++) {
				obs_data_t *e = obs_data_array_item(a, i);
				if (i)
					printf(",");
				dump_data(e);
				obs_data_release(e);
			}
			printf("]");
		}
		obs_data_array_release(a);
		break;
	}
	default:
		printf("?");
	}
}

static void dump_data(obs_data_t *d)
{
	printf("{");
	obs_data_item_t *it = obs_data_first(d);
	bool first = true;
	for (; it;) {
		if (!first)
			printf(",");
		first = false;
		pname(obs_data_item_get_name(it));
		printf(":%s[", type_name(obs_data_item_gettype(it)));
		bool any = false;
		if (obs_data_item_has_user_value(it)) {
			printf("user=");
			dump_value(it, 0);
			any = true;
		}
		if (obs_data_item_has_default_value(it)) {
			printf("%sdefault=", any ? " " : "");
			dump_value(it, 1);
			any = true;
		}
		if (obs_data_item_has_autoselect_value(it)) {
			printf("%sauto=", any ? " " : "");
			dump_value(it, 2);
		}
		printf("]");
		if (!obs_data_item_next(&it))
			break;
	}
	printf("}");
}

/* ---- generic set/get dispatch ---- */
typedef void (*set_str_fn)(obs_data_t *, const char *, const char *);
typedef void (*set_int_fn)(obs_data_t *, const char *, long long);
typedef void (*set_dbl_fn)(obs_data_t *, const char *, double);
typedef void (*set_bool_fn)(obs_data_t *, const char *, bool);
typedef void (*set_obj_fn)(obs_data_t *, const char *, obs_data_t *);
typedef void (*set_arr_fn)(obs_data_t *, const char *, obs_data_array_t *);
static set_str_fn S_STR[3] = {obs_data_set_string, obs_data_set_default_string, obs_data_set_autoselect_string};
static set_int_fn S_INT[3] = {obs_data_set_int, obs_data_set_default_int, obs_data_set_autoselect_int};
static set_dbl_fn S_DBL[3] = {obs_data_set_double, obs_data_set_default_double, obs_data_set_autoselect_double};
static set_bool_fn S_BOOL[3] = {obs_data_set_bool, obs_data_set_default_bool, obs_data_set_autoselect_bool};
static set_obj_fn S_OBJ[3] = {obs_data_set_obj, obs_data_set_default_obj, obs_data_set_autoselect_obj};
static set_arr_fn S_ARR[3] = {obs_data_set_array, obs_data_set_default_array, obs_data_set_autoselect_array};

typedef const char *(*get_str_fn)(obs_data_t *, const char *);
typedef long long (*get_int_fn)(obs_data_t *, const char *);
typedef double (*get_dbl_fn)(obs_data_t *, const char *);
typedef bool (*get_bool_fn)(obs_data_t *, const char *);
typedef obs_data_t *(*get_obj_fn)(obs_data_t *, const char *);
typedef obs_data_array_t *(*get_arr_fn)(obs_data_t *, const char *);
static get_str_fn G_STR[3] = {obs_data_get_string, obs_data_get_default_string, obs_data_get_autoselect_string};
static get_int_fn G_INT[3] = {obs_data_get_int, obs_data_get_default_int, obs_data_get_autoselect_int};
static get_dbl_fn G_DBL[3] = {obs_data_get_double, obs_data_get_default_double, obs_data_get_autoselect_double};
static get_bool_fn G_BOOL[3] = {obs_data_get_bool, obs_data_get_default_bool, obs_data_get_autoselect_bool};
static get_obj_fn G_OBJ[3] = {obs_data_get_obj, obs_data_get_default_obj, obs_data_get_autoselect_obj};
static get_arr_fn G_ARR[3] = {obs_data_get_array, obs_data_get_default_array, obs_data_get_autoselect_array};

typedef void (*iset_str_fn)(obs_data_item_t **, const char *);
typedef void (*iset_int_fn)(obs_data_item_t **, long long);
typedef void (*iset_dbl_fn)(obs_data_item_t **, double);
typedef void (*iset_bool_fn)(obs_data_item_t **, bool);
typedef void (*iset_obj_fn)(obs_data_item_t **, obs_data_t *);
typedef void (*iset_arr_fn)(obs_data_item_t **, obs_data_array_t *);
static iset_str_fn IS_STR[3] = {obs_data_item_set_string, obs_data_item_set_default_string,
				obs_data_item_set_autoselect_string};
static iset_int_fn IS_INT[3] = {obs_data_item_set_int, obs_data_item_set_default_int,
				obs_data_item_set_autoselect_int};
static iset_dbl_fn IS_DBL[3] = {obs_data_item_set_double, obs_data_item_set_default_double,
				obs_data_item_set_autoselect_double};
static iset_bool_fn IS_BOOL[3] = {obs_data_item_set_bool, obs_data_item_set_default_bool,
				  obs_data_item_set_autoselect_bool};
static iset_obj_fn IS_OBJ[3] = {obs_data_item_set_obj, obs_data_item_set_default_obj,
				obs_data_item_set_autoselect_obj};
static iset_arr_fn IS_ARR[3] = {obs_data_item_set_array, obs_data_item_set_default_array,
				obs_data_item_set_autoselect_array};

typedef const char *(*iget_str_fn)(obs_data_item_t *);
typedef long long (*iget_int_fn)(obs_data_item_t *);
typedef double (*iget_dbl_fn)(obs_data_item_t *);
typedef bool (*iget_bool_fn)(obs_data_item_t *);
typedef obs_data_t *(*iget_obj_fn)(obs_data_item_t *);
typedef obs_data_array_t *(*iget_arr_fn)(obs_data_item_t *);
static iget_str_fn IG_STR[3] = {obs_data_item_get_string, obs_data_item_get_default_string,
				obs_data_item_get_autoselect_string};
static iget_int_fn IG_INT[3] = {obs_data_item_get_int, obs_data_item_get_default_int,
				obs_data_item_get_autoselect_int};
static iget_dbl_fn IG_DBL[3] = {obs_data_item_get_double, obs_data_item_get_default_double,
				obs_data_item_get_autoselect_double};
static iget_bool_fn IG_BOOL[3] = {obs_data_item_get_bool, obs_data_item_get_default_bool,
				  obs_data_item_get_autoselect_bool};
static iget_obj_fn IG_OBJ[3] = {obs_data_item_get_obj, obs_data_item_get_default_obj,
				obs_data_item_get_autoselect_obj};
static iget_arr_fn IG_ARR[3] = {obs_data_item_get_array, obs_data_item_get_default_array,
				obs_data_item_get_autoselect_array};

typedef bool (*has_fn)(obs_data_t *, const char *);
static has_fn HAS[3] = {obs_data_has_user_value, obs_data_has_default_value, obs_data_has_autoselect_value};
typedef void (*unset_fn)(obs_data_t *, const char *);
static unset_fn UNSET[3] = {obs_data_unset_user_value, obs_data_unset_default_value,
			    obs_data_unset_autoselect_value};
typedef bool (*ihas_fn)(obs_data_item_t *);
static ihas_fn IHAS[3] = {obs_data_item_has_user_value, obs_data_item_has_default_value,
			  obs_data_item_has_autoselect_value};
typedef void (*iunset_fn)(obs_data_item_t *);
static iunset_fn IUNSET[3] = {obs_data_item_unset_user_value, obs_data_item_unset_default_value,
			      obs_data_item_unset_autoselect_value};

typedef void (*svec2_fn)(obs_data_t *, const char *, const struct vec2 *);
typedef void (*svec3_fn)(obs_data_t *, const char *, const struct vec3 *);
typedef void (*svec4_fn)(obs_data_t *, const char *, const struct vec4 *);
typedef void (*squat_fn)(obs_data_t *, const char *, const struct quat *);
static svec2_fn SV2[3] = {obs_data_set_vec2, obs_data_set_default_vec2, obs_data_set_autoselect_vec2};
static svec3_fn SV3[3] = {obs_data_set_vec3, obs_data_set_default_vec3, obs_data_set_autoselect_vec3};
static svec4_fn SV4[3] = {obs_data_set_vec4, obs_data_set_default_vec4, obs_data_set_autoselect_vec4};
static squat_fn SQ[3] = {obs_data_set_quat, obs_data_set_default_quat, obs_data_set_autoselect_quat};
typedef void (*gvec2_fn)(obs_data_t *, const char *, struct vec2 *);
typedef void (*gvec3_fn)(obs_data_t *, const char *, struct vec3 *);
typedef void (*gvec4_fn)(obs_data_t *, const char *, struct vec4 *);
typedef void (*gquat_fn)(obs_data_t *, const char *, struct quat *);
static gvec2_fn GV2[3] = {obs_data_get_vec2, obs_data_get_default_vec2, obs_data_get_autoselect_vec2};
static gvec3_fn GV3[3] = {obs_data_get_vec3, obs_data_get_default_vec3, obs_data_get_autoselect_vec3};
static gvec4_fn GV4[3] = {obs_data_get_vec4, obs_data_get_default_vec4, obs_data_get_autoselect_vec4};
static gquat_fn GQ[3] = {obs_data_get_quat, obs_data_get_default_quat, obs_data_get_autoselect_quat};

typedef void (*sfps_fn)(obs_data_t *, const char *, struct media_frames_per_second, const char *);
typedef bool (*gfps_fn)(obs_data_t *, const char *, struct media_frames_per_second *, const char **);
static sfps_fn SFPS[3] = {obs_data_set_frames_per_second, obs_data_set_default_frames_per_second,
			  obs_data_set_autoselect_frames_per_second};
static gfps_fn GFPS[3] = {obs_data_get_frames_per_second, obs_data_get_default_frames_per_second,
			  obs_data_get_autoselect_frames_per_second};
typedef void (*isfps_fn)(obs_data_item_t **, struct media_frames_per_second, const char *);
typedef bool (*igfps_fn)(obs_data_item_t *, struct media_frames_per_second *, const char **);
static isfps_fn ISFPS[3] = {obs_data_item_set_frames_per_second, obs_data_item_set_default_frames_per_second,
			    obs_data_item_set_autoselect_frames_per_second};
static igfps_fn IGFPS[3] = {obs_data_item_get_frames_per_second, obs_data_item_get_default_frames_per_second,
			    obs_data_item_get_autoselect_frames_per_second};

static bool eat(const char **s, const char *prefix)
{
	size_t n = strlen(prefix);
	if (strncmp(*s, prefix, n))
		return false;
	*s += n;
	return true;
}

static void print_str_or_null(const char *s)
{
	if (s)
		script_print_quoted(s);
	else
		printf("@null");
}

static void cb_enum(obs_data_t *d, void *param)
{
	int *idx = param;
	printf("enum %d ", (*idx)++);
	dump_data(d);
	printf("\n");
}

static void print_file(const char *name)
{
	char p[1024];
	path_of(p, sizeof p, name);
	FILE *f = fopen(p, "rb");
	if (!f) {
		printf("file %s nofile\n", name);
		return;
	}
	static char buf[65536];
	size_t n = fread(buf, 1, sizeof buf - 1, f);
	fclose(f);
	buf[n] = 0;
	printf("file %s ", name);
	script_print_quoted(buf);
	printf("\n");
}

static int exec(const struct script_line *sl);

static int do_try(const struct script_line *sl, bool quiet)
{
	/* "try CMD ARGS ; CMD ARGS ; ..." runs the commands in a forked child so that
	 * a crash is reported instead of killing the script. State changes of the
	 * child are not visible to the parent. */
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		if (quiet && !freopen("/dev/null", "w", stdout))
			_exit(3);
		struct script_line sub = *sl;
		int start = 1;
		for (int i = 1; i <= sl->argc; i++) {
			if (i == sl->argc || !strcmp(sl->argv[i], ";")) {
				if (i > start) {
					struct script_line one = *sl;
					one.argc = i - start;
					for (int k = 0; k < one.argc; k++)
						one.argv[k] = one.storage + (sl->argv[start + k] - sl->storage);
					exec(&one);
				}
				start = i + 1;
			}
		}
		(void)sub;
		fflush(stdout);
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFSIGNALED(st))
		printf("try %s crash\n", sl->argv[1]);
	else if (WEXITSTATUS(st) != 0)
		printf("try %s exit-%d\n", sl->argv[1], WEXITSTATUS(st));
	else
		printf("try %s ok\n", sl->argv[1]);
	return 0;
}

static int exec(const struct script_line *sl)
{
	const char *cmd = sl->argv[0];
	char path[1024];

	/* ---- named commands ---- */
	if (!strcmp(cmd, "echo")) {
		printf("echo");
		for (int i = 1; i < sl->argc; i++)
			printf(" %s", sl->argv[i]);
		printf("\n");
	} else if (!strcmp(cmd, "try") || !strcmp(cmd, "try_quiet")) {
		script_need(sl, 2);
		do_try(sl, cmd[3] == '_');
	} else if (!strcmp(cmd, "new")) {
		script_need(sl, 2);
		*out_slot(dat, sl->argv[1]) = obs_data_create();
	} else if (!strcmp(cmd, "release")) {
		script_need(sl, 2);
		struct slot *s = slot_find(dat, sl->argv[1], false);
		obs_data_release(s->p);
		s->p = NULL;
		s->used = false;
	} else if (!strcmp(cmd, "addref")) { /* addref D E : E becomes a second handle */
		script_need(sl, 3);
		obs_data_t *d = D(sl->argv[1]);
		obs_data_addref(d);
		*out_slot(dat, sl->argv[2]) = d;
	} else if (!strcmp(cmd, "newref")) { /* obs_data_newref(D) -> E */
		script_need(sl, 3);
		*out_slot(dat, sl->argv[2]) = obs_data_newref(D(sl->argv[1]));
	} else if (!strcmp(cmd, "refs") || !strcmp(cmd, "info_refs")) {
		script_need(sl, 2);
		printf("%s %s %ld\n", cmd, sl->argv[1], refs_of(D(sl->argv[1])));
	} else if (!strcmp(cmd, "same")) { /* same D E : pointer identity */
		script_need(sl, 3);
		printf("same %s %s %d\n", sl->argv[1], sl->argv[2], D(sl->argv[1]) == D(sl->argv[2]));
	} else if (!strcmp(cmd, "from_json")) {
		script_need(sl, 3);
		obs_data_t *d = obs_data_create_from_json(arg(sl, 2));
		printf("from_json %s %s\n", sl->argv[1], d ? "ok" : "fail");
		if (d)
			*out_slot(dat, sl->argv[1]) = d;
	} else if (!strcmp(cmd, "from_json_deep")) { /* from_json_deep D KIND N: N nested levels, KIND obj|arr */
		script_need(sl, 4);
		int n = atoi(sl->argv[3]);
		bool ao = !strcmp(sl->argv[2], "arr");
		size_t cap = (size_t)n * 8 + 64;
		char *t = malloc(cap), *w = t;
		for (int i = 0; i < n; i++)
			w += sprintf(w, ao ? "[" : "{\"a\":");
		w += sprintf(w, "1");
		for (int i = 0; i < n; i++)
			w += sprintf(w, ao ? "]" : "}");
		obs_data_t *d = obs_data_create_from_json(t);
		free(t);
		printf("from_json_deep %s %s\n", sl->argv[1], d ? "ok" : "fail");
		if (d)
			*out_slot(dat, sl->argv[1]) = d;
	} else if (!strcmp(cmd, "from_json_file") || !strcmp(cmd, "from_json_file_safe")) {
		bool safe = cmd[14] == '_';
		script_need(sl, safe ? 4 : 3);
		path_of(path, sizeof path, sl->argv[2]);
		obs_data_t *d = safe ? obs_data_create_from_json_file_safe(path, arg(sl, 3))
				     : obs_data_create_from_json_file(path);
		printf("%s %s %s\n", cmd, sl->argv[1], d ? "ok" : "fail");
		if (d)
			*out_slot(dat, sl->argv[1]) = d;
	} else if (!strncmp(cmd, "json", 4) || !strcmp(cmd, "last_json")) {
		/* json | json_defaults | json_pretty | json_pretty_defaults | last_json */
		script_need(sl, 2);
		obs_data_t *d = D(sl->argv[1]);
		const char *j;
		if (!strcmp(cmd, "json"))
			j = obs_data_get_json(d);
		else if (!strcmp(cmd, "json_defaults"))
			j = obs_data_get_json_with_defaults(d);
		else if (!strcmp(cmd, "json_pretty"))
			j = obs_data_get_json_pretty(d);
		else if (!strcmp(cmd, "json_pretty_defaults"))
			j = obs_data_get_json_pretty_with_defaults(d);
		else if (!strcmp(cmd, "last_json"))
			j = obs_data_get_last_json(d);
		else
			die("unknown command", cmd);
		printf("%s %s ", cmd, sl->argv[1]);
		print_str_or_null(j);
		printf("\n");
	} else if (!strcmp(cmd, "save_json")) {
		script_need(sl, 3);
		path_of(path, sizeof path, sl->argv[2]);
		printf("save_json %d\n", obs_data_save_json(D(sl->argv[1]), path));
	} else if (!strcmp(cmd, "save_json_safe") || !strcmp(cmd, "save_json_pretty_safe")) {
		script_need(sl, 5);
		path_of(path, sizeof path, sl->argv[2]);
		bool pretty = cmd[10] == 'p';
		bool ok = pretty ? obs_data_save_json_pretty_safe(D(sl->argv[1]), path, arg(sl, 3), arg(sl, 4))
				 : obs_data_save_json_safe(D(sl->argv[1]), path, arg(sl, 3), arg(sl, 4));
		printf("%s %d\n", cmd, ok);
	} else if (!strcmp(cmd, "write_file")) { /* write_file NAME CONTENT */
		script_need(sl, 3);
		path_of(path, sizeof path, sl->argv[1]);
		FILE *f = fopen(path, "wb");
		if (!f)
			die("cannot write", path);
		fwrite(sl->argv[2], 1, strlen(sl->argv[2]), f);
		fclose(f);
	} else if (!strcmp(cmd, "read_file")) {
		script_need(sl, 2);
		print_file(sl->argv[1]);
	} else if (!strcmp(cmd, "exists")) {
		script_need(sl, 2);
		path_of(path, sizeof path, sl->argv[1]);
		FILE *f = fopen(path, "rb");
		printf("exists %s %d\n", sl->argv[1], f != NULL);
		if (f)
			fclose(f);
	} else if (!strcmp(cmd, "remove_file")) {
		script_need(sl, 2);
		path_of(path, sizeof path, sl->argv[1]);
		remove(path);
	} else if (!strcmp(cmd, "apply")) {
		script_need(sl, 3);
		obs_data_apply(D(sl->argv[1]), D(sl->argv[2]));
	} else if (!strcmp(cmd, "erase")) {
		script_need(sl, 3);
		obs_data_erase(D(sl->argv[1]), arg(sl, 2));
	} else if (!strcmp(cmd, "clear")) {
		script_need(sl, 2);
		obs_data_clear(D(sl->argv[1]));
	} else if (!strcmp(cmd, "get_defaults")) { /* get_defaults D E */
		script_need(sl, 3);
		*out_slot(dat, sl->argv[2]) = obs_data_get_defaults(D(sl->argv[1]));
	} else if (!strcmp(cmd, "dump")) {
		script_need(sl, 2);
		printf("dump %s ", sl->argv[1]);
		if (!D(sl->argv[1]))
			printf("@null");
		else
			dump_data(D(sl->argv[1]));
		printf("\n");
	} else if (!strcmp(cmd, "iter")) { /* walk with first/next, printing name and type */
		script_need(sl, 2);
		obs_data_item_t *it = obs_data_first(D(sl->argv[1]));
		printf("iter %s", sl->argv[1]);
		while (it) {
			printf(" ");
			pname(obs_data_item_get_name(it));
			printf(":%s", type_name(obs_data_item_gettype(it)));
			if (!obs_data_item_next(&it))
				break;
		}
		printf("\n");
	} else if (!strcmp(cmd, "names")) { /* names of all items in iteration order, one line */
		script_need(sl, 2);
		obs_data_item_t *it = obs_data_first(D(sl->argv[1]));
		printf("names %s", sl->argv[1]);
		while (it) {
			printf(" ");
			pname(obs_data_item_get_name(it));
			if (!obs_data_item_next(&it))
				break;
		}
		printf("\n");
	}
	/* ---- arrays ---- */
	else if (!strcmp(cmd, "a_new")) {
		script_need(sl, 2);
		*out_slot(arr, sl->argv[1]) = obs_data_array_create();
	} else if (!strcmp(cmd, "a_release")) {
		script_need(sl, 2);
		struct slot *s = slot_find(arr, sl->argv[1], false);
		obs_data_array_release(s->p);
		s->p = NULL;
		s->used = false;
	} else if (!strcmp(cmd, "a_addref")) {
		script_need(sl, 3);
		obs_data_array_t *a = A(sl->argv[1]);
		obs_data_array_addref(a);
		*out_slot(arr, sl->argv[2]) = a;
	} else if (!strcmp(cmd, "a_refs") || !strcmp(cmd, "info_a_refs")) {
		script_need(sl, 2);
		printf("%s %s %ld\n", cmd, sl->argv[1], refs_of(A(sl->argv[1])));
	} else if (!strcmp(cmd, "a_count")) {
		script_need(sl, 2);
		printf("a_count %s %zu\n", sl->argv[1], obs_data_array_count(A(sl->argv[1])));
	} else if (!strcmp(cmd, "a_item")) { /* a_item A idx D : D = item or empty */
		script_need(sl, 4);
		obs_data_t *d = obs_data_array_item(A(sl->argv[1]), (size_t)strtoull(sl->argv[2], NULL, 0));
		printf("a_item %s %s %s\n", sl->argv[1], sl->argv[2], d ? "ok" : "null");
		if (d)
			*out_slot(dat, sl->argv[3]) = d;
	} else if (!strcmp(cmd, "a_push")) {
		script_need(sl, 3);
		printf("a_push %s %zu\n", sl->argv[1], obs_data_array_push_back(A(sl->argv[1]), D(sl->argv[2])));
	} else if (!strcmp(cmd, "a_insert")) {
		script_need(sl, 4);
		obs_data_array_insert(A(sl->argv[1]), (size_t)strtoull(sl->argv[2], NULL, 0), D(sl->argv[3]));
	} else if (!strcmp(cmd, "a_push_array")) {
		script_need(sl, 3);
		obs_data_array_push_back_array(A(sl->argv[1]), A(sl->argv[2]));
	} else if (!strcmp(cmd, "a_erase")) {
		script_need(sl, 3);
		obs_data_array_erase(A(sl->argv[1]), (size_t)strtoull(sl->argv[2], NULL, 0));
	} else if (!strcmp(cmd, "a_enum")) {
		script_need(sl, 2);
		int idx = 0;
		obs_data_array_enum(A(sl->argv[1]), cb_enum, &idx);
		printf("a_enum %s done %d\n", sl->argv[1], idx);
	} else if (!strcmp(cmd, "a_enum_nullcb")) {
		script_need(sl, 2);
		obs_data_array_enum(A(sl->argv[1]), NULL, NULL);
	} else if (!strcmp(cmd, "a_dump")) {
		script_need(sl, 2);
		obs_data_array_t *a = A(sl->argv[1]);
		printf("a_dump %s [", sl->argv[1]);
		for (size_t i = 0; i < obs_data_array_count(a); i++) {
			obs_data_t *e = obs_data_array_item(a, i);
			if (i)
				printf(",");
			dump_data(e);
			obs_data_release(e);
		}
		printf("]\n");
	}
	/* ---- items ---- */
	else if (!strcmp(cmd, "first")) { /* first D I */
		script_need(sl, 3);
		obs_data_item_t *it = obs_data_first(D(sl->argv[1]));
		printf("first %s %s\n", sl->argv[1], it ? "ok" : "null");
		if (it)
			*out_slot(itm, sl->argv[2]) = it;
	} else if (!strcmp(cmd, "byname")) { /* byname D name I */
		script_need(sl, 4);
		obs_data_item_t *it = obs_data_item_byname(D(sl->argv[1]), arg(sl, 2));
		printf("byname %s %s %s\n", sl->argv[1], sl->argv[2], it ? "ok" : "null");
		if (it)
			*out_slot(itm, sl->argv[3]) = it;
	} else if (!strcmp(cmd, "next")) { /* next I : advance in place; slot is emptied at the end */
		script_need(sl, 2);
		struct slot *s = slot_find(itm, sl->argv[1], false);
		bool r = obs_data_item_next((obs_data_item_t **)&s->p);
		printf("next %s %d", sl->argv[1], r);
		if (r)
			printf(" %s", obs_data_item_get_name(s->p));
		printf("\n");
	} else if (!strcmp(cmd, "next_null")) {
		obs_data_item_t *nul = NULL;
		printf("next_null %d\n", obs_data_item_next(&nul));
		printf("next_nullptr %d\n", obs_data_item_next(NULL));
	} else if (!strcmp(cmd, "item_release")) {
		script_need(sl, 2);
		struct slot *s = slot_find(itm, sl->argv[1], false);
		obs_data_item_release((obs_data_item_t **)&s->p);
		s->p = NULL;
		s->used = false;
	} else if (!strcmp(cmd, "item_remove")) {
		script_need(sl, 2);
		struct slot *s = slot_find(itm, sl->argv[1], false);
		obs_data_item_remove((obs_data_item_t **)&s->p);
		s->p = NULL;
		s->used = false;
	} else if (!strcmp(cmd, "item_refs")) {
		script_need(sl, 2);
		printf("item_refs %s %ld\n", sl->argv[1], refs_of(I(sl->argv[1])));
	} else if (!strcmp(cmd, "item_type")) {
		script_need(sl, 2);
		printf("item_type %s %s\n", sl->argv[1], type_name(obs_data_item_gettype(I(sl->argv[1]))));
	} else if (!strcmp(cmd, "item_numtype")) {
		script_need(sl, 2);
		enum obs_data_number_type t = obs_data_item_numtype(I(sl->argv[1]));
		printf("item_numtype %s %s\n", sl->argv[1], t == OBS_DATA_NUM_INT ? "int" : t == OBS_DATA_NUM_DOUBLE ? "double" : "invalid");
	} else if (!strcmp(cmd, "item_name")) {
		script_need(sl, 2);
		printf("item_name %s ", sl->argv[1]);
		print_str_or_null(obs_data_item_get_name(I(sl->argv[1])));
		printf("\n");
	} else if (!strcmp(cmd, "item_dump")) {
		script_need(sl, 2);
		obs_data_item_t *it = I(sl->argv[1]);
		printf("item_dump %s ", sl->argv[1]);
		pname(obs_data_item_get_name(it));
		printf(":%s[", type_name(obs_data_item_gettype(it)));
		bool any = false;
		if (obs_data_item_has_user_value(it)) {
			printf("user=");
			dump_value(it, 0);
			any = true;
		}
		if (obs_data_item_has_default_value(it)) {
			printf("%sdefault=", any ? " " : "");
			dump_value(it, 1);
			any = true;
		}
		if (obs_data_item_has_autoselect_value(it)) {
			printf("%sauto=", any ? " " : "");
			dump_value(it, 2);
		}
		printf("]\n");
	} else {
		/* ---- generic families ---- */
		const char *c = cmd;
		bool item = eat(&c, "item_");
		int op = 0; /* 1 set, 2 get, 3 has, 4 unset */
		if (eat(&c, "set_"))
			op = 1;
		else if (eat(&c, "get_"))
			op = 2;
		else if (eat(&c, "has_"))
			op = 3;
		else if (eat(&c, "unset_"))
			op = 4;
		else
			die("unknown command", cmd);
		int mode = 0;
		if (eat(&c, "default_"))
			mode = 1;
		else if (eat(&c, "autoselect_"))
			mode = 2;
		const char *kind = c;
		/* has_user / has_default / has_autoselect / unset_user ... */
		if (op == 3 || op == 4) {
			mode = !strcmp(kind, "user") ? 0 : !strcmp(kind, "default") ? 1 : !strcmp(kind, "autoselect") ? 2 : -1;
			if (mode < 0)
				die("unknown command", cmd);
			if (item) {
				script_need(sl, 2);
				obs_data_item_t *it = I(sl->argv[1]);
				if (op == 3)
					printf("%s %s %d\n", cmd, sl->argv[1], IHAS[mode](it));
				else
					IUNSET[mode](it);
			} else {
				script_need(sl, 3);
				if (op == 3)
					printf("%s %s %s %d\n", cmd, sl->argv[1], sl->argv[2], HAS[mode](D(sl->argv[1]), arg(sl, 2)));
				else
					UNSET[mode](D(sl->argv[1]), arg(sl, 2));
			}
			return 0;
		}
		/* data-level: <cmd> D name [value]; item-level: <cmd> I [value] */
		int base = item ? 2 : 3; /* index of first value argument */
		obs_data_t *d = item ? NULL : D(sl->argv[1]);
		struct slot *is = item ? slot_find(itm, sl->argv[1], false) : NULL;
		obs_data_item_t **pit = item ? (obs_data_item_t **)&is->p : NULL;
		obs_data_item_t *it = item ? is->p : NULL;
		const char *name = item ? NULL : arg(sl, 2);
		script_need(sl, base);
		if (op == 1) { /* set */
			if (!strcmp(kind, "string")) {
				script_need(sl, base + 1);
				if (item)
					IS_STR[mode](pit, arg(sl, base));
				else
					S_STR[mode](d, name, arg(sl, base));
			} else if (!strcmp(kind, "int")) {
				script_need(sl, base + 1);
				long long v = strtoll(sl->argv[base], NULL, 0);
				if (item)
					IS_INT[mode](pit, v);
				else
					S_INT[mode](d, name, v);
			} else if (!strcmp(kind, "double")) {
				script_need(sl, base + 1);
				double v = strtod(sl->argv[base], NULL);
				if (item)
					IS_DBL[mode](pit, v);
				else
					S_DBL[mode](d, name, v);
			} else if (!strcmp(kind, "bool")) {
				script_need(sl, base + 1);
				bool v = strtol(sl->argv[base], NULL, 0) != 0;
				if (item)
					IS_BOOL[mode](pit, v);
				else
					S_BOOL[mode](d, name, v);
			} else if (!strcmp(kind, "obj")) {
				script_need(sl, base + 1);
				obs_data_t *v = D(sl->argv[base]);
				if (item)
					IS_OBJ[mode](pit, v);
				else
					S_OBJ[mode](d, name, v);
			} else if (!strcmp(kind, "array")) {
				script_need(sl, base + 1);
				obs_data_array_t *v = A(sl->argv[base]);
				if (item)
					IS_ARR[mode](pit, v);
				else
					S_ARR[mode](d, name, v);
			} else if (!strcmp(kind, "vec2") && !item) {
				script_need(sl, 5);
				struct vec2 v = {{{(float)strtod(sl->argv[3], NULL), (float)strtod(sl->argv[4], NULL)}}};
				SV2[mode](d, name, &v);
			} else if (!strcmp(kind, "vec3") && !item) {
				script_need(sl, 6);
				struct vec3 v;
				vec3_set(&v, (float)strtod(sl->argv[3], NULL), (float)strtod(sl->argv[4], NULL),
					 (float)strtod(sl->argv[5], NULL));
				SV3[mode](d, name, &v);
			} else if (!strcmp(kind, "vec4") && !item) {
				script_need(sl, 7);
				struct vec4 v;
				v.x = (float)strtod(sl->argv[3], NULL);
				v.y = (float)strtod(sl->argv[4], NULL);
				v.z = (float)strtod(sl->argv[5], NULL);
				v.w = (float)strtod(sl->argv[6], NULL);
				SV4[mode](d, name, &v);
			} else if (!strcmp(kind, "quat") && !item) {
				script_need(sl, 7);
				struct quat v;
				v.x = (float)strtod(sl->argv[3], NULL);
				v.y = (float)strtod(sl->argv[4], NULL);
				v.z = (float)strtod(sl->argv[5], NULL);
				v.w = (float)strtod(sl->argv[6], NULL);
				SQ[mode](d, name, &v);
			} else if (!strcmp(kind, "frames_per_second")) {
				script_need(sl, base + 3);
				struct media_frames_per_second f = {(uint32_t)strtoull(sl->argv[base], NULL, 0),
								    (uint32_t)strtoull(sl->argv[base + 1], NULL, 0)};
				if (item)
					ISFPS[mode](pit, f, arg(sl, base + 2));
				else
					SFPS[mode](d, name, f, arg(sl, base + 2));
			} else
				die("unknown command", cmd);
			if (item && pit)
				; /* the slot already holds the (possibly relocated) item */
			return 0;
		}
		/* get: prints "<cmd> <handle> [<name>] <value>" */
		if (item)
			printf("%s %s ", cmd, sl->argv[1]);
		else
			printf("%s %s %s ", cmd, sl->argv[1], sl->argv[2]);
		if (!strcmp(kind, "string")) {
			print_str_or_null(item ? IG_STR[mode](it) : G_STR[mode](d, name));
			printf("\n");
		} else if (!strcmp(kind, "int")) {
			printf("%lld\n", item ? IG_INT[mode](it) : G_INT[mode](d, name));
		} else if (!strcmp(kind, "double")) {
			printf("%.17g\n", item ? IG_DBL[mode](it) : G_DBL[mode](d, name));
		} else if (!strcmp(kind, "bool")) {
			printf("%d\n", (item ? IG_BOOL[mode](it) : G_BOOL[mode](d, name)) ? 1 : 0);
		} else if (!strcmp(kind, "obj")) { /* get_obj D name E / item_get_obj I E */
			script_need(sl, base + 1);
			obs_data_t *o = item ? IG_OBJ[mode](it) : G_OBJ[mode](d, name);
			printf("%s\n", o ? "ok" : "null");
			if (o)
				*out_slot(dat, sl->argv[base]) = o;
		} else if (!strcmp(kind, "array")) {
			script_need(sl, base + 1);
			obs_data_array_t *a = item ? IG_ARR[mode](it) : G_ARR[mode](d, name);
			printf("%s\n", a ? "ok" : "null");
			if (a)
				*out_slot(arr, sl->argv[base]) = a;
		} else if (!strcmp(kind, "vec2") && !item) {
			struct vec2 v = {{{7.f, 7.f}}};
			GV2[mode](d, name, &v);
			printf("%.9g %.9g\n", v.x, v.y);
		} else if (!strcmp(kind, "vec3") && !item) {
			struct vec3 v;
			vec3_set(&v, 7.f, 7.f, 7.f);
			GV3[mode](d, name, &v);
			printf("%.9g %.9g %.9g\n", v.x, v.y, v.z);
		} else if (!strcmp(kind, "vec4") && !item) {
			struct vec4 v = {{{7.f, 7.f, 7.f, 7.f}}};
			GV4[mode](d, name, &v);
			printf("%.9g %.9g %.9g %.9g\n", v.x, v.y, v.z, v.w);
		} else if (!strcmp(kind, "quat") && !item) {
			struct quat v = {{{7.f, 7.f, 7.f, 7.f}}};
			GQ[mode](d, name, &v);
			printf("%.9g %.9g %.9g %.9g\n", v.x, v.y, v.z, v.w);
		} else if (!strcmp(kind, "frames_per_second")) {
			struct media_frames_per_second f = {7, 7};
			const char *opt = "<unset>";
			bool want_opt = sl->argc > base && !strcmp(sl->argv[base], "opt");
			bool r = item ? IGFPS[mode](it, &f, want_opt ? &opt : NULL)
				      : GFPS[mode](d, name, &f, want_opt ? &opt : NULL);
			printf("%d %u %u ", r, f.numerator, f.denominator);
			if (want_opt)
				script_print_quoted(opt);
			else
				printf("-");
			printf("\n");
		} else
			die("unknown command", cmd);
	}
	return 0;
}

static void cleanup_files(void)
{
	DIR *dir = opendir(tmpdir);
	if (!dir)
		return;
	struct dirent *e;
	while ((e = readdir(dir))) {
		if (strncmp(e->d_name, "obsdata-f-", 10))
			continue;
		char p[1024];
		snprintf(p, sizeof p, "%s/%s", tmpdir, e->d_name);
		remove(p);
	}
	closedir(dir);
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	tmpdir = getenv("CONFORMANCE_TMPDIR");
	if (!tmpdir)
		tmpdir = ".";
	cleanup_files();

	while (script_next(f, &sl, &lineno))
		exec(&sl);

	for (int i = 0; i < MAX_SLOTS; i++)
		if (itm[i].used && itm[i].p)
			obs_data_item_release((obs_data_item_t **)&itm[i].p);
	for (int i = 0; i < MAX_SLOTS; i++)
		if (arr[i].used && arr[i].p)
			obs_data_array_release(arr[i].p);
	for (int i = 0; i < MAX_SLOTS; i++)
		if (dat[i].used && dat[i].p)
			obs_data_release(dat[i].p);
	fclose(f);
	cleanup_files();
	return 0;
}
