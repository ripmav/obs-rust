/*
 * Conformance runner for the RTMP protocol engine of the vendored librtmp (spec S-031):
 * plugins/obs-outputs/librtmp/{rtmp.c,parseurl.c} (+ amf.c, log.c). See the "rtmp runner"
 * section of test/conformance/README.md for the script language. The engine runs over the
 * fake transport of gen/rtmp_stubs.c: bytes the engine sends are captured, bytes it receives
 * are scripted. All output is on stdout.
 */
#include <errno.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#include <mbedtls/md.h>

#include "harness.h"
#include "rtmp_stubs.h"
#include "../../../plugins/obs-outputs/librtmp/rtmp.h"
#include "../../../plugins/obs-outputs/librtmp/log.h"

int rt_digest_pos(const uint8_t *b, int alg);
void rt_block_digest(const uint8_t *b, int pos, const uint8_t *key, size_t kl, uint8_t *out);
const uint8_t *rt_fms_key(void);

static RTMP *R;
static RTMPPacket pkt; /* the caller's packet struct, reused by read/drain/client */
static char *urlbuf;

/* ---------------- logging ---------------- */
static void rlog(int level, const char *fmt, va_list ap)
{
	char msg[2048];
	if (level > RTMP_LOGINFO)
		return;
	vsnprintf(msg, sizeof msg, fmt, ap);
	for (char *p = msg; *p; p++)
		if (*p == '\n')
			*p = ' ';
	printf("log %s %s\n", level <= RTMP_LOGERROR ? "error" : level == RTMP_LOGWARNING ? "warning" : "info", msg);
}

static void hexp(const uint8_t *d, size_t n)
{
	script_print_hex(d, n);
}

/* ---------------- outgoing byte decoder ---------------- */
static struct {
	size_t hs_left; /* handshake bytes still to come (C0+C1, C2) */
	int hs_stage;
	uint32_t cs;
	struct cs_state {
		uint32_t ts, len, sid;
		int type, fmt, chunks, extflag;
		uint8_t *body;
		uint32_t got;
		char hdr[64];
		int hdrlen;
		uint8_t hdrb[32];
	} *st[70000];
	uint8_t *pend;
	size_t pend_len;
} dec = {.cs = 128};

static void dec_reset(int hs)
{
	dec.cs = 128;
	dec.hs_left = hs ? 3073 : 0;
	dec.hs_stage = 0;
	for (int i = 0; i < 70000; i++) {
		if (dec.st[i]) {
			free(dec.st[i]->body);
			free(dec.st[i]);
			dec.st[i] = NULL;
		}
	}
	free(dec.pend);
	dec.pend = NULL;
	dec.pend_len = 0;
}

