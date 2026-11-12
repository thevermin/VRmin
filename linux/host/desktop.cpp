// Escritorio de Linux en el visor (paquete 7), con el mouse y el teclado del Quest. Por ahora con X11: la pantalla
// se captura con XShm (o XGetImage, si X11 no está en esta PC), los monitores salen de XRandR y el mouse y el
// teclado van con XTest. En Wayland solo se puede con los portales (ScreenCast y RemoteDesktop, con un diálogo en
// la PC que hay que aprobar cada vez): eso todavía no está, y el Quest lo avisa con deskErr.
#include "x11.h"
#include <cmath>
#include <thread>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

bool lobbyShowsDesk();

static std::atomic<bool> g_deskOn{false};
static std::mutex errLock;
static std::string g_deskErr;
static VideoEncoder *g_deskEnc;
static std::mutex boundsLock;
static ScreenRect g_deskBounds{0, 0, 1920, 1080};   // la pantalla que se manda (coordenadas del escritorio)
static std::atomic<uint32_t> g_deskTarget{0};       // el monitor que se ve en la pantalla principal del Quest (0: el principal)

// ---------- La conexión con X11 ----------
std::mutex xMainLock;
thread_local int t_xError = 0;
// (una ventana que se cerró mientras se la leía no es motivo para cerrar el programa, que es lo que hace Xlib si no)
static int xErrors(Display *, XErrorEvent *e) { t_xError = e->error_code; return 0; }
static bool onWayland() {
    const char *t = getenv("XDG_SESSION_TYPE");
    return (getenv("WAYLAND_DISPLAY") && *getenv("WAYLAND_DISPLAY")) || (t && strcmp(t, "wayland") == 0);
}
Display *xOpen() {
    static std::once_flag once;
    std::call_once(once, [] { XInitThreads(); XSetErrorHandler(xErrors); });
    if (onWayland()) return nullptr;   // (XWayland solo ve sus propias ventanas: la pantalla saldría negra)
    return XOpenDisplay(nullptr);
}
Display *xMain() {
    static Display *d = nullptr;
    static uint64_t triedAt = 0;
    if (!d && (triedAt == 0 || nowNs() - triedAt > 5000000000ull)) { triedAt = nowNs(); d = xOpen(); }
    return d;
}
std::vector<unsigned long> xLongs(Display *d, Window w, const char *prop, Atom type) {
    std::vector<unsigned long> out;
    Atom got;
    int fmt;
    unsigned long n = 0, after;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(d, w, XInternAtom(d, prop, False), 0, 4096, False, type, &got, &fmt, &n, &after, &data) == Success && data) {
        if (got == type && fmt == 32) out.assign((unsigned long *)data, (unsigned long *)data + n);   // (Xlib las da en long)
        XFree(data);
    }
    return out;
}

