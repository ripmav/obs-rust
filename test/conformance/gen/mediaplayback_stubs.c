/*
 * Conformance harness for spec S-036 (media playback): a scripted stand-in for the FFmpeg demuxer/decoder
 * entry points that shared/media-playback calls, plus a fake clock and a few libobs helpers.
 *
 * The real libavutil / libswscale / libavcodec packet helpers are linked; the functions that would open,
 * read, seek and decode a container are defined HERE (executable symbols win over the shared libraries), so
 * every packet, frame and error is dictated by the script. See test/conformance/README.md
 * ("mediaplayback runner") for the script language and the spec for the model.
 *
 * Test tooling, not part of libobs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdbool.h>
#include <errno.h>
#include <pthread.h>

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/samplefmt.h>
#include <libavutil/channel_layout.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/pixdesc.h>
#include <libavdevice/avdevice.h>

#include <obs.h>
#include <media-io/video-frame.h>
#include <stdarg.h>
#include "mediaplayback_api.h"

/* Output produced by library code (possibly on the media thread) is queued and printed by the main thread
 * (mpt_flush), so that vectors never depend on thread interleaving. */
static pthread_mutex_t out_lock = PTHREAD_MUTEX_INITIALIZER;
static char *out_buf;
static size_t out_len, out_cap;

void mpt_out(const char *fmt, ...)
{
	char tmp[2048];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
	va_end(ap);
	if (n < 0)
		return;
	if ((size_t)n >= sizeof(tmp))
		n = sizeof(tmp) - 1;
	pthread_mutex_lock(&out_lock);
	if (out_len + (size_t)n + 1 > out_cap) {
		out_cap = (out_len + (size_t)n + 1) * 2;
		out_buf = realloc(out_buf, out_cap);
	}
	memcpy(out_buf + out_len, tmp, (size_t)n);
	out_len += (size_t)n;
	pthread_mutex_unlock(&out_lock);
}

void mpt_flush(void)
{
	pthread_mutex_lock(&out_lock);
	if (out_len)
		fwrite(out_buf, 1, out_len, stdout);
	out_len = 0;
	pthread_mutex_unlock(&out_lock);
	fflush(stdout);
}

#define printf mpt_out

/* ------------------------------------------------------------------ clock */

static pthread_mutex_t clock_lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t fake_now;
int mp_trace_sleep = 1;

void mpt_set_clock(uint64_t ns)
{
	pthread_mutex_lock(&clock_lock);
	fake_now = ns;
	pthread_mutex_unlock(&clock_lock);
}

uint64_t os_gettime_ns(void)
{
	pthread_mutex_lock(&clock_lock);
	uint64_t v = fake_now;
	pthread_mutex_unlock(&clock_lock);
	return v;
}

void os_sleep_ms(uint32_t ms)
{
	pthread_mutex_lock(&clock_lock);
	fake_now += (uint64_t)ms * 1000000ULL;
	pthread_mutex_unlock(&clock_lock);
	if (mp_trace_sleep)
		printf("sleep %u\n", ms);
}

/* ------------------------------------------------- libobs helper stand-ins */

void obs_source_frame_init(struct obs_source_frame *frame, enum video_format format, uint32_t width, uint32_t height)
{
	struct video_frame vf;
	video_frame_init(&vf, format, width, height);
	frame->format = format;
	frame->width = width;
	frame->height = height;
	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		frame->data[i] = vf.data[i];
		frame->linesize[i] = vf.linesize[i];
	}
}

void obs_source_frame_copy(struct obs_source_frame *dst, const struct obs_source_frame *src)
{
	/* copy the pixel planes row by row, honouring both line sizes */
	struct video_frame d, s;
	for (size_t i = 0; i < MAX_AV_PLANES; i++) {
		d.data[i] = dst->data[i];
		d.linesize[i] = dst->linesize[i];
		s.data[i] = src->data[i];
		s.linesize[i] = src->linesize[i];
	}
	video_frame_copy(&d, &s, src->format, src->height);
	memcpy(dst->color_matrix, src->color_matrix, sizeof(dst->color_matrix));
	memcpy(dst->color_range_min, src->color_range_min, sizeof(dst->color_range_min));
	memcpy(dst->color_range_max, src->color_range_max, sizeof(dst->color_range_max));
	dst->full_range = src->full_range;
	dst->max_luminance = src->max_luminance;
	dst->flip = src->flip;
	dst->flags = src->flags;
	dst->trc = src->trc;
}

