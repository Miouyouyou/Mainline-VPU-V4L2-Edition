// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Rockchip Electronics Co., Ltd.
 *
 * JPEG encoder
 * ------------
 * The VPU JPEG encoder produces JPEG baseline sequential format.
 * The quantization coefficients are 8-bit values, complying with
 * the baseline specification. Therefore, it requires application-defined
 * luma and chroma quantization tables. The hardware does entrophy
 * encoding using internal Huffman tables, as specified in the JPEG
 * specification.
 *
 * In other words, only the luma and chroma quantization tables are
 * required as application-defined parameters for the encoding operation.
 *
 * Quantization luma table values are written to registers
 * VEPU_swreg_0-VEPU_swreg_15, and chroma table values to
 * VEPU_swreg_16-VEPU_swreg_31.
 *
 * JPEG zigzag order is expected on the quantization tables.
 */

#include <asm/unaligned.h>
#include <media/v4l2-mem2mem.h>
#include "rockchip_vpu.h"
#include "rockchip_vpu_common.h"
#include "rockchip_vpu_hw.h"
#include "rk3399_vpu_regs.h"

#define VEPU_JPEG_QUANT_TABLE_COUNT 16

static void rk3399_vpu_set_src_img_ctrl(struct rockchip_vpu_dev *vpu,
					struct rockchip_vpu_ctx *ctx)
{
	struct v4l2_pix_format_mplane *pix_fmt = &ctx->src_fmt;
	u32 reg;

	/* The pix fmt width/height are already MiB aligned
	 * by .vidioc_s_fmt_vid_cap_mplane() callback
	 */
	reg = VEPU_REG_IN_IMG_CTRL_ROW_LEN(pix_fmt->width);
	vepu_write_relaxed(vpu, reg, VEPU_REG_INPUT_LUMA_INFO);

	reg = VEPU_REG_IN_IMG_CTRL_OVRFLR_D4(0) |
	      VEPU_REG_IN_IMG_CTRL_OVRFLB(0);
	vepu_write_relaxed(vpu, reg, VEPU_REG_ENC_OVER_FILL_STRM_OFFSET);

	reg = VEPU_REG_IN_IMG_CTRL_FMT(ctx->vpu_src_fmt->enc_fmt);
	vepu_write_relaxed(vpu, reg, VEPU_REG_ENC_CTRL1);
}

static void rk3399_vpu_jpeg_enc_set_buffers(struct rockchip_vpu_dev *vpu,
					 struct rockchip_vpu_ctx *ctx,
					 struct vb2_buffer *src_buf,
					 struct vb2_buffer *dst_buf)
{
	struct v4l2_pix_format_mplane *pix_fmt = &ctx->src_fmt;
	dma_addr_t dst, src[3];
	u32 dst_size;

	WARN_ON(pix_fmt->num_planes > 3);

	dst = vb2_dma_contig_plane_dma_addr(dst_buf, 0);
	dst_size = vb2_plane_size(dst_buf, 0);

	vepu_write_relaxed(vpu, dst, VEPU_REG_ADDR_OUTPUT_STREAM);
	vepu_write_relaxed(vpu, dst_size, VEPU_REG_STR_BUF_LIMIT);

	if (pix_fmt->num_planes == 1) {
		src[0] = vb2_dma_contig_plane_dma_addr(src_buf, 0);
		/* single plane formats we supported are all interlaced */
		src[1] = src[2] = src[0];
	} else if (pix_fmt->num_planes == 2) {
		src[PLANE_Y] = vb2_dma_contig_plane_dma_addr(src_buf, PLANE_Y);
		src[PLANE_CB] = vb2_dma_contig_plane_dma_addr(src_buf, PLANE_CB);
		src[PLANE_CR] = src[PLANE_CB];
	} else {
		src[PLANE_Y] = vb2_dma_contig_plane_dma_addr(src_buf, PLANE_Y);
		src[PLANE_CB] = vb2_dma_contig_plane_dma_addr(src_buf, PLANE_CB);
		src[PLANE_CR] = vb2_dma_contig_plane_dma_addr(src_buf, PLANE_CR);
	}

