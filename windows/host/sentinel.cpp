// Modo Centinela: el visor queda apoyado en el escritorio mirándote y sigue los controles; el juego se ve en una ventana
// de la PC (el ojo izquierdo de lo que dibuja el juego, estirado a la ventana en la GPU con Direct2D), con indicaciones
// encima. Al visor va solo 1 cuadro de cada 20. Esc, «Salir» o cerrar la ventana lo terminan.
#include "host.h"
#include <d2d1.h>
#include <dwrite.h>
#include <algorithm>
#include <thread>

void cableProximity(bool keepOn);

static std::atomic<bool> g_on{false};
static std::atomic<HWND> g_win{nullptr};
static std::atomic<uint64_t> lastPoseAt{0}, lastFrameAt{0};
static std::atomic<bool> hadFrames{false};

// El último cuadro (lo deja el hilo del juego, lo toma la ventana)
static std::mutex fLock;
static std::vector<uint8_t> fBuf;
static int fW = 0, fH = 0;
static bool fRgba = false, fNew = false;

bool sentinelOn() { return g_on; }
void sentinelPose() { if (g_on) lastPoseAt = nowNs(); }

/// Un cuadro del juego (dos ojos lado a lado): se copia el ojo izquierdo para la ventana. true: también va al visor
bool sentinelFrame(const uint8_t *src, bool rgba, int ew, int eh) {
    static int skip = 0;
    lastFrameAt = nowNs();
    bool post = false;
    {
        std::lock_guard<std::mutex> g(fLock);
        if (!fNew) {   // (si la ventana todavía no mostró el anterior, este se saltea)
            fBuf.resize((size_t)ew * eh * 4);
            for (int y = 0; y < eh; y++) memcpy(fBuf.data() + (size_t)y * ew * 4, src + (size_t)y * ew * 8, (size_t)ew * 4);
            fW = ew; fH = eh; fRgba = rgba; fNew = true;
            post = true;
        }
    }
    HWND w = g_win;
    if (post && w) PostMessageW(w, WM_APP + 2, 0, 0);
    return ++skip % 20 == 0;
}

// ---------------- La ventana ----------------
static ID2D1Factory *d2f;
static IDWriteFactory *dwf;
static ID2D1HwndRenderTarget *rt;
static ID2D1Bitmap *bmp;
static int bmpW, bmpH;
static bool bmpRgba;
static IDWriteTextFormat *fmtMsg, *fmtTitle, *fmtHint;
static HFONT btnFont;
static HWND btnExit;
static std::vector<HWND> gameBtns;
static std::vector<std::string> gameIds;
static std::string gamesKey, launching;
static std::wstring msgText;
static uint64_t msgUntil = 0;   // 0: queda
static uint64_t proxAt = 0;
static int dpi = 96;
static int px(int v) { return MulDiv(v, dpi, 96); }

template <class T> static void release(T *&p) { if (p) { p->Release(); p = nullptr; } }

