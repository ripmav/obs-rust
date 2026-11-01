/*
 * Conformance runner for obs_properties / obs_property (spec S-015).
 * See the "props runner" section of test/conformance/README.md.
 *
 * Objects (all referenced by script-chosen names):
 *   token T                       opaque labelled pointer; used as param / priv / data
 *   behavior B action...          scripted callback behaviour (actions below)
 *   props P [TOKEN [destroy|nodestroy]]   obs_properties_create / create_param
 *   obj O [TOKEN|@null]           a context object (its ->data is TOKEN) for button clicks
 *   data D                        an obs_data settings object
 * Property references ("H" in the command list) are either a handle bound with a
 * trailing "as NAME", the form  P:NAME  (looked up with obs_properties_get at use time),
 * or @null.  Container references are a props name or @null.
 *
 * Commands
 *   props / destroy P / set_param P TOKEN|@null destroy|nodestroy / get_param P
 *   flags P V / get_flags P / first P [as H] / get P NAME [as H] / next H [as H2]
 *   parent P / remove P NAME / apply P D|@null
 *   data D / data_set D KEY int|str|bool|dbl V / data_show D / data_free D
 *   add KIND P NAME DESC args... [as H]        KIND: bool int int_slider float float_slider
 *        text(TYPE) path(TYPE FILTER DEFAULT) list(TYPE FORMAT) color color_alpha
 *        button(CB) button2(CB PRIV) font editable_list(TYPE FILTER DEFAULT) frame_rate group(TYPE G)
 *   set H WHAT args...      visible enabled description long_description int_limits float_limits
 *        int_suffix float_suffix text_monospace text_info_type text_word_wrap button_type button_url
 *        modified BEH|@null  modified2 BEH|@null TOKEN|@null  click BEH
 *   info H / probe H / dump P / list_item H IDX
 *   list_add H FMT NAME VAL / list_insert H FMT IDX NAME VAL / list_remove H IDX /
 *   list_clear H / list_disable H IDX 0|1
 *   fr_option_add H N D / fr_option_insert H IDX N D / fr_range_add H n d n d /
 *   fr_range_insert H IDX n d n d / fr_clear H / fr_options_clear H / fr_ranges_clear H
 *   modified H D|@null / click H O|@null
 *
 * Behaviour actions (run in order after the trace line):
 *   ret:V set:K:V ... (see exec_action)
 */
#include "obs-internal.h"
#include "obs-properties.h"
#include "harness.h"

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

#define MAXN 128
#define MAXACT 12

struct token {
	char label[32];
	bool used;
};
struct behav {
	char label[32];
	char *act[MAXACT];
	int n;
	bool used;
};
struct cont {
	char name[32];
	obs_properties_t *p;
	bool used, dead, owned;
};
struct handle {
	char name[32];
	obs_property_t *p;
	bool used;
};
struct dat {
	char name[32];
	obs_data_t *d;
	bool used;
};
struct obj {
	char name[32];
	struct obs_context_data ctx;
	bool used;
};
struct bind {
	obs_property_t *p;
	struct behav *mod, *mod2, *click;
	bool used;
};

static struct token tokens[MAXN];
static struct behav behs[MAXN];
static struct cont conts[MAXN];
static struct handle hands[MAXN];
static struct dat dats[MAXN];
static struct obj objs[MAXN];
static struct bind binds[512];
static bool quiet;

static void die(const char *m, const char *a)
{
	fprintf(stderr, "script: %s %s\n", m, a ? a : "");
	exit(2);
}

#define FIND(tab, nm, what)                                              \
	do {                                                             \
		for (int i_ = 0; i_ < MAXN; i_++)                        \
			if (tab[i_].used && !strcmp(tab[i_].name, nm))   \
				return &tab[i_];                         \
		die("unknown " what, nm);                                \
		return NULL;                                             \
	} while (0)

static struct token *tok(const char *l)
{
	if (!strcmp(l, "@null"))
		return NULL;
	for (int i = 0; i < MAXN; i++)
		if (tokens[i].used && !strcmp(tokens[i].label, l))
			return &tokens[i];
	die("unknown token", l);
	return NULL;
}
static struct behav *beh(const char *l)
{
	if (!strcmp(l, "@null"))
		return NULL;
	for (int i = 0; i < MAXN; i++)
		if (behs[i].used && !strcmp(behs[i].label, l))
			return &behs[i];
	die("unknown behavior", l);
	return NULL;
}
static struct cont *cont_entry(const char *n)
{
	FIND(conts, n, "props");
}
static struct dat *dat_entry(const char *n)
{
	FIND(dats, n, "data");
}
static struct obj *obj_entry(const char *n)
{
	FIND(objs, n, "obj");
}

static obs_properties_t *cref(const char *n)
{
	if (!strcmp(n, "@null"))
		return NULL;
	struct cont *c = cont_entry(n);
	if (c->dead)
		die("props destroyed:", n);
	return c->p;
}
static obs_data_t *dref(const char *n)
{
	return !strcmp(n, "@null") ? NULL : dat_entry(n)->d;
}

static const char *cont_name(obs_properties_t *p)
{
	if (!p)
		return "@null";
	for (int i = 0; i < MAXN; i++)
		if (conts[i].used && !conts[i].dead && conts[i].p == p)
			return conts[i].name;
	return "?";
}

static obs_property_t *pref(const char *s)
{
	if (!strcmp(s, "@null"))
		return NULL;
	const char *colon = strchr(s, ':');
	if (colon) {
		char pn[32];
		size_t n = (size_t)(colon - s);
		memcpy(pn, s, n);
		pn[n] = 0;
		return obs_properties_get(cref(pn), colon + 1);
	}
	for (int i = 0; i < MAXN; i++)
		if (hands[i].used && !strcmp(hands[i].name, s))
			return hands[i].p;
	die("unknown handle", s);
	return NULL;
}

