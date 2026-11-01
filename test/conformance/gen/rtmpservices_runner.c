/*
 * Conformance runner for plugins/rtmp-services (spec S-026). Links the real rtmp-common.c,
 * rtmp-custom.c, rtmp-services-main.c and service-specific/ sources plus the real obs-data.c /
 * obs-properties.c; the rest of OBS is faked in rtmpservices_stubs.c.
 * Script language: see test/conformance/README.md, section "rtmpservices runner".
 */
#include <jansson.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <obs.h>
#include <obs-module.h>
#include <obs-properties.h>
#include <callback/calldata.h>
#include <file-updater/file-updater.h>
#include <util/platform.h>

#include "harness.h"

/* ---- controls implemented in rtmpservices_stubs.c ---- */
extern char *fake_data_dir, *fake_config_dir;
extern char *fake_protocols[];
extern int fake_protocol_count;
extern bool fake_video_valid;
extern struct obs_video_info fake_video;
extern uint64_t fake_now_ns;
struct fake_proc {
	char decl[128];
	proc_handler_proc_t proc;
	void *data;
};
extern struct fake_proc fake_procs[];
extern int fake_proc_count;
extern struct obs_service_info fake_registered[];
extern int fake_registered_count;
extern confirm_file_callback_t fake_confirm_cb;
extern void *fake_confirm_param;
void fake_fs_put(const char *path, const char *data, size_t len);
bool fake_fs_get(const char *path, const char **data, size_t *len);
void fake_fs_del(const char *path);
void fake_fs_list(void);
void fake_advance_ms(uint64_t ms);
void fake_updater_route(const char *url, uint64_t delay_ms, const char *body);
void fake_curl_route(const char *url, long status, const char *body);
void fake_curl_clear(void);

/* from rtmp-services-main.c / rtmp-common.c / rtmp-custom.c */
extern struct obs_service_info rtmp_common_service;
extern struct obs_service_info rtmp_custom_service;
extern const char *get_module_name(void);
extern void obs_module_set_pointer(obs_module_t *module);
extern void obs_module_set_locale(const char *locale);
extern void obs_module_free_locale(void);

static char script_dir[1024];

/* ---- handle tables ---- */
#define MAXH 16
static struct {
	char *name;
	obs_data_t *d;
} settings_tab[MAXH];
static struct {
	char *name;
	const struct obs_service_info *info;
	void *data;
} svc_tab[MAXH];
static struct {
	char *name;
	obs_properties_t *p;
} props_tab[MAXH];

static obs_data_t *find_settings(const char *name, bool required)
{
	if (strcmp(name, "-") == 0)
		return NULL;
	for (int i = 0; i < MAXH; i++)
		if (settings_tab[i].name && strcmp(settings_tab[i].name, name) == 0)
			return settings_tab[i].d;
	if (required) {
		fprintf(stderr, "script: unknown settings '%s'\n", name);
		exit(2);
	}
	return NULL;
}

static int find_svc(const char *name)
{
	for (int i = 0; i < MAXH; i++)
		if (svc_tab[i].name && strcmp(svc_tab[i].name, name) == 0)
			return i;
	fprintf(stderr, "script: unknown service '%s'\n", name);
	exit(2);
}

static int find_props(const char *name)
{
	for (int i = 0; i < MAXH; i++)
		if (props_tab[i].name && strcmp(props_tab[i].name, name) == 0)
			return i;
	fprintf(stderr, "script: unknown props '%s'\n", name);
	exit(2);
}

static void pstr(const char *s)
{
	if (!s)
		fputs("@null", stdout);
	else
		script_print_quoted(s);
}

