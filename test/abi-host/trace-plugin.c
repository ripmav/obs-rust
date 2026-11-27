/* WP-40 ABI conformance plugin (P0-15 + P1-27.5).
 *
 * Registers one object of each kind (source/encoder/output/service) and
 * logs every callback through the single abi_trace() channel (abitrace.h).
 * The source's get_properties builds EVERY property kind (S-015 §5):
 * bool, int, int slider, float, float slider, text (+types, +info word
 * wrap), path (+filter), list (string/int/float/bool, incl. disabled
 * items), editable list, font, color, color alpha, frame rate (options +
 * fps ranges), button (URL type), group (normal + checkable, with
 * children). Every property has a DISTINCT id; the host feeds non-default
 * values so the round-trips are observable in the trace.
 *
 * The plugin is ALWAYS compiled as C. What differs between the two host
 * builds is the libobs implementation behind these callbacks (the
 * OBS_ENABLE_RUST swap set). Byte-identical traces are the proof.
 */
#include <obs-module.h>
#include <obs-encoder.h>
#include <obs-output.h>
#include <obs-service.h>
#include <obs-source.h>
#include "abitrace.h"

#define OBS_MODULE_NAME "abi-trace"
#define OBS_MODULE_AUTHOR "WP-40"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE(OBS_MODULE_NAME, "en-US")

MODULE_EXPORT const char *obs_module_name(void)
{
	return OBS_MODULE_NAME;
}

/* Trace lines must stay single-line (normalisation rule): render raw
 * newlines as the two literal chars backslash-n. Single static buffer —
 * calls are synchronous and each line is flushed before the next. */
static const char *trace_str(const char *s)
{
	static char buf[512];
	size_t o = 0;

	if (!s)
		s = "";
	for (size_t i = 0; s[i] && o < sizeof(buf) - 4; i++) {
		if (s[i] == '\n') {
			buf[o++] = '\\';
			buf[o++] = 'n';
		} else {
			buf[o++] = s[i];
		}
	}
	buf[o] = '\0';
	return buf;
}

/* ------------------------------------------------------------------ */
/* source: abi_trace (synchronous input)                               */
/* ------------------------------------------------------------------ */

struct abi_src_data {
	int ticks;
	int renders;
};

static const char *abi_src_get_name(void *type_data)
{
	(void)type_data;
	return "ABI trace"; /* required in 33.0.1 (obs_register_source checks it) */
}

static void *abi_src_create(obs_data_t *settings, obs_source_t *source)
{
	(void)source;
	struct abi_src_data *d = bmalloc(sizeof(*d));
	d->ticks = 0;
	d->renders = 0;

	/* Value round-trip #1: the settings the host passed must come back
	 * through the data layer (obs_data, C in both builds). */
	abi_trace("source", "create", "id=abi_trace");
	abi_trace("source", "create-value", "key=abi_bool value=%d", obs_data_get_bool(settings, "abi_bool"));
	abi_trace("source", "create-value", "key=abi_int value=%lld", obs_data_get_int(settings, "abi_int"));
	abi_trace("source", "create-value", "key=abi_int_slider value=%lld",
		  obs_data_get_int(settings, "abi_int_slider"));
	abi_trace("source", "create-value", "key=abi_float value=%.9g", obs_data_get_double(settings, "abi_float"));
	abi_trace("source", "create-value", "key=abi_float_slider value=%.9g",
		  obs_data_get_double(settings, "abi_float_slider"));
	abi_trace("source", "create-value", "key=abi_text value=%s", trace_str(obs_data_get_string(settings, "abi_text")));
	abi_trace("source", "create-value", "key=abi_text_pw value=%s", trace_str(obs_data_get_string(settings, "abi_text_pw")));
	abi_trace("source", "create-value", "key=abi_text_ml value=%s", trace_str(obs_data_get_string(settings, "abi_text_ml")));
	abi_trace("source", "create-value", "key=abi_text_info value=%s",
		  trace_str(obs_data_get_string(settings, "abi_text_info")));
	abi_trace("source", "create-value", "key=abi_path_file value=%s",
		  obs_data_get_string(settings, "abi_path_file"));
	abi_trace("source", "create-value", "key=abi_path_dir value=%s", obs_data_get_string(settings, "abi_path_dir"));
	abi_trace("source", "create-value", "key=abi_list_str value=%s", obs_data_get_string(settings, "abi_list_str"));
	abi_trace("source", "create-value", "key=abi_list_int value=%lld", obs_data_get_int(settings, "abi_list_int"));
	abi_trace("source", "create-value", "key=abi_list_float value=%.9g",
		  obs_data_get_double(settings, "abi_list_float"));
	abi_trace("source", "create-value", "key=abi_list_bool value=%d", obs_data_get_bool(settings, "abi_list_bool"));
	abi_trace("source", "create-value", "key=abi_editable value=%s", trace_str(obs_data_get_string(settings, "abi_editable")));
	{
		obs_data_t *font = obs_data_get_obj(settings, "abi_font");
		if (font) {
			/* size/flags only: the C source-creation path rewrites the
			 * font face/style strings from the platform's font list
			 * (fontconfig), so they are environment-dependent and are
			 * excluded from the trace like pointers and paths */
			abi_trace("source", "create-value",
				  "key=abi_font size=%lld flags=%lld",
				  obs_data_get_int(font, "size"), obs_data_get_int(font, "flags"));
			obs_data_release(font);
		}
	}
	abi_trace("source", "create-value", "key=abi_color value=%llx",
		  (unsigned long long)obs_data_get_int(settings, "abi_color"));
	abi_trace("source", "create-value", "key=abi_color_alpha value=%llx",
		  (unsigned long long)obs_data_get_int(settings, "abi_color_alpha"));
	abi_trace("source", "create-value", "key=abi_group_child value=%lld",
		  obs_data_get_int(settings, "abi_group_child"));
	abi_trace("source", "create-value", "key=abi_group_check_child value=%lld",
		  obs_data_get_int(settings, "abi_group_check_child"));
	return d;
}

