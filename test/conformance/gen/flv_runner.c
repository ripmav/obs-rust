/*
 * Conformance runner for the FLV muxer, the HEVC/AV1 configuration-record builders and the
 * AMF0 codec (spec S-021): plugins/obs-outputs/flv-mux.c, rtmp-hevc.c, rtmp-av1.c and
 * librtmp/amf.c. See the "flv runner" section of test/conformance/README.md for the script
 * language; this comment lists the commands.
 *
 * Every *action* command runs in a forked child (stdout is collected through a pipe). A child
 * that dies (assert, SIGSEGV, SIGFPE) makes the parent print "<command> crash". Commands that
 * read a caller supplied byte string receive it in memory whose last byte is directly followed
 * by an inaccessible page, so that any over-read of the reference is a crash, not a flaky value.
 * State commands (buf, cfg, pkt, obj, add, ...) run in the parent and are inherited by children.
 *
 * DATA arguments: hex string, "-" (empty), @name (buffer defined with buf/cat/rep), "@null".
 *
 * State commands
 *   buf NAME HEX...            define buffer NAME as the concatenation of the hex tokens
 *   cat NAME DATA...           NAME = concatenation of DATA arguments
 *   rep NAME COUNT HEX         NAME = HEX repeated COUNT times
 *   trail N                    N readable filler bytes (0xA5) follow the input (disables over-read detection)
 *   echo TEXT
 *   cfg KEY VALUE              scripted values of the fake encoders/output (see spec 3.3):
 *                              vcodec width height vbitrate fps abitrate arate channels version
 *   cap N                      capacity of the AMF encode buffer (default 4096)
 *   pkt NAME TYPE PTS DTS NUM DEN KEY DATA      define an encoder_packet (TYPE video|audio)
 *   obj NAME                   define an empty AMFObject
 *   add OBJ PNAME KIND [VALUE] append a property: KIND number V | bool V | string S | null |
 *                              object @O | ecma @O | strict @O | date V UTC | invalid | type N V
 *                              (PNAME "-" = no name). Strings: literal text or buf:NAME.
 * Action commands (FLV)
 *   meta WRITE_HEADER          flv_meta_data
 *   fileinfo DURATION_MS SIZE [WRITE_HEADER]   flv_meta_data to a temp file, then write_file_info
 *   flv_mux PKT DTS_OFFSET IS_HEADER           flv_packet_mux
 *   ex_start PKT CODEC IDX | ex_frames PKT CODEC DTS_OFFSET IDX | ex_end PKT CODEC IDX
 *   ex_raw PKT CODEC DTS_OFFSET TYPE IDX       flv_packet_ex with any packet type
 *   ex_metadata CODEC IDX BITS PRIM TRC SPACE MINL MAXL    flv_packet_metadata
 *   ax_start PKT CODEC IDX | ax_frames PKT CODEC DTS_OFFSET IDX | ax_raw PKT CODEC DTS_OFFSET TYPE IDX
 *   ms PKT VALUE | to_video TEXT | to_audio TEXT
 * Action commands (AMF)
 *   enc_int16|enc_int24|enc_int32 V, enc_number V, enc_bool V, enc_string S
 *   enc_named_string NAME S, enc_named_number NAME V, enc_named_bool NAME V
 *   enc_obj OBJ [object|ecma|strict], enc_prop OBJ INDEX
 *   dec_int16|dec_int24|dec_int32|dec_number|dec_bool|dec_string|dec_longstring DATA
 *   decode DATA [BDECODENAME=1], decode_array DATA COUNT [BDECODENAME=0], prop_decode DATA [BDECODENAME=0]
 *   getprop DATA NAME_OR_INDEX            decode DATA, then AMF_GetProp (index if numeric >= 0 else name)
 * Action commands (codec config)
 *   hevc_header DATA | av1_header DATA | av1_packet DATA [PRIORITY [KEYFRAME]] | rbsp DATA HEADER_LEN
 *   layout
 */
#include <stddef.h>
#include <math.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "harness.h"
#include "obs.h"
#include "../../../plugins/obs-outputs/flv-mux.h"
#include "../../../plugins/obs-outputs/rtmp-hevc.h"
#include "../../../plugins/obs-outputs/rtmp-av1.h"
#include "../../../plugins/obs-outputs/librtmp/amf.h"
#include "../../../plugins/obs-outputs/librtmp/log.h"

/* flv-mux.c exports that are not in flv-mux.h */
extern void flv_packet_ex(struct encoder_packet *packet, enum video_id_t codec_id, int32_t dts_offset, uint8_t **output,
			  size_t *size, int type, size_t idx);
extern void flv_packet_audio_ex(struct encoder_packet *packet, enum audio_id_t codec_id, int32_t dts_offset,
				uint8_t **output, size_t *size, int type, size_t idx);
extern uint8_t *ff_nal_unit_extract_rbsp(uint8_t *dst, const uint8_t *src, int src_len, uint32_t *dst_len,
					 int header_len);
/* flv_stubs.c */
struct flv_fake {
	char vcodec[32];
	uint32_t width, height;
	double vbitrate;
	double fps;
	double abitrate;
	uint32_t arate;
	size_t channels;
	char version[8192];
};
extern struct flv_fake flv_fake;
extern obs_output_t *flv_fake_output(void);

static FILE *out; /* where action commands write (a memstream in the child) */

/* ------------------------------------------------------------------------------------------ */
/* buffers, guard pages                                                                        */

#define MAX_BUFS 64
static struct {
	char name[64];
	uint8_t *data;
	size_t len;
} bufs[MAX_BUFS];
static int nbufs;
static int trail_bytes;

static void set_buf(const char *name, uint8_t *data, size_t len)
{
	for (int i = 0; i < nbufs; i++)
		if (!strcmp(bufs[i].name, name)) {
			free(bufs[i].data);
			bufs[i].data = data;
			bufs[i].len = len;
			return;
		}
	if (nbufs >= MAX_BUFS) {
		fprintf(stderr, "script: too many buffers\n");
		exit(2);
	}
	snprintf(bufs[nbufs].name, sizeof(bufs[nbufs].name), "%s", name);
	bufs[nbufs].data = data;
	bufs[nbufs].len = len;
	nbufs++;
}

static bool find_buf(const char *name, const uint8_t **d, size_t *len)
{
	for (int i = 0; i < nbufs; i++)
		if (!strcmp(bufs[i].name, name)) {
			*d = bufs[i].data;
			*len = bufs[i].len;
			return true;
		}
	return false;
}

