/*
 * Conformance runner for the video bitstream helpers (spec S-012):
 * obs-nal.c (Annex-B start code finder), obs-avc.c, obs-hevc.c, obs-av1.c.
 *
 * Every command runs in a forked child; the input is placed so that its last byte is
 * the last byte before an inaccessible page. Any read past the end of the input
 * (a reference DEFECT) therefore kills the child.
 *
 * Reference defects (see spec S-012 D-1..D-4): for the AV1 commands the runner also
 * contains a small *safe model* (m_* functions) that implements the behaviour the spec
 * requires. The printed line is the reference's output when the reference behaves
 * correctly (the model is then cross-checked against it and a difference aborts the run
 * with exit status 2) and the model's output when the input triggers a defect
 * (reference crashes or has undefined behaviour). A NULL data argument is run as an
 * empty, non-NULL buffer (the reference crashes on NULL, defect D-5).
 *
 * Data arguments (DATA): hex string, "-" (empty), @name (a buffer defined by buf/cat/rep),
 * "@null" (NULL pointer, size 0).
 *
 *   buf NAME HEX...            define NAME as the concatenation of the hex tokens
 *   cat NAME ARG...            NAME = concatenation of DATA arguments
 *   rep NAME COUNT HEX         NAME = HEX repeated COUNT times
 *   trail N                    N filler bytes (0xA5) follow the input inside readable memory
 *                              (changes the alignment; disables over-read detection)
 *   echo TEXT
 *   layout                     sizeof/offsetof of struct encoder_packet
 *   find DATA [START [END]]    obs_nal_find_startcode; prints the offset of the result
 *   avc_find DATA [START [END]]  obs_avc_find_startcode
 *   avc_keyframe | hevc_keyframe | av1_keyframe DATA
 *   avc_priority | hevc_priority DATA [PRIORITY]     obs_parse_*_packet_priority
 *   avc_packet | hevc_packet DATA [PRIORITY [KEYFRAME]]  obs_parse_*_packet
 *   avc_header DATA            obs_parse_avc_header
 *   avc_extract | hevc_extract | av1_extract DATA   obs_extract_*_headers
 *   av1_metadata DATA TYPE | av1_metadata_t35 DATA  metadata_obu / metadata_obu_itu_t35
 *   av1_obu DATA               file-local parse_obu_header
 *   av1_leb128 DATA            file-local leb128
 */
#include <stddef.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include "harness.h"
#include "obs.h"
#include "obs-avc.h"
#include "obs-hevc.h"
#include "obs-nal.h"
/* pulls in obs-av1.h and the static helpers leb128()/parse_obu_header() */
#include "obs-av1.c"

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

/* Resolves a DATA argument to a malloc'd copy (never NULL unless "@null"). */
static uint8_t *resolve(const char *arg, size_t *len, bool *is_null)
{
	*is_null = false;
	if (!strcmp(arg, "@null")) {
		*is_null = true;
		*len = 0;
		return NULL;
	}
	if (arg[0] == '@') {
		for (int i = 0; i < nbufs; i++)
			if (!strcmp(bufs[i].name, arg + 1)) {
				uint8_t *c = malloc(bufs[i].len ? bufs[i].len : 1);
				memcpy(c, bufs[i].data, bufs[i].len);
				*len = bufs[i].len;
				return c;
			}
		fprintf(stderr, "script: unknown buffer %s\n", arg);
		exit(2);
	}
	return script_parse_hex(arg, len);
}

/* Copies src to memory whose end is directly followed by an inaccessible page
 * (plus trail_bytes of readable filler). */
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

static void print_hexv(FILE *o, const char *key, const uint8_t *d, size_t n)
{
	fprintf(o, " %s=", key);
	if (!n)
		fputs("-", o);
	for (size_t i = 0; i < n; i++)
		fprintf(o, "%02x", d[i]);
}

static void packet_sentinels(struct encoder_packet *p)
{
	memset(p, 0, sizeof(*p));
	p->pts = 1001;
	p->dts = 1002;
	p->timebase_num = 1;
	p->timebase_den = 60;
	p->type = OBS_ENCODER_VIDEO;
	p->dts_usec = 7;
	p->sys_dts_usec = 8;
	p->drop_priority = 99;
	p->track_idx = 3;
	p->encoder = (obs_encoder_t *)(uintptr_t)0x1234;
}

static bool sentinels_ok(const struct encoder_packet *p)
{
	return p->pts == 1001 && p->dts == 1002 && p->timebase_num == 1 && p->timebase_den == 60 &&
	       p->type == OBS_ENCODER_VIDEO && p->dts_usec == 7 && p->sys_dts_usec == 8 && p->track_idx == 3 &&
	       p->encoder == (obs_encoder_t *)(uintptr_t)0x1234;
}

