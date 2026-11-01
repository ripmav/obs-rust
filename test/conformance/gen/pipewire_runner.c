/*
 * Conformance runner for area "pipewire" (spec S-056): drives the real plugins/linux-pipewire
 * sources (screencast portal, camera portal, stream engine, formats, module entry) through the
 * scripted fakes in pipewire_stubs.c. See the "pipewire runner" section of test/conformance/README.md.
 */
#include <stdio.h>
#include <string.h>

#include "obs-module.h"
#include "harness.h"
#include "pipewire_api.h"

#define IS(s) (strcmp(a[0], (s)) == 0)

bool obs_module_load(void);
void obs_module_unload(void);
void screencast_portal_load(void);
void screencast_portal_unload(void);
void camera_portal_load(void);
void camera_portal_unload(void);

/* The stream/registry/event pointers the plugin logs with %p are masked; so is the strerror text of %m. */
static void pw_log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096], out[4096];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	char *o = out;
	for (const char *p = msg; *p;) {
		if (p[0] == '0' && p[1] == 'x' && p > msg && p[-1] == ' ') {
			const char *q = p - 1;
			while (q > msg && q[-1] != ' ')
				q--;
			if (!strncmp(q, "stream", 6) || !strncmp(q, "Stream", 6) || !strncmp(q, "registry", 8) ||
			    !strncmp(q, "event", 5)) {
				o += sprintf(o, "<ptr>");
				p += 2;
				while (isxdigit((unsigned char)*p))
					p++;
				continue;
			}
		}
		*o++ = *p == '\n' ? ' ' : *p;
		p++;
	}
	*o = 0;
	char *jv = strstr(out, "Failed reading json string");
	if (jv) /* the jansson error text after it is library dependent */
		strcpy(jv + 26, " <jansson>");
	char *rv = strstr(out, "Removed device ");
	if (rv) /* the reference logs the device id after freeing it (use after free): content not reproducible */
		strcpy(rv + 15, "<id>");
	char *hv = strstr(out, "Header version: ");
	if (hv)
		strcpy(hv + 16, "<hdr>");
	printf("log %s %s\n", level == LOG_ERROR ? "error" : level == LOG_WARNING ? "warning" : level == LOG_INFO ? "info" : "debug",
	       out);
}

static const char *ptype(enum obs_property_type t)
{
	switch (t) {
	case OBS_PROPERTY_BOOL: return "bool";
	case OBS_PROPERTY_INT: return "int";
	case OBS_PROPERTY_FLOAT: return "float";
	case OBS_PROPERTY_TEXT: return "text";
	case OBS_PROPERTY_LIST: return "list";
	case OBS_PROPERTY_BUTTON: return "button";
	case OBS_PROPERTY_GROUP: return "group";
	default: return "other";
	}
}

static void dump_props(obs_properties_t *props, int depth)
{
	if (!props) {
		printf("props NULL\n");
		return;
	}
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		enum obs_property_type t = obs_property_get_type(p);
		printf("%*sprop %s type=%s desc=", depth * 2, "", obs_property_name(p), ptype(t));
		script_print_quoted(obs_property_description(p));
		printf(" visible=%d enabled=%d", obs_property_visible(p), obs_property_enabled(p));
		if (t == OBS_PROPERTY_INT)
			printf(" min=%d max=%d step=%d slider=%d", obs_property_int_min(p), obs_property_int_max(p),
			       obs_property_int_step(p), obs_property_int_type(p) == OBS_NUMBER_SLIDER);
		if (t == OBS_PROPERTY_FLOAT)
			printf(" min=%.9g max=%.9g step=%.9g slider=%d", obs_property_float_min(p), obs_property_float_max(p),
			       obs_property_float_step(p), obs_property_float_type(p) == OBS_NUMBER_SLIDER);
		if (t == OBS_PROPERTY_LIST) {
			size_t n = obs_property_list_item_count(p);
			enum obs_combo_format f = obs_property_list_format(p);
			printf(" format=%d items=%zu\n", (int)f, n);
			for (size_t i = 0; i < n; i++) {
				printf("%*s  item ", depth * 2, "");
				script_print_quoted(obs_property_list_item_name(p, i));
				if (f == OBS_COMBO_FORMAT_INT)
					printf(" = %lld", obs_property_list_item_int(p, i));
				else {
					printf(" = ");
					script_print_quoted(obs_property_list_item_string(p, i));
				}
				printf(" disabled=%d\n", obs_property_list_item_disabled(p, i));
			}
			continue;
		}
		printf("\n");
		if (t == OBS_PROPERTY_GROUP)
			dump_props(obs_property_group_content(p), depth + 1);
	}
}