/* ------------------------------------------------------- scripted media */

struct mpt_media g_media[MPT_MAX_MEDIA];
int g_media_count;

struct mpt_media *mpt_find(const char *name)
{
	if (!name)
		return NULL;
	for (int i = 0; i < g_media_count; i++)
		if (!strcmp(g_media[i].name, name))
			return &g_media[i];
	return NULL;
}

/* One live "container" per open; the cursor and the opened definition. */
struct mpt_ctx {
	struct mpt_media *def;
	int cursor;
};

static AVInputFormat fake_formats[8];
static const char *fake_format_names[] = {"mpegts", "flv", "matroska", "mp4", NULL};
static AVCodec fake_codec;

void avdevice_register_all(void) {}
int avformat_network_init(void)
{
	return 0;
}

bool g_have_libvpx;

struct mpt_stream *mpt_stream_by_tag(uint32_t tag)
{
	return &g_media[tag >> 4].streams[(tag & 15) - 1];
}

struct mpt_media *mpt_stream_media(struct mpt_stream *s)
{
	for (int i = 0; i < g_media_count; i++)
		if (s >= g_media[i].streams && s < g_media[i].streams + MPT_MAX_STREAMS)
			return &g_media[i];
	return NULL;
}

const AVInputFormat *av_find_input_format(const char *short_name)
{
	for (int i = 0; fake_format_names[i]; i++) {
		if (!strcmp(short_name, fake_format_names[i])) {
			fake_formats[i].name = fake_format_names[i];
			return &fake_formats[i];
		}
	}
	return NULL;
}

AVFormatContext *avformat_alloc_context(void)
{
	AVFormatContext *c = av_mallocz(sizeof(*c));
	return c;
}

static void print_dict(AVDictionary *d)
{
	const AVDictionaryEntry *e = NULL;
	int first = 1;
	printf("[");
	while ((e = av_dict_iterate(d, e))) {
		printf("%s%s=%s", first ? "" : ";", e->key, e->value);
		first = 0;
	}
	printf("]");
}

