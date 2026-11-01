/* Shared declarations between the S-041 runner, the fake core and the obs-output.c wrapper. Test tooling only. */
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "obs-internal.h"

/* fake clock */
void eo_set_clock(uint64_t ns);

/* fake raw video / audio connections: deliver frames to every registered callback */
void eo_feed_video(uint64_t ts_ns);
void eo_feed_audio(size_t mix, uint64_t ts_ns, uint32_t frames);
size_t eo_video_conns(void);
size_t eo_audio_conns(void);

/* fake media configuration */
void eo_config_video(uint32_t fps_num, uint32_t fps_den, uint32_t w, uint32_t h);
void eo_config_audio(uint32_t rate);
video_t *eo_video(void);
audio_t *eo_audio(void);

/* reconnect thread control (obs-output.c wrapper) */
extern volatile long eo_reconnect_created;
extern volatile long eo_waits_recorded;
extern volatile long eo_gate;
extern uint32_t eo_wait_ms[64];
extern volatile int eo_release;
extern volatile long eo_waits_done;
extern volatile long eo_threads_finished;
