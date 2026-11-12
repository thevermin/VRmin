// Ventanas sueltas: cada ventana de la PC (o cada monitor) en su propia pantalla del Quest, como en Windows y la Mac.
// El Quest pide una con «win» (id, on); se manda codificada en paquetes 8: [8][id 4][clave 1][ancho 2][alto 2][video].
// La lista sale del gestor de ventanas (_NET_CLIENT_LIST); cada ventana se captura con XComposite (también las tapadas
// por otras) o, si no está, con XGetImage; los monitores, como la pantalla principal (desktop.cpp).
#include "x11.h"
#include <cmath>
#include <map>
#include <memory>
#include <thread>
#include <unistd.h>

static bool isDisplay(uint32_t sid) { return (sid & DISPLAY_BIT) != 0; }

// ---------- La lista de ventanas ----------
struct WinInfo { uint32_t id; std::string app, title; ScreenRect r; };
static std::mutex listLock;
static std::vector<WinInfo> g_wins;
static uint64_t g_listAt = 0;

static bool hasAtom(Display *d, const std::vector<unsigned long> &l, const char *name) {
    Atom a = XInternAtom(d, name, False);
    for (unsigned long x : l) if (x == a) return true;
    return false;
}
static std::string titleOf(Display *d, Window w) {
    std::string t;
    Atom got;
    int fmt;
    unsigned long n = 0, after;
    unsigned char *data = nullptr;
    if (XGetWindowProperty(d, w, XInternAtom(d, "_NET_WM_NAME", False), 0, 1024, False, XInternAtom(d, "UTF8_STRING", False), &got, &fmt, &n, &after, &data) == Success && data) {
        if (fmt == 8) t.assign((const char *)data, n);
        XFree(data);
    }
    if (t.empty()) {   // (apps viejas: solo WM_NAME)
        char *nm = nullptr;
        if (XFetchName(d, w, &nm) && nm) { t = nm; XFree(nm); }
    }
    return t;
}
/// El rectángulo de la ventana (sin el marco del gestor de ventanas) en la pantalla
static bool clientRect(Display *d, Window w, ScreenRect &r, XWindowAttributes *out = nullptr) {
    XWindowAttributes wa;
    if (!XGetWindowAttributes(d, w, &wa)) return false;
    int x, y;
    Window child;
    if (!XTranslateCoordinates(d, w, DefaultRootWindow(d), 0, 0, &x, &y, &child)) return false;
    r = {x, y, x + wa.width, y + wa.height};
    if (out) *out = wa;
    return wa.width > 0 && wa.height > 0;
}
static std::vector<WinInfo> listWindows(Display *d) {
    std::vector<WinInfo> out;
    Window root = DefaultRootWindow(d);
    for (unsigned long w : xLongs(d, root, "_NET_CLIENT_LIST", XA_WINDOW)) {
        ScreenRect r;
        XWindowAttributes wa;
        // (sin mostrar: minimizada o en otro escritorio virtual)
        if (!clientRect(d, w, r, &wa) || wa.map_state != IsViewable) continue;
        Window owner = 0;
        if (XGetTransientForHint(d, w, &owner) && owner) continue;   // (diálogos de otra ventana)
        std::vector<unsigned long> state = xLongs(d, w, "_NET_WM_STATE", XA_ATOM), type = xLongs(d, w, "_NET_WM_WINDOW_TYPE", XA_ATOM);
        if (hasAtom(d, state, "_NET_WM_STATE_HIDDEN") || hasAtom(d, state, "_NET_WM_STATE_SKIP_TASKBAR")) continue;
        if (!type.empty() && !hasAtom(d, type, "_NET_WM_WINDOW_TYPE_NORMAL") && !hasAtom(d, type, "_NET_WM_WINDOW_TYPE_DIALOG")) continue;   // (paneles, el escritorio, menús…)
        if (r.right - r.left < 120 || r.bottom - r.top < 80) continue;
        std::vector<unsigned long> pid = xLongs(d, w, "_NET_WM_PID", XA_CARDINAL);
        if (!pid.empty() && pid[0] == (unsigned long)getpid()) continue;   // (VRmin Host mismo no)
        uint32_t id = (uint32_t)w;
        if (isDisplay(id) || id == 0) continue;   // (no se confunde con un monitor)
        std::string title = titleOf(d, w);
        if (title.empty()) continue;
        std::string app;
        XClassHint ch{};
        if (XGetClassHint(d, w, &ch)) {
            app = ch.res_class ? ch.res_class : ch.res_name ? ch.res_name : "";
            if (ch.res_name) XFree(ch.res_name);
            if (ch.res_class) XFree(ch.res_class);
        }
        out.push_back({id, app, title, r});
    }
    return out;
}
static std::vector<WinInfo> windowsNow() {
    std::lock_guard<std::mutex> g(listLock);
    uint64_t now = nowNs();
    // (con un juego abierto se revisa mucho menos: no hace falta y no le roba tiempo)
    if (now - g_listAt > (gameActive() ? 16000000000ull : 2000000000ull) || g_wins.empty()) {
        g_listAt = now;
        std::lock_guard<std::mutex> x(xMainLock);
        Display *d = xMain();
        g_wins = d ? listWindows(d) : std::vector<WinInfo>();
    }
    return g_wins;
}
Json windowsListJson() {
    Json a = Json::array();
    for (auto &w : windowsNow())
        a.push(Json::object().set("id", (double)w.id).set("app", w.app).set("title", w.title).set("w", w.r.right - w.r.left).set("h", w.r.bottom - w.r.top));
    return a;
}

