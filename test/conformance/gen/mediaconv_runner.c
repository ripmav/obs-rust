/*
 * Conformance runner for the pure parts of libobs/media-io (spec S-013).
 *
 * See the "mediaconv runner" section of test/conformance/README.md for the command list.
 * Test tooling, not part of libobs.
 *
 * Every command may be prefixed with `forked` to run it in a child process; a child that
 * dies or exits non-zero (e.g. through bcrash or SIGFPE) is reported as "result crash".
 */
#include <math.h>
#include <stddef.h>
#include <sys/wait.h>
#include <unistd.h>

#include "media-io/video-io.h"
#include "media-io/video-frame.h"
#include "media-io/audio-io.h"
#include "media-io/audio-math.h"
#include "media-io/format-conversion.h"
#include "media-io/frame-rate.h"
#include "harness.h"

/* defined in video-frame.c but not declared in the public header */
void video_frame_get_linesizes(uint32_t linesize[MAX_AV_PLANES], enum video_format format, uint32_t width);
void video_frame_get_plane_heights(uint32_t heights[MAX_AV_PLANES], enum video_format format, uint32_t height);

/* ------------------------------------------------------------------------- helpers */

static void crash_handler(const char *fmt, va_list args, void *param)
{
	(void)fmt;
	(void)args;
	(void)param;
	_exit(77);
}

static long long ival(const char *s)
{
	return strtoll(s, NULL, 0);
}
static unsigned long long uval(const char *s)
{
	return strtoull(s, NULL, 0);
}

/* floats: always the same text for non-finite values so that vectors do not depend on the libc */
static void pf(double v, int doubleprec)
{
	if (isnan(v))
		fputs("nan", stdout);
	else if (isinf(v))
		fputs(v < 0 ? "-inf" : "inf", stdout);
	else if (doubleprec)
		printf("%.17g", v);
	else
		printf("%.9g", v);
}

/* A zero-padded 32-byte aligned buffer; `data` may be placed at a small offset (misalignment test). */
struct abuf {
	uint8_t *base;
	uint8_t *data;
	size_t size;
};
#define GUARD 64

static void abuf_alloc(struct abuf *b, size_t size, int fill, size_t offset)
{
	size_t total = ((size + offset + 2 * GUARD) + 31) & ~(size_t)31;
	b->base = aligned_alloc(32, total);
	memset(b->base, fill, total);
	b->data = b->base + offset;
	b->size = size;
	memset(b->data + size, fill, GUARD);
}
static void abuf_from_hex(struct abuf *b, const char *hex, size_t offset)
{
	size_t n;
	uint8_t *raw = script_parse_hex(hex, &n);
	abuf_alloc(b, n, 0, offset);
	memcpy(b->data, raw, n);
	free(raw);
}
static void abuf_free(struct abuf *b)
{
	free(b->base);
}
static int guard_ok(const struct abuf *b, int fill)
{
	for (size_t i = 0; i < GUARD; i++)
		if (b->data[b->size + i] != fill)
			return 0;
	return 1;
}
/* prints `size` bytes as rows of `ls` bytes separated by spaces */
static void print_rows(const uint8_t *p, size_t ls, size_t rows)
{
	for (size_t r = 0; r < rows; r++) {
		if (r)
			putchar(' ');
		script_print_hex(p + r * ls, ls);
	}
}

/* ------------------------------------------------------------------------- frames */

#define NFRAMES 8
static struct video_frame frames[NFRAMES];
static int frame_id(const char *n)
{
	int id = atoi(n);
	if (id < 0 || id >= NFRAMES) {
		fprintf(stderr, "script: bad frame id %s\n", n);
		exit(2);
	}
	return id;
}

static void cmd_linesizes(const struct script_line *sl)
{
	script_need(sl, 3);
	uint32_t ls[MAX_AV_PLANES] = {0};
	enum video_format f = (enum video_format)ival(sl->argv[1]);
	video_frame_get_linesizes(ls, f, (uint32_t)uval(sl->argv[2]));
	printf("linesizes fmt=%s w=%s:", sl->argv[1], sl->argv[2]);
	for (int i = 0; i < MAX_AV_PLANES; i++)
		printf(" %u", ls[i]);
	printf("\n");
}