int avformat_open_input(AVFormatContext **ps, const char *url, const AVInputFormat *fmt, AVDictionary **options)
{
	AVFormatContext *c = *ps;
	printf("ff open path=%s format=%s nobuffer=%d interrupt=%d opts=", url ? url : "-", fmt ? fmt->name : "-",
	       !!(c->flags & AVFMT_FLAG_NOBUFFER), c->interrupt_callback.callback != NULL);
	print_dict(options ? *options : NULL);
	printf("\n");

	struct mpt_media *def = mpt_find(url);
	if (!def || def->open_fail) {
		printf("ff open -> fail\n");
		av_free(c);
		*ps = NULL;
		return AVERROR(ENOENT);
	}

	struct mpt_ctx *x = av_mallocz(sizeof(*x));
	x->def = def;
	c->priv_data = x;
	c->duration = def->duration;
	c->start_time = def->start_time;
	c->nb_streams = (unsigned)def->nstreams;
	c->streams = av_calloc(def->nstreams ? def->nstreams : 1, sizeof(AVStream *));
	for (int i = 0; i < def->nstreams; i++) {
		struct mpt_stream *sd = &def->streams[i];
		AVStream *st = av_mallocz(sizeof(*st));
		st->index = i;
		st->time_base = (AVRational){sd->tb_num, sd->tb_den};
		st->nb_frames = sd->nb_frames;
		st->avg_frame_rate = (AVRational){sd->fps_num, sd->fps_den};
		st->codecpar = avcodec_parameters_alloc();
		st->codecpar->codec_type = sd->audio ? AVMEDIA_TYPE_AUDIO : AVMEDIA_TYPE_VIDEO;
		st->codecpar->codec_id = sd->codec_id;
		st->codecpar->codec_tag = (uint32_t)((def - g_media) << 4 | (i + 1)); /* our back reference to the definition */
		if (sd->alpha_mode)
			av_dict_set(&st->metadata, "alpha_mode", "1", 0);
		if (sd->cll) {
			size_t sz;
			AVContentLightMetadata *cl = av_content_light_metadata_alloc(&sz);
			cl->MaxCLL = (unsigned)sd->cll;
			AVPacketSideData *sdp = av_packet_side_data_new(&st->codecpar->coded_side_data,
									&st->codecpar->nb_coded_side_data,
									AV_PKT_DATA_CONTENT_LIGHT_LEVEL, sz, 0);
			memcpy(sdp->data, cl, sz);
			av_free(cl);
		}
		if (sd->mdcv) {
			AVPacketSideData *sdp = av_packet_side_data_new(&st->codecpar->coded_side_data,
									&st->codecpar->nb_coded_side_data,
									AV_PKT_DATA_MASTERING_DISPLAY_METADATA,
									sizeof(AVMasteringDisplayMetadata), 0);
			AVMasteringDisplayMetadata md = {0};
			md.has_luminance = 1;
			md.max_luminance = (AVRational){sd->mdcv * 10 + 5, 10}; /* x.5 to show the +0.5 rounding */
			memcpy(sdp->data, &md, sizeof(md));
		}
		c->streams[i] = st;
	}
	return 0;
}

int avformat_find_stream_info(AVFormatContext *ic, AVDictionary **options)
{
	(void)options;
	struct mpt_ctx *x = ic->priv_data;
	return x->def->info_fail ? AVERROR_INVALIDDATA : 0;
}

void avformat_close_input(AVFormatContext **ps)
{
	AVFormatContext *c = *ps;
	if (!c)
		return;
	struct mpt_ctx *x = c->priv_data;
	for (unsigned i = 0; i < c->nb_streams; i++) {
		AVStream *st = c->streams[i];
		av_dict_free(&st->metadata);
		avcodec_parameters_free(&st->codecpar);
		av_free(st);
	}
	av_free(c->streams);
	av_free(x);
	av_free(c);
	*ps = NULL;
}

int av_find_best_stream(AVFormatContext *ic, enum AVMediaType type, int wanted, int related, const AVCodec **dec,
			int flags)
{
	(void)wanted;
	(void)related;
	(void)dec;
	(void)flags;
	for (unsigned i = 0; i < ic->nb_streams; i++)
		if (ic->streams[i]->codecpar->codec_type == type)
			return (int)i;
	return AVERROR_STREAM_NOT_FOUND;
}

int av_read_frame(AVFormatContext *s, AVPacket *pkt)
{
	struct mpt_ctx *x = s->priv_data;
	struct mpt_media *def = x->def;
	if (x->cursor >= def->npkts)
		return AVERROR_EOF;
	struct mpt_pkt *p = &def->pkts[x->cursor];
	int idx = x->cursor++;
	if (p->rderr) {
		printf("ff read #%d -> error %d\n", idx, p->rderr);
		return p->rderr;
	}
	av_new_packet(pkt, p->size);
	if (p->size)
		memset(pkt->data, 0xA5, (size_t)p->size);
	pkt->stream_index = p->stream;
	pkt->pts = p->nopts ? AV_NOPTS_VALUE : p->pts;
	pkt->dts = pkt->pts;
	pkt->pos = idx; /* back reference used by the fake decoder */
	pkt->flags = p->key ? AV_PKT_FLAG_KEY : 0;
	printf("ff read #%d stream=%d pts=%lld size=%d\n", idx, p->stream, (long long)pkt->pts, p->size);
	return 0;
}

