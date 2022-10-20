// SPDX-License-Identifier: MIT
/*
 * Copyright © 2020 Intel Corporation
 */
#include "xe_migrate.h"

#include "xe_bb.h"
#include "xe_bo.h"
#include "xe_engine.h"
#include "xe_ggtt.h"
#include "xe_gt.h"
#include "xe_hw_engine.h"
#include "xe_lrc.h"
#include "xe_res_cursor.h"
#include "xe_sched_job.h"
#include "xe_sync.h"

#include <linux/sizes.h>
#include <drm/drm_managed.h>
#include <drm/ttm/ttm_tt.h>

#include "../i915/gt/intel_gpu_commands.h"

struct xe_migrate {
	struct drm_mm_node copy_node;
	struct xe_engine *eng;
	struct xe_gt *gt;
	struct mutex job_mutex;

	struct drm_suballoc_manager vm_update_sa;
};

#define CHUNK_SZ SZ_8M

static void xe_migrate_fini(struct drm_device *dev, void *arg)
{
	struct xe_migrate *m = arg;

	drm_suballoc_manager_fini(&m->vm_update_sa);
	mutex_destroy(&m->job_mutex);
	xe_engine_put(m->eng);
	xe_ggtt_remove_node(m->gt->mem.ggtt, &m->copy_node);
}

struct xe_migrate *xe_migrate_init(struct xe_gt *gt)
{
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_migrate *m;
	int err;

	m = drmm_kzalloc(&xe->drm, sizeof(*m), GFP_KERNEL);
	if (!m)
		return ERR_PTR(-ENOMEM);

	m->gt = gt;

	/* first 2 CHUNK_SZ, are used by the copy engine, last CHUNK_SZ is shared by bound VM updates */
	err = xe_ggtt_insert_special_node(gt->mem.ggtt, &m->copy_node, 3 * CHUNK_SZ, CHUNK_SZ);
	if (err)
		return ERR_PTR(err);

	m->eng = xe_engine_create_class(xe, NULL, XE_ENGINE_CLASS_COPY, ENGINE_FLAG_KERNEL);
	if (IS_ERR(m->eng)) {
		xe_ggtt_remove_node(gt->mem.ggtt, &m->copy_node);
		return ERR_CAST(m->eng);
	}
	mutex_init(&m->job_mutex);
	drm_suballoc_manager_init(&m->vm_update_sa, CHUNK_SZ, GEN8_PAGE_SIZE);

	err = drmm_add_action_or_reset(&xe->drm, xe_migrate_fini, m);
	if (err)
		return ERR_PTR(err);

	return m;
}

static void emit_arb_clear(struct xe_bb *bb)
{
	/* 1 dword */
	bb->cs[bb->len++] = MI_ARB_ON_OFF | MI_ARB_DISABLE;
}

#define MAX_GGTT_UPDATE_SIZE \
	(2 * DIV_ROUND_UP(CHUNK_SZ >> GEN8_PTE_SHIFT, 0xff) /* (nr of MI_UPDATE_GTT) */ + \
	 2 * (CHUNK_SZ >> GEN8_PTE_SHIFT) /* size of each entry in dwords * nr of entries*/ )

static void emit_pte(struct xe_ggtt *ggtt, struct xe_bb *bb, u64 ggtt_ofs,
		     struct ttm_resource *res, struct xe_res_cursor *cur,
		     u32 ofs, u32 size, struct ttm_tt *ttm)
{
	u32 ptes = size >> GEN8_PTE_SHIFT;
	bool lmem = res->mem_type == TTM_PL_VRAM;

	while (ptes) {
		u32 chunk = min(0xffU, ptes);

		bb->cs[bb->len++] = MI_UPDATE_GTT | (chunk * 2);
		bb->cs[bb->len++] = ggtt_ofs;

		ofs += chunk << GEN8_PTE_SHIFT;
		ggtt_ofs += chunk << GEN8_PTE_SHIFT;
		ptes -= chunk;

		while (chunk--) {
			u64 addr;

			if (lmem) {
				addr = cur->start;
				addr |= 3;
			} else {
				u32 ofs = cur->start >> GEN8_PTE_SHIFT;

				addr = ttm->dma_address[ofs];
				addr |= 1;
			}

			bb->cs[bb->len++] = lower_32_bits(addr);
			bb->cs[bb->len++] = upper_32_bits(addr);

			xe_res_next(cur, GEN8_PAGE_SIZE);
		}
	}
}

