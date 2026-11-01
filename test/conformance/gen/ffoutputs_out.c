/*
 * S-046 ffoutputs, ffmpeg_output side: the real plugins/obs-ffmpeg/obs-ffmpeg-output.c is #included (libav* are the real
 * system libraries; the "null" muxer with rawvideo / pcm_s16le is the target so that no file is touched). Commands "fo_*".
 * Hooks (macros applied after the libav headers): avformat_network_init() prints the received configuration,
 * av_interleaved_write_frame() can be forced to fail.
 */
#include <inttypes.h>
#include <errno.h>
#include <pthread.h>
#include <obs-module.h>
#include <util/threading.h>
#include <util/deque.h>
#include <util/darray.h>
#include <util/dstr.h>
#include <util/platform.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include "script.h"
#include "ffoutputs_api.h"

static void ffo_net_hook(void);
static int ffo_write_hook(AVFormatContext *s, AVPacket *pkt);
#define avformat_network_init() ffo_net_hook()
#define av_interleaved_write_frame ffo_write_hook
#define ffmpeg_data_init real_ffmpeg_data_init
#define ffmpeg_data_free real_ffmpeg_data_free
#define pthread_create ffo_pthread_create
#define pthread_join ffo_pthread_join
#include "../../../plugins/obs-ffmpeg/obs-ffmpeg-output.c"
#undef avformat_network_init
#undef av_interleaved_write_frame
#undef pthread_create
#undef pthread_join

static struct ffmpeg_output *OUT;
static int write_ret; /* forced result of av_interleaved_write_frame */
static bool write_forced;
static int writes;
static uint64_t vid_bytes_written; /* video packet bytes seen by the write hook (libav encoder dependent, never printed) */

static void print_cfg(const struct ffmpeg_cfg *c)
{
#define S(x) (c->x ? c->x : "(null)")
	printf("cfg url=%s format_name=%s mime=%s muxer_settings=\"%s\"\n", S(url), S(format_name), S(format_mime_type),
	       S(muxer_settings));
	printf("  gop=%d vbitrate=%d abitrate=%d venc=%s venc_id=%d aenc=%s aenc_id=%d\n", c->gop_size, c->video_bitrate,
	       c->audio_bitrate, S(video_encoder), c->video_encoder_id, S(audio_encoder), c->audio_encoder_id);
	printf("  vsettings=\"%s\" asettings=\"%s\" mix_count=%d tracks=0x%x\n", S(video_settings), S(audio_settings),
	       c->audio_mix_count, c->audio_tracks);
	printf("  pix_fmt=%s range=%d pri=%d trc=%d cs=%d\n", av_get_pix_fmt_name(c->format), (int)c->color_range,
	       (int)c->color_primaries, (int)c->color_trc, (int)c->colorspace);
	printf("  size=%dx%d scale=%dx%d\n", c->width, c->height, c->scale_width, c->scale_height);
	for (int i = 0; i < c->audio_mix_count; i++)
		printf("  audio_name[%d]=%s\n", i, c->audio_stream_names[i] ? c->audio_stream_names[i] : "(null)");
#undef S
}

static void ffo_net_hook(void)
{
	if (OUT)
		print_cfg(&OUT->ff_data.config);
}

static int ffo_write_hook(AVFormatContext *s, AVPacket *pkt)
{
	writes++;
	if (pkt->stream_index == 0)
		vid_bytes_written += (uint64_t)pkt->size;
	if (write_forced)
		return write_ret;
	return av_interleaved_write_frame(s, pkt);
}

extern struct obs_output_info ffmpeg_output;

static int64_t I(const char *s)
{
	return (int64_t)strtoll(s, NULL, 0);
}

void ffo_out_reset(void)
{
	vid_bytes_written = 0;
	OUT = NULL;
	write_forced = false;
	writes = 0;
}

