/*
 * Conformance runner for the hotkey system (spec S-034). Command list: section
 * "hotkeys runner" in test/conformance/README.md.
 *
 * The runner #includes libobs/obs-hotkey.c so that the file-local polling step
 * (query_hotkeys, run by the 25 ms hotkey thread in the product) can be called
 * synchronously with a scripted fake platform backend (set of "pressed" keys).
 * The real obs-data.c and calldata.c are linked; the signal handler, the weak
 * reference functions and the profiler are recording fakes (gen/hotkeys_stubs.c).
 */
#include <inttypes.h>
#include <pthread.h>
#include "harness.h"
#include "obs-internal.h"
#include "obs-data.h"
#include "obs-hotkey.c"

/* ---- fakes exported by hotkeys_stubs.c ---- */
extern int fake_weak_live;
extern int fake_weak_null_releases;
extern bool fake_pressed[];

#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

static void die(const char *msg, const char *what)
{
	fprintf(stderr, "script error: %s %s\n", msg, what ? what : "");
	exit(2);
}

/* ---- name tables ---- */
#define MAXN 64
struct named {
	char name[40];
	size_t val;
	bool used;
};
static struct named hk_tbl[MAXN], pair_tbl[MAXN];

static void put(struct named *t, const char *name, size_t v)
{
	for (int i = 0; i < MAXN; i++)
		if (t[i].used && !strcmp(t[i].name, name)) {
			t[i].val = v;
			return;
		}
	for (int i = 0; i < MAXN; i++)
		if (!t[i].used) {
			snprintf(t[i].name, sizeof t[i].name, "%s", name);
			t[i].val = v;
			t[i].used = true;
			return;
		}
	die("table full", name);
}

/* "id:N" = literal numeric id, INVALID = the invalid id, otherwise a registered handle */
static size_t get(struct named *t, const char *name)
{
	if (!strncmp(name, "id:", 3))
		return (size_t)strtoull(name + 3, NULL, 0);
	if (!strcmp(name, "INVALID"))
		return (size_t)-1;
	for (int i = 0; i < MAXN; i++)
		if (t[i].used && !strcmp(t[i].name, name))
			return t[i].val;
	die("unknown handle", name);
	return 0;
}

static void print_id(size_t id)
{
	if (id == (size_t)-1)
		fputs("INVALID", stdout);
	else
		printf("%zu", id);
}

/* ---- callbacks ---- */
struct cb {
	char label[40];
	bool reenter;
	bool ret;
	bool quiet; /* print only the label and the pressed flag (used where the id argument is undefined) */
	struct cb *next;
};
static struct cb *cbs;

static struct cb *cb_for(const char *label)
{
	for (struct cb *c = cbs; c; c = c->next)
		if (!strcmp(c->label, label))
			return c;
	struct cb *c = calloc(1, sizeof *c);
	snprintf(c->label, sizeof c->label, "%s", label);
	c->ret = true;
	c->next = cbs;
	cbs = c;
	return c;
}

static void cb_reenter(struct cb *c, obs_hotkey_id id)
{
	if (!c->reenter)
		return;
	/* callbacks run with the (recursive) hotkey lock held; these calls must not deadlock */
	obs_data_array_t *a = obs_hotkey_save(id);
	printf("  reenter save count=%zu\n", obs_data_array_count(a));
	obs_data_array_release(a);
	obs_hotkey_set_description(id, "touched");
}

static void hk_func(void *data, obs_hotkey_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct cb *c = data;
	if (c->quiet) {
		printf("cb %s pressed=%d\n", c->label, pressed);
		return;
	}
	printf("cb %s id=", c->label);
	print_id(id);
	printf(" same_ptr=%d pressed=%d\n", obs_hotkey_get_id(hotkey) == id, pressed);
	cb_reenter(c, id);
}

