/* Conformance wrapper (area audiofilters): includes the real plugin source and exposes its private state. */
#include "audiofilters_api.h"
#include "../../../plugins/obs-filters/expander-filter.c"

void af_state_expander(void *data)
{
	struct expander_data *cd = data;
	printf("state expander channels=%zu rate=%zu detector=%d is_gate=%d is_upwcomp=%d buf_len=%zu", cd->num_channels,
	       cd->sample_rate, cd->detector, cd->is_gate, cd->is_upwcomp, cd->envelope_buf_len);
	AF_F("ratio", cd->ratio);
	AF_F("threshold", cd->threshold);
	AF_F("attack_gain", cd->attack_gain);
	AF_F("release_gain", cd->release_gain);
	AF_F("output_gain", cd->output_gain);
	AF_F("slope", cd->slope);
	AF_F("knee", cd->knee);
	for (size_t i = 0; i < 2; i++) {
		printf(" ch%zu[", i);
		AF_F("env", cd->envelope[i]);
		AF_F("runave", cd->runave[i]);
		AF_F("gain_db", cd->gain_db_buf[i]);
		printf(" ]");
	}
	printf("\n");
}
