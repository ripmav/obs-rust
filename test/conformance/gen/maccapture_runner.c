/*
 * Conformance runner for spec S-052 (macOS plugins: mac-capture, mac-avcapture, mac-videotoolbox, mac-virtualcam).
 *
 * Everything here is a MODEL: the reference is Objective-C / CoreAudio / ScreenCaptureKit / VideoToolbox code that
 * cannot be compiled on Linux. Each command re-implements, from reading the source, one pure decision of the
 * reference. Expected output is therefore source-derived, NOT produced by the reference binary.
 * The only real C linked in is libobs/media-io/audio-io.h (get_audio_channels, speaker layouts) and
 * the video-io.h enums, which the macOS plugins use unchanged.
 * Script language: test/conformance/README.md, section "maccapture runner (spec S-052)".
 */
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "harness.h"
#include <media-io/audio-io.h>
#include <media-io/video-io.h>

static const char *arg(struct script_line *sl, int i)
{
	return i >= sl->argc ? "-" : sl->argv[i];
}
static long long num(struct script_line *sl, int i)
{
	return strtoll(arg(sl, i), NULL, 0);
}
static double dnum(struct script_line *sl, int i)
{
	return strtod(arg(sl, i), NULL);
}
static const char *nz(const char *s)
{
	return !strcmp(s, "-") ? "" : s;
}
static const char *tf(bool b)
{
	return b ? "true" : "false";
}

/* ----------------------------------------------------------------- Apple constants (values from the SDK) */
#define kAudioFormatFlagIsFloat 0x1u
#define kAudioFormatFlagIsSignedInteger 0x4u
#define kAudioFormatFlagIsNonInterleaved 0x20u

/* macOS version is passed as major*100+minor (12.5 -> 1205, 14.2 -> 1402) */
#define MAC(maj, min) ((maj) * 100 + (min))

/* ----------------------------------------------------------------- screen capture (SCK) */

static const char *const sck_prop[] = {"display_uuid", "window", "application", "show_empty_names",
				       "show_hidden_windows", "hide_obs"};

static void sck_visibility(int type)
{
	/* both properties() (initial) and content_settings_changed() (type 0/2 additionally touch capture_type_info) */
	bool v[6] = {0};
	switch (type) {
	case 0:
		v[0] = 1; v[5] = 1;
		break;
	case 1:
		v[1] = 1; v[3] = 1; v[4] = 1;
		break;
	case 2:
		v[0] = 1; v[2] = 1; v[4] = 1;
		break;
	default:
		printf("sck_visibility type=%d unchanged-from-default (no case)\n", type);
		return;
	}
	printf("sck_visibility type=%d", type);
	for (int i = 0; i < 6; i++)
		printf(" %s=%s", sck_prop[i], tf(v[i]));
	printf("\n");
}

struct sckst {
	int type, display;
	bool hide_cursor, hide_obs;
	int window;
	char app[64];
};

static void sck_update(struct script_line *sl)
{
	/* cur: type display hide_cursor hide_obs window app ; new: type display show_cursor hide_obs window app */
	struct sckst sc = {(int)num(sl, 1), (int)num(sl, 2), num(sl, 3) != 0, num(sl, 4) != 0, (int)num(sl, 5), ""};
	snprintf(sc.app, sizeof sc.app, "%s", nz(arg(sl, 6)));
	int ntype = (int)num(sl, 7), ndisplay = (int)num(sl, 8);
	bool nshow = num(sl, 9) != 0, nhide_obs = num(sl, 10) != 0;
	unsigned nwin = (unsigned)num(sl, 11);
	const char *napp = nz(arg(sl, 12));

	unsigned old_window = (unsigned)sc.window;
	if (nwin > 0 && nwin != old_window) /* persists even when the stream is kept */
		sc.window = (int)nwin;
	bool keep = false;
	if (ntype == sc.type) {
		switch (sc.type) {
		case 0:
			keep = sc.display == ndisplay && sc.hide_cursor != nshow && sc.hide_obs == nhide_obs;
			break;
		case 1:
			keep = old_window == (unsigned)sc.window && sc.hide_cursor != nshow;
			break;
		case 2:
			keep = sc.display == ndisplay && !strcmp(napp, sc.app) && sc.hide_cursor != nshow;
			break;
		}
	}
	printf("sck_update decision=%s window=%d\n", keep ? "keep" : "restart", sc.window);
}

static void sck_config(struct script_line *sl)
{
	/* type macos fps_num fps_den show_cursor have_video_info target_w target_h */
	int type = (int)num(sl, 1), mac = (int)num(sl, 2);
	double fn = dnum(sl, 3), fd = dnum(sl, 4);
	bool show = num(sl, 5) != 0, have_vi = num(sl, 6) != 0;
	printf("sck_config type=%d macos=%d\n", type, mac);
	if (mac < MAC(13, 0) && type != 1) {
		printf("  stream=none (pre-13: only window capture creates a stream, returns success)\n");
		return;
	}
	printf("  size=%lldx%lld (display: pixel size of current display mode; window: frame size in points)\n",
	       num(sl, 7), num(sl, 8));
	printf("  queue_depth=8 shows_cursor=%s color_space=DisplayP3 background=clear\n", tf(show));
	if (have_vi)
		printf("  min_frame_interval_seconds=%.9g\n", 0.9 * fd / fn);
	else
		printf("  min_frame_interval=unset (warning logged)\n");
	printf("  pixel_format=%s\n", mac >= MAC(15, 0) ? "64RGBAHalf" : "32BGRA");
	if (mac >= MAC(13, 0))
		printf("  captures_audio=true excludes_current_process_audio=true channel_count=2\n");
	if (type == 1 && mac >= MAC(14, 2))
		printf("  include_child_windows=true\n");
	if (type == 2 && mac >= MAC(14, 2))
		printf("  include_menu_bar=true\n");
}

