/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */


#include "xe_bo.h"

#include <drm/drm_gem_ttm_helper.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_tt.h>
#include <drm/xe_drm.h>

#include "xe_device.h"
#include "xe_ggtt.h"
#include "xe_gt.h"
#include "xe_migrate.h"
#include "xe_res_cursor.h"
#include "xe_vm.h"

static const struct ttm_place sys_placement_flags = {
	.fpfn = 0,
	.lpfn = 0,
	.mem_type = TTM_PL_SYSTEM,
	.flags = 0,
};

static struct ttm_placement sys_placement = {
	.num_placement = 1,
	.placement = &sys_placement_flags,
	.num_busy_placement = 1,
	.busy_placement = &sys_placement_flags,
};

static int xe_bo_placement_for_flags(struct xe_device *xe, struct xe_bo *bo,
				     uint32_t bo_flags)
{
	struct ttm_place *places = bo->placements;
	u32 c = 0;

	if (bo_flags & XE_BO_CREATE_VRAM_BIT) {
		XE_BUG_ON(!to_gt(xe)->mem.vram.size);
		places[c++] = (struct ttm_place) {
			.mem_type = TTM_PL_VRAM,
		};
	}

	if (bo_flags & XE_BO_CREATE_SYSTEM_BIT) {
		places[c++] = (struct ttm_place) {
			.mem_type = TTM_PL_TT,
		};
	}

	if (!c)
		return -EINVAL;

	bo->placement = (struct ttm_placement) {
		.num_placement = c,
		.placement = places,
		.num_busy_placement = c,
		.busy_placement = places,
	};

	return 0;
}

static void xe_evict_flags(struct ttm_buffer_object *tbo,
			   struct ttm_placement *placement)
{
	struct xe_bo *bo;

	/* Don't handle scatter gather BOs */
	if (tbo->type == ttm_bo_type_sg) {
		placement->num_placement = 0;
		placement->num_busy_placement = 0;
		return;
	}

	if (!xe_bo_is_xe_bo(tbo)) {
		*placement = sys_placement;
		return;
	}

	bo = ttm_to_xe_bo(tbo);
	switch (tbo->resource->mem_type) {
	case TTM_PL_VRAM:
	case TTM_PL_TT:
	default:
		/* for now kick out to system */
		*placement = sys_placement;
		break;
	}
}

struct xe_ttm_tt {
	struct ttm_tt ttm;
};

static struct ttm_tt *xe_ttm_tt_create(struct ttm_buffer_object *ttm_bo,
				       uint32_t page_flags)
{
	struct xe_bo *bo = ttm_to_xe_bo(ttm_bo);
	struct xe_ttm_tt *tt;
	int err;

	tt = kzalloc(sizeof(*tt), GFP_KERNEL);
	if (!tt)
		return NULL;

	/* TODO: We only need to do this for user allocated BOs */
	page_flags |= TTM_TT_FLAG_ZERO_ALLOC;

	/* TODO: Select caching mode */
	err = ttm_sg_tt_init(&tt->ttm, &bo->ttm, page_flags, ttm_cached);
	if (err) {
		kfree(tt);
		return NULL;
	}

	return &tt->ttm;
}

static void xe_ttm_tt_destroy(struct ttm_device *ttm_dev, struct ttm_tt *tt)
{
	ttm_tt_fini(tt);
	kfree(tt);
}

static int xe_ttm_io_mem_reserve(struct ttm_device *bdev,
				 struct ttm_resource *mem)
{
	struct xe_device *xe = ttm_to_xe_device(bdev);

