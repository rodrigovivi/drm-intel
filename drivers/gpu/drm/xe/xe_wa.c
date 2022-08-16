// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <linux/compiler_types.h>

#include "xe_gt_types.h"
#include "xe_hw_engine_types.h"
#include "xe_platform_types.h"
#include "xe_rtp.h"
#include "xe_step.h"

#include "../i915/gt/intel_engine_regs.h"
#include "../i915/gt/intel_gt_regs.h"

/* TODO:
 * - whitelist
 * - steering:  we probably want that separate, and xe_wa.c only cares about the
 *   value to be added to the table
 * - apply workarounds with and without guc
 * - move tables to single compilation units? or single elf section?
 */

static const struct xe_rtp_entry gt_was[] = {
	/* TODO: 14011060649 */
	{ XE_RTP_NAME("16010515920"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10),
		       STEP(A0, B0),
		       ENGINE_CLASS(VIDEO_DECODE)),
	  XE_RTP_SET(VDBOX_CGCTL3F18(0), ALNUNIT_CLKGATE_DIS,
		     XE_RTP_FLAG(FOREACH_ENGINE))
	},
	{ XE_RTP_NAME("22010523718"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10)),
	  XE_RTP_SET(UNSLICE_UNIT_LEVEL_CLKGATE, CG3DDISCFEG_CLKGATE_DIS)
	},
	{ XE_RTP_NAME("14011006942"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10)),
	  XE_RTP_SET(SUBSLICE_UNIT_LEVEL_CLKGATE, DSS_ROUTER_CLKGATE_DIS)
	},
	{ XE_RTP_NAME("14010948348"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10), STEP(A0, B0)),
	  XE_RTP_SET(UNSLCGCTL9430, MSQDUNIT_CLKGATE_DIS)
	},
	{ XE_RTP_NAME("14011037102"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10), STEP(A0, B0)),
	  XE_RTP_SET(UNSLCGCTL9444, LTCDD_CLKGATE_DIS)
	},
	{ XE_RTP_NAME("14011371254"),
	  XE_RTP_RULES(SUBPLATFORM(DG2, G10), STEP(A0, B0)),
	  XE_RTP_SET(SLICE_UNIT_LEVEL_CLKGATE, NODEDSS_CLKGATE_DIS)
	},
	{}
};

static const struct xe_rtp_entry engine_was[] = {
	{ XE_RTP_NAME("14015227452"),
	  XE_RTP_RULES(PLATFORM(DG2), ENGINE_CLASS(RENDER)),
	  XE_RTP_SET(GEN9_ROW_CHICKEN4, XEHP_DIS_BBL_SYSPIPE,
		     XE_RTP_FLAG(MASKED_REG))
	},
	{}
};

void xe_wa_process_gt(struct xe_gt *gt)
{
	xe_rtp_process(gt_was, &gt->reg_sr, gt, NULL);
}

void xe_wa_process_engine(struct xe_hw_engine *hwe)
{
	xe_rtp_process(engine_was, &hwe->reg_sr, hwe->gt, hwe);
}

void xe_wa_process_ctx(struct xe_hw_engine *hwe)
{
	//xe_rtp_process(engine_was, &hwe->reg_sr, gt, hwe);
}
