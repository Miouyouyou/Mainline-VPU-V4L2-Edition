// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Collabora, Ltd.
 * Copyright (C) 2014 Google, Inc.
 *	Tomasz Figa <tfiga@chromium.org>
 *
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 */

#ifdef DUMBY_THE_EDITOR
#include <generated/autoconf.h>
#endif

#include <linux/clk.h>
#include <linux/module.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/pm.h>
#include <linux/pm_runtime.h>
#include <linux/slab.h>
#include <linux/videodev2.h>
#include <linux/workqueue.h>
#include <media/v4l2-event.h>
#include <media/v4l2-mem2mem.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

#include "rockchip_vpu_common.h"
#include "rockchip_vpu.h"
#include "rockchip_vpu_hw.h"

#define DRIVER_NAME "rockchip-vpu"

int rockchip_vpu_debug;
module_param_named(debug, rockchip_vpu_debug, int, 0644);
MODULE_PARM_DESC(debug,
		 "Debug level - higher value produces more verbose messages");

static void rockchip_vpu_job_finish(struct rockchip_vpu_dev *vpu,
		struct rockchip_vpu_ctx *ctx,
		unsigned int bytesused,
		enum vb2_buffer_state result)
{
	struct vb2_v4l2_buffer *src, *dst;

	src = v4l2_m2m_src_buf_remove(ctx->fh.m2m_ctx);
	dst = v4l2_m2m_dst_buf_remove(ctx->fh.m2m_ctx);

	if (WARN_ON(!src))
		return;
	if (WARN_ON(!dst))
		return;

	src->sequence = ctx->sequence_out++;
	dst->sequence = ctx->sequence_cap++;

	dst->field = src->field;
	dst->timecode = src->timecode;
	dst->vb2_buf.timestamp = src->vb2_buf.timestamp;
	dst->flags &= ~V4L2_BUF_FLAG_TSTAMP_SRC_MASK;
	dst->flags |= src->flags & V4L2_BUF_FLAG_TSTAMP_SRC_MASK;

	if (bytesused)
		dst->vb2_buf.planes[0].bytesused = bytesused;

	v4l2_m2m_buf_done(src, result);
	v4l2_m2m_buf_done(dst, result);

	v4l2_m2m_job_finish(vpu->m2m_enc_dev, ctx->fh.m2m_ctx);

	pm_runtime_mark_last_busy(vpu->dev);
	pm_runtime_put_autosuspend(vpu->dev);
}

void rockchip_vpu_irq_done(struct rockchip_vpu_dev *vpu,
			   unsigned int bytesused,
			   enum vb2_buffer_state result)
{
	struct rockchip_vpu_ctx *ctx =
		(struct rockchip_vpu_ctx *)v4l2_m2m_get_curr_priv(vpu->m2m_enc_dev);

	/* Atomic watchdog cancel. The worker may still be
	 * running after calling this.
	 */
	cancel_delayed_work(&vpu->watchdog_work);
	if (ctx)
		rockchip_vpu_job_finish(vpu, ctx, bytesused, result);
}

void rockchip_vpu_watchdog(struct work_struct *work)
{
	struct rockchip_vpu_dev *vpu;
	struct rockchip_vpu_ctx *ctx;

	vpu = container_of(to_delayed_work(work),
			   struct rockchip_vpu_dev, watchdog_work);
	ctx = (struct rockchip_vpu_ctx *)v4l2_m2m_get_curr_priv(vpu->m2m_enc_dev);
	if (ctx) {
		vpu_err("frame processing timed out!\n");
		ctx->codec_ops->reset(ctx);
		rockchip_vpu_job_finish(vpu, ctx, 0, VB2_BUF_STATE_ERROR);
	}
}

static void device_run(void *priv)
{
	struct rockchip_vpu_ctx *ctx = priv;

	pm_runtime_get_sync(ctx->dev->dev);

	ctx->codec_ops->run(ctx);
}