	switch (mem->mem_type) {
	case TTM_PL_SYSTEM:
	case TTM_PL_TT:
		return 0;
	case TTM_PL_VRAM:
		mem->bus.offset = mem->start << PAGE_SHIFT;

		if (to_gt(xe)->mem.vram.mapping &&
		    mem->placement & TTM_PL_FLAG_CONTIGUOUS)
			mem->bus.addr = (u8 *)to_gt(xe)->mem.vram.mapping +
				mem->bus.offset;

		mem->bus.offset += to_gt(xe)->mem.vram.io_start;
		mem->bus.is_iomem = true;
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

static int xe_move_blit(struct xe_bo *bo, bool evict, struct ttm_resource *new_mem,
			struct ttm_resource *old_mem, struct xe_gt *gt,
			struct ttm_operation_ctx *ctx)
{
	struct dma_fence *fence;
	int ret;

	/* TODO: add VM unbind worker here */

	fence = xe_migrate_copy(gt->migrate, bo, old_mem, new_mem);
	if (IS_ERR(fence))
		return PTR_ERR(fence);

	ret = ttm_bo_move_accel_cleanup(&bo->ttm, fence, evict, true, new_mem);
	dma_fence_put(fence);
	return ret;
}

static int xe_bo_move(struct ttm_buffer_object *ttm_bo, bool evict,
		      struct ttm_operation_ctx *ctx,
		      struct ttm_resource *new_mem,
		      struct ttm_place *hop)
{
	struct xe_bo *bo = ttm_to_xe_bo(ttm_bo);
	struct ttm_resource *old_mem = bo->ttm.resource;
	struct xe_gt *gt;
	int r;

	xe_bo_vunmap(bo);

	if (old_mem->mem_type == TTM_PL_SYSTEM && !ttm_bo->ttm) {
		ttm_bo_move_null(&bo->ttm, new_mem);
		goto out;
	}

	if (old_mem->mem_type == TTM_PL_SYSTEM &&
	    (new_mem->mem_type == TTM_PL_TT)) {
		ttm_bo_move_null(&bo->ttm, new_mem);
		goto out;
	}

	if (((old_mem->mem_type == TTM_PL_SYSTEM &&
	      new_mem->mem_type == TTM_PL_VRAM) ||
	     (old_mem->mem_type == TTM_PL_VRAM &&
	      new_mem->mem_type == TTM_PL_SYSTEM))) {
		hop->fpfn = 0;
		hop->lpfn = 0;
		hop->mem_type = TTM_PL_TT;
		hop->flags = TTM_PL_FLAG_TEMPORARY;
		return -EMULTIHOP;
	}

	/* TODO: Determine GT based on (new,old)_mem->mem_type's VRAM on multitile */
	gt = to_gt(ttm_to_xe_device(ttm_bo->bdev));

	if (gt->migrate)
		r = xe_move_blit(bo, evict, new_mem, old_mem, gt, ctx);
	else
		r = ttm_bo_move_memcpy(&bo->ttm, ctx, new_mem);

	if (r)
		return r;

	DRM_ERROR("Moving not supported yet: Missing rebind\n");
	return -ENODEV;

out:
	return 0;

}

static unsigned long xe_ttm_io_mem_pfn(struct ttm_buffer_object *bo,
				       unsigned long page_offset)
{
	struct xe_device *xe = ttm_to_xe_device(bo->bdev);
	struct xe_res_cursor cursor;

	xe_res_first(bo->resource, (u64)page_offset << PAGE_SHIFT, 0, &cursor);
	return (to_gt(xe)->mem.vram.io_start + cursor.start) >> PAGE_SHIFT;
}

static void __xe_bo_vunmap(struct xe_bo *bo);

static void xe_ttm_bo_release_notify(struct ttm_buffer_object *ttm_bo)
{
	struct xe_bo *bo = ttm_to_xe_bo(ttm_bo);

	__xe_bo_vunmap(bo);
}

struct ttm_device_funcs xe_ttm_funcs = {
	.ttm_tt_create = xe_ttm_tt_create,
	.ttm_tt_destroy = xe_ttm_tt_destroy,
	.evict_flags = xe_evict_flags,
	.move = xe_bo_move,
	.io_mem_reserve = xe_ttm_io_mem_reserve,
	.io_mem_pfn = xe_ttm_io_mem_pfn,
	.release_notify = xe_ttm_bo_release_notify,
};

static void xe_ttm_bo_destroy(struct ttm_buffer_object *ttm_bo)
{
	struct xe_bo *bo = ttm_to_xe_bo(ttm_bo);

	drm_gem_object_release(&bo->ttm.base);

	WARN_ON(!list_empty(&bo->vmas));

	if (bo->ggtt_node.size)
		xe_ggtt_remove_bo(to_gt(xe_bo_device(bo))->mem.ggtt, bo);

	if (bo->vm && (bo->flags & XE_BO_CREATE_USER_BIT))
		xe_vm_put(bo->vm);

	kfree(bo);
}

static void xe_gem_object_free(struct drm_gem_object *obj)
{
	/* Our BO reference counting scheme works as follows:
	 *
	 * The ttm_buffer_object and the drm_gem_object each have their own
	 * kref.  We treat the ttm_buffer_object.kref as the "real" reference
	 * count.  The drm_gem_object implicitly owns a reference to the
	 * ttm_buffer_object and, when drm_gem_object.refcount hits zero, we
	 * drop that reference here.  When ttm_buffer_object.kref hits zero,
	 * xe_ttm_bo_destroy() is invoked to do the actual free.
	 */
	xe_bo_put(gem_to_xe_bo(obj));
}

static const struct drm_gem_object_funcs xe_gem_object_funcs = {
	.free = xe_gem_object_free,
	.mmap = drm_gem_ttm_mmap,
};

struct xe_bo *xe_bo_create_locked(struct xe_device *xe, struct xe_vm *vm,
				  size_t size, enum ttm_bo_type type,
				  uint32_t flags)
{
	struct xe_bo *bo;
	struct ttm_operation_ctx ctx = {
		.interruptible = true,
		.no_wait_gpu = false,
	};
	int err;

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	if (flags & XE_BO_CREATE_VRAM_BIT &&
	    !(flags & XE_BO_CREATE_IGNORE_MIN_PAGE_SIZE_BIT) &&
	    xe->info.vram_flags & XE_VRAM_FLAGS_NEED64K) {
		size = ALIGN(size, SZ_64K);
		flags |= XE_BO_INTERNAL_64K;
	}

	bo->size = size;
	bo->flags = flags;
	bo->ttm.base.funcs = &xe_gem_object_funcs;
	INIT_LIST_HEAD(&bo->vmas);

	drm_gem_private_object_init(&xe->drm, &bo->ttm.base, size);

	if (vm)
		xe_vm_assert_held(vm);

	err = xe_bo_placement_for_flags(xe, bo, flags);
	if (WARN_ON(err))
		return ERR_PTR(err);

	err = ttm_bo_init_reserved(&xe->ttm, &bo->ttm, size, type,
				   &bo->placement, SZ_64K >> PAGE_SHIFT,
				   &ctx, NULL, vm ? &vm->resv : NULL,
				   xe_ttm_bo_destroy);
	if (WARN_ON(err))
		return ERR_PTR(err);

	if (vm && (flags & XE_BO_CREATE_USER_BIT))
		xe_vm_get(vm);
	bo->vm = vm;

	if (flags & XE_BO_CREATE_GGTT_BIT) {
		err = xe_ggtt_insert_bo(to_gt(xe)->mem.ggtt, bo);
		if (err)
			goto err_unlock_put_bo;
	}

	return bo;

err_unlock_put_bo:
	xe_bo_unlock_vm_held(bo);
	xe_bo_put(bo);
	return ERR_PTR(err);
}

struct xe_bo *xe_bo_create(struct xe_device *xe, struct xe_vm *vm,
			   size_t size, enum ttm_bo_type type, uint32_t flags)
{
	struct xe_bo *bo = xe_bo_create_locked(xe, vm, size, type, flags);

	if (!IS_ERR(bo))
		xe_bo_unlock_vm_held(bo);

	return bo;
}

struct xe_bo *xe_bo_create_pin_map(struct xe_device *xe, struct xe_vm *vm,
				   size_t size, enum ttm_bo_type type,
				   uint32_t flags)
{
	struct xe_bo *bo = xe_bo_create_locked(xe, vm, size, type, flags);
	int err;

	if (IS_ERR(bo))
		return bo;

	err = xe_bo_pin(bo);
	if (err)
		goto err_put;

	err = xe_bo_vmap(bo);
	if (err)
		goto err_unpin;

	xe_bo_unlock_vm_held(bo);

	return bo;

err_unpin:
	xe_bo_unpin(bo);
err_put:
	xe_bo_unlock_vm_held(bo);
	xe_bo_put(bo);
	return ERR_PTR(err);
}

struct xe_bo *xe_bo_create_from_data(struct xe_device *xe, const void *data,
				     size_t size, enum ttm_bo_type type,
				     uint32_t flags)
{
	struct xe_bo *bo = xe_bo_create_pin_map(xe, NULL,
						ALIGN(size, PAGE_SIZE),
						type, flags);
	if (IS_ERR(bo))
		return bo;

	dma_buf_map_memcpy_to(&bo->vmap, data, size);

	return bo;
}

int xe_bo_populate(struct xe_bo *bo)
{
	struct ttm_operation_ctx ctx = {
		.interruptible = false,
		.no_wait_gpu = false
	};

	xe_bo_assert_held(bo);

	/* only populate non-VRAM */
	if (bo->ttm.resource->mem_type == TTM_PL_VRAM)
		return 0;

	return ttm_tt_populate(bo->ttm.bdev, bo->ttm.ttm, &ctx);
}

int xe_bo_pin(struct xe_bo *bo)
{
	int err = xe_bo_populate(bo);
	if (err)
		return err;

	ttm_bo_pin(&bo->ttm);

	return 0;
}

void xe_bo_unpin(struct xe_bo *bo)
{
	ttm_bo_unpin(&bo->ttm);
}

bool xe_bo_is_xe_bo(struct ttm_buffer_object *bo)
{
	if (bo->destroy == &xe_ttm_bo_destroy)
		return true;

	return false;
}

dma_addr_t xe_bo_addr(struct xe_bo *bo, uint64_t offset,
		      size_t page_size, bool *is_lmem)
{
	uint64_t page;

	if (!READ_ONCE(bo->ttm.pin_count))
		xe_bo_assert_held(bo);

	XE_BUG_ON(page_size > PAGE_SIZE);
	page = offset >> PAGE_SHIFT;
	offset &= (PAGE_SIZE - 1);

	*is_lmem = bo->ttm.resource->mem_type == TTM_PL_VRAM;

	if (!*is_lmem) {
		XE_BUG_ON(!bo->ttm.ttm || !bo->ttm.ttm->dma_address);
		return bo->ttm.ttm->dma_address[page] + offset;
	} else {
		struct xe_res_cursor cur;

		xe_res_first(bo->ttm.resource, page << PAGE_SHIFT,
			     page_size, &cur);
		return cur.start + offset;
	}
}

int xe_bo_vmap(struct xe_bo *bo)
{
	xe_bo_assert_held(bo);

	if (!dma_buf_map_is_null(&bo->vmap))
		return 0;

	return ttm_bo_vmap(&bo->ttm, &bo->vmap);
}

static void __xe_bo_vunmap(struct xe_bo *bo)
{
	if (dma_buf_map_is_null(&bo->vmap))
		return;

	ttm_bo_vunmap(&bo->ttm, &bo->vmap);
	dma_buf_map_clear(&bo->vmap);
}

void xe_bo_vunmap(struct xe_bo *bo)
{
	xe_bo_assert_held(bo);
	__xe_bo_vunmap(bo);
}

#define ALL_DRM_XE_GEM_CREATE_FLAGS (\
	DRM_XE_GEM_CREATE_SYSTEM | DRM_XE_GEM_CREATE_VRAM)

#define MEM_DRM_XE_GEM_CREATE_FLAGS (\
	DRM_XE_GEM_CREATE_SYSTEM | DRM_XE_GEM_CREATE_VRAM)

