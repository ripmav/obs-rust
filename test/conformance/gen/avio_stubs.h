/* Shared declarations between avio_runner.c and avio_stubs.c (test tooling, not part of libobs). */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <semaphore.h>

/* fake clock + lock-step control (all state is touched from the driver thread and, under the semaphores, the io thread) */
extern uint64_t avio_fake_now;       /* value returned by os_gettime_ns() */
extern uint64_t avio_consumed_target; /* sleep target of the tick the driver just released */
extern uint64_t avio_last_sleep_target; /* target of the most recent os_sleepto_ns_fast() */
extern bool avio_audio_lockstep;     /* audio thread blocks in os_sleepto_ns_fast until avio_audio_go is posted */
extern sem_t avio_audio_go, avio_audio_done, avio_clock_read, avio_video_done;
extern volatile bool avio_shutdown;

/* fake scaler / resampler controls */
extern int avio_scaler_create_result;   /* 0 ok, -1 bad conversion, -2 failed */
extern bool avio_scaler_scale_ok;
extern bool avio_resampler_create_ok;
extern bool avio_resampler_resample_ok;
extern uint64_t avio_resampler_offset;
extern uint32_t avio_resampler_out_frames; /* 0 = derive from rate ratio */
