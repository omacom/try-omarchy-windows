/* SPDX-License-Identifier: MIT */
#ifndef HW_WDDM_ALLOCATION_HUB_H
#define HW_WDDM_ALLOCATION_HUB_H
#include "system/memory.h"
bool wddm_allocation_hub_mapping(MemoryRegion *mr, void **process,
                                  void **source, uint64_t *bytes);
#endif
