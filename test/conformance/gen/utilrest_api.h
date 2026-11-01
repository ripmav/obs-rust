/* S-055 harness: interface between the runner, the fake libobs and the fake PulseAudio. Test tooling. */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "obs-internal.h"

struct ur_gfx_stats {
	int enter, leave, timer_create, timer_destroy, timer_begin, timer_end;
	int range_create, range_destroy, range_begin, range_end;
};
extern struct ur_gfx_stats ur_gfx;
void ur_set_clock(uint64_t ns);
void ur_gfx_cfg(uint64_t ticks, bool disjoint, uint64_t freq, bool data_ok);
int ur_gfx_unbalanced(void);
int ur_tasks_queued(void);
int ur_run_tasks(void);
void ur_set_async_ts(uint64_t ts);
extern const char *ur_dev_setting;
extern int ur_cap_adds, ur_cap_removes;
void ur_feed(obs_source_t *s, const struct audio_data *ad);
bool ur_have_cb(void);
void ur_set_ao(uint32_t rate, enum speaker_layout sp);
void ur_resampler_fail(bool f);

/* fake PulseAudio configuration / observation */
struct ur_pa_sink { char name[64], desc[64]; int format, rate, channels; };
struct ur_pa_cfg {
	char server_name[32], server_version[32], default_sink[64];
	bool default_sink_null;
	struct ur_pa_sink sinks[24];
	int nsinks;
	bool connect_fails;   /* context goes to FAILED instead of READY */
	bool ops_fail;        /* every pa_context_get_* returns NULL */
	bool stream_new_fails;
	int connect_ret;      /* result of pa_stream_connect_playback */
	size_t begin_size;    /* if non-zero pa_stream_begin_write reports this many bytes */
	int begin_ret;        /* result of pa_stream_begin_write */
};
extern struct ur_pa_cfg ur_pa;
void ur_pa_reset(void);
/* number of pa_threaded_mainloop_* calls that hit a NULL mainloop */
extern int ur_pa_null_mainloop_calls;
extern int ur_pa_contexts_created, ur_pa_contexts_freed, ur_pa_loops_created, ur_pa_loops_freed;
