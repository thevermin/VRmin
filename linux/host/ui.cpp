// La ventana de VRmin Host para Linux: la misma página que en Windows (ui.html, dentro del programa) en WebKitGTK, por
// medio de webview. La página pide el estado cada segundo y llama a las acciones con window.vrmin(acción, dato); con
// state.os == "linux" esconde lo que es solo de Windows. Sin pantalla (o sin WebKitGTK al compilar), no hay ventana.
#include "host.h"
#include <csignal>
#include <cstdlib>
#include <spawn.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#ifdef VRMIN_UI
#include "webview/webview.h"

extern char **environ;
void settingsApply(const Json &m);
std::string gamesStatus();
bool gamesBusy();
bool vrRuntimeInstalled();
bool vrOcInstalled();
std::string pcGpu();
int pcVram();

// La página va dentro del programa (ui.html es el mismo archivo que usa el de Windows)
__asm__(".section .rodata\n.global vrmin_ui_html\nvrmin_ui_html:\n.incbin \"ui.html\"\n"
        ".global vrmin_ui_html_end\nvrmin_ui_html_end:\n.byte 0\n.previous\n");
extern "C" const char vrmin_ui_html[], vrmin_ui_html_end[];

/// Las últimas líneas del registro (del final de VRmin.log)
static std::vector<std::string> logTail() {
    std::vector<std::string> out;
    FILE *f = fopen((dataDir() + "/VRmin.log").c_str(), "rb");
    if (!f) return out;
    fseek(f, 0, SEEK_END);
    long size = ftell(f), from = size > 65536 ? size - 65536 : 0;
    fseek(f, from, SEEK_SET);
    std::string s(size - from, '\0');
    s.resize(fread(&s[0], 1, s.size(), f));
    fclose(f);
    size_t i = 0;
    if (from > 0) { i = s.find('\n'); i = i == std::string::npos ? s.size() : i + 1; }   // (la primera línea, cortada)
    while (i < s.size()) {
        size_t e = s.find('\n', i);
        if (e == std::string::npos) e = s.size();
        out.push_back(s.substr(i, e - i));
        i = e + 1;
    }
    if (out.size() > 300) out.erase(out.begin(), out.end() - 300);
    return out;
}

/// Abre una carpeta o una página con el programa del escritorio (xdg-open), sin esperarlo
static void xdgOpen(const std::string &what) {
    pid_t pid;
    const char *argv[] = {"xdg-open", what.c_str(), nullptr};
    if (posix_spawnp(&pid, "xdg-open", nullptr, nullptr, (char *const *)argv, environ) == 0)
        std::thread([pid] { int st; waitpid(pid, &st, 0); }).detach();
    else logmsg("[window] Couldn't open %s (xdg-open is missing)", what.c_str());
}

// ---------- Estado para la página ----------
static Json stateJson() {
    Settings s;
    { std::lock_guard<std::mutex> g(g_setLock); s = g_set; }
    std::string running = gamesRunningName();   // (la carpeta del juego abierto)
    Json prof = profilesState();
    Json games = Json::array();
    for (auto &ge : gamesList())
        games.push(Json::object().set("id", ge.id).set("name", ge.name).set("dir", ge.dir).set("vr", ge.vr).set("open", !running.empty() && running == ge.dir)
                       .set("own", prof["own"].has(ge.dir)));
    std::string stats;
    if (gameActive()) {
        Json p = perfSnapshot();
        char b[160];
        snprintf(b, sizeof b, "%.0f fps · %s %.1f ms · %s %.1f ms", p["fps"].num(0), tr("juego", "game"), p["juego"].num(0), tr("video", "video"), p["video"].num(0));
        stats = b;
    }
    std::vector<std::string> tail = logTail();
    Json o = Json::object();
    o.set("os", "linux").set("lang", IDIOMA_ES ? "es" : "en").set("version", VRMIN_LINUX_VERSION).set("pairCode", (int)g_pairCode)
     .set("quest", Json::object().set("connected", questConnected()).set("wifi", (bool)g_questWifi))
     .set("setup", Json::object().set("runtimeOk", vrRuntimeInstalled()).set("ocOk", vrOcInstalled()))
     .set("games", games).set("askWorked", Json()).set("busy", gamesBusy() ? gamesStatus() : "").set("stats", stats)
     .set("lastLog", tail.empty() ? std::string() : tail.back())
     .set("cfg", Json::object().set("eye", s.eye).set("fps", s.fps).set("mbps", s.mbps).set("codec", s.hevc ? "hevc" : "h264").set("fov", s.fov).set("audio", s.audio))
     .set("gpu", Json::object().set("name", pcGpu()).set("vram", pcVram())).set("prof", prof).set("dataDir", dataDir());
    return o;
}

