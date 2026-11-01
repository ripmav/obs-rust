/*
 * Conformance wrapper (area linuxinputs, spec S-062): includes the real plugins/linux-alsa/alsa-input.c and
 * replaces libasound (PCM, hw_params, device-name hints) by a scripted fake PCM device model. libasound is NOT
 * linked. os_event_timedwait is replaced (macro) by a credit-driven wait so that the 1000..5000 ms reopen
 * back-off is deterministic and fast; the requested timeouts are recorded and printed.
 * Test tooling, not part of OBS.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <alloca.h>
#include <util/threading.h>
struct os_event_data;
int li_alsa_timedwait(struct os_event_data *event, unsigned long ms);
#define os_event_timedwait li_alsa_timedwait
#include "../../../plugins/linux-alsa/alsa-input.c"
#include "linuxinputs_api.h"

#define IS(s) (!strcmp(a[0], s))

/* ------------------------------------------------------------------ fake device model */
struct _snd_pcm {
	int state;
	int framebytes;
	int seed_counter;
};
struct _snd_pcm_hw_params {
	int dummy;
};

static struct {
	unsigned fmts; /* bit 0 s16, 1 s32, 2 float, 3 u8 */
	unsigned rates[16];
	int nrates;
	unsigned chmin, chmax;
	unsigned long period;
	int state_after; /* SND_PCM_STATE_* after hw_params */
	char fail_stage[32];
	int fail_err;
	int open_fail_count, open_fail_err;
	int resume_ok;
	unsigned long hw_chosen_rate;
	unsigned ch_chosen;
	snd_pcm_format_t fmt_chosen;
} cfg;

struct hint {
	char *name, *descr, *ioid;
};
static struct hint hints[16];
static int nhints;
static int hint_fail;

static struct qitem {
	int err; /* 0 = data */
	unsigned frames;
	unsigned long long now;
} q[256];
static int qhead, qtail;
static volatile unsigned idle_polls;
static pthread_mutex_t qlock = PTHREAD_MUTEX_INITIALIZER;
static int seed;
static volatile int open_count, close_count;

static void cfg_reset(void)
{
	memset(&cfg, 0, sizeof(cfg));
	cfg.fmts = 0xf;
	cfg.rates[0] = 8000; cfg.rates[1] = 32000; cfg.rates[2] = 44100; cfg.rates[3] = 48000; cfg.nrates = 4;
	cfg.chmin = 1; cfg.chmax = 8;
	cfg.period = 1024;
	cfg.state_after = SND_PCM_STATE_PREPARED;
	cfg.resume_ok = 1;
}
static bool failing(const char *stage)
{
	return cfg.fail_err && !strcmp(cfg.fail_stage, stage);
}
static const char *fmtname(snd_pcm_format_t f)
{
	switch (f) {
	case SND_PCM_FORMAT_S16_LE: return "S16_LE";
	case SND_PCM_FORMAT_S32_LE: return "S32_LE";
	case SND_PCM_FORMAT_FLOAT_LE: return "FLOAT_LE";
	case SND_PCM_FORMAT_U8: return "U8";
	default: return "OTHER";
	}
}
static int fmtbit(snd_pcm_format_t f)
{
	switch (f) {
	case SND_PCM_FORMAT_S16_LE: return 0;
	case SND_PCM_FORMAT_S32_LE: return 1;
	case SND_PCM_FORMAT_FLOAT_LE: return 2;
	case SND_PCM_FORMAT_U8: return 3;
	default: return 7;
	}
}

const char *snd_strerror(int err)
{
	switch (err) {
	case -ENOENT: return "No such file or directory";
	case -EBUSY: return "Device or resource busy";
	case -EINVAL: return "Invalid argument";
	case -EPIPE: return "Broken pipe";
	case -ENODEV: return "No such device";
	default: return "Unknown error";
	}
}
size_t snd_pcm_hw_params_sizeof(void) { return sizeof(struct _snd_pcm_hw_params); }
int snd_pcm_format_physical_width(snd_pcm_format_t f)
{
	switch (f) {
	case SND_PCM_FORMAT_U8: return 8;
	case SND_PCM_FORMAT_S16_LE: return 16;
	case SND_PCM_FORMAT_S32_LE: case SND_PCM_FORMAT_FLOAT_LE: return 32;
	default: return -EINVAL;
	}
}

