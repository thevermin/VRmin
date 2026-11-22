/* VRmin · ocultar_steam.exe: esconde la ventana de Steam (Steam sigue abierto de fondo, como al cerrarla con la X).
 * Busca las ventanas de primer nivel de steam.exe que se ven (la biblioteca, la tienda, «Amigos») y les manda
 * WM_CLOSE: Steam las cierra pero no se cierra él. La de iniciar sesión no se toca (se llama «Iniciar sesión en
 * Steam» / «Sign in to Steam» y no «Steam»), así que si hace falta entrar, sigue ahí.
 * Compilar: x86_64-w64-mingw32-gcc -O2 -s -mwindows -municode -o ocultar_steam.exe ocultar_steam.c */
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>

/* Todos los procesos de Steam: puede haber más de un steam.exe (el que abre un juego o «Mostrar Steam» para avisarle
 * al que ya está abierto) y la ventana puede ser de su navegador (steamwebhelper). Con solo el primer steam.exe que
 * aparecía, a veces no se encontraba ninguna ventana y Steam quedaba a la vista. */
static DWORD pids[64];
static int npids = 0;
static void steam_pids(void) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e = {sizeof e};
    if (s != INVALID_HANDLE_VALUE && Process32FirstW(s, &e)) {
        do {
            if ((!_wcsicmp(e.szExeFile, L"steam.exe") || !_wcsicmp(e.szExeFile, L"steamwebhelper.exe")
                 || !_wcsicmp(e.szExeFile, L"steamwebhelper_real.exe")) && npids < 64)
                pids[npids++] = e.th32ProcessID;
        } while (Process32NextW(s, &e));
    }
    if (s != INVALID_HANDLE_VALUE) CloseHandle(s);
}

static int cerradas = 0;
static BOOL CALLBACK una(HWND w, LPARAM nada) {
    DWORD p = 0;
    wchar_t t[128] = L"";
    GetWindowThreadProcessId(w, &p);
    int de_steam = 0;
    for (int i = 0; i < npids; i++) if (pids[i] == p) de_steam = 1;
    if (!de_steam || !IsWindowVisible(w)) return TRUE;
    GetWindowTextW(w, t, 128);
    if (!wcscmp(t, L"Steam") || !wcscmp(t, L"Friends List") || !wcscmp(t, L"Lista de amigos")) {
        PostMessageW(w, WM_CLOSE, 0, 0);
        cerradas++;
    }
    return TRUE;
}

int WINAPI wWinMain(HINSTANCE a, HINSTANCE b, PWSTR c, int d) {
    steam_pids();
    if (!npids) return 2;
    EnumWindows(una, 0);
    return cerradas ? 0 : 1;
}
