/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (c) 2018, The Linux Foundation. All rights reserved.
 */

#ifndef _DT_BINDINGS_CLK_SDM_GPU_CC_SDM845_H
#define _DT_BINDINGS_CLK_SDM_GPU_CC_SDM845_H

/* GPU_CC clock registers */
#define GPU_CC_CRC_AHB_CLK			0
#define GPU_CC_CX_APB_CLK			1
#define GPU_CC_CX_GFX3D_CLK			2
#define GPU_CC_CX_GFX3D_SLV_CLK			3
#define GPU_CC_CX_GMU_CLK			4
#define GPU_CC_CX_SNOC_DVM_CLK			5
#define GPU_CC_CXO_CLK				6
#define GPU_CC_GMU_CLK_SRC			7
#define GPU_CC_GX_GMU_CLK			8
#define GPU_CC_GX_GFX3D_CLK_SRC			9
#define GPU_CC_GX_GFX3D_CLK			10
#define GPU_CC_PLL0				11
#define GPU_CC_PLL0_OUT_EVEN			12
#define GPU_CC_PLL1				12

/* GPU_CC Resets */
#define GPUCC_GPU_CC_ACD_BCR			0
#define GPUCC_GPU_CC_CX_BCR			1
#define GPUCC_GPU_CC_GFX3D_AON_BCR		2
#define GPUCC_GPU_CC_GMU_BCR			3
#define GPUCC_GPU_CC_GX_BCR			4
#define GPUCC_GPU_CC_SPDM_BCR			5
#define GPUCC_GPU_CC_XO_BCR			6

/* GPU_CC GDSCRs */
#define GPU_CX_GDSC				0
#define GPU_GX_GDSC				1

#endif
