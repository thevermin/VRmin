// «Algo anda mal»: junta los registros y los datos de la PC en un .zip en el Escritorio, para mandarlo en un Issue
#include "host.h"
#include <shellapi.h>
#include <shlobj.h>

std::string pcGpuName();

static std::wstring knownFolder(REFKNOWNFOLDERID id) {
    wchar_t *p = nullptr;
    std::wstring r;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &p))) { r = p; CoTaskMemFree(p); }
    return r;
}

static std::string windowsVersion() {
    typedef LONG(WINAPI * RtlGetVersionFn)(OSVERSIONINFOW *);
    OSVERSIONINFOW v{};
    v.dwOSVersionInfoSize = sizeof v;
    RtlGetVersionFn f = (RtlGetVersionFn)(void *)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion");
    if (!f || f(&v) != 0) return "?";
    char b[64];
    snprintf(b, sizeof b, "%lu.%lu (build %lu)", v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber);
    return b;
}

static std::atomic<bool> making{false};

bool reportMake(bool fromHeadset) {
    if (making.exchange(true)) return false;
    SYSTEMTIME t;
    GetLocalTime(&t);
    wchar_t stamp[64];
    swprintf(stamp, 64, L"%04d-%02d-%02d %02d.%02d.%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"VRmin report " + stamp;
    CreateDirectoryW(dir.c_str(), nullptr);
    logmsg("[report] Making a report%s", fromHeadset ? " (asked from the headset)" : "");
    for (const wchar_t *f : {L"VRmin.log", L"VRmin-previous.log", L"settings.json", L"game_profiles.json"})
        CopyFileW((dataDir() + L"\\" + f).c_str(), (dir + L"\\" + f).c_str(), FALSE);
    SetupState s = setupCheck();
    MEMORYSTATUSEX mem{};
    mem.dwLength = sizeof mem;
    GlobalMemoryStatusEx(&mem);
    char info[2048];
    snprintf(info, sizeof info,
             "VRmin for Windows %s\nWindows %s\nGraphics card: %s\nMemory: %llu GB\nQuest: %s\nGame: %s\n"
             "OpenXR runtime: %s (VRmin's: %s)\nOpenComposite active: %s\nHeadset microphone in games: %s (VB-CABLE %s)\n",
             VRMIN_WIN_VERSION, windowsVersion().c_str(), pcGpuName().c_str(), (unsigned long long)(mem.ullTotalPhys >> 30),
             questConnected() ? (g_questWifi ? "connected over Wi-Fi" : "connected over the cable") : "not connected",
             gameActive() ? gamesRunningName().c_str() : "none", s.runtimeNow.empty() ? "none" : s.runtimeNow.c_str(), s.runtimeOk ? "yes" : "no",
             s.ocOk ? "yes" : "no", micEnabled() ? "on" : "off", micCableInstalled() ? "installed" : "not installed");
    writeFile(dir + L"\\system.txt", info);
    // Comprimir con PowerShell (viene con Windows)
    std::wstring desk = knownFolder(FOLDERID_Desktop);
    if (desk.empty()) desk = dataDir();
    std::wstring zip = desk + L"\\VRmin report " + stamp + L".zip";
    std::wstring args = L"-NoProfile -NonInteractive -Command \"Compress-Archive -Path '" + dir + L"\\*' -DestinationPath '" + zip + L"' -Force\"";
    SHELLEXECUTEINFOW ei{};
    ei.cbSize = sizeof ei;
    ei.fMask = SEE_MASK_NOCLOSEPROCESS;
    ei.lpFile = L"powershell.exe";
    ei.lpParameters = args.c_str();
    ei.nShow = SW_HIDE;
    bool ok = ShellExecuteExW(&ei) && ei.hProcess;
    if (ok) { WaitForSingleObject(ei.hProcess, 60000); CloseHandle(ei.hProcess); }
    ok = ok && GetFileAttributesW(zip.c_str()) != INVALID_FILE_ATTRIBUTES;
    if (ok) {
        logmsg("[report] Saved on the Desktop: %s", utf8(zip.substr(zip.find_last_of(L'\\') + 1)).c_str());
        std::wstring sel = L"/select,\"" + zip + L"\"";
        ShellExecuteW(nullptr, L"open", L"explorer.exe", sel.c_str(), nullptr, SW_SHOWNORMAL);
        ShellExecuteW(nullptr, L"open", L"https://github.com/thevermin/VRmin/issues/new/choose", nullptr, nullptr, SW_SHOWNORMAL);
    } else {
        logmsg("[report] Couldn't make the .zip: the files are in %s", utf8(dir).c_str());
        ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }
    making = false;
    return ok;
}
