// SPDX-License-Identifier: MIT
/*
 * Zero-copy presentation check, on the Windows host alone: a shared D3D12
 * render target (what a guest's scanout texture is on the host) is cleared
 * on the GPU, exported as an NT handle, imported into OpenGL with
 * GL_EXT_memory_object_win32 (what QEMU's SDL display renders with) and
 * read back. A shared D3D12 fence, imported with GL_EXT_semaphore_win32,
 * orders the GL read after the D3D12 clear.
 *
 *   gcc -O2 -o interoptest.exe interoptest.c -ld3d12 -ldxgi -lopengl32 -lgdi32
 */
#define COBJMACROS
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <windows.h>
#include <initguid.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <stdio.h>
#include <stdint.h>

#define W 2560
#define H 1600

static PFNGLCREATEMEMORYOBJECTSEXTPROC pglCreateMemoryObjectsEXT;
static PFNGLMEMORYOBJECTPARAMETERIVEXTPROC pglMemoryObjectParameterivEXT;
static PFNGLIMPORTMEMORYWIN32HANDLEEXTPROC pglImportMemoryWin32HandleEXT;
static PFNGLTEXSTORAGEMEM2DEXTPROC pglTexStorageMem2DEXT;
static PFNGLGENSEMAPHORESEXTPROC pglGenSemaphoresEXT;
static PFNGLIMPORTSEMAPHOREWIN32HANDLEEXTPROC pglImportSemaphoreWin32HandleEXT;
static PFNGLSEMAPHOREPARAMETERUI64VEXTPROC pglSemaphoreParameterui64vEXT;
static PFNGLWAITSEMAPHOREEXTPROC pglWaitSemaphoreEXT;
static PFNGLGENFRAMEBUFFERSPROC pglGenFramebuffers;
static PFNGLBINDFRAMEBUFFERPROC pglBindFramebuffer;
static PFNGLFRAMEBUFFERTEXTURE2DPROC pglFramebufferTexture2D;
static PFNGLCHECKFRAMEBUFFERSTATUSPROC pglCheckFramebufferStatus;
static PFNGLBLITFRAMEBUFFERPROC pglBlitFramebuffer;

#define LOAD(f) (p##f = (void *)wglGetProcAddress(#f))

static double now_ms(void)
{
	LARGE_INTEGER f, t;

	QueryPerformanceFrequency(&f);
	QueryPerformanceCounter(&t);
	return t.QuadPart * 1000.0 / f.QuadPart;
}

static int fail(const char *what, HRESULT hr)
{
	printf("FAIL %s: 0x%08lx\n", what, (unsigned long)hr);
	return 1;
}

static HGLRC gl_context(void)
{
	PIXELFORMATDESCRIPTOR pfd = { sizeof(pfd), 1,
		PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER,
		PFD_TYPE_RGBA, 32 };
	HWND wnd = CreateWindowA("STATIC", "interop", WS_POPUP, 0, 0, 16, 16,
				 NULL, NULL, NULL, NULL);
	HDC dc = GetDC(wnd);
	HGLRC rc;

	SetPixelFormat(dc, ChoosePixelFormat(dc, &pfd), &pfd);
	rc = wglCreateContext(dc);
	wglMakeCurrent(dc, rc);
	return rc;
}

