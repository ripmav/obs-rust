/*
 * S-059 mpegtsout: scripted fake network layer. Replaces libsrt completely (every srt_* function the reference header
 * obs-ffmpeg-srt.h calls) and the sender side of librist (create / peer / start / write / destroy / stats); the address parser
 * and logging of librist are the real library. Every call that changes socket state is printed as an "srt ..." / "rist ..."
 * line so that the option mapping and the call order are part of the expected output. Bytes handed to srt_send /
 * rist_sender_data_write are captured and can be analysed as an MPEG-TS stream ("ts" command). Test tooling, not part of OBS.
 */
#define _SYS_SYSLOG_H
#define INC_SRT_LOGGING_API_H
#define LOG_DEBUG_TRACE 8
#define SRT_LOGF_DISABLE_TIME 1
typedef void SRT_LOG_HANDLER_FN(void *opaque, int level, const char *file, int line, const char *area, const char *message);
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>
#include <unistd.h>
#include <poll.h>
#include <srt/srt.h>
#include <librist/librist.h>
#include "mpegtsout_api.h"

/* ------------------------------------------------------------------ capture + fault table */
static pthread_mutex_t lk = PTHREAD_MUTEX_INITIALIZER;
static uint8_t *cap;
static size_t cap_len, cap_cap;
static int nsends, nbad_chunks;
static size_t max_chunk, min_chunk;
static int capture_on = 1;

struct fault {
	char name[48];
	int err, os_errno, nth, calls;
};
static struct fault faults[24];
static int nfaults;

static void capture(const void *p, size_t n)
{
	pthread_mutex_lock(&lk);
	nsends++;
	if (n > max_chunk)
		max_chunk = n;
	if (nsends == 1 || n < min_chunk)
		min_chunk = n;
	if (n % 188)
		nbad_chunks++;
	if (capture_on) {
		if (cap_len + n > cap_cap) {
			cap_cap = (cap_len + n) * 2;
			cap = realloc(cap, cap_cap);
		}
		memcpy(cap + cap_len, p, n);
		cap_len += n;
	}
	pthread_mutex_unlock(&lk);
}
size_t net_captured(void)
{
	return cap_len;
}
size_t net_max_chunk(void)
{
	return max_chunk;
}
void net_capture_clear(void)
{
	pthread_mutex_lock(&lk);
	cap_len = 0;
	nsends = nbad_chunks = 0;
	max_chunk = min_chunk = 0;
	pthread_mutex_unlock(&lk);
}
int net_sends(void)
{
	return nsends;
}

static int g_lasterr, g_errno;
static bool hit(const char *name)
{
	for (int i = 0; i < nfaults; i++) {
		struct fault *f = &faults[i];
		if (strcmp(f->name, name))
			continue;
		f->calls++;
		if (f->nth == 0 || f->calls == f->nth) {
			g_lasterr = f->err;
			g_errno = f->os_errno;
			return true;
		}
	}
	return false;
}

/* ------------------------------------------------------------------ fake libsrt */
static int next_sock, ep_mode, ep_err, ep_skip, ep_calls, state_override = -1, payload_override = -1, payload_set = -1;
static int next_eid, startups, cleanups, reject_override = -1;
static double st_ts, st_rate, st_sent, st_retrans, st_drop;
static int connected_fd[64];

