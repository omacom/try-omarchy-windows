// d3d12bench [TEXTURES=100] [ROUNDS=200]: the per-call cost of the D3D12
// calls Mesa's d3d12 driver makes for each small offscreen pass (GTK's
// shadows), measured the same way on Windows (native) and in the Linux guest
// (WSL runtime over virtio-dxg), to compare NVIDIA's driver in both.
// Linux: g++ -O2 -I<DirectX-Headers>/include -I<DirectX-Headers>/include/wsl/stubs d3d12bench.cpp -ldl
// Windows (MSYS2 UCRT64): g++ -O2 d3d12bench.cpp -ld3d12 -ldxgi -o d3d12bench.exe
#ifdef _WIN32
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#else
#include <wsl/winadapter.h>
#include <directx/d3d12.h>
#include <directx/dxcore.h>
#include <dxguids/dxguids.h>
#include <dlfcn.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <chrono>
#include <algorithm>

static double now_ns()
{
	return (double)std::chrono::duration_cast<std::chrono::nanoseconds>(
		std::chrono::steady_clock::now().time_since_epoch()).count();
}

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { printf("%s failed: 0x%08x\n", #x, (unsigned)hr_); exit(1); } } while (0)

int main(int argc, char **argv)
{
	int n = argc > 1 ? atoi(argv[1]) : 100;
	int rounds = argc > 2 ? atoi(argv[2]) : 200;
	ID3D12Device *dev = nullptr;
#ifdef _WIN32
	CHECK(D3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)));
#else
	void *lib = dlopen("libd3d12.so", RTLD_NOW);
	if (!lib) { printf("no libd3d12.so: %s\n", dlerror()); return 1; }
	auto create = (PFN_D3D12_CREATE_DEVICE)dlsym(lib, "D3D12CreateDevice");
	// The hardware adapter, through DXCore as Mesa finds it.
	void *dxcore = dlopen("libdxcore.so", RTLD_NOW);
	typedef HRESULT (*PFN_FACTORY)(REFIID, void **);
	auto factory_fn = dxcore ? (PFN_FACTORY)dlsym(dxcore, "DXCoreCreateAdapterFactory") : nullptr;
	if (!factory_fn) { printf("no libdxcore.so\n"); return 1; }
	IDXCoreAdapterFactory *factory;
	CHECK(factory_fn(IID_PPV_ARGS(&factory)));
	IDXCoreAdapterList *list;
	CHECK(factory->CreateAdapterList(1, &DXCORE_ADAPTER_ATTRIBUTE_D3D12_GRAPHICS, IID_PPV_ARGS(&list)));
	IDXCoreAdapter *adapter = nullptr;
	for (uint32_t i = 0; i < list->GetAdapterCount(); i++) {
		IDXCoreAdapter *a;
		bool hw = false;
		if (SUCCEEDED(list->GetAdapter(i, IID_PPV_ARGS(&a))) &&
		    SUCCEEDED(a->GetProperty(DXCoreAdapterProperty::IsHardware, &hw)) && hw) {
			adapter = a;
			break;
		}
	}
	if (!adapter) { printf("no hardware adapter\n"); return 1; }
	CHECK(create(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev)));
#endif
	D3D12_COMMAND_QUEUE_DESC qd = {};
	qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
	ID3D12CommandQueue *q;
	CHECK(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
	ID3D12CommandAllocator *alloc;
	CHECK(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)));
	ID3D12GraphicsCommandList *cl;
	CHECK(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)));
	ID3D12Fence *fence;
	CHECK(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));

	D3D12_DESCRIPTOR_HEAP_DESC hd = {};
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
	hd.NumDescriptors = n;
	ID3D12DescriptorHeap *rtv_heap, *srv_heap, *gpu_heap;
	CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtv_heap)));
	hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
	CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srv_heap)));
	hd.NumDescriptors = n * 4;
	hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
	CHECK(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gpu_heap)));
	UINT rtv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
	UINT srv_inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
#ifdef _WIN32
	D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = rtv_heap->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE srv0 = srv_heap->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE gpu0 = gpu_heap->GetCPUDescriptorHandleForHeapStart();
