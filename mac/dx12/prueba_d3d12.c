// VRmin · prueba de DirectX 12: dice qué tan completo es el D3D12 que hay (nivel, shader model, ataduras) y dibuja
// colores en una ventana con una cadena de presentación durante unos segundos, midiendo los cuadros por segundo.
// Uso: prueba_d3d12.exe [segundos]
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>
#include <math.h>

#define N 2
#define OK(x, what) do { HRESULT _h = (x); if (FAILED(_h)) { printf("ERROR %s: 0x%08lx\n", what, (unsigned long)_h); fflush(stdout); return 1; } } while (0)

static LRESULT CALLBACK wndproc(HWND w, UINT m, WPARAM a, LPARAM b) {
    if (m == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(w, m, a, b);
}

int main(int argc, char **argv) {
    double secs = argc > 1 ? atof(argv[1]) : 5;
    setvbuf(stdout, NULL, _IONBF, 0);

    IDXGIFactory4 *fac;
    OK(CreateDXGIFactory2(0, &IID_IDXGIFactory4, (void **)&fac), "CreateDXGIFactory2");
    IDXGIAdapter1 *ad;
    OK(IDXGIFactory4_EnumAdapters1(fac, 0, &ad), "EnumAdapters1");
    DXGI_ADAPTER_DESC1 ds;
    IDXGIAdapter1_GetDesc1(ad, &ds);
    printf("Adaptador: %ls (%lu MB)\n", ds.Description, (unsigned long)(ds.DedicatedVideoMemory >> 20));

    ID3D12Device *dev;
    OK(D3D12CreateDevice((IUnknown *)ad, D3D_FEATURE_LEVEL_11_0, &IID_ID3D12Device, (void **)&dev), "D3D12CreateDevice");

    D3D_FEATURE_LEVEL lv[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_12_0, D3D_FEATURE_LEVEL_12_1, (D3D_FEATURE_LEVEL)0xc200 /* 12_2 */};
    D3D12_FEATURE_DATA_FEATURE_LEVELS fl = {5, lv, 0};
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_FEATURE_LEVELS, &fl, sizeof fl)))
        printf("Nivel máximo: %x_%x\n", fl.MaxSupportedFeatureLevel >> 12, (fl.MaxSupportedFeatureLevel >> 8) & 0xf);
    D3D12_FEATURE_DATA_SHADER_MODEL sm = {0x67};
    while (sm.HighestShaderModel >= 0x60 && FAILED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_SHADER_MODEL, &sm, sizeof sm))) sm.HighestShaderModel--;
    printf("Shader model: %x.%x\n", sm.HighestShaderModel >> 4, sm.HighestShaderModel & 0xf);
    D3D12_FEATURE_DATA_D3D12_OPTIONS o = {0};
    if (SUCCEEDED(ID3D12Device_CheckFeatureSupport(dev, D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof o)))
        printf("Ataduras (resource binding tier): %d · recursos (tiled tier): %d · ROVs: %d · conservative raster: %d\n",
               o.ResourceBindingTier, o.TiledResourcesTier, o.ROVsSupported, o.ConservativeRasterizationTier);

    // Ventana y cadena de presentación
    WNDCLASSA wc = {0};
    wc.lpfnWndProc = wndproc; wc.hInstance = GetModuleHandleA(NULL); wc.lpszClassName = "vrmin_d3d12";
    RegisterClassA(&wc);
    HWND win = CreateWindowA("vrmin_d3d12", "VRmin · DX12", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 100, 100, 960, 540, NULL, NULL, wc.hInstance, NULL);

    D3D12_COMMAND_QUEUE_DESC qd = {D3D12_COMMAND_LIST_TYPE_DIRECT};
    ID3D12CommandQueue *q;
    OK(ID3D12Device_CreateCommandQueue(dev, &qd, &IID_ID3D12CommandQueue, (void **)&q), "CreateCommandQueue");
    DXGI_SWAP_CHAIN_DESC1 sd = {0};
    sd.Width = 960; sd.Height = 540; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT; sd.BufferCount = N; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1 *sc1;
    OK(IDXGIFactory4_CreateSwapChainForHwnd(fac, (IUnknown *)q, win, &sd, NULL, NULL, &sc1), "CreateSwapChainForHwnd");
    IDXGISwapChain3 *sc;
    OK(IDXGISwapChain1_QueryInterface(sc1, &IID_IDXGISwapChain3, (void **)&sc), "IDXGISwapChain3");

    D3D12_DESCRIPTOR_HEAP_DESC hd = {D3D12_DESCRIPTOR_HEAP_TYPE_RTV, N};
    ID3D12DescriptorHeap *heap;
    OK(ID3D12Device_CreateDescriptorHeap(dev, &hd, &IID_ID3D12DescriptorHeap, (void **)&heap), "CreateDescriptorHeap");
    UINT inc = ID3D12Device_GetDescriptorHandleIncrementSize(dev, D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE h0;
    // (devuelve una estructura: en el ABI de verdad va por un puntero escondido; los encabezados de MinGW no coinciden)
    ((void (STDMETHODCALLTYPE *)(ID3D12DescriptorHeap *, D3D12_CPU_DESCRIPTOR_HANDLE *))heap->lpVtbl->GetCPUDescriptorHandleForHeapStart)(heap, &h0);
    ID3D12Resource *bb[N];
    for (int i = 0; i < N; i++) {
        OK(IDXGISwapChain3_GetBuffer(sc, i, &IID_ID3D12Resource, (void **)&bb[i]), "GetBuffer");
        D3D12_CPU_DESCRIPTOR_HANDLE h = {h0.ptr + i * inc};
        ID3D12Device_CreateRenderTargetView(dev, bb[i], NULL, h);
    }
    ID3D12CommandAllocator *al;
    OK(ID3D12Device_CreateCommandAllocator(dev, D3D12_COMMAND_LIST_TYPE_DIRECT, &IID_ID3D12CommandAllocator, (void **)&al), "CreateCommandAllocator");
    ID3D12GraphicsCommandList *cl;
    OK(ID3D12Device_CreateCommandList(dev, 0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, NULL, &IID_ID3D12GraphicsCommandList, (void **)&cl), "CreateCommandList");
    ID3D12GraphicsCommandList_Close(cl);
    ID3D12Fence *fence;
    OK(ID3D12Device_CreateFence(dev, 0, D3D12_FENCE_FLAG_NONE, &IID_ID3D12Fence, (void **)&fence), "CreateFence");
    HANDLE ev = CreateEventA(NULL, FALSE, FALSE, NULL);
    UINT64 fv = 0;

    LARGE_INTEGER f, t0, t;
    QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t0);
    int frames = 0;
    for (;;) {
        MSG m;
        while (PeekMessageA(&m, NULL, 0, 0, PM_REMOVE)) { if (m.message == WM_QUIT) goto fin; TranslateMessage(&m); DispatchMessageA(&m); }
        QueryPerformanceCounter(&t);
        double s = (double)(t.QuadPart - t0.QuadPart) / f.QuadPart;
        if (s >= secs) break;
        UINT i = IDXGISwapChain3_GetCurrentBackBufferIndex(sc);
        ID3D12CommandAllocator_Reset(al);
        ID3D12GraphicsCommandList_Reset(cl, al, NULL);
        D3D12_RESOURCE_BARRIER b = {D3D12_RESOURCE_BARRIER_TYPE_TRANSITION};
        b.Transition.pResource = bb[i]; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PRESENT; b.Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
        float c[4] = {0.5f + 0.5f * (float)sin(s * 2), 0.5f + 0.5f * (float)sin(s * 2 + 2), 0.5f + 0.5f * (float)sin(s * 2 + 4), 1};
        D3D12_CPU_DESCRIPTOR_HANDLE h = {h0.ptr + i * inc};
        ID3D12GraphicsCommandList_ClearRenderTargetView(cl, h, c, 0, NULL);
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
        ID3D12GraphicsCommandList_Close(cl);
        ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&cl);
        if (FAILED(IDXGISwapChain3_Present(sc, 1, 0))) { printf("ERROR Present\n"); return 1; }
        ID3D12CommandQueue_Signal(q, fence, ++fv);
        ID3D12Fence_SetEventOnCompletion(fence, fv, ev);
        WaitForSingleObject(ev, 2000);
        frames++;
    }
fin:
    QueryPerformanceCounter(&t);
    printf("Dibujo: %d cuadros en %.1f s (%.0f por segundo)\n", frames, (double)(t.QuadPart - t0.QuadPart) / f.QuadPart,
           frames / ((double)(t.QuadPart - t0.QuadPart) / f.QuadPart));
    printf("OK\n");
    return 0;
}
