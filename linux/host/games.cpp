// Juegos de Steam (el de Linux, con Proton): lista, abrir y cerrar
#include "host.h"
#include <algorithm>
#include <csignal>
#include <ctime>
#include <dirent.h>
#include <map>
#include <set>
#include <spawn.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

extern char **environ;
bool vrSetupActivate();

static std::mutex gLock;
static std::vector<GameEntry> games;
static std::map<std::string, bool> vrCache;     // appid -> es VR (se guarda en vr_games.json)
static std::string status;
static bool busy = false;
static Json launching;                           // juego que se está abriendo (pantalla de carga del visor)
static uint64_t launchingAt = 0, lastScan = 0;
// (no son juegos: Steamworks, SteamVR, Proton, Steam Linux Runtime y otras herramientas)
static const std::set<std::string> NOT_GAMES = {"228980", "250820", "1070560", "1391110", "1628350", "1826330", "1493710"};

static bool isDir(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISDIR(st.st_mode); }
static bool isFile(const std::string &p) { struct stat st; return stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode); }

/// La carpeta de Steam: la de siempre, o la de Steam en Flatpak o Snap
static std::string steamPath() {
    const char *h = getenv("HOME");
    std::string home = h ? h : "";
    for (const char *rel : {"/.local/share/Steam", "/.steam/steam", "/.var/app/com.valvesoftware.Steam/.local/share/Steam",
                            "/snap/steam/common/.local/share/Steam"}) {
        std::string p = home + rel;
        if (isFile(p + "/steamapps/libraryfolders.vdf")) {
            char real[4096];
            return realpath(p.c_str(), real) ? std::string(real) : p;
        }
    }
    return "";
}

/// Los valores "clave" "valor" de un archivo de Steam (.acf / .vdf), en orden
static std::vector<std::pair<std::string, std::string>> vdfPairs(const std::string &t) {
    std::vector<std::pair<std::string, std::string>> out;
    std::vector<std::string> toks;
    for (size_t i = 0; i < t.size(); i++) {
        if (t[i] != '"') {
            if (t[i] == '{' || t[i] == '}') toks.push_back(std::string(1, t[i]));
            continue;
        }
        std::string s;
        for (i++; i < t.size() && t[i] != '"'; i++) {
            if (t[i] == '\\' && i + 1 < t.size()) { i++; s += t[i]; } else s += t[i];
        }
        toks.push_back("\x01" + s);
    }
    for (size_t i = 0; i + 1 < toks.size(); i++)
        if (toks[i][0] == '\x01' && toks[i + 1][0] == '\x01') { out.push_back({toks[i].substr(1), toks[i + 1].substr(1)}); i++; }
    return out;
}

static std::vector<std::string> libraries() {
    std::string sp = steamPath();
    if (sp.empty()) return {};
    std::vector<std::string> libs = {sp + "/steamapps"};
    for (auto &kv : vdfPairs(readFile(sp + "/steamapps/libraryfolders.vdf")))
        if (kv.first == "path") {
            char real[4096];
            std::string p = (realpath(kv.second.c_str(), real) ? std::string(real) : kv.second) + "/steamapps";
            if (std::find(libs.begin(), libs.end(), p) == libs.end() && isDir(p)) libs.push_back(p);
        }
    return libs;
}

/// ¿Es un juego de VR? Si en su carpeta está la biblioteca de OpenVR o de OpenXR (o el plugin de Oculus), sí
static bool looksVR(const std::string &dir, int depth = 0) {
    DIR *d = opendir(dir.c_str());
    if (!d) return false;
    bool vr = false;
    std::vector<std::string> sub;
    while (dirent *e = readdir(d)) {
        std::string n = e->d_name;
        if (n == "." || n == "..") continue;
        std::string full = dir + "/" + n;
        if (isDir(full)) { if (depth < 4) sub.push_back(full); continue; }
        if (!strcasecmp(n.c_str(), "openvr_api.dll") || !strcasecmp(n.c_str(), "openxr_loader.dll") || !strcasecmp(n.c_str(), "OVRPlugin.dll")) { vr = true; break; }
    }
    closedir(d);
    for (size_t i = 0; !vr && i < sub.size() && i < 200; i++) vr = looksVR(sub[i], depth + 1);
    return vr;
}

