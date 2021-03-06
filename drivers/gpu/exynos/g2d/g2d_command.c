/*
 * linux/drivers/gpu/exynos/g2d/g2d_command.c
 *
 * Copyright (C) 2017 Samsung Electronics Co., Ltd.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 */

#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/device.h>

#include <video/exynos_hdr_tunables.h>

#include "g2d.h"
#include "g2d_task.h"
#include "g2d_uapi.h"
#include "g2d_command.h"
#include "g2d_regs.h"
#include "g2d_format.h"

#define layer_crop_width(layer)					\
		(((layer)->commands[G2DSFR_IMG_RIGHT].value) -	\
		 ((layer)->commands[G2DSFR_IMG_LEFT].value))
#define layer_crop_height(layer)				\
		(((layer)->commands[G2DSFR_IMG_BOTTOM].value) -	\
		 ((layer)->commands[G2DSFR_IMG_TOP].value))
#define layer_width(layer)	((layer)->commands[G2DSFR_IMG_WIDTH].value)
#define layer_height(layer)	((layer)->commands[G2DSFR_IMG_HEIGHT].value)
#define layer_pixelcount(layer)	(layer_width(layer) * layer_height(layer))

enum {
	TASK_REG_SOFT_RESET,
	TASK_REG_SECURE_MODE,
	TASK_REG_LAYER_UPDATE,

	TASK_REG_COUNT
};

/*
 * G2D_SECURE_LAYER_REG and G2D_LAYER_UPDATE_REG are updated in
 * g2d_prepare_source() and g2d_prepare_target().
 */
static struct g2d_reg g2d_setup_commands[TASK_REG_COUNT] = {
	{G2D_SOFT_RESET_REG,   0x00000004}, /* CoreSFRClear */
	{G2D_SECURE_MODE_REG,  0x00000000},
	{G2D_LAYER_UPDATE_REG, 0x00000000},
};

void g2d_init_commands(struct g2d_task *task)
{
	memcpy(page_address(task->cmd_page),
		&g2d_setup_commands, sizeof(g2d_setup_commands));
	task->sec.cmd_count = ARRAY_SIZE(g2d_setup_commands);
}

static void g2d_set_taskctl_commands(struct g2d_task *task)
{
	struct g2d_reg *regs = (struct g2d_reg *)page_address(task->cmd_page);
	struct g2d_layer *layer;
	u32 rot = 0;
	u32 n_rot = 0;
	u32 size = 0; /* Size doesn't cause overflow */
	u32 width = layer_width(&task->target);
	u32 height = layer_height(&task->target);
	int i;

	for (i = 0; i < task->num_source; i++) {
		layer = &task->source[i];
		size = layer_crop_width(layer) * layer_crop_height(layer);

		if (layer->commands[G2DSFR_SRC_ROTATE].value & 1)
			rot += size;
		else
			n_rot += size;
	}

	if (rot > n_rot) {
		u32 mode = task->target.commands[G2DSFR_IMG_COLORMODE].value;

		regs[task->sec.cmd_count].offset = G2D_TILE_DIRECTION_ORDER_REG;
		regs[task->sec.cmd_count].value = G2D_TILE_DIRECTION_VERTICAL;

		if (!IS_HWFC(task->flags) &&
		    (IS_YUV420(mode) || IS_YUV422_2P(mode)))
			regs[task->sec.cmd_count].value |=
					G2D_TILE_DIRECTION_ZORDER;

		task->sec.cmd_count++;
	}

	/*
	 * Divide the entire destination in half by verital or horizontal,
	 * and let the H/W work in parallel.
	 * split index is half the width or height divided by 16
	 */
	regs[task->sec.cmd_count].offset = G2D_DST_SPLIT_TILE_IDX_REG;
	if (!IS_HWFC(task->flags) && (height > width))
		regs[task->sec.cmd_count].value = ((height / 2) >> 4);
	else
		regs[task->sec.cmd_count].value =
			((width / 2) >> 4) | G2D_DST_SPLIT_TILE_IDX_VFLAG;

	task->sec.cmd_count++;
}

static void g2d_set_hwfc_commands(struct g2d_task *task)
{
	struct g2d_reg *regs = (struct g2d_reg *)page_address(task->cmd_page);

	regs[task->sec.cmd_count].offset = G2D_HWFC_CAPTURE_IDX_REG;
	regs[task->sec.cmd_count].value = IS_HWFC(task->flags) ?
			G2D_HWFC_CAPTURE_HWFC_JOB : 0;
	regs[task->sec.cmd_count].value |= task->sec.job_id;

	task->sec.cmd_count++;
}

static void g2d_set_start_commands(struct g2d_task *task)
{
	bool self_prot = task->g2d_dev->caps & G2D_DEVICE_CAPS_SELF_PROTECTION;
	struct g2d_reg *regs = page_address(task->cmd_page);

	if (!self_prot && IS_ENABLED(CONFIG_EXYNOS_CONTENT_PATH_PROTECTION))
		return;

	/*
	 * Number of commands should be multiple of 8.
	 * If it is not, then pad dummy commands with no side effect.
	 */
	while ((task->sec.cmd_count & 7) != 0) {
		regs[task->sec.cmd_count].offset = G2D_LAYER_UPDATE_REG;
		regs[task->sec.cmd_count].value =
					regs[TASK_REG_LAYER_UPDATE].value;
		task->sec.cmd_count++;
	}
}

void g2d_complete_commands(struct g2d_task *task)
{
	g2d_set_taskctl_commands(task);

	g2d_set_hwfc_commands(task);

	g2d_set_start_commands(task);

	BUG_ON(task->sec.cmd_count > G2D_MAX_COMMAND);
}

static const struct g2d_fmt g2d_formats_common[] = {
	{
		.name		= "ARGB8888",
		.fmtvalue	= G2D_FMT_ARGB8888,	/* [31:0] ARGB */
		.bpp		= { 32 },
		.num_planes	= 1,
	}, {
		.name		= "ABGR8888",
		.fmtvalue	= G2D_FMT_ABGR8888,	/* [31:0] ABGR */
		.bpp		= { 32 },
		.num_planes	= 1,
	}, {
		.name		= "XBGR8888",
		.fmtvalue	= G2D_FMT_XBGR8888,	/* [31:0] XBGR */
		.bpp		= { 32 },
		.num_planes	= 1,
	}, {
		.name		= "XRGB8888",
		.fmtvalue	= G2D_FMT_XRGB8888,	/* [31:0] XBGR */
		.bpp		= { 32 },
		.num_planes	= 1,
	}, {
		.name		= "RGB888",
		.fmtvalue	= G2D_FMT_RGB888,	/* [23:0] RGB */
		.bpp		= { 24 },
		.num_planes	= 1,
	}, {
		.name		= "ARGB4444",
		.fmtvalue	= G2D_FMT_ARGB4444,	/* [15:0] ARGB */
		.bpp		= { 16 },
		.num_planes	= 1,
	}, {
		.name		= "ARGB1555",
		.fmtvalue	= G2D_FMT_ARGB1555,	/* [15:0] ARGB */
		.bpp		= { 16 },
		.num_planes	= 1,
	}, {
		.name		= "RGB565",
		.fmtvalue	= G2D_FMT_RGB565,	/* [15:0] RGB */
		.bpp		= { 16 },
		.num_planes	= 1,
	}, {
		.name		= "BGR565",
		.fmtvalue	= G2D_FMT_BGR565,	/* [15:0] BGR */
		.bpp		= { 16 },
		.num_planes	= 1,
	}, {
		.name		= "NV12",
		.fmtvalue	= G2D_FMT_NV12,
		.bpp		= { 8, 4 },
		.num_planes	= 2,
	}, {
		.name		= "NV21",
		.fmtvalue	= G2D_FMT_NV21,
		.bpp		= { 8, 4 },
		.num_planes	= 2,
	}, {
		.name		= "YUYV",
		.fmtvalue	= G2D_FMT_YUYV,
		.bpp		= { 16 },
		.num_planes	= 1,
	}, {
		.name		= "YVYU",
		.fmtvalue	= G2D_FMT_YVYU,
		.bpp		= { 16 },
		.num_planes	= 1,
	}, {
		.name		= "UYVY",
		.fmtvalue	= G2D_FMT_UYVY,
		.bpp		= { 16 },
		.num_planes	= 1,
	}, {
		.name		= "VYUY",
		.fmtvalue	= G2D_FMT_VYUY,
		.bpp		= { 16 },
		.num_planes	= 1,
	}, {
		.name		= "NV16",
		.fmtvalue	= G2D_FMT_NV16,
		.bpp		= { 8, 8 },
		.num_planes	= 2,
	}, {
		.name		= "NV61",
		.fmtvalue	= G2D_FMT_NV61,
		.bpp		= { 8, 8 },
		.num_planes	= 2,
	}, {
		.name		= "ABGR2101010",
		.fmtvalue	= G2D_FMT_ABGR2101010,	/* [31:0] ABGR */
		.bpp		= { 32 },
		.num_planes	= 1,
	},
};