int snd_pcm_open(snd_pcm_t **pcm, const char *name, snd_pcm_stream_t stream, int mode)
{
	li_say("snd_pcm_open '%s' stream=%d mode=%d", name, (int)stream, mode);
	if (cfg.open_fail_count > 0) {
		cfg.open_fail_count--;
		return cfg.open_fail_err;
	}
	if (failing("open"))
		return cfg.fail_err;
	*pcm = calloc(1, sizeof(**pcm));
	(*pcm)->state = SND_PCM_STATE_OPEN;
	open_count++;
	return 0;
}
int snd_pcm_close(snd_pcm_t *pcm) { li_say("snd_pcm_close"); close_count++; free(pcm); return 0; }
int snd_pcm_drop(snd_pcm_t *pcm) { (void)pcm; li_say("snd_pcm_drop"); return 0; }
snd_pcm_state_t snd_pcm_state(snd_pcm_t *pcm) { return pcm->state; }
int snd_pcm_start(snd_pcm_t *pcm)
{
	li_say("snd_pcm_start");
	if (failing("start"))
		return cfg.fail_err;
	pcm->state = SND_PCM_STATE_RUNNING;
	return 0;
}
int snd_pcm_hw_params_any(snd_pcm_t *pcm, snd_pcm_hw_params_t *p) { (void)pcm; (void)p; li_say("hw_params_any"); return failing("any") ? cfg.fail_err : 0; }
int snd_pcm_hw_params_set_access(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_access_t acc)
{
	(void)pcm; (void)p;
	li_say("hw_params_set_access %d", (int)acc);
	return failing("access") ? cfg.fail_err : 0;
}
int snd_pcm_hw_params_test_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_format_t f)
{
	(void)pcm; (void)p;
	int ok = (cfg.fmts >> fmtbit(f)) & 1;
	li_say("hw_params_test_format %s -> %s", fmtname(f), ok ? "ok" : "unsupported");
	return ok ? 0 : -EINVAL;
}
int snd_pcm_hw_params_set_format(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, snd_pcm_format_t f)
{
	(void)pcm; (void)p;
	li_say("hw_params_set_format %s", fmtname(f));
	cfg.fmt_chosen = f;
	/* the plugin ignores this function's result (see spec quirk), so a failure here is observable only in the log */
	return failing("format") ? cfg.fail_err : 0;
}
int snd_pcm_hw_params_set_rate_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned int *val, int *dir)
{
	(void)pcm; (void)p;
	li_say("hw_params_set_rate_near req=%u dir=%s", *val, dir ? "ptr" : "null");
	if (failing("rate"))
		return cfg.fail_err;
	unsigned best = cfg.rates[0];
	for (int i = 1; i < cfg.nrates; i++) {
		unsigned d1 = cfg.rates[i] > *val ? cfg.rates[i] - *val : *val - cfg.rates[i];
		unsigned d0 = best > *val ? best - *val : *val - best;
		if (d1 < d0)
			best = cfg.rates[i];
	}
	*val = best;
	return 0;
}
int snd_pcm_hw_params_get_channels(const snd_pcm_hw_params_t *p, unsigned int *val)
{
	(void)p;
	if (cfg.chmin != cfg.chmax) {
		li_say("hw_params_get_channels -> EINVAL (range %u..%u)", cfg.chmin, cfg.chmax);
		return -EINVAL;
	}
	*val = cfg.chmin;
	li_say("hw_params_get_channels -> %u", *val);
	return 0;
}
int snd_pcm_hw_params_set_channels_near(snd_pcm_t *pcm, snd_pcm_hw_params_t *p, unsigned int *val)
{
	(void)pcm; (void)p;
	li_say("hw_params_set_channels_near req=%u", *val);
	if (failing("channels"))
		return cfg.fail_err;
	if (*val < cfg.chmin)
		*val = cfg.chmin;
	if (*val > cfg.chmax)
		*val = cfg.chmax;
	cfg.ch_chosen = *val;
	return 0;
}
int snd_pcm_hw_params(snd_pcm_t *pcm, snd_pcm_hw_params_t *p)
{
	(void)p;
	li_say("hw_params commit");
	if (failing("hwparams"))
		return cfg.fail_err;
	pcm->state = cfg.state_after;
	pcm->framebytes = cfg.ch_chosen * (snd_pcm_format_physical_width(cfg.fmt_chosen) / 8);
	return 0;
}
int snd_pcm_hw_params_get_period_size(const snd_pcm_hw_params_t *p, snd_pcm_uframes_t *frames, int *dir)
{
	(void)p;
	if (failing("period"))
		return cfg.fail_err;
	*frames = cfg.period;
	*dir = 0;
	li_say("hw_params_get_period_size -> %lu", cfg.period);
	return 0;
}

