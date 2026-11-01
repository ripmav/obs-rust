/* Conformance wrapper (area linuxcapture): includes the real linux-v4l2 sources (helpers, controls, input) against fake libv4l2. */
#include <dirent.h>
#include <pthread.h>
int lc_pthread_create(pthread_t *, const pthread_attr_t *, void *(*)(void *), void *);
int lc_pthread_join(pthread_t, void **);
#define pthread_create lc_pthread_create
#define pthread_join lc_pthread_join
DIR *lc_opendir(const char *);
struct dirent *lc_readdir(DIR *);
int lc_closedir(DIR *);
#define opendir lc_opendir
#define readdir lc_readdir
#define closedir lc_closedir
#include "../../../plugins/linux-v4l2/v4l2-helpers.c"
#undef blog
#include "../../../plugins/linux-v4l2/v4l2-controls.c"
#undef blog
#include "../../../plugins/linux-v4l2/v4l2-input.c"
#undef opendir
#undef readdir
#undef closedir
#include "linuxcapture_api.h"

static void *g_src;
static int g_fd = -1;
static struct v4l2_buffer_data g_bufs;
#define IS(s) (!strcmp(a[0], s))

static const char *obsfmt_name(enum video_format f)
{
	static const char *n[] = {"NONE", "I420", "NV12", "YVYU", "YUY2", "UYVY", "RGBA", "BGRA", "BGRX", "Y800", "I444",
				  "BGR3", "I422", "I40A", "I42A", "YUVA", "AYUV", "I010", "P010", "I210", "I412", "YA2L",
				  "P216", "P416", "V210", "R10L"};
	return (unsigned)f < sizeof(n) / sizeof(n[0]) ? n[f] : "?";
}
static unsigned fcc(const char *s)
{
	return strlen(s) == 4 ? v4l2_fourcc(s[0], s[1], s[2], s[3]) : (unsigned)strtoul(s, NULL, 0);
}
static void state(void)
{
	struct v4l2_data *d = g_src;
	printf("state device_id=%s input=%d pixfmt=%d standard=%d dv_timing=%d resolution=%lld framerate=%lld color_range=%d "
	       "auto_reset=%d timeout_frames=%d dev=%s width=%d height=%d linesize=%d buffers=%u res_unch=%d fr_unch=%d thread=%d\n",
	       d->device_id ? d->device_id : "(null)", d->input, d->pixfmt, d->standard, d->dv_timing, (long long)d->resolution,
	       (long long)d->framerate, d->color_range, d->auto_reset, d->timeout_frames, d->dev == -1 ? "closed" : "open",
	       d->width, d->height, d->linesize, (unsigned)d->buffers.count, d->resolution_unchanged, d->framerate_unchanged,
	       d->thread != 0);
}