static const struct {
	int opt;
	const char *name;
	char kind; /* i int, l int64, s string, b bool, k linger */
} optab[] = {
#define O(x, k) {x, #x, k}
	O(SRTO_RENDEZVOUS, 'i'),  O(SRTO_TRANSTYPE, 'i'), O(SRTO_MAXBW, 'l'),      O(SRTO_PBKEYLEN, 'i'),
	O(SRTO_PASSPHRASE, 's'),  O(SRTO_ENFORCEDENCRYPTION, 'i'), O(SRTO_KMREFRESHRATE, 'i'),
	O(SRTO_KMPREANNOUNCE, 'i'), O(SRTO_SNDDROPDELAY, 'i'), O(SRTO_MSS, 'i'),   O(SRTO_FC, 'i'),
	O(SRTO_IPTTL, 'i'),       O(SRTO_IPTOS, 'i'),     O(SRTO_LATENCY, 'i'),    O(SRTO_RCVLATENCY, 'i'),
	O(SRTO_PEERLATENCY, 'i'), O(SRTO_TLPKTDROP, 'i'), O(SRTO_NAKREPORT, 'i'),  O(SRTO_CONNTIMEO, 'i'),
	O(SRTO_SNDBUF, 'i'),      O(SRTO_RCVBUF, 'i'),    O(SRTO_LOSSMAXTTL, 'i'), O(SRTO_MINVERSION, 'i'),
	O(SRTO_STREAMID, 's'),    O(SRTO_CONGESTION, 's'), O(SRTO_MESSAGEAPI, 'i'), O(SRTO_PAYLOADSIZE, 'i'),
	O(SRTO_SENDER, 'i'),      O(SRTO_TSBPDMODE, 'i'), O(SRTO_LINGER, 'k'),     O(SRTO_INPUTBW, 'l'),
	O(SRTO_OHEADBW, 'i'),     O(SRTO_SNDSYN, 'i'),    O(SRTO_RCVSYN, 'i'),     O(SRTO_REUSEADDR, 'i'),
	O(SRTO_UDP_RCVBUF, 'i'),  O(SRTO_UDP_SNDBUF, 'i'),
#undef O
};