static void emit_flush(struct xe_bb *bb)
{
	bb->cs[bb->len++] = (MI_FLUSH_DW | MI_INVALIDATE_TLB | MI_FLUSH_DW_OP_STOREDW | MI_FLUSH_DW_STORE_INDEX) + 1;
	bb->cs[bb->len++] = LRC_PPHWSP_SCRATCH_ADDR | MI_FLUSH_DW_USE_GTT; /* lower_32_bits(addr) */
	bb->cs[bb->len++] = 0; /* upper_32_bits(addr) */
	bb->cs[bb->len++] = 0; /* value */
}

static void emit_copy(struct xe_gt *gt, struct xe_bb *bb,
		      u64 src_ofs, u64 dst_ofs, unsigned int size)
{
	bb->cs[bb->len++] = GEN9_XY_FAST_COPY_BLT_CMD | (10 - 2);
	bb->cs[bb->len++] = BLT_DEPTH_32 | GEN8_PAGE_SIZE;
	bb->cs[bb->len++] = 0;
	bb->cs[bb->len++] = (size >> GEN8_PTE_SHIFT) << 16 | GEN8_PAGE_SIZE / 4;
	bb->cs[bb->len++] = lower_32_bits(dst_ofs);
	bb->cs[bb->len++] = upper_32_bits(dst_ofs);
	bb->cs[bb->len++] = 0;
	bb->cs[bb->len++] = GEN8_PAGE_SIZE;
	bb->cs[bb->len++] = lower_32_bits(src_ofs);
	bb->cs[bb->len++] = upper_32_bits(src_ofs);
}

struct dma_fence *xe_migrate_copy(struct xe_migrate *m,
				  struct xe_bo *bo,
				  struct ttm_resource *src,
				  struct ttm_resource *dst)
{
	struct xe_gt *gt = m->gt;
	struct dma_fence *fence = NULL;
	u32 size = bo->size;
	u32 ofs = 0;
	u64 ggtt_copy_ofs = m->copy_node.start;
	struct xe_res_cursor src_it, dst_it;
	struct ttm_tt *ttm = bo->ttm.ttm;
	int err;

	err = dma_resv_reserve_fences(bo->ttm.base.resv, 1);
	if (err)
		return ERR_PTR(err);

	xe_res_first(src, 0, bo->size, &src_it);
	xe_res_first(dst, 0, bo->size, &dst_it);

	while (size) {
		u32 copy = min_t(u32, CHUNK_SZ, size);
		u32 batch_size = 16 + 2 * MAX_GGTT_UPDATE_SIZE;
		struct xe_sched_job *job;
		struct xe_bb *bb;
		int err;

		dma_fence_put(fence);

		bb = xe_bb_new(gt, batch_size);
		if (IS_ERR(bb))
			return ERR_CAST(bb);

		emit_arb_clear(bb);
		emit_pte(gt->mem.ggtt, bb, ggtt_copy_ofs, src, &src_it, ofs, copy, ttm);
		emit_pte(gt->mem.ggtt, bb, ggtt_copy_ofs + CHUNK_SZ, dst, &dst_it, ofs, copy, ttm);
		emit_flush(bb);
		emit_copy(gt, bb, ggtt_copy_ofs, ggtt_copy_ofs + CHUNK_SZ, copy);

		mutex_lock(&m->job_mutex);

		job = xe_bb_create_job(m->eng, bb);
		if (IS_ERR(job)) {
			err = PTR_ERR(job);
			goto err;
		}

		if (!fence) {
			err = drm_sched_job_add_dependencies_resv(&job->drm,
								  bo->ttm.base.resv,
								  DMA_RESV_USAGE_PREEMPT_FENCE);
			if (err)
				goto err_job;
		}

		xe_sched_job_arm(job);
		fence = dma_fence_get(&job->drm.s_fence->finished);
		xe_sched_job_push(job);

		mutex_unlock(&m->job_mutex);

		xe_bb_free(bb, fence);
		ofs += copy;
		size -= copy;
		continue;

err_job:
		xe_sched_job_free(job);
err:
		mutex_unlock(&m->job_mutex);
		xe_bb_free(bb, NULL);
		return ERR_PTR(err);
	}

