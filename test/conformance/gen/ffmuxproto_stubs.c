/* S-037: the only libobs symbol platform.c needs that the runner does not link. Never called. */
#include <stdbool.h>
struct obs_video_info;
bool obs_get_video_info(struct obs_video_info *ovi)
{
	(void)ovi;
	return false;
}