static void abi_src_destroy(void *data)
{
	struct abi_src_data *d = data;
	abi_trace("source", "destroy", "ticks=%d renders=%d", d->ticks, d->renders);
	bfree(d);
}

static uint32_t abi_src_width(void *data)
{
	(void)data;
	return 128;
}

static uint32_t abi_src_height(void *data)
{
	(void)data;
	return 72;
}

static void abi_src_update(void *data, obs_data_t *settings)
{
	(void)data;
	/* Value round-trip #2: updated settings come back through the same
	 * data layer (two distinct updates are driven by the host). */
	abi_trace("source", "update-value", "key=abi_int value=%lld", obs_data_get_int(settings, "abi_int"));
	abi_trace("source", "update-value", "key=abi_float value=%.9g", obs_data_get_double(settings, "abi_float"));
	abi_trace("source", "update-value", "key=abi_text value=%s", trace_str(obs_data_get_string(settings, "abi_text")));
	abi_trace("source", "update-value", "key=abi_bool value=%d", obs_data_get_bool(settings, "abi_bool"));
}

static void abi_src_tick(void *data, float seconds)
{
	struct abi_src_data *d = data;
	d->ticks++;
	abi_trace("source", "tick", "i=%d seconds=%.6f", d->ticks, (double)seconds);
}

static void abi_src_render(void *data, gs_effect_t *effect)
{
	struct abi_src_data *d = data;
	(void)effect; /* headless: no graphics, the trace records entry only */
	d->renders++;
	abi_trace("source", "render", "i=%d", d->renders);
}

/* --- property callbacks (P1-27.4 reachability) --- */

/* modified callback shared by all value-carrying properties: logs the value
 * the host's settings carry (round-trip #3: settings -> callback). */
static bool abi_prop_modified(obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	(void)props;
	const char *name = obs_property_name(p);
	switch (obs_property_get_type(p)) {
	case OBS_PROPERTY_BOOL:
		abi_trace("prop", "modified", "name=%s value=%d", name, obs_data_get_bool(settings, name));
		break;
	case OBS_PROPERTY_INT:
		abi_trace("prop", "modified", "name=%s value=%lld", name, obs_data_get_int(settings, name));
		break;
	case OBS_PROPERTY_FLOAT:
		abi_trace("prop", "modified", "name=%s value=%.9g", name, obs_data_get_double(settings, name));
		break;
	case OBS_PROPERTY_TEXT:
		abi_trace("prop", "modified", "name=%s value=%s", name, obs_data_get_string(settings, name));
		break;
	case OBS_PROPERTY_COLOR:
		abi_trace("prop", "modified", "name=%s value=%llx", name, (unsigned long long)obs_data_get_int(settings, name));
		break;
	case OBS_PROPERTY_LIST:
		abi_trace("prop", "modified", "name=%s value=%s", name, obs_data_get_string(settings, name));
		break;
	default:
		return false;
	}
	return false;
}

