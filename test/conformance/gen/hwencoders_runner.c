/*
 * Conformance harness (area hwencoders, spec S-050): script-driven runner for the hardware
 * encoder glue. Language documented in test/conformance/README.md (section "hwencoders runner").
 * Test tooling, not part of OBS.
 */
#include <string.h>
#include <strings.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <stdlib.h>
#include "harness.h"
#include "hwencoders_api.h"

extern int g_hw_dump;
static obs_data_t *S;
static void *ENC;
static int ENC_CODEC;
static void *FENC;
static void *VENC;
static void *QENC;
static void die(const char *m, const char *a);
static int qcodec(const char *s) { return !strcmp(s, "h264") ? 0 : !strcmp(s, "av1") ? 1 : !strcmp(s, "hevc") ? 2 : (die("codec", s), 0); }
static size_t hexbuf(const char *h, uint8_t *b)
{
	size_t len = strcmp(h, "-") ? strlen(h) / 2 : 0;
	for (size_t i = 0; i < len; i++) b[i] = (uint8_t)(script_hexval(h[2 * i]) * 16 + script_hexval(h[2 * i + 1]));
	return len;
}

static void die(const char *m, const char *a)
{
	fprintf(stderr, "script: %s %s\n", m, a ? a : "");
	exit(2);
}
static int codec_of(const char *s)
{
	return !strcmp(s, "h264") ? 0 : !strcmp(s, "hevc") ? 1 : !strcmp(s, "av1") ? 2 : (die("codec", s), 0);
}
static bool tobool(const char *s)
{
	return !strcmp(s, "1") || !strcmp(s, "true");
}
static enum video_format fmt_of(const char *s)
{
	static const struct {
		const char *n;
		enum video_format f;
	} t[] = {{"none", VIDEO_FORMAT_NONE}, {"nv12", VIDEO_FORMAT_NV12}, {"p010", VIDEO_FORMAT_P010},
		 {"i420", VIDEO_FORMAT_I420}, {"i444", VIDEO_FORMAT_I444}, {"i010", VIDEO_FORMAT_I010},
		 {"bgra", VIDEO_FORMAT_BGRA}, {"rgba", VIDEO_FORMAT_RGBA}, {"bgrx", VIDEO_FORMAT_BGRX},
		 {"p216", VIDEO_FORMAT_P216}, {"p416", VIDEO_FORMAT_P416}, {"yuy2", VIDEO_FORMAT_YUY2}};
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcmp(t[i].n, s))
			return t[i].f;
	die("format", s);
	return 0;
}
static const char *fmt_name(int f)
{
	return f == VIDEO_FORMAT_NV12   ? "nv12"
	       : f == VIDEO_FORMAT_P010 ? "p010"
	       : f == VIDEO_FORMAT_I444 ? "i444"
	       : f == VIDEO_FORMAT_I420 ? "i420"
	       : f == VIDEO_FORMAT_I010 ? "i010"
					: "other";
}

static void list_registered(void)
{
	for (int i = 0; i < g_hw_nregistered; i++) {
		struct obs_encoder_info *e = &g_hw_registered[i];
		uint32_t c = e->caps;
		printf("encoder id=%s codec=%s type=%s caps=[%s%s%s%s%s%s%s] cb=[%s%s%s%s%s%s%s%s%s%s]\n", e->id,
		       e->codec, e->type == OBS_ENCODER_VIDEO ? "video" : "audio",
		       c & OBS_ENCODER_CAP_DEPRECATED ? "deprecated " : "", c & OBS_ENCODER_CAP_PASS_TEXTURE ? "pass_texture " : "",
		       c & OBS_ENCODER_CAP_DYN_BITRATE ? "dyn_bitrate " : "",
		       c & OBS_ENCODER_CAP_INTERNAL ? "internal " : "", c & OBS_ENCODER_CAP_ROI ? "roi " : "",
		       c & OBS_ENCODER_CAP_SCALING ? "scaling " : "",
		       c & OBS_ENCODER_CAP_MULTITRACK_DYN_BITRATE ? "mt_dyn_bitrate" : "",
		       e->encode ? "encode " : "", e->encode_texture2 ? "encode_texture2 " : "",
		       e->get_extra_data ? "extra_data " : "", e->get_sei_data ? "sei_data " : "",
		       e->get_video_info ? "video_info " : "", e->update ? "update " : "",
		       e->get_defaults ? "defaults " : "", e->get_properties ? "properties " : "",
		       e->destroy ? "destroy " : "", e->create ? "create" : "");
	}
}

