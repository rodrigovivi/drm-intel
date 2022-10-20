// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <linux/bitfield.h>
#include <linux/bitmap.h>
#include <linux/circ_buf.h>
#include <linux/delay.h>
#include <linux/dma-fence-array.h>
#include <linux/kthread.h>

#include <drm/drm_managed.h>

#include "xe_device.h"
#include "xe_engine.h"
#include "xe_guc.h"
#include "xe_guc_ct.h"
#include "xe_guc_engine_types.h"
#include "xe_guc_submit.h"
#include "xe_gt.h"
#include "xe_force_wake.h"
#include "xe_hw_fence.h"
#include "xe_lrc.h"
#include "xe_macros.h"
#include "xe_ring_ops_types.h"
#include "xe_sched_job.h"
#include "xe_trace.h"

#include "../i915/gt/intel_lrc_reg.h"

static struct xe_gt *
guc_to_gt(struct xe_guc *guc)
{
	return container_of(guc, struct xe_gt, uc.guc);
}

static struct xe_device *
guc_to_xe(struct xe_guc *guc)
{
	return gt_to_xe(guc_to_gt(guc));
}

static struct xe_guc *
engine_to_guc(struct xe_engine *e)
{
	return &e->gt->uc.guc;
}

/*
 * Helpers for engine state, no lock required as transitions are mutually
 * exclusive.
 */
#define ENGINE_STATE_REGISTERED		BIT(0)
#define ENGINE_STATE_ENABLED		BIT(1)
#define ENGINE_STATE_PENDING_ENABLE	BIT(2)
#define ENGINE_STATE_PENDING_DISABLE	BIT(3)
#define ENGINE_STATE_DESTROYED		BIT(4)
#define ENGINE_STATE_USED		BIT(5)

static bool engine_registered(struct xe_engine *e)
{
	return (e->guc->state & ENGINE_STATE_REGISTERED);
}

static void set_engine_registered(struct xe_engine *e)
{
	e->guc->state |= ENGINE_STATE_REGISTERED;
}

static bool engine_enabled(struct xe_engine *e)
{
	return (e->guc->state & ENGINE_STATE_ENABLED);
}

static void set_engine_enabled(struct xe_engine *e)
{
	e->guc->state |= ENGINE_STATE_ENABLED;
}

static void clear_engine_enabled(struct xe_engine *e)
{
	e->guc->state &= ~ENGINE_STATE_ENABLED;
}

static bool engine_pending_enable(struct xe_engine *e)
{
	return (e->guc->state & ENGINE_STATE_PENDING_ENABLE);
}

static void set_engine_pending_enable(struct xe_engine *e)
{
	e->guc->state |= ENGINE_STATE_PENDING_ENABLE;
}

static void clear_engine_pending_enable(struct xe_engine *e)
{
	e->guc->state &= ~ENGINE_STATE_PENDING_ENABLE;
}

static bool engine_pending_disable(struct xe_engine *e)
{
	return (e->guc->state & ENGINE_STATE_PENDING_DISABLE);
}

static void set_engine_pending_disable(struct xe_engine *e)
{
	e->guc->state |= ENGINE_STATE_PENDING_DISABLE;
}

static void clear_engine_pending_disable(struct xe_engine *e)
{
	e->guc->state &= ~ENGINE_STATE_PENDING_DISABLE;
}

static bool engine_destroyed(struct xe_engine *e)
{
	return (e->guc->state & ENGINE_STATE_DESTROYED);
}

static void set_engine_destroyed(struct xe_engine *e)
{
	e->guc->state |= ENGINE_STATE_DESTROYED;
}

static bool engine_banned(struct xe_engine *e)
{
	return (e->flags & ENGINE_FLAG_BANNED);
}

static void set_engine_banned(struct xe_engine *e)
{
	e->flags |= ENGINE_FLAG_BANNED;
}

static bool engine_used(struct xe_engine *e)
{
	return (e->guc->state & ENGINE_STATE_USED);
}

static void set_engine_used(struct xe_engine *e)
{
	e->guc->state |= ENGINE_STATE_USED;
}

static bool engine_reset(struct xe_engine *e)
{
	return e->guc->reset;
}

static void set_engine_reset(struct xe_engine *e)
{
	e->guc->reset = true;
}

static bool engine_killed(struct xe_engine *e)
{
	return e->guc->killed;
}

static void set_engine_killed(struct xe_engine *e)
{
	e->guc->killed = true;
}

static void guc_submit_fini(struct drm_device *drm, void *arg)
{
	struct xe_guc *guc = arg;

	xa_destroy(&guc->submission_state.engine_lookup);
	ida_destroy(&guc->submission_state.guc_ids);
	bitmap_free(guc->submission_state.guc_ids_bitmap);
}

static void primelockdep(struct xe_guc *guc)

{
#if IS_ENABLED(CONFIG_LOCKDEP)
	bool cookie = dma_fence_begin_signalling();

	mutex_lock(&guc->submission_state.lock);
	mutex_unlock(&guc->submission_state.lock);

	dma_fence_end_signalling(cookie);
#endif
}

static int guc_engine_init(struct xe_engine *e);
static void guc_engine_kill(struct xe_engine *e);
static void guc_engine_fini(struct xe_engine *e);

static const struct xe_engine_ops guc_engine_ops = {
	.init = guc_engine_init,
	.kill = guc_engine_kill,
	.fini = guc_engine_fini,
};

