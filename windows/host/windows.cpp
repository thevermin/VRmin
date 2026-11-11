// Ventanas sueltas: cada ventana de la PC (o cada monitor) en su propia pantalla del Quest, como en la Mac.
// El Quest pide una con «win» (id, on); se manda codificada en paquetes 8: [8][id 4][clave 1][ancho 2][alto 2][video].
// Las ventanas se capturan con PrintWindow (también las tapadas por otras); los monitores, con la Duplicación de escritorio.
#include "host.h"
#include <dwmapi.h>
#include <psapi.h>
#include <cmath>
#include <map>
#include <thread>

static const uint32_t DISPLAY_BIT = 0x40000000;
static bool isDisplay(uint32_t sid) { return (sid & DISPLAY_BIT) != 0; }

// ---------- La lista de ventanas ----------
struct WinInfo { uint32_t id; HWND h; std::string app, title; RECT r; };
static std::mutex listLock;
static std::vector<WinInfo> g_wins;
static uint64_t g_listAt = 0;

/// El rectángulo que se ve de la ventana (sin los bordes invisibles de Windows 10 y 11)
static bool visibleRect(HWND h, RECT &r) {
    if (SUCCEEDED(DwmGetWindowAttribute(h, DWMWA_EXTENDED_FRAME_BOUNDS, &r, sizeof r))) return r.right > r.left && r.bottom > r.top;
    return GetWindowRect(h, &r) && r.right > r.left && r.bottom > r.top;
}
static std::string appName(HWND h) {
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    std::string name;
    HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (p) {
        wchar_t path[MAX_PATH];
        DWORD n = MAX_PATH;
        if (QueryFullProcessImageNameW(p, 0, path, &n)) {
            std::wstring f = path;
            f = f.substr(f.find_last_of(L'\\') + 1);
            if (f.size() > 4 && _wcsicmp(f.c_str() + f.size() - 4, L".exe") == 0) f.resize(f.size() - 4);
            name = utf8(f);
        }
        CloseHandle(p);
    }
    return name;
}
static BOOL CALLBACK enumWin(HWND h, LPARAM lp) {
    auto *out = (std::vector<WinInfo> *)lp;
    if (!IsWindowVisible(h) || IsIconic(h) || GetWindow(h, GW_OWNER)) return TRUE;
    LONG ex = GetWindowLongW(h, GWL_EXSTYLE);
    if (ex & WS_EX_TOOLWINDOW) return TRUE;
    BOOL cloaked = FALSE;
    DwmGetWindowAttribute(h, DWMWA_CLOAKED, &cloaked, sizeof cloaked);
    if (cloaked) return TRUE;   // (ventanas de otros escritorios virtuales, o apps suspendidas)
    wchar_t t[256];
    int n = GetWindowTextW(h, t, 256);
    if (n <= 0) return TRUE;
    RECT r;
    if (!visibleRect(h, r) || r.right - r.left < 120 || r.bottom - r.top < 80) return TRUE;
    uint32_t id = (uint32_t)(uintptr_t)h;
    if (isDisplay(id) || id == 0) return TRUE;   // (no se confunde con un monitor)
    DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid == GetCurrentProcessId()) return TRUE;   // (VRmin Host mismo no)
    out->push_back({id, h, appName(h), utf8(std::wstring(t, n)), r});
    return TRUE;
}
static std::vector<WinInfo> windowsNow() {
    std::lock_guard<std::mutex> g(listLock);
    uint64_t now = nowNs();
    // (con un juego abierto se revisa mucho menos: no hace falta y no le roba tiempo)
    if (now - g_listAt > (gameActive() ? 16000000000ull : 2000000000ull) || g_wins.empty()) {
        g_listAt = now;
        std::vector<WinInfo> l;
        EnumWindows(enumWin, (LPARAM)&l);
        g_wins = l;
    }
    return g_wins;
}
Json windowsListJson() {
    Json a = Json::array();
    for (auto &w : windowsNow())
        a.push(Json::object().set("id", (double)w.id).set("app", w.app).set("title", w.title).set("w", (int)(w.r.right - w.r.left)).set("h", (int)(w.r.bottom - w.r.top)));
    return a;
}
static HWND hwndOf(uint32_t sid) {
    HWND h = (HWND)(uintptr_t)sid;
    return IsWindow(h) ? h : nullptr;
}