static int errcode(const char *s)
{
	if (isdigit((unsigned char)s[0]))
		return atoi(s);
	for (int i = 0; i < 27; i++)
		if (!strcmp(hw_nh_errname(i), s))
			return i;
	die("errcode", s);
	return 0;
}

static void run(struct script_line *sl)
{
	char **a = sl->argv;
	int n = sl->argc;
	const char *c = a[0];

	printf("> ");
	for (int i = 0; i < n; i++) {
		printf("%s", i ? " " : "");
		if (strpbrk(a[i], " \t\n\"\\") || !a[i][0])
			script_print_quoted(a[i]);
		else
			printf("%s", a[i]);
	}
	printf("\n");

	if (!strcmp(c, "host")) {
		const char *k = a[1], *v = n > 2 ? a[2] : "";
		if (!strcmp(k, "format")) g_hw.format = fmt_of(v);
		else if (!strcmp(k, "pref_format")) g_hw.pref_format = fmt_of(v);
		else if (!strcmp(k, "colorspace")) {
			g_hw.colorspace = !strcmp(v, "601") ? VIDEO_CS_601 : !strcmp(v, "709") ? VIDEO_CS_709
				: !strcmp(v, "srgb") ? VIDEO_CS_SRGB : !strcmp(v, "pq") ? VIDEO_CS_2100_PQ
				: !strcmp(v, "hlg") ? VIDEO_CS_2100_HLG : VIDEO_CS_DEFAULT;
		} else if (!strcmp(k, "range")) g_hw.range = !strcmp(v, "full") ? VIDEO_RANGE_FULL : VIDEO_RANGE_PARTIAL;
		else if (!strcmp(k, "fps")) { g_hw.fps_num = atoi(a[2]); g_hw.fps_den = atoi(a[3]); }
		else if (!strcmp(k, "size")) { g_hw.width = atoi(a[2]); g_hw.height = atoi(a[3]); }
		else if (!strcmp(k, "tex_nv12")) g_hw.tex_nv12 = tobool(v);
		else if (!strcmp(k, "tex_p010")) g_hw.tex_p010 = tobool(v);
		else if (!strcmp(k, "scaling")) g_hw.scaling = tobool(v);
		else if (!strcmp(k, "gpu_scaling")) g_hw.gpu_scaling = tobool(v);
		else if (!strcmp(k, "hdr_peak")) g_hw.hdr_peak = atoi(v);
		else if (!strcmp(k, "timebase")) g_hw.timebase_num = atoll(v);
		else if (!strcmp(k, "has_roi")) g_hw.has_roi = tobool(v);
		else if (!strcmp(k, "roi_increment")) g_hw.roi_increment = atoi(v);
		else die("host key", k);
	} else if (!strcmp(c, "roi")) {
		struct obs_encoder_roi *r = &g_hw.roi[g_hw.nroi++];
		r->top = atoi(a[2]); r->left = atoi(a[1]); r->right = atoi(a[3]); r->bottom = atoi(a[4]);
		r->priority = (float)atof(a[5]);
	} else if (!strcmp(c, "roi_clear")) {
		g_hw.nroi = 0;
	} else if (!strcmp(c, "reset")) {
		obs_data_release(S);
		S = obs_data_create();
	} else if (!strcmp(c, "set")) {
		if (!strcmp(a[1], "int")) obs_data_set_int(S, a[2], atoll(a[3]));
		else if (!strcmp(a[1], "str")) obs_data_set_string(S, a[2], a[3]);
		else if (!strcmp(a[1], "bool")) obs_data_set_bool(S, a[2], tobool(a[3]));
		else die("set type", a[1]);
	} else if (!strcmp(c, "get")) {
		if (!strcmp(a[1], "int")) printf("%s = %lld\n", a[2], (long long)obs_data_get_int(S, a[2]));
		else if (!strcmp(a[1], "str")) printf("%s = \"%s\"\n", a[2], obs_data_get_string(S, a[2]));
		else printf("%s = %s\n", a[2], obs_data_get_bool(S, a[2]) ? "true" : "false");
	} else if (!strcmp(c, "has_user")) {
		printf("has_user_value(%s) = %d\n", a[1], obs_data_has_user_value(S, a[1]));
	} else if (!strcmp(c, "defaults")) {
		hw_nv_defaults(codec_of(a[1]), S);
	} else if (!strcmp(c, "dump")) {
		g_hw_dump = atoi(a[1]);
	} else if (!strcmp(c, "nvcap")) {
		hw_nv_cap(a[1], atoi(a[2]));
	} else if (!strcmp(c, "nvstatus")) {
		hw_nv_status(a[1], errcode(a[2]));
	} else if (!strcmp(c, "nvpreset")) {
		hw_nv_preset_field(a[1], atoi(a[2]));
	} else if (!strcmp(c, "nverr")) {
		hw_nv_set_errstr(n > 1 ? a[1] : NULL);
	} else if (!strcmp(c, "lockpic")) {
		hw_nv_lockpic(atoi(a[1]), atoll(a[2]), atoi(a[3]));
	} else if (!strcmp(c, "header")) {
		uint8_t b[64];
		size_t len = strlen(a[1]) / 2;
		for (size_t i = 0; i < len; i++) b[i] = (uint8_t)(script_hexval(a[1][2 * i]) * 16 + script_hexval(a[1][2 * i + 1]));
		hw_nv_set_header(b, len);
	} else if (!strcmp(c, "create")) {
		ENC_CODEC = codec_of(a[1]);
		ENC = hw_nv_create(ENC_CODEC, !strcmp(a[2], "tex"), S);
		printf("create -> %s\n", ENC ? (ENC == (void *)1 ? "rerouted" : "ok") : "NULL");
		if (ENC == (void *)1) ENC = NULL;
	} else if (!strcmp(c, "update")) {
		printf("update -> %d\n", hw_nv_update(ENC, S));
	} else if (!strcmp(c, "state")) {
		hw_nv_state(ENC);
	} else if (!strcmp(c, "encode")) {
		struct encoder_packet pkt;
		memset(&pkt, 0, sizeof(pkt));
		pkt.timebase_num = (int32_t)g_hw.timebase_num;
		bool got = false;
		bool ok = hw_nv_encode(ENC, atoll(a[1]), &pkt, &got);
		printf("encode -> %d received=%d", ok, got);
		if (got) {
			printf(" size=%zu head=", pkt.size);
			script_print_hex(pkt.data, pkt.size < 4 ? pkt.size : 4);
			printf(" pts=%lld dts=%lld keyframe=%d priority=%d", (long long)pkt.pts, (long long)pkt.dts,
			       pkt.keyframe, pkt.priority);
		}
		printf("\n");
	} else if (!strcmp(c, "encodes")) {
		/* encodes FROM TO: submit pts FROM..TO; print only calls that return a packet or fail */
		int cnt = 0;
		for (long long t = atoll(a[1]); t <= atoll(a[2]); t++) {
			struct encoder_packet pkt;
			memset(&pkt, 0, sizeof(pkt));
			pkt.timebase_num = (int32_t)g_hw.timebase_num;
			bool got = false;
			bool ok = hw_nv_encode(ENC, t, &pkt, &got);
			cnt++;
			if (got || !ok)
				printf("encode pts=%lld -> %d received=%d size=%zu out_pts=%lld dts=%lld keyframe=%d priority=%d\n", t, ok, got,
				       got ? pkt.size : 0, (long long)pkt.pts, (long long)pkt.dts, pkt.keyframe, pkt.priority);
		}
		printf("submitted %d frames\n", cnt);
	} else if (!strcmp(c, "extra")) {
		const uint8_t *p = NULL;
		size_t len = 0;
		bool r = hw_nv_extra(ENC, &p, &len);
		printf("extra_data -> %d ", r);
		if (r) script_print_hex(p, len);
		printf("\nsei_data -> %d\n", hw_nv_sei(ENC));
	} else if (!strcmp(c, "destroy")) {
		if (ENC)
			hw_nv_destroy(ENC);
		printf(ENC ? "destroyed\n" : "destroy: no encoder\n");
		ENC = NULL;
	} else if (!strcmp(c, "register")) {
		g_hw_nregistered = 0;
		hw_nv_register();
		list_registered();
	} else if (!strcmp(c, "compat_register")) {
		g_hw_nregistered = 0;
		hw_nc_register();
		list_registered();
	} else if (!strcmp(c, "invoke")) {
		struct obs_encoder_info *e = NULL;
		for (int i = 0; i < g_hw_nregistered; i++)
			if (!strcmp(g_hw_registered[i].id, a[1])) e = &g_hw_registered[i];
		if (!e) die("encoder id", a[1]);
		if (!strcmp(a[2], "create")) {
			void *r = e->create(S, (obs_encoder_t *)&g_hw);
			printf("invoke create -> %s\n", r ? "non-null" : "NULL");
		} else if (!strcmp(a[2], "defaults")) {
			obs_data_t *d = obs_data_create();
			e->get_defaults(d);
			printf("defaults of %s:", e->id);
			const char *keys[] = {"bitrate", "max_bitrate", "keyint_sec", "cqp", "gpu", "bf"};
			for (int i = 0; i < 6; i++) printf(" %s=%lld", keys[i], (long long)obs_data_get_int(d, keys[i]));
			const char *skeys[] = {"rate_control", "preset2", "multipass", "tune", "profile"};
			for (int i = 0; i < 5; i++) printf(" %s=%s", skeys[i], obs_data_get_string(d, skeys[i]));
			printf(" psycho_aq=%d repeat_headers=%d\n", obs_data_get_bool(d, "psycho_aq"), obs_data_get_bool(d, "repeat_headers"));
			obs_data_release(d);
		} else if (!strcmp(a[2], "properties")) {
			obs_properties_t *p = e->get_properties(NULL);
			printf("properties of %s:", e->id);
			for (obs_property_t *q = obs_properties_first(p); q; obs_property_next(&q))
				printf(" %s%s", obs_property_name(q), obs_property_visible(q) ? "" : "(hidden)");
			printf("\n");
			obs_properties_destroy(p);
		}
	} else if (!strcmp(c, "ini")) {
		g_hw_ini = bstrdup(a[1]);
	} else if (!strcmp(c, "dll")) {
		g_hw_dll = atoi(a[1]);
	} else if (!strcmp(c, "sym")) {
		if (!strcmp(a[1], "maxver")) g_hw_sym_maxver = atoi(a[2]);
		else g_hw_sym_create = atoi(a[2]);
	} else if (!strcmp(c, "maxver")) {
		g_hw_maxver = (uint32_t)strtoul(a[1], NULL, 0);
	} else if (!strcmp(c, "pipefail")) {
		g_hw_pipe_fail = atoi(a[1]);
	} else if (!strcmp(c, "supported")) {
		hw_nh_supported();
	} else if (!strcmp(c, "ff_create")) {
		FENC = hw_ff_create(!strcmp(a[1], "hevc"), S);
		printf("ff_create -> %s\n", FENC ? "ok" : "NULL");
	} else if (!strcmp(c, "ff_reconfigure")) {
		printf("ff_reconfigure -> %d\n", hw_ff_reconfigure(FENC, S));
	} else if (!strcmp(c, "ff_destroy")) {
		if (FENC) hw_ff_destroy(FENC);
		FENC = NULL;
		printf("ff_destroyed\n");
	} else if (!strcmp(c, "ff_initfail")) {
		g_ff_init_codec_ret = !atoi(a[1]);
		g_ff_init_error = atoi(a[2]);
		g_ff_init_fail_once = n > 3 ? atoi(a[3]) : 0;
		if (g_ff_init_fail_once) g_ff_init_codec_ret = 1;
	} else if (!strcmp(c, "ff_packet")) {
		uint8_t b[256];
		size_t len = strlen(a[1]) / 2;
		for (size_t i = 0; i < len; i++) b[i] = (uint8_t)(script_hexval(a[1][2 * i]) * 16 + script_hexval(a[1][2 * i + 1]));
		hw_ff_set_packet(b, len, atoll(a[2]));
	} else if (!strcmp(c, "ff_ctx")) {
		hw_ff_set_ctx_bframes_timebase(FENC, atoi(a[1]), atoi(a[2]));
	} else if (!strcmp(c, "ff_encode")) {
		struct encoder_packet pkt;
		memset(&pkt, 0, sizeof(pkt));
		bool got = false;
		bool ok = hw_ff_encode(FENC, &pkt, &got);
		printf("ff_encode -> %d received=%d dts=%lld size=%zu data=", ok, got, (long long)pkt.dts, pkt.size);
		script_print_hex(pkt.data, pkt.size);
		printf("\n");
	} else if (!strcmp(c, "ff_state")) {
		hw_ff_state(FENC);
	} else if (!strcmp(c, "ff_video_info")) {
		int out;
		hw_ff_video_info(fmt_of(a[1]), fmt_of(a[2]), &out);
		printf("video_info(pref=%s, in=%s) -> %s\n", a[1], a[2], fmt_name(out));
	} else if (!strcmp(c, "ff_register")) {
		g_hw_nregistered = 0;
		hw_ff_register();
		list_registered();
	} else if (!strcmp(c, "ff_defaults")) {
		hw_ff_defaults(codec_of(a[1]), S);
	} else if (!strcmp(c, "q_adapter")) {
		hw_q_adapter(atoi(a[1]), atoi(a[2]), atoi(a[3]), atoi(a[4]), atoi(a[5]));
	} else if (!strcmp(c, "q_set")) {
		hw_q_set(a[1], atoi(a[2]));
	} else if (!strcmp(c, "q_load")) {
		hw_q_load();
		list_registered();
	} else if (!strcmp(c, "q_headers")) {
		uint8_t v[64], s2[64], p2[64];
		size_t nv = hexbuf(a[1], v), ns = hexbuf(a[2], s2), np = hexbuf(a[3], p2);
		hw_q_headers(v, nv, s2, ns, p2, np);
	} else if (!strcmp(c, "q_bs")) {
		uint8_t b[256];
		size_t len = hexbuf(a[1], b);
		hw_q_bitstream(b, len, (unsigned)strtoul(a[2], NULL, 0), atoll(a[3]), atoll(a[4]));
	} else if (!strcmp(c, "q_create")) {
		QENC = hw_q_create(qcodec(a[1]), !strcmp(a[2], "tex"), S);
		printf("q_create -> %s\n", QENC ? (QENC == (void *)1 ? "rerouted" : "ok") : "NULL");
		if (QENC == (void *)1) QENC = NULL;
	} else if (!strcmp(c, "q_destroy")) {
		if (QENC) hw_q_destroy(QENC);
		QENC = NULL;
		printf("q_destroyed\n");
	} else if (!strcmp(c, "q_encode")) {
		struct encoder_packet pkt;
		memset(&pkt, 0, sizeof(pkt));
		bool got = false;
		bool ok = hw_q_encode(QENC, atoll(a[1]), n > 2 ? (!strcmp(a[2], "tex") ? 1 : !strcmp(a[2], "badtex") ? 2 : 0) : 0, &pkt, &got);
		printf("q_encode -> %d received=%d", ok, got);
		if (got) {
			printf(" size=%zu data=", pkt.size);
			script_print_hex(pkt.data, pkt.size);
			printf(" pts=%lld dts=%lld keyframe=%d priority=%d type=%d", (long long)pkt.pts, (long long)pkt.dts, pkt.keyframe, pkt.priority, (int)pkt.type);
		}
		printf("\n");
	} else if (!strcmp(c, "q_extra")) {
		hw_q_extra(QENC);
	} else if (!strcmp(c, "q_ts")) {
		hw_q_ts(atoll(a[2]), !strcmp(a[1], "obs"));
	} else if (!strcmp(c, "va_caps")) {
		hw_va_caps(a[1], atoi(a[2]));
	} else if (!strcmp(c, "va_fail")) {
		hw_va_fail(atoi(a[1]));
	} else if (!strcmp(c, "va_create")) {
		VENC = hw_va_create(codec_of(a[1]), S);
		printf("va_create -> %s\n", VENC ? "ok" : "NULL");
	} else if (!strcmp(c, "va_update")) {
		printf("va_update -> %d\n", hw_va_update(VENC, S));
	} else if (!strcmp(c, "va_destroy")) {
		if (VENC) hw_va_destroy(VENC);
		VENC = NULL;
		printf("va_destroyed\n");
	} else if (!strcmp(c, "va_defaults")) {
		hw_va_defaults(codec_of(a[1]), S);
	} else if (!strcmp(c, "va_encode")) {
		/* va_encode HEX|- PTS DTS SEND_RET RECV_RET   ("-" = empty packet) */
		uint8_t b[256];
		size_t len = strcmp(a[1], "-") ? strlen(a[1]) / 2 : 0;
		for (size_t i = 0; i < len; i++) b[i] = (uint8_t)(script_hexval(a[1][2 * i]) * 16 + script_hexval(a[1][2 * i + 1]));
		struct encoder_packet pkt;
		memset(&pkt, 0, sizeof(pkt));
		bool got = false;
		bool ok = hw_va_encode(VENC, b, len, atoll(a[2]), atoll(a[3]), atoi(a[4]), atoi(a[5]), &pkt, &got);
		printf("va_encode -> %d received=%d", ok, got);
		if (got) {
			printf(" size=%zu data=", pkt.size);
			script_print_hex(pkt.data, pkt.size);
			printf(" pts=%lld dts=%lld keyframe=%d", (long long)pkt.pts, (long long)pkt.dts, pkt.keyframe);
		}
		printf("\n");
	} else if (!strcmp(c, "va_video_info")) {
		int out;
		hw_va_video_info(VENC, fmt_of(a[1]), fmt_of(a[2]), &out);
		printf("video_info(pref=%s, in=%s) -> %s\n", a[1], a[2], fmt_name(out));
	} else if (!strcmp(c, "va_rc")) {
		int qp, br, mr;
		const char *nm = hw_va_rc(a[1], &qp, &br, &mr);
		printf("rc_mode(%s) -> %s qp=%d bitrate=%d maxrate=%d\n", a[1], nm ? nm : "(null name)", qp, br, mr);
	} else if (!strcmp(c, "va_rc_modified")) {
		obs_properties_t *p = hw_va_mkprops();
		hw_va_rc_modified(p, S);
		const char *ks[] = {"qp", "bitrate", "maxrate"};
		for (int i = 0; i < 3; i++) printf("%s visible=%d\n", ks[i], obs_property_visible(obs_properties_get(p, ks[i])));
		obs_properties_destroy(p);
	} else if (!strcmp(c, "va_device_modified")) {
		obs_properties_t *p = hw_va_mkprops();
		obs_property_list_add_string(obs_properties_get(p, "rate_control"), "stale", "stale");
		hw_va_device_modified(p, S);
		obs_property_t *l = obs_properties_get(p, "rate_control");
		printf("rate_control items:");
		for (size_t i = 0; i < obs_property_list_item_count(l); i++) printf(" %s", obs_property_list_item_string(l, i));
		printf("\nbf visible=%d\n", obs_property_visible(obs_properties_get(p, "bf")));
		obs_properties_destroy(p);
	} else if (!strcmp(c, "va_register")) {
		g_hw_nregistered = 0;
		hw_va_register();
		list_registered();
	} else if (!strcmp(c, "fcall")) {
		/* run one command in a forked child; report a crash of the reference as data */
		struct script_line sub;
		memset(&sub, 0, sizeof(sub));
		char line[512];
		line[0] = 0;
		for (int i = 1; i < n; i++) { strcat(line, a[i]); strcat(line, " "); }
		fflush(stdout);
		pid_t pid = fork();
		if (pid == 0) {
			script_tokenize(&sub, line);
			run(&sub);
			fflush(stdout);
			_exit(0);
		}
		int st = 0;
		waitpid(pid, &st, 0);
		if (WIFSIGNALED(st)) printf("crash: signal %d\n", WTERMSIG(st));
		else printf("exit %d\n", WEXITSTATUS(st));
	} else if (!strcmp(c, "fsupported")) {
		/* nvenc_supported() in a forked child: reports crashes of the reference */
		fflush(stdout);
		pid_t pid = fork();
		if (pid == 0) {
			hw_nh_supported();
			fflush(stdout);
			_exit(0);
		}
		int st = 0;
		waitpid(pid, &st, 0);
		if (WIFSIGNALED(st))
			printf("crash: signal %d\n", WTERMSIG(st));
		else
			printf("exit %d\n", WEXITSTATUS(st));
	} else if (!strcmp(c, "load")) {
		g_hw_nregistered = 0;
		hw_nh_load();
		list_registered();
	} else if (!strcmp(c, "initnv")) {
		printf("init_nvenc -> %d\n", hw_nh_init());
	} else if (!strcmp(c, "caps")) {
		hw_nh_caps(codec_of(a[1]));
	} else if (!strcmp(c, "vinfo")) {
		hw_nh_version_info();
	} else if (!strcmp(c, "nvfail")) {
		hw_nh_failed(errcode(a[1]), n > 2 ? a[2] : NULL);
	} else if (!strcmp(c, "errname")) {
		printf("%s\n", hw_nh_errname(atoi(a[1])));
	} else if (!strcmp(c, "brokensplit")) {
		printf("has_broken_split_encoding -> %d\n", hw_nh_broken_split());
	} else if (!strcmp(c, "props")) {
		/* real property list under the probed caps (see S-025 for the rules) */
		extern obs_properties_t *h264_nvenc_properties(void *), *hevc_nvenc_properties(void *), *av1_nvenc_properties(void *);
		int cd = codec_of(a[1]);
		obs_properties_t *p = cd == 0 ? h264_nvenc_properties(NULL) : cd == 1 ? hevc_nvenc_properties(NULL) : av1_nvenc_properties(NULL);
		for (obs_property_t *q = obs_properties_first(p); q; obs_property_next(&q)) printf(" %s", obs_property_name(q));
		printf("\n");
		obs_properties_destroy(p);
	} else {
		die("unknown command", c);
	}
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	S = obs_data_create();
	while (script_next(f, &sl, &lineno))
		run(&sl);
	fclose(f);
	return 0;
}
