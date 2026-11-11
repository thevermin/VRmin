// Monitor virtual: una pantalla más de la PC que solo se ve en el Quest. Windows necesita un driver firmado para eso: se
// usa Virtual Display Driver (MIT, firmado por SignPath Foundation), que VRmin Host instala con un botón la primera vez
// (Windows pide permiso de administrador). Queda instalado pero fuera del escritorio: VRmin lo agrega al escritorio
// cuando el Quest abre el monitor virtual y lo saca al cerrarlo (eso no necesita permisos).
#include "host.h"
#include <shellapi.h>
#include <urlmon.h>
#include <bcrypt.h>
#include <thread>

static const wchar_t *VDD_URL = L"https://github.com/VirtualDrivers/Virtual-Display-Driver/releases/download/25.7.23/VirtualDisplayDriver-x86.Driver.Only.zip";
static const char *VDD_SHA256 = "e24210692b442b39af763536330ce78b423f19342b7a7792c26de3944e418b3a";
static const wchar_t *NEFCON_URL = L"https://github.com/nefarius/nefcon/releases/download/v1.14.0/nefcon_v1.14.0.zip";
static const char *NEFCON_SHA256 = "a15557da24a9efca203158de3b43b0eaf982db231f0194031f1ed428bc13e669";
static const uint32_t DISPLAY_BIT = 0x40000000;

static std::mutex vLock;
static bool g_on = false;
static std::string g_err, g_busy;

/// El monitor del driver (\\.\DISPLAYn), esté o no en el escritorio ("" si el driver no está)
static std::wstring vddDevice(bool *attached = nullptr) {
    DISPLAY_DEVICEW dd{};
    dd.cb = sizeof dd;
    for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); i++, dd.cb = sizeof dd)
        if (wcsstr(dd.DeviceString, L"Virtual Display Driver")) {
            if (attached) *attached = (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) != 0;
            return dd.DeviceName;
        }
    return L"";
}
bool vmonInstalled() { return !vddDevice().empty(); }

/// El número de pantalla del monitor virtual (como los demás monitores) mientras está abierto; 0 si no
uint32_t vmonDisplayId() {
    { std::lock_guard<std::mutex> g(vLock); if (!g_on) return 0; }
    bool att = false;
    std::wstring d = vddDevice(&att);
    const wchar_t *p = att ? wcsstr(d.c_str(), L"DISPLAY") : nullptr;
    return p ? DISPLAY_BIT | (uint32_t)_wtoi(p + 7) : 0;
}

/// Lo pone en el escritorio (a la derecha de todo, 1920×1080 o lo más parecido que ofrezca) o lo saca
static bool attach(const std::wstring &dev, bool on) {
    DEVMODEW dm{};
    dm.dmSize = sizeof dm;
    dm.dmFields = DM_POSITION | DM_PELSWIDTH | DM_PELSHEIGHT;
    if (on) {
        DWORD bw = 0, bh = 0;
        DEVMODEW m{};
        m.dmSize = sizeof m;
        for (DWORD i = 0; EnumDisplaySettingsW(dev.c_str(), i, &m); i++) {
            bool exact = m.dmPelsWidth == 1920 && m.dmPelsHeight == 1080;
            bool better = m.dmPelsWidth <= 2560 && m.dmPelsWidth * 9 == m.dmPelsHeight * 16 && m.dmPelsWidth > bw;
            if (exact || (bw != 1920 && better)) { bw = m.dmPelsWidth; bh = m.dmPelsHeight; }
            if (!bw && i == 0) { bw = m.dmPelsWidth; bh = m.dmPelsHeight; }
        }
        dm.dmPelsWidth = bw ? bw : 1920;
        dm.dmPelsHeight = bh ? bh : 1080;
        dm.dmPosition.x = GetSystemMetrics(SM_XVIRTUALSCREEN) + GetSystemMetrics(SM_CXVIRTUALSCREEN);
        dm.dmPosition.y = 0;
    }
    LONG r = ChangeDisplaySettingsExW(dev.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY | CDS_NORESET, nullptr);
    if (r == DISP_CHANGE_SUCCESSFUL) r = ChangeDisplaySettingsExW(nullptr, nullptr, nullptr, 0, nullptr);
    if (r != DISP_CHANGE_SUCCESSFUL) logmsg("[monitor] Windows didn't %s the virtual monitor (code %ld)", on ? "add" : "remove", r);
    else logmsg("[monitor] Virtual monitor %s%s", on ? "added to the desktop: " : "removed from the desktop", on ? (std::to_string(dm.dmPelsWidth) + "x" + std::to_string(dm.dmPelsHeight)).c_str() : "");
    return r == DISP_CHANGE_SUCCESSFUL;
}