static struct v4l2_m2m_ops vpu_m2m_ops = {
	.device_run = device_run,
};

static int
enc_queue_init(void *priv, struct vb2_queue *src_vq, struct vb2_queue *dst_vq)
{
	struct rockchip_vpu_ctx *ctx = priv;
	int ret;

	src_vq->type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
	src_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	src_vq->drv_priv = ctx;
	src_vq->ops = &rockchip_vpu_enc_queue_ops;
	src_vq->mem_ops = &vb2_dma_contig_memops;
	src_vq->dma_attrs = DMA_ATTR_ALLOC_SINGLE_PAGES |
			    DMA_ATTR_NO_KERNEL_MAPPING;
	src_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	src_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	src_vq->lock = &ctx->dev->vpu_mutex;
	src_vq->dev = ctx->dev->v4l2_dev.dev;

	ret = vb2_queue_init(src_vq);
	if (ret)
		return ret;

	dst_vq->type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
	dst_vq->io_modes = VB2_MMAP | VB2_DMABUF;
	dst_vq->drv_priv = ctx;
	dst_vq->ops = &rockchip_vpu_enc_queue_ops;
	dst_vq->mem_ops = &vb2_dma_contig_memops;
	dst_vq->dma_attrs = DMA_ATTR_ALLOC_SINGLE_PAGES;
	dst_vq->buf_struct_size = sizeof(struct v4l2_m2m_buffer);
	dst_vq->timestamp_flags = V4L2_BUF_FLAG_TIMESTAMP_COPY;
	dst_vq->lock = &ctx->dev->vpu_mutex;
	dst_vq->dev = ctx->dev->v4l2_dev.dev;

	return vb2_queue_init(dst_vq);
}

static struct rockchip_vpu_ctrl controls[] = {
	{
		.id = V4L2_CID_JPEG_QUANTIZATION,
		.codec = RK_VPU_CODEC_JPEG,
	},
};

void *rockchip_vpu_find_control_data(struct rockchip_vpu_ctx *ctx,
				     unsigned int id)
{
	unsigned int i;

	for (i = 0; i < ctx->num_ctrls; i++) {
		if (!ctx->ctrls[i])
			continue;
		if (ctx->ctrls[i]->id == id)
			return ctx->ctrls[i]->p_cur.p;
	}
	return NULL;
}

static int rockchip_vpu_ctrls_setup(struct rockchip_vpu_dev *vpu,
				    struct rockchip_vpu_ctx *ctx)
{
	int j, i, num_ctrls = ARRAY_SIZE(controls);

	if (num_ctrls > ARRAY_SIZE(ctx->ctrls)) {
		vpu_err("context control array not large enough\n");
		return -EINVAL;
	}

	v4l2_ctrl_handler_init(&ctx->ctrl_handler, num_ctrls);
	if (ctx->ctrl_handler.error) {
		vpu_err("v4l2_ctrl_handler_init failed\n");
		return ctx->ctrl_handler.error;
	}

	for (i = 0, j = 0; i < num_ctrls; i++) {
		if (!(vpu->variant->codec & controls[i].codec))
			continue;
		controls[i].cfg.id = controls[i].id;
		ctx->ctrls[j++] = v4l2_ctrl_new_custom(&ctx->ctrl_handler,
						     &controls[i].cfg, NULL);
		if (ctx->ctrl_handler.error) {
			vpu_err("Adding control (%d) failed %d\n",
				controls[i].id,
				ctx->ctrl_handler.error);
			v4l2_ctrl_handler_free(&ctx->ctrl_handler);
			return ctx->ctrl_handler.error;
		}
	}

	v4l2_ctrl_handler_setup(&ctx->ctrl_handler);
	ctx->num_ctrls = num_ctrls;
	return 0;
}

/*
 * V4L2 file operations.
 */