int srt_startup(void)
{
	startups++;
	next_sock = 0; /* fake descriptor numbers restart with every library session (real ones are arbitrary) */
	printf("srt startup\n");
	if (hit("startup"))
		return -1;
	return 0;
}
int srt_cleanup(void)
{
	cleanups++;
	printf("srt cleanup\n");
	return 0;
}
SRTSOCKET srt_create_socket(void)
{
	if (hit("create_socket"))
		return SRT_INVALID_SOCK;
	int id = ++next_sock;
	payload_set = -1; /* a fresh socket reports the default payload size again */
	if (id < 64)
		connected_fd[id] = 0;
	printf("srt create_socket -> %d\n", id);
	return id;
}
static void addr_str(const struct sockaddr *a, char *out, size_t n)
{
	char h[64] = "?";
	int port = 0;
	if (a->sa_family == AF_INET) {
		const struct sockaddr_in *s = (const struct sockaddr_in *)a;
		inet_ntop(AF_INET, &s->sin_addr, h, sizeof(h));
		port = ntohs(s->sin_port);
	} else if (a->sa_family == AF_INET6) {
		const struct sockaddr_in6 *s = (const struct sockaddr_in6 *)a;
		inet_ntop(AF_INET6, &s->sin6_addr, h, sizeof(h));
		port = ntohs(s->sin6_port);
	}
	snprintf(out, n, "%s:%d", h, port);
}
int srt_bind(SRTSOCKET u, const struct sockaddr *name, int namelen)
{
	char b[96];
	(void)namelen;
	addr_str(name, b, sizeof(b));
	printf("srt bind fd=%d %s\n", u, b);
	return hit("bind") ? -1 : 0;
}
int srt_listen(SRTSOCKET u, int backlog)
{
	printf("srt listen fd=%d backlog=%d\n", u, backlog);
	return hit("listen") ? -1 : 0;
}
SRTSOCKET srt_accept(SRTSOCKET u, struct sockaddr *addr, int *addrlen)
{
	(void)addr;
	(void)addrlen;
	if (hit("accept"))
		return SRT_INVALID_SOCK;
	int id = ++next_sock;
	if (id < 64)
		connected_fd[id] = 1;
	printf("srt accept fd=%d -> %d\n", u, id);
	return id;
}
int srt_connect(SRTSOCKET u, const struct sockaddr *name, int namelen)
{
	char b[96];
	(void)namelen;
	addr_str(name, b, sizeof(b));
	printf("srt connect fd=%d %s\n", u, b);
	if (hit("connect"))
		return -1;
	if (u < 64)
		connected_fd[u] = 1;
	return 0;
}
int srt_close(SRTSOCKET u)
{
	printf("srt close fd=%d\n", u);
	return hit("close") ? -1 : 0;
}
SRT_SOCKSTATUS srt_getsockstate(SRTSOCKET u)
{
	if (state_override >= 0)
		return (SRT_SOCKSTATUS)state_override;
	return (u > 0 && u < 64 && connected_fd[u]) ? SRTS_CONNECTED : SRTS_OPENED;
}
int srt_setsockopt(SRTSOCKET u, int level, SRT_SOCKOPT opt, const void *val, int len)
{
	(void)level;
	char nm[48], v[160] = "";
	const char *name = "?";
	char kind = 'i';
	for (size_t i = 0; i < sizeof(optab) / sizeof(optab[0]); i++)
		if (optab[i].opt == (int)opt) {
			name = optab[i].name;
			kind = optab[i].kind;
		}
	if (!strcmp(name, "?"))
		snprintf(nm, sizeof(nm), "opt%d", (int)opt), name = nm;
	switch (kind) {
	case 'l':
		if (len == 8)
			snprintf(v, sizeof(v), "%lld", (long long)*(const int64_t *)val);
		else
			snprintf(v, sizeof(v), "len%d", len);
		break;
	case 's':
		snprintf(v, sizeof(v), "\"%.*s\"", len, (const char *)val);
		break;
	case 'k': {
		const struct linger *l = val;
		snprintf(v, sizeof(v), "onoff=%d,linger=%d", l->l_onoff, l->l_linger);
		break;
	}
	default:
		if (len == 4)
			snprintf(v, sizeof(v), "%d", *(const int *)val);
		else
			snprintf(v, sizeof(v), "len%d", len);
	}
	char key[64];
	snprintf(key, sizeof(key), "setsockopt:%s", name);
	bool f = hit(key);
	printf("srt setsockopt fd=%d %s=%s%s\n", u, name, v, f ? " FAIL" : "");
	if (!f && opt == SRTO_PAYLOADSIZE && len == 4)
		payload_set = *(const int *)val;
	return f ? -1 : 0;
}
int srt_getsockopt(SRTSOCKET u, int level, SRT_SOCKOPT opt, void *val, int *len)
{
	(void)u;
	(void)level;
	if (hit("getsockopt"))
		return -1;
	if (opt == SRTO_PAYLOADSIZE) {
		*(int *)val = payload_override >= 0 ? payload_override : (payload_set >= 0 ? payload_set : 1316);
		*len = 4;
	} else if (opt == SRTO_STREAMID) {
		strcpy(val, "accepted-id");
		*len = 11;
	}
	return 0;
}
int srt_epoll_create(void)
{
	if (hit("epoll_create"))
		return -1;
	return 1000 + (++next_eid);
}
int srt_epoll_add_usock(int eid, SRTSOCKET s, const int *events)
{
	(void)eid;
	(void)s;
	(void)events;
	return hit("epoll_add") ? -1 : 0;
}
int srt_epoll_release(int eid)
{
	(void)eid;
	printf("srt epoll_release\n");
	return 0;
}
int srt_epoll_wait(int eid, SRTSOCKET *r, int *rn, SRTSOCKET *w, int *wn, int64_t ms, SYSSOCKET *lr, int *lrn,
		   SYSSOCKET *lw, int *lwn)
{
	(void)eid;
	(void)r;
	(void)rn;
	(void)w;
	(void)wn;
	(void)ms;
	(void)lr;
	(void)lrn;
	(void)lw;
	(void)lwn;
	int n;
	pthread_mutex_lock(&lk);
	n = ++ep_calls;
	pthread_mutex_unlock(&lk);
	if (hit("epoll_wait"))
		return -1;
	if (ep_mode == 0 || n <= ep_skip)
		return 0;
	g_errno = 0;
	g_lasterr = ep_mode == 1 ? SRT_ETIMEOUT : ep_err;
	return -1;
}
int srt_send(SRTSOCKET u, const char *buf, int len)
{
	(void)u;
	if (hit("send"))
		return -1;
	capture(buf, (size_t)len);
	return len;
}
int srt_getlasterror(int *errno_loc)
{
	if (errno_loc)
		*errno_loc = g_errno;
	return g_lasterr;
}
const char *srt_getlasterror_str(void)
{
	static __thread char b[48];
	snprintf(b, sizeof(b), "fake srt error %d", g_lasterr);
	return b;
}
int srt_getrejectreason(SRTSOCKET u)
{
	(void)u;
	return g_lasterr == SRT_ECONNREJ ? (reject_override >= 0 ? reject_override : SRT_REJ_PEER) : SRT_REJ_TIMEOUT;
}
const char *srt_rejectreason_str(int id)
{
	static __thread char b[48];
	snprintf(b, sizeof(b), "fake reject reason %d", id);
	return b;
}
int srt_bstats(SRTSOCKET u, SRT_TRACEBSTATS *perf, int clear)
{
	(void)u;
	(void)clear;
	memset(perf, 0, sizeof(*perf));
	perf->msTimeStamp = (int64_t)st_ts;
	perf->mbpsSendRate = st_rate;
	perf->byteSentTotal = (uint64_t)st_sent;
	perf->byteRetransTotal = (uint32_t)st_retrans;
	perf->byteSndDropTotal = (uint32_t)st_drop;
	return 0;
}
int srt_bistats(SRTSOCKET u, SRT_TRACEBSTATS *perf, int clear, int instantaneous)
{
	(void)clear;
	(void)instantaneous;
	srt_bstats(u, perf, 0);
	perf->msRTT = 12.5;
	perf->mbpsBandwidth = 100.0;
	return 0;
}

