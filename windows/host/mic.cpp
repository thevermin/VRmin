// Micrófono del visor en los juegos: el Quest manda lo que graba (paquete 0x0B: 48 kHz, mono, 16 bits) y VRmin Host lo
// pasa a la entrada de VB-CABLE (un micrófono virtual gratuito que instala el usuario). Mientras se usa, la salida de
// VB-CABLE ("CABLE Output") queda como el micrófono de Windows, así los juegos la toman; al terminar, vuelve el de antes.
#include "host.h"
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <propsys.h>
#include <thread>

bool micEnabled() { std::lock_guard<std::mutex> g(g_setLock); return g_set.mic == "quest"; }
void micSetEnabled(bool on) {
    { std::lock_guard<std::mutex> g(g_setLock); g_set.mic = on ? "quest" : "pc"; }
    saveSettings();
    logmsg("[mic] Games microphone: %s", on ? "the headset's" : "the PC's");
    uiRefresh();
}

// ---------- Colchón: lo que llega del Quest (a tirones, por Wi-Fi) y lo que pide Windows (parejo) ----------
static std::mutex ringLock;
static std::vector<int16_t> ring(48000);   // 1 s
static size_t rd = 0, wr = 0, count = 0;
static bool primed = false;
static int cuts = 0, droppedMs = 0;

void micPush(const uint8_t *pcm, size_t n) {
    std::lock_guard<std::mutex> g(ringLock);
    size_t s = n / 2;
    for (size_t i = 0; i < s; i++) {
        ring[wr] = (int16_t)(pcm[i * 2] | pcm[i * 2 + 1] << 8);
        wr = (wr + 1) % ring.size();
        if (count < ring.size()) count++;
        else rd = (rd + 1) % ring.size();
    }
    if (count > 12000) { droppedMs += (int)(count - 3840) / 48; rd = (rd + count - 3840) % ring.size(); count = 3840; }   // (más de 250 ms atrasado: se pone al día)
    if (!primed && count >= 2880) primed = true;   // (60 ms de colchón antes de empezar)
}
static void micPull(int16_t *out, size_t n) {
    std::lock_guard<std::mutex> g(ringLock);
    for (size_t i = 0; i < n; i++) {
        if (primed && count > 0) {
            out[i] = ring[rd];
            rd = (rd + 1) % ring.size();
            count--;
            if (count == 0) { primed = false; cuts++; }
        } else out[i] = 0;
    }
}

// ---------- Dispositivos ----------
static const PROPERTYKEY PK_FRIENDLY = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};

/// El dispositivo de VB-CABLE de ese lado (entrada: donde VRmin Host escribe; salida: el micrófono que ven los juegos)
static std::wstring findCable(EDataFlow flow, const wchar_t *name) {
    std::wstring id;
    IMMDeviceEnumerator *en = nullptr;
    IMMDeviceCollection *col = nullptr;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) return id;
    if (SUCCEEDED(en->EnumAudioEndpoints(flow, DEVICE_STATE_ACTIVE, &col))) {
        UINT n = 0;
        col->GetCount(&n);
        for (UINT i = 0; i < n && id.empty(); i++) {
            IMMDevice *d = nullptr;
            IPropertyStore *ps = nullptr;
            if (FAILED(col->Item(i, &d))) continue;
            if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                PROPVARIANT v;
                PropVariantInit(&v);
                if (SUCCEEDED(ps->GetValue(PK_FRIENDLY, &v)) && v.vt == VT_LPWSTR && wcsstr(v.pwszVal, name)) {
                    LPWSTR s = nullptr;
                    if (SUCCEEDED(d->GetId(&s))) { id = s; CoTaskMemFree(s); }
                }
                PropVariantClear(&v);
                ps->Release();
            }
            d->Release();
        }
        col->Release();
    }
    en->Release();
    return id;
}
static std::wstring defaultCapture(ERole role) {
    std::wstring id;
    IMMDeviceEnumerator *en = nullptr;
    IMMDevice *d = nullptr;
    if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
        if (SUCCEEDED(en->GetDefaultAudioEndpoint(eCapture, role, &d))) {
            LPWSTR s = nullptr;
            if (SUCCEEDED(d->GetId(&s))) { id = s; CoTaskMemFree(s); }
            d->Release();
        }
        en->Release();
    }
    return id;
}

