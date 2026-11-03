// Preparar Windows para los juegos VR:
//  - el runtime de OpenXR de VRmin como el activo (es una clave de Windows para todos: pide permiso de administrador)
//  - OpenComposite en lugar de SteamVR, para los juegos de SteamVR (archivo openvrpaths.vrpath del usuario: sin permisos)
// «Volver a SteamVR» deja las dos cosas como estaban. Al cerrar VRmin Host, SteamVR vuelve solo.
#include "host.h"
#include <shellapi.h>
#include <shlobj.h>
#include <urlmon.h>

static const wchar_t *XR_KEY = L"SOFTWARE\\Khronos\\OpenXR\\1";
static const char *OC_URL_64 = "https://znix.xyz/OpenComposite/download.php?arch=x64&branch=openxr";
static const char *OC_URL_32 = "https://github.com/GameNative/opencomposite/releases/download/v10/opencomposite_x86.dll";

static std::wstring ourJson(bool b32) { return exeDir() + (b32 ? L"\\runtime\\questvrbridge_openxr32.json" : L"\\runtime\\questvrbridge_openxr.json"); }
static std::wstring ocDir() { return dataDir() + L"\\OpenComposite"; }
static std::wstring vrpathFile() {
    wchar_t *p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p))) { r = std::wstring(p) + L"\\openvr\\openvrpaths.vrpath"; CoTaskMemFree(p); }
    return r;
}

static std::wstring activeRuntime(bool b32) {
    wchar_t buf[1024];
    DWORD sz = sizeof buf;
    if (RegGetValueW(HKEY_LOCAL_MACHINE, XR_KEY, L"ActiveRuntime", RRF_RT_REG_SZ | (b32 ? RRF_SUBKEY_WOW6432KEY : RRF_SUBKEY_WOW6464KEY), nullptr, buf, &sz) == ERROR_SUCCESS)
        return buf;
    return L"";
}

static bool ocFilesOk() {
    return GetFileAttributesW((ocDir() + L"\\bin\\vrclient_x64.dll").c_str()) != INVALID_FILE_ATTRIBUTES;
}
static bool ocActive() {
    Json j = Json::parse(readFile(vrpathFile()));
    const Json &r = j["runtime"];
    return r.t == Json::Arr && !r.a.empty() && _wcsicmp(wide(r.a[0].str()).c_str(), ocDir().c_str()) == 0;
}

SetupState setupCheck() {
    SetupState s;
    s.runtimeNow = utf8(activeRuntime(false));
    s.runtimeOk = _wcsicmp(activeRuntime(false).c_str(), ourJson(false).c_str()) == 0;
    s.ocOk = ocFilesOk() && ocActive();
    return s;
}

/// Ejecuta como administrador (Windows muestra su cartel de permiso) y espera
static bool runElevated(const std::wstring &exe, const std::wstring &args) {
    SHELLEXECUTEINFOW ei{};
    ei.cbSize = sizeof ei;
    ei.fMask = SEE_MASK_NOCLOSEPROCESS;
    ei.lpVerb = L"runas";
    ei.lpFile = exe.c_str();
    ei.lpParameters = args.c_str();
    ei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&ei) || !ei.hProcess) return false;
    WaitForSingleObject(ei.hProcess, 60000);
    DWORD code = 1;
    GetExitCodeProcess(ei.hProcess, &code);
    CloseHandle(ei.hProcess);
    return code == 0;
}

static bool setRuntimes(const std::wstring &j64, const std::wstring &j32) {
    std::wstring args = L"/c reg add \"HKLM\\SOFTWARE\\Khronos\\OpenXR\\1\" /v ActiveRuntime /t REG_SZ /d \"" + j64 + L"\" /f /reg:64";
    if (!j32.empty()) args += L" && reg add \"HKLM\\SOFTWARE\\Khronos\\OpenXR\\1\" /v ActiveRuntime /t REG_SZ /d \"" + j32 + L"\" /f /reg:32";
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    return runElevated(std::wstring(sys) + L"\\cmd.exe", args);
}

static bool download(const char *url, const std::wstring &to) {
    HRESULT hr = URLDownloadToFileW(nullptr, wide(url).c_str(), to.c_str(), 0, nullptr);
    if (FAILED(hr)) { logmsg("[preparar] No pude bajar %s (0x%08lx)", url, (unsigned long)hr); return false; }
    // (que sea una DLL de verdad y no una página de error)
    std::string head = readFile(to).substr(0, 2);
    if (head != "MZ") { logmsg("[preparar] Lo que bajé de %s no es una DLL", url); DeleteFileW(to.c_str()); return false; }
    return true;
}

