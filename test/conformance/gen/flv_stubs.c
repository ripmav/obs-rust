/*
 * Conformance harness (area flv, spec S-021): fake libobs objects for flv-mux.c.
 *
 * flv_meta_data() asks the output for its encoders, and the encoders for codec, size,
 * settings and the audio/video output. The functions below answer from the scripted
 * values in struct flv_fake (set by the runner's "cfg" command). Every stub is an INPUT of
 * the muxer and is documented as such in spec S-021 section 3.3.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "obs.h"

struct flv_fake {
	char vcodec[32];
	uint32_t width, height;
	double vbitrate;
	double fps;
	double abitrate;
	uint32_t arate;
	size_t channels;
	char version[8192];
};

struct flv_fake flv_fake = {
	.vcodec = "h264",
	.width = 1920,
	.height = 1080,
	.vbitrate = 2500.0,
	.fps = 60.0,
	.abitrate = 160.0,
	.arate = 48000,
	.channels = 2,
	.version = "0.0.0-conformance",
};

/* the addresses are the object identities; the contents are never read */
static char the_output, the_venc, the_aenc, the_video, the_audio, the_vsettings, the_asettings;

obs_encoder_t *obs_output_get_video_encoder(const obs_output_t *output)
{
	(void)output;
	return (obs_encoder_t *)&the_venc;
}

obs_encoder_t *obs_output_get_audio_encoder(const obs_output_t *output, size_t idx)
{
	(void)output, (void)idx;
	return (obs_encoder_t *)&the_aenc;
}

video_t *obs_encoder_video(const obs_encoder_t *encoder)
{
	(void)encoder;
	return (video_t *)&the_video;
}

audio_t *obs_encoder_audio(const obs_encoder_t *encoder)
{
	(void)encoder;
	return (audio_t *)&the_audio;
}

obs_data_t *obs_encoder_get_settings(const obs_encoder_t *encoder)
{
	return (obs_data_t *)(encoder == (const obs_encoder_t *)&the_venc ? &the_vsettings : &the_asettings);
}

double obs_data_get_double(obs_data_t *data, const char *name)
{
	if (!strcmp(name, "bitrate"))
		return data == (obs_data_t *)&the_vsettings ? flv_fake.vbitrate : flv_fake.abitrate;
	return 0.0;
}

void obs_data_release(obs_data_t *data)
{
	(void)data;
}

const char *obs_encoder_get_codec(const obs_encoder_t *encoder)
{
	(void)encoder;
	return flv_fake.vcodec;
}

uint32_t obs_encoder_get_width(const obs_encoder_t *encoder)
{
	(void)encoder;
	return flv_fake.width;
}

uint32_t obs_encoder_get_height(const obs_encoder_t *encoder)
{
	(void)encoder;
	return flv_fake.height;
}

uint32_t obs_encoder_get_sample_rate(const obs_encoder_t *encoder)
{
	(void)encoder;
	return flv_fake.arate;
}

double video_output_get_frame_rate(const video_t *video)
{
	(void)video;
	return flv_fake.fps;
}

size_t audio_output_get_channels(const audio_t *audio)
{
	(void)audio;
	return flv_fake.channels;
}

const char *obs_get_version_string(void)
{
	return flv_fake.version;
}

/* the opaque output handle given to flv_meta_data() */
obs_output_t *flv_fake_output(void)
{
	return (obs_output_t *)&the_output;
}
