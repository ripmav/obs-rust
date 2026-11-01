/* Conformance wrapper (area audiofilters): includes the real plugin source and exposes its private state. */
#include "audiofilters_api.h"
#include "../../../plugins/obs-filters/async-delay-filter.c"

void af_state_delay(void *data)
{
	struct async_delay_data *f = data;
	printf("state delay interval=%llu samplerate=%llu queued=%zu last_video_ts=%llu reached=%d reset_video=%d reset_audio=%d",
	       (unsigned long long)f->interval, (unsigned long long)f->samplerate,
	       f->video_frames.size / sizeof(void *), (unsigned long long)f->last_video_ts, f->video_delay_reached,
	       f->reset_video, f->reset_audio);
	printf("\n");
}
