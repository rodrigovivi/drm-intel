// SPDX-License-Identifier: MIT
/*
 * Copyright © 2021 Intel Corporation
 */


#include "xe_bo.h"

#include <linux/dma-buf.h>

#include <drm/drm_gem_ttm_helper.h>
#include <drm/ttm/ttm_device.h>
#include <drm/ttm/ttm_placement.h>
#include <drm/ttm/ttm_tt.h>
#include <drm/xe_drm.h>

#include "xe_device.h"
#include "xe_dma_buf.h"
#include "xe_ggtt.h"
#include "xe_gt.h"
#include "xe_map.h"
#include "xe_migrate.h"
#include "xe_res_cursor.h"
#include "xe_trace.h"
#include "xe_vm.h"

static const struct ttm_place sys_placement_flags = {
	.fpfn = 0,
	.lpfn = 0,
	.mem_type = XE_PL_SYSTEM,
	.flags = 0,
};

static struct ttm_placement sys_placement = {
	.num_placement = 1,
	.placement = &sys_placement_flags,
	.num_busy_placement = 1,
	.busy_placement = &sys_placement_flags,
};

bool mem_type_is_vram(u32 mem_type)
{
	return mem_type >= XE_PL_VRAM0;
}

static bool resource_is_vram(struct ttm_resource *res)
{
	return mem_type_is_vram(res->mem_type);
}

bool xe_bo_is_vram(struct xe_bo *bo)
{
	return resource_is_vram(bo->ttm.resource);
}

static struct xe_gt *
mem_type_to_gt(struct xe_device *xe, u32 mem_type)
{
	XE_BUG_ON(!mem_type_is_vram(mem_type));

	return xe_device_get_gt(xe, mem_type - XE_PL_VRAM0);
}

