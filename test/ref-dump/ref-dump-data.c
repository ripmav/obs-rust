/*
 * obs-ref-dump (WP-39, P0-14.3): data dump.
 *
 * Deterministic reference output of the pinned 33.0.1 obs_data
 * (build/get/defaults/apply/erase/clear, JSON round trip over jansson
 * 2.15.1) and calldata (per-type set/get, raw data, unset lookups).
 * JSON is printed sorted + compact (jansson JSON_SORT_KEYS | JSON_COMPACT)
 * so the output is deterministic. Fixtures: test/fixtures/data-dump/ *.json files
 * (sorted by name; the run must happen from the repository root).
 */
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <jansson.h>

#include "obs-data.h"
#include "callback/calldata.h"

extern uint32_t next_u32(void);
extern void rd_reset_prng(void);

#define CASES 32

static double r01d(void)
{
	return (double)next_u32() * (1.0 / 4294967296.0);
}

static void dump_sorted_json(const char *label, const char *json)
{
	if (!json) {
		printf("%s: (null)\n", label);
		return;
	}
	json_t *t = json_loads(json, 0, NULL);
	if (!t) {
		printf("%s: (unparseable) %s\n", label, json);
		return;
	}
	char *out = json_dumps(t, JSON_SORT_KEYS | JSON_COMPACT);
	printf("%s: %s\n", label, out ? out : "(dump failed)");
	free(out);
	json_decref(t);
}

static void dump_data_sorted(const char *label, obs_data_t *data)
{
	dump_sorted_json(label, obs_data_get_json(data));
}