static const struct g2d_fmt g2d_formats_9810[] = {
	{
		.name		= "NV12_8+2",
		.fmtvalue	= G2D_FMT_NV12_82_9810,
		.bpp		= { 8, 2, 4, 1 },
		.num_planes	= 4,
	}, {
		.name		= "NV21_8+2",
		.fmtvalue	= G2D_FMT_NV21_82_9810,
		.bpp		= { 8, 2, 4, 1 },
		.num_planes	= 4,
	}, {
		.name		= "NV12_P010",
		.fmtvalue	= G2D_FMT_NV12_P010_9810,
		.bpp		= { 16, 8},
		.num_planes	= 2,
	}, {
		.name		= "NV21_P010",
		.fmtvalue	= G2D_FMT_NV21_P010_9810,
		.bpp		= { 16, 8},
		.num_planes	= 2,
	},
};

static const struct g2d_fmt g2d_formats_9820[] = {
	{
		.name		= "NV12_8+2",
		.fmtvalue	= G2D_FMT_NV12_82_9820,
		.bpp		= { 8, 2, 4, 1 },
		.num_planes	= 4,
	}, {
		.name		= "NV21_8+2",
		.fmtvalue	= G2D_FMT_NV21_82_9820,
		.bpp		= { 8, 2, 4, 1 },
		.num_planes	= 4,
	}, {
		.name		= "NV12_P010",
		.fmtvalue	= G2D_FMT_NV12_P010_9820,
		.bpp		= { 16, 8},
		.num_planes	= 2,
	}, {
		.name		= "NV21_P010",
		.fmtvalue	= G2D_FMT_NV21_P010_9820,
		.bpp		= { 16, 8},
		.num_planes	= 2,
	}, {
		.name		= "NV16_P210",
		.fmtvalue	= G2D_FMT_NV16_P210_9820,
		.bpp		= { 16, 16},
		.num_planes	= 2,
	}, {
		.name		= "NV61_P210",
		.fmtvalue	= G2D_FMT_NV61_P210_9820,
		.bpp		= { 16, 16},
		.num_planes	= 2,
	},
};

const struct g2d_fmt *g2d_find_format(u32 fmtval, unsigned long devcaps)
{
	size_t i;

	for (i = 0; i < ARRAY_SIZE(g2d_formats_common); i++)
		if (g2d_formats_common[i].fmtvalue == G2D_IMGFMT(fmtval))
			return &g2d_formats_common[i];

	if (!(devcaps & G2D_DEVICE_CAPS_YUV_BITDEPTH)) {
		for (i = 0; i < ARRAY_SIZE(g2d_formats_9810); i++)
			if (g2d_formats_9810[i].fmtvalue == G2D_IMGFMT(fmtval))
				return &g2d_formats_9810[i];
	} else {
		for (i = 0; i < ARRAY_SIZE(g2d_formats_9820); i++)
			if (g2d_formats_9820[i].fmtvalue == G2D_IMGFMT(fmtval))
				return &g2d_formats_9820[i];
	}
	return NULL;
}

/*
 * Buffer stride alignment and padding restriction of MFC
 * YCbCr420 semi-planar 8+2 layout:
 *    Y8 -> Y2 -> C8 -> C2
 * 8 bit segments:
 *  - width stride: 16 bytes
 *  - height stride: 16 pixels
 *  - padding 256 bytes
 * 2 bit segments:
 *  - width stride: 16 bytes
 *  - height stride: 16 pixels
 *  - padding: 64 bytes
 */
#define MFC_PAD_SIZE		     256
#define MFC_2B_PAD_SIZE		     (MFC_PAD_SIZE / 4)
#define MFC_ALIGN(v)		     ALIGN(v, 16)

#define NV12_MFC_Y_PAYLOAD(w, h)     (MFC_ALIGN(w) * MFC_ALIGN(h))
#define NV12_MFC_Y_PAYLOAD_PAD(w, h) (NV12_MFC_Y_PAYLOAD(w, h) + MFC_PAD_SIZE)
#define NV12_MFC_C_PAYLOAD(w, h)     (MFC_ALIGN(w) * (h) / 2)
#define NV12_MFC_C_PAYLOAD_ALIGNED(w, h) (NV12_MFC_Y_PAYLOAD(w, h) / 2)
#define NV12_MFC_C_PAYLOAD_PAD(w, h) (NV12_MFC_C_PAYLOAD_ALIGNED(w, h) +      \
				      MFC_PAD_SIZE)
#define NV12_MFC_PAYLOAD(w, h)       (NV12_MFC_Y_PAYLOAD_PAD(w, h) +	      \
				      NV12_MFC_C_PAYLOAD(w, h))
#define NV12_82_MFC_Y_PAYLOAD(w, h)  (NV12_MFC_Y_PAYLOAD_PAD(w, h) +	      \
				      MFC_ALIGN((w) / 4) * (h))
#define NV12_82_MFC_C_PAYLOAD(w, h)  (NV12_MFC_C_PAYLOAD_PAD(w, h) +	      \
				      MFC_ALIGN((w) / 4) * (h) / 2)
#define NV12_82_MFC_PAYLOAD(w, h)    (NV12_MFC_Y_PAYLOAD_PAD(w, h) +	      \
				      MFC_ALIGN((w) / 4) * MFC_ALIGN(h) +     \
				      MFC_2B_PAD_SIZE +			      \
				      NV12_82_MFC_C_PAYLOAD(w, h))
#define NV12_82_MFC_2Y_PAYLOAD(w, h) (MFC_ALIGN((w) / 4) * MFC_ALIGN(h))
#define NV12_MFC_CBASE(base, w, h)   (base + NV12_MFC_Y_PAYLOAD_PAD(w, h))

#define YUV82_BASE_ALIGNED(addr, idx) IS_ALIGNED((addr), 32 >> (idx & 1))
#define YUV82_BASE_ALIGN(addr, idx)   ALIGN((addr), 32 >> (idx & 1))

