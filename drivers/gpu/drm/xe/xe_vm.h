/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2021 Intel Corporation
 */

#ifndef _XE_VM_H_
#define _XE_VM_H_

#include "xe_macros.h"
#include "xe_map.h"
#include "xe_vm_types.h"

struct drm_device;
struct drm_file;

struct ttm_buffer_object;

struct xe_engine;
struct xe_file;
struct xe_sync_entry;

struct xe_vm *xe_vm_create(struct xe_device *xe, u32 flags);
void xe_vm_free(struct kref *ref);

struct xe_vm *xe_vm_lookup(struct xe_file *xef, u32 id);
int xe_vma_cmp_vma_cb(const void *key, const struct rb_node *node);

static inline struct xe_vm *xe_vm_get(struct xe_vm *vm)
{
	kref_get(&vm->refcount);
	return vm;
}

static inline void xe_vm_put(struct xe_vm *vm)
{
	kref_put(&vm->refcount, xe_vm_free);
}

int xe_vm_lock(struct xe_vm *vm, struct ww_acquire_ctx *ww,
	       int num_resv, bool intr);

void xe_vm_unlock(struct xe_vm *vm, struct ww_acquire_ctx *ww);

static inline bool xe_vm_is_closed(struct xe_vm *vm)
{
	/* Only guaranteed not to change when vm->resv is held */
	return !vm->size;
}

struct xe_vma *
xe_vm_find_overlapping_vma(struct xe_vm *vm, const struct xe_vma *vma);

struct dma_fence *
__xe_vm_bind_vma(struct xe_gt *gt, struct xe_vma *vma, struct xe_engine *e,
		 struct xe_sync_entry *syncs, u32 num_syncs,
		 bool rebind);

#define xe_vm_assert_held(vm) dma_resv_assert_held(&(vm)->resv)

u64 xe_vm_pdp4_descriptor(struct xe_vm *vm, struct xe_gt *full_gt);

int xe_vm_create_ioctl(struct drm_device *dev, void *data,
		       struct drm_file *file);
int xe_vm_destroy_ioctl(struct drm_device *dev, void *data,
			struct drm_file *file);
int xe_vm_bind_ioctl(struct drm_device *dev, void *data,
		     struct drm_file *file);

void xe_vm_close_and_put(struct xe_vm *vm);

static inline bool xe_vm_in_compute_mode(struct xe_vm *vm)
{
	return vm->flags & XE_VM_FLAG_COMPUTE_MODE;
}

static inline bool xe_vm_in_fault_mode(struct xe_vm *vm)
{
	return vm->flags & XE_VM_FLAG_FAULT_MODE;
}

static inline bool xe_vm_no_dma_fences(struct xe_vm *vm)
{
	return xe_vm_in_compute_mode(vm) || xe_vm_in_fault_mode(vm);
}

int xe_vm_add_compute_engine(struct xe_vm *vm, struct xe_engine *e);

int xe_vm_userptr_pin(struct xe_vm *vm, bool rebind_worker);
int xe_vm_userptr_needs_repin(struct xe_vm *vm, bool rebind_worker);
struct dma_fence *xe_vm_rebind(struct xe_vm *vm, bool rebind_worker);
static inline bool xe_vm_has_userptr(struct xe_vm *vm)
{
	lockdep_assert_held(&vm->lock);

	return !list_empty(&vm->userptr.list);
}

int xe_vm_invalidate_vma(struct xe_vma *vma);

int xe_vm_async_fence_wait_start(struct dma_fence *fence);

#define xe_pt_write(xe, map, idx, data) \
	xe_map_wr(xe, map, idx * sizeof(u64), u64, data)

u64 gen8_pde_encode(struct xe_bo *bo, u64 bo_offset,
		    const enum xe_cache_level level);
u64 gen8_pte_encode(struct xe_vma *vma, struct xe_bo *bo,
		    u64 offset, enum xe_cache_level cache,
		    u32 flags, u32 pt_level);

extern struct ttm_device_funcs xe_ttm_funcs;

struct ttm_buffer_object *xe_vm_ttm_bo(struct xe_vm *vm);

static inline bool xe_vma_is_userptr(struct xe_vma *vma)
{
	return !vma->bo;
}

int xe_vma_userptr_pin_pages(struct xe_vma *vma);
int xe_vma_userptr_needs_repin(struct xe_vma *vma);

#if IS_ENABLED(CONFIG_DRM_XE_DEBUG_VM)
#define xe_pt_set_addr(__xe_pt, __addr) ((__xe_pt)->addr = (__addr))
#define xe_pt_addr(__xe_pt) ((__xe_pt)->addr)
#else
#define xe_pt_set_addr(__xe_pt, __addr)
#define xe_pt_addr(__xe_pt) 0ull
#endif

#endif
