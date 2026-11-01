/* Conformance harness (area imagesource, spec S-030): obs-slideshow.c (v1) compiled unchanged + inspector. */
#include <util/platform.h>
bool host_os_file_exists(const char *);
#define os_file_exists host_os_file_exists
#include "../../../plugins/image-source/obs-slideshow.c"

extern const char *imagesource_label(obs_source_t *s);

void imagesource_ss1_dump(void *data)
{
	struct slideshow *s = data;
	printf("state slideshow1 behavior=%d manual=%d randomize=%d loop=%d hide=%d use_cut=%d paused=%d stop=%d "
	       "restart_on_activate=%d pause_on_deactivate=%d slide_time=%.9g tr_speed=%u tr_name=%s cur_item=%zu "
	       "elapsed=%.9g cx=%u cy=%u mem_usage=%llu state=%d transition=%s\n",
	       (int)s->behavior, s->manual, s->randomize, s->loop, s->hide, s->use_cut, s->paused, s->stop,
	       s->restart_on_activate, s->pause_on_deactivate, (double)s->slide_time, s->tr_speed,
	       s->tr_name ? s->tr_name : "null", s->cur_item, (double)s->elapsed, s->cx, s->cy,
	       (unsigned long long)s->mem_usage, (int)s->state, imagesource_label(s->transition));
	for (size_t i = 0; i < s->files.num; i++)
		printf("state   file %zu %s %s\n", i, s->files.array[i].path, imagesource_label(s->files.array[i].source));
}
