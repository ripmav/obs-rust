/*
 * Conformance runner for the MP4/MOV muxer (spec S-022).
 *
 * The runner #includes plugins/obs-outputs/mp4-mux.c so that it can reach the muxer's state
 * (creation time override). Script language: see test/conformance/README.md ("mp4 runner").
 *
 * Output is deterministic: the muxer writes its file through the real file serializer into
 * $CONFORMANCE_TMPDIR/mp4-out.bin; the runner prints the file as hex rows and as a decoded box
 * tree. No paths, times or pointers are printed.
 */
#include <sys/wait.h>
#include <unistd.h>

/* The muxer stamps files with time(NULL); the harness makes that a script input ("clock"). */
#include <time.h>
static time_t g_clock = 1700000000;
static time_t mp4stub_time(time_t *t)
{
	if (t)
		*t = g_clock;
	return g_clock;
}
#define time(x) mp4stub_time(x)
#include "../../../plugins/obs-outputs/mp4-mux.c"
#undef time

#include <util/file-serializer.h>

#include "harness.h"

/* Interface to mp4_stubs.c */
void mp4stub_reset(void);
obs_output_t *mp4stub_output(void);
void mp4stub_set_output_name(const char *name);
void mp4stub_set_peak(float peak);
void mp4stub_set_version(const char *v);
void mp4stub_set_start_chapter_text(const char *v);
obs_encoder_t *mp4stub_encoder_new(bool video);
long mp4stub_encoder_refs(obs_encoder_t *e);
void mp4stub_encoder_set_slot(obs_encoder_t *e, size_t idx);
bool mp4stub_encoder_set(obs_encoder_t *e, const char *key, const char *val, size_t extra_len, const uint8_t *extra);
enum obs_encoder_type mp4stub_encoder_type(obs_encoder_t *e);
uint32_t mp4stub_encoder_timebase_num(obs_encoder_t *e);
uint32_t mp4stub_encoder_timebase_den(obs_encoder_t *e);
const char *mp4stub_encoder_codec(obs_encoder_t *e);
void mp4stub_encoder_free(obs_encoder_t *e);

#define MAX_ENC 16

static struct {
	char name[32];
	obs_encoder_t *enc;
} g_enc[MAX_ENC];
static int g_enc_count;

static struct mp4_mux *g_mux;
static struct serializer g_file;
static bool g_file_open;
static char g_path[512];
static unsigned g_fill_seq;
static unsigned g_vslot_next, g_aslot_next;


/* ------------------------------------------------------------------ */

static obs_encoder_t *find_enc(const char *name)
{
	for (int i = 0; i < g_enc_count; i++)
		if (!strcmp(g_enc[i].name, name))
			return g_enc[i].enc;
	fprintf(stderr, "script: unknown encoder '%s'\n", name);
	exit(2);
}

static void close_file(void)
{
	if (g_file_open) {
		file_output_serializer_free(&g_file);
		g_file_open = false;
	}
}

static void destroy_mux(void)
{
	if (g_mux) {
		mp4_mux_destroy(g_mux);
		g_mux = NULL;
	}
	close_file();
}

static void do_reset(void)
{
	destroy_mux();
	for (int i = 0; i < g_enc_count; i++)
		mp4stub_encoder_free(g_enc[i].enc);
	g_enc_count = 0;
	g_vslot_next = g_aslot_next = 0;
	g_fill_seq = 0;
	mp4stub_reset();
}

/* ------------------------------------------------------------------ */
/* Box tree                                                            */