static unsigned char src_base_reg_offset[4] = {0x1C, 0x80, 0x64, 0x68};
static unsigned char src_base_reg_offset_yuv82[4] = {0x1C, 0x64, 0x80, 0x68};
static unsigned char dst_base_reg_offset[4] = {0x00, 0x50, 0x30, 0x34};

static unsigned char src_sbwc_reg_offset[4] = {0x68, 0x84, 0x64, 0x80};
static unsigned char dst_sbwc_reg_offset[4] = {0x34, 0x54, 0x30, 0x50};

#define BASE_REG_OFFSET(base, offsets, buf_idx)	((base) + (offsets)[(buf_idx)])

#define AFBC_HEADER_SIZE(cmd)						\
		((ALIGN((cmd)[G2DSFR_IMG_WIDTH].value, 16) / 16) *	\
		 (ALIGN((cmd)[G2DSFR_IMG_HEIGHT].value, 16) / 16) * 16)

#define SBWC_BLOCK_WIDTH 32
#define SBWC_BLOCK_HEIGHT 4
#define SBWC_BLOCK_SIZE(bit) (SBWC_BLOCK_WIDTH * SBWC_BLOCK_HEIGHT * (bit) / 8)

#define SBWC_HEADER_ALIGN 16
#define SBWC_PAYLOAD_ALIGN 32

#define SBWC_HEADER_STRIDE(w) \
	ALIGN(((w) / SBWC_BLOCK_WIDTH / 2), SBWC_HEADER_ALIGN)
#define SBWC_PAYLOAD_STRIDE(w, dep)\
	ALIGN(((w) / SBWC_BLOCK_WIDTH) * SBWC_BLOCK_SIZE(dep), \
	      SBWC_PAYLOAD_ALIGN)

#define SBWC_PAYLOAD_Y_SIZE(w, h, dep) \
	(SBWC_PAYLOAD_STRIDE(w, dep) * ((h) / 4))
#define SBWC_HEADER_Y_SIZE(w, h) \
	(SBWC_HEADER_STRIDE(w) * ((h) / 4))

#define SBWC_PAYLOAD_422_C_SIZE(w, h, dep) SBWC_PAYLOAD_Y_SIZE(w, h, dep)
#define SBWC_HEADER_422_C_SIZE(w, h) SBWC_HEADER_Y_SIZE(w, h)

#define SBWC_PAYLOAD_420_C_SIZE(w, h, dep) \
	(SBWC_PAYLOAD_STRIDE(w, dep) * (ALIGN((h), 8) / 8))
#define SBWC_HEADER_420_C_SIZE(w, h) \
	(SBWC_HEADER_STRIDE(w) * (ALIGN((h), 8) / 8))

#define SBWC_Y_SIZE(w, h, dep) \
	(SBWC_PAYLOAD_Y_SIZE(w, h, dep) + SBWC_HEADER_Y_SIZE(w, h))
#define SBWC_422_C_SIZE(w, h, dep) \
	(SBWC_PAYLOAD_422_C_SIZE(w, h, dep) + SBWC_HEADER_422_C_SIZE(w, h))

#define SBWC_CBCR_BASE(base, w, h, dep) \
	((base) + SBWC_Y_SIZE(w, h, dep))

// SBWC Lossy format definition
#define SBWCL_STRIDE(w, block_byte) \
	ALIGN(((w) / SBWC_BLOCK_WIDTH) * (block_byte), \
	      SBWC_PAYLOAD_ALIGN)

#define SBWCL_Y_SIZE(w, h, block_byte) \
	(SBWCL_STRIDE(w, block_byte) * ((h) / 4))
#define SBWCL_422_C_SIZE(w, h, block_byte) \
	SBWCL_Y_SIZE(w, h, block_byte)
#define SBWCL_420_C_SIZE(w, h, block_byte) \
	(SBWCL_STRIDE(w, block_byte) * (ALIGN((h), 8) / 8))

#define SBWCL_CBCR_BASE(base, w, h, block_byte) \
	((base) + SBWCL_Y_SIZE(w, h, block_byte))

#define SBWCL_BLOCK_SIZE(sbwc) ((((sbwc) >> 4) & 0xF) << 5)
#define IS_SBWCL(sbwc) ((sbwc) & 1)

/*
 * Buffer stride alignment and padding restriction of MFC
 * YCbCr semi-planar SBWC layout:
 *    Y payload -> Y header -> CbCr payload -> CbCr header
 *
 * payload segments:
 *  - padding : 64 bytes
 *  - height : 8 pixel
 * header segments:
 *  - padding : 256 bytes
 *  - height : 8 pixel
 */
#define MFC_SBWC_PAYLOAD_PAD	64
#define MFC_SBWC_HEADER_PAD	256

#define MFC_SBWC_PAYLOAD_Y_SIZE(w, h, dep) \
	(SBWC_PAYLOAD_STRIDE(w, dep) * (ALIGN((h), 8) / 4) + \
	 MFC_SBWC_PAYLOAD_PAD)
#define MFC_SBWC_HEADER_Y_SIZE(w, h) \
	(SBWC_HEADER_STRIDE(w) * (ALIGN((h), 8) / 4) + \
	 MFC_SBWC_HEADER_PAD)

#define MFC_SBWC_PAYLOAD_420_C_SIZE(w, h, dep) \
	(SBWC_PAYLOAD_420_C_SIZE(w, h, dep) + MFC_SBWC_PAYLOAD_PAD)
#define MFC_SBWC_HEADER_420_C_SIZE(w, h) \
	(SBWC_HEADER_420_C_SIZE(w, h) + (MFC_SBWC_HEADER_PAD / 2))

#define MFC_SBWC_Y_SIZE(w, h, dep) \
	MFC_SBWC_PAYLOAD_Y_SIZE(w, h, dep) + \
	MFC_SBWC_HEADER_Y_SIZE(w, h)
#define MFC_SBWC_420_C_SIZE(w, h, dep) \
	MFC_SBWC_PAYLOAD_420_C_SIZE(w, h, dep) + \
	MFC_SBWC_HEADER_420_C_SIZE(w, h)

#define MFC_SBWC_CBCR_BASE(base, w, h, dep) \
	((base) + MFC_SBWC_Y_SIZE(w, h, dep))

