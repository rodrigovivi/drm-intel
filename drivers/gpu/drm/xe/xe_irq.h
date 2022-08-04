/* SPDX-License-Identifier: MIT */
/*
 * Copyright © 2022 Intel Corporation
 */

#ifndef _XE_IRQ_H_
#define _XE_IRQ_H_

struct xe_device;

int xe_irq_install(struct xe_device *xe);
void xe_irq_shutdown(struct xe_device *xe);
void xe_irq_suspend(struct xe_device *xe);
void xe_irq_resume(struct xe_device *xe);

#endif
