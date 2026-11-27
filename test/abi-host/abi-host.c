/* WP-40 ABI conformance host (P0-15).
 *
 * Loads the trace plugin (libtest-input.so, built from trace-plugin.c) with
 * obs_open_module/obs_init_module, creates ONE object of each kind, drives
 * the fixed workload and writes the normalised trace to argv[1].
 *
 *   abi-host <trace.txt> [plugin.so]
 *
 * The plugin path defaults to libtest-input.so next to the executable.
 *
 * Headless scope cut (documented): NO obs_reset_video. 33.0.1's
 * obs_init_graphics needs a real graphics module (libobs-opengl + a GL
 * context); the harness drives source ticks and renders directly
 * (obs_source_video_tick / obs_source_video_render) with gs_* stubbed, so
 * every callback that matters for the C-vs-Rust ABI identity is still
 * exercised. The gs_* no-op family must make gs_create FAIL, so no
 * graphics initialisation is attempted.
 *
 * Any failure: trace + stderr, exit 1.
 */
#include <obs.h>
#include <obs-encoder.h>
#include <obs-module.h>
#include <obs-properties.h>
#include <obs-service.h>
#include <obs-source.h>
#include <obs-internal.h>
#include <util/threading.h>
#include "abitrace.h"

#include <libgen.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

static void fail(const char *what)
{
	abi_trace("host", "fail", "stage=%s", what);
	fprintf(stderr, "abi-host: %s failed\n", what);
}

