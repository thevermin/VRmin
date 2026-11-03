// Sonido de la PC al Quest: lo que suena en la salida de Windows (WASAPI, "loopback"), en bloques de 20 ms
// Paquete 2: [2,0,0,0][Hz u32][muestras de 16 bits, estéreo intercalado]
#include "host.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <thread>

bool lobbyShowsDesk();

static bool wantAudio() {
    if (!questConnected()) return false;
    Settings s;
    { std::lock_guard<std::mutex> g(g_setLock); s = g_set; }
    if (gameActive()) return s.audio != "mac";
    return lobbyShowsDesk() && deskOn() && (s.deskAudio == "quest" || s.deskAudio == "both");
}

static void audioThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    std::vector<int16_t> acc;
    bool warned = false;
    while (true) {
        if (!wantAudio()) { Sleep(100); continue; }
        IMMDeviceEnumerator *en = nullptr;
        IMMDevice *dev = nullptr;
        IAudioClient *ac = nullptr;
        IAudioCaptureClient *cc = nullptr;
        WAVEFORMATEX *wf = nullptr;
        auto done = [&] {
            if (ac) ac->Stop();
            if (cc) cc->Release();
            if (ac) ac->Release();
            if (dev) dev->Release();
            if (en) en->Release();
            if (wf) CoTaskMemFree(wf);
            cc = nullptr; ac = nullptr; dev = nullptr; en = nullptr; wf = nullptr;
        };
        if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
            FAILED(en->GetDefaultAudioEndpoint(eRender, eConsole, &dev)) ||
            FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&ac)) ||
            FAILED(ac->GetMixFormat(&wf)) ||
            FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK, 200000, 0, wf, nullptr)) ||
            FAILED(ac->GetService(IID_PPV_ARGS(&cc))) || FAILED(ac->Start())) {
            if (!warned) logmsg("[audio] No pude capturar el sonido de la PC");
            warned = true;
            done();
            Sleep(3000);
            continue;
        }
        int ch = wf->nChannels, rate = (int)wf->nSamplesPerSec, bits = wf->wBitsPerSample;
        bool isFloat = wf->wFormatTag == WAVE_FORMAT_IEEE_FLOAT;
        if (wf->wFormatTag == WAVE_FORMAT_EXTENSIBLE) {
            WAVEFORMATEXTENSIBLE *x = (WAVEFORMATEXTENSIBLE *)wf;
            static const GUID FLOAT_FMT = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};
            isFloat = IsEqualGUID(x->SubFormat, FLOAT_FMT);
        }
        logmsg("[audio] Enviando el sonido de la PC al Quest (%d Hz, %d canales, %s)", rate, ch, isFloat ? "float" : "enteros");
        acc.clear();
        const size_t chunk = (size_t)(rate / 50) * 2;   // 20 ms, 2 canales
        while (wantAudio()) {
            Sleep(5);
            UINT32 n = 0;
            bool fail = false;
            while (cc->GetNextPacketSize(&n) == S_OK && n > 0) {
                BYTE *data = nullptr;
                UINT32 frames = 0;
                DWORD flags = 0;
                if (FAILED(cc->GetBuffer(&data, &frames, &flags, nullptr, nullptr))) { fail = true; break; }
                bool silent = (flags & AUDCLNT_BUFFERFLAGS_SILENT) != 0;
                for (UINT32 i = 0; i < frames; i++) {
                    float l = 0, r = 0;
                    if (!silent) {
                        if (isFloat && bits == 32) { const float *p = (const float *)data + (size_t)i * ch; l = p[0]; r = ch > 1 ? p[1] : p[0]; }
                        else if (bits == 16) { const int16_t *p = (const int16_t *)data + (size_t)i * ch; l = p[0] / 32768.f; r = (ch > 1 ? p[1] : p[0]) / 32768.f; }
                        else if (bits == 32) { const int32_t *p = (const int32_t *)data + (size_t)i * ch; l = p[0] / 2147483648.f; r = (ch > 1 ? p[1] : p[0]) / 2147483648.f; }
                    }
                    acc.push_back((int16_t)(std::max(-1.f, std::min(1.f, l)) * 32767));
                    acc.push_back((int16_t)(std::max(-1.f, std::min(1.f, r)) * 32767));
                }
                cc->ReleaseBuffer(frames);
            }
            if (fail) break;
            // Windows no manda nada cuando no suena nada: está bien, el Quest no necesita silencio
            while (acc.size() >= chunk) {
                std::vector<uint8_t> p(8 + chunk * 2);
                p[0] = 2;
                p[4] = (uint8_t)(rate >> 24); p[5] = (uint8_t)(rate >> 16); p[6] = (uint8_t)(rate >> 8); p[7] = (uint8_t)rate;
                memcpy(p.data() + 8, acc.data(), chunk * 2);
                acc.erase(acc.begin(), acc.begin() + chunk);
                questSend(std::move(p));
            }
        }
        done();
    }
}

void audioStart() { std::thread(audioThread).detach(); }