static void sck_window_frame(struct script_line *sl)
{
	/* type content_w content_h points_to_pixels scale_factor cur_w cur_h buf_w buf_h errored */
	int type = (int)num(sl, 1);
	float cw = (float)dnum(sl, 2), ch = (float)dnum(sl, 3), p2p = (float)dnum(sl, 4), sf = (float)dnum(sl, 5);
	double curw = dnum(sl, 6), curh = dnum(sl, 7);
	int bw = (int)num(sl, 8), bh = (int)num(sl, 9);
	bool err = num(sl, 10) != 0;
	bool upd = false;
	double nw = curw, nh = curh;
	if (!err) {
		if (type == 1) {
			double ww = (double)(cw / p2p * sf), wh = (double)(ch / p2p * sf);
			if (curw != ww || curh != wh) {
				nw = ww; nh = wh; upd = true;
			}
		} else if ((int)curw != bw || (int)curh != bh) {
			nw = bw; nh = bh; upd = true;
		}
	}
	printf("sck_window_frame update_config=%s frame=%.9gx%.9g\n", tf(upd), nw, nh);
	if (upd)
		printf("  config_size=%zux%zu\n", (size_t)nw, (size_t)nh);
}

static void sck_color_space(struct script_line *sl)
{
	static const char *const names[] = {"SRGB", "SRGB_16F", "709_EXTENDED", "709_SCRGB"};
	/* priority: SRGB_16F, then 709_EXTENDED, then SRGB, else SRGB_16F */
	const char *res = "SRGB_16F";
	bool f16 = 0, ext = 0, srgb = 0;
	for (int i = 1; i < sl->argc; i++) {
		f16 |= !strcmp(sl->argv[i], names[1]);
		ext |= !strcmp(sl->argv[i], names[2]);
		srgb |= !strcmp(sl->argv[i], names[0]);
	}
	if (f16) res = "SRGB_16F";
	else if (ext) res = "709_EXTENDED";
	else if (srgb) res = "SRGB";
	printf("sck_color_space %s\n", res);
}

static void sck_dispatch(struct script_line *sl)
{
	/* kind(video|audio) audio_only(0|1) macos sc_null(0|1) */
	bool video = !strcmp(arg(sl, 1), "video");
	bool audio_only = num(sl, 2) != 0;
	int mac = (int)num(sl, 3);
	const char *r = "ignored";
	if (num(sl, 4))
		r = "ignored";
	else if (video && !audio_only)
		r = "video_update";
	else if (mac >= MAC(13, 0) && !video)
		r = "audio_update";
	printf("sck_dispatch %s\n", r);
}

static const char *const speaker_names[] = {"UNKNOWN", "MONO", "STEREO", "2POINT1", "4POINT0", "4POINT1", "5POINT1", "-", "7POINT1"};

static void sck_audio_cfg(struct script_line *sl)
{
	int sp = (int)num(sl, 1);
	uint32_t ch = get_audio_channels((enum speaker_layout)sp);
	printf("sck_audio_cfg speakers=%s channels=%u sck_channel_count=%u\n", speaker_names[sp], ch, ch > 1 ? 2 : ch);
}

static void sck_audio_buf(struct script_line *sl)
{
	/* bytes channels bytes_per_frame rate pts_seconds */
	uint32_t len = (uint32_t)num(sl, 1), ch = (uint32_t)num(sl, 2), bpf = (uint32_t)num(sl, 3);
	uint32_t rate = (uint32_t)dnum(sl, 4);
	double pts = dnum(sl, 5);
	if (ch < 1) {
		printf("sck_audio_buf rejected: error log, buffer dropped\n");
		return;
	}
	printf("sck_audio_buf speakers=%u frames=%u rate=%u ts_ns=%llu format=FLOAT_PLANAR\n", ch, len / bpf / ch, rate,
	       (unsigned long long)(pts * 1000000000.0));
	for (uint32_t i = 0; i < ch; i++)
		printf("  plane[%u]=+%u%s\n", i, (len / ch) * i, i >= MAX_AV_PLANES ? " OUT-OF-BOUNDS(data[8])" : "");
}

/* window / application / display lists */
#define MAXI 32
static struct {
	int id;
	char app[64], title[64];
} scw[MAXI];
static int nscw;
static struct {
	char name[64], bundle[64];
} sca[MAXI];
static int nsca;

static int cmp_win(const void *a, const void *b)
{
	const typeof(scw[0]) *x = a, *y = b;
	int c = strcasecmp(x->app, y->app);
	if (c)
		return c;
	return strcasecmp(x->title, y->title);
}
static int cmp_app(const void *a, const void *b)
{
	return strcasecmp(((typeof(sca[0]) *)a)->name, ((typeof(sca[0]) *)b)->name);
}

/* legacy cocoa window table */
static struct {
	int num, pid;
	char owner[48], name[48];
} cw[MAXI];
static int ncw;
static int cw_cmp(const void *a, const void *b)
{
	const typeof(cw[0]) *x = a, *y = b;
	int c = strcmp(x->owner, y->owner);
	if (c) return c;
	if (x->pid != y->pid) return x->pid < y->pid ? -1 : 1;
	c = strcmp(x->name, y->name);
	if (c) return c;
	return x->num < y->num ? -1 : x->num > y->num;
}
static const char *make_name(const char *owner, const char *name, char *buf, size_t n)
{
	if (!*owner)
		return "";
	snprintf(buf, n, "[%s] %s", owner, name);
	return buf;
}

/* ----------------------------------------------------------------- CoreAudio */
static const char *ca_format(unsigned flags, unsigned bits)
{
	bool planar = flags & kAudioFormatFlagIsNonInterleaved;
	if (flags & kAudioFormatFlagIsFloat)
		return planar ? "FLOAT_PLANAR" : "FLOAT";
	if (!(flags & kAudioFormatFlagIsSignedInteger) && bits == 8)
		return planar ? "U8BIT_PLANAR" : "U8BIT";
	if (!(flags & kAudioFormatFlagIsSignedInteger))
		return "UNKNOWN";
	if (bits == 16)
		return planar ? "16BIT_PLANAR" : "16BIT";
	if (bits == 32)
		return planar ? "32BIT_PLANAR" : "32BIT";
	return "UNKNOWN";
}
static const char *ca_layout(unsigned ch)
{
	switch (ch) {
	case 1: return "MONO";
	case 2: return "STEREO";
	case 3: return "2POINT1";
	case 4: return "4POINT0";
	case 5: return "4POINT1";
	case 6: return "5POINT1";
	case 8: return "7POINT1";
	}
	return "UNKNOWN";
}
static void ca_sanitize(const char *name, char *out)
{
	size_t len = strlen(name), o = 0;
	char buf[64];
	for (size_t i = len > 64 ? len - 64 : 0; i < len; i++) {
		char c = name[i];
		if (isalnum((unsigned char)c))
			buf[o++] = c;
		if (c == '-' || c == ' ' || c == '_' || c == ':')
			buf[o++] = '_';
	}
	memcpy(out, buf, o);
	out[o] = 0;
}
static bool contains_ci(const char *h, const char *n)
{
	size_t nl = strlen(n);
	for (; *h; h++)
		if (!strncasecmp(h, n, nl))
			return true;
	return false;
}
static bool device_is_input(const char *d)
{
	static const char *const loop[] = {"soundflower", "wavtap", "soundsiphon", "ishowu", "blackhole", "loopback",
					   "groundcontrol", "vbcable"};
	for (size_t i = 0; i < sizeof loop / sizeof *loop; i++)
		if (contains_ci(d, loop[i]))
			return false;
	return true;
}