static obs_property_t *find_prop(struct pwx_inst *in, const char *group, const char *name)
{
	obs_properties_t *ps = in->props;
	if (ps && group && strcmp(group, "-")) {
		obs_property_t *g = obs_properties_get(ps, group);
		ps = g ? obs_property_group_content(g) : NULL;
	}
	return ps ? obs_properties_get(ps, name) : NULL;
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	base_set_log_handler(pw_log_handler, NULL);
	struct script_line sl;
	int lineno = 0;
	while (script_next(f, &sl, &lineno)) {
		char **a = sl.argv;
		printf("> ");
		for (int i = 0; i < sl.argc; i++)
			printf("%s%s", i ? " " : "", a[i]);
		printf("\n");
		if (IS("echo"))
			continue;
		if (IS("module_load")) { printf("module_load -> %d\n", obs_module_load()); continue; }
		if (IS("module_unload")) { obs_module_unload(); continue; }
		if (IS("screencast_load")) { screencast_portal_load(); continue; }
		if (IS("screencast_unload")) { screencast_portal_unload(); continue; }
		if (IS("camera_load")) { camera_portal_load(); continue; }
		if (IS("camera_unload")) { camera_portal_unload(); continue; }
		if (IS("set")) { /* set NAME KEY int|bool|str|dbl VALUE */
			script_need(&sl, 5);
			struct pwx_inst *in = pwx_inst_get(a[1], true);
			if (!strcmp(a[3], "int")) obs_data_set_int(in->settings, a[2], strtoll(a[4], NULL, 0));
			else if (!strcmp(a[3], "bool")) obs_data_set_bool(in->settings, a[2], atoi(a[4]));
			else if (!strcmp(a[3], "dbl")) obs_data_set_double(in->settings, a[2], atof(a[4]));
			else obs_data_set_string(in->settings, a[2], a[4]);
			continue;
		}
		if (IS("get")) { /* get NAME KEY int|bool|str|dbl|json */
			script_need(&sl, 4);
			struct pwx_inst *in = pwx_inst_get(a[1], false);
			if (!in) { printf("no instance\n"); continue; }
			if (!strcmp(a[3], "int")) printf("%s = %lld\n", a[2], obs_data_get_int(in->settings, a[2]));
			else if (!strcmp(a[3], "bool")) printf("%s = %d\n", a[2], obs_data_get_bool(in->settings, a[2]));
			else if (!strcmp(a[3], "dbl")) printf("%s = %.9g\n", a[2], obs_data_get_double(in->settings, a[2]));
			else if (!strcmp(a[3], "user")) printf("%s user=%d\n", a[2], obs_data_has_user_value(in->settings, a[2]));
			else { printf("%s = ", a[2]); script_print_quoted(obs_data_get_string(in->settings, a[2])); printf("\n"); }
			continue;
		}
		if (IS("outflags")) { script_need(&sl, 3); pwx_inst_get(a[1], true)->flags = strtoul(a[2], NULL, 0); continue; }
		if (IS("new")) { /* new NAME SOURCEID */
			script_need(&sl, 3);
			struct pwx_inst *in = pwx_inst_get(a[1], true);
			in->info = pwx_find_info(a[2]);
			if (!in->info) { printf("new: unknown source id\n"); continue; }
			in->flags = in->info->output_flags;
			if (in->info->get_defaults)
				in->info->get_defaults(in->settings);
			in->data = in->info->create(in->settings, (obs_source_t *)in);
			printf("created %s\n", in->name);
			continue;
		}
		if (IS("name")) { script_need(&sl, 2); struct pwx_inst *in = pwx_inst_get(a[1], false); printf("name = %s\n", in->info->get_name(NULL)); continue; }
		if (IS("update")) { struct pwx_inst *in = pwx_inst_get(a[1], false); in->info->update(in->data, in->settings); continue; }
		if (IS("save")) { struct pwx_inst *in = pwx_inst_get(a[1], false); in->info->save(in->data, in->settings); continue; }
		if (IS("show")) { struct pwx_inst *in = pwx_inst_get(a[1], false); in->info->show(in->data); continue; }
		if (IS("hide")) { struct pwx_inst *in = pwx_inst_get(a[1], false); in->info->hide(in->data); continue; }
		if (IS("size")) { struct pwx_inst *in = pwx_inst_get(a[1], false); printf("size = %ux%u\n", in->info->get_width(in->data), in->info->get_height(in->data)); continue; }
		if (IS("render")) { struct pwx_inst *in = pwx_inst_get(a[1], false); in->info->video_render(in->data, (gs_effect_t *)&in->flags); continue; }
		if (IS("destroy")) {
			struct pwx_inst *in = pwx_inst_get(a[1], false);
			if (in->props) obs_properties_destroy(in->props);
			in->props = NULL;
			in->info->destroy(in->data);
			in->data = NULL;
			continue;
		}
		if (IS("props")) {
			struct pwx_inst *in = pwx_inst_get(a[1], false);
			if (in->props) obs_properties_destroy(in->props);
			in->props = in->info->get_properties(in->data);
			dump_props(in->props, 0);
			continue;
		}
		if (IS("dump")) { struct pwx_inst *in = pwx_inst_get(a[1], false); dump_props(in->props, 0); continue; }
		if (IS("click")) { /* click NAME PROP */
			struct pwx_inst *in = pwx_inst_get(a[1], false);
			obs_property_t *p = find_prop(in, NULL, a[2]);
			printf("click -> %d\n", p ? obs_property_button_clicked(p, in->data) : -1);
			continue;
		}
		if (IS("modified")) { /* modified NAME PROP [GROUP] */
			struct pwx_inst *in = pwx_inst_get(a[1], false);
			obs_property_t *p = find_prop(in, sl.argc > 3 ? a[3] : NULL, a[2]);
			if (!p) { printf("modified: no such property\n"); continue; }
			printf("modified -> %d\n", obs_property_modified(p, in->settings));
			continue;
		}
		if (IS("settings_json")) { struct pwx_inst *in = pwx_inst_get(a[1], false); printf("%s\n", obs_data_get_json(in->settings)); continue; }
		if (pwx_cmd(&sl))
			continue;
		fprintf(stderr, "script: line %d: unknown command '%s'\n", lineno, a[0]);
		return 2;
	}
	return 0;
}
