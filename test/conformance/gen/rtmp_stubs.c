/*
 * Conformance harness (area rtmp, spec S-031): the transport seam of the vendored librtmp.
 * Defines send/recv/times/rand (executable definitions override libc), the happy-eyeballs
 * connector and the system-trust-store loader. Every function is an INPUT of the engine
 * documented in spec S-031 section 3.
 */
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/times.h>
#include <unistd.h>

#include <mbedtls/md.h>
#include <mbedtls/x509_crt.h>

#include "rtmp_stubs.h"

struct happy_eyeballs_ctx {
	int fd;
};

struct rt_fake rt = {.send_fail_after = -1, .rand_state = 1};

static void append(uint8_t **b, size_t *len, const void *d, size_t n)
{
	*b = realloc(*b, *len + n + 1);
	memcpy(*b + *len, d, n);
	*len += n;
}

void rt_feed(const uint8_t *d, size_t n)
{
	append(&rt.in, &rt.in_len, d, n);
}

void rt_feed_marker(int kind, int arg)
{
	uint8_t z[1536] = {0};
	rt.markers[rt.nmarkers++] = (struct rt_marker){.off = rt.in_len, .kind = kind, .arg = arg};
	rt_feed(z, sizeof z);
}

void rt_reset_conn(void)
{
	free(rt.conn_out);
	rt.conn_out = NULL;
	rt.conn_len = 0;
}

static const uint8_t fms_key[68] = {
	'G', 'e', 'n', 'u', 'i', 'n', 'e', ' ', 'A', 'd', 'o', 'b', 'e', ' ', 'F', 'l', 'a', 's', 'h', ' ',
	'M', 'e', 'd', 'i', 'a', ' ', 'S', 'e', 'r', 'v', 'e', 'r', ' ', '0', '0', '1', 0xf0, 0xee, 0xc2, 0x4a,
	0x80, 0x68, 0xbe, 0xe8, 0x2e, 0x00, 0xd0, 0xd1, 0x02, 0x9e, 0x7e, 0x57, 0x6e, 0xec, 0x5d, 0x2d, 0x29, 0x80,
	0x6f, 0xab, 0x93, 0xb8, 0xe6, 0x36, 0xcf, 0xeb, 0x31, 0xae};

static void hmac(const uint8_t *key, size_t kl, const uint8_t *m, size_t ml, uint8_t *out)
{
	mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), key, kl, m, ml, out);
}

/* position of the 32-byte digest inside a 1536 byte handshake block */
int rt_digest_pos(const uint8_t *b, int alg)
{
	if (alg == 0)
		return (b[8] + b[9] + b[10] + b[11]) % 728 + 12;
	return (b[772] + b[773] + b[774] + b[775]) % 728 + 776;
}

/* HMAC-SHA256 over the block without its 32 digest bytes */
void rt_block_digest(const uint8_t *b, int pos, const uint8_t *key, size_t kl, uint8_t *out)
{
	uint8_t m[1504];
	memcpy(m, b, pos);
	memcpy(m + pos, b + pos + 32, 1504 - pos);
	hmac(key, kl, m, 1504, out);
}

const uint8_t *rt_fms_key(void)
{
	return fms_key;
}

static void materialize(struct rt_marker *m)
{
	uint8_t *s2 = rt.in + m->off;
	if (m->done || rt.conn_len < 1537)
		return;
	if (m->kind == 1) {
		memcpy(s2, rt.conn_out + 1, 1536);
	} else {
		const uint8_t *c1 = rt.conn_out + 1;
		uint8_t dk[32];
		for (int i = 0; i < 1504; i++)
			s2[i] = (uint8_t)(i * 5 + 3);
		hmac(fms_key, 68, c1 + rt_digest_pos(c1, m->arg), 32, dk);
		hmac(dk, 32, s2, 1504, s2 + 1504);
	}
	m->done = 1;
}