/* DATA argument -> malloc'd copy (never NULL, except for "@null" where *is_null is set) */
static uint8_t *resolve(const char *arg, size_t *len, bool *is_null)
{
	*is_null = false;
	if (!strcmp(arg, "@null")) {
		*is_null = true;
		*len = 0;
		return NULL;
	}
	if (arg[0] == '@') {
		const uint8_t *d;
		if (!find_buf(arg + 1, &d, len)) {
			fprintf(stderr, "script: unknown buffer %s\n", arg);
			exit(2);
		}
		uint8_t *c = malloc(*len ? *len : 1);
		if (*len)
			memcpy(c, d, *len);
		return c;
	}
	return script_parse_hex(arg, len);
}

static uint8_t *guard_copy(const uint8_t *src, size_t len)
{
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	size_t total = len + (size_t)trail_bytes;
	size_t pages = (total + page - 1) / page;
	if (!pages)
		pages = 1;
	uint8_t *m = mmap(NULL, (pages + 1) * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (m == MAP_FAILED) {
		perror("mmap");
		exit(2);
	}
	mprotect(m + pages * page, page, PROT_NONE);
	uint8_t *p = m + pages * page - total;
	if (len)
		memcpy(p, src, len);
	memset(p + len, 0xA5, (size_t)trail_bytes);
	return p;
}

/* string argument: literal text, or buf:NAME (raw bytes of a buffer) */
static char *resolve_str(const char *arg, int *len)
{
	const uint8_t *d;
	size_t n;
	if (!strncmp(arg, "buf:", 4)) {
		if (!find_buf(arg + 4, &d, &n)) {
			fprintf(stderr, "script: unknown buffer %s\n", arg);
			exit(2);
		}
	} else {
		d = (const uint8_t *)arg;
		n = strlen(arg);
	}
	char *c = malloc(n + 1);
	memcpy(c, d, n);
	c[n] = 0;
	*len = (int)n;
	return c;
}

/* ------------------------------------------------------------------------------------------ */
/* printing helpers                                                                            */

static void print_hexl(FILE *o, const uint8_t *d, size_t n, size_t limit)
{
	if (!n) {
		fputs("-", o);
		return;
	}
	if (n > limit) { /* long data: head, "...", last 16 bytes and the length */
		size_t head = limit > 128 ? 160 : 48;
		for (size_t i = 0; i < head; i++)
			fprintf(o, "%02x", d[i]);
		fprintf(o, "...");
		for (size_t i = n - 16; i < n; i++)
			fprintf(o, "%02x", d[i]);
		fprintf(o, "(%zu)", n);
		return;
	}
	for (size_t i = 0; i < n; i++)
		fprintf(o, "%02x", d[i]);
}

static void print_hexv(FILE *o, const uint8_t *d, size_t n)
{
	print_hexl(o, d, n, 512);
}

static void print_qs(FILE *o, const char *s, size_t n)
{
	fputc('"', o);
	for (size_t i = 0; i < n; i++) {
		unsigned char c = (unsigned char)s[i];
		if (c == '\\')
			fputs("\\\\", o);
		else if (c == '"')
			fputs("\\\"", o);
		else if (c == '\n')
			fputs("\\n", o);
		else if (c < 0x20 || c >= 0x7f)
			fprintf(o, "\\x%02x", c);
		else
			fputc(c, o);
	}
	fputc('"', o);
}

static void print_double(FILE *o, double v)
{
	uint64_t bits;
	memcpy(&bits, &v, 8);
	if (isnan(v))
		fprintf(o, "nan bits=%016llx", (unsigned long long)bits);
	else
		fprintf(o, "%.17g bits=%016llx", v, (unsigned long long)bits);
}

static const char *amf_type_name(int t)
{
	static const char *const n[] = {"NUMBER",    "BOOLEAN",  "STRING",    "OBJECT",       "MOVIECLIP",
					"NULL",      "UNDEFINED", "REFERENCE", "ECMA_ARRAY",   "OBJECT_END",
					"STRICT_ARRAY", "DATE",   "LONG_STRING", "UNSUPPORTED", "RECORDSET",
					"XML_DOC",   "TYPED_OBJECT", "AVMPLUS"};
	if (t >= 0 && t < 18)
		return n[t];
	if (t == 0xff)
		return "INVALID";
	return "UNKNOWN";
}

static void dump_obj(FILE *o, const AMFObject *obj, int depth)
{
	for (int i = 0; i < obj->o_num; i++) {
		const AMFObjectProperty *p = &obj->o_props[i];
		fprintf(o, "%*s[%d] name=", depth * 2 + 2, "", i);
		if (p->p_name.av_len > 0)
			print_qs(o, p->p_name.av_val, (size_t)p->p_name.av_len);
		else
			fputs("-", o);
		fprintf(o, " type=%s", amf_type_name(p->p_type));
		switch (p->p_type) {
		case AMF_NUMBER:
			fputs(" value=", o);
			print_double(o, p->p_vu.p_number);
			break;
		case AMF_BOOLEAN:
			fprintf(o, " value=%d", p->p_vu.p_number != 0);
			break;
		case AMF_STRING:
		case AMF_XML_DOC:
			fprintf(o, " len=%d value=", p->p_vu.p_aval.av_len);
			print_qs(o, p->p_vu.p_aval.av_val, p->p_vu.p_aval.av_len > 0 ? (size_t)p->p_vu.p_aval.av_len : 0);
			break;
		case AMF_DATE:
			fputs(" value=", o);
			print_double(o, p->p_vu.p_number);
			fprintf(o, " utc=%d", p->p_UTCoffset);
			break;
		case AMF_OBJECT:
		case AMF_ECMA_ARRAY:
		case AMF_STRICT_ARRAY:
			fprintf(o, " num=%d\n", p->p_vu.p_object.o_num);
			dump_obj(o, &p->p_vu.p_object, depth + 1);
			continue;
		default:
			break;
		}
		fputc('\n', o);
	}
}

/* ------------------------------------------------------------------------------------------ */
/* FLV tag decoder (the "decoded line" of the muxer commands)                                  */

static uint32_t be24(const uint8_t *p)
{
	return (uint32_t)p[0] << 16 | (uint32_t)p[1] << 8 | p[2];
}
static uint32_t be32(const uint8_t *p)
{
	return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static int32_t si24(const uint8_t *p)
{
	uint32_t v = be24(p);
	return (int32_t)(v & 0x800000 ? v | 0xff000000u : v);
}

static void print_fourcc(FILE *o, const uint8_t *p)
{
	print_qs(o, (const char *)p, 4);
}

/* Reads the AMF values of a script/metadata payload with the reference decoder and prints them. */
static void show_amf_payload(FILE *o, const uint8_t *d, size_t n)
{
	AMFObject obj;
	int r = AMF_Decode(&obj, (const char *)d, (int)n, FALSE);
	if (r < 0) /* the partial list is unspecified (spec R15) */
		fprintf(o, "    amf ret=%d\n", r);
	else {
		fprintf(o, "    amf ret=%d num=%d\n", r, obj.o_num);
		dump_obj(o, &obj, 2);
	}
	AMF_Reset(&obj);
}

static void show_video(FILE *o, const uint8_t *d, size_t n)
{
	if (n < 1) {
		fputs("    video empty\n", o);
		return;
	}
	size_t pos = 1;
	if (d[0] & 0x80) {
		int ft = (d[0] >> 4) & 7, pt = d[0] & 15;
		fprintf(o, "    video ex=1 frametype=%d ptype=%d", ft, pt);
		int mt = -1, inner = pt, track = -1;
		if (pt == 6) {
			if (n < pos + 1) {
				fputs(" truncated\n", o);
				return;
			}
			mt = d[pos] >> 4;
			inner = d[pos] & 15;
			pos++;
			fprintf(o, " multitrack=%d inner=%d", mt, inner);
		}
		if (n < pos + 4) {
			fputs(" truncated\n", o);
			return;
		}
		fputs(" fourcc=", o);
		print_fourcc(o, d + pos);
		bool h26x = !memcmp(d + pos, "avc1", 4) || !memcmp(d + pos, "hvc1", 4);
		pos += 4;
		if (pt == 6) {
			if (n < pos + 1) {
				fputs(" truncated\n", o);
				return;
			}
			track = d[pos++];
			fprintf(o, " track=%d", track);
		}
		if (h26x && inner == 1) {
			if (n < pos + 3) {
				fputs(" truncated\n", o);
				return;
			}
			fprintf(o, " cts=%d", si24(d + pos));
			pos += 3;
		}
		fputs(" payload=", o);
		print_hexl(o, d + pos, n - pos, 128);
		fputc('\n', o);
		if (inner == 4)
			show_amf_payload(o, d + pos, n - pos);
	} else {
		fprintf(o, "    video ex=0 frametype=%d codec=%d", d[0] >> 4, d[0] & 15);
		if (n < 5) {
			fputs(" truncated\n", o);
			return;
		}
		fprintf(o, " avcptype=%d cts=%d payload=", d[1], si24(d + 2));
		print_hexl(o, d + 5, n - 5, 128);
		fputc('\n', o);
	}
}

static void show_audio(FILE *o, const uint8_t *d, size_t n)
{
	if (n < 1) {
		fputs("    audio empty\n", o);
		return;
	}
	int fmt = d[0] >> 4;
	if (fmt == 9) {
		int pt = d[0] & 15, inner = pt, mt = -1;
		size_t pos = 1;
		fprintf(o, "    audio ex=1 ptype=%d", pt);
		if (pt == 5) {
			if (n < 2) {
				fputs(" truncated\n", o);
				return;
			}
			mt = d[1] >> 4;
			inner = d[1] & 15;
			pos = 2;
			fprintf(o, " multitrack=%d inner=%d", mt, inner);
		}
		if (n < pos + 4) {
			fputs(" truncated\n", o);
			return;
		}
		fputs(" fourcc=", o);
		print_fourcc(o, d + pos);
		pos += 4;
		if (pt == 5) {
			if (n < pos + 1) {
				fputs(" truncated\n", o);
				return;
			}
			fprintf(o, " track=%d", d[pos++]);
		}
		fputs(" payload=", o);
		print_hexl(o, d + pos, n - pos, 128);
		fputc('\n', o);
	} else {
		fprintf(o, "    audio ex=0 format=%d rate=%d size=%d type=%d", fmt, (d[0] >> 2) & 3, (d[0] >> 1) & 1,
			d[0] & 1);
		if (n < 2) {
			fputs(" truncated\n", o);
			return;
		}
		fprintf(o, " aacptype=%d payload=", d[1]);
		print_hexl(o, d + 2, n - 2, 128);
		fputc('\n', o);
	}
}

/* Prints "<cmd> size=N hex=..." followed by one decoded block per tag. */
static void show_flv(FILE *o, const char *cmd, const uint8_t *d, size_t n, bool has_header)
{
	fprintf(o, "%s size=%zu hex=", cmd, n);
	print_hexv(o, d, n);
	fputc('\n', o);
	size_t pos = 0;
	if (has_header) {
		if (n < 13) {
			fputs("  header truncated\n", o);
			return;
		}
		fprintf(o, "  flvheader sig=%c%c%c version=%d flags=0x%02x offset=%u prevsize0=%u\n", d[0], d[1], d[2],
			d[3], d[4], be32(d + 5), be32(d + 9));
		pos = 13;
	}
	while (pos < n) {
		if (n - pos < 11) {
			fprintf(o, "  trailing %zu bytes\n", n - pos);
			return;
		}
		const uint8_t *t = d + pos;
		uint32_t ds = be24(t + 1), ts = be24(t + 4) | (uint32_t)t[7] << 24;
		fprintf(o, "  tag type=%u datasize=%u ts=%u streamid=%u\n", t[0], ds, ts, be24(t + 8));
		if (ds > n - pos - 11) {
			fprintf(o, "  tag body truncated avail=%zu\n", n - pos - 11);
			return;
		}
		if (t[0] == 9)
			show_video(o, t + 11, ds);
		else if (t[0] == 8)
			show_audio(o, t + 11, ds);
		else if (t[0] == 18)
			show_amf_payload(o, t + 11, ds);
		size_t after = pos + 11 + ds;
		if (n - after < 4) {
			fputs("  prevsize missing\n", o);
			return;
		}
		fprintf(o, "  prevsize=%u expected=%u\n", be32(d + after), 11 + ds);
		pos = after + 4;
	}
}

/* ------------------------------------------------------------------------------------------ */
/* scripted state: packets, objects                                                            */

#define MAX_PKTS 32
static struct {
	char name[64];
	struct encoder_packet pkt;
} pkts[MAX_PKTS];
static int npkts;

static struct encoder_packet *find_pkt(const char *name)
{
	for (int i = 0; i < npkts; i++)
		if (!strcmp(pkts[i].name, name))
			return &pkts[i].pkt;
	fprintf(stderr, "script: unknown packet %s\n", name);
	exit(2);
}

#define MAX_OBJS 64
static struct {
	char name[64];
	AMFObject o;
} objs[MAX_OBJS];
static int nobjs;

static AMFObject *find_obj(const char *name)
{
	if (name[0] == '@')
		name++;
	for (int i = 0; i < nobjs; i++)
		if (!strcmp(objs[i].name, name))
			return &objs[i].o;
	fprintf(stderr, "script: unknown object %s\n", name);
	exit(2);
}

static int enc_cap = 4096;

static enum video_id_t parse_vcodec(const char *s)
{
	if (!strcmp(s, "none"))
		return CODEC_NONE;
	if (!strcmp(s, "h264"))
		return CODEC_H264;
	if (!strcmp(s, "av1"))
		return CODEC_AV1;
	if (!strcmp(s, "hevc"))
		return CODEC_HEVC;
	return (enum video_id_t)strtol(s, NULL, 0);
}

static enum audio_id_t parse_acodec(const char *s)
{
	if (!strcmp(s, "none"))
		return AUDIO_CODEC_NONE;
	if (!strcmp(s, "aac"))
		return AUDIO_CODEC_AAC;
	return (enum audio_id_t)strtol(s, NULL, 0);
}

static double parse_dbl(const char *s)
{
	if (!strncmp(s, "bits:", 5)) {
		uint64_t b = strtoull(s + 5, NULL, 16);
		double d;
		memcpy(&d, &b, 8);
		return d;
	}
	return strtod(s, NULL);
}

/* ------------------------------------------------------------------------------------------ */
/* RTMP_Log capture: only ERROR and worse are part of the contract                              */

static void rtmp_log_cb(int level, const char *fmt, va_list ap)
{
	if (level > RTMP_LOGWARNING)
		return;
	char msg[1024];
	vsnprintf(msg, sizeof(msg), fmt, ap);
	fprintf(out, "log %s %s\n", level <= RTMP_LOGERROR ? "error" : "warning", msg);
}

/* ------------------------------------------------------------------------------------------ */
/* actions (run in the forked child; output goes to `out`)                                      */

static uint8_t *const SENT_PTR = (uint8_t *)(uintptr_t)1;
#define SENT_SIZE ((size_t)0xdeadbeef)

static uint32_t be24(const uint8_t *p);
static void report_flv(const char *cmd, uint8_t *o, size_t n, bool has_header)
{
	if (o == SENT_PTR && n == SENT_SIZE) {
		fprintf(out, "%s untouched\n", cmd);
		return;
	}
	/* Reference defect D-6 (spec S-021): the 24-bit tag DataSize silently wraps when the payload is
	 * 16 MiB or larger. The output is then a corrupt FLV tag, so the runner models the failure:
	 * the command "crashes" deterministically instead of printing the corrupt bytes. */
	if (!has_header && n >= 15 && (n - 15) != be24(o + 1))
		abort();
	show_flv(out, cmd, o, n, has_header);
	bfree(o);
}

static void report_enc(const char *cmd, const char *base, const char *ret)
{
	if (!ret) {
		fprintf(out, "%s null\n", cmd);
		return;
	}
	fprintf(out, "%s len=%zu hex=", cmd, (size_t)(ret - base));
	print_hexv(out, (const uint8_t *)base, (size_t)(ret - base));
	fputc('\n', out);
}

/* encode buffer: cap bytes directly before an inaccessible page, pre-filled with 0xEE */
static char *enc_buffer(char **end)
{
	size_t page = (size_t)sysconf(_SC_PAGESIZE);
	size_t cap = (size_t)enc_cap;
	size_t pages = (cap + page - 1) / page + 1;
	uint8_t *m = mmap(NULL, (pages + 1) * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	mprotect(m + pages * page, page, PROT_NONE);
	uint8_t *p = m + pages * page - cap;
	memset(p, 0xEE, cap);
	*end = (char *)p + cap;
	return (char *)p;
}

static void do_layout(void)
{
	fprintf(out, "layout sizeof=%zu data=%zu size=%zu pts=%zu dts=%zu timebase_num=%zu timebase_den=%zu type=%zu "
		     "keyframe=%zu priority=%zu drop_priority=%zu\n",
		sizeof(struct encoder_packet), offsetof(struct encoder_packet, data),
		offsetof(struct encoder_packet, size), offsetof(struct encoder_packet, pts),
		offsetof(struct encoder_packet, dts), offsetof(struct encoder_packet, timebase_num),
		offsetof(struct encoder_packet, timebase_den), offsetof(struct encoder_packet, type),
		offsetof(struct encoder_packet, keyframe), offsetof(struct encoder_packet, priority),
		offsetof(struct encoder_packet, drop_priority));
}

static void do_action(const struct script_line *sl, const uint8_t *p, size_t len, bool data_null)
{
	const char *c = sl->argv[0];
	uint8_t *o = SENT_PTR;
	size_t n = SENT_SIZE;
	(void)data_null;
	RTMP_LogSetCallback(rtmp_log_cb);

	if ((!strcmp(c, "meta") || !strcmp(c, "fileinfo")) && strlen(flv_fake.version) > 3699) {
		/* Reference defect D-5 (spec S-021): the 4096-byte property buffer of flv_meta_data is
		 * overrun (version of 3700 characters: one byte written past the buffer) or a NULL
		 * pointer is dereferenced (3701+). Both are undefined behaviour, so the runner models
		 * the failure instead of executing it: the command "crashes" deterministically. */
		abort();
	}
	if (!strcmp(c, "meta")) {
		script_need(sl, 2);
		bool hdr = atoi(sl->argv[1]) != 0;
		flv_meta_data(flv_fake_output(), &o, &n, hdr);
		report_flv(c, o, n, hdr);
	} else if (!strcmp(c, "fileinfo")) {
		script_need(sl, 3);
		bool hdr = sl->argc > 3 ? atoi(sl->argv[3]) != 0 : true;
		flv_meta_data(flv_fake_output(), &o, &n, hdr);
		FILE *f = tmpfile();
		fwrite(o, 1, n, f);
		write_file_info(f, strtoll(sl->argv[1], NULL, 0), strtoll(sl->argv[2], NULL, 0));
		fflush(f);
		fseek(f, 0, SEEK_END);
		long sz = ftell(f);
		uint8_t *buf = malloc((size_t)sz);
		fseek(f, 0, SEEK_SET);
		if (fread(buf, 1, (size_t)sz, f) != (size_t)sz)
			_exit(3);
		fclose(f);
		fprintf(out, "fileinfo before=%zu after=%ld\n", n, sz);
		show_flv(out, "fileinfo", buf, (size_t)sz, hdr);
		free(buf);
		bfree(o);
	} else if (!strcmp(c, "flv_mux")) {
		script_need(sl, 4);
		flv_packet_mux(find_pkt(sl->argv[1]), (int32_t)strtol(sl->argv[2], NULL, 0), &o, &n,
			       atoi(sl->argv[3]) != 0);
		if (o == SENT_PTR && n == SENT_SIZE) {
			fprintf(out, "%s untouched\n", c);
		} else if (!n) {
			fprintf(out, "%s empty\n", c);
			bfree(o);
		} else {
			report_flv(c, o, n, false);
		}
	} else if (!strcmp(c, "ex_start") || !strcmp(c, "ex_end")) {
		script_need(sl, 4);
		struct encoder_packet *pk = find_pkt(sl->argv[1]);
		enum video_id_t cid = parse_vcodec(sl->argv[2]);
		size_t idx = (size_t)strtoull(sl->argv[3], NULL, 0);
		if (c[3] == 's')
			flv_packet_start(pk, cid, &o, &n, idx);
		else
			flv_packet_end(pk, cid, &o, &n, idx);
		report_flv(c, o, n, false);
	} else if (!strcmp(c, "ex_frames")) {
		script_need(sl, 5);
		flv_packet_frames(find_pkt(sl->argv[1]), parse_vcodec(sl->argv[2]), (int32_t)strtol(sl->argv[3], NULL, 0),
				  &o, &n, (size_t)strtoull(sl->argv[4], NULL, 0));
		report_flv(c, o, n, false);
	} else if (!strcmp(c, "ex_raw")) {
		script_need(sl, 6);
		flv_packet_ex(find_pkt(sl->argv[1]), parse_vcodec(sl->argv[2]), (int32_t)strtol(sl->argv[3], NULL, 0), &o,
			      &n, atoi(sl->argv[4]), (size_t)strtoull(sl->argv[5], NULL, 0));
		report_flv(c, o, n, false);
	} else if (!strcmp(c, "ex_metadata")) {
		script_need(sl, 9);
		flv_packet_metadata(parse_vcodec(sl->argv[1]), &o, &n, atoi(sl->argv[3]),
				    (uint8_t)strtol(sl->argv[4], NULL, 0), atoi(sl->argv[5]), atoi(sl->argv[6]),
				    atoi(sl->argv[7]), atoi(sl->argv[8]), (size_t)strtoull(sl->argv[2], NULL, 0));
		report_flv(c, o, n, false);
	} else if (!strcmp(c, "ax_start")) {
		script_need(sl, 4);
		flv_packet_audio_start(find_pkt(sl->argv[1]), parse_acodec(sl->argv[2]), &o, &n,
				       (size_t)strtoull(sl->argv[3], NULL, 0));
		report_flv(c, o, n, false);
	} else if (!strcmp(c, "ax_frames")) {
		script_need(sl, 5);
		flv_packet_audio_frames(find_pkt(sl->argv[1]), parse_acodec(sl->argv[2]),
					(int32_t)strtol(sl->argv[3], NULL, 0), &o, &n,
					(size_t)strtoull(sl->argv[4], NULL, 0));
		report_flv(c, o, n, false);
	} else if (!strcmp(c, "ax_raw")) {
		script_need(sl, 6);
		flv_packet_audio_ex(find_pkt(sl->argv[1]), parse_acodec(sl->argv[2]), (int32_t)strtol(sl->argv[3], NULL, 0),
				    &o, &n, atoi(sl->argv[4]), (size_t)strtoull(sl->argv[5], NULL, 0));
		report_flv(c, o, n, false);
	} else if (!strcmp(c, "ms")) {
		script_need(sl, 3);
		fprintf(out, "ms %d\n", get_ms_time(find_pkt(sl->argv[1]), strtoll(sl->argv[2], NULL, 0)));
	} else if (!strcmp(c, "to_video")) {
		script_need(sl, 2);
		fprintf(out, "to_video %d\n", (int)to_video_type(sl->argv[1]));
	} else if (!strcmp(c, "to_audio")) {
		script_need(sl, 2);
		fprintf(out, "to_audio %d\n", (int)to_audio_type(sl->argv[1]));
	} else if (!strcmp(c, "layout")) {
		do_layout();

		/* ---- AMF encoders ---- */
	} else if (!strncmp(c, "enc_", 4)) {
		char *end;
		char *base = enc_buffer(&end);
		char *r = NULL;
		AVal a, b;
		char *s1 = NULL, *s2 = NULL;
		if (!strcmp(c, "enc_int16")) {
			script_need(sl, 2);
			r = AMF_EncodeInt16(base, end, (short)strtol(sl->argv[1], NULL, 0));
		} else if (!strcmp(c, "enc_int24")) {
			script_need(sl, 2);
			r = AMF_EncodeInt24(base, end, (int)strtol(sl->argv[1], NULL, 0));
		} else if (!strcmp(c, "enc_int32")) {
			script_need(sl, 2);
			r = AMF_EncodeInt32(base, end, (int)strtoll(sl->argv[1], NULL, 0));
		} else if (!strcmp(c, "enc_number")) {
			script_need(sl, 2);
			r = AMF_EncodeNumber(base, end, parse_dbl(sl->argv[1]));
		} else if (!strcmp(c, "enc_bool")) {
			script_need(sl, 2);
			r = AMF_EncodeBoolean(base, end, (int)strtol(sl->argv[1], NULL, 0));
		} else if (!strcmp(c, "enc_string")) {
			script_need(sl, 2);
			s1 = resolve_str(sl->argv[1], &a.av_len);
			a.av_val = s1;
			r = AMF_EncodeString(base, end, &a);
		} else if (!strcmp(c, "enc_named_string")) {
			script_need(sl, 3);
			s1 = resolve_str(sl->argv[1], &a.av_len);
			a.av_val = s1;
			s2 = resolve_str(sl->argv[2], &b.av_len);
			b.av_val = s2;
			r = AMF_EncodeNamedString(base, end, &a, &b);
		} else if (!strcmp(c, "enc_named_number") || !strcmp(c, "enc_named_bool")) {
			script_need(sl, 3);
			s1 = resolve_str(sl->argv[1], &a.av_len);
			a.av_val = s1;
			if (c[10] == 'n')
				r = AMF_EncodeNamedNumber(base, end, &a, parse_dbl(sl->argv[2]));
			else
				r = AMF_EncodeNamedBoolean(base, end, &a, (int)strtol(sl->argv[2], NULL, 0));
		} else if (!strcmp(c, "enc_obj")) {
			script_need(sl, 2);
			AMFObject *ob = find_obj(sl->argv[1]);
			const char *kind = sl->argc > 2 ? sl->argv[2] : "object";
			if (!strcmp(kind, "ecma"))
				r = AMF_EncodeEcmaArray(ob, base, end);
			else if (!strcmp(kind, "strict"))
				r = AMF_EncodeArray(ob, base, end);
			else
				r = AMF_Encode(ob, base, end);
		} else if (!strcmp(c, "enc_prop")) {
			script_need(sl, 3);
			AMFObject *ob = find_obj(sl->argv[1]);
			r = AMFProp_Encode(&ob->o_props[atoi(sl->argv[2])], base, end);
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl->lineno, c);
			exit(2);
		}
		report_enc(c, base, r);
		free(s1);
		free(s2);

		/* ---- AMF decoders ---- */
	} else if (!strcmp(c, "dec_int16")) {
		fprintf(out, "%s %u\n", c, AMF_DecodeInt16((const char *)p));
	} else if (!strcmp(c, "dec_int24")) {
		fprintf(out, "%s %u\n", c, AMF_DecodeInt24((const char *)p));
	} else if (!strcmp(c, "dec_int32")) {
		fprintf(out, "%s %u\n", c, AMF_DecodeInt32((const char *)p));
	} else if (!strcmp(c, "dec_bool")) {
		fprintf(out, "%s %d\n", c, AMF_DecodeBoolean((const char *)p));
	} else if (!strcmp(c, "dec_number")) {
		fprintf(out, "%s ", c);
		print_double(out, AMF_DecodeNumber((const char *)p));
		fputc('\n', out);
	} else if (!strcmp(c, "dec_string") || !strcmp(c, "dec_longstring")) {
		AVal v = {(char *)1, -7};
		if (c[4] == 's')
			AMF_DecodeString((const char *)p, &v);
		else
			AMF_DecodeLongString((const char *)p, &v);
		fprintf(out, "%s len=%d val=", c, v.av_len);
		if (!v.av_val)
			fputs("null", out);
		else
			print_qs(out, v.av_val, (size_t)v.av_len);
		fputc('\n', out);
	} else if (!strcmp(c, "decode") || !strcmp(c, "decode_array")) {
		AMFObject obj = {-1, (void *)1};
		int r;
		if (c[6] == 0) {
			int nm = sl->argc > 2 ? atoi(sl->argv[2]) : 1;
			r = AMF_Decode(&obj, (const char *)p, (int)len, nm);
		} else {
			script_need(sl, 3);
			int nm = sl->argc > 3 ? atoi(sl->argv[3]) : 0;
			r = AMF_DecodeArray(&obj, (const char *)p, (int)len, atoi(sl->argv[2]), nm);
		}
		/* on failure the object content is unspecified: only the return value is printed */
		if (r < 0)
			fprintf(out, "%s ret=%d\n", c, r);
		else {
			fprintf(out, "%s ret=%d num=%d\n", c, r, obj.o_num);
			dump_obj(out, &obj, 0);
		}
		AMF_Reset(&obj);
	} else if (!strcmp(c, "prop_decode")) {
		AMFObjectProperty pr;
		memset(&pr, 0, sizeof(pr));
		pr.p_type = AMF_INVALID;
		int nm = sl->argc > 2 ? atoi(sl->argv[2]) : 0;
		int r = AMFProp_Decode(&pr, (const char *)p, (int)len, nm);
		fprintf(out, "%s ret=%d\n", c, r);
		AMFObject tmp = {1, &pr};
		if (r >= 0)
			dump_obj(out, &tmp, 0);
		AMFProp_Reset(&pr);
	} else if (!strcmp(c, "getprop")) {
		script_need(sl, 3);
		AMFObject obj;
		int r = AMF_Decode(&obj, (const char *)p, (int)len, TRUE);
		if (r < 0) /* the partial list is unspecified (spec R15): look up in an empty object */
			AMF_Reset(&obj);
		AVal nm = {0, 0};
		int idx = -1;
		if (isdigit((unsigned char)sl->argv[2][0]))
			idx = atoi(sl->argv[2]);
		else {
			nm.av_val = sl->argv[2];
			nm.av_len = (int)strlen(sl->argv[2]);
		}
		AMFObjectProperty *pr = AMF_GetProp(&obj, &nm, idx);
		fprintf(out, "getprop decode=%d count=%d valid=%d type=%s\n", r, AMF_CountProp(&obj), AMFProp_IsValid(pr),
			amf_type_name(AMFProp_GetType(pr)));
		{
			AVal pn, ps;
			AMFObject po;
			AMFProp_GetName(pr, &pn);
			AMFProp_GetString(pr, &ps);
			AMFProp_GetObject(pr, &po);
			fprintf(out, "  name=");
			print_qs(out, pn.av_val, pn.av_len > 0 ? (size_t)pn.av_len : 0);
			fprintf(out, " number=");
			if (pr->p_type == AMF_NUMBER || pr->p_type == AMF_BOOLEAN || pr->p_type == AMF_INVALID)
				print_double(out, AMFProp_GetNumber(pr));
			else
				fputs("n/a", out);
			fprintf(out, " boolean=%d string=", (pr->p_type == AMF_NUMBER || pr->p_type == AMF_BOOLEAN ||
								pr->p_type == AMF_INVALID)
								? AMFProp_GetBoolean(pr)
								: -1);
			print_qs(out, ps.av_val, ps.av_len > 0 ? (size_t)ps.av_len : 0);
			fprintf(out, " object_num=%d\n", po.o_num);
		}
		AMF_Reset(&obj);

		/* ---- codec configuration records ---- */
	} else if (!strcmp(c, "hevc_header") || !strcmp(c, "av1_header")) {
		uint8_t *h = SENT_PTR;
		size_t r = c[0] == 'h' ? obs_parse_hevc_header(&h, p, len) : obs_parse_av1_header(&h, p, len);
		fprintf(out, "%s ret=%zu", c, r);
		if (h == SENT_PTR)
			fputs(" header=unset\n", out);
		else {
			fputs(" header=", out);
			print_hexv(out, h, r);
			fputc('\n', out);
			bfree(h);
		}
	} else if (!strcmp(c, "av1_packet")) {
		struct encoder_packet src, dst;
		memset(&src, 0, sizeof(src));
		memset(&dst, 0x5A, sizeof(dst));
		src.data = (uint8_t *)p;
		src.size = len;
		src.priority = sl->argc > 2 ? atoi(sl->argv[2]) : 0;
		src.keyframe = sl->argc > 3 ? atoi(sl->argv[3]) != 0 : false;
		src.pts = 11;
		src.dts = 12;
		obs_parse_av1_packet(&dst, &src);
		fprintf(out, "%s size=%zu keyframe=%d priority=%d drop_priority=%d pts=%lld dts=%lld fresh=%d data=", c,
			dst.size, dst.keyframe, dst.priority, dst.drop_priority, (long long)dst.pts, (long long)dst.dts,
			dst.data != src.data);
		print_hexv(out, dst.data, dst.size);
		fputc('\n', out);
		bfree((long *)dst.data - 1);
	} else if (!strcmp(c, "rbsp")) {
		script_need(sl, 3);
		uint8_t *dst = malloc(len + 64 + 1);
		uint32_t dl = 0;
		ff_nal_unit_extract_rbsp(dst, p, (int)len, &dl, atoi(sl->argv[2]));
		fprintf(out, "rbsp len=%u data=", dl);
		print_hexv(out, dst, dl);
		bool z = true;
		for (int i = 0; i < 64; i++)
			z &= dst[dl + i] == 0;
		fprintf(out, " tail64zero=%d\n", z);
		free(dst);
	} else {
		fprintf(stderr, "script: line %d: unknown command '%s'\n", sl->lineno, c);
		exit(2);
	}
}

static bool is_data_action(const char *c)
{
	static const char *const names[] = {"dec_int16", "dec_int24",   "dec_int32",  "dec_number", "dec_bool",
					    "dec_string", "dec_longstring", "decode",   "decode_array", "prop_decode",
					    "getprop",   "hevc_header", "av1_header", "av1_packet", "rbsp"};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(c, names[i]))
			return true;
	return false;
}

static bool is_action(const char *c)
{
	static const char *const names[] = {"meta",     "fileinfo", "flv_mux",  "ex_start", "ex_frames", "ex_end",
					    "ex_raw",   "ex_metadata", "ax_start", "ax_frames", "ax_raw",   "ms",
					    "to_video", "to_audio", "layout"};
	if (is_data_action(c) || !strncmp(c, "enc_", 4))
		return true;
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(c, names[i]))
			return true;
	return false;
}

