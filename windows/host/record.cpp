// Grabar un video (por ejemplo, para un trailer): la pantalla principal de la PC (MP4, H.264, 30 cuadros por segundo,
// sin sonido) y, si el visor está conectado por cable con adb, también lo que se ve en el visor. Van a Videos\VRmin.
#include "host.h"
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <shlobj.h>
#include <shellapi.h>
#include <thread>

bool cableReady();

static std::atomic<bool> g_rec{false}, g_stop{false};
static std::atomic<uint64_t> g_recSince{0};
static std::atomic<int> g_running{0};   // hilos que todavía están guardando

static std::wstring recDir() {
    wchar_t *p = nullptr;
    std::wstring d;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Videos, 0, nullptr, &p))) { d = p; CoTaskMemFree(p); }
    if (d.empty()) d = dataDir();
    d += L"\\VRmin";
    CreateDirectoryW(d.c_str(), nullptr);
    return d;
}

/// La pantalla principal, hasta que se pide parar
static void pcThread(std::wstring file) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    MFStartup(MF_VERSION, MFSTARTUP_LITE);
    int sw = GetSystemMetrics(SM_CXSCREEN), sh = GetSystemMetrics(SM_CYSCREEN);
    // (hasta 1920 de ancho; las medidas, pares)
    int w = sw, h = sh;
    if (w > 1920) { h = (int)((int64_t)h * 1920 / w); w = 1920; }
    w &= ~1; h &= ~1;
    const int FPS = 30;
    IMFAttributes *at = nullptr;
    MFCreateAttributes(&at, 1);
    at->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    IMFSinkWriter *sw2 = nullptr;
    DWORD stream = 0;
    bool ok = SUCCEEDED(MFCreateSinkWriterFromURL(file.c_str(), nullptr, at, &sw2));
    if (ok) {
        IMFMediaType *out = nullptr, *in = nullptr;
        MFCreateMediaType(&out);
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, 16000000);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(out, MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(out, MF_MT_FRAME_RATE, FPS, 1);
        MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        ok = SUCCEEDED(sw2->AddStream(out, &stream));
        MFCreateMediaType(&in);
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_NV12);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in, MF_MT_FRAME_SIZE, w, h);
        MFSetAttributeRatio(in, MF_MT_FRAME_RATE, FPS, 1);
        MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        ok = ok && SUCCEEDED(sw2->SetInputMediaType(stream, in, nullptr)) && SUCCEEDED(sw2->BeginWriting());
        out->Release(); in->Release();
    }
    at->Release();
    if (!ok) {
        logmsg("[record] Couldn't start recording the PC");
        if (sw2) sw2->Release();
    } else {
        HDC scr = GetDC(nullptr), mem = CreateCompatibleDC(scr);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof bi.bmiHeader;
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;   // (de arriba hacia abajo)
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        void *bits = nullptr;
        HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, dib);
        SetStretchBltMode(mem, HALFTONE);
        uint64_t t0 = nowNs();
        int64_t n = 0;
        const LONGLONG dur = 10000000LL / FPS;
        while (!g_stop) {
            if (w == sw && h == sh) BitBlt(mem, 0, 0, w, h, scr, 0, 0, SRCCOPY);
            else StretchBlt(mem, 0, 0, w, h, scr, 0, 0, sw, sh, SRCCOPY);
            // el cursor (BitBlt no lo copia)
            CURSORINFO ci{sizeof ci};
            if (GetCursorInfo(&ci) && (ci.flags & CURSOR_SHOWING)) {
                ICONINFO ii{};
                if (GetIconInfo(ci.hCursor, &ii)) {
                    DrawIconEx(mem, (ci.ptScreenPos.x - (int)ii.xHotspot) * w / sw, (ci.ptScreenPos.y - (int)ii.yHotspot) * h / sh, ci.hCursor, 0, 0, 0, nullptr, DI_NORMAL);
                    if (ii.hbmMask) DeleteObject(ii.hbmMask);
                    if (ii.hbmColor) DeleteObject(ii.hbmColor);
                }
            }
            GdiFlush();
            IMFMediaBuffer *buf = nullptr;
            DWORD size = (DWORD)w * h * 3 / 2;
            if (SUCCEEDED(MFCreateMemoryBuffer(size, &buf))) {
                BYTE *p = nullptr;
                buf->Lock(&p, nullptr, nullptr);
                bgraToNv12((const uint8_t *)bits, w * 4, w, h, p);
                buf->Unlock();
                buf->SetCurrentLength(size);
                IMFSample *s = nullptr;
                MFCreateSample(&s);
                s->AddBuffer(buf);
                s->SetSampleTime(n * dur);
                s->SetSampleDuration(dur);
                sw2->WriteSample(stream, s);
                s->Release();
                buf->Release();
            }
            n++;
            int64_t next = (int64_t)(t0 + (uint64_t)n * 1000000000ull / FPS) - (int64_t)nowNs();
            if (next > 0) Sleep((DWORD)(next / 1000000));
            else if (next < -500000000LL) n = (int64_t)((nowNs() - t0) * FPS / 1000000000ull);   // (atrasado: se saltean cuadros)
        }
        sw2->Finalize();
        sw2->Release();
        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
        ReleaseDC(nullptr, scr);
        logmsg("[record] PC recording: %s", utf8(file).c_str());
    }
    MFShutdown();
    CoUninitialize();
    g_running--;
}