static void cmd_heights(const struct script_line *sl)
{
	script_need(sl, 3);
	uint32_t h[MAX_AV_PLANES] = {0};
	enum video_format f = (enum video_format)ival(sl->argv[1]);
	video_frame_get_plane_heights(h, f, (uint32_t)uval(sl->argv[2]));
	printf("heights fmt=%s h=%s:", sl->argv[1], sl->argv[2]);
	for (int i = 0; i < MAX_AV_PLANES; i++)
		printf(" %u", h[i]);
	printf("\n");
}

/* frame_init ID FMT W H : prints linesizes and the plane offsets relative to plane 0 */
static void cmd_frame_init(const struct script_line *sl)
{
	script_need(sl, 5);
	int id = frame_id(sl->argv[1]);
	struct video_frame *fr = &frames[id];
	video_frame_init(fr, (enum video_format)ival(sl->argv[2]), (uint32_t)uval(sl->argv[3]),
			 (uint32_t)uval(sl->argv[4]));
	printf("frame %d aligned32=%d linesize:", id, fr->data[0] ? (int)(((uintptr_t)fr->data[0] & 31) == 0) : -1);
	for (int i = 0; i < MAX_AV_PLANES; i++)
		printf(" %u", fr->linesize[i]);
	printf(" offsets:");
	for (int i = 0; i < MAX_AV_PLANES; i++) {
		if (fr->data[i])
			printf(" %lld", (long long)(fr->data[i] - fr->data[0]));
		else
			printf(" -");
	}
	printf("\n");
}

/* frame_fill ID FMT H MODE VALUE : MODE const -> every byte VALUE; seq -> byte = VALUE + plane*64 + index */
static void cmd_frame_fill(const struct script_line *sl)
{
	script_need(sl, 6);
	struct video_frame *fr = &frames[frame_id(sl->argv[1])];
	enum video_format f = (enum video_format)ival(sl->argv[2]);
	uint32_t h[MAX_AV_PLANES] = {0};
	video_frame_get_plane_heights(h, f, (uint32_t)uval(sl->argv[3]));
	int seq = !strcmp(sl->argv[4], "seq");
	unsigned v = (unsigned)uval(sl->argv[5]);
	for (int p = 0; p < MAX_AV_PLANES; p++) {
		if (!h[p] || !fr->data[p])
			continue;
		size_t n = (size_t)fr->linesize[p] * h[p];
		for (size_t i = 0; i < n; i++)
			fr->data[p][i] = (uint8_t)(seq ? v + p * 64 + i : v);
	}
}

/* frame_dump ID FMT H : prints every plane as rows of linesize bytes */
static void cmd_frame_dump(const struct script_line *sl)
{
	script_need(sl, 4);
	int id = frame_id(sl->argv[1]);
	struct video_frame *fr = &frames[id];
	enum video_format f = (enum video_format)ival(sl->argv[2]);
	uint32_t h[MAX_AV_PLANES] = {0};
	video_frame_get_plane_heights(h, f, (uint32_t)uval(sl->argv[3]));
	for (int p = 0; p < MAX_AV_PLANES; p++) {
		if (!h[p] || !fr->data[p])
			continue;
		printf("frame %d plane %d ls=%u rows=%u: ", id, p, fr->linesize[p], h[p]);
		print_rows(fr->data[p], fr->linesize[p], h[p]);
		printf("\n");
	}
}

/* frame_copy DST SRC FMT H */
static void cmd_frame_copy(const struct script_line *sl)
{
	script_need(sl, 5);
	video_frame_copy(&frames[frame_id(sl->argv[1])], &frames[frame_id(sl->argv[2])],
			 (enum video_format)ival(sl->argv[3]), (uint32_t)uval(sl->argv[4]));
	printf("frame_copy done\n");
}

