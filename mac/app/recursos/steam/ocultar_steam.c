/* VRmin · ocultar_steam.exe: esconde la ventana de Steam (Steam sigue abierto de fondo, como al cerrarla con la X).
 * Busca las ventanas de primer nivel de steam.exe que se ven (la biblioteca, la tienda, «Amigos») y les manda
 * WM_CLOSE: Steam las cierra pero no se cierra él. La de iniciar sesión no se toca (se llama «Iniciar sesión en
 * Steam» / «Sign in to Steam» y no «Steam»), así que si hace falta entrar, sigue ahí.
 * Compilar: x86_64-w64-mingw32-gcc -O2 -mwindows -o ocultar_steam.exe ocultar_steam.c */
#include <windows.h>
#include <tlhelp32.h>
#include <wchar.h>

static DWORD steam_pid(void) {
    DWORD pid = 0;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e = {sizeof e};
    if (s != INVALID_HANDLE_VALUE && Process32FirstW(s, &e)) {
        do { if (!_wcsicmp(e.szExeFile, L"steam.exe")) { pid = e.th32ProcessID; break; } } while (Process32NextW(s, &e));
    }
    if (s != INVALID_HANDLE_VALUE) CloseHandle(s);
    return pid;
}

static int cerradas = 0;
static BOOL CALLBACK una(HWND w, LPARAM pid) {
    DWORD p = 0;
    wchar_t t[128] = L"";
    GetWindowThreadProcessId(w, &p);
    if (p != (DWORD)pid || !IsWindowVisible(w)) return TRUE;
    GetWindowTextW(w, t, 128);
    if (!wcscmp(t, L"Steam") || !wcscmp(t, L"Friends List") || !wcscmp(t, L"Lista de amigos")) {
        PostMessageW(w, WM_CLOSE, 0, 0);
        cerradas++;
    }
    return TRUE;
}

int WINAPI wWinMain(HINSTANCE a, HINSTANCE b, PWSTR c, int d) {
    DWORD pid = steam_pid();
    if (!pid) return 2;
    EnumWindows(una, (LPARAM)pid);
    return cerradas ? 0 : 1;
}
