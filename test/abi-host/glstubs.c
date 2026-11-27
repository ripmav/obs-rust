/* WP-40 ABI harness: GL (gs_*) no-op stubs. The ABI harness drives ticks
 * headless (no real rendering); the trace records callback entry/exit and
 * property/data operations, never pixels. Every stub logs nothing and
 * returns harmless defaults (NULL / 0). */
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
struct gs_effect; struct gs_texture; struct gs_samplerstate; struct gs_stagesurface;
struct gs_swapchain; struct gs_texture_render; struct vec2; struct vec4; struct matrix4;

void *gs_create(void) { return NULL; }
void gs_destroy(void) {}
void gs_enter_context(void *) {}
void gs_leave_context(void) {}
bool gs_begin_frame(int, int) { return true; }
void gs_end_scene(void) {}
void gs_begin_scene(void) {}
void gs_ortho(double, double, double, double, double, double) {}
void gs_matrix_push(void) {}
void gs_matrix_pop(void) {}
void gs_matrix_rotaa4f(float, float, float, float) {}
void gs_matrix_translate3f(float, float, float) {}
void gs_set_viewport(uint32_t, uint32_t, uint32_t, uint32_t) {}
void gs_set_cull_mode(uint32_t) {}
void gs_enable_blending(bool) {}
void gs_enable_depth_test(bool) {}
void gs_enable_framebuffer_srgb(bool) {}
void gs_blend_state_push(void) {}
void gs_blend_state_pop(void) {}
void gs_blend_function(uint32_t, uint32_t) {}
void gs_blend_function_separate(uint32_t, uint32_t, uint32_t, uint32_t) {}
void gs_clear(uint32_t, float, float, float, float) {}
void gs_draw(uint32_t, uint32_t, uint32_t, uint32_t) {}
void gs_draw_sprite(void *, float, float, uint32_t, uint32_t) {}
void gs_flush(void) {}
void *gs_effect_create_from_file(const char *, const char **) { return NULL; }
void gs_effect_destroy(void *) {}
struct gs_effect *gs_get_effect(void) { return NULL; }
void *gs_effect_get_technique(void *, const char *) { return NULL; }
void *gs_effect_get_param_by_name(void *, const char *) { return NULL; }
void gs_effect_set_val(void *, void *, int) {}
void gs_effect_set_float(void *, float) {}
void gs_effect_set_matrix4(void *, const void *) {}
void gs_effect_set_texture(void *, void *) {}
void gs_effect_set_texture_srgb(void *, void *) {}
void gs_effect_set_vec2(void *, const void *) {}
void gs_effect_set_vec4(void *, const void *) {}
int gs_effect_loop(void *, void *) { return 1; }
void gs_technique_begin(void *) {}
int gs_technique_begin_pass(void *, int) { return 1; }
void gs_technique_end_pass(void) {}
void gs_technique_end(void) {}
void *gs_texture_create(void) { return NULL; }
void *gs_texture_create_nv12(void) { return NULL; }
void *gs_texture_create_p010(void) { return NULL; }
void gs_texture_destroy(void *) {}
bool gs_texture_set_image(void *, const uint8_t *, uint32_t, uint32_t, uint32_t, bool) { return true; }
uint32_t gs_texture_get_width(void *) { return 0; }
uint32_t gs_texture_get_height(void *) { return 0; }
uint32_t gs_texture_get_color_format(void *) { return 0; }
void gs_copy_texture(void *, void *, int, int, int, int, int, int) {}
bool gs_nv12_available(void) { return false; }
bool gs_p010_available(void) { return false; }
void *gs_samplerstate_create(void) { return NULL; }
void gs_samplerstate_destroy(void *) {}
void *gs_stagesurface_create(uint32_t, uint32_t, uint32_t, uint32_t) { return NULL; }
void gs_stagesurface_destroy(void *) {}
bool gs_stagesurface_map(void *, const uint8_t **, uint32_t *) { return false; }
void gs_stagesurface_unmap(void *) {}
void gs_stage_texture(void *, int, int, int, int) {}
void *gs_swapchain_create(void) { return NULL; }
void gs_swapchain_destroy(void *) {}
void *gs_texrender_create(uint32_t, uint32_t, uint32_t, uint32_t) { return NULL; }
void gs_texrender_destroy(void *) {}
bool gs_texrender_reset(void *, uint32_t, uint32_t, uint32_t, uint32_t) { return true; }
bool gs_texrender_begin(void *, uint32_t, uint32_t) { return true; }
bool gs_texrender_begin_with_color_space(void *, uint32_t, uint32_t, uint32_t) { return true; }
void gs_texrender_end(void *) {}
void *gs_texrender_get_texture(void *) { return NULL; }
uint32_t gs_texrender_get_format(void *) { return 0; }
void gs_set_render_target(void *, void *) {}
void gs_set_render_target_with_color_space(void *, void *, uint32_t) {}
void gs_present(void) {}
void gs_resize(uint32_t, uint32_t) {}
void gs_update_color_space(uint32_t) {}
/* 33.0.1: enum gs_color_space gs_get_color_space(void) — return 0
 * (= GS_CS_SRGB, SDR) so source_render takes the SDR path */