static uint32_t be32(const uint8_t *p)
{
	return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static uint64_t be64(const uint8_t *p)
{
	return ((uint64_t)be32(p) << 32) | be32(p + 4);
}

static bool is_type(const uint8_t *t, const char *s)
{
	return !memcmp(t, s, 4);
}

struct walk {
	const uint8_t *d;
	char handler[5]; /* handler_type of the track being walked */
	bool is_mov;
};

static void print_type(const uint8_t *t)
{
	for (int i = 0; i < 4; i++) {
		if (t[i] >= 0x20 && t[i] < 0x7f && t[i] != '"' && t[i] != '\\')
			putchar(t[i]);
		else
			printf("\\x%02x", t[i]);
	}
}

/* Returns the number of payload bytes (after the box header) that precede child boxes, or -1 if
 * the box is a leaf for the tree printer. parent is the parent box type (or NULL). */
static long child_skip(struct walk *w, const uint8_t *type, const uint8_t *parent, const uint8_t *payload,
		       size_t payload_len)
{
	static const char *const plain[] = {"moov", "trak", "edts", "mdia", "minf", "dinf", "stbl", "mvex",
					    "moof", "traf", "tref", "ilst", "wave", "gmhd"};
	for (size_t i = 0; i < sizeof(plain) / sizeof(plain[0]); i++)
		if (is_type(type, plain[i]))
			return 0;
	if (is_type(type, "udta"))
		return 0;
	if (is_type(type, "meta") || is_type(type, "keys"))
		return is_type(type, "meta") ? 4 : 8;
	if (is_type(type, "stsd") || is_type(type, "dref"))
		return 8;
	if (parent && is_type(parent, "ilst"))
		return 0; /* item atoms: children are the data atom */
	if (parent && is_type(parent, "stsd")) {
		if (!strcmp(w->handler, "vide"))
			return payload_len >= 78 ? 78 : -1;
		if (!strcmp(w->handler, "soun")) {
			if (payload_len < 28)
				return -1;
			unsigned ver = (payload[8] << 8) | payload[9];
			if (w->is_mov && ver == 2)
				return payload_len >= 64 ? 64 : -1;
			if (w->is_mov && ver == 1)
				return payload_len >= 44 ? 44 : -1;
			return 28;
		}
	}
	return -1;
}

static void walk_boxes(struct walk *w, size_t start, size_t end, int depth, const uint8_t *parent)
{
	const uint8_t *d = w->d;
	const uint8_t *prev = NULL;
	while (start < end) {
		if (end - start < 8) {
			printf("%*s... %zu trailing bytes at %zu\n", depth * 2, "", end - start, start);
			return;
		}
		uint64_t size = be32(d + start);
		size_t hdr = 8;
		if (size == 1 && end - start >= 16) {
			size = be64(d + start + 8);
			hdr = 16;
		} else if (size == 0) {
			size = end - start;
		}
		if (size < hdr || size > end - start) {
			printf("%*s... bad box at %zu (size %llu, available %zu)\n", depth * 2, "", start,
			       (unsigned long long)size, end - start);
			return;
		}
		const uint8_t *type = d + start + 4;
		printf("%*s", depth * 2, "");
		print_type(type);
		printf(" @%zu size=%llu\n", start, (unsigned long long)size);

		const uint8_t *payload = d + start + hdr;
		size_t payload_len = (size_t)size - hdr;

		if (is_type(type, "hdlr") && parent && is_type(parent, "mdia") && payload_len >= 12)
			memcpy(w->handler, payload + 8, 4), w->handler[4] = 0;

		long skip = child_skip(w, type, parent, payload, payload_len);
		/* Soft-remuxed file: the mdat that directly follows ftyp starts with the 8 payload bytes
		 * of the former placeholder "free" box; the original fragmented file follows. */
		if (!parent && is_type(type, "mdat") && prev && is_type(prev, "ftyp") && payload_len >= 8)
			skip = 8;
		if (skip >= 0 && (size_t)skip <= payload_len)
			walk_boxes(w, start + hdr + (size_t)skip, start + (size_t)size, depth + 1, type);
		prev = type;
		start += (size_t)size;
	}
}

static uint8_t *read_output(size_t *len)
{
	fflush(NULL);
	FILE *f = fopen(g_path, "rb");
	*len = 0;
	if (!f)
		return calloc(1, 1);
	fseek(f, 0, SEEK_END);
	long n = ftell(f);
	fseek(f, 0, SEEK_SET);
	uint8_t *buf = malloc((size_t)n + 1);
	size_t got = fread(buf, 1, (size_t)n, f);
	fclose(f);
	*len = got;
	return buf;
}

static void print_hex_rows(const uint8_t *d, size_t len)
{
	for (size_t off = 0; off < len; off += 32) {
		size_t n = len - off < 32 ? len - off : 32;
		printf("hex %08zx ", off);
		script_print_hex(d + off, n);
		printf("\n");
	}
}

static void print_tree(const uint8_t *d, size_t len)
{
	struct walk w = {.d = d};
	w.is_mov = len >= 12 && is_type(d + 8, "qt  ");
	printf("tree\n");
	walk_boxes(&w, 0, len, 1, NULL);
	printf("tree end\n");
}

/* ------------------------------------------------------------------ */
/* Packets                                                             */

/* Builds a deterministic payload of n bytes for the encoder's codec. Fill bytes are >= 0x80 so
 * that no start code or zero run appears inside. */
static uint8_t *gen_payload(obs_encoder_t *enc, size_t n, bool key)
{
	uint8_t *b = malloc(n ? n : 1);
	size_t hdr = 0;
	const char *codec = mp4stub_encoder_codec(enc);
	bool video = mp4stub_encoder_type(enc) == OBS_ENCODER_VIDEO;

	if (video && codec && !strcmp(codec, "h264") && n >= 5) {
		const uint8_t h[] = {0, 0, 0, 1, key ? 0x65 : 0x41};
		memcpy(b, h, 5);
		hdr = 5;
	} else if (video && codec && !strcmp(codec, "hevc") && n >= 6) {
		const uint8_t h[] = {0, 0, 0, 1, key ? 0x26 : 0x02, 0x01};
		memcpy(b, h, 6);
		hdr = 6;
	} else if (video && codec && !strcmp(codec, "av1") && n >= 3 && n - 2 < 128) {
		b[0] = 0x32; /* OBU_FRAME, has_size_field */
		b[1] = (uint8_t)(n - 2);
		b[2] = key ? 0x10 : 0x30; /* show_existing_frame=0, frame_type KEY(0)/INTER(1), show_frame=1 */
		hdr = 3;
	}
	for (size_t i = hdr; i < n; i++)
		b[i] = (uint8_t)(0x80 | ((g_fill_seq + i) & 0x3f));
	g_fill_seq++;
	return b;
}

static void cmd_pkt(struct script_line *sl)
{
	script_need(sl, 6);
	obs_encoder_t *enc = find_enc(sl->argv[1]);
	int64_t pts = strtoll(sl->argv[2], NULL, 0);
	int64_t dts = strtoll(sl->argv[3], NULL, 0);
	bool key = strtol(sl->argv[4], NULL, 0) != 0;
	uint8_t *data;
	size_t size;

	if (!strncmp(sl->argv[5], "gen:", 4)) {
		size = (size_t)strtoul(sl->argv[5] + 4, NULL, 0);
		data = gen_payload(enc, size, key);
	} else {
		data = script_parse_hex(sl->argv[5], &size);
	}

	/* Packet data carries the reference count in front, as produced by obs_encoder_packet_create_instance */
	long *refs = bmalloc(size + sizeof(long));
	*refs = 1;
	memcpy(refs + 1, data, size);
	free(data);

	struct encoder_packet pkt = {0};
	pkt.data = (uint8_t *)(refs + 1);
	pkt.size = size;
	pkt.pts = pts;
	pkt.dts = dts;
	pkt.timebase_num = (int32_t)mp4stub_encoder_timebase_num(enc);
	pkt.timebase_den = (int32_t)mp4stub_encoder_timebase_den(enc);
	pkt.type = mp4stub_encoder_type(enc);
	pkt.keyframe = key;
	pkt.encoder = enc;
	if (sl->argc > 6)
		pkt.priority = pkt.drop_priority = atoi(sl->argv[6]);

	bool ok = mp4_mux_submit_packet(g_mux, &pkt);
	/* The muxer took its own reference; drop ours */
	obs_encoder_packet_release(&pkt);
	printf("pkt %s pts=%lld dts=%lld key=%d size=%zu -> %d pos=%lld\n", sl->argv[1], (long long)pts,
	       (long long)dts, key, size, ok, (long long)serializer_get_pos(&g_file));
}

/* ------------------------------------------------------------------ */

static void cmd_enc(struct script_line *sl)
{
	script_need(sl, 3);
	if (g_enc_count >= MAX_ENC) {
		fprintf(stderr, "script: too many encoders\n");
		exit(2);
	}
	bool video = !strcmp(sl->argv[2], "video");
	obs_encoder_t *e = mp4stub_encoder_new(video);
	snprintf(g_enc[g_enc_count].name, sizeof(g_enc[0].name), "%s", sl->argv[1]);
	g_enc[g_enc_count++].enc = e;
	size_t slot = video ? g_vslot_next : g_aslot_next;
	bool has_slot = true;

	for (int i = 3; i + 1 < sl->argc; i += 2) {
		const char *k = sl->argv[i], *v = sl->argv[i + 1];
		if (!strcmp(k, "slot")) {
			slot = strtoul(v, NULL, 0);
		} else if (!strcmp(k, "noslot")) {
			has_slot = false;
		} else if (!strcmp(k, "extra")) {
			size_t n;
			uint8_t *x = script_parse_hex(v, &n);
			mp4stub_encoder_set(e, "extra", strcmp(v, "-") ? "set" : "-", n, x);
			free(x);
		} else if (!mp4stub_encoder_set(e, k, v, 0, NULL)) {
			fprintf(stderr, "script: line %d: unknown enc key '%s'\n", sl->lineno, k);
			exit(2);
		}
	}
	if (has_slot) {
		mp4stub_encoder_set_slot(e, slot);
		if (video)
			g_vslot_next = (unsigned)slot + 1;
		else
			g_aslot_next = (unsigned)slot + 1;
	}
}

static void cmd_mux(struct script_line *sl)
{
	script_need(sl, 2);
	destroy_mux();
	snprintf(g_path, sizeof(g_path), "%s/mp4-out.bin", getenv("CONFORMANCE_TMPDIR") ? getenv("CONFORMANCE_TMPDIR") : ".");
	if (!file_output_serializer_init(&g_file, g_path)) {
		fprintf(stderr, "script: cannot open output file\n");
		exit(2);
	}
	g_file_open = true;

	enum mp4_flavor flavor = !strcmp(sl->argv[1], "mov") ? FLAVOR_MOV : !strcmp(sl->argv[1], "cmaf") ? FLAVOR_CMAF : FLAVOR_MP4;
	unsigned flags = 0;
	for (int i = 2; i < sl->argc; i++) {
		if (!strcmp(sl->argv[i], "mdta"))
			flags |= MP4_USE_MDTA_KEY_VALUE;
		else if (!strcmp(sl->argv[i], "encinfo"))
			flags |= MP4_WRITE_ENCODER_INFO;
		else if (!strcmp(sl->argv[i], "skipfin"))
			flags |= MP4_SKIP_FINALISATION;
		else if (!strcmp(sl->argv[i], "negcts"))
			flags |= MP4_USE_NEGATIVE_CTS;
		else {
			fprintf(stderr, "script: line %d: unknown mux flag '%s'\n", sl->lineno, sl->argv[i]);
			exit(2);
		}
	}
	g_mux = mp4_mux_create(mp4stub_output(), &g_file, (enum mp4_mux_flags)flags, flavor);
	printf("mux %s tracks=%zu\n", sl->argv[1], g_mux->tracks.num);
}

static void cmd_dump(const char *what)
{
	size_t len;
	uint8_t *d = read_output(&len);
	if (!strcmp(what, "size") || !strcmp(what, "dump"))
		printf("size %zu\n", len);
	if (!strcmp(what, "hex") || !strcmp(what, "dump"))
		print_hex_rows(d, len);
	if (!strcmp(what, "tree") || !strcmp(what, "dump"))
		print_tree(d, len);
	free(d);
}

static int run_command(struct script_line *sl);

static void cmd_try(struct script_line *sl)
{
	script_need(sl, 2);
	struct script_line inner = *sl;
	inner.argc = sl->argc - 1;
	for (int i = 0; i < inner.argc; i++)
		inner.argv[i] = sl->argv[i + 1];

	fflush(NULL);
	pid_t pid = fork();
	if (pid == 0) {
		run_command(&inner);
		fflush(stdout);
		_exit(0);
	}
	int status = 0;
	waitpid(pid, &status, 0);
	if (WIFEXITED(status))
		printf("try %s ok\n", inner.argv[0]);
	else
		printf("try %s crash\n", inner.argv[0]);
}

static int run_command(struct script_line *sl)
{
	const char *c = sl->argv[0];

	if (!strcmp(c, "reset")) {
		do_reset();
	} else if (!strcmp(c, "echo")) {
		printf("echo");
		for (int i = 1; i < sl->argc; i++)
			printf(" %s", sl->argv[i]);
		printf("\n");
	} else if (!strcmp(c, "version")) {
		script_need(sl, 2);
		mp4stub_set_version(sl->argv[1]);
	} else if (!strcmp(c, "peak")) {
		script_need(sl, 2);
		mp4stub_set_peak(strtof(sl->argv[1], NULL));
	} else if (!strcmp(c, "start_chapter_text")) {
		script_need(sl, 2);
		mp4stub_set_start_chapter_text(sl->argv[1]);
	} else if (!strcmp(c, "enc")) {
		cmd_enc(sl);
	} else if (!strcmp(c, "mux")) {
		cmd_mux(sl);
	} else if (!strcmp(c, "clock")) {
		/* seconds since 1970 returned by time(NULL) at the next "mux" */
		script_need(sl, 2);
		g_clock = (time_t)strtoll(sl->argv[1], NULL, 0);
	} else if (!strcmp(c, "pkt")) {
		cmd_pkt(sl);
	} else if (!strcmp(c, "chapter")) {
		script_need(sl, 3);
		bool ok = mp4_mux_add_chapter(g_mux, strtoll(sl->argv[1], NULL, 0), strcmp(sl->argv[2], "-") ? sl->argv[2] : "");
		printf("chapter %s -> %d\n", sl->argv[1], ok);
	} else if (!strcmp(c, "finalise")) {
		bool ok = mp4_mux_finalise(g_mux);
		printf("finalise -> %d\n", ok);
	} else if (!strcmp(c, "pos")) {
		printf("pos %lld\n", (long long)serializer_get_pos(&g_file));
	} else if (!strcmp(c, "size") || !strcmp(c, "hex") || !strcmp(c, "tree") || !strcmp(c, "dump")) {
		cmd_dump(c);
	} else if (!strcmp(c, "refs")) {
		script_need(sl, 2);
		printf("refs %s %ld\n", sl->argv[1], mp4stub_encoder_refs(find_enc(sl->argv[1])));
	} else if (!strcmp(c, "destroy")) {
		mp4_mux_destroy(g_mux);
		g_mux = NULL;
		printf("destroy\n");
	} else if (!strcmp(c, "try")) {
		cmd_try(sl);
	} else {
		fprintf(stderr, "script: line %d: unknown command '%s'\n", sl->lineno, c);
		exit(2);
	}
	return 0;
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;

	mp4stub_reset();
	while (script_next(f, &sl, &lineno))
		run_command(&sl);
	do_reset();
	fclose(f);
	return 0;
}
