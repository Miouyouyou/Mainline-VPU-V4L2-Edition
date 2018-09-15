
#include <media/v4l2-ioctl.h>
#include <media/v4l2-mem2mem.h>
#include <media/v4l2-ctrls.h>
#include <media/v4l2-event.h>

#include "rockchip_vpu.h"

static const struct rockchip_vpu_fmt *
rockchip_vpu_find_format(
	struct rockchip_vpu_ctx* __restrict const ctx,
	u32 fourcc)
{
	struct rockchip_vpu_dev *dev = ctx->dev;
	const struct rockchip_vpu_fmt *formats;
	unsigned int num_fmts, i;

	formats = dev->variant->dec_fmts;
	num_fmts = dev->variant->num_dec_fmts;
	for (i = 0; i < num_fmts; i++)
		if (formats[i].fourcc == fourcc)
			return &formats[i];
	return NULL;
}

static int vidioc_querycap(
	struct file* __restrict const file,
	void* __restrict const priv,
	struct v4l2_capability* __restrict const cap)
{
	struct rockchip_vpu_dev *vpu = video_drvdata(file);

	strlcpy((char *) (cap->driver), vpu->dev->driver->name, sizeof(cap->driver));
	strlcpy((char *) (cap->card), vpu->vfd_dec->name, sizeof(cap->card));
	snprintf((char *) (cap->bus_info), sizeof(cap->bus_info), "platform: %s",
		 vpu->dev->driver->name);
	return 0;
}

static int vidioc_enum_framesizes(
	struct file* __restrict const file,
	void* __restrict const priv,
	struct v4l2_frmsizeenum* __restrict const fsize)
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

	/* TODO Check if that's really useful.
	 * There's also alternatives like :
	 * - V4L2_FRMSIZE_TYPE_CONTINUOUS
	 * - V4L2_FRMSIZE_TYPE_DISCRETE
	 */
	fsize->type = V4L2_FRMSIZE_TYPE_STEPWISE;
	fsize->stepwise = fmt->frmsize;

	return 0;
}

static int rockchip_vpu_enum_fmt(
	struct file* __restrict const file,
	void* __restrict const priv,
	struct v4l2_fmtdesc* const f)
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

static int vidioc_enum_fmt_vid_cap_mplane(
	struct file* __restrict const file,
	void* __restrict const priv,
	struct v4l2_fmtdesc* __restrict const f)
{
	return rockchip_vpu_enum_fmt(file, priv, f);
}

static int vidioc_enum_fmt_vid_out_mplane(
	struct file* __restrict const file,
	void* __restrict const priv,
	struct v4l2_fmtdesc* __restrict const f)
{
	return rockchip_vpu_enum_fmt(file, priv, f);
}

static int vidioc_g_fmt_out(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);

	vpu_debug(4, "f->type = %d\n", f->type);

	/* FIXME What does that do ???
	 * Taken from the old RK3288 H264 decoder code.
	 */
	f->fmt.pix_mp = ctx->src_fmt;

	return 0;
}

static int vidioc_g_fmt_cap(struct file *file, void *priv,
				struct v4l2_format *f)
{
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);

	vpu_debug(4, "f->type = %d\n", f->type);

	/* FIXME What does that do ???
	 * Taken from the old RK3288 H264 decoder code.
	 */
	f->fmt.pix_mp = ctx->dst_fmt;

	return 0;
}



static int
vidioc_try_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct rockchip_vpu_fmt *fmt;
	char str[5];

 	fmt2str(pix_mp->pixelformat, str);
	vpu_debug(4, "%s\n", str);

	fmt = rockchip_vpu_find_format(ctx, pix_mp->pixelformat);
	if (!fmt) {
		/* We're dealing with multiple formats so if the user
		 * is not sending the right one, don't infer some
		 * "default" one.
		 */
		dev_info(ctx->dev->dev, "Format %s not recognised", str);
		return -EINVAL;
	}

	if (pix_mp->plane_fmt[0].sizeimage == 0) {
		vpu_err("size image of output format must be given");
		return -EINVAL;
	}

	/* TODO Copied from Tomasz Figa code...
	 * What's the purpose of that ?
	 */
	pix_mp->plane_fmt[0].bytesperline = 0;

	return 0;
}

