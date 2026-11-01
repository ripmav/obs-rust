/* Conformance harness (area audiofilters): minimal fake of the SpeexDSP preprocessor API
 * used by plugins/obs-filters/noise-suppress-filter.c. Test tooling, not part of OBS. */
#pragma once
#include <stdint.h>

typedef int16_t spx_int16_t;
typedef struct SpeexPreprocessState_ SpeexPreprocessState;

#define SPEEX_PREPROCESS_SET_NOISE_SUPPRESS 18

SpeexPreprocessState *speex_preprocess_state_init(int frame_size, int sampling_rate);
void speex_preprocess_state_destroy(SpeexPreprocessState *st);
int speex_preprocess_ctl(SpeexPreprocessState *st, int request, void *ptr);
int speex_preprocess_run(SpeexPreprocessState *st, spx_int16_t *x);
