/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#include <drm/drm_managed.h>

#include "xe_execlist.h"

#include "xe_bo.h"
#include "xe_device.h"
#include "xe_engine.h"
#include "xe_hw_fence.h"
#include "xe_gt.h"
#include "xe_lrc.h"
#include "xe_macros.h"
#include "xe_mmio.h"
#include "xe_ring_ops_types.h"
#include "xe_sched_job.h"

#include "../i915/i915_reg.h"
#include "../i915/gt/intel_gpu_commands.h"
#include "../i915/gt/intel_lrc_reg.h"
#include "../i915/gt/intel_engine_regs.h"

#define XE_EXECLIST_HANG_LIMIT 1

#define GEN11_SW_CTX_ID \
	GENMASK_ULL(GEN11_SW_CTX_ID_WIDTH + GEN11_SW_CTX_ID_SHIFT - 1, \
		    GEN11_SW_CTX_ID_SHIFT)

static void __start_lrc(struct xe_hw_engine *hwe, struct xe_lrc *lrc,
			uint32_t ctx_id)
{
	struct xe_gt *gt = hwe->gt;
	uint64_t lrc_desc;

	printk(KERN_INFO "__start_lrc(%s, 0x%p, %u)\n", hwe->name, lrc, ctx_id);

	lrc_desc = xe_lrc_descriptor(lrc);

	XE_BUG_ON(!FIELD_FIT(GEN11_SW_CTX_ID, ctx_id));
	lrc_desc |= FIELD_PREP(GEN11_SW_CTX_ID, ctx_id);

	xe_lrc_write_ctx_reg(lrc, CTX_RING_TAIL, lrc->ring.tail);
	lrc->ring.old_tail = lrc->ring.tail;

	/*
	 * Make sure the context image is complete before we submit it to HW.
	 *
	 * Ostensibly, writes (including the WCB) should be flushed prior to
	 * an uncached write such as our mmio register access, the empirical
	 * evidence (esp. on Braswell) suggests that the WC write into memory
	 * may not be visible to the HW prior to the completion of the UC
	 * register write and that we may begin execution from the context
	 * before its image is complete leading to invalid PD chasing.
	 */
	wmb();

	xe_mmio_write32(gt, RING_HWS_PGA(hwe->mmio_base).reg,
			xe_bo_ggtt_addr(hwe->hwsp));
	xe_mmio_read32(gt, RING_HWS_PGA(hwe->mmio_base).reg);
	xe_mmio_write32(gt, RING_MODE_GEN7(hwe->mmio_base).reg,
			_MASKED_BIT_ENABLE(GEN11_GFX_DISABLE_LEGACY_MODE));

	xe_mmio_write32(gt, RING_EXECLIST_SQ_CONTENTS(hwe->mmio_base).reg + 0,
			lower_32_bits(lrc_desc));
	xe_mmio_write32(gt, RING_EXECLIST_SQ_CONTENTS(hwe->mmio_base).reg + 4,
			upper_32_bits(lrc_desc));
	xe_mmio_write32(gt, RING_EXECLIST_CONTROL(hwe->mmio_base).reg,
			EL_CTRL_LOAD);
}

static void __xe_execlist_port_start(struct xe_execlist_port *port,
				     struct xe_execlist_engine *exl)
{
	xe_execlist_port_assert_held(port);

	if (port->running_exl != exl || !exl->has_run) {
		port->last_ctx_id++;

		/* 0 is reserved for the kernel context */
		if (port->last_ctx_id > FIELD_MAX(GEN11_SW_CTX_ID))
			port->last_ctx_id = 1;
	}

	__start_lrc(port->hwe, &exl->engine->lrc, port->last_ctx_id);
	port->running_exl = exl;
	exl->has_run = true;
}

static void __xe_execlist_port_idle(struct xe_execlist_port *port)
{
	uint32_t noop[2] = { MI_NOOP, MI_NOOP };

	xe_execlist_port_assert_held(port);

	if (!port->running_exl)
		return;

	printk(KERN_INFO "__xe_execlist_port_idle()");

	xe_lrc_write_ring(&port->hwe->kernel_lrc, noop, sizeof(noop));
	__start_lrc(port->hwe, &port->hwe->kernel_lrc, 0);
	port->running_exl = NULL;
}

static bool xe_execlist_is_idle(struct xe_execlist_engine *exl)
{
	struct xe_lrc *lrc = &exl->engine->lrc;

	return lrc->ring.tail == lrc->ring.old_tail;
}