static __thread int idle_poll_thread; /* set when this thread's last read was a synthetic idle poll (not logged) */
static pthread_t cls_reopen, cls_listen;
static int classify(pthread_t t)
{
	return pthread_equal(t, cls_reopen) ? 0 : pthread_equal(t, cls_listen) ? 1 : 2;
}

snd_pcm_sframes_t snd_pcm_readi(snd_pcm_t *pcm, void *buf, snd_pcm_uframes_t size)
{
	pthread_mutex_lock(&qlock);
	if (qhead == qtail) {
		idle_polls++;
		pthread_mutex_unlock(&qlock);
		li_sleep_ms(1);
		idle_poll_thread = 1;
		return -EAGAIN;
	}
	idle_poll_thread = 0;
	struct qitem it = q[qhead++ % 256];
	pthread_mutex_unlock(&qlock);
	li_set_now(it.now);
	if (it.err)
		return it.err;
	unsigned n = it.frames < size ? it.frames : (unsigned)size;
	li_audio_framebytes = pcm->framebytes;
	int s = ++seed;
	for (unsigned k = 0; k < n * (unsigned)pcm->framebytes; k++)
		((uint8_t *)buf)[k] = (uint8_t)(s + k);
	return n;
}
int snd_pcm_recover(snd_pcm_t *pcm, int err, int silent)
{
	(void)silent;
	int r = err;
	if (err == -EINTR)
		r = 0;
	else if (err == -EPIPE) {
		pcm->state = SND_PCM_STATE_PREPARED;
		r = 0;
	} else if (err == -ESTRPIPE)
		r = cfg.resume_ok ? 0 : err;
	if (!idle_poll_thread)
		li_say("snd_pcm_recover err=%d -> %d", err, r);
	return r;
}
int snd_pcm_wait(snd_pcm_t *pcm, int timeout)
{
	(void)pcm;
	if (!idle_poll_thread)
		li_say("snd_pcm_wait %d", timeout);
	li_sleep_ms(1);
	return 1;
}

int snd_device_name_hint(int card, const char *iface, void ***out)
{
	li_say("snd_device_name_hint card=%d iface=%s", card, iface);
	if (hint_fail)
		return -ENOMEM;
	void **arr = calloc(nhints + 1, sizeof(void *));
	for (int i = 0; i < nhints; i++)
		arr[i] = &hints[i];
	*out = arr;
	return 0;
}
char *snd_device_name_get_hint(const void *hint, const char *id)
{
	const struct hint *h = hint;
	const char *v = !strcmp(id, "NAME") ? h->name : !strcmp(id, "DESC") ? h->descr : !strcmp(id, "IOID") ? h->ioid : NULL;
	return v ? strdup(v) : NULL;
}
int snd_device_name_free_hint(void **h) { free(h); return 0; }

/* ------------------------------------------------------------------ credit-driven timedwait */
static pthread_mutex_t tw_lock = PTHREAD_MUTEX_INITIALIZER;
static int tw_credits;
static volatile int tw_blocked;
static volatile unsigned tw_entries;
int li_alsa_timedwait(struct os_event_data *ev, unsigned long ms)
{
	li_say("reopen wait %lu ms", ms);
	pthread_mutex_lock(&tw_lock);
	tw_entries++;
	tw_blocked = 1;
	for (;;) {
		if (os_event_try(ev) == 0) {
			tw_blocked = 0;
			pthread_mutex_unlock(&tw_lock);
			return 0;
		}
		if (tw_credits > 0) {
			tw_credits--;
			tw_blocked = 0;
			pthread_mutex_unlock(&tw_lock);
			return ETIMEDOUT;
		}
		pthread_mutex_unlock(&tw_lock);
		li_sleep_ms(1);
		pthread_mutex_lock(&tw_lock);
	}
}

