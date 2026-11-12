// Sonido de la PC al Quest, con el servidor de sonido del usuario por libpulse (PulseAudio, o PipeWire por pipewire-pulse)
// Paquete 2: [2,0,0,0][Hz u32][muestras de 16 bits, estéreo intercalado], en bloques de 20 ms (como en Windows)
//  - "quest": mientras se juega, la salida de Linux pasa a ser «VRmin headset sound» (un parlante virtual que no suena
//    en la PC) y se manda lo que suena ahí; al terminar, vuelve la de antes
//  - "both": se manda lo que suena en la salida de siempre (su «monitor»), que sigue sonando en la PC
//  - "mac" (acá: la PC): no se manda nada
// Acá también está lo que comparte con el micrófono (mic.cpp): crear los dispositivos virtuales y elegir los de Linux.
#include "host.h"
#include <pulse/pulseaudio.h>
#include <pulse/simple.h>
#include <thread>

// ---------- Control del servidor de sonido (una conexión corta por vez) ----------
static std::mutex paLock;
static bool paClosed = false;               // (VRmin Host se está cerrando: ya no se cambia nada)
static std::string prevDef[2];              // [0] la salida, [1] el micrófono de Linux de antes de los de VRmin
static bool taken[2] = {false, false};

typedef std::vector<std::pair<uint32_t, std::string>> Modules;   // número y argumentos de cada módulo cargado
struct DefaultQuery { bool source; std::string name; };

struct PaCtl {
    pa_mainloop *ml = pa_mainloop_new();
    pa_context *c = nullptr;
    bool ok = false;
    PaCtl() {
        c = pa_context_new(pa_mainloop_get_api(ml), "VRmin Host");
        if (!c || pa_context_connect(c, nullptr, PA_CONTEXT_NOAUTOSPAWN, nullptr) < 0) return;
        while (true) {
            pa_context_state_t s = pa_context_get_state(c);
            if (s == PA_CONTEXT_READY) { ok = true; return; }
            if (!PA_CONTEXT_IS_GOOD(s) || pa_mainloop_iterate(ml, 1, nullptr) < 0) return;
        }
    }
    ~PaCtl() {
        if (c) { pa_context_disconnect(c); pa_context_unref(c); }
        pa_mainloop_free(ml);
    }
    void wait(pa_operation *o) {
        if (!o) return;
        while (pa_operation_get_state(o) == PA_OPERATION_RUNNING && pa_mainloop_iterate(ml, 1, nullptr) >= 0) {}
        pa_operation_unref(o);
    }
    std::string defaultName(bool source) {
        DefaultQuery q{source, ""};
        wait(pa_context_get_server_info(c, [](pa_context *, const pa_server_info *i, void *u) {
            DefaultQuery *q = (DefaultQuery *)u;
            const char *n = i ? (q->source ? i->default_source_name : i->default_sink_name) : nullptr;
            if (n) q->name = n;
        }, &q));
        return q.name;
    }
    bool setDefault(bool source, const std::string &name) {
        int r = 0;
        auto cb = [](pa_context *, int ok, void *u) { *(int *)u = ok; };
        wait(source ? pa_context_set_default_source(c, name.c_str(), cb, &r) : pa_context_set_default_sink(c, name.c_str(), cb, &r));
        return r != 0;
    }
    Modules modules() {
        Modules all;
        wait(pa_context_get_module_info_list(c, [](pa_context *, const pa_module_info *i, int eol, void *u) {
            if (eol == 0 && i) ((Modules *)u)->push_back({i->index, i->argument ? i->argument : ""});
        }, &all));
        return all;
    }
    bool load(const char *module, const std::string &args) {
        uint32_t idx = PA_INVALID_INDEX;
        wait(pa_context_load_module(c, module, args.c_str(), [](pa_context *, uint32_t i, void *u) { *(uint32_t *)u = i; }, &idx));
        return idx != PA_INVALID_INDEX;
    }
    /// Sacar los módulos que cargó VRmin (los que crean dispositivos «vrmin_…»), del último al primero
    int unloadOurs() {
        Modules all = modules();
        int n = 0;
        for (auto it = all.rbegin(); it != all.rend(); ++it)
            if (it->second.find("sink_name=vrmin_") != std::string::npos || it->second.find("source_name=vrmin_") != std::string::npos) {
                wait(pa_context_unload_module(c, it->first, [](pa_context *, int, void *) {}, nullptr));
                n++;
            }
        return n;
    }
};

/// Que exista el dispositivo virtual de VRmin que crea ese módulo (si ya está, no se carga de nuevo)
bool paEnsure(const char *module, const std::string &devArg, const std::string &more) {
    std::lock_guard<std::mutex> g(paLock);
    if (paClosed) return false;
    PaCtl p;
    if (!p.ok) return false;
    for (auto &m : p.modules()) if (m.second.find(devArg) != std::string::npos) return true;
    bool ok = p.load(module, devArg + " " + more);
    if (ok) logmsg("[audio] Created the virtual device %s", devArg.c_str());
    return ok;
}