static void cmd_frame_free(const struct script_line *sl)
{
	script_need(sl, 2);
	struct video_frame *fr = &frames[frame_id(sl->argv[1])];
	video_frame_free(fr);
	int clean = 1;
	for (int i = 0; i < MAX_AV_PLANES; i++)
		clean &= (fr->data[i] == NULL && fr->linesize[i] == 0);
	printf("frame_free zeroed=%d\n", clean);
	/* video_frame_free(NULL) must be a no-op */
	video_frame_free(NULL);
	video_frame_init(NULL, VIDEO_FORMAT_I420, 4, 4);
	printf("null-frame ok\n");
}

/* ------------------------------------------------------------------------- converters */

/* compress FN START END ILS OLS0 OLS1 OLS2 H0 H1 H2 INHEX [OFF] */
static void cmd_compress(const struct script_line *sl)
{
	script_need(sl, 12);
	const char *fn = sl->argv[1];
	uint32_t start = (uint32_t)uval(sl->argv[2]), end = (uint32_t)uval(sl->argv[3]);
	uint32_t ils = (uint32_t)uval(sl->argv[4]);
	uint32_t ols[3], hs[3];
	for (int i = 0; i < 3; i++) {
		ols[i] = (uint32_t)uval(sl->argv[5 + i]);
		hs[i] = (uint32_t)uval(sl->argv[8 + i]);
	}
	size_t off = sl->argc > 12 ? (size_t)uval(sl->argv[12]) : 0;
	struct abuf in, out[3];
	abuf_from_hex(&in, sl->argv[11], off);
	uint8_t *planes[3];
	for (int i = 0; i < 3; i++) {
		abuf_alloc(&out[i], (size_t)ols[i] * hs[i], 0xEE, 0);
		planes[i] = hs[i] ? out[i].data : NULL;
	}
	if (!strcmp(fn, "i420"))
		compress_uyvx_to_i420(in.data, ils, start, end, planes, ols);
	else if (!strcmp(fn, "nv12"))
		compress_uyvx_to_nv12(in.data, ils, start, end, planes, ols);
	else if (!strcmp(fn, "i444"))
		convert_uyvx_to_i444(in.data, ils, start, end, planes, ols);
	else {
		fprintf(stderr, "script: line %d: unknown converter %s\n", sl->lineno, fn);
		exit(2);
	}
	printf("compress %s\n", fn);
	for (int i = 0; i < 3; i++) {
		if (!hs[i])
			continue;
		printf("  plane %d ls=%u rows=%u guard=%s: ", i, ols[i], hs[i], guard_ok(&out[i], 0xEE) ? "intact" : "overrun");
		print_rows(out[i].data, ols[i], hs[i]);
		printf("\n");
	}
	abuf_free(&in);
	for (int i = 0; i < 3; i++)
		abuf_free(&out[i]);
}

/* decompress FN START END ILS0 ILS1 ILS2 OLS OH P0 P1 P2 [OFF]   FN = 420 | nv12 ; unused plane hex = - */
static void cmd_decompress(const struct script_line *sl)
{
	script_need(sl, 12);
	const char *fn = sl->argv[1];
	uint32_t start = (uint32_t)uval(sl->argv[2]), end = (uint32_t)uval(sl->argv[3]);
	uint32_t ils[3] = {(uint32_t)uval(sl->argv[4]), (uint32_t)uval(sl->argv[5]), (uint32_t)uval(sl->argv[6])};
	uint32_t ols = (uint32_t)uval(sl->argv[7]), oh = (uint32_t)uval(sl->argv[8]);
	size_t off = sl->argc > 12 ? (size_t)uval(sl->argv[12]) : 0;
	struct abuf in[3], out;
	const uint8_t *planes[3];
	for (int i = 0; i < 3; i++) {
		abuf_from_hex(&in[i], sl->argv[9 + i], off);
		planes[i] = in[i].data;
	}
	abuf_alloc(&out, (size_t)ols * oh, 0xEE, 0);
	if (!strcmp(fn, "420"))
		decompress_420(planes, ils, start, end, out.data, ols);
	else if (!strcmp(fn, "nv12"))
		decompress_nv12(planes, ils, start, end, out.data, ols);
	else {
		fprintf(stderr, "script: line %d: unknown converter %s\n", sl->lineno, fn);
		exit(2);
	}
	printf("decompress %s ols=%u rows=%u guard=%s: ", fn, ols, oh, guard_ok(&out, 0xEE) ? "intact" : "overrun");
	print_rows(out.data, ols, oh);
	printf("\n");
	for (int i = 0; i < 3; i++)
		abuf_free(&in[i]);
	abuf_free(&out);
}

