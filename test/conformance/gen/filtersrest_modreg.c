/* Conformance wrapper (area filtersrest, S-054): compiles the REAL plugins/obs-filters/obs-filters.c (module registration
 * table). The filter objects themselves are replaced by dummies carrying only their name as id, except the noise suppress
 * filters, which are the real ones from filtersrest_runner.c. */
#include <obs-module.h>
#undef OBS_MODULE_USE_DEFAULT_LOCALE
#define OBS_MODULE_USE_DEFAULT_LOCALE(a, b)
#include "../../../plugins/obs-filters/obs-filters.c"

#define DUMMY(n) struct obs_source_info n = {.id = #n};
DUMMY(mask_filter) DUMMY(mask_filter_v2) DUMMY(crop_filter) DUMMY(gain_filter) DUMMY(eq_filter)
DUMMY(hdr_tonemap_filter) DUMMY(sdr_on_hdr_filter) DUMMY(color_filter) DUMMY(color_filter_v2) DUMMY(scale_filter)
DUMMY(scroll_filter) DUMMY(gpu_delay_filter) DUMMY(color_key_filter) DUMMY(color_key_filter_v2)
DUMMY(color_grade_filter) DUMMY(sharpness_filter) DUMMY(sharpness_filter_v2) DUMMY(chroma_key_filter)
DUMMY(chroma_key_filter_v2) DUMMY(async_delay_filter) DUMMY(invert_polarity_filter) DUMMY(noise_gate_filter)
DUMMY(compressor_filter) DUMMY(limiter_filter) DUMMY(expander_filter) DUMMY(upward_compressor_filter)
DUMMY(luma_key_filter) DUMMY(luma_key_filter_v2)

bool filtersrest_module_load(void) { return obs_module_load(); }
const char *filtersrest_module_description(void) { return obs_module_description(); }
