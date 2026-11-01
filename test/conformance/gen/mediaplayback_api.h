/*
 * Shared declarations between mediaplayback_runner.c and mediaplayback_stubs.c (spec S-036 conformance harness).
 * Test tooling, not part of libobs.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <libavutil/pixfmt.h>
#include <libavutil/samplefmt.h>
#include <libavcodec/codec_id.h>

#define MPT_MAX_MEDIA 64
#define MPT_MAX_PKTS 128
#define MPT_MAX_STREAMS 4

struct mpt_stream {
	bool audio;
	int tb_num, tb_den;
	int dectb_num, dectb_den; /* decoder context time base (used by the duration estimate) */
	int w, h;
	enum AVPixelFormat pix_fmt;
	enum AVSampleFormat sample_fmt;
	int sample_rate, channels;
	int64_t nb_frames;
	int fps_num, fps_den;
	int cs, range, trc, pri; /* AVColorSpace / AVColorRange / AVColorTransferCharacteristic / AVColorPrimaries */
	enum AVCodecID codec_id;
	int alpha_mode;
	int cll, mdcv;
	int latency; /* number of packets the fake decoder holds back */
	bool flip;
	bool open_fail;
};

struct mpt_pkt {
	int stream;
	int64_t pts;
	int size;
	bool nopts;
	bool key;
	int64_t fdur; /* AVFrame.duration (stream time base) the decoder reports; 0 = unknown */
	bool has_bet, nopts_bet;
	int64_t bet; /* best_effort_timestamp override */
	int samples;
	bool decerr; /* avcodec_send_packet fails for this packet */
	int rderr;   /* when non-zero this entry is a demuxer error (value returned by av_read_frame) */
};

struct mpt_media {
	char name[64];
	int64_t duration;   /* AVFormatContext.duration (microseconds) or AV_NOPTS_VALUE */
	int64_t start_time; /* AVFormatContext.start_time or AV_NOPTS_VALUE */
	int nstreams;
	struct mpt_stream streams[MPT_MAX_STREAMS];
	int npkts;
	struct mpt_pkt pkts[MPT_MAX_PKTS];
	bool open_fail, info_fail, seek_fail;
};

extern struct mpt_media g_media[MPT_MAX_MEDIA];
extern int g_media_count;
extern bool g_have_libvpx;
extern int mp_trace_sleep;

struct mpt_media *mpt_find(const char *name);
struct mpt_stream *mpt_stream_by_tag(uint32_t tag);
struct mpt_media *mpt_stream_media(struct mpt_stream *s);
void mpt_set_clock(uint64_t ns);
void mpt_out(const char *fmt, ...);
void mpt_flush(void);
