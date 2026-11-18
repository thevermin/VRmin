// VRmin · prueba de DirectX 12: dice qué tan completo es el D3D12 que hay (nivel, shader model, ataduras) y dibuja
// colores en una ventana con una cadena de presentación durante unos segundos, con un triángulo que gira (shaders de
// HLSL compilados con d3dcompiler, raíz con constantes, estado de tubería), midiendo los cuadros por segundo. Una vez
// lee el píxel del centro para confirmar que el triángulo se dibujó de verdad.
// Uso: prueba_d3d12.exe [segundos]
#define COBJMACROS
#define INITGUID
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <d3dcompiler.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <limits.h>

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
    // Triángulo: shaders de HLSL (modelo 5.0) y una raíz con una constante (el ángulo)
    static const char *hlsl =
        "cbuffer c : register(b0) { float ang; };\n"
        "struct V { float4 p : SV_Position; float3 c : COLOR; };\n"
        "V vs(uint i : SV_VertexID) { V o; float a = ang + i * 2.0943951; o.p = float4(0.6 * sin(a), 0.6 * cos(a), 0, 1);\n"
        "  o.c = float3(i == 0, i == 1, i == 2); return o; }\n"
        "float4 ps(V v) : SV_Target { return float4(v.c, 1); }\n";
    ID3DBlob *vsb, *psb, *err = NULL;
    if (FAILED(D3DCompile(hlsl, strlen(hlsl), "t", NULL, NULL, "vs", "vs_5_0", 0, 0, &vsb, &err)) ||
        FAILED(D3DCompile(hlsl, strlen(hlsl), "t", NULL, NULL, "ps", "ps_5_0", 0, 0, &psb, &err))) {
        printf("ERROR D3DCompile: %s\n", err ? (char *)ID3D10Blob_GetBufferPointer(err) : "?"); return 1;
    }
    D3D12_ROOT_PARAMETER rp = {D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS};
    rp.Constants.Num32BitValues = 1; rp.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    D3D12_ROOT_SIGNATURE_DESC rsd = {1, &rp, 0, NULL, D3D12_ROOT_SIGNATURE_FLAG_NONE};
    ID3DBlob *rsb;
    OK(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsb, &err), "D3D12SerializeRootSignature");
    ID3D12RootSignature *rs;
    OK(ID3D12Device_CreateRootSignature(dev, 0, ID3D10Blob_GetBufferPointer(rsb), ID3D10Blob_GetBufferSize(rsb), &IID_ID3D12RootSignature, (void **)&rs), "CreateRootSignature");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pd = {0};
    pd.pRootSignature = rs;
    pd.VS.pShaderBytecode = ID3D10Blob_GetBufferPointer(vsb); pd.VS.BytecodeLength = ID3D10Blob_GetBufferSize(vsb);
    pd.PS.pShaderBytecode = ID3D10Blob_GetBufferPointer(psb); pd.PS.BytecodeLength = ID3D10Blob_GetBufferSize(psb);
    pd.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pd.SampleMask = UINT_MAX;
    pd.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID; pd.RasterizerState.CullMode = D3D12_CULL_MODE_NONE; pd.RasterizerState.DepthClipEnable = TRUE;
    pd.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pd.NumRenderTargets = 1; pd.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM; pd.SampleDesc.Count = 1;
    ID3D12PipelineState *pso;
    OK(ID3D12Device_CreateGraphicsPipelineState(dev, &pd, &IID_ID3D12PipelineState, (void **)&pso), "CreateGraphicsPipelineState");
    printf("Triángulo: shaders y estado de tubería listos\n");
    // (para leer el píxel del centro una vez)
    D3D12_HEAP_PROPERTIES hp = {D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC rd = {D3D12_RESOURCE_DIMENSION_BUFFER, 0, 256, 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0}, D3D12_TEXTURE_LAYOUT_ROW_MAJOR};
    ID3D12Resource *rb;
    OK(ID3D12Device_CreateCommittedResource(dev, &hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, NULL, &IID_ID3D12Resource, (void **)&rb), "CreateCommittedResource");
    int leido = 0;

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
        ID3D12GraphicsCommandList_OMSetRenderTargets(cl, 1, &h, FALSE, NULL);
        D3D12_VIEWPORT vp = {0, 0, 960, 540, 0, 1}; D3D12_RECT sr = {0, 0, 960, 540};
        ID3D12GraphicsCommandList_RSSetViewports(cl, 1, &vp); ID3D12GraphicsCommandList_RSSetScissorRects(cl, 1, &sr);
        ID3D12GraphicsCommandList_SetGraphicsRootSignature(cl, rs);
        ID3D12GraphicsCommandList_SetPipelineState(cl, pso);
        float ang = (float)s;
        ID3D12GraphicsCommandList_SetGraphicsRoot32BitConstants(cl, 0, 1, &ang, 0);
        ID3D12GraphicsCommandList_IASetPrimitiveTopology(cl, D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        ID3D12GraphicsCommandList_DrawInstanced(cl, 3, 1, 0, 0);
        int leer = frames == 30;
        if (leer) {   // el píxel del centro, a la memoria de lectura
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
            ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
            D3D12_TEXTURE_COPY_LOCATION dst = {rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
            dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM; dst.PlacedFootprint.Footprint.Width = 1;
            dst.PlacedFootprint.Footprint.Height = 1; dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = 256;
            D3D12_TEXTURE_COPY_LOCATION src = {bb[i], D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
            D3D12_BOX box = {480, 270, 0, 481, 271, 1};
            ID3D12GraphicsCommandList_CopyTextureRegion(cl, &dst, 0, 0, 0, &src, &box);
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        } else {
            b.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET; b.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
        }
        ID3D12GraphicsCommandList_ResourceBarrier(cl, 1, &b);
        ID3D12GraphicsCommandList_Close(cl);
        ID3D12CommandQueue_ExecuteCommandLists(q, 1, (ID3D12CommandList **)&cl);
        if (FAILED(IDXGISwapChain3_Present(sc, 1, 0))) { printf("ERROR Present\n"); return 1; }
        ID3D12CommandQueue_Signal(q, fence, ++fv);
        ID3D12Fence_SetEventOnCompletion(fence, fv, ev);
        WaitForSingleObject(ev, 2000);
        if (leer) {
            unsigned char *px; D3D12_RANGE r = {0, 4};
            if (SUCCEEDED(ID3D12Resource_Map(rb, 0, &r, (void **)&px))) {
                // (en el centro los tres colores se mezclan: gris; el fondo es otro color)
                int dif = abs(px[0] - (int)(c[0] * 255)) + abs(px[1] - (int)(c[1] * 255)) + abs(px[2] - (int)(c[2] * 255));
                printf("Triángulo: centro %d,%d,%d · fondo %d,%d,%d → %s\n", px[0], px[1], px[2], (int)(c[0] * 255), (int)(c[1] * 255),
                       (int)(c[2] * 255), dif > 30 ? "se dibujó" : "NO se ve");
                leido = 1;
                ID3D12Resource_Unmap(rb, 0, NULL);
            }
        }
        frames++;
    }
fin:
    QueryPerformanceCounter(&t);
    printf("Dibujo: %d cuadros en %.1f s (%.0f por segundo)\n", frames, (double)(t.QuadPart - t0.QuadPart) / f.QuadPart,
           frames / ((double)(t.QuadPart - t0.QuadPart) / f.QuadPart));
    if (!leido) printf("Triángulo: no se pudo leer el centro\n");
    printf("OK\n");
    return 0;
}
