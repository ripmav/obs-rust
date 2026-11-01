/*
 * Conformance runner for the image/colour/slideshow sources (spec S-030). Script language: see
 * test/conformance/README.md, section "imagesource runner". The reference plugin code is linked unchanged
 * and driven through its obs_source_info callbacks by the fake host in imagesource_stubs.c.
 */
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <obs-module.h>
#include <obs-properties.h>
#include <obs-missing-files.h>
#include "harness.h"
#include "imagesource_host.h"

obs_source_t *host_create(const char *name, const char *id, obs_data_t *settings);

static obs_source_t *need(const char *name)
{
	obs_source_t *s = host_find(name);
	if (!s) {
		fprintf(stderr, "script error: unknown source %s\n", name);
		exit(2);
	}
	return s;
}

static const struct obs_source_info *need_info(const char *id)
{
	const struct obs_source_info *i = host_find_info(id);
	if (!i) {
		fprintf(stderr, "script error: unknown id %s\n", id);
		exit(2);
	}
	return i;
}

static void *data_of(obs_source_t *s)
{
	return host_data(s);
}

static obs_data_t *json_arg(const char *s)
{
	if (!strcmp(s, "-"))
		return NULL;
	obs_data_t *d = obs_data_create_from_json(s);
	if (!d) {
		fprintf(stderr, "script error: bad json %s\n", s);
		exit(2);
	}
	return d;
}

void host_info_print(const struct obs_source_info *i)
{
#define CB(n) i->n ? " " #n : ""
	printf("info id=%s version=%u type=%d flags=0x%x icon=%d\n", i->id, i->version, (int)i->type,
	       (unsigned)i->output_flags, (int)i->icon_type);
	printf("info callbacks%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s%s\n", CB(get_name), CB(create), CB(destroy),
	       CB(update), CB(get_defaults), CB(get_properties), CB(get_width), CB(get_height), CB(video_tick),
	       CB(video_render), CB(audio_render), CB(enum_active_sources), CB(show), CB(hide), CB(activate),
	       CB(deactivate), CB(missing_files), CB(media_play_pause), CB(media_restart), CB(media_stop),
	       CB(media_next), CB(media_previous), CB(media_get_state), CB(video_get_color_space));
	printf("info media_extra%s%s%s\n", CB(media_get_time), CB(media_get_duration), CB(media_set_time));
#undef CB
}

void host_defaults(const struct obs_source_info *i)
{
	obs_data_t *d = obs_data_create();
	if (i->get_defaults)
		i->get_defaults(d);
	obs_data_t *df = obs_data_get_defaults(d);
	printf("defaults %s\n", obs_data_get_json(df));
	obs_data_release(df);
	obs_data_release(d);
}

void host_size(obs_source_t *s)
{
	void *d = data_of(s);
	printf("size %u %u\n", host_info_of(s)->get_width ? host_info_of(s)->get_width(d) : 0, host_info_of(s)->get_height ? host_info_of(s)->get_height(d) : 0);
}

void host_colorspace(obs_source_t *s)
{
	enum gs_color_space pref[2] = {GS_CS_709_EXTENDED, GS_CS_SRGB};
	printf("colorspace %d\n", (int)host_info_of(s)->video_get_color_space(data_of(s), 2, pref));
}

void host_dump(obs_source_t *s)
{
	const char *id = host_info_of(s)->id;
	void *d = data_of(s);
	if (!d) {
		printf("state none\n");
		return;
	}
	if (!strcmp(id, "image_source"))
		imagesource_img_dump(d);
	else if (!strncmp(id, "color_source", 12))
		imagesource_color_dump(d);
	else if (!strcmp(id, "slideshow") && host_info_of(s)->version == 2)
		imagesource_ss2_dump(d);
	else if (!strcmp(id, "slideshow"))
		imagesource_ss1_dump(d);
}

static void sig_cb(void *p, calldata_t *cd)
{
	const char *name = p;
	const char *path = NULL;
	long long idx = 0;
	calldata_get_int(cd, "index", &idx);
	calldata_get_string(cd, "path", &path);
	printf("ev signal %s index=%lld path=%s\n", name, idx, path ? path : "@null");
}

void host_connect(obs_source_t *s, const char *sig)
{
	signal_handler_connect(host_sh(s), sig, sig_cb, (void *)bstrdup(sig));
}

