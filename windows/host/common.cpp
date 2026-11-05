// Registro, archivos, ajustes, foveated encoding, rendimiento y perfiles de juegos
#include "host.h"
#include <shlobj.h>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <deque>

bool IDIOMA_ES = false;
Settings g_set;
std::mutex g_setLock;
Stream g_run;
std::mutex g_runLock;

// ---------------- Utilidades ----------------
std::string utf8(const std::wstring &w) {
    if (w.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), &s[0], n, nullptr, nullptr);
    return s;
}
std::wstring wide(const std::string &s) {
    if (s.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), &w[0], n);
    return w;
}
std::wstring dataDir() {
    static std::wstring d;
    if (d.empty()) {
        wchar_t *p = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) { d = std::wstring(p) + L"\\VRmin"; CoTaskMemFree(p); }
        else d = L"C:\\VRmin";
        CreateDirectoryW(d.c_str(), nullptr);
    }
    return d;
}
std::wstring exeDir() {
    wchar_t b[MAX_PATH];
    DWORD n = GetModuleFileNameW(nullptr, b, MAX_PATH);
    std::wstring p(b, n);
    size_t k = p.find_last_of(L"\\/");
    return k == std::wstring::npos ? L"." : p.substr(0, k);
}
std::string readFile(const std::wstring &path) {
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return "";
    std::string s;
    char buf[65536];
    DWORD n = 0;
    while (ReadFile(f, buf, sizeof buf, &n, nullptr) && n > 0) s.append(buf, n);
    CloseHandle(f);
    return s;
}
bool writeFile(const std::wstring &path, const std::string &data) {
    std::wstring tmp = path + L".tmp";
    HANDLE f = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    DWORD w = 0;
    bool ok = WriteFile(f, data.data(), (DWORD)data.size(), &w, nullptr) && w == data.size();
    CloseHandle(f);
    return ok && MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
}
uint64_t nowNs() {
    static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }();
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    return (uint64_t)((double)c.QuadPart * 1e9 / (double)f.QuadPart);
}

static std::mutex g_logLock;
static FILE *g_logFile;
void logmsg(const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    std::lock_guard<std::mutex> g(g_logLock);
    if (!g_logFile) {
        std::wstring p = dataDir() + L"\\VRmin.log", old = dataDir() + L"\\VRmin-previous.log";
        MoveFileExW(p.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);   // (el de la vez anterior queda aparte)
        g_logFile = _wfopen(p.c_str(), L"w");
    }
    SYSTEMTIME t;
    GetLocalTime(&t);
    if (g_logFile) { fprintf(g_logFile, "%02d:%02d:%02d %s\n", t.wHour, t.wMinute, t.wSecond, buf); fflush(g_logFile); }
    OutputDebugStringA(buf);
}

// ---------------- Ajustes ----------------
void loadSettings() {
    Json j = Json::parse(readFile(dataDir() + L"\\settings.json"));
    std::lock_guard<std::mutex> g(g_setLock);
    Settings &s = g_set;
    if (j.has("eye")) s.eye = std::min(2560, std::max(512, (int)j["eye"].num()));
    if (j.has("fps")) { int f = (int)j["fps"].num(); if (f == 72 || f == 80 || f == 90 || f == 120) s.fps = f; }
    if (j.has("mbps")) s.mbps = std::min(300, std::max(10, (int)j["mbps"].num()));
    if (j.has("codec")) s.hevc = j["codec"].str() != "h264";
    if (j.has("fov")) s.fov = j["fov"].str("0");
    if (j.has("audio")) s.audio = j["audio"].str("quest");
    if (j.has("deskMaxW")) s.deskMaxW = std::min(3840, std::max(960, (int)j["deskMaxW"].num()));
    if (j.has("deskFps")) s.deskFps = std::min(60, std::max(15, (int)j["deskFps"].num()));
    if (j.has("deskMbps")) s.deskMbps = std::min(80, std::max(4, (int)j["deskMbps"].num()));
    if (j.has("deskAudio")) s.deskAudio = j["deskAudio"].str("mac");
}
void saveSettings() {
    Json j = Json::object();
    {
        std::lock_guard<std::mutex> g(g_setLock);
        const Settings &s = g_set;
        j.set("eye", s.eye).set("fps", s.fps).set("mbps", s.mbps).set("codec", s.hevc ? "hevc" : "h264").set("fov", s.fov)
         .set("audio", s.audio).set("deskMaxW", s.deskMaxW).set("deskFps", s.deskFps).set("deskMbps", s.deskMbps).set("deskAudio", s.deskAudio);
    }
    writeFile(dataDir() + L"\\settings.json", j.dump());
}

