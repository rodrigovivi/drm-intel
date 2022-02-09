// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_device_types.h"
#include "xe_bo.h"
#include "xe_guc.h"
#include "xe_guc_ads.h"
#include "xe_guc_log.h"
#include "xe_guc_reg.h"
#include "xe_gt.h"
#include "xe_uc_fw.h"
#include "xe_wopcm.h"
#include "xe_mmio.h"
#include "xe_force_wake.h"
#include "i915_reg_defs.h"

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

/* GuC addresses above GUC_GGTT_TOP also don't map through the GTT */
#define GUC_GGTT_TOP    0xFEE00000
static u32 guc_bo_ggtt_addr(struct xe_guc *guc,
			    struct xe_bo *bo)
{
	u32 addr = xe_bo_ggtt_addr(bo);

	XE_BUG_ON(addr < xe_wopcm_size(guc_to_xe(guc)));
	XE_BUG_ON(range_overflows_t(u32, addr, bo->size, GUC_GGTT_TOP));

	return addr;
}

static u32 guc_ctl_debug_flags(struct xe_guc *guc)
{
	u32 level = xe_guc_log_get_level(&guc->log);
	u32 flags = 0;

	if (!GUC_LOG_LEVEL_IS_VERBOSE(level))
		flags |= GUC_LOG_DISABLED;
	else
		flags |= GUC_LOG_LEVEL_TO_VERBOSITY(level) <<
			 GUC_LOG_VERBOSITY_SHIFT;

	return flags;
}

static u32 guc_ctl_feature_flags(struct xe_guc *guc)
{
	/* FIXME: Just loading the GuC for now, disable submission */
	return GUC_CTL_DISABLE_SCHEDULER;
}

static u32 guc_ctl_log_params_flags(struct xe_guc *guc)
{
	u32 offset = guc_bo_ggtt_addr(guc, guc->log.bo) >> PAGE_SHIFT;
	u32 flags;

	#if (((CRASH_BUFFER_SIZE) % SZ_1M) == 0)
	#define LOG_UNIT SZ_1M
	#define LOG_FLAG GUC_LOG_LOG_ALLOC_UNITS
	#else
	#define LOG_UNIT SZ_4K
	#define LOG_FLAG 0
	#endif

	#if (((CAPTURE_BUFFER_SIZE) % SZ_1M) == 0)
	#define CAPTURE_UNIT SZ_1M
	#define CAPTURE_FLAG GUC_LOG_CAPTURE_ALLOC_UNITS
	#else
	#define CAPTURE_UNIT SZ_4K
	#define CAPTURE_FLAG 0
	#endif

	BUILD_BUG_ON(!CRASH_BUFFER_SIZE);
	BUILD_BUG_ON(!IS_ALIGNED(CRASH_BUFFER_SIZE, LOG_UNIT));
	BUILD_BUG_ON(!DEBUG_BUFFER_SIZE);
	BUILD_BUG_ON(!IS_ALIGNED(DEBUG_BUFFER_SIZE, LOG_UNIT));
	BUILD_BUG_ON(!CAPTURE_BUFFER_SIZE);
	BUILD_BUG_ON(!IS_ALIGNED(CAPTURE_BUFFER_SIZE, CAPTURE_UNIT));

	BUILD_BUG_ON((CRASH_BUFFER_SIZE / LOG_UNIT - 1) >
			(GUC_LOG_CRASH_MASK >> GUC_LOG_CRASH_SHIFT));
	BUILD_BUG_ON((DEBUG_BUFFER_SIZE / LOG_UNIT - 1) >
			(GUC_LOG_DEBUG_MASK >> GUC_LOG_DEBUG_SHIFT));
	BUILD_BUG_ON((CAPTURE_BUFFER_SIZE / CAPTURE_UNIT - 1) >
			(GUC_LOG_CAPTURE_MASK >> GUC_LOG_CAPTURE_SHIFT));

	flags = GUC_LOG_VALID |
		GUC_LOG_NOTIFY_ON_HALF_FULL |
		CAPTURE_FLAG |
		LOG_FLAG |
		((CRASH_BUFFER_SIZE / LOG_UNIT - 1) << GUC_LOG_CRASH_SHIFT) |
		((DEBUG_BUFFER_SIZE / LOG_UNIT - 1) << GUC_LOG_DEBUG_SHIFT) |
		((CAPTURE_BUFFER_SIZE / CAPTURE_UNIT - 1) << GUC_LOG_CAPTURE_SHIFT) |
		(offset << GUC_LOG_BUF_ADDR_SHIFT);

	#undef LOG_UNIT
	#undef LOG_FLAG
	#undef CAPTURE_UNIT
	#undef CAPTURE_FLAG

	return flags;
}

