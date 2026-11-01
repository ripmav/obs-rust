/*
 * Stubs for the symbols libobs-opengl expects from the rest of libobs (graphics.c,
 * obs-nix-platform.c). Test tooling only.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <graphics/graphics.h>
#include <graphics/matrix4.h>
#include <obs-nix-platform.h>

/* The view matrix the backend asks the matrix stack for (device_draw); identity unless
 * the runner sets it. */
struct matrix4 glbackend_view_matrix;

void gs_matrix_get(struct matrix4 *dst)
{
	*dst = glbackend_view_matrix;
}

gs_effect_t *gs_get_effect(void)
{
	return NULL;
}

void gs_effect_update_params(gs_effect_t *effect)
{
	(void)effect;
}

gs_texture_t *gs_texture_create(uint32_t width, uint32_t height, enum gs_color_format color_format,
				uint32_t levels, const uint8_t **data, uint32_t flags)
{
	(void)width, (void)height, (void)color_format, (void)levels, (void)data, (void)flags;
	return NULL;
}

enum obs_nix_platform_type obs_get_nix_platform(void)
{
	return OBS_NIX_PLATFORM_X11_EGL;
}

void *obs_get_nix_platform_display(void)
{
	return NULL;
}

void obs_set_nix_platform(enum obs_nix_platform_type platform)
{
	(void)platform;
}

void obs_set_nix_platform_display(void *display)
{
	(void)display;
}