	dma_resv_add_fence(bo->ttm.base.resv, fence,
			   DMA_RESV_USAGE_KERNEL);

	return fence;
}

static int emit_clear(struct xe_bb *bb, u64 src_ofs, u32 size, u32 value)
{
	BUG_ON(size >> GEN8_PTE_SHIFT > S16_MAX);

	bb->cs[bb->len++] = XY_COLOR_BLT_CMD | BLT_WRITE_RGBA | (7 - 2);
	bb->cs[bb->len++] = BLT_DEPTH_32 | BLT_ROP_COLOR_COPY | GEN8_PAGE_SIZE;
	bb->cs[bb->len++] = 0;
	bb->cs[bb->len++] = (size >> GEN8_PTE_SHIFT) << 16 | GEN8_PAGE_SIZE / 4;
	bb->cs[bb->len++] = lower_32_bits(src_ofs);
	bb->cs[bb->len++] = upper_32_bits(src_ofs);
	bb->cs[bb->len++] = value;

	return 0;
}

struct dma_fence *xe_migrate_clear(struct xe_migrate *m,
				   struct xe_bo *bo,
				   u32 value)
{
	struct xe_gt *gt = m->gt;
	struct dma_fence *fence = NULL;
	u32 size = bo->size;
	u32 ofs = 0;
	u64 ggtt_copy_ofs = m->copy_node.start;
	struct xe_res_cursor src_it;
	struct ttm_resource *src = bo->ttm.resource;
	int err;

	err = dma_resv_reserve_fences(bo->ttm.base.resv, 1);
	if (err)
		return ERR_PTR(err);

	xe_res_first(src, 0, bo->size, &src_it);

	while (size) {
		u32 clear = min_t(u32, CHUNK_SZ, size);
		struct xe_sched_job *job;
		struct xe_bb *bb;
		int err;

		dma_fence_put(fence);

		bb = xe_bb_new(gt, 13 + MAX_GGTT_UPDATE_SIZE);
		if (IS_ERR(bb))
			return ERR_CAST(bb);

		/* TODO: Add dependencies here */
		emit_arb_clear(bb);
		emit_pte(gt->mem.ggtt, bb, ggtt_copy_ofs, src, &src_it, ofs, clear, bo->ttm.ttm);
		emit_flush(bb);
		emit_clear(bb, ggtt_copy_ofs, clear, value);

		mutex_lock(&m->job_mutex);
		job = xe_bb_create_job(m->eng, bb);
		if (IS_ERR(job)) {
			err = PTR_ERR(job);
			goto err;
		}

		if (!fence) {
			err = drm_sched_job_add_implicit_dependencies(&job->drm, &bo->ttm.base, true);
			if (err)
				goto err_job;
		}

		xe_sched_job_arm(job);
		fence = dma_fence_get(&job->drm.s_fence->finished);
		xe_sched_job_push(job);
		mutex_unlock(&m->job_mutex);

		xe_bb_free(bb, fence);

		ofs += clear;
		size -= clear;
		continue;

err_job:
		xe_sched_job_free(job);
		mutex_unlock(&m->job_mutex);
err:
		xe_bb_free(bb, NULL);
		return ERR_PTR(err);
	}

	dma_resv_add_fence(bo->ttm.base.resv, fence,
			   DMA_RESV_USAGE_KERNEL);

	return fence;
}