static bool setVrpath(bool ours) {
    std::wstring f = vrpathFile();
    if (f.empty()) return false;
    Json j = Json::parse(readFile(f));
    if (j.t != Json::Obj) {
        if (!ours) return true;
        std::wstring steam;
        wchar_t buf[MAX_PATH];
        DWORD sz = sizeof buf;
        if (RegGetValueW(HKEY_CURRENT_USER, L"Software\\Valve\\Steam", L"SteamPath", RRF_RT_REG_SZ, nullptr, buf, &sz) == ERROR_SUCCESS) steam = buf;
        j = Json::object();
        j.set("config", Json::array().push(utf8(steam) + "/config")).set("log", Json::array().push(utf8(steam) + "/logs"))
         .set("external_drivers", Json()).set("jsonid", "vrpathreg").set("version", 1);
    }
    Json rt = Json::array();
    std::string oc = utf8(ocDir());
    if (ours) rt.push(oc);
    for (auto &x : j["runtime"].a) if (_wcsicmp(wide(x.str()).c_str(), ocDir().c_str()) != 0) rt.push(x);
    j.set("runtime", rt);
    CreateDirectoryW(f.substr(0, f.find_last_of(L'\\')).c_str(), nullptr);
    return writeFile(f, j.dump());
}

bool setupActivate() {
    bool ok = true;
    // 1) OpenXR: el runtime de VRmin (la primera vez se guarda cuál había, para «Volver a SteamVR»)
    if (_wcsicmp(activeRuntime(false).c_str(), ourJson(false).c_str()) != 0) {
        std::wstring prev = dataDir() + L"\\openxr_anterior.txt";
        if (GetFileAttributesW(prev.c_str()) == INVALID_FILE_ATTRIBUTES)
            writeFile(prev, utf8(activeRuntime(false)) + "\n" + utf8(activeRuntime(true)) + "\n");
        logmsg("[preparar] Pidiendo permiso para activar el runtime de OpenXR de VRmin");
        if (!setRuntimes(ourJson(false), ourJson(true))) { logmsg("[preparar] No se activó el runtime de OpenXR (¿se canceló el permiso?)"); ok = false; }
    }
    // 2) OpenComposite (la primera vez se baja)
    if (!ocFilesOk()) {
        CreateDirectoryW(ocDir().c_str(), nullptr);
        CreateDirectoryW((ocDir() + L"\\bin").c_str(), nullptr);
        logmsg("[preparar] Bajando OpenComposite…");
        if (!download(OC_URL_64, ocDir() + L"\\bin\\vrclient_x64.dll")) ok = false;
        download(OC_URL_32, ocDir() + L"\\bin\\vrclient.dll");   // (los juegos de 32 bits son pocos: si falla, sigue)
    }
    if (ocFilesOk() && !setVrpath(true)) { logmsg("[preparar] No pude activar OpenComposite"); ok = false; }
    SetupState s = setupCheck();
    logmsg("[preparar] OpenXR: %s · OpenComposite: %s", s.runtimeOk ? "VRmin" : "otro", s.ocOk ? "activo" : "no");
    uiRefresh();
    return ok && s.runtimeOk && s.ocOk;
}

bool setupRestoreSteamVR() {
    setVrpath(false);
    std::string prev = readFile(dataDir() + L"\\openxr_anterior.txt");
    bool ok = true;
    if (_wcsicmp(activeRuntime(false).c_str(), ourJson(false).c_str()) == 0) {
        size_t nl = prev.find('\n');
        std::wstring p64 = wide(prev.substr(0, nl)), p32 = nl == std::string::npos ? L"" : wide(prev.substr(nl + 1));
        while (!p32.empty() && (p32.back() == L'\n' || p32.back() == L'\r')) p32.pop_back();
        if (p64.empty()) {
            // (no había ninguno: se borra la clave)
            wchar_t sys[MAX_PATH];
            GetSystemDirectoryW(sys, MAX_PATH);
            ok = runElevated(std::wstring(sys) + L"\\cmd.exe",
                             L"/c reg delete \"HKLM\\SOFTWARE\\Khronos\\OpenXR\\1\" /v ActiveRuntime /f /reg:64 & reg delete \"HKLM\\SOFTWARE\\Khronos\\OpenXR\\1\" /v ActiveRuntime /f /reg:32 & exit 0");
        } else ok = setRuntimes(p64, p32);
        if (ok) DeleteFileW((dataDir() + L"\\openxr_anterior.txt").c_str());
    }
    logmsg("[preparar] SteamVR %s", ok ? "quedó como antes" : "no se pudo restaurar del todo");
    uiRefresh();
    return ok;
}

void setupOnExit() { if (ocActive()) setVrpath(false); }
