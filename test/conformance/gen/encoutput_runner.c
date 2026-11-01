/*
 * S-041 runner: drives the real obs-encoder.c / obs-output.c / obs-output-delay.c through a fake core
 * (encoutput_stubs.c, encoutput_out.c) with scripted fake encoder and output plugins.
 * Script language: see test/conformance/README.md, section "encoutput runner (spec S-041)".
 */
#include <inttypes.h>
#include <signal.h>
#include <time.h>
#include <unistd.h>
#include "harness.h"
#include "encoutput_api.h"
#include "util/threading.h"

/* ------------------------------------------------------------ plugin configuration tables */

struct ecfg {
	char name[32];
	bool video;
	int kf;          /* keyframe every kf-th packet; 0 = only the first */
	int dts_delay;   /* dts = pts - dts_delay * step */
	int hold;        /* first `hold` encode calls produce no packet */
	int fail_at;     /* encode call number (1-based) that fails; 0 = never */
	size_t size;     /* packet payload size */
	uint8_t sei[16]; /* get_sei_data result */
	size_t sei_len;
	size_t frame_size; /* audio only */
	bool create_fail;
	int calls;
	uint32_t caps;
	uint8_t buf[256];
	struct obs_encoder *enc;
};

struct ocfg {
	char name[32];
	uint32_t flags;
	bool start_ok, autobegin, autoend;
	int fail_code; /* if start fails: report this code through obs_output_signal_stop (0 = do not) */
	volatile long start_calls;
	struct obs_output *out;
};

static struct ecfg ecfgs[16];
static int necfg;
static struct ocfg ocfgs[16];
static int nocfg;

static struct ecfg *find_ecfg(const char *name)
{
	for (int i = 0; i < necfg; i++)
		if (!strcmp(ecfgs[i].name, name))
			return &ecfgs[i];
	return NULL;
}
static struct ocfg *find_ocfg(const char *name)
{
	for (int i = 0; i < nocfg; i++)
		if (!strcmp(ocfgs[i].name, name))
			return &ocfgs[i];
	return NULL;
}

/* ------------------------------------------------------------ fake encoder plugin */

static void *fenc_create(obs_data_t *s, obs_encoder_t *e)
{
	(void)s;
	struct ecfg *c = find_ecfg(e->context.name);
	if (!c || c->create_fail) {
		printf("plugin enc %s create fail\n", e->context.name);
		return NULL;
	}
	printf("plugin enc %s create\n", c->name);
	c->calls = 0;
	return c;
}
static void fenc_destroy(void *d)
{
	printf("plugin enc %s destroy\n", ((struct ecfg *)d)->name);
}
static size_t fenc_frame_size(void *d)
{
	return ((struct ecfg *)d)->frame_size;
}
static bool fenc_sei(void *d, uint8_t **sei, size_t *size)
{
	struct ecfg *c = d;
	if (!c->sei_len)
		return false;
	*sei = c->sei;
	*size = c->sei_len;
	return true;
}
static bool fenc_encode(void *d, struct encoder_frame *f, struct encoder_packet *pkt, bool *received)
{
	struct ecfg *c = d;
	c->calls++;
	if (c->fail_at && c->calls == c->fail_at)
		return false;
	if (c->calls <= c->hold) {
		*received = false;
		return true;
	}
	int64_t step;
	if (c->video) {
		step = pkt->timebase_num; /* preset by do_encode */
	} else {
		step = (int64_t)c->frame_size;
		pkt->timebase_num = 1;
		pkt->timebase_den = c->enc->timebase_den;
	}
	pkt->type = c->video ? OBS_ENCODER_VIDEO : OBS_ENCODER_AUDIO;
	pkt->pts = f->pts - c->hold * step;
	pkt->dts = pkt->pts - c->dts_delay * step;
	pkt->keyframe = c->video && (c->kf ? ((c->calls - c->hold - 1) % c->kf == 0) : (c->calls == c->hold + 1));
	memset(c->buf, c->calls & 0xff, sizeof(c->buf));
	pkt->data = c->buf;
	pkt->size = c->size;
	*received = true;
	return true;
}