/* ---- safe AV1 model: what the spec requires (never reads out of bounds) ---- */

/* Correct 56-bit LEB128 (at most 8 bytes). *ub is set when the reference's
 * computation is undefined (shift of an int beyond its width). */
static uint64_t m_leb128(const uint8_t *buf, size_t size, size_t *len, bool *ub)
{
	uint64_t v = 0;
	*len = 0;
	for (int i = 0; i < 8 && (size_t)i < size; i++) {
		(*len)++;
		uint8_t b = buf[i];
		v |= (uint64_t)(b & 0x7f) << (i * 7);
		if (i >= 5 && (b & 0x7f))
			*ub = true;
		if (i == 4 && (b & 0x7f) > 7)
			*ub = true;
		if (!(b & 0x80))
			break;
	}
	return v;
}

static void m_obu(const uint8_t *buf, size_t size, size_t *st, size_t *sz, int *ty, bool *defect)
{
	*st = *sz = 0;
	*ty = 0;
	if (size < 1)
		return;
	*ty = (buf[0] >> 3) & 15;
	int ext = (buf[0] >> 2) & 1, has_size = (buf[0] >> 1) & 1;
	*st = 1 + (size_t)ext;
	size_t rem = size > *st ? size - *st : 0;
	if (*st > size)
		*defect = true; /* reference computes size - start with unsigned wrap-around */
	size_t l = 0;
	if (has_size)
		*sz = (size_t)m_leb128(buf + (rem ? *st : 0), rem, &l, defect);
	else
		*sz = size - 1;
	*st += l;
}

static bool m_keyframe(const uint8_t *buf, size_t size, bool *defect)
{
	size_t pos = 0;
	while (pos < size) {
		size_t st, sz;
		int ty;
		m_obu(buf + pos, size - pos, &st, &sz, &ty, defect);
		if (sz && (ty == OBS_OBU_FRAME || ty == OBS_OBU_FRAME_HEADER)) {
			if (pos + st >= size) {
				*defect = true; /* reference reads one byte past the end */
				return false;
			}
			uint8_t v = buf[pos + st];
			if (!(v >> 7))
				return ((v >> 5) & 3) == 0;
			return false;
		}
		if (st + sz > size - pos)
			break;
		pos += st + sz;
	}
	return false;
}

static void m_extract(const uint8_t *buf, size_t size, uint8_t **np, size_t *ns, uint8_t **hp, size_t *hs, bool *defect)
{
	*np = malloc(size + 1);
	*hp = malloc(size + 1);
	*ns = *hs = 0;
	size_t pos = 0;
	while (pos < size) {
		size_t st, sz;
		int ty;
		m_obu(buf + pos, size - pos, &st, &sz, &ty, defect);
		size_t chunk = st + sz;
		if (chunk > size - pos) {
			*defect = true; /* reference copies bytes beyond the end */
			chunk = size - pos;
		}
		if (ty == OBS_OBU_METADATA || ty == OBS_OBU_SEQUENCE_HEADER) {
			memcpy(*hp + *hs, buf + pos, chunk);
			*hs += chunk;
		}
		memcpy(*np + *ns, buf + pos, chunk);
		*ns += chunk;
		pos += chunk;
	}
}

/* Writes the result line of one command to o. With model=true the AV1 commands use the
 * safe model above instead of the reference and set *defect when the reference's
 * behaviour on this input is a defect. */