static void bind_handle(const char *name, obs_property_t *p)
{
	for (int i = 0; i < MAXN; i++)
		if (hands[i].used && !strcmp(hands[i].name, name)) {
			hands[i].p = p;
			return;
		}
	for (int i = 0; i < MAXN; i++)
		if (!hands[i].used) {
			hands[i].used = true;
			snprintf(hands[i].name, sizeof(hands[i].name), "%s", name);
			hands[i].p = p;
			return;
		}
	die("too many handles", name);
}

/* Optional trailing "as H". Returns the effective argc (without it). */
static int take_as(struct script_line *sl, obs_property_t *p, bool bind)
{
	int n = sl->argc;
	if (n >= 3 && !strcmp(sl->argv[n - 2], "as")) {
		if (bind)
			bind_handle(sl->argv[n - 1], p);
		return n - 2;
	}
	return n;
}
static int peel_as(struct script_line *sl, char **name)
{
	int n = sl->argc;
	*name = NULL;
	if (n >= 3 && !strcmp(sl->argv[n - 2], "as")) {
		*name = sl->argv[n - 1];
		return n - 2;
	}
	return n;
}

/* ---- binding table (property pointer -> scripted behaviours) ---- */
static struct bind *bind_get(obs_property_t *p, bool create)
{
	for (int i = 0; i < 512; i++)
		if (binds[i].used && binds[i].p == p)
			return &binds[i];
	if (!create)
		return NULL;
	for (int i = 0; i < 512; i++)
		if (!binds[i].used) {
			memset(&binds[i], 0, sizeof(binds[i]));
			binds[i].used = true;
			binds[i].p = p;
			return &binds[i];
		}
	die("too many bindings", "");
	return NULL;
}

/* Must run before a property tree is freed so addresses can be reused safely. */
static void unbind_tree(obs_properties_t *props)
{
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		struct bind *b = bind_get(p, false);
		if (b)
			b->used = false;
		for (int i = 0; i < MAXN; i++)
			if (hands[i].used && hands[i].p == p)
				hands[i].used = false;
		if (obs_property_get_type(p) == OBS_PROPERTY_GROUP) {
			obs_properties_t *g = obs_property_group_content(p);
			unbind_tree(g);
			for (int i = 0; i < MAXN; i++)
				if (conts[i].used && conts[i].p == g)
					conts[i].dead = true;
		}
	}
}

/* ---- enum tables ---- */
static const char *const t_ptype[] = {"invalid", "bool", "int", "float", "text", "path", "list",
				      "color", "button", "font", "editable_list", "frame_rate", "group", "color_alpha"};
static const char *const t_cfmt[] = {"invalid", "int", "float", "string", "bool"};
static const char *const t_ctype[] = {"invalid", "editable", "list", "radio"};
static const char *const t_etype[] = {"strings", "files", "files_and_urls"};
static const char *const t_path[] = {"file", "file_save", "directory"};
static const char *const t_text[] = {"default", "password", "multiline", "info"};
static const char *const t_info[] = {"normal", "warning", "error"};
static const char *const t_num[] = {"scroller", "slider"};
static const char *const t_group[] = {"invalid", "normal", "checkable"};
static const char *const t_btn[] = {"default", "url"};

#define NT(t) (int)(sizeof(t) / sizeof(t[0]))
static int en(const char *const *t, int n, const char *s)
{
	for (int i = 0; i < n; i++)
		if (!strcmp(t[i], s))
			return i;
	return (int)strtol(s, NULL, 0);
}
static void pen(const char *const *t, int n, int v)
{
	if (v >= 0 && v < n)
		fputs(t[v], stdout);
	else
		printf("?%d", v);
}
#define EN(t, s) en(t, NT(t), s)
#define PEN(t, v) pen(t, NT(t), (int)(v))

static void ps(const char *s)
{
	if (!s)
		fputs("@null", stdout);
	else
		script_print_quoted(s);
}

/* ---- settings printing (keys sorted so that order never matters) ---- */
static int cmp_s(const void *a, const void *b)
{
	return strcmp(*(char *const *)a, *(char *const *)b);
}
static void print_settings(obs_data_t *d)
{
	if (!d) {
		fputs("@null", stdout);
		return;
	}
	char *keys[64];
	int n = 0;
	obs_data_item_t *it = obs_data_first(d);
	for (; it; obs_data_item_next(&it))
		if (n < 64)
			keys[n++] = strdup(obs_data_item_get_name(it));
	qsort(keys, (size_t)n, sizeof(char *), cmp_s);
	putchar('{');
	for (int i = 0; i < n; i++) {
		obs_data_item_t *item = obs_data_item_byname(d, keys[i]);
		if (i)
			putchar(',');
		printf("%s=", keys[i]);
		switch (obs_data_item_gettype(item)) {
		case OBS_DATA_STRING:
			ps(obs_data_item_get_string(item));
			break;
		case OBS_DATA_BOOLEAN:
			fputs(obs_data_item_get_bool(item) ? "true" : "false", stdout);
			break;
		case OBS_DATA_NUMBER:
			if (obs_data_item_numtype(item) == OBS_DATA_NUM_INT)
				printf("%lld", obs_data_item_get_int(item));
			else
				printf("%.17g", obs_data_item_get_double(item));
			break;
		default:
			fputs("?", stdout);
		}
		obs_data_item_release(&item);
		free(keys[i]);
	}
	putchar('}');
}

/* ---- scripted callbacks ---- */
static void exec_action(const char *action, obs_properties_t *props, obs_property_t *self, obs_data_t *settings,
			bool *ret);

