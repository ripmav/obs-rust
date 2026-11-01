/* Conformance wrapper (area audiofilters): includes the real plugin source and exposes its private state. */
#include "audiofilters_api.h"
#include "../../../plugins/obs-filters/gain-filter.c"

void af_state_gain(void *data)
{
	struct gain_data *gf = data;
	printf("state gain channels=%zu", gf->channels);
	AF_F("multiple", gf->multiple);
	printf("\n");
}