/* ------------------------------------------------------------ fake output plugin */

static void print_pkt(const char *who, const struct encoder_packet *p)
{
	if (!p) {
		printf("plugin out %s packet NULL\n", who);
		return;
	}
	printf("plugin out %s pkt %c%zu pts=%" PRId64 " dts=%" PRId64 " usec=%" PRId64 " kf=%d size=%zu tb=%" PRId32 "/%" PRId32
	       " b0=%02x\n",
	       who, p->type == OBS_ENCODER_VIDEO ? 'V' : 'A', p->track_idx, p->pts, p->dts, p->dts_usec, p->keyframe, p->size,
	       p->timebase_num, p->timebase_den, p->size ? p->data[0] : 0);
}
static void *fout_create(obs_data_t *s, obs_output_t *o)
{
	(void)s;
	struct ocfg *c = find_ocfg(o->context.name);
	c->out = o;
	return c;
}
static void fout_destroy(void *d)
{
	printf("plugin out %s destroy\n", ((struct ocfg *)d)->name);
}
static bool fout_start(void *d)
{
	struct ocfg *c = d;
	__sync_fetch_and_add(&c->start_calls, 1);
	printf("plugin out %s start\n", c->name);
	if (!c->start_ok) {
		if (c->fail_code)
			obs_output_signal_stop(c->out, c->fail_code);
		return false;
	}
	if (c->autobegin) {
		/* what a real output plugin does in its start callback */
		if (!obs_output_can_begin_data_capture(c->out, 0) || !obs_output_initialize_encoders(c->out, 0)) {
			printf("plugin out %s cannot begin\n", c->name);
			return false;
		}
		bool r = obs_output_begin_data_capture(c->out, 0);
		printf("plugin out %s begin_data_capture -> %d\n", c->name, r);
	}
	return true;
}
static void fout_stop(void *d, uint64_t ts)
{
	struct ocfg *c = d;
	printf("plugin out %s stop ts=%" PRIu64 "\n", c->name, ts);
	if (c->autoend)
		obs_output_end_data_capture(c->out);
}
static void fout_packet(void *d, struct encoder_packet *p)
{
	print_pkt(((struct ocfg *)d)->name, p);
}

/* ------------------------------------------------------------ signal printing */

struct sigp {
	char out[32];
	char sig[24];
};
static struct sigp sigps[16 * 10];
static int nsigp;

static void sig_cb(void *param, calldata_t *cd)
{
	struct sigp *s = param;
	if (!strcmp(s->sig, "stop")) {
		long long code = 0;
		const char *err = NULL;
		calldata_get_int(cd, "code", &code);
		calldata_get_string(cd, "last_error", &err);
		printf("signal %s stop code=%lld last_error=%s\n", s->out, code, err ? err : "-");
	} else if (!strcmp(s->sig, "reconnect")) {
		long long t = -1;
		calldata_get_int(cd, "timeout_sec", &t);
		printf("signal %s reconnect timeout_sec=%lld\n", s->out, t);
	} else {
		printf("signal %s %s\n", s->out, s->sig);
	}
}

static const char *signal_names[] = {"start",    "stop",       "pause",     "unpause",           "starting",
				     "stopping", "activate",   "deactivate", "reconnect", "reconnect_success"};

/* ------------------------------------------------------------ helpers */

static const char *kv(struct script_line *sl, int from, const char *key)
{
	size_t n = strlen(key);
	for (int i = from; i < sl->argc; i++)
		if (!strncmp(sl->argv[i], key, n) && sl->argv[i][n] == '=')
			return sl->argv[i] + n + 1;
	return NULL;
}
static long kvl(struct script_line *sl, int from, const char *key, long def)
{
	const char *v = kv(sl, from, key);
	return v ? strtol(v, NULL, 0) : def;
}

static struct obs_output *outputs[16];
static struct obs_encoder *encoders[16];