static void run_actions(struct behav *b, obs_properties_t *props, obs_property_t *self, obs_data_t *settings, bool *ret)
{
	for (int i = 0; i < b->n; i++)
		exec_action(b->act[i], props, self, settings, ret);
}

static bool mod_entry(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	struct bind *bd = bind_get(p, false);
	if (!bd || !bd->mod)
		die("modified callback without behavior", obs_property_name(p));
	printf("modified %s prop=%s props=%s settings=", bd->mod->label, obs_property_name(p), cont_name(props));
	print_settings(settings);
	putchar('\n');
	bool ret = false;
	run_actions(bd->mod, props, p, settings, &ret);
	return ret;
}

static bool mod2_entry(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	struct bind *bd = bind_get(p, false);
	if (!bd || !bd->mod2)
		die("modified2 callback without behavior", obs_property_name(p));
	printf("modified2 %s priv=%s prop=%s props=%s settings=", bd->mod2->label,
	       priv ? ((struct token *)priv)->label : "@null", obs_property_name(p), cont_name(props));
	print_settings(settings);
	putchar('\n');
	bool ret = false;
	run_actions(bd->mod2, props, p, settings, &ret);
	return ret;
}

static bool click_entry(obs_properties_t *props, obs_property_t *p, void *data)
{
	struct bind *bd = bind_get(p, false);
	if (!bd || !bd->click)
		die("click callback without behavior", obs_property_name(p));
	printf("click %s prop=%s props=%s data=%s\n", bd->click->label, obs_property_name(p), cont_name(props),
	       data ? ((struct token *)data)->label : "@null");
	bool ret = false;
	run_actions(bd->click, props, p, NULL, &ret);
	return ret;
}

static void param_destroy(void *param)
{
	if (!quiet)
		printf("param-destroy %s\n", ((struct token *)param)->label);
}

/* action: name[:arg[:arg[:arg]]] */
static void exec_action(const char *action, obs_properties_t *props, obs_property_t *self, obs_data_t *settings,
			bool *ret)
{
	char part[4][64];
	int n = 0;
	const char *p = action;
	while (*p && n < 4) {
		const char *q = strchr(p, ':');
		size_t len = q ? (size_t)(q - p) : strlen(p);
		memcpy(part[n], p, len);
		part[n][len] = 0;
		n++;
		if (!q)
			break;
		p = q + 1;
	}
	const char *a = part[0];
	if (!strcmp(a, "ret")) {
		*ret = strtol(part[1], NULL, 0) != 0;
	} else if (!strcmp(a, "set_int")) {
		obs_data_set_int(settings, part[1], strtoll(part[2], NULL, 0));
	} else if (!strcmp(a, "set_str")) {
		obs_data_set_string(settings, part[1], part[2]);
	} else if (!strcmp(a, "set_bool")) {
		obs_data_set_bool(settings, part[1], strtol(part[2], NULL, 0) != 0);
	} else if (!strcmp(a, "visible")) {
		obs_property_set_visible(obs_properties_get(props, part[1]), strtol(part[2], NULL, 0) != 0);
	} else if (!strcmp(a, "enabled")) {
		obs_property_set_enabled(obs_properties_get(props, part[1]), strtol(part[2], NULL, 0) != 0);
	} else if (!strcmp(a, "self_visible")) {
		obs_property_set_visible(self, strtol(part[1], NULL, 0) != 0);
	} else if (!strcmp(a, "self_enabled")) {
		obs_property_set_enabled(self, strtol(part[1], NULL, 0) != 0);
	} else if (!strcmp(a, "desc")) {
		obs_property_set_description(obs_properties_get(props, part[1]), part[2]);
	} else if (!strcmp(a, "add_bool")) {
		obs_property_t *np = obs_properties_add_bool(props, part[1], part[1]);
		printf("  action add_bool %s %s\n", part[1], np ? "ok" : "null");
	} else if (!strcmp(a, "remove")) {
		obs_properties_remove_by_name(props, part[1]);
		printf("  action remove %s\n", part[1]);
	} else if (!strcmp(a, "parent")) {
		printf("  action parent %s\n", cont_name(obs_properties_get_parent(props)));
	} else if (!strcmp(a, "list_add")) {
		obs_property_list_add_string(obs_properties_get(props, part[1]), part[2], part[3]);
	} else if (!strcmp(a, "list_clear")) {
		obs_property_list_clear(obs_properties_get(props, part[1]));
	} else if (!strcmp(a, "apply")) {
		printf("  action apply (nested)\n");
		obs_properties_apply_settings(props, settings);
	} else {
		die("unknown action", action);
	}
}

/* ---- property printing ---- */
static void print_item_value(obs_property_t *p, size_t i)
{
	switch (obs_property_list_format(p)) {
	case OBS_COMBO_FORMAT_STRING:
		ps(obs_property_list_item_string(p, i));
		break;
	case OBS_COMBO_FORMAT_INT:
		printf("%lld", obs_property_list_item_int(p, i));
		break;
	case OBS_COMBO_FORMAT_FLOAT:
		printf("%.17g", obs_property_list_item_float(p, i));
		break;
	case OBS_COMBO_FORMAT_BOOL:
		printf("%d", obs_property_list_item_bool(p, i));
		break;
	default:
		fputs("-", stdout);
	}
}

static void print_fps(struct media_frames_per_second f)
{
	printf("%u/%u", f.numerator, f.denominator);
}

