/*
 * Compiles the real libobs/graphics/graphics-ffmpeg.c into the gfximages runner and adds test hooks that reach
 * its static functions (frame reformatting, EXIF orientation) with a fabricated AVFrame. Test tooling.
 *
 * The orientation code only runs when the decoder reports an "Orientation" entry in the frame metadata. Recent
 * FFmpeg releases (8.x) convert EXIF orientation into display-matrix side data instead, so decoding real files
 * never reaches that code there; the hook sets the metadata entry itself.
 */
#include "../../../libobs/graphics/graphics-ffmpeg.c"

/* pix: rgba | bgra | bgr0 | rgba64be | rgb24 | gray8 | yuv420p ("yuv420p" planes: Y from src, U = V = 128).
 * Returns the converted pixel buffer (bfree it) or NULL; *gs_format receives the format texture creation would
 * report for the result, cx/cy the (possibly rotated) size. */
uint8_t *gfx_hook_reformat(const char *pix, int w, int h, int linesize, const uint8_t *src, int orient, int alpha_mode,
			   int *gs_format, int *cx, int *cy)
{
	struct ffmpeg_image info;
	memset(&info, 0, sizeof info);
	info.file = "hook";
	info.cx = w;
	info.cy = h;
	int planes = 1;
	if (!strcmp(pix, "rgba"))
		info.format = AV_PIX_FMT_RGBA;
	else if (!strcmp(pix, "bgra"))
		info.format = AV_PIX_FMT_BGRA;
	else if (!strcmp(pix, "bgr0"))
		info.format = AV_PIX_FMT_BGR0;
	else if (!strcmp(pix, "rgba64be"))
		info.format = AV_PIX_FMT_RGBA64BE;
	else if (!strcmp(pix, "rgb24"))
		info.format = AV_PIX_FMT_RGB24;
	else if (!strcmp(pix, "gray8"))
		info.format = AV_PIX_FMT_GRAY8;
	else if (!strcmp(pix, "yuv420p")) {
		info.format = AV_PIX_FMT_YUV420P;
		planes = 3;
	} else
		return NULL;

	AVFrame *frame = av_frame_alloc();
	frame->format = info.format;
	frame->width = w;
	frame->height = h;
	size_t sz = (size_t)linesize * h;
	uint8_t *plane0 = av_mallocz(sz + 64);
	memcpy(plane0, src, sz);
	frame->data[0] = plane0;
	frame->linesize[0] = linesize;
	uint8_t *u = NULL, *v = NULL;
	if (planes == 3) {
		int cw = (w + 1) / 2, ch = (h + 1) / 2;
		u = av_malloc((size_t)cw * ch + 64);
		v = av_malloc((size_t)cw * ch + 64);
		memset(u, 128, (size_t)cw * ch);
		memset(v, 128, (size_t)cw * ch);
		frame->data[1] = u;
		frame->data[2] = v;
		frame->linesize[1] = cw;
		frame->linesize[2] = cw;
	}
	if (orient) {
		char buf[16];
		snprintf(buf, sizeof buf, "%d", orient);
		av_dict_set(&frame->metadata, "Orientation", buf, 0);
	}

	void *data = ffmpeg_image_reformat_frame(&info, frame, (enum gs_image_alpha_mode)alpha_mode);
	if (data) {
		*gs_format = (int)convert_format(info.format);
		*cx = info.cx;
		*cy = info.cy;
	}
	av_dict_free(&frame->metadata);
	av_free(plane0);
	av_free(u);
	av_free(v);
	frame->data[0] = frame->data[1] = frame->data[2] = NULL;
	av_frame_free(&frame);
	return data;
}