static int rockchip_vpu_open(struct file *filp)
{
	struct rockchip_vpu_dev *vpu = video_drvdata(filp);
	struct video_device *vdev = video_devdata(filp);
	struct rockchip_vpu_ctx *ctx;
	int ret;

	/*
	 * We do not need any extra locking here, because we operate only
	 * on local data here, except reading few fields from dev, which
	 * do not change through device's lifetime (which is guaranteed by
	 * reference on module from open()) and V4L2 internal objects (such
	 * as vdev and ctx->fh), which have proper locking done in respective
	 * helper functions used here.
	 */

	ctx = kzalloc(sizeof(*ctx), GFP_KERNEL);
	if (!ctx)
		return -ENOMEM;

	ctx->dev = vpu;
	if (vdev == vpu->vfd_enc)
		ctx->fh.m2m_ctx = v4l2_m2m_ctx_init(vpu->m2m_enc_dev, ctx,
						    &enc_queue_init);
	else
		ctx->fh.m2m_ctx = ERR_PTR(-ENODEV);
	if (IS_ERR(ctx->fh.m2m_ctx)) {
		ret = PTR_ERR(ctx->fh.m2m_ctx);
		kfree(ctx);
		return ret;
	}

	v4l2_fh_init(&ctx->fh, vdev);
	filp->private_data = &ctx->fh;
	v4l2_fh_add(&ctx->fh);

	if (vdev == vpu->vfd_enc) {
		rockchip_vpu_enc_reset_dst_fmt(vpu, ctx);
		rockchip_vpu_enc_reset_src_fmt(vpu, ctx);
	}

	ret = rockchip_vpu_ctrls_setup(vpu, ctx);
	if (ret) {
		vpu_err("Failed to set up controls\n");
		goto err_fh_free;
	}
	ctx->fh.ctrl_handler = &ctx->ctrl_handler;
	return 0;

err_fh_free:
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	kfree(ctx);
	return ret;
}

static int rockchip_vpu_release(struct file *filp)
{
	struct rockchip_vpu_ctx *ctx =
		container_of(filp->private_data, struct rockchip_vpu_ctx, fh);

	/*
	 * No need for extra locking because this was the last reference
	 * to this file.
	 */
	v4l2_m2m_ctx_release(ctx->fh.m2m_ctx);
	v4l2_fh_del(&ctx->fh);
	v4l2_fh_exit(&ctx->fh);
	v4l2_ctrl_handler_free(&ctx->ctrl_handler);
	kfree(ctx);

	return 0;
}

static const struct v4l2_file_operations rockchip_vpu_fops = {
	.owner = THIS_MODULE,
	.open = rockchip_vpu_open,
	.release = rockchip_vpu_release,
	.poll = v4l2_m2m_fop_poll,
	.unlocked_ioctl = video_ioctl2,
	.mmap = v4l2_m2m_fop_mmap,
};

static const struct of_device_id of_rockchip_vpu_match[] = {
	{ .compatible = "rockchip,rk3399-vpu", .data = &rk3399_vpu_variant, },
	{ .compatible = "rockchip,rk3288-vpu", .data = &rk3288_vpu_variant, },
	{ /* sentinel */ }
};
MODULE_DEVICE_TABLE(of, of_rockchip_vpu_match);

static const struct video_device rockchip_vfd_common_props = {
	.device_caps = V4L2_CAP_STREAMING | V4L2_CAP_VIDEO_M2M_MPLANE,
	.fops        = &rockchip_vpu_fops,
	.ioctl_ops   = &rockchip_vpu_enc_ioctl_ops,
	.release     = video_device_release,
	.vfl_dir     = VFL_DIR_M2M,
};