#define GUC_ID_MAX		65535
#define GUC_ID_NUMBER_MLRC	4096
#define GUC_ID_NUMBER_SLRC	(GUC_ID_MAX - GUC_ID_NUMBER_MLRC)
#define GUC_ID_START_MLRC	GUC_ID_NUMBER_SLRC

int xe_guc_submit_init(struct xe_guc *guc)
{
	struct xe_device *xe = guc_to_xe(guc);
	struct xe_gt *gt = guc_to_gt(guc);
	int err;

	guc->submission_state.guc_ids_bitmap =
		bitmap_zalloc(GUC_ID_NUMBER_MLRC, GFP_KERNEL);
	if (!guc->submission_state.guc_ids_bitmap)
		return -ENOMEM;

	gt->engine_ops = &guc_engine_ops;

	mutex_init(&guc->submission_state.lock);
	xa_init(&guc->submission_state.engine_lookup);
	ida_init(&guc->submission_state.guc_ids);

	primelockdep(guc);

	err = drmm_add_action_or_reset(&xe->drm, guc_submit_fini, guc);
	if (err)
		return err;

	return 0;
}

static int alloc_guc_id(struct xe_guc *guc, struct xe_engine *e)
{
	int ret;
	void *ptr;

	/*
	 * Must use GFP_NOWAIT as this lock is in the dma fence signalling path,
	 * worse case user gets -ENOMEM on engine create and has to try again.
	 *
	 * FIXME: Have caller pre-alloc or post-alloc /w GFP_KERNEL to prevent
	 * failure.
	 */
	lockdep_assert_held(&guc->submission_state.lock);

	if (xe_engine_is_parallel(e))
		ret = bitmap_find_free_region(guc->submission_state.guc_ids_bitmap,
					      GUC_ID_NUMBER_MLRC,
					      order_base_2(e->width));
	else
		ret = ida_simple_get(&guc->submission_state.guc_ids, 0,
				     GUC_ID_NUMBER_SLRC, GFP_NOWAIT);
	if (ret < 0)
		return ret;

	e->guc->id = ret;
	if (xe_engine_is_parallel(e))
		e->guc->id += GUC_ID_START_MLRC;

	ptr = xa_store(&guc->submission_state.engine_lookup,
		       e->guc->id, e, GFP_NOWAIT);
	if (IS_ERR(ptr)) {
		ret = PTR_ERR(ptr);
		goto err_release;
	}

	return 0;

err_release:
	ida_simple_remove(&guc->submission_state.guc_ids, e->guc->id);
	return ret;
}

static void release_guc_id(struct xe_guc *guc, struct xe_engine *e)
{
	mutex_lock(&guc->submission_state.lock);
	xa_erase(&guc->submission_state.engine_lookup, e->guc->id);
	if (xe_engine_is_parallel(e))
		bitmap_release_region(guc->submission_state.guc_ids_bitmap,
				      e->guc->id, order_base_2(e->width));
	else
		ida_simple_remove(&guc->submission_state.guc_ids, e->guc->id);
	mutex_unlock(&guc->submission_state.lock);
}

struct engine_policy {
	u32 count;
	struct guc_update_engine_policy h2g;
};

static u32 __guc_engine_policy_action_size(struct engine_policy *policy)
{
	size_t bytes = sizeof(policy->h2g.header) +
		       (sizeof(policy->h2g.klv[0]) * policy->count);

	return bytes / sizeof(u32);
}

static void __guc_engine_policy_start_klv(struct engine_policy *policy,
					  u16 guc_id)
{
	policy->h2g.header.action =
		XE_GUC_ACTION_HOST2GUC_UPDATE_CONTEXT_POLICIES;
	policy->h2g.header.guc_id = guc_id;
	policy->count = 0;
}