// ---------- Monitores (XRandR) ----------
std::vector<DisplayInfo> monitorsOf(Display *d) {
    std::vector<DisplayInfo> out;
    int ev, er, major = 0, minor = 0;
    if (XRRQueryExtension(d, &ev, &er) && XRRQueryVersion(d, &major, &minor) && (major > 1 || minor >= 5)) {
        int n = 0;
        XRRMonitorInfo *m = XRRGetMonitors(d, DefaultRootWindow(d), True, &n);
        for (int i = 0; m && i < n; i++) {
            DisplayInfo di;
            di.id = DISPLAY_BIT | (uint32_t)(i + 1);
            di.r = {m[i].x, m[i].y, m[i].x + m[i].width, m[i].y + m[i].height};
            di.primary = m[i].primary != 0;
            char *nm = m[i].name ? XGetAtomName(d, m[i].name) : nullptr;
            di.name = nm ? nm : "Monitor " + std::to_string(i + 1);
            if (nm) XFree(nm);
            out.push_back(di);
        }
        if (m) XRRFreeMonitors(m);
    }
    if (out.empty()) {   // (sin XRandR: toda la pantalla)
        int s = DefaultScreen(d);
        out.push_back({DISPLAY_BIT | 1, "Monitor 1", {0, 0, DisplayWidth(d, s), DisplayHeight(d, s)}, true});
    }
    bool any = false;
    for (auto &m : out) any |= m.primary;
    if (!any) {   // (si ninguno dice ser el principal: el de arriba a la izquierda, o el primero)
        DisplayInfo *p = &out[0];
        for (auto &m : out) if (m.r.left == 0 && m.r.top == 0) p = &m;
        p->primary = true;
    }
    return out;
}
std::vector<DisplayInfo> displaysList() {
    std::lock_guard<std::mutex> g(xMainLock);
    Display *d = xMain();
    return d ? monitorsOf(d) : std::vector<DisplayInfo>();
}
uint32_t deskDisplayId() {
    uint32_t t = g_deskTarget;
    if (t) return t;
    for (auto &d : displaysList()) if (d.primary) return d.id;
    return DISPLAY_BIT | 1;
}
void deskSetDisplay(uint32_t id) {
    uint32_t t = id;
    for (auto &d : displaysList()) if (d.id == id && d.primary) t = 0;
    if (t != g_deskTarget) logmsg("[desktop] Main screen in the Quest: %s", t ? ("monitor " + std::to_string(t & ~DISPLAY_BIT)).c_str() : "the main monitor");
    g_deskTarget = t;
    deskRequestKey();
}

void deskSetOn(bool on) {
    if (on != g_deskOn) logmsg("[desktop] %s", on ? "on" : "off");
    g_deskOn = on;
}
bool deskOn() { return g_deskOn; }
void deskRequestKey() { if (g_deskEnc) g_deskEnc->requestKey(); }
std::string deskError() { std::lock_guard<std::mutex> g(errLock); return g_deskErr; }
static void setErr(const std::string &e) {
    std::lock_guard<std::mutex> g(errLock);
    if (e != g_deskErr && !e.empty()) logmsg("[desktop] %s", e.c_str());
    g_deskErr = e;
}

static void onDeskEncoded(const uint8_t *d, size_t n, bool key, uint64_t tag) {
    int w = (int)(tag >> 16) & 0xFFFF, h = (int)(tag & 0xFFFF);
    std::vector<uint8_t> p(6 + n);
    p[0] = 7; p[1] = key ? 1 : 0;
    p[2] = (uint8_t)(w >> 8); p[3] = (uint8_t)w; p[4] = (uint8_t)(h >> 8); p[5] = (uint8_t)h;
    memcpy(p.data() + 6, d, n);
    questSend(std::move(p));
}

/// Una flecha simple donde está el mouse (la captura de X11 no lo trae dibujado)
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

/// ¿La imagen de X11 es BGRA en memoria (lo que espera bgraToNv12)? Es lo normal con color de 24 o 32 bits
static bool isBgra(const XImage *im) {
    return im->bits_per_pixel == 32 && im->byte_order == LSBFirst && im->red_mask == 0xFF0000 && im->green_mask == 0xFF00 && im->blue_mask == 0xFF;
}