static void say(const std::string &code0) {
    // sin juego todavía: el aviso es abrir un juego
    std::string code = code0 == "sincalibrar" && !hadFrames ? "esperando" : code0;
    const char *t;
    if (code == "calibrar") t = tr("Centrando en 3 segundos: mirá la pantalla donde ves el juego, con los dos controles delante del pecho", "Centering in 3 seconds: look at the screen where you see the game, with both controllers in front of your chest");
    else if (code == "listo") t = tr("¡Listo! Centrado: adelante en el juego es hacia el visor", "Done! Centered: forward in the game is toward the headset");
    else if (code == "sincontroles") t = tr("El visor no ve los dos controles: ponete delante del visor, a 1–2 m, con los controles a la vista (si alguno no responde, apretá un botón)", "The headset can't see both controllers: stand in front of it, 1–2 m away, with the controllers in view (if one doesn't respond, press a button)");
    else if (code == "sincalibrar") t = tr("Si el juego mira para otro lado o te ves desde afuera: mirá al visor con los controles delante del pecho y mantené el botón de menú izquierdo (centra y calibra)", "If the game faces the wrong way or you see yourself from outside: look at the headset with the controllers in front of your chest and hold the left menu button (centers and calibrates)");
    else if (code == "sintracking") t = tr("El visor perdió el seguimiento (no sabe dónde está): asegurate de que vea la habitación con luz; si sigue, levantalo, mové un poco la cabeza con él y volvé a apoyarlo", "The headset lost tracking: make sure it sees a lit room; if it continues, pick it up, move it around a bit and set it down again");
    else if (code == "dormidoizq") t = tr("El control izquierdo parece dormido: apretá cualquier botón para despertarlo", "The left controller seems asleep: press any button to wake it");
    else if (code == "dormidoder") t = tr("El control derecho parece dormido: apretá cualquier botón para despertarlo", "The right controller seems asleep: press any button to wake it");
    else if (code == "esperando") t = tr("Modo Centinela listo. Elegí un juego acá o en VRmin Host: se va a ver en esta ventana", "Sentinel Mode ready. Pick a game here or in VRmin Host: it will show in this window");
    else if (code == "recentrado") t = tr("Centrado: adelante en el juego es hacia el visor", "Centered: forward in the game is toward the headset");
    else t = code.c_str();
    std::wstring w = wide(t);
    uint64_t until = code == "calibrar" || code == "esperando" ? 0 : nowNs() + (code == "listo" ? 4 : 8) * 1000000000ull;
    HWND win = g_win;
    // (se cambia en el hilo de la ventana)
    if (win) PostMessageW(win, WM_APP + 4, (WPARAM) new std::wstring(w), (LPARAM) new uint64_t(until));
}

