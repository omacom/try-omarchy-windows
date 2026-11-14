/* SPDX-License-Identifier: MIT */
#ifndef DXG_NVLOAD_H
#define DXG_NVLOAD_H

#include <windows.h>

/*
 * The NVIDIA adapter's Vulkan ICD path and LUID, asked of dxgkrnl (through
 * the bridge, or of Windows itself when on_windows). 0 when found.
 */
int nv_find_driver(wchar_t *path, LUID *luid, int on_windows);
/*
 * Opens the bridge (unix_lib: Windows path of dxgkmt.so), routes D3DKMT and
 * loads the driver. trace: 1 logs every call, 0 failures, -1 nothing.
 */
int nv_load(int trace, const wchar_t *unix_lib, HMODULE *icd, LUID *luid);

#endif