static void obs_ref_dump_data_build(void)
{
	printf("# data/obs_data_build\n");

	obs_data_t *root = obs_data_create();
	obs_data_set_string(root, "name", "ref-dump");
	obs_data_set_int(root, "count", 42);
	obs_data_set_double(root, "ratio", 0.5);
	obs_data_set_bool(root, "enabled", true);
	obs_data_set_string(root, "empty_str", "");
	obs_data_set_int(root, "neg_int", -123456789LL);
	obs_data_set_double(root, "big_double", 1e15);

	obs_data_t *settings = obs_data_create();
	obs_data_set_string(settings, "inner_string", "inner");
	obs_data_set_int(settings, "inner_int", 7);
	obs_data_set_double(settings, "inner_double", 1.5);
	obs_data_set_bool(settings, "inner_bool", false);
	obs_data_set_obj(root, "settings", settings);

	obs_data_array_t *items = obs_data_array_create();
	for (int i = 0; i < 3; i++) {
		obs_data_t *item = obs_data_create();
		obs_data_set_int(item, "v", i + 1);
		obs_data_array_push_back(items, item);
	}
	obs_data_set_array(root, "items", items);

	obs_data_set_default_string(root, "def_name", "d");
	obs_data_set_default_int(root, "def_count", 7);
	obs_data_set_default_double(root, "def_ratio", 1.5);
	obs_data_set_default_bool(root, "def_enabled", true);

	printf("set values:\n");
	printf("  name=%s\n", obs_data_get_string(root, "name"));
	printf("  count=%lld\n", obs_data_get_int(root, "count"));
	printf("  ratio=%.9g\n", obs_data_get_double(root, "ratio"));
	printf("  enabled=%d\n", (int)obs_data_get_bool(root, "enabled"));
	printf("  empty_str=[%s]\n", obs_data_get_string(root, "empty_str"));
	printf("  neg_int=%lld\n", obs_data_get_int(root, "neg_int"));
	printf("  big_double=%.9g\n", obs_data_get_double(root, "big_double"));

	printf("defaults (unset keys):\n");
	printf("  def_name=%s\n", obs_data_get_string(root, "def_name"));
	printf("  def_count=%lld\n", obs_data_get_int(root, "def_count"));
	printf("  def_ratio=%.9g\n", obs_data_get_double(root, "def_ratio"));
	printf("  def_enabled=%d\n", (int)obs_data_get_bool(root, "def_enabled"));
	printf("  get_default_string(def_name)=%s\n", obs_data_get_default_string(root, "def_name"));
	printf("  get_default_int(def_count)=%lld\n", obs_data_get_default_int(root, "def_count"));
	printf("  get_default_double(def_ratio)=%.9g\n", obs_data_get_default_double(root, "def_ratio"));
	printf("  get_default_bool(def_enabled)=%d\n", (int)obs_data_get_default_bool(root, "def_enabled"));
	printf("  get_default_string(name)=%s\n", obs_data_get_default_string(root, "name"));
	printf("  get_string(missing)=[%s]\n", obs_data_get_string(root, "missing"));
	printf("  get_int(missing)=%lld\n", obs_data_get_int(root, "missing"));
	printf("  get_double(missing)=%.9g\n", obs_data_get_double(root, "missing"));
	printf("  get_bool(missing)=%d\n", (int)obs_data_get_bool(root, "missing"));

	obs_data_t *got = obs_data_get_obj(root, "settings");
	printf("settings obj:\n");
	if (got) {
		printf("  inner_string=%s\n", obs_data_get_string(got, "inner_string"));
		printf("  inner_int=%lld\n", obs_data_get_int(got, "inner_int"));
		printf("  inner_double=%.9g\n", obs_data_get_double(got, "inner_double"));
		printf("  inner_bool=%d\n", (int)obs_data_get_bool(got, "inner_bool"));
		dump_data_sorted("  settings_json", got);
	} else {
		printf("  (null)\n");
	}
	obs_data_t *missing_obj = obs_data_get_obj(root, "no-such-obj");
	printf("get_obj(missing)=%p\n", (const void *)missing_obj);

	obs_data_array_t *arr = obs_data_get_array(root, "items");
	printf("items array:\n");
	if (arr) {
		size_t n = obs_data_array_count(arr);
		printf("  count=%zu\n", n);
		for (size_t i = 0; i < n; i++) {
			obs_data_t *item = obs_data_array_item(arr, i);
			printf("  item %zu v=%lld\n", i, obs_data_get_int(item, "v"));
		}
		obs_data_t *oob = obs_data_array_item(arr, 99);
		printf("  item(99)=%p\n", (const void *)oob);
	} else {
		printf("  (null)\n");
	}
	obs_data_array_t *missing_arr = obs_data_get_array(root, "no-such-array");
	printf("get_array(missing)=%p\n", (const void *)missing_arr);

	dump_data_sorted("json", root);
	dump_sorted_json("json_with_defaults", obs_data_get_json_with_defaults(root));

	obs_data_t *defaults = obs_data_get_defaults(root);
	dump_data_sorted("defaults_only", defaults);
	obs_data_release(defaults);

	/* JSON round trip through jansson + obs_data */
	const char *json = obs_data_get_json(root);
	obs_data_t *rt = obs_data_create_from_json(json);
	printf("round trip:\n");
	if (rt) {
		printf("  rt_name=%s\n", obs_data_get_string(rt, "name"));
		printf("  rt_count=%lld\n", obs_data_get_int(rt, "count"));
		printf("  rt_ratio=%.9g\n", obs_data_get_double(rt, "ratio"));
		dump_data_sorted("rt_json", rt);
		obs_data_t *rt_settings = obs_data_get_obj(rt, "settings");
		if (rt_settings) {
			printf("  rt_settings.inner_int=%lld\n", obs_data_get_int(rt_settings, "inner_int"));
			obs_data_release(rt_settings);
		}
		obs_data_t *rt_item0 = obs_data_array_item(obs_data_get_array(rt, "items"), 0);
		if (rt_item0) {
			printf("  rt_items[0].v=%lld\n", obs_data_get_int(rt_item0, "v"));
			obs_data_release(rt_item0);
		}
		obs_data_release(rt);
	} else {
		printf("  (parse failed)\n");
	}

	/* invalid JSON */
	obs_data_t *bad = obs_data_create_from_json("{not json");
	printf("from_json(invalid)=%p\n", (const void *)bad);
	if (bad)
		obs_data_release(bad);

	/* apply */
	obs_data_t *target = obs_data_create();
	obs_data_set_string(target, "name", "old");
	obs_data_set_int(target, "count", 1);
	obs_data_t *patch = obs_data_create();
	obs_data_set_string(patch, "name", "patched");
	obs_data_set_bool(patch, "enabled", false);
	obs_data_apply(target, patch);
	dump_data_sorted("after_apply", target);
	obs_data_release(target);
	obs_data_release(patch);

	/* erase + clear */
	obs_data_t *ec = obs_data_create();
	obs_data_set_string(ec, "a", "1");
	obs_data_set_string(ec, "b", "2");
	obs_data_erase(ec, "a");
	dump_data_sorted("after_erase_a", ec);
	obs_data_clear(ec);
	dump_data_sorted("after_clear", ec);
	obs_data_release(ec);

	obs_data_release(settings);
	obs_data_array_release(items);
	obs_data_release(root);
}

