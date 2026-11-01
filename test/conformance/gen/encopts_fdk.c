/*
 * Conformance harness (area encopts): wrapper around the real plugins/obs-libfdk/obs-libfdk.c
 * with a recording fake of the fdk-aac encoder library (aacEnc*). Test tooling.
 */
#include <obs-module.h>
/* the module boilerplate would define obs_module_text() & co.; encopts_stubs.c provides it */
#undef OBS_DECLARE_MODULE
#define OBS_DECLARE_MODULE()
#undef OBS_MODULE_USE_DEFAULT_LOCALE
#define OBS_MODULE_USE_DEFAULT_LOCALE(a, b)
#include "../../../plugins/obs-libfdk/obs-libfdk.c"
#include "encopts_api.h"

static int g_calls, g_fail_at;
static int g_dummy;

void fdkw_fail(int at)
{
	g_fail_at = at;
}

static AACENC_ERROR tick(const char *what)
{
	g_calls++;
	if (g_fail_at && g_calls == g_fail_at) {
		printf("fdk.%s -> INIT_ERROR\n", what);
		return AACENC_INIT_ERROR;
	}
	return AACENC_OK;
}

AACENC_ERROR aacEncOpen(HANDLE_AACENCODER *h, const UINT encModules, const UINT maxChannels)
{
	printf("fdk.aacEncOpen modules=%u channels=%u\n", encModules, maxChannels);
	AACENC_ERROR e = tick("aacEncOpen");
	if (e == AACENC_OK)
		*h = (HANDLE_AACENCODER)&g_dummy;
	return e;
}

AACENC_ERROR aacEncClose(HANDLE_AACENCODER *h)
{
	(void)h;
	printf("fdk.aacEncClose\n");
	return AACENC_OK;
}

static const char *param_name(AACENC_PARAM p)
{
	switch (p) {
	case AACENC_AOT:
		return "AOT";
	case AACENC_SAMPLERATE:
		return "SAMPLERATE";
	case AACENC_CHANNELMODE:
		return "CHANNELMODE";
	case AACENC_CHANNELORDER:
		return "CHANNELORDER";
	case AACENC_BITRATEMODE:
		return "BITRATEMODE";
	case AACENC_BITRATE:
		return "BITRATE";
	case AACENC_TRANSMUX:
		return "TRANSMUX";
	case AACENC_AFTERBURNER:
		return "AFTERBURNER";
	default:
		return "OTHER";
	}
}

AACENC_ERROR aacEncoder_SetParam(const HANDLE_AACENCODER h, const AACENC_PARAM param, const UINT value)
{
	(void)h;
	printf("fdk.SetParam %s=%u\n", param_name(param), value);
	return tick("SetParam");
}

AACENC_ERROR aacEncEncode(const HANDLE_AACENCODER h, const AACENC_BufDesc *a, const AACENC_BufDesc *b,
			  const AACENC_InArgs *c, AACENC_OutArgs *d)
{
	(void)h, (void)a, (void)b, (void)c, (void)d;
	printf("fdk.aacEncEncode (init)\n");
	return tick("aacEncEncode");
}

AACENC_ERROR aacEncInfo(const HANDLE_AACENCODER h, AACENC_InfoStruct *info)
{
	(void)h;
	memset(info, 0, sizeof(*info));
	info->frameLength = 1024;
	printf("fdk.aacEncInfo\n");
	return tick("aacEncInfo");
}

void fdkw_defaults(obs_data_t *s)
{
	libfdk_defaults(s);
}

obs_properties_t *fdkw_props(void)
{
	return libfdk_properties(NULL);
}

bool fdkw_create(obs_data_t *s)
{
	g_calls = 0;
	libfdk_encoder_t *enc = libfdk_create(s, (obs_encoder_t *)&g_host);
	if (!enc)
		return false;
	printf("fdk_state channels=%d sample_rate=%d frame_size_bytes=%d packet_buffer_size=%d\n", enc->channels,
	       enc->sample_rate, enc->frame_size_bytes, enc->packet_buffer_size);
	libfdk_destroy(enc);
	return true;
}
