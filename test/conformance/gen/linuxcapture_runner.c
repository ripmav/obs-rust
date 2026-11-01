/*
 * Conformance runner (area linuxcapture, spec S-049): drives the real linux-v4l2, linux-pulseaudio and
 * linux-capture (xshm) plugin sources against fake system calls. Commands are documented in
 * test/conformance/README.md (section "linuxcapture runner"). Test tooling.
 */
#include "obs.h"
#include "harness.h"
#include "linuxcapture_api.h"

obs_data_t *lc_settings;
obs_properties_t *lc_props;
#define IS(s) (!strcmp(a[0], s))

static const char *ptype(enum obs_property_type t)
{
	switch (t) {
	case OBS_PROPERTY_BOOL: return "bool";
	case OBS_PROPERTY_INT: return "int";
	case OBS_PROPERTY_FLOAT: return "float";
	case OBS_PROPERTY_TEXT: return "text";
	case OBS_PROPERTY_LIST: return "list";
	case OBS_PROPERTY_GROUP: return "group";
	default: return "other";
	}
}

void lc_dump_props(obs_properties_t *props, int depth)
{
	if (!props) { printf("%*sprops (null)\n", depth * 2, ""); return; }
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		enum obs_property_type t = obs_property_get_type(p);
		printf("%*sprop %s type=%s desc=", depth * 2, "", obs_property_name(p), ptype(t));
		script_print_quoted(obs_property_description(p));
		printf(" visible=%d enabled=%d", obs_property_visible(p), obs_property_enabled(p));
		if (t == OBS_PROPERTY_INT)
			printf(" min=%d max=%d step=%d slider=%d", obs_property_int_min(p), obs_property_int_max(p), obs_property_int_step(p),
			       obs_property_int_type(p) == OBS_NUMBER_SLIDER);
		if (t == OBS_PROPERTY_TEXT)
			printf(" texttype=%d", (int)obs_property_text_type(p));
		if (t == OBS_PROPERTY_LIST) {
			size_t n = obs_property_list_item_count(p);
			enum obs_combo_format f = obs_property_list_format(p);
			printf(" format=%s items=%zu", f == OBS_COMBO_FORMAT_INT ? "int" : f == OBS_COMBO_FORMAT_STRING ? "string" : "other", n);
		}
		printf("\n");
		if (t == OBS_PROPERTY_LIST) {
			size_t n = obs_property_list_item_count(p);
			enum obs_combo_format f = obs_property_list_format(p);
			for (size_t i = 0; i < n; i++) {
				printf("%*s- ", depth * 2 + 2, "");
				script_print_quoted(obs_property_list_item_name(p, i));
				if (f == OBS_COMBO_FORMAT_INT)
					printf(" = %lld", obs_property_list_item_int(p, i));
				else
					printf(" = \"%s\"", obs_property_list_item_string(p, i));
				printf(" disabled=%d\n", obs_property_list_item_disabled(p, i));
			}
		}
		if (t == OBS_PROPERTY_GROUP)
			lc_dump_props(obs_property_group_content(p), depth + 1);
	}
}

/* Like harness_log_handler, but the platform-dependent strerror text after "errno: " is replaced by <E>. */
static void lc_log_handler(int level, const char *format, va_list args, void *param)
{
	char msg[4096];
	(void)param;
	vsnprintf(msg, sizeof(msg), format, args);
	for (char *p = msg; *p; p++)
		if (*p == '\n')
			*p = ' ';
	char *e = strstr(msg, ", errno: ");
	if (e)
		strcpy(e + 9, "<E>");
	printf("log %s %s\n", level == LOG_ERROR ? "error" : level == LOG_WARNING ? "warning" : level == LOG_INFO ? "info" : "debug", msg);
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	base_set_log_handler(lc_log_handler, NULL);
	struct script_line sl;
	int lineno = 0;
	lc_settings = obs_data_create();
	while (script_next(f, &sl, &lineno)) {
		char **a = sl.argv;
		printf("> ");
		for (int i = 0; i < sl.argc; i++)
			printf("%s%s", i ? " " : "", a[i]);
		printf("\n");
		if (IS("echo")) continue;
		if (IS("set-int")) { script_need(&sl, 3); obs_data_set_int(lc_settings, a[1], strtoll(a[2], NULL, 0)); continue; }
		if (IS("set-bool")) { script_need(&sl, 3); obs_data_set_bool(lc_settings, a[1], atoi(a[2])); continue; }
		if (IS("set-str")) { script_need(&sl, 3); obs_data_set_string(lc_settings, a[1], a[2]); continue; }
		if (IS("settings-clear")) { obs_data_release(lc_settings); lc_settings = obs_data_create(); continue; }
		if (IS("get")) {
			/* get KEY TYPE */
			script_need(&sl, 3);
			if (!strcmp(a[2], "int")) printf("%s = %lld\n", a[1], obs_data_get_int(lc_settings, a[1]));
			else if (!strcmp(a[2], "bool")) printf("%s = %d\n", a[1], obs_data_get_bool(lc_settings, a[1]));
			else printf("%s = \"%s\"\n", a[1], obs_data_get_string(lc_settings, a[1]));
			continue;
		}
		if (IS("props")) { lc_dump_props(lc_props, 0); continue; }
		if (IS("modified") || IS("modified-in")) {
			bool in = IS("modified-in");
			script_need(&sl, in ? 3 : 2);
			obs_properties_t *ps = lc_props;
			if (in) {
				obs_property_t *g = obs_properties_get(ps, a[1]);
				ps = g ? obs_property_group_content(g) : NULL;
			}
			obs_property_t *p = ps ? obs_properties_get(ps, a[in ? 2 : 1]) : NULL;
			if (!p) { printf("modified: no such property\n"); continue; }
			printf("modified -> %d\n", obs_property_modified(p, lc_settings));
			continue;
		}
		if (lc_fake_cmd(&sl) || lc_v4l2_cmd(&sl) || lc_pulse_cmd(&sl) || lc_xshm_cmd(&sl))
			continue;
		fprintf(stderr, "script: line %d: unknown command '%s'\n", lineno, a[0]);
		return 2;
	}
	return 0;
}