/* ----------------------------------------------------------------- AVCapture helpers */
static int gcd_(int a, int b)
{
	return b == 0 ? a : gcd_(b, a % b);
}

static void av_aspect(int w, int h)
{
	char buf[64] = "";
	if (w > 0 && h > 0) {
		double d = (double)gcd_(w, h);
		if (d <= 50) {
			if (w > h)
				snprintf(buf, sizeof buf, "%.2f:1", (double)w / (double)h);
			else
				snprintf(buf, sizeof buf, "1:%.2f", (double)h / (double)w);
		} else {
			int x = w / (int)d, y = h / (int)d;
			if (x == 8 && y == 5) {
				x = 16; y = 10;
			}
			snprintf(buf, sizeof buf, "%d:%d", x, y);
		}
	}
	printf("av_aspect %dx%d -> \"%s\"\n", w, h, buf);
}

struct rng {
	double min, max;
};
static int rng_cmp_desc(const void *a, const void *b)
{
	const struct rng *l = a, *r = b;
	if (l->max > r->max) return -1;
	if (l->max < r->max) return 1;
	if (l->min > r->min) return -1;
	if (l->min < r->min) return 1;
	return 0;
}
static void av_fps_desc(struct script_line *sl)
{
	struct rng r[32];
	int n = 0;
	for (int i = 1; i < sl->argc && n < 32; i++, n++)
		sscanf(sl->argv[i], "%lf:%lf", &r[n].min, &r[n].max);
	qsort(r, n, sizeof *r, rng_cmp_desc); /* NSArray sort is not guaranteed stable; ties are identical anyway */
	char out[1024] = "";
	int cnt = 0;
	for (int i = n - 1; i >= 0; i--) {
		double mn = round(r[i].min * 100) / 100, mx = round(r[i].max * 100) / 100;
		char piece[64];
		bool ints = fmod(mn, 1.0) == 0 && fmod(mx, 1.0) == 0;
		if (mn == mx)
			snprintf(piece, sizeof piece, ints ? "%.0f" : "%.2f", mx);
		else if (ints)
			snprintf(piece, sizeof piece, "%.0f-%.0f", mn, mx);
		else
			snprintf(piece, sizeof piece, "%.2f-%.2f", mn, mx);
		if (cnt++)
			strcat(out, ", ");
		strcat(out, piece);
	}
	if (cnt > 0 && cnt <= 10)
		printf("av_fps_desc \"%s FPS\"\n", out);
	else if (cnt > 10)
		printf("av_fps_desc \"%.0f-%.0f FPS (%d values)\"\n", r[n - 1].min, r[0].max, n);
	else
		printf("av_fps_desc (nil)\n");
}

static void av_fallback_fps(struct script_line *sl)
{
	/* output_fps then ranges min:max:minDurValue:minDurTimescale:maxDurValue:maxDurTimescale */
	double out = dnum(sl, 1), cu = 0, ca = 1.7976931348623157e308;
	unsigned un = 0, ud = 0, an = 0, ad = 0;
	for (int i = 2; i < sl->argc; i++) {
		double mn, mx;
		long long a, b, c, d;
		sscanf(sl->argv[i], "%lf:%lf:%lld:%lld:%lld:%lld", &mn, &mx, &a, &b, &c, &d);
		if (mx > cu && mx <= out) {
			cu = mx; un = (unsigned)b; ud = (unsigned)a;
		}
		if (mn > out && mn < ca) {
			ca = mn; an = (unsigned)d; ad = (unsigned)c;
		}
	}
	if (cu > 0)
		printf("av_fallback_fps %u/%u (closest at-or-below)\n", un, ud);
	else
		printf("av_fallback_fps %u/%u (closest above)\n", an, ad);
}

static const char *av_preset(const char *p)
{
	static const char *const k[][2] = {{"Low", "Low"},   {"Medium", "Medium"},     {"High", "High"},
					   {"320x240", "320x240"}, {"352x288", "352x288"}, {"640x480", "640x480"},
					   {"960x540", "960x460"}, {"1280x720", "1280x720"}, {"1920x1080", "1920x1080"},
					   {"3840x2160", "3840x2160"}};
	for (size_t i = 0; i < sizeof k / sizeof *k; i++)
		if (!strcmp(k[i][0], p))
			return k[i][1];
	return NULL;
}

/* subtype names: value is the symbolic constant */
static const char *av_fmt(const char *st)
{
	if (!strcmp(st, "422YpCbCr8")) return "UYVY";
	if (!strcmp(st, "422YpCbCr8_yuvs")) return "YUY2";
	if (!strcmp(st, "32BGRA")) return "BGRA";
	if (!strncmp(st, "420YpCbCr8BiPlanar", 18)) return "NV12";
	if (!strncmp(st, "420YpCbCr10BiPlanar", 19)) return "P010";
	return "NONE";
}
static bool av_fullrange(const char *st)
{
	return !strcmp(st, "420YpCbCr8PlanarFullRange") || !strcmp(st, "420YpCbCr8BiPlanarFullRange") ||
	       !strcmp(st, "420YpCbCr10BiPlanarFullRange") || !strcmp(st, "422YpCbCr8FullRange");
}
static const char *av_fourcc(const char *fmt, bool full)
{
	if (!strcmp(fmt, "UYVY")) return "422YpCbCr8";
	if (!strcmp(fmt, "YUY2")) return "422YpCbCr8_yuvs";
	if (!strcmp(fmt, "NV12")) return full ? "420YpCbCr8BiPlanarFullRange" : "420YpCbCr8BiPlanarVideoRange";
	if (!strcmp(fmt, "P010")) return full ? "420YpCbCr10BiPlanarFullRange" : "420YpCbCr10BiPlanarVideoRange";
	if (!strcmp(fmt, "BGRA")) return "32BGRA";
	return "0";
}