/* ---- settings dump (keys sorted) ---- */
static int cmp_names(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static void dump_data(obs_data_t *d, int indent)
{
	char *names[64];
	int n = 0;
	for (obs_data_item_t *it = obs_data_first(d); it; obs_data_item_next(&it)) {
		if (n < 64)
			names[n++] = strdup(obs_data_item_get_name(it));
	}
	qsort(names, (size_t)n, sizeof(names[0]), cmp_names);
	for (int i = 0; i < n; i++) {
		obs_data_item_t *it = obs_data_item_byname(d, names[i]);
		printf("%*s%s = ", indent, "", names[i]);
		switch (obs_data_item_gettype(it)) {
		case OBS_DATA_STRING:
			script_print_quoted(obs_data_item_get_string(it));
			putchar('\n');
			break;
		case OBS_DATA_NUMBER:
			if (obs_data_item_numtype(it) == OBS_DATA_NUM_INT)
				printf("int %lld\n", obs_data_item_get_int(it));
			else
				printf("double %.17g\n", obs_data_item_get_double(it));
			break;
		case OBS_DATA_BOOLEAN:
			printf("bool %d\n", obs_data_item_get_bool(it) ? 1 : 0);
			break;
		case OBS_DATA_OBJECT: {
			obs_data_t *o = obs_data_item_get_obj(it);
			printf("object\n");
			dump_data(o, indent + 2);
			obs_data_release(o);
			break;
		}
		case OBS_DATA_ARRAY: {
			obs_data_array_t *a = obs_data_item_get_array(it);
			printf("array count=%zu\n", obs_data_array_count(a));
			obs_data_array_release(a);
			break;
		}
		default:
			printf("null\n");
		}
		obs_data_item_release(&it);
		free(names[i]);
	}
}

static void show_settings(const char *label, obs_data_t *d)
{
	printf("settings %s\n", label);
	if (d)
		dump_data(d, 2);
	else
		printf("  @null\n");
}

/* ---- property dump ---- */
static const char *ptype_name(enum obs_property_type t)
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
	case OBS_PROPERTY_PATH:
		return "path";
	case OBS_PROPERTY_LIST:
		return "list";
	case OBS_PROPERTY_COLOR:
		return "color";
	case OBS_PROPERTY_BUTTON:
		return "button";
	case OBS_PROPERTY_FONT:
		return "font";
	case OBS_PROPERTY_EDITABLE_LIST:
		return "editable_list";
	case OBS_PROPERTY_FRAME_RATE:
		return "frame_rate";
	case OBS_PROPERTY_GROUP:
		return "group";
	default:
		return "other";
	}
}

static void show_props(const char *label, obs_properties_t *props)
{
	printf("props %s\n", label);
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		enum obs_property_type t = obs_property_get_type(p);
		printf("  prop %s type=%s desc=", obs_property_name(p), ptype_name(t));
		pstr(obs_property_description(p));
		printf(" visible=%d enabled=%d", obs_property_visible(p) ? 1 : 0, obs_property_enabled(p) ? 1 : 0);
		if (t == OBS_PROPERTY_TEXT)
			printf(" text_type=%d", (int)obs_property_text_type(p));
		if (t == OBS_PROPERTY_LIST) {
			size_t cnt = obs_property_list_item_count(p);
			printf(" combo=%d format=%d items=%zu", (int)obs_property_list_type(p),
			       (int)obs_property_list_format(p), cnt);
			putchar('\n');
			for (size_t i = 0; i < cnt; i++) {
				printf("    item %zu name=", i);
				pstr(obs_property_list_item_name(p, i));
				printf(" value=");
				pstr(obs_property_list_item_string(p, i));
				printf(" disabled=%d\n", obs_property_list_item_disabled(p, i) ? 1 : 0);
			}
			continue;
		}
		putchar('\n');
	}
}

/* ---- service getters ---- */
static void print_codecs(const char **list)
{
	if (!list) {
		printf("@null\n");
		return;
	}
	printf("[");
	for (size_t i = 0; list[i]; i++) {
		if (i)
			printf(",");
		script_print_quoted(list[i]);
	}
	printf("]\n");
}

