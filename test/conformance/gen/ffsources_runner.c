/*
 * Conformance runner for the obs-ffmpeg media source and encoder wrappers (spec S-047). See the
 * section "ffsources runner" in test/conformance/README.md for the command list.
 *
 * Drives the REAL code of plugins/obs-ffmpeg/obs-ffmpeg-source.c, obs-ffmpeg-audio-encoders.c and
 * obs-ffmpeg-video-encoders.c (+ the inline helpers of obs-ffmpeg-formats.h) against the real
 * obs-data.c / obs-properties.c. libavcodec, the OBS host and media-playback are recording
 * fakes (ffsources_stubs.c); libavutil is the real system library.
 */
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include "obs.h"
#include "obs-properties.h"
#include "util/bmem.h"
#include "harness.h"
#include "ffsources_api.h"
#include "../../../plugins/obs-ffmpeg/obs-ffmpeg-video-encoders.h"

#define MAXN 64
static struct {
	char name[32];
	void *p;
	int kind; /* 1 data, 2 props, 3 source, 4 audio enc, 5 video enc */
} objs[MAXN];

static void die(const char *m, const char *a)
{
	fprintf(stderr, "script: %s %s\n", m, a ? a : "");
	exit(2);
}
static void *get(const char *n, int kind)
{
	for (int i = 0; i < MAXN; i++)
		if (objs[i].p && objs[i].kind == kind && !strcmp(objs[i].name, n))
			return objs[i].p;
	die("unknown object", n);
	return NULL;
}
static void *get_or_null(const char *n, int kind)
{
	return !strcmp(n, "@null") ? NULL : get(n, kind);
}
static void put(const char *n, void *p, int kind)
{
	for (int i = 0; i < MAXN; i++)
		if (objs[i].p && objs[i].kind == kind && !strcmp(objs[i].name, n)) {
			if (kind == 1)
				obs_data_release(objs[i].p);
			else if (kind == 2)
				obs_properties_destroy(objs[i].p);
			objs[i].p = p;
			return;
		}
	for (int i = 0; i < MAXN; i++)
		if (!objs[i].p) {
			snprintf(objs[i].name, sizeof(objs[i].name), "%s", n);
			objs[i].p = p;
			objs[i].kind = kind;
			return;
		}
	die("too many objects", n);
}
static void drop(const char *n, int kind)
{
	for (int i = 0; i < MAXN; i++)
		if (objs[i].p && objs[i].kind == kind && !strcmp(objs[i].name, n)) {
			objs[i].p = NULL;
			return;
		}
	die("unknown object", n);
}
#define DAT(n) ((obs_data_t *)get(n, 1))

static void pq(const char *s)
{
	if (!s)
		fputs("@null", stdout);
	else
		script_print_quoted(s);
}

static const struct {
	const char *n;
	int v;
} vfmt[] = {{"none", VIDEO_FORMAT_NONE}, {"I420", VIDEO_FORMAT_I420}, {"NV12", VIDEO_FORMAT_NV12},
	    {"I444", VIDEO_FORMAT_I444}, {"I010", VIDEO_FORMAT_I010}, {"P010", VIDEO_FORMAT_P010},
	    {"BGRA", VIDEO_FORMAT_BGRA}, {"I422", VIDEO_FORMAT_I422}, {"YUY2", VIDEO_FORMAT_YUY2},
	    {"RGBA", VIDEO_FORMAT_RGBA}, {"I40A", VIDEO_FORMAT_I40A}, {"YUVA", VIDEO_FORMAT_YUVA},
	    {"Y800", VIDEO_FORMAT_Y800}, {NULL, 0}};
static const struct {
	const char *n;
	int v;
} vcs[] = {{"default", VIDEO_CS_DEFAULT}, {"601", VIDEO_CS_601},     {"709", VIDEO_CS_709},
	   {"srgb", VIDEO_CS_SRGB},       {"pq", VIDEO_CS_2100_PQ}, {"hlg", VIDEO_CS_2100_HLG},
	   {NULL, 0}};
