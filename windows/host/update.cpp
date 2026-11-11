// Actualizaciones: las versiones publicadas en GitHub (estables y, si se eligió, beta). VRmin Host se reemplaza solo
// (se cierra, se copia la nueva encima y se abre), y la app del Quest se instala por el cable USB-C con adb.
#include "host.h"
#include <winhttp.h>
#include <urlmon.h>
#include <shellapi.h>
#include <algorithm>
#include <thread>

bool cableReady();
void cableForgetVersion();
std::string cableQuestApp();

static const char *RELEASES = "https://github.com/thevermin/VRmin/releases/download/";

/// "0.40" es más nueva que "0.39" (se compara por partes, como números)
static bool newer(const std::string &a, const std::string &b) {
    auto parts = [](const std::string &s) {
        std::vector<int> v;
        size_t i = 0;
        while (i < s.size()) { v.push_back(atoi(s.c_str() + i)); size_t d = s.find('.', i); if (d == std::string::npos) break; i = d + 1; }
        return v;
    };
    std::vector<int> x = parts(a), y = parts(b);
    for (size_t i = 0; i < std::max(x.size(), y.size()); i++) {
        int p = i < x.size() ? x[i] : 0, q = i < y.size() ? y[i] : 0;
        if (p != q) return p > q;
    }
    return false;
}
/// El primer número con puntos de un nombre («VRmin.Windows.0.3.zip» → "0.3")
static std::string versionIn(const std::string &n) {
    for (size_t i = 0; i < n.size(); i++)
        if (isdigit((unsigned char)n[i])) {
            size_t j = i;
            while (j < n.size() && (isdigit((unsigned char)n[j]) || (n[j] == '.' && j + 1 < n.size() && isdigit((unsigned char)n[j + 1])))) j++;
            std::string v = n.substr(i, j - i);
            if (v.find('.') != std::string::npos) return v;
            i = j;
        }
    return "";
}