// ---------- Mandar una ventana o un monitor ----------
// Los codificadores se reusan (cada uno tiene su hilo): al cerrar una ventana, el suyo queda libre para la próxima
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

/// La ventana de arriba de todo que contiene a esta (el marco que le pone el gestor de ventanas, o ella misma)
static Window topLevelOf(Display *d, Window w) {
    Window root = DefaultRootWindow(d);
    for (int i = 0; i < 16; i++) {
        Window r, parent = 0, *kids = nullptr;
        unsigned int n = 0;
        if (!XQueryTree(d, w, &r, &parent, &kids, &n)) return w;
        if (kids) XFree(kids);
        if (!parent || parent == root) return w;
        w = parent;
    }
    return w;
}

/// Una ventana: con XComposite se lee de su propia imagen (aunque esté tapada); se achica hasta el ancho elegido y se codifica
static void windowLoop(uint32_t sid, std::shared_ptr<WinStream> st) {
    Display *d = xOpen();
    int ev, er, major = 0, minor = 0;
    bool comp = d && XCompositeQueryExtension(d, &ev, &er) && XCompositeQueryVersion(d, &major, &minor) && (major > 0 || minor >= 2);
    Window redirected = 0;
    std::vector<uint8_t> crop, scaled, nv;
    uint64_t last = 0;
    int lastW = 0, lastH = 0;
    if (d) logmsg("[windows] Showing window %u in the Quest%s", sid, comp ? "" : " (without XComposite: only its visible part)");
    while (d && st->run && questConnected()) {
        Settings s;
        { std::lock_guard<std::mutex> g(g_setLock); s = g_set; }
        int fps = std::min(s.deskFps, 30);
        uint64_t now = nowNs();
        if (now - last < 1000000000ull / (uint64_t)fps) { sleepMs(2); continue; }
        last = now;
        Window w = (Window)sid;
        XWindowAttributes wa;
        ScreenRect r;
        if (!clientRect(d, w, r, &wa)) { logmsg("[windows] Window %u closed", sid); break; }
        if (wa.map_state != IsViewable) { sleepMs(200); continue; }   // (minimizada)
        int cw = wa.width, ch = wa.height;
        if (cw < 16 || ch < 16) { sleepMs(100); continue; }
        if (cw != lastW || ch != lastH) { lastW = cw; lastH = ch; st->enc->requestKey(); }
        XImage *im = nullptr;
        if (comp) {
            // La ventana de arriba (con el marco) queda redirigida mientras se manda: X11 le guarda su imagen entera
            Window top = topLevelOf(d, w);
            if (top != redirected) { XCompositeRedirectWindow(d, top, CompositeRedirectAutomatic); redirected = top; }
            int ox = 0, oy = 0;
            Window child;
            XWindowAttributes ta;
            if (XGetWindowAttributes(d, top, &ta) && XTranslateCoordinates(d, w, top, 0, 0, &ox, &oy, &child)) {
                Pixmap pm = XCompositeNameWindowPixmap(d, top);
                im = XGetImage(d, pm, ox + ta.border_width, oy + ta.border_width, cw, ch, AllPlanes, ZPixmap);
                XFreePixmap(d, pm);
            }
        }
        if (!im) im = XGetImage(d, w, 0, 0, cw, ch, AllPlanes, ZPixmap);
        if (!im) { sleepMs(100); continue; }
        if (im->bits_per_pixel != 32 || im->byte_order != LSBFirst) { XDestroyImage(im); sleepMs(1000); continue; }
        crop.resize((size_t)cw * ch * 4);
        for (int y = 0; y < ch; y++) {
            uint8_t *row = crop.data() + (size_t)y * cw * 4;
            memcpy(row, im->data + (size_t)y * im->bytes_per_line, (size_t)cw * 4);
            for (int x = 0; x < cw; x++) row[x * 4 + 3] = 255;
        }
        XDestroyImage(im);
        // Tamaño que se manda: hasta el ancho elegido, par
        double k = std::min(1.0, (double)s.deskMaxW / cw);
        int ow = std::max(64, (int)(cw * k) & ~1), oh = std::max(64, (int)(ch * k) & ~1);
        const uint8_t *src = crop.data();
        if (ow != cw || oh != ch) {
            if (ow > cw || oh > ch || k < 1.0) {
                scaled.resize((size_t)ow * oh * 4);
                parallelRows(oh, [&](int y0, int y1) {
                    for (int y = y0; y < y1; y++) {
                        int sy = std::min(ch - 1, (int)((y + 0.5) / oh * ch));
                        const uint8_t *rr = crop.data() + (size_t)sy * cw * 4;
                        uint8_t *o = scaled.data() + (size_t)y * ow * 4;
                        for (int x = 0; x < ow; x++) memcpy(o + x * 4, rr + (size_t)std::min(cw - 1, (int)((x + 0.5) / ow * cw)) * 4, 4);
                    }
                });
            } else {
                // (el codificador pide medidas pares: se recorta un píxel)
                scaled.resize((size_t)ow * oh * 4);
                for (int y = 0; y < oh; y++) memcpy(scaled.data() + (size_t)y * ow * 4, crop.data() + (size_t)y * cw * 4, (size_t)ow * 4);
            }
            src = scaled.data();
        }
        nv.resize((size_t)ow * oh * 3 / 2);
        bgraToNv12(src, ow * 4, ow, oh, nv.data());
        st->enc->encode(nv.data(), ow, oh, fps, s.deskMbps * 1000000, false, false, ((uint64_t)ow << 16) | (uint64_t)oh);
    }
    if (d) XCloseDisplay(d);   // (al cerrar la conexión, X11 deja de redirigir la ventana)
    finished(sid, st);
}