static void gamesScan() {
    {
        std::lock_guard<std::mutex> g(gLock);
        if (vrCache.empty()) {
            Json c = Json::parse(readFile(dataDir() + "/vr_games.json"));
            for (auto &kv : c.o) vrCache[kv.first] = kv.second.boolean(true);
        }
    }
    std::vector<GameEntry> out;
    bool cacheChanged = false;
    for (auto &lib : libraries()) {
        DIR *d = opendir(lib.c_str());
        if (!d) continue;
        while (dirent *f = readdir(d)) {
            std::string fn = f->d_name;
            if (fn.rfind("appmanifest_", 0) != 0 || fn.size() < 4 || fn.compare(fn.size() - 4, 4, ".acf") != 0) continue;
            std::map<std::string, std::string> m;
            for (auto &kv : vdfPairs(readFile(lib + "/" + fn))) if (!m.count(kv.first)) m[kv.first] = kv.second;
            std::string id = m["appid"], dir = m["installdir"];
            if (id.empty() || dir.empty() || NOT_GAMES.count(id)) continue;
            std::string full = lib + "/common/" + dir;
            if (!isDir(full)) continue;
            // (Proton y el Steam Linux Runtime también aparecen como juegos)
            if (isFile(full + "/proton") || isFile(full + "/toolmanifest.vdf")) continue;
            GameEntry e;
            e.id = id; e.dir = dir; e.name = m.count("name") ? m["name"] : dir;
            bool known;
            { std::lock_guard<std::mutex> g(gLock); known = vrCache.count(id) > 0; if (known) e.vr = vrCache[id]; }
            if (!known) {
                e.vr = looksVR(full);
                std::lock_guard<std::mutex> g(gLock);
                vrCache[id] = e.vr;
                cacheChanged = true;
            }
            out.push_back(e);
        }
        closedir(d);
    }
    std::sort(out.begin(), out.end(), [](const GameEntry &a, const GameEntry &b) { return strcasecmp(a.name.c_str(), b.name.c_str()) < 0; });
    std::lock_guard<std::mutex> g(gLock);
    games = out;
    if (cacheChanged) {
        Json c = Json::object();
        for (auto &kv : vrCache) c.set(kv.first, kv.second);
        writeFile(dataDir() + "/vr_games.json", c.dump());
    }
}

std::vector<GameEntry> gamesList() {
    uint64_t now = nowNs();
    bool rescan;
    {
        std::lock_guard<std::mutex> g(gLock);
        rescan = now - lastScan > (gameActive() ? 30000000000ull : 5000000000ull) || (games.empty() && now - lastScan > 1000000000ull);
        if (rescan) lastScan = now;
    }
    if (rescan) std::thread(gamesScan).detach();
    std::lock_guard<std::mutex> g(gLock);
    return games;
}

static void setStatus(const std::string &s) { std::lock_guard<std::mutex> g(gLock); status = s; }
std::string gamesStatus() { std::lock_guard<std::mutex> g(gLock); return status; }
bool gamesBusy() { std::lock_guard<std::mutex> g(gLock); return busy; }

Json gamesLaunchingInfo(bool active) {
    std::lock_guard<std::mutex> g(gLock);
    if (active || (launching.t == Json::Obj && nowNs() - launchingAt > 240000000000ull)) launching = Json();
    if (active) return Json();
    return launching;
}

