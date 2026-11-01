/*
 * Conformance harness (area encopts): wrapper around the real
 * plugins/obs-ffmpeg/obs-ffmpeg-audio-encoders.c (create-time codec configuration) with
 * recording fakes of the libavcodec/libavutil functions it calls. Test tooling.
 */
#include "../../../plugins/obs-ffmpeg/obs-ffmpeg-audio-encoders.c"
#include "encopts_api.h"

/* ---- fake FFmpeg ------------------------------------------------------- */

#define MAXC 16
static struct {
	const char *name;
	int present, lossless;
	enum AVCodecID id;
} codecs[MAXC] = {{"aac", 1, 0, AV_CODEC_ID_AAC},      {"libopus", 1, 0, AV_CODEC_ID_OPUS},
		  {"opus", 1, 0, AV_CODEC_ID_OPUS},    {"pcm_s16le", 1, 1, AV_CODEC_ID_PCM_S16LE},
		  {"pcm_s24le", 1, 1, AV_CODEC_ID_PCM_S24LE}, {"pcm_f32le", 1, 1, AV_CODEC_ID_PCM_F32LE},
		  {"alac", 1, 1, AV_CODEC_ID_ALAC},    {"flac", 1, 1, AV_CODEC_ID_FLAC}};
static AVCodec fake_codec[MAXC];
static enum AVSampleFormat fmt_list[16];
static int fmt_set;
static int rate_list[16];
static int rate_set;

void audw_codec(const char *name, int present)
{
	for (int i = 0; i < MAXC; i++)
		if (codecs[i].name && !strcmp(codecs[i].name, name))
			codecs[i].present = present;
}

void audw_codec_lossless(const char *name, int v)
{
	for (int i = 0; i < MAXC; i++)
		if (codecs[i].name && !strcmp(codecs[i].name, name))
			codecs[i].lossless = v;
}

static const struct {
	const char *n;
	enum AVSampleFormat f;
} sfmts[] = {{"u8", AV_SAMPLE_FMT_U8},   {"s16", AV_SAMPLE_FMT_S16},   {"s32", AV_SAMPLE_FMT_S32},
	     {"flt", AV_SAMPLE_FMT_FLT},  {"u8p", AV_SAMPLE_FMT_U8P},   {"s16p", AV_SAMPLE_FMT_S16P},
	     {"s32p", AV_SAMPLE_FMT_S32P}, {"fltp", AV_SAMPLE_FMT_FLTP}, {"dbl", AV_SAMPLE_FMT_DBL}};

/* "none" = codec reports no list (NULL); otherwise a comma list of format names. */
void audw_formats(const char *list)
{
	fmt_set = 0;
	if (!strcmp(list, "none"))
		return;
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", list);
	int n = 0;
	for (char *t = strtok(buf, ","); t && n < 15; t = strtok(NULL, ",")) {
		for (size_t i = 0; i < sizeof(sfmts) / sizeof(*sfmts); i++)
			if (!strcmp(sfmts[i].n, t))
				fmt_list[n++] = sfmts[i].f;
	}
	fmt_list[n] = AV_SAMPLE_FMT_NONE;
	fmt_set = 1;
}

void audw_rates(const char *list)
{
	rate_set = 0;
	if (!strcmp(list, "none"))
		return;
	char buf[128];
	snprintf(buf, sizeof(buf), "%s", list);
	int n = 0;
	for (char *t = strtok(buf, ","); t && n < 15; t = strtok(NULL, ","))
		rate_list[n++] = atoi(t);
	rate_list[n] = 0;
	rate_set = 1;
}

const AVCodec *avcodec_find_encoder_by_name(const char *name)
{
	for (int i = 0; i < MAXC; i++)
		if (codecs[i].name && !strcmp(codecs[i].name, name)) {
			printf("av.find_encoder %s -> %s\n", name, codecs[i].present ? "found" : "missing");
			if (!codecs[i].present)
				return NULL;
			fake_codec[i].name = codecs[i].name;
			fake_codec[i].id = codecs[i].id;
			return &fake_codec[i];
		}
	printf("av.find_encoder %s -> missing\n", name);
	return NULL;
}

const AVCodecDescriptor *avcodec_descriptor_get(enum AVCodecID id)
{
	static AVCodecDescriptor d;
	d.id = id;
	d.props = 0;
	for (int i = 0; i < MAXC; i++)
		if (codecs[i].name && codecs[i].id == id && codecs[i].lossless)
			d.props = AV_CODEC_PROP_LOSSLESS;
	return &d;
}

AVCodecContext *avcodec_alloc_context3(const AVCodec *codec)
{
	(void)codec;
	AVCodecContext *c = calloc(1, sizeof(*c));
	c->sample_fmt = AV_SAMPLE_FMT_NONE;
	return c;
}

void avcodec_free_context(AVCodecContext **c)
{
	free(*c);
	*c = NULL;
}

AVFrame *av_frame_alloc(void)
{
	return calloc(1, sizeof(AVFrame));
}