static void run_cmd(const struct script_line *sl, const uint8_t *p, size_t len, FILE *o, bool model, bool *defect)
{
	const char *c = sl->argv[0];

	if (!strcmp(c, "find") || !strcmp(c, "avc_find")) {
		long s = sl->argc > 2 ? strtol(sl->argv[2], NULL, 0) : 0;
		long e = sl->argc > 3 ? strtol(sl->argv[3], NULL, 0) : (long)len;
		const uint8_t *r = !strcmp(c, "find") ? obs_nal_find_startcode(p + s, p + e)
						      : obs_avc_find_startcode(p + s, p + e);
		fprintf(o, "%s %ld\n", c, (long)(r - p));
	} else if (!strcmp(c, "avc_keyframe")) {
		fprintf(o, "%s %d\n", c, obs_avc_keyframe(p, len));
	} else if (!strcmp(c, "hevc_keyframe")) {
		fprintf(o, "%s %d\n", c, obs_hevc_keyframe(p, len));
	} else if (!strcmp(c, "av1_keyframe")) {
		bool r = model ? m_keyframe(p, len, defect) : obs_av1_keyframe(p, len);
		fprintf(o, "%s %d\n", c, r);
	} else if (!strcmp(c, "avc_priority") || !strcmp(c, "hevc_priority")) {
		struct encoder_packet pk;
		packet_sentinels(&pk);
		pk.data = (uint8_t *)p;
		pk.size = len;
		pk.priority = sl->argc > 2 ? atoi(sl->argv[2]) : 0;
		int r = c[0] == 'a' ? obs_parse_avc_packet_priority(&pk) : obs_parse_hevc_packet_priority(&pk);
		fprintf(o, "%s %d\n", c, r);
	} else if (!strcmp(c, "avc_packet") || !strcmp(c, "hevc_packet")) {
		struct encoder_packet src, out;
		packet_sentinels(&src);
		memset(&out, 0x5A, sizeof(out));
		src.data = (uint8_t *)p;
		src.size = len;
		src.priority = sl->argc > 2 ? atoi(sl->argv[2]) : 0;
		src.keyframe = sl->argc > 3 ? atoi(sl->argv[3]) != 0 : false;
		if (c[0] == 'a')
			obs_parse_avc_packet(&out, &src);
		else
			obs_parse_hevc_packet(&out, &src);
		long refs = *((long *)out.data - 1);
		fprintf(o, "%s size=%zu keyframe=%d priority=%d drop_priority=%d refs=%ld fields=%s fresh=%d", c,
			out.size, out.keyframe, out.priority, out.drop_priority, refs,
			sentinels_ok(&out) ? "preserved" : "CHANGED", out.data != src.data);
		print_hexv(o, "data", out.data, out.size);
		fprintf(o, "\n");
		bfree((long *)out.data - 1);
	} else if (!strcmp(c, "avc_header")) {
		uint8_t *h = NULL;
		size_t r = obs_parse_avc_header(&h, p, len);
		fprintf(o, "%s ret=%zu", c, r);
		print_hexv(o, "data", h, h ? r : 0);
		fprintf(o, "\n");
		bfree(h);
	} else if (!strcmp(c, "avc_extract") || !strcmp(c, "hevc_extract")) {
		uint8_t *np = (uint8_t *)1, *hp = (uint8_t *)1, *sp = (uint8_t *)1;
		size_t ns = 99, hs = 99, ss = 99;
		if (c[0] == 'a')
			obs_extract_avc_headers(p, len, &np, &ns, &hp, &hs, &sp, &ss);
		else
			obs_extract_hevc_headers(p, len, &np, &ns, &hp, &hs, &sp, &ss);
		fprintf(o, "%s", c);
		print_hexv(o, "new", np, ns);
		print_hexv(o, "header", hp, hs);
		print_hexv(o, "sei", sp, ss);
		fprintf(o, "\n");
		bfree(np);
		bfree(hp);
		bfree(sp);
	} else if (!strcmp(c, "av1_extract")) {
		uint8_t *np = (uint8_t *)1, *hp = (uint8_t *)1;
		size_t ns = 99, hs = 99;
		if (model)
			m_extract(p, len, &np, &ns, &hp, &hs, defect);
		else
			obs_extract_av1_headers(p, len, &np, &ns, &hp, &hs);
		fprintf(o, "%s", c);
		print_hexv(o, "new", np, ns);
		print_hexv(o, "header", hp, hs);
		fprintf(o, "\n");
		if (model) {
			free(np);
			free(hp);
		} else {
			bfree(np);
			bfree(hp);
		}
	} else if (!strcmp(c, "av1_metadata") || !strcmp(c, "av1_metadata_t35")) {
		uint8_t *ob = NULL;
		size_t os = 0;
		if (!strcmp(c, "av1_metadata"))
			metadata_obu(p, len, &ob, &os, (uint8_t)strtol(sl->argv[2], NULL, 0));
		else
			metadata_obu_itu_t35(p, len, &ob, &os);
		fprintf(o, "%s size=%zu", c, os);
		print_hexv(o, "data", ob, os);
		fprintf(o, "\n");
		bfree(ob);
	} else if (!strcmp(c, "av1_obu")) {
		size_t st, sz;
		int ty;
		if (model)
			m_obu(p, len, &st, &sz, &ty, defect);
		else
			parse_obu_header(p, len, &st, &sz, &ty);
		fprintf(o, "%s start=%zu size=%zu type=%d\n", c, st, sz, ty);
	} else if (!strcmp(c, "av1_leb128")) {
		size_t l;
		uint64_t v = model ? m_leb128(p, len, &l, defect) : leb128(p, len, &l);
		fprintf(o, "%s value=%llu len=%zu\n", c, (unsigned long long)v, l);
	} else {
		fprintf(stderr, "script: line %d: unknown command '%s'\n", sl->lineno, c);
		exit(2);
	}
}