int xe_gem_create_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = to_xe_file(file);
	struct drm_xe_gem_create *args = data;
	struct xe_vm *vm = NULL;
	struct xe_bo *bo;
	unsigned bo_flags = XE_BO_CREATE_USER_BIT;
	u32 handle;
	int err;

	if (XE_IOCTL_ERR(xe, args->extensions))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, args->flags & ~ALL_DRM_XE_GEM_CREATE_FLAGS))
		return -EINVAL;

	/* at least one memory type must be specified */
	if (XE_IOCTL_ERR(xe, !(args->flags & MEM_DRM_XE_GEM_CREATE_FLAGS)))
		return -EINVAL;

	if (!IS_DGFX(xe)) {
		if (XE_IOCTL_ERR(xe, args->flags & DRM_XE_GEM_CREATE_VRAM))
			return -EINVAL;
	}

	if (XE_IOCTL_ERR(xe, args->handle))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, args->size > SIZE_MAX))
		return -EINVAL;

	if (args->vm_id) {
		vm = xe_vm_lookup(xef, args->vm_id);
		if (XE_IOCTL_ERR(xe, !vm))
			return -ENOENT;
		xe_vm_lock(vm, NULL);
	}

	if (args->flags & DRM_XE_GEM_CREATE_SYSTEM)
		bo_flags |= XE_BO_CREATE_SYSTEM_BIT;
	if (args->flags & DRM_XE_GEM_CREATE_VRAM)
		bo_flags |= XE_BO_CREATE_VRAM_BIT;

	bo = xe_bo_create(xe, vm, args->size, ttm_bo_type_device, bo_flags);
	if (vm) {
		xe_vm_unlock(vm);
		xe_vm_put(vm);
	}

	if (IS_ERR(bo))
		return PTR_ERR(bo);

	err = drm_gem_handle_create(file, &bo->ttm.base, &handle);
	drm_gem_object_put(&bo->ttm.base);
	if (err)
		return err;

	args->handle = handle;
	return 0;
}

int xe_gem_mmap_offset_ioctl(struct drm_device *dev, void *data,
			     struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct drm_xe_gem_mmap_offset *args = data;
	struct drm_gem_object *gem_obj;

	if (XE_IOCTL_ERR(xe, args->extensions))
		return -EINVAL;

	if (XE_IOCTL_ERR(xe, args->flags))
		return -EINVAL;

	gem_obj = drm_gem_object_lookup(file, args->handle);
	if (XE_IOCTL_ERR(xe, !gem_obj))
		return -ENOENT;

	/* The mmap offset was set up at BO allocation time. */
	args->offset = drm_vma_node_offset_addr(&gem_obj->vma_node);

	drm_gem_object_put(gem_obj);
	return 0;
}