static void print_prop(obs_property_t *p, int ind, bool recurse)
{
	printf("%*sprop ", ind, "");
	if (!p) {
		puts("@null");
		return;
	}
	enum obs_property_type t = obs_property_get_type(p);
	printf("%s type=", obs_property_name(p));
	PEN(t_ptype, t);
	fputs(" desc=", stdout);
	ps(obs_property_description(p));
	fputs(" long=", stdout);
	ps(obs_property_long_description(p));
	printf(" enabled=%d visible=%d\n", obs_property_enabled(p), obs_property_visible(p));
	printf("%*s ", ind, "");
	switch (t) {
	case OBS_PROPERTY_INT:
		printf(" min=%d max=%d step=%d numtype=", obs_property_int_min(p), obs_property_int_max(p),
		       obs_property_int_step(p));
		PEN(t_num, obs_property_int_type(p));
		fputs(" suffix=", stdout);
		ps(obs_property_int_suffix(p));
		break;
	case OBS_PROPERTY_FLOAT:
		printf(" min=%.17g max=%.17g step=%.17g numtype=", obs_property_float_min(p), obs_property_float_max(p),
		       obs_property_float_step(p));
		PEN(t_num, obs_property_float_type(p));
		fputs(" suffix=", stdout);
		ps(obs_property_float_suffix(p));
		break;
	case OBS_PROPERTY_TEXT:
		fputs(" texttype=", stdout);
		PEN(t_text, obs_property_text_type(p));
		printf(" monospace=%d infotype=", obs_property_text_monospace(p));
		PEN(t_info, obs_property_text_info_type(p));
		printf(" wrap=%d", obs_property_text_info_word_wrap(p));
		break;
	case OBS_PROPERTY_PATH:
		fputs(" pathtype=", stdout);
		PEN(t_path, obs_property_path_type(p));
		fputs(" filter=", stdout);
		ps(obs_property_path_filter(p));
		fputs(" default=", stdout);
		ps(obs_property_path_default_path(p));
		break;
	case OBS_PROPERTY_LIST: {
		fputs(" listtype=", stdout);
		PEN(t_ctype, obs_property_list_type(p));
		fputs(" format=", stdout);
		PEN(t_cfmt, obs_property_list_format(p));
		size_t cnt = obs_property_list_item_count(p);
		printf(" count=%zu", cnt);
		for (size_t i = 0; i < cnt; i++) {
			printf("\n%*s   item %zu name=", ind, "", i);
			ps(obs_property_list_item_name(p, i));
			printf(" disabled=%d value=", obs_property_list_item_disabled(p, i));
			print_item_value(p, i);
		}
		break;
	}
	case OBS_PROPERTY_BUTTON:
		fputs(" buttontype=", stdout);
		PEN(t_btn, obs_property_button_type(p));
		fputs(" url=", stdout);
		ps(obs_property_button_url(p));
		break;
	case OBS_PROPERTY_EDITABLE_LIST:
		fputs(" listtype=", stdout);
		PEN(t_etype, obs_property_editable_list_type(p));
		fputs(" filter=", stdout);
		ps(obs_property_editable_list_filter(p));
		fputs(" default=", stdout);
		ps(obs_property_editable_list_default_path(p));
		break;
	case OBS_PROPERTY_FRAME_RATE: {
		size_t no = obs_property_frame_rate_options_count(p), nr = obs_property_frame_rate_fps_ranges_count(p);
		printf(" options=%zu ranges=%zu", no, nr);
		for (size_t i = 0; i < no; i++) {
			printf("\n%*s   option %zu name=", ind, "", i);
			ps(obs_property_frame_rate_option_name(p, i));
			fputs(" desc=", stdout);
			ps(obs_property_frame_rate_option_description(p, i));
		}
		for (size_t i = 0; i < nr; i++) {
			printf("\n%*s   range %zu min=", ind, "", i);
			print_fps(obs_property_frame_rate_fps_range_min(p, i));
			fputs(" max=", stdout);
			print_fps(obs_property_frame_rate_fps_range_max(p, i));
		}
		break;
	}
	case OBS_PROPERTY_GROUP:
		fputs(" grouptype=", stdout);
		PEN(t_group, obs_property_group_type(p));
		printf(" content=%s", cont_name(obs_property_group_content(p)));
		break;
	default:
		fputs(" -", stdout);
	}
	putchar('\n');
	if (recurse && t == OBS_PROPERTY_GROUP) {
		obs_properties_t *g = obs_property_group_content(p);
		for (obs_property_t *c = obs_properties_first(g); c; obs_property_next(&c))
			print_prop(c, ind + 4, true);
	}
}