void host_proc(obs_source_t *s, const char *name)
{
	calldata_t cd;
	calldata_init(&cd);
	bool ok = proc_handler_call(host_ph(s), name, &cd);
	long long v = -1;
	if (ok) {
		if (!calldata_get_int(&cd, name, &v))
			v = -1;
		printf("proc %s ok %lld\n", name, v);
	} else {
		printf("proc %s not_found\n", name);
	}
	calldata_free(&cd);
}

void host_missing(obs_source_t *s, int idx, const char *newpath)
{
	obs_missing_files_t *f = host_info_of(s)->missing_files(data_of(s));
	size_t n = obs_missing_files_count(f);
	printf("missing count=%zu\n", n);
	for (size_t i = 0; i < n; i++) {
		obs_missing_file_t *mf = obs_missing_files_get_file(f, (int)i);
		printf("missing %zu path=", i);
		script_print_quoted(obs_missing_file_get_path(mf));
		printf("\n");
	}
	if (idx >= 0 && (size_t)idx < n)
		obs_missing_file_issue_callback(obs_missing_files_get_file(f, idx), newpath);
	obs_missing_files_destroy(f);
}

static const char *ptype(enum obs_property_type t)
{
	static const char *n[] = {"invalid", "bool", "int", "float", "text", "path", "list", "color", "button", "font",
				  "editable_list", "frame_rate", "group", "color_alpha"};
	return (int)t < 14 ? n[t] : "?";
}

static void props_dump(obs_properties_t *p);
void host_props(obs_source_t *s)
{
	props_dump(host_info_of(s)->get_properties(data_of(s)));
}

static void props_dump(obs_properties_t *p)
{
	for (obs_property_t *q = obs_properties_first(p); q; obs_property_next(&q)) {
		enum obs_property_type t = obs_property_get_type(q);
		printf("prop %s type=%s desc=", obs_property_name(q), ptype(t));
		script_print_quoted(obs_property_description(q));
		if (t == OBS_PROPERTY_INT)
			printf(" min=%d max=%d step=%d suffix=\"%s\"", obs_property_int_min(q), obs_property_int_max(q),
			       obs_property_int_step(q), obs_property_int_suffix(q) ? obs_property_int_suffix(q) : "");
		if (t == OBS_PROPERTY_PATH) {
			const char *f = obs_property_path_filter(q), *d = obs_property_path_default_path(q);
			printf(" pathtype=%d filter=", (int)obs_property_path_type(q));
			script_print_quoted(f ? f : "");
			printf(" default=");
			script_print_quoted(d ? d : "");
		}
		if (t == OBS_PROPERTY_EDITABLE_LIST) {
			const char *f = obs_property_editable_list_filter(q), *d = obs_property_editable_list_default_path(q);
			printf(" listtype=%d filter=", (int)obs_property_editable_list_type(q));
			script_print_quoted(f ? f : "");
			printf(" default=");
			script_print_quoted(d ? d : "");
		}
		if (t == OBS_PROPERTY_LIST) {
			printf(" combo=%d format=%d count=%zu", (int)obs_property_list_type(q),
			       (int)obs_property_list_format(q), obs_property_list_item_count(q));
		}
		printf("\n");
		if (t == OBS_PROPERTY_LIST)
			for (size_t i = 0; i < obs_property_list_item_count(q); i++)
				printf("  item %zu name=%s value=%s\n", i, obs_property_list_item_name(q, i),
				       obs_property_list_item_string(q, i));
	}
	obs_properties_destroy(p);
}

static int ntok(char **v, int from, int argc, int *out, int max)
{
	int n = 0;
	for (int i = from; i < argc && n < max; i++)
		out[n++] = (int)strtol(v[i], NULL, 0);
	return n;
}


