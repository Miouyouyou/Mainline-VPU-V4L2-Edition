// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Collabora, Ltd.
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 *	Alpha Lin <Alpha.Lin@rock-chips.com>
 *	Jeffy Chen <jeffy.chen@rock-chips.com>
 *
 * Copyright (C) 2018 Google, Inc.
 *	Tomasz Figa <tfiga@chromium.org>
 *
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2010-2011 Samsung Electronics Co., Ltd.
 */

#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pm_runtime.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-sg.h>

#include "rockchip_vpu.h"
#include "rockchip_vpu_hw.h"
#include "rockchip_vpu_common.h"

static const struct rockchip_vpu_fmt *
rockchip_vpu_find_format(struct rockchip_vpu_ctx *ctx, u32 fourcc)
{
	struct rockchip_vpu_dev *dev = ctx->dev;
	const struct rockchip_vpu_fmt *formats;
	unsigned int num_fmts, i;

	formats = dev->variant->enc_fmts;
	num_fmts = dev->variant->num_enc_fmts;
	for (i = 0; i < num_fmts; i++)
		if (formats[i].fourcc == fourcc)
			return &formats[i];
	return NULL;
}

static const struct rockchip_vpu_fmt *
rockchip_vpu_get_default_fmt(struct rockchip_vpu_ctx *ctx, bool bitstream)
{
	struct rockchip_vpu_dev *dev = ctx->dev;
	const struct rockchip_vpu_fmt *formats;
	unsigned int num_fmts, i;

	formats = dev->variant->enc_fmts;
	num_fmts = dev->variant->num_enc_fmts;
	for (i = 0; i < num_fmts; i++)
		if (bitstream == (formats[i].codec_mode != RK_VPU_MODE_NONE))
			return &formats[i];
	return NULL;
}

static int vidioc_querycap(struct file *file, void *priv,
			   struct v4l2_capability *cap)
{
	struct rockchip_vpu_dev *vpu = video_drvdata(file);

	strlcpy(cap->driver, vpu->dev->driver->name, sizeof(cap->driver));
	strlcpy(cap->card, vpu->vfd_enc->name, sizeof(cap->card));
	snprintf(cap->bus_info, sizeof(cap->bus_info), "platform: %s",
		 vpu->dev->driver->name);
	return 0;
}

static int vidioc_enum_framesizes(struct file *file, void *priv,
				  struct v4l2_frmsizeenum *fsize)
{
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	const struct rockchip_vpu_fmt *fmt;

	if (fsize->index != 0) {
		vpu_debug(0, "invalid frame size index (expected 0, got %d)\n",
				fsize->index);
		return -EINVAL;
	}

	fmt = rockchip_vpu_find_format(ctx, fsize->pixel_format);
	if (!fmt) {
		vpu_debug(0, "unsupported bitstream format (%08x)\n",
				fsize->pixel_format);
		return -EINVAL;
	}

	/* This only makes sense for codec formats */
	if (fmt->codec_mode == RK_VPU_MODE_NONE)
		return -EINVAL;

	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise = fmt->frmsize;

	return 0;
}

static int vidioc_enum_fmt_vid_cap_mplane(struct file *file, void *priv,
					  struct v4l2_fmtdesc *f)
{
	struct rockchip_vpu_dev *dev = video_drvdata(file);
	const struct rockchip_vpu_fmt *fmt;
	const struct rockchip_vpu_fmt *formats;
	int num_fmts, i, j = 0;

	formats = dev->variant->enc_fmts;
	num_fmts = dev->variant->num_enc_fmts;
	for (i = 0; i < num_fmts; i++) {
		/* Skip uncompressed formats */
		if (formats[i].codec_mode == RK_VPU_MODE_NONE)
			continue;
		if (j == f->index) {
			fmt = &formats[i];
			f->pixelformat = fmt->fourcc;
			return 0;
		}
		++j;
	}
	return -EINVAL;
}

static int vidioc_enum_fmt_vid_out_mplane(struct file *file, void *priv,
					  struct v4l2_fmtdesc *f)
{
	struct rockchip_vpu_dev *dev = video_drvdata(file);
	const struct rockchip_vpu_fmt *formats;
	const struct rockchip_vpu_fmt *fmt;
	int num_fmts, i, j = 0;