#define MAKE_ENGINE_POLICY_ADD(func, id) \
static void __guc_engine_policy_add_##func(struct engine_policy *policy, \
					   u32 data) \
{ \
	XE_BUG_ON(policy->count >= GUC_CONTEXT_POLICIES_KLV_NUM_IDS); \
 \
	policy->h2g.klv[policy->count].kl = \
		FIELD_PREP(GUC_KLV_0_KEY, \
			   GUC_CONTEXT_POLICIES_KLV_ID_##id) | \
		FIELD_PREP(GUC_KLV_0_LEN, 1); \
	policy->h2g.klv[policy->count].value = data; \
	policy->count++; \
}

MAKE_ENGINE_POLICY_ADD(execution_quantum, EXECUTION_QUANTUM)
MAKE_ENGINE_POLICY_ADD(preemption_timeout, PREEMPTION_TIMEOUT)
MAKE_ENGINE_POLICY_ADD(priority, SCHEDULING_PRIORITY)
#undef MAKE_ENGINE_POLICY_ADD

static void init_policies(struct xe_guc *guc, struct xe_engine *e)
{
        struct engine_policy policy;

	/* FIXME: Wire these up so they can be configured */
        __guc_engine_policy_start_klv(&policy, e->guc->id);
        __guc_engine_policy_add_priority(&policy, DRM_SCHED_PRIORITY_NORMAL);
        __guc_engine_policy_add_execution_quantum(&policy, 1 * 1000);
        __guc_engine_policy_add_preemption_timeout(&policy, 640 * 1000);

	xe_guc_ct_send(&guc->ct, (u32 *)&policy.h2g,
		       __guc_engine_policy_action_size(&policy), 0, 0);
}

static void set_min_preemption_timeout(struct xe_guc *guc, struct xe_engine *e)
{
	struct engine_policy policy;

        __guc_engine_policy_start_klv(&policy, e->guc->id);
        __guc_engine_policy_add_preemption_timeout(&policy, 1);

	xe_guc_ct_send(&guc->ct, (u32 *)&policy.h2g,
		       __guc_engine_policy_action_size(&policy), 0, 0);
}

/* FIXME: Move to helper */
static u8 engine_class_to_guc_class(enum xe_engine_class class)
{
	switch (class) {
	case XE_ENGINE_CLASS_RENDER:
		return GUC_RENDER_CLASS;
	case XE_ENGINE_CLASS_VIDEO_DECODE:
		return GUC_VIDEO_CLASS;
	case XE_ENGINE_CLASS_VIDEO_ENHANCE:
		return GUC_VIDEOENHANCE_CLASS;
	case XE_ENGINE_CLASS_COPY:
		return GUC_BLITTER_CLASS;
	case XE_ENGINE_CLASS_OTHER:
	case XE_ENGINE_CLASS_COMPUTE:
	default:
		XE_WARN_ON(class);
		return -1;
	}
}

#define PARALLEL_SCRATCH_SIZE	2048
#define WQ_SIZE			(PARALLEL_SCRATCH_SIZE / 2)
#define WQ_OFFSET		(PARALLEL_SCRATCH_SIZE - WQ_SIZE)
#define CACHELINE_BYTES		64

struct sync_semaphore {
	u32 semaphore;
	u8 unused[CACHELINE_BYTES - sizeof(u32)];
};

struct parallel_scratch {
	struct guc_sched_wq_desc wq_desc;

	struct sync_semaphore go;
	struct sync_semaphore join[XE_HW_ENGINE_MAX_INSTANCE];

	u8 unused[WQ_OFFSET - sizeof(struct guc_sched_wq_desc) -
		sizeof(struct sync_semaphore) * (XE_HW_ENGINE_MAX_INSTANCE + 1)];

	u32 wq[WQ_SIZE / sizeof(u32)];
};

#define parallel_read(map_, field_)				\
	dma_buf_map_read_field(&map_, struct parallel_scratch,	\
			       field_)
#define parallel_write(map_, field_, val_)			\
	dma_buf_map_write_field(&map_, struct parallel_scratch,	\
				field_, val_)

static void __register_mlrc_engine(struct xe_guc *guc,
				   struct xe_engine *e,
				   struct guc_ctxt_registration_info *info)
{
#define MAX_MLRC_REG_SIZE      (11 + XE_HW_ENGINE_MAX_INSTANCE * 2)
	u32 action [MAX_MLRC_REG_SIZE];
	int len = 0;
	int i;

	XE_BUG_ON(!xe_engine_is_parallel(e));

	action[len++] = XE_GUC_ACTION_REGISTER_CONTEXT_MULTI_LRC;
	action[len++] = info->flags;
	action[len++] = info->context_idx;
	action[len++] = info->engine_class;
	action[len++] = info->engine_submit_mask;
	action[len++] = info->wq_desc_lo;
	action[len++] = info->wq_desc_hi;
	action[len++] = info->wq_base_lo;
	action[len++] = info->wq_base_hi;
	action[len++] = info->wq_size;
	action[len++] = e->width;
	action[len++] = info->hwlrca_lo;
	action[len++] = info->hwlrca_hi;

	for (i = 1; i < e->width; ++i) {
		struct xe_lrc *lrc = e->lrc + i;

		action[len++] = lower_32_bits(xe_lrc_descriptor(lrc));
		action[len++] = upper_32_bits(xe_lrc_descriptor(lrc));
	}

	XE_BUG_ON(len > MAX_MLRC_REG_SIZE);
#undef MAX_MLRC_REG_SIZE

	xe_guc_ct_send(&guc->ct, action, len, 0, 0);
}

static void __register_engine(struct xe_guc *guc,
			      struct guc_ctxt_registration_info *info)
{
	u32 action[] = {
		XE_GUC_ACTION_REGISTER_CONTEXT,
		info->flags,
		info->context_idx,
		info->engine_class,
		info->engine_submit_mask,
		info->wq_desc_lo,
		info->wq_desc_hi,
		info->wq_base_lo,
		info->wq_base_hi,
		info->wq_size,
		info->hwlrca_lo,
		info->hwlrca_hi,
	};

	xe_guc_ct_send(&guc->ct, action, ARRAY_SIZE(action), 0, 0);
}

static void register_engine(struct xe_engine *e)
{
	struct xe_guc *guc = engine_to_guc(e);
	struct xe_lrc *lrc = e->lrc;
	struct guc_ctxt_registration_info info;

	XE_BUG_ON(engine_registered(e));

	memset(&info, 0, sizeof(info));
	info.context_idx = e->guc->id;
	info.engine_class = engine_class_to_guc_class(e->class);
	info.engine_submit_mask = e->logical_mask;
	info.hwlrca_lo = lower_32_bits(xe_lrc_descriptor(lrc));
	info.hwlrca_hi = upper_32_bits(xe_lrc_descriptor(lrc));
	info.flags = CONTEXT_REGISTRATION_FLAG_KMD;

	if (xe_engine_is_parallel(e)) {
		u32 ggtt_addr = xe_lrc_parallel_ggtt_addr(lrc);
		struct dma_buf_map map = xe_lrc_parallel_map(lrc);

		info.wq_desc_lo = lower_32_bits(ggtt_addr +
			offsetof(struct parallel_scratch, wq_desc));
		info.wq_desc_hi = upper_32_bits(ggtt_addr +
			offsetof(struct parallel_scratch, wq_desc));
		info.wq_base_lo = lower_32_bits(ggtt_addr +
			offsetof(struct parallel_scratch, wq[0]));
		info.wq_base_hi = upper_32_bits(ggtt_addr +
			offsetof(struct parallel_scratch, wq[0]));
		info.wq_size = WQ_SIZE;

		e->guc->wqi_head = 0;
		e->guc->wqi_tail = 0;
		dma_buf_map_memset(&map, 0, PARALLEL_SCRATCH_SIZE - WQ_SIZE);
		parallel_write(map, wq_desc.wq_status, WQ_STATUS_ACTIVE);
	}

	set_engine_registered(e);
	trace_xe_engine_register(e);
	if (xe_engine_is_parallel(e))
		__register_mlrc_engine(guc, e, &info);
	else
		__register_engine(guc, &info);
	init_policies(guc, e);
}

static u32 wq_space_until_wrap(struct xe_engine *e)
{
	return (WQ_SIZE - e->guc->wqi_tail);
}

static int wq_wait_for_space(struct xe_engine *e, u32 wqi_size)
{
	struct dma_buf_map map = xe_lrc_parallel_map(e->lrc);
	unsigned int sleep_period_ms = 1;

#define AVAILABLE_SPACE \
	CIRC_SPACE(e->guc->wqi_tail, e->guc->wqi_head, WQ_SIZE)
	if (wqi_size > AVAILABLE_SPACE) {
try_again:
		e->guc->wqi_head = parallel_read(map, wq_desc.head);
		if (wqi_size > AVAILABLE_SPACE) {
			if (sleep_period_ms == 1024) {
				xe_gt_reset_async(e->gt);
				return -ENODEV;
			}

			msleep(sleep_period_ms);
			sleep_period_ms <<= 1;
			goto try_again;
		}
	}
#undef AVAILABLE_SPACE

	return 0;
}

static int wq_noop_append(struct xe_engine *e)
{
	struct dma_buf_map map = xe_lrc_parallel_map(e->lrc);
	u32 len_dw = wq_space_until_wrap(e) / sizeof(u32) - 1;

	if (wq_wait_for_space(e, wq_space_until_wrap(e)))
		return -ENODEV;

	XE_BUG_ON(!FIELD_FIT(WQ_LEN_MASK, len_dw));

	parallel_write(map, wq[e->guc->wqi_tail / sizeof(u32)],
		       FIELD_PREP(WQ_TYPE_MASK, WQ_TYPE_NOOP) |
		       FIELD_PREP(WQ_LEN_MASK, len_dw));
	e->guc->wqi_tail = 0;

	return 0;
}

static void wq_item_append(struct xe_engine *e)
{
	struct dma_buf_map map = xe_lrc_parallel_map(e->lrc);
	u32 wqi[XE_HW_ENGINE_MAX_INSTANCE + 3];
	u32 wqi_size = (e->width + 3) * sizeof(u32);
	u32 len_dw = (wqi_size / sizeof(u32)) - 1;
	int i = 0, j;

	if (wqi_size > wq_space_until_wrap(e)) {
		if (wq_noop_append(e))
			return;
	}
	if (wq_wait_for_space(e, wqi_size))
		return;

	wqi[i++] = FIELD_PREP(WQ_TYPE_MASK, WQ_TYPE_MULTI_LRC) |
		FIELD_PREP(WQ_LEN_MASK, len_dw);
	wqi[i++] = xe_lrc_descriptor(e->lrc);
	wqi[i++] = FIELD_PREP(WQ_GUC_ID_MASK, e->guc->id) |
		FIELD_PREP(WQ_RING_TAIL_MASK, e->lrc->ring.tail / sizeof(u64));
	wqi[i++] = 0;
	for (j = 1; j < e->width; ++j) {
		struct xe_lrc *lrc = e->lrc + j;

		wqi[i++] = lrc->ring.tail / sizeof(u64);
	}

	XE_BUG_ON(i != wqi_size / sizeof(u32));

	dma_buf_map_incr(&map, offsetof(struct parallel_scratch,
					wq[e->guc->wqi_tail / sizeof(u32)]));
	dma_buf_map_memcpy_to(&map, wqi, wqi_size);
	e->guc->wqi_tail += wqi_size;
	XE_BUG_ON(e->guc->wqi_tail > WQ_SIZE);

	xe_guc_wb(engine_to_guc(e));

	map = xe_lrc_parallel_map(e->lrc);
	parallel_write(map, wq_desc.tail, e->guc->wqi_tail);
}

static void submit_engine(struct xe_engine *e)
{
	struct xe_guc *guc = engine_to_guc(e);
	struct xe_lrc *lrc = e->lrc;
	u32 action[3];
	u32 g2h_len = 0;
	u32 num_g2h = 0;
	int len = 0;
	bool extra_submit = false;

	XE_BUG_ON(!engine_registered(e));

	if (xe_engine_is_parallel(e))
		wq_item_append(e);
	else
		xe_lrc_write_ctx_reg(lrc, CTX_RING_TAIL, lrc->ring.tail);

	if (!engine_enabled(e)) {
		action[len++] = XE_GUC_ACTION_SCHED_CONTEXT_MODE_SET;
		action[len++] = e->guc->id;
		action[len++] = GUC_CONTEXT_ENABLE;
		g2h_len = G2H_LEN_DW_SCHED_CONTEXT_MODE_SET;
		num_g2h = 1;
		if (xe_engine_is_parallel(e))
			extra_submit = true;

		set_engine_pending_enable(e);
		set_engine_enabled(e);
		set_engine_used(e);
		trace_xe_engine_scheduling_enable(e);
	} else {
		action[len++] = XE_GUC_ACTION_SCHED_CONTEXT;
		action[len++] = e->guc->id;
	}

	XE_BUG_ON(!engine_enabled(e));

	xe_guc_ct_send(&guc->ct, action, len, g2h_len, num_g2h);

	if (extra_submit) {
		len = 0;
		action[len++] = XE_GUC_ACTION_SCHED_CONTEXT;
		action[len++] = e->guc->id;

		xe_guc_ct_send(&guc->ct, action, len, 0, 0);
	}
}

static struct dma_fence *
guc_engine_run_job(struct drm_sched_job *drm_job)
{
	struct xe_sched_job *job = to_xe_sched_job(drm_job);
	struct xe_engine *e = job->engine;

	XE_BUG_ON((engine_destroyed(e) || engine_pending_disable(e)) &&
		  !engine_banned(e));

	trace_xe_sched_job_run(job);

	if (!engine_banned(e) && !engine_killed(e)) {
		if (!engine_registered(e))
			register_engine(e);
		e->ring_ops->emit_job(job);

		submit_engine(e);
	}

	return dma_fence_get(job->fence);
}

static void guc_engine_free_job(struct drm_sched_job *drm_job)
{
	struct xe_sched_job *job = to_xe_sched_job(drm_job);
	struct xe_engine *e = job->engine;

	trace_xe_sched_job_free(job);

	xe_sched_job_free(job);
	xe_engine_put(e);
}

static void disable_scheduling(struct xe_guc *guc, struct xe_engine *e)
{
	u32 action[] = {
		XE_GUC_ACTION_SCHED_CONTEXT_MODE_SET,
		e->guc->id,
		GUC_CONTEXT_DISABLE,
	};

	set_min_preemption_timeout(guc, e);
	wait_event(guc->ct.wq, !engine_pending_enable(e) ||
		   guc->submission_state.stopped);

	clear_engine_enabled(e);
	set_engine_pending_disable(e);
	set_engine_destroyed(e);
	trace_xe_engine_scheduling_disable(e);

	/*
	 * Reserve space for both G2H here as the 2nd G2H is sent from a G2H
	 * handler and we are not allowed to reserved G2H space in handlers.
	 */
	xe_guc_ct_send(&guc->ct, action, ARRAY_SIZE(action),
		       G2H_LEN_DW_SCHED_CONTEXT_MODE_SET +
		       G2H_LEN_DW_DEREGISTER_CONTEXT, 2);
}

#define MIN_SCHED_TIMEOUT	1

static enum drm_gpu_sched_stat
guc_engine_timedout_job(struct drm_sched_job *drm_job)
{
	struct xe_sched_job *job = to_xe_sched_job(drm_job);
	struct xe_sched_job *tmp_job;
	struct xe_engine *e = job->engine;
	struct drm_gpu_scheduler *sched = &e->guc->sched;
	struct xe_device *xe = guc_to_xe(engine_to_guc(e));
	int err = -ETIME;
	int i = 0;

	XE_WARN_ON(e->flags & ENGINE_FLAG_KERNEL);
	drm_warn(&xe->drm, "Timedout job: seqno=%u, guc_id=%d",
		 xe_sched_job_seqno(job), e->guc->id);
	trace_xe_sched_job_timedout(job);

	/* Kill the run_job entry point */
	kthread_park(sched->thread);

	/* Engine state now stable, disable scheduling if needed */
	if (engine_enabled(e)) {
		struct xe_guc *guc = engine_to_guc(e);

		if (engine_reset(e))
			err = -EIO;
		set_engine_banned(e);
		xe_engine_get(e);
		disable_scheduling(engine_to_guc(e), e);

		/*
		 * Must wait for scheduling to be disabled before signalling
		 * any fences, if GT broken the GT reset code should signal us.
		 */
		wait_event(guc->ct.wq, !engine_pending_disable(e) ||
			   guc->submission_state.stopped);
	}

	/*
	 * Fence state now stable, stop / start scheduler which cleans up any
	 * fences that are complete
	 */
	list_add(&drm_job->list, &sched->pending_list);
	kthread_unpark(sched->thread);
	drm_sched_set_timeout(&e->guc->sched, MIN_SCHED_TIMEOUT);

	/* Mark all outstanding fences as bad, thus completing them */
	spin_lock(&sched->job_list_lock);
	list_for_each_entry(tmp_job, &sched->pending_list, drm.list) {
		if (!i++)
			dma_fence_set_error(tmp_job->fence, err);
		else
			dma_fence_set_error(tmp_job->fence, -ECANCELED);

		if (dma_fence_is_array(tmp_job->fence)) {
			struct dma_fence_array *array =
				to_dma_fence_array(tmp_job->fence);
			struct dma_fence **child = array->fences;
			unsigned int nchild = array->num_fences;

			do {
				struct dma_fence *current_fence = *child++;

				dma_fence_set_error(current_fence, -ECANCELED);
			} while (--nchild);
		}
		trace_xe_sched_job_set_error(tmp_job);
	}
	spin_unlock(&sched->job_list_lock);

	/* Kick HW fence IRQ handler to signal fences */
	xe_hw_fence_irq_run(e->fence_irq);

	return DRM_GPU_SCHED_STAT_NOMINAL;
}

static void __guc_engine_fini_async(struct work_struct *w)
{
	struct xe_guc_engine *ge =
		container_of(w, struct xe_guc_engine, fini_async);
	struct xe_engine *e = ge->engine;
	struct xe_guc *guc = engine_to_guc(e);

	trace_xe_engine_destroy(e);

	if (e->flags & ENGINE_FLAG_PERSISTENT)
		xe_device_remove_persitent_engines(gt_to_xe(e->gt), e);
	release_guc_id(guc, e);
	drm_sched_entity_fini(&ge->entity);
	drm_sched_fini(&ge->sched);
	kfree(ge);

	xe_engine_fini(e);
}

static void guc_engine_fini_async(struct xe_engine *e)
{
	INIT_WORK(&e->guc->fini_async, __guc_engine_fini_async);
	queue_work(system_unbound_wq, &e->guc->fini_async);
}

static void __guc_engine_fini(struct xe_guc *guc, struct xe_engine *e)
{
	/*
	 * Might be done from within the GPU scheduler, need to do async as we
	 * fini the scheduler when the engine is fini'd, the scheduler can't
	 * complete fini within itself (circular dependency). Async resolves
	 * this we and don't really care when everything is fini'd, just that it
	 * is.
	 */
	guc_engine_fini_async(e);
}

static void guc_engine_cleanup_entity(struct drm_sched_entity *entity)
{
	struct xe_guc_engine *ge =
		container_of(entity, struct xe_guc_engine, entity);
	struct xe_engine *e = ge->engine;
	struct xe_guc *guc = engine_to_guc(e);

	XE_BUG_ON(!xe_gt_guc_submission_enabled(guc_to_gt(guc)));

	trace_xe_engine_cleanup_entity(e);

	if (engine_enabled(e))
		disable_scheduling(guc, e);
	else
		__guc_engine_fini(guc, e);
	entity->do_cleanup = false;
}

static const struct drm_sched_backend_ops drm_sched_ops = {
	.run_job = guc_engine_run_job,
	.free_job = guc_engine_free_job,
	.timedout_job = guc_engine_timedout_job,
	.cleanup_entity = guc_engine_cleanup_entity,
};

static int guc_engine_init(struct xe_engine *e)
{
	struct drm_gpu_scheduler *sched;
	struct xe_guc *guc = engine_to_guc(e);
	struct xe_guc_engine *ge;
	int err;

	XE_BUG_ON(!xe_gt_guc_submission_enabled(guc_to_gt(guc)));

	ge = kzalloc(sizeof(*ge), GFP_KERNEL);
	if (!ge)
		return -ENOMEM;

	e->guc = ge;
	ge->engine = e;

	err = drm_sched_init(&ge->sched, &drm_sched_ops,
			     e->lrc[0].ring.size / MAX_JOB_SIZE_BYTES,
			     64, HZ * 5, NULL, NULL, e->name);
	if (err)
		goto err_free;

	sched = &ge->sched;
	sched->tdr_skip_signalled = true;
	err = drm_sched_entity_init(&ge->entity, DRM_SCHED_PRIORITY_NORMAL,
				    &sched, 1, NULL);
	if (err)
		goto err_sched;

	mutex_lock(&guc->submission_state.lock);

	err = alloc_guc_id(guc, e);
	if (err)
		goto err_entity;

	e->entity = &ge->entity;

	if (guc->submission_state.stopped)
		drm_sched_stop(sched, NULL);

	mutex_unlock(&guc->submission_state.lock);

	switch (e->class) {
	case XE_ENGINE_CLASS_RENDER:
		sprintf(e->name, "rcs%d", e->guc->id);
		break;
	case XE_ENGINE_CLASS_VIDEO_DECODE:
		sprintf(e->name, "vcs%d", e->guc->id);
		break;
	case XE_ENGINE_CLASS_VIDEO_ENHANCE:
		sprintf(e->name, "vecs%d", e->guc->id);
		break;
	case XE_ENGINE_CLASS_COPY:
		sprintf(e->name, "bcs%d", e->guc->id);
		break;
	case XE_ENGINE_CLASS_COMPUTE:
		sprintf(e->name, "ccs%d", e->guc->id);
		break;
	default:
		XE_WARN_ON(e->class);
	}

	trace_xe_engine_create(e);

	return 0;

err_entity:
	drm_sched_entity_fini(&ge->entity);
err_sched:
	drm_sched_fini(&ge->sched);
err_free:
	kfree(ge);

	return err;
}

static void guc_engine_kill(struct xe_engine *e)
{
	set_engine_killed(e);
	drm_sched_set_timeout(&e->guc->sched, MIN_SCHED_TIMEOUT);
}

static void guc_engine_fini(struct xe_engine *e)
{
	if (engine_used(e))
		drm_sched_entity_trigger_cleanup(&e->guc->entity);
	else
		guc_engine_fini_async(e);
}

static void guc_engine_stop(struct xe_guc *guc, struct xe_engine *e)
{
	struct drm_gpu_scheduler *sched = &e->guc->sched;
	long timeout = sched->timeout;

	/* Stop scheduling + flush any DRM scheduler operations */
	timeout = sched->timeout;
	sched->timeout = MAX_SCHEDULE_TIMEOUT;
	wake_up_all(&guc->ct.wq);
	cancel_delayed_work_sync(&sched->work_tdr);
	kthread_park(sched->thread);
	sched->timeout = timeout;

	/* Clean up lost G2H + reset engine state */
	if (engine_destroyed(e)) {
		if (engine_banned(e))
			xe_engine_put(e);
		else
			__guc_engine_fini(guc, e);
	}
	e->guc->state = 0;
	trace_xe_engine_stop(e);

	/*
	 * Ban any engine (aside from kernel) with a started but not complete
	 * job or if a job has gone through a GT reset more than twice.
	 */
	if (!(e->flags & ENGINE_FLAG_KERNEL)) {
		struct drm_sched_job *drm_job =
			list_first_entry_or_null(&sched->pending_list,
						 struct drm_sched_job, list);

		if (drm_job) {
			struct xe_sched_job *job = to_xe_sched_job(drm_job);

			if ((xe_sched_job_started(job) &&
			    !xe_sched_job_completed(job)) ||
			    drm_sched_invalidate_job(drm_job, 2)) {
				trace_xe_sched_job_ban(job);
				sched->timeout = MIN_SCHED_TIMEOUT;
				set_engine_banned(e);
			}
		}
	}
}

int xe_guc_submit_stop(struct xe_guc *guc)
{
	struct xe_engine *e;
	unsigned long index;

	mutex_lock(&guc->submission_state.lock);

	guc->submission_state.stopped = true;
	smp_mb();
	xa_for_each(&guc->submission_state.engine_lookup, index, e)
		guc_engine_stop(guc, e);

	mutex_unlock(&guc->submission_state.lock);

	/*
	 * No one can enter the backend at this point, aside from new engine
	 * creation which is protected by guc->submission_state.lock.
	 */

	return 0;
}

static void guc_engine_start(struct xe_engine *e)
{
	struct drm_gpu_scheduler *sched = &e->guc->sched;

	if (!engine_banned(e) && !engine_killed(e)) {
		int i;

		trace_xe_engine_resubmit(e);
		for (i = 0; i < e->width; ++i)
			xe_lrc_set_ring_head(e->lrc + i, e->lrc[i].ring.tail);
		drm_sched_resubmit_jobs(sched);
	}

	kthread_unpark(sched->thread);
	drm_sched_set_timeout(sched, sched->timeout);
}

int xe_guc_submit_start(struct xe_guc *guc)
{
	struct xe_engine *e;
	unsigned long index;

	mutex_lock(&guc->submission_state.lock);

	guc->submission_state.stopped = false;
	xa_for_each(&guc->submission_state.engine_lookup, index, e)
		guc_engine_start(e);

	mutex_unlock(&guc->submission_state.lock);

	return 0;
}

static struct xe_engine *
g2h_engine_lookup(struct xe_guc *guc, u32 guc_id)
{
	struct xe_device *xe = guc_to_xe(guc);
	struct xe_engine *e;

	if (unlikely(guc_id >= GUC_ID_MAX)) {
		drm_err(&xe->drm, "Invalid guc_id %u", guc_id);
		return NULL;
	}

	e = xa_load(&guc->submission_state.engine_lookup, guc_id);
	if (unlikely(!e)) {
		drm_err(&xe->drm, "Not engine present for guc_id %u", guc_id);
		return NULL;
	}

	XE_BUG_ON(e->guc->id != guc_id);

	return e;
}

static void deregister_engine(struct xe_guc *guc, struct xe_engine *e)
{
	u32 action[] = {
		XE_GUC_ACTION_DEREGISTER_CONTEXT,
		e->guc->id,
	};

	trace_xe_engine_deregister(e);

	xe_guc_ct_send_g2h_handler(&guc->ct, action, ARRAY_SIZE(action));
}

int xe_guc_sched_done_handler(struct xe_guc *guc, u32 *msg, u32 len)
{
	struct xe_device *xe = guc_to_xe(guc);
	struct xe_engine *e;
	u32 guc_id = msg[0];

	XE_BUG_ON(guc->submission_state.stopped);

	if (unlikely(len < 2)) {
		drm_err(&xe->drm, "Invalid length %u", len);
		return -EPROTO;
	}

	e = g2h_engine_lookup(guc, guc_id);
	if (unlikely(!e))
		return -EPROTO;

	if (unlikely(!engine_pending_enable(e) &&
		     !engine_pending_disable(e))) {
		drm_err(&xe->drm, "Unexpected engine state 0x%04x",
			e->guc->state);
		return -EPROTO;
	}

	trace_xe_engine_scheduling_done(e);

	if (engine_pending_enable(e)) {
		clear_engine_pending_enable(e);
		smp_mb();
		wake_up_all(&guc->ct.wq);
	} else {
		clear_engine_pending_disable(e);
		if (engine_banned(e)) {
			smp_mb();
			wake_up_all(&guc->ct.wq);
		}
		deregister_engine(guc, e);
	}

	return 0;
}

int xe_guc_deregister_done_handler(struct xe_guc *guc, u32 *msg, u32 len)
{
	struct xe_device *xe = guc_to_xe(guc);
	struct xe_engine *e;
	u32 guc_id = msg[0];

	XE_BUG_ON(guc->submission_state.stopped);

	if (unlikely(len < 1)) {
		drm_err(&xe->drm, "Invalid length %u", len);
		return -EPROTO;
	}

	e = g2h_engine_lookup(guc, guc_id);
	if (unlikely(!e))
		return -EPROTO;

	if (!engine_destroyed(e) || engine_pending_disable(e) ||
	    engine_pending_enable(e) || engine_enabled(e)) {
		drm_err(&xe->drm, "Unexpected engine state 0x%04x",
			e->guc->state);
		return -EPROTO;
	}

	trace_xe_engine_deregister_done(e);

	if (engine_banned(e))
		xe_engine_put(e);
	else
		__guc_engine_fini(guc, e);

	return 0;
}

int xe_guc_engine_reset_handler(struct xe_guc *guc, u32 *msg, u32 len)
{
	struct xe_device *xe = guc_to_xe(guc);
	struct xe_engine *e;
	u32 guc_id = msg[0];

	XE_BUG_ON(guc->submission_state.stopped);

	if (unlikely(len < 1)) {
		drm_err(&xe->drm, "Invalid length %u", len);
		return -EPROTO;
	}

	e = g2h_engine_lookup(guc, guc_id);
	if (unlikely(!e))
		return -EPROTO;

	drm_warn(&xe->drm, "Engine reset: guc_id=%d", guc_id);

	/* FIXME: Do error capture, most likely async */

	trace_xe_engine_reset(e);

	/*
	 * A banned engine is a NOP at this point (came from
	 * guc_engine_timedout_job). Otherwise, kick drm scheduler to cancel
	 * jobs by setting timeout of the job to the minimum value kicking
	 * guc_engine_timedout_job.
	 */
	set_engine_reset(e);
	if (!engine_banned(e))
		drm_sched_set_timeout(&e->guc->sched, MIN_SCHED_TIMEOUT);

	return 0;
}

int xe_guc_engine_reset_failure_handler(struct xe_guc *guc, u32 *msg, u32 len)
{
	struct xe_device *xe = guc_to_xe(guc);
	u8 guc_class, instance;
	u32 reason;

	XE_BUG_ON(guc->submission_state.stopped);

	if (unlikely(len != 3)) {
		drm_err(&xe->drm, "Invalid length %u", len);
		return -EPROTO;
	}

	guc_class = msg[0];
	instance = msg[1];
	reason = msg[2];

	/* Unexpected failure of a hardware feature, log an actual error */
	drm_err(&xe->drm, "GuC engine reset request failed on %d:%d because 0x%08X",
		guc_class, instance, reason);

	xe_gt_reset_async(guc_to_gt(guc));

	return 0;
}

static void guc_engine_wq_print(struct xe_engine *e, struct drm_printer *p)
{
	struct dma_buf_map map = xe_lrc_parallel_map(e->lrc);
	int i;

	drm_printf(p, "\tWQ head: %u (internal), %d (memory)\n",
		   e->guc->wqi_head, parallel_read(map, wq_desc.head));
	drm_printf(p, "\tWQ tail: %u (internal), %d (memory)\n",
		   e->guc->wqi_tail, parallel_read(map, wq_desc.tail));
	drm_printf(p, "\tWQ status: %u\n",
		   parallel_read(map, wq_desc.wq_status));
	if (parallel_read(map, wq_desc.head) !=
	    parallel_read(map, wq_desc.tail)) {
		for (i = parallel_read(map, wq_desc.head);
		     i != parallel_read(map, wq_desc.tail);
		     i = (i + sizeof(u32)) % WQ_SIZE)
			drm_printf(p, "\tWQ[%ld]: 0x%08x\n", i / sizeof(u32),
				   parallel_read(map, wq[i / sizeof(u32)]));
	}
}

static void guc_engine_print(struct xe_engine *e, struct drm_printer *p)
{
	struct drm_gpu_scheduler *sched = &e->guc->sched;
	struct xe_sched_job *job;
	int i;

	drm_printf(p, "\nGuC ID: %d\n", e->guc->id);
	drm_printf(p, "\tName: %s\n", e->name);
	drm_printf(p, "\tClass: %d\n", e->class);
	drm_printf(p, "\tLogical mask: 0x%x\n", e->logical_mask);
	drm_printf(p, "\tRef: %d\n", kref_read(&e->refcount));
	drm_printf(p, "\tTimeout: %ld\n", sched->timeout);
	for (i = 0; i < e->width; ++i ) {
		struct xe_lrc *lrc = e->lrc + i;

		drm_printf(p, "\tHW Context Desc: 0x%08x\n",
			   lower_32_bits(xe_lrc_ggtt_addr(lrc)));
		drm_printf(p, "\tLRC Head: (memory) %u\n",
			   xe_lrc_ring_head(lrc));
		drm_printf(p, "\tLRC Tail: (internal) %u, (memory) %u\n",
			   lrc->ring.tail,
			   xe_lrc_read_ctx_reg(lrc, CTX_RING_TAIL));
		drm_printf(p, "\tStart seqno: (memory) %d\n",
			   xe_lrc_start_seqno(lrc));
		drm_printf(p, "\tSeqno: (memory) %d\n", xe_lrc_seqno(lrc));
	}
	drm_printf(p, "\tSchedule State: 0x%x\n", e->guc->state);
	drm_printf(p, "\tFlags: 0x%lx\n", e->flags);
	if (xe_engine_is_parallel(e))
		guc_engine_wq_print(e, p);

	spin_lock(&sched->job_list_lock);
	list_for_each_entry(job, &sched->pending_list, drm.list)
		drm_printf(p, "\tJob: seqno=%d, fence=%d, finished=%d\n",
			   xe_sched_job_seqno(job),
			   dma_fence_is_signaled(job->fence) ? 1 : 0,
			   dma_fence_is_signaled(&job->drm.s_fence->finished) ?
			   1 : 0);
	spin_unlock(&sched->job_list_lock);
}

void xe_guc_submit_print(struct xe_guc *guc, struct drm_printer *p)
{
	struct xe_engine *e;
	unsigned long index;

	if (!xe_gt_guc_submission_enabled(guc_to_gt(guc)))
		return;

	mutex_lock(&guc->submission_state.lock);
	xa_for_each(&guc->submission_state.engine_lookup, index, e)
		guc_engine_print(e, p);
	mutex_unlock(&guc->submission_state.lock);
}
