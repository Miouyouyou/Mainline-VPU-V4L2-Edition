// SPDX-License-Identifier: GPL-2.0
/*
 * Rockchip VPU codec driver
 *
 * Copyright (C) 2018 Google, Inc.
 *	Tomasz Figa <tfiga@chromium.org>
 *
 * Based on s5p-mfc driver by Samsung Electronics Co., Ltd.
 * Copyright (C) 2011 Samsung Electronics Co., Ltd.
 */

#ifndef ROCKCHIP_VPU_H_
#define ROCKCHIP_VPU_H_

#include <linux/platform_device.h>
#include <linux/videodev2.h>
#include <linux/wait.h>
#include <linux/clk.h>

#include <media/v4l2-ctrls.h>
#include <media/v4l2-device.h>
#include <media/v4l2-ioctl.h>
#include <media/videobuf2-core.h>
#include <media/videobuf2-dma-contig.h>

#include "rockchip_vpu_hw.h"

#define ROCKCHIP_VPU_MAX_CLOCKS		4
#define ROCKCHIP_VPU_MAX_CTRLS		16

#define MB_DIM				16
#define MB_WIDTH(x_size)		DIV_ROUND_UP(x_size, MB_DIM)
#define MB_HEIGHT(y_size)		DIV_ROUND_UP(y_size, MB_DIM)
#define SB_DIM				64
#define SB_WIDTH(x_size)		DIV_ROUND_UP(x_size, SB_DIM)
#define SB_HEIGHT(y_size)		DIV_ROUND_UP(y_size, SB_DIM)

struct rockchip_vpu_ctx;
struct rockchip_vpu_codec_ops;

#define	RK_VPU_CODEC_JPEG BIT(0)

/**
 * struct rockchip_vpu_variant - information about VPU hardware variant
 *
 * @enc_offset:   Offset from VPU base to encoder registers.
 * @enc_fmts:     Encoder formats.
 * @num_enc_fmts: Number of encoder formats.
 * @dec_offset:   Offset from VPU base to decoder registers.
 * @dec_fmts:     Decoder formats.
 * @num_dec_fmts: Number of decoder formats.
 * @codec:        Supported codecs
 * @codec_ops:    Codec ops.
 * @init:         Initialize hardware.
 * @vepu_irq:     encoder interrupt handler
 * @vdpu_irq:     decoder interrupt handler
 * @clocks:       array of clock names
 * @num_clocks:   number of clocks in the array
 */
struct rockchip_vpu_variant {
	unsigned int enc_offset;
	const struct rockchip_vpu_fmt *enc_fmts;
	unsigned int num_enc_fmts;

	unsigned int dec_offset;
	const struct rockchip_vpu_fmt *dec_fmts;
	unsigned int num_dec_fmts;

	unsigned int codec;
	const struct rockchip_vpu_codec_ops *codec_ops;
	int (*init)(struct rockchip_vpu_dev *vpu);

	irqreturn_t (*vepu_irq)(int irq, void *priv);
	irqreturn_t (*vdpu_irq)(int irq, void *priv);

	const char *clk_names[ROCKCHIP_VPU_MAX_CLOCKS];
	int num_clocks;
};

/**
 * enum rockchip_vpu_codec_mode - codec operating mode.
 * @RK_VPU_MODE_NONE:  No operating mode. Used for RAW video formats.
 * @RK_VPU_MODE_JPEG_ENC: JPEG encoder.
 */
enum rockchip_vpu_codec_mode {
	RK_VPU_MODE_NONE = -1,
	RK_VPU_MODE_JPEG_ENC,
};

struct rockchip_vpu_ctrl {
	u32 id;
	bool required;
	unsigned int codec;
	struct v4l2_ctrl_config cfg;
};

/**
 * enum rockchip_vpu_plane - indices of planes inside a VB2 buffer.
 * @PLANE_Y:		Plane containing luminance data (also denoted as Y).
 * @PLANE_CB_CR:	Plane containing interleaved chrominance data (also
 *			denoted as CbCr).
 * @PLANE_CB:		Plane containing CB part of chrominance data.
 * @PLANE_CR:		Plane containing CR part of chrominance data.
 */
enum rockchip_vpu_plane {
	PLANE_Y		= 0,
	PLANE_CB_CR	= 1,
	PLANE_CB	= 1,
	PLANE_CR	= 2,
};

