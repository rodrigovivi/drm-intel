/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GUC_ADS_TYPES_H_
#define _XE_GUC_ADS_TYPES_H_

#include <linux/types.h>

struct xe_bo;

/**
 * struct xe_guc_ads - GuC additional data structures (ADS)
 */
struct xe_guc_ads {
	/** @bo: XE BO for GuC ads blob */
	struct xe_bo *bo;
	u32 regset_size;
};

#endif
