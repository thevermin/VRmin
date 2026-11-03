// Juegos de Steam instalados en la PC: lista, abrir y cerrar
#include "host.h"
#include <shellapi.h>
#include <tlhelp32.h>
#include <algorithm>
#include <ctime>
#include <map>
#include <set>
#include <thread>

static std::mutex gLock;
static std::vector<GameEntry> games;
static std::map<std::string, bool> vrCache;     // appid -> es VR (se guarda en juegos_vr.json)
static std::string status;
static bool busy = false;
static Json launching;                           // juego que se está abriendo (pantalla de carga del visor)
static uint64_t launchingAt = 0, lastScan = 0;
// (no son juegos: Steamworks, SteamVR, herramientas)
static const std::set<std::string> NOT_GAMES = {"228980", "250820", "1070560", "1391110", "1628350", "1826330", "1493710"};

static std::wstring steamPath() {
    wchar_t buf[MAX_PATH];
    DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) {
        std::wstring p = buf;
        for (auto &c : p) if (c == L'/') c = L'\\';
        return p;
    }
    return L"C:\\Program Files (x86)\\Steam";
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
            if (t[i] == '\\' && i + 1 < t.size()) { i++; s += t[i] == '\\' ? '\\' : t[i]; } else s += t[i];
        }
        toks.push_back("\x01" + s);
    }
    for (size_t i = 0; i + 1 < toks.size(); i++)
        if (toks[i][0] == '\x01' && toks[i + 1][0] == '\x01') { out.push_back({toks[i].substr(1), toks[i + 1].substr(1)}); i++; }
    return out;
}

static std::vector<std::wstring> libraries() {
    std::vector<std::wstring> libs = {steamPath() + L"\\steamapps"};
    for (auto &kv : vdfPairs(readFile(steamPath() + L"\\steamapps\\libraryfolders.vdf")))
        if (kv.first == "path") {
            std::wstring p = wide(kv.second) + L"\\steamapps";
            bool dup = false;
            for (auto &l : libs) if (_wcsicmp(l.c_str(), p.c_str()) == 0) dup = true;
            if (!dup) libs.push_back(p);
        }
    return libs;
}

/// ¿Es un juego de VR? Si en su carpeta está la biblioteca de OpenVR o de OpenXR (o el plugin de Oculus), sí
static bool looksVR(const std::wstring &dir, int depth = 0) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool vr = false;
    std::vector<std::wstring> sub;
    do {
        std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth < 4) sub.push_back(dir + L"\\" + n); continue; }
        if (_wcsicmp(n.c_str(), L"openvr_api.dll") == 0 || _wcsicmp(n.c_str(), L"openxr_loader.dll") == 0 || _wcsicmp(n.c_str(), L"OVRPlugin.dll") == 0 ||
            _wcsicmp(n.c_str(), L"OVRPlugin.dll") == 0) { vr = true; break; }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (size_t i = 0; !vr && i < sub.size() && i < 200; i++) vr = looksVR(sub[i], depth + 1);
    return vr;
}

