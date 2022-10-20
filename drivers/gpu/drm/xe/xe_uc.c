// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_huc.h"
#include "xe_gt.h"
#include "xe_guc.h"
#include "xe_uc.h"
#include "xe_uc_fw.h"
#include "xe_wopcm.h"

static struct xe_gt *
uc_to_gt(struct xe_uc *uc)
{
	return container_of(uc, struct xe_gt, uc);
}

static struct xe_device *
uc_to_xe(struct xe_uc *uc)
{
	return gt_to_xe(uc_to_gt(uc));
}

/* Should be called once at driver load only */
int xe_uc_init(struct xe_uc *uc)
{
	int ret;

	ret = xe_guc_init(&uc->guc);
	if (ret)
		return ret;

	ret = xe_huc_init(&uc->huc);
	if (ret)
		return ret;

	ret = xe_wopcm_init(&uc->wopcm);
	if (ret)
		return ret;

	return 0;
}

static int uc_reset(struct xe_uc *uc)
{
	struct xe_device *xe = uc_to_xe(uc);
	int ret;

	ret = xe_guc_reset(&uc->guc);
	if (ret) {
		drm_err(&xe->drm, "Failed to reset GuC, ret = %d\n", ret);
		return ret;
	}

	return 0;
}

static int uc_sanitize(struct xe_uc *uc)
{
	xe_huc_sanitize(&uc->huc);
	xe_guc_sanitize(&uc->guc);

	return uc_reset(uc);
}

/*
 * Should be called during driver load, after every GT reset, and after every
 * suspend to reload / auth the firmwares.
 */
int xe_uc_init_hw(struct xe_uc *uc)
{
	struct xe_device *xe = uc_to_xe(uc);
	int ret;

	/*
	 * XXX: For some reason if the GuC is loaded on DG1, execlist submission
	 * breaks (seen by xe_exec_basic hanging). Apply quick hack to disable
	 * the GuC on DG1 for now. A follow up will plumb a modparam in.
	 */
	if (IS_DGFX(xe)) {
		drm_dbg(&xe->drm, "Skipping GuC load");
		return 0;
	}

	/* If any uC firmwares not found, bail out and fall back to execlists */
	if (!xe_uc_fw_is_loadable(&uc->guc.fw) ||
	    !xe_uc_fw_is_loadable(&uc->huc.fw))
		return 0;

	ret = uc_sanitize(uc);
	if (ret)
		return ret;

	ret = xe_huc_upload(&uc->huc);
	if (ret)
		return ret;

	ret = xe_guc_upload(&uc->guc);
	if (ret)
		return ret;

	ret = xe_guc_enable_communication(&uc->guc);
	if (ret)
		return ret;

	/* We don't fail the driver load if HuC fails to auth, but let's warn */
	ret = xe_huc_auth(&uc->huc);
	XE_WARN_ON(ret);

	return 0;
}