static void run_cmd(int argc, char **v)
{
	const char *c = v[0];
	if (!strcmp(c, "echo")) {
		printf("echo");
		for (int i = 1; i < argc; i++)
			printf(" %s", v[i]);
		printf("\n");
	} else if (!strcmp(c, "rand")) {
		int vals[256];
		int n = ntok(v, 1, argc, vals, 256);
		host_reset_rand(vals, n);
	} else if (!strcmp(c, "rand_add")) {
		int vals[256];
		int n = ntok(v, 1, argc, vals, 256);
		host_add_rand(vals, n);
	} else if (!strcmp(c, "clock")) {
		host_clock_ns = strtoull(v[1], NULL, 0);
	} else if (!strcmp(c, "core")) {
		script_need(&(struct script_line){.argc = argc, .lineno = 0}, 3);
		if (!strcmp(v[1], "srgb_fb"))
			host_srgb_fb = atoi(v[2]);
		else if (!strcmp(v[1], "linear_srgb"))
			host_linear_srgb = atoi(v[2]);
		else if (!strcmp(v[1], "task_auto"))
			host_task_auto = atoi(v[2]);
		else if (!strcmp(v[1], "base_size") && argc >= 4) {
			host_base_w = (uint32_t)atoi(v[2]);
			host_base_h = (uint32_t)atoi(v[3]);
		}
	} else if (!strcmp(c, "fs_file")) {
		host_fs_file(v[1], argc > 2 ? strtoll(v[2], NULL, 0) : 0);
	} else if (!strcmp(c, "fs_rm")) {
		host_fs_rm(v[1]);
	} else if (!strcmp(c, "fs_dir")) {
		host_fs_dir(v[1], argc - 2, v + 2);
	} else if (!strcmp(c, "img")) {
		host_img_still(v[1], atoi(v[2]), atoi(v[3]));
	} else if (!strcmp(c, "img_anim")) {
		int d[64];
		int n = ntok(v, 6, argc, d, 64);
		host_img_anim(v[1], atoi(v[2]), atoi(v[3]), atoi(v[4]), atoi(v[5]), n, d);
	} else if (!strcmp(c, "info")) {
		printf("info query=%s\n", v[1]), host_info_print(need_info(v[1]));
	} else if (!strcmp(c, "registered")) {
		for (int i = 0; i < host_num_infos(); i++)
			printf("registered %s version=%u\n", host_reg_id(i), host_info_at(i)->version);
	} else if (!strcmp(c, "defaults")) {
		host_defaults(need_info(v[1]));
	} else if (!strcmp(c, "create")) {
		obs_data_t *d = argc > 3 ? json_arg(v[3]) : NULL;
		host_create(v[1], v[2], d);
		obs_data_release(d);
	} else if (!strcmp(c, "update")) {
		obs_data_t *d = json_arg(v[2]);
		host_update(need(v[1]), d);
		obs_data_release(d);
	} else if (!strcmp(c, "tick")) {
		float dt = strtof(v[1], NULL);
		int n = argc > 2 ? atoi(v[2]) : 1;
		for (int i = 0; i < n; i++) {
			host_tick_all(dt);
			printf("tick %d clock=%llu\n", i, (unsigned long long)host_clock_ns);
		}
	} else if (!strcmp(c, "refs")) {
		host_set_refs(need(v[1]), atoi(v[2]), atoi(v[3]));
	} else if (!strcmp(c, "media")) {
		host_media(need(v[1]), v[2]);
	} else if (!strcmp(c, "mediastate")) {
		host_media_state(need(v[1]));
	} else if (!strcmp(c, "hotkey")) {
		host_hotkey(need(v[1]), v[2], atoi(v[3]));
	} else if (!strcmp(c, "proc")) {
		host_proc(need(v[1]), v[2]);
	} else if (!strcmp(c, "connect")) {
		host_connect(need(v[1]), v[2]);
	} else if (!strcmp(c, "render")) {
		host_render(need(v[1]));
	} else if (!strcmp(c, "size")) {
		host_size(need(v[1]));
	} else if (!strcmp(c, "state")) {
		host_dump(need(v[1]));
	} else if (!strcmp(c, "props")) {
		host_props(need(v[1]));
	} else if (!strcmp(c, "props_type")) {
		props_dump(need_info(v[1])->get_properties(NULL));
	} else if (!strcmp(c, "colorspace")) {
		host_colorspace(need(v[1]));
	} else if (!strcmp(c, "missing")) {
		host_missing(need(v[1]), argc > 2 ? atoi(v[2]) : -1, argc > 3 && strcmp(v[3], "@null") ? v[3] : NULL);
	} else if (!strcmp(c, "tasks")) {
		host_run_tasks();
	} else if (!strcmp(c, "destroy")) {
		obs_source_release(need(v[1]));
	} else {
		fprintf(stderr, "script error: unknown command %s\n", c);
		exit(2);
	}
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	obs_module_load();
	while (script_next(f, &sl, &lineno)) {
		if (!strcmp(sl.argv[0], "isolate") && sl.argc > 1) {
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				run_cmd(sl.argc - 1, sl.argv + 1);
				fflush(stdout);
				_exit(0);
			}
			int st = 0;
			waitpid(pid, &st, 0);
			if (WIFSIGNALED(st))
				printf("isolate crash\n");
			else if (WEXITSTATUS(st))
				printf("isolate exit %d\n", WEXITSTATUS(st));
			else
				printf("isolate ok\n");
			continue;
		}
		run_cmd(sl.argc, sl.argv);
	}
	fclose(f);
	return 0;
}
