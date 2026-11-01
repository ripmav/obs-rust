/* Conformance harness (area imagesource, spec S-030): color-source.c compiled unchanged + inspector. */
#include "../../../plugins/image-source/color-source.c"

void imagesource_color_dump(void *data)
{
	struct color_source *c = data;
	printf("state color color=%.9g,%.9g,%.9g,%.9g color_srgb=%.9g,%.9g,%.9g,%.9g width=%u height=%u\n",
	       (double)c->color.x, (double)c->color.y, (double)c->color.z, (double)c->color.w, (double)c->color_srgb.x,
	       (double)c->color_srgb.y, (double)c->color_srgb.z, (double)c->color_srgb.w, c->width, c->height);
}
