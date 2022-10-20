// SPDX-License-Identifier: MIT
/*
 * Copyright © 2022 Intel Corporation
 */

#include <drm/drm_managed.h>

#include "xe_bo.h"
#include "xe_gt.h"
#include "xe_guc.h"
#include "xe_guc_ads.h"
#include "xe_guc_reg.h"
#include "xe_hw_engine.h"
#include "xe_map.h"
#include "xe_mmio.h"
#include "xe_platform_types.h"
#include "../i915/gt/intel_gt_regs.h"
#include "../i915/gt/intel_engine_regs.h"

/* Slack of a few additional entries per engine */
#define ADS_REGSET_EXTRA_MAX	8

static struct xe_guc *
ads_to_guc(struct xe_guc_ads *ads)
{
	return container_of(ads, struct xe_guc, ads);
}

static struct xe_gt *
ads_to_gt(struct xe_guc_ads *ads)
{
	return container_of(ads, struct xe_gt, uc.guc.ads);
}

static struct xe_device *
ads_to_xe(struct xe_guc_ads *ads)
{
	return gt_to_xe(ads_to_gt(ads));
}

static struct iosys_map *
ads_to_map(struct xe_guc_ads *ads)
{
	return &ads->bo->vmap;
}

/*
 * The Additional Data Struct (ADS) has pointers for different buffers used by
 * the GuC. One single gem object contains the ADS struct itself (guc_ads) and
 * all the extra buffers indirectly linked via the ADS struct's entries.
 *
 * Layout of the ADS blob allocated for the GuC:
 *
 *      +---------------------------------------+ <== base
 *      | guc_ads                               |
 *      +---------------------------------------+
 *      | guc_policies                          |
 *      +---------------------------------------+
 *      | guc_gt_system_info                    |
 *      +---------------------------------------+
 *      | guc_engine_usage                      |
 *      +---------------------------------------+ <== static
 *      | guc_mmio_reg[countA] (engine 0.0)     |
 *      | guc_mmio_reg[countB] (engine 0.1)     |
 *      | guc_mmio_reg[countC] (engine 1.0)     |
 *      |   ...                                 |
 *      +---------------------------------------+ <== dynamic
 *      | padding                               |
 *      +---------------------------------------+ <== 4K aligned
 *      | golden contexts                       |
 *      +---------------------------------------+
 *      | padding                               |
 *      +---------------------------------------+ <== 4K aligned
 *      | capture lists                         |
 *      +---------------------------------------+
 *      | padding                               |
 *      +---------------------------------------+ <== 4K aligned
 *      | private data                          |
 *      +---------------------------------------+
 *      | padding                               |
 *      +---------------------------------------+ <== 4K aligned
 */
struct __guc_ads_blob {
	struct guc_ads ads;
	struct guc_policies policies;
	struct guc_gt_system_info system_info;
	struct guc_engine_usage engine_usage;
	/* From here on, location is dynamic! Refer to above diagram. */
	struct guc_mmio_reg regset[0];
} __packed;

#define ads_blob_read(ads_, field_) \
	xe_map_rd_field(ads_to_xe(ads_), ads_to_map(ads_), 0, \
			struct __guc_ads_blob, field_)

#define ads_blob_write(ads_, field_, val_)			\
	xe_map_wr_field(ads_to_xe(ads_), ads_to_map(ads_), 0,	\
			struct __guc_ads_blob, field_, val_)

#define info_map_write(xe_, map_, field_, val_) \
	xe_map_wr_field(xe_, map_, 0, struct guc_gt_system_info, field_, val_)

#define info_map_read(xe_, map_, field_) \
	xe_map_rd_field(xe_, map_, 0, struct guc_gt_system_info, field_)

static size_t guc_ads_regset_size(struct xe_guc_ads *ads)
{
	XE_BUG_ON(!ads->regset_size);

	return ads->regset_size;
}

static size_t guc_ads_golden_ctxt_size(struct xe_guc_ads *ads)
{
	/* FIXME: Allocate a proper golden context size */
	return PAGE_ALIGN(PAGE_SIZE * 4);
}

static size_t guc_ads_capture_size(struct xe_guc_ads *ads)
{
	/* FIXME: Allocate a proper capture list */
	return PAGE_ALIGN(PAGE_SIZE);
}

static size_t guc_ads_private_data_size(struct xe_guc_ads *ads)
{
	return PAGE_ALIGN(ads_to_guc(ads)->fw.private_data_size);
}