static int xe_bo_placement_for_flags(struct xe_device *xe, struct xe_bo *bo,
				     u32 bo_flags)
{
	struct ttm_place *places = bo->placements;
	struct xe_gt *gt;
	u32 c = 0;

	if (bo_flags & XE_BO_CREATE_VRAM0_BIT) {
		gt = mem_type_to_gt(xe, XE_PL_VRAM0);
		XE_BUG_ON(!gt->mem.vram.size);

		places[c++] = (struct ttm_place) {
			.mem_type = XE_PL_VRAM0,
			/*
			 * For eviction / restore on suspend / resume objects
			 * pinned in VRAM must be contiguous
			 */
			.flags = bo_flags & (XE_BO_CREATE_PINNED_BIT |
					     XE_BO_CREATE_GGTT_BIT) ?
				TTM_PL_FLAG_CONTIGUOUS : 0,
		};
	}

	if (bo_flags & XE_BO_CREATE_VRAM1_BIT) {
		gt = mem_type_to_gt(xe, XE_PL_VRAM1);
		XE_BUG_ON(!gt->mem.vram.size);

		places[c++] = (struct ttm_place) {
			.mem_type = XE_PL_VRAM1,
			.flags = bo_flags & (XE_BO_CREATE_PINNED_BIT |
					     XE_BO_CREATE_GGTT_BIT) ?
				TTM_PL_FLAG_CONTIGUOUS : 0,
		};
	}

	if (bo_flags & XE_BO_CREATE_SYSTEM_BIT) {
		places[c++] = (struct ttm_place) {
			.mem_type = XE_PL_TT,
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
	case XE_PL_VRAM0:
	case XE_PL_VRAM1:
	case XE_PL_TT:
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
				       u32 page_flags)
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
	struct xe_gt *gt;

	switch (mem->mem_type) {
	case XE_PL_SYSTEM:
	case XE_PL_TT:
		return 0;
	case XE_PL_VRAM0:
	case XE_PL_VRAM1:
		gt = mem_type_to_gt(xe, mem->mem_type);
		mem->bus.offset = mem->start << PAGE_SHIFT;

		if (gt->mem.vram.mapping &&
		    mem->placement & TTM_PL_FLAG_CONTIGUOUS)
			mem->bus.addr = (u8 *)gt->mem.vram.mapping +
				mem->bus.offset;

		mem->bus.offset += gt->mem.vram.io_start;
		mem->bus.is_iomem = true;

#if  !defined(CONFIG_X86)
		mem->bus.caching = ttm_write_combined;
#endif
		break;
	default:
		return -EINVAL;
	}
	return 0;
}

void xe_bo_trigger_rebind(struct xe_device *xe, struct xe_bo *bo)
{
	struct dma_resv_iter cursor;
	struct dma_fence *fence;
	struct xe_vma *vma;

	dma_resv_iter_begin(&cursor, bo->ttm.base.resv,
			    DMA_RESV_USAGE_PREEMPT_FENCE);
	dma_resv_for_each_fence_unlocked(&cursor, fence)
		dma_fence_enable_sw_signaling(fence);
	dma_resv_iter_end(&cursor);

	list_for_each_entry(vma, &bo->vmas, bo_link) {
		trace_xe_vma_evict(vma);

		if (list_empty(&vma->evict_link))
			list_add_tail(&vma->evict_link, &vma->vm->evict_list);
		if (xe_vm_in_compute_mode(vma->vm))
			queue_work(xe->ordered_wq,
				   &vma->vm->preempt.rebind_work);
	}
}

static int xe_bo_move(struct ttm_buffer_object *ttm_bo, bool evict,
		      struct ttm_operation_ctx *ctx,
		      struct ttm_resource *new_mem,
		      struct ttm_place *hop)
{
	struct xe_device *xe = ttm_to_xe_device(ttm_bo->bdev);
	struct xe_bo *bo = ttm_to_xe_bo(ttm_bo);
	struct ttm_resource *old_mem = ttm_bo->resource;
	struct xe_gt *gt = NULL;
	struct dma_fence *fence;
	int ret = 0;
	bool nounmap = false;

	if (old_mem->mem_type == XE_PL_SYSTEM && !ttm_bo->ttm) {
		ttm_bo_move_null(ttm_bo, new_mem);
		goto out;
	}

	if (old_mem->mem_type == XE_PL_SYSTEM &&
	    (new_mem->mem_type == XE_PL_TT)) {
		ttm_bo_move_null(ttm_bo, new_mem);
		goto out;
	}

	if (old_mem->mem_type == XE_PL_TT &&
	    new_mem->mem_type == XE_PL_SYSTEM) {
		long timeout = dma_resv_wait_timeout(ttm_bo->base.resv,
						     DMA_RESV_USAGE_PREEMPT_FENCE,
						     true,
						     MAX_SCHEDULE_TIMEOUT);
		if (timeout <= 0) {
			ret = -ETIME;
			goto out;
		}
		ttm_bo_move_null(ttm_bo, new_mem);
		goto rebind;
	}

	if (((old_mem->mem_type == XE_PL_SYSTEM && resource_is_vram(new_mem)) ||
	     (resource_is_vram(old_mem) &&
	      new_mem->mem_type == XE_PL_SYSTEM))) {
		hop->fpfn = 0;
		hop->lpfn = 0;
		hop->mem_type = XE_PL_TT;
		hop->flags = TTM_PL_FLAG_TEMPORARY;
		ret = -EMULTIHOP;
		goto out;
	}

	if (bo->gt)
		gt = bo->gt;
	else if (resource_is_vram(new_mem))
		gt = mem_type_to_gt(xe, new_mem->mem_type);
	else if (resource_is_vram(old_mem))
		gt = mem_type_to_gt(xe, old_mem->mem_type);

	XE_BUG_ON(!gt);
	XE_BUG_ON(!gt->migrate);

	trace_xe_bo_move(bo);
	xe_device_mem_access_wa_get(xe);

	if (xe_bo_is_pinned(bo)) {
		/*
		 * Memory that is pinned should only be moved on suspend /
		 * resume, some of the pinned memory is required for the device
		 * to resume / use the GPU to move other evicted memory (user
		 * memory) around. This likely could be optimized a bit futher
		 * where we find the minimum set of pinned memory required for
		 * resume but for simplity doing a memcpy for all pinned memory.
		 */
		ret = xe_bo_vmap(bo);
		if (!ret) {
			ret = ttm_bo_move_memcpy(ttm_bo, ctx, new_mem);

			/* Create a new VMAP once kernel BO back in VRAM */
			if (!ret && resource_is_vram(new_mem)) {
				void *new_addr = gt->mem.vram.mapping +
					(new_mem->start << PAGE_SHIFT);

				XE_BUG_ON(new_mem->start !=
					  bo->placements->fpfn);

				iosys_map_set_vaddr_iomem(&bo->vmap, new_addr);
				nounmap = true;
			}
		}
	} else {
		fence = xe_migrate_copy(gt->migrate, bo, old_mem, new_mem);
		if (IS_ERR(fence)) {
			ret = PTR_ERR(fence);
			xe_device_mem_access_wa_put(xe);
			goto out;
		}
		ret = ttm_bo_move_accel_cleanup(ttm_bo, fence, evict, true,
						new_mem);
		dma_fence_put(fence);
	}

	xe_device_mem_access_wa_put(xe);

rebind:
	trace_printk("new_mem->mem_type=%d\n", new_mem->mem_type);
	if (!xe_bo_is_pinned(bo)) {
		xe_bo_trigger_rebind(xe, bo);
		if (ttm_bo->base.dma_buf)
			dma_buf_move_notify(ttm_bo->base.dma_buf);
	}

out:
	if (!nounmap)
		xe_bo_vunmap(bo);
	return ret;

}

static unsigned long xe_ttm_io_mem_pfn(struct ttm_buffer_object *bo,
				       unsigned long page_offset)
{
	struct xe_device *xe = ttm_to_xe_device(bo->bdev);
	struct xe_gt *gt = mem_type_to_gt(xe, bo->resource->mem_type);
	struct xe_res_cursor cursor;

	xe_res_first(bo->resource, (u64)page_offset << PAGE_SHIFT, 0, &cursor);
	return (gt->mem.vram.io_start + cursor.start) >> PAGE_SHIFT;
}

static void __xe_bo_vunmap(struct xe_bo *bo);

static void xe_ttm_bo_release_notify(struct ttm_buffer_object *ttm_bo)
{
	struct xe_bo *bo;

	if (!xe_bo_is_xe_bo(ttm_bo))
		return;

	bo = ttm_to_xe_bo(ttm_bo);
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
	.eviction_valuable = ttm_bo_eviction_valuable,
};

static void xe_ttm_bo_destroy(struct ttm_buffer_object *ttm_bo)
{
	struct xe_bo *bo = ttm_to_xe_bo(ttm_bo);

	drm_gem_object_release(&bo->ttm.base);

	WARN_ON(!list_empty(&bo->vmas));

	if (bo->ggtt_node.size)
		xe_ggtt_remove_bo(bo->gt->mem.ggtt, bo);

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
	.export = xe_gem_prime_export,
};

struct xe_bo *__xe_bo_create_locked(struct xe_device *xe, struct xe_gt *gt,
				    struct dma_resv *resv, size_t size,
				    enum ttm_bo_type type, u32 flags)
{
	struct xe_bo *bo;
	struct ttm_operation_ctx ctx = {
		.interruptible = true,
		.no_wait_gpu = false,
	};
	int err;

	/* Only kernel objects should set GT */
	XE_BUG_ON(gt && type != ttm_bo_type_kernel);

	if (resv) {
		ctx.allow_res_evict = true;
		ctx.resv = resv;
	}

	bo = kzalloc(sizeof(*bo), GFP_KERNEL);
	if (!bo)
		return ERR_PTR(-ENOMEM);

	if (flags & (XE_BO_CREATE_VRAM0_BIT | XE_BO_CREATE_VRAM1_BIT) &&
	    !(flags & XE_BO_CREATE_IGNORE_MIN_PAGE_SIZE_BIT) &&
	    xe->info.vram_flags & XE_VRAM_FLAGS_NEED64K) {
		size = ALIGN(size, SZ_64K);
		flags |= XE_BO_INTERNAL_64K;
	}

	bo->gt = gt;
	bo->size = size;
	bo->flags = flags;
	bo->ttm.base.funcs = &xe_gem_object_funcs;
	bo->extobj_tv.num_shared = 1;
	bo->extobj_tv.bo = &bo->ttm;
	INIT_LIST_HEAD(&bo->vmas);

	drm_gem_private_object_init(&xe->drm, &bo->ttm.base, size);

	err = xe_bo_placement_for_flags(xe, bo, flags);
	if (WARN_ON(err))
		return ERR_PTR(err);

	err = ttm_bo_init_reserved(&xe->ttm, &bo->ttm, type,
				   DMA_RESV_USAGE_BOOKKEEP,
				   &bo->placement, SZ_64K >> PAGE_SHIFT,
				   &ctx, NULL, resv, xe_ttm_bo_destroy);
	if (WARN_ON(err))
		return ERR_PTR(err);

	return bo;
}

struct xe_bo *xe_bo_create_locked(struct xe_device *xe, struct xe_gt *gt,
				  struct xe_vm *vm, size_t size,
				  enum ttm_bo_type type, u32 flags)
{
	struct xe_bo *bo;
	int err;

	if (vm)
		xe_vm_assert_held(vm);
	bo = __xe_bo_create_locked(xe, gt, vm ? &vm->resv : NULL, size,
				   type, flags);
	if (IS_ERR(bo))
		return bo;

	if (vm && (flags & XE_BO_CREATE_USER_BIT))
		xe_vm_get(vm);
	bo->vm = vm;

	if (flags & XE_BO_CREATE_GGTT_BIT) {
		XE_BUG_ON(!gt);

		err = xe_ggtt_insert_bo(gt->mem.ggtt, bo);
		if (err)
			goto err_unlock_put_bo;
	}

	return bo;

err_unlock_put_bo:
	xe_bo_unlock_vm_held(bo);
	xe_bo_put(bo);
	return ERR_PTR(err);
}

struct xe_bo *xe_bo_create(struct xe_device *xe, struct xe_gt *gt,
			   struct xe_vm *vm, size_t size,
			   enum ttm_bo_type type, u32 flags)
{
	struct xe_bo *bo = xe_bo_create_locked(xe, gt, vm, size, type, flags);

	if (!IS_ERR(bo))
		xe_bo_unlock_vm_held(bo);

	return bo;
}

struct xe_bo *xe_bo_create_pin_map(struct xe_device *xe, struct xe_gt *gt,
				   struct xe_vm *vm, size_t size,
				   enum ttm_bo_type type, u32 flags)
{
	struct xe_bo *bo = xe_bo_create_locked(xe, gt, vm, size, type, flags);
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

struct xe_bo *xe_bo_create_from_data(struct xe_device *xe, struct xe_gt *gt,
				     const void *data, size_t size,
				     enum ttm_bo_type type, u32 flags)
{
	struct xe_bo *bo = xe_bo_create_pin_map(xe, gt, NULL,
						ALIGN(size, PAGE_SIZE),
						type, flags);
	if (IS_ERR(bo))
		return bo;

	xe_map_memcpy_to(xe, &bo->vmap, 0, data, size);

	return bo;
}

int xe_bo_populate(struct xe_bo *bo)
{
	struct ttm_operation_ctx ctx = {
		.interruptible = false,
		.no_wait_gpu = false
	};

	xe_bo_assert_held(bo);

	if (bo->vm) {
		ctx.allow_res_evict = true;
		ctx.resv = &bo->vm->resv;
	}

	/* only populate non-VRAM */
	if (xe_bo_is_vram(bo))
		return 0;

	return ttm_tt_populate(bo->ttm.bdev, bo->ttm.ttm, &ctx);
}

/*
 * XXX: This is in the VM bind data path, likely should calculate this once and
 * store, with a recalculation if the BO is moved.
 */
static uint64_t vram_region_io_offset(struct xe_bo *bo)
{
	struct xe_device *xe = xe_bo_device(bo);
	struct xe_gt *gt = mem_type_to_gt(xe, bo->ttm.resource->mem_type);

	return gt->mem.vram.io_start - xe->mem.vram.io_start;
}

int xe_bo_pin(struct xe_bo *bo)
{
	struct xe_device *xe = xe_bo_device(bo);
	int err;

	/* We currently don't expect user BO to be pinned */
	XE_BUG_ON(bo->flags & XE_BO_CREATE_USER_BIT);

	/* Pinned object must be in GGTT or have pinned flag */
	XE_BUG_ON(!(bo->flags & (XE_BO_CREATE_PINNED_BIT |
				 XE_BO_CREATE_GGTT_BIT)));

	/*
	 * No reason we can't support pinning imported dma-bufs we just don't
	 * expect to pin an imported dma-buf.
	 */
	XE_BUG_ON(bo->ttm.base.import_attach);

	/* We only expect at most 1 pin */
	XE_BUG_ON(xe_bo_is_pinned(bo));

	err = xe_bo_populate(bo);
	if (err)
		return err;

	/*
	 * For pinned objects in on DGFX, we expect these objects to be in
	 * contiguous VRAM memory. Required eviction / restore during suspend /
	 * resume (force restore to same physical address).
	 */
	if (IS_DGFX(xe) && !(IS_ENABLED(CONFIG_DRM_XE_DEBUG) &&
	    bo->flags & XE_BO_INTERNAL_TEST)) {
		struct ttm_place *place = &(bo->placements[0]);
		bool lmem;

		XE_BUG_ON(!(place->flags & TTM_PL_FLAG_CONTIGUOUS));
		XE_BUG_ON(!mem_type_is_vram(place->mem_type));

		place->fpfn = (xe_bo_addr(bo, 0, PAGE_SIZE, &lmem) -
			       vram_region_io_offset(bo)) >> PAGE_SHIFT;
		place->lpfn = place->fpfn + (bo->size >> PAGE_SHIFT);

		spin_lock(&xe->pinned.lock);
		INIT_LIST_HEAD(&bo->pinned_link);
		list_add_tail(&bo->pinned_link, &xe->pinned.present);
		spin_unlock(&xe->pinned.lock);
	}

	ttm_bo_pin(&bo->ttm);

	/*
	 * FIXME: If we always use the reserve / unreserve functions for locking
	 * we do not need this.
	 */
	ttm_bo_move_to_lru_tail_unlocked(&bo->ttm);

	return 0;
}

void xe_bo_unpin(struct xe_bo *bo)
{
	struct xe_device *xe = xe_bo_device(bo);

	XE_BUG_ON(bo->ttm.base.import_attach);
	XE_BUG_ON(!xe_bo_is_pinned(bo));

	if (IS_DGFX(xe) && !(IS_ENABLED(CONFIG_DRM_XE_DEBUG) &&
	    bo->flags & XE_BO_INTERNAL_TEST)) {
		XE_BUG_ON(list_empty(&bo->pinned_link));

		spin_lock(&xe->pinned.lock);
		list_del(&bo->pinned_link);
		spin_unlock(&xe->pinned.lock);
	}

	__xe_bo_vunmap(bo);	/* FIXME: W/A for blow up in ttm_bo_vunmap */
	ttm_bo_unpin(&bo->ttm);
}

int xe_bo_validate(struct xe_bo *bo, struct xe_vm *vm)
{
	struct ttm_operation_ctx ctx = {
		.interruptible = true,
		.no_wait_gpu = false,
	};

	if (vm) {
		lockdep_assert_held(&vm->lock);
		xe_vm_assert_held(vm);

		ctx.allow_res_evict = true;
		ctx.resv = &vm->resv;
	}

	return ttm_bo_validate(&bo->ttm, &bo->placement, &ctx);
}

bool xe_bo_is_xe_bo(struct ttm_buffer_object *bo)
{
	if (bo->destroy == &xe_ttm_bo_destroy)
		return true;

	return false;
}

dma_addr_t xe_bo_addr(struct xe_bo *bo, u64 offset,
		      size_t page_size, bool *is_lmem)
{
	u64 page;

	if (!READ_ONCE(bo->ttm.pin_count))
		xe_bo_assert_held(bo);

	XE_BUG_ON(page_size > PAGE_SIZE);
	page = offset >> PAGE_SHIFT;
	offset &= (PAGE_SIZE - 1);

	*is_lmem = xe_bo_is_vram(bo);

	if (!*is_lmem) {
		XE_BUG_ON(!bo->ttm.ttm || !bo->ttm.ttm->dma_address);
		return bo->ttm.ttm->dma_address[page] + offset;
	} else {
		struct xe_res_cursor cur;

		xe_res_first(bo->ttm.resource, page << PAGE_SHIFT,
			     page_size, &cur);
		return cur.start + offset + vram_region_io_offset(bo);
	}
}

int xe_bo_vmap(struct xe_bo *bo)
{
	xe_bo_assert_held(bo);

	if (!iosys_map_is_null(&bo->vmap))
		return 0;

	return ttm_bo_vmap(&bo->ttm, &bo->vmap);
}

static void __xe_bo_vunmap(struct xe_bo *bo)
{
	/* FIXME: W/A for blow up in ttm_bo_vunmap */
	if (xe_bo_is_pinned(bo) && IS_DGFX(xe_bo_device(bo))) {
		iosys_map_clear(&bo->vmap);
		return;
	}

	ttm_bo_vunmap(&bo->ttm, &bo->vmap);
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
	struct ww_acquire_ctx ww;
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

	if (XE_IOCTL_ERR(xe, args->size & ~PAGE_MASK))
		return -EINVAL;

	if (args->vm_id) {
		vm = xe_vm_lookup(xef, args->vm_id);
		if (XE_IOCTL_ERR(xe, !vm))
			return -ENOENT;
		err = xe_vm_lock(vm, &ww, 0, true);
		if (err) {
			xe_vm_put(vm);
			return err;
		}
	}

	if (args->flags & DRM_XE_GEM_CREATE_SYSTEM)
		bo_flags |= XE_BO_CREATE_SYSTEM_BIT;
	if (args->flags & DRM_XE_GEM_CREATE_VRAM)
		bo_flags |= XE_BO_CREATE_VRAM0_BIT;

	bo = xe_bo_create(xe, NULL, vm, args->size, ttm_bo_type_device,
			  bo_flags);
	if (vm) {
		xe_vm_unlock(vm, &ww);
		xe_vm_put(vm);
	}

	if (IS_ERR(bo))
		return PTR_ERR(bo);

	err = drm_gem_handle_create(file, &bo->ttm.base, &handle);
	drm_gem_object_put(&bo->ttm.base);
	if (err)
		return err;

	args->handle = handle;

#if IS_ENABLED(CONFIG_DRM_XE_DEBUG_MEM)
	/* Warning: Security issue - never enable by default */
	args->reserved[0] = xe_bo_main_addr(bo, GEN8_PAGE_SIZE);
#endif

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

int xe_bo_lock(struct xe_bo *bo, struct ww_acquire_ctx *ww,
	       int num_resv, bool intr)
{
	struct ttm_validate_buffer tv_bo;
	LIST_HEAD(objs);
	LIST_HEAD(dups);

	XE_BUG_ON(!ww);

	tv_bo.num_shared = num_resv;
	tv_bo.bo = &bo->ttm;;
	list_add_tail(&tv_bo.head, &objs);

	return ttm_eu_reserve_buffers(ww, &objs, intr, &dups);
}

void xe_bo_unlock(struct xe_bo *bo, struct ww_acquire_ctx *ww)
{
	dma_resv_unlock(bo->ttm.base.resv);
	ww_acquire_fini(ww);
}