/* ------------------------------------------------------------------ fake librist sender side */
struct rist_ctx {
	int dummy;
};
struct rist_peer {
	int dummy;
};
static struct rist_ctx g_ctx;
static struct rist_peer g_peer;
static int (*stats_cb)(void *, const struct rist_stats *);
static void *stats_arg;
static int stats_interval;
static int rist_destroys;
int rist_sender_create(struct rist_ctx **ctx, enum rist_profile profile, uint32_t flow_id, struct rist_logging_settings *ls)
{
	(void)ls;
	if (hit("rist_sender_create"))
		return g_lasterr;
	*ctx = &g_ctx;
	printf("rist sender_create profile=%d flow_id=%u\n", (int)profile, flow_id);
	return 0;
}
int rist_peer_create(struct rist_ctx *ctx, struct rist_peer **peer, const struct rist_peer_config *c)
{
	(void)ctx;
	printf("rist peer_create address=%s physical_port=%u virt_dst_port=%u key_size=%d secret=\"%s\"\n", c->address,
	       (unsigned)c->physical_port, (unsigned)c->virt_dst_port, c->key_size, c->secret);
	printf("rist   srp_username=\"%s\" srp_password=\"%s\" recovery_length_min=%u recovery_length_max=%u\n",
	       c->srp_username, c->srp_password, (unsigned)c->recovery_length_min, (unsigned)c->recovery_length_max);
	printf("rist   initiate_conn=%d recovery_mode=%d weight=%u session_timeout=%u cname=\"%s\" compression=%d\n",
	       c->initiate_conn, (int)c->recovery_mode, (unsigned)c->weight, (unsigned)c->session_timeout, c->cname,
	       c->compression);
	if (hit("rist_peer_create"))
		return g_lasterr;
	*peer = &g_peer;
	return 0;
}
int rist_start(struct rist_ctx *ctx)
{
	(void)ctx;
	printf("rist start\n");
	return hit("rist_start") ? g_lasterr : 0;
}
int rist_stats_callback_set(struct rist_ctx *ctx, int interval, int (*cb)(void *, const struct rist_stats *), void *arg)
{
	(void)ctx;
	printf("rist stats_callback_set interval=%d\n", interval);
	if (hit("rist_stats_callback_set"))
		return -1;
	stats_cb = cb;
	stats_arg = arg;
	stats_interval = interval;
	return 0;
}
int rist_stats_free(const struct rist_stats *s)
{
	free((void *)s);
	return 0;
}
int rist_sender_data_write(struct rist_ctx *ctx, const struct rist_data_block *b)
{
	(void)ctx;
	if (hit("rist_data_write"))
		return g_lasterr;
	capture(b->payload, b->payload_len);
	return (int)b->payload_len;
}
int rist_destroy(struct rist_ctx *ctx)
{
	(void)ctx;
	rist_destroys++;
	printf("rist destroy\n");
	return hit("rist_destroy") ? g_lasterr : 0;
}

/* ------------------------------------------------------------------ real loopback sockets (udp:// and tcp:// paths) */
static int lfd = -1, lport, lis_tcp;
static pthread_t lth;
static volatile int lstop, lth_on;