static size_t guc_ads_regset_offset(struct xe_guc_ads *ads)
{
	return offsetof(struct __guc_ads_blob, regset);
}

static size_t guc_ads_golden_ctxt_offset(struct xe_guc_ads *ads)
{
	size_t offset;

	offset = guc_ads_regset_offset(ads) +
		guc_ads_regset_size(ads);

	return PAGE_ALIGN(offset);
}

static size_t guc_ads_capture_offset(struct xe_guc_ads *ads)
{
	size_t offset;

	offset = guc_ads_golden_ctxt_offset(ads) +
		guc_ads_golden_ctxt_size(ads);

	return PAGE_ALIGN(offset);
}

static size_t guc_ads_private_data_offset(struct xe_guc_ads *ads)
{
	size_t offset;

	offset = guc_ads_capture_offset(ads) +
		guc_ads_capture_size(ads);

	return PAGE_ALIGN(offset);
}

static size_t guc_ads_size(struct xe_guc_ads *ads)
{
	return guc_ads_private_data_offset(ads) +
		guc_ads_private_data_size(ads);
}

static void guc_ads_fini(struct drm_device *drm, void *arg)
{
	struct xe_guc_ads *ads = arg;

	xe_bo_unpin_map_no_vm(ads->bo);
}

static size_t calculate_regset_size(struct xe_gt *gt)
{
	struct xe_reg_sr_entry *sr_entry;
	unsigned long sr_idx;
	struct xe_hw_engine *hwe;
	enum xe_hw_engine_id id;
	unsigned int count = 0;

	for_each_hw_engine(hwe, gt, id)
		xa_for_each(&hwe->reg_sr.xa, sr_idx, sr_entry)
			count++;

	count += ADS_REGSET_EXTRA_MAX * XE_NUM_HW_ENGINES;

	return count * sizeof(struct guc_mmio_reg);
}

int xe_guc_ads_init(struct xe_guc_ads *ads)
{
	struct xe_device *xe = ads_to_xe(ads);
	struct xe_gt *gt = ads_to_gt(ads);
	struct xe_bo *bo;
	int err;

	ads->regset_size = calculate_regset_size(gt);

	bo = xe_bo_create_pin_map(xe, gt, NULL, guc_ads_size(ads),
				  ttm_bo_type_kernel,
				  XE_BO_CREATE_VRAM_IF_DGFX(gt) |
				  XE_BO_CREATE_GGTT_BIT);
	if (IS_ERR(bo))
		return PTR_ERR(bo);

	ads->bo = bo;

	err = drmm_add_action_or_reset(&xe->drm, guc_ads_fini, ads);
	if (err)
		return err;

	return 0;
}

static void guc_policies_init(struct xe_guc_ads *ads)
{
	ads_blob_write(ads, policies.dpc_promote_time,
		       GLOBAL_POLICY_DEFAULT_DPC_PROMOTE_TIME_US);
	ads_blob_write(ads, policies.max_num_work_items,
		       GLOBAL_POLICY_MAX_NUM_WI);
	ads_blob_write(ads, policies.global_flags, 0);
	ads_blob_write(ads, policies.is_valid, 1);
}

static u32 engine_enable_mask(struct xe_gt *gt, enum xe_engine_class class)
{
	struct xe_hw_engine *hwe;
	enum xe_hw_engine_id id;
	u32 mask = 0;

	for_each_hw_engine(hwe, gt, id)
		if (hwe->class == class)
			mask |= BIT(hwe->instance);

	return mask;
}

static void fill_engine_enable_masks(struct xe_gt *gt,
				     struct iosys_map *info_map)
{
	struct xe_device *xe = gt_to_xe(gt);

	info_map_write(xe, info_map, engine_enabled_masks[GUC_RENDER_CLASS],
		       engine_enable_mask(gt, XE_ENGINE_CLASS_RENDER));
	info_map_write(xe, info_map, engine_enabled_masks[GUC_BLITTER_CLASS],
		       engine_enable_mask(gt, XE_ENGINE_CLASS_COPY));
	info_map_write(xe, info_map, engine_enabled_masks[GUC_VIDEO_CLASS],
		       engine_enable_mask(gt, XE_ENGINE_CLASS_VIDEO_DECODE));
	info_map_write(xe, info_map,
		       engine_enabled_masks[GUC_VIDEOENHANCE_CLASS],
		       engine_enable_mask(gt, XE_ENGINE_CLASS_VIDEO_ENHANCE));
	info_map_write(xe, info_map, engine_enabled_masks[GUC_COMPUTE_CLASS],
		       engine_enable_mask(gt, XE_ENGINE_CLASS_COMPUTE));
}

