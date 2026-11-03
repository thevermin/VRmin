// VRmin Host para Windows: lo que comparten las partes del programa
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <vector>
#include "json.h"

#define VRMIN_WIN_VERSION "0.1"

// ---------- Textos y registro ----------
extern bool IDIOMA_ES;
inline const char *tr(const char *es, const char *en) { return IDIOMA_ES ? es : en; }
void logmsg(const char *fmt, ...);
std::string utf8(const std::wstring &w);
std::wstring wide(const std::string &s);
std::wstring dataDir();                 // %LOCALAPPDATA%\VRmin
std::wstring exeDir();                  // carpeta de VRmin Host.exe
std::string readFile(const std::wstring &path);
bool writeFile(const std::wstring &path, const std::string &data);
uint64_t nowNs();

// ---------- Ajustes (los elige el Quest, quedan en ajustes.json) ----------
struct Settings {
    int eye = 1536, fps = 72, mbps = 40;
    bool hevc = true;
    std::string fov = "0";              // foveated encoding: "nivel,centro,cx izq,cx der,cy"
    std::string audio = "quest";        // juegos: "quest", "both" o "mac" (acá: la PC)
    int deskMaxW = 2560, deskFps = 60, deskMbps = 20;
    std::string deskAudio = "mac";
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
extern std::atomic<int> g_pairCode;             // código para emparejar que se muestra en la ventana (-1: ninguno)
extern std::atomic<bool> g_questWifi;
extern std::string g_questName;

// ---------- Conexión con el juego (el runtime de OpenXR) ----------
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
std::string profilesCurrent();
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
    ~VideoEncoder();
    // nv12: w*h + w*h/2 bytes. tag: lo que vuelve con el cuadro codificado. No bloquea: si está ocupado, descarta.
    bool encode(const uint8_t *nv12, int w, int h, int fps, int bitrate, bool hevc, bool forceKey, uint64_t tag);
    void requestKey() { key_ = true; }
    void setBitrate(int b);
    bool busy() const { return pending_.load() >= 2; }
    bool usingHevc() const { return hevcNow_; }
    void reset();
    struct Impl;
private:
    Impl *impl_ = nullptr;
    std::mutex lock_;
    std::string name_;
    Out out_;
    std::atomic<bool> key_{true};
    std::atomic<int> pending_{0};
    bool hevcNow_ = false;
    friend struct EncWorker;
};

bool encoderCanHevc(int w, int h, int fps, int bitrate);

// Conversión e imágenes (en el procesador, en varios hilos)
void parallelRows(int rows, const std::function<void(int y0, int y1)> &fn);
void bgraToNv12(const uint8_t *bgra, int stride, int w, int h, uint8_t *nv12, bool rgba = false);
// Del cuadro del juego (dos ojos lado a lado, ew×eh cada uno) a la imagen que se codifica (W×H, BGRA), con o sin foveated
void warpEyes(const uint8_t *src, int ew, int eh, bool rgba, const Stream &st, std::vector<uint8_t> &out);

// ---------- Escritorio, mouse y teclado ----------
void deskStart();
void deskSetOn(bool on);
bool deskOn();
void deskRequestKey();
std::string deskError();
void deskMouse(float u, float v, const Json &down, float scroll);
void typeKeys(const Json &m);
bool typingActive();

// ---------- Sonido ----------
void audioStart();

// ---------- Juegos de Steam ----------
struct GameEntry { std::string id, name, dir; bool vr = true; };
void gamesScan();
std::vector<GameEntry> gamesList();
void gamesLaunch(const std::string &id);
void gamesQuit();
std::string gamesRunningName();
Json gamesLaunchingInfo(bool active);
std::string gamesStatus();
bool gamesBusy();

// ---------- Preparar Windows: runtime de OpenXR y OpenComposite ----------
struct SetupState { bool runtimeOk = false; bool ocOk = false; std::string runtimeNow; std::string msg; };
SetupState setupCheck();
bool setupActivate();          // pide permiso de administrador una vez (OpenXR) y activa OpenComposite
bool setupRestoreSteamVR();    // deja SteamVR como estaba
void setupOnExit();

// ---------- Ventana ----------
void uiRefresh();