void gamesScan() {
    {
        std::lock_guard<std::mutex> g(gLock);
        if (vrCache.empty()) {
            Json c = Json::parse(readFile(dataDir() + L"\\juegos_vr.json"));
            for (auto &kv : c.o) vrCache[kv.first] = kv.second.boolean(true);
        }
    }
    std::vector<GameEntry> out;
    bool cacheChanged = false;
    for (auto &lib : libraries()) {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((lib + L"\\appmanifest_*.acf").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            std::map<std::string, std::string> d;
            for (auto &kv : vdfPairs(readFile(lib + L"\\" + fd.cFileName))) if (!d.count(kv.first)) d[kv.first] = kv.second;
            std::string id = d["appid"], dir = d["installdir"];
            if (id.empty() || dir.empty() || NOT_GAMES.count(id)) continue;
            std::wstring full = lib + L"\\common\\" + wide(dir);
            if (GetFileAttributesW(full.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
            GameEntry e;
            e.id = id; e.dir = dir; e.name = d.count("name") ? d["name"] : dir;
            bool known;
            { std::lock_guard<std::mutex> g(gLock); known = vrCache.count(id) > 0; if (known) e.vr = vrCache[id]; }
            if (!known) {
                e.vr = looksVR(full);
                std::lock_guard<std::mutex> g(gLock);
                vrCache[id] = e.vr;
                cacheChanged = true;
            }
            out.push_back(e);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    std::sort(out.begin(), out.end(), [](const GameEntry &a, const GameEntry &b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    std::lock_guard<std::mutex> g(gLock);
    games = out;
    if (cacheChanged) {
        Json c = Json::object();
        for (auto &kv : vrCache) c.set(kv.first, kv.second);
        writeFile(dataDir() + L"\\juegos_vr.json", c.dump());
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

/// Procesos de juegos de Steam abiertos (los que corren desde steamapps\common\...)
static void forEachGameProcess(const std::function<void(DWORD pid, const std::string &folder)> &fn) {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{};
    pe.dwSize = sizeof pe;
    DWORD self = GetCurrentProcessId();
    for (BOOL ok = Process32FirstW(snap, &pe); ok; ok = Process32NextW(snap, &pe)) {
        if (pe.th32ProcessID == self) continue;
        HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pe.th32ProcessID);
        if (!p) continue;
        wchar_t path[MAX_PATH * 2];
        DWORD n = MAX_PATH * 2;
        if (QueryFullProcessImageNameW(p, 0, path, &n)) {
            std::wstring s(path, n), low = s;
            for (auto &c : low) c = towlower(c);
            size_t k = low.find(L"\\steamapps\\common\\");
            if (k != std::wstring::npos) {
                std::wstring rest = s.substr(k + 18);
                size_t e = rest.find(L'\\');
                fn(pe.th32ProcessID, utf8(e == std::wstring::npos ? rest : rest.substr(0, e)));
            }
        }
        CloseHandle(p);
    }
    CloseHandle(snap);
}

std::string gamesRunningName() {
    std::string name;
    forEachGameProcess([&](DWORD, const std::string &f) { if (name.empty()) name = f; });
    return name;
}

void gamesQuit() {
    forEachGameProcess([](DWORD pid, const std::string &f) {
        HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pid);
        if (p) { TerminateProcess(p, 0); CloseHandle(p); logmsg("[sala] Cerré %s (proceso %lu)", f.c_str(), pid); }
    });
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
    logmsg("[sala] Abriendo %s desde el visor%s", ge.name.c_str(), ge.vr ? "" : " (juego plano)");
    std::thread([ge] {
        if (ge.vr) {
            SetupState s = setupCheck();
            if (!s.runtimeOk || !s.ocOk) {
                setStatus(tr("Preparando VRmin en Windows (aceptá el permiso en la PC)…", "Setting up VRmin on Windows (accept the prompt on the PC)…"));
                setupActivate();
            }
        }
        // Steam abre el juego (y Steam mismo, si estaba cerrado)
        std::wstring url = L"steam://rungameid/" + wide(ge.id);
        ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        setStatus(ge.vr ? std::string(tr("Abriendo ", "Opening ")) + ge.name + tr("… (si Steam pregunta, elegí VR en la PC)", "… (if Steam asks, choose VR on the PC)")
                        : std::string(tr("Abriendo ", "Opening ")) + ge.name + tr("… aparece en el escritorio", "… it shows up on the desktop"));
        for (int i = 0; i < 40 && !gameActive(); i++) Sleep(1000);
        std::lock_guard<std::mutex> g(gLock);
        busy = false;
        if (gameActive()) launching = Json();
        if (status.rfind(tr("Abriendo", "Opening"), 0) == 0 || status.rfind(tr("Preparando", "Setting"), 0) == 0) status.clear();
    }).detach();
}