#define LR_HW_CONTEXT_SIZE (80 * sizeof(u32))
#define XEHP_LR_HW_CONTEXT_SIZE (96 * sizeof(u32))
#define LR_HW_CONTEXT_SZ(xe) (GRAPHICS_VERx100(xe) >= 1250 ? \
			      XEHP_LR_HW_CONTEXT_SIZE : \
			      LR_HW_CONTEXT_SIZE)
#define LRC_SKIP_SIZE(xe) (PAGE_SIZE + LR_HW_CONTEXT_SZ(xe))

static void guc_prep_golden_context(struct xe_guc_ads *ads)
{
	struct xe_device *xe = ads_to_xe(ads);
	struct iosys_map info_map = IOSYS_MAP_INIT_OFFSET(ads_to_map(ads),
			offsetof(struct __guc_ads_blob, system_info));
	u8 guc_class;

	/* FIXME: Setting up dummy golden contexts */
	for (guc_class = 0; guc_class <= GUC_MAX_ENGINE_CLASSES;
	     ++guc_class) {
		if (!info_map_read(xe, &info_map,
				   engine_enabled_masks[guc_class]))
			continue;

		ads_blob_write(ads, ads.eng_state_size[guc_class],
			       guc_ads_golden_ctxt_size(ads) -
			       LRC_SKIP_SIZE(xe));
		ads_blob_write(ads, ads.golden_context_lrca[guc_class],
			       xe_bo_ggtt_addr(ads->bo) +
			       guc_ads_golden_ctxt_offset(ads));
	}
}

static void guc_mapping_table_init(struct xe_gt *gt,
				   struct iosys_map *info_map)
{
	struct xe_device *xe = gt_to_xe(gt);
	struct xe_hw_engine *hwe;
	enum xe_hw_engine_id id;
	unsigned int i, j;

	/* Table must be set to invalid values for entries not used */
	for (i = 0; i < GUC_MAX_ENGINE_CLASSES; ++i)
		for (j = 0; j < GUC_MAX_INSTANCES_PER_CLASS; ++j)
			info_map_write(xe, info_map, mapping_table[i][j],
				       GUC_MAX_INSTANCES_PER_CLASS);

	for_each_hw_engine(hwe, gt, id) {
		u8 guc_class;

		guc_class = xe_engine_class_to_guc_class(hwe->class);
		info_map_write(xe, info_map,
			       mapping_table[guc_class][hwe->logical_instance],
			       hwe->instance);
	}
}

static void guc_capture_list_init(struct xe_guc_ads *ads)
{
	int i, j;
	u32 addr = xe_bo_ggtt_addr(ads->bo) + guc_ads_capture_offset(ads);

	/* FIXME: Populate a proper capture list */
	for (i = 0; i < GUC_CAPTURE_LIST_INDEX_MAX; i++) {
		for (j = 0; j < GUC_MAX_ENGINE_CLASSES; j++) {
			ads_blob_write(ads, ads.capture_instance[i][j], addr);
			ads_blob_write(ads, ads.capture_class[i][j], addr);
		}

		ads_blob_write(ads, ads.capture_global[i], addr);
	}
}

static void guc_mmio_regset_write_one(struct xe_guc_ads *ads,
				      struct iosys_map *regset_map,
				      u32 reg, u32 flags,
				      unsigned int n_entry)
{
	struct guc_mmio_reg entry = {
		.offset = reg,
		.flags = flags,
		/* TODO: steering */
	};

	xe_map_memcpy_to(ads_to_xe(ads), regset_map, n_entry * sizeof(entry),
			 &entry, sizeof(entry));
}

static unsigned int guc_mmio_regset_write(struct xe_guc_ads *ads,
					  struct iosys_map *regset_map,
					  struct xe_hw_engine *hwe)
{
	struct xe_hw_engine *hwe_rcs_reset_domain =
		xe_gt_any_hw_engine_by_reset_domain(hwe->gt, XE_ENGINE_CLASS_RENDER);
	struct xe_reg_sr_entry *entry;
	unsigned long idx;
	unsigned count = 0;
	const struct {
		u32 reg;
		u32 flags;
		bool skip;
	} *e, extra_regs[] = {
		{ .reg = RING_MODE_GEN7(hwe->mmio_base).reg,		},
		{ .reg = RING_HWS_PGA(hwe->mmio_base).reg,		},
		{ .reg = RING_IMR(hwe->mmio_base).reg,			},
		{ .reg = GEN12_RCU_MODE.reg, .flags = 0x3,
		  .skip = hwe != hwe_rcs_reset_domain			},
	};

