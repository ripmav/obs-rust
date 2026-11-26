/*
 * Conformance harness: weak abort stubs for the C services the obs-ffi
 * staticlib references (the monolithic staticlib objects pull in every
 * bridge, so all of their C externs must resolve in every runner that
 * links it — the deque area in check mode, every swapped area in swap
 * mode). Weak so the strong providers win where they exist (the real
 * obs-data.c / platform-nix.c / libcurl, the helpers runner's swap-mode
 * fakes, the rtmp area's stubs); where no provider exists, the stubs
 * satisfy the link and any call that reaches one means a script strayed
 * outside the area the harness covers.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* obs-data.c */
void * __attribute__((weak)) obs_data_create(void)
{
	abort();
}
void * __attribute__((weak)) obs_data_create_from_json(const char *json)
{
	(void)json;
	abort();
}
void * __attribute__((weak)) obs_data_create_from_json_file(const char *path)
{
	(void)path;
	abort();
}
void __attribute__((weak)) obs_data_release(void *data)
{
	(void)data;
	abort();
}
void __attribute__((weak)) obs_data_addref(void *data)
{
	(void)data;
	abort();
}
const char * __attribute__((weak)) obs_data_get_string(void *data, const char *name)
{
	(void)data;
	(void)name;
	abort();
}
int64_t __attribute__((weak)) obs_data_get_int(void *data, const char *name)
{
	(void)data;
	(void)name;
	abort();
}
void * __attribute__((weak)) obs_data_get_array(void *data, const char *name)
{
	(void)data;
	(void)name;
	abort();
}
size_t __attribute__((weak)) obs_data_array_count(void *array)
{
	(void)array;
	abort();
}
void * __attribute__((weak)) obs_data_array_item(void *array, size_t index)
{
	(void)array;
	(void)index;
	abort();
}
void __attribute__((weak)) obs_data_set_string(void *data, const char *name, const char *value)
{
	(void)data;
	(void)name;
	(void)value;
	abort();
}
bool __attribute__((weak)) obs_data_save_json(void *data, const char *file)
{
	(void)data;
	(void)file;
	abort();
}

/* libcurl */
void * __attribute__((weak)) curl_easy_init(void)
{
	abort();
}
void __attribute__((weak)) curl_easy_cleanup(void *handle)
{
	(void)handle;
	abort();
}
int __attribute__((weak)) curl_easy_setopt(void *handle, long option, ...)
{
	(void)handle;
	(void)option;
	abort();
}
int __attribute__((weak)) curl_easy_perform(void *handle)
{
	(void)handle;
	abort();
}
int __attribute__((weak)) curl_easy_getinfo(void *handle, long info, ...)
{
	(void)handle;
	(void)info;
	abort();
}
void * __attribute__((weak)) curl_slist_append(void *list, const char *s)
{
	(void)list;
	(void)s;
	abort();
}
void __attribute__((weak)) curl_slist_free_all(void *list)
{
	(void)list;
	abort();
}

/* platform file API (platform-nix.c) */
int __attribute__((weak)) os_mkdir(const char *path)
{
	(void)path;
	abort();
}
int __attribute__((weak)) os_rmdir(const char *path)
{
	(void)path;
	abort();
}
int __attribute__((weak)) os_copyfile(const char *input, const char *output)
{
	(void)input;
	(void)output;
	abort();
}
int __attribute__((weak)) os_unlink(const char *path)
{
	(void)path;
	abort();
}
int __attribute__((weak)) os_rename(const char *from, const char *to)
{
	(void)from;
	(void)to;
	abort();
}
bool __attribute__((weak)) os_quick_write_utf8_file(const char *path, const char *data, size_t len, bool marker)
{
	(void)path;
	(void)data;
	(void)len;
	(void)marker;
	abort();
}