static void setOurs(bool v) {
    { std::lock_guard<std::mutex> g(g_setLock); if (g_set.vmonOurs == v) return; g_set.vmonOurs = v; }
    saveSettings();
}

void vmonSet(bool on) {
    std::wstring dev = vddDevice();
    {
        std::lock_guard<std::mutex> g(vLock);
        if (dev.empty()) {
            g_on = false;
            g_err = on ? tr("Falta instalar el monitor virtual: en VRmin Host, «Monitor virtual» › Instalar", "The virtual monitor isn't installed: in VRmin Host, \"Virtual monitor\" › Install") : "";
            return;
        }
        if (on == g_on) return;
        g_on = on;
        g_err.clear();
    }
    bool ok = attach(dev, on);
    if (on && !ok) { std::lock_guard<std::mutex> g(vLock); g_on = false; g_err = tr("Windows no dejó agregar el monitor virtual", "Windows didn't let VRmin add the virtual monitor"); }
    setOurs(on && ok);
    uiRefresh();
}
bool vmonOn() { std::lock_guard<std::mutex> g(vLock); return g_on; }
std::string vmonError() { std::lock_guard<std::mutex> g(vLock); return g_err; }

/// Al abrir y al cerrar VRmin Host: si quedó en el escritorio por VRmin (se cerró de golpe), se saca
void vmonCleanup() {
    bool ours;
    { std::lock_guard<std::mutex> g(g_setLock); ours = g_set.vmonOurs; }
    bool att = false;
    std::wstring dev = vddDevice(&att);
    if ((ours || vmonOn()) && !dev.empty() && att) attach(dev, false);
    { std::lock_guard<std::mutex> g(vLock); g_on = false; }
    setOurs(false);
}

/// Al abrir VRmin Host: lo que haya quedado, fuera; y si el Quest se desconecta un rato, el monitor se saca (así no
/// quedan ventanas en una pantalla que no se ve)
void vmonStart() {
    std::thread([] {
        vmonCleanup();
        uint64_t away = 0;
        while (true) {
            Sleep(2000);
            if (questConnected() || !vmonOn()) { away = 0; continue; }
            if (!away) away = nowNs();
            else if (nowNs() - away > 15000000000ull) { logmsg("[monitor] The Quest disconnected: removing the virtual monitor"); vmonSet(false); away = 0; }
        }
    }).detach();
}

Json vmonState() {
    std::lock_guard<std::mutex> g(vLock);
    return Json::object().set("installed", !vddDevice().empty()).set("on", g_on).set("busy", g_busy);
}

static std::string sha256File(const std::wstring &path) {
    std::string data = readFile(path);
    BCRYPT_ALG_HANDLE alg;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0) return "";
    uint8_t h[32];
    BCryptHash(alg, nullptr, 0, (PUCHAR)data.data(), (ULONG)data.size(), h, 32);
    BCryptCloseAlgorithmProvider(alg, 0);
    char hex[65];
    for (int i = 0; i < 32; i++) snprintf(hex + i * 2, 3, "%02x", h[i]);
    return hex;
}