void av_frame_free(AVFrame **f)
{
	free(*f);
	*f = NULL;
}

void av_freep(void *p)
{
	void **pp = p;
	free(*pp);
	*pp = NULL;
}

int avcodec_get_supported_config(const AVCodecContext *ctx, const AVCodec *codec, enum AVCodecConfig config,
				 unsigned flags, const void **out, int *n)
{
	(void)ctx, (void)codec, (void)flags, (void)n;
	*out = NULL;
	if (config == AV_CODEC_CONFIG_SAMPLE_FORMAT && fmt_set)
		*out = fmt_list;
	else if (config == AV_CODEC_CONFIG_SAMPLE_RATE && rate_set)
		*out = rate_list;
	return 0;
}

int avcodec_open2(AVCodecContext *ctx, const AVCodec *codec, AVDictionary **opts)
{
	(void)opts;
	printf("av.open2 codec=%s bit_rate=%lld sample_rate=%d channels=%d sample_fmt=%d strict=%d flags=%d\n",
	       codec->name, (long long)ctx->bit_rate, ctx->sample_rate, ctx->ch_layout.nb_channels, (int)ctx->sample_fmt,
	       ctx->strict_std_compliance, ctx->flags);
	if (g_host.ff_open_fail)
		return -1;
	ctx->frame_size = strncmp(codec->name, "pcm", 3) == 0 ? 0 : 1024;
	return 0;
}

int av_samples_alloc(uint8_t **data, int *linesize, int channels, int nb_samples, enum AVSampleFormat fmt, int align)
{
	(void)linesize, (void)align;
	printf("av.samples_alloc channels=%d nb_samples=%d fmt=%d\n", channels, nb_samples, (int)fmt);
	data[0] = calloc(1, 16);
	return 16;
}

/* Default layout for N channels, as libavutil does (FFmpeg >= 5.1). */
void av_channel_layout_default(AVChannelLayout *l, int n)
{
	static const AVChannelLayout t[] = {AV_CHANNEL_LAYOUT_MONO,      AV_CHANNEL_LAYOUT_STEREO,
					    AV_CHANNEL_LAYOUT_SURROUND,   AV_CHANNEL_LAYOUT_4POINT0,
					    AV_CHANNEL_LAYOUT_5POINT0,    AV_CHANNEL_LAYOUT_5POINT1,
					    AV_CHANNEL_LAYOUT_6POINT1,    AV_CHANNEL_LAYOUT_7POINT1};
	memset(l, 0, sizeof(*l));
	if (n >= 1 && n <= 8)
		*l = t[n - 1];
	else {
		l->order = AV_CHANNEL_ORDER_UNSPEC;
		l->nb_channels = n;
	}
}

int av_channel_layout_describe(const AVChannelLayout *l, char *buf, size_t n)
{
	return snprintf(buf, n, "layout-%d-%llx", l->nb_channels, (unsigned long long)l->u.mask);
}

int av_strerror(int e, char *buf, size_t n)
{
	return snprintf(buf, n, "error %d", e);
}

/* ---- wrappers ----------------------------------------------------------- */

void audw_defaults(obs_data_t *s)
{
	enc_defaults(s);
}

obs_properties_t *audw_props(void)
{
	return enc_properties(NULL);
}

void audw_create(const char *kind, obs_data_t *s)
{
	static const struct {
		const char *k;
		void *(*f)(obs_data_t *, obs_encoder_t *);
	} tab[] = {{"aac", aac_create},     {"opus", opus_create}, {"pcm", pcm_create},    {"pcm24", pcm24_create},
		   {"pcm32", pcm32_create}, {"alac", alac_create}, {"flac", flac_create}};
	for (size_t i = 0; i < sizeof(tab) / sizeof(*tab); i++) {
		if (strcmp(tab[i].k, kind))
			continue;
		struct enc_encoder *e = tab[i].f(s, (obs_encoder_t *)&g_host);
		if (!e) {
			printf("aud_create %s null\n", kind);
			return;
		}
		AVCodecContext *c = e->context;
		printf("aud_create %s ok type=%s\n", kind, e->type);
		printf("aud state bit_rate=%lld sample_rate=%d channels=%d order=%d mask=0x%llx sample_fmt=%d\n",
		       (long long)c->bit_rate, c->sample_rate, c->ch_layout.nb_channels, (int)c->ch_layout.order,
		       (unsigned long long)(c->ch_layout.order == AV_CHANNEL_ORDER_NATIVE ? c->ch_layout.u.mask : 0),
		       (int)c->sample_fmt);
		printf("aud state frame_size=%d frame_size_bytes=%d planes=%d audio_size=%d strict=%d flags=%d\n",
		       e->frame_size, e->frame_size_bytes, (int)e->audio_planes, (int)e->audio_size,
		       c->strict_std_compliance, c->flags);
		enc_destroy(e);
		return;
	}
	fprintf(stderr, "script: unknown aud kind %s\n", kind);
	exit(2);
}