static bool windowExists(uint32_t sid) {
    std::lock_guard<std::mutex> g(xMainLock);
    Display *d = xMain();
    XWindowAttributes wa;
    return d && XGetWindowAttributes(d, (Window)sid, &wa);
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
        if (!isDisplay(sid) && !windowExists(sid)) return;
        st = std::make_shared<WinStream>();
        st->target = sid;
        st->pooled = takeEncoder(sid);
        st->enc = st->pooled.enc;
        g_streams[sid] = st;
    }
    if (isDisplay(sid)) {
        std::thread([sid, st] {
            logmsg("[windows] Showing monitor %u in its own screen", sid & ~DISPLAY_BIT);
            DisplayCapture c;
            c.sid = sid;
            c.enc = st->enc;
            c.want = [st] { return st->run && questConnected(); };
            c.stop = [st] { return !st->run || !questConnected(); };
            c.target = [st] { return (uint32_t)st->target; };
            displayCaptureLoop(c);
            finished(sid, st);
        }).detach();
    } else std::thread(windowLoop, sid, st).detach();
}

void winSwap(uint32_t sid, uint32_t to) {
    std::lock_guard<std::mutex> g(streamsLock);
    auto it = g_streams.find(sid);
    if (it == g_streams.end() || !isDisplay(sid)) return;
    it->second->target = isDisplay(to) ? to : sid;
    it->second->enc->requestKey();
    logmsg("[windows] Screen %u now shows monitor %u", sid & ~DISPLAY_BIT, (uint32_t)it->second->target & ~DISPLAY_BIT);
}

/// Un pedido al gestor de ventanas (EWMH): un mensaje a la raíz, como el de un panel (fuente 2)
static void wmMessage(Display *d, Window w, const char *type, long a, long b, long c2, long d2, long e) {
    XEvent ev{};
    ev.xclient.type = ClientMessage;
    ev.xclient.window = w;
    ev.xclient.message_type = XInternAtom(d, type, False);
    ev.xclient.format = 32;
    long l[5] = {a, b, c2, d2, e};
    for (int i = 0; i < 5; i++) ev.xclient.data.l[i] = l[i];
    XSendEvent(d, DefaultRootWindow(d), False, SubstructureRedirectMask | SubstructureNotifyMask, &ev);
}