static void print_state(void)
{
	if (!OUT) {
		printf("fo_state none\n");
		return;
	}
	struct ffmpeg_data *d = &OUT->ff_data;
	printf("fo_state active=%d connecting=%d stopping=%d write_thread_active=%d total_bytes_without_video=%" PRIu64 "\n",
	       (int)OUT->active, (int)OUT->connecting, (int)OUT->stopping, (int)OUT->write_thread_active,
	       OUT->total_bytes - vid_bytes_written);
	printf("  video_start_ts=%" PRIu64 " audio_start_ts=%" PRIu64 " stop_ts=%" PRIu64 " queued=%zu writes=%d\n",
	       OUT->video_start_ts, OUT->audio_start_ts, OUT->stop_ts, OUT->packets.num, writes);
	printf("  initialized=%d has_video=%d audio_streams=%d tracks=0x%x frame_size=%d total_frames=%" PRId64
	       " start_timestamp=%" PRIu64 "\n",
	       (int)d->initialized, d->video != NULL, d->num_audio_streams, d->audio_tracks, d->frame_size,
	       d->total_frames, d->start_timestamp);
	for (int i = 0; d->audio_infos && i < d->num_audio_streams && i < 6; i++)
		printf("  track[%d] total_samples=%" PRId64 " excess_bytes=%zu\n", i, d->total_samples[i],
		       d->excess_frames[i][0].size);
	printf("  audio_format=%d audio_planes=%zu audio_size=%zu samplerate=%u last_error=\"%s\"\n",
	       (int)d->audio_format, d->audio_planes, d->audio_size, d->audio_samplerate,
	       d->last_error ? d->last_error : "");
}

static void print_streams(void)
{
	struct ffmpeg_data *d = &OUT->ff_data;
	if (d->video)
		printf("  video: %dx%d pix_fmt=%s tb=%d/%d fps=%d/%d gop=%d bit_rate=%" PRId64 " range=%d pri=%d trc=%d cs=%d chroma=%d swscale=%d\n",
		       d->video_ctx->width, d->video_ctx->height,
		       av_get_pix_fmt_name(d->video_ctx->pix_fmt), d->video_ctx->time_base.num,
		       d->video_ctx->time_base.den, d->video_ctx->framerate.num, d->video_ctx->framerate.den,
		       d->video_ctx->gop_size, (int64_t)d->video_ctx->bit_rate, (int)d->video_ctx->color_range,
		       (int)d->video_ctx->color_primaries, (int)d->video_ctx->color_trc, (int)d->video_ctx->colorspace,
		       (int)d->video_ctx->chroma_sample_location, d->swscale != NULL);
	for (int i = 0; d->audio_infos && i < d->num_audio_streams; i++) {
		AVCodecContext *c = d->audio_infos[i].ctx;
		AVDictionaryEntry *t = av_dict_get(d->audio_infos[i].stream->metadata, "title", NULL, 0);
		printf("  audio[%d]: codec=%s rate=%d channels=%d sample_fmt=%s stream_index=%d title=%s\n",
		       i, d->acodec->name, c->sample_rate, c->ch_layout.nb_channels, av_get_sample_fmt_name(c->sample_fmt),
		       d->audio_infos[i].stream->index, t ? t->value : "(none)");
	}
}

static void print_packets(void)
{
	printf("fo_packets %zu\n", OUT->packets.num);
	for (size_t i = 0; i < OUT->packets.num; i++) {
		AVPacket *p = OUT->packets.array[i];
		printf("  [%zu] stream=%d pts=%" PRId64 " dts=%" PRId64 " dur=%" PRId64 " size=%s%d\n", i, p->stream_index,
		       p->pts, p->dts, p->stream_index ? p->duration : 0, p->stream_index ? "" : "(video)",
		       p->stream_index ? p->size : 0);
	}
}

static uint8_t *planes[MAX_AV_PLANES];
static int frame_no;

static void do_video(uint64_t ts)
{
	uint32_t w = ffo_O->width, h = ffo_O->height;
	struct video_data vd;
	memset(&vd, 0, sizeof(vd));
	size_t pw[4] = {0}, ph[4] = {0};
	int np = 0;
	switch (W.vformat) {
	case VIDEO_FORMAT_I420:
		np = 3; pw[0] = w; ph[0] = h; pw[1] = pw[2] = w / 2; ph[1] = ph[2] = h / 2;
		break;
	case VIDEO_FORMAT_NV12:
		np = 2; pw[0] = w; ph[0] = h; pw[1] = w; ph[1] = h / 2;
		break;
	case VIDEO_FORMAT_I444:
		np = 3; pw[0] = pw[1] = pw[2] = w; ph[0] = ph[1] = ph[2] = h;
		break;
	default:
		np = 1; pw[0] = w * 4; ph[0] = h;
		break;
	}
	for (int p = 0; p < np; p++) {
		planes[p] = malloc(pw[p] * ph[p]);
		for (size_t i = 0; i < pw[p] * ph[p]; i++)
			planes[p][i] = (uint8_t)(frame_no * 7 + p * 31 + i);
		vd.data[p] = planes[p];
		vd.linesize[p] = (uint32_t)pw[p];
	}
	vd.timestamp = ts;
	ffmpeg_output.raw_video(OUT, &vd);
	for (int p = 0; p < np; p++)
		free(planes[p]);
	frame_no++;
}

