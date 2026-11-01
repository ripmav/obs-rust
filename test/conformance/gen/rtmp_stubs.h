/* Fake transport / clock / random for the rtmp runner (spec S-031). Test tooling. */
#pragma once
#include <stddef.h>
#include <stdint.h>

struct rt_marker {
	size_t off;
	int kind; /* 1 = S2 echo of C1, 2 = S2 digest reply (alg in arg) */
	int arg;
	int done;
};

struct rt_fake {
	uint8_t *in;
	size_t in_len, in_pos;
	struct rt_marker markers[16];
	int nmarkers;
	size_t recv_cap;  /* 0 = unlimited bytes per recv call */
	int empty_mode;   /* 0 EAGAIN (timeout), 1 EOF, 2 ECONNRESET */
	uint8_t *out;     /* bytes sent since last runner drain */
	size_t out_len;
	uint8_t *conn_out; /* all bytes sent on the current connection */
	size_t conn_len;
	long send_fail_after; /* -1 none; fail every send once conn_len >= this */
	int send_fail_errno;
	int send_eintr_once;
	long long total_sent;
	unsigned long clock_ms;
	uint32_t rand_state;
	size_t cuts[8];   /* recv never serves across a cut; a new connection skips to the cut */
	int ncuts, cut_idx;
	void (*on_connect)(void);
	int hev_errno;    /* 0 = connect succeeds */
	int connects;
	char last_host[64];
	int last_port;
};
extern struct rt_fake rt;
void rt_feed(const uint8_t *d, size_t n);
void rt_feed_marker(int kind, int arg);
void rt_reset_conn(void);