/* ------------------------------------------------------------------ script commands */
static struct alsa_data *g;

static void state(void)
{
	if (!g) {
		printf("state (no instance)\n");
		return;
	}
	printf("state device=%s rate=%u channels=%u format=%s sample_size=%u period=%lu handle=%d listen_thread=%d reopen_thread=%d "
	       "reopen=%d shutdown=%d buffer=%d first_ts=%llu\n",
	       g->device ? g->device : "(null)", g->rate, g->channels, fmtname(g->format), g->sample_size, (unsigned long)g->period_size,
	       g->handle != NULL, g->listen_thread != 0, g->reopen_thread != 0, g->reopen, g->shutdown, g->buffer != NULL,
	       (unsigned long long)g->first_ts);
}

static bool wait_blocked(void)
{
	for (int i = 0; i < 3000 && !tw_blocked; i++)
		li_sleep_ms(1);
	return tw_blocked;
}

/* The capture thread sets `listen` itself after it started; stopping before that deadlocks the reference (D-1),
 * so scripts wait for it, as a real host does implicitly by being slow. */
static void note_threads(void)
{
	if (g && g->reopen_thread)
		cls_reopen = g->reopen_thread;
	if (g && g->listen_thread)
		cls_listen = g->listen_thread;
}
static void wait_listen(void)
{
	for (int i = 0; g && i < 3000 && g->listen_thread && !g->listen; i++)
		li_sleep_ms(1);
	/* likewise the reopen thread sets `reopen` itself; update() racing that startup joins a thread that never got the
	 * abort signal (the reference then simply waits out one back-off period) */
	for (int i = 0; g && i < 100 && g->reopen_thread && !g->reopen && !tw_blocked; i++)
		li_sleep_ms(1);
	note_threads();
}