/// Captura un monitor y lo manda codificado mientras c.want() diga que sí.
/// La usan la pantalla principal del Quest y las pantallas de monitores que se abren como ventanas (windows.cpp).
void displayCaptureLoop(const DisplayCapture &c) {
    Display *d = nullptr;
    XImage *shmImg = nullptr;
    XShmSegmentInfo shm{};
    bool useShm = true, ready = false;
    ScreenRect bounds;
    uint32_t showing = 0;   // el monitor que se está capturando (g_deskTarget cuando se abrió)
    std::vector<uint8_t> frame, prev, scaled, withCursor, nv;
    uint64_t lastSent = 0, lastGrab = 0;
    int lastCx = -1, lastCy = -1;
    auto freeShm = [&] {
        if (shmImg) {
            XShmDetach(d, &shm);
            XSync(d, False);
            shmImg->data = nullptr;   // (la memoria es la compartida: se suelta abajo)
            XDestroyImage(shmImg);
            shmImg = nullptr;
        }
        if (shm.shmaddr && shm.shmaddr != (char *)-1) shmdt(shm.shmaddr);
        shm = XShmSegmentInfo{};
        ready = false;
    };
    auto cleanup = [&] {
        if (!d) return;
        freeShm();
        XCloseDisplay(d);
        d = nullptr;
    };
    while (true) {
        if (!c.want()) {
            if (d) cleanup();
            if (c.stop && c.stop()) break;
            sleepMs(50);
            continue;
        }
        if (ready && showing != c.target()) { freeShm(); prev.clear(); }   // (se eligió otro monitor)
        if (!d) {
            d = xOpen();
            if (!d) {
                if (c.main) setErr(tr("No pude conectarme con la pantalla de la PC (X11)", "Couldn't connect to the PC's screen (X11)"));
                sleepMs(2000);
                continue;
            }
            int major, minor, pix;
            useShm = XShmQueryVersion(d, &major, &minor, &pix) != 0;
        }
        if (!ready) {
            showing = c.target();
            const DisplayInfo *pick = nullptr;
            std::vector<DisplayInfo> mons = monitorsOf(d);
            for (auto &m : mons) if (showing ? m.id == showing : m.primary) pick = &m;
            if (!pick) {   // (ese monitor ya no está)
                if (c.main) g_deskTarget = 0; else sleepMs(1000);
                continue;
            }
            bounds = pick->r;
            if (c.main) { std::lock_guard<std::mutex> g(boundsLock); g_deskBounds = bounds; }
            int s = DefaultScreen(d), w = bounds.right - bounds.left, h = bounds.bottom - bounds.top;
            if (useShm) {
                // Memoria compartida con X11: la imagen llega sin pasar por el socket
                shmImg = XShmCreateImage(d, DefaultVisual(d, s), DefaultDepth(d, s), ZPixmap, nullptr, &shm, w, h);
                if (shmImg) {
                    shm.shmid = shmget(IPC_PRIVATE, (size_t)shmImg->bytes_per_line * shmImg->height, IPC_CREAT | 0600);
                    shm.shmaddr = shmImg->data = shm.shmid >= 0 ? (char *)shmat(shm.shmid, nullptr, 0) : (char *)-1;
                    shm.readOnly = False;
                    t_xError = 0;
                    if (shm.shmaddr != (char *)-1) { XShmAttach(d, &shm); XSync(d, False); }
                    if (shm.shmid >= 0) shmctl(shm.shmid, IPC_RMID, nullptr);   // (se borra sola cuando nadie la usa)
                    if (shm.shmaddr == (char *)-1 || t_xError) {
                        // (X11 en otra máquina, o sin permiso: con XGetImage, más lento)
                        shmImg->data = nullptr;
                        XDestroyImage(shmImg);
                        shmImg = nullptr;
                        if (shm.shmaddr != (char *)-1) shmdt(shm.shmaddr);
                        shm = XShmSegmentInfo{};
                        useShm = false;
                        logmsg("[desktop] Shared memory with X11 not available: using XGetImage");
                    }
                } else useShm = false;
            }
            ready = true;
            if (c.main) setErr("");
            c.enc->requestKey();
        }
        Settings s;
        { std::lock_guard<std::mutex> g(g_setLock); s = g_set; }
        // FPS elegidos
        uint64_t now = nowNs(), per = 1000000000ull / (uint64_t)std::max(1, s.deskFps);
        if (now - lastGrab < per) { sleepMs(std::max(1, (int)((per - (now - lastGrab)) / 1000000))); continue; }
        lastGrab = now;
        int sw = bounds.right - bounds.left, sh = bounds.bottom - bounds.top;
        Window root = DefaultRootWindow(d);
        XImage *im = shmImg;
        if (shmImg) { if (!XShmGetImage(d, root, shmImg, bounds.left, bounds.top, AllPlanes)) im = nullptr; }
        else im = XGetImage(d, root, bounds.left, bounds.top, sw, sh, AllPlanes, ZPixmap);
        if (!im) { freeShm(); sleepMs(500); continue; }   // (cambió el tamaño de la pantalla: se vuelve a leer)
        bool ok = isBgra(im);
        if (ok) {
            frame.resize((size_t)sw * sh * 4);
            for (int y = 0; y < sh; y++) memcpy(frame.data() + (size_t)y * sw * 4, im->data + (size_t)y * im->bytes_per_line, (size_t)sw * 4);
        }
        if (!shmImg) XDestroyImage(im);
        if (!ok) {
            if (c.main) setErr(tr("La pantalla usa un formato de color que VRmin todavía no captura (hace falta color de 24 bits)",
                                  "The screen uses a color format VRmin can't capture yet (24-bit color is needed)"));
            sleepMs(2000);
            continue;
        }
        Window rr, cr;
        int cx = -100000, cy = -100000, wx, wy;
        unsigned int mask;
        if (!XQueryPointer(d, root, &rr, &cr, &cx, &cy, &wx, &wy, &mask)) cx = cy = -100000;
        // (la pantalla no cambió: igual se manda la última de vez en cuando, por si el Quest perdió algo)
        if (frame == prev && cx == lastCx && cy == lastCy && now - lastSent < 1000000000ull) continue;
        frame.swap(prev);
        lastCx = cx; lastCy = cy;
        lastSent = now;
        // Tamaño que se manda: hasta el ancho elegido, par (lo pide el codificador)
        double k = std::min(1.0, (double)s.deskMaxW / sw);
        int ow = std::max(64, (int)(sw * k) & ~1), oh = std::max(64, (int)(sh * k) & ~1);
        const uint8_t *src;
        ScreenRect b = bounds;
        if (ow == sw && oh == sh) {
            withCursor = prev;
            drawCursor(withCursor.data(), sw, sh, sw * 4, cx - b.left, cy - b.top, sh / 1080.f);
            src = withCursor.data();
        } else {
            const std::vector<uint8_t> &fr = prev;
            scaled.resize((size_t)ow * oh * 4);
            parallelRows(oh, [&](int y0, int y1) {
                for (int y = y0; y < y1; y++) {
                    double sy = std::min((double)sh - 1.001, std::max(0.0, (y + 0.5) / oh * sh - 0.5));
                    int iy = (int)sy, fy = (int)((sy - iy) * 256);
                    const uint8_t *r0 = fr.data() + (size_t)iy * sw * 4, *r1 = iy + 1 < sh ? r0 + (size_t)sw * 4 : r0;
                    uint8_t *o = scaled.data() + (size_t)y * ow * 4;
                    for (int x = 0; x < ow; x++) {
                        double sx = std::min((double)sw - 1.001, std::max(0.0, (x + 0.5) / ow * sw - 0.5));
                        int ix = (int)sx, fx = (int)((sx - ix) * 256);
                        for (int ch = 0; ch < 4; ch++) {
                            int top = r0[ix * 4 + ch] * (256 - fx) + r0[ix * 4 + 4 + ch] * fx, bot = r1[ix * 4 + ch] * (256 - fx) + r1[ix * 4 + 4 + ch] * fx;
                            o[x * 4 + ch] = (uint8_t)((top * (256 - fy) + bot * fy) >> 16);
                        }
                    }
                }
            });
            drawCursor(scaled.data(), ow, oh, ow * 4, (int)((cx - b.left) * k), (int)((cy - b.top) * k), oh / 1080.f);
            src = scaled.data();
        }
        nv.resize((size_t)ow * oh * 3 / 2);
        bgraToNv12(src, ow * 4, ow, oh, nv.data());
        c.enc->encode(nv.data(), ow, oh, s.deskFps, s.deskMbps * 1000000, false, false, ((uint64_t)c.sid << 32) | ((uint64_t)ow << 16) | (uint64_t)oh);
    }
    cleanup();
}

