// Mensajes del Quest y el estado que se le manda (lobby2d) varias veces por segundo
#include "host.h"
#include <cstdio>
#include <thread>
#include <unistd.h>

static std::atomic<bool> mode2D{false}, uiInHeadset{false}, hiddenSent{false};
static std::string pcModel, g_gpu;
static int pcVramMb = 0;

/// La placa de video (el Quest recomienda la calidad con eso): el nombre, de lspci; la memoria, de amdgpu o de nvidia-smi
static void findGpu() {
    auto run = [](const char *cmd) {
        std::string out;
        if (FILE *p = popen(cmd, "r")) {
            char b[512];
            while (fgets(b, sizeof b, p)) out += b;
            pclose(p);
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
        return out;
    };
    std::string nv = run("nvidia-smi --query-gpu=name,memory.total --format=csv,noheader,nounits 2>/dev/null | head -n1");
    size_t c = nv.find(',');
    if (c != std::string::npos) {
        g_gpu = nv.substr(0, c);
        pcVramMb = atoi(nv.c_str() + c + 1);
    } else {
        // (la que tiene más memoria propia; si ninguna lo dice, la primera que muestre lspci)
        std::string l = run("lspci 2>/dev/null | grep -Ei 'vga|3d controller|display controller' | head -n1");
        size_t k = l.find(": ");
        if (k != std::string::npos) g_gpu = l.substr(k + 2);
        for (int i = 0; i < 8; i++) {
            std::string v = readFile("/sys/class/drm/card" + std::to_string(i) + "/device/mem_info_vram_total");
            int mb = v.empty() ? 0 : (int)(strtoull(v.c_str(), nullptr, 10) >> 20);
            if (mb > pcVramMb) pcVramMb = mb;
        }
    }
    logmsg("[pc] Graphics card: %s (%d MB)", g_gpu.empty() ? "?" : g_gpu.c_str(), pcVramMb);
}

std::string gamesStatus();
bool gamesBusy();
Json gamesLaunchingInfo(bool active);

bool lobbyShowsDesk() { return mode2D || uiInHeadset; }
void questOnConnect() { mode2D = false; uiInHeadset = false; hiddenSent = false; deskRequestKey(); }
void questOnDisconnect() {}

static void settingsApply(const Json &m) {
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
    if (t == "pose") gameSendLine(line);
    else if (t == "key") questRequestKey();
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
    } else if (t == "launch") {
        gamesLaunch(m["game"].str());
    } else if (t == "quitgame") {
        logmsg("[room] Closing the game from the headset");
        std::thread(gamesQuit).detach();
    }
    // Escritorio y ventanas (desktop.cpp, windows.cpp)
    else if (t == "deskkey") deskRequestKey();
    else if (t == "deskcfg") {
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
    } else if (t == "deskdisp") {
        deskSetDisplay((uint32_t)m["id"].num(0));   // «Cambiar de monitor» en la pantalla principal del Quest
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
    }
}

// Estado para el Quest: lista de juegos, si hay un juego abierto, ajustes…
static void lobbyThread() {
    std::string last;
    uint64_t lastAt = 0;
    {
        char b[256] = "";
        pcModel = gethostname(b, sizeof b - 1) == 0 && b[0] ? b : "PC";
    }
    findGpu();
    while (true) {
        sleepMs(200);
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
            if (l.size() != disps.size()) for (auto &d : l) logmsg("[pc] Monitor %u: %s, %dx%d%s", d.id & 0xFFFF, d.name.c_str(), d.r.right - d.r.left, d.r.bottom - d.r.top, d.primary ? " (main)" : "");
            disps = l;
        }
        Json mainD, others = Json::array();
        for (auto &d : disps) {
            Json j = Json::object().set("id", (double)d.id).set("name", d.primary ? pcModel : d.name).set("w", d.r.right - d.r.left).set("h", d.r.bottom - d.r.top);
            if (d.primary) mainD = j; else others.push(j);
        }
        Json o = Json::object();
        o.set("type", "lobby2d").set("launching", gamesLaunchingInfo(active)).set("game", active).set("gameName", active ? gamesRunningName() : std::string())
         .set("macModel", pcModel).set("busy", gamesBusy()).set("status", gamesStatus()).set("desk", deskOn()).set("deskErr", deskError()).set("cfg", cfg).set("games", games)
         .set("ll", Json::array()).set("perf", active ? perfSnapshot() : Json::object()).set("adv", profilesState())
         .set("os", "linux").set("pcGpu", g_gpu).set("pcVram", pcVramMb)
         .set("displays", others).set("deskDisplay", (double)deskDisplayId()).set("windows", windowsListJson()).set("typing", typingActive());
        if (mainD.t == Json::Obj) o.set("mainDisplay", mainD);   // (sin X11 no hay pantallas para abrir)
        std::string sj = o.dump();
        uint64_t now = nowNs();
        if (sj != last || now - lastAt > 2000000000ull) { last = sj; lastAt = now; questSendText(sj); }
    }
}

void lobbyStart() { std::thread(lobbyThread).detach(); }
