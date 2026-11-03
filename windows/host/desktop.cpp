// Escritorio de Windows en el visor (paquete 7), con el mouse y el teclado del Quest
#include "host.h"
#include <d3d11.h>
#include <dxgi1_2.h>
#include <cmath>
#include <thread>

bool lobbyShowsDesk();

static std::atomic<bool> g_deskOn{false};
static std::mutex errLock;
static std::string g_deskErr;
static VideoEncoder *g_deskEnc;
static std::mutex boundsLock;
static RECT g_deskBounds{0, 0, 1920, 1080};   // la pantalla que se manda (coordenadas del escritorio)

void deskSetOn(bool on) {
    if (on != g_deskOn) logmsg("[escritorio] %s", on ? "encendido" : "apagado");
    g_deskOn = on;
}
bool deskOn() { return g_deskOn; }
void deskRequestKey() { if (g_deskEnc) g_deskEnc->requestKey(); }
std::string deskError() { std::lock_guard<std::mutex> g(errLock); return g_deskErr; }
static void setErr(const std::string &e) {
    std::lock_guard<std::mutex> g(errLock);
    if (e != g_deskErr && !e.empty()) logmsg("[escritorio] %s", e.c_str());
    g_deskErr = e;
}

static void onDeskEncoded(const uint8_t *d, size_t n, bool key, uint64_t tag) {
    int w = (int)(tag >> 16) & 0xFFFF, h = (int)tag & 0xFFFF;
    std::vector<uint8_t> p(6 + n);
    p[0] = 7; p[1] = key ? 1 : 0;
    p[2] = (uint8_t)(w >> 8); p[3] = (uint8_t)w; p[4] = (uint8_t)(h >> 8); p[5] = (uint8_t)h;
    memcpy(p.data() + 6, d, n);
    questSend(std::move(p));
}

/// Una flecha simple donde está el mouse (la captura de Windows no lo trae dibujado)
static void drawCursor(uint8_t *img, int w, int h, int stride, int cx, int cy, float k) {
    static const char *arrow[] = {
        "X", "XX", "XoX", "XooX", "XoooX", "XooooX", "XoooooX", "XooooooX", "XoooooooX", "XooooooooX", "XoooooXXXXX", "XooXooX",
        "XoX XooX", "XX  XooX", "X    XooX", "     XooX", "      XX"};
    int sc = std::max(1, (int)(k + 0.5f));
    for (int r = 0; r < 17; r++)
        for (int c = 0; arrow[r][c]; c++) {
            char ch = arrow[r][c];
            if (ch == ' ') continue;
            uint8_t v = ch == 'X' ? 0 : 255;
            for (int dy = 0; dy < sc; dy++)
                for (int dx = 0; dx < sc; dx++) {
                    int x = cx + c * sc + dx, y = cy + r * sc + dy;
                    if (x < 0 || y < 0 || x >= w || y >= h) continue;
                    uint8_t *p = img + (size_t)y * stride + x * 4;
                    p[0] = p[1] = p[2] = v;
                }
        }
}

