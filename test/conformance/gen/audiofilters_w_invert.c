/* Conformance wrapper (area audiofilters): includes the real plugin source and exposes its private state. */
#include "audiofilters_api.h"
#include "../../../plugins/obs-filters/invert-audio-polarity.c"

void af_state_invert(void *data)
{
	(void)data;
	printf("state invert");
	printf("\n");
}