int lc_v4l2_cmd(struct script_line *sl)
{
	char **a = sl->argv;
	if (IS("v4l2-pack")) { script_need(sl, 3); printf("pack %lld\n", (long long)v4l2_pack_tuple(atoi(a[1]), atoi(a[2]))); return 1; }
	if (IS("v4l2-unpack")) {
		script_need(sl, 2);
		int x, y;
		v4l2_unpack_tuple(&x, &y, strtoll(a[1], NULL, 0));
		printf("unpack %d %d\n", x, y);
		return 1;
	}
	if (IS("v4l2-obsfmt")) { script_need(sl, 2); printf("obsfmt %s\n", obsfmt_name(v4l2_to_obs_video_format(fcc(a[1])))); return 1; }
	if (IS("v4l2-tables")) {
		for (const int64_t *p = v4l2_framesizes; *p; p++) { int w, h; v4l2_unpack_tuple(&w, &h, *p); printf("framesize %dx%d\n", w, h); }
		for (const int64_t *p = v4l2_framerates; *p; p++) { int w, h; v4l2_unpack_tuple(&w, &h, *p); printf("framerate %d/%d\n", w, h); }
		return 1;
	}
	if (IS("h-open")) { script_need(sl, 2); g_fd = v4l2_open(a[1], 0); printf("h-open -> %s\n", g_fd == -1 ? "-1" : "fd"); return 1; }
	if (IS("h-set-input")) { script_need(sl, 2); int v = atoi(a[1]); int r = (int)v4l2_set_input(g_fd, &v); printf("set_input ret=%d value=%d\n", r, v); return 1; }
	if (IS("h-input-caps")) { script_need(sl, 2); uint32_t c = 0xdead; int r = (int)v4l2_get_input_caps(g_fd, atoi(a[1]), &c); printf("input_caps ret=%d caps=0x%x\n", r, c == 0xdead ? 0 : c); return 1; }
	if (IS("h-set-format")) {
		script_need(sl, 3);
		int64_t res = strtoll(a[1], NULL, 0); int pix = !strcmp(a[2], "-1") ? -1 : (int)fcc(a[2]); int bpl = 0;
		int r = (int)v4l2_set_format(g_fd, &res, &pix, &bpl);
		printf("set_format ret=%d resolution=%lld pixfmt=0x%x bytesperline=%d\n", r, (long long)res, pix, bpl);
		return 1;
	}
	if (IS("h-set-framerate")) { script_need(sl, 2); int64_t f = strtoll(a[1], NULL, 0); int r = (int)v4l2_set_framerate(g_fd, &f); printf("set_framerate ret=%d framerate=%lld\n", r, (long long)f); return 1; }
	if (IS("h-set-standard")) { script_need(sl, 2); int s = atoi(a[1]); int r = (int)v4l2_set_standard(g_fd, &s); printf("set_standard ret=%d value=%d\n", r, s); return 1; }
	if (IS("h-set-dv")) { script_need(sl, 2); int t = atoi(a[1]); printf("set_dv ret=%d\n", (int)v4l2_set_dv_timing(g_fd, &t)); return 1; }
	if (IS("h-null-args")) { /* NULL-argument handling of the helpers */
		int v = 0; int64_t q = 0;
		printf("null set_input(dev=0)=%d set_input(NULL)=%d set_format(NULL)=%d set_framerate(NULL)=%d set_standard(NULL)=%d set_dv(NULL)=%d caps(NULL)=%d\n",
		       (int)v4l2_set_input(0, &v), (int)v4l2_set_input(g_fd, NULL), (int)v4l2_set_format(g_fd, NULL, &v, &v),
		       (int)v4l2_set_framerate(g_fd, NULL), (int)v4l2_set_standard(g_fd, NULL), (int)v4l2_set_dv_timing(g_fd, NULL),
		       (int)v4l2_get_input_caps(g_fd, 0, NULL));
		(void)q;
		return 1;
	}
	if (IS("h-create-mmap")) {
		int r = (int)v4l2_create_mmap(g_fd, &g_bufs);
		printf("create_mmap ret=%d count=%u", r, (unsigned)g_bufs.count);
		for (unsigned i = 0; r == 0 && i < g_bufs.count; i++) printf(" len%u=%zu", i, g_bufs.info[i].length);
		printf("\n");
		return 1;
	}
	if (IS("h-destroy-mmap")) { printf("destroy_mmap ret=%d\n", (int)v4l2_destroy_mmap(&g_bufs)); printf("count=%u\n", (unsigned)g_bufs.count); return 1; }
	if (IS("h-start")) { printf("start_capture ret=%d\n", (int)v4l2_start_capture(g_fd, &g_bufs)); return 1; }
	if (IS("h-stop")) { printf("stop_capture ret=%d\n", (int)v4l2_stop_capture(g_fd)); return 1; }
	if (IS("h-reset")) { printf("reset_capture ret=%d\n", (int)v4l2_reset_capture(g_fd, &g_bufs)); return 1; }
	if (IS("h-controls")) {
		obs_properties_t *p = obs_properties_create();
		int r = (int)v4l2_update_controls(g_fd, p, lc_settings);
		printf("update_controls ret=%d\n", r);
		lc_dump_props(p, 1);
		obs_properties_destroy(p);
		return 1;
	}
	if (IS("src-create")) {
		g_src = v4l2_input.create(lc_settings, (obs_source_t *)(uintptr_t)1);
		printf("src-create\n");
		state();
		return 1;
	}
	if (IS("src-update")) { v4l2_input.update(g_src, lc_settings); state(); return 1; }
	if (IS("src-destroy")) { v4l2_input.destroy(g_src); g_src = NULL; printf("src-destroy\n"); return 1; }
	if (IS("src-defaults")) { v4l2_input.get_defaults(lc_settings); return 1; }
	if (IS("src-props")) {
		if (lc_props) obs_properties_destroy(lc_props);
		lc_props = v4l2_input.get_properties(g_src);
		return 1;
	}
	if (IS("src-thread-run")) { /* run the capture thread body once with its stop event already signalled */
		struct v4l2_data *d = g_src;
		if (!d->thread) { printf("no thread\n"); return 1; }
		os_event_signal(d->event);
		v4l2_thread(d);
		return 1;
	}
	if (IS("src-state")) { state(); return 1; }
	if (IS("src-name")) { printf("name %s\n", v4l2_input.get_name(NULL)); printf("flags async_video=%d do_not_duplicate=%d\n", !!(v4l2_input.output_flags & OBS_SOURCE_ASYNC_VIDEO), !!(v4l2_input.output_flags & OBS_SOURCE_DO_NOT_DUPLICATE)); return 1; }
	if (IS("src-changed")) { printf("settings_changed %d\n", v4l2_settings_changed(g_src, lc_settings)); state(); return 1; }
	if (IS("src-field")) { /* name value */
		script_need(sl, 3);
		struct v4l2_data *d = g_src; long long v = strtoll(a[2], NULL, 0);
		if (!strcmp(a[1], "pixfmt")) d->pixfmt = a[2][0] >= '0' && a[2][0] <= '9' ? (int)v : (int)fcc(a[2]);
		else if (!strcmp(a[1], "width")) d->width = v;
		else if (!strcmp(a[1], "height")) d->height = v;
		else if (!strcmp(a[1], "linesize")) d->linesize = v;
		else if (!strcmp(a[1], "color_range")) d->color_range = v;
		return 1;
	}
	if (IS("src-prep-frame")) {
		struct obs_source_frame f; size_t off[MAX_AV_PLANES];
		v4l2_prep_obs_frame(g_src, &f, off);
		printf("frame format=%s %dx%d flags=%u linesize=%u,%u,%u offsets=%zu,%zu,%zu range=%.9g..%.9g\n", obsfmt_name(f.format),
		       f.width, f.height, f.flags, f.linesize[0], f.linesize[1], f.linesize[2], off[0], off[1], off[2],
		       f.color_range_min[0], f.color_range_max[0]);
		return 1;
	}
	return 0;
}
