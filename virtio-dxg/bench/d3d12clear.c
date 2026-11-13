/* A Direct3D 12 window for Wine tests (vkd3d-proton): clears the back buffer
 * to a changing colour and presents without vsync; prints frames per second.
 *
 *   gcc -O2 -o d3d12clear.exe d3d12clear.c -ld3d12 -ldxgi -ldxguid -luser32
 *   wine d3d12clear.exe [seconds]
 */
#define COBJMACROS
#define WIDL_C_INLINE_WRAPPERS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#define FRAMES 3

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b)
{
	if (m == WM_DESTROY)
		PostQuitMessage(0);
	return DefWindowProcA(w, m, a, b);
}

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("FAIL: %s: 0x%08lx\n", #x, hr_); return 1; } } while (0)

int main(int argc, char **argv)
{
	double seconds = argc > 1 ? atof(argv[1]) : 8;
	WNDCLASSA wc = { .lpfnWndProc = proc, .hInstance = GetModuleHandleA(NULL),
			 .lpszClassName = "d3d12clear" };
	IDXGIFactory4 *factory;
	IDXGIAdapter1 *adapter;
	DXGI_ADAPTER_DESC1 desc;
	ID3D12Device *dev;
	ID3D12CommandQueue *queue;
	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	IDXGISwapChain1 *sc1;
	IDXGISwapChain3 *sc;
	DXGI_SWAP_CHAIN_DESC1 sd = { 0 };
	ID3D12DescriptorHeap *rtv_heap;
	D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, FRAMES };
	ID3D12Resource *bufs[FRAMES];
	ID3D12CommandAllocator *alloc[FRAMES];
	ID3D12GraphicsCommandList *list;
	ID3D12Fence *fence;
	UINT64 fence_values[FRAMES] = { 0 }, next_value = 1;
	HANDLE event = CreateEventA(NULL, FALSE, FALSE, NULL);
	UINT rtv_size, i, frames = 0, total = 0;
	LARGE_INTEGER f, t0, t;
	double last = 0;

	setvbuf(stdout, NULL, _IONBF, 0);
	RegisterClassA(&wc);
	HWND win = CreateWindowA("d3d12clear", "d3d12clear", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
				 100, 100, 1280, 720, NULL, NULL, wc.hInstance, NULL);

	CHECK(CreateDXGIFactory1(&IID_IDXGIFactory4, (void **)&factory));
	CHECK(IDXGIFactory4_EnumAdapters1(factory, 0, &adapter));
	IDXGIAdapter1_GetDesc1(adapter, &desc);
	CHECK(D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_12_0, &IID_ID3D12Device,
				(void **)&dev));
	printf("D3D12 device on %ls\n", desc.Description);
	{
		D3D12_FEATURE_DATA_D3D12_OPTIONS5 o5 = { 0 };
		D3D12_FEATURE_DATA_SHADER_MODEL sm = { D3D_SHADER_MODEL_6_6 };

		ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS5, &o5, sizeof(o5));
		ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof(sm));
		printf("raytracing tier 0x%x, shader model 0x%x\n", o5.RaytracingTier,
		       sm.HighestShaderModel);
	}
	CHECK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&queue));
	sd.Width = 1280;
	sd.Height = 720;
	sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	sd.SampleDesc.Count = 1;
	sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
	sd.BufferCount = FRAMES;
	sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
	CHECK(IDXGIFactory4_CreateSwapChainForHwnd(factory, (IUnknown *)queue, win, &sd, NULL, NULL, &sc1));
	CHECK(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc));
	CHECK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&rtv_heap));
	rtv_size = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	D3D12_CPU_DESCRIPTOR_HANDLE base =
		ID3D12DescriptorHeap_GetCPUDescriptorHandleForHeapStart(rtv_heap);
	for (i = 0; i < FRAMES; i++) {
		D3D12_CPU_DESCRIPTOR_HANDLE h = { base.ptr + i * rtv_size };

		CHECK(IDXGISwapChain3_GetBuffer(sc, i, &IID_ID3D12Resource, (void **)&bufs[i]));
		ID3D12Device_CreateRenderTargetView(dev, bufs[i], NULL, h);
		CHECK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
			&IID_ID3D12CommandAllocator, (void **)&alloc[i]));
	}
	CHECK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], NULL,
					     &IID_ID3D12GraphicsCommandList, (void **)&list));
	ID3D12GraphicsCommandList_Close(list);
	CHECK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence));

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t0);
	for (;;) {
		MSG msg;
		double now;
		UINT idx = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
		D3D12_CPU_DESCRIPTOR_HANDLE rtv = { base.ptr + idx * rtv_size };
		D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };

		while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT)
				return 0;
			DispatchMessageA(&msg);
		}
		QueryPerformanceCounter(&t);
		now = (double)(t.QuadPart - t0.QuadPart) / f.QuadPart;
		if (now >= seconds)
			break;
		if (ID3D12Fence_GetCompletedValue(fence) < fence_values[idx]) {
			ID3D12Fence_SetEventOnCompletion(fence, fence_values[idx], event);
			WaitForSingleObject(event, INFINITE);
		}
		ID3D12CommandAllocator_Reset(alloc[idx]);
		ID3D12GraphicsCommandList_Reset(list, alloc[idx], NULL);
		b.Transition.pResource = bufs[idx];
		b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
		ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
		float color[4] = { 0.5f + 0.5f * (float)sin(now * 3), 0.5f + 0.5f * (float)sin(now * 2 + 2),
				   0.5f + 0.5f * (float)sin(now + 4), 1 };
		ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv, color, 0, NULL);
		b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
		ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
		ID3D12GraphicsCommandList_Close(list);
		ID3D12CommandQueue_ExecuteCommandLists(queue, 1, (ID3D12CommandList **)&list);
		CHECK(IDXGISwapChain3_Present(sc, 0, 0));
		fence_values[idx] = next_value;
		ID3D12CommandQueue_Signal(queue, fence, next_value++);
		frames++;
		total++;
		if (now - last >= 1) {
			printf("%u fps\n", frames);
			frames = 0;
			last = now;
		}
	}
	printf("PASS: %u frames in %.1f s (%.0f fps average)\n", total, seconds, total / seconds);
	return 0;
}