static void render(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    D2D1_SIZE_U sz = D2D1::SizeU(std::max(1L, rc.right), std::max(1L, rc.bottom));
    if (!rt) {
        D2D1_RENDER_TARGET_PROPERTIES p = D2D1::RenderTargetProperties();
        p.dpiX = p.dpiY = 96;   // (todo en píxeles)
        if (FAILED(d2f->CreateHwndRenderTarget(p, D2D1::HwndRenderTargetProperties(h, sz), &rt))) return;
        release(bmp);
    } else rt->Resize(sz);
    {
        std::lock_guard<std::mutex> g(fLock);
        if (fNew) {
            if (!bmp || bmpW != fW || bmpH != fH || bmpRgba != fRgba) {
                release(bmp);
                D2D1_BITMAP_PROPERTIES bp = D2D1::BitmapProperties(D2D1::PixelFormat(fRgba ? DXGI_FORMAT_R8G8B8A8_UNORM : DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_IGNORE));
                if (SUCCEEDED(rt->CreateBitmap(D2D1::SizeU(fW, fH), bp, &bmp))) { bmpW = fW; bmpH = fH; bmpRgba = fRgba; }
            }
            if (bmp) bmp->CopyFromMemory(nullptr, fBuf.data(), fW * 4);
            fNew = false;
            if (!hadFrames) { hadFrames = true; PostMessageW(h, WM_APP + 5, 0, 0); }
        }
    }
    float W = (float)sz.width, H = (float)sz.height;
    rt->BeginDraw();
    rt->Clear(D2D1::ColorF(0, 0, 0));
    ID2D1SolidColorBrush *white = nullptr, *dim = nullptr, *back = nullptr;
    rt->CreateSolidColorBrush(D2D1::ColorF(1, 1, 1), &white);
    rt->CreateSolidColorBrush(D2D1::ColorF(0.7f, 0.7f, 0.72f), &dim);
    rt->CreateSolidColorBrush(D2D1::ColorF(0, 0, 0, 0.6f), &back);
    // El juego dibuja cada ojo cuadrado pero con un campo de visión de pantalla: se estira a toda la ventana
    if (bmp && hadFrames) rt->DrawBitmap(bmp, D2D1::RectF(0, 0, W, H), 1.0f, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    if (!hadFrames && white) {
        bool ready = nowNs() - lastPoseAt < 2000000000ull;
        std::wstring title = wide(!launching.empty() ? std::string(tr("Abriendo ", "Opening ")) + launching + "…"
                                  : !ready ? tr("Esperando al visor…", "Waiting for the headset…")
                                  : gameIds.empty() ? tr("Todavía no hay juegos: instalalos desde Steam", "No games yet: install them from Steam")
                                  : tr("Elegí un juego", "Pick a game"));
        int rows = ((int)gameIds.size() + 2) / 3;
        float top = H / 2 - (rows * px(56)) / 2.0f - px(70);
        if (fmtTitle) rt->DrawText(title.c_str(), (UINT32)title.size(), fmtTitle, D2D1::RectF(0, top, W, top + px(44)), white);
        if (!ready && launching.empty() && fmtHint) {
            std::wstring hint = wide(tr("Los juegos se pueden abrir cuando el visor esté listo. Si tarda, tocá «Empezar» en VRmin Host otra vez o apretá una vez el botón de encendido del visor.",
                                        "Games can be opened once the headset is ready. If it takes long, click “Start” in VRmin Host again or press the headset's power button once."));
            rt->DrawText(hint.c_str(), (UINT32)hint.size(), fmtHint, D2D1::RectF(W / 2 - px(280), top + px(54), W / 2 + px(280), top + px(140)), dim);
        }
    }
    if (!msgText.empty() && (msgUntil == 0 || nowNs() < msgUntil) && fmtMsg && back && white) {
        D2D1_RECT_F r = D2D1::RectF(W * 0.1f, H - px(24) - px(64), W * 0.9f, H - px(24));
        rt->FillRoundedRectangle(D2D1::RoundedRect(r, (float)px(10), (float)px(10)), back);
        D2D1_RECT_F tr2 = D2D1::RectF(r.left + px(12), r.top + px(4), r.right - px(12), r.bottom - px(4));
        rt->DrawText(msgText.c_str(), (UINT32)msgText.size(), fmtMsg, tr2, white);
    }
    release(white); release(dim); release(back);
    if (rt->EndDraw() == (HRESULT)D2DERR_RECREATE_TARGET) { release(bmp); release(rt); }
}

/// Lista de juegos VR de la biblioteca: un botón por juego, de a 3 por fila (se esconde cuando ya se ve el juego)
static void layoutGames(HWND h) {
    RECT rc;
    GetClientRect(h, &rc);
    int bw = px(240), bh = px(44), gap = px(14);
    int rows = ((int)gameBtns.size() + 2) / 3;
    int y0 = (rc.bottom - rows * (bh + gap)) / 2;
    for (size_t i = 0; i < gameBtns.size(); i++) {
        int row = (int)i / 3, col = (int)i % 3;
        int inRow = std::min(3, (int)gameBtns.size() - row * 3);
        int x0 = (rc.right - inRow * bw - (inRow - 1) * gap) / 2;
        SetWindowPos(gameBtns[i], nullptr, x0 + col * (bw + gap), y0 + row * (bh + gap), bw, bh, SWP_NOZORDER);
    }
    if (btnExit) SetWindowPos(btnExit, nullptr, rc.right - px(260) - px(16), px(12), px(260), px(32), SWP_NOZORDER);
}
static void refreshGames(HWND h) {
    bool ready = nowNs() - lastPoseAt < 2000000000ull;
    std::vector<GameEntry> gs;
    if (!hadFrames && ready && launching.empty())
        for (auto &g : gamesList()) if (g.vr) gs.push_back(g);
    std::sort(gs.begin(), gs.end(), [](const GameEntry &a, const GameEntry &b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    std::string key = std::to_string(hadFrames) + launching + std::to_string(ready);
    for (auto &g : gs) key += "," + g.id;
    if (key == gamesKey) return;
    gamesKey = key;
    for (HWND b : gameBtns) DestroyWindow(b);
    gameBtns.clear();
    gameIds.clear();
    for (auto &g : gs) {
        HWND b = CreateWindowW(L"BUTTON", wide(g.name).c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 10, 10, h,
                               (HMENU)(INT_PTR)(1000 + gameIds.size()), nullptr, nullptr);
        SendMessageW(b, WM_SETFONT, (WPARAM)btnFont, TRUE);
        gameBtns.push_back(b);
        gameIds.push_back(g.id);
    }
    layoutGames(h);
    InvalidateRect(h, nullptr, FALSE);
}

void sentinelSet(bool on);
static void exitPressed() { sentinelSet(false); questRequestKey(); }

static LRESULT CALLBACK wndProc(HWND h, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: { PAINTSTRUCT ps; BeginPaint(h, &ps); render(h); EndPaint(h, &ps); return 0; }
    case WM_ERASEBKGND: return 1;
    case WM_SIZE: layoutGames(h); InvalidateRect(h, nullptr, FALSE); return 0;
    case WM_APP + 2: render(h); return 0;   // cuadro nuevo
    case WM_APP + 4: {                      // aviso del visor
        std::wstring *t = (std::wstring *)wp;
        uint64_t *u = (uint64_t *)lp;
        msgText = *t; msgUntil = *u;
        delete t; delete u;
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    case WM_APP + 5: refreshGames(h); return 0;   // empezó a verse el juego: sin lista
    case WM_TIMER:
        // Se cerró el juego (sin cuadros hace 5 s): el Modo Centinela sigue (se sale a mano); vuelve la lista de juegos
        if (hadFrames && nowNs() - lastFrameAt > 5000000000ull) {
            hadFrames = false; launching.clear();
            release(bmp);
        }
        // Mientras sigue: se vuelve a avisar al visor cada 30 s (por si se reinició)
        if (nowNs() - proxAt > 30000000000ull) { proxAt = nowNs(); std::thread(cableProximity, true).detach(); }
        refreshGames(h);
        if (msgUntil && nowNs() >= msgUntil) { msgUntil = 0; msgText.clear(); }
        if (!hadFrames) InvalidateRect(h, nullptr, FALSE);
        return 0;
    case WM_COMMAND:
        if ((HWND)lp == btnExit) exitPressed();
        else if (LOWORD(wp) >= 1000 && LOWORD(wp) - 1000 < (int)gameIds.size()) {
            std::string id = gameIds[LOWORD(wp) - 1000];
            for (auto &g : gamesList()) if (g.id == id) launching = g.name;
            gamesKey.clear();
            refreshGames(h);
            std::thread([id] { gamesLaunch(id); }).detach();
        }
        return 0;
    case WM_CLOSE: exitPressed(); return 0;   // (cerrar la ventana = salir del Modo Centinela)
    case WM_APP + 3: DestroyWindow(h); return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(h, m, wp, lp);
}

static void windowThread() {
    HINSTANCE inst = GetModuleHandleW(nullptr);
    static bool reg = false;
    if (!reg) {
        WNDCLASSW wc{};
        wc.lpfnWndProc = wndProc;
        wc.hInstance = inst;
        wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
        wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
        wc.lpszClassName = L"VRminSentinel";
        RegisterClassW(&wc);
        reg = true;
    }
    if (!d2f) D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, __uuidof(ID2D1Factory), nullptr, (void **)&d2f);
    if (!dwf) DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory), (IUnknown **)&dwf);
    HDC sdc = GetDC(nullptr);
    dpi = GetDeviceCaps(sdc, LOGPIXELSY);
    ReleaseDC(nullptr, sdc);
    if (dwf && !fmtMsg) {
        dwf->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, (float)px(20), L"", &fmtMsg);
        dwf->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_BOLD, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, (float)px(30), L"", &fmtTitle);
        dwf->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, (float)px(15), L"", &fmtHint);
        for (IDWriteTextFormat *f : {fmtMsg, fmtTitle, fmtHint})
            if (f) { f->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER); f->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER); }
    }
    if (!btnFont) btnFont = CreateFontW(-px(15), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    RECT wa{0, 0, 1600, 900};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
    int ww = std::min(px(1440), (int)((wa.right - wa.left) * 0.85)), wh = (int)(ww / 1.54);
    wh = std::min(wh, (int)(wa.bottom - wa.top) - px(20));
    HWND h = CreateWindowW(L"VRminSentinel", wide(tr("VRmin · Modo Centinela", "VRmin · Sentinel Mode")).c_str(), WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
                           wa.left + (wa.right - wa.left - ww) / 2, wa.top + (wa.bottom - wa.top - wh) / 2, ww, wh, nullptr, nullptr, inst, nullptr);
    if (!h) { logmsg("[sentinel] Couldn't open the window"); return; }
    btnExit = CreateWindowW(L"BUTTON", wide(tr("Salir del Modo Centinela (Esc)", "Exit Sentinel Mode (Esc)")).c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
                            0, 0, 10, 10, h, nullptr, inst, nullptr);
    SendMessageW(btnExit, WM_SETFONT, (WPARAM)btnFont, TRUE);
    hadFrames = false; launching.clear(); gamesKey.clear(); proxAt = nowNs();
    g_win = h;
    say("esperando");
    refreshGames(h);
    SetTimer(h, 1, 2000, nullptr);
    ShowWindow(h, SW_SHOWNORMAL);
    SetForegroundWindow(h);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        // Esc en la ventana o en sus botones
        if (msg.message == WM_KEYDOWN && msg.wParam == VK_ESCAPE && (msg.hwnd == h || GetParent(msg.hwnd) == h)) { exitPressed(); continue; }
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    g_win = nullptr;
    btnExit = nullptr;
    gameBtns.clear(); gameIds.clear();
    release(bmp); release(rt);
    msgText.clear(); msgUntil = 0;
}