static void write_pgtable(struct xe_bb *bb, u64 ggtt_ofs,
			  struct xe_vm_pgtable_update *update,
			  xe_migrate_populatefn_t populatefn, void *arg)
{
	u32 chunk;
	u32 ofs = update->ofs, size = update->qwords;

	/*
	 * If we have 512 entries (max), we would populate it ourselves,
	 * and update the PDE above it to the new pointer.
	 * The only time this can only happen if we have to update the top
	 * PDE. This requires a BO that is almost vm->size big.
	 *
	 * This shouldn't be possible in practice.. might change when 16K
	 * pages are used. Hence the BUG_ON.
	 */
	XE_BUG_ON(update->qwords > 0x1ff);
	do {
		chunk = min(update->qwords, 0x1ffU);

		/* Ensure populatefn can do memset64 by aligning bb->cs */
		if (!(bb->len & 1))
			bb->cs[bb->len++] = MI_NOOP;

		bb->cs[bb->len++] = MI_STORE_DATA_IMM | BIT(22) | BIT(21) | (chunk * 2 + 1);
		bb->cs[bb->len++] = lower_32_bits(ggtt_ofs + ofs * 8);
		bb->cs[bb->len++] = upper_32_bits(ggtt_ofs + ofs * 8);
		populatefn(bb->cs + bb->len, ofs, chunk, update, arg);

		bb->len += chunk * 2;
		ofs += chunk;
		size -= chunk;
	} while (size);
}

static struct dma_fence *
xe_migrate_update_pgtables_cpu(struct xe_migrate *m,
			       struct xe_vm *vm,
			       struct xe_bo *bo,
			       struct xe_engine *eng,
			       struct xe_vm_pgtable_update *updates, u32 num_updates,
			       struct xe_sync_entry *syncs, u32 num_syncs,
			       xe_migrate_populatefn_t populatefn,
			       void *arg)
{
	int err = 0;
	u32 i, j;
	struct ttm_bo_kmap_obj maps[9];

	BUG_ON(num_updates > ARRAY_SIZE(maps));

	for (i = 0; i < num_syncs; i++) {
		err = xe_sync_entry_wait(&syncs[i]);
		if (err)
			return ERR_PTR(err);
	}

	if (bo) {
		long wait;

		wait = dma_resv_wait_timeout(bo->ttm.base.resv,
					     DMA_RESV_USAGE_KERNEL,
					     true, MAX_SCHEDULE_TIMEOUT);
		if (wait <= 0)
			return ERR_PTR(-ETIME);
	}

	for (i = 0; i < num_updates; i++) {
		err = ttm_bo_kmap(&updates[i].pt_bo->ttm, 0,
				  updates[i].pt_bo->size / GEN8_PAGE_SIZE, &maps[i]);
		if (err)
			goto unmap;
	}

	for (i = 0; i < num_updates; i++) {
		bool is_iomem;
		struct xe_vm_pgtable_update *update = &updates[i];
		u64 *map_u64 = ttm_kmap_obj_virtual(&maps[i], &is_iomem);

		if (is_iomem) {
			for (j = 0; j < update->qwords; j++) {
				u64 val;

				populatefn(&val, j + update->ofs, 1, update, arg);
				writeq(val, (u64 __iomem *)&map_u64[j + update->ofs]);
			}
		} else {
			populatefn(&map_u64[update->ofs], update->ofs,
				   update->qwords, update, arg);
		}
	}

unmap:
	while (i-- > 0)
		ttm_bo_kunmap(&maps[i]);

	if (err)
		return ERR_PTR(err);

	return dma_fence_get_stub();
}


