// SPDX-License-Identifier: MIT
/*
 * Copyright © 2023 Intel Corporation
 */

#if IS_ENABLED(CONFIG_DRM_XE_DISPLAY)

#include "xe_device.h"
#include "xe_display.h"
#include "xe_module.h"

#include <drm/drm_aperture.h>
#include <drm/drm_managed.h>
#include <drm/xe_drm.h>

#include "display/intel_acpi.h"
#include "display/intel_audio.h"
#include "display/intel_bw.h"
#include "display/intel_display.h"
#include "display/intel_fbdev.h"
#include "display/intel_hdcp.h"
#include "display/intel_opregion.h"
#include "display/ext/i915_irq.h"
#include "display/ext/intel_dram.h"
#include "display/ext/intel_pm.h"

/* Xe device functions */

int xe_display_enable(struct pci_dev *pdev, struct drm_driver *driver)
{
	if (!enable_display)
		return 0;

	/* Detect if we need to wait for other drivers early on */
	if (intel_modeset_probe_defer(pdev))
		return EPROBE_DEFER;

	driver->driver_features |= DRIVER_MODESET | DRIVER_ATOMIC;
	driver->lastclose = intel_fbdev_restore_mode;

	return 0;
}

void xe_display_fini_nommio(struct drm_device *dev, void *dummy)
{
	struct xe_device *xe = to_xe_device(dev);

	if (!xe->info.enable_display)
		return;

	intel_power_domains_cleanup(xe);
}

int xe_display_init_nommio(struct xe_device *xe)
{
	int err;

	if (!xe->info.enable_display)
		return 0;

	/* This must be called before any calls to HAS_PCH_* */
	intel_detect_pch(xe);
	intel_display_irq_init(xe);

	err = intel_power_domains_init(xe);
	if (err)
		return err;

	intel_init_display_hooks(xe);

	return drmm_add_action_or_reset(&xe->drm, xe_display_fini_nommio, xe);
}

void xe_display_fini_noirq(struct drm_device *dev, void *dummy)
{
	struct xe_device *xe = to_xe_device(dev);

	if (!xe->info.enable_display)
		return;

	intel_modeset_driver_remove_noirq(xe);
	intel_power_domains_driver_remove(xe);
}

int xe_display_init_noirq(struct xe_device *xe)
{
	int err;

	if (!xe->info.enable_display)
		return 0;

	/* Early display init.. */
	intel_opregion_setup(xe);

	/*
	 * Fill the dram structure to get the system dram info. This will be
	 * used for memory latency calculation.
	 */
	intel_dram_detect(xe);

	intel_bw_init_hw(xe);

	intel_device_info_runtime_init(xe);

	err = drm_aperture_remove_conflicting_pci_framebuffers(to_pci_dev(xe->drm.dev),
							       xe->drm.driver);
	if (err)
		return err;

	err = intel_modeset_init_noirq(xe);
	if (err)
		return err;

	return drmm_add_action_or_reset(&xe->drm, xe_display_fini_noirq, NULL);
}

void xe_display_fini_noaccel(struct drm_device *dev, void *dummy)
{
	struct xe_device *xe = to_xe_device(dev);

	if (!xe->info.enable_display)
		return;

	intel_modeset_driver_remove_nogem(xe);
}

int xe_display_init_noaccel(struct xe_device *xe)
{
	int err;

	if (!xe->info.enable_display)
		return 0;

	err = intel_modeset_init_nogem(xe);
	if (err)
		return err;

	return drmm_add_action_or_reset(&xe->drm, xe_display_fini_noaccel, NULL);
}

int xe_display_init(struct xe_device *xe)
{
	if (!xe->info.enable_display)
		return 0;

	return intel_modeset_init(xe);
}

void xe_display_unlink(struct xe_device *xe)
{
	if (!xe->info.enable_display)
		return;

	/* poll work can call into fbdev, hence clean that up afterwards */
	intel_hpd_poll_fini(xe);
	intel_fbdev_fini(xe);

	intel_hdcp_component_fini(xe);
	intel_audio_deinit(xe);
}

void xe_display_register(struct xe_device *xe)
{
	if (!xe->info.enable_display)
		return;

	intel_display_driver_register(xe);
	intel_register_dsm_handler();
	intel_power_domains_enable(xe);
}

void xe_display_unregister(struct xe_device *xe)
{
	if (!xe->info.enable_display)
		return;

	intel_unregister_dsm_handler();
	intel_power_domains_disable(xe);
	intel_display_driver_unregister(xe);
}

void xe_display_modset_driver_remove(struct xe_device *xe)
{
	if (!xe->info.enable_display)
		return;

	intel_modeset_driver_remove(xe);
}

#endif
