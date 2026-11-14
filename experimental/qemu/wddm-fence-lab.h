/* SPDX-License-Identifier: MIT */
#ifndef HW_WDDM_FENCE_LAB_H
#define HW_WDDM_FENCE_LAB_H

#include "system/memory.h"

/* Host-controlled diagnostic mapping only; no guest-provided process/address. */
bool wddm_fence_lab_mapping(MemoryRegion *mr, void **process, void **source);
bool wddm_fence_hub_mapping(MemoryRegion *mr, void **process, void **source);

#endif
