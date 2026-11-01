/*
 * Conformance runner for spec S-046 (obs-ffmpeg outputs). The real plugins/obs-ffmpeg/obs-ffmpeg-mux.c (ffmpeg_muxer,
 * ffmpeg_mpegts_muxer, replay_buffer) and obs-ffmpeg-hls-mux.c are #included; libobs, the helper process pipe, the file
 * system and threads are fakes (ffoutputs_stubs.c). The ffmpeg_output side lives in ffoutputs_out.c (commands "fo_*").
 * Script language: see test/conformance/README.md ("ffoutputs runner").
 */
#include <inttypes.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <pthread.h>
#include <obs-module.h>
#include <util/threading.h>
#include <util/dstr.h>
#include <util/deque.h>
#include <util/darray.h>
#include <callback/calldata.h>
#include "harness.h"
#include "ffoutputs_api.h"

#define pthread_create ffo_pthread_create
#define pthread_join ffo_pthread_join
#include "../../../plugins/obs-ffmpeg/obs-ffmpeg-mux.c"
#undef do_log
#undef warn
#undef info
#include "../../../plugins/obs-ffmpeg/obs-ffmpeg-hls-mux.c"
#undef pthread_create
#undef pthread_join

extern struct obs_output_info ffmpeg_muxer, ffmpeg_mpegts_muxer, replay_buffer, ffmpeg_hls_muxer;

struct obs_output *ffo_O;
static struct obs_output_info *OI;
static void *ST; /* struct ffmpeg_muxer * */
static unsigned pkt_seq;

#define MAXPKT 512
static uint8_t *pool[MAXPKT];
static int npool;

static int64_t I(const char *s)
{
	return (int64_t)strtoll(s, NULL, 0);
}
static bool B(const char *s)
{
	return s[0] == '1' || s[0] == 't';
}

static void world_reset(void)
{
	if (ffo_O) {
		fd_free(ffo_O->settings);
		fd_free(ffo_O->venc ? ffo_O->venc->settings : NULL);
		for (int i = 0; i < 6; i++)
			fd_free(ffo_O->aenc[i].settings);
		free(ffo_O->venc);
		free(ffo_O->service);
		free(ffo_O);
	}
	for (int i = 0; i < npool; i++)
		free(pool[i]);
	npool = 0;
	memset(&W, 0, sizeof(W));
	W.fps_num = 30;
	W.fps_den = 1;
	W.colorspace = VIDEO_CS_709;
	W.range = VIDEO_RANGE_PARTIAL;
	W.vformat = VIDEO_FORMAT_NV12;
	W.channels = 2;
	W.hdr_peak = 1000.0f;
	W.sample_rate = 48000;
	W.speakers = SPEAKERS_STEREO;
	W.have_video_info = W.have_audio_info = true;
	W.base_w = 1920;
	W.base_h = 1080;
	W.now_ns = 1000000000ULL;
	ffo_O = NULL;
	OI = NULL;
	ST = NULL;
	pkt_seq = 0;
}

static void new_output(const char *name, uint32_t flags)
{
	ffo_O = calloc(1, sizeof(*ffo_O));
	ffo_O->settings = fd_create();
	snprintf(ffo_O->name, sizeof(ffo_O->name), "%s", name);
	ffo_O->flags = flags;
	ffo_O->width = 1280;
	ffo_O->height = 720;
	ffo_O->can_begin = ffo_O->init_enc_ok = true;
}

static uint8_t *unhex(const char *s, size_t *n)
{
	size_t l = strlen(s) / 2;
	uint8_t *b = malloc(l ? l : 1);
	for (size_t i = 0; i < l; i++)
		b[i] = (uint8_t)(script_hexval(s[2 * i]) * 16 + script_hexval(s[2 * i + 1]));
	*n = l;
	return b;
}

static const char *kvget(int argc, char **argv, const char *k)
{
	size_t kl = strlen(k);
	for (int i = 0; i < argc; i++)
		if (!strncmp(argv[i], k, kl) && argv[i][kl] == '=')
			return argv[i] + kl + 1;
	return NULL;
}