// ---------------- Foveated encoding ----------------
Fovea::Fovea(const std::string &s) {
    std::vector<double> p;
    size_t i = 0;
    while (i <= s.size()) {
        size_t e = s.find(',', i);
        if (e == std::string::npos) e = s.size();
        p.push_back(atof(s.substr(i, e - i).c_str()));
        i = e + 1;
    }
    if (p.size() >= 1) level = std::max(0, std::min(3, (int)p[0]));
    if (p.size() >= 2) size = std::max(0, std::min(2, (int)p[1]));
    if (p.size() >= 5) {
        cx[0] = std::min(0.8, std::max(0.2, p[2])); cx[1] = std::min(0.8, std::max(0.2, p[3]));
        cy = std::min(0.8, std::max(0.2, p[4]));
    }
}
std::string Fovea::text() const {
    char b[96];
    snprintf(b, sizeof b, "%d,%d,%.4f,%.4f,%.4f", level, size, cx[0], cx[1], cy);
    return b;
}
void Fovea::params(double &m, double &r0, double &r1) const {
    static const double ms[4] = {1, 0.62, 0.48, 0.36};
    static const double a[3] = {0.10, 0.16, 0.24}, b[3] = {0.30, 0.38, 0.46};
    m = ms[level]; r0 = a[size]; r1 = b[size];
}
std::vector<double> Fovea::cumulative(double c) const {
    double m, r0, r1;
    params(m, r0, r1);
    const int n = 2048;
    std::vector<double> out(n + 1, 0.0);
    double acc = 0;
    for (int i = 0; i < n; i++) {
        double t = (i + 0.5) / n, d = std::fabs(t - c);
        double x = std::max(0.0, std::min(1.0, (d - r0) / (r1 - r0))), sm = x * x * (3 - 2 * x);
        acc += (1 - (1 - m) * sm) / n;
        out[i + 1] = acc;
    }
    return out;
}
double Fovea::ratio(double c) const { auto e = cumulative(c); return e.back(); }
std::vector<float> Fovea::forward(double c) const {
    std::vector<double> E = cumulative(c);
    double tot = E.back();
    int n = (int)E.size() - 1, j = 0;
    std::vector<float> out(1024);
    for (int i = 0; i < 1024; i++) {
        double e = (double)i / 1023 * tot;
        while (j < n - 1 && E[j + 1] < e) j++;
        double a = E[j], b = E[j + 1], k = b > a ? (e - a) / (b - a) : 0;
        out[i] = (float)((j + k) / n);
    }
    return out;
}