static void svc_get(const struct obs_service_info *info, void *data, const char *field)
{
	/* evaluate first so that "fake ..." lines printed by the callee precede the result line */
	const char *sv = NULL;
	bool is_str = true;
	if (!strcmp(field, "protocol"))
		sv = info->get_protocol(data);
	else if (!strcmp(field, "url"))
		sv = info->get_url(data);
	else if (!strcmp(field, "key"))
		sv = info->get_key(data);
	else if (!strcmp(field, "user"))
		sv = info->get_username(data);
	else if (!strcmp(field, "pass"))
		sv = info->get_password(data);
	else if (!strncmp(field, "connect:", 8))
		sv = info->get_connect_info(data, (uint32_t)atoi(field + 8));
	else
		is_str = false;
	if (is_str) {
		printf("get %s = ", field);
		pstr(sv);
		putchar('\n');
		return;
	}
	if (!strcmp(field, "can_connect")) {
		int r = info->can_try_to_connect ? (info->can_try_to_connect(data) ? 1 : 0) : 1;
		printf("get %s = %d\n", field, r);
	} else if (!strcmp(field, "resolutions")) {
		struct obs_service_resolution *res = NULL;
		size_t cnt = 0;
		if (info->get_supported_resolutions)
			info->get_supported_resolutions(data, &res, &cnt);
		printf("get %s = [", field);
		for (size_t i = 0; i < cnt; i++)
			printf("%s%dx%d", i ? "," : "", res[i].cx, res[i].cy);
		printf("]\n");
		bfree(res);
	} else if (!strcmp(field, "maxfps")) {
		int fps = 0;
		if (info->get_max_fps)
			info->get_max_fps(data, &fps);
		printf("get %s = %d\n", field, fps);
	} else if (!strcmp(field, "maxbitrate")) {
		int v = 0, a = 0; /* the host wrapper zeroes both before calling */
		if (info->get_max_bitrate)
			info->get_max_bitrate(data, &v, &a);
		printf("get %s = video=%d audio=%d\n", field, v, a);
	} else if (!strcmp(field, "vcodecs")) {
		printf("get %s = ", field);
		print_codecs(info->get_supported_video_codecs ? info->get_supported_video_codecs(data) : NULL);
	} else if (!strcmp(field, "acodecs")) {
		printf("get %s = ", field);
		print_codecs(info->get_supported_audio_codecs ? info->get_supported_audio_codecs(data) : NULL);
	} else {
		printf("get %s = ?unknown-field\n", field);
	}
}

static const char *const all_fields[] = {"protocol",  "url",       "key",       "user",      "pass",
					 "can_connect", "connect:0", "connect:2", "connect:4", "connect:6",
					 "connect:8", "connect:10", "resolutions", "maxfps",   "maxbitrate",
					 "vcodecs",   "acodecs"};

