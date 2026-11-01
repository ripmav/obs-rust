/*
 * Conformance runner for spec S-032 (audio mixer, faders, volmeters, source audio ingestion).
 * Links the real obs-audio.c / obs-audio-controls.c (#included so static helpers are reachable) and, through
 * gen/audiomix_stubs.c, the real obs-source.c. Script language: see test/conformance/README.md.
 */
#include <math.h>
#include <stdlib.h>
#include "harness.h"
#include "audiomix_api.h"

#include "obs-audio.c"
#include "obs-audio-controls.c"

#define MAXN 32
static obs_source_t *srcs[MAXN];
static char *names[MAXN];
static int nsrc;
static obs_fader_t *faders[MAXN];
static obs_volmeter_t *meters[MAXN];
static struct audio_output_data mixes[MAX_AUDIO_MIXES];
static float mixbuf[MAX_AUDIO_MIXES][MAX_AUDIO_CHANNELS][AUDIO_OUTPUT_FRAMES];
static uint64_t tl_start, tl_samples;
static uint32_t rate_g;

static void pf(float v)
{
	if (v != v)
		fputs("nan", stdout);
	else if (isinf(v))
		fputs(v < 0 ? "-inf" : "inf", stdout);
	else
		printf("%.9g", v == 0 ? 0.0f : v);
}

static obs_source_t *S(const char *n)
{
	if (!strcmp(n, "-"))
		return NULL;
	for (int i = 0; i < nsrc; i++)
		if (!strcmp(names[i], n))
			return srcs[i];
	fprintf(stderr, "unknown source %s\n", n);
	exit(2);
}

static const char *nm(obs_source_t *s)
{
	return s ? s->context.name : "-";
}

static const char *intern(const char *s)
{
	static char *tab[64];
	for (int i = 0; i < 64; i++) {
		if (!tab[i])
			tab[i] = strdup(s);
		if (!strcmp(tab[i], s))
			return tab[i];
	}
	abort();
}

static void gen_fill(float *out, int ch, uint32_t frames, const char *kind, double a, double b, double c)
{
	double scale = 1.0 - 0.25 * ch;
	for (uint32_t i = 0; i < frames; i++) {
		double v = 0;
		if (!strcmp(kind, "const"))
			v = a;
		else if (!strcmp(kind, "sine")) /* freq(Hz) amp startindex */
			v = b * sin(2.0 * M_PI * a * (c + i) / rate_g);
		else if (!strcmp(kind, "impulse")) /* index value */
			v = (i == (uint32_t)a) ? b : 0;
		else if (!strcmp(kind, "ramp")) /* step start */
			v = b + a * i;
		out[i] = (float)(v * scale);
	}
}

static void print_order(void)
{
	struct obs_core_audio *au = &obs->audio;
	printf("order");
	for (size_t i = 0; i < au->render_order.num; i++)
		printf(" %s%s", nm(au->render_order.array[i]), au->render_order.array[i]->audio_is_duplicated ? "*" : "");
	printf(" | roots");
	for (size_t i = 0; i < au->root_nodes.num; i++)
		printf(" %s", nm(au->root_nodes.array[i]));
	printf("\n");
}

static void print_mixes(size_t channels)
{
	for (int m = 0; m < MAX_AUDIO_MIXES; m++)
		for (size_t c = 0; c < channels; c++) {
			float *p = mixbuf[m][c], sum = 0, pk = 0;
			int first = -1, last = -1;
			for (int i = 0; i < AUDIO_OUTPUT_FRAMES; i++) {
				if (p[i] != 0) {
					if (first < 0)
						first = i;
					last = i;
				}
				sum += p[i];
				if (fabsf(p[i]) > pk)
					pk = fabsf(p[i]);
			}
			if (first < 0)
				continue;
			printf("mix %d %zu first=%d last=%d sum=", m, c, first, last);
			pf(sum);
			printf(" peak=");
			pf(pk);
			printf("\n");
		}
}