size_t g2d_get_payload_index(struct g2d_reg cmd[], const struct g2d_fmt *fmt,
			     unsigned int idx, unsigned int buffer_count,
			     unsigned long caps, u32 flags, bool dst)
{
	bool yuv82 = IS_YUV_82(fmt->fmtvalue,
			       caps & G2D_DEVICE_CAPS_YUV_BITDEPTH);
	u32 w = cmd[G2DSFR_IMG_WIDTH].value;
	u32 h = cmd[G2DSFR_IMG_BOTTOM].value;
	unsigned int colormode = cmd[G2DSFR_IMG_COLORMODE].value;

	BUG_ON(!IS_YUV(cmd[G2DSFR_IMG_COLORMODE].value));

	if (IS_SBWC(colormode)) {
		unsigned int dep = IS_YUV_P10(colormode,
			       caps & G2D_DEVICE_CAPS_YUV_BITDEPTH) ? 10: 8;
		size_t payload = 0;
		u32 sbwc = (dst) ? cmd[G2DSFR_DST_SBWCINFO].value :
			cmd[G2DSFR_SRC_SBWCINFO].value;
		unsigned int size = SBWCL_BLOCK_SIZE(sbwc);

		if (IS_SBWCL(sbwc)) {
			if (idx == 0)
				payload = SBWCL_Y_SIZE(w, h, size);
			else if (IS_YUV420(colormode))
				payload = SBWCL_420_C_SIZE(w, h, size);
			else
				payload = SBWCL_422_C_SIZE(w, h, size);

			return payload;
		}
		if (IS_YUV420(colormode)) {
			/* YUV420 with SBWC must observe MFC restriction */
			if (idx == 0)
				payload = MFC_SBWC_Y_SIZE(w, h, dep);
			else
				payload = MFC_SBWC_420_C_SIZE(w, h, dep);
		} else if (IS_YUV422_2P(colormode)) {
			if (idx == 0)
				payload = SBWC_Y_SIZE(w, h, dep);
			else
				payload = SBWC_422_C_SIZE(w, h, dep);
		}
		return payload;
	}

	if (yuv82 && (buffer_count == 2)) {
		/* YCbCr420 8+2 semi-planar in two buffers */
		/* regard G2D_LAYERFLAG_MFC_STRIDE is set */

		return (idx == 0) ? NV12_82_MFC_Y_PAYLOAD(w, h)
				  : NV12_82_MFC_C_PAYLOAD(w, h);
	}

	return ((w * fmt->bpp[idx]) / 8) * h;
}

size_t g2d_get_payload(struct g2d_reg cmd[], const struct g2d_fmt *fmt,
		       u32 flags, unsigned long cap, bool dst)
{
	size_t payload = 0;
	u32 mode = cmd[G2DSFR_IMG_COLORMODE].value;
	u32 w = cmd[G2DSFR_IMG_WIDTH].value;
	u32 h = cmd[G2DSFR_IMG_BOTTOM].value;
	size_t pixcount = w * h;
	bool yuv82 = IS_YUV_82(mode, cap & G2D_DEVICE_CAPS_YUV_BITDEPTH);
	bool mfc_stride = flags & G2D_LAYERFLAG_MFC_STRIDE;

	if (IS_SBWC(mode)) {
		unsigned int dep = IS_YUV_P10(mode,
				cap & G2D_DEVICE_CAPS_YUV_BITDEPTH) ? 10 : 8;
		u32 sbwc = (dst) ? cmd[G2DSFR_DST_SBWCINFO].value :
			cmd[G2DSFR_SRC_SBWCINFO].value;
		unsigned int size = SBWCL_BLOCK_SIZE(sbwc);

		if (IS_SBWCL(sbwc)) {
			payload = SBWCL_Y_SIZE(w, h, size);
			if (IS_YUV420(mode))
				payload += SBWCL_420_C_SIZE(w, h, size);
			else
				payload += SBWCL_422_C_SIZE(w, h, size);
		} else {
			/* YUV420 with SBWC must observe MFC restriction */
			if (IS_YUV420(mode))
				payload = MFC_SBWC_Y_SIZE(w, h, dep) +
					MFC_SBWC_420_C_SIZE(w, h, dep);
			else if (IS_YUV422_2P(mode))
				payload = SBWC_Y_SIZE(w, h, dep) +
					SBWC_422_C_SIZE(w, h, dep);
		}
	} else if (yuv82) {
		if (!mfc_stride) {
			/*
			 * constraints of base addresses of NV12/21 8+2
			 * 32 byte aligned: 8bit of Y and CbCr
			 * 16 byte aligned: 2bit of Y and CbCr
			 */
			payload += ALIGN((pixcount * fmt->bpp[0]) / 8, 16);
			payload += ALIGN((pixcount * fmt->bpp[1]) / 8, 32);
			payload += ALIGN((pixcount * fmt->bpp[2]) / 8, 16);
			payload += (pixcount * fmt->bpp[3]) / 8;
		} else {
			payload += NV12_82_MFC_PAYLOAD(w, h);
		}
	} else if (IS_YUV(mode)) {
		if (mfc_stride && IS_YUV420(mode)) {
			payload += NV12_MFC_PAYLOAD(w, h);
		} else {
			unsigned int i;

			for (i = 0; i < fmt->num_planes; i++)
				payload += (pixcount * fmt->bpp[i]) / 8;
		}
	} else if (IS_AFBC(mode)) {
		payload = AFBC_HEADER_SIZE(cmd) + (pixcount * fmt->bpp[0]) / 8;
	} else {
		payload = cmd[G2DSFR_IMG_STRIDE].value *
				cmd[G2DSFR_IMG_BOTTOM].value;
	}

	return payload;
}

static bool check_width_height(u32 value)
{
	return (value > 0) && (value <= G2D_MAX_SIZE);
}

/* 8bpp(grayscale) format is not supported */
static bool check_srccolor_mode(u32 value)
{
	u32 fmt = ((value) & G2D_DATAFMT_MASK) >> G2D_DATAFMT_SHIFT;

	if ((fmt > 14) || (fmt == 6) || (fmt == 7) || (fmt == 9))
		return false;

	if (IS_YUV(value) && (value & G2D_DATAFORMAT_AFBC))
		return false;

	if (IS_SBWC(value) && !(IS_YUV420(value) || IS_YUV422_2P(value)))
		return false;

	return true;
}

static bool check_dstcolor_mode(u32 value)
{
	u32 fmt = ((value) & G2D_DATAFMT_MASK) >> G2D_DATAFMT_SHIFT;
	u32 mode = value & (G2D_DATAFORMAT_AFBC | G2D_DATAFORMAT_UORDER |
			    G2D_DATAFORMAT_SBWC);

	/* src + YCbCr420 3p, - YCbCr420 2p 8.2 */
	if ((fmt > 14) || (fmt == 13) || (fmt == 6) || (fmt == 7))
		return false;

	/* SBWC, AFBC and UORDER should not be set together */
	if (mode & (mode - 1))
		return false;

	if (IS_SBWC(mode) && !(IS_YUV420(value) || IS_YUV422_2P(value)))
		return false;

	mode &= ~G2D_DATAFORMAT_SBWC;
	if (mode && IS_YUV(value))
		return false;

	return true;
}

static bool check_blend_mode(u32 value)
{
	int i = 0;

	for (i = 0; i < 2; i++) { /* for each source and destination */
		if ((value & 0xF) > 7) /* Coeff */
			return false;
		value >>= 4;
		if ((value & 0x3) > 2) /* CoeffSA */
			return false;
		value >>= 2;
		if ((value & 0x3) > 2) /* CoeffDA */
			return false;
		value >>= 2;
	}

	return true;
}

static bool check_scale_control(u32 value)
{
	return value != 3;
}

struct command_checker {
	const char *cmdname;
	u32 offset;
	u32 mask;
	bool (*checker)(u32 value);
};

