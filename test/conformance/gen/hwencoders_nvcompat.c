/*
 * Conformance harness (area hwencoders, spec S-050): wrapper for plugins/obs-nvenc/nvenc-compat.c
 * (pre-31.0 encoder ids that migrate their settings and reroute). Test tooling, not part of OBS.
 */
#define ENABLE_HEVC 1
#include "hwencoders_api.h"
#include "../../../plugins/obs-nvenc/nvenc-compat.c"

void hw_nc_register(void)
{
	register_compat_encoders();
}
