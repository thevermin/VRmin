// Mensajes del Quest y el estado que se le manda (lobby2d) varias veces por segundo
#include "host.h"
#include <dxgi.h>
#include <thread>

static std::atomic<bool> mode2D{false}, uiInHeadset{false}, hiddenSent{false};
static std::string pcModel, g_gpu;
static int pcVramMb = 0;

/// La placa de video de la PC (la de más memoria propia): el Quest recomienda la calidad con eso
static void findGpu() {
    IDXGIFactory1 *f = nullptr;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void **)&f))) return;
    IDXGIAdapter1 *a = nullptr;
    SIZE_T best = 0;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d;
        if (SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && (g_gpu.empty() || d.DedicatedVideoMemory > best)) {
            best = d.DedicatedVideoMemory;
            g_gpu = utf8(d.Description);
            pcVramMb = (int)(d.DedicatedVideoMemory >> 20);
        }
        a->Release();
    }
    f->Release();
    logmsg("[pc] Graphics card: %s (%d MB)", g_gpu.c_str(), pcVramMb);
}

bool lobbyShowsDesk() { return mode2D || uiInHeadset; }
bool reportMake(bool fromHeadset);
std::string pcGpu() { return g_gpu; }
int pcVram() { return pcVramMb; }
std::string pcGpuName() { return g_gpu.empty() ? "?" : g_gpu + " (" + std::to_string(pcVramMb) + " MB)"; }
bool lobbyInGame() { return gameActive(); }

void questOnConnect() { mode2D = false; uiInHeadset = false; hiddenSent = false; deskRequestKey(); }
void questOnDisconnect() {}

void settingsApply(const Json &m) {
    {
        std::lock_guard<std::mutex> g(g_setLock);
        Settings &s = g_set;
        if (m.has("eye")) s.eye = std::min(2560, std::max(512, (int)m["eye"].num()));
        if (m.has("fps")) { int f = (int)m["fps"].num(); if (f == 72 || f == 80 || f == 90 || f == 120) s.fps = f; }
        if (m.has("mbps")) s.mbps = std::min(300, std::max(10, (int)m["mbps"].num()));
        if (m.has("codec")) s.hevc = m["codec"].str() != "h264";
        if (m.has("fov") && m["fov"].str().size() < 80) s.fov = m["fov"].str();
        if (m.has("audio")) s.audio = m["audio"].str();
        else if (m.has("audioMac")) s.audio = m["audioMac"].boolean() ? "both" : "quest";
    }
    saveSettings();
    applySettings();
}

void handleQuestMessage(const std::string &line) {
    // Las poses van directo al juego (es lo más frecuente: no hace falta leer todo el mensaje)
    if (line.compare(0, 14, "{\"type\":\"pose\"") == 0) {
        if (mode2D) mode2D = false;
        gameSendLine(line);
        return;
    }
    Json m = Json::parse(line);
    std::string t = m["type"].str();
    if (t == "pose") { sentinelPose(); gameSendLine(line); return; }
    if (t == "key") questRequestKey();
    else if (t == "deskkey") deskRequestKey();
    else if (t == "log") logmsg("[headset] %s", m["msg"].str().c_str());
    else if (t == "framegen") { g_halfRate = m["on"].boolean(false); gameConfigChanged(); }
    else if (t == "hello") {
        std::string mode = m["mode"].str();
        mode2D = mode == "2d"; uiInHeadset = mode == "vr2";
        if (mode2D || uiInHeadset) deskSetOn(false);   // (el escritorio arranca oculto)
        logmsg("[quest] %s", mode2D ? "Quest window (2D): game list and desktop" : uiInHeadset ? "VR room with menu and windows" : "VR mode");
    } else if (t == "rejoin") {
        gameSendLine("{\"resetimg\":1}");
        questRequestKey();
        logmsg("[quest] Back to the game: rebuilding the game picture");
    } else if (t == "gamecfg") {
        profilesApply(m);
    } else if (t == "settings") {
        settingsApply(m);
    } else if (t == "deskcfg") {
        {
            std::lock_guard<std::mutex> g(g_setLock);
            Settings &s = g_set;
            if (m.has("maxw")) s.deskMaxW = std::min(3840, std::max(960, (int)m["maxw"].num()));
            if (m.has("fps")) s.deskFps = std::min(60, std::max(15, (int)m["fps"].num()));
            if (m.has("mbps")) s.deskMbps = std::min(80, std::max(4, (int)m["mbps"].num()));
            if (m.has("audio")) s.deskAudio = m["audio"].str();
        }
        saveSettings();
        deskRequestKey();
    } else if (t == "kb") {
        typeKeys(m);
    } else if (t == "launch") {
        gamesLaunch(m["game"].str());
    } else if (t == "quitgame") {
        logmsg("[room] Closing the game from the headset");
        std::thread(gamesQuit).detach();
    } else if (t == "report") {
        std::thread([] { reportMake(true); }).detach();
    } else if (t == "deskdisp") {
        // «Cambiar de monitor» en la pantalla principal del Quest
        deskSetDisplay((uint32_t)m["id"].num(0));
    } else if (t == "desk") {
        if (m.has("on")) deskSetOn(m["on"].boolean());
        if (m["on"].boolean()) deskRequestKey();
    } else if (t == "mouse") {
        deskMouse((float)m["u"].num(0.5), (float)m["v"].num(0.5), m["down"], (float)m["scroll"].num(0));
    } else if (t == "win") {
        winOpen((uint32_t)m["id"].num(0), m["on"].boolean(false));
    } else if (t == "winkey") {
        winOpen((uint32_t)m["id"].num(0), true);   // (ya abierta: manda una imagen completa)
    } else if (t == "wmouse") {
        winMouse((uint32_t)m["id"].num(0), (float)m["u"].num(0.5), (float)m["v"].num(0.5), m["down"], (float)m["scroll"].num(0));
    } else if (t == "wsize") {
        winResize((uint32_t)m["id"].num(0), (int)m["w"].num(0), (int)m["h"].num(0));
    } else if (t == "dispswap") {
        winSwap((uint32_t)m["id"].num(0), (uint32_t)m["to"].num(0));
    } else if (t == "vigia") {
        if (m.has("on")) { sentinelSet(m["on"].boolean(false)); if (m["on"].boolean(false)) questRequestKey(); }
        if (m.has("msg")) sentinelSay(m["msg"].str());
    } else if (t == "record") {   // ajustes rápidos del visor: empezar o terminar la grabación
        recordSet(m["on"].boolean(false));
    } else if (t == "depth3d") {
        bool on = m["on"].boolean(false);
        if (on) depthPrepare();
        uint32_t sid = (uint32_t)m["id"].num(0);
        if (sid) { depthSetWindow(sid, on); logmsg("[3D] Window %u in 3D: %s", sid, on ? "on" : "off"); }
        else { depthSetDesktop(on); logmsg("[3D] Desktop in 3D: %s", on ? "on" : "off"); }
    } else if (t == "monitor") {
        // (monitor virtual: todavía solo en la Mac)
    }
}

