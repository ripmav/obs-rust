/* Conformance wrapper (area linuxcapture): includes the real linux-pulseaudio input source against fake pulse wrapper/stream. */
#include "../../../plugins/linux-pulseaudio/pulse-input.c"
#include "linuxcapture_api.h"

void lc_pa_deliver(void);
int lc_pa_inited(void);
static void *g_ps;
#define IS(s) (!strcmp(a[0], s))

static const char *afmt(enum audio_format f)
{
	switch (f) {
	case AUDIO_FORMAT_UNKNOWN: return "UNKNOWN";
	case AUDIO_FORMAT_U8BIT: return "U8BIT";
	case AUDIO_FORMAT_16BIT: return "16BIT";
	case AUDIO_FORMAT_32BIT: return "32BIT";
	case AUDIO_FORMAT_FLOAT: return "FLOAT";
	default: return "OTHER";
	}
}
static const char *spk(enum speaker_layout s)
{
	switch (s) {
	case SPEAKERS_UNKNOWN: return "UNKNOWN";
	case SPEAKERS_MONO: return "MONO";
	case SPEAKERS_STEREO: return "STEREO";
	case SPEAKERS_2POINT1: return "2.1";
	case SPEAKERS_4POINT0: return "4.0";
	case SPEAKERS_4POINT1: return "4.1";
	case SPEAKERS_5POINT1: return "5.1";
	case SPEAKERS_7POINT1: return "7.1";
	default: return "OTHER";
	}
}
static void state(void)
{
	struct pulse_data *d = g_ps;
	printf("state device=%s is_default=%d input=%d stream=%d speakers=%s format=%s rate=%u bytes_per_frame=%u channels=%u first_ts=%llu packets=%u frames=%llu\n",
	       d->device ? d->device : "(null)", d->is_default, d->input, d->stream != NULL, spk(d->speakers),
	       pa_sample_format_to_string(d->format) ? pa_sample_format_to_string(d->format) : "invalid", (unsigned)d->samples_per_sec,
	       (unsigned)d->bytes_per_frame, (unsigned)d->channels, (unsigned long long)d->first_ts, (unsigned)d->packets,
	       (unsigned long long)d->frames);
}

int lc_pulse_cmd(struct script_line *sl)
{
	char **a = sl->argv;
	if (IS("pulse-fmt")) { script_need(sl, 2); pa_sample_format_t f = pa_parse_sample_format(a[1]); printf("pulse-fmt %s -> %s\n", a[1], afmt(pulse_to_obs_audio_format(f))); return 1; }
	if (IS("pulse-fmt-num")) { script_need(sl, 2); printf("pulse-fmt-num %s -> %s\n", a[1], afmt(pulse_to_obs_audio_format(atoi(a[1])))); return 1; }
	if (IS("pulse-ch")) { script_need(sl, 2); printf("pulse-ch %s -> %s\n", a[1], spk(pulse_channels_to_obs_speakers(atoi(a[1])))); return 1; }
	if (IS("pulse-chmap")) {
		script_need(sl, 2);
		enum speaker_layout l = (enum speaker_layout)atoi(a[1]);
		pa_channel_map m = pulse_channel_map(l);
		printf("pulse-chmap %s channels=%u:", spk(l), m.channels);
		for (unsigned i = 0; i < 8; i++) printf(" %s", i < m.channels ? pa_channel_position_to_string(m.map[i]) : "-");
		printf("\n");
		return 1;
	}
	if (IS("pulse-timecalc")) { script_need(sl, 3); printf("samples_to_ns %llu\n", (unsigned long long)samples_to_ns(strtoull(a[1], NULL, 0), atoi(a[2]))); return 1; }
	if (IS("pulse-info")) {
		const struct obs_source_info *i = !strcmp(a[1], "input") ? &pulse_input_capture : &pulse_output_capture;
		printf("info id=%s name=%s audio=%d do_not_duplicate=%d no_self_monitor=%d\n", i->id, i->get_name(NULL),
		       !!(i->output_flags & OBS_SOURCE_AUDIO), !!(i->output_flags & OBS_SOURCE_DO_NOT_DUPLICATE),
		       !!(i->output_flags & OBS_SOURCE_DO_NOT_SELF_MONITOR));
		return 1;
	}
	if (IS("pulse-defaults")) { pulse_input_capture.get_defaults(lc_settings); return 1; }
	if (IS("pulse-create")) {
		script_need(sl, 2);
		const struct obs_source_info *i = !strcmp(a[1], "input") ? &pulse_input_capture : &pulse_output_capture;
		g_ps = i->create(lc_settings, (obs_source_t *)(uintptr_t)1);
		state();
		return 1;
	}
	if (IS("pulse-update")) { pulse_update(g_ps, lc_settings); state(); return 1; }
	if (IS("pulse-destroy")) { pulse_destroy(g_ps); g_ps = NULL; printf("pulse-destroy refs=%d\n", lc_pa_inited()); return 1; }
	if (IS("pulse-props")) {
		script_need(sl, 2);
		if (lc_props) obs_properties_destroy(lc_props);
		lc_props = (!strcmp(a[1], "input") ? pulse_input_capture : pulse_output_capture).get_properties(NULL);
		return 1;
	}
	if (IS("pulse-read")) { lc_pa_deliver(); state(); return 1; }
	if (IS("pulse-state")) { state(); return 1; }
	if (IS("pulse-read-nostream")) { struct pulse_data *d = g_ps; pa_stream *s = d->stream; d->stream = NULL; pulse_stream_read(NULL, 0, d); d->stream = s; printf("read-nostream ok\n"); return 1; }
	return 0;
}
