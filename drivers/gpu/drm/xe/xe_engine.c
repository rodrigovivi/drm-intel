/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#include "xe_engine.h"

#include <drm/drm_device.h>
#include <drm/drm_file.h>
#include <drm/gpu_scheduler.h>
#include <drm/drm_syncobj.h>
#include <drm/xe_drm.h>

#include "xe_bo.h"
#include "xe_device.h"
#include "xe_gt.h"
#include "xe_lrc.h"
#include "xe_sched_job.h"
#include "xe_sync.h"
#include "xe_trace.h"
#include "xe_vm.h"

static struct xe_engine *__xe_engine_create(struct xe_device *xe,
					    struct xe_vm *vm,
					    u32 logical_mask,
					    u16 width, struct xe_hw_engine *hwe,
					    u32 flags)
{
	struct xe_engine *e;
	struct xe_gt *gt = to_gt(xe);
	int err;

	e = kzalloc(sizeof(*e), GFP_KERNEL);
	if (!e)
		return ERR_PTR(-ENOMEM);

	kref_init(&e->refcount);
	e->flags = flags;
	e->hwe = hwe;
	e->gt = gt;
	if (vm)
		e->vm = xe_vm_get(vm);
	e->class = hwe->class;
	e->width = width;
	e->logical_mask = logical_mask;
	e->fence_irq = &gt->fence_irq[hwe->class];
	e->ring_ops = gt->ring_ops[hwe->class];
	INIT_LIST_HEAD(&e->persitent.link);

	err = xe_lrc_init(&e->lrc, hwe, vm, SZ_16K);
	if (err)
		goto err_kfree;

	err = gt->engine_ops->init(e);
	if (err)
		goto err_lrc;

	return e;

err_lrc:
	xe_lrc_finish(&e->lrc);
err_kfree:
	kfree(e);
	return ERR_PTR(err);
}

struct xe_engine *xe_engine_create(struct xe_device *xe, struct xe_vm *vm,
				   u32 logical_mask, u16 width,
				   struct xe_hw_engine *hwe, u32 flags)
{
	struct xe_engine *e;

	if (vm)
		xe_vm_lock(vm, NULL);
	e = __xe_engine_create(xe, vm, logical_mask, width, hwe, flags);
	if (vm)
		xe_vm_unlock(vm);

	return e;
}

void xe_engine_destroy(struct kref *ref)
{
	struct xe_engine *e = container_of(ref, struct xe_engine, refcount);

	e->gt->engine_ops->fini(e);
}

void xe_engine_fini(struct xe_engine *e)
{
	xe_lrc_finish(&e->lrc);
	if (e->vm)
		xe_vm_put(e->vm);

	kfree(e);
}

struct xe_engine *xe_engine_lookup(struct xe_file *xef, u32 id)
{
	struct xe_engine *e;

	mutex_lock(&xef->engine.lock);
	e = xa_load(&xef->engine.xa, id);
	mutex_unlock(&xef->engine.lock);

	if (e)
		xe_engine_get(e);

	return e;
}

static int xe_engine_begin(struct xe_engine *e)
{
	int err;

	xe_vm_lock(e->vm, NULL);

	err = xe_bo_vmap(e->lrc.bo);
	if (err)
		goto err_unlock_vm;

	return 0;

err_unlock_vm:
	xe_vm_unlock(e->vm);
	return err;
}

static void xe_engine_end(struct xe_engine *e)
{
	xe_vm_unlock(e->vm);
}

static const enum xe_engine_class user_to_xe_engine_class[] = {
	[DRM_XE_ENGINE_CLASS_RENDER] = XE_ENGINE_CLASS_RENDER,
	[DRM_XE_ENGINE_CLASS_COPY] = XE_ENGINE_CLASS_COPY,
	[DRM_XE_ENGINE_CLASS_VIDEO_DECODE] = XE_ENGINE_CLASS_VIDEO_DECODE,
	[DRM_XE_ENGINE_CLASS_VIDEO_ENHANCE] = XE_ENGINE_CLASS_VIDEO_ENHANCE,
	[DRM_XE_ENGINE_CLASS_COMPUTE] = XE_ENGINE_CLASS_COMPUTE,
};

static struct xe_hw_engine *
find_hw_engine(struct xe_device *xe,
	       struct drm_xe_engine_class_instance eci)
{
	if (eci.engine_class > ARRAY_SIZE(user_to_xe_engine_class))
		return NULL;

	if (eci.gt_id != 0)
		return NULL;

	return xe_gt_hw_engine(to_gt(xe),
			       user_to_xe_engine_class[eci.engine_class],
			       eci.engine_instance, true);
}

static u32 calc_validate_logical_mask(struct xe_device *xe,
				      struct drm_xe_engine_class_instance *eci,
				      u16 width, u16 num_placements)
{
	int len = width * num_placements;
	int i, j, n;
	u16 class;
	u32 return_mask = 0, prev_mask;

	if (XE_IOCTL_ERR(xe, !xe_gt_guc_submission_enabled(to_gt(xe)) &&
			 len > 1))
		return 0;

	for (i = 0; i < width; ++i) {
		u32 current_mask = 0;

		for (j = 0; j < num_placements; ++j) {
			n = i * num_placements + j;

			if (XE_IOCTL_ERR(xe, !find_hw_engine(xe, eci[n])))
				return 0;

			if (n && XE_IOCTL_ERR(xe, eci[n].engine_class != class))
				return 0;
			else
				class = eci[n].engine_class;

			if (width == 1 || !j)
				return_mask |= BIT(eci[n].engine_instance);
			current_mask |= BIT(eci[n].engine_instance);
		}

		/* Parallel submissions must be logically contiguous */
		if (i && XE_IOCTL_ERR(xe, current_mask != prev_mask << 1))
			return 0;

		prev_mask = current_mask;
	}

	return return_mask;
}

