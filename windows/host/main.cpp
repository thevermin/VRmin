// VRmin Host para Windows: una ventana chica con el estado; todo lo demás se maneja desde el Quest
#include "host.h"
#include <shellapi.h>
#include <objbase.h>
#include <mmsystem.h>
#include <thread>

void lobbyStart();

static HWND g_wnd, g_btnPrep, g_btnRestore, g_btnLogs;
static HFONT g_font, g_fontBig, g_fontTitle;
static const COLORREF BG = RGB(18, 18, 20), FG = RGB(235, 235, 235), DIM = RGB(150, 150, 155), AMBER = RGB(255, 183, 77);

void uiRefresh() { if (g_wnd) PostMessageW(g_wnd, WM_APP + 1, 0, 0); }

static void drawLine(HDC dc, int &y, const std::string &s, HFONT f, COLORREF c, int h) {
    SelectObject(dc, f);
    SetTextColor(dc, c);
    RECT r{24, y, 536, y + h};
    std::wstring w = wide(s);
    DrawTextW(dc, w.c_str(), (int)w.size(), &r, DT_LEFT | DT_WORDBREAK | DT_NOPREFIX);
    y += h;
}

static void paint(HWND w) {
    PAINTSTRUCT ps;
    HDC dc = BeginPaint(w, &ps);
    RECT rc;
    GetClientRect(w, &rc);
    HBRUSH bg = CreateSolidBrush(BG);
    FillRect(dc, &rc, bg);
    DeleteObject(bg);
    SetBkMode(dc, TRANSPARENT);
    int y = 18;
    drawLine(dc, y, "VRmin Host", g_fontTitle, FG, 36);
    drawLine(dc, y, std::string(tr("Windows · beta ", "Windows · beta ")) + VRMIN_WIN_VERSION, g_font, DIM, 30);
    int code = g_pairCode;
    if (code >= 0) {
        char c[16];
        snprintf(c, sizeof c, "%03d %03d", code / 1000, code % 1000);
        drawLine(dc, y, tr("Escribí este código en el Quest para emparejarlo:", "Type this code on the Quest to pair it:"), g_font, FG, 24);
        drawLine(dc, y, c, g_fontBig, AMBER, 56);
    }
    if (questConnected())
        drawLine(dc, y, std::string("● ") + tr("Quest conectado", "Quest connected") + (g_questWifi ? tr(" por Wi-Fi", " over Wi-Fi") : tr(" por cable", " over cable")), g_font, RGB(120, 220, 140), 26);
    else
        drawLine(dc, y, std::string("○ ") + tr("Esperando al Quest: abrí VRmin en el visor (misma red Wi-Fi)", "Waiting for the Quest: open VRmin on the headset (same Wi-Fi)"), g_font, DIM, 44);
    std::string game = gameActive() ? gamesRunningName() : "";
    drawLine(dc, y, gameActive() ? std::string(tr("Jugando: ", "Playing: ")) + (game.empty() ? "?" : game) : tr("Ningún juego abierto", "No game open"), g_font, FG, 30);
    SetupState s = setupCheck();
    drawLine(dc, y, s.runtimeOk && s.ocOk ? tr("Juegos VR: listos (VRmin reemplaza a SteamVR mientras esta ventana esté abierta)", "VR games: ready (VRmin replaces SteamVR while this window is open)")
                                          : tr("Juegos VR: falta preparar Windows (se hace solo al abrir el primer juego, o con el botón)", "VR games: Windows isn't set up yet (it happens when you open your first game, or with the button)"),
             g_font, s.runtimeOk && s.ocOk ? DIM : AMBER, 44);
    EndPaint(w, &ps);
}

