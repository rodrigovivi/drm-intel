/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_ENGINE_H_
#define _XE_ENGINE_H_

#include "xe_engine_types.h"
#include "xe_vm_types.h"

struct drm_device;
struct drm_file;
struct xe_device;
struct xe_file;

struct xe_engine *xe_engine_create(struct xe_device *xe, struct xe_vm *vm,
				   u32 logical_mask, u16 width,
				   struct xe_hw_engine *hw_engine, u32 flags);
void xe_engine_fini(struct xe_engine *e);
void xe_engine_destroy(struct kref *ref);

struct xe_engine *xe_engine_lookup(struct xe_file *xef, u32 id);

static inline struct xe_engine *xe_engine_get(struct xe_engine *engine)
{
	kref_get(&engine->refcount);
	return engine;
}

static inline void xe_engine_put(struct xe_engine *engine)
{
	kref_put(&engine->refcount, xe_engine_destroy);
}

static inline bool xe_engine_is_parallel(struct xe_engine *engine)
{
	return engine->width > 1;
}

#define xe_engine_assert_held(e) \
	do { \
		struct xe_engine *_eng = (e); \
		if (_eng->vm) \
			xe_vm_assert_held(_eng->vm); \
	} while (0)

int xe_engine_create_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file);
int xe_engine_destroy_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file);
int xe_engine_set_property_ioctl(struct drm_device *dev, void *data,
				 struct drm_file *file);

#endif /* _XE_ENGINE_H_ */