static struct command_checker source_command_checker[G2DSFR_SRC_FIELD_COUNT] = {
	{"STRIDE",	0x0020, 0x0001FFFF, NULL,},
	{"COLORMODE",	0x0028, 0x335FFFFF, check_srccolor_mode,},
	{"LEFT",	0x002C, 0x00001FFF, NULL,},
	{"TOP",		0x0030, 0x00001FFF, NULL,},
	{"RIGHT",	0x0034, 0x00003FFF, check_width_height,},
	{"BOTTOM",	0x0038, 0x00003FFF, check_width_height,},
	{"WIDTH",	0x0070, 0x00003FFF, check_width_height,},
	{"HEIGHT",	0x0074, 0x00003FFF, check_width_height,},
	{"COMMAND",	0x0000, 0x03100003, NULL,},
	{"SELECT",	0x0004, 0x00000001, NULL,},
	{"ROTATE",	0x0008, 0x00000031, NULL,},
	{"DSTLEFT",	0x000C, 0x00001FFF, NULL,},
	{"DSTTOP",	0x0010, 0x00001FFF, NULL,},
	{"DSTRIGHT",	0x0014, 0x00003FFF, check_width_height,},
	{"DSTBOTTOM",	0x0018, 0x00003FFF, check_width_height,},
	{"SCALECONTROL", 0x048, 0x00000003, check_scale_control,},
	{"XSCALE",	0x004C, 0x3FFFFFFF, NULL,},
	{"YSCALE",	0x0050, 0x3FFFFFFF, NULL,},
	{"XPHASE",	0x0054, 0x0000FFFF, NULL,},
	{"YPHASE",	0x0058, 0x0000FFFF, NULL,},
	{"COLOR",	0x005C, 0xFFFFFFFF, NULL,},
	{"ALPHA",	0x0060, 0xFFFFFFFF, NULL,},
	{"BLEND",	0x003C, 0x0035FFFF, check_blend_mode,},
	{"YCBCRMODE",	0x0088, 0x10000017, NULL,},
	{"HDRMODE",	0x0090, 0x000011B3, NULL,},
	{"YHEADERSTRIDE",  0x00A0, 0x00003FFF, NULL,},
	{"YPAYLOADSTRIDE", 0x00A4, 0x0000FFFF, NULL,},
	{"CHEADERSTRIDE",  0x00A8, 0x00003FFF, NULL,},
	{"CPAYLOADSTRIDE", 0x00AC, 0x0000FFFF, NULL,},
	{"SBWCINFO",	0x00B0, 0x000000F1, NULL,},
};

static struct command_checker target_command_checker[G2DSFR_DST_FIELD_COUNT] = {
	/* BASE OFFSET: 0x0120 */
	{"STRIDE",	0x0004, 0x0001FFFF, NULL,},
	{"COLORMODE",	0x000C, 0x337FFFFF, check_dstcolor_mode,},
	{"LEFT",	0x0010, 0x00001FFF, NULL,},
	{"TOP",		0x0014, 0x00001FFF, NULL,},
	{"RIGHT",	0x0018, 0x00003FFF, check_width_height,},
	{"BOTTOM",	0x001C, 0x00003FFF, check_width_height,},
	{"WIDTH",	0x0040, 0x00003FFF, check_width_height,},
	{"HEIGHT",	0x0044, 0x00003FFF, check_width_height,},
	/* TODO: check csc */
	{"YCBCRMODE",	0x0058, 0x0000F714, NULL,},
	{"YHEADERSTRIDE",  0x0070, 0x00003FFF, NULL,},
	{"YPAYLOADSTRIDE", 0x0074, 0x0000FFFF, NULL,},
	{"CHEADERSTRIDE",  0x0078, 0x00003FFF, NULL,},
	{"CPAYLOADSTRIDE", 0x007C, 0x0000FFFF, NULL,},
	{"SBWCINFO",	0x0090, 0x000000F1, NULL,},
};

#define TARGET_OFFSET		0x120
#define LAYER_OFFSET(idx)	((2 + (idx)) << 8)

static int g2d_copy_commands(struct g2d_device *g2d_dev, int index,
			      struct g2d_reg regs[], __u32 cmd[],
			      struct command_checker checker[],
			      unsigned int num_cmds)
{
	unsigned int base = (index < 0) ? TARGET_OFFSET : LAYER_OFFSET(index);
	int i;

	for (i = 0; i < num_cmds; i++) {
		if (((cmd[i] & ~checker[i].mask) != 0) ||
				(checker[i].checker &&
					!checker[i].checker(cmd[i]))) {
			perrfndev(g2d_dev, "Invalid %s[%d] SFR '%s' value %#x",
				  (index < 0) ? "target" : "source",
				  index, checker[i].cmdname, cmd[i]);
			perrdev(g2d_dev, "mask %#x", checker[i].mask);
			return -EINVAL;
		}

		regs[i].offset = base + checker[i].offset;
		regs[i].value = cmd[i];
	}

	return num_cmds;
}

static bool g2d_validate_image_dimension(struct g2d_device *g2d_dev,
					 u32 width, u32 height,
					 u32 left, u32 top,
					 u32 right, u32 bottom)
{
	if ((left >= right) || (top >= bottom) ||
			(width < (right - left)) || (height < (bottom - top))) {
		perrfndev(g2d_dev, "Invalid dimension [%ux%u, %ux%u) / %ux%u",
			  left, top, right, bottom, width, height);
		return false;
	}

	return true;
}

#define IS_EVEN(value) (((value) & 1) == 0)

static bool g2d_validate_image_format(struct g2d_device *g2d_dev,
		struct g2d_task *task, struct g2d_reg commands[], bool dst)
{
	bool yuvbitdepth = !!(g2d_dev->caps & G2D_DEVICE_CAPS_YUV_BITDEPTH);
	u32 stride = commands[G2DSFR_IMG_STRIDE].value;
	u32 mode   = commands[G2DSFR_IMG_COLORMODE].value;
	u32 width  = commands[G2DSFR_IMG_WIDTH].value;
	u32 height = commands[G2DSFR_IMG_HEIGHT].value;
	u32 left   = commands[G2DSFR_IMG_LEFT].value;
	u32 right  = commands[G2DSFR_IMG_RIGHT].value;
	u32 top    = commands[G2DSFR_IMG_TOP].value;
	u32 bottom = commands[G2DSFR_IMG_BOTTOM].value;
	u32 Bpp = 0;
	const struct g2d_fmt *fmt;

	if (IS_AFBC(mode) && !dst) {
		width++;
		height++;
	}

	if (!g2d_validate_image_dimension(g2d_dev, width, height,
					left, top, right, bottom))
		return false;

	fmt = g2d_find_format(mode, g2d_dev->caps);
	if (fmt == NULL) {
		perrfndev(g2d_dev, "Color mode %#x is not supported", mode);
		return false;
	}

	Bpp = fmt->bpp[0] / 8;

	if (stride) {
		int err = 0;

		if (IS_SBWC(mode) || IS_AFBC(mode) || IS_YUV(mode))
			err |= 1 << 1;
		if (IS_UORDER(mode) & (stride != ALIGN(width * Bpp, 16)))
			err |= 1 << 2;
		if (stride < (width * Bpp))
			err |= 1 << 4;
		if (stride > (G2D_MAX_SIZE * Bpp))
			err |= 1 << 5;

		if (err) {
			perrfndev(g2d_dev,
				  "wrong(%#x) stride %u mode %#x size %ux%u",
				  err, stride, mode, width, height);
			return false;
		}
	} else if (IS_YUV(mode)) {
		/* TODO: Y8 handling if required */
		if (!IS_EVEN(width) || (!IS_YUV422(mode) && !IS_EVEN(height)))
			goto err_align;
	} else if (!IS_AFBC(mode)) {
		perrfndev(g2d_dev, "Non AFBC RGB requires valid stride");
		return false;
	}

	if (IS_HWFC(task->flags)) {
		if ((dst && IS_AFBC(mode)) || IS_UORDER(mode)) {
			perrfndev(g2d_dev, "Invalid HWFC format with %s",
				  IS_AFBC(mode) ? "AFBC" : "UORDER");
			return false;
		}
		if (dst &&
			((width != right - left) || (height != bottom - top)))
			goto err_align;
	}

