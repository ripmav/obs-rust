/*
 * Conformance runner (area linuxinputs, spec S-062): drives the real Linux input plugins that S-049 only
 * catalogued (ALSA, xcomposite, OSS, sndio, JACK, v4l2 output/udev/decoder) against scripted fakes of the system
 * APIs. Commands: see test/conformance/README.md section "linuxinputs runner". Test tooling.
 */
#include <unistd.h>
#include "obs.h"
#include "harness.h"
#include "linuxinputs_api.h"

void li_install_log(void);
#define IS(s) (!strcmp(a[0], s))

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	li_init();
	li_install_log();
	struct script_line sl;
	int lineno = 0;
	li_settings = obs_data_create();
	while (script_next(f, &sl, &lineno)) {
		char **a = sl.argv;
		li_flush();
		printf("> ");
		for (int i = 0; i < sl.argc; i++)
			printf("%s%s", i ? " " : "", a[i]);
		printf("\n");
		if (IS("echo")) continue;
		if (IS("set-int")) { script_need(&sl, 3); obs_data_set_int(li_settings, a[1], strtoll(a[2], NULL, 0)); continue; }
		if (IS("set-bool")) { script_need(&sl, 3); obs_data_set_bool(li_settings, a[1], atoi(a[2])); continue; }
		if (IS("set-str")) { script_need(&sl, 3); obs_data_set_string(li_settings, a[1], strcmp(a[2], "-") ? a[2] : ""); continue; }
		if (IS("settings-clear")) { obs_data_release(li_settings); li_settings = obs_data_create(); continue; }
		if (IS("now")) { script_need(&sl, 2); li_set_now(strtoull(a[1], NULL, 10)); continue; }
		if (IS("sleep")) { script_need(&sl, 2); li_sleep_ms(atoi(a[1])); continue; }
		if (IS("flush")) { continue; }
		if (IS("audio-reset")) { li_audio_reset(); continue; }
		if (IS("audio-wait")) {
			script_need(&sl, 2);
			printf("audio-wait -> %s\n", li_audio_wait(atoi(a[1]), 3000) ? "ok" : "timeout");
			li_flush();
			continue;
		}
		if (IS("audio-dump")) { li_audio_dump(); continue; }
		if (IS("props")) { li_dump_props(li_props, 0); continue; }
		if (IS("modified")) {
			script_need(&sl, 2);
			obs_property_t *p = li_props ? obs_properties_get(li_props, a[1]) : NULL;
			if (!p) { printf("modified: no such property\n"); continue; }
			printf("modified -> %d\n", obs_property_modified(p, li_settings));
			continue;
		}
		if (li_alsa_cmd(&sl) || li_xcomp_cmd(&sl) || li_oss_cmd(&sl) || li_sndio_cmd(&sl) || li_jack_cmd(&sl) ||
		    li_v4l2out_cmd(&sl) || li_udev_cmd(&sl))
			continue;
		fprintf(stderr, "script: line %d: unknown command '%s'\n", lineno, a[0]);
		return 2;
	}
	li_flush();
	fflush(stdout);
	_exit(0);
}