#else
	D3D12_CPU_DESCRIPTOR_HANDLE rtv0 = rtv_heap->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE srv0 = srv_heap->GetCPUDescriptorHandleForHeapStart();
	D3D12_CPU_DESCRIPTOR_HANDLE gpu0 = gpu_heap->GetCPUDescriptorHandleForHeapStart();
#endif

	ID3D12Resource **tex = (ID3D12Resource **)calloc(n, sizeof(*tex));
	D3D12_HEAP_PROPERTIES hp = {};
	hp.Type = D3D12_HEAP_TYPE_DEFAULT;
	D3D12_RESOURCE_DESC rd = {};
	rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
	rd.Width = 96;
	rd.Height = 96;
	rd.DepthOrArraySize = 1;
	rd.MipLevels = 1;
	rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
	rd.SampleDesc.Count = 1;
	rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
	for (int i = 0; i < n; i++)
		CHECK(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
			D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr, IID_PPV_ARGS(&tex[i])));
	ID3D12Resource *cbuf;
	D3D12_HEAP_PROPERTIES up = {};
	up.Type = D3D12_HEAP_TYPE_UPLOAD;
	D3D12_RESOURCE_DESC bd = {};
	bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
	bd.Width = 65536;
	bd.Height = 1;
	bd.DepthOrArraySize = 1;
	bd.MipLevels = 1;
	bd.SampleDesc.Count = 1;
	bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
	CHECK(dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd,
		D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&cbuf)));

	enum { RTV, SRV, CBV, BAR1, OMSET, CLEAR, BAR2, COPY, SUBMIT, NOPS };
	const char *names[NOPS] = { "CreateRenderTargetView", "CreateShaderResourceView",
		"CreateConstantBufferView (CPU heap)", "ResourceBarrier SRV->RT", "OMSetRenderTargets",
		"ClearRenderTargetView", "ResourceBarrier RT->SRV", "CopyDescriptorsSimple",
		"Close+Execute+Signal+wait (per round)" };
	double total[NOPS] = {};
	long count[NOPS] = {};
	float *samples[NOPS];
	for (int k = 0; k < NOPS; k++)
		samples[k] = (float *)calloc((size_t)n * (rounds + 5), sizeof(float));
	uint64_t fv = 0;
	float color[4] = { 0, 0, 0, 0 };
	for (int r = 0; r < rounds + 5; r++) {
		bool measure = r >= 5;
		cl->SetDescriptorHeaps(1, &gpu_heap);
		for (int i = 0; i < n; i++) {
			D3D12_CPU_DESCRIPTOR_HANDLE rtv = { rtv0.ptr + (size_t)i * rtv_inc };
			D3D12_CPU_DESCRIPTOR_HANDLE srv = { srv0.ptr + (size_t)i * srv_inc };
			D3D12_CPU_DESCRIPTOR_HANDLE dst = { gpu0.ptr + (size_t)((r * n + i) % (n * 4)) * srv_inc };
			double t[NOPS + 1];
			t[0] = now_ns();
			dev->CreateRenderTargetView(tex[i], nullptr, rtv);
			t[1] = now_ns();
			dev->CreateShaderResourceView(tex[i], nullptr, srv);
			t[2] = now_ns();
			D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = { cbuf->GetGPUVirtualAddress() + (uint64_t)(i % 64) * 256, 256 };
			dev->CreateConstantBufferView(&cbv, srv);
			t[3] = now_ns();
			D3D12_RESOURCE_BARRIER b = {};
			b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b.Transition.pResource = tex[i];
			b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			cl->ResourceBarrier(1, &b);
			t[4] = now_ns();
			cl->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
			t[5] = now_ns();
			cl->ClearRenderTargetView(rtv, color, 0, nullptr);
			t[6] = now_ns();
			b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
			b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			cl->ResourceBarrier(1, &b);
			t[7] = now_ns();
			dev->CopyDescriptorsSimple(1, dst, srv, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
			t[8] = now_ns();
			if (measure)
				for (int k = 0; k < 8; k++) {
					total[k] += t[k + 1] - t[k];
					samples[k][count[k]] = (float)(t[k + 1] - t[k]);
					count[k]++;
				}
		}
		double s0 = now_ns();
		CHECK(cl->Close());
		ID3D12CommandList *lists[] = { cl };
		q->ExecuteCommandLists(1, lists);
		CHECK(q->Signal(fence, ++fv));
		while (fence->GetCompletedValue() < fv)
			;
		CHECK(alloc->Reset());
		CHECK(cl->Reset(alloc, nullptr));
		if (measure) {
			total[SUBMIT] += now_ns() - s0;
			count[SUBMIT]++;
		}
	}
	// Variants Mesa could use instead: two transitions in one ResourceBarrier
	// call; a render pass with a clear load-op instead of OMSet + Clear.
	ID3D12GraphicsCommandList4 *cl4 = nullptr;
	cl->QueryInterface(IID_PPV_ARGS(&cl4));
	double pair_total = 0, rp_total = 0;
	long pair_count = 0, rp_count = 0;
	for (int r = 0; r < rounds + 5; r++) {
		bool measure = r >= 5;
		for (int i = 0; i < n; i++) {
			D3D12_CPU_DESCRIPTOR_HANDLE rtv = { rtv0.ptr + (size_t)i * rtv_inc };
			// tex[i] PSR->RT and tex[i-1] RT->PSR in one call.
			D3D12_RESOURCE_BARRIER b[2] = {};
			int nb = 0;
			b[nb].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
			b[nb].Transition.pResource = tex[i];
			b[nb].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
			b[nb].Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
			b[nb].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
			nb++;
			if (i > 0) {
				b[nb] = b[0];
				b[nb].Transition.pResource = tex[i - 1];
				b[nb].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
				b[nb].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
				nb++;
			}
			double t0 = now_ns();
			cl->ResourceBarrier(nb, b);
			double t1 = now_ns();
			if (cl4) {
				D3D12_RENDER_PASS_RENDER_TARGET_DESC rt = {};
				rt.cpuDescriptor = rtv;
				rt.BeginningAccess.Type = D3D12_RENDER_PASS_BEGINNING_ACCESS_TYPE_CLEAR;
				rt.BeginningAccess.Clear.ClearValue.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
				rt.EndingAccess.Type = D3D12_RENDER_PASS_ENDING_ACCESS_TYPE_PRESERVE;
				cl4->BeginRenderPass(1, &rt, nullptr, D3D12_RENDER_PASS_FLAG_NONE);
				cl4->EndRenderPass();
			}
			double t2 = now_ns();
			if (measure && nb == 2) {
				pair_total += t1 - t0;
				pair_count++;
			}
			if (measure && cl4) {
				rp_total += t2 - t1;
				rp_count++;
			}
		}
		D3D12_RESOURCE_BARRIER last = {};
		last.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
		last.Transition.pResource = tex[n - 1];
		last.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
		last.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
		last.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
		cl->ResourceBarrier(1, &last);
		CHECK(cl->Close());
		ID3D12CommandList *lists[] = { cl };
		q->ExecuteCommandLists(1, lists);
		CHECK(q->Signal(fence, ++fv));
		while (fence->GetCompletedValue() < fv)
			;
		CHECK(alloc->Reset());
		CHECK(cl->Reset(alloc, nullptr));
	}
#ifdef _WIN32
	printf("native Windows, %d textures x %d rounds\n", n, rounds);
#else
	printf("Linux guest (WSL runtime), %d textures x %d rounds\n", n, rounds);
#endif
	printf("  %-40s %9s %7s %7s %8s\n", "", "mean ns", "p50", "p90", "p99");
	for (int k = 0; k < NOPS; k++) {
		if (k < SUBMIT) {
			std::sort(samples[k], samples[k] + count[k]);
			float *v = samples[k];
			long c = count[k];
			printf("  %-40s %9.0f %7.0f %7.0f %8.0f\n", names[k], total[k] / c,
			       v[c / 2], v[c * 9 / 10], v[c * 99 / 100]);
		} else {
			printf("  %-40s %9.0f\n", names[k], total[k] / count[k]);
		}
	}
	printf("  %-40s %9.0f\n", "ResourceBarrier, 2 transitions in 1 call", pair_total / pair_count);
	if (rp_count)
		printf("  %-40s %9.0f\n", "BeginRenderPass(clear) + EndRenderPass", rp_total / rp_count);
	return 0;
}
