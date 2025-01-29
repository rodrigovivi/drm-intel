/* SPDX-License-Identifier: MIT */
/*
 * Copyright(c) 2015 Intel Corporation. All rights reserved.
 */

#ifndef __XE_NVM_H__
#define __XE_NVM_H__

struct xe_device;

void xe_nvm_init(struct xe_device *xe);
void xe_nvm_fini(struct xe_device *xe);

#endif
