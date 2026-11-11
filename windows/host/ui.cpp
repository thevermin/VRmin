// La ventana de VRmin Host: una página (ui.html, dentro del .exe) en WebView2, el navegador de Windows. La página pide
// el estado cada segundo y llama a las acciones con window.vrmin(acción, dato). Sin WebView2, queda la ventana chica.
#include "host.h"
#include <shellapi.h>
#include <tlhelp32.h>
#include <set>
#include <thread>
#include "webview/webview.h"

void settingsApply(const Json &m);
std::string pcGpuName();
int pcVram();
std::string pcGpu();
bool reportMake(bool fromHeadset);
std::vector<std::string> logTail();
std::set<std::string> gamesOpenDirs();
std::string gameStatusOf(const std::string &id);
void gameStatusSet(const std::string &id, const std::string &status);
bool steamInstalled();
bool steamRunning();
void steamOpen();
void steamClose();

// ---------- La PC: abrir al iniciar Windows y no suspender ----------
static const wchar_t *RUN_KEY = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
static bool openAtLogin() {
    return RegGetValueW(HKEY_CURRENT_USER, RUN_KEY, L"VRmin Host", RRF_RT_REG_SZ, nullptr, nullptr, nullptr) == ERROR_SUCCESS;
}
static void setOpenAtLogin(bool on) {
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, RUN_KEY, 0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (on) {
        wchar_t exe[MAX_PATH];
        GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring v = L"\"" + std::wstring(exe) + L"\"";
        RegSetValueExW(k, L"VRmin Host", 0, REG_SZ, (const BYTE *)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    } else RegDeleteValueW(k, L"VRmin Host");
    RegCloseKey(k);
    logmsg("[pc] Open VRmin Host when Windows starts: %s", on ? "yes" : "no");
}
static bool keepAwake() { std::lock_guard<std::mutex> g(g_setLock); return g_set.keepAwake; }
static void awakeThread() {
    // (el «no suspender» de Windows vale para el hilo que lo pide: este hilo vive mientras VRmin Host esté abierto)
    bool on = false;
    while (true) {
        bool want = keepAwake() && questConnected();
        if (want != on) {
            SetThreadExecutionState(want ? ES_CONTINUOUS | ES_SYSTEM_REQUIRED : ES_CONTINUOUS);
            on = want;
        }
        Sleep(2000);
    }
}

// ---------- «¿Funcionó?»: al cerrar un juego que no se había probado ----------
static std::mutex askLock;
static Json g_ask;                 // {id, name} o nada
static std::set<std::string> wasOpen;
static void watchGames(const std::vector<GameEntry> &games, const std::set<std::string> &open) {
    std::lock_guard<std::mutex> g(askLock);
    for (auto &d : wasOpen)
        if (!open.count(d))
            for (auto &ge : games) {
                std::string low = ge.dir;
                for (auto &c : low) c = (char)tolower((unsigned char)c);
                std::string st = gameStatusOf(ge.id);
                if (low == d && ge.vr && (st.empty() || st == "sin probar")) g_ask = Json::object().set("id", ge.id).set("name", ge.name);
            }
    wasOpen = open;
}

// ---------- Estado para la página ----------
static Json stateJson() {
    Settings s;
    { std::lock_guard<std::mutex> g(g_setLock); s = g_set; }
    SetupState su = setupCheck();
    std::set<std::string> open = gamesOpenDirs();
    std::vector<GameEntry> list = gamesList();
    watchGames(list, open);
    Json prof = profilesState();
    Json games = Json::array();
    for (auto &ge : list) {
        std::string low = ge.dir;
        for (auto &c : low) c = (char)tolower((unsigned char)c);
        games.push(Json::object().set("id", ge.id).set("name", ge.name).set("dir", ge.dir).set("vr", ge.vr).set("open", open.count(low) > 0)
                       .set("status", gameStatusOf(ge.id)).set("own", prof["own"].has(ge.dir)));
    }
    std::string stats;
    if (gameActive()) {
        Json p = perfSnapshot();
        char b[160];
        snprintf(b, sizeof b, "%.0f fps · %s %.1f ms · %s %.1f ms", p["fps"].num(0), tr("juego", "game"), p["juego"].num(0), tr("video", "video"), p["video"].num(0));
        stats = b;
    }
    std::vector<std::string> tail = logTail();
    std::string busy = gamesBusy() ? gamesStatus() : "";
    Json ask;
    { std::lock_guard<std::mutex> g(askLock); ask = g_ask; }
    Json o = Json::object();
    o.set("lang", IDIOMA_ES ? "es" : "en").set("version", VRMIN_WIN_VERSION).set("pairCode", (int)g_pairCode)
     .set("quest", Json::object().set("connected", questConnected()).set("wifi", (bool)g_questWifi))
     .set("setup", Json::object().set("runtimeOk", su.runtimeOk).set("ocOk", su.ocOk))
     .set("steam", Json::object().set("installed", steamInstalled()).set("running", steamRunning()))
     .set("games", games).set("askWorked", ask).set("busy", busy).set("stats", stats).set("lastLog", tail.empty() ? std::string() : tail.back())
     .set("cfg", Json::object().set("eye", s.eye).set("fps", s.fps).set("mbps", s.mbps).set("codec", s.hevc ? "hevc" : "h264").set("fov", s.fov).set("audio", s.audio))
     .set("gpu", Json::object().set("name", pcGpu()).set("vram", pcVram())).set("prof", prof)
     .set("mic", Json::object().set("on", micEnabled()).set("cable", micCableInstalled()))
     .set("pc", Json::object().set("openAtLogin", openAtLogin()).set("keepAwake", s.keepAwake)).set("dataDir", utf8(dataDir()))
     .set("cable", cableState()).set("upd", updatesState()).set("sentinel", sentinelOn()).set("vmon", vmonState())
     .set("rec", Json::object().set("on", recording()).set("secs", recording() ? (double)((nowNs() - recordingSince()) / 1000000000ull) : 0.0).set("quest", s.recQuest));
    return o;
}

static void openUrl(const std::string &u) {
    // (solo las páginas de VRmin y de lo que hace falta instalar)
    static const char *ok[] = {"https://ko-fi.com/thevermin", "https://github.com/thevermin/", "https://vb-audio.com/Cable/", "https://store.steampowered.com/about/"};
    for (const char *p : ok)
        if (u.rfind(p, 0) == 0) { ShellExecuteW(nullptr, L"open", wide(u).c_str(), nullptr, nullptr, SW_SHOWNORMAL); return; }
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
    if (cmd == "gameStatus") { gameStatusSet(a["id"].str(), a["status"].str()); return "true"; }
    if (cmd == "worked") {
        Json ask;
        { std::lock_guard<std::mutex> g(askLock); ask = g_ask; g_ask = Json(); }
        if (!a.str().empty() && ask.has("id")) gameStatusSet(ask["id"].str(), a.str());
        return "true";
    }
    if (cmd == "prepare") { std::thread([] { setupActivate(); }).detach(); return "true"; }
    if (cmd == "restore") { std::thread([] { setupRestoreSteamVR(); }).detach(); return "true"; }
    if (cmd == "steamOpen" || cmd == "steamShow") { steamOpen(); return "true"; }
    if (cmd == "steamClose") { std::thread(steamClose).detach(); return "true"; }
    if (cmd == "mic") { micSetEnabled(a.boolean(false)); return "true"; }
    if (cmd == "report") { std::thread([] { reportMake(false); }).detach(); return "true"; }
    if (cmd == "logs" || cmd == "data") { ShellExecuteW(nullptr, L"open", dataDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); return "true"; }
    if (cmd == "openUrl") { openUrl(a.str()); return "true"; }
    if (cmd == "lang") {
        IDIOMA_ES = a.str() == "es";
        { std::lock_guard<std::mutex> g(g_setLock); g_set.lang = IDIOMA_ES ? "es" : "en"; }
        saveSettings();
        uiRefresh();
        return "true";
    }
    if (cmd == "cableInstall") { std::thread(cableInstallTools).detach(); return "true"; }
    if (cmd == "sentinelStart") { std::thread(cableStartSentinel).detach(); return "true"; }
    if (cmd == "updateHost") { std::thread(updatesInstallHost).detach(); return "true"; }
    if (cmd == "questApp") { std::thread(updatesInstallQuestApp).detach(); return "true"; }
    if (cmd == "checkUpdates") { std::thread([] { updatesCheck(true); }).detach(); return "true"; }
    if (cmd == "beta") { updatesSetBeta(a.boolean(false)); return "true"; }
    if (cmd == "vmonInstall") { std::thread(vmonInstall).detach(); return "true"; }
    if (cmd == "record") { recordSet(!recording()); return "true"; }
    if (cmd == "recFolder") { recordOpenFolder(); return "true"; }
    if (cmd == "recQuest") {
        { std::lock_guard<std::mutex> g(g_setLock); g_set.recQuest = a.boolean(true); }
        saveSettings();
        return "true";
    }
    if (cmd == "openAtLogin") { setOpenAtLogin(a.boolean(false)); return "true"; }
    if (cmd == "keepAwake") {
        { std::lock_guard<std::mutex> g(g_setLock); g_set.keepAwake = a.boolean(false); }
        saveSettings();
        return "true";
    }
    if (cmd == "forget") {
        DeleteFileW((dataDir() + L"\\paired.json").c_str());
        logmsg("[quest] Paired Quests forgotten: they'll ask for the code next time");
        return "true";
    }
    return "null";
}

static std::string loadPage() {
    HRSRC r = FindResourceW(nullptr, L"UI_HTML", RT_RCDATA);
    HGLOBAL g = r ? LoadResource(nullptr, r) : nullptr;
    const char *p = g ? (const char *)LockResource(g) : nullptr;
    return p ? std::string(p, SizeofResource(nullptr, r)) : std::string();
}

/// Abre la ventana y espera a que se cierre. false si no se pudo (sin WebView2): queda la ventana chica
bool uiRun(HINSTANCE inst) {
    std::thread(awakeThread).detach();
    std::string page = loadPage();
    if (page.empty()) { logmsg("[window] The page is missing from VRmin Host.exe"); return false; }
    try {
        webview::webview w(false, nullptr);
        HWND hw = (HWND)w.window().value();
        HICON icon = LoadIconW(inst, MAKEINTRESOURCEW(1));
        if (hw && icon) { SendMessageW(hw, WM_SETICON, ICON_BIG, (LPARAM)icon); SendMessageW(hw, WM_SETICON, ICON_SMALL, (LPARAM)icon); }
        w.set_title("VRmin Host");
        // (que entre en la pantalla, centrada: en pantallas chicas, más chica)
        RECT wa{0, 0, 1280, 800};
        SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
        UINT dpi = hw ? GetDpiForWindow(hw) : 96;
        int aw = MulDiv(wa.right - wa.left, 96, dpi ? dpi : 96), ah = MulDiv(wa.bottom - wa.top, 96, dpi ? dpi : 96);
        int ww = std::min(980, aw - 40), wh = std::min(760, ah - 60);
        w.set_size(std::min(720, ww), std::min(520, wh), WEBVIEW_HINT_MIN);
        w.set_size(ww, wh, WEBVIEW_HINT_NONE);
        if (hw) {
            RECT r;
            GetWindowRect(hw, &r);
            SetWindowPos(hw, nullptr, wa.left + (wa.right - wa.left - (r.right - r.left)) / 2, wa.top + (wa.bottom - wa.top - (r.bottom - r.top)) / 2, 0, 0,
                         SWP_NOSIZE | SWP_NOZORDER);
        }
        w.bind("vrmin", [](const std::string &req) -> std::string {
            Json a = Json::parse(req);   // ["acción", dato]
            if (a.t != Json::Arr || a.a.empty()) return "null";
            return handle(a.a[0].str(), a.a.size() > 1 ? a.a[1] : Json());
        });
        w.set_html(page);
        logmsg("[window] VRmin Host window open");
        w.run();
    } catch (const std::exception &e) {
        logmsg("[window] Couldn't open the window with WebView2 (%s): using the simple window", e.what());
        return false;
    }
    return true;
}