static void captureThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ID3D11Device *dev = nullptr;
    ID3D11DeviceContext *ctx = nullptr;
    IDXGIOutputDuplication *dup = nullptr;
    ID3D11Texture2D *staging = nullptr;
    int sw = 0, sh = 0;
    std::vector<uint8_t> frame, scaled, nv;
    uint64_t lastSent = 0;
    auto cleanup = [&] {
        if (dup) { dup->Release(); dup = nullptr; }
        if (staging) { staging->Release(); staging = nullptr; }
        if (ctx) { ctx->Release(); ctx = nullptr; }
        if (dev) { dev->Release(); dev = nullptr; }
    };
    while (true) {
        bool want = questConnected() && g_deskOn && lobbyShowsDesk() && !gameActive();
        if (!want) { if (dup) cleanup(); Sleep(50); continue; }
        if (!dup) {
            D3D_FEATURE_LEVEL fl;
            if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &dev, &fl, &ctx))) {
                setErr(tr("No pude usar la placa de video para capturar la pantalla", "Couldn't use the graphics card to capture the screen"));
                Sleep(2000); continue;
            }
            IDXGIDevice *dd = nullptr;
            IDXGIAdapter *ad = nullptr;
            IDXGIOutput *out = nullptr;
            IDXGIOutput1 *out1 = nullptr;
            dev->QueryInterface(IID_PPV_ARGS(&dd));
            if (dd) dd->GetAdapter(&ad);
            if (ad) ad->EnumOutputs(0, &out);
            if (out) out->QueryInterface(IID_PPV_ARGS(&out1));
            HRESULT hr = out1 ? out1->DuplicateOutput(dev, &dup) : E_FAIL;
            if (out) {
                DXGI_OUTPUT_DESC od;
                out->GetDesc(&od);
                std::lock_guard<std::mutex> g(boundsLock);
                g_deskBounds = od.DesktopCoordinates;
            }
            if (out1) out1->Release();
            if (out) out->Release();
            if (ad) ad->Release();
            if (dd) dd->Release();
            if (FAILED(hr)) {
                setErr(tr("Windows no deja capturar la pantalla (¿otra app la usa, o la pantalla está en otra placa de video?)",
                          "Windows won't let VRmin capture the screen (is another app using it, or is the screen on another graphics card?)"));
                cleanup(); Sleep(2000); continue;
            }
            setErr("");
            deskRequestKey();
        }
        DXGI_OUTDUPL_FRAME_INFO fi;
        IDXGIResource *res = nullptr;
        HRESULT hr = dup->AcquireNextFrame(100, &fi, &res);
        if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
            // (la pantalla no cambió: igual se manda la última de vez en cuando, por si el Quest perdió algo)
            if (!frame.empty() && nowNs() - lastSent < 1000000000ull) continue;
        } else if (FAILED(hr)) { cleanup(); Sleep(100); continue; }
        if (res) {
            ID3D11Texture2D *tex = nullptr;
            res->QueryInterface(IID_PPV_ARGS(&tex));
            if (tex) {
                D3D11_TEXTURE2D_DESC d;
                tex->GetDesc(&d);
                if (!staging || (int)d.Width != sw || (int)d.Height != sh) {
                    if (staging) staging->Release();
                    staging = nullptr;
                    sw = d.Width; sh = d.Height;
                    D3D11_TEXTURE2D_DESC sd = d;
                    sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
                    dev->CreateTexture2D(&sd, nullptr, &staging);
                }
                if (staging) {
                    ctx->CopyResource(staging, tex);
                    D3D11_MAPPED_SUBRESOURCE m;
                    if (SUCCEEDED(ctx->Map(staging, 0, D3D11_MAP_READ, 0, &m))) {
                        frame.resize((size_t)sw * sh * 4);
                        for (int y = 0; y < sh; y++) memcpy(frame.data() + (size_t)y * sw * 4, (uint8_t *)m.pData + (size_t)y * m.RowPitch, (size_t)sw * 4);
                        ctx->Unmap(staging, 0);
                    }
                }
                tex->Release();
            }
            res->Release();
            dup->ReleaseFrame();
        }
        if (frame.empty()) continue;
        // Tamaño que se manda: hasta el ancho elegido, par (lo pide el codificador)
        Settings s;
        { std::lock_guard<std::mutex> g(g_setLock); s = g_set; }
        double k = std::min(1.0, (double)s.deskMaxW / sw);
        int ow = std::max(64, (int)(sw * k) & ~1), oh = std::max(64, (int)(sh * k) & ~1);
        // FPS elegidos
        uint64_t now = nowNs();
        if (now - lastSent < 1000000000ull / (uint64_t)s.deskFps) continue;
        lastSent = now;
        const uint8_t *src = frame.data();
        std::vector<uint8_t> withCursor;
        POINT cp;
        RECT b;
        { std::lock_guard<std::mutex> g(boundsLock); b = g_deskBounds; }
        if (ow == sw && oh == sh) {
            withCursor = frame;
            if (GetCursorPos(&cp)) drawCursor(withCursor.data(), sw, sh, sw * 4, cp.x - b.left, cp.y - b.top, sh / 1080.f);
            src = withCursor.data();
        } else {
            scaled.resize((size_t)ow * oh * 4);
            parallelRows(oh, [&](int y0, int y1) {
                for (int y = y0; y < y1; y++) {
                    double sy = std::min((double)sh - 1.001, std::max(0.0, (y + 0.5) / oh * sh - 0.5));
                    int iy = (int)sy, fy = (int)((sy - iy) * 256);
                    const uint8_t *r0 = frame.data() + (size_t)iy * sw * 4, *r1 = iy + 1 < sh ? r0 + (size_t)sw * 4 : r0;
                    uint8_t *o = scaled.data() + (size_t)y * ow * 4;
                    for (int x = 0; x < ow; x++) {
                        double sx = std::min((double)sw - 1.001, std::max(0.0, (x + 0.5) / ow * sw - 0.5));
                        int ix = (int)sx, fx = (int)((sx - ix) * 256);
                        for (int c = 0; c < 4; c++) {
                            int top = r0[ix * 4 + c] * (256 - fx) + r0[ix * 4 + 4 + c] * fx, bot = r1[ix * 4 + c] * (256 - fx) + r1[ix * 4 + 4 + c] * fx;
                            o[x * 4 + c] = (uint8_t)((top * (256 - fy) + bot * fy) >> 16);
                        }
                    }
                }
            });
            if (GetCursorPos(&cp)) drawCursor(scaled.data(), ow, oh, ow * 4, (int)((cp.x - b.left) * k), (int)((cp.y - b.top) * k), oh / 1080.f);
            src = scaled.data();
        }
        nv.resize((size_t)ow * oh * 3 / 2);
        bgraToNv12(src, ow * 4, ow, oh, nv.data());
        g_deskEnc->encode(nv.data(), ow, oh, s.deskFps, s.deskMbps * 1000000, false, false, ((uint64_t)ow << 16) | (uint64_t)oh);
    }
}

