/*
 * Conformance runner for serializers, the bit reader and text lookup (spec S-010).
 * Command list: see "serial runner" in test/conformance/README.md.
 *
 * Up to 4 serializer "slots" (command `slot N`); every serializer command acts on the
 * current slot. Files live in $CONFORMANCE_TMPDIR and are addressed by plain names.
 */
#include <inttypes.h>
#include <unistd.h>

#include "util/array-serializer.h"
#include "util/file-serializer.h"
#include "util/buffered-file-serializer.h"
#include "util/bitstream.h"
#include "util/text-lookup.h"
#include "harness.h"

/* not declared in bitstream.h but linkable */
uint8_t bitstream_reader_read_bit(struct bitstream_reader *r);

enum kind { K_NONE_OPEN, K_NULL, K_EMPTY, K_ARRAY, K_FIN, K_FOUT, K_FSAFE, K_BUF, K_CUSTOM };

/* "custom" serializer: a scripted double with a configurable subset of callbacks.
 *  write: appends to a log, returns the size.      read: returns min(size, 3) bytes 01 02 03 ... (short reads)
 *  seek: returns offset * 10 + mode.               get_pos: returns 777. */
struct cust {
	uint8_t *log;
	size_t n, cap;
	uint8_t next;
};

static size_t cust_write(void *p, const void *d, size_t n)
{
	struct cust *c = p;
	if (c->n + n > c->cap) {
		c->cap = (c->n + n) * 2;
		c->log = realloc(c->log, c->cap);
	}
	memcpy(c->log + c->n, d, n);
	c->n += n;
	return n;
}
static size_t cust_read(void *p, void *d, size_t n)
{
	struct cust *c = p;
	size_t k = n < 3 ? n : 3;
	for (size_t i = 0; i < k; i++)
		((uint8_t *)d)[i] = ++c->next;
	return k;
}
static int64_t cust_seek(void *p, int64_t off, enum serialize_seek_type t)
{
	(void)p;
	return off * 10 + (int)t;
}
static int64_t cust_pos(void *p)
{
	(void)p;
	return 777;
}

struct slot {
	enum kind kind;
	struct serializer s;
	struct serializer *sp;
	struct array_output_data ad;
	struct cust cu;
};

static struct slot slots[4];
static struct slot *cur = &slots[0];
static lookup_t *lookups[4];
static struct bitstream_reader br;
static uint8_t *br_buf;
static const char *tmpdir;

static void path_of(char *out, size_t n, const char *name)
{
	snprintf(out, n, "%s/serial-%s", tmpdir, name);
}

static void close_slot(struct slot *sl)
{
	switch (sl->kind) {
	case K_ARRAY:
		array_output_serializer_free(&sl->ad);
		break;
	case K_FIN:
		file_input_serializer_free(&sl->s);
		break;
	case K_FOUT:
	case K_FSAFE:
		file_output_serializer_free(&sl->s);
		break;
	case K_BUF:
		buffered_file_serializer_free(&sl->s);
		break;
	case K_CUSTOM:
		free(sl->cu.log);
		break;
	default:
		break;
	}
	memset(sl, 0, sizeof(*sl));
}

static const char *seek_name(enum serialize_seek_type t)
{
	return t == SERIALIZE_SEEK_START ? "start" : t == SERIALIZE_SEEK_CURRENT ? "cur" : "end";
}

static uint64_t parse_u64(const char *s)
{
	return strtoull(s, NULL, 0);
}

/* float/double argument: decimal text, or "bits:0x..." for an exact bit pattern */
static float parse_f32(const char *s)
{
	if (!strncmp(s, "bits:", 5)) {
		uint32_t u = (uint32_t)strtoull(s + 5, NULL, 0);
		float f;
		memcpy(&f, &u, 4);
		return f;
	}
	return strtof(s, NULL);
}
static double parse_f64(const char *s)
{
	if (!strncmp(s, "bits:", 5)) {
		uint64_t u = strtoull(s + 5, NULL, 0);
		double d;
		memcpy(&d, &u, 8);
		return d;
	}
	return strtod(s, NULL);
}

