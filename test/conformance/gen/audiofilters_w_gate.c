/* Conformance wrapper (area audiofilters): includes the real plugin source and exposes its private state. */
#include "audiofilters_api.h"
#include "../../../plugins/obs-filters/noise-gate-filter.c"

void af_state_gate(void *data)
{
	struct noise_gate_data *ng = data;
	printf("state gate channels=%zu is_open=%d", ng->channels, ng->is_open);
	AF_F("sample_rate_i", ng->sample_rate_i);
	AF_F("open_threshold", ng->open_threshold);
	AF_F("close_threshold", ng->close_threshold);
	AF_F("decay_rate", ng->decay_rate);
	AF_F("attack_rate", ng->attack_rate);
	AF_F("release_rate", ng->release_rate);
	AF_F("hold_time", ng->hold_time);
	AF_F("attenuation", ng->attenuation);
	AF_F("level", ng->level);
	AF_F("held_time", ng->held_time);
	printf("\n");
}