static void do_tick(uint64_t start, uint64_t end, uint32_t mixers, int idx)
{
	size_t channels = audio_output_get_channels(obs->audio.audio);
	uint64_t out_ts = 0;
	memset(mixbuf, 0, sizeof(mixbuf));
	for (int m = 0; m < MAX_AUDIO_MIXES; m++)
		for (size_t c = 0; c < channels; c++)
			mixes[m].data[c] = mixbuf[m][c];
	am_tick_start = start;
	bool ret = audio_callback(NULL, start, end, &out_ts, mixers, mixes);
	printf("tick %d ret=%d out_ts=%" PRIu64 " wait=%" PRIu64 " total=%d bufq=%zu\n", idx, ret, out_ts,
	       (uint64_t)obs->audio.buffering_wait_ticks, obs->audio.total_buffering_ticks,
	       obs->audio.buffered_timestamps.size / sizeof(struct ts_info));
	print_order();
	print_mixes(channels);
}

static void print_state(obs_source_t *s)
{
	printf("state %s audio_ts=%" PRIu64 " pending=%d buf=%zu last_buf=%zu pending_stop=%d timing_set=%d "
	       "timing_adjust=%" PRIu64 " next_ts_min=%" PRIu64 " next_sys_ts_min=%" PRIu64 " resample_offset=%" PRIu64
	       " failed=%d vol=",
	       nm(s), s->audio_ts, s->audio_pending, s->audio_input_buf[0].size / sizeof(float),
	       s->last_audio_input_buf_size / sizeof(float), s->pending_stop, s->timing_set,
	       (uint64_t)s->timing_adjust, s->next_audio_ts_min, s->next_audio_sys_ts_min, s->resample_offset,
	       s->audio_failed);
	pf(s->volume);
	printf(" user_vol=");
	pf(s->user_volume);
	printf(" muted=%d user_muted=%d actions=%zu\n", s->muted, s->user_muted, s->audio_actions.num);
}

static void fader_cb(void *p, float db)
{
	printf("fader_cb %s ", (char *)p);
	pf(db);
	printf("\n");
}

static void meter_cb(void *p, const float mag[8], const float peak[8], const float ip[8])
{
	printf("meter_cb %s", (char *)p);
	size_t n = 0;
	(void)n;
	printf(" mag");
	for (int i = 0; i < 8; i++) {
		printf(" ");
		pf(mag[i]);
	}
	printf(" peak");
	for (int i = 0; i < 8; i++) {
		printf(" ");
		pf(peak[i]);
	}
	printf(" ip");
	for (int i = 0; i < 8; i++) {
		printf(" ");
		pf(ip[i]);
	}
	printf("\n");
}

static void sig_halve(void *p, calldata_t *cd)
{
	(void)p;
	calldata_set_float(cd, "volume", calldata_float(cd, "volume") * 0.5);
}

static void task_cb(void *p)
{
	printf("task %s\n", (char *)p);
}

