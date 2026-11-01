/* Conformance wrapper (area linuxcapture): includes the real xshm-input.c against a fake xcb/X11 helper layer. */
#include "../../../plugins/linux-capture/xshm-input.c"
#include "linuxcapture_api.h"

int lc_tex_alive(void);
extern char lc_x_server[48];
static void *g_xs;
static struct obs_source_info *g_xi = &xshm_input_v2;
#define IS(s) (!strcmp(a[0], s))

static void state(void)
{
	struct xshm_data *d = g_xs;
	printf("state screen=%lld origin=%lld,%lld size=%lldx%lld cut=t%lld,l%lld,r%lld,b%lld adj=%lld,%lld %lldx%lld cursor=%d "
	       "advanced=%d randr=%d xinerama=%d server=%s texture=%d xcb=%d shm=%d cursorobj=%d get=%ux%u\n",
	       (long long)d->screen_id, (long long)d->x_org, (long long)d->y_org, (long long)d->width, (long long)d->height,
	       (long long)d->cut_top, (long long)d->cut_left, (long long)d->cut_right, (long long)d->cut_bot,
	       (long long)d->adj_x_org, (long long)d->adj_y_org, (long long)d->adj_width, (long long)d->adj_height, d->show_cursor,
	       d->advanced, d->use_randr, d->use_xinerama, d->server ? d->server : "(null)", d->texture != NULL, d->xcb != NULL,
	       d->xshm != NULL, d->cursor != NULL, xshm_getwidth(d), xshm_getheight(d));
}

int lc_xshm_cmd(struct script_line *sl)
{
	char **a = sl->argv;
	if (IS("xshm-version")) { script_need(sl, 2); g_xi = atoi(a[1]) == 1 ? &xshm_input : &xshm_input_v2; return 1; }
	if (IS("xshm-info")) {
		printf("info id=%s name=%s obsolete=%d srgb=%d custom_draw=%d\n", g_xi->id, g_xi->get_name(NULL), !!(g_xi->output_flags & OBS_SOURCE_CAP_OBSOLETE),
		       !!(g_xi->output_flags & OBS_SOURCE_SRGB), !!(g_xi->output_flags & OBS_SOURCE_CUSTOM_DRAW));
		return 1;
	}
	if (IS("xshm-defaults")) { g_xi->get_defaults(lc_settings); return 1; }
	if (IS("xshm-create")) { g_xs = g_xi->create(lc_settings, (obs_source_t *)(uintptr_t)1); printf("xshm-create\n"); state(); return 1; }
	if (IS("xshm-update")) { g_xi->update(g_xs, lc_settings); state(); return 1; }
	if (IS("xshm-destroy")) { g_xi->destroy(g_xs); g_xs = NULL; printf("xshm-destroy textures=%d\n", lc_tex_alive()); return 1; }
	if (IS("xshm-state")) { state(); return 1; }
	if (IS("xshm-props")) {
		if (lc_props) obs_properties_destroy(lc_props);
		lc_props = g_xi->get_properties(g_xs);
		return 1;
	}
	if (IS("xshm-last-server")) { printf("last connect server=%s\n", lc_x_server); return 1; }
	if (IS("xshm-geometry")) { printf("update_geometry ret=%d\n", (int)xshm_update_geometry(g_xs)); state(); return 1; }
	if (IS("xshm-mode")) { struct xshm_data *d = g_xs; script_need(sl, 2); d->use_randr = !strcmp(a[1], "randr"); d->use_xinerama = !strcmp(a[1], "xinerama"); return 1; }
	return 0;
}