int gs_get_color_space(void) { return 0; }
bool gs_set_linear_srgb(bool) { return false; }
bool gs_get_linear_srgb(void) { return false; }
bool gs_framebuffer_srgb_enabled(void) { return false; }
bool gs_is_present_ready(void) { return false; }
uint32_t gs_get_device_type(void) { return 0; }

/* --- missing gs_* (profiler timers + misc) --- */
struct gs_timer *gs_timer_create(void) { return (struct gs_timer *)1; }
void gs_timer_destroy(struct gs_timer *) {}
void gs_timer_begin(struct gs_timer *) {}
void gs_timer_end(struct gs_timer *) {}
void gs_timer_get_data(struct gs_timer *, uint64_t *, uint64_t *, uint64_t *, uint64_t *) {}
struct gs_timer_range *gs_timer_range_create(const char *) { return (struct gs_timer_range *)1; }
void gs_timer_range_destroy(struct gs_timer_range *) {}
void gs_timer_range_begin(struct gs_timer_range *) {}
void gs_timer_range_end(struct gs_timer_range *) {}
void gs_timer_range_get_data(struct gs_timer_range *, uint64_t *, uint64_t *, uint64_t *, uint64_t *) {}
void gs_blend_op(uint32_t, uint32_t, uint32_t, uint32_t) {}
void gs_effect_set_bool(void *, bool) {}
void gs_effect_set_int(void *, int) {}
void gs_effect_set_next_sampler(void *, void *) {}
bool gs_load_swapchain(void *) { return false; }
void gs_matrix_mul(void) {}
void gs_matrix_scale3f(float, float, float) {}
void gs_reset_blend_state(void) {}

/* --- CMake-generated core-module list (none in the mini build) --- */
const char *obs_core_modules[] = {NULL};
unsigned int obs_core_modules_count = 0;

/* --- audio resampler (FFmpeg-backed in the real build; the mini harness
 * never buffers audio, so these abort if ever reached) --- */
struct audio_resampler;
void *audio_resampler_create(void) { return NULL; }
void audio_resampler_destroy(void *) {}
int audio_resampler_resample(void *, void *, void *, uint32_t, uint32_t) { return -1; }

/* --- core-module membership (no core modules in the mini build) --- */
bool obs_in_core_module_list(const char *name) { return false; }

/* No-op hotkeys vtable (the real X11 vtable n-adjacent; the harness emits no
 * AV1 packets) --- */
void metadata_obu(const uint8_t *source_buffer, size_t source_bufsize, uint8_t **out_buffer, size_t *outbuf_size,
		  uint8_t metadata_type)
{
	(void)source_buffer;
	(void)source_bufsize;
	*out_buffer = NULL;
	*outbuf_size = 0;
	(void)metadata_type;
}

/* --- CMake-generated version strings (cmake/common/versionconfig.cmake) --- */
const char *OBS_VERSION = "33.0.1";
const char *OBS_VERSION_CANONICAL = "33.0.1";

/* --- FFmpeg video scaler (the mini harness never scales frames) --- */
struct video_scaler;
void *video_scaler_create(void) { return NULL; }
void video_scaler_destroy(void *) {}
int video_scaler_scale(void *, void *, const uint8_t *, uint32_t, const uint8_t *, uint32_t, uint32_t, uint32_t)
{
	return -1;
}

void gs_texture_unmap(void *, uint8_t *, uint32_t *) {}
bool gs_texture_map(struct gs_texture *tex, uint8_t **ptr, uint32_t *linesize)
{
	(void)tex;
	if (ptr)
		*ptr = NULL;
	if (linesize)
		*linesize = 0;
	return false;
}
