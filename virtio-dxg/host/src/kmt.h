/* SPDX-License-Identifier: GPL-2.0 */
/*
 * D3DKMT entry points, resolved from gdi32.dll at run time.
 *
 * Every call the host makes into dxgkrnl goes through this table, so tests
 * can run the whole executor against a fake driver without a GPU.
 */
#ifndef DXGHOST_KMT_H
#define DXGHOST_KMT_H

#include <stdint.h>

#if defined(_WIN32)
#define DXGHOST_KMT_CALL __stdcall
#else
#define DXGHOST_KMT_CALL
#endif

typedef int32_t(DXGHOST_KMT_CALL *dxghost_kmt_fn)(void *args);
typedef int32_t(DXGHOST_KMT_CALL *dxghost_kmt_share_fn)(
	uint32_t count, const uint32_t *objects, void *attributes,
	uint32_t access, void **nt_handle);

#define DXGHOST_KMT_FUNCTIONS(X)				\
	X(OpenAdapterFromLuid)					\
	X(CloseAdapter)						\
	X(QueryAdapterInfo)					\
	X(CreateDevice)						\
	X(DestroyDevice)					\
	X(CreateContextVirtual)					\
	X(DestroyContext)					\
	X(CreatePagingQueue)					\
	X(DestroyPagingQueue)					\
	X(CreateAllocation2)					\
	X(DestroyAllocation2)					\
	X(MakeResident)						\
	X(Evict)						\
	X(Lock2)						\
	X(Unlock2)						\
	X(ReserveGpuVirtualAddress)				\
	X(MapGpuVirtualAddress)					\
	X(FreeGpuVirtualAddress)				\
	X(UpdateGpuVirtualAddress)				\
	X(SubmitCommand)					\
	X(SubmitCommandToHwQueue)				\
	X(CreateSynchronizationObject2)				\
	X(DestroySynchronizationObject)				\
	X(SignalSynchronizationObjectFromCpu)			\
	X(SignalSynchronizationObjectFromGpu2)			\
	X(WaitForSynchronizationObjectFromCpu)			\
	X(WaitForSynchronizationObjectFromGpu)			\
	X(CreateHwQueue)					\
	X(DestroyHwQueue)					\
	X(Escape)						\
	X(QueryVideoMemoryInfo)					\
	X(GetDeviceState)					\
	X(QueryClockCalibration)				\
	X(FlushHeapTransitions)					\
	X(QueryAllocationResidency)				\
	X(UpdateAllocationProperty)				\
	X(SetAllocationPriority)				\
	X(GetAllocationPriority)				\
	X(SetContextSchedulingPriority)				\
	X(SetContextInProcessSchedulingPriority)		\
	X(GetContextSchedulingPriority)				\
	X(GetContextInProcessSchedulingPriority)		\
	X(OfferAllocations)					\
	X(ReclaimAllocations2)					\
	X(ChangeVideoMemoryReservation)				\
	X(MarkDeviceAsError)					\
	X(OpenSyncObjectFromNtHandle2)				\
	X(OpenResourceFromNtHandle)				\
	X(QueryResourceInfoFromNtHandle)			\
	X(InvalidateCache)					\
	X(EnumAdapters2)					\
	X(SubmitSignalSyncObjectsToHwQueue)			\
	X(SubmitWaitForSyncObjectsToHwQueue)

struct dxghost_kmt {
#define DXGHOST_KMT_FIELD(name) dxghost_kmt_fn name;
	DXGHOST_KMT_FUNCTIONS(DXGHOST_KMT_FIELD)
#undef DXGHOST_KMT_FIELD
	dxghost_kmt_share_fn ShareObjects;
};

/*
 * Fills the table from gdi32.dll. Entry points missing on older Windows are
 * left NULL; the executor answers STATUS_NOT_SUPPORTED for them.
 * Returns 0, or -1 when gdi32.dll cannot be loaded.
 */
int dxghost_kmt_load(struct dxghost_kmt *kmt);

#endif /* DXGHOST_KMT_H */