static void put_file(const char *name, const void *data, size_t n)
{
	char p[1024];
	path_of(p, sizeof p, name);
	FILE *f = fopen(p, "wb");
	if (!f) {
		perror(p);
		exit(2);
	}
	if (n)
		fwrite(data, 1, n, f);
	fclose(f);
}

static uint8_t *get_file(const char *name, size_t *n)
{
	char p[1024];
	path_of(p, sizeof p, name);
	FILE *f = fopen(p, "rb");
	if (!f)
		return NULL;
	size_t cap = 4096, len = 0;
	uint8_t *b = malloc(cap);
	size_t r;
	while ((r = fread(b + len, 1, cap - len, f)) > 0) {
		len += r;
		if (len == cap) {
			cap *= 2;
			b = realloc(b, cap);
		}
	}
	fclose(f);
	*n = len;
	return b;
}

static void *pattern(size_t n, size_t start)
{
	uint8_t *b = malloc(n ? n : 1);
	for (size_t i = 0; i < n; i++)
		b[i] = (uint8_t)((start + i) * 31 + 7);
	return b;
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	struct script_line sl;
	int lineno = 0;
	tmpdir = getenv("CONFORMANCE_TMPDIR");
	if (!tmpdir)
		tmpdir = ".";

	while (script_next(f, &sl, &lineno)) {
		const char *c = sl.argv[0];
		struct serializer *s = cur->sp;

		if (!strcmp(c, "slot")) {
			script_need(&sl, 2);
			cur = &slots[strtoul(sl.argv[1], NULL, 10) & 3];
		} else if (!strcmp(c, "open")) {
			script_need(&sl, 2);
			const char *k = sl.argv[1];
			char p[1024];
			bool ok = true;
			close_slot(cur);
			cur->sp = &cur->s;
			if (!strcmp(k, "null")) {
				cur->kind = K_NULL;
				cur->sp = NULL;
			} else if (!strcmp(k, "empty")) {
				cur->kind = K_EMPTY;
			} else if (!strcmp(k, "array")) {
				cur->kind = K_ARRAY;
				array_output_serializer_init(&cur->s, &cur->ad);
			} else if (!strcmp(k, "custom")) {
				script_need(&sl, 3);
				cur->kind = K_CUSTOM;
				cur->s.data = &cur->cu;
				for (const char *q = sl.argv[2]; *q; q++) {
					if (*q == 'r')
						cur->s.read = cust_read;
					if (*q == 'w')
						cur->s.write = cust_write;
					if (*q == 's')
						cur->s.seek = cust_seek;
					if (*q == 'p')
						cur->s.get_pos = cust_pos;
				}
			} else if (!strcmp(k, "fin")) {
				script_need(&sl, 3);
				path_of(p, sizeof p, sl.argv[2]);
				ok = file_input_serializer_init(&cur->s, p);
				cur->kind = ok ? K_FIN : K_NONE_OPEN;
			} else if (!strcmp(k, "fout")) {
				script_need(&sl, 3);
				path_of(p, sizeof p, sl.argv[2]);
				ok = file_output_serializer_init(&cur->s, p);
				cur->kind = ok ? K_FOUT : K_NONE_OPEN;
			} else if (!strcmp(k, "fsafe")) {
				script_need(&sl, 4);
				path_of(p, sizeof p, sl.argv[2]);
				ok = file_output_serializer_init_safe(&cur->s, p,
								      strcmp(sl.argv[3], "@null") ? sl.argv[3] : NULL);
				cur->kind = ok ? K_FSAFE : K_NONE_OPEN;
			} else if (!strcmp(k, "buf")) {
				script_need(&sl, 3);
				path_of(p, sizeof p, sl.argv[2]);
				if (sl.argc >= 5)
					ok = buffered_file_serializer_init(&cur->s, p, (size_t)parse_u64(sl.argv[3]),
									   (size_t)parse_u64(sl.argv[4]));
				else
					ok = buffered_file_serializer_init_defaults(&cur->s, p);
				cur->kind = ok ? K_BUF : K_NONE_OPEN;
			} else {
				fprintf(stderr, "script: line %d: unknown kind %s\n", sl.lineno, k);
				return 2;
			}
			printf("open %s %s\n", k, ok ? "ok" : "fail");
			if (!ok)
				memset(cur, 0, sizeof(*cur));
		} else if (!strcmp(c, "close")) {
			close_slot(cur);
		} else if (!strcmp(c, "w8")) {
			script_need(&sl, 2);
			s_w8(s, (uint8_t)parse_u64(sl.argv[1]));
		} else if (!strcmp(c, "wl16")) {
			script_need(&sl, 2);
			s_wl16(s, (uint16_t)parse_u64(sl.argv[1]));
		} else if (!strcmp(c, "wl24")) {
			script_need(&sl, 2);
			s_wl24(s, (uint32_t)parse_u64(sl.argv[1]));
		} else if (!strcmp(c, "wl32")) {
			script_need(&sl, 2);
			s_wl32(s, (uint32_t)parse_u64(sl.argv[1]));
		} else if (!strcmp(c, "wl64")) {
			script_need(&sl, 2);
			s_wl64(s, parse_u64(sl.argv[1]));
		} else if (!strcmp(c, "wlf")) {
			script_need(&sl, 2);
			s_wlf(s, parse_f32(sl.argv[1]));
		} else if (!strcmp(c, "wld")) {
			script_need(&sl, 2);
			s_wld(s, parse_f64(sl.argv[1]));
		} else if (!strcmp(c, "wb16")) {
			script_need(&sl, 2);
			s_wb16(s, (uint16_t)parse_u64(sl.argv[1]));
		} else if (!strcmp(c, "wb24")) {
			script_need(&sl, 2);
			s_wb24(s, (uint32_t)parse_u64(sl.argv[1]));
		} else if (!strcmp(c, "wb32")) {
			script_need(&sl, 2);
			s_wb32(s, (uint32_t)parse_u64(sl.argv[1]));
		} else if (!strcmp(c, "wb64")) {
			script_need(&sl, 2);
			s_wb64(s, parse_u64(sl.argv[1]));
		} else if (!strcmp(c, "wbf")) {
			script_need(&sl, 2);
			s_wbf(s, parse_f32(sl.argv[1]));
		} else if (!strcmp(c, "wbd")) {
			script_need(&sl, 2);
			s_wbd(s, parse_f64(sl.argv[1]));
		} else if (!strcmp(c, "write") || !strcmp(c, "serialize_w")) {
			/* write HEX|- | @null N  [SIZE] */
			script_need(&sl, 2);
			size_t n = 0, size;
			uint8_t *buf = NULL;
			if (!strcmp(sl.argv[1], "@null")) {
				script_need(&sl, 3);
				size = (size_t)parse_u64(sl.argv[2]);
				printf("%s @null %zu", c, size);
			} else {
				buf = script_parse_hex(sl.argv[1], &n);
				size = sl.argc >= 3 ? (size_t)parse_u64(sl.argv[2]) : n;
				printf("%s %s %zu", c, sl.argv[1], size);
			}
			size_t r;
			if (!strcmp(c, "write"))
				r = s_write(s, buf, size);
			else
				r = serialize(s, buf, size);
			printf(" -> %zu\n", r);
			free(buf);
		} else if (!strcmp(c, "write_pat")) {
			/* write_pat N [START]: N generated bytes through s_write */
			script_need(&sl, 2);
			size_t n = (size_t)parse_u64(sl.argv[1]);
			size_t st = sl.argc >= 3 ? (size_t)parse_u64(sl.argv[2]) : 0;
			uint8_t *b = pattern(n, st);
			size_t r = s_write(s, b, n);
			printf("write_pat %zu %zu -> %zu\n", n, st, r);
			free(b);
		} else if (!strcmp(c, "read") || !strcmp(c, "serialize_r")) {
			/* read N | read @null N */
			script_need(&sl, 2);
			bool isnull = !strcmp(sl.argv[1], "@null");
			size_t n = (size_t)parse_u64(sl.argv[isnull ? 2 : 1]);
			uint8_t *buf = calloc(1, n ? n : 1);
			size_t r = !strcmp(c, "read") ? s_read(s, isnull ? NULL : buf, n)
						      : serialize(s, isnull ? NULL : buf, n);
			printf("%s %s%zu -> %zu data=", c, isnull ? "@null " : "", n, r);
			script_print_hex(buf, r > n ? n : r);
			printf("\n");
			free(buf);
		} else if (!strcmp(c, "seek")) {
			script_need(&sl, 3);
			enum serialize_seek_type t = !strcmp(sl.argv[1], "start") ? SERIALIZE_SEEK_START
						     : !strcmp(sl.argv[1], "cur")  ? SERIALIZE_SEEK_CURRENT
										   : SERIALIZE_SEEK_END;
			int64_t off = strtoll(sl.argv[2], NULL, 0);
			printf("seek %s %" PRId64 " -> %" PRId64 "\n", seek_name(t), off, serializer_seek(s, off, t));
		} else if (!strcmp(c, "pos")) {
			printf("pos -> %" PRId64 "\n", serializer_get_pos(s));
		} else if (!strcmp(c, "dump")) {
			if (cur->kind == K_ARRAY) {
				printf("dump array num=%zu pos=%zu bytes=", cur->ad.bytes.num, cur->ad.cur_pos);
				script_print_hex(cur->ad.bytes.array, cur->ad.bytes.num);
				printf("\n");
			} else if (cur->kind == K_CUSTOM) {
				printf("dump custom log=");
				script_print_hex(cur->cu.log, cur->cu.n);
				printf("\n");
			} else {
				printf("dump none\n");
			}
		} else if (!strcmp(c, "array_reset")) {
			array_output_serializer_reset(&cur->ad);
		} else if (!strcmp(c, "fput")) {
			script_need(&sl, 3);
			size_t n;
			uint8_t *b = script_parse_hex(sl.argv[2], &n);
			put_file(sl.argv[1], b, n);
			free(b);
		} else if (!strcmp(c, "fputs")) {
			script_need(&sl, 3);
			put_file(sl.argv[1], sl.argv[2], strlen(sl.argv[2]));
		} else if (!strcmp(c, "fget")) {
			script_need(&sl, 2);
			size_t n;
			uint8_t *b = get_file(sl.argv[1], &n);
			if (!b) {
				printf("file %s missing\n", sl.argv[1]);
			} else {
				printf("file %s size=%zu bytes=", sl.argv[1], n);
				script_print_hex(b, n);
				printf("\n");
				free(b);
			}
		} else if (!strcmp(c, "fsum")) {
			/* size and FNV-1a 64 of a (possibly large) file */
			script_need(&sl, 2);
			size_t n;
			uint8_t *b = get_file(sl.argv[1], &n);
			if (!b) {
				printf("file %s missing\n", sl.argv[1]);
			} else {
				uint64_t h = 1469598103934665603ULL;
				for (size_t i = 0; i < n; i++)
					h = (h ^ b[i]) * 1099511628211ULL;
				printf("file %s size=%zu fnv=%016" PRIx64 "\n", sl.argv[1], n, h);
				free(b);
			}
		} else if (!strcmp(c, "funlink")) {
			script_need(&sl, 2);
			char p[1024];
			path_of(p, sizeof p, sl.argv[1]);
			unlink(p);
		} else if (!strcmp(c, "bs_init") || !strcmp(c, "bs_init_gen")) {
			/* bs_init HEX [LEN] | bs_init_gen N [LEN] */
			script_need(&sl, 2);
			size_t n, len;
			free(br_buf);
			if (!strcmp(c, "bs_init")) {
				br_buf = script_parse_hex(sl.argv[1], &n);
			} else {
				n = (size_t)parse_u64(sl.argv[1]);
				br_buf = pattern(n, 0);
			}
			len = sl.argc >= 3 ? (size_t)parse_u64(sl.argv[2]) : n;
			bitstream_reader_init(&br, br_buf, len);
		} else if (!strcmp(c, "bs_bits")) {
			script_need(&sl, 2);
			int bits = (int)strtol(sl.argv[1], NULL, 0);
			printf("bs_bits %d -> 0x%02x\n", bits, bitstream_reader_read_bits(&br, bits));
		} else if (!strcmp(c, "bs_bit")) {
			printf("bs_bit -> %u\n", bitstream_reader_read_bit(&br));
		} else if (!strcmp(c, "bs_r8")) {
			printf("bs_r8 -> 0x%02x\n", bitstream_reader_r8(&br));
		} else if (!strcmp(c, "bs_r16")) {
			printf("bs_r16 -> 0x%04x\n", bitstream_reader_r16(&br));
		} else if (!strcmp(c, "bs_skip")) {
			/* bs_skip N: N silent bitstream_reader_r8 calls */
			script_need(&sl, 2);
			for (uint64_t i = parse_u64(sl.argv[1]); i > 0; i--)
				bitstream_reader_r8(&br);
		} else if (!strcmp(c, "bs_state")) {
			printf("bs_state pos=%u sub=0x%02x len=%zu\n", br.pos, br.subPos, br.len);
		} else if (!strcmp(c, "tl_create") || !strcmp(c, "tl_add")) {
			script_need(&sl, 3);
			int i = atoi(sl.argv[1]) & 3;
			char p[1024];
			path_of(p, sizeof p, sl.argv[2]);
			if (!strcmp(c, "tl_create")) {
				text_lookup_destroy(lookups[i]);
				lookups[i] = text_lookup_create(p);
				printf("tl_create %d %s -> %s\n", i, sl.argv[2], lookups[i] ? "ok" : "null");
			} else {
				printf("tl_add %d %s -> %s\n", i, sl.argv[2],
				       text_lookup_add(lookups[i], p) ? "true" : "false");
			}
		} else if (!strcmp(c, "tl_get")) {
			script_need(&sl, 3);
			int i = atoi(sl.argv[1]) & 3;
			const char *out = "UNSET";
			bool r = text_lookup_getstr(lookups[i], sl.argv[2], &out);
			printf("tl_get %d ", i);
			script_print_quoted(sl.argv[2]);
			if (r) {
				printf(" -> ");
				script_print_quoted(out);
				printf("\n");
			} else {
				printf(" -> false\n");
			}
		} else if (!strcmp(c, "tl_destroy")) {
			script_need(&sl, 2);
			int i = atoi(sl.argv[1]) & 3;
			text_lookup_destroy(lookups[i]);
			lookups[i] = NULL;
		} else if (!strcmp(c, "tl_defect")) {
			/* tl_defect I NAME EQUIV: NAME is an input on which the reference has undefined
			 * behaviour (see spec S-010 D-1). The reference runner does not load NAME; it loads
			 * EQUIV, a file whose required result is identical to the required result of NAME,
			 * into slot I and prints "ub". A conforming runner loads NAME itself. */
			script_need(&sl, 4);
			int i = atoi(sl.argv[1]) & 3;
			char p[1024];
			path_of(p, sizeof p, sl.argv[3]);
			text_lookup_destroy(lookups[i]);
			lookups[i] = text_lookup_create(p);
			printf("tl_defect %d %s -> ub\n", i, sl.argv[2]);
		} else if (!strcmp(c, "echo")) {
			for (int i = 1; i < sl.argc; i++)
				printf("%s%s", i > 1 ? " " : "", sl.argv[i]);
			printf("\n");
		} else {
			fprintf(stderr, "script: line %d: unknown command %s\n", sl.lineno, c);
			return 2;
		}
	}

	for (int i = 0; i < 4; i++) {
		close_slot(&slots[i]);
		text_lookup_destroy(lookups[i]);
	}
	free(br_buf);
	fclose(f);
	return 0;
}