// Elegir el micrófono de Windows: no hay una función pública para eso; esta interfaz es la que usa el propio Windows
// (y programas como EarTrumpet o SoundSwitch)
struct IPolicyConfig : public IUnknown {
    virtual HRESULT STDMETHODCALLTYPE GetMixFormat(PCWSTR, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFormat(PCWSTR, INT, WAVEFORMATEX **) = 0;
    virtual HRESULT STDMETHODCALLTYPE ResetDeviceFormat(PCWSTR) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDeviceFormat(PCWSTR, WAVEFORMATEX *, WAVEFORMATEX *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetProcessingPeriod(PCWSTR, INT, PINT64, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetProcessingPeriod(PCWSTR, PINT64) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetShareMode(PCWSTR, void *) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetPropertyValue(PCWSTR, const PROPERTYKEY &, PROPVARIANT *) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetDefaultEndpoint(PCWSTR, ERole) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetEndpointVisibility(PCWSTR, INT) = 0;
};
static const CLSID CLSID_PolicyConfig = {0x870af99c, 0x171d, 0x4f9e, {0xaf, 0x0d, 0xe6, 0x3d, 0xf4, 0x0c, 0x2b, 0xc9}};
static const IID IID_PolicyConfig = {0xf8679f50, 0x850a, 0x41cf, {0x9c, 0x72, 0x43, 0x0f, 0x29, 0x02, 0x90, 0xc8}};

static bool setDefaultCapture(const std::wstring &id, ERole role) {
    IPolicyConfig *pc = nullptr;
    if (id.empty() || FAILED(CoCreateInstance(CLSID_PolicyConfig, nullptr, CLSCTX_ALL, IID_PolicyConfig, (void **)&pc))) return false;
    HRESULT hr = pc->SetDefaultEndpoint(id.c_str(), role);
    pc->Release();
    return SUCCEEDED(hr);
}

// ---------- Estado ----------
static std::atomic<bool> g_cableFound{false};
static std::wstring prevConsole, prevComm;   // el micrófono de Windows antes de usar el del visor
bool micCableInstalled() { return g_cableFound; }

static void restoreDefault() {
    if (prevConsole.empty() && prevComm.empty()) return;
    bool ok = setDefaultCapture(prevConsole, eConsole) | setDefaultCapture(prevComm, eCommunications);
    logmsg("[mic] Windows microphone %s", ok ? "is back to the one before" : "couldn't be put back (pick it in Windows' sound settings)");
    prevConsole.clear();
    prevComm.clear();
}
void micOnExit() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    restoreDefault();
}

static void micThread() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    bool sent = false, sentConnected = false;
    uint64_t lastLook = 0, lastStat = nowNs();
    while (true) {
        // VB-CABLE: se busca cada tanto (por si lo instalan con VRmin Host abierto)
        uint64_t now = nowNs();
        if (now - lastLook > 5000000000ull) {
            lastLook = now;
            bool found = !findCable(eRender, L"CABLE Input").empty();
            if (found != g_cableFound) { g_cableFound = found; logmsg("[mic] VB-CABLE %s", found ? "found" : "not installed"); uiRefresh(); }
        }
        bool want = micEnabled() && g_cableFound && questConnected() && gameActive();
        // Al Quest: que grabe solo mientras hace falta (y de nuevo cada vez que se conecta)
        bool conn = questConnected();
        if (conn && (want != sent || !sentConnected)) { questSendText(want ? "{\"type\":\"mic\",\"on\":true}" : "{\"type\":\"mic\",\"on\":false}"); sent = want; }
        sentConnected = conn;
        if (!want) { restoreDefault(); Sleep(500); continue; }

        // El micrófono de Windows pasa a ser VB-CABLE mientras se usa el del visor
        std::wstring out = findCable(eCapture, L"CABLE Output");
        if (prevConsole.empty() && prevComm.empty() && !out.empty()) {
            prevConsole = defaultCapture(eConsole);
            prevComm = defaultCapture(eCommunications);
            if (prevConsole != out || prevComm != out) {
                bool ok = setDefaultCapture(out, eConsole) & setDefaultCapture(out, eCommunications);
                logmsg("[mic] Windows microphone: %s", ok ? "CABLE Output (the headset's microphone)" : "couldn't change it (pick CABLE Output in the game)");
            }
        }

        // Escribir en "CABLE Input"
        std::wstring inId = findCable(eRender, L"CABLE Input");
        IMMDeviceEnumerator *en = nullptr;
        IMMDevice *dev = nullptr;
        IAudioClient *ac = nullptr;
        IAudioRenderClient *rc = nullptr;
        WAVEFORMATEX wf{};
        wf.wFormatTag = WAVE_FORMAT_PCM; wf.nChannels = 1; wf.nSamplesPerSec = 48000; wf.wBitsPerSample = 16;
        wf.nBlockAlign = 2; wf.nAvgBytesPerSec = 96000;
        auto done = [&] {
            if (ac) ac->Stop();
            if (rc) rc->Release();
            if (ac) ac->Release();
            if (dev) dev->Release();
            if (en) en->Release();
            rc = nullptr; ac = nullptr; dev = nullptr; en = nullptr;
        };
        if (inId.empty() || FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en))) ||
            FAILED(en->GetDevice(inId.c_str(), &dev)) || FAILED(dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&ac)) ||
            FAILED(ac->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY, 500000, 0, &wf, nullptr)) ||
            FAILED(ac->GetService(IID_PPV_ARGS(&rc))) || FAILED(ac->Start())) {
            logmsg("[mic] Couldn't open CABLE Input");
            done();
            Sleep(3000);
            continue;
        }
        UINT32 bufFrames = 0;
        ac->GetBufferSize(&bufFrames);
        logmsg("[mic] Headset microphone → CABLE Input (48 kHz, buffer %u frames)", bufFrames);
        { std::lock_guard<std::mutex> g(ringLock); rd = wr = count = 0; primed = false; }
        // (se mantienen unos 30 ms escritos por delante: poca demora, sin cortes)
        const UINT32 ahead = 1440;
        while (micEnabled() && questConnected() && gameActive()) {
            Sleep(5);
            UINT32 pad = 0;
            if (FAILED(ac->GetCurrentPadding(&pad))) break;
            if (pad >= ahead) continue;
            UINT32 n = std::min(ahead - pad, bufFrames - pad);
            BYTE *data = nullptr;
            if (FAILED(rc->GetBuffer(n, &data))) break;
            micPull((int16_t *)data, n);
            rc->ReleaseBuffer(n, 0);
            uint64_t t = nowNs();
            if (t - lastStat > 5000000000ull) {
                lastStat = t;
                std::lock_guard<std::mutex> g(ringLock);
                if (cuts || droppedMs) logmsg("[mic] last 5 s: %d gaps with no audio, %d ms dropped for running late, buffer %d ms", cuts, droppedMs, (int)(count / 48));
                cuts = 0; droppedMs = 0;
            }
        }
        done();
    }
}

void micStart() { std::thread(micThread).detach(); }