static int rockchip_vpu_video_register_device(
	struct rockchip_vpu_dev* __restrict const vpu,
	struct video_device** __restrict const dst,
	struct v4l2_m2m_dev* __restrict const m2m_dev,
	int const media_controller_function,
	char const *  __restrict const name_suffix)
{
	const struct of_device_id *match;
	struct video_device *vfd;
	int ret;

	/* Match the node AGAIN ? */
	match = of_match_node(of_rockchip_vpu_match, vpu->dev->of_node);

	if (!match) {
		dev_err(vpu->dev,
			"... I don't know how matching the same node "
			"twice failed only the second time...");
		return -ENODEV;
	}

	vfd = video_device_alloc();
	if (!vfd) {
		v4l2_err(&vpu->v4l2_dev, "Failed to allocate video device\n");
		return -ENOMEM;
	}

	*vfd = rockchip_vfd_common_props;
	/* Using the same Mutex and SpinLock, since I think that
	 * Rockchip VPU can only do one operation at a time, be it
	 * encoding or decoding.
	 */
	vfd->lock = &vpu->vpu_mutex;
	vfd->v4l2_dev = &vpu->v4l2_dev;
	snprintf(vfd->name, sizeof(vfd->name), "%s-%s",
		match->compatible, name_suffix);

	ret = video_register_device(vfd, VFL_TYPE_GRABBER, 0);
	if (ret) {
		v4l2_err(&vpu->v4l2_dev, "Failed to register video device\n");
		goto err_free_dev;
	}
	v4l2_info(&vpu->v4l2_dev, "registered as /dev/video%d\n", vfd->num);

	ret = v4l2_m2m_register_media_controller(m2m_dev,
				vfd, media_controller_function);

	if (ret) {
		v4l2_err(&vpu->v4l2_dev, "Failed to init mem2mem media controller\n");
		goto err_unreg_video;
	}

	*dst = vfd;
	video_set_drvdata(vfd, vpu);
	return 0;

err_unreg_video:
	video_unregister_device(vfd);
err_free_dev:
	video_device_release(vfd);
	return ret;
}

static int rockchip_vpu_video_register_encoder_device(
	struct rockchip_vpu_dev *vpu)
{
	return rockchip_vpu_video_register_device(
		vpu, &vpu->vfd_enc,
		vpu->m2m_enc_dev, MEDIA_ENT_F_PROC_VIDEO_ENCODER,
		"enc");
}

static int rockchip_vpu_video_register_decoder_device(
	struct rockchip_vpu_dev *vpu)
{
	return rockchip_vpu_video_register_device(
		vpu, &vpu->vfd_dec,
		vpu->m2m_dec_dev, MEDIA_ENT_F_PROC_VIDEO_DECODER,
		"dec");
}