static bool pair_func(void *data, obs_hotkey_pair_id id, obs_hotkey_t *hotkey, bool pressed)
{
	struct cb *c = data;
	printf("pcb %s pair=", c->label);
	print_id(id);
	printf(" hk=");
	print_id(obs_hotkey_get_id(hotkey));
	printf(" pressed=%d -> %d\n", pressed, c->ret);
	return c->ret;
}

static void router(void *data, obs_hotkey_id id, bool pressed)
{
	(void)data;
	printf("route id=");
	print_id(id);
	printf(" pressed=%d\n", pressed);
}

static void atomic_fn(void *data)
{
	printf("atomic %s\n", (const char *)data);
}

/* ---- contexts ---- */
struct ctx {
	char name[40];
	int kind; /* 0 enc 1 out 2 svc 3 src 4 private src */
	void *obj;
	bool used;
};
static struct ctx ctxs[MAXN];

static struct ctx *ctx_find(const char *name)
{
	for (int i = 0; i < MAXN; i++)
		if (ctxs[i].used && !strcmp(ctxs[i].name, name))
			return &ctxs[i];
	die("unknown context", name);
	return NULL;
}

static struct obs_context_data *ctx_data(struct ctx *c)
{
	switch (c->kind) {
	case 0:
		return &((struct obs_encoder *)c->obj)->context;
	case 1:
		return &((struct obs_output *)c->obj)->context;
	case 2:
		return &((struct obs_service *)c->obj)->context;
	default:
		return &((struct obs_source *)c->obj)->context;
	}
}

static int kind_of(const char *s)
{
	if (!strcmp(s, "encoder"))
		return 0;
	if (!strcmp(s, "output"))
		return 1;
	if (!strcmp(s, "service"))
		return 2;
	if (!strcmp(s, "source"))
		return 3;
	if (!strcmp(s, "private-source"))
		return 4;
	if (!strcmp(s, "frontend"))
		return -1;
	die("bad kind", s);
	return 0;
}

/* ---- helpers for combos / json ---- */
static obs_key_combination_t parse_combo(const char *s)
{
	obs_key_combination_t k;
	char *end;
	k.modifiers = (uint32_t)strtoul(s, &end, 0);
	if (*end != ':')
		die("bad combo (MODS:KEYNAME)", s);
	k.key = obs_key_from_name(end + 1);
	return k;
}

/* Parses a JSON array text into an obs_data_array (kept alive through the wrapper). */
static obs_data_array_t *parse_array(const char *json, obs_data_t **holder)
{
	if (!strcmp(json, "-")) {
		*holder = NULL;
		return NULL;
	}
	char buf[4096];
	snprintf(buf, sizeof buf, "{\"a\":%s}", json);
	*holder = obs_data_create_from_json(buf);
	if (!*holder)
		die("bad json", json);
	return obs_data_get_array(*holder, "a");
}

static void print_array(const char *tag, obs_data_array_t *a)
{
	if (!a) {
		printf("%s null\n", tag);
		return;
	}
	size_t n = obs_data_array_count(a);
	printf("%s count=%zu\n", tag, n);
	for (size_t i = 0; i < n; i++) {
		obs_data_t *it = obs_data_array_item(a, i);
		const char *j = obs_data_get_json(it);
		printf("  item %s\n", j);
		obs_data_release(it);
	}
}

static bool on_off(const char *s)
{
	return !strcmp(s, "on") || !strcmp(s, "1");
}

static int enum_limit;
static bool enum_cb(void *d, obs_hotkey_id id, obs_hotkey_t *hk)
{
	(void)d;
	printf("h id=");
	print_id(id);
	printf(" name=");
	script_print_quoted(obs_hotkey_get_name(hk));
	printf("\n");
	return --enum_limit != 0;
}

static bool enumb_cb(void *d, size_t idx, obs_hotkey_binding_t *b)
{
	(void)d;
	printf("b %zu hk=", idx);
	print_id(obs_hotkey_binding_get_hotkey_id(b));
	obs_key_combination_t k = obs_hotkey_binding_get_key_combination(b);
	printf(" mods=0x%x key=%s pressed=%d modmatch=%d hkptr_ok=%d\n", k.modifiers, obs_key_to_name(k.key),
	       b->pressed, b->modifiers_match, obs_hotkey_binding_get_hotkey(b) != NULL);
	return --enum_limit != 0;
}