	if (IS_SBWC(mode)) {
		u32 sbwc = (dst) ? commands[G2DSFR_DST_SBWCINFO].value :
			commands[G2DSFR_SRC_SBWCINFO].value;
		int dep = IS_YUV_P10(mode, g2d_dev->caps &
			G2D_DEVICE_CAPS_YUV_BITDEPTH) ? 10 : 8;
		u32 offset = dst ? G2DSFR_DST_Y_HEADER_STRIDE :
			G2DSFR_SRC_Y_HEADER_STRIDE;
		u32 header_stride;
		u32 payload_stride;
		unsigned int size = SBWCL_BLOCK_SIZE(sbwc);
		int i;

		if (IS_SBWCL(sbwc)) {
			header_stride = 0;
			payload_stride = SBWCL_STRIDE(width, size);
		} else {
			header_stride = SBWC_HEADER_STRIDE(width);
			payload_stride = SBWC_PAYLOAD_STRIDE(width, dep);
		}

		/* Stride is in the order : header, payload, header, payload */
		for (i = 0; i < 4; i += 2) {
			if ((commands[offset + i].value != header_stride) ||
			    (commands[offset + i + 1].value !=
			     payload_stride)) {
				perrfndev(g2d_dev,
					  "Bad stride %u, %u for w %u mode %x",
					  header_stride, payload_stride,
					  width, mode);
				return false;
			}
		}

		if (IS_SBWCL(sbwc)) {
			// 96 lossy super block is currently not supported.
			if (((dep == 8) && (size == 128)) || (size == 96)) {
				perrfndev(g2d_dev,
					  "Bad SBWC Lossy (mode %x, info %x)",
					  mode, sbwc);
				return false;
			}

			if (!(g2d_dev->caps &
				G2D_DEVICE_CAPS_COMPRESSED_LOSSY_YUV)) {
				perrfndev(g2d_dev,
					  "SBWC Lossy is not supported");
				return false;
			}
		}

		if (!(g2d_dev->caps & G2D_DEVICE_CAPS_COMPRESSED_YUV)) {
			perrfndev(g2d_dev, "SBWC format is not supported");
			return false;
		}

		if (!IS_SBWC_WIDTH_ALIGNED(width) ||
			!IS_SBWC_HEIGHT_ALIGNED(height)) {
			goto err_align;
		}
		if (dst &&
			((width != right - left) || (height != bottom - top)))
			goto err_align;
	}

	if (!dst) {
		if (IS_AFBC(mode) && (!IS_AFBC_WIDTH_ALIGNED(width) ||
					!IS_AFBC_HEIGHT_ALIGNED(height)))
			goto err_align;

		if (IS_YUV_82(mode, yuvbitdepth) && !IS_ALIGNED(width, 64))
			goto err_align;

		return true;
	}

	width = commands[G2DSFR_IMG_LEFT].value |
				commands[G2DSFR_IMG_RIGHT].value;
	height = commands[G2DSFR_IMG_TOP].value |
					commands[G2DSFR_IMG_BOTTOM].value;

	if (IS_AFBC(mode) && !IS_AFBC_WIDTH_ALIGNED(width | height))
		goto err_align;

	if (dst && IS_SBWC(mode)) {
		if (!IS_SBWC_WIDTH_ALIGNED(width))
			goto err_align;

		if (IS_YUV420(mode) && !IS_SBWC_HEIGHT_420_ALIGNED(height))
			goto err_align;

		if (IS_YUV422_2P(mode) && !IS_SBWC_HEIGHT_ALIGNED(height))
			goto err_align;
	}

	if (IS_YUV(mode)) {
		/*
		 * DST clip region has the alignment restrictions
		 * accroding to the chroma subsampling
		 */
		if (!IS_EVEN(width) || (!IS_YUV422(mode) && !IS_EVEN(height)))
			goto err_align;
	}

	return true;
err_align:
	perrfndev(g2d_dev,
		  "Unaligned size %ux%u or crop [%ux%u, %ux%u) for %s %s %s",
		  commands[G2DSFR_IMG_WIDTH].value,
		  commands[G2DSFR_IMG_HEIGHT].value,
		  commands[G2DSFR_IMG_LEFT].value,
		  commands[G2DSFR_IMG_TOP].value,
		  commands[G2DSFR_IMG_RIGHT].value,
		  commands[G2DSFR_IMG_BOTTOM].value,
		  IS_AFBC(mode) ? "AFBC" :
			  IS_YUV422(mode) ? "YUV422" : "YUV20",
		  IS_YUV_82(mode, yuvbitdepth) ? "8+2" : "",
		  IS_HWFC(task->flags) ? "HWFC" : IS_SBWC(mode) ? "SBWC" : "");

	return false;
}

bool g2d_validate_source_commands(struct g2d_device *g2d_dev,
				  struct g2d_task *task,
				  unsigned int i, struct g2d_layer *source,
				  struct g2d_layer *target)
{
	u32 colormode = source->commands[G2DSFR_IMG_COLORMODE].value;
	u32 width, height;

	if (!g2d_validate_image_format(
			g2d_dev, task, source->commands, false)) {
		perrfndev(g2d_dev, "Failed to validate source[%d] commands", i);
		return false;
	}

	if (((source->flags & G2D_LAYERFLAG_COLORFILL) != 0) &&
							!IS_RGB(colormode)) {
		perrfndev(g2d_dev,
			  "Image type should be RGB for Solid color layer:");
		perrdev(g2d_dev, "\tindex: %d, flags %#x, colormode: %#010x",
			i, source->flags, colormode);
		return false;
	}

	width = target->commands[G2DSFR_IMG_WIDTH].value;
	height = target->commands[G2DSFR_IMG_HEIGHT].value;

	if (IS_AFBC(colormode)) {
		width++;
		height++;
	}

	if (!g2d_validate_image_dimension(g2d_dev, width, height,
				source->commands[G2DSFR_SRC_DSTLEFT].value,
				source->commands[G2DSFR_SRC_DSTTOP].value,
				source->commands[G2DSFR_SRC_DSTRIGHT].value,
				source->commands[G2DSFR_SRC_DSTBOTTOM].value)) {
		perrfndev(g2d_dev, "Window of source[%d] floods the target", i);
		return false;
	}

	return true;
}

bool g2d_validate_target_commands(struct g2d_device *g2d_dev,
				  struct g2d_task *task)
{
	if (!g2d_validate_image_format(g2d_dev, task,
			task->target.commands, true)) {
		perrfndev(g2d_dev, "Failed to validate target commands");
		return false;
	}

	return true;
}

/*
 * List of extra command
 *
 * {0x2000, 0x208C}, SRC CSC Coefficients
 * {0x2100, 0x2120}, DST CSC Coefficients
 * {0x3000, 0x3100}, HDR EOTF Coefficients
 * {0x3200, 0x3300}, Degamma Coefficients
 * {0x3400, 0x3420}, HDR Gamut Mapping Coefficients
 * {0x3500, 0x3520}, Degamma 2.2 Coefficients
 * {0x3600, 0x3680}, HDR Tone Mapping Coefficients
 * {0x3700, 0x3780}, Degamma Tone Mapping Coefficients
 * {0x5000, 0x5C40}, SET 0,1,2 Coefficients
 */
static u16 extra_valid_range[2] = {0x2000, 0x8000}; // {0x2000, 0x8000]