/* obscore output / encoder / video counter surface */
struct encoder_packet;
int __attribute__((weak)) obs_output_get_total_frames(void *output)
{
	(void)output;
	abort();
}
int __attribute__((weak)) obs_output_get_frames_dropped(void *output)
{
	(void)output;
	abort();
}
uint32_t __attribute__((weak)) obs_get_total_frames(void)
{
	abort();
}
uint32_t __attribute__((weak)) obs_get_lagged_frames(void)
{
	abort();
}
void * __attribute__((weak)) obs_encoder_video(void *encoder)
{
	(void)encoder;
	abort();
}
uint32_t __attribute__((weak)) video_output_get_total_frames(void *video)
{
	(void)video;
	abort();
}
uint32_t __attribute__((weak)) video_output_get_skipped_frames(void *video)
{
	(void)video;
	abort();
}
uint32_t __attribute__((weak)) obs_encoder_get_encoded_frames(void *encoder)
{
	(void)encoder;
	abort();
}
const char * __attribute__((weak)) obs_encoder_get_name(void *encoder)
{
	(void)encoder;
	abort();
}
const char * __attribute__((weak)) obs_encoder_get_codec(void *encoder)
{
	(void)encoder;
	abort();
}
void __attribute__((weak)) obs_encoder_packet_release(struct encoder_packet *packet)
{
	(void)packet;
	abort();
}

/* threading-posix.c (excluded from the rustlib-area common sources —
 * the staticlib's shim does not provide it) */
uint64_t __attribute__((weak)) os_gettime_ns(void)
{
	abort();
}
struct timespec * __attribute__((weak)) os_nstime_to_timespec(uint64_t timestamp, struct timespec *storage)
{
	(void)timestamp;
	(void)storage;
	abort();
}

/* source-profiler bridge (P1-12.3, WP-37): the C services the
 * abi_source_profiler bridge calls (the graphics timers, the task queue,
 * the source async accessors) and the srcprof context shim. In the
 * utilrest swap runner the strong providers win (utilrest_stubs.c's
 * fake timers / accessors, the real c/srcprof-bridge.c shim); elsewhere
 * they satisfy the link and any call that reaches one means a script
 * strayed outside the area the harness covers. */
void *__attribute__((weak)) gs_timer_create(void)
{
	abort();
}
void __attribute__((weak)) gs_timer_destroy(void *timer)
{
	(void)timer;
	abort();
}
void __attribute__((weak)) gs_timer_begin(void *timer)
{
	(void)timer;
	abort();
}
void __attribute__((weak)) gs_timer_end(void *timer)
{
	(void)timer;
	abort();
}
bool __attribute__((weak)) gs_timer_get_data(void *timer, uint64_t *ticks)
{
	(void)timer;
	(void)ticks;
	abort();
}
void *__attribute__((weak)) gs_timer_range_create(void)
{
	abort();
}
void __attribute__((weak)) gs_timer_range_destroy(void *range)
{
	(void)range;
	abort();
}
void __attribute__((weak)) gs_timer_range_begin(void *range)
{
	(void)range;
	abort();
}
void __attribute__((weak)) gs_timer_range_end(void *range)
{
	(void)range;
	abort();
}
bool __attribute__((weak)) gs_timer_range_get_data(void *range, bool *disjoint, uint64_t *freq)
{
	(void)range;
	(void)disjoint;
	(void)freq;
	abort();
}
void __attribute__((weak)) obs_queue_task(int type, void *task, void *param, int wait)
{
	(void)type;
	(void)task;
	(void)param;
	(void)wait;
	abort();
}
uint32_t __attribute__((weak)) obs_source_get_output_flags(const void *source)
{
	(void)source;
	abort();
}
uint64_t __attribute__((weak)) obs_source_get_last_async_ts(const void *source)
{
	(void)source;
	abort();
}
void __attribute__((weak)) obs_ffi_srcprof_enter_context(void)
{
	abort();
}
void __attribute__((weak)) obs_ffi_srcprof_leave_context(void)
{
	abort();
}
