/* Conformance suite: minimal stand-in for the build-generated obsconfig.h so
 * that libobs sources that include obs-internal.h compile without CMake.
 * Test tooling, not part of libobs. */
#pragma once
#define OBS_DATA_PATH "data/libobs"
#define OBS_INSTALL_PREFIX ""
#define OBS_PLUGIN_PATH "obs-plugins"
#define OBS_PLUGIN_DESTINATION "obs-plugins"
