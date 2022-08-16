/* SPDX-Liense-Identifier: GPL-2.0 */
/*
 * Copyright © 2022 Intel Corporation
 */

#undef TRACE_SYSTEM
#define TRACE_SYSTEM xe

#if !defined(_XE_TRACE_H_) || defined(TRACE_HEADER_MULTI_READ)
#define _XE_TRACE_H_

#include <linux/types.h>
#include <linux/tracepoint.h>

#include "xe_engine_types.h"
#include "xe_guc_engine_types.h"
#include "xe_sched_job.h"
#include "xe_vm_types.h"

DECLARE_EVENT_CLASS(xe_engine,
		    TP_PROTO(struct xe_engine *e),
		    TP_ARGS(e),

		    TP_STRUCT__entry(
			     __field(enum xe_engine_class, class)
			     __field(u32, logical_mask)
			     __field(u16, width)
			     __field(u16, guc_id)
			     __field(u32, guc_state)
			     __field(u32, flags)
			     ),

		    TP_fast_assign(
			   __entry->class = e->class;
			   __entry->logical_mask = e->logical_mask;
			   __entry->width = e->width;
			   __entry->guc_id = e->guc->id;
			   __entry->guc_state = atomic_read(&e->guc->state);
			   __entry->flags = e->flags;
			   ),

		    TP_printk("%d:0x%x, width=%d, guc_id=%d, guc_state=0x%x, flags=0x%x",
			      __entry->class, __entry->logical_mask,
			      __entry->width, __entry->guc_id,
			      __entry->guc_state, __entry->flags)
);