/* Recursively enumerate the property tree (group contents included). */
static int enum_props(obs_properties_t *props, const char *kind)
{
	int count = 0;
	obs_property_t *p;

	if (!props)
		return 0;
	p = obs_properties_first(props);
	while (p) {
		const char *name = obs_property_name(p);
		enum obs_property_type type = obs_property_get_type(p);

		abi_trace("prop", kind, "name=%s type=%d visible=%d enabled=%d", name, (int)type,
			  (int)obs_property_visible(p), (int)obs_property_enabled(p));
		count++;
		switch (type) {
		case OBS_PROPERTY_INT: {
			const char *suffix = obs_property_int_suffix(p);
			abi_trace("prop", kind, "name=%s min=%d max=%d step=%d ntype=%d suffix=%s", name,
				  obs_property_int_min(p), obs_property_int_max(p), obs_property_int_step(p),
				  (int)obs_property_int_type(p), suffix ? suffix : "");
			break;
		}
		case OBS_PROPERTY_FLOAT:
			abi_trace("prop", kind, "name=%s min=%.9g max=%.9g step=%.9g ntype=%d", name,
				  obs_property_float_min(p), obs_property_float_max(p), obs_property_float_step(p),
				  (int)obs_property_float_type(p));
			break;
		case OBS_PROPERTY_TEXT:
			abi_trace("prop", kind, "name=%s ttype=%d mono=%d itype=%d wrap=%d", name,
				  (int)obs_property_text_type(p), (int)obs_property_text_monospace(p),
				  (int)obs_property_text_info_type(p), (int)obs_property_text_info_word_wrap(p));
			break;
		case OBS_PROPERTY_PATH:
			abi_trace("prop", kind, "name=%s ptype=%d filter=%s default=%s", name,
				  (int)obs_property_path_type(p), obs_property_path_filter(p) ? obs_property_path_filter(p) : "",
				  obs_property_path_default_path(p) ? obs_property_path_default_path(p) : "");
			break;
		case OBS_PROPERTY_LIST: {
			size_t n = obs_property_list_item_count(p);
			abi_trace("prop", kind, "name=%s ltype=%d lformat=%d items=%zu", name,
				  (int)obs_property_list_type(p), (int)obs_property_list_format(p), n);
			for (size_t i = 0; i < n; i++) {
				switch (obs_property_list_format(p)) {
				case OBS_COMBO_FORMAT_STRING:
					abi_trace("prop", kind, "name=%s item=%zu name=%s value=%s disabled=%d", name, i,
						  obs_property_list_item_name(p, i), obs_property_list_item_string(p, i),
						  (int)obs_property_list_item_disabled(p, i));
					break;
				case OBS_COMBO_FORMAT_INT:
					abi_trace("prop", kind, "name=%s item=%zu name=%s value=%lld disabled=%d", name,
						  i, obs_property_list_item_name(p, i),
						  obs_property_list_item_int(p, i),
						  (int)obs_property_list_item_disabled(p, i));
					break;
				case OBS_COMBO_FORMAT_FLOAT:
					abi_trace("prop", kind, "name=%s item=%zu name=%s value=%.9g disabled=%d", name,
						  i, obs_property_list_item_name(p, i),
						  obs_property_list_item_float(p, i),
						  (int)obs_property_list_item_disabled(p, i));
					break;
				case OBS_COMBO_FORMAT_BOOL:
					abi_trace("prop", kind, "name=%s item=%zu name=%s value=%d disabled=%d", name,
						  i, obs_property_list_item_name(p, i),
						  obs_property_list_item_bool(p, i),
						  (int)obs_property_list_item_disabled(p, i));
					break;
				default:
					break;
				}
			}
			break;
		}
		case OBS_PROPERTY_EDITABLE_LIST:
			abi_trace("prop", kind, "name=%s etype=%d filter=%s default=%s", name,
				  (int)obs_property_editable_list_type(p),
				  obs_property_editable_list_filter(p) ? obs_property_editable_list_filter(p) : "",
				  obs_property_editable_list_default_path(p) ? obs_property_editable_list_default_path(p)
									     : "");
			break;
		case OBS_PROPERTY_FRAME_RATE: {
			size_t no = obs_property_frame_rate_options_count(p);
			size_t nr = obs_property_frame_rate_fps_ranges_count(p);
			abi_trace("prop", kind, "name=%s options=%zu ranges=%zu", name, no, nr);
			for (size_t i = 0; i < no; i++)
				abi_trace("prop", kind, "name=%s option=%zu name=%s desc=%s", name, i,
					  obs_property_frame_rate_option_name(p, i),
					  obs_property_frame_rate_option_description(p, i));
			for (size_t i = 0; i < nr; i++) {
				struct media_frames_per_second lo = obs_property_frame_rate_fps_range_min(p, i);
				struct media_frames_per_second hi = obs_property_frame_rate_fps_range_max(p, i);
				abi_trace("prop", kind, "name=%s range=%zu min=%u/%u max=%u/%u", name, i, lo.numerator,
					  lo.denominator, hi.numerator, hi.denominator);
			}
			break;
		}
		case OBS_PROPERTY_BUTTON:
			abi_trace("prop", kind, "name=%s btype=%d url=%s", name, (int)obs_property_button_type(p),
				  obs_property_button_url(p));
			break;
		case OBS_PROPERTY_GROUP:
			abi_trace("prop", kind, "name=%s gtype=%d", name, (int)obs_property_group_type(p));
			count += enum_props(obs_property_group_content(p), kind);
			break;
		default:
			break;
		}
		if (!obs_property_next(&p))
			break;
	}
	return count;
}

/* The host's settings: one DISTINCT non-default value per property id. */
static obs_data_t *make_source_settings(void)
{
	obs_data_t *s = obs_data_create();
	obs_data_set_bool(s, "abi_bool", true);
	obs_data_set_int(s, "abi_int", 42);
	obs_data_set_int(s, "abi_int_slider", 77);
	obs_data_set_double(s, "abi_float", 3.5);
	obs_data_set_double(s, "abi_float_slider", 1.25);
	obs_data_set_string(s, "abi_text", "plain");
	obs_data_set_string(s, "abi_text_pw", "secret");
	obs_data_set_string(s, "abi_text_ml", "line1\nline2");
	obs_data_set_string(s, "abi_text_info", "note");
	obs_data_set_string(s, "abi_path_file", "/tmp/abi/f.txt");
	obs_data_set_string(s, "abi_path_dir", "/tmp/abi/d");
	obs_data_set_string(s, "abi_list_str", "optB");
	obs_data_set_int(s, "abi_list_int", 202);
	obs_data_set_double(s, "abi_list_float", 7.25);
	obs_data_set_bool(s, "abi_list_bool", true);
	obs_data_set_string(s, "abi_editable", "alpha\nbeta");
	obs_data_t *font = obs_data_create();
	obs_data_set_string(font, "face", "Serif");
	obs_data_set_string(font, "style", "Bold");
	obs_data_set_int(font, "size", 14);
	obs_data_set_int(font, "flags", 1);
	obs_data_set_obj(s, "abi_font", font);
	obs_data_release(font);
	obs_data_set_int(s, "abi_color", 0xFF112233LL);
	obs_data_set_int(s, "abi_color_alpha", 0x44556677LL);
	obs_data_set_int(s, "abi_group_child", 99);
	obs_data_set_int(s, "abi_group_check_child", 5);
	return s;
}