/* returns bytes needed to parse the next chunk header+payload, or consumes it */
static void dec_feed(const uint8_t *d, size_t n)
{
	dec.pend = realloc(dec.pend, dec.pend_len + n + 1);
	memcpy(dec.pend + dec.pend_len, d, n);
	dec.pend_len += n;
	for (;;) {
		uint8_t *p = dec.pend;
		size_t have = dec.pend_len, used = 0;
		if (dec.hs_left) {
			size_t want = dec.hs_stage == 0 ? 1537 : 1536;
			if (have < want)
				return;
			printf("out %s ", dec.hs_stage == 0 ? "c0c1" : "c2");
			hexp(p, want);
			putchar('\n');
			dec.hs_stage++;
			dec.hs_left -= want;
			used = want;
		} else {
			if (!have)
				return;
			int fmt = p[0] >> 6, csid = p[0] & 63;
			size_t hl = 1;
			if (csid == 0) {
				if (have < 2)
					return;
				csid = p[1] + 64;
				hl = 2;
			} else if (csid == 1) {
				if (have < 3)
					return;
				csid = p[1] + p[2] * 256 + 64;
				hl = 3;
			}
			struct cs_state *s = dec.st[csid];
			int cont = s && s->body;
			static const int msz[4] = {11, 7, 3, 0};
			size_t mh = msz[fmt];
			if (have < hl + mh)
				return;
			uint32_t tsf = s ? s->ts : 0;
			uint32_t len = s ? s->len : 0, sid = s ? s->sid : 0;
			int type = s ? s->type : 0;
			if (fmt <= 2)
				tsf = p[hl] << 16 | p[hl + 1] << 8 | p[hl + 2];
			if (fmt <= 1) {
				len = p[hl + 3] << 16 | p[hl + 4] << 8 | p[hl + 5];
				type = p[hl + 6];
			}
			if (fmt == 0)
				sid = p[hl + 7] | p[hl + 8] << 8 | p[hl + 9] << 16 | (uint32_t)p[hl + 10] << 24;
			int ext = 0;
			size_t eh = hl + mh;
			if ((fmt <= 2 || cont) && tsf == 0xffffff)
				ext = 1;
			if (ext) {
				if (have < eh + 4)
					return;
				eh += 4;
			}
			uint32_t got = cont ? s->got : 0;
			size_t chunk = len - got < dec.cs ? len - got : dec.cs;
			if (!cont && fmt == 3 && !s) {
				printf("out error chunk fmt3 without state\n");
				free(dec.pend);
				dec.pend = NULL;
				dec.pend_len = 0;
				return;
			}
			if (have < eh + chunk)
				return;
			if (!s)
				s = dec.st[csid] = calloc(1, sizeof *s);
			uint32_t ets = ext ? ((uint32_t)p[hl + mh] << 24 | p[hl + mh + 1] << 16 | p[hl + mh + 2] << 8 | p[hl + mh + 3])
					   : tsf;
			if (!cont) {
				s->fmt = fmt;
				s->chunks = 0;
				s->body = malloc(len + 1);
				s->got = 0;
				s->hdrlen = (int)eh;
				memcpy(s->hdrb, p, eh > 32 ? 32 : eh);
				s->extflag = ext;
			}
			s->ts = tsf;
			s->len = len;
			s->sid = sid;
			s->type = type;
			memcpy(s->body + s->got, p + eh, chunk);
			s->got += (uint32_t)chunk;
			s->chunks++;
			if (s->got == s->len) {
				printf("out msg csid=%d fmt=%d chunks=%d ts=%u type=%d sid=%u len=%u hdr=", csid, s->fmt, s->chunks, ets,
				       s->type, s->sid, s->len);
				hexp(s->hdrb, s->hdrlen);
				printf(" body=");
				hexp(s->body, s->len);
				putchar('\n');
				if (s->type == 1 && s->len >= 4)
					dec.cs = s->body[0] << 24 | s->body[1] << 16 | s->body[2] << 8 | s->body[3];
				free(s->body);
				s->body = NULL;
				s->got = 0;
			}
			used = eh + chunk;
		}
		memmove(dec.pend, dec.pend + used, dec.pend_len - used);
		dec.pend_len -= used;
	}
}

static int raw_mode;

static void flush_out(void)
{
	if (rt.out_len && raw_mode) {
		printf("out raw ");
		hexp(rt.out, rt.out_len);
		putchar('\n');
		rt.out_len = 0;
	}
	if (rt.out_len) {
		dec_feed(rt.out, rt.out_len);
		rt.out_len = 0;
	}
}

/* ---------------- helpers ---------------- */
static const char *ARG(const struct script_line *sl, int i)
{
	return i < sl->argc ? sl->argv[i] : "";
}

static char *dupn(const char *s, size_t n)
{
	char *p = malloc(n + 1);
	memcpy(p, s, n);
	p[n] = 0;
	return p;
}

static void pav(const char *name, AVal *a)
{
	printf(" %s=", name);
	if (!a->av_val) {
		printf("-");
		return;
	}
	char *t = dupn(a->av_val, a->av_len > 0 ? a->av_len : 0);
	script_print_quoted(t);
	free(t);
}

static void print_link(void)
{
	printf("link protocol=%d port=%u", R->Link.protocol, R->Link.port);
	pav("host", &R->Link.hostname);
	pav("app", &R->Link.app);
	pav("tcUrl", &R->Link.tcUrl);
	printf(" lflags=%d\n", R->Link.lFlags);
}

