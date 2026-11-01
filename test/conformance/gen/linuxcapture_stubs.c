/*
 * Conformance fakes (area linuxcapture, spec S-049). Test tooling, not part of OBS.
 * Provides: a minimal fake OBS core (module text, source object, audio/video output recorders),
 * a fake libv4l2 + kernel V4L2 device model, a fake PulseAudio wrapper/stream, and a fake
 * xcb/X11 helper layer. All behaviour is configured by script commands (lc_fake_cmd).
 */
#define _GNU_SOURCE
#include <errno.h>
#include <dirent.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <linux/videodev2.h>
#include <libv4l2.h>
#include <pulse/pulseaudio.h>
#include <xcb/xcb.h>
#include <xcb/randr.h>
#include <xcb/shm.h>
#include <xcb/xinerama.h>
#include <obs-module.h>
#include <util/bmem.h>
#include "linuxcapture_api.h"
#include "../../../plugins/linux-pulseaudio/pulse-wrapper.h"
#include "../../../plugins/linux-v4l2/v4l2-decoder.h"
#include "../../../plugins/linux-capture/xhelpers.h"
#include "../../../plugins/linux-capture/xcursor-xcb.h"

int lc_trace;
char lc_source_name[64] = "Src";
bool lc_async_unbuffered;

void lc_trace_printf(const char *fmt, ...)
{
	if (!lc_trace)
		return;
	va_list ap;
	va_start(ap, fmt);
	fputs("  | ", stdout);
	vprintf(fmt, ap);
	fputc('\n', stdout);
	va_end(ap);
}
#define TR(...) lc_trace_printf(__VA_ARGS__)

/* ---------------------------------------------------------------- fake OBS core */
const char *obs_module_text(const char *key) { return key; }
obs_data_t *obs_source_get_settings(const obs_source_t *s) { (void)s; obs_data_addref(lc_settings); return lc_settings; }
const char *obs_source_get_name(const obs_source_t *s) { (void)s; return lc_source_name; }
void obs_source_set_async_unbuffered(obs_source_t *s, bool u) { (void)s; lc_async_unbuffered = u; TR("set_async_unbuffered %d", u); }
void obs_source_update_properties(obs_source_t *s) { (void)s; TR("update_properties"); }
void obs_source_output_video(obs_source_t *s, const struct obs_source_frame *f) { (void)s; (void)f; }

static uint64_t g_now_ns;
uint64_t os_gettime_ns(void) { return g_now_ns; }

void obs_source_audio_output_capture_device_changed(obs_source_t *s, const char *dev)
{
	(void)s;
	printf("event capture_device_changed %s\n", dev ? dev : "(null)");
}
void obs_source_output_audio(obs_source_t *s, const struct obs_source_audio *a)
{
	(void)s;
	printf("event output_audio speakers=%d rate=%u format=%d frames=%u ts=%llu\n", (int)a->speakers, a->samples_per_sec,
	       (int)a->format, a->frames, (unsigned long long)a->timestamp);
}
void obs_enter_graphics(void) {}
void obs_leave_graphics(void) {}
bool obs_source_showing(const obs_source_t *s) { (void)s; return true; }

/* graphics (xshm) -- only creation/destruction is observable */
static int g_tex_alive;
gs_texture_t *gs_texture_create(uint32_t w, uint32_t h, enum gs_color_format f, uint32_t lv, const uint8_t **d, uint32_t fl)
{
	(void)lv; (void)d; (void)fl;
	g_tex_alive++;
	TR("gs_texture_create %ux%u fmt=%d", w, h, (int)f);
	return (gs_texture_t *)(uintptr_t)(g_tex_alive + 1);
}
void gs_texture_destroy(gs_texture_t *t) { (void)t; g_tex_alive--; TR("gs_texture_destroy"); }
int lc_tex_alive(void) { return g_tex_alive; }
void gs_texture_set_image(gs_texture_t *t, const uint8_t *d, uint32_t l, bool f) { (void)t; (void)d; (void)l; (void)f; }
bool gs_get_linear_srgb(void) { return false; }
bool gs_framebuffer_srgb_enabled(void) { return false; }
void gs_enable_framebuffer_srgb(bool e) { (void)e; }
gs_eparam_t *gs_effect_get_param_by_name(const gs_effect_t *e, const char *n) { (void)e; (void)n; return NULL; }
void gs_effect_set_texture(gs_eparam_t *p, gs_texture_t *t) { (void)p; (void)t; }
void gs_effect_set_texture_srgb(gs_eparam_t *p, gs_texture_t *t) { (void)p; (void)t; }
bool gs_effect_loop(gs_effect_t *e, const char *n) { (void)e; (void)n; return false; }
void gs_draw_sprite(gs_texture_t *t, uint32_t f, uint32_t w, uint32_t h) { (void)t; (void)f; (void)w; (void)h; }
gs_effect_t *obs_get_base_effect(enum obs_base_effect e) { (void)e; return NULL; }

/* the capture thread is never really started: the thread body is run on demand by the wrapper */
#include <pthread.h>
int lc_pthread_create(pthread_t *t, const pthread_attr_t *a, void *(*fn)(void *), void *arg)
{
	(void)a; (void)fn; (void)arg;
	*t = (pthread_t)1;
	TR("thread start");
	return 0;
}
int lc_pthread_join(pthread_t t, void **r) { (void)t; (void)r; TR("thread join"); return 0; }