/// Una acción de la página: devuelve lo que la página recibe (JSON)
static std::string handle(const std::string &cmd, const Json &a) {
    if (cmd == "state") return stateJson().dump();
    if (cmd == "log") {
        Json l = Json::array();
        for (auto &s : logTail()) l.push(s);
        return l.dump();
    }
    if (cmd == "settings") { settingsApply(a); return "true"; }
    if (cmd == "prof") { profilesApply(a); return "true"; }
    if (cmd == "play") { gamesLaunch(a.str()); return "true"; }
    if (cmd == "quit") { std::thread(gamesQuit).detach(); return "true"; }
    if (cmd == "logs" || cmd == "data") { xdgOpen(dataDir()); return "true"; }
    if (cmd == "openUrl") {
        // (solo las páginas de VRmin)
        for (const char *p : {"https://ko-fi.com/thevermin", "https://github.com/thevermin/"})
            if (a.str().rfind(p, 0) == 0) { xdgOpen(a.str()); break; }
        return "true";
    }
    if (cmd == "lang") {
        IDIOMA_ES = a.str() == "es";
        { std::lock_guard<std::mutex> g(g_setLock); g_set.lang = IDIOMA_ES ? "es" : "en"; }
        saveSettings();
        return "true";
    }
    if (cmd == "forget") {
        unlink((dataDir() + "/paired.json").c_str());
        logmsg("[quest] Paired Quests forgotten: they'll ask for the code next time");
        return "true";
    }
    if (cmd == "jsError") { logmsg("[window] Page error: %s", a.str().c_str()); return "true"; }
    return "null";
}

static std::mutex winLock;
static webview::webview *g_win;   // la ventana, mientras está abierta

/// Abre la ventana y espera a que se cierre (o a Ctrl+C / SIGTERM / SIGHUP, que la cierran). false si no hay pantalla.
bool uiRun(const sigset_t &quit) {
    const char *x = getenv("DISPLAY"), *wl = getenv("WAYLAND_DISPLAY");
    if (!(x && *x) && !(wl && *wl)) { logmsg("[window] No display: running without a window"); return false; }
    try {
        webview::webview w(false, nullptr);
        w.set_title("VRmin Host");
        // (que entre en la pantalla: en pantallas chicas, más chica)
        int ww = 980, wh = 760;
        GdkDisplay *d = gdk_display_get_default();
        GdkMonitor *m = d ? gdk_display_get_primary_monitor(d) : nullptr;
        if (!m && d) m = gdk_display_get_monitor(d, 0);
        if (m) {
            GdkRectangle wa;
            gdk_monitor_get_workarea(m, &wa);
            ww = std::min(ww, wa.width - 40);
            wh = std::min(wh, wa.height - 60);
        }
        w.set_size(std::min(720, ww), std::min(520, wh), WEBVIEW_HINT_MIN);
        w.set_size(ww, wh, WEBVIEW_HINT_NONE);
        gtk_window_set_position(GTK_WINDOW(w.window().value()), GTK_WIN_POS_CENTER);
        w.bind("vrmin", [](const std::string &req) -> std::string {
            Json a = Json::parse(req);   // ["acción", dato]
            if (a.t != Json::Arr || a.a.empty()) return "null";
            return handle(a.a[0].str(), a.a.size() > 1 ? a.a[1] : Json());
        });
        // (los errores de la página van al registro)
        w.init("window.addEventListener('error', e => window.vrmin && window.vrmin('jsError', e.message + ' (' + e.lineno + ':' + e.colno + ')'));"
               "window.addEventListener('unhandledrejection', e => window.vrmin && window.vrmin('jsError', String(e.reason)));");
        // (como set_html, pero con una dirección de base que no existe: sin eso, WebKit esconde el detalle de los errores)
        std::string page(vrmin_ui_html, vrmin_ui_html_end - vrmin_ui_html);
        webkit_web_view_load_html(WEBKIT_WEB_VIEW(w.browser_controller().value()), page.c_str(), "https://vrmin.invalid/");
        // Las señales para cerrar (bloqueadas en todos los hilos) las espera este hilo, que cierra la ventana
        { std::lock_guard<std::mutex> g(winLock); g_win = &w; }
        std::thread([quit] {
            int sig = 0;
            sigwait(&quit, &sig);
            std::lock_guard<std::mutex> g(winLock);
            if (g_win) g_win->terminate();
        }).detach();
        logmsg("[window] VRmin Host window open");
        w.run();
        { std::lock_guard<std::mutex> g(winLock); g_win = nullptr; }
    } catch (const std::exception &e) {
        logmsg("[window] Couldn't open the window (%s): running without a window", e.what());
        return false;
    }
    logmsg("[window] Window closed");
    return true;
}

#else
bool uiRun(const sigset_t &) { logmsg("[window] Built without a window (WebKitGTK was missing)"); return false; }
#endif