static bool g2d_validate_extra_command(struct g2d_device *g2d_dev,
				       struct g2d_reg extra[],
				       unsigned int num_regs)
{
	unsigned int n;

	for (n = 0; n < num_regs; n++) {
		if ((extra[n].offset < extra_valid_range[0]) ||
			(extra[n].offset >= extra_valid_range[1])) {
			perrfndev(g2d_dev, "wrong offset %#x @ extra cmd[%d]",
				  extra[n].offset, n);
			return false;
		}
	}

	return true;
}

#define G2D_MAX_IMAGE_COMMAND	\
	((G2D_MAX_IMAGES * G2DSFR_SRC_FIELD_COUNT) + G2DSFR_DST_FIELD_COUNT)
/*
 * Maximum of number of register set by driver.
 *
 * 3 initial commands
 * 17 sets of 4 address of source and destinaion.
 * 2 of Task control register
 * 1 of HW flow control register
 * 1 of Secure layer register
 */
#define G2D_TASK_COMMAND	(3 + (4 * (G2D_MAX_IMAGES + 1)) + 2 + 1 + 1)

// Sum of taskctl, layer, extra must not exceed G2D_MAX_COMMAND
#define G2D_MAX_EXTRA_COMMAND \
	(G2D_MAX_COMMAND - G2D_MAX_IMAGE_COMMAND - G2D_TASK_COMMAND)

int g2d_import_commands(struct g2d_device *g2d_dev, struct g2d_task *task,
			struct g2d_task_data *data, unsigned int num_sources)
{
	struct g2d_reg *cmdaddr = page_address(task->cmd_page);
	struct g2d_commands *cmds = &data->commands;
	u32 tm_tuned_lut[NR_TM_LUT_VALUES];
	u32 tgtcmds[G2DSFR_DST_FIELD_COUNT];
	unsigned int i;
	int copied;

	if (cmds->num_extra_regs > G2D_MAX_EXTRA_COMMAND) {
		perrfndev(g2d_dev, "Too many coefficient reigsters %d",
			  cmds->num_extra_regs);
		return -EINVAL;
	}

	cmdaddr += task->sec.cmd_count;

	if (copy_from_user(tgtcmds, cmds->target, sizeof(tgtcmds))) {
		perrfndev(g2d_dev, "Failed to get target commands");
		return -EFAULT;
	}
	copied = g2d_copy_commands(g2d_dev, -1, cmdaddr, tgtcmds,
				target_command_checker, G2DSFR_DST_FIELD_COUNT);
	if (copied < 0)
		return -EINVAL;

	task->target.commands = cmdaddr;

	cmdaddr += copied;
	task->sec.cmd_count += copied;

	for (i = 0; i < num_sources; i++) {
		u32 srccmds[G2DSFR_SRC_FIELD_COUNT];

		if (copy_from_user(srccmds, cmds->source[i], sizeof(srccmds))) {
			perrfndev(g2d_dev, "Failed to get src[%d] commands", i);
			return -EFAULT;
		}

		copied = g2d_copy_commands(g2d_dev, i, cmdaddr, srccmds,
				source_command_checker, G2DSFR_SRC_FIELD_COUNT);
		if (copied < 0)
			return -EINVAL;

		task->source[i].commands = cmdaddr;
		cmdaddr += copied;
		task->sec.cmd_count += copied;
	}

	if (copy_from_user(cmdaddr, cmds->extra,
			   cmds->num_extra_regs * sizeof(struct g2d_reg))) {
		perrfndev(g2d_dev, "Failed to get %u extra commands",
			  cmds->num_extra_regs);
		return -EFAULT;
	}

	if (!g2d_validate_extra_command(g2d_dev, cmdaddr, cmds->num_extra_regs))
		return -EINVAL;

	task->sec.cmd_count += cmds->num_extra_regs;

	/* overwrite if TM LUT values are specified: consumes 66 entries  */
	if (exynos_hdr_get_tm_lut(tm_tuned_lut)) {
		u32 base;
		/* offsets of TM LUT values: 0x3600, 0x3700 */
		cmdaddr += cmds->num_extra_regs;
		for (base = 0x3600; base < 0x3800; base += 0x100) {
			for (i = 0; i < NR_TM_LUT_VALUES; i++) {
				cmdaddr->offset =
					(unsigned long)(base + i * sizeof(u32));
				cmdaddr->value = tm_tuned_lut[i];
				cmdaddr++;
				task->sec.cmd_count++;
			}
		}
	}

	return 0;
}

static unsigned int g2d_set_image_buffer(struct g2d_task *task,
					 struct g2d_layer *layer, u32 colormode,
					 unsigned char offsets[], u32 base)
{
	const struct g2d_fmt *fmt = g2d_find_format(colormode,
						    task->g2d_dev->caps);
	struct g2d_reg *reg = (struct g2d_reg *)page_address(task->cmd_page);
	unsigned int cmd_count = task->sec.cmd_count;
	u32 width = layer_width(layer);
	u32 height = layer_height(layer);
	unsigned int i;

	if (fmt->num_planes == 4) {
		unsigned int nbufs = min_t(unsigned int,
					   layer->num_buffers, fmt->num_planes);

		for (i = 0; i < nbufs; i++) {
			if (!YUV82_BASE_ALIGNED(layer->buffer[i].dma_addr, i)) {
				perrfndev(task->g2d_dev,
					  "addr[%d] not aligned for YUV8+2", i);
				return 0;
			}
		}
	} else if (!IS_ALIGNED(layer->buffer[0].dma_addr, 4)) {
		perrfndev(task->g2d_dev, "Plane 0 address isn't aligned by 4.");
		return 0;
	}

	for (i = 0; i < layer->num_buffers; i++) {
		reg[cmd_count].offset = BASE_REG_OFFSET(base, offsets, i);
		reg[cmd_count].value = layer->buffer[i].dma_addr;
		cmd_count++;
	}

	if (layer->num_buffers == fmt->num_planes)
		return cmd_count;

	/* address of plane 0 is set in the above for() */

	if (fmt->num_planes == 2) {
		/* YCbCr semi-planar in a single buffer */
		reg[cmd_count].offset = BASE_REG_OFFSET(base, offsets, 1);
		if (!!(layer->flags & G2D_LAYERFLAG_MFC_STRIDE) &&
						IS_YUV420(fmt->fmtvalue)) {
			reg[cmd_count].value =
				NV12_MFC_CBASE(layer->buffer[0].dma_addr,
					       width, height);
		} else {
			reg[cmd_count].value = layer_pixelcount(layer);
			reg[cmd_count].value *= fmt->bpp[0] / 8;
			reg[cmd_count].value += layer->buffer[0].dma_addr;
			reg[cmd_count].value = ALIGN(reg[cmd_count].value, 2);
		}
		cmd_count++;
		return cmd_count;
	}

	BUG_ON(fmt->num_planes != 4);

	if ((layer->num_buffers == 1) &&
			!(layer->flags & G2D_LAYERFLAG_MFC_STRIDE)) {
		dma_addr_t addr = layer->buffer[0].dma_addr;
		/* YCbCr semi-planar 8+2 in a single buffer */
		for (i = 1; i < 4; i++) {
			addr += (layer_pixelcount(layer) * fmt->bpp[i - 1]) / 8;
			addr = YUV82_BASE_ALIGN(addr, i);
			reg[cmd_count].value = addr;
			reg[cmd_count].offset =
					BASE_REG_OFFSET(base, offsets, i);
			cmd_count++;
		}

		return cmd_count;
	}

	/* G2D_LAYERFLAG_MFC_STRIDE is set */
	reg[cmd_count].offset = BASE_REG_OFFSET(base, offsets, 1);
	reg[cmd_count].value = layer->buffer[0].dma_addr +
			       NV12_MFC_Y_PAYLOAD_PAD(width, height);
	cmd_count++;

	reg[cmd_count].offset = BASE_REG_OFFSET(base, offsets, 2);
	if (layer->num_buffers == 2)
		reg[cmd_count].value = layer->buffer[1].dma_addr;
	else
		reg[cmd_count].value = reg[cmd_count - 1].value +
				       NV12_82_MFC_2Y_PAYLOAD(width, height) +
				       MFC_2B_PAD_SIZE;
	cmd_count++;

	reg[cmd_count].offset = BASE_REG_OFFSET(base, offsets, 3);
	reg[cmd_count].value = reg[cmd_count - 1].value +
			       NV12_MFC_C_PAYLOAD_PAD(width, height);
	cmd_count++;

	return cmd_count;
}

