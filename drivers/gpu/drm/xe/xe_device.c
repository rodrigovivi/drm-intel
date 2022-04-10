/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2021 Intel Corporation
 */

#include "xe_device.h"

#include <drm/drm_gem_ttm_helper.h>
#include <drm/drm_aperture.h>
#include <drm/drm_ioctl.h>
#include <drm/xe_drm.h>

#include "xe_bo.h"
#include "xe_debugfs.h"
#include "xe_drv.h"
#include "xe_engine.h"
#include "xe_exec.h"
#include "xe_gt.h"
#include "xe_irq.h"
#include "xe_mmio.h"
#include "xe_query.h"
#include "xe_vm.h"
#include "xe_wait_user_fence.h"

static int xe_file_open(struct drm_device *dev, struct drm_file *file)
{
	struct xe_file *xef;

	xef = kzalloc(sizeof(*xef), GFP_KERNEL);
	if (!xef)
		return -ENOMEM;

	xef->drm = file;

	mutex_init(&xef->vm.lock);
	xa_init_flags(&xef->vm.xa, XA_FLAGS_ALLOC1);

	mutex_init(&xef->engine.lock);
	xa_init_flags(&xef->engine.xa, XA_FLAGS_ALLOC1);

	file->driver_priv = xef;
	return 0;
}

static void device_kill_persitent_engines(struct xe_device *xe,
					  struct xe_file *xef);

static void xe_file_close(struct drm_device *dev, struct drm_file *file)
{
	struct xe_device *xe = to_xe_device(dev);
	struct xe_file *xef = file->driver_priv;
	struct xe_vm *vm;
	struct xe_engine *e;
	unsigned long idx;

	xa_for_each(&xef->engine.xa, idx, e) {
		xe_engine_kill(e);
		xe_engine_put(e);
	}
	mutex_destroy(&xef->engine.lock);
	device_kill_persitent_engines(xe, xef);

	xa_for_each(&xef->vm.xa, idx, vm)
		xe_vm_put(vm);
	mutex_destroy(&xef->vm.lock);

	kfree(xef);
}

