/* Conformance harness (area imagesource, spec S-030): obs-slideshow-mk2.c compiled unchanged + inspector. */
#include <util/platform.h>
bool host_os_file_exists(const char *);
#define os_file_exists host_os_file_exists
#include "../../../plugins/image-source/obs-slideshow-mk2.c"

extern const char *imagesource_label(obs_source_t *s);

static void dump_deque(const char *name, struct deque *d)
{
	size_t n = d->size / sizeof(struct source_data);
	printf("state   %s", name);
	for (size_t i = 0; i < n; i++) {
		struct source_data *sd = deque_data(d, i * sizeof(*sd));
		printf(" %zu:%s", sd->slide_idx, imagesource_label(sd->source));
	}
	printf("\n");
}

void imagesource_ss2_dump(void *data)
{
	struct slideshow *s = data;
	struct slideshow_data *d = &s->data;
	printf("state slideshow2 behavior=%d manual=%d randomize=%d loop=%d hide=%d use_cut=%d paused=%d stop=%d "
	       "restart_on_activate=%d pause_on_deactivate=%d slide_time=%.9g tr_speed=%u tr_name=%s elapsed=%.9g "
	       "cx=%u cy=%u state=%d transition=%s files=%zu\n",
	       (int)d->behavior, d->manual, d->randomize, d->loop, d->hide, d->use_cut, d->paused, d->stop,
	       d->restart_on_activate, d->pause_on_deactivate, (double)d->slide_time, d->tr_speed,
	       d->tr_name ? d->tr_name : "null", (double)d->elapsed, s->cx, s->cy, (int)d->state,
	       imagesource_label(s->transition), d->files.num);
	for (size_t i = 0; i < d->files.num; i++)
		printf("state   file %zu %s\n", i, d->files.array[i].path);
	if (d->slides.cur.source)
		printf("state   cur %zu %s\n", d->slides.cur.slide_idx, imagesource_label(d->slides.cur.source));
	else
		printf("state   cur none\n");
	dump_deque("prev", &d->slides.prev);
	dump_deque("next", &d->slides.next);
}
