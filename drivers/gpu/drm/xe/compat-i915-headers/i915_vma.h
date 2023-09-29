/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2023 Intel Corporation
 */

#ifndef I915_VMA_H
#define I915_VMA_H

#include <uapi/drm/i915_drm.h>
#include <drm/drm_mm.h>

struct xe_bo;

struct i915_vma {
	struct xe_bo *bo, *dpt;
	struct drm_mm_node node;
};

#define i915_ggtt_clear_scanout(bo) do { } while (0)

#define i915_vma_fence_id(vma) -1
#endif