static std::mutex sLock;
void sentinelSet(bool on) {
    std::lock_guard<std::mutex> g(sLock);
    if (on == g_on) return;
    g_on = on;
    if (on) {
        lastPoseAt = 0;
        std::thread(windowThread).detach();
        std::thread(cableProximity, true).detach();
        logmsg("[sentinel] Sentinel Mode: the game shows on the PC");
    } else {
        HWND w = g_win;
        if (w) PostMessageW(w, WM_APP + 3, 0, 0);
        { std::lock_guard<std::mutex> f(fLock); fNew = false; }
        std::thread(cableProximity, false).detach();
        logmsg("[sentinel] Sentinel Mode ended");
    }
    gameConfigChanged();   // (el runtime adelanta las poses al momento en que se ve el cuadro: no hay reproyección)
    uiRefresh();
}

void sentinelSay(const std::string &code) { if (g_on) say(code); }

/// Prueba (en Windows, sin visor): abre la ventana y le pasa cuadros de prueba durante 15 s
void sentinelSelfTest() {
    sentinelSet(true);
    const int ew = 1024, eh = 1024;
    std::vector<uint8_t> f((size_t)ew * 2 * eh * 4);
    uint64_t t0 = nowNs();
    for (int i = 0; nowNs() - t0 < 15000000000ull; i++) {
        sentinelPose();
        if (nowNs() - t0 > 3000000000ull) {   // (los primeros 3 s, sin juego: la lista)
            for (int y = 0; y < eh; y++)
                for (int x = 0; x < ew * 2; x++) {
                    uint8_t *p = &f[((size_t)y * ew * 2 + x) * 4];
                    bool sq = ((x + i * 4) / 64 + y / 64) % 2;
                    p[0] = (uint8_t)(y * 255 / eh); p[1] = sq ? 200 : 60; p[2] = (uint8_t)(x * 255 / (ew * 2)); p[3] = 255;
                }
            sentinelFrame(f.data(), false, ew, eh);
        }
        if (i == 120) sentinelSay("listo");
        Sleep(33);
    }
    logmsg("[selftest] Sentinel window: %s", g_win.load() ? "open" : "NOT open");
    sentinelSet(false);
    Sleep(500);
}
