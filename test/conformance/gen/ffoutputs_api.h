/*
 * S-046 ffoutputs: shared declarations of the fake libobs world (settings store, output, encoders, service,
 * fake pipe, fake clock, gated semaphores) used by ffoutputs_runner.c (mux / replay buffer / HLS output),
 * ffoutputs_out.c (ffmpeg_output) and ffoutputs_stubs.c. Test tooling, not part of libobs.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>

struct obs_data;
struct kv_ent {
	char key[40];
	int utype, dtype; /* 0 none, 1 int, 2 bool, 3 str */
	int64_t ui, di;
	char *us, *ds;
};
#define MAXKV 48
struct obs_data {
	struct kv_ent kv[MAXKV];
	int n;
	struct obs_data_array *arrays[4];
	char akey[4][24];
	int na;
};
struct obs_data_array {
	struct obs_data *items[8];
	int n;
};

struct obs_encoder {
	char codec[16], name[32];
	struct obs_data *settings;
	uint8_t extra[16];
	size_t extra_size;
	bool has_extra;
	uint32_t sample_rate, frame_size, priming;
	bool paused;
	int updates; /* obs_encoder_update calls */
};

struct obs_service {
	char url[96], key[48];
};

struct calldata;
struct proc_rec {
	char name[32];
	void (*fn)(void *, struct calldata *);
	void *param;
};

struct obs_output {
	struct obs_data *settings;
	char name[32];
	uint32_t flags;
	struct obs_encoder *venc;
	struct obs_encoder aenc[6];
	int naenc; /* consecutive audio encoders starting at track 0 */
	struct obs_service *service;
	uint32_t width, height;
	size_t mixers;
	uint64_t pause_offset;
	struct proc_rec procs[8];
	int nprocs;
	/* scripted behaviour */
	bool can_begin, init_enc_ok;
	int begin_calls, end_calls;
};

/* world knobs (ffoutputs_stubs.c) */
struct ffo_world {
	/* video / audio info */
	uint32_t fps_num, fps_den;
	int colorspace, range, vformat;
	uint32_t base_w, base_h;
	float hdr_peak;
	uint32_t channels;
	uint32_t sample_rate;
	int speakers;
	bool have_video_info, have_audio_info;
	/* fake file system */
	char files[16][160];
	int nfiles;
	char deny[4][160];
	int ndeny;
	/* pipe */
	bool pipe_fail;
	int pipe_creates;
	int write_fail_at; /* 1-based write number in the current pipe, 0 = never */
	int pipe_writes;
	int destroy_ret;
	char err_text[256];
	/* clock / filename generator */
	uint64_t now_ns;
	char fmt_seen[160];
	bool deferred_threads;
	/* packet refs */
	long pkt_refs;
	/* last stop code / error */
	char last_error[512];
};
extern struct ffo_world W;
extern uint8_t *wire_buf;
extern size_t wire_len;

/* sem / event gating */
void ffo_gate_pump(int n);
void ffo_thread_started(void);
void ffo_thread_ended(void);

/* helpers from ffoutputs_stubs.c */
struct obs_data *fd_create(void);
void fd_free(struct obs_data *d);
void fd_dump(const struct obs_data *d);
void ffo_log_signal(const char *name);

/* ffmpeg_output TU (ffoutputs_out.c) */
struct video_data;
struct audio_data;
void ffo_out_command(int argc, char **argv);
void ffo_out_reset(void);

/* thread policy used by the pthread_create/join wrappers (ffoutputs_stubs.c) */
extern int ffo_thread_mode; /* 0 run inline, 1 defer (run_pending), 2 real thread */
extern void *(*ffo_pending_fn)(void *);
extern void *ffo_pending_arg;
int ffo_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
int ffo_pthread_join(pthread_t t, void **ret);
void ffo_run_pending(void);
extern struct obs_output *ffo_O;