static int round64(double v) { return std::max(256, (int)std::lround(v / 64) * 64); }
Stream currentStream() { std::lock_guard<std::mutex> g(g_runLock); return g_run; }
static void computeStream() {
    Settings s;
    { std::lock_guard<std::mutex> g(g_setLock); s = g_set; }
    Stream st;
    st.eye = s.eye; st.fps = s.fps; st.bitrate = s.mbps * 1000000; st.hevc = s.hevc;
    st.fov = Fovea(s.fov);
    // (H.264 no pasa de 4096 de ancho: más grande, solo con HEVC o con foveated encoding)
    st.W = st.fov.on() ? round64(st.eye * st.fov.ratio(0.5)) * 2 : st.eye * 2;
    st.H = st.fov.on() ? round64(st.eye * st.fov.ratio(st.fov.cy)) : st.eye;
    if (st.hevc && !encoderCanHevc(st.W, st.H, st.fps, st.bitrate)) st.hevc = false;
    if (!st.hevc && st.W > 4096) logmsg("[settings] WARNING: %d wide is too much for H.264 (4096 max): pick a lower resolution or foveated encoding", st.W);
    std::lock_guard<std::mutex> g(g_runLock);
    g_run = st;
}
void applySettings() {
    Stream before = currentStream();
    computeStream();
    Stream st = currentStream();
    logmsg("[settings] %d per eye · %d Hz · %d Mbps · %s · foveated %s · encoding %dx%d", st.eye, st.fps, st.bitrate / 1000000,
           st.hevc ? "HEVC" : "H.264", st.fov.on() ? st.fov.text().c_str() : "off", st.W, st.H);
    if (before.W != st.W || before.H != st.H || before.hevc != st.hevc || before.fps != st.fps || before.bitrate != st.bitrate || before.eye != st.eye ||
        before.fov.text() != st.fov.text()) {
        gameQualityChanged();
        Json c = Json::object();
        c.set("type", "config").set("eye", st.eye).set("fps", st.fps).set("vw", st.W).set("vh", st.H)
         .set("fov", st.fov.on() ? st.fov.text() : std::string("0")).set("codec", st.hevc ? "hevc" : "h264");
        questSendText(c.dump());
        gameConfigChanged();
        questRequestKey();
    }
}

// ---------------- Rendimiento (para el indicador del visor) ----------------
static std::mutex g_perfLock;
static Json g_perfRuntime = Json::object(), g_perfLast = Json::object();
static double sumMac = 0, sumEnc = 0;
static int nMac = 0, nEnc = 0, dropped = 0;
static uint64_t perfAt = 0;
void perfSetRuntime(const std::string &json) {
    Json j = Json::parse(json);
    if (j.t != Json::Obj) return;
    std::lock_guard<std::mutex> g(g_perfLock);
    g_perfRuntime = j;
}
void perfAddMac(double ms) { std::lock_guard<std::mutex> g(g_perfLock); sumMac += ms; nMac++; }
void perfAddEnc(double ms) { std::lock_guard<std::mutex> g(g_perfLock); sumEnc += ms; nEnc++; }
void perfAddDrop() { std::lock_guard<std::mutex> g(g_perfLock); dropped++; }
Json perfSnapshot() {
    std::lock_guard<std::mutex> g(g_perfLock);
    uint64_t now = nowNs();
    if (now - perfAt < 1000000000ull) return g_perfLast;
    Json o = g_perfRuntime;
    o.t = Json::Obj;
    o.set("mac", nMac ? std::round(sumMac / nMac * 100) / 100 : 0.0);
    o.set("video", nEnc ? std::round(sumEnc / nEnc * 100) / 100 : 0.0);
    o.set("recibidos", nMac).set("saltados", dropped).set("termico", 0);
    sumMac = sumEnc = 0; nMac = nEnc = dropped = 0;
    g_perfLast = o; perfAt = now;
    return o;
}

