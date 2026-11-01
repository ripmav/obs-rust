/*
 * Conformance harness (area platform): the few things platform.c / platform-nix.c
 * need that are not part of the platform layer, plus two test hooks:
 *
 *  - obs_get_video_info() returns a structure the runner can set (the formatted
 *    file name tokens %FPS %CRES %ORES %VF read it);
 *  - time() returns a runner-controlled value. platform.c calls time(0) and then
 *    localtime(); defining time() here makes the executable's definition win over
 *    the C library's, so os_generate_formatted_filename() is deterministic.
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <time.h>
#include "obs.h"

static struct obs_video_info conf_ovi;
static time_t conf_now;

struct obs_video_info *conf_video_info(void)
{
	return &conf_ovi;
}

void conf_set_now(int64_t sec)
{
	conf_now = (time_t)sec;
}

bool obs_get_video_info(struct obs_video_info *ovi)
{
	*ovi = conf_ovi;
	return true;
}

time_t time(time_t *t)
{
	if (t)
		*t = conf_now;
	return conf_now;
}