static struct obs_core *core;
static int dummy_signals;

static void core_new(void)
{
	core = bzalloc(sizeof *core);
	pthread_mutex_init_recursive(&core->hotkeys.mutex);
	core->hotkeys.signals = (signal_handler_t *)&dummy_signals;
	core->hotkeys.strict_modifiers = true; /* the product default */
	obs = core;
}

static void field_set(struct obs_hotkeys_translations *t, const char *f, const char *v)
{
#define F(n)                    \
	if (!strcmp(f, #n)) {   \
		t->n = v;       \
		return;         \
	}
	F(insert) F(del) F(home) F(end) F(page_up) F(page_down) F(num_lock) F(scroll_lock) F(caps_lock) F(backspace)
		F(tab) F(print) F(pause) F(left) F(right) F(up) F(down) F(shift) F(alt) F(control) F(meta) F(menu)
			F(space) F(numpad_num) F(numpad_divide) F(numpad_multiply) F(numpad_minus) F(numpad_plus)
				F(numpad_decimal) F(mouse_num) F(escape)
#undef F
					die("bad translation field", f);
}

static void show_str(const char *label, const char *s)
{
	printf("%s=", label);
	if (s)
		script_print_quoted(s);
	else
		fputs("null", stdout);
	printf("\n");
}

static void print_info(const char *h)
{
	size_t id = get(hk_tbl, h);
	obs_hotkey_t *hk;
	HASH_FIND_HKEY(obs->hotkeys.hotkeys, id, hk);
	if (!hk) {
		printf("info %s missing\n", h);
		return;
	}
	printf("info %s id=", h);
	print_id(obs_hotkey_get_id(hk));
	printf(" type=%d partner=", obs_hotkey_get_registerer_type(hk));
	print_id(obs_hotkey_get_pair_partner_id(hk));
	printf(" registerer=%s pressed_count=%d\n", obs_hotkey_get_registerer(hk) ? "weak" : "none", hk->pressed);
	printf("  ");
	show_str("name", obs_hotkey_get_name(hk));
	printf("  ");
	show_str("desc", obs_hotkey_get_description(hk));
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	core_new();

	while (script_next(f, &sl, &lineno)) {
		char **a = sl.argv;
		const char *cmd = a[0];

		if (!strcmp(cmd, "core")) {
			script_need(&sl, 2);
			if (!strcmp(a[1], "new"))
				core_new();
			else if (!strcmp(a[1], "off"))
				obs = NULL;
			else if (!strcmp(a[1], "on"))
				obs = core;
			else
				die("core new|on|off", a[1]);
		} else if (!strcmp(cmd, "ctx")) { /* ctx KIND NAME [JSON|-] */
			script_need(&sl, 3);
			int k = kind_of(a[1]);
			struct ctx *c = NULL;
			for (int i = 0; i < MAXN && !c; i++)
				if (!ctxs[i].used)
					c = &ctxs[i];
			snprintf(c->name, sizeof c->name, "%s", a[2]);
			c->kind = k;
			c->used = true;
			switch (k) {
			case 0:
				c->obj = bzalloc(sizeof(struct obs_encoder));
				break;
			case 1:
				c->obj = bzalloc(sizeof(struct obs_output));
				break;
			case 2:
				c->obj = bzalloc(sizeof(struct obs_service));
				break;
			default:
				c->obj = bzalloc(sizeof(struct obs_source));
				break;
			}
			if (k == 4)
				((struct obs_source *)c->obj)->context.private = true;
			if (sl.argc > 3 && strcmp(a[3], "-"))
				ctx_data(c)->hotkey_data = obs_data_create_from_json(a[3]);
		} else if (!strcmp(cmd, "reg")) { /* reg KIND [CTX|-] H NAME DESC|- */
			script_need(&sl, 2);
			int k = kind_of(a[1]);
			int o = k < 0 ? 3 : 4; /* index of NAME */
			script_need(&sl, o + 2);
			const char *h = a[o - 1];
			void *obj = NULL;
			if (k >= 0 && strcmp(a[2], "-"))
				obj = ctx_find(a[2])->obj;
			const char *desc = strcmp(a[o + 1], "-") ? a[o + 1] : NULL;
			struct cb *c = cb_for(h);
			obs_hotkey_id id;
			switch (k) {
			case -1:
				id = obs_hotkey_register_frontend(a[o], desc, hk_func, c);
				break;
			case 0:
				id = obs_hotkey_register_encoder(obj, a[o], desc, hk_func, c);
				break;
			case 1:
				id = obs_hotkey_register_output(obj, a[o], desc, hk_func, c);
				break;
			case 2:
				id = obs_hotkey_register_service(obj, a[o], desc, hk_func, c);
				break;
			default:
				id = obs_hotkey_register_source(obj, a[o], desc, hk_func, c);
				break;
			}
			put(hk_tbl, h, id);
			printf("reg %s -> ", h);
			print_id(id);
			printf("\n");
		} else if (!strcmp(cmd, "regp")) { /* regp KIND [CTX|-] P N0 D0 N1 D1 : handles P.a P.b */
			script_need(&sl, 2);
			int k = kind_of(a[1]);
			int o = k < 0 ? 2 : 3;
			script_need(&sl, o + 5);
			const char *p = a[o];
			void *obj = NULL;
			if (k >= 0 && strcmp(a[2], "-"))
				obj = ctx_find(a[2])->obj;
			char la[48], lb[48];
			snprintf(la, sizeof la, "%s.a", p);
			snprintf(lb, sizeof lb, "%s.b", p);
			struct cb *c0 = cb_for(la), *c1 = cb_for(lb);
			const char *d0 = strcmp(a[o + 2], "-") ? a[o + 2] : NULL;
			const char *d1 = strcmp(a[o + 4], "-") ? a[o + 4] : NULL;
			obs_hotkey_pair_id id;
			switch (k) {
			case -1:
				id = obs_hotkey_pair_register_frontend(a[o + 1], d0, a[o + 3], d1, pair_func, pair_func,
								       c0, c1);
				break;
			case 0:
				id = obs_hotkey_pair_register_encoder(obj, a[o + 1], d0, a[o + 3], d1, pair_func,
								      pair_func, c0, c1);
				break;
			case 1:
				id = obs_hotkey_pair_register_output(obj, a[o + 1], d0, a[o + 3], d1, pair_func,
								     pair_func, c0, c1);
				break;
			case 2:
				id = obs_hotkey_pair_register_service(obj, a[o + 1], d0, a[o + 3], d1, pair_func,
								      pair_func, c0, c1);
				break;
			default:
				id = obs_hotkey_pair_register_source(obj, a[o + 1], d0, a[o + 3], d1, pair_func,
								     pair_func, c0, c1);
				break;
			}
			put(pair_tbl, p, id);
			printf("regp %s -> pair=", p);
			print_id(id);
			if (id != OBS_INVALID_HOTKEY_PAIR_ID) {
				obs_hotkey_pair_t *pr;
				HASH_FIND_HKEY(obs->hotkeys.hotkey_pairs, id, pr);
				put(hk_tbl, la, pr->id[0]);
				put(hk_tbl, lb, pr->id[1]);
				printf(" a=");
				print_id(pr->id[0]);
				printf(" b=");
				print_id(pr->id[1]);
			}
			printf("\n");
		} else if (!strcmp(cmd, "pairret")) { /* pairret LABEL 0|1 */
			script_need(&sl, 3);
			cb_for(a[1])->ret = on_off(a[2]);
		} else if (!strcmp(cmd, "reenter")) { /* reenter LABEL on|off */
			script_need(&sl, 3);
			cb_for(a[1])->reenter = on_off(a[2]);
		} else if (!strcmp(cmd, "quiet")) { /* quiet LABEL on|off */
			script_need(&sl, 3);
			cb_for(a[1])->quiet = on_off(a[2]);
		} else if (!strcmp(cmd, "info")) {
			script_need(&sl, 2);
			print_info(a[1]);
		} else if (!strcmp(cmd, "setname")) {
			script_need(&sl, 3);
			obs_hotkey_set_name(get(hk_tbl, a[1]), a[2]);
		} else if (!strcmp(cmd, "setdesc")) {
			script_need(&sl, 3);
			obs_hotkey_set_description(get(hk_tbl, a[1]), a[2]);
		} else if (!strcmp(cmd, "pairnames")) {
			script_need(&sl, 4);
			obs_hotkey_pair_set_names(get(pair_tbl, a[1]), a[2], a[3]);
		} else if (!strcmp(cmd, "pairdescs")) {
			script_need(&sl, 4);
			obs_hotkey_pair_set_descriptions(get(pair_tbl, a[1]), a[2], a[3]);
		} else if (!strcmp(cmd, "unreg")) {
			script_need(&sl, 2);
			obs_hotkey_unregister(get(hk_tbl, a[1]));
		} else if (!strcmp(cmd, "unregp")) {
			script_need(&sl, 2);
			obs_hotkey_pair_unregister(get(pair_tbl, a[1]));
		} else if (!strcmp(cmd, "release")) { /* release CTX */
			script_need(&sl, 2);
			struct ctx *c = ctx_find(a[1]);
			obs_hotkeys_context_release(ctx_data(c));
			memset(ctx_data(c), 0, sizeof(struct obs_context_data));
		} else if (!strcmp(cmd, "loadbind")) { /* loadbind H MODS:KEY ... */
			script_need(&sl, 2);
			obs_key_combination_t ks[16];
			int n = 0;
			for (int i = 2; i < sl.argc && n < 16; i++)
				ks[n++] = parse_combo(a[i]);
			obs_hotkey_load_bindings(get(hk_tbl, a[1]), n ? ks : NULL, (size_t)n);
		} else if (!strcmp(cmd, "load")) { /* load H JSONARRAY|- */
			script_need(&sl, 3);
			obs_data_t *h;
			obs_data_array_t *arr = parse_array(a[2], &h);
			obs_hotkey_load(get(hk_tbl, a[1]), arr);
			obs_data_array_release(arr);
			obs_data_release(h);
		} else if (!strcmp(cmd, "loadpair")) { /* loadpair P JSON0|- JSON1|- */
			script_need(&sl, 4);
			obs_data_t *h0, *h1;
			obs_data_array_t *a0 = parse_array(a[2], &h0), *a1 = parse_array(a[3], &h1);
			obs_hotkey_pair_load(get(pair_tbl, a[1]), a0, a1);
			obs_data_array_release(a0);
			obs_data_array_release(a1);
			obs_data_release(h0);
			obs_data_release(h1);
		} else if (!strcmp(cmd, "ctxload")) { /* ctxload CTX JSON|- */
			script_need(&sl, 3);
			struct ctx *c = ctx_find(a[1]);
			obs_data_t *d = strcmp(a[2], "-") ? obs_data_create_from_json(a[2]) : NULL;
			switch (c->kind) {
			case 0:
				obs_hotkeys_load_encoder(c->obj, d);
				break;
			case 1:
				obs_hotkeys_load_output(c->obj, d);
				break;
			case 2:
				obs_hotkeys_load_service(c->obj, d);
				break;
			default:
				obs_hotkeys_load_source(c->obj, d);
				break;
			}
			obs_data_release(d);
		} else if (!strcmp(cmd, "ctxsave")) {
			script_need(&sl, 2);
			struct ctx *c = ctx_find(a[1]);
			obs_data_t *d;
			switch (c->kind) {
			case 0:
				d = obs_hotkeys_save_encoder(c->obj);
				break;
			case 1:
				d = obs_hotkeys_save_output(c->obj);
				break;
			case 2:
				d = obs_hotkeys_save_service(c->obj);
				break;
			default:
				d = obs_hotkeys_save_source(c->obj);
				break;
			}
			if (d)
				printf("ctxsave %s %s\n", a[1], obs_data_get_json(d));
			else
				printf("ctxsave %s null\n", a[1]);
			obs_data_release(d);
		} else if (!strcmp(cmd, "save")) {
			script_need(&sl, 2);
			obs_data_array_t *arr = obs_hotkey_save(get(hk_tbl, a[1]));
			char tag[64];
			snprintf(tag, sizeof tag, "save %s", a[1]);
			print_array(tag, arr);
			obs_data_array_release(arr);
		} else if (!strcmp(cmd, "savepair")) { /* savepair P both|0|1|none */
			script_need(&sl, 3);
			obs_data_array_t *d0 = NULL, *d1 = NULL;
			bool w0 = !strcmp(a[2], "both") || !strcmp(a[2], "0");
			bool w1 = !strcmp(a[2], "both") || !strcmp(a[2], "1");
			obs_hotkey_pair_save(get(pair_tbl, a[1]), w0 ? &d0 : NULL, w1 ? &d1 : NULL);
			if (w0)
				print_array("savepair 0", d0);
			if (w1)
				print_array("savepair 1", d1);
			if (!w0 && !w1)
				printf("savepair none\n");
			obs_data_array_release(d0);
			obs_data_array_release(d1);
		} else if (!strcmp(cmd, "enum")) { /* enum [LIMIT] */
			enum_limit = sl.argc > 1 ? atoi(a[1]) : 0;
			obs_enum_hotkeys(enum_cb, NULL);
			printf("enum done\n");
		} else if (!strcmp(cmd, "enumb")) {
			enum_limit = sl.argc > 1 ? atoi(a[1]) : 0;
			obs_enum_hotkey_bindings(enumb_cb, NULL);
			printf("enumb done\n");
		} else if (!strcmp(cmd, "down") || !strcmp(cmd, "up")) {
			bool v = !strcmp(cmd, "down");
			for (int i = 1; i < sl.argc; i++) {
				obs_key_t k = obs_key_from_name(a[i]);
				fake_pressed[k] = v;
			}
		} else if (!strcmp(cmd, "upall")) {
			for (int i = 0; i < OBS_KEY_LAST_VALUE; i++)
				fake_pressed[i] = false;
		} else if (!strcmp(cmd, "poll")) { /* poll [N] : N polling ticks */
			int n = sl.argc > 1 ? atoi(a[1]) : 1;
			for (int i = 0; i < n; i++) {
				printf("poll\n");
				if (lock()) {
					query_hotkeys();
					unlock();
				}
			}
		} else if (!strcmp(cmd, "inject")) { /* inject MODS KEYNAME 0|1 */
			script_need(&sl, 4);
			obs_key_combination_t k;
			k.modifiers = (uint32_t)strtoul(a[1], NULL, 0);
			k.key = obs_key_from_name(a[2]);
			obs_hotkey_inject_event(k, on_off(a[3]));
		} else if (!strcmp(cmd, "strict")) {
			script_need(&sl, 2);
			obs->hotkeys.strict_modifiers = on_off(a[1]);
		} else if (!strcmp(cmd, "bgpress")) {
			script_need(&sl, 2);
			obs_hotkey_enable_background_press(on_off(a[1]));
			printf("bgpress thread_disable_press=%d\n", obs ? obs->hotkeys.thread_disable_press : -1);
		} else if (!strcmp(cmd, "reroute")) {
			script_need(&sl, 2);
			obs_hotkey_enable_callback_rerouting(on_off(a[1]));
		} else if (!strcmp(cmd, "router")) {
			script_need(&sl, 2);
			obs_hotkey_set_callback_routing_func(on_off(a[1]) ? router : NULL, NULL);
		} else if (!strcmp(cmd, "routed")) { /* routed H 0|1 */
			script_need(&sl, 3);
			obs_hotkey_trigger_routed_callback(get(hk_tbl, a[1]), on_off(a[2]));
		} else if (!strcmp(cmd, "atomic")) {
			obs_hotkey_update_atomic(atomic_fn, "ran");
		} else if (!strcmp(cmd, "setnext")) {
			script_need(&sl, 2);
			obs->hotkeys.next_id = get(hk_tbl, a[1]);
		} else if (!strcmp(cmd, "setnextpair")) {
			script_need(&sl, 2);
			obs->hotkeys.next_pair_id = get(pair_tbl, a[1]);
		} else if (!strcmp(cmd, "hkfree")) {
			obs_hotkeys_free();
			printf("hkfree hotkeys=%u pairs=%u bindings=%zu weak_live=%d\n",
			       HASH_COUNT(obs->hotkeys.hotkeys), HASH_COUNT(obs->hotkeys.hotkey_pairs),
			       obs->hotkeys.bindings.num, fake_weak_live);
		} else if (!strcmp(cmd, "weaks")) {
			printf("weaks live=%d null_releases=%d\n", fake_weak_live, fake_weak_null_releases);
		} else if (!strcmp(cmd, "trans")) { /* trans [size=N] FIELD=VALUE ... */
			struct obs_hotkeys_translations t;
			memset(&t, 0, sizeof t);
			size_t size = sizeof t;
			for (int i = 1; i < sl.argc; i++) {
				char *eq = strchr(a[i], '=');
				if (!eq)
					die("FIELD=VALUE", a[i]);
				*eq = 0;
				if (!strcmp(a[i], "size"))
					size = (size_t)atoi(eq + 1);
				else
					field_set(&t, a[i], eq + 1);
			}
			obs_hotkeys_set_translations_s(&t, size);
		} else if (!strcmp(cmd, "transnull")) {
			obs_hotkeys_set_translations_s(NULL, 0);
		} else if (!strcmp(cmd, "tget")) { /* tget KEYNAME DEF|- */
			script_need(&sl, 3);
			const char *r = obs_get_hotkey_translation(obs_key_from_name(a[1]), strcmp(a[2], "-") ? a[2] : NULL);
			printf("tget %s ", a[1]);
			if (r)
				script_print_quoted(r);
			else
				fputs("null", stdout);
			printf("\n");
		} else if (!strcmp(cmd, "audiotrans")) { /* 6 strings */
			script_need(&sl, 7);
			obs_hotkeys_set_audio_hotkeys_translations(a[1], a[2], a[3], a[4], a[5], a[6]);
		} else if (!strcmp(cmd, "sitrans")) {
			script_need(&sl, 3);
			obs_hotkeys_set_sceneitem_hotkeys_translations(a[1], a[2]);
		} else if (!strcmp(cmd, "showtrans")) {
			show_str("mute", obs->hotkeys.mute);
			show_str("unmute", obs->hotkeys.unmute);
			show_str("push_to_mute", obs->hotkeys.push_to_mute);
			show_str("push_to_talk", obs->hotkeys.push_to_talk);
			show_str("monitor_on", obs->hotkeys.monitor_on);
			show_str("monitor_off", obs->hotkeys.monitor_off);
			show_str("sceneitem_show", obs->hotkeys.sceneitem_show);
			show_str("sceneitem_hide", obs->hotkeys.sceneitem_hide);
		} else if (!strcmp(cmd, "echo")) {
			for (int i = 1; i < sl.argc; i++)
				printf("%s%s", i > 1 ? " " : "", a[i]);
			printf("\n");
		} else {
			die("unknown command", cmd);
		}
	}
	fclose(f);
	return 0;
}
