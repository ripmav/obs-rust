/* Conformance harness (area audiofilters): interface between runner and fake host. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "obs.h"

#define AF_MAX_CB 4

struct af_source {
	char name[64];
	bool audio;       /* OBS_SOURCE_AUDIO in output flags */
	bool dead;        /* weak references no longer resolve */
	enum speaker_layout layout;
	struct af_source *parent; /* obs_filter_get_parent() */
	int refs;         /* strong references handed out and not released */
	int ncb;
	struct {
		obs_source_audio_capture_t cb;
		void *param;
	} cbs[AF_MAX_CB];
	uint64_t pos;     /* running sample position for sc_push generators */
};

struct af_host {
	uint32_t rate;
	size_t channels;
	uint64_t clock;
	bool events;
	uint32_t resampler_out_frames; /* 0 = formula */
	int weak_live;                 /* weak refs allocated and not released */
	uint32_t seq;
};

extern struct af_host g_af;
#define AF_MAX_SOURCES 16
extern struct af_source *g_sources[AF_MAX_SOURCES];
extern int g_nsources;

struct af_source *af_find_source(const char *name);
void af_frame_released_hook(struct obs_source_frame *f);

#include <math.h>
#include <stdio.h>
/* Canonical float printing: nan / inf / -inf, -0 printed as 0, otherwise %.9g. */
static inline void af_pf(double v)
{
	if (isnan(v))
		fputs("nan", stdout);
	else if (isinf(v))
		fputs(v < 0 ? "-inf" : "inf", stdout);
	else if (v == 0)
		fputs("0", stdout);
	else
		printf("%.9g", v);
}
#define AF_F(label, v)                  \
	do {                            \
		printf(" %s=", label); \
		af_pf((double)(v));    \
	} while (0)