/* ----------------------------------------------------------------- VideoToolbox */
static const char *vt_profile(const char *codec, const char *p, const char *fmt, bool *warn)
{
	*warn = false;
	if (!strcmp(codec, "h264")) {
		if (!strcmp(p, "baseline")) return "H264_Baseline_AutoLevel";
		if (!strcmp(p, "main")) return "H264_Main_AutoLevel";
		if (!strcmp(p, "high")) return "H264_High_AutoLevel";
		return "H264_Main_AutoLevel";
	}
	if (!strcmp(codec, "hevc")) {
		if (!strcmp(p, "main")) {
			if (!strcmp(fmt, "P010")) {
				*warn = true;
				return "HEVC_Main10_AutoLevel";
			}
			return "HEVC_Main_AutoLevel";
		}
		if (!strcmp(p, "main10")) return "HEVC_Main10_AutoLevel";
		if (!strcmp(p, "main42210")) return "HEVC_Main42210_AutoLevel"; /* needs macOS 12.3 */
		return "HEVC_Main_AutoLevel";
	}
	return "H264_Baseline_AutoLevel";
}

static const char *cs_name(const char *c)
{
	return c;
}

static void vt_cs(const char *cs)
{
	const char *mat = "ITU_R_709_2", *pri = "ITU_R_709_2", *tf = "ITU_R_709_2";
	if (!strcmp(cs, "601")) { mat = "ITU_R_601_4"; pri = "SMPTE_C"; }
	if (!strcmp(cs, "2100_PQ") || !strcmp(cs, "2100_HLG")) { mat = "ITU_R_2020"; pri = "ITU_R_2020"; }
	if (!strcmp(cs, "SRGB")) tf = "sRGB";
	if (!strcmp(cs, "2100_PQ")) tf = "SMPTE_ST_2084_PQ";
	if (!strcmp(cs, "2100_HLG")) tf = "ITU_R_2100_HLG";
	printf("vt_cs %s matrix=%s primaries=%s transfer=%s\n", cs_name(cs), mat, pri, tf);
}

static void hexdump(const unsigned char *b, size_t n)
{
	for (size_t i = 0; i < n; i++)
		printf("%02x", b[i]);
}
static void vt_hdr(unsigned peak)
{
	unsigned char m[24] = {0}, c[4];
	unsigned prim[8] = {13250, 34500, 7500, 3000, 34000, 16000, 15635, 16450};
	for (int i = 0; i < 8; i++) {
		m[i * 2] = prim[i] >> 8;
		m[i * 2 + 1] = prim[i] & 0xff;
	}
	uint32_t lum = (uint32_t)peak * 10000;
	m[16] = lum >> 24; m[17] = lum >> 16; m[18] = lum >> 8; m[19] = lum;
	uint16_t p16 = (uint16_t)peak;
	c[0] = p16 >> 8; c[1] = p16 & 0xff; c[2] = c[0]; c[3] = c[1];
	printf("vt_hdr peak=%u mdcv=", peak);
	hexdump(m, 24);
	printf(" clli=");
	hexdump(c, 4);
	printf("\n");
}

static void vt_rc(struct script_line *sl)
{
	/* rate_control apple_silicon macos limit_bitrate bitrate_kbps quality_pct max_kbps window */
	const char *rc = arg(sl, 1);
	bool as = num(sl, 2) != 0;
	int mac = (int)num(sl, 3);
	bool limit = num(sl, 4) != 0;
	long long br = num(sl, 5), q = num(sl, 6), mx = num(sl, 7);
	double win = dnum(sl, 8);
	const char *key;
	bool can;
	printf("vt_rc %s as=%d macos=%d\n", rc, as, mac);
	if (!strcmp(rc, "CBR")) {
		key = "AverageBitRate"; can = true;
		if (mac >= MAC(13, 0)) {
			if (as) { key = "ConstantBitRate"; can = false; }
			else printf("  warn: CBR requires Apple Silicon, using ABR\n");
		} else
			printf("  warn: CBR requires macOS 13, using ABR\n");
	} else if (!strcmp(rc, "ABR")) {
		key = "AverageBitRate"; can = true;
	} else if (!strcmp(rc, "CRF")) {
		can = true;
		if (as) {
			key = "Quality";
			printf("  set Quality=%.9g (float32)\n", (float)q / 100);
		} else {
			printf("  warn: CRF requires Apple Silicon, using ABR\n");
			key = "AverageBitRate";
		}
	} else {
		printf("  error: unsupported rate control -> kVTParameterErr\n");
		return;
	}
	if (strcmp(key, "Quality"))
		printf("  set %s=%lld bps\n", key, br * 1000);
	if (limit && can) {
		double cpb = (double)mx * 125 * win;
		/* D-2: the reference hands the address of a double to CFNumberCreate with kCFNumberIntType / FloatType,
		 * so on a little-endian host only the low 32 bits of each double are read. */
		union { double d; unsigned u[2]; int i[2]; float f[2]; } a = {cpb}, b = {win};
		printf("  DataRateLimits intended_bytes=%.9g intended_window=%.9g reference_int_slot=%d reference_float_slot=%.9g\n",
		       cpb, win, a.i[0], (double)b.f[0]);
	} else
		printf("  DataRateLimits not set\n");
}

static void vt_visibility(const char *rc, bool use_limit)
{
	bool has_bitrate = true, can = true;
	if (!strcmp(rc, "CBR")) can = false;
	else if (!strcmp(rc, "CRF")) { can = true; has_bitrate = false; }
	printf("vt_visibility rc=%s limit=%s limit_bitrate=%s max_bitrate=%s max_bitrate_window=%s bitrate=%s quality=%s\n", rc,
	       tf(use_limit), tf(can), tf(can && use_limit), tf(can && use_limit), tf(has_bitrate), tf(!has_bitrate));
}

