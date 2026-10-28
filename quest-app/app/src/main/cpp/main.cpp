// QuestVRBridge · cliente nativo para Meta Quest (OpenXR + MediaCodec + AAudio)
//
// - Encuentra la Mac sola (aviso UDP en el puerto 9101) y se conecta por TCP (9100).
// - Cada cuadro manda la pose de la cabeza, las manos y los botones (mismo formato que la página web).
// - Decodifica el video H.264 con el chip del Quest y lo muestra con la pose para la que la Mac lo dibujó:
//   el compositor del Quest corrige solo el movimiento de la cabeza (timewarp).
// - Reproduce el sonido del juego.
// - En la sala, la lista de juegos y el escritorio de la Mac llegan aparte y se muestran como capas
//   propias del Quest (a resolución completa, nítidas para leer); el juego usa su propia resolución.

#include <android_native_app_glue.h>
#include <android/log.h>
#include <android/native_window.h>
#include <android/native_window_jni.h>
#include <android/hardware_buffer.h>
#include <media/NdkMediaCodec.h>
#include <media/NdkMediaFormat.h>
#include <media/NdkImageReader.h>
#include <aaudio/AAudio.h>
#include <cstdint>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#include <jni.h>
#include <zlib.h>

#define XR_USE_PLATFORM_ANDROID
#define XR_USE_GRAPHICS_API_OPENGL_ES
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <arpa/inet.h>
#include <android/multinetwork.h>
#include <net/if.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>

#include <atomic>
#include <map>
#include <cerrno>
#include <cstdarg>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#define LOG_TAG "QVB"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#include "cam.h"

static const int NATIVE_PORT = 9100;
static const int BEACON_PORT = 9101;

static double nowMs() {
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Estado compartido
// ---------------------------------------------------------------------------
struct EyeSnap { XrPosef pose; XrFovf fov; };
struct PoseSnap { uint32_t id = 0; EyeSnap eye[2]; double at = 0; };
// Indicador de rendimiento
static std::atomic<uint64_t> g_bytesIn{0};       // todo lo que llega de la Mac
static std::atomic<uint32_t> g_rendered{0};      // cuadros dibujados en el visor
static std::atomic<float> g_latency{0};          // desde que se mandó la pose hasta que su imagen se muestra (juegos)

static const int HIST = 1024;
static PoseSnap g_hist[HIST];
static std::mutex g_histLock;

static std::atomic<int> g_sock{-1};
static std::mutex g_sendLock;
static std::atomic<bool> g_running{true};
static std::atomic<int> g_targetFps{72};
static std::atomic<bool> g_needKey{true};
static std::atomic<uint32_t> g_framesIn{0}, g_framesDecoded{0}, g_framesDropped{0}, g_audioChunks{0};
static std::string g_serverName;

// ---------------------------------------------------------------------------
// Red
// ---------------------------------------------------------------------------
static bool sendPacket(const std::string &s) {
    int fd = g_sock.load();
    if (fd < 0) return false;
    std::lock_guard<std::mutex> lk(g_sendLock);
    uint32_t len = htonl((uint32_t)s.size());
    std::string buf((const char *)&len, 4);
    buf += s;
    size_t off = 0;
    double t0 = nowMs();
    while (off < buf.size()) {
        ssize_t n = send(fd, buf.data() + off, buf.size() - off, MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n > 0) { off += (size_t)n; continue; }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            if (off == 0) return false;               // nada enviado: se descarta esta pose
            if (nowMs() - t0 > 20) return false;      // no dejar un mensaje a medias mucho tiempo
            usleep(200);
            continue;
        }
        return false;
    }
    return true;
}

static void requestKey() {
    g_needKey = true;
    sendPacket("{\"type\":\"key\"}");
}

static void sendLog(const char *fmt, ...) {
    char msg[512];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    LOGI("%s", msg);
    std::string s = "{\"type\":\"log\",\"msg\":\"";
    for (const char *p = msg; *p; p++) { if (*p == '"' || *p == '\\') s += '\\'; s += *p; }
    s += "\"}";
    sendPacket(s);
}

/// Espera el aviso de la Mac en la red local. Devuelve la IP (y el puerto) o vacío.
// Macs que se anuncian en la red; la sala VR se conecta solo a la que elegiste en «Computadoras»
struct MacSeen { std::string ip; int port = NATIVE_PORT; bool idle = false; double seen = 0; bool usb = false; };
// Cable USB-C (Horizon OS 2.5+): la red que pidió la parte Java (0 = no hay) y su interfaz (por ejemplo ncm0)
static std::atomic<uint64_t> g_usbNet{0};
static std::mutex g_usbLock;
static std::string g_usbIf;
// Direcciones de la Mac en el cable (fe80::…, sin la interfaz): las manda la Mac en su aviso por Wi-Fi y en el estado.
// Sirven para conectarse por el cable aunque la pregunta a todos (ff02::1) no tenga respuesta.
static std::mutex g_llLock;
static std::vector<std::string> g_macLL;
static std::string g_cableIp;                   // dirección por el cable que anduvo (se reusa al reconectar)
static std::atomic<bool> g_cableOnly{false};    // ajuste: solo por cable
static void parseLL(const char *js) {
    const char *p = strstr(js, "\"ll\":[");
    if (!p) return;
    p += 6;
    std::vector<std::string> v;
    while (*p && *p != ']') {
        const char *q = strchr(p, '"'); if (!q) break;
        const char *e = strchr(q + 1, '"'); if (!e) break;
        v.emplace_back(q + 1, e - q - 1);
        p = e + 1;
    }
    std::lock_guard<std::mutex> lk(g_llLock);
    g_macLL = v;
}
static std::mutex g_macLock;
static std::map<std::string, MacSeen> g_macs;
static std::string g_target;          // nombre de la Mac elegida ("" = ninguna)
static void javaMacs(const std::string &json);

/// Dirección «fe80::…%ncm0» (por el cable) → sockaddr_in6 con su interfaz
static bool v6addr(const std::string &ip, int port, sockaddr_in6 &a) {
    a = sockaddr_in6{}; a.sin6_family = AF_INET6; a.sin6_port = htons(port);
    std::string host = ip, scope;
    size_t pc = ip.find('%');
    if (pc != std::string::npos) { host = ip.substr(0, pc); scope = ip.substr(pc + 1); }
    if (inet_pton(AF_INET6, host.c_str(), &a.sin6_addr) != 1) return false;
    if (!scope.empty()) a.sin6_scope_id = if_nametoindex(scope.c_str());
    return true;
}

static void askStart(const std::string &ip) {
    if (ip.find(':') != std::string::npos) {   // por el cable USB
        int u = socket(AF_INET6, SOCK_DGRAM, 0);
        if (u < 0) return;
        uint64_t nh = g_usbNet.load();
        if (nh) android_setsocknetwork((net_handle_t)nh, u);
        sockaddr_in6 a;
        const char *m = "{\"qvb\":\"start\"}";
        if (v6addr(ip, 9102, a)) sendto(u, m, strlen(m), 0, (sockaddr *)&a, sizeof a);
        close(u);
        return;
    }
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    if (u < 0) return;
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(9102);
    inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
    const char *m = "{\"qvb\":\"start\"}";
    sendto(u, m, strlen(m), 0, (sockaddr *)&a, sizeof a);
    close(u);
}

static std::string discoverMac(int &port) {
    int u = socket(AF_INET, SOCK_DGRAM, 0);
    if (u < 0) return "";
    int one = 1;
    setsockopt(u, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(u, SOL_SOCKET, SO_BROADCAST, &one, sizeof one);
    sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(BEACON_PORT); a.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(u, (sockaddr *)&a, sizeof a) != 0) { close(u); sleep(1); return ""; }
    timeval tv{1, 0};
    setsockopt(u, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    char buf[512];
    double until = nowMs() + 1200;
    while (nowMs() < until) {
        sockaddr_in from{}; socklen_t fl = sizeof from;
        ssize_t n = recvfrom(u, buf, sizeof buf - 1, 0, (sockaddr *)&from, &fl);
        if (n <= 0) break;
        buf[n] = 0;
        if (!strstr(buf, "\"qvb\"")) continue;
        parseLL(buf);
        MacSeen m;
        const char *pp = strstr(buf, "\"port\":");
        m.port = pp ? atoi(pp + 7) : NATIVE_PORT;
        if (m.port <= 0) m.port = NATIVE_PORT;
        m.idle = strstr(buf, "\"idle\":true") != nullptr;
        std::string name = "Mac";
        const char *np = strstr(buf, "\"name\": \"");
        if (np) { np += 9; const char *e = strchr(np, '"'); if (e) name.assign(np, e - np); }
        char ip[64];
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        m.ip = ip; m.seen = nowMs();
        std::lock_guard<std::mutex> lk(g_macLock);
        auto it = g_macs.find(name);
        // si hace poco se vio el servidor encendido, no pisarlo con el aviso de "apagado" de la app
        if (m.idle && it != g_macs.end() && !it->second.idle && m.seen - it->second.seen < 2500) it->second.seen = m.seen;
        else if (it != g_macs.end() && it->second.usb && m.seen - it->second.seen < 4000) { }   // por cable gana al Wi-Fi
        else g_macs[name] = m;
    }
    close(u);
    // Por el cable USB-C: preguntar a todos (ff02::1) en el puerto 9103; la app de la Mac contesta con su nombre
    uint64_t nh = g_usbNet.load();
    std::string ifn;
    { std::lock_guard<std::mutex> lk(g_usbLock); ifn = g_usbIf; }
    if (nh && !ifn.empty()) {
        int v = socket(AF_INET6, SOCK_DGRAM, 0);
        if (v >= 0 && android_setsocknetwork((net_handle_t)nh, v) == 0) {
            timeval tv6{0, 150000};
            setsockopt(v, SOL_SOCKET, SO_RCVTIMEO, &tv6, sizeof tv6);
            sockaddr_in6 all{}; all.sin6_family = AF_INET6; all.sin6_port = htons(9103);
            inet_pton(AF_INET6, "ff02::1", &all.sin6_addr);
            all.sin6_scope_id = if_nametoindex(ifn.c_str());
            const char *q = "{\"qvb\":\"probe\"}";
            sendto(v, q, strlen(q), 0, (sockaddr *)&all, sizeof all);
            double until6 = nowMs() + 350;
            while (nowMs() < until6) {
                sockaddr_in6 from{}; socklen_t fl = sizeof from;
                ssize_t n = recvfrom(v, buf, sizeof buf - 1, 0, (sockaddr *)&from, &fl);
                if (n <= 0) continue;
                buf[n] = 0;
                if (!strstr(buf, "\"qvb\"")) continue;
                MacSeen m;
                const char *pp = strstr(buf, "\"port\":");
                m.port = pp ? atoi(pp + 7) : NATIVE_PORT;
                if (m.port <= 0) m.port = NATIVE_PORT;
                m.idle = strstr(buf, "\"idle\":true") != nullptr;
                std::string name = "Mac";
                const char *np = strstr(buf, "\"name\": \"");
                if (np) { np += 9; const char *e = strchr(np, '"'); if (e) name.assign(np, e - np); }
                char ip[80];
                inet_ntop(AF_INET6, &from.sin6_addr, ip, sizeof ip);
                m.ip = std::string(ip) + "%" + ifn; m.seen = nowMs(); m.usb = true;
                std::lock_guard<std::mutex> lk(g_macLock);
                g_macs[name] = m;
            }
        }
        if (v >= 0) close(v);
    }
    // Avisarle a la interfaz qué Macs hay, y decidir si conectar
    std::string json = "[", ip;
    {
        std::lock_guard<std::mutex> lk(g_macLock);
        double now = nowMs();
        for (auto it = g_macs.begin(); it != g_macs.end();) { if (now - it->second.seen > 6000) it = g_macs.erase(it); else ++it; }
        for (auto &kv : g_macs) {
            if (json.size() > 1) json += ",";
            json += "{\"name\":\"" + kv.first + "\",\"ip\":\"" + kv.second.ip + "\",\"idle\":" + (kv.second.idle ? "true" : "false")
                  + ",\"usb\":" + (kv.second.usb ? "true" : "false") + "}";
        }
        auto t = g_macs.find(g_target);
        if (!g_target.empty() && t != g_macs.end()) {
            if (t->second.idle) askStart(t->second.ip);   // la app de la Mac está abierta: que encienda el servidor
            else { ip = t->second.ip; port = t->second.port; g_serverName = g_target; }
        }
    }
    javaMacs(json + "]");
    return ip;
}

static int connectTo(const std::string &ip, int port) {
    int fd;
    if (ip.find(':') != std::string::npos) {   // por el cable USB (IPv6 de la interfaz del cable)
        sockaddr_in6 a6;
        if (!v6addr(ip, port, a6)) return -1;
        fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        uint64_t nh = g_usbNet.load();
        if (nh) android_setsocknetwork((net_handle_t)nh, fd);
        if (connect(fd, (sockaddr *)&a6, sizeof a6) != 0) { close(fd); return -1; }
    } else {
        fd = socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        sockaddr_in a{}; a.sin_family = AF_INET; a.sin_port = htons(port);
        inet_pton(AF_INET, ip.c_str(), &a.sin_addr);
        if (connect(fd, (sockaddr *)&a, sizeof a) != 0) { close(fd); return -1; }
    }
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int rcv = 8 << 20;
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcv, sizeof rcv);
    return fd;
}

/// La conexión actual con la Mac va por el cable (IPv6 de la interfaz USB)
static std::atomic<bool> g_connUsb{false};
/// ¿La Mac contesta por el cable? (pregunta a todos en ff02::1, puerto 9103)
static bool usbMacAnswers() {
    uint64_t nh = g_usbNet.load();
    std::string ifn;
    { std::lock_guard<std::mutex> lk(g_usbLock); ifn = g_usbIf; }
    if (!nh || ifn.empty()) return false;
    int v = socket(AF_INET6, SOCK_DGRAM, 0);
    if (v < 0) return false;
    bool ok = false;
    if (android_setsocknetwork((net_handle_t)nh, v) == 0) {
        timeval tv{0, 150000};
        setsockopt(v, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        sockaddr_in6 all{}; all.sin6_family = AF_INET6; all.sin6_port = htons(9103);
        inet_pton(AF_INET6, "ff02::1", &all.sin6_addr);
        all.sin6_scope_id = if_nametoindex(ifn.c_str());
        const char *q = "{\"qvb\":\"probe\"}";
        sendto(v, q, strlen(q), 0, (sockaddr *)&all, sizeof all);
        char b[512];
        double until = nowMs() + 600;
        while (!ok && nowMs() < until) {
            ssize_t n = recv(v, b, sizeof b - 1, 0);
            if (n > 0) { b[n] = 0; ok = strstr(b, "\"qvb\"") != nullptr; }
        }
    }
    close(v);
    return ok;
}
/// Prueba conectarse por el cable a cada dirección que dio la Mac (TCP, medio segundo cada una). Devuelve la que anduvo.
static std::string tryCableLL() {
    uint64_t nh = g_usbNet.load();
    std::string ifn;
    { std::lock_guard<std::mutex> lk(g_usbLock); ifn = g_usbIf; }
    if (!nh || ifn.empty()) return "";
    std::vector<std::string> ll;
    { std::lock_guard<std::mutex> lk(g_llLock); ll = g_macLL; }
    for (const std::string &a : ll) {
        std::string ip = a + "%" + ifn;
        sockaddr_in6 a6;
        if (!v6addr(ip, NATIVE_PORT, a6)) continue;
        int fd = socket(AF_INET6, SOCK_STREAM, 0);
        if (fd < 0) continue;
        android_setsocknetwork((net_handle_t)nh, fd);
        int fl = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, fl | O_NONBLOCK);
        bool ok = false;
        if (connect(fd, (sockaddr *)&a6, sizeof a6) == 0) ok = true;
        else if (errno == EINPROGRESS) {
            pollfd pf{fd, POLLOUT, 0};
            if (poll(&pf, 1, 500) == 1) { int err = 0; socklen_t el = sizeof err; getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &el); ok = err == 0; }
        }
        close(fd);
        if (ok) return ip;
    }
    return "";
}

/// Cable enchufado con la conexión por Wi-Fi: en cuanto la Mac contesta por el cable, se corta la conexión
/// (el hilo de red se vuelve a conectar solo y elige el cable). Lo prueba durante unos 20 s.
static void usbSwitchLater() {
    // Vigilante permanente: mientras haya red por el cable y la conexión vaya por Wi-Fi, cada 2 s se fija si la
    // Mac contesta por el cable; si contesta, corta (el hilo de red se reconecta y elige el cable)
    static std::atomic<bool> running{false};
    if (running.exchange(true)) return;
    int fails = 0;
    while (g_usbNet.load()) {
        sleep(2);
        int fd = g_sock.load();
        if (fd < 0 || g_connUsb) continue;
        std::string direct = usbMacAnswers() ? "" : tryCableLL();
        if (!direct.empty()) { std::lock_guard<std::mutex> lk(g_llLock); g_cableIp = direct; }
        if (!direct.empty() || usbMacAnswers()) {
            LOGI("Cable USB: me paso del Wi-Fi al cable");
            sendLog("Cable USB-C: la Mac contesta por el cable; me paso del Wi-Fi al cable");
            shutdown(fd, SHUT_RDWR);
            sleep(4);
        } else if (++fails % 15 == 1) sendLog("Cable USB-C: hay red por el cable pero la Mac no contesta por ahí (¿VRmin Host abierta?)");
    }
    running = false;
}

static bool readExact(int fd, uint8_t *p, size_t n) {
    size_t off = 0;
    while (off < n) {
        ssize_t r = recv(fd, p + off, n - off, 0);
        if (r <= 0) return false;
        off += (size_t)r;
    }
    return true;
}

// ---------------------------------------------------------------------------
// Video: MediaCodec -> AImageReader (memoria de GPU) -> textura externa
// ---------------------------------------------------------------------------
static AMediaCodec *g_codec = nullptr;
static AImageReader *g_reader = nullptr;
static std::mutex g_codecLock;
static int g_vw = 0, g_vh = 0;   // tamaño del video (2 ojos lado a lado)

/// Se crea cuando la Mac avisa la resolución (mensaje de configuración).
/// El lector de imágenes se crea una sola vez con el tamaño más grande posible
/// para no liberarlo mientras la GPU lo usa; el decodificador sí se recrea.
// Foveated encoding: la Mac achica el borde de cada ojo antes de codificar; acá se vuelve a estirar al mostrarlo.
// La cuenta es la misma que en la Mac (densidad 1 en el centro, que baja suave hasta «m» en el borde).
struct FovParams { int level = 0, size = 1; float cx[2] = {0.5f, 0.5f}, cy = 0.5f; };
static std::mutex g_fovLock;
static FovParams g_fovP;
static std::atomic<int> g_fovVer{0};
static std::atomic<float> g_fovC[3] = {{0.5f}, {0.5f}, {0.5f}};   // centro óptico de cada ojo (x izq, x der, y) en la imagen
/// De la imagen del juego (0..1) a la codificada (0..1): 1024 valores
static void fovInverse(const FovParams &P, float c, float *out) {
    const float ms[4] = {1, 0.62f, 0.48f, 0.36f};
    const float r0s[3] = {0.10f, 0.16f, 0.24f}, r1s[3] = {0.30f, 0.38f, 0.46f};
    const double m = ms[std::max(0, std::min(3, P.level))], r0 = r0s[std::max(0, std::min(2, P.size))], r1 = r1s[std::max(0, std::min(2, P.size))];
    const int n = 2048;
    static double E[2049];
    double acc = 0; E[0] = 0;
    for (int i = 0; i < n; i++) {
        double t = (i + 0.5) / n, d = fabs(t - c);
        double x = std::max(0.0, std::min(1.0, (d - r0) / (r1 - r0))), sm = x * x * (3 - 2 * x);
        acc += (1 - (1 - m) * sm) / n;
        E[i + 1] = acc;
    }
    for (int i = 0; i < 1024; i++) {
        double sv = i / 1023.0 * n;
        int j = std::min(n - 1, (int)sv);
        double k = sv - j;
        out[i] = (float)((E[j] + (E[j + 1] - E[j]) * k) / acc);
    }
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_questvrbridge_client_VrActivity_nativeFovCenters(JNIEnv *e, jclass) {
    char b[64]; snprintf(b, sizeof b, "%.4f,%.4f,%.4f", g_fovC[0].load(), g_fovC[1].load(), g_fovC[2].load());
    return e->NewStringUTF(b);
}

static bool g_hevc = false;   // códec de los juegos que manda la Mac (HEVC o H.264)
static void decoderCreate(int w, int h, bool hevc) {
    std::lock_guard<std::mutex> lk(g_codecLock);
    if (g_codec && w == g_vw && h == g_vh && hevc == g_hevc) return;
    if (g_codec) { AMediaCodec_stop(g_codec); AMediaCodec_delete(g_codec); g_codec = nullptr; }
    g_vw = w; g_vh = h; g_hevc = hevc;
    const char *mime = hevc ? "video/hevc" : "video/avc";
    if (!g_reader) {
        if (AImageReader_newWithUsage(w, h, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 6, &g_reader) != AMEDIA_OK) {
            LOGE("No se pudo crear AImageReader"); g_reader = nullptr; return;
        }
    }
    ANativeWindow *win = nullptr;
    AImageReader_getWindow(g_reader, &win);
    g_codec = AMediaCodec_createDecoderByType(mime);
    if (!g_codec) { LOGE("No hay decodificador %s", mime); return; }
    AMediaFormat *f = AMediaFormat_new();
    AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, mime);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, w);
    AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, h);
    AMediaFormat_setInt32(f, "low-latency", 1);
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
    AMediaFormat_setInt32(f, "vendor.qti-ext-dec-picture-order.enable", 1);
    AMediaFormat_setInt32(f, "priority", 0);
    AMediaFormat_setInt32(f, "operating-rate", 1000);
    media_status_t st = AMediaCodec_configure(g_codec, f, win, nullptr, 0);
    AMediaFormat_delete(f);
    if (st != AMEDIA_OK || AMediaCodec_start(g_codec) != AMEDIA_OK) {
        LOGE("No se pudo iniciar el decodificador (%d)", (int)st);
        AMediaCodec_delete(g_codec); g_codec = nullptr; return;
    }
    LOGI("Decodificador H.264 listo (%dx%d)", w, h);
}

static void decoderDestroy() {
    std::lock_guard<std::mutex> lk(g_codecLock);
    if (g_codec) { AMediaCodec_stop(g_codec); AMediaCodec_delete(g_codec); g_codec = nullptr; }
}

static void decodeFrame(const uint8_t *p, size_t n) {
    // cabecera: [1][poseId u32][key u8][ms u16] + H.264 Annex-B
    if (n < 9) return;
    uint32_t pid = (uint32_t)p[1] << 24 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 8 | p[4];
    bool key = p[5] == 1;
    g_framesIn++;
    if (g_needKey && !key) return;
    std::lock_guard<std::mutex> lk(g_codecLock);
    if (!g_codec) return;
    ssize_t idx = AMediaCodec_dequeueInputBuffer(g_codec, 8000);
    if (idx < 0) { g_framesDropped++; g_needKey = true; return; }
    size_t cap = 0;
    uint8_t *buf = AMediaCodec_getInputBuffer(g_codec, (size_t)idx, &cap);
    size_t len = n - 8;
    if (!buf || len > cap) { AMediaCodec_queueInputBuffer(g_codec, (size_t)idx, 0, 0, 0, 0); g_needKey = true; return; }
    memcpy(buf, p + 8, len);
    AMediaCodec_queueInputBuffer(g_codec, (size_t)idx, 0, len, (uint64_t)pid, 0);
    if (key) g_needKey = false;
}

static void decoderOutputLoop() {
    while (g_running) {
        AMediaCodec *c;
        { std::lock_guard<std::mutex> lk(g_codecLock); c = g_codec; }
        if (!c) { usleep(5000); continue; }
        AMediaCodecBufferInfo info;
        ssize_t idx = AMediaCodec_dequeueOutputBuffer(c, &info, 5000);
        if (idx >= 0) {
            AMediaCodec_releaseOutputBuffer(c, (size_t)idx, true);   // va directo a la textura
            g_framesDecoded++;
        }
    }
}

// ---------------------------------------------------------------------------
// Sonido: PCM 16 bits estéreo -> AAudio
// ---------------------------------------------------------------------------
// Colchón de sonido sin bloqueos (un hilo escribe —la red— y otro lee —el parlante—), con:
//  · colchón adaptable: empieza en 60 ms y crece si la red viene a los saltos (hasta 160 ms)
//  · compensación de relojes: si se llena o se vacía de a poco, se reproduce apenas más rápido o más lento
//    (±0,6 %, inaudible) en vez de tirar pedazos o quedarse sin sonido
//  · si igual se vacía, baja el volumen suave (sin clic) y vuelve a subir suave
static constexpr size_t AUD_CAP = 1 << 15;                 // cuadros estéreo (≈ 680 ms a 48 kHz)
static int16_t g_aRing[AUD_CAP * 2];
static std::atomic<uint64_t> g_aW{0}, g_aR{0};             // cuadros escritos y leídos (siempre crecen)
static std::atomic<uint64_t> g_aSkip{0};                   // la red pide tirar lo viejo (colchón demasiado lleno)
static AAudioStream *g_aStream = nullptr;
static std::atomic<int> g_aRate{0};
static std::atomic<int> g_aOutRate{0};                     // a cuánto suena de verdad el parlante (puede no ser lo pedido)
static std::atomic<bool> g_aRestart{false};
// Solo los usa el hilo del parlante
static bool g_aPrimed = false;
static double g_aFrac = 0, g_aFill = 0, g_aTarget = 0.06, g_aGain = 0;
static float g_aLast[2] = {0, 0};
static int64_t g_aCalm = 0;                                // cuadros seguidos sin quedarse sin sonido
static std::atomic<uint32_t> g_aUnder{0};                  // veces que el sonido se quedó sin colchón (un corte)

static aaudio_data_callback_result_t audioCb(AAudioStream *, void *, void *data, int32_t frames) {
    int16_t *out = (int16_t *)data;
    const int rate = std::max(8000, g_aRate.load());
    uint64_t r = g_aR.load(std::memory_order_relaxed), w = g_aW.load(std::memory_order_acquire);
    uint64_t skip = g_aSkip.exchange(0);
    if (skip && w - r > skip) r += skip;                    // demasiado atrasado: saltar (raro, por ejemplo después de una pausa)
    double avail = (double)(w - r);
    g_aFill = g_aFill == 0 ? avail : g_aFill * 0.98 + avail * 0.02;
    const double target = g_aTarget * rate;
    if (!g_aPrimed) {
        if (avail < target) { memset(out, 0, (size_t)frames * 4); g_aR.store(r, std::memory_order_release); return AAUDIO_CALLBACK_RESULT_CONTINUE; }
        g_aPrimed = true; g_aFrac = 0; g_aGain = 0; g_aFill = avail;
    }
    // Más lleno que el objetivo → un poquito más rápido; más vacío → un poquito más lento
    // (y si el parlante suena a otra frecuencia que la que manda la Mac, se convierte: si no, se vacía cada pocos segundos)
    const int outRate = std::max(8000, g_aOutRate.load() > 0 ? g_aOutRate.load() : rate);
    double ratio = (double)rate / outRate * (1.0 + std::max(-0.006, std::min(0.006, (g_aFill - target) / target * 0.01)));
    const double fadeStep = 1.0 / (outRate * 0.005);        // subir o bajar el volumen en 5 ms
    for (int i = 0; i < frames; i++) {
        uint64_t idx = r + (uint64_t)g_aFrac;
        if (idx + 1 >= w) {
            // Se vació: apagar suave desde el último valor y esperar a juntar colchón de nuevo
            for (int j = i; j < frames; j++) {
                g_aLast[0] *= 0.94f; g_aLast[1] *= 0.94f;
                out[j * 2] = (int16_t)g_aLast[0]; out[j * 2 + 1] = (int16_t)g_aLast[1];
            }
            g_aPrimed = false;
            g_aCalm = 0;
            g_aUnder++;
            g_aTarget = std::min(0.16, g_aTarget + 0.02);   // la red viene a los saltos: más colchón
            r += (uint64_t)g_aFrac; g_aFrac = 0;
            g_aR.store(std::min(r, w), std::memory_order_release);
            return AAUDIO_CALLBACK_RESULT_CONTINUE;
        }
        double f = g_aFrac - floor(g_aFrac);
        const int16_t *s0 = &g_aRing[(idx % AUD_CAP) * 2], *s1 = &g_aRing[((idx + 1) % AUD_CAP) * 2];
        g_aGain = std::min(1.0, g_aGain + fadeStep);
        for (int c = 0; c < 2; c++) {
            float v = (float)((s0[c] + (s1[c] - s0[c]) * f) * g_aGain);
            g_aLast[c] = v;
            out[i * 2 + c] = (int16_t)std::max(-32768.f, std::min(32767.f, v));
        }
        g_aFrac += ratio;
    }
    r += (uint64_t)g_aFrac;
    g_aFrac -= floor(g_aFrac);
    g_aR.store(r, std::memory_order_release);
    // Si anda bien un buen rato, achicar de a poco el colchón (menos demora)
    g_aCalm += frames;
    if (g_aCalm > outRate * 20 && g_aTarget > 0.06) { g_aTarget = std::max(0.06, g_aTarget - 0.01); g_aCalm = 0; }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

/// Si el parlante se desconecta (por ejemplo cambia la salida de sonido), se vuelve a abrir con el próximo paquete
static void audioErr(AAudioStream *, void *, aaudio_result_t e) { LOGE("Sonido: error %d, se vuelve a abrir", (int)e); g_aRestart = true; }

static void audioStart(int rate) {
    if (g_aStream && g_aRate == rate && !g_aRestart) return;
    if (g_aStream) { AAudioStream_requestStop(g_aStream); AAudioStream_close(g_aStream); g_aStream = nullptr; }
    g_aRestart = false;
    AAudioStreamBuilder *b = nullptr;
    if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setSampleRate(b, rate);
    AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(b, AAUDIO_USAGE_GAME);
    AAudioStreamBuilder_setDataCallback(b, audioCb, nullptr);
    AAudioStreamBuilder_setErrorCallback(b, audioErr, nullptr);
    g_aRate = rate;
    g_aPrimed = false;
    if (AAudioStreamBuilder_openStream(b, &g_aStream) == AAUDIO_OK) {
        // Margen del lado del parlante: 4 ráfagas (si no, cualquier demora del sistema se oye como un clic)
        int32_t burst = AAudioStream_getFramesPerBurst(g_aStream);
        if (burst > 0) AAudioStream_setBufferSizeInFrames(g_aStream, burst * 4);
        g_aOutRate = AAudioStream_getSampleRate(g_aStream);
        AAudioStream_requestStart(g_aStream);
        LOGI("Sonido: %d Hz (pedido), %d Hz (real), ráfaga %d", rate, AAudioStream_getSampleRate(g_aStream), (int)burst);
        if (AAudioStream_getSampleRate(g_aStream) != rate) sendLog("Sonido: la Mac manda %d Hz y el visor suena a %d Hz (se convierte)", rate, AAudioStream_getSampleRate(g_aStream));
    } else {
        g_aStream = nullptr;
        LOGE("No se pudo abrir el sonido");
    }
    AAudioStreamBuilder_delete(b);
}

static void playAudio(const uint8_t *p, size_t n) {
    // [2][0][0][0][rate u32 BE] + PCM 16 bits estéreo intercalado (little endian)
    if (n < 12) return;
    int rate = (int)((uint32_t)p[4] << 24 | (uint32_t)p[5] << 16 | (uint32_t)p[6] << 8 | p[7]);
    if (rate < 8000 || rate > 192000) return;
    audioStart(rate);
    g_audioChunks++;
    const int16_t *s = (const int16_t *)(p + 8);
    size_t frames = (n - 8) / 4;
    uint64_t w = g_aW.load(std::memory_order_relaxed), r = g_aR.load(std::memory_order_acquire);
    if (w - r + frames > AUD_CAP - 64) return;               // lleno (el parlante no está leyendo): tirar este pedazo
    for (size_t i = 0; i < frames; i++) {
        size_t k = ((w + i) % AUD_CAP) * 2;
        g_aRing[k] = s[i * 2]; g_aRing[k + 1] = s[i * 2 + 1];
    }
    g_aW.store(w + frames, std::memory_order_release);
    // Más de 350 ms acumulados (por ejemplo, el parlante estuvo trabado): pedirle que salte hasta el objetivo
    uint64_t have = w + frames - r;
    if (have > (uint64_t)rate * 35 / 100) g_aSkip = have - (uint64_t)rate / 10;
}

// ---------------------------------------------------------------------------
// Sala (lobby): paneles nítidos que la app dibuja como capas propias del Quest,
// a la resolución completa del visor, aparte del video del juego.
// ---------------------------------------------------------------------------
struct PanelPose { bool on = false; XrPosef pose{}; float w = 0, h = 0; };
static std::mutex g_panelLock;
static bool g_lobby = false;
static PanelPose g_pList, g_pDesk;
static std::vector<uint8_t> g_listPixels;     // RGBA, ya dada vuelta (fila 0 = abajo, como GL)
static int g_listW = 0, g_listH = 0;
static bool g_listNew = false;

static bool parseFloats(const std::string &t, size_t from, size_t to, const char *key, float *out, int n) {
    size_t k = t.find(key, from);
    if (k == std::string::npos || k >= to) return false;
    const char *p = t.c_str() + k + strlen(key);
    for (int i = 0; i < n; i++) {
        while (*p == ' ' || *p == '[' || *p == ',') p++;
        char *e = nullptr;
        out[i] = strtof(p, &e);
        if (e == p || !std::isfinite(out[i])) return false;
        p = e;
    }
    return true;
}

static PanelPose parsePanel(const std::string &t, const char *name) {
    PanelPose pp;
    size_t k = t.find(name);
    if (k == std::string::npos) return pp;
    size_t end = t.find('}', k);
    if (end == std::string::npos) return pp;
    float pos[3], rot[4], w, h;
    if (!parseFloats(t, k, end, "\"pos\":", pos, 3) || !parseFloats(t, k, end, "\"rot\":", rot, 4) ||
        !parseFloats(t, k, end, "\"w\":", &w, 1) || !parseFloats(t, k, end, "\"h\":", &h, 1)) return pp;
    float ql = sqrtf(rot[0] * rot[0] + rot[1] * rot[1] + rot[2] * rot[2] + rot[3] * rot[3]);
    if (ql < 0.5f || w <= 0 || h <= 0 || w > 10 || h > 10) return pp;
    pp.pose.position = {pos[0], pos[1], pos[2]};
    pp.pose.orientation = {rot[0] / ql, rot[1] / ql, rot[2] / ql, rot[3] / ql};
    pp.w = w; pp.h = h; pp.on = true;
    return pp;
}

static std::atomic<int> g_panelState{0};   // 0 sin datos, 1 sala visible, 2 hay un juego abierto
static std::atomic<bool> g_exitVR{false};    // volver a la ventana 2D (botón de la sala o los dos joysticks apretados)

static void receivePanels(const std::string &t) {
    bool lobby = t.find("\"lobby\":true") != std::string::npos;
    g_panelState = lobby ? 1 : 2;
    PanelPose l = lobby ? parsePanel(t, "\"list\":") : PanelPose{};
    PanelPose d = lobby ? parsePanel(t, "\"desk\":") : PanelPose{};
    std::lock_guard<std::mutex> lk(g_panelLock);
    g_lobby = lobby; g_pList = l; g_pDesk = d;
}

/// [6][0][w u16][h u16][tamaño original u32] + RGBA comprimido (deflate)
static void receiveList(const uint8_t *p, size_t n) {
    if (n < 11) return;
    int w = p[2] << 8 | p[3], h = p[4] << 8 | p[5];
    uint32_t raw = (uint32_t)p[6] << 24 | (uint32_t)p[7] << 16 | (uint32_t)p[8] << 8 | p[9];
    if (w <= 0 || h <= 0 || w > 4096 || h > 4096 || raw != (uint32_t)w * (uint32_t)h * 4) return;
    std::vector<uint8_t> out(raw);
    int r = Z_DATA_ERROR;
    for (int bits : {-15, 15}) {      // deflate "crudo" (Apple) o con cabecera zlib
        z_stream z{};
        if (inflateInit2(&z, bits) != Z_OK) continue;
        z.next_in = (Bytef *)(p + 10); z.avail_in = (uInt)(n - 10);
        z.next_out = out.data(); z.avail_out = raw;
        r = inflate(&z, Z_FINISH);
        inflateEnd(&z);
        if (r == Z_STREAM_END) break;
    }
    if (r != Z_STREAM_END) { LOGE("No pude descomprimir la lista (%d)", r); return; }
    std::vector<uint8_t> flip(raw);
    size_t row = (size_t)w * 4;
    for (int y = 0; y < h; y++) memcpy(flip.data() + (size_t)(h - 1 - y) * row, out.data() + (size_t)y * row, row);
    std::lock_guard<std::mutex> lk(g_panelLock);
    g_listPixels.swap(flip); g_listW = w; g_listH = h; g_listNew = true;
}

// Escritorio de la Mac: un segundo decodificador H.264, con su propia resolución (nítida)
static AMediaCodec *g_deskCodec = nullptr;
static AImageReader *g_deskReader = nullptr;
static std::mutex g_deskLock;
static int g_deskW = 0, g_deskH = 0;                       // tamaño del decodificador actual
static std::atomic<int> g_deskWantW{0}, g_deskWantH{0};    // tamaño que manda la Mac
static std::atomic<bool> g_deskNeedKey{true};
static std::atomic<uint32_t> g_deskIn{0}, g_deskDecoded{0};

static void requestDeskKey() {
    static double last = 0;
    g_deskNeedKey = true;
    double t = nowMs();
    if (t - last < 300) return;
    last = t;
    sendPacket("{\"type\":\"deskkey\"}");
}

/// [7][key u8][w u16][h u16] + H.264 Annex-B
static void receiveDesk(const uint8_t *p, size_t n) {
    if (n < 7) return;
    bool key = p[1] == 1;
    int w = p[2] << 8 | p[3], h = p[4] << 8 | p[5];
    if (w < 64 || h < 64 || w > 4096 || h > 4096) return;
    g_deskWantW = w; g_deskWantH = h;   // el hilo de dibujo crea el decodificador si cambió
    g_deskIn++;
    bool askKey = false;
    {
        std::lock_guard<std::mutex> lk(g_deskLock);
        if (!g_deskCodec || w != g_deskW || h != g_deskH) return;
        if (g_deskNeedKey && !key) return;
        ssize_t idx = AMediaCodec_dequeueInputBuffer(g_deskCodec, 4000);
        if (idx < 0) { askKey = true; }
        else {
            size_t cap = 0;
            uint8_t *buf = AMediaCodec_getInputBuffer(g_deskCodec, (size_t)idx, &cap);
            size_t len = n - 6;
            if (!buf || len > cap) { AMediaCodec_queueInputBuffer(g_deskCodec, (size_t)idx, 0, 0, 0, 0); askKey = true; }
            else {
                memcpy(buf, p + 6, len);
                static uint64_t ts = 0;
                AMediaCodec_queueInputBuffer(g_deskCodec, (size_t)idx, 0, len, ++ts, 0);
                if (key) g_deskNeedKey = false;
            }
        }
    }
    if (askKey) requestDeskKey();
}

// ---------------------------------------------------------------------------
// Puente con la parte Java (VrActivity): el menú y las ventanas de la sala son interfaz de Android
// dibujada en pantallas virtuales; acá se muestran como paneles y se les manda el puntero.
// ---------------------------------------------------------------------------
static JavaVM *g_vm = nullptr;
static jobject g_act = nullptr;
static jmethodID g_jPacket = nullptr, g_jConn = nullptr, g_jPointer = nullptr;
static std::atomic<bool> g_gameActive{false};

static JNIEnv *jenv() {
    JNIEnv *e = nullptr;
    if (!g_vm) return nullptr;
    if (g_vm->GetEnv((void **)&e, JNI_VERSION_1_6) != JNI_OK) g_vm->AttachCurrentThread(&e, nullptr);
    return e;
}

static void javaPacket(const uint8_t *p, size_t n) {
    JNIEnv *e = jenv();
    if (!e || !g_act || !g_jPacket) return;
    jbyteArray arr = e->NewByteArray((jsize)n);
    if (!arr) { e->ExceptionClear(); return; }
    e->SetByteArrayRegion(arr, 0, (jsize)n, (const jbyte *)p);
    e->CallVoidMethod(g_act, g_jPacket, arr);
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(arr);
}

static void javaConnection(bool c, const std::string &name) {
    JNIEnv *e = jenv();
    if (!e || !g_act || !g_jConn) return;
    jstring s = e->NewStringUTF(name.c_str());
    e->CallVoidMethod(g_act, g_jConn, (jboolean)c, s);
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(s);
}

static jmethodID g_jInputLock = nullptr;
static jmethodID g_jMacs = nullptr, g_jWatch = nullptr, g_jAuth = nullptr, g_jGameState = nullptr, g_jGameMenu = nullptr, g_jPortalDone = nullptr;
static void javaMacs(const std::string &json) {
    JNIEnv *e = jenv();
    if (!e || !g_act || !g_jMacs) return;
    jstring s = e->NewStringUTF(json.c_str());
    e->CallVoidMethod(g_act, g_jMacs, s);
    if (e->ExceptionCheck()) e->ExceptionClear();
    e->DeleteLocalRef(s);
}

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetCableOnly(JNIEnv *, jclass, jboolean on) {
    g_cableOnly = on;
    if (on && g_sock >= 0 && !g_connUsb) shutdown(g_sock, SHUT_RDWR);   // reconectar (ahora solo por el cable)
}
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetUsbNet(JNIEnv *env, jclass, jlong handle, jstring ifn) {
    const char *c = ifn ? env->GetStringUTFChars(ifn, nullptr) : nullptr;
    { std::lock_guard<std::mutex> lk(g_usbLock); g_usbIf = c ? c : ""; }
    if (c) env->ReleaseStringUTFChars(ifn, c);
    g_usbNet = (uint64_t)handle;
    LOGI("Cable USB: %s", handle ? g_usbIf.c_str() : "sin conexión");
    // Si ya estabas conectado por Wi-Fi y enchufás el cable: pasarse al cable (cuando la Mac contesta por ahí)
    if (handle) std::thread(usbSwitchLater).detach();
}

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeConnectTo(JNIEnv *env, jclass, jstring name) {
    const char *c = env->GetStringUTFChars(name, nullptr);
    bool other;
    { std::lock_guard<std::mutex> lk(g_macLock); g_target = c ? c : ""; other = g_target != g_serverName; }
    if (c) env->ReleaseStringUTFChars(name, c);
    int fd = g_sock.load();
    if (other && fd >= 0) shutdown(fd, SHUT_RDWR);   // estaba conectada a otra Mac
}

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeDisconnect(JNIEnv *, jclass) {
    { std::lock_guard<std::mutex> lk(g_macLock); g_target.clear(); }
    int fd = g_sock.load();
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
}

/// Primer mensaje para la Mac (clave guardada o código del usuario); "" = esperar el código
static std::string javaAuth(const std::string &mac) {
    JNIEnv *e = jenv();
    if (!e || !g_act || !g_jAuth) return "";
    jstring jn = e->NewStringUTF(mac.c_str());
    jstring r = (jstring)e->CallObjectMethod(g_act, g_jAuth, jn);
    e->DeleteLocalRef(jn);
    if (e->ExceptionCheck()) { e->ExceptionClear(); return ""; }
    std::string out;
    if (r) { const char *c = e->GetStringUTFChars(r, nullptr); if (c) { out = c; e->ReleaseStringUTFChars(r, c); } e->DeleteLocalRef(r); }
    return out;
}

// Juego VR: menú propio (mantener el botón de menú izquierdo), ventanas que flotan durante el juego y
// manos reales dentro del juego (experimental)
static const int GAME_MENU_ID = 0x2FFFFFD0;
static std::atomic<bool> g_gameMenu{false};      // el menú del juego está abierto (el juego no recibe los botones)
static std::atomic<bool> g_gameHidden{false};    // «Menú principal» desde el menú del juego: el juego sigue abierto, se ve la sala
static std::atomic<bool> g_gameHands{false};     // manos reales también durante el juego (experimental)
static std::atomic<bool> g_handGestures{true};
// Ajuste del ángulo de las manos y el láser en los juegos (grados, se suma a lo de fábrica):
// inclinación (arriba/abajo) y giro (sobre el eje largo de la mano). Vale con manos y con controles.
static std::atomic<float> g_gripPitch[2] = {{0.f}, {0.f}}, g_gripRoll[2] = {{0.f}, {0.f}};   // [0] izquierda · [1] derecha
/// Aplica el ajuste a una pose (giro sobre su X propio y después sobre su Z propio; el giro va en espejo en la izquierda)
static XrQuaternionf adjustPose(XrQuaternionf q, float pitchDeg, float rollDeg, int side) {
    const float hA = pitchDeg * 3.14159265f / 180.f * 0.5f, rx = sinf(hA), rw = cosf(hA);
    XrQuaternionf r{q.w * rx + q.x * rw, q.y * rw + q.z * rx, -q.y * rx + q.z * rw, q.w * rw - q.x * rx};
    const float hR = (side == 0 ? -1.f : 1.f) * rollDeg * 3.14159265f / 180.f * 0.5f, rz = sinf(hR), rzw = cosf(hR);
    return {r.x * rzw + r.y * rz, -r.x * rz + r.y * rzw, r.w * rz + r.z * rzw, r.w * rzw - r.z * rz};
}
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetHandAngles(JNIEnv *, jclass, jfloat pl, jfloat rl, jfloat pr, jfloat rr) {
    g_gripPitch[0] = pl; g_gripRoll[0] = rl; g_gripPitch[1] = pr; g_gripRoll[1] = rr;
}
// El teclado se abre debajo de esta pantalla o ventana
static std::atomic<int> g_kbAnchor{-1};

// ---------------------------------------------------------------------------
// Modo Centinela: el visor queda apoyado en el escritorio mirándote, sigue los controles, y el juego se ve en la
// pantalla de la Mac. La «cabeza» que recibe el juego es virtual: a tu altura, detrás de tus manos, y (después
// de calibrar) se mueve con los controles (agacharte, inclinarte).
// ---------------------------------------------------------------------------
static std::atomic<bool> g_vigia{false};
static std::atomic<bool> g_vigiaCalReq{false};
static std::atomic<float> g_vigiaH{1.75f};   // altura de la cabeza en el Modo Centinela (m)
// Cabeza vista por la cámara de la Mac (lo que te moviste desde que se centró, en metros y radianes)
struct CamHead { double at = 0; bool ok = false; float dx = 0, dy = 0, dz = 0, yaw = 0, pitch = 0; };
static std::mutex g_camHeadLock;
static CamHead g_camHead;
// Muñecas vistas por la cámara de la Mac (en la imagen, 0 a 1; c = confianza): 0 izquierda, 1 derecha (tuyas)
struct CamHands { double at = 0; float x[2] = {0, 0}, y[2] = {0, 0}, c[2] = {0, 0}; };
static CamHands g_camHands;
/// De la imagen de la cámara de la Mac a la sala: se aprende mientras el visor ve los controles (mínimos cuadrados
/// que se van olvidando de a poco) y se usa cuando deja de verlos. pos = W · (x, y, 1)
struct CamFit {
    double A[3][3] = {}, B[3][3] = {};   // Σ f·fᵀ y Σ f·posᵀ
    double n = 0;
    void add(float x, float y, const XrVector3f &p) {
        const double f[3] = {x, y, 1}, q[3] = {p.x, p.y, p.z};
        for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) { A[i][j] = A[i][j] * 0.997 + f[i] * f[j]; B[i][j] = B[i][j] * 0.997 + f[i] * q[j]; }
        n = n * 0.997 + 1;
    }
    bool predict(float x, float y, float out[3]) const {
        if (n < 40) return false;
        // resolver A · w = B (columna por columna), con Cramer
        auto det = [](const double m[3][3]) { return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]); };
        double D = det(A);
        if (fabs(D) < 1e-6 * n * n * n) return false;   // las muestras no alcanzan (todas en el mismo lugar)
        const double f[3] = {x, y, 1};
        for (int k = 0; k < 3; k++) {
            double w[3];
            for (int c = 0; c < 3; c++) {
                double M[3][3];
                for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) M[i][j] = j == c ? B[i][k] : A[i][j];
                w[c] = det(M) / D;
            }
            out[k] = (float)(w[0] * f[0] + w[1] * f[1] + w[2] * f[2]);
        }
        return true;
    }
};
static CamFit g_camFit[2];
static float jsonNum(const std::string &t, const char *key) {
    size_t p = t.find(key);
    return p == std::string::npos ? 0.f : (float)atof(t.c_str() + p + strlen(key));
}
struct Vigia {
    bool calibrated = false;
    float calMid[3] = {0, 0, 0}, calHead[3] = {0, 0, 0};
    float fwd[2] = {0, -1};         // hacia dónde mirás (hacia el visor), en el piso
    float yawOff = 0;               // giro con el joystick derecho
    float head[3] = {0, 1.64f, 0};  // cabeza (suavizada)
    float mid[3] = {0, 1.2f, 0};    // punto medio de los controles (suavizado)
    bool haveMid = false;
    double calAt = 0;               // cuenta regresiva para calibrar
    double lastMsg = 0;
    bool turned = false;
    // Marco del juego: se gira y se corre todo (cabeza y controles) para que mires hacia adelante del juego (−Z)
    // parado en el centro, aunque el visor esté apoyado en cualquier dirección. Se toma al empezar, al calibrar
    // y al recentrar (botón de Meta).
    std::atomic<bool> needFrame{true};
    float c[2] = {0, 0}, yaw0 = 0;
    bool haveFrame = false;
    float yOff = 0;   // corrimiento de altura de la sala (se saca: el juego recibe alturas reales)
    // Corrección del piso: con el visor apoyado, el Quest a veces calcula mal dónde está el piso (las manos le
    // quedan muy arriba o muy abajo). Se mide con los controles: al calibrar (delante del pecho) siempre, y al
    // empezar solo si la diferencia es grande.
    float corr = 0;
    bool calNow = false, didStartFloor = false;
    bool handOk[2] = {true, true};   // la posición de cada control es creíble este cuadro
    bool headTracked = true;
    bool camOn = false;               // la cámara de la Mac está siguiendo tu cabeza
    float camYaw = 0, camPitch = 0;         // el visor sabe dónde está (si no, los controles quedan «adivinados» cerca suyo)
    double untrackedSince = 0;
    float lastHand[2][3] = {{0, 0, 0}, {0, 0, 0}};
    double handStillSince[2] = {0, 0};
    // Cuerpo: manos promediadas (lentas) y su referencia (al centrar), con la cabeza de referencia (x, z)
    float slow[3] = {0, 0, 0}, ref[3] = {0, 0, 0}, refHead[2] = {0, 0};
    bool haveSlow = false, haveRef = false;
    // Mirar alrededor con el pulgar apoyado en el joystick derecho
    bool looking = false;
    float lookY0 = 0, lookP0 = 0, lookYaw = 0, lookPitch = 0;
};
static Vigia g_vg;
/// Lleva una pose del espacio de la sala al marco del Modo Centinela
static void vigiaXf(XrPosef &p) {
    p.position.y -= g_vg.yOff;
    if (!g_vg.haveFrame) return;
    const float th = -g_vg.yaw0, cs = cosf(th), sn = sinf(th);
    const float x = p.position.x - g_vg.c[0], z = p.position.z - g_vg.c[1];
    p.position.x = x * cs + z * sn;
    p.position.z = -x * sn + z * cs;
    const float s = sinf(th * 0.5f), w = cosf(th * 0.5f);
    const XrQuaternionf b = p.orientation;
    p.orientation = {w * b.x + s * b.z, w * b.y + s * b.w, w * b.z - s * b.x, w * b.w - s * b.y};
    // Giro del cuerpo (pulgar en el joystick derecho): todo gira alrededor de la cabeza
    const float ly = g_vg.lookYaw;
    if (ly != 0.f) {
        const float hx0 = g_vg.head[0] - g_vg.c[0], hz0 = g_vg.head[2] - g_vg.c[1];
        const float px = hx0 * cs + hz0 * sn, pz = -hx0 * sn + hz0 * cs;   // la cabeza en el marco del juego
        const float c2 = cosf(ly), s2 = sinf(ly);
        const float rx = p.position.x - px, rz = p.position.z - pz;
        p.position.x = px + rx * c2 + rz * s2;
        p.position.z = pz - rx * s2 + rz * c2;
        const float s3 = sinf(ly * 0.5f), w3 = cosf(ly * 0.5f);
        const XrQuaternionf o = p.orientation;
        p.orientation = {w3 * o.x + s3 * o.z, w3 * o.y + s3 * o.w, w3 * o.z - s3 * o.x, w3 * o.w - s3 * o.y};
    }
}
static void vigiaSay(const char *msg) {
    std::string t = "{\"type\":\"vigia\",\"msg\":\"";
    t += msg; t += "\"}";
    sendPacket(t);
}
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetVigia(JNIEnv *, jclass, jboolean on) {
    g_vigia = on;
    if (on) { g_vg.calibrated = false; g_vg.haveMid = false; g_vg.yawOff = 0; g_vg.calAt = 0; g_vg.haveFrame = false; g_vg.needFrame = true; g_vg.corr = 0; g_vg.calNow = false; g_vg.didStartFloor = false; g_vg.haveSlow = false; g_vg.haveRef = false; }
    sendPacket(on ? "{\"type\":\"vigia\",\"on\":true}" : "{\"type\":\"vigia\",\"on\":false}");
}
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetVigiaHeight(JNIEnv *, jclass, jfloat m) { g_vigiaH = m > 1.0f && m < 2.3f ? m : 1.75f; }
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeVigiaCalibrate(JNIEnv *, jclass) { g_vigiaCalReq = true; }
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetKeyboardAnchor(JNIEnv *, jclass, jint id) { g_kbAnchor = id; }
static std::atomic<int> g_frameGenMode{0};       // generación de cuadros: 0 apagada, 1 completar hasta los Hz del visor, 2 mitad (Meta)
static std::atomic<bool> g_frameGenOn{false};    // (este cuadro se están generando cuadros: para el indicador)
static std::atomic<bool> g_frameGenSent{false};  // ya se le avisó a la Mac el estado actual   // en los juegos, la mano sin control hace de control con gestos
static bool g_inGameNow = false;                 // este cuadro se ve un juego
static char g_appVersion[32] = "?";              // versión de la app (para el registro)
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetVersion(JNIEnv *env, jclass, jstring v) {
    const char *c = env->GetStringUTFChars(v, nullptr);
    if (c) { snprintf(g_appVersion, sizeof g_appVersion, "%s", c); env->ReleaseStringUTFChars(v, c); }
}

static void javaBool(jmethodID m, bool v) {
    JNIEnv *e = jenv();
    if (!e || !g_act || !m) return;
    e->CallVoidMethod(g_act, m, (jboolean)v);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetPanelInGame(JNIEnv *, jclass, jint id, jboolean on);
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetGameHands(JNIEnv *, jclass, jboolean on) { g_gameHands = on; }
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeCloseGameMenu(JNIEnv *, jclass) { g_gameMenu = false; }
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetGameHidden(JNIEnv *, jclass, jboolean on) { g_gameHidden = on; }
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetHandGestures(JNIEnv *, jclass, jboolean on) { g_handGestures = on; }
// Altura en la sala y en los juegos (0 = la real del visor). Se mide al elegirla y al recentrar.
static std::atomic<float> g_heightM{1.75f};
static std::atomic<bool> g_heightDirty{true};
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetHeight(JNIEnv *, jclass, jfloat m) { g_heightM = m; g_heightDirty = true; }
// Moverse por la sala con las manos (puño con el pulgar encima: adelante = teletransporte, a los costados = girar)
static std::atomic<bool> g_handMove{true};
// Controles y manos en pausa (para mirar algo y comer o usar el celular sin tocar nada): A+B, X+Y o pulgar con meñique derecho
static std::atomic<bool> g_inputLock{false}, g_inputLockReq{false};
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeToggleInputLock(JNIEnv *, jclass) { g_inputLockReq = true; }   // (el botón de la barra)
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetHandMove(JNIEnv *, jclass, jboolean on) { g_handMove = on; }
// Movimiento en la sala: giro (0 por saltos, 1 suave), grados por salto, velocidad del giro suave (°/s),
// desplazamiento (0 teletransporte, 1 joystick, 2 los dos), velocidad al caminar (m/s) y viñeta al moverte
static std::atomic<int> g_turnMode{0}, g_snapDeg{30}, g_turnSpeed{120}, g_moveMode{0};
static std::atomic<float> g_moveSpeed{2.0f};
static std::atomic<bool> g_vignette{true};
static float g_vigAmt = 0;   // (hilo de dibujo) cuánto se oscurecen los bordes ahora
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetLocomotion(JNIEnv *, jclass, jint turnMode, jint snapDeg, jint turnSpeed, jint moveMode, jfloat moveSpeed, jboolean vignette) {
    g_turnMode = turnMode; g_snapDeg = std::max(5, (int)snapDeg); g_turnSpeed = std::max(10, (int)turnSpeed);
    g_moveMode = moveMode; g_moveSpeed = moveSpeed; g_vignette = vignette;
}
// «Traer todo al frente»: gira la sala y las ventanas para que queden delante de donde mirás
static std::atomic<bool> g_recenter{false};
// Recentrar con el botón de Meta: el espacio de la sala (STAGE) no gira con el recentrado, así que la sala quedaba
// donde estaba (a veces a un costado): unos cuadros después (con la pose ya nueva) se trae todo al frente
static std::atomic<int> g_recenterIn{0};
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeRecenter(JNIEnv *, jclass) { g_recenter = true; }
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetFrameGen(JNIEnv *, jclass, jint mode) { g_frameGenMode = std::max(0, std::min(2, (int)mode)); g_frameGenSent = false; }

/// Miraste el reloj: abrir o cerrar el menú
static void javaWatch() {
    JNIEnv *e = jenv();
    if (!e || !g_act || !g_jWatch) return;
    e->CallVoidMethod(g_act, g_jWatch);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

/// El portal quedó listo: el cartel de cómo dibujarlo se va
static void javaPortalDone() {
    JNIEnv *e = jenv();
    if (!e || !g_act || !g_jPortalDone) return;
    e->CallVoidMethod(g_act, g_jPortalDone);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

static void javaPointer(int id, int action, float u, float v, float scroll) {
    JNIEnv *e = jenv();
    if (!e || !g_act || !g_jPointer) return;
    e->CallVoidMethod(g_act, g_jPointer, (jint)id, (jint)action, u, v, scroll);
    if (e->ExceptionCheck()) e->ExceptionClear();
}

struct UiHeld { AImage *img; EGLImageKHR egl; };
struct UiPanel {
    int id = 0, w = 0, h = 0;
    float wm = 1, hm = 1;        // tamaño en metros
    bool tv = false, dead = false, placed = false, ready = false;
    AImageReader *reader = nullptr;
    XrPosef pose{};
    std::deque<UiHeld> held;
    // swapchain (se crea en el hilo de dibujo)
    XrSwapchain sc = XR_NULL_HANDLE;
    int mips = 1;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    std::vector<GLuint> fbos;
    int parent = 0;              // barra de botones de otro panel (va pegada arriba a la izquierda de él)
    bool curved = false;         // curvada (como un monitor curvo)
    float radius = 0;            // radio de la curva (la distancia a la que estabas al curvarla)
    double hotUntil = 0;         // hasta cuándo se ve su barra de botones (aparece al apuntarle)
    float baseW = 0, baseH = 0;  // tamaño original de una barra de botones (crece con la distancia para que se pueda usar)
    int dock = 0;                // barra de botones: 0 arriba a la izquierda (por fuera), 1 abajo a la derecha
    float growK = 0, growAt[3] = {0, 0, 0};   // cuánto creció la barra y dónde estaba su ventana (no cambia al mover la cabeza)
    bool noTouch = false;        // el láser no toca esta pantalla (los botones de su barra sí)
    int lastImg = -1;            // la imagen más nueva del swapchain (para el 3D)
    bool fresh = false;          // cambió desde la última vez que se armó el 3D
    // 3D: una imagen por ojo
    XrSwapchain sc3[2] = {XR_NULL_HANDLE, XR_NULL_HANDLE};
    std::vector<XrSwapchainImageOpenGLESKHR> images3[2];
    std::vector<GLuint> fbos3[2];
    // 3D de una ventana (la TV usa el 3D general del escritorio)
    int mode3d = 0;                  // 0 apagado, 1 IA, 2 SBS, 3 arriba/abajo
    // 3D real (SBS, arriba/abajo): sin las barras negras del video, y ampliado (puede pasarse de los bordes)
    float zoom = 1;                  // 1 = el tamaño del video en la pantalla
    float crop[4] = {0, 0, 1, 1};    // lo que es video (sin barras), en la imagen de un ojo
    double cropAt = 0;
    bool fillOn = false;             // este cuadro se muestra solo el video (recortado y ampliado)
    float fillOff[2] = {0, 0}, fillSize[2] = {1, 1};   // dónde va y de qué tamaño (en metros, respecto de la pantalla)
    int lastMode = -1;
    bool hud = false;                // indicador de rendimiento: va pegado a la vista
    bool inGame = false;             // se ve durante un juego VR (el menú del juego y las ventanas abiertas desde él)
    float curveAmt = 0;              // cuánto se curva (0 = plana, 1 = muy curva)
    float baseWm = 0;                // tamaño original (el control de tamaño es relativo a esto)
    uint32_t imgVer = 0;             // ajustes de imagen con los que se armó la última copia
    bool follow = false, folInit = false;   // fijada a la vista: te sigue suavemente cuando girás
    double openAt = 0, closeAt = 0;         // animaciones: cuándo apareció (y cuándo se empezó a cerrar)
    float barK = 0, grabK = 0;              // barrita: 0 quieta · 0,5 apuntada · 1 agarrada; ventana «levantada» al moverla
    float folOff[3] = {0, 0, 0};            // dónde está respecto de tu cabeza (derecha, arriba, adelante)
    float str3d = 0.009f;
    float vrect[4] = {0, 0, 1, 1};   // dónde está la imagen de la Mac dentro de la ventana
    GLuint depthTex = 0;
    bool depthReady = false;
    double depthSeenAt = 0;          // cuándo se subió la última profundidad a su textura
    float lastStr = -1;
};
static std::mutex g_uiLock;

// 3D automático del escritorio: profundidad calculada por una IA en la Mac
static std::atomic<int> g_3dMode{0};
static std::atomic<int> g_handModel{0};        // 0 control remoto, 1 celular, 2 banana, 3 lápiz
static std::atomic<int> g_handColor{1};        // tono de piel: 0 (muy clara) … 5 (muy oscura)
static std::atomic<int> g_tzMin{INT32_MIN};    // diferencia horaria del Quest en minutos (la manda Java; si no, la del sistema)
static std::atomic<bool> g_realHands{true};   // ver las manos (y los controles) reales
static float g_palmDot[3] = {0, 0, 0};          // dónde dibujar el puntito de progreso (mirando el borde de la mano)
static std::atomic<bool> g_watchOn{true};       // reloj en la muñeca izquierda (mirarlo abre o cierra el menú)
static std::atomic<float> g_watchProg{0};        // cuánto falta para que se abra el menú (0 a 1), mientras lo mirás
static float g_watchWorld[3] = {0, 1.2f, -0.4f};   // dónde estaba el reloj cuando lo miraste (para poner el menú encima)
static const int KEYBOARD_ID = 0x2FFFFFC0;          // el teclado para escribir en la Mac
static const int LOCK_NOTE_ID = 0x2FFFFFB1;         // el aviso de «controles y manos en pausa» (va donde el teclado)
static const int WRIST_MENU_ID = 0x2FFFFFE0;        // el menú que aparece sobre el reloj
static std::atomic<bool> g_wristFollow{true};       // el menú del reloj lo sigue (o quedó suelto, como una ventana)
static float g_watchNow[3] = {0, 1.2f, -0.4f};      // dónde está el reloj ahora
static bool g_watchNowOk = false;
static std::atomic<int> g_laserColor{0};       // 0 ámbar (el clásico), 1 blanco, 2 celeste, 3 verde, 4 rojo, 5 violeta, 6 rosa, 7 amarillo
static std::atomic<float> g_laserGain{1.f};    // intensidad del láser (0,15 a 1,7; 1 = normal)
// Ajustes de imagen (filtros): brillo, contraste, saturación, nitidez, calidez
static std::mutex g_imgLock;
static float g_img[5] = {1, 1, 1, 0, 0};
static std::atomic<uint32_t> g_imgVer{0};
static std::atomic<float> g_3dStrength{0.008f};
static std::atomic<bool> g_leds{true};
static std::atomic<float> g_lightGain{1.0f};   // brillo de la pantalla y de las luces de atrás
static std::atomic<int> g_env{0};              // 0 living, 1 espacio, 2 cine, 3 passthrough (tu cuarto real, sin moverte), 4 carga (el vacío de la pantalla de carga)
static std::atomic<bool> g_envChanged{false};
static std::atomic<int> g_envUser{0};          // la sala elegida (g_env es la que se dibuja: cambia en la mitad de la transición)
// Pantalla de carga: mientras se abre un juego (la pide la parte Java) se va a la sala «carga» con su panel adelante
static std::atomic<bool> g_loading{false};
static bool g_loadingNow = false;              // (hilo de dibujo) se está viendo la pantalla de carga
static const int LOADING_ID = 0x2FFFFFA0;
/// Ambientes con la pantalla principal fija (living: sobre su mueble; cine: la pantalla grande).
/// Ahí la pantalla principal no se mueve, no cambia de tamaño y no se curva. Solo en el espacio flota libre.
/// Cualquier ambiente nuevo es "fijo" salvo que se agregue acá.
static bool fixedScreenEnv() { return g_env != 1 && g_env != 3 && g_env != 4; }
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetLight(JNIEnv *, jclass, jfloat g) { g_lightGain = std::min(2.f, std::max(0.f, (float)g)); }
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetEnvironment(JNIEnv *, jclass, jint e) {
    g_envUser = std::max(0, std::min(4, (int)e));   // (el cambio, con su transición, lo hace el hilo de dibujo)
}
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetLoading(JNIEnv *, jclass, jboolean on) { g_loading = on; }
/// Durante la pantalla de carga solo se ven su panel, el menú del reloj y el teclado (lo demás de la sala vuelve después)
static bool hiddenByLoading(const UiPanel *p);
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSet3DStrength(JNIEnv *, jclass, jfloat s) { g_3dStrength = std::min(0.04f, std::max(0.f, (float)s)); }
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetLeds(JNIEnv *, jclass, jboolean on) { g_leds = on; }
static std::mutex g_depthLock;
/// Profundidad que manda la Mac: la clave 0 es el escritorio (TV); las demás, el número de cada ventana
struct DepthBuf { std::vector<uint8_t> px; int w = 0, h = 0; double at = 0; };
static std::map<uint32_t, DepthBuf> g_depth;
static float g_deskRect[4] = {0, 0, 1, 1};
static std::atomic<bool> g_deskVisible{false};

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSet3DMode(JNIEnv *, jclass, jint m) { g_3dMode = std::min(3, std::max(0, (int)m)); }

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetHandModel(JNIEnv *, jclass, jint m) { g_handModel = std::min(3, std::max(0, (int)m)); }

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetHandColor(JNIEnv *, jclass, jint c) { g_handColor = std::min(5, std::max(0, (int)c)); }

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetTimeZone(JNIEnv *, jclass, jint minutes) { g_tzMin = minutes; }

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetWatch(JNIEnv *, jclass, jboolean on) { g_watchOn = on; }

/// Ver tus manos y controles reales en la sala (passthrough proyectado)
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetRealHands(JNIEnv *, jclass, jboolean on) { g_realHands = on; }

/// El menú del reloj sigue al reloj (true) o queda suelto donde está, como una ventana más (false)
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetWristFollow(JNIEnv *, jclass, jboolean on) { g_wristFollow = on; }

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetLaser(JNIEnv *, jclass, jint color, jfloat gain) {
    g_laserColor = std::min(7, std::max(0, (int)color));
    g_laserGain = std::min(1.7f, std::max(0.15f, (float)gain));
}

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetImage(JNIEnv *, jclass, jfloat b, jfloat c, jfloat sat, jfloat sh, jfloat w) {
    std::lock_guard<std::mutex> lk(g_imgLock);
    g_img[0] = b; g_img[1] = c; g_img[2] = sat; g_img[3] = sh; g_img[4] = w;
    g_imgVer++;
}

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeDeskRect(JNIEnv *, jclass, jboolean vis, jfloat x0, jfloat y0, jfloat x1, jfloat y1) {
    std::lock_guard<std::mutex> lk(g_depthLock);
    g_deskRect[0] = x0; g_deskRect[1] = y0; g_deskRect[2] = x1; g_deskRect[3] = y1;
    g_deskVisible = vis;
}

/// [9][0][w u16][h u16][tamaño u32] + profundidad de 8 bits comprimida (fila 0 = arriba): el escritorio.
/// [9][1][ventana u32][w u16][h u16][tamaño u32] + lo mismo: una ventana.
static void receiveDepth(const uint8_t *p, size_t n) {
    if (n < 11) return;
    uint32_t key = 0;
    if (p[1] == 1) {
        if (n < 15) return;
        key = (uint32_t)p[2] << 24 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 8 | p[5];
        p += 4; n -= 4;
    }
    int w = p[2] << 8 | p[3], h = p[4] << 8 | p[5];
    uint32_t raw = (uint32_t)p[6] << 24 | (uint32_t)p[7] << 16 | (uint32_t)p[8] << 8 | p[9];
    if (w <= 0 || h <= 0 || w > 2048 || h > 2048 || raw != (uint32_t)(w * h)) return;
    std::vector<uint8_t> out(raw);
    z_stream z{};
    if (inflateInit2(&z, -15) != Z_OK) return;
    z.next_in = (Bytef *)(p + 10); z.avail_in = (uInt)(n - 10);
    z.next_out = out.data(); z.avail_out = raw;
    int r = inflate(&z, Z_FINISH);
    inflateEnd(&z);
    if (r != Z_STREAM_END) return;
    std::lock_guard<std::mutex> lk(g_depthLock);
    DepthBuf &d = g_depth[key];
    d.px.swap(out); d.w = w; d.h = h; d.at = nowMs();
}

static std::vector<UiPanel *> g_ui;

/// 3D de una ventana: prendido o apagado, e intensidad
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetPanel3D(JNIEnv *, jclass, jint id, jint mode, jfloat s) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (UiPanel *p : g_ui) if (p->id == id) { p->mode3d = std::min(3, std::max(0, (int)mode)); p->str3d = std::min(0.04f, std::max(0.f, (float)s)); }
}

/// Ampliar el video en 3D real (SBS o arriba/abajo): 1 = su tamaño en la pantalla; más, se pasa de los bordes
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetZoom(JNIEnv *, jclass, jint id, jfloat z) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (UiPanel *p : g_ui) if (p->id == id) { p->zoom = std::min(6.f, std::max(1.f, (float)z)); p->fresh = true; }
}

/// Indicador de rendimiento: el panel va pegado a la vista (abajo), no se apunta ni se mueve
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetHud(JNIEnv *, jclass, jint id) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (UiPanel *p : g_ui) if (p->id == id) { p->hud = true; p->placed = true; }
}

/// Tamaño de una ventana relativo al original (el control de tamaño de la barra)
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSizePanel(JNIEnv *, jclass, jint id, jfloat f) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (UiPanel *p : g_ui) if (p->id == id && !p->parent && !(p->tv && fixedScreenEnv())) {
        if (p->baseWm <= 0) p->baseWm = p->wm;
        float nw = std::min(10.f, std::max(0.3f, p->baseWm * (float)f));
        p->hm *= nw / p->wm; p->wm = nw;
    }
}

/// Números para el indicador de rendimiento (se piden una vez por segundo)
extern "C" JNIEXPORT jstring JNICALL
Java_org_questvrbridge_client_VrActivity_nativeStats(JNIEnv *env, jclass) {
    static double lastT = 0;
    static uint64_t lastBytes = 0;
    static uint32_t lastR = 0, lastIn = 0, lastDec = 0, lastDesk = 0, lastDrop = 0;
    double t = nowMs();
    double secs = lastT > 0 ? std::max(0.2, (t - lastT) / 1000.0) : 1.0;
    uint64_t b = g_bytesIn;
    uint32_t r = g_rendered, in = g_framesIn, dec = g_framesDecoded, desk = g_deskIn, drop = g_framesDropped;
    char out[400];
    float shownFps = (r - lastR) / secs * (g_frameGenOn && g_frameGenMode == 2 ? 2.f : 1.f);   // con el modo de Meta, el visor muestra el doble
    snprintf(out, sizeof out, "{\"pantalla\":%.0f,\"generando\":%s,\"visor\":%.0f,\"juego\":%.0f,\"decod\":%.0f,\"escritorio\":%.0f,\"mbps\":%.1f,\"lat\":%.0f,\"perdidos\":%u,\"conectado\":%s,\"enJuego\":%s,\"usb\":%s}",
             shownFps, g_frameGenOn ? "true" : "false", (r - lastR) / secs, (in - lastIn) / secs, (dec - lastDec) / secs, (desk - lastDesk) / secs,
             (b - lastBytes) * 8.0 / 1e6 / secs, (double)g_latency.load(), drop - lastDrop,
             g_sock >= 0 ? "true" : "false", g_gameActive ? "true" : "false", g_connUsb ? "true" : "false");
    lastT = t; lastBytes = b; lastR = r; lastIn = in; lastDec = dec; lastDesk = desk; lastDrop = drop;
    return env->NewStringUTF(out);
}

/// Dónde está la imagen de la Mac dentro de una ventana (el resto es fondo y no se corre)
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativePanelRect(JNIEnv *, jclass, jint id, jfloat x0, jfloat y0, jfloat x1, jfloat y1) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (UiPanel *p : g_ui) if (p->id == id) { p->vrect[0] = x0; p->vrect[1] = y0; p->vrect[2] = x1; p->vrect[3] = y1; }
}

extern "C" JNIEXPORT jobject JNICALL
Java_org_questvrbridge_client_VrActivity_nativeCreatePanel(JNIEnv *env, jclass, jint id, jint w, jint h, jfloat meters, jboolean tv) {
    UiPanel *p = new UiPanel();
    p->id = id; p->w = w; p->h = h; p->tv = tv;
    p->wm = meters; p->hm = meters * (float)h / (float)w;
    if (AImageReader_newWithUsage(w, h, AIMAGE_FORMAT_RGBA_8888, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 4, &p->reader) != AMEDIA_OK) {
        LOGE("Panel %d: no se pudo crear el lector de imágenes", id);
        delete p;
        return nullptr;
    }
    ANativeWindow *win = nullptr;
    AImageReader_getWindow(p->reader, &win);
    jobject surf = ANativeWindow_toSurface(env, win);
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (UiPanel *q : g_ui) if (q->id == id && q->closeAt > 0) q->dead = true;   // (la que se estaba cerrando con el mismo número)
    g_ui.push_back(p);
    return surf;
}

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetPanelInGame(JNIEnv *, jclass, jint id, jboolean on) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (UiPanel *p : g_ui) if (p->id == id) { p->inGame = on; if (on && g_inGameNow) p->placed = false; }   // durante el juego: delante tuyo
}

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeAttachPanel(JNIEnv *, jclass, jint child, jint parent) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (UiPanel *p : g_ui) if (p->id == child) p->parent = parent;
}

/// Dónde va una barra de botones: 0 arriba a la izquierda, 1 abajo a la derecha
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetDock(JNIEnv *, jclass, jint id, jint dock) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (UiPanel *p : g_ui) if (p->id == id) p->dock = dock;
}

/// El láser toca (hace clic en) esta pantalla o no; los botones de su barra siempre se pueden usar
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetPanelNoTouch(JNIEnv *, jclass, jint id, jboolean on) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (UiPanel *p : g_ui) if (p->id == id) p->noTouch = on;
}

/// Agrandar o achicar una ventana en la sala (la imagen mantiene su resolución)
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeScalePanel(JNIEnv *, jclass, jint id, jfloat f) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (UiPanel *p : g_ui) if (p->id == id && !(p->tv && fixedScreenEnv())) {   // la pantalla principal fija no cambia de tamaño
        float nw = std::min(8.f, std::max(0.4f, p->wm * f));
        p->hm *= nw / p->wm; p->wm = nw;
    }
}

/// Curvar o enderezar una ventana
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeCurvePanel(JNIEnv *, jclass, jint id) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (UiPanel *p : g_ui) if (p->id == id && !(p->tv && fixedScreenEnv())) p->curveAmt = p->curveAmt > 0.01f ? 0.f : 0.5f;
}

/// Cuánto curvar una ventana (0 = plana … 1 = muy curva): el control de la barra
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetCurve(JNIEnv *, jclass, jint id, jfloat a) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (UiPanel *p : g_ui) if (p->id == id && !p->parent && !p->hud && !(p->tv && fixedScreenEnv())) p->curveAmt = std::min(1.f, std::max(0.f, (float)a));
}

/// Volver a poner la ventana en su lugar (delante tuyo; la TV, sobre su mueble)
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeResetPanel(JNIEnv *, jclass, jint id) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (UiPanel *p : g_ui) if (p->id == id) p->placed = false;
}

/// Fijar una ventana a la vista (te sigue cuando girás la cabeza) o soltarla
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetFollow(JNIEnv *, jclass, jint id, jboolean on) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (UiPanel *p : g_ui) if (p->id == id) { p->follow = on; p->folInit = false; }
}

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeDestroyPanel(JNIEnv *, jclass, jint id) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    // Se achica y se desvanece (≈ 0,18 s) y recién ahí se borra; si nunca se vio, se borra ya
    for (UiPanel *p : g_ui) if (p->id == id && !p->dead && p->closeAt == 0) {
        if (p->ready && p->placed && !p->hud) p->closeAt = nowMs(); else p->dead = true;
    }
}

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSend(JNIEnv *env, jclass, jstring js) {
    const char *c = env->GetStringUTFChars(js, nullptr);
    if (c) { sendPacket(c); env->ReleaseStringUTFChars(js, c); }
}

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeExitVR(JNIEnv *, jclass) { g_exitVR = true; }

// ---------------------------------------------------------------------------
// Hilo de red: descubrir la Mac, conectarse y recibir video/sonido/config
// ---------------------------------------------------------------------------
static void networkLoop() {
    jenv();   // este hilo también le habla a la parte Java
    while (g_running) {
        int port = NATIVE_PORT;
        std::string ip;
        {   // por el cable con una dirección que ya anduvo
            std::lock_guard<std::mutex> lk(g_llLock);
            if (g_usbNet.load() && !g_cableIp.empty() && !g_serverName.empty()) ip = g_cableIp;
        }
        if (ip.empty()) {
            ip = discoverMac(port);
            if (ip.empty()) continue;
            if (ip.find(':') == std::string::npos && g_usbNet.load()) {
                // Encontrada por Wi-Fi pero hay cable: probar directo por el cable
                std::string c = tryCableLL();
                if (!c.empty()) { ip = c; port = NATIVE_PORT; std::lock_guard<std::mutex> lk(g_llLock); g_cableIp = c; }
            }
            if (g_cableOnly && ip.find(':') == std::string::npos) {
                static double lastSay = 0;
                if (nowMs() - lastSay > 10000) { lastSay = nowMs(); LOGI("Solo por cable: la Mac no aparece por el cable (%s)", g_usbNet.load() ? "hay red USB" : "sin red USB"); }
                sleep(1);
                continue;
            }
        }
        // Emparejado: la clave guardada o el código que escribió el usuario (si falta el código, esperar)
        std::string auth = javaAuth(g_serverName);
        if (auth.empty()) { usleep(400000); continue; }
        int fd = connectTo(ip, port);
        if (fd < 0) {
            LOGE("No pude conectar con %s:%d", ip.c_str(), port);
            { std::lock_guard<std::mutex> lk(g_llLock); if (ip == g_cableIp) g_cableIp.clear(); }
            sleep(1); continue;
        }
        LOGI("Conectado a la Mac %s (%s)", ip.c_str(), g_serverName.c_str());
        {   // lo primero que recibe la Mac (antes que cualquier otro mensaje)
            uint32_t len = htonl((uint32_t)auth.size());
            std::string b((const char *)&len, 4); b += auth;
            size_t off = 0;
            while (off < b.size()) { ssize_t n = send(fd, b.data() + off, b.size() - off, MSG_NOSIGNAL); if (n <= 0) break; off += (size_t)n; }
        }
        g_connUsb = ip.find(':') != std::string::npos;
        g_sock = fd;
        sendPacket("{\"type\":\"hello\",\"mode\":\"vr2\"}");   // sala con el menú y las ventanas dibujados acá
        bool authed = false;
        std::vector<uint8_t> buf(1 << 20);
        while (g_running) {
            uint8_t h[4];
            if (!readExact(fd, h, 4)) break;
            uint32_t n = (uint32_t)h[0] << 24 | (uint32_t)h[1] << 16 | (uint32_t)h[2] << 8 | h[3];
            if (n == 0 || n > (64u << 20)) break;
            if (n > buf.size()) buf.resize(n);
            if (!readExact(fd, buf.data(), n)) break;
            g_bytesIn += n + 4;
            switch (buf[0]) {
                case 1: decodeFrame(buf.data(), n); break;
                case 2: playAudio(buf.data(), n); break;
                case 6: receiveList(buf.data(), n); break;
                case 7: case 8: javaPacket(buf.data(), n); break;   // escritorio y ventanas: los muestra la interfaz Java
                case 9: receiveDepth(buf.data(), n); break;          // profundidad para el 3D
                case 3: {
                    std::string t((const char *)buf.data() + 1, n - 1);
                    if (t.find("\"type\":\"auth\"") != std::string::npos) {
                        javaPacket(buf.data(), n);   // la parte Java guarda la clave o pide el código
                        if (!authed && t.find("\"ok\":true") != std::string::npos) {
                            authed = true;
                            g_frameGenSent = false;   // decirle a la Mac si el juego dibuja la mitad de cuadros
                            javaConnection(true, g_serverName.empty() ? ip : g_serverName);
                            requestKey();
                            sendLog("App nativa conectada (Quest) · versión %s", g_appVersion);
                            if (g_vigia) sendPacket("{\"type\":\"vigia\",\"on\":true}");   // (Modo Centinela pedido antes de conectar)
                        }
                    }
                    else if (t.find("\"lobby2d\"") != std::string::npos) {
                        parseLL(t.c_str());
                        g_gameActive = t.find("\"game\":true") != std::string::npos;
                        if (!g_gameActive) g_gameHidden = false;   // (el próximo juego se ve)
                        javaPacket(buf.data(), n);
                    }
                    else if (t.find("\"vigiahands\"") != std::string::npos) {
                        CamHands c;
                        c.at = nowMs();
                        c.x[0] = jsonNum(t, "\"lx\":"); c.y[0] = jsonNum(t, "\"ly\":"); c.c[0] = jsonNum(t, "\"lc\":");
                        c.x[1] = jsonNum(t, "\"rx\":"); c.y[1] = jsonNum(t, "\"ry\":"); c.c[1] = jsonNum(t, "\"rc\":");
                        std::lock_guard<std::mutex> lk(g_camHeadLock);
                        g_camHands = c;
                    }
                    else if (t.find("\"vigiahead\"") != std::string::npos) {
                        CamHead c;
                        c.ok = t.find("\"ok\":true") != std::string::npos;
                        c.at = nowMs();
                        c.dx = jsonNum(t, "\"dx\":"); c.dy = jsonNum(t, "\"dy\":"); c.dz = jsonNum(t, "\"dz\":");
                        c.yaw = jsonNum(t, "\"yaw\":"); c.pitch = jsonNum(t, "\"pitch\":");
                        std::lock_guard<std::mutex> lk(g_camHeadLock);
                        g_camHead = c;
                    }
                    else if (t.find("\"panels\"") != std::string::npos) receivePanels(t);
                    else if (t.find("\"exitvr\"") != std::string::npos) g_exitVR = true;
                    else if (t.find("\"config\"") != std::string::npos) {
                        size_t q = t.find("\"fps\":");
                        if (q != std::string::npos) { int f = atoi(t.c_str() + q + 6); if (f >= 60 && f <= 144) g_targetFps = f; }
                        size_t e = t.find("\"eye\":");
                        int eye = e != std::string::npos ? atoi(t.c_str() + e + 6) : 1024;
                        if (eye < 256 || eye > 4096) eye = 1024;
                        // tamaño de lo que se codifica (con foveated encoding es más chico que lo que dibuja el juego)
                        int vw = (int)jsonNum(t, "\"vw\":"), vh = (int)jsonNum(t, "\"vh\":");
                        if (vw < 256 || vw > 8192 || vh < 256 || vh > 8192) { vw = eye * 2; vh = eye; }
                        {
                            FovParams P;
                            size_t f = t.find("\"fov\":\"");
                            if (f != std::string::npos) {
                                float v[5] = {0, 1, 0.5f, 0.5f, 0.5f};
                                int nv = sscanf(t.c_str() + f + 7, "%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4]);
                                if (nv >= 1) P.level = (int)v[0];
                                if (nv >= 2) P.size = (int)v[1];
                                if (nv >= 5) { P.cx[0] = v[2]; P.cx[1] = v[3]; P.cy = v[4]; }
                            }
                            std::lock_guard<std::mutex> lk(g_fovLock);
                            g_fovP = P;
                            g_fovVer++;
                            sendLog(P.level > 0 ? "Foveated encoding: nivel %d (video %dx%d)" : "Foveated encoding: apagado (video %dx%d)", P.level > 0 ? P.level : vw, P.level > 0 ? vw : vh, vh);
                        }
                        decoderCreate(vw, vh, t.find("\"codec\":\"hevc\"") != std::string::npos);
                        requestKey();
                    }
                    break;
                }
                default: break;
            }
        }
        LOGI("Se cortó la conexión con la Mac");
        g_sock = -1;
        close(fd);
        decoderDestroy();
        { std::lock_guard<std::mutex> lk(g_panelLock); g_lobby = false; }
        g_panelState = 0;
        g_deskNeedKey = true;
        g_gameActive = false;
        javaConnection(false, "");
    }
}

// ---------------------------------------------------------------------------
// Matemática
// ---------------------------------------------------------------------------
static void quatToMat(const XrQuaternionf &q, float m[9]) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    m[0] = 1 - 2 * (y * y + z * z); m[3] = 2 * (x * y - z * w);     m[6] = 2 * (x * z + y * w);
    m[1] = 2 * (x * y + z * w);     m[4] = 1 - 2 * (x * x + z * z); m[7] = 2 * (y * z - x * w);
    m[2] = 2 * (x * z - y * w);     m[5] = 2 * (y * z + x * w);     m[8] = 1 - 2 * (x * x + y * y);
}

// Matriz de vista (inversa de la pose del ojo), column-major como WebGL
static void viewMatrix(const XrPosef &p, float out[16]) {
    float r[9]; quatToMat(p.orientation, r);
    // R^T
    out[0] = r[0]; out[4] = r[1]; out[8] = r[2];
    out[1] = r[3]; out[5] = r[4]; out[9] = r[5];
    out[2] = r[6]; out[6] = r[7]; out[10] = r[8];
    out[3] = 0; out[7] = 0; out[11] = 0;
    const XrVector3f &t = p.position;
    out[12] = -(out[0] * t.x + out[4] * t.y + out[8] * t.z);
    out[13] = -(out[1] * t.x + out[5] * t.y + out[9] * t.z);
    out[14] = -(out[2] * t.x + out[6] * t.y + out[10] * t.z);
    out[15] = 1;
}

static void projMatrix(const XrFovf &f, float out[16]) {
    float l = tanf(f.angleLeft), r = tanf(f.angleRight), d = tanf(f.angleDown), u = tanf(f.angleUp);
    float n = 0.05f, fa = 100.f;
    memset(out, 0, 16 * sizeof(float));
    out[0] = 2 / (r - l); out[5] = 2 / (u - d);
    out[8] = (r + l) / (r - l); out[9] = (u + d) / (u - d);
    out[10] = -(fa + n) / (fa - n); out[11] = -1;
    out[14] = -2 * fa * n / (fa - n);
}

static void appendNum(std::string &s, float v) {
    char b[32];
    if (!std::isfinite(v)) v = 0;
    snprintf(b, sizeof b, "%.5g", v);
    s += b;
}

// ---------------------------------------------------------------------------
// OpenXR + EGL
// ---------------------------------------------------------------------------
#define XRCHK(x) do { XrResult _r = (x); if (XR_FAILED(_r)) { LOGE("%s falló: %d (línea %d)", #x, (int)_r, __LINE__); } } while (0)

struct Swapchain {
    XrSwapchain handle = XR_NULL_HANDLE;
    int32_t w = 0, h = 0, mips = 1;
    std::vector<XrSwapchainImageOpenGLESKHR> images;
    std::vector<GLuint> fbos;
};

struct HandInput {
    float trigger = 0, squeeze = 0, tx = 0, ty = 0;
    bool tclick = false, a = false, b = false, ttouch = false, thtouch = false, atouch = false, btouch = false, menu = false;
};

struct App {
    android_app *android = nullptr;
    bool resumed = false;

    EGLDisplay display = EGL_NO_DISPLAY;
    EGLConfig config = nullptr;
    EGLContext context = EGL_NO_CONTEXT;
    EGLSurface surface = EGL_NO_SURFACE;

    XrInstance instance = XR_NULL_HANDLE;
    XrSystemId system = XR_NULL_SYSTEM_ID;
    XrSession session = XR_NULL_HANDLE;
    XrSpace space = XR_NULL_HANDLE;
    XrSessionState state = XR_SESSION_STATE_UNKNOWN;
    bool sessionRunning = false;
    bool exitRequested = false;
    Swapchain sc[2];
    bool srgb = false;

    XrActionSet actionSet = XR_NULL_HANDLE;
    XrAction aGrip, aAim, aTrigger, aSqueeze, aStick, aStickClick, aStickTouch, aTrigTouch,
             aA, aB, aATouch, aBTouch, aMenu;
    XrPath hand[2];
    XrSpace gripSpace[2], aimSpace[2];

    bool hasRefresh = false;
    PFN_xrRequestDisplayRefreshRateFB xrRequestDisplayRefreshRateFB = nullptr;
    PFN_xrEnumerateDisplayRefreshRatesFB xrEnumerateDisplayRefreshRatesFB = nullptr;
    int appliedFps = 0;

    GLuint prog = 0, vao = 0, tex = 0;
    GLint uOff = -1, uSrgb = -1, uTex = -1, uFlow = -1, uFlowK = -1, uFovLut = -1, uFovOn = -1, uFovRow = -1;
    GLuint fovTex = 0; int fovVer = -1; bool fovOn = false;
    PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC getClientBuffer = nullptr;
    PFNEGLCREATEIMAGEKHRPROC createImage = nullptr;
    PFNEGLDESTROYIMAGEKHRPROC destroyImage = nullptr;
    PFNGLEGLIMAGETARGETTEXTURE2DOESPROC imageTarget = nullptr;

    // Imágenes en uso (se liberan con demora para no pisar a la GPU)
    struct Held { AImage *img; EGLImageKHR egl; };
    std::deque<Held> held;
    bool haveFrame = false;
    PoseSnap shown;
    double lastFrameMs = 0;

    uint32_t poseId = 0;
    double lastStats = 0;
    uint32_t rendered = 0;

    // Sala: capas propias del Quest (nítidas) + rayos de los controles
    int64_t scFormat = GL_RGBA8;
    bool hasLayerSettings = false;
    bool hasCylinder = false;
    bool hasColorScale = false;   // para que la TV y las ventanas aparezcan y desaparezcan de a poco
    Swapchain listSc, deskSc, overlay[2];
    bool listReady = false, deskReady = false;
    GLuint deskTex = 0;
    std::deque<Held> deskHeld;
    GLuint lineProg = 0, lineVao = 0, lineVbo = 0;
    GLint uVP = -1, uCol = -1, uRound = -1;
    GLint uSx = -1, uSy = -1, uKeepA = -1;
    GLint uFilt = -1, uRect = -1, uAdj = -1, uWarm = -1, uTexel = -1;
    GLint u3Mode = -1;
    XrSpace viewSpace = XR_NULL_HANDLE;   // pegado a la cabeza (indicador de rendimiento)
    double sticksSince = 0;

    // Sala VR con interfaz Java
    GLuint uiTex = 0;
    bool anchor = false;
    float anc[3] = {0, 0, 0}, fwd[3] = {0, 0, -1}, eyeH = 1.5f;
    int placedWindows = 0;
    struct HandPtr { int hover = -1; int down = -1; int grab = -1; bool trig = false; float grabT = 1, off[3] = {0, 0, 0}; int tick = 0; } hp[2];
    int lastHover = -1;   // panel que recibió el último "hover" (para avisarle cuando el puntero se va)
    // 3D: imagen de cada ojo armada desde la TV + la profundidad
    // Teletransporte (joystick derecho hacia abajo)
    float headPos[3] = {0, 1.2f, 0};
    float headFwd[3] = {0, 0, -1};   // hacia dónde mirás (para poner el menú del juego delante)
    float worldY = 0;   // altura del piso donde estás parado respecto del piso real (gradas del cine)
    bool tpByHand = false;   // el arco del teletransporte sale de la mano (pistola) y no del joystick
    float heightOff = 0, lastEyeY = 0;   // altura elegida: cuánto se sube (o baja) todo respecto del piso real
    bool haveEye = false, stageSpace = false;
    XrReferenceSpaceType spaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    // Transición tipo Vision Pro: el ambiente "llena" el lugar de a poco desde adelante (y al salir, al revés)
    bool hasPassthrough = false;
    bool hasTriMesh = false, hasHandTracking = false, hasHandMesh = false, hasHandAim = false;   // manos reales
    bool hasRenderModel = false, hasTimespec = false, hasSimul = false;
    bool hasSpaceWarp = false;           // generación de cuadros de Meta (Application SpaceWarp)
    uint32_t frameSeq = 0;               // cuántos cuadros nuevos del juego llegaron   // modelo del control, reloj de la cámara, manos y controles a la vez
    XrPassthroughFB pt = XR_NULL_HANDLE;
    XrPassthroughLayerFB ptLayer = XR_NULL_HANDLE;
    PFN_xrPassthroughStartFB ptStart = nullptr;
    PFN_xrPassthroughPauseFB ptPause = nullptr;
    bool ptRunning = false;
    double revealStart = 0, exitStart = 0;
    double envSwitchStart = 0;   // cambio de sala: la nueva aparece sobre la vieja con el borde de la transición
    int envSwitchTo = 0, envFrom = 0;
    bool envSwitched = false;
    bool envSwitchPT = false;    // (con el passthrough de por medio: la sala vieja se cierra a tu cuarto y se abre la nueva)
    GLuint xTex[2] = {0, 0}, xFbo[2] = {0, 0}, xDepth[2] = {0, 0};   // la sala que se va, dibujada aparte
    int xW[2] = {0, 0}, xH[2] = {0, 0};
    GLuint blitProg = 0; GLint bTex = -1;
    double anchorAt = 0;   // cuándo se tomó el punto de referencia (la transición espera a que todo esté listo)
    float revealFwd[3] = {0, 0, -1};
    GLuint maskProg = 0;
    GLint mTan = -1, mRot = -1, mFwd = -1, mAng = -1;
    bool tpAim = false, tpOk = false;
    float tpTgt[3] = {0, 0, 0};
    std::vector<float> tpArc;
    GLuint prog3d = 0, depthTex = 0;
    GLint u3Crop = -1, u3Fill = -1;
    GLint u3Src = -1, u3Dep = -1, u3Rect = -1, u3Eye = -1, u3Str = -1;
    bool depthReady = false;

    // Living con la TV (lo dibuja el Quest; la Mac no manda video mientras no haya juego)
    GLuint roomProg = 0, cubeVao = 0, cubeVbo = 0, depth[2] = {0, 0}, readFbo = 0;
    // Manos: un objeto en cada mano (control remoto, celular, banana o lápiz) y el láser solo en una
    GLuint handProg = 0, ovDepth[2] = {0, 0};
    GLint hVP = -1, hM = -1, hCol = -1, hEye = -1, hEmit = -1, hSkin = -1, hScl = -1;
    GLuint handVao = 0, handVbo = 0;
    int shFirst[16] = {}, shCount[16] = {};
    int activeHand = 1;   // la mano con el láser (empieza la derecha)
    // Lectura de colores de la TV sin frenar la GPU: se pide en un cuadro y se lee varios cuadros después
    GLuint ledPbo[2] = {0, 0};
    GLsync ledFence[2] = {nullptr, nullptr};
    int ledPw[2] = {0, 0}, ledPh[2] = {0, 0}, ledSlot = 0;
    GLint rEye = -1, rVP = -1, rM = -1, rN = -1, rCol = -1, rMat = -1, rLight = -1, rLamp = -1, rAnc = -1, rRx = -1, rFz = -1,
          rLedP = -1, rLedD = -1, rLedC = -1, rLedOn = -1, rAmb = -1, rScrP = -1, rScrN = -1, rScrC = -1, rGain = -1, rTime = -1, rScrK = -1, rScrR = -1, rScrU = -1, rScrH = -1, rScrG = -1;
    float led[12][3] = {};      // tiras de luz detrás de la TV: 3 tramos por lado (izq, der, arriba, abajo)
    float scrGrid[9][3] = {};   // la imagen de la pantalla en 3x3 zonas (la pantalla ilumina con esos colores)
    bool ledReady = false;
    int ledTick = 0;
};

static void initEGL(App &app) {
    app.display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    eglInitialize(app.display, nullptr, nullptr);
    const EGLint cfgAttr[] = { EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                               EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT_KHR, EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_NONE };
    EGLint num = 0;
    eglChooseConfig(app.display, cfgAttr, &app.config, 1, &num);
    const EGLint ctxAttr[] = { EGL_CONTEXT_CLIENT_VERSION, 3, EGL_NONE };
    app.context = eglCreateContext(app.display, app.config, EGL_NO_CONTEXT, ctxAttr);
    const EGLint pb[] = { EGL_WIDTH, 16, EGL_HEIGHT, 16, EGL_NONE };
    app.surface = eglCreatePbufferSurface(app.display, app.config, pb);
    eglMakeCurrent(app.display, app.surface, app.surface, app.context);

    app.getClientBuffer = (PFNEGLGETNATIVECLIENTBUFFERANDROIDPROC)eglGetProcAddress("eglGetNativeClientBufferANDROID");
    app.createImage = (PFNEGLCREATEIMAGEKHRPROC)eglGetProcAddress("eglCreateImageKHR");
    app.destroyImage = (PFNEGLDESTROYIMAGEKHRPROC)eglGetProcAddress("eglDestroyImageKHR");
    app.imageTarget = (PFNGLEGLIMAGETARGETTEXTURE2DOESPROC)eglGetProcAddress("glEGLImageTargetTexture2DOES");
    LOGI("EGL listo (%s)", (const char *)glGetString(GL_RENDERER));
}

static GLuint compileShader(GLenum type, const char *src) {
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, nullptr);
    glCompileShader(s);
    GLint ok = 0; glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) { char log[1024]; glGetShaderInfoLog(s, sizeof log, nullptr, log); LOGE("Shader: %s", log); }
    return s;
}

static void initGL(App &app) {
    const char *vs = R"(#version 300 es
out vec2 uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
    uv = vec2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);   // la fila 0 de la imagen es la de arriba
    gl_Position = vec4(p, 0.0, 1.0);
})";
    const char *fs = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES tex;
uniform float off;
uniform float sx;
uniform float sy;
uniform float srgb;
uniform float keepAlpha;
// Ajustes de imagen: solo dentro de rect (la imagen de la Mac), no sobre los menús
uniform float filt;
uniform vec4 rect;
uniform vec4 adj;      // brillo, contraste, saturación, nitidez
uniform float warm;    // calidez (-1 fría … +1 cálida)
uniform vec2 texel;
uniform highp sampler2D flow;   // completar cuadros: movimiento de cada zona (píxeles chicos por cuadro, y hacia arriba)
uniform vec2 flowK;             // x: activado, y: cuánto adelantar (fracción de cuadro / tamaño chico)
uniform highp sampler2D fovLut; // foveated encoding: de la imagen del juego a la codificada (fila 0/1: x de cada ojo, 2: y)
uniform float fovOn;
uniform float fovRow;
in vec2 uv;
out vec4 color;
highp float fl(highp float s, int row) {
    highp float f = clamp(s, 0.0, 1.0) * 1023.0;
    int i = min(int(f), 1022);
    highp float t = f - float(i);
    return mix(texelFetch(fovLut, ivec2(i, row), 0).r, texelFetch(fovLut, ivec2(i + 1, row), 0).r, t);
}
void main() {
    highp vec2 u2 = uv;
    if (fovOn > 0.5) u2 = vec2(fl(uv.x, int(fovRow)), fl(uv.y, 2));
    if (flowK.x > 0.5) { vec2 m = texture(flow, u2).xy; u2 = clamp(u2 - vec2(m.x, -m.y) * flowK.y, 0.0, 1.0); }
    vec2 tc = vec2(off + u2.x * sx, u2.y * sy);
    vec4 t4 = texture(tex, tc);
    vec3 c = t4.rgb;
    if (filt > 0.5 && uv.x >= rect.x && uv.x <= rect.z && uv.y >= rect.y && uv.y <= rect.w) {
        if (adj.w > 0.0) {
            vec3 b = (texture(tex, tc + vec2(texel.x, 0.0)).rgb + texture(tex, tc - vec2(texel.x, 0.0)).rgb
                    + texture(tex, tc + vec2(0.0, texel.y)).rgb + texture(tex, tc - vec2(0.0, texel.y)).rgb) * 0.25;
            c += (c - b) * adj.w;
        }
        c = (c - 0.5) * adj.y + 0.5;
        c *= adj.x;
        float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
        c = mix(vec3(l), c, adj.z);
        c *= vec3(1.0 + warm * 0.08, 1.0 + warm * 0.01, 1.0 - warm * 0.1);
        c = clamp(c, 0.0, 1.0);
    }
    if (srgb > 0.5) c = pow(c, vec3(2.2));   // el video ya viene en sRGB: evitar aplicarlo dos veces
    color = vec4(c, keepAlpha > 0.5 ? t4.a : 1.0);
})";
    app.prog = glCreateProgram();
    glAttachShader(app.prog, compileShader(GL_VERTEX_SHADER, vs));
    glAttachShader(app.prog, compileShader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(app.prog);
    app.uOff = glGetUniformLocation(app.prog, "off");
    app.uSrgb = glGetUniformLocation(app.prog, "srgb");
    app.uTex = glGetUniformLocation(app.prog, "tex");
    app.uSx = glGetUniformLocation(app.prog, "sx");
    app.uSy = glGetUniformLocation(app.prog, "sy");
    app.uKeepA = glGetUniformLocation(app.prog, "keepAlpha");
    app.uFilt = glGetUniformLocation(app.prog, "filt");
    app.uRect = glGetUniformLocation(app.prog, "rect");
    app.uAdj = glGetUniformLocation(app.prog, "adj");
    app.uWarm = glGetUniformLocation(app.prog, "warm");
    app.uTexel = glGetUniformLocation(app.prog, "texel");
    app.uFlow = glGetUniformLocation(app.prog, "flow");
    app.uFlowK = glGetUniformLocation(app.prog, "flowK");
    app.uFovLut = glGetUniformLocation(app.prog, "fovLut"); app.uFovOn = glGetUniformLocation(app.prog, "fovOn"); app.uFovRow = glGetUniformLocation(app.prog, "fovRow");
    glUseProgram(app.prog);
    glUniform1i(app.uFovLut, 2);   // (cada tipo de textura en su unidad: la 0 es la del video)
    glUniform1f(app.uFovOn, 0.f);
    glUseProgram(0);
    // "flow" (sampler2D) siempre en la unidad 1: si quedara en la 0 junto a "tex" (externa), ningún dibujo con este programa funciona
    glUseProgram(app.prog); glUniform1i(app.uFlow, 1); glUniform2f(app.uFlowK, 0.f, 0.f); glUseProgram(0);
    glGenVertexArrays(1, &app.vao);
    for (GLuint *t : {&app.tex, &app.deskTex, &app.uiTex}) {
        glGenTextures(1, t);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, *t);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }

    // Rayos y puntero de los controles (capa transparente encima de los paneles)
    const char *lvs = R"(#version 300 es
layout(location = 0) in vec3 pos;
layout(location = 1) in vec2 uvIn;
uniform mat4 VP;
out vec2 uv;
void main() { uv = uvIn; gl_Position = VP * vec4(pos, 1.0); })";
    const char *lfs = R"(#version 300 es
precision mediump float;
uniform vec4 col;
uniform float roundShape;
in vec2 uv;
out vec4 color;
void main() {
    float a = col.a;
    if (roundShape > 1.5) {
        // barrita para mover la ventana (bordes suaves)
        a *= 1.0 - smoothstep(0.55, 1.0, abs(uv.y * 2.0 - 1.0));
        a *= 1.0 - smoothstep(0.93, 1.0, abs(uv.x * 2.0 - 1.0));
    } else if (roundShape > 0.5) {
        float r = length(uv - 0.5) * 2.0;
        if (r > 1.0) discard;
        a *= 1.0 - smoothstep(0.7, 1.0, r);
    } else {
        a *= 1.0 - 0.65 * uv.y;                       // se desvanece hacia la punta
        a *= 1.0 - smoothstep(0.6, 1.0, abs(uv.x * 2.0 - 1.0));
    }
    color = vec4(col.rgb * a, a);
})";
    app.lineProg = glCreateProgram();
    glAttachShader(app.lineProg, compileShader(GL_VERTEX_SHADER, lvs));
    glAttachShader(app.lineProg, compileShader(GL_FRAGMENT_SHADER, lfs));
    glLinkProgram(app.lineProg);
    app.uVP = glGetUniformLocation(app.lineProg, "VP");
    app.uCol = glGetUniformLocation(app.lineProg, "col");
    app.uRound = glGetUniformLocation(app.lineProg, "roundShape");
    glGenVertexArrays(1, &app.lineVao);
    glGenBuffers(1, &app.lineVbo);
    glBindVertexArray(app.lineVao);
    glBindBuffer(GL_ARRAY_BUFFER, app.lineVbo);
    glBufferData(GL_ARRAY_BUFFER, 20 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 5 * sizeof(float), (void *)(3 * sizeof(float)));
    glBindVertexArray(0);
}

/// Crea (o vuelve a crear) un swapchain con su framebuffer por imagen.
static bool createSwapchain(App &app, Swapchain &s, int w, int h, int mips) {
    if (s.handle != XR_NULL_HANDLE) {
        if (!s.fbos.empty()) glDeleteFramebuffers((GLsizei)s.fbos.size(), s.fbos.data());
        xrDestroySwapchain(s.handle);
        s = Swapchain();
    }
    XrSwapchainCreateInfo sw{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sw.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
    sw.format = app.scFormat; sw.sampleCount = 1; sw.width = (uint32_t)w; sw.height = (uint32_t)h;
    sw.faceCount = 1; sw.arraySize = 1; sw.mipCount = (uint32_t)mips;
    if (XR_FAILED(xrCreateSwapchain(app.session, &sw, &s.handle))) {
        if (mips == 1) { LOGE("No se pudo crear un swapchain %dx%d", w, h); s.handle = XR_NULL_HANDLE; return false; }
        return createSwapchain(app, s, w, h, 1);          // sin mipmaps
    }
    s.w = w; s.h = h; s.mips = mips;
    uint32_t ic = 0;
    xrEnumerateSwapchainImages(s.handle, 0, &ic, nullptr);
    s.images.assign(ic, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(s.handle, ic, &ic, (XrSwapchainImageBaseHeader *)s.images.data());
    s.fbos.resize(ic);
    glGenFramebuffers((GLsizei)ic, s.fbos.data());
    for (uint32_t i = 0; i < ic; i++) {
        glBindFramebuffer(GL_FRAMEBUFFER, s.fbos[i]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, s.images[i].image, 0);
        if (mips > 1) {
            glBindTexture(GL_TEXTURE_2D, s.images[i].image);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return true;
}

static int mipLevels(int w, int h) { int m = 1; while ((w | h) >> m) m++; return std::min(m, 6); }

static bool hasExt(const std::vector<XrExtensionProperties> &exts, const char *name) {
    for (auto &e : exts) if (strcmp(e.extensionName, name) == 0) return true;
    return false;
}

static XrPath path(App &app, const char *s) { XrPath p = XR_NULL_PATH; xrStringToPath(app.instance, s, &p); return p; }

static XrAction makeAction(App &app, const char *name, XrActionType type) {
    XrActionCreateInfo ci{XR_TYPE_ACTION_CREATE_INFO};
    strncpy(ci.actionName, name, XR_MAX_ACTION_NAME_SIZE - 1);
    strncpy(ci.localizedActionName, name, XR_MAX_LOCALIZED_ACTION_NAME_SIZE - 1);
    ci.actionType = type;
    ci.countSubactionPaths = 2;
    ci.subactionPaths = app.hand;
    XrAction a = XR_NULL_HANDLE;
    XRCHK(xrCreateAction(app.actionSet, &ci, &a));
    return a;
}

static void realHandsInit(App &app);   // (más abajo: manos reales)

static bool initXR(App &app) {
    PFN_xrInitializeLoaderKHR initLoader = nullptr;
    xrGetInstanceProcAddr(XR_NULL_HANDLE, "xrInitializeLoaderKHR", (PFN_xrVoidFunction *)&initLoader);
    if (initLoader) {
        XrLoaderInitInfoAndroidKHR li{XR_TYPE_LOADER_INIT_INFO_ANDROID_KHR};
        li.applicationVM = app.android->activity->vm;
        li.applicationContext = app.android->activity->clazz;
        initLoader((XrLoaderInitInfoBaseHeaderKHR *)&li);
    }
    uint32_t n = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
    std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
    xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());

    std::vector<const char *> enable = { XR_KHR_ANDROID_CREATE_INSTANCE_EXTENSION_NAME, XR_KHR_OPENGL_ES_ENABLE_EXTENSION_NAME };
    if (hasExt(exts, XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME)) { enable.push_back(XR_FB_DISPLAY_REFRESH_RATE_EXTENSION_NAME); app.hasRefresh = true; }
    // Supermuestreo del compositor para las capas con texto (lista y escritorio)
    if (hasExt(exts, XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME)) { enable.push_back(XR_FB_COMPOSITION_LAYER_SETTINGS_EXTENSION_NAME); app.hasLayerSettings = true; }
    // Ver tu cuarto real durante la transición de entrada y salida
    if (hasExt(exts, XR_FB_PASSTHROUGH_EXTENSION_NAME)) { enable.push_back(XR_FB_PASSTHROUGH_EXTENSION_NAME); app.hasPassthrough = true; }
    // Manos reales: seguimiento de manos de Meta, su malla, su rayo, y passthrough proyectado sobre una malla
    if (hasExt(exts, XR_FB_TRIANGLE_MESH_EXTENSION_NAME)) { enable.push_back(XR_FB_TRIANGLE_MESH_EXTENSION_NAME); app.hasTriMesh = true; }
    if (hasExt(exts, XR_EXT_HAND_TRACKING_EXTENSION_NAME)) { enable.push_back(XR_EXT_HAND_TRACKING_EXTENSION_NAME); app.hasHandTracking = true; }
    if (app.hasHandTracking && hasExt(exts, XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME)) { enable.push_back(XR_FB_HAND_TRACKING_MESH_EXTENSION_NAME); app.hasHandMesh = true; }
    if (app.hasHandTracking && hasExt(exts, XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME)) { enable.push_back(XR_FB_HAND_TRACKING_AIM_EXTENSION_NAME); app.hasHandAim = true; }
    // Forma de los controles (modelo de Meta), reloj de las cámaras y ver las manos también con los controles en la mano
    if (hasExt(exts, XR_FB_SPACE_WARP_EXTENSION_NAME)) { enable.push_back(XR_FB_SPACE_WARP_EXTENSION_NAME); app.hasSpaceWarp = true; }
    if (hasExt(exts, XR_FB_RENDER_MODEL_EXTENSION_NAME)) { enable.push_back(XR_FB_RENDER_MODEL_EXTENSION_NAME); app.hasRenderModel = true; }
    if (hasExt(exts, "XR_KHR_convert_timespec_time")) { enable.push_back("XR_KHR_convert_timespec_time"); app.hasTimespec = true; }
    if (app.hasHandTracking && hasExt(exts, "XR_META_simultaneous_hands_and_controllers")) { enable.push_back("XR_META_simultaneous_hands_and_controllers"); app.hasSimul = true; }
    // Ventanas curvas
    if (hasExt(exts, XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME)) { enable.push_back(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME); app.hasCylinder = true; }
    // Transparencia por capa (aparecer y desaparecer suave)
    if (hasExt(exts, XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME)) { enable.push_back(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME); app.hasColorScale = true; }

    XrInstanceCreateInfoAndroidKHR ai{XR_TYPE_INSTANCE_CREATE_INFO_ANDROID_KHR};
    ai.applicationVM = app.android->activity->vm;
    ai.applicationActivity = app.android->activity->clazz;
    XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
    ci.next = &ai;
    strcpy(ci.applicationInfo.applicationName, "QuestVRBridge");
    ci.applicationInfo.applicationVersion = 1;
    strcpy(ci.applicationInfo.engineName, "QuestVRBridge");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 34);
    ci.enabledExtensionCount = (uint32_t)enable.size();
    ci.enabledExtensionNames = enable.data();
    if (XR_FAILED(xrCreateInstance(&ci, &app.instance))) { LOGE("No se pudo crear la instancia OpenXR"); return false; }

    XrSystemGetInfo sg{XR_TYPE_SYSTEM_GET_INFO};
    sg.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    if (XR_FAILED(xrGetSystem(app.instance, &sg, &app.system))) { LOGE("No hay visor"); return false; }

    PFN_xrGetOpenGLESGraphicsRequirementsKHR getReq = nullptr;
    xrGetInstanceProcAddr(app.instance, "xrGetOpenGLESGraphicsRequirementsKHR", (PFN_xrVoidFunction *)&getReq);
    XrGraphicsRequirementsOpenGLESKHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    if (getReq) getReq(app.instance, app.system, &req);

    if (app.hasRefresh) {
        xrGetInstanceProcAddr(app.instance, "xrRequestDisplayRefreshRateFB", (PFN_xrVoidFunction *)&app.xrRequestDisplayRefreshRateFB);
        xrGetInstanceProcAddr(app.instance, "xrEnumerateDisplayRefreshRatesFB", (PFN_xrVoidFunction *)&app.xrEnumerateDisplayRefreshRatesFB);
    }

    XrGraphicsBindingOpenGLESAndroidKHR gb{XR_TYPE_GRAPHICS_BINDING_OPENGL_ES_ANDROID_KHR};
    gb.display = app.display;
    gb.config = app.config;
    gb.context = app.context;
    XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
    sci.next = &gb;
    sci.systemId = app.system;
    if (XR_FAILED(xrCreateSession(app.instance, &sci, &app.session))) { LOGE("No se pudo crear la sesión"); return false; }
    if (app.hasPassthrough) {
        PFN_xrCreatePassthroughFB cpt = nullptr;
        PFN_xrCreatePassthroughLayerFB cpl = nullptr;
        xrGetInstanceProcAddr(app.instance, "xrCreatePassthroughFB", (PFN_xrVoidFunction *)&cpt);
        xrGetInstanceProcAddr(app.instance, "xrCreatePassthroughLayerFB", (PFN_xrVoidFunction *)&cpl);
        xrGetInstanceProcAddr(app.instance, "xrPassthroughStartFB", (PFN_xrVoidFunction *)&app.ptStart);
        xrGetInstanceProcAddr(app.instance, "xrPassthroughPauseFB", (PFN_xrVoidFunction *)&app.ptPause);
        XrPassthroughCreateInfoFB pci{XR_TYPE_PASSTHROUGH_CREATE_INFO_FB};
        pci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
        if (!cpt || XR_FAILED(cpt(app.session, &pci, &app.pt))) app.hasPassthrough = false;
        else {
            XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
            lci.passthrough = app.pt;
            lci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
            lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_RECONSTRUCTION_FB;
            if (!cpl || XR_FAILED(cpl(app.session, &lci, &app.ptLayer))) app.hasPassthrough = false;
            else app.ptRunning = true;
        }
        LOGI("Passthrough para la transición: %s", app.hasPassthrough ? "sí" : "no");
    }

    XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_STAGE;
    rs.poseInReferenceSpace.orientation.w = 1;
    if (XR_FAILED(xrCreateReferenceSpace(app.session, &rs, &app.space))) {
        rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
        XRCHK(xrCreateReferenceSpace(app.session, &rs, &app.space));
    }
    app.stageSpace = rs.referenceSpaceType == XR_REFERENCE_SPACE_TYPE_STAGE;
    app.spaceType = rs.referenceSpaceType;
    {
        XrReferenceSpaceCreateInfo vs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
        vs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
        vs.poseInReferenceSpace.orientation.w = 1;
        if (XR_FAILED(xrCreateReferenceSpace(app.session, &vs, &app.viewSpace))) app.viewSpace = XR_NULL_HANDLE;
    }
    // Manos reales: después de crear el espacio de la sala (las formas se ubican respecto de él)
    realHandsInit(app);

    // Swapchains (uno por ojo)
    uint32_t vc = 0;
    xrEnumerateViewConfigurationViews(app.instance, app.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &vc, nullptr);
    std::vector<XrViewConfigurationView> views(vc, {XR_TYPE_VIEW_CONFIGURATION_VIEW});
    xrEnumerateViewConfigurationViews(app.instance, app.system, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, vc, &vc, views.data());
    uint32_t fc = 0;
    xrEnumerateSwapchainFormats(app.session, 0, &fc, nullptr);
    std::vector<int64_t> fmts(fc);
    xrEnumerateSwapchainFormats(app.session, fc, &fc, fmts.data());
    app.scFormat = GL_RGBA8;
    for (int64_t f : fmts) if (f == GL_SRGB8_ALPHA8) { app.scFormat = f; app.srgb = true; break; }
    for (int e = 0; e < 2 && e < (int)vc; e++) {
        int w = (int)views[e].recommendedImageRectWidth, h = (int)views[e].recommendedImageRectHeight;
        createSwapchain(app, app.sc[e], w, h, 1);
        createSwapchain(app, app.overlay[e], w, h, 1);   // rayos de los controles en la sala
    }
    LOGI("Swapchains %dx%d por ojo (%s)", app.sc[0].w, app.sc[0].h, app.srgb ? "sRGB" : "RGBA8");

    // Controles
    XrActionSetCreateInfo as{XR_TYPE_ACTION_SET_CREATE_INFO};
    strcpy(as.actionSetName, "main");
    strcpy(as.localizedActionSetName, "main");
    XRCHK(xrCreateActionSet(app.instance, &as, &app.actionSet));
    app.hand[0] = path(app, "/user/hand/left");
    app.hand[1] = path(app, "/user/hand/right");
    app.aGrip = makeAction(app, "grip", XR_ACTION_TYPE_POSE_INPUT);
    app.aAim = makeAction(app, "aim", XR_ACTION_TYPE_POSE_INPUT);
    app.aTrigger = makeAction(app, "trigger", XR_ACTION_TYPE_FLOAT_INPUT);
    app.aSqueeze = makeAction(app, "squeeze", XR_ACTION_TYPE_FLOAT_INPUT);
    app.aStick = makeAction(app, "stick", XR_ACTION_TYPE_VECTOR2F_INPUT);
    app.aStickClick = makeAction(app, "stick_click", XR_ACTION_TYPE_BOOLEAN_INPUT);
    app.aStickTouch = makeAction(app, "stick_touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
    app.aTrigTouch = makeAction(app, "trigger_touch", XR_ACTION_TYPE_BOOLEAN_INPUT);
    app.aA = makeAction(app, "button_a", XR_ACTION_TYPE_BOOLEAN_INPUT);
    app.aB = makeAction(app, "button_b", XR_ACTION_TYPE_BOOLEAN_INPUT);
    app.aATouch = makeAction(app, "touch_a", XR_ACTION_TYPE_BOOLEAN_INPUT);
    app.aBTouch = makeAction(app, "touch_b", XR_ACTION_TYPE_BOOLEAN_INPUT);
    app.aMenu = makeAction(app, "menu", XR_ACTION_TYPE_BOOLEAN_INPUT);

    std::vector<XrActionSuggestedBinding> b;
    auto sug = [&](XrAction act, const char *pth) { b.push_back({act, path(app, pth)}); };
    for (const char *h : {"left", "right"}) {
        std::string base = std::string("/user/hand/") + h;
        sug(app.aGrip, (base + "/input/grip/pose").c_str());
        sug(app.aAim, (base + "/input/aim/pose").c_str());
        sug(app.aTrigger, (base + "/input/trigger/value").c_str());
        sug(app.aTrigTouch, (base + "/input/trigger/touch").c_str());
        sug(app.aSqueeze, (base + "/input/squeeze/value").c_str());
        sug(app.aStick, (base + "/input/thumbstick").c_str());
        sug(app.aStickClick, (base + "/input/thumbstick/click").c_str());
        sug(app.aStickTouch, (base + "/input/thumbstick/touch").c_str());
    }
    sug(app.aA, "/user/hand/left/input/x/click");
    sug(app.aB, "/user/hand/left/input/y/click");
    sug(app.aATouch, "/user/hand/left/input/x/touch");
    sug(app.aBTouch, "/user/hand/left/input/y/touch");
    sug(app.aMenu, "/user/hand/left/input/menu/click");
    sug(app.aA, "/user/hand/right/input/a/click");
    sug(app.aB, "/user/hand/right/input/b/click");
    sug(app.aATouch, "/user/hand/right/input/a/touch");
    sug(app.aBTouch, "/user/hand/right/input/b/touch");
    XrInteractionProfileSuggestedBinding sb{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING};
    sb.interactionProfile = path(app, "/interaction_profiles/oculus/touch_controller");
    sb.countSuggestedBindings = (uint32_t)b.size();
    sb.suggestedBindings = b.data();
    XRCHK(xrSuggestInteractionProfileBindings(app.instance, &sb));
    XrSessionActionSetsAttachInfo at{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO};
    at.countActionSets = 1;
    at.actionSets = &app.actionSet;
    XRCHK(xrAttachSessionActionSets(app.session, &at));
    for (int h = 0; h < 2; h++) {
        XrActionSpaceCreateInfo sc{XR_TYPE_ACTION_SPACE_CREATE_INFO};
        sc.subactionPath = app.hand[h];
        sc.poseInActionSpace.orientation.w = 1;
        sc.action = app.aGrip; XRCHK(xrCreateActionSpace(app.session, &sc, &app.gripSpace[h]));
        sc.action = app.aAim;  XRCHK(xrCreateActionSpace(app.session, &sc, &app.aimSpace[h]));
    }
    return true;
}

// ---------------------------------------------------------------------------
// Manos reales. El seguimiento de manos de Meta da la posición de cada articulación y una malla con la forma de tu
// mano; el control tiene su modelo (también de Meta). Dos maneras de mostrarlos:
//  · Con la cámara (Passthrough Camera API): la imagen de la cámara del Quest se proyecta sobre esa malla y se
//    dibuja en nuestra capa, con el borde difuminado.
//  · De respaldo (sin permiso de cámara): passthrough proyectado de Meta sobre la malla (bordes duros).
// ---------------------------------------------------------------------------
static void rotate(const XrQuaternionf &q, const float v[3], float out[3]);

typedef XrResult (XRAPI_PTR *PFN_convTimespecToXr)(XrInstance, const struct timespec *, XrTime *);
struct SimulResumeInfo { XrStructureType type; const void *next; };
typedef XrResult (XRAPI_PTR *PFN_simulResume)(XrSession, const SimulResumeInfo *);

struct RealHands {
    bool ok = false;                 // hay passthrough proyectado (respaldo)
    bool tracking = false;           // hay seguimiento de manos
    XrPassthroughLayerFB layer = XR_NULL_HANDLE;
    PFN_xrCreateTriangleMeshFB createMesh = nullptr;
    PFN_xrTriangleMeshBeginVertexBufferUpdateFB beginVB = nullptr;
    PFN_xrTriangleMeshGetVertexBufferFB getVB = nullptr;
    PFN_xrTriangleMeshEndVertexBufferUpdateFB endVB = nullptr;
    PFN_xrTriangleMeshBeginUpdateFB beginUpd = nullptr;
    PFN_xrTriangleMeshGetIndexBufferFB getIB = nullptr;
    PFN_xrTriangleMeshEndUpdateFB endUpd = nullptr;
    PFN_xrCreateGeometryInstanceFB createGeo = nullptr;
    PFN_xrGeometryInstanceSetTransformFB setGeo = nullptr;
    PFN_xrCreateHandTrackerEXT createTracker = nullptr;
    PFN_xrLocateHandJointsEXT locate = nullptr;
    PFN_xrGetHandMeshFB getMesh = nullptr;
    PFN_xrEnumerateRenderModelPathsFB enumModels = nullptr;
    PFN_xrGetRenderModelPropertiesFB modelProps = nullptr;
    PFN_xrLoadRenderModelFB loadModel = nullptr;
    PFN_convTimespecToXr toXrTime = nullptr;
    bool hasAim = false;
    bool modelPaths = false;
    struct Hand {
        XrHandTrackerEXT tracker = XR_NULL_HANDLE;
        XrTriangleMeshFB mesh = XR_NULL_HANDLE;
        XrGeometryInstanceFB geo = XR_NULL_HANDLE;    // la mano (respaldo)
        XrGeometryInstanceFB ctl = XR_NULL_HANDLE;    // forma redondeada alrededor del control (si no hay modelo)
        std::vector<XrVector3f> basePos, baseNrm;     // malla de la mano en reposo y sus normales
        std::vector<XrVector4sFB> bi;
        std::vector<XrVector4f> bw;
        std::vector<XrPosef> bind;                    // pose de reposo de cada articulación
        std::vector<uint32_t> idx;
        bool active = false;                          // la mano se está siguiendo
        bool aimValid = false;                        // hay rayo para apuntar con la mano
        XrPosef aim{};
        float pinch = 0;
        bool handShown = false, ctlShown = false;     // (respaldo) formas visibles
        bool edgeOk = false;                          // borde de la palma del lado del meñique (para abrir el menú)
        float edge[3] = {0, 0, 0}, edgeDir[3] = {0, 0, 0};
        float palmN[3] = {0, 1, 0};                   // normal de la palma (el signo no importa)
        bool tipOk = false; float tip[3] = {0, 0, 0}; // punta del índice (para dibujar el portal)
        // Gestos como control (para los juegos): valores de 0 a 1 y botones
        float gTrig = 0, gGrip = 0, gPinchMid = 0, gPinchRing = 0, gPinchLittle = 0;
        bool gStickClick = false, gStickOn = false;
        float gStick[2] = {0, 0};
        float gPalm[3] = {0, 0, 0}, gStickOrigin[3] = {0, 0, 0};
        XrPosef gGripPose{};                          // pose de agarre como la de un control (para los juegos)
        bool gPoint = false, gShot = false;           // «pistola»: índice estirado y los otros tres cerrados · pulgar abajo
        float gPointO[3] = {0, 0, 0}, gPointD[3] = {0, 0, -1};   // punta del índice y hacia dónde apunta
        bool gGripOk = false;
        XrPosef palmPose{}; bool palmOk = false;      // la palma (para la calibración con los controles)
        // Gestos configurables (ver g_gm): valor de cada gesto (0 a 1), joystick virtual y el control que resulta
        float gv[9] = {0};
        bool joyOn = false; float joyAnchor[3] = {0, 0, 0};
        struct VCtl { float trig = 0, grip = 0, sx = 0, sy = 0; bool a = false, b = false, click = false, menu = false, joy = false; } v, vo;   // vo: lo que esta mano aprieta en el OTRO control (macros)
        // antebrazo (hasta la mitad): un tubo ovalado que sigue a la muñeca, con el final difuminado
        std::vector<float> armV;
        GLuint avbo = 0, aibo = 0;
        bool armDraw = false;
        float armP[3] = {0, 0, 0}, armD[3] = {0, 0, 1};
        // con la cámara
        std::vector<float> camV;                      // posición y normal de cada vértice, ya en la sala
        bool camDraw = false;
        GLuint vbo = 0, ibo = 0;
        // modelo del control (agrandado 1,5 mm)
        std::vector<float> cmPN;                      // posición y normal
        std::vector<uint32_t> cmIdx;
        bool cmLoaded = false;
        int cmTries = 0;
        XrTriangleMeshFB cmMesh = XR_NULL_HANDLE;
        XrGeometryInstanceFB cmGeo = XR_NULL_HANDLE;
        bool cmShown = false;
        GLuint cvbo = 0, cibo = 0;
        bool ctlCamDraw = false, ctlCamBlob = false;
        XrPosef ctlPose{};
    } h[2];
    XrTriangleMeshFB blob = XR_NULL_HANDLE;           // forma redondeada (si no hay modelo del control)
    std::vector<float> blobPN;                        // la misma, para dibujarla con la cámara
    std::vector<uint32_t> blobIdx;
    GLuint bvbo = 0, bibo = 0;
    int simul = 0;                                    // manos y controles a la vez: 0 no pedido, 1 sí, <0 error
};
static RealHands g_rh;
/// El control virtual de una mano: sus gestos + lo que las macros de la otra mano aprietan en este control
static RealHands::Hand::VCtl vctl(int h) {
    RealHands::Hand::VCtl v = g_rh.h[h].active ? g_rh.h[h].v : RealHands::Hand::VCtl{};
    const RealHands::Hand &o = g_rh.h[1 - h];
    if (o.active) {
        const RealHands::Hand::VCtl &x = o.vo;
        v.trig = std::max(v.trig, x.trig); v.grip = std::max(v.grip, x.grip);
        v.a |= x.a; v.b |= x.b; v.click |= x.click; v.menu |= x.menu;
    }
    return v;
}
static bool g_ctlInUse = false;               // hay un control en uso (entonces no se usan las manos)
static std::atomic<bool> g_handsOff{false};   // seguimiento de manos apagado del todo (solo controles)
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetHandTracking(JNIEnv *, jclass, jboolean on) { g_handsOff = !on; }
static std::atomic<bool> g_laserNoTouch{false};   // el láser no toca las pantallas (solo el menú)
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetLaserNoTouch(JNIEnv *, jclass, jboolean on) { g_laserNoTouch = on; }
static char g_rhStatus[240] = "sin iniciar";   // estado de las manos reales (va en el registro que se manda a la Mac)
static int g_rhErr = 0;                       // último error de OpenXR al prepararlas
static int g_rhE[5] = {0, 0, 0, 0, 0};        // errores por paso: forma del control, su instancia, malla de mano, su llenado, su instancia
static int g_rhSent = 0;                      // cuántas veces se mandó la capa de las manos (para el registro)
static std::atomic<int> g_camAllowed{-1};     // permiso de cámara del visor: -1 sin respuesta, 0 no, 1 sí
static bool g_camMode = false;                // este cuadro las manos se dibujan con la cámara
static XrTime g_camT = 0;                     // cuándo se tomó la última imagen de la cámara
static int g_camDrawn = 0;                    // cuadros dibujados con la cámara (para el registro)
static std::atomic<float> g_edgeSoft{0.6f};   // suavidad del borde de las manos (0 a 1)
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetEdgeSoft(JNIEnv *, jclass, jfloat v) { g_edgeSoft = std::max(0.f, std::min(1.f, (float)v)); }

/// Respuesta al permiso de cámara del visor (lo pide la parte Java)
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetCameraAllowed(JNIEnv *, jclass, jboolean on) {
    g_camAllowed = on ? 1 : 0;
    if (!on) snprintf(g_camStatus, sizeof g_camStatus, "sin permiso de cámara (se usa el passthrough de Meta)");
}

template <class T> static void xrProc(App &app, const char *name, T &fn) { xrGetInstanceProcAddr(app.instance, name, (PFN_xrVoidFunction *)&fn); }

/// Prepara el seguimiento de manos, sus mallas y el passthrough proyectado de respaldo (después de crear el espacio de la sala)
static void realHandsInit(App &app) {
    xrProc(app, "xrCreateTriangleMeshFB", g_rh.createMesh);
    xrProc(app, "xrTriangleMeshBeginVertexBufferUpdateFB", g_rh.beginVB);
    xrProc(app, "xrTriangleMeshGetVertexBufferFB", g_rh.getVB);
    xrProc(app, "xrTriangleMeshEndVertexBufferUpdateFB", g_rh.endVB);
    xrProc(app, "xrTriangleMeshBeginUpdateFB", g_rh.beginUpd);
    xrProc(app, "xrTriangleMeshGetIndexBufferFB", g_rh.getIB);
    xrProc(app, "xrTriangleMeshEndUpdateFB", g_rh.endUpd);
    xrProc(app, "xrCreateGeometryInstanceFB", g_rh.createGeo);
    xrProc(app, "xrGeometryInstanceSetTransformFB", g_rh.setGeo);
    if (app.hasRenderModel) {
        xrProc(app, "xrEnumerateRenderModelPathsFB", g_rh.enumModels);
        xrProc(app, "xrGetRenderModelPropertiesFB", g_rh.modelProps);
        xrProc(app, "xrLoadRenderModelFB", g_rh.loadModel);
    }
    if (app.hasTimespec) xrProc(app, "xrConvertTimespecTimeToTimeKHR", g_rh.toXrTime);

    // Forma redondeada para los controles (una esfera de radio 0,5 que se estira con la escala)
    std::vector<XrVector3f> bv;
    {
        const int ST = 10, SL = 16;
        bv.push_back({0, 0, 0.5f});                      // polo de arriba (0)
        for (int i = 1; i < ST; i++)
            for (int j = 0; j < SL; j++) {
                float t = 3.14159265f * i / ST, p = 6.2831853f * j / SL;
                bv.push_back({0.5f * sinf(t) * cosf(p), 0.5f * sinf(t) * sinf(p), 0.5f * cosf(t)});
            }
        bv.push_back({0, 0, -0.5f});                     // polo de abajo
        const uint32_t S = (uint32_t)bv.size() - 1;
        auto ring = [&](int i, int j) { return (uint32_t)(1 + (i - 1) * SL + (j % SL)); };
        std::vector<uint32_t> &idx = g_rh.blobIdx;
        for (int j = 0; j < SL; j++) idx.insert(idx.end(), {0u, ring(1, j + 1), ring(1, j)});
        for (int i = 1; i < ST - 1; i++)
            for (int j = 0; j < SL; j++) {
                uint32_t a = ring(i, j), b = ring(i, j + 1), c = ring(i + 1, j), d = ring(i + 1, j + 1);
                idx.insert(idx.end(), {a, b, c, b, d, c});
            }
        for (int j = 0; j < SL; j++) idx.insert(idx.end(), {S, ring(ST - 1, j), ring(ST - 1, j + 1)});
        for (auto &v : bv) { float l = sqrtf(v.x * v.x + v.y * v.y + v.z * v.z) * 2; g_rh.blobPN.insert(g_rh.blobPN.end(), {v.x, v.y, v.z, v.x / l, v.y / l, v.z / l}); }
    }

    // Passthrough proyectado de Meta (respaldo si no hay permiso de cámara)
    PFN_xrCreatePassthroughLayerFB cpl = nullptr;
    xrProc(app, "xrCreatePassthroughLayerFB", cpl);
    if (app.hasPassthrough && app.pt && app.hasTriMesh && cpl && g_rh.createMesh && g_rh.createGeo && g_rh.setGeo) {
        XrPassthroughLayerCreateInfoFB lci{XR_TYPE_PASSTHROUGH_LAYER_CREATE_INFO_FB};
        lci.passthrough = app.pt;
        lci.flags = XR_PASSTHROUGH_IS_RUNNING_AT_CREATION_BIT_FB;
        lci.purpose = XR_PASSTHROUGH_LAYER_PURPOSE_PROJECTED_FB;
        XrResult r = cpl(app.session, &lci, &g_rh.layer);
        if (XR_FAILED(r)) { g_rh.layer = XR_NULL_HANDLE; g_rhErr = (int)r; }
        else {
            XrTriangleMeshCreateInfoFB mci{XR_TYPE_TRIANGLE_MESH_CREATE_INFO_FB};
            mci.windingOrder = XR_WINDING_ORDER_UNKNOWN_FB;   // (como el ejemplo oficial de Meta)
            mci.vertexCount = (uint32_t)bv.size(); mci.vertexBuffer = bv.data();
            mci.triangleCount = (uint32_t)g_rh.blobIdx.size() / 3; mci.indexBuffer = g_rh.blobIdx.data();
            r = g_rh.createMesh(app.session, &mci, &g_rh.blob);
            if (XR_FAILED(r)) { g_rh.blob = XR_NULL_HANDLE; g_rhErr = (int)r; g_rhE[0] = (int)r; }
            g_rh.ok = true;
        }
    }
    auto makeGeo = [&](XrTriangleMeshFB mesh, XrGeometryInstanceFB &out, int step) {
        if (!g_rh.layer || !mesh) return;
        XrGeometryInstanceCreateInfoFB gci{XR_TYPE_GEOMETRY_INSTANCE_CREATE_INFO_FB};
        gci.layer = g_rh.layer; gci.mesh = mesh; gci.baseSpace = app.space;
        gci.pose.orientation.w = 1; gci.pose.position = {0, -1000, 0};   // escondida hasta que haga falta
        gci.scale = {1, 1, 1};
        XrResult r = g_rh.createGeo(app.session, &gci, &out);
        if (XR_FAILED(r)) { out = XR_NULL_HANDLE; g_rhErr = (int)r; g_rhE[step] = (int)r; }
    };
    for (int s = 0; s < 2; s++) makeGeo(g_rh.blob, g_rh.h[s].ctl, 1);

    // Seguimiento de manos y la malla de cada mano
    if (app.hasHandTracking) {
        xrProc(app, "xrCreateHandTrackerEXT", g_rh.createTracker);
        xrProc(app, "xrLocateHandJointsEXT", g_rh.locate);
        xrProc(app, "xrGetHandMeshFB", g_rh.getMesh);
        g_rh.hasAim = app.hasHandAim;
        for (int s = 0; s < 2 && g_rh.createTracker && g_rh.locate; s++) {
            RealHands::Hand &H = g_rh.h[s];
            XrHandTrackerCreateInfoEXT tci{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT};
            tci.hand = s == 0 ? XR_HAND_LEFT_EXT : XR_HAND_RIGHT_EXT;
            tci.handJointSet = XR_HAND_JOINT_SET_DEFAULT_EXT;
            {
                XrResult r = g_rh.createTracker(app.session, &tci, &H.tracker);
                if (XR_FAILED(r)) { H.tracker = XR_NULL_HANDLE; g_rhErr = (int)r; continue; }
            }
            g_rh.tracking = true;
            if (!g_rh.getMesh || !app.hasHandMesh) continue;
            XrHandTrackingMeshFB m{XR_TYPE_HAND_TRACKING_MESH_FB};
            {
                XrResult r = g_rh.getMesh(H.tracker, &m);
                if (XR_FAILED(r) || m.vertexCountOutput == 0) { g_rhErr = (int)r; continue; }
            }
            std::vector<XrPosef> bind(m.jointCountOutput);
            std::vector<float> radii(m.jointCountOutput);
            std::vector<XrHandJointEXT> parents(m.jointCountOutput);
            std::vector<XrVector3f> pos(m.vertexCountOutput), nrm(m.vertexCountOutput);
            std::vector<XrVector2f> uv(m.vertexCountOutput);
            std::vector<XrVector4sFB> bi(m.vertexCountOutput);
            std::vector<XrVector4f> bw(m.vertexCountOutput);
            std::vector<int16_t> ind(m.indexCountOutput);
            m.jointCapacityInput = m.jointCountOutput; m.jointBindPoses = bind.data(); m.jointRadii = radii.data(); m.jointParents = parents.data();
            m.vertexCapacityInput = m.vertexCountOutput; m.vertexPositions = pos.data(); m.vertexNormals = nrm.data(); m.vertexUVs = uv.data();
            m.vertexBlendIndices = bi.data(); m.vertexBlendWeights = bw.data();
            m.indexCapacityInput = m.indexCountOutput; m.indices = ind.data();
            if (XR_FAILED(g_rh.getMesh(H.tracker, &m))) continue;
            std::vector<uint32_t> idx;
            for (int16_t k : ind) idx.push_back((uint32_t)(uint16_t)k);
            H.basePos = pos; H.baseNrm = nrm; H.bi = bi; H.bw = bw; H.bind = bind; H.idx = idx;
            if (!g_rh.layer) continue;
            // Respaldo: malla que cambia en cada cuadro (se crea vacía y se llena aparte)
            XrTriangleMeshCreateInfoFB mci{XR_TYPE_TRIANGLE_MESH_CREATE_INFO_FB};
            mci.flags = XR_TRIANGLE_MESH_MUTABLE_BIT_FB;
            mci.windingOrder = XR_WINDING_ORDER_UNKNOWN_FB;
            mci.vertexCount = (uint32_t)pos.size(); mci.vertexBuffer = nullptr;
            mci.triangleCount = (uint32_t)idx.size() / 3; mci.indexBuffer = nullptr;
            {
                XrResult r = g_rh.createMesh(app.session, &mci, &H.mesh);
                if (XR_FAILED(r)) { H.mesh = XR_NULL_HANDLE; g_rhErr = (int)r; g_rhE[2] = (int)r; continue; }
            }
            {
                XrVector3f *vb = nullptr;
                uint32_t *ib = nullptr;
                XrResult r = g_rh.beginUpd ? g_rh.beginUpd(H.mesh) : XR_ERROR_FUNCTION_UNSUPPORTED;
                if (XR_SUCCEEDED(r) && g_rh.getVB) r = g_rh.getVB(H.mesh, &vb);
                if (XR_SUCCEEDED(r) && g_rh.getIB) r = g_rh.getIB(H.mesh, &ib);
                if (XR_SUCCEEDED(r) && vb && ib) {
                    for (size_t i = 0; i < pos.size(); i++) vb[i] = pos[i];
                    for (size_t i = 0; i < idx.size(); i++) ib[i] = idx[i];
                    r = g_rh.endUpd(H.mesh, (uint32_t)pos.size(), (uint32_t)idx.size() / 3);
                }
                if (XR_FAILED(r) || !vb || !ib) { g_rhErr = (int)r; g_rhE[3] = XR_FAILED(r) ? (int)r : -99; continue; }
            }
            makeGeo(H.mesh, H.geo, 4);
        }
    }
    snprintf(g_rhStatus, sizeof g_rhStatus, "respaldo de Meta %s · seguimiento %s (ext %d, malla %d, rayo %d) · mallas de manos %s%s · modelos de control %s · errores %d/%d/%d/%d/%d",
             g_rh.ok ? "sí" : "no", g_rh.tracking ? "sí" : "no",
             (int)app.hasHandTracking, (int)app.hasHandMesh, (int)app.hasHandAim,
             g_rh.h[0].basePos.empty() ? "-" : "I", g_rh.h[1].basePos.empty() ? "-" : "D", app.hasRenderModel ? "pedibles" : "no",
             g_rhE[0], g_rhE[1], g_rhE[2], g_rhE[3], g_rhE[4]);
    LOGI("Manos reales: %s", g_rhStatus);
}

/// Modelo de cada control (lo da Meta como .glb): se intenta cada tanto hasta tenerlo
static void ctlModelsTry(App &app) {
    if (!g_rh.enumModels || !g_rh.modelProps || !g_rh.loadModel) return;
    static double next = 0;
    double now = nowMs();
    if (now < next) return;
    next = now + 1500;
    if (g_rh.h[0].cmLoaded && g_rh.h[1].cmLoaded) return;
    uint32_t n = 0;
    if (XR_FAILED(g_rh.enumModels(app.session, 0, &n, nullptr)) || !n) return;
    std::vector<XrRenderModelPathInfoFB> paths(n, {XR_TYPE_RENDER_MODEL_PATH_INFO_FB});
    if (XR_FAILED(g_rh.enumModels(app.session, n, &n, paths.data()))) return;
    for (int s = 0; s < 2; s++) {
        RealHands::Hand &H = g_rh.h[s];
        if (H.cmLoaded || H.cmTries > 20) continue;
        H.cmTries++;
        const char *want = s == 0 ? "/model_fb/controller/left" : "/model_fb/controller/right";
        for (auto &pi : paths) {
            char buf[256]; uint32_t len = 0;
            if (XR_FAILED(xrPathToString(app.instance, pi.path, sizeof buf, &len, buf)) || strncmp(buf, want, strlen(want))) continue;
            XrRenderModelPropertiesFB prop{XR_TYPE_RENDER_MODEL_PROPERTIES_FB};
            if (XR_FAILED(g_rh.modelProps(app.session, pi.path, &prop)) || prop.modelKey == XR_NULL_RENDER_MODEL_KEY_FB) continue;
            XrRenderModelLoadInfoFB li{XR_TYPE_RENDER_MODEL_LOAD_INFO_FB};
            li.modelKey = prop.modelKey;
            XrRenderModelBufferFB rb{XR_TYPE_RENDER_MODEL_BUFFER_FB};
            if (XR_FAILED(g_rh.loadModel(app.session, &li, &rb)) || !rb.bufferCountOutput) continue;
            std::vector<uint8_t> glb(rb.bufferCountOutput);
            rb.buffer = glb.data(); rb.bufferCapacityInput = (uint32_t)glb.size();
            if (XR_FAILED(g_rh.loadModel(app.session, &li, &rb))) continue;
            std::vector<float> p, gp, gn; std::vector<uint32_t> idx;
            if (!glbLoad(glb, p, idx)) { sendLog("Modelo del control %s: no se pudo leer (%u bytes)", s ? "der." : "izq.", (unsigned)glb.size()); continue; }
            smoothGrow(p, idx, 0.0015f, gp, gn);   // 1,5 mm más grande que el control
            H.cmPN.clear();
            for (size_t i = 0; i < gp.size() / 3; i++) H.cmPN.insert(H.cmPN.end(), {gp[i * 3], gp[i * 3 + 1], gp[i * 3 + 2], gn[i * 3], gn[i * 3 + 1], gn[i * 3 + 2]});
            H.cmIdx = idx;
            H.cmLoaded = true;
            // para el respaldo de Meta: una malla fija con esa forma
            if (g_rh.layer && g_rh.createMesh && g_rh.createGeo) {
                std::vector<XrVector3f> v(gp.size() / 3);
                for (size_t i = 0; i < v.size(); i++) v[i] = {gp[i * 3], gp[i * 3 + 1], gp[i * 3 + 2]};
                XrTriangleMeshCreateInfoFB mci{XR_TYPE_TRIANGLE_MESH_CREATE_INFO_FB};
                mci.windingOrder = XR_WINDING_ORDER_UNKNOWN_FB;
                mci.vertexCount = (uint32_t)v.size(); mci.vertexBuffer = v.data();
                mci.triangleCount = (uint32_t)idx.size() / 3; mci.indexBuffer = idx.data();
                if (XR_SUCCEEDED(g_rh.createMesh(app.session, &mci, &H.cmMesh))) {
                    XrGeometryInstanceCreateInfoFB gci{XR_TYPE_GEOMETRY_INSTANCE_CREATE_INFO_FB};
                    gci.layer = g_rh.layer; gci.mesh = H.cmMesh; gci.baseSpace = app.space;
                    gci.pose.orientation.w = 1; gci.pose.position = {0, -1000, 0};
                    gci.scale = {1, 1, 1};
                    if (XR_FAILED(g_rh.createGeo(app.session, &gci, &H.cmGeo))) H.cmGeo = XR_NULL_HANDLE;
                } else H.cmMesh = XR_NULL_HANDLE;
            }
            sendLog("Modelo del control %s: %u vértices, %u triángulos", s ? "der." : "izq.", (unsigned)(gp.size() / 3), (unsigned)(idx.size() / 3));
            break;
        }
    }
}

static void qmul(const XrQuaternionf &q, const float v[3], float o[3]) { rotate(q, v, o); }
static XrQuaternionf qconj(const XrQuaternionf &q) { return {-q.x, -q.y, -q.z, q.w}; }

static void geoPose(XrGeometryInstanceFB g, XrSpace space, XrTime t, const XrPosef &p, XrVector3f scale) {
    if (!g || !g_rh.setGeo) return;
    XrGeometryInstanceTransformFB tf{XR_TYPE_GEOMETRY_INSTANCE_TRANSFORM_FB};
    tf.baseSpace = space; tf.time = t; tf.pose = p; tf.scale = scale;
    g_rh.setGeo(g, &tf);
}
static void geoHide(XrGeometryInstanceFB g, XrSpace space, XrTime t, bool &shown) {
    if (!shown) return;
    XrPosef away{}; away.orientation.w = 1; away.position = {0, -1000, 0};
    geoPose(g, space, t, away, {1, 1, 1});
    shown = false;
}

/// Deforma la malla de la mano con sus articulaciones (pose actual · inversa de la de reposo).
/// outPos: solo posiciones (agrandadas `grow`), outPN: posición y normal intercaladas. Todo ya en la sala.
static bool skinHand(const RealHands::Hand &H, const XrHandJointLocationEXT *jl, float grow, XrVector3f *outPos, float *outPN) {
    for (int j = 0; j < XR_HAND_JOINT_COUNT_EXT; j++)
        if (!(jl[j].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) || !(jl[j].locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) return false;
    float M[XR_HAND_JOINT_COUNT_EXT][12];
    size_t nj = std::min((size_t)XR_HAND_JOINT_COUNT_EXT, H.bind.size());
    for (size_t j = 0; j < nj; j++) {
        XrQuaternionf ib = qconj(H.bind[j].orientation);
        const XrQuaternionf &a = jl[j].pose.orientation, &b = ib;
        XrQuaternionf q{a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
                        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
                        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
        float R[9]; quatToMat(q, R);
        const float bp[3] = {H.bind[j].position.x, H.bind[j].position.y, H.bind[j].position.z};
        float rb[3]; for (int r = 0; r < 3; r++) rb[r] = R[r] * bp[0] + R[3 + r] * bp[1] + R[6 + r] * bp[2];
        for (int k = 0; k < 9; k++) M[j][k] = R[k];
        M[j][9] = jl[j].pose.position.x - rb[0]; M[j][10] = jl[j].pose.position.y - rb[1]; M[j][11] = jl[j].pose.position.z - rb[2];
    }
    size_t vc = H.basePos.size();
    for (size_t i = 0; i < vc; i++) {
        const XrVector3f &bn = H.baseNrm[i];
        const XrVector3f p{H.basePos[i].x + bn.x * grow, H.basePos[i].y + bn.y * grow, H.basePos[i].z + bn.z * grow};
        const int16_t idx[4] = {H.bi[i].x, H.bi[i].y, H.bi[i].z, H.bi[i].w};
        const float w[4] = {H.bw[i].x, H.bw[i].y, H.bw[i].z, H.bw[i].w};
        float o[3] = {0, 0, 0}, n[3] = {0, 0, 0};
        for (int k = 0; k < 4; k++) {
            if (w[k] <= 0 || idx[k] < 0 || (size_t)idx[k] >= nj) continue;
            const float *m = M[idx[k]];
            for (int r = 0; r < 3; r++) {
                o[r] += (m[r] * p.x + m[3 + r] * p.y + m[6 + r] * p.z + m[9 + r]) * w[k];
                n[r] += (m[r] * bn.x + m[3 + r] * bn.y + m[6 + r] * bn.z) * w[k];
            }
        }
        if (outPos) outPos[i] = {o[0], o[1], o[2]};
        if (outPN) {
            float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (l < 1e-6f) l = 1;
            float *d = outPN + i * 6;
            d[0] = o[0]; d[1] = o[1]; d[2] = o[2]; d[3] = n[0] / l; d[4] = n[1] / l; d[5] = n[2] / l;
        }
    }
    return true;
}

static void hideHand(RealHands::Hand &H, XrSpace space, XrTime t) {
    geoHide(H.geo, space, t, H.handShown);
}

/// Antebrazo hasta la mitad (unos 13 cm desde la muñeca): tubo ovalado, más ancho hacia el codo
static void buildForearm(RealHands::Hand &H, const XrHandJointLocationEXT *jl) {
    H.armDraw = false;
    const XrHandJointLocationEXT &wr = jl[XR_HAND_JOINT_WRIST_EXT], &mp = jl[XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT],
                                 &im = jl[XR_HAND_JOINT_INDEX_METACARPAL_EXT], &lm = jl[XR_HAND_JOINT_LITTLE_METACARPAL_EXT];
    float d[3] = {wr.pose.position.x - mp.pose.position.x, wr.pose.position.y - mp.pose.position.y, wr.pose.position.z - mp.pose.position.z};
    float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dl < 1e-3f) return;
    for (float &k : d) k /= dl;                               // hacia el codo
    float x[3] = {lm.pose.position.x - im.pose.position.x, lm.pose.position.y - im.pose.position.y, lm.pose.position.z - im.pose.position.z};
    float xd = x[0] * d[0] + x[1] * d[1] + x[2] * d[2];
    for (int k = 0; k < 3; k++) x[k] -= d[k] * xd;
    float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    if (xl < 1e-4f) return;
    for (float &k : x) k /= xl;                               // ancho del brazo
    float y[3] = {d[1] * x[2] - d[2] * x[1], d[2] * x[0] - d[0] * x[2], d[0] * x[1] - d[1] * x[0]};   // grosor
    const int N = 8, S = 18;
    const float L = 0.06f, start = -0.025f;
    H.armV.resize(N * S * 6);
    const float w0[3] = {wr.pose.position.x, wr.pose.position.y, wr.pose.position.z};
    for (int i = 0; i < N; i++) {
        float t = (float)i / (N - 1);
        float along = start + t * (L - start);
        float tt = std::max(0.f, along / L);
        float a = 0.029f + 0.003f * tt, b = 0.020f + 0.0035f * tt;   // semiejes (ancho y grosor)
        for (int j = 0; j < S; j++) {
            float ang = 6.2831853f * j / S, c = cosf(ang), sn = sinf(ang);
            float *v = &H.armV[(i * S + j) * 6];
            float nn[3];
            for (int k = 0; k < 3; k++) {
                v[k] = w0[k] + d[k] * along + x[k] * a * c + y[k] * b * sn;
                nn[k] = x[k] * c / a + y[k] * sn / b;
            }
            float nl = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]);
            for (int k = 0; k < 3; k++) v[3 + k] = nn[k] / nl;
        }
    }
    if (!H.avbo) {
        std::vector<uint32_t> idx;
        for (int i = 0; i < N - 1; i++)
            for (int j = 0; j < S; j++) {
                uint32_t a0 = i * S + j, a1 = i * S + (j + 1) % S, b0 = (i + 1) * S + j, b1 = (i + 1) * S + (j + 1) % S;
                idx.insert(idx.end(), {a0, a1, b0, a1, b1, b0});
            }
        glGenBuffers(1, &H.avbo); glGenBuffers(1, &H.aibo);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, H.aibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * 4, idx.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
    glBindBuffer(GL_ARRAY_BUFFER, H.avbo);
    glBufferData(GL_ARRAY_BUFFER, H.armV.size() * 4, H.armV.data(), GL_STREAM_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    for (int k = 0; k < 3; k++) { H.armP[k] = w0[k]; H.armD[k] = d[k]; }
    H.armDraw = true;
}

// ---------------------------------------------------------------------------
// Gestos configurables: cada gesto de cada mano activa uno o varios botones (macro), y un joystick virtual,
// con un perfil para la sala VR y otro para los juegos (se eligen en el menú, con dibujos)
// ---------------------------------------------------------------------------
enum { G_PI, G_PM, G_PR, G_PL, G_FIST, G_GRAB, G_POINT, G_THUMBPALM, G_THUMBUP, G_N };
enum { A_TRIG = 1, A_GRIP = 2, A_A = 4, A_B = 8, A_CLICK = 16, A_MENU = 32 };
// [perfil 0 sala / 1 juego][mano 0 izq / 1 der][gesto … , G_N = modo del joystick (0 nada, 1 puño + pulgar,
//  2 pellizco del mayor y arrastrar como VRChat, 3 puño y mover la mano)]
static int g_gm[2][2][G_N + 1] = {
    {{0, 0, 0, A_MENU, 0, 0, 0, 0, 0, 1}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 1}},
    {{A_TRIG, A_B, A_A, A_MENU, 0, A_GRIP, 0, A_CLICK, 0, 1}, {A_TRIG, A_B, A_A, 0, 0, A_GRIP, 0, A_CLICK, 0, 1}}};
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetGestureMap(JNIEnv *env, jclass, jint profile, jint hand, jintArray arr) {
    if (profile < 0 || profile > 1 || hand < 0 || hand > 1 || !arr) return;
    jsize n = env->GetArrayLength(arr);
    jint *v = env->GetIntArrayElements(arr, nullptr);
    for (int i = 0; i < std::min((int)n, G_N + 1); i++) g_gm[profile][hand][i] = v[i];
    env->ReleaseIntArrayElements(arr, v, JNI_ABORT);
}

/// Gestos de la mano → botones de un control (como ALVR):
///  pellizco índice = gatillo · cerrar mayor, anular y meñique = agarre · pellizco mayor = B/Y · pellizco anular = A/X
///  pellizco meñique (izquierda) = menú · pulgar sobre la palma = apretar el joystick
///  joystick: puño cerrado (los cuatro dedos) con el pulgar encima → mover la mano desde ahí es mover el joystick
// Calibrar las manos para jugar con ellas: con los controles en la mano, el visor sigue a la vez las manos y los
// controles (por un momento). Lo que hay de la palma a cada control (giro y lugar, del agarre y del rayo) queda
// guardado por mano: después, sin controles, el juego recibe tu mano como si tuviera el control agarrado.
struct HandMap { bool ok = false; XrQuaternionf g{0, 0, 0, 1}, a{0, 0, 0, 1}; float gp[3] = {0, 0, 0}, ap[3] = {0, 0, 0}; };
static std::mutex g_handCalLock;
static HandMap g_hmap[2];
static std::atomic<bool> g_handCalReq{false};
static double g_handCalStart = 0;     // >0: midiendo desde este momento
static float g_calQ[2][2][4], g_calP[2][2][3]; static int g_calN[2];
static int g_handCalStatus = 0;       // -1 midiendo · bit 0: izquierda lista · bit 1: derecha lista · 8: el visor no puede las dos cosas a la vez
static XrQuaternionf qmulq(const XrQuaternionf &a, const XrQuaternionf &b) {
    return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y, a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w, a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
struct SimulPauseInfo { XrStructureType type; const void *next; };
typedef XrResult (XRAPI_PTR *PFN_simulPause)(XrSession, const SimulPauseInfo *);
static bool locateJoints(RealHands::Hand &H, XrSpace space, XrTime t, XrHandJointLocationEXT *jl, XrHandTrackingAimStateFB *aim);
/// (hilo de dibujo, cada cuadro) la calibración: prende el seguimiento de manos y controles a la vez, mide y lo apaga
static void handCalTick(App &app, XrTime t) {
    auto simul = [&](bool on) -> bool {
        if (!app.hasSimul) return false;
        PFN_xrVoidFunction f = nullptr;
        xrGetInstanceProcAddr(app.instance, on ? "xrResumeSimultaneousHandsAndControllersTrackingMETA" : "xrPauseSimultaneousHandsAndControllersTrackingMETA", &f);
        if (!f) return false;
        const SimulResumeInfo ri{(XrStructureType)1000532002, nullptr};
        const SimulPauseInfo pi{(XrStructureType)1000532003, nullptr};
        XrResult r = on ? ((PFN_simulResume)f)(app.session, &ri) : ((PFN_simulPause)f)(app.session, &pi);
        g_rh.simul = XR_SUCCEEDED(r) ? (on ? 1 : 0) : (int)r;
        return XR_SUCCEEDED(r);
    };
    if (g_handCalReq.exchange(false)) {
        std::lock_guard<std::mutex> lk(g_handCalLock);
        memset(g_calQ, 0, sizeof g_calQ); memset(g_calP, 0, sizeof g_calP); g_calN[0] = g_calN[1] = 0;
        if (simul(true)) { g_handCalStart = nowMs(); g_handCalStatus = -1; }
        else { g_handCalStart = 0; g_handCalStatus = 8; sendLog("Calibrar las manos: el visor no siguió manos y controles a la vez (%d)", g_rh.simul); }
        return;
    }
    if (g_handCalStart <= 0) return;
    const double el = nowMs() - g_handCalStart;
    if (el > 400 && el < 2200) {   // (al principio el visor todavía está empezando a seguir las manos)
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        for (int h = 0; h < 2; h++) {
            RealHands::Hand &H = g_rh.h[h];
            if (!H.tracker || !g_rh.locate) continue;
            XrHandJointLocationEXT jl[XR_HAND_JOINT_COUNT_EXT];
            if (!locateJoints(H, app.space, t, jl, nullptr) || (jl[XR_HAND_JOINT_PALM_EXT].locationFlags & need) != need) continue;
            XrSpaceLocation g{XR_TYPE_SPACE_LOCATION}, a{XR_TYPE_SPACE_LOCATION};
            if (XR_FAILED(xrLocateSpace(app.gripSpace[h], app.space, t, &g)) || (g.locationFlags & need) != need) continue;
            if (XR_FAILED(xrLocateSpace(app.aimSpace[h], app.space, t, &a)) || (a.locationFlags & need) != need) continue;
            const XrPosef &pl = jl[XR_HAND_JOINT_PALM_EXT].pose;
            const XrQuaternionf ip = qconj(pl.orientation);
            const XrPosef *ps[2] = {&g.pose, &a.pose};
            std::lock_guard<std::mutex> lk(g_handCalLock);
            for (int k = 0; k < 2; k++) {
                XrQuaternionf c = qmulq(ip, ps[k]->orientation);
                float *q = g_calQ[h][k];
                if (q[0] * c.x + q[1] * c.y + q[2] * c.z + q[3] * c.w < 0) { c.x = -c.x; c.y = -c.y; c.z = -c.z; c.w = -c.w; }
                q[0] += c.x; q[1] += c.y; q[2] += c.z; q[3] += c.w;
                const float d[3] = {ps[k]->position.x - pl.position.x, ps[k]->position.y - pl.position.y, ps[k]->position.z - pl.position.z};
                float o[3]; rotate(ip, d, o);
                for (int i = 0; i < 3; i++) g_calP[h][k][i] += o[i];
            }
            g_calN[h]++;
        }
    }
    if (el >= 2200) {
        simul(false);
        std::lock_guard<std::mutex> lk(g_handCalLock);
        g_handCalStart = 0;
        int st = 0;
        for (int h = 0; h < 2; h++) {
            if (g_calN[h] < 20) continue;   // esa mano no se vio bien con su control: queda como estaba
            HandMap m; m.ok = true;
            XrQuaternionf *qs[2] = {&m.g, &m.a};
            float *pv[2] = {m.gp, m.ap};
            for (int k = 0; k < 2; k++) {
                const float *q = g_calQ[h][k];
                const float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
                if (l < 1e-4f) { m.ok = false; break; }
                *qs[k] = {q[0] / l, q[1] / l, q[2] / l, q[3] / l};
                for (int i = 0; i < 3; i++) pv[k][i] = g_calP[h][k][i] / g_calN[h];
            }
            if (m.ok) { g_hmap[h] = m; st |= 1 << h; }
        }
        g_handCalStatus = st;
        sendLog("Calibrar las manos: listo (izquierda %s, derecha %s)", (st & 1) ? "sí" : "no", (st & 2) ? "sí" : "no");
    }
}
/// La pose de una mano para el juego según la calibración (k 0: agarre, 1: rayo). false: esa mano no está calibrada
static bool handCalApply(int h, int k, const RealHands::Hand &H, XrPosef &p) {
    if (!H.palmOk) return false;
    std::lock_guard<std::mutex> lk(g_handCalLock);
    const HandMap &m = g_hmap[h];
    if (!m.ok) return false;
    p.orientation = qmulq(H.palmPose.orientation, k == 0 ? m.g : m.a);
    float o[3]; rotate(H.palmPose.orientation, k == 0 ? m.gp : m.ap, o);
    p.position = {H.palmPose.position.x + o[0], H.palmPose.position.y + o[1], H.palmPose.position.z + o[2]};
    return true;
}
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeHandCalibrate(JNIEnv *, jclass) {
    std::lock_guard<std::mutex> lk(g_handCalLock);
    g_handCalStatus = -1;
    g_handCalReq = true;
}
/// [estado, y por mano (izquierda, derecha): ok, agarre x y z w, px py pz, rayo x y z w, px py pz]; estado -1 mientras mide
extern "C" JNIEXPORT jfloatArray JNICALL
Java_org_questvrbridge_client_VrActivity_nativeHandCalResult(JNIEnv *e, jclass) {
    std::lock_guard<std::mutex> lk(g_handCalLock);
    float o[31] = {(float)g_handCalStatus};
    for (int h = 0; h < 2; h++) {
        const HandMap &m = g_hmap[h];
        float *v = o + 1 + h * 15;
        v[0] = m.ok ? 1.f : 0.f;
        v[1] = m.g.x; v[2] = m.g.y; v[3] = m.g.z; v[4] = m.g.w; v[5] = m.gp[0]; v[6] = m.gp[1]; v[7] = m.gp[2];
        v[8] = m.a.x; v[9] = m.a.y; v[10] = m.a.z; v[11] = m.a.w; v[12] = m.ap[0]; v[13] = m.ap[1]; v[14] = m.ap[2];
    }
    jfloatArray a = e->NewFloatArray(31);
    e->SetFloatArrayRegion(a, 0, 31, o);
    return a;
}
/// Lo guardado (30 valores, como en el resultado sin el estado); vacío = sin calibrar
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetHandCal(JNIEnv *e, jclass, jfloatArray a) {
    std::lock_guard<std::mutex> lk(g_handCalLock);
    g_hmap[0] = HandMap{}; g_hmap[1] = HandMap{};
    if (!a || e->GetArrayLength(a) < 30) return;
    float o[30]; e->GetFloatArrayRegion(a, 0, 30, o);
    for (int h = 0; h < 2; h++) {
        const float *v = o + h * 15;
        if (v[0] < 0.5f) continue;
        HandMap m; m.ok = true;
        const float lg = sqrtf(v[1] * v[1] + v[2] * v[2] + v[3] * v[3] + v[4] * v[4]), la = sqrtf(v[8] * v[8] + v[9] * v[9] + v[10] * v[10] + v[11] * v[11]);
        if (lg < 1e-4f || la < 1e-4f) continue;
        m.g = {v[1] / lg, v[2] / lg, v[3] / lg, v[4] / lg}; m.gp[0] = v[5]; m.gp[1] = v[6]; m.gp[2] = v[7];
        m.a = {v[8] / la, v[9] / la, v[10] / la, v[11] / la}; m.ap[0] = v[12]; m.ap[1] = v[13]; m.ap[2] = v[14];
        g_hmap[h] = m;
    }
}

static void handGestures(RealHands::Hand &H, int side, const XrHandJointLocationEXT *jl, const XrHandTrackingAimStateFB &aim, const float headFwd[3]) {
    auto P = [&](int j) { return &jl[j].pose.position.x; };
    auto dist = [](const float *a, const float *b) { float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]); };
    const float *palm = P(XR_HAND_JOINT_PALM_EXT);
    for (int k = 0; k < 3; k++) H.gPalm[k] = palm[k];
    // qué tan cerrado está cada dedo: punta cerca de la palma = 1
    auto curl = [&](int tip) { return std::min(1.f, std::max(0.f, 1.f - (dist(P(tip), palm) - 0.035f) / 0.05f)); };
    float cIdx = curl(XR_HAND_JOINT_INDEX_TIP_EXT), cMid = curl(XR_HAND_JOINT_MIDDLE_TIP_EXT),
          cRing = curl(XR_HAND_JOINT_RING_TIP_EXT), cLit = curl(XR_HAND_JOINT_LITTLE_TIP_EXT);
    float three = (cMid + cRing + cLit) / 3.f;
    float thumbToIdx = dist(P(XR_HAND_JOINT_THUMB_TIP_EXT), P(XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT));
    float thumbToPalm = dist(P(XR_HAND_JOINT_THUMB_TIP_EXT), palm);
    bool fist = three > 0.75f && cIdx > 0.7f;
    bool stick = fist && thumbToIdx < 0.03f;
    if (stick && !H.gStickOn) for (int k = 0; k < 3; k++) H.gStickOrigin[k] = palm[k];
    H.gStickOn = stick;
    if (stick) {
        // derecha y adelante según hacia dónde mirás (en el piso)
        float f[2] = {headFwd[0], headFwd[2]}, fl = sqrtf(f[0] * f[0] + f[1] * f[1]);
        if (fl < 1e-3f) { f[0] = 0; f[1] = -1; fl = 1; }
        f[0] /= fl; f[1] /= fl;
        float r[2] = {-f[1], f[0]};
        float d[2] = {palm[0] - H.gStickOrigin[0], palm[2] - H.gStickOrigin[2]};
        auto ax = [](float v) { v /= 0.06f; if (fabsf(v) < 0.15f) return 0.f; return std::max(-1.f, std::min(1.f, v)); };
        H.gStick[0] = ax(d[0] * r[0] + d[1] * r[1]);
        H.gStick[1] = ax(d[0] * f[0] + d[1] * f[1]);
    } else { H.gStick[0] = H.gStick[1] = 0; }
    H.gTrig = stick ? 0.f : aim.pinchStrengthIndex;
    H.gGrip = stick ? 0.f : (cIdx < 0.6f ? std::min(1.f, std::max(0.f, (three - 0.4f) / 0.4f)) : 0.f);
    H.gPinchMid = aim.pinchStrengthMiddle; H.gPinchRing = aim.pinchStrengthRing; H.gPinchLittle = aim.pinchStrengthLittle;
    H.gStickClick = !fist && three < 0.3f && thumbToPalm < 0.04f;
    // «Pistola» (para moverte por la sala): índice estirado, mayor/anular/meñique cerrados; «disparo» = bajar el pulgar
    {
        const float *ip = P(XR_HAND_JOINT_INDEX_PROXIMAL_EXT), *it = P(XR_HAND_JOINT_INDEX_TIP_EXT);
        float d[3] = {it[0] - ip[0], it[1] - ip[1], it[2] - ip[2]};
        float dl = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        H.gPoint = cIdx < 0.35f && three > 0.6f && dl > 0.03f;
        if (H.gPoint) for (int k = 0; k < 3; k++) { H.gPointD[k] = d[k] / dl; H.gPointO[k] = it[k]; }
        float tPress = std::min(dist(P(XR_HAND_JOINT_THUMB_TIP_EXT), P(XR_HAND_JOINT_MIDDLE_INTERMEDIATE_EXT)),
                                dist(P(XR_HAND_JOINT_THUMB_TIP_EXT), P(XR_HAND_JOINT_INDEX_PROXIMAL_EXT)));
        // con margen (entra a 2,5 cm, sale a 4 cm) para que no tiemble
        H.gShot = H.gPoint && (H.gShot ? tPress < 0.04f : tPress < 0.025f);
    }
    // ---- gestos configurables ----
    {
        const int prof = g_inGameNow ? 1 : 0;
        const int *map = g_gm[prof][side];
        const float *tt = P(XR_HAND_JOINT_THUMB_TIP_EXT);
        float gv[G_N];
        gv[G_PI] = aim.pinchStrengthIndex; gv[G_PM] = aim.pinchStrengthMiddle;
        gv[G_PR] = aim.pinchStrengthRing; gv[G_PL] = aim.pinchStrengthLittle;
        gv[G_FIST] = fist ? 1.f : 0.f;
        gv[G_GRAB] = cIdx < 0.6f ? std::min(1.f, std::max(0.f, (three - 0.4f) / 0.4f)) : 0.f;
        gv[G_POINT] = H.gPoint ? 1.f : 0.f;
        gv[G_THUMBPALM] = H.gStickClick ? 1.f : 0.f;
        gv[G_THUMBUP] = (fist && thumbToIdx > 0.055f) ? 1.f : 0.f;
        // Joystick virtual
        const int jm = map[G_N];
        float sx = 0, sy = 0;
        bool on = false;
        float R[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1};
        const bool rOk = (jl[XR_HAND_JOINT_PALM_EXT].locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) != 0;
        if (rOk) quatToMat(jl[XR_HAND_JOINT_PALM_EXT].pose.orientation, R);
        auto local = [&](const float *w, float o[3]) {   // un punto en los ejes de la palma
            float d[3] = {w[0] - palm[0], w[1] - palm[1], w[2] - palm[2]};
            for (int c = 0; c < 3; c++) o[c] = R[c * 3] * d[0] + R[c * 3 + 1] * d[1] + R[c * 3 + 2] * d[2];
        };
        auto ax = [](float v, float full) { v /= full; if (fabsf(v) < 0.15f) return 0.f; return std::max(-1.f, std::min(1.f, v)); };
        if (jm == 1 && rOk) {
            // puño cerrado con el pulgar apoyado encima: mover el pulgar es mover el joystick (como el de un control)
            const bool want = fist && (H.joyOn ? thumbToIdx < 0.06f : thumbToIdx < 0.045f);
            if (want) {
                float lt[3]; local(tt, lt);
                if (!H.joyOn) for (int k = 0; k < 3; k++) H.joyAnchor[k] = lt[k];
                on = true;
                const float dY = lt[1] - H.joyAnchor[1], dZ = lt[2] - H.joyAnchor[2];
                sx = ax(dY * (side == 1 ? 1.f : -1.f), 0.015f);
                sy = ax(-dZ, 0.015f);
            }
        } else if (jm == 2) {
            // como VRChat: pellizcá el mayor con el pulgar y arrastrá desde ahí
            const bool want = H.joyOn ? aim.pinchStrengthMiddle > 0.5f : aim.pinchStrengthMiddle > 0.85f;
            if (want) {
                if (!H.joyOn) for (int k = 0; k < 3; k++) H.joyAnchor[k] = tt[k];
                on = true;
                float f[2] = {headFwd[0], headFwd[2]}, fl = sqrtf(f[0] * f[0] + f[1] * f[1]);
                if (fl < 1e-3f) { f[0] = 0; f[1] = -1; fl = 1; }
                f[0] /= fl; f[1] /= fl;
                const float r[2] = {-f[1], f[0]}, d[2] = {tt[0] - H.joyAnchor[0], tt[2] - H.joyAnchor[2]};
                sx = ax(d[0] * r[0] + d[1] * r[1], 0.05f);
                sy = ax(d[0] * f[0] + d[1] * f[1], 0.05f);
            }
        } else if (jm == 3) {
            on = H.gStickOn; sx = H.gStick[0]; sy = H.gStick[1];
        }
        H.joyOn = on;
        for (int g = 0; g < G_N; g++) H.gv[g] = gv[g];
        // El control que resulta: con el joystick en uso, los demás gestos no cuentan
        RealHands::Hand::VCtl v, vo;
        v.joy = on; v.sx = sx; v.sy = sy;
        auto apply = [](RealHands::Hand::VCtl &c, int m, float val) {
            if (m & A_TRIG) c.trig = std::max(c.trig, val);
            if (m & A_GRIP) c.grip = std::max(c.grip, val);
            const bool pressed = val > 0.8f;
            if (m & A_A) c.a |= pressed;
            if (m & A_B) c.b |= pressed;
            if (m & A_CLICK) c.click |= pressed;
            if (m & A_MENU) c.menu |= pressed;
        };
        if (!on) {
            for (int g = 0; g < G_N; g++) {
                const int m = map[g];
                if (!m) continue;
                apply(v, m & 0xFF, gv[g]);          // botones de este control
                apply(vo, (m >> 8) & 0xFF, gv[g]);  // (macro) botones del otro control
            }
        }
        H.v = v; H.vo = vo;
    }
    // Pose de agarre como la de un control (convención de OpenXR): X = normal de la palma (afuera de la palma en
    // la izquierda, desde el dorso en la derecha: en las dos, hacia la derecha con las palmas enfrentadas);
    // -Z = del meñique al pulgar (el tubo que forman los dedos al cerrar la mano); origen un poco delante de la palma
    H.gGripOk = false;
    H.palmOk = (jl[XR_HAND_JOINT_PALM_EXT].locationFlags & (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT))
               == (XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT);
    if (H.palmOk) H.palmPose = jl[XR_HAND_JOINT_PALM_EXT].pose;
    if ((jl[XR_HAND_JOINT_PALM_EXT].locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) {
        float R[9]; quatToMat(jl[XR_HAND_JOINT_PALM_EXT].pose.orientation, R);
        // +Y de la palma sale por el dorso: izquierda → afuera de la palma (−Y); derecha → desde el dorso (+Y)
        const float sg = side == 0 ? -1.f : 1.f;
        float X[3] = {sg * R[3], sg * R[4], sg * R[5]};
        const float out = -sg;   // (para correr el origen hacia el lado de la palma)
        const float *im = P(XR_HAND_JOINT_INDEX_METACARPAL_EXT), *lm = P(XR_HAND_JOINT_LITTLE_METACARPAL_EXT);
        float t[3] = {im[0] - lm[0], im[1] - lm[1], im[2] - lm[2]};   // del meñique hacia el índice/pulgar
        float tx = t[0] * X[0] + t[1] * X[1] + t[2] * X[2];
        for (int k = 0; k < 3; k++) t[k] -= X[k] * tx;
        float tl = sqrtf(t[0] * t[0] + t[1] * t[1] + t[2] * t[2]);
        if (tl > 1e-4f) {
            float Z[3] = {-t[0] / tl, -t[1] / tl, -t[2] / tl};
            float Y[3] = {Z[1] * X[2] - Z[2] * X[1], Z[2] * X[0] - Z[0] * X[2], Z[0] * X[1] - Z[1] * X[0]};
            // matriz (columnas X, Y, Z) → cuaternión
            float m00 = X[0], m11 = Y[1], m22 = Z[2], tr = m00 + m11 + m22;
            XrQuaternionf q;
            if (tr > 0) { float s = sqrtf(tr + 1.f) * 2; q.w = 0.25f * s; q.x = (Y[2] - Z[1]) / s; q.y = (Z[0] - X[2]) / s; q.z = (X[1] - Y[0]) / s; }
            else if (m00 > m11 && m00 > m22) { float s = sqrtf(1.f + m00 - m11 - m22) * 2; q.w = (Y[2] - Z[1]) / s; q.x = 0.25f * s; q.y = (Y[0] + X[1]) / s; q.z = (Z[0] + X[2]) / s; }
            else if (m11 > m22) { float s = sqrtf(1.f + m11 - m00 - m22) * 2; q.w = (Z[0] - X[2]) / s; q.x = (Y[0] + X[1]) / s; q.y = 0.25f * s; q.z = (Z[1] + Y[2]) / s; }
            else { float s = sqrtf(1.f + m22 - m00 - m11) * 2; q.w = (X[1] - Y[0]) / s; q.x = (Z[0] + X[2]) / s; q.y = (Z[1] + Y[2]) / s; q.z = 0.25f * s; }
            float ql = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
            q.x /= ql; q.y /= ql; q.z /= ql; q.w /= ql;
            // Con la mano abierta, «meñique → índice» apunta más arriba que el tubo de un puño cerrado sobre un control:
            // inclinar 45° hacia adelante/abajo (giro sobre el X propio) para que el láser y las manos del juego coincidan
            q = adjustPose(q, -45.f, 90.f, side);   // de fábrica: 45° hacia adelante y la palma como la de un control
            // Lo «0» de las manos sin calibrar (probado jugando): 85° hacia adelante y el giro de cada mano
            q = adjustPose(q, -85.f, side == 0 ? -130.f : 80.f, side);
            H.gGripPose.orientation = q;
            H.gGripPose.position = {palm[0] - R[3] * 0.035f, palm[1] - R[4] * 0.035f, palm[2] - R[5] * 0.035f};   // delante de la palma
            (void)out;
            H.gGripOk = true;
        }
    }
}

static bool locateJoints(RealHands::Hand &H, XrSpace space, XrTime t, XrHandJointLocationEXT *jl, XrHandTrackingAimStateFB *aim) {
    XrHandJointLocationsEXT locs{XR_TYPE_HAND_JOINT_LOCATIONS_EXT};
    locs.jointCount = XR_HAND_JOINT_COUNT_EXT; locs.jointLocations = jl;
    if (aim && g_rh.hasAim) locs.next = aim;
    XrHandJointsLocateInfoEXT li{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT};
    li.baseSpace = space; li.time = t;
    return XR_SUCCEEDED(g_rh.locate(H.tracker, &li, &locs)) && locs.isActive;
}

/// Una vez por cuadro: lee las manos (articulaciones, rayo y pellizco) y los controles, y prepara lo que se ve.
/// show = false: esconde todo (por ejemplo, durante un juego). cam: dibujar con la cámara (tc: cuándo se tomó su imagen)
static void realHandsUpdate(App &app, XrTime t, bool show, bool cam, XrTime tc) {
    const bool meta = show && !cam && g_rh.ok && app.ptRunning;   // respaldo: passthrough proyectado de Meta
    // O las manos o los controles: si hay algún control en uso, las manos no se siguen ni se ven
    g_ctlInUse = false;
    for (int s = 0; s < 2; s++) {
        XrSpaceLocation g{XR_TYPE_SPACE_LOCATION};
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (XR_SUCCEEDED(xrLocateSpace(app.gripSpace[s], app.space, t, &g)) && (g.locationFlags & need) == need) g_ctlInUse = true;
    }
    for (int s = 0; s < 2; s++) {
        RealHands::Hand &H = g_rh.h[s];
        H.active = false; H.aimValid = false; H.camDraw = false; H.ctlCamDraw = false; H.edgeOk = false; H.armDraw = false; H.tipOk = false;
        if (g_ctlInUse) hideHand(H, app.space, t);
        if (g_handsOff && !g_ctlInUse) hideHand(H, app.space, t);
        if (H.tracker && g_rh.locate && !g_ctlInUse && !g_handsOff) {
            XrHandJointLocationEXT jl[XR_HAND_JOINT_COUNT_EXT];
            XrHandTrackingAimStateFB aim{XR_TYPE_HAND_TRACKING_AIM_STATE_FB};
            if (locateJoints(H, app.space, t, jl, &aim)) {
                H.active = true;
                if (jl[XR_HAND_JOINT_INDEX_TIP_EXT].locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) {
                    const XrVector3f &tp = jl[XR_HAND_JOINT_INDEX_TIP_EXT].pose.position;
                    H.tip[0] = tp.x; H.tip[1] = tp.y; H.tip[2] = tp.z; H.tipOk = true;
                }
                if (g_rh.hasAim && (aim.status & XR_HAND_TRACKING_AIM_VALID_BIT_FB) && !(aim.status & XR_HAND_TRACKING_AIM_SYSTEM_GESTURE_BIT_FB)) {
                    H.aimValid = true; H.aim = aim.aimPose; H.pinch = aim.pinchStrengthIndex;
                }
                handGestures(H, s, jl, aim, app.headFwd);
                // Borde de la palma del lado del meñique: entre la base del meñique y su nudillo, un poco hacia afuera
                {
                    const XrHandJointLocationEXT &ip = jl[XR_HAND_JOINT_INDEX_PROXIMAL_EXT], &lp = jl[XR_HAND_JOINT_LITTLE_PROXIMAL_EXT],
                                                 &im = jl[XR_HAND_JOINT_INDEX_METACARPAL_EXT], &lm = jl[XR_HAND_JOINT_LITTLE_METACARPAL_EXT];
                    const XrSpaceLocationFlags pv = XR_SPACE_LOCATION_POSITION_VALID_BIT;
                    if ((ip.locationFlags & pv) && (lp.locationFlags & pv) && (im.locationFlags & pv) && (lm.locationFlags & pv)) {
                        // hacia afuera: del índice al meñique (promedio de los nudillos y de la base)
                        float d[3];
                        for (int k = 0; k < 3; k++) d[k] = ((&lp.pose.position.x)[k] - (&ip.pose.position.x)[k]) + ((&lm.pose.position.x)[k] - (&im.pose.position.x)[k]);
                        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                        if (l > 1e-3f) {
                            for (int k = 0; k < 3; k++) {
                                H.edgeDir[k] = d[k] / l;
                                H.edge[k] = (&lm.pose.position.x)[k] * 0.5f + (&lp.pose.position.x)[k] * 0.5f + H.edgeDir[k] * 0.012f;
                            }
                            H.edgeOk = true;
                            // normal de la palma: perpendicular al ancho de la mano y a los dedos
                            const XrHandJointLocationEXT &wr = jl[XR_HAND_JOINT_WRIST_EXT], &mp = jl[XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT];
                            float f[3] = {mp.pose.position.x - wr.pose.position.x, mp.pose.position.y - wr.pose.position.y, mp.pose.position.z - wr.pose.position.z};
                            float n[3] = {H.edgeDir[1] * f[2] - H.edgeDir[2] * f[1], H.edgeDir[2] * f[0] - H.edgeDir[0] * f[2], H.edgeDir[0] * f[1] - H.edgeDir[1] * f[0]};
                            float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                            if (nl > 1e-6f) for (int k = 0; k < 3; k++) H.palmN[k] = n[k] / nl;
                        }
                    }
                }
                const XrHandJointLocationEXT *use = jl;
                if (show && cam && !H.basePos.empty()) {
                    // con la cámara: la mano como estaba cuando se tomó la imagen (así la imagen calza con la forma)
                    static XrHandJointLocationEXT jc[2][XR_HAND_JOINT_COUNT_EXT];
                    if (tc && tc != t && locateJoints(H, app.space, tc, jc[s], nullptr)) use = jc[s];
                    H.camV.resize(H.basePos.size() * 6);
                    const float grow = -0.0005f + 0.003f * g_edgeSoft.load();
                    if (skinHand(H, use, grow, nullptr, H.camV.data())) {   // (el tamaño de antes; el difuminado del borde es más fino)
                        if (!H.vbo) {
                            glGenBuffers(1, &H.vbo); glGenBuffers(1, &H.ibo);
                            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, H.ibo);
                            glBufferData(GL_ELEMENT_ARRAY_BUFFER, H.idx.size() * 4, H.idx.data(), GL_STATIC_DRAW);
                            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
                        }
                        glBindBuffer(GL_ARRAY_BUFFER, H.vbo);
                        glBufferData(GL_ARRAY_BUFFER, H.camV.size() * 4, H.camV.data(), GL_STREAM_DRAW);
                        glBindBuffer(GL_ARRAY_BUFFER, 0);
                        H.camDraw = true;
                        buildForearm(H, use);
                    }
                }
                // Passthrough proyectado de Meta: solo, o (con la cámara) por debajo y un poco más chico, para que la mano
                // se siga viendo donde la cámara no llega (por ejemplo, muy abajo en la vista)
                if (meta && H.mesh && H.geo && g_rh.beginVB && g_rh.getVB && g_rh.endVB) {
                    uint32_t vc = 0;
                    XrVector3f *vb = nullptr;
                    XrResult rb = g_rh.beginVB(H.mesh, &vc);
                    if (XR_FAILED(rb)) g_rhErr = (int)rb;
                    else {
                        bool okSkin = XR_SUCCEEDED(g_rh.getVB(H.mesh, &vb)) && vb && vc == H.basePos.size() && skinHand(H, use, 0.0015f, vb, nullptr);
                        g_rh.endVB(H.mesh);
                        if (okSkin) {
                            XrPosef id{}; id.orientation.w = 1;
                            geoPose(H.geo, app.space, t, id, {1, 1, 1});   // los vértices ya están en la sala
                            H.handShown = true;
                        } else hideHand(H, app.space, t);
                    }
                } else hideHand(H, app.space, t);
            } else hideHand(H, app.space, t);
        }
        // Control: su forma (1,5 mm más grande) en la pose de agarre; sin modelo, una forma redondeada
        bool ctlOk = false, cmOk = false;
        if (show && g_realHands) {
            XrSpaceLocation g{XR_TYPE_SPACE_LOCATION}, a{XR_TYPE_SPACE_LOCATION};
            const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
            XrTime when = cam && tc ? tc : t;
            bool gOk = XR_SUCCEEDED(xrLocateSpace(app.gripSpace[s], app.space, when, &g)) && (g.locationFlags & need) == need;
            if (gOk && H.cmLoaded) {
                if (cam) { H.ctlPose = g.pose; H.ctlCamDraw = true; H.ctlCamBlob = false; }
                else if (meta && H.cmGeo) { geoPose(H.cmGeo, app.space, t, g.pose, {1, 1, 1}); H.cmShown = true; cmOk = true; }
            } else if (gOk && !H.active && XR_SUCCEEDED(xrLocateSpace(app.aimSpace[s], app.space, when, &a)) && (a.locationFlags & need) == need) {
                XrPosef p = g.pose;
                p.position = {g.pose.position.x + (a.pose.position.x - g.pose.position.x) * 0.3f,
                              g.pose.position.y + (a.pose.position.y - g.pose.position.y) * 0.3f,
                              g.pose.position.z + (a.pose.position.z - g.pose.position.z) * 0.3f};
                if (cam) { H.ctlPose = p; H.ctlCamDraw = true; H.ctlCamBlob = true; }
                else if (meta && H.ctl) { geoPose(H.ctl, app.space, t, p, {0.125f, 0.14f, 0.2f}); H.ctlShown = true; ctlOk = true; }
            }
        }
        if (!ctlOk) geoHide(H.ctl, app.space, t, H.ctlShown);
        if (!cmOk) geoHide(H.cmGeo, app.space, t, H.cmShown);
        // buffers del modelo del control (una vez)
        if (H.cmLoaded && !H.cvbo) {
            glGenBuffers(1, &H.cvbo); glGenBuffers(1, &H.cibo);
            glBindBuffer(GL_ARRAY_BUFFER, H.cvbo);
            glBufferData(GL_ARRAY_BUFFER, H.cmPN.size() * 4, H.cmPN.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, H.cibo);
            glBufferData(GL_ELEMENT_ARRAY_BUFFER, H.cmIdx.size() * 4, H.cmIdx.data(), GL_STATIC_DRAW);
            glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
        }
    }
    if (cam && !g_rh.bvbo && !g_rh.blobPN.empty()) {
        glGenBuffers(1, &g_rh.bvbo); glGenBuffers(1, &g_rh.bibo);
        glBindBuffer(GL_ARRAY_BUFFER, g_rh.bvbo);
        glBufferData(GL_ARRAY_BUFFER, g_rh.blobPN.size() * 4, g_rh.blobPN.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_rh.bibo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, g_rh.blobIdx.size() * 4, g_rh.blobIdx.data(), GL_STATIC_DRAW);
        glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    }
}

// ---------------------------------------------------------------------------
// Imagen de las cámaras → textura, y cómo proyectar un punto de la sala sobre esa imagen
// ---------------------------------------------------------------------------
// ---------------------------------------------------------------------------
// Portal: con la opción activada, dibujás un círculo (con la punta del índice pellizcando, o con la punta del control
// apretando el gatillo) alrededor de algo real, por ejemplo el teclado, y esa zona se ve con la cámara hasta apagarlo.
// ---------------------------------------------------------------------------
struct Portal {
    int state = 0;                  // 0 apagado · 1 esperando que dibujes · 2 visible
    int hand = -1;                  // la mano que está dibujando
    std::vector<float> trail;       // puntos del trazo (x, y, z)
    GLuint vbo = 0, ibo = 0;
    GLsizei n = 0;
};
static Portal g_portal;
static std::atomic<int> g_portalReq{-1};   // pedido desde el menú: 1 activar, 0 desactivar

extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetPortal(JNIEnv *, jclass, jboolean on) { g_portalReq = on ? 1 : 0; }

/// Arma la malla del portal a partir del trazo: el plano que mejor sigue el contorno dibujado (acostado sobre la
/// mesa, parado delante tuyo o inclinado), con el borde difuminado
static bool portalBuild() {
    std::vector<float> &t = g_portal.trail;
    size_t n = t.size() / 3;
    if (n < 12) return false;
    double len = 0;
    for (size_t i = 1; i < n; i++) len += sqrt(pow(t[i * 3] - t[i * 3 - 3], 2) + pow(t[i * 3 + 1] - t[i * 3 - 2], 2) + pow(t[i * 3 + 2] - t[i * 3 - 1], 2));
    if (len < 0.3) return false;   // muy chico
    float c[3] = {0, 0, 0};
    for (size_t i = 0; i < n; i++) for (int k = 0; k < 3; k++) c[k] += t[i * 3 + k] / n;
    // Normal del contorno (método de Newell: funciona con lazos cerrados aunque no sean perfectos)
    float nr[3] = {0, 0, 0};
    for (size_t i = 0; i < n; i++) {
        const float *p0 = &t[i * 3], *p1 = &t[((i + 1) % n) * 3];
        nr[0] += (p0[1] - p1[1]) * (p0[2] + p1[2]);
        nr[1] += (p0[2] - p1[2]) * (p0[0] + p1[0]);
        nr[2] += (p0[0] - p1[0]) * (p0[1] + p1[1]);
    }
    float nl = sqrtf(nr[0] * nr[0] + nr[1] * nr[1] + nr[2] * nr[2]);
    if (nl < 1e-5f) { nr[0] = 0; nr[1] = 1; nr[2] = 0; } else for (float &v : nr) v /= nl;
    // Ejes del plano
    float ref[3] = {fabsf(nr[1]) < 0.9f ? 0.f : 1.f, fabsf(nr[1]) < 0.9f ? 1.f : 0.f, 0};
    float U[3] = {ref[1] * nr[2] - ref[2] * nr[1], ref[2] * nr[0] - ref[0] * nr[2], ref[0] * nr[1] - ref[1] * nr[0]};
    float ul = sqrtf(U[0] * U[0] + U[1] * U[1] + U[2] * U[2]);
    for (float &v : U) v /= ul;
    float W[3] = {nr[1] * U[2] - nr[2] * U[1], nr[2] * U[0] - nr[0] * U[2], nr[0] * U[1] - nr[1] * U[0]};
    // remuestrear por ángulo alrededor del centro (así el contorno queda ordenado y cerrado), y suavizar
    const int R = 72;
    std::vector<float> rad(R, 0.f);
    std::vector<int> cnt(R, 0);
    for (size_t i = 0; i < n; i++) {
        float d[3] = {t[i * 3] - c[0], t[i * 3 + 1] - c[1], t[i * 3 + 2] - c[2]};
        float dx = d[0] * U[0] + d[1] * U[1] + d[2] * U[2], dz = d[0] * W[0] + d[1] * W[1] + d[2] * W[2];
        int b = (int)floorf((atan2f(dz, dx) + 3.14159265f) / 6.2831853f * R) % R;
        float r = sqrtf(dx * dx + dz * dz);
        if (r > rad[b]) rad[b] = r;
        cnt[b]++;
    }
    // sectores sin puntos: interpolar con los vecinos
    for (int pass = 0; pass < R; pass++) {
        bool any = false;
        for (int i = 0; i < R; i++) if (!cnt[i]) {
            int a = (i + R - 1) % R, b = (i + 1) % R;
            if (cnt[a] || cnt[b]) { rad[i] = (cnt[a] && cnt[b]) ? (rad[a] + rad[b]) * 0.5f : (cnt[a] ? rad[a] : rad[b]); cnt[i] = -1; any = true; }
        }
        for (int i = 0; i < R; i++) if (cnt[i] < 0) cnt[i] = 1;
        if (!any) break;
    }
    for (int it = 0; it < 3; it++) {
        std::vector<float> o(R);
        for (int i = 0; i < R; i++) o[i] = (rad[(i + R - 1) % R] + 2 * rad[i] + rad[(i + 1) % R]) * 0.25f;
        rad = o;
    }
    const float fade = 0.035f;   // ancho del borde difuminado
    std::vector<float> v;
    auto vert = [&](float a2, float b2, float al) {
        v.insert(v.end(), {c[0] + U[0] * a2 + W[0] * b2, c[1] + U[1] * a2 + W[1] * b2, c[2] + U[2] * a2 + W[2] * b2, nr[0], nr[1], nr[2], al});
    };
    vert(0, 0, 1.f);
    for (int i = 0; i < R; i++) {
        float ang = -3.14159265f + (i + 0.5f) * 6.2831853f / R, cs = cosf(ang), sn = sinf(ang);
        float rin = std::max(0.02f, rad[i] - fade * 0.5f), rout = rad[i] + fade * 0.5f;
        vert(cs * rin, sn * rin, 1.f);
        vert(cs * rout, sn * rout, 0.f);
    }
    std::vector<uint32_t> idx;
    for (int i = 0; i < R; i++) {
        uint32_t a = 1 + i * 2, b = 1 + ((i + 1) % R) * 2;
        idx.insert(idx.end(), {0u, a, b, a, a + 1, b, b, a + 1, b + 1});
    }
    if (!g_portal.vbo) { glGenBuffers(1, &g_portal.vbo); glGenBuffers(1, &g_portal.ibo); }
    glBindBuffer(GL_ARRAY_BUFFER, g_portal.vbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * 4, v.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_portal.ibo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, idx.size() * 4, idx.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    g_portal.n = (GLsizei)idx.size();
    return true;
}

/// Pedidos del menú (una vez por cuadro, en el hilo de dibujo)
static void portalUpdate() {
    int req = g_portalReq.exchange(-1);
    if (req == 1) { g_portal.state = 1; g_portal.hand = -1; g_portal.trail.clear(); g_portal.n = 0; }
    if (req == 0) { g_portal.state = 0; g_portal.hand = -1; g_portal.trail.clear(); g_portal.n = 0; }
}

/// Dibujo del portal con una mano: true si el gesto se usó para dibujar (no es un clic)
static bool portalInput(int h, bool byHand, const float aimO[3], bool pressed, bool released, bool overPanel) {
    if (g_portal.state != 1) return false;
    if (g_portal.hand < 0) {
        if (!pressed || overPanel) return false;
        g_portal.hand = h; g_portal.trail.clear();
    }
    if (g_portal.hand != h) return false;
    float p[3];
    if (byHand && g_rh.h[h].tipOk) for (int k = 0; k < 3; k++) p[k] = g_rh.h[h].tip[k];
    else for (int k = 0; k < 3; k++) p[k] = aimO[k];
    std::vector<float> &t = g_portal.trail;
    size_t n = t.size();
    if (n < 3 || hypot(hypot(p[0] - t[n - 3], p[1] - t[n - 2]), p[2] - t[n - 1]) > 0.006) t.insert(t.end(), {p[0], p[1], p[2]});
    if (released) {
        g_portal.hand = -1;
        if (portalBuild()) { g_portal.state = 2; sendLog("Portal: listo (%u puntos)", (unsigned)(t.size() / 3)); javaPortalDone(); }
        else { t.clear(); sendLog("Portal: el trazo fue muy chico, se puede volver a dibujar"); }
    }
    return true;
}

struct CamFrame {
    GLuint tex = 0;
    std::deque<App::Held> held;
    bool have = false;
    double gotMs = 0;
    XrTime xt = 0;
    float sx = 1, sy = 1;          // parte válida de la textura
    float M[12] = {};              // sala → textura (proyectiva, 3 filas de 4, por filas)
    int frames = 0;
};
static CamFrame g_cf[2];
static bool g_camOpenTried = false;
static GLuint g_camProg = 0, g_camVao = 0;
static GLint cCam2 = -1, cTex2 = -1, cUse2 = -1, cLim2 = -1;
static GLint cOnlyOver = -1, cNRect = -1, cRc = -1, cRa = -1, cRb = -1;
static bool panelShown(const UiPanel *p);
static bool fixedScreenEnv();
static GLint cVP = -1, cMdl = -1, cCam = -1, cEye = -1, cSrgb = -1, cDepthOnly = -1, cLim = -1, cTex = -1, cFadeP = -1, cFadeD = -1, cSat = -1, cSoft = -1, cFlat = -1;
static std::atomic<float> g_handSat{0.8f};    // saturación de las manos (ajuste del usuario)
extern "C" JNIEXPORT void JNICALL
Java_org_questvrbridge_client_VrActivity_nativeSetHandSat(JNIEnv *, jclass, jfloat s) { g_handSat = std::max(0.f, std::min(2.f, (float)s)); }

static void camInitGL() {
    const char *vs = R"(#version 300 es
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
layout(location = 2) in float va;
uniform mat4 VP;
uniform mat4 Mdl;
uniform mat4 Cam;
uniform mat4 Cam2;
out float alphaV;
out vec3 cc;
out vec3 cc2;
out vec3 wn;
out vec3 wp;
invariant gl_Position;
void main() {
    vec4 w = Mdl * vec4(pos, 1.0);
    wp = w.xyz;
    alphaV = va;
    wn = mat3(Mdl) * nrm;
    cc = (Cam * w).xyz;
    cc2 = (Cam2 * w).xyz;
    gl_Position = VP * w;
})";
    const char *fs = R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision highp float;
uniform samplerExternalOES cam;
uniform samplerExternalOES cam2;   // la otra cámara: cubre lo que la primera no ve (más campo, sin cortes)
uniform float use2;
uniform vec2 lim2;
uniform vec3 eye;
uniform float srgb;
uniform float depthOnly;   // 1: solo profundidad · 2: solo transparencia · 0: solo color
uniform vec2 lim;
uniform vec4 fadeP;        // antebrazo: desde dónde (xyz) y hasta qué largo (w) se ve
uniform vec3 fadeD;
uniform float sat;         // saturación (el usuario la ajusta)
uniform float soft;        // suavidad del borde (0 a 1)
uniform float flatSurf;    // 1: superficie plana (portal): sin borde por ángulo, solo el difuminado propio
uniform float onlyOver;    // 1: sala passthrough: solo se dibuja delante de una pantalla (afuera ya se ve tu mano real)
uniform int nRect;
uniform vec3 rc[12];       // pantallas: centro, medio ancho (eje x) y media altura (eje y)
uniform vec3 ra[12];
uniform vec3 rb[12];
in float alphaV;
in vec3 cc;
in vec3 cc2;
in vec3 wn;
in vec3 wp;
out vec4 color;
void main() {
    if (depthOnly > 0.5 && depthOnly < 1.5) { color = vec4(0.0); return; }
    // borde de la imagen de cada cámara: se desvanece de a poco (arriba, más ancho: ahí la cámara ve menos que vos)
    vec2 uv = cc.z > 0.001 ? cc.xy / cc.z : vec2(-1.0);
    float w1 = cc.z > 0.001 ? smoothstep(0.0, 0.08 * lim.x, uv.x) * smoothstep(0.0, 0.14 * lim.y, uv.y)
                            * smoothstep(lim.x, lim.x * 0.92, uv.x) * smoothstep(lim.y, lim.y * 0.86, uv.y) : 0.0;
    vec2 uv2 = cc2.z > 0.001 ? cc2.xy / cc2.z : vec2(-1.0);
    float w2 = (use2 > 0.5 && cc2.z > 0.001) ? smoothstep(0.0, 0.08 * lim2.x, uv2.x) * smoothstep(0.0, 0.14 * lim2.y, uv2.y)
                            * smoothstep(lim2.x, lim2.x * 0.92, uv2.x) * smoothstep(lim2.y, lim2.y * 0.86, uv2.y) : 0.0;
    float inImg = max(w1, w2);
    if (inImg <= 0.001) discard;
    if (onlyOver > 0.5) {
        // ¿hay una pantalla detrás de este punto (entre el punto y lo que está atrás)? si no, no se dibuja
        vec3 dir = wp - eye;
        bool over = false;
        for (int i = 0; i < 12; i++) {
            if (i >= nRect) break;
            vec3 nn = cross(ra[i], rb[i]);
            float den = dot(dir, nn);
            if (abs(den) < 1e-6) continue;
            float t = dot(rc[i] - eye, nn) / den;
            if (t < 1.0) continue;
            vec3 h = eye + dir * t - rc[i];
            float u = dot(h, ra[i]) / dot(ra[i], ra[i]), w = dot(h, rb[i]) / dot(rb[i], rb[i]);
            if (abs(u) <= 1.0 && abs(w) <= 1.0) { over = true; break; }
        }
        if (!over) discard;
    }
    // borde difuminado: donde la superficie se ve de costado (el contorno de la mano), se vuelve transparente
    vec3 n = normalize(wn);
    vec3 v = normalize(eye - wp);
    float e0 = 0.02, e1 = 0.10 + 0.14 * soft;   // (igual en todas las salas)
    float a = flatSurf > 0.5 ? alphaV * inImg
                             : smoothstep(e0, e1, abs(dot(n, v))) * inImg;   // borde finito (antes era muy ancho y se veía como una sombra doble)
    a *= smoothstep(fadeP.w, fadeP.w - 0.03, dot(wp - fadeP.xyz, fadeD));   // final del antebrazo, difuminado
    if (depthOnly > 1.5) { color = vec4(0.0, 0.0, 0.0, a); return; }
    // color: mezcla de las dos cámaras según cuánto ve cada una ese punto
    // color: una sola cámara (la de este ojo); la otra solo donde la primera no ve (así no se ve doble)
    float k1 = w2 > 0.0 ? smoothstep(0.0, 0.35, w1) : 1.0;
    vec3 c = mix(w2 > 0.0 ? texture(cam2, uv2).rgb : vec3(0.0), w1 > 0.0 ? texture(cam, uv).rgb : vec3(0.0), k1);
    // la imagen de la cámara llega con el rango de colores estirado (más contraste y saturación): volverlo al real
    c = c * (219.0 / 255.0) + 16.0 / 255.0;
    float l = dot(c, vec3(0.299, 0.587, 0.114));
    c = clamp(mix(vec3(l), c, sat), 0.0, 1.0);
    if (srgb > 0.5) c = pow(c, vec3(2.2));
    color = vec4(c, 1.0);
})";
    g_camProg = glCreateProgram();
    glAttachShader(g_camProg, compileShader(GL_VERTEX_SHADER, vs));
    glAttachShader(g_camProg, compileShader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(g_camProg);
    GLint ok = 0; glGetProgramiv(g_camProg, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; glGetProgramInfoLog(g_camProg, sizeof log, nullptr, log); LOGE("Programa de la cámara: %s", log); sendLog("Manos con cámara: el programa de dibujo falló (%s)", log); }
    cVP = glGetUniformLocation(g_camProg, "VP"); cMdl = glGetUniformLocation(g_camProg, "Mdl"); cCam = glGetUniformLocation(g_camProg, "Cam");
    cEye = glGetUniformLocation(g_camProg, "eye"); cSrgb = glGetUniformLocation(g_camProg, "srgb"); cDepthOnly = glGetUniformLocation(g_camProg, "depthOnly");
    cLim = glGetUniformLocation(g_camProg, "lim"); cTex = glGetUniformLocation(g_camProg, "cam");
    cSat = glGetUniformLocation(g_camProg, "sat");
    cOnlyOver = glGetUniformLocation(g_camProg, "onlyOver"); cNRect = glGetUniformLocation(g_camProg, "nRect");
    cRc = glGetUniformLocation(g_camProg, "rc"); cRa = glGetUniformLocation(g_camProg, "ra"); cRb = glGetUniformLocation(g_camProg, "rb");
    cSoft = glGetUniformLocation(g_camProg, "soft");
    cFlat = glGetUniformLocation(g_camProg, "flatSurf");
    cCam2 = glGetUniformLocation(g_camProg, "Cam2"); cTex2 = glGetUniformLocation(g_camProg, "cam2");
    cUse2 = glGetUniformLocation(g_camProg, "use2"); cLim2 = glGetUniformLocation(g_camProg, "lim2");
    cFadeP = glGetUniformLocation(g_camProg, "fadeP"); cFadeD = glGetUniformLocation(g_camProg, "fadeD");
    glGenVertexArrays(1, &g_camVao);
    for (int e = 0; e < 2; e++) {
        glGenTextures(1, &g_cf[e].tex);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, g_cf[e].tex);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    }
}

/// Durante un juego sin manos, las cámaras dejan de mandar imágenes (y vuelven al salir)
static double g_camActiveSince = 0;   // desde cuándo las cámaras tienen que estar mandando imágenes
static void camSetActive(bool on) {
    static bool active = true;
    if (on == active) return;
    active = on;
    if (on) g_camActiveSince = nowMs();
    for (CamEye &E : g_cam) {
        if (!E.ok || !E.ses) continue;
        if (on) ACameraCaptureSession_setRepeatingRequest(E.ses, nullptr, 1, &E.req, nullptr);
        else ACameraCaptureSession_stopRepeating(E.ses);
    }
}

/// Una vez por cuadro: abre las cámaras (si hay permiso), toma la última imagen de cada una y calcula su proyección
static void camFrameUpdate(App &app, XrTime now) {
    g_camMode = false;
    if (g_camAllowed.load() == 1 && !g_camOpenTried && g_realHands) {
        g_camOpenTried = true;
        camOpen();
        sendLog("Manos con cámara: %s", g_camStatus);
    }
    // Después de que el visor estuvo apagado (o lo dejaste un rato), a veces las cámaras dejan de mandar imágenes y las
    // manos no se ven más: si hace 3 s que no llega ninguna, se cierran y se vuelven a abrir (como mucho cada 5 s)
    {
        static double lastReopen = 0;
        const double t = nowMs();
        double last = std::max(g_camActiveSince, std::max(g_cf[0].gotMs, g_cf[1].gotMs));
        if (last <= 0) g_camActiveSince = last = t;
        const bool wasWorking = g_cf[0].frames > 0 || g_cf[1].frames > 0;
        if (g_camOpenTried && wasWorking && t - last > 3000 && t - lastReopen > 5000) {
            lastReopen = t;
            for (CamFrame &F : g_cf) {
                while (!F.held.empty()) { app.destroyImage(app.display, F.held.front().egl); AImage_delete(F.held.front().img); F.held.pop_front(); }
                F.have = false;
            }
            camClose();
            g_camOpenTried = false;
            g_camActiveSince = t;
            sendLog("Manos con cámara: las cámaras dejaron de mandar imágenes; las vuelvo a abrir");
        }
    }
    if (!g_cam[0].ok && !g_cam[1].ok) return;
    if (!g_camProg) camInitGL();
    for (int e = 0; e < 2; e++) {
        CamEye &E = g_cam[e];
        CamFrame &F = g_cf[e];
        if (!E.ok || !E.rd) continue;
        AImage *img = nullptr;
        if (AImageReader_acquireLatestImage(E.rd, &img) != AMEDIA_OK || !img) continue;
        AHardwareBuffer *hb = nullptr;
        if (AImage_getHardwareBuffer(img, &hb) != AMEDIA_OK || !hb) { AImage_delete(img); continue; }
        int64_t ts = 0;
        AImage_getTimestamp(img, &ts);
        AHardwareBuffer_Desc desc{};
        AHardwareBuffer_describe(hb, &desc);
        AImageCropRect crop{0, 0, E.w, E.h};
        AImage_getCropRect(img, &crop);
        const EGLint attr[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
        EGLImageKHR egl = app.createImage(app.display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, app.getClientBuffer(hb), attr);
        if (egl == EGL_NO_IMAGE_KHR) { AImage_delete(img); continue; }
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, F.tex);
        app.imageTarget(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)egl);
        F.held.push_back({img, egl});
        while (F.held.size() > 3) { app.destroyImage(app.display, F.held.front().egl); AImage_delete(F.held.front().img); F.held.pop_front(); }
        F.sx = desc.width ? (float)std::max(1, crop.right) / (float)desc.width : 1.f;
        F.sy = desc.height ? (float)std::max(1, crop.bottom) / (float)desc.height : 1.f;
        // cuándo se tomó (en el reloj de OpenXR)
        int64_t mono = camToMonotonic(E, ts);
        XrTime xt = mono;
        if (g_rh.toXrTime) {
            timespec tsp{}; tsp.tv_sec = (time_t)(mono / 1000000000LL); tsp.tv_nsec = (long)(mono % 1000000000LL);
            XrTime o = 0;
            if (XR_SUCCEEDED(g_rh.toXrTime(app.instance, &tsp, &o))) xt = o;
        }
        if (xt <= 0 || xt > now || now - xt > 300000000LL) xt = now;   // (si el reloj no tiene sentido, usar el de ahora)
        // dónde estaba la cabeza en ese momento
        XrSpaceLocation hl{XR_TYPE_SPACE_LOCATION};
        const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
        if (!app.viewSpace || XR_FAILED(xrLocateSpace(app.viewSpace, app.space, xt, &hl)) || (hl.locationFlags & need) != need) {
            if (!app.viewSpace || XR_FAILED(xrLocateSpace(app.viewSpace, app.space, now, &hl)) || (hl.locationFlags & need) != need) continue;
        }
        // sala → cámara: p_cam = R(r) · (R(cabeza)ᵀ · (p − cabeza) − t)   (cámara: x a la derecha, y hacia abajo, z hacia adelante)
        float RH[9], RC[9];
        quatToMat(hl.pose.orientation, RH);
        quatToMat(XrQuaternionf{E.r[0], E.r[1], E.r[2], E.r[3]}, RC);
        float A[9];   // A = RC · RHᵀ   (column-major: A[c*3+r])
        for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) {
            float sum = 0;
            for (int k = 0; k < 3; k++) sum += RC[k * 3 + r] * RH[k * 3 + c];   // RC[r][k] · RHᵀ[k][c] = RC[r][k] · RH[c][k]
            A[c * 3 + r] = sum;
        }
        const float hp[3] = {hl.pose.position.x, hl.pose.position.y, hl.pose.position.z};
        float b[3];
        for (int r = 0; r < 3; r++) {
            float rt = RC[r] * E.t[0] + RC[3 + r] * E.t[1] + RC[6 + r] * E.t[2];
            b[r] = -(A[r] * hp[0] + A[3 + r] * hp[1] + A[6 + r] * hp[2]) - rt;
        }
        // cámara → textura: la imagen pedida es el centro del sensor, escalado
        float sc = std::min((float)E.sensW / E.w, (float)E.sensH / E.h);
        float cw = E.w * sc, ch = E.h * sc, ox = (E.sensW - cw) * 0.5f, oy = (E.sensH - ch) * 0.5f;
        const float K[3][3] = {{E.fx / cw * F.sx, E.sk / cw * F.sx, (E.cx - ox) / cw * F.sx},
                               {0, E.fy / ch * F.sy, (E.cy - oy) / ch * F.sy},
                               {0, 0, 1}};
        for (int r = 0; r < 3; r++) {
            for (int c = 0; c < 3; c++) F.M[r * 4 + c] = K[r][0] * A[c * 3 + 0] + K[r][1] * A[c * 3 + 1] + K[r][2] * A[c * 3 + 2];
            F.M[r * 4 + 3] = K[r][0] * b[0] + K[r][1] * b[1] + K[r][2] * b[2];
        }
        F.xt = xt;
        F.have = true;
        F.gotMs = nowMs();
        F.frames++;
    }
    double t = nowMs();
    bool fresh0 = g_cf[0].have && t - g_cf[0].gotMs < 600, fresh1 = g_cf[1].have && t - g_cf[1].gotMs < 600;
    g_camMode = g_realHands && g_camProg && (fresh0 || fresh1);
    g_camT = fresh0 ? g_cf[0].xt : fresh1 ? g_cf[1].xt : 0;
}

static void poseMatrix(const XrPosef &p, const float s[3], float out[16]) {
    float R[9]; quatToMat(p.orientation, R);
    for (int c = 0; c < 3; c++) { for (int r = 0; r < 3; r++) out[c * 4 + r] = R[c * 3 + r] * s[c]; out[c * 4 + 3] = 0; }
    out[12] = p.position.x; out[13] = p.position.y; out[14] = p.position.z; out[15] = 1;
}

/// Dibuja las manos y los controles con la imagen de la cámara (en la capa de encima, antes del láser)
static void drawCamHands(App &app, int e, const float VP[16], const float eye[3]) {
    if (!g_camMode || !g_camProg) return;
    const CamFrame &F = (g_cf[e].have && nowMs() - g_cf[e].gotMs < 600) ? g_cf[e] : g_cf[1 - e];
    if (!F.have) return;
    bool any = false;
    for (int s = 0; s < 2; s++) any |= g_rh.h[s].camDraw || g_rh.h[s].ctlCamDraw || g_rh.h[s].armDraw;
    any |= g_portal.vbo && g_portal.n > 0;
    if (!any) return;
    glUseProgram(g_camProg);
    glUniformMatrix4fv(cVP, 1, GL_FALSE, VP);
    float C[16] = {F.M[0], F.M[4], F.M[8], 0, F.M[1], F.M[5], F.M[9], 0, F.M[2], F.M[6], F.M[10], 0, F.M[3], F.M[7], F.M[11], 1};
    glUniformMatrix4fv(cCam, 1, GL_FALSE, C);
    glUniform3f(cEye, eye[0], eye[1], eye[2]);
    glUniform1f(cSrgb, app.srgb ? 1.f : 0.f);
    glUniform2f(cLim, F.sx, F.sy);
    glUniform1f(cSat, g_handSat.load());
    glUniform1f(cSoft, g_edgeSoft.load());
    glUniform1f(cFlat, 0.f);
    // Sala passthrough: tus manos y controles reales ya se ven; la imagen de la cámara solo hace falta delante de las pantallas
    {
        static float RC[36], RA[36], RB[36];
        static int nR = 0;
        std::unique_lock<std::mutex> lk(g_uiLock, std::try_to_lock);
        if (lk.owns_lock()) {
            nR = 0;
            const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0};
            for (UiPanel *p : g_ui) {
                if (nR >= 12) break;
                if (p->dead || !p->ready || !p->placed || p->hud || !panelShown(p)) continue;
                float ax[3], ay[3];
                rotate(p->pose.orientation, ex, ax); rotate(p->pose.orientation, ey, ay);
                float hw = p->wm / 2, hh = p->hm / 2;
                RC[nR * 3] = p->pose.position.x; RC[nR * 3 + 1] = p->pose.position.y; RC[nR * 3 + 2] = p->pose.position.z;
                for (int k = 0; k < 3; k++) { RA[nR * 3 + k] = ax[k] * hw; RB[nR * 3 + k] = ay[k] * hh; }
                nR++;
            }
        }
        // (durante un juego el juego tapa el passthrough: ahí las manos van en todos lados, no solo sobre las pantallas)
        const bool only = g_env == 3 && app.hasPassthrough && !g_inGameNow;
        glUniform1f(cOnlyOver, only ? 1.f : 0.f);

        glUniform1i(cNRect, nR);
        if (nR > 0) { glUniform3fv(cRc, nR, RC); glUniform3fv(cRa, nR, RA); glUniform3fv(cRb, nR, RB); }
    }
    glDisableVertexAttribArray(2);
    glVertexAttrib1f(2, 1.f);
    // La otra cámara (si tiene imagen reciente): completa lo que esta no ve
    const CamFrame &G = (&F == &g_cf[0]) ? g_cf[1] : g_cf[0];
    const bool use2 = G.have && nowMs() - G.gotMs < 600 && G.tex;
    {
        const CamFrame &S = use2 ? G : F;
        float C2[16] = {S.M[0], S.M[4], S.M[8], 0, S.M[1], S.M[5], S.M[9], 0, S.M[2], S.M[6], S.M[10], 0, S.M[3], S.M[7], S.M[11], 1};
        glUniformMatrix4fv(cCam2, 1, GL_FALSE, C2);
        glUniform2f(cLim2, S.sx, S.sy);
        glUniform1f(cUse2, use2 ? 1.f : 0.f);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, S.tex);
        glUniform1i(cTex2, 1);
    }
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, F.tex);
    glUniform1i(cTex, 0);
    glBindVertexArray(g_camVao);
    glEnable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    auto drawMesh = [&](GLuint vbo, GLuint ibo, GLsizei n, const float M[16]) {
        glUniformMatrix4fv(cMdl, 1, GL_FALSE, M);
        glBindBuffer(GL_ARRAY_BUFFER, vbo);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 24, (void *)0);
        glEnableVertexAttribArray(1);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 24, (void *)12);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
        glDrawElements(GL_TRIANGLES, n, GL_UNSIGNED_INT, nullptr);
    };
    static const float I[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    auto drawAll = [&]() {
        for (int s = 0; s < 2; s++) {
            const RealHands::Hand &H = g_rh.h[s];
            glUniform4f(cFadeP, 0, 0, 0, 1000.f); glUniform3f(cFadeD, 0, 0, 1);
            if (H.camDraw && H.vbo) drawMesh(H.vbo, H.ibo, (GLsizei)H.idx.size(), I);
            if (H.ctlCamDraw) {
                float M[16];
                if (!H.ctlCamBlob && H.cvbo) { const float one[3] = {1, 1, 1}; poseMatrix(H.ctlPose, one, M); drawMesh(H.cvbo, H.cibo, (GLsizei)H.cmIdx.size(), M); }
                else if (H.ctlCamBlob && g_rh.bvbo) { const float sc[3] = {0.125f, 0.14f, 0.2f}; poseMatrix(H.ctlPose, sc, M); drawMesh(g_rh.bvbo, g_rh.bibo, (GLsizei)g_rh.blobIdx.size(), M); }
            }
            if (H.armDraw && H.avbo) {
                glUniform4f(cFadeP, H.armP[0], H.armP[1], H.armP[2], 0.06f); glUniform3f(cFadeD, H.armD[0], H.armD[1], H.armD[2]);
                drawMesh(H.avbo, H.aibo, (GLsizei)((8 - 1) * 18 * 6), I);
            }
        }
        // Portal: el pedazo del cuarto real que marcaste (posición, normal y transparencia por vértice)
        if (g_portal.vbo && g_portal.n > 0) {
            glUniform4f(cFadeP, 0, 0, 0, 1000.f); glUniform3f(cFadeD, 0, 0, 1);
            glUniform1f(cFlat, 1.f);
            glUniformMatrix4fv(cMdl, 1, GL_FALSE, I);
            glBindBuffer(GL_ARRAY_BUFFER, g_portal.vbo);
            glEnableVertexAttribArray(0); glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 28, (void *)0);
            glEnableVertexAttribArray(1); glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 28, (void *)12);
            glEnableVertexAttribArray(2); glVertexAttribPointer(2, 1, GL_FLOAT, GL_FALSE, 28, (void *)24);
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_portal.ibo);
            glDrawElements(GL_TRIANGLES, g_portal.n, GL_UNSIGNED_INT, nullptr);
            glDisableVertexAttribArray(2); glVertexAttrib1f(2, 1.f);
            glUniform1f(cFlat, 0.f);
        }
    };
    // 1) profundidad: queda la superficie de adelante de todo (mano, dedos, brazo, control)
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE); glDepthMask(GL_TRUE); glDepthFunc(GL_LESS); glDisable(GL_BLEND);
    glUniform1f(cDepthOnly, 1.f);
    drawAll();
    // 2) transparencia: el máximo de todas las partes (así el borde de un dedo encima de la palma no se nota;
    //    solo se difumina el contorno de todo junto)
    glDisable(GL_DEPTH_TEST);
    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_TRUE);
    glEnable(GL_BLEND); glBlendEquationSeparate(GL_FUNC_ADD, GL_MAX); glBlendFunc(GL_ONE, GL_ONE);
    glUniform1f(cDepthOnly, 2.f);
    drawAll();
    // 3) color de la superficie de adelante, multiplicado por esa transparencia (la capa usa alfa premultiplicado)
    glEnable(GL_DEPTH_TEST); glDepthFunc(GL_EQUAL); glDepthMask(GL_FALSE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_FALSE);
    glBlendEquation(GL_FUNC_ADD); glBlendFuncSeparate(GL_DST_ALPHA, GL_ZERO, GL_ZERO, GL_ONE);
    glUniform1f(cDepthOnly, 0.f);
    drawAll();
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDepthMask(GL_TRUE);
    glDepthFunc(GL_LESS);
    glDisable(GL_DEPTH_TEST);
    glBindBuffer(GL_ARRAY_BUFFER, 0);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    glBindVertexArray(0);
    g_camDrawn++;
}

static float getFloat(App &app, XrAction a, int h) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a; gi.subactionPath = app.hand[h];
    XrActionStateFloat s{XR_TYPE_ACTION_STATE_FLOAT};
    return XR_SUCCEEDED(xrGetActionStateFloat(app.session, &gi, &s)) && s.isActive ? s.currentState : 0.f;
}
static bool getBool(App &app, XrAction a, int h) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a; gi.subactionPath = app.hand[h];
    XrActionStateBoolean s{XR_TYPE_ACTION_STATE_BOOLEAN};
    return XR_SUCCEEDED(xrGetActionStateBoolean(app.session, &gi, &s)) && s.isActive && s.currentState;
}
static XrVector2f getVec2(App &app, XrAction a, int h) {
    XrActionStateGetInfo gi{XR_TYPE_ACTION_STATE_GET_INFO}; gi.action = a; gi.subactionPath = app.hand[h];
    XrActionStateVector2f s{XR_TYPE_ACTION_STATE_VECTOR2F};
    if (XR_SUCCEEDED(xrGetActionStateVector2f(app.session, &gi, &s)) && s.isActive) return s.currentState;
    return {0, 0};
}

static void applyRefreshRate(App &app) {
    int want = g_targetFps.load();
    if (g_frameGenMode == 1 && g_inGameNow) want = 120;   // completar cuadros: el visor a lo máximo que pueda
    if (!app.hasRefresh || !app.xrRequestDisplayRefreshRateFB || want == app.appliedFps) return;
    uint32_t n = 0;
    app.xrEnumerateDisplayRefreshRatesFB(app.session, 0, &n, nullptr);
    std::vector<float> rates(n);
    if (n) app.xrEnumerateDisplayRefreshRatesFB(app.session, n, &n, rates.data());
    float best = 0;
    for (float r : rates) if (r <= want + 0.5f && r > best) best = r;
    if (best > 0 && XR_SUCCEEDED(app.xrRequestDisplayRefreshRateFB(app.session, best)))
        sendLog("Frecuencia del visor: %.0f Hz (la Mac pide %d)", best, want);
    app.appliedFps = want;
}

/// Toma la imagen decodificada más nueva (si hay) y la conecta a la textura.
static void latchFrame(App &app) {
    if (!g_reader || !app.getClientBuffer || !app.createImage || !app.imageTarget) return;
    AImage *img = nullptr;
    if (AImageReader_acquireLatestImage(g_reader, &img) != AMEDIA_OK || !img) return;
    AHardwareBuffer *hb = nullptr;
    int64_t ts = 0;
    AImage_getTimestamp(img, &ts);
    if (AImage_getHardwareBuffer(img, &hb) != AMEDIA_OK || !hb) { AImage_delete(img); return; }
    EGLClientBuffer cb = app.getClientBuffer(hb);
    const EGLint attr[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLImageKHR egl = app.createImage(app.display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attr);
    if (egl == EGL_NO_IMAGE_KHR) { AImage_delete(img); return; }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, app.tex);
    app.imageTarget(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)egl);
    app.held.push_back({img, egl});
    while (app.held.size() > 3) {   // la GPU ya terminó con las de hace varios cuadros
        app.destroyImage(app.display, app.held.front().egl);
        AImage_delete(app.held.front().img);
        app.held.pop_front();
    }
    uint32_t pid = (uint32_t)(ts / 1000);
    {
        std::lock_guard<std::mutex> lk(g_histLock);
        const PoseSnap &h = g_hist[pid % HIST];
        if (h.id == pid) {
            app.shown = h; app.haveFrame = true;
            float l = (float)(nowMs() - h.at);
            if (l > 0 && l < 1000) g_latency = g_latency * 0.9f + l * 0.1f;
        }
    }
    app.lastFrameMs = nowMs();
    app.frameSeq++;
}

static void sendPose(App &app, const XrView views[2], XrTime t) {
    uint32_t id = ++app.poseId;
    {
        std::lock_guard<std::mutex> lk(g_histLock);
        PoseSnap &h = g_hist[id % HIST];
        h.id = id;
        h.at = nowMs();
        for (int e = 0; e < 2; e++) { h.eye[e].pose = views[e].pose; h.eye[e].fov = views[e].fov; }
    }
    if (g_sock < 0) return;

    std::string s;
    s.reserve(3000);
    char head[96];
    snprintf(head, sizeof head, "{\"type\":\"pose\",\"id\":%u,\"t\":%.1f,\"native\":1,\"views\":[", id, nowMs());
    s += head;
    // Modo Centinela: todo pasa al marco del juego (mirando hacia adelante, en el centro)
    const bool vg = g_vigia.load();
    XrView vv[2] = {views[0], views[1]};
    if (vg) { vigiaXf(vv[0].pose); vigiaXf(vv[1].pose); views = vv; }
    for (int e = 0; e < 2; e++) {
        float P[16], V[16];
        projMatrix(views[e].fov, P);
        viewMatrix(views[e].pose, V);
        s += e ? ",{\"P\":[" : "{\"P\":[";
        for (int i = 0; i < 16; i++) { if (i) s += ','; appendNum(s, P[i]); }
        s += "],\"V\":[";
        for (int i = 0; i < 16; i++) { if (i) s += ','; appendNum(s, V[i]); }
        s += "]}";
    }
    s += "],\"xr\":[1";
    auto add = [&](float v) { s += ','; appendNum(s, v); };
    for (int e = 0; e < 2; e++) {
        const XrPosef &p = views[e].pose;
        add(p.position.x); add(p.position.y); add(p.position.z);
        add(p.orientation.x); add(p.orientation.y); add(p.orientation.z); add(p.orientation.w);
        add(tanf(views[e].fov.angleLeft)); add(tanf(views[e].fov.angleRight));
        add(tanf(views[e].fov.angleDown)); add(tanf(views[e].fov.angleUp));
    }
    for (int h = 0; h < 2; h++) {
        XrSpaceLocation g{XR_TYPE_SPACE_LOCATION}, a{XR_TYPE_SPACE_LOCATION};
        xrLocateSpace(app.gripSpace[h], app.space, t, &g);
        xrLocateSpace(app.aimSpace[h], app.space, t, &a);
        bool present = (g.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (g.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        if (!present) {
            // Sin control: la mano hace de control con gestos (si está activado y la mano se ve)
            const RealHands::Hand &H = g_rh.h[h];
            if (g_handGestures && H.active && H.aimValid && !g_ctlInUse) {
                const bool blk = g_gameMenu.load() || g_gameHidden.load();
                add(1);
                for (int k = 0; k < 2; k++) {   // agarre (desde la palma) y rayo (el de la mano), con el ajuste del usuario
                    XrPosef p = (k == 0 && H.gGripOk) ? H.gGripPose : H.aim;
                    // Calibrada con los controles: como si tuvieras el control agarrado. Si no, lo «0» (probado jugando:
                    // 85° hacia adelante y el giro de cada mano; el agarre ya lo trae). Encima va el ajuste del usuario.
                    if (!handCalApply(h, k, H, p) && !(k == 0 && H.gGripOk)) p.orientation = adjustPose(p.orientation, -85.f, h == 0 ? -130.f : 80.f, h);
                    p.orientation = adjustPose(p.orientation, g_gripPitch[h].load(), g_gripRoll[h].load(), h);
                    if (vg) vigiaXf(p);
                    add(p.position.x); add(p.position.y); add(p.position.z);
                    add(p.orientation.x); add(p.orientation.y); add(p.orientation.z); add(p.orientation.w);
                }
                // (el control que arman los gestos configurados para los juegos)
                const RealHands::Hand::VCtl V = vctl(h);
                add(blk ? 0.f : V.trig); add(blk ? 0.f : V.grip);
                add(blk ? 0.f : V.sx); add(blk ? 0.f : V.sy);
                add(!blk && V.click); add(!blk && V.a); add(!blk && V.b);
                add(!blk && V.trig > 0.1f); add(!blk && V.joy);
                add(!blk && V.a); add(!blk && V.b);
                add(h == 0 ? (float)V.menu : 0.f);
                continue;
            }
            for (int k = 0; k < 27; k++) add(0);
            continue;
        }
        if (!(a.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) a.pose = g.pose;
        if (g_gripPitch[h].load() != 0.f || g_gripRoll[h].load() != 0.f) {   // ajuste del usuario (manos y láser del juego, por mano)
            g.pose.orientation = adjustPose(g.pose.orientation, g_gripPitch[h].load(), g_gripRoll[h].load(), h);
            a.pose.orientation = adjustPose(a.pose.orientation, g_gripPitch[h].load(), g_gripRoll[h].load(), h);
        }
        if (vg) {
            // Modo Centinela: si el visor (apoyado) deja de ver el control, el Quest adivina su posición y se va a
            // cualquier lado; se mantiene la última posición vista (el giro sigue andando)
            static XrVector3f lastG[2], lastA[2];
            static bool have[2] = {false, false};
            // Visto por las cámaras: se usa tal cual. Adivinado (fuera de la vista del visor): se deja moverse un poco
            // desde donde se vio por última vez (hasta 30 cm), así no se va lejos. Imposible: queda donde estaba.
            const bool tracked = (g.locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) != 0;
            // La cámara de la Mac (si está prendida) también ve tus muñecas: mientras el visor ve el control se aprende
            // cómo se corresponden; cuando deja de verlo, la posición sale de la cámara
            CamHands ch;
            { std::lock_guard<std::mutex> lk(g_camHeadLock); ch = g_camHands; }
            const bool camOk = nowMs() - ch.at < 250 && ch.c[h] > 0.3f;
            static float camPos[2][3];
            static bool camUsed[2] = {false, false};
            float pred[3];
            if (g_vg.handOk[h] && tracked) {
                lastG[h] = g.pose.position; lastA[h] = a.pose.position; have[h] = true;
                if (camOk) g_camFit[h].add(ch.x[h], ch.y[h], g.pose.position);
                camUsed[h] = false;
            }
            else if (have[h] && camOk && g_camFit[h].predict(ch.x[h], ch.y[h], pred)) {
                // (no más de 1 m de donde se vio por última vez, y suave)
                float d[3] = {pred[0] - lastG[h].x, pred[1] - lastG[h].y, pred[2] - lastG[h].z};
                float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                if (l > 1.f) for (int k = 0; k < 3; k++) pred[k] = (&lastG[h].x)[k] + d[k] / l;
                if (!camUsed[h]) for (int k = 0; k < 3; k++) camPos[h][k] = (&g.pose.position.x)[k];
                for (int k = 0; k < 3; k++) camPos[h][k] += (pred[k] - camPos[h][k]) * 0.35f;
                camUsed[h] = true;
                float dl[3] = {camPos[h][0] - g.pose.position.x, camPos[h][1] - g.pose.position.y, camPos[h][2] - g.pose.position.z};
                g.pose.position = {camPos[h][0], camPos[h][1], camPos[h][2]};
                a.pose.position.x += dl[0]; a.pose.position.y += dl[1]; a.pose.position.z += dl[2];
            }
            else if (have[h]) {
                if (!g_vg.handOk[h]) { g.pose.position = lastG[h]; a.pose.position = lastA[h]; }
                else {
                    float d[3] = {g.pose.position.x - lastG[h].x, g.pose.position.y - lastG[h].y, g.pose.position.z - lastG[h].z};
                    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                    if (l > 0.30f) {
                        float k = 0.30f / l, cx = d[0] * (k - 1), cy = d[1] * (k - 1), cz = d[2] * (k - 1);
                        g.pose.position.x += cx; g.pose.position.y += cy; g.pose.position.z += cz;
                        a.pose.position.x += cx; a.pose.position.y += cy; a.pose.position.z += cz;
                    }
                }
            }
            // Brazos arriba: por encima del pecho, la altura de las manos se agranda un 30 % (para alcanzar arriba
            // de la cabeza aunque el visor apoyado no vea tan alto)
            if (g_vg.haveRef) {
                const float chest = g_vg.ref[1];
                for (XrPosef *pp : {&g.pose, &a.pose}) if (pp->position.y > chest) pp->position.y = chest + (pp->position.y - chest) * 1.3f;
            }
            vigiaXf(g.pose); vigiaXf(a.pose);
        }
        add(1);
        for (const XrPosef *p : {&g.pose, &a.pose}) {
            add(p->position.x); add(p->position.y); add(p->position.z);
            add(p->orientation.x); add(p->orientation.y); add(p->orientation.z); add(p->orientation.w);
        }
        // con el menú del juego abierto, el juego no recibe los botones (solo la posición de los controles)
        const bool blk = g_gameMenu.load() || g_gameHidden.load();
        XrVector2f st = blk ? XrVector2f{0, 0} : getVec2(app, app.aStick, h);
        add(blk ? 0.f : getFloat(app, app.aTrigger, h)); add(blk ? 0.f : getFloat(app, app.aSqueeze, h));
        add(st.x); add(st.y);
        add(!blk && getBool(app, app.aStickClick, h)); add(!blk && getBool(app, app.aA, h)); add(!blk && getBool(app, app.aB, h));
        add(!blk && getBool(app, app.aTrigTouch, h)); add(!blk && getBool(app, app.aStickTouch, h));
        add(!blk && getBool(app, app.aATouch, h)); add(!blk && getBool(app, app.aBTouch, h));
        add(h == 0 ? getBool(app, app.aMenu, h) : 0.f);
    }
    s += "]}";
    sendPacket(s);
}

/// Modo Centinela: arma los dos «ojos» de la cabeza virtual (lo que el juego cree que es tu cabeza)
static void vigiaViews(App &app, XrTime t, const XrView &real, XrView out[2]) {
    XrSpaceLocation g[2];
    const XrSpaceLocationFlags need = XR_SPACE_LOCATION_POSITION_VALID_BIT;
    int nOk = 0; float sum[3] = {0, 0, 0};
    for (int h = 0; h < 2; h++) {
        g[h] = {XR_TYPE_SPACE_LOCATION};
        bool ok = XR_SUCCEEDED(xrLocateSpace(app.gripSpace[h], app.space, t, &g[h])) && (g[h].locationFlags & need) == need;
        // Con el visor apoyado, cuando pierde un control el Quest a veces lo manda lejísimos (10 m): se descarta lo
        // que no puede ser (a más de 2,5 m del visor, o muy abajo o muy arriba) y queda la última posición buena
        if (ok) {
            const XrVector3f &q = g[h].pose.position;
            float dx = q.x - real.pose.position.x, dy = q.y - real.pose.position.y, dz = q.z - real.pose.position.z;
            ok = dx * dx + dy * dy + dz * dz < 2.5f * 2.5f && dy > -1.6f && dy < 1.4f;
        }
        g_vg.handOk[h] = ok;
        if (ok) {
            sum[0] += g[h].pose.position.x; sum[1] += g[h].pose.position.y; sum[2] += g[h].pose.position.z; nOk++;
        }
    }
    static double lastT = 0;
    double now = nowMs();
    float dt = lastT > 0 ? std::min(0.1f, (float)(now - lastT) / 1000.f) : 0.016f;
    lastT = now;
    // ojos a la altura elegida (≈ 94 % de la estatura: 1,75 m → 1,64 m) medida desde el piso real
    // (app.space ya está corrido por el ajuste de altura: se suma ese corrimiento para que quede igual que las manos)
    // Ojos SIEMPRE a la altura de la cabeza elegida para el Modo Centinela (1,75 m → 1,64 m), medida desde el piso real.
    // (app.space está corrido por el ajuste de altura de la sala: se suma acá y vigiaXf lo resta a todo, así
    // cabeza y manos le llegan al juego en alturas reales)
    g_vg.yOff = app.heightOff + g_vg.corr;
    float eyeH = g_vigiaH.load() * 0.936f + g_vg.yOff;
    if (nOk > 0) {
        float m[3] = {sum[0] / nOk, sum[1] / nOk, sum[2] / nOk};
        if (!g_vg.haveMid) { for (int k = 0; k < 3; k++) g_vg.mid[k] = m[k]; g_vg.haveMid = true; }
        float a = 1.f - expf(-dt / 0.12f);   // suave, para que un golpe con la mano no sacuda la cabeza
        for (int k = 0; k < 3; k++) g_vg.mid[k] += (m[k] - g_vg.mid[k]) * a;
        // Mirás hacia el visor (que está apoyado delante tuyo)
        float fx = real.pose.position.x - g_vg.mid[0], fz = real.pose.position.z - g_vg.mid[2], fl = sqrtf(fx * fx + fz * fz);
        // (solo al empezar, calibrar o recentrar: después queda fijo, así la vista no gira sola al moverte)
        if (fl > 0.2f && (!g_vg.haveFrame || g_vg.needFrame.load())) { g_vg.fwd[0] = fx / fl; g_vg.fwd[1] = fz / fl; }
    }
    // Calibrar: pedido desde el menú (o el botón de menú izquierdo mantenido): 3 s para ponerte en posición
    if (g_vigiaCalReq.exchange(false)) { g_vg.calAt = now + 3000; vigiaSay("calibrar"); }
    if (g_vg.calAt > 0 && now >= g_vg.calAt) {
        g_vg.calAt = 0;
        if (nOk == 2) {
            for (int k = 0; k < 3; k++) g_vg.calMid[k] = g_vg.mid[k];
            {   // mirando al visor desde donde estás ahora
                float fx = real.pose.position.x - g_vg.mid[0], fz = real.pose.position.z - g_vg.mid[2], fl = sqrtf(fx * fx + fz * fz);
                if (fl > 0.2f) { g_vg.fwd[0] = fx / fl; g_vg.fwd[1] = fz / fl; }
            }
            // la cabeza: a tu altura, unos 30 cm detrás de las manos (con los controles delante del pecho)
            g_vg.calHead[0] = g_vg.mid[0] - g_vg.fwd[0] * 0.30f;
            g_vg.calHead[1] = eyeH;
            g_vg.calHead[2] = g_vg.mid[2] - g_vg.fwd[1] * 0.30f;
            g_vg.calibrated = true;
            g_vg.needFrame = true;   // y se recentra: mirando hacia adelante del juego
            g_vg.calNow = true;      // (y se corrige el piso: las manos están delante del pecho)
            vigiaSay("listo");
        } else vigiaSay("sincontroles");
    }
    // El cuerpo sigue a las manos «en promedio» (sin los golpes ni los movimientos rápidos de los brazos): si te
    // corrés para esquivar o te agachás, las manos van con vos y la cabeza también (un poco más, para que se note)
    {
        float k = 1.f - expf(-dt / 0.30f);
        if (!g_vg.haveSlow) { for (int i = 0; i < 3; i++) g_vg.slow[i] = g_vg.mid[i]; g_vg.haveSlow = true; }
        for (int i = 0; i < 3; i++) g_vg.slow[i] += (g_vg.mid[i] - g_vg.slow[i]) * k;
    }
    auto bodyTarget = [&](float out[3]) {
        // de costado (esquivar) se sigue más que hacia adelante: estirar los brazos para agarrar algo no tiene
        // que llevar la cabeza hacia las manos
        const float dx = g_vg.slow[0] - g_vg.ref[0], dz = g_vg.slow[2] - g_vg.ref[2];
        const float along = dx * g_vg.fwd[0] + dz * g_vg.fwd[1];          // hacia el visor (adelante)
        const float sx = dx - along * g_vg.fwd[0], sz = dz - along * g_vg.fwd[1];   // de costado
        out[0] = g_vg.refHead[0] + sx * 1.3f + along * g_vg.fwd[0] * 0.35f;
        out[2] = g_vg.refHead[1] + sz * 1.3f + along * g_vg.fwd[1] * 0.35f;
        // agacharte: las manos bajan con vos (más de 25 cm: bajar los brazos para descansar no cuenta)
        float drop = std::max(0.f, (g_vg.ref[1] - g_vg.slow[1]) - 0.25f) * 1.3f;
        out[1] = eyeH - std::min(0.9f, drop);
    };
    float tgt[3];
    if (g_vg.haveRef) bodyTarget(tgt);
    else { tgt[0] = g_vg.mid[0] - g_vg.fwd[0] * 0.30f; tgt[1] = eyeH; tgt[2] = g_vg.mid[2] - g_vg.fwd[1] * 0.30f; }
    // Con la cámara de la Mac viendo tu cara: la cabeza es la de verdad (no la adivinada por las manos)
    CamHead cam;
    { std::lock_guard<std::mutex> lk(g_camHeadLock); cam = g_camHead; }
    const bool camOn = cam.ok && now - cam.at < 400 && g_vg.haveRef;
    g_vg.camOn = camOn;
    if (camOn) {
        const float rx = -g_vg.fwd[1], rz = g_vg.fwd[0];   // derecha (mirando hacia el visor y la Mac)
        tgt[0] = g_vg.refHead[0] + rx * cam.dx + g_vg.fwd[0] * cam.dz;
        tgt[2] = g_vg.refHead[1] + rz * cam.dx + g_vg.fwd[1] * cam.dz;
        tgt[1] = eyeH + std::max(-1.0f, std::min(0.4f, cam.dy));
        g_vg.camYaw = cam.yaw; g_vg.camPitch = cam.pitch;
    } else { g_vg.camYaw = 0; g_vg.camPitch = 0; }
    float ah = 1.f - expf(-dt / 0.08f);
    for (int k = 0; k < 3; k++) g_vg.head[k] += (tgt[k] - g_vg.head[k]) * ah;
    // Marco del juego: la cabeza queda en el centro mirando hacia adelante (al empezar, al calibrar, al recentrar)
    if (nOk == 2 && g_vg.needFrame.exchange(false)) {
        for (int k = 0; k < 3; k++) g_vg.head[k] = tgt[k];   // (sin el suavizado: ya en su lugar)
        // Piso: las manos delante del pecho están a ≈ 72 % de tu altura (al empezar, a ≈ 64 %: más relajadas)
        {
            const float H = g_vigiaH.load();
            const float handsReal = g_vg.mid[1] - app.heightOff;   // altura de las manos según el Quest
            const float want = H * (g_vg.calNow ? 0.72f : 0.64f);
            const float c = std::max(-1.0f, std::min(1.0f, handsReal - want));
            if (fabsf(handsReal - want) > 1.0f) LOGI("Modo Centinela: piso no corregido (manos a %.2f m: no es creíble)", handsReal);
            else if (g_vg.calNow || (!g_vg.didStartFloor && fabsf(c) > 0.35f)) {   // (al recentrar no se toca el piso)
                g_vg.didStartFloor = true;
                g_vg.corr = c;
                g_vg.yOff = app.heightOff + g_vg.corr;
                eyeH = H * 0.936f + g_vg.yOff;
                tgt[1] = eyeH;
                LOGI("Modo Centinela: piso corregido %.2f m (manos a %.2f m según el Quest)", c, handsReal);
            }
            g_vg.calNow = false;
            g_vg.didStartFloor = true;
        }
        // referencia del cuerpo: dónde están ahora las manos (paradas) y la cabeza 30 cm detrás
        for (int k = 0; k < 3; k++) { g_vg.ref[k] = g_vg.mid[k]; g_vg.slow[k] = g_vg.mid[k]; }
        g_vg.haveSlow = true;
        g_vg.refHead[0] = g_vg.mid[0] - g_vg.fwd[0] * 0.30f; g_vg.refHead[1] = g_vg.mid[2] - g_vg.fwd[1] * 0.30f;
        g_vg.haveRef = true;
        bodyTarget(tgt);
        for (int k = 0; k < 3; k++) g_vg.head[k] = tgt[k];
        g_vg.c[0] = tgt[0]; g_vg.c[1] = tgt[2];
        g_vg.lookYaw = 0; g_vg.lookPitch = 0; g_vg.looking = false;
        g_vg.yaw0 = atan2f(-g_vg.fwd[0], -g_vg.fwd[1]);
        g_vg.haveFrame = true;
    }
    // Mirar alrededor: con el pulgar apoyado en el joystick derecho (sin moverlo ni apretarlo), girar el control
    // gira la cabeza (el doble, para poder mirar atrás); al levantar el pulgar vuelve sola hacia adelante
    {
        XrSpaceLocation a{XR_TYPE_SPACE_LOCATION};
        const bool aOk = XR_SUCCEEDED(xrLocateSpace(app.aimSpace[1], app.space, t, &a)) && (a.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        XrVector2f st = getVec2(app, app.aStick, 1);
        const bool look = aOk && getBool(app, app.aStickTouch, 1) && !getBool(app, app.aStickClick, 1) && fabsf(st.x) < 0.25f && fabsf(st.y) < 0.25f;
        float cy = 0, cp = 0;
        if (aOk) {
            const XrQuaternionf &o = a.pose.orientation;
            const float fx = -2.f * (o.x * o.z + o.w * o.y), fy = -2.f * (o.y * o.z - o.w * o.x), fz = -(1.f - 2.f * (o.x * o.x + o.y * o.y));
            cy = atan2f(-fx, -fz); cp = asinf(std::max(-1.f, std::min(1.f, fy)));
        }
        if (look) {
            if (!g_vg.looking) { g_vg.looking = true; g_vg.lookY0 = cy - g_vg.lookYaw / 2.f; g_vg.lookP0 = cp - g_vg.lookPitch / 1.5f; }
            float dy = cy - g_vg.lookY0;
            while (dy > (float)M_PI) dy -= 2.f * (float)M_PI;
            while (dy < -(float)M_PI) dy += 2.f * (float)M_PI;
            g_vg.lookYaw = dy * 2.f;
            g_vg.lookPitch = std::max(-1.3f, std::min(1.3f, (cp - g_vg.lookP0) * 1.5f));
        } else g_vg.looking = false;   // la vista se queda donde la dejaste (centrar la vuelve adelante)
    }
    // (el giro con el pulgar gira todo el cuerpo: lo aplica vigiaXf a la cabeza y a los controles)
    // (+ el giro de la cabeza que ve la cámara de la Mac)
    float yaw = atan2f(-g_vg.fwd[0], -g_vg.fwd[1]) + g_vg.yawOff + g_vg.camYaw;   // (−Z mira hacia fwd)
    const float pt = g_vg.lookPitch - g_vg.camPitch;
    // giro (Y) y después cabeceo (X propio)
    XrQuaternionf q{cosf(yaw / 2) * sinf(pt / 2), sinf(yaw / 2) * cosf(pt / 2), -sinf(yaw / 2) * sinf(pt / 2), cosf(yaw / 2) * cosf(pt / 2)};
    float right[3] = {cosf(yaw), 0, -sinf(yaw)};
    for (int e = 0; e < 2; e++) {
        out[e] = {XR_TYPE_VIEW};
        float sgn = e == 0 ? -1.f : 1.f;
        out[e].pose.position = {g_vg.head[0] + right[0] * 0.032f * sgn, g_vg.head[1], g_vg.head[2] + right[2] * 0.032f * sgn};
        out[e].pose.orientation = q;
        // campo de visión pensado para la pantalla de la Mac (≈ 16:10): 90° de ancho
        out[e].fov.angleLeft = -atanf(1.0f); out[e].fov.angleRight = atanf(1.0f);
        out[e].fov.angleDown = -atanf(0.65f); out[e].fov.angleUp = atanf(0.65f);   // (≈ la pantalla de la MacBook Air, 1,54:1)
    }
    {   // registro (cada 5 s) para entender por qué los controles se van de lugar
        static double lastLog = 0;
        if (now - lastLog > 5000) {
            lastLog = now;
            sendLog("Modo Centinela: visor %.2f,%.2f,%.2f · mano izq. %.2f,%.2f,%.2f (%s) · mano der. %.2f,%.2f,%.2f (%s) · cabeza %.2f,%.2f,%.2f · piso %+.2f · altura sala %+.2f · mirar %.0f°/%.0f° · cámara Mac %s (giro %.0f°, cabeceo %.0f°)",
                    real.pose.position.x, real.pose.position.y, real.pose.position.z,
                    g[0].pose.position.x, g[0].pose.position.y, g[0].pose.position.z,
                    (g[0].locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) ? "vista" : "adivinada",
                    g[1].pose.position.x, g[1].pose.position.y, g[1].pose.position.z,
                    (g[1].locationFlags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) ? "vista" : "adivinada",
                    g_vg.head[0], g_vg.head[1], g_vg.head[2], g_vg.corr, app.heightOff,
                    g_vg.lookYaw * 57.3f, g_vg.lookPitch * 57.3f, g_vg.camOn ? "sí" : "no", g_vg.camYaw * 57.3f, g_vg.camPitch * 57.3f);
        }
    }
    // Avisos en la Mac: el visor perdió el seguimiento, o un control está dormido (no se movió nada en 8 s)
    if (!g_vg.headTracked) { if (g_vg.untrackedSince == 0) g_vg.untrackedSince = now; } else g_vg.untrackedSince = 0;
    int asleep = -1;
    for (int h = 0; h < 2; h++) {
        const XrVector3f &q = g[h].pose.position;
        float d = fabsf(q.x - g_vg.lastHand[h][0]) + fabsf(q.y - g_vg.lastHand[h][1]) + fabsf(q.z - g_vg.lastHand[h][2]);
        if (d > 0.0005f || g_vg.handStillSince[h] == 0) { g_vg.handStillSince[h] = now; g_vg.lastHand[h][0] = q.x; g_vg.lastHand[h][1] = q.y; g_vg.lastHand[h][2] = q.z; }
        else if (now - g_vg.handStillSince[h] > 8000) asleep = h;
    }
    if (now - g_vg.lastMsg > 4000 && g_vg.calAt == 0) {
        if (g_vg.untrackedSince > 0 && now - g_vg.untrackedSince > 2000) { g_vg.lastMsg = now; vigiaSay("sintracking"); }
        else if (asleep >= 0) { g_vg.lastMsg = now; vigiaSay(asleep == 0 ? "dormidoizq" : "dormidoder"); }
        else if (!g_vg.calibrated) { g_vg.lastMsg = now; vigiaSay(nOk == 2 ? "sincalibrar" : "sincontroles"); }
    }
}

// ---------------------------------------------------------------------------
// Sala: lista de juegos, escritorio y rayos
// ---------------------------------------------------------------------------
static void rotate(const XrQuaternionf &q, const float v[3], float out[3]) {
    float m[9]; quatToMat(q, m);
    for (int i = 0; i < 3; i++) out[i] = m[i] * v[0] + m[3 + i] * v[1] + m[6 + i] * v[2];
}
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

/// Mismo cálculo que hace la Mac para los clics: dónde toca el rayo al panel.
static bool hitPanel(const PanelPose &p, const float o[3], const float d[3], float &t) {
    if (!p.on) return false;
    const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
    float ax[3], ay[3], n[3];
    rotate(p.pose.orientation, ex, ax); rotate(p.pose.orientation, ey, ay); rotate(p.pose.orientation, ez, n);
    float den = dot3(d, n);
    if (fabsf(den) < 1e-4f) return false;
    float c[3] = {p.pose.position.x, p.pose.position.y, p.pose.position.z};
    float co[3] = {c[0] - o[0], c[1] - o[1], c[2] - o[2]};
    t = dot3(co, n) / den;
    if (t <= 0) return false;
    float q[3] = {o[0] + d[0] * t - c[0], o[1] + d[1] * t - c[1], o[2] + d[2] * t - c[2]};
    return fabsf(dot3(q, ax)) <= p.w / 2 && fabsf(dot3(q, ay)) <= p.h / 2;
}

static void mul4(const float a[16], const float b[16], float out[16]) {
    for (int c = 0; c < 4; c++)
        for (int r = 0; r < 4; r++) {
            float s = 0;
            for (int k = 0; k < 4; k++) s += a[k * 4 + r] * b[c * 4 + k];
            out[c * 4 + r] = s;
        }
}

/// Sube la imagen nueva de la lista (si llegó) a su capa.
static void updateList(App &app) {
    std::vector<uint8_t> px;
    int w, h;
    {
        std::lock_guard<std::mutex> lk(g_panelLock);
        if (!g_listNew) return;
        g_listNew = false;
        px = g_listPixels; w = g_listW; h = g_listH;
    }
    if (app.listSc.handle == XR_NULL_HANDLE || app.listSc.w != w || app.listSc.h != h) {
        if (!createSwapchain(app, app.listSc, w, h, mipLevels(w, h))) return;
    }
    Swapchain &s = app.listSc;
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(s.handle, &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(s.handle, &wi);
    glBindTexture(GL_TEXTURE_2D, s.images[idx].image);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    // Los bytes ya están en sRGB (como los dibujó la Mac): la capa sRGB los muestra tal cual
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    if (s.mips > 1) glGenerateMipmap(GL_TEXTURE_2D);
    glBindTexture(GL_TEXTURE_2D, 0);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(s.handle, &ri);
    app.listReady = true;
}

static void releaseDeskImages(App &app) {
    for (auto &h : app.deskHeld) { app.destroyImage(app.display, h.egl); AImage_delete(h.img); }
    app.deskHeld.clear();
}

/// Decodificador del escritorio: se (re)crea con el tamaño que manda la Mac y su imagen va a su propia capa.
static void updateDesk(App &app) {
    int ww = g_deskWantW, wh = g_deskWantH;
    if (ww > 0 && (ww != g_deskW || wh != g_deskH)) {
        glFinish();
        releaseDeskImages(app);
        {
            std::lock_guard<std::mutex> lk(g_deskLock);
            if (g_deskCodec) { AMediaCodec_stop(g_deskCodec); AMediaCodec_delete(g_deskCodec); g_deskCodec = nullptr; }
            if (g_deskReader) { AImageReader_delete(g_deskReader); g_deskReader = nullptr; }
            g_deskW = ww; g_deskH = wh;
            if (AImageReader_newWithUsage(ww, wh, AIMAGE_FORMAT_PRIVATE, AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE, 6, &g_deskReader) != AMEDIA_OK) {
                g_deskReader = nullptr; LOGE("Escritorio: no se pudo crear AImageReader");
            } else {
                ANativeWindow *win = nullptr;
                AImageReader_getWindow(g_deskReader, &win);
                g_deskCodec = AMediaCodec_createDecoderByType("video/avc");
                AMediaFormat *f = AMediaFormat_new();
                AMediaFormat_setString(f, AMEDIAFORMAT_KEY_MIME, "video/avc");
                AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_WIDTH, ww);
                AMediaFormat_setInt32(f, AMEDIAFORMAT_KEY_HEIGHT, wh);
                AMediaFormat_setInt32(f, "low-latency", 1);
                AMediaFormat_setInt32(f, "vendor.qti-ext-dec-low-latency.enable", 1);
                if (!g_deskCodec || AMediaCodec_configure(g_deskCodec, f, win, nullptr, 0) != AMEDIA_OK ||
                    AMediaCodec_start(g_deskCodec) != AMEDIA_OK) {
                    LOGE("Escritorio: no se pudo iniciar el decodificador");
                    if (g_deskCodec) AMediaCodec_delete(g_deskCodec);
                    g_deskCodec = nullptr;
                }
                AMediaFormat_delete(f);
            }
        }
        app.deskReady = false;
        createSwapchain(app, app.deskSc, ww, wh, mipLevels(ww, wh));
        sendLog("Escritorio de la Mac en la app: %dx%d", ww, wh);
        requestDeskKey();
    }

    // Pasar lo decodificado a la imagen
    {
        std::lock_guard<std::mutex> lk(g_deskLock);
        if (!g_deskCodec) return;
        AMediaCodecBufferInfo info;
        ssize_t idx;
        while ((idx = AMediaCodec_dequeueOutputBuffer(g_deskCodec, &info, 0)) >= 0) {
            AMediaCodec_releaseOutputBuffer(g_deskCodec, (size_t)idx, true);
            g_deskDecoded++;
        }
    }
    if (!g_deskReader || app.deskSc.handle == XR_NULL_HANDLE) return;
    AImage *img = nullptr;
    if (AImageReader_acquireLatestImage(g_deskReader, &img) != AMEDIA_OK || !img) return;
    AHardwareBuffer *hb = nullptr;
    if (AImage_getHardwareBuffer(img, &hb) != AMEDIA_OK || !hb) { AImage_delete(img); return; }
    AHardwareBuffer_Desc desc{};
    AHardwareBuffer_describe(hb, &desc);
    AImageCropRect crop{0, 0, g_deskW, g_deskH};
    AImage_getCropRect(img, &crop);
    EGLClientBuffer cb = app.getClientBuffer(hb);
    const EGLint attr[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
    EGLImageKHR egl = app.createImage(app.display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, cb, attr);
    if (egl == EGL_NO_IMAGE_KHR) { AImage_delete(img); return; }
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, app.deskTex);
    app.imageTarget(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)egl);
    app.deskHeld.push_back({img, egl});
    while (app.deskHeld.size() > 3) {
        app.destroyImage(app.display, app.deskHeld.front().egl);
        AImage_delete(app.deskHeld.front().img);
        app.deskHeld.pop_front();
    }
    // Las dimensiones del búfer pueden ser mayores que la imagen (alineación): usar solo la parte válida
    float sx = desc.width ? (float)std::max(1, crop.right) / (float)desc.width : 1.f;
    float sy = desc.height ? (float)std::max(1, crop.bottom) / (float)desc.height : 1.f;

    Swapchain &s = app.deskSc;
    uint32_t si = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(s.handle, &ai, &si))) return;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(s.handle, &wi);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbos[si]);
    glViewport(0, 0, s.w, s.h);
    glUseProgram(app.prog);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, app.deskTex);
    glUniform1i(app.uTex, 0);
    glUniform1f(app.uOff, 0.0f);
    glUniform1f(app.uSx, std::min(1.f, sx));
    glUniform1f(app.uSy, std::min(1.f, sy));
    glUniform1f(app.uSrgb, app.srgb ? 1.0f : 0.0f);
    glUniform1f(app.uKeepA, 0.0f);
    glUniform1f(app.uFilt, 0.0f);
    glBindVertexArray(app.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (s.mips > 1) {
        glBindTexture(GL_TEXTURE_2D, s.images[si].image);
        glGenerateMipmap(GL_TEXTURE_2D);
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(s.handle, &ri);
    app.deskReady = true;
}

struct Ray { float o[3], d[3], len; bool hit; bool hidden = false; int hand = -1; XrPosef pose{}; float trig = 0, squeeze = 0; bool trigTouch = false, thumbTouch = false; };

/// Rayos de los dos controles hasta el panel que tocan (o 2,5 m si no tocan nada).
static int computeRays(App &app, XrTime t, const PanelPose &pl, const PanelPose &pd, Ray rays[2]) {
    int n = 0;
    for (int h = 0; h < 2; h++) {
        XrSpaceLocation a{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(app.aimSpace[h], app.space, t, &a))) continue;
        if (!(a.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) || !(a.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) continue;
        Ray &r = rays[n];
        r.o[0] = a.pose.position.x; r.o[1] = a.pose.position.y; r.o[2] = a.pose.position.z;
        const float fw[3] = {0, 0, -1};
        rotate(a.pose.orientation, fw, r.d);
        r.len = 2.5f; r.hit = false;
        float tt;
        if (hitPanel(pl, r.o, r.d, tt) && tt < r.len) { r.len = tt; r.hit = true; }
        if (hitPanel(pd, r.o, r.d, tt) && tt < r.len) { r.len = tt; r.hit = true; }
        n++;
    }
    return n;
}

static void drawQuad(App &app, const float v[20]) {
    glBindBuffer(GL_ARRAY_BUFFER, app.lineVbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, 20 * sizeof(float), v);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}

struct Bar { float c[3], ax[3], ay[3], hw, hh; bool hot; float a = 0.4f; };

// ---------------------------------------------------------------------------
// Manos: cada control se ve como un objeto (control remoto, celular, banana o lápiz), armado con cajitas.
// Coordenadas del control (pose «aim»): X derecha, Y arriba, -Z hacia adelante (de ahí sale el láser).
// ---------------------------------------------------------------------------
/// Formas de las piezas (todas de tamaño 1, centradas): caja, cilindro, esfera, prisma de 6 lados y cono (eje Z)
enum HandShape { SH_BOX = 0, SH_CYL, SH_SPHERE, SH_HEX, SH_CONE, SH_BANANA, SH_BANANA_NECK, SH_BANANA_TIP, SH_REMOTE, SH_COUNT };
struct HandPart { float c[3], s[3]; float R[9]; float col[3]; float emit; bool skin; int shape; };

// Banana: una superficie lisa a lo largo de una curva, con 5 aristas suaves (como las de verdad).
// t = 0 punta de adelante … 1 donde empieza el cabo; a = ángulo alrededor
static void bananaFrame(float t, float c[3], float T[3], float N[3]) {
    float u = (t - 0.5f) * 2.2f;
    c[0] = 0; c[1] = 0.045f * (1 - cosf(u)) - 0.01f; c[2] = -0.01f + t * 0.19f;
    float dy = 0.045f * 2.2f * sinf(u), dz = 0.19f, l = sqrtf(dy * dy + dz * dz);
    T[0] = 0; T[1] = dy / l; T[2] = dz / l;
    N[0] = 0; N[1] = dz / l; N[2] = -dy / l;   // «arriba» de la banana en cada punto
}
static float bananaRadius(float t) {
    float r = 0.0182f * powf(std::max(0.f, sinf(3.14159265f * std::min(1.f, t * 1.04f))), 0.55f);
    if (t > 0.9f) r = std::max(r, 0.0062f - (t - 0.9f) * 0.012f);   // el cuello se angosta hacia el cabo
    return std::max(r, 0.0024f);
}
static void bananaPoint(float t, float a, float p[3]) {
    float c[3], T[3], N[3];
    bananaFrame(t, c, T, N);
    float rr = bananaRadius(t) * (1 + 0.06f * cosf(5 * a + 0.3f));   // 5 aristas redondeadas
    float x = rr * cosf(a), n = rr * sinf(a) * 0.92f;
    for (int k = 0; k < 3; k++) p[k] = c[k] + (k == 0 ? x : 0) + N[k] * n;
}

/// Mallas de las formas, una detrás de otra (posición + normal); first/count de cada forma
static void buildHandMeshes(std::vector<float> &v, int first[SH_COUNT], int count[SH_COUNT]) {
    auto vert = [&](float x, float y, float z, float nx, float ny, float nz) {
        v.push_back(x); v.push_back(y); v.push_back(z); v.push_back(nx); v.push_back(ny); v.push_back(nz);
    };
    auto start = [&](int sh) { first[sh] = (int)(v.size() / 6); };
    auto end = [&](int sh) { count[sh] = (int)(v.size() / 6) - first[sh]; };
    // caja
    start(SH_BOX);
    const float F[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (auto &f : F) {
        float a[3] = {0, 0, 0};
        if (f[0] != 0) a[1] = 1; else a[0] = 1;
        float b[3] = {f[1] * a[2] - f[2] * a[1], f[2] * a[0] - f[0] * a[2], f[0] * a[1] - f[1] * a[0]};
        const float cr[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
        for (auto &k : cr)
            vert(0.5f * (f[0] + a[0] * k[0] + b[0] * k[1]), 0.5f * (f[1] + a[1] * k[0] + b[1] * k[1]), 0.5f * (f[2] + a[2] * k[0] + b[2] * k[1]), f[0], f[1], f[2]);
    }
    end(SH_BOX);
    // cilindros y cono: N lados, radio 0.5 (el cono termina en punta en z = -0.5)
    auto tube = [&](int sh, int N, bool smooth, bool cone) {
        start(sh);
        const float slope = cone ? 0.5f : 0;   // la normal del cono se inclina hacia adelante
        for (int i = 0; i < N; i++) {
            float a0 = 6.2831853f * i / N, a1 = 6.2831853f * (i + 1) / N, am = (a0 + a1) / 2;
            float c0 = cosf(a0), s0 = sinf(a0), c1 = cosf(a1), s1 = sinf(a1);
            float n0x = smooth ? c0 : cosf(am), n0y = smooth ? s0 : sinf(am), n1x = smooth ? c1 : cosf(am), n1y = smooth ? s1 : sinf(am);
            float k = cone ? 1 / sqrtf(1 + slope * slope) : 1;
            float r0 = cone ? 0.f : 0.5f;   // radio en z = -0.5
            vert(0.5f * c0, 0.5f * s0, 0.5f, n0x * k, n0y * k, -slope * k);
            vert(0.5f * c1, 0.5f * s1, 0.5f, n1x * k, n1y * k, -slope * k);
            vert(r0 * c1, r0 * s1, -0.5f, n1x * k, n1y * k, -slope * k);
            vert(0.5f * c0, 0.5f * s0, 0.5f, n0x * k, n0y * k, -slope * k);
            vert(r0 * c1, r0 * s1, -0.5f, n1x * k, n1y * k, -slope * k);
            vert(r0 * c0, r0 * s0, -0.5f, n0x * k, n0y * k, -slope * k);
            // tapas
            vert(0, 0, 0.5f, 0, 0, 1); vert(0.5f * c1, 0.5f * s1, 0.5f, 0, 0, 1); vert(0.5f * c0, 0.5f * s0, 0.5f, 0, 0, 1);
            if (!cone) { vert(0, 0, -0.5f, 0, 0, -1); vert(0.5f * c0, 0.5f * s0, -0.5f, 0, 0, -1); vert(0.5f * c1, 0.5f * s1, -0.5f, 0, 0, -1); }
        }
        end(sh);
    };
    tube(SH_CYL, 20, true, false);
    // esfera
    start(SH_SPHERE);
    const int ST = 8, SL = 12;
    for (int i = 0; i < ST; i++)
        for (int j = 0; j < SL; j++) {
            float t0 = 3.14159265f * i / ST, t1 = 3.14159265f * (i + 1) / ST, p0 = 6.2831853f * j / SL, p1 = 6.2831853f * (j + 1) / SL;
            float P[4][3] = {{sinf(t0) * cosf(p0), sinf(t0) * sinf(p0), cosf(t0)}, {sinf(t0) * cosf(p1), sinf(t0) * sinf(p1), cosf(t0)},
                             {sinf(t1) * cosf(p1), sinf(t1) * sinf(p1), cosf(t1)}, {sinf(t1) * cosf(p0), sinf(t1) * sinf(p0), cosf(t1)}};
            const int idx[6] = {0, 2, 1, 0, 3, 2};
            for (int k : idx) vert(0.5f * P[k][0], 0.5f * P[k][1], 0.5f * P[k][2], P[k][0], P[k][1], P[k][2]);
        }
    end(SH_SPHERE);
    tube(SH_HEX, 6, false, false);
    tube(SH_CONE, 20, true, true);
    // banana (ya en su lugar y tamaño: se dibuja con escala 1), en tres tramos de color
    auto banana = [&](int sh, float t0, float t1, int NT) {
        start(sh);
        const int NA = 30;
        auto P = [&](float t, float a, float o[3]) { bananaPoint(t, a, o); };
        auto normal = [&](float t, float a, float n[3]) {
            float p0[3], pa[3], pt[3], c[3], T[3], N[3];
            const float e = 0.002f;
            // diferencias hacia adelante (o hacia atrás en el borde t = 1, para no dividir por cero)
            float sg = t + e <= 1 ? 1.f : -1.f;
            P(t, a, p0); P(t, a + e, pa); P(t + sg * e, a, pt);
            float da[3] = {pa[0] - p0[0], pa[1] - p0[1], pa[2] - p0[2]}, dt[3] = {(pt[0] - p0[0]) * sg, (pt[1] - p0[1]) * sg, (pt[2] - p0[2]) * sg};
            n[0] = da[1] * dt[2] - da[2] * dt[1]; n[1] = da[2] * dt[0] - da[0] * dt[2]; n[2] = da[0] * dt[1] - da[1] * dt[0];
            bananaFrame(t, c, T, N);
            float out = n[0] * (p0[0] - c[0]) + n[1] * (p0[1] - c[1]) + n[2] * (p0[2] - c[2]);
            float l = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]) * (out < 0 ? -1 : 1);
            for (int k = 0; k < 3; k++) n[k] /= l;
        };
        for (int i = 0; i < NT; i++)
            for (int j = 0; j < NA; j++) {
                float ta = t0 + (t1 - t0) * i / NT, tb = t0 + (t1 - t0) * (i + 1) / NT;
                float aa = 6.2831853f * j / NA, ab = 6.2831853f * (j + 1) / NA;
                const float q[4][2] = {{ta, aa}, {tb, aa}, {tb, ab}, {ta, ab}};
                const int idx[6] = {0, 1, 2, 0, 2, 3};
                for (int k : idx) {
                    float p[3], n[3];
                    P(q[k][0], q[k][1], p); normal(q[k][0], q[k][1], n);
                    vert(p[0], p[1], p[2], n[0], n[1], n[2]);
                }
            }
        end(sh);
    };
    banana(SH_BANANA, 0.04f, 0.9f, 40);
    banana(SH_BANANA_NECK, 0.9f, 1.0f, 6);
    banana(SH_BANANA_TIP, 0.0f, 0.04f, 3);
}

/// sRGB (como se eligen los colores) → lineal (lo que espera el visor)
static float lin(float c) { return powf(c, 2.2f); }

/// Rotación (columnas = ejes X, Y, Z de la pieza) a partir de giros en X, Y y Z: R = Ry · Rx · Rz
static void eulerBasis(float rx, float ry, float rz, float R[9]) {
    float cx = cosf(rx), sx = sinf(rx), cy = cosf(ry), sy = sinf(ry), cz = cosf(rz), sz = sinf(rz);
    float A[9] = {cz, cx * sz, sx * sz, -sz, cx * cz, sx * cz, 0, -sx, cx};   // Rx · Rz (columnas)
    for (int c = 0; c < 3; c++) {
        float x = A[c * 3], y = A[c * 3 + 1], z = A[c * 3 + 2];
        R[c * 3] = cy * x + sy * z; R[c * 3 + 1] = y; R[c * 3 + 2] = -sy * x + cy * z;
    }
}

/// Armado de los objetos y la mano. Coordenadas del control: X derecha, Y arriba, -Z adelante.
struct HandBuilder {
    std::vector<HandPart> &out;
    float r = 0, g = 0, b = 0;
    bool skin = false;
    /// Botón que se hunde y se ilumina al apretar el gatillo (pressAmt de 0 a 1)
    bool pressing = false;
    float pressAmt = 0, sink = 0, glow = 0;
    explicit HandBuilder(std::vector<HandPart> &o) : out(o) { }
    HandBuilder &color(float rr, float gg, float bb) { r = rr; g = gg; b = bb; return *this; }
    void push(HandPart p) {
        if (pressing) { p.c[1] -= pressAmt * sink; p.emit += pressAmt * glow; }
        out.push_back(p);
    }
    /// Pieza de tamaño (sx, sy, sz) centrada en (cx, cy, cz), girada rx/ry/rz
    void part(int shape, float cx, float cy, float cz, float sx, float sy, float sz, float rx = 0, float ry = 0, float rz = 0, float emit = 0) {
        HandPart p{{cx, cy, cz}, {sx, sy, sz}, {}, {lin(r), lin(g), lin(b)}, emit, skin, shape};
        eulerBasis(rx, ry, rz, p.R);
        push(p);
    }
    /// Pieza con sus ejes dados (X, Y, Z deben ser perpendiculares y de largo 1)
    void oriented(int shape, const float c[3], const float X[3], const float Y[3], const float Z[3], float sx, float sy, float sz, float emit = 0) {
        HandPart p{{c[0], c[1], c[2]}, {sx, sy, sz}, {X[0], X[1], X[2], Y[0], Y[1], Y[2], Z[0], Z[1], Z[2]}, {lin(r), lin(g), lin(b)}, emit, skin, shape};
        push(p);
    }
    void box(float cx, float cy, float cz, float sx, float sy, float sz, float rx = 0, float ry = 0, float rz = 0, float emit = 0) {
        part(SH_BOX, cx, cy, cz, sx, sy, sz, rx, ry, rz, emit);
    }
    /// Botón redondo sobre una cara de arriba (cilindro parado, eje Y)
    void dotY(float cx, float cy, float cz, float d, float h, float emit = 0) { part(SH_CYL, cx, cy, cz, d, d, h, 1.5708f, 0, 0, emit); }
    /// Rectángulo con esquinas redondeadas, acostado (en el plano XZ), de alto h
    void roundedRect(float cx, float cy, float cz, float w, float h, float l, float rad, float emit = 0) {
        box(cx, cy, cz, w, h, l - 2 * rad, 0, 0, 0, emit);
        box(cx, cy, cz, w - 2 * rad, h, l, 0, 0, 0, emit);
        for (int i = 0; i < 4; i++)
            dotY(cx + (i & 1 ? 1 : -1) * (w / 2 - rad), cy, cz + (i & 2 ? 1 : -1) * (l / 2 - rad), 2 * rad, h, emit);
    }
    /// Tramo redondeado entre dos puntos (cápsula: cilindro + esferas en las puntas). dy: grosor en el otro sentido
    void segment(const float a[3], const float bpt[3], float d, float dy = 0, bool caps = true, int shape = SH_CYL) {
        if (dy <= 0) dy = d;
        float v[3] = {bpt[0] - a[0], bpt[1] - a[1], bpt[2] - a[2]};
        float len = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        if (len < 1e-5f) return;
        float z[3] = {v[0] / len, v[1] / len, v[2] / len};
        float ref[3] = {0, 0, 1};
        if (fabsf(z[2]) > 0.9f) { ref[1] = 1; ref[2] = 0; }
        float x[3] = {ref[1] * z[2] - ref[2] * z[1], ref[2] * z[0] - ref[0] * z[2], ref[0] * z[1] - ref[1] * z[0]};
        float xl = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
        for (float &k : x) k /= xl;
        float y[3] = {z[1] * x[2] - z[2] * x[1], z[2] * x[0] - z[0] * x[2], z[0] * x[1] - z[1] * x[0]};
        HandPart p{{(a[0] + bpt[0]) / 2, (a[1] + bpt[1]) / 2, (a[2] + bpt[2]) / 2}, {d, dy, len},
                   {x[0], x[1], x[2], y[0], y[1], y[2], z[0], z[1], z[2]}, {lin(r), lin(g), lin(b)}, 0, skin, shape};
        push(p);
        if (caps)
            for (const float *e : {a, bpt}) {
                HandPart s = p;
                s.shape = SH_SPHERE;
                s.c[0] = e[0]; s.c[1] = e[1]; s.c[2] = e[2];
                s.s[2] = std::min(d, dy);
                push(s);
            }
    }
};

// ---------------------------------------------------------------------------
// Mano: un esqueleto de mano real (palma, cinco dedos con sus falanges, muñeca y antebrazo con manga).
// Se arma la derecha en sus propias coordenadas H (origen en la muñeca; X hacia el pulgar, Y hacia los dedos,
// Z saliendo de la palma) y después se ubica en el control; la izquierda es el espejo.
// Para agarrar un objeto, cada dedo se va cerrando hasta tocarlo (como en la vida real) y el pulgar
// se dobla para llegar al botón.
// ---------------------------------------------------------------------------

/// Tonos de piel (sRGB), de más claro a más oscuro
static const float SKIN_TONES[6][3] = {
    {0.97f, 0.86f, 0.78f}, {0.92f, 0.75f, 0.63f}, {0.80f, 0.60f, 0.45f},
    {0.63f, 0.43f, 0.30f}, {0.44f, 0.29f, 0.19f}, {0.29f, 0.19f, 0.13f},
};

static inline void v3sub(const float a[3], const float b[3], float o[3]) { o[0] = a[0] - b[0]; o[1] = a[1] - b[1]; o[2] = a[2] - b[2]; }
static inline float v3dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static inline void v3cross(const float a[3], const float b[3], float o[3]) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x; o[1] = y; o[2] = z;
}
static inline float v3norm(float a[3]) { float l = sqrtf(v3dot(a, a)); if (l > 1e-9f) { a[0] /= l; a[1] /= l; a[2] /= l; } return l; }
/// Gira v alrededor del eje k (de largo 1) un ángulo a
static void v3rot(const float v[3], const float k[3], float a, float o[3]) {
    float c = cosf(a), s = sinf(a), kv[3], d = v3dot(k, v);
    v3cross(k, v, kv);
    for (int i = 0; i < 3; i++) o[i] = v[i] * c + kv[i] * s + k[i] * d * (1 - c);
}

/// Distancia (con signo) a la superficie de lo que tiene la mano, en coordenadas del control. 1 = lejos (sin objeto)
static float objectSDF(int model, const float p[3]) {
    auto rbox = [&](float cx, float cy, float cz, float hx, float hy, float hz, float r) {
        float q[3] = {fabsf(p[0] - cx) - (hx - r), fabsf(p[1] - cy) - (hy - r), fabsf(p[2] - cz) - (hz - r)};
        float m[3] = {std::max(q[0], 0.f), std::max(q[1], 0.f), std::max(q[2], 0.f)};
        return sqrtf(v3dot(m, m)) + std::min(std::max(q[0], std::max(q[1], q[2])), 0.f) - r;
    };
    switch (model) {
    case 0: return rbox(0, 0, 0.075f, 0.023f, 0.01f, 0.085f, 0.006f);
    case 1: return rbox(0, 0, 0.068f, 0.0355f, 0.0041f, 0.0735f, 0.003f);
    case 2: {
        // la banana avanza casi derecho en Z: buscar solo cerca del tramo que corresponde
        float best = 1;
        int i0 = (int)floorf((p[2] + 0.01f) / 0.19f * 40.f);
        if (i0 < -8 || i0 > 48) {   // lejos de las puntas: alcanza con las puntas
            float t = i0 < 0 ? 0.f : 1.f, c[3], T[3], N[3];
            bananaFrame(t, c, T, N);
            float d[3]; v3sub(p, c, d);
            return sqrtf(v3dot(d, d)) - bananaRadius(t);
        }
        for (int i = std::max(0, i0 - 7); i <= std::min(40, i0 + 7); i++) {
            float t = i / 40.f, c[3], T[3], N[3];
            bananaFrame(t, c, T, N);
            float d[3]; v3sub(p, c, d);
            best = std::min(best, sqrtf(v3dot(d, d)) - bananaRadius(t));
        }
        return best;
    }
    case 3: {
        float z = std::min(0.19f, std::max(-0.01f, p[2]));
        return sqrtf(p[0] * p[0] + p[1] * p[1] + (p[2] - z) * (p[2] - z)) - 0.0046f;
    }
    default: return 1;
    }
}

/// Dónde y cómo se agarra cada objeto (coordenadas del control, mano derecha)
struct ObjGrip {
    float fingers[3];   // hacia dónde apuntan los dedos (estirados)
    float palm[3];      // hacia dónde mira la palma
    float center[3];    // punto del objeto que queda en el medio de la palma
    float thumb[3];     // dónde apoya la yema del pulgar (el botón)
    bool thumbAnchor = false;   // center es dónde va la base del pulgar (no el medio de la palma): el celular
};

/// Mano vacía (con el reloj): relajada, con los dedos un poco doblados, como de costado
static const ObjGrip EMPTY_GRIP = {{0, -0.25f, -1}, {-1, 0, 0}, {0.004f, -0.034f, 0.035f}, {0, 0, 0}};

struct HandXf { float M[9]; float t[3]; };   // control = M · H + t (M en columnas)
static void xfPoint(const HandXf &x, const float h[3], float o[3]) {
    for (int r = 0; r < 3; r++) o[r] = x.M[r] * h[0] + x.M[3 + r] * h[1] + x.M[6 + r] * h[2] + x.t[r];
}
static void xfDir(const HandXf &x, const float h[3], float o[3]) {
    for (int r = 0; r < 3; r++) o[r] = x.M[r] * h[0] + x.M[3 + r] * h[1] + x.M[6 + r] * h[2];
}

// ---------------------------------------------------------------------------
// Mano lisa (estilo «VR hands»): UNA sola superficie suave para palma, dedos y pulgar, hecha una vez al empezar
// a partir de un esqueleto (formas redondeadas que se funden entre sí) y deformada por sus huesos en cada pose.
// Coordenadas H de la mano derecha: origen en la muñeca, X hacia el pulgar, Y hacia los dedos, Z sale de la palma.
// ---------------------------------------------------------------------------
namespace hmesh {

static const int NB = 16;   // huesos: 0 palma · 1…12 dedos (dedo f, falange j → 1 + f·3 + j) · 13…15 pulgar
static int parentOf(int b) { if (b <= 0) return -1; if (b <= 12) return (b - 1) % 3 == 0 ? 0 : b - 1; return b == 13 ? 0 : b - 1; }

// Esqueleto en reposo (mano abierta, como la referencia)
static const float MCP[4][3] = {{0.0255f, 0.0880f, 0}, {0.0060f, 0.0920f, 0}, {-0.0125f, 0.0875f, 0}, {-0.0300f, 0.0785f, 0.001f}};
static const float FAN[4] = {-0.13f, -0.03f, 0.07f, 0.17f};   // en abanico hacia afuera (el índice hacia el pulgar, el meñique hacia afuera)
static const float PH[4][3] = {{0.040f, 0.024f, 0.0205f}, {0.045f, 0.028f, 0.0215f}, {0.042f, 0.027f, 0.021f}, {0.033f, 0.0195f, 0.019f}};
static const float RAD[4][4] = {{0.0094f, 0.0086f, 0.0078f, 0.0068f}, {0.0097f, 0.0089f, 0.0080f, 0.0070f},
                                {0.0092f, 0.0084f, 0.0077f, 0.0067f}, {0.0081f, 0.0074f, 0.0068f, 0.0060f}};
static const float CMC[3] = {0.0190f, 0.0180f, 0.0040f};
static const float TL[3] = {0.042f, 0.031f, 0.027f};
static const float TRAD[4] = {0.0128f, 0.0112f, 0.0102f, 0.0084f};
static float tdir[3];          // dirección del pulgar en reposo

static float Jr[NB][3], Tr[NB][3];   // cabeza y punta de cada hueso, en reposo
struct Cone { float a[3], b[3], ra, rb; int bone; };
static std::vector<Cone> cones;      // formas del esqueleto (la palma además tiene una caja redondeada)

static float sdRoundCone(const float p[3], const float a[3], const float b[3], float r1, float r2) {
    float ba[3], pa[3]; v3sub(b, a, ba); v3sub(p, a, pa);
    float l2 = v3dot(ba, ba), rr = r1 - r2, a2 = l2 - rr * rr, il2 = 1.f / l2;
    float y = v3dot(pa, ba), z = y - l2;
    float xv[3] = {pa[0] * l2 - ba[0] * y, pa[1] * l2 - ba[1] * y, pa[2] * l2 - ba[2] * y};
    float x2 = v3dot(xv, xv), y2 = y * y * l2, z2 = z * z * l2;
    float k = (rr > 0 ? 1.f : rr < 0 ? -1.f : 0.f) * rr * rr * x2;
    if ((z > 0 ? 1.f : -1.f) * a2 * z2 > k) return sqrtf(x2 + z2) * il2 - r2;
    if ((y > 0 ? 1.f : -1.f) * a2 * y2 < k) return sqrtf(x2 + y2) * il2 - r1;
    return (sqrtf(x2 * a2 * il2) + y * rr) * il2 - r1;
}
static float smin(float a, float b, float k) { float h = std::max(k - fabsf(a - b), 0.f) / k; return std::min(a, b) - h * h * k * 0.25f; }
static float sdPalmBox(const float p[3]) {
    float q[3] = {fabsf(p[0] + 0.002f) - 0.028f, fabsf(p[1] - 0.050f) - 0.032f, fabsf(p[2] - 0.0005f) - 0.004f};
    float m[3] = {std::max(q[0], 0.f), std::max(q[1], 0.f), std::max(q[2], 0.f)};
    return sqrtf(v3dot(m, m)) + std::min(std::max(q[0], std::max(q[1], q[2])), 0.f) - 0.0095f;
}

static void buildSkeleton() {
    cones.clear();
    float t[3] = {0.70f, 0.62f, 0.35f};
    v3norm(t);
    for (int k = 0; k < 3; k++) tdir[k] = t[k];
    // palma: huesos del dorso hasta cada nudillo, la base carnosa del pulgar y la muñeca
    for (int f = 0; f < 4; f++) {
        Cone c{{MCP[f][0] * 0.5f, 0.008f, 0}, {MCP[f][0], MCP[f][1], MCP[f][2]}, 0.0125f, RAD[f][0] * 1.15f, 0};
        cones.push_back(c);
    }
    cones.push_back({{0.013f, 0.016f, 0.005f}, {0.027f, 0.046f, 0.0075f}, 0.0135f, 0.0112f, 0});
    for (int s = -1; s <= 1; s += 2) cones.push_back({{s * 0.0105f, -0.075f, 0}, {s * 0.0115f, 0.012f, 0}, 0.0172f, 0.0168f, 0});
    Jr[0][0] = 0; Jr[0][1] = 0; Jr[0][2] = 0; Tr[0][0] = 0; Tr[0][1] = 0.09f; Tr[0][2] = 0;
    // dedos (estirados, en abanico)
    for (int f = 0; f < 4; f++) {
        float d[3] = {-sinf(FAN[f]), cosf(FAN[f]), 0}, p[3] = {MCP[f][0], MCP[f][1], MCP[f][2]};
        for (int j = 0; j < 3; j++) {
            int b = 1 + f * 3 + j;
            float q[3] = {p[0] + d[0] * PH[f][j], p[1] + d[1] * PH[f][j], p[2] + d[2] * PH[f][j]};
            for (int k = 0; k < 3; k++) { Jr[b][k] = p[k]; Tr[b][k] = q[k]; }
            cones.push_back({{p[0], p[1], p[2]}, {q[0], q[1], q[2]}, RAD[f][j], RAD[f][j + 1], b});
            for (int k = 0; k < 3; k++) p[k] = q[k];
        }
    }
    // pulgar
    float p[3] = {CMC[0], CMC[1], CMC[2]};
    for (int j = 0; j < 3; j++) {
        int b = 13 + j;
        float q[3] = {p[0] + tdir[0] * TL[j], p[1] + tdir[1] * TL[j], p[2] + tdir[2] * TL[j]};
        for (int k = 0; k < 3; k++) { Jr[b][k] = p[k]; Tr[b][k] = q[k]; }
        cones.push_back({{p[0], p[1], p[2]}, {q[0], q[1], q[2]}, TRAD[j], TRAD[j + 1], b});
        for (int k = 0; k < 3; k++) p[k] = q[k];
    }
}

/// La forma de la mano en reposo (distancia con signo a su superficie)
static float handSDF(const float p[3]) {
    float palm = sdPalmBox(p);
    size_t i = 0;
    for (; i < cones.size() && cones[i].bone == 0; i++) {
        const Cone &c = cones[i];
        palm = smin(palm, sdRoundCone(p, c.a, c.b, c.ra, c.rb), i >= 5 ? 0.016f : 0.012f);
    }
    // cada dedo se funde suave con la palma, pero NO con los otros dedos (si no, quedan pegados entre sí)
    float d = palm;
    for (int f = 0; f < 4; f++) {
        float fd = 1;
        for (int j = 0; j < 3; j++, i++) fd = std::min(fd, sdRoundCone(p, cones[i].a, cones[i].b, cones[i].ra, cones[i].rb));
        d = std::min(d, smin(palm, fd, 0.007f));
    }
    float td = 1;
    for (int j = 0; j < 3; j++, i++) td = std::min(td, sdRoundCone(p, cones[i].a, cones[i].b, cones[i].ra, cones[i].rb));
    d = std::min(d, smin(palm, td, 0.012f));
    // corte limpio en la muñeca (redondeado apenas)
    float cut = -0.058f - p[1];
    float h = std::max(0.003f - fabsf(d - cut), 0.f) / 0.003f;
    return std::max(d, cut) + h * h * 0.003f * 0.25f;
}

/// «Surface nets»: convierte una distancia con signo en una malla lisa (vértices, normales, triángulos)
template <class F>
static void surfaceNets(F sdf, const float lo[3], const float hi[3], float vox, std::vector<float> &pos, std::vector<float> &nrm, std::vector<uint32_t> &idx) {
    int n[3];
    for (int k = 0; k < 3; k++) n[k] = (int)ceilf((hi[k] - lo[k]) / vox) + 1;
    std::vector<float> v((size_t)n[0] * n[1] * n[2]);
    auto G = [&](int i, int j, int k) -> float & { return v[((size_t)k * n[1] + j) * n[0] + i]; };
    for (int k = 0; k < n[2]; k++)
        for (int j = 0; j < n[1]; j++)
            for (int i = 0; i < n[0]; i++) {
                float p[3] = {lo[0] + i * vox, lo[1] + j * vox, lo[2] + k * vox};
                G(i, j, k) = sdf(p);
            }
    std::vector<int> cell((size_t)n[0] * n[1] * n[2], -1);
    auto C = [&](int i, int j, int k) -> int & { return cell[((size_t)k * n[1] + j) * n[0] + i]; };
    static const int E[12][2] = {{0, 1}, {2, 3}, {4, 5}, {6, 7}, {0, 2}, {1, 3}, {4, 6}, {5, 7}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
    for (int k = 0; k < n[2] - 1; k++)
        for (int j = 0; j < n[1] - 1; j++)
            for (int i = 0; i < n[0] - 1; i++) {
                float c[8];
                int neg = 0;
                for (int q = 0; q < 8; q++) { c[q] = G(i + (q & 1), j + ((q >> 1) & 1), k + ((q >> 2) & 1)); if (c[q] < 0) neg++; }
                if (neg == 0 || neg == 8) continue;
                float s[3] = {0, 0, 0};
                int cnt = 0;
                for (const auto &e : E) {
                    float a = c[e[0]], b = c[e[1]];
                    if ((a < 0) == (b < 0)) continue;
                    float t = a / (a - b);
                    for (int ax = 0; ax < 3; ax++) {
                        float pa = (e[0] >> ax) & 1, pb = (e[1] >> ax) & 1;
                        s[ax] += pa + (pb - pa) * t;
                    }
                    cnt++;
                }
                float p[3] = {lo[0] + (i + s[0] / cnt) * vox, lo[1] + (j + s[1] / cnt) * vox, lo[2] + (k + s[2] / cnt) * vox};
                // llevarlo justo a la superficie y calcular la normal
                for (int it = 0; it < 2; it++) {
                    const float e = vox * 0.25f;
                    float g[3];
                    for (int ax = 0; ax < 3; ax++) {
                        float p1[3] = {p[0], p[1], p[2]}, p0[3] = {p[0], p[1], p[2]};
                        p1[ax] += e; p0[ax] -= e;
                        g[ax] = sdf(p1) - sdf(p0);
                    }
                    v3norm(g);
                    float d = sdf(p);
                    if (fabsf(d) < vox) for (int ax = 0; ax < 3; ax++) p[ax] -= g[ax] * d;
                    if (it == 1) { nrm.push_back(g[0]); nrm.push_back(g[1]); nrm.push_back(g[2]); }
                }
                C(i, j, k) = (int)(pos.size() / 3);
                pos.push_back(p[0]); pos.push_back(p[1]); pos.push_back(p[2]);
            }
    // un cuadrilátero por cada arista de la grilla que cruza la superficie
    for (int k = 1; k < n[2] - 1; k++)
        for (int j = 1; j < n[1] - 1; j++)
            for (int i = 1; i < n[0] - 1; i++) {
                float a = G(i, j, k);
                for (int ax = 0; ax < 3; ax++) {
                    int di = ax == 0, dj = ax == 1, dk = ax == 2;
                    float b = G(i + di, j + dj, k + dk);
                    if ((a < 0) == (b < 0)) continue;
                    int q[4];
                    if (ax == 0) { q[0] = C(i, j - 1, k - 1); q[1] = C(i, j, k - 1); q[2] = C(i, j, k); q[3] = C(i, j - 1, k); }
                    else if (ax == 1) { q[0] = C(i - 1, j, k - 1); q[1] = C(i - 1, j, k); q[2] = C(i, j, k); q[3] = C(i, j, k - 1); }
                    else { q[0] = C(i - 1, j - 1, k); q[1] = C(i, j - 1, k); q[2] = C(i, j, k); q[3] = C(i - 1, j, k); }
                    if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[3] < 0) continue;
                    if (a < 0) std::swap(q[1], q[3]);
                    idx.push_back(q[0]); idx.push_back(q[1]); idx.push_back(q[2]);
                    idx.push_back(q[0]); idx.push_back(q[2]); idx.push_back(q[3]);
                }
            }
}

// La malla de la mano en reposo, y a qué huesos sigue cada vértice
static std::vector<float> rpos, rnrm;
static std::vector<uint32_t> ridx;
static std::vector<uint8_t> boneA;          // el hueso que más pesa en cada vértice
static std::vector<uint8_t> bi;             // hasta 3 huesos por vértice…
static std::vector<float> bw;               // …y cuánto sigue a cada uno (suman 1)

static float boneDist(int b, const float p[3]) {
    if (b == 0) {
        float d = sdPalmBox(p);
        for (const Cone &c : cones) if (c.bone == 0) d = std::min(d, sdRoundCone(p, c.a, c.b, c.ra, c.rb));
        return d;
    }
    for (const Cone &c : cones) if (c.bone == b) return sdRoundCone(p, c.a, c.b, c.ra, c.rb);
    return 1;
}

static void build() {
    buildSkeleton();
    const float lo[3] = {-0.058f, -0.062f, -0.03f}, hi[3] = {0.108f, 0.203f, 0.05f};
    rpos.clear(); rnrm.clear(); ridx.clear();
    surfaceNets(handSDF, lo, hi, 0.0021f, rpos, rnrm, ridx);
    size_t nv = rpos.size() / 3;
    boneA.resize(nv); bi.resize(nv * 3); bw.resize(nv * 3);
    for (size_t i = 0; i < nv; i++) {
        const float *p = &rpos[i * 3];
        // Pesos suaves: cada vértice sigue a los huesos más cercanos, más cuanto más cerca (así la piel se estira
        // en las uniones en vez de romperse)
        float w[NB];
        for (int b = 0; b < NB; b++) {
            float d = std::max(boneDist(b, p), 0.f) + 0.0018f;
            float d2 = d * d;
            w[b] = 1.f / (d2 * d2);
        }
        int top[3] = {-1, -1, -1};
        for (int b = 0; b < NB; b++)
            for (int k = 0; k < 3; k++)
                if (top[k] < 0 || w[b] > w[top[k]]) {
                    for (int m = 2; m > k; m--) top[m] = top[m - 1];
                    top[k] = b;
                    break;
                }
        float sum = 0;
        for (int k = 0; k < 3; k++) sum += w[top[k]];
        for (int k = 0; k < 3; k++) { bi[i * 3 + k] = (uint8_t)top[k]; bw[i * 3 + k] = w[top[k]] / sum; }
        boneA[i] = (uint8_t)top[0];
    }
}

/// Una pose: cuánto se dobla cada articulación de los dedos, cuánto se juntan, y dónde termina el pulgar (en H)
struct Pose { float flex[4][3]; float spread; float thumbT[3]; float thumbD0[3] = {0, 0, 0}; };

/// Transformación de cada hueso: v' = Q[b] + R[b] · (v − Jr[b])
struct BoneXf { float R[NB][9]; float Q[NB][3]; float tip[NB][3]; };

static void matRot(const float k[3], float a, float M[9]) {   // rotación alrededor de k (columnas)
    float c = cosf(a), s = sinf(a), t = 1 - c;
    M[0] = c + k[0] * k[0] * t;        M[3] = k[0] * k[1] * t - k[2] * s; M[6] = k[0] * k[2] * t + k[1] * s;
    M[1] = k[1] * k[0] * t + k[2] * s; M[4] = c + k[1] * k[1] * t;        M[7] = k[1] * k[2] * t - k[0] * s;
    M[2] = k[2] * k[0] * t - k[1] * s; M[5] = k[2] * k[1] * t + k[0] * s; M[8] = c + k[2] * k[2] * t;
}
static void matMul(const float A[9], const float B[9], float O[9]) {
    float T[9];
    for (int c = 0; c < 3; c++) for (int r = 0; r < 3; r++) T[c * 3 + r] = A[r] * B[c * 3] + A[3 + r] * B[c * 3 + 1] + A[6 + r] * B[c * 3 + 2];
    for (int i = 0; i < 9; i++) O[i] = T[i];
}
static void matVec(const float M[9], const float v[3], float o[3]) {
    float x = M[0] * v[0] + M[3] * v[1] + M[6] * v[2], y = M[1] * v[0] + M[4] * v[1] + M[7] * v[2], z = M[2] * v[0] + M[5] * v[1] + M[8] * v[2];
    o[0] = x; o[1] = y; o[2] = z;
}
/// La rotación más corta que lleva la dirección a a la b
static void matFromTo(const float a[3], const float b[3], float M[9]) {
    float ax[3]; v3cross(a, b, ax);
    float s = v3norm(ax), c = v3dot(a, b);
    if (s < 1e-6f) { for (int i = 0; i < 9; i++) M[i] = (i % 4 == 0) ? 1.f : 0.f; return; }
    matRot(ax, atan2f(s, c), M);
}

/// Puntos de un dedo (nudillo, articulaciones y punta) para una pose, y sus huesos
static void fingerPose(int f, const float flex[3], float spread, float pts[4][3], float R[3][9]) {
    float dz = FAN[f] * (spread - 1);                         // juntar (o abrir) los dedos alrededor de Z
    const float Z[3] = {0, 0, 1};
    float Rz[9]; matRot(Z, dz, Rz);
    float side[3] = {cosf(FAN[f]), sinf(FAN[f]), 0};
    matVec(Rz, side, side);
    float acc = 0;
    for (int k = 0; k < 3; k++) pts[0][k] = MCP[f][k];
    for (int j = 0; j < 3; j++) {
        acc += flex[j];
        float Rf[9]; matRot(side, acc, Rf);
        matMul(Rf, Rz, R[j]);
        int b = 1 + f * 3 + j;
        float seg[3]; v3sub(Tr[b], Jr[b], seg);
        matVec(R[j], seg, seg);
        for (int k = 0; k < 3; k++) pts[j + 1][k] = pts[j][k] + seg[k];
    }
}

/// El pulgar: base fija, y las dos falanges llegan a la punta pedida (doblándose hacia «bend»)
static void thumbPose(const float T[3], const float bendHint[3], float P[4][3], const float *d0in = nullptr) {
    float toT[3]; v3sub(T, CMC, toT); v3norm(toT);
    float d0[3] = {tdir[0] * 0.6f + toT[0] * 0.4f, tdir[1] * 0.6f + toT[1] * 0.4f, tdir[2] * 0.6f + toT[2] * 0.4f};
    if (d0in && v3dot(d0in, d0in) > 0.5f) { d0[0] = d0in[0]; d0[1] = d0in[1]; d0[2] = d0in[2]; }
    v3norm(d0);
    for (int k = 0; k < 3; k++) { P[0][k] = CMC[k]; P[1][k] = CMC[k] + d0[k] * TL[0]; }
    float D[3]; v3sub(T, P[1], D);
    float dist = v3norm(D), l1 = TL[1], l2 = TL[2];
    dist = std::max(fabsf(l1 - l2) + 0.004f, std::min(dist, (l1 + l2) * 0.999f));
    float a = (l1 * l1 - l2 * l2 + dist * dist) / (2 * dist), h = sqrtf(std::max(0.f, l1 * l1 - a * a));
    float bend[3] = {bendHint[0], bendHint[1], bendHint[2]};
    float bd = v3dot(bend, D);
    for (int k = 0; k < 3; k++) bend[k] -= D[k] * bd;
    if (v3norm(bend) < 1e-5f) { bend[0] = 0; bend[1] = 0; bend[2] = -1; }
    for (int k = 0; k < 3; k++) { P[2][k] = P[1][k] + D[k] * a + bend[k] * h; P[3][k] = P[1][k] + D[k] * dist; }
}

static void poseBones(const Pose &ps, const float thumbBend[3], BoneXf &X) {
    for (int i = 0; i < 9; i++) X.R[0][i] = (i % 4 == 0) ? 1.f : 0.f;
    for (int k = 0; k < 3; k++) { X.Q[0][k] = 0; X.tip[0][k] = Tr[0][k]; }
    for (int f = 0; f < 4; f++) {
        float pts[4][3], R[3][9];
        fingerPose(f, ps.flex[f], ps.spread, pts, R);
        for (int j = 0; j < 3; j++) {
            int b = 1 + f * 3 + j;
            for (int i = 0; i < 9; i++) X.R[b][i] = R[j][i];
            for (int k = 0; k < 3; k++) { X.Q[b][k] = pts[j][k]; X.tip[b][k] = pts[j + 1][k]; }
        }
    }
    float P[4][3];
    thumbPose(ps.thumbT, thumbBend, P, ps.thumbD0);
    for (int j = 0; j < 3; j++) {
        int b = 13 + j;
        float rd[3], pd[3]; v3sub(Tr[b], Jr[b], rd); v3sub(P[j + 1], P[j], pd);
        v3norm(rd); v3norm(pd);
        matFromTo(rd, pd, X.R[b]);
        for (int k = 0; k < 3; k++) { X.Q[b][k] = P[j][k]; X.tip[b][k] = P[j + 1][k]; }
    }
}

/// Deforma la malla: posición y normal de cada vértice en H
static void skin(const BoneXf &X, std::vector<float> &opos, std::vector<float> &onrm) {
    size_t nv = rpos.size() / 3;
    opos.resize(nv * 3); onrm.resize(nv * 3);
    for (size_t i = 0; i < nv; i++) {
        const float *p = &rpos[i * 3], *n = &rnrm[i * 3];
        float op[3] = {0, 0, 0}, on[3] = {0, 0, 0};
        for (int m = 0; m < 3; m++) {
            int b = bi[i * 3 + m];
            float w = bw[i * 3 + m], t[3], pp[3], nn[3];
            if (w < 1e-4f) continue;
            v3sub(p, Jr[b], t); matVec(X.R[b], t, pp); matVec(X.R[b], n, nn);
            for (int k = 0; k < 3; k++) { op[k] += (pp[k] + X.Q[b][k]) * w; on[k] += nn[k] * w; }
        }
        v3norm(on);
        for (int k = 0; k < 3; k++) { opos[i * 3 + k] = op[k]; onrm[i * 3 + k] = on[k]; }
    }
}

}  // namespace hmesh

/// Formas lisas de los objetos (mismo estilo que la mano): el cuerpo del control remoto, con todos los cantos redondeados
static void addSmoothShapes(std::vector<float> &v, int first[SH_COUNT], int count[SH_COUNT]) {
    std::vector<float> P, N;
    std::vector<uint32_t> I;
    auto remote = [](const float p[3]) {
        const float r = 0.0088f;
        float q[3] = {fabsf(p[0]) - (0.023f - r), fabsf(p[1]) - (0.010f - r), fabsf(p[2] - 0.075f) - (0.085f - r)};
        float m[3] = {std::max(q[0], 0.f), std::max(q[1], 0.f), std::max(q[2], 0.f)};
        return sqrtf(v3dot(m, m)) + std::min(std::max(q[0], std::max(q[1], q[2])), 0.f) - r;
    };
    const float lo[3] = {-0.026f, -0.013f, -0.014f}, hi[3] = {0.026f, 0.013f, 0.164f};
    hmesh::surfaceNets(remote, lo, hi, 0.0009f, P, N, I);
    first[SH_REMOTE] = (int)(v.size() / 6);   // (posición y normal intercaladas, en triángulos sueltos)
    for (uint32_t i : I) { for (int k = 0; k < 3; k++) v.push_back(P[i * 3 + k]); for (int k = 0; k < 3; k++) v.push_back(N[i * 3 + k]); }
    count[SH_REMOTE] = (int)(v.size() / 6) - first[SH_REMOTE];
}

/// Reloj (en coordenadas del control, nunca espejado: siempre se lee bien). wristC: centro de la muñeca,
/// dors: hacia afuera del dorso, toHand: hacia los dedos, across: a lo ancho de la muñeca
struct WatchInfo { bool on; int minuteOfDay; int lit; };
static void addWatch(HandBuilder &B, const WatchInfo &wi, bool leftHand, const float wristC[3], const float dors[3],
                     const float toHand[3], const float across[3], float watchC[3], float watchN[3]) {
    B.skin = false;
    float sc[3] = {wristC[0], wristC[1], wristC[2]};
    B.color(0.13f, 0.13f, 0.14f);                                                   // malla
    B.oriented(SH_CYL, sc, across, dors, toHand, 0.0605f, 0.0395f, 0.019f);
    float c[3], f[3];
    for (int k = 0; k < 3; k++) { c[k] = wristC[k] + dors[k] * 0.0215f; f[k] = c[k] + dors[k] * 0.0041f; }
    // Las «3» apuntan a la mano en la izquierda y al codo en la derecha: así se lee derecho en las dos
    float RT[3] = {toHand[0], toHand[1], toHand[2]};
    if (!leftHand) for (float &k : RT) k = -k;
    float UP[3]; v3cross(dors, RT, UP);   // las «12»
    B.color(0.56f, 0.56f, 0.58f);                                                   // caja
    B.oriented(SH_CYL, c, RT, UP, dors, 0.038f, 0.038f, 0.008f);
    B.color(0.03f, 0.03f, 0.035f);                                                  // vidrio
    B.oriented(SH_CYL, f, RT, UP, dors, 0.033f, 0.033f, 0.0006f, 0.05f);
    auto onFace = [&](float ang, float r, float h, float o[3], float dir[3]) {
        for (int k = 0; k < 3; k++) { dir[k] = UP[k] * cosf(ang) + RT[k] * sinf(ang); o[k] = f[k] + dors[k] * h + dir[k] * r; }
    };
    for (int i = 0; i < 12; i++) {   // marcas: se encienden en ámbar mientras lo mirás
        float o[3], dir[3], side[3];
        onFace(i * 0.5235988f, 0.0135f, 0.0006f, o, dir);
        v3cross(dors, dir, side);
        bool lit = i < wi.lit;
        if (lit) B.color(0.95f, 0.68f, 0.28f); else B.color(0.70f, 0.70f, 0.72f);
        B.oriented(SH_BOX, o, side, dors, dir, i % 3 == 0 ? 0.0016f : 0.0011f, 0.0005f, i % 3 == 0 ? 0.0042f : 0.0028f, lit ? 1.2f : 0.25f);
    }
    float hAng = ((wi.minuteOfDay % 720) / 720.f) * 6.2831853f, mAng = ((wi.minuteOfDay % 60) / 60.f) * 6.2831853f;
    const float hand[2][3] = {{hAng, 0.0075f, 0.0016f}, {mAng, 0.0112f, 0.0011f}};
    B.color(0.95f, 0.95f, 0.96f);
    for (int k = 0; k < 2; k++) {   // agujas
        float o[3], dir[3], side[3];
        onFace(hand[k][0], hand[k][1] * 0.42f, 0.0010f + k * 0.0003f, o, dir);
        v3cross(dors, dir, side);
        B.oriented(SH_BOX, o, side, dors, dir, hand[k][2], 0.0004f, hand[k][1], 0.5f);
    }
    B.color(0.90f, 0.66f, 0.29f);                                                   // centro ámbar
    float cc[3] = {f[0] + dors[0] * 0.0016f, f[1] + dors[1] * 0.0016f, f[2] + dors[2] * 0.0016f};
    B.oriented(SH_CYL, cc, RT, UP, dors, 0.0028f, 0.0028f, 0.0006f, 0.6f);
    B.color(0.50f, 0.50f, 0.52f);                                                   // corona (en las «3»)
    float cr[3] = {c[0] + RT[0] * 0.0195f, c[1] + RT[1] * 0.0195f, c[2] + RT[2] * 0.0195f};
    B.oriented(SH_CYL, cr, dors, UP, RT, 0.004f, 0.004f, 0.003f);
    for (int k = 0; k < 3; k++) { watchC[k] = f[k]; watchN[k] = dors[k]; }
}

/// Dónde quedó el reloj de cada mano (coordenadas de su control), para saber cuándo lo mirás
static float g_watchPos[2][3] = {{0, 0, 0.1f}, {0, 0, 0.1f}}, g_watchNrm[2][3] = {{-1, 0, 0}, {1, 0, 0}};

/// Orientación de la mano para un agarre (columnas X, Y, Z de H en el control), con la palma sobre g.center
static HandXf gripFrame(const ObjGrip &g) {
    float Y[3] = {g.fingers[0], g.fingers[1], g.fingers[2]}, Z[3] = {g.palm[0], g.palm[1], g.palm[2]};
    v3norm(Y);
    float yz = v3dot(Y, Z);
    for (int k = 0; k < 3; k++) Z[k] -= Y[k] * yz;
    v3norm(Z);
    float Xa[3]; v3cross(Y, Z, Xa);
    HandXf X{};
    for (int k = 0; k < 3; k++) { X.M[k] = Xa[k]; X.M[3 + k] = Y[k]; X.M[6 + k] = Z[k]; }
    const float palmC[3] = {0, 0.050f, 0.0145f};
    float pc[3]; xfDir(X, g.thumbAnchor ? hmesh::CMC : palmC, pc);
    for (int k = 0; k < 3; k++) X.t[k] = g.center[k] - pc[k];
    return X;
}
static void xfInv(const HandXf &X, const float c[3], float h[3]) {   // del control a H (M es una rotación)
    float d[3]; v3sub(c, X.t, d);
    for (int r = 0; r < 3; r++) h[r] = X.M[r * 3] * d[0] + X.M[r * 3 + 1] * d[1] + X.M[r * 3 + 2] * d[2];
}

/// Cierre de un dedo t (0 estirado … 1 puño) → ángulo de cada articulación
static void flexFor(float t, float out[3]) { out[0] = 1.45f * t; out[1] = 1.72f * std::min(1.f, t * 1.15f); out[2] = 1.15f * t; }

/// Agarre de un objeto (una pose por objeto): la palma apoyada, cada dedo cerrado hasta tocarlo y el pulgar en su
/// botón. Después se revisa la malla entera: si algún punto atraviesa el objeto, se abre ese dedo o se aleja la mano.
static void solveHolding(int model, float press, const ObjGrip &g, HandXf &X, hmesh::Pose &ps, float thumbBend[3]) {
    using namespace hmesh;
    X = gripFrame(g);
    float Zc[3] = {X.M[6], X.M[7], X.M[8]};
    auto sdfCtlH = [&](const float h[3]) { float p[3]; xfPoint(X, h, p); return objectSDF(model, p); };
    // 1) alejar la palma (y la base del pulgar) hasta que no toque el objeto
    size_t nv = rpos.size() / 3;
    for (int it = 0; it < 90; it++) {
        float worst = 1;
        for (size_t i = 0; i < nv; i += 3) if (boneA[i] == 0) worst = std::min(worst, sdfCtlH(&rpos[i * 3]));
        if (worst > 0.0006f) break;
        for (int k = 0; k < 3; k++) X.t[k] -= Zc[k] * 0.001f;
    }
    // 2) dedos: se cierran hasta tocar
    ps.spread = 0.45f;   // al agarrar algo, los dedos se juntan
    float tf[4];
    for (int f = 0; f < 4; f++) {
        tf[f] = 1;
        for (float t = 0; t <= 1.0001f; t += 0.02f) {
            float fl[3], pts[4][3], R[3][9];
            flexFor(t, fl);
            fingerPose(f, fl, ps.spread, pts, R);
            bool touch = false;
            for (int j = 0; j < 3 && !touch; j++)
                for (int s = 0; s <= 4 && !touch; s++) {
                    float u = s / 4.f, q[3];
                    for (int k = 0; k < 3; k++) q[k] = pts[j][k] + (pts[j + 1][k] - pts[j][k]) * u;
                    float r = RAD[f][j] + (RAD[f][j + 1] - RAD[f][j]) * u;
                    if (sdfCtlH(q) < r * 1.02f) touch = true;
                }
            if (touch) { tf[f] = std::max(0.f, t - 0.02f); break; }
        }
    }
    // 3) pulgar: su yema sobre el botón (al apretar, baja). Se prueban varios caminos (más por afuera del objeto,
    //    más o menos inclinado) y se queda con el que no lo atraviesa
    float tc[3] = {g.thumb[0], g.thumb[1] - press * 0.0045f, g.thumb[2]};
    auto gradH = [&](const float m[3], float gr[3]) {
        const float e = 0.002f;
        for (int k = 0; k < 3; k++) {
            float p1[3] = {m[0], m[1], m[2]}, p0[3] = {m[0], m[1], m[2]};
            p1[k] += e; p0[k] -= e;
            gr[k] = sdfCtlH(p1) - sdfCtlH(p0);
        }
        v3norm(gr);
    };
    auto thumbDepth = [&](const float P[4][3]) {   // cuánto se mete el pulgar en el objeto (0 = nada)
        float worst = 0;
        for (int j = 0; j < 3; j++)
            for (int s = 0; s <= 6; s++) {
                float u = s / 6.f, q[3];
                for (int k = 0; k < 3; k++) q[k] = P[j][k] + (P[j + 1][k] - P[j][k]) * u;
                float r = TRAD[j] + (TRAD[j + 1] - TRAD[j]) * u;
                worst = std::max(worst, r - sdfCtlH(q));
            }
        return worst;
    };
    float lift = 0;
    for (int it = 0; it < 10; it++) {
        for (int f = 0; f < 4; f++) flexFor(tf[f], ps.flex[f]);
        float tcl[3] = {tc[0], tc[1] + lift, tc[2]};
        xfInv(X, tcl, ps.thumbT);
        {
            float out0[3]; gradH(CMC, out0);   // hacia afuera del objeto desde la base del pulgar
            float toT[3]; v3sub(ps.thumbT, CMC, toT); v3norm(toT);
            float bestScore = 1e9f;
            for (float w : {0.4f, 0.2f, 0.6f, 0.0f, 0.8f})
                for (float o : {0.0f, 0.35f, 0.7f, 1.1f}) {
                    float d0[3];
                    for (int k = 0; k < 3; k++) d0[k] = tdir[k] * (1 - w) + toT[k] * w + out0[k] * o;
                    v3norm(d0);
                    float mid[3];
                    for (int k = 0; k < 3; k++) mid[k] = CMC[k] + d0[k] * TL[0] * 1.4f;
                    float gr[3]; gradH(mid, gr);
                    float bh[3] = {gr[0], gr[1], gr[2] - 0.3f};
                    float P[4][3];
                    thumbPose(ps.thumbT, bh, P, d0);
                    float reach[3]; v3sub(P[3], ps.thumbT, reach);
                    float score = thumbDepth(P) * 10 + sqrtf(v3dot(reach, reach)) * 4 + o * 0.002f + fabsf(w - 0.4f) * 0.001f;
                    if (score < bestScore) {
                        bestScore = score;
                        for (int k = 0; k < 3; k++) { ps.thumbD0[k] = d0[k]; thumbBend[k] = bh[k]; }
                    }
                }
        }
        // 4) revisar la malla: ¿algo atraviesa el objeto?
        BoneXf B;
        poseBones(ps, thumbBend, B);
        std::vector<float> P, N;
        skin(B, P, N);
        bool fingerHit[4] = {false, false, false, false}, thumbHit = false, any = false;
        for (size_t i = 0; i < nv; i++) {
            if (sdfCtlH(&P[i * 3]) > -0.0007f) continue;
            int b = boneA[i];
            if (b >= 1 && b <= 12) fingerHit[(b - 1) / 3] = true;
            else if (b >= 13) thumbHit = true;
            else continue;   // la palma ya se separó antes
            any = true;
        }
#ifdef HAND_DEBUG
        { float P[4][3]; thumbPose(ps.thumbT, thumbBend, P, ps.thumbD0); fprintf(stderr, "  vuelta %d: lift %.1f mm, pulgar adentro %.1f mm, dedos %d%d%d%d\n", it, lift * 1000, thumbDepth(P) * 1000, fingerHit[0], fingerHit[1], fingerHit[2], fingerHit[3]); }
#endif
        if (!any) break;
        for (int f = 0; f < 4; f++) if (fingerHit[f]) tf[f] = std::max(0.f, tf[f] - 0.04f);
        if (thumbHit) lift += 0.0012f;
    }
    for (int f = 0; f < 4; f++) flexFor(tf[f], ps.flex[f]);
}

/// Estado de cada mano: la pose (la mano del reloj se mueve suave con lo que tocás del control) y su malla deformada
struct HandState {
    bool ready = false, holding = false;
    long key = -1;                  // agarre armado (objeto, mano, cuánto apretás)
    HandXf X{};                     // dónde está la mano en el control (antes del espejo)
    float t[4] = {0.2f, 0.25f, 0.28f, 0.32f}, spread = 1, thumb[3] = {0.052f, 0.09f, 0.026f};   // mano libre (suavizada)
    float drawn[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
    std::vector<float> vbuf;        // posición + normal intercaladas, ya en el control
    GLuint vao = 0, vbo = 0;
    bool upload = false;
};
static HandState g_hs[2];
static GLuint g_handEbo = 0;
static GLsizei g_handIdx = 0;

/// Deforma la malla con una pose y la pasa al control (espejada si es la izquierda)
static void handSkinTo(HandState &hs, const hmesh::Pose &ps, const float thumbBend[3], bool left) {
    hmesh::BoneXf B;
    hmesh::poseBones(ps, thumbBend, B);
    std::vector<float> P, N;
    hmesh::skin(B, P, N);
    size_t nv = P.size() / 3;
    hs.vbuf.resize(nv * 6);
    for (size_t i = 0; i < nv; i++) {
        float p[3], n[3];
        xfPoint(hs.X, &P[i * 3], p); xfDir(hs.X, &N[i * 3], n);
        if (left) { p[0] = -p[0]; n[0] = -n[0]; }
        for (int k = 0; k < 3; k++) { hs.vbuf[i * 6 + k] = p[k]; hs.vbuf[i * 6 + 3 + k] = n[k]; }
    }
    hs.upload = true;
}

/// Una vez por cuadro: la pose de cada mano. La del láser agarra su objeto; la otra se mueve un poco cuando apoyás
/// el índice en el gatillo, apretás el grip o apoyás el pulgar
struct HandTouch { bool present; float trig, squeeze; bool trigTouch, thumbTouch; };
static void handsUpdate(int activeHand, int model, const HandTouch in[2], float dt, float press[2]);

/// Una mano: con el objeto (la del láser) o vacía con el reloj. press: cuánto está apretado el gatillo (0 a 1):
/// el pulgar aprieta un botón del objeto
/// Cómo se agarra cada objeto (mano derecha; la izquierda es el espejo). El pulgar apoya su yema en el botón
static ObjGrip gripFor(int model) {
    switch (model) {
    default:
    case 0: return {{-0.75f, -0.3f, -0.6f}, {-0.45f, 0.85f, 0.1f}, {0.0f, -0.002f, 0.090f}, {0.0f, 0.0225f, 0.0375f}};
    case 1: return {{-0.8f, -0.12f, -0.6f}, {-0.8f, 0.55f, 0.1f}, {0.044f, -0.004f, 0.11f}, {0.0083f, 0.0135f, 0.0685f}, true};   // la base del pulgar en el borde derecho
    case 2: {
        float c[3], T[3], N[3], t3[3], T3[3], N3[3];
        bananaFrame(0.5f, c, T, N);
        bananaFrame(0.36f, t3, T3, N3);
        float r3 = bananaRadius(0.36f) * 0.92f + 0.0086f;
        return {{-0.75f, -0.3f, -0.6f}, {-0.45f, 0.85f, 0.1f}, {c[0], c[1], c[2]}, {t3[0] + N3[0] * r3, t3[1] + N3[1] * r3, t3[2] + N3[2] * r3}};
    }
    case 3: return {{-0.75f, -0.3f, -0.6f}, {-0.45f, 0.85f, 0.1f}, {0.0f, 0.0f, 0.11f}, {0.0f, 0.0132f, 0.07f}};
    }
}

/// Lo que va en una mano además de la mano misma: el objeto (la del láser) o el reloj (la otra)
static void handParts(int model, bool left, bool holding, int skin, float press, std::vector<HandPart> &out, const WatchInfo *watch = nullptr) {
    (void)skin;
    out.clear();
    HandBuilder B(out);
    B.pressAmt = press;
    if (holding) switch (model) {
    default:
    case 0: {   // control remoto de TV: grafito con bordes redondeados, botones de goma y el OK ámbar de VRmin
        const float H = 0.02f, L = 0.17f, cz = 0.075f;
        B.color(0.15f, 0.15f, 0.16f);
        B.part(SH_REMOTE, 0, 0, 0, 1, 1, 1);                                                  // cuerpo liso, todo redondeado
        B.color(0.19f, 0.19f, 0.20f);
        B.roundedRect(0, H / 2 + 0.0002f, cz, 0.029f, 0.0006f, L - 0.02f, 0.006f);          // cara de arriba, más clara
        B.color(0.22f, 0.04f, 0.04f);
        B.part(SH_SPHERE, 0, 0.0004f, cz - L / 2 + 0.0004f, 0.018f, 0.0055f, 0.003f, 0, 0, 0, 0.15f);   // ventanita del infrarrojo
        B.color(0.80f, 0.17f, 0.15f);
        B.dotY(-0.012f, 0.0108f, 0.004f, 0.0085f, 0.0035f);                                  // encendido (rojo)
        B.color(0.30f, 0.30f, 0.32f);
        B.dotY(0.012f, 0.0108f, 0.004f, 0.0085f, 0.003f);                                    // entrada
        // Aro de las flechas, con marcas en las cuatro direcciones
        B.color(0.28f, 0.28f, 0.30f);
        B.dotY(0, 0.0108f, 0.032f, 0.031f, 0.003f);
        B.color(0.17f, 0.17f, 0.18f);
        B.dotY(0, 0.0109f, 0.032f, 0.0165f, 0.0032f);                                         // canaleta alrededor del OK
        B.color(0.55f, 0.55f, 0.57f);
        for (int i = 0; i < 4; i++) {
            float a = i * 1.5708f;
            B.box(sinf(a) * 0.0118f, 0.0125f, 0.032f + cosf(a) * 0.0118f, 0.0034f, 0.0006f, 0.0016f, 0, a, 0);
        }
        B.color(0.90f, 0.66f, 0.29f);
        B.pressing = true; B.sink = 0.0009f; B.glow = 0.9f;                                   // el pulgar lo aprieta
        B.dotY(0, 0.0118f, 0.032f, 0.0125f, 0.004f, 0.35f);                                  // OK ámbar
        B.pressing = false;
        // Volver, inicio y menú
        B.color(0.30f, 0.30f, 0.32f);
        for (int c = -1; c <= 1; c++) B.dotY(c * 0.0125f, 0.0108f, 0.056f, 0.0078f, 0.003f);
        B.color(0.62f, 0.62f, 0.64f);
        B.box(0, 0.0124f, 0.056f, 0.0028f, 0.0005f, 0.0028f);                                // marca del botón de inicio
        B.box(-0.0125f, 0.0124f, 0.056f, 0.0032f, 0.0005f, 0.0008f);                          // volver
        for (int k = -1; k <= 1; k++) B.box(0.0125f, 0.0124f, 0.056f + k * 0.0012f, 0.003f, 0.0005f, 0.0005f);   // menú
        // Volumen y canales: dos teclas largas redondeadas a los costados, y silencio al medio
        B.color(0.27f, 0.27f, 0.29f);
        for (int s = -1; s <= 1; s += 2) {
            float a[3] = {s * 0.0125f, 0.0108f, 0.071f}, b[3] = {s * 0.0125f, 0.0108f, 0.088f};
            B.segment(a, b, 0.0078f, 0.003f);
        }
        B.dotY(0, 0.0108f, 0.0795f, 0.0068f, 0.003f);
        B.color(0.62f, 0.62f, 0.64f);
        for (int s = -1; s <= 1; s += 2) {
            B.box(s * 0.0125f, 0.0126f, 0.0725f, 0.003f, 0.0005f, 0.0007f);                  // +
            B.box(s * 0.0125f, 0.0126f, 0.0865f, 0.003f, 0.0005f, 0.0007f);                  // −
        }
        B.box(-0.0125f, 0.0126f, 0.0725f, 0.0007f, 0.0005f, 0.003f);
        B.box(0.0125f, 0.0126f, 0.0725f, 0.0007f, 0.0005f, 0.003f);
        // Números: 3 × 4 botones redondos chicos
        B.color(0.25f, 0.25f, 0.27f);
        for (int row = 0; row < 4; row++)
            for (int col = -1; col <= 1; col++) B.dotY(col * 0.0118f, 0.0107f, 0.104f + row * 0.0118f, 0.0078f, 0.0028f);
        B.color(0.5f, 0.5f, 0.52f);
        for (int row = 0; row < 4; row++)
            for (int col = -1; col <= 1; col++) B.box(col * 0.0118f, 0.0122f, 0.104f + row * 0.0118f, 0.0012f, 0.0004f, 0.0022f);
        // Tapa de las pilas (línea atrás)
        B.color(0.11f, 0.11f, 0.12f);
        B.box(0, -H / 2 - 0.0002f, 0.115f, 0.034f, 0.0005f, 0.0007f);
        break;
    }
    case 1: {   // celular tipo iPhone: marco de titanio, vidrio negro, isla dinámica, íconos y cámara triple atrás
        const float w = 0.071f, l = 0.147f, t = 0.0082f, rad = 0.0105f, cz = 0.068f;
        B.color(0.56f, 0.55f, 0.53f);                                             // marco (titanio natural)
        B.roundedRect(0, 0, cz, w, t, l, rad);
        B.color(0.36f, 0.35f, 0.34f);                                             // vidrio de atrás, mate
        B.roundedRect(0, -t / 2 - 0.0002f, cz, w - 0.0016f, 0.0006f, l - 0.0016f, rad - 0.0008f);
        B.color(0.02f, 0.02f, 0.025f);                                            // vidrio de adelante (bordes negros)
        B.roundedRect(0, t / 2 + 0.0002f, cz, w - 0.0014f, 0.0006f, l - 0.0014f, rad - 0.0007f);
        // Pantalla encendida: fondo con un degradé grafito → ámbar suave (en franjas), esquinas redondeadas
        const float sw = w - 0.0062f, sl = l - 0.0062f, sy = t / 2 + 0.0007f;
        const int NB = 8;
        for (int i = 0; i < NB; i++) {
            float k = i / (NB - 1.f), zz = cz - sl / 2 + sl * (i + 0.5f) / NB;
            B.color(0.10f + 0.22f * k, 0.09f + 0.13f * k, 0.11f + 0.03f * k);
            if (i == 0 || i == NB - 1) {
                float e = i == 0 ? -1 : 1, rr = 0.0078f;
                B.box(0, sy, zz, sw - 2 * rr, 0.0004f, sl / NB, 0, 0, 0, 0.35f);
                B.box(0, sy, zz - e * rr / 2, sw, 0.0004f, sl / NB - rr, 0, 0, 0, 0.35f);
                for (int s = -1; s <= 1; s += 2) B.dotY(s * (sw / 2 - rr), sy, zz + e * (sl / NB / 2 - rr), 2 * rr, 0.0004f, 0.35f);
            } else {
                B.box(0, sy, zz, sw, 0.0004f, sl / NB + 0.0001f, 0, 0, 0, 0.35f);
            }
        }
        const float top = cz - sl / 2, iy = sy + 0.0003f;
        // Isla dinámica (arriba = adelante, -Z)
        B.color(0, 0, 0);
        {
            float a[3] = {-0.0085f, iy, top + 0.0078f}, b[3] = {0.0085f, iy, top + 0.0078f};
            B.segment(a, b, 0.0056f, 0.0004f);
        }
        // Hora y batería
        B.color(0.95f, 0.95f, 0.96f);
        B.box(-0.021f, iy, top + 0.0078f, 0.009f, 0.0003f, 0.0028f, 0, 0, 0, 0.6f);
        B.box(0.022f, iy, top + 0.0078f, 0.006f, 0.0003f, 0.0026f, 0, 0, 0, 0.6f);
        // Íconos de apps: 4 × 5 cuadraditos redondeados, y la barra de abajo con 4 más
        const float ic[6][3] = {{0.95f, 0.62f, 0.25f}, {0.30f, 0.75f, 0.45f}, {0.92f, 0.92f, 0.93f},
                                {0.95f, 0.40f, 0.35f}, {0.55f, 0.55f, 0.60f}, {0.98f, 0.80f, 0.30f}};
        const float isz = 0.0118f, gap = (sw - 4 * isz) / 5;
        auto icon = [&](float x, float z, const float *c) {
            B.color(c[0], c[1], c[2]);
            B.roundedRect(x, iy, z, isz, 0.0004f, isz, 0.0028f, 0.45f);
        };
        for (int row = 0; row < 5; row++)
            for (int col = 0; col < 4; col++) {
                // el ícono que queda debajo del pulgar se hunde un poco y se ilumina al apretar
                B.pressing = row == 2 && col == (left ? 1 : 2); B.sink = 0.0003f; B.glow = 1.2f;
                icon(-sw / 2 + gap + isz / 2 + col * (isz + gap), top + 0.02f + isz / 2 + row * (isz + 0.0085f), ic[(row * 4 + col * 3 + row) % 6]);
                B.pressing = false;
            }
        B.color(0.34f, 0.32f, 0.31f);
        B.roundedRect(0, iy - 0.0001f, cz + sl / 2 - 0.0135f, sw - 0.007f, 0.0004f, 0.0185f, 0.0065f, 0.3f);   // barra de abajo
        for (int col = 0; col < 4; col++) icon(-sw / 2 + gap + isz / 2 + col * (isz + gap), cz + sl / 2 - 0.0135f, ic[(col * 2 + 1) % 6]);
        B.color(0.9f, 0.9f, 0.9f);
        {
            float a[3] = {-0.011f, iy, cz + sl / 2 - 0.0022f}, b[3] = {0.011f, iy, cz + sl / 2 - 0.0022f};
            B.segment(a, b, 0.0011f, 0.0003f);                                                          // rayita de inicio
        }
        // Botones del costado: acción y volumen a la izquierda, encendido a la derecha (redondeados)
        B.color(0.50f, 0.49f, 0.47f);
        auto side = [&](float x, float z0, float z1) {
            float a[3] = {x, 0, cz + z0}, b[3] = {x, 0, cz + z1};
            B.segment(a, b, 0.0018f, 0.0034f);
        };
        side(-w / 2 - 0.0004f, -0.05f, -0.044f);
        side(-w / 2 - 0.0004f, -0.036f, -0.026f);
        side(-w / 2 - 0.0004f, -0.022f, -0.012f);
        side(w / 2 + 0.0004f, -0.034f, -0.018f);
        // Cámara atrás (arriba a la izquierda vista de atrás = derecha vista de adelante)
        const float mx = w / 2 - 0.0215f, mz = cz - l / 2 + 0.0215f, by = -t / 2;
        B.color(0.40f, 0.39f, 0.38f);
        B.roundedRect(mx, by - 0.0008f, mz, 0.035f, 0.0016f, 0.035f, 0.0075f);
        const float lens[3][2] = {{-0.008f, -0.008f}, {-0.008f, 0.008f}, {0.0085f, 0}};
        for (const auto &p : lens) {
            B.color(0.55f, 0.54f, 0.52f); B.dotY(mx + p[0], by - 0.0022f, mz + p[1], 0.0138f, 0.0024f);   // aro
            B.color(0.05f, 0.05f, 0.07f); B.dotY(mx + p[0], by - 0.0034f, mz + p[1], 0.0108f, 0.0004f);   // vidrio
            B.color(0.12f, 0.14f, 0.24f); B.dotY(mx + p[0], by - 0.0036f, mz + p[1], 0.005f, 0.0004f, 0.15f);
        }
        B.color(0.95f, 0.92f, 0.80f); B.dotY(mx + 0.0085f, by - 0.0017f, mz - 0.0105f, 0.0042f, 0.0004f);   // flash
        B.color(0.08f, 0.08f, 0.09f); B.dotY(mx + 0.0085f, by - 0.0017f, mz + 0.0105f, 0.0038f, 0.0004f);   // sensor
        break;
    }
    case 2: {   // banana: superficie lisa con 5 aristas suaves, cuello verdoso, punta y cabo marrones, pequitas
        B.color(0.98f, 0.84f, 0.24f);
        B.part(SH_BANANA, 0, 0, 0, 1, 1, 1);
        B.color(0.80f, 0.80f, 0.30f);                                                    // cuello: amarillo verdoso
        B.part(SH_BANANA_NECK, 0, 0, 0, 1, 1, 1);
        B.color(0.30f, 0.22f, 0.11f);                                                    // punta de adelante (la flor seca)
        B.part(SH_BANANA_TIP, 0, 0, 0, 1, 1, 1);
        {
            float c[3], T[3], N[3];
            bananaFrame(0, c, T, N);
            B.color(0.20f, 0.15f, 0.08f);
            const float X[3] = {1, 0, 0};
            float tip[3] = {c[0] - T[0] * 0.0012f, c[1] - T[1] * 0.0012f, c[2] - T[2] * 0.0012f};
            B.oriented(SH_SPHERE, tip, X, N, T, 0.0052f, 0.0048f, 0.004f);
            // Cabo: sigue la curva, con 5 lados, y la punta cortada más oscura
            bananaFrame(1, c, T, N);
            float a0[3] = {c[0] - T[0] * 0.002f, c[1] - T[1] * 0.002f, c[2] - T[2] * 0.002f};
            float dir[3] = {T[0], T[1] * 0.75f + 0.25f, T[2]}, dl = sqrtf(dir[1] * dir[1] + dir[2] * dir[2]);
            dir[1] /= dl; dir[2] /= dl;
            float a1[3] = {c[0], c[1] + dir[1] * 0.026f, c[2] + dir[2] * 0.026f};
            float a2[3] = {c[0], c[1] + dir[1] * 0.0285f, c[2] + dir[2] * 0.0285f};
            B.color(0.80f, 0.80f, 0.30f);   // tapa del cuello (para que no se vea el hueco)
            float n0[3] = {c[0] - T[0] * 0.0015f, c[1] - T[1] * 0.0015f, c[2] - T[2] * 0.0015f}, n1[3] = {c[0] + T[0] * 0.0005f, c[1] + T[1] * 0.0005f, c[2] + T[2] * 0.0005f};
            B.segment(n0, n1, 2 * bananaRadius(1) * 1.04f, 2 * bananaRadius(1) * 0.96f, false);
            B.color(0.52f, 0.47f, 0.20f);
            B.segment(a0, a1, 0.0082f, 0.0076f, false, SH_HEX);
            B.color(0.26f, 0.19f, 0.09f);
            B.segment(a1, a2, 0.0088f, 0.0082f, false, SH_HEX);
        }
        // Pequitas marrones sobre la cáscara (puntos fijos, siempre iguales)
        B.color(0.66f, 0.48f, 0.20f);
        const float spots[11][2] = {{0.18f, 0.6f}, {0.27f, 1.9f}, {0.33f, 0.2f}, {0.41f, 1.2f}, {0.47f, 2.6f}, {0.55f, 0.9f},
                                    {0.6f, 1.7f}, {0.66f, 0.4f}, {0.72f, 2.2f}, {0.79f, 1.1f}, {0.5f, -0.4f}};
        for (const auto &sp : spots) {
            float p[3], c[3], T[3], N[3];
            bananaPoint(sp[0], sp[1], p);
            bananaFrame(sp[0], c, T, N);
            float o[3] = {p[0] - c[0], p[1] - c[1], p[2] - c[2]}, ol = sqrtf(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
            for (float &k : o) k /= ol;
            float w[3] = {T[1] * o[2] - T[2] * o[1], T[2] * o[0] - T[0] * o[2], T[0] * o[1] - T[1] * o[0]};
            float sz = 0.0026f + 0.0018f * fabsf(sinf(sp[0] * 37.f));
            B.oriented(SH_SPHERE, p, w, o, T, sz, 0.0007f, sz * 1.3f);
        }
        // Líneas finas a lo largo de las aristas (un poco más oscuras), solo en el medio
        B.color(0.92f, 0.76f, 0.21f);
        for (int k = 0; k < 5; k++) {
            float ang = (6.2831853f * k - 0.3f) / 5;
            for (int i = 0; i < 6; i++) {
                float ta = 0.2f + i * 0.1f, tb = ta + 0.1f, pa[3], pb[3];
                bananaPoint(ta, ang, pa); bananaPoint(tb, ang, pb);
                B.segment(pa, pb, 0.0016f, 0.0016f, false);
            }
        }
        break;
    }
    case 3: {   // lápiz: hexagonal amarillo, punta de madera con mina, virola con anillos y goma
        B.color(0.95f, 0.74f, 0.12f);
        B.part(SH_HEX, 0, 0, 0.09f, 0.0092f, 0.0092f, 0.15f);
        B.color(0.20f, 0.20f, 0.22f);
        B.box(0, 0.0041f, 0.12f, 0.0014f, 0.0004f, 0.028f, 0, 0, 0);                    // letras (una raya oscura)
        B.color(0.87f, 0.72f, 0.52f);
        B.part(SH_CONE, 0, 0, 0.004f, 0.0092f, 0.0092f, 0.022f);                         // madera
        B.color(0.18f, 0.18f, 0.20f);
        B.part(SH_CONE, 0, 0, -0.0097f, 0.0028f, 0.0028f, 0.0067f);                       // mina
        B.color(0.74f, 0.74f, 0.76f);
        B.part(SH_CYL, 0, 0, 0.172f, 0.0096f, 0.0096f, 0.014f);                          // virola
        B.color(0.58f, 0.58f, 0.60f);
        B.part(SH_CYL, 0, 0, 0.167f, 0.0101f, 0.0101f, 0.0014f);
        B.part(SH_CYL, 0, 0, 0.177f, 0.0101f, 0.0101f, 0.0014f);
        B.color(0.93f, 0.55f, 0.58f);
        B.part(SH_CYL, 0, 0, 0.1855f, 0.0088f, 0.0088f, 0.013f);                         // goma
        B.part(SH_SPHERE, 0, 0, 0.192f, 0.0088f, 0.0088f, 0.004f);
        break;
    }
    }
    if (!holding && watch && watch->on) {   // el reloj, en la muñeca de la mano libre
        const HandXf &X = g_hs[left ? 0 : 1].X;
        auto toCtl = [&](const float h[3], float o[3], bool dir) { if (dir) xfDir(X, h, o); else xfPoint(X, h, o); if (left) o[0] = -o[0]; };
        const float hc[3] = {0, -0.03f, 0}, hd[3] = {0, 0, -1}, hy[3] = {0, 1, 0}, hx[3] = {1, 0, 0};
        float wc[3], dors[3], toHand[3], across[3];
        toCtl(hc, wc, false); toCtl(hd, dors, true); toCtl(hy, toHand, true); toCtl(hx, across, true);
        addWatch(B, *watch, left, wc, dors, toHand, across, g_watchPos[left ? 0 : 1], g_watchNrm[left ? 0 : 1]);
    }
}

static void handsUpdate(int activeHand, int model, const HandTouch in[2], float dt, float press[2]) {
    if (hmesh::rpos.empty()) return;
    for (int side = 0; side < 2; side++) {
        if (!in[side].present) continue;
        HandState &hs = g_hs[side];
        bool left = side == 0, holding = side == activeHand;
        hmesh::Pose ps{};
        float bend[3];
        if (holding) {
            int level = (int)lroundf(std::min(1.f, std::max(0.f, press[side])) * 8);
            long key = model * 100 + level;
            if (hs.holding && hs.key == key && hs.ready) continue;
            solveHolding(model, level / 8.f, gripFor(model), hs.X, ps, bend);
            hs.key = key; hs.holding = true; hs.ready = true;
            handSkinTo(hs, ps, bend, left);
            continue;
        }
        // Mano libre: relajada; el índice se acerca al gatillo al apoyarlo, el resto se cierra con el grip,
        // y el pulgar baja cuando lo apoyás en el joystick o los botones
        const HandTouch &h = in[side];
        float tgt[8] = {0.14f + (h.trigTouch ? 0.14f : 0.f) + h.trig * 0.45f,
                        0.20f + h.squeeze * 0.5f, 0.24f + h.squeeze * 0.52f, 0.29f + h.squeeze * 0.55f,
                        0.72f - h.squeeze * 0.3f,
                        h.thumbTouch ? 0.040f : 0.054f, h.thumbTouch ? 0.084f : 0.092f, h.thumbTouch ? 0.034f : 0.024f};
        float a = hs.holding || !hs.ready ? 1.f : 1.f - expf(-dt * 14.f);
        for (int f = 0; f < 4; f++) hs.t[f] += (tgt[f] - hs.t[f]) * a;
        hs.spread += (tgt[4] - hs.spread) * a;
        for (int k = 0; k < 3; k++) hs.thumb[k] += (tgt[5 + k] - hs.thumb[k]) * a;
        float now[8] = {hs.t[0], hs.t[1], hs.t[2], hs.t[3], hs.spread, hs.thumb[0], hs.thumb[1], hs.thumb[2]};
        float change = 0;
        for (int k = 0; k < 8; k++) change = std::max(change, fabsf(now[k] - hs.drawn[k]) * (k >= 5 ? 50.f : 1.f));
        if (!hs.holding && hs.ready && change < 0.004f) continue;
        hs.X = gripFrame(EMPTY_GRIP);
        for (int f = 0; f < 4; f++) flexFor(hs.t[f], ps.flex[f]);
        ps.spread = hs.spread;
        for (int k = 0; k < 3; k++) ps.thumbT[k] = hs.thumb[k];
        const float bh[3] = {0.25f, 0.05f, -1.f};
        handSkinTo(hs, ps, bh, left);
        for (int k = 0; k < 8; k++) hs.drawn[k] = now[k];
        hs.holding = false; hs.ready = true; hs.key = -1;
    }
}

static void initHands(App &app) {
    const char *vs = R"(#version 300 es
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
uniform mat4 VP;
uniform mat4 M;
uniform vec3 scl;
out vec3 wn;
out vec3 wp;
void main() {
    vec4 w = M * vec4(pos, 1.0);
    wp = w.xyz;
    wn = normalize(mat3(M) * (nrm / (scl * scl)));   // normales correctas aunque la pieza esté estirada
    gl_Position = VP * w;
})";
    const char *fs = R"(#version 300 es
precision mediump float;
uniform vec3 col;
uniform vec3 eye;
uniform float emit;
uniform float skin;
in vec3 wn;
in vec3 wp;
out vec4 o;
void main() {
    vec3 n = normalize(wn);
    vec3 v = normalize(eye - wp);
    vec3 key = normalize(vec3(0.3, 0.9, 0.35));
    float d = max(dot(n, key), 0.0) * 0.75 + max(dot(n, -key), 0.0) * 0.12 + 0.22;
    // la mano: luz que «envuelve» (más suave, como tela) y casi sin brillo
    float dw = (dot(n, key) * 0.5 + 0.5) * 0.72 + 0.26;
    d = mix(d, dw, 0.45 + 0.55 * skin);   // todo con luz suave (estilo «arcilla»), la piel más
    // piel: en la sombra la luz «atraviesa» un poco y se ve más cálida (como la piel de verdad)
    vec3 sss = col * vec3(1.0, 0.55, 0.45) * (1.0 - dw) * 0.35 * skin;
    float rim = pow(1.0 - max(dot(n, v), 0.0), 3.0) * mix(0.25, 0.16, skin);
    float sp = pow(max(dot(n, normalize(key + v)), 0.0), 30.0) * mix(0.14, 0.04, skin);
    vec3 c = col * d + sss + vec3(rim + sp) * 0.6 + col * emit * 2.0;
    o = vec4(c, 1.0);
})";
    app.handProg = glCreateProgram();
    glAttachShader(app.handProg, compileShader(GL_VERTEX_SHADER, vs));
    glAttachShader(app.handProg, compileShader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(app.handProg);
    app.hVP = glGetUniformLocation(app.handProg, "VP");
    app.hM = glGetUniformLocation(app.handProg, "M");
    app.hCol = glGetUniformLocation(app.handProg, "col");
    app.hEye = glGetUniformLocation(app.handProg, "eye");
    app.hEmit = glGetUniformLocation(app.handProg, "emit");
    app.hSkin = glGetUniformLocation(app.handProg, "skin");
    app.hScl = glGetUniformLocation(app.handProg, "scl");
    // Mallas: caja, cilindro, esfera, prisma de 6 lados y cono
    std::vector<float> mv;
    buildHandMeshes(mv, app.shFirst, app.shCount);
    addSmoothShapes(mv, app.shFirst, app.shCount);
    // (la mano lisa ya no se usa: ahora se ven las manos reales)
    glGenBuffers(1, &g_handEbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_handEbo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, hmesh::ridx.size() * sizeof(uint32_t), hmesh::ridx.data(), GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
    g_handIdx = (GLsizei)hmesh::ridx.size();
    LOGI("Mano: %d vértices, %d triángulos", (int)(hmesh::rpos.size() / 3), (int)(hmesh::ridx.size() / 3));
    glGenVertexArrays(1, &app.handVao);
    glGenBuffers(1, &app.handVbo);
    glBindVertexArray(app.handVao);
    glBindBuffer(GL_ARRAY_BUFFER, app.handVbo);
    glBufferData(GL_ARRAY_BUFFER, mv.size() * sizeof(float), mv.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(3 * sizeof(float)));
    glBindVertexArray(0);
    // Profundidad para la capa de los controles (las partes de cada objeto se tapan entre sí)
    for (int e = 0; e < 2; e++) {
        Swapchain &s = app.overlay[e];
        if (!s.handle) continue;
        glGenRenderbuffers(1, &app.ovDepth[e]);
        glBindRenderbuffer(GL_RENDERBUFFER, app.ovDepth[e]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, s.w, s.h);
        for (GLuint fb : s.fbos) {
            glBindFramebuffer(GL_FRAMEBUFFER, fb);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, app.ovDepth[e]);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

/// Dibuja el objeto y la mano en la pose del control (con profundidad, antes del láser). hand: 0 izquierda, 1 derecha
static void drawHand(App &app, const float VP[16], const XrView &view, const XrPosef &pose, int hand, float trig) {
    if (!app.handProg || !app.ovDepth[0] || !app.handVao) return;
    static std::vector<HandPart> parts[2];
    static long built[2] = {-1, -1};
    int side = hand == 0 ? 0 : 1;
    int model = g_handModel.load(), color = g_handColor.load();
    // La mano del láser tiene el objeto; la otra, solo el reloj (y se intercambian al pasar el láser)
    bool holding = side == app.activeHand;
    int level = holding ? (int)lroundf(std::min(1.f, std::max(0.f, (trig - 0.05f) / 0.6f)) * 8) : 0;   // 9 posiciones del pulgar
    WatchInfo wi{false, 0, 0};
    if (!holding && g_watchOn) {   // el reloj: la hora del Quest y las marcas encendidas mientras lo mirás
        time_t now = time(nullptr);
        int mod;
        int tz = g_tzMin.load();
        if (tz != INT32_MIN) mod = (int)(((now / 60 + tz) % 1440 + 1440) % 1440);
        else { struct tm lt{}; localtime_r(&now, &lt); mod = lt.tm_hour * 60 + lt.tm_min; }
        wi = {true, mod, (int)lroundf(std::min(1.f, std::max(0.f, g_watchProg.load())) * 12)};
    }
    long key = (((long)model * 10 + color) * 10 + level) * 100000L + (holding ? 50000L : 0) + (wi.on ? 1 + wi.minuteOfDay * 13 + wi.lit : 0)
               + (g_hs[side].ready ? 0 : 1000000000000L);
    if (key != built[side]) { handParts(model, side == 0, holding, color, level / 8.f, parts[side], &wi); built[side] = key; }
    float R[9]; quatToMat(pose.orientation, R);   // columnas: ejes X, Y, Z del control
    glUseProgram(app.handProg);
    glUniformMatrix4fv(app.hVP, 1, GL_FALSE, VP);
    glUniform3f(app.hEye, view.pose.position.x, view.pose.position.y, view.pose.position.z);
    glBindVertexArray(app.handVao);
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDisable(GL_BLEND);
    for (const HandPart &p : parts[side]) {
        // local = R de la pieza · escala, desplazado a c; después la pose del control
        float M[16];
        for (int col = 0; col < 3; col++) {
            float lc[3] = {p.R[col * 3] * p.s[col], p.R[col * 3 + 1] * p.s[col], p.R[col * 3 + 2] * p.s[col]};
            for (int r = 0; r < 3; r++) M[col * 4 + r] = R[r] * lc[0] + R[3 + r] * lc[1] + R[6 + r] * lc[2];
            M[col * 4 + 3] = 0;
        }
        for (int r = 0; r < 3; r++) M[12 + r] = (&pose.position.x)[r] + R[r] * p.c[0] + R[3 + r] * p.c[1] + R[6 + r] * p.c[2];
        M[15] = 1;
        glUniformMatrix4fv(app.hM, 1, GL_FALSE, M);
        glUniform3f(app.hCol, p.col[0], p.col[1], p.col[2]);
        glUniform1f(app.hEmit, p.emit);
        glUniform1f(app.hSkin, p.skin ? 1.f : 0.f);
        glUniform3f(app.hScl, p.s[0], p.s[1], p.s[2]);
        glDrawArrays(GL_TRIANGLES, app.shFirst[p.shape], app.shCount[p.shape]);
    }
    // La mano (una sola malla lisa, ya deformada y ubicada en el control)
    HandState &hs = g_hs[side];
    if (hs.ready && g_handIdx > 0) {
        if (!hs.vao) {
            glGenVertexArrays(1, &hs.vao);
            glGenBuffers(1, &hs.vbo);
            glBindVertexArray(hs.vao);
            glBindBuffer(GL_ARRAY_BUFFER, hs.vbo);
            glBufferData(GL_ARRAY_BUFFER, hs.vbuf.size() * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
            glEnableVertexAttribArray(0);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
            glEnableVertexAttribArray(1);
            glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(3 * sizeof(float)));
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g_handEbo);
            hs.upload = true;
        }
        glBindVertexArray(hs.vao);
        if (hs.upload) {
            glBindBuffer(GL_ARRAY_BUFFER, hs.vbo);
            glBufferData(GL_ARRAY_BUFFER, hs.vbuf.size() * sizeof(float), hs.vbuf.data(), GL_DYNAMIC_DRAW);
            hs.upload = false;
        }
        float M[16] = {R[0], R[1], R[2], 0, R[3], R[4], R[5], 0, R[6], R[7], R[8], 0, pose.position.x, pose.position.y, pose.position.z, 1};
        const float *sk = SKIN_TONES[std::min(5, std::max(0, color))];
        glUniformMatrix4fv(app.hM, 1, GL_FALSE, M);
        glUniform3f(app.hCol, lin(sk[0]), lin(sk[1]), lin(sk[2]));
        glUniform1f(app.hEmit, 0);
        glUniform1f(app.hSkin, 1);
        glUniform3f(app.hScl, 1, 1, 1);
        glDrawElements(GL_TRIANGLES, g_handIdx, GL_UNSIGNED_INT, (void *)0);
    }
    glBindVertexArray(0);
    glDisable(GL_DEPTH_TEST);
    glUseProgram(app.lineProg);
}

static void renderOverlay(App &app, int e, const XrView &view, const Ray *rays, int nr, const std::vector<Bar> &bars = {}, bool teleport = false) {
    Swapchain &s = app.overlay[e];
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    xrAcquireSwapchainImage(s.handle, &ai, &idx);
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(s.handle, &wi);
    glBindFramebuffer(GL_FRAMEBUFFER, s.fbos[idx]);
    glViewport(0, 0, s.w, s.h);
    glClearColor(0, 0, 0, 0);
    glClearDepthf(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    float P[16], V[16], VP[16];
    projMatrix(view.fov, P);
    viewMatrix(view.pose, V);
    mul4(P, V, VP);
    const float eye[3] = {view.pose.position.x, view.pose.position.y, view.pose.position.z};
    // Manos reales con la imagen de la cámara (borde difuminado)
    drawCamHands(app, e, VP, eye);
    const float right[3] = {V[0], V[4], V[8]}, up[3] = {V[1], V[5], V[9]};

    glUseProgram(app.lineProg);
    glUniformMatrix4fv(app.uVP, 1, GL_FALSE, VP);
    glBindVertexArray(app.lineVao);
    glEnable(GL_BLEND);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    // Teletransporte: una curva de puntos y un círculo donde vas a aparecer
    if (teleport && !app.tpArc.empty()) {
        const float right[3] = {V[0], V[4], V[8]}, upv[3] = {V[1], V[5], V[9]};
        glUniform1f(app.uRound, 1);
        size_t np = app.tpArc.size() / 3;
        for (size_t i = 0; i < np; i++) {
            bool last = i + 1 == np;
            float k = last ? 0.14f : 0.018f;
            const float *c = &app.tpArc[i * 3];
            float q[20];
            const float cr[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
            for (int j = 0; j < 4; j++) {
                for (int a = 0; a < 3; a++)
                    q[j * 5 + a] = last ? c[a] + (a == 0 ? cr[j][0] * k : a == 2 ? cr[j][1] * k : 0.01f)   // círculo apoyado en el piso
                                        : c[a] + (right[a] * cr[j][0] + upv[a] * cr[j][1]) * k;
                q[j * 5 + 3] = (cr[j][0] + 1) * 0.5f; q[j * 5 + 4] = (cr[j][1] + 1) * 0.5f;
            }
            if (app.tpOk) glUniform4f(app.uCol, 0.90f, 0.66f, 0.29f, last ? 0.85f : 0.7f);   // ámbar (el color de la app)
            else glUniform4f(app.uCol, 1.0f, 0.35f, 0.3f, last ? 0.6f : 0.5f);
            drawQuad(app, q);
        }
    }
    // Portal: el trazo mientras lo dibujás (puntitos ámbar)
    if (g_portal.state == 1 && !g_portal.trail.empty()) {
        const float rr[3] = {V[0], V[4], V[8]}, uu[3] = {V[1], V[5], V[9]};
        const float k = 0.004f;
        const float cr[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        glUniform1f(app.uRound, 1);
        glUniform4f(app.uCol, 0.90f, 0.66f, 0.29f, 0.85f);
        for (size_t i = 0; i + 2 < g_portal.trail.size(); i += 6) {
            const float *c = &g_portal.trail[i];
            float q[20];
            for (int j = 0; j < 4; j++) {
                for (int a = 0; a < 3; a++) q[j * 5 + a] = c[a] + (rr[a] * cr[j][0] + uu[a] * cr[j][1]) * k;
                q[j * 5 + 3] = (cr[j][0] + 1) * 0.5f; q[j * 5 + 4] = (cr[j][1] + 1) * 0.5f;
            }
            drawQuad(app, q);
        }
    }
    // Mirando el borde de la mano: un puntito ámbar que se llena hasta abrir el menú
    if (g_watchProg.load() > 0.05f) {
        const float rr[3] = {V[0], V[4], V[8]}, uu[3] = {V[1], V[5], V[9]};
        float k = 0.004f + 0.008f * std::min(1.f, g_watchProg.load());
        float q[20];
        const float cr[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        for (int j = 0; j < 4; j++) {
            for (int a = 0; a < 3; a++) q[j * 5 + a] = g_palmDot[a] + (rr[a] * cr[j][0] + uu[a] * cr[j][1]) * k;
            q[j * 5 + 3] = (cr[j][0] + 1) * 0.5f; q[j * 5 + 4] = (cr[j][1] + 1) * 0.5f;
        }
        glUniform1f(app.uRound, 1);
        glUniform4f(app.uCol, 0.90f, 0.66f, 0.29f, 0.5f + 0.4f * g_watchProg.load());
        drawQuad(app, q);
    }
    // Barritas debajo de cada ventana (para moverlas, como en visionOS)
    for (const Bar &b : bars) {
        float q[20];
        const float cr[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
        for (int j = 0; j < 4; j++) {
            for (int a = 0; a < 3; a++) q[j * 5 + a] = b.c[a] + b.ax[a] * cr[j][0] * b.hw + b.ay[a] * cr[j][1] * b.hh;
            q[j * 5 + 3] = (cr[j][0] + 1) * 0.5f; q[j * 5 + 4] = (cr[j][1] + 1) * 0.5f;
        }
        glUniform1f(app.uRound, 2);
        glUniform4f(app.uCol, 1, 1, 1, b.a);
        drawQuad(app, q);
    }
    for (int i = 0; i < nr; i++) {
        const Ray &r = rays[i];
        if (r.hand >= 0 && r.hand != app.activeHand) continue;   // el láser solo sale de una mano
        if (g_laserNoTouch && !r.hit) continue;                    // sin tocar pantallas: solo se ve sobre el menú
        if (r.hidden) continue;                                     // sobre una pantalla que el láser no toca
        float toEye[3] = {eye[0] - r.o[0], eye[1] - r.o[1], eye[2] - r.o[2]};
        // Ancho perpendicular al rayo y a la mirada (siempre se ve de frente)
        float sd[3] = {r.d[1] * toEye[2] - r.d[2] * toEye[1], r.d[2] * toEye[0] - r.d[0] * toEye[2], r.d[0] * toEye[1] - r.d[1] * toEye[0]};
        float sl = sqrtf(dot3(sd, sd));
        if (sl < 1e-5f) continue;
        // Intensidad: más intensa = más opaca y un poco más gruesa
        const float gain = g_laserGain.load();
        const float hw = 0.0025f * (0.75f + 0.25f * std::min(gain, 1.7f));
        for (float &c : sd) c = c / sl * hw;
        float tip[3] = {r.o[0] + r.d[0] * r.len, r.o[1] + r.d[1] * r.len, r.o[2] + r.d[2] * r.len};
        float v[20] = {
            r.o[0] - sd[0], r.o[1] - sd[1], r.o[2] - sd[2], 0, 0,
            r.o[0] + sd[0], r.o[1] + sd[1], r.o[2] + sd[2], 1, 0,
            tip[0] - sd[0], tip[1] - sd[1], tip[2] - sd[2], 0, 1,
            tip[0] + sd[0], tip[1] + sd[1], tip[2] + sd[2], 1, 1,
        };
        glUniform1f(app.uRound, 0);
        // Color: clásico (blanco y ámbar al apuntar a algo) o uno fijo, más fuerte al apuntar a algo
        // El color elegido (ámbar = el clásico); sin apuntar a una pantalla o ventana, la mitad de opaco
        static const float LASER[8][3] = {{0.90f, 0.66f, 0.29f}, {0.94f, 0.94f, 0.95f}, {0.35f, 0.75f, 1.0f}, {0.35f, 0.95f, 0.55f},
                                          {1.0f, 0.33f, 0.30f}, {0.72f, 0.45f, 1.0f}, {1.0f, 0.45f, 0.75f}, {0.95f, 0.92f, 0.35f}};
        int lc = g_laserColor.load();
        const float *col = LASER[lc];
        float alpha = (r.hit ? 0.9f : 0.45f) * gain;
        glUniform4f(app.uCol, col[0], col[1], col[2], std::min(1.f, alpha));
        drawQuad(app, v);
        if (r.hit) {
            // Puntero: un punto redondo donde toca el rayo, un poquito hacia adelante del panel
            float c[3] = {tip[0] - r.d[0] * 0.004f, tip[1] - r.d[1] * 0.004f, tip[2] - r.d[2] * 0.004f};
            const float k = 0.011f;
            float q[20];
            const float cr[4][2] = {{-1, -1}, {1, -1}, {-1, 1}, {1, 1}};
            for (int j = 0; j < 4; j++) {
                for (int a = 0; a < 3; a++) q[j * 5 + a] = c[a] + (right[a] * cr[j][0] + up[a] * cr[j][1]) * k;
                q[j * 5 + 3] = (cr[j][0] + 1) * 0.5f; q[j * 5 + 4] = (cr[j][1] + 1) * 0.5f;
            }
            glUniform1f(app.uRound, 1);
            glUniform4f(app.uCol, 1, 1, 1, 1);
            drawQuad(app, q);
        }
    }
    // Viñeta al moverte: bordes oscuros (centrados en el centro óptico de cada ojo)
    if (g_vigAmt > 0.01f) {
        static GLuint vp = 0; static GLint uA = -1, uC = -1;
        if (!vp) {
            const char *vsrc = R"(#version 300 es
out vec2 p;
void main() {
    vec2 q = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
    p = q;
    gl_Position = vec4(q, 0.0, 1.0);
})";
            const char *fsrc = R"(#version 300 es
precision mediump float;
in vec2 p;
uniform float amt;
uniform vec2 ctr;
out vec4 o;
void main() {
    float r = length(p - ctr);
    float a = smoothstep(0.45, 1.05, r) * amt * 0.92;
    o = vec4(0.0, 0.0, 0.0, a);
})";
            vp = glCreateProgram();
            glAttachShader(vp, compileShader(GL_VERTEX_SHADER, vsrc));
            glAttachShader(vp, compileShader(GL_FRAGMENT_SHADER, fsrc));
            glLinkProgram(vp);
            uA = glGetUniformLocation(vp, "amt"); uC = glGetUniformLocation(vp, "ctr");
        }
        glEnable(GL_BLEND);
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        glDisable(GL_DEPTH_TEST);
        glUseProgram(vp);
        glUniform1f(uA, g_vigAmt);
        // centro óptico: donde cae «adelante» dentro del campo de visión de este ojo
        float tl = tanf(view.fov.angleLeft), tr = tanf(view.fov.angleRight), td = tanf(view.fov.angleDown), tu = tanf(view.fov.angleUp);
        glUniform2f(uC, -(tr + tl) / (tr - tl), -(tu + td) / (tu - td));
        glBindVertexArray(app.vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
    }
    glDisable(GL_BLEND);
    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(s.handle, &ri);
}

// ---------------------------------------------------------------------------
// Living: piso, paredes, mueble, alfombra, sillón y la TV con luces detrás que siguen la imagen
// ---------------------------------------------------------------------------
static void initRoom(App &app) {
    const char *vs = R"(#version 300 es
layout(location = 0) in vec3 pos;
layout(location = 1) in vec3 nrm;
uniform mat4 VP;
uniform mat4 M;
uniform mat3 N;
out vec3 wp;
out vec3 wn;
void main() {
    vec4 w = M * vec4(pos, 1.0);
    wp = w.xyz;
    wn = N * nrm;
    gl_Position = VP * w;
})";
    // Materiales: 0 liso, 1 piso de madera, 2 alfombra, 3 ventana de noche, 4 brilla solo, 5 pared, 6 tela, 7 cuadro
    const char *fs = R"(#version 300 es
precision highp float;
in vec3 wp;
in vec3 wn;
uniform vec3 col;
uniform int mat;
uniform vec3 eye, lightP, lampP;
uniform vec3 anc, rx, fz;            // ejes de la habitación
uniform vec3 ledP[4], ledD[4];     // un foco por lado de la TV (izq, der, arriba, abajo)
uniform vec3 ledC[12];               // colores de los 12 tramos (para el resplandor en la pared)
uniform float ledOn;
uniform vec3 amb;                    // tinte general de las luces de la TV
uniform vec3 scrP, scrN, scrC;       // la pantalla de la TV también ilumina hacia adelante
uniform vec3 scrR, scrU;             // ejes de la pantalla
uniform vec2 scrH;                   // medio ancho y medio alto de la pantalla
uniform vec3 scrG[9];                // colores de la imagen en 3x3 zonas
uniform float gain;                  // brillo elegido para la pantalla y las luces
uniform float scrK;                  // qué tan rápido se apaga la luz de la pantalla con la distancia
out vec4 o;
float h1(float n) { return fract(sin(n * 127.1) * 43758.5453); }
float h2(vec2 p) { return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453); }
uniform float time;
float h3(vec3 p) { return fract(sin(dot(p, vec3(127.1, 311.7, 74.7))) * 43758.5453); }
float vnoise(vec3 p) {
    vec3 i = floor(p), f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    return mix(mix(mix(h3(i), h3(i + vec3(1, 0, 0)), f.x), mix(h3(i + vec3(0, 1, 0)), h3(i + vec3(1, 1, 0)), f.x), f.y),
               mix(mix(h3(i + vec3(0, 0, 1)), h3(i + vec3(1, 0, 1)), f.x), mix(h3(i + vec3(0, 1, 1)), h3(i + vec3(1, 1, 1)), f.x), f.y), f.z);
}
float fbm(vec3 p) { float a = 0.5, s = 0.0; for (int i = 0; i < 4; i++) { s += a * vnoise(p); p *= 2.03; a *= 0.5; } return s; }

// Espacio: nebulosas de colores, estrellas de varios tamaños, un planeta con atmósfera y anillo, y un sol lejano
vec3 spaceSky(vec3 d, vec3 rx, vec3 fz) {
    vec3 c = vec3(0.003, 0.004, 0.010);
    float n1 = fbm(d * 2.2 + vec3(3.1, 0.0, 1.7)), n2 = fbm(d * 3.7 + vec3(-2.0, 5.0, 0.3));
    c += vec3(0.32, 0.08, 0.40) * pow(smoothstep(0.45, 0.85, n1), 2.0) * 0.55;
    c += vec3(0.05, 0.22, 0.42) * pow(smoothstep(0.5, 0.9, n2), 2.0) * 0.5;
    c += vec3(0.6, 0.25, 0.12) * pow(smoothstep(0.62, 0.95, n1 * n2 * 1.6), 3.0) * 0.4;
    for (int l = 0; l < 3; l++) {
        float sc = 140.0 + float(l) * 160.0;
        vec3 g = floor(d * sc);
        float st = h3(g + float(l) * 31.0);
        vec3 cen = (g + 0.5 + (vec3(h3(g + 7.0), h3(g + 13.0), h3(g + 19.0)) - 0.5) * 0.7) / sc;
        float dist = length(d - normalize(cen)) * sc;
        float thr = 0.985 - float(l) * 0.01;
        if (st > thr) {
            vec3 tint = mix(vec3(0.75, 0.82, 1.0), vec3(1.0, 0.85, 0.65), h3(g + 3.0));
            c += tint * smoothstep(0.45, 0.0, dist) * (1.5 - float(l) * 0.4) * (0.6 + 0.4 * sin(time * (1.0 + st * 3.0) + st * 50.0));
        }
    }
    // sol lejano
    vec3 sun = normalize(-rx * 0.8 + fz * 0.3 + vec3(0.0, 0.35, 0.0));
    float sd = max(dot(d, sun), 0.0);
    c += vec3(1.0, 0.9, 0.75) * pow(sd, 900.0) * 6.0 + vec3(1.0, 0.7, 0.4) * pow(sd, 18.0) * 0.12;
    // planeta
    vec3 pc = normalize(rx * 0.6 + fz * 0.75 + vec3(0.0, 0.18, 0.0));
    float rad = 0.24, ca = dot(d, pc);
    float ang = acos(clamp(ca, -1.0, 1.0));
    // anillo inclinado
    vec3 ringN = normalize(vec3(0.15, 1.0, 0.25));
    vec3 o = -pc * 1.0;
    float t = -dot(o, ringN) / dot(d, ringN);
    if (t > 0.0) {
        vec3 hp = o + d * t;
        float rr = length(hp) / sin(rad);
        if (rr > 1.35 && rr < 2.1 && !(ang < rad && t > 1.0)) {
            float band = 0.55 + 0.45 * sin(rr * 40.0) * sin(rr * 13.0);
            c = mix(c, vec3(0.75, 0.68, 0.55) * band * 0.55, 0.65 * smoothstep(1.35, 1.45, rr) * smoothstep(2.1, 1.95, rr));
        }
    }
    if (ang < rad) {
        float x = ang / rad;
        vec3 sn = normalize(d - pc * cos(rad));
        float lit = clamp(dot(normalize(cross(cross(pc, sun), pc)), normalize(d - pc)) * 1.3 + 0.35, 0.0, 1.0);
        float bands = fbm(vec3(d.y * 18.0, d.x * 3.0, d.z * 3.0));
        vec3 surf = mix(vec3(0.10, 0.18, 0.36), vec3(0.45, 0.62, 0.85), bands) * (0.15 + 0.85 * lit);
        surf += vec3(0.25, 0.5, 1.0) * pow(x, 6.0) * 0.6 * (0.3 + lit);     // atmósfera en el borde
        c = surf;
    } else if (ang < rad * 1.08) c += vec3(0.2, 0.45, 1.0) * (1.0 - (ang - rad) / (rad * 0.08)) * 0.35;
    return c;
}

// Atardecer: cielo naranja y violeta, sol bajo y nubes suaves
// Cielo de la sala de carga: casi negro, un brillo ámbar finito en el horizonte y polvo que titila
vec3 voidSky(vec3 d, float t) {
    vec3 c = vec3(0.0015, 0.0013, 0.0012);   // negro
    c += vec3(0.90, 0.58, 0.22) * (0.035 * exp(-abs(d.y) * 40.0) + 0.004 * exp(-abs(d.y) * 6.0));   // apenas un hilo ámbar
    vec3 q = d * 70.0;
    vec3 cell = floor(q);
    float h = fract(sin(dot(cell, vec3(127.1, 311.7, 74.7))) * 43758.5453);
    if (h > 0.9965) {   // polvo flotando: puntitos ámbar que titilan
        vec3 fr = fract(q) - 0.5;
        float tw = 0.45 + 0.55 * sin(t * (0.6 + h * 1.7) + h * 40.0);
        c += vec3(0.95, 0.65, 0.32) * smoothstep(0.34, 0.0, length(fr)) * 0.30 * tw;
    }
    return c;
}
vec3 sunsetSky(vec3 d, vec3 rx, vec3 fz) {
    vec3 sun = normalize(fz + vec3(0.0, 0.06, 0.0));
    float y = d.y;
    vec3 c = mix(vec3(0.95, 0.45, 0.18), vec3(0.16, 0.12, 0.32), smoothstep(-0.02, 0.45, y));
    c = mix(c, vec3(0.04, 0.05, 0.14), smoothstep(0.35, 0.95, y));
    float sd = max(dot(d, sun), 0.0);
    c += vec3(1.0, 0.75, 0.4) * pow(sd, 12.0) * 0.6 + vec3(1.0, 0.9, 0.7) * smoothstep(0.9993, 0.9996, sd) * 3.0;
    if (y > 0.02) {
        float cl = fbm(vec3(d.xz / (y + 0.15) * 1.6, time * 0.01));
        c = mix(c, mix(vec3(0.95, 0.55, 0.35), vec3(0.35, 0.25, 0.45), smoothstep(0.0, 0.5, y)), smoothstep(0.55, 0.8, cl) * 0.6);
    }
    return c;
}
void main() {
    vec3 n = normalize(wn);
    if (dot(n, eye - wp) < 0.0) n = -n;   // las paredes se ven desde adentro
    vec3 q = wp - anc;
    float lx = dot(q, rx), lz = dot(q, fz), y = wp.y;
    vec3 c = col;
    if (mat == 1) {
        // tablas de madera: franjas a lo largo, cada una con su tono, juntas oscuras y veta
        float w = 0.19, row = floor(lz / w), off = h1(row) * 1.7;
        float plank = floor((lx + off) / 1.4);
        float t = h2(vec2(row, plank));
        c *= 0.78 + 0.4 * t;
        c *= 0.93 + 0.07 * sin((lx + off) * 38.0 + t * 20.0 + sin(lz * 9.0) * 2.0);
        float fx = fract(lz / w), fe = fract((lx + off) / 1.4);
        if (fx < 0.025 || fe < 0.004) c *= 0.45;
    } else if (mat == 2) {
        // alfombra con borde
        float bx = abs(lx) , bz = abs(lz - 1.5);
        float edge = max(bx / 1.6, bz / 1.05);
        if (edge > 0.9) c = vec3(0.32, 0.22, 0.14);
        else if (edge > 0.84) c = vec3(0.55, 0.45, 0.30);
        c *= 0.9 + 0.1 * h2(floor(vec2(lx, lz) * 60.0));
    } else if (mat == 3) {
        // ventana de noche: cielo, edificios con luces encendidas
        float u = lz, v = y;
        vec3 sky = mix(vec3(0.02, 0.03, 0.08), vec3(0.10, 0.06, 0.16), clamp((v - 1.0) / 1.3, 0.0, 1.0));
        float b = floor(u * 6.0 + 20.0);
        float hgt = 1.15 + h1(b) * 0.9;
        if (v < hgt) {
            sky = vec3(0.015, 0.015, 0.025);
            vec2 cell = floor(vec2(u * 30.0, v * 22.0));
            if (h2(cell + b) > 0.72 && fract(u * 30.0) > 0.3 && fract(v * 22.0) > 0.35) sky = vec3(0.9, 0.7, 0.35) * (0.5 + h2(cell) * 0.6);
        }
        o = vec4(sky, 1.0);
        return;
    } else if (mat == 4) {
        o = vec4(col, 1.0);
        return;
    } else if (mat == 5) {
        c *= 0.96 + 0.04 * h2(floor(vec2(lx + lz, y) * 40.0));
    } else if (mat == 6) {
        c *= 0.92 + 0.08 * h2(floor(vec2(lx * 90.0, y * 90.0 + lz * 90.0)));
    } else if (mat == 7) {
        // cuadro abstracto
        vec2 a = vec2(lz, y) * 2.2;
        c = 0.5 + 0.5 * cos(vec3(0.0, 2.0, 4.0) + a.x * 2.0 + sin(a.y * 3.0 + a.x));
        c *= 0.6;
    }
    // Sombra suave en los rincones (para que se entiendan los bordes del cuarto)
    float ao = 1.0;
    if (mat == 1 || mat == 5 || mat == 0) {
        float dx = 3.5 - abs(lx), dzz = min(3.4 - lz, 2.8 + lz), dy = min(y, 2.9 - y);
        if (abs(n.y) > 0.7) ao = 0.55 + 0.45 * smoothstep(0.0, 0.8, min(dx, dzz));
        else if (mat == 5) ao = (0.6 + 0.4 * smoothstep(0.0, 0.5, dy)) * (0.7 + 0.3 * smoothstep(0.0, 0.6, abs(dot(n, rx)) > 0.7 ? dzz : dx));
    }
    // Cuarto a oscuras: solo la pantalla de la TV y las tiras LED de atrás iluminan (como un cine en casa)
    if (mat == 8) { o = vec4(spaceSky(normalize(wp - eye), rx, fz), 1.0); return; }
    if (mat == 10) { o = vec4(sunsetSky(normalize(wp - eye), rx, fz), 1.0); return; }
    if (mat == 12) { o = vec4(voidSky(normalize(wp - eye), time), 1.0); return; }
    if (mat == 13) {
        // piso de la sala de carga: grilla de 1 m que brilla apenas, se apaga con la distancia y se funde con el horizonte
        vec2 q = vec2(lx, lz);
        vec2 gd = abs(fract(q - 0.5) - 0.5) / max(fwidth(q), vec2(1e-4));
        float line = 1.0 - min(min(gd.x, gd.y), 1.0);
        float d = length(wp.xz - eye.xz);   // (la luz y la niebla van con vos: siempre estás en el medio)
        vec3 v = normalize(wp - eye);
        vec3 pcol = vec3(0.0012, 0.0011, 0.0010) + vec3(0.90, 0.58, 0.22) * line * 0.045 * exp(-d * 0.10);   // grilla ámbar tenue
        pcol += vec3(0.9, 0.58, 0.22) * 0.006 * exp(-d * 0.6);   // apenas luz bajo tus pies
        pcol = mix(pcol, voidSky(normalize(vec3(v.x, 0.0, v.z)), time), smoothstep(6.0, 27.0, d));
        o = vec4(pcol, 1.0);
        return;
    }
    if (mat == 9) {
        // plataforma del espacio: vidrio oscuro en hexágonos con bordes que brillan suave
        vec2 p = vec2(lx, lz) * 1.6;
        vec2 r = vec2(1.0, 1.732), h = r * 0.5;
        vec2 a = mod(p, r) - h, b = mod(p - h, r) - h;
        vec2 g = dot(a, a) < dot(b, b) ? a : b;
        float edge = 0.5 - max(abs(g.x) * 0.866 + abs(g.y) * 0.5, abs(g.y));
        float rr = length(vec2(lx, lz));
        vec3 pcol = vec3(0.02, 0.025, 0.035);
        pcol += vec3(0.15, 0.4, 0.9) * smoothstep(0.035, 0.0, edge) * 0.35 * smoothstep(5.8, 1.0, rr);
        pcol += vec3(0.2, 0.5, 1.0) * smoothstep(0.04, 0.0, abs(rr - 5.6)) * 0.8;   // borde de la plataforma
        // reflejo tenue del cielo
        vec3 v = normalize(wp - eye);
        pcol += spaceSky(reflect(v, vec3(0.0, 1.0, 0.0)), rx, fz) * 0.18;
        o = vec4(pcol, 1.0);
        return;
    }
    if (mat == 11) {
        // agua con olas y el reflejo del atardecer
        vec3 v = normalize(wp - eye);
        vec2 q2 = wp.xz;
        vec3 nn = normalize(vec3((vnoise(vec3(q2 * 1.3, time * 0.6)) - 0.5) * 0.25, 1.0, (vnoise(vec3(q2 * 1.3 + 7.0, time * 0.6)) - 0.5) * 0.25));
        vec3 rd = reflect(v, nn);
        rd.y = abs(rd.y);
        float fres = 0.15 + 0.85 * pow(1.0 - max(-v.y, 0.0), 4.0);
        vec3 wc = mix(vec3(0.02, 0.04, 0.08), sunsetSky(rd, rx, fz), fres);
        o = vec4(wc, 1.0);
        return;
    }
    // --- Iluminación tipo cine en casa ---
    vec3 V = normalize(eye - wp);
    float ks = mat == 1 ? 0.35 : (mat == 6 || mat == 2) ? 0.015 : mat == 5 ? 0.04 : 0.2;   // brillo (piso de madera, telas, paredes…)
    float sh = mat == 1 ? 80.0 : (mat == 6 || mat == 2) ? 6.0 : 30.0;
    vec3 dif = vec3(0.005, 0.005, 0.007) + amb;
    vec3 spe = vec3(0.0);
    // 1) La pantalla es una luz de área: 9 puntos sobre ella, cada uno del color de su zona de la imagen.
    //    Solo ilumina hacia adelante, y se refleja en lo brillante (el piso, el mueble).
    for (int i = 0; i < 9; i++) {
        float gx = float(i - (i / 3) * 3) - 1.0, gy = 1.0 - float(i / 3);
        vec3 lp = scrP + scrR * gx * scrH.x * 0.72 + scrU * gy * scrH.y * 0.72;
        vec3 L = lp - wp;
        float d2 = max(dot(L, L), 0.04);
        L *= inversesqrt(d2);
        float emit = max(dot(-L, scrN), 0.0);
        vec3 lc = scrG[i] * gain * emit * 0.5 / (1.0 + scrK * d2);
        dif += lc * max(dot(n, L), 0.0);
        if (ks > 0.1) spe += lc * pow(max(dot(n, normalize(L + V)), 0.0), sh) * (sh + 8.0) / 25.0;   // brillo solo en lo brillante (piso, mueble)
    }
    vec3 tq = wp - scrP;
    if (ledOn > 0.5 && dot(tq, tq) < 16.0) {   // a más de 4 m de la TV las tiras ya no se notan: no se calculan
        // 2) Tiras LED detrás de la TV: un foco por lado (color promedio de sus tramos) que baña la pared y rebota suave
        for (int i = 0; i < 4; i++) {
            vec3 v = ledP[i] - wp;
            float dd = max(dot(v, v), 0.01);
            vec3 ld = v * inversesqrt(dd);
            float cone = smoothstep(0.0, 0.9, dot(-ld, ledD[i]));
            vec3 sc = (ledC[i * 3] + ledC[i * 3 + 1] + ledC[i * 3 + 2]) * (1.0 / 3.0);
            dif += sc * gain * cone * 4.0 / (1.0 + 3.0 * dd) * max(dot(n, ld), 0.0);
        }
        // 3) El resplandor en la pared de atrás (lo que se ve con tiras reales): más intenso pegado a los bordes
        vec3 q2 = wp - scrP;
        float lx2 = dot(q2, scrR), ly2 = dot(q2, scrU), dz2 = dot(q2, scrN);
        if (dz2 < -0.03 && dot(n, scrN) > 0.5) {
            float ex = max(abs(lx2) - scrH.x * 0.9, 0.0), ey = max(abs(ly2) - scrH.y * 0.9, 0.0);
            float tx = clamp(lx2 / (2.0 * scrH.x) + 0.5, 0.0, 1.0) * 2.0, ty = clamp(0.5 - ly2 / (2.0 * scrH.y), 0.0, 1.0) * 2.0;
            int ix = tx < 1.0 ? 0 : 1, iy = ty < 1.0 ? 0 : 1;
            vec3 hc = ly2 > 0.0 ? mix(ledC[6 + ix], ledC[7 + ix], tx - float(ix)) : mix(ledC[9 + ix], ledC[10 + ix], tx - float(ix));
            vec3 vc = lx2 < 0.0 ? mix(ledC[0 + iy], ledC[1 + iy], ty - float(iy)) : mix(ledC[3 + iy], ledC[4 + iy], ty - float(iy));
            vec3 ec = mix(hc, vc, ex / (ex + ey + 1e-4));
            float dist = length(vec2(ex, ey));
            dif += ec * gain * 2.2 * exp(-2.4 * dist) * smoothstep(-1.2, -0.05, dz2);
        }
    }
    vec3 col3 = (c * dif + spe) * ao * 1.5;
    col3 = clamp((col3 * (2.51 * col3 + 0.03)) / (col3 * (2.43 * col3 + 0.59) + 0.14), 0.0, 1.0);   // tono de cine (ACES)
    o = vec4(col3, 1.0);
})";
    app.roomProg = glCreateProgram();
    glAttachShader(app.roomProg, compileShader(GL_VERTEX_SHADER, vs));
    glAttachShader(app.roomProg, compileShader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(app.roomProg);
    GLint ok = 0; glGetProgramiv(app.roomProg, GL_LINK_STATUS, &ok);
    if (!ok) { char log[1024]; glGetProgramInfoLog(app.roomProg, sizeof log, nullptr, log); LOGE("Sala: %s", log); }
    GLuint P = app.roomProg;
    app.rVP = glGetUniformLocation(P, "VP"); app.rM = glGetUniformLocation(P, "M"); app.rN = glGetUniformLocation(P, "N");
    app.rCol = glGetUniformLocation(P, "col"); app.rMat = glGetUniformLocation(P, "mat");
    app.rEye = glGetUniformLocation(P, "eye"); app.rLight = glGetUniformLocation(P, "lightP"); app.rLamp = glGetUniformLocation(P, "lampP");
    app.rAnc = glGetUniformLocation(P, "anc"); app.rRx = glGetUniformLocation(P, "rx"); app.rFz = glGetUniformLocation(P, "fz");
    app.rLedP = glGetUniformLocation(P, "ledP"); app.rLedD = glGetUniformLocation(P, "ledD"); app.rLedC = glGetUniformLocation(P, "ledC");
    app.rLedOn = glGetUniformLocation(P, "ledOn");
    app.rGain = glGetUniformLocation(P, "gain");
    app.rTime = glGetUniformLocation(P, "time");
    app.rScrK = glGetUniformLocation(P, "scrK");
    app.rScrR = glGetUniformLocation(P, "scrR"); app.rScrU = glGetUniformLocation(P, "scrU");
    app.rScrH = glGetUniformLocation(P, "scrH"); app.rScrG = glGetUniformLocation(P, "scrG");
    app.rScrP = glGetUniformLocation(P, "scrP"); app.rScrN = glGetUniformLocation(P, "scrN"); app.rScrC = glGetUniformLocation(P, "scrC"); app.rAmb = glGetUniformLocation(P, "amb");

    // Cubo unitario (posición + normal por cara)
    std::vector<float> v;
    const float F[6][3] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0}, {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    for (auto &f : F) {
        float a[3] = {0, 0, 0};
        if (f[0] != 0) a[1] = 1; else a[0] = 1;
        float b[3] = {f[1] * a[2] - f[2] * a[1], f[2] * a[0] - f[0] * a[2], f[0] * a[1] - f[1] * a[0]};
        const float cr[6][2] = {{-1, -1}, {1, -1}, {1, 1}, {-1, -1}, {1, 1}, {-1, 1}};
        for (auto &k : cr)
            for (int i = 0; i < 6; i++) v.push_back(i < 3 ? 0.5f * (f[i] + a[i] * k[0] + b[i] * k[1]) : f[i - 3]);
    }
    glGenVertexArrays(1, &app.cubeVao);
    glGenBuffers(1, &app.cubeVbo);
    glBindVertexArray(app.cubeVao);
    glBindBuffer(GL_ARRAY_BUFFER, app.cubeVbo);
    glBufferData(GL_ARRAY_BUFFER, v.size() * sizeof(float), v.data(), GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, 6 * sizeof(float), (void *)(3 * sizeof(float)));
    glBindVertexArray(0);

    // Profundidad para cada ojo (la habitación tiene cosas delante de otras)
    for (int e = 0; e < 2; e++) {
        Swapchain &s = app.sc[e];
        glGenRenderbuffers(1, &app.depth[e]);
        glBindRenderbuffer(GL_RENDERBUFFER, app.depth[e]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, s.w, s.h);
        for (GLuint fb : s.fbos) {
            glBindFramebuffer(GL_FRAMEBUFFER, fb);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, app.depth[e]);
        }
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glGenFramebuffers(1, &app.readFbo);
}

/// Colores de los bordes de la imagen de la TV (para las luces), leídos de una versión chiquita (mipmap)
static void processLed(App &app, const uint8_t *data, int lw, int lh);

/// Pide los colores de la TV (asincrónico: la GPU los copia a un búfer y se leen cuando ya terminó, sin esperarla)
static void updateLed(App &app, GLuint tex, int w, int h, int mips) {
    int lvl = 0;
    while (lvl + 1 < mips && (w >> lvl) > 64) lvl++;
    int lw = std::max(1, w >> lvl), lh = std::max(1, h >> lvl);
    if (lw > 128 || lh > 128) return;
    if (!app.ledPbo[0]) {
        glGenBuffers(2, app.ledPbo);
        for (int i = 0; i < 2; i++) {
            glBindBuffer(GL_PIXEL_PACK_BUFFER, app.ledPbo[i]);
            glBufferData(GL_PIXEL_PACK_BUFFER, 128 * 128 * 4, nullptr, GL_STREAM_READ);
        }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    // 1) leer lo que ya esté listo (de un pedido anterior)
    for (int i = 0; i < 2; i++) {
        if (!app.ledFence[i]) continue;
        GLenum st = glClientWaitSync(app.ledFence[i], 0, 0);
        if (st != GL_ALREADY_SIGNALED && st != GL_CONDITION_SATISFIED) continue;
        glDeleteSync(app.ledFence[i]); app.ledFence[i] = nullptr;
        glBindBuffer(GL_PIXEL_PACK_BUFFER, app.ledPbo[i]);
        const uint8_t *data = (const uint8_t *)glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)app.ledPw[i] * app.ledPh[i] * 4, GL_MAP_READ_BIT);
        if (data) { processLed(app, data, app.ledPw[i], app.ledPh[i]); glUnmapBuffer(GL_PIXEL_PACK_BUFFER); }
        glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    }
    // 2) pedir una lectura nueva en un búfer libre
    int slot = -1;
    for (int k = 0; k < 2; k++) { int i = (app.ledSlot + k) % 2; if (!app.ledFence[i]) { slot = i; break; } }
    if (slot < 0) return;   // los dos todavía en camino
    app.ledSlot = (slot + 1) % 2;
    glBindFramebuffer(GL_FRAMEBUFFER, app.readFbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, lvl);
    glBindBuffer(GL_PIXEL_PACK_BUFFER, app.ledPbo[slot]);
    glReadPixels(0, 0, lw, lh, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);   // no espera: la copia la hace la GPU
    glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    app.ledPw[slot] = lw; app.ledPh[slot] = lh;
    app.ledFence[slot] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

/// Promedios de los bordes (luces LED) y de la imagen en 3x3 zonas (luz de la pantalla)
static void processLed(App &app, const uint8_t *data, int lw, int lh) {
    const uint8_t *px = data;
    static float lin[256];   // sRGB → lineal, precalculado
    static bool linOk = false;
    if (!linOk) { for (int i = 0; i < 256; i++) lin[i] = powf(i / 255.f, 2.2f); linOk = true; }
    // fila 0 = abajo de la imagen
    auto avg = [&](float x0, float x1, float y0, float y1, float out[3]) {
        int ax = (int)(x0 * lw), bx = std::max(ax + 1, (int)(x1 * lw)), ay = (int)(y0 * lh), by = std::max(ay + 1, (int)(y1 * lh));
        double r = 0, g = 0, b = 0; int n = 0;
        for (int y = ay; y < by && y < lh; y++) for (int x = ax; x < bx && x < lw; x++) {
            const uint8_t *q = px + ((size_t)y * lw + x) * 4;
            r += lin[q[0]]; g += lin[q[1]]; b += lin[q[2]]; n++;
        }
        if (!n) return;
        float t[3] = {(float)(r / n), (float)(g / n), (float)(b / n)};
        // un poco más de saturación para que se note el color, y suavizado en el tiempo
        float m = (t[0] + t[1] + t[2]) / 3;
        for (int k = 0; k < 3; k++) {
            float v = std::max(0.f, m + (t[k] - m) * 1.5f);
            out[k] = app.ledReady ? out[k] + (v - out[k]) * 0.3f : v;
        }
    };
    const float B = 0.18f;   // ancho de la franja del borde
    for (int i = 0; i < 3; i++) {
        float a0 = i / 3.f, a1 = (i + 1) / 3.f;
        avg(0, B, 1 - a1, 1 - a0, app.led[0 + i]);      // izquierda (de arriba hacia abajo)
        avg(1 - B, 1, 1 - a1, 1 - a0, app.led[3 + i]);  // derecha
        avg(a0, a1, 1 - B, 1, app.led[6 + i]);          // arriba (de izquierda a derecha)
        avg(a0, a1, 0, B, app.led[9 + i]);              // abajo
    }
    // la pantalla entera en 3x3 zonas (fila 0 = arriba)
    for (int gy = 0; gy < 3; gy++)
        for (int gx = 0; gx < 3; gx++)
            avg(gx / 3.f, (gx + 1) / 3.f, 1 - (gy + 1) / 3.f, 1 - gy / 3.f, app.scrGrid[gy * 3 + gx]);
    app.ledReady = true;
}

struct TvInfo { XrPosef pose; float wm, hm; };

static void renderRoom(App &app, const XrView &view, const TvInfo *tv) {
    float P[16], V[16], VP[16];
    projMatrix(view.fov, P);
    viewMatrix(view.pose, V);
    mul4(P, V, VP);
    glClearColor(0.01f, 0.01f, 0.012f, 1);
    glClearDepthf(1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    glEnable(GL_DEPTH_TEST);
    glUseProgram(app.roomProg);
    glUniformMatrix4fv(app.rVP, 1, GL_FALSE, VP);
    glUniform3f(app.rEye, view.pose.position.x, view.pose.position.y, view.pose.position.z);
    glBindVertexArray(app.cubeVao);

    // Ejes de la habitación: "adelante" es hacia la TV desde el sillón (donde estás sentado)
    const float f[3] = {app.fwd[0], 0, app.fwd[2]}, up[3] = {0, 1, 0}, r[3] = {-app.fwd[2], 0, app.fwd[0]};
    float N[9] = {r[0], r[1], r[2], up[0], up[1], up[2], f[0], f[1], f[2]};
    glUniformMatrix3fv(app.rN, 1, GL_FALSE, N);
    float anc[3] = {app.anc[0], app.worldY, app.anc[2]};   // worldY: el mundo sube o baja cuando cambiás de fila en el cine
    glUniform3fv(app.rAnc, 1, anc); glUniform3fv(app.rRx, 1, r); glUniform3fv(app.rFz, 1, f);
    auto local = [&](float x, float y, float z, float out[3]) {
        for (int k = 0; k < 3; k++) out[k] = anc[k] + r[k] * x + up[k] * y + f[k] * z;
    };
    float lp[3]; local(0, 2.8f, 0.9f, lp);
    glUniform3fv(app.rLight, 1, lp);
    float lampP[3]; local(1.55f, 1.35f, -0.15f, lampP);
    glUniform3fv(app.rLamp, 1, lampP);

    // Tiras LED en la parte de atrás de la TV: 3 por lado, apuntando hacia afuera y hacia la pared
    bool ledOn = tv && app.ledReady && g_leds;
    glUniform1f(app.rLedOn, ledOn ? 1.f : 0.f);
    float ambC[3] = {0, 0, 0};
    if (ledOn) {
        const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
        float tr[3], tu[3], tn[3];
        rotate(tv->pose.orientation, ex, tr); rotate(tv->pose.orientation, ey, tu); rotate(tv->pose.orientation, ez, tn);
        float tc[3] = {tv->pose.position.x, tv->pose.position.y, tv->pose.position.z};
        float hw = tv->wm / 2 - 0.1f, hh = tv->hm / 2 - 0.1f;
        // Un foco por lado, en el medio del borde, apuntando hacia afuera y hacia la pared
        float LP[4][3], LD[4][3];
        for (int side = 0; side < 4; side++) {
            float x = side == 0 ? -hw : side == 1 ? hw : 0.f;
            float y = side == 2 ? hh : side == 3 ? -hh : 0.f;
            float ox = side == 0 ? -1.f : side == 1 ? 1.f : 0.f, oy = side == 2 ? 1.f : side == 3 ? -1.f : 0.f;
            float len = 0;
            for (int a = 0; a < 3; a++) {
                LP[side][a] = tc[a] + tr[a] * x + tu[a] * y - tn[a] * 0.07f;
                LD[side][a] = tr[a] * ox + tu[a] * oy - tn[a] * 0.8f;
                len += LD[side][a] * LD[side][a];
            }
            len = sqrtf(len);
            for (int a = 0; a < 3; a++) LD[side][a] /= len;
        }
        for (int k = 0; k < 12; k++) for (int a = 0; a < 3; a++) ambC[a] += app.led[k][a] / 12.f;
        glUniform3fv(app.rLedP, 4, &LP[0][0]);
        glUniform3fv(app.rLedD, 4, &LD[0][0]);
        glUniform3fv(app.rLedC, 12, &app.led[0][0]);
        for (float &a : ambC) a *= 0.06f * g_lightGain.load();   // y un poco de ese color en todo el cuarto
    }
    glUniform3fv(app.rAmb, 1, ambC);
    // Luz de la pantalla: el color promedio de la imagen, hacia el living
    float scrC[3] = {0.12f, 0.13f, 0.16f}, scrP[3] = {0, 0, 0}, scrN[3] = {0, 0, 1};
    if (tv) {
        const float ez[3] = {0, 0, 1};
        rotate(tv->pose.orientation, ez, scrN);
        for (int a = 0; a < 3; a++) scrP[a] = (&tv->pose.position.x)[a] + scrN[a] * 0.02f;
        if (app.ledReady) {
            for (int a = 0; a < 3; a++) { scrC[a] = 0; for (int k = 0; k < 12; k++) scrC[a] += app.led[k][a] / 12.f; scrC[a] = 0.05f + scrC[a] * 1.6f; }
        }
    }
    glUniform3fv(app.rScrP, 1, scrP); glUniform3fv(app.rScrN, 1, scrN); glUniform3fv(app.rScrC, 1, scrC);
    {
        float sr[3] = {1, 0, 0}, su[3] = {0, 1, 0};
        float hw = 1.1f, hh = 0.7f;
        if (tv) {
            const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0};
            rotate(tv->pose.orientation, ex, sr); rotate(tv->pose.orientation, ey, su);
            hw = tv->wm / 2; hh = tv->hm / 2;
        }
        glUniform3fv(app.rScrR, 1, sr); glUniform3fv(app.rScrU, 1, su); glUniform2f(app.rScrH, hw, hh);
        float g[9][3];
        for (int i = 0; i < 9; i++) for (int k = 0; k < 3; k++) g[i][k] = app.ledReady ? app.scrGrid[i][k] : scrC[k];
        glUniform3fv(app.rScrG, 9, &g[0][0]);
    }
    glUniform1f(app.rGain, g_lightGain.load());
    glUniform1f(app.rScrK, g_env == 2 ? 0.035f : 0.45f);   // en el cine la pantalla grande ilumina toda la sala

    glUniform1f(app.rTime, (float)fmod(nowMs() / 1000.0, 1000.0));
    // Cine: sala oscura con butacas en gradas; solo la pantalla ilumina (sin luces detrás)
    if (g_env == 2) {
        glUniform1f(app.rLedOn, 0.f);
        float a0[3] = {0, 0, 0}; glUniform3fv(app.rAmb, 1, a0);
        auto cb = [&](float x, float y, float z, float sx, float sy, float sz, float cr, float cg, float cbl, int mat) {
            float c[3]; local(x, y, z, c);
            float M[16] = {r[0] * sx, r[1] * sx, r[2] * sx, 0, up[0] * sy, up[1] * sy, up[2] * sy, 0, f[0] * sz, f[1] * sz, f[2] * sz, 0, c[0], c[1], c[2], 1};
            glUniformMatrix4fv(app.rM, 1, GL_FALSE, M);
            glUniform3f(app.rCol, cr, cg, cbl);
            glUniform1i(app.rMat, mat);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        };
        // sala: 13 m de ancho, de z=-6 a z=9 (pantalla), 7 m de alto; el piso baja hacia la pantalla en gradas
        cb(0, 2.5f, 1.5f, 13.0f, 9.0f, 15.0f, 0.10f, 0.07f, 0.07f, 5);          // paredes y techo (tela oscura)
        for (int row = -4; row <= 6; row++) {
            float z = row * 1.1f, y = -0.28f * row;                             // tu fila (0) está a la altura del piso real
            cb(0, y - 0.15f, z, 12.9f, 0.3f, 1.1f, 0.12f, 0.06f, 0.05f, 2);    // grada alfombrada
            if (row > 4) continue;
            for (int k = -5; k <= 5; k++) {
                if (k == 0) continue;                                           // pasillo central
                float x = k * 0.62f + (k > 0 ? 0.2f : -0.2f);
                cb(x, y + 0.24f, z - 0.05f, 0.52f, 0.12f, 0.5f, 0.42f, 0.05f, 0.06f, 6);   // asiento
                cb(x, y + 0.55f, z - 0.32f, 0.52f, 0.65f, 0.1f, 0.40f, 0.05f, 0.06f, 6);   // respaldo
                cb(x + 0.29f, y + 0.36f, z - 0.05f, 0.06f, 0.36f, 0.5f, 0.05f, 0.04f, 0.04f, 0);   // apoyabrazos
            }
            cb(-0.2f, y + 0.02f, z + 0.5f, 0.1f, 0.01f, 0.05f, 0.1f, 0.25f, 0.6f, 4);   // lucecita del pasillo
            cb(0.2f, y + 0.02f, z + 0.5f, 0.1f, 0.01f, 0.05f, 0.1f, 0.25f, 0.6f, 4);
        }
        cb(0, -2.1f, 7.6f, 12.9f, 0.4f, 3.0f, 0.06f, 0.04f, 0.04f, 2);          // frente, debajo de la pantalla
        cb(-5.6f, 2.6f, 8.6f, 1.6f, 6.0f, 0.3f, 0.45f, 0.04f, 0.05f, 6);        // telones
        cb(5.6f, 2.6f, 8.6f, 1.6f, 6.0f, 0.3f, 0.45f, 0.04f, 0.05f, 6);
        cb(0, 5.9f, 8.6f, 12.9f, 0.9f, 0.3f, 0.45f, 0.04f, 0.05f, 6);
        for (int k = 0; k < 6; k++) {                                          // paneles en las paredes
            cb(-6.45f, 2.2f, -4.0f + k * 2.2f, 0.06f, 3.2f, 1.6f, 0.16f, 0.09f, 0.08f, 6);
            cb(6.45f, 2.2f, -4.0f + k * 2.2f, 0.06f, 3.2f, 1.6f, 0.16f, 0.09f, 0.08f, 6);
        }
        cb(-6.4f, 3.3f, -5.9f, 0.6f, 0.25f, 0.04f, 0.1f, 0.9f, 0.3f, 4);        // cartel de salida
        glBindVertexArray(0);
        glDisable(GL_DEPTH_TEST);
        return;
    }
    // Carga: un vacío oscuro con un piso de grilla que se pierde en un horizonte ámbar y polvo flotando
    if (g_env == 4) {
        auto boxW = [&](const float c[3], float sx, float sy, float sz, int mat) {
            float M[16] = {r[0] * sx, r[1] * sx, r[2] * sx, 0, up[0] * sy, up[1] * sy, up[2] * sy, 0, f[0] * sz, f[1] * sz, f[2] * sz, 0, c[0], c[1], c[2], 1};
            glUniformMatrix4fv(app.rM, 1, GL_FALSE, M);
            glUniform3f(app.rCol, 1, 1, 1);
            glUniform1i(app.rMat, mat);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        };
        glDepthMask(GL_FALSE);
        float ec[3] = {view.pose.position.x, view.pose.position.y, view.pose.position.z};
        boxW(ec, 60, 60, 60, 12);
        glDepthMask(GL_TRUE);
        float pc[3]; local(0, -0.05f, 0, pc);
        boxW(pc, 56, 0.1f, 56, 13);
        glBindVertexArray(0);
        glDisable(GL_DEPTH_TEST);
        return;
    }
    // Espacio: cielo alrededor y una plataforma; sin habitación (la TV flota como una ventana más)
    if (g_env == 1) {
        auto boxW = [&](const float c[3], float sx, float sy, float sz, int mat) {
            float M[16] = {r[0] * sx, r[1] * sx, r[2] * sx, 0, up[0] * sy, up[1] * sy, up[2] * sy, 0, f[0] * sz, f[1] * sz, f[2] * sz, 0, c[0], c[1], c[2], 1};
            glUniformMatrix4fv(app.rM, 1, GL_FALSE, M);
            glUniform3f(app.rCol, 1, 1, 1);
            glUniform1i(app.rMat, mat);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        };
        glDepthMask(GL_FALSE);
        float ec[3] = {view.pose.position.x, view.pose.position.y, view.pose.position.z};
        boxW(ec, 60, 60, 60, 8);
        glDepthMask(GL_TRUE);
        float pc[3]; local(0, -0.05f, 0, pc);
        boxW(pc, 11.4f, 0.1f, 11.4f, 9);
        glBindVertexArray(0);
        glDisable(GL_DEPTH_TEST);
        return;
    }

    auto box = [&](float x, float y, float z, float sx, float sy, float sz, float cr, float cg, float cb, int mat = 0) {
        float c[3]; local(x, y, z, c);
        float M[16] = {r[0] * sx, r[1] * sx, r[2] * sx, 0, up[0] * sy, up[1] * sy, up[2] * sy, 0, f[0] * sz, f[1] * sz, f[2] * sz, 0, c[0], c[1], c[2], 1};
        glUniformMatrix4fv(app.rM, 1, GL_FALSE, M);
        glUniform3f(app.rCol, cr, cg, cb);
        glUniform1i(app.rMat, mat);
        glDrawArrays(GL_TRIANGLES, 0, 36);
    };
    // Habitación: 7 m de ancho, de z=-2.8 (atrás del sillón) a z=2.8 (pared de la TV), 2.9 m de alto
    box(0, -0.05f, 0.3f, 7.0f, 0.1f, 6.2f, 0.30f, 0.19f, 0.11f, 1);         // piso de madera
    box(0, 2.95f, 0.3f, 7.0f, 0.1f, 6.2f, 0.55f, 0.54f, 0.52f, 5);          // techo
    box(0, 1.45f, 3.45f, 7.0f, 2.9f, 0.1f, 0.30f, 0.30f, 0.33f, 5);      // pared de la TV
    box(0, 1.45f, -2.85f, 7.0f, 2.9f, 0.1f, 0.52f, 0.48f, 0.43f, 5);     // pared de atrás
    box(-3.55f, 1.45f, 0.3f, 0.1f, 2.9f, 6.2f, 0.50f, 0.46f, 0.41f, 5);     // pared izquierda
    box(3.55f, 1.45f, 0.3f, 0.1f, 2.9f, 6.2f, 0.50f, 0.46f, 0.41f, 5);      // pared derecha
    // zócalos
    box(0, 0.05f, 3.39f, 7.0f, 0.1f, 0.02f, 0.85f, 0.83f, 0.8f);
    box(0, 0.05f, -2.79f, 7.0f, 0.1f, 0.02f, 0.85f, 0.83f, 0.8f);
    box(-3.49f, 0.05f, 0.3f, 0.02f, 0.1f, 6.2f, 0.85f, 0.83f, 0.8f);
    box(3.49f, 0.05f, 0.3f, 0.02f, 0.1f, 6.2f, 0.85f, 0.83f, 0.8f);
    // alfombra
    box(0, 0.006f, 1.5f, 3.2f, 0.012f, 2.1f, 0.18f, 0.22f, 0.32f, 2);
    // mueble de la TV con cajones, separado de la pared (así las luces de atrás tienen lugar para pintarla)
    box(0, 0.24f, 2.85f, 2.9f, 0.48f, 0.45f, 0.13f, 0.09f, 0.06f);
    for (int i = -1; i <= 1; i++) box(i * 0.95f, 0.24f, 2.62f, 0.88f, 0.38f, 0.01f, 0.18f, 0.13f, 0.09f);
    // parlantes
    for (int sgn = -1; sgn <= 1; sgn += 2) {
        box(sgn * 1.85f, 0.6f, 2.85f, 0.32f, 1.2f, 0.32f, 0.05f, 0.05f, 0.055f);
        box(sgn * 1.85f, 0.95f, 2.685f, 0.18f, 0.18f, 0.01f, 0.02f, 0.02f, 0.02f);
        box(sgn * 1.85f, 0.55f, 2.685f, 0.24f, 0.24f, 0.01f, 0.02f, 0.02f, 0.02f);
    }
    // sillón: estás sentado en el medio
    box(0, 0.21f, -0.05f, 2.3f, 0.42f, 0.9f, 0.20f, 0.24f, 0.30f, 6);     // asiento
    box(0, 0.45f, 0.0f, 2.0f, 0.08f, 0.75f, 0.23f, 0.27f, 0.34f, 6);      // almohadón
    box(0, 0.68f, -0.44f, 2.3f, 0.95f, 0.22f, 0.20f, 0.24f, 0.30f, 6);    // respaldo
    box(-1.08f, 0.38f, -0.05f, 0.18f, 0.76f, 0.9f, 0.19f, 0.22f, 0.28f, 6);  // apoyabrazos
    box(1.08f, 0.38f, -0.05f, 0.18f, 0.76f, 0.9f, 0.19f, 0.22f, 0.28f, 6);
    // mesa ratona
    box(0, 0.38f, 1.25f, 1.2f, 0.05f, 0.6f, 0.30f, 0.20f, 0.12f);
    for (int sx = -1; sx <= 1; sx += 2) for (int sz = -1; sz <= 1; sz += 2) box(sx * 0.55f, 0.18f, 1.25f + sz * 0.25f, 0.04f, 0.36f, 0.04f, 0.1f, 0.08f, 0.06f);
    // mesita con lámpara de pie al lado del sillón
    box(1.55f, 0.25f, -0.15f, 0.45f, 0.5f, 0.45f, 0.30f, 0.20f, 0.12f);
    box(1.55f, 0.85f, -0.15f, 0.03f, 0.7f, 0.03f, 0.1f, 0.1f, 0.1f);
    box(1.55f, 1.33f, -0.15f, 0.36f, 0.28f, 0.36f, 0.55f, 0.48f, 0.38f, 6);   // pantalla de la lámpara (apagada)
    // biblioteca en la pared izquierda, con libros
    box(-3.3f, 1.0f, 1.3f, 0.35f, 2.0f, 1.6f, 0.22f, 0.15f, 0.09f);
    for (int sh = 0; sh < 4; sh++) {
        float y0 = 0.25f + sh * 0.48f;
        box(-3.12f, y0, 1.3f, 0.02f, 0.03f, 1.5f, 0.28f, 0.19f, 0.11f);
        float z = 0.6f;
        int k = sh * 31;
        while (z < 1.98f) {
            float w = 0.03f + 0.03f * (float)((k * 37) % 7) / 6.f, hgt = 0.26f + 0.12f * (float)((k * 53) % 5) / 4.f;
            float cr = 0.2f + 0.5f * (float)((k * 17) % 9) / 8.f, cg = 0.15f + 0.4f * (float)((k * 29) % 7) / 6.f, cb = 0.15f + 0.45f * (float)((k * 13) % 5) / 4.f;
            if ((k % 11) != 3) box(-3.25f, y0 + 0.02f + hgt / 2, z + w / 2, 0.22f, hgt, w * 0.95f, cr, cg, cb);
            z += w; k++;
        }
    }
    // planta en una maceta (rincón derecho, al lado de la TV)
    box(2.9f, 0.22f, 2.9f, 0.36f, 0.44f, 0.36f, 0.55f, 0.35f, 0.22f);
    for (int i = 0; i < 7; i++) {
        float a = i * 0.9f, rr = 0.12f + 0.05f * (i % 3);
        box(2.9f + cosf(a) * rr, 0.6f + 0.13f * i, 2.9f + sinf(a) * rr, 0.22f, 0.07f, 0.22f, 0.12f, 0.32f + 0.05f * (i % 2), 0.12f);
    }
    // ventana de noche en la pared derecha
    box(3.495f, 1.55f, 0.6f, 0.01f, 1.3f, 2.0f, 1, 1, 1, 3);
    box(3.47f, 2.22f, 0.6f, 0.06f, 0.06f, 2.1f, 0.85f, 0.83f, 0.8f);
    box(3.47f, 0.88f, 0.6f, 0.08f, 0.06f, 2.1f, 0.85f, 0.83f, 0.8f);
    box(3.47f, 1.55f, -0.45f, 0.06f, 1.4f, 0.06f, 0.85f, 0.83f, 0.8f);
    box(3.47f, 1.55f, 1.65f, 0.06f, 1.4f, 0.06f, 0.85f, 0.83f, 0.8f);
    box(3.47f, 1.55f, 0.6f, 0.05f, 1.3f, 0.04f, 0.85f, 0.83f, 0.8f);
    // cuadro en la pared de atrás… y otro a la izquierda
    box(-3.48f, 1.7f, -1.3f, 0.03f, 0.9f, 1.3f, 0.05f, 0.05f, 0.05f);
    box(-3.465f, 1.7f, -1.3f, 0.01f, 0.8f, 1.2f, 1, 1, 1, 7);
    // luz empotrada en el techo
    box(0, 2.895f, 0.9f, 1.0f, 0.01f, 1.0f, 0.5f, 0.5f, 0.5f, 0);   // (apagada)

    if (tv) {
        // Marco de la TV (la imagen va encima, como capa del Quest) y soporte a la pared
        float tc[3] = {tv->pose.position.x, tv->pose.position.y, tv->pose.position.z};
        float rr[3], uu[3], nn[3];
        const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
        rotate(tv->pose.orientation, ex, rr); rotate(tv->pose.orientation, ey, uu); rotate(tv->pose.orientation, ez, nn);
        float N2[9] = {rr[0], rr[1], rr[2], uu[0], uu[1], uu[2], nn[0], nn[1], nn[2]};
        glUniformMatrix3fv(app.rN, 1, GL_FALSE, N2);
        auto tvBox = [&](float dz, float sx, float sy, float sz, float g) {
            float c[3] = {tc[0] - nn[0] * dz, tc[1] - nn[1] * dz, tc[2] - nn[2] * dz};
            float M[16] = {rr[0] * sx, rr[1] * sx, rr[2] * sx, 0, uu[0] * sy, uu[1] * sy, uu[2] * sy, 0, nn[0] * sz, nn[1] * sz, nn[2] * sz, 0, c[0], c[1], c[2], 1};
            glUniformMatrix4fv(app.rM, 1, GL_FALSE, M);
            glUniform3f(app.rCol, g, g, g * 1.1f);
            glUniform1i(app.rMat, 4);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        };
        tvBox(0.03f, tv->wm + 0.04f, tv->hm + 0.04f, 0.05f, 0.012f);   // marco fino
        tvBox(0.09f, tv->wm * 0.9f, tv->hm * 0.9f, 0.08f, 0.02f);     // cuerpo de atrás
        // Pie de la TV sobre el mueble
        float bottom = tc[1] - tv->hm / 2, top = 0.49f;
        if (bottom > top + 0.01f) {
            float neck = bottom - top;
            float c[3] = {tc[0] - nn[0] * 0.08f, top + neck / 2, tc[2] - nn[2] * 0.08f};
            float M[16] = {rr[0] * 0.1f, rr[1] * 0.1f, rr[2] * 0.1f, 0, 0, neck, 0, 0, nn[0] * 0.05f, nn[1] * 0.05f, nn[2] * 0.05f, 0, c[0], c[1], c[2], 1};
            glUniformMatrix4fv(app.rM, 1, GL_FALSE, M);
            glUniform3f(app.rCol, 0.05f, 0.05f, 0.055f);
            glUniform1i(app.rMat, 0);
            glDrawArrays(GL_TRIANGLES, 0, 36);
            float c2[3] = {tc[0] - nn[0] * 0.08f, top + 0.01f, tc[2] - nn[2] * 0.08f};
            float M2[16] = {rr[0] * 0.6f, rr[1] * 0.6f, rr[2] * 0.6f, 0, 0, 0.02f, 0, 0, nn[0] * 0.3f, nn[1] * 0.3f, nn[2] * 0.3f, 0, c2[0], c2[1], c2[2], 1};
            glUniformMatrix4fv(app.rM, 1, GL_FALSE, M2);
            glDrawArrays(GL_TRIANGLES, 0, 36);
        }
    }
    glBindVertexArray(0);
    glDisable(GL_DEPTH_TEST);
}

// ---------------------------------------------------------------------------
// Sala VR: la TV (menú y escritorio principal) y las ventanas flotantes
// ---------------------------------------------------------------------------
static XrQuaternionf facing(const float pos[3], const float target[3]) {
    float th = atan2f(target[0] - pos[0], target[2] - pos[2]);   // girar +Z hacia el usuario
    return {0, sinf(th / 2), 0, cosf(th / 2)};
}

/// Como facing(), pero además inclinada hacia vos (arriba de tu cabeza mira hacia abajo, abajo mira hacia arriba), como en Meta
static XrQuaternionf facingTilt(const float pos[3], const float target[3]) {
    float dx = target[0] - pos[0], dy = target[1] - pos[1], dz = target[2] - pos[2];
    float th = atan2f(dx, dz);
    float ph = atan2f(dy, sqrtf(dx * dx + dz * dz));
    ph = std::max(-1.2f, std::min(1.2f, ph));
    float a = -ph;   // giro sobre el X propio: +Z (el frente) sube o baja hacia la cabeza
    float cy = cosf(th / 2), sy = sinf(th / 2), cx = cosf(a / 2), sx = sinf(a / 2);
    return {cy * sx, sy * cx, -sy * sx, cy * cx};
}

static void placePanel(App &app, UiPanel *p) {
    if (p->id == 0x2FFFFFB0) {
        // Cartel de cómo dibujar el portal: delante tuyo, un poco arriba de la vista
        float f[3] = {app.headFwd[0], 0, app.headFwd[2]};
        float l = sqrtf(f[0] * f[0] + f[2] * f[2]);
        if (l < 1e-3f) { f[0] = 0; f[2] = -1; l = 1; }
        f[0] /= l; f[2] /= l;
        float pos[3] = {app.headPos[0] + f[0] * 1.1f, app.headPos[1] + 0.22f, app.headPos[2] + f[2] * 1.1f};
        p->pose.position = {pos[0], pos[1], pos[2]};
        p->pose.orientation = facingTilt(pos, app.headPos);
        p->placed = true;
        return;
    }
    if (p->id == LOADING_ID) {
        // Pantalla de carga: delante de donde mirás, un poco lejos y a la altura de los ojos
        float f[3] = {app.headFwd[0], 0, app.headFwd[2]};
        float l = sqrtf(f[0] * f[0] + f[2] * f[2]);
        if (l < 1e-3f) { f[0] = 0; f[2] = -1; l = 1; }
        f[0] /= l; f[2] /= l;
        float pos[3] = {app.headPos[0] + f[0] * 2.0f, app.headPos[1] - 0.05f, app.headPos[2] + f[2] * 2.0f};
        float tgt[3] = {app.headPos[0], pos[1], app.headPos[2]};
        p->pose.position = {pos[0], pos[1], pos[2]};
        p->pose.orientation = facing(pos, tgt);
        p->placed = true;
        return;
    }
    if (p->id == KEYBOARD_ID || p->id == LOCK_NOTE_ID) {
        // Debajo de la pantalla o ventana desde la que se abrió (el aviso de la pausa, donde aparece el teclado) (un poco hacia vos, inclinado como sobre una mesa)
        for (UiPanel *q : g_ui) if (q->id == g_kbAnchor.load() && q->placed && !q->dead && q != p && !q->hud) {
            const float ey[3] = {0, 1, 0};
            float ay[3]; rotate(q->pose.orientation, ey, ay);
            float pos[3] = {q->pose.position.x - ay[0] * (q->hm / 2 + 0.09f + p->hm / 2), q->pose.position.y - ay[1] * (q->hm / 2 + 0.09f + p->hm / 2),
                            q->pose.position.z - ay[2] * (q->hm / 2 + 0.09f + p->hm / 2)};
            float th[3] = {app.headPos[0] - pos[0], 0, app.headPos[2] - pos[2]};
            float tl = sqrtf(th[0] * th[0] + th[2] * th[2]);
            // cerca tuyo, al alcance de la mano (a unos 60 cm como mucho), en la dirección de la pantalla
            const float reach = 0.6f;
            if (tl > reach) { pos[0] = app.headPos[0] - th[0] / tl * reach; pos[2] = app.headPos[2] - th[2] / tl * reach; }
            pos[1] = std::min(app.headPos[1] - 0.35f, std::max(pos[1], app.headPos[1] - 0.65f));   // a la altura de las manos
            p->pose.position = {pos[0], pos[1], pos[2]};
            p->pose.orientation = facingTilt(pos, app.headPos);
            p->placed = true;
            return;
        }
        // Sin pantalla de referencia: abajo y adelante tuyo, inclinado hacia vos
        float f[3] = {app.headFwd[0], 0, app.headFwd[2]};
        float l = sqrtf(f[0] * f[0] + f[2] * f[2]);
        if (l < 1e-3f) { f[0] = 0; f[2] = -1; l = 1; }
        f[0] /= l; f[2] /= l;
        float pos[3] = {app.headPos[0] + f[0] * 0.55f, app.headPos[1] - 0.40f, app.headPos[2] + f[2] * 0.55f};
        p->pose.position = {pos[0], pos[1], pos[2]};
        p->pose.orientation = facingTilt(pos, app.headPos);
        p->placed = true;
        return;
    }
    if (p->inGame && g_inGameNow) {
        // Durante un juego: delante de donde mirás (el menú más cerca, las ventanas un poco más lejos)
        float d = p->id == GAME_MENU_ID ? 0.95f : 1.6f;
        float f[3] = {app.headFwd[0], 0, app.headFwd[2]};
        float l = sqrtf(f[0] * f[0] + f[2] * f[2]);
        if (l < 1e-3f) { f[0] = 0; f[2] = -1; l = 1; }
        f[0] /= l; f[2] /= l;
        float pos[3] = {app.headPos[0] + f[0] * d, app.headPos[1] - (p->id == GAME_MENU_ID ? 0.12f : 0.05f), app.headPos[2] + f[2] * d};
        float tgt[3] = {app.headPos[0], pos[1], app.headPos[2]};
        p->pose.position = {pos[0], pos[1], pos[2]};
        p->pose.orientation = facing(pos, tgt);
        p->placed = true;
        return;
    }
    if (p->id == WRIST_MENU_ID) {
        // Menú del reloj: flotando encima de donde estaba el reloj, un poco hacia vos, mirándote
        float pos[3] = {g_watchWorld[0], g_watchWorld[1] + 0.06f + p->hm / 2, g_watchWorld[2]};
        float toHead[3] = {app.headPos[0] - pos[0], 0, app.headPos[2] - pos[2]};
        float l = sqrtf(toHead[0] * toHead[0] + toHead[2] * toHead[2]);
        if (l > 1e-3f) { pos[0] += toHead[0] / l * 0.04f; pos[2] += toHead[2] / l * 0.04f; }
        float tgt[3] = {app.headPos[0], pos[1], app.headPos[2]};
        p->pose.position = {pos[0], pos[1], pos[2]};
        p->pose.orientation = facing(pos, tgt);
        p->placed = true;
        return;
    }
    // TV al frente; las ventanas en arco a los costados (derecha, izquierda, más a la derecha…)
    static const float angles[] = {40, -40, 75, -75, 20, -20, 55, -55};
    // La TV va sobre el mueble (que mide 48 cm), a la altura de los ojos si se puede
    // Living: sobre su pie, encima del mueble. Espacio: flotando delante tuyo, a la altura de los ojos
    bool floating = g_env == 1 || g_env == 3 || g_env == 4;
    if (p->tv) {
        // Tamaño de la TV según el lugar: pantalla de cine enorme, o TV de living
        float W = g_env == 2 ? 9.0f : 2.2f;
        p->hm = p->hm / p->wm * W; p->wm = W;
    }
    float ang = 0, dist = floating ? 2.6f : 2.85f, y = floating ? app.eyeH : 0.59f + p->hm / 2;
    if (p->tv && g_env == 2) { dist = 8.6f; y = 2.6f; }
    if (!floating) y += app.worldY;
    if (!p->tv) { ang = angles[app.placedWindows % 8] * (float)M_PI / 180.f; dist = 2.0f; y = app.eyeH - 0.05f; app.placedWindows++; }
    float c = cosf(ang), sn = sinf(ang);
    // rotar el "adelante" alrededor del eje vertical (ángulo positivo = hacia la derecha)
    float dx = app.fwd[0] * c - app.fwd[2] * sn, dz = app.fwd[0] * sn + app.fwd[2] * c;
    float pos[3] = {app.anc[0] + dx * dist, y, app.anc[2] + dz * dist};
    float tgt[3] = {app.anc[0], y, app.anc[2]};
    p->pose.position = {pos[0], pos[1], pos[2]};
    p->pose.orientation = facing(pos, tgt);
    p->placed = true;
}

static void destroyPanelGL(App &app, UiPanel *p) {
    for (auto &h : p->held) { app.destroyImage(app.display, h.egl); AImage_delete(h.img); }
    p->held.clear();
    if (!p->fbos.empty()) glDeleteFramebuffers((GLsizei)p->fbos.size(), p->fbos.data());
    if (p->sc != XR_NULL_HANDLE) xrDestroySwapchain(p->sc);
    for (int e = 0; e < 2; e++) {
        if (!p->fbos3[e].empty()) glDeleteFramebuffers((GLsizei)p->fbos3[e].size(), p->fbos3[e].data());
        if (p->sc3[e] != XR_NULL_HANDLE) xrDestroySwapchain(p->sc3[e]);
    }
    if (p->depthTex) { glDeleteTextures(1, &p->depthTex); p->depthTex = 0; }
    if (p->reader) AImageReader_delete(p->reader);
}

/// Copia lo último que dibujó la interfaz Java de cada panel a su swapchain
/// Ajustes de imagen para el próximo dibujo con el programa de video (on=false: sin filtro)
static void setFilter(App &app, bool on, const float rect[4], float tw, float th) {
    float v[5];
    { std::lock_guard<std::mutex> lk(g_imgLock); for (int k = 0; k < 5; k++) v[k] = g_img[k]; }
    bool neutral = fabsf(v[0] - 1) < 1e-3f && fabsf(v[1] - 1) < 1e-3f && fabsf(v[2] - 1) < 1e-3f && v[3] < 1e-3f && fabsf(v[4]) < 1e-3f;
    glUniform1f(app.uFilt, on && !neutral ? 1.f : 0.f);
    glUniform4f(app.uRect, rect[0], rect[1], rect[2], rect[3]);
    glUniform4f(app.uAdj, v[0], v[1], v[2], v[3]);
    glUniform1f(app.uWarm, v[4]);
    glUniform2f(app.uTexel, tw, th);
}

static void updateUiPanels(App &app) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    for (size_t i = 0; i < g_ui.size();) {
        UiPanel *p = g_ui[i];
        if (p->closeAt > 0 && nowMs() - p->closeAt > 190) p->dead = true;
        if (p->dead) { glFinish(); destroyPanelGL(app, p); delete p; g_ui.erase(g_ui.begin() + i); continue; }
        i++;
        if (p->openAt == 0 && p->ready && p->placed) p->openAt = nowMs();
        if (p->id == WRIST_MENU_ID && p->placed && g_wristFollow && g_watchNowOk) {
            // Menú del reloj: encima del reloj, un poco hacia vos, mirándote; sigue al reloj (suave, sin temblar)
            float tgt[3] = {g_watchNow[0], g_watchNow[1] + 0.06f + p->hm / 2, g_watchNow[2]};
            float toHead[3] = {app.headPos[0] - tgt[0], 0, app.headPos[2] - tgt[2]};
            float l = sqrtf(toHead[0] * toHead[0] + toHead[2] * toHead[2]);
            if (l > 1e-3f) { tgt[0] += toHead[0] / l * 0.04f; tgt[2] += toHead[2] / l * 0.04f; }
            float *pp = &p->pose.position.x;
            for (int k = 0; k < 3; k++) pp[k] += (tgt[k] - pp[k]) * 0.25f;
            float hp[3] = {app.headPos[0], pp[1], app.headPos[2]};
            p->pose.orientation = facing(pp, hp);
        }
        // Curva: a más cantidad, menor radio (el ángulo que abarca la ventana crece hasta ~120°)
        if (p->curveAmt > 0.01f && !p->parent && !p->hud) { p->curved = true; p->radius = std::max(0.4f, p->wm / (p->curveAmt * 2.1f)); }
        else { p->curved = false; p->radius = 0; }
        if (p->parent) {
            // Barra de botones: arriba a la izquierda de su ventana, por fuera (siguiendo la curva si está curvada)
            // (mientras la estás tocando, por ejemplo deslizando el tamaño de su pantalla, la barra queda quieta:
            //  si siguiera al borde de la pantalla que crece, se correría debajo del dedo y el tamaño saltaría)
            const bool held = p->placed && (app.hp[0].down == p->id || app.hp[1].down == p->id);
            for (UiPanel *q : g_ui) if (q->id == p->parent && q->placed && !held) {
                if (p->baseW == 0) { p->baseW = p->wm; p->baseH = p->hm; }
                // Crece con la distancia (para que se pueda usar de lejos), pero solo se recalcula cuando se mueve su ventana:
                // así no se mueve ni cambia de tamaño cuando movés la cabeza
                {
                    float mx = q->pose.position.x - p->growAt[0], my = q->pose.position.y - p->growAt[1], mz = q->pose.position.z - p->growAt[2];
                    if (p->growK <= 0 || mx * mx + my * my + mz * mz > 0.05f * 0.05f) {
                        float qdx = q->pose.position.x - app.headPos[0], qdz = q->pose.position.z - app.headPos[2];
                        p->growK = std::max(1.0f, sqrtf(qdx * qdx + qdz * qdz) / 2.4f);
                        p->growAt[0] = q->pose.position.x; p->growAt[1] = q->pose.position.y; p->growAt[2] = q->pose.position.z;
                    }
                }
                const float grow = p->growK;
                p->wm = p->baseW * grow; p->hm = p->baseH * grow;
                const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
                float ax[3], ay[3], an[3];
                rotate(q->pose.orientation, ex, ax); rotate(q->pose.orientation, ey, ay); rotate(q->pose.orientation, ez, an);
                float ox = -q->wm / 2 + p->wm / 2, oy = q->hm / 2 + p->hm / 2 + 0.03f;
                if (p->dock == 1) {   // abajo a la derecha (si no entra al lado de la barrita para mover, va más abajo)
                    ox = q->wm / 2 - p->wm / 2;
                    const bool clash = q->wm / 2 - p->wm < 0.2f;
                    oy = -(q->hm / 2 + p->hm / 2 + (clash ? 0.09f : 0.02f));
                }
                if (q->curved && q->radius > 0.3f) {
                    float R = q->radius, th = ox / R;
                    float cx = q->pose.position.x + an[0] * R, cz = q->pose.position.z + an[2] * R, cy = q->pose.position.y + an[1] * R;
                    p->pose.position = {cx + ax[0] * R * sinf(th) - an[0] * R * cosf(th) + ay[0] * oy,
                                        cy + ax[1] * R * sinf(th) - an[1] * R * cosf(th) + ay[1] * oy,
                                        cz + ax[2] * R * sinf(th) - an[2] * R * cosf(th) + ay[2] * oy};
                    XrQuaternionf yq{0, sinf(-th / 2), 0, cosf(-th / 2)};
                    const XrQuaternionf &a = q->pose.orientation;
                    p->pose.orientation = {a.w * yq.x + a.x * yq.w + a.y * yq.z - a.z * yq.y, a.w * yq.y - a.x * yq.z + a.y * yq.w + a.z * yq.x,
                                           a.w * yq.z + a.x * yq.y - a.y * yq.x + a.z * yq.w, a.w * yq.w - a.x * yq.x - a.y * yq.y - a.z * yq.z};
                } else {
                    p->pose.orientation = q->pose.orientation;
                    p->pose.position = {q->pose.position.x + ax[0] * ox + ay[0] * oy, q->pose.position.y + ax[1] * ox + ay[1] * oy,
                                        q->pose.position.z + ax[2] * ox + ay[2] * oy};
                }
                p->placed = true;
            }
        } else if (!p->placed && app.anchor) placePanel(app, p);
        if (p->sc == XR_NULL_HANDLE) {
            Swapchain tmp;
            if (!createSwapchain(app, tmp, p->w, p->h, mipLevels(p->w, p->h))) continue;
            p->sc = tmp.handle; p->images = tmp.images; p->fbos = tmp.fbos; p->mips = tmp.mips;
        }
        // Si cambiaron los ajustes de imagen, se vuelve a copiar la última imagen aunque no haya una nueva
        bool filtered = !p->parent && !p->hud;
        bool redo = filtered && p->imgVer != g_imgVer.load() && !p->held.empty();
        AImage *img = nullptr;
        if (AImageReader_acquireLatestImage(p->reader, &img) != AMEDIA_OK || !img) {
            if (!redo) continue;
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, app.uiTex);
            app.imageTarget(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)p->held.back().egl);
        } else {
            AHardwareBuffer *hb = nullptr;
            if (AImage_getHardwareBuffer(img, &hb) != AMEDIA_OK || !hb) { AImage_delete(img); continue; }
            const EGLint attr[] = { EGL_IMAGE_PRESERVED_KHR, EGL_TRUE, EGL_NONE };
            EGLImageKHR egl = app.createImage(app.display, EGL_NO_CONTEXT, EGL_NATIVE_BUFFER_ANDROID, app.getClientBuffer(hb), attr);
            if (egl == EGL_NO_IMAGE_KHR) { AImage_delete(img); continue; }
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, app.uiTex);
            app.imageTarget(GL_TEXTURE_EXTERNAL_OES, (GLeglImageOES)egl);
            p->held.push_back({img, egl});
            while (p->held.size() > 2) {
                app.destroyImage(app.display, p->held.front().egl);
                AImage_delete(p->held.front().img);
                p->held.pop_front();
            }
        }
        p->imgVer = g_imgVer.load();
        uint32_t si = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(p->sc, &ai, &si))) continue;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(p->sc, &wi);
        glBindFramebuffer(GL_FRAMEBUFFER, p->fbos[si]);
        glViewport(0, 0, p->w, p->h);
        glUseProgram(app.prog);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_EXTERNAL_OES, app.uiTex);
        glUniform1i(app.uTex, 0);
        glUniform1f(app.uOff, 0.0f);
        glUniform1f(app.uSx, 1.0f);
        glUniform1f(app.uSy, 1.0f);
        glUniform1f(app.uSrgb, app.srgb ? 1.0f : 0.0f);
        glUniform1f(app.uKeepA, 1.0f);   // las barras de botones tienen partes transparentes
        {
            // Filtros solo sobre la imagen de la Mac: en la TV, el escritorio (si se ve); en una ventana, su video
            float rect[4] = {0, 0, 1, 1};
            bool on = filtered;
            if (p->tv) {
                std::lock_guard<std::mutex> lk(g_depthLock);
                for (int k = 0; k < 4; k++) rect[k] = g_deskRect[k];
                on = on && g_deskVisible;
            } else for (int k = 0; k < 4; k++) rect[k] = p->vrect[k];
            setFilter(app, on, rect, 1.f / p->w, 1.f / p->h);
        }
        glBindVertexArray(app.vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        if (p->mips > 1) {
            glBindTexture(GL_TEXTURE_2D, p->images[si].image);
            glGenerateMipmap(GL_TEXTURE_2D);
            glBindTexture(GL_TEXTURE_2D, 0);
            // Las luces detrás de la TV siguen los bordes de su imagen
            if (p->tv && ++app.ledTick % 4 == 0) updateLed(app, p->images[si].image, p->w, p->h, p->mips);
        }
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(p->sc, &ri);
        p->ready = true;
        p->lastImg = (int)si;
        p->fresh = true;
    }
}

/// Las barras de botones aparecen solo mientras apuntás a su ventana (o a ellas), como en visionOS
/// Se ve durante un juego: el menú del juego, las ventanas abiertas desde él y sus barras de botones
static bool panelInGame(const UiPanel *p) {
    if (p->inGame) return true;
    if (p->parent) for (UiPanel *q : g_ui) if (q->id == p->parent) return q->inGame;
    return false;
}

static bool hiddenByLoading(const UiPanel *p) {
    const UiPanel *top = p;
    if (p->parent) for (UiPanel *q : g_ui) if (q->id == p->parent) top = q;
    if (top->id == LOADING_ID) return !g_loadingNow;
    return g_loadingNow && !top->hud;   // (la pantalla de carga es solo eso: ni menús ni ventanas)
}

static bool panelShown(const UiPanel *p) {
    if (!p->parent) return true;
    for (UiPanel *q : g_ui) if (q->id == p->parent) return nowMs() < q->hotUntil;
    return false;
}
static void markHot(int id) {
    for (UiPanel *p : g_ui) if (p->id == id) {
        int target = p->parent ? p->parent : p->id;
        for (UiPanel *q : g_ui) if (q->id == target) q->hotUntil = nowMs() + 1800;
    }
}

/// Dónde toca un rayo a un panel: 0 nada, 1 el panel (u,v), 2 la barrita de abajo
static int hitUi(const UiPanel *p, const float o[3], const float d[3], float &t, float &u, float &v) {
    const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0}, ez[3] = {0, 0, 1};
    float ax[3], ay[3], n[3];
    rotate(p->pose.orientation, ex, ax); rotate(p->pose.orientation, ey, ay); rotate(p->pose.orientation, ez, n);
    float c[3] = {p->pose.position.x, p->pose.position.y, p->pose.position.z};
    // (el menú del reloj, mientras sigue al reloj, no tiene barrita: se mueve con el reloj)
    float barY = p->id == WRIST_MENU_ID && g_wristFollow ? -1000.f : -(p->hm / 2 + 0.05f);
    if (p->curved && p->radius > 0.3f) {
        // Ventana curva: parte de un cilindro vertical; vos estás cerca del centro
        float R = p->radius;
        float cc[3] = {c[0] + n[0] * R, c[1] + n[1] * R, c[2] + n[2] * R};
        float oc[3] = {o[0] - cc[0], o[1] - cc[1], o[2] - cc[2]};
        float ox = dot3(oc, ax), oy = dot3(oc, ay), oz = dot3(oc, n);
        float dx = dot3(d, ax), dy = dot3(d, ay), dz = dot3(d, n);
        float A = dx * dx + dz * dz, B = 2 * (ox * dx + oz * dz), C = ox * ox + oz * oz - R * R;
        float disc = B * B - 4 * A * C;
        if (A < 1e-6f || disc < 0) return 0;
        t = (-B + sqrtf(disc)) / (2 * A);
        if (t <= 0) return 0;
        float hx = ox + dx * t, hy = oy + dy * t, hz = oz + dz * t;
        float th = atan2f(hx, -hz);
        float arc = th * R;
        u = arc / p->wm + 0.5f; v = 0.5f - hy / p->hm;
        if (fabsf(arc) <= p->wm / 2 && fabsf(hy) <= p->hm / 2) return 1;
        if (fabsf(arc) <= 0.16f && fabsf(hy - barY) <= 0.03f) return 2;
        return 0;
    }
    float den = dot3(d, n);
    if (fabsf(den) < 1e-4f) return 0;
    float co[3] = {c[0] - o[0], c[1] - o[1], c[2] - o[2]};
    t = dot3(co, n) / den;
    if (t <= 0) return 0;
    float q[3] = {o[0] + d[0] * t - c[0], o[1] + d[1] * t - c[1], o[2] + d[2] * t - c[2]};
    float lx = dot3(q, ax), ly = dot3(q, ay);
    u = lx / p->wm + 0.5f; v = 0.5f - ly / p->hm;
    if (fabsf(lx) <= p->wm / 2 && fabsf(ly) <= p->hm / 2) return 1;
    if (fabsf(lx) <= 0.16f && fabsf(ly - barY) <= 0.03f) return 2;
    return 0;
}

/// Rayos de los controles: puntero sobre la interfaz (hover, clic, arrastre, rueda) y mover ventanas por la barrita
/// Reloj: si mirás la muñeca izquierda (el reloj de frente y cerca del centro de la vista) durante 0,7 s,
/// se abre o se cierra el menú. Hay que dejar de mirarlo para que vuelva a funcionar.
static void watchUpdate(int activeHand, XrTime time, const XrView &head, const Ray *rays, int nr) {
    static XrTime last = 0;
    static float looked = 0, away = 1;
    static bool armed = true;
    float dt = last ? std::min(0.1f, (time - last) * 1e-9f) : 0;
    last = time;
    const Ray *L = nullptr;
    for (int i = 0; i < nr; i++) if (rays[i].hand >= 0 && rays[i].hand != activeHand) L = &rays[i];   // la mano sin láser
    float wc[3] = {0, 0, 0};
    bool look = false;
    if (g_watchOn && L) {
        float R[9]; quatToMat(L->pose.orientation, R);
        float c[3], n[3];
        for (int r = 0; r < 3; r++) {
            const float *C = g_watchPos[L->hand], *Nn = g_watchNrm[L->hand];
            c[r] = (&L->pose.position.x)[r] + R[r] * C[0] + R[3 + r] * C[1] + R[6 + r] * C[2];
            n[r] = R[r] * Nn[0] + R[3 + r] * Nn[1] + R[6 + r] * Nn[2];
        }
        for (int r = 0; r < 3; r++) { wc[r] = c[r]; g_watchNow[r] = c[r]; }
        g_watchNowOk = true;
        float e[3] = {head.pose.position.x, head.pose.position.y, head.pose.position.z};
        float toEye[3] = {e[0] - c[0], e[1] - c[1], e[2] - c[2]};
        float dist = sqrtf(dot3(toEye, toEye));
        if (dist > 0.05f && dist < 0.65f) {
            for (float &k : toEye) k /= dist;
            const float fw0[3] = {0, 0, -1};
            float fw[3];
            rotate(head.pose.orientation, fw0, fw);
            float facing = dot3(n, toEye), centered = -dot3(fw, toEye);
            look = facing > 0.65f && centered > 0.9f;
        }
    }
    if (look) { away = 0; if (armed) looked += dt; }
    else { looked = std::max(0.f, looked - dt * 2); away += dt; if (away > 0.35f) armed = true; }
    const float need = 0.7f;
    if (armed && looked >= need) {
        armed = false; looked = 0;
        for (int r = 0; r < 3; r++) g_watchWorld[r] = wc[r];   // el menú aparece encima de donde estaba el reloj
        javaWatch();
    }
    g_watchProg = armed ? looked / need : 0;
}

/// Mirar el borde de la mano (del lado del meñique) durante 0,6 s abre o cierra el menú, que flota encima de esa mano
/// y la sigue. Mientras lo mirás aparece un puntito que se llena. Hay que dejar de mirarlo para que vuelva a funcionar.
static float g_palmBestFacing = -2, g_palmBestCentered = -2;   // (para el registro: lo mejor que se logró)
static int g_palmOpens = 0;
static void palmMenuUpdate(App &app, XrTime time, const XrView &head) {
    static XrTime last = 0;
    static float looked = 0, away = 1;
    static bool armed = true;
    static int menuHand = -1;   // la mano que abrió el menú (el menú la sigue)
    float dt = last ? std::min(0.1f, (time - last) * 1e-9f) : 0;
    last = time;
    float e[3] = {head.pose.position.x, head.pose.position.y, head.pose.position.z};
    const float fw0[3] = {0, 0, -1};
    float fw[3];
    rotate(head.pose.orientation, fw0, fw);
    int lookHand = -1;
    float bestScore = -10;
    for (int s = 0; s < 2 && g_watchOn; s++) {
        const RealHands::Hand &H = g_rh.h[s];
        if (!H.active || !H.edgeOk || H.pinch > 0.6f) continue;
        float toEye[3] = {e[0] - H.edge[0], e[1] - H.edge[1], e[2] - H.edge[2]};
        float dist = sqrtf(dot3(toEye, toEye));
        if (dist < 0.08f || dist > 0.75f) continue;
        for (float &k : toEye) k /= dist;
        float facingEye = dot3(H.edgeDir, toEye), centered = -dot3(fw, toEye);
        if (centered > 0.75f) g_palmBestFacing = std::max(g_palmBestFacing, facingEye);
        if (facingEye > 0.2f) g_palmBestCentered = std::max(g_palmBestCentered, centered);
        // el borde del meñique hacia vos (de costado o con la palma un poco hacia arriba) y cerca del centro de la vista
        // la palma de costado (ni hacia arriba ni hacia abajo): con la derecha, palma hacia la izquierda
        bool sideways = fabsf(H.palmN[1]) < 0.55f;
        if (facingEye > 0.35f && centered > 0.82f && sideways && facingEye + centered > bestScore) { bestScore = facingEye + centered; lookHand = s; }
    }
    // el menú sigue a la mano que lo abrió (mientras se la vea)
    if (menuHand >= 0 && g_rh.h[menuHand].active && g_rh.h[menuHand].edgeOk) {
        for (int k = 0; k < 3; k++) g_watchNow[k] = g_rh.h[menuHand].edge[k];
        g_watchNowOk = true;
    } else g_watchNowOk = false;
    if (lookHand >= 0) { away = 0; if (armed) looked += dt; for (int k = 0; k < 3; k++) g_palmDot[k] = g_rh.h[lookHand].edge[k]; }
    else { looked = std::max(0.f, looked - dt * 2); away += dt; if (away > 0.35f) armed = true; }
    const float need = 0.6f;
    if (armed && looked >= need && lookHand >= 0) {
        armed = false; looked = 0;
        menuHand = lookHand;
        for (int k = 0; k < 3; k++) { g_watchWorld[k] = g_rh.h[lookHand].edge[k]; g_watchNow[k] = g_watchWorld[k]; }
        g_palmOpens++;
        // el láser pasa a la otra mano (para elegir en el menú con ella)
        int other = 1 - lookHand;
        if (app.activeHand != other) {
            App::HandPtr &old = app.hp[app.activeHand];
            if (old.down >= 0) { javaPointer(old.down, 3, 0, 0, 0); old.down = -1; }
            old.grab = -1;
            app.activeHand = other;
        }
        javaWatch();
    }
    g_watchProg = armed ? looked / need : 0;
}

static int uiInput(App &app, XrTime time, const XrView &head, Ray rays[2], std::vector<Bar> &bars) {
    std::lock_guard<std::mutex> lk(g_uiLock);
    int nr = 0;
    int active = -1;          // mano que maneja el puntero de la interfaz
    int hitPanel[2] = {-1, -1}, hitKind[2] = {0, 0};
    float hu[2] = {0, 0}, hv[2] = {0, 0};
    float hpos[3] = {head.pose.position.x, head.pose.position.y, head.pose.position.z};
    for (int h = 1; h >= 0; h--) {   // la derecha tiene prioridad
        XrSpaceLocation a{XR_TYPE_SPACE_LOCATION};
        // Con el control: su rayo. Sin control (seguimiento de manos): el rayo de la mano, y pellizcar es hacer clic
        bool ctl = XR_SUCCEEDED(xrLocateSpace(app.aimSpace[h], app.space, time, &a)) &&
                   (a.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) && (a.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT);
        bool byHand = !ctl && !g_ctlInUse && g_rh.h[h].aimValid;
        if (!ctl && !byHand) continue;
        if (byHand) a.pose = g_rh.h[h].aim;
        Ray &r = rays[nr++];
        r.o[0] = a.pose.position.x; r.o[1] = a.pose.position.y; r.o[2] = a.pose.position.z;
        const float fw[3] = {0, 0, -1};
        rotate(a.pose.orientation, fw, r.d);
        r.len = 2.5f; r.hit = false;
        r.hand = h; r.pose = a.pose;
        App::HandPtr &hp = app.hp[h];
        float trigV = byHand ? g_rh.h[h].pinch : getFloat(app, app.aTrigger, h);
        r.trig = trigV;
        r.squeeze = getFloat(app, app.aSqueeze, h);
        r.trigTouch = getBool(app, app.aTrigTouch, h);
        r.thumbTouch = getBool(app, app.aStickTouch, h) || getBool(app, app.aATouch, h) || getBool(app, app.aBTouch, h);
        bool pressed = !hp.trig && trigV > 0.7f, released = hp.trig && trigV < 0.3f;
        if (pressed) hp.trig = true;
        if (released) hp.trig = false;
        XrVector2f stick = byHand ? XrVector2f{0, 0} : getVec2(app, app.aStick, h);

        // Un solo láser: el gatillo de la otra mano se lo pasa a esa mano (ese apretón no hace clic)
        if (h != app.activeHand) {
            if (pressed) {
                App::HandPtr &old = app.hp[app.activeHand];
                if (old.down >= 0) { javaPointer(old.down, 3, 0, 0, 0); old.down = -1; }
                old.grab = -1;
                app.activeHand = h;
            }
            continue;
        }

        // Moviendo una ventana: sigue al rayo (el joystick la acerca o la aleja)
        if (hp.grab >= 0) {
            UiPanel *g = nullptr;
            for (UiPanel *p : g_ui) if (p->id == hp.grab && !p->dead) g = p;
            if (!g || released) { hp.grab = -1; }
            else {
                hp.grabT = std::min(6.f, std::max(0.6f, hp.grabT * (1.f + stick.y * 0.02f)));
                float pos[3] = {r.o[0] + r.d[0] * hp.grabT + hp.off[0], r.o[1] + r.d[1] * hp.grabT + hp.off[1], r.o[2] + r.d[2] * hp.grabT + hp.off[2]};
                g->pose.position = {pos[0], pos[1], pos[2]};
                g->pose.orientation = facingTilt(pos, hpos);
                r.len = hp.grabT; r.hit = true;
                hitPanel[h] = g->id; hitKind[h] = 2;
                continue;
            }
        }
        // Panel más cercano que toca el rayo
        float best = 1e9f, noTouchT = 1e9f;
        for (UiPanel *p : g_ui) {
            if (p->dead || p->closeAt > 0 || !p->ready || !p->placed || !panelShown(p) || p->hud) continue;   // (cerrándose: ya no se toca)
            if (g_inGameNow && !panelInGame(p)) continue;
            if (hiddenByLoading(p)) continue;
            // (el menú de VRmin abierto sobre la pantalla principal es un menú: ahí el láser siempre toca)
            const bool menuOnTv = p->tv && !g_deskVisible;
            if (g_laserNoTouch && p->id != WRIST_MENU_ID && p->id != GAME_MENU_ID && p->id != KEYBOARD_ID && !menuOnTv) continue;   // solo el menú
            float t, u, v;
            int k = hitUi(p, r.o, r.d, t, u, v);
            // (la pantalla fija y una pantalla sin toque igual muestran sus botones al apuntarles: así se puede volver a activar el toque)
            if (k == 2 && !p->parent && p->tv && fixedScreenEnv()) markHot(p->id);
            if (k == 2 && (p->parent || (p->tv && fixedScreenEnv()))) k = 0;   // las barras de botones y la pantalla principal fija no se mueven
            if (k == 1 && p->noTouch && !menuOnTv) { if (t < noTouchT) noTouchT = t; k = 0; markHot(p->id); }   // el láser no toca esta pantalla (se puede mover igual, y su barra se usa)
            if (k && t < best) { best = t; hitPanel[h] = p->id; hitKind[h] = k; hu[h] = u; hv[h] = v; }
        }
        if (hitPanel[h] >= 0) { r.len = best; r.hit = hitKind[h] == 1; markHot(hitPanel[h]); }
        // Apuntando a una pantalla que el láser no toca (y no a algo delante, como su barra o un menú): el láser no se ve
        // (un menú o una barra pegados a esa pantalla, apenas detrás, igual cuentan como «delante»)
        r.hidden = noTouchT < best - 0.05f && hp.down < 0;
        if (r.hidden && hp.grab < 0) { hitPanel[h] = -1; hitKind[h] = 0; r.hit = false; }   // (lo de atrás no se toca a través de esa pantalla)
        // Portal: dibujando el círculo (pellizco o gatillo fuera de las ventanas)
        if (hp.down < 0 && portalInput(h, byHand, r.o, pressed, released, hitPanel[h] >= 0 || noTouchT < 1e8f)) { hp.hover = -1; continue; }
        // Agarrar la barrita para mover
        if (pressed && hitKind[h] == 2 && hp.down < 0) {
            for (UiPanel *p : g_ui) if (p->id == hitPanel[h]) {
                hp.grab = p->id; hp.grabT = best;
                hp.off[0] = p->pose.position.x - (r.o[0] + r.d[0] * best);
                hp.off[1] = p->pose.position.y - (r.o[1] + r.d[1] * best);
                hp.off[2] = p->pose.position.z - (r.o[2] + r.d[2] * best);
            }
            continue;
        }
        if (active < 0 && (hp.down >= 0 || hitKind[h] == 1)) active = h;
        // Clic / arrastre / soltar (el panel donde empezó el clic recibe todo hasta soltar)
        if (hp.down >= 0) {
            UiPanel *d = nullptr;
            for (UiPanel *p : g_ui) if (p->id == hp.down) d = p;
            float t, u = hu[h], v = hv[h];
            if (d) hitUi(d, r.o, r.d, t, u, v);
            // (en las barras de botones no hay límite: mantener tamaño o curva y pasarse del borde sigue ajustando)
            if (!d || !d->parent) { u = std::min(1.f, std::max(0.f, u)); v = std::min(1.f, std::max(0.f, v)); }
            else { u = std::min(6.f, std::max(-5.f, u)); v = std::min(6.f, std::max(-5.f, v)); }
            if (released || !d) { javaPointer(hp.down, 3, u, v, 0); hp.down = -1; }
            else { javaPointer(hp.down, 2, u, v, 0); markHot(hp.down); }   // mientras se arrastra, su barra no se esconde
        } else if (pressed && hitKind[h] == 1) {
            hp.down = hitPanel[h];
            javaPointer(hp.down, 1, hu[h], hv[h], 0);
        }
        hp.hover = hitKind[h] == 1 ? hitPanel[h] : -1;
        // Rueda: joystick arriba/abajo sobre un panel
        if (hitKind[h] == 1 && fabsf(stick.y) > 0.4f && hp.down < 0 && ++hp.tick % 4 == 0)
            javaPointer(hitPanel[h], 4, hu[h], hv[h], stick.y > 0 ? 1.f : -1.f);
    }
    // Hover de la mano con el láser (y avisar al panel anterior que el puntero se fue)
    int hov = active >= 0 && app.hp[active].down < 0 && hitKind[active] == 1 ? hitPanel[active] : -1;
    if (app.lastHover >= 0 && app.lastHover != hov) javaPointer(app.lastHover, 5, 0, 0, 0);
    if (hov >= 0) javaPointer(hov, 0, hu[active], hv[active], 0);
    app.lastHover = hov;
    // Barritas de las ventanas
    static double lastBarT = 0;
    double barNow = nowMs();
    float barDt = lastBarT > 0 ? std::min(0.1f, (float)(barNow - lastBarT) / 1000.f) : 0.f;
    lastBarT = barNow;
    for (UiPanel *p : g_ui) {
        if (p->dead || !p->ready || !p->placed || p->parent || p->hud || (p->tv && fixedScreenEnv())) continue;
        if (p->id == WRIST_MENU_ID && g_wristFollow) continue;
        if (g_inGameNow && (!p->inGame || p->id == GAME_MENU_ID)) continue;
        if (hiddenByLoading(p) || p->id == LOADING_ID) continue;   // (la pantalla de carga no se mueve)
        Bar b;
        const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0};
        rotate(p->pose.orientation, ex, b.ax); rotate(p->pose.orientation, ey, b.ay);
        float barY = -(p->hm / 2 + 0.05f);
        for (int k = 0; k < 3; k++) b.c[k] = (&p->pose.position.x)[k] + b.ay[k] * barY;
        b.hot = (hitPanel[0] == p->id && hitKind[0] == 2) || (hitPanel[1] == p->id && hitKind[1] == 2);
        bool grabbed = app.hp[0].grab == p->id || app.hp[1].grab == p->id;
        // Más chica cuando no la usás; crece un poco al apuntarle y se estira al agarrarla (con un rebote suave)
        float tgt = grabbed ? 1.f : b.hot ? 0.5f : 0.f;
        p->barK += (tgt - p->barK) * std::min(1.f, barDt * 14.f);
        p->grabK += ((grabbed ? 1.f : 0.f) - p->grabK) * std::min(1.f, barDt * 10.f);
        float wob = grabbed ? 0.012f * sinf((float)(nowMs() * 0.012)) : 0.f;   // late apenas mientras la movés
        b.hw = 0.055f + 0.045f * p->barK + wob; b.hh = 0.0055f + 0.0035f * p->barK;
        b.a = 0.32f + 0.6f * p->barK;
        if (p->closeAt > 0) b.a *= std::max(0.f, 1.f - (float)(nowMs() - p->closeAt) / 150.f);
        if (p->openAt > 0 && nowMs() - p->openAt < 320) b.a *= (float)(nowMs() - p->openAt) / 320.f;
        bars.push_back(b);
    }
    if (!g_inGameNow) palmMenuUpdate(app, time, head);
    if (false) {   // (manos dibujadas y reloj: reemplazados por las manos reales)
        watchUpdate(app.activeHand, time, head, rays, nr);
        static XrTime lastT = 0;
        float dt = lastT ? std::min(0.1f, (time - lastT) * 1e-9f) : 0.016f;
        lastT = time;
        HandTouch in[2] = {};
        float press[2] = {0, 0};
        for (int i = 0; i < nr; i++) {
            int hh = rays[i].hand;
            if (hh < 0 || hh > 1) continue;
            in[hh] = {true, rays[i].trig, rays[i].squeeze, rays[i].trigTouch, rays[i].thumbTouch};
            press[hh] = std::min(1.f, std::max(0.f, (rays[i].trig - 0.05f) / 0.6f));
        }
        handsUpdate(app.activeHand, g_handModel.load(), in, dt, press);
    }
    return nr;
}

// ---------------------------------------------------------------------------
// 3D automático: cada ojo ve la imagen de la TV corrida según la profundidad (lo cercano "sale" de la pantalla)
// ---------------------------------------------------------------------------
static void init3D(App &app) {
    const char *vs = R"(#version 300 es
out vec2 uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
    uv = vec2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);   // 0,0 = arriba a la izquierda
    gl_Position = vec4(p, 0.0, 1.0);
})";
    const char *fs = R"(#version 300 es
precision highp float;
uniform sampler2D src;
uniform sampler2D dep;
uniform vec4 rect;       // dónde está el escritorio dentro de la TV (arriba a la izquierda = 0,0)
uniform float eyeSign;   // +1 ojo izquierdo, -1 ojo derecho
uniform float strength;
uniform int mode;        // 1 IA (profundidad), 2 lado a lado (SBS), 3 arriba/abajo
uniform vec4 crop;       // solo el video (sin barras negras), en la imagen de un ojo
uniform float fill;      // 1: toda la imagen es ese pedazo (después se agranda la capa)
in vec2 uv;
out vec4 color;
void main() {
    vec2 tex = vec2(uv.x, 1.0 - uv.y);
    if (fill > 0.5 && mode >= 2) {
        vec2 sz = rect.zw - rect.xy;
        vec2 d = crop.xy + uv * (crop.zw - crop.xy);
        float hs = eyeSign > 0.0 ? 0.0 : 0.5;
        if (mode == 2) d.x = d.x * 0.5 + hs; else d.y = d.y * 0.5 + hs;
        vec2 q = rect.xy + d * sz;
        color = texture(src, vec2(q.x, 1.0 - q.y));
        return;
    }
    if (uv.x > rect.x && uv.x < rect.z && uv.y > rect.y && uv.y < rect.w) {
        vec2 sz = rect.zw - rect.xy;
        vec2 d = (uv - rect.xy) / sz;
        if (mode == 1) {
            float s = 0.0;
            for (int i = 0; i < 3; i++) {
                float z = texture(dep, vec2(d.x - s, d.y)).r;   // 1 = cerca
                s = eyeSign * strength * (z - 0.3);
            }
            tex = vec2(clamp(uv.x - s * sz.x, rect.x, rect.z), 1.0 - uv.y);
        } else {
            // 3D real: cada ojo ve su mitad (izquierda/derecha, o arriba/abajo) estirada a toda la pantalla
            float hs = eyeSign > 0.0 ? 0.0 : 0.5;
            if (mode == 2) d.x = d.x * 0.5 + hs; else d.y = d.y * 0.5 + hs;
            vec2 q = rect.xy + d * sz;
            tex = vec2(q.x, 1.0 - q.y);
        }
    }
    color = texture(src, tex);
})";
    app.prog3d = glCreateProgram();
    glAttachShader(app.prog3d, compileShader(GL_VERTEX_SHADER, vs));
    glAttachShader(app.prog3d, compileShader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(app.prog3d);
    app.u3Src = glGetUniformLocation(app.prog3d, "src");
    app.u3Dep = glGetUniformLocation(app.prog3d, "dep");
    app.u3Rect = glGetUniformLocation(app.prog3d, "rect");
    app.u3Eye = glGetUniformLocation(app.prog3d, "eyeSign");
    app.u3Str = glGetUniformLocation(app.prog3d, "strength");
    app.u3Mode = glGetUniformLocation(app.prog3d, "mode");
    app.u3Crop = glGetUniformLocation(app.prog3d, "crop");
    app.u3Fill = glGetUniformLocation(app.prog3d, "fill");
}

/// Barras negras del video (SBS o arriba/abajo): mira una copia chiquita de la imagen y busca, en la mitad de un ojo,
/// las filas y columnas del borde que son casi negras. Deja en p->crop lo que es video.
static void detectBars(UiPanel *p, GLuint src, const float rect[4], int mode) {
    static GLuint fbo = 0;
    if (!fbo) glGenFramebuffers(1, &fbo);
    int lvl = 0, w = p->w, h = p->h;
    while (lvl + 1 < p->mips && (w > 160 || h > 160)) { lvl++; w = std::max(1, w / 2); h = std::max(1, h / 2); }
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, src, lvl);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) { glBindFramebuffer(GL_FRAMEBUFFER, 0); return; }
    static std::vector<uint8_t> px;
    px.resize((size_t)w * h * 4);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    // la mitad del ojo izquierdo, en coordenadas del video (arriba = 0)
    float ex0 = 0, ex1 = mode == 2 ? 0.5f : 1.f, ey0 = 0, ey1 = mode == 3 ? 0.5f : 1.f;
    const int N = 48;
    auto lum = [&](float dx, float dy) {   // dx, dy: 0..1 dentro de la mitad del ojo
        float u = rect[0] + (ex0 + dx * (ex1 - ex0)) * (rect[2] - rect[0]);
        float v = rect[1] + (ey0 + dy * (ey1 - ey0)) * (rect[3] - rect[1]);
        int x = std::min(w - 1, std::max(0, (int)(u * w))), y = std::min(h - 1, std::max(0, (int)((1.f - v) * h)));
        const uint8_t *q = &px[((size_t)y * w + x) * 4];
        return (q[0] * 3 + q[1] * 6 + q[2]) / 10;
    };
    auto dark = [&](bool row, float at) {   // ¿esa fila (o columna) es casi toda negra?
        int n = 0;
        for (int i = 0; i < N; i++) { float t = (i + 0.5f) / N; if ((row ? lum(t, at) : lum(at, t)) > 22) n++; }
        return n <= 1;
    };
    float c[4] = {0, 0, 1, 1};
    const int S = 64;
    for (int i = 0; i < S / 2 && dark(true, (i + 0.5f) / S); i++) c[1] = (i + 1.f) / S;
    for (int i = 0; i < S / 2 && dark(true, 1.f - (i + 0.5f) / S); i++) c[3] = 1.f - (i + 1.f) / S;
    for (int i = 0; i < S / 2 && dark(false, (i + 0.5f) / S); i++) c[0] = (i + 1.f) / S;
    for (int i = 0; i < S / 2 && dark(false, 1.f - (i + 0.5f) / S); i++) c[2] = 1.f - (i + 1.f) / S;
    if (c[2] - c[0] < 0.3f || c[3] - c[1] < 0.3f) { c[0] = c[1] = 0; c[2] = c[3] = 1; }   // (una escena oscura no es una barra)
    // cambia solo si se movió de verdad (que no tiemble)
    float diff = 0;
    for (int k = 0; k < 4; k++) diff = std::max(diff, fabsf(c[k] - p->crop[k]));
    if (diff > 0.02f) for (int k = 0; k < 4; k++) p->crop[k] = c[k];
}

/// Arma la imagen de cada ojo para la TV o una ventana. Devuelve true si se muestra en 3D en este cuadro.
/// La TV usa el 3D general del escritorio (solo con el escritorio a la vista); cada ventana, el suyo.
static bool update3D(App &app, UiPanel *p) {
    if (!p || p->parent || !p->ready || p->lastImg < 0 || !app.prog3d) return false;
    int mode = p->tv ? (g_deskVisible ? g_3dMode.load() : 0) : p->mode3d;
    if (mode <= 0 || p->hud) return false;
    float str = p->tv ? g_3dStrength.load() : p->str3d;
    uint32_t key = p->tv ? 0u : (uint32_t)p->id;
    bool depthNew = false;
    float rect[4];
    {
        std::lock_guard<std::mutex> lk(g_depthLock);
        if (p->tv) for (int k = 0; k < 4; k++) rect[k] = g_deskRect[k];
        else for (int k = 0; k < 4; k++) rect[k] = p->vrect[k];
        auto it = g_depth.find(key);
        bool needDepth = mode == 1;   // SBS y arriba/abajo no necesitan la IA
        if (needDepth && (it == g_depth.end() || nowMs() - it->second.at > 2000)) return false;   // la Mac dejó de mandar profundidad
        if (needDepth && it->second.at != p->depthSeenAt && it->second.w > 0) {
            DepthBuf &d = it->second;
            if (!p->depthTex) {
                glGenTextures(1, &p->depthTex);
                glBindTexture(GL_TEXTURE_2D, p->depthTex);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            }
            glBindTexture(GL_TEXTURE_2D, p->depthTex);
            glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, d.w, d.h, 0, GL_RED, GL_UNSIGNED_BYTE, d.px.data());
            glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
            glBindTexture(GL_TEXTURE_2D, 0);
            p->depthSeenAt = d.at;
            depthNew = true;
            p->depthReady = true;
        }
    }
    if (mode == 1 && !p->depthReady) return false;
    if (str != p->lastStr) { p->lastStr = str; depthNew = true; }   // se movió el control de intensidad
    if (mode != p->lastMode) { p->lastMode = mode; depthNew = true; }
    for (int e = 0; e < 2; e++) {
        if (p->sc3[e] != XR_NULL_HANDLE) continue;
        Swapchain tmp;
        if (!createSwapchain(app, tmp, p->w, p->h, 1)) return false;
        p->sc3[e] = tmp.handle; p->images3[e] = tmp.images; p->fbos3[e] = tmp.fbos;
        depthNew = true;
    }
    // 3D real: sin barras negras y ampliado (la capa de cada ojo es solo el video, y puede pasarse de los bordes)
    p->fillOn = mode >= 2 && !p->hud;
    if (p->fillOn) {
        const float *c = p->crop;
        float sw = rect[2] - rect[0], sh = rect[3] - rect[1];
        float cw = p->wm * sw * (c[2] - c[0]), ch = p->hm * sh * (c[3] - c[1]);
        float ux = rect[0] + sw * (c[0] + c[2]) / 2, uy = rect[1] + sh * (c[1] + c[3]) / 2;
        p->fillSize[0] = cw * p->zoom; p->fillSize[1] = ch * p->zoom;
        p->fillOff[0] = (ux - 0.5f) * p->wm; p->fillOff[1] = (0.5f - uy) * p->hm;
    }
    if (!p->fresh && !depthNew) return true;   // no cambió nada: se reusan las imágenes de antes
    p->fresh = false;
    GLuint src = p->images[p->lastImg].image;
    if (p->fillOn && nowMs() - p->cropAt > 800) { p->cropAt = nowMs(); detectBars(p, src, rect, mode); }
    for (int e = 0; e < 2; e++) {
        uint32_t si = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(p->sc3[e], &ai, &si))) return false;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(p->sc3[e], &wi);
        glBindFramebuffer(GL_FRAMEBUFFER, p->fbos3[e][si]);
        glViewport(0, 0, p->w, p->h);
        glUseProgram(app.prog3d);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, src);
        glUniform1i(app.u3Src, 0);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, p->depthTex ? p->depthTex : src);
        glUniform1i(app.u3Dep, 1);
        glUniform1i(app.u3Mode, mode);
        glActiveTexture(GL_TEXTURE0);
        glUniform4f(app.u3Rect, rect[0], rect[1], rect[2], rect[3]);
        glUniform1f(app.u3Eye, e == 0 ? 1.f : -1.f);
        glUniform1f(app.u3Str, str);
        glUniform4f(app.u3Crop, p->crop[0], p->crop[1], p->crop[2], p->crop[3]);
        glUniform1f(app.u3Fill, p->fillOn ? 1.f : 0.f);
        glBindVertexArray(app.vao);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(p->sc3[e], &ri);
    }
    return true;
}

/// Altura del piso del ambiente en un punto (en el cine, cada fila de butacas está a otra altura)
static float floorAt(App &app, float x, float z) {
    if (g_env != 2) return app.worldY;
    float qx = x - app.anc[0], qz = z - app.anc[2];
    float lz = qx * app.fwd[0] + qz * app.fwd[2];
    int row = (int)lroundf(lz / 1.1f);
    row = std::max(-4, std::min(6, row));
    return app.worldY - 0.28f * row;
}

/// ¿Ese punto del piso está dentro del lugar? (living: entre las paredes; espacio: la plataforma; cine: la sala)
static bool insideRoom(const App &app, float x, float z) {
    float q[3] = {x - app.anc[0], 0, z - app.anc[2]};
    float lx = q[0] * -app.fwd[2] + q[2] * app.fwd[0], lz = q[0] * app.fwd[0] + q[2] * app.fwd[2];
    return g_env == 4 ? sqrtf(lx * lx + lz * lz) < 20.0f
         : g_env == 1 ? sqrtf(lx * lx + lz * lz) < 5.5f
         : g_env == 2 ? (fabsf(lx) < 6.0f && lz > -4.5f && lz < 6.5f)
         : (fabsf(lx) < 3.1f && lz > -2.5f && lz < 2.45f);
}

/// Teletransporte: con el joystick derecho hacia abajo se apunta al piso; al soltarlo, aparecés ahí
static bool teleportInput(App &app, XrTime time, const XrView &head) {
    if (g_loadingNow) { app.tpAim = false; return false; }   // en la pantalla de carga no te movés ni girás
    XrVector2f st = getVec2(app, app.aStick, 1);
    // Sin controles: el joystick virtual de la mano derecha (gestos de la sala)
    if (g_handMove && !g_ctlInUse && g_rh.h[1].active && g_rh.h[1].v.joy) st = {g_rh.h[1].v.sx, g_rh.h[1].v.sy};
    if (g_env == 3) st = {0, 0};   // passthrough: no te movés (ni girás ni te teletransportás): es tu cuarto real
    // Sin controles, con la mano derecha en «pistola» (índice estirado, los otros tres dedos cerrados):
    //  · apuntando hacia adelante sale el arco; bajá el pulgar («disparo») para ir ahí
    //  · apuntando a un costado (más de 40°), el pulgar gira la sala hacia ese lado
    const RealHands::Hand &RH = g_rh.h[1];
    const bool handPoint = g_handMove && g_env != 3 && !g_ctlInUse && RH.active && RH.gPoint && app.hp[1].down < 0 && app.hp[1].grab < 0;   // (aunque el láser esté sobre una pantalla)
    static bool shotPrev = false;
    const bool shot = handPoint && RH.gShot && !shotPrev;
    shotPrev = handPoint && RH.gShot;
    bool handAim = false;
    float turnA = 0;
    if (handPoint) {
        float m[9]; quatToMat(head.pose.orientation, m);
        float fx = -m[6], fz = -m[8], fl = sqrtf(fx * fx + fz * fz);
        float px = RH.gPointD[0], pz = RH.gPointD[2], pl = sqrtf(px * px + pz * pz);
        if (fl > 1e-3f && pl > 0.2f) {
            fx /= fl; fz /= fl; px /= pl; pz /= pl;
            float side = px * -fz + pz * fx;            // + = a la derecha de donde mirás
            float fwdDot = px * fx + pz * fz;
            float ang = atan2f(side, fwdDot);
            if (fabsf(ang) < 40.f * (float)M_PI / 180.f) handAim = RH.gPointD[1] < -0.25f || app.tpAim;   // hacia el piso (apuntar a una pantalla no saca el arco)
            else if (shot) turnA = (side > 0 ? 1.f : -1.f) * g_snapDeg.load() * (float)M_PI / 180.f;
        }
    }
    // «Traer todo al frente»: el ángulo entre hacia dónde mira la sala y hacia dónde mirás vos
    if (g_recenterIn > 0 && --g_recenterIn == 0) g_recenter = true;
    if (g_recenter.exchange(false)) {
        float m[9]; quatToMat(head.pose.orientation, m);
        float hx = -m[6], hz = -m[8];
        if (hx * hx + hz * hz > 1e-4f) turnA = atan2f(app.fwd[0] * hz - app.fwd[2] * hx, app.fwd[0] * hx + app.fwd[2] * hz);
        // (el signo: girar fwd hacia la mirada con la misma convención que el giro del joystick)
        turnA = -turnA;
    }
    static XrTime lastT = 0;
    float dt = lastT ? std::min(0.05f, (time - lastT) * 1e-9f) : 0.f;
    lastT = time;
    bool moving = false;
    const bool rightFree = app.hp[1].down < 0 && app.hp[1].grab < 0;
    // Girar con el joystick derecho a los costados (gira el mundo alrededor de tu cabeza): por saltos o suave
    static bool turned = false;
    if (g_turnMode == 1) {
        if (fabsf(st.x) > 0.25f && rightFree) {   // (también con el láser sobre una pantalla)
            float k = (fabsf(st.x) - 0.25f) / 0.75f;
            turnA += (st.x > 0 ? 1.f : -1.f) * k * g_turnSpeed.load() * (float)M_PI / 180.f * dt;
            moving = true;
        }
    } else if (fabsf(st.x) > 0.7f && !turned && rightFree) {
        turned = true;
        turnA = (st.x > 0 ? 1.f : -1.f) * g_snapDeg.load() * (float)M_PI / 180.f;
    }
    // Caminar con el joystick izquierdo (o el puño izquierdo con el pulgar encima), hacia donde mirás
    if (g_moveMode >= 1 && dt > 0 && g_env != 3) {
        XrVector2f mv = getVec2(app, app.aStick, 0);
        if (g_handMove && !g_ctlInUse && g_rh.h[0].active && g_rh.h[0].v.joy) mv = {g_rh.h[0].v.sx, g_rh.h[0].v.sy};   // (joystick virtual de la mano)
        float ml = sqrtf(mv.x * mv.x + mv.y * mv.y);
        if (ml > 0.2f && app.hp[0].down < 0 && app.hp[0].grab < 0) {   // (también con el láser sobre una pantalla)
            float m[9]; quatToMat(head.pose.orientation, m);
            float fx = -m[6], fz = -m[8], fl = sqrtf(fx * fx + fz * fz);
            if (fl > 1e-3f) {
                fx /= fl; fz /= fl;
                float rx = -fz, rz = fx;
                float k = std::min(1.f, (ml - 0.2f) / 0.8f) * g_moveSpeed.load() * dt / ml;
                float d[3] = {(fx * mv.y + rx * mv.x) * k, 0, (fz * mv.y + rz * mv.x) * k};
                if (insideRoom(app, head.pose.position.x + d[0], head.pose.position.z + d[2])) {
                    app.anc[0] -= d[0]; app.anc[2] -= d[2];
                    std::lock_guard<std::mutex> lk(g_uiLock);
                    for (UiPanel *p : g_ui) { p->pose.position.x -= d[0]; p->pose.position.z -= d[2]; }
                }
                moving = true;
            }
        }
    }
    // Viñeta: los bordes se oscurecen mientras te movés de forma continua (marea menos)
    {
        float target = (moving && g_vignette) ? 1.f : 0.f;
        g_vigAmt += (target - g_vigAmt) * std::min(1.f, dt * 8.f);
    }
    if (turnA != 0) {
        if (g_vignette && fabsf(turnA) > 0.1f) g_vigAmt = std::max(g_vigAmt, 0.85f);   // (giro por saltos: parpadeo suave)
        float a = turnA;
        float c = cosf(a), sn = sinf(a), hx = head.pose.position.x, hz = head.pose.position.z;
        auto rot = [&](float &x, float &z) { float dx = x - hx, dz = z - hz; x = hx + dx * c + dz * sn; z = hz - dx * sn + dz * c; };
        rot(app.anc[0], app.anc[2]);
        float fx = app.fwd[0], fz = app.fwd[2];
        app.fwd[0] = fx * c + fz * sn; app.fwd[2] = -fx * sn + fz * c;
        XrQuaternionf q{0, sinf(a / 2), 0, cosf(a / 2)};
        std::lock_guard<std::mutex> lk(g_uiLock);
        for (UiPanel *p : g_ui) {
            rot(p->pose.position.x, p->pose.position.z);
            const XrQuaternionf &o = p->pose.orientation;
            p->pose.orientation = {q.w * o.x + q.x * o.w + q.y * o.z - q.z * o.y, q.w * o.y - q.x * o.z + q.y * o.w + q.z * o.x,
                                   q.w * o.z + q.x * o.y - q.y * o.x + q.z * o.w, q.w * o.w - q.x * o.x - q.y * o.y - q.z * o.z};
        }
    }
    if (fabsf(st.x) < 0.3f) turned = false;
    // Mover el mundo (habitación, TV y ventanas) para que tu cabeza quede sobre el punto elegido
    auto doTeleport = [&]() {
        if (g_vignette) g_vigAmt = 1.f;   // un parpadeo suave de los bordes al aparecer en el lugar nuevo
        float d[3] = {app.tpTgt[0] - head.pose.position.x, 0, app.tpTgt[2] - head.pose.position.z};
        // y bajarlo o subirlo para que el piso de destino (por ejemplo otra fila del cine) quede bajo tus pies
        float dy = app.tpTgt[1];
        app.anc[0] -= d[0]; app.anc[2] -= d[2];
        app.worldY -= dy;
        std::lock_guard<std::mutex> lk(g_uiLock);
        for (UiPanel *p : g_ui) { p->pose.position.x -= d[0]; p->pose.position.z -= d[2]; p->pose.position.y -= dy; }
    };
    bool hold = handAim || (!handPoint && g_moveMode != 1 && st.y < -0.65f && app.hp[1].hover < 0 && app.hp[1].down < 0 && app.hp[1].grab < 0);
    if (!hold) {
        // (con la mano, dejar de apuntar cancela; con el joystick, soltarlo te lleva)
        if (app.tpAim && !app.tpByHand && st.y > -0.3f && app.tpOk) doTeleport();
        if (app.tpAim && (app.tpByHand || st.y > -0.3f)) { app.tpAim = false; app.tpArc.clear(); }
        return false;
    }
    app.tpAim = true;
    app.tpByHand = handAim;
    float p[3], d[3];
    if (handAim) {   // con la mano: el arco sale de la punta del índice, hacia donde apunta
        for (int k = 0; k < 3; k++) { p[k] = RH.gPointO[k]; d[k] = RH.gPointD[k]; }
    } else {
        XrSpaceLocation a{XR_TYPE_SPACE_LOCATION};
        if (XR_FAILED(xrLocateSpace(app.aimSpace[1], app.space, time, &a)) || !(a.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) return true;
        p[0] = a.pose.position.x; p[1] = a.pose.position.y; p[2] = a.pose.position.z;
        const float fw[3] = {0, 0, -1};
        rotate(a.pose.orientation, fw, d);
    }
    float v[3] = {d[0] * 6.5f, d[1] * 6.5f, d[2] * 6.5f};
    app.tpArc.clear();
    app.tpOk = false;
    for (int i = 0; i < 70; i++) {
        float np[3] = {p[0] + v[0] * 0.025f, p[1] + v[1] * 0.025f, p[2] + v[2] * 0.025f};
        v[1] -= 9.8f * 0.025f;
        float fy = floorAt(app, np[0], np[2]);
        if (np[1] <= fy) {
            float t = (p[1] - fy) / std::max(1e-4f, p[1] - np[1]);
            for (int k = 0; k < 3; k++) app.tpTgt[k] = p[k] + (np[k] - p[k]) * t;
            app.tpTgt[1] = fy;
            // ¿Cae dentro del lugar? (living: entre las paredes y antes del mueble; espacio: sobre la plataforma)
            app.tpOk = insideRoom(app, app.tpTgt[0], app.tpTgt[2]);
            for (int k = 0; k < 3; k++) app.tpArc.push_back(app.tpTgt[k]);
            break;
        }
        if (i % 2 == 0) for (int k = 0; k < 3; k++) app.tpArc.push_back(np[k]);
        for (int k = 0; k < 3; k++) p[k] = np[k];
    }
    if (handAim && shot && app.tpOk) { doTeleport(); app.tpAim = false; app.tpArc.clear(); return false; }
    return true;
}

static void initReveal(App &app) {
    const char *vs = R"(#version 300 es
out vec2 uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
    uv = (p + 1.0) * 0.5;
    gl_Position = vec4(p, 0.0, 1.0);
})";
    const char *fs = R"(#version 300 es
precision highp float;
uniform vec4 tanLRDU;
uniform mat3 rot;
uniform vec3 fwd;
uniform float ang;
in vec2 uv;
out vec4 o;
void main() {
    vec3 dv = normalize(vec3(mix(tanLRDU.x, tanLRDU.y, uv.x), mix(tanLRDU.z, tanLRDU.w, uv.y), -1.0));
    vec3 dw = rot * dv;
    float a = acos(clamp(dot(dw, fwd), -1.0, 1.0));
    // borde muy suave que avanza desde atrás tuyo hacia adelante (con un poco de grano para que no se vean escalones)
    float m = 1.0 - smoothstep(ang - 0.75, ang, a);
    m = clamp(m + (fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233))) * 43758.5453) - 0.5) / 128.0, 0.0, 1.0);
    o = vec4(m);
})";
    app.maskProg = glCreateProgram();
    glAttachShader(app.maskProg, compileShader(GL_VERTEX_SHADER, vs));
    glAttachShader(app.maskProg, compileShader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(app.maskProg);
    app.mTan = glGetUniformLocation(app.maskProg, "tanLRDU");
    app.mRot = glGetUniformLocation(app.maskProg, "rot");
    app.mFwd = glGetUniformLocation(app.maskProg, "fwd");
    app.mAng = glGetUniformLocation(app.maskProg, "ang");
}

/// Cuánto del ambiente se ve (0 = solo tu cuarto real, 1 = todo el ambiente), con aceleración suave
static float revealAmount(App &app) {
    double t = nowMs();
    float r = app.revealStart > 0 ? (float)std::min(1.0, (t - app.revealStart) / 2000.0) : 0.f;
    if (app.exitStart > 0) r = std::min(r, (float)std::max(0.0, 1.0 - (t - app.exitStart) / 2000.0));
    if (app.envSwitchStart > 0 && app.envSwitchPT) {   // cambio de/al passthrough: se cierra (0,9 s) y se abre la nueva (0,9 s)
        double k = (t - app.envSwitchStart) / 900.0;
        r = std::min(r, (float)std::min(1.0, std::max(0.0, k < 1.0 ? 1.0 - k : k - 1.0)));
    }
    return r * r * r * (r * (6 * r - 15) + 10);   // arranca y termina despacio
}

/// Deja ver tu cuarto real fuera del círculo que se va abriendo (multiplica la imagen ya dibujada)
static void applyReveal(App &app, const XrView &view, float r) {
    glUseProgram(app.maskProg);
    glUniform4f(app.mTan, tanf(view.fov.angleLeft), tanf(view.fov.angleRight), tanf(view.fov.angleDown), tanf(view.fov.angleUp));
    float m[9]; quatToMat(view.pose.orientation, m);
    glUniformMatrix3fv(app.mRot, 1, GL_FALSE, m);
    glUniform3fv(app.mFwd, 1, app.revealFwd);
    glUniform1f(app.mAng, r * ((float)M_PI + 0.8f));
    glEnable(GL_BLEND);
    glBlendFunc(GL_ZERO, GL_SRC_COLOR);
    glBindVertexArray(app.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
}

/// Cambio de sala directo: cuánto de la sala nueva se ve (0 → 1 en 1,5 s), con aceleración suave
static float crossAmount(App &app) {
    if (app.envSwitchStart <= 0 || app.envSwitchPT) return 1.f;
    float r = (float)std::min(1.0, (nowMs() - app.envSwitchStart) / 1500.0);
    return r * r * r * (r * (6 * r - 15) + 10);
}

/// La sala que se va, encima de la nueva fuera del borde: se dibuja aparte (en una textura) y se mezcla con la máscara
static void renderRoom(App &app, const XrView &view, const TvInfo *tv);
static void crossFrom(App &app, int e, const XrView &view, const TvInfo *tv, GLuint dstFbo, int w, int h, float k) {
    if (!app.blitProg) {
        const char *vs = R"(#version 300 es
out vec2 uv;
void main() { vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)); uv = p; gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0); })";
        const char *fs = R"(#version 300 es
precision mediump float;
uniform sampler2D tex;
in vec2 uv;
out vec4 o;
void main() { o = vec4(texture(tex, uv).rgb, 1.0); })";
        app.blitProg = glCreateProgram();
        glAttachShader(app.blitProg, compileShader(GL_VERTEX_SHADER, vs));
        glAttachShader(app.blitProg, compileShader(GL_FRAGMENT_SHADER, fs));
        glLinkProgram(app.blitProg);
        app.bTex = glGetUniformLocation(app.blitProg, "tex");
    }
    if (app.xW[e] != w || app.xH[e] != h) {
        if (!app.xTex[e]) { glGenTextures(1, &app.xTex[e]); glGenFramebuffers(1, &app.xFbo[e]); glGenRenderbuffers(1, &app.xDepth[e]); }
        glBindTexture(GL_TEXTURE_2D, app.xTex[e]);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindRenderbuffer(GL_RENDERBUFFER, app.xDepth[e]);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, app.xFbo[e]);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, app.xTex[e], 0);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, app.xDepth[e]);
        app.xW[e] = w; app.xH[e] = h;
    }
    // la sala vieja, en su textura
    glBindFramebuffer(GL_FRAMEBUFFER, app.xFbo[e]);
    glViewport(0, 0, w, h);
    const int cur = g_env;
    g_env = app.envFrom;
    renderRoom(app, view, tv);
    g_env = cur;
    glDisable(GL_DEPTH_TEST);
    // la nueva ya está dibujada: la máscara la deja solo dentro del borde (y su alfa dice cuánto), y afuera va la vieja
    glBindFramebuffer(GL_FRAMEBUFFER, dstFbo);
    glViewport(0, 0, w, h);
    applyReveal(app, view, k);
    glUseProgram(app.blitProg);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, app.xTex[e]);
    glUniform1i(app.bTex, 0);
    glEnable(GL_BLEND);
    glBlendFuncSeparate(GL_ONE_MINUS_DST_ALPHA, GL_ONE, GL_ONE, GL_ZERO);
    glBindVertexArray(app.vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glDisable(GL_BLEND);
}

/// Cuánto se ve un panel durante la transición: el mismo borde suave que abre la sala lo va descubriendo
static float panelReveal(App &app, const XrPosef &pose, float reveal) {
    if (reveal >= 1.f) return 1.f;
    if (reveal <= 0.f) return 0.f;
    float d[3] = {pose.position.x - app.headPos[0], pose.position.y - app.headPos[1], pose.position.z - app.headPos[2]};
    float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 1e-3f) return reveal;
    float c = (d[0] * app.revealFwd[0] + d[1] * app.revealFwd[1] + d[2] * app.revealFwd[2]) / l;
    float a = acosf(std::min(1.f, std::max(-1.f, c)));
    float ang = reveal * ((float)M_PI + 0.8f);
    float t = std::min(1.f, std::max(0.f, (a - (ang - 0.75f)) / 0.75f));
    return 1.f - t * t * (3.f - 2.f * t);
}

// ---------------------------------------------------------------------------
// Generación de cuadros (Application SpaceWarp de Meta): con un juego abierto, la Mac lo hace dibujar a la mitad
// de cuadros y el Quest inventa los intermedios. Para eso el Quest necesita saber cómo se mueve cada parte de la
// imagen: se compara cada cuadro nuevo con el anterior (en chiquito, por bloques) y se le pasa ese movimiento.
// ---------------------------------------------------------------------------
struct FrameGen {
    bool ready = false, failed = false;
    int mvW = 0, mvH = 0;                    // tamaño recomendado del movimiento (por ojo)
    Swapchain mv[2], depth[2];
    GLuint lumaTex[2][2][2] = {};            // [ojo][actual/anterior][fino/grueso]
    GLuint flowTex[2][2] = {};               // [ojo][grueso/fino] (movimiento en píxeles de ese nivel)
    GLuint lumaWarp[2][2] = {};              // [ojo][fino/grueso]: el anterior visto desde la cabeza del actual
    GLuint fbo = 0;
    GLuint progLuma = 0, progSearch = 0, progOut = 0, progWarp = 0;
    GLint wPrev = -1, wCurR = -1, wPrevRT = -1, wCurTan = -1, wPrevTan = -1;
    XrPosef pose[2][2] = {};                 // [actual/anterior][ojo]: con qué pose dibujó la Mac cada cuadro
    XrFovf fov[2][2] = {};
    double lastArrival = 0, interval = 14;   // cuándo llegó el último cuadro y cada cuánto llegan (ms)
    GLint lOff = -1, lTex = -1, sCur = -1, sPrev = -1, sInit = -1, sR = -1, sScale = -1, sHas = -1, oFlow = -1, oSize = -1;
    int cur = 0;                             // cuál de los dos lumas es el actual
    uint32_t lastSeq = 0;
    bool havePrev = false;
    int updates = 0;
};
static FrameGen g_fg;
static const int FG_FINE = 128, FG_COARSE = 64;

static GLuint fgProgram(const char *fs) {
    static const char *vs = R"(#version 300 es
out vec2 uv;
void main() {
    vec2 p = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2)) * 2.0 - 1.0;
    uv = vec2((p.x + 1.0) * 0.5, (1.0 - p.y) * 0.5);
    gl_Position = vec4(p, 0.0, 1.0);
})";
    GLuint p = glCreateProgram();
    glAttachShader(p, compileShader(GL_VERTEX_SHADER, vs));
    glAttachShader(p, compileShader(GL_FRAGMENT_SHADER, fs));
    glLinkProgram(p);
    GLint ok = 0; glGetProgramiv(p, GL_LINK_STATUS, &ok);
    if (!ok) { char log[512]; glGetProgramInfoLog(p, sizeof log, nullptr, log); sendLog("Generación de cuadros: falló un programa (%s)", log); }
    return p;
}

static GLuint fgTex(GLenum ifmt, GLenum fmt, GLenum type, int w, int h, GLenum filter) {
    GLuint t; glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, ifmt, w, h, 0, fmt, type, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    return t;
}

static bool fgInit(App &app) {
    if (g_fg.ready || g_fg.failed) return g_fg.ready;
    g_fg.failed = true;   // si algo sale mal, no se reintenta en cada cuadro
    XrSystemSpaceWarpPropertiesFB sw{XR_TYPE_SYSTEM_SPACE_WARP_PROPERTIES_FB};
    XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
    if (app.hasSpaceWarp) sp.next = &sw;
    if (app.hasSpaceWarp && XR_SUCCEEDED(xrGetSystemProperties(app.instance, app.system, &sp)) && sw.recommendedMotionVectorImageRectWidth) {
        g_fg.mvW = (int)sw.recommendedMotionVectorImageRectWidth; g_fg.mvH = (int)sw.recommendedMotionVectorImageRectHeight;
    } else { g_fg.mvW = g_fg.mvH = 0; }
    for (int e = 0; e < 2; e++) {
        for (int pass = 0; pass < 2 && g_fg.mvW > 0; pass++) {
            Swapchain &s = pass == 0 ? g_fg.mv[e] : g_fg.depth[e];
            XrSwapchainCreateInfo ci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
            ci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | (pass == 0 ? XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT : XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT);
            ci.format = pass == 0 ? GL_RGBA16F : GL_DEPTH_COMPONENT24;
            ci.sampleCount = 1; ci.width = (uint32_t)g_fg.mvW; ci.height = (uint32_t)g_fg.mvH;
            ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
            XrResult r = xrCreateSwapchain(app.session, &ci, &s.handle);
            if (XR_FAILED(r)) { sendLog("Generación de cuadros: no se pudo crear la imagen de %s (error %d)", pass ? "profundidad" : "movimiento", (int)r); return false; }
            s.w = g_fg.mvW; s.h = g_fg.mvH;
            uint32_t n = 0;
            xrEnumerateSwapchainImages(s.handle, 0, &n, nullptr);
            s.images.assign(n, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
            xrEnumerateSwapchainImages(s.handle, n, &n, (XrSwapchainImageBaseHeader *)s.images.data());
        }
        for (int k = 0; k < 2; k++) {
            g_fg.lumaTex[e][k][0] = fgTex(GL_R8, GL_RED, GL_UNSIGNED_BYTE, FG_FINE, FG_FINE, GL_LINEAR);
            g_fg.lumaTex[e][k][1] = fgTex(GL_R8, GL_RED, GL_UNSIGNED_BYTE, FG_COARSE, FG_COARSE, GL_LINEAR);
        }
        g_fg.lumaWarp[e][0] = fgTex(GL_R8, GL_RED, GL_UNSIGNED_BYTE, FG_FINE, FG_FINE, GL_LINEAR);
        g_fg.lumaWarp[e][1] = fgTex(GL_R8, GL_RED, GL_UNSIGNED_BYTE, FG_COARSE, FG_COARSE, GL_LINEAR);
        g_fg.flowTex[e][0] = fgTex(GL_RG16F, GL_RG, GL_HALF_FLOAT, FG_COARSE, FG_COARSE, GL_NEAREST);
        g_fg.flowTex[e][1] = fgTex(GL_RG16F, GL_RG, GL_HALF_FLOAT, FG_FINE, FG_FINE, GL_LINEAR);
    }
    glGenFramebuffers(1, &g_fg.fbo);
    // la profundidad: todo «lejos» (el video no trae profundidad); se arma una sola vez
    for (int e = 0; e < 2 && g_fg.mvW > 0; e++) {
        Swapchain &d = g_fg.depth[e];
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        xrAcquireSwapchainImage(d.handle, &ai, &idx);
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(d.handle, &wi);
        glBindFramebuffer(GL_FRAMEBUFFER, g_fg.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, d.images[idx].image, 0);
        glDepthMask(GL_TRUE); glClearDepthf(1.f); glClear(GL_DEPTH_BUFFER_BIT);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, 0, 0);
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(d.handle, &ri);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // brillo de la imagen del juego, en chiquito (el promedio de 4 muestras, para no perder detalle)
    g_fg.progLuma = fgProgram(R"(#version 300 es
#extension GL_OES_EGL_image_external_essl3 : require
precision mediump float;
uniform samplerExternalOES tex;
uniform vec4 off;   // x: desde dónde (0 u 0,5 = cada ojo), y: ancho de un ojo, zw: medio píxel de salida
in vec2 uv;
out vec4 color;
float l(vec2 p) { return dot(texture(tex, vec2(off.x + clamp(p.x, 0.0, 1.0) * off.y, clamp(p.y, 0.0, 1.0))).rgb, vec3(0.299, 0.587, 0.114)); }
void main() {
    float v = 0.25 * (l(uv + vec2(-off.z, -off.w)) + l(uv + vec2(off.z, -off.w)) + l(uv + vec2(-off.z, off.w)) + l(uv + vec2(off.z, off.w)));
    color = vec4(v, 0.0, 0.0, 1.0);
})");
    // búsqueda por bloques: para cada punto, dónde estaba en el cuadro anterior (dentro de ±r píxeles)
    g_fg.progSearch = fgProgram(R"(#version 300 es
precision highp float;
precision highp int;
uniform highp sampler2D cur;
uniform highp sampler2D prev;
uniform highp sampler2D init;   // estimación del nivel anterior (más grueso)
uniform int r;
uniform float scale;            // de la estimación al tamaño de este nivel
uniform float hasInit;
in vec2 uv;
out vec4 color;
void main() {
    ivec2 sz = textureSize(cur, 0);
    ivec2 p = ivec2(uv * vec2(sz));
    vec2 base = hasInit > 0.5 ? texture(init, uv).xy * scale : vec2(0.0);
    ivec2 b = ivec2(floor(base + 0.5));
    float best = 1e9; vec2 bestMv = base;
    for (int dy = -r; dy <= r; dy++) {
        for (int dx = -r; dx <= r; dx++) {
            ivec2 d = ivec2(dx, dy) - b;   // dónde buscar en el anterior: p + d   (movimiento = -d)
            float c = 0.0;
            for (int ky = -1; ky <= 1; ky++)
                for (int kx = -1; kx <= 1; kx++) {
                    ivec2 q = clamp(p + ivec2(kx, ky), ivec2(0), sz - 1);
                    ivec2 qq = clamp(q + d, ivec2(0), sz - 1);
                    c += abs(texelFetch(cur, q, 0).r - texelFetch(prev, qq, 0).r);
                }
            c += 0.004 * length(vec2(dx, dy));   // a igualdad, preferir el movimiento más chico
            if (c < best) { best = c; bestMv = -vec2(d); }
        }
    }
    color = vec4(bestMv, 0.0, 1.0);
})");
    // al formato de Meta: diferencia en coordenadas de pantalla (-1 a 1), actual menos anterior
    g_fg.progOut = fgProgram(R"(#version 300 es
precision highp float;
uniform highp sampler2D flow;
uniform vec2 size;
in vec2 uv;
out vec4 color;
void main() {
    vec2 m = texture(flow, uv).xy;           // en píxeles del nivel fino (y hacia abajo)
    color = vec4(m.x / size.x * 2.0, m.y / size.y * 2.0, 0.0, 0.0);   // (y del movimiento: hacia arriba, como la pantalla)
})");
    // el cuadro anterior visto desde donde estaba la cabeza en el actual (así el giro de cabeza no cuenta como movimiento)
    g_fg.progWarp = fgProgram(R"(#version 300 es
precision highp float;
uniform highp sampler2D prev;
uniform mat3 curR;      // ojo actual → sala
uniform mat3 prevRT;    // sala → ojo anterior
uniform vec4 curTan;    // tangentes del campo de visión (izq, der, arriba, abajo)
uniform vec4 prevTan;
in vec2 uv;
out vec4 color;
void main() {
    vec3 d = vec3(mix(curTan.x, curTan.y, uv.x), mix(curTan.z, curTan.w, uv.y), -1.0);
    vec3 q = prevRT * (curR * d);
    if (q.z > -1e-4) { color = vec4(0.5, 0.0, 0.0, 1.0); return; }
    vec2 t = vec2(q.x / -q.z, q.y / -q.z);
    vec2 u = vec2((t.x - prevTan.x) / (prevTan.y - prevTan.x), (prevTan.z - t.y) / (prevTan.z - prevTan.w));
    color = vec4(texture(prev, vec2(u.x, 1.0 - u.y)).r, 0.0, 0.0, 1.0);   // (las imágenes chicas están guardadas al revés en vertical)
})");
    g_fg.wPrev = glGetUniformLocation(g_fg.progWarp, "prev"); g_fg.wCurR = glGetUniformLocation(g_fg.progWarp, "curR");
    g_fg.wPrevRT = glGetUniformLocation(g_fg.progWarp, "prevRT"); g_fg.wCurTan = glGetUniformLocation(g_fg.progWarp, "curTan");
    g_fg.wPrevTan = glGetUniformLocation(g_fg.progWarp, "prevTan");
    g_fg.lOff = glGetUniformLocation(g_fg.progLuma, "off"); g_fg.lTex = glGetUniformLocation(g_fg.progLuma, "tex");
    g_fg.sCur = glGetUniformLocation(g_fg.progSearch, "cur"); g_fg.sPrev = glGetUniformLocation(g_fg.progSearch, "prev");
    g_fg.sInit = glGetUniformLocation(g_fg.progSearch, "init"); g_fg.sR = glGetUniformLocation(g_fg.progSearch, "r");
    g_fg.sScale = glGetUniformLocation(g_fg.progSearch, "scale"); g_fg.sHas = glGetUniformLocation(g_fg.progSearch, "hasInit");
    g_fg.oFlow = glGetUniformLocation(g_fg.progOut, "flow"); g_fg.oSize = glGetUniformLocation(g_fg.progOut, "size");
    g_fg.failed = false; g_fg.ready = true;
    sendLog("Generación de cuadros lista (movimiento %dx%d por ojo)", g_fg.mvW, g_fg.mvH);
    return true;
}

static void fgDrawTo(GLuint tex, int w, int h) {
    glBindFramebuffer(GL_FRAMEBUFFER, g_fg.fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    glViewport(0, 0, w, h);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

/// Un cuadro nuevo del juego: calcular cuánto se movió cada zona respecto del anterior y dejarlo listo para Meta
static void fgUpdate(App &app, bool compensateHead) {
    if (app.frameSeq == g_fg.lastSeq) return;
    g_fg.lastSeq = app.frameSeq;
    {   // cada cuánto llegan los cuadros del juego (para saber cuánto adelantar la imagen)
        double now = nowMs();
        if (g_fg.lastArrival > 0) { double d = now - g_fg.lastArrival; if (d > 2 && d < 100) g_fg.interval = g_fg.interval * 0.9 + d * 0.1; }
        g_fg.lastArrival = now;
    }
    for (int e = 0; e < 2; e++) { g_fg.pose[1][e] = g_fg.pose[0][e]; g_fg.fov[1][e] = g_fg.fov[0][e]; g_fg.pose[0][e] = app.shown.eye[e].pose; g_fg.fov[0][e] = app.shown.eye[e].fov; }
    glDisable(GL_BLEND); glDisable(GL_DEPTH_TEST); glDisable(GL_CULL_FACE);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glBindVertexArray(app.vao);
    int nc = g_fg.cur ^ 1;   // el nuevo actual
    // 1) brillo en chiquito (fino y grueso) de cada ojo
    glUseProgram(g_fg.progLuma);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_EXTERNAL_OES, app.tex);
    glUniform1i(g_fg.lTex, 0);
    for (int e = 0; e < 2; e++) {
        const int sizes[2] = {FG_FINE, FG_COARSE};
        for (int lv = 0; lv < 2; lv++) {
            glUniform4f(g_fg.lOff, e == 0 ? 0.f : 0.5f, 0.5f, 0.25f / sizes[lv], 0.25f / sizes[lv]);
            fgDrawTo(g_fg.lumaTex[e][nc][lv], sizes[lv], sizes[lv]);
        }
    }
    g_fg.cur = nc;
    if (!g_fg.havePrev) { g_fg.havePrev = true; return; }
    int pc = nc ^ 1;
    // 1b) completar cuadros: el anterior se lleva a la pose de cabeza del actual (el giro de cabeza ya lo corrige el visor)
    if (compensateHead) {
        glUseProgram(g_fg.progWarp);
        glUniform1i(g_fg.wPrev, 0);
        for (int e = 0; e < 2; e++) {
            float C[9], Pm[9], PT[9];
            quatToMat(g_fg.pose[0][e].orientation, C);
            quatToMat(g_fg.pose[1][e].orientation, Pm);
            for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) PT[c * 3 + r] = Pm[r * 3 + c];   // traspuesta
            glUniformMatrix3fv(g_fg.wCurR, 1, GL_FALSE, C);
            glUniformMatrix3fv(g_fg.wPrevRT, 1, GL_FALSE, PT);
            const XrFovf &fc = g_fg.fov[0][e], &fp = g_fg.fov[1][e];
            glUniform4f(g_fg.wCurTan, tanf(fc.angleLeft), tanf(fc.angleRight), tanf(fc.angleUp), tanf(fc.angleDown));
            glUniform4f(g_fg.wPrevTan, tanf(fp.angleLeft), tanf(fp.angleRight), tanf(fp.angleUp), tanf(fp.angleDown));
            const int sizes[2] = {FG_FINE, FG_COARSE};
            for (int lv = 0; lv < 2; lv++) {
                glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_fg.lumaTex[e][pc][lv]);
                fgDrawTo(g_fg.lumaWarp[e][lv], sizes[lv], sizes[lv]);
            }
        }
    }
    // 2) búsqueda: primero en grueso (±5), después se afina (±2)
    glUseProgram(g_fg.progSearch);
    glUniform1i(g_fg.sCur, 0); glUniform1i(g_fg.sPrev, 1); glUniform1i(g_fg.sInit, 2);
    for (int e = 0; e < 2; e++) {
        for (int lv = 1; lv >= 0; lv--) {
            glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_fg.lumaTex[e][nc][lv]);
            glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, compensateHead ? g_fg.lumaWarp[e][lv] : g_fg.lumaTex[e][pc][lv]);
            // (en el nivel grueso no se usa la estimación: se enlaza otra textura para no leer y escribir la misma)
            glActiveTexture(GL_TEXTURE2); glBindTexture(GL_TEXTURE_2D, g_fg.flowTex[e][lv == 1 ? 1 : 0]);
            glUniform1i(g_fg.sR, lv == 1 ? 5 : 2);
            glUniform1f(g_fg.sScale, (float)FG_FINE / FG_COARSE);
            glUniform1f(g_fg.sHas, lv == 1 ? 0.f : 1.f);
            int sz = lv == 1 ? FG_COARSE : FG_FINE;
            fgDrawTo(g_fg.flowTex[e][lv == 1 ? 0 : 1], sz, sz);
        }
    }
    // 3) al formato de Meta, en su imagen de movimiento (solo en el modo de Meta)
    if (compensateHead || g_fg.mvW <= 0) { glBindFramebuffer(GL_FRAMEBUFFER, 0); glActiveTexture(GL_TEXTURE0); g_fg.updates++; return; }
    glUseProgram(g_fg.progOut);
    glUniform1i(g_fg.oFlow, 0);
    glUniform2f(g_fg.oSize, (float)FG_FINE, (float)FG_FINE);
    for (int e = 0; e < 2; e++) {
        Swapchain &s = g_fg.mv[e];
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        if (XR_FAILED(xrAcquireSwapchainImage(s.handle, &ai, &idx))) continue;
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(s.handle, &wi);
        glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, g_fg.flowTex[e][1]);
        fgDrawTo(s.images[idx].image, s.w, s.h);
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(s.handle, &ri);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glActiveTexture(GL_TEXTURE0);
    g_fg.updates++;
}

static void renderFrame(App &app) {
    XrFrameState fs{XR_TYPE_FRAME_STATE};
    if (XR_FAILED(xrWaitFrame(app.session, nullptr, &fs))) return;
    xrBeginFrame(app.session, nullptr);

    XrActiveActionSet aas{app.actionSet, XR_NULL_PATH};
    XrActionsSyncInfo si{XR_TYPE_ACTIONS_SYNC_INFO};
    si.countActiveActionSets = 1; si.activeActionSets = &aas;
    xrSyncActions(app.session, &si);

    // Salida de emergencia, también durante un juego: los dos joysticks apretados 1,5 segundos
    if (getBool(app, app.aStickClick, 0) && getBool(app, app.aStickClick, 1)) {
        if (app.sticksSince == 0) app.sticksSince = nowMs();
        else if (nowMs() - app.sticksSince > 1500) g_exitVR = true;
    } else app.sticksSince = 0;
    handCalTick(app, fs.predictedDisplayTime);

    // Pausar los controles y las manos (en Studio): mantener A+B (o X+Y) o juntar el pulgar con el meñique derecho 1 segundo
    {
        static double since = 0;
        static bool armed = true;
        const RealHands::Hand &R = g_rh.h[1];
        const bool combo = (getBool(app, app.aA, 0) && getBool(app, app.aB, 0)) || (getBool(app, app.aA, 1) && getBool(app, app.aB, 1));
        const bool gest = !g_handsOff && R.active && R.gPinchLittle > 0.85f && R.gTrig < 0.4f && R.gPinchMid < 0.4f && R.gPinchRing < 0.4f;
        bool toggle = g_inputLockReq.exchange(false) && !g_inputLock;   // (el botón solo pausa: en pausa no se toca nada)
        if ((combo || gest) && !g_inGameNow) {
            if (since == 0) since = nowMs();
            else if (armed && nowMs() - since > 1000) { armed = false; toggle = true; }
        } else { since = 0; armed = true; }
        if (toggle) {
            {
                const bool on = !g_inputLock;
                g_inputLock = on;
                if (on) {   // lo que estaba apretado se suelta
                    std::lock_guard<std::mutex> lk(g_uiLock);
                    for (int h = 0; h < 2; h++) {
                        App::HandPtr &hp = app.hp[h];
                        if (hp.down >= 0) { javaPointer(hp.down, 3, 0, 0, 0); hp.down = -1; }
                        hp.grab = -1; hp.hover = -1;
                    }
                    if (app.lastHover >= 0) { javaPointer(app.lastHover, 5, 0, 0, 0); app.lastHover = -1; }
                }
                javaBool(g_jInputLock, on);
                sendLog(on ? "Controles y manos en pausa" : "Controles y manos activos de nuevo");
            }
        }
    }

    // Altura elegida: se corre el origen del espacio de la sala (todo: sala, ventanas, manos y juegos)
    if (app.stageSpace && app.haveEye && g_heightDirty.exchange(false)) {
        float realEye = app.lastEyeY - app.heightOff;
        float hm = g_heightM.load();
        float off = hm > 0.5f ? std::max(-1.2f, std::min(1.2f, hm * 0.936f - realEye)) : 0.f;
        if (fabsf(off - app.heightOff) > 0.005f) {
            XrReferenceSpaceCreateInfo rs{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
            rs.referenceSpaceType = app.spaceType;
            rs.poseInReferenceSpace.orientation.w = 1;
            rs.poseInReferenceSpace.position.y = -off;
            XrSpace ns = XR_NULL_HANDLE;
            if (XR_SUCCEEDED(xrCreateReferenceSpace(app.session, &rs, &ns))) {
                float d = off - app.heightOff;
                app.space = ns;   // (el anterior queda vivo: algunas formas se crearon respecto de él)
                app.heightOff = off;
                app.eyeH += d; app.lastEyeY += d;
                std::lock_guard<std::mutex> lk(g_uiLock);
                for (UiPanel *p : g_ui) {
                    // la pantalla fija del living o del cine va sobre su mueble (se mueve con la sala);
                    // las ventanas quedan a la misma altura respecto de tus ojos
                    if (p->tv && fixedScreenEnv()) { if (p->placed) placePanel(app, p); }
                    else p->pose.position.y += d;
                }
                LOGI("Altura: %.2f m (ojos reales a %.2f m) → se suben %.2f m", hm, realEye, off);
            }
        }
    }
    XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
    vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    vli.displayTime = fs.predictedDisplayTime;
    vli.space = app.space;
    XrViewState vs{XR_TYPE_VIEW_STATE};
    XrView views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
    uint32_t vc = 0;
    xrLocateViews(app.session, &vli, &vs, 2, &vc, views);
    bool valid = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) && (vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT);
    if (valid) { app.lastEyeY = (views[0].pose.position.y + views[1].pose.position.y) * 0.5f; app.haveEye = true; }
    if (g_vigia) {   // Modo Centinela: el juego recibe la cabeza virtual (el visor está apoyado)
        XrView fv[2];
        g_vg.headTracked = (vs.viewStateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT) != 0;
        vigiaViews(app, fs.predictedDisplayTime, views[0], fv);
        sendPose(app, fv, fs.predictedDisplayTime);
    } else if (valid) sendPose(app, views, fs.predictedDisplayTime);
    applyRefreshRate(app);
    latchFrame(app);

    bool live = app.haveFrame && nowMs() - app.lastFrameMs < 1500;
    // Con un juego abierto se ve el video de la Mac; si no, el living (dibujado acá)
    bool showVideo = live && g_gameActive && !g_gameHidden;
    // Entrar o salir de un juego: la parte Java pausa lo que no se ve (ventanas de la sala, menú) y lo reanuda al volver
    if (showVideo != g_inGameNow) {
        g_inGameNow = showVideo;
        if (!showVideo && g_gameMenu) { g_gameMenu = false; javaBool(g_jGameMenu, false); }
        javaBool(g_jGameState, showVideo);
        sendLog(showVideo ? "Juego VR: la sala se apaga (menú del juego: mantener el botón de menú izquierdo)" : "Juego VR cerrado: vuelve la sala");
    }
    // Menú del juego: mantener apretado el botón de menú del control izquierdo (el cóncavo) medio segundo
    {
        static double since = 0;
        static bool armed = true;
        // (con las manos: el gesto que tenga «Menú» en el perfil de la sala o del juego)
        bool mb = getBool(app, app.aMenu, 0) ||
                  ((!showVideo || g_handGestures) && !g_ctlInUse && (vctl(0).menu || vctl(1).menu));
        if (mb) {
            if (since == 0) since = nowMs();
            else if (armed && nowMs() - since > 550) {
                armed = false;
                if (g_vigia) g_vigiaCalReq = true;   // Modo Centinela: el botón de menú izquierdo calibra
                else if (showVideo) { g_gameMenu = !g_gameMenu; javaBool(g_jGameMenu, g_gameMenu); }
                else if (!g_inputLock && !g_loadingNow) {
                    // En la sala: abre o cierra el menú flotante delante tuyo (como mirar el borde de la mano)
                    float f[3] = {app.headFwd[0], 0, app.headFwd[2]};
                    float l = sqrtf(f[0] * f[0] + f[2] * f[2]);
                    if (l < 1e-3f) { f[0] = 0; f[2] = -1; l = 1; }
                    g_watchWorld[0] = app.headPos[0] + f[0] / l * 0.55f;
                    g_watchWorld[1] = app.headPos[1] - 0.35f;
                    g_watchWorld[2] = app.headPos[2] + f[2] / l * 0.55f;
                    g_watchNowOk = false;
                    javaWatch();
                }
            }
        } else { since = 0; armed = true; }
    }
    // Generación de cuadros: la Mac hace dibujar al juego la mitad de cuadros; acá se inventan los intermedios
    // (solo el modo «mitad» le pide a la Mac que el juego dibuje la mitad de cuadros)
    if (g_sock >= 0 && !g_frameGenSent.exchange(true)) sendPacket(g_frameGenMode == 2 ? "{\"type\":\"framegen\",\"on\":true}" : "{\"type\":\"framegen\",\"on\":false}");
    const int fgMode = g_frameGenMode.load();
    const bool fgOn = showVideo && fgMode == 2 && app.hasSpaceWarp && fgInit(app);       // Meta: el visor inventa 1 de cada 2
    const bool fgFill = showVideo && fgMode == 1 && fgInit(app);                          // propio: completar hasta los Hz del visor
    if (fgOn || fgFill) fgUpdate(app, fgFill);
    g_frameGenOn = fgOn || fgFill;
    // Pantalla de carga y cambio de sala (con la misma transición que al entrar: la sala que se va se cierra y la nueva se abre)
    g_loadingNow = g_loading && !showVideo && !g_vigia;
    {
        const int want = g_loadingNow ? 4 : g_envUser.load();
        static bool wasVideo = false;
        const bool fromGame = wasVideo && !showVideo;   // (al cerrarse un juego no se ve la sala de antes: directo a la nueva)
        wasVideo = showVideo;
        const double tNow = nowMs();
        if (app.envSwitchStart > 0 && (showVideo || !app.anchor)) {   // (un juego empezó en medio del cambio: se termina ya)
            app.envSwitchStart = 0;
            if (g_env != app.envSwitchTo) { g_env = app.envSwitchTo; g_envChanged = true; }
        }
        if (app.envSwitchStart == 0 && want != g_env) {
            const bool animate = app.anchor && !showVideo && !fromGame && app.revealStart > 0 && tNow - app.revealStart > 2000 && app.exitStart == 0;
            if (!animate) { g_env = want; g_envChanged = true; }
            else {
                app.envSwitchStart = tNow; app.envSwitchTo = want; app.envSwitched = false;
                // del passthrough (o hacia él) se pasa por tu cuarto; entre dos salas, la nueva aparece directo sobre la vieja
                app.envSwitchPT = g_env == 3 || want == 3;
                app.envFrom = g_env;
                if (!app.envSwitchPT) { app.envSwitched = true; g_env = want; g_envChanged = true; }
                // el borde avanza como al entrar: la sala nueva aparece desde atrás tuyo y termina adelante
                float hm[9]; quatToMat(views[0].pose.orientation, hm);
                float l = sqrtf(hm[6] * hm[6] + hm[8] * hm[8]);
                if (l > 1e-3f) { app.revealFwd[0] = hm[6] / l; app.revealFwd[1] = 0; app.revealFwd[2] = hm[8] / l; }
                sendLog("Cambio de sala: %d → %d%s", g_env.load(), want, g_loadingNow ? " (pantalla de carga)" : "");
            }
        }
        if (app.envSwitchStart > 0) {
            const double k = tNow - app.envSwitchStart;
            if (k >= 900 && !app.envSwitched) { app.envSwitched = true; g_env = app.envSwitchTo; g_envChanged = true; }
            if (k >= (app.envSwitchPT ? 1800 : 1500)) app.envSwitchStart = 0;
        }
    }
    float reveal = revealAmount(app);
    const float cross = crossAmount(app);
    if (app.hasPassthrough) {
        // El cuarto real hace falta durante la transición, y en la sala para ver tus manos reales
        bool need = reveal < 1.f || (g_rh.ok && g_realHands && !showVideo && !g_camMode) || (g_env == 3 && !showVideo);
        if (need && !app.ptRunning && app.ptStart) { app.ptStart(app.pt); app.ptRunning = true; }
        if (!need && app.ptRunning && app.ptPause) { app.ptPause(app.pt); app.ptRunning = false; }
    }
    TvInfo tvInfo{}; bool haveTv = false;
    {
        std::lock_guard<std::mutex> lk(g_uiLock);
        for (UiPanel *p : g_ui) if (p->tv && p->placed && !p->dead) { tvInfo = {p->pose, p->wm, p->hm}; haveTv = true; }
    }
    XrCompositionLayerProjectionView pv[2];
    XrCompositionLayerSpaceWarpInfoFB swInfo[2];
    for (int e = 0; e < 2; e++) {
        Swapchain &s = app.sc[e];
        uint32_t idx = 0;
        XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
        xrAcquireSwapchainImage(s.handle, &ai, &idx);
        XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
        wi.timeout = XR_INFINITE_DURATION;
        xrWaitSwapchainImage(s.handle, &wi);
        glBindFramebuffer(GL_FRAMEBUFFER, s.fbos[idx]);
        glViewport(0, 0, s.w, s.h);
        // La sala se dibuja al 85% de la resolución (el Quest la estira): la TV y las ventanas son capas aparte y siguen nítidas
        int rw = s.w, rh = s.h;
        if (showVideo) {
            glUseProgram(app.prog);
            glActiveTexture(GL_TEXTURE0);
            glBindTexture(GL_TEXTURE_EXTERNAL_OES, app.tex);
            glUniform1i(app.uTex, 0);
            glUniform1f(app.uOff, e == 0 ? 0.0f : 0.5f);
            glUniform1f(app.uSx, 0.5f);
            glUniform1f(app.uSy, 1.0f);
            glUniform1f(app.uSrgb, app.srgb ? 1.0f : 0.0f);
            glUniform1f(app.uKeepA, 0.0f);
            { const float full[4] = {0, 0, 1, 1}; setFilter(app, true, full, 1.f / std::max(1, g_vw), 1.f / std::max(1, g_vh)); }
            // Completar cuadros: adelantar la imagen según cómo se viene moviendo, hasta que llegue el próximo cuadro
            if (fgFill && g_fg.havePrev) {
                float k = (float)std::min(1.0, std::max(0.0, (nowMs() - g_fg.lastArrival) / std::max(4.0, g_fg.interval)));
                glActiveTexture(GL_TEXTURE1);
                glBindTexture(GL_TEXTURE_2D, g_fg.flowTex[e][1]);
                glUniform1i(app.uFlow, 1);
                glUniform2f(app.uFlowK, 1.f, k / FG_FINE);
                glActiveTexture(GL_TEXTURE0);
            }
            // Foveated encoding: la tabla para volver a estirar el borde (se rehace si cambió)
            if (app.fovVer != g_fovVer.load()) {
                FovParams P;
                { std::lock_guard<std::mutex> lk(g_fovLock); P = g_fovP; app.fovVer = g_fovVer.load(); }
                app.fovOn = P.level > 0;
                if (app.fovOn) {
                    static float lut[3 * 1024];
                    fovInverse(P, P.cx[0], lut); fovInverse(P, P.cx[1], lut + 1024); fovInverse(P, P.cy, lut + 2048);
                    if (!app.fovTex) glGenTextures(1, &app.fovTex);
                    glBindTexture(GL_TEXTURE_2D, app.fovTex);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                    glTexImage2D(GL_TEXTURE_2D, 0, GL_R32F, 1024, 3, 0, GL_RED, GL_FLOAT, lut);
                    glBindTexture(GL_TEXTURE_2D, 0);
                }
            }
            if (app.fovOn && app.fovTex) {
                glActiveTexture(GL_TEXTURE2);
                glBindTexture(GL_TEXTURE_2D, app.fovTex);
                glUniform1i(app.uFovLut, 2);
                glUniform1f(app.uFovOn, 1.f);
                glUniform1f(app.uFovRow, (float)e);
                glActiveTexture(GL_TEXTURE0);
            }
            glBindVertexArray(app.vao);
            glDrawArrays(GL_TRIANGLES, 0, 3);
            glUniform2f(app.uFlowK, 0.f, 0.f);
            glUniform1f(app.uFovOn, 0.f);
        } else if (app.anchor && g_env == 3 && app.hasPassthrough) {
            // Passthrough: no se dibuja ninguna sala, se ve tu cuarto real (las ventanas y las manos van encima)
            glClearColor(0, 0, 0, 0);
            glClear(GL_COLOR_BUFFER_BIT);
        } else if (app.anchor && app.roomProg) {
            rw = (int)(s.w * 0.85f) & ~1; rh = (int)(s.h * 0.85f) & ~1;
            glViewport(0, 0, rw, rh);
            renderRoom(app, views[e], haveTv ? &tvInfo : nullptr);
            if (reveal < 1.f) applyReveal(app, views[e], reveal);
            else if (cross < 1.f && app.envFrom != 3) crossFrom(app, e, views[e], haveTv ? &tvInfo : nullptr, s.fbos[idx], rw, rh, cross);
        } else {
            // Sin video: gris oscuro con un pulso suave (para saber que la app está viva); al entrar, se ve tu cuarto
            float k = 0.04f + 0.03f * (float)(0.5 + 0.5 * sin(nowMs() / 400.0));
            if (reveal < 1.f && app.hasPassthrough) glClearColor(0, 0, 0, 0); else glClearColor(k, k, k * 1.6f, 1);
            glClear(GL_COLOR_BUFFER_BIT);
        }
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
        xrReleaseSwapchainImage(s.handle, &ri);

        pv[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
        if (fgOn && g_fg.havePrev && g_fg.mvW > 0) {
            XrCompositionLayerSpaceWarpInfoFB &w = swInfo[e];
            w = {XR_TYPE_COMPOSITION_LAYER_SPACE_WARP_INFO_FB};
            w.motionVectorSubImage.swapchain = g_fg.mv[e].handle;
            w.motionVectorSubImage.imageRect = {{0, 0}, {g_fg.mvW, g_fg.mvH}};
            w.depthSubImage.swapchain = g_fg.depth[e].handle;
            w.depthSubImage.imageRect = {{0, 0}, {g_fg.mvW, g_fg.mvH}};
            w.appSpaceDeltaPose.orientation.w = 1;   // la sala no se mueve sola
            w.minDepth = 0.f; w.maxDepth = 1.f;
            w.nearZ = 0.1f; w.farZ = INFINITY;
            pv[e].next = &w;
        }
        // La imagen se muestra con la pose para la que la Mac la dibujó: el Quest corrige el resto
        pv[e].pose = showVideo ? app.shown.eye[e].pose : views[e].pose;
        pv[e].fov = showVideo ? app.shown.eye[e].fov : views[e].fov;
        pv[e].subImage.swapchain = s.handle;
        pv[e].subImage.imageRect = {{0, 0}, {rw, rh}};
    }
    XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    layer.space = app.space;
    layer.viewCount = 2;
    layer.views = pv;
    // Sala: la TV y las ventanas son capas propias del Quest, a resolución completa, encima de la habitación
    const int MAXL = 24;
    const XrCompositionLayerBaseHeader *layers[MAXL] = { (XrCompositionLayerBaseHeader *)&layer };
    uint32_t nLayers = 1;
    XrCompositionLayerPassthroughFB ptl{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    if ((reveal < 1.f || (g_env == 3 && app.anchor)) && !showVideo) {
        layer.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        if (app.hasPassthrough && app.ptLayer) {
            ptl.layerHandle = app.ptLayer;
            ptl.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            layers[0] = (XrCompositionLayerBaseHeader *)&ptl;
            layers[1] = (XrCompositionLayerBaseHeader *)&layer;
            nLayers = 2;
        }
    }
    if (valid && !app.anchor) {
        // Punto de referencia: dónde estabas y hacia dónde mirabas al entrar
        const XrPosef &hp = views[0].pose;
        float m[9]; quatToMat(hp.orientation, m);
        float fx = -m[6], fz = -m[8], l = sqrtf(fx * fx + fz * fz);
        if (l > 1e-3f) { app.fwd[0] = fx / l; app.fwd[2] = fz / l; }
        app.anc[0] = hp.position.x; app.anc[1] = 0; app.anc[2] = hp.position.z;
        app.eyeH = std::max(1.0f, hp.position.y);
        app.anchor = true;
        app.anchorAt = nowMs();
        // empieza detrás tuyo y avanza hacia adelante
        app.revealFwd[0] = m[6]; app.revealFwd[1] = 0; app.revealFwd[2] = m[8];
        { float l = sqrtf(m[6] * m[6] + m[8] * m[8]); if (l > 1e-3f) { app.revealFwd[0] /= l; app.revealFwd[2] /= l; } }
    }
    // La transición de entrada arranca recién cuando la cámara del Quest ya muestra tu cuarto (tarda un poco en
    // encenderse: antes se ve negro) y la TV tiene imagen; si no, se veía un corte antes de empezar.
    if (app.anchor && app.revealStart == 0) {
        double since = nowMs() - app.anchorAt;
        bool tvReady = false;
        {
            std::lock_guard<std::mutex> lk(g_uiLock);
            for (UiPanel *p : g_ui) if (p->tv && p->ready) tvReady = true;
        }
        if (since > 2500 || (since > 700 && tvReady)) app.revealStart = nowMs();
    }
    if (g_envChanged.exchange(false) && app.anchor) {
        std::lock_guard<std::mutex> lk(g_uiLock);
        // el piso vuelve a estar a la altura real (por si estabas en otra fila del cine)
        for (UiPanel *p : g_ui) p->pose.position.y -= app.worldY;
        app.worldY = 0;
        for (UiPanel *p : g_ui) if (p->tv) {   // la TV va a su lugar en el ambiente nuevo
            if (fixedScreenEnv()) { p->curved = false; p->radius = 0; p->curveAmt = 0; }   // y si es fija, plana
            placePanel(app, p);
        }
    }
    updateUiPanels(app);
    // Durante un juego la sala se esconde. En la transición, la TV y las ventanas aparecen (y se van) de a poco,
    // con el mismo borde que abre la sala; solo se pueden usar cuando la transición terminó.
    bool smoothPanels = app.hasColorScale;
    bool showPanels = (!g_gameActive || !live || g_gameHidden) && (smoothPanels ? reveal > 0.f : reveal > 0.85f && app.exitStart == 0);
    bool showUi = (!g_gameActive || !live || g_gameHidden) && reveal >= 1.f && app.exitStart == 0;
    XrCompositionLayerSettingsFB settings{XR_TYPE_COMPOSITION_LAYER_SETTINGS_FB};
    settings.layerFlags = XR_COMPOSITION_LAYER_SETTINGS_QUALITY_SUPER_SAMPLING_BIT_FB;
    XrCompositionLayerQuad quads[MAXL];
    XrCompositionLayerCylinderKHR cyls[MAXL];
    XrCompositionLayerColorScaleBiasKHR fades[MAXL];
    int nq = 0, ncyl = 0, nf = 0;
    app.headPos[0] = views[0].pose.position.x; app.headPos[1] = views[0].pose.position.y; app.headPos[2] = views[0].pose.position.z;
    { const float f0[3] = {0, 0, -1}; rotate(views[0].pose.orientation, f0, app.headFwd); }
    // Ventanas fijadas a la vista: te siguen con un poco de demora (sin moverse con cada pequeño giro)
    {
        static double lastF = 0;
        double tf = nowMs();
        float dt = lastF > 0 ? std::min(0.1f, (float)(tf - lastF) / 1000.f) : 0.f;
        lastF = tf;
        float fx = app.headFwd[0], fz = app.headFwd[2], fl = sqrtf(fx * fx + fz * fz);
        if (valid && fl > 1e-3f) {
            fx /= fl; fz /= fl;
            const float rx = -fz, rz = fx;
            std::lock_guard<std::mutex> lk(g_uiLock);
            for (UiPanel *p : g_ui) {
                if (!p->follow || p->dead || !p->placed) continue;
                bool grabbed = app.hp[0].grab == p->id || app.hp[1].grab == p->id;
                float d[3] = {p->pose.position.x - app.headPos[0], p->pose.position.y - app.headPos[1], p->pose.position.z - app.headPos[2]};
                if (!p->folInit || grabbed) {   // (al moverla con la barrita, queda fijada en el lugar nuevo)
                    p->folOff[0] = d[0] * rx + d[2] * rz; p->folOff[1] = d[1]; p->folOff[2] = d[0] * fx + d[2] * fz;
                    p->folInit = true;
                    continue;
                }
                float tgt[3] = {app.headPos[0] + rx * p->folOff[0] + fx * p->folOff[2], app.headPos[1] + p->folOff[1],
                                app.headPos[2] + rz * p->folOff[0] + fz * p->folOff[2]};
                float k = 1.f - expf(-dt * 3.5f);
                float np[3] = {p->pose.position.x + (tgt[0] - p->pose.position.x) * k, p->pose.position.y + (tgt[1] - p->pose.position.y) * k,
                               p->pose.position.z + (tgt[2] - p->pose.position.z) * k};
                p->pose.position = {np[0], np[1], np[2]};
                p->pose.orientation = facingTilt(np, app.headPos);
            }
        }
    }
    // Un panel como capa del Quest: plano, o curvo (parte de un cilindro) si se eligió curvarlo
    // Cabeza: punto medio entre los dos ojos
    // Centro óptico de cada ojo en la imagen (para el foveated encoding: lo nítido va ahí)
    for (int e = 0; e < 2; e++) {
        float tl = tanf(views[e].fov.angleLeft), tr = tanf(views[e].fov.angleRight);
        float tu = tanf(views[e].fov.angleUp), td = tanf(views[e].fov.angleDown);
        if (tr - tl > 0.1f) g_fovC[e] = -tl / (tr - tl);
        if (e == 0 && tu - td > 0.1f) g_fovC[2] = tu / (tu - td);
    }
    XrPosef headPose = views[0].pose;
    headPose.position = {(views[0].pose.position.x + views[1].pose.position.x) / 2, (views[0].pose.position.y + views[1].pose.position.y) / 2,
                         (views[0].pose.position.z + views[1].pose.position.z) / 2};
    auto submit = [&](UiPanel *p, XrSwapchain sc, XrEyeVisibility eye) {
        // Transparencia durante la transición (la imagen es premultiplicada: se escalan color y alfa)
        const void *chain = app.hasLayerSettings ? &settings : nullptr;
        bool fading = false;
        // Animación al abrir (crece un poco y aparece), al cerrar (se achica y se va) y al moverla (se «levanta»)
        float animA = 1.f, animS = 1.f;
        {
            const UiPanel *ap = p;
            if (p->parent) for (UiPanel *q : g_ui) if (q->id == p->parent) { ap = q; break; }   // la barra sigue a su ventana
            double tn = nowMs();
            if (ap->openAt > 0 && tn - ap->openAt < 320) {
                float x = (float)(tn - ap->openAt) / 320.f, e = 1.f - (1.f - x) * (1.f - x) * (1.f - x);
                animA *= e; animS *= 0.9f + 0.1f * e;
            }
            if (ap->closeAt > 0) {
                float x = std::min(1.f, (float)(tn - ap->closeAt) / 180.f), e = x * x;
                animA *= 1.f - e; animS *= 1.f - 0.12f * e;
            }
            animS *= 1.f - 0.03f * ap->grabK; animA *= 1.f - 0.12f * ap->grabK;
            if (!app.hasColorScale) animA = 1.f;
        }
        if ((smoothPanels && reveal < 1.f) || animA < 0.999f) {
            float a = (smoothPanels && reveal < 1.f) ? (p->hud ? reveal : panelReveal(app, p->pose, reveal)) : 1.f;
            a *= animA;
            if (a <= 0.002f) return;
            XrCompositionLayerColorScaleBiasKHR &f = fades[nf++];
            f = {XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
            f.next = chain;
            f.colorScale = {a, a, a, a};
            f.colorBias = {0, 0, 0, 0};
            chain = &f; fading = true;
        }
        const bool fillEye = eye != XR_EYE_VISIBILITY_BOTH && p->fillOn;
        if (p->curved && p->radius > 0.3f && app.hasCylinder && !p->hud && !fillEye) {
            XrCompositionLayerCylinderKHR &c = cyls[ncyl++];
            c = {XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
            c.next = chain;
            if (fading) c.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            c.space = app.space;
            c.eyeVisibility = eye;
            c.subImage.swapchain = sc;
            c.subImage.imageRect = {{0, 0}, {p->w, p->h}};
            const float ez[3] = {0, 0, 1};
            float n[3]; rotate(p->pose.orientation, ez, n);
            c.pose.orientation = p->pose.orientation;
            c.pose.position = {p->pose.position.x + n[0] * p->radius, p->pose.position.y + n[1] * p->radius, p->pose.position.z + n[2] * p->radius};
            c.radius = p->radius;
            c.centralAngle = p->wm * animS / p->radius;
            c.aspectRatio = p->wm / p->hm;
            layers[nLayers++] = (XrCompositionLayerBaseHeader *)&c;
        } else {
            XrCompositionLayerQuad &q = quads[nq++];
            q = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            q.next = chain;
            q.space = app.space;
            if (p->parent || fading || p->id == GAME_MENU_ID || p->id == LOADING_ID) q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;   // barra de botones y menú del juego: bordes redondos
            q.eyeVisibility = eye;
            q.subImage.swapchain = sc;
            q.subImage.imageRect = {{0, 0}, {p->w, p->h}};
            q.pose = p->pose;
            q.size = {p->wm * animS, p->hm * animS};
            if (fillEye) {   // solo el video, sin barras, ampliado alrededor de su centro
                const float ex[3] = {1, 0, 0}, ey[3] = {0, 1, 0};
                float ax[3], ay[3]; rotate(p->pose.orientation, ex, ax); rotate(p->pose.orientation, ey, ay);
                q.pose.position = {p->pose.position.x + ax[0] * p->fillOff[0] + ay[0] * p->fillOff[1],
                                   p->pose.position.y + ax[1] * p->fillOff[0] + ay[1] * p->fillOff[1],
                                   p->pose.position.z + ax[2] * p->fillOff[0] + ay[2] * p->fillOff[1]};
                q.size = {p->fillSize[0] * animS, p->fillSize[1] * animS};
            }
            if (p->hud) {
                // Indicador de rendimiento: sigue a la cabeza, arriba a la izquierda de la vista, mirando a tus ojos
                const float off[3] = {-0.30f, 0.17f, -1.0f};
                float w[3]; rotate(headPose.orientation, off, w);
                float pos[3] = {headPose.position.x + w[0], headPose.position.y + w[1], headPose.position.z + w[2]};
                float eye[3] = {headPose.position.x, headPose.position.y, headPose.position.z};
                q.pose.position = {pos[0], pos[1], pos[2]};
                q.pose.orientation = facing(pos, eye);
                q.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            }
            layers[nLayers++] = (XrCompositionLayerBaseHeader *)&q;
        }
    };
    {
        // El indicador de rendimiento se ve también durante un juego
        std::lock_guard<std::mutex> lk(g_uiLock);
        for (UiPanel *p : g_ui) {
            if (p->dead || !p->ready || !p->placed || nLayers >= MAXL - 3 || !panelShown(p)) continue;
            if (p->hud) continue;   // el indicador va al final, encima de todo
            if (!showPanels && !(g_inGameNow && panelInGame(p))) continue;   // en un juego: solo su menú y sus ventanas
            if (hiddenByLoading(p)) continue;
            if (update3D(app, p)) {
                // TV o ventana en 3D: una imagen distinta para cada ojo
                submit(p, p->sc3[0], XR_EYE_VISIBILITY_LEFT);
                submit(p, p->sc3[1], XR_EYE_VISIBILITY_RIGHT);
                continue;
            }
            submit(p, p->sc, XR_EYE_VISIBILITY_BOTH);
        }
        // Indicador de rendimiento: la última capa, así nada lo tapa (también durante un juego)
        if (valid && app.exitStart == 0)
            for (UiPanel *p : g_ui) if (p->hud && !p->dead && p->ready && nLayers < MAXL - 2) {
                submit(p, p->sc, XR_EYE_VISIBILITY_BOTH);
                static bool logged = false;
                if (!logged) { logged = true; sendLog("Indicador de rendimiento: visible"); }
            }
    }

    // Manos reales (passthrough proyectado sobre tus manos o alrededor de los controles): encima de las ventanas,
    // debajo del láser
    ctlModelsTry(app);
    portalUpdate();
    // Durante un juego, la cámara y las manos solo si se activaron las manos en el juego (experimental)
    const bool handsHere = showUi || (g_inGameNow && g_gameHands);
    camSetActive(!g_inGameNow || g_gameHands);
    if (!g_inGameNow || g_gameHands) camFrameUpdate(app, fs.predictedDisplayTime); else g_camMode = false;
    // Sala passthrough: tus manos y controles sobre las pantallas con el passthrough del propio visor (proyectado sobre
    // su forma), así calzan con tu mano real de al lado. Con la cámara de VRmin la imagen quedaba más chica y corrida.
    const bool ptRoom = g_env == 3 && !g_inGameNow && app.ptRunning && g_rh.ok;
    const bool useCam = g_camMode && !ptRoom;
    realHandsUpdate(app, fs.predictedDisplayTime, handsHere && valid && g_realHands, useCam, g_camT);
    XrCompositionLayerPassthroughFB ptHands{XR_TYPE_COMPOSITION_LAYER_PASSTHROUGH_FB};
    if (g_rh.ok && g_realHands && showUi && app.ptRunning && !useCam && nLayers < MAXL - 1 &&
        (g_rh.h[0].handShown || g_rh.h[1].handShown || g_rh.h[0].ctlShown || g_rh.h[1].ctlShown || g_rh.h[0].cmShown || g_rh.h[1].cmShown)) {
        ptHands.layerHandle = g_rh.layer;
        ptHands.flags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        layers[nLayers++] = (XrCompositionLayerBaseHeader *)&ptHands;
        g_rhSent++;
    }
    XrCompositionLayerProjection over{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
    XrCompositionLayerProjectionView ov[2];
    const bool overlayGame = g_inGameNow && valid && (g_gameMenu || (g_gameHands && g_camMode));
    if (((showUi && nq + ncyl > 0 && valid) || overlayGame) && app.overlay[0].handle && app.overlay[1].handle) {
        Ray rays[2];
        std::vector<Bar> bars;
        // en un juego, el láser solo con el menú del juego abierto (si no, los botones son del juego)
        const bool locked = g_inputLock && !g_inGameNow;   // (en pausa: ni láser, ni clics, ni moverse)
        int nr = (showUi || g_gameMenu) && !locked ? uiInput(app, fs.predictedDisplayTime, views[0], rays, bars) : 0;
        bool tp = showUi && !locked && teleportInput(app, fs.predictedDisplayTime, views[0]);
        for (int e = 0; e < 2; e++) {
            renderOverlay(app, e, views[e], rays, nr, bars, tp);
            ov[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            ov[e].pose = views[e].pose;
            ov[e].fov = views[e].fov;
            ov[e].subImage.swapchain = app.overlay[e].handle;
            ov[e].subImage.imageRect = {{0, 0}, {app.overlay[e].w, app.overlay[e].h}};
        }
        over.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
        over.space = app.space;
        over.viewCount = 2;
        over.views = ov;
        layers[nLayers++] = (XrCompositionLayerBaseHeader *)&over;
    }
    bool lobby = !g_gameActive;

    XrFrameEndInfo fe{XR_TYPE_FRAME_END_INFO};
    fe.displayTime = fs.predictedDisplayTime;
    fe.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    fe.layerCount = fs.shouldRender ? nLayers : 0;
    fe.layers = layers;
    xrEndFrame(app.session, &fe);
    app.rendered++;
    g_rendered++;

    double t = nowMs();
    if (t - app.lastStats > 5000) {
        double secs = app.lastStats > 0 ? (t - app.lastStats) / 1000.0 : 5.0;
        static uint32_t lastIn = 0, lastDec = 0, lastAud = 0;
        uint32_t in = g_framesIn, dec = g_framesDecoded, aud = g_audioChunks;
        static uint32_t lastDeskIn = 0, lastDeskDec = 0;
        uint32_t dIn = g_deskIn, dDec = g_deskDecoded;
        static uint32_t lastUnder = 0;
        const uint32_t under = g_aUnder;
        sendLog("App nativa: visor %.0f fps · video recibido %.0f/s · decodificado %.0f/s · sonido %s (cortes %u, colchón %.0f ms) · descartados %u · sala %s (lista %s, escritorio %.0f/%.0f por s)",
                app.rendered / secs, (in - lastIn) / secs, (dec - lastDec) / secs, aud != lastAud ? "OK" : "-", under - lastUnder, g_aTarget * 1000, (unsigned)g_framesDropped.load(),
                lobby ? "visible" : "oculta", app.listReady ? "OK" : "-", (dIn - lastDeskIn) / secs, (dDec - lastDeskDec) / secs);
        lastIn = in; lastDec = dec; lastAud = aud; lastDeskIn = dIn; lastDeskDec = dDec; lastUnder = under;
        {
            std::string ifn; { std::lock_guard<std::mutex> lk(g_usbLock); ifn = g_usbIf; }
            sendLog("Conexión de la sala VR: %s · red del cable USB: %s", g_connUsb ? "cable USB-C" : "Wi-Fi",
                    g_usbNet.load() ? (ifn.empty() ? "sí (sin nombre)" : ifn.c_str()) : "no (sin cable o sin permiso)");
        }
        sendLog("Manos reales: %s · ahora: modo %s · cámara: permiso %d, imágenes %d/%d, dibujadas %d · passthrough %s · mano izq. %s%s · mano der. %s%s · controles %s/%s · manos y controles a la vez %d · capa de Meta %d veces · error %d",
                g_rhStatus, g_camMode ? "cámara (bordes suaves)" : "respaldo de Meta", g_camAllowed.load(), g_cf[0].frames, g_cf[1].frames, g_camDrawn,
                app.ptRunning ? "andando" : "pausado",
                g_rh.h[0].active ? "seguida" : "no", (g_rh.h[0].handShown || g_rh.h[0].camDraw) ? "/visible" : "",
                g_rh.h[1].active ? "seguida" : "no", (g_rh.h[1].handShown || g_rh.h[1].camDraw) ? "/visible" : "",
                (g_rh.h[0].ctlShown || g_rh.h[0].cmShown || g_rh.h[0].ctlCamDraw) ? "visible" : "-", (g_rh.h[1].ctlShown || g_rh.h[1].cmShown || g_rh.h[1].ctlCamDraw) ? "visible" : "-",
                g_rh.simul, g_rhSent, g_rhErr);
        sendLog("Cámaras: %s · modelos de control %s/%s", g_camStatus, g_rh.h[0].cmLoaded ? "sí" : "no", g_rh.h[1].cmLoaded ? "sí" : "no");
        sendLog("Menú desde la mano: %s · abierto %d veces · mejor ángulo del borde %.2f (hace falta 0,35) · mejor centrado %.2f (hace falta 0,82)",
                g_watchOn ? "activado" : "apagado", g_palmOpens, g_palmBestFacing, g_palmBestCentered);
        g_palmBestFacing = -2; g_palmBestCentered = -2;
        g_rhSent = 0; g_camDrawn = 0; g_cf[0].frames = 0; g_cf[1].frames = 0;
        app.rendered = 0;
        app.lastStats = t;
    }
}

static void pollXrEvents(App &app) {
    XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(app.instance, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto *sc = (XrEventDataSessionStateChanged *)&ev;
            app.state = sc->state;
            if (g_vigia) sendLog("Modo Centinela: estado de la sesión del visor %d (5 = con foco; 4 = visible sin controles, p. ej. un aviso del sistema)", (int)sc->state);
            if (sc->state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XRCHK(xrBeginSession(app.session, &bi));
                app.sessionRunning = true;
                // Manos y controles a la vez (ver tus manos mientras sostenés los controles)
                if (false && app.hasSimul) {   // (o manos o controles: sin las dos cosas a la vez)
                    PFN_simulResume resume = nullptr;
                    xrGetInstanceProcAddr(app.instance, "xrResumeSimultaneousHandsAndControllersTrackingMETA", (PFN_xrVoidFunction *)&resume);
                    SimulResumeInfo ri{(XrStructureType)1000532002, nullptr};
                    XrResult r = resume ? resume(app.session, &ri) : XR_ERROR_FUNCTION_UNSUPPORTED;
                    g_rh.simul = XR_SUCCEEDED(r) ? 1 : (int)r;
                }
                app.appliedFps = 0;
            } else if (sc->state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(app.session);
                app.sessionRunning = false;
            } else if (sc->state == XR_SESSION_STATE_EXITING || sc->state == XR_SESSION_STATE_LOSS_PENDING) {
                app.exitRequested = true;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING) {
            // recentrado (botón de Meta): medir de nuevo la altura; en el Modo Centinela (visor apoyado) la altura no se
            // mide (es la elegida) y se recentra el juego hacia donde estás mirando
            if (g_vigia) { g_vg.needFrame = true; vigiaSay("recentrado"); }
            else { g_heightDirty = true; g_recenterIn = 6; }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            app.exitRequested = true;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

static void onAppCmd(android_app *a, int32_t cmd) {
    App &app = *(App *)a->userData;
    if (cmd == APP_CMD_RESUME) app.resumed = true;
    if (cmd == APP_CMD_PAUSE) app.resumed = false;
    if (cmd == APP_CMD_DESTROY) app.exitRequested = true;
}

/// Vuelve a la ventana normal del Quest (lista de juegos y escritorio).
static void openWindow2D(android_app *a, JNIEnv *env) {
    jobject act = a->activity->clazz;
    jclass ic = env->FindClass("android/content/Intent");
    jobject intent = env->NewObject(ic, env->GetMethodID(ic, "<init>", "()V"));
    env->CallObjectMethod(intent, env->GetMethodID(ic, "setClassName", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;"),
                          env->NewStringUTF("org.questvrbridge.client"), env->NewStringUTF("org.questvrbridge.client.MainActivity"));
    env->CallObjectMethod(intent, env->GetMethodID(ic, "addFlags", "(I)Landroid/content/Intent;"), (jint)0x10000000);  // NEW_TASK
    jclass ac = env->GetObjectClass(act);
    env->CallVoidMethod(act, env->GetMethodID(ac, "startActivity", "(Landroid/content/Intent;)V"), intent);
    if (env->ExceptionCheck()) { env->ExceptionClear(); LOGE("No pude abrir la ventana 2D"); }
}

/// Sin este "candado", el Wi-Fi del Quest descarta los avisos (broadcast) de la Mac para ahorrar batería.
static jobject g_multicastLock = nullptr;
static void acquireMulticastLock(android_app *a, JNIEnv *env) {
    jobject act = a->activity->clazz;
    jclass ctxCls = env->FindClass("android/content/Context");
    jmethodID getSys = ctxCls ? env->GetMethodID(ctxCls, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;") : nullptr;
    jobject wifi = getSys ? env->CallObjectMethod(act, getSys, env->NewStringUTF("wifi")) : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!wifi) { LOGE("Sin WifiManager"); return; }
    jclass wmCls = env->GetObjectClass(wifi);
    jmethodID create = env->GetMethodID(wmCls, "createMulticastLock", "(Ljava/lang/String;)Landroid/net/wifi/WifiManager$MulticastLock;");
    jobject lock = create ? env->CallObjectMethod(wifi, create, env->NewStringUTF("questvrbridge")) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); lock = nullptr; }
    if (!lock) { LOGE("No se pudo crear el MulticastLock"); return; }
    jclass lCls = env->GetObjectClass(lock);
    jmethodID acq = env->GetMethodID(lCls, "acquire", "()V");
    if (acq) env->CallVoidMethod(lock, acq);
    if (env->ExceptionCheck()) { env->ExceptionClear(); LOGE("No se pudo activar el MulticastLock"); return; }
    g_multicastLock = env->NewGlobalRef(lock);
    LOGI("MulticastLock activo: se reciben los avisos de la Mac");
}

void android_main(android_app *a) {
    App app;
    app.android = a;
    a->userData = &app;
    a->onAppCmd = onAppCmd;

    JNIEnv *env = nullptr;
    a->activity->vm->AttachCurrentThread(&env, nullptr);
    acquireMulticastLock(a, env);
    // Métodos de VrActivity (Java) que se llaman desde acá
    g_vm = a->activity->vm;
    g_act = env->NewGlobalRef(a->activity->clazz);
    jclass ac = env->GetObjectClass(g_act);
    g_jPacket = env->GetMethodID(ac, "onUiPacket", "([B)V");
    g_jConn = env->GetMethodID(ac, "onNativeConnection", "(ZLjava/lang/String;)V");
    g_jPointer = env->GetMethodID(ac, "onPointer", "(IIFFF)V");
    g_jMacs = env->GetMethodID(ac, "onMacs", "(Ljava/lang/String;)V");
    g_jWatch = env->GetMethodID(ac, "onWatch", "()V");
    g_jPortalDone = env->GetMethodID(ac, "onPortalDone", "()V");
    g_jAuth = env->GetMethodID(ac, "authFor", "(Ljava/lang/String;)Ljava/lang/String;");
    g_jGameState = env->GetMethodID(ac, "onGameState", "(Z)V");
    g_jGameMenu = env->GetMethodID(ac, "onGameMenu", "(Z)V");
    g_jInputLock = env->GetMethodID(ac, "onInputLock", "(Z)V");
    if (env->ExceptionCheck()) { env->ExceptionClear(); g_jPacket = g_jConn = g_jPointer = nullptr; LOGE("Sin VrActivity: la sala no tendrá menú"); }

    initEGL(app);
    initGL(app);
    if (!initXR(app)) { LOGE("No se pudo iniciar OpenXR"); ANativeActivity_finish(a->activity); return; }

    initRoom(app);
    initHands(app);
    init3D(app);
    initReveal(app);
    std::thread net(networkLoop);
    std::thread dec(decoderOutputLoop);

    bool gameSeen = false, back2D = false;
    double lobbySince = 0;
    while (!a->destroyRequested && !app.exitRequested) {
        int events = 0;
        android_poll_source *src = nullptr;
        int timeout = app.sessionRunning ? 0 : 100;
        while (ALooper_pollOnce(timeout, nullptr, &events, (void **)&src) >= 0) {
            if (src) src->process(a, src);
            if (a->destroyRequested) break;
            timeout = 0;
        }
        pollXrEvents(app);
        if (app.sessionRunning) renderFrame(app);

        // Cuando se cierra el juego (la Mac vuelve a mostrar la sala por unos segundos), volver a la ventana
        if (g_exitVR) {
            // Primero el ambiente se "vacía" de a poco (2 segundos) y después se vuelve a la ventana
            if (app.exitStart == 0) { app.exitStart = nowMs(); sendLog("Saliendo del modo VR: vuelvo a la ventana del Quest"); }
            if (nowMs() - app.exitStart > 2050 || !app.sessionRunning) { back2D = true; break; }
        }
        int ps = g_panelState;
        if (ps == 2) gameSeen = true;
        if (ps == 1 && gameSeen && g_sock >= 0) {
            if (lobbySince == 0) lobbySince = nowMs();
            else if (nowMs() - lobbySince > 6000) { back2D = true; sendLog("El juego se cerró: vuelvo a la ventana del Quest"); break; }
        } else lobbySince = 0;
    }

    g_running = false;
    int fd = g_sock.exchange(-1);
    if (fd >= 0) shutdown(fd, SHUT_RDWR);
    net.detach();
    dec.join();
    decoderDestroy();
    if (app.session) xrDestroySession(app.session);
    if (app.instance) xrDestroyInstance(app.instance);
    if (back2D) openWindow2D(a, env);
    ANativeActivity_finish(a->activity);
    // La parte VR vive en su propio proceso: terminarlo para que la próxima vez arranque de cero
    usleep(300 * 1000);
    _exit(0);
}