/* decompress422 lead|trail START END ILS OLS OH INHEX [OFF] */
static void cmd_decompress422(const struct script_line *sl)
{
	script_need(sl, 8);
	bool lead = !strcmp(sl->argv[1], "lead");
	uint32_t start = (uint32_t)uval(sl->argv[2]), end = (uint32_t)uval(sl->argv[3]);
	uint32_t ils = (uint32_t)uval(sl->argv[4]), ols = (uint32_t)uval(sl->argv[5]);
	uint32_t oh = (uint32_t)uval(sl->argv[6]);
	size_t off = sl->argc > 8 ? (size_t)uval(sl->argv[8]) : 0;
	struct abuf in, out;
	abuf_from_hex(&in, sl->argv[7], off);
	abuf_alloc(&out, (size_t)ols * oh, 0xEE, 0);
	decompress_422(in.data, ils, start, end, out.data, ols, lead);
	printf("decompress422 %s ols=%u rows=%u guard=%s: ", sl->argv[1], ols, oh,
	       guard_ok(&out, 0xEE) ? "intact" : "overrun");
	print_rows(out.data, ols, oh);
	printf("\n");
	abuf_free(&in);
	abuf_free(&out);
}

/* ------------------------------------------------------------------------- matrices */

/* vparams CS RANGE FMT [nomin] [nomax]   FMT = - selects video_format_get_parameters (no format) */
static void cmd_vparams(const struct script_line *sl)
{
	script_need(sl, 4);
	enum video_colorspace cs = (enum video_colorspace)ival(sl->argv[1]);
	enum video_range_type rt = (enum video_range_type)ival(sl->argv[2]);
	bool nomin = false, nomax = false;
	for (int i = 4; i < sl->argc; i++) {
		nomin |= !strcmp(sl->argv[i], "nomin");
		nomax |= !strcmp(sl->argv[i], "nomax");
	}
	float m[16], mn[3], mx[3];
	for (int i = 0; i < 16; i++)
		m[i] = 7.0f; /* sentinel: shows whether the function wrote the matrix */
	for (int i = 0; i < 3; i++)
		mn[i] = mx[i] = 7.0f;
	bool ok;
	if (!strcmp(sl->argv[3], "-"))
		ok = video_format_get_parameters(cs, rt, m, nomin ? NULL : mn, nomax ? NULL : mx);
	else
		ok = video_format_get_parameters_for_format(cs, rt, (enum video_format)ival(sl->argv[3]), m,
							    nomin ? NULL : mn, nomax ? NULL : mx);
	printf("vparams cs=%s range=%s fmt=%s ok=%d\n", sl->argv[1], sl->argv[2], sl->argv[3], ok);
	for (int r = 0; r < 4; r++) {
		printf("  row%d", r);
		for (int c = 0; c < 4; c++) {
			putchar(' ');
			pf(m[r * 4 + c], 0);
		}
		printf("\n");
	}
	printf("  min");
	for (int i = 0; i < 3; i++) {
		putchar(' ');
		pf(mn[i], 0);
	}
	printf("\n  max");
	for (int i = 0; i < 3; i++) {
		putchar(' ');
		pf(mx[i], 0);
	}
	printf("\n");
}

/* ------------------------------------------------------------------------- enums, names, audio */

#define E(x) printf(#x "=%d\n", (int)(x))

