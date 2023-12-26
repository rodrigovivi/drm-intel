// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_pm.h"

#include <linux/pm_runtime.h>

#include <drm/drm_managed.h>
#include <drm/ttm/ttm_placement.h>

#include "xe_bo.h"
#include "xe_bo_evict.h"
#include "xe_device.h"
#include "xe_device_sysfs.h"
#include "xe_display.h"
#include "xe_ggtt.h"
#include "xe_gt.h"
#include "xe_guc.h"
#include "xe_irq.h"
#include "xe_pcode.h"
#include "xe_wa.h"

/**
 * DOC: Xe Power Management
 *
 * Xe PM shall be guided by the simplicity.
 * Use the simplest hook options whenever possible.
 * Let's not reinvent the runtime_pm references and hooks.
 * Shall have a clear separation of display and gt underneath this component.
 *
 * What's next:
 *
 * For now s2idle and s3 are only working in integrated devices. The next step
 * is to iterate through all VRAM's BO backing them up into the system memory
 * before allowing the system suspend.
 *
 * Also runtime_pm needs to be here from the beginning.
 *
 * RC6/RPS are also critical PM features. Let's start with GuCRC and GuC SLPC
 * and no wait boost. Frequency optimizations should come on a next stage.
 */

/**
 * xe_pm_suspend - Helper for System suspend, i.e. S0->S3 / S0->S2idle
 * @xe: xe device instance
 *
 * Return: 0 on success
 */
int xe_pm_suspend(struct xe_device *xe)
{
	struct xe_gt *gt;
	u8 id;
	int err;

	for_each_gt(gt, xe, id)
		xe_gt_suspend_prepare(gt);

	/* FIXME: Super racey... */
	err = xe_bo_evict_all(xe);
	if (err)
		return err;

	xe_display_pm_suspend(xe);

	for_each_gt(gt, xe, id) {
		err = xe_gt_suspend(gt);
		if (err) {
			xe_display_pm_resume(xe);
			return err;
		}
	}

	xe_irq_suspend(xe);

	xe_display_pm_suspend_late(xe);

	return 0;
}

/**
 * xe_pm_resume - Helper for System resume S3->S0 / S2idle->S0
 * @xe: xe device instance
 *
 * Return: 0 on success
 */
int xe_pm_resume(struct xe_device *xe)
{
	struct xe_tile *tile;
	struct xe_gt *gt;
	u8 id;
	int err;

	for_each_tile(tile, xe, id)
		xe_wa_apply_tile_workarounds(tile);

	for_each_gt(gt, xe, id) {
		err = xe_pcode_init(gt);
		if (err)
			return err;
	}

	xe_display_pm_resume_early(xe);

	/*
	 * This only restores pinned memory which is the memory required for the
	 * GT(s) to resume.
	 */
	err = xe_bo_restore_kernel(xe);
	if (err)
		return err;

	xe_irq_resume(xe);

	xe_display_pm_resume(xe);

	for_each_gt(gt, xe, id)
		xe_gt_resume(gt);

	err = xe_bo_restore_user(xe);
	if (err)
		return err;

	return 0;
}

static bool xe_pm_pci_d3cold_capable(struct pci_dev *pdev)
{
	struct pci_dev *root_pdev;

	root_pdev = pcie_find_root_port(pdev);
	if (!root_pdev)
		return false;

	/* D3Cold requires PME capability and _PR3 power resource */
	if (!pci_pme_capable(root_pdev, PCI_D3cold) || !pci_pr3_present(root_pdev))
		return false;

	return true;
}

static void xe_pm_runtime_init(struct xe_device *xe)
{
	struct device *dev = xe->drm.dev;

	/*
	 * Disable the system suspend direct complete optimization.
	 * We need to ensure that the regular device suspend/resume functions
	 * are called since our runtime_pm cannot guarantee local memory
	 * eviction for d3cold.
	 * TODO: Check HDA audio dependencies claimed by i915, and then enforce
	 *       this option to integrated graphics as well.
	 */
	if (IS_DGFX(xe))
		dev_pm_set_driver_flags(dev, DPM_FLAG_NO_DIRECT_COMPLETE);

	pm_runtime_use_autosuspend(dev);
	pm_runtime_set_autosuspend_delay(dev, 1000);
	pm_runtime_set_active(dev);
	pm_runtime_allow(dev);
	pm_runtime_mark_last_busy(dev);
	pm_runtime_put(dev);
}

void xe_pm_init(struct xe_device *xe)
{
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);

	/* For now suspend/resume is only allowed with GuC */
	if (!xe_device_uc_enabled(xe))
		return;

	xe->d3cold.capable = xe_pm_pci_d3cold_capable(pdev);

	if (xe->d3cold.capable) {
		xe_device_sysfs_init(xe);
		xe_pm_set_vram_threshold(xe, DEFAULT_VRAM_THRESHOLD);
	}

	xe_pm_runtime_init(xe);
}

void xe_pm_runtime_fini(struct xe_device *xe)
{
	struct device *dev = xe->drm.dev;

	pm_runtime_get_sync(dev);
	pm_runtime_forbid(dev);
}

static void xe_pm_write_callback_task(struct xe_device *xe,
				      struct task_struct *task)
{
	WRITE_ONCE(xe->pm_callback_task, task);

	/*
	 * Just in case it's somehow possible for our writes to be reordered to
	 * the extent that something else re-uses the task written in
	 * pm_callback_task. For example after returning from the callback, but
	 * before the reordered write that resets pm_callback_task back to NULL.
	 */
	smp_mb(); /* pairs with xe_pm_read_callback_task */
}