static int rockchip_vpu_probe(struct platform_device *pdev)
{
	const struct of_device_id *match;
	struct rockchip_vpu_dev *vpu;
	struct resource *res;
	int i, ret;

	vpu = devm_kzalloc(&pdev->dev, sizeof(*vpu), GFP_KERNEL);
	if (!vpu)
		return -ENOMEM;

	/* Set up the dev/pdev pointers in the main structure */
	vpu->dev = &pdev->dev;
	vpu->pdev = pdev;

	/* Init a mutex and spinlock */
	mutex_init(&vpu->vpu_mutex);
	spin_lock_init(&vpu->irqlock);

	/* Try to match rockchip,rk3399-vpu or rockchip,rk3288-vpu */
	match = of_match_node(of_rockchip_vpu_match, pdev->dev.of_node);
	if (!match) {
		dev_err(&pdev->dev, "This is not the node your are looking for");
		return -ENODEV;
	}

	/* Use the "variant" data associated with the current match */
	vpu->variant = match->data;

	/* Init a watchdog */
	INIT_DELAYED_WORK(&vpu->watchdog_work, rockchip_vpu_watchdog);

	/* Initialize the clocks */
	for (i = 0; i < vpu->variant->num_clocks; i++)
		vpu->clocks[i].id = vpu->variant->clk_names[i];
	ret = devm_clk_bulk_get(&pdev->dev, vpu->variant->num_clocks,
				vpu->clocks);
	if (ret)
		return ret;

	/* Get the MMIO Virtual Address */
	res = platform_get_resource(vpu->pdev, IORESOURCE_MEM, 0);
	vpu->base = devm_ioremap_resource(vpu->dev, res);
	if (IS_ERR(vpu->base))
		return PTR_ERR(vpu->base);

	/* Infer the encoder and decoder registers address from
	 * that Virtual Address */
	vpu->enc_base = vpu->base + vpu->variant->enc_offset;
	vpu->dec_base = vpu->base + vpu->variant->dec_offset;

	/* Set up the device for DMA transfers */
	ret = dma_set_coherent_mask(vpu->dev, DMA_BIT_MASK(32));
	if (ret) {
		dev_err(vpu->dev, "Could not set DMA coherent mask.\n");
		return ret;
	}

	/* Setup the encoders and decoders IRQ, if needed */
	if (vpu->variant->vepu_irq) {
		int irq;

		irq = platform_get_irq_byname(vpu->pdev, "vepu");
		if (irq <= 0) {
			dev_err(vpu->dev, "Could not get vepu IRQ.\n");
			return -ENXIO;
		}

		ret = devm_request_irq(vpu->dev, irq, vpu->variant->vepu_irq,
				       0, dev_name(vpu->dev), vpu);
		if (ret) {
			dev_err(vpu->dev, "Could not request vepu IRQ.\n");
			return ret;
		}
	}

	if (vpu->variant->vdpu_irq) {
		int irq;

		irq = platform_get_irq_byname(vpu->pdev, "vdpu");
		if (irq <= 0) {
			dev_err(vpu->dev, "Could not get vdpu IRQ.\n");
			return -ENXIO;
		}

		ret = devm_request_irq(vpu->dev, irq, vpu->variant->vdpu_irq,
			0, dev_name(vpu->dev), vpu);
		if (ret) {
			dev_err(vpu->dev, "Could not request vdpu IRQ.\n");
			return ret;
		}
	}

	/* Let the rk3xxx_init function take care of specificities */
	ret = vpu->variant->init(vpu);
	if (ret) {
		dev_err(&pdev->dev, "Failed to init VPU hardware\n");
		return ret;
	}

	/* Set up the Power Management Auto Suspend (??) */
	pm_runtime_set_autosuspend_delay(vpu->dev, 100);
	pm_runtime_use_autosuspend(vpu->dev);
	pm_runtime_enable(vpu->dev);

	/* Prepare the clocks */
	ret = clk_bulk_prepare(vpu->variant->num_clocks, vpu->clocks);
	if (ret) {
		dev_err(&pdev->dev, "Failed to prepare clocks\n");
		return ret;
	}

	/* Register the V4L2 device */
	ret = v4l2_device_register(&pdev->dev, &vpu->v4l2_dev);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register v4l2 device\n");
		goto err_clk_unprepare;
	}
	platform_set_drvdata(pdev, vpu);

	/* Initialize V4L2 M2M operations */
	vpu->m2m_enc_dev = v4l2_m2m_init(&vpu_m2m_ops);
	if (IS_ERR(vpu->m2m_enc_dev)) {
		v4l2_err(&vpu->v4l2_dev, "Failed to init encoder mem2mem device\n");
		ret = PTR_ERR(vpu->m2m_enc_dev);
		goto err_v4l2_unreg;
	}

	vpu->m2m_dec_dev = v4l2_m2m_init(&vpu_m2m_ops);
	if (IS_ERR(vpu->m2m_dec_dev)) {
		v4l2_err(&vpu->v4l2_dev, "Failed to init decoder mem2mem device\n");
		ret = PTR_ERR(vpu->m2m_dec_dev);
		goto err_m2m_enc_dev_rel;
	}

	/* ??? */
	vpu->mdev.dev = vpu->dev;
	strlcpy(vpu->mdev.model, DRIVER_NAME, sizeof(vpu->mdev.model));
	media_device_init(&vpu->mdev);
	vpu->v4l2_dev.mdev = &vpu->mdev;

	ret = rockchip_vpu_video_register_encoder_device(vpu);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register encoder\n");
		goto err_m2m_dec_dev_rel;
	}

	ret = rockchip_vpu_video_register_decoder_device(vpu);
	if (ret) {
		dev_err(&pdev->dev, "Failed to register encoder\n");
		goto err_video_encoder_dev_unreg;
	}

	ret = media_device_register(&vpu->mdev);
	if (ret) {
		v4l2_err(&vpu->v4l2_dev, "Failed to register mem2mem media device\n");
		goto err_video_decoder_dev_unreg;
	}
	return 0;

