/* Conformance harness (spec S-032): interface between audiomix_runner.c and audiomix_stubs.c. Test tooling. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "obs.h"

#define AM_TS_TICK UINT64_MAX /* "report the start timestamp of the tick being mixed" */

enum am_kind { AM_INPUT, AM_CUSTOM, AM_CUSTOM_AUDIO, AM_SUBMIX, AM_FILTER };

struct am_src;

void am_set_clock(uint64_t ns);
void am_core_reset(uint32_t rate, enum speaker_layout speakers);
struct obs_core *am_obs(void);
obs_source_t *am_source_create(const char *name, enum am_kind kind);
void am_cfg(obs_source_t *source, bool ok, uint64_t ts, float value, float gain, bool drop);
void am_add_child(obs_source_t *parent, obs_source_t *child);
void am_add_filter(obs_source_t *source, obs_source_t *filter);
void am_hotkey_ptm(obs_source_t *source, bool pressed);
void am_hotkey_ptt(obs_source_t *source, bool pressed);
void am_add_video_mix(bool mix_audio);
void am_set_channel(size_t mix, uint32_t channel, obs_source_t *source);
void am_set_task(void (*fn)(void *), void *param);
extern uint64_t am_tick_start, am_resampler_ts_offset;
extern int am_resampler_creates;
extern bool am_resampler_fail;
