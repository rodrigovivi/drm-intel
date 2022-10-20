/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_GT_H_
#define _XE_GT_H_

#include <drm/drm_util.h>

#include "xe_device_types.h"
#include "xe_hw_engine.h"

#define for_each_hw_engine(hwe__, gt__, id__) \
	for ((id__) = 0; (id__) < ARRAY_SIZE((gt__)->hw_engines); (id__)++) \
	     for_each_if (((hwe__) = (gt__)->hw_engines + (id__)) && \
			  xe_hw_engine_is_valid((hwe__)))

int xe_gt_alloc(struct xe_device *xe, struct xe_gt *gt, u8 id);
int xe_gt_init(struct xe_gt *gt);
int xe_gt_suspend(struct xe_gt *gt);
int xe_gt_resume(struct xe_gt *gt);
void xe_gt_reset_async(struct xe_gt *gt);
void xe_gt_migrate_wait(struct xe_gt *gt);

struct xe_hw_engine *xe_gt_hw_engine(struct xe_gt *gt,
				     enum xe_engine_class class,
				     u16 instance,
				     bool logical);

static inline struct xe_device * gt_to_xe(struct xe_gt *gt)
{
	return gt->xe;
}

#endif	/* _XE_GT_H_ */