void winMouse(uint32_t sid, float u, float v, const Json &down, float scroll) {
    ScreenRect r;
    if (isDisplay(sid)) {
        uint32_t target = sid;
        {
            std::lock_guard<std::mutex> g(streamsLock);
            auto it = g_streams.find(sid);
            if (it != g_streams.end()) target = it->second->target;
        }
        bool found = false;
        for (auto &d : displaysList()) if (d.id == target) { r = d.r; found = true; }
        if (!found) return;
    } else {
        std::lock_guard<std::mutex> g(xMainLock);
        Display *d = xMain();
        if (!d || !clientRect(d, (Window)sid, r)) return;
        // Al hacer clic, la ventana pasa adelante (así le llega lo que se escribe)
        if (down.t == Json::Bool && down.b) {
            std::vector<unsigned long> act = xLongs(d, DefaultRootWindow(d), "_NET_ACTIVE_WINDOW", XA_WINDOW);
            if (act.empty() || act[0] != sid) { wmMessage(d, (Window)sid, "_NET_ACTIVE_WINDOW", 2, CurrentTime, 0, 0, 0); XSync(d, False); }
        }
    }
    mouseAt(r, u, v, down, scroll);
}

/// El Quest cambió el tamaño de la pantalla de esa ventana: la ventana de la PC toma la misma forma (sin pasarse del monitor)
void winResize(uint32_t sid, int qw, int qh) {
    if (isDisplay(sid) || qw < 100 || qh < 100) return;
    static std::mutex m;
    static std::map<uint32_t, std::pair<int, int>> last;
    {
        std::lock_guard<std::mutex> g(m);
        if (last[sid] == std::make_pair(qw, qh)) return;
        last[sid] = {qw, qh};
    }
    {
        std::lock_guard<std::mutex> g(xMainLock);
        Display *d = xMain();
        ScreenRect vr;
        if (!d || !clientRect(d, (Window)sid, vr)) return;
        std::vector<unsigned long> state = xLongs(d, (Window)sid, "_NET_WM_STATE", XA_ATOM);
        if (hasAtom(d, state, "_NET_WM_STATE_MAXIMIZED_VERT") || hasAtom(d, state, "_NET_WM_STATE_MAXIMIZED_HORZ") || hasAtom(d, state, "_NET_WM_STATE_FULLSCREEN")) return;
        // El monitor donde está (sin los paneles del escritorio, si el gestor de ventanas los dice)
        int cx = (vr.left + vr.right) / 2, cy = (vr.top + vr.bottom) / 2;
        std::vector<DisplayInfo> mons = monitorsOf(d);
        ScreenRect wa = mons[0].r;
        for (auto &mo : mons) if (cx >= mo.r.left && cx < mo.r.right && cy >= mo.r.top && cy < mo.r.bottom) wa = mo.r;
        std::vector<unsigned long> work = xLongs(d, DefaultRootWindow(d), "_NET_WORKAREA", XA_CARDINAL);
        if (work.size() >= 4) {
            wa.left = std::max(wa.left, (int)work[0]); wa.top = std::max(wa.top, (int)work[1]);
            wa.right = std::min(wa.right, (int)(work[0] + work[2])); wa.bottom = std::min(wa.bottom, (int)(work[1] + work[3]));
        }
        // (el marco del gestor de ventanas también tiene que entrar)
        std::vector<unsigned long> fe = xLongs(d, (Window)sid, "_NET_FRAME_EXTENTS", XA_CARDINAL);
        int fl = fe.size() >= 4 ? (int)fe[0] : 0, fr = fe.size() >= 4 ? (int)fe[1] : 0, ft = fe.size() >= 4 ? (int)fe[2] : 0, fb = fe.size() >= 4 ? (int)fe[3] : 0;
        int aw = wa.right - wa.left - fl - fr, ah = wa.bottom - wa.top - ft - fb;
        if (aw < 100 || ah < 100) return;
        double w = qw, hh = qh, k = std::min({1.0, aw / w, ah / hh});
        int nw = (int)std::lround(w * k), nh = (int)std::lround(hh * k);
        int x = std::min(std::max(vr.left, wa.left + fl), wa.right - fr - nw), y = std::min(std::max(vr.top, wa.top + ft), wa.bottom - fb - nh);
        // (_NET_MOVERESIZE_WINDOW con StaticGravity: la posición y el tamaño son los de adentro de la ventana, sin el marco)
        wmMessage(d, (Window)sid, "_NET_MOVERESIZE_WINDOW", StaticGravity | (0xF << 8) | (2 << 12), x, y, nw, nh);
        XFlush(d);
    }
    std::lock_guard<std::mutex> g(listLock);
    g_listAt = 0;   // (volver a leer las ventanas pronto)
}