void deskStart() {
    g_deskEnc = new VideoEncoder("desktop", onDeskEncoded);
    if (onWayland()) {
        setErr(tr("En Wayland el escritorio todavía no se puede ver en el visor: entrá a la PC con una sesión X11 (en la pantalla de inicio, «GNOME en Xorg» o «Plasma (X11)»)",
                  "On Wayland the desktop can't be shown in the headset yet: log in to the PC with an X11 session (on the login screen, \"GNOME on Xorg\" or \"Plasma (X11)\")"));
        return;
    }
    if (!getenv("DISPLAY") || !*getenv("DISPLAY")) {
        setErr(tr("No hay una sesión gráfica: abrí VRmin Host desde el escritorio de la PC", "No graphical session: open VRmin Host from the PC's desktop"));
        return;
    }
    std::thread([] {
        DisplayCapture c;
        c.main = true;
        c.enc = g_deskEnc;
        c.want = [] { return questConnected() && g_deskOn && lobbyShowsDesk() && !gameActive(); };
        c.target = [] { return (uint32_t)g_deskTarget; };
        displayCaptureLoop(c);
    }).detach();
}

// ---------------- Mouse ----------------
void deskMouse(float u, float v, const Json &down, float scroll) {
    ScreenRect b;
    { std::lock_guard<std::mutex> g(boundsLock); b = g_deskBounds; }
    mouseAt(b, u, v, down, scroll);
}
void mouseAt(const ScreenRect &b, float u, float v, const Json &down, float scroll) {
    std::lock_guard<std::mutex> g(xMainLock);
    Display *d = xMain();
    if (!d) return;
    u = std::min(1.f, std::max(0.f, u)); v = std::min(1.f, std::max(0.f, v));
    int px = b.left + (int)(u * (b.right - b.left)), py = b.top + (int)(v * (b.bottom - b.top));
    XTestFakeMotionEvent(d, -1, px, py, CurrentTime);
    if (down.t == Json::Bool) XTestFakeButtonEvent(d, 1, down.b ? True : False, CurrentTime);
    // La rueda: en X11 son clics de los botones 4 (arriba) y 5 (abajo); se junta lo que mande el Quest (120 = un paso, como en Windows)
    static float wheel = 0;
    if (std::fabs(scroll) > 0.25f) wheel += scroll * 40;
    for (; wheel >= 120; wheel -= 120) { XTestFakeButtonEvent(d, 4, True, CurrentTime); XTestFakeButtonEvent(d, 4, False, CurrentTime); }
    for (; wheel <= -120; wheel += 120) { XTestFakeButtonEvent(d, 5, True, CurrentTime); XTestFakeButtonEvent(d, 5, False, CurrentTime); }
    XFlush(d);
}