static void do_audio(size_t mix, size_t frames, uint64_t ts)
{
	struct audio_data ad;
	memset(&ad, 0, sizeof(ad));
	size_t planes_n = OUT->ff_data.audio_planes ? OUT->ff_data.audio_planes : 1;
	size_t bytes = frames * (OUT->ff_data.audio_size ? OUT->ff_data.audio_size : 4);
	static unsigned seq;
	for (size_t p = 0; p < planes_n; p++) {
		uint8_t *b = malloc(bytes ? bytes : 1);
		for (size_t i = 0; i < bytes; i++)
			b[i] = (uint8_t)(seq * 3 + p + i);
		ad.data[p] = b;
	}
	seq++;
	ad.frames = (uint32_t)frames;
	ad.timestamp = ts;
	ffmpeg_output.raw_audio2(OUT, mix, &ad);
	for (size_t p = 0; p < planes_n; p++)
		free(ad.data[p]);
}

void ffo_out_command(int argc, char **argv)
{
	const char *c = argv[0];
	(void)argc;
	if (!strcmp(c, "fo_create")) {
		ffo_thread_mode = 0;
		OUT = ffmpeg_output.create(ffo_O->settings, (obs_output_t *)ffo_O);
		av_log_set_callback(av_log_default_callback);
		av_log_set_level(AV_LOG_QUIET);
		printf("fo_create -> %s\n", OUT ? "ok" : "NULL");
	} else if (!strcmp(c, "fo_info")) {
		printf("info id=%s flags=0x%x\n", ffmpeg_output.id, ffmpeg_output.flags);
	} else if (!strcmp(c, "fo_start")) {
		ffo_thread_mode = 1;
		bool r = ffmpeg_output.start(OUT);
		printf("fo_start -> %d connecting=%d\n", (int)r, (int)OUT->connecting);
	} else if (!strcmp(c, "fo_connect")) {
		ffo_thread_mode = 2;
		ffo_run_pending();
		ffo_thread_mode = 0;
		printf("fo_connect done connecting=%d active=%d\n", (int)OUT->connecting, (int)OUT->active);
		if (OUT->ff_data.initialized)
			print_streams();
	} else if (!strcmp(c, "fo_pump")) {
		ffo_gate_pump((int)I(argv[1]));
		printf("fo_pump done queued=%zu total_bytes_without_video=%" PRIu64 " active=%d\n", OUT->packets.num,
		       OUT->total_bytes - vid_bytes_written, (int)OUT->active);
	} else if (!strcmp(c, "fo_stop")) {
		ffmpeg_output.stop(OUT, (uint64_t)I(argv[1]));
	} else if (!strcmp(c, "fo_destroy")) {
		ffo_thread_mode = 2;
		ffmpeg_output.destroy(OUT);
		OUT = NULL;
	} else if (!strcmp(c, "fo_video")) {
		do_video((uint64_t)I(argv[1]));
	} else if (!strcmp(c, "fo_audio")) {
		do_audio((size_t)I(argv[1]), (size_t)I(argv[2]), (uint64_t)I(argv[3]));
	} else if (!strcmp(c, "fo_state")) {
		print_state();
	} else if (!strcmp(c, "fo_packets")) {
		print_packets();
	} else if (!strcmp(c, "fo_write_ret")) {
		write_forced = strcmp(argv[1], "real") != 0;
		write_ret = write_forced ? (int)I(argv[1]) : 0;
	} else if (!strcmp(c, "fo_set")) {
		if (!strcmp(argv[1], "stopping"))
			OUT->stopping = I(argv[2]) != 0;
		else if (!strcmp(argv[1], "stop_ts"))
			OUT->stop_ts = (uint64_t)I(argv[2]);
		else if (!strcmp(argv[1], "pause_offset"))
			ffo_O->pause_offset = (uint64_t)I(argv[2]);
		else if (!strcmp(argv[1], "total_bytes"))
			OUT->total_bytes = (uint64_t)I(argv[2]);
	} else if (!strcmp(c, "fo_process")) {
		int r = process_packet(OUT);
		printf("fo_process -> %d total_bytes_without_video=%" PRIu64 " queued=%zu writes=%d\n", r,
		       OUT->total_bytes - vid_bytes_written, OUT->packets.num, writes);
	} else if (!strcmp(c, "fo_sys_dts")) {
		printf("fo_sys_dts -> %" PRIu64 "\n", get_packet_sys_dts(OUT, OUT->packets.array[I(argv[1])]));
	} else if (!strcmp(c, "fo_track_order")) {
		printf("fo_track_order mask=0x%x mix=%d -> %d\n", (int)I(argv[1]), (int)I(argv[2]),
		       get_track_order((int)I(argv[1]), (size_t)I(argv[2])));
	} else if (!strcmp(c, "fo_mix_count")) {
		printf("fo_mix_count mask=0x%x -> %d\n", (int)I(argv[1]), get_audio_mix_count((int)I(argv[1])));
	} else if (!strcmp(c, "fo_data_init")) {
		/* fo_data_init URL FORMAT|- MIME|- : direct call of the real ffmpeg_data_init with an otherwise empty config */
		struct ffmpeg_data d;
		struct ffmpeg_cfg cfg;
		memset(&cfg, 0, sizeof(cfg));
		cfg.url = strcmp(argv[1], "-") ? argv[1] : NULL;
		cfg.format_name = strcmp(argv[2], "-") ? argv[2] : NULL;
		cfg.format_mime_type = strcmp(argv[3], "-") ? argv[3] : NULL;
		cfg.muxer_settings = "";
		OUT = NULL;
		bool r = real_ffmpeg_data_init(&d, &cfg);
		printf("fo_data_init -> %d last_error=\"%s\"\n", (int)r, d.last_error ? d.last_error : "");
		real_ffmpeg_data_free(&d);
	} else if (!strcmp(c, "fo_formats")) {
		/* every OBS video format -> libav pixel format and chroma location (709 / BT2020_NCL) */
		static const struct {
			const char *n;
			int f;
		} t[] = {{"NONE", VIDEO_FORMAT_NONE}, {"I420", VIDEO_FORMAT_I420}, {"NV12", VIDEO_FORMAT_NV12},
			 {"YVYU", VIDEO_FORMAT_YVYU}, {"YUY2", VIDEO_FORMAT_YUY2}, {"UYVY", VIDEO_FORMAT_UYVY},
			 {"RGBA", VIDEO_FORMAT_RGBA}, {"BGRA", VIDEO_FORMAT_BGRA}, {"BGRX", VIDEO_FORMAT_BGRX},
			 {"Y800", VIDEO_FORMAT_Y800}, {"I444", VIDEO_FORMAT_I444}, {"BGR3", VIDEO_FORMAT_BGR3},
			 {"I422", VIDEO_FORMAT_I422}, {"I40A", VIDEO_FORMAT_I40A}, {"I42A", VIDEO_FORMAT_I42A},
			 {"YUVA", VIDEO_FORMAT_YUVA}, {"AYUV", VIDEO_FORMAT_AYUV}, {"I010", VIDEO_FORMAT_I010},
			 {"P010", VIDEO_FORMAT_P010}, {"I210", VIDEO_FORMAT_I210}, {"I412", VIDEO_FORMAT_I412},
			 {"YA2L", VIDEO_FORMAT_YA2L}, {"P216", VIDEO_FORMAT_P216}, {"P416", VIDEO_FORMAT_P416}};
		for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
			enum AVPixelFormat pf = obs_to_ffmpeg_video_format((enum video_format)t[i].f);
			printf("fo_format %s -> %s chroma709=%d chroma2020=%d\n", t[i].n,
			       pf == AV_PIX_FMT_NONE ? "none" : av_get_pix_fmt_name(pf),
			       (int)determine_chroma_location(pf, AVCOL_SPC_BT709),
			       (int)determine_chroma_location(pf, AVCOL_SPC_BT2020_NCL));
		}
	} else if (!strcmp(c, "fo_sample_formats")) {
		for (int f = -1; f <= AV_SAMPLE_FMT_NB; f++)
			printf("fo_sample_format %d -> audio_format %d\n", f,
			       (int)convert_ffmpeg_sample_format((enum AVSampleFormat)f));
	} else if (!strcmp(c, "fo_rescale")) {
		/* fo_rescale VAL CTX_NUM CTX_DEN NEW_NUM NEW_DEN */
		AVCodecContext ctx;
		memset(&ctx, 0, sizeof(ctx));
		ctx.time_base = (AVRational){(int)I(argv[2]), (int)I(argv[3])};
		printf("fo_rescale -> %" PRId64 "\n",
		       rescale_ts(I(argv[1]), &ctx, (AVRational){(int)I(argv[4]), (int)I(argv[5])}));
	} else {
		fprintf(stderr, "unknown command %s\n", c);
		exit(2);
	}
}