/**
 * struct rockchip_vpu_dev - driver data
 * @v4l2_dev:		V4L2 device to register video devices for.
 * @m2m_enc_dev: M2M Encoder device. Used with M2M functions.
 * @m2m_dev_dev: M2M Decoder device. Used with M2M functions.
 * @vfd_enc:		Video device for encoder.
 * @pdev:		Pointer to VPU platform device.
 * @dev:		Pointer to device for convenient logging using
 *			dev_ macros.
 * @clocks:		Array of clock handles.
 * @base:		Mapped address of VPU registers.
 * @enc_base:		Mapped address of VPU encoder register for convenience.
 * @vpu_mutex:		Mutex to synchronize V4L2 calls.
 * @irqlock:		Spinlock to synchronize access to data structures
 *			shared with interrupt handlers.
 * @variant:		Hardware variant-specific parameters.
 * @watchdog_work:	Delayed work for hardware timeout handling.
 */
struct rockchip_vpu_dev {
	struct v4l2_device v4l2_dev;
	struct v4l2_m2m_dev *m2m_enc_dev;
	struct v4l2_m2m_dev *m2m_dec_dev;
	struct media_device mdev;
	struct video_device *vfd_enc;
	struct video_device *vfd_dec;
	struct platform_device *pdev;
	struct device *dev;
	struct clk_bulk_data clocks[ROCKCHIP_VPU_MAX_CLOCKS];
	void __iomem *base;
	void __iomem *enc_base;
	void __iomem *dec_base;

	struct mutex vpu_mutex;	/* video_device lock */
	spinlock_t irqlock;
	const struct rockchip_vpu_variant *variant;
	struct delayed_work watchdog_work;
};

/**
 * struct rockchip_vpu_aux_buf - auxiliary DMA buffer for hardware data
 * @cpu:        CPU pointer to the buffer.
 * @dma:        DMA address of the buffer.
 * @size:       Size of the buffer.
 */
struct rockchip_vpu_aux_buf {
	void *cpu;
	dma_addr_t dma;
	size_t size;
};

/**
 * struct rockchip_vpu_ctx - Context (instance) private data.
 *
 * @dev:		VPU driver data to which the context belongs.
 * @fh:			V4L2 file handler.
 *
 * @sequence_cap:       Sequence counter for capture queue
 * @sequence_out:       Sequence counter for output queue
 * @codec_mode:		Active codec mode
 *
 * @vpu_src_fmt:	Descriptor of active source format.
 * @src_fmt:		V4L2 pixel format of active source format.
 * @vpu_dst_fmt:	Descriptor of active destination format.
 * @dst_fmt:		V4L2 pixel format of active destination format.
 *
 * @ctrls:		Array containing pointer to registered controls.
 * @ctrl_handler:	Control handler used to register controls.
 * @num_ctrls:		Number of registered controls.
 *
 * @codec_ops:		Set of operations related to codec mode.
 */
struct rockchip_vpu_ctx {
	struct rockchip_vpu_dev *dev;
	struct v4l2_fh fh;

	u32 sequence_cap;
	u32 sequence_out;

	/* Format info */
	const struct rockchip_vpu_fmt *vpu_src_fmt;
	struct v4l2_pix_format_mplane src_fmt;
	const struct rockchip_vpu_fmt *vpu_dst_fmt;
	struct v4l2_pix_format_mplane dst_fmt;

	enum v4l2_colorspace colorspace;
	enum v4l2_ycbcr_encoding ycbcr_enc;
	enum v4l2_quantization quantization;
	enum v4l2_xfer_func xfer_func;

	/* Controls */
	struct v4l2_ctrl *ctrls[ROCKCHIP_VPU_MAX_CTRLS];
	struct v4l2_ctrl_handler ctrl_handler;
	unsigned int num_ctrls;

	const struct rockchip_vpu_codec_ops *codec_ops;
	struct vb2_buffer *dst_bufs[VIDEO_MAX_FRAME];
};

/**
 * struct rockchip_vpu_fmt - information about supported video formats.
 * @name:	Human readable name of the format.
 * @fourcc:	FourCC code of the format. See V4L2_PIX_FMT_*.
 * @codec_mode:	Codec mode related to this format. See
 *		enum rockchip_vpu_codec_mode.
 * @num_planes:	Number of planes used by this format.
 * @max_depth:	Maximum depth, for bitstream formats
 * @depth:	Depth of each plane in bits per pixel.
 * @enc_fmt:	Format identifier for encoder registers.
 * @frmsize:	Supported range of frame sizes (only for bitstream formats).
 */
