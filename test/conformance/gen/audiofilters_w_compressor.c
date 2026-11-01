/* Conformance wrapper (area audiofilters): includes the real plugin source and exposes its private state. */
#include "audiofilters_api.h"
#include "../../../plugins/obs-filters/compressor-filter.c"

void af_state_compressor(void *data)
{
	struct compressor_data *cd = data;
	printf("state compressor channels=%zu rate=%zu buf_len=%zu", cd->num_channels, cd->sample_rate, cd->envelope_buf_len);
	AF_F("ratio", cd->ratio);
	AF_F("threshold", cd->threshold);
	AF_F("attack_gain", cd->attack_gain);
	AF_F("release_gain", cd->release_gain);
	AF_F("output_gain", cd->output_gain);
	AF_F("slope", cd->slope);
	AF_F("envelope", cd->envelope);
	printf(" sidechain_name=%s weak=%d max_sc_frames=%zu sc_queue0=%zu", cd->sidechain_name ? cd->sidechain_name : "(null)",
	       cd->weak_sidechain != NULL, cd->max_sidechain_frames, cd->sidechain_data[0].size);
	printf(" check_time=%llu", (unsigned long long)cd->sidechain_check_time);
	printf("\n");
}
