/*
 * S-059 mpegtsout: declarations shared by mpegtsout_runner.c (includes the real obs-ffmpeg-mpegts.c), mpegtsout_stubs.c
 * (fake libobs world) and mpegtsout_net.c (scripted fake libsrt / librist transport). Test tooling, not part of OBS.
 */
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#define MW_MAXEVT 32

/* knobs of the fake libobs world; reset by "world_reset" */
struct mw_world {
	/* video */
	uint32_t fps_num, fps_den;
	int colorspace, range, vformat; /* libobs enum values */
	uint32_t width, height;
	float hdr_peak;
	bool have_vinfo;
	int vbitrate, keyint_sec;
	char vcodec[16];
	/* audio */
	uint32_t sample_rate;
	int speakers;
	bool have_ainfo;
	int ntracks;
	int abitrate[6];
	uint32_t frame_size;
	char acodec[16];
	/* extradata availability */
	bool vextra;
	bool aextra[6];
	/* service connect info; NULL means "not set" */
	bool have_service;
	char *url, *user, *pass, *streamid, *passphrase;
	/* output core */
	bool can_begin, init_ok;
	uint64_t pause_offset;
	/* recorded effects */
	int begin_calls, end_calls;
	int nevt;
	int evt[MW_MAXEVT]; /* signalled stop codes in order */
	char last_error[256];
};
extern struct mw_world W;

void mw_world_reset(void);
struct obs_data;
struct obs_output;
extern struct obs_output *mw_output; /* the single fake output */
struct obs_output *mw_output_create(void);
struct obs_data *mw_settings(void);
void mw_output_free(void);
void mw_set_str(char **dst, const char *v); /* "-" = NULL */

/* fake network (mpegtsout_net.c) */
void net_reset(void);
void net_command(int argc, char **argv); /* srt_* / rist_* / tsdump knobs */
void net_ts_report(void);
size_t net_captured(void);
int net_port(void);
size_t net_max_chunk(void);
void net_capture_clear(void);
int net_sends(void);