static void probe(obs_property_t *p)
{
	printf("probe int: %d %d %d ", obs_property_int_min(p), obs_property_int_max(p), obs_property_int_step(p));
	PEN(t_num, obs_property_int_type(p));
	putchar(' ');
	ps(obs_property_int_suffix(p));
	printf("\nprobe float: %.17g %.17g %.17g ", obs_property_float_min(p), obs_property_float_max(p),
	       obs_property_float_step(p));
	PEN(t_num, obs_property_float_type(p));
	putchar(' ');
	ps(obs_property_float_suffix(p));
	fputs("\nprobe text: ", stdout);
	PEN(t_text, obs_property_text_type(p));
	printf(" %d ", obs_property_text_monospace(p));
	PEN(t_info, obs_property_text_info_type(p));
	printf(" %d", obs_property_text_info_word_wrap(p));
	fputs("\nprobe path: ", stdout);
	PEN(t_path, obs_property_path_type(p));
	putchar(' ');
	ps(obs_property_path_filter(p));
	putchar(' ');
	ps(obs_property_path_default_path(p));
	fputs("\nprobe list: ", stdout);
	PEN(t_ctype, obs_property_list_type(p));
	putchar(' ');
	PEN(t_cfmt, obs_property_list_format(p));
	printf(" count=%zu name0=", obs_property_list_item_count(p));
	ps(obs_property_list_item_name(p, 0));
	fputs(" str0=", stdout);
	ps(obs_property_list_item_string(p, 0));
	printf(" int0=%lld float0=%.17g bool0=%d disabled0=%d", obs_property_list_item_int(p, 0),
	       obs_property_list_item_float(p, 0), obs_property_list_item_bool(p, 0),
	       obs_property_list_item_disabled(p, 0));
	fputs("\nprobe editable_list: ", stdout);
	PEN(t_etype, obs_property_editable_list_type(p));
	putchar(' ');
	ps(obs_property_editable_list_filter(p));
	putchar(' ');
	ps(obs_property_editable_list_default_path(p));
	printf("\nprobe frame_rate: options=%zu ranges=%zu name0=", obs_property_frame_rate_options_count(p),
	       obs_property_frame_rate_fps_ranges_count(p));
	ps(obs_property_frame_rate_option_name(p, 0));
	fputs(" desc0=", stdout);
	ps(obs_property_frame_rate_option_description(p, 0));
	fputs(" min0=", stdout);
	print_fps(obs_property_frame_rate_fps_range_min(p, 0));
	fputs(" max0=", stdout);
	print_fps(obs_property_frame_rate_fps_range_max(p, 0));
	fputs("\nprobe group: ", stdout);
	PEN(t_group, obs_property_group_type(p));
	printf(" content=%s", cont_name(obs_property_group_content(p)));
	fputs("\nprobe button: ", stdout);
	PEN(t_btn, obs_property_button_type(p));
	putchar(' ');
	ps(obs_property_button_url(p));
	putchar('\n');
}

static bool getb(const char *s)
{
	return strtol(s, NULL, 0) != 0;
}

static size_t gs(const char *s)
{
	return (size_t)strtoull(s, NULL, 0);
}

static const char *sv(const char *s)
{
	return !strcmp(s, "@null") ? NULL : s;
}

static struct media_frames_per_second fps(const char *n, const char *d)
{
	struct media_frames_per_second f = {(uint32_t)strtoul(n, NULL, 0), (uint32_t)strtoul(d, NULL, 0)};
	return f;
}

static void do_add(struct script_line *sl)
{
	script_need(sl, 5);
	const char *kind = sl->argv[1];
	obs_properties_t *P = cref(sl->argv[2]);
	const char *name = sl->argv[3];
	const char *desc = sv(sl->argv[4]);
	char *as;
	int argc = peel_as(sl, &as);
	obs_property_t *np = NULL;
	struct behav *cb = NULL;
	struct token *priv = NULL;
	obs_properties_t *grp = NULL;
	struct cont *grp_entry = NULL;

	if (!strcmp(kind, "bool")) {
		np = obs_properties_add_bool(P, name, desc);
	} else if (!strcmp(kind, "int") || !strcmp(kind, "int_slider")) {
		if (argc < 8)
			die("add int needs min max step", "");
		int mn = (int)strtol(sl->argv[5], NULL, 0), mx = (int)strtol(sl->argv[6], NULL, 0),
		    st = (int)strtol(sl->argv[7], NULL, 0);
		np = !strcmp(kind, "int") ? obs_properties_add_int(P, name, desc, mn, mx, st)
					  : obs_properties_add_int_slider(P, name, desc, mn, mx, st);
	} else if (!strcmp(kind, "float") || !strcmp(kind, "float_slider")) {
		if (argc < 8)
			die("add float needs min max step", "");
		double mn = strtod(sl->argv[5], NULL), mx = strtod(sl->argv[6], NULL), st = strtod(sl->argv[7], NULL);
		np = !strcmp(kind, "float") ? obs_properties_add_float(P, name, desc, mn, mx, st)
					    : obs_properties_add_float_slider(P, name, desc, mn, mx, st);
	} else if (!strcmp(kind, "text")) {
		script_need(sl, 6);
		np = obs_properties_add_text(P, name, desc, EN(t_text, sl->argv[5]));
	} else if (!strcmp(kind, "path")) {
		if (argc < 8)
			die("add path needs type filter default", "");
		np = obs_properties_add_path(P, name, desc, EN(t_path, sl->argv[5]), sv(sl->argv[6]), sv(sl->argv[7]));
	} else if (!strcmp(kind, "list")) {
		if (argc < 7)
			die("add list needs type format", "");
		np = obs_properties_add_list(P, name, desc, EN(t_ctype, sl->argv[5]), EN(t_cfmt, sl->argv[6]));
	} else if (!strcmp(kind, "color")) {
		np = obs_properties_add_color(P, name, desc);
	} else if (!strcmp(kind, "color_alpha")) {
		np = obs_properties_add_color_alpha(P, name, desc);
	} else if (!strcmp(kind, "button") || !strcmp(kind, "button2")) {
		script_need(sl, 6);
		cb = beh(sl->argv[5]);
		if (!strcmp(kind, "button")) {
			np = obs_properties_add_button(P, name, desc, cb ? click_entry : NULL);
		} else {
			script_need(sl, 7);
			priv = tok(sl->argv[6]);
			np = obs_properties_add_button2(P, name, desc, cb ? click_entry : NULL, priv);
		}
		if (np && cb)
			bind_get(np, true)->click = cb;
	} else if (!strcmp(kind, "font")) {
		np = obs_properties_add_font(P, name, desc);
	} else if (!strcmp(kind, "editable_list")) {
		if (argc < 8)
			die("add editable_list needs type filter default", "");
		np = obs_properties_add_editable_list(P, name, desc, EN(t_etype, sl->argv[5]), sv(sl->argv[6]),
						      sv(sl->argv[7]));
	} else if (!strcmp(kind, "frame_rate")) {
		np = obs_properties_add_frame_rate(P, name, desc);
	} else if (!strcmp(kind, "group")) {
		if (argc < 7)
			die("add group needs type G", "");
		grp_entry = !strcmp(sl->argv[6], "@null") ? NULL : cont_entry(sl->argv[6]);
		grp = grp_entry ? grp_entry->p : NULL;
		np = obs_properties_add_group(P, name, desc, EN(t_group, sl->argv[5]), grp);
		if (np && grp_entry)
			grp_entry->owned = true;
	} else {
		die("unknown add kind", kind);
	}
	printf("add %s %s %s\n", kind, name, np ? "ok" : "null");
	if (as)
		bind_handle(as, np);
}

