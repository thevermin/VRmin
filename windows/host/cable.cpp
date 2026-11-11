// El visor por el cable USB-C con adb (las herramientas de Android de Google, que se bajan con un botón): el Modo
// Centinela (que el visor apoyado no apague la pantalla) y abrir VRmin en el visor sin tocarlo
#include "host.h"
#include <thread>
#include <urlmon.h>

std::string questPcName();

static std::wstring toolsDir() { return dataDir() + L"\\tools"; }

static bool exists(const std::wstring &p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

/// adb: el que instala VRmin Host, el del Android SDK o el que esté en el PATH ("" si no hay)
std::wstring adbPath() {
    std::wstring a = toolsDir() + L"\\platform-tools\\adb.exe";
    if (exists(a)) return a;
    wchar_t la[MAX_PATH];
    if (GetEnvironmentVariableW(L"LOCALAPPDATA", la, MAX_PATH)) {
        a = std::wstring(la) + L"\\Android\\Sdk\\platform-tools\\adb.exe";
        if (exists(a)) return a;
    }
    wchar_t found[MAX_PATH];
    if (SearchPathW(nullptr, L"adb.exe", nullptr, MAX_PATH, found, nullptr)) return found;
    return L"";
}

/// Corre un programa sin ventana y devuelve lo que escribió (salida y errores juntos). -1: no se pudo abrir o tardó demasiado
std::string runCapture(const std::wstring &exe, const std::wstring &args, int timeoutMs, int *code) {
    if (code) *code = -1;
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE rd, wr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return "";
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si{};
    si.cb = sizeof si;
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = nullptr;
    PROCESS_INFORMATION pi{};
    std::wstring cmd = L"\"" + exe + L"\" " + args;
    if (!CreateProcessW(exe.c_str(), &cmd[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        return "";
    }
    CloseHandle(wr);
    // (se lee mientras corre; no se espera el fin del tubo: el servidor de adb que queda abierto lo hereda)
    std::string out;
    char buf[4096];
    uint64_t end = nowNs() + (uint64_t)timeoutMs * 1000000ull;
    auto drain = [&] {
        DWORD avail = 0, n = 0;
        while (PeekNamedPipe(rd, nullptr, 0, nullptr, &avail, nullptr) && avail && ReadFile(rd, buf, std::min<DWORD>(avail, sizeof buf), &n, nullptr) && n)
            out.append(buf, n);
    };
    bool done = false;
    while (!done) {
        drain();
        done = WaitForSingleObject(pi.hProcess, 20) == WAIT_OBJECT_0;
        if (!done && nowNs() > end) { TerminateProcess(pi.hProcess, 1); break; }
    }
    drain();
    DWORD ec = 1;
    if (done && GetExitCodeProcess(pi.hProcess, &ec) && code) *code = (int)ec;
    CloseHandle(pi.hProcess); CloseHandle(pi.hThread); CloseHandle(rd);
    return out;
}

static std::string adb(const std::wstring &args, int timeoutMs = 15000, int *code = nullptr) {
    std::wstring a = adbPath();
    return a.empty() ? std::string() : runCapture(a, args, timeoutMs, code);
}

// ---------- Estado del cable (se revisa cada 3 s mientras haya adb) ----------
static std::mutex cLock;
static std::string g_adbState;      // "device" (listo), "unauthorized" (aceptar en el visor), "" (sin Quest por cable)
static std::string g_questApp = "?";   // versión de VRmin en el visor: "?" no se sabe, "" no está instalada
static std::string g_cableBusy;     // lo que se está haciendo ahora (instalar, abrir…)

Json cableState() {
    std::lock_guard<std::mutex> g(cLock);
    return Json::object().set("adb", !adbPath().empty()).set("state", g_adbState).set("questApp", g_questApp).set("busy", g_cableBusy);
}
bool cableReady() { std::lock_guard<std::mutex> g(cLock); return g_adbState == "device"; }
void cableForgetVersion() { std::lock_guard<std::mutex> g(cLock); g_questApp = "?"; }

static void cableThread() {
    bool started = false;
    while (true) {
        Sleep(3000);
        if (adbPath().empty()) { std::lock_guard<std::mutex> g(cLock); g_adbState.clear(); continue; }
        if (!started) { started = true; adb(L"start-server", 20000); }
        std::string out = adb(L"devices", 8000), st;
        size_t p = out.find("List of devices attached");
        if (p != std::string::npos) {
            size_t ln = out.find('\n', p);
            while (ln != std::string::npos && st.empty()) {
                size_t e = out.find('\n', ln + 1);
                std::string line = out.substr(ln + 1, e == std::string::npos ? std::string::npos : e - ln - 1);
                size_t tab = line.find('\t');
                if (tab != std::string::npos) {
                    st = line.substr(tab + 1);
                    while (!st.empty() && (st.back() == '\r' || st.back() == ' ')) st.pop_back();
                }
                ln = e;
            }
        }
        bool needVer;
        {
            std::lock_guard<std::mutex> g(cLock);
            if (st != g_adbState) logmsg("[cable] Quest over USB (adb): %s", st.empty() ? "not connected" : st.c_str());
            g_adbState = st;
            if (st != "device") g_questApp = "?";
            needVer = st == "device" && g_questApp == "?";
        }
        if (needVer) {
            std::string d = adb(L"shell dumpsys package com.vrmin.app", 10000), ver;
            size_t v = d.find("versionName=");
            if (v != std::string::npos) {
                size_t e = d.find_first_of("\r\n ", v + 12);
                ver = d.substr(v + 12, e == std::string::npos ? std::string::npos : e - v - 12);
            }
            std::lock_guard<std::mutex> g(cLock);
            g_questApp = ver;
            logmsg("[cable] VRmin on the headset: %s", ver.empty() ? "not installed" : ver.c_str());
        }
    }
}
void cableStart() { std::thread(cableThread).detach(); }

static void busy(const std::string &s) { std::lock_guard<std::mutex> g(cLock); g_cableBusy = s; }

/// Baja adb (de Google, ≈15 MB) a <datos>\tools\platform-tools
bool cableInstallTools() {
    {
        std::lock_guard<std::mutex> g(cLock);
        if (!g_cableBusy.empty()) return false;
        g_cableBusy = tr("Bajando las herramientas de Android…", "Downloading the Android tools…");
    }
    logmsg("[cable] Downloading Google's Android platform tools (about 15 MB)…");
    CreateDirectoryW(toolsDir().c_str(), nullptr);
    std::wstring zip = toolsDir() + L"\\platform-tools.zip";
    bool ok = SUCCEEDED(URLDownloadToFileW(nullptr, L"https://dl.google.com/android/repository/platform-tools-latest-windows.zip", zip.c_str(), 0, nullptr));
    if (!ok) logmsg("[cable] Couldn't download them. Is the PC online?");
    else {
        busy(tr("Instalando las herramientas de Android…", "Installing the Android tools…"));
        int code = -1;
        wchar_t ps[MAX_PATH];
        GetSystemDirectoryW(ps, MAX_PATH);
        runCapture(std::wstring(ps) + L"\\WindowsPowerShell\\v1.0\\powershell.exe",
                   L"-NoProfile -NonInteractive -Command \"Expand-Archive -LiteralPath '" + zip + L"' -DestinationPath '" + toolsDir() + L"' -Force\"", 120000, &code);
        DeleteFileW(zip.c_str());
        ok = exists(toolsDir() + L"\\platform-tools\\adb.exe");
        if (!ok) logmsg("[cable] The downloaded file is damaged (code %d)", code);
        else {
            std::string v = adb(L"version", 10000);
            logmsg("[cable] Done: %s", v.substr(0, v.find_first_of("\r\n")).c_str());
            logmsg("[cable] Connect the Quest with the USB-C cable (developer mode on) and tap \"Allow\" on the USB debugging prompt in the headset");
        }
    }
    busy("");
    return ok;
}

/// Que el visor apoyado no apague la pantalla (o que vuelva a lo normal)
void cableProximity(bool keepOn) {
    static bool told = false;
    if (keepOn && !told && adbPath().empty()) {
        told = true;
        logmsg("[sentinel] No adb: so the headset doesn't turn off its display, install the cable tools in VRmin Host (or cover the sensor)");
    }
    if (!cableReady()) return;
    int code = -1;
    adb(keepOn ? L"shell am broadcast -a com.oculus.vrpowermanager.prox_close" : L"shell am broadcast -a com.oculus.vrpowermanager.automation_disable", 10000, &code);
    logmsg("[sentinel] adb: proximity sensor %s (code %d)", keepOn ? "ignored" : "normal", code);
    if (keepOn) adb(L"shell input keyevent KEYCODE_WAKEUP", 10000);   // (si el visor apagó la pantalla, los controles dejan de responder)
}

/// «Empezar» el Modo Centinela desde la PC: despierta el visor apoyado y abre VRmin directo en ese modo
bool cableStartSentinel() {
    {
        std::lock_guard<std::mutex> g(cLock);
        if (!g_cableBusy.empty()) return false;
        if (g_adbState != "device") {
            logmsg(g_adbState == "unauthorized" ? "[sentinel] Put on the headset once and tap \"Allow\" on the USB debugging prompt"
                                                : "[sentinel] I can't see the Quest: connect it with the USB-C cable");
            return false;
        }
        g_cableBusy = tr("Abriendo VRmin en el visor…", "Opening VRmin on the headset…");
    }
    logmsg("[sentinel] Waking the headset…");
    adb(L"shell input keyevent KEYCODE_WAKEUP", 10000);
    adb(L"shell am broadcast -a com.oculus.vrpowermanager.prox_close", 10000);
    Sleep(1000);
    // (adb shell junta todo en una línea para el visor: el nombre de la PC va entre comillas simples, con sus espacios)
    std::string n = questPcName(), q;
    for (char c : n) { if (c == '\'') q += "'\\''"; else q += c; }
    std::string out = adb(L"shell am start -n com.vrmin.app/org.questvrbridge.client.VrActivity --es mac \"'" + wide(q) + L"'\" --ez vigia true", 20000);
    bool ok = out.find("Error") == std::string::npos;
    logmsg(ok ? "[sentinel] Done: leave the headset resting facing you at chest height, and open a game"
              : "[sentinel] Couldn't open VRmin on the headset: %s", out.c_str());
    busy("");
    return ok;
}