static void set_prop(obs_source_t *s, const char *k, const char *v)
{
	double d = strtod(v, NULL);
	long long i = strtoll(v, NULL, 0);
	if (!strcmp(k, "volume"))
		obs_source_set_volume(s, (float)d);
	else if (!strcmp(k, "mute"))
		obs_source_set_muted(s, i);
	else if (!strcmp(k, "balance"))
		obs_source_set_balance_value(s, (float)d);
	else if (!strcmp(k, "sync_offset"))
		obs_source_set_sync_offset(s, i);
	else if (!strcmp(k, "mixers"))
		obs_source_set_audio_mixers(s, (uint32_t)i);
	else if (!strcmp(k, "enabled"))
		s->enabled = i;
	else if (!strcmp(k, "active"))
		s->activate_refs = (int)i;
	else if (!strcmp(k, "removed"))
		s->removed = i;
	else if (!strcmp(k, "monitoring_enabled"))
		s->monitoring_enabled = i;
	else if (!strcmp(k, "monitoring_type"))
		s->monitoring_type = (enum obs_monitoring_type)i;
	else if (!strcmp(k, "force_mono"))
		s->flags = i ? (s->flags | OBS_SOURCE_FLAG_FORCE_MONO) : (s->flags & ~OBS_SOURCE_FLAG_FORCE_MONO);
	else if (!strcmp(k, "ptm"))
		obs_source_enable_push_to_mute(s, i);
	else if (!strcmp(k, "ptm_delay"))
		obs_source_set_push_to_mute_delay(s, (uint64_t)i);
	else if (!strcmp(k, "ptt"))
		obs_source_enable_push_to_talk(s, i);
	else if (!strcmp(k, "ptt_delay"))
		obs_source_set_push_to_talk_delay(s, (uint64_t)i);
	else if (!strcmp(k, "async_unbuffered"))
		s->async_unbuffered = i;
	else if (!strcmp(k, "async_decoupled"))
		s->async_decoupled = i;
	else if (!strcmp(k, "audio_ts"))
		s->audio_ts = (uint64_t)i;
	else if (!strcmp(k, "pending"))
		s->audio_pending = i;
	else {
		fprintf(stderr, "unknown prop %s\n", k);
		exit(2);
	}
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	while (script_next(f, &sl, &lineno)) {
		char **a = sl.argv;
		const char *cmd = a[0];
		if (!strcmp(cmd, "init")) {
			script_need(&sl, 3);
			rate_g = (uint32_t)strtoul(a[1], 0, 0);
			am_core_reset(rate_g, (enum speaker_layout)strtol(a[2], 0, 0));
			if (sl.argc > 3)
				obs->audio.max_buffering_ticks = atoi(a[3]);
			if (sl.argc > 4)
				obs->audio.fixed_buffer = atoi(a[4]);
			nsrc = 0;
		} else if (!strcmp(cmd, "clock")) {
			script_need(&sl, 2);
			am_set_clock(strtoull(a[1], 0, 0));
		} else if (!strcmp(cmd, "source")) {
			script_need(&sl, 3);
			static const char *kinds[] = {"input", "custom", "custom_audio", "submix", "filter"};
			int k = -1;
			for (int i = 0; i < 5; i++)
				if (!strcmp(a[2], kinds[i]))
					k = i;
			srcs[nsrc] = am_source_create(a[1], (enum am_kind)k);
			names[nsrc++] = strdup(a[1]);
		} else if (!strcmp(cmd, "custom")) { /* src ok ts|tick value */
			script_need(&sl, 5);
			am_cfg(S(a[1]), atoi(a[2]), !strcmp(a[3], "tick") ? AM_TS_TICK : strtoull(a[3], 0, 0),
			       (float)atof(a[4]), 1.0f, false);
		} else if (!strcmp(cmd, "child")) {
			script_need(&sl, 3);
			am_add_child(S(a[1]), S(a[2]));
		} else if (!strcmp(cmd, "filter")) { /* src filter gain drop */
			script_need(&sl, 5);
			am_add_filter(S(a[1]), S(a[2]));
			am_cfg(S(a[2]), true, 0, 0, (float)atof(a[3]), atoi(a[4]));
		} else if (!strcmp(cmd, "set")) {
			script_need(&sl, 4);
			set_prop(S(a[1]), a[2], a[3]);
		} else if (!strcmp(cmd, "hotkey")) {
			script_need(&sl, 4);
			if (!strcmp(a[2], "ptm"))
				am_hotkey_ptm(S(a[1]), atoi(a[3]));
			else
				am_hotkey_ptt(S(a[1]), atoi(a[3]));
		} else if (!strcmp(cmd, "video_mix")) {
			script_need(&sl, 2);
			am_add_video_mix(atoi(a[1]));
		} else if (!strcmp(cmd, "channel")) {
			script_need(&sl, 4);
			am_set_channel((size_t)atoi(a[1]), (uint32_t)atoi(a[2]), S(a[3]));
		} else if (!strcmp(cmd, "dup_source")) {
			script_need(&sl, 2);
			obs->audio.monitoring_duplicating_source = S(a[1]);
		} else if (!strcmp(cmd, "resampler")) { /* offset_ns fail */
			script_need(&sl, 3);
			am_resampler_ts_offset = strtoull(a[1], 0, 0);
			am_resampler_fail = atoi(a[2]);
		} else if (!strcmp(cmd, "output")) { /* src rate speakers frames ts gen a b c */
			script_need(&sl, 7);
			obs_source_t *s = S(a[1]);
			struct obs_source_audio au;
			memset(&au, 0, sizeof(au));
			au.samples_per_sec = (uint32_t)strtoul(a[2], 0, 0);
			au.speakers = (enum speaker_layout)atoi(a[3]);
			au.format = AUDIO_FORMAT_FLOAT_PLANAR;
			au.frames = (uint32_t)strtoul(a[4], 0, 0);
			au.timestamp = strtoull(a[5], 0, 0);
			size_t nch = get_audio_channels(au.speakers);
			float *bufs[MAX_AUDIO_CHANNELS];
			uint32_t fr = au.frames ? au.frames : 1;
			for (size_t c = 0; c < nch; c++) {
				bufs[c] = aligned_alloc(32, ((fr * 4 + 31) / 32) * 32);
				rate_g = rate_g ? rate_g : au.samples_per_sec;
				gen_fill(bufs[c], (int)c, au.frames, a[6], sl.argc > 7 ? atof(a[7]) : 0,
					 sl.argc > 8 ? atof(a[8]) : 0, sl.argc > 9 ? atof(a[9]) : 0);
				au.data[c] = (uint8_t *)bufs[c];
			}
			obs_source_output_audio(s, &au);
			for (size_t c = 0; c < nch; c++)
				free(bufs[c]);
		} else if (!strcmp(cmd, "timeline")) {
			script_need(&sl, 2);
			tl_start = strtoull(a[1], 0, 0);
			tl_samples = 0;
		} else if (!strcmp(cmd, "run")) {
			script_need(&sl, 3);
			int n = atoi(a[1]);
			uint32_t mixers = (uint32_t)strtoul(a[2], 0, 0);
			size_t rate = audio_output_get_sample_rate(obs->audio.audio);
			for (int i = 0; i < n; i++) {
				uint64_t prev = tl_start + audio_frames_to_ns(rate, tl_samples);
				tl_samples += AUDIO_OUTPUT_FRAMES;
				uint64_t next = tl_start + audio_frames_to_ns(rate, tl_samples);
				do_tick(prev, next, mixers, i);
			}
		} else if (!strcmp(cmd, "tick")) {
			script_need(&sl, 4);
			do_tick(strtoull(a[1], 0, 0), strtoull(a[2], 0, 0), (uint32_t)strtoul(a[3], 0, 0), 0);
		} else if (!strcmp(cmd, "dump_mix")) {
			script_need(&sl, 5);
			int m = atoi(a[1]), c = atoi(a[2]), from = atoi(a[3]), n = atoi(a[4]);
			printf("dump %d %d %d:", m, c, from);
			for (int i = from; i < from + n && i < AUDIO_OUTPUT_FRAMES; i++) {
				printf(" ");
				pf(mixbuf[m][c][i]);
			}
			printf("\n");
		} else if (!strcmp(cmd, "state")) {
			script_need(&sl, 2);
			print_state(S(a[1]));
		} else if (!strcmp(cmd, "task")) {
			script_need(&sl, 2);
			am_set_task(task_cb, strdup(a[1]));
		} else if (!strcmp(cmd, "sig_halve")) {
			script_need(&sl, 2);
			signal_handler_connect(obs_source_get_signal_handler(S(a[1])), "volume", sig_halve, NULL);
		} else if (!strcmp(cmd, "mixer_info")) { /* src: audio_pending / audio_ts via public API */
			script_need(&sl, 2);
			obs_source_t *s = S(a[1]);
			printf("info %s pending=%d ts=%" PRIu64 " mixers=%u balance=", nm(s), obs_source_audio_pending(s),
			       obs_source_get_audio_timestamp(s), obs_source_get_audio_mixers(s));
			pf(obs_source_get_balance_value(s));
			printf(" sync=%" PRId64 "\n", obs_source_get_sync_offset(s));
		} else if (!strcmp(cmd, "render")) { /* src mixers: call obs_source_audio_render directly */
			script_need(&sl, 3);
			obs_source_t *s = S(a[1]);
			size_t channels = audio_output_get_channels(obs->audio.audio);
			am_tick_start = strtoull(sl.argc > 3 ? a[3] : "0", 0, 0);
			obs_source_audio_render(s, (uint32_t)strtoul(a[2], 0, 0), channels,
						audio_output_get_sample_rate(obs->audio.audio),
						AUDIO_OUTPUT_FRAMES * sizeof(float));
			printf("render %s pending=%d ts=%" PRIu64 "\n", nm(s), s->audio_pending, s->audio_ts);
			for (int m = 0; m < MAX_AUDIO_MIXES; m++)
				for (size_t c = 0; c < channels; c++) {
					float *p = s->audio_output_buf[m][c], sum = 0, pk = 0;
					if (!p)
						continue;
					for (int i = 0; i < AUDIO_OUTPUT_FRAMES; i++) {
						sum += p[i];
						pk = fmaxf(pk, fabsf(p[i]));
					}
					if (pk != 0) {
						printf("out %d %zu sum=", m, c);
						pf(sum);
						printf(" peak=");
						pf(pk);
						printf(" s0=");
						pf(p[0]);
						printf("\n");
					}
				}
		} else if (!strcmp(cmd, "fader_new")) {
			script_need(&sl, 3);
			faders[atoi(a[1])] = obs_fader_create((enum obs_fader_type)atoi(a[2]));
			printf("fader_new %s %s\n", a[1], faders[atoi(a[1])] ? "ok" : "null");
		} else if (!strcmp(cmd, "fader_set")) { /* id db|def|mul value */
			script_need(&sl, 4);
			obs_fader_t *fd = faders[atoi(a[1])];
			float v = (float)atof(a[3]);
			bool r = !strcmp(a[2], "db")    ? obs_fader_set_db(fd, v)
				 : !strcmp(a[2], "def") ? obs_fader_set_deflection(fd, v)
							: obs_fader_set_mul(fd, v);
			printf("fader_set %s %s ret=%d\n", a[1], a[2], r);
		} else if (!strcmp(cmd, "fader_get")) {
			script_need(&sl, 2);
			obs_fader_t *fd = faders[atoi(a[1])];
			printf("fader_get %s db=", a[1]);
			pf(obs_fader_get_db(fd));
			printf(" def=");
			pf(obs_fader_get_deflection(fd));
			printf(" mul=");
			pf(obs_fader_get_mul(fd));
			printf("\n");
		} else if (!strcmp(cmd, "fader_null")) { /* NULL fader behaviour */
			printf("fader_null set_db=%d get_db=", obs_fader_set_db(NULL, 1.f));
			pf(obs_fader_get_db(NULL));
			printf(" def=");
			pf(obs_fader_get_deflection(NULL));
			printf(" mul=");
			pf(obs_fader_get_mul(NULL));
			printf(" attach=%d create_bad=%d\n", obs_fader_attach_source(NULL, NULL),
			       obs_fader_create((enum obs_fader_type)7) != NULL);
		} else if (!strcmp(cmd, "fader_conv")) { /* type def|db value : pure conversion */
			script_need(&sl, 4);
			obs_fader_t *fd = obs_fader_create((enum obs_fader_type)atoi(a[1]));
			float v = (float)atof(a[3]);
			float r = !strcmp(a[2], "def") ? fd->def_to_db(v) : obs_fader_db_to_def(fd)(v);
			printf("conv %s %s %s -> ", a[1], a[2], a[3]);
			pf(r);
			printf("\n");
			obs_fader_destroy(fd);
		} else if (!strcmp(cmd, "fader_attach")) {
			script_need(&sl, 3);
			bool r = obs_fader_attach_source(faders[atoi(a[1])], S(a[2]));
			printf("fader_attach ret=%d\n", r);
		} else if (!strcmp(cmd, "fader_detach")) {
			obs_fader_detach_source(faders[atoi(a[1])]);
		} else if (!strcmp(cmd, "fader_destroy")) {
			obs_fader_destroy(faders[atoi(a[1])]);
			faders[atoi(a[1])] = NULL;
		} else if (!strcmp(cmd, "fader_cb")) { /* id add|remove */
			obs_fader_t *fd = faders[atoi(a[1])];
			const char *tag = intern(sl.argc > 3 ? a[3] : a[1]);
			if (!strcmp(a[2], "add"))
				obs_fader_add_callback(fd, fader_cb, (void *)tag);
			else
				obs_fader_remove_callback(fd, fader_cb, (void *)tag);
		} else if (!strcmp(cmd, "meter_new")) {
			meters[atoi(a[1])] = obs_volmeter_create((enum obs_fader_type)atoi(a[2]));
		} else if (!strcmp(cmd, "meter_attach")) {
			printf("meter_attach ret=%d\n", obs_volmeter_attach_source(meters[atoi(a[1])], S(a[2])));
		} else if (!strcmp(cmd, "meter_detach")) {
			obs_volmeter_detach_source(meters[atoi(a[1])]);
		} else if (!strcmp(cmd, "meter_destroy")) {
			obs_volmeter_destroy(meters[atoi(a[1])]);
			meters[atoi(a[1])] = NULL;
		} else if (!strcmp(cmd, "meter_type")) {
			obs_volmeter_set_peak_meter_type(meters[atoi(a[1])], (enum obs_peak_meter_type)atoi(a[2]));
		} else if (!strcmp(cmd, "meter_channels")) {
			printf("meter_channels %d\n", obs_volmeter_get_nr_channels(meters[atoi(a[1])]));
		} else if (!strcmp(cmd, "meter_cb")) {
			const char *tag = intern(sl.argc > 3 ? a[3] : a[1]);
			if (!strcmp(a[2], "add"))
				obs_volmeter_add_callback(meters[atoi(a[1])], meter_cb, (void *)tag);
			else
				obs_volmeter_remove_callback(meters[atoi(a[1])], meter_cb, (void *)tag);
		} else if (!strcmp(cmd, "meter_feed")) {
			/* id src muted nplanes frames gen a b c : direct call of the audio-capture callback */
			script_need(&sl, 7);
			struct audio_data d;
			memset(&d, 0, sizeof(d));
			d.frames = (uint32_t)atoi(a[5]);
			int np = atoi(a[4]);
			float *bufs[8] = {0};
			uint32_t fr = d.frames ? d.frames : 1;
			for (int c = 0; c < np; c++) {
				bufs[c] = aligned_alloc(32, ((fr * 4 + 31) / 32) * 32 + 32);
				gen_fill(bufs[c], c, d.frames, a[6], sl.argc > 7 ? atof(a[7]) : 0,
					 sl.argc > 8 ? atof(a[8]) : 0, sl.argc > 9 ? atof(a[9]) : 0);
				d.data[c] = (uint8_t *)bufs[c];
			}
			volmeter_source_data_received(meters[atoi(a[1])], S(a[2]), &d, atoi(a[3]));
			for (int c = 0; c < np; c++)
				free(bufs[c]);
		} else if (!strcmp(cmd, "meter_misaligned")) {
			/* id src : one plane at +4 bytes (not 16-aligned) */
			struct audio_data d;
			memset(&d, 0, sizeof(d));
			d.frames = 8;
			float *buf = aligned_alloc(32, 64);
			memset(buf, 0, 64);
			d.data[0] = (uint8_t *)(buf + 1);
			FILE *saved = stdout;
			fflush(stdout);
			stdout = fopen("/dev/null", "w"); /* the reference prints a pointer; keep it out of the vector */
			volmeter_source_data_received(meters[atoi(a[1])], S(a[2]), &d, 0);
			fclose(stdout);
			stdout = saved;
			free(buf);
		} else if (!strcmp(cmd, "db")) { /* mul_to_db / db_to_mul via the exported wrappers */
			script_need(&sl, 3);
			float v = (float)atof(a[2]);
			printf("%s %s -> ", a[1], a[2]);
			pf(!strcmp(a[1], "mul_to_db") ? obs_mul_to_db(v) : obs_db_to_mul(v));
			printf("\n");
		} else {
			fprintf(stderr, "script: line %d: unknown command %s\n", sl.lineno, cmd);
			return 2;
		}
		fflush(stdout);
	}
	return 0;
}