	formats = dev->variant->enc_fmts;
	num_fmts = dev->variant->num_enc_fmts;
	for (i = 0; i < num_fmts; i++) {
		if (formats[i].codec_mode != RK_VPU_MODE_NONE)
			continue;
		if (j == f->index) {
			fmt = &formats[i];
			f->pixelformat = fmt->fourcc;
			return 0;
		}
		++j;
	}
	return -EINVAL;
}

static int vidioc_g_fmt_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);

	vpu_debug(4, "f->type = %d\n", f->type);

	*pix_mp = ctx->src_fmt;
	pix_mp->colorspace = ctx->colorspace;
	pix_mp->ycbcr_enc = ctx->ycbcr_enc;
	pix_mp->xfer_func = ctx->xfer_func;
	pix_mp->quantization = ctx->quantization;

	return 0;
}

static int vidioc_g_fmt_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);

	vpu_debug(4, "f->type = %d\n", f->type);

	*pix_mp = ctx->dst_fmt;
	pix_mp->colorspace = ctx->colorspace;
	pix_mp->ycbcr_enc = ctx->ycbcr_enc;
	pix_mp->xfer_func = ctx->xfer_func;
	pix_mp->quantization = ctx->quantization;

	return 0;
}

static void calculate_plane_sizes(const struct rockchip_vpu_fmt *fmt,
				  struct v4l2_pix_format_mplane *pix_mp)
{
	unsigned int w = pix_mp->width;
	unsigned int h = pix_mp->height;
	int i;

	for (i = 0; i < fmt->num_planes; ++i) {
		memset(pix_mp->plane_fmt[i].reserved, 0,
		       sizeof(pix_mp->plane_fmt[i].reserved));
		pix_mp->plane_fmt[i].bytesperline = w * fmt->depth[i] / 8;
		pix_mp->plane_fmt[i].sizeimage = h *
					pix_mp->plane_fmt[i].bytesperline;
	}
}

static int
vidioc_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct rockchip_vpu_fmt *fmt;
	char str[5];

	vpu_debug(4, "%s\n", fmt2str(pix_mp->pixelformat, str));

	fmt = rockchip_vpu_find_format(ctx, pix_mp->pixelformat);
	if (!fmt) {
		fmt = rockchip_vpu_get_default_fmt(ctx, true);
		f->fmt.pix.pixelformat = fmt->fourcc;
	}

	pix_mp->num_planes = fmt->num_planes;
	pix_mp->field = V4L2_FIELD_NONE;
	pix_mp->width = clamp(pix_mp->width,
			fmt->frmsize.min_width,
			fmt->frmsize.max_width);
	pix_mp->height = clamp(pix_mp->height,
			fmt->frmsize.min_height,
			fmt->frmsize.max_height);
	pix_mp->plane_fmt[0].sizeimage =
		pix_mp->width * pix_mp->height * fmt->max_depth;
	memset(pix_mp->plane_fmt[0].reserved, 0,
	       sizeof(pix_mp->plane_fmt[0].reserved));
	return 0;
}

static int
vidioc_try_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct rockchip_vpu_fmt *fmt;
	unsigned long dma_align;
	bool need_alignment;
	char str[5];
	int i;

	vpu_debug(4, "%s\n", fmt2str(pix_mp->pixelformat, str));

	fmt = rockchip_vpu_find_format(ctx, pix_mp->pixelformat);
	if (!fmt) {
		fmt = rockchip_vpu_get_default_fmt(ctx, false);
		f->fmt.pix.pixelformat = fmt->fourcc;
	}

	pix_mp->num_planes = fmt->num_planes;
	pix_mp->field = V4L2_FIELD_NONE;
	pix_mp->width = clamp(pix_mp->width,
			ctx->vpu_dst_fmt->frmsize.min_width,
			ctx->vpu_dst_fmt->frmsize.max_width);
	pix_mp->height = clamp(pix_mp->height,
			ctx->vpu_dst_fmt->frmsize.min_height,
			ctx->vpu_dst_fmt->frmsize.max_height);
	/* Round up to macroblocks. */
	pix_mp->width = round_up(pix_mp->width, MB_DIM);
	pix_mp->height = round_up(pix_mp->height, MB_DIM);

	/* Fill remaining fields */
	calculate_plane_sizes(fmt, pix_mp);

	dma_align = dma_get_cache_alignment();
	need_alignment = false;
	for (i = 0; i < fmt->num_planes; i++) {
		if (!IS_ALIGNED(pix_mp->plane_fmt[i].sizeimage,
				dma_align)) {
			need_alignment = true;
			break;
		}
	}
	if (!need_alignment)
		return 0;

	pix_mp->height = round_up(pix_mp->height, dma_align * 4 / MB_DIM);
	if (pix_mp->height > ctx->vpu_dst_fmt->frmsize.max_height) {
		vpu_err("Aligned height higher than maximum.\n");
		return -EINVAL;
	}
	/* Fill in remaining fields, again */
	calculate_plane_sizes(fmt, pix_mp);
	return 0;
}