static LRESULT CALLBACK wndProc(HWND w, UINT m, WPARAM wp, LPARAM lp) {
    switch (m) {
    case WM_PAINT: paint(w); return 0;
    case WM_APP + 1: InvalidateRect(w, nullptr, FALSE); return 0;
    case WM_TIMER: InvalidateRect(w, nullptr, FALSE); return 0;
    case WM_CTLCOLORBTN: return (LRESULT)GetStockObject(BLACK_BRUSH);
    case WM_COMMAND:
        if ((HWND)lp == g_btnPrep) CreateThread(nullptr, 0, [](LPVOID) -> DWORD { setupActivate(); return 0; }, nullptr, 0, nullptr);
        else if ((HWND)lp == g_btnRestore) CreateThread(nullptr, 0, [](LPVOID) -> DWORD { setupRestoreSteamVR(); return 0; }, nullptr, 0, nullptr);
        else if ((HWND)lp == g_btnLogs) ShellExecuteW(nullptr, L"open", dataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        return 0;
    case WM_DESTROY:
        setupOnExit();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(w, m, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    HANDLE single = CreateMutexW(nullptr, TRUE, L"VRminHostWindows");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        HWND other = FindWindowW(L"VRminHost", nullptr);
        if (other) { ShowWindow(other, SW_RESTORE); SetForegroundWindow(other); }
        return 0;
    }
    (void)single;
    IDIOMA_ES = PRIMARYLANGID(GetUserDefaultUILanguage()) == LANG_SPANISH;
    SetProcessDPIAware();
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    WSADATA wd;
    WSAStartup(MAKEWORD(2, 2), &wd);
    timeBeginPeriod(1);
    SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS);   // (es video en vivo: que Windows lo atienda primero)
    logmsg("VRmin Host for Windows %s", VRMIN_WIN_VERSION);
    loadSettings();
    applySettings();

    questStart();
    gameStart();
    deskStart();
    audioStart();
    lobbyStart();
    std::thread(gamesScan).detach();
    // Si Windows ya estaba preparado de otra vez, OpenComposite vuelve a quedar activo (al cerrar se saca)
    if (setupCheck().runtimeOk) std::thread([] { setupActivate(); }).detach();

    WNDCLASSW wc{};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = inst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(1));
    wc.lpszClassName = L"VRminHost";
    RegisterClassW(&wc);
    int dpi = GetDeviceCaps(GetDC(nullptr), LOGPIXELSY);
    auto px = [&](int v) { return MulDiv(v, dpi, 96); };
    g_font = CreateFontW(-px(15), 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontTitle = CreateFontW(-px(26), 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Segoe UI");
    g_fontBig = CreateFontW(-px(44), 0, 0, 0, FW_BOLD, 0, 0, 0, DEFAULT_CHARSET, 0, 0, CLEARTYPE_QUALITY, 0, L"Consolas");
    g_wnd = CreateWindowW(L"VRminHost", L"VRmin Host", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX, CW_USEDEFAULT, CW_USEDEFAULT,
                          px(580), px(500), nullptr, nullptr, inst, nullptr);
    // (los botones se acomodan con el mismo tamaño de letra que el resto)
    auto button = [&](const char *t, int x, int y, int w) {
        HWND b = CreateWindowW(L"BUTTON", wide(t).c_str(), WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, px(x), px(y), px(w), px(32), g_wnd, nullptr, inst, nullptr);
        SendMessageW(b, WM_SETFONT, (WPARAM)g_font, TRUE);
        return b;
    };
    g_btnPrep = button(tr("Preparar juegos VR", "Set up VR games"), 24, 390, 170);
    g_btnRestore = button(tr("Volver a SteamVR", "Back to SteamVR"), 204, 390, 160);
    g_btnLogs = button(tr("Registros", "Logs"), 374, 390, 120);
    SetTimer(g_wnd, 1, 1000, nullptr);
    ShowWindow(g_wnd, show);
    UpdateWindow(g_wnd);
    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) { TranslateMessage(&msg); DispatchMessageW(&msg); }
    logmsg("VRmin Host closed");
    return 0;
}