static void __xe_execlist_port_start_next_active(struct xe_execlist_port *port)
{
	struct xe_execlist_engine *exl = NULL;
	int i;

	xe_execlist_port_assert_held(port);

	for (i = ARRAY_SIZE(port->active) - 1; i >= 0; i--) {
		while (!list_empty(&port->active[i])) {
			exl = list_first_entry(&port->active[i],
					       struct xe_execlist_engine,
					       active_link);
			list_del(&exl->active_link);

			if (xe_execlist_is_idle(exl)) {
				exl->active_priority = DRM_SCHED_PRIORITY_UNSET;
				continue;
			}

			list_add_tail(&exl->active_link, &port->active[i]);
			__xe_execlist_port_start(port, exl);
			return;
		}
	}

	__xe_execlist_port_idle(port);
}

static uint64_t read_execlist_status(struct xe_hw_engine *hwe)
{
	struct xe_gt *gt = hwe->gt;
	uint32_t hi, lo;

	lo = xe_mmio_read32(gt, RING_EXECLIST_STATUS_LO(hwe->mmio_base).reg);
	hi = xe_mmio_read32(gt, RING_EXECLIST_STATUS_HI(hwe->mmio_base).reg);

	printk(KERN_INFO "EXECLIST_STATUS = 0x%08x %08x\n", hi, lo);

	return lo | (uint64_t)hi << 32;
}

static void xe_execlist_port_irq_handler_locked(struct xe_execlist_port *port)
{
	uint64_t status;

	xe_execlist_port_assert_held(port);

	status = read_execlist_status(port->hwe);
	if (status & BIT(7))
		return;

	__xe_execlist_port_start_next_active(port);
}

static void xe_execlist_port_irq_handler(struct xe_hw_engine *hwe,
					 uint16_t intr_vec)
{
	struct xe_execlist_port *port = hwe->exl_port;

	spin_lock(&port->lock);
	xe_execlist_port_irq_handler_locked(port);
	spin_unlock(&port->lock);
}

static void xe_execlist_port_wake_locked(struct xe_execlist_port *port,
					 enum drm_sched_priority priority)
{
	xe_execlist_port_assert_held(port);

	if (port->running_exl && port->running_exl->active_priority >= priority)
		return;

	__xe_execlist_port_start_next_active(port);
}

static void xe_execlist_make_active(struct xe_execlist_engine *exl)
{
	struct xe_execlist_port *port = exl->port;
	enum drm_sched_priority priority = exl->entity.priority;

	XE_BUG_ON(priority == DRM_SCHED_PRIORITY_UNSET);
	XE_BUG_ON(priority < 0);
	XE_BUG_ON(priority >= ARRAY_SIZE(exl->port->active));

	spin_lock_irq(&port->lock);

	if (exl->active_priority != priority &&
	    exl->active_priority != DRM_SCHED_PRIORITY_UNSET) {
		/* Priority changed, move it to the right list */
		list_del(&exl->active_link);
		exl->active_priority = DRM_SCHED_PRIORITY_UNSET;
	}

	if (exl->active_priority == DRM_SCHED_PRIORITY_UNSET) {
		exl->active_priority = priority;
		list_add_tail(&exl->active_link, &port->active[priority]);
	}

	xe_execlist_port_wake_locked(exl->port, priority);

	spin_unlock_irq(&port->lock);
}

static void xe_execlist_port_irq_fail_timer(struct timer_list *timer)
{
	struct xe_execlist_port *port =
		container_of(timer, struct xe_execlist_port, irq_fail);

	spin_lock_irq(&port->lock);
	xe_execlist_port_irq_handler_locked(port);
	spin_unlock_irq(&port->lock);

	port->irq_fail.expires = jiffies + msecs_to_jiffies(1000);
	add_timer(&port->irq_fail);
}

struct xe_execlist_port *xe_execlist_port_create(struct xe_device *xe,
						 struct xe_hw_engine *hwe)
{
	struct drm_device *drm = &xe->drm;
	struct xe_execlist_port *port;
	int i;

	port = drmm_kzalloc(drm, sizeof(*port), GFP_KERNEL);
	if (!port)
		return ERR_PTR(-ENOMEM);

	port->hwe = hwe;

	spin_lock_init(&port->lock);
	for (i = 0; i < ARRAY_SIZE(port->active); i++)
		INIT_LIST_HEAD(&port->active[i]);

	port->last_ctx_id = 1;
	port->running_exl = NULL;

	hwe->irq_handler = xe_execlist_port_irq_handler;

	/* TODO: Fix the interrupt code so it doesn't race like mad */
	timer_setup(&port->irq_fail, xe_execlist_port_irq_fail_timer, 0);
	port->irq_fail.expires = jiffies + msecs_to_jiffies(1000);
	add_timer(&port->irq_fail);

	return port;
}

void xe_execlist_port_destroy(struct xe_execlist_port *port)
{
	del_timer(&port->irq_fail);

	/* Prevent an interrupt while we're destroying */
	spin_lock_irq(&gt_to_xe(port->hwe->gt)->irq.lock);
	port->hwe->irq_handler = NULL;
	spin_unlock_irq(&gt_to_xe(port->hwe->gt)->irq.lock);
}