void rockchip_vpu_enc_reset_dst_fmt(struct rockchip_vpu_dev *vpu,
				struct rockchip_vpu_ctx *ctx)
{
	struct v4l2_pix_format_mplane *fmt = &ctx->dst_fmt;

	ctx->vpu_dst_fmt = rockchip_vpu_get_default_fmt(ctx, true);

	memset(fmt, 0, sizeof(*fmt));

	fmt->num_planes = ctx->vpu_dst_fmt->num_planes;
	fmt->width = clamp(fmt->width, ctx->vpu_dst_fmt->frmsize.min_width,
		ctx->vpu_dst_fmt->frmsize.max_width);
	fmt->height = clamp(fmt->height, ctx->vpu_dst_fmt->frmsize.min_height,
		ctx->vpu_dst_fmt->frmsize.max_height);
	fmt->pixelformat = ctx->vpu_dst_fmt->fourcc;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = ctx->colorspace;
	fmt->ycbcr_enc = ctx->ycbcr_enc;
	fmt->xfer_func = ctx->xfer_func;
	fmt->quantization = ctx->quantization;

	fmt->plane_fmt[0].sizeimage =
		fmt->width * fmt->height * ctx->vpu_dst_fmt->max_depth;
}

void rockchip_vpu_enc_reset_src_fmt(struct rockchip_vpu_dev *vpu,
				struct rockchip_vpu_ctx *ctx)
{
	struct v4l2_pix_format_mplane *fmt = &ctx->src_fmt;

	ctx->vpu_src_fmt = rockchip_vpu_get_default_fmt(ctx, false);

	memset(fmt, 0, sizeof(*fmt));

	fmt->num_planes = ctx->vpu_src_fmt->num_planes;
	fmt->width = clamp(fmt->width, ctx->vpu_dst_fmt->frmsize.min_width,
		ctx->vpu_dst_fmt->frmsize.max_width);
	fmt->height = clamp(fmt->height, ctx->vpu_dst_fmt->frmsize.min_height,
		ctx->vpu_dst_fmt->frmsize.max_height);
	fmt->pixelformat = ctx->vpu_src_fmt->fourcc;
	fmt->field = V4L2_FIELD_NONE;
	fmt->colorspace = ctx->colorspace;
	fmt->ycbcr_enc = ctx->ycbcr_enc;
	fmt->xfer_func = ctx->xfer_func;
	fmt->quantization = ctx->quantization;

	calculate_plane_sizes(ctx->vpu_src_fmt, fmt);
}

static int
vidioc_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct vb2_queue *vq, *peer_vq;
	int ret;

	/* Change not allowed if queue is streaming. */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_streaming(vq))
		return -EBUSY;

	ctx->colorspace = pix_mp->colorspace;
	ctx->ycbcr_enc = pix_mp->ycbcr_enc;
	ctx->xfer_func = pix_mp->xfer_func;
	ctx->quantization = pix_mp->quantization;

	/*
	 * Pixel format change is not allowed when the other queue has
	 * buffers allocated.
	 */
	peer_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
		V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	if (vb2_is_busy(peer_vq) &&
	    pix_mp->pixelformat != ctx->src_fmt.pixelformat)
		return -EBUSY;

	ret = vidioc_try_fmt_out(file, priv, f);
	if (ret)
		return ret;

	ctx->vpu_src_fmt = rockchip_vpu_find_format(ctx, pix_mp->pixelformat);
	ctx->src_fmt = *pix_mp;

	vpu_debug(0, "OUTPUT codec mode: %d\n", ctx->vpu_src_fmt->codec_mode);
	vpu_debug(0, "fmt - w: %d, h: %d, mb - w: %d, h: %d\n",
		  pix_mp->width, pix_mp->height,
		  MB_WIDTH(pix_mp->width),
		  MB_HEIGHT(pix_mp->height));
	return 0;
}