static const struct drm_ioctl_desc xe_ioctls[] = {
	DRM_IOCTL_DEF_DRV(XE_DEVICE_QUERY, xe_query_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(XE_GEM_CREATE, xe_gem_create_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(XE_GEM_MMAP_OFFSET, xe_gem_mmap_offset_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(XE_VM_CREATE, xe_vm_create_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(XE_VM_DESTROY, xe_vm_destroy_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(XE_VM_BIND, xe_vm_bind_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(XE_ENGINE_CREATE, xe_engine_create_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(XE_ENGINE_DESTROY, xe_engine_destroy_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(XE_EXEC, xe_exec_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(XE_MMIO, xe_mmio_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(XE_ENGINE_SET_PROPERTY, xe_engine_set_property_ioctl, DRM_RENDER_ALLOW),
	DRM_IOCTL_DEF_DRV(XE_WAIT_USER_FENCE, xe_wait_user_fence_ioctl, DRM_RENDER_ALLOW),
};

static const struct file_operations xe_driver_fops = {
	.owner = THIS_MODULE,
	.open = drm_open,
	.release = drm_release_noglobal,
	.unlocked_ioctl = drm_ioctl,
	.mmap = drm_gem_mmap,
	.poll = drm_poll,
	.read = drm_read,
//	.compat_ioctl = i915_ioc32_compat_ioctl,
	.llseek = noop_llseek,
};

static void xe_driver_release(struct drm_device *dev)
{
	struct xe_device *xe = to_xe_device(dev);

	pci_set_drvdata(to_pci_dev(xe->drm.dev), NULL);
}

static const struct drm_driver driver = {
	/* Don't use MTRRs here; the Xserver or userspace app should
	 * deal with them for Intel hardware.
	 */
	.driver_features =
	    DRIVER_GEM |
	    DRIVER_RENDER | DRIVER_SYNCOBJ |
	    DRIVER_SYNCOBJ_TIMELINE,
	.open = xe_file_open,
	.postclose = xe_file_close,

	.prime_handle_to_fd = drm_gem_prime_handle_to_fd,
	.prime_fd_to_handle = drm_gem_prime_fd_to_handle,
//	.gem_prime_import = i915_gem_prime_import,
//
//	.dumb_create = i915_gem_dumb_create,
	.dumb_map_offset = drm_gem_ttm_dumb_map_offset,
	.release = &xe_driver_release,

	.ioctls = xe_ioctls,
	.num_ioctls = ARRAY_SIZE(xe_ioctls),
	.fops = &xe_driver_fops,
	.name = DRIVER_NAME,
	.desc = DRIVER_DESC,
	.date = DRIVER_DATE,
	.major = DRIVER_MAJOR,
	.minor = DRIVER_MINOR,
	.patchlevel = DRIVER_PATCHLEVEL,
};

struct xe_device *xe_device_create(struct pci_dev *pdev,
				   const struct pci_device_id *ent)
{
	struct xe_device *xe;
	int err;

	err = drm_aperture_remove_conflicting_pci_framebuffers(pdev, &driver);
	if (err)
		return ERR_PTR(err);

	xe = devm_drm_dev_alloc(&pdev->dev, &driver, struct xe_device, drm);
	if (IS_ERR(xe))
		return xe;

	err = ttm_device_init(&xe->ttm, &xe_ttm_funcs, xe->drm.dev,
			      xe->drm.anon_inode->i_mapping,
			      xe->drm.vma_offset_manager, false, false);
	if (WARN_ON(err))
		goto err_put;

	xe->info.devid = pdev->device;
	xe->info.revid = pdev->revision;

	spin_lock_init(&xe->irq.lock);

	init_waitqueue_head(&xe->ufence_wq);

	mutex_init(&xe->persitent_engines.lock);
	INIT_LIST_HEAD(&xe->persitent_engines.list);

	return xe;

err_put:
	drm_dev_put(&xe->drm);
	return ERR_PTR(err);
}

static int xe_set_dma_info(struct xe_device *xe)
{
	unsigned int mask_size = 39; /* TODO: Don't hard-code */
	int err;

	/*
	 * We don't have a max segment size, so set it to the max so sg's
	 * debugging layer doesn't complain
	 */
	dma_set_max_seg_size(xe->drm.dev, UINT_MAX);

	err = dma_set_mask(xe->drm.dev, DMA_BIT_MASK(mask_size));
	if (err)
		goto mask_err;

	err = dma_set_coherent_mask(xe->drm.dev, DMA_BIT_MASK(mask_size));
	if (err)
		goto mask_err;

	return 0;

mask_err:
	drm_err(&xe->drm, "Can't set DMA mask/consistent mask (%d)\n", err);
	return err;
}

int xe_device_probe(struct xe_device *xe)
{
	int err;

	err = xe_gt_alloc(to_gt(xe));
	if (err)
		return err;

	err = xe_mmio_init(xe);
	if (err)
		return err;

	err = xe_set_dma_info(xe);
	if (err)
		return err;

	err = xe_irq_install(xe);
	if (err)
		return err;

	err = xe_gt_init(to_gt(xe));
	if (err)
		return err;

	err = drm_dev_register(&xe->drm, 0);
	if (err)
		return err;

	xe_debugfs_register(xe);

	return 0;
}

void xe_device_remove(struct xe_device *xe)
{
	mutex_destroy(&xe->persitent_engines.lock);
	drm_dev_unregister(&xe->drm);
	ttm_device_fini(&xe->ttm);
}

void xe_device_shutdown(struct xe_device *xe)
{
}

void xe_device_add_persitent_engines(struct xe_device *xe, struct xe_engine *e)
{
	mutex_lock(&xe->persitent_engines.lock);
	list_add_tail(&e->persitent.link, &xe->persitent_engines.list);
	mutex_unlock(&xe->persitent_engines.lock);
}

void xe_device_remove_persitent_engines(struct xe_device *xe,
					struct xe_engine *e)
{
	mutex_lock(&xe->persitent_engines.lock);
	if (!list_empty(&e->persitent.link))
		list_del(&e->persitent.link);
	mutex_unlock(&xe->persitent_engines.lock);
}

static void device_kill_persitent_engines(struct xe_device *xe,
					  struct xe_file *xef)
{
	struct xe_engine *e, *next;

	mutex_lock(&xe->persitent_engines.lock);
	list_for_each_entry_safe(e, next, &xe->persitent_engines.list,
				 persitent.link)
		if (e->persitent.xef == xef) {
			xe_engine_kill(e);
			list_del_init(&e->persitent.link);
		}
	mutex_unlock(&xe->persitent_engines.lock);
}
