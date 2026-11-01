/*
 * Conformance harness (area ffsources, spec S-047): interface between the script runner
 * (ffsources_runner.c), the wrapper translation units that #include real plugin code
 * (ffsources_src.c: obs-ffmpeg-source.c, ffsources_aud.c: obs-ffmpeg-audio-encoders.c) and the
 * recording fakes (ffsources_stubs.c). Test tooling, not part of OBS.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "obs.h"
#include "media-io/audio-io.h"
#include "obs-hotkey.h"
#include "obs-missing-files.h"
#include "callback/proc.h"
#include "util/threading.h"
#include <libavcodec/avcodec.h>

#define FS_MAXQ 8

struct fs_host {
	/* source host */
	int active, showing;
	int media_fail, media_has_video;
	int64_t media_duration_us, media_frames, media_time;
	int event_init_fail, mutex_init_fail, thread_create_fail;
	obs_data_t *src_settings;
	/* video/audio host */
	uint32_t width, height, fps_num, fps_den;
	enum video_format format;
	enum video_colorspace colorspace;
	enum video_range_type range;
	int channels, speakers;
	uint32_t sample_rate;
	char last_error[512];
	int has_last_error;
	uint64_t now_ns, pause_offset;
	/* fake libavcodec */
	int open_rc;
	int send_q[FS_MAXQ], send_n;
	struct {
		int rc, size;
		int64_t pts, dts;
		int flags;
	} recv_q[FS_MAXQ];
	int recv_n;
	int fill_rc;
	int tb_num, tb_den; /* time base given to the context by avcodec_open2; 0 = 1/sample_rate */
	int frame_size_after_open;
	int extradata_size, initial_padding;
};
extern struct fs_host g_host;

/* fake libavcodec configuration */
void fs_codec(const char *name, int present);
void fs_codec_lossless(const char *name, int v);
void fs_formats(const char *list);
void fs_rates(const char *list);
void fs_fill_sentinel(AVFrame *f);
void fs_set_exists(const char *path, int v);

struct fs_proc {
	char decl[96];
	proc_handler_proc_t fn;
	void *data;
};
struct fs_hotkey {
	char name[48];
	obs_hotkey_func fn;
	obs_hotkey_active_func afn;
	void *data;
};
struct fs_mfile {
	char path[64];
	obs_missing_file_cb cb;
	void *src;
	int type;
};
extern struct fs_proc fs_procs[64];
extern int fs_proc_n;
extern struct fs_hotkey fs_hotkeys[64];
extern int fs_hotkey_n;
extern struct fs_mfile fs_last_missing[4];
extern int fs_last_missing_n;

/* substitutes the source wrapper maps pthread/os_event/os_file_exists to */
bool fs_os_file_exists(const char *p);
int fs_os_event_init(os_event_t **ev, enum os_event_type type);
void fs_os_event_destroy(os_event_t *ev);
int fs_os_event_signal(os_event_t *ev);
void fs_os_event_reset(os_event_t *ev);
int fs_os_event_timedwait(os_event_t *ev, unsigned long ms);
int fs_pthread_mutex_init(pthread_mutex_t *m, const pthread_mutexattr_t *a);
int fs_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg);
int fs_pthread_join(pthread_t t, void **r);

/* source wrapper (ffsources_src.c) */
void fsw_defaults(obs_data_t *s);
obs_properties_t *fsw_props(void *h);
void *fsw_create(obs_data_t *s);
void fsw_destroy(void *h);
void fsw_update(void *h, obs_data_t *s);
void fsw_tick(void *h);
void fsw_activate(void *h);
void fsw_deactivate(void *h);
void fsw_play_pause(void *h, bool pause);
void fsw_stop(void *h);
void fsw_restart(void *h);
long long fsw_duration(void *h);
long long fsw_time(void *h);
void fsw_set_time(void *h, long long ms);
int fsw_state(void *h);
void fsw_missing(void *h);
void fsw_dump(void *h);
void fsw_hotkey(void *h, const char *which, bool pressed);
void fsw_proc(void *h, const char *name);
void fsw_media_cb(const char *which);
void fsw_thread_run(void);
void fsw_table(void);
void fsw_missing_cb(void *h, int index, const char *path);

/* audio wrapper (ffsources_aud.c) */
void *fsa_create(const char *kind, obs_data_t *s);
void fsa_destroy(void *h);
void fsa_encode(void *h, int fill);
void fsa_dump(void *h);
void fsa_table(void);
void fsa_info(const char *kind, void *h);
void fsa_defaults(const char *kind, obs_data_t *s);
obs_properties_t *fsa_props(const char *kind);