/* modified2 (priv variant) on one property: priv is never traced (pointer). */
static bool abi_prop_modified2(void *priv, obs_properties_t *props, obs_property_t *p, obs_data_t *settings)
{
	(void)priv;
	(void)props;
	abi_trace("prop", "modified2", "name=%s value=%d", obs_property_name(p), obs_data_get_bool(settings, "abi_bool"));
	return false;
}

static bool abi_button_clicked(obs_properties_t *props, obs_property_t *p, void *priv)
{
	(void)props;
	(void)priv;
	abi_trace("prop", "clicked", "name=%s", obs_property_name(p));
	return false;
}

/* Builds EVERY S-015 §5 property kind. Returns a fresh tree on every call. */
static obs_properties_t *abi_src_props(void *data)
{
	(void)data;
	obs_properties_t *props = obs_properties_create();
	obs_property_t *p;

	abi_trace("source", "props-build", NULL);

	p = obs_properties_add_bool(props, "abi_bool", obs_module_text("Abi bool"));
	obs_property_set_modified_callback(p, abi_prop_modified);

	p = obs_properties_add_int(props, "abi_int", obs_module_text("Abi int"), 0, 100, 1);
	obs_property_int_set_suffix(p, "x");
	obs_property_set_modified_callback(p, abi_prop_modified);

	p = obs_properties_add_int_slider(props, "abi_int_slider", "Abi int slider", 0, 100, 1);
	obs_property_set_enabled(p, false); /* one disabled property (attribute observable) */

	p = obs_properties_add_float(props, "abi_float", "Abi float", 0.0, 10.0, 0.25);
	obs_property_set_modified_callback(p, abi_prop_modified);

	p = obs_properties_add_float_slider(props, "abi_float_slider", "Abi float slider", 0.0, 10.0, 0.25);

	p = obs_properties_add_text(props, "abi_text", "Abi text", OBS_TEXT_DEFAULT);
	obs_property_set_modified_callback(p, abi_prop_modified);

	p = obs_properties_add_text(props, "abi_text_pw", "Abi text pw", OBS_TEXT_PASSWORD);
	obs_property_text_set_monospace(p, true);

	p = obs_properties_add_text(props, "abi_text_ml", "Abi text ml", OBS_TEXT_MULTILINE);

	p = obs_properties_add_text(props, "abi_text_info", "Abi text info", OBS_TEXT_INFO);
	obs_property_text_set_info_type(p, OBS_TEXT_INFO_WARNING);
	obs_property_text_set_info_word_wrap(p, true); /* the "word" (word-wrap) kind */
	obs_property_set_visible(p, false); /* one hidden property (attribute observable) */

	p = obs_properties_add_path(props, "abi_path_file", "Abi path file", OBS_PATH_FILE,
				    "Images (*.png *.jpg);;All files (*.*)", "/tmp/abi/f.txt");
	p = obs_properties_add_path(props, "abi_path_dir", "Abi path dir", OBS_PATH_DIRECTORY, NULL, "/tmp/abi/d");

	p = obs_properties_add_list(props, "abi_list_str", "Abi list str", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_STRING);
	obs_property_list_add_string(p, "optA", "a");
	obs_property_list_add_string(p, "optB", "b");
	obs_property_list_add_string(p, "optC", "c");
	obs_property_list_add_string(p, "optX", "x");
	obs_property_list_item_disable(p, 3, true); /* one disabled item */
	obs_property_set_modified_callback(p, abi_prop_modified);

	p = obs_properties_add_list(props, "abi_list_int", "Abi list int", OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(p, "ten", 10);
	obs_property_list_add_int(p, "hundred", 100);
	obs_property_list_add_int(p, "two-hundred-two", 202);
	obs_property_set_modified_callback(p, abi_prop_modified);

	p = obs_properties_add_list(props, "abi_list_float", "Abi list float", OBS_COMBO_TYPE_LIST,
				    OBS_COMBO_FORMAT_FLOAT);
	obs_property_list_add_float(p, "one", 1.0);
	obs_property_list_add_float(p, "seven-point-two-five", 7.25);
	obs_property_set_modified_callback(p, abi_prop_modified);

	p = obs_properties_add_list(props, "abi_list_bool", "Abi list bool", OBS_COMBO_TYPE_RADIO, OBS_COMBO_FORMAT_BOOL);
	obs_property_list_add_bool(p, "yes", true);
	obs_property_list_add_bool(p, "no", false);
	obs_property_set_modified_callback(p, abi_prop_modified);

	p = obs_properties_add_editable_list(props, "abi_editable", "Abi editable list",
					     OBS_EDITABLE_LIST_TYPE_STRINGS, "*.txt", "/tmp/abi");

	p = obs_properties_add_font(props, "abi_font", "Abi font");

	p = obs_properties_add_color(props, "abi_color", "Abi color");
	obs_property_set_modified_callback(p, abi_prop_modified);

	p = obs_properties_add_color_alpha(props, "abi_color_alpha", "Abi color alpha");

	p = obs_properties_add_frame_rate(props, "abi_frame_rate", "Abi frame rate");
	obs_property_frame_rate_option_add(p, "30 fps", "3000");
	obs_property_frame_rate_option_add(p, "60 fps", "6000");
	{
		struct media_frames_per_second fmin = { 24, 1 };
		struct media_frames_per_second fmax = { 120, 1 };
		obs_property_frame_rate_fps_range_add(p, fmin, fmax);
	}

	p = obs_properties_add_button2(props, "abi_button", "Abi button", abi_button_clicked, NULL);
	obs_property_button_set_type(p, OBS_BUTTON_URL);
	obs_property_button_set_url(p, (char *)"https://obsproject.com/");

	/* group (normal): children live in their own container */
	{
		obs_properties_t *group = obs_properties_create();
		obs_property_t *c = obs_properties_add_int(group, "abi_group_child", "Group child", 0, 1000, 1);
		obs_property_set_modified_callback(c, abi_prop_modified);
		p = obs_properties_add_group(props, "abi_group", "Abi group", OBS_GROUP_NORMAL, group);
	}

	/* group (checkable) */
	{
		obs_properties_t *group = obs_properties_create();
		obs_property_t *c = obs_properties_add_int(group, "abi_group_check_child", "Group check child", 0, 1000, 1);
		obs_property_set_modified_callback2(c, abi_prop_modified2, (void *)0x1);
		p = obs_properties_add_group(props, "abi_group_check", "Abi group check", OBS_GROUP_CHECKABLE, group);
	}

	abi_trace("source", "props-built", NULL);
	return props;
}

static struct obs_source_info abi_src_info = {
	.id = "abi_trace",
	.type = OBS_SOURCE_TYPE_INPUT,
	/* OBS_SOURCE_VIDEO: render_video() requires it for input sources in
	 * 33.0.1; OBS_SOURCE_CUSTOM_DRAW: route to the plugin's video_render
	 * (NULL effect) instead of the default render path */
	.output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW,
	.get_name = abi_src_get_name,
	.create = abi_src_create,
	.destroy = abi_src_destroy,
	.get_width = abi_src_width,
	.get_height = abi_src_height,
	.get_properties = abi_src_props,
	.update = abi_src_update,
	.video_tick = abi_src_tick,
	.video_render = abi_src_render,
};

/* ------------------------------------------------------------------ */
/* encoder: abi_enc (dummy video encoder)                              */
/* ------------------------------------------------------------------ */

static const char *abi_enc_name(void *type_data)
{
	(void)type_data;
	return "ABI encoder";
}

static void *abi_enc_create(obs_data_t *settings, obs_encoder_t *encoder)
{
	(void)encoder;
	abi_trace("enc", "create", "id=abi_enc bitrate=%lld keyint=%lld", obs_data_get_int(settings, "abi_bitrate"),
		  obs_data_get_int(settings, "abi_keyint"));
	return (void *)1;
}

static void abi_enc_destroy(void *data)
{
	(void)data;
	abi_trace("enc", "destroy", NULL);
}

static bool abi_enc_encode(void *data, struct encoder_frame *frame, struct encoder_packet *packet,
			   bool *received_packet)
{
	/* The harness never feeds frames (33.0.1 has no public frame-queue
	 * API — the video system owns queuing); logged only if reached. */
	(void)data;
	(void)frame;
	(void)packet;
	*received_packet = false;
	return false;
}

static bool abi_enc_update(void *data, obs_data_t *settings)
{
	(void)data;
	/* control: settings update (three distinct cycles driven by the host) */
	abi_trace("enc", "update", "bitrate=%lld", obs_data_get_int(settings, "abi_bitrate"));
	return true;
}

static bool abi_enc_extra_data(void *data, uint8_t **extra_data, size_t *size)
{
	(void)data;
	/* control: extra-data request (the host logs the returned size) */
	static const uint8_t marker[4] = { 0xAB, 0x41, 0x42, 0x43 };
	*extra_data = bmemdup(marker, sizeof(marker));
	*size = sizeof(marker);
	return true;
}

static struct obs_encoder_info abi_enc_info = {
	.id = "abi_enc",
	.type = OBS_ENCODER_VIDEO,
	.codec = "abi",
	.get_name = abi_enc_name,
	.create = abi_enc_create,
	.destroy = abi_enc_destroy,
	.encode = abi_enc_encode,
	.update = abi_enc_update,
	.get_extra_data = abi_enc_extra_data,
};

/* ------------------------------------------------------------------ */
/* output: abi_out (dummy)                                             */
/* ------------------------------------------------------------------ */

static const char *abi_out_name(void *type_data)
{
	(void)type_data;
	return "ABI output";
}

static void *abi_out_create(obs_data_t *settings, obs_output_t *output)
{
	(void)settings;
	(void)output;
	abi_trace("out", "create", "id=abi_out");
	return (void *)1;
}

static void abi_out_destroy(void *data)
{
	(void)data;
	abi_trace("out", "destroy", NULL);
}

static bool abi_out_start(void *data)
{
	(void)data;
	abi_trace("out", "start", NULL);
	return true;
}

static void abi_out_stop(void *data, uint64_t ts)
{
	(void)data;
	/* the ts is a wall-clock time — never traced (normalisation rule) */
	abi_trace("out", "stop", NULL);
}

static void abi_out_encoded_packet(void *data, struct encoder_packet *packet)
{
	/* required for OBS_OUTPUT_ENCODED registration; the harness never
	 * attaches an encoder, so this is never called */
	(void)data;
	(void)packet;
}

static struct obs_output_info abi_out_info = {
	.id = "abi_out",
	/* OBS_OUTPUT_ENCODED: skips obs_get_video()/obs_get_audio() in
	 * obs_output_create, which crash headless (no video mix) */
	.flags = OBS_OUTPUT_ENCODED,
	.get_name = abi_out_name,
	.create = abi_out_create,
	.destroy = abi_out_destroy,
	.start = abi_out_start,
	.stop = abi_out_stop,
	.encoded_packet = abi_out_encoded_packet,
};

/* ------------------------------------------------------------------ */
/* service: abi_svc (dummy)                                            */
/* ------------------------------------------------------------------ */

static const char *abi_svc_name(void *type_data)
{
	(void)type_data;
	return "ABI service";
}

static void *abi_svc_create(obs_data_t *settings, obs_service_t *service)
{
	(void)settings;
	(void)service;
	abi_trace("svc", "create", "id=abi_svc");
	return (void *)1;
}

static void abi_svc_destroy(void *data)
{
	(void)data;
	abi_trace("svc", "destroy", NULL);
}

static const char *abi_svc_protocol(void *data)
{
	(void)data;
	return "abi"; /* required in 33.0.1 (obs_register_service checks it) */
}

static struct obs_service_info abi_svc_info = {
	.id = "abi_svc",
	.get_name = abi_svc_name,
	.create = abi_svc_create,
	.destroy = abi_svc_destroy,
	.get_protocol = abi_svc_protocol,
};

/* ------------------------------------------------------------------ */

bool obs_module_load(void)
{
	obs_register_source(&abi_src_info);
	obs_register_encoder(&abi_enc_info);
	obs_register_output(&abi_out_info);
	obs_register_service(&abi_svc_info);
	abi_trace("module", "load", NULL);
	return true;
}

void obs_module_unload(void)
{
	abi_trace("module", "unload", NULL);
}