/* ---------------------------------------------------------------- failure injection (shared) */
#define MAXFAIL 16
static char g_fail[MAXFAIL][24];
static int g_nfail;
static bool failing(const char *name)
{
	for (int i = 0; i < g_nfail; i++)
		if (!strcmp(g_fail[i], name))
			return true;
	return false;
}

/* ---------------------------------------------------------------- fake V4L2 kernel + libv4l2 */
#define MAXDEV 6
#define MAXN 16
struct fk_dev {
	bool used;
	char path[48];
	char card[32], bus[32];
	unsigned caps, devcaps;
	struct { char name[32]; unsigned caps; } in[MAXN];
	int nin;
	struct { unsigned fourcc; char desc[32]; unsigned flags; } fmt[MAXN];
	int nfmt;
	struct { char name[24]; uint64_t id; } std[MAXN];
	int nstd;
	struct v4l2_dv_timings dv[MAXN];
	int ndv;
	struct { unsigned fourcc, w, h; } sz[MAXN * 2];
	int nsz;
	unsigned szstep[4];
	int nszstep;
	struct { unsigned fourcc, w, h, num, den; } iv[MAXN * 2];
	int niv;
	bool ivstep;
	struct v4l2_queryctrl ctrl[MAXN];
	int nctrl;
	struct { unsigned ctrl, index; char name[32]; } menu[MAXN * 2];
	int nmenu;
	int cur_input;
	unsigned cw, ch, cpix, cbpl;
	unsigned tnum, tden;
	uint64_t cstd;
	unsigned reqbufs, bufsize;
};
static struct fk_dev g_dev[MAXDEV];
static struct fk_dev *g_cur;
static char g_dir[MAXN * 2][32];
static int g_ndir = -1; /* -1: opendir fails */
static bool g_decoder_fail;
static bool g_mmap_fail;
static char g_arena[16];

static struct fk_dev *dev_find(const char *path)
{
	for (int i = 0; i < MAXDEV; i++)
		if (g_dev[i].used && !strcmp(g_dev[i].path, path))
			return &g_dev[i];
	return NULL;
}

struct fk_dir { int pos; struct dirent ent; };
DIR *lc_opendir(const char *p)
{
	(void)p;
	TR("opendir /sys/class/video4linux -> %s", g_ndir < 0 ? "fail" : "ok");
	if (g_ndir < 0)
		return NULL;
	struct fk_dir *d = calloc(1, sizeof(*d));
	return (DIR *)d;
}
struct dirent *lc_readdir(DIR *dd)
{
	struct fk_dir *d = (struct fk_dir *)dd;
	if (d->pos >= g_ndir)
		return NULL;
	const char *n = g_dir[d->pos++];
	memset(&d->ent, 0, sizeof(d->ent));
	if (!strncmp(n, "d:", 2)) {
		d->ent.d_type = DT_DIR;
		n += 2;
	} else {
		d->ent.d_type = DT_LNK;
	}
	snprintf(d->ent.d_name, sizeof(d->ent.d_name), "%s", n);
	return &d->ent;
}
int lc_closedir(DIR *dd) { free(dd); return 0; }

int v4l2_open(const char *file, int oflag, ...)
{
	(void)oflag;
	struct fk_dev *d = dev_find(file);
	bool ok = d && !failing("open");
	TR("open %s -> %s", file, ok ? "ok" : "fail");
	if (!ok) {
		errno = ENOENT;
		return -1;
	}
	return 3 + (int)(d - g_dev);
}
int v4l2_close(int fd) { TR("close fd%d", fd - 3); return 0; }
void *v4l2_mmap(void *start, size_t length, int prot, int flags, int fd, int64_t offset)
{
	(void)start; (void)prot; (void)flags; (void)fd;
	static int n;
	TR("mmap len=%zu offset=%lld -> %s", length, (long long)offset, g_mmap_fail ? "MAP_FAILED" : "ok");
	if (g_mmap_fail)
		return MAP_FAILED;
	return &g_arena[(n++) % 16];
}
int v4l2_munmap(void *start, size_t length) { (void)start; TR("munmap len=%zu", length); return 0; }

static const char *ioctl_name(unsigned long r)
{
	switch (r) {
#define N(x) case x: return #x + 7;
		N(VIDIOC_QUERYCAP) N(VIDIOC_ENUMINPUT) N(VIDIOC_G_INPUT) N(VIDIOC_S_INPUT) N(VIDIOC_ENUM_FMT) N(VIDIOC_ENUMSTD)
		N(VIDIOC_ENUM_DV_TIMINGS) N(VIDIOC_S_DV_TIMINGS) N(VIDIOC_ENUM_FRAMESIZES) N(VIDIOC_ENUM_FRAMEINTERVALS)
		N(VIDIOC_G_FMT) N(VIDIOC_S_FMT) N(VIDIOC_G_PARM) N(VIDIOC_S_PARM) N(VIDIOC_G_STD) N(VIDIOC_S_STD)
		N(VIDIOC_QUERYCTRL) N(VIDIOC_QUERYMENU) N(VIDIOC_S_CTRL) N(VIDIOC_REQBUFS) N(VIDIOC_QUERYBUF) N(VIDIOC_QBUF)
		N(VIDIOC_DQBUF) N(VIDIOC_STREAMON) N(VIDIOC_STREAMOFF) N(VIDIOC_LOG_STATUS)
#undef N
	}
	return "?";
}
static void cstr(char *dst, size_t n, const char *s) { snprintf(dst, n, "%s", s); }

