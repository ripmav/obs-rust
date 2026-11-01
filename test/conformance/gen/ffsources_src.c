/*
 * Conformance harness (area ffsources, spec S-047): wrapper around the real
 * plugins/obs-ffmpeg/obs-ffmpeg-source.c. The plugin file is #included so that its static
 * functions can be called. Threads and events are replaced by deterministic single-threaded
 * substitutes (ffsources_stubs.c) through the macros below, which only affect this
 * translation unit. Test tooling.
 */
#include <obs-module.h>
#include <util/threading.h>
#include <util/platform.h>
#include <util/dstr.h>
#include <pthread.h>
#include "ffsources_api.h"
#include "script.h"

#define pthread_create fs_pthread_create
#define pthread_join fs_pthread_join
#define pthread_mutex_init fs_pthread_mutex_init
#define os_event_init fs_os_event_init
#define os_event_destroy fs_os_event_destroy
#define os_event_signal fs_os_event_signal
#define os_event_reset fs_os_event_reset
#define os_event_timedwait fs_os_event_timedwait
#define os_file_exists fs_os_file_exists

#include "../../../plugins/obs-ffmpeg/obs-ffmpeg-source.c"

#undef pthread_create
#undef pthread_join
#undef pthread_mutex_init
#undef os_event_init
#undef os_event_destroy
#undef os_event_signal
#undef os_event_reset
#undef os_event_timedwait
#undef os_file_exists

void fsw_defaults(obs_data_t *s)
{
	ffmpeg_source_defaults(s);
}
obs_properties_t *fsw_props(void *h)
{
	return ffmpeg_source_getproperties(h);
}
void *fsw_create(obs_data_t *s)
{
	return ffmpeg_source_create(s, (obs_source_t *)&g_host);
}
void fsw_destroy(void *h)
{
	ffmpeg_source_destroy(h);
}
void fsw_update(void *h, obs_data_t *s)
{
	ffmpeg_source_update(h, s);
}
void fsw_tick(void *h)
{
	ffmpeg_source_tick(h, 0.0f);
}
void fsw_activate(void *h)
{
	ffmpeg_source_activate(h);
}
void fsw_deactivate(void *h)
{
	ffmpeg_source_deactivate(h);
}
void fsw_play_pause(void *h, bool pause)
{
	ffmpeg_source_play_pause(h, pause);
}
void fsw_stop(void *h)
{
	ffmpeg_source_stop(h);
}
void fsw_restart(void *h)
{
	ffmpeg_source_restart(h);
}
long long fsw_duration(void *h)
{
	return ffmpeg_source_get_duration(h);
}
long long fsw_time(void *h)
{
	return ffmpeg_source_get_time(h);
}
void fsw_set_time(void *h, long long ms)
{
	ffmpeg_source_set_time(h, ms);
}
int fsw_state(void *h)
{
	return (int)ffmpeg_source_get_state(h);
}
void fsw_missing(void *h)
{
	obs_missing_files_t *f = ffmpeg_source_missingfiles(h);
	(void)f; /* the fake obs_missing_files_add_file() already consumed the list */
}
void fsw_missing_cb(void *h, int index, const char *path)
{
	/* libobs invokes the callback with the source's plugin data, not with the obs_source_t */
	fs_last_missing[index].cb(h, path, NULL);
}

static void pq(const char *s)
{
	if (!s)
		fputs("@null", stdout);
	else
		script_print_quoted(s);
}

void fsw_dump(void *h)
{
	struct ffmpeg_source *s = h;
	printf("source input=");
	pq(s->input);
	printf(" input_format=");
	pq(s->input_format);
	printf(" ffmpeg_options=");
	pq(s->ffmpeg_options);
	printf("\n  buffering_mb=%d speed_percent=%d range=%d reconnect_delay_sec=%d state=%d\n", s->buffering_mb,
	       s->speed_percent, (int)s->range, s->reconnect_delay_sec, (int)s->state);
	printf("  looping=%d local=%d hw=%d full_decode=%d clear_on_end=%d restart_on_activate=%d close_when_inactive=%d\n",
	       s->is_looping, s->is_local_file, s->is_hw_decoding, s->full_decode, s->is_clear_on_media_end,
	       s->restart_on_activate, s->close_when_inactive);
	printf("  seekable=%d stinger=%d track_matte=%d log_changes=%d linear_alpha=%d\n", s->seekable, s->is_stinger,
	       s->is_track_matte, s->log_changes, s->is_linear_alpha);
	printf("  media=%d destroy_media=%d reconnecting=%d reconnect_thread_valid=%d\n", s->media != NULL,
	       s->destroy_media, (int)s->reconnecting, s->reconnect_thread_valid);
}

void fsw_hotkey(void *h, const char *which, bool pressed)
{
	const char *n = !strcmp(which, "restart") ? "MediaSource.Restart"
			: !strcmp(which, "play")  ? "MediaSource.Play"
			: !strcmp(which, "pause") ? "MediaSource.Pause"
						  : "MediaSource.Stop";
	for (int i = 0; i < fs_hotkey_n; i++) {
		if (strcmp(fs_hotkeys[i].name, n) || fs_hotkeys[i].data != h)
			continue;
		if (fs_hotkeys[i].fn) {
			fs_hotkeys[i].fn(fs_hotkeys[i].data, 0, NULL, pressed);
			printf("hotkey %s pressed=%d\n", which, pressed);
		} else {
			bool r = fs_hotkeys[i].afn(fs_hotkeys[i].data, 0, NULL, pressed);
			printf("hotkey %s pressed=%d -> %d\n", which, pressed, r);
		}
		return;
	}
	fprintf(stderr, "script: unknown hotkey %s\n", which);
	exit(2);
}

void fsw_proc(void *h, const char *name)
{
	for (int i = 0; i < fs_proc_n; i++) {
		if (!strstr(fs_procs[i].decl, name) || fs_procs[i].data != h)
			continue;
		calldata_t cd = {0};
		fs_procs[i].fn(fs_procs[i].data, &cd);
		if (!strcmp(name, "get_duration")) {
			long long v = 0;
			calldata_get_int(&cd, "duration", &v);
			printf("proc %s duration=%lld\n", name, v);
		} else if (!strcmp(name, "get_nb_frames")) {
			long long v = 0;
			calldata_get_int(&cd, "num_frames", &v);
			printf("proc %s num_frames=%lld\n", name, v);
		} else {
			printf("proc %s\n", name);
		}
		calldata_free(&cd);
		return;
	}
	fprintf(stderr, "script: unknown proc %s\n", name);
	exit(2);
}

void fsw_table(void)
{
	const struct obs_source_info *i = &ffmpeg_source;
	printf("source id=%s type=%d output_flags=0x%x icon=%d name=%s\n", i->id, (int)i->type, i->output_flags,
	       (int)i->icon_type, i->get_name(NULL));
	printf("  callbacks create=%d destroy=%d get_defaults=%d get_properties=%d activate=%d deactivate=%d "
	       "video_tick=%d missing_files=%d update=%d\n",
	       i->create != NULL, i->destroy != NULL, i->get_defaults != NULL, i->get_properties != NULL,
	       i->activate != NULL, i->deactivate != NULL, i->video_tick != NULL, i->missing_files != NULL,
	       i->update != NULL);
	printf("  media play_pause=%d restart=%d stop=%d get_duration=%d get_time=%d set_time=%d get_state=%d\n",
	       i->media_play_pause != NULL, i->media_restart != NULL, i->media_stop != NULL,
	       i->media_get_duration != NULL, i->media_get_time != NULL, i->media_set_time != NULL,
	       i->media_get_state != NULL);
}