static unsigned int g2d_set_sbwc_buffer(struct g2d_task *task,
					struct g2d_layer *layer, u32 colormode,
					u32 base)
{
	struct g2d_reg *reg = (struct g2d_reg *)page_address(task->cmd_page);
	unsigned char *offsets = (base == TARGET_OFFSET) ?
				dst_sbwc_reg_offset : src_sbwc_reg_offset;
	u32 w = layer_width(layer);
	u32 h = layer_height(layer);
	u32 align = (base == TARGET_OFFSET) ? 64 : 32;
	unsigned int cmd_count = task->sec.cmd_count;
	unsigned int i;
	unsigned int dep;
	unsigned int sbwc = (base == TARGET_OFFSET) ?
		layer->commands[G2DSFR_DST_SBWCINFO].value :
		layer->commands[G2DSFR_SRC_SBWCINFO].value;
	unsigned int base_addr = layer->buffer[0].dma_addr;
	unsigned int size;

	dep = IS_YUV_P10(colormode, task->g2d_dev->caps &
			G2D_DEVICE_CAPS_YUV_BITDEPTH) ? 10 : 8;

	if (!IS_ALIGNED(base_addr, align)) {
		perrfndev(task->g2d_dev, "SBWC base %#llx is not aligned %u",
			  base_addr, align);
		return 0;
	}

	for (i = 0; i < layer->num_buffers; i++) {
		reg[cmd_count].offset = BASE_REG_OFFSET(base, offsets, i);
		reg[cmd_count].value = layer->buffer[i].dma_addr;
		cmd_count++;
	}

	if (layer->num_buffers == 1) {
		reg[cmd_count].offset = BASE_REG_OFFSET(base, offsets, 1);
		if (IS_SBWCL(sbwc))
			reg[cmd_count].value =
				SBWCL_CBCR_BASE(base_addr, w, h,
						SBWCL_BLOCK_SIZE(sbwc));
		else if (IS_YUV420(colormode))
			reg[cmd_count].value =
				MFC_SBWC_CBCR_BASE(base_addr, w, h, dep);
		else
			reg[cmd_count].value =
				SBWC_CBCR_BASE(base_addr, w, h, dep);
		cmd_count++;
	}

	/* The header is placed after the payload */
	reg[cmd_count].offset = BASE_REG_OFFSET(base, offsets, 3);
	if (IS_SBWCL(sbwc))
		size = 0;
	else if (IS_YUV420(colormode))
		size = MFC_SBWC_PAYLOAD_420_C_SIZE(w, h, dep);
	else
		size = SBWC_PAYLOAD_422_C_SIZE(w, h, dep);

	reg[cmd_count].value = reg[cmd_count - 1].value + size;
	cmd_count++;

	reg[cmd_count].offset = BASE_REG_OFFSET(base, offsets, 2);
	if (IS_SBWCL(sbwc))
		size = 0;
	else if (IS_YUV420(colormode))
		size = MFC_SBWC_PAYLOAD_Y_SIZE(w, h, dep);
	else
		size = SBWC_PAYLOAD_Y_SIZE(w, h, dep);

	reg[cmd_count].value = base_addr + size;
	cmd_count++;

	return cmd_count;
}

static unsigned int g2d_set_afbc_buffer(struct g2d_task *task,
					struct g2d_layer *layer,
					u32 base_offset)
{
	struct g2d_reg *reg = (struct g2d_reg *)page_address(task->cmd_page);
	u32 align = (base_offset == TARGET_OFFSET) ? 64 : 16;
	unsigned char *reg_offset = (base_offset == TARGET_OFFSET) ?
				dst_base_reg_offset : src_base_reg_offset;

	if (!IS_ALIGNED(layer->buffer[0].dma_addr, align)) {
		perrfndev(task->g2d_dev, "AFBC base %#llx is not aligned by %u",
			  layer->buffer[0].dma_addr, align);
		return 0;
	}

	reg[task->sec.cmd_count].offset = base_offset + reg_offset[2];
	reg[task->sec.cmd_count].value = layer->buffer[0].dma_addr;
	reg[task->sec.cmd_count + 1].offset = base_offset + reg_offset[3];
	if (base_offset == TARGET_OFFSET)
		reg[task->sec.cmd_count + 1].value =
			ALIGN(AFBC_HEADER_SIZE(layer->commands)
					+ layer->buffer[0].dma_addr, align);
	else
		reg[task->sec.cmd_count + 1].value = layer->buffer[0].dma_addr;

	return task->sec.cmd_count + 2;
}

bool g2d_prepare_source(struct g2d_task *task,
			struct g2d_layer *layer, int index)
{
	struct g2d_reg *reg = (struct g2d_reg *)page_address(task->cmd_page);
	u32 colormode = layer->commands[G2DSFR_IMG_COLORMODE].value;
	bool yuv82;
	unsigned char *offsets;

	yuv82 = IS_YUV_82(colormode,
			  task->g2d_dev->caps & G2D_DEVICE_CAPS_YUV_BITDEPTH);
	offsets = yuv82 ? src_base_reg_offset_yuv82 : src_base_reg_offset;

	reg[TASK_REG_LAYER_UPDATE].value |= 1 << index;

	if ((layer->flags & G2D_LAYERFLAG_COLORFILL) != 0)
		return true;

	if (IS_AFBC(colormode))
		task->sec.cmd_count =
			g2d_set_afbc_buffer(task, layer, LAYER_OFFSET(index));
	else if (IS_SBWC(colormode))
		task->sec.cmd_count =
			g2d_set_sbwc_buffer(task, layer, colormode,
					    LAYER_OFFSET(index));
	else
		task->sec.cmd_count =
			g2d_set_image_buffer(task, layer, colormode,
					     offsets, LAYER_OFFSET(index));
	/*
	 * It is alright to set task->cmd_count to 0
	 * because this task is to be discarded.
	 */
	return task->sec.cmd_count != 0;
}

bool g2d_prepare_target(struct g2d_task *task)
{
	u32 colormode = task->target.commands[G2DSFR_IMG_COLORMODE].value;

	if (IS_AFBC(colormode))
		task->sec.cmd_count =
			g2d_set_afbc_buffer(task, &task->target,
					    TARGET_OFFSET);
	else if (IS_SBWC(colormode))
		task->sec.cmd_count =
			g2d_set_sbwc_buffer(task, &task->target,
					     colormode, TARGET_OFFSET);
	else
		task->sec.cmd_count =
			g2d_set_image_buffer(task, &task->target, colormode,
					dst_base_reg_offset, TARGET_OFFSET);

	return task->sec.cmd_count != 0;
}
