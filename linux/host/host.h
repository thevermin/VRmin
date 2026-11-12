// VRmin Host para Linux: lo que comparten las partes del programa (las mismas que en el de Windows, sin lo de Windows)
#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "json.h"

#define VRMIN_LINUX_VERSION "0.1"

// ---------- Textos, registro, archivos ----------
extern bool IDIOMA_ES;
inline const char *tr(const char *es, const char *en) { return IDIOMA_ES ? es : en; }
void logmsg(const char *fmt, ...);
std::string dataDir();                  // ~/.local/share/VRmin
std::string readFile(const std::string &path);
bool writeFile(const std::string &path, const std::string &data);
uint64_t nowNs();
void sleepMs(int ms);

// ---------- Ajustes (los elige el Quest, quedan en settings.json) ----------
struct Settings {
    int eye = 1536, fps = 72, mbps = 40;
    bool hevc = true;
    std::string fov = "0";              // foveated encoding: "nivel,centro,cx izq,cx der,cy"
    std::string audio = "quest";        // juegos: "quest", "both" o "mac" (acá: la PC)
    int deskMaxW = 2560, deskFps = 60, deskMbps = 20;
    std::string deskAudio = "mac";
    std::string lang;                   // "es", "en" o "" (el del sistema)
};
extern Settings g_set;
extern std::mutex g_setLock;
void loadSettings();
void saveSettings();

// ---------- Foveated encoding (la misma cuenta que la Mac y el Quest) ----------
struct Fovea {
    int level = 0, size = 1;
    double cx[2] = {0.5, 0.5}, cy = 0.5;
    explicit Fovea(const std::string &s);
    bool on() const { return level > 0; }
    std::string text() const;
    void params(double &m, double &r0, double &r1) const;
    std::vector<double> cumulative(double c) const;
    double ratio(double c) const;
    std::vector<float> forward(double c) const;   // del codificado (0..1) a la imagen del juego (0..1), 1024 valores
};

// Lo que corre ahora (se recalcula al cambiar los ajustes)
struct Stream {
    int eye = 1536, fps = 72, bitrate = 40000000;
    bool hevc = true;
    Fovea fov{"0"};
    int W = 3072, H = 1536;             // lo que se codifica y se manda
};
extern Stream g_run;
extern std::mutex g_runLock;
Stream currentStream();
void applySettings();                   // recalcula g_run, rearma el codificador y avisa al Quest y al juego

// ---------- Conexión con el Quest ----------
void questStart();
bool questConnected();
void questSend(std::vector<uint8_t> &&p);       // [tipo][...] (el largo lo agrega)
void questSendText(const std::string &json);
void questRequestKey();                         // pedir un cuadro completo del juego
extern std::atomic<int> g_pairCode;             // código para emparejar (-1: ninguno)
extern std::atomic<bool> g_questWifi;
extern std::string g_questName;

// ---------- Conexión con el juego (el runtime de OpenXR, dentro de Wine o Proton) ----------
void gameStart();
void gameSendLine(const std::string &line);
bool gameActive();                               // llegó un cuadro del juego hace menos de medio segundo
void gameConfigChanged();                        // mandar la línea config nueva al juego
void gameQualityChanged();                       // rearmar lo que procesa los cuadros
extern std::atomic<bool> g_halfRate;

// ---------- Perfiles de juegos (Ajustes › Avanzado del Quest) ----------
std::string profilesConfigLine(int fps, bool hello);
void profilesBegin(const std::string &app);
void profilesEnd();
Json profilesState();
void profilesApply(const Json &m);
void perfSetRuntime(const std::string &json);
void perfAddMac(double ms);
void perfAddEnc(double ms);
void perfAddDrop();
Json perfSnapshot();

// ---------- Video ----------
class VideoEncoder {
public:
    using Out = std::function<void(const uint8_t *data, size_t n, bool key, uint64_t tag)>;
    VideoEncoder(const char *name, Out out) : name_(name), out_(std::move(out)) {}
    // nv12: w*h + w*h/2 bytes. tag: lo que vuelve con el cuadro codificado. No bloquea: si está ocupado, descarta.
    bool encode(const uint8_t *nv12, int w, int h, int fps, int bitrate, bool hevc, bool forceKey, uint64_t tag);
    void requestKey() { key_ = true; }
    bool busy() const { return pending_.load() >= 2; }
    void reset();
private:
    std::string name_;
    Out out_;
    std::atomic<bool> key_{true};
    std::atomic<int> pending_{0};
    friend struct EncWorker;
};
bool encoderCanHevc(int w, int h, int fps, int bitrate);

// Conversión e imágenes (en el procesador, en varios hilos)
void parallelRows(int rows, const std::function<void(int y0, int y1)> &fn);
void bgraToNv12(const uint8_t *bgra, int stride, int w, int h, uint8_t *nv12, bool rgba = false);
void warpEyes(const uint8_t *src, int ew, int eh, bool rgba, const Stream &st, std::vector<uint8_t> &out);

// ---------- Escritorio, mouse y teclado (desktop.cpp, con X11) ----------
void deskStart();
void deskSetOn(bool on);
bool deskOn();
void deskRequestKey();
std::string deskError();
void deskMouse(float u, float v, const Json &down, float scroll);
struct ScreenRect { int left = 0, top = 0, right = 0, bottom = 0; };   // (como el RECT de Windows: coordenadas de la pantalla)
struct DisplayInfo { uint32_t id; std::string name; ScreenRect r; bool primary; };
/// Una captura de monitor: la pantalla principal del Quest (main) o un monitor abierto como ventana (sid)
struct DisplayCapture {
    bool main = false;
    uint32_t sid = 0;
    VideoEncoder *enc = nullptr;
    std::function<bool()> want;          // mandar ahora
    std::function<bool()> stop;          // terminar (sin esto, espera a que want vuelva a ser sí)
    std::function<uint32_t()> target;    // el monitor (0: el principal)
};
void displayCaptureLoop(const DisplayCapture &c);
void mouseAt(const ScreenRect &r, float u, float v, const Json &down, float scroll);   // el mouse del Quest en ese rectángulo de la pantalla
std::vector<DisplayInfo> displaysList();      // los monitores de la PC
uint32_t deskDisplayId();                     // el que se ve en la pantalla principal del Quest
void deskSetDisplay(uint32_t id);
void typeKeys(const Json &m);
bool typingActive();

// ---------- Ventanas sueltas (windows.cpp): cada ventana o monitor de la PC en su propia pantalla del Quest ----------
Json windowsListJson();                                  // [{id, app, title, w, h}]
void winOpen(uint32_t sid, bool on);
void winMouse(uint32_t sid, float u, float v, const Json &down, float scroll);
void winResize(uint32_t sid, int qw, int qh);
void winSwap(uint32_t sid, uint32_t to);

// ---------- Juegos de Steam ----------
struct GameEntry { std::string id, name, dir; bool vr = true; };
std::vector<GameEntry> gamesList();
void gamesLaunch(const std::string &id);
void gamesQuit();
std::string gamesRunningName();

// ---------- Micrófono del visor ----------
void micPush(const uint8_t *pcm, size_t n);   // lo que grabó el Quest (48 kHz, mono, 16 bits)

// ---------- Estado para el Quest ----------
void lobbyStart();