/* ---- smoke test of a services.json ---- */
static void smoke(const char *vpath)
{
	const char *data;
	size_t len;
	if (!fake_fs_get(vpath, &data, &len)) {
		printf("smoke missing\n");
		return;
	}
	json_error_t err;
	json_t *root = json_loads(data, 0, &err);
	if (!root) {
		printf("smoke parse-error\n");
		return;
	}
	printf("smoke format_version=%d\n", (int)json_integer_value(json_object_get(root, "format_version")));
	json_t *services = json_object_get(root, "services");
	size_t nserv = json_array_size(services);
	printf("smoke services>0=%d\n", nserv > 0);

	/* distinct key sets (sorted) */
	const char *scopes[] = {"service", "server", "recommended", "bitrate-matrix"};
	char *keys[4][128] = {{0}};
	int nk[4] = {0};
	size_t idx;
	json_t *s;
	json_array_foreach (services, idx, s) {
		const char *k;
		json_t *v;
		json_object_foreach (s, k, v) {
			bool have = false;
			for (int i = 0; i < nk[0]; i++)
				have |= !strcmp(keys[0][i], k);
			if (!have && nk[0] < 128)
				keys[0][nk[0]++] = strdup(k);
		}
		json_t *servers = json_object_get(s, "servers");
		size_t si;
		json_t *sv;
		json_array_foreach (servers, si, sv) {
			json_object_foreach (sv, k, v) {
				bool have = false;
				for (int i = 0; i < nk[1]; i++)
					have |= !strcmp(keys[1][i], k);
				if (!have && nk[1] < 128)
					keys[1][nk[1]++] = strdup(k);
			}
		}
		json_t *rec = json_object_get(s, "recommended");
		json_object_foreach (rec, k, v) {
			bool have = false;
			for (int i = 0; i < nk[2]; i++)
				have |= !strcmp(keys[2][i], k);
			if (!have && nk[2] < 128)
				keys[2][nk[2]++] = strdup(k);
		}
		json_t *bm = json_object_get(rec, "bitrate matrix");
		size_t bi;
		json_t *b;
		json_array_foreach (bm, bi, b) {
			json_object_foreach (b, k, v) {
				bool have = false;
				for (int i = 0; i < nk[3]; i++)
					have |= !strcmp(keys[3][i], k);
				if (!have && nk[3] < 128)
					keys[3][nk[3]++] = strdup(k);
			}
		}
	}
	for (int sc = 0; sc < 4; sc++) {
		qsort(keys[sc], (size_t)nk[sc], sizeof(char *), cmp_names);
		printf("smoke keys %s:", scopes[sc]);
		for (int i = 0; i < nk[sc]; i++)
			printf(" %s", keys[sc][i]);
		putchar('\n');
	}

	/* every service: create from settings {service:name}, open properties, check the invariants.
	 * One forked child per service so that a reference crash is attributed (and survivable). */
	size_t crashes = 0, no_server = 0, no_url = 0, selected = 0;
	size_t nlist = 0;
	json_array_foreach (services, idx, s) {
		const char *name = json_string_value(json_object_get(s, "name"));
		if (!name)
			continue;
		nlist++;
		fflush(stdout);
		pid_t pid = fork();
		if (pid == 0) {
			freopen("/dev/null", "w", stdout);
			obs_data_t *st = obs_data_create();
			obs_data_set_string(st, "service", name);
			void *svc = rtmp_common_service.create(st, NULL);
			obs_properties_t *props = rtmp_common_service.get_properties(svc);
			obs_properties_apply_settings(props, st);
			obs_property_t *sp = obs_properties_get(props, "server");
			size_t ns = obs_property_list_item_count(sp);
			int code = 0;
			if (ns == 0)
				code |= 1;
			/* choose the first server of the list, as a user would, and re-read the settings */
			obs_data_set_string(st, "server", obs_property_list_item_string(sp, 0));
			rtmp_common_service.update(svc, st);
			const char *url = rtmp_common_service.get_url(svc);
			static const char *const dynamic[] = {"Twitch", "Amazon IVS", "Nimo TV", "SHOWROOM", "Dacast"};
			bool dyn = false;
			for (size_t k = 0; k < 5; k++)
				dyn |= !strcmp(name, dynamic[k]);
			if (!dyn && (!url || !*url))
				code |= 2;
			const char *proto = rtmp_common_service.get_protocol(svc);
			if (!proto)
				code |= 4;
			/* touch every getter */
			(void)rtmp_common_service.get_key(svc);
			if (rtmp_common_service.get_supported_video_codecs)
				(void)rtmp_common_service.get_supported_video_codecs(svc);
			if (rtmp_common_service.get_supported_audio_codecs)
				(void)rtmp_common_service.get_supported_audio_codecs(svc);
			_exit(code);
		}
		int status = 0;
		waitpid(pid, &status, 0);
		if (!WIFEXITED(status)) {
			crashes++;
			if (getenv("SMOKE_DEBUG"))
				fprintf(stderr, "smoke crash: %s\n", name);
		}
		else {
			selected++;
			if (WEXITSTATUS(status) & 1)
				no_server++;
			if (WEXITSTATUS(status) & 2)
				no_url++;
		}
	}
	printf("smoke listed>0=%d crashes=%zu\n", nlist > 0, crashes);
	if (obs_is_output_protocol_registered("RTMPS")) {
		printf("smoke all-have-servers=%d\n", no_server == 0);
		printf("smoke all-have-url=%d\n", no_url == 0);
	}
	(void)selected;
	json_decref(root);
}