static bool is_data_cmd(const char *c)
{
	static const char *const names[] = {"find",        "avc_find",     "avc_keyframe",     "hevc_keyframe",
					    "av1_keyframe", "avc_priority", "hevc_priority",    "avc_packet",
					    "hevc_packet", "avc_header",    "avc_extract",      "hevc_extract",
					    "av1_extract", "av1_metadata", "av1_metadata_t35", "av1_obu",
					    "av1_leb128"};
	for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
		if (!strcmp(c, names[i]))
			return true;
	return false;
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
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
		} else if (!strcmp(c, "layout")) {
			printf("layout sizeof=%zu data=%zu size=%zu pts=%zu dts=%zu timebase_num=%zu timebase_den=%zu "
			       "type=%zu keyframe=%zu dts_usec=%zu sys_dts_usec=%zu priority=%zu drop_priority=%zu "
			       "track_idx=%zu encoder=%zu\n",
			       sizeof(struct encoder_packet), offsetof(struct encoder_packet, data),
			       offsetof(struct encoder_packet, size), offsetof(struct encoder_packet, pts),
			       offsetof(struct encoder_packet, dts), offsetof(struct encoder_packet, timebase_num),
			       offsetof(struct encoder_packet, timebase_den), offsetof(struct encoder_packet, type),
			       offsetof(struct encoder_packet, keyframe), offsetof(struct encoder_packet, dts_usec),
			       offsetof(struct encoder_packet, sys_dts_usec), offsetof(struct encoder_packet, priority),
			       offsetof(struct encoder_packet, drop_priority), offsetof(struct encoder_packet, track_idx),
			       offsetof(struct encoder_packet, encoder));
		} else if (is_data_cmd(c)) {
			script_need(&sl, 2);
			size_t len;
			bool is_null;
			uint8_t *raw = resolve(sl.argv[1], &len, &is_null);
			(void)is_null;
			/* NULL is run as an empty, non-NULL buffer (reference defect D-5) */
			uint8_t *g = guard_copy(raw, len);
			free(raw);
			bool defect = false, has_model = !strncmp(c, "av1_", 4) && strncmp(c, "av1_metadata", 12);
			char *mbuf = NULL, *rbuf = NULL;
			size_t mlen = 0, rlen = 0;
			if (has_model) {
				FILE *mo = open_memstream(&mbuf, &mlen);
				run_cmd(&sl, g, len, mo, true, &defect);
				fclose(mo);
			}
			fflush(stdout);
			int fds[2];
			if (pipe(fds)) {
				perror("pipe");
				return 2;
			}
			pid_t pid = fork();
			if (pid == 0) {
				close(fds[0]);
				/* the reference may print "out of memory" before dying on a defect input */
				if (!freopen("/dev/null", "w", stderr))
					_exit(3);
				FILE *ro = open_memstream(&rbuf, &rlen);
				bool d = false;
				run_cmd(&sl, g, len, ro, false, &d);
				fclose(ro);
				for (size_t off = 0; off < rlen;) {
					ssize_t w = write(fds[1], rbuf + off, rlen - off);
					if (w <= 0)
						_exit(3);
					off += (size_t)w;
				}
				_exit(0);
			}
			close(fds[1]);
			rbuf = malloc(1);
			rlen = 0;
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
			bool ok = WIFEXITED(status) && WEXITSTATUS(status) == 0;
			if (has_model && defect) {
				fwrite(mbuf, 1, mlen, stdout);
			} else if (!ok) {
				if (has_model) {
					fprintf(stderr, "line %d: reference crashed but the model saw no defect\n", sl.lineno);
					return 2;
				}
				printf("%s crash\n", c);
			} else {
				if (has_model && (mlen != rlen || memcmp(mbuf, rbuf, rlen))) {
					fprintf(stderr, "line %d: model differs from reference\n  model: %.*s  ref:   %.*s",
						sl.lineno, (int)mlen, mbuf, (int)rlen, rbuf);
					return 2;
				}
				fwrite(rbuf, 1, rlen, stdout);
			}
			free(mbuf);
			free(rbuf);
		} else {
			fprintf(stderr, "script: line %d: unknown command '%s'\n", sl.lineno, c);
			return 2;
		}
	}
	fclose(f);
	return 0;
}