static void cmd_enums(void)
{
	E(VIDEO_FORMAT_NONE);
	E(VIDEO_FORMAT_I420);
	E(VIDEO_FORMAT_NV12);
	E(VIDEO_FORMAT_YVYU);
	E(VIDEO_FORMAT_YUY2);
	E(VIDEO_FORMAT_UYVY);
	E(VIDEO_FORMAT_RGBA);
	E(VIDEO_FORMAT_BGRA);
	E(VIDEO_FORMAT_BGRX);
	E(VIDEO_FORMAT_Y800);
	E(VIDEO_FORMAT_I444);
	E(VIDEO_FORMAT_BGR3);
	E(VIDEO_FORMAT_I422);
	E(VIDEO_FORMAT_I40A);
	E(VIDEO_FORMAT_I42A);
	E(VIDEO_FORMAT_YUVA);
	E(VIDEO_FORMAT_AYUV);
	E(VIDEO_FORMAT_I010);
	E(VIDEO_FORMAT_P010);
	E(VIDEO_FORMAT_I210);
	E(VIDEO_FORMAT_I412);
	E(VIDEO_FORMAT_YA2L);
	E(VIDEO_FORMAT_P216);
	E(VIDEO_FORMAT_P416);
	E(VIDEO_FORMAT_V210);
	E(VIDEO_FORMAT_R10L);
	E(VIDEO_TRC_DEFAULT);
	E(VIDEO_TRC_SRGB);
	E(VIDEO_TRC_PQ);
	E(VIDEO_TRC_HLG);
	E(VIDEO_CS_DEFAULT);
	E(VIDEO_CS_601);
	E(VIDEO_CS_709);
	E(VIDEO_CS_SRGB);
	E(VIDEO_CS_2100_PQ);
	E(VIDEO_CS_2100_HLG);
	E(VIDEO_RANGE_DEFAULT);
	E(VIDEO_RANGE_PARTIAL);
	E(VIDEO_RANGE_FULL);
	E(VIDEO_SCALE_DEFAULT);
	E(VIDEO_SCALE_POINT);
	E(VIDEO_SCALE_FAST_BILINEAR);
	E(VIDEO_SCALE_BILINEAR);
	E(VIDEO_SCALE_BICUBIC);
	E(VIDEO_OUTPUT_SUCCESS);
	E(VIDEO_OUTPUT_INVALIDPARAM);
	E(VIDEO_OUTPUT_FAIL);
	E(AUDIO_FORMAT_UNKNOWN);
	E(AUDIO_FORMAT_U8BIT);
	E(AUDIO_FORMAT_16BIT);
	E(AUDIO_FORMAT_32BIT);
	E(AUDIO_FORMAT_FLOAT);
	E(AUDIO_FORMAT_U8BIT_PLANAR);
	E(AUDIO_FORMAT_16BIT_PLANAR);
	E(AUDIO_FORMAT_32BIT_PLANAR);
	E(AUDIO_FORMAT_FLOAT_PLANAR);
	E(SPEAKERS_UNKNOWN);
	E(SPEAKERS_MONO);
	E(SPEAKERS_STEREO);
	E(SPEAKERS_2POINT1);
	E(SPEAKERS_4POINT0);
	E(SPEAKERS_4POINT1);
	E(SPEAKERS_5POINT1);
	E(SPEAKERS_7POINT1);
	E(AUDIO_OUTPUT_SUCCESS);
	E(AUDIO_OUTPUT_INVALIDPARAM);
	E(AUDIO_OUTPUT_FAIL);
	E(MAX_AV_PLANES);
	E(MAX_AUDIO_MIXES);
	E(MAX_AUDIO_CHANNELS);
	E(MAX_DEVICE_INPUT_CHANNELS);
	E(AUDIO_OUTPUT_FRAMES);
	printf("TOTAL_AUDIO_SIZE=%zu\n", (size_t)TOTAL_AUDIO_SIZE);
}