static void print_pkt(RTMPPacket *p, int ready)
{
	printf("pkt ready=%d ch=%d fmt=%d type=%d ts=%u abs=%d info2=%d len=%u read=%u", ready, p->m_nChannel, p->m_headerType,
	       p->m_packetType, p->m_nTimeStamp, p->m_hasAbsTimestamp, p->m_nInfoField2, p->m_nBodySize, p->m_nBytesRead);
	if (ready && p->m_body) {
		printf(" body=");
		hexp((uint8_t *)p->m_body, p->m_nBodySize);
	}
	putchar('\n');
}

static void print_state(void)
{
	printf("state connected=%d playing=%d stream_id=%d invokes=%d calls=[", RTMP_IsConnected(R), R->m_bPlaying,
	       R->m_stream_id, R->m_numInvokes);
	for (int i = 0; i < R->m_numCalls; i++)
		printf("%s%s:%d", i ? "," : "", R->m_methodCalls[i].name.av_val, R->m_methodCalls[i].num);
	printf("] bytes_in=%d bytes_in_sent=%d in_chunk=%d out_chunk=%d server_bw=%d client_bw=%d client_bw2=%d bwcheck=%d",
	       R->m_nBytesIn, R->m_nBytesInSent, R->m_inChunkSize, R->m_outChunkSize, R->m_nServerBW, R->m_nClientBW,
	       R->m_nClientBW2, R->m_nBWCheckCounter);
	printf(" streams=[");
	for (int i = 0; i < R->Link.nStreams; i++) {
		printf("%s%d:", i ? "," : "", R->Link.streams[i].id);
		pav("pp", &R->Link.streams[i].playpath);
	}
	printf("] cur=%d playing_streams=%d pflags=%d timedout=%d\n", R->Link.curStreamIdx, R->Link.playingStreams,
	       R->Link.pFlags, RTMP_IsTimedout(R));
}

static void do_new(void)
{
	if (R) {
		RTMP_Close(R);
		RTMP_Free(R);
	}
	R = RTMP_Alloc();
	RTMP_Init(R);
	memset(&pkt, 0, sizeof pkt);
	dec_reset(0);
	rt_reset_conn();
	free(rt.in);
	rt.in = NULL;
	rt.in_len = rt.in_pos = 0;
	rt.nmarkers = 0;
	rt.out_len = 0;
	rt.recv_cap = 0;
	rt.empty_mode = 0;
	rt.send_fail_after = -1;
	rt.send_eintr_once = 0;
	rt.hev_errno = 0;
	rt.connects = 0;
	rt.ncuts = rt.cut_idx = 0;
	rt.on_connect = NULL;
	raw_mode = 0;
}

static void feed_s1(int alg, uint32_t uptime, const uint8_t *ver)
{
	uint8_t s1[1536];
	for (int i = 0; i < 1536; i++)
		s1[i] = (uint8_t)(i * 7 + 1);
	s1[0] = uptime >> 24, s1[1] = uptime >> 16, s1[2] = uptime >> 8, s1[3] = uptime;
	memcpy(s1 + 4, ver, 4);
	int pos = rt_digest_pos(s1, alg);
	rt_block_digest(s1, pos, rt_fms_key(), 36, s1 + pos);
	rt_feed(s1, 1536);
}

