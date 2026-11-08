// SPDX-License-Identifier: GPL-2.0
/*
 * The host passes the guest's uapi structures straight to D3DKMT. Every one
 * it uses must match the Windows SDK definition byte for byte. A mismatch
 * fails the build.
 */
#include <windows.h>
#include <d3dkmthk.h>
#include <stddef.h>
#include <stdio.h>
#include "../src/wire.h"

#define SAME_SIZE(uapi, sdk)						\
	_Static_assert(sizeof(struct uapi) == sizeof(sdk),		\
		       #uapi " size differs from " #sdk)
#define SAME_FIELD(uapi, ufield, sdk, sfield)				\
	_Static_assert(offsetof(struct uapi, ufield) ==			\
		       offsetof(sdk, sfield),				\
		       #uapi "." #ufield " offset differs from " #sdk)

SAME_SIZE(d3dkmt_openadapterfromluid, D3DKMT_OPENADAPTERFROMLUID);
SAME_SIZE(d3dkmt_closeadapter, D3DKMT_CLOSEADAPTER);
SAME_SIZE(d3dkmt_queryadapterinfo, D3DKMT_QUERYADAPTERINFO);
SAME_FIELD(d3dkmt_queryadapterinfo, private_data, D3DKMT_QUERYADAPTERINFO, pPrivateDriverData);

SAME_SIZE(d3dkmt_createdevice, D3DKMT_CREATEDEVICE);
SAME_FIELD(d3dkmt_createdevice, device, D3DKMT_CREATEDEVICE, hDevice);
SAME_SIZE(d3dkmt_destroydevice, D3DKMT_DESTROYDEVICE);

SAME_SIZE(d3dkmt_createcontextvirtual, D3DKMT_CREATECONTEXTVIRTUAL);
SAME_FIELD(d3dkmt_createcontextvirtual, priv_drv_data, D3DKMT_CREATECONTEXTVIRTUAL, pPrivateDriverData);
SAME_FIELD(d3dkmt_createcontextvirtual, context, D3DKMT_CREATECONTEXTVIRTUAL, hContext);
SAME_SIZE(d3dkmt_destroycontext, D3DKMT_DESTROYCONTEXT);

SAME_SIZE(d3dkmt_createpagingqueue, D3DKMT_CREATEPAGINGQUEUE);
SAME_FIELD(d3dkmt_createpagingqueue, fence_cpu_virtual_address, D3DKMT_CREATEPAGINGQUEUE, FenceValueCPUVirtualAddress);

SAME_SIZE(d3dddi_allocationinfo2, D3DDDI_ALLOCATIONINFO2);
SAME_FIELD(d3dddi_allocationinfo2, priv_drv_data, D3DDDI_ALLOCATIONINFO2, pPrivateDriverData);
SAME_FIELD(d3dddi_allocationinfo2, gpu_virtual_address, D3DDDI_ALLOCATIONINFO2, GpuVirtualAddress);
SAME_SIZE(d3dkmt_createallocation, D3DKMT_CREATEALLOCATION);
SAME_FIELD(d3dkmt_createallocation, priv_drv_data, D3DKMT_CREATEALLOCATION, pPrivateDriverData);
SAME_FIELD(d3dkmt_createallocation, allocation_info, D3DKMT_CREATEALLOCATION, pAllocationInfo2);
SAME_FIELD(d3dkmt_createallocation, flags, D3DKMT_CREATEALLOCATION, Flags);
SAME_FIELD(d3dkmt_createallocation, private_runtime_resource_handle, D3DKMT_CREATEALLOCATION, hPrivateRuntimeResourceHandle);
SAME_SIZE(d3dkmt_destroyallocation2, D3DKMT_DESTROYALLOCATION2);
SAME_FIELD(d3dkmt_destroyallocation2, flags, D3DKMT_DESTROYALLOCATION2, Flags);

SAME_SIZE(d3dkmt_lock2, D3DKMT_LOCK2);
SAME_FIELD(d3dkmt_lock2, data, D3DKMT_LOCK2, pData);
SAME_SIZE(d3dkmt_unlock2, D3DKMT_UNLOCK2);

SAME_SIZE(d3dddi_makeresident, D3DDDI_MAKERESIDENT);
SAME_FIELD(d3dddi_makeresident, allocation_list, D3DDDI_MAKERESIDENT, AllocationList);
SAME_FIELD(d3dddi_makeresident, paging_fence_value, D3DDDI_MAKERESIDENT, PagingFenceValue);
SAME_SIZE(d3dkmt_evict, D3DKMT_EVICT);
SAME_FIELD(d3dkmt_evict, num_bytes_to_trim, D3DKMT_EVICT, NumBytesToTrim);

SAME_SIZE(d3dddi_reservegpuvirtualaddress, D3DDDI_RESERVEGPUVIRTUALADDRESS);
SAME_FIELD(d3dddi_reservegpuvirtualaddress, virtual_address, D3DDDI_RESERVEGPUVIRTUALADDRESS, VirtualAddress);
SAME_SIZE(d3dddi_mapgpuvirtualaddress, D3DDDI_MAPGPUVIRTUALADDRESS);
SAME_FIELD(d3dddi_mapgpuvirtualaddress, virtual_address, D3DDDI_MAPGPUVIRTUALADDRESS, VirtualAddress);
SAME_FIELD(d3dddi_mapgpuvirtualaddress, driver_protection, D3DDDI_MAPGPUVIRTUALADDRESS, DriverProtection);
SAME_SIZE(d3dkmt_freegpuvirtualaddress, D3DKMT_FREEGPUVIRTUALADDRESS);
SAME_SIZE(d3dkmt_updategpuvirtualaddress, D3DKMT_UPDATEGPUVIRTUALADDRESS);
SAME_FIELD(d3dkmt_updategpuvirtualaddress, operations, D3DKMT_UPDATEGPUVIRTUALADDRESS, Operations);
SAME_FIELD(d3dkmt_updategpuvirtualaddress, fence_value, D3DKMT_UPDATEGPUVIRTUALADDRESS, FenceValue);
SAME_SIZE(d3dddi_updategpuvirtualaddress_operation, D3DDDI_UPDATEGPUVIRTUALADDRESS_OPERATION);

SAME_SIZE(d3dkmt_submitcommand, D3DKMT_SUBMITCOMMAND);
SAME_FIELD(d3dkmt_submitcommand, priv_drv_data, D3DKMT_SUBMITCOMMAND, pPrivateDriverData);
SAME_FIELD(d3dkmt_submitcommand, history_buffer_array, D3DKMT_SUBMITCOMMAND, HistoryBufferArray);
SAME_SIZE(d3dkmt_submitcommandtohwqueue, D3DKMT_SUBMITCOMMANDTOHWQUEUE);
SAME_FIELD(d3dkmt_submitcommandtohwqueue, priv_drv_data, D3DKMT_SUBMITCOMMANDTOHWQUEUE, pPrivateDriverData);
SAME_FIELD(d3dkmt_submitcommandtohwqueue, written_primaries, D3DKMT_SUBMITCOMMANDTOHWQUEUE, WrittenPrimaries);
SAME_SIZE(d3dkmt_createhwqueue, D3DKMT_CREATEHWQUEUE);
SAME_FIELD(d3dkmt_createhwqueue, queue_progress_fence_cpu_va, D3DKMT_CREATEHWQUEUE, HwQueueProgressFenceCPUVirtualAddress);
SAME_SIZE(d3dkmt_destroyhwqueue, D3DKMT_DESTROYHWQUEUE);

SAME_SIZE(d3dkmt_createsynchronizationobject2, D3DKMT_CREATESYNCHRONIZATIONOBJECT2);
SAME_FIELD(d3dkmt_createsynchronizationobject2, sync_object, D3DKMT_CREATESYNCHRONIZATIONOBJECT2, hSyncObject);
SAME_SIZE(d3dddi_synchronizationobjectinfo2, D3DDDI_SYNCHRONIZATIONOBJECTINFO2);
SAME_FIELD(d3dddi_synchronizationobjectinfo2, monitored_fence.fence_cpu_virtual_address, D3DDDI_SYNCHRONIZATIONOBJECTINFO2, MonitoredFence.FenceValueCPUVirtualAddress);
SAME_FIELD(d3dddi_synchronizationobjectinfo2, monitored_fence.fence_gpu_virtual_address, D3DDDI_SYNCHRONIZATIONOBJECTINFO2, MonitoredFence.FenceValueGPUVirtualAddress);
SAME_FIELD(d3dddi_synchronizationobjectinfo2, periodic_monitored_fence.fence_cpu_virtual_address, D3DDDI_SYNCHRONIZATIONOBJECTINFO2, PeriodicMonitoredFence.FenceValueCPUVirtualAddress);
SAME_FIELD(d3dddi_synchronizationobjectinfo2, shared_handle, D3DDDI_SYNCHRONIZATIONOBJECTINFO2, SharedHandle);
SAME_SIZE(d3dkmt_destroysynchronizationobject, D3DKMT_DESTROYSYNCHRONIZATIONOBJECT);
SAME_SIZE(d3dkmt_signalsynchronizationobjectfromcpu, D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU);
SAME_FIELD(d3dkmt_signalsynchronizationobjectfromcpu, flags, D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMCPU, Flags);
SAME_SIZE(d3dkmt_signalsynchronizationobjectfromgpu2, D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2);
SAME_FIELD(d3dkmt_signalsynchronizationobjectfromgpu2, contexts, D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2, BroadcastContextArray);
SAME_FIELD(d3dkmt_signalsynchronizationobjectfromgpu2, monitored_fence_values, D3DKMT_SIGNALSYNCHRONIZATIONOBJECTFROMGPU2, MonitoredFenceValueArray);
SAME_SIZE(d3dkmt_waitforsynchronizationobjectfromcpu, D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU);
SAME_FIELD(d3dkmt_waitforsynchronizationobjectfromcpu, async_event, D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU, hAsyncEvent);
SAME_FIELD(d3dkmt_waitforsynchronizationobjectfromcpu, flags, D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMCPU, Flags);
SAME_SIZE(d3dkmt_waitforsynchronizationobjectfromgpu, D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU);
SAME_FIELD(d3dkmt_waitforsynchronizationobjectfromgpu, monitored_fence_values, D3DKMT_WAITFORSYNCHRONIZATIONOBJECTFROMGPU, MonitoredFenceValueArray);
SAME_SIZE(d3dkmt_submitsignalsyncobjectstohwqueue, D3DKMT_SUBMITSIGNALSYNCOBJECTSTOHWQUEUE);
SAME_FIELD(d3dkmt_submitsignalsyncobjectstohwqueue, fence_values, D3DKMT_SUBMITSIGNALSYNCOBJECTSTOHWQUEUE, FenceValueArray);
SAME_SIZE(d3dkmt_submitwaitforsyncobjectstohwqueue, D3DKMT_SUBMITWAITFORSYNCOBJECTSTOHWQUEUE);
SAME_FIELD(d3dkmt_submitwaitforsyncobjectstohwqueue, fence_values, D3DKMT_SUBMITWAITFORSYNCOBJECTSTOHWQUEUE, FenceValueArray);

SAME_SIZE(d3dkmt_escape, D3DKMT_ESCAPE);
SAME_FIELD(d3dkmt_escape, priv_drv_data, D3DKMT_ESCAPE, pPrivateDriverData);
SAME_FIELD(d3dkmt_escape, context, D3DKMT_ESCAPE, hContext);
SAME_SIZE(d3dkmt_queryvideomemoryinfo, D3DKMT_QUERYVIDEOMEMORYINFO);
SAME_FIELD(d3dkmt_queryvideomemoryinfo, physical_adapter_index, D3DKMT_QUERYVIDEOMEMORYINFO, PhysicalAdapterIndex);
SAME_SIZE(d3dkmt_getdevicestate, D3DKMT_GETDEVICESTATE);
SAME_SIZE(d3dkmt_markdeviceaserror, D3DKMT_MARKDEVICEASERROR);
SAME_SIZE(d3dkmt_queryclockcalibration, D3DKMT_QUERYCLOCKCALIBRATION);
SAME_FIELD(d3dkmt_queryclockcalibration, clock_data, D3DKMT_QUERYCLOCKCALIBRATION, ClockData);
SAME_SIZE(d3dkmt_flushheaptransitions, D3DKMT_FLUSHHEAPTRANSITIONS);
SAME_SIZE(d3dkmt_queryallocationresidency, D3DKMT_QUERYALLOCATIONRESIDENCY);
SAME_FIELD(d3dkmt_queryallocationresidency, residency_status, D3DKMT_QUERYALLOCATIONRESIDENCY, pResidencyStatus);
SAME_SIZE(d3dddi_updateallocproperty, D3DDDI_UPDATEALLOCPROPERTY);
SAME_FIELD(d3dddi_updateallocproperty, paging_fence_value, D3DDDI_UPDATEALLOCPROPERTY, PagingFenceValue);
SAME_SIZE(d3dkmt_setallocationpriority, D3DKMT_SETALLOCATIONPRIORITY);
SAME_FIELD(d3dkmt_setallocationpriority, priorities, D3DKMT_SETALLOCATIONPRIORITY, pPriorities);
SAME_SIZE(d3dkmt_getallocationpriority, D3DKMT_GETALLOCATIONPRIORITY);
SAME_SIZE(d3dkmt_setcontextschedulingpriority, D3DKMT_SETCONTEXTSCHEDULINGPRIORITY);
SAME_SIZE(d3dkmt_getcontextschedulingpriority, D3DKMT_GETCONTEXTSCHEDULINGPRIORITY);
SAME_SIZE(d3dkmt_offerallocations, D3DKMT_OFFERALLOCATIONS);
SAME_FIELD(d3dkmt_offerallocations, flags, D3DKMT_OFFERALLOCATIONS, Flags);
SAME_SIZE(d3dkmt_reclaimallocations2, D3DKMT_RECLAIMALLOCATIONS2);
SAME_FIELD(d3dkmt_reclaimallocations2, paging_fence_value, D3DKMT_RECLAIMALLOCATIONS2, PagingFenceValue);
SAME_SIZE(d3dkmt_changevideomemoryreservation, D3DKMT_CHANGEVIDEOMEMORYRESERVATION);
SAME_FIELD(d3dkmt_changevideomemoryreservation, reservation, D3DKMT_CHANGEVIDEOMEMORYRESERVATION, Reservation);
SAME_SIZE(d3dkmt_invalidatecache, D3DKMT_INVALIDATECACHE);
SAME_SIZE(d3dkmt_adapterinfo, D3DKMT_ADAPTERINFO);
_Static_assert(sizeof(struct dxghost_device_ids) == sizeof(D3DKMT_QUERY_DEVICE_IDS),
	       "dxghost_device_ids differs from D3DKMT_QUERY_DEVICE_IDS");

int main(void)
{
	puts("layout_test: every uapi structure matches the Windows SDK");
	return 0;
}
