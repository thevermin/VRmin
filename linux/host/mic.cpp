// Micrófono del visor en los juegos: el Quest manda lo que graba (paquete 0x0B: 48 kHz, mono, 16 bits) y VRmin Host lo
// pasa a un micrófono virtual, «VRmin headset microphone»: un parlante virtual (vrmin_mic_in, no suena en ningún lado)
// y su «monitor» presentado como micrófono (vrmin_mic). Mientras hay un Quest conectado es el micrófono de Linux, así los
// juegos lo toman; al desconectarlo o al cerrar VRmin Host vuelve el de antes.
#include "host.h"
#include <pulse/simple.h>
#include <pulse/error.h>
#include <thread>

bool paEnsure(const char *module, const std::string &devArg, const std::string &more);
bool paTakeDefault(bool source, const std::string &ours);
void paGiveBackDefault(bool source);

// ---------- Colchón: lo que llega del Quest (a tirones, por Wi-Fi) y lo que pide el servidor de sonido (parejo) ----------
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

/// Los dos dispositivos virtuales del micrófono, y que sea el de Linux
static bool micDevices() {
    return paEnsure("module-null-sink", "sink_name=vrmin_mic_in",
                    "rate=48000 channels=1 channel_map=mono sink_properties='device.description=\"VRmin headset microphone (input)\"'") &&
           paEnsure("module-remap-source", "source_name=vrmin_mic",
                    "master=vrmin_mic_in.monitor channels=1 channel_map=mono source_properties='device.description=\"VRmin headset microphone\"'") &&
           paTakeDefault(true, "vrmin_mic");
}

static void micThread() {
    bool sent = false, sentConnected = false, ready = false, warned = false;
    uint64_t lastStat = nowNs();
    while (true) {
        bool conn = questConnected(), want = conn && gameActive();
        // Al Quest: que grabe solo mientras se juega (y de nuevo cada vez que se conecta)
        if (conn && (want != sent || !sentConnected)) { questSendText(want ? "{\"type\":\"mic\",\"on\":true}" : "{\"type\":\"mic\",\"on\":false}"); sent = want; }
        sentConnected = conn;
        if (!conn && ready) { paGiveBackDefault(true); ready = false; }
        if (conn && !ready) {
            ready = micDevices();
            if (!ready && !warned) logmsg("[mic] Couldn't create the virtual microphone (is PulseAudio or PipeWire running?)");
            warned = !ready;
            if (!ready) { sleepMs(3000); continue; }
        }
        if (!want) { sleepMs(500); continue; }

        // Escribir en vrmin_mic_in (se mantienen unos 50 ms escritos por delante: con PipeWire, 30 ms se cortan)
        pa_sample_spec ss{PA_SAMPLE_S16LE, 48000, 1};
        pa_buffer_attr ba{(uint32_t)-1, 2400 * 2, (uint32_t)-1, 480 * 2, (uint32_t)-1};
        int err = 0;
        pa_simple *s = pa_simple_new(nullptr, "VRmin Host", PA_STREAM_PLAYBACK, "vrmin_mic_in", "Headset microphone", &ss, nullptr, &ba, &err);
        if (!s) {
            logmsg("[mic] Couldn't open the virtual microphone (%s)", pa_strerror(err));
            ready = false;
            sleepMs(3000);
            continue;
        }
        logmsg("[mic] Headset microphone → VRmin headset microphone (48 kHz)");
        { std::lock_guard<std::mutex> g(ringLock); rd = wr = count = 0; primed = false; }
        size_t minCount = ring.size();
        uint64_t minSince = nowNs();
        int16_t buf[480];   // 10 ms
        while (questConnected() && gameActive()) {
            micPull(buf, 480);
            if (pa_simple_write(s, buf, sizeof buf, &err) < 0) break;   // (espera a que haya lugar: va al ritmo del servidor)
            uint64_t t = nowNs();
            {
                // El servidor pide a tirones (y nada mientras nadie usa el micrófono): si en todo un segundo el colchón
                // no bajó de 80 ms, se vuelve a 60 ms (si no, esa demora queda para siempre)
                std::lock_guard<std::mutex> g(ringLock);
                minCount = std::min(minCount, count);
                if (t - minSince > 1000000000ull) {
                    if (minCount > 3840) { size_t d = minCount - 2880; rd = (rd + d) % ring.size(); count -= d; droppedMs += (int)d / 48; }
                    minCount = ring.size(); minSince = t;
                }
            }
            if (t - lastStat > 5000000000ull) {
                lastStat = t;
                std::lock_guard<std::mutex> g(ringLock);
                if (cuts || droppedMs) logmsg("[mic] last 5 s: %d gaps with no audio, %d ms dropped for running late, buffer %d ms", cuts, droppedMs, (int)(count / 48));
                cuts = 0; droppedMs = 0;
            }
        }
        pa_simple_free(s);
    }
}

void micStart() { std::thread(micThread).detach(); }