static void do_set(struct script_line *sl)
{
	script_need(sl, 3);
	obs_property_t *p = pref(sl->argv[1]);
	const char *w = sl->argv[2];
	const char *a = sl->argc > 3 ? sl->argv[3] : "";
	if (!strcmp(w, "visible"))
		obs_property_set_visible(p, getb(a));
	else if (!strcmp(w, "enabled"))
		obs_property_set_enabled(p, getb(a));
	else if (!strcmp(w, "description"))
		obs_property_set_description(p, sv(a));
	else if (!strcmp(w, "long_description"))
		obs_property_set_long_description(p, sv(a));
	else if (!strcmp(w, "int_limits")) {
		script_need(sl, 6);
		obs_property_int_set_limits(p, (int)strtol(a, NULL, 0), (int)strtol(sl->argv[4], NULL, 0),
					    (int)strtol(sl->argv[5], NULL, 0));
	} else if (!strcmp(w, "float_limits")) {
		script_need(sl, 6);
		obs_property_float_set_limits(p, strtod(a, NULL), strtod(sl->argv[4], NULL), strtod(sl->argv[5], NULL));
	} else if (!strcmp(w, "int_suffix"))
		obs_property_int_set_suffix(p, sv(a));
	else if (!strcmp(w, "float_suffix"))
		obs_property_float_set_suffix(p, sv(a));
	else if (!strcmp(w, "text_monospace"))
		obs_property_text_set_monospace(p, getb(a));
	else if (!strcmp(w, "text_info_type"))
		obs_property_text_set_info_type(p, EN(t_info, a));
	else if (!strcmp(w, "text_word_wrap"))
		obs_property_text_set_info_word_wrap(p, getb(a));
	else if (!strcmp(w, "button_type"))
		obs_property_button_set_type(p, EN(t_btn, a));
	else if (!strcmp(w, "button_url")) {
		char *u = (char *)sv(a);
		obs_property_button_set_url(p, u);
	} else if (!strcmp(w, "modified")) {
		struct behav *b = beh(a);
		if (p) {
			bind_get(p, true)->mod = b;
			obs_property_set_modified_callback(p, b ? mod_entry : NULL);
		} else {
			obs_property_set_modified_callback(p, b ? mod_entry : NULL);
		}
	} else if (!strcmp(w, "modified2")) {
		script_need(sl, 5);
		struct behav *b = beh(a);
		struct token *t = tok(sl->argv[4]);
		if (p)
			bind_get(p, true)->mod2 = b;
		obs_property_set_modified_callback2(p, b ? mod2_entry : NULL, t);
	} else if (!strcmp(w, "click")) {
		/* attach a click behaviour to an existing button (runner-side only) */
		if (p)
			bind_get(p, true)->click = beh(a);
	} else
		die("unknown set", w);
}

static void do_list(struct script_line *sl)
{
	const char *c = sl->argv[0];
	script_need(sl, 2);
	obs_property_t *p = pref(sl->argv[1]);
	if (!strcmp(c, "list_clear")) {
		obs_property_list_clear(p);
	} else if (!strcmp(c, "list_remove")) {
		script_need(sl, 3);
		obs_property_list_item_remove(p, gs(sl->argv[2]));
	} else if (!strcmp(c, "list_disable")) {
		script_need(sl, 4);
		obs_property_list_item_disable(p, gs(sl->argv[2]), getb(sl->argv[3]));
	} else if (!strcmp(c, "list_item")) {
		script_need(sl, 3);
		size_t i = gs(sl->argv[2]);
		printf("list_item %zu name=", i);
		ps(obs_property_list_item_name(p, i));
		printf(" disabled=%d str=", obs_property_list_item_disabled(p, i));
		ps(obs_property_list_item_string(p, i));
		printf(" int=%lld float=%.17g bool=%d\n", obs_property_list_item_int(p, i),
		       obs_property_list_item_float(p, i), obs_property_list_item_bool(p, i));
	} else if (!strcmp(c, "list_add")) {
		script_need(sl, 5);
		const char *f = sl->argv[2], *nm = sv(sl->argv[3]), *v = sl->argv[4];
		size_t r = 0;
		if (!strcmp(f, "string"))
			r = obs_property_list_add_string(p, nm, sv(v));
		else if (!strcmp(f, "int"))
			r = obs_property_list_add_int(p, nm, strtoll(v, NULL, 0));
		else if (!strcmp(f, "float"))
			r = obs_property_list_add_float(p, nm, strtod(v, NULL));
		else if (!strcmp(f, "bool"))
			r = obs_property_list_add_bool(p, nm, getb(v));
		else
			die("bad list format", f);
		printf("list_add %s -> %zu\n", f, r);
	} else if (!strcmp(c, "list_insert")) {
		script_need(sl, 6);
		const char *f = sl->argv[2], *nm = sv(sl->argv[4]), *v = sl->argv[5];
		size_t idx = gs(sl->argv[3]);
		if (!strcmp(f, "string"))
			obs_property_list_insert_string(p, idx, nm, sv(v));
		else if (!strcmp(f, "int"))
			obs_property_list_insert_int(p, idx, nm, strtoll(v, NULL, 0));
		else if (!strcmp(f, "float"))
			obs_property_list_insert_float(p, idx, nm, strtod(v, NULL));
		else if (!strcmp(f, "bool"))
			obs_property_list_insert_bool(p, idx, nm, getb(v));
		else
			die("bad list format", f);
	}
}