static struct obs_output *get_out(const char *name)
{
	for (int i = 0; i < nocfg; i++)
		if (!strcmp(ocfgs[i].name, name))
			return outputs[i];
	fprintf(stderr, "script: unknown output %s\n", name);
	exit(2);
}
static struct obs_encoder *get_enc(const char *name)
{
	for (int i = 0; i < necfg; i++)
		if (!strcmp(ecfgs[i].name, name))
			return encoders[i];
	fprintf(stderr, "script: unknown encoder %s\n", name);
	exit(2);
}

static void nap(void)
{
	struct timespec ts = {0, 200000};
	nanosleep(&ts, NULL);
}

/* Wait until every thread the reference started has reached a defined resting point. */
static void settle(void)
{
	for (;;) {
		bool busy = (eo_reconnect_created - eo_waits_recorded) + (eo_waits_done - eo_threads_finished) != 0;
		for (int i = 0; i < nocfg; i++)
			if (outputs[i] && outputs[i]->end_data_capture_thread_active) {
				busy = true;
				eo_release = 1; /* let the held-back end-of-capture thread run */
			}
		if (!busy) {
			eo_release = 0;
			return;
		}
		nap();
	}
}

static uint32_t parse_flags(const char *s)
{
	uint32_t f = 0;
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", s);
	for (char *t = strtok(buf, ","); t; t = strtok(NULL, ",")) {
		if (!strcmp(t, "video")) f |= OBS_OUTPUT_VIDEO;
		else if (!strcmp(t, "audio")) f |= OBS_OUTPUT_AUDIO;
		else if (!strcmp(t, "av")) f |= OBS_OUTPUT_AV;
		else if (!strcmp(t, "encoded")) f |= OBS_OUTPUT_ENCODED;
		else if (!strcmp(t, "service")) f |= OBS_OUTPUT_SERVICE;
		else if (!strcmp(t, "multi_a")) f |= OBS_OUTPUT_MULTI_TRACK_AUDIO;
		else if (!strcmp(t, "multi_v")) f |= OBS_OUTPUT_MULTI_TRACK_VIDEO;
		else if (!strcmp(t, "pause")) f |= OBS_OUTPUT_CAN_PAUSE;
		else {
			fprintf(stderr, "script: unknown flag %s\n", t);
			exit(2);
		}
	}
	return f;
}

static bool rc_allow = true;
static bool reconnect_cb(void *param, obs_output_t *o, int code)
{
	(void)param;
	printf("reconnect_cb %s code=%d -> %d\n", o->context.name, code, rc_allow);
	return rc_allow;
}

static void on_signal(int sig)
{
	(void)sig;
	/* a hang or an unstubbed call: the vector records it */
	const char *m = sig == SIGALRM ? "result timeout\n" : "result crash\n";
	if (write(1, m, strlen(m)) < 0) {}
	_exit(0);
}

