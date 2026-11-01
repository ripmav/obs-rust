/* Conformance wrapper (area audiofilters): includes the real plugin source and exposes its private state. */
#include "audiofilters_api.h"
#include "../../../plugins/obs-filters/eq-filter.c"

void af_state_eq(void *data)
{
	struct eq_data *eq = data;
	printf("state eq channels=%zu", eq->channels);
	AF_F("lf", eq->lf);
	AF_F("hf", eq->hf);
	AF_F("low_gain", eq->low_gain);
	AF_F("mid_gain", eq->mid_gain);
	AF_F("high_gain", eq->high_gain);
	printf("\n");
}