/// La salida (o el micrófono) de Linux pasa a ser la de VRmin; la de antes queda guardada
bool paTakeDefault(bool source, const std::string &ours) {
    std::lock_guard<std::mutex> g(paLock);
    if (paClosed) return false;
    if (taken[source]) return true;
    PaCtl p;
    if (!p.ok) return false;
    std::string cur = p.defaultName(source);
    if (cur != ours) prevDef[source] = cur;   // (si quedó la de VRmin de una vez anterior, no se guarda esa)
    if (!p.setDefault(source, ours)) return false;
    taken[source] = true;
    logmsg("[audio] Linux %s: %s (was %s)", source ? "microphone" : "sound output", ours.c_str(), prevDef[source].empty() ? "?" : prevDef[source].c_str());
    return true;
}

static void giveBack(PaCtl &p, bool source) {
    // (si el usuario eligió otra mientras tanto, queda la que eligió)
    std::string cur = p.defaultName(source);
    bool ours = cur == (source ? "vrmin_mic" : "vrmin_sound") && !prevDef[source].empty();
    if (ours) p.setDefault(source, prevDef[source]);
    logmsg("[audio] Linux %s is back to %s", source ? "microphone" : "sound output", (ours ? prevDef[source] : cur).c_str());
    taken[source] = false;
}
void paGiveBackDefault(bool source) {
    std::lock_guard<std::mutex> g(paLock);
    if (!taken[source] || paClosed) return;
    PaCtl p;
    if (p.ok) giveBack(p, source);
}

std::string paDefaultName(bool source) {
    std::lock_guard<std::mutex> g(paLock);
    PaCtl p;
    return p.ok ? p.defaultName(source) : std::string();
}

/// Al cerrar VRmin Host: vuelven la salida y el micrófono de antes y se sacan los dispositivos virtuales
void audioOnExit() {
    std::lock_guard<std::mutex> g(paLock);
    paClosed = true;
    PaCtl p;
    if (!p.ok) return;
    for (int s = 0; s < 2; s++) if (taken[s]) giveBack(p, s);
    if (p.unloadOurs() > 0) logmsg("[audio] Removed VRmin's virtual sound devices");
}

// ---------- El sonido ----------
/// Qué se hace ahora con el sonido de la PC: "" (nada), "quest" (solo en el visor) o "both" (también en la PC)
static std::string soundMode() {
    if (!questConnected() || !gameActive()) return "";
    std::lock_guard<std::mutex> g(g_setLock);
    return g_set.audio == "quest" || g_set.audio == "both" ? g_set.audio : "";
}

static void audioThread() {
    bool warned = false;
    const int rate = 48000;
    const size_t chunk = (size_t)(rate / 50) * 2;   // 20 ms, 2 canales
    while (true) {
        std::string mode = soundMode();
        if (mode != "quest") paGiveBackDefault(false);
        if (mode.empty()) { sleepMs(100); continue; }
        std::string dev;
        if (mode == "quest") {
            if (paEnsure("module-null-sink", "sink_name=vrmin_sound", "rate=48000 channels=2 sink_properties='device.description=\"VRmin headset sound\"'") &&
                paTakeDefault(false, "vrmin_sound"))
                dev = "vrmin_sound.monitor";
        } else {
            dev = paDefaultName(false);
            if (!dev.empty()) dev += ".monitor";
        }
        pa_sample_spec ss{PA_SAMPLE_S16LE, (uint32_t)rate, 2};
        pa_buffer_attr ba{(uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)-1, (uint32_t)(chunk * 2)};
        int err = 0;
        pa_simple *s = dev.empty() ? nullptr : pa_simple_new(nullptr, "VRmin Host", PA_STREAM_RECORD, dev.c_str(), "Sound to the headset", &ss, nullptr, &ba, &err);
        if (!s) {
            if (!warned) logmsg("[audio] Couldn't capture the PC sound (%s)", err ? pa_strerror(err) : "is PulseAudio or PipeWire running?");
            warned = true;
            sleepMs(3000);
            continue;
        }
        warned = false;
        logmsg("[audio] Sending the PC sound to the Quest (%d Hz, 2 channels, from %s%s)", rate, dev.c_str(), mode == "both" ? ", also on the PC" : "");
        while (soundMode() == mode) {
            std::vector<uint8_t> p(8 + chunk * 2);
            if (pa_simple_read(s, p.data() + 8, chunk * 2, &err) < 0) break;
            p[0] = 2;
            p[4] = (uint8_t)(rate >> 24); p[5] = (uint8_t)(rate >> 16); p[6] = (uint8_t)(rate >> 8); p[7] = (uint8_t)rate;
            questSend(std::move(p));
        }
        pa_simple_free(s);
    }
}

void audioStart() {
    // (si VRmin Host no se cerró bien la vez anterior, pueden haber quedado sus dispositivos virtuales)
    {
        std::lock_guard<std::mutex> g(paLock);
        PaCtl p;
        if (p.ok && p.unloadOurs() > 0) logmsg("[audio] Removed VRmin's virtual sound devices left from before");
    }
    std::thread(audioThread).detach();
}