#define SZ(t) printf("sizeof(" #t ")=%zu\n", sizeof(t))
#define OF(t, f) printf("offsetof(" #t "," #f ")=%zu\n", offsetof(t, f))
static void cmd_layouts(void)
{
	SZ(enum video_format);
	SZ(enum audio_format);
	SZ(enum speaker_layout);
	SZ(struct video_frame);
	OF(struct video_frame, data);
	OF(struct video_frame, linesize);
	SZ(struct video_data);
	OF(struct video_data, data);
	OF(struct video_data, linesize);
	OF(struct video_data, timestamp);
	SZ(struct audio_data);
	OF(struct audio_data, data);
	OF(struct audio_data, frames);
	OF(struct audio_data, timestamp);
	SZ(struct audio_output_data);
	SZ(struct video_scale_info);
	OF(struct video_scale_info, format);
	OF(struct video_scale_info, width);
	OF(struct video_scale_info, height);
	OF(struct video_scale_info, range);
	OF(struct video_scale_info, colorspace);
	SZ(struct audio_convert_info);
	OF(struct audio_convert_info, samples_per_sec);
	OF(struct audio_convert_info, format);
	OF(struct audio_convert_info, speakers);
	OF(struct audio_convert_info, allow_clipping);
	SZ(struct media_frames_per_second);
	OF(struct media_frames_per_second, numerator);
	OF(struct media_frames_per_second, denominator);
}

/* vinfo FMT : name, yuv flag, default range */
static void cmd_vinfo(const struct script_line *sl)
{
	script_need(sl, 2);
	enum video_format f = (enum video_format)ival(sl->argv[1]);
	printf("vinfo %s name=%s yuv=%d default_range=%s", sl->argv[1], get_video_format_name(f), format_is_yuv(f),
	       get_video_range_name(f, VIDEO_RANGE_DEFAULT));
	printf(" resolve(default)=%d resolve(partial)=%d resolve(full)=%d\n",
	       (int)resolve_video_range(f, VIDEO_RANGE_DEFAULT), (int)resolve_video_range(f, VIDEO_RANGE_PARTIAL),
	       (int)resolve_video_range(f, VIDEO_RANGE_FULL));
}

/* rangename FMT RANGE */
static void cmd_rangename(const struct script_line *sl)
{
	script_need(sl, 3);
	enum video_format f = (enum video_format)ival(sl->argv[1]);
	enum video_range_type r = (enum video_range_type)ival(sl->argv[2]);
	printf("rangename fmt=%s range=%s: resolved=%d name=%s\n", sl->argv[1], sl->argv[2], (int)resolve_video_range(f, r),
	       get_video_range_name(f, r));
}

static void cmd_csname(const struct script_line *sl)
{
	script_need(sl, 2);
	printf("csname %s=%s\n", sl->argv[1], get_video_colorspace_name((enum video_colorspace)ival(sl->argv[1])));
}

/* fourcc TOKEN : TOKEN is four characters or 0xHEX */
static void cmd_fourcc(const struct script_line *sl)
{
	script_need(sl, 2);
	uint32_t v;
	if (!strncmp(sl->argv[1], "0x", 2)) {
		v = (uint32_t)uval(sl->argv[1]);
	} else {
		const unsigned char *s = (const unsigned char *)sl->argv[1];
		v = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
	}
	enum video_format f = video_format_from_fourcc(v);
	printf("fourcc 0x%08x -> %d (%s)\n", v, (int)f, get_video_format_name(f));
}

/* ainfo FMT SPEAKERS FRAMES */
static void cmd_ainfo(const struct script_line *sl)
{
	script_need(sl, 4);
	enum audio_format f = (enum audio_format)ival(sl->argv[1]);
	enum speaker_layout s = (enum speaker_layout)ival(sl->argv[2]);
	uint32_t frames_n = (uint32_t)uval(sl->argv[3]);
	printf("ainfo fmt=%s spk=%s frames=%s: channels=%u bytes_per_channel=%zu planar=%d planes=%zu size=%zu total=%zu\n",
	       sl->argv[1], sl->argv[2], sl->argv[3], get_audio_channels(s), get_audio_bytes_per_channel(f),
	       is_audio_planar(f), get_audio_planes(f, s), get_audio_size(f, s, frames_n),
	       get_total_audio_size(f, s, frames_n));
}

static void cmd_audio_ns(const struct script_line *sl)
{
	script_need(sl, 3);
	size_t rate = (size_t)uval(sl->argv[1]);
	if (!strcmp(sl->argv[0], "frames_to_ns"))
		printf("frames_to_ns rate=%s frames=%s: %llu\n", sl->argv[1], sl->argv[2],
		       (unsigned long long)audio_frames_to_ns(rate, uval(sl->argv[2])));
	else
		printf("ns_to_frames rate=%s ns=%s: %llu\n", sl->argv[1], sl->argv[2],
		       (unsigned long long)ns_to_audio_frames(rate, uval(sl->argv[2])));
}