static void set_extra(struct obs_encoder *e, const char *hex)
{
	if (!hex || !strcmp(hex, "-")) {
		e->has_extra = false;
		return;
	}
	size_t n;
	uint8_t *b = unhex(hex, &n);
	memcpy(e->extra, b, n > 16 ? 16 : n);
	e->extra_size = n > 16 ? 16 : n;
	e->has_extra = true;
	free(b);
}

static void fill_enc_settings(struct obs_encoder *e, int argc, char **argv)
{
	const char *v;
	if ((v = kvget(argc, argv, "bitrate")))
		obs_data_set_int(e->settings, "bitrate", I(v));
	if ((v = kvget(argc, argv, "codec_type")))
		obs_data_set_int(e->settings, "codec_type", I(v));
	if ((v = kvget(argc, argv, "keyint_sec")))
		obs_data_set_int(e->settings, "keyint_sec", I(v));
	if ((v = kvget(argc, argv, "extra")))
		set_extra(e, v);
}

/* state printers: "state" (common), "state_rb" (replay buffer), "state_split" (split file), "state_hls" */
static void print_state(const char *kind)
{
	struct ffmpeg_muxer *s = ST;
	if (!s) {
		printf("state none\n");
		return;
	}
	if (!kind[5]) {
		printf("state active=%d capturing=%d stopping=%d sent_headers=%d total_bytes=%" PRIu64 " stop_ts=%" PRId64
		       " pipe=%d refs=%ld\n",
		       (int)s->active, (int)s->capturing, (int)s->stopping, (int)s->sent_headers, s->total_bytes,
		       s->stop_ts, s->pipe != NULL, W.pkt_refs);
		printf("  path=\"%s\" is_network=%d is_hls=%d\n", s->path.array ? s->path.array : "", (int)s->is_network,
		       (int)s->is_hls);
	} else if (!strcmp(kind, "state_rb")) {
		printf("state_rb save_ts=%" PRId64 " keyframes=%d muxing=%d queued=%zu mux_packets=%zu cur_size=%" PRId64
		       " cur_time=%" PRId64 " max_size=%" PRId64 " max_time=%" PRId64 "\n",
		       s->save_ts, s->keyframes, (int)s->muxing, s->packets.size / sizeof(struct encoder_packet),
		       s->mux_packets.num, s->cur_size, s->cur_time, s->max_size, s->max_time);
	} else if (!strcmp(kind, "state_split")) {
		printf("state_split split_file=%d allow_overwrite=%d cur_size=%" PRId64 " cur_time=%" PRId64
		       " max_size=%" PRId64 " max_time=%" PRId64 " manual=%d ready=%d mux_packets=%zu\n",
		       (int)s->split_file, (int)s->allow_overwrite, s->cur_size, s->cur_time, s->max_size, s->max_time,
		       (int)s->manual_split, (int)s->split_file_ready, s->mux_packets.num);
		printf("  found_video=%d video_pts_offset=%" PRId64 " found_audio=%d%d audio_dts_offsets=%" PRId64 ",%" PRId64
		       "\n",
		       (int)s->found_video, s->video_pts_offset, (int)s->found_audio[0], (int)s->found_audio[1],
		       s->audio_dts_offsets[0], s->audio_dts_offsets[1]);
	} else {
		printf("state_hls dropped=%d min_priority=%d last_dts_usec=%" PRId64 " keyint_sec=%d queued=%zu\n",
		       s->dropped_frames, s->min_priority, s->last_dts_usec, s->keyint_sec,
		       s->packets.size / sizeof(struct encoder_packet));
		printf("  muxer_settings=\"%s\" stream_key=\"%s\" printable=\"%s\"\n",
		       s->muxer_settings.array ? s->muxer_settings.array : "", s->stream_key.array ? s->stream_key.array : "",
		       s->printable_path.array ? s->printable_path.array : "");
	}
}

