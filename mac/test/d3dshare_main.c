/* Prueba (CI): bajo Wine + DXMT crea una textura D3D11 compartida y escribe su nombre de Metal (como la copia directa).
 * El cuerpo de gpu_name se copia del runtime (mac/openxr/questvrbridge_openxr.c) al compilar. */
#define COBJMACROS
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
static void logmsg(const char *f, ...) { va_list a; va_start(a, f); vprintf(f, a); va_end(a); printf("\n"); fflush(stdout); }
#include "gpu_name.inc"
int main(void) {
    ID3D11Device *dev = NULL; ID3D11DeviceContext *ctx = NULL;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, &ctx);
    if (FAILED(hr)) { logmsg("D3D11CreateDevice falló %lx", (unsigned long)hr); return 2; }
    D3D11_TEXTURE2D_DESC d; memset(&d, 0, sizeof d);
    d.Width = 64; d.Height = 32; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    ID3D11Texture2D *t = NULL;
    /* varias combinaciones (la copia directa usa la primera con el formato del juego) */
    DXGI_FORMAT fm[3] = { DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_TYPELESS, DXGI_FORMAT_R8G8B8A8_TYPELESS };
    UINT bf[2] = { D3D11_BIND_SHADER_RESOURCE, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET };
    for (int i = 0; i < 3; i++) for (int j = 0; j < 2; j++) {
        d.Format = fm[i]; d.BindFlags = bf[j];
        ID3D11Texture2D *x = NULL;
        hr = ID3D11Device_CreateTexture2D(dev, &d, NULL, &x);
        logmsg("formato %d bind %u: %lx", (int)fm[i], bf[j], (unsigned long)hr);
        if (SUCCEEDED(hr) && !t) t = x;
    }
    if (!t) { logmsg("CreateTexture2D falló en todas"); return 3; }
    char name[64];
    if (!gpu_name(dev, t, name)) return 4;
    logmsg("NOMBRE %s", name);
    Sleep(20000);
    return 0;
}