static void guarded_playpath(const char *s)
{
	long ps = sysconf(_SC_PAGESIZE);
	char *m = mmap(NULL, ps * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	mprotect(m, ps, PROT_NONE);
	char *str = m + ps;
	memcpy(str, s, strlen(s) + 1);
	AVal in = {str, (int)strlen(s)}, out;
	RTMP_ParsePlaypath(&in, &out);
	printf("playpath out=");
	if (!out.av_val)
		printf("-");
	else {
		script_print_quoted(out.av_val);
		printf(" len=%d", out.av_len);
	}
	putchar('\n');
}

static int run_cmd(struct script_line *sl);

/* called by the fake connector: output of the previous connection is complete */
static void on_new_connection(void)
{
	flush_out();
	dec_reset(1);
	if (rt.connects > 1)
		printf("reconnect\n");
}

static int in_child(struct script_line *sl, int silent)
{
	fflush(stdout);
	pid_t pid = fork();
	if (pid == 0) {
		alarm(5);
		if (silent)
			freopen("/dev/null", "w", stdout);
		run_cmd(sl);
		fflush(stdout);
		_exit(0);
	}
	int st = 0;
	waitpid(pid, &st, 0);
	return WIFEXITED(st) && WEXITSTATUS(st) == 0 ? 0 : WIFSIGNALED(st) && WTERMSIG(st) == SIGALRM ? 2 : 1;
}

static void set_str(AVal *a, const char *s)
{
	a->av_val = strdup(s);
	a->av_len = (int)strlen(s);
}

static int run_cmd(struct script_line *sl)
{
	const char *c = sl->argv[0];
	if (!strcmp(c, "echo")) {
		printf("echo");
		for (int i = 1; i < sl->argc; i++)
			printf(" %s", sl->argv[i]);
		putchar('\n');
	} else if (!strcmp(c, "new")) {
		do_new();
	} else if (!strcmp(c, "clock")) {
		rt.clock_ms = strtoul(ARG(sl, 1), 0, 0);
	} else if (!strcmp(c, "seed")) {
		rt.rand_state = (uint32_t)strtoul(ARG(sl, 1), 0, 0);
	} else if (!strcmp(c, "recvcap")) {
		rt.recv_cap = strtoul(ARG(sl, 1), 0, 0);
	} else if (!strcmp(c, "empty")) {
		rt.empty_mode = !strcmp(ARG(sl, 1), "eof") ? 1 : !strcmp(ARG(sl, 1), "reset") ? 2 : 0;
	} else if (!strcmp(c, "sendfail")) {
		rt.send_fail_after = atol(ARG(sl, 1));
		rt.send_fail_errno = atoi(ARG(sl, 2));
	} else if (!strcmp(c, "sendeintr")) {
		rt.send_eintr_once = 1;
	} else if (!strcmp(c, "hevfail")) {
		rt.hev_errno = atoi(ARG(sl, 1));
	} else if (!strcmp(c, "feed")) {
		for (int i = 1; i < sl->argc; i++) {
			size_t n;
			uint8_t *b = script_parse_hex(sl->argv[i], &n);
			rt_feed(b, n);
			free(b);
		}
	} else if (!strcmp(c, "feed_cut")) {
		rt.cuts[rt.ncuts++] = rt.in_len;
	} else if (!strcmp(c, "raw")) {
		raw_mode = atoi(ARG(sl, 1));
	} else if (!strcmp(c, "feed_echo")) {
		rt_feed_marker(1, 0);
	} else if (!strcmp(c, "feed_digest")) {
		rt_feed_marker(2, atoi(ARG(sl, 1)));
	} else if (!strcmp(c, "feed_s1")) {
		size_t n;
		uint8_t *v = script_parse_hex(ARG(sl, 3), &n);
		feed_s1(atoi(ARG(sl, 1)), (uint32_t)strtoul(ARG(sl, 2), 0, 0), v);
		free(v);
	} else if (!strcmp(c, "parseurl")) {
		int proto;
		unsigned port;
		AVal host = {0}, app = {0};
		char *u = strdup(ARG(sl, 1));
		int ret = RTMP_ParseURL(u, &proto, &host, &port, &app);
		printf("parseurl ret=%d protocol=%d port=%u", ret, proto, port);
		pav("host", &host);
		pav("app", &app);
		putchar('\n');
	} else if (!strcmp(c, "playpath")) {
		return in_child(sl, 0) ? (printf("playpath crash\n"), 0) : 0;
	} else if (!strcmp(c, "playpath_inner")) {
		guarded_playpath(ARG(sl, 1));
	} else if (!strcmp(c, "seturl")) {
		urlbuf = strdup(ARG(sl, 1));
		int ret = RTMP_SetupURL(R, urlbuf);
		printf("seturl ret=%d ", ret);
		print_link();
	} else if (!strcmp(c, "enablewrite")) {
		RTMP_EnableWrite(R);
	} else if (!strcmp(c, "addstream")) {
		int idx = RTMP_AddStream(R, !strcmp(ARG(sl, 1), "-") ? NULL : strdup(ARG(sl, 1)));
		printf("addstream idx=%d", idx);
		if (idx >= 0 && idx < RTMP_MAX_STREAMS)
			pav(" playpath", &R->Link.streams[idx].playpath);
		putchar('\n');
	} else if (!strcmp(c, "set")) {
		const char *k = ARG(sl, 1), *v = ARG(sl, 2);
		if (!strcmp(k, "outchunk"))
			R->m_outChunkSize = atoi(v);
		else if (!strcmp(k, "chunkinfo"))
			R->m_bSendChunkSizeInfo = atoi(v);
		else if (!strcmp(k, "flashver"))
			set_str(&R->Link.flashVer, v);
		else if (!strcmp(k, "swfurl"))
			set_str(&R->Link.swfUrl, v);
		else if (!strcmp(k, "swfurl_is_tcurl"))
			R->Link.swfUrl = R->Link.tcUrl;
		else if (!strcmp(k, "pageurl"))
			set_str(&R->Link.pageUrl, v);
		else if (!strcmp(k, "user"))
			set_str(&R->Link.pubUser, v);
		else if (!strcmp(k, "pass"))
			set_str(&R->Link.pubPasswd, v);
		else if (!strcmp(k, "auth"))
			set_str(&R->Link.auth, v);
		else if (!strcmp(k, "authflag"))
			R->Link.lFlags |= atoi(v) ? RTMP_LF_AUTH : 0;
		else if (!strcmp(k, "usher"))
			set_str(&R->Link.usherToken, v);
		else if (!strcmp(k, "subscribe"))
			set_str(&R->Link.subscribepath, v);
		else if (!strcmp(k, "live"))
			R->Link.lFlags |= atoi(v) ? RTMP_LF_LIVE : 0;
		else if (!strcmp(k, "playlist"))
			R->Link.lFlags |= atoi(v) ? RTMP_LF_PLST : 0;
		else if (!strcmp(k, "bufx"))
			R->Link.lFlags |= atoi(v) ? RTMP_LF_BUFX : 0;
		else if (!strcmp(k, "swfv"))
			R->Link.lFlags |= atoi(v) ? RTMP_LF_SWFV : 0;
		else if (!strcmp(k, "token"))
			set_str(&R->Link.token, v);
		else if (!strcmp(k, "swfsize"))
			R->Link.SWFSize = (uint32_t)strtoul(v, 0, 0);
		else if (!strcmp(k, "swfhash")) {
			size_t n;
			uint8_t *b = script_parse_hex(v, &n);
			memcpy(R->Link.SWFHash, b, n > 32 ? 32 : n);
			free(b);
		} else if (!strcmp(k, "encoding"))
			R->m_fEncoding = atof(v);
		else if (!strcmp(k, "sendenc"))
			R->m_bSendEncoding = atoi(v);
		else if (!strcmp(k, "clientbw"))
			R->m_nClientBW = atoi(v);
		else if (!strcmp(k, "bufms"))
			RTMP_SetBufferMS(R, atoi(v));
		else if (!strcmp(k, "seek"))
			R->Link.seekTime = atoi(v);
		else if (!strcmp(k, "stop"))
			R->Link.stopTime = atoi(v);
		else if (!strcmp(k, "sendcounter"))
			R->m_bSendCounter = atoi(v);
		else if (!strcmp(k, "protocol_or"))
			R->Link.protocol |= atoi(v);
		else
			printf("set unknown %s\n", k);
	} else if (!strcmp(c, "connect")) {
		dec_reset(1);
		rt.on_connect = on_new_connection;
		rt_reset_conn();
		int ret = RTMP_Connect(R, NULL);
		flush_out();
		printf("connect ret=%d connects=%d host=%s port=%d\n", ret, rt.connects, rt.last_host, rt.last_port);
	} else if (!strcmp(c, "connectstream")) {
		int ret = RTMP_ConnectStream(R, 0);
		flush_out();
		printf("connectstream ret=%d\n", ret);
	} else if (!strcmp(c, "read") || !strcmp(c, "client")) {
		int ret = RTMP_ReadPacket(R, &pkt);
		flush_out();
		printf("read ret=%d\n", ret);
		if (ret) {
			int ready = RTMPPacket_IsReady(&pkt);
			print_pkt(&pkt, ready);
			if (ready) {
				if (c[0] == 'c') {
					int r2 = pkt.m_nBodySize ? RTMP_ClientPacket(R, &pkt) : 0;
					flush_out();
					printf("clientpacket ret=%d\n", r2);
				}
				RTMPPacket_Free(&pkt);
			}
		}
	} else if (!strcmp(c, "drain")) {
		/* OBS streaming-time reader: ReadPacket only, no ClientPacket */
		int n = 0;
		while (RTMP_ReadPacket(R, &pkt)) {
			if (RTMPPacket_IsReady(&pkt)) {
				print_pkt(&pkt, 1);
				RTMPPacket_Free(&pkt);
			}
			if (++n > 1000)
				break;
		}
		flush_out();
		printf("drain end connected=%d\n", RTMP_IsConnected(R));
	} else if (!strcmp(c, "write")) {
		size_t n;
		uint8_t *b = script_parse_hex(ARG(sl, 1), &n);
		char *cp = malloc(n + 1);
		memcpy(cp, b, n);
		int ret = RTMP_Write(R, cp, (int)n, atoi(ARG(sl, 2)));
		flush_out();
		printf("write ret=%d\n", ret);
	} else if (!strcmp(c, "writetag")) {
		/* writetag TYPE TS LEN IDX [FLVHEADER]: one FLV tag with body byte i = (i*3)&255, followed by the
		 * 4 byte previous-tag-size; a nonzero FLVHEADER prepends the 13 byte file header */
		int type = atoi(ARG(sl, 1)), len = atoi(ARG(sl, 3)), hdr = atoi(ARG(sl, 5));
		uint32_t ts = (uint32_t)strtoul(ARG(sl, 2), 0, 0);
		size_t n = (hdr ? 13 : 0) + 11 + len + 4, o = 0;
		uint8_t *b = calloc(1, n);
		if (hdr) {
			memcpy(b, "FLV\1\5\0\0\0\x09\0\0\0\0", 13);
			o = 13;
		}
		b[o] = (uint8_t)type;
		b[o + 1] = len >> 16, b[o + 2] = len >> 8, b[o + 3] = len;
		b[o + 4] = ts >> 16, b[o + 5] = ts >> 8, b[o + 6] = ts, b[o + 7] = ts >> 24;
		for (int i = 0; i < len; i++)
			b[o + 11 + i] = (uint8_t)(i * 3);
		b[n - 4] = (11 + len) >> 24, b[n - 3] = (11 + len) >> 16, b[n - 2] = (11 + len) >> 8, b[n - 1] = 11 + len;
		int ret = RTMP_Write(R, (char *)b, (int)n, atoi(ARG(sl, 4)));
		flush_out();
		printf("writetag ret=%d\n", ret);
	} else if (!strcmp(c, "sendpacket")) {
		/* sendpacket CH FMT TYPE TS INFO2 HEX QUEUE */
		size_t n;
		uint8_t *b = script_parse_hex(ARG(sl, 6), &n);
		RTMPPacket p;
		memset(&p, 0, sizeof p);
		char *mem = calloc(1, n + RTMP_MAX_HEADER_SIZE + 1);
		memcpy(mem + RTMP_MAX_HEADER_SIZE, b, n);
		p.m_body = mem + RTMP_MAX_HEADER_SIZE;
		p.m_nChannel = atoi(ARG(sl, 1));
		p.m_headerType = (uint8_t)atoi(ARG(sl, 2));
		p.m_packetType = (uint8_t)atoi(ARG(sl, 3));
		p.m_nTimeStamp = (uint32_t)strtoul(ARG(sl, 4), 0, 0);
		p.m_nInfoField2 = (int32_t)strtol(ARG(sl, 5), 0, 0);
		p.m_nBodySize = (uint32_t)n;
		int ret = RTMP_SendPacket(R, &p, atoi(ARG(sl, 7)));
		flush_out();
		printf("sendpacket ret=%d\n", ret);
	} else if (!strcmp(c, "ctrl")) {
		int ret = RTMP_SendCtrl(R, (short)atoi(ARG(sl, 1)), (unsigned)strtoul(ARG(sl, 2), 0, 0),
					(unsigned)strtoul(ARG(sl, 3), 0, 0));
		flush_out();
		printf("ctrl ret=%d\n", ret);
	} else if (!strcmp(c, "sendfn")) {
		const char *f = ARG(sl, 1);
		int a = atoi(ARG(sl, 2)), ret = -99;
		if (!strcmp(f, "createstream"))
			ret = RTMP_SendCreateStream(R);
		else if (!strcmp(f, "pause"))
			ret = RTMP_SendPause(R, a, atoi(ARG(sl, 3)));
		else if (!strcmp(f, "rpause"))
			ret = RTMP_Pause(R, a);
		else if (!strcmp(f, "seek"))
			ret = RTMP_SendSeek(R, a);
		else if (!strcmp(f, "serverbw"))
			ret = RTMP_SendServerBW(R);
		else if (!strcmp(f, "clientbw"))
			ret = RTMP_SendClientBW(R);
		else if (!strcmp(f, "updatebuf"))
			RTMP_UpdateBufferMS(R), ret = 1;
		flush_out();
		printf("sendfn %s ret=%d\n", f, ret);
	} else if (!strcmp(c, "deletestream")) {
		RTMP_DeleteStream(R, atoi(ARG(sl, 1)));
		flush_out();
		printf("deletestream\n");
	} else if (!strcmp(c, "close")) {
		RTMP_Close(R);
		flush_out();
		printf("close connected=%d\n", RTMP_IsConnected(R));
	} else if (!strcmp(c, "state")) {
		print_state();
	} else if (!strcmp(c, "chts")) {
		int ch = atoi(ARG(sl, 1));
		printf("chts ch=%d ts=%d\n", ch, ch < R->m_channelsAllocatedIn ? R->m_channelTimestamp[ch] : -1);
	} else if (!strcmp(c, "gettime")) {
		printf("gettime %u\n", RTMP_GetTime());
	} else if (!strcmp(c, "try") || !strcmp(c, "ignore")) {
		struct script_line sub;
		sub.argc = sl->argc - 1;
		for (int i = 1; i < sl->argc; i++)
			sub.argv[i - 1] = sl->argv[i];
		int silent = c[0] == 'i';
		printf("%s-begin\n", c);
		int r = in_child(&sub, silent);
		printf("%s-end %s\n", c, r == 0 ? "ok" : r == 2 ? "hang" : "crash");
	} else {
		fprintf(stderr, "script: unknown command '%s'\n", c);
		exit(2);
	}
	return 0;
}

int main(int argc, char **argv)
{
	FILE *f = harness_open(argc, argv);
	RTMP_LogSetCallback(rlog);
	struct script_line sl;
	int lineno = 0;
	do_new();
	while (script_next(f, &sl, &lineno)) {
		/* playpath runs the guarded variant in a child */
		if (!strcmp(sl.argv[0], "playpath")) {
			struct script_line in = sl;
			in.argv[0] = "playpath_inner";
			fflush(stdout);
			pid_t pid = fork();
			if (pid == 0) {
				alarm(5);
				run_cmd(&in);
				fflush(stdout);
				_exit(0);
			}
			int st;
			waitpid(pid, &st, 0);
			if (!(WIFEXITED(st) && WEXITSTATUS(st) == 0))
				printf("playpath crash\n");
			continue;
		}
		run_cmd(&sl);
	}
	return 0;
}
