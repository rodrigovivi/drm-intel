/*
 * SPDX-License-Identifier: MIT
 *
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_SCHED_JOB_TYPES_H_
#define _XE_SCHED_JOB_TYPES_H_

#include <drm/gpu_scheduler.h>

struct xe_engine;

/**
 * struct xe_sched_job - XE schedule job (batch buffer tracking)
 */
struct xe_sched_job {
	/** @drm: base DRM scheduler job */
	struct drm_sched_job drm;
	/** @engine: XE submission engine */
	struct xe_engine *engine;
	/**
	 * @fence: dma fence to indicate completion. 1 way relationship - job
	 * can safely reference fence, fence cannot safely reference job.
	 */
	struct dma_fence *fence;
	/** @batch_addr: batch buffer address of job */
	uint64_t batch_addr[0];
};

#endif	/* _XE_SCHED_JOB_TYPES_H_ */