static void print_queue(void)
{
	struct ffmpeg_muxer *s = ST;
	size_t n = s->packets.size / sizeof(struct encoder_packet);
	printf("queue %zu\n", n);
	for (size_t i = 0; i < n; i++) {
		struct encoder_packet *p = deque_data(&s->packets, i * sizeof(*p));
		printf("  [%zu] %s track=%zu pts=%" PRId64 " dts=%" PRId64 " dts_usec=%" PRId64 " key=%d size=%zu dprio=%d\n",
		       i, p->type == OBS_ENCODER_VIDEO ? "V" : "A", p->track_idx, p->pts, p->dts, p->dts_usec,
		       (int)p->keyframe, p->size, p->drop_priority);
	}
	printf("mux_packets %zu\n", s->mux_packets.num);
	for (size_t i = 0; i < s->mux_packets.num; i++) {
		struct encoder_packet *p = &s->mux_packets.array[i];
		printf("  [%zu] %s track=%zu pts=%" PRId64 " dts=%" PRId64 " dts_usec=%" PRId64 " key=%d size=%zu\n", i,
		       p->type == OBS_ENCODER_VIDEO ? "V" : "A", p->track_idx, p->pts, p->dts, p->dts_usec,
		       (int)p->keyframe, p->size);
	}
}

static void do_pkt(int argc, char **argv)
{
	/* pkt v|a key=val ... */
	struct encoder_packet p = {0};
	bool video = argv[1][0] == 'v';
	const char *v;
	p.type = video ? OBS_ENCODER_VIDEO : OBS_ENCODER_AUDIO;
	p.timebase_num = 1;
	p.timebase_den = (v = kvget(argc, argv, "tb")) ? (int32_t)I(v) : 1000;
	p.pts = (v = kvget(argc, argv, "pts")) ? I(v) : 0;
	p.dts = (v = kvget(argc, argv, "dts")) ? I(v) : p.pts;
	p.dts_usec = (v = kvget(argc, argv, "dus")) ? I(v) : p.dts * 1000000 / p.timebase_den;
	p.sys_dts_usec = (v = kvget(argc, argv, "sys")) ? I(v) : p.dts_usec;
	p.keyframe = (v = kvget(argc, argv, "key")) ? B(v) : false;
	p.track_idx = (v = kvget(argc, argv, "track")) ? (size_t)I(v) : 0;
	p.priority = (v = kvget(argc, argv, "prio")) ? (int)I(v) : 0;
	p.drop_priority = (v = kvget(argc, argv, "dprio")) ? (int)I(v) : 0;
	p.encoder = video ? (obs_encoder_t *)ffo_O->venc : (obs_encoder_t *)&ffo_O->aenc[p.track_idx];
	if ((v = kvget(argc, argv, "data"))) {
		p.data = unhex(v, &p.size);
	} else {
		p.size = (v = kvget(argc, argv, "size")) ? (size_t)I(v) : 8;
		p.data = malloc(p.size ? p.size : 1);
		for (size_t i = 0; i < p.size; i++)
			p.data[i] = (uint8_t)(pkt_seq * 16 + i);
	}
	pkt_seq++;
	if (npool < MAXPKT)
		pool[npool++] = p.data;
	if (kvget(argc, argv, "null")) {
		OI->encoded_packet(ST, NULL);
		return;
	}
	OI->encoded_packet(ST, &p);
	if (video && OI == &ffmpeg_hls_muxer)
		printf("  parsed drop_priority=%d\n", p.drop_priority);
}

static const struct {
	const char *n;
	int v;
} vfmts[] = {{"I420", VIDEO_FORMAT_I420}, {"NV12", VIDEO_FORMAT_NV12}, {"I444", VIDEO_FORMAT_I444},
	     {"YUY2", VIDEO_FORMAT_YUY2},   {"UYVY", VIDEO_FORMAT_UYVY}, {"BGRA", VIDEO_FORMAT_BGRA},
	     {"P010", VIDEO_FORMAT_P010},   {"I010", VIDEO_FORMAT_I010}, {"I422", VIDEO_FORMAT_I422},
	     {"I210", VIDEO_FORMAT_I210},   {"NONE", VIDEO_FORMAT_NONE}, {0, 0}};
static int vfmt(const char *s)
{
	for (int i = 0; vfmts[i].n; i++)
		if (!strcmp(vfmts[i].n, s))
			return vfmts[i].v;
	fprintf(stderr, "bad format %s\n", s);
	exit(2);
}
static int csv(const char *s)
{
	static const char *n[] = {"DEFAULT", "601", "709", "SRGB", "PQ", "HLG"};
	for (int i = 0; i < 6; i++)
		if (!strcmp(n[i], s))
			return i;
	return (int)I(s);
}

