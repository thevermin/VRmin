/* De github.com/ramiabih/play-windows-steam-on-mac (licencia MIT, Copyright (c) 2026 Daniil Efimenko), sin cambios.
 * Arregla la ventana negra de Steam en Wine 11 (Mac): agrega --disable-gpu --single-process a steamwebhelper. */
/*
 * steamwebhelper-wrapper — prepend Chromium flags and delegate to
 * steamwebhelper_real.exe. Fixes black Steam UI on Wine/macOS.
 *
 * Build: make -C wrapper
 * Install: scripts/install-wrapper.sh
 */

#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif

#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <wchar.h>

#define EXTRA_FLAGS_SUFFIX \
    L"--disable-gpu --single-process " \
    L"--disable-features=IsolateOrigins,site-per-process,SpareRendererForSitePerProcess"
#define REAL_BINARY L"steamwebhelper_real.exe"

static wchar_t *resolve_real_binary(void)
{
    wchar_t self[MAX_PATH];
    DWORD len = GetModuleFileNameW(NULL, self, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return NULL;

    wchar_t *slash = wcsrchr(self, L'\\');
    if (!slash) return NULL;
    *(slash + 1) = L'\0';

    size_t cap = wcslen(self) + wcslen(REAL_BINARY) + 1;
    wchar_t *real = (wchar_t *)calloc(cap, sizeof(wchar_t));
    if (!real) return NULL;
    wcscpy(real, self);
    wcscat(real, REAL_BINARY);
    return real;
}

static const wchar_t *args_tail(void)
{
    const wchar_t *cmd = GetCommandLineW();
    if (!cmd) return L"";

    int in_quotes = 0;
    while (*cmd) {
        wchar_t c = *cmd;
        if (c == L'"') in_quotes = !in_quotes;
        else if (c == L' ' && !in_quotes) break;
        ++cmd;
    }
    while (*cmd == L' ') ++cmd;
    return cmd;
}

int wmain(void)
{
    wchar_t *real = resolve_real_binary();
    if (!real) return 1;

    const wchar_t *tail = args_tail();
    size_t cap = wcslen(real) + wcslen(EXTRA_FLAGS_SUFFIX) + wcslen(tail) + 8;
    wchar_t *cmdline = (wchar_t *)calloc(cap, sizeof(wchar_t));
    if (!cmdline) {
        free(real);
        return 1;
    }
    /* Append our flags after Steam's so they override --valve-enable-site-isolation
       (needed for checkout/payment iframes in the store). */
    _snwprintf(cmdline, cap, L"\"%ls\" %ls %ls", real, tail, EXTRA_FLAGS_SUFFIX);

    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    if (!CreateProcessW(real, cmdline, NULL, NULL, TRUE, 0, NULL, NULL, &si, &pi)) {
        free(cmdline);
        free(real);
        return 1;
    }

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    free(cmdline);
    free(real);
    return (int)code;
}
