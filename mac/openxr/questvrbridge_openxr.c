/*
 * QuestVRBridge OpenXR runtime (Windows x64, D3D11, Vulkan y OpenGL).
 *
 * Se carga dentro del "Windows" de GameHub (Wine) en lugar de SteamVR/Oculus.
 * - Recibe la pose de la cabeza y los controles del Quest a través de
 *   QuestRenderer (Mac), por TCP en 127.0.0.1:9002.
 * - Copia las imágenes de cada ojo que dibuja el juego y se las manda a
 *   QuestRenderer, que las comprime y las envía al visor.
 *
 * Compilar (desde Linux/Mac con mingw-w64; cabeceras de OpenXR-SDK 1.1.x, por ejemplo release-1.1.43,
 * y de Vulkan-Headers, por ejemplo v1.3.296):
 *   x86_64-w64-mingw32-gcc -O2 -shared -o questvrbridge_openxr.dll questvrbridge_openxr.c questvrbridge_openxr.def \
 *       -I<OpenXR-SDK>/include -I<Vulkan-Headers>/include -ld3d11 -ldxgi -ldxguid -luuid -lws2_32 -lwinmm -static-libgcc -Wl,--kill-at
 * (GitHub Actions lo compila solo: «Runtime OpenXR (Windows)».)
 */
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#define _USE_MATH_DEFINES
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <unknwn.h>
#include <d3d11.h>
#include <d3d11_4.h>
#include <dxgi.h>
#include <mmsystem.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>
#include <math.h>

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <GL/gl.h>
#include <GL/glext.h>
#include <GL/wglext.h>

#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#define XR_USE_GRAPHICS_API_VULKAN
#define XR_USE_GRAPHICS_API_OPENGL
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include <openxr/openxr_reflection.h>

#define EXPORT __declspec(dllexport)
#define RT_NAME "QuestVRBridge"
#define BRIDGE_PORT 9002

/* ------------------------------------------------------------------ */
/* Logging                                                             */
/* ------------------------------------------------------------------ */
static CRITICAL_SECTION g_lock;      /* estado de tracking, eventos, acciones */
static volatile LONG g_resetImages;  /* pedido del hilo de red: rearmar la conexión de imágenes */
static CRITICAL_SECTION g_sendlock;  /* socket */
static FILE *g_logf;
static SOCKET g_sock = INVALID_SOCKET;
static void send_packet(uint8_t type, const void *a, uint32_t alen, const void *b, uint32_t blen);

static void logmsg(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof buf, fmt, ap);
    va_end(ap);
    if (g_logf) { fprintf(g_logf, "%s\n", buf); fflush(g_logf); }
    send_packet(3, buf, (uint32_t)strlen(buf), NULL, 0);
}

/* ------------------------------------------------------------------ */
/* Tiempo                                                              */
/* ------------------------------------------------------------------ */
static LARGE_INTEGER g_qpf;
static int64_t qpc_to_ns(int64_t c) {
    return (int64_t)((double)c * 1e9 / (double)g_qpf.QuadPart);
}
static int64_t now_ns(void) {
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return qpc_to_ns(c.QuadPart);
}

/* ------------------------------------------------------------------ */
/* Matemática de poses                                                 */
/* ------------------------------------------------------------------ */
static XrQuaternionf qmul(XrQuaternionf a, XrQuaternionf b) {
    XrQuaternionf r;
    r.w = a.w*b.w - a.x*b.x - a.y*b.y - a.z*b.z;
    r.x = a.w*b.x + a.x*b.w + a.y*b.z - a.z*b.y;
    r.y = a.w*b.y - a.x*b.z + a.y*b.w + a.z*b.x;
    r.z = a.w*b.z + a.x*b.y - a.y*b.x + a.z*b.w;
    return r;
}
static XrQuaternionf qconj(XrQuaternionf q) { XrQuaternionf r = { -q.x, -q.y, -q.z, q.w }; return r; }
static XrVector3f qrot(XrQuaternionf q, XrVector3f v) {
    XrQuaternionf p = { v.x, v.y, v.z, 0 };
    XrQuaternionf r = qmul(qmul(q, p), qconj(q));
    XrVector3f o = { r.x, r.y, r.z };
    return o;
}
static XrPosef pmul(XrPosef a, XrPosef b) { /* a ∘ b */
    XrPosef r;
    XrVector3f t = qrot(a.orientation, b.position);
    r.position.x = a.position.x + t.x; r.position.y = a.position.y + t.y; r.position.z = a.position.z + t.z;
    r.orientation = qmul(a.orientation, b.orientation);
    return r;
}
static XrPosef pinv(XrPosef a) {
    XrPosef r;
    r.orientation = qconj(a.orientation);
    XrVector3f t = qrot(r.orientation, a.position);
    r.position.x = -t.x; r.position.y = -t.y; r.position.z = -t.z;
    return r;
}
static XrPosef pident(void) { XrPosef p = { {0,0,0,1}, {0,0,0} }; return p; }

/* ------------------------------------------------------------------ */
/* Estado de tracking que llega del Quest                              */
/* ------------------------------------------------------------------ */
enum { IN_TRIGGER, IN_SQUEEZE, IN_TX, IN_TY, IN_TCLICK, IN_A, IN_B,
       IN_TTOUCH, IN_THTOUCH, IN_ATOUCH, IN_BTOUCH, IN_MENU, IN_COUNT };
typedef struct {
    int valid;
    uint32_t id;
    XrPosef eye[2];
    XrFovf fov[2];
    int handValid[2];
    XrPosef grip[2], aim[2];
    float in[2][IN_COUNT];
    XrVector3f lin[3], ang[3];   /* velocidades en el mundo: 0 = mano izq, 1 = mano der, 2 = cabeza */
    double tms;                  /* tiempo del Quest (ms) */
    int64_t rx;                  /* cuándo llegó (reloj de este runtime, ns) */
} Track;
static Track g_track;            /* protegido por g_lock */
static int g_eyeW = 1024, g_eyeH = 1024, g_fps = 72;
/* Ajustes avanzados por juego (los manda la Mac al saber qué juego es, en la línea "config" con "pv"):
 * campo de visión recortado (las esquinas casi no se ven por las lentes) y emular controles de Valve Index */
static volatile float g_fovK = 1.0f;
static volatile int g_index = 0;
static volatile int g_vigia = 0;   /* Modo Centinela: el cuadro se ve en la Mac sin reproyección → adelantar las poses */
static volatile LONG g_cfgVer = 0, g_helloVer = -1;
static char g_appName[128];
static int g_connected = 0;

static XrPosef head_pose(const Track *t) {
    XrPosef h = t->eye[0];
    h.position.x = (t->eye[0].position.x + t->eye[1].position.x) * 0.5f;
    h.position.y = (t->eye[0].position.y + t->eye[1].position.y) * 0.5f;
    h.position.z = (t->eye[0].position.z + t->eye[1].position.z) * 0.5f;
    return h;
}

/* Modo Centinela: adelanta cabeza y controles desde que llegó la pose hasta «time» (cuando el juego dice que se va a
 * ver el cuadro), con la velocidad medida. Sin esto, lo que se ve en la Mac va siempre un poco atrasado y a saltos. */
static void predict(Track *t, XrTime time) {
    if (!g_vigia || !t->rx || !time) return;
    double dt = (double)(time - t->rx) / 1e9;
    if (dt <= 0) return;
    if (dt > 0.05) dt = 0.05;   /* nunca más de 50 ms (si no, una pausa de la red lo dispara) */
    for (int k = 0; k < 3; k++) {
        if (k < 2 ? !t->handValid[k] : !t->valid) continue;
        XrVector3f w = t->ang[k];
        float a = sqrtf(w.x * w.x + w.y * w.y + w.z * w.z) * (float)dt;
        XrQuaternionf dq = { 0, 0, 0, 1 };
        if (a > 1e-6f) {
            float sn = sinf(a * 0.5f) / (a / (float)dt);
            dq.x = w.x * sn; dq.y = w.y * sn; dq.z = w.z * sn; dq.w = cosf(a * 0.5f);
        }
        XrVector3f d = { t->lin[k].x * (float)dt, t->lin[k].y * (float)dt, t->lin[k].z * (float)dt };
        if (k < 2) {
            XrPosef *ps[2] = { &t->grip[k], &t->aim[k] };
            for (int j = 0; j < 2; j++) {
                ps[j]->position.x += d.x; ps[j]->position.y += d.y; ps[j]->position.z += d.z;
                ps[j]->orientation = qmul(dq, ps[j]->orientation);
            }
        } else {   /* los dos ojos giran alrededor del centro de la cabeza */
            XrPosef h = head_pose(t);
            for (int e = 0; e < 2; e++) {
                XrVector3f off = { t->eye[e].position.x - h.position.x, t->eye[e].position.y - h.position.y, t->eye[e].position.z - h.position.z };
                off = qrot(dq, off);
                t->eye[e].position.x = h.position.x + d.x + off.x;
                t->eye[e].position.y = h.position.y + d.y + off.y;
                t->eye[e].position.z = h.position.z + d.z + off.z;
                t->eye[e].orientation = qmul(dq, t->eye[e].orientation);
            }
        }
    }
}

/* Formato de la línea "pose": ... "id":N, ... "xr":[77 números] ... */
static void parse_pose_line(const char *line) {
    const char *p = strstr(line, "\"xr\":");
    if (!p) return;
    p += 5;
    while (*p == ' ') p++;
    if (*p != '[') return;
    p++;
    double v[96]; int n = 0;
    while (n < 96 && *p && *p != ']') {
        char *end;
        double d = strtod(p, &end);
        if (end == p) { p++; continue; }
        v[n++] = d; p = end;
        while (*p == ',' || *p == ' ') p++;
    }
    if (n < 77) return;
    for (int i = 0; i < 77; i++) if (!isfinite(v[i]) || fabs(v[i]) > 1000.0) return;  /* datos inválidos: descartar */
    uint32_t id = 0;
    const char *q = strstr(line, "\"id\":");
    if (q) id = (uint32_t)strtoul(q + 5, NULL, 10);

    Track t; memset(&t, 0, sizeof t);
    int k = 0;
    t.valid = v[k++] > 0.5;
    for (int e = 0; e < 2; e++) {
        t.eye[e].position.x = (float)v[k++]; t.eye[e].position.y = (float)v[k++]; t.eye[e].position.z = (float)v[k++];
        t.eye[e].orientation.x = (float)v[k++]; t.eye[e].orientation.y = (float)v[k++];
        t.eye[e].orientation.z = (float)v[k++]; t.eye[e].orientation.w = (float)v[k++];
        t.fov[e].angleLeft = (float)atan(v[k++]); t.fov[e].angleRight = (float)atan(v[k++]);
        t.fov[e].angleDown = (float)atan(v[k++]); t.fov[e].angleUp = (float)atan(v[k++]);
    }
    for (int h = 0; h < 2; h++) {
        t.handValid[h] = v[k++] > 0.5;
        XrPosef *ps[2] = { &t.grip[h], &t.aim[h] };
        for (int j = 0; j < 2; j++) {
            ps[j]->position.x = (float)v[k++]; ps[j]->position.y = (float)v[k++]; ps[j]->position.z = (float)v[k++];
            ps[j]->orientation.x = (float)v[k++]; ps[j]->orientation.y = (float)v[k++];
            ps[j]->orientation.z = (float)v[k++]; ps[j]->orientation.w = (float)v[k++];
        }
        for (int i = 0; i < IN_COUNT; i++) t.in[h][i] = (float)v[k++];
    }
    /* normalizar cuaterniones (por si llegan levemente desnormalizados) */
    XrQuaternionf *qs[6] = { &t.eye[0].orientation, &t.eye[1].orientation, &t.grip[0].orientation, &t.grip[1].orientation, &t.aim[0].orientation, &t.aim[1].orientation };
    for (int i = 0; i < 6; i++) {
        float m = sqrtf(qs[i]->x * qs[i]->x + qs[i]->y * qs[i]->y + qs[i]->z * qs[i]->z + qs[i]->w * qs[i]->w);
        if (m < 1e-4f) { qs[i]->x = qs[i]->y = qs[i]->z = 0; qs[i]->w = 1; }
        else { qs[i]->x /= m; qs[i]->y /= m; qs[i]->z /= m; qs[i]->w /= m; }
    }
    t.id = id;
    const char *tq = strstr(line, "\"t\":");
    t.tms = tq ? strtod(tq + 4, NULL) : 0;
    t.rx = now_ns();
    EnterCriticalSection(&g_lock);
    {
        /* Velocidad por diferencia entre poses consecutivas, suavizada.
         * Superhot detecta los golpes por la velocidad de la mano. */
        const Track *o = &g_track;
        double dtms = t.tms - o->tms;
        XrPosef now3[3] = { t.grip[0], t.grip[1], head_pose(&t) };
        XrPosef old3[3] = { o->grip[0], o->grip[1], head_pose(o) };
        int okNow[3] = { t.handValid[0], t.handValid[1], t.valid };
        int okOld[3] = { o->handValid[0], o->handValid[1], o->valid };
        for (int k = 0; k < 3; k++) {
            XrVector3f lv = { 0, 0, 0 }, av = { 0, 0, 0 };
            if (o->tms > 0 && dtms > 1.0 && dtms < 100.0 && okNow[k] && okOld[k]) {
                float dt = (float)(dtms / 1000.0);
                lv.x = (now3[k].position.x - old3[k].position.x) / dt;
                lv.y = (now3[k].position.y - old3[k].position.y) / dt;
                lv.z = (now3[k].position.z - old3[k].position.z) / dt;
                XrQuaternionf dq = qmul(now3[k].orientation, qconj(old3[k].orientation));
                if (dq.w < 0) { dq.x = -dq.x; dq.y = -dq.y; dq.z = -dq.z; dq.w = -dq.w; }
                float sn = sqrtf(dq.x * dq.x + dq.y * dq.y + dq.z * dq.z);
                if (sn > 1e-6f) {
                    float ang = 2.0f * atan2f(sn, dq.w) / dt;
                    av.x = dq.x / sn * ang; av.y = dq.y / sn * ang; av.z = dq.z / sn * ang;
                }
                const float a = 0.6f;   /* suavizado: 60 % nuevo, 40 % anterior */
                lv.x = a * lv.x + (1 - a) * o->lin[k].x; lv.y = a * lv.y + (1 - a) * o->lin[k].y; lv.z = a * lv.z + (1 - a) * o->lin[k].z;
                av.x = a * av.x + (1 - a) * o->ang[k].x; av.y = a * av.y + (1 - a) * o->ang[k].y; av.z = a * av.z + (1 - a) * o->ang[k].z;
            } else if (o->tms > 0 && dtms <= 1.0) {
                lv = o->lin[k]; av = o->ang[k];   /* pose repetida: mantener la última velocidad */
            }
            t.lin[k] = lv; t.ang[k] = av;
        }
    }
    g_track = t;
    LeaveCriticalSection(&g_lock);
}

static void parse_config_line(const char *line) {
    const char *p;
    if ((p = strstr(line, "\"eye\":"))) { int v = atoi(p + 6 + (p[6] == ' ')); if (v >= 256 && v <= 4096) g_eyeW = g_eyeH = v; }
    if ((p = strstr(line, "\"eyeH\":"))) { int v = atoi(p + 7); if (v >= 256 && v <= 4096) g_eyeH = v; }
    if ((p = strstr(line, "\"fps\":"))) { int v = atoi(p + 6 + (p[6] == ' ')); if (v >= 30 && v <= 144) g_fps = v; }
    if ((p = strstr(line, "\"fovk\":"))) { double v = atof(p + 7); if (v >= 0.6 && v <= 1.0) g_fovK = (float)v; }
    if ((p = strstr(line, "\"index\":"))) g_index = atoi(p + 8) != 0;
    if ((p = strstr(line, "\"vg\":"))) g_vigia = atoi(p + 5) != 0;
    if (strstr(line, "\"pv\":")) {
        InterlockedIncrement(&g_cfgVer);
        logmsg("Ajustes del juego: %dx%d por ojo, campo de visión %.0f%%%s", g_eyeW, g_eyeH, g_fovK * 100.0, g_index ? ", controles de Index" : "");
    }
}

/* ------------------------------------------------------------------ */
/* Red (hilo de recepción + envío)                                     */
/* ------------------------------------------------------------------ */
static int send_all(SOCKET s, const char *p, int n) {
    while (n > 0) {
        int chunk = n > (1 << 20) ? (1 << 20) : n;   /* de a 1 MB */
        int r = send(s, p, chunk, 0);
        if (r <= 0) {
            int e = WSAGetLastError();
            if (e == WSAEWOULDBLOCK || e == WSAEINTR) { Sleep(1); continue; }
            if (g_logf) { fprintf(g_logf, "send falló (error %d)\n", e); fflush(g_logf); }
            return 0;
        }
        p += r; n -= r;
    }
    return 1;
}

static void send_packet(uint8_t type, const void *a, uint32_t alen, const void *b, uint32_t blen) {
    if (g_sock == INVALID_SOCKET) return;
    if (type == 3 || type == 7) {
        /* Los mensajes de log nunca esperan: si la conexión está ocupada, solo van al archivo */
        if (!TryEnterCriticalSection(&g_sendlock)) return;
    } else {
        EnterCriticalSection(&g_sendlock);
    }
    if (g_sock != INVALID_SOCKET) {
        uint32_t len = 1 + alen + blen;
        uint8_t hdr[5] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len, type };
        if (!send_all(g_sock, (const char *)hdr, 5) ||
            (alen && !send_all(g_sock, (const char *)a, (int)alen)) ||
            (blen && !send_all(g_sock, (const char *)b, (int)blen))) {
            shutdown(g_sock, SD_BOTH);      /* el hilo de red lo cierra cuando su recv falle */
            g_sock = INVALID_SOCKET;
        }
    }
    LeaveCriticalSection(&g_sendlock);
}

static DWORD WINAPI net_thread(LPVOID arg) {
    (void)arg;
    WSADATA wsa; WSAStartup(MAKEWORD(2, 2), &wsa);
    char *buf = (char *)malloc(1 << 16);
    for (;;) {
        SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        struct sockaddr_in a; memset(&a, 0, sizeof a);
        a.sin_family = AF_INET; a.sin_port = htons(BRIDGE_PORT);
        a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(s); Sleep(1000); continue; }
        int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
        int sz = 16 << 20; setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&sz, sizeof sz);
        EnterCriticalSection(&g_sendlock); g_sock = s; LeaveCriticalSection(&g_sendlock);
        g_connected = 1;
        g_resetImages = 1;   /* la Mac pudo haberse reiniciado: la conexión de imágenes (y la memoria compartida) se arman de nuevo */
        logmsg("Runtime OpenXR conectado a QuestRenderer");
        if (g_appName[0]) { g_helloVer = g_cfgVer; send_packet(9, g_appName, (uint32_t)strlen(g_appName), NULL, 0); }   /* qué juego es: la Mac responde con sus ajustes */
        int used = 0;
        for (;;) {
            int r = recv(s, buf + used, (1 << 16) - 1 - used, 0);
            if (r <= 0) break;
            used += r;
            buf[used] = 0;
            char *start = buf, *nl;
            while ((nl = strchr(start, '\n'))) {
                *nl = 0;
                if (strstr(start, "\"resetimg\"")) { g_resetImages = 1; logmsg("La Mac pidió rearmar la imagen (volver al juego)"); }
                else if (strstr(start, "\"xr\":")) parse_pose_line(start);
                else if (strstr(start, "\"config\"")) parse_config_line(start);
                start = nl + 1;
            }
            used = (int)(buf + used - start);
            memmove(buf, start, used);
            if (used >= (1 << 16) - 1) used = 0;
        }
        g_connected = 0;
        EnterCriticalSection(&g_sendlock);
        g_sock = INVALID_SOCKET;
        LeaveCriticalSection(&g_sendlock);
        closesocket(s);                     /* solo este hilo cierra su socket */
        EnterCriticalSection(&g_lock); g_track.valid = 0; LeaveCriticalSection(&g_lock);
        Sleep(1000);
    }
    return 0;
}

/* Hilo de envío de cuadros: el hilo de dibujo deja el cuadro y sigue. */
static HANDLE g_frameEvent;
static uint8_t *g_frameBuf[2];
static size_t g_frameCap;
static int g_frameReady = -1;       /* índice del buffer listo para enviar */
static int g_frameSending = -1;
static uint8_t g_frameHdr[2][9];
static uint32_t g_frameLen[2];
static volatile LONG g_statSubmitted, g_statSent, g_statBusy;
static volatile LONG g_hookHits;
static volatile const char *g_inside = "-";
static volatile int64_t g_lastEndFrame;
static volatile LONG g_skipNoSock, g_skipFmt, g_mapFail, g_noProj, g_layers; static HRESULT g_lastMapHr;
static int64_t g_statT0;
/* Rendimiento (para el indicador del visor): cuánto espera el juego, cuánto tarda en dibujar y la copia de la imagen */
static int64_t g_waitEnd, g_perfT0;
static double g_sumWait, g_sumGame, g_sumCopy, g_sumMap; static long g_nWait, g_nGame, g_nCopy;
static volatile LONG g_perfFrames, g_perfSent, g_perfBusy;

/* Conexión SOLO para imágenes (separada de la de poses: en algunos Wine,
 * mandar un bloque grande y recibir por el mismo socket se traban entre sí). */
static SOCKET g_fsock = INVALID_SOCKET;
static int64_t g_fsockRetry;
static int g_fsockGen;              /* cuenta las conexiones de imágenes (el número de socket se puede repetir) */
static int64_t g_shmFullSince;      /* desde cuándo están todos los lugares ocupados (si dura, la Mac se fue) */

static int frame_sock_ready(void) {
    if (g_fsock != INVALID_SOCKET) return 1;
    if (now_ns() < g_fsockRetry) return 0;
    g_fsockRetry = now_ns() + 1000000000LL;
    SOCKET s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    struct sockaddr_in a; memset(&a, 0, sizeof a);
    a.sin_family = AF_INET; a.sin_port = htons(BRIDGE_PORT);
    a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(s, (struct sockaddr *)&a, sizeof a) != 0) { closesocket(s); return 0; }
    int one = 1; setsockopt(s, IPPROTO_TCP, TCP_NODELAY, (const char *)&one, sizeof one);
    int sz = 16 << 20; setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const char *)&sz, sizeof sz);
    uint8_t hello[6] = { 0, 0, 0, 2, 4, 'F' };   /* paquete tipo 4: "esta conexión es de imágenes" */
    if (!send_all(s, (const char *)hello, 6)) { closesocket(s); return 0; }
    g_fsock = s;
    g_fsockGen++;
    logmsg("Conexión de imágenes abierta");
    return 1;
}