static u32 guc_ctl_ads_flags(struct xe_guc *guc)
{
	u32 ads = guc_bo_ggtt_addr(guc, guc->ads.bo) >> PAGE_SHIFT;
	u32 flags = ads << GUC_ADS_ADDR_SHIFT;

	return flags;
}

static u32 guc_ctl_wa_flags(struct xe_guc *guc)
{
	struct xe_device *xe = guc_to_xe(guc);
	u32 flags = 0;

	/* Wa_22012773006:gen11,gen12 < XeHP */
	if (GRAPHICS_VER(xe) >= 11 &&
	    GRAPHICS_VERx10(xe) < 125)
		flags |= GUC_WA_POLLCS;

	return flags;
}

static u32 guc_ctl_devid(struct xe_guc *guc)
{
	struct xe_device *xe = guc_to_xe(guc);

	return (((u32)xe->info.devid) << 16) | xe->info.revid;
}

static void guc_init_params(struct xe_guc *guc)
{
	struct xe_device *xe = guc_to_xe(guc);
	u32 *params = guc->params;
	int i;

	BUILD_BUG_ON(sizeof(guc->params) != GUC_CTL_MAX_DWORDS * sizeof(u32));
	BUILD_BUG_ON(SOFT_SCRATCH_COUNT != GUC_CTL_MAX_DWORDS + 2);

	params[GUC_CTL_LOG_PARAMS] = guc_ctl_log_params_flags(guc);
	params[GUC_CTL_FEATURE] = guc_ctl_feature_flags(guc);
	params[GUC_CTL_DEBUG] = guc_ctl_debug_flags(guc);
	params[GUC_CTL_ADS] = guc_ctl_ads_flags(guc);
	params[GUC_CTL_WA] = guc_ctl_wa_flags(guc);
	params[GUC_CTL_DEVID] = guc_ctl_devid(guc);

	for (i = 0; i < GUC_CTL_MAX_DWORDS; i++)
		drm_dbg(&xe->drm, "GuC param[%2d] = 0x%08x\n", i, params[i]);
}

/*
 * Initialise the GuC parameter block before starting the firmware
 * transfer. These parameters are read by the firmware on startup
 * and cannot be changed thereafter.
 */
void guc_write_params(struct xe_guc *guc)
{
	struct xe_gt *gt = guc_to_gt(guc);
	int i;

	xe_force_wake_assert_held(gt->mmio.fw, XE_FW_GT);

	xe_mmio_write32(gt, SOFT_SCRATCH(0).reg, 0);

	for (i = 0; i < GUC_CTL_MAX_DWORDS; i++)
		xe_mmio_write32(gt, SOFT_SCRATCH(1 + i).reg, guc->params[i]);
}

int xe_guc_init(struct xe_guc *guc)
{
	struct xe_device *xe = guc_to_xe(guc);
	int ret;

	guc->fw.type = XE_UC_FW_TYPE_GUC;
	ret = xe_uc_fw_init(&guc->fw);
	if (ret)
		goto out;

	ret = xe_guc_log_init(&guc->log);
	if (ret)
		goto err_fw;

	ret = xe_guc_ads_init(&guc->ads);
	if (ret)
		goto err_log;

	guc_init_params(guc);

	xe_uc_fw_change_status(&guc->fw, XE_UC_FIRMWARE_LOADABLE);

	return 0;

err_log:
	xe_guc_log_fini(&guc->log);
err_fw:
	xe_uc_fw_fini(&guc->fw);
out:
	drm_err(&xe->drm, "GuC init failed with %d", ret);
	return ret;
}

