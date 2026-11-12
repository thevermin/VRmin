/*
 * Lo poco de Windows que usa la parte común de mac/openxr/questvrbridge_openxr.c, hecho con lo de Linux (POSIX), para
 * compilar el mismo archivo como runtime de OpenXR nativo de Linux (libvrmin_openxr.so). Lo de D3D11, OpenGL y los
 * arreglos para juegos dentro de Wine quedan afuera con #ifdef _WIN32; acá solo están los tipos que se nombran en las
 * estructuras compartidas (quedan sin usar).
 */
#pragma once
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>

/* ---------- Tipos ---------- */
typedef int32_t LONG;
typedef uint32_t DWORD;
typedef unsigned int UINT;
typedef int BOOL;
typedef int32_t HRESULT;
typedef void *LPVOID;
#define TRUE 1
#define FALSE 0
#define WINAPI
#define __cdecl
#define __declspec(x) __attribute__((visibility("default")))
#define MAX_PATH 4096
#define _strdup strdup

/* (solo para que compilen las estructuras compartidas: en Linux quedan siempre en NULL) */
typedef struct ID3D11Device ID3D11Device;
typedef struct ID3D11DeviceContext ID3D11DeviceContext;
typedef struct ID3D11Texture2D ID3D11Texture2D;
typedef void *HDC, *HGLRC, *GLsync;
typedef unsigned int GLuint;

/* Los formatos de DXGI con que el código común nombra los de Vulkan (los mismos números que en Windows) */
typedef enum {
    DXGI_FORMAT_UNKNOWN = 0, DXGI_FORMAT_R32G8X24_TYPELESS = 19, DXGI_FORMAT_R8G8B8A8_TYPELESS = 27,
    DXGI_FORMAT_R32_TYPELESS = 39, DXGI_FORMAT_R24G8_TYPELESS = 44, DXGI_FORMAT_R16_TYPELESS = 53,
    DXGI_FORMAT_B8G8R8A8_TYPELESS = 90,
} DXGI_FORMAT;

/* ---------- Tiempo ---------- */
static inline void Sleep(DWORD ms) { usleep((useconds_t)ms * 1000); }
static inline DWORD GetTickCount(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return (DWORD)((uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u);
}
static inline DWORD GetCurrentProcessId(void) { return (DWORD)getpid(); }

/* ---------- Atómicos ---------- */
#define MemoryBarrier() __sync_synchronize()
static inline LONG InterlockedIncrement(volatile LONG *p) { return __sync_add_and_fetch(p, 1); }
static inline LONG InterlockedExchange(volatile LONG *p, LONG v) { __sync_synchronize(); return __sync_lock_test_and_set(p, v); }
static inline LONG InterlockedExchangeAdd(volatile LONG *p, LONG v) { return __sync_fetch_and_add(p, v); }
static inline LONG InterlockedCompareExchange(volatile LONG *p, LONG v, LONG cmp) { return __sync_val_compare_and_swap(p, cmp, v); }

/* ---------- Secciones críticas (como en Windows: el mismo hilo puede volver a entrar) ---------- */
typedef pthread_mutex_t CRITICAL_SECTION;
static inline void InitializeCriticalSection(CRITICAL_SECTION *c) {
    pthread_mutexattr_t a;
    pthread_mutexattr_init(&a);
    pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(c, &a);
    pthread_mutexattr_destroy(&a);
}
static inline void EnterCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_lock(c); }
static inline void LeaveCriticalSection(CRITICAL_SECTION *c) { pthread_mutex_unlock(c); }
static inline BOOL TryEnterCriticalSection(CRITICAL_SECTION *c) { return pthread_mutex_trylock(c) == 0; }