// ---------- Mandar una ventana o un monitor ----------
// Los codificadores se reusan (cada uno tiene su hilo y su codificador de Windows): al cerrar una ventana, el suyo
// queda libre para la próxima
struct Enc { VideoEncoder *enc; std::shared_ptr<std::atomic<uint32_t>> sid; };
static std::mutex poolLock;
static std::vector<Enc> g_free;
static void sendWin(uint32_t sid, const uint8_t *d, size_t n, bool key, uint64_t tag);
static Enc takeEncoder(uint32_t sid) {
    std::lock_guard<std::mutex> g(poolLock);
    if (!g_free.empty()) {
        Enc e = g_free.back();
        g_free.pop_back();
        e.sid->store(sid);
        e.enc->reset();
        return e;
    }
    auto ref = std::make_shared<std::atomic<uint32_t>>(sid);
    VideoEncoder *v = new VideoEncoder("window", [ref](const uint8_t *d, size_t n, bool key, uint64_t tag) { if (uint32_t s = ref->load()) sendWin(s, d, n, key, tag); });
    return {v, ref};
}
static void giveBack(Enc e) {
    e.sid->store(0);   // (lo que todavía salga del codificador ya no se manda)
    std::lock_guard<std::mutex> g(poolLock);
    g_free.push_back(e);
}

struct WinStream {
    std::atomic<bool> run{true};
    std::atomic<uint32_t> target{0};   // (monitores: el que muestra; «Cambiar de monitor» lo cambia)
    VideoEncoder *enc = nullptr;
    Enc pooled;
};
static std::mutex streamsLock;
static std::map<uint32_t, std::shared_ptr<WinStream>> g_streams;
static void finished(uint32_t sid, const std::shared_ptr<WinStream> &st) {
    {
        std::lock_guard<std::mutex> g(streamsLock);
        auto it = g_streams.find(sid);
        if (it != g_streams.end() && it->second == st) g_streams.erase(it);
    }
    giveBack(st->pooled);
}

static void sendWin(uint32_t sid, const uint8_t *d, size_t n, bool key, uint64_t tag) {
    int w = (int)(tag >> 16) & 0xFFFF, h = (int)(tag & 0xFFFF);
    std::vector<uint8_t> p(10 + n);
    p[0] = 8;
    p[1] = (uint8_t)(sid >> 24); p[2] = (uint8_t)(sid >> 16); p[3] = (uint8_t)(sid >> 8); p[4] = (uint8_t)sid;
    p[5] = key ? 1 : 0;
    p[6] = (uint8_t)(w >> 8); p[7] = (uint8_t)w; p[8] = (uint8_t)(h >> 8); p[9] = (uint8_t)h;
    memcpy(p.data() + 10, d, n);
    questSend(std::move(p));
}