int xe_guc_reset(struct xe_guc *guc)
{
	struct xe_device *xe = guc_to_xe(guc);
	struct xe_gt *gt = guc_to_gt(guc);
	u32 guc_status;
	int ret;
	bool cookie;

	cookie = dma_fence_begin_signalling();
	xe_force_wake_assert_held(gt->mmio.fw, XE_FW_GT);

	xe_mmio_write32(gt, GEN6_GDRST.reg, GEN11_GRDOM_GUC);

	ret = xe_mmio_wait32(gt, GEN6_GDRST.reg, 0, GEN11_GRDOM_GUC, 5);
	if (ret) {
		drm_err(&xe->drm, "GuC reset timed out, GEN6_GDRST=0x%8x\n",
			xe_mmio_read32(gt, GEN6_GDRST.reg));
		goto err_out;
	}

	guc_status = xe_mmio_read32(gt, GUC_STATUS.reg);
	if (!(guc_status & GS_MIA_IN_RESET)) {
		drm_err(&xe->drm,
			"GuC status: 0x%x, MIA core expected to be in reset\n",
			guc_status);
		ret = -EIO;
		goto err_out;
	}

	dma_fence_end_signalling(cookie);
	return 0;

err_out:
	dma_fence_end_signalling(cookie);

	return ret;
}

static void guc_prepare_xfer(struct xe_guc *guc)
{
	struct xe_gt *gt = guc_to_gt(guc);
	u32 shim_flags = GUC_DISABLE_SRAM_INIT_TO_ZEROES |
			 GUC_ENABLE_READ_CACHE_LOGIC |
			 GUC_ENABLE_MIA_CACHING |
			 GUC_ENABLE_READ_CACHE_FOR_SRAM_DATA |
			 GUC_ENABLE_READ_CACHE_FOR_WOPCM_DATA |
			 GUC_ENABLE_MIA_CLOCK_GATING;

	/* Must program this register before loading the ucode with DMA */
	xe_mmio_write32(gt, GUC_SHIM_CONTROL.reg, shim_flags);

	xe_mmio_write32(gt, GEN9_GT_PM_CONFIG.reg, GT_DOORBELL_ENABLE);
}

/*
 * FIXME: Only supporting MMIO RSA at the moment, rsa in memory only required on
 * DG2+
 */
static int guc_xfer_rsa(struct xe_guc *guc)
{
	struct xe_gt *gt = guc_to_gt(guc);
	u32 rsa[UOS_RSA_SCRATCH_COUNT];
	size_t copied;
	int i;

	copied = xe_uc_fw_copy_rsa(&guc->fw, rsa, sizeof(rsa));
	if (copied < sizeof(rsa))
		return -ENOMEM;

	for (i = 0; i < UOS_RSA_SCRATCH_COUNT; i++)
		xe_mmio_write32(gt, UOS_RSA_SCRATCH(i).reg, rsa[i]);

	return 0;
}

/*
 * Read the GuC status register (GUC_STATUS) and store it in the
 * specified location; then return a boolean indicating whether
 * the value matches either of two values representing completion
 * of the GuC boot process.
 *
 * This is used for polling the GuC status in a wait_for()
 * loop below.
 */
static bool guc_ready(struct xe_guc *guc, u32 *status)
{
	u32 val = xe_mmio_read32(guc_to_gt(guc), GUC_STATUS.reg);
	u32 uk_val = REG_FIELD_GET(GS_UKERNEL_MASK, val);

	*status = val;
	return uk_val == XE_GUC_LOAD_STATUS_READY;
}