DEFINE_EVENT(xe_engine, xe_engine_create,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_supress_resume,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_submit,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_scheduling_enable,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_scheduling_disable,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_scheduling_done,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_register,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_deregister,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_deregister_done,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_close,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_kill,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_cleanup_entity,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_destroy,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_reset,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_stop,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DEFINE_EVENT(xe_engine, xe_engine_resubmit,
	     TP_PROTO(struct xe_engine *e),
	     TP_ARGS(e)
);

DECLARE_EVENT_CLASS(xe_sched_job,
		    TP_PROTO(struct xe_sched_job *job),
		    TP_ARGS(job),

		    TP_STRUCT__entry(
			     __field(u32, seqno)
			     __field(u16, guc_id)
			     __field(u32, guc_state)
			     __field(u32, flags)
			     __field(int, error)
			     ),

		    TP_fast_assign(
			   __entry->seqno = xe_sched_job_seqno(job);
			   __entry->guc_id = job->engine->guc->id;
			   __entry->guc_state =
			   atomic_read(&job->engine->guc->state);
			   __entry->flags = job->engine->flags;
			   __entry->error = job->fence->error;
			   ),

		    TP_printk("seqno=%u, guc_id=%d, guc_state=0x%x, flags=0x%x, error=%d",
			      __entry->seqno, __entry->guc_id,
			      __entry->guc_state, __entry->flags,
			      __entry->error)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_create,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_exec,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_run,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_free,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_timedout,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_set_error,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DEFINE_EVENT(xe_sched_job, xe_sched_job_ban,
	     TP_PROTO(struct xe_sched_job *job),
	     TP_ARGS(job)
);

DECLARE_EVENT_CLASS(drm_sched_msg,
		    TP_PROTO(struct drm_sched_msg *msg),
		    TP_ARGS(msg),

		    TP_STRUCT__entry(
			     __field(u32, opcode)
			     __field(u16, guc_id)
			     ),

		    TP_fast_assign(
			   __entry->opcode = msg->opcode;
			   __entry->guc_id =
			   ((struct xe_engine *)msg->private_data)->guc->id;
			   ),

		    TP_printk("guc_id=%d, opcode=%u", __entry->guc_id,
			      __entry->opcode)
);

DEFINE_EVENT(drm_sched_msg, drm_sched_msg_add,
	     TP_PROTO(struct drm_sched_msg *msg),
	     TP_ARGS(msg)
);

DEFINE_EVENT(drm_sched_msg, drm_sched_msg_recv,
	     TP_PROTO(struct drm_sched_msg *msg),
	     TP_ARGS(msg)
);

DECLARE_EVENT_CLASS(xe_hw_fence,
		    TP_PROTO(struct xe_hw_fence *fence),
		    TP_ARGS(fence),

		    TP_STRUCT__entry(
			     __field(u64, ctx)
			     __field(u32, seqno)
			     ),

		    TP_fast_assign(
			   __entry->ctx = fence->dma.context;
			   __entry->seqno = fence->dma.seqno;
			   ),

		    TP_printk("ctx=0x%016llx, seqno=%u", __entry->ctx,
			      __entry->seqno)
);

DEFINE_EVENT(xe_hw_fence, xe_hw_fence_create,
	     TP_PROTO(struct xe_hw_fence *fence),
	     TP_ARGS(fence)
);

DEFINE_EVENT(xe_hw_fence, xe_hw_fence_signal,
	     TP_PROTO(struct xe_hw_fence *fence),
	     TP_ARGS(fence)
);

DEFINE_EVENT(xe_hw_fence, xe_hw_fence_free,
	     TP_PROTO(struct xe_hw_fence *fence),
	     TP_ARGS(fence)
);

DECLARE_EVENT_CLASS(xe_vma,
		    TP_PROTO(struct xe_vma *vma),
		    TP_ARGS(vma),

		    TP_STRUCT__entry(
			     __field(u64, vma)
			     __field(u64, start)
			     __field(u64, end)
			     __field(u64, ptr)
			     ),

		    TP_fast_assign(
			   __entry->vma = (u64)vma;
			   __entry->start = vma->start;
			   __entry->end = vma->end;
			   __entry->ptr = (u64)vma->userptr.ptr;
			   ),

		    TP_printk("vma=0x%016llx, start=0x%016llx, end=0x%016llx, ptr=0x%016llx,",
			      __entry->vma, __entry->start, __entry->end,
			      __entry->ptr)
)

DEFINE_EVENT(xe_vma, xe_vma_flush,
	     TP_PROTO(struct xe_vma *vma),
	     TP_ARGS(vma)
);

DEFINE_EVENT(xe_vma, xe_vma_fail,
	     TP_PROTO(struct xe_vma *vma),
	     TP_ARGS(vma)
);

DEFINE_EVENT(xe_vma, xe_vma_bind,
	     TP_PROTO(struct xe_vma *vma),
	     TP_ARGS(vma)
);

DEFINE_EVENT(xe_vma, xe_vma_unbind,
	     TP_PROTO(struct xe_vma *vma),
	     TP_ARGS(vma)
);

DEFINE_EVENT(xe_vma, xe_vma_userptr_rebind_worker,
	     TP_PROTO(struct xe_vma *vma),
	     TP_ARGS(vma)
);

DEFINE_EVENT(xe_vma, xe_vma_userptr_rebind_exec,
	     TP_PROTO(struct xe_vma *vma),
	     TP_ARGS(vma)
);

DEFINE_EVENT(xe_vma, xe_vma_rebind_worker,
	     TP_PROTO(struct xe_vma *vma),
	     TP_ARGS(vma)
);

DEFINE_EVENT(xe_vma, xe_vma_rebind_exec,
	     TP_PROTO(struct xe_vma *vma),
	     TP_ARGS(vma)
);

DEFINE_EVENT(xe_vma, xe_vma_userptr_invalidate,
	     TP_PROTO(struct xe_vma *vma),
	     TP_ARGS(vma)
);

DEFINE_EVENT(xe_vma, xe_vma_evict,
	     TP_PROTO(struct xe_vma *vma),
	     TP_ARGS(vma)
);

DEFINE_EVENT(xe_vma, xe_vma_userptr_pin_set_dirty,
	     TP_PROTO(struct xe_vma *vma),
	     TP_ARGS(vma)
);

DEFINE_EVENT(xe_vma, xe_vma_userptr_invalidate_complete,
	     TP_PROTO(struct xe_vma *vma),
	     TP_ARGS(vma)
);

DECLARE_EVENT_CLASS(xe_vm,
		    TP_PROTO(struct xe_vm *vm),
		    TP_ARGS(vm),

		    TP_STRUCT__entry(
			     __field(u64, vm)
			     ),

		    TP_fast_assign(
			   __entry->vm = (u64)vm;
			   ),

		    TP_printk("vm=0x%016llx",  __entry->vm)
);

DEFINE_EVENT(xe_vm, xe_vm_create,
	     TP_PROTO(struct xe_vm *vm),
	     TP_ARGS(vm)
);

DEFINE_EVENT(xe_vm, xe_vm_free,
	     TP_PROTO(struct xe_vm *vm),
	     TP_ARGS(vm)
);

DEFINE_EVENT(xe_vm, xe_vm_restart,
	     TP_PROTO(struct xe_vm *vm),
	     TP_ARGS(vm)
);

DEFINE_EVENT(xe_vm, xe_vm_rebind_worker_enter,
	     TP_PROTO(struct xe_vm *vm),
	     TP_ARGS(vm)
);

DEFINE_EVENT(xe_vm, xe_vm_rebind_worker_retry,
	     TP_PROTO(struct xe_vm *vm),
	     TP_ARGS(vm)
);

DEFINE_EVENT(xe_vm, xe_vm_rebind_worker_exit,
	     TP_PROTO(struct xe_vm *vm),
	     TP_ARGS(vm)
);

#endif /* _XE_TRACE_H_ */

/* This part must be outside protection */
#undef TRACE_INCLUDE_PATH
#undef TRACE_INCLUDE_FILE
#define TRACE_INCLUDE_PATH ../../drivers/gpu/drm/xe
#define TRACE_INCLUDE_FILE xe_trace
#include <trace/define_trace.h>