/// Una ventana: PrintWindow la dibuja aunque esté tapada; se achica hasta el ancho elegido y se codifica
static void windowLoop(uint32_t sid, std::shared_ptr<WinStream> st) {
    HDC screen = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(screen);
    HBITMAP bmp = nullptr;
    void *bits = nullptr;
    int bw = 0, bh = 0;
    std::vector<uint8_t> crop, scaled, nv;
    uint64_t last = 0;
    logmsg("[windows] Showing window %u in the Quest", sid);
    while (st->run && questConnected()) {
        Settings s;
        { std::lock_guard<std::mutex> g(g_setLock); s = g_set; }
        int fps = std::min(s.deskFps, 30);
        uint64_t now = nowNs();
        if (now - last < 1000000000ull / (uint64_t)fps) { Sleep(2); continue; }
        last = now;
        HWND h = hwndOf(sid);
        if (!h) { logmsg("[windows] Window %u closed", sid); break; }
        if (IsIconic(h)) { Sleep(200); continue; }
        RECT wr, vr;
        if (!GetWindowRect(h, &wr) || !visibleRect(h, vr)) { Sleep(100); continue; }
        int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
        if (ww < 16 || wh < 16) { Sleep(100); continue; }
        if (ww != bw || wh != bh) {
            if (bmp) DeleteObject(bmp);
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof bi.bmiHeader;
            bi.bmiHeader.biWidth = ww; bi.bmiHeader.biHeight = -wh; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
            bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
            bw = ww; bh = wh;
            st->enc->requestKey();
        }
        if (!bmp) { Sleep(500); continue; }
        HGDIOBJ old = SelectObject(mem, bmp);
        BOOL ok = PrintWindow(h, mem, 2 /* PW_RENDERFULLCONTENT: también lo que dibuja la placa de video */);
        SelectObject(mem, old);
        if (!ok) { Sleep(100); continue; }
        GdiFlush();
        // Recorte: lo que se ve (sin los bordes invisibles)
        int cx = std::max(0, (int)(vr.left - wr.left)), cy = std::max(0, (int)(vr.top - wr.top));
        int cw = std::min(ww - cx, (int)(vr.right - vr.left)), ch = std::min(wh - cy, (int)(vr.bottom - vr.top));
        if (cw < 16 || ch < 16) continue;
        crop.resize((size_t)cw * ch * 4);
        for (int y = 0; y < ch; y++) {
            uint8_t *row = crop.data() + (size_t)y * cw * 4;
            memcpy(row, (uint8_t *)bits + ((size_t)(y + cy) * ww + cx) * 4, (size_t)cw * 4);
            for (int x = 0; x < cw; x++) row[x * 4 + 3] = 255;
        }
        // Tamaño que se manda: hasta el ancho elegido, par
        double k = std::min(1.0, (double)s.deskMaxW / cw);
        int ow = std::max(64, (int)(cw * k) & ~1), oh = std::max(64, (int)(ch * k) & ~1);
        const uint8_t *src = crop.data();
        if (ow != cw || oh != ch) {
            scaled.resize((size_t)ow * oh * 4);
            parallelRows(oh, [&](int y0, int y1) {
                for (int y = y0; y < y1; y++) {
                    int sy = std::min(ch - 1, (int)((y + 0.5) / oh * ch));
                    const uint8_t *r = crop.data() + (size_t)sy * cw * 4;
                    uint8_t *o = scaled.data() + (size_t)y * ow * 4;
                    for (int x = 0; x < ow; x++) memcpy(o + x * 4, r + (size_t)std::min(cw - 1, (int)((x + 0.5) / ow * cw)) * 4, 4);
                }
            });
            src = scaled.data();
        } else if ((cw & 1) || (ch & 1)) {
            // (el codificador pide medidas pares: se recorta un píxel)
            scaled.resize((size_t)ow * oh * 4);
            for (int y = 0; y < oh; y++) memcpy(scaled.data() + (size_t)y * ow * 4, crop.data() + (size_t)y * cw * 4, (size_t)ow * 4);
            src = scaled.data();
        }
        depthSubmit(sid, src, ow, oh);   // (3D de esta ventana, si está activado)
        nv.resize((size_t)ow * oh * 3 / 2);
        bgraToNv12(src, ow * 4, ow, oh, nv.data());
        st->enc->encode(nv.data(), ow, oh, fps, s.deskMbps * 1000000, false, false, ((uint64_t)ow << 16) | (uint64_t)oh);
    }
    if (bmp) DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
    finished(sid, st);
}

void winOpen(uint32_t sid, bool on) {
    std::shared_ptr<WinStream> st;
    {
        std::lock_guard<std::mutex> g(streamsLock);
        auto it = g_streams.find(sid);
        if (!on) {
            if (it != g_streams.end()) { it->second->run = false; g_streams.erase(it); logmsg("[windows] Stopped showing %u", sid); }
            return;
        }
        if (it != g_streams.end()) { it->second->enc->requestKey(); return; }   // (ya se manda: una imagen completa)
        if (!isDisplay(sid) && !hwndOf(sid)) return;
        st = std::make_shared<WinStream>();
        st->target = sid;
        st->pooled = takeEncoder(sid);
        st->enc = st->pooled.enc;
        g_streams[sid] = st;
    }
    if (isDisplay(sid)) {
        std::thread([sid, st] {
            bool virt = sid == VMON_SID;   // (el monitor virtual: el que haya puesto VRmin en el escritorio)
            if (virt) logmsg("[windows] Showing the virtual monitor");
            else logmsg("[windows] Showing monitor %u in its own screen", sid & ~DISPLAY_BIT);
            DisplayCapture c;
            c.sid = sid;
            c.enc = st->enc;
            c.want = [st, virt] { return st->run && questConnected() && (!virt || vmonDisplayId()); };
            c.stop = [st] { return !st->run || !questConnected(); };
            c.target = [st, virt] { return virt ? vmonDisplayId() : (uint32_t)st->target; };
            displayCaptureLoop(c);
            finished(sid, st);
        }).detach();
    } else std::thread(windowLoop, sid, st).detach();
}

void winSwap(uint32_t sid, uint32_t to) {
    std::lock_guard<std::mutex> g(streamsLock);
    auto it = g_streams.find(sid);
    if (it == g_streams.end() || !isDisplay(sid) || sid == VMON_SID) return;
    it->second->target = isDisplay(to) ? to : sid;
    it->second->enc->requestKey();
    logmsg("[windows] Screen %u now shows monitor %u", sid & ~DISPLAY_BIT, (uint32_t)it->second->target & ~DISPLAY_BIT);
}