int v4l2_ioctl(int fd, unsigned long int request, ...)
{
	va_list ap;
	va_start(ap, request);
	void *arg = va_arg(ap, void *);
	va_end(ap);
	struct fk_dev *d = &g_dev[fd - 3];
	const char *nm = ioctl_name(request);
	if (failing(nm)) {
		TR("ioctl %s -> -1 (injected)", nm);
		errno = EINVAL;
		return -1;
	}
	int rc = 0;
	switch (request) {
	case VIDIOC_QUERYCAP: {
		struct v4l2_capability *c = arg;
		memset(c, 0, sizeof(*c));
		cstr((char *)c->card, sizeof(c->card), d->card);
		cstr((char *)c->bus_info, sizeof(c->bus_info), d->bus);
		c->capabilities = d->caps;
		c->device_caps = d->devcaps;
		break;
	}
	case VIDIOC_ENUMINPUT: {
		struct v4l2_input *in = arg;
		if (in->index >= (unsigned)d->nin) { rc = -1; break; }
		cstr((char *)in->name, sizeof(in->name), d->in[in->index].name);
		in->capabilities = d->in[in->index].caps;
		break;
	}
	case VIDIOC_G_INPUT: *(int *)arg = d->cur_input; TR("ioctl G_INPUT -> %d", d->cur_input); return 0;
	case VIDIOC_S_INPUT: d->cur_input = *(int *)arg; TR("ioctl S_INPUT %d", d->cur_input); return 0;
	case VIDIOC_ENUM_FMT: {
		struct v4l2_fmtdesc *f = arg;
		if (f->index >= (unsigned)d->nfmt) { rc = -1; break; }
		f->pixelformat = d->fmt[f->index].fourcc;
		f->flags = d->fmt[f->index].flags;
		cstr((char *)f->description, sizeof(f->description), d->fmt[f->index].desc);
		break;
	}
	case VIDIOC_ENUMSTD: {
		struct v4l2_standard *s = arg;
		if (s->index >= (unsigned)d->nstd) { rc = -1; break; }
		s->id = d->std[s->index].id;
		cstr((char *)s->name, sizeof(s->name), d->std[s->index].name);
		break;
	}
	case VIDIOC_ENUM_DV_TIMINGS: {
		struct v4l2_enum_dv_timings *e = arg;
		if (e->index >= (unsigned)d->ndv) { rc = -1; break; }
		e->timings = d->dv[e->index];
		break;
	}
	case VIDIOC_S_DV_TIMINGS: {
		struct v4l2_dv_timings *t = arg;
		TR("ioctl S_DV_TIMINGS %ux%u", t->bt.width, t->bt.height);
		return 0;
	}
	case VIDIOC_ENUM_FRAMESIZES: {
		struct v4l2_frmsizeenum *f = arg;
		/* nszstep: fourccs reported as stepwise */
		for (int i = 0; i < d->nszstep; i++)
			if (d->szstep[i] == f->pixel_format) {
				if (f->index) { rc = -1; break; }
				f->type = V4L2_FRMSIZE_TYPE_STEPWISE;
				goto done;
			}
		int k = 0, found = -1;
		for (int i = 0; i < d->nsz; i++)
			if (d->sz[i].fourcc == f->pixel_format && k++ == (int)f->index) { found = i; break; }
		if (found < 0) {
			/* NOTE: a real kernel leaves *f untouched; the fake zeroes type so the reference's
			 * uninitialised read becomes deterministic (see spec defect D-2). */
			f->type = 0;
			rc = -1;
			break;
		}
		f->type = V4L2_FRMSIZE_TYPE_DISCRETE;
		f->discrete.width = d->sz[found].w;
		f->discrete.height = d->sz[found].h;
		break;
	}
	case VIDIOC_ENUM_FRAMEINTERVALS: {
		struct v4l2_frmivalenum *f = arg;
		if (d->ivstep) {
			if (f->index) { rc = -1; break; }
			f->type = V4L2_FRMIVAL_TYPE_STEPWISE;
			break;
		}
		int k = 0, found = -1;
		for (int i = 0; i < d->niv; i++)
			if (d->iv[i].fourcc == f->pixel_format && d->iv[i].w == f->width && d->iv[i].h == f->height &&
			    k++ == (int)f->index) { found = i; break; }
		if (found < 0) { f->type = 0; rc = -1; break; }
		f->type = V4L2_FRMIVAL_TYPE_DISCRETE;
		f->discrete.numerator = d->iv[found].num;
		f->discrete.denominator = d->iv[found].den;
		break;
	}
	case VIDIOC_G_FMT: {
		struct v4l2_format *f = arg;
		f->fmt.pix.width = d->cw; f->fmt.pix.height = d->ch;
		f->fmt.pix.pixelformat = d->cpix; f->fmt.pix.bytesperline = d->cbpl;
		TR("ioctl G_FMT -> %ux%u %08x bpl=%u", d->cw, d->ch, d->cpix, d->cbpl);
		return 0;
	}
	case VIDIOC_S_FMT: {
		struct v4l2_format *f = arg;
		d->cw = f->fmt.pix.width; d->ch = f->fmt.pix.height; d->cpix = f->fmt.pix.pixelformat;
		TR("ioctl S_FMT %ux%u %08x", d->cw, d->ch, d->cpix);
		return 0;
	}
	case VIDIOC_G_PARM: {
		struct v4l2_streamparm *p = arg;
		p->parm.capture.timeperframe.numerator = d->tnum;
		p->parm.capture.timeperframe.denominator = d->tden;
		TR("ioctl G_PARM -> %u/%u", d->tnum, d->tden);
		return 0;
	}
	case VIDIOC_S_PARM: {
		struct v4l2_streamparm *p = arg;
		d->tnum = p->parm.capture.timeperframe.numerator;
		d->tden = p->parm.capture.timeperframe.denominator;
		TR("ioctl S_PARM %u/%u", d->tnum, d->tden);
		return 0;
	}
	/* NOTE: the reference passes a 4-byte int; a real kernel reads/writes 8 bytes (spec defect D-3). The fake models the low 32 bits only. */
	case VIDIOC_G_STD: *(int32_t *)arg = (int32_t)d->cstd; TR("ioctl G_STD -> %llu", (unsigned long long)d->cstd); return 0;
	case VIDIOC_S_STD: d->cstd = (uint32_t)*(int32_t *)arg; TR("ioctl S_STD %llu", (unsigned long long)d->cstd); return 0;
	case VIDIOC_QUERYCTRL: {
		struct v4l2_queryctrl *q = arg;
		/* NEXT_CTRL: first control with id strictly greater than (id & ~NEXT) */
		unsigned base = q->id & ~V4L2_CTRL_FLAG_NEXT_CTRL;
		int best = -1;
		for (int i = 0; i < d->nctrl; i++)
			if (d->ctrl[i].id > base && (best < 0 || d->ctrl[i].id < d->ctrl[best].id))
				best = i;
		if (best < 0) { rc = -1; break; }
		*q = d->ctrl[best];
		break;
	}
	case VIDIOC_QUERYMENU: {
		struct v4l2_querymenu *m = arg;
		rc = -1;
		for (int i = 0; i < d->nmenu; i++)
			if (d->menu[i].ctrl == m->id && d->menu[i].index == m->index) {
				cstr((char *)m->name, sizeof(m->name), d->menu[i].name);
				rc = 0;
			}
		break;
	}
	case VIDIOC_S_CTRL: {
		struct v4l2_control *c = arg;
		TR("ioctl S_CTRL id=%u value=%d", c->id, c->value);
		return 0;
	}
	case VIDIOC_REQBUFS: {
		struct v4l2_requestbuffers *r = arg;
		TR("ioctl REQBUFS count=%u -> %u", r->count, d->reqbufs);
		r->count = d->reqbufs;
		return 0;
	}
	case VIDIOC_QUERYBUF: {
		struct v4l2_buffer *b = arg;
		b->length = d->bufsize;
		b->m.offset = b->index * d->bufsize;
		TR("ioctl QUERYBUF index=%u", b->index);
		return 0;
	}
	case VIDIOC_QBUF: TR("ioctl QBUF index=%u", ((struct v4l2_buffer *)arg)->index); return 0;
	case VIDIOC_STREAMON: TR("ioctl STREAMON"); return 0;
	case VIDIOC_STREAMOFF: TR("ioctl STREAMOFF"); return 0;
	case VIDIOC_LOG_STATUS: TR("ioctl LOG_STATUS"); return 0;
	default: rc = -1;
	}
done:
	if (rc < 0)
		errno = EINVAL;
	return rc;
}