/* ---------- Eventos (de reinicio automático) e hilos: los dos son un HANDLE ---------- */
typedef struct W32Obj {
    pthread_mutex_t m; pthread_cond_t c;
    int set;                            /* evento con señal / hilo terminado */
    int thread; pthread_t th;
    DWORD (*fn)(void *); void *arg;
} *HANDLE;
#define INVALID_HANDLE_VALUE ((HANDLE)(intptr_t)-1)
#define INFINITE 0xFFFFFFFFu
static inline HANDLE w32_obj(void) {
    HANDLE h = (HANDLE)calloc(1, sizeof *h);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_mutex_init(&h->m, NULL);
    pthread_cond_init(&h->c, &ca);
    pthread_condattr_destroy(&ca);
    return h;
}
static inline HANDLE CreateEventA(void *sa, BOOL manual, BOOL initial, const char *name) {
    (void)sa; (void)manual; (void)name;
    HANDLE h = w32_obj();
    h->set = initial;
    return h;
}
static inline BOOL SetEvent(HANDLE h) {
    pthread_mutex_lock(&h->m); h->set = 1; pthread_cond_signal(&h->c); pthread_mutex_unlock(&h->m);
    return TRUE;
}
/* 0: llegó la señal (o terminó el hilo); 0x102: se acabó el tiempo */
static inline DWORD WaitForSingleObject(HANDLE h, DWORD ms) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    if (ms != INFINITE) {
        t.tv_sec += ms / 1000; t.tv_nsec += (long)(ms % 1000) * 1000000L;
        if (t.tv_nsec >= 1000000000L) { t.tv_sec++; t.tv_nsec -= 1000000000L; }
    }
    pthread_mutex_lock(&h->m);
    int r = 0;
    while (!h->set && r == 0) r = ms == INFINITE ? pthread_cond_wait(&h->c, &h->m) : pthread_cond_timedwait(&h->c, &h->m, &t);
    int ok = h->set;
    if (ok && !h->thread) h->set = 0;
    pthread_mutex_unlock(&h->m);
    return ok ? 0 : 0x102;
}
static void *w32_thread(void *p) {
    HANDLE h = (HANDLE)p;
    h->fn(h->arg);
    pthread_mutex_lock(&h->m); h->set = 1; pthread_cond_broadcast(&h->c); pthread_mutex_unlock(&h->m);
    return NULL;
}
static inline HANDLE CreateThread(void *sa, size_t stack, DWORD (*fn)(void *), void *arg, DWORD flags, DWORD *id) {
    (void)sa; (void)stack; (void)flags; (void)id;
    HANDLE h = w32_obj();
    h->thread = 1; h->fn = fn; h->arg = arg;
    if (pthread_create(&h->th, NULL, w32_thread, h) != 0) { free(h); return NULL; }
    return h;
}
static inline BOOL CloseHandle(HANDLE h) {
    if (!h || h == INVALID_HANDLE_VALUE) return FALSE;
    if (h->thread) {
        pthread_mutex_lock(&h->m); int done = h->set; pthread_mutex_unlock(&h->m);
        if (!done) { pthread_detach(h->th); return TRUE; }   /* (sigue andando: su estructura queda, la usa al terminar) */
        pthread_join(h->th, NULL);
    }
    pthread_cond_destroy(&h->c); pthread_mutex_destroy(&h->m);
    free(h);
    return TRUE;
}

/* ---------- Red ---------- */
typedef int SOCKET;
#define INVALID_SOCKET (-1)
#define SD_BOTH SHUT_RDWR
#define WSAEWOULDBLOCK EWOULDBLOCK
#define WSAEINTR EINTR
#define closesocket close
#define WSAGetLastError() errno
typedef struct { int unused; } WSADATA;
#define MAKEWORD(a, b) ((a) | ((b) << 8))
static inline int WSAStartup(int v, WSADATA *d) { (void)v; (void)d; return 0; }
/* (si VRmin Host se cierra, mandar no tiene que matar al juego con SIGPIPE) */
#define send(s, b, n, f) send((s), (b), (n), (f) | MSG_NOSIGNAL)