static struct dma_fence *
execlist_run_job(struct drm_sched_job *drm_job)
{
	struct xe_sched_job *job = to_xe_sched_job(drm_job);
	struct xe_engine *e = job->engine;
	struct xe_execlist_engine *exl = job->engine->execlist;

	e->ring_ops->emit_job(job);
	xe_execlist_make_active(exl);

	return dma_fence_get(job->fence);
}

static void execlist_job_free(struct drm_sched_job *drm_job)
{
	struct xe_sched_job *job = to_xe_sched_job(drm_job);
	struct xe_engine *e = job->engine;

	xe_sched_job_free(job);
	xe_engine_put(e);
}

static const struct drm_sched_backend_ops drm_sched_ops = {
	.run_job = execlist_run_job,
	.free_job = execlist_job_free,
};

static int execlist_engine_init(struct xe_engine *e)
{
	struct drm_gpu_scheduler *sched;
	struct xe_execlist_engine *exl;
	int err;

	XE_BUG_ON(xe_gt_guc_submission_enabled(e->gt));

	exl = kzalloc(sizeof(*exl), GFP_KERNEL);
	if (!exl)
		return -ENOMEM;

	exl->engine = e;

	err = drm_sched_init(&exl->sched, &drm_sched_ops,
			     e->lrc.ring.size / MAX_JOB_SIZE_BYTES,
			     XE_SCHED_HANG_LIMIT, XE_SCHED_JOB_TIMEOUT,
			     NULL, NULL, e->hwe->name);
	if (err)
		goto err_free;

	sched = &exl->sched;
	err = drm_sched_entity_init(&exl->entity, DRM_SCHED_PRIORITY_NORMAL,
				    &sched, 1, NULL);
	if (err)
		goto err_sched;

	exl->port = e->hwe->exl_port;
	exl->has_run = false;
	exl->active_priority = DRM_SCHED_PRIORITY_UNSET;
	e->execlist = exl;
	e->entity = &exl->entity;

	switch (e->class) {
	case XE_ENGINE_CLASS_RENDER:
		sprintf(e->name, "rcs%d", ffs(e->logical_mask) - 1);
		break;
	case XE_ENGINE_CLASS_VIDEO_DECODE:
		sprintf(e->name, "vcs%d", ffs(e->logical_mask) - 1);
		break;
	case XE_ENGINE_CLASS_VIDEO_ENHANCE:
		sprintf(e->name, "vecs%d", ffs(e->logical_mask) - 1);
		break;
	case XE_ENGINE_CLASS_COPY:
		sprintf(e->name, "bcs%d", ffs(e->logical_mask) - 1);
		break;
	case XE_ENGINE_CLASS_COMPUTE:
		sprintf(e->name, "ccs%d", ffs(e->logical_mask) - 1);
		break;
	default:
		XE_WARN_ON(e->class);
	}

	return 0;

err_sched:
	drm_sched_fini(&exl->sched);
err_free:
	kfree(exl);
	return err;
}

static void execlist_engine_fini_async(struct work_struct *w)
{
	struct xe_execlist_engine *ee =
		container_of(w, struct xe_execlist_engine, fini_async);
	struct xe_engine *e = ee->engine;
	struct xe_execlist_engine *exl = e->execlist;
	unsigned long flags;

	XE_BUG_ON(xe_gt_guc_submission_enabled(e->gt));

	spin_lock_irqsave(&exl->port->lock, flags);
	if (WARN_ON(exl->active_priority != DRM_SCHED_PRIORITY_UNSET))
		list_del(&exl->active_link);
	spin_unlock_irqrestore(&exl->port->lock, flags);

	if (e->flags & ENGINE_FLAG_PERSISTENT)
		xe_device_remove_persitent_engines(gt_to_xe(e->gt), e);
	drm_sched_entity_fini(&exl->entity);
	drm_sched_fini(&exl->sched);
	kfree(exl);

	xe_engine_fini(e);
}

static void execlist_engine_kill(struct xe_engine *e)
{
	/* NIY */
}

static void execlist_engine_fini(struct xe_engine *e)
{
	INIT_WORK(&e->execlist->fini_async, execlist_engine_fini_async);
	queue_work(system_unbound_wq, &e->execlist->fini_async);
}

static const struct xe_engine_ops execlist_engine_ops = {
	.init = execlist_engine_init,
	.kill = execlist_engine_kill,
	.fini = execlist_engine_fini,
};

int xe_execlist_init(struct xe_gt *gt)
{
	/* GuC submission enabled, nothing to do */
	if (xe_gt_guc_submission_enabled(gt))
		return 0;

	gt->engine_ops = &execlist_engine_ops;

	return 0;
}
