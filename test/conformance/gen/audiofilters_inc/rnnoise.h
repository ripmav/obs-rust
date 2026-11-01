/* Conformance harness (area audiofilters): minimal fake of the RNNoise API used by
 * plugins/obs-filters/noise-suppress-filter.c. Test tooling, not part of OBS. */
#pragma once
typedef struct DenoiseState DenoiseState;
typedef struct RNNModel RNNModel;

DenoiseState *rnnoise_create(RNNModel *model);
void rnnoise_destroy(DenoiseState *st);
float rnnoise_process_frame(DenoiseState *st, float *out, const float *in);