	vepu_write_relaxed(vpu, src[PLANE_Y], VEPU_REG_ADDR_IN_LUMA);
	vepu_write_relaxed(vpu, src[PLANE_CR], VEPU_REG_ADDR_IN_CR);
	vepu_write_relaxed(vpu, src[PLANE_CB], VEPU_REG_ADDR_IN_CB);
}

static void rk3399_vpu_jpeg_enc_set_qtable(struct rockchip_vpu_dev *vpu,
		const struct v4l2_ctrl_jpeg_quantization *qtable)
{
	const __u16 *chroma_coef;
	const __u16 *luma_coef;
	int i;

	chroma_coef = qtable->chroma_quantization_matrix;
	luma_coef = qtable->luma_quantization_matrix;

	for (i = 0; i < VEPU_JPEG_QUANT_TABLE_COUNT; i++) {
		u32 luma, chroma;

		luma = RK_QUANT_ROW(luma_coef[i*4], luma_coef[i*4 + 1],
				 luma_coef[i*4 + 2], luma_coef[i*4 + 3]);
		chroma = RK_QUANT_ROW(chroma_coef[i*4], chroma_coef[i*4 + 1],
				   chroma_coef[i*4 + 2], chroma_coef[i*4 + 3]);
		vepu_write_relaxed(vpu, luma, VEPU_REG_JPEG_LUMA_QUAT(i));
		vepu_write_relaxed(vpu, chroma, VEPU_REG_JPEG_CHROMA_QUAT(i));
	}
}

void rk3399_vpu_jpeg_enc_run(struct rockchip_vpu_ctx *ctx)
{
	const struct v4l2_ctrl_jpeg_quantization *qtable;
	struct rockchip_vpu_dev *vpu = ctx->dev;
	struct vb2_buffer *src_buf, *dst_buf;
	u32 reg;

	src_buf = v4l2_m2m_next_src_buf(ctx->fh.m2m_ctx);
	dst_buf = v4l2_m2m_next_dst_buf(ctx->fh.m2m_ctx);

	/* Switch to JPEG encoder mode before writing registers */
	vepu_write_relaxed(vpu, VEPU_REG_ENCODE_FORMAT_JPEG,
			   VEPU_REG_ENCODE_START);

	rk3399_vpu_set_src_img_ctrl(vpu, ctx);
	rk3399_vpu_jpeg_enc_set_buffers(vpu, ctx, src_buf, dst_buf);

	qtable = rockchip_vpu_find_control_data(ctx,
			V4L2_CID_JPEG_QUANTIZATION);
	rk3399_vpu_jpeg_enc_set_qtable(vpu, qtable);

	/* Make sure that all registers are written at this point. */
	wmb();

	reg = VEPU_REG_OUTPUT_SWAP32
		| VEPU_REG_OUTPUT_SWAP16
		| VEPU_REG_OUTPUT_SWAP8
		| VEPU_REG_INPUT_SWAP8
		| VEPU_REG_INPUT_SWAP16
		| VEPU_REG_INPUT_SWAP32;
	vepu_write_relaxed(vpu, reg, VEPU_REG_DATA_ENDIAN);

	reg = VEPU_REG_AXI_CTRL_BURST_LEN(16);
	vepu_write_relaxed(vpu, reg, VEPU_REG_AXI_CTRL);

	reg = VEPU_REG_MB_WIDTH(MB_WIDTH(ctx->src_fmt.width))
		| VEPU_REG_MB_HEIGHT(MB_HEIGHT(ctx->src_fmt.height))
		| VEPU_REG_FRAME_TYPE_INTRA
		| VEPU_REG_ENCODE_FORMAT_JPEG
		| VEPU_REG_ENCODE_ENABLE;

	/* Kick the watchdog and start encoding */
	schedule_delayed_work(&vpu->watchdog_work, msecs_to_jiffies(2000));
	vepu_write(vpu, reg, VEPU_REG_ENCODE_START);
}