/* Runs one action in a child process and relays its output; "<cmd> crash" if it dies. */
static int run_forked(const struct script_line *sl, const uint8_t *p, size_t len, bool is_null)
{
	fflush(stdout);
	int fds[2];
	if (pipe(fds)) {
		perror("pipe");
		return 2;
	}
	pid_t pid = fork();
	if (pid == 0) {
		close(fds[0]);
		if (!getenv("FLV_KEEP_STDERR") && !freopen("/dev/null", "w", stderr)) /* debugging aid */
			_exit(3);
		char *rbuf = NULL;
		size_t rlen = 0;
		out = open_memstream(&rbuf, &rlen);
		do_action(sl, p, len, is_null);
		fclose(out);
		for (size_t off = 0; off < rlen;) {
			ssize_t w = write(fds[1], rbuf + off, rlen - off);
			if (w <= 0)
				_exit(3);
			off += (size_t)w;
		}
		_exit(0);
	}
	close(fds[1]);
	char *rbuf = malloc(1);
	size_t rlen = 0;
	for (;;) {
		char tmp[65536];
		ssize_t r = read(fds[0], tmp, sizeof(tmp));
		if (r <= 0)
			break;
		rbuf = realloc(rbuf, rlen + (size_t)r + 1);
		memcpy(rbuf + rlen, tmp, (size_t)r);
		rlen += (size_t)r;
	}
	close(fds[0]);
	int status = 0;
	waitpid(pid, &status, 0);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		fwrite(rbuf, 1, rlen, stdout);
	else if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
		fprintf(stderr, "line %d: script error\n", sl->lineno);
		return 2;
	} else
		printf("%s crash\n", sl->argv[0]);
	free(rbuf);
	return 0;
}