// ---------------- Teclado ----------------
// (X11 no dice si hay un campo de texto con el foco, como el cursor de texto de Windows: el teclado del Quest se abre a mano)
bool typingActive() { return false; }

/// Las letras del texto (UTF-8) como números Unicode
static std::vector<uint32_t> codepoints(const std::string &s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        unsigned char c = s[i];
        int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 0;
        if (!n || i + n > s.size()) { i++; continue; }
        uint32_t cp = n == 1 ? c : c & (0x7F >> n);
        for (int k = 1; k < n; k++) cp = cp << 6 | (s[i + k] & 0x3F);
        out.push_back(cp);
        i += n;
    }
    return out;
}

void typeKeys(const Json &m) {
    std::lock_guard<std::mutex> g(xMainLock);
    Display *d = xMain();
    if (!d) return;
    auto tap = [&](KeyCode kc, bool down) { XTestFakeKeyEvent(d, kc, down ? True : False, CurrentTime); };
    std::vector<KeyCode> mods;
    for (auto &x : m["mods"].a) {
        std::string s = x.str();
        // (cmd del teclado del Quest = Control en Linux: cmd+C copia)
        KeySym ks = s == "cmd" || s == "ctrl" ? XK_Control_L : s == "shift" ? XK_Shift_L : s == "alt" ? XK_Alt_L : NoSymbol;
        if (ks != NoSymbol) if (KeyCode kc = XKeysymToKeycode(d, ks)) mods.push_back(kc);
    }
    std::string k = m["k"].str();
    static const std::pair<const char *, KeySym> named[] = {{"return", XK_Return}, {"tab", XK_Tab}, {"space", XK_space}, {"delete", XK_BackSpace},
        {"escape", XK_Escape}, {"fdelete", XK_Delete}, {"left", XK_Left}, {"right", XK_Right}, {"down", XK_Down}, {"up", XK_Up},
        {"home", XK_Home}, {"end", XK_End}};
    KeySym vk = NoSymbol;
    for (auto &p : named) if (k == p.first) vk = p.second;
    if (vk == NoSymbol && k.size() == 1 && k[0] >= 'a' && k[0] <= 'z') vk = (KeySym)k[0];
    if (vk != NoSymbol) {
        KeyCode kc = XKeysymToKeycode(d, vk);
        if (kc) {
            for (KeyCode md : mods) tap(md, true);
            tap(kc, true); tap(kc, false);
            for (auto it = mods.rbegin(); it != mods.rend(); ++it) tap(*it, false);
        }
        XFlush(d);
        return;
    }
    // Texto: cada letra con la tecla que la escribe (con Shift si hace falta). Si el teclado de la PC no la tiene
    // (ñ en un teclado inglés, €…), se le asigna un rato a una tecla libre (no depende de la distribución del teclado)
    int minKc, maxKc, per = 0;
    XDisplayKeycodes(d, &minKc, &maxKc);
    KeySym *map = XGetKeyboardMapping(d, (KeyCode)minKc, maxKc - minKc + 1, &per);
    if (!map) return;
    KeyCode shift = XKeysymToKeycode(d, XK_Shift_L), spare = 0;
    for (int kc = maxKc; kc >= minKc && !spare; kc--) {
        bool empty = true;
        for (int i = 0; i < per; i++) empty &= map[(kc - minKc) * per + i] == NoSymbol;
        if (empty) spare = (KeyCode)kc;
    }
    bool remapped = false;
    for (uint32_t cp : codepoints(m["text"].str())) {
        KeySym ks = cp == '\n' ? XK_Return : cp == '\t' ? XK_Tab : (cp >= 0x20 && cp < 0x7F) || (cp >= 0xA0 && cp <= 0xFF) ? (KeySym)cp : (KeySym)(0x01000000 | cp);
        KeyCode kc = 0;
        bool sh = false;
        for (int c = minKc; c <= maxKc && !kc; c++)
            for (int col = 0; col < std::min(per, 2) && !kc; col++)
                if (map[(c - minKc) * per + col] == ks) { kc = (KeyCode)c; sh = col == 1; }
        if (!kc && spare) {
            KeySym two[2] = {ks, ks};
            XChangeKeyboardMapping(d, spare, 2, two, 1);
            XSync(d, False);
            sleepMs(20);   // (las apps leen el cambio de teclas antes de que llegue la tecla)
            kc = spare;
            remapped = true;
        }
        if (!kc) continue;
        if (sh && shift) tap(shift, true);
        tap(kc, true); tap(kc, false);
        if (sh && shift) tap(shift, false);
        XSync(d, False);
        if (kc == spare) sleepMs(20);
    }
    if (remapped) {
        sleepMs(30);
        KeySym none[2] = {NoSymbol, NoSymbol};
        XChangeKeyboardMapping(d, spare, 2, none, 1);
    }
    XFree(map);
    XFlush(d);
}
