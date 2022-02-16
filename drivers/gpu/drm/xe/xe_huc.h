/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_HUC_H_
#define _XE_HUC_H_

#include "xe_huc_types.h"

struct drm_printer;

int xe_huc_init(struct xe_huc *huc);
int xe_huc_upload(struct xe_huc *huc);
void xe_huc_print_info(struct xe_huc *huc, struct drm_printer *p);

static inline void
xe_huc_sanitize(struct xe_huc *huc)
{
	// TODO - Reset HuC SW state
}

#endif	/* _XE_HUC_H_ */