static void send_frame(int idx) {
    if (!frame_sock_ready()) return;
    uint32_t len = 1 + 9 + g_frameLen[idx];
    uint8_t hdr[5] = { (uint8_t)(len >> 24), (uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len, 2 };
    if (!send_all(g_fsock, (const char *)hdr, 5) ||
        !send_all(g_fsock, (const char *)g_frameHdr[idx], 9) ||
        !send_all(g_fsock, (const char *)g_frameBuf[idx], (int)g_frameLen[idx])) {
        closesocket(g_fsock);
        g_fsock = INVALID_SOCKET;
        logmsg("Se cortó la conexión de imágenes; reintentando");
        return;
    }
    InterlockedIncrement(&g_statSent);
}

/* ------------------------------------------------------------------ */
/* Memoria compartida con QuestRenderer (la Mac)                       */
/* Mandar 13 MB por cuadro por TCP dentro de Wine da ~25-30 cuadros/s.  */
/* En cambio, el cuadro se escribe en un archivo mapeado en memoria que */
/* el renderizador (programa nativo de la Mac) mapea también: por TCP   */
/* solo va un aviso chico. Si la Mac no confirma, se sigue por TCP.     */
/* ------------------------------------------------------------------ */
#define SHM_MAGIC 0x31535256u   /* "VRS1" */
#define SHM_SLOTS 3
#define STAGES 2       /* texturas de lectura: se lee la del cuadro anterior (3 sumaba un cuadro más de demora) */
#define SHM_HDR 64
#define SHM_SLOT_MAX ((size_t)2560 * 2 * 2560 * 4)   /* hasta 2560 por ojo */
typedef struct {
    volatile uint32_t magic, nonce, ack, nslots, slotSize;
    uint32_t reserved[3];
    volatile LONG state[SHM_SLOTS];   /* 0 libre · 2 escribiendo · 1 listo para la Mac */
} ShmHdr;
static HANDLE g_shmFile = INVALID_HANDLE_VALUE, g_shmMap;
static uint8_t *g_shm;
static int g_shmMode;               /* 0 esperando confirmación · 1 activa · -1 no disponible (TCP) */
static int64_t g_shmAsked;
static int g_shmGen = -1;           /* conexión de imágenes donde se ofreció */
static volatile LONG g_shmSentStat;

static int shm_create(void) {
    if (g_shm) return 1;
    const wchar_t *path = L"C:\\questvrbridge\\cuadros.bin";
    CreateDirectoryW(L"C:\\questvrbridge", NULL);
    g_shmFile = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                            NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (g_shmFile == INVALID_HANDLE_VALUE) { logmsg("Memoria compartida: no se pudo crear el archivo (%lu)", GetLastError()); return 0; }
    uint64_t total = SHM_HDR + SHM_SLOT_MAX * SHM_SLOTS;
    g_shmMap = CreateFileMappingW(g_shmFile, NULL, PAGE_READWRITE, (DWORD)(total >> 32), (DWORD)total, NULL);
    if (!g_shmMap) { logmsg("Memoria compartida: CreateFileMapping falló (%lu)", GetLastError()); CloseHandle(g_shmFile); g_shmFile = INVALID_HANDLE_VALUE; return 0; }
    g_shm = (uint8_t *)MapViewOfFile(g_shmMap, FILE_MAP_WRITE | FILE_MAP_READ, 0, 0, (SIZE_T)total);
    if (!g_shm) { logmsg("Memoria compartida: MapViewOfFile falló (%lu)", GetLastError()); CloseHandle(g_shmMap); CloseHandle(g_shmFile); g_shmFile = INVALID_HANDLE_VALUE; return 0; }
    return 1;
}

/* Ofrece la memoria compartida por la conexión de imágenes (paquete tipo 5: nonce, tamaño de cada lugar, ruta Unix) */
static void shm_offer(void) {
    if (!shm_create()) { g_shmMode = -1; return; }
    ShmHdr *h = (ShmHdr *)g_shm;
    h->magic = SHM_MAGIC; h->nslots = SHM_SLOTS; h->slotSize = (uint32_t)SHM_SLOT_MAX; h->ack = 0;
    for (int i = 0; i < SHM_SLOTS; i++) h->state[i] = 0;
    uint32_t nonce = (uint32_t)GetTickCount() ^ ((uint32_t)GetCurrentProcessId() << 16) ^ 0x5a5a1234u;
    if (!nonce) nonce = 1;
    MemoryBarrier();
    h->nonce = nonce;
    char unixPath[1024] = "";
    typedef char *(CDECL *wgufn_t)(LPCWSTR);
    wgufn_t wgufn = (wgufn_t)GetProcAddress(GetModuleHandleA("kernel32.dll"), "wine_get_unix_file_name");
    if (wgufn) {
        char *u = wgufn(L"C:\\questvrbridge\\cuadros.bin");
        if (u) { strncpy(unixPath, u, sizeof unixPath - 1); HeapFree(GetProcessHeap(), 0, u); }
    }
    if (!unixPath[0]) { logmsg("Memoria compartida: no pude saber la ruta en la Mac; sigo por la red"); g_shmMode = -1; return; }
    uint8_t pkt[5 + 1 + 8 + 1024];
    uint32_t plen = (uint32_t)strlen(unixPath);
    uint32_t len = 1 + 8 + plen;
    pkt[0] = (uint8_t)(len >> 24); pkt[1] = (uint8_t)(len >> 16); pkt[2] = (uint8_t)(len >> 8); pkt[3] = (uint8_t)len; pkt[4] = 5;
    pkt[5] = (uint8_t)(nonce >> 24); pkt[6] = (uint8_t)(nonce >> 16); pkt[7] = (uint8_t)(nonce >> 8); pkt[8] = (uint8_t)nonce;
    uint32_t ss = (uint32_t)SHM_SLOT_MAX;
    pkt[9] = (uint8_t)(ss >> 24); pkt[10] = (uint8_t)(ss >> 16); pkt[11] = (uint8_t)(ss >> 8); pkt[12] = (uint8_t)ss;
    memcpy(pkt + 13, unixPath, plen);
    if (!send_all(g_fsock, (const char *)pkt, (int)(4 + len))) {   /* 4 de largo + el paquete (tipo incluido) */ closesocket(g_fsock); g_fsock = INVALID_SOCKET; return; }
    g_shmGen = g_fsockGen; g_shmMode = 0; g_shmAsked = now_ns();
    logmsg("Memoria compartida ofrecida a la Mac (%s)", unixPath);
}

/* Devuelve 1 si hay que usar la memoria compartida para este cuadro */
static int shm_active(void) {
    /* La Mac se reconectó (por ejemplo, al cambiar la calidad se reinicia): cerrar la conexión de imágenes vieja */
    if (InterlockedExchange(&g_resetImages, 0) && g_fsock != INVALID_SOCKET && g_shmMode >= 0) {
        closesocket(g_fsock); g_fsock = INVALID_SOCKET; g_fsockRetry = 0; g_shmMode = 0;
        logmsg("La Mac se reconectó: rearmando la conexión de imágenes");
    }
    if (g_shmMode < 0) return 0;   /* (la Mac no la soporta: la conexión queda para el hilo de envío por TCP) */
    if (!frame_sock_ready()) return 0;
    if (g_fsockGen != g_shmGen) { g_shmMode = 0; shm_offer(); if (g_shmMode < 0) return 0; }   /* conexión nueva: ofrecer de nuevo */
    if (g_shmMode == 1) return 1;
    if (g_shmMode == 0 && g_shm) {
        ShmHdr *h = (ShmHdr *)g_shm;
        if (h->ack == h->nonce) { g_shmMode = 1; logmsg("Memoria compartida activa: los cuadros ya no pasan por la red"); return 1; }
        if (now_ns() - g_shmAsked > 3000000000LL) { g_shmMode = -1; logmsg("La Mac no confirmó la memoria compartida: sigo por la red"); }
    }
    return 0;
}

/* Aviso de cuadro listo (tipo 6): lugar, pose, formato, ancho y alto por ojo */
static void shm_notify_t(uint8_t type, int slot, uint32_t pid, uint8_t fmt, uint32_t ew, uint32_t eh) {
    uint8_t p[4 + 11];
    uint32_t len = 11;   /* tipo + lugar + pose (4) + formato + ancho (2) + alto (2) */
    p[0] = 0; p[1] = 0; p[2] = 0; p[3] = (uint8_t)len; p[4] = type;
    p[5] = (uint8_t)slot;
    p[6] = (uint8_t)(pid >> 24); p[7] = (uint8_t)(pid >> 16); p[8] = (uint8_t)(pid >> 8); p[9] = (uint8_t)pid;
    p[10] = fmt;
    p[11] = (uint8_t)(ew >> 8); p[12] = (uint8_t)ew; p[13] = (uint8_t)(eh >> 8); p[14] = (uint8_t)eh;
    if (!send_all(g_fsock, (const char *)p, 4 + (int)len)) {
        closesocket(g_fsock); g_fsock = INVALID_SOCKET; g_shmMode = 0;
        ((ShmHdr *)g_shm)->state[slot] = 0;
        logmsg("Se cortó la conexión de imágenes; reintentando");
        return;
    }
    InterlockedIncrement(&g_statSent);
}
static void shm_notify(int slot, uint32_t pid, uint8_t fmt, uint32_t ew, uint32_t eh) { shm_notify_t(6, slot, pid, fmt, ew, eh); }

/* ------------------------------------------------------------------ */
/* Copia directa: el cuadro no sale de la GPU. Se copia (en la GPU) a     */
/* texturas compartidas que DXMT crea como texturas de Metal compartidas  */
/* con un nombre en el sistema; el renderizador (la Mac) las abre por ese */
/* nombre. Se ahorra leer el cuadro de la GPU, copiarlo a la memoria y    */
/* volver a subirlo (≈15–20 ms por cuadro, y el juego no espera).         */
/* El lugar de cada textura se maneja con los mismos estados de la memoria */
/* compartida. Si algo no anda, se sigue con la memoria compartida.       */
/* ------------------------------------------------------------------ */
#define GPU_MAGIC 0x31555047u   /* "GPU1": la Mac abrió las texturas (en reserved[0] del encabezado) */
#define GPU_FAIL  0x4c494146u   /* "FAIL": la Mac no pudo abrirlas */
typedef UINT KMT_HANDLE;
typedef struct { LUID luid; KMT_HANDLE hAdapter; } KMT_OPENADAPTERFROMLUID;
typedef struct { union { KMT_HANDLE hAdapter; void *pAdapter; } a; UINT Flags; KMT_HANDLE hDevice; void *pCommandBuffer; UINT CommandBufferSize;
                 void *pAllocationList; UINT AllocationListSize; void *pPatchLocationList; UINT PatchLocationListSize; } KMT_CREATEDEVICE;
typedef struct { KMT_HANDLE hDevice; KMT_HANDLE hGlobalShare; void *pPrivateRuntimeData; UINT PrivateRuntimeDataSize;
                 UINT TotalPrivateDriverDataSize; UINT ResourcePrivateDriverDataSize; UINT NumAllocations; } KMT_QUERYRESOURCEINFO;
typedef struct { KMT_HANDLE hAllocation; const void *pPrivateDriverData; UINT PrivateDriverDataSize; UINT64 GpuVirtualAddress; ULONG_PTR Reserved[6]; } KMT_OPENALLOCATIONINFO2;
typedef struct { KMT_HANDLE hDevice; KMT_HANDLE hGlobalShare; UINT NumAllocations; KMT_OPENALLOCATIONINFO2 *pOpenAllocationInfo2;
                 void *pPrivateRuntimeData; UINT PrivateRuntimeDataSize; void *pResourcePrivateDriverData; UINT ResourcePrivateDriverDataSize;
                 void *pTotalPrivateDriverDataBuffer; UINT TotalPrivateDriverDataBufferSize; KMT_HANDLE hResource; } KMT_OPENRESOURCE;
typedef struct { KMT_HANDLE hDevice; KMT_HANDLE hResource; const KMT_HANDLE *phAllocationList; UINT AllocationCount; } KMT_DESTROYALLOCATION;
typedef LONG (WINAPI *kmt_fn)(void *);

static uint8_t fmt_code(DXGI_FORMAT f) {
    return (f == DXGI_FORMAT_B8G8R8A8_TYPELESS) ? 0 : 1; /* 0 = BGRA, 1 = RGBA */
}

static int g_gpuState;              /* 0 sin probar · 1 esperando a la Mac · 2 activa · -1 no disponible */
static ID3D11Texture2D *g_gpuTex[SHM_SLOTS];
static ID3D11Query *g_gpuQ[SHM_SLOTS];
static int g_gpuPending[SHM_SLOTS];
static uint32_t g_gpuPose[SHM_SLOTS];
static uint32_t g_gpuW, g_gpuH, g_gpuGen; static DXGI_FORMAT g_gpuFmt;
static int g_gpuShmGen = -1;        /* conexión de imágenes donde se ofrecieron */
/* La Mac se entera apenas la GPU termina la copia: se revisa mientras el juego espera su próximo cuadro
 * (xrWaitFrame, a veces en otro hilo). Antes se esperaba como mucho 3 ms en xrEndFrame y casi siempre se avisaba
 * recién en el cuadro siguiente (~14 ms más tarde). g_gpuLock ordena a los dos hilos. */
static CRITICAL_SECTION g_gpuLock;
static ID3D11DeviceContext *g_gpuCtx;
static const GUID IID_Multithread_ = { 0x9b7e4e00, 0x342c, 0x4106, { 0xa1, 0x9f, 0x4f, 0x27, 0x04, 0xf6, 0x89, 0xf0 } };   /* ID3D11Multithread */
static int64_t g_gpuAsked;

static void gpu_release(void) {
    for (int i = 0; i < SHM_SLOTS; i++) {
        if (g_gpuTex[i]) ID3D11Texture2D_Release(g_gpuTex[i]);
        if (g_gpuQ[i]) ID3D11Query_Release(g_gpuQ[i]);
        g_gpuTex[i] = NULL; g_gpuQ[i] = NULL; g_gpuPending[i] = 0;
    }
    g_gpuW = g_gpuH = 0;
}

/* El nombre con el que DXMT registró la textura compartida en el sistema ("DXMT_shared_resource_…") */
static int gpu_name(ID3D11Device *dev, ID3D11Texture2D *t, char out[64]) {
    static KMT_HANDLE kdev; static int tried;
    static kmt_fn pOpenAd, pCreateDev, pQuery, pOpenRes, pDestroy;
    if (!kdev) {
        if (tried) return 0;
        tried = 1;
        HMODULE gdi = GetModuleHandleA("gdi32.dll");
        if (!gdi) gdi = LoadLibraryA("gdi32.dll");
        if (!gdi) { logmsg("Copia directa: no está gdi32"); return 0; }
        pOpenAd = (kmt_fn)GetProcAddress(gdi, "D3DKMTOpenAdapterFromLuid");
        pCreateDev = (kmt_fn)GetProcAddress(gdi, "D3DKMTCreateDevice");
        pQuery = (kmt_fn)GetProcAddress(gdi, "D3DKMTQueryResourceInfo");
        pOpenRes = (kmt_fn)GetProcAddress(gdi, "D3DKMTOpenResource2");
        pDestroy = (kmt_fn)GetProcAddress(gdi, "D3DKMTDestroyAllocation");
        if (!pOpenAd || !pCreateDev || !pQuery || !pOpenRes || !pDestroy) { logmsg("Copia directa: este Wine no tiene recursos compartidos (D3DKMT)"); return 0; }
        IDXGIDevice *dd = NULL; IDXGIAdapter *ad = NULL; DXGI_ADAPTER_DESC desc;
        if (FAILED(ID3D11Device_QueryInterface(dev, &IID_IDXGIDevice, (void **)&dd)) || !dd) { logmsg("Copia directa: sin IDXGIDevice"); return 0; }
        HRESULT hr = IDXGIDevice_GetAdapter(dd, &ad);
        IDXGIDevice_Release(dd);
        if (FAILED(hr) || !ad) { logmsg("Copia directa: sin adaptador"); return 0; }
        hr = IDXGIAdapter_GetDesc(ad, &desc);
        IDXGIAdapter_Release(ad);
        if (FAILED(hr)) return 0;
        KMT_OPENADAPTERFROMLUID oa; memset(&oa, 0, sizeof oa); oa.luid = desc.AdapterLuid;
        LONG st = pOpenAd(&oa);
        if (st) { logmsg("Copia directa: D3DKMTOpenAdapterFromLuid falló (%lx)", (unsigned long)st); return 0; }
        KMT_CREATEDEVICE cd; memset(&cd, 0, sizeof cd); cd.a.hAdapter = oa.hAdapter;
        st = pCreateDev(&cd);
        if (st || !cd.hDevice) { logmsg("Copia directa: D3DKMTCreateDevice falló (%lx)", (unsigned long)st); return 0; }
        kdev = cd.hDevice;
    }
    IDXGIResource *r = NULL; HANDLE h = NULL;
    if (FAILED(ID3D11Texture2D_QueryInterface(t, &IID_IDXGIResource, (void **)&r)) || !r) { logmsg("Copia directa: sin IDXGIResource"); return 0; }
    HRESULT hr = IDXGIResource_GetSharedHandle(r, &h);
    IDXGIResource_Release(r);
    if (FAILED(hr) || !h) { logmsg("Copia directa: la textura no se pudo compartir (%lx)", (unsigned long)hr); return 0; }
    uint8_t data[512]; memset(data, 0, sizeof data);
    KMT_QUERYRESOURCEINFO q; memset(&q, 0, sizeof q);
    q.hDevice = kdev; q.hGlobalShare = (KMT_HANDLE)(uintptr_t)h; q.pPrivateRuntimeData = data; q.PrivateRuntimeDataSize = sizeof data;
    LONG st = pQuery(&q);
    if (st || q.PrivateRuntimeDataSize < 54 || q.PrivateRuntimeDataSize > sizeof data) {
        logmsg("Copia directa: D3DKMTQueryResourceInfo falló (%lx, %u bytes)", (unsigned long)st, q.PrivateRuntimeDataSize); return 0;
    }
    KMT_OPENALLOCATIONINFO2 al; memset(&al, 0, sizeof al);
    KMT_OPENRESOURCE o; memset(&o, 0, sizeof o);
    o.hDevice = kdev; o.hGlobalShare = q.hGlobalShare; o.NumAllocations = 1; o.pOpenAllocationInfo2 = &al;
    o.pPrivateRuntimeData = data; o.PrivateRuntimeDataSize = q.PrivateRuntimeDataSize;
    st = pOpenRes(&o);
    if (st) { logmsg("Copia directa: D3DKMTOpenResource2 falló (%lx)", (unsigned long)st); return 0; }
    KMT_DESTROYALLOCATION da; memset(&da, 0, sizeof da); da.hDevice = kdev; da.hResource = o.hResource;
    pDestroy(&da);
    memcpy(out, data, 54); out[54] = 0;
    if (strncmp(out, "DXMT_shared_resource_", 21) != 0) { logmsg("Copia directa: nombre inesperado («%.20s»)", out); return 0; }
    return 1;
}

/* Crea las texturas compartidas y le pasa sus nombres a la Mac (paquete 10). 0 = no se pudo (sigue la memoria) */
static int gpu_setup(ID3D11Device *dev, uint32_t ew, uint32_t eh, DXGI_FORMAT fmt) {
    gpu_release();
    D3D11_TEXTURE2D_DESC d; memset(&d, 0, sizeof d);
    d.Width = ew * 2; d.Height = eh; d.MipLevels = 1; d.ArraySize = 1;
    d.Format = fmt; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE; d.MiscFlags = D3D11_RESOURCE_MISC_SHARED;
    char names[SHM_SLOTS][64];
    for (int i = 0; i < SHM_SLOTS; i++) {
        HRESULT hr = ID3D11Device_CreateTexture2D(dev, &d, NULL, &g_gpuTex[i]);
        if (FAILED(hr) || !g_gpuTex[i]) { logmsg("Copia directa: no se pudo crear una textura compartida (%lx)", (unsigned long)hr); g_gpuTex[i] = NULL; gpu_release(); return 0; }
        D3D11_QUERY_DESC qd; qd.Query = D3D11_QUERY_EVENT; qd.MiscFlags = 0;
        if (FAILED(ID3D11Device_CreateQuery(dev, &qd, &g_gpuQ[i]))) { logmsg("Copia directa: no se pudo crear la consulta"); gpu_release(); return 0; }
        if (!gpu_name(dev, g_gpuTex[i], names[i])) { gpu_release(); return 0; }
    }
    g_gpuW = ew; g_gpuH = eh; g_gpuFmt = fmt; g_gpuGen++;
    /* La copia se revisa también desde el hilo de xrWaitFrame: el contexto de D3D11 tiene que aceptar varios hilos */
    {
        ID3D11DeviceContext *ic = NULL; ID3D11Multithread *mt = NULL;
        ID3D11Device_GetImmediateContext(dev, &ic);
        if (ic && SUCCEEDED(ID3D11DeviceContext_QueryInterface(ic, &IID_Multithread_, (void **)&mt)) && mt) {
            ID3D11Multithread_SetMultithreadProtected(mt, TRUE);
            ID3D11Multithread_Release(mt);
        }
        if (ic) ID3D11DeviceContext_Release(ic);
    }
    ShmHdr *h = (ShmHdr *)g_shm;
    h->reserved[0] = 0; h->reserved[1] = g_gpuGen;
    MemoryBarrier();
    /* [largo][10][generación u32][ancho u16][alto u16][formato][3 nombres de 56] */
    uint8_t p[4 + 1 + 4 + 4 + 1 + SHM_SLOTS * 56];
    uint32_t len = 1 + 4 + 4 + 1 + SHM_SLOTS * 56;
    p[0] = (uint8_t)(len >> 24); p[1] = (uint8_t)(len >> 16); p[2] = (uint8_t)(len >> 8); p[3] = (uint8_t)len; p[4] = 10;
    p[5] = (uint8_t)(g_gpuGen >> 24); p[6] = (uint8_t)(g_gpuGen >> 16); p[7] = (uint8_t)(g_gpuGen >> 8); p[8] = (uint8_t)g_gpuGen;
    p[9] = (uint8_t)(ew >> 8); p[10] = (uint8_t)ew; p[11] = (uint8_t)(eh >> 8); p[12] = (uint8_t)eh;
    p[13] = fmt_code(fmt);
    for (int i = 0; i < SHM_SLOTS; i++) { memset(p + 14 + i * 56, 0, 56); memcpy(p + 14 + i * 56, names[i], 55); }
    if (!send_all(g_fsock, (const char *)p, (int)(4 + len))) { closesocket(g_fsock); g_fsock = INVALID_SOCKET; g_shmMode = 0; gpu_release(); return 0; }
    g_gpuShmGen = g_shmGen; g_gpuState = 1; g_gpuAsked = now_ns();
    logmsg("Copia directa: texturas compartidas ofrecidas a la Mac (%ux%u por ojo)", ew, eh);
    return 1;
}

/* ¿Terminó la GPU la copia de este lugar? Entonces avisarle a la Mac */
static int gpu_done(ID3D11DeviceContext *ctx, int i) {   /* (con g_gpuLock tomado) */
    if (!g_gpuPending[i]) return 1;
    BOOL done = FALSE;
    if (ID3D11DeviceContext_GetData(ctx, (ID3D11Asynchronous *)g_gpuQ[i], &done, sizeof done, D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) return 0;
    g_gpuPending[i] = 0;
    MemoryBarrier();
    ((ShmHdr *)g_shm)->state[i] = 1;
    shm_notify_t(11, i, g_gpuPose[i], fmt_code(g_gpuFmt), g_gpuW, g_gpuH);
    return 1;
}

static DWORD WINAPI send_thread(LPVOID arg) {
    (void)arg;
    for (;;) {
        WaitForSingleObject(g_frameEvent, INFINITE);
        int idx;
        EnterCriticalSection(&g_lock);
        idx = g_frameReady;
        if (idx >= 0) { g_frameReady = -1; g_frameSending = idx; } else idx = -1;
        LeaveCriticalSection(&g_lock);
        if (idx >= 0) send_frame(idx);
        EnterCriticalSection(&g_lock); g_frameSending = -1; LeaveCriticalSection(&g_lock);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* Paths                                                               */
/* ------------------------------------------------------------------ */
#define MAX_PATHS 4096
static char *g_paths[MAX_PATHS];
static int g_npaths;
static XrPath intern_path(const char *s) {
    for (int i = 0; i < g_npaths; i++) if (!strcmp(g_paths[i], s)) return (XrPath)(i + 1);
    if (g_npaths >= MAX_PATHS) return XR_NULL_PATH;
    g_paths[g_npaths] = _strdup(s);
    return (XrPath)(++g_npaths);
}
static const char *path_str(XrPath p) {
    if (p == XR_NULL_PATH || p > (XrPath)g_npaths) return "";
    return g_paths[p - 1];
}

/* ------------------------------------------------------------------ */
/* Objetos                                                             */
/* ------------------------------------------------------------------ */
typedef struct { int dummy; } Instance;
typedef struct ActionSet { char name[XR_MAX_ACTION_SET_NAME_SIZE]; } ActionSet;
#define MAX_BIND 64
typedef struct Action {
    XrActionType type;
    char name[XR_MAX_ACTION_NAME_SIZE];
    ActionSet *set;
    XrPath sub[8]; int nsub;
    struct { XrPath profile, binding; } all[MAX_BIND]; int nall;
    float cur[3][2], prev[3][2];   /* [subaction: 0 = todas, 1 = izq, 2 = der][x,y] */
    int active[3];
    XrTime changeTime[3];
} Action;
#define MAX_ACTIONS 256
static Action *g_actions[MAX_ACTIONS];
static int g_nactions;

typedef struct Swapchain {
    int64_t format;
    DXGI_FORMAT dxgi;
    uint32_t w, h, arraySize, mips, count;
    ID3D11Texture2D *img[3];
    VkImage vimg[3]; VkDeviceMemory vmem[3];   /* (sesión Vulkan) */
    GLuint gltex[3]; int isGl;                 /* (sesión OpenGL) */
    uint32_t next, released;
} Swapchain;

typedef struct Space {
    int isAction;
    XrReferenceSpaceType ref;
    Action *action;
    XrPath subaction;
    XrPosef offset;
} Space;

typedef struct Session {
    ID3D11Device *dev;
    ID3D11DeviceContext *ctx;
    int running;
    XrSessionState state;
    /* readback */
    ID3D11Texture2D *staging[3];
    uint32_t stW, stH; DXGI_FORMAT stFmt; int stFlip[3]; int loggedRects;
    uint32_t stPose[3]; int stValid[3]; int stNext;
    XrPath profile;
    int frameCount;
    /* Vulkan (juegos que dibujan con Vulkan, por ejemplo por OpenComposite): la imagen se lee de la GPU a la memoria */
    int vk;
    VkInstance vkInst; VkPhysicalDevice vkPhys; VkDevice vkDev; VkQueue vkQueue;
    VkCommandPool vkPool; VkCommandBuffer vkCb[STAGES]; VkFence vkFence[STAGES];
    VkBuffer vkBuf[STAGES]; VkDeviceMemory vkBufMem[STAGES]; void *vkMap[STAGES];
    int vkWarned;
    /* OpenGL: el contexto del juego, y para leer la imagen un framebuffer y dos PBO */
    int gl; HDC gldc; HGLRC glrc;
    GLuint glFbo, glPbo[STAGES]; GLsync glSync[STAGES];
} Session;

static Instance g_instance;
static Session *g_session;
static XrPosef g_localOrigin; static int g_localSet;

/* Cola de eventos */
#define MAX_EVENTS 32
static XrEventDataBuffer g_events[MAX_EVENTS];
static int g_evHead, g_evTail;
static void push_event(const void *ev, size_t size) {
    EnterCriticalSection(&g_lock);
    int next = (g_evTail + 1) % MAX_EVENTS;
    if (next != g_evHead) {
        memset(&g_events[g_evTail], 0, sizeof g_events[0]);
        memcpy(&g_events[g_evTail], ev, size);
        g_evTail = next;
    }
    LeaveCriticalSection(&g_lock);
}
static void set_state(Session *s, XrSessionState st) {
    s->state = st;
    XrEventDataSessionStateChanged e = { XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED };
    e.session = (XrSession)s; e.state = st; e.time = now_ns();
    push_event(&e, sizeof e);
    static const char *names[] = { "UNKNOWN","IDLE","READY","SYNCHRONIZED","VISIBLE","FOCUSED","STOPPING","LOSS_PENDING","EXITING" };
    logmsg("Estado de sesión: %s", st < 9 ? names[st] : "?");
}

/* ------------------------------------------------------------------ */
/* Utilidades                                                          */
/* ------------------------------------------------------------------ */
#define FILL_STR(dst, src) do { strncpy((dst), (src), sizeof(dst) - 1); (dst)[sizeof(dst) - 1] = 0; } while (0)

static const char *g_extensions[] = {
    XR_KHR_D3D11_ENABLE_EXTENSION_NAME,
    XR_KHR_VULKAN_ENABLE_EXTENSION_NAME,
    XR_KHR_OPENGL_ENABLE_EXTENSION_NAME,
    XR_KHR_WIN32_CONVERT_PERFORMANCE_COUNTER_TIME_EXTENSION_NAME,
};
#define N_EXT (sizeof g_extensions / sizeof g_extensions[0])

static XrPosef space_world(const Space *sp, const Track *t, int *valid) {
    XrPosef base = pident();
    *valid = 1;
    if (!sp->isAction) {
        switch (sp->ref) {
        case XR_REFERENCE_SPACE_TYPE_VIEW: base = head_pose(t); *valid = t->valid; break;
        case XR_REFERENCE_SPACE_TYPE_LOCAL: base = g_localOrigin; break;
        default: base = pident(); break; /* STAGE = piso del Quest */
        }
    } else {
        const char *sub = path_str(sp->subaction);
        int hand = -1, useAim = 0;
        if (strstr(sub, "/user/hand/left")) hand = 0;
        else if (strstr(sub, "/user/hand/right")) hand = 1;
        Action *a = sp->action;
        if (a) {
            for (int i = 0; i < a->nall; i++) {
                if (g_session && a->all[i].profile != g_session->profile) continue;
                const char *b = path_str(a->all[i].binding);
                int bh = strstr(b, "/user/hand/left") ? 0 : strstr(b, "/user/hand/right") ? 1 : -1;
                if (hand >= 0 && bh != hand) continue;
                if (hand < 0) hand = bh;
                useAim = strstr(b, "/aim/pose") != NULL;
                break;
            }
        }
        if (hand < 0) { *valid = 0; return sp->offset; }
        base = useAim ? t->aim[hand] : t->grip[hand];
        *valid = t->valid && t->handValid[hand];
    }
    return pmul(base, sp->offset);
}

/* Valor de un componente de entrada a partir del path de binding */
static int input_value(const Track *t, const char *b, float out[2], int *isPose) {
    int hand = strstr(b, "/user/hand/left") ? 0 : strstr(b, "/user/hand/right") ? 1 : -1;
    *isPose = 0;
    out[0] = out[1] = 0;
    if (hand < 0) return 0;
    const char *c = strstr(b, "/input/");
    if (!c) return 0;
    c += 7;
    const float *in = t->in[hand];
#define IS(pfx) (!strncmp(c, pfx, strlen(pfx)))
    if (IS("grip/pose") || IS("aim/pose") || IS("grip_surface/pose")) { *isPose = 1; return t->handValid[hand]; }
    if (IS("trigger")) { out[0] = strstr(c, "touch") ? in[IN_TTOUCH] : in[IN_TRIGGER]; return 1; }
    if (IS("squeeze/force")) { float f = (in[IN_SQUEEZE] - 0.75f) * 4.0f; out[0] = f < 0 ? 0 : f > 1 ? 1 : f; return 1; }   /* Index: apretar fuerte */
    if (IS("squeeze") || IS("grip/")) { out[0] = in[IN_SQUEEZE]; return 1; }
    if (IS("trackpad") && g_index) return 1;               /* Index: el Quest no tiene trackpad (queda en 0, no copia el joystick) */
    if (IS("select")) { out[0] = in[IN_TRIGGER] > 0.5f; return 1; }
    if (IS("thumbstick") || IS("trackpad") || IS("joystick")) {
        const char *rest = strchr(c, '/');
        if (!rest) { out[0] = in[IN_TX]; out[1] = in[IN_TY]; return 1; }
        rest++;
        if (!strcmp(rest, "x")) out[0] = in[IN_TX];
        else if (!strcmp(rest, "y")) out[0] = in[IN_TY];
        else if (!strncmp(rest, "click", 5)) out[0] = in[IN_TCLICK];
        else if (!strncmp(rest, "touch", 5)) out[0] = in[IN_THTOUCH];
        return 1;
    }
    if (IS("a/") || IS("x/")) { out[0] = strstr(c, "touch") ? in[IN_ATOUCH] : in[IN_A]; return 1; }
    if (IS("b/") || IS("y/")) { out[0] = strstr(c, "touch") ? in[IN_BTOUCH] : in[IN_B]; return 1; }
    if (IS("menu") || IS("system")) { out[0] = in[IN_MENU]; return 1; }
#undef IS
    return 0;
}

/* ------------------------------------------------------------------ */
/* Funciones OpenXR                                                    */
/* ------------------------------------------------------------------ */
XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function);
static void init_once(void);
static void install_gl_hook(void);
static void fix_opencomposite_stubs(void);
static void copy_unity_logs(int current);

XRAPI_ATTR XrResult XRAPI_CALL rt_xrEnumerateInstanceExtensionProperties(const char *layerName, uint32_t cap, uint32_t *count, XrExtensionProperties *props) {
    init_once();
    (void)layerName;
    *count = (uint32_t)N_EXT;
    if (cap == 0) return XR_SUCCESS;
    if (cap < N_EXT) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < N_EXT; i++) {
        FILL_STR(props[i].extensionName, g_extensions[i]);
        props[i].extensionVersion = 1;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrEnumerateApiLayerProperties(uint32_t cap, uint32_t *count, XrApiLayerProperties *props) {
    init_once();
    (void)cap; (void)props; *count = 0; return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrCreateInstance(const XrInstanceCreateInfo *info, XrInstance *instance) {
    init_once();
    for (uint32_t i = 0; i < info->enabledExtensionCount; i++) {
        int ok = 0;
        for (size_t j = 0; j < N_EXT; j++) if (!strcmp(info->enabledExtensionNames[i], g_extensions[j])) ok = 1;
        if (!ok) { logmsg("Extensión no soportada pedida: %s", info->enabledExtensionNames[i]); return XR_ERROR_EXTENSION_NOT_PRESENT; }
    }
    logmsg("xrCreateInstance de '%s' (%s)", info->applicationInfo.applicationName, info->applicationInfo.engineName);
    install_gl_hook();   /* (solo hace algo si el juego usa OpenGL) */
    fix_opencomposite_stubs();
    /* Pedirle a la Mac los ajustes de este juego (resolución, campo de visión, controles) */
    snprintf(g_appName, sizeof g_appName, "%s", info->applicationInfo.applicationName[0] ? info->applicationInfo.applicationName : "?");
    g_helloVer = g_cfgVer;
    send_packet(9, g_appName, (uint32_t)strlen(g_appName), NULL, 0);
    *instance = (XrInstance)&g_instance;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL rt_xrDestroyInstance(XrInstance i) { (void)i; logmsg("xrDestroyInstance"); copy_unity_logs(1); return XR_SUCCESS; }

XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetInstanceProperties(XrInstance i, XrInstanceProperties *p) {
    (void)i;
    p->runtimeVersion = XR_MAKE_VERSION(0, 1, 0);
    FILL_STR(p->runtimeName, RT_NAME);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrPollEvent(XrInstance i, XrEventDataBuffer *ev) {
    g_inside = "xrPollEvent";
    (void)i;
    XrResult r = XR_EVENT_UNAVAILABLE;
    EnterCriticalSection(&g_lock);
    if (g_evHead != g_evTail) {
        memcpy(ev, &g_events[g_evHead], sizeof *ev);
        g_evHead = (g_evHead + 1) % MAX_EVENTS;
        r = XR_SUCCESS;
    }
    LeaveCriticalSection(&g_lock);
    return r;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrResultToString(XrInstance i, XrResult value, char buffer[XR_MAX_RESULT_STRING_SIZE]) {
    (void)i;
#define RES_CASE(name, val) case val: FILL_STR_N(buffer, #name); return XR_SUCCESS;
#define FILL_STR_N(dst, src) do { strncpy((dst), (src), XR_MAX_RESULT_STRING_SIZE - 1); (dst)[XR_MAX_RESULT_STRING_SIZE - 1] = 0; } while (0)
    switch (value) { XR_LIST_ENUM_XrResult(RES_CASE) default: break; }
    snprintf(buffer, XR_MAX_RESULT_STRING_SIZE, "%s_%d", value < 0 ? "XR_UNKNOWN_FAILURE" : "XR_UNKNOWN_SUCCESS", (int)value);
    return XR_SUCCESS;
#undef RES_CASE
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrStructureTypeToString(XrInstance i, XrStructureType value, char buffer[XR_MAX_STRUCTURE_NAME_SIZE]) {
    (void)i;
#define ST_CASE(name, val) case val: strncpy(buffer, #name, XR_MAX_STRUCTURE_NAME_SIZE - 1); buffer[XR_MAX_STRUCTURE_NAME_SIZE - 1] = 0; return XR_SUCCESS;
    switch (value) { XR_LIST_ENUM_XrStructureType(ST_CASE) default: break; }
    snprintf(buffer, XR_MAX_STRUCTURE_NAME_SIZE, "XR_UNKNOWN_STRUCTURE_TYPE_%d", (int)value);
    return XR_SUCCESS;
#undef ST_CASE
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrStringToPath(XrInstance i, const char *s, XrPath *path) {
    (void)i;
    if (!s || s[0] != '/') return XR_ERROR_PATH_FORMAT_INVALID;
    EnterCriticalSection(&g_lock);
    *path = intern_path(s);
    LeaveCriticalSection(&g_lock);
    return *path ? XR_SUCCESS : XR_ERROR_PATH_COUNT_EXCEEDED;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrPathToString(XrInstance i, XrPath path, uint32_t cap, uint32_t *count, char *buffer) {
    (void)i;
    if (path == XR_NULL_PATH || path > (XrPath)g_npaths) return XR_ERROR_PATH_INVALID;
    const char *s = path_str(path);
    uint32_t n = (uint32_t)strlen(s) + 1;
    *count = n;
    if (cap == 0) return XR_SUCCESS;
    if (cap < n) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(buffer, s, n);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetSystem(XrInstance i, const XrSystemGetInfo *info, XrSystemId *id) {
    (void)i;
    if (info->formFactor != XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY) return XR_ERROR_FORM_FACTOR_UNSUPPORTED;
    *id = 1;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetSystemProperties(XrInstance i, XrSystemId id, XrSystemProperties *p) {
    (void)i;
    p->systemId = id;
    p->vendorId = 0x2833;
    FILL_STR(p->systemName, "Quest 3 via QuestVRBridge");
    p->graphicsProperties.maxSwapchainImageWidth = 4096;
    p->graphicsProperties.maxSwapchainImageHeight = 4096;
    p->graphicsProperties.maxLayerCount = 16;
    p->trackingProperties.orientationTracking = XR_TRUE;
    p->trackingProperties.positionTracking = XR_TRUE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrEnumerateEnvironmentBlendModes(XrInstance i, XrSystemId id, XrViewConfigurationType t, uint32_t cap, uint32_t *count, XrEnvironmentBlendMode *modes) {
    (void)i; (void)id; (void)t;
    *count = 1;
    if (cap == 0) return XR_SUCCESS;
    modes[0] = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrEnumerateViewConfigurations(XrInstance i, XrSystemId id, uint32_t cap, uint32_t *count, XrViewConfigurationType *types) {
    (void)i; (void)id;
    *count = 1;
    if (cap == 0) return XR_SUCCESS;
    types[0] = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetViewConfigurationProperties(XrInstance i, XrSystemId id, XrViewConfigurationType t, XrViewConfigurationProperties *p) {
    (void)i; (void)id;
    if (t != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    p->viewConfigurationType = t;
    p->fovMutable = XR_TRUE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrEnumerateViewConfigurationViews(XrInstance i, XrSystemId id, XrViewConfigurationType t, uint32_t cap, uint32_t *count, XrViewConfigurationView *views) {
    (void)i; (void)id;
    if (t != XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO) return XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED;
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    /* Esperar (un momento) los ajustes de este juego, así el tamaño pedido ya es el suyo */
    for (int w = 0; w < 150 && g_helloVer >= 0 && g_cfgVer == g_helloVer && g_connected; w++) Sleep(10);
    for (int v = 0; v < 2; v++) {
        views[v].recommendedImageRectWidth = (uint32_t)g_eyeW;
        views[v].recommendedImageRectHeight = (uint32_t)g_eyeH;
        views[v].maxImageRectWidth = 4096;
        views[v].maxImageRectHeight = 4096;
        views[v].recommendedSwapchainSampleCount = 1;
        views[v].maxSwapchainSampleCount = 1;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetD3D11GraphicsRequirementsKHR(XrInstance i, XrSystemId id, XrGraphicsRequirementsD3D11KHR *req) {
    (void)i; (void)id;
    memset(&req->adapterLuid, 0, sizeof req->adapterLuid);
    IDXGIFactory1 *f = NULL;
    if (SUCCEEDED(CreateDXGIFactory1(&IID_IDXGIFactory1, (void **)&f))) {
        IDXGIAdapter1 *a = NULL;
        if (SUCCEEDED(IDXGIFactory1_EnumAdapters1(f, 0, &a))) {
            DXGI_ADAPTER_DESC1 d;
            if (SUCCEEDED(IDXGIAdapter1_GetDesc1(a, &d))) req->adapterLuid = d.AdapterLuid;
            IDXGIAdapter1_Release(a);
        }
        IDXGIFactory1_Release(f);
    }
    req->minFeatureLevel = D3D_FEATURE_LEVEL_11_0;
    return XR_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Vulkan (XR_KHR_vulkan_enable). Los juegos con Vulkan (por ejemplo,  */
/* juegos de OpenVR por OpenComposite) dibujan en imágenes que crea el */
/* runtime; en xrEndFrame se copian (en la GPU) a memoria que se puede */
/* leer y de ahí a la memoria compartida con la Mac.                   */
/* ------------------------------------------------------------------ */
static PFN_vkGetInstanceProcAddr g_vkGIPA;
static void install_vulkan_hook(void);
static int patch_iat(HMODULE mod, const char *dll, const char *fn, void *repl, void **orig);
static struct {
    PFN_vkEnumeratePhysicalDevices EnumeratePhysicalDevices;
    PFN_vkGetPhysicalDeviceMemoryProperties GetPhysicalDeviceMemoryProperties;
    PFN_vkGetDeviceProcAddr GetDeviceProcAddr;
    PFN_vkGetDeviceQueue GetDeviceQueue;
    PFN_vkCreateImage CreateImage; PFN_vkDestroyImage DestroyImage;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements; PFN_vkBindImageMemory BindImageMemory;
    PFN_vkCreateBuffer CreateBuffer; PFN_vkDestroyBuffer DestroyBuffer;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements; PFN_vkBindBufferMemory BindBufferMemory;
    PFN_vkAllocateMemory AllocateMemory; PFN_vkFreeMemory FreeMemory;
    PFN_vkMapMemory MapMemory; PFN_vkUnmapMemory UnmapMemory;
    PFN_vkCreateCommandPool CreateCommandPool; PFN_vkDestroyCommandPool DestroyCommandPool;
    PFN_vkAllocateCommandBuffers AllocateCommandBuffers; PFN_vkFreeCommandBuffers FreeCommandBuffers;
    PFN_vkResetCommandBuffer ResetCommandBuffer;
    PFN_vkBeginCommandBuffer BeginCommandBuffer; PFN_vkEndCommandBuffer EndCommandBuffer;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier; PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer;
    PFN_vkQueueSubmit QueueSubmit; PFN_vkQueueWaitIdle QueueWaitIdle;
    PFN_vkCreateFence CreateFence; PFN_vkDestroyFence DestroyFence;
    PFN_vkWaitForFences WaitForFences; PFN_vkResetFences ResetFences;
} vk;

/* vkGetInstanceProcAddr del Vulkan de Wine (el mismo vulkan-1.dll que cargó el juego) */
static int vk_loader(void) {
    if (g_vkGIPA) return 1;
    HMODULE m = LoadLibraryA("vulkan-1.dll");
    if (m) g_vkGIPA = (PFN_vkGetInstanceProcAddr)(void (*)(void))GetProcAddress(m, "vkGetInstanceProcAddr");
    if (!g_vkGIPA) logmsg("Vulkan: no se encontró vulkan-1.dll");
    return g_vkGIPA != NULL;
}

static int vk_load(VkInstance inst, VkDevice dev) {
#define VKI(n) vk.n = (PFN_vk##n)g_vkGIPA(inst, "vk" #n)
#define VKD(n) do { vk.n = (PFN_vk##n)vk.GetDeviceProcAddr(dev, "vk" #n); if (!vk.n) { logmsg("Vulkan: falta vk" #n); return 0; } } while (0)
    VKI(EnumeratePhysicalDevices); VKI(GetPhysicalDeviceMemoryProperties); VKI(GetDeviceProcAddr);
    if (!vk.GetDeviceProcAddr || !vk.GetPhysicalDeviceMemoryProperties) { logmsg("Vulkan: faltan funciones de la instancia"); return 0; }
    VKD(GetDeviceQueue); VKD(CreateImage); VKD(DestroyImage); VKD(GetImageMemoryRequirements); VKD(BindImageMemory);
    VKD(CreateBuffer); VKD(DestroyBuffer); VKD(GetBufferMemoryRequirements); VKD(BindBufferMemory);
    VKD(AllocateMemory); VKD(FreeMemory); VKD(MapMemory); VKD(UnmapMemory);
    VKD(CreateCommandPool); VKD(DestroyCommandPool); VKD(AllocateCommandBuffers); VKD(FreeCommandBuffers); VKD(ResetCommandBuffer);
    VKD(BeginCommandBuffer); VKD(EndCommandBuffer); VKD(CmdPipelineBarrier); VKD(CmdCopyImageToBuffer);
    VKD(QueueSubmit); VKD(QueueWaitIdle); VKD(CreateFence); VKD(DestroyFence); VKD(WaitForFences); VKD(ResetFences);
#undef VKI
#undef VKD
    return 1;
}

/* Tipo de memoria con las propiedades pedidas (y, si hay, también las preferidas) */
static int vk_memtype(Session *s, uint32_t bits, VkMemoryPropertyFlags want, VkMemoryPropertyFlags prefer) {
    VkPhysicalDeviceMemoryProperties mp;
    vk.GetPhysicalDeviceMemoryProperties(s->vkPhys, &mp);
    int best = -1;
    for (uint32_t i = 0; i < mp.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
        if (!((bits >> i) & 1) || (f & want) != want) continue;
        if ((f & prefer) == prefer) return (int)i;
        if (best < 0) best = (int)i;
    }
    return best;
}

static XrResult ext_string(const char *str, uint32_t cap, uint32_t *count, char *buf) {
    uint32_t n = (uint32_t)strlen(str) + 1;
    *count = n;
    if (cap == 0) return XR_SUCCESS;
    if (cap < n) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(buf, str, n);
    return XR_SUCCESS;
}

/* (no hace falta ninguna extensión de Vulkan: la imagen se lee con una copia común) */
XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetVulkanInstanceExtensionsKHR(XrInstance i, XrSystemId id, uint32_t cap, uint32_t *count, char *buf) {
    (void)i; (void)id; install_vulkan_hook(); return ext_string("", cap, count, buf);
}
XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetVulkanDeviceExtensionsKHR(XrInstance i, XrSystemId id, uint32_t cap, uint32_t *count, char *buf) {
    (void)i; (void)id; return ext_string("", cap, count, buf);
}
XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetVulkanGraphicsDeviceKHR(XrInstance i, XrSystemId id, VkInstance inst, VkPhysicalDevice *out) {
    (void)i; (void)id;
    install_vulkan_hook();
    if (!vk_loader()) return XR_ERROR_RUNTIME_FAILURE;
    PFN_vkEnumeratePhysicalDevices en = (PFN_vkEnumeratePhysicalDevices)g_vkGIPA(inst, "vkEnumeratePhysicalDevices");
    uint32_t n = 1;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    if (!en || (en(inst, &n, &pd) < 0) || !pd) { logmsg("Vulkan: no hay GPU"); return XR_ERROR_RUNTIME_FAILURE; }
    *out = pd;   /* (en la Mac hay una sola) */
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetVulkanGraphicsRequirementsKHR(XrInstance i, XrSystemId id, XrGraphicsRequirementsVulkanKHR *req) {
    (void)i; (void)id;
    req->minApiVersionSupported = XR_MAKE_VERSION(1, 0, 0);
    req->maxApiVersionSupported = XR_MAKE_VERSION(1, 3, 0);
    return XR_SUCCESS;
}

/* Ejecuta en la cola del juego un cambio de diseño (layout) de una imagen y espera a que termine */
static int vk_initial_layout(Session *s, VkImage img, VkImageAspectFlags aspect, VkImageLayout layout, uint32_t mips, uint32_t layers) {
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = s->vkPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    if (vk.AllocateCommandBuffers(s->vkDev, &ai, &cb) != VK_SUCCESS) return 0;
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.BeginCommandBuffer(cb, &bi);
    VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
    b.srcAccessMask = 0; b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED; b.newLayout = layout;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = img;
    b.subresourceRange.aspectMask = aspect; b.subresourceRange.levelCount = mips; b.subresourceRange.layerCount = layers;
    vk.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, NULL, 0, NULL, 1, &b);
    vk.EndCommandBuffer(cb);
    VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    si.commandBufferCount = 1; si.pCommandBuffers = &cb;
    int ok = vk.QueueSubmit(s->vkQueue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS && vk.QueueWaitIdle(s->vkQueue) == VK_SUCCESS;
    vk.FreeCommandBuffers(s->vkDev, s->vkPool, 1, &cb);
    return ok;
}

static void vk_free_buffers(Session *s) {
    for (int k = 0; k < STAGES; k++) {
        if (s->vkMap[k]) vk.UnmapMemory(s->vkDev, s->vkBufMem[k]);
        if (s->vkBuf[k]) vk.DestroyBuffer(s->vkDev, s->vkBuf[k], NULL);
        if (s->vkBufMem[k]) vk.FreeMemory(s->vkDev, s->vkBufMem[k], NULL);
        s->vkMap[k] = NULL; s->vkBuf[k] = VK_NULL_HANDLE; s->vkBufMem[k] = VK_NULL_HANDLE; s->stValid[k] = 0;
    }
}

static int vk_session_setup(Session *s, const XrGraphicsBindingVulkanKHR *vb) {
    if (!vk_loader() || !vk_load(vb->instance, vb->device)) return 0;
    s->vk = 1;
    s->vkInst = vb->instance; s->vkPhys = vb->physicalDevice; s->vkDev = vb->device;
    vk.GetDeviceQueue(s->vkDev, vb->queueFamilyIndex, vb->queueIndex, &s->vkQueue);
    VkCommandPoolCreateInfo pi = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
    pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT; pi.queueFamilyIndex = vb->queueFamilyIndex;
    if (vk.CreateCommandPool(s->vkDev, &pi, NULL, &s->vkPool) != VK_SUCCESS) { logmsg("Vulkan: no se pudo crear el command pool"); return 0; }
    VkCommandBufferAllocateInfo ai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
    ai.commandPool = s->vkPool; ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; ai.commandBufferCount = STAGES;
    if (vk.AllocateCommandBuffers(s->vkDev, &ai, s->vkCb) != VK_SUCCESS) { logmsg("Vulkan: no se pudieron crear los command buffers"); return 0; }
    VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    for (int k = 0; k < STAGES; k++)
        if (vk.CreateFence(s->vkDev, &fi, NULL, &s->vkFence[k]) != VK_SUCCESS) { logmsg("Vulkan: no se pudo crear el fence"); return 0; }
    return 1;
}

static void vk_session_release(Session *s) {
    if (!s->vkDev) return;
    if (s->vkQueue) vk.QueueWaitIdle(s->vkQueue);
    vk_free_buffers(s);
    for (int k = 0; k < STAGES; k++) if (s->vkFence[k]) vk.DestroyFence(s->vkDev, s->vkFence[k], NULL);
    if (s->vkPool) vk.DestroyCommandPool(s->vkDev, s->vkPool, NULL);
}

/* ------------------------------------------------------------------ */
/* OpenGL (XR_KHR_opengl_enable). Los juegos con OpenGL (por ejemplo,  */
/* Google Earth VR por OpenComposite) dibujan en texturas que crea el  */
/* runtime en su contexto; en xrEndFrame se leen (con PBO, sin esperar)*/
/* a la memoria compartida con la Mac.                                 */
/* ------------------------------------------------------------------ */
static struct {
    PROC (WINAPI *GetProcAddress)(LPCSTR);
    HGLRC (WINAPI *GetCurrentContext)(void);
    HDC (WINAPI *GetCurrentDC)(void);
    BOOL (WINAPI *MakeCurrent)(HDC, HGLRC);
    void (WINAPI *GenTextures)(GLsizei, GLuint *);
    void (WINAPI *DeleteTextures)(GLsizei, const GLuint *);
    void (WINAPI *BindTexture)(GLenum, GLuint);
    void (WINAPI *TexImage2D)(GLenum, GLint, GLint, GLsizei, GLsizei, GLint, GLenum, GLenum, const void *);
    void (WINAPI *TexParameteri)(GLenum, GLenum, GLint);
    void (WINAPI *PixelStorei)(GLenum, GLint);
    void (WINAPI *ReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *);
    void (WINAPI *GetIntegerv)(GLenum, GLint *);
    void (WINAPI *ReadBuffer)(GLenum);
    PFNGLTEXIMAGE3DPROC TexImage3D;
    PFNGLGENFRAMEBUFFERSPROC GenFramebuffers; PFNGLDELETEFRAMEBUFFERSPROC DeleteFramebuffers;
    PFNGLBINDFRAMEBUFFERPROC BindFramebuffer; PFNGLFRAMEBUFFERTEXTURELAYERPROC FramebufferTextureLayer;
    PFNGLFRAMEBUFFERTEXTURE2DPROC FramebufferTexture2D;
    PFNGLGENBUFFERSPROC GenBuffers; PFNGLDELETEBUFFERSPROC DeleteBuffers; PFNGLBINDBUFFERPROC BindBuffer;
    PFNGLBUFFERDATAPROC BufferData; PFNGLMAPBUFFERRANGEPROC MapBufferRange; PFNGLUNMAPBUFFERPROC UnmapBuffer;
    PFNGLFENCESYNCPROC FenceSync; PFNGLCLIENTWAITSYNCPROC ClientWaitSync; PFNGLDELETESYNCPROC DeleteSync;
} gl;

static int gl_load(void) {
    static int ok = -1;
    if (ok >= 0) return ok;
    HMODULE m = LoadLibraryA("opengl32.dll");
    ok = 0;
    if (!m) { logmsg("OpenGL: no se encontró opengl32.dll"); return 0; }
#define GL1(f, n) do { FARPROC f_ = GetProcAddress(m, n); memcpy(&gl.f, &f_, sizeof f_); } while (0)
    GL1(GetProcAddress, "wglGetProcAddress"); GL1(GetCurrentContext, "wglGetCurrentContext"); GL1(GetCurrentDC, "wglGetCurrentDC");
    GL1(MakeCurrent, "wglMakeCurrent"); GL1(GenTextures, "glGenTextures"); GL1(DeleteTextures, "glDeleteTextures");
    GL1(BindTexture, "glBindTexture"); GL1(TexImage2D, "glTexImage2D"); GL1(TexParameteri, "glTexParameteri");
    GL1(PixelStorei, "glPixelStorei"); GL1(ReadPixels, "glReadPixels"); GL1(GetIntegerv, "glGetIntegerv"); GL1(ReadBuffer, "glReadBuffer");
#undef GL1
    ok = gl.GetProcAddress && gl.MakeCurrent && gl.GenTextures && gl.ReadPixels;
    if (!ok) logmsg("OpenGL: faltan funciones de opengl32.dll");
    return ok;
}
/* Las funciones nuevas (≥ 1.2) se piden con un contexto activo */
static int gl_load_ext(void) {
#define GLX(f, n) do { PROC f_ = gl.GetProcAddress(n); memcpy(&gl.f, &f_, sizeof f_); if (!gl.f) { logmsg("OpenGL: falta %s", n); return 0; } } while (0)
    GLX(TexImage3D, "glTexImage3D"); GLX(GenFramebuffers, "glGenFramebuffers"); GLX(DeleteFramebuffers, "glDeleteFramebuffers");
    GLX(BindFramebuffer, "glBindFramebuffer"); GLX(FramebufferTextureLayer, "glFramebufferTextureLayer");
    GLX(FramebufferTexture2D, "glFramebufferTexture2D"); GLX(GenBuffers, "glGenBuffers"); GLX(DeleteBuffers, "glDeleteBuffers");
    GLX(BindBuffer, "glBindBuffer"); GLX(BufferData, "glBufferData"); GLX(MapBufferRange, "glMapBufferRange");
    GLX(UnmapBuffer, "glUnmapBuffer"); GLX(FenceSync, "glFenceSync"); GLX(ClientWaitSync, "glClientWaitSync"); GLX(DeleteSync, "glDeleteSync");
#undef GLX
    return 1;
}

/* Activa el contexto del juego en este hilo si hace falta (y después se restaura el que estaba) */
typedef struct { HDC dc; HGLRC rc; int changed; } GlPrev;
static int gl_begin(Session *s, GlPrev *p) {
    p->rc = gl.GetCurrentContext(); p->dc = gl.GetCurrentDC(); p->changed = 0;
    if (p->rc == s->glrc) return 1;
    if (!gl.MakeCurrent(s->gldc, s->glrc)) return 0;
    p->changed = 1;
    return 1;
}
static void gl_end(GlPrev *p) { if (p->changed) gl.MakeCurrent(p->dc, p->rc); }

XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetOpenGLGraphicsRequirementsKHR(XrInstance i, XrSystemId id, XrGraphicsRequirementsOpenGLKHR *req) {
    (void)i; (void)id;
    req->minApiVersionSupported = XR_MAKE_VERSION(3, 2, 0);
    req->maxApiVersionSupported = XR_MAKE_VERSION(4, 6, 0);
    return XR_SUCCESS;
}

static const int64_t g_glFormats[] = {
    GL_SRGB8_ALPHA8, GL_RGBA8, GL_DEPTH_COMPONENT32F, GL_DEPTH24_STENCIL8, GL_DEPTH_COMPONENT24, GL_DEPTH_COMPONENT16,
};
#define N_GLFMT (sizeof g_glFormats / sizeof g_glFormats[0])
/* formato interno → formato/tipo para crear la textura; isDepth; el equivalente "typeless" de DXGI */
static DXGI_FORMAT typeless_of_gl(int64_t f, GLenum *fmt, GLenum *type, int *isDepth) {
    *isDepth = 0; *fmt = GL_RGBA; *type = GL_UNSIGNED_BYTE;
    switch (f) {
    case GL_SRGB8_ALPHA8: case GL_RGBA8: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case GL_DEPTH_COMPONENT32F: *isDepth = 1; *fmt = GL_DEPTH_COMPONENT; *type = GL_FLOAT; return DXGI_FORMAT_R32_TYPELESS;
    case GL_DEPTH24_STENCIL8: *isDepth = 1; *fmt = GL_DEPTH_STENCIL; *type = GL_UNSIGNED_INT_24_8; return DXGI_FORMAT_R24G8_TYPELESS;
    case GL_DEPTH_COMPONENT24: *isDepth = 1; *fmt = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_INT; return DXGI_FORMAT_R24G8_TYPELESS;
    case GL_DEPTH_COMPONENT16: *isDepth = 1; *fmt = GL_DEPTH_COMPONENT; *type = GL_UNSIGNED_SHORT; return DXGI_FORMAT_R16_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

static XrResult gl_create_swapchain(Session *s, const XrSwapchainCreateInfo *ci, XrSwapchain *out) {
    GLenum fmt, type; int isDepth;
    DXGI_FORMAT tl = typeless_of_gl(ci->format, &fmt, &type, &isDepth);
    if (tl == DXGI_FORMAT_UNKNOWN) { logmsg("Formato de swapchain OpenGL no soportado: 0x%llx", (long long)ci->format); return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED; }
    GlPrev p;
    if (!gl_begin(s, &p)) { logmsg("OpenGL: no se pudo activar el contexto del juego"); return XR_ERROR_RUNTIME_FAILURE; }
    Swapchain *sc = calloc(1, sizeof *sc);
    sc->format = ci->format; sc->dxgi = tl; sc->isGl = 1;
    sc->w = ci->width; sc->h = ci->height;
    sc->arraySize = ci->arraySize ? ci->arraySize : 1;
    sc->mips = ci->mipCount ? ci->mipCount : 1;
    sc->count = 3;
    GLenum target = sc->arraySize > 1 ? GL_TEXTURE_2D_ARRAY : GL_TEXTURE_2D;
    GLint prevTex = 0; gl.GetIntegerv(sc->arraySize > 1 ? GL_TEXTURE_BINDING_2D_ARRAY : GL_TEXTURE_BINDING_2D, &prevTex);
    gl.GenTextures((GLsizei)sc->count, sc->gltex);
    for (uint32_t k = 0; k < sc->count; k++) {
        gl.BindTexture(target, sc->gltex[k]);
        for (uint32_t m = 0; m < sc->mips; m++) {
            GLsizei w = (GLsizei)(sc->w >> m), h = (GLsizei)(sc->h >> m);
            if (w < 1) w = 1;
            if (h < 1) h = 1;
            if (target == GL_TEXTURE_2D_ARRAY) gl.TexImage3D(target, (GLint)m, (GLint)ci->format, w, h, (GLsizei)sc->arraySize, 0, fmt, type, NULL);
            else gl.TexImage2D(target, (GLint)m, (GLint)ci->format, w, h, 0, fmt, type, NULL);
        }
        gl.TexParameteri(target, GL_TEXTURE_MAX_LEVEL, (GLint)sc->mips - 1);
    }
    gl.BindTexture(target, (GLuint)prevTex);
    gl_end(&p);
    logmsg("Swapchain OpenGL %ux%u formato 0x%llx capas %u%s", ci->width, ci->height, (long long)ci->format, sc->arraySize, isDepth ? " (profundidad)" : "");
    *out = (XrSwapchain)sc;
    return XR_SUCCESS;
}

/* Juegos que piden un contexto OpenGL 3.2+ «de compatibilidad»: macOS solo da contextos «core», y Wine
   se niega. Se pide core (forward-compatible), que en la Mac es OpenGL 4.1. */
static PFNWGLCREATECONTEXTATTRIBSARBPROC g_origCreateCtxAttribs;
static PROC (WINAPI *g_origWglGPA)(LPCSTR);
static HGLRC WINAPI hook_wglCreateContextAttribsARB(HDC dc, HGLRC share, const int *attribs) {
    int a[64], n = 0, major = 1, minor = 0, profile = -1, flagsAt = -1;
    for (; attribs && attribs[n] && n < 60; n += 2) {
        a[n] = attribs[n]; a[n + 1] = attribs[n + 1];
        if (a[n] == WGL_CONTEXT_MAJOR_VERSION_ARB) major = a[n + 1];
        if (a[n] == WGL_CONTEXT_MINOR_VERSION_ARB) minor = a[n + 1];
        if (a[n] == WGL_CONTEXT_PROFILE_MASK_ARB) profile = n;
        if (a[n] == WGL_CONTEXT_FLAGS_ARB) flagsAt = n;
    }
    if (major * 10 + minor >= 32 && profile >= 0 && (a[profile + 1] & WGL_CONTEXT_COMPATIBILITY_PROFILE_BIT_ARB)) {
        a[profile + 1] = WGL_CONTEXT_CORE_PROFILE_BIT_ARB;
        if (flagsAt >= 0) a[flagsAt + 1] |= WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB;
        else { a[n++] = WGL_CONTEXT_FLAGS_ARB; a[n++] = WGL_CONTEXT_FORWARD_COMPATIBLE_BIT_ARB; }
        logmsg("OpenGL: el juego pidió un contexto %d.%d de compatibilidad; se pide core (lo que tiene la Mac)", major, minor);
    }
    a[n] = 0;
    return g_origCreateCtxAttribs(dc, share, attribs ? a : NULL);
}
/* OpenComposite copia los cuadros con funciones de OpenGL 4.3/4.5 que la Mac no tiene (llega hasta 4.1):
 * se hacen con funciones viejas equivalentes */
static struct {
    int ok;
    void (WINAPI *GetTexLevelParameteriv)(GLenum, GLint, GLenum, GLint *);
    GLenum (WINAPI *GetError)(void);
    GLboolean (WINAPI *IsEnabled)(GLenum);
    void (WINAPI *Enable)(GLenum); void (WINAPI *Disable)(GLenum);
    PFNGLBLITFRAMEBUFFERPROC BlitFramebuffer;
    GLuint fbo[2];
} glx;
static int glx_load(void) {
    if (glx.ok) return glx.ok > 0;
    glx.ok = -1;
    if (!gl_load() || !gl_load_ext()) return 0;
    HMODULE m = GetModuleHandleA("opengl32.dll");
    FARPROC f;
    f = GetProcAddress(m, "glGetTexLevelParameteriv"); memcpy(&glx.GetTexLevelParameteriv, &f, sizeof f);
    f = GetProcAddress(m, "glGetError"); memcpy(&glx.GetError, &f, sizeof f);
    f = GetProcAddress(m, "glIsEnabled"); memcpy(&glx.IsEnabled, &f, sizeof f);
    f = GetProcAddress(m, "glEnable"); memcpy(&glx.Enable, &f, sizeof f);
    f = GetProcAddress(m, "glDisable"); memcpy(&glx.Disable, &f, sizeof f);
    PROC p = gl.GetProcAddress("glBlitFramebuffer"); memcpy(&glx.BlitFramebuffer, &p, sizeof p);
    if (!glx.GetTexLevelParameteriv || !glx.GetError || !glx.IsEnabled || !glx.BlitFramebuffer) return 0;
    gl.GenFramebuffers(2, glx.fbo);
    glx.ok = 1;
    return 1;
}
/* Enlaza la textura con el tipo que tenga (2D o arreglo 2D) y devuelve ese tipo; 0 si no se pudo */
static GLenum glx_bind_any(GLuint tex) {
    static const GLenum tipos[] = { GL_TEXTURE_2D, GL_TEXTURE_2D_ARRAY };
    while (glx.GetError() != GL_NO_ERROR) {}
    for (int i = 0; i < 2; i++) {
        gl.BindTexture(tipos[i], tex);
        if (glx.GetError() == GL_NO_ERROR) return tipos[i];
    }
    return 0;
}
static void APIENTRY emu_glGetTextureLevelParameteriv(GLuint tex, GLint level, GLenum pname, GLint *params) {
    if (!glx_load()) return;
    GLint p2d = 0, parr = 0;
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &p2d); gl.GetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &parr);
    GLenum t = glx_bind_any(tex);
    if (t) glx.GetTexLevelParameteriv(t, level, pname, params);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)p2d); gl.BindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)parr);
}
static void glx_attach(GLenum fb, GLuint tex, GLenum target, GLint level, GLint z, GLbitfield *mask) {
    GLint p2d = 0, parr = 0, fmt = 0;
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &p2d); gl.GetIntegerv(GL_TEXTURE_BINDING_2D_ARRAY, &parr);
    gl.BindTexture(target, tex);
    glx.GetTexLevelParameteriv(target, level, GL_TEXTURE_INTERNAL_FORMAT, &fmt);
    gl.BindTexture(GL_TEXTURE_2D, (GLuint)p2d); gl.BindTexture(GL_TEXTURE_2D_ARRAY, (GLuint)parr);
    GLenum at = GL_COLOR_ATTACHMENT0; *mask = GL_COLOR_BUFFER_BIT;
    if (fmt == GL_DEPTH24_STENCIL8 || fmt == GL_DEPTH32F_STENCIL8) { at = GL_DEPTH_STENCIL_ATTACHMENT; *mask = GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT; }
    else if (fmt == GL_DEPTH_COMPONENT16 || fmt == GL_DEPTH_COMPONENT24 || fmt == GL_DEPTH_COMPONENT32F || fmt == GL_DEPTH_COMPONENT32) { at = GL_DEPTH_ATTACHMENT; *mask = GL_DEPTH_BUFFER_BIT; }
    if (target == GL_TEXTURE_2D_ARRAY) gl.FramebufferTextureLayer(fb, at, tex, level, z);
    else gl.FramebufferTexture2D(fb, at, GL_TEXTURE_2D, tex, level);
}
static void APIENTRY emu_glCopyImageSubData(GLuint src, GLenum srcTarget, GLint srcLevel, GLint srcX, GLint srcY, GLint srcZ,
                                            GLuint dst, GLenum dstTarget, GLint dstLevel, GLint dstX, GLint dstY, GLint dstZ,
                                            GLsizei w, GLsizei h, GLsizei depth) {
    if (!glx_load()) return;
    static int avisado;
    if (!avisado) { avisado = 1; logmsg("OpenGL: copia de cuadros de OpenComposite hecha sin OpenGL 4.3 (0x%x → 0x%x)", srcTarget, dstTarget); }
    GLint rfb = 0, dfb = 0;
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &rfb); gl.GetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &dfb);
    GLboolean sc = glx.IsEnabled(GL_SCISSOR_TEST), srgb = glx.IsEnabled(GL_FRAMEBUFFER_SRGB);
    if (sc) glx.Disable(GL_SCISSOR_TEST);
    if (srgb) glx.Disable(GL_FRAMEBUFFER_SRGB);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, glx.fbo[0]);
    gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, glx.fbo[1]);
    for (GLsizei z = 0; z < (depth > 0 ? depth : 1); z++) {
        GLbitfield m1, m2;
        glx_attach(GL_READ_FRAMEBUFFER, src, srcTarget, srcLevel, srcZ + z, &m1);
        glx_attach(GL_DRAW_FRAMEBUFFER, dst, dstTarget, dstLevel, dstZ + z, &m2);
        glx.BlitFramebuffer(srcX, srcY, srcX + w, srcY + h, dstX, dstY, dstX + w, dstY + h, m1 & m2, GL_NEAREST);
    }
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)rfb); gl.BindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint)dfb);
    if (sc) glx.Enable(GL_SCISSOR_TEST);
    if (srgb) glx.Enable(GL_FRAMEBUFFER_SRGB);
}

static PROC WINAPI hook_wglGetProcAddress(LPCSTR name) {
    PROC p = g_origWglGPA(name);
    if (!p && name && !strcmp(name, "glGetTextureLevelParameteriv")) return (PROC)(void (*)(void))emu_glGetTextureLevelParameteriv;
    if (!p && name && !strcmp(name, "glCopyImageSubData")) return (PROC)(void (*)(void))emu_glCopyImageSubData;
    if (p && name && !strcmp(name, "wglCreateContextAttribsARB")) {
        g_origCreateCtxAttribs = (PFNWGLCREATECONTEXTATTRIBSARBPROC)(void (*)(void))p;
        return (PROC)(void (*)(void))hook_wglCreateContextAttribsARB;
    }
    return p;
}
static void install_gl_hook(void) {
    static int done;
    if (done) return;
    done = 1;
    void *o = NULL;
    if (patch_iat(GetModuleHandleA(NULL), "opengl32.dll", "wglGetProcAddress", (void *)hook_wglGetProcAddress, &o)) {
        g_origWglGPA = (PROC (WINAPI *)(LPCSTR))o;
        logmsg("OpenGL: ajuste de contexto instalado");
    }
    /* También en OpenComposite, para darle las funciones que la Mac no tiene */
    if (patch_iat(GetModuleHandleA("vrclient_x64.dll"), "opengl32.dll", "wglGetProcAddress", (void *)hook_wglGetProcAddress, &o)) {
        if (!g_origWglGPA) g_origWglGPA = (PROC (WINAPI *)(LPCSTR))o;
        logmsg("OpenGL: funciones para OpenComposite instaladas");
    }
}

/* OpenComposite nunca implementó IVRCompositor::GetFrameTimeRemaining: si un juego la llama (Google Earth VR),
 * OpenComposite aborta y el juego nunca manda cuadros. Se busca esa función en su DLL (vrclient_x64.dll, ya
 * cargada en el proceso) por el nombre que usa al abortar, y se la cambia en memoria por una que responde
 * «quedan 5 ms» (el archivo en disco no se toca). */
static void fix_opencomposite_stubs(void) {
    static int done;
    if (done) return;
    done = 1;
    unsigned char *b = (unsigned char *)GetModuleHandleA("vrclient_x64.dll");
    if (!b) return;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(b + ((IMAGE_DOS_HEADER *)b)->e_lfanew);
    IMAGE_SECTION_HEADER *sec = IMAGE_FIRST_SECTION(nt);
    static const char nombre[] = "\0GetFrameTimeRemaining";   /* (con el \0 de antes y el de después) */
    unsigned char *lit = NULL, *text = NULL; DWORD textSize = 0; int nLit = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        unsigned char *s = b + sec[i].VirtualAddress; DWORD sz = sec[i].Misc.VirtualSize;
        if (!memcmp(sec[i].Name, ".text", 6)) { text = s; textSize = sz; }
        if (!memcmp(sec[i].Name, ".rdata", 7))
            for (DWORD k = 0; k + sizeof nombre <= sz; k++)
                if (!memcmp(s + k, nombre, sizeof nombre)) { lit = s + k + 1; nLit++; }
    }
    if (nLit != 1 || !text) { logmsg("OpenComposite: no encontré GetFrameTimeRemaining (%d)", nLit); return; }
    /* La única instrucción «lea reg, [rip+x]» que apunta a ese nombre está dentro de la función */
    unsigned char *fn = NULL; int nLea = 0;
    for (DWORD k = 0; k + 7 <= textSize; k++) {
        unsigned char *p = text + k;
        if ((p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 5 &&
            p + 7 + *(int32_t *)(p + 3) == lit) {
            nLea++;
            for (unsigned char *q = p; q > p - 0x40 && q > text; q--)
                if (q[-1] == 0xCC) { fn = q; break; }   /* el comienzo: después del relleno int3 */
        }
    }
    if (nLea != 1 || !fn) { logmsg("OpenComposite: no encontré dónde arreglar GetFrameTimeRemaining (%d)", nLea); return; }
    /* mov eax, 0.005f ; movd xmm0, eax ; ret */
    static const unsigned char nuevo[] = { 0xB8, 0x0A, 0xD7, 0xA3, 0x3B, 0x66, 0x0F, 0x6E, 0xC0, 0xC3 };
    DWORD viejo;
    if (!VirtualProtect(fn, sizeof nuevo, PAGE_EXECUTE_READWRITE, &viejo)) return;
    memcpy(fn, nuevo, sizeof nuevo);
    VirtualProtect(fn, sizeof nuevo, viejo, &viejo);
    FlushInstructionCache(GetCurrentProcess(), fn, sizeof nuevo);
    logmsg("OpenComposite: GetFrameTimeRemaining arreglada");
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrCreateSession(XrInstance i, const XrSessionCreateInfo *info, XrSession *session) {
    (void)i;
    const XrBaseInStructure *n = (const XrBaseInStructure *)info->next;
    const XrGraphicsBindingD3D11KHR *gb = NULL;
    const XrGraphicsBindingVulkanKHR *vb = NULL;
    while (n) {
        if (n->type == XR_TYPE_GRAPHICS_BINDING_D3D11_KHR) gb = (const XrGraphicsBindingD3D11KHR *)n;
        if (n->type == XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR) vb = (const XrGraphicsBindingVulkanKHR *)n;
        if (n->type == XR_TYPE_GRAPHICS_BINDING_OPENGL_WIN32_KHR) {
            const XrGraphicsBindingOpenGLWin32KHR *gb2 = (const XrGraphicsBindingOpenGLWin32KHR *)n;
            if (g_session) return XR_ERROR_LIMIT_REACHED;
            if (!gb2->hGLRC || !gl_load()) { logmsg("xrCreateSession: OpenGL no disponible"); return XR_ERROR_GRAPHICS_DEVICE_INVALID; }
            Session *s = calloc(1, sizeof *s);
            s->gl = 1; s->gldc = gb2->hDC; s->glrc = gb2->hGLRC;
            GlPrev p;
            if (!gl_begin(s, &p) || !gl_load_ext()) { gl_end(&p); free(s); logmsg("xrCreateSession: no se pudo usar el contexto OpenGL del juego"); return XR_ERROR_GRAPHICS_DEVICE_INVALID; }
            gl_end(&p);
            g_session = s;
            *session = (XrSession)s;
            logmsg("Sesión creada (OpenGL)");
            set_state(s, XR_SESSION_STATE_IDLE);
            set_state(s, XR_SESSION_STATE_READY);
            return XR_SUCCESS;
        }
        n = n->next;
    }
    if (g_session) return XR_ERROR_LIMIT_REACHED;
    if (vb && !gb) {
        Session *s = calloc(1, sizeof *s);
        if (!vb->device || !vk_session_setup(s, vb)) { vk_session_release(s); free(s); logmsg("xrCreateSession: Vulkan no disponible"); return XR_ERROR_GRAPHICS_DEVICE_INVALID; }
        g_session = s;
        *session = (XrSession)s;
        logmsg("Sesión creada (Vulkan, familia de cola %u)", vb->queueFamilyIndex);
        set_state(s, XR_SESSION_STATE_IDLE);
        set_state(s, XR_SESSION_STATE_READY);
        return XR_SUCCESS;
    }
    if (!gb || !gb->device) { logmsg("xrCreateSession sin D3D11 ni Vulkan"); return XR_ERROR_GRAPHICS_DEVICE_INVALID; }
    Session *s = calloc(1, sizeof *s);
    s->dev = gb->device;
    ID3D11Device_AddRef(s->dev);
    ID3D11Device_GetImmediateContext(s->dev, &s->ctx);
    g_session = s;
    *session = (XrSession)s;
    logmsg("Sesión creada (D3D11, nivel 0x%x)", (unsigned)ID3D11Device_GetFeatureLevel(s->dev));
    set_state(s, XR_SESSION_STATE_IDLE);
    set_state(s, XR_SESSION_STATE_READY);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrDestroySession(XrSession session) {
    Session *s = (Session *)session;
    if (s->vk) vk_session_release(s);
    if (s->gl) {
        GlPrev p;
        if (gl_begin(s, &p)) {
            if (s->glPbo[0]) gl.DeleteBuffers(STAGES, s->glPbo);
            if (s->glFbo) gl.DeleteFramebuffers(1, &s->glFbo);
            for (int k = 0; k < STAGES; k++) if (s->glSync[k]) gl.DeleteSync(s->glSync[k]);
            gl_end(&p);
        }
    }
    for (int k = 0; k < 3; k++) if (s->staging[k]) ID3D11Texture2D_Release(s->staging[k]);
    EnterCriticalSection(&g_gpuLock); gpu_release(); g_gpuShmGen = -1; g_gpuCtx = NULL; LeaveCriticalSection(&g_gpuLock);
    if (s->ctx) ID3D11DeviceContext_Release(s->ctx);
    if (s->dev) ID3D11Device_Release(s->dev);
    if (g_session == s) g_session = NULL;
    free(s);
    logmsg("Sesión destruida");
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrBeginSession(XrSession session, const XrSessionBeginInfo *info) {
    (void)info;
    Session *s = (Session *)session;
    s->running = 1;
    set_state(s, XR_SESSION_STATE_SYNCHRONIZED);
    set_state(s, XR_SESSION_STATE_VISIBLE);
    set_state(s, XR_SESSION_STATE_FOCUSED);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrEndSession(XrSession session) {
    Session *s = (Session *)session;
    s->running = 0;
    set_state(s, XR_SESSION_STATE_IDLE);
    set_state(s, XR_SESSION_STATE_EXITING);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrRequestExitSession(XrSession session) {
    Session *s = (Session *)session;
    set_state(s, XR_SESSION_STATE_VISIBLE);
    set_state(s, XR_SESSION_STATE_SYNCHRONIZED);
    set_state(s, XR_SESSION_STATE_STOPPING);
    return XR_SUCCESS;
}

/* --- Espacios --- */
XRAPI_ATTR XrResult XRAPI_CALL rt_xrEnumerateReferenceSpaces(XrSession s, uint32_t cap, uint32_t *count, XrReferenceSpaceType *types) {
    (void)s;
    *count = 3;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 3) return XR_ERROR_SIZE_INSUFFICIENT;
    types[0] = XR_REFERENCE_SPACE_TYPE_VIEW;
    types[1] = XR_REFERENCE_SPACE_TYPE_LOCAL;
    types[2] = XR_REFERENCE_SPACE_TYPE_STAGE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrCreateReferenceSpace(XrSession s, const XrReferenceSpaceCreateInfo *info, XrSpace *space) {
    (void)s;
    if (info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_VIEW &&
        info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_LOCAL &&
        info->referenceSpaceType != XR_REFERENCE_SPACE_TYPE_STAGE) return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
    Space *sp = calloc(1, sizeof *sp);
    sp->ref = info->referenceSpaceType;
    sp->offset = info->poseInReferenceSpace;
    *space = (XrSpace)sp;
    if (sp->ref == XR_REFERENCE_SPACE_TYPE_LOCAL && !g_localSet) {
        /* LOCAL: origen a la altura de los ojos (1,6 m) sobre el centro */
        g_localOrigin = pident();
        g_localOrigin.position.y = 1.6f;
        g_localSet = 1;
    }
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetReferenceSpaceBoundsRect(XrSession s, XrReferenceSpaceType t, XrExtent2Df *b) {
    (void)s; (void)t;
    b->width = 0; b->height = 0;
    return XR_SPACE_BOUNDS_UNAVAILABLE;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrCreateActionSpace(XrSession s, const XrActionSpaceCreateInfo *info, XrSpace *space) {
    (void)s;
    Space *sp = calloc(1, sizeof *sp);
    sp->isAction = 1;
    sp->action = (Action *)info->action;
    sp->subaction = info->subactionPath;
    sp->offset = info->poseInActionSpace;
    *space = (XrSpace)sp;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrDestroySpace(XrSpace space) { free((Space *)space); return XR_SUCCESS; }

/* Qué cosa sigue un espacio: 0/1 = mano, 2 = cabeza, -1 = fijo */
static int space_tracked(const Space *sp) {
    if (!sp->isAction) return sp->ref == XR_REFERENCE_SPACE_TYPE_VIEW ? 2 : -1;
    const char *sub = path_str(sp->subaction);
    if (strstr(sub, "/user/hand/left")) return 0;
    if (strstr(sub, "/user/hand/right")) return 1;
    Action *a = sp->action;
    if (a) for (int i = 0; i < a->nall; i++) {
        if (g_session && a->all[i].profile != g_session->profile) continue;
        const char *b = path_str(a->all[i].binding);
        if (strstr(b, "/user/hand/left")) return 0;
        if (strstr(b, "/user/hand/right")) return 1;
    }
    return -1;
}

static void fill_velocity(void *next, const Track *t, const Space *sp, XrPosef baseWorld, int valid) {
    XrBaseOutStructure *n = (XrBaseOutStructure *)next;
    while (n) {
        if (n->type == XR_TYPE_SPACE_VELOCITY) {
            XrSpaceVelocity *v = (XrSpaceVelocity *)n;
            int k = space_tracked(sp);
            memset(&v->linearVelocity, 0, sizeof v->linearVelocity);
            memset(&v->angularVelocity, 0, sizeof v->angularVelocity);
            v->velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
            if (k >= 0 && valid) {
                /* expresar la velocidad en los ejes del espacio base */
                XrQuaternionf inv = qconj(baseWorld.orientation);
                v->linearVelocity = qrot(inv, t->lin[k]);
                v->angularVelocity = qrot(inv, t->ang[k]);
            } else if (!valid) {
                v->velocityFlags = 0;
            }
        }
        n = n->next;
    }
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrLocateSpace(XrSpace space, XrSpace baseSpace, XrTime time, XrSpaceLocation *loc) {
    g_inside = "xrLocateSpace";
    Track t;
    EnterCriticalSection(&g_lock); t = g_track; LeaveCriticalSection(&g_lock);
    predict(&t, time);
    int v1, v2;
    XrPosef a = space_world((Space *)space, &t, &v1);
    XrPosef b = space_world((Space *)baseSpace, &t, &v2);
    loc->pose = pmul(pinv(b), a);
    if (v1 && v2)
        loc->locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_POSITION_VALID_BIT |
                             XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
    else
        loc->locationFlags = 0;
    if (!v1 || !v2) loc->pose = pident();
    fill_velocity(loc->next, &t, (Space *)space, b, v1 && v2);
    return XR_SUCCESS;
}

/* OpenXR 1.1: varias ubicaciones de una vez (Unity la pide si existe) */
XRAPI_ATTR XrResult XRAPI_CALL rt_xrLocateSpaces(XrSession session, const XrSpacesLocateInfo *info, XrSpaceLocations *out) {
    (void)session;
    g_inside = "xrLocateSpaces";
    if (!info || !out || out->locationCount < info->spaceCount) return XR_ERROR_VALIDATION_FAILURE;
    XrSpaceVelocities *vels = NULL;
    for (XrBaseOutStructure *n = (XrBaseOutStructure *)out->next; n; n = n->next)
        if (n->type == XR_TYPE_SPACE_VELOCITIES) vels = (XrSpaceVelocities *)n;
    for (uint32_t i = 0; i < info->spaceCount; i++) {
        XrSpaceVelocity v = { XR_TYPE_SPACE_VELOCITY };
        XrSpaceLocation l = { XR_TYPE_SPACE_LOCATION };
        l.next = vels ? &v : NULL;
        rt_xrLocateSpace(info->spaces[i], info->baseSpace, info->time, &l);
        out->locations[i].locationFlags = l.locationFlags;
        out->locations[i].pose = l.pose;
        if (vels && i < vels->velocityCount) {
            vels->velocities[i].velocityFlags = v.velocityFlags;
            vels->velocities[i].linearVelocity = v.linearVelocity;
            vels->velocities[i].angularVelocity = v.angularVelocity;
        }
    }
    return XR_SUCCESS;
}

/* --- Vistas --- */
#define POSE_HIST 16
static struct { XrTime time; uint32_t id; } g_poseHist[POSE_HIST];
static int g_poseHistIdx;

XRAPI_ATTR XrResult XRAPI_CALL rt_xrLocateViews(XrSession s, const XrViewLocateInfo *info, XrViewState *state, uint32_t cap, uint32_t *count, XrView *views) {
    g_inside = "xrLocateViews";
    (void)s;
    *count = 2;
    if (cap == 0) return XR_SUCCESS;
    if (cap < 2) return XR_ERROR_SIZE_INSUFFICIENT;
    Track t;
    EnterCriticalSection(&g_lock);
    t = g_track;
    g_poseHist[g_poseHistIdx].time = info->displayTime;
    g_poseHist[g_poseHistIdx].id = t.id;
    g_poseHistIdx = (g_poseHistIdx + 1) % POSE_HIST;
    LeaveCriticalSection(&g_lock);
    predict(&t, info->displayTime);
    int vb;
    XrPosef base = space_world((Space *)info->space, &t, &vb);
    XrPosef binv = pinv(base);
    for (int e = 0; e < 2; e++) {
        if (t.valid) {
            views[e].pose = pmul(binv, t.eye[e]);
            views[e].fov = t.fov[e];
            if (g_fovK < 0.999f) {   /* campo de visión recortado: el juego dibuja menos (la Mac completa el borde en negro) */
                const float k = g_fovK;
                views[e].fov.angleLeft = atanf(tanf(t.fov[e].angleLeft) * k); views[e].fov.angleRight = atanf(tanf(t.fov[e].angleRight) * k);
                views[e].fov.angleUp = atanf(tanf(t.fov[e].angleUp) * k); views[e].fov.angleDown = atanf(tanf(t.fov[e].angleDown) * k);
            }
        } else {
            views[e].pose = pident();
            views[e].pose.position.x = e ? 0.032f : -0.032f;
            XrFovf f = { -0.8f, 0.8f, 0.8f, -0.8f };
            f.angleLeft = -0.8f; f.angleRight = 0.8f; f.angleUp = 0.8f; f.angleDown = -0.8f;
            views[e].fov = f;
        }
    }
    state->viewStateFlags = t.valid ? (XR_VIEW_STATE_ORIENTATION_VALID_BIT | XR_VIEW_STATE_POSITION_VALID_BIT |
                                       XR_VIEW_STATE_ORIENTATION_TRACKED_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT) : 0;
    return XR_SUCCESS;
}

/* --- Swapchains --- */
static const int64_t g_formats[] = {
    DXGI_FORMAT_R8G8B8A8_UNORM_SRGB, DXGI_FORMAT_B8G8R8A8_UNORM_SRGB,
    DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM,
    DXGI_FORMAT_D32_FLOAT, DXGI_FORMAT_D24_UNORM_S8_UINT, DXGI_FORMAT_D16_UNORM, DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
};
#define N_FMT (sizeof g_formats / sizeof g_formats[0])
static const int64_t g_vkFormats[] = {
    VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM,
    VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM, VK_FORMAT_D32_SFLOAT_S8_UINT,   /* (sin D24S8: Metal no lo tiene) */
};
#define N_VKFMT (sizeof g_vkFormats / sizeof g_vkFormats[0])
/* Formato de Vulkan → el equivalente "typeless" de DXGI (así el resto del código no cambia) */
static DXGI_FORMAT typeless_of_vk(int64_t f, int *isDepth) {
    *isDepth = 0;
    switch (f) {
    case VK_FORMAT_R8G8B8A8_SRGB: case VK_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case VK_FORMAT_B8G8R8A8_SRGB: case VK_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case VK_FORMAT_D32_SFLOAT: *isDepth = 1; return DXGI_FORMAT_R32_TYPELESS;
    case VK_FORMAT_D16_UNORM: *isDepth = 1; return DXGI_FORMAT_R16_TYPELESS;
    case VK_FORMAT_D32_SFLOAT_S8_UINT: *isDepth = 2; return DXGI_FORMAT_R32G8X24_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

static DXGI_FORMAT typeless_of(int64_t f, int *isDepth) {
    *isDepth = 0;
    switch (f) {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT: *isDepth = 1; return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_D24_UNORM_S8_UINT: *isDepth = 1; return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_D16_UNORM: *isDepth = 1; return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: *isDepth = 1; return DXGI_FORMAT_R32G8X24_TYPELESS;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrEnumerateSwapchainFormats(XrSession s, uint32_t cap, uint32_t *count, int64_t *formats) {
    if (((Session *)s)->gl) {
        *count = (uint32_t)N_GLFMT;
        if (cap == 0) return XR_SUCCESS;
        if (cap < N_GLFMT) return XR_ERROR_SIZE_INSUFFICIENT;
        for (uint32_t i = 0; i < N_GLFMT; i++) formats[i] = g_glFormats[i];
        return XR_SUCCESS;
    }
    if (((Session *)s)->vk) {
        *count = (uint32_t)N_VKFMT;
        if (cap == 0) return XR_SUCCESS;
        if (cap < N_VKFMT) return XR_ERROR_SIZE_INSUFFICIENT;
        for (uint32_t i = 0; i < N_VKFMT; i++) formats[i] = g_vkFormats[i];
        return XR_SUCCESS;
    }
    *count = (uint32_t)N_FMT;
    if (cap == 0) return XR_SUCCESS;
    if (cap < N_FMT) return XR_ERROR_SIZE_INSUFFICIENT;
    for (uint32_t i = 0; i < N_FMT; i++) formats[i] = g_formats[i];
    return XR_SUCCESS;
}

static XrResult vk_create_swapchain(Session *s, const XrSwapchainCreateInfo *ci, XrSwapchain *out) {
    int isDepth;
    DXGI_FORMAT tl = typeless_of_vk(ci->format, &isDepth);
    if (tl == DXGI_FORMAT_UNKNOWN) { logmsg("Formato de swapchain Vulkan no soportado: %lld", (long long)ci->format); return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED; }
    Swapchain *sc = calloc(1, sizeof *sc);
    sc->format = ci->format; sc->dxgi = tl;
    sc->w = ci->width; sc->h = ci->height;
    sc->arraySize = ci->arraySize ? ci->arraySize : 1;
    sc->mips = ci->mipCount ? ci->mipCount : 1;
    sc->count = 3;
    VkImageCreateInfo ic = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT) ic.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT;
    ic.imageType = VK_IMAGE_TYPE_2D; ic.format = (VkFormat)ci->format;
    ic.extent.width = ci->width; ic.extent.height = ci->height; ic.extent.depth = 1;
    ic.mipLevels = sc->mips; ic.arrayLayers = sc->arraySize;
    ic.samples = VK_SAMPLE_COUNT_1_BIT; ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
               (isDepth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) ic.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    ic.sharingMode = VK_SHARING_MODE_EXCLUSIVE; ic.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageAspectFlags aspect = !isDepth ? VK_IMAGE_ASPECT_COLOR_BIT
                              : (isDepth == 2 ? VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_DEPTH_BIT);
    VkImageLayout layout = isDepth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    for (uint32_t k = 0; k < sc->count; k++) {
        VkMemoryRequirements mr;
        VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        int ok = vk.CreateImage(s->vkDev, &ic, NULL, &sc->vimg[k]) == VK_SUCCESS;
        if (ok) {
            vk.GetImageMemoryRequirements(s->vkDev, sc->vimg[k], &mr);
            int mt = vk_memtype(s, mr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
            ma.allocationSize = mr.size; ma.memoryTypeIndex = (uint32_t)mt;
            ok = mt >= 0 && vk.AllocateMemory(s->vkDev, &ma, NULL, &sc->vmem[k]) == VK_SUCCESS
                 && vk.BindImageMemory(s->vkDev, sc->vimg[k], sc->vmem[k], 0) == VK_SUCCESS
                 && vk_initial_layout(s, sc->vimg[k], aspect, layout, sc->mips, sc->arraySize);
        }
        if (!ok) {
            logmsg("Vulkan: no se pudo crear la imagen %ux%u formato %lld capas %u", ci->width, ci->height, (long long)ci->format, sc->arraySize);
            for (uint32_t j = 0; j <= k; j++) {
                if (sc->vimg[j]) vk.DestroyImage(s->vkDev, sc->vimg[j], NULL);
                if (sc->vmem[j]) vk.FreeMemory(s->vkDev, sc->vmem[j], NULL);
            }
            free(sc);
            return XR_ERROR_RUNTIME_FAILURE;
        }
    }
    logmsg("Swapchain Vulkan %ux%u formato %lld capas %u%s", ci->width, ci->height, (long long)ci->format, sc->arraySize, isDepth ? " (profundidad)" : "");
    *out = (XrSwapchain)sc;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrCreateSwapchain(XrSession session, const XrSwapchainCreateInfo *ci, XrSwapchain *out) {
    Session *s = (Session *)session;
    if (s->vk) return vk_create_swapchain(s, ci, out);
    if (s->gl) return gl_create_swapchain(s, ci, out);
    int isDepth;
    DXGI_FORMAT tl = typeless_of(ci->format, &isDepth);
    if (tl == DXGI_FORMAT_UNKNOWN) { logmsg("Formato de swapchain no soportado: %lld", (long long)ci->format); return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED; }
    Swapchain *sc = calloc(1, sizeof *sc);
    sc->format = ci->format; sc->dxgi = tl;
    sc->w = ci->width; sc->h = ci->height;
    sc->arraySize = ci->arraySize ? ci->arraySize : 1;
    sc->mips = ci->mipCount ? ci->mipCount : 1;
    sc->count = 3;
    D3D11_TEXTURE2D_DESC d; memset(&d, 0, sizeof d);
    d.Width = ci->width; d.Height = ci->height;
    d.MipLevels = sc->mips; d.ArraySize = sc->arraySize;
    d.Format = tl;
    d.SampleDesc.Count = 1;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (isDepth) d.BindFlags |= D3D11_BIND_DEPTH_STENCIL;
    else d.BindFlags |= D3D11_BIND_RENDER_TARGET;
    if (ci->usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT) d.BindFlags |= D3D11_BIND_UNORDERED_ACCESS;
    for (uint32_t k = 0; k < sc->count; k++) {
        HRESULT hr = ID3D11Device_CreateTexture2D(s->dev, &d, NULL, &sc->img[k]);
        if (FAILED(hr)) {
            logmsg("CreateTexture2D falló (0x%08lx) %ux%u fmt %lld array %u", (unsigned long)hr, ci->width, ci->height, (long long)ci->format, sc->arraySize);
            for (uint32_t j = 0; j < k; j++) ID3D11Texture2D_Release(sc->img[j]);
            free(sc);
            return XR_ERROR_RUNTIME_FAILURE;
        }
    }
    sc->next = 0; sc->released = 0;
    logmsg("Swapchain %ux%u formato %lld capas %u%s", ci->width, ci->height, (long long)ci->format, sc->arraySize, isDepth ? " (profundidad)" : "");
    *out = (XrSwapchain)sc;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrDestroySwapchain(XrSwapchain h) {
    Swapchain *sc = (Swapchain *)h;
    for (uint32_t k = 0; k < sc->count; k++) if (sc->img[k]) ID3D11Texture2D_Release(sc->img[k]);
    if (sc->isGl && g_session && g_session->gl) {
        GlPrev p;
        if (gl_begin(g_session, &p)) { gl.DeleteTextures((GLsizei)sc->count, sc->gltex); gl_end(&p); }
    }
    if (g_session && g_session->vk) {
        vk.QueueWaitIdle(g_session->vkQueue);
        for (uint32_t k = 0; k < sc->count; k++) {
            if (sc->vimg[k]) vk.DestroyImage(g_session->vkDev, sc->vimg[k], NULL);
            if (sc->vmem[k]) vk.FreeMemory(g_session->vkDev, sc->vmem[k], NULL);
        }
    }
    free(sc);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrEnumerateSwapchainImages(XrSwapchain h, uint32_t cap, uint32_t *count, XrSwapchainImageBaseHeader *images) {
    Swapchain *sc = (Swapchain *)h;
    *count = sc->count;
    if (cap == 0) return XR_SUCCESS;
    if (cap < sc->count) return XR_ERROR_SIZE_INSUFFICIENT;
    if (sc->isGl) {
        XrSwapchainImageOpenGLKHR *gi = (XrSwapchainImageOpenGLKHR *)images;
        for (uint32_t k = 0; k < sc->count; k++) gi[k].image = sc->gltex[k];
        return XR_SUCCESS;
    }
    if (sc->vimg[0]) {
        XrSwapchainImageVulkanKHR *vi = (XrSwapchainImageVulkanKHR *)images;
        for (uint32_t k = 0; k < sc->count; k++) vi[k].image = sc->vimg[k];
        return XR_SUCCESS;
    }
    XrSwapchainImageD3D11KHR *im = (XrSwapchainImageD3D11KHR *)images;
    for (uint32_t k = 0; k < sc->count; k++) im[k].texture = sc->img[k];
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrAcquireSwapchainImage(XrSwapchain h, const XrSwapchainImageAcquireInfo *i, uint32_t *index) {
    g_inside = "xrAcquireSwapchainImage";
    (void)i;
    Swapchain *sc = (Swapchain *)h;
    *index = sc->next;
    sc->next = (sc->next + 1) % sc->count;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL rt_xrWaitSwapchainImage(XrSwapchain h, const XrSwapchainImageWaitInfo *i) { (void)h; (void)i; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL rt_xrReleaseSwapchainImage(XrSwapchain h, const XrSwapchainImageReleaseInfo *i) {
    (void)i;
    Swapchain *sc = (Swapchain *)h;
    sc->released = (sc->next + sc->count - 1) % sc->count;
    return XR_SUCCESS;
}

/* --- Frames --- */
static int64_t g_nextFrame;

/* Copia directa: ¿terminó la GPU alguna copia pendiente? Entonces se le avisa a la Mac ya (no en el próximo cuadro) */
static void gpu_poll(void) {
    if (g_gpuState != 2 || !g_gpuCtx) return;
    if (!TryEnterCriticalSection(&g_gpuLock)) return;   /* (xrEndFrame está trabajando: se revisa en un momento) */
    if (g_gpuState == 2 && g_gpuCtx)
        for (int i = 0; i < SHM_SLOTS; i++) gpu_done(g_gpuCtx, i);
    LeaveCriticalSection(&g_gpuLock);
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrWaitFrame(XrSession session, const XrFrameWaitInfo *i, XrFrameState *st) {
    g_inside = "xrWaitFrame";
    (void)i;
    Session *s = (Session *)session;
    int64_t period = 1000000000LL / g_fps;
    int64_t now = now_ns();
    if (g_nextFrame == 0 || g_nextFrame < now - period) g_nextFrame = now;
    else g_nextFrame += period;
    while (now_ns() < g_nextFrame - 1500000) { gpu_poll(); Sleep(1); }
    while (now_ns() < g_nextFrame) { gpu_poll(); /* espera activa breve */ }
    { int64_t e = now_ns(); g_sumWait += (e - now) / 1e6; g_nWait++; g_waitEnd = e; }
    st->predictedDisplayTime = g_nextFrame + period;
    st->predictedDisplayPeriod = period;
    st->shouldRender = (s->state == XR_SESSION_STATE_VISIBLE || s->state == XR_SESSION_STATE_FOCUSED) ? XR_TRUE : XR_FALSE;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrBeginFrame(XrSession s, const XrFrameBeginInfo *i) {
    g_inside = "xrBeginFrame"; (void)s; (void)i; return XR_SUCCESS; }


/* Copia los dos ojos (en la GPU) lado a lado en dst. 0 = algo no coincide (se descarta el cuadro) */
static int copy_eyes(Session *s, const XrCompositionLayerProjection *pl, ID3D11Texture2D *dst, uint32_t ew, uint32_t eh, DXGI_FORMAT fmt) {
    for (int e = 0; e < 2; e++) {
        const XrSwapchainSubImage *si = &pl->views[e].subImage;
        Swapchain *sc = (Swapchain *)si->swapchain;
        if (!sc || sc->dxgi != fmt) { InterlockedIncrement(&g_skipFmt); return 0; }
        UINT sub = si->imageArrayIndex * sc->mips; /* mip 0 de la capa */
        int x = si->imageRect.offset.x, y = si->imageRect.offset.y;
        int w = si->imageRect.extent.width, h = si->imageRect.extent.height;
        if (w < 0) { x += w; w = -w; }
        if (h < 0) { y += h; h = -h; }
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (w > (int)ew) w = (int)ew;
        if (h > (int)eh) h = (int)eh;
        D3D11_BOX box;
        box.left = (UINT)x; box.top = (UINT)y;
        box.right = box.left + (UINT)w; box.bottom = box.top + (UINT)h;
        box.front = 0; box.back = 1;
        if (box.right > sc->w) box.right = sc->w;
        if (box.bottom > sc->h) box.bottom = sc->h;
        if (box.right <= box.left || box.bottom <= box.top) { InterlockedIncrement(&g_skipFmt); return 0; }
        ID3D11DeviceContext_CopySubresourceRegion(s->ctx, (ID3D11Resource *)dst, 0, e * ew, 0, 0,
                                                  (ID3D11Resource *)sc->img[sc->released], sub, &box);
    }
    return 1;
}

static void readback(Session *s, const XrCompositionLayerProjection *pl, XrTime displayTime) {
    Swapchain *sc0 = (Swapchain *)pl->views[0].subImage.swapchain;
    if (!sc0 || sc0->dxgi == DXGI_FORMAT_UNKNOWN) return;
    if (sc0->dxgi != DXGI_FORMAT_R8G8B8A8_TYPELESS && sc0->dxgi != DXGI_FORMAT_B8G8R8A8_TYPELESS) return;
    /* Algunos programas (OpenComposite con D3D11) mandan la altura NEGATIVA para indicar
       "imagen dada vuelta". Lo normalizamos: rectángulo positivo + bandera de volteo. */
    int rw = pl->views[0].subImage.imageRect.extent.width, rh = pl->views[0].subImage.imageRect.extent.height;
    int flip = rh < 0;
    uint32_t ew = (uint32_t)(rw < 0 ? -rw : rw);
    uint32_t eh = (uint32_t)(rh < 0 ? -rh : rh);
    if (!ew || !eh || ew > 4096 || eh > 4096) { InterlockedIncrement(&g_skipFmt); return; }

    if (!s->staging[0] || s->stW != ew || s->stH != eh || s->stFmt != sc0->dxgi) {
        for (int k = 0; k < 3; k++) { if (s->staging[k]) ID3D11Texture2D_Release(s->staging[k]); s->staging[k] = NULL; s->stValid[k] = 0; }
        D3D11_TEXTURE2D_DESC d; memset(&d, 0, sizeof d);
        d.Width = ew * 2; d.Height = eh; d.MipLevels = 1; d.ArraySize = 1;
        d.Format = sc0->dxgi; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_STAGING; d.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        for (int k = 0; k < 3; k++) {
            if (FAILED(ID3D11Device_CreateTexture2D(s->dev, &d, NULL, &s->staging[k]))) { logmsg("No se pudo crear la textura de lectura"); return; }
        }
        s->stW = ew; s->stH = eh; s->stFmt = sc0->dxgi;
        size_t need = (size_t)ew * 2 * eh * 4;
        if (need > g_frameCap) {
            /* Esperar a que el hilo de envío suelte los buffers antes de cambiarlos */
            for (int w = 0; w < 2000; w++) {
                EnterCriticalSection(&g_lock);
                int busy = (g_frameSending >= 0 || g_frameReady >= 0);
                if (!busy) { g_frameReady = -2; }   /* -2: bloqueado mientras se cambian */
                LeaveCriticalSection(&g_lock);
                if (!busy) break;
                Sleep(1);
            }
            uint8_t *n0 = malloc(need), *n1 = malloc(need);
            uint8_t *o0 = g_frameBuf[0], *o1 = g_frameBuf[1];
            EnterCriticalSection(&g_lock);
            g_frameBuf[0] = n0; g_frameBuf[1] = n1; g_frameCap = need;
            g_frameReady = -1;
            LeaveCriticalSection(&g_lock);
            free(o0); free(o1);
        }
        logmsg("Imagen del juego: %ux%u por ojo (%s)%s", ew, eh, fmt_code(sc0->dxgi) ? "RGBA" : "BGRA", flip ? ", dada vuelta (se corrige)" : "");
    }

    /* Copia directa (la imagen no sale de la GPU): con la memoria compartida funcionando, sin Modo Centinela
       (que necesita los píxeles) y sin imagen dada vuelta */
    if (g_gpuState >= 0 && !g_vigia && !flip && shm_active() && ew <= 2560 && eh <= 2560) {
        if (g_gpuW != ew || g_gpuH != eh || g_gpuFmt != sc0->dxgi || g_gpuShmGen != g_shmGen) {
            if (!gpu_setup(s->dev, ew, eh, sc0->dxgi)) { g_gpuState = -1; logmsg("Copia directa no disponible: sigo con la memoria compartida"); }
        } else if (g_gpuState == 1) {
            ShmHdr *h = (ShmHdr *)g_shm;
            if (h->reserved[0] == GPU_MAGIC && h->reserved[1] == g_gpuGen) { g_gpuState = 2; logmsg("Copia directa activa: el cuadro ya no sale de la GPU"); }
            else if (h->reserved[0] == GPU_FAIL || now_ns() - g_gpuAsked > 3000000000LL) {
                g_gpuState = -1; gpu_release();
                logmsg(h->reserved[0] == GPU_FAIL ? "La Mac no pudo abrir las texturas compartidas: sigo con la memoria compartida"
                                                  : "La Mac no confirmó la copia directa: sigo con la memoria compartida");
            }
        }
        if (g_gpuState == 2) {
            for (int i = 0; i < SHM_SLOTS; i++) gpu_done(s->ctx, i);   /* (los que terminaron desde el cuadro anterior) */
            ShmHdr *sh = (ShmHdr *)g_shm;
            int ss = -1;
            for (int i = 0; i < SHM_SLOTS; i++) if (!g_gpuPending[i] && InterlockedCompareExchange(&sh->state[i], 2, 0) == 0) { ss = i; break; }
            if (ss < 0) {
                if (!g_shmFullSince) g_shmFullSince = now_ns();
                else if (now_ns() - g_shmFullSince > 1500000000LL) {
                    g_shmFullSince = 0;
                    for (int i = 0; i < SHM_SLOTS; i++) g_gpuPending[i] = 0;
                    closesocket(g_fsock); g_fsock = INVALID_SOCKET; g_fsockRetry = 0; g_shmMode = 0;
                    logmsg("La Mac no libera los cuadros: rearmando la conexión de imágenes");
                }
                InterlockedIncrement(&g_statBusy); return;
            }
            g_shmFullSince = 0;
            int64_t c0 = now_ns();
            g_inside = "xrEndFrame: copia directa en la GPU";
            if (!copy_eyes(s, pl, g_gpuTex[ss], ew, eh, sc0->dxgi)) { sh->state[ss] = 0; return; }
            uint32_t pid = 0;
            EnterCriticalSection(&g_lock);
            pid = g_track.id;
            for (int hh = 0; hh < POSE_HIST; hh++) if (g_poseHist[hh].time == displayTime) { pid = g_poseHist[hh].id; break; }
            LeaveCriticalSection(&g_lock);
            g_gpuPose[ss] = pid; g_gpuPending[ss] = 1;
            ID3D11DeviceContext_End(s->ctx, (ID3D11Asynchronous *)g_gpuQ[ss]);
            ID3D11DeviceContext_Flush(s->ctx);
            g_gpuCtx = s->ctx;
            gpu_done(s->ctx, ss);   /* (si ya terminó, se avisa ahora; si no, lo avisa gpu_poll mientras el juego espera) */
            g_sumMap += (now_ns() - c0) / 1e6; g_sumCopy += (now_ns() - c0) / 1e6; g_nCopy++;
            return;
        }
    }

    g_inside = "xrEndFrame: copiando ojos en la GPU";
    /* 1) Copiar los dos ojos a la textura de lectura actual */
    int k = s->stNext;
    if (!copy_eyes(s, pl, s->staging[k], ew, eh, s->stFmt)) return;
    uint32_t id = 0;
    EnterCriticalSection(&g_lock);
    id = g_track.id;
    for (int h = 0; h < POSE_HIST; h++) if (g_poseHist[h].time == displayTime) { id = g_poseHist[h].id; break; }
    LeaveCriticalSection(&g_lock);
    s->stPose[k] = id; s->stValid[k] = 1; s->stFlip[k] = flip;
    s->stNext = (k + 1) % STAGES;

    /* 2) Leer la copia del cuadro anterior (ya tuvo un cuadro entero para terminar en la GPU) */
    int r = s->stNext;
    if (!s->stValid[r]) return;
    if (g_sock == INVALID_SOCKET && g_fsock == INVALID_SOCKET) { InterlockedIncrement(&g_skipNoSock); return; }
    if (shm_active() && s->stW <= 2560 && s->stH <= 2560) {
        ShmHdr *sh = (ShmHdr *)g_shm;
        int ss = -1;
        for (int i = 0; i < SHM_SLOTS; i++) if (InterlockedCompareExchange(&sh->state[i], 2, 0) == 0) { ss = i; break; }
        if (ss < 0) {
            /* la Mac todavía procesa los anteriores; si pasa más de 1,5 s, se fue (se reinició): rearmar */
            if (!g_shmFullSince) g_shmFullSince = now_ns();
            else if (now_ns() - g_shmFullSince > 1500000000LL) {
                g_shmFullSince = 0;
                closesocket(g_fsock); g_fsock = INVALID_SOCKET; g_fsockRetry = 0; g_shmMode = 0;
                logmsg("La Mac no libera los cuadros: rearmando la conexión de imágenes");
            }
            InterlockedIncrement(&g_statBusy); return;
        }
        g_shmFullSince = 0;
        D3D11_MAPPED_SUBRESOURCE sm;
        int64_t c0 = now_ns();
        g_inside = "xrEndFrame: leyendo imagen de la GPU (Map)";
        HRESULT shr = ID3D11DeviceContext_Map(s->ctx, (ID3D11Resource *)s->staging[r], 0, D3D11_MAP_READ, 0, &sm);
        if (FAILED(shr)) { sh->state[ss] = 0; g_lastMapHr = shr; InterlockedIncrement(&g_mapFail); s->stValid[r] = 0; return; }
        g_sumMap += (now_ns() - c0) / 1e6;   /* cuánto se esperó a la GPU (el resto de la copia es memcpy) */
        uint32_t rb = s->stW * 2 * 4;
        if (sm.RowPitch < rb) { ID3D11DeviceContext_Unmap(s->ctx, (ID3D11Resource *)s->staging[r], 0); sh->state[ss] = 0; s->stValid[r] = 0; InterlockedIncrement(&g_skipFmt); return; }
        g_inside = "xrEndFrame: copiando imagen a la memoria compartida";
        uint8_t *dst = g_shm + SHM_HDR + (size_t)ss * SHM_SLOT_MAX;
        if (!s->stFlip[r] && sm.RowPitch == rb) memcpy(dst, sm.pData, (size_t)rb * s->stH);
        else for (uint32_t y = 0; y < s->stH; y++) {
            uint32_t sy = s->stFlip[r] ? s->stH - 1 - y : y;
            memcpy(dst + (size_t)y * rb, (uint8_t *)sm.pData + (size_t)sy * sm.RowPitch, rb);
        }
        ID3D11DeviceContext_Unmap(s->ctx, (ID3D11Resource *)s->staging[r], 0);
        g_sumCopy += (now_ns() - c0) / 1e6; g_nCopy++;
        s->stValid[r] = 0;
        MemoryBarrier();
        sh->state[ss] = 1;
        shm_notify(ss, s->stPose[r], fmt_code(s->stFmt), s->stW, s->stH);
        return;
    }
    /* Esperando que la Mac confirme la memoria compartida: no mezclar con envíos por TCP en la misma conexión */
    if (g_shmMode == 0) { InterlockedIncrement(&g_statBusy); s->stValid[r] = 0; return; }
    int slot = -1;
    EnterCriticalSection(&g_lock);
    if (g_frameReady == -1) slot = (g_frameSending == 0) ? 1 : 0;
    LeaveCriticalSection(&g_lock);
    if (slot < 0) { InterlockedIncrement(&g_statBusy); return; } /* el envío anterior todavía no terminó: descartamos este cuadro */
    D3D11_MAPPED_SUBRESOURCE m;
    g_inside = "xrEndFrame: leyendo imagen de la GPU (Map)";
    HRESULT mhr = ID3D11DeviceContext_Map(s->ctx, (ID3D11Resource *)s->staging[r], 0, D3D11_MAP_READ, 0, &m);
    if (FAILED(mhr)) { g_lastMapHr = mhr; InterlockedIncrement(&g_mapFail); s->stValid[r] = 0; return; }
    g_inside = "xrEndFrame: copiando imagen a memoria";
    uint32_t rowBytes = s->stW * 2 * 4;
    if (m.RowPitch < rowBytes) { ID3D11DeviceContext_Unmap(s->ctx, (ID3D11Resource *)s->staging[r], 0); s->stValid[r] = 0; InterlockedIncrement(&g_skipFmt); return; }
    for (uint32_t y = 0; y < s->stH; y++) {
        uint32_t sy = s->stFlip[r] ? s->stH - 1 - y : y;
        memcpy(g_frameBuf[slot] + (size_t)y * rowBytes, (uint8_t *)m.pData + (size_t)sy * m.RowPitch, rowBytes);
    }
    ID3D11DeviceContext_Unmap(s->ctx, (ID3D11Resource *)s->staging[r], 0);
    s->stValid[r] = 0;
    uint8_t *h = g_frameHdr[slot];
    uint32_t pid = s->stPose[r];
    h[0] = (uint8_t)(pid >> 24); h[1] = (uint8_t)(pid >> 16); h[2] = (uint8_t)(pid >> 8); h[3] = (uint8_t)pid;
    h[4] = fmt_code(s->stFmt);
    h[5] = (uint8_t)(s->stW >> 8); h[6] = (uint8_t)s->stW;
    h[7] = (uint8_t)(s->stH >> 8); h[8] = (uint8_t)s->stH;
    g_frameLen[slot] = rowBytes * s->stH;
    EnterCriticalSection(&g_lock); g_frameReady = slot; LeaveCriticalSection(&g_lock);
    SetEvent(g_frameEvent);
}

/* Vulkan: copia los dos ojos (en la GPU) a memoria que se puede leer, lado a lado; lee la del cuadro
   anterior (ya terminó en la GPU) y la pasa a la memoria compartida con la Mac */
static void vk_readback(Session *s, const XrCompositionLayerProjection *pl, XrTime displayTime) {
    Swapchain *sc0 = (Swapchain *)pl->views[0].subImage.swapchain;
    if (!sc0 || !sc0->vimg[0]) return;
    if (sc0->dxgi != DXGI_FORMAT_R8G8B8A8_TYPELESS && sc0->dxgi != DXGI_FORMAT_B8G8R8A8_TYPELESS) return;
    int rw = pl->views[0].subImage.imageRect.extent.width, rh = pl->views[0].subImage.imageRect.extent.height;
    int flip = rh < 0;
    uint32_t ew = (uint32_t)(rw < 0 ? -rw : rw);
    uint32_t eh = (uint32_t)(rh < 0 ? -rh : rh);
    if (!ew || !eh || ew > 2560 || eh > 2560) { InterlockedIncrement(&g_skipFmt); return; }
    if (!shm_active()) {   /* (con Vulkan, los cuadros van solo por la memoria compartida) */
        if (g_shmMode < 0 && !s->vkWarned) { s->vkWarned = 1; logmsg("Vulkan: sin memoria compartida con la Mac no se pueden mandar los cuadros"); }
        InterlockedIncrement(&g_statBusy); return;
    }

    if (!s->vkBuf[0] || s->stW != ew || s->stH != eh || s->stFmt != sc0->dxgi) {
        vk.QueueWaitIdle(s->vkQueue);
        vk_free_buffers(s);
        VkBufferCreateInfo bc = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        bc.size = (VkDeviceSize)ew * 2 * eh * 4; bc.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT; bc.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        for (int k = 0; k < STAGES; k++) {
            VkMemoryRequirements mr;
            VkMemoryAllocateInfo ma = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
            int ok = vk.CreateBuffer(s->vkDev, &bc, NULL, &s->vkBuf[k]) == VK_SUCCESS;
            if (ok) {
                vk.GetBufferMemoryRequirements(s->vkDev, s->vkBuf[k], &mr);
                int mt = vk_memtype(s, mr.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                                    VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
                ma.allocationSize = mr.size; ma.memoryTypeIndex = (uint32_t)mt;
                ok = mt >= 0 && vk.AllocateMemory(s->vkDev, &ma, NULL, &s->vkBufMem[k]) == VK_SUCCESS
                     && vk.BindBufferMemory(s->vkDev, s->vkBuf[k], s->vkBufMem[k], 0) == VK_SUCCESS
                     && vk.MapMemory(s->vkDev, s->vkBufMem[k], 0, VK_WHOLE_SIZE, 0, &s->vkMap[k]) == VK_SUCCESS;
            }
            if (!ok) { logmsg("Vulkan: no se pudo crear la memoria de lectura"); vk_free_buffers(s); return; }
        }
        s->stW = ew; s->stH = eh; s->stFmt = sc0->dxgi;
        logmsg("Imagen del juego (Vulkan): %ux%u por ojo (%s)%s", ew, eh, fmt_code(sc0->dxgi) ? "RGBA" : "BGRA", flip ? ", dada vuelta (se corrige)" : "");
    }

    /* 1) Copiar los dos ojos a la memoria de lectura actual */
    g_inside = "xrEndFrame: copiando ojos en la GPU (Vulkan)";
    int k = s->stNext;
    vk.WaitForFences(s->vkDev, 1, &s->vkFence[k], VK_TRUE, 1000000000ULL);
    VkImage imgs[2]; uint32_t layers[2]; VkBufferImageCopy reg[2];
    for (int e = 0; e < 2; e++) {
        const XrSwapchainSubImage *si = &pl->views[e].subImage;
        Swapchain *sc = (Swapchain *)si->swapchain;
        if (!sc || !sc->vimg[0] || sc->dxgi != s->stFmt) { InterlockedIncrement(&g_skipFmt); return; }
        int x = si->imageRect.offset.x, y = si->imageRect.offset.y;
        int w = si->imageRect.extent.width, h = si->imageRect.extent.height;
        if (w < 0) { x += w; w = -w; }
        if (h < 0) { y += h; h = -h; }
        if (x < 0) x = 0;
        if (y < 0) y = 0;
        if (w > (int)ew) w = (int)ew;
        if (h > (int)eh) h = (int)eh;
        if (x + w > (int)sc->w) w = (int)sc->w - x;
        if (y + h > (int)sc->h) h = (int)sc->h - y;
        if (w <= 0 || h <= 0) { InterlockedIncrement(&g_skipFmt); return; }
        imgs[e] = sc->vimg[sc->released]; layers[e] = si->imageArrayIndex;
        memset(&reg[e], 0, sizeof reg[e]);
        reg[e].bufferOffset = (VkDeviceSize)e * ew * 4;
        reg[e].bufferRowLength = ew * 2; reg[e].bufferImageHeight = eh;
        reg[e].imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        reg[e].imageSubresource.baseArrayLayer = layers[e]; reg[e].imageSubresource.layerCount = 1;
        reg[e].imageOffset.x = x; reg[e].imageOffset.y = y;
        reg[e].imageExtent.width = (uint32_t)w; reg[e].imageExtent.height = (uint32_t)h; reg[e].imageExtent.depth = 1;
    }
    int nb = (imgs[1] == imgs[0] && layers[1] == layers[0]) ? 1 : 2;   /* (los dos ojos en la misma imagen y capa: un solo cambio) */
    VkImageMemoryBarrier toSrc[2], back[2];
    for (int e = 0; e < nb; e++) {
        VkImageMemoryBarrier b = { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER };
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = imgs[e];
        b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        b.subresourceRange.levelCount = 1; b.subresourceRange.baseArrayLayer = layers[e]; b.subresourceRange.layerCount = 1;
        toSrc[e] = b; back[e] = b;
        toSrc[e].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT; toSrc[e].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        toSrc[e].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL; toSrc[e].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        back[e].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT; back[e].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
        back[e].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL; back[e].newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    }
    VkCommandBuffer cb = s->vkCb[k];
    vk.ResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk.BeginCommandBuffer(cb, &bi);
    vk.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, NULL, 0, NULL, (uint32_t)nb, toSrc);
    for (int e = 0; e < 2; e++) vk.CmdCopyImageToBuffer(cb, imgs[e], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s->vkBuf[k], 1, &reg[e]);
    VkBufferMemoryBarrier hb = { VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER };
    hb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT; hb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    hb.srcQueueFamilyIndex = hb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    hb.buffer = s->vkBuf[k]; hb.size = VK_WHOLE_SIZE;
    vk.CmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0,
                          0, NULL, 1, &hb, (uint32_t)nb, back);
    vk.EndCommandBuffer(cb);
    vk.ResetFences(s->vkDev, 1, &s->vkFence[k]);
    VkSubmitInfo sub = { VK_STRUCTURE_TYPE_SUBMIT_INFO };
    sub.commandBufferCount = 1; sub.pCommandBuffers = &cb;
    if (vk.QueueSubmit(s->vkQueue, 1, &sub, s->vkFence[k]) != VK_SUCCESS) {
        /* (el fence quedó sin señal: se rearma para que el próximo uso no espere de más) */
        VkFenceCreateInfo fi = { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO }; fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        vk.DestroyFence(s->vkDev, s->vkFence[k], NULL); vk.CreateFence(s->vkDev, &fi, NULL, &s->vkFence[k]);
        InterlockedIncrement(&g_mapFail); return;
    }
    uint32_t id = 0;
    EnterCriticalSection(&g_lock);
    id = g_track.id;
    for (int hh = 0; hh < POSE_HIST; hh++) if (g_poseHist[hh].time == displayTime) { id = g_poseHist[hh].id; break; }
    LeaveCriticalSection(&g_lock);
    s->stPose[k] = id; s->stValid[k] = 1; s->stFlip[k] = flip;
    s->stNext = (k + 1) % STAGES;

    /* 2) Leer la copia del cuadro anterior y pasarla a la memoria compartida */
    int r = s->stNext;
    if (!s->stValid[r]) return;
    ShmHdr *sh = (ShmHdr *)g_shm;
    int ss = -1;
    for (int i = 0; i < SHM_SLOTS; i++) if (InterlockedCompareExchange(&sh->state[i], 2, 0) == 0) { ss = i; break; }
    if (ss < 0) {
        if (!g_shmFullSince) g_shmFullSince = now_ns();
        else if (now_ns() - g_shmFullSince > 1500000000LL) {
            g_shmFullSince = 0;
            closesocket(g_fsock); g_fsock = INVALID_SOCKET; g_fsockRetry = 0; g_shmMode = 0;
            logmsg("La Mac no libera los cuadros: rearmando la conexión de imágenes");
        }
        InterlockedIncrement(&g_statBusy); s->stValid[r] = 0; return;
    }
    g_shmFullSince = 0;
    int64_t c0 = now_ns();
    g_inside = "xrEndFrame: esperando la copia de la GPU (Vulkan)";
    if (vk.WaitForFences(s->vkDev, 1, &s->vkFence[r], VK_TRUE, 1000000000ULL) != VK_SUCCESS) {
        sh->state[ss] = 0; s->stValid[r] = 0; InterlockedIncrement(&g_mapFail); return;
    }
    g_sumMap += (now_ns() - c0) / 1e6;
    g_inside = "xrEndFrame: copiando imagen a la memoria compartida (Vulkan)";
    uint32_t rb = s->stW * 2 * 4;
    uint8_t *dst = g_shm + SHM_HDR + (size_t)ss * SHM_SLOT_MAX;
    const uint8_t *src = (const uint8_t *)s->vkMap[r];
    if (!s->stFlip[r]) memcpy(dst, src, (size_t)rb * s->stH);
    else for (uint32_t y = 0; y < s->stH; y++) memcpy(dst + (size_t)y * rb, src + (size_t)(s->stH - 1 - y) * rb, rb);
    g_sumCopy += (now_ns() - c0) / 1e6; g_nCopy++;
    s->stValid[r] = 0;
    MemoryBarrier();
    sh->state[ss] = 1;
    shm_notify(ss, s->stPose[r], fmt_code(s->stFmt), s->stW, s->stH);
}

/* OpenGL: lee los dos ojos lado a lado con PBO (el del cuadro anterior ya terminó en la GPU) y los pasa a la
   memoria compartida con la Mac. OpenGL guarda las filas de abajo hacia arriba: se dan vuelta al copiar. */
static void gl_readback(Session *s, const XrCompositionLayerProjection *pl, XrTime displayTime) {
    Swapchain *sc0 = (Swapchain *)pl->views[0].subImage.swapchain;
    if (!sc0 || !sc0->isGl || sc0->dxgi != DXGI_FORMAT_R8G8B8A8_TYPELESS) return;
    int rw = pl->views[0].subImage.imageRect.extent.width, rh = pl->views[0].subImage.imageRect.extent.height;
    int flip = !(rh < 0);   /* (OpenGL: de abajo hacia arriba, salvo que la altura venga negativa) */
    uint32_t ew = (uint32_t)(rw < 0 ? -rw : rw), eh = (uint32_t)(rh < 0 ? -rh : rh);
    if (!ew || !eh || ew > 2560 || eh > 2560) { InterlockedIncrement(&g_skipFmt); return; }
    if (!shm_active()) {
        if (g_shmMode < 0 && !s->vkWarned) { s->vkWarned = 1; logmsg("OpenGL: sin memoria compartida con la Mac no se pueden mandar los cuadros"); }
        InterlockedIncrement(&g_statBusy); return;
    }
    GlPrev p;
    if (!gl_begin(s, &p)) { InterlockedIncrement(&g_mapFail); return; }
    GLint prevRead = 0, prevPack = 0, prevRow = 0;
    gl.GetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &prevRead);
    gl.GetIntegerv(GL_PIXEL_PACK_BUFFER_BINDING, &prevPack);
    gl.GetIntegerv(GL_PACK_ROW_LENGTH, &prevRow);
    size_t size = (size_t)ew * 2 * eh * 4;
    if (!s->glPbo[0] || s->stW != ew || s->stH != eh) {
        if (s->glPbo[0]) gl.DeleteBuffers(STAGES, s->glPbo);
        for (int k = 0; k < STAGES; k++) { if (s->glSync[k]) gl.DeleteSync(s->glSync[k]); s->glSync[k] = NULL; s->stValid[k] = 0; }
        gl.GenBuffers(STAGES, s->glPbo);
        for (int k = 0; k < STAGES; k++) { gl.BindBuffer(GL_PIXEL_PACK_BUFFER, s->glPbo[k]); gl.BufferData(GL_PIXEL_PACK_BUFFER, (GLsizeiptr)size, NULL, GL_STREAM_READ); }
        if (!s->glFbo) gl.GenFramebuffers(1, &s->glFbo);
        s->stW = ew; s->stH = eh; s->stFmt = sc0->dxgi;
        logmsg("Imagen del juego (OpenGL): %ux%u por ojo (RGBA)", ew, eh);
    }
    /* 1) Leer los dos ojos al PBO actual (la GPU lo hace sola; acá no se espera) */
    g_inside = "xrEndFrame: leyendo ojos (OpenGL)";
    int k = s->stNext;
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, s->glFbo);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, s->glPbo[k]);
    gl.PixelStorei(GL_PACK_ROW_LENGTH, (GLint)(ew * 2));
    int ok = 1;
    for (int e = 0; e < 2 && ok; e++) {
        const XrSwapchainSubImage *si = &pl->views[e].subImage;
        Swapchain *sc = (Swapchain *)si->swapchain;
        if (!sc || !sc->isGl || sc->dxgi != s->stFmt) { ok = 0; break; }
        int x = si->imageRect.offset.x, y = si->imageRect.offset.y, w = si->imageRect.extent.width, h = si->imageRect.extent.height;
        if (w < 0) { x += w; w = -w; }
        if (h < 0) { y += h; h = -h; }
        if (w > (int)ew) w = (int)ew;
        if (h > (int)eh) h = (int)eh;
        GLuint tex = sc->gltex[sc->released];
        if (sc->arraySize > 1) gl.FramebufferTextureLayer(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, tex, 0, (GLint)si->imageArrayIndex);
        else gl.FramebufferTexture2D(GL_READ_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        gl.ReadBuffer(GL_COLOR_ATTACHMENT0);
        gl.ReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, (void *)(uintptr_t)((size_t)e * ew * 4));
    }
    if (ok) {
        if (s->glSync[k]) gl.DeleteSync(s->glSync[k]);
        s->glSync[k] = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
        uint32_t id = 0;
        EnterCriticalSection(&g_lock);
        id = g_track.id;
        for (int hh = 0; hh < POSE_HIST; hh++) if (g_poseHist[hh].time == displayTime) { id = g_poseHist[hh].id; break; }
        LeaveCriticalSection(&g_lock);
        s->stPose[k] = id; s->stValid[k] = 1; s->stFlip[k] = flip;
        s->stNext = (k + 1) % STAGES;
        /* 2) El PBO del cuadro anterior → memoria compartida */
        int r = s->stNext;
        ShmHdr *sh = (ShmHdr *)g_shm;
        int ss = -1;
        if (s->stValid[r]) for (int i = 0; i < SHM_SLOTS; i++) if (InterlockedCompareExchange(&sh->state[i], 2, 0) == 0) { ss = i; break; }
        if (s->stValid[r] && ss < 0) { InterlockedIncrement(&g_statBusy); s->stValid[r] = 0; }
        if (ss >= 0) {
            int64_t c0 = now_ns();
            if (s->glSync[r]) gl.ClientWaitSync(s->glSync[r], GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ULL);
            gl.BindBuffer(GL_PIXEL_PACK_BUFFER, s->glPbo[r]);
            const uint8_t *src = (const uint8_t *)gl.MapBufferRange(GL_PIXEL_PACK_BUFFER, 0, (GLsizeiptr)size, GL_MAP_READ_BIT);
            g_sumMap += (now_ns() - c0) / 1e6;
            if (src) {
                uint32_t rb = s->stW * 2 * 4;
                uint8_t *dst = g_shm + SHM_HDR + (size_t)ss * SHM_SLOT_MAX;
                if (!s->stFlip[r]) memcpy(dst, src, (size_t)rb * s->stH);
                else for (uint32_t yy = 0; yy < s->stH; yy++) memcpy(dst + (size_t)yy * rb, src + (size_t)(s->stH - 1 - yy) * rb, rb);
                gl.UnmapBuffer(GL_PIXEL_PACK_BUFFER);
                g_sumCopy += (now_ns() - c0) / 1e6; g_nCopy++;
                MemoryBarrier();
                sh->state[ss] = 1;
                shm_notify(ss, s->stPose[r], fmt_code(s->stFmt), s->stW, s->stH);
            } else { sh->state[ss] = 0; InterlockedIncrement(&g_mapFail); }
            s->stValid[r] = 0;
        }
    } else InterlockedIncrement(&g_skipFmt);
    gl.PixelStorei(GL_PACK_ROW_LENGTH, prevRow);
    gl.BindBuffer(GL_PIXEL_PACK_BUFFER, (GLuint)prevPack);
    gl.BindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint)prevRead);
    gl_end(&p);
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrEndFrame(XrSession session, const XrFrameEndInfo *info) {
    Session *s = (Session *)session;
    g_inside = "xrEndFrame";
    int hadProj = 0;
    InterlockedExchangeAdd(&g_layers, (LONG)info->layerCount);
    for (uint32_t i = 0; i < info->layerCount; i++) {
        if (info->layers[i] && info->layers[i]->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) hadProj = 1;
    }
    if (!hadProj) InterlockedIncrement(&g_noProj);
    for (uint32_t i = 0; i < info->layerCount; i++) {
        const XrCompositionLayerBaseHeader *l = info->layers[i];
        if (l && l->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION) {
            const XrCompositionLayerProjection *pl = (const XrCompositionLayerProjection *)l;
            if (pl->viewCount >= 2) {
                if (!s->loggedRects) {
                    s->loggedRects = 1;
                    for (int e = 0; e < 2; e++) {
                        const XrSwapchainSubImage *si = &pl->views[e].subImage;
                        logmsg("Ojo %d: swapchain %p capa %u rect %d,%d %dx%d", e, (void *)si->swapchain, si->imageArrayIndex,
                               si->imageRect.offset.x, si->imageRect.offset.y, si->imageRect.extent.width, si->imageRect.extent.height);
                    }
                    logmsg("Ajuste multi-pass consultado por el juego %ld veces", (long)g_hookHits);
                }
                if (s->vk) vk_readback(s, pl, info->displayTime);
                else if (s->gl) gl_readback(s, pl, info->displayTime);
                else {
                    EnterCriticalSection(&g_gpuLock);
                    readback(s, pl, info->displayTime);
                    LeaveCriticalSection(&g_gpuLock);
                }
                break;
            }
        }
    }
    g_lastEndFrame = now_ns();
    if (g_waitEnd) { g_sumGame += (g_lastEndFrame - g_waitEnd) / 1e6; g_nGame++; }
    g_inside = "fuera del runtime (en el juego)";
    InterlockedIncrement(&g_perfFrames);
    if (!g_perfT0) g_perfT0 = g_lastEndFrame;
    else if (g_lastEndFrame - g_perfT0 > 1000000000LL) {
        double secs = (g_lastEndFrame - g_perfT0) / 1e9;
        char js[256];
        int n = snprintf(js, sizeof js, "{\"fps\":%.0f,\"hz\":%d,\"espera\":%.2f,\"juego\":%.2f,\"copia\":%.2f,\"gpu\":%.2f}",
                         g_perfFrames / secs, g_fps, g_nWait ? g_sumWait / g_nWait : 0.0, g_nGame ? g_sumGame / g_nGame : 0.0, g_nCopy ? g_sumCopy / g_nCopy : 0.0, g_nCopy ? g_sumMap / g_nCopy : 0.0);
        if (n > 0) send_packet(7, js, (uint32_t)n, NULL, 0);
        g_perfFrames = 0; g_sumWait = g_sumGame = g_sumCopy = g_sumMap = 0; g_nWait = g_nGame = g_nCopy = 0;
        g_perfT0 = g_lastEndFrame;
    }
    if (++s->frameCount == 1) { logmsg("Primer cuadro del juego recibido (%u capas)", info->layerCount); g_statT0 = now_ns(); }
    InterlockedIncrement(&g_statSubmitted);
    int64_t tnow = now_ns();
    if (g_statT0 && tnow - g_statT0 > 5000000000LL) {
        double secs = (tnow - g_statT0) / 1e9;
        logmsg("Juego: %.0f cuadros/s dibujados, %.0f/s enviados a la Mac, %.0f/s descartados (red ocupada)%s | sin conexión %ld, formato %ld, lectura GPU falló %ld (0x%08lx) | cuadros sin imagen 3D %ld, capas %ld",
               g_statSubmitted / secs, g_statSent / secs, g_statBusy / secs, g_sock == INVALID_SOCKET ? " [SIN CONEXIÓN]" : "",
               (long)g_skipNoSock, (long)g_skipFmt, (long)g_mapFail, (unsigned long)g_lastMapHr, (long)g_noProj, (long)g_layers);
        g_statSubmitted = g_statSent = g_statBusy = 0;
        g_skipNoSock = g_skipFmt = g_mapFail = g_noProj = g_layers = 0;
        g_statT0 = tnow;
    }
    return XR_SUCCESS;
}

/* --- Acciones --- */
XRAPI_ATTR XrResult XRAPI_CALL rt_xrCreateActionSet(XrInstance i, const XrActionSetCreateInfo *ci, XrActionSet *out) {
    (void)i;
    ActionSet *as = calloc(1, sizeof *as);
    FILL_STR(as->name, ci->actionSetName);
    *out = (XrActionSet)as;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL rt_xrDestroyActionSet(XrActionSet a) { (void)a; return XR_SUCCESS; }

XRAPI_ATTR XrResult XRAPI_CALL rt_xrCreateAction(XrActionSet set, const XrActionCreateInfo *ci, XrAction *out) {
    Action *a = calloc(1, sizeof *a);
    a->type = ci->actionType;
    a->set = (ActionSet *)set;
    FILL_STR(a->name, ci->actionName);
    a->nsub = (int)(ci->countSubactionPaths < 8 ? ci->countSubactionPaths : 8);
    for (int k = 0; k < a->nsub; k++) a->sub[k] = ci->subactionPaths[k];
    EnterCriticalSection(&g_lock);
    if (g_nactions < MAX_ACTIONS) g_actions[g_nactions++] = a;
    LeaveCriticalSection(&g_lock);
    *out = (XrAction)a;
    return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL rt_xrDestroyAction(XrAction a) { (void)a; return XR_SUCCESS; }

XRAPI_ATTR XrResult XRAPI_CALL rt_xrSuggestInteractionProfileBindings(XrInstance i, const XrInteractionProfileSuggestedBinding *sb) {
    (void)i;
    EnterCriticalSection(&g_lock);
    for (uint32_t k = 0; k < sb->countSuggestedBindings; k++) {
        Action *a = (Action *)sb->suggestedBindings[k].action;
        if (a->nall < MAX_BIND) {
            a->all[a->nall].profile = sb->interactionProfile;
            a->all[a->nall].binding = sb->suggestedBindings[k].binding;
            a->nall++;
        }
    }
    LeaveCriticalSection(&g_lock);
    logmsg("Bindings sugeridos para %s (%u)", path_str(sb->interactionProfile), sb->countSuggestedBindings);
    return XR_SUCCESS;
}

static const char *g_profilePref[] = {
    "/interaction_profiles/oculus/touch_controller",
    "/interaction_profiles/meta/touch_controller_plus",
    "/interaction_profiles/meta/touch_plus_controller",
    "/interaction_profiles/meta/touch_pro_controller",
    "/interaction_profiles/valve/index_controller",
    "/interaction_profiles/htc/vive_controller",
    "/interaction_profiles/microsoft/motion_controller",
    "/interaction_profiles/khr/simple_controller",
};

XRAPI_ATTR XrResult XRAPI_CALL rt_xrAttachSessionActionSets(XrSession session, const XrSessionActionSetsAttachInfo *info) {
    (void)info;
    Session *s = (Session *)session;
    EnterCriticalSection(&g_lock);
    s->profile = XR_NULL_PATH;
    /* Emular Index: si el juego tiene controles de Index, se eligen esos (dedos y botones como en un Index) */
    if (g_index) {
        XrPath ip = intern_path("/interaction_profiles/valve/index_controller");
        for (int k = 0; k < g_nactions && !s->profile; k++)
            for (int j = 0; j < g_actions[k]->nall; j++)
                if (g_actions[k]->all[j].profile == ip) { s->profile = ip; break; }
    }
    for (size_t p = 0; p < sizeof g_profilePref / sizeof g_profilePref[0] && !s->profile; p++) {
        XrPath pp = intern_path(g_profilePref[p]);
        for (int k = 0; k < g_nactions && !s->profile; k++)
            for (int j = 0; j < g_actions[k]->nall; j++)
                if (g_actions[k]->all[j].profile == pp) { s->profile = pp; break; }
    }
    LeaveCriticalSection(&g_lock);
    logmsg("Perfil de controles elegido: %s", s->profile ? path_str(s->profile) : "(ninguno)");
    XrEventDataInteractionProfileChanged e = { XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED };
    e.session = session;
    push_event(&e, sizeof e);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetCurrentInteractionProfile(XrSession session, XrPath topLevel, XrInteractionProfileState *st) {
    (void)topLevel;
    Session *s = (Session *)session;
    st->interactionProfile = s->profile;
    return XR_SUCCESS;
}

static int sub_index(XrPath p) {
    if (p == XR_NULL_PATH) return 0;
    const char *s = path_str(p);
    if (!strcmp(s, "/user/hand/left")) return 1;
    if (!strcmp(s, "/user/hand/right")) return 2;
    return -1;
}

static void eval_action(Session *s, Action *a, const Track *t, XrTime now) {
    for (int si = 0; si < 3; si++) {
        float best[2] = { 0, 0 }; int active = 0;
        const char *filter = si == 1 ? "/user/hand/left/" : si == 2 ? "/user/hand/right/" : "";
        for (int j = 0; j < a->nall; j++) {
            if (a->all[j].profile != s->profile) continue;
            const char *b = path_str(a->all[j].binding);
            if (filter[0] && strncmp(b, filter, strlen(filter))) continue;
            float v[2]; int isPose;
            if (!input_value(t, b, v, &isPose)) continue;
            int hand = strstr(b, "/user/hand/left") ? 0 : 1;
            if (!t->valid || !t->handValid[hand]) continue;
            active = 1;
            if (a->type == XR_ACTION_TYPE_VECTOR2F_INPUT) {
                if (v[0]*v[0] + v[1]*v[1] > best[0]*best[0] + best[1]*best[1]) { best[0] = v[0]; best[1] = v[1]; }
            } else if (fabsf(v[0]) > fabsf(best[0])) best[0] = v[0];
        }
        if (a->type == XR_ACTION_TYPE_BOOLEAN_INPUT) best[0] = best[0] > 0.5f ? 1.0f : 0.0f;
        a->prev[si][0] = a->cur[si][0]; a->prev[si][1] = a->cur[si][1];
        a->cur[si][0] = best[0]; a->cur[si][1] = best[1];
        if (a->cur[si][0] != a->prev[si][0] || a->cur[si][1] != a->prev[si][1]) a->changeTime[si] = now;
        a->active[si] = active;
    }
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrSyncActions(XrSession session, const XrActionsSyncInfo *info) {
    g_inside = "xrSyncActions";
    (void)info;
    Session *s = (Session *)session;
    if (s->state != XR_SESSION_STATE_FOCUSED) return XR_SESSION_NOT_FOCUSED;
    Track t;
    EnterCriticalSection(&g_lock);
    t = g_track;
    XrTime now = now_ns();
    for (int k = 0; k < g_nactions; k++) eval_action(s, g_actions[k], &t, now);
    LeaveCriticalSection(&g_lock);
    return XR_SUCCESS;
}

static int get_si(Action *a, XrPath sub) {
    int si = sub_index(sub);
    (void)a;
    return si;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetActionStateBoolean(XrSession s, const XrActionStateGetInfo *gi, XrActionStateBoolean *st) {
    (void)s;
    Action *a = (Action *)gi->action;
    if (a->type != XR_ACTION_TYPE_BOOLEAN_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;
    int si = get_si(a, gi->subactionPath); if (si < 0) return XR_ERROR_PATH_UNSUPPORTED;
    EnterCriticalSection(&g_lock);
    st->currentState = a->cur[si][0] > 0.5f;
    st->changedSinceLastSync = (a->cur[si][0] > 0.5f) != (a->prev[si][0] > 0.5f);
    st->lastChangeTime = a->changeTime[si];
    st->isActive = a->active[si];
    LeaveCriticalSection(&g_lock);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetActionStateFloat(XrSession s, const XrActionStateGetInfo *gi, XrActionStateFloat *st) {
    (void)s;
    Action *a = (Action *)gi->action;
    if (a->type != XR_ACTION_TYPE_FLOAT_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;
    int si = get_si(a, gi->subactionPath); if (si < 0) return XR_ERROR_PATH_UNSUPPORTED;
    EnterCriticalSection(&g_lock);
    st->currentState = a->cur[si][0];
    st->changedSinceLastSync = a->cur[si][0] != a->prev[si][0];
    st->lastChangeTime = a->changeTime[si];
    st->isActive = a->active[si];
    LeaveCriticalSection(&g_lock);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetActionStateVector2f(XrSession s, const XrActionStateGetInfo *gi, XrActionStateVector2f *st) {
    (void)s;
    Action *a = (Action *)gi->action;
    if (a->type != XR_ACTION_TYPE_VECTOR2F_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;
    int si = get_si(a, gi->subactionPath); if (si < 0) return XR_ERROR_PATH_UNSUPPORTED;
    EnterCriticalSection(&g_lock);
    st->currentState.x = a->cur[si][0]; st->currentState.y = a->cur[si][1];
    st->changedSinceLastSync = a->cur[si][0] != a->prev[si][0] || a->cur[si][1] != a->prev[si][1];
    st->lastChangeTime = a->changeTime[si];
    st->isActive = a->active[si];
    LeaveCriticalSection(&g_lock);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetActionStatePose(XrSession s, const XrActionStateGetInfo *gi, XrActionStatePose *st) {
    (void)s;
    Action *a = (Action *)gi->action;
    if (a->type != XR_ACTION_TYPE_POSE_INPUT) return XR_ERROR_ACTION_TYPE_MISMATCH;
    int si = get_si(a, gi->subactionPath); if (si < 0) return XR_ERROR_PATH_UNSUPPORTED;
    EnterCriticalSection(&g_lock);
    st->isActive = a->active[si];
    LeaveCriticalSection(&g_lock);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrEnumerateBoundSourcesForAction(XrSession session, const XrBoundSourcesForActionEnumerateInfo *info, uint32_t cap, uint32_t *count, XrPath *sources) {
    Session *s = (Session *)session;
    Action *a = (Action *)info->action;
    uint32_t n = 0;
    for (int j = 0; j < a->nall; j++) if (a->all[j].profile == s->profile) { if (cap && n < cap) sources[n] = a->all[j].binding; n++; }
    *count = n;
    if (cap && cap < n) return XR_ERROR_SIZE_INSUFFICIENT;
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetInputSourceLocalizedName(XrSession s, const XrInputSourceLocalizedNameGetInfo *info, uint32_t cap, uint32_t *count, char *buffer) {
    (void)s;
    const char *name = path_str(info->sourcePath);
    uint32_t n = (uint32_t)strlen(name) + 1;
    *count = n;
    if (cap == 0) return XR_SUCCESS;
    if (cap < n) return XR_ERROR_SIZE_INSUFFICIENT;
    memcpy(buffer, name, n);
    return XR_SUCCESS;
}

XRAPI_ATTR XrResult XRAPI_CALL rt_xrApplyHapticFeedback(XrSession s, const XrHapticActionInfo *i, const XrHapticBaseHeader *h) { (void)s; (void)i; (void)h; return XR_SUCCESS; }
XRAPI_ATTR XrResult XRAPI_CALL rt_xrStopHapticFeedback(XrSession s, const XrHapticActionInfo *i) { (void)s; (void)i; return XR_SUCCESS; }

/* --- Cuando reemplazamos openxr_loader.dll --- */
XRAPI_ATTR XrResult XRAPI_CALL rt_xrInitializeLoaderKHR(const void *info) { (void)info; init_once(); logmsg("xrInitializeLoaderKHR (modo loader)"); return XR_SUCCESS; }

/* --- Conversión de tiempo (XR_KHR_win32_convert_performance_counter_time) --- */
XRAPI_ATTR XrResult XRAPI_CALL rt_xrConvertWin32PerformanceCounterToTimeKHR(XrInstance i, const LARGE_INTEGER *pc, XrTime *t) {
    (void)i; *t = qpc_to_ns(pc->QuadPart); return XR_SUCCESS;
}
XRAPI_ATTR XrResult XRAPI_CALL rt_xrConvertTimeToWin32PerformanceCounterKHR(XrInstance i, XrTime t, LARGE_INTEGER *pc) {
    (void)i; pc->QuadPart = (LONGLONG)((double)t * (double)g_qpf.QuadPart / 1e9); return XR_SUCCESS;
}

/* ------------------------------------------------------------------ */
/* Tabla de funciones                                                  */
/* ------------------------------------------------------------------ */
#define FN(name) { #name, (PFN_xrVoidFunction)rt_##name }
static const struct { const char *name; PFN_xrVoidFunction fn; } g_fns[] = {
    FN(xrGetInstanceProcAddr), FN(xrEnumerateInstanceExtensionProperties), FN(xrEnumerateApiLayerProperties),
    FN(xrCreateInstance), FN(xrDestroyInstance), FN(xrGetInstanceProperties), FN(xrPollEvent),
    FN(xrResultToString), FN(xrStructureTypeToString), FN(xrStringToPath), FN(xrPathToString),
    FN(xrGetSystem), FN(xrGetSystemProperties), FN(xrEnumerateEnvironmentBlendModes),
    FN(xrEnumerateViewConfigurations), FN(xrGetViewConfigurationProperties), FN(xrEnumerateViewConfigurationViews),
    FN(xrGetD3D11GraphicsRequirementsKHR),
    FN(xrGetVulkanInstanceExtensionsKHR), FN(xrGetVulkanDeviceExtensionsKHR),
    FN(xrGetVulkanGraphicsDeviceKHR), FN(xrGetVulkanGraphicsRequirementsKHR),
    FN(xrGetOpenGLGraphicsRequirementsKHR),
    FN(xrCreateSession), FN(xrDestroySession), FN(xrBeginSession), FN(xrEndSession), FN(xrRequestExitSession),
    FN(xrEnumerateReferenceSpaces), FN(xrCreateReferenceSpace), FN(xrGetReferenceSpaceBoundsRect),
    FN(xrCreateActionSpace), FN(xrLocateSpace), FN(xrLocateSpaces), FN(xrDestroySpace),
    FN(xrLocateViews),
    FN(xrEnumerateSwapchainFormats), FN(xrCreateSwapchain), FN(xrDestroySwapchain), FN(xrEnumerateSwapchainImages),
    FN(xrAcquireSwapchainImage), FN(xrWaitSwapchainImage), FN(xrReleaseSwapchainImage),
    FN(xrWaitFrame), FN(xrBeginFrame), FN(xrEndFrame),
    FN(xrCreateActionSet), FN(xrDestroyActionSet), FN(xrCreateAction), FN(xrDestroyAction),
    FN(xrSuggestInteractionProfileBindings), FN(xrAttachSessionActionSets), FN(xrGetCurrentInteractionProfile),
    FN(xrGetActionStateBoolean), FN(xrGetActionStateFloat), FN(xrGetActionStateVector2f), FN(xrGetActionStatePose),
    FN(xrSyncActions), FN(xrEnumerateBoundSourcesForAction), FN(xrGetInputSourceLocalizedName),
    FN(xrApplyHapticFeedback), FN(xrStopHapticFeedback),
    FN(xrInitializeLoaderKHR), FN(xrConvertWin32PerformanceCounterToTimeKHR), FN(xrConvertTimeToWin32PerformanceCounterKHR),
};

XRAPI_ATTR XrResult XRAPI_CALL rt_xrGetInstanceProcAddr(XrInstance instance, const char *name, PFN_xrVoidFunction *function) {
    init_once();
    (void)instance;
    for (size_t k = 0; k < sizeof g_fns / sizeof g_fns[0]; k++)
        if (!strcmp(g_fns[k].name, name)) { *function = g_fns[k].fn; return XR_SUCCESS; }
    *function = NULL;
    logmsg("Función no implementada pedida: %s", name);
    return XR_ERROR_FUNCTION_UNSUPPORTED;
}


/* ------------------------------------------------------------------ */
/* Desactivar "single-pass instanced" en el dispositivo D3D11 del juego */
/* ------------------------------------------------------------------ */
/* La traducción DirectX->Metal de GameHub dice soportar
 * VPAndRTArrayIndexFromAnyShaderFeedingRasterizer pero no lo implementa:
 * Unity dibuja los dos ojos encimados en la capa 0. Si respondemos FALSE,
 * Unity pasa a dibujar cada ojo por separado (multi-pass). */
typedef HRESULT (STDMETHODCALLTYPE *PFN_CheckFeatureSupport)(ID3D11Device *, D3D11_FEATURE, void *, UINT);
static PFN_CheckFeatureSupport g_origCFS;
static void **g_vtOrig, **g_vtCopy;
static HRESULT STDMETHODCALLTYPE hook_CheckFeatureSupport(ID3D11Device *d, D3D11_FEATURE f, void *data, UINT size) {
    HRESULT hr = g_origCFS(d, f, data, size);
    if (SUCCEEDED(hr) && f == D3D11_FEATURE_D3D11_OPTIONS3 && data && size >= sizeof(D3D11_FEATURE_DATA_D3D11_OPTIONS3)) {
        ((D3D11_FEATURE_DATA_D3D11_OPTIONS3 *)data)->VPAndRTArrayIndexFromAnyShaderFeedingRasterizer = FALSE;
        InterlockedIncrement(&g_hookHits);
        if (g_vtCopy && *(void ***)d == g_vtCopy) {
            *(void ***)d = g_vtOrig;   /* dispositivo intacto de nuevo */
            logmsg("Unity consultó el soporte de dibujo en una pasada: respondimos NO y restauramos el dispositivo");
        }
    }
    return hr;
}
/* Plan B: interceptar D3D11CreateDevice cuando Unity lo pide con GetProcAddress
 * y cambiarle el vtable SOLO al dispositivo del juego (copia en memoria propia). */
static PFN_D3D11_CREATE_DEVICE g_origCreate;
static FARPROC (WINAPI *g_origGPA)(HMODULE, LPCSTR);

static void patch_device(ID3D11Device *dev) {
    void **vt = *(void ***)dev;
    if (vt == g_vtCopy) return;
    size_t n = sizeof(ID3D11Device5Vtbl) / sizeof(void *);
    size_t idx = offsetof(ID3D11DeviceVtbl, CheckFeatureSupport) / sizeof(void *);
    if (vt != g_vtOrig) {
        void **copy = (void **)malloc(n * sizeof(void *));
        memcpy(copy, vt, n * sizeof(void *));
        g_origCFS = (PFN_CheckFeatureSupport)vt[idx];
        copy[idx] = (void *)hook_CheckFeatureSupport;
        g_vtOrig = vt; g_vtCopy = copy;
    }
    *(void ***)dev = g_vtCopy;
    logmsg("Dispositivo D3D11 del juego ajustado (dibujo multi-pass)");
}

static HRESULT WINAPI hook_D3D11CreateDevice(IDXGIAdapter *a, D3D_DRIVER_TYPE t, HMODULE sw, UINT flags,
                                             const D3D_FEATURE_LEVEL *fl, UINT nfl, UINT sdk,
                                             ID3D11Device **dev, D3D_FEATURE_LEVEL *ofl, ID3D11DeviceContext **ctx) {
    static LONG count = 0;
    HRESULT hr = g_origCreate(a, t, sw, flags, fl, nfl, sdk, dev, ofl, ctx);
    LONG n = InterlockedIncrement(&count);
    if (SUCCEEDED(hr) && dev && *dev) {
        if (n == 1) patch_device(*dev);   /* solo el dispositivo principal; el de video queda intacto */
        else logmsg("Dispositivo D3D11 #%ld (secundario) sin modificar", (long)n);
    }
    return hr;
}

static FARPROC WINAPI hook_GetProcAddress(HMODULE m, LPCSTR name) {
    FARPROC p = g_origGPA(m, name);
    if (p && ((ULONG_PTR)name >> 16) && !strcmp(name, "D3D11CreateDevice")) {
        g_origCreate = (PFN_D3D11_CREATE_DEVICE)(void *)p;
        logmsg("Unity pidió D3D11CreateDevice: interceptado");
        return (FARPROC)(void *)hook_D3D11CreateDevice;
    }
    return p;
}

static int patch_iat(HMODULE mod, const char *dll, const char *fn, void *repl, void **orig) {
    if (!mod) return 0;
    BYTE *base = (BYTE *)mod;
    IMAGE_DOS_HEADER *dos = (IMAGE_DOS_HEADER *)base;
    IMAGE_NT_HEADERS *nt = (IMAGE_NT_HEADERS *)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress) return 0;
    for (IMAGE_IMPORT_DESCRIPTOR *d = (IMAGE_IMPORT_DESCRIPTOR *)(base + dir.VirtualAddress); d->Name; d++) {
        if (_stricmp((const char *)(base + d->Name), dll)) continue;
        IMAGE_THUNK_DATA *names = (IMAGE_THUNK_DATA *)(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        IMAGE_THUNK_DATA *iat = (IMAGE_THUNK_DATA *)(base + d->FirstThunk);
        for (; names->u1.AddressOfData; names++, iat++) {
            if (IMAGE_SNAP_BY_ORDINAL(names->u1.Ordinal)) continue;
            IMAGE_IMPORT_BY_NAME *ibn = (IMAGE_IMPORT_BY_NAME *)(base + names->u1.AddressOfData);
            if (strcmp((const char *)ibn->Name, fn)) continue;
            DWORD old;
            if (!VirtualProtect(&iat->u1.Function, sizeof(void *), PAGE_READWRITE, &old)) return 0;
            *orig = (void *)iat->u1.Function;
            iat->u1.Function = (ULONG_PTR)repl;
            VirtualProtect(&iat->u1.Function, sizeof(void *), old, &old);
            return 1;
        }
    }
    return 0;
}

/* Vulkan: algunos juegos (DOOM VFR) piden funciones de la GPU que Metal no tiene (por ejemplo depth bounds o
   cull distance) y vkCreateDevice falla. Se interceptan vkCreateDevice/vkGetInstanceProcAddr del juego y se
   apagan solo las funciones que la Mac no tiene; el resto queda igual. */
static PFN_vkCreateDevice g_origVkCreateDevice;
static PFN_vkGetInstanceProcAddr g_origVkGIPA;
static const char *const g_vkFeatNames[] = {
    "robustBufferAccess", "fullDrawIndexUint32", "imageCubeArray", "independentBlend", "geometryShader",
    "tessellationShader", "sampleRateShading", "dualSrcBlend", "logicOp", "multiDrawIndirect",
    "drawIndirectFirstInstance", "depthClamp", "depthBiasClamp", "fillModeNonSolid", "depthBounds", "wideLines",
    "largePoints", "alphaToOne", "multiViewport", "samplerAnisotropy", "textureCompressionETC2",
    "textureCompressionASTC_LDR", "textureCompressionBC", "occlusionQueryPrecise", "pipelineStatisticsQuery",
    "vertexPipelineStoresAndAtomics", "fragmentStoresAndAtomics", "shaderTessellationAndGeometryPointSize",
    "shaderImageGatherExtended", "shaderStorageImageExtendedFormats", "shaderStorageImageMultisample",
    "shaderStorageImageReadWithoutFormat", "shaderStorageImageWriteWithoutFormat",
    "shaderUniformBufferArrayDynamicIndexing", "shaderSampledImageArrayDynamicIndexing",
    "shaderStorageBufferArrayDynamicIndexing", "shaderStorageImageArrayDynamicIndexing", "shaderClipDistance",
    "shaderCullDistance", "shaderFloat64", "shaderInt64", "shaderInt16", "shaderResourceResidency",
    "shaderResourceMinLod", "sparseBinding", "sparseResidencyBuffer", "sparseResidencyImage2D",
    "sparseResidencyImage3D", "sparseResidency2Samples", "sparseResidency4Samples", "sparseResidency8Samples",
    "sparseResidency16Samples", "sparseResidencyAliased", "variableMultisampleRate", "inheritedQueries",
};
#define N_VKFEAT (sizeof(VkPhysicalDeviceFeatures) / sizeof(VkBool32))

/* Apaga en «want» lo que la GPU no tiene; devuelve cuántas apagó */
static int vk_mask_features(VkPhysicalDeviceFeatures *want, const VkPhysicalDeviceFeatures *has) {
    VkBool32 *w = (VkBool32 *)want; const VkBool32 *h = (const VkBool32 *)has;
    int n = 0;
    for (size_t i = 0; i < N_VKFEAT; i++)
        if (w[i] && !h[i]) {
            w[i] = VK_FALSE; n++;
            logmsg("Vulkan: el juego pidió «%s», que la GPU de la Mac no tiene: se apaga", i < sizeof g_vkFeatNames / sizeof g_vkFeatNames[0] ? g_vkFeatNames[i] : "?");
        }
    return n;
}

static VkResult VKAPI_CALL hook_vkCreateDevice(VkPhysicalDevice pd, const VkDeviceCreateInfo *ci, const VkAllocationCallbacks *al, VkDevice *dev) {
    static PFN_vkGetPhysicalDeviceFeatures gpf;
    if (!gpf) gpf = (PFN_vkGetPhysicalDeviceFeatures)(void (*)(void))GetProcAddress(GetModuleHandleA("vulkan-1.dll"), "vkGetPhysicalDeviceFeatures");
    if (!gpf || !ci) return g_origVkCreateDevice(pd, ci, al, dev);
    VkPhysicalDeviceFeatures has; gpf(pd, &has);
    VkDeviceCreateInfo c = *ci;
    VkPhysicalDeviceFeatures f;
    if (ci->pEnabledFeatures) { f = *ci->pEnabledFeatures; vk_mask_features(&f, &has); c.pEnabledFeatures = &f; }
    /* (también en VkPhysicalDeviceFeatures2 dentro de pNext: se cambia y se restaura después) */
    VkPhysicalDeviceFeatures2 *f2 = NULL; VkPhysicalDeviceFeatures saved;
    for (VkBaseOutStructure *n = (VkBaseOutStructure *)ci->pNext; n; n = n->pNext)
        if (n->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2) { f2 = (VkPhysicalDeviceFeatures2 *)n; break; }
    if (f2) { saved = f2->features; vk_mask_features(&f2->features, &has); }
    VkResult r = g_origVkCreateDevice(pd, &c, al, dev);
    if (f2) f2->features = saved;
    logmsg("Vulkan: vkCreateDevice del juego → %d", (int)r);
    return r;
}

static PFN_vkVoidFunction VKAPI_CALL hook_vkGetInstanceProcAddr(VkInstance inst, const char *name) {
    PFN_vkVoidFunction p = g_origVkGIPA(inst, name);
    if (p && name && !strcmp(name, "vkCreateDevice")) {
        g_origVkCreateDevice = (PFN_vkCreateDevice)p;
        return (PFN_vkVoidFunction)hook_vkCreateDevice;
    }
    return p;
}

/* Se instala una vez, cuando el juego usa Vulkan con nuestro runtime (en el .exe del juego) */
static void install_vulkan_hook(void) {
    static int done;
    if (done) return;
    done = 1;
    HMODULE exe = GetModuleHandleA(NULL), vk1 = GetModuleHandleA("vulkan-1.dll");
    if (!vk1) return;
    void *o = NULL;
    int a = patch_iat(exe, "vulkan-1.dll", "vkCreateDevice", (void *)hook_vkCreateDevice, &o);
    if (a) g_origVkCreateDevice = (PFN_vkCreateDevice)o;
    else g_origVkCreateDevice = (PFN_vkCreateDevice)(void (*)(void))GetProcAddress(vk1, "vkCreateDevice");
    int b = patch_iat(exe, "vulkan-1.dll", "vkGetInstanceProcAddr", (void *)hook_vkGetInstanceProcAddr, &o);
    if (b) g_origVkGIPA = (PFN_vkGetInstanceProcAddr)o;
    logmsg("Vulkan: ajuste de funciones de la GPU instalado (vkCreateDevice %s, vkGetInstanceProcAddr %s)", a ? "sí" : "no", b ? "sí" : "no");
}

static void install_d3d11_hook(void) {
    /* Plan A: parchear el vtable compartido (no funciona en todas las traducciones) */
    ID3D11Device *dev = NULL;
    HRESULT hr = D3D11CreateDevice(NULL, D3D_DRIVER_TYPE_HARDWARE, NULL, 0, NULL, 0, D3D11_SDK_VERSION, &dev, NULL, NULL);
    if (SUCCEEDED(hr) && dev && getenv("QVRB_SIN_PLAN_A")) { ID3D11Device_Release(dev); dev = NULL; }
    if (SUCCEEDED(hr) && dev) {
        D3D11_FEATURE_DATA_D3D11_OPTIONS3 o3; memset(&o3, 0, sizeof o3);
        ID3D11Device_CheckFeatureSupport(dev, D3D11_FEATURE_D3D11_OPTIONS3, &o3, sizeof o3);
        logmsg("GPU informa 'array index desde vertex shader' = %d", (int)o3.VPAndRTArrayIndexFromAnyShaderFeedingRasterizer);
        void **vt = *(void ***)dev;
        size_t idx = offsetof(ID3D11DeviceVtbl, CheckFeatureSupport) / sizeof(void *);
        DWORD old;
        if (VirtualProtect(&vt[idx], sizeof(void *), PAGE_EXECUTE_READWRITE, &old)) {
            g_origCFS = (PFN_CheckFeatureSupport)vt[idx];
            vt[idx] = (void *)hook_CheckFeatureSupport;
            VirtualProtect(&vt[idx], sizeof(void *), old, &old);
            logmsg("Ajuste D3D11 instalado (vtable)");
        }
        ID3D11Device_Release(dev);
    }
    /* Plan B: interceptar el GetProcAddress de Unity */
    HMODULE up = GetModuleHandleA("UnityPlayer.dll");
    if (patch_iat(up, "KERNEL32.dll", "GetProcAddress", (void *)hook_GetProcAddress, (void **)&g_origGPA))
        logmsg("Interceptación de D3D11CreateDevice instalada en UnityPlayer.dll");
    else
        logmsg("No se pudo interceptar GetProcAddress en UnityPlayer.dll (%s)", up ? "sin import" : "módulo no encontrado");
}

/* ------------------------------------------------------------------ */
/* Punto de entrada del loader                                         */
/* ------------------------------------------------------------------ */
static char g_dir[MAX_PATH];
static void copy_unity_logs(int current) {
    const char *up = getenv("USERPROFILE");
    if (!up || !g_dir[0]) return;
    char src[MAX_PATH * 2], dst[MAX_PATH * 2];
    snprintf(src, sizeof src, "%s\\AppData\\LocalLow\\SUPERHOT_Team\\SUPERHOT_VR\\%s", up, current ? "Player.log" : "Player-prev.log");
    snprintf(dst, sizeof dst, "%s%s", g_dir, current ? "unity_actual.log" : "unity_anterior.log");
    CopyFileA(src, dst, FALSE);
}

/* Copia el error.log del último reporte de crash de Unity */
static void copy_last_crash(void) {
    const char *tmp = getenv("TEMP");
    if (!tmp || !g_dir[0]) return;
    char pat[MAX_PATH * 2], best[MAX_PATH] = "";
    FILETIME bt = {0, 0};
    snprintf(pat, sizeof pat, "%s\\SUPERHOT_Team\\SUPERHOT_VR\\Crashes\\*", tmp);
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA(pat, &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || fd.cFileName[0] == '.') continue;
        if (CompareFileTime(&fd.ftLastWriteTime, &bt) > 0) { bt = fd.ftLastWriteTime; strcpy(best, fd.cFileName); }
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    if (!best[0]) return;
    char src[MAX_PATH * 3], dst[MAX_PATH * 2];
    snprintf(src, sizeof src, "%s\\SUPERHOT_Team\\SUPERHOT_VR\\Crashes\\%s\\error.log", tmp, best);
    snprintf(dst, sizeof dst, "%sunity_crash_error.log", g_dir);
    CopyFileA(src, dst, FALSE);
    snprintf(src, sizeof src, "%s\\SUPERHOT_Team\\SUPERHOT_VR\\Crashes\\%s\\Player.log", tmp, best);
    snprintf(dst, sizeof dst, "%sunity_crash_player.log", g_dir);
    CopyFileA(src, dst, FALSE);
}

static DWORD WINAPI watchdog_thread(LPVOID arg) {
    (void)arg;
    int reported = 0;
    for (;;) {
        Sleep(1000);
        int64_t last = g_lastEndFrame;
        if (!last) continue;
        double secs = (now_ns() - last) / 1e9;
        if (secs > 3.0 && !reported) {
            reported = 1;
            logmsg("ATENCIÓN: el juego lleva %.1f s sin entregar un cuadro. Último lugar: %s", secs, (const char *)g_inside);
            copy_unity_logs(1);   /* queda como unity_actual.log */
        } else if (secs < 1.0 && reported) {
            reported = 0;
            logmsg("El juego volvió a entregar cuadros");
        } else if (reported && ((int)secs) % 10 == 0) {
            logmsg("Sigue trabado (%.0f s). Lugar: %s", secs, (const char *)g_inside);
        }
    }
    return 0;
}

static volatile LONG g_initState = 0;   /* 0 = sin iniciar, 1 = iniciando, 2 = listo */
static void init_once(void) {
    /* Unity llama al runtime desde varios hilos a la vez al arrancar: esto debe correr UNA sola vez */
    if (InterlockedCompareExchange(&g_initState, 1, 0) != 0) {
        while (g_initState != 2) Sleep(1);
        return;
    }
    /* Unity carga, descarga y vuelve a cargar este DLL al arrancar. Si se descarga con
     * nuestros hilos vivos, la segunda carga crea otro juego de hilos que pisan la misma
     * memoria (cuelgues y crashes). Lo fijamos: FreeLibrary ya no lo puede descargar. */
    HMODULE pinned = NULL;
    BOOL pinOk = GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                    (LPCSTR)&init_once, &pinned);
    InitializeCriticalSection(&g_lock);
    InitializeCriticalSection(&g_sendlock);
    InitializeCriticalSection(&g_gpuLock);
    QueryPerformanceFrequency(&g_qpf);
    timeBeginPeriod(1);
    char path[MAX_PATH];
    HMODULE self = NULL;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&init_once, &self);
    if (GetModuleFileNameA(self, path, MAX_PATH)) {
        char *slash = strrchr(path, '\\');
        if (slash) {
            slash[1] = 0; strcpy(g_dir, path);
            /* (el del juego anterior queda como questvrbridge_log_anterior.txt) */
            char prev[MAX_PATH];
            strcpy(prev, path); strcpy(prev + (slash + 1 - path), "questvrbridge_log_anterior.txt");
            strcpy(slash + 1, "questvrbridge_log.txt");
            MoveFileExA(path, prev, MOVEFILE_REPLACE_EXISTING);
            g_logf = fopen(path, "w");
        }
    }
    g_frameEvent = CreateEventA(NULL, FALSE, FALSE, NULL);
    CreateThread(NULL, 0, net_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, send_thread, NULL, 0, NULL);
    CreateThread(NULL, 0, watchdog_thread, NULL, 0, NULL);
    /* Dar un momento para conectar y recibir la configuración */
    for (int i = 0; i < 20 && !g_connected; i++) Sleep(25);
    logmsg("QuestVRBridge OpenXR runtime cargado (%s) · proceso %lu · módulo %p %s",
           g_connected ? "conectado a la Mac" : "todavía sin conexión con la Mac",
           (unsigned long)GetCurrentProcessId(), (void *)pinned, pinOk ? "fijado en memoria" : "NO SE PUDO FIJAR");
    copy_unity_logs(0);
    copy_last_crash();
    /* El ajuste de D3D11 ya no hace falta: el modo multi-pass se configura en los datos del juego.
     * Se puede volver a activar con la variable QVRB_AJUSTE_D3D11=1. */
    if (getenv("QVRB_AJUSTE_D3D11")) install_d3d11_hook();
    InterlockedExchange(&g_initState, 2);
}

EXPORT XrResult XRAPI_CALL xrNegotiateLoaderRuntimeInterface(const XrNegotiateLoaderInfo *loaderInfo, XrNegotiateRuntimeRequest *req) {
    init_once();
    if (!loaderInfo || !req ||
        loaderInfo->structType != XR_LOADER_INTERFACE_STRUCT_LOADER_INFO ||
        req->structType != XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST ||
        loaderInfo->minInterfaceVersion > XR_CURRENT_LOADER_RUNTIME_VERSION ||
        loaderInfo->maxInterfaceVersion < XR_CURRENT_LOADER_RUNTIME_VERSION) {
        logmsg("Negociación con el loader falló");
        return XR_ERROR_INITIALIZATION_FAILED;
    }
    req->runtimeInterfaceVersion = XR_CURRENT_LOADER_RUNTIME_VERSION;
    req->runtimeApiVersion = XR_MAKE_VERSION(1, 0, 34);
    req->getInstanceProcAddr = rt_xrGetInstanceProcAddr;
    logmsg("Negociación con el loader OK");
    return XR_SUCCESS;
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID r) {
    (void)h;
    if (reason == DLL_PROCESS_DETACH && r == NULL && g_logf) {
        fprintf(g_logf, "AVISO: se intentó descargar el runtime (no debería pasar si está fijado)\n");
        fflush(g_logf);
    }
    return TRUE;
}