/// Baja el driver y nefcon (el instalador de drivers, MIT), los revisa y los instala con permiso de administrador
bool vmonInstall() {
    {
        std::lock_guard<std::mutex> g(vLock);
        if (!g_busy.empty()) return false;
        g_busy = tr("Bajando el monitor virtual…", "Downloading the virtual monitor…");
    }
    auto done = [](bool ok, const char *msg) {
        if (msg) logmsg("[monitor] %s", msg);
        std::lock_guard<std::mutex> g(vLock);
        g_busy.clear();
        return ok;
    };
    std::wstring dir = dataDir() + L"\\vdd", vz = dir + L"\\vdd.zip", nz = dir + L"\\nefcon.zip", x = dir + L"\\files";
    CreateDirectoryW(dir.c_str(), nullptr);
    logmsg("[monitor] Downloading Virtual Display Driver 25.7.23 and nefcon 1.14.0…");
    if (FAILED(URLDownloadToFileW(nullptr, VDD_URL, vz.c_str(), 0, nullptr)) || FAILED(URLDownloadToFileW(nullptr, NEFCON_URL, nz.c_str(), 0, nullptr)))
        return done(false, "Couldn't download it. Is the PC online?");
    if (sha256File(vz) != VDD_SHA256 || sha256File(nz) != NEFCON_SHA256) return done(false, "The downloaded files aren't the expected ones: not installed");
    wchar_t sys[MAX_PATH];
    GetSystemDirectoryW(sys, MAX_PATH);
    std::wstring ps = std::wstring(sys) + L"\\WindowsPowerShell\\v1.0\\powershell.exe";
    runCapture(ps, L"-NoProfile -NonInteractive -Command \"Remove-Item -LiteralPath '" + x + L"' -Recurse -Force -ErrorAction SilentlyContinue; Expand-Archive -LiteralPath '" + vz +
                       L"' -DestinationPath '" + x + L"' -Force; Expand-Archive -LiteralPath '" + nz + L"' -DestinationPath '" + x + L"' -Force\"", 120000, nullptr);
    std::wstring inf = x + L"\\VirtualDisplayDriver\\MttVDD.inf", nef = x + L"\\x64\\nefconw.exe";
    if (GetFileAttributesW(inf.c_str()) == INVALID_FILE_ATTRIBUTES || GetFileAttributesW(nef.c_str()) == INVALID_FILE_ATTRIBUTES) return done(false, "The downloaded files are damaged");
    { std::lock_guard<std::mutex> g(vLock); g_busy = tr("Instalando (aceptá el permiso de Windows)…", "Installing (accept Windows' permission prompt)…"); }
    logmsg("[monitor] Installing: Windows asks for administrator permission (and may ask to trust \"SignPath Foundation\", the driver's signer)");
    // Con permiso de administrador: los ajustes del driver (sus resoluciones) y el driver. Los .zip se vuelven a
    // revisar ahí mismo, por si algo los cambió en el medio.
    std::wstring script = dir + L"\\install.ps1";
    writeFile(script, "param($vz, $nz, $x)\r\n"
                      "if ((Get-FileHash -Algorithm SHA256 $vz).Hash -ne '" + std::string(VDD_SHA256) + "') { exit 3 }\r\n"
                      "if ((Get-FileHash -Algorithm SHA256 $nz).Hash -ne '" + std::string(NEFCON_SHA256) + "') { exit 3 }\r\n"
                      "Remove-Item -LiteralPath $x -Recurse -Force -ErrorAction SilentlyContinue\r\n"
                      "Expand-Archive -LiteralPath $vz -DestinationPath $x -Force\r\n"
                      "Expand-Archive -LiteralPath $nz -DestinationPath $x -Force\r\n"
                      "New-Item -ItemType Directory -Force 'C:\\VirtualDisplayDriver' | Out-Null\r\n"
                      "if (-not (Test-Path 'C:\\VirtualDisplayDriver\\vdd_settings.xml')) { Copy-Item (Join-Path $x 'VirtualDisplayDriver\\vdd_settings.xml') 'C:\\VirtualDisplayDriver\\' }\r\n"
                      "$p = Start-Process -FilePath (Join-Path $x 'x64\\nefconw.exe') -ArgumentList @('install', (Join-Path $x 'VirtualDisplayDriver\\MttVDD.inf'), 'Root\\MttVDD', '--no-duplicates') -Wait -PassThru\r\n"
                      "exit $p.ExitCode\r\n");
    std::wstring args = L"-NoProfile -NonInteractive -ExecutionPolicy Bypass -WindowStyle Hidden -File \"" + script + L"\" \"" + vz + L"\" \"" + nz + L"\" \"" + x + L"\"";
    SHELLEXECUTEINFOW ei{};
    ei.cbSize = sizeof ei;
    ei.fMask = SEE_MASK_NOCLOSEPROCESS;
    ei.lpVerb = L"runas";
    ei.lpFile = ps.c_str();
    ei.lpParameters = args.c_str();
    ei.nShow = SW_HIDE;
    if (!ShellExecuteExW(&ei) || !ei.hProcess) return done(false, "Not installed: Windows' permission wasn't given");
    WaitForSingleObject(ei.hProcess, 300000);
    DWORD code = 1;
    GetExitCodeProcess(ei.hProcess, &code);
    CloseHandle(ei.hProcess);
    // Windows lo agrega solo al escritorio al instalarlo: queda fuera hasta que el Quest lo abra
    bool att = false;
    std::wstring dev;
    for (int i = 0; i < 40 && (dev = vddDevice(&att)).empty(); i++) Sleep(500);
    if (dev.empty()) {
        logmsg("[monitor] The installer finished with code %lu", code);
        return done(false, "The virtual monitor didn't show up after installing");
    }
    if (att && !vmonOn()) attach(dev, false);
    return done(true, "Done: the virtual monitor is installed. Open it from the headset (Add monitor)");
}
