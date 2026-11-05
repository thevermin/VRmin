// Mensajes del Quest y el estado que se le manda (lobby2d) varias veces por segundo
#include "host.h"
#include <thread>

static std::atomic<bool> mode2D{false}, uiInHeadset{false}, hiddenSent{false};
static std::string pcModel;

bool lobbyShowsDesk() { return mode2D || uiInHeadset; }
bool lobbyInGame() { return gameActive(); }

void questOnConnect() { mode2D = false; uiInHeadset = false; hiddenSent = false; deskRequestKey(); }
void questOnDisconnect() {}

static void applyQuestSettings(const Json &m) {
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
    if (t == "pose") { gameSendLine(line); return; }
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
        applyQuestSettings(m);
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
        logmsg("[report] requested from the headset");
    } else if (t == "desk") {
        if (m.has("on")) deskSetOn(m["on"].boolean());
        if (m["on"].boolean()) deskRequestKey();
    } else if (t == "mouse") {
        deskMouse((float)m["u"].num(0.5), (float)m["v"].num(0.5), m["down"], (float)m["scroll"].num(0));
    } else if (t == "wmouse" || t == "win" || t == "wsize" || t == "monitor" || t == "depth3d" || t == "deskdisp" || t == "dispswap" || t == "vigia" || t == "winkey" || t == "record") {
        // (ventanas sueltas, monitor virtual, 3D y Modo Centinela: todavía solo en la Mac)
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
        Json o = Json::object();
        o.set("type", "lobby2d").set("launching", gamesLaunchingInfo(active)).set("game", active).set("gameName", active ? gamesRunningName() : std::string())
         .set("macModel", pcModel).set("busy", gamesBusy()).set("status", gamesStatus()).set("desk", deskOn()).set("deskErr", deskError())
         .set("depth3d", false).set("depthStatus", "").set("cfg", cfg).set("games", games).set("monitor", false)
         .set("monitorErr", tr("Todavía no está en Windows", "Not available on Windows yet")).set("displays", Json::array())
         .set("mainDisplay", Json::object().set("id", 0x40000001).set("name", pcModel).set("w", GetSystemMetrics(SM_CXSCREEN)).set("h", GetSystemMetrics(SM_CYSCREEN)))
         .set("deskDisplay", 0x40000001).set("monitorId", 0).set("typing", typingActive()).set("vigia", false).set("ll", Json::array())
         .set("perf", active ? perfSnapshot() : Json::object()).set("adv", profilesState()).set("windows", Json::array()).set("os", "windows");
        std::string sj = o.dump();
        uint64_t now = nowNs();
        if (sj != last || now - lastAt > 2000000000ull) { last = sj; lastAt = now; questSendText(sj); }
    }
}

void lobbyStart() { std::thread(lobbyThread).detach(); }