struct dma_fence *
xe_migrate_update_pgtables(struct xe_migrate *m,
			   struct xe_vm *vm,
			   struct xe_bo *bo,
			   struct xe_engine *eng,
			   struct xe_vm_pgtable_update *updates,
			   u32 num_updates,
			   struct xe_sync_entry *syncs, u32 num_syncs,
			   xe_migrate_populatefn_t populatefn, void *arg)
{
	struct xe_gt *gt = m->gt;
	struct xe_sched_job *job;
	struct dma_fence *fence;
	struct drm_suballoc *sa_bo = NULL;
	struct xe_bb *bb;
	u32 i, batch_size;
	u64 ggtt_ofs;
	int err = 0;

	if (gt_to_xe(gt)->info.platform == XE_DG2) {
		fence = xe_migrate_update_pgtables_cpu(m, vm, bo, eng, updates,
						       num_updates, syncs,
						       num_syncs, populatefn,
						       arg);
		if (IS_ERR(fence))
			return fence;

		for (i = 0; i < num_syncs; ++i)
			xe_sync_entry_signal(&syncs[i], NULL, fence);

		return fence;
	}

	/* fixed + PTE entries */
	batch_size = 7;

	for (i = 0; i < num_updates; i++) {
		u32 num_cmds = DIV_ROUND_UP(updates[i].qwords, 0x1ff);

		batch_size += 2;
		/* align noops + MI_STORE_DATA_IMM cmd prefix */
		batch_size += 2 + 4 * num_cmds + updates[i].qwords * 2;
	}

	/*
	 * XXX: Create temp bo to copy from, if batch_size becomes too big?
	 *
	 * Worst case: Sum(2 * (each lower level page size) + (top level page size))
	 * Should be reasonably bound..
	 */
	XE_BUG_ON(batch_size >= SZ_128K);

	ggtt_ofs = m->copy_node.start;
	if (eng) {
		sa_bo = drm_suballoc_new(&m->vm_update_sa, num_updates * GEN8_PAGE_SIZE);
		if (IS_ERR(sa_bo))
			return ERR_CAST(sa_bo);

		ggtt_ofs += 2 * CHUNK_SZ + sa_bo->soffset;
	}

	bb = xe_bb_new(gt, batch_size);
	if (IS_ERR(bb)) {
		err = PTR_ERR(bb);
		goto err;
	}

	emit_arb_clear(bb);

	/* Map our PT's to gtt */
	bb->cs[bb->len++] = MI_UPDATE_GTT | (num_updates * 2);
	bb->cs[bb->len++] = ggtt_ofs;

	for (i = 0; i < num_updates; i++) {
		struct xe_bo *bo = updates[i].pt_bo;
		u64 addr;

		BUG_ON(bo->size != SZ_4K);

		if (bo->ttm.resource->mem_type == TTM_PL_VRAM) {
			struct xe_res_cursor src_it;

			xe_res_first(bo->ttm.resource, 0, bo->size, &src_it);
			addr = src_it.start | 3;
		} else {
			addr = bo->ttm.ttm->dma_address[0] | 1;
		}

		bb->cs[bb->len++] = lower_32_bits(addr);
		bb->cs[bb->len++] = upper_32_bits(addr);
	}

	emit_flush(bb);

	for (i = 0; i < num_updates; i++) {
		write_pgtable(bb, ggtt_ofs + i * GEN8_PAGE_SIZE, &updates[i], populatefn, arg);
	}

	if (!eng)
		mutex_lock(&m->job_mutex);

	job = xe_bb_create_job(eng ?: m->eng, bb);
	if (IS_ERR(job)) {
		err = PTR_ERR(job);
		goto err_bb;
	}

	/* Wait on BO move */
	if (bo) {
		err = drm_sched_job_add_dependencies_resv(&job->drm,
							  bo->ttm.base.resv,
							  DMA_RESV_USAGE_KERNEL);
		if (err)
			goto err_job;
	}

	for (i = 0; !err && i < num_syncs; i++)
		err = xe_sync_entry_add_deps(&syncs[i], job);

	if (err)
		goto err_job;

	xe_sched_job_arm(job);
	fence = dma_fence_get(&job->drm.s_fence->finished);
	xe_sched_job_push(job);

	if (!eng)
		mutex_unlock(&m->job_mutex);

	for (i = 0; i < num_syncs; i++)
		xe_sync_entry_signal(&syncs[i], job, fence);

	xe_bb_free(bb, fence);
	drm_suballoc_free(sa_bo, fence, -1);

	return fence;

err_job:
	xe_sched_job_free(job);
err_bb:
	if (!eng)
		mutex_unlock(&m->job_mutex);
	xe_bb_free(bb, NULL);
err:
	drm_suballoc_free(sa_bo, NULL, 0);
	return ERR_PTR(err);
}