static void vt_pixfmt(const char *fmt, bool full, const char *codec)
{
	const char *r = NULL;
	if (!strcmp(fmt, "I420")) r = full ? "420YpCbCr8PlanarFullRange" : "420YpCbCr8Planar";
	else if (!strcmp(fmt, "NV12")) r = full ? "420YpCbCr8BiPlanarFullRange" : "420YpCbCr8BiPlanarVideoRange";
	else if (!strcmp(fmt, "P010") && !strcmp(codec, "hevc")) r = full ? "420YpCbCr10BiPlanarFullRange" : "420YpCbCr10BiPlanarVideoRange";
	else if (!strcmp(fmt, "P216")) {
		if (full) { printf("vt_pixfmt %s full=%d -> error FullRangeUnsupported\n", fmt, full); return; }
		r = "422YpCbCr16BiPlanarVideoRange";
	} else if (!strcmp(fmt, "P416")) {
		if (full) { printf("vt_pixfmt %s full=%d -> error FullRangeUnsupported\n", fmt, full); return; }
		r = "444YpCbCr16BiPlanarVideoRange";
	}
	if (r) printf("vt_pixfmt %s full=%d codec=%s -> %s\n", fmt, full, codec, r);
	else printf("vt_pixfmt %s full=%d codec=%s -> error ColorFormatUnsupported\n", fmt, full, codec);
}

static void vt_props(const char *codec, bool hw, bool as, int mac)
{
	printf("vt_props codec=%s hw=%d apple_silicon=%d macos=%d\n  rate_control:", codec, hw, as, mac);
	if (mac >= MAC(13, 0) && hw && as) printf(" CBR");
	printf(" ABR");
	if (hw && as) printf(" CRF");
	printf("\n  profile:");
	if (!strcmp(codec, "h264")) printf(" baseline main high");
	else {
		printf(" main main10");
		if (mac >= MAC(12, 3)) printf(" main42210");
	}
	printf("\n  spatial_aq_mode: %s\n", mac >= MAC(15, 0) ? "auto(1) disabled(2) enabled(3)" : "absent");
}

static void vt_defaults(const char *codec, bool hw, bool as, int mac)
{
	printf("vt_defaults codec=%s rate_control=%s bitrate=6000 quality=60 limit_bitrate=false max_bitrate=6000 "
	       "max_bitrate_window=1.5 keyint_sec=2 profile=%s codec_type=AppleProRes422 bframes=true spatial_aq_mode=1\n",
	       codec, (mac >= MAC(13, 0) && hw && as) ? "CBR" : "ABR", !strcmp(codec, "h264") ? "high" : "main");
}

static void vt_name(const char *d)
{
	const char *r = d;
	if (!strcmp(d, "Apple H.264 (HW)")) r = "VTH264EncHW";
	else if (!strcmp(d, "Apple H.264 (SW)")) r = "VTH264EncSW";
	else if (!strcmp(d, "Apple HEVC (HW)")) r = "VTHEVCEncHW";
	else if (!strcmp(d, "Apple HEVC (AVE)")) r = "VTHEVCEncT2";
	else if (!strcmp(d, "Apple HEVC (SW)")) r = "VTHEVCEncSW";
	else if (!strncmp(d, "AppleProResHW", 13)) r = "VTProResEncHW";
	else if (!strncmp(d, "Apple ProRes", 12)) r = "VTProResEncSW";
	printf("vt_name \"%s\" -> %s\n", d, r);
}

static void vt_params(struct script_line *sl)
{
	/* rate_control keyint_sec fps_num fps_den quality_int spatial_aq_mode(0 invalid,1 auto,2 disabled,3 enabled) */
	const char *rc = arg(sl, 1);
	long long k = num(sl, 2), fn = num(sl, 3), fd = num(sl, 4), q = num(sl, 5);
	int aq = (int)num(sl, 6);
	int fps = (int)(fn / fd);
	bool saq = aq == 1 ? !strcmp(rc, "CRF") : aq == 3; /* AQ_INVALID=0 AQ_AUTO=1 AQ_DISABLED=2 AQ_ENABLED=3 */
	printf("vt_params MaxKeyFrameIntervalDuration=%lld MaxKeyFrameInterval=%d ExpectedFrameRate=%d quality=%.9g "
	       "spatial_aq=%s\n",
	       k, (int)k * fps, fps, (float)q / 100, tf(saq));
}

static void vt_update_rule(struct script_line *sl)
{
	/* old_bitrate new_bitrate old_limit new_limit old_rc new_rc old_quality new_quality */
	bool changed = num(sl, 1) != num(sl, 2) || (num(sl, 3) != 0) != (num(sl, 4) != 0);
	printf("vt_update reapply_rate_control=%s\n", tf(changed));
}

