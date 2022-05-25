// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include "xe_huc.h"
#include "xe_gt.h"
#include "xe_guc.h"
#include "xe_guc_submit.h"
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

	/* GuC submission not enabled, nothing to do */
	if (!xe_gt_guc_submission_enabled(uc_to_gt(uc)))
		return 0;

	ret = xe_guc_init(&uc->guc);
	if (ret)
		goto err;

	ret = xe_huc_init(&uc->huc);
	if (ret)
		goto err;

	ret = xe_wopcm_init(&uc->wopcm);
	if (ret)
		goto err;

	ret = xe_guc_submit_init(&uc->guc);
	if (ret)
		goto err;

	return 0;

err:
	/* If any uC firmwares not found, fall back to execlists */
	xe_gt_guc_submission_disable(uc_to_gt(uc));

	return ret;
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
	int ret;

	/* GuC submission not enabled, nothing to do */
	if (!xe_gt_guc_submission_enabled(uc_to_gt(uc)))
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

int xe_uc_reset_prepare(struct xe_uc *uc)
{
	/* GuC submission not enabled, nothing to do */
	if (!xe_gt_guc_submission_enabled(uc_to_gt(uc)))
		return 0;

	return xe_guc_reset_prepare(&uc->guc);
}

int xe_uc_stop(struct xe_uc *uc)
{
	/* GuC submission not enabled, nothing to do */
	if (!xe_gt_guc_submission_enabled(uc_to_gt(uc)))
		return 0;

	return xe_guc_stop(&uc->guc);
}

int xe_uc_start(struct xe_uc *uc)
{
	/* GuC submission not enabled, nothing to do */
	if (!xe_gt_guc_submission_enabled(uc_to_gt(uc)))
		return 0;

	return xe_guc_start(&uc->guc);
}