/// El visor: screenrecord de Android, en tramos de 3 minutos (su límite), hasta que se pide parar
static void questThread(std::wstring stamp) {
    std::wstring adb = adbPath(), dir = recDir();
    int n = 1;
    bool told = false;
    while (!g_stop) {
        std::wstring f = L"/sdcard/Movies/vrmin_grabacion_" + std::to_wstring(n) + L".mp4";
        uint64_t t0 = nowNs();
        std::atomic<bool> segDone{false};
        std::thread stopper([&] {   // (al pedir parar, screenrecord termina y guarda)
            while (!g_stop && !segDone) Sleep(300);
            if (g_stop && !segDone) runCapture(adb, L"shell pkill -INT screenrecord", 10000, nullptr);
        });
        runCapture(adb, L"shell screenrecord --bit-rate 24000000 --time-limit 180 " + f, 200000, nullptr);
        segDone = true;
        Sleep(1000);
        // Si terminó enseguida, el visor no estaba mostrando nada (pantalla apagada: no lo tenés puesto). No se
        // guarda ese tramo vacío; se vuelve a probar en un rato con el mismo número.
        if (!g_stop && nowNs() - t0 < 4000000000ull) {
            runCapture(adb, L"shell rm -f " + f, 10000, nullptr);
            if (!told) { told = true; logmsg("[record] Headset: the display is off; recording starts when you put it on"); }
            stopper.join();
            Sleep(2000);
            continue;
        }
        stopper.join();
        std::wstring dest = dir + L"\\VRmin " + stamp + L" - Headset " + std::to_wstring(n) + L".mp4";
        runCapture(adb, L"pull " + f + L" \"" + dest + L"\"", 120000, nullptr);
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(dest.c_str(), GetFileExInfoStandard, &fa) && (fa.nFileSizeLow > 100000 || fa.nFileSizeHigh)) {
            logmsg("[record] Headset recording: %s", utf8(dest).c_str());
            n++;
            told = false;
        } else {
            DeleteFileW(dest.c_str());
            logmsg("[record] Headset part %d came out empty (did the headset's display turn off?): not saved", n);
        }
        runCapture(adb, L"shell rm -f " + f, 10000, nullptr);
    }
    g_running--;
}

bool recording() { return g_rec; }
uint64_t recordingSince() { return g_recSince; }

void recordSet(bool on) {
    if (on == g_rec) return;
    if (on) {
        if (g_running > 0) return;   // (todavía guarda la anterior)
        bool quest;
        { std::lock_guard<std::mutex> g(g_setLock); quest = g_set.recQuest; }
        quest = quest && cableReady();
        SYSTEMTIME t;
        GetLocalTime(&t);
        wchar_t stamp[64];
        swprintf(stamp, 64, L"%04d-%02d-%02d %02d.%02d.%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
        g_stop = false;
        g_rec = true;
        g_recSince = nowNs();
        g_running = quest ? 2 : 1;
        std::thread(pcThread, recDir() + L"\\VRmin " + stamp + L" - PC.mp4").detach();
        if (quest) std::thread(questThread, std::wstring(stamp)).detach();
        logmsg(quest ? "[record] Recording the PC and the headset…" : "[record] Recording the PC… (the headset isn't connected over the cable with adb: only the PC is recorded)");
    } else {
        g_stop = true;
        g_rec = false;
        logmsg("[record] Stopping: the videos go to Videos › VRmin");
        std::thread([] {
            for (int i = 0; i < 600 && g_running > 0; i++) Sleep(200);
            logmsg("[record] Recording saved in Videos › VRmin");
        }).detach();
    }
    uiRefresh();
}

void recordOpenFolder() { ShellExecuteW(nullptr, L"open", recDir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