static void run_cmd(struct script_line *sl);

static void run_cmd(struct script_line *sl)
{
	int argc = sl->argc;
	char **argv = sl->argv;
	const char *c = argv[0];

	if (!strcmp(c, "probe")) {
		/* probe CMD...: run one command in a forked child with a 2 s alarm (reference hangs / crashes) */
		fflush(stdout);
		pid_t pid = fork();
		if (pid == 0) {
			alarm(2);
			for (int i = 1; i < sl->argc; i++)
				sl->argv[i - 1] = sl->argv[i];
			sl->argc--;
			run_cmd(sl);
			fflush(stdout);
			_exit(0);
		}
		int st = 0;
		waitpid(pid, &st, 0);
		if (WIFSIGNALED(st))
			printf("probe signal=%d%s\n", WTERMSIG(st), WTERMSIG(st) == SIGALRM ? " (hang)" : "");
		else
			printf("probe exit=%d\n", WEXITSTATUS(st));
		return;
	}
	if (!strncmp(c, "fo_", 3)) {
		ffo_out_command(argc, argv);
		return;
	}
	if (!strcmp(c, "reset")) {
		ffo_out_reset();
		world_reset();
	} else if (!strcmp(c, "output")) {
		uint32_t fl = 0;
		for (int i = 2; i < argc; i++) {
			if (!strcmp(argv[i], "service"))
				fl |= OBS_OUTPUT_SERVICE;
			if (!strcmp(argv[i], "av"))
				fl |= OBS_OUTPUT_AV | OBS_OUTPUT_ENCODED;
		}
		new_output(argv[1], fl);
	} else if (!strcmp(c, "size")) {
		ffo_O->width = (uint32_t)I(argv[1]);
		ffo_O->height = (uint32_t)I(argv[2]);
	} else if (!strcmp(c, "venc")) {
		ffo_O->venc = calloc(1, sizeof(*ffo_O->venc));
		snprintf(ffo_O->venc->codec, sizeof(ffo_O->venc->codec), "%s", argv[1]);
		snprintf(ffo_O->venc->name, sizeof(ffo_O->venc->name), "vid");
		ffo_O->venc->settings = fd_create();
		fill_enc_settings(ffo_O->venc, argc - 2, argv + 2);
	} else if (!strcmp(c, "no_venc")) {
		free(ffo_O->venc);
		ffo_O->venc = NULL;
	} else if (!strcmp(c, "aenc")) {
		/* aenc NAME CODEC RATE FRAME PRIMING [bitrate=..] [extra=..] */
		struct obs_encoder *e = &ffo_O->aenc[ffo_O->naenc++];
		snprintf(e->name, sizeof(e->name), "%s", argv[1]);
		snprintf(e->codec, sizeof(e->codec), "%s", argv[2]);
		e->sample_rate = (uint32_t)I(argv[3]);
		e->frame_size = (uint32_t)I(argv[4]);
		e->priming = (uint32_t)I(argv[5]);
		e->settings = fd_create();
		fill_enc_settings(e, argc - 6, argv + 6);
	} else if (!strcmp(c, "venc_extra")) {
		set_extra(ffo_O->venc, argv[1]);
	} else if (!strcmp(c, "aenc_extra")) {
		set_extra(&ffo_O->aenc[I(argv[1])], argv[2]);
	} else if (!strcmp(c, "venc_set")) {
		obs_data_set_int(ffo_O->venc->settings, argv[1], I(argv[2]));
	} else if (!strcmp(c, "venc_paused")) {
		ffo_O->venc->paused = B(argv[1]);
	} else if (!strcmp(c, "video_info")) {
		W.fps_num = (uint32_t)I(argv[1]);
		W.fps_den = (uint32_t)I(argv[2]);
		W.colorspace = csv(argv[3]);
		W.range = argv[4][0] == 'F' ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
		W.vformat = vfmt(argv[5]);
	} else if (!strcmp(c, "hdr_peak")) {
		W.hdr_peak = (float)I(argv[1]);
	} else if (!strcmp(c, "mixers")) {
		ffo_O->mixers = (size_t)I(argv[1]);
	} else if (!strcmp(c, "speakers")) {
		W.speakers = (int)I(argv[1]);
		W.have_audio_info = W.speakers >= 0;
	} else if (!strcmp(c, "rate")) {
		W.sample_rate = (uint32_t)I(argv[1]);
	} else if (!strcmp(c, "no_video_info")) {
		W.have_video_info = false;
	} else if (!strcmp(c, "names")) {
		/* names A B ... : settings array "audio_names" with items {name: X}; "-" = array item without a name key */
		struct obs_data *d = ffo_O->settings;
		struct obs_data_array *a = calloc(1, sizeof(*a));
		for (int i = 1; i < argc && a->n < 8; i++) {
			struct obs_data *it = fd_create();
			if (strcmp(argv[i], "-"))
				obs_data_set_string(it, "name", argv[i]);
			a->items[a->n++] = it;
		}
		snprintf(d->akey[d->na], sizeof(d->akey[0]), "audio_names");
		d->arrays[d->na++] = a;
	} else if (!strcmp(c, "channels")) {
		W.channels = (uint32_t)I(argv[1]);
	} else if (!strcmp(c, "set")) {
		if (!strcmp(argv[1], "int"))
			obs_data_set_int(ffo_O->settings, argv[2], I(argv[3]));
		else if (!strcmp(argv[1], "bool"))
			obs_data_set_bool(ffo_O->settings, argv[2], B(argv[3]));
		else
			obs_data_set_string(ffo_O->settings, argv[2], argv[3]);
	} else if (!strcmp(c, "service")) {
		free(ffo_O->service);
		ffo_O->service = NULL;
		if (strcmp(argv[1], "-")) {
			ffo_O->service = calloc(1, sizeof(*ffo_O->service));
			snprintf(ffo_O->service->url, sizeof(ffo_O->service->url), "%s", argv[1]);
			snprintf(ffo_O->service->key, sizeof(ffo_O->service->key), "%s", argc > 2 ? argv[2] : "");
		}
	} else if (!strcmp(c, "capture")) {
		ffo_O->can_begin = B(argv[1]);
		ffo_O->init_enc_ok = argc > 2 ? B(argv[2]) : true;
	} else if (!strcmp(c, "knob")) {
		const char *k = argv[1];
		if (!strcmp(k, "pipe_fail"))
			W.pipe_fail = B(argv[2]);
		else if (!strcmp(k, "write_fail_at"))
			W.write_fail_at = (int)I(argv[2]);
		else if (!strcmp(k, "destroy_ret"))
			W.destroy_ret = (int)I(argv[2]);
		else if (!strcmp(k, "err_text"))
			snprintf(W.err_text, sizeof(W.err_text), "%s", strcmp(argv[2], "-") ? argv[2] : "");
		else if (!strcmp(k, "now_ns"))
			W.now_ns = (uint64_t)I(argv[2]);
		else if (!strcmp(k, "defer_threads"))
			W.deferred_threads = B(argv[2]);
		else {
			fprintf(stderr, "bad knob %s\n", k);
			exit(2);
		}
	} else if (!strcmp(c, "fs")) {
		if (!strcmp(argv[1], "add") && W.nfiles < 16)
			snprintf(W.files[W.nfiles++], sizeof(W.files[0]), "%s", argv[2]);
		else if (!strcmp(argv[1], "deny") && W.ndeny < 4)
			snprintf(W.deny[W.ndeny++], sizeof(W.deny[0]), "%s", argv[2]);
		else if (!strcmp(argv[1], "clear"))
			W.nfiles = W.ndeny = 0;
	} else if (!strcmp(c, "create")) {
		const char *id = argv[1];
		OI = !strcmp(id, "ffmpeg_muxer")       ? &ffmpeg_muxer
		     : !strcmp(id, "ffmpeg_mpegts_muxer") ? &ffmpeg_mpegts_muxer
		     : !strcmp(id, "replay_buffer")    ? &replay_buffer
						       : &ffmpeg_hls_muxer;
		ffo_thread_mode = OI == &ffmpeg_hls_muxer ? 2 : (W.deferred_threads ? 1 : 0);
		ST = OI->create(ffo_O->settings, (obs_output_t *)ffo_O);
		printf("create %s -> %s\n", id, ST ? "ok" : "NULL");
	} else if (!strcmp(c, "defaults")) {
		if (OI->get_defaults)
			OI->get_defaults(ffo_O->settings);
		else
			printf("no defaults callback\n");
	} else if (!strcmp(c, "settings")) {
		printf("settings\n");
		fd_dump(ffo_O->settings);
	} else if (!strcmp(c, "info")) {
		printf("info id=%s flags=0x%x protocols=%s vcodecs=%s acodecs=%s\n", OI->id, OI->flags,
		       OI->protocols ? OI->protocols : "-", OI->encoded_video_codecs ? OI->encoded_video_codecs : "-",
		       OI->encoded_audio_codecs ? OI->encoded_audio_codecs : "-");
	} else if (!strcmp(c, "getname")) {
		printf("name %s\n", OI->get_name(NULL));
	} else if (!strcmp(c, "props")) {
		if (OI->get_properties)
			OI->get_properties(ST);
		else
			printf("no properties callback\n");
	} else if (!strcmp(c, "start")) {
		bool r = OI->start(ST);
		printf("start -> %d\n", (int)r);
	} else if (!strcmp(c, "stop")) {
		OI->stop(ST, (uint64_t)I(argv[1]));
	} else if (!strcmp(c, "destroy")) {
		OI->destroy(ST);
		ST = NULL;
	} else if (!strcmp(c, "pkt")) {
		do_pkt(argc, argv);
	} else if (!strcmp(c, "proc")) {
		for (int i = 0; i < ffo_O->nprocs; i++)
			if (!strcmp(ffo_O->procs[i].name, argv[1])) {
				calldata_t cd = {0};
				ffo_O->procs[i].fn(ffo_O->procs[i].param, &cd);
				bool b;
				const char *s;
				if (calldata_get_bool(&cd, "split_file_enabled", &b))
					printf("  out split_file_enabled=%d\n", (int)b);
				if (calldata_get_string(&cd, "path", &s))
					{ if (s) printf("  out path=\"%s\"\n", s); else printf("  out path=<null>\n"); }
				calldata_free(&cd);
				return;
			}
		printf("no such proc %s\n", argv[1]);
	} else if (!strcmp(c, "run_pending")) {
		ffo_run_pending();
	} else if (!strcmp(c, "pump")) {
		ffo_gate_pump((int)I(argv[1]));
	} else if (!strncmp(c, "state", 5)) {
		print_state(c);
	} else if (!strcmp(c, "queue")) {
		print_queue();
	} else if (!strcmp(c, "total_bytes")) {
		printf("total_bytes %" PRIu64 "\n", OI->get_total_bytes(ST));
	} else if (!strcmp(c, "dropped")) {
		printf("dropped %d\n", OI->get_dropped_frames ? OI->get_dropped_frames(ST) : -1);
	} else if (!strcmp(c, "echo")) {
		printf("%s\n", argc > 1 ? argv[1] : "");
	} else if (!strcmp(c, "set_field")) {
		struct ffmpeg_muxer *s = ST;
		if (!strcmp(argv[1], "last_dts_usec"))
			s->last_dts_usec = I(argv[2]);
		else if (!strcmp(argv[1], "stream_key"))
			dstr_copy(&s->stream_key, argv[2]);
		else if (!strcmp(argv[1], "muxer_settings"))
			dstr_copy(&s->muxer_settings, argv[2]);
		else if (!strcmp(argv[1], "printable_path"))
			dstr_copy(&s->printable_path, argv[2]);
		else if (!strcmp(argv[1], "keyint_sec"))
			s->keyint_sec = (int)I(argv[2]);
		else if (!strcmp(argv[1], "cur_size"))
			s->cur_size = I(argv[2]);
		else {
			fprintf(stderr, "bad field %s\n", argv[1]);
			exit(2);
		}
	} else {
		fprintf(stderr, "unknown command %s\n", c);
		exit(2);
	}
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	world_reset();
	while (script_next(f, &sl, &lineno)) {
		printf("> ");
		for (int i = 0; i < sl.argc; i++)
			printf("%s%s", i ? " " : "", sl.argv[i]);
		printf("\n");
		run_cmd(&sl);
	}
	fclose(f);
	return 0;
}