static int
vidioc_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct rockchip_vpu_dev *vpu = ctx->dev;
	struct vb2_queue *vq, *peer_vq;
	int ret;

	/* Change not allowed if queue is streaming. */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_streaming(vq))
		return -EBUSY;

	ctx->colorspace = pix_mp->colorspace;
	ctx->ycbcr_enc = pix_mp->ycbcr_enc;
	ctx->xfer_func = pix_mp->xfer_func;
	ctx->quantization = pix_mp->quantization;

	/*
	 * Pixel format change is not allowed when the other queue has
	 * buffers allocated.
	 */
	peer_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
			V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE);
	if (vb2_is_busy(peer_vq) &&
	    pix_mp->pixelformat != ctx->dst_fmt.pixelformat)
		return -EBUSY;

	ret = vidioc_try_fmt_cap(file, priv, f);
	if (ret)
		return ret;

	ctx->vpu_dst_fmt = rockchip_vpu_find_format(ctx, pix_mp->pixelformat);
	ctx->dst_fmt = *pix_mp;

	vpu_debug(0, "CAPTURE codec mode: %d\n", ctx->vpu_dst_fmt->codec_mode);
	vpu_debug(0, "fmt - w: %d, h: %d, mb - w: %d, h: %d\n",
		  pix_mp->width, pix_mp->height,
		  MB_WIDTH(pix_mp->width),
		  MB_HEIGHT(pix_mp->height));

	/*
	 * Current raw format might have become invalid with newly
	 * selected codec, so reset it to default just to be safe and
	 * keep internal driver state sane. User is mandated to set
	 * the raw format again after we return, so we don't need
	 * anything smarter.
	 */
	rockchip_vpu_enc_reset_src_fmt(vpu, ctx);
	return 0;
}

const struct v4l2_ioctl_ops rockchip_vpu_enc_ioctl_ops = {
	.vidioc_querycap = vidioc_querycap,
	.vidioc_enum_framesizes = vidioc_enum_framesizes,

	.vidioc_try_fmt_vid_cap_mplane = vidioc_try_fmt_cap,
	.vidioc_try_fmt_vid_out_mplane = vidioc_try_fmt_out,
	.vidioc_s_fmt_vid_out_mplane = vidioc_s_fmt_out,
	.vidioc_s_fmt_vid_cap_mplane = vidioc_s_fmt_cap,
	.vidioc_g_fmt_vid_out_mplane = vidioc_g_fmt_out,
	.vidioc_g_fmt_vid_cap_mplane = vidioc_g_fmt_cap,
	.vidioc_enum_fmt_vid_out_mplane = vidioc_enum_fmt_vid_out_mplane,
	.vidioc_enum_fmt_vid_cap_mplane = vidioc_enum_fmt_vid_cap_mplane,

	.vidioc_reqbufs = v4l2_m2m_ioctl_reqbufs,
	.vidioc_querybuf = v4l2_m2m_ioctl_querybuf,
	.vidioc_qbuf = v4l2_m2m_ioctl_qbuf,
	.vidioc_dqbuf = v4l2_m2m_ioctl_dqbuf,
	.vidioc_prepare_buf = v4l2_m2m_ioctl_prepare_buf,
	.vidioc_create_bufs = v4l2_m2m_ioctl_create_bufs,
	.vidioc_expbuf = v4l2_m2m_ioctl_expbuf,

	.vidioc_subscribe_event = v4l2_ctrl_subscribe_event,
	.vidioc_unsubscribe_event = v4l2_event_unsubscribe,

	.vidioc_streamon = v4l2_m2m_ioctl_streamon,
	.vidioc_streamoff = v4l2_m2m_ioctl_streamoff,
};

