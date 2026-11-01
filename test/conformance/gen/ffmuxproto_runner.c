/*
 * Conformance runner for the obs-ffmpeg-mux stdin protocol (spec S-037).
 *
 * The real ffmpeg-mux.c is compiled into this file (main renamed to ffmux_main). Every case
 * forks a child that gets the scripted bytes as stdin and either runs the real main() ("run")
 * or only the real initialisation followed by a dump of the stream parameters ("inspect").
 * The parent prints exit status, stdout/stderr of the child and, with "probe", re-reads the
 * produced file with libavformat. See README section "ffmuxproto runner".
 */
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>

#define main ffmux_main
#include "../../../plugins/obs-ffmpeg/ffmpeg-mux/ffmpeg-mux.c"
#undef main

#include "harness.h"
#include <libavutil/pixdesc.h>

static const char *tmpdir;

#define MAXARGS 128
static char *g_args[MAXARGS];
static int g_nargs;

static uint8_t *g_in;
static size_t g_in_len, g_in_cap;

static void in_put(const void *p, size_t n)
{
	if (g_in_len + n > g_in_cap) {
		g_in_cap = (g_in_len + n) * 2 + 64;
		g_in = realloc(g_in, g_in_cap);
	}
	if (n)
		memcpy(g_in + g_in_len, p, n);
	g_in_len += n;
}

static char *expand(const char *s)
{
	char buf[2048];
	if (s[0] == '@' && s[1])
		snprintf(buf, sizeof(buf), "%s/ffmuxproto-%s", tmpdir, s + 1);
	else
		snprintf(buf, sizeof(buf), "%s", s);
	return strdup(buf);
}

static void args_clear(void)
{
	for (int i = 0; i < g_nargs; i++)
		free(g_args[i]);
	g_nargs = 0;
}

static void args_add(const char *s)
{
	if (g_nargs >= MAXARGS)
		exit(2);
	g_args[g_nargs++] = expand(s);
}

static void put_info(int64_t pts, int64_t dts, uint32_t size, uint32_t index, int type, int key)
{
	struct ffm_packet_info info;
	memset(&info, 0, sizeof(info));
	info.pts = pts;
	info.dts = dts;
	info.size = size;
	info.index = index;
	info.type = (enum ffm_packet_type)type;
	info.keyframe = key ? 1 : 0;
	in_put(&info, sizeof(info));
}

static int type_of(const char *s)
{
	if (!strcmp(s, "video"))
		return 0;
	if (!strcmp(s, "audio"))
		return 1;
	if (!strcmp(s, "change"))
		return 2;
	return atoi(s);
}

static void put_fill(size_t n, int b)
{
	for (size_t i = 0; i < n; i++) {
		uint8_t v = (uint8_t)b;
		in_put(&v, 1);
	}
}

/* ---- child side ------------------------------------------------------- */

static void dump_hex(const uint8_t *d, int n)
{
	if (n <= 0 || !d) {
		fputs("-", stdout);
		return;
	}
	for (int i = 0; i < n; i++)
		printf("%02x", d[i]);
}

