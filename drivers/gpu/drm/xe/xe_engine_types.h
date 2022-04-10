/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_ENGINE_TYPES_H_
#define _XE_ENGINE_TYPES_H_

#include <linux/kref.h>

#include <drm/gpu_scheduler.h>

#include "xe_hw_engine_types.h"
#include "xe_hw_fence_types.h"
#include "xe_lrc_types.h"

struct xe_execlist_engine;
struct xe_gt;
struct xe_guc_engine;
struct xe_hw_engine;
struct xe_vm;

/**
 * struct xe_engine - Submission engine
 *
 * Contains all state necessary for submissions. Can either be a user object or
 * a kernel object.
 */
struct xe_engine {
	/** @gt: graphics tile this engine can submit to */
	struct xe_gt *gt;
	/**
	 * @hwe: A hardware of the same class. May (physical engine) or may not
	 * (virtual engine) be where jobs actual engine up running. Should never
	 * really be used for submissions.
	 */
	struct xe_hw_engine *hwe;
	/** @refcount: ref count of this engine */
	struct kref refcount;
	/** @vm: VM (address space) for this engine */
	struct xe_vm *vm;
	/** @class: class of this engine */
	enum xe_engine_class class;
	/**
	 * @logical_mask: logical mask of where job submitted to engine can run
	 */
	u32 logical_mask;
	/** @name: name of this engine */
	char name[MAX_FENCE_NAME_LEN];
	/** @width: width (number BB submitted per exec) of this engine */
	u16 width;
	/** @fence_irq: fence IRQ used to signal job completion */
	struct xe_hw_fence_irq *fence_irq;

#define ENGINE_FLAG_BANNED	BIT(0)
#define ENGINE_FLAG_KERNEL	BIT(1)
#define ENGINE_FLAG_PERSISTENT	BIT(2)
	/**
	 * @flags: flags this is engine, should statically setup aside from ban
	 * bit
	 */
	unsigned long flags;

	union {
		/** @execlist: execlist backend specific state for engine */
		struct xe_execlist_engine *execlist;
		/** @guc: GuC backend specific state for engine */
		struct xe_guc_engine *guc;
	};

	/**
	 * @persitent: persitent engine state
	 */
	struct {
		/** @xef: file which this engine belongs to */
		struct xe_file *xef;
		/** @link: link in list of persitent engines */
		struct list_head link;
	} persitent;

	/**
	 * @parallel: parallel submission state
	 */
	struct {
		/** @composite_fence_ctx: context composite fence */
		u64 composite_fence_ctx;
		/** @composite_fence_seqno: seqno for composite fence */
		u32 composite_fence_seqno;
	} parallel;

	/** @ring_ops: ring operations for this engine */
	const struct xe_ring_ops *ring_ops;
	/** @entity: DRM sched entity for this engine (1 to 1 relationship) */
	struct drm_sched_entity *entity;
	/** @lrc: logical ring context for this engine */
	struct xe_lrc lrc[0];
};

/**
 * struct xe_engine_ops - Submission backend engine operations
 */
struct xe_engine_ops {
	/** @init: Initialize engine for submission backend */
	int (*init)(struct xe_engine *e);
	/** @kill: Kill inflight submissions for backend */
	void (*kill)(struct xe_engine *e);
	/** @init: Fini engine for submission backend */
	void (*fini)(struct xe_engine *e);
	/** @set_priority: Set priority for engine */
	int (*set_priority)(struct xe_engine *e,
			    enum drm_sched_priority priority);
};

#endif	/* _XE_ENGINE_TYPES_H_ */