int v4l2_init_decoder(struct v4l2_decoder *dec, int pixfmt)
{
	(void)dec;
	TR("decoder init %08x -> %s", pixfmt, g_decoder_fail ? "fail" : "ok");
	return g_decoder_fail ? -1 : 0;
}
void v4l2_destroy_decoder(struct v4l2_decoder *dec) { (void)dec; TR("decoder destroy"); }
int v4l2_decode_frame(struct obs_source_frame *o, uint8_t *d, size_t l, struct v4l2_decoder *dec)
{
	(void)o; (void)d; (void)l; (void)dec;
	return 0;
}

static unsigned fourcc_of(const char *s)
{
	return strlen(s) == 4 ? v4l2_fourcc(s[0], s[1], s[2], s[3]) : (unsigned)strtoul(s, NULL, 0);
}

/* ---------------------------------------------------------------- fake PulseAudio */
#define MAXPA 8
static struct { char name[48], desc[48]; int monitor_of; int fmt; unsigned rate; int ch; } g_psrc[MAXPA];
static int g_npsrc;
static struct { char name[48], desc[48]; int mon_idx; char mon_name[56]; } g_psink[MAXPA];
static int g_npsink;
static char g_srv_name[32] = "pulseaudio", g_srv_ver[16] = "16.1", g_def_src[48] = "src0", g_def_sink[48] = "sink0";
static struct { bool present; size_t bytes; bool hole; } g_chunk;
static struct pa_stream { int x; } g_stream_obj;
static pa_stream_request_cb_t g_read_cb;
static void *g_read_ud;
static int g_pa_inited;

