/* Conformance harness (area videofilters, spec S-028): shared declarations of the fake OBS host. Test tooling. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "obs.h"

/* Fake filter/plain source. The filter's own settings object is created with the defaults applied. */
struct obs_source {
	int id;
	char name[24];
	const struct obs_source_info *info;
	void *data;
	obs_data_t *settings;
	long defer_update_count;
	struct obs_source *target, *parent;
	uint32_t bw, bh, w, h; /* base size, size */
	enum gs_color_space space;
	uint32_t flags; /* output flags of a plain source */
	obs_properties_t *props;
};

struct vf_host {
	struct obs_video_info ovi;
	float sdr_white;
	enum gs_color_space cur_space;
	uint64_t frame_time;
	int effect_fail, begin_result, texrender_fail, tex_fail;
	bool fb_srgb;
	char tmpdir[512];
};
extern struct vf_host g_vf;

void vf_define_image(const char *name, uint32_t cx, uint32_t cy, enum gs_color_format fmt, bool loaded, bool animated);
const char *vf_basename(const char *path);
void vf_resolve_missing(const char *new_path);