static int cmp_names(const void *a, const void *b)
{
	const char *const *x = a;
	const char *const *y = b;
	return strcmp(*x, *y);
}

static void obs_ref_dump_data_fixtures(void)
{
	const char *dir = "test/fixtures/data-dump";
	DIR *d = opendir(dir);
	if (!d) {
		printf("# data/obs_data_fixtures\n  (no fixture dir: %s)\n", dir);
		return;
	}
	char **names = NULL;
	size_t n = 0, cap = 0;
	struct dirent *e;
	while ((e = readdir(d)) != NULL) {
		size_t len = strlen(e->d_name);
		if (len < 6 || strcmp(e->d_name + len - 5, ".json") != 0)
			continue;
		if (n == cap) {
			cap = cap ? cap * 2 : 8;
			names = realloc(names, cap * sizeof(char *));
		}
		names[n] = strdup(e->d_name);
		n++;
	}
	closedir(d);
	qsort(names, n, sizeof(char *), cmp_names);

	printf("# data/obs_data_fixtures\n");
	for (size_t i = 0; i < n; i++) {
		char path[512];
		snprintf(path, sizeof(path), "%s/%s", dir, names[i]);
		obs_data_t *data = obs_data_create_from_json_file(path);
		printf("file %s:\n", names[i]);
		if (!data) {
			printf("  (parse failed)\n");
			free(names[i]);
			continue;
		}
		dump_data_sorted("  json", data);
		const char *json = obs_data_get_json(data);
		obs_data_t *rt = obs_data_create_from_json(json);
		if (rt) {
			dump_data_sorted("  rt_json", rt);
			obs_data_release(rt);
		} else {
			printf("  (round trip failed)\n");
		}
		obs_data_release(data);
		free(names[i]);
	}
	free(names);
}

/* jansson 2.15.1 shortest round-trip real: set_double -> json text ->
 * reparse; the golden pins the exact encoding for each value. */
static void obs_ref_dump_data_double_roundtrip(void)
{
	printf("# data/obs_data_double_roundtrip\n");
	rd_reset_prng();
	for (int c = 0; c < CASES; c++) {
		double v;
		switch (c % 4) {
		case 0:
			v = r01d() * 2.0 - 1.0;
			break;
		case 1:
			v = r01d() * 1e-300;
			break;
		case 2:
			v = r01d() * 1e308;
			break;
		default:
			v = (r01d() * 2.0 - 1.0) * 1e16;
			break;
		}
		obs_data_t *data = obs_data_create();
		obs_data_set_double(data, "v", v);
		const char *json = obs_data_get_json(data);
		json_t *t = json ? json_loads(json, 0, NULL) : NULL;
			double back = 0.0;
		bool ok = false;
		if (t && json_is_real(json_object_get(t, "v"))) {
			back = json_real_value(json_object_get(t, "v"));
			ok = (back == v);
		}
		printf("case %d: v=%.17g json=%s reparsed=%.17g roundtrip=%d\n", c, v,
		       json ? json : "(null)", back, (int)ok);
		if (t)
			json_decref(t);
		obs_data_release(data);
	}
}