int_fast32_t pulse_init(void) { g_pa_inited++; TR("pulse_init"); return failing("ctx") ? -1 : 0; }
void pulse_unref(void) { g_pa_inited--; TR("pulse_unref"); }
void pulse_lock(void) {}
void pulse_unlock(void) {}
void pulse_signal(int w) { (void)w; }
void pulse_wait(void) {}
void pulse_accept(void) {}
int lc_pa_inited(void) { return g_pa_inited; }

int_fast32_t pulse_get_server_info(pa_server_info_cb_t cb, void *ud)
{
	if (failing("server"))
		return -1;
	pa_server_info i;
	memset(&i, 0, sizeof(i));
	i.server_name = g_srv_name;
	i.server_version = g_srv_ver;
	i.default_source_name = g_def_src;
	i.default_sink_name = g_def_sink;
	cb(NULL, &i, ud);
	return 0;
}
static void fill_src(pa_source_info *i, int k)
{
	memset(i, 0, sizeof(*i));
	i->name = g_psrc[k].name;
	i->description = g_psrc[k].desc;
	i->monitor_of_sink = g_psrc[k].monitor_of < 0 ? PA_INVALID_INDEX : (uint32_t)g_psrc[k].monitor_of;
	i->sample_spec.format = g_psrc[k].fmt;
	i->sample_spec.rate = g_psrc[k].rate;
	i->sample_spec.channels = g_psrc[k].ch;
}
int_fast32_t pulse_get_source_info(pa_source_info_cb_t cb, const char *name, void *ud)
{
	if (failing("source"))
		return -1;
	for (int k = 0; k < g_npsrc; k++)
		if (!strcmp(name, g_psrc[k].name)) {
			pa_source_info i;
			fill_src(&i, k);
			cb(NULL, &i, 0, ud);
			cb(NULL, NULL, 1, ud);
			return 0;
		}
	cb(NULL, NULL, -1, ud); /* PA_ERR_NOENTITY */
	return 0;
}
int_fast32_t pulse_get_source_info_list(pa_source_info_cb_t cb, void *ud)
{
	for (int k = 0; k < g_npsrc; k++) {
		pa_source_info i;
		fill_src(&i, k);
		cb(NULL, &i, 0, ud);
	}
	cb(NULL, NULL, 1, ud);
	return 0;
}
int_fast32_t pulse_get_sink_info_list(pa_sink_info_cb_t cb, void *ud)
{
	for (int k = 0; k < g_npsink; k++) {
		pa_sink_info i;
		memset(&i, 0, sizeof(i));
		i.name = g_psink[k].name;
		i.description = g_psink[k].desc;
		i.monitor_source = g_psink[k].mon_idx < 0 ? PA_INVALID_INDEX : (uint32_t)g_psink[k].mon_idx;
		i.monitor_source_name = g_psink[k].mon_name;
		cb(NULL, &i, 0, ud);
	}
	cb(NULL, NULL, 1, ud);
	return 0;
}
pa_stream *pulse_stream_new(const char *name, const pa_sample_spec *ss, const pa_channel_map *map)
{
	if (failing("stream"))
		return NULL;
	printf("event stream_new name=\"%s\" format=%s rate=%u channels=%u map=", name, pa_sample_format_to_string(ss->format),
	       ss->rate, ss->channels);
	for (int i = 0; i < map->channels; i++)
		printf("%s%s", i ? "," : "", pa_channel_position_to_string(map->map[i]));
	printf("\n");
	return &g_stream_obj;
}
void pa_stream_set_read_callback(pa_stream *p, pa_stream_request_cb_t cb, void *ud) { (void)p; g_read_cb = cb; g_read_ud = ud; }
int pa_stream_connect_record(pa_stream *s, const char *dev, const pa_buffer_attr *attr, pa_stream_flags_t flags)
{
	(void)s;
	printf("event connect_record dev=\"%s\" fragsize=%u maxlength=%u flags=0x%x\n", dev, attr->fragsize, attr->maxlength,
	       (unsigned)flags);
	return failing("connect") ? -1 : 0;
}
int pa_stream_disconnect(pa_stream *s) { (void)s; printf("event stream_disconnect\n"); return 0; }
void pa_stream_unref(pa_stream *s) { (void)s; }
static char g_zero[1 << 16];
int pa_stream_peek(pa_stream *s, const void **data, size_t *nbytes)
{
	(void)s;
	*nbytes = g_chunk.present ? g_chunk.bytes : 0;
	*data = (g_chunk.present && !g_chunk.hole) ? g_zero : NULL;
	return 0;
}
int pa_stream_drop(pa_stream *s) { (void)s; printf("event stream_drop\n"); g_chunk.present = false; return 0; }
void lc_pa_deliver(void) { if (g_read_cb) g_read_cb(&g_stream_obj, g_chunk.bytes, g_read_ud); }

