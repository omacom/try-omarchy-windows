/* A Direct3D 11 window for Wine tests: a rotating triangle drawn many times
 * per frame, presented without vsync; prints frames per second.
 *
 *   gcc -O2 -o d3d11tri.exe d3d11tri.c -ld3d11 -ld3dcompiler -ldxguid -luser32
 *   wine d3d11tri.exe [seconds]
 */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <math.h>

static const char shader[] =
    "cbuffer c : register(b0) { float4 rot; };\n"
    "struct V { float4 p : SV_Position; float3 c : COLOR; };\n"
    "V vs(float2 p : POSITION, float3 c : COLOR) { V o;\n"
    "  o.p = float4(p.x * rot.x - p.y * rot.y, p.x * rot.y + p.y * rot.x, 0, 1);\n"
    "  o.c = c; return o; }\n"
    "float4 ps(V i) : SV_Target { return float4(i.c, 1); }\n";

static LRESULT CALLBACK proc(HWND w, UINT m, WPARAM a, LPARAM b)
{
    if (m == WM_DESTROY) PostQuitMessage(0);
    return DefWindowProcA(w, m, a, b);
}

int main(int argc, char **argv)
{
    double seconds = argc > 1 ? atof(argv[1]) : 6;
    WNDCLASSA wc = { .lpfnWndProc = proc, .hInstance = GetModuleHandleA(NULL),
                     .lpszClassName = "d3d11tri" };
    RegisterClassA(&wc);
    HWND win = CreateWindowA("d3d11tri", "d3d11tri", WS_OVERLAPPEDWINDOW | WS_VISIBLE,
                             100, 100, 1280, 720, NULL, NULL, wc.hInstance, NULL);
    DXGI_SWAP_CHAIN_DESC sd = { .BufferCount = 2, .BufferDesc.Width = 1280,
        .BufferDesc.Height = 720, .BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM,
        .BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT, .OutputWindow = win,
        .SampleDesc.Count = 1, .Windowed = TRUE };
    ID3D11Device *dev; ID3D11DeviceContext *ctx; IDXGISwapChain *sc;
    D3D_FEATURE_LEVEL level;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0,
        NULL, 0, D3D11_SDK_VERSION, &sd, &sc, &dev, &level, &ctx);
    if (FAILED(hr)) { printf("FAIL: device 0x%08lx\n", hr); return 1; }

    IDXGIDevice *xd; IDXGIAdapter *ad; DXGI_ADAPTER_DESC desc;
    ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&xd);
    IDXGIDevice_GetAdapter(xd, &ad); IDXGIAdapter_GetDesc(ad, &desc);
    printf("feature level 0x%x on %ls\n", level, desc.Description);

    ID3D11Texture2D *bb; ID3D11RenderTargetView *rtv;
    IDXGISwapChain_GetBuffer(sc, 0, &IID_ID3D11Texture2D, (void **)&bb);
    ID3D11Device_CreateRenderTargetView(dev, (ID3D11Resource *)bb, NULL, &rtv);

    ID3DBlob *vsb, *psb, *err = NULL;
    if (FAILED(D3DCompile(shader, sizeof(shader) - 1, NULL, NULL, NULL, "vs", "vs_4_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(shader, sizeof(shader) - 1, NULL, NULL, NULL, "ps", "ps_4_0", 0, 0, &psb, &err))) {
        printf("FAIL: shader %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?");
        return 1;
    }
    ID3D11VertexShader *vs; ID3D11PixelShader *ps; ID3D11InputLayout *il;
    ID3D11Device_CreateVertexShader(dev, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), NULL, &vs);
    ID3D11Device_CreatePixelShader(dev, ID3D10Blob_GetBufferPointer(psb), ID3D10Blob_GetBufferSize(psb), NULL, &ps);
    D3D11_INPUT_ELEMENT_DESC ie[] = {
        { "POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0 },
        { "COLOR", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 8, D3D11_INPUT_PER_VERTEX_DATA, 0 } };
    ID3D11Device_CreateInputLayout(dev, ie, 2, ID3D10Blob_GetBufferPointer(vsb), ID3D10Blob_GetBufferSize(vsb), &il);

    float verts[] = { 0, .8f, 1, 0, 0,  .7f, -.6f, 0, 1, 0,  -.7f, -.6f, 0, 0, 1 };
    D3D11_BUFFER_DESC bd = { sizeof(verts), D3D11_USAGE_DEFAULT, D3D11_BIND_VERTEX_BUFFER };
    D3D11_SUBRESOURCE_DATA init = { verts };
    ID3D11Buffer *vb, *cb;
    ID3D11Device_CreateBuffer(dev, &bd, &init, &vb);
    D3D11_BUFFER_DESC cd = { 16, D3D11_USAGE_DEFAULT, D3D11_BIND_CONSTANT_BUFFER };
    ID3D11Device_CreateBuffer(dev, &cd, NULL, &cb);

    D3D11_VIEWPORT vp = { 0, 0, 1280, 720, 0, 1 };
    UINT stride = 20, offset = 0;
    LARGE_INTEGER f, t0, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    unsigned frames = 0, total = 0; double last = 0;
    for (;;) {
        MSG msg;
        while (PeekMessageA(&msg, NULL, 0, 0, PM_REMOVE)) {
            if (msg.message == WM_QUIT) return 0;
            DispatchMessageA(&msg);
        }
        QueryPerformanceCounter(&t);
        double now = (double)(t.QuadPart - t0.QuadPart) / f.QuadPart;
        float clear[4] = { .05f, .05f, .1f, 1 };
        ID3D11DeviceContext_OMSetRenderTargets(ctx, 1, &rtv, NULL);
        ID3D11DeviceContext_RSSetViewports(ctx, 1, &vp);
        ID3D11DeviceContext_ClearRenderTargetView(ctx, rtv, clear);
        ID3D11DeviceContext_IASetInputLayout(ctx, il);
        ID3D11DeviceContext_IASetPrimitiveTopology(ctx, D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D11DeviceContext_IASetVertexBuffers(ctx, 0, 1, &vb, &stride, &offset);
        ID3D11DeviceContext_VSSetShader(ctx, vs, NULL, 0);
        ID3D11DeviceContext_PSSetShader(ctx, ps, NULL, 0);
        ID3D11DeviceContext_VSSetConstantBuffers(ctx, 0, 1, &cb);
        for (int i = 0; i < 200; i++) {       /* 200 draws per frame */
            float rot[4] = { (float)cos(now + i * .0314), (float)sin(now + i * .0314), 0, 0 };
            ID3D11DeviceContext_UpdateSubresource(ctx, (ID3D11Resource *)cb, 0, NULL, rot, 0, 0);
            ID3D11DeviceContext_Draw(ctx, 3, 0);
        }
        IDXGISwapChain_Present(sc, 0, 0);
        frames++; total++;
        if (now - last >= 1) {
            printf("%u fps\n", frames); fflush(stdout);
            frames = 0; last = now;
        }
        if (now >= seconds) break;
    }
    printf("PASS: %u frames in %.1f s (%.0f fps average)\n", total, seconds, total / seconds);
    return 0;
}