static void dump_stream(const char *label, AVStream *st, AVCodecContext *ctx)
{
	/* Values are read from the codec context the program built (the muxer may rewrite codecpar
	 * in write_header, e.g. codec tag or bit rate, which is container specific). */
	AVCodecParameters *p = st->codecpar;
	char lay[128] = "";
	printf("%s: stream_id=%d type=%s codec=%s tag=0x%08x bit_rate=%lld\n", label, st->id,
	       av_get_media_type_string(ctx->codec_type), avcodec_get_name(ctx->codec_id), ctx->codec_tag,
	       (long long)ctx->bit_rate);
	if (ctx->codec_type == AVMEDIA_TYPE_VIDEO) {
		printf("  size=%dx%d coded=%dx%d primaries=%d trc=%d space=%d range=%d chroma_loc=%d\n", ctx->width,
		       ctx->height, ctx->coded_width, ctx->coded_height, ctx->color_primaries, ctx->color_trc,
		       ctx->colorspace, ctx->color_range, ctx->chroma_sample_location);
		printf("  ctx_time_base=%d/%d avg_frame_rate=%d/%d global_header=%d\n", ctx->time_base.num,
		       ctx->time_base.den, st->avg_frame_rate.num, st->avg_frame_rate.den,
		       !!(ctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER));
	} else {
		av_channel_layout_describe(&ctx->ch_layout, lay, sizeof(lay));
		printf("  sample_rate=%d frame_size=%d channels=%d layout=%s initial_padding=%d\n", ctx->sample_rate,
		       ctx->frame_size, ctx->ch_layout.nb_channels, lay, ctx->initial_padding);
		AVDictionaryEntry *e = av_dict_get(st->metadata, "title", NULL, 0);
		printf("  ctx_time_base=%d/%d global_header=%d title=", ctx->time_base.num, ctx->time_base.den,
		       !!(ctx->flags & AV_CODEC_FLAG_GLOBAL_HEADER));
		script_print_quoted(e ? e->value : "(none)");
		putchar('\n');
	}
	printf("  extradata=");
	dump_hex(ctx->extradata, ctx->extradata_size);
	putchar('\n');
	for (int i = 0; i < p->nb_coded_side_data; i++) {
		const AVPacketSideData *sd = &p->coded_side_data[i];
		if (sd->type == AV_PKT_DATA_CONTENT_LIGHT_LEVEL) {
			const AVContentLightMetadata *c = (const AVContentLightMetadata *)sd->data;
			printf("  side_data content_light size=%zu MaxCLL=%u MaxFALL=%u\n", sd->size, c->MaxCLL,
			       c->MaxFALL);
		} else if (sd->type == AV_PKT_DATA_MASTERING_DISPLAY_METADATA) {
			const AVMasteringDisplayMetadata *m = (const AVMasteringDisplayMetadata *)sd->data;
			printf("  side_data mastering size=%zu prim=", sd->size);
			for (int c = 0; c < 3; c++)
				printf("%s%d/%d,%d/%d", c ? ";" : "", m->display_primaries[c][0].num,
				       m->display_primaries[c][0].den, m->display_primaries[c][1].num,
				       m->display_primaries[c][1].den);
			printf(" white=%d/%d,%d/%d min=%d/%d max=%d/%d has_prim=%d has_lum=%d\n",
			       m->white_point[0].num, m->white_point[0].den, m->white_point[1].num,
			       m->white_point[1].den, m->min_luminance.num, m->min_luminance.den,
			       m->max_luminance.num, m->max_luminance.den, m->has_primaries, m->has_luminance);
		} else {
			printf("  side_data other type=%d\n", (int)sd->type);
		}
	}
}

static int child_inspect(int argc, char **argv)
{
	struct ffmpeg_mux ffm = {0};
	int ret = ffmpeg_mux_init(&ffm, argc, argv);
	printf("init=%d\n", ret);
	if (ret != FFM_SUCCESS)
		return ret;
	printf("params: has_video=%d tracks=%d num_audio_streams=%d nb_streams=%u\n", ffm.params.has_video,
	       ffm.params.tracks, ffm.num_audio_streams, ffm.output->nb_streams);
	printf("format=%s\n", ffm.output->oformat->name);
	printf("stream_key=");
	script_print_quoted(global_stream_key);
	printf(" printable_file=");
	script_print_quoted(ffm.params.printable_file.array);
	putchar('\n');
	if (ffm.video_stream)
		dump_stream("video", ffm.video_stream, ffm.video_ctx);
	for (int i = 0; i < ffm.num_audio_streams; i++) {
		char l[32];
		snprintf(l, sizeof(l), "audio%d", i);
		dump_stream(l, ffm.audio_infos[i].stream, ffm.audio_infos[i].ctx);
	}
	fflush(stdout);
	ffmpeg_mux_free(&ffm);
	return 0;
}

/* ---- parent side ------------------------------------------------------ */

static void replace_print(const char *prefix, const char *text)
{
	/* print text line by line, tmpdir replaced by '@' */
	const char *p = text;
	size_t tl = strlen(tmpdir);
	char line[4096];
	while (*p) {
		const char *nl = strchr(p, '\n');
		size_t n = nl ? (size_t)(nl - p) : strlen(p);
		char *o = line;
		for (size_t i = 0; i < n && o < line + sizeof(line) - 2;) {
			if (n - i >= tl && !strncmp(p + i, tmpdir, tl)) {
				*o++ = '@';
				i += tl;
				/* path form is TMP/ffmuxproto-NAME -> @NAME */
				if (!strncmp(p + i, "/ffmuxproto-", 12))
					i += 12;
			} else
				*o++ = p[i++];
		}
		*o = 0;
		printf("%s|", prefix);
		script_print_quoted(line);
		putchar('\n');
		if (!nl)
			break;
		p = nl + 1;
	}
}

static char *slurp(const char *path)
{
	FILE *f = fopen(path, "rb");
	char *b = calloc(1, 65536);
	if (f) {
		fread(b, 1, 65535, f);
		fclose(f);
	}
	return b;
}