static int lookup(const char *what, const char *n, const void *tab_)
{
	const struct {
		const char *n;
		int v;
	} *tab = tab_;
	for (; tab->n; tab++)
		if (!strcmp(tab->n, n))
			return tab->v;
	die(what, n);
	return 0;
}

static const char *ptype_name(enum obs_property_type t)
{
	static const char *const names[] = {"invalid", "bool",  "int",  "float",         "text",   "path",
					    "list",    "color", "button", "font",      "editable_list", "frame_rate",
					    "group",   "color_alpha"};
	return (unsigned)t < sizeof(names) / sizeof(*names) ? names[t] : "?";
}

static void dump_props(const char *label, obs_properties_t *props)
{
	printf("props %s flags=%u\n", label, obs_properties_get_flags(props));
	for (obs_property_t *p = obs_properties_first(props); p; obs_property_next(&p)) {
		enum obs_property_type t = obs_property_get_type(p);
		printf("  %s %s desc=", obs_property_name(p), ptype_name(t));
		pq(obs_property_description(p));
		printf(" visible=%d enabled=%d", obs_property_visible(p) ? 1 : 0, obs_property_enabled(p) ? 1 : 0);
		if (obs_property_long_description(p)) {
			printf(" long=");
			pq(obs_property_long_description(p));
		}
		if (t == OBS_PROPERTY_INT) {
			printf(" min=%d max=%d step=%d numtype=%d", obs_property_int_min(p), obs_property_int_max(p),
			       obs_property_int_step(p), (int)obs_property_int_type(p));
			if (obs_property_int_suffix(p)) {
				printf(" suffix=");
				pq(obs_property_int_suffix(p));
			}
		} else if (t == OBS_PROPERTY_TEXT) {
			printf(" texttype=%d", (int)obs_property_text_type(p));
		} else if (t == OBS_PROPERTY_PATH) {
			printf(" pathtype=%d filter=", (int)obs_property_path_type(p));
			pq(obs_property_path_filter(p));
			printf(" default_path=");
			pq(obs_property_path_default_path(p));
		} else if (t == OBS_PROPERTY_LIST) {
			enum obs_combo_format f = obs_property_list_format(p);
			printf(" combo=%d format=%d count=%d", (int)obs_property_list_type(p), (int)f,
			       (int)obs_property_list_item_count(p));
			for (size_t i = 0; i < obs_property_list_item_count(p); i++) {
				printf("\n    item %d ", (int)i);
				pq(obs_property_list_item_name(p, i));
				printf(" = ");
				if (f == OBS_COMBO_FORMAT_INT)
					printf("%lld", obs_property_list_item_int(p, i));
				else if (f == OBS_COMBO_FORMAT_STRING)
					pq(obs_property_list_item_string(p, i));
			}
		}
		putchar('\n');
	}
	printf("end props %s\n", label);
}

static void dump_data(const char *label, obs_data_t *d)
{
	printf("data %s\n", label);
	for (obs_data_item_t *it = obs_data_first(d); it; obs_data_item_next(&it)) {
		printf("  %s ", obs_data_item_get_name(it));
		switch (obs_data_item_gettype(it)) {
		case OBS_DATA_STRING:
			printf("string ");
			pq(obs_data_item_get_string(it));
			break;
		case OBS_DATA_NUMBER:
			printf("int %lld", obs_data_item_get_int(it));
			break;
		case OBS_DATA_BOOLEAN:
			printf("bool %d", obs_data_item_get_bool(it) ? 1 : 0);
			break;
		default:
			printf("other");
		}
		printf(" %s\n", obs_data_item_has_user_value(it) ? "user" : "default");
	}
	printf("end data %s\n", label);
}

/* ---- video encoder wrapper ---------------------------------------------- */

static void on_init_error(void *parent, int ret)
{
	(void)parent;
	printf("cb.init_error ret=%d\n", ret);
}
static void on_first_packet(void *parent, AVPacket *pkt, struct darray *out)
{
	(void)parent;
	printf("cb.first_packet size=%d\n", pkt->size);
	static const uint8_t hdr[3] = {'H', 'D', 'R'};
	darray_push_back_array(1, out, hdr, 3);
	darray_push_back_array(1, out, pkt->data, (size_t)pkt->size);
}