/// Procesos de juegos de Steam abiertos: los que corren desde steamapps/common/... (con Proton, la ruta de Windows
/// del juego, Z:\...\steamapps\common\...)
static void forEachGameProcess(const std::function<void(pid_t pid, const std::string &folder)> &fn) {
    DIR *d = opendir("/proc");
    if (!d) return;
    pid_t self = getpid();
    while (dirent *e = readdir(d)) {
        pid_t pid = (pid_t)atoi(e->d_name);
        if (pid <= 0 || pid == self) continue;
        std::string cmd = readFile("/proc/" + std::string(e->d_name) + "/cmdline");
        size_t z = cmd.find('\0');
        std::string exe = z == std::string::npos ? cmd : cmd.substr(0, z), low = exe;   // (solo el programa, no sus argumentos)
        for (auto &c : low) c = c == '\\' ? '/' : (char)tolower((unsigned char)c);
        size_t k = low.find("/steamapps/common/");
        if (k == std::string::npos) continue;
        std::string rest = exe.substr(k + 18);
        size_t end = rest.find_first_of("/\\");
        std::string folder = end == std::string::npos ? rest : rest.substr(0, end);
        // (Proton y el Steam Linux Runtime, que abren el juego, también están en steamapps/common)
        if (folder.rfind("Proton", 0) == 0 || folder.rfind("SteamLinuxRuntime", 0) == 0 || folder == "Steamworks Shared") continue;
        fn(pid, folder);
    }
    closedir(d);
}

std::string gamesRunningName() {
    std::string name;
    forEachGameProcess([&](pid_t, const std::string &f) { if (name.empty()) name = f; });
    return name;
}

void gamesQuit() {
    forEachGameProcess([](pid_t pid, const std::string &f) {
        if (kill(pid, SIGTERM) == 0) logmsg("[room] Closed %s (process %d)", f.c_str(), (int)pid);
    });
}

/// Abre una dirección steam:// con el Steam de la PC (y Steam mismo, si estaba cerrado)
static void openSteamUrl(const std::string &url) {
    for (const char *prog : {"steam", "xdg-open"}) {
        pid_t pid;
        const char *argv[] = {prog, url.c_str(), nullptr};
        if (posix_spawnp(&pid, prog, nullptr, nullptr, (char *const *)argv, environ) == 0) {
            std::thread([pid] { int st; waitpid(pid, &st, 0); }).detach();   // (Steam sigue abierto: solo se lo espera para que no quede colgado)
            return;
        }
    }
    logmsg("[steam] Couldn't open Steam (is it installed?)");
}

void gamesLaunch(const std::string &id) {
    GameEntry ge;
    bool found = false;
    {
        std::lock_guard<std::mutex> g(gLock);
        if (busy) return;
        for (auto &x : games) if (x.id == id) { ge = x; found = true; }
        if (!found) return;
        busy = true;
        status = std::string(tr("Abriendo ", "Opening ")) + ge.name + "…";
        launching = Json::object().set("id", ge.id).set("name", ge.name).set("t", (long long)time(nullptr));
        launchingAt = nowNs();
    }
    logmsg("[room] Opening %s from the headset%s", ge.name.c_str(), ge.vr ? "" : " (flat game)");
    std::thread([ge] {
        if (ge.vr) vrSetupActivate();
        openSteamUrl("steam://rungameid/" + ge.id);
        setStatus(ge.vr ? std::string(tr("Abriendo ", "Opening ")) + ge.name + tr("… (si Steam pregunta, elegí VR en la PC)", "… (if Steam asks, choose VR on the PC)")
                        : std::string(tr("Abriendo ", "Opening ")) + ge.name + tr("… aparece en el escritorio", "… it shows up on the desktop"));
        for (int i = 0; i < 40 && !gameActive(); i++) sleepMs(1000);
        std::lock_guard<std::mutex> g(gLock);
        busy = false;
        if (gameActive()) launching = Json();
        if (status.rfind(tr("Abriendo", "Opening"), 0) == 0) status.clear();
    }).detach();
}
