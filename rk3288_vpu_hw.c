// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 *	Jeffy Chen <jeffy.chen@rock-chips.com>
 */

#include <linux/clk.h>

#include "rockchip_vpu.h"
#include "rk3288_vpu_regs.h"

#define RK3288_ACLK_MAX_FREQ (400 * 1000 * 1000)

/*
 * Supported formats.
 */

static const struct rockchip_vpu_fmt rk3288_vpu_enc_fmts[] = {
	{
		.fourcc = V4L2_PIX_FMT_YUV420M,
		.codec_mode = RK_VPU_MODE_NONE,
		.num_planes = 3,
		.depth = { 8, 2, 2 },
		.enc_fmt = RK3288_VPU_ENC_FMT_YUV420P,
	},
	{
		.fourcc = V4L2_PIX_FMT_NV12M,
		.codec_mode = RK_VPU_MODE_NONE,
		.num_planes = 2,
		.depth = { 8, 4 },
		.enc_fmt = RK3288_VPU_ENC_FMT_YUV420SP,
	},
	{
		.fourcc = V4L2_PIX_FMT_YUYV,
		.codec_mode = RK_VPU_MODE_NONE,
		.num_planes = 1,
		.depth = { 16 },
		.enc_fmt = RK3288_VPU_ENC_FMT_YUYV422,
	},
	{
		.fourcc = V4L2_PIX_FMT_UYVY,
		.codec_mode = RK_VPU_MODE_NONE,
		.num_planes = 1,
		.depth = { 16 },
		.enc_fmt = RK3288_VPU_ENC_FMT_UYVY422,
	},
	{
		.fourcc = V4L2_PIX_FMT_JPEG_RAW,
		.codec_mode = RK_VPU_MODE_JPEG_ENC,
		.num_planes = 1,
		.max_depth = 2,
		.frmsize = {
			.min_width = 96,
			.max_width = 8192,
			.step_width = MB_DIM,
			.min_height = 32,
			.max_height = 8192,
			.step_height = MB_DIM,
		},
	},
};

static const struct rockchip_vpu_fmt rk3288_vpu_dec_fmts[] = {
	{
		.name = "One slice of an H264 Encoded Stream (RK3288)",
		.fourcc = V4L2_PIX_FMT_H264,
		.codec_mode = RK_VPU_MODE_H264_DEC,
		.num_planes = 1,
		/* FIXME Provide the actual VPU sizes limits for H264 */
		.frmsize = { 
			.min_width = 96,
			.max_width = 4096,
			.step_width = MB_DIM,
			.min_height = 32,
			.max_height = 4096,
			.step_height = MB_DIM,
		},
	},
	{
		.name = "One frame of a VP8 Encoded Stream (RK3288)",
		.fourcc = V4L2_PIX_FMT_VP8,
		.codec_mode = RK_VPU_MODE_VP8_DEC,
		.num_planes = 1,
		/* FIXME Provide the actual VPU sizes limits for VP8 */
		.frmsize = {
			.min_width = 96,
			.max_width = 4096,
			.step_width = MB_DIM,
			.min_height = 32,
			.max_height = 4096,
			.step_height = MB_DIM,
		},
	},
};

static irqreturn_t rk3288_vepu_irq(int irq, void *dev_id)
{
	struct rockchip_vpu_dev *vpu = dev_id;
	u32 status = vepu_read(vpu, VEPU_REG_INTERRUPT);
	u32 bytesused =	vepu_read(vpu, VEPU_REG_STR_BUF_LIMIT) / 8;

	vepu_write(vpu, 0, VEPU_REG_INTERRUPT);
	vepu_write(vpu, 0, VEPU_REG_AXI_CTRL);

	rockchip_vpu_irq_done(vpu,
		bytesused,
		status & VEPU_REG_INTERRUPT_FRAME_RDY ?
		VB2_BUF_STATE_DONE :
		VB2_BUF_STATE_ERROR);
	return IRQ_HANDLED;
}

static irqreturn_t rk3288_vdpu_irq(int irq, void *dev_id)
{
	struct rockchip_vpu_dev *vpu = dev_id;
	printk(KERN_INFO "Handled IRQ %d", irq);

	vdpu_write(vpu, 0, VDPU_REG_INTERRUPT);
	return IRQ_HANDLED;
}

static int rk3288_vpu_hw_init(struct rockchip_vpu_dev *vpu)
{
	/* TODO : ??? The clocks are prepared after setting the
	 * rate ? Shouldn't we do this before ? */
	/* Bump ACLK to max. possible freq. to improve performance. */
	clk_set_rate(vpu->clocks[0].clk, RK3288_ACLK_MAX_FREQ);
	return 0;
}

static void rk3288_vpu_enc_reset(struct rockchip_vpu_ctx *ctx)
{
	struct rockchip_vpu_dev *vpu = ctx->dev;

	vepu_write(vpu, VEPU_REG_INTERRUPT_DIS_BIT, VEPU_REG_INTERRUPT);
	vepu_write(vpu, 0, VEPU_REG_ENC_CTRL);
	vepu_write(vpu, 0, VEPU_REG_AXI_CTRL);
}

/*
 * Supported codec ops.
 */

static const struct rockchip_vpu_codec_ops rk3288_vpu_codec_ops[] = {
	[RK_VPU_MODE_JPEG_ENC] = {
		.run = rk3288_vpu_jpeg_enc_run,
		.reset = rk3288_vpu_enc_reset,
	},
};

/*
 * VPU variant.
 */

const struct rockchip_vpu_variant rk3288_vpu_variant = {
	.enc_offset = 0x0,
	.enc_fmts = rk3288_vpu_enc_fmts,
	.num_enc_fmts = ARRAY_SIZE(rk3288_vpu_enc_fmts),
	.dec_offset = 0x400,
	.dec_fmts = rk3288_vpu_dec_fmts,
	.num_dec_fmts = ARRAY_SIZE(rk3288_vpu_dec_fmts),
	.codec_ops = rk3288_vpu_codec_ops,
	.codec = RK_VPU_CODEC_JPEG,
	.vepu_irq = rk3288_vepu_irq,
	.vdpu_irq = rk3288_vdpu_irq,
	.init = rk3288_vpu_hw_init,
	.clk_names = {"aclk", "hclk"},
	.num_clocks = 2
};
