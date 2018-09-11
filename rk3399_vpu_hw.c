// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 *	Jeffy Chen <jeffy.chen@rock-chips.com>
 */

#include <linux/clk.h>

#include "rockchip_vpu.h"
#include "rk3399_vpu_regs.h"

#define RK3399_ACLK_MAX_FREQ (400 * 1000 * 1000)

/*
 * Supported formats.
 */

static const struct rockchip_vpu_fmt rk3399_vpu_enc_fmts[] = {
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

static irqreturn_t rk3399_vepu_irq(int irq, void *dev_id)
{
	struct rockchip_vpu_dev *vpu = dev_id;
	u32 status = vepu_read(vpu, VEPU_REG_INTERRUPT);
	u32 bytesused =	vepu_read(vpu, VEPU_REG_STR_BUF_LIMIT) / 8;

	vepu_write(vpu, 0, VEPU_REG_INTERRUPT);
	vepu_write(vpu, 0, VEPU_REG_AXI_CTRL);

	rockchip_vpu_irq_done(vpu,
		bytesused,
		status & VEPU_REG_INTERRUPT_FRAME_READY ?
		VB2_BUF_STATE_DONE :
		VB2_BUF_STATE_ERROR);
	return IRQ_HANDLED;
}

static int rk3399_vpu_hw_init(struct rockchip_vpu_dev *vpu)
{
	/* Bump ACLK to max. possible freq. to improve performance. */
	clk_set_rate(vpu->clocks[0].clk, RK3399_ACLK_MAX_FREQ);
	return 0;
}

static void rk3399_vpu_enc_reset(struct rockchip_vpu_ctx *ctx)
{
	struct rockchip_vpu_dev *vpu = ctx->dev;

	vepu_write(vpu, VEPU_REG_INTERRUPT_DIS_BIT, VEPU_REG_INTERRUPT);
	vepu_write(vpu, 0, VEPU_REG_ENCODE_START);
	vepu_write(vpu, 0, VEPU_REG_AXI_CTRL);
}

/*
 * Supported codec ops.
 */

static const struct rockchip_vpu_codec_ops rk3399_vpu_codec_ops[] = {
	[RK_VPU_MODE_JPEG_ENC] = {
		.run = rk3399_vpu_jpeg_enc_run,
		.reset = rk3399_vpu_enc_reset,
	},
};

/*
 * VPU variant.
 */

const struct rockchip_vpu_variant rk3399_vpu_variant = {
	.enc_offset = 0x0,
	.enc_fmts = rk3399_vpu_enc_fmts,
	.num_enc_fmts = ARRAY_SIZE(rk3399_vpu_enc_fmts),
	.codec = RK_VPU_CODEC_JPEG,
	.codec_ops = rk3399_vpu_codec_ops,
	.vepu_irq = rk3399_vepu_irq,
	.init = rk3399_vpu_hw_init,
	.clk_names = {"aclk", "hclk"},
	.num_clocks = 2
};