// ---------------- Perfiles de juegos ----------------
// Igual que en la Mac: "" son los ajustes de todos los juegos; cada juego, por su carpeta de Steam, con lo suyo encima
static std::mutex g_profLock;
static Json g_prof = Json::object();
static bool g_profLoaded = false;
static std::string g_current;
static std::wstring profFile() { return dataDir() + L"\\game_profiles.json"; }
static void profLoad() {
    if (g_profLoaded) return;
    g_profLoaded = true;
    Json j = Json::parse(readFile(profFile()));
    if (j.t == Json::Obj) g_prof = j;
}
struct Eff { double scale, fovk; bool autoS, index, vive; };
static Eff effective(const std::string &key) {
    const Json &a = g_prof[""], &g = key.empty() ? Json() : g_prof[key];
    auto pick = [&](const char *k) -> const Json & { return g.has(k) ? g[k] : a[k]; };
    Eff e;
    e.scale = std::min(1.0, std::max(0.5, pick("scale").num(1)));
    e.fovk = std::min(1.0, std::max(0.75, pick("fovk").num(1)));
    e.autoS = pick("auto").boolean(false);
    e.index = pick("index").boolean(false);
    e.vive = pick("vive").boolean(false);
    return e;
}
std::string profilesConfigLine(int fps, bool hello) {
    std::lock_guard<std::mutex> g(g_profLock);
    profLoad();
    Eff e = effective(g_current);
    Stream st = currentStream();
    int eye = std::max(256, (int)std::lround(st.eye * e.scale * e.fovk / 8) * 8);
    char b[256];
    snprintf(b, sizeof b, "{\"type\":\"config\",\"eye\":%d,\"fps\":%d,\"fovk\":%.3f,\"index\":%d,\"vive\":%d,\"vg\":0%s}", eye, fps, e.fovk,
             e.index ? 1 : 0, e.vive ? 1 : 0, hello ? ",\"pv\":1" : "");
    return b;
}
std::string profilesCurrent() { std::lock_guard<std::mutex> g(g_profLock); return g_current; }
void profilesBegin(const std::string &app) {
    std::string key = gamesRunningName();
    if (key.empty()) key = app;
    std::lock_guard<std::mutex> g(g_profLock);
    profLoad();
    g_current = key;
    Eff e = effective(key);
    logmsg("[game] Settings for \"%s\": scale %d %%, field of view %d %%%s%s", key.c_str(), (int)(e.scale * 100), (int)(e.fovk * 100),
           e.index ? ", Index controllers" : "", e.vive ? ", Vive controllers" : "");
}
void profilesEnd() { std::lock_guard<std::mutex> g(g_profLock); g_current.clear(); }
Json profilesState() {
    std::lock_guard<std::mutex> g(g_profLock);
    profLoad();
    Eff a = effective("");
    Json o = Json::object();
    o.set("all", Json::object().set("scale", a.scale).set("auto", a.autoS).set("fovk", a.fovk).set("index", a.index).set("vive", a.vive));
    Json own = Json::object();
    for (auto &kv : g_prof.o) if (!kv.first.empty()) own.set(kv.first, kv.second);
    o.set("own", own);
    if (!g_current.empty()) {
        Eff e = effective(g_current);
        o.set("game", Json::object().set("name", g_current).set("scale", e.scale).set("auto", e.autoS).set("fovk", e.fovk).set("index", e.index)
                          .set("vive", e.vive).set("own", g_prof.has(g_current)));
    }
    return o;
}
void profilesApply(const Json &m) {
    std::lock_guard<std::mutex> g(g_profLock);
    profLoad();
    std::string scope = m["scope"].str("all");
    std::string key = scope == "game" ? g_current : scope.rfind("key:", 0) == 0 ? scope.substr(4) : "";
    if (scope != "all" && key.empty()) return;
    if (m["reset"].boolean(false)) g_prof.o.erase(key);
    else {
        Json p = g_prof[key];
        p.t = Json::Obj;
        if (m.has("scale")) p.set("scale", std::min(1.0, std::max(0.5, m["scale"].num())));
        if (m.has("auto")) p.set("auto", m["auto"].boolean());
        if (m.has("fovk")) p.set("fovk", std::min(1.0, std::max(0.75, m["fovk"].num())));
        if (m.has("index")) p.set("index", m["index"].boolean());
        if (m.has("vive")) p.set("vive", m["vive"].boolean());
        if (!key.empty()) {
            if (m.has("eye")) p.set("eye", (int)m["eye"].num());
            if (m.has("fps")) p.set("fps", (int)m["fps"].num());
            if (m.has("mbps")) p.set("mbps", (int)m["mbps"].num());
            if (m.has("fov")) p.set("fov", m["fov"].str());
        }
        g_prof.set(key, p);
    }
    writeFile(profFile(), g_prof.dump());
    logmsg("[game] Advanced settings for %s saved (they apply the next time the game opens)", key.empty() ? "all games" : key.c_str());
}