static void vid_update_cmd(struct ffmpeg_video_encoder *e, int bitrate, int keyint, const char *opts)
{
	struct video_output_info voi = {0};
	voi.fps_num = g_host.fps_num;
	voi.fps_den = g_host.fps_den;
	struct video_scale_info info = {0};
	info.format = g_host.format;
	info.colorspace = g_host.colorspace;
	info.range = g_host.range;
	ffmpeg_video_encoder_update(e, bitrate, keyint, &voi, &info, opts);
}

static void vid_encode_cmd(struct ffmpeg_video_encoder *e, int64_t pts, const int *ls)
{
	struct encoder_frame f;
	struct encoder_packet pk;
	memset(&f, 0, sizeof(f));
	memset(&pk, 0, sizeof(pk));
	uint8_t *bufs[4] = {0};
	for (int p = 0; p < 4; p++) {
		if (ls[p] <= 0)
			continue;
		size_t sz = (size_t)ls[p] * g_host.height;
		bufs[p] = malloc(sz);
		for (size_t i = 0; i < sz; i++) {
			size_t y = i / (size_t)ls[p], x = i % (size_t)ls[p];
			bufs[p][i] = (uint8_t)(p * 0x40 + y * 0x10 + x);
		}
		f.data[p] = bufs[p];
		f.linesize[p] = (uint32_t)ls[p];
	}
	f.pts = pts;
	if (e->vframe)
		fs_fill_sentinel(e->vframe);
	bool received = false;
	bool r = ffmpeg_video_encode(e, &f, &pk, &received);
	printf("encode -> %d received=%d", r, received);
	if (received) {
		printf(" pts=%lld dts=%lld size=%zu type=%d keyframe=%d data=", (long long)pk.pts, (long long)pk.dts,
		       pk.size, (int)pk.type, pk.keyframe);
		script_print_hex(pk.data, pk.size > 12 ? 12 : pk.size);
	}
	putchar('\n');
	for (int p = 0; p < 4; p++)
		free(bufs[p]);
}

/* The reference declares a struct dstr without initialising it before dstr_printf() in the
 * "codec not found" path of ffmpeg_video_encoder_init() (defect D-1): the outcome depends on the
 * stack contents. The runner reproduces both outcomes in a child process: the stack below the
 * caller is first filled with a byte value (0x00 = works, 0xFF = crash). */
static void __attribute__((noinline)) poison_stack(int byte)
{
	volatile unsigned char buf[16384];
	for (size_t i = 0; i < sizeof(buf); i++)
		buf[i] = (unsigned char)byte;
}
static void init_in_child(int byte, const char *lib, const char *lib2, const char *name)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		struct ffmpeg_video_encoder *e = calloc(1, sizeof(*e));
		poison_stack(byte);
		bool r = ffmpeg_video_encoder_init(e, e, (obs_encoder_t *)&g_host, lib, lib2, name, NULL, NULL);
		printf("child: init -> %d\n", r);
		printf("child: last_error ");
		pq(g_host.has_last_error ? g_host.last_error : NULL);
		putchar('\n');
		ffmpeg_video_encoder_free(e);
		printf("child: freed\n");
		fflush(stdout);
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	if (WIFSIGNALED(st))
		printf("crash (child killed by a signal)\n");
	else
		printf("child exit %d\n", WEXITSTATUS(st));
}

#define IS(c) (!strcmp(a[0], c))