static void drain_fd(int fd)
{
	uint8_t buf[65536];
	for (;;) {
		ssize_t n = recv(fd, buf, sizeof(buf), MSG_DONTWAIT);
		if (n <= 0)
			break;
		capture(buf, (size_t)n);
	}
}
static void *reader(void *arg)
{
	(void)arg;
	int fd = lfd;
	if (lis_tcp) {
		int c = -1;
		while (!lstop && c < 0) {
			struct pollfd p = {fd, POLLIN, 0};
			if (poll(&p, 1, 50) > 0)
				c = accept(fd, NULL, NULL);
		}
		while (!lstop && c >= 0) {
			struct pollfd p = {c, POLLIN, 0};
			if (poll(&p, 1, 50) > 0) {
				uint8_t buf[65536];
				ssize_t n = recv(c, buf, sizeof(buf), 0);
				if (n <= 0)
					break;
				capture(buf, (size_t)n);
			}
		}
		if (c >= 0) {
			drain_fd(c);
			close(c);
		}
	} else {
		while (!lstop) {
			struct pollfd p = {fd, POLLIN, 0};
			if (poll(&p, 1, 50) > 0)
				drain_fd(fd);
		}
		drain_fd(fd);
	}
	return NULL;
}
int net_port(void)
{
	return lport;
}
static void sock_listen(bool tcp)
{
	struct sockaddr_in a = {0};
	socklen_t l = sizeof(a);
	a.sin_family = AF_INET;
	a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	lfd = socket(AF_INET, tcp ? SOCK_STREAM : SOCK_DGRAM, 0);
	lis_tcp = tcp;
	if (bind(lfd, (struct sockaddr *)&a, sizeof(a)) || (tcp && listen(lfd, 1)) ||
	    getsockname(lfd, (struct sockaddr *)&a, &l)) {
		printf("sock_listen failed\n");
		return;
	}
	lport = ntohs(a.sin_port);
	lstop = 0;
	lth_on = 1;
	pthread_create(&lth, NULL, reader, NULL);
	printf("sock_listen %s ok\n", tcp ? "tcp" : "udp");
}
static void sock_close(void)
{
	if (lth_on) {
		lstop = 1;
		pthread_join(lth, NULL);
		lth_on = 0;
	}
	if (lfd >= 0)
		close(lfd);
	lfd = -1;
}

/* ------------------------------------------------------------------ command interface */
static int err_code(const char *s)
{
	static const struct {
		const char *n;
		int v;
	} t[] = {
#define E(x) {#x, SRT_##x}
		E(EUNKNOWN), E(ECONNREJ), E(ECONNSOCK), E(ESECFAIL), E(ECONNLOST), E(ENOCONN),   E(ECONNFAIL),
		E(ENOSERVER), E(ETIMEOUT), E(EASYNCRCV), E(EASYNCSND), E(ECONNSETUP), E(EINVSOCK),
#undef E
	};
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcmp(t[i].n, s))
			return t[i].v;
	return (int)strtol(s, NULL, 0);
}

void net_reset(void)
{
	pthread_mutex_lock(&lk);
	cap_len = 0;
	nsends = nbad_chunks = 0;
	max_chunk = min_chunk = 0;
	capture_on = 1;
	pthread_mutex_unlock(&lk);
	nfaults = 0;
	g_lasterr = g_errno = 0;
	ep_mode = ep_err = ep_skip = ep_calls = 0;
	state_override = payload_override = payload_set = reject_override = -1;
	next_sock = 0;
	memset(connected_fd, 0, sizeof(connected_fd));
	stats_cb = NULL;
	st_ts = st_rate = st_sent = st_retrans = st_drop = 0;
}