static void cmd_db(const struct script_line *sl)
{
	script_need(sl, 2);
	float v = (float)strtod(sl->argv[1], NULL);
	printf("%s %s: ", sl->argv[0], sl->argv[1]);
	pf(!strcmp(sl->argv[0], "mul_to_db") ? mul_to_db(v) : db_to_mul(v), 0);
	printf("\n");
}

static void cmd_fps(const struct script_line *sl)
{
	script_need(sl, 3);
	struct media_frames_per_second fps = {(uint32_t)uval(sl->argv[1]), (uint32_t)uval(sl->argv[2])};
	printf("fps %s/%s: interval=", sl->argv[1], sl->argv[2]);
	pf(media_frames_per_second_to_frame_interval(fps), 1);
	printf(" fps=");
	pf(media_frames_per_second_to_fps(fps), 1);
	printf(" valid=%d\n", media_frames_per_second_is_valid(fps));
}

/* ------------------------------------------------------------------------- dispatcher */

static int exec(const struct script_line *sl)
{
	const char *c = sl->argv[0];
	if (!strcmp(c, "linesizes"))
		cmd_linesizes(sl);
	else if (!strcmp(c, "heights"))
		cmd_heights(sl);
	else if (!strcmp(c, "frame_init"))
		cmd_frame_init(sl);
	else if (!strcmp(c, "frame_fill"))
		cmd_frame_fill(sl);
	else if (!strcmp(c, "frame_dump"))
		cmd_frame_dump(sl);
	else if (!strcmp(c, "frame_copy"))
		cmd_frame_copy(sl);
	else if (!strcmp(c, "frame_free"))
		cmd_frame_free(sl);
	else if (!strcmp(c, "compress"))
		cmd_compress(sl);
	else if (!strcmp(c, "decompress"))
		cmd_decompress(sl);
	else if (!strcmp(c, "decompress422"))
		cmd_decompress422(sl);
	else if (!strcmp(c, "vparams"))
		cmd_vparams(sl);
	else if (!strcmp(c, "enums"))
		cmd_enums();
	else if (!strcmp(c, "layouts"))
		cmd_layouts();
	else if (!strcmp(c, "vinfo"))
		cmd_vinfo(sl);
	else if (!strcmp(c, "rangename"))
		cmd_rangename(sl);
	else if (!strcmp(c, "csname"))
		cmd_csname(sl);
	else if (!strcmp(c, "fourcc"))
		cmd_fourcc(sl);
	else if (!strcmp(c, "ainfo"))
		cmd_ainfo(sl);
	else if (!strcmp(c, "frames_to_ns") || !strcmp(c, "ns_to_frames"))
		cmd_audio_ns(sl);
	else if (!strcmp(c, "mul_to_db") || !strcmp(c, "db_to_mul"))
		cmd_db(sl);
	else if (!strcmp(c, "fps"))
		cmd_fps(sl);
	else {
		fprintf(stderr, "script: line %d: unknown command '%s'\n", sl->lineno, c);
		return 2;
	}
	return 0;
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	base_set_crash_handler(crash_handler, NULL);

	while (script_next(f, &sl, &lineno)) {
		if (!strcmp(sl.argv[0], "forked")) {
			script_need(&sl, 2);
			memmove(sl.argv, sl.argv + 1, sizeof(char *) * (size_t)(sl.argc - 1));
			sl.argc--;
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				int rc = exec(&sl);
				fflush(stdout);
				_exit(rc);
			}
			int status = 0;
			waitpid(pid, &status, 0);
			if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
				if (WIFEXITED(status) && WEXITSTATUS(status) == 2)
					return 2;
				printf("result crash\n");
			}
			continue;
		}
		int rc = exec(&sl);
		if (rc)
			return rc;
	}
	fclose(f);
	return 0;
}