static void register_encoder(struct ecfg *c)
{
	struct obs_encoder_info ei = {0};
	ei.id = c->name;
	ei.type = c->video ? OBS_ENCODER_VIDEO : OBS_ENCODER_AUDIO;
	ei.codec = c->video ? "fakev" : "fakea";
	ei.caps = c->caps;
	ei.create = fenc_create;
	ei.destroy = fenc_destroy;
	ei.encode = fenc_encode;
	ei.get_sei_data = c->video ? fenc_sei : NULL;
	ei.get_frame_size = c->video ? NULL : fenc_frame_size;
	da_push_back(obs->encoder_types, &ei);
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;

	if (!getenv("EO_NOSIG")) signal(SIGSEGV, on_signal);
	signal(SIGALRM, on_signal);
	signal(SIGFPE, on_signal);
	signal(SIGABRT, on_signal);
	alarm(30);

	obs = bzalloc(sizeof(*obs));
	eo_config_video(30, 1, 1280, 720);
	eo_config_audio(48000);

	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		if (!strcmp(c, "echo")) {
			for (int i = 1; i < sl.argc; i++)
				printf("%s%s", i > 1 ? " " : "", sl.argv[i]);
			printf("\n");
		} else if (!strcmp(c, "video")) {
			script_need(&sl, 5);
			eo_config_video(atoi(sl.argv[1]), atoi(sl.argv[2]), atoi(sl.argv[3]), atoi(sl.argv[4]));
		} else if (!strcmp(c, "audio")) {
			script_need(&sl, 2);
			eo_config_audio(atoi(sl.argv[1]));
		} else if (!strcmp(c, "clock")) {
			script_need(&sl, 2);
			eo_set_clock(strtoull(sl.argv[1], NULL, 0));
		} else if (!strcmp(c, "venc") || !strcmp(c, "aenc")) {
			script_need(&sl, 2);
			struct ecfg *e = &ecfgs[necfg];
			memset(e, 0, sizeof(*e));
			snprintf(e->name, sizeof(e->name), "%s", sl.argv[1]);
			e->video = c[0] == 'v';
			e->kf = (int)kvl(&sl, 2, "kf", 0);
			e->dts_delay = (int)kvl(&sl, 2, "dts_delay", 0);
			e->hold = (int)kvl(&sl, 2, "hold", 0);
			e->fail_at = (int)kvl(&sl, 2, "fail_at", 0);
			e->size = (size_t)kvl(&sl, 2, "size", 4);
			e->frame_size = (size_t)kvl(&sl, 2, "frame_size", 1024);
			e->create_fail = kvl(&sl, 2, "create_fail", 0);
			e->caps = (uint32_t)kvl(&sl, 2, "caps", 0);
			const char *sei = kv(&sl, 2, "sei");
			if (sei) {
				size_t n;
				uint8_t *b = script_parse_hex(sei, &n);
				memcpy(e->sei, b, n);
				e->sei_len = n;
				free(b);
			}
			register_encoder(e);
			struct obs_encoder *enc;
			if (e->video)
				enc = obs_video_encoder_create(e->name, e->name, NULL, NULL);
			else
				enc = obs_audio_encoder_create(e->name, e->name, NULL, (size_t)kvl(&sl, 2, "mixer", 0), NULL);
			e->enc = enc;
			encoders[necfg++] = enc;
			if (!kvl(&sl, 2, "nomedia", 0)) {
				if (e->video)
					obs_encoder_set_video(enc, eo_video());
				else
					obs_encoder_set_audio(enc, eo_audio());
			}
			long div = kvl(&sl, 2, "divisor", 0);
			if (div && e->video)
				printf("set_frame_rate_divisor -> %d\n", obs_encoder_set_frame_rate_divisor(enc, (uint32_t)div));
			printf("encoder %s created tb=%d/%d\n", e->name, enc->timebase_num, enc->timebase_den);
		} else if (!strcmp(c, "enc_raw")) {
			/* enc_raw video|audio ID NAME: create with an arbitrary id (unknown id, wrong type) */
			script_need(&sl, 4);
			struct obs_encoder *enc = sl.argv[1][0] == 'v' ? obs_video_encoder_create(sl.argv[2], sl.argv[3], NULL, NULL)
								       : obs_audio_encoder_create(sl.argv[2], sl.argv[3], NULL, 0, NULL);
			printf("enc_raw -> %s\n", enc ? "created" : "NULL");
			if (enc)
				printf("enc_raw codec=%s type=%d\n", obs_encoder_get_codec(enc) ? obs_encoder_get_codec(enc) : "-",
				       (int)obs_encoder_get_type(enc));
		} else if (!strcmp(c, "enc_info")) {
			script_need(&sl, 2);
			struct obs_encoder *e = get_enc(sl.argv[1]);
			printf("enc_info %s w=%u h=%u divisor=%u rate=%u frame_size=%zu mixer=%zu caps=%u codec=%s last_error=%s\n",
			       sl.argv[1], e->info.type == OBS_ENCODER_VIDEO ? obs_encoder_get_width(e) : 0,
			       e->info.type == OBS_ENCODER_VIDEO ? obs_encoder_get_height(e) : 0,
			       e->info.type == OBS_ENCODER_VIDEO ? obs_encoder_get_frame_rate_divisor(e) : 0,
			       e->info.type == OBS_ENCODER_AUDIO ? obs_encoder_get_sample_rate(e) : 0,
			       e->info.type == OBS_ENCODER_AUDIO ? obs_encoder_get_frame_size(e) : 0,
			       e->info.type == OBS_ENCODER_AUDIO ? obs_encoder_get_mixer_index(e) : 0, obs_encoder_get_caps(e),
			       obs_encoder_get_codec(e), obs_encoder_get_last_error(e) ? obs_encoder_get_last_error(e) : "-");
		} else if (!strcmp(c, "enc_init")) {
			script_need(&sl, 2);
			printf("enc_init %s -> %d\n", sl.argv[1], obs_encoder_initialize(get_enc(sl.argv[1])));
		} else if (!strcmp(c, "enc_set_video") || !strcmp(c, "enc_set_audio")) {
			script_need(&sl, 2);
			struct obs_encoder *e = get_enc(sl.argv[1]);
			if (c[8] == 'v')
				obs_encoder_set_video(e, eo_video());
			else
				obs_encoder_set_audio(e, eo_audio());
			printf("%s tb=%d/%d media=%d\n", c, e->timebase_num, e->timebase_den, e->media != NULL);
		} else if (!strcmp(c, "enc_divisor")) {
			script_need(&sl, 3);
			printf("enc_divisor -> %d\n", obs_encoder_set_frame_rate_divisor(get_enc(sl.argv[1]), atoi(sl.argv[2])));
		} else if (!strcmp(c, "enc_scaled")) {
			script_need(&sl, 4);
			struct obs_encoder *e = get_enc(sl.argv[1]);
			obs_encoder_set_scaled_size(e, atoi(sl.argv[2]), atoi(sl.argv[3]));
			printf("enc_scaled %u x %u enabled=%d\n", obs_encoder_get_width(e), obs_encoder_get_height(e),
			       obs_encoder_scaling_enabled(e));
		} else if (!strcmp(c, "enc_start_ts")) {
			script_need(&sl, 3);
			get_enc(sl.argv[1])->start_ts = strtoull(sl.argv[2], NULL, 0);
		} else if (!strcmp(c, "out")) {
			script_need(&sl, 3);
			int oidx = nocfg++;
			struct ocfg *o = &ocfgs[oidx];
			memset(o, 0, sizeof(*o));
			snprintf(o->name, sizeof(o->name), "%s", sl.argv[1]);
			o->flags = parse_flags(sl.argv[2]);
			o->start_ok = kvl(&sl, 3, "start_ok", 1);
			o->autobegin = kvl(&sl, 3, "autobegin", 0);
			o->autoend = kvl(&sl, 3, "autoend", 1);
			struct obs_output_info oi = {0};
			oi.id = o->name;
			oi.flags = o->flags;
			oi.create = fout_create;
			oi.destroy = fout_destroy;
			oi.start = fout_start;
			oi.stop = fout_stop;
			if (o->flags & OBS_OUTPUT_ENCODED)
				oi.encoded_packet = fout_packet;
			da_push_back(obs->output_types, &oi);
			struct obs_output *out = obs_output_create(o->name, o->name, NULL, NULL);
			outputs[oidx] = out;
			for (size_t i = 0; i < sizeof(signal_names) / sizeof(signal_names[0]); i++) {
				struct sigp *s = &sigps[nsigp++];
				snprintf(s->out, sizeof(s->out), "%s", o->name);
				snprintf(s->sig, sizeof(s->sig), "%s", signal_names[i]);
				signal_handler_connect(out->context.signals, signal_names[i], sig_cb, s);
			}
			printf("output %s created flags=%u reconnect=%u/%u\n", o->name, o->flags, out->reconnect_retry_max,
			       out->reconnect_retry_sec);
		} else if (!strcmp(c, "out_unknown")) {
			/* out_unknown ID: an output whose id is not registered (inert output, no plugin data) */
			script_need(&sl, 2);
			struct obs_output *o = obs_output_create(sl.argv[1], "unknown", NULL, NULL);
			printf("out_unknown -> %s data=%d start=%d flags=%u\n", o ? "created" : "NULL", o && o->context.data != NULL,
			       o ? obs_output_start(o) : -1, o ? obs_output_get_flags(o) : 0);
			obs_output_stop(o);
			obs_output_destroy(o);
		} else if (!strcmp(c, "plugin")) {
			/* plugin OUT key=value...: reconfigure the fake output plugin (start_ok, autobegin, autoend, fail_code) */
			script_need(&sl, 2);
			struct ocfg *o = find_ocfg(sl.argv[1]);
			o->start_ok = kvl(&sl, 2, "start_ok", o->start_ok);
			o->autobegin = kvl(&sl, 2, "autobegin", o->autobegin);
			o->autoend = kvl(&sl, 2, "autoend", o->autoend);
			o->fail_code = (int)kvl(&sl, 2, "fail_code", o->fail_code);
		} else if (!strcmp(c, "set_venc") || !strcmp(c, "set_aenc")) {
			script_need(&sl, 3);
			struct obs_output *o = get_out(sl.argv[1]);
			struct obs_encoder *e = !strcmp(sl.argv[2], "-") ? NULL : get_enc(sl.argv[2]);
			size_t idx = sl.argc > 3 ? (size_t)atoi(sl.argv[3]) : 0;
			if (c[4] == 'v')
				obs_output_set_video_encoder2(o, e, idx);
			else
				obs_output_set_audio_encoder(o, e, idx);
{
				unsigned vm = 0, am = 0;
				for (int i = 0; i < MAX_OUTPUT_VIDEO_ENCODERS; i++)
					vm |= (o->video_encoders[i] != NULL) << i;
				for (int i = 0; i < MAX_OUTPUT_AUDIO_ENCODERS; i++)
					am |= (o->audio_encoders[i] != NULL) << i;
				printf("%s %s idx=%zu -> video_slots=%02x audio_slots=%02x\n", c, sl.argv[1], idx, vm, am);
			}
		} else if (!strcmp(c, "set_delay")) {
			script_need(&sl, 3);
			struct obs_output *o = get_out(sl.argv[1]);
			obs_output_set_delay(o, (uint32_t)atoi(sl.argv[2]), sl.argc > 3 ? (uint32_t)atoi(sl.argv[3]) : 0);
			printf("delay %s sec=%u flags=%u active=%u\n", sl.argv[1], obs_output_get_delay(o), o->delay_flags,
			       obs_output_get_active_delay(o));
		} else if (!strcmp(c, "reconnect_cfg")) {
			script_need(&sl, 4);
			obs_output_set_reconnect_settings(get_out(sl.argv[1]), atoi(sl.argv[2]), atoi(sl.argv[3]));
		} else if (!strcmp(c, "set_service")) {
			script_need(&sl, 2);
			struct obs_service *s = bzalloc(sizeof(*s));
			obs_output_set_service(get_out(sl.argv[1]), s);
			printf("service set=%d\n", get_out(sl.argv[1])->service != NULL);
		} else if (!strcmp(c, "start")) {
			script_need(&sl, 2);
			printf("start %s -> %d\n", sl.argv[1], obs_output_start(get_out(sl.argv[1])));
		} else if (!strcmp(c, "stop")) {
			script_need(&sl, 2);
			obs_output_stop(get_out(sl.argv[1]));
			printf("stop %s done\n", sl.argv[1]);
		} else if (!strcmp(c, "force_stop")) {
			script_need(&sl, 2);
			obs_output_force_stop(get_out(sl.argv[1]));
			printf("force_stop %s done\n", sl.argv[1]);
		} else if (!strcmp(c, "begin")) {
			script_need(&sl, 2);
			printf("begin %s -> %d\n", sl.argv[1], obs_output_begin_data_capture(get_out(sl.argv[1]), 0));
		} else if (!strcmp(c, "can_begin")) {
			script_need(&sl, 2);
			printf("can_begin %s -> %d\n", sl.argv[1], obs_output_can_begin_data_capture(get_out(sl.argv[1]), 0));
		} else if (!strcmp(c, "init_encoders")) {
			script_need(&sl, 2);
			printf("init_encoders %s -> %d\n", sl.argv[1], obs_output_initialize_encoders(get_out(sl.argv[1]), 0));
		} else if (!strcmp(c, "end")) {
			script_need(&sl, 2);
			obs_output_end_data_capture(get_out(sl.argv[1]));
			printf("end %s done\n", sl.argv[1]);
		} else if (!strcmp(c, "signal_stop")) {
			script_need(&sl, 3);
			obs_output_signal_stop(get_out(sl.argv[1]), atoi(sl.argv[2]));
			printf("signal_stop %s %s done\n", sl.argv[1], sl.argv[2]);
		} else if (!strcmp(c, "destroy")) {
			script_need(&sl, 2);
			for (int i = 0; i < nocfg; i++)
				if (!strcmp(ocfgs[i].name, sl.argv[1])) {
					obs_output_destroy(outputs[i]);
					outputs[i] = NULL;
				}
			printf("destroy %s done\n", sl.argv[1]);
		} else if (!strcmp(c, "fire")) {
			/* let the pending reconnect timer expire and run the reconnect attempt to completion */
			while (eo_waits_recorded - eo_waits_done <= 0)
				nap();
			__sync_fetch_and_add(&eo_gate, 1);
			while (eo_gate != 0)
				nap();
			settle();
			printf("fire done\n");
		} else if (!strcmp(c, "lasterr")) {
			script_need(&sl, 3);
			struct obs_output *o = get_out(sl.argv[1]);
			if (strcmp(sl.argv[2], "?"))
				obs_output_set_last_error(o, strcmp(sl.argv[2], "-") ? sl.argv[2] : NULL);
			const char *e = obs_output_get_last_error(o);
			printf("lasterr %s = %s\n", sl.argv[1], e ? e : "-");
		} else if (!strcmp(c, "vframe")) {
			script_need(&sl, 2);
			eo_feed_video(strtoull(sl.argv[1], NULL, 0));
		} else if (!strcmp(c, "aframe")) {
			script_need(&sl, 4);
			eo_feed_audio((size_t)atoi(sl.argv[1]), strtoull(sl.argv[2], NULL, 0), (uint32_t)atoi(sl.argv[3]));
		} else if (!strcmp(c, "emit")) {
			/* emit ENC PTS DTS KF [SIZE [TBNUM TBDEN]] : inject one packet through send_off_encoder_packet */
			script_need(&sl, 5);
			struct obs_encoder *e = get_enc(sl.argv[1]);
			struct encoder_packet p = {0};
			static uint8_t data[256];
			static uint8_t serial;
			p.type = e->info.type;
			p.pts = strtoll(sl.argv[2], NULL, 0);
			p.dts = strtoll(sl.argv[3], NULL, 0);
			p.keyframe = atoi(sl.argv[4]) != 0;
			p.size = sl.argc > 5 ? (size_t)atoi(sl.argv[5]) : 4;
			p.timebase_num = sl.argc > 7 ? atoi(sl.argv[6]) : (e->info.type == OBS_ENCODER_VIDEO ? e->timebase_num : 1);
			p.timebase_den = sl.argc > 7 ? atoi(sl.argv[7]) : e->timebase_den;
			p.encoder = e;
			if (e->info.type == OBS_ENCODER_VIDEO) {
				/* do_encode() queues one timing record per submitted frame */
				struct encoder_packet_time *t = da_push_back_new(e->encoder_packet_times);
				t->pts = p.pts;
				t->cts = 1;
			}
			memset(data, ++serial, sizeof(data));
			p.data = data;
			send_off_encoder_packet(e, true, true, &p);
		} else if (!strcmp(c, "emit_fail")) {
			script_need(&sl, 2);
			struct encoder_packet p = {0};
			send_off_encoder_packet(get_enc(sl.argv[1]), false, false, &p);
		} else if (!strcmp(c, "state")) {
			script_need(&sl, 2);
			struct obs_output *o = get_out(sl.argv[1]);
			printf("state %s active=%d data_active=%d delay_active=%d delay_capturing=%d reconnecting=%d stopping=%d "
			       "stop_code=%d total_frames=%d delay_ns=%" PRIu64 " refs=%ld queued=%zu recv_v=%d%d recv_a=%d "
			       "hi_a=%" PRId64 " hi_v0=%" PRId64 " voff=%" PRId64 " aoff=%" PRId64 " batch=%zu retries=%u cur_ms=%u\n",
			       sl.argv[1], os_atomic_load_bool(&o->active), os_atomic_load_bool(&o->data_active),
			       os_atomic_load_bool(&o->delay_active), os_atomic_load_bool(&o->delay_capturing),
			       os_atomic_load_bool(&o->reconnecting), os_event_try(o->stopping_event) == EAGAIN, o->stop_code,
			       o->total_frames, o->active_delay_ns, os_atomic_load_long(&o->delay_restart_refs),
			       o->interleaved_packets.num, o->received_video[0], o->received_video[1], o->received_audio,
			       o->highest_audio_ts, o->highest_video_ts[0], o->video_offsets[0], o->audio_offsets[0],
			       o->interleaver_max_batch_size, o->reconnect_retries, o->reconnect_retry_cur_msec);
		} else if (!strcmp(c, "queue")) {
			script_need(&sl, 2);
			struct obs_output *o = get_out(sl.argv[1]);
			printf("queue %s n=%zu delayq=%zu:", sl.argv[1], o->interleaved_packets.num,
			       o->delay_data.size / sizeof(struct delay_data));
			for (size_t i = 0; i < o->interleaved_packets.num; i++) {
				struct encoder_packet *p = &o->interleaved_packets.array[i];
				printf(" %c%zu@%" PRId64, p->type == OBS_ENCODER_VIDEO ? 'V' : 'A', p->track_idx, p->dts_usec);
			}
			printf("\n");
		} else if (!strcmp(c, "encstate")) {
			script_need(&sl, 2);
			struct obs_encoder *e = get_enc(sl.argv[1]);
			printf("encstate %s init=%d active=%d first_received=%d start_ts=%" PRIu64 " offset_usec=%" PRId64
			       " cur_pts=%" PRId64 " frames=%u callbacks=%zu paired=%zu outputs=%zu\n",
			       sl.argv[1], e->initialized, obs_encoder_active(e), e->first_received, e->start_ts, e->offset_usec,
			       e->cur_pts, obs_encoder_get_encoded_frames(e), e->callbacks.num, e->paired_encoders.num,
			       e->outputs.num);
		} else if (!strcmp(c, "conns")) {
			printf("conns video=%zu audio=%zu\n", eo_video_conns(), eo_audio_conns());
		} else if (!strcmp(c, "info")) {
			script_need(&sl, 2);
			struct obs_output *o = get_out(sl.argv[1]);
			printf("info %s flags=%u active=%d reconnecting=%d delay=%u active_delay=%u\n", sl.argv[1],
			       obs_output_get_flags(o), obs_output_active(o), obs_output_reconnecting(o), obs_output_get_delay(o),
			       obs_output_get_active_delay(o));
		} else if (!strcmp(c, "reconnect_cb")) {
			/* reconnect_cb OUT allow|veto|off */
			script_need(&sl, 3);
			rc_allow = strcmp(sl.argv[2], "veto") != 0;
			obs_output_set_reconnect_callback(get_out(sl.argv[1]), strcmp(sl.argv[2], "off") ? reconnect_cb : NULL, NULL);
		} else if (!strcmp(c, "poke_exp")) {
			/* test hook: set the reconnect exponent (the reference randomises it in [1.5, 1.55)) */
			script_need(&sl, 3);
			get_out(sl.argv[1])->reconnect_retry_exp = strtof(sl.argv[2], NULL);
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, c);
			return 2;
		}
		fflush(stdout);
		settle();
	}
	return 0;
}
