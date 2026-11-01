/* Conformance wrapper (area audiofilters): includes the real plugin source and exposes its private state. */
#include "audiofilters_api.h"
#include "../../../plugins/obs-filters/limiter-filter.c"

void af_state_limiter(void *data)
{
	struct limiter_data *cd = data;
	printf("state limiter channels=%zu rate=%zu buf_len=%zu", cd->num_channels, cd->sample_rate, cd->envelope_buf_len);
	AF_F("threshold", cd->threshold);
	AF_F("attack_gain", cd->attack_gain);
	AF_F("release_gain", cd->release_gain);
	AF_F("output_gain", cd->output_gain);
	AF_F("slope", cd->slope);
	AF_F("envelope", cd->envelope);
	printf("\n");
}
