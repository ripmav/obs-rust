/*
 * Conformance harness (area ffsources, spec S-047): wrapper around the real
 * plugins/obs-ffmpeg/obs-ffmpeg-audio-encoders.c (create, encode, info callbacks and the
 * registration tables). The plugin file is #included so that its static functions can be called.
 * Test tooling.
 */
#include "../../../plugins/obs-ffmpeg/obs-ffmpeg-audio-encoders.c"
#include "ffsources_api.h"
#include "script.h"

static const struct {
	const char *k;
	struct obs_encoder_info *info;
	void *(*create)(obs_data_t *, obs_encoder_t *);
} tab[] = {{"aac", &aac_encoder_info, aac_create},       {"opus", &opus_encoder_info, opus_create},
	   {"pcm", &pcm_encoder_info, pcm_create},       {"pcm24", &pcm24_encoder_info, pcm24_create},
	   {"pcm32", &pcm32_encoder_info, pcm32_create}, {"alac", &alac_encoder_info, alac_create},
	   {"flac", &flac_encoder_info, flac_create}};

static const struct obs_encoder_info *find(const char *kind)
{
	for (size_t i = 0; i < sizeof(tab) / sizeof(*tab); i++)
		if (!strcmp(tab[i].k, kind))
			return tab[i].info;
	fprintf(stderr, "script: unknown audio kind %s\n", kind);
	exit(2);
}

void *fsa_create(const char *kind, obs_data_t *s)
{
	for (size_t i = 0; i < sizeof(tab) / sizeof(*tab); i++)
		if (!strcmp(tab[i].k, kind)) {
			void *e = tab[i].create(s, (obs_encoder_t *)&g_host);
			printf("audio_create %s %s\n", kind, e ? "ok" : "null");
			return e;
		}
	find(kind);
	return NULL;
}

void fsa_destroy(void *h)
{
	enc_destroy(h);
}

void fsa_dump(void *h)
{
	struct enc_encoder *e = h;
	AVCodecContext *c = e->context;
	printf("audio type=%s bit_rate=%lld sample_rate=%d channels=%d sample_fmt=%d frame_size=%d "
	       "frame_size_bytes=%d planes=%d audio_size=%d total_samples=%lld time_base=%d/%d\n",
	       e->type, (long long)c->bit_rate, c->sample_rate, c->ch_layout.nb_channels, (int)c->sample_fmt,
	       e->frame_size, e->frame_size_bytes, (int)e->audio_planes, (int)e->audio_size,
	       (long long)e->total_samples, c->time_base.num, c->time_base.den);
}

void fsa_encode(void *h, int fill)
{
	struct enc_encoder *e = h;
	struct encoder_frame frame;
	struct encoder_packet packet;
	bool received = false;
	memset(&frame, 0, sizeof(frame));
	memset(&packet, 0, sizeof(packet));
	for (size_t p = 0; p < e->audio_planes; p++) {
		uint8_t *d = malloc((size_t)e->frame_size_bytes);
		for (int j = 0; j < e->frame_size_bytes; j++)
			d[j] = (uint8_t)(fill + (int)p * 16 + (j & 15));
		frame.data[p] = d;
	}
	frame.frames = (uint32_t)e->frame_size;
	bool r = enc_encode(e, &frame, &packet, &received);
	printf("encode -> %d received=%d", r, received);
	if (r && received) {
		printf(" pts=%lld dts=%lld size=%zu type=%d keyframe=%d timebase=%d/%d data=", (long long)packet.pts,
		       (long long)packet.dts, packet.size, (int)packet.type, packet.keyframe, packet.timebase_num,
		       packet.timebase_den);
		script_print_hex(packet.data, packet.size > 8 ? 8 : packet.size);
	}
	putchar('\n');
	for (size_t p = 0; p < e->audio_planes; p++)
		free((void *)frame.data[p]);
}

void fsa_table(void)
{
	for (size_t i = 0; i < sizeof(tab) / sizeof(*tab); i++) {
		const struct obs_encoder_info *n = tab[i].info;
		printf("encoder %s id=%s type=%d codec=%s name=%s priming=%d audio_info=%s extra_data=%d frame_size=%d "
		       "create=%d destroy=%d\n",
		       tab[i].k, n->id, (int)n->type, n->codec, n->get_name(NULL), n->get_priming_samples != NULL,
		       n->get_audio_info == enc_audio_info_float ? "float" : "plain", n->get_extra_data != NULL,
		       n->get_frame_size != NULL, n->create != NULL, n->destroy != NULL);
	}
}

void fsa_info(const char *kind, void *h)
{
	const struct obs_encoder_info *n = find(kind);
	struct audio_convert_info info;
	memset(&info, 0, sizeof(info));
	n->get_audio_info(h, &info);
	printf("info %s format=%d samples_per_sec=%u speakers=%d allow_clipping=%d\n", kind, (int)info.format,
	       info.samples_per_sec, (int)info.speakers, info.allow_clipping);
	uint8_t *ed = NULL;
	size_t sz = 99;
	bool r = n->get_extra_data(h, &ed, &sz);
	printf("info %s extra_data=%d size=%zu", kind, r, sz);
	if (sz && ed) {
		putchar(' ');
		script_print_hex(ed, sz);
	}
	putchar('\n');
	printf("info %s frame_size=%zu priming=", kind, n->get_frame_size(h));
	if (n->get_priming_samples)
		printf("%u\n", n->get_priming_samples(h));
	else
		printf("none\n");
}

void fsa_defaults(const char *kind, obs_data_t *s)
{
	find(kind)->get_defaults(s);
}
obs_properties_t *fsa_props(const char *kind)
{
	return find(kind)->get_properties(NULL);
}