void net_command(int argc, char **argv)
{
	const char *c = argv[0];
	if (!strcmp(c, "srt_fail") && argc >= 3 && nfaults < 24) {
		/* srt_fail NAME ERR [OSERRNO [NTH]] */
		struct fault *f = &faults[nfaults++];
		memset(f, 0, sizeof(*f));
		snprintf(f->name, sizeof(f->name), "%s", argv[1]);
		f->err = err_code(argv[2]);
		f->os_errno = argc > 3 ? atoi(argv[3]) : 0;
		f->nth = argc > 4 ? atoi(argv[4]) : 0;
	} else if (!strcmp(c, "rist_fail") && argc >= 2 && nfaults < 24) {
		/* rist_fail NAME [RET [NTH]] */
		struct fault *f = &faults[nfaults++];
		memset(f, 0, sizeof(*f));
		snprintf(f->name, sizeof(f->name), "rist_%s", argv[1]);
		f->err = argc > 2 ? atoi(argv[2]) : -1;
		f->nth = argc > 3 ? atoi(argv[3]) : 0;
	} else if (!strcmp(c, "srt_epoll") && argc >= 2) {
		/* srt_epoll ready|timeout|error [ERR [SKIP]] : result of every srt_epoll_wait after SKIP calls */
		ep_mode = !strcmp(argv[1], "ready") ? 0 : !strcmp(argv[1], "timeout") ? 1 : 2;
		ep_err = argc > 2 ? err_code(argv[2]) : SRT_EUNKNOWN;
		ep_skip = argc > 3 ? atoi(argv[3]) : 0;
		ep_calls = 0;
	} else if (!strcmp(c, "srt_state") && argc >= 2) {
		static const struct {
			const char *n;
			int v;
		} t[] = {{"init", SRTS_INIT},       {"opened", SRTS_OPENED},   {"listening", SRTS_LISTENING},
			 {"connecting", SRTS_CONNECTING}, {"connected", SRTS_CONNECTED}, {"broken", SRTS_BROKEN},
			 {"closing", SRTS_CLOSING},   {"closed", SRTS_CLOSED},   {"nonexist", SRTS_NONEXIST}};
		state_override = -1;
		for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++)
			if (!strcmp(t[i].n, argv[1]))
				state_override = t[i].v;
	} else if (!strcmp(c, "srt_reject") && argc >= 2) {
		reject_override = !strcmp(argv[1], "badsecret") ? SRT_REJ_BADSECRET : atoi(argv[1]);
	} else if (!strcmp(c, "srt_payload") && argc >= 2) {
		payload_override = !strcmp(argv[1], "auto") ? -1 : atoi(argv[1]);
	} else if (!strcmp(c, "srt_stats") && argc >= 6) {
		st_ts = atof(argv[1]);
		st_rate = atof(argv[2]);
		st_sent = atof(argv[3]);
		st_retrans = atof(argv[4]);
		st_drop = atof(argv[5]);
	} else if (!strcmp(c, "sock_listen") && argc >= 2) {
		sock_listen(!strcmp(argv[1], "tcp"));
	} else if (!strcmp(c, "capture_clear")) {
		net_capture_clear();
	} else if (!strcmp(c, "sock_close")) {
		sock_close();
		printf("sock_close\n");
	} else if (!strcmp(c, "capture") && argc >= 2) {
		capture_on = !strcmp(argv[1], "on");
	} else if (!strcmp(c, "rist_stats") && stats_cb) {
		struct rist_stats *s = calloc(1, sizeof(*s));
		s->stats_json = "{\"fake\":1}";
		s->json_size = 10;
		s->stats_type = RIST_STATS_SENDER_PEER;
		s->stats.sender_peer.bandwidth = 2500000;
		s->stats.sender_peer.sent = 1000;
		s->stats.sender_peer.received = 990;
		s->stats.sender_peer.retransmitted = 5;
		s->stats.sender_peer.quality = 99.5;
		s->stats.sender_peer.rtt = 21;
		stats_cb(stats_arg, s);
	} else if (!strcmp(c, "rist_stats_other") && stats_cb) {
		struct rist_stats *s = calloc(1, sizeof(*s));
		s->stats_json = "{}";
		s->stats_type = RIST_STATS_RECEIVER_FLOW;
		stats_cb(stats_arg, s);
	} else {
		printf("net: unknown command %s\n", c);
	}
	(void)stats_interval;
}

/* ------------------------------------------------------------------ MPEG-TS structural analysis */
static uint64_t pts_at(const uint8_t *p)
{
	return ((uint64_t)((p[0] >> 1) & 7) << 30) | ((uint64_t)p[1] << 22) | ((uint64_t)(p[2] >> 1) << 15) |
	       ((uint64_t)p[3] << 7) | (p[4] >> 1);
}

