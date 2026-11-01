/* Conformance wrapper (area audiofilters): includes the real plugin source and exposes its private state. */
#include "audiofilters_api.h"
#include "../../../plugins/obs-filters/noise-suppress-filter.c"

void af_state_ns(void *data)
{
	struct noise_suppress_data *ng = data;
	printf("state ns level=%d latency=%llu frames=%zu channels=%zu use_rnnoise=%d has_mono_src=%d last_ts=%llu",
	       ng->suppress_level, (unsigned long long)ng->latency, ng->frames, ng->channels, ng->use_rnnoise,
	       ng->has_mono_src, (unsigned long long)ng->last_timestamp);
	printf(" allocated=%d,%d resamplers=%d,%d in0=%zu out0=%zu info=%zu", ng->spx_states[0] != NULL,
	       ng->rnn_states[0] != NULL, ng->rnn_resampler != NULL, ng->rnn_resampler_back != NULL,
	       ng->input_buffers[0].size, ng->output_buffers[0].size, ng->info_buffer.size);
	printf("\n");
}