/* ---- main ---- */
static int in_guard_skip(FILE *f, int *lineno)
{
	struct script_line sl;
	while (script_next(f, &sl, lineno))
		if (!strcmp(sl.argv[0], "guard-end"))
			return 1;
	return 0;
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	static char module_dummy;

	snprintf(script_dir, sizeof(script_dir), "%s", argv[1]);
	char *slash = strrchr(script_dir, '/');
	if (slash)
		*slash = 0;
	else
		strcpy(script_dir, ".");

	obs_module_set_pointer((obs_module_t *)&module_dummy);
	obs_module_set_locale("en-US");
	/* fs defaults initialise lazily inside the stubs: touch it */
	(void)os_file_exists("/");

	bool in_guard = false;
	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];

		if (!strcmp(c, "echo")) {
			printf("echo");
			for (int i = 1; i < sl.argc; i++)
				printf(" %s", sl.argv[i]);
			putchar('\n');
		} else if (!strcmp(c, "guard-begin")) {
			printf("guard-begin\n");
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				in_guard = true;
				/* "guard-begin quiet": result lines of the guarded block are discarded (used where
				 * the reference result is undefined behaviour and only "did not crash" is specified) */
				if (sl.argc > 1 && !strcmp(sl.argv[1], "quiet")) {
					fflush(stdout);
					if (!freopen("/dev/null", "w", stdout))
						_exit(3);
					setvbuf(stdout, NULL, _IOLBF, 0);
				}
				continue;
			}
			in_guard_skip(f, &lineno);
			int status = 0;
			waitpid(pid, &status, 0);
			if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
				printf("guard-end ok\n");
			else
				printf("guard-end crash\n");
		} else if (!strcmp(c, "guard-end")) {
			if (in_guard) {
				fflush(stdout);
				_exit(0);
			}
		} else if (!strcmp(c, "fs-put")) {
			script_need(&sl, 3);
			fake_fs_put(sl.argv[1], sl.argv[2], strlen(sl.argv[2]));
		} else if (!strcmp(c, "fs-load")) {
			script_need(&sl, 3);
			char path[2048];
			snprintf(path, sizeof(path), "%s/%s", script_dir, sl.argv[2]);
			FILE *in = fopen(path, "rb");
			if (!in) {
				fprintf(stderr, "script: cannot open %s\n", path);
				exit(2);
			}
			fseek(in, 0, SEEK_END);
			long sz = ftell(in);
			fseek(in, 0, SEEK_SET);
			char *buf = malloc((size_t)sz + 1);
			size_t got = fread(buf, 1, (size_t)sz, in);
			fclose(in);
			fake_fs_put(sl.argv[1], buf, got);
			free(buf);
		} else if (!strcmp(c, "fs-del")) {
			script_need(&sl, 2);
			fake_fs_del(sl.argv[1]);
		} else if (!strcmp(c, "fs-cat")) {
			script_need(&sl, 2);
			const char *d;
			size_t l;
			printf("fs-cat %s ", sl.argv[1]);
			if (fake_fs_get(sl.argv[1], &d, &l))
				script_print_quoted(d);
			else
				printf("@missing");
			putchar('\n');
		} else if (!strcmp(c, "fs-ls")) {
			fake_fs_list();
		} else if (!strcmp(c, "dirs")) {
			script_need(&sl, 3);
			free(fake_data_dir);
			free(fake_config_dir);
			fake_data_dir = strcmp(sl.argv[1], "-") ? strdup(sl.argv[1]) : NULL;
			fake_config_dir = strcmp(sl.argv[2], "-") ? strdup(sl.argv[2]) : NULL;
		} else if (!strcmp(c, "protocols")) {
			fake_protocol_count = 0;
			for (int i = 1; i < sl.argc && fake_protocol_count < 8; i++)
				if (strcmp(sl.argv[i], "-"))
					fake_protocols[fake_protocol_count++] = strdup(sl.argv[i]);
		} else if (!strcmp(c, "video-info")) {
			script_need(&sl, 2);
			if (!strcmp(sl.argv[1], "-")) {
				fake_video_valid = false;
			} else {
				script_need(&sl, 5);
				fake_video_valid = true;
				fake_video.output_width = (uint32_t)atoi(sl.argv[1]);
				fake_video.output_height = (uint32_t)atoi(sl.argv[2]);
				fake_video.fps_num = (uint32_t)atoi(sl.argv[3]);
				fake_video.fps_den = (uint32_t)atoi(sl.argv[4]);
			}
		} else if (!strcmp(c, "locale")) {
			script_need(&sl, 2);
			if (!strcmp(sl.argv[1], "none"))
				obs_module_free_locale();
			else
				obs_module_set_locale(sl.argv[1]);
		} else if (!strcmp(c, "advance")) {
			script_need(&sl, 2);
			fake_advance_ms((uint64_t)atoll(sl.argv[1]));
		} else if (!strcmp(c, "clock")) {
			printf("clock %llu ms\n", (unsigned long long)(fake_now_ns / 1000000ULL));
		} else if (!strcmp(c, "curl-route")) {
			script_need(&sl, 4);
			fake_curl_route(sl.argv[1], atol(sl.argv[2]), sl.argv[3]);
		} else if (!strcmp(c, "curl-clear")) {
			fake_curl_clear();
		} else if (!strcmp(c, "updater-route")) {
			script_need(&sl, 4);
			fake_updater_route(sl.argv[1], (uint64_t)atoll(sl.argv[2]), sl.argv[3]);
		} else if (!strcmp(c, "module-load")) {
			bool r = obs_module_load();
			printf("module-load -> %d name=", r ? 1 : 0);
			pstr(get_module_name());
			putchar('\n');
		} else if (!strcmp(c, "module-unload")) {
			obs_module_unload();
			printf("module-unload\n");
		} else if (!strcmp(c, "confirm")) {
			script_need(&sl, 3);
			if (!fake_confirm_cb) {
				printf("confirm no-callback\n");
				continue;
			}
			struct file_download_data fd = {0};
			size_t l = strlen(sl.argv[2]);
			fd.name = sl.argv[1];
			da_resize(fd.buffer, l + 1);
			memcpy(fd.buffer.array, sl.argv[2], l + 1);
			fd.buffer.num = l;
			bool r = fake_confirm_cb(fake_confirm_param, &fd);
			printf("confirm %s -> %d\n", sl.argv[1], r ? 1 : 0);
			da_free(fd.buffer);
		} else if (!strcmp(c, "proc-call")) {
			script_need(&sl, 3);
			for (int i = 0; i < fake_proc_count; i++) {
				if (strstr(fake_procs[i].decl, sl.argv[1])) {
					calldata_t cd;
					calldata_init(&cd);
					calldata_set_int(&cd, "seconds", atoll(sl.argv[2]));
					fake_procs[i].proc(fake_procs[i].data, &cd);
					calldata_free(&cd);
					printf("proc-call %s seconds=%s\n", sl.argv[1], sl.argv[2]);
				}
			}
		} else if (!strcmp(c, "settings")) {
			script_need(&sl, 2);
			int slot = 0;
			while (slot < MAXH && settings_tab[slot].name && strcmp(settings_tab[slot].name, sl.argv[1]))
				slot++;
			if (slot == MAXH) {
				fprintf(stderr, "script: settings table full\n");
				exit(2);
			}
			if (settings_tab[slot].name)
				obs_data_release(settings_tab[slot].d);
			else
				settings_tab[slot].name = strdup(sl.argv[1]);
			if (sl.argc > 2) {
				/* JSON may be spread over several tokens: they are re-joined with single spaces */
				char js[SCRIPT_MAX_LINE];
				js[0] = 0;
				for (int i = 2; i < sl.argc; i++) {
					if (i > 2)
						strcat(js, " ");
					strcat(js, sl.argv[i]);
				}
				settings_tab[slot].d = obs_data_create_from_json(js);
			} else {
				settings_tab[slot].d = obs_data_create();
			}
			if (!settings_tab[slot].d) {
				printf("settings %s json-error\n", sl.argv[1]);
				settings_tab[slot].d = obs_data_create();
			}
		} else if (!strcmp(c, "set")) {
			script_need(&sl, 5);
			obs_data_t *d = find_settings(sl.argv[1], true);
			if (!strcmp(sl.argv[3], "str"))
				obs_data_set_string(d, sl.argv[2], sl.argv[4]);
			else if (!strcmp(sl.argv[3], "int"))
				obs_data_set_int(d, sl.argv[2], atoll(sl.argv[4]));
			else if (!strcmp(sl.argv[3], "bool"))
				obs_data_set_bool(d, sl.argv[2], atoi(sl.argv[4]) != 0);
			else if (!strcmp(sl.argv[3], "dstr"))
				obs_data_set_default_string(d, sl.argv[2], sl.argv[4]);
			else {
				fprintf(stderr, "script: bad type\n");
				exit(2);
			}
		} else if (!strcmp(c, "unset")) {
			script_need(&sl, 3);
			obs_data_unset_user_value(find_settings(sl.argv[1], true), sl.argv[2]);
		} else if (!strcmp(c, "show")) {
			script_need(&sl, 2);
			show_settings(sl.argv[1], find_settings(sl.argv[1], true));
		} else if (!strcmp(c, "new")) {
			script_need(&sl, 4);
			int slot = 0;
			while (slot < MAXH && svc_tab[slot].name)
				slot++;
			if (slot == MAXH) {
				fprintf(stderr, "script: svc table full\n");
				exit(2);
			}
			const struct obs_service_info *info = !strcmp(sl.argv[2], "rtmp_common") ? &rtmp_common_service
												 : &rtmp_custom_service;
			svc_tab[slot].name = strdup(sl.argv[1]);
			svc_tab[slot].info = info;
			svc_tab[slot].data = info->create(find_settings(sl.argv[3], true), NULL);
		} else if (!strcmp(c, "update")) {
			script_need(&sl, 3);
			int s = find_svc(sl.argv[1]);
			svc_tab[s].info->update(svc_tab[s].data, find_settings(sl.argv[2], true));
		} else if (!strcmp(c, "destroy")) {
			script_need(&sl, 2);
			int s = find_svc(sl.argv[1]);
			svc_tab[s].info->destroy(svc_tab[s].data);
			free(svc_tab[s].name);
			svc_tab[s].name = NULL;
		} else if (!strcmp(c, "get")) {
			script_need(&sl, 3);
			int s = find_svc(sl.argv[1]);
			for (int i = 2; i < sl.argc; i++)
				svc_get(svc_tab[s].info, svc_tab[s].data, sl.argv[i]);
		} else if (!strcmp(c, "all")) {
			script_need(&sl, 2);
			int s = find_svc(sl.argv[1]);
			printf("all %s\n", sl.argv[1]);
			for (size_t i = 0; i < sizeof(all_fields) / sizeof(all_fields[0]); i++) {
				printf("  ");
				svc_get(svc_tab[s].info, svc_tab[s].data, all_fields[i]);
			}
		} else if (!strcmp(c, "apply")) {
			script_need(&sl, 4);
			int s = find_svc(sl.argv[1]);
			obs_data_t *v = find_settings(sl.argv[2], false), *a = find_settings(sl.argv[3], false);
			/* the host wrapper only forwards when at least one of the two is non-NULL */
			if ((v || a) && svc_tab[s].info->apply_encoder_settings)
				svc_tab[s].info->apply_encoder_settings(svc_tab[s].data, v, a);
			printf("apply %s forwarded=%d\n", sl.argv[1], (v || a) ? 1 : 0);
			if (v)
				show_settings(sl.argv[2], v);
			if (a)
				show_settings(sl.argv[3], a);
		} else if (!strcmp(c, "props") || !strcmp(c, "props-raw")) {
			script_need(&sl, 4);
			int s = find_svc(sl.argv[2]);
			int slot = 0;
			while (slot < MAXH && props_tab[slot].name)
				slot++;
			props_tab[slot].name = strdup(sl.argv[1]);
			props_tab[slot].p = svc_tab[s].info->get_properties(svc_tab[s].data);
			/* obs_service_properties() applies the settings (fires every modified callback,
			 * last property first); props-raw does not */
			if (!strcmp(c, "props"))
				obs_properties_apply_settings(props_tab[slot].p, find_settings(sl.argv[3], true));
			show_props(sl.argv[1], props_tab[slot].p);
		} else if (!strcmp(c, "props-show")) {
			script_need(&sl, 2);
			int p = find_props(sl.argv[1]);
			show_props(sl.argv[1], props_tab[p].p);
		} else if (!strcmp(c, "modify")) {
			script_need(&sl, 4);
			int p = find_props(sl.argv[1]);
			obs_property_t *prop = obs_properties_get(props_tab[p].p, sl.argv[2]);
			bool r = prop ? obs_property_modified(prop, find_settings(sl.argv[3], true)) : false;
			printf("modify %s -> %d\n", sl.argv[2], r ? 1 : 0);
		} else if (!strcmp(c, "props-apply")) {
			script_need(&sl, 3);
			int p = find_props(sl.argv[1]);
			obs_properties_apply_settings(props_tab[p].p, find_settings(sl.argv[2], true));
			printf("props-apply\n");
		} else if (!strcmp(c, "props-free")) {
			script_need(&sl, 2);
			int p = find_props(sl.argv[1]);
			obs_properties_destroy(props_tab[p].p);
			free(props_tab[p].name);
			props_tab[p].name = NULL;
		} else if (!strcmp(c, "smoke")) {
			script_need(&sl, 2);
			smoke(sl.argv[1]);
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, c);
			return 2;
		}
	}
	if (in_guard)
		_exit(0);
	return 0;
}