static void run_child(bool inspect)
{
	char inp[1024], outp[1024], errp[1024];
	snprintf(inp, sizeof(inp), "%s/ffmuxproto-child.in", tmpdir);
	snprintf(outp, sizeof(outp), "%s/ffmuxproto-child.out", tmpdir);
	snprintf(errp, sizeof(errp), "%s/ffmuxproto-child.err", tmpdir);
	FILE *f = fopen(inp, "wb");
	if (g_in_len)
		fwrite(g_in, 1, g_in_len, f);
	fclose(f);

	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		int fi = open(inp, O_RDONLY);
		int fo = open(outp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		int fe = open(errp, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		dup2(fi, 0);
		dup2(fo, 1);
		dup2(fe, 2);
		char *argv[MAXARGS + 2];
		argv[0] = "ffmpeg-mux";
		for (int i = 0; i < g_nargs; i++)
			argv[1 + i] = g_args[i];
		argv[1 + g_nargs] = NULL;
		int rc = inspect ? child_inspect(1 + g_nargs, argv) : ffmux_main(1 + g_nargs, argv);
		fflush(stdout);
		fflush(stderr);
		_exit(rc & 0xff);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	if (WIFEXITED(status))
		printf("exit %d\n", (int)(signed char)WEXITSTATUS(status));
	else
		printf("signal %d\n", WTERMSIG(status));
	char *o = slurp(outp), *e = slurp(errp);
	replace_print("stdout", o);
	replace_print("stderr", e);
	free(o);
	free(e);
}

static void probe(const char *path, bool rel)
{
	AVFormatContext *fc = NULL;
	if (avformat_open_input(&fc, path, NULL, NULL) < 0) {
		puts("probe: cannot open");
		return;
	}
	printf("probe: format=%s streams=%u\n", fc->iformat->name, fc->nb_streams);
	for (unsigned i = 0; i < fc->nb_streams; i++) {
		AVStream *s = fc->streams[i];
		AVCodecParameters *p = s->codecpar;
		printf("  stream %u: %s %s", i, av_get_media_type_string(p->codec_type), avcodec_get_name(p->codec_id));
		if (p->codec_type == AVMEDIA_TYPE_VIDEO)
			printf(" %dx%d", p->width, p->height);
		else
			printf(" rate=%d ch=%d", p->sample_rate, p->ch_layout.nb_channels);
		AVDictionaryEntry *e = av_dict_get(s->metadata, "title", NULL, 0);
		if (e) {
			printf(" title=");
			script_print_quoted(e->value);
		}
		printf(" tb=%d/%d extradata=", s->time_base.num, s->time_base.den);
		dump_hex(p->extradata, p->extradata_size);
		putchar('\n');
	}
	AVPacket *pk = av_packet_alloc();
	int n = 0;
	int64_t base = INT64_MIN;
	while (av_read_frame(fc, pk) >= 0) {
		if (rel) {
			/* container start offsets are muxer specific: print relative to the first dts */
			if (base == INT64_MIN)
				base = pk->dts;
			pk->pts -= base;
			pk->dts -= base;
		}
		printf("  pkt stream=%d size=%d pts=%lld dts=%lld key=%d first=", pk->stream_index, pk->size,
		       (long long)pk->pts, (long long)pk->dts, !!(pk->flags & AV_PKT_FLAG_KEY));
		dump_hex(pk->data, pk->size < 4 ? pk->size : 4);
		putchar('\n');
		av_packet_unref(pk);
		n++;
	}
	printf("  packets=%d\n", n);
	av_packet_free(&pk);
	avformat_close_input(&fc);
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	tmpdir = getenv("CONFORMANCE_TMPDIR");
	if (!tmpdir)
		tmpdir = ".";
	av_log_set_level(AV_LOG_QUIET);

	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		if (!strcmp(c, "reset")) {
			args_clear();
			g_in_len = 0;
		} else if (!strcmp(c, "args")) {
			args_clear();
			for (int i = 1; i < sl.argc; i++)
				args_add(sl.argv[i]);
		} else if (!strcmp(c, "argsadd")) {
			for (int i = 1; i < sl.argc; i++)
				args_add(sl.argv[i]);
		} else if (!strcmp(c, "raw")) {
			for (int i = 1; i < sl.argc; i++) {
				size_t n;
				uint8_t *b = script_parse_hex(sl.argv[i], &n);
				in_put(b, n);
				free(b);
			}
		} else if (!strcmp(c, "str")) {
			script_need(&sl, 2);
			in_put(sl.argv[1], strlen(sl.argv[1]));
		} else if (!strcmp(c, "fill")) {
			script_need(&sl, 3);
			put_fill(strtoul(sl.argv[1], NULL, 0), (int)strtoul(sl.argv[2], NULL, 16));
		} else if (!strcmp(c, "info")) {
			/* info TYPE INDEX KEY PTS DTS SIZE */
			script_need(&sl, 7);
			put_info(strtoll(sl.argv[4], NULL, 0), strtoll(sl.argv[5], NULL, 0),
				 (uint32_t)strtoul(sl.argv[6], NULL, 0), (uint32_t)strtoul(sl.argv[2], NULL, 0),
				 type_of(sl.argv[1]), atoi(sl.argv[3]));
		} else if (!strcmp(c, "hdr")) {
			/* hdr video|audio INDEX HEX|-  : extradata packet, pts=dts=0, keyframe 0 */
			script_need(&sl, 4);
			size_t n;
			uint8_t *b = script_parse_hex(sl.argv[3], &n);
			put_info(0, 0, (uint32_t)n, (uint32_t)strtoul(sl.argv[2], NULL, 0), type_of(sl.argv[1]), 0);
			in_put(b, n);
			free(b);
		} else if (!strcmp(c, "frame")) {
			/* frame video|audio INDEX KEY PTS DTS N FILLHEX */
			script_need(&sl, 8);
			size_t n = strtoul(sl.argv[6], NULL, 0);
			put_info(strtoll(sl.argv[4], NULL, 0), strtoll(sl.argv[5], NULL, 0), (uint32_t)n,
				 (uint32_t)strtoul(sl.argv[2], NULL, 0), type_of(sl.argv[1]), atoi(sl.argv[3]));
			put_fill(n, (int)strtoul(sl.argv[7], NULL, 16));
		} else if (!strcmp(c, "change")) {
			script_need(&sl, 2);
			char *p = expand(sl.argv[1]);
			put_info(0, 0, (uint32_t)strlen(p), 0, 2, 0);
			in_put(p, strlen(p));
			free(p);
		} else if (!strcmp(c, "cut")) {
			script_need(&sl, 2);
			size_t n = strtoul(sl.argv[1], NULL, 0);
			g_in_len = n > g_in_len ? 0 : g_in_len - n;
		} else if (!strcmp(c, "dump")) {
			printf("stdin %zu bytes\n", g_in_len);
			for (size_t i = 0; i < g_in_len; i += 16) {
				printf("  %04zx ", i);
				for (size_t j = i; j < i + 16 && j < g_in_len; j++)
					printf(" %02x", g_in[j]);
				putchar('\n');
			}
		} else if (!strcmp(c, "sizeof")) {
			printf("sizeof(info)=%zu pts@%zu dts@%zu size@%zu index@%zu type@%zu keyframe@%zu\n",
			       sizeof(struct ffm_packet_info), offsetof(struct ffm_packet_info, pts),
			       offsetof(struct ffm_packet_info, dts), offsetof(struct ffm_packet_info, size),
			       offsetof(struct ffm_packet_info, index), offsetof(struct ffm_packet_info, type),
			       offsetof(struct ffm_packet_info, keyframe));
		} else if (!strcmp(c, "run")) {
			run_child(false);
		} else if (!strcmp(c, "inspect")) {
			run_child(true);
		} else if (!strcmp(c, "probe")) {
			script_need(&sl, 2);
			char *p = expand(sl.argv[1]);
			probe(p, sl.argc > 2 && !strcmp(sl.argv[2], "rel"));
			free(p);
		} else if (!strcmp(c, "exists")) {
			script_need(&sl, 2);
			char *p = expand(sl.argv[1]);
			printf("exists %s: %d\n", sl.argv[1], access(p, F_OK) == 0);
			free(p);
		} else if (!strcmp(c, "rmfile")) {
			script_need(&sl, 2);
			char *p = expand(sl.argv[1]);
			unlink(p);
			free(p);
		} else if (!strcmp(c, "chdirtmp")) {
			/* later relative file names live in the scratch directory */
			if (chdir(tmpdir))
				perror("chdir");
		} else if (!strcmp(c, "linkdev")) {
			/* linkdev NAME TARGET : symlink @NAME -> TARGET (e.g. /dev/full) */
			script_need(&sl, 3);
			char *p = expand(sl.argv[1]);
			unlink(p);
			if (symlink(sl.argv[2], p))
				perror("symlink");
			free(p);
		} else if (!strcmp(c, "echo")) {
			for (int i = 1; i < sl.argc; i++)
				printf("%s%s", i > 1 ? " " : "", sl.argv[i]);
			putchar('\n');
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, c);
			return 2;
		}
	}
	return 0;
}