// Estado para el Quest: lista de juegos, si hay un juego abierto, ajustes…
static void lobbyThread() {
    std::string last;
    uint64_t lastAt = 0;
    {
        wchar_t b[256];
        DWORD n = 256;
        pcModel = GetComputerNameW(b, &n) ? utf8(std::wstring(b, n)) : "PC";
    }
    findGpu();
    while (true) {
        Sleep(200);
        if (!questConnected()) { last.clear(); continue; }
        bool active = gameActive();
        if (active) {
            if (!hiddenSent) { hiddenSent = true; questSendText("{\"type\":\"panels\",\"lobby\":false}"); }
        } else if (hiddenSent) { hiddenSent = false; deskRequestKey(); }
        Settings s;
        { std::lock_guard<std::mutex> g(g_setLock); s = g_set; }
        Stream st = currentStream();
        Json games = Json::array();
        for (auto &gm : gamesList()) games.push(Json::object().set("id", gm.id).set("name", gm.name).set("vr", gm.vr).set("dir", gm.dir));
        char run[96];
        snprintf(run, sizeof run, "%d · %d Hz · %d Mbps", st.eye, st.fps, st.bitrate / 1000000);
        Json cfg = Json::object();
        cfg.set("eye", s.eye).set("fps", s.fps).set("mbps", s.mbps).set("audioMac", s.audio == "both").set("fov", s.fov)
           .set("codec", s.hevc ? "hevc" : "h264").set("run", run).set("audio", s.audio).set("deskMaxW", s.deskMaxW)
           .set("deskFps", s.deskFps).set("deskMbps", s.deskMbps).set("deskAudio", s.deskAudio);
        // Los monitores de la PC (se revisan cada 2 s): el principal y los demás
        static uint64_t dispAt = 0;
        static std::vector<DisplayInfo> disps;
        if (nowNs() - dispAt > 2000000000ull) {
            dispAt = nowNs();
            std::vector<DisplayInfo> l = displaysList();
            if (l.size() != disps.size()) for (auto &d : l) logmsg("[pc] Monitor %u: %s, %ldx%ld%s", d.id & 0xFFFF, d.name.c_str(), d.r.right - d.r.left, d.r.bottom - d.r.top, d.primary ? " (main)" : "");
            disps = l;
        }
        Json mainD = Json::object().set("id", 0x40000001).set("name", pcModel).set("w", GetSystemMetrics(SM_CXSCREEN)).set("h", GetSystemMetrics(SM_CYSCREEN));
        Json others = Json::array();
        for (auto &d : disps) {
            Json j = Json::object().set("id", (double)d.id).set("name", d.primary ? pcModel : d.name).set("w", (int)(d.r.right - d.r.left)).set("h", (int)(d.r.bottom - d.r.top));
            if (d.primary) mainD = j; else others.push(j);
        }
        Json o = Json::object();
        o.set("type", "lobby2d").set("launching", gamesLaunchingInfo(active)).set("game", active).set("gameName", active ? gamesRunningName() : std::string())
         .set("macModel", pcModel).set("busy", gamesBusy()).set("status", gamesStatus()).set("desk", deskOn()).set("deskErr", deskError())
         .set("depth3d", depthEnabled()).set("depthStatus", depthStatus()).set("cfg", cfg).set("games", games).set("monitor", false)
         .set("monitorErr", tr("Todavía no está en Windows", "Not available on Windows yet")).set("displays", others)
         .set("mainDisplay", mainD).set("deskDisplay", (double)deskDisplayId()).set("monitorId", 0).set("typing", typingActive()).set("vigia", sentinelOn()).set("ll", Json::array())
         .set("perf", active ? perfSnapshot() : Json::object()).set("adv", profilesState()).set("windows", windowsListJson()).set("os", "windows")
         .set("pcGpu", g_gpu).set("pcVram", pcVramMb);
        std::string sj = o.dump();
        uint64_t now = nowNs();
        if (sj != last || now - lastAt > 2000000000ull) { last = sj; lastAt = now; questSendText(sj); }
    }
}

void lobbyStart() { std::thread(lobbyThread).detach(); }