static void do_fr(struct script_line *sl)
{
	const char *c = sl->argv[0];
	script_need(sl, 2);
	obs_property_t *p = pref(sl->argv[1]);
	if (!strcmp(c, "fr_clear"))
		obs_property_frame_rate_clear(p);
	else if (!strcmp(c, "fr_options_clear"))
		obs_property_frame_rate_options_clear(p);
	else if (!strcmp(c, "fr_ranges_clear"))
		obs_property_frame_rate_fps_ranges_clear(p);
	else if (!strcmp(c, "fr_option_add")) {
		script_need(sl, 4);
		size_t r = obs_property_frame_rate_option_add(p, sv(sl->argv[2]), sv(sl->argv[3]));
		if (r == DARRAY_INVALID)
			puts("fr_option_add -> invalid");
		else
			printf("fr_option_add -> %zu\n", r);
	} else if (!strcmp(c, "fr_option_insert")) {
		script_need(sl, 5);
		obs_property_frame_rate_option_insert(p, gs(sl->argv[2]), sv(sl->argv[3]), sv(sl->argv[4]));
	} else if (!strcmp(c, "fr_range_add")) {
		script_need(sl, 6);
		size_t r = obs_property_frame_rate_fps_range_add(p, fps(sl->argv[2], sl->argv[3]),
								 fps(sl->argv[4], sl->argv[5]));
		if (r == DARRAY_INVALID)
			puts("fr_range_add -> invalid");
		else
			printf("fr_range_add -> %zu\n", r);
	} else if (!strcmp(c, "fr_range_insert")) {
		script_need(sl, 7);
		obs_property_frame_rate_fps_range_insert(p, gs(sl->argv[2]), fps(sl->argv[3], sl->argv[4]),
							 fps(sl->argv[5], sl->argv[6]));
	}
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;

	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		if (!strcmp(c, "echo")) {
			printf("echo");
			for (int i = 1; i < sl.argc; i++)
				printf(" %s", sl.argv[i]);
			putchar('\n');
		} else if (!strcmp(c, "token")) {
			script_need(&sl, 2);
			for (int i = 0; i < MAXN; i++)
				if (!tokens[i].used) {
					tokens[i].used = true;
					snprintf(tokens[i].label, 32, "%s", sl.argv[1]);
					break;
				}
		} else if (!strcmp(c, "behavior")) {
			script_need(&sl, 2);
			for (int i = 0; i < MAXN; i++)
				if (!behs[i].used) {
					behs[i].used = true;
					snprintf(behs[i].label, 32, "%s", sl.argv[1]);
					for (int k = 2; k < sl.argc && behs[i].n < MAXACT; k++)
						behs[i].act[behs[i].n++] = strdup(sl.argv[k]);
					break;
				}
		} else if (!strcmp(c, "props")) {
			script_need(&sl, 2);
			for (int i = 0; i < MAXN; i++)
				if (!conts[i].used) {
					conts[i].used = true;
					snprintf(conts[i].name, 32, "%s", sl.argv[1]);
					if (sl.argc > 2) {
						struct token *t = tok(sl.argv[2]);
						bool d = sl.argc > 3 ? !strcmp(sl.argv[3], "destroy") : true;
						conts[i].p = obs_properties_create_param(t, d ? param_destroy : NULL);
					} else {
						conts[i].p = obs_properties_create();
					}
					break;
				}
		} else if (!strcmp(c, "destroy")) {
			script_need(&sl, 2);
			struct cont *e = cont_entry(sl.argv[1]);
			if (e->dead || e->owned)
				die("cannot destroy", sl.argv[1]);
			unbind_tree(e->p);
			obs_properties_destroy(e->p);
			e->dead = true;
		} else if (!strcmp(c, "set_param")) {
			script_need(&sl, 4);
			struct token *t = tok(sl.argv[2]);
			obs_properties_set_param(cref(sl.argv[1]), t, !strcmp(sl.argv[3], "destroy") ? param_destroy : NULL);
		} else if (!strcmp(c, "get_param")) {
			script_need(&sl, 2);
			struct token *t = obs_properties_get_param(cref(sl.argv[1]));
			printf("get_param %s\n", t ? t->label : "@null");
		} else if (!strcmp(c, "flags")) {
			script_need(&sl, 3);
			obs_properties_set_flags(cref(sl.argv[1]), (uint32_t)strtoul(sl.argv[2], NULL, 0));
		} else if (!strcmp(c, "get_flags")) {
			script_need(&sl, 2);
			printf("get_flags %u\n", obs_properties_get_flags(cref(sl.argv[1])));
		} else if (!strcmp(c, "first")) {
			script_need(&sl, 2);
			obs_property_t *p = obs_properties_first(cref(sl.argv[1]));
			printf("first %s\n", p ? obs_property_name(p) : "@null");
			take_as(&sl, p, true);
		} else if (!strcmp(c, "get")) {
			script_need(&sl, 3);
			obs_property_t *p = obs_properties_get(cref(sl.argv[1]), sl.argv[2]);
			printf("get %s %s\n", sl.argv[2], p ? "found" : "null");
			take_as(&sl, p, true);
		} else if (!strcmp(c, "next")) {
			script_need(&sl, 2);
			obs_property_t *p = pref(sl.argv[1]);
			bool r = obs_property_next(&p);
			printf("next -> %d %s\n", r, p ? obs_property_name(p) : "@null");
			char *as;
			peel_as(&sl, &as);
			bind_handle(as ? as : sl.argv[1], p);
		} else if (!strcmp(c, "parent")) {
			script_need(&sl, 2);
			printf("parent %s\n", cont_name(obs_properties_get_parent(cref(sl.argv[1]))));
		} else if (!strcmp(c, "remove")) {
			script_need(&sl, 3);
			obs_properties_t *P = cref(sl.argv[1]);
			if (P) {
				obs_property_t *p = obs_properties_get(P, sl.argv[2]);
				if (p) {
					struct bind *b = bind_get(p, false);
					if (b)
						b->used = false;
					for (int i = 0; i < MAXN; i++)
						if (hands[i].used && hands[i].p == p)
							hands[i].used = false;
					if (obs_property_get_type(p) == OBS_PROPERTY_GROUP) {
						obs_properties_t *g = obs_property_group_content(p);
						unbind_tree(g);
						for (int i = 0; i < MAXN; i++)
							if (conts[i].used && conts[i].p == g)
								conts[i].dead = true;
					}
				}
			}
			obs_properties_remove_by_name(P, sl.argv[2]);
		} else if (!strcmp(c, "apply")) {
			script_need(&sl, 3);
			printf("apply %s\n", sl.argv[1]);
			obs_properties_apply_settings(cref(sl.argv[1]), dref(sl.argv[2]));
			puts("apply-done");
		} else if (!strcmp(c, "data")) {
			script_need(&sl, 2);
			for (int i = 0; i < MAXN; i++)
				if (!dats[i].used) {
					dats[i].used = true;
					snprintf(dats[i].name, 32, "%s", sl.argv[1]);
					dats[i].d = obs_data_create();
					break;
				}
		} else if (!strcmp(c, "data_set")) {
			script_need(&sl, 5);
			obs_data_t *d = dref(sl.argv[1]);
			const char *k = sl.argv[2], *t = sl.argv[3], *v = sl.argv[4];
			if (!strcmp(t, "int"))
				obs_data_set_int(d, k, strtoll(v, NULL, 0));
			else if (!strcmp(t, "str"))
				obs_data_set_string(d, k, v);
			else if (!strcmp(t, "bool"))
				obs_data_set_bool(d, k, getb(v));
			else if (!strcmp(t, "dbl"))
				obs_data_set_double(d, k, strtod(v, NULL));
			else
				die("bad data type", t);
		} else if (!strcmp(c, "data_show")) {
			script_need(&sl, 2);
			printf("data %s ", sl.argv[1]);
			print_settings(dref(sl.argv[1]));
			putchar('\n');
		} else if (!strcmp(c, "data_free")) {
			script_need(&sl, 2);
			struct dat *e = dat_entry(sl.argv[1]);
			obs_data_release(e->d);
			e->used = false;
		} else if (!strcmp(c, "obj")) {
			script_need(&sl, 2);
			for (int i = 0; i < MAXN; i++)
				if (!objs[i].used) {
					memset(&objs[i], 0, sizeof(objs[i]));
					objs[i].used = true;
					snprintf(objs[i].name, 32, "%s", sl.argv[1]);
					objs[i].ctx.data = sl.argc > 2 ? tok(sl.argv[2]) : NULL;
					break;
				}
		} else if (!strcmp(c, "add")) {
			do_add(&sl);
		} else if (!strcmp(c, "set")) {
			do_set(&sl);
		} else if (!strcmp(c, "info")) {
			script_need(&sl, 2);
			printf("info\n");
			print_prop(pref(sl.argv[1]), 2, false);
		} else if (!strcmp(c, "probe")) {
			script_need(&sl, 2);
			probe(pref(sl.argv[1]));
		} else if (!strcmp(c, "dump")) {
			script_need(&sl, 2);
			obs_properties_t *P = cref(sl.argv[1]);
			printf("dump %s flags=%u param=", sl.argv[1], obs_properties_get_flags(P));
			struct token *t = obs_properties_get_param(P);
			printf("%s\n", t ? t->label : "@null");
			for (obs_property_t *p = obs_properties_first(P); p; obs_property_next(&p))
				print_prop(p, 2, true);
		} else if (!strncmp(c, "list_", 5)) {
			do_list(&sl);
		} else if (!strncmp(c, "fr_", 3)) {
			do_fr(&sl);
		} else if (!strcmp(c, "modified")) {
			script_need(&sl, 3);
			obs_property_t *p = pref(sl.argv[1]);
			bool r = obs_property_modified(p, dref(sl.argv[2]));
			printf("modified -> %d\n", r);
		} else if (!strcmp(c, "click")) {
			script_need(&sl, 3);
			obs_property_t *p = pref(sl.argv[1]);
			void *o = !strcmp(sl.argv[2], "@null") ? NULL : &obj_entry(sl.argv[2])->ctx;
			bool r = obs_property_button_clicked(p, o);
			printf("click -> %d\n", r);
		} else {
			die("unknown command", c);
		}
	}

	/* silent cleanup so that sanitizer runs stay leak-free */
	quiet = true;
	for (int i = 0; i < MAXN; i++)
		if (conts[i].used && !conts[i].dead && !conts[i].owned) {
			obs_properties_destroy(conts[i].p);
			conts[i].dead = true;
		}
	for (int i = 0; i < MAXN; i++)
		if (dats[i].used)
			obs_data_release(dats[i].d);
	fclose(f);
	return 0;
}