/* ---------------------------------------------------------------- fake X11 */
#define MAXSCR 8
static struct { int x, y, w, h; char name[24]; bool fail; } g_scr[MAXSCR];
static int g_nscr;
static bool g_x_randr, g_x_xin, g_x_shm = true, g_x_connect_fail;
static xcb_connection_t *g_xconn = (xcb_connection_t *)&g_scr;
static xcb_screen_t g_xscreen[MAXSCR];
static xcb_shm_t g_shm;
static xcb_xcursor_t *g_cursor = (xcb_xcursor_t *)&g_xscreen;
char lc_x_server[48];

xcb_connection_t *xcb_connect(const char *name, int *scr)
{
	(void)scr;
	snprintf(lc_x_server, sizeof(lc_x_server), "%s", name ? name : "(null)");
	TR("xcb_connect %s -> %s", name ? name : "(null)", g_x_connect_fail ? "error" : "ok");
	return g_xconn;
}
int xcb_connection_has_error(xcb_connection_t *c) { (void)c; return g_x_connect_fail; }
void xcb_disconnect(xcb_connection_t *c) { (void)c; TR("xcb_disconnect"); }
const struct xcb_query_extension_reply_t *xcb_get_extension_data(xcb_connection_t *c, xcb_extension_t *ext)
{
	static xcb_query_extension_reply_t r;
	(void)c;
	r.present = ext == &xcb_shm_id ? g_x_shm : ext == &xcb_randr_id ? g_x_randr : ext == &xcb_xinerama_id ? g_x_xin : 0;
	return &r;
}
const struct xcb_setup_t *xcb_get_setup(xcb_connection_t *c) { (void)c; return (const struct xcb_setup_t *)&g_scr; }
int xcb_setup_roots_length(const xcb_setup_t *s) { (void)s; return g_nscr; }
bool xinerama_is_active(xcb_connection_t *c) { (void)c; return g_x_xin; }
bool randr_is_active(xcb_connection_t *c) { (void)c; return g_x_randr; }
int xinerama_screen_count(xcb_connection_t *c) { (void)c; return g_nscr; }
int randr_screen_count(xcb_connection_t *c) { (void)c; return g_nscr; }
int randr_screen_geo(xcb_connection_t *c, int_fast32_t s, int_fast32_t *x, int_fast32_t *y, int_fast32_t *w, int_fast32_t *h,
		     xcb_screen_t **rs, char **name)
{
	(void)c;
	if (s < 0 || s >= g_nscr || g_scr[s].fail) {
		*x = *y = *w = *h = 0;
		return -1;
	}
	*x = g_scr[s].x; *y = g_scr[s].y; *w = g_scr[s].w; *h = g_scr[s].h;
	if (rs)
		*rs = &g_xscreen[0];
	if (name && g_scr[s].name[0])
		*name = strdup(g_scr[s].name);
	return 0;
}
int xinerama_screen_geo(xcb_connection_t *c, int_fast32_t s, int_fast32_t *x, int_fast32_t *y, int_fast32_t *w, int_fast32_t *h)
{
	(void)c;
	if (s < 0 || s >= g_nscr || g_scr[s].fail) {
		*x = *y = *w = *h = 0;
		return -1;
	}
	*x = g_scr[s].x; *y = g_scr[s].y; *w = g_scr[s].w; *h = g_scr[s].h;
	return 0;
}
int x11_screen_geo(xcb_connection_t *c, int_fast32_t s, int_fast32_t *w, int_fast32_t *h)
{
	(void)c;
	if (s < 0 || s >= g_nscr || g_scr[s].fail)
		return -1;
	*w = g_scr[s].w; *h = g_scr[s].h;
	return 0;
}
xcb_screen_t *xcb_get_screen(xcb_connection_t *c, int s) { (void)c; return s >= 0 && s < g_nscr ? &g_xscreen[s] : NULL; }
xcb_shm_t *xshm_xcb_attach(xcb_connection_t *c, const int w, const int h)
{
	(void)c;
	TR("xshm_attach %dx%d -> %s", w, h, failing("attach") ? "fail" : "ok");
	return failing("attach") ? NULL : &g_shm;
}
void xshm_xcb_detach(xcb_shm_t *s) { (void)s; TR("xshm_detach"); }
xcb_xcursor_t *xcb_xcursor_init(xcb_connection_t *c) { (void)c; TR("cursor_init"); return g_cursor; }
void xcb_xcursor_offset(xcb_xcursor_t *c, const int x, const int y) { (void)c; TR("cursor_offset %d,%d", (int)x, (int)y); }
void xcb_xcursor_destroy(xcb_xcursor_t *c) { (void)c; TR("cursor_destroy"); }
void xcb_xcursor_update(xcb_connection_t *x, xcb_xcursor_t *c) { (void)x; (void)c; }
void xcb_xcursor_render(xcb_xcursor_t *c) { (void)c; }
xcb_shm_get_image_cookie_t xcb_shm_get_image_unchecked(xcb_connection_t *c, xcb_drawable_t d, int16_t x, int16_t y, uint16_t w,
						       uint16_t h, uint32_t pm, uint8_t f, xcb_shm_seg_t s, uint32_t o)
{
	(void)c; (void)d; (void)x; (void)y; (void)w; (void)h; (void)pm; (void)f; (void)s; (void)o;
	xcb_shm_get_image_cookie_t k = {0};
	return k;
}
xcb_shm_get_image_reply_t *xcb_shm_get_image_reply(xcb_connection_t *c, xcb_shm_get_image_cookie_t k, xcb_generic_error_t **e)
{
	(void)c; (void)k; (void)e;
	return NULL;
}

