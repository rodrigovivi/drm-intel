/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_BO_TYPES_H_
#define _XE_BO_TYPES_H_

#include <linux/iosys-map.h>

#include <drm/drm_mm.h>
#include <drm/ttm/ttm_bo_api.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_execbuf_util.h>
#include <drm/ttm/ttm_placement.h>

struct xe_device;
struct xe_vm;

#define XE_BO_MAX_PLACEMENTS	3

struct xe_bo {
	struct ttm_buffer_object ttm;

	size_t size;

	uint32_t flags;

	struct xe_vm *vm;

	struct list_head vmas;

	struct ttm_place placements[XE_BO_MAX_PLACEMENTS];
	struct ttm_placement placement;

	struct drm_mm_node ggtt_node;

	struct iosys_map vmap;

	/** @extobj_tv: used during exec to lock all external BOs */
	struct ttm_validate_buffer extobj_tv;
};

#endif	/* _XE_BO_TYPES_H_ */
