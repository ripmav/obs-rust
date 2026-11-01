/*
 * Conformance harness (area imagesource, spec S-030): compiles the reference image source
 * (plugins/image-source/image-source.c) unchanged and adds one inspector that prints the private state.
 * Test tooling, not part of the plugin.
 */
#include <util/platform.h>
bool host_os_file_exists(const char *);
#define os_file_exists host_os_file_exists
#include "../../../plugins/image-source/image-source.c"

#include <inttypes.h>
#include "script.h"


void imagesource_img_dump(void *data)
{
	struct image_source *c = data;
	printf("state image file=");
	script_print_quoted(c->file ? c->file : "");
	printf(" persistent=%d is_slide=%d linear_alpha=%d file_timestamp=%lld update_time_elapsed=%.9g last_time=%" PRIu64
	       " active=%d restart_gif=%d file_decoded=%d texture_loaded=%d\n",
	       c->persistent, c->is_slide, c->linear_alpha, (long long)c->file_timestamp, (double)c->update_time_elapsed,
	       c->last_time, c->active, c->restart_gif, (int)c->file_decoded, (int)c->texture_loaded);
	printf("state image.file loaded=%d cx=%u cy=%u animated=%d texture=%d cur_frame=%d cur_loop=%d cur_time=%" PRIu64
	       " mem_usage=%" PRIu64 " format=%d alpha_mode=%d space=%d\n",
	       c->image.loaded, c->image.cx, c->image.cy, c->image.is_animated_gif, c->image.texture != NULL,
	       c->image.cur_frame, c->image.cur_loop, c->image.cur_time, c->image.mem_usage, (int)c->image.format,
	       (int)c->image.alpha_mode, (int)c->image.space);
}
