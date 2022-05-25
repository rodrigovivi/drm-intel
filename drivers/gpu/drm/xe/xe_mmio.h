/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_MMIO_H_
#define _XE_MMIO_H_

#include <linux/delay.h>

#include "xe_device.h"

/*
 * FIXME: This header has been deemed evil and we need to kill it. Temporarily
 * including so we can use 'wait_for' and unblock initial development. A follow
 * should replace 'wait_for' with a sane version and drop including this header.
 */
#include "i915_utils.h"

int xe_mmio_init(struct xe_device *xe);
void xe_mmio_finish(struct xe_device *xe);

static inline void xe_mmio_write32(struct xe_device *xe,
				   uint32_t reg, uint32_t val)
{
	writel(val, xe->mmio.regs + reg);
}

static inline uint32_t xe_mmio_read32(struct xe_device *xe, uint32_t reg)
{
	return readl(xe->mmio.regs + reg);
}

static inline uint64_t xe_mmio_read64(struct xe_device *xe, uint32_t reg)
{
	return readq(xe->mmio.regs + reg);
}

static inline int xe_mmio_write32_and_verify(struct xe_device *xe,
					     uint32_t reg, uint32_t val,
					     uint32_t mask, uint32_t eval)
{
	uint32_t reg_val;

	xe_mmio_write32(xe, reg, val);
	reg_val = xe_mmio_read32(xe, reg);

	return (reg_val & mask) != eval ? -EINVAL : 0;
}

static inline int xe_mmio_wait32(struct xe_device *xe,
				 uint32_t reg, uint32_t val,
				 uint32_t mask, uint32_t timeout_ms)
{
	return wait_for((xe_mmio_read32(xe, reg) & mask) == val,
			timeout_ms);
}

int xe_mmio_ioctl(struct drm_device *dev, void *data,
		  struct drm_file *file);

#endif /* _XE_MMIO_H_ */