void deskStart() {
    g_deskEnc = new VideoEncoder("escritorio", onDeskEncoded);
    std::thread(captureThread).detach();
}

// ---------------- Mouse ----------------
static bool mouseDownNow = false;
void deskMouse(float u, float v, const Json &down, float scroll) {
    RECT b;
    { std::lock_guard<std::mutex> g(boundsLock); b = g_deskBounds; }
    u = std::min(1.f, std::max(0.f, u)); v = std::min(1.f, std::max(0.f, v));
    int px = b.left + (int)(u * (b.right - b.left)), py = b.top + (int)(v * (b.bottom - b.top));
    int vx = GetSystemMetrics(SM_XVIRTUALSCREEN), vy = GetSystemMetrics(SM_YVIRTUALSCREEN);
    int vw = std::max(1, GetSystemMetrics(SM_CXVIRTUALSCREEN)), vh = std::max(1, GetSystemMetrics(SM_CYVIRTUALSCREEN));
    INPUT in[3]{};
    int n = 0;
    in[n].type = INPUT_MOUSE;
    in[n].mi.dx = (LONG)((px - vx) * 65535.0 / (vw - 1));
    in[n].mi.dy = (LONG)((py - vy) * 65535.0 / (vh - 1));
    in[n].mi.dwFlags = MOUSEEVENTF_MOVE | MOUSEEVENTF_ABSOLUTE | MOUSEEVENTF_VIRTUALDESK;
    n++;
    if (down.t == Json::Bool) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = down.b ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
        mouseDownNow = down.b;
        n++;
    }
    if (std::fabs(scroll) > 0.25f) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = MOUSEEVENTF_WHEEL;
        in[n].mi.mouseData = (DWORD)(int)(scroll * 40);
        n++;
    }
    SendInput(n, in, sizeof(INPUT));
}

// ---------------- Teclado ----------------
bool typingActive() {
    GUITHREADINFO gi{};
    gi.cbSize = sizeof gi;
    return GetGUIThreadInfo(0, &gi) && gi.hwndCaret != nullptr;
}
void typeKeys(const Json &m) {
    std::vector<WORD> mods;
    for (auto &x : m["mods"].a) {
        std::string s = x.str();
        // (cmd del teclado del Quest = Control en Windows: cmd+C copia)
        if (s == "cmd" || s == "ctrl") mods.push_back(VK_CONTROL);
        else if (s == "shift") mods.push_back(VK_SHIFT);
        else if (s == "alt") mods.push_back(VK_MENU);
    }
    std::vector<INPUT> in;
    auto key = [&](WORD vk, bool up) { INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = vk; i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0; in.push_back(i); };
    std::string k = m["k"].str();
    static const std::pair<const char *, WORD> named[] = {{"return", VK_RETURN}, {"tab", VK_TAB}, {"space", VK_SPACE}, {"delete", VK_BACK},
        {"escape", VK_ESCAPE}, {"fdelete", VK_DELETE}, {"left", VK_LEFT}, {"right", VK_RIGHT}, {"down", VK_DOWN}, {"up", VK_UP},
        {"home", VK_HOME}, {"end", VK_END}};
    WORD vk = 0;
    for (auto &p : named) if (k == p.first) vk = p.second;
    if (!vk && k.size() == 1 && k[0] >= 'a' && k[0] <= 'z') vk = (WORD)(k[0] - 'a' + 'A');
    if (vk) {
        for (WORD md : mods) key(md, false);
        key(vk, false); key(vk, true);
        for (auto it = mods.rbegin(); it != mods.rend(); ++it) key(*it, true);
    } else {
        // Texto: se manda como caracteres (no depende de la distribución del teclado de la PC)
        std::wstring w = wide(m["text"].str());
        for (wchar_t c : w) {
            INPUT d{}, u{};
            d.type = u.type = INPUT_KEYBOARD;
            d.ki.wScan = u.ki.wScan = c;
            d.ki.dwFlags = KEYEVENTF_UNICODE;
            u.ki.dwFlags = KEYEVENTF_UNICODE | KEYEVENTF_KEYUP;
            in.push_back(d); in.push_back(u);
        }
    }
    if (!in.empty()) SendInput((UINT)in.size(), in.data(), sizeof(INPUT));
}