void winMouse(uint32_t sid, float u, float v, const Json &down, float scroll) {
    RECT r;
    if (isDisplay(sid)) {
        uint32_t target = sid == VMON_SID ? vmonDisplayId() : sid;
        if (sid != VMON_SID) {
            std::lock_guard<std::mutex> g(streamsLock);
            auto it = g_streams.find(sid);
            if (it != g_streams.end()) target = it->second->target;
        }
        bool found = false;
        for (auto &d : displaysList()) if (d.id == target) { r = d.r; found = true; }
        if (!found) return;
    } else {
        HWND h = hwndOf(sid);
        if (!h || !visibleRect(h, r)) return;
        // Al hacer clic, la ventana pasa adelante (así le llega lo que se escribe)
        if (down.t == Json::Bool && down.b && GetForegroundWindow() != h) {
            if (IsIconic(h)) ShowWindow(h, SW_RESTORE);
            SetForegroundWindow(h);
        }
    }
    mouseAt(r, u, v, down, scroll);
}

/// El Quest cambió el tamaño de la pantalla de esa ventana: la ventana de la PC toma la misma forma (sin pasarse del monitor)
void winResize(uint32_t sid, int qw, int qh) {
    if (isDisplay(sid) || qw < 100 || qh < 100) return;
    HWND h = hwndOf(sid);
    if (!h || IsZoomed(h)) return;
    static std::mutex m;
    static std::map<uint32_t, std::pair<int, int>> last;
    {
        std::lock_guard<std::mutex> g(m);
        if (last[sid] == std::make_pair(qw, qh)) return;
        last[sid] = {qw, qh};
    }
    MONITORINFO mi{};
    mi.cbSize = sizeof mi;
    GetMonitorInfoW(MonitorFromWindow(h, MONITOR_DEFAULTTONEAREST), &mi);
    RECT wa = mi.rcWork, wr, vr;
    if (!GetWindowRect(h, &wr) || !visibleRect(h, vr)) return;
    // (los bordes invisibles se suman al tamaño pedido)
    int bx = (wr.right - wr.left) - (vr.right - vr.left), by = (wr.bottom - wr.top) - (vr.bottom - vr.top);
    double w = qw, hh = qh, k = std::min({1.0, (wa.right - wa.left) / w, (wa.bottom - wa.top) / hh});
    int nw = (int)std::lround(w * k), nh = (int)std::lround(hh * k);
    int x = std::min(std::max((int)vr.left, (int)wa.left), (int)wa.right - nw), y = std::min(std::max((int)vr.top, (int)wa.top), (int)wa.bottom - nh);
    SetWindowPos(h, nullptr, x - (vr.left - wr.left), y - (vr.top - wr.top), nw + bx, nh + by, SWP_NOZORDER | SWP_NOACTIVATE);
    std::lock_guard<std::mutex> g(listLock);
    g_listAt = 0;   // (volver a leer las ventanas pronto)
}

/// Prueba de la compilación en un Windows de verdad (VRmin Host.exe --selftest): monitores, ventanas y una captura
void winSelfTest() {
    for (auto &d : displaysList()) logmsg("[selftest] Monitor %u: %s %ldx%ld%s", d.id & ~DISPLAY_BIT, d.name.c_str(), d.r.right - d.r.left, d.r.bottom - d.r.top, d.primary ? " (main)" : "");
    std::vector<WinInfo> l;
    EnumWindows(enumWin, (LPARAM)&l);
    logmsg("[selftest] %d windows", (int)l.size());
    for (auto &w : l) {
        RECT wr;
        GetWindowRect(w.h, &wr);
        int ww = wr.right - wr.left, wh = wr.bottom - wr.top;
        HDC screen = GetDC(nullptr), mem = CreateCompatibleDC(screen);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = ww; bi.bmiHeader.biHeight = -wh; bi.bmiHeader.biPlanes = 1; bi.bmiHeader.biBitCount = 32;
        void *bits = nullptr;
        HBITMAP bmp = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, bmp);
        BOOL ok = PrintWindow(w.h, mem, 2);
        SelectObject(mem, old);
        GdiFlush();
        long lit = 0;
        for (long i = 0; ok && bits && i < (long)ww * wh; i += 7) { const uint8_t *p = (const uint8_t *)bits + i * 4; lit += p[0] + p[1] + p[2] > 30; }
        logmsg("[selftest] Window %u \"%s\" (%s) %dx%d: PrintWindow %s, %ld%% not black", w.id, w.title.c_str(), w.app.c_str(), ww, wh, ok ? "ok" : "failed",
               (ww > 0 && wh > 0) ? lit * 700 / ((long)ww * wh) : 0L);
        DeleteObject(bmp);
        DeleteDC(mem);
        ReleaseDC(nullptr, screen);
    }
}