static std::string httpGet(const wchar_t *host, const wchar_t *path) {
    std::string out;
    HINTERNET s = WinHttpOpen(L"VRmin Host", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, nullptr, nullptr, 0);
    if (!s) s = WinHttpOpen(L"VRmin Host", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, nullptr, nullptr, 0);
    if (!s) return out;
    HINTERNET c = WinHttpConnect(s, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    HINTERNET r = c ? WinHttpOpenRequest(c, L"GET", path, nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
    if (r && WinHttpSendRequest(r, L"Accept: application/vnd.github+json\r\n", (DWORD)-1L, nullptr, 0, 0, 0) && WinHttpReceiveResponse(r, nullptr)) {
        DWORD code = 0, sz = sizeof code;
        WinHttpQueryHeaders(r, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, nullptr, &code, &sz, nullptr);
        DWORD n = 0;
        char buf[16384];
        while (code == 200 && WinHttpReadData(r, buf, sizeof buf, &n) && n) out.append(buf, n);
    }
    if (r) WinHttpCloseHandle(r);
    if (c) WinHttpCloseHandle(c);
    WinHttpCloseHandle(s);
    return out;
}

// ---------- Lo que hay publicado ----------
struct Nueva { std::string version, url; };
static std::mutex uLock;
static Nueva g_host, g_apk;          // VRmin Host para Windows y la app del Quest del canal elegido ("" si no hay)
static bool g_hostBack = false;      // g_host es volver a la estable (esta es una beta)
static std::vector<std::string> g_betaApks;   // versiones de la app del Quest que salieron solo como beta
static std::string g_updMsg, g_busy;

static bool beta() { std::lock_guard<std::mutex> g(g_setLock); return g_set.beta; }

void updatesCheck(bool manual) {
    if (manual) { std::lock_guard<std::mutex> g(uLock); g_updMsg = tr("Buscando…", "Checking…"); }
    Json list = Json::parse(httpGet(L"api.github.com", L"/repos/thevermin/VRmin/releases?per_page=30"));
    if (list.t != Json::Arr) {
        std::lock_guard<std::mutex> g(uLock);
        if (manual) g_updMsg = tr("No se pudo buscar. ¿Hay internet?", "Couldn't check. Is the PC online?");
        return;
    }
    struct Rel { bool pre; Nueva win, apk; };
    std::vector<Rel> rels;
    for (auto &j : list.a) {
        if (j["draft"].boolean(false)) continue;
        Rel r{j["prerelease"].boolean(false), {}, {}};
        for (auto &a : j["assets"].a) {
            std::string n = a["name"].str(), u = a["browser_download_url"].str();
            if (u.rfind(RELEASES, 0) != 0) continue;
            std::string low = n;
            for (auto &c : low) c = (char)tolower((unsigned char)c);
            if (low.size() > 4 && low.compare(low.size() - 4, 4, ".zip") == 0 && low.find("windows") != std::string::npos) r.win = {versionIn(n.substr(low.find("windows"))), u};   // «VRmin.Windows.0.3.zip»
            if (low.size() > 4 && low.compare(low.size() - 4, 4, ".apk") == 0) r.apk = {versionIn(n), u};   // «VRmin-0.81.apk»
        }
        rels.push_back(r);
    }
    bool b = beta();
    Nueva host, apk;
    bool back = false, minePre = false, mineStable = false;
    std::vector<std::string> stableApks, betaApks;
    for (auto &r : rels) {
        if (r.win.version == VRMIN_WIN_VERSION) (r.pre ? minePre : mineStable) = true;
        if (!r.apk.version.empty()) (r.pre ? betaApks : stableApks).push_back(r.apk.version);
        if (r.pre && !b) continue;
        if (!r.win.version.empty() && (host.version.empty() || newer(r.win.version, host.version))) host = r.win;
        if (!r.apk.version.empty() && (apk.version.empty() || newer(r.apk.version, apk.version))) apk = r.apk;
    }
    bool mineIsBeta = minePre && !mineStable;   // (si la misma versión salió estable, no es beta)
    if (!host.version.empty() && !newer(host.version, VRMIN_WIN_VERSION)) {
        if (!b && mineIsBeta && host.version != VRMIN_WIN_VERSION) back = true;   // esta es una beta y se eligió volver a las estables
        else host = {};
    }
    std::vector<std::string> onlyBeta;
    for (auto &v : betaApks) if (std::find(stableApks.begin(), stableApks.end(), v) == stableApks.end()) onlyBeta.push_back(v);
    std::lock_guard<std::mutex> g(uLock);
    if (host.version != g_host.version && !host.version.empty()) logmsg("[update] VRmin Host for Windows %s is available%s", host.version.c_str(), back ? " (back to the stable version)" : "");
    g_host = host; g_hostBack = back; g_apk = apk; g_betaApks = onlyBeta;
    if (manual) g_updMsg = host.version.empty() ? tr("Estás al día", "You're up to date") : back ? tr("Podés volver a la estable: botón arriba", "You can go back to the stable version: button at the top")
                                                 : tr("Hay una versión nueva: botón arriba", "There's a new version: button at the top");
}

static void updatesThread() {
    while (true) {
        updatesCheck(false);
        Sleep(6 * 3600 * 1000);
    }
}
void updatesStart() { std::thread(updatesThread).detach(); }

Json updatesState() {
    std::lock_guard<std::mutex> g(uLock);
    std::string v = cableQuestApp();
    bool back = !beta() && !v.empty() && v != "?" && !g_apk.version.empty() && g_apk.version != v
                && std::find(g_betaApks.begin(), g_betaApks.end(), v) != g_betaApks.end();
    bool appAction = cableReady() && !g_apk.version.empty() && v != "?" && (v.empty() || newer(g_apk.version, v) || back);
    return Json::object().set("host", g_host.version).set("hostBack", g_hostBack).set("apk", g_apk.version).set("apkBack", back)
        .set("apkAction", appAction).set("msg", g_updMsg).set("busy", g_busy).set("beta", beta());
}

static void busy(const std::string &s) { std::lock_guard<std::mutex> g(uLock); g_busy = s; }

static std::wstring tempDir(const wchar_t *name) {
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring d = std::wstring(tmp) + name;
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

/// Instala (o actualiza, o vuelve a la estable) VRmin en el visor por el cable USB-C
bool updatesInstallQuestApp() {
    Nueva a;
    {
        std::lock_guard<std::mutex> g(uLock);
        if (!g_busy.empty() || g_apk.url.empty()) return false;
        a = g_apk;
        g_busy = tr("Bajando la app del Quest…", "Downloading the Quest app…");
    }
    std::wstring adb = adbPath(), apk = tempDir(L"VRmin update") + L"\\VRmin.apk";
    bool ok = false;
    logmsg("[cable] Downloading the Quest app %s…", a.version.c_str());
    if (adb.empty() || !cableReady()) logmsg("[cable] Connect the Quest with the USB-C cable first");
    else if (FAILED(URLDownloadToFileW(nullptr, wide(a.url).c_str(), apk.c_str(), 0, nullptr))) logmsg("[cable] Couldn't download it. Is the PC online?");
    else {
        busy(tr("Instalando en el visor (no lo desconectes)…", "Installing on the headset (don't unplug it)…"));
        logmsg("[cable] Installing on the headset (don't unplug it)…");
        std::string out = runCapture(adb, L"install -r \"" + apk + L"\"", 300000, nullptr);
        // Volver a una versión anterior (de una beta a la estable): Android no deja instalarla encima; se desinstala
        // la beta y se instala la estable (los ajustes del visor vuelven a los de fábrica)
        if (out.find("INSTALL_FAILED_VERSION_DOWNGRADE") != std::string::npos) {
            logmsg("[cable] It's an older version: uninstalling the one on the headset and installing this one (headset settings go back to defaults)…");
            runCapture(adb, L"uninstall com.vrmin.app", 60000, nullptr);
            out = runCapture(adb, L"install \"" + apk + L"\"", 300000, nullptr);
        }
        ok = out.find("Success") != std::string::npos;
        if (ok) logmsg("[cable] Done: open VRmin in the headset (Library › Unknown sources)");
        else if (out.find("INSTALL_FAILED_UPDATE_INCOMPATIBLE") != std::string::npos)
            logmsg("[cable] The VRmin on the headset has a different signature. Uninstall it in the headset (Library › ⋯ › Uninstall) and click Install again");
        else logmsg("[cable] Couldn't install it: %s", out.c_str());
    }
    DeleteFileW(apk.c_str());
    cableForgetVersion();
    busy("");
    return ok;
}

/// Baja la VRmin Host nueva (o la estable) y se reemplaza: al cerrarse, la nueva se copia encima y se abre
bool updatesInstallHost() {
    Nueva n;
    {
        std::lock_guard<std::mutex> g(uLock);
        if (!g_busy.empty() || g_host.url.empty()) return false;
        n = g_host;
        g_busy = tr("Bajando la versión nueva…", "Downloading the new version…");
    }
    auto fail = [](const char *msg) {
        logmsg("[update] %s", msg);
        ShellExecuteW(nullptr, L"open", L"https://github.com/thevermin/VRmin/releases/latest", nullptr, nullptr, SW_SHOWNORMAL);
        busy("");
        return false;
    };
    // (donde está la app tiene que poder escribirse)
    std::wstring dir = exeDir(), probe = dir + L"\\.vrmin-write-test";
    HANDLE f = CreateFileW(probe.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_FLAG_DELETE_ON_CLOSE, nullptr);
    if (f == INVALID_HANDLE_VALUE) return fail("I can't replace the app where it is: download the new one from the releases page");
    CloseHandle(f);
    std::wstring t = tempDir(L"VRmin update"), zip = t + L"\\new.zip", x = t + L"\\new";
    logmsg("[update] Downloading VRmin Host for Windows %s…", n.version.c_str());
    if (FAILED(URLDownloadToFileW(nullptr, wide(n.url).c_str(), zip.c_str(), 0, nullptr))) return fail("Couldn't download it. Is the PC online?");
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring ps = std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    runCapture(ps, L"-NoProfile -NonInteractive -Command \"Remove-Item -LiteralPath '" + x + L"' -Recurse -Force -ErrorAction SilentlyContinue; Expand-Archive -LiteralPath '" + zip +
                       L"' -DestinationPath '" + x + L"' -Force\"", 120000, nullptr);
    DeleteFileW(zip.c_str());
    // La carpeta de adentro que tiene «VRmin Host.exe»
    std::wstring src;
    if (GetFileAttributesW((x + L"\\VRmin Host.exe").c_str()) != INVALID_FILE_ATTRIBUTES) src = x;
    else {
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((x + L"\\*").c_str(), &fd);
        if (h != INVALID_HANDLE_VALUE) {
            do {
                std::wstring c = x + L"\\" + fd.cFileName;
                if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && fd.cFileName[0] != L'.' && GetFileAttributesW((c + L"\\VRmin Host.exe").c_str()) != INVALID_FILE_ATTRIBUTES) src = c;
            } while (src.empty() && FindNextFileW(h, &fd));
            FindClose(h);
        }
    }
    if (src.empty()) return fail("The downloaded file doesn't contain VRmin Host");
    logmsg("[update] Done: VRmin Host closes and the new version opens");
    // Cuando esta se cierra: la nueva encima y se abre (un script de PowerShell sin ventana)
    std::wstring script = t + L"\\replace.ps1";
    writeFile(script, "param($procId, $src, $dst)\r\n"
                      "Wait-Process -Id $procId -ErrorAction SilentlyContinue\r\n"
                      "Start-Sleep -Milliseconds 500\r\n"
                      "Copy-Item -Path (Join-Path $src '*') -Destination $dst -Recurse -Force\r\n"
                      "Start-Process -FilePath (Join-Path $dst 'VRmin Host.exe')\r\n"
                      "Remove-Item -LiteralPath $src -Recurse -Force -ErrorAction SilentlyContinue\r\n");
    std::wstring args = L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + script + L"\" " + std::to_wstring(GetCurrentProcessId()) +
                        L" \"" + src + L"\" \"" + dir + L"\"";
    std::wstring cmd = L"\"" + ps + L"\" " + args;
    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(ps.c_str(), &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) return fail("Couldn't start the update");
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread);
    busy("");
    // Cerrar VRmin Host (su ventana: así deja Windows como estaba)
    EnumWindows([](HWND w, LPARAM) -> BOOL {
        DWORD pid = 0;
        wchar_t title[64] = {};
        GetWindowThreadProcessId(w, &pid);
        GetWindowTextW(w, title, 64);
        if (pid == GetCurrentProcessId() && wcscmp(title, L"VRmin Host") == 0) { PostMessageW(w, WM_CLOSE, 0, 0); return FALSE; }
        return TRUE;
    }, 0);
    return true;
}

void updatesSetBeta(bool on) {
    { std::lock_guard<std::mutex> g(g_setLock); g_set.beta = on; }
    saveSettings();
    std::thread([] { updatesCheck(false); }).detach();
}