static void run(struct script_line *sl)
{
	char **a = sl->argv;
	int n = sl->argc;

	if (IS("echo")) {
		printf("echo");
		for (int i = 1; i < n; i++)
			printf(" %s", a[i]);
		putchar('\n');
	} else if (IS("data")) {
		script_need(sl, 2);
		put(a[1], obs_data_create(), 1);
	} else if (IS("set_bool")) {
		script_need(sl, 4);
		obs_data_set_bool(DAT(a[1]), a[2], atoi(a[3]));
	} else if (IS("set_int")) {
		script_need(sl, 4);
		obs_data_set_int(DAT(a[1]), a[2], atoll(a[3]));
	} else if (IS("set_str")) {
		script_need(sl, 4);
		obs_data_set_string(DAT(a[1]), a[2], a[3]);
	} else if (IS("dump_data")) {
		script_need(sl, 2);
		dump_data(a[1], DAT(a[1]));
	} else if (IS("dump_props")) {
		script_need(sl, 2);
		dump_props(a[1], get(a[1], 2));
	}
	/* ---- host ---- */
	else if (IS("host_int")) {
		script_need(sl, 3);
		static const struct {
			const char *n;
			int *p;
		} t[] = {{"active", &g_host.active},
			 {"showing", &g_host.showing},
			 {"media_fail", &g_host.media_fail},
			 {"media_has_video", &g_host.media_has_video},
			 {"event_init_fail", &g_host.event_init_fail},
			 {"mutex_init_fail", &g_host.mutex_init_fail},
			 {"thread_create_fail", &g_host.thread_create_fail},
			 {"open_rc", &g_host.open_rc},
			 {"fill_rc", &g_host.fill_rc},
			 {"frame_size", &g_host.frame_size_after_open},
			 {"tb_num", &g_host.tb_num},
			 {"tb_den", &g_host.tb_den},
			 {"extradata", &g_host.extradata_size},
			 {"initial_padding", &g_host.initial_padding},
			 {NULL, NULL}};
		for (int i = 0; t[i].n; i++)
			if (!strcmp(t[i].n, a[1])) {
				*t[i].p = atoi(a[2]);
				return;
			}
		if (!strcmp(a[1], "duration_us"))
			g_host.media_duration_us = atoll(a[2]);
		else if (!strcmp(a[1], "frames"))
			g_host.media_frames = atoll(a[2]);
		else if (!strcmp(a[1], "time"))
			g_host.media_time = atoll(a[2]);
		else if (!strcmp(a[1], "now_ns"))
			g_host.now_ns = (uint64_t)atoll(a[2]);
		else if (!strcmp(a[1], "pause_offset"))
			g_host.pause_offset = (uint64_t)atoll(a[2]);
		else
			die("unknown host key", a[1]);
	} else if (IS("host_settings")) {
		script_need(sl, 2);
		g_host.src_settings = DAT(a[1]);
	} else if (IS("host_last_error")) {
		script_need(sl, 2);
		if (!strcmp(a[1], "@null")) {
			g_host.has_last_error = 0;
		} else {
			snprintf(g_host.last_error, sizeof(g_host.last_error), "%s", a[1]);
			g_host.has_last_error = 1;
		}
	} else if (IS("show_last_error")) {
		printf("last_error ");
		pq(g_host.has_last_error ? g_host.last_error : NULL);
		putchar('\n');
	} else if (IS("host_video")) {
		script_need(sl, 8);
		g_host.width = (uint32_t)atoi(a[1]);
		g_host.height = (uint32_t)atoi(a[2]);
		g_host.format = lookup("video format", a[3], vfmt);
		g_host.colorspace = lookup("colorspace", a[4], vcs);
		g_host.range = !strcmp(a[5], "full") ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
		g_host.fps_num = (uint32_t)atoi(a[6]);
		g_host.fps_den = (uint32_t)atoi(a[7]);
	} else if (IS("host_audio")) {
		script_need(sl, 4);
		g_host.channels = atoi(a[1]);
		g_host.speakers = atoi(a[2]);
		g_host.sample_rate = (uint32_t)atoi(a[3]);
	} else if (IS("exists")) {
		script_need(sl, 3);
		fs_set_exists(a[1], atoi(a[2]));
	}
	/* ---- source ---- */
	else if (IS("src_table")) {
		fsw_table();
	} else if (IS("src_defaults")) {
		script_need(sl, 2);
		fsw_defaults(DAT(a[1]));
	} else if (IS("src_create")) {
		script_need(sl, 3);
		void *s = fsw_create(DAT(a[2]));
		printf("src_create %s\n", s ? "ok" : "null");
		if (s)
			put(a[1], s, 3);
	} else if (IS("src_update")) {
		script_need(sl, 3);
		fsw_update(get(a[1], 3), DAT(a[2]));
	} else if (IS("src_destroy")) {
		script_need(sl, 2);
		fsw_destroy(get(a[1], 3));
		drop(a[1], 3);
	} else if (IS("src_tick")) {
		script_need(sl, 2);
		fsw_tick(get(a[1], 3));
	} else if (IS("src_activate")) {
		script_need(sl, 2);
		fsw_activate(get(a[1], 3));
	} else if (IS("src_deactivate")) {
		script_need(sl, 2);
		fsw_deactivate(get(a[1], 3));
	} else if (IS("src_play_pause")) {
		script_need(sl, 3);
		fsw_play_pause(get(a[1], 3), atoi(a[2]));
	} else if (IS("src_stop")) {
		script_need(sl, 2);
		fsw_stop(get(a[1], 3));
	} else if (IS("src_restart")) {
		script_need(sl, 2);
		fsw_restart(get(a[1], 3));
	} else if (IS("src_duration")) {
		script_need(sl, 2);
		printf("duration_ms %lld\n", fsw_duration(get(a[1], 3)));
	} else if (IS("src_time")) {
		script_need(sl, 2);
		printf("time_ms %lld\n", fsw_time(get(a[1], 3)));
	} else if (IS("src_set_time")) {
		script_need(sl, 3);
		fsw_set_time(get(a[1], 3), atoll(a[2]));
	} else if (IS("src_state")) {
		script_need(sl, 2);
		printf("state %d\n", fsw_state(get(a[1], 3)));
	} else if (IS("src_missing")) {
		script_need(sl, 2);
		fsw_missing(get(a[1], 3));
	} else if (IS("missing_cb")) {
		script_need(sl, 4);
		fsw_missing_cb(get(a[1], 3), atoi(a[2]), a[3]);
	} else if (IS("src_dump")) {
		script_need(sl, 2);
		fsw_dump(get(a[1], 3));
	} else if (IS("hotkey")) {
		script_need(sl, 4);
		fsw_hotkey(get(a[1], 3), a[2], atoi(a[3]));
	} else if (IS("proc")) {
		script_need(sl, 3);
		fsw_proc(get(a[1], 3), a[2]);
	} else if (IS("media_cb")) {
		script_need(sl, 2);
		fsw_media_cb(a[1]);
	} else if (IS("thread_run")) {
		fsw_thread_run();
	} else if (IS("src_props")) {
		script_need(sl, 3);
		put(a[1], fsw_props(get_or_null(a[2], 3)), 2);
	} else if (IS("props_modified")) {
		script_need(sl, 4);
		obs_properties_t *p = get(a[1], 2);
		obs_property_t *pr = obs_properties_get(p, a[2]);
		if (!pr)
			die("no such property", a[2]);
		bool r = obs_property_modified(pr, DAT(a[3]));
		printf("modified %s -> %d\n", a[2], r);
	}
	/* ---- fake libavcodec ---- */
	else if (IS("av_codec")) {
		script_need(sl, 3);
		fs_codec(a[1], atoi(a[2]));
	} else if (IS("av_lossless")) {
		script_need(sl, 3);
		fs_codec_lossless(a[1], atoi(a[2]));
	} else if (IS("av_formats")) {
		script_need(sl, 2);
		fs_formats(a[1]);
	} else if (IS("av_rates")) {
		script_need(sl, 2);
		fs_rates(a[1]);
	} else if (IS("av_clear")) {
		g_host.send_n = 0;
		g_host.recv_n = 0;
	} else if (IS("av_send")) {
		for (int i = 1; i < n && g_host.send_n < FS_MAXQ; i++)
			g_host.send_q[g_host.send_n++] = atoi(a[i]);
	} else if (IS("av_recv")) {
		script_need(sl, 6);
		if (g_host.recv_n < FS_MAXQ) {
			g_host.recv_q[g_host.recv_n].rc = atoi(a[1]);
			g_host.recv_q[g_host.recv_n].size = atoi(a[2]);
			g_host.recv_q[g_host.recv_n].pts = atoll(a[3]);
			g_host.recv_q[g_host.recv_n].dts = atoll(a[4]);
			g_host.recv_q[g_host.recv_n].flags = atoi(a[5]);
			g_host.recv_n++;
		}
	}
	/* ---- audio encoders ---- */
	else if (IS("audio_table")) {
		fsa_table();
	} else if (IS("audio_defaults")) {
		script_need(sl, 3);
		fsa_defaults(a[1], DAT(a[2]));
	} else if (IS("audio_props")) {
		script_need(sl, 3);
		put(a[2], fsa_props(a[1]), 2);
	} else if (IS("audio_create")) {
		script_need(sl, 4);
		void *e = fsa_create(a[2], DAT(a[3]));
		if (e)
			put(a[1], e, 4);
	} else if (IS("audio_dump")) {
		script_need(sl, 2);
		fsa_dump(get(a[1], 4));
	} else if (IS("audio_encode")) {
		script_need(sl, 3);
		fsa_encode(get(a[1], 4), atoi(a[2]));
	} else if (IS("audio_info")) {
		script_need(sl, 3);
		fsa_info(a[2], get(a[1], 4));
	} else if (IS("audio_destroy")) {
		script_need(sl, 2);
		fsa_destroy(get(a[1], 4));
		drop(a[1], 4);
	}
	/* ---- video encoder wrapper ---- */
	else if (IS("vid_init")) {
		script_need(sl, 7);
		struct ffmpeg_video_encoder *e = calloc(1, sizeof(*e));
		bool r = ffmpeg_video_encoder_init(e, e, (obs_encoder_t *)&g_host, a[2], !strcmp(a[3], "@null") ? NULL : a[3],
						   strdup(a[4]), atoi(a[5]) ? on_init_error : NULL,
						   atoi(a[6]) ? on_first_packet : NULL);
		printf("vid_init -> %d\n", r);
		put(a[1], e, 5);
	} else if (IS("vid_init_stack")) {
		script_need(sl, 5);
		init_in_child(!strcmp(a[1], "ones") ? 0xFF : 0x00, a[2], !strcmp(a[3], "@null") ? NULL : a[3], a[4]);
	} else if (IS("vid_update")) {
		script_need(sl, 5);
		vid_update_cmd(get(a[1], 5), atoi(a[2]), atoi(a[3]), !strcmp(a[4], "@null") ? NULL : a[4]);
	} else if (IS("vid_init_codec")) {
		script_need(sl, 2);
		struct ffmpeg_video_encoder *e = get(a[1], 5);
		bool r = ffmpeg_video_encoder_init_codec(e);
		printf("vid_init_codec -> %d initialized=%d vframe=%d\n", r, e->initialized, e->vframe != NULL);
		if (e->vframe)
			printf("  vframe fmt=%d %dx%d range=%d pri=%d trc=%d cs=%d chroma=%d\n", e->vframe->format,
			       e->vframe->width, e->vframe->height, (int)e->vframe->color_range,
			       (int)e->vframe->color_primaries, (int)e->vframe->color_trc, (int)e->vframe->colorspace,
			       (int)e->vframe->chroma_location);
	} else if (IS("vid_encode")) {
		script_need(sl, 7);
		int ls[4] = {atoi(a[3]), atoi(a[4]), atoi(a[5]), atoi(a[6])};
		vid_encode_cmd(get(a[1], 5), atoll(a[2]), ls);
	} else if (IS("vid_dump")) {
		script_need(sl, 2);
		struct ffmpeg_video_encoder *e = get(a[1], 5);
		printf("vid initialized=%d first_packet=%d start_ts=%lld height=%d buffer=%zu enc_name=", e->initialized,
		       e->first_packet, (long long)e->start_ts, e->height, e->buffer.num);
		pq(e->enc_name);
		putchar('\n');
	} else if (IS("vid_free")) {
		script_need(sl, 2);
		ffmpeg_video_encoder_free(get(a[1], 5));
		drop(a[1], 5);
	} else {
		die("unknown command", a[0]);
	}
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	while (script_next(f, &sl, &lineno))
		run(&sl);
	fclose(f);
	return 0;
}