struct rockchip_vpu_fmt {
	char *name;
	u32 fourcc;
	enum rockchip_vpu_codec_mode codec_mode;
	int num_planes;
	int max_depth;
	u8 depth[VIDEO_MAX_PLANES];
	enum rockchip_vpu_enc_fmt enc_fmt;
	struct v4l2_frmsize_stepwise frmsize;
};

#define RK_QUANT_ROW(e0, e1, e2, e3) \
	(e0 & 0xff) | ((e1 & 0xff) << 8) | ((e2 & 0xff) << 16) | ((e3 & 0xff) << 24)

/* Logging helpers */

/**
 * debug - Module parameter to control level of debugging messages.
 *
 * Level of debugging messages can be controlled by bits of module parameter
 * called "debug". Meaning of particular bits is as follows:
 *
 * bit 0 - global information: mode, size, init, release
 * bit 1 - each run start/result information
 * bit 2 - contents of small controls from userspace
 * bit 3 - contents of big controls from userspace
 * bit 4 - detail fmt, ctrl, buffer q/dq information
 * bit 5 - detail function enter/leave trace information
 * bit 6 - register write/read information
 */
extern int rockchip_vpu_debug;

#define vpu_debug(level, fmt, args...)				\
	do {							\
		if (rockchip_vpu_debug & BIT(level))		\
			pr_info("%s:%d: " fmt,	                \
				 __func__, __LINE__, ##args);	\
	} while (0)

#define vpu_err(fmt, args...)					\
	pr_err("%s:%d: " fmt, __func__, __LINE__, ##args)

static inline char *fmt2str(u32 fmt, char *str)
{
	char a = fmt & 0xFF;
	char b = (fmt >> 8) & 0xFF;
	char c = (fmt >> 16) & 0xFF;
	char d = (fmt >> 24) & 0xFF;

	sprintf(str, "%c%c%c%c", a, b, c, d);

	return str;
}

/* Structure access helpers. */
static inline struct rockchip_vpu_ctx *fh_to_ctx(struct v4l2_fh *fh)
{
	return container_of(fh, struct rockchip_vpu_ctx, fh);
}

static inline unsigned int rockchip_vpu_rounded_luma_size(unsigned int w,
							  unsigned int h)
{
	return round_up(w, MB_DIM) * round_up(h, MB_DIM);
}

int rockchip_vpu_enc_ctrls_setup(struct rockchip_vpu_ctx *ctx);

/* Register accessors. */
static inline void vepu_write_relaxed(struct rockchip_vpu_dev *vpu,
				       u32 val, u32 reg)
{
	vpu_debug(6, "MARK: set reg[%03d]: %08x\n", reg / 4, val);
	writel_relaxed(val, vpu->enc_base + reg);
}

static inline void vepu_write(struct rockchip_vpu_dev *vpu, u32 val, u32 reg)
{
	vpu_debug(6, "MARK: set reg[%03d]: %08x\n", reg / 4, val);
	writel(val, vpu->enc_base + reg);
}

static inline u32 vepu_read(struct rockchip_vpu_dev *vpu, u32 reg)
{
	u32 val = readl(vpu->enc_base + reg);

	vpu_debug(6, "MARK: get reg[%03d]: %08x\n", reg / 4, val);
	return val;
}

static inline void vdpu_write_relaxed(
	struct rockchip_vpu_dev *vpu,
	u32 val, u32 reg)
{
	vpu_debug(6, "MARK: Decoder - set reg[%03d]: %08x\n", reg / 4, val);
	writel_relaxed(val, vpu->dec_base + reg);
}

static inline void vdpu_write(
	struct rockchip_vpu_dev *vpu,
	u32 val, u32 reg)
{
	vpu_debug(6, "MARK: Decoder - set reg[%03d]: %08x\n", reg / 4, val);
	writel(val, vpu->dec_base + reg);
}

static inline void vdpu_read(struct rockchip_vpu_dev *vpu, u32 reg)
{
	u32 val = readl(vpu->dec_base + reg);

	vpu_debug(6, "MARK: Decoder - get reg[%03d]: %08x\n", reg / 4, val);
	writel_relaxed(val, vpu->dec_base + reg);
}


#endif /* ROCKCHIP_VPU_H_ */