int av_seek_frame(AVFormatContext *s, int stream_index, int64_t timestamp, int flags)
{
	struct mpt_ctx *x = s->priv_data;
	struct mpt_media *def = x->def;
	printf("ff seek stream=%d target=%lld flags=%s\n", stream_index, (long long)timestamp,
	       flags & AVSEEK_FLAG_BACKWARD ? "BACKWARD" : (flags & AVSEEK_FLAG_FRAME ? "FRAME" : "NONE"));
	if (def->seek_fail)
		return AVERROR(EIO);

	/* Fake semantics: BACKWARD: restart at the last key packet of the stream whose pts <= target (pts in the
	 * stream time base); FRAME: restart at the target-th packet of that stream. Otherwise the beginning. */
	int pos = 0;
	int n = 0;
	for (int i = 0; i < def->npkts; i++) {
		struct mpt_pkt *p = &def->pkts[i];
		if (p->rderr || p->stream != stream_index)
			continue;
		if (flags & AVSEEK_FLAG_FRAME) {
			if (n == timestamp)
				pos = i;
			n++;
		} else if (p->key && !p->nopts && p->pts <= timestamp) {
			pos = i;
		}
	}
	x->cursor = pos;
	return 0;
}

/* ------------------------------------------------------------ fake codec */

struct mpt_dec {
	struct mpt_stream *sd;
	int queue[MPT_MAX_PKTS];
	int qn;
	bool draining;
	uint8_t *vbuf;
	uint8_t *abuf;
};

const AVCodec *avcodec_find_decoder(enum AVCodecID id)
{
	if (id == AV_CODEC_ID_NONE) {
		printf("ff find_decoder none -> null\n");
		return NULL;
	}
	return &fake_codec;
}

const AVCodec *avcodec_find_decoder_by_name(const char *name)
{
	bool have = g_have_libvpx;
	printf("ff find_decoder_by_name %s -> %s\n", name, have ? "found" : "null");
	return have ? &fake_codec : NULL;
}

AVCodecContext *avcodec_alloc_context3(const AVCodec *codec)
{
	(void)codec;
	return av_mallocz(sizeof(AVCodecContext));
}

int avcodec_parameters_to_context(AVCodecContext *c, const AVCodecParameters *par)
{
	struct mpt_dec *d = av_mallocz(sizeof(*d));
	/* codec_tag was set to index+1 at open; find the owning media via the global "current open" */
	d->sd = mpt_stream_by_tag(par->codec_tag);
	c->priv_data = d;
	c->codec_type = par->codec_type;
	c->codec_id = par->codec_id;
	c->time_base = (AVRational){d->sd->dectb_num, d->sd->dectb_den};
	return 0;
}

int avcodec_open2(AVCodecContext *c, const AVCodec *codec, AVDictionary **options)
{
	(void)codec;
	(void)options;
	struct mpt_dec *d = c->priv_data;
	return d->sd->open_fail ? AVERROR_INVALIDDATA : 0;
}

void avcodec_free_context(AVCodecContext **pc)
{
	if (!pc || !*pc)
		return;
	struct mpt_dec *d = (*pc)->priv_data;
	if (d) {
		if (d->vbuf)
			av_free(d->vbuf);
		if (d->abuf)
			av_free(d->abuf);
		av_free(d);
	}
	av_free(*pc);
	*pc = NULL;
}

void avcodec_flush_buffers(AVCodecContext *c)
{
	struct mpt_dec *d = c->priv_data;
	printf("ff decoder flush\n");
	d->qn = 0;
	d->draining = false;
}

int avcodec_send_packet(AVCodecContext *c, const AVPacket *pkt)
{
	struct mpt_dec *d = c->priv_data;
	if (!pkt || (!pkt->data && !pkt->size)) {
		if (d->draining)
			return AVERROR_EOF;
		d->draining = true;
		printf("ff decoder drain\n");
		return 0;
	}
	if (d->draining)
		return AVERROR_EOF;
	struct mpt_media *def = mpt_stream_media(d->sd);
	struct mpt_pkt *p = &def->pkts[pkt->pos];
	if (p->decerr) {
		printf("ff decoder send #%d -> error\n", (int)pkt->pos);
		return AVERROR_INVALIDDATA;
	}
	d->queue[d->qn++] = (int)pkt->pos;
	return 0;
}