	BUILD_BUG_ON(ARRAY_SIZE(extra_regs) > ADS_REGSET_EXTRA_MAX);

	xa_for_each(&hwe->reg_sr.xa, idx, entry) {
		u32 flags = entry->masked_reg ? GUC_REGSET_MASKED : 0;

		guc_mmio_regset_write_one(ads, regset_map, idx, flags, count++);
	}

	for (e = extra_regs; e < extra_regs + ARRAY_SIZE(extra_regs); e++) {
		if (e->skip)
			continue;

		guc_mmio_regset_write_one(ads, regset_map,
					  e->reg, e->flags, count++);
	}

	return count;
}

static void guc_mmio_reg_state_init(struct xe_guc_ads *ads)
{
	size_t regset_offset = guc_ads_regset_offset(ads);
	struct xe_gt *gt = ads_to_gt(ads);
	struct xe_hw_engine *hwe;
	enum xe_hw_engine_id id;
	u32 addr = xe_bo_ggtt_addr(ads->bo) + regset_offset;
	struct iosys_map regset_map = IOSYS_MAP_INIT_OFFSET(ads_to_map(ads),
							    regset_offset);

	for_each_hw_engine(hwe, gt, id) {
		unsigned int count;
		u8 gc;

		/*
		 * 1. Write all MMIO entries for this engine to the table. No
		 * need to worry about fused-off engines and when there are
		 * entries in the regset: the reg_state_list has been zero'ed
		 * by xe_guc_ads_populate()
		 */
		count = guc_mmio_regset_write(ads, &regset_map, hwe);
		if (!count)
			continue;

		/*
		 * 2. Record in the header (ads.reg_state_list) the address
		 * location and number of entries
		 */
		gc = xe_engine_class_to_guc_class(hwe->class);
		ads_blob_write(ads, ads.reg_state_list[gc][hwe->instance].address, addr);
		ads_blob_write(ads, ads.reg_state_list[gc][hwe->instance].count, count);

		addr += count * sizeof(struct guc_mmio_reg);
		iosys_map_incr(&regset_map, count * sizeof(struct guc_mmio_reg));
	}
}

void xe_guc_ads_populate(struct xe_guc_ads *ads)
{
	struct xe_device *xe = ads_to_xe(ads);
	struct xe_gt *gt = ads_to_gt(ads);
	struct iosys_map info_map = IOSYS_MAP_INIT_OFFSET(ads_to_map(ads),
			offsetof(struct __guc_ads_blob, system_info));
	u32 base = xe_bo_ggtt_addr(ads->bo);

	XE_BUG_ON(!ads->bo);

	xe_map_memset(ads_to_xe(ads), ads_to_map(ads), 0, 0, guc_ads_size(ads));
	guc_policies_init(ads);
	fill_engine_enable_masks(gt, &info_map);
	guc_mmio_reg_state_init(ads);
	guc_prep_golden_context(ads);
	guc_mapping_table_init(gt, &info_map);
	guc_capture_list_init(ads);

	if (GRAPHICS_VER(xe) >= 12 && !IS_DGFX(xe)) {
		u32 distdbreg =
			xe_mmio_read32(gt, GEN12_DIST_DBS_POPULATED.reg);

		ads_blob_write(ads,
			       system_info.generic_gt_sysinfo[GUC_GENERIC_GT_SYSINFO_DOORBELL_COUNT_PER_SQIDI],
			       ((distdbreg >> GEN12_DOORBELLS_PER_SQIDI_SHIFT)
				& GEN12_DOORBELLS_PER_SQIDI) + 1);
	}

	ads_blob_write(ads, ads.scheduler_policies, base +
		       offsetof(struct __guc_ads_blob, policies));
	ads_blob_write(ads, ads.gt_system_info, base +
		       offsetof(struct __guc_ads_blob, system_info));
	ads_blob_write(ads, ads.private_data, base +
		       guc_ads_private_data_offset(ads));
}

void xe_guc_ads_fini(struct xe_guc_ads *ads)
{
	xe_bo_unpin_map_no_vm(ads->bo);
}