static int guc_wait_ucode(struct xe_guc *guc)
{
	struct xe_device *xe = guc_to_xe(guc);
	u32 status;
	int ret;

	/*
	 * Wait for the GuC to start up.
	 * NB: Docs recommend not using the interrupt for completion.
	 * Measurements indicate this should take no more than 20ms
	 * (assuming the GT clock is at maximum frequency). So, a
	 * timeout here indicates that the GuC has failed and is unusable.
	 * (Higher levels of the driver may decide to reset the GuC and
	 * attempt the ucode load again if this happens.)
	 *
	 * FIXME: There is a known (but exceedingly unlikely) race condition
	 * where the asynchronous frequency management code could reduce
	 * the GT clock while a GuC reload is in progress (during a full
	 * GT reset). A fix is in progress but there are complex locking
	 * issues to be resolved. In the meantime bump the timeout to
	 * 200ms. Even at slowest clock, this should be sufficient. And
	 * in the working case, a larger timeout makes no difference.
	 */
	ret = wait_for(guc_ready(guc, &status), 200);
	if (ret) {
		struct drm_device *drm = &xe->drm;
		struct drm_printer p = drm_info_printer(drm->dev);

		drm_info(drm, "GuC load failed: status = 0x%08X\n", status);
		drm_info(drm, "GuC load failed: status: Reset = %d, "
			"BootROM = 0x%02X, UKernel = 0x%02X, "
			"MIA = 0x%02X, Auth = 0x%02X\n",
			REG_FIELD_GET(GS_MIA_IN_RESET, status),
			REG_FIELD_GET(GS_BOOTROM_MASK, status),
			REG_FIELD_GET(GS_UKERNEL_MASK, status),
			REG_FIELD_GET(GS_MIA_MASK, status),
			REG_FIELD_GET(GS_AUTH_STATUS_MASK, status));

		if ((status & GS_BOOTROM_MASK) == GS_BOOTROM_RSA_FAILED) {
			drm_info(drm, "GuC firmware signature verification failed\n");
			ret = -ENOEXEC;
		}

		if (REG_FIELD_GET(GS_UKERNEL_MASK, status) ==
		    XE_GUC_LOAD_STATUS_EXCEPTION) {
			drm_info(drm, "GuC firmware exception. EIP: %#x\n",
				 xe_mmio_read32(guc_to_gt(guc),
						SOFT_SCRATCH(13).reg));
			ret = -ENXIO;
		}

		xe_guc_log_dump(&guc->log, &p);
	} else {
		drm_dbg(&xe->drm, "GuC successfully loaded");
	}

	return ret;
}

int xe_guc_upload(struct xe_guc *guc)
{
	int ret;

	xe_guc_ads_populate(&guc->ads);
	guc_write_params(guc);
	guc_prepare_xfer(guc);

	/*
	 * Note that GuC needs the CSS header plus uKernel code to be copied
	 * by the DMA engine in one operation, whereas the RSA signature is
	 * loaded separately, either by copying it to the UOS_RSA_SCRATCH
	 * register (if key size <= 256) or through a ggtt-pinned vma (if key
	 * size > 256). The RSA size and therefore the way we provide it to the
	 * HW is fixed for each platform and hard-coded in the bootrom.
	 */
	ret = guc_xfer_rsa(guc);
	if (ret)
		goto out;
	/*
	 * Current uCode expects the code to be loaded at 8k; locations below
	 * this are used for the stack.
	 */
	ret = xe_uc_fw_upload(&guc->fw, 0x2000, UOS_MOVE);
	if (ret)
		goto out;

	/* Wait for authentication */
	ret = guc_wait_ucode(guc);
	if (ret)
		goto out;

	xe_uc_fw_change_status(&guc->fw, XE_UC_FIRMWARE_RUNNING);
	return 0;

out:
	xe_uc_fw_change_status(&guc->fw, XE_UC_FIRMWARE_LOAD_FAIL);
	return 0	/* FIXME: ret, don't want to stop load currently */;
}

void xe_guc_fini(struct xe_guc *guc)
{
	if (!xe_uc_fw_is_loadable(&guc->fw))
		return;

	xe_guc_ads_fini(&guc->ads);
	xe_guc_log_fini(&guc->log);
	xe_uc_fw_fini(&guc->fw);
}