int xe_engine_create_ioctl(struct drm_device *dev, void *data,
			   struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_engine_create *args = data;
	struct drm_xe_engine_class_instance eci[XE_HW_ENGINE_MAX_INSTANCE];
	struct drm_xe_engine_class_instance __user *user_eci =
		u64_to_user_ptr(args->instances);
	struct xe_hw_engine *hwe;
	struct xe_vm *vm;
	struct xe_engine *e;
	u32 logical_mask;
	u32 id;
	int len;
	int err;

	if (XE_IOCTL_ERR(xe, args->extensions))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, args->flags))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, args->width != 1))
		return -EINVAL;

	len = args->width * args->num_placements;
	if (XE_IOCTL_ERR(xe, !len || len > XE_HW_ENGINE_MAX_INSTANCE))
		return -EINVAL;

	err = __copy_from_user(eci, user_eci,
			       sizeof(struct drm_xe_engine_class_instance) *
			       len);
	if (XE_IOCTL_ERR(xe, err))
		return -EFAULT;

	logical_mask = calc_validate_logical_mask(xe, eci, args->width,
						  args->num_placements);
	if (XE_IOCTL_ERR(xe, !logical_mask))
		return -EINVAL;

	hwe = find_hw_engine(xe, eci[0]);
	if (XE_IOCTL_ERR(xe, !hwe))
		return -EINVAL;

	vm = xe_vm_lookup(xef, args->vm_id);
	if (XE_IOCTL_ERR(xe, !vm))
		return -ENOENT;

	/* FIXME: Wire ENGINE_FLAG_PERSISTENT to engine parameter */
	e = xe_engine_create(xe, vm, logical_mask, args->width, hwe,
			     ENGINE_FLAG_PERSISTENT);
	xe_vm_put(vm);
	if (IS_ERR(e))
		return PTR_ERR(e);

	e->persitent.xef = xef;

	mutex_lock(&xef->engine.lock);
	err = xa_alloc(&xef->engine.xa, &id, e, xa_limit_32b, GFP_KERNEL);
	mutex_unlock(&xef->engine.lock);
	if (err) {
		xe_engine_put(e);
		return err;
	}

	args->engine_id = id;

	return 0;
}

int xe_engine_destroy_ioctl(struct drm_device *dev, void *data,
			    struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_engine_destroy *args = data;
	struct xe_engine *e;

	if (XE_IOCTL_ERR(xe, args->pad))
		return -EINVAL;

	mutex_lock(&xef->engine.lock);
	e = xa_erase(&xef->engine.xa, args->engine_id);
	mutex_unlock(&xef->engine.lock);
	if (XE_IOCTL_ERR(xe, !e))
		return -ENOENT;

	if (!(e->flags & ENGINE_FLAG_PERSISTENT))
		e->gt->engine_ops->kill(e);
	else
		xe_device_add_persitent_engines(xe, e);

	trace_xe_engine_close(e);
	xe_engine_put(e);

	return 0;
}

int xe_exec_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_exec *args = data;
	struct drm_xe_sync __user *syncs_user = u64_to_user_ptr(args->syncs);
	struct xe_engine *engine;
	struct xe_sync_entry *syncs;
	uint32_t i, num_syncs = 0;
	struct xe_sched_job *job;
	int err = 0;

	if (XE_IOCTL_ERR(xe, args->extensions))
		return -EINVAL;

	engine = xe_engine_lookup(xef, args->engine_id);
	if (XE_IOCTL_ERR(xe, !engine))
		return -ENOENT;

	if (XE_IOCTL_ERR(xe, engine->width != args->num_batch_buffer))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, engine->flags & ENGINE_FLAG_BANNED)) {
		err = -ECANCELED;
		goto err_engine;
	}

	syncs = kcalloc(args->num_syncs, sizeof(*syncs), GFP_KERNEL);
	if (!syncs) {
		err = -ENOMEM;
		goto err_engine;
	}

	for (i = 0; i < args->num_syncs; i++) {
		err = xe_sync_entry_parse(xe, xef, &syncs[num_syncs++], &syncs_user[i]);
		if (err)
			goto err_syncs;
	}

	err = xe_engine_begin(engine);
	if (err)
		goto err_syncs;

	job = xe_sched_job_create(engine, args->address);
	if (IS_ERR(job)) {
		err = PTR_ERR(job);
		goto err_engine_end;
	}

	for (i = 0; i < num_syncs; i++) {
		err = xe_sync_entry_add_deps(&syncs[i], job);
		if (err)
			goto err_put_job;
	}

	xe_sched_job_arm(job);

	for (i = 0; i < num_syncs; i++)
		xe_sync_entry_signal(&syncs[i], &job->drm.s_fence->finished);

	xe_sched_job_push(job);

err_put_job:
	if (err)
		xe_sched_job_free(job);
err_engine_end:
	xe_engine_end(engine);
err_syncs:
	for (i = 0; i < num_syncs; i++)
		xe_sync_entry_cleanup(&syncs[i]);
	kfree(syncs);
err_engine:
	xe_engine_put(engine);

	return err;
}