bool li_alsa_cmd(struct script_line *sl)
{
	char **a = sl->argv;
	if (strncmp(a[0], "alsa-", 5) != 0)
		return false;
	li_set_classifier(classify);
	if (g) {
		if (g->reopen_thread)
			cls_reopen = g->reopen_thread;
		if (g->listen_thread)
			cls_listen = g->listen_thread;
	}
	if (IS("alsa-reset")) {
		cfg_reset();
		nhints = 0;
		hint_fail = 0;
		qhead = qtail = 0;
		seed = 0;
		li_audio_reset();
	} else if (IS("alsa-cfg")) {
		script_need(sl, 3);
		if (!strcmp(a[1], "formats")) {
			cfg.fmts = 0;
			char *s = strdup(a[2]), *tok = strtok(s, ",");
			for (; tok; tok = strtok(NULL, ","))
				cfg.fmts |= !strcmp(tok, "s16") ? 1 : !strcmp(tok, "s32") ? 2 : !strcmp(tok, "float") ? 4 : !strcmp(tok, "u8") ? 8 : 0;
			free(s);
		} else if (!strcmp(a[1], "rates")) {
			cfg.nrates = 0;
			char *s = strdup(a[2]), *tok = strtok(s, ",");
			for (; tok && cfg.nrates < 16; tok = strtok(NULL, ","))
				cfg.rates[cfg.nrates++] = strtoul(tok, NULL, 10);
			free(s);
		} else if (!strcmp(a[1], "channels")) {
			script_need(sl, 4);
			cfg.chmin = atoi(a[2]);
			cfg.chmax = atoi(a[3]);
		} else if (!strcmp(a[1], "period"))
			cfg.period = strtoul(a[2], NULL, 10);
		else if (!strcmp(a[1], "state"))
			cfg.state_after = !strcmp(a[2], "prepared") ? SND_PCM_STATE_PREPARED : !strcmp(a[2], "setup") ? SND_PCM_STATE_SETUP : SND_PCM_STATE_RUNNING;
		else if (!strcmp(a[1], "fail")) {
			script_need(sl, 4);
			snprintf(cfg.fail_stage, sizeof(cfg.fail_stage), "%s", a[2]);
			cfg.fail_err = atoi(a[3]);
		} else if (!strcmp(a[1], "open-fail")) {
			script_need(sl, 4);
			cfg.open_fail_count = atoi(a[2]);
			cfg.open_fail_err = atoi(a[3]);
		} else if (!strcmp(a[1], "resume"))
			cfg.resume_ok = atoi(a[2]);
	} else if (IS("alsa-hint")) {
		script_need(sl, 4);
		struct hint *h = &hints[nhints++];
		h->name = strcmp(a[1], "-") ? strdup(a[1]) : NULL;
		h->descr = strcmp(a[2], "-") ? strdup(a[2]) : NULL;
		h->ioid = strcmp(a[3], "-") ? strdup(a[3]) : NULL;
	} else if (IS("alsa-hint-fail"))
		hint_fail = atoi(a[1]);
	else if (IS("alsa-info")) {
		printf("info id=%s type=%d flags=0x%x icon=%d name=%s\n", alsa_input_capture.id, (int)alsa_input_capture.type,
		       alsa_input_capture.output_flags, (int)alsa_input_capture.icon_type, alsa_input_capture.get_name(NULL));
	} else if (IS("alsa-defaults")) {
		obs_data_t *d = obs_data_create();
		alsa_input_capture.get_defaults(d);
		printf("defaults device_id=%s custom_pcm=%s rate=%lld\n", obs_data_get_string(d, "device_id"), obs_data_get_string(d, "custom_pcm"),
		       obs_data_get_int(d, "rate"));
		obs_data_release(d);
	} else if (IS("alsa-props")) {
		li_props = alsa_input_capture.get_properties(NULL);
		li_dump_props(li_props, 0);
	} else if (IS("alsa-create")) {
		g = alsa_input_capture.create(li_settings, (obs_source_t *)(uintptr_t)1);
		wait_listen();
		li_flush();
		printf("create -> %s\n", g ? "ok" : "NULL");
	} else if (IS("alsa-update")) {
		alsa_input_capture.update(g, li_settings);
		wait_listen();
		li_flush();
	} else if (IS("alsa-destroy")) {
		alsa_input_capture.destroy(g);
		g = NULL;
	} else if (IS("alsa-state"))
		state();
	else if (IS("alsa-push")) {
		/* alsa-push FRAMES NOW_NS */
		script_need(sl, 3);
		pthread_mutex_lock(&qlock);
		q[qtail++ % 256] = (struct qitem){0, (unsigned)atoi(a[1]), strtoull(a[2], NULL, 10)};
		pthread_mutex_unlock(&qlock);
	} else if (IS("alsa-push-err")) {
		script_need(sl, 3);
		pthread_mutex_lock(&qlock);
		q[qtail++ % 256] = (struct qitem){atoi(a[1]), 0, strtoull(a[2], NULL, 10)};
		pthread_mutex_unlock(&qlock);
	} else if (IS("alsa-wait-drain")) {
		for (int i = 0; i < 3000 && qhead != qtail; i++)
			li_sleep_ms(1);
		unsigned s = idle_polls;
		for (int i = 0; i < 3000 && idle_polls < s + 2; i++)
			li_sleep_ms(1);
		li_flush();
	} else if (IS("alsa-reopen-tick")) {
		if (!wait_blocked()) {
			printf("reopen-tick: reopen thread not waiting\n");
			return true;
		}
		unsigned prev = tw_entries;
		pthread_mutex_lock(&tw_lock);
		tw_credits++;
		pthread_mutex_unlock(&tw_lock);
		for (int i = 0; i < 3000 && tw_entries == prev && g->reopen; i++)
			li_sleep_ms(1);
		li_flush();
	} else if (IS("alsa-reopen-reap")) {
		/* joins the finished reopen thread exactly as the plugin would on the next stop_reopen */
		if (g && g->reopen_thread) {
			pthread_join(g->reopen_thread, NULL);
			g->reopen_thread = 0;
		}
		wait_listen();
		li_flush();
	} else if (IS("alsa-counts"))
		printf("counts opened=%d closed=%d\n", open_count, close_count);
	else
		return false;
	return true;
}