int main(int argc, char **argv)
{
	if (argc < 2) {
		fprintf(stderr, "usage: %s <trace.txt> [plugin.so]\n", argv[0]);
		return 2;
	}
	const char *trace_path = argv[1];

	/* fresh trace file + trace channel for host and plugin */
	{
		FILE *tf = fopen(trace_path, "w");
		if (!tf) {
			fprintf(stderr, "abi-host: cannot open %s\n", trace_path);
			return 1;
		}
		fclose(tf);
	}
	if (setenv("ABI_TRACE", trace_path, 1) != 0) {
		fprintf(stderr, "abi-host: setenv failed\n");
		return 1;
	}

	/* plugin: libtest-input.so next to the executable (or argv[2]) */
	char plugin_path[4096];
	if (argc >= 3) {
		snprintf(plugin_path, sizeof(plugin_path), "%s", argv[2]);
	} else {
		char self[4096];
		ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
		if (n <= 0) {
			fprintf(stderr, "abi-host: readlink failed\n");
			return 1;
		}
		self[n] = '\0';
		snprintf(plugin_path, sizeof(plugin_path), "%s/libtest-input.so", dirname(self));
	}

	/* scratch dir for the module config (obs_startup) */
	char tmpdir[] = "/tmp/abi-host-XXXXXX";
	if (!mkdtemp(tmpdir)) {
		fprintf(stderr, "abi-host: mkdtemp failed\n");
		return 1;
	}

	abi_trace("host", "start", NULL);
	if (!obs_startup("en-US", tmpdir, NULL)) {
		fail("startup");
		return 1;
	}
	abi_trace("host", "startup", "locale=en-US");

	obs_module_t *m = NULL;
	if (obs_open_module(&m, plugin_path, NULL) != 0 || !m) {
		fail("open-module");
		return 1;
	}
	abi_trace("host", "open", "name=%s", obs_get_module_name(m));
	if (!obs_init_module(m)) {
		fail("init-module");
		return 1;
	}
	abi_trace("host", "module-load", "ok=1");

	/* --- source: create, 10x (tick + render), properties, 2x update,
	 * release --- */
	obs_data_t *settings = make_source_settings();
	obs_source_t *src = obs_source_create("abi_trace", "abi_trace", settings, NULL);
	if (!src) {
		fail("source-create");
		return 1;
	}
	for (int i = 0; i < 10; i++) {
		obs_source_video_tick(src, 0.016f);
		obs_source_video_render(src);
	}

	obs_properties_t *props = obs_source_properties(src);
	if (!props) {
		fail("source-properties");
		return 1;
	}
	abi_trace("source", "props-enum", NULL);
	int nprops = enum_props(props, "enum");
	abi_trace("source", "props-enumerated", "count=%d", nprops);

	/* value round-trip #3: settings values come back through the
	 * modified/modified2 callbacks (P1-27.4 machinery) */
	obs_properties_apply_settings(props, settings);
	obs_property_t *btn = obs_properties_get(props, "abi_button");
	obs_property_button_clicked(btn, NULL);

	obs_properties_destroy(props);
	abi_trace("source", "props-destroy", NULL);

	obs_source_update(src, settings);
	obs_data_set_int(settings, "abi_int", 43);
	obs_data_set_double(settings, "abi_float", 3.75);
	obs_data_set_string(settings, "abi_text", "plain-2");
	obs_data_set_bool(settings, "abi_bool", false);
	obs_source_update(src, settings);
	obs_source_release(src);
	obs_wait_for_destroy_queue();
	obs_data_release(settings);

	/* --- encoder: create with settings, 3x control (update), extra
	 * data, release. In 33.0.1 the encoder's create/update/extra_data/
	 * destroy callbacks fire inside obs_encoder_initialize() (which
	 * requires encoder->media) — there is no public frame-queue API
	 * (queuing is owned by the video system). Headless: no video_t
	 * exists, so the harness sets the internal `media` field to a
	 * sentinel; a video encoder with no scaling configured never
	 * dereferences it (maybe_set_up_gpu_rescale returns early, and the
	 * audio init path is skipped for video encoders). --- */
	obs_data_t *enc_settings = obs_data_create();
	obs_data_set_int(enc_settings, "abi_bitrate", 12345);
	obs_data_set_int(enc_settings, "abi_keyint", 48);
	struct obs_encoder *enc = obs_video_encoder_create("abi_enc", "abi_enc", enc_settings, NULL);
	obs_data_release(enc_settings);
	if (!enc) {
		fail("enc-create");
		return 1;
	}
	enc->media = (void *)1; /* sentinel, see above */
	if (!obs_encoder_initialize(enc)) {
		fail("enc-initialize");
		return 1;
	}
	long long bitrates[3] = { 11111, 22222, 33333 };
	for (int i = 0; i < 3; i++) {
		obs_data_t *u = obs_data_create();
		obs_data_set_int(u, "abi_bitrate", bitrates[i]);
		obs_encoder_update(enc, u);
		obs_data_release(u);
	}
	uint8_t *extra = NULL;
	size_t extra_size = 0;
	obs_encoder_get_extra_data(enc, &extra, &extra_size);
	abi_trace("enc", "extra-data", "size=%zu", extra_size);
	bfree(extra);
	obs_encoder_release(enc);
	obs_wait_for_destroy_queue();

	/* --- output: create, start, stop, release. For OBS_OUTPUT_ENCODED
	 * outputs 33.0.1 sets output->active only in the raw-data capture
	 * thread, which an encoded output without a video pipeline never
	 * starts — so the public obs_output_stop early-returns. The harness
	 * therefore also drives the internal obs_output_actual_stop(force)
	 * (obs-internal.h), which calls the plugin's stop callback. --- */
	obs_output_t *out = obs_output_create("abi_out", "abi_out", NULL, NULL);
	if (!out) {
		fail("out-create");
		return 1;
	}
	bool started = obs_output_start(out);
	abi_trace("out", "started", "ok=%d", (int)started);
	obs_output_stop(out);
	obs_output_actual_stop(out, true, 0);
	/* actual_stop resets stopping_event; in the normal flow the raw-data
	 * capture thread re-signals it on exit (obs_output_destroy waits on
	 * it). Headless encoded output has no capture thread — emulate the
	 * signal or the destroy wait blocks forever. */
	os_event_signal(((struct obs_output *)out)->stopping_event);
	obs_output_release(out);
	obs_wait_for_destroy_queue();

	/* --- service: create, release (33.0.1 has no public service
	 * tick/heartbeat API) --- */
	obs_service_t *svc = obs_service_create("abi_svc", "abi_svc", NULL, NULL);
	if (!svc) {
		fail("svc-create");
		return 1;
	}
	obs_service_release(svc);
	obs_wait_for_destroy_queue();

	/* module unload happens in obs_shutdown; the destroy queue is
	 * drained there, so the plugin's destroy/unload lines land before
	 * the shutdown line below */
	obs_shutdown();
	abi_trace("host", "shutdown", NULL);

	/* clean up the scratch dir (best effort) */
	{
		char path[4300];
		snprintf(path, sizeof(path), "%s/obs-module.ini", tmpdir);
		remove(path);
		rmdir(tmpdir);
	}
	abi_trace("host", "done", NULL);
	return 0;
}