void net_ts_report(void)
{
	size_t n = cap_len;
	printf("ts bytes_multiple_of_188=%d send_chunks_multiple_of_188=%d\n", n % 188 == 0, nbad_chunks == 0);
	size_t np = n / 188;
	int sync_ok = 1;
	int cc_ok = 1;
	int cc[8192];
	int seen[8192] = {0};
	int pes_n[8192] = {0};
	int pes_sid[8192] = {0};
	uint64_t last_pts[8192] = {0};
	char deltas[8192][160] = {{0}};
	for (int i = 0; i < 8192; i++)
		cc[i] = -1;
	int pmt_pid = -1, have_pat = 0, have_pmt = 0, pat_first = -1;
	for (size_t i = 0; i < np; i++) {
		const uint8_t *t = cap + i * 188;
		if (t[0] != 0x47) {
			sync_ok = 0;
			continue;
		}
		int pid = ((t[1] & 0x1f) << 8) | t[2];
		int pusi = (t[1] >> 6) & 1;
		int afc = (t[3] >> 4) & 3;
		int c = t[3] & 15;
		if (pat_first < 0)
			pat_first = pid;
		seen[pid] = 1;
		if (afc & 1) {
			if (cc[pid] >= 0 && c != ((cc[pid] + 1) & 15))
				cc_ok = 0;
			cc[pid] = c;
		}
		int off = 4;
		if (afc & 2)
			off += 1 + t[4];
		if (!(afc & 1) || off >= 188)
			continue;
		const uint8_t *pl = t + off;
		int plen = 188 - off;
		if (pid == 0 && pusi && !have_pat && plen > 12) {
			const uint8_t *s = pl + 1 + pl[0];
			int slen = ((s[1] & 15) << 8) | s[2];
			if (s[0] == 0 && slen >= 9) {
				pmt_pid = ((s[10] & 0x1f) << 8) | s[11];
				have_pat = 1;
				printf("ts pat program=%d pmt_pid=0x%04x\n", (s[8] << 8) | s[9], pmt_pid);
			}
		} else if (pid == pmt_pid && pusi && !have_pmt && plen > 16) {
			const uint8_t *s = pl + 1 + pl[0];
			int slen = ((s[1] & 15) << 8) | s[2];
			if (s[0] == 2) {
				int pcr = ((s[8] & 0x1f) << 8) | s[9];
				int pil = ((s[10] & 15) << 8) | s[11];
				printf("ts pmt pcr_pid=0x%04x\n", pcr);
				const uint8_t *e = s + 12 + pil;
				const uint8_t *end = s + 3 + slen - 4;
				while (e + 5 <= end) {
					int es = ((e[3] & 15) << 8) | e[4];
					printf("ts pmt stream pid=0x%04x type=0x%02x\n", ((e[1] & 0x1f) << 8) | e[2], e[0]);
					e += 5 + es;
				}
				have_pmt = 1;
			}
		} else if (pusi && plen >= 14 && pl[0] == 0 && pl[1] == 0 && pl[2] == 1) {
			pes_n[pid]++;
			pes_sid[pid] = pl[3];
			if (pl[7] & 0x80) {
				uint64_t p = pts_at(pl + 9);
				if (pes_n[pid] > 1) {
					char b[24];
					snprintf(b, sizeof(b), "%s%lld", strlen(deltas[pid]) ? "," : "",
						 (long long)(p - last_pts[pid]));
					if (strlen(deltas[pid]) + strlen(b) < 150)
						strcat(deltas[pid], b);
				}
				last_pts[pid] = p;
			}
		}
	}
	printf("ts packets_nonzero=%d sync_ok=%d continuity_ok=%d\n", np > 0, sync_ok, cc_ok);
	for (int i = 0; i < 8192; i++)
		if (seen[i]) {
			printf("ts pid 0x%04x%s%s", i, i == 0 ? " pat" : "", i == pmt_pid ? " pmt" : "");
			if (pes_n[i])
				printf(" pes_starts=%d stream_id=0x%02x pts_deltas=%s", pes_n[i], pes_sid[i], deltas[i]);
			printf("\n");
		}
}