static void fill_video(struct mpt_dec *d, AVFrame *f, struct mpt_pkt *p, int idx)
{
	struct mpt_stream *sd = d->sd;
	int linesz[4] = {0};
	if (d->vbuf)
		av_free(d->vbuf);
	d->vbuf = NULL;
	f->width = sd->w;
	f->height = sd->h;
	f->format = sd->pix_fmt;
	f->colorspace = sd->cs;
	f->color_range = sd->range;
	f->color_trc = sd->trc;
	f->color_primaries = sd->pri;
	if (sd->w <= 0 || sd->h <= 0)
		return; /* degenerate frame: no pixel data */
	int size = av_image_get_buffer_size(sd->pix_fmt, sd->w, sd->h, 1);
	d->vbuf = av_malloc((size_t)size);
	for (int i = 0; i < size; i++)
		d->vbuf[i] = (uint8_t)(idx * 16 + 1);
	uint8_t *data[4];
	av_image_fill_linesizes(linesz, sd->pix_fmt, sd->w);
	av_image_fill_pointers(data, sd->pix_fmt, sd->h, d->vbuf, linesz);
	for (int i = 0; i < 4; i++) {
		f->data[i] = data[i];
		f->linesize[i] = linesz[i];
	}
	if (sd->flip) {
		/* bottom-up image: pointer to the last row, negative stride (only meaningful for packed formats) */
		f->data[0] = d->vbuf + (size_t)linesz[0] * (size_t)(sd->h - 1);
		f->linesize[0] = -linesz[0];
	}
	(void)p;
}

static void fill_audio(struct mpt_dec *d, AVFrame *f, struct mpt_pkt *p, int idx)
{
	struct mpt_stream *sd = d->sd;
	if (d->abuf)
		av_free(d->abuf);
	d->abuf = NULL;
	int ns = p->samples ? p->samples : 1024;
	int bps = av_get_bytes_per_sample(sd->sample_fmt);
	int planar = av_sample_fmt_is_planar(sd->sample_fmt);
	int planes = planar ? sd->channels : 1;
	int plane_size = planar ? bps * ns : bps * ns * sd->channels;
	d->abuf = av_malloc((size_t)(planes * plane_size));
	for (int i = 0; i < planes * plane_size; i++)
		d->abuf[i] = (uint8_t)(idx * 16 + 2);
	for (int i = 0; i < planes && i < AV_NUM_DATA_POINTERS; i++)
		f->data[i] = d->abuf + (size_t)i * plane_size;
	f->extended_data = f->data;
	f->nb_samples = ns;
	f->format = sd->sample_fmt;
	f->sample_rate = sd->sample_rate;
	av_channel_layout_default(&f->ch_layout, sd->channels);
}

int avcodec_receive_frame(AVCodecContext *c, AVFrame *f)
{
	struct mpt_dec *d = c->priv_data;
	bool emit = d->qn > d->sd->latency || (d->draining && d->qn > 0);
	av_frame_unref(f);
	if (!emit)
		return d->draining ? AVERROR_EOF : AVERROR(EAGAIN);

	int idx = d->queue[0];
	memmove(d->queue, d->queue + 1, (size_t)(d->qn - 1) * sizeof(int));
	d->qn--;
	struct mpt_media *def = mpt_stream_media(d->sd);
	struct mpt_pkt *p = &def->pkts[idx];

	if (d->sd->audio)
		fill_audio(d, f, p, idx);
	else
		fill_video(d, f, p, idx);
	f->best_effort_timestamp = (p->nopts_bet || (p->nopts && !p->has_bet)) ? AV_NOPTS_VALUE : (p->has_bet ? p->bet : p->pts);
	f->pts = f->best_effort_timestamp;
	f->duration = p->fdur;
	if (p->key)
		f->flags |= AV_FRAME_FLAG_KEY;
	return 0;
}