/* ----------------------------------------------------------------- main */
int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		if (!strcmp(c, "sck_visibility")) {
			sck_visibility((int)num(&sl, 1));
		} else if (!strcmp(c, "sck_defaults")) {
			printf("sck_defaults application=(null) display_uuid=(null) type=0 window=0 show_cursor=true hide_obs=false "
			       "show_empty_names=false show_hidden_windows=false\n");
		} else if (!strcmp(c, "sck_audio_defaults")) {
			printf("sck_audio_defaults application=(null) type=0\n");
		} else if (!strcmp(c, "sck_update")) {
			sck_update(&sl);
		} else if (!strcmp(c, "sck_config")) {
			sck_config(&sl);
		} else if (!strcmp(c, "sck_window_frame")) {
			sck_window_frame(&sl);
		} else if (!strcmp(c, "sck_color_space")) {
			sck_color_space(&sl);
		} else if (!strcmp(c, "sck_dispatch")) {
			sck_dispatch(&sl);
		} else if (!strcmp(c, "sck_audio_cfg")) {
			sck_audio_cfg(&sl);
		} else if (!strcmp(c, "sck_audio_buf")) {
			sck_audio_buf(&sl);
		} else if (!strcmp(c, "sck_audio_visibility")) {
			printf("sck_audio_visibility type=%lld application=%s\n", num(&sl, 1), tf(num(&sl, 1) == 1));
		} else if (!strcmp(c, "sck_content_flags")) { /* kind(display|window) show_hidden */
			bool disp = !strcmp(arg(&sl, 1), "display");
			printf("sck_content_flags exclude_desktop_windows=true on_screen_windows_only=%s\n",
			       tf(disp ? false : !num(&sl, 2)));
		} else if (!strcmp(c, "scw_reset")) {
			nscw = 0; nsca = 0;
		} else if (!strcmp(c, "scw_add")) { /* id app title */
			scw[nscw].id = (int)num(&sl, 1);
			snprintf(scw[nscw].app, 64, "%s", nz(arg(&sl, 2)));
			snprintf(scw[nscw].title, 64, "%s", nz(arg(&sl, 3)));
			nscw++;
		} else if (!strcmp(c, "scw_list")) { /* show_empty */
			bool se = num(&sl, 1) != 0;
			typeof(scw[0]) t[MAXI];
			int n = 0;
			for (int i = 0; i < nscw; i++)
				if (se || (*scw[i].app && *scw[i].title))
					t[n++] = scw[i];
			qsort(t, n, sizeof t[0], cmp_win);
			printf("scw_list\n  \" \" = 0\n");
			for (int i = 0; i < n; i++)
				printf("  \"[%s] %s\" = %d\n", t[i].app, t[i].title, t[i].id);
		} else if (!strcmp(c, "sca_add")) {
			snprintf(sca[nsca].name, 64, "%s", nz(arg(&sl, 1)));
			snprintf(sca[nsca].bundle, 64, "%s", nz(arg(&sl, 2)));
			nsca++;
		} else if (!strcmp(c, "sca_list")) {
			typeof(sca[0]) t[MAXI];
			int n = 0;
			for (int i = 0; i < nsca; i++)
				if (*sca[i].name)
					t[n++] = sca[i];
			qsort(t, n, sizeof t[0], cmp_app);
			printf("sca_list\n  \" \" = (null)\n");
			for (int i = 0; i < n; i++)
				printf("  \"%s\" = \"%s\"\n", t[i].name, t[i].bundle);
		} else if (!strcmp(c, "sck_display_name")) { /* name w h x y */
			char d[4][12], nb[256];
			snprintf(d[0], 12, "%u", (uint32_t)num(&sl, 2));
			snprintf(d[1], 12, "%u", (uint32_t)num(&sl, 3));
			snprintf(d[2], 12, "%d", (int32_t)num(&sl, 4));
			snprintf(d[3], 12, "%d", (int32_t)num(&sl, 5));
			snprintf(nb, sizeof nb, "%.200s: %.12sx%.12s @ %.12s,%.12s", arg(&sl, 1), d[0], d[1], d[2], d[3]);
			printf("sck_display_name \"%s\"\n", nb);
		} else if (!strcmp(c, "sck_display_migrate")) { /* has_legacy has_new legacy_uuid_ok uuid_nonempty */
			bool l = num(&sl, 1), n = num(&sl, 2), ok = num(&sl, 3), ne = num(&sl, 4);
			const char *act = "none";
			bool zero = false;
			if (l && !n) {
				if (ok) act = "display_uuid<-uuid(legacy display), erase display";
				else { act = "none"; zero = true; }
			} else if (l && n)
				act = "erase display";
			if (zero) printf("sck_display_migrate action=%s result=0 (early return)\n", act);
			else printf("sck_display_migrate action=%s result=%s\n", act, ne || (l && !n && ok) ? "lookup-by-uuid" : "0");
		} else if (!strcmp(c, "cw_reset")) {
			ncw = 0;
		} else if (!strcmp(c, "cw_add")) { /* num pid owner name */
			cw[ncw].num = (int)num(&sl, 1);
			cw[ncw].pid = (int)num(&sl, 2);
			snprintf(cw[ncw].owner, 48, "%s", nz(arg(&sl, 3)));
			snprintf(cw[ncw].name, 48, "%s", nz(arg(&sl, 4)));
			ncw++;
		} else if (!strcmp(c, "cw_init")) { /* owner name pid window */
			const char *o = nz(arg(&sl, 1)), *n = nz(arg(&sl, 2));
			int pid = (int)num(&sl, 3), wid = (int)num(&sl, 4);
			qsort(cw, ncw, sizeof cw[0], cw_cmp);
			if (!*o && !*n) { printf("cw_init invalid_name -> no window\n"); continue; }
			for (int i = 0; i < ncw; i++) {
				bool om = !strcmp(o, cw[i].owner), idm = pid == cw[i].pid && wid == cw[i].num,
				     nm = !strcmp(n, cw[i].name);
				if (om && (idm || nm)) {
					printf("cw_init found window=%d pid=%d\n", cw[i].num, cw[i].pid);
					goto cw_done;
				}
			}
			printf("cw_init not found\n");
cw_done:;
		} else if (!strcmp(c, "cw_find")) { /* owner name  (force) */
			const char *o = nz(arg(&sl, 1)), *n = nz(arg(&sl, 2));
			qsort(cw, ncw, sizeof cw[0], cw_cmp);
			if (!*o && !*n) { printf("cw_find invalid_name -> false\n"); continue; }
			for (int i = 0; i < ncw; i++)
				if (!strcmp(o, cw[i].owner) && !strcmp(n, cw[i].name)) {
					printf("cw_find found window=%d pid=%d\n", cw[i].num, cw[i].pid);
					goto cf_done;
				}
			printf("cw_find not found\n");
cf_done:;
		} else if (!strcmp(c, "cw_list")) { /* window_id owner name show_empty */
			int wid = (int)num(&sl, 1);
			const char *wo = nz(arg(&sl, 2)), *wn = nz(arg(&sl, 3));
			bool se = num(&sl, 4) != 0;
			qsort(cw, ncw, sizeof cw[0], cw_cmp);
			typeof(cw[0]) key = {0, 0, "", ""};
			snprintf(key.owner, 48, "%s", wo);
			snprintf(key.name, 48, "%s", wn);
			bool found = false;
			for (int i = 0; i < ncw; i++) found |= cw[i].num == wid;
			bool added = found;
			char nb[128];
			printf("cw_list\n");
			for (int i = 0; i < ncw; i++) {
				/* win_info_cmp(key, dict): owner, (pid absent -> nil compares equal), name, number absent */
				int cmpr = strcmp(wo, cw[i].owner);
				if (!cmpr) cmpr = strcmp(wn, cw[i].name);
				if (!added && cmpr < 0) {
					added = true;
					printf("  \"%s\" = %d disabled\n", make_name(wo, wn, nb, sizeof nb), wid);
				}
				if (!se && !*cw[i].name && wid != cw[i].num)
					continue;
				printf("  \"%s\" = %d\n", make_name(cw[i].owner, cw[i].name, nb, sizeof nb), cw[i].num);
			}
			if (!added)
				printf("  \"%s\" = %d disabled\n", make_name(wo, wn, nb, sizeof nb), wid);
			(void)key;
		} else if (!strcmp(c, "dc_crop_mode")) {
			int m = (int)num(&sl, 1);
			printf("dc_crop_mode %d -> %d\n", m, (m >= 0 && m < 4) ? m : 0);
		} else if (!strcmp(c, "dc_visibility")) {
			int m = (int)num(&sl, 1);
			if (!(m >= 0 && m < 4)) m = 0;
			printf("dc_visibility mode=%d manual.*=%s window.*=%s window,show_empty_names=%s\n", m, tf(m == 1), tf(m == 3),
			       tf(m == 2 || m == 3));
		} else if (!strcmp(c, "dc_size")) { /* mode frame_w frame_h crop_x crop_y crop_w crop_h win_w win_h */
			int m = (int)num(&sl, 1);
			double fw = dnum(&sl, 2), fh = dnum(&sl, 3), cx = dnum(&sl, 4), cy = dnum(&sl, 5), cwid = dnum(&sl, 6),
			       chh = dnum(&sl, 7), ww = dnum(&sl, 8), wh = dnum(&sl, 9);
			double cropw = cx + cwid, croph = cy + chh;
			uint32_t w = 0, h = 0;
			switch (m) {
			case 0: w = (uint32_t)fw; h = (uint32_t)fh; break;
			case 1: w = (uint32_t)fabs(fw - cropw); h = (uint32_t)fabs(fh - croph); break;
			case 2: w = (uint32_t)ww; h = (uint32_t)wh; break;
			case 3: w = (uint32_t)fabs(ww - cropw); h = (uint32_t)fabs(wh - croph); break;
			}
			printf("dc_size mode=%d -> %ux%u\n", m, w, h);
		} else if (!strcmp(c, "dc_rect")) { /* mode fx fy fw fh cx cy cw ch wx wy ww wh */
			int m = (int)num(&sl, 1);
			double fx = dnum(&sl, 2), fy = dnum(&sl, 3), fw = dnum(&sl, 4), fh = dnum(&sl, 5);
			double cx = dnum(&sl, 6), cy = dnum(&sl, 7), cwid = dnum(&sl, 8), chh = dnum(&sl, 9);
			double wx = dnum(&sl, 10), wy = dnum(&sl, 11), ww = dnum(&sl, 12), wh = dnum(&sl, 13);
			double ox = 0, oy = 0, ex = 0, ey = 0;
			switch (m) {
			case 1:
				ox += cx; oy += cy; ey -= chh; ex -= cwid;
				/* fallthrough */
			case 0:
				ey += fh; ex += fw;
				break;
			case 3:
				ox += cx; oy += cy; ey -= chh; ex -= cwid;
				/* fallthrough */
			case 2: {
				double x = wx - fx, y = wy - fy;
				ox += x; oy += y;
				ey += wh + y; ex += ww + x;
			} break;
			}
			printf("dc_rect mode=%d origin=(%.9g,%.9g) end=(%.9g,%.9g)\n", m, (float)ox, (float)oy, (float)ex, (float)ey);
		} else if (!strcmp(c, "ca_format")) {
			printf("ca_format flags=0x%llx bits=%lld -> %s\n", num(&sl, 1), num(&sl, 2),
			       ca_format((unsigned)num(&sl, 1), (unsigned)num(&sl, 2)));
		} else if (!strcmp(c, "ca_layout")) {
			printf("ca_layout %lld -> %s\n", num(&sl, 1), ca_layout((unsigned)num(&sl, 1)));
		} else if (!strcmp(c, "ca_sanitize")) {
			char o[80];
			ca_sanitize(nz(arg(&sl, 1)), o);
			printf("ca_sanitize \"%s\"\n", o);
		} else if (!strcmp(c, "ca_listed")) { /* input(0|1) device_name */
			bool input = num(&sl, 1) != 0;
			const char *d = arg(&sl, 2);
			printf("ca_listed input=%d \"%s\" -> %s\n", input, d, tf(input || !device_is_input(d)));
		} else if (!strcmp(c, "ca_defaults")) {
			printf("ca_defaults device_id=default enable_downmix=true\n");
		} else if (!strcmp(c, "ca_resolve")) { /* uid input(0|1) first_output_device_uid_or_- */
			const char *uid = arg(&sl, 1);
			bool input = num(&sl, 2) != 0;
			const char *first = arg(&sl, 3);
			if (!strcasecmp(uid, "default")) {
				if (input)
					printf("ca_resolve default input -> follow system default input device (default_device=true)\n");
				else if (!strcmp(first, "-"))
					printf("ca_resolve default output -> no_devices=true, init fails\n");
				else
					printf("ca_resolve default output -> uid replaced by first listed output device \"%s\"\n", first);
			} else
				printf("ca_resolve \"%s\" -> lookup by uid\n", uid);
		} else if (!strcmp(c, "ca_chmap")) { /* uid then up to 8 "-"|int user values; downmix flag first */
			char o[80], name[160];
			ca_sanitize(nz(arg(&sl, 1)), o);
			printf("ca_chmap uid=\"%s\"\n", nz(arg(&sl, 1)));
			for (int i = 0; i < 8; i++) {
				snprintf(name, sizeof name, "output-%s-%d", o, i + 1);
				const char *v = arg(&sl, 2 + i);
				long long found = !strcmp(v, "-") ? -1 : atoll(v);
				long long adj = found > 0 ? found - 1 : -1;
				printf("  %s -> map[%d]=%lld\n", name, i, adj);
			}
		} else if (!strcmp(c, "ca_chmap_clamp")) { /* channels available m0 m1 ... */
			int ch = (int)num(&sl, 1), av = (int)num(&sl, 2);
			printf("ca_chmap_clamp [");
			for (int i = 0; i < ch; i++) {
				int m = (int)num(&sl, 3 + i);
				if (m >= av) m = -1;
				printf("%s%d", i ? "," : "", m);
			}
			printf("] (%d channels available)\n", av);
		} else if (!strcmp(c, "ca_combo")) { /* out_chan available */
			int oc = (int)num(&sl, 1), av = (int)num(&sl, 2);
			printf("ca_combo out=%d avail=%d:", oc, av);
			bool muted = false;
			if (oc < av) printf(" [ch%d=%d]", oc + 1, oc + 1);
			else { printf(" [None=-1]"); muted = true; }
			for (int i = 0; i < av; i++)
				if (i != oc) printf(" [ch%d=%d]", i + 1, i + 1);
			if (!muted) printf(" [None=-1]");
			printf("\n");
		} else if (!strcmp(c, "ca_retry")) { /* default_device_selector_changed(0|1) */
			printf("ca_retry_ms %d\n", num(&sl, 1) ? 300 : 2000);
		} else if (!strcmp(c, "ca_init_format")) { /* downmix avail speakers */
			bool dm = num(&sl, 1) != 0;
			uint32_t av = (uint32_t)num(&sl, 2);
			if (av > MAX_DEVICE_INPUT_CHANNELS) av = MAX_DEVICE_INPUT_CHANNELS;
			uint32_t ch = get_audio_channels((enum speaker_layout)num(&sl, 3));
			printf("ca_init_format available=%u output_channels=%u mode=%s\n", av, dm ? av : ch,
			       dm ? "downmix (libobs mixes)" : "channel map");
		} else if (!strcmp(c, "ca_buffer")) { /* speakers_count_buffers avail */
			uint32_t n = (uint32_t)num(&sl, 1);
			printf("ca_callback speakers=%u buffers=%u\n", n > MAX_AUDIO_CHANNELS ? MAX_AUDIO_CHANNELS : n, n);
		} else if (!strcmp(c, "av_defaults")) {
			bool fast = num(&sl, 1) != 0;
			if (fast)
				printf("av_defaults(fast) device=\"\" use_preset=false enable_audio=true\n");
			else
				printf("av_defaults device=\"\" use_preset=true preset=High enable_audio=true\n");
		} else if (!strcmp(c, "av_preset")) {
			const char *r = av_preset(arg(&sl, 1));
			if (r) printf("av_preset %s -> \"%s\"\n", arg(&sl, 1), r);
			else printf("av_preset %s -> \"Unknown (%s)\"\n", arg(&sl, 1), arg(&sl, 1));
		} else if (!strcmp(c, "av_aspect")) {
			av_aspect((int)num(&sl, 1), (int)num(&sl, 2));
		} else if (!strcmp(c, "av_fps_desc")) {
			av_fps_desc(&sl);
		} else if (!strcmp(c, "av_fallback_fps")) {
			av_fallback_fps(&sl);
		} else if (!strcmp(c, "av_subtype")) {
			printf("av_subtype %s -> format=%s full_range=%s\n", arg(&sl, 1), av_fmt(arg(&sl, 1)), tf(av_fullrange(arg(&sl, 1))));
		} else if (!strcmp(c, "av_fourcc")) {
			printf("av_fourcc %s full=%d -> %s\n", arg(&sl, 1), (int)num(&sl, 2), av_fourcc(arg(&sl, 1), num(&sl, 2)));
		} else if (!strcmp(c, "av_cs")) {
			static const char *const n[] = {"Default", "CS 601", "CS 709", "sRGB", "CS 2100 (PQ)", "CS 2100 (HLG)"};
			int v = (int)num(&sl, 1);
			printf("av_cs %d -> \"%s\" valid=%s\n", v, v >= 0 && v <= 5 ? n[v] : "Unknown", tf(v >= 0 && v <= 2));
		} else if (!strcmp(c, "av_range")) {
			static const char *const n[] = {"Default", "Partial", "Full"};
			printf("av_range %d -> \"%s\"\n", (int)num(&sl, 1), n[num(&sl, 1)]);
		} else if (!strcmp(c, "av_fourcc_str")) { /* 4 chars -> string and back */
			const char *s = arg(&sl, 1);
			uint32_t v = ((uint8_t)s[0] << 24) | ((uint8_t)s[1] << 16) | ((uint8_t)s[2] << 8) | (uint8_t)s[3];
			printf("av_fourcc_str \"%s\" -> 0x%08x\n", s, v);
		} else if (!strcmp(c, "av_update")) { /* cur_uuid new_uuid preset_based use_preset cur_preset new_preset */
			bool same_dev = !strcmp(arg(&sl, 1), arg(&sl, 2));
			bool pb = num(&sl, 3), up = num(&sl, 4);
			bool same_preset = !strcmp(arg(&sl, 5), arg(&sl, 6));
			const char *a;
			if (!same_dev) a = "switch_device_then_reconfigure";
			else if (pb && up && same_preset) a = "no_change";
			else a = up ? "configure_with_preset" : "configure_manual";
			printf("av_update %s\n", a);
		} else if (!strcmp(c, "vt_profile")) {
			bool w;
			const char *r = vt_profile(arg(&sl, 1), arg(&sl, 2), arg(&sl, 3), &w);
			printf("vt_profile %s %s %s -> %s%s\n", arg(&sl, 1), arg(&sl, 2), arg(&sl, 3), r, w ? " (warn: Forcing main10 for P010)" : "");
		} else if (!strcmp(c, "vt_cs")) {
			vt_cs(arg(&sl, 1));
		} else if (!strcmp(c, "vt_hdr")) {
			vt_hdr((unsigned)num(&sl, 1));
		} else if (!strcmp(c, "vt_rc")) {
			vt_rc(&sl);
		} else if (!strcmp(c, "vt_visibility")) {
			vt_visibility(arg(&sl, 1), num(&sl, 2) != 0);
		} else if (!strcmp(c, "vt_pixfmt")) {
			vt_pixfmt(arg(&sl, 1), num(&sl, 2) != 0, arg(&sl, 3));
		} else if (!strcmp(c, "vt_props")) {
			vt_props(arg(&sl, 1), num(&sl, 2) != 0, num(&sl, 3) != 0, (int)num(&sl, 4));
		} else if (!strcmp(c, "vt_defaults")) {
			vt_defaults(arg(&sl, 1), num(&sl, 2) != 0, num(&sl, 3) != 0, (int)num(&sl, 4));
		} else if (!strcmp(c, "vt_name")) {
			vt_name(arg(&sl, 1));
		} else if (!strcmp(c, "vt_params")) {
			vt_params(&sl);
		} else if (!strcmp(c, "vt_update")) {
			vt_update_rule(&sl);
		} else if (!strcmp(c, "vt_register")) { /* codec_type fourcc */
			const char *t = arg(&sl, 1);
			const char *r = "skipped";
			if (!strcmp(t, "avc1")) r = "register id=h264 codec=h264 props=h26x";
			else if (!strcmp(t, "hvc1")) r = "register codec=hevc props=h26x";
			else if (!strcmp(t, "ap4h-422")) r = "register codec=prores props=prores (marker 422)";
			else if (!strcmp(t, "prores-other")) r = "record in prores list only, no registration";
			printf("vt_register %s -> %s\n", t, r);
		} else if (!strcmp(c, "mach_ids")) {
			printf("mach_ids Connect=1 Frame=2 Stop=3 service=com.obsproject.obs-mac-virtualcam.server\n");
		} else {
			fprintf(stderr, "maccapture: unknown command '%s' at line %d\n", c, lineno);
			return 2;
		}
	}
	return 0;
}