struct task_struct *xe_pm_read_callback_task(struct xe_device *xe)
{
	smp_mb(); /* pairs with xe_pm_write_callback_task */

	return READ_ONCE(xe->pm_callback_task);
}

int xe_pm_runtime_suspend(struct xe_device *xe)
{
	struct xe_gt *gt;
	u8 id;
	int err = 0;

	/* Disable access_ongoing asserts and prevent recursive pm calls */
	xe_pm_write_callback_task(xe, current);

	if (xe->d3cold.allowed) {
		err = xe_bo_evict_all(xe);
		if (err)
			goto out;

		xe_display_pm_suspend(xe);
	}

	for_each_gt(gt, xe, id) {
		err = xe_gt_suspend(gt);
		if (err)
			goto out;
	}

	xe_irq_suspend(xe);

	if (xe->d3cold.allowed)
		xe_display_pm_suspend_late(xe);
out:
	xe_pm_write_callback_task(xe, NULL);
	return err;
}

int xe_pm_runtime_resume(struct xe_device *xe)
{
	struct xe_gt *gt;
	u8 id;
	int err = 0;

	/* Disable access_ongoing asserts and prevent recursive pm calls */
	xe_pm_write_callback_task(xe, current);

	/*
	 * It can be possible that xe has allowed d3cold but other pcie devices
	 * in gfx card soc would have blocked d3cold, therefore card has not
	 * really lost power. Detecting primary Gt power is sufficient.
	 */
	gt = xe_device_get_gt(xe, 0);
	xe->d3cold.power_lost = xe_guc_in_reset(&gt->uc.guc);

printk(KERN_ERR "KERNEL-DEBUG: %s %d\n", __FUNCTION__, __LINE__);

	if (xe->d3cold.allowed && xe->d3cold.power_lost) {
printk(KERN_ERR "KERNEL-DEBUG: %s %d\n", __FUNCTION__, __LINE__);

		for_each_gt(gt, xe, id) {
			err = xe_pcode_init(gt);
			if (err)
				goto out;
		}

		xe_display_pm_resume_early(xe);

		/*
		 * This only restores pinned memory which is the memory
		 * required for the GT(s) to resume.
		 */
		err = xe_bo_restore_kernel(xe);
		if (err)
			goto out;
	}

	xe_irq_resume(xe);
	for_each_gt(gt, xe, id)
		xe_gt_resume(gt);

	if (xe->d3cold.allowed && xe->d3cold.power_lost) {
		xe_display_pm_resume(xe);
		err = xe_bo_restore_user(xe);
		if (err)
			goto out;
	}
out:
	xe_pm_write_callback_task(xe, NULL);
	return err;
}

int xe_pm_runtime_get(struct xe_device *xe)
{
	if (xe_pm_read_callback_task(xe) == current)
		return 0;

	return pm_runtime_get_sync(xe->drm.dev);
}

int xe_pm_runtime_put(struct xe_device *xe)
{
	if (xe_pm_read_callback_task(xe) == current)
		return 0;

	pm_runtime_mark_last_busy(xe->drm.dev);
	return pm_runtime_put(xe->drm.dev);
}

int xe_pm_runtime_get_if_in_use(struct xe_device *xe)
{
	return pm_runtime_get_if_in_use(xe->drm.dev);
}

void xe_pm_assert_unbounded_bridge(struct xe_device *xe)
{
	struct pci_dev *pdev = to_pci_dev(xe->drm.dev);
	struct pci_dev *bridge = pci_upstream_bridge(pdev);

	if (!bridge)
		return;

	if (!bridge->driver) {
		drm_warn(&xe->drm, "unbounded parent pci bridge, device won't support any PM support.\n");
		device_set_pm_not_required(&pdev->dev);
	}
}

int xe_pm_set_vram_threshold(struct xe_device *xe, u32 threshold)
{
	struct ttm_resource_manager *man;
	u32 vram_total_mb = 0;
	int i;

	for (i = XE_PL_VRAM0; i <= XE_PL_VRAM1; ++i) {
		man = ttm_manager_type(&xe->ttm, i);
		if (man)
			vram_total_mb += DIV_ROUND_UP_ULL(man->size, 1024 * 1024);
	}

	drm_dbg(&xe->drm, "Total vram %u mb\n", vram_total_mb);

	if (threshold > vram_total_mb)
		return -EINVAL;

	xe->d3cold.vram_threshold = threshold;

	return 0;
}

void xe_pm_d3cold_allowed_toggle(struct xe_device *xe)
{
	struct ttm_resource_manager *man;
	u32 total_vram_used_mb = 0;
	u64 vram_used;
	int i;

	if (!xe->d3cold.capable) {
		xe->d3cold.allowed = false;
		return;
	}

	for (i = XE_PL_VRAM0; i <= XE_PL_VRAM1; ++i) {
		man = ttm_manager_type(&xe->ttm, i);
		if (man) {
			vram_used = ttm_resource_manager_usage(man);
			total_vram_used_mb += DIV_ROUND_UP_ULL(vram_used, 1024 * 1024);
		}
	}

	if (total_vram_used_mb < xe->d3cold.vram_threshold)
		xe->d3cold.allowed = true;
	else
		xe->d3cold.allowed = false;

	drm_dbg(&xe->drm,
		"d3cold: allowed=%s\n", str_yes_no(xe->d3cold.allowed));
}
