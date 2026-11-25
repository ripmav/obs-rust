/*
 * Conformance harness: abort stubs for the C services the obs-ffi
 * staticlib references but the deque runner never exercises. The deque
 * area is the check-mode rustlib area (the monolithic staticlib object
 * pulls in every bridge, so all of their C externs must resolve), and no
 * other runner links this file, so the definitions cannot collide with
 * the real providers (obs-data.c, libcurl, platform-nix.c, obscore) or
 * the helpers runner's swap-mode fakes. Any call that reaches a stub
 * means the deque script strayed outside the area the harness covers.
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <time.h>

/* obs-data.c */
void *obs_data_create(void)
{
	abort();
}
void *obs_data_create_from_json(const char *json)
{
	(void)json;
	abort();
}
void *obs_data_create_from_json_file(const char *path)
{
	(void)path;
	abort();
}
void obs_data_release(void *data)
{
	(void)data;
	abort();
}
void obs_data_addref(void *data)
{
	(void)data;
	abort();
}
const char *obs_data_get_string(void *data, const char *name)
{
	(void)data;
	(void)name;
	abort();
}
int64_t obs_data_get_int(void *data, const char *name)
{
	(void)data;
	(void)name;
	abort();
}
void *obs_data_get_array(void *data, const char *name)
{
	(void)data;
	(void)name;
	abort();
}
size_t obs_data_array_count(void *array)
{
	(void)array;
	abort();
}
void *obs_data_array_item(void *array, size_t index)
{
	(void)array;
	(void)index;
	abort();
}
void obs_data_set_string(void *data, const char *name, const char *value)
{
	(void)data;
	(void)name;
	(void)value;
	abort();
}
bool obs_data_save_json(void *data, const char *file)
{
	(void)data;
	(void)file;
	abort();
}

/* libcurl */
void *curl_easy_init(void)
{
	abort();
}
void curl_easy_cleanup(void *handle)
{
	(void)handle;
	abort();
}
int curl_easy_setopt(void *handle, long option, ...)
{
	(void)handle;
	(void)option;
	abort();
}
int curl_easy_perform(void *handle)
{
	(void)handle;
	abort();
}
int curl_easy_getinfo(void *handle, long info, ...)
{
	(void)handle;
	(void)info;
	abort();
}
void *curl_slist_append(void *list, const char *s)
{
	(void)list;
	(void)s;
	abort();
}
void curl_slist_free_all(void *list)
{
	(void)list;
	abort();
}

/* platform file API (platform-nix.c) */
int os_mkdir(const char *path)
{
	(void)path;
	abort();
}
int os_rmdir(const char *path)
{
	(void)path;
	abort();
}
int os_copyfile(const char *input, const char *output)
{
	(void)input;
	(void)output;
	abort();
}
int os_unlink(const char *path)
{
	(void)path;
	abort();
}
int os_rename(const char *from, const char *to)
{
	(void)from;
	(void)to;
	abort();
}
bool os_quick_write_utf8_file(const char *path, const char *data, size_t len, bool marker)
{
	(void)path;
	(void)data;
	(void)len;
	(void)marker;
	abort();
}

/* obscore output / encoder / video counter surface */
struct encoder_packet;
int obs_output_get_total_frames(void *output)
{
	(void)output;
	abort();
}
int obs_output_get_frames_dropped(void *output)
{
	(void)output;
	abort();
}
uint32_t obs_get_total_frames(void)
{
	abort();
}
uint32_t obs_get_lagged_frames(void)
{
	abort();
}
void *obs_encoder_video(void *encoder)
{
	(void)encoder;
	abort();
}
uint32_t video_output_get_total_frames(void *video)
{
	(void)video;
	abort();
}
uint32_t video_output_get_skipped_frames(void *video)
{
	(void)video;
	abort();
}
uint32_t obs_encoder_get_encoded_frames(void *encoder)
{
	(void)encoder;
	abort();
}
const char *obs_encoder_get_name(void *encoder)
{
	(void)encoder;
	abort();
}
const char *obs_encoder_get_codec(void *encoder)
{
	(void)encoder;
	abort();
}
void obs_encoder_packet_release(struct encoder_packet *packet)
{
	(void)packet;
	abort();
}

/* threading-posix.c (excluded from the rustlib-area common sources —
 * the staticlib's shim does not provide it) */
uint64_t os_gettime_ns(void)
{
	abort();
}
struct timespec *os_nstime_to_timespec(uint64_t timestamp, struct timespec *storage)
{
	(void)timestamp;
	(void)storage;
	abort();
}