ssize_t recv(int fd, void *buf, size_t len, int flags)
{
	(void)flags;
	if (fd < 0) {
		errno = EBADF;
		return -1;
	}
	size_t limit = rt.in_len;
	if (rt.cut_idx < rt.ncuts && rt.cuts[rt.cut_idx] < limit)
		limit = rt.cuts[rt.cut_idx];
	for (int i = 0; i < rt.nmarkers; i++)
		if (rt.markers[i].off >= rt.in_pos && rt.markers[i].off < rt.in_pos + len && rt.markers[i].off < limit)
			materialize(&rt.markers[i]);
	size_t avail = limit - rt.in_pos;
	/* never serve an unmaterialised marker */
	for (int i = 0; i < rt.nmarkers; i++)
		if (!rt.markers[i].done && rt.markers[i].off >= rt.in_pos && rt.markers[i].off - rt.in_pos < avail)
			avail = rt.markers[i].off - rt.in_pos;
	if (!avail) {
		if (rt.empty_mode == 1)
			return 0;
		errno = rt.empty_mode == 2 ? ECONNRESET : EAGAIN;
		return -1;
	}
	size_t n = len < avail ? len : avail;
	if (rt.recv_cap && n > rt.recv_cap)
		n = rt.recv_cap;
	memcpy(buf, rt.in + rt.in_pos, n);
	rt.in_pos += n;
	return (ssize_t)n;
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
	(void)flags;
	if (fd < 0) {
		errno = EBADF;
		return -1;
	}
	if (rt.send_eintr_once) {
		rt.send_eintr_once = 0;
		errno = EINTR;
		return -1;
	}
	if (rt.send_fail_after >= 0 && (long)rt.conn_len >= rt.send_fail_after) {
		errno = rt.send_fail_errno;
		return -1;
	}
	append(&rt.out, &rt.out_len, buf, len);
	append(&rt.conn_out, &rt.conn_len, buf, len);
	return (ssize_t)len;
}

clock_t times(struct tms *t)
{
	memset(t, 0, sizeof *t);
	return (clock_t)(rt.clock_ms * (unsigned long)sysconf(_SC_CLK_TCK) / 1000);
}

/* conformance generator: 31-bit LCG, state = state * 1103515245 + 12345 mod 2^31, returns state */
int rand(void)
{
	rt.rand_state = (rt.rand_state * 1103515245u + 12345u) & 0x7fffffffu;
	return (int)rt.rand_state;
}

/* the fake sockets accept every socket option */
int setsockopt(int fd, int level, int name, const void *val, socklen_t len)
{
	(void)fd, (void)level, (void)name, (void)val, (void)len;
	return 0;
}

int mbedtls_x509_crt_parse_path(mbedtls_x509_crt *chain, const char *path)
{
	(void)chain, (void)path;
	return 0;
}

/* ---- happy eyeballs: every connect hands out one end of a fresh socketpair ---- */
int happy_eyeballs_create(struct happy_eyeballs_ctx **c)
{
	*c = calloc(1, sizeof **c);
	(*c)->fd = -1;
	return 0;
}

int happy_eyeballs_connect(struct happy_eyeballs_ctx *c, const char *host, int port)
{
	rt.connects++;
	strncpy(rt.last_host, host, sizeof(rt.last_host) - 1);
	rt.last_port = port;
	if (rt.hev_errno)
		return -1;
	int sv[2];
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv))
		return -1;
	c->fd = sv[0];
	if (rt.on_connect)
		rt.on_connect();
	if (rt.connects > 1 && rt.cut_idx < rt.ncuts)
		rt.in_pos = rt.cuts[rt.cut_idx++];
	rt_reset_conn();
	return 0;
}

int happy_eyeballs_set_bind_addr(struct happy_eyeballs_ctx *c, socklen_t l, const struct sockaddr_storage *a)
{
	(void)c, (void)l, (void)a;
	return 0;
}
int happy_eyeballs_timedwait_default(struct happy_eyeballs_ctx *c)
{
	(void)c;
	return 0;
}
int happy_eyeballs_get_error_code(const struct happy_eyeballs_ctx *c)
{
	(void)c;
	return rt.hev_errno;
}
int happy_eyeballs_get_remote_addr(const struct happy_eyeballs_ctx *c, struct sockaddr_storage *a)
{
	(void)c;
	memset(a, 0, sizeof *a);
	return 0;
}
uint64_t happy_eyeballs_get_connection_time_ns(const struct happy_eyeballs_ctx *c)
{
	(void)c;
	return 0;
}
int happy_eyeballs_get_socket_fd(const struct happy_eyeballs_ctx *c)
{
	return c->fd;
}
int happy_eyeballs_destroy(struct happy_eyeballs_ctx *c)
{
	free(c);
	return 0;
}