static void obs_ref_dump_calldata(void)
{
	printf("# data/calldata\n");

	calldata_t *cd = calldata_create();

	calldata_set_int(cd, "i", 123456789LL);
	calldata_set_float(cd, "f", 3.5);
	calldata_set_bool(cd, "b", true);
	calldata_set_string(cd, "s", "hello");
	calldata_set_ptr(cd, "p", (void *)(uintptr_t)0xdeadbeef0123UL);
	uint8_t raw[6] = {0, 1, 2, 3, 255, 254};
	calldata_set_data(cd, "raw", raw, sizeof(raw));

	long long i = 0;
	bool gi = calldata_get_int(cd, "i", &i);
	printf("get_int(i)=%d val=%lld\n", (int)gi, i);
	double f = 0;
	bool gf = calldata_get_float(cd, "f", &f);
	printf("get_float(f)=%d val=%.9g\n", (int)gf, f);
	bool b = false;
	bool gb = calldata_get_bool(cd, "b", &b);
	printf("get_bool(b)=%d val=%d\n", (int)gb, (int)b);
	const char *s = NULL;
	bool gs = calldata_get_string(cd, "s", &s);
	printf("get_string(s)=%d val=[%s]\n", (int)gs, s ? s : "(null)");
	void *p = NULL;
	bool gp = calldata_get_ptr(cd, "p", &p);
	printf("get_ptr(p)=%d val=0x%lx\n", (int)gp, (unsigned long)(uintptr_t)p);
	/* calldata_get_data requires the EXACT stored size (C quirk, pinned) */
	uint8_t out[6] = {0};
	bool rd = calldata_get_data(cd, "raw", out, sizeof(out));
	printf("get_data(raw,6)=%d bytes=%02x %02x %02x %02x %02x %02x\n", (int)rd, out[0], out[1], out[2], out[3], out[4],
	       out[5]);
	uint8_t out16[16] = {0xAA};
	bool rd16 = calldata_get_data(cd, "raw", out16, sizeof(out16));
	printf("get_data(raw,16) wrong-size=%d byte0=%02x\n", (int)rd16, out16[0]);

	/* type mismatch + unset */
	long long wrong = 0;
	bool gw = calldata_get_int(cd, "s", &wrong);
	printf("get_int(s) type-mismatch=%d val=%lld\n", (int)gw, wrong);
	long long no = 0;
	bool gn = calldata_get_int(cd, "missing", &no);
	printf("get_int(missing)=%d val=%lld\n", (int)gn, no);
	const char *ns = NULL;
	bool gns = calldata_get_string(cd, "missing", &ns);
	printf("get_string(missing)=%d val=[%s]\n", (int)gns, ns ? ns : "(null)");

	/* 'call if you know your data is valid' getters: default on miss */
	printf("calldata_int(missing)=%lld\n", calldata_int(cd, "missing"));
	printf("calldata_float(missing)=%.9g\n", calldata_float(cd, "missing"));
	printf("calldata_bool(missing)=%d\n", (int)calldata_bool(cd, "missing"));
	printf("calldata_ptr(missing)=%p\n", calldata_ptr(cd, "missing"));
	printf("calldata_string(missing)=%p\n", (const void *)calldata_string(cd, "missing"));
	printf("calldata_int(i)=%lld\n", calldata_int(cd, "i"));
	printf("calldata_string(s)=[%s]\n", calldata_string(cd, "s"));

	/* overwrite + zero-length data */
	calldata_set_int(cd, "i", -1);
	bool gi2 = calldata_get_int(cd, "i", &i);
	printf("after_set_int(i,-1): get_int(i)=%d val=%lld\n", (int)gi2, i);
	calldata_set_data(cd, "raw", NULL, 0);
	uint8_t out2[16] = {0xAA};
	bool rd2 = calldata_get_data(cd, "raw", out2, sizeof(out2));
	printf("after_set_data(raw,NULL,0): get_data=%d byte0=%02x\n", (int)rd2, out2[0]);

	/* clear + get after clear */
	calldata_clear(cd);
	long long after = 0;
	bool ga = calldata_get_int(cd, "i", &after);
	printf("after_clear get_int(i)=%d val=%lld\n", (int)ga, after);

	calldata_destroy(cd);
}

void obs_ref_dump_data(void)
{
	rd_reset_prng();
	obs_ref_dump_data_build();
	obs_ref_dump_data_double_roundtrip();
	obs_ref_dump_data_fixtures();
	obs_ref_dump_calldata();
}
