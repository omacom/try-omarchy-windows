// SPDX-License-Identifier: GPL-2.0
#include <string.h>
#include <windows.h>
#include "kmt.h"

int dxghost_kmt_load(struct dxghost_kmt *kmt)
{
	HMODULE gdi = LoadLibraryExW(L"gdi32.dll", NULL,
				     LOAD_LIBRARY_SEARCH_SYSTEM32);

	memset(kmt, 0, sizeof(*kmt));
	if (!gdi)
		return -1;
#define DXGHOST_KMT_LOAD(name)						\
	kmt->name = (dxghost_kmt_fn)(void (*)(void))GetProcAddress(gdi,	\
		"D3DKMT" #name);
	DXGHOST_KMT_FUNCTIONS(DXGHOST_KMT_LOAD)
#undef DXGHOST_KMT_LOAD
	kmt->ShareObjects = (dxghost_kmt_share_fn)(void (*)(void))
		GetProcAddress(gdi, "D3DKMTShareObjects");
	return 0;
}