int main(void)
{
	IDXGIFactory6 *factory;
	IDXGIAdapter1 *adapter;
	DXGI_ADAPTER_DESC1 ad;
	ID3D12Device *dev;
	ID3D12CommandQueue *queue;
	ID3D12CommandAllocator *alloc;
	ID3D12GraphicsCommandList *list;
	ID3D12DescriptorHeap *rtvs;
	ID3D12Resource *tex;
	ID3D12Fence *fence;
	HANDLE tex_nt, fence_nt;
	D3D12_RESOURCE_ALLOCATION_INFO info;
	D3D12_CPU_DESCRIPTOR_HANDLE rtv;
	HRESULT hr;

	setvbuf(stdout, NULL, _IONBF, 0);
	hr = CreateDXGIFactory2(0, &IID_IDXGIFactory6, (void **)&factory);
	if (FAILED(hr))
		return fail("CreateDXGIFactory2", hr);
	IDXGIFactory6_EnumAdapterByGpuPreference(factory, 0,
		DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, &IID_IDXGIAdapter1,
		(void **)&adapter);
	IDXGIAdapter1_GetDesc1(adapter, &ad);
	printf("adapter: %ls\n", ad.Description);
	hr = D3D12CreateDevice((IUnknown *)adapter, D3D_FEATURE_LEVEL_11_0,
			       &IID_ID3D12Device, (void **)&dev);
	if (FAILED(hr))
		return fail("D3D12CreateDevice", hr);

	D3D12_COMMAND_QUEUE_DESC qd = { D3D12_COMMAND_LIST_TYPE_DIRECT };
	ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue,
					(void **)&queue);
	ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT,
		&IID_ID3D12CommandAllocator, (void **)&alloc);
	ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT,
		alloc, NULL, &IID_ID3D12GraphicsCommandList, (void **)&list);

	/* The scanout texture: what Mesa creates for a shared KMS mirror. */
	D3D12_HEAP_PROPERTIES heap = { D3D12_HEAP_TYPE_DEFAULT };
	D3D12_RESOURCE_DESC rd = { D3D12_RESOURCE_DIMENSION_TEXTURE2D, 0, W, H,
		1, 1, DXGI_FORMAT_B8G8R8A8_UNORM, { 1, 0 },
		D3D12_TEXTURE_LAYOUT_UNKNOWN,
		D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET };
	D3D12_CLEAR_VALUE cv = { DXGI_FORMAT_B8G8R8A8_UNORM,
				 { { 1.0f, 0.5f, 0.25f, 1.0f } } };
	hr = ID3D12Device_CreateCommittedResource(dev, &heap,
		D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_RENDER_TARGET,
		&cv, &IID_ID3D12Resource, (void **)&tex);
	if (FAILED(hr))
		return fail("CreateCommittedResource(shared)", hr);
	dev->lpVtbl->GetResourceAllocationInfo(dev, &info, 0, 1, &rd);

	D3D12_DESCRIPTOR_HEAP_DESC hd = { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 1 };
	ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap,
					  (void **)&rtvs);
	rtvs->lpVtbl->GetCPUDescriptorHandleForHeapStart(rtvs, &rtv);
	ID3D12Device_CreateRenderTargetView(dev, tex, NULL, rtv);
	ID3D12GraphicsCommandList_ClearRenderTargetView(list, rtv,
		cv.Color, 0, NULL);
	/* Shared resources leave the queue in COMMON for other APIs. */
	D3D12_RESOURCE_BARRIER b = { D3D12_RESOURCE_BARRIER_TYPE_TRANSITION };
	b.Transition.pResource = tex;
	b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
	b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
	b.Transition.StateAfter = D3D12_RESOURCE_STATE_COMMON;
	ID3D12GraphicsCommandList_ResourceBarrier(list, 1, &b);
	ID3D12GraphicsCommandList_Close(list);

	hr = ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_SHARED,
				      &IID_ID3D12Fence, (void **)&fence);
	if (FAILED(hr))
		return fail("CreateFence(shared)", hr);
	hr = ID3D12Device_CreateSharedHandle(dev, (ID3D12DeviceChild *)tex, NULL,
					     GENERIC_ALL, NULL, &tex_nt);
	if (FAILED(hr))
		return fail("CreateSharedHandle(texture)", hr);
	hr = ID3D12Device_CreateSharedHandle(dev, (ID3D12DeviceChild *)fence,
					     NULL, GENERIC_ALL, NULL, &fence_nt);
	if (FAILED(hr))
		return fail("CreateSharedHandle(fence)", hr);

	/* OpenGL side: QEMU's display. */
	gl_context();
	const char *ext = (const char *)glGetString(GL_EXTENSIONS);
	printf("GL: %s, %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));
	printf("GL_EXT_memory_object_win32: %s, GL_EXT_semaphore_win32: %s\n",
	       strstr(ext, "GL_EXT_memory_object_win32") ? "yes" : "no",
	       strstr(ext, "GL_EXT_semaphore_win32") ? "yes" : "no");
	LOAD(glCreateMemoryObjectsEXT);
	LOAD(glMemoryObjectParameterivEXT);
	LOAD(glImportMemoryWin32HandleEXT);
	LOAD(glTexStorageMem2DEXT);
	LOAD(glGenSemaphoresEXT);
	LOAD(glImportSemaphoreWin32HandleEXT);
	LOAD(glSemaphoreParameterui64vEXT);
	LOAD(glWaitSemaphoreEXT);
	LOAD(glGenFramebuffers);
	LOAD(glBindFramebuffer);
	LOAD(glFramebufferTexture2D);
	LOAD(glCheckFramebufferStatus);
	LOAD(glBlitFramebuffer);
	if (!pglImportMemoryWin32HandleEXT || !pglTexStorageMem2DEXT ||
	    !pglImportSemaphoreWin32HandleEXT)
		return fail("GL interop entry points", 0);

	GLuint mem, gtex, sem, fbo[2], dst;
	GLint dedicated = GL_TRUE;

	pglCreateMemoryObjectsEXT(1, &mem);
	pglMemoryObjectParameterivEXT(mem, GL_DEDICATED_MEMORY_OBJECT_EXT,
				      &dedicated);
	pglImportMemoryWin32HandleEXT(mem, info.SizeInBytes,
				      GL_HANDLE_TYPE_D3D12_RESOURCE_EXT, tex_nt);
	glGenTextures(1, &gtex);
	glBindTexture(GL_TEXTURE_2D, gtex);
	pglTexStorageMem2DEXT(GL_TEXTURE_2D, 1, GL_RGBA8, W, H, mem, 0);
	printf("import texture (%llu bytes): GL error 0x%x\n",
	       (unsigned long long)info.SizeInBytes, glGetError());

	pglGenSemaphoresEXT(1, &sem);
	pglImportSemaphoreWin32HandleEXT(sem, GL_HANDLE_TYPE_D3D12_FENCE_EXT,
					 fence_nt);
	printf("import fence: GL error 0x%x\n", glGetError());

	/* D3D12 clears and signals 1; GL waits for 1 on the GPU, then reads. */
	ID3D12CommandQueue_ExecuteCommandLists(queue, 1,
					       (ID3D12CommandList **)&list);
	ID3D12CommandQueue_Signal(queue, fence, 1);
	GLuint64 value = 1;
	GLenum layout = GL_LAYOUT_SHADER_READ_ONLY_EXT;

	pglSemaphoreParameterui64vEXT(sem, GL_D3D12_FENCE_VALUE_EXT, &value);
	pglWaitSemaphoreEXT(sem, 0, NULL, 1, &gtex, &layout);

	pglGenFramebuffers(2, fbo);
	pglBindFramebuffer(GL_READ_FRAMEBUFFER, fbo[0]);
	pglFramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				GL_TEXTURE_2D, gtex, 0);
	printf("framebuffer: 0x%x\n", pglCheckFramebufferStatus(GL_READ_FRAMEBUFFER));
	unsigned char px[4] = {0};

	glReadPixels(W / 2, H / 2, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
	printf("pixel read through GL as RGBA8: %u %u %u %u "
	       "(cleared to B8G8R8A8 r=255 g=128 b=64)\n", px[0], px[1], px[2], px[3]);
	int ok = (px[0] == 255 && px[2] == 64) || (px[0] == 64 && px[2] == 255);

	/* What presenting costs: a full-screen GPU blit per frame. */
	glGenTextures(1, &dst);
	glBindTexture(GL_TEXTURE_2D, dst);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA,
		     GL_UNSIGNED_BYTE, NULL);
	pglBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo[1]);
	pglFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
				GL_TEXTURE_2D, dst, 0);
	glFinish();
	double t0 = now_ms();
	for (int i = 0; i < 240; i++)
		pglBlitFramebuffer(0, 0, W, H, 0, 0, W, H, GL_COLOR_BUFFER_BIT,
				   GL_NEAREST);
	glFinish();
	printf("2560x1600 blit from the shared texture: %.3f ms each\n",
	       (now_ms() - t0) / 240);
	printf("%s\n", ok ? "INTEROP TEST PASSED" : "INTEROP TEST FAILED");
	return ok ? 0 : 1;
}