static int
vidioc_try_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	const struct rockchip_vpu_fmt *fmt;
	char str[5];
	struct v4l2_frmsize_stepwise const * __restrict const frame_limits =
		&(ctx->vpu_dst_fmt->frmsize);
	__u32 rounded_width, rounded_height;
	unsigned long dma_align;
	bool need_alignment;
	int i;

 	fmt2str(pix_mp->pixelformat, str);
	vpu_debug(4, "%s\n", str);

	fmt = rockchip_vpu_find_format(ctx, pix_mp->pixelformat);
	if (!fmt) {
		/* We're dealing with multiple formats so if the user
		 * is not sending the right one, don't infer some
		 * "default" one.
		 */
		dev_info(ctx->dev->dev, "Format %s not recognised", str);
		return -EINVAL;
	}

	if (fmt->num_planes != pix_mp->num_planes) {
		vpu_err(
			"Number of planes differ. Expected %ul got %ul",
			fmt->num_planes, pix_mp->num_planes);
		return -EINVAL;
	}

	/* FIXME Does that make sense ?
	 * On RockMyy kernels, dma_align == 64
	 * If the frame is a 1080p video frame, then the height is
	 * 1080.
	 * The height will be rounded to 1088 since 1080 isn't a
	 * multiple of 64.
	 * Do we want to deal with these 8 extra pixels ?
	 * Won't this pause any problem ?
	 * Stay tuned for the next episode of : Rockchip !
	 */
	dma_align = dma_get_cache_alignment();
	need_alignment = false;
	for (i = 0; i < fmt->num_planes && !need_alignment; i++)
		need_alignment = 
			IS_ALIGNED(pix_mp->plane_fmt[i].sizeimage, dma_align);

	if (!need_alignment) {
		rounded_height = round_up(pix_mp->height,
			frame_limits->step_height);
	} else {
		rounded_height = round_up(pix_mp->height,
			dma_align * 4 / frame_limits->step_height);
	}

	rounded_width  = round_up(pix_mp->width, frame_limits->step_width);

	pix_mp->width = clamp(rounded_width,
		frame_limits->min_width, frame_limits->max_width);
	pix_mp->height = clamp(rounded_height,
		frame_limits->min_height, frame_limits->max_height);
	/* TODO Copied from Tomasz Figa code...
	 * What's the purpose of that ?
	 */
	pix_mp->plane_fmt[0].bytesperline = 0;

	return 0;
}

static int vidioc_s_fmt_out(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	struct vb2_queue *vq, *peer_vq;
	int ret;

	/* Change not allowed if queue is streaming. */
	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	if (vb2_is_streaming(vq))
		return -EBUSY;

	peer_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
		V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	/* Pixel format change is not allowed when the other queue has
	 * buffers allocated.
	 */
	if (vb2_is_busy(peer_vq)
		&& pix_mp->pixelformat != ctx->src_fmt.pixelformat) {
		return -EBUSY;
	}

	ret = vidioc_try_fmt_out(file, priv, f);
	if (ret)
		return ret;

	ctx->vpu_src_fmt = rockchip_vpu_find_format(ctx, pix_mp->pixelformat);
	ctx->src_fmt = *pix_mp;

	return 0;
}

/* TODO Shamelessly stolen from Tomasz Figa code without
 * checking anything.
 * Because I can't, right now.
 */
static int vidioc_s_fmt_cap(struct file *file, void *priv, struct v4l2_format *f)
{
	struct v4l2_pix_format_mplane *pix_mp = &f->fmt.pix_mp;
	struct rockchip_vpu_ctx *ctx = fh_to_ctx(priv);
	const struct rockchip_vpu_fmt *fmt;
	struct vb2_queue *vq, *peer_vq;
	__u32 mb_width, mb_height;
	int ret;
	int i;

	vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx, f->type);
	/*
	 * Change not allowed if this queue is streaming.
	 *
	 * NOTE: We allow changes with source queue streaming
	 * to support resolution change in decoded stream.
	 */
	if (vb2_is_streaming(vq))
		return -EBUSY;

	peer_vq = v4l2_m2m_get_vq(ctx->fh.m2m_ctx,
		V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE);
	/*
	 * Pixel format change is not allowed when the other queue has
	 * buffers allocated.
	 */
	if (vb2_is_busy(peer_vq)
		&& pix_mp->pixelformat != ctx->dst_fmt.pixelformat)
		return -EBUSY;

	ret = vidioc_try_fmt_cap(file, priv, f);
	if (ret)
		return ret;

	fmt = rockchip_vpu_find_format(ctx, pix_mp->pixelformat);
	ctx->vpu_dst_fmt = fmt;

	mb_width  = MB_WIDTH(pix_mp->width);
	mb_height = MB_HEIGHT(pix_mp->height);

	vpu_debug(0, "CAPTURE codec mode: %d\n", fmt->codec_mode);
	vpu_debug(0, "fmt - w: %d, h: %d, mb - w: %d, h: %d\n",
			pix_mp->width, pix_mp->height,
			mb_width, mb_height);

	for (i = 0; i < fmt->num_planes; ++i) {
		__u32 const bytesperline = 
			mb_width * MB_DIM * fmt->depth[i] / 8;
		pix_mp->plane_fmt[i].bytesperline =
			bytesperline;

		/*
		 * All of multiplanar formats we support have chroma
		 * planes subsampled by 2.
		 */
		pix_mp->plane_fmt[i].sizeimage =
			(bytesperline * mb_height * MB_DIM) >> (i != 0);
	}

	ctx->dst_fmt = *pix_mp;

	/* TODO Do some checks.
	 * Ezequiel Garcia's code reset "fmt" in some cases.
	 */

	return 0;
}

const struct v4l2_ioctl_ops rockchip_vpu_dec_ioctl_ops = {
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