/* ---------------------------------------------------------------- script commands */
#define IS(s) (!strcmp(a[0], s))
int lc_fake_cmd(struct script_line *sl)
{
	char **a = sl->argv;
	int n = sl->argc;
	if (IS("trace")) { script_need(sl, 2); lc_trace = !strcmp(a[1], "on"); return 1; }
	if (IS("clock")) { script_need(sl, 2); g_now_ns = strtoull(a[1], NULL, 0); return 1; }
	if (IS("source-name")) { script_need(sl, 2); snprintf(lc_source_name, sizeof(lc_source_name), "%s", a[1]); return 1; }
	if (IS("fail")) { script_need(sl, 2); if (g_nfail < MAXFAIL) snprintf(g_fail[g_nfail++], 24, "%s", a[1]); return 1; }
	if (IS("unfail")) { g_nfail = 0; return 1; }
	/* v4l2 */
	if (IS("fake-dir")) {
		g_ndir = 0;
		for (int i = 1; i < n && g_ndir < MAXN * 2; i++)
			if (strcmp(a[i], "-"))
				snprintf(g_dir[g_ndir++], 32, "%s", a[i]);
		return 1;
	}
	if (IS("fake-dir-missing")) { g_ndir = -1; return 1; }
	if (IS("dev")) {
		script_need(sl, 2);
		g_cur = dev_find(a[1]);
		if (!g_cur) {
			for (int i = 0; i < MAXDEV; i++)
				if (!g_dev[i].used) { g_cur = &g_dev[i]; break; }
			memset(g_cur, 0, sizeof(*g_cur));
			g_cur->used = true;
			snprintf(g_cur->path, sizeof(g_cur->path), "%s", a[1]);
			g_cur->reqbufs = 4; g_cur->bufsize = 4096;
			g_cur->caps = g_cur->devcaps = V4L2_CAP_VIDEO_CAPTURE;
		}
		return 1;
	}
	if (IS("dev-cap")) { /* card bus caps devcaps */
		script_need(sl, 5);
		snprintf(g_cur->card, 32, "%s", a[1]); snprintf(g_cur->bus, 32, "%s", a[2]);
		g_cur->caps = strtoul(a[3], NULL, 0); g_cur->devcaps = strtoul(a[4], NULL, 0);
		return 1;
	}
	if (IS("dev-input")) { script_need(sl, 3); snprintf(g_cur->in[g_cur->nin].name, 32, "%s", a[1]); g_cur->in[g_cur->nin++].caps = strtoul(a[2], NULL, 0); return 1; }
	if (IS("dev-fmt")) { /* fourcc desc flags */
		script_need(sl, 4);
		int i = g_cur->nfmt++;
		g_cur->fmt[i].fourcc = fourcc_of(a[1]); snprintf(g_cur->fmt[i].desc, 32, "%s", a[2]); g_cur->fmt[i].flags = strtoul(a[3], NULL, 0);
		return 1;
	}
	if (IS("dev-std")) { script_need(sl, 3); int i = g_cur->nstd++; snprintf(g_cur->std[i].name, 24, "%s", a[1]); g_cur->std[i].id = strtoull(a[2], NULL, 0); return 1; }
	if (IS("dev-dv")) { /* w h clock hfp hs hbp vfp vs vbp ilvfp ilvs ilvbp interlaced */
		script_need(sl, 14);
		struct v4l2_dv_timings *t = &g_cur->dv[g_cur->ndv++];
		memset(t, 0, sizeof(*t));
		t->type = V4L2_DV_BT_656_1120;
		t->bt.width = atoi(a[1]); t->bt.height = atoi(a[2]); t->bt.pixelclock = strtoull(a[3], NULL, 0);
		t->bt.hfrontporch = atoi(a[4]); t->bt.hsync = atoi(a[5]); t->bt.hbackporch = atoi(a[6]);
		t->bt.vfrontporch = atoi(a[7]); t->bt.vsync = atoi(a[8]); t->bt.vbackporch = atoi(a[9]);
		t->bt.il_vfrontporch = atoi(a[10]); t->bt.il_vsync = atoi(a[11]); t->bt.il_vbackporch = atoi(a[12]);
		t->bt.interlaced = atoi(a[13]);
		return 1;
	}
	if (IS("dev-size")) { script_need(sl, 4); int i = g_cur->nsz++; g_cur->sz[i].fourcc = fourcc_of(a[1]); g_cur->sz[i].w = atoi(a[2]); g_cur->sz[i].h = atoi(a[3]); return 1; }
	if (IS("dev-size-stepwise")) { script_need(sl, 2); g_cur->szstep[g_cur->nszstep++] = fourcc_of(a[1]); return 1; }
	if (IS("dev-ival")) { script_need(sl, 6); int i = g_cur->niv++; g_cur->iv[i].fourcc = fourcc_of(a[1]); g_cur->iv[i].w = atoi(a[2]); g_cur->iv[i].h = atoi(a[3]); g_cur->iv[i].num = atoi(a[4]); g_cur->iv[i].den = atoi(a[5]); return 1; }
	if (IS("dev-ival-stepwise")) { g_cur->ivstep = true; return 1; }
	if (IS("dev-ctrl")) { /* id type name min max step def flags */
		script_need(sl, 9);
		struct v4l2_queryctrl *q = &g_cur->ctrl[g_cur->nctrl++];
		memset(q, 0, sizeof(*q));
		q->id = strtoul(a[1], NULL, 0); q->type = atoi(a[2]); snprintf((char *)q->name, 32, "%s", a[3]);
		q->minimum = atoi(a[4]); q->maximum = atoi(a[5]); q->step = atoi(a[6]); q->default_value = atoi(a[7]); q->flags = strtoul(a[8], NULL, 0);
		return 1;
	}
	if (IS("dev-menu")) { script_need(sl, 4); int i = g_cur->nmenu++; g_cur->menu[i].ctrl = strtoul(a[1], NULL, 0); g_cur->menu[i].index = atoi(a[2]); snprintf(g_cur->menu[i].name, 32, "%s", a[3]); return 1; }
	if (IS("dev-cur")) { /* input w h fourcc bpl num den std */
		script_need(sl, 9);
		g_cur->cur_input = atoi(a[1]); g_cur->cw = atoi(a[2]); g_cur->ch = atoi(a[3]); g_cur->cpix = fourcc_of(a[4]);
		g_cur->cbpl = atoi(a[5]); g_cur->tnum = atoi(a[6]); g_cur->tden = atoi(a[7]); g_cur->cstd = strtoull(a[8], NULL, 0);
		return 1;
	}
	if (IS("dev-reqbufs")) { script_need(sl, 2); g_cur->reqbufs = atoi(a[1]); return 1; }
	if (IS("mmap-fail")) { g_mmap_fail = true; return 1; }
	if (IS("mmap-ok")) { g_mmap_fail = false; return 1; }
	if (IS("decoder-fail")) { g_decoder_fail = true; return 1; }
	/* pulse */
	if (IS("pulse-server")) { script_need(sl, 5); snprintf(g_srv_name, 32, "%s", a[1]); snprintf(g_srv_ver, 16, "%s", a[2]); snprintf(g_def_src, 48, "%s", a[3]); snprintf(g_def_sink, 48, "%s", a[4]); return 1; }
	if (IS("pulse-source")) { /* name desc monitor_of fmt rate channels */
		script_need(sl, 7);
		int i = g_npsrc++;
		snprintf(g_psrc[i].name, 48, "%s", a[1]); snprintf(g_psrc[i].desc, 48, "%s", a[2]);
		g_psrc[i].monitor_of = !strcmp(a[3], "-") ? -1 : atoi(a[3]);
		g_psrc[i].fmt = pa_parse_sample_format(a[4]); g_psrc[i].rate = atoi(a[5]); g_psrc[i].ch = atoi(a[6]);
		return 1;
	}
	if (IS("pulse-sink")) { /* name desc monitor_idx monitor_name */
		script_need(sl, 5);
		int i = g_npsink++;
		snprintf(g_psink[i].name, 48, "%s", a[1]); snprintf(g_psink[i].desc, 48, "%s", a[2]);
		g_psink[i].mon_idx = !strcmp(a[3], "-") ? -1 : atoi(a[3]); snprintf(g_psink[i].mon_name, 56, "%s", a[4]);
		return 1;
	}
	if (IS("pulse-chunk")) { /* BYTES|hole BYTES */
		script_need(sl, 3);
		g_chunk.present = true; g_chunk.hole = !strcmp(a[1], "hole"); g_chunk.bytes = atoi(a[2]);
		return 1;
	}
	/* x11 */
	if (IS("x-ext")) { script_need(sl, 3); bool v = atoi(a[2]); if (!strcmp(a[1], "shm")) g_x_shm = v; else if (!strcmp(a[1], "randr")) g_x_randr = v; else g_x_xin = v; return 1; }
	if (IS("x-connect-fail")) { script_need(sl, 2); g_x_connect_fail = atoi(a[1]); return 1; }
	if (IS("x-screen")) { /* x y w h [name|-] [fail] */
		script_need(sl, 5);
		int i = g_nscr++;
		g_scr[i].x = atoi(a[1]); g_scr[i].y = atoi(a[2]); g_scr[i].w = atoi(a[3]); g_scr[i].h = atoi(a[4]);
		if (n > 5 && strcmp(a[5], "-")) snprintf(g_scr[i].name, 24, "%s", a[5]);
		g_scr[i].fail = n > 6;
		return 1;
	}
	return 0;
}

/* ---- platform functions obs-data.c links against (persistence is never exercised) ---- */
#include <stdlib.h>


int os_rename(const char *old_path, const char *new_path)
{
	return rename(old_path, new_path);
}




char *os_quick_read_utf8_file(const char *path)
{
	(void)path;
	abort();
}

bool os_quick_write_utf8_file(const char *path, const char *str, size_t len, bool marker)
{
	(void)path, (void)str, (void)len, (void)marker;
	abort();
}

bool os_quick_write_utf8_file_safe(const char *path, const char *str, size_t len, bool marker, const char *temp_ext,
				   const char *backup_ext)
{
	(void)path, (void)str, (void)len, (void)marker, (void)temp_ext, (void)backup_ext;
	abort();
}