err_video_decoder_dev_unreg:
	if (vpu->vfd_enc) {
		video_unregister_device(vpu->vfd_dec);
		video_device_release(vpu->vfd_dec);
	}
err_video_encoder_dev_unreg:
	if (vpu->vfd_enc) {
		video_unregister_device(vpu->vfd_enc);
		video_device_release(vpu->vfd_enc);
	}
err_m2m_dec_dev_rel:
	v4l2_m2m_release(vpu->m2m_dec_dev);
err_m2m_enc_dev_rel:
	v4l2_m2m_release(vpu->m2m_enc_dev);
err_v4l2_unreg:
	v4l2_device_unregister(&vpu->v4l2_dev);
err_clk_unprepare:
	clk_bulk_unprepare(vpu->variant->num_clocks, vpu->clocks);
	pm_runtime_disable(vpu->dev);
	return ret;
}

static int rockchip_vpu_remove(struct platform_device *pdev)
{
	struct rockchip_vpu_dev *vpu = platform_get_drvdata(pdev);

	v4l2_info(&vpu->v4l2_dev, "Removing %s\n", pdev->name);

	media_device_unregister(&vpu->mdev);
	v4l2_m2m_unregister_media_controller(vpu->m2m_enc_dev);
	v4l2_m2m_release(vpu->m2m_enc_dev);
	media_device_cleanup(&vpu->mdev);
	if (vpu->vfd_enc) {
		video_unregister_device(vpu->vfd_enc);
		video_device_release(vpu->vfd_enc);
	}
	v4l2_device_unregister(&vpu->v4l2_dev);
	clk_bulk_unprepare(vpu->variant->num_clocks, vpu->clocks);
	pm_runtime_disable(vpu->dev);
	return 0;
}

static int __maybe_unused rockchip_vpu_runtime_suspend(struct device *dev)
{
	struct rockchip_vpu_dev *vpu = dev_get_drvdata(dev);

	clk_bulk_disable(vpu->variant->num_clocks, vpu->clocks);
	return 0;
}

static int __maybe_unused rockchip_vpu_runtime_resume(struct device *dev)
{
	struct rockchip_vpu_dev *vpu = dev_get_drvdata(dev);

	return clk_bulk_enable(vpu->variant->num_clocks, vpu->clocks);
}

static const struct dev_pm_ops rockchip_vpu_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(pm_runtime_force_suspend,
				pm_runtime_force_resume)
	SET_RUNTIME_PM_OPS(rockchip_vpu_runtime_suspend,
			   rockchip_vpu_runtime_resume, NULL)
};

static struct platform_driver rockchip_vpu_driver = {
	.probe = rockchip_vpu_probe,
	.remove = rockchip_vpu_remove,
	.driver = {
		   .name = DRIVER_NAME,
		   .of_match_table = of_match_ptr(of_rockchip_vpu_match),
		   .pm = &rockchip_vpu_pm_ops,
	},
};
module_platform_driver(rockchip_vpu_driver);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Alpha Lin <Alpha.Lin@Rock-Chips.com>");
MODULE_AUTHOR("Tomasz Figa <tfiga@chromium.org>");
MODULE_AUTHOR("Ezequiel Garcia <ezequiel@collabora.com>");
MODULE_DESCRIPTION("Rockchip VPU codec driver");