static int rockchip_vpu_queue_setup(struct vb2_queue *vq,
				  unsigned int *num_buffers,
				  unsigned int *num_planes,
				  unsigned int sizes[],
				  struct device *alloc_devs[])
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vq);
	const struct rockchip_vpu_fmt *vpu_fmt;
	struct v4l2_pix_format_mplane *pixfmt;
	int i;

	switch (vq->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		vpu_fmt = ctx->vpu_dst_fmt;
		pixfmt = &ctx->dst_fmt;
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		vpu_fmt = ctx->vpu_src_fmt;
		pixfmt = &ctx->src_fmt;
		break;
	default:
		vpu_err("invalid queue type: %d\n", vq->type);
		return -EINVAL;
	}

	if (*num_planes) {
		if (*num_planes !=  vpu_fmt->num_planes)
			return -EINVAL;
		for (i = 0; i < vpu_fmt->num_planes; ++i)
			if (sizes[i] < pixfmt->plane_fmt[i].sizeimage)
				return -EINVAL;
		return 0;
	}

	*num_planes = vpu_fmt->num_planes;
	for (i = 0; i < vpu_fmt->num_planes; ++i)
		sizes[i] = pixfmt->plane_fmt[i].sizeimage;
	return 0;
}

static int rockchip_vpu_buf_prepare(struct vb2_buffer *vb)
{
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);
	struct vb2_queue *vq = vb->vb2_queue;
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vq);
	const struct rockchip_vpu_fmt *vpu_fmt;
	struct v4l2_pix_format_mplane *pixfmt;
	unsigned int sz;
	int ret = 0;
	int i;

	switch (vq->type) {
	case V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE:
		vpu_fmt = ctx->vpu_dst_fmt;
		pixfmt = &ctx->dst_fmt;
		break;
	case V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE:
		vpu_fmt = ctx->vpu_src_fmt;
		pixfmt = &ctx->src_fmt;

		if (vbuf->field == V4L2_FIELD_ANY)
			vbuf->field = V4L2_FIELD_NONE;
		if (vbuf->field != V4L2_FIELD_NONE) {
			vpu_debug(4, "field %d not supported\n",
				  vbuf->field);
			return -EINVAL;
		}
		break;
	default:
		vpu_err("invalid queue type: %d\n", vq->type);
		return -EINVAL;
	}

	for (i = 0; i < vpu_fmt->num_planes; ++i) {
		sz = pixfmt->plane_fmt[i].sizeimage;
		vpu_debug(4, "plane %d size: %ld, sizeimage: %u\n",
			  i, vb2_plane_size(vb, i), sz);
		if (vb2_plane_size(vb, i) < sz) {
			vpu_err("plane %d is too small for output\n", i);
			ret = -EINVAL;
			break;
		}
	}

	return ret;
}

static void rockchip_vpu_buf_queue(struct vb2_buffer *vb)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(vb->vb2_queue);
	struct vb2_v4l2_buffer *vbuf = to_vb2_v4l2_buffer(vb);

	v4l2_m2m_buf_queue(ctx->fh.m2m_ctx, vbuf);
}

static int rockchip_vpu_start_streaming(struct vb2_queue *q, unsigned int count)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(q);
	enum rockchip_vpu_codec_mode codec_mode;

	if (V4L2_TYPE_IS_OUTPUT(q->type))
		ctx->sequence_out = 0;
	else
		ctx->sequence_cap = 0;

	/* Set codec_ops for the chosen destination format */
	codec_mode = ctx->vpu_dst_fmt->codec_mode;

	vpu_debug(4, "Codec mode = %d\n", codec_mode);
	ctx->codec_ops = &ctx->dev->variant->codec_ops[codec_mode];

	return 0;
}

static void rockchip_vpu_stop_streaming(struct vb2_queue *q)
{
	struct rockchip_vpu_ctx *ctx = vb2_get_drv_priv(q);

	/* The mem2mem framework calls v4l2_m2m_cancel_job before
	 * .stop_streaming, so there isn't any job running and
	 * it is safe to return all the buffers.
	 */
	for (;;) {
		struct vb2_v4l2_buffer *vbuf;

		if (V4L2_TYPE_IS_OUTPUT(q->type))
			vbuf = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
		else
			vbuf = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);
		if (!vbuf)
			break;
		v4l2_m2m_buf_done(vbuf, VB2_BUF_STATE_ERROR);
	}
}

const struct vb2_ops rockchip_vpu_enc_queue_ops = {
	.queue_setup = rockchip_vpu_queue_setup,
	.buf_prepare = rockchip_vpu_buf_prepare,
	.buf_queue = rockchip_vpu_buf_queue,
	.start_streaming = rockchip_vpu_start_streaming,
	.stop_streaming = rockchip_vpu_stop_streaming,
	.wait_prepare = vb2_ops_wait_prepare,
	.wait_finish = vb2_ops_wait_finish,
};