/* bcrash() (e.g. bmalloc(0)) would call exit(0); make it a real crash so that the forked child dies */
static void crash_to_abort(const char *format, va_list args, void *param)
{
	(void)format, (void)args, (void)param;
	abort();
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	base_set_crash_handler(crash_to_abort, NULL);
	struct script_line sl;
	int lineno = 0;

	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		if (!strcmp(c, "buf")) {
			script_need(&sl, 2);
			size_t total = 0;
			uint8_t *acc = malloc(1);
			for (int i = 2; i < sl.argc; i++) {
				size_t n;
				uint8_t *h = script_parse_hex(sl.argv[i], &n);
				acc = realloc(acc, total + n + 1);
				memcpy(acc + total, h, n);
				total += n;
				free(h);
			}
			set_buf(sl.argv[1], acc, total);
		} else if (!strcmp(c, "cat")) {
			script_need(&sl, 2);
			size_t total = 0;
			uint8_t *acc = malloc(1);
			for (int i = 2; i < sl.argc; i++) {
				size_t n;
				bool nul;
				uint8_t *h = resolve(sl.argv[i], &n, &nul);
				acc = realloc(acc, total + n + 1);
				if (n)
					memcpy(acc + total, h, n);
				total += n;
				free(h);
			}
			set_buf(sl.argv[1], acc, total);
		} else if (!strcmp(c, "rep")) {
			script_need(&sl, 4);
			size_t cnt = (size_t)strtoull(sl.argv[2], NULL, 0), n;
			uint8_t *h = script_parse_hex(sl.argv[3], &n);
			uint8_t *acc = malloc(cnt * n + 1);
			for (size_t i = 0; i < cnt; i++)
				memcpy(acc + i * n, h, n);
			free(h);
			set_buf(sl.argv[1], acc, cnt * n);
		} else if (!strcmp(c, "trail")) {
			script_need(&sl, 2);
			trail_bytes = atoi(sl.argv[1]);
		} else if (!strcmp(c, "echo")) {
			script_need(&sl, 2);
			printf("echo %s\n", sl.argv[1]);
		} else if (!strcmp(c, "cap")) {
			script_need(&sl, 2);
			enc_cap = atoi(sl.argv[1]);
		} else if (!strcmp(c, "cfg")) {
			script_need(&sl, 3);
			const char *k = sl.argv[1], *v = sl.argv[2];
			if (!strcmp(k, "vcodec"))
				snprintf(flv_fake.vcodec, sizeof(flv_fake.vcodec), "%s", v);
			else if (!strcmp(k, "width"))
				flv_fake.width = (uint32_t)strtoul(v, NULL, 0);
			else if (!strcmp(k, "height"))
				flv_fake.height = (uint32_t)strtoul(v, NULL, 0);
			else if (!strcmp(k, "vbitrate"))
				flv_fake.vbitrate = parse_dbl(v);
			else if (!strcmp(k, "fps"))
				flv_fake.fps = parse_dbl(v);
			else if (!strcmp(k, "abitrate"))
				flv_fake.abitrate = parse_dbl(v);
			else if (!strcmp(k, "arate"))
				flv_fake.arate = (uint32_t)strtoul(v, NULL, 0);
			else if (!strcmp(k, "channels"))
				flv_fake.channels = (size_t)strtoul(v, NULL, 0);
			else if (!strcmp(k, "version"))
				snprintf(flv_fake.version, sizeof(flv_fake.version), "%s", v);
			else if (!strcmp(k, "version_rep")) { /* version = COUNT x 'v' */
				size_t cnt = (size_t)strtoul(v, NULL, 0);
				if (cnt >= sizeof(flv_fake.version))
					cnt = sizeof(flv_fake.version) - 1;
				memset(flv_fake.version, 'v', cnt);
				flv_fake.version[cnt] = 0;
			} else {
				fprintf(stderr, "script: line %d: unknown cfg key '%s'\n", lineno, k);
				return 2;
			}
		} else if (!strcmp(c, "pkt")) {
			script_need(&sl, 9);
			if (npkts >= MAX_PKTS)
				return 2;
			snprintf(pkts[npkts].name, sizeof(pkts[npkts].name), "%s", sl.argv[1]);
			struct encoder_packet *pk = &pkts[npkts].pkt;
			memset(pk, 0, sizeof(*pk));
			pk->type = !strcmp(sl.argv[2], "video") ? OBS_ENCODER_VIDEO : OBS_ENCODER_AUDIO;
			pk->pts = strtoll(sl.argv[3], NULL, 0);
			pk->dts = strtoll(sl.argv[4], NULL, 0);
			pk->timebase_num = (int32_t)strtol(sl.argv[5], NULL, 0);
			pk->timebase_den = (int32_t)strtol(sl.argv[6], NULL, 0);
			pk->keyframe = atoi(sl.argv[7]) != 0;
			bool nul;
			size_t n;
			pk->data = resolve(sl.argv[8], &n, &nul);
			pk->size = n;
			if (nul) {
				free(pk->data);
				pk->data = NULL;
			}
			/* a later pkt with an existing name replaces it */
			int found = -1;
			for (int i = 0; i < npkts; i++)
				if (!strcmp(pkts[i].name, sl.argv[1]))
					found = i;
			if (found >= 0) {
				pkts[found].pkt = *pk;
			} else
				npkts++;
		} else if (!strcmp(c, "obj")) {
			script_need(&sl, 2);
			snprintf(objs[nobjs].name, sizeof(objs[nobjs].name), "%s", sl.argv[1]);
			objs[nobjs].o.o_num = 0;
			objs[nobjs].o.o_props = NULL;
			nobjs++;
		} else if (!strcmp(c, "add")) {
			script_need(&sl, 4);
			AMFObject *ob = find_obj(sl.argv[1]);
			AMFObjectProperty pr;
			memset(&pr, 0, sizeof(pr));
			if (strcmp(sl.argv[2], "-")) {
				int l;
				pr.p_name.av_val = resolve_str(sl.argv[2], &l);
				pr.p_name.av_len = l;
			}
			const char *k = sl.argv[3];
			if (!strcmp(k, "number")) {
				pr.p_type = AMF_NUMBER;
				pr.p_vu.p_number = parse_dbl(sl.argv[4]);
			} else if (!strcmp(k, "bool")) {
				pr.p_type = AMF_BOOLEAN;
				pr.p_vu.p_number = parse_dbl(sl.argv[4]);
			} else if (!strcmp(k, "string")) {
				int l;
				pr.p_type = AMF_STRING;
				pr.p_vu.p_aval.av_val = resolve_str(sl.argv[4], &l);
				pr.p_vu.p_aval.av_len = l;
			} else if (!strcmp(k, "null")) {
				pr.p_type = AMF_NULL;
			} else if (!strcmp(k, "object") || !strcmp(k, "ecma") || !strcmp(k, "strict")) {
				pr.p_type = k[0] == 'o' ? AMF_OBJECT : k[0] == 'e' ? AMF_ECMA_ARRAY : AMF_STRICT_ARRAY;
				pr.p_vu.p_object = *find_obj(sl.argv[4]);
			} else if (!strcmp(k, "date")) {
				pr.p_type = AMF_DATE;
				pr.p_vu.p_number = parse_dbl(sl.argv[4]);
				pr.p_UTCoffset = (int16_t)(sl.argc > 5 ? atoi(sl.argv[5]) : 0);
			} else if (!strcmp(k, "invalid")) {
				pr.p_type = AMF_INVALID;
			} else if (!strcmp(k, "type")) {
				pr.p_type = (AMFDataType)strtol(sl.argv[4], NULL, 0);
				pr.p_vu.p_number = sl.argc > 5 ? parse_dbl(sl.argv[5]) : 0;
			} else {
				fprintf(stderr, "script: line %d: unknown kind '%s'\n", lineno, k);
				return 2;
			}
			AMF_AddProp(ob, &pr);
		} else if (is_action(c)) {
			size_t len = 0;
			bool is_null = false;
			const uint8_t *g = NULL;
			if (is_data_action(c) && strcmp(c, "layout")) {
				script_need(&sl, 2);
				uint8_t *raw = resolve(sl.argv[1], &len, &is_null);
				g = guard_copy(raw, len);
				free(raw);
			}
			int rc = run_forked(&sl, g, len, is_null);
			if (rc)
				return rc;
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", lineno, c);
			return 2;
		}
	}
	fclose(f);
	return 0;
}
